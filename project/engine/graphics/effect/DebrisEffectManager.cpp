#include <cstdint>
#include "engine/graphics/effect/VfxAssetPath.h"
#include "DebrisEffectManager.h"
#include <numbers>

#include "BaseScene.h"
#include "CameraManager.h"
#include "DebugConsole.h"
#include "DirectXCommon.h"
#include "LightManager.h"
#include "ModelManager.h"
#include "SceneManager.h"
#include "SrvManager.h"
#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;

namespace {
constexpr const char* kDebrisPresetDirectory = "Resources/json/debris/";
constexpr size_t kMaxActiveDebrisPieces = 96;
constexpr bool kEnableDebrisShadowWvp = true;

engine::Vector3 ReadVector3(const json& j, const char* key, const engine::Vector3& fallback) {
    // キーがなければ、またはXYZを持つ配列として読めない場合は元の値を保つ。
    if (!j.contains(key) || !j[key].is_array() || j[key].size() < 3) {
        return fallback;
    }

    return { j[key][0].get<float>(), j[key][1].get<float>(), j[key][2].get<float>() };
}

engine::Vector4 ReadVector4(const json& j, const char* key, const engine::Vector4& fallback) {
    // RGBAに必要な四成分を配列から取れなければ、その項目の既定値を使う。
    if (!j.contains(key) || !j[key].is_array() || j[key].size() < 4) {
        return fallback;
    }

    return { j[key][0].get<float>(), j[key][1].get<float>(), j[key][2].get<float>(), j[key][3].get<float>() };
}

void WriteVector3(json& j, const char* key, const engine::Vector3& value) {
    j[key] = { value.x, value.y, value.z };
}

void WriteVector4(json& j, const char* key, const engine::Vector4& value) {
    j[key] = { value.x, value.y, value.z, value.w };
}

// 最大値がほぼゼロの場合は比率1として扱います。
float SafeRatio(float value, float maxValue) {
    // 分母が正の小さな値以下なら、除算を避けて進行済み相当の1を返す。
    if (maxValue <= 0.0001f) {
        return 1.0f;
    }

    return value / maxValue;
}
}

DebrisEffectManager* DebrisEffectManager::GetInstance() {
    // 破片プールとプリセットを Scene をまたいで共有する。
    static DebrisEffectManager sInstance;
    return &sInstance;
}

// プレビューで種を固定されていない通常時は、運動の抽選に使う乱数を初期化する。
DebrisEffectManager::DebrisEffectManager()
    : randomEngine_(std::random_device{}()) {
}

void DebrisEffectManager::Initialize(engine::Object3DCommon* common) {
    // 描画共通資源が切り替わったときは旧 GPU バッファを再利用せず作り直す。
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // 描画基盤の参照が変わったときは、旧シーンの破片とカメラ定数を破棄する。
    if (common_ != common) {
        activePieces_.clear();
        pooledPieces_.clear();
        common_ = common;
        cameraResource_.Reset();
        cameraData_ = nullptr;

        // 新しい共通資源とDirectX基盤が揃った場合だけ、カメラ定数を確保する。
        if (common_ && common_->GetDxCommon()) {
            cameraResource_ = common_->GetDxCommon()->CreateBufferResource(sizeof(engine::MeshRenderer::CameraForGpu));

            // カメラの定数バッファを確保できなければ、書き込み先を要求せず戻る。
            if (!cameraResource_) {
                DebugConsole::GetInstance()->AddLog(LogLevel::kError, "[DebrisEffect] camera buffer creation failed.");
                return;
            }

            HRESULT hr = cameraResource_->Map(0, nullptr, reinterpret_cast<void**>(&cameraData_));

            // カメラ定数をCPUから書けない場合は、描画に使わないよう解放する。
            if (FAILED(hr) || !cameraData_) {
                DebugConsole::GetInstance()->AddLog(LogLevel::kError, "[DebrisEffect] camera buffer map failed.");
                cameraResource_.Reset();
                cameraData_ = nullptr;
                return;
            }

            *cameraData_ = {}; // 破片には視界確保の透過を適用しない。
        }
    }
}

engine::Object3DCommon* DebrisEffectManager::ResolveCommon() {
    // 現在 Scene の描画資源へ追従し、前 Scene のリソース参照を残さない。
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    SceneManager* sceneManager = SceneManager::GetInstance();
    BaseScene* scene = sceneManager ? sceneManager->GetCurrentScene() : nullptr;
    engine::Object3DCommon* sceneCommon = scene ? scene->GetObject3DCommon() : nullptr;

    // 現在のSceneが別の描画基盤を持つ場合は初期化し直す。同じロックで入れ子に呼べるよう再帰Mutexを使う。
    if (sceneCommon && sceneCommon != common_) {
        Initialize(sceneCommon);
    }

    return common_;
}

