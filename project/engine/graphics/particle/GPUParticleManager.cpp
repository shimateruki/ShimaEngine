#include <cstdint>
#include "engine/graphics/effect/VfxAssetPath.h"
#include "GpuParticleManager.h"
#include "GpuParticlePresetJsonCodec.h"
#include <filesystem>
#include <fstream>
#include "json.hpp"
#include "DebugConsole.h"
#include "SrvManager.h"
#include <TextureManager.h>
#include "CameraManager.h"
#include <d3d12.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
// GPUパーティクルの描画状態と容量から、共有可能なSystemを識別するキーを作ります。
std::string MakeParticleSystemKey(const GpuParticleConfig& config) {
    // 粒子ごとに持たない描画設定をキーに含め、同じSystemへ異なる材質状態が混ざるのを防ぐ。
    // softParticleFadeや衝突設定など、このキーに含まれない値の共有範囲にも注意する。
    std::ostringstream key;
    key << config.texturePath
        << "_blend" << config.blendModeIndex
        << "_sheet" << config.spriteSheetColumns
        << "x" << config.spriteSheetRows
        << "_frames" << config.spriteSheetFrameCount
        << "_fps" << config.spriteSheetFps
        << "_loop" << config.spriteSheetLoop
        << "_rand" << config.spriteSheetRandomStart
        << "_align" << config.alignToVelocity
        << "_stretch" << config.velocityStretch
        << "_type" << config.particleType
        << "_trail" << config.trailLength
        << "_light" << config.receiveLighting
        << "_lightStrength" << config.lightingStrength
        << "_lightDir" << config.lightDirection.x << ',' << config.lightDirection.y << ',' << config.lightDirection.z
        << "_lightColor" << config.lightColor.x << ',' << config.lightColor.y << ',' << config.lightColor.z
        << "_capacity" << GpuParticleManager::ResolveParticleCapacity(config);
    return key.str();
}
}

GpuParticleManager* GpuParticleManager::GetInstance() {
    static GpuParticleManager sInstance;
    return &sInstance;
}

// 最大数の明示設定を優先し、未指定なら寿命中に重なる放出回数から2の累乗で容量を見積もります。
// LOD上限とGPU Systemの許容範囲を最後に適用します。
uint32_t GpuParticleManager::ResolveParticleCapacity(const GpuParticleConfig& config) {
    const auto applyLodLimit = [&config](uint32_t capacity) {
        // LOD が無効か正の生存上限が無ければ、見積もった容量を使う。
        if (!config.lod.enabled || config.lod.maxAliveParticles <= 0) {
            return capacity;
        }

        const uint32_t lodCapacity = static_cast<uint32_t>(std::clamp(
            config.lod.maxAliveParticles,
            static_cast<std::int32_t>(GpuParticleSystem::kMinParticles),
            static_cast<std::int32_t>(GpuParticleSystem::kMaxParticles)));
        return (std::min)(capacity, lodCapacity);
    };

    // 最大数を明示している設定は、放出回数で見積もらず GPU の許容範囲と LOD で制限する。
    if (config.maxParticles > 0) {
        return applyLodLimit(static_cast<uint32_t>(std::clamp(
            config.maxParticles,
            static_cast<std::int32_t>(GpuParticleSystem::kMinParticles),
            static_cast<std::int32_t>(GpuParticleSystem::kMaxParticles))));
    }

    const uint64_t emitCount = static_cast<uint64_t>((std::max)(config.emitCount, 1));
    uint64_t requiredCapacity = 0;

    // ループ式では、粒子の寿命までに重なる放出回数と予備の2回分から必要数を求める。
    if (config.isLooping) {
        const float interval = (std::max)(config.emitInterval, 1.0f / 60.0f);
        const float life = (std::max)(config.emitLife, 0.0f);
        const uint64_t overlappingEmits = static_cast<uint64_t>(std::ceil(life / interval)) + 2u;
        requiredCapacity = emitCount * overlappingEmits;
    }
    else {
        // 同じTexture/Blendの単発演出が同時に6回重なっても溢れにくい余裕を持たせる。
        requiredCapacity = emitCount * 6u;
    }

    requiredCapacity = (std::max)(
        requiredCapacity,
        static_cast<uint64_t>(GpuParticleSystem::kMinParticles));
    // 必要数が GPU の最大数へ達する場合は、2の累乗へ切り上げる前に上限を使う。
    if (requiredCapacity >= GpuParticleSystem::kMaxParticles) {
        return applyLodLimit(GpuParticleSystem::kMaxParticles);
    }

    uint32_t capacity = GpuParticleSystem::kMinParticles;

    // 必要数に達するまで最小容量から倍増し、システムの最大数より大きくしない。
    while (capacity < requiredCapacity && capacity < GpuParticleSystem::kMaxParticles) {
        capacity *= 2u;
    }

    return applyLodLimit((std::min)(capacity, GpuParticleSystem::kMaxParticles));
}

