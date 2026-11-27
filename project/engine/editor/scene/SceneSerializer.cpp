#include "SceneSerializer.h"
#include "EditorCommon.h"
#include "game/system/scene/SceneEntityParameterCodec.h"
#include "DebugEditor.h"
#include "Object3D.h"
#include "BaseScene.h"
#include "SceneManager.h"
#include "DebugConsole.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <filesystem>
#include <initializer_list>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
constexpr const char* kSceneObjectDirectory = "Resources/json/3Dobject";
constexpr const char* kSceneSpriteDirectory = "Resources/json/sprite";
// 分割保存のファイル名と表示名。全体保存にはこの順序で並べる。
struct ObjectSaveCategory {
    SaveMode mode;
    const char* label;
    const char* suffix;
};

constexpr ObjectSaveCategory kDefaultObjectSaveCategory{SaveMode::kObject, "Object", "_object.json"};
constexpr ObjectSaveCategory kObjectSaveCategories[] = {
    {SaveMode::kPlayer, "Player", "_player.json"},
    {SaveMode::kEnemy, "Enemy", "_enemy.json"},
    kDefaultObjectSaveCategory,
    {SaveMode::kCamera, "Camera", "_camera.json"},
};

// 実体がカメラなら、オブジェクトに保存された分類より優先する。
const ObjectSaveCategory& ResolveObjectSaveCategory(bool isCamera, std::string_view category) {
    // 実体がカメラの場合は、指定された分類文字列にかかわらずカメラ用の接尾辞を使います。
    if (isCamera) {
        category = "Camera";
    }
    for (const auto& format : kObjectSaveCategories) {
        // 既知の分類名と一致した場合は、その分類の表示名と分割ファイル接尾辞を使います。
        if (category == format.label) {
            return format;
        }
    }
    // 分類なしと独自分類は通常のObjectファイルに置く。
    return kDefaultObjectSaveCategory;
}

// 保存入口では従来どおり最初の「.json」より前をベース名にする。
std::string GetSaveBaseName(const std::string& filename) {
    return filename.substr(0, filename.find(".json"));
}

std::string GetSceneBaseName(std::string filename) {
    // ディレクトリと拡張子を除き、分割されたScene JSONに共通のIDを取り出す。
    const size_t slash = filename.find_last_of("/\\");

    // フォルダー区切りが含まれていれば、最後の区切りより後を使います。
    if (slash != std::string::npos) {
        filename = filename.substr(slash + 1);
    }

    // 末尾が .json の場合だけ、シーンIDから拡張子を除きます。
    if (filename.size() >= 5 && filename.substr(filename.size() - 5) == ".json") {
        filename.resize(filename.size() - 5);
    }

    return filename;
}

bool HasSceneCategorySuffix(const std::string& filename) {
    // 分割ファイルを独立したシーンとして一覧へ重複登録しないための判定。
    for (const auto& format : kObjectSaveCategories) {
        const char* suffix = format.suffix;
        const size_t suffixLength = std::char_traits<char>::length(suffix);
        // カテゴリ別の接尾辞と完全に一致する場合、シーンの本体ではなく分割データとして扱います。
        if (filename.size() >= suffixLength &&
            filename.compare(filename.size() - suffixLength, suffixLength, suffix) == 0) {
            return true;
        }
    }

    return false;
}

bool IsValidSceneId(const std::string& sceneId, std::string& errorMessage) {
    // ファイル名として安全な文字だけ許し、カテゴリ別JSONの接尾辞とも衝突させない。
    // 空のIDは出力ファイルの名前を確定できないため受け付けません。
    if (sceneId.empty()) {
        errorMessage = "Scene IDを入力してください。";
        return false;
    }

    // 80バイトを超える入力は命名規則の上限を超えるため拒否します。
    if (sceneId.size() > 80) {
        errorMessage = "Scene IDは80文字以内にしてください。";
        return false;
    }

    for (unsigned char character : sceneId) {
        // 半角英数字・下線・ハイフン以外の文字を検出したら、そのIDを保存先に使いません。
        if (!std::isalnum(character) && character != '_' && character != '-') {
            errorMessage = "Scene IDには半角英数字、_、-だけを使用できます。";
            return false;
        }
    }

    const std::string filename = sceneId + ".json";

    // 既存の分割ファイルと同じ接尾辞になるIDを拒否し、本体と分割側の区別を保ちます。
    if (HasSceneCategorySuffix(filename)) {
        errorMessage = "_player、_enemy、_object、_cameraで終わるIDは使用できません。";
        return false;
    }

    return true;
}

json ReadJsonObject(const fs::path& path) {
    // シーンデータをオブジェクト型のJSONとして読み、不正な内容は空値へ戻す。
    std::ifstream stream(path);

    // ファイルを開けなければ空のオブジェクトを返します。
    if (!stream.is_open()) {
        return json::object();
    }

    try {
        json data;
        stream >> data;
        return data.is_object() ? data : json::object();
    }
    // JSON解析が例外で止まった場合も、有効なメタデータなしとして空を返します。
    catch (...) {
        return json::object();
    }
}

bool WriteJsonObject(const fs::path& path, const json& data, std::string& errorMessage) {
    // 保存先を用意してからJSONを書き、失敗理由をEditorへ返す。
    std::error_code error;
    fs::create_directories(path.parent_path(), error);

    // 親フォルダーを作成できなければ、ファイルを開きません。
    if (error) {
        errorMessage = "保存先フォルダーを作成できませんでした: " + path.parent_path().string();
        return false;
    }

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);

    // 保存先のファイルを開けなければ、書き込みできない理由を返します。
    if (!stream.is_open()) {
        errorMessage = "ファイルを作成できませんでした: " + path.string();
        return false;
    }

    stream << data.dump(4);

    // JSONの出力直後にストリームが失敗していれば、保存失敗として返します。
    if (!stream.good()) {
        errorMessage = "ファイルの書き込みに失敗しました: " + path.string();
        return false;
    }

    return true;
}

// 複製したSceneの全Objectに新GUIDを振り、親GUID参照も同じ対応表で更新します。
bool RegenerateCopiedSceneObjectGuids(
    const std::vector<fs::path>& paths,
    std::string& errorMessage) {
    // GUID の付け替えを行う文書と保存先。複数ファイル間の参照を同じ remap で更新する。
    struct SceneDocument {
        fs::path path;
        json data;
    };

    std::vector<SceneDocument> documents;
    std::unordered_map<std::string, std::string> guidRemap;

    for (const fs::path& path : paths) {
        // 元に無かった分割ファイルは複製されていないため飛ばします。
        if (!fs::exists(path))
            continue;

        json data = ReadJsonObject(path);

        // 配置配列を持たないメタデータや非対応の配置形式はGUIDの更新対象にしません。
        if (!data.contains("objects") || !data["objects"].is_array()) {
            continue;
        }

        for (json& object : data["objects"]) {
            // 配列に含まれていても、オブジェクト型でない要素にはGUIDを付けません。
            if (!object.is_object())
                continue;

            const std::string oldGuid = object.value("guid", std::string());
            const std::string newGuid = engine::Object3D::GeneratePersistentGuid();
            object["guid"] = newGuid;

            // 元のGUIDが有効な場合だけ、親参照の置換に使える対応表へ登録します。
            if (engine::Object3D::IsPersistentGuidValid(oldGuid)) {
                guidRemap.emplace(oldGuid, newGuid);
            }
        }

        documents.push_back({ path, std::move(data) });
    }

    // 全ファイルの旧GUIDを集めた後で親参照を置き換え、ファイル間の親子関係も保ちます。
    for (SceneDocument& document : documents) {
        for (json& object : document.data["objects"]) {
            // 親GUIDが文字列として保存された配置だけ、複製先の親参照を解決します。
            if (!object.is_object() || !object.contains("parentGuid") ||
                !object["parentGuid"].is_string()) {
                continue;
            }

            const auto parent = guidRemap.find(object["parentGuid"].get<std::string>());

            // 複製した範囲内に対応する親GUIDがあれば、複製後の親へつなぎます。
            if (parent != guidRemap.end()) {
                object["parentGuid"] = parent->second;
            }
        }

        // 変更した文書の保存に失敗した場合は、呼び出し側へ複製の失敗を返します。
        if (!WriteJsonObject(document.path, document.data, errorMessage)) {
            return false;
        }
    }

    return true;
}