// 再生倍率を反映して破片の物理と寿命を進め、現在の視点に合わせて行列を更新する。
void DebrisEffectManager::Update(float deltaTime) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    deltaTime *= timeScale_;

    // 生存破片が一つもなければ、行列取得と更新を省く。
    if (activePieces_.empty()) {
        return;
    }

    // このフレームのカメラ・ライト行列は全破片で共有し、休止中の破片にも視点変更を反映する。
    engine::Math math;
    engine::Matrix4x4 viewProjection = engine::Math::MakeIdentity4x4();
    engine::Camera* camera = CameraManager::GetInstance()->GetActiveCamera();

    // 現在のカメラを取得できればビュー射影行列を使い、不在時は単位行列を使う。
    if (camera) {
        viewProjection = math.Multiply(camera->GetViewMatrix(), camera->GetProjectionMatrix());

        // 共有の定数バッファを書ける場合は、カメラのワールド位置を更新する。
        if (cameraData_) {
            cameraData_->worldPosition = camera->GetEye();
        }
    }

    engine::Matrix4x4 lightViewProjection = engine::LightManager::GetInstance()->GetDirectionalLight().lightViewProj;
    const engine::Matrix4x4* lightViewProjectionPtr = kEnableDebrisShadowWvp ? &lightViewProjection : nullptr;

    for (size_t index = 0; index < activePieces_.size();) {
        DebrisPiece& piece = activePieces_[index];
        piece.age += deltaTime;

        // 経過秒が寿命に達した破片は、それ以降の運動や描画を処理せずプールへ戻す。
        if (piece.age >= piece.lifetime) {
            // RecyclePieceは末尾の要素を現在位置へ移すため、ここではindexを進めない。
            RecyclePiece(index);
            continue;
        }

        float lifeRatio = SafeRatio(piece.age, piece.lifetime);
        bool transformDirty = false;

        // 休止中は運動計算だけを止め、寿命の進行と後続の縮小処理は続ける。
        if (!piece.sleeping) {
            // 経過秒を掛けて重力を速度へ反映し、空気抵抗の係数を掛けた速度で位置を進めます。
            piece.velocity.y -= piece.gravity * deltaTime;
            float drag = (std::max)(0.0f, 1.0f - piece.airDrag * deltaTime);
            piece.velocity.x *= drag;
            piece.velocity.y *= drag;
            piece.velocity.z *= drag;

            engine::Vector3 position = piece.position + piece.velocity * deltaTime;
            engine::Vector3 rotation = piece.rotation + piece.angularVelocity * deltaTime;

            // 地面との衝突が有効で、降下中に床の高さへ到達した場合は速度を反射する。
            if (piece.collideGround && position.y <= piece.groundY && piece.velocity.y < 0.0f) {
                // 降下中に床を越えた破片を面へ戻します。反発は鉛直速度、摩擦は水平と回転の速度に掛けます。
                position.y = piece.groundY;
                piece.velocity.y = -piece.velocity.y * piece.restitution;
                piece.velocity.x *= piece.friction;
                piece.velocity.z *= piece.friction;
                piece.angularVelocity = piece.angularVelocity * piece.friction;

                // 反射後の鉛直速度と全体の速さがどちらも小さければ、その後の運動を休止する。
                if (std::abs(piece.velocity.y) < 0.7f && engine::Math::Length(piece.velocity) < 2.0f) {
                    piece.sleeping = true;
                }
            }

            piece.position = position;
            piece.rotation = rotation;
            transformDirty = true;
        }

        float scale = piece.baseScale;

        // 消失区間だけ基準サイズから縮め、前フレームの縮小値を累積して掛けない。
        if (piece.shrinkOnFade && lifeRatio > piece.fadeStartRatio) {
            // 寿命全体の割合を消失部分だけの0～1へ変換し、縮小が始まる前の大きさを変えないようにします。
            float fadeT = (lifeRatio - piece.fadeStartRatio) / (std::max)(0.001f, 1.0f - piece.fadeStartRatio);
            scale *= (std::max)(0.001f, 1.0f - fadeT);
        }

        // 消失でサイズが変化した場合も、運動した場合と同様に姿勢の変更を記録する。
        if (std::abs(scale - piece.currentScale) > 0.0001f) {
            transformDirty = true;
        }

        piece.currentScale = scale;
        piece.matrixDirty = piece.matrixDirty || transformDirty;
        UpdatePieceMatrix(piece, viewProjection, lightViewProjectionPtr);
        piece.matrixDirty = false;
        ++index;
    }
}

void DebrisEffectManager::Draw(ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource) {
    // 破片ごとの wvp・材質を渡し、共通のカメラとライトでメッシュ描画する。
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // 表示対象がないか必要な描画基盤・カメラ定数が揃わなければコマンドを発行しない。
    if (activePieces_.empty() || !common_ || !cameraResource_) {
        return;
    }

    common_->SetGraphicsCommand();
    common_->SetPipelineState(BlendMode::kNormal);
    ID3D12GraphicsCommandList* commandList = common_->GetDxCommon()->GetCommandList();
    uint32_t shadowMapSrvHandle = common_->GetDxCommon()->GetShadowMapSrvHandle();

    for (auto& piece : activePieces_) {
        // モデルと通常描画用の姿勢・材質が揃う破片だけ描画する。
        if (!piece.model || !piece.wvpResource || !piece.materialResource) {
            continue;
        }

        // 影用の変換を持つ破片は、画面用WVPに加えてライトから見たWVPを束縛する。
        if (piece.shadowWvpResource) {
            commandList->SetGraphicsRootConstantBufferView(11, piece.shadowWvpResource->GetGPUVirtualAddress());
        }

        engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 12, shadowMapSrvHandle);
        piece.model->Draw(
            piece.wvpResource.Get(),
            engine::LightManager::GetInstance()->GetDirectionalLightResource(),
            cameraResource_.Get(),
            pointLightResource,
            spotLightResource,
            piece.materialResource.Get(),
            0,
            0,
            0
        );
    }
}

void DebrisEffectManager::Clear() {
    // GPU リソースを破棄せずプールへ戻し、次の発生で再利用できるようにする。
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    while (!activePieces_.empty()) {
        RecyclePiece(activePieces_.size() - 1);
    }
}