void GpuParticleManager::Initialize(engine::DirectXCommon* dxCommon) {
    dxCommon_ = dxCommon;

    // テクスチャはScenePreloaderの転送バッチで準備します。
    // Scene初期化のたびに全PNGを同期ロードするとLoadingSceneが停止するため、ここでは読み込みません。
    // プリセットが未登録の初回だけ、共通フォルダーから設定を読み込む。
    if (presets_.empty()) {
        LoadAllPresets("Resources/json/gpu_particles/");
    }
}

void GpuParticleManager::InitializeWithPresets(
    engine::DirectXCommon* dxCommon, const std::vector<std::string>& presetNames) {
    // 呼出し側が指定したプリセットだけを登録する。空の一覧では追加読込を行わない。
    dxCommon_ = dxCommon;

    // 具体的な一覧が指定された場合だけ、その設定を準備する。
    if (!presetNames.empty()) {
        LoadPresetFiles("Resources/json/gpu_particles/", presetNames);
    }
}

// 次フレームの通常更新を一度だけ受け付けられるようにする。
void GpuParticleManager::BeginFrame() {
    updatedThisFrame_ = false;
}

// 1フレーム1回だけ自動Emitterと各GPU Systemを更新し、設定された時間倍率を反映します。
void GpuParticleManager::Update(float deltaTime) {
    // 同じフレームで別の呼出元が更新済みの場合は、粒子と放出時計を二重に進めない。
    if (updatedThisFrame_) {
        return;
    }

    const auto cpuStart = std::chrono::high_resolution_clock::now();
    updatedThisFrame_ = true;

    float scaledDelta = deltaTime * timeScale_;

    // 自動発生の時計を進め、周期を越えたものだけ放出する。
    for (auto& emitter : autoEmitters_) {
        auto it = vfxAssetPath::FindPreset(presets_, emitter.presetName);

        // プリセットを解決でき、継続発生が有効なエミッターだけ時計を進める。
        if (it != presets_.end() && it->second.isLooping) {
            emitter.timer += scaledDelta;

            // 設定間隔に達したフレームで一度放出する。
            if (emitter.timer >= it->second.emitInterval) {
                Emit(emitter.presetName, emitter.position, emitter.transform);
                // 1フレームに一度だけ放出し、間隔を超えた分は次の放出へ繰り越さない。
                emitter.timer = 0.0f;
            }
        }
    }

    // テクスチャ・ブレンドごとの System に同じ時間倍率を渡してから更新します。
    for (auto& pair : systems_) {
        pair.second->SetTimeScale(timeScale_);
        pair.second->Update(deltaTime);
    }

    const auto cpuEnd = std::chrono::high_resolution_clock::now();
    lastUpdateCpuTimeMs_ = std::chrono::duration<float, std::milli>(cpuEnd - cpuStart).count();
}

// シーク中の手動ステップでは、通常更新の同フレーム制限を解除してから刻みを渡す。
void GpuParticleManager::UpdateEditorPreviewStep(float deltaTime) {
    updatedThisFrame_ = false;
    Update(deltaTime);
}