// 新規Scene用の空のObject配置JSONを作ります。
json MakeEmptyObjectLayout() {
    json data;
    data["objects"] = json::array();
    return data;
}

// 設計解像度を含む空のSprite配置JSONを作ります。
json MakeEmptySpriteLayout() {
    json data;
    data["designResolution"] = { 1920.0f, 1080.0f };
    // 追加するSpriteの座標は1920×1080を基準とし、実ウィンドウサイズへの倍率を有効にする。
    data["scaleToWindow"] = true;
    data["sprites"] = json::array();
    return data;
}

// Scene Assetの識別子、実行クラス、参照リソースの既定値を保存JSONへ補います。
void ApplySceneAssetMetadata(
    json& data,
    const std::string& sceneId,
    const std::string& displayName,
    const std::string& runtimeScene) {
    data["_sceneAsset"]["version"] = 3;
    data["_sceneAsset"]["id"] = sceneId;
    data["_sceneAsset"]["displayName"] = displayName.empty() ? sceneId : displayName;
    data["_sceneAsset"]["runtimeScene"] = runtimeScene.empty() ? "SCENE_EDITOR" : runtimeScene;

    // 既存アセットで選ばれたControllerは保持し、未設定時だけ既定値を補います。
    if (!data["_sceneAsset"].contains("controller")) {
        data["_sceneAsset"]["controller"] = "DEFAULT";
    }

    // リソース欄が無いかオブジェクトでなければ、個別パスを書ける形に作り直します。
    if (!data["_sceneAsset"].contains("resources") || !data["_sceneAsset"]["resources"].is_object()) {
        data["_sceneAsset"]["resources"] = json::object();
    }

    // 個別リソースの参照は維持し、欠けたキーだけ空値で揃えます。
    json& resources = data["_sceneAsset"]["resources"];
    if (!resources.contains("bgm"))
        resources["bgm"] = "";
    if (!resources.contains("light"))
        resources["light"] = "";
    if (!resources.contains("camera"))
        resources["camera"] = "";
    if (!resources.contains("skybox"))
        resources["skybox"] = "";
    data["_sceneAsset"]["spriteLayout"] =
        std::string(kSceneSpriteDirectory) + "/" + sceneId + "_sprite.json";
    // 配置配列が無いメタデータには、関連する分割ファイルの案内も保存します。
    if (!data.contains("objects")) {
        data["_comment"] = "Actual data is in _player, _enemy, _object, and _camera.json";
    }
}

std::vector<fs::path> BuildObjectAssetPaths(const std::string& sceneId) {
    // 単一ファイルと種類別ファイルを同じ順序で列挙し、複製・削除で取りこぼさない。
    const fs::path base = fs::path(kSceneObjectDirectory) / sceneId;
    std::vector<fs::path> paths{base.string() + ".json"};
    for (const auto& format : kObjectSaveCategories) {
        paths.emplace_back(base.string() + format.suffix);
    }
    return paths;
}

bool AnySceneDestinationExists(const std::string& sceneId) {
    // ObjectとSpriteの両形式を調べ、作成・複製で既存ファイルを上書きしない。
    for (const fs::path& path : BuildObjectAssetPaths(sceneId)) {
        // 単一または分割の3D配置が一つでも存在する場合に、既存IDとして扱います。
        if (fs::exists(path)) {
            return true;
        }
    }

    return fs::exists(fs::path(kSceneSpriteDirectory) / (sceneId + "_sprite.json")) ||
        fs::exists(fs::path(kSceneSpriteDirectory) / (sceneId + ".json"));
}

std::string ResolveExistingSpritePath(const std::string& sceneId) {
    // 現行の接尾辞付きSprite配置を優先し、旧形式も参照可能にする。
    const fs::path standard = fs::path(kSceneSpriteDirectory) / (sceneId + "_sprite.json");

    // 接尾辞付きの現行Spriteレイアウトが実在する場合は最優先します。
    if (fs::exists(standard)) {
        return standard.generic_string();
    }

    const fs::path legacy = fs::path(kSceneSpriteDirectory) / (sceneId + ".json");

    // 現行レイアウトが無く、旧形式だけが存在する場合はそのパスを返します。
    if (fs::exists(legacy)) {
        return legacy.generic_string();
    }

    return {};
}

bool IsRuntimeReferencedScene(const std::string& sceneId, std::string& referenceDescription) {
    // 固定名で参照されるゲーム用シーンは、エディターからの削除候補にしない。
    static const char* kFixedRuntimeScenes[] = {
        "titleScene",
        "stageSelect",
        "gameOverScene",
        "tutorial",
        "stage1",
        "stage2",
        "stage3",
        "stage4",
        "stage5"
    };

    for (const char* fixedScene : kFixedRuntimeScenes) {
        // 起動やゲーム進行が直接参照する固定名に一致した時点で保護対象とします。
        if (sceneId == fixedScene) {
            referenceDescription = "ゲーム用Sceneクラスから固定パスで参照されています。";
            return true;
        }
    }

    const fs::path stageDefinitions = "Resources/json/stage_select/stages.json";
    const json root = ReadJsonObject(stageDefinitions);

    // ステージ定義から配列を取得できなければ、追加の参照元を特定できません。
    if (!root.contains("stages") || !root["stages"].is_array()) {
        return false;
    }

    const std::string expectedLevelPath =
        (fs::path(kSceneObjectDirectory) / (sceneId + ".json")).generic_string();

    // ステージ定義に採用されたJSONも、固定名以外であっても保護する。
    for (const json& stage : root["stages"]) {
        // ステージ定義のオブジェクトでない要素は参照の検索から除外します。
        if (!stage.is_object()) {
            continue;
        }

        // 3D配置へのパスが一致すれば、どのステージが参照しているかを返します。
        if (stage.value("levelPath", std::string()) == expectedLevelPath) {
            referenceDescription = "stages.jsonのステージ「" +
                stage.value("name", stage.value("id", sceneId)) + "」から参照されています。";
            return true;
        }
    }

    return false;
}