void DebrisEffectManager::ResetEditorPreview(uint32_t randomSeed) {
    // 編集中の再生を同じ乱数列で始め直し、時刻シーク時の見た目を再現する。
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Clear();
    randomEngine_.seed(randomSeed);
}

std::int32_t DebrisEffectManager::GetActivePieceCount() const {
    // 更新・描画と同じロックの下で個数を読み、編集 UI への値を安定させる。
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return static_cast<std::int32_t>(activePieces_.size());
}

void DebrisEffectManager::LoadAllPresets(const std::string& directoryPath) {
    // 下位フォルダーを含む相対パスをプリセットIDとして登録する。
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    namespace fs = std::filesystem;

    // 保存先が存在しない場合は、今ある台帳の内容を保って戻る。
    if (!fs::exists(directoryPath)) {
        return;
    }

    fs::path root(directoryPath);
    // ルート表記に末尾の区切りが付いていれば、相対パスの基準をディレクトリそのものへ直す。
    if (!root.has_filename()) root = root.parent_path();
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        // 列挙した中から通常のJSONファイルだけを設定として処理する。
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }

        DebrisEffectConfig config;

        // 設定を読み込めた場合だけ、拡張子を外した相対パスをIDにして登録する。
        if (LoadConfig(entry.path(), config)) {
            auto relative = entry.path().lexically_relative(root);
            const auto key = relative.replace_extension().generic_u8string();
            presets_[std::string(key.begin(), key.end())] = config;
        }
    }
}

bool DebrisEffectManager::LoadConfig(const std::filesystem::path& filePath, DebrisEffectConfig& outConfig) const {
    // 欠けた項目は Config の初期値を使い、古い JSON も読めるようにする。
    std::ifstream file(vfxAssetPath::ResolveDependency(filePath));

    // 設定ファイルが開けない場合は、出力を変更せずfalseを返す。
    if (!file.is_open()) {
        return false;
    }

    json j;
    file >> j;

    DebrisEffectConfig config;
    // 表示用の名前が記録されていれば設定値へ反映する。
    if (j.contains("name"))
        config.name = j["name"].get<std::string>();
    // モデル候補が配列で指定された場合は、既定の候補を置き換える。
    if (j.contains("modelNames") && j["modelNames"].is_array()) {
        config.modelNames.clear();

        for (const auto& model : j["modelNames"]) {
            // 型が文字列かつ空でない要素だけ、抽選可能なモデル名とする。
            if (model.is_string() && !model.get<std::string>().empty()) {
                config.modelNames.push_back(model.get<std::string>());
            }
        }
    }

    // モデル配列が空でも、発生処理へ空集合を渡さない。
    if (config.modelNames.empty()) {
        config.modelNames.push_back("Primitives/cube");
    }

    // ここから発生範囲と初速度の設定を読み、各項目の欠落時はConfigの既定値を残す。
    // 設定に指定されている場合だけ、発生個数を上書きする。
    if (j.contains("spawnCount"))
        config.spawnCount = j["spawnCount"].get<std::int32_t>();
    config.spawnOffset = ReadVector3(j, "spawnOffset", config.spawnOffset);
    config.spawnBoxHalfExtents = ReadVector3(j, "spawnBoxHalfExtents", config.spawnBoxHalfExtents);
    config.baseDirection = ReadVector3(j, "baseDirection", config.baseDirection);
    // 設定に指定されている場合だけ、水平方向の拡散量を上書きする。
    if (j.contains("horizontalSpread"))
        config.horizontalSpread = j["horizontalSpread"].get<float>();
    // 設定に指定されている場合だけ、鉛直方向成分の下限を上書きする。
    if (j.contains("verticalMin"))
        config.verticalMin = j["verticalMin"].get<float>();
    // 設定に指定されている場合だけ、鉛直方向成分の上限を上書きする。
    if (j.contains("verticalMax"))
        config.verticalMax = j["verticalMax"].get<float>();
    // 設定に指定されている場合だけ、初速の下限を上書きする。
    if (j.contains("speedMin"))
        config.speedMin = j["speedMin"].get<float>();
    // 設定に指定されている場合だけ、初速の上限を上書きする。
    if (j.contains("speedMax"))
        config.speedMax = j["speedMax"].get<float>();
    // 設定に指定されている場合だけ、回転速度の下限（ラジアン毎秒）を上書きする。
    if (j.contains("angularSpeedMin"))
        config.angularSpeedMin = j["angularSpeedMin"].get<float>();
    // 設定に指定されている場合だけ、回転速度の上限（ラジアン毎秒）を上書きする。
    if (j.contains("angularSpeedMax"))
        config.angularSpeedMax = j["angularSpeedMax"].get<float>();
    // 設定に指定されている場合だけ、基準サイズの倍率の下限を上書きする。
    if (j.contains("scaleMin"))
        config.scaleMin = j["scaleMin"].get<float>();
    // 設定に指定されている場合だけ、基準サイズの倍率の上限を上書きする。
    if (j.contains("scaleMax"))
        config.scaleMax = j["scaleMax"].get<float>();
    // 設定に指定されている場合だけ、寿命の下限（秒）を上書きする。
    if (j.contains("lifetimeMin"))
        config.lifetimeMin = j["lifetimeMin"].get<float>();
    // 設定に指定されている場合だけ、寿命の上限（秒）を上書きする。
    if (j.contains("lifetimeMax"))
        config.lifetimeMax = j["lifetimeMax"].get<float>();
    // 生成後の運動・地面衝突・消失タイミングは破片ごとの更新に引き継ぐ。
    // 設定に指定されている場合だけ、毎秒ごとに鉛直速度へ適用する重力を上書きする。
    if (j.contains("gravity"))
        config.gravity = j["gravity"].get<float>();
    // 設定に指定されている場合だけ、速度減衰の係数を上書きする。
    if (j.contains("airDrag"))
        config.airDrag = j["airDrag"].get<float>();
    // 設定に指定されている場合だけ、床での鉛直反発係数を上書きする。
    if (j.contains("restitution"))
        config.restitution = j["restitution"].get<float>();
    // 設定に指定されている場合だけ、床での水平・回転速度の減衰倍率を上書きする。
    if (j.contains("friction"))
        config.friction = j["friction"].get<float>();
    // 設定に指定されている場合だけ、固定の床面に使うワールドY座標を上書きする。
    if (j.contains("groundY"))
        config.groundY = j["groundY"].get<float>();
    // 設定に指定されている場合だけ、消失を始める寿命の進行率を上書きする。
    if (j.contains("fadeStartRatio"))
        config.fadeStartRatio = j["fadeStartRatio"].get<float>();
    // 描画材質の設定は物理値と分けて読み、生成時のGPUバッファ初期化へ渡す。
    config.color = ReadVector4(j, "color", config.color);
    // 設定に指定されている場合だけ、描画に渡す材質番号を上書きする。
    if (j.contains("materialType"))
        config.materialType = j["materialType"].get<std::int32_t>();
    // 設定に指定されている場合だけ、反射の粗さを上書きする。
    if (j.contains("roughness"))
        config.roughness = j["roughness"].get<float>();
    // 設定に指定されている場合だけ、金属度を上書きする。
    if (j.contains("metallic"))
        config.metallic = j["metallic"].get<float>();
    // 設定に指定されている場合だけ、環境反射の有無を上書きする。
    if (j.contains("enableEnvMap"))
        config.enableEnvMap = j["enableEnvMap"].get<bool>();
    // 設定に指定されている場合だけ、環境反射の強さを上書きする。
    if (j.contains("envIntensity"))
        config.envIntensity = j["envIntensity"].get<float>();
    // 設定に指定されている場合だけ、描画時の自己発光を上書きする。
    if (j.contains("emissive"))
        config.emissive = j["emissive"].get<float>();
    // 床との衝突を使う指定がある場合は、その真偽値を反映する。
    if (j.contains("collideGround"))
        config.collideGround = j["collideGround"].get<bool>();
    // 消失に合わせてサイズを変える指定がある場合は、その真偽値を反映する。
    if (j.contains("shrinkOnFade"))
        config.shrinkOnFade = j["shrinkOnFade"].get<bool>();

    outConfig = config;
    return true;
}