// 通常Systemと表示中の制作用プレビューを、同じディスクリプタヒープで描画します。
void GpuParticleManager::Draw(ID3D12GraphicsCommandList* commandList, const engine::Matrix4x4& viewMatrix, const engine::Matrix4x4& projectionMatrix, uint32_t dummy, uint32_t depthSrvHandle) {
    // 通常系が空で、制作用プレビューも非表示なら描画と計測を省く。
    if (systems_.empty() && !authoringPreviewVisible_) {
        lastDrawCpuTimeMs_ = 0.0f;
        return;
    }

    const auto cpuStart = std::chrono::high_resolution_clock::now();
    dxCommon_->StartGpuProfile("Particle GPU pass");

    // 全 System が使うディスクリプタヒープを一度だけ設定します。
    engine::SrvManager::GetInstance()->SetDescriptorHeaps(commandList);

    for (auto& pair : systems_) {
        // 各 System が自身のテクスチャとブレンド設定を使います。
        pair.second->Draw(commandList, viewMatrix, projectionMatrix, dummy, depthSrvHandle);
    }

    // 制作用プレビューが可視の場合に限り、独立したシステム群を続けて描く。
    if (authoringPreviewVisible_) {
        for (auto& [key, system] : authoringSystems_)
            system->Draw(commandList, viewMatrix, projectionMatrix, dummy, depthSrvHandle);
    }

    dxCommon_->EndGpuProfile("Particle GPU pass");
    const auto cpuEnd = std::chrono::high_resolution_clock::now();
    lastDrawCpuTimeMs_ = std::chrono::duration<float, std::milli>(cpuEnd - cpuStart).count();
}

// 描画設定・容量を含むMakeParticleSystemKeyで共有し、未登録の組み合わせだけ生成する。
GpuParticleSystem* GpuParticleManager::GetOrCreateSystem(const GpuParticleConfig& config) {
    std::string key = MakeParticleSystemKey(config);

    // 同じ描画・容量の組み合わせが未登録の場合にだけ、GPU 資源を準備する。
    if (systems_.find(key) == systems_.end()) {
        auto newSystem = std::make_unique<GpuParticleSystem>();
        newSystem->Initialize(dxCommon_, ResolveParticleCapacity(config));
        systems_[key] = std::move(newSystem);
        DebugConsole::GetInstance()->AddLog("Created new Particle System for: " + key);
    }

    return systems_[key].get();
}

void GpuParticleManager::PreloadPresetSystem(const std::string& presetName) {
    // 初回放出前に使用予定のGPU粒子設定と素材を準備する。
    auto it = vfxAssetPath::FindPreset(presets_, presetName);

    // 未登録のプリセットはテクスチャや容量を確定できないため、診断を出して飛ばす。
    if (it == presets_.end()) {
        DebugConsole::GetInstance()->AddLog(LogLevel::kWarning, "[GPUParticleManager] preload preset not found: " + presetName);
        return;
    }

    GpuParticleSystem* targetSystem = GetOrCreateSystem(it->second);
    targetSystem->SetEmitterMesh(meshVb_, meshVCount_, meshVStride_, meshBoneSrv_);
    targetSystem->SetCurrentTexture(it->second.texturePath);
    targetSystem->RequestWarmup();
}

// シーンが使うPresetだけを列挙してGPU Systemを事前準備します。
void GpuParticleManager::PreloadPresetSystems(const std::vector<std::string>& presetNames) {
    for (const std::string& presetName : presetNames) {
        PreloadPresetSystem(presetName);
    }
}

bool GpuParticleManager::IsEmpty() const {
    // 制作プレビューが表示中なら、通常システムが休止していても描画対象ありとする。
    if (authoringPreviewVisible_)
        return false;
    for (const auto& pair : systems_) {
        // 通常システムのうち、使用中のものが一つでもあれば空とはみなさない。
        if (pair.second && pair.second->IsActive()) {
            return false;
        }
    }

    return true;
}

