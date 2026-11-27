#include <cstdint>
#include "EditorPropertyRegistry.h"
#include "game/system/scene/LevelLoader.h"
#include "engine/graphics/3d/material/MaterialRenderPolicy.h"
#include "engine/graphics/3d/material/WaterParamJsonCodec.h"
#include "game/system/scene/SceneEntityParameterCodec.h"
#include "BaseScene.h"
#include "game/system/scene/SceneGameplayAccess.h"
#include "engine/system/scene/LoadingProfiler.h"
#include "json.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <utility>
#include <algorithm>
#include <limits>
#include <vector>
#include <Windows.h> // OutputDebugStringA用

// 生成するクラス群のインクルード
#include "Object3D.h"
#include "CollisionConfig.h"
#include "Sprite.h"
#include "Player.h"
#include "game/actor/enemy/spawn/EnemyFactory.h"
#include "game/system/rules/GameplayStatusManager.h"
#include "game/actor/enemy/spawn/EnemySpawner.h"
#include "BaseEnemy.h"
#include "game/system/scene/GimmickFactory.h"
#include "BaseGimmick.h"
#include "game/system/scene/ItemFactory.h"
#include "MoveStrategy3D.h"
#include "GhostRecorder.h"
// マネージャ系
#include "ModelManager.h"
#include "CollisionManager.h"
#include "InputManager.h"
#include "game/ui/SpriteLayoutScaler.h"

using json = nlohmann::json;

namespace {
constexpr std::int32_t kSlimeSoftMaterialType = 25;
constexpr std::int32_t kPrismCrystalMaterialType = 27;

// 全配置物を生成した後で敵へシーン参照を結び、読み込み順に依存する参照を解決します。
void ConfigureEnemyRuntimeReferencesInternal(BaseScene* scene) {
    // シーンが渡されていなければ、結び付ける参照が無いため終了します。
    if (!scene) {
        return;
    }

    auto& objects = scene->GetObjects();

    for (auto& object : objects) {
        auto* enemy = dynamic_cast<BaseEnemy*>(object.get());

        // 敵以外の配置物には敵用のスポーン参照を設定しません。
        if (!enemy) {
            continue;
        }

        enemy->BindSceneSpawnContext(scene);
    }
}

bool IsSlimeEnemyType(const std::string& enemyType) {
    // スライム用の軟質材質を適用する種類だけを列挙する。
    return enemyType == "Slime" || enemyType == "Bomber" || enemyType == "FireSlime" || enemyType == "ThunderSlime" ||
           enemyType == "WindSlime" || enemyType == "GiantSlime" || enemyType == "PrismSlime" ||
           enemyType == "MagmaSlime";
}

bool IsSlimeModelName(std::string modelName) {
    // JSON の区切り文字差を吸収して、既存モデル名の分類を安定させる。
    std::replace(modelName.begin(), modelName.end(), '\\', '/');
    return modelName.find("Characters/slime") != std::string::npos;
}

void ApplySlimeSoftMaterialIfStandard(engine::Object3D* object) {
    // エディターで明示した材質は尊重し、標準材質の時だけ柔らかい見た目へ変える。
    if (object && object->GetMaterialType() == 0) {
        object->SetMaterialType(kSlimeSoftMaterialType);
    }
}

void ApplySlimeMaterialDefault(engine::Object3D* object) {
    // 実体の生成や取得に失敗した場合は、材質を変更できません。
    if (!object) {
        return;
    }

    // プレイヤーと敵は同じ標準材質を共有しますが、敵種別ごとの例外を先に判定します。
    if (object->GetClassName() == "Player") {
        ApplySlimeSoftMaterialIfStandard(object);
        return;
    }

    const std::string enemyType = object->GetEnemyType();

    // 対象のスライム種以外にスライム用の材質を適用しません。
    if (!IsSlimeEnemyType(enemyType)) {
        return;
    }

    // マグマスライムはモデル内の溶岩・黒曜石PBR材質を維持します。
    if (enemyType == "MagmaSlime") {
        return;
    }

    // プリズム種で標準材質か軟質材質のときは、専用の結晶材質を使います。
    if (enemyType == "PrismSlime" &&
        (object->GetMaterialType() == 0 || object->GetMaterialType() == kSlimeSoftMaterialType)) {
        object->SetMaterialType(kPrismCrystalMaterialType);
        return;
    }

    ApplySlimeSoftMaterialIfStandard(object);
}

// 先頭3成分が数値の配列を読みます。形式が異なる場合は出力を維持し false を返します。
bool TryReadVector3(const json& value, float& x, float& y, float& z) {
    // 形式が異なる旧データでは出力引数を変更せず、呼び出し側へ失敗を返す。
    if (!value.is_array() || value.size() < 3 || !value[0].is_number() || !value[1].is_number() ||
        !value[2].is_number()) {
        return false;
    }

    x = value[0].get<float>();
    y = value[1].get<float>();
    z = value[2].get<float>();
    return true;
}

// Blenderの軸順からゲーム用の軸順へ変換し、必要な場合は符号を反転します。
json ConvertBlenderVector(const json& value, bool negate) {
    // Blender 側の軸順をゲーム側へ揃え、回転だけ符号を反転する。
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    // 座標を読み取れなければ、後段へ元データを渡します。
    if (!TryReadVector3(value, x, y, z)) {
        return value;
    }

    // 回転の変換では軸を入れ替えた上で各成分の符号を反転します。
    if (negate) {
        return json::array({-x, -z, -y});
    }

    return json::array({x, z, y});
}

json NormalizeLevelObject(const json& source, const std::string& inheritedParentName) {
    // 旧形式の Transform と親子名を現行の配置 JSON へ移し、元の未知項目は残す。
    json normalized = source;
    const bool hasLegacyTransform = source.contains("transform") && source["transform"].is_object();

    // 旧 transform がオブジェクトなら旧キーを調べ、現行の位置・回転・拡縮のうち不足分を補います。
    if (hasLegacyTransform) {
        const json& transform = source["transform"];

        if (!normalized.contains("position") && transform.contains("translation")) {
            normalized["position"] = ConvertBlenderVector(transform["translation"], false);
        }

        if (!normalized.contains("rotation") && transform.contains("rotation")) {
            normalized["rotation"] = ConvertBlenderVector(transform["rotation"], true);
        }

        if (!normalized.contains("scale") && transform.contains("scaling")) {
            normalized["scale"] = ConvertBlenderVector(transform["scaling"], false);
        }

        // 旧形式の Collider 設定があれば、中心・回転・大きさのうち存在する値に軸変換を適用します。
        if (normalized.contains("collider") && normalized["collider"].is_object()) {
            auto& collider = normalized["collider"];

            if (collider.contains("center")) {
                collider["center"] = ConvertBlenderVector(collider["center"], false);
            }

            if (collider.contains("rotation")) {
                collider["rotation"] = ConvertBlenderVector(collider["rotation"], true);
            }

            if (collider.contains("size")) {
                collider["size"] = ConvertBlenderVector(collider["size"], false);
            }
        }
    }

    // 現行キーが指定済みなら尊重し、旧モデル名は不足時だけ移します。
    if (!normalized.contains("modelName") && source.contains("file_name") && source["file_name"].is_string()) {
        normalized["modelName"] = source["file_name"];
    }

    const bool isPlayerSpawn = source.value("spawn", false);
    const bool isEnemySpawn = source.value("enemy", false);
    const std::string sourceType = source.value("type", "Model");

    // プレイヤー開始地点の指定を最優先し、型と保存カテゴリを設定します。
    if (isPlayerSpawn) {
        normalized["type"] = "Player";
        normalized["saveCategory"] = "Player";
    // プレイヤー開始地点でなく敵の開始地点なら、敵の型・保存先・種別を設定します。
    } else if (isEnemySpawn) {
        normalized["type"] = "Enemy";
        normalized["saveCategory"] = "Enemy";
        std::string enemyType = source.value("enemyType", "");

        // 現行の種別が無ければ旧キーを調べ、種別を得られなければ Slime にします。
        if (enemyType.empty()) {
            enemyType = source.value("enemy_type", "Slime");
        }

        normalized["enemyType"] = enemyType.empty() ? "Slime" : enemyType;
    // スポーン指定が無い Blender のオブジェクト型は、通常のモデル配置へ変換します。
    } else if (sourceType == "MESH" || sourceType == "EMPTY" || sourceType == "CURVE" || sourceType == "SURFACE" ||
               sourceType == "META" || sourceType == "FONT") {
        normalized["type"] = "Model";

        // 保存カテゴリが未指定なら、通常オブジェクトとして分類します。
        if (!normalized.contains("saveCategory")) {
            normalized["saveCategory"] = "Object";
        }
    }

    // 親名を引き継いだ子の参照が欠落・空欄・文字列以外なら、階層から得た親名を補います。
    if (!inheritedParentName.empty() && (!normalized.contains("parentName") || !normalized["parentName"].is_string() ||
                                         normalized["parentName"].get<std::string>().empty())) {
        normalized["parentName"] = inheritedParentName;
    }

    // 旧形式の変換に消費したキーを除き、それ以外の保存項目を持ち越します。
    normalized.erase("transform");
    normalized.erase("children");
    normalized.erase("file_name");
    normalized.erase("spawn");
    normalized.erase("enemy");
    normalized.erase("enemy_type");
    return normalized;
}

// 旧形式の親子配置を現行形式へ変換し、destinationへ平坦に追加します。
void AppendNormalizedLevelObjects(const json& sourceObjects, json& destination,
                                  const std::string& inheritedParentName = "") {
    // 入力が配列でなければ、階層を辿る対象がありません。
    if (!sourceObjects.is_array()) {
        return;
    }

    for (const auto& source : sourceObjects) {
        // 配置物のオブジェクトとして解釈できない配列要素を飛ばします。
        if (!source.is_object()) {
            continue;
        }

        // 親を先に一覧へ入れ、子は親名を引き継いで同じ平坦な一覧に変換する。
        json normalized = NormalizeLevelObject(source, inheritedParentName);
        const std::string childParentName = normalized.value("name", inheritedParentName);
        destination.push_back(std::move(normalized));

        // 元データの子配列も辿り、後続の生成側が階層形式を個別に扱わずに済むようにする。
        if (source.contains("children")) {
            AppendNormalizedLevelObjects(source["children"], destination, childParentName);
        }
    }
}

// 旧形式の配置項目がある場合だけ変換が必要と判定します。
bool RequiresLevelObjectNormalization(const json& sourceObjects) {
    // 配列でなければ、変換が必要な配置物を検出できません。
    if (!sourceObjects.is_array()) {
        return false;
    }

    for (const auto& source : sourceObjects) {
        // オブジェクト以外の要素は旧形式の判定から除きます。
        if (!source.is_object()) {
            continue;
        }

        // 旧キーが一つでも見つかれば、変換が必要です。
        if (source.contains("transform") || source.contains("file_name") || source.contains("spawn") ||
            source.contains("enemy")) {
            return true;
        }

        const std::string sourceType = source.value("type", "Model");
        // Blender の型名が残っていれば、現行のオブジェクト型への変換が必要です。
        if (sourceType == "MESH" || sourceType == "EMPTY" || sourceType == "CURVE" || sourceType == "SURFACE" ||
            sourceType == "META" || sourceType == "FONT") {
            return true;
        }

        // 子を持つ階層形式なら、配置の一覧を平坦化する必要があります。
        if (source.contains("children") && source["children"].is_array() && !source["children"].empty()) {
            return true;
        }
    }

    return false;
}
} // namespace