bool DebrisEffectManager::SaveConfig(const std::filesystem::path& filePath, const DebrisEffectConfig& config) const {
    // 編集した発生範囲・物理・材質を同じ JSON キーへ戻す。
    namespace fs = std::filesystem;
    fs::create_directories(fs::path(filePath).parent_path());

    json j;
    j["name"] = config.name;
    j["modelNames"] = config.modelNames;
    j["spawnCount"] = config.spawnCount;
    WriteVector3(j, "spawnOffset", config.spawnOffset);
    WriteVector3(j, "spawnBoxHalfExtents", config.spawnBoxHalfExtents);
    WriteVector3(j, "baseDirection", config.baseDirection);
    j["horizontalSpread"] = config.horizontalSpread;
    j["verticalMin"] = config.verticalMin;
    j["verticalMax"] = config.verticalMax;
    j["speedMin"] = config.speedMin;
    j["speedMax"] = config.speedMax;
    j["angularSpeedMin"] = config.angularSpeedMin;
    j["angularSpeedMax"] = config.angularSpeedMax;
    j["scaleMin"] = config.scaleMin;
    j["scaleMax"] = config.scaleMax;
    j["lifetimeMin"] = config.lifetimeMin;
    j["lifetimeMax"] = config.lifetimeMax;
    j["gravity"] = config.gravity;
    j["airDrag"] = config.airDrag;
    j["restitution"] = config.restitution;
    j["friction"] = config.friction;
    j["groundY"] = config.groundY;
    j["fadeStartRatio"] = config.fadeStartRatio;
    WriteVector4(j, "color", config.color);
    j["materialType"] = config.materialType;
    j["roughness"] = config.roughness;
    j["metallic"] = config.metallic;
    j["enableEnvMap"] = config.enableEnvMap;
    j["envIntensity"] = config.envIntensity;
    j["emissive"] = config.emissive;
    j["collideGround"] = config.collideGround;
    j["shrinkOnFade"] = config.shrinkOnFade;

    // すべての設定値をJSONに組み立ててからファイルを開き、書き込み結果を返す。
    std::ofstream file(filePath);

    // 出力先を開けない場合は書込みを試さずfalseを返す。
    if (!file.is_open()) {
        return false;
    }

    file << j.dump(4);
    file.close();
    return file.good();
}

void DebrisEffectManager::RegisterPreset(const std::string& presetName, const DebrisEffectConfig& config) {
    // 編集中の設定を名前で上書きし、以後の Spawn へすぐ反映する。
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    presets_[presetName] = config;
}