std::string InferLegacyRuntimeScene(const std::string& sceneId) {
    // 旧形式の既知のファイル名は、それぞれの実行用クラスへ対応付けます。
    if (sceneId == "titleScene")
        return "TITLE";
    if (sceneId == "stageSelect")
        return "SELECT";
    if (sceneId == "gameOverScene")
        return "GAMEOVER";
    if (sceneId == "sample")
        return "SCENE_EDITOR";
    if (sceneId == "tutorial")
        return "TUTORIAL";

    // 既知の画面名以外は、ステージ定義でレベルとして使われているか確認します。
    const json root = ReadJsonObject("Resources/json/stage_select/stages.json");

    // 定義にステージ配列があれば、現在のIDから求めたパスを検索します。
    if (root.contains("stages") && root["stages"].is_array()) {
        const std::string expectedPath =
            (fs::path(kSceneObjectDirectory) / (sceneId + ".json")).generic_string();
        for (const json& stage : root["stages"]) {
            // 定義がオブジェクトで配置パスが一致する場合に、実行クラスをGAMEPLAYと推定します。
            if (stage.is_object() && stage.value("levelPath", std::string()) == expectedPath) {
                return "GAMEPLAY";
            }
        }
    }

    return {};
}
}

void SceneSerializer::Initialize(DebugEditor* editor) {
    // 保存時にScene状態を取得するため、Editorの参照を借用します。
    editor_ = editor;
}

std::vector<SceneSerializer::SaveTarget> SceneSerializer::BuildSceneSaveTargets(const std::string& currentFilename, SaveMode mode) {
    std::vector<SaveTarget> targets;
    // シーン管理または現在のシーンが無ければ、保存対象の一覧を空のまま返します。
    if (!editor_->GetSceneManager() || !editor_->GetSceneManager()->GetCurrentScene())
        return targets;

    const std::string baseName = GetSaveBaseName(currentFilename);
    const std::string basePath = std::string(kSceneObjectDirectory) + "/" + baseName;

    // 対象が0件のカテゴリも空配列を保存し、以前の配置が残らないようにする。
    std::unordered_map<SaveMode, json> objectsByCategory;
    for (const auto& format : kObjectSaveCategories) {
        objectsByCategory[format.mode] = json::array();
    }

    BaseScene* currentScene = editor_->GetSceneManager()->GetCurrentScene();

    // 保存対象間で GUID が衝突しないよう、シリアライズ前にシーン全体を検査する。
    currentScene->EnsureUniquePersistentObjectGuids();
    auto& allObjects = currentScene->GetObjects();

    // 編集用の仮Objectを除き、名前の付いた配置Objectだけを保存へ渡す。
    for (auto& obj : allObjects) {
        // 名前なしの配置は後で個体を識別できないため保存対象から外します。
        if (obj->GetName().empty())
            continue;
        // 内部用フラグ・専用クラス・専用の接頭辞のどれかで指定された編集用配置は除外します。
        if (obj->IsEditorInternal() || obj->GetClassName() == "EditorOnly" || obj->GetName().rfind("__Editor_", 0) == 0)
            continue;

        json objectData = SerializeObject(obj.get());
        const auto& format = ResolveObjectSaveCategory(obj->IsCameraObject(), obj->GetSaveCategory());
        objectsByCategory.at(format.mode).push_back(std::move(objectData));
    }

    // 指定カテゴリだけ保存するときも、全体保存と同じ分類・接尾辞を使う。
    for (const auto& format : kObjectSaveCategories) {
        if (mode == SaveMode::kAll || mode == format.mode) {
            targets.push_back({format.label, basePath + format.suffix,
                {{"objects", std::move(objectsByCategory.at(format.mode))}}, false});
        }
    }

    // 全体保存の場合に、既存の表示名と実行設定を保ちながらメタデータも作成します。
    if (mode == SaveMode::kAll) {
        const fs::path metadataPath = fs::path(kSceneObjectDirectory) / currentFilename;
        json metadata = ReadJsonObject(metadataPath);
        metadata.erase("objects");
        std::string displayName = baseName;

        // 既存メタデータがオブジェクトなら、表示名を引き継ぎます。
        if (metadata.contains("_sceneAsset") && metadata["_sceneAsset"].is_object()) {
            displayName = metadata["_sceneAsset"].value("displayName", baseName);
        }

        const std::string runtimeScene = metadata.contains("_sceneAsset") && metadata["_sceneAsset"].is_object()
            ? metadata["_sceneAsset"].value("runtimeScene", std::string("SCENE_EDITOR"))
            : "SCENE_EDITOR";
        ApplySceneAssetMetadata(metadata, baseName, displayName, runtimeScene);
        targets.push_back({ "Meta", metadataPath.generic_string(), metadata, true });
    }

    return targets;
}

std::vector<SceneSerializer::SaveTarget> SceneSerializer::BuildSingleObjectSaveTargets(engine::Object3D* object, const std::string& filename) {
    std::vector<SaveTarget> targets;
    // 対象を取得できなければ、カテゴリも保存先も決められないため空を返します。
    if (!object)
        return targets;

    // 保存先の分類は全体保存と共通。表示名には元のカテゴリを使う。
    const std::string cat = object->GetSaveCategory();
    const auto& format = ResolveObjectSaveCategory(object->IsCameraObject(), cat);
    const std::string targetFilename = std::string(kSceneObjectDirectory) + "/" +
        GetSaveBaseName(filename) + format.suffix;

    // 元のカテゴリファイルが読める場合は、その中から今回の対象だけを更新します。
    json sceneData;
    std::ifstream inFile(targetFilename);

    if (inFile.is_open()) {
        try {
            // ファイルにデータが入っていれば解析します。
            if (inFile.peek() != std::ifstream::traits_type::eof())
                inFile >> sceneData;
            // 空ファイルは、まだ保存されていない配置配列として扱います。
            else sceneData["objects"] = json::array();
        }
        catch (...) {
            sceneData["objects"] = json::array();
        }

        inFile.close();
    }
    // カテゴリファイルを開けなければ、新しい配置配列から始めます。
    else {
        sceneData["objects"] = json::array();
    }

    // 現在の個体を保存形式に変換し、元の配置行を残すか入れ替えるか判定します。
    json currentData = SerializeObject(object);

    // GUID を優先し、旧形式で GUID がない行だけ名前で照合する。
    bool found = false;

    // 既存JSONに配置配列が無ければ、書き込み先として空の配列を設定します。
    if (!sceneData.contains("objects") || !sceneData["objects"].is_array()) {
        sceneData["objects"] = json::array();
    }

    for (auto& objData : sceneData["objects"]) {
        const bool sameGuid = objData.contains("guid") && objData["guid"].is_string() &&
            objData["guid"] == object->GetPersistentGuid();
        const bool legacyNameMatch = !objData.contains("guid") &&
            objData.contains("name") && objData["name"] == object->GetName();
        // GUIDが一致した行か、GUIDの無い旧形式で名前が一致した行だけを置き換えます。
        if (sameGuid || legacyNameMatch) {
            objData = currentData;
            found = true;
            break;
        }
    }

    if (!found) {
        // 保存済み配置と一致する行がなければ、今回のObjectを新規行として追記する。
        sceneData["objects"].push_back(currentData);
    }

    targets.push_back({ cat.empty() ? "Object" : cat, targetFilename, sceneData, false });
    return targets;
}

