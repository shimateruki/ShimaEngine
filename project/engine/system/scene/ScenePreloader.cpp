#include <cstdint>
#include "engine/graphics/effect/VfxAssetPath.h"
#include "ScenePreloader.h"

#include "ModelManager.h"
#include "AudioPlayer.h"
#include "TextureManager.h"
#include "Sprite.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <Windows.h>

namespace {
std::string NormalizePath(std::string path) {
    // 同じAssetが区切り文字の違いで重複しないよう正規化する。
    std::replace(path.begin(), path.end(), '\\', '/');

    while (path.rfind("./", 0) == 0) {
        path.erase(0, 2);
    }

    return path;
}

std::string ToLower(std::string value) {
    // 拡張子やJSONのキーは大文字小文字を無視して分類する。
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

// 表示文をWindowsのパスへ変換せず、UTF-8のまま拡張子だけを調べる。
std::string GetPathExtension(const std::string& text) {
    const auto separator = text.find_last_of("/\\");
    const auto dot = text.find_last_of('.');
    // 最後の点がファイル名の部分にない場合は、拡張子なしとして返す。
    if (dot == std::string::npos || (separator != std::string::npos && dot < separator)) {
        return {};
    }
    return ToLower(text.substr(dot));
}

// C++20のchar8_t対応コンストラクターでUTF-8をWindowsパスへ変換する。
std::filesystem::path PathFromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

// 依存報告に使えるよう、パスをスラッシュ区切りのUTF-8文字列へ戻す。
std::string PathToUtf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return { text.begin(), text.end() };
}

template <class T, class KeySelector>
void AddUnique(std::vector<T>& values, T value, KeySelector keySelector) {
    // Assetの識別キーで重複を除き、読込と警告を一回に抑える。
    const auto key = keySelector(value);

    // 空の識別キーは読込対象として使えないため登録しない。
    if (key.empty()) {
        return;
    }

    const auto found = std::find_if(values.begin(), values.end(), [&](const T& candidate) {
        return keySelector(candidate) == key;
    });

    // 同じ識別キーがまだなければ先読みリストに登録する。
    if (found == values.end()) {
        values.push_back(std::move(value));
    }
}

void AddUniquePath(std::vector<std::string>& paths, const std::string& path) {
    // シーン内のパスを正規化したうえで一意に保つ。
    AddUnique(paths, NormalizePath(path), [](const std::string& value) { return value; });
}

bool IsTextureKey(const std::string& lowerKey) {
    // 拡張子のない参照も、プロパティ名からテクスチャと判別する。
    return lowerKey == "texture" ||
        lowerKey.find("texturepath") != std::string::npos ||
        lowerKey.find("texturename") != std::string::npos ||
        lowerKey.find("normalmap") != std::string::npos ||
        lowerKey.find("ormmap") != std::string::npos ||
        lowerKey.find("albedomap") != std::string::npos ||
        lowerKey.find("emissivemap") != std::string::npos ||
        lowerKey.find("maskmap") != std::string::npos;
}

bool IsLinearTextureKey(const std::string& lowerKey) {
    // 法線やORMの値をsRGB変換しないため、線形テクスチャとして要求する。
    return lowerKey.find("normal") != std::string::npos ||
        lowerKey.find("orm") != std::string::npos ||
        lowerKey.find("mask") != std::string::npos ||
        lowerKey.find("roughness") != std::string::npos ||
        lowerKey.find("metallic") != std::string::npos;
}

bool IsTextureFile(const std::filesystem::path& path) {
    // 依存探索ではゲームが扱う画像拡張子だけを対象にする。
    const std::string extension = ToLower(PathToUtf8(path.extension()));
    return extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
        extension == ".tga" || extension == ".dds" || extension == ".hdr";
}

// 依存ファイルを種別ごとに記録し、必要なら実ファイルの欠落も報告します。
void RecordDependency(
    ScenePreloadData& data,
    SceneDependencyType type,
    const std::string& rawPath,
    bool validateExists = true) {
    const std::string path = NormalizePath(rawPath);

    // 空欄は依存先を指定していないため報告に加えない。
    if (path.empty()) {
        return;
    }

    // 同じ種類・パスの参照を一度だけ記録し、欠落警告も重複させない。
    const auto found = std::find_if(
        data.dependencyReport.assets.begin(),
        data.dependencyReport.assets.end(),
        [&](const SceneDependencyRecord& dependency) {
            return dependency.type == type && dependency.path == path;
        });
    // 同じ種別とパスを記録済みなら存在確認を繰り返さない。
    if (found != data.dependencyReport.assets.end()) {
        return;
    }

    std::error_code error;
    const bool exists = !validateExists || std::filesystem::exists(PathFromUtf8(path), error);
    data.dependencyReport.assets.push_back({ type, path, exists });
    // 存在確認に失敗したパスは、種別にかかわらず欠落一覧に一度だけ記録する。
    if (!exists &&
        std::find(
            data.dependencyReport.missingPaths.begin(),
            data.dependencyReport.missingPaths.end(),
            path) == data.dependencyReport.missingPaths.end()) {
        data.dependencyReport.missingPaths.push_back(path);
    }
}

void AddJsonDependency(ScenePreloadData& data, const std::string& path) {
    // JSON参照も欠落調査の対象へ記録する。
    RecordDependency(data, SceneDependencyType::kJson, PathToUtf8(vfxAssetPath::ResolveDependency(PathFromUtf8(path))));
}

void AddAudioDependency(ScenePreloadData& data, std::string path) {
    // 短いSE名は共有SEディレクトリからの相対指定として扱う。
    path = NormalizePath(std::move(path));

    // Resources配下から始まらない指定は、共通SEディレクトリを基準にする。
    if (path.rfind("Resources/", 0) != 0) {
        path = "Resources/audio/se/" + path;
    }

    AddUniquePath(data.audioPaths, path);
    RecordDependency(data, SceneDependencyType::kAudio, path);
}

// 色空間の違いも識別して共有画像パスの重複を避けます。
void AddSharedTexture(
    ScenePreloadData& data,
    const std::string& path,
    bool linear = false) {
    SceneLoadManifest::TextureRequest request{ NormalizePath(path), linear };
    AddUnique(data.textures, std::move(request), [](const SceneLoadManifest::TextureRequest& item) {
        return item.path + (item.linear ? "|linear" : "|color");
    });
}

// 指定ディレクトリ直下の画像だけを共有の先読みリストに加える。
void CollectSharedTextures(ScenePreloadData& data, const std::filesystem::path& directory) {
    std::error_code error;

    // 共有ディレクトリが存在しない、または確認できない場合は共通画像の追加を省く。
    if (!std::filesystem::exists(directory, error) || error) {
        return;
    }

    for (std::filesystem::directory_iterator iterator(
             directory,
             std::filesystem::directory_options::skip_permission_denied,
             error), end;
         iterator != end;
         iterator.increment(error)) {
        // 列挙エラーがある箇所では項目を読まず、エラーを解除して次へ進む。
        if (error) {
            error.clear();
            continue;
        }

        // 通常ファイルの確認に失敗した項目と対象拡張子以外は先読みしない。
        if (!iterator->is_regular_file(error) || error || !IsTextureFile(iterator->path())) {
            error.clear();
            continue;
        }

        AddSharedTexture(data, PathToUtf8(iterator->path()));
    }
}

// JSONを再帰的に走査し、画像・音声・JSONなどの参照先を種別ごとに集めます。
void CollectDependencies(
    const nlohmann::json& value,
    const std::string& propertyName,
    ScenePreloadData& data,
    bool spriteTexturePaths) {
    // ネストしたJSONを再帰走査し、モデル・画像・音声・演出プリセットを抽出する。
    // オブジェクトでは、専用形式を先に判定してから個々のプロパティを再帰探索する。
    if (value.is_object()) {
        // VFXイベントは同じ presetName でも type ごとに探索先が違うため、一般の文字列判定より先に扱う。
        if (value.contains("type") && value["type"].is_number_integer() &&
            value.contains("presetName") && value["presetName"].is_string() &&
            value.contains("triggerTime")) {
            const std::int32_t eventType = value["type"].get<std::int32_t>();
            const std::string presetName =
                NormalizePath(value["presetName"].get<std::string>());
            // 名前が空のイベントに対して、参照先ファイルを作らない。
            if (!presetName.empty()) {
                // VFXイベントの種別番号で、粒子・メッシュ・音声のどの保存先から同名の素材を探すか決める。
                // 種別0はGPU粒子の設定JSONとして解決する。
                if (eventType == 0) {
                    AddJsonDependency(
                        data,
                        "Resources/json/gpu_particles/" + presetName + ".json");
                }
                // 種別1はメッシュエフェクトの設定JSONとして解決する。
                else if (eventType == 1) {
                    AddJsonDependency(
                        data,
                        "Resources/json/effect/" + presetName + ".json");
                }
                // 種別2はイベント設定JSONか直接指定の音声として解決する。
                else if (eventType == 2) {
                    const std::filesystem::path presetPath = PathFromUtf8(presetName);

                    // 拡張子がJSONなら、共有音声イベントの保存先でファイル名を解決する。
                    if (ToLower(PathToUtf8(presetPath.extension())) == ".json") {
                        std::filesystem::path eventPath =
                            std::filesystem::path("Resources/json/audio_events") /
                            presetPath.filename();
                        AddJsonDependency(data, PathToUtf8(eventPath));
                    }
                    // 直接指定の音声なら、共通SEのパス解決へ渡す。
                    else {
                        AddAudioDependency(data, presetName);
                    }
                }
            }
        }

        for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
            CollectDependencies(iterator.value(), iterator.key(), data, spriteTexturePaths);
        }

        return;
    }