void DebrisEffectManager::PrewarmPreset(const std::string& presetName) {
    // 発生前にモデルと破片用定数バッファを用意し、初回フレームの遅延を抑える。
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    DebrisEffectConfig config;
    auto it = vfxAssetPath::FindPreset(presets_, presetName);

    // 管理に登録済みの設定を解決できた場合は、バッファ準備にそのまま使う。
    if (it != presets_.end()) {
        config = it->second;
    }
    // 未登録でもファイルを読めた場合は、次回の発生用に登録する。
    else if (LoadConfig(ResolvePresetPath(presetName), config)) {
        presets_[presetName] = config;
    }
    // どちらからも設定を取得できなければ、名前を警告に残して準備を中止する。
    else {
        DebugConsole::GetInstance()->AddLog(LogLevel::kWarning, "[DebrisEffect] prewarm preset not found: " + presetName);
        return;
    }

    engine::Object3DCommon* common = ResolveCommon();

    // 描画基盤を取得できない間はGPU定数を作れないため、警告を残して戻る。
    if (!common) {
        DebugConsole::GetInstance()->AddLog(LogLevel::kWarning, "[DebrisEffect] prewarm skipped without Object3dCommon: " + presetName);
        return;
    }

    // 同じモデルを重複して準備せず、候補未指定なら既定の箱だけをロードする。
    std::vector<std::string> modelNames;
    modelNames.reserve(config.modelNames.size());

    for (const std::string& modelName : config.modelNames) {
        // 空の候補はモデルの読込へ渡さない。
        if (modelName.empty()) {
            continue;
        }

        // 既に準備を要求した同じモデル名は再度ロードしない。
        if (std::find(modelNames.begin(), modelNames.end(), modelName) != modelNames.end()) {
            continue;
        }

        ModelManager::GetInstance()->LoadModel(modelName);
        modelNames.push_back(modelName);
    }

    // 有効なモデル名を一つも指定されていなければ、既定の箱を先読みする。
    if (modelNames.empty()) {
        ModelManager::GetInstance()->LoadModel("Primitives/cube");
    }

    // 発生数を上限で制限し、足りない分だけプールを増やす。
    std::int32_t count = std::clamp(config.spawnCount, 1, static_cast<std::int32_t>(kMaxActiveDebrisPieces));

    while (pooledPieces_.size() < static_cast<size_t>(count)) {
        DebrisPiece piece;

        // 必要なバッファを揃えられない場合はプールの拡張を打ち切り、既存枠は残す。
        if (!InitializePieceResources(piece, config)) {
            break;
        }

        pooledPieces_.push_back(std::move(piece));
    }
}

void DebrisEffectManager::PrewarmPresets(const std::vector<std::string>& presetNames) {
    // 同じプールを複数プリセットから共有し、順に必要量へ拡張する。
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    for (const std::string& presetName : presetNames) {
        PrewarmPreset(presetName);
    }
}

void DebrisEffectManager::Spawn(const std::string& presetName, const engine::Vector3& position) {
    // 登録済み設定を優先し、無いときだけ JSON を読んでキャッシュする。
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto it = vfxAssetPath::FindPreset(presets_, presetName);

    // 登録済みの候補を解決できたら、保存ファイルを再読込せず発生させる。
    if (it != presets_.end()) {
        SpawnFromConfig(it->second, position);
        return;
    }

    DebrisEffectConfig config;

    // 台帳になくても対応するファイルから読めた場合は、キャッシュに加えて発生させる。
    if (LoadConfig(ResolvePresetPath(presetName), config)) {
        presets_[presetName] = config;
        SpawnFromConfig(config, position);
    }
    // 両方で解決できない場合は発生せず、対象のプリセット名を警告に残す。
    else {
        DebugConsole::GetInstance()->AddLog(LogLevel::kWarning, "[DebrisEffect] preset not found: " + presetName);
    }
}

void DebrisEffectManager::SpawnOnGround(const std::string& presetName, const engine::Vector3& position, float groundY) {
    // プリセット本体は変更せず、今回の発生に限って衝突面の高さを差し替える。
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto it = vfxAssetPath::FindPreset(presets_, presetName);

    // 登録済みの設定があれば、そのコピーの床面だけを指定位置へ変える。
    if (it != presets_.end()) {
        DebrisEffectConfig config = it->second;
        config.groundY = groundY;
        SpawnFromConfig(config, position);
        return;
    }

    DebrisEffectConfig config;

    // 登録されていなくてもファイルを読めた場合は元の値をキャッシュし、今回のコピーだけ床面を変える。
    if (LoadConfig(ResolvePresetPath(presetName), config)) {
        presets_[presetName] = config;
        config.groundY = groundY;
        SpawnFromConfig(config, position);
    }
    // ファイルからも設定を読めない場合は、破片を出さず警告を記録する。
    else {
        DebugConsole::GetInstance()->AddLog(LogLevel::kWarning, "[DebrisEffect] preset not found: " + presetName);
    }
}