std::string SceneSerializer::SaveTargets(const std::vector<SaveTarget>& targets) {
    std::string savedFilesMsg;

    // 表示用ラベルを返すだけで、個々の書き込み成否はこの関数では集計しない。
    for (const auto& target : targets) {
        SaveToFile(target.path, target.data);

        // 画面に表示するラベルがある対象だけ、戻り値の一覧に加えます。
        if (!target.label.empty()) {
            savedFilesMsg += target.label + ", ";
        }
    }

    // 一覧が空でない場合は、最後に付けたカンマと空白を取り除きます。
    if (!savedFilesMsg.empty()) {
        savedFilesMsg.pop_back();
        savedFilesMsg.pop_back();
    }

    return savedFilesMsg;
}

std::string SceneSerializer::SaveScene(const std::string& currentFilename, SaveMode mode) {
    // 保存モードから対象ファイルを構築し、実際の書込経路はSaveTargetsへ集約します。
    return SaveTargets(BuildSceneSaveTargets(currentFilename, mode));
}

nlohmann::json SceneSerializer::SerializeObject(engine::Object3D* obj) {
    json snapshot = obj->ExportToJson();
    json scene = json::object();
    auto& snapshotFields = snapshot.get_ref<json::object_t&>();
    auto& sceneFields = scene.get_ref<json::object_t&>();
    // Object全体のスナップショットから、シーン形式が扱うキーだけを移す。
    const auto moveFields = [&](std::initializer_list<const char*> keys) {
        for (const char* key : keys) {
            // メンバーがスナップショットに存在した場合だけ、移動してシーン形式に含めます。
            if (const auto field = snapshotFields.find(key); field != snapshotFields.end()) {
                sceneFields.insert(snapshotFields.extract(field));
            }
        }
    };

    // カメラにも保存する共通項目。Scene JSONで許可するキーをここに示す。
    moveFields({"guid", "name", "components", "tag", "layer", "saveCategory",
                "enemyType", "gimmickType", "itemType",
                "quaternion", "scale", "animation", "recorder"});
    // Prefabに属する個体の場合は、所属を復元するためのメタデータも含めます。
    if (obj->IsPrefabInstance()) {
        moveFields({"prefabInstance"});
    }

    const std::string className = obj->GetClassName();
    scene["type"] = className.empty() ? "Model" : className;
    scene["position"] = snapshot.at("translate");
    scene["rotation"] = snapshot.at("rotate");
    scene["recorder"].erase("phaseOffset");
    editorCommon::WriteObjectParentReference(scene, *obj);

    // カメラ自身の場合は保存上の型を統一し、カメラ設定を移した時点で返します。
    if (obj->IsCameraObject()) {
        scene["type"] = "Camera";
        scene["saveCategory"] = "Camera";
        moveFields({"camera"});
        return scene;
    }

    // カメラ専用項目を返した後は通常配置を処理する。非表示の判定箱には描画モデル名を保存しない。
    if (className != "InvisibleBox") {
        moveFields({"modelName"});
    }
    moveFields({"targetID", "myEventID", "collider", "collisionAttribute", "collisionMask", "collisionAttributeIds",
                "collisionMaskIds", "collisionAttributeUnknownBits", "collisionMaskUnknownBits",
                "terrainCollisionPath", "color", "blendMode", "materialType", "shaderMaterialId",
                "materialInstance", "meshDrawIndex", "metallic", "roughness", "enableNormalMap",
                "normalMapPath", "ormMapPath", "texturePath", "textureTiling", "autoTextureTiling",
                "enableLighting", "enableEnvMap", "envIntensity", "emissive", "castShadow", "isStatic",
                "particleName", "gpuParticleName", "meshEffect1", "meshEffect2", "localFog", "waterParam"});
    scene["eventID"] = snapshot.at("eventType");

    // ゲーム側の型別パラメータは専用変換へ委ね、値があるときだけ配置 JSON へ追加する。
    const json parameter = sceneEntityParameterCodec::Serialize(*obj);
    // ゲーム用のパラメーターが空でない場合に、専用のパラメーター欄を保存します。
    if (!parameter.empty()) {
        scene["param"] = parameter;
    }
    return scene;
}

// 対象個体がある場合だけ、その個体を含むファイルを更新します。
void SceneSerializer::UpdateObjectInSceneJson(engine::Object3D* object, const std::string& filename) {
    // 単一Objectの差分だけを保存対象にし、Scene全体の再保存を避けます。
    // 保存する個体が無ければ、対象のファイルを開きません。
    if (!object)
        return;

    SaveTargets(BuildSingleObjectSaveTargets(object, filename));
    DebugConsole::GetInstance()->AddLog("Updated object JSON: " + object->GetName());
}

void SceneSerializer::SaveToFile(const std::string& path, const json& data) {
    // ファイルを開けた場合にJSONを出力し、出力先をログに残します。
    std::ofstream f(path);

    // ファイルを開けた場合にだけ、JSONの出力と保存先のログを行います。
    if (f.is_open()) {
        f << data.dump(4);
        DebugConsole::GetInstance()->AddLog("Saved JSON to " + path);
    }
}

std::vector<SceneSerializer::SceneAssetInfo> SceneSerializer::DiscoverSceneAssets() const {
    // Scene ObjectのJSONから利用可能なScene Asset情報を列挙します。
    std::vector<SceneAssetInfo> assets;
    const fs::path directory = kSceneObjectDirectory;

    // 配置ディレクトリが無ければ、開けるScene Assetの一覧を空で返します。
    if (!fs::exists(directory)) {
        return assets;
    }

    std::error_code error;

    for (const fs::directory_entry& entry : fs::directory_iterator(directory, error)) {
        // ディレクトリの列挙が正常であり、拡張子が .json の通常ファイルだけを候補にします。
        if (error || !entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }

        const std::string filename = entry.path().filename().string();

        // 分割用のファイルは、本体と重複して一覧へ出さないよう飛ばします。
        if (HasSceneCategorySuffix(filename)) {
            continue;
        }

        SceneAssetInfo asset;
        asset.filename = filename;
        asset.id = GetSceneBaseName(filename);
        asset.displayName = asset.id;
        asset.objectLayoutPath = entry.path().generic_string();

        const json metadata = ReadJsonObject(entry.path());
        const bool hasSceneAssetMetadata =
            metadata.contains("_sceneAsset") && metadata["_sceneAsset"].is_object();
        // 明示メタデータを優先し、旧形式は実行クラスを推定できるSceneだけ一覧へ残す。
        if (hasSceneAssetMetadata) {
            const json& sceneAsset = metadata["_sceneAsset"];
            asset.displayName = sceneAsset.value("displayName", asset.id);
            asset.runtimeScene = sceneAsset.value(
                "runtimeScene",
                std::string("SCENE_EDITOR"));
            asset.controllerName = sceneAsset.value("controller", std::string("DEFAULT"));

            // リソース設定がオブジェクトなら、BGM・光・カメラ・空の各パスを取得します。
            if (sceneAsset.contains("resources") && sceneAsset["resources"].is_object()) {
                const json& resources = sceneAsset["resources"];
                asset.bgmPath = resources.value("bgm", std::string());
                asset.lightPath = resources.value("light", std::string());
                asset.cameraPath = resources.value("camera", std::string());
                asset.skyboxPath = resources.value("skybox", std::string());
            }
        }
        // 現行のメタデータが無い場合は、旧形式の名前・定義で実行クラスを探します。
        else {
            asset.runtimeScene = InferLegacyRuntimeScene(asset.id);

            // 旧形式で実行クラスを決定できなければ、開く対象に含めません。
            if (asset.runtimeScene.empty()) {
                continue;
            }

            asset.usesLegacyMetadata = true;
        }

        // メタデータの有無とは別に、分割配置と現行・旧形式のSpriteファイルの実在を調べる。
        const std::vector<fs::path> objectPaths = BuildObjectAssetPaths(asset.id);

        for (size_t index = 1; index < objectPaths.size(); ++index) {
            // 分割カテゴリが一つでも実在する場合に、分割形式として表示します。
            if (fs::exists(objectPaths[index])) {
                asset.usesSplitFiles = true;
                break;
            }
        }

        asset.spriteLayoutPath = ResolveExistingSpritePath(asset.id);
        asset.hasSpriteLayout = !asset.spriteLayoutPath.empty();
        assets.push_back(std::move(asset));
    }

    std::sort(assets.begin(), assets.end(), [](const SceneAssetInfo& left, const SceneAssetInfo& right) {
        // 表示名が異なる場合は表示名順にし、同名の場合はID順を使います。
        if (left.displayName != right.displayName) {
            return left.displayName < right.displayName;
        }
        return left.id < right.id;
    });
    return assets;
}