// シーンJSONから配置物を生成し、参照を接続します。生成と紐付けを分け、読み込み順に依存する敵・ギミックを扱います。

void LevelLoader::ConfigureEnemyRuntimeReferences(BaseScene* scene) {
    // 読み込み側とシーン再構築側から同じ参照解決処理を使う。
    ConfigureEnemyRuntimeReferencesInternal(scene);
}

// 分割読込APIを完了まで進める同期入口です。全Objectと参照の準備が終わるまで戻りません。
void LevelLoader::LoadObjectLayout(BaseScene* scene, const std::string& filename, bool resolveSceneAssetPath) {
    BeginObjectLayoutLoad(scene, filename, resolveSceneAssetPath);

    // 同期の呼び出しでは件数上限を設けず、準備の各段階を同じ呼び出し中に終わらせます。
    while (!ContinueObjectLayoutLoad(scene, (std::numeric_limits<size_t>::max)())) {
    }
}

// SceneAssetの参照を解決し、分割JSONの存在を調べて段階読込の対象一覧を作ります。
// 親子リンクは全ファイルのObject生成後まで保留します。
void LevelLoader::BeginObjectLayoutLoad(BaseScene* scene, const std::string& filename, bool resolveSceneAssetPath) {
    // Scene Assetから読むときは登録済みの配置パスを優先し、直接指定の読込では受け取ったパスを使う。
    const std::string resolvedFilename =
        scene && resolveSceneAssetPath ? scene->ResolvePrimaryObjectLayoutPath(filename) : filename;
    std::string justName = resolvedFilename;
    size_t slashPos = justName.find_last_of("/\\");

    // パスから区切り文字の後ろを取り出し、レイアウト名の表示に使います。
    if (slashPos != std::string::npos) {
        justName = justName.substr(slashPos + 1);
    }

    // 通常のシーン読込だけは、エディターなどが参照する読込名を更新する。
    if (scene && resolveSceneAssetPath) {
        scene->SetLoadedFilename(justName);
    }

    std::string baseFilename = resolvedFilename;

    size_t extPos = baseFilename.find(".json");

    // パスに .json が含まれていれば、それより前を分割ファイル名の基準にします。
    if (extPos != std::string::npos) {
        baseFilename = baseFilename.substr(0, extPos);
    }

    // 親子リンクは全ファイルの生成が終わるまで保留します。
    parentPendingList_.clear();
    pendingObjectFiles_.clear();
    pendingFileIndex_ = 0;
    pendingObjectIndex_ = 0;
    pendingObjects_ = json::array();
    pendingFileLoaded_ = false;
    objectLayoutLoading_ = true;
    preparationStep_ = 0;

    // 分割形式のファイル名を同じベース名から導出します。
    std::string playerFile = baseFilename + "_player.json";
    std::string enemyFile = baseFilename + "_enemy.json";
    std::string objectFile = baseFilename + "_object.json";
    std::string cameraFile = baseFilename + "_camera.json";

    // どれか一つでも存在すれば分割形式を優先します。
    bool useSplitFiles = false;
    std::ifstream pFile(playerFile);
    std::ifstream eFile(enemyFile);
    std::ifstream oFile(objectFile);
    std::ifstream cFile(cameraFile);

    // 4種類の候補のうち一つでも開ければ分割形式として扱います。
    if (pFile.is_open() || eFile.is_open() || oFile.is_open() || cFile.is_open()) {
        useSplitFiles = true;
    }

    // 存在確認に開けたストリームだけを閉じます。配置の解析は LoadSingleJson() が担当します。
    if (pFile.is_open())
        pFile.close();
    if (eFile.is_open())
        eFile.close();
    if (oFile.is_open())
        oFile.close();
    if (cFile.is_open())
        cFile.close();

    // 新しい分割形式を優先し、旧形式の単一JSONも読み込み可能にします。
    if (useSplitFiles) {
        pendingObjectFiles_ = {playerFile, enemyFile, objectFile, cameraFile};
    // 分割候補を一つも開けなければ、指定の単一ファイルを読み込みます。
    } else {
        pendingObjectFiles_ = {resolvedFilename};
    }
}