// 一回分の発生位置・初速・描画資源を確定し、更新できる破片だけ稼働リストへ登録する。
void DebrisEffectManager::SpawnFromConfig(const DebrisEffectConfig& config, const engine::Vector3& position) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    engine::Object3DCommon* common = ResolveCommon();

    // 描画基盤を解決できなければGPU資源を準備できないため生成しない。
    if (!common) {
        DebugConsole::GetInstance()->AddLog(LogLevel::kError, "[DebrisEffect] Object3dCommon is null.");
        return;
    }

    // 空名と重複を除いたモデル候補を作り、この候補列から破片ごとにランダム選択する。
    std::vector<std::string> modelNames;
    modelNames.reserve(config.modelNames.size());

    for (const std::string& modelName : config.modelNames) {
        // 空のモデル名は抽選候補から外す。
        if (modelName.empty()) {
            continue;
        }

        // 同じモデル名が重複しても抽選の比率を変えないようにする。
        if (std::find(modelNames.begin(), modelNames.end(), modelName) != modelNames.end()) {
            continue;
        }

        ModelManager::GetInstance()->LoadModel(modelName);
        modelNames.push_back(modelName);
    }

    // 候補が一つも指定されていない場合は箱を使い、空配列からの抽選を避ける。
    if (modelNames.empty()) {
        modelNames.push_back("Primitives/cube");
        ModelManager::GetInstance()->LoadModel(modelNames.front());
    }

    // 一度の要求数を制限してから、表示中の総数が収まるだけの枠を確保する。
    std::int32_t count = std::clamp(config.spawnCount, 1, 256);

    // 一回の発生数が管理側の同時上限より多い場合は、要求数をその上限に丸める。
    if (static_cast<size_t>(count) > kMaxActiveDebrisPieces) {
        count = static_cast<std::int32_t>(kMaxActiveDebrisPieces);
    }

    size_t requiredRoom = activePieces_.size() + static_cast<size_t>(count);

    // 同時表示数の上限を守るため、既存の破片を再利用して新しい破片の枠を確保する。
    while (requiredRoom > kMaxActiveDebrisPieces && !activePieces_.empty()) {
        RecyclePiece(0);
        --requiredRoom;
    }

    activePieces_.reserve(activePieces_.size() + static_cast<size_t>(count));

    for (std::int32_t i = 0; i < count; ++i) {
        const std::string& modelName = modelNames[RandomIndex(static_cast<std::int32_t>(modelNames.size()))];
        engine::Model* model = ModelManager::GetInstance()->LoadModel(modelName);

        // 抽選したモデルが読めない場合は、描画資源の取得前にこの破片を飛ばす。
        if (!model) {
            continue;
        }

        // プールからGPU資源を引き継ぎつつ、経過時間・休止状態・配置を今回の発生用に戻す。
        DebrisPiece piece = AcquirePiece();
        piece.model = model;
        piece.age = 0.0f;
        piece.sleeping = false;
        piece.matrixDirty = true;
        piece.position = position + config.spawnOffset + engine::Vector3{
            RandomRange(-std::abs(config.spawnBoxHalfExtents.x), std::abs(config.spawnBoxHalfExtents.x)),
            RandomRange(-std::abs(config.spawnBoxHalfExtents.y), std::abs(config.spawnBoxHalfExtents.y)),
            RandomRange(-std::abs(config.spawnBoxHalfExtents.z), std::abs(config.spawnBoxHalfExtents.z))
        };

        piece.rotation = {
            RandomRange(-std::numbers::pi_v<float>, std::numbers::pi_v<float>),
            RandomRange(-std::numbers::pi_v<float>, std::numbers::pi_v<float>),
            RandomRange(-std::numbers::pi_v<float>, std::numbers::pi_v<float>)
        };

        // 見た目の大きさと初速度・回転速度を個別に抽選し、同時発生でも動きを揃えない。
        float scale = RandomRange(config.scaleMin, config.scaleMax);
        piece.currentScale = scale;
        piece.velocity = RandomDirection(config) * RandomRange(config.speedMin, config.speedMax);
        piece.angularVelocity = {
            RandomRange(config.angularSpeedMin, config.angularSpeedMax) * RandomRange(-1.0f, 1.0f),
            RandomRange(config.angularSpeedMin, config.angularSpeedMax) * RandomRange(-1.0f, 1.0f),
            RandomRange(config.angularSpeedMin, config.angularSpeedMax) * RandomRange(-1.0f, 1.0f)
        };

        // 寿命と物理・消失設定を各破片へコピーし、発生後はプリセット参照なしで更新する。
        piece.lifetime = RandomRange(config.lifetimeMin, config.lifetimeMax);
        piece.baseScale = scale;
        piece.gravity = config.gravity;
        piece.airDrag = config.airDrag;
        piece.restitution = config.restitution;
        piece.friction = config.friction;
        piece.groundY = config.groundY;
        piece.fadeStartRatio = config.fadeStartRatio;
        piece.collideGround = config.collideGround;
        piece.shrinkOnFade = config.shrinkOnFade;

        // 描画バッファを準備できない破片は表示リストへ登録せず、今回の生成を見送る。
        if (!InitializePieceResources(piece, config)) {
            continue;
        }

        // 登録前に初期行列まで書き込み、次のUpdateを待たず描画できる状態にする。
        engine::Camera* camera = CameraManager::GetInstance()->GetActiveCamera();
        engine::Matrix4x4 viewProjection = engine::Math::MakeIdentity4x4();

        // カメラがある場合は初回描画のビュー射影を現在の視点に合わせる。
        if (camera) {
            viewProjection = engine::Math::Multiply(camera->GetViewMatrix(), camera->GetProjectionMatrix());
        }

        engine::Matrix4x4 lightViewProjection = engine::LightManager::GetInstance()->GetDirectionalLight().lightViewProj;
        UpdatePieceMatrix(piece, viewProjection, kEnableDebrisShadowWvp ? &lightViewProjection : nullptr);
        piece.matrixDirty = false;
        activePieces_.push_back(std::move(piece));
    }
}

DebrisEffectManager::DebrisPiece DebrisEffectManager::AcquirePiece() {
    // プールに残る GPU リソースを移動して取得し、空なら新規初期化へ任せる。
    if (pooledPieces_.empty()) {
        return {};
    }

    DebrisPiece piece = std::move(pooledPieces_.back());
    pooledPieces_.pop_back();
    return piece;
}