bool SceneSerializer::CreateSceneAsset(
    const std::string& sceneIdInput,
    const std::string& displayName,
    const std::string& runtimeScene,
    SceneAssetTemplate sceneTemplate,
    std::string& createdFilename,
    std::string& errorMessage) {
    errorMessage.clear();
    createdFilename.clear();
    const std::string sceneId = GetSceneBaseName(sceneIdInput);

    // IDが命名規則を満たさなければ、配置やメタデータを作成しません。
    if (!IsValidSceneId(sceneId, errorMessage)) {
        return false;
    }

    // ファイルを作る前に、実行クラスの登録とScene IDの重複を検査する。
    if (runtimeScene.empty() || !editor_ || !editor_->GetSceneManager() ||
        !editor_->GetSceneManager()->IsSceneRegistered(runtimeScene)) {
        errorMessage = "登録されていない実行クラスです: " + runtimeScene;
        return false;
    }

    // 同じIDの既存ファイルが見つかった場合は、新規作成を中止します。
    if (AnySceneDestinationExists(sceneId)) {
        errorMessage = "同じScene IDのファイルが既に存在します。";
        return false;
    }

    const std::string filename = sceneId + ".json";

    // 現在のシーンを雛形に選んだ場合は、その配置とSpriteのレイアウトを引き継ぎます。
    if (sceneTemplate == SceneAssetTemplate::kCurrentScene) {
        // 現在のシーンを取得できなければ、雛形として使える配置がありません。
        if (!editor_ || !editor_->GetSceneManager() || !editor_->GetSceneManager()->GetCurrentScene()) {
            errorMessage = "複製元となる現在のシーンがありません。";
            return false;
        }

        // 現在Sceneの配置を新IDの保存先へ組み立て、メタデータだけ新規作成時の指定へ差し替える。
        std::vector<SaveTarget> targets = BuildSceneSaveTargets(filename, SaveMode::kAll);

        for (SaveTarget& target : targets) {
            // メタデータに該当する対象だけ、作成時に指定された表示名と実行クラスへ差し替えます。
            if (target.isMetadata) {
                ApplySceneAssetMetadata(target.data, sceneId, displayName, runtimeScene);
            }
        }

        SaveTargets(targets);

        const std::string currentId = GetSceneBaseName(editor_->GetCurrentSceneFilenameBuffer());
        const std::string currentSpritePath = ResolveExistingSpritePath(currentId);
        const fs::path newSpritePath = fs::path(kSceneSpriteDirectory) / (sceneId + "_sprite.json");

        // Object 側の複製だけでは UI レイアウトを失うため、Sprite JSON も同じ Scene ID へ複製する。
        if (!currentSpritePath.empty()) {
            std::error_code copyError;
            fs::copy_file(currentSpritePath, newSpritePath, fs::copy_options::none, copyError);

            // Spriteの複製に失敗したときは、作りかけの新規Assetの削除を試します。
            if (copyError) {
                errorMessage = "Spriteレイアウトの複製に失敗しました: " + copyError.message();
                DeleteSceneAsset(filename, errorMessage);
                return false;
            }
        }
        // 既存のSpriteレイアウトが無く、空のレイアウトすら作れなかった場合は作成を中止します。
        else if (!WriteJsonObject(newSpritePath, MakeEmptySpriteLayout(), errorMessage)) {
            DeleteSceneAsset(filename, errorMessage);
            return false;
        }
    }
    else {
        // 空テンプレートは複数ファイルで構成される。途中失敗時はこの呼び出しで作った分だけ戻す。
        std::vector<fs::path> createdPaths;
        auto writeAndTrack = [&](const fs::path& path, const json& data) {
            // 出力に失敗したパスは成功済み一覧に含めず、呼び出し側へ失敗を返します。
            if (!WriteJsonObject(path, data, errorMessage)) {
                return false;
            }

            createdPaths.push_back(path);
            return true;
        };

        json metadata = json::object();
        ApplySceneAssetMetadata(metadata, sceneId, displayName, runtimeScene);
        const std::vector<fs::path> objectPaths = BuildObjectAssetPaths(sceneId);

        // 最初のメタデータが保存できなければ、カテゴリ別ファイルへ進みません。
        if (!writeAndTrack(objectPaths[0], metadata)) {
            return false;
        }

        for (size_t index = 1; index < objectPaths.size(); ++index) {
            // いずれかの空の3D配置で失敗したら、それまでに成功したファイルを削除します。
            if (!writeAndTrack(objectPaths[index], MakeEmptyObjectLayout())) {
                for (const fs::path& createdPath : createdPaths) {
                    std::error_code removeError;
                    fs::remove(createdPath, removeError);
                }

                return false;
            }
        }

        // 空のSprite配置が書けなかった場合も、保存済みの3D配置とメタデータを削除します。
        if (!writeAndTrack(fs::path(kSceneSpriteDirectory) / (sceneId + "_sprite.json"), MakeEmptySpriteLayout())) {
            for (const fs::path& createdPath : createdPaths) {
                std::error_code removeError;
                fs::remove(createdPath, removeError);
            }

            return false;
        }
    }

    createdFilename = filename;
    return true;
}