std::int32_t GpuParticleManager::GetActiveSystemCount() const {
    // 共有済みシステムの総数ではなく、実際に粒子を保持するものだけ数える。
    std::int32_t activeCount = 0;

    for (const auto& pair : systems_) {
        // 存在し、粒子の更新や描画が有効なシステムだけ数える。
        if (pair.second && pair.second->IsActive()) {
            ++activeCount;
        }
    }

    return activeCount;
}

// 同じ構成で作成済みの GPU 資源の推定バイト数を返す。未生成なら0。
size_t GpuParticleManager::GetEstimatedMemoryBytesForConfig(const GpuParticleConfig& config) const {
    const auto it = systems_.find(MakeParticleSystemKey(config));

    // 同じキーにシステムが無い場合は、参照できる確保済み資源を0として扱う。
    if (it == systems_.end() || !it->second) {
        return 0;
    }

    return it->second->GetEstimatedMemoryBytes();
}

bool GpuParticleManager::RequiresSceneColorCopy() const {
    // プレビュー系も通常系も、背景色を参照する描画が一つでもあればコピーを要求する。
    if (authoringPreviewVisible_) for (const auto& [key, system] : authoringSystems_)
        // 可視の独立プレビューの中に、背景色を参照するものがあれば背景コピーが必要。
        if (system && system->RequiresSceneColorCopy())
            return true;
    for (const auto& pair : systems_) {
        // 通常描画のシステムに一つでも必要なものがあれば、背景コピーを有効にする。
        if (pair.second && pair.second->RequiresSceneColorCopy()) {
            return true;
        }
    }

    return false;
}

// カメラ距離によるLODを放出数へ適用し、描画設定を共有するGPU Systemへ放出します。
void GpuParticleManager::EmitFromConfig(const GpuParticleConfig& config) {
    GpuParticleConfig resolvedConfig = config;

    // LOD を使う設定でだけ、距離による発生量と一要求の粒子数上限を調整する。
    if (config.lod.enabled) {
        // 表示中のカメラを取得できた場合に、視点と発生点の距離で倍率を計算する。
        if (const engine::Camera* camera = CameraManager::GetInstance()->GetActiveCamera()) {
            const engine::Vector3 eye = camera->GetEye();
            const float x = config.emitPos.x - eye.x;
            const float y = config.emitPos.y - eye.y;
            const float z = config.emitPos.z - eye.z;
            const float emissionScale = config.lod.EvaluateEmissionScale(std::sqrt(x * x + y * y + z * z));

            // 距離による倍率が事実上0なら、GPU システムを取得する前に放出を中止する。
            if (emissionScale <= 0.0001f) {
                return;
            }

            resolvedConfig.emitCount = (std::max)(
                1,
                static_cast<std::int32_t>(std::lround(static_cast<float>(config.emitCount) * emissionScale)));
        }

        // 生存数の上限が正で指定されていれば、一度に要求する放出数もその範囲に収める。
        if (config.lod.maxAliveParticles > 0) {
            resolvedConfig.emitCount = (std::min)(
                resolvedConfig.emitCount,
                config.lod.maxAliveParticles);
        }
    }

    // GPU側の容量と描画設定に対応するSystemを取得します。放出数だけはLOD適用後の値を渡します。
    GpuParticleSystem* targetSystem = GetOrCreateSystem(config);

    // メッシュ表面から放出するPresetに備え、現在の発生元モデル情報を渡します。
    targetSystem->SetEmitterMesh(meshVb_, meshVCount_, meshVStride_, meshBoneSrv_);
    targetSystem->SetCurrentTexture(config.texturePath);
    targetSystem->EmitFromConfig(resolvedConfig);
}

void GpuParticleManager::Emit(const std::string& presetName, const engine::Vector3& position, const engine::Matrix4x4& emitterWorldMatrix) {
    // 登録済みの設定をコピーし、今回の発生位置と親行列だけ上書きして流す。
    auto it = vfxAssetPath::FindPreset(presets_, presetName);

    // プリセットを解決できた場合に、登録値を保持したまま位置だけを変えて放出する。
    if (it != presets_.end()) {
        GpuParticleConfig config = it->second;
        config.emitPos = position;
        config.emitterWorldMatrix = emitterWorldMatrix;
        EmitFromConfig(config);
    }
}