// 非表示になった破片から描画リソースを回収し、稼働リストを詰める。
void DebrisEffectManager::RecyclePiece(size_t index) {
    // 指定番号が現在のリストの範囲外なら、リストもプールも変更しない。
    if (index >= activePieces_.size()) {
        return;
    }

    // ModelManagerから借りたモデル参照を外してから、次の発生で使う描画バッファだけをプールへ移します。
    activePieces_[index].model = nullptr;

    // 待機プールに空きがある場合だけリソースを残す。満杯ならリストから除く際に解放される。
    if (pooledPieces_.size() < kMaxActiveDebrisPieces) {
        pooledPieces_.push_back(std::move(activePieces_[index]));
    }

    size_t lastIndex = activePieces_.size() - 1;

    // 末尾以外を削除する場合だけ、末尾の破片を空いた位置へ移す。
    if (index != lastIndex) {
        // 順序を維持する必要がないため、末尾を詰めて削除コストを一定にする。
        activePieces_[index] = std::move(activePieces_[lastIndex]);
    }

    activePieces_.pop_back();
}

bool DebrisEffectManager::InitializePieceResources(DebrisPiece& piece, const DebrisEffectConfig& config) {
    // プールから戻った破片のバッファは再利用し、不足したバッファだけ生成する。
    // 描画基盤かDirectXの資源管理を利用できなければ、定数バッファを作れない。
    if (!common_ || !common_->GetDxCommon()) {
        return false;
    }

    engine::DirectXCommon* dxCommon = common_->GetDxCommon();

    // 通常の変換を格納するバッファがまだない場合だけ新規作成する。
    if (!piece.wvpResource) {
        piece.wvpResource = dxCommon->CreateBufferResource(sizeof(engine::MeshRenderer::TransformationMatrix));

        // 変換バッファの確保に失敗したら、マップを要求しない。
        if (!piece.wvpResource) {
            return false;
        }

        HRESULT hr = piece.wvpResource->Map(0, nullptr, reinterpret_cast<void**>(&piece.wvpData));

        // 変換を書けるアドレスを取得できなければ、バッファを解放して失敗を返す。
        if (FAILED(hr) || !piece.wvpData) {
            piece.wvpResource.Reset();
            piece.wvpData = nullptr;
            return false;
        }
    }

    // 影用行列は影描画が有効な構成だけ用意し、通常描画のバッファとは別に保持する。
    if (kEnableDebrisShadowWvp && !piece.shadowWvpResource) {
        piece.shadowWvpResource = dxCommon->CreateBufferResource(sizeof(engine::MeshRenderer::TransformationMatrix));

        // 影の行列を使う設定で確保に失敗した場合は、この破片の準備を中止する。
        if (!piece.shadowWvpResource) {
            return false;
        }

        HRESULT hr = piece.shadowWvpResource->Map(0, nullptr, reinterpret_cast<void**>(&piece.shadowWvpData));

        // 影用バッファへ書けない場合は、参照を解放して再利用対象にしない。
        if (FAILED(hr) || !piece.shadowWvpData) {
            piece.shadowWvpResource.Reset();
            piece.shadowWvpData = nullptr;
            return false;
        }
    }

    // 材質の定数バッファがない場合だけ、現在の描画基盤から作る。
    if (!piece.materialResource) {
        piece.materialResource = dxCommon->CreateBufferResource(sizeof(engine::MeshRenderer::MaterialData));

        // 材質の格納先を確保できない場合は書き込みに進まない。
        if (!piece.materialResource) {
            return false;
        }

        HRESULT hr = piece.materialResource->Map(0, nullptr, reinterpret_cast<void**>(&piece.materialData));

        // 材質を書き込めるアドレスが得られない場合は、バッファを解放して失敗を返す。
        if (FAILED(hr) || !piece.materialData) {
            piece.materialResource.Reset();
            piece.materialData = nullptr;
            return false;
        }
    }

    // 再利用した破片も含め、必須の通常変換と材質のGPU資源が揃わないと描画できない。
    if (!piece.wvpResource || !piece.materialResource) {
        return false;
    }

    // 必須のGPU資源があってもCPUの書き込み参照が欠けていれば、初期値を反映せず失敗とする。
    if (!piece.wvpData || !piece.materialData) {
        return false;
    }

    // 再利用したバッファにも単位行列を書き、前の破片の変換を残さない。
    piece.wvpData->wvp = engine::Math::MakeIdentity4x4();
    piece.wvpData->world = engine::Math::MakeIdentity4x4();
    piece.wvpData->worldInverseTranspose = engine::Math::MakeIdentity4x4();

    // 任意の影用の書き込み先がある場合は、旧破片が使った値を単位行列に戻す。
    if (piece.shadowWvpData) {
        piece.shadowWvpData->wvp = engine::Math::MakeIdentity4x4();
        piece.shadowWvpData->world = engine::Math::MakeIdentity4x4();
        piece.shadowWvpData->worldInverseTranspose = engine::Math::MakeIdentity4x4();
    }

    // 以前の材質値が混ざらないよう、今回のプリセット値で全体を初期化する。
    std::memset(piece.materialData, 0, sizeof(engine::MeshRenderer::MaterialData));
    piece.materialData->color = config.color;
    piece.materialData->enableLighting = 1;
    piece.materialData->uvTransform = engine::Math::MakeIdentity4x4();
    piece.materialData->selectedLighting = 2;
    piece.materialData->shininess = 20.0f;
    piece.materialData->materialType = config.materialType;
    piece.materialData->roughness = std::clamp(config.roughness, 0.0f, 1.0f);
    piece.materialData->metallic = std::clamp(config.metallic, 0.0f, 1.0f);
    piece.materialData->enableNormalMap = 0;
    piece.materialData->enableEnvMap = config.enableEnvMap ? 1 : 0;
    piece.materialData->envIntensity = (std::max)(0.0f, config.envIntensity);
    piece.materialData->emissive = config.emissive;
    return true;
}