bool SceneSerializer::DuplicateSceneAsset(
    const std::string& sourceFilename,
    const std::string& newSceneIdInput,
    const std::string& displayName,
    std::string& createdFilename,
    std::string& errorMessage) {
    // 新しいScene IDと出力先の重複を先に調べ、関連JSONを一式として複製します。
    errorMessage.clear();
    createdFilename.clear();
    const std::string sourceId = GetSceneBaseName(sourceFilename);
    const std::string newSceneId = GetSceneBaseName(newSceneIdInput);

    // 新しいIDが規則に合わなければ、複製先を作りません。
    if (!IsValidSceneId(newSceneId, errorMessage)) {
        return false;
    }

    // 新しいIDに関連ファイルが既にある場合は上書きせず終了します。
    if (AnySceneDestinationExists(newSceneId)) {
        errorMessage = "同じScene IDのファイルが既に存在します。";
        return false;
    }

    const std::vector<fs::path> sourcePaths = BuildObjectAssetPaths(sourceId);
    const std::vector<fs::path> destinationPaths = BuildObjectAssetPaths(newSceneId);

    // 複製元の本体ファイルが存在しない場合は、関連ファイルのコピーを始めません。
    if (!fs::exists(sourcePaths[0])) {
        errorMessage = "複製元のScene Assetが見つかりません。";
        return false;
    }

    std::vector<fs::path> createdPaths;

    // Scene本体だけでなくカテゴリ別JSONも複製し、途中失敗時は作成済みファイルだけを巻き戻す。
    for (size_t index = 0; index < sourcePaths.size(); ++index) {
        // 存在しない分割元ファイルは、複製先にも作らず飛ばします。
        if (!fs::exists(sourcePaths[index])) {
            continue;
        }

        std::error_code copyError;
        fs::copy_file(sourcePaths[index], destinationPaths[index], fs::copy_options::none, copyError);

        // 一つでもコピーに失敗したら、すでに複製できたファイルを削除して止めます。
        if (copyError) {
            errorMessage = "Scene Assetの複製に失敗しました: " + copyError.message();

            for (const fs::path& path : createdPaths) {
                std::error_code removeError;
                fs::remove(path, removeError);
            }

            return false;
        }

        createdPaths.push_back(destinationPaths[index]);
    }

    // 複製先のGUIDを振り直せなければ、作成した配置を残さず失敗を返します。
    if (!RegenerateCopiedSceneObjectGuids(destinationPaths, errorMessage)) {
        // GUIDを複製したまま残すと元Sceneと同一Objectとして扱われるため、失敗時は複製を破棄する。
        for (const fs::path& path : createdPaths) {
            std::error_code removeError;
            fs::remove(path, removeError);
        }

        return false;
    }

    const std::string sourceSpritePath = ResolveExistingSpritePath(sourceId);
    // 現行・旧形式のどちらから複製しても、出力先は現在の Sprite 配置名へ揃える。
    const fs::path destinationSpritePath = fs::path(kSceneSpriteDirectory) / (newSceneId + "_sprite.json");

    if (!sourceSpritePath.empty()) {
        // 元SceneのSprite配置があれば引き継ぎ、なければ編集可能な空レイアウトを用意する。
        std::error_code copyError;
        fs::copy_file(sourceSpritePath, destinationSpritePath, fs::copy_options::none, copyError);

        // 元にあったSpriteレイアウトのコピーが失敗したら、作成済みの配置も削除します。
        if (copyError) {
            errorMessage = "Spriteレイアウトの複製に失敗しました: " + copyError.message();

            for (const fs::path& path : createdPaths) {
                std::error_code removeError;
                fs::remove(path, removeError);
            }

            return false;
        }

        createdPaths.push_back(destinationSpritePath);
    }
    // 元にSprite配置が無く、新しい空レイアウトの保存も失敗した場合は複製を取り消します。
    else if (!WriteJsonObject(destinationSpritePath, MakeEmptySpriteLayout(), errorMessage)) {
        for (const fs::path& path : createdPaths) {
            std::error_code removeError;
            fs::remove(path, removeError);
        }

        return false;
    }

    // 配置の複製後も実行クラスは引き継ぎ、識別子・表示名・Sprite参照を複製先へ揃える。
    json metadata = ReadJsonObject(destinationPaths[0]);
    const std::string runtimeScene = metadata.contains("_sceneAsset") && metadata["_sceneAsset"].is_object()
        ? metadata["_sceneAsset"].value("runtimeScene", std::string("SCENE_EDITOR"))
        : "SCENE_EDITOR";
    ApplySceneAssetMetadata(metadata, newSceneId, displayName, runtimeScene);

    // 複製先のメタデータを保存できなければ、ここまで作ったファイルを削除します。
    if (!WriteJsonObject(destinationPaths[0], metadata, errorMessage)) {
        for (const fs::path& path : createdPaths) {
            std::error_code removeError;
            fs::remove(path, removeError);
        }

        return false;
    }

    createdFilename = newSceneId + ".json";
    return true;
}

bool SceneSerializer::RenameSceneAsset(
    const std::string& sourceFilename,
    const std::string& newSceneIdInput,
    const std::string& displayName,
    std::string& renamedFilename,
    std::string& errorMessage) {
    errorMessage.clear();
    renamedFilename.clear();
    const std::string sourceId = GetSceneBaseName(sourceFilename);
    const std::string newSceneId = GetSceneBaseName(newSceneIdInput);

    // 新しいIDが命名規則を満たさない場合は変更しません。
    if (!IsValidSceneId(newSceneId, errorMessage)) {
        return false;
    }

    // 表示名だけの変更は許し、IDを変える場合だけ出力先の衝突を検査する。
    if (sourceId != newSceneId && AnySceneDestinationExists(newSceneId)) {
        errorMessage = "同じScene IDのファイルが既に存在します。";
        return false;
    }

    // 固定パスやステージ定義の参照元はここでは書き換えないため、参照中のID変更を拒否する。
    if (sourceId != newSceneId) {
        std::string referenceDescription;

        // 外部参照で保護されている場合は、既存の参照を保つため改名を拒否します。
        if (IsRuntimeReferencedScene(sourceId, referenceDescription)) {
            errorMessage = "このScene IDは変更できません。" + referenceDescription;
            return false;
        }
    }

    const std::vector<fs::path> sourcePaths = BuildObjectAssetPaths(sourceId);
    const std::vector<fs::path> destinationPaths = BuildObjectAssetPaths(newSceneId);

    // 改名元の本体が見つからなければ、ファイル移動は始めません。
    if (!fs::exists(sourcePaths[0])) {
        errorMessage = "名前を変更するScene Assetが見つかりません。";
        return false;
    }

    // 移動済みの組を記録し、後続の名前変更が失敗したときに逆順で元へ戻す。
    struct MovedPath {
        fs::path source;
        fs::path destination;
    };

    std::vector<MovedPath> movedPaths;

    // 表示名だけの変更ならファイルは移動しません。IDを変える場合に一式を移します。
    if (sourceId != newSceneId) {
        // 本体と存在するカテゴリ別JSONを同じIDへ移し、旧形式の単一ファイルにも対応する。
        for (size_t index = 0; index < sourcePaths.size(); ++index) {
            // 元に存在しない分割ファイルは、移動する必要がないため飛ばします。
            if (!fs::exists(sourcePaths[index])) {
                continue;
            }

            std::error_code moveError;
            fs::rename(sourcePaths[index], destinationPaths[index], moveError);

            // 一つでも移動に失敗したら、先に移動できたファイルを逆順で戻します。
            if (moveError) {
                errorMessage = "Scene Assetの名前変更に失敗しました: " + moveError.message();

                for (auto rollback = movedPaths.rbegin(); rollback != movedPaths.rend(); ++rollback) {
                    std::error_code rollbackError;
                    fs::rename(rollback->destination, rollback->source, rollbackError);
                }

                return false;
            }

            movedPaths.push_back({ sourcePaths[index], destinationPaths[index] });
        }

        // Spriteも同じ改名履歴へ加え、後続処理の失敗時にはObject側と一緒に名前を戻す。
        const std::string sourceSpritePath = ResolveExistingSpritePath(sourceId);

        // 現在の形式か旧形式でSprite配置が見つかった場合に、新しいIDへ移動します。
        if (!sourceSpritePath.empty()) {
            const fs::path destinationSpritePath = fs::path(kSceneSpriteDirectory) / (newSceneId + "_sprite.json");
            std::error_code moveError;
            fs::rename(sourceSpritePath, destinationSpritePath, moveError);

            // Sprite配置の移動に失敗した場合は、先に移動した3Dファイルを元の名前へ戻します。
            if (moveError) {
                errorMessage = "Spriteレイアウトの名前変更に失敗しました: " + moveError.message();

                for (auto rollback = movedPaths.rbegin(); rollback != movedPaths.rend(); ++rollback) {
                    std::error_code rollbackError;
                    fs::rename(rollback->destination, rollback->source, rollbackError);
                }

                return false;
            }

            movedPaths.push_back({ sourceSpritePath, destinationSpritePath });
        }
    }

    // ファイル名の変更後に内部IDとSprite参照を揃える。IDが同じ場合は表示名の更新もここで行う。
    const fs::path metadataPath = destinationPaths[0];
    json metadata = ReadJsonObject(metadataPath);
    const std::string runtimeScene = metadata.contains("_sceneAsset") && metadata["_sceneAsset"].is_object()
        ? metadata["_sceneAsset"].value("runtimeScene", std::string("SCENE_EDITOR"))
        : "SCENE_EDITOR";
    ApplySceneAssetMetadata(metadata, newSceneId, displayName, runtimeScene);

    // 保存失敗時に戻すのは移動したパス名。メタデータの書込内容を復元する処理ではない。
    if (!WriteJsonObject(metadataPath, metadata, errorMessage)) {
        for (auto rollback = movedPaths.rbegin(); rollback != movedPaths.rend(); ++rollback) {
            std::error_code rollbackError;
            fs::rename(rollback->destination, rollback->source, rollbackError);
        }

        return false;
    }

    renamedFilename = newSceneId + ".json";
    return true;
}