    // 配列の要素名は省略されるため、親プロパティの名前を渡して同じ種類として調べる。
    if (value.is_array()) {
        for (const auto& element : value) {
            CollectDependencies(element, propertyName, data, spriteTexturePaths);
        }

        return;
    }

    // オブジェクトと配列を処理した後は、参照名を持てる文字列だけを調べる。
    if (!value.is_string()) {
        return;
    }

    // キー名と拡張子の双方を見て分類する。拡張子を持たないモデル名やPreset名も対象にするため。
    std::string stringValue = NormalizePath(value.get<std::string>());

    // 空の値は参照先を指定していないため無視する。
    if (stringValue.empty()) {
        return;
    }

    const std::string lowerKey = ToLower(propertyName);

    // GPUパーティクル名を保持し、対応する設定JSONも依存先に加える。
    if (lowerKey == "gpuparticlename") {
        AddUniquePath(data.gpuParticlePresetNames, stringValue);
        AddJsonDependency(data, "Resources/json/gpu_particles/" + stringValue + ".json");
        return;
    }

    // モデル名はModelManagerが解決するキーで、ここではファイルパスとして存在確認しない。
    if (lowerKey == "modelname") {
        AddUniquePath(data.modelNames, stringValue);
        RecordDependency(data, SceneDependencyType::kModel, stringValue, false);
        return;
    }