void DebrisEffectManager::UpdatePieceMatrix(DebrisPiece& piece, const engine::Matrix4x4& viewProjection, const engine::Matrix4x4* lightViewProjection) {
    // 通常描画と影描画へ同じ world 行列を渡し、破片の向きを一致させる。
    // 通常描画用の書き込み先がない場合は、影や材質も含めて更新しない。
    if (!piece.wvpData) {
        return;
    }

    engine::Vector3 scale = { piece.currentScale, piece.currentScale, piece.currentScale };
    engine::Matrix4x4 world = engine::Math::MakeAffineMatrix(scale, piece.rotation, piece.position);
    piece.wvpData->wvp = engine::Math::Multiply(world, viewProjection);
    piece.wvpData->world = world;
    // 法線を正しい方向へ変換するため、位置用のワールド行列と逆転置行列を別々にシェーダーへ渡します。
    piece.wvpData->worldInverseTranspose = engine::Math::Transpose(engine::Math::Inverse(world));

    // 材質の領域がある場合だけ、生成後の経過秒をシェーダーに渡す。
    if (piece.materialData) {
        piece.materialData->time = piece.age;
    }

    // 影用バッファと光源側のビュー射影が揃う場合だけ、影用の行列を更新する。
    if (piece.shadowWvpData && lightViewProjection) {
        piece.shadowWvpData->wvp = engine::Math::Multiply(world, *lightViewProjection);
        piece.shadowWvpData->world = world;
        piece.shadowWvpData->worldInverseTranspose = piece.wvpData->worldInverseTranspose;
    }
}

float DebrisEffectManager::RandomRange(float minValue, float maxValue) {
    // JSON で上下限が逆でも同じ範囲を使えるよう、分布生成前に並べ替える。
    if (minValue > maxValue) {
        std::swap(minValue, maxValue);
    }

    std::uniform_real_distribution<float> distribution(minValue, maxValue);
    return distribution(randomEngine_);
}

std::int32_t DebrisEffectManager::RandomIndex(std::int32_t maxExclusive) {
    // 候補が一つ以下なら乱数を消費せず、配列外参照を防ぐ。
    if (maxExclusive <= 1) {
        return 0;
    }

    std::uniform_int_distribution<std::int32_t> distribution(0, maxExclusive - 1);
    return distribution(randomEngine_);
}

engine::Vector3 DebrisEffectManager::RandomDirection(const DebrisEffectConfig& config) {
    // 基準方向へ水平拡散と垂直範囲を加え、最後に単位方向へ正規化する。
    engine::Vector3 base = engine::Math::Normalize(config.baseDirection);

    // 指定方向がほぼ0の場合は、正のZを拡散の基準にする。
    if (engine::Math::Length(base) < 0.001f) {
        base = { 0.0f, 0.0f, 1.0f };
    }

    float angle = RandomRange(-std::numbers::pi_v<float>, std::numbers::pi_v<float>);
    float spread = (std::max)(0.0f, config.horizontalSpread);
    engine::Vector3 randomHorizontal = { std::cos(angle) * spread, 0.0f, std::sin(angle) * spread };
    engine::Vector3 direction = {
        base.x + randomHorizontal.x,
        RandomRange(config.verticalMin, config.verticalMax),
        base.z + randomHorizontal.z
    };

    return engine::Math::Normalize(direction);
}

std::filesystem::path DebrisEffectManager::ResolvePresetPath(const std::string& presetName) const {
    const std::filesystem::path path(std::u8string(presetName.begin(), presetName.end()));
    // 明示したファイル名はそのまま使い、拡張子なしの名前だけ破片プリセットのルートから解決する。
    if (path.is_absolute() || path.extension() == ".json") return path;
    return std::filesystem::path(kDebrisPresetDirectory) /
        std::filesystem::path(std::u8string(presetName.begin(), presetName.end()) + u8".json");
}

#ifdef USE_IMGUI
// 現在の破片と再生の時計を退避し、独立したエディター再生を始める。
void DebrisEffectManager::BeginEditorSession() {
    std::scoped_lock lock(mutex_);
    // 既に編集セッション内なら、退避した本来の状態を上書きしない。
    if (editorSession_) return;
    savedEditorScale_ = timeScale_; savedEditorRandom_ = randomEngine_;
    // 本番の破片を一時退避し、プレビュー用の乱数を固定して同じ条件から再生します。
    activePieces_.swap(savedEditorPieces_); randomEngine_.seed(0xD3B1u);
    timeScale_ = 1; editorSession_ = true;
}
// プレビューを終了し、退避した破片と再生倍率・乱数を戻す。
void DebrisEffectManager::EndEditorSession() {
    std::scoped_lock lock(mutex_);
    // 編集セッションがない場合は復元できる退避状態がないため何もしない。
    if (!editorSession_) return;
    // プレビューで生成した破片を先に回収し、その後で本番の破片、時間倍率、乱数の続きへ戻します。
    Clear(); activePieces_.swap(savedEditorPieces_);
    timeScale_ = savedEditorScale_; randomEngine_ = savedEditorRandom_; editorSession_ = false;
}
#endif