bool SceneSerializer::DeleteSceneAsset(const std::string& filename, std::string& errorMessage) {
    // 実行側から参照されるSceneは削除を拒み、参照切れを防ぎます。
    errorMessage.clear();
    const std::string sceneId = GetSceneBaseName(filename);
    std::string referenceDescription;

    // ゲームやステージ定義から参照されている場合は、ファイルの削除を開始しません。
    if (IsRuntimeReferencedScene(sceneId, referenceDescription)) {
        errorMessage = "このScene Assetは削除できません。" + referenceDescription;
        return false;
    }

    bool removedAny = false;

    // 存在する構成ファイルを順に削除する。途中失敗時に削除済みファイルを復元する処理はない。
    for (const fs::path& path : BuildObjectAssetPaths(sceneId)) {
        // 本体または分割ファイルが無ければ、削除する対象がありません。
        if (!fs::exists(path)) {
            continue;
        }

        std::error_code removeError;

        // 3D配置を削除できなければ、後続ファイルを処理する前に失敗を返します。
        if (!fs::remove(path, removeError) || removeError) {
            errorMessage = "Scene Assetを削除できませんでした: " + path.string();
            return false;
        }

        removedAny = true;
    }

    const fs::path spritePaths[] = {
        fs::path(kSceneSpriteDirectory) / (sceneId + "_sprite.json"),
        // 接尾辞を付けなかった旧形式も、残っていれば同じSceneの関連ファイルとして削除する。
        fs::path(kSceneSpriteDirectory) / (sceneId + ".json")
    };

    for (const fs::path& path : spritePaths) {
        // 現行・旧形式のうち実在するSprite配置だけを削除します。
        if (!fs::exists(path)) {
            continue;
        }

        std::error_code removeError;

        // Sprite配置の削除に失敗した場合は、そのファイルを示して終了します。
        if (!fs::remove(path, removeError) || removeError) {
            errorMessage = "Spriteレイアウトを削除できませんでした: " + path.string();
            return false;
        }

        removedAny = true;
    }

    // 3D・Spriteのどのファイルも削除対象にならなかった場合は、対象無しの失敗を返します。
    if (!removedAny) {
        errorMessage = "削除するScene Assetが見つかりません。";
        return false;
    }

    return true;
}

bool SceneSerializer::SetSceneAssetRuntimeScene(
    const std::string& filename,
    const std::string& runtimeScene,
    std::string& errorMessage) {
    // 登録済み実行クラスだけを許し、Asset metadataの実行先を変更します。
    errorMessage.clear();
    // 指定名が空、シーン管理が利用不可、または名前が未登録なら実行先を変更できません。
    if (runtimeScene.empty() || !editor_ || !editor_->GetSceneManager() ||
        !editor_->GetSceneManager()->IsSceneRegistered(runtimeScene)) {
        errorMessage = "登録されていない実行クラスです: " + runtimeScene;
        return false;
    }

    const fs::path metadataPath = fs::path(kSceneObjectDirectory) / (GetSceneBaseName(filename) + ".json");

    // 本体のJSONが存在しなければ、新しい実行設定は保存しません。
    if (!fs::exists(metadataPath)) {
        errorMessage = "Scene Assetが見つかりません: " + filename;
        return false;
    }

    json metadata = ReadJsonObject(metadataPath);
    const std::string sceneId = GetSceneBaseName(filename);
    const std::string displayName = metadata.contains("_sceneAsset") && metadata["_sceneAsset"].is_object()
        ? metadata["_sceneAsset"].value("displayName", sceneId)
        : sceneId;
    ApplySceneAssetMetadata(metadata, sceneId, displayName, runtimeScene);
    return WriteJsonObject(metadataPath, metadata, errorMessage);
}