// プリセットの形状を保ち、方向が有効なときに単位方向と速度倍率を初速へ反映する。
void GpuParticleManager::EmitDirected(
    const std::string& presetName,
    const engine::Vector3& position,
    const engine::Vector3& direction,
    float speedScale,
    const engine::Matrix4x4& emitterWorldMatrix) {
    const auto it = vfxAssetPath::FindPreset(presets_, presetName);

    // 名前に対応する設定が無ければ発生を見送る。
    if (it == presets_.end()) {
        return;
    }

    GpuParticleConfig config = it->second;
    config.emitPos = position;
    config.emitterWorldMatrix = emitterWorldMatrix;

    // 有効な方向がある場合だけ初速の向きを差し替え、速さはプリセットを基準に倍率で調整する。
    const float directionLength = engine::Math::Length(direction);

    // 方向に十分な長さがある場合だけ、プリセットの初速の向きを置き換える。
    if (directionLength > 0.0001f) {
        const float presetSpeed = engine::Math::Length(config.emitVelocity);
        const float resolvedSpeed = (std::max)(presetSpeed, 0.01f) * (std::max)(speedScale, 0.0f);
        config.emitVelocity = direction / directionLength * resolvedSpeed;
    }

    EmitFromConfig(config);
}

// ====================================================================
// プリセットの登録と自動エミッターの管理
// ====================================================================
void GpuParticleManager::LoadAllPresets(const std::string& directoryPath) {
    LoadPresetFiles(directoryPath, {});
}

// 指定された設定だけ、または空の一覧ならフォルダー配下をまとめて登録する。
void GpuParticleManager::LoadPresetFiles(
    const std::string& directoryPath, const std::vector<std::string>& presetNames) {
    const bool loadAll = presetNames.empty();
    std::string normalizedDirectory = directoryPath;
    std::replace(normalizedDirectory.begin(), normalizedDirectory.end(), '\\', '/');

    // 同じディレクトリの全件を読み込み済みの場合は、再走査を省く。
    if (loadAll && !presets_.empty() && loadedPresetDirectory_ == normalizedDirectory) {
        return;
    }

    // 対象フォルダーが無ければ空の保存先を用意し、今回の登録は行わない。
    if (!fs::exists(directoryPath)) {
        fs::create_directories(directoryPath);
        return;
    }
    std::vector<fs::path> presetPaths;

    // 全件指定では、下位フォルダーも走査してファイル候補を集める。
    if (loadAll) {
        for (const auto& entry : fs::recursive_directory_iterator(directoryPath)) {
            // 読み込み候補には通常ファイルだけを含める。
            if (entry.is_regular_file()) presetPaths.push_back(entry.path());
        }
    // 一覧指定では、要求されたプリセット名から素材のファイルを解決する。
    } else {
        for (const std::string& name : presetNames) {
            fs::path root(directoryPath);
            // 末尾に区切りがあるパスは、ディレクトリ自体を指す形に正規化する。
            if (!root.has_filename()) root = root.parent_path();
            const auto requested = root / fs::path(std::u8string(name.begin(), name.end()) + u8".json");
            presetPaths.push_back(vfxAssetPath::Resolve(root, requested));
        }
    }

    for (const fs::path& presetPath : presetPaths) {
        // 候補のうち .json のファイルだけをプリセットとして開く。
        if (presetPath.extension() == ".json") {
            fs::path root(directoryPath);
            // キーの相対基準を同じディレクトリに揃えるため、末尾が区切りだけの場合は除く。
            if (!root.has_filename()) root = root.parent_path();
            auto relative = presetPath.lexically_relative(root);
            // プリセット名の衝突を避けるため、拡張子を外した相対パスで登録する。
            const auto key = relative.replace_extension().generic_u8string();
            const std::string filename(key.begin(), key.end());
            std::ifstream file(presetPath);

            // ファイルを開けた場合だけ、共通コーデックから設定を読み込んで登録する。
            if (file.is_open()) {
                json j;
                file >> j;
                GpuParticleConfig config;
                gpuParticlePresetJsonCodec::Read(j, config);

                // 実行時だけの描画モード互換補正は、Editorの読込とは分けて維持する。
                if (config.blendModeIndex >= 2) {
                    config.blendModeIndex = 1;
                }

                presets_[filename] = config;
                DebugConsole::GetInstance()->AddLog("Loaded Particle Preset: " + filename);
            }
        }
    }

    // 全件を処理した場合だけ、以降の全件要求を省けるよう読み込んだ場所を記録する。
    if (loadAll) {
        loadedPresetDirectory_ = normalizedDirectory;
    }
}