    // パスや拡張子なしの素材名を、以下でキー名と拡張子を使って分類する。
    const std::string extension = GetPathExtension(stringValue);

    // JSON参照は保存先を必要に応じて補い、さらに走査できる依存先として登録する。
    if (extension == ".json") {
        std::filesystem::path path = PathFromUtf8(stringValue);
        // AnimatorまたはControllerの短い参照は、Animator設定の保存先を基準にする。
        if ((lowerKey.find("animator") != std::string::npos ||
             lowerKey.find("controller") != std::string::npos) &&
            stringValue.rfind("Resources/", 0) != 0) {
            path = std::filesystem::path("Resources/json/animator") / path.filename();
        }
        // Cinematicの短い参照は、演出設定の保存先を基準にする。
        else if (lowerKey.find("cinematic") != std::string::npos &&
            stringValue.rfind("Resources/", 0) != 0) {
            path = std::filesystem::path("Resources/json/cinematic") / path.filename();
        }

        AddJsonDependency(data, PathToUtf8(path));
        return;
    }

    // 対応する音声拡張子なら、画像や設定の分類へ進む前に音声依存として登録する。
    if (extension == ".wav" || extension == ".mp3" || extension == ".ogg" || extension == ".flac") {
        AddAudioDependency(data, stringValue);
        return;
    }