bool LevelLoader::ContinueObjectLayoutLoad(BaseScene* scene, size_t maxObjects) {
    // 読み込み済み、または開始していない場合は呼び元へ完了を返す。
    if (!objectLayoutLoading_) {
        return true;
    }

    // 配置物生成前の管理データも別フレームで読み、JSON解析と重ならないようにする。
    if (preparationStep_ == 0) {
        const auto start = std::chrono::steady_clock::now();
        runtimePresets_.Load();
        LogSlowLoadingOperation("LevelLoader preset initialization",
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        ++preparationStep_;
        return false;
    }

    // プリセットに続いて、生成したキャラクターへ適用するステータス定義を用意する。
    if (preparationStep_ == 1) {
        const auto start = std::chrono::steady_clock::now();
        GameplayStatusManager::GetInstance()->Initialize();
        LogSlowLoadingOperation("LevelLoader status initialization",
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        ++preparationStep_;
        return false;
    }

    // 少なくとも一件は進める。残り件数をファイル間で共有し、分割ファイルも一フレームに集中させない。
    size_t remainingObjects = (std::max)(size_t{1}, maxObjects);

    while (pendingFileIndex_ < pendingObjectFiles_.size() && remainingObjects > 0) {
        // ファイル内の途中まで生成した場合は、同じファイルの次の項目を次回に再開する。
        if (!LoadSingleJson(scene, pendingObjectFiles_[pendingFileIndex_], remainingObjects)) {
            return false;
        }

        ++pendingFileIndex_;
    }

    // 今回の生成枠を使い切ってファイルが残った場合は、親子関係の復元をまだ始めない。
    if (pendingFileIndex_ < pendingObjectFiles_.size()) {
        return false;
    }

    // 全配置物のGUIDを確定してから、保留していた親子関係を解決します。
    auto& objects = scene->GetObjects();
    scene->EnsureUniquePersistentObjectGuids();

    for (auto const& [childObj, parentReference] : parentPendingList_) {
        engine::Object3D* parentObj = scene->FindObjectByPersistentGuid(parentReference.guid);

        // GUIDで親を特定できず保存された名前がある場合は、旧形式の名前で探します。
        if (!parentObj && !parentReference.legacyName.empty()) {
            for (auto& obj : objects) {
                // 実体があり旧形式の名前が一致した最初の配置物を親候補にします。
                if (obj && obj->GetName() == parentReference.legacyName) {
                    parentObj = obj.get();
                    break;
                }
            }
        }

        // 親が特定できた子だけ親子リンクを設定します。
        if (parentObj) {
            childObj->SetParent(parentObj);
        }
    }

    // 全オブジェクトと親子関係が揃った後に、敵が使うシーン内の参照を設定する。
    ConfigureEnemyRuntimeReferences(scene);
    objectLayoutLoading_ = false;
    pendingObjectFiles_.clear();
    return true;
}

float LevelLoader::GetObjectLayoutLoadProgress() const {
    // 完了ファイル数に現在ファイル内の生成済み割合を加えて全体進捗へ換算する。
    if (!objectLayoutLoading_ || pendingObjectFiles_.empty()) {
        return 1.0f;
    }

    const float fileProgress = pendingFileLoaded_ && !pendingObjects_.empty()
        ? static_cast<float>(pendingObjectIndex_) / static_cast<float>(pendingObjects_.size())
        : 0.0f;
    return (static_cast<float>(pendingFileIndex_) + fileProgress) /
           static_cast<float>(pendingObjectFiles_.size());
}

// 1ファイル分を割り当て件数内で進めます。未処理の要素が残る場合は false を返して次回に引き継ぎます。
bool LevelLoader::LoadSingleJson(BaseScene* scene, const std::string& filename, size_t& remainingObjects) {
    auto& objects = scene->GetObjects();
    engine::Object3DCommon* object3dCommon = scene->GetObject3DCommon();

    try {
        // 再開時は解析済みの配列を使う。新しいファイルの先頭だけJSONを展開する。
        if (!pendingFileLoaded_) {
            json sceneData;

            // 非同期で解析済みの内容を優先し、準備されていないファイルだけここで読み込む。
            if (!scene->TakePreparedJson(filename, sceneData)) {
                std::ifstream file(filename);

                // 分割配置では一部のファイルがないことがあるため、このファイルを完了扱いにして次へ進む。
                if (!file.is_open()) {
                    return true;
                }

                sceneData = json::parse(file);
            }

            pendingObjects_ = json::array();

            // 配置の配列が見つかった場合だけ生成対象を用意します。
            if (sceneData.contains("objects") && sceneData["objects"].is_array()) {
                // 階層などの正規化が必要な形式だけ変換し、平坦な配列はそのまま引き継ぐ。
                if (RequiresLevelObjectNormalization(sceneData["objects"])) {
                    AppendNormalizedLevelObjects(sceneData["objects"], pendingObjects_);
                // 現行の平坦な形式なら、座標を変換せず読み込んだ配列を引き継ぎます。
                } else {
                    pendingObjects_ = std::move(sceneData["objects"]);
                }
            }

            pendingFileLoaded_ = true;
            pendingObjectIndex_ = 0;
        }

        // 配置の配列を扱える場合は、現在の索引から件数枠まで処理します。
        if (pendingObjects_.is_array()) {
            while (pendingObjectIndex_ < pendingObjects_.size() && remainingObjects > 0) {
                // 無効な項目も1件分として消費し、次のフレームで同じ項目に戻らないようにする。
                const auto& objData = pendingObjects_[pendingObjectIndex_++];
                --remainingObjects;
                // 名前が無いか文字列でなければ、配置対象を特定できないため飛ばします。
                if (!objData.contains("name") || !objData["name"].is_string())
                    continue;
                std::string name = objData["name"].get<std::string>();

                auto* targetObject = FindOrCreateLayoutObject(scene, object3dCommon, objects, objData, name);
                // 既存実体も新規実体も取得できなければ、設定や親子リンクの処理に進みません。
                if (!targetObject) {
                    continue;
                }

                ApplyLayoutSettings(targetObject, objData);
                QueueLayoutParent(targetObject, objData);
            }
        }

        // 今回の生成枠を使い切った場合は解析済み配列と索引を残し、次回はその続きから再開します。
        if (pendingObjectIndex_ < pendingObjects_.size()) {
            return false;
        }

        // ファイルを処理し終えたら、次のファイル用に配列と索引を初期化します。
        pendingObjects_ = json::array();
        pendingObjectIndex_ = 0;
        pendingFileLoaded_ = false;
    } catch (const json::parse_error& e) {
        // 破損したJSONは診断ログへ出し、読み込み状態を初期化して後続のファイルへ進みます。
        std::string message = "Failed to parse " + filename + " : " + e.what() + "\n";
        OutputDebugStringA(message.c_str());
        pendingObjects_ = json::array();
        pendingObjectIndex_ = 0;
        pendingFileLoaded_ = false;
    }

    return true;
}

engine::Object3D* LevelLoader::FindOrCreateLayoutObject(
    BaseScene* scene, engine::Object3DCommon* object3dCommon, const std::vector<std::unique_ptr<engine::Object3D>>& objects,
    const json& objData, const std::string& name) {
    // 同名の既存オブジェクトがあれば、シーン固有の実体を残して配置値だけ更新します。
    engine::Object3D* targetObject = nullptr;

    for (auto& obj : objects) {
        // 空名でない同名の実体が見つかれば、その実体の設定を更新します。
        if (obj && !obj->GetName().empty() && obj->GetName() == name) {
            targetObject = obj.get();
            break;
        }
    }

    // 既存の実体が見つからず3D描画の共通設定が利用できる場合だけ、新しく生成します。
    if (!targetObject && object3dCommon) {
        std::unique_ptr<engine::Object3D> newObj;

        // 種別の各キーは文字列で保存された場合だけ採用し、省略時は空文字または通常モデルを使います。
        std::string enemyType = "";

        if (objData.contains("enemyType") && objData["enemyType"].is_string()) {
            enemyType = objData["enemyType"].get<std::string>();
        }

        std::string type = "Model";

        if (objData.contains("type") && objData["type"].is_string()) {
            type = objData["type"].get<std::string>();
        }

        std::string gimmickType = "";

        if (objData.contains("gimmickType") && objData["gimmickType"].is_string()) {
            gimmickType = objData["gimmickType"].get<std::string>();
        }

        std::string itemType = "";

        if (objData.contains("itemType") && objData["itemType"].is_string()) {
            itemType = objData["itemType"].get<std::string>();
        }


        // 敵種の指定があれば、ほかの型より優先して EnemyFactory に生成を依頼します。
        if (!enemyType.empty()) {
            auto enemy = EnemyFactory::GetInstance()->CreateEnemy(enemyType, object3dCommon);

            // Factory が生成できた敵には種別とシーン参照を設定します。
            if (enemy) {
                enemy->SetEnemyType(enemyType);

                enemy->BindSceneSpawnContext(scene);
                newObj = std::move(enemy);
            }
        }

        // 敵種の指定がなくプレイヤー型または Player という名前なら、操作を持つプレイヤーを初期化します。
        else if (type == "Player" || name == "Player") {
            auto player = std::make_unique<Player>();
            player->Initialize(object3dCommon, engine::InputManager::GetInstance(), scene->GetParticleSystem(),
                               scene->GetSpriteCommon());
            player->SetMoveStrategy(std::make_unique<MoveStrategy3D>());

            // シーンにプレイヤーを登録
            SetScenePlayer(scene, player.get());

            newObj = std::move(player);
        }

        // Spawner 型には敵種・秒単位の生成間隔・最大数を持つ生成地点を用意します。
        else if (type == "Spawner") {
            auto spawner = std::make_unique<EnemySpawner>();
            std::string spawnEnemyType = "Goblin";
            float interval = 3.0f;
            std::int32_t maxCount = 5;

            // param がオブジェクトなら、文字列の敵種と数値の間隔・最大数で各既定値を上書きします。
            if (objData.contains("param") && objData["param"].is_object()) {
                auto& p = objData["param"];
                if (p.contains("enemyType") && p["enemyType"].is_string())
                    spawnEnemyType = p["enemyType"];
                if (p.contains("interval") && p["interval"].is_number())
                    interval = p["interval"];
                if (p.contains("maxCount") && p["maxCount"].is_number())
                    maxCount = p["maxCount"];
            }

            spawner->Initialize(object3dCommon, spawnEnemyType, interval, maxCount);
            spawner->SetClassName("Spawner");
            spawner->SetModel(nullptr);
            spawner->SetIsVisible(false);

            // 後から出現する敵も通常読込と同じ生成クラスを使い、発生地点とシーン参照を付けて追加します。
            spawner->SetOnSpawnCallback([scene, spawnEnemyType](const engine::Vector3& spawnPos) {
                auto newEnemy =
                    EnemyFactory::GetInstance()->CreateEnemy(spawnEnemyType, scene->GetObject3DCommon());
                // Factory が敵を生成できた場合は、その場で行列と参照を揃えてシーンへ登録します。
                if (newEnemy) {
                    newEnemy->SetTranslate(spawnPos);
                    newEnemy->UpdateLocalMatrix();
                    newEnemy->UpdateWorldMatrix();
                    newEnemy->SetEnemyType(spawnEnemyType);
                    newEnemy->BindSceneSpawnContext(scene);
                    scene->AddObjectImmediate(std::move(newEnemy));
                }
            });
            newObj = std::move(spawner);
        }

        // アイテム種が指定済みか Item 型なら、ItemFactory で生成します。
        else if (!itemType.empty() || type == "Item") {
            auto item = ItemFactory::GetInstance()->CreateItem(itemType, object3dCommon);

            // 生成されたアイテムに、配置から読み取った種別を記録します。
            if (item) {
                item->SetItemType(itemType);
                newObj = std::move(item);
            }
        }

        // ギミック種が指定済みか Gimmick 型なら、既定種を補って GimmickFactory で生成します。
        else if (!gimmickType.empty() || type == "Gimmick") {
            // type == "Gimmick" だが gimmickType が空の場合はデフォルト名で作成
            std::string gType = gimmickType.empty() ? "DefaultGimmick" : gimmickType;
            auto gimmick = GimmickFactory::GetInstance()->CreateGimmick(gType, object3dCommon);

            // 生成できたギミックに種別と対応するプリセットを反映します。
            if (gimmick) {
                gimmick->SetGimmickType(gType);

                // プリセットによるパラメータ上書き
                runtimePresets_.ApplyToObject(gType, gimmick.get());

                newObj = std::move(gimmick);
            }
        }

        // 専用の生成分岐に一致しなければ、通常の Object3D を初期化します。
        else {
            newObj = std::make_unique<engine::Object3D>();
            newObj->Initialize(object3dCommon);
        }

        // 生成できた実体だけに名前と保存カテゴリを設定してシーンへ渡します。
        if (newObj) {
            newObj->SetName(name);
            std::string currentClass = newObj->GetClassName();
            // 専用のゲームクラスが持つクラス名は保持し、その他は保存された型名を使います。
            if (currentClass != "Enemy" && currentClass != "Player" && currentClass != "Spawner" &&
                currentClass != "Gimmick" && currentClass != "Item") {
                newObj->SetClassName(type);
            }

            // カメラ系の2種類は、保存上は同じ Camera カテゴリに統一します。
            if (type == "Camera" || type == "CinematicCamera") {
                newObj->SetClassName("Camera");
                newObj->SetSaveCategory("Camera");
            // 生成クラスか保存形式がプレイヤーであれば、Player カテゴリを優先します。
            } else if (currentClass == "Player" || type == "Player") {
                newObj->SetSaveCategory("Player");
            // 専用カテゴリに該当せず文字列のカテゴリ指定があれば、その指定を使います。
            } else if (objData.contains("saveCategory") && objData["saveCategory"].is_string()) {
                newObj->SetSaveCategory(objData["saveCategory"].get<std::string>());
            // 分類の指定がなければ、旧配置を生成したクラスや型に従って仕分けます。
            } else {
                // 敵とスポーナーは、カテゴリ指定の無い旧配置では Enemy に分類します。
                if (currentClass == "Enemy" || currentClass == "Spawner" || type == "Spawner") {
                    newObj->SetSaveCategory("Enemy");
                // 残りの旧配置は通常オブジェクトに分類します。
                } else {
                    newObj->SetSaveCategory("Object");
                }
            }


            targetObject = newObj.get();

            // 生成したら即座にCollisionManagerとシーンリストへ登録
            scene->AddObjectImmediate(std::move(newObj));
        }
    }
    return targetObject;
}

namespace {

// カテゴリID配列を優先し、未知ビットを保持する。旧形式しかなければ現在のカテゴリへ変換する。
uint32_t ReadLayoutCollisionMask(const json& objData, const char* idsKey,
                                 const char* unknownKey, const char* legacyKey, uint32_t fallback) {
    // カテゴリIDの配列があれば新形式を優先し、保存された未知ビットを合成します。
    if (objData.contains(idsKey) && objData[idsKey].is_array()) {
        std::vector<std::string> ids;

        for (const auto& id : objData[idsKey]) {
            // カテゴリIDとして扱える文字列だけを変換対象にします。
            if (id.is_string())
                ids.push_back(id.get<std::string>());
        }

        const uint32_t unknown = objData.value(unknownKey, uint32_t{0});
        return engine::collisionCategories::FromIds(ids) | unknown;
    }

    // 新形式が無く旧マスクが符号なし整数なら、旧ビットを現行カテゴリに変換します。
    if (objData.contains(legacyKey) && objData[legacyKey].is_number_unsigned()) {
        return engine::collisionCategories::FromLegacyMask(objData[legacyKey].get<uint32_t>());
    }

    // 符号付きの旧整数もビット列として解釈して変換します。
    if (objData.contains(legacyKey) && objData[legacyKey].is_number_integer()) {
        return engine::collisionCategories::FromLegacyMask(static_cast<uint32_t>(objData[legacyKey].get<std::int32_t>()));
    }

    return fallback;
}

void ApplyLayoutCollisionSettings(engine::Object3D& object, const json& objData) {
    // カメラ用の当たり判定は配置の衝突設定で上書きしない。
    if (object.IsCameraObject()) {
        return;
    }

    // 形状を復元してからカテゴリを適用する。設定がなければ生成時の値を引き継ぐ。
    if (objData.contains("collider")) {
        json colData = objData["collider"];
        // 既存の Collider 設定を基準に、保存された型・中心・大きさ・回転だけを上書きします。
        engine::Object3D::ColliderConfig config = object.GetColliderConfig();
        if (colData.contains("type"))
            config.type = (engine::ColliderType)colData["type"].get<std::int32_t>();
        if (colData.contains("center")) {
            config.center.x = colData["center"][0];
            config.center.y = colData["center"][1];
            config.center.z = colData["center"][2];
        }

        if (colData.contains("size")) {
            config.size.x = colData["size"][0];
            config.size.y = colData["size"][1];
            config.size.z = colData["size"][2];
        }

        if (colData.contains("rotation")) {
            config.rotation.x = colData["rotation"][0];
            config.rotation.y = colData["rotation"][1];
            config.rotation.z = colData["rotation"][2];
        }

        config.useModelSurface = colData.value("useModelSurface", false);
        config.useModelVolume = colData.value("useModelVolume", false);
        config.footprint.clear();

        // 頂点の配列があれば、平面の輪郭を保存された順に再構成します。
        if (colData.contains("footprint") && colData["footprint"].is_array()) {
            for (const auto& point : colData["footprint"]) {
                // 頂点は2要素の数値配列だけを採用します。
                if (point.is_array() && point.size() == 2 &&
                    point[0].is_number() && point[1].is_number()) {
                    config.footprint.push_back({ point[0].get<float>(), point[1].get<float>() });
                }
            }
        }

        object.SetColliderConfig(config);
    }

    object.SetCollisionAttribute(ReadLayoutCollisionMask(
        objData, "collisionAttributeIds", "collisionAttributeUnknownBits",
        "collisionAttribute", object.GetCollisionAttribute()));
    object.SetCollisionMask(ReadLayoutCollisionMask(
        objData, "collisionMaskIds", "collisionMaskUnknownBits", "collisionMask", object.GetCollisionMask()));

    // 地形データを読み込む前に静的設定を反映する。
    if (objData.contains("isStatic"))
        object.SetStatic(objData["isStatic"].get<bool>());
    // 地形判定のファイルパスが文字列で指定されている場合にデータを読みます。
    if (objData.contains("terrainCollisionPath") &&
        objData["terrainCollisionPath"].is_string()) {
        object.LoadTerrainCollisionFromFile(objData["terrainCollisionPath"].get<std::string>());
    }
}

} // namespace

namespace {

// 配置に書かれた項目だけを更新し、回転の基準は従来どおりクォータニオンを優先する。
Transform ReadLayoutTransform(const json& layout, Transform transform) {
    // 位置の指定がない配置では、生成時に設定されたローカル座標を保持する。
    if (layout.contains("position")) {
        const auto& position = layout["position"];
        transform.translate = {position[0], position[1], position[2]};
    }

    const bool hasQuaternion = layout.contains("quaternion");
    // クォータニオンがある場合は、読み込み直後の行列をその姿勢から計算する。
    if (hasQuaternion) {
        const auto& rotation = layout["quaternion"];
        transform.quaternion = {rotation[0], rotation[1], rotation[2], rotation[3]};
        transform.isQuaternionMaster = true;
    }

    // オイラー角はインスペクターにも使う。旧形式でこの項目だけなら角度を行列計算の基準にする。
    if (layout.contains("rotation")) {
        const auto& rotation = layout["rotation"];
        transform.rotate = {rotation[0], rotation[1], rotation[2]};
        transform.isQuaternionMaster = hasQuaternion;
    }

    // 拡縮の指定がない配置では、モデルを生成したときの大きさを保つ。
    if (layout.contains("scale")) {
        const auto& scale = layout["scale"];
        transform.scale = {scale[0], scale[1], scale[2]};
    }

    return transform;
}

} // namespace

void LevelLoader::ApplyLayoutSettings(engine::Object3D* targetObject, const json& objData) {
    // 旧データにはIDが省略されているため、保存値を採用した後で不足分を補う。
    if (objData.contains("guid") && objData["guid"].is_string()) {
        targetObject->SetPersistentGuid(objData["guid"].get<std::string>());
    }

    targetObject->EnsurePersistentGuid();
    targetObject->DeserializeFeatureComponents(objData);

    // Prefabから来たオブジェクトには、復元後の所属と元オブジェクトを辿るための情報を保持する。
    if (objData.contains("prefabInstance") && objData["prefabInstance"].is_object()) {
        const auto& prefab = objData["prefabInstance"];
        engine::Object3D::PrefabInstanceInfo info;
        info.assetId = prefab.value("assetId", "");
        info.prefabName = prefab.value("prefabName", "");
        info.instanceId = prefab.value("instanceId", "");
        info.sourceObjectId = prefab.value("sourceObjectId", "");
        info.isRoot = prefab.value("isRoot", false);
        targetObject->SetPrefabInstanceInfo(info);
    }

    // カメラは編集用モデルで場所を示し、衝突や影を持たない設定にする。
    if (targetObject->IsCameraObject()) {
        targetObject->SetClassName("Camera");
        targetObject->SetSaveCategory("Camera");
        targetObject->SetModel("Editor/camera_gizmo");
        targetObject->SetColor({0.25f, 0.75f, 1.0f, 1.0f});
        targetObject->SetCastShadow(false);
        engine::Object3D::ColliderConfig cameraCollider;
        cameraCollider.type = engine::ColliderType::kNone;
        targetObject->SetColliderConfig(cameraCollider);

        // カメラ固有の設定が保存されていれば、現行の値を基準に反映します。
        if (objData.contains("camera")) {
            SceneCameraSettings settings = targetObject->GetSceneCameraSettings();
            DeserializeSceneCameraSettings(objData["camera"], settings);
            targetObject->SetSceneCameraSettings(settings);
        }
    }

    // モデルの指定を解決し、材質値の初期設定に使うモデル名を保持します。
    std::string type = targetObject->GetClassName();
    std::string loadedModelName;

    // 判定や生成だけのオブジェクトはモデルを外し、シーン内の機能は引き続き使えるようにする。
    if (type == "InvisibleBox" || type == "Spawner") {
        targetObject->SetModel(nullptr);
        targetObject->SetIsVisible(true);
    // 表示モデルを扱う種類では、保存されたモデル指定を確認します。
    } else {
        targetObject->SetIsVisible(true);

        // モデルキーが文字列なら指定を反映します。キーが無い場合は既存モデルを保ちます。
        if (objData.contains("modelName") && objData["modelName"].is_string()) {
            std::string modelName = objData["modelName"].get<std::string>();
            loadedModelName = modelName;

            // モデル名が空でなければ ModelManager で読み込んで割り当てます。
            if (!modelName.empty()) {
                ModelManager::GetInstance()->LoadModel(modelName);
                targetObject->SetModel(modelName);
            // 空文字が明示されている場合はモデルを外します。
            } else {
                targetObject->SetModel(nullptr);
            }
        }
    }

    // JSONに指定された位置・姿勢・拡縮を適用し、未指定項目は現在の初期値を使う。
    targetObject->SetLocalTransform(ReadLayoutTransform(objData, *targetObject->GetTransform()));

    // 読み込み直後の座標を参照できるよう行列を更新し、保存されている描画モード・材質・メッシュ・色を復元します。
    targetObject->UpdateLocalMatrix();
    targetObject->UpdateWorldMatrix();
    if (objData.contains("blendMode"))
        targetObject->SetBlendMode(static_cast<BlendMode>(objData["blendMode"].get<std::int32_t>()));
    if (objData.contains("materialType"))
        targetObject->SetMaterialType(objData["materialType"].get<std::int32_t>());
    if (objData.contains("meshDrawIndex"))
        targetObject->SetMeshDrawIndex(objData["meshDrawIndex"].get<std::int32_t>());

    if (objData.contains("color")) {
        targetObject->SetColor(
            {objData["color"][0], objData["color"][1], objData["color"][2], objData["color"][3]});
    }

    // PBRの設定と画像パスは、各キーがある場合だけ現在値を上書きします。
    if (objData.contains("metallic"))
        targetObject->SetMetallic(objData["metallic"].get<float>());
    if (objData.contains("roughness"))
        targetObject->SetRoughness(objData["roughness"].get<float>());
    if (objData.contains("enableNormalMap"))
        targetObject->SetEnableNormalMap(objData["enableNormalMap"].get<bool>());
    if (objData.contains("normalMapPath"))
        targetObject->SetNormalMap(objData["normalMapPath"].get<std::string>());
    if (objData.contains("ormMapPath"))
        targetObject->SetOrmMap(objData["ormMapPath"].get<std::string>());
    if (objData.contains("texturePath"))
        targetObject->SetTexture(objData["texturePath"].get<std::string>());
    // 標準材質のスライム種かスライム用のモデルに軟質材質を割り当てます。
    if (targetObject->GetMaterialType() == 0 &&
        (IsSlimeEnemyType(targetObject->GetEnemyType()) || IsSlimeModelName(loadedModelName))) {
        targetObject->SetMaterialType(kSlimeSoftMaterialType);
    }

    // タイリングに2成分以上ある場合はUVの横方向と縦方向の倍率を読みます。
    if (objData.contains("textureTiling") && objData["textureTiling"].is_array() &&
        objData["textureTiling"].size() >= 2) {
        targetObject->SetTextureTiling(
            {objData["textureTiling"][0].get<float>(), objData["textureTiling"][1].get<float>()});
    }

    // タイリングの自動設定、照明、環境光、発光、影への参加は、各保存キーがある場合だけ更新します。
    if (objData.contains("autoTextureTiling"))
        targetObject->SetAutoTextureTiling(objData["autoTextureTiling"].get<bool>());
    if (objData.contains("enableLighting"))
        targetObject->SetEnableLighting(objData["enableLighting"].get<bool>());
    if (objData.contains("enableEnvMap"))
        targetObject->SetEnableEnvMap(objData["enableEnvMap"].get<bool>());
    if (objData.contains("envIntensity"))
        targetObject->SetEnvIntensity(objData["envIntensity"].get<float>());
    if (objData.contains("emissive"))
        targetObject->SetEmissive(objData["emissive"].get<float>());
    if (objData.contains("castShadow"))
        targetObject->SetCastShadow(objData["castShadow"].get<bool>());
    // 局所フォグの設定が保存されている場合、書き込み可能な実体を探します。
    if (objData.contains("localFog")) {
        // 局所フォグの実体がある場合だけ、指定された色・濃度・境界・ノイズ・散乱を復元します。
        if (auto* fogData = targetObject->GetLocalFogData()) {
            auto& f = objData["localFog"];

            if (f.contains("color")) {
                fogData->fogColor = {f["color"][0], f["color"][1], f["color"][2], f["color"][3]};
            }

            if (f.contains("density"))
                fogData->fogDensity = f["density"];
            if (f.contains("edgeFade"))
                fogData->edgeFade = f["edgeFade"];
            if (f.contains("noiseSpeed"))
                fogData->noiseSpeed = f["noiseSpeed"];
            if (f.contains("noiseScale"))
                fogData->noiseScale = f["noiseScale"];
            if (f.contains("scatteringG"))
                fogData->scatteringG = f["scatteringG"];
            if (f.contains("scatteringIntensity"))
                fogData->scatteringIntensity = f["scatteringIntensity"];
        }
    }

    // 水パラメーターが保存され、専用材質か登録された対応プロパティがある場合に復元対象にします。
    if (objData.contains("waterParam") &&
        (materialRenderPolicy::IsSpecial(targetObject->GetMaterialType()) ||
         EditorPropertyRegistry::GetInstance()->HasSerializedProperties(*targetObject, "waterParam."))) {
        // MeshRendererと水パラメーター領域がそろっている場合に、共有のJSON読込処理を呼びます。
        if (targetObject->GetMeshRenderer() && targetObject->GetMeshRenderer()->GetWaterParamData()) {
            auto* water = targetObject->GetMeshRenderer()->GetWaterParamData();
            waterParamJsonCodec::Read(objData["waterParam"], *water);
        }
    }

    ApplyLayoutCollisionSettings(*targetObject, objData);

    // イベントID、アイテム種、個体パラメーターを各保存項目の指定に従って設定します。
    if (objData.contains("eventID"))
        targetObject->SetEventType(static_cast<EventType>(objData["eventID"]));
    // アイテム種が文字列で保存されていれば、実体の種別を上書きします。
    if (objData.contains("itemType") && objData["itemType"].is_string()) {
        targetObject->SetItemType(objData["itemType"].get<std::string>());
    }

    // 個体パラメーターがあれば、シーン用の共通処理で反映します。
    if (objData.contains("param")) {
        sceneEntityParameterCodec::Apply(*targetObject, objData["param"]);
    }

    // 個体の配置設定を先に読み、外観の既定値と共通ステータスを反映してから行列を確定します。
    ApplySlimeMaterialDefault(targetObject);
    GameplayStatusManager::GetInstance()->ApplyManagedStatus(targetObject, true);
    targetObject->UpdateLocalMatrix();
    targetObject->UpdateWorldMatrix();


    // 旧形式で直下にクリップ名やループ指定があれば読み、続く現行形式の候補値にします。
    if (objData.contains("animName"))
        targetObject->SetAnimationName(objData["animName"]);
    if (objData.contains("isAnimLoop"))
        targetObject->SetAnimationLoop(objData["isAnimLoop"]);

    // 現行の animation があれば、指定された値だけを旧形式や現在値に優先します。
    if (objData.contains("animation")) {
        const auto& anim = objData["animation"];
        if (anim.contains("animName"))
            targetObject->SetAnimationName(anim["animName"]);
        if (anim.contains("isAnimLoop"))
            targetObject->SetAnimationLoop(anim["isAnimLoop"]);
        // コントローラーのキーが存在する場合にアニメーション制御の参照を更新します。
        if (anim.contains("animatorController")) {
            targetObject->SetAnimatorController(anim.value("animatorController", ""));
        }
    }

    // 録画再生を使えるようにし、カメラかどうかを再生モードへ渡します。
    targetObject->InitializeRecorder(nullptr);
    bool isCinematic = targetObject->IsCameraObject();

    // 記録パス名があり Recorder が利用できる場合に、ループと相対移動の指定を渡して再生を始めます。
    if (!targetObject->GetRecordPathName().empty() && targetObject->GetRecorder()) {
        targetObject->GetRecorder()->Play(targetObject->GetRecordPathName(), targetObject->IsRecordLoop(),
                                      targetObject->IsRecordRelative(), isCinematic);
    }
}

void LevelLoader::QueueLayoutParent(engine::Object3D* targetObject, const json& objData) {
    // 他のカテゴリ JSON に親がある場合も解決できるよう、全件生成まで親子付けを保留する。
    PendingParentReference parentReference;

    // 親のGUIDが文字列なら、読込後の検索キーとして保存します。
    if (objData.contains("parentGuid") && objData["parentGuid"].is_string()) {
        parentReference.guid = objData["parentGuid"].get<std::string>();
    }

    // 旧形式の親名が文字列なら、GUIDで解決できなかった場合の候補として保持します。
    if (objData.contains("parentName") && objData["parentName"].is_string()) {
        parentReference.legacyName = objData["parentName"].get<std::string>();
    }

    // GUIDまたは親名のどちらかが空でなければ、親子付けを後段に引き継ぎます。
    if (!parentReference.guid.empty() || !parentReference.legacyName.empty()) {
        parentPendingList_[targetObject] = std::move(parentReference);
    }
}

void LevelLoader::LoadSpriteLayout(BaseScene* scene, const std::string& filename) {
    // シーンごとの優先レイアウトを解決し、保存時に使うファイル名をシーンへ通知する。
    const std::string resolvedFilename = scene ? scene->ResolvePrimarySpriteLayoutPath(filename) : filename;
    std::string justName = resolvedFilename;
    size_t slashPos = justName.find_last_of("/\\");

    // パスに区切り文字があれば、シーンに通知するレイアウト名からフォルダーを除きます。
    if (slashPos != std::string::npos) {
        justName = justName.substr(slashPos + 1);
    }

    scene->SetLoadedSpriteFilename(justName);
    auto& sprites = scene->GetSprites();
    engine::SpriteCommon* spriteCommon = scene->GetSpriteCommon();

    json layoutData;
    try {
        // 先読み済みJSONを優先し、ない場合だけディスクから読む。
        if (!scene->TakePreparedJson(resolvedFilename, layoutData)) {
            std::ifstream file(resolvedFilename);

            // 設定ファイルを開けなければ、現在のスプライト配置を保持します。
            if (!file.is_open()) {
                return;
            }

            layoutData = json::parse(file);
        }

        const auto layoutScale = spriteLayoutScaler::Make(layoutData);

        // スプライトの配列を取得できた場合だけ、配置と後段の親子リンクを更新します。
        if (layoutData.contains("sprites") && layoutData["sprites"].is_array()) {
            std::vector<std::pair<engine::Sprite*, std::string>> spriteParentPending;

            for (const auto& spriteData : layoutData["sprites"]) {
                // 名前が省略された項目には更新先と生成名を決められないため飛ばします。
                if (!spriteData.contains("name"))
                    continue;
                std::string name = spriteData["name"];

                engine::Sprite* targetSprite = nullptr;

                // 同名Spriteは更新し、新規名だけ生成して既存Scene固有のSpriteを重複させない。
                for (auto& sprite : sprites) {
                    // 同じ名前の有効なスプライトを見つけたら、その実体を再利用します。
                    if (sprite && sprite->GetName() == name) {
                        targetSprite = sprite.get();
                        break;
                    }
                }

                // 同名の実体が無く SpriteCommon がある場合に、新規スプライトを生成します。
                if (!targetSprite && spriteCommon) {
                    std::string textureFile = "";
                    // 画像の指定があれば読み取り、空パスでは読み込みを行わずハンドル0を使います。
                    if (spriteData.contains("texture"))
                        textureFile = spriteData["texture"];
                    uint32_t handle = textureFile.empty() ? 0 : engine::Sprite::LoadTexture(textureFile);

                    auto newSprite = std::make_unique<engine::Sprite>();
                    newSprite->Initialize(spriteCommon, handle);
                    newSprite->SetName(name);
                    newSprite->SetTextureName(textureFile);
                    targetSprite = newSprite.get();
                    scene->AddSprite(std::move(newSprite));
                }

                // 生成または取得できたスプライトだけに保存された配置値を反映します。
                if (targetSprite) {
                    // 位置とサイズは、各項目があれば保存時の基準解像度から現在の Game View に合わせます。
                    if (spriteData.contains("position")) {
                        targetSprite->SetPosition(spriteLayoutScaler::ScalePosition(
                            spriteLayoutScaler::ReadVector2(spriteData["position"], targetSprite->GetPosition()),
                            layoutScale));
                    }

                    if (spriteData.contains("size")) {
                        targetSprite->SetSize(spriteLayoutScaler::ScaleSize(
                            spriteLayoutScaler::ReadVector2(spriteData["size"], targetSprite->GetSize()), layoutScale));
                    }

                    // アンカーが指定されていれば、保存された基準点を適用します。
                    if (spriteData.contains("anchor"))
                        targetSprite->SetAnchorPoint({spriteData["anchor"][0], spriteData["anchor"][1]});
                    // 旧レイアウトのRGBだけの指定では、元の透明度を引き継ぎます。
                    if (spriteData.contains("color")) {
                        engine::Vector4 currentColor = targetSprite->GetColor();
                        float alpha = currentColor.w;

                        // RGBAの4成分があるなら保存された透明度も使います。
                        if (spriteData["color"].is_array() && spriteData["color"].size() >= 4) {
                            alpha = spriteData["color"][3];
                        }

                        targetSprite->SetColor({spriteData["color"][0], // R
                                                spriteData["color"][1], // G
                                                spriteData["color"][2], // B
                                                alpha});
                    }

                    // 発光値が保存されていれば上書きします。
                    if (spriteData.contains("emissive")) {
                        targetSprite->SetEmissive(spriteData["emissive"].get<float>());
                    }

                    // 親が後続の項目にあっても解決できるよう名前を保留し、親指定がないものは以前のリンクを解除します。
                    if (spriteData.contains("parentName") && spriteData["parentName"].is_string()) {
                        spriteParentPending.emplace_back(targetSprite, spriteData["parentName"].get<std::string>());
                    // 親名が省略されているか文字列でない場合は、既存の親とのリンクを外します。
                    } else {
                        targetSprite->SetParent(nullptr);
                    }

                    targetSprite->Update();
                }
            }

            // 子が親より先に定義されていても結び付けられるよう、全Sprite生成後に親を解決する。
            for (const auto& [sprite, parentName] : spriteParentPending) {
                // 無効な子の参照は親子リンクの更新から除きます。
                if (!sprite)
                    continue;

                engine::Sprite* parentSprite = nullptr;

                // 保留した親名が空でないときだけ配置一覧を検索します。
                if (!parentName.empty()) {
                    for (auto& candidate : sprites) {
                        // 子自身を候補から除き、名前が一致する有効なスプライトを親に選びます。
                        if (candidate && candidate.get() != sprite && candidate->GetName() == parentName) {
                            parentSprite = candidate.get();
                            break;
                        }
                    }
                }

                // 親が見つからなければリンクを解除し、確定した階層から表示状態を再計算します。
                sprite->SetParent(parentSprite);
                sprite->Update();
            }
        }
    } catch (...) {
        // レイアウト不正でもシーン本体の読み込みを止めず、配置済みSpriteは維持する。
    }
}