// 全件読込の記録と定義を消し、ディスクの設定から登録をやり直す。
void GpuParticleManager::ReloadAllPresets(const std::string& directoryPath) {
    presets_.clear();
    loadedPresetDirectory_.clear();
    LoadAllPresets(directoryPath);
}

// 発生元の変形を指定しない場合は単位行列で自動発生を登録する。
uint32_t GpuParticleManager::PlayAutoEmitter(const std::string& presetName, const engine::Vector3& position) {
    engine::Matrix4x4 identity = { 1.0f,0.0f,0.0f,0.0f, 0.0f,1.0f,0.0f,0.0f, 0.0f,0.0f,1.0f,0.0f, 0.0f,0.0f,0.0f,1.0f };
    return PlayAutoEmitter(presetName, position, identity);
}

uint32_t GpuParticleManager::PlayAutoEmitter(const std::string& presetName, const engine::Vector3& position, const engine::Matrix4x4& transform) {
    // 登録時に一回発生させ、更新ループの次回周期まで何も表示されない時間を作らない。
    AutoEmitter em;
    em.id = nextAutoEmitterId_++;
    em.presetName = presetName;
    em.position = position;
    em.transform = transform;
    em.timer = 0.0f;
    autoEmitters_.push_back(em);
    Emit(presetName, position, transform);
    return em.id;
}

// 指定IDの今後の放出だけ止める。既に放出した粒子はGPU Systemで寿命まで表示する。
void GpuParticleManager::StopAutoEmitter(uint32_t id) {
    autoEmitters_.erase(std::remove_if(autoEmitters_.begin(), autoEmitters_.end(), [id](const AutoEmitter& e) { return e.id == id; }), autoEmitters_.end());
}

// 自動発生だけを止める。放出済みの粒子のリセットは別経路で行う。
void GpuParticleManager::ClearAllAutoEmitters() { autoEmitters_.clear(); }

// 自動Emitterを破棄し、各GPU Systemの初期化を次回更新へ予約します。
void GpuParticleManager::ResetSimulation() {
    autoEmitters_.clear();

    for (auto& [key, system] : systems_) {
        (void)key;

        // GPU システムを生成済みの場合に、次回更新での初期化を予約する。
        if (system) {
            system->RequestSimulationReset();
        }
    }

    updatedThisFrame_ = false;
}

// シーン固有のEmitterと借用メッシュ参照を解除し、GPU Systemを次シーン用に戻します。
void GpuParticleManager::ClearSceneRuntime() {
    ClearAuthoringPreview();
    autoEmitters_.clear();

    for (auto& [key, system] : systems_) {
        (void)key;

        // 生存中のシステムだけ、旧シーンのメッシュとシミュレーション状態を外す。
        if (system) {
            // 旧シーンのModelバッファを次のシーンへ持ち越さない。
            system->SetEmitterMesh(nullptr, 0, 0, 0);
            system->ResetForSceneTransition();
        }
    }

    nextAutoEmitterId_ = 0;
    timeScale_ = 1.0f;
    updatedThisFrame_ = false;
    lastUpdateCpuTimeMs_ = 0.0f;
    lastDrawCpuTimeMs_ = 0.0f;
    meshVb_ = nullptr;
    meshVCount_ = 0;
    meshVStride_ = 0;
    meshBoneSrv_ = 0;
}