    // 対応する画像拡張子か画像用のキーなら、色空間を含む読込要求を作る。
    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
        extension == ".tga" || extension == ".dds" || extension == ".hdr" || IsTextureKey(lowerKey)) {
        SceneLoadManifest::TextureRequest request;
        // Sprite配置JSONの相対パスは実ロードと同じ規則で解決し、先読みと本読込のパスを一致させる。
        request.path = spriteTexturePaths ? engine::Sprite::ResolveTexturePath(stringValue) : stringValue;
        request.linear = IsLinearTextureKey(lowerKey);
        AddUnique(data.textures, request, [](const SceneLoadManifest::TextureRequest& item) {
            return item.path + (item.linear ? "|linear" : "|color");
        });
        RecordDependency(data, SceneDependencyType::kTexture, request.path);
        return;
    }

    // ファイル拡張子で分類できなかったAnimator系の指定も、Controllerへの参照として扱う。
    if (lowerKey.find("animator") != std::string::npos ||
        lowerKey.find("controller") != std::string::npos) {
        std::filesystem::path controllerPath = PathFromUtf8(stringValue);

        // 拡張子なしのController名にはJSONの拡張子を補う。
        if (controllerPath.extension().empty()) {
            controllerPath.replace_extension(".json");
        }

        // Resources配下の明示指定でなければ、Animatorの既定ディレクトリから探す。
        if (NormalizePath(PathToUtf8(controllerPath)).rfind("Resources/", 0) != 0) {
            controllerPath =
                std::filesystem::path("Resources/json/animator") /
                controllerPath.filename();
        }

        AddJsonDependency(data, PathToUtf8(controllerPath));
        return;
    }

    // 拡張子のない演出名もJSON参照として解決し、依存探索の対象にする。
    if (lowerKey.find("cinematic") != std::string::npos) {
        std::filesystem::path cinematicPath = PathFromUtf8(stringValue);

        // 拡張子を省略した演出名はJSONファイル名へ変換する。
        if (cinematicPath.extension().empty()) {
            cinematicPath.replace_extension(".json");
        }

        // Resources配下の明示指定でなければ、Cinematicの既定ディレクトリから探す。
        if (NormalizePath(PathToUtf8(cinematicPath)).rfind("Resources/", 0) != 0) {
            cinematicPath =
                std::filesystem::path("Resources/json/cinematic") /
                cinematicPath.filename();
        }

        AddJsonDependency(data, PathToUtf8(cinematicPath));
        return;
    }

    // 既知の種類に当てはまらないResources内のファイル参照も欠落確認に残す。
    if (stringValue.rfind("Resources/", 0) == 0 && !extension.empty()) {
        RecordDependency(data, SceneDependencyType::kOther, stringValue);
    }
}

void ExpandObjectLayoutPaths(const std::string& path, std::vector<std::string>& jsonPaths) {
    // 分割されたScene Assetがある場合は各領域を使い、旧形式の単一JSONにも対応する。
    const std::filesystem::path sourcePath = PathFromUtf8(NormalizePath(path));
    std::filesystem::path basePath = sourcePath;
    basePath.replace_extension();

    const std::string base = NormalizePath(PathToUtf8(basePath));
    const std::vector<std::string> splitPaths = {
        base + "_player.json",
        base + "_enemy.json",
        base + "_object.json",
        base + "_camera.json"
    };

    bool hasSplitFile = false;

    for (const std::string& splitPath : splitPaths) {
        // 分割先が一件でも存在すれば、分割配置を選ぶ条件を満たす。
        if (std::filesystem::exists(PathFromUtf8(splitPath))) {
            hasSplitFile = true;
            break;
        }
    }

    // 分割配置が一つでも見つかれば存在する分割ファイルを使い、旧単一ファイルとは重複して読まない。
    if (!hasSplitFile) {
        AddUniquePath(jsonPaths, path);
        return;
    }

    for (const std::string& splitPath : splitPaths) {
        // 分割配置では、実際に存在するファイルだけを読込リストへ渡す。
        if (std::filesystem::exists(PathFromUtf8(splitPath))) {
            AddUniquePath(jsonPaths, splitPath);
        }
    }
}