bool SceneSerializer::SetSceneAssetRuntimeSettings(
    const std::string& filename,
    const std::string& controllerName,
    const std::string& bgmPath,
    const std::string& lightPath,
    const std::string& cameraPath,
    const std::string& skyboxPath,
    std::string& errorMessage) {
    // 実行設定をScene metadataへまとめ、個々のAsset参照を同時に保存します。
    errorMessage.clear();

    // Controller名が空の場合は、更新すべき設定を確定できないため保存しません。
    if (controllerName.empty()) {
        errorMessage = "Controller名を入力してください。";
        return false;
    }

    const std::string sceneId = GetSceneBaseName(filename);
    const fs::path metadataPath = fs::path(kSceneObjectDirectory) / (sceneId + ".json");

    // 保存するScene Assetが存在しなければ、新しい設定を作りません。
    if (!fs::exists(metadataPath)) {
        errorMessage = "Scene Assetが見つかりません: " + filename;
        return false;
    }

    json metadata = ReadJsonObject(metadataPath);
    const bool hasSceneAsset = metadata.contains("_sceneAsset") && metadata["_sceneAsset"].is_object();
    const std::string displayName = hasSceneAsset
        ? metadata["_sceneAsset"].value("displayName", sceneId)
        : sceneId;
    // 旧形式では実行クラスを推定し、特定できない場合は汎用Sceneとしてメタデータを補う。
    std::string runtimeScene = hasSceneAsset
        ? metadata["_sceneAsset"].value("runtimeScene", std::string())
        : InferLegacyRuntimeScene(sceneId);
    // 取得値または推定結果が空なら、汎用の編集シーンを実行クラスにします。
    if (runtimeScene.empty()) {
        runtimeScene = "SCENE_EDITOR";
    }

    ApplySceneAssetMetadata(metadata, sceneId, displayName, runtimeScene);
    metadata["_sceneAsset"]["controller"] = controllerName;
    json& resources = metadata["_sceneAsset"]["resources"];
    resources["bgm"] = bgmPath;
    resources["light"] = lightPath;
    resources["camera"] = cameraPath;
    resources["skybox"] = skyboxPath;
    return WriteJsonObject(metadataPath, metadata, errorMessage);
}

SceneSerializer::SceneAssetValidationResult SceneSerializer::ValidateSceneAsset(
    const std::string& filename) const {
    // Object配置と実行クラスなどの参照を検査し、開く前に問題を列挙します。
    SceneAssetValidationResult result;
    const std::vector<SceneAssetInfo> assets = DiscoverSceneAssets();
    const auto asset = std::find_if(assets.begin(), assets.end(), [&](const SceneAssetInfo& candidate) {
        return candidate.filename == filename;
    });

    // 対象が一覧に無い場合は、以降のチェックに必要な参照を取得できないため終了します。
    if (asset == assets.end()) {
        result.errors.push_back("Scene Assetが見つかりません: " + filename);
        return result;
    }

    // 3D配置の本体が実在しなければ、読み込みに必要なファイルの欠落として扱います。
    if (!fs::exists(asset->objectLayoutPath)) {
        result.errors.push_back("Objectレイアウトが見つかりません: " + asset->objectLayoutPath);
    }

    // Sprite配置の指定があるのにファイルが実在しなければ、欠落をエラーとします。
    if (!asset->spriteLayoutPath.empty() && !fs::exists(asset->spriteLayoutPath)) {
        result.errors.push_back("Spriteレイアウトが見つかりません: " + asset->spriteLayoutPath);
    }

    // 旧形式から見つけたAssetでは、設定を保存すると形式を更新できることを案内します。
    if (asset->usesLegacyMetadata) {
        result.warnings.push_back("旧形式Sceneです。実行設定を保存するとScene Asset形式へ更新されます。");
    }

    // リソースの空欄は既定値を使うため、指定済みのパスだけ存在を検査する。
    if (!asset->bgmPath.empty() && !fs::exists(asset->bgmPath)) {
        result.warnings.push_back("BGMが見つかりません: " + asset->bgmPath);
    }

    if (!asset->lightPath.empty() && !fs::exists(asset->lightPath)) {
        result.warnings.push_back("Light JSONが見つかりません: " + asset->lightPath);
    }

    // カメラだけは保存名をカメラ用の基準フォルダーに結合して存在を確認します。
    if (!asset->cameraPath.empty() &&
        !fs::exists(fs::path("Resources/json/camera") / asset->cameraPath)) {
        result.warnings.push_back("Camera JSONが見つかりません: " + asset->cameraPath);
    }

    if (!asset->skyboxPath.empty() && !fs::exists(asset->skyboxPath)) {
        result.warnings.push_back("Skyboxが見つかりません: " + asset->skyboxPath);
    }

    // 単一・分割の配置を横断して必要Objectと名前の重複を集め、ファイル単位の見落としを防ぐ。
    bool hasPlayer = false;
    bool hasCamera = false;
    bool hasGoal = false;
    std::unordered_set<std::string> objectNames;
    std::unordered_set<std::string> duplicateNames;
    const std::vector<fs::path> objectPaths = BuildObjectAssetPaths(asset->id);

    for (const fs::path& path : objectPaths) {
        // 存在しない種類別ファイルは、配置の検査から飛ばします。
        if (!fs::exists(path)) {
            continue;
        }

        const json data = ReadJsonObject(path);

        // 3D配置の配列を取得できないファイルからは、必須の役者や名前を集計しません。
        if (!data.contains("objects") || !data["objects"].is_array()) {
            continue;
        }

        for (const json& object : data["objects"]) {
            // 配列中でオブジェクト型の配置だけを分類と重複の判定に使います。
            if (!object.is_object()) {
                continue;
            }

            const std::string name = object.value("name", std::string());
            const std::string category = object.value("saveCategory", std::string());
            hasPlayer = hasPlayer || category == "Player" || name == "player";
            hasCamera = hasCamera || category == "Camera" || object.contains("camera");
            hasGoal = hasGoal || name == "goal" || name == "Goal";

            // 空でない名前が既に配置一覧に出ていたら、別ファイルかどうかを問わず重複に加えます。
            if (!name.empty() && !objectNames.insert(name).second) {
                duplicateNames.insert(name);
            }
        }
    }

    // GAMEPLAY向けの必須配置と進行定義を確認し、汎用編集Sceneへゲーム固有の条件を課さない。
    if (asset->runtimeScene == "GAMEPLAY") {
        // Player候補が0件の場合は、操作キャラクターの配置不足をエラーにします。
        if (!hasPlayer) {
            result.errors.push_back("GAMEPLAYにはPlayerが1体必要です。");
        }

        // カメラオブジェクトを検出できない場合は、固定カメラなどで補う必要を警告します。
        if (!hasCamera) {
            result.warnings.push_back("Camera Objectがありません。固定Camera設定だけで成立するか確認してください。");
        }

        // ゴール名の配置が無ければ、代わりのクリア条件を確認する警告を出します。
        if (!hasGoal) {
            result.warnings.push_back("goal Objectがありません。クリア条件が別実装か確認してください。");
        }

        // 同じ配置をGAMEPLAYとして推定できなければ、ステージ定義への登録を案内します。
        if (InferLegacyRuntimeScene(asset->id) != "GAMEPLAY") {
            result.warnings.push_back(
                "stages.jsonに同じStage IDがありません。解放進行へ使用する場合はStage定義へ登録してください。");
        }
    }

    // 名前の重複は実行クラスにかかわらず警告へ追加し、必須条件のエラーと区別する。
    for (const std::string& duplicateName : duplicateNames) {
        result.warnings.push_back("Object名が重複しています: " + duplicateName);
    }

    return result;
}

std::string SceneSerializer::ResolveSceneAssetObjectPath(const std::string& filename) const {
    // Scene IDに揃えたObject JSONの規約パスを返します。
    const std::string sceneId = GetSceneBaseName(filename);
    return (fs::path(kSceneObjectDirectory) / (sceneId + ".json")).generic_string();
}

std::string SceneSerializer::ResolveSceneAssetSpritePath(const std::string& filename) const {
    // 旧形式の配置も探す共通解決経路へ渡します。
    return ResolveExistingSpritePath(GetSceneBaseName(filename));
}