// 制作用の表示は直ちに止め、各Systemの粒子状態を次に使えるよう戻す。System自体は保持する。
void GpuParticleManager::ClearAuthoringPreview() {
    authoringPreviewVisible_ = false;

    for (auto& [key, system] : authoringSystems_)
        system->ResetForSceneTransition();
}

// 制作用の独立SystemでPresetを指定年齢まで進め、通常再生中のSystemへ状態を混ぜません。
bool GpuParticleManager::ShowAuthoringPreview(const std::string& preset, const engine::Vector3& position,
    const engine::Vector3& direction, bool directed, float speedScale, float age) {
    const auto found = vfxAssetPath::FindPreset(presets_, preset);
    // 描画管理の準備ができていないか、指定プリセットが登録されていなければ表示できない。
    if (!dxCommon_ || found == presets_.end())
        return false;
    auto config = found->second;

    // メッシュ頂点発生には所有モデルが必要です。他Objectの頂点バッファを借りて誤表示しません。
    if (config.shapeType == 3)
        return false;
    config.emitPos = position;
    config.emitterWorldMatrix = engine::Math::MakeIdentity4x4();
    // 明示的に方向を指定し、単位化できる長さがある場合だけ初速を上書きする。
    if (directed && engine::Math::Length(direction) > 0.0001f)
        config.emitVelocity = direction / engine::Math::Length(direction) *
            ((std::max)(0.01f, engine::Math::Length(config.emitVelocity)) * (std::max)(0.0f, speedScale));

    // 配置では素材ごとに分離し、補助素材の設定や放出年齢が主素材へ混ざるのを防ぎます。
    const auto key = preset + "|" + MakeParticleSystemKey(config);
    auto& system = authoringSystems_[key];
    const bool created = !system;

    // 独立したプレビュー用キーに資源が無い場合だけ、対応する容量で初期化する。
    if (!system) {
        system = std::make_unique<GpuParticleSystem>();
        system->Initialize(dxCommon_, ResolveParticleCapacity(config));
    }

    system->SetCurrentTexture(config.texturePath);
    system->SetTimeScale(1);
    const bool firstEmission = created || !system->IsActive();
    system->EmitFromConfig(config);

    // GPU側は次のDrawで経過時間をまとめて消費します。固定刻みの再現とは区別します。
    if (firstEmission)
        system->Update(std::clamp(age, 0.0f, 5.0f));
    authoringPreviewVisible_ = true;
    return true;
}

#ifdef USE_IMGUI
void GpuParticleManager::BeginEditorSession() {
    // すでにプレビューへ切替済みなら、退避先と通常用システムを再交換しない。
    if (editorSession_) return;
    // 通常再生とプレビューでGPUの粒子バッファを分け、通常再生の時間倍率と借用メッシュも退避する。
    savedEditorScale_ = timeScale_;
    savedEditorMesh_ = meshVb_; savedEditorMeshCount_ = meshVCount_; savedEditorMeshStride_ = meshVStride_; savedEditorMeshBones_ = meshBoneSrv_;
    systems_.swap(savedEditorSystems_); autoEmitters_.swap(savedEditorEmitters_);
    // 前回の確認用GPUバッファは再利用し、開始前のバッファには触れない。
    ResetSimulation(); timeScale_ = 1; updatedThisFrame_ = false; editorSession_ = true;
}
void GpuParticleManager::EndEditorSession() {
    // 終了時はプレビューの放出停止を予約してから、退避した通常再生のバッファを戻す。
    if (!editorSession_) return;
    ResetSimulation();
    systems_.swap(savedEditorSystems_); autoEmitters_.swap(savedEditorEmitters_);
    timeScale_ = savedEditorScale_; updatedThisFrame_ = false; editorSession_ = false;
    SetEmitterMesh(savedEditorMesh_, savedEditorMeshCount_, savedEditorMeshStride_, savedEditorMeshBones_);
}
#endif