bool LoadJsonDocument(const std::string& path, nlohmann::json& document, std::string& warning) {
    // 失敗理由を呼び出し側へ渡し、他の依存ファイルの探索は継続する。
    std::ifstream file(PathFromUtf8(path));

    // ファイルを開けなければ、構文解析へ進まずパス付きの警告を返す。
    if (!file.is_open()) {
        warning = "Scene preload could not open JSON: " + path;
        return false;
    }

    try {
        file >> document;
        return true;
    }
    // 開けても構文解析に失敗したときは、例外の内容を警告に残す。
    catch (const std::exception& exception) {
        warning = "Scene preload JSON parse failed: " + path + " (" + exception.what() + ")";
        return false;
    }
}
}

void SceneLoadManifest::AddObjectLayout(const std::string& path) {
    // 同じ配置ファイルを複数のシーン機能が要求しても一件にまとめる。
    AddUniquePath(objectLayoutPaths, path);
}

void SceneLoadManifest::AddSpriteLayout(const std::string& path) {
    // Sprite配置の重複読込を避ける。
    AddUniquePath(spriteLayoutPaths, path);
}

void SceneLoadManifest::AddJson(const std::string& path) {
    // 配置以外のJSONも同じ依存探索へ含める。
    AddUniquePath(additionalJsonPaths, path);
}

void SceneLoadManifest::AddModel(const std::string& modelName) {
    // モデル名を先読み対象として一度だけ登録する。
    AddUniquePath(modelNames, modelName);
}

void SceneLoadManifest::AddTexture(const std::string& path, bool linear) {
    // 同一画像でも線形色空間と色画像は別のGPU資源要求として扱う。
    TextureRequest request{ NormalizePath(path), linear };
    AddUnique(textures, std::move(request), [](const TextureRequest& item) {
        return item.path + (item.linear ? "|linear" : "|color");
    });
}

void SceneLoadManifest::AddAudio(const std::string& path) {
    // 明示されたBGMやSEの候補を一意に保つ。
    AddUniquePath(audioPaths, path);
}

std::size_t SceneDependencyReport::Count(SceneDependencyType type) const {
    // 欠落の有無にかかわらず、種類ごとの参照件数を集計する。
    return static_cast<std::size_t>(std::count_if(
        assets.begin(),
        assets.end(),
        [type](const SceneDependencyRecord& dependency) {
            return dependency.type == type;
        }));
}

void ScenePreloadProgress::Begin(std::size_t totalTasks) {
    // 進捗表示でゼロ除算しないよう、最低一件として開始する。
    completedTasks_.store(0, std::memory_order_relaxed);
    totalTasks_.store((std::max)(totalTasks, std::size_t{ 1 }), std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(taskMutex_);
    currentTask_.clear();
}

void ScenePreloadProgress::Advance(const std::string& currentTask) {
    // 件数は原子変数、表示文字列はMutexで別々に保護する。
    completedTasks_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(taskMutex_);
    currentTask_ = currentTask;
}

void ScenePreloadProgress::AddTasks(std::size_t taskCount) {
    // JSONから発見した追加依存を進捗の分母へ加える。
    totalTasks_.fetch_add(taskCount, std::memory_order_relaxed);
}

// 完了時は処理済み件数を総件数に合わせ、UIの表示を100%にする。
void ScenePreloadProgress::Finish() {
    completedTasks_.store(totalTasks_.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

float ScenePreloadProgress::GetRatio() const {
    // 作業スレッドから更新された件数を読み、UI向けに0から1へ収める。
    const std::size_t total = totalTasks_.load(std::memory_order_relaxed);
    const std::size_t completed = completedTasks_.load(std::memory_order_relaxed);

    // 総件数がまだ設定されていなければ、除算せず完了相当の値を返す。
    if (total == 0) {
        return 1.0f;
    }

    return (std::min)(1.0f, static_cast<float>(completed) / static_cast<float>(total));
}

std::string ScenePreloadProgress::GetCurrentTask() const {
    // 作業中のパスはMutex下でコピーし、UIへ安全に渡す。
    std::lock_guard<std::mutex> lock(taskMutex_);
    return currentTask_;
}

bool ScenePreloadData::TakeJson(const std::string& path, nlohmann::json& destination) {
    // 解析済みJSONを移動して渡し、同じ文書の二重消費を防ぐ。
    const auto found = jsonDocuments.find(NormalizePath(path));

    // 未解析、または既に受け渡したパスには文書が残っていないため、出力を変えずに戻る。
    if (found == jsonDocuments.end()) {
        return false;
    }

    destination = std::move(found->second);
    jsonDocuments.erase(found);
    return true;
}

std::shared_ptr<ScenePreloadData> ScenePreloader::Prepare(
    const SceneLoadManifest& manifest,
    const std::shared_ptr<ScenePreloadProgress>& progress) {
    // CPU側で読めるシーン構成と素材をワーカーで事前準備し、結果と警告をまとめて返す。
    auto result = std::make_shared<ScenePreloadData>();

    // 明示された配置と設定を集め、既存の分割配置は個別ファイルとして扱う。
    std::vector<std::string> jsonPaths;

    for (const std::string& path : manifest.objectLayoutPaths) {
        ExpandObjectLayoutPaths(path, jsonPaths);
    }

    for (const std::string& path : manifest.spriteLayoutPaths) {
        AddUniquePath(jsonPaths, path);
    }

    for (const std::string& path : manifest.additionalJsonPaths) {
        AddUniquePath(jsonPaths, path);
    }

    // 進捗を表示する呼出元にだけ、初期のJSON件数を知らせる。
    if (progress) {
        progress->Begin(jsonPaths.size());
    }

    // JSON 内の参照からさらに JSON が見つかるため、走査中に候補列を伸ばします。
    std::size_t jsonIndex = 0;

    while (jsonIndex < jsonPaths.size()) {
        const std::string path = jsonPaths[jsonIndex++];
        RecordDependency(*result, SceneDependencyType::kJson, path);

        nlohmann::json document;
        std::string warning;

        // 解析した文書から依存先を集め、依存探索で警告が発生しても解析済みの文書を保存する。
        if (LoadJsonDocument(path, document, warning)) {
            // Sprite配置の画像参照には、実際のSprite生成と同じパス変換を適用する。
            const bool spriteAssetPaths =
                (document.contains("sprites") && document["sprites"].is_array()) ||
                (document.contains("labelTextures") && document["labelTextures"].is_object());
            try {
                CollectDependencies(document, "", *result, spriteAssetPaths);
            }
            catch (const std::exception& exception) {
                // この文書の途中で依存探索に失敗しても、解析済みJSONの保存と他のファイルの準備は続ける。
                result->warnings.push_back("Scene dependency collection failed for " + path + ": " + exception.what());
            }
            result->jsonDocuments.emplace(NormalizePath(path), std::move(document));

            // 参照先JSONが増えた分だけ進捗の総数を伸ばし、割合が早く100%にならないようにする。
            const std::size_t previousCount = jsonPaths.size();

            for (const SceneDependencyRecord& dependency :
                result->dependencyReport.assets) {
                // 設定JSONの参照だけを再帰探索用のリストへ加える。
                if (dependency.type == SceneDependencyType::kJson) {
                    AddUniquePath(jsonPaths, dependency.path);
                }
            }

            // 既に見たパスは追加されない。新規分だけを総件数へ足し、循環参照で同じJSONを読み直さない。
            if (progress && jsonPaths.size() > previousCount) {
                progress->AddTasks(jsonPaths.size() - previousCount);
            }
        }
        // 文書を解析できなかった場合は、得られた警告を結果に移す。
        else if (!warning.empty()) {
            result->warnings.push_back(std::move(warning));
        }

        // 進捗を通知する場合は、読込可否にかかわらず試行したJSON一件分を進める。
        if (progress) {
            progress->Advance(path);
        }
    }

    // JSONの走査で見つからない明示指定も、同じリストへ重複なく取り込む。
    for (const std::string& modelName : manifest.modelNames) {
        AddUniquePath(result->modelNames, modelName);
    }

    // 同一パスでも線形色空間と色テクスチャは別の準備要求として扱う。
    for (const SceneLoadManifest::TextureRequest& texture : manifest.textures) {
        AddUnique(result->textures, texture, [](const SceneLoadManifest::TextureRequest& item) {
            return NormalizePath(item.path) + (item.linear ? "|linear" : "|color");
        });
    }

    for (const std::string& audioPath : manifest.audioPaths) {
        AddAudioDependency(*result, audioPath);
    }

    for (const std::string& modelName : result->modelNames) {
        RecordDependency(*result, SceneDependencyType::kModel, modelName, false);
    }

    for (const SceneLoadManifest::TextureRequest& texture : result->textures) {
        RecordDependency(*result, SceneDependencyType::kTexture, texture.path);
    }

    // 共有パーティクル画像を使うよう指定された場合だけ、指定ディレクトリの画像を集める。
    if (manifest.includeSharedParticleTextures) {
        CollectSharedTextures(*result, "Resources/sprite/particle");
    }

    CollectSharedTextures(*result, "Resources/sprite/fade/crown_iris");
    AddSharedTexture(*result, "Resources/sprite/effect/noise0.png", true);
    AddSharedTexture(*result, "Resources/texture/lut/soft_adventure_lut.png");

    // 探索で素材を見つけた順序に依存しないよう、準備する順序を名前で固定する。
    std::sort(result->modelNames.begin(), result->modelNames.end());
    std::sort(result->textures.begin(), result->textures.end(), [](const auto& left, const auto& right) {
        // パスが同一なら自動色空間の要求を先、線形の要求を後にする。
        if (left.path == right.path) {
            return left.linear < right.linear;
        }
        return left.path < right.path;
    });

    std::sort(result->audioPaths.begin(), result->audioPaths.end());
    std::sort(
        result->dependencyReport.missingPaths.begin(),
        result->dependencyReport.missingPaths.end());
    for (const std::string& missing : result->dependencyReport.missingPaths) {
        result->warnings.push_back("Scene dependency is missing: " + missing);
    }

    // 進捗の受け手がいる場合は、確定した素材の件数を分母へ加える。
    if (progress) {
        // この時点で依存解析が終わり、残りの事前準備件数が確定します。
        progress->AddTasks(result->textures.size() + result->modelNames.size() + result->audioPaths.size());
    }

    // WIC画像デコードを行うため、ワーカースレッド側でCOMを初期化します。
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // 個別アセットの失敗を警告として残し、先読み全体が空の結果へ置き換わるのを防ぐ。
    const auto prepareAsset = [&](const std::string& path, const auto& prepare) {
        try {
            // 素材の準備関数が失敗を返した場合は、対象パスを警告へ記録する。
            if (!prepare()) {
                result->warnings.push_back("Scene asset preparation failed: " + path);
            }
        }
        // 素材の準備が例外で止まっても、警告へ記録して次の素材へ進む。
        catch (const std::exception& exception) {
            result->warnings.push_back("Scene asset preparation failed for " + path + ": " + exception.what());
        }
        // 進捗の受け手がいれば、成功・失敗にかかわらず一件を処理済みにする。
        if (progress) {
            progress->Advance(path);
        }
    };

    for (const SceneLoadManifest::TextureRequest& texture : result->textures) {
        prepareAsset(texture.path, [&] {
            return engine::TextureManager::GetInstance()->Prepare(
                texture.path,
                texture.linear ? engine::TextureManager::TextureColorSpace::kLinear
                               : engine::TextureManager::TextureColorSpace::kAuto);
        });
    }

    for (const std::string& modelName : result->modelNames) {
        prepareAsset(modelName, [&] { return ModelManager::GetInstance()->PrepareModel(modelName); });
    }

    // 音声はここで再生を開始せず、後から使える読込ハンドルだけを準備する。
    for (const std::string& audioPath : result->audioPaths) {
        prepareAsset(audioPath, [&] {
            AudioPlayer::GetInstance()->LoadSoundFile(audioPath);
            return true;
        });
    }

    // このワーカーでCOM初期化が成功した場合だけ、画像の事前デコードを終えてから対応する登録を解除する。
    if (SUCCEEDED(comResult)) {
        CoUninitialize();
    }

    // 進捗を通知している場合は、COMの後片付け後に完了を反映する。
    if (progress) {
        progress->Finish();
    }

    return result;
}

SceneDependencyReport ScenePreloader::Inspect(
    const SceneLoadManifest& manifest) {
    // Prepare()で実際の事前準備と同じ探索を行い、依存レポートだけを返す。
    const std::shared_ptr<ScenePreloadData> data = Prepare(manifest, nullptr);
    return data ? data->dependencyReport : SceneDependencyReport{};
}
