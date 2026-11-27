#include <cstdint>
#include "engine/graphics/effect/VfxAssetPath.h"
#include "MeshEffectManager.h"
#include "MeshEffectTarget.h"
#include <numbers>
#include "ModelManager.h"
#include "TextureManager.h"
#include "IconsFontAwesome5.h"
#include "json.hpp"
#include <fstream>
#include <DebugConsole.h>
#include "SceneManager.h"
#include "BaseScene.h"
#include <exception>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <CollisionManager.h>
using json = nlohmann::json;

engine::Object3D* FindMeshEffectTarget(BaseScene* scene, const std::string& name) {
    // シーンか名前がない場合は、追従先を探せないため空の参照を返す。
    if (!scene || name.empty()) {
        return nullptr;
    }

    // シーンのルートから子の順に探索するため、最上位に並ばないボーンや子Objectも名前で解決できます。
    const auto findInHierarchy = [&name](const auto& self, engine::Object3D* object) -> engine::Object3D* {
        // 空の枝では空を返し、名前が一致した枝ではそのオブジェクトを返す。
        if (!object || object->GetName() == name) {
            return object;
        }
        for (auto* child : object->GetChildren()) {
            // 子の階層で名前が一致したら、残りの子の探索を止める。
            if (auto* found = self(self, child)) {
                return found;
            }
        }
        return nullptr;
    };

    for (const auto& object : scene->GetObjects()) {
        // いずれかのルートから一致するものを見つけたら、最初の実体を採用する。
        if (auto* found = findInHierarchy(findInHierarchy, object.get())) {
            return found;
        }
    }
    return nullptr;
}

namespace {
// 解析済みのプリセットと取得できた更新時刻。時刻が一致するときだけ再読込を省く。
struct EffectJsonCacheEntry {
    json data;
    std::filesystem::file_time_type writeTime{};
    bool hasWriteTime = false;
};

bool LoadEffectJsonCached(const std::string& jsonFilePath, json& outJson) {
    // 更新時刻が同じ設定は解析済みJSONを再利用し、エフェクト生成時のI/Oを減らす。
    static std::unordered_map<std::string, EffectJsonCacheEntry> sCache;

    std::error_code ec;
    const auto path = vfxAssetPath::ResolveDependency(std::filesystem::path(std::u8string(jsonFilePath.begin(), jsonFilePath.end())));
    const auto writeTime = std::filesystem::last_write_time(path, ec);
    const bool hasWriteTime = !ec;

    auto it = sCache.find(jsonFilePath);

    // キャッシュと現在のファイルの両方で更新時刻を確認でき、その値が同じ場合だけ解析結果を使う。
    if (it != sCache.end() && hasWriteTime && it->second.hasWriteTime && it->second.writeTime == writeTime) {
        outJson = it->second.data;
        return true;
    }

    // キャッシュを使えない場合は、解決した依存パスからプリセットを読み込み直します。
    std::ifstream file(path);

    // プリセットを開けなければ、呼び出し元にロード失敗を返す。
    if (!file.is_open()) {
        return false;
    }

    json loaded;
    file >> loaded;

    // 解析を終えた内容と取得時刻を記録し、次の生成で再利用できるようにします。
    EffectJsonCacheEntry entry;
    entry.data = loaded;
    entry.writeTime = writeTime;
    entry.hasWriteTime = hasWriteTime;
    sCache[jsonFilePath] = entry;

    outJson = std::move(loaded);
    return true;
}
}

MeshEffectManager* MeshEffectManager::GetInstance() {
    static MeshEffectManager sInstance;
    return &sInstance;
}

void MeshEffectManager::Initialize(engine::Object3DCommon* common) {
    // エフェクト自身も生成時のcommonを保持します。管理側のポインタだけを
    // 差し替えると、前Sceneの描画基盤を参照したエフェクトが描画されます。
    if (common_ != common)
        Clear();
    common_ = common;
    timeScale_ = 1.0f;
}

void MeshEffectManager::ClearActiveEffects() {
    // 所有物を消す前に、衝突管理とデバッグ表示の非所有参照を解除します。
    for (const auto& effect : activeEffects_) {
        // 編集途中で判定フラグをOFFにした場合も、以前の登録を残しません。
        // 所有している実体がある場合は、衝突と再利用の管理から参照を外します。
        if (effect) {
            engine::CollisionManager::GetInstance()->RemoveObject(effect.get());
            reusableEffects_.erase(effect.get());
        }
    }

    previewEffectForDebug_ = nullptr;
    activeEffects_.clear();
    effectScopes_.clear();
    timelineScopes_.clear();
    timelineOffsets_.clear();
    updatedThisFrame_ = false;
}

// 描画基盤の切り替え時に使う。再生中の参照を外してから、プールと共有形状を解放する。
void MeshEffectManager::Clear() {
    ClearActiveEffects();
    recycledEffects_.clear();
    reusableEffects_.clear();
    preloadedEffects_.clear();
    EffectObject3D::ClearProceduralMeshCache();
    common_ = nullptr;
    timeScale_ = 1.0f;
}

void MeshEffectManager::RecycleEffect(std::unique_ptr<EffectObject3D> effect) {
    // 実体がない場合は、再利用の登録やプレビューの参照を確認する必要がない。
    if (!effect) return;
    // 再利用可能と登録された個体だけを検討し、その登録は移動・破棄の前に消します。
    const bool reusable = reusableEffects_.erase(effect.get()) != 0;
    // 編集対象の個体は回収せず、未保存の変更が他の演出へ混ざるのを防ぎます。
    if (previewEffectForDebug_ == effect.get()) {
        previewEffectForDebug_ = nullptr;
        return;
    }
    // 再利用が許可され、衝突と親子関係を持たず、空きがある個体だけを再利用リストへ戻す。
    if (reusable && !effect->GetMeshSettings().hasCollision && !effect->GetParent() &&
        recycledEffects_.size() < kMaxRecycledEffects) {
        engine::CollisionManager::GetInstance()->RemoveObject(effect.get());
        recycledEffects_.push_back(std::move(effect));
    }
}

// 新しいフレームの最初に、通常更新を一度だけ受け付ける状態へ戻します。
void MeshEffectManager::BeginFrame() {
    updatedThisFrame_ = false;
}

void MeshEffectManager::PreloadEffect(const std::string& jsonFilePath) {
    // 同じ設定を複数回GPUへ準備しないよう、ロード済みパスを先に確認する。
    if (preloadedEffects_.find(jsonFilePath) != preloadedEffects_.end()) {
        return;
    }

    // 共有描画基盤が未接続なら、現在のシーンから借りられるか調べる。
    if (!common_) {
        SceneManager* sm = SceneManager::GetInstance();

        // シーン管理と現在のシーンの両方が存在する場合に、共有3D基盤を取得する。
        if (sm && sm->GetCurrentScene()) {
            common_ = sm->GetCurrentScene()->GetObject3DCommon();
        }
    }

    // 基盤を取得できない場合は、プリセットから描画資源を作成できない。
    if (!common_) {
        return;
    }

    json j;
    try {
        // プリセットを開けなかった場合は、先読みを止めてパス付きの診断を出す。
        if (!LoadEffectJsonCached(jsonFilePath, j)) {
            DebugConsole::GetInstance()->AddLog(LogLevel::kWarning, "[MeshEffectManager] PreloadEffect: failed to open " + jsonFilePath);
            return;
        }
    } catch (const std::exception& e) {
        DebugConsole::GetInstance()->AddLog(LogLevel::kError, "[MeshEffectManager] PreloadEffect JSON error: " + jsonFilePath + " / " + e.what());
        return;
    }

    // プリセットから値を一つも取得できない場合も、先読み済みとして扱わない。
    if (j.empty()) {
        DebugConsole::GetInstance()->AddLog(LogLevel::kWarning, "[MeshEffectManager] PreloadEffect: failed to open " + jsonFilePath);
        return;
    }

    auto effect = std::make_unique<EffectObject3D>();

    // 設定ファイルに値がない項目は安全な初期値に戻してから、存在する項目だけ適用する。
    effect->Initialize(common_);
    effect->SetProceduralType(0);
    effect->SetEnableNoiseTexture(false);
    effect->SetEnableColorRamp(false);

    // テクスチャ色を使う設定が保存されている場合は、先読み個体にも適用する。
    if (j.contains("UseTextureColor")) {
        effect->SetUseTextureColor(j["UseTextureColor"].get<bool>());
    }

    effect->SetEnableDistortion(false);
    effect->SetEnableReveal(true);
    effect->SetDistortionStrength(0.0f);
    effect->SetEdgeFadeStrength(1.0f);

    // モデル名の項目がある場合だけ、その名前を読み取る。
    if (j.contains("ModelName")) {
        const std::string modelName = j["ModelName"].get<std::string>();

        // 空のモデル名は、先読みでモデルを読み込まない指定として扱う。
        if (!modelName.empty()) {
            effect->SetModel(modelName);
        }
    }

    // 通常画像のパスが保存されている場合は、表示用のメッシュへ渡せるか調べる。
    if (j.contains("TexturePath")) {
        const std::string texturePath = j["TexturePath"].get<std::string>();

        // 空文字でなくメッシュの描画先が存在する場合に、通常画像を設定する。
        if (!texturePath.empty() && effect->GetMeshRenderer()) {
            effect->GetMeshRenderer()->SetTexture(texturePath);
        }
    }

    // ノイズ画像の設定があるときは、線形テクスチャとして扱う。
    if (j.contains("NoiseTexturePath")) {
        const std::string texturePath = j["NoiseTexturePath"].get<std::string>();

        // ノイズパスが空の場合は、ノイズ画像をロードしない。
        if (!texturePath.empty()) {
            effect->SetNoiseTexture(engine::TextureManager::GetInstance()->Load(
                texturePath, engine::TextureManager::TextureColorSpace::kLinear));
            effect->SetEnableNoiseTexture(true);
        }
    }

    // 色のランプが指定された場合は、表示色として読み込む。
    if (j.contains("RampTexturePath")) {
        const std::string texturePath = j["RampTexturePath"].get<std::string>();

        // ランプのパスが空の場合は、画像の登録を行わない。
        if (!texturePath.empty()) {
            effect->SetRampTexture(engine::TextureManager::GetInstance()->Load(
                texturePath, engine::TextureManager::TextureColorSpace::kSRGB));
            effect->SetEnableColorRamp(true);
        }
    }

    // 形状種別の指定があれば、保存値を先読みの個体に反映する。
    if (j.contains("ProceduralType")) {
        // プロシージャル形状だけは全パラメータを反映した後に一度生成する。
        std::int32_t procType = j["ProceduralType"];
        effect->SetProceduralType(procType);

        // 手続き形状の番号の場合だけ、寸法を読み込んでからメッシュを生成する。
        if (procType >= 1) {
            effect->ReadProceduralMeshSettings(j);
            effect->UpdateProceduralMesh();
        }
    }

    // プールに空きがあれば、準備した描画バッファも後続の単発エフェクトに再利用します。
    if (recycledEffects_.size() < kMaxRecycledEffects)
        recycledEffects_.push_back(std::move(effect));
    preloadedEffects_.insert(jsonFilePath);
}

// 同じフレームの二重更新を避け、タイムライン管理外のエフェクトの寿命を進めます。
void MeshEffectManager::Update(float deltaTime) {
    // このフレームに進めた個体は、同じ寿命を二回進めないよう次回の呼び出しを無視する。
    if (updatedThisFrame_) {
        return;
    }

    updatedThisFrame_ = true;
    deltaTime *= timeScale_;

    // 通常再生の個体は寿命を進め、停止済みの個体は次の更新時に回収します。
    for (auto it = activeEffects_.begin(); it != activeEffects_.end();) {
        const auto scope = effectScopes_.find(it->get());

        // タイムラインに属する個体は、寿命と停止時刻の管理をタイムライン側に任せる。
        if (scope != effectScopes_.end() && timelineScopes_.count(scope->second)) {
            ++it; // 寿命も攻撃側が管理するので、一時停止中に勝手に消さない。
            continue;
        }

        // 再生を終えた個体は、衝突登録やスコープを解放してから回収する。
        if (!(*it)->IsPlaying()) {
            EffectObject3D* finishedEffect = it->get();

            // 終了した個体が衝突を持つ場合は、破棄前に衝突管理から外す。
            if ((*it)->GetMeshSettings().hasCollision) {
                engine::CollisionManager::GetInstance()->RemoveObject(it->get());
            }

            // 再生が終了したエフェクトは、所有スコープの追跡情報も同時に破棄します。
            effectScopes_.erase(finishedEffect);

            RecycleEffect(std::move(*it));
            it = activeEffects_.erase(it);
        }
        // まだ再生中の個体は、経過秒数を進めて次の要素へ移る。
        else {
            (*it)->Update(deltaTime);
            (*it)->UpdateLocalMatrix();
            (*it)->UpdateWorldMatrix();
            ++it;
        }
    }
}

// Editorの単独ステップではフレーム済みフラグを戻してから更新します。
void MeshEffectManager::UpdateEditorPreviewStep(float deltaTime) {
    updatedThisFrame_ = false;
    Update(deltaTime);
}

void MeshEffectManager::Draw(ID3D12Resource* pLight, ID3D12Resource* sLight) {
    for (auto& effect : activeEffects_) {
        // スコープ付き演出は、シーンのカメラ更新後に地形と同じ視点の描画行列を使う。
        // エディター再生では演出の更新が先に走るため、補間中の古い行列では輪が床に隠れる。
        if (effectScopes_.contains(effect.get())) {
            effect->RefreshRenderCameraData();
        }

        effect->Draw(pLight, sLight);
    }
}

// 再生中の歪みエフェクトがあれば、背景色テクスチャの複製が必要と返します。
bool MeshEffectManager::RequiresSceneColorCopy() const {
    return std::any_of(
        activeEffects_.begin(),
        activeEffects_.end(),
        [](const std::unique_ptr<EffectObject3D>& effect) {
            return effect && effect->IsPlaying() && effect->IsDistortionEnabled();
        });
}

// 個別停止に使うIDを払い出し、無効値は飛ばします。
MeshEffectManager::EffectScopeId MeshEffectManager::CreateEffectScope() {
    EffectScopeId scopeId = nextEffectScopeId_++;

    // 確保したIDが無効値に当たった場合は、その値をスコープへ割り当てない。
    if (scopeId == kInvalidEffectScope) {
        scopeId = nextEffectScopeId_++;
    }

    return scopeId;
}

void MeshEffectManager::StopEffectScope(EffectScopeId scopeId) {
    // 停止対象が無効なIDなら、個体とタイムラインの登録に触れません。
    if (scopeId == kInvalidEffectScope) {
        return;
    }

    // 指定IDのタイムライン管理を解除し、同じIDに登録された個体だけを回収します。
    timelineScopes_.erase(scopeId);

    for (auto it = activeEffects_.begin(); it != activeEffects_.end();) {
        EffectObject3D* effect = it->get();
        const auto scopeIt = effectScopes_.find(effect);

        // 登録のない個体や異なるスコープの個体は、現在の停止要求の対象外とする。
        if (scopeIt == effectScopes_.end() || scopeIt->second != scopeId) {
            ++it;
            continue;
        }

        // 停止対象が衝突付きなら、表示を消す前に当たり判定も外す。
        if (effect && effect->GetMeshSettings().hasCollision) {
            engine::CollisionManager::GetInstance()->RemoveObject(effect);
        }

        effectScopes_.erase(scopeIt);
        timelineOffsets_.erase(effect);
        RecycleEffect(std::move(*it));
        it = activeEffects_.erase(it);
    }
}

// 同じScopeの旧演出を止めてから生成し、攻撃判定は別トラックに任せます。
// 生成に失敗した場合は追加した演出を破棄してfalseを返します。
bool MeshEffectManager::StartTimelineEffect(EffectScopeId scopeId, const std::string& path) {
    // 有効なスコープを渡されなければ、タイムライン用の個体を生成しない。
    if (scopeId == kInvalidEffectScope)
        return false;
    StopEffectScope(scopeId);
    // 生成前の個数を記録し、例外で止まった場合は今回追加した末尾だけを取り除きます。
    const size_t firstEffect = activeEffects_.size();
    try {
        SpawnEffectAtScoped(scopeId, path, {}, {}, {1, 1, 1});
    } catch (const std::exception& exception) {
        while (activeEffects_.size() > firstEffect) {
            effectScopes_.erase(activeEffects_.back().get());
            reusableEffects_.erase(activeEffects_.back().get());
            activeEffects_.pop_back();
        }

        StopEffectScope(scopeId);
        DebugConsole::GetInstance()->AddLog(LogLevel::kError, "[AttackTimeline] " + path + ": " + exception.what());
        return false;
    }

    bool found = false;

    for (auto& effect : activeEffects_) {
        const auto it = effectScopes_.find(effect.get());
        // 生成済みの個体から、このタイムラインへ登録されたものだけを初期化する。
        if (it == effectScopes_.end() || it->second != scopeId)
            continue;

        // 表示物のプリセットに衝突設定があっても、判定は専用トラックだけが担当します。
        if (effect->GetMeshSettings().hasCollision)
            engine::CollisionManager::GetInstance()->RemoveObject(effect.get());
        effect->SetMeshSetting(&EffectObject3D::MeshSettings::hasCollision, false);
        // タイムラインの座標を優先させるため、個体側の対象追従と自動ループを止めて素材内の相対姿勢だけを控える。
        effect->SetTargetObject(nullptr);
        effect->SetAutoLoop(false);
        timelineOffsets_[effect.get()] = {effect->GetTranslate(), effect->GetRotation()};
        found = true;
    }

    // 対象のスコープに表示物を見つけた場合だけ、タイムライン側で駆動するスコープとして登録する。
    if (found)
        timelineScopes_.insert(scopeId);
    return found;
}

void MeshEffectManager::EvaluateTimelineEffect(EffectScopeId scopeId, float time, float duration,
    const engine::Vector3& position, const engine::Vector3& rotation, const engine::Vector3& scale,
    bool preservePresetScale) {
    // タイムライン駆動へ登録済みのスコープだけ、時刻指定による姿勢変更を受ける。
    if (!timelineScopes_.count(scopeId))
        return;
    for (auto& effect : activeEffects_) {
        const auto it = effectScopes_.find(effect.get());
        // 別スコープの個体へタイムラインの時刻や姿勢を混ぜない。
        if (it == effectScopes_.end() || it->second != scopeId)
            continue;
        // 時刻と尺は秒です。差分の積算ではなく再生直後からの指定秒で評価するため、逆向きのシークでも同じ姿勢になります。
        const float resolvedDuration = duration > 0.0f ? duration : effect->GetLifetime();
        effect->Play(resolvedDuration);
        effect->Update(std::clamp(time, 0.0f, (std::max)(0.0f, resolvedDuration)));

        // プリセット内部の位置・スケールアニメーションに重ねず、制作者のキーを優先します。
        const auto& offset = timelineOffsets_.at(effect.get());

        // VolumeModeの交差面・厚み用の相対配置を残して、部品全体を動かします。
        effect->SetTranslate(engine::Math::Transform(offset.position, engine::Math::MakeAffineMatrix(scale, rotation, position)));
        effect->SetRotation(rotation + offset.rotation);
        // プリセットの補間結果を残す設定では各軸にキーの倍率を掛け、上書きする設定ではキーの倍率を直接適用します。
        const auto presetScale = effect->GetScale();
        effect->SetScale(preservePresetScale ? engine::Vector3{
            presetScale.x * scale.x, presetScale.y * scale.y, presetScale.z * scale.z} : scale);
        effect->UpdateLocalMatrix();
        effect->UpdateWorldMatrix();
    }
}

namespace {
// 追従生成と座標指定生成で共有する描画設定。scaleはプリセットの開始・終了サイズへ掛ける倍率。
void ApplySpawnVisualSettings(EffectObject3D& effect, nlohmann::json& j, const engine::Vector3& scale) {
    // 再利用個体にも同じ初期状態を適用してから、プリセットで指定した値を反映する。
    effect.SetProceduralType(0);
    effect.SetEnableNoiseTexture(false);
    effect.SetEnableColorRamp(false);
    effect.SetEnableDistortion(false);
    effect.SetEnableReveal(true);
    effect.SetDistortionStrength(0.0f);
    effect.SetEdgeFadeStrength(1.0f);

    // モデル名が指定されている場合は、エフェクト用の表示モデルを割り当てます。
    if (j.contains("ModelName"))
        effect.SetModel(j["ModelName"].get<std::string>());
    // テクスチャパスが保存されていれば、通常画像の切り替えを検討する。
    if (j.contains("TexturePath")) {
        std::string tp = j["TexturePath"];
        // 非空のパスとメッシュ描画先が揃う場合だけ、テクスチャを設定する。
        if (!tp.empty() && effect.GetMeshRenderer())
            effect.GetMeshRenderer()->SetTexture(tp);
    }
    // ノイズ画像の項目を保存している場合に、そのパスを取り出す。
    if (j.contains("NoiseTexturePath")) {
        std::string np = j["NoiseTexturePath"];
        // ノイズ画像のパスが有効な指定なら、線形で読み、ノイズ参照も有効にする。
        if (!np.empty()) {
            effect.SetNoiseTexture(engine::TextureManager::GetInstance()->Load(np, engine::TextureManager::TextureColorSpace::kLinear));
            effect.SetEnableNoiseTexture(true);
        }
    }
    // ランプ画像の項目がある場合だけ、色変化用のパスを取り出す。
    if (j.contains("RampTexturePath")) {
        std::string rp = j["RampTexturePath"];
        // ランプ画像を指定している場合は、sRGBで登録して色変化を有効にする。
        if (!rp.empty()) {
            effect.SetRampTexture(engine::TextureManager::GetInstance()->Load(rp, engine::TextureManager::TextureColorSpace::kSRGB));
            effect.SetEnableColorRamp(true);
        }
    }
    // 生成側の倍率を開始・終了の両方へ掛けることで、プリセット内の拡縮の変化も維持する。
    if (j.contains("StartScale"))
        effect.SetStartScale({ j["StartScale"][0] * scale.x, j["StartScale"][1] * scale.y, j["StartScale"][2] * scale.z });
    // 終了倍率も保存されている場合は、生成側のXYZ倍率を掛ける。
    if (j.contains("EndScale"))
        effect.SetEndScale({ j["EndScale"][0] * scale.x, j["EndScale"][1] * scale.y, j["EndScale"][2] * scale.z });
    // 開始色が保存されたプリセットには、そのRGBAを再生開始時の値として使う。
    if (j.contains("StartColor"))
        effect.SetStartColor({ j["StartColor"][0], j["StartColor"][1], j["StartColor"][2], j["StartColor"][3] });
    // 終了色が保存されていれば、再生終了時に到達するRGBAを設定する。
    if (j.contains("EndColor"))
        effect.SetEndColor({ j["EndColor"][0], j["EndColor"][1], j["EndColor"][2], j["EndColor"][3] });
    // UVの移動量が保存されていれば、U・Vの両方向へ反映する。
    if (j.contains("ScrollSpeed"))
        effect.SetScrollSpeed({ j["ScrollSpeed"][0], j["ScrollSpeed"][1] });
    // 強度を指定された場合だけ、発光に使う値を上書きする。
    if (j.contains("Intensity"))
        effect.SetIntensity(j["Intensity"]);
    // テクスチャ自身の色を使うか指定された場合だけ、そのフラグを反映する。
    if (j.contains("UseTextureColor"))
        effect.SetUseTextureColor(j["UseTextureColor"].get<bool>());
    // 歪みの強さが指定されていれば、背景をずらす量に使う。
    if (j.contains("DistortionStrength"))
        effect.SetDistortionStrength(j["DistortionStrength"]);
    // 歪みの速さが指定されていれば、パターンの変化速度に使う。
    if (j.contains("DistortionSpeed"))
        effect.SetDistortionSpeed(j["DistortionSpeed"]);
    // 縁の減衰が指定されていれば、輪郭のぼかし方を切り替える。
    if (j.contains("EdgeFadeStrength"))
        effect.SetEdgeFadeStrength(j["EdgeFadeStrength"]);
    // 歪みを有効にする設定が保存されていれば、そのフラグを反映する。
    if (j.contains("EnableDistortion"))
        effect.SetEnableDistortion(j["EnableDistortion"]);
    // 合成方式の番号があれば、描画側が使う合成方式へ変換する。
    if (j.contains("BlendMode"))
        effect.SetBlendMode(static_cast<BlendMode>(j["BlendMode"].get<std::int32_t>()));
    // 旧形式の出現フラグがあれば、基本の出現方式として適用する。
    if (j.contains("EnableReveal"))
        effect.SetEnableReveal(j["EnableReveal"]);
    // 出現方式の番号が保存されている場合は、直前の旧形式のフラグより優先する。
    if (j.contains("RevealMode"))
        effect.SetRevealMode(j["RevealMode"]);
    // 補間方式を指定されていれば、色や大きさを進める曲線を変更する。
    if (j.contains("EasingType"))
        effect.SetEasingType(j["EasingType"]);
    // アルファのしきい値を指定されていれば、その値以下のピクセルをシェーダーで破棄する。
    if (j.contains("AlphaReference"))
        effect.SetAlphaReference(j["AlphaReference"]);
}
}

void MeshEffectManager::SpawnEffect(const std::string& jsonFilePath, engine::Object3D* baseObject, const engine::Vector3& extOffset, const engine::Vector3& extRot, const engine::Vector3& extScale) {
    // 描画基盤が未接続の場合は、現在のシーンから借りる。
    if (!common_) {
        SceneManager* sm = SceneManager::GetInstance();

        // シーン管理と現在のシーンを取得できる場合に限り、3D描画基盤を取り出す。
        if (sm && sm->GetCurrentScene()) {
            common_ = sm->GetCurrentScene()->GetObject3DCommon();
        }
    }

    // 基盤が取得できないままなら、表示物を作る前に失敗として終了する。
    if (!common_) {
        DebugConsole::GetInstance()->AddLog(LogLevel::kError, "[MeshEffectManager] SpawnEffect ABORT: common_ is null!");
        return;
    }

    // 描画基盤を確定してから設定を読み、失敗時には半端なエフェクトを登録しません。
    json j;
    try {
        // プリセットを開けない場合は、発生位置の計算や個体生成へ進まない。
        if (!LoadEffectJsonCached(jsonFilePath, j)) {
            DebugConsole::GetInstance()->AddLog(LogLevel::kError, "[MeshEffectManager] Failed to open effect json: " + jsonFilePath);
            return;
        }
    } catch (const std::exception& e) {
        DebugConsole::GetInstance()->AddLog(LogLevel::kError, "[MeshEffectManager] SpawnEffect JSON error: " + jsonFilePath + " / " + e.what());
        return;
    }

    // 発生基準に使うワールド座標とY軸回転を、その時点の対象から取り出す。
    engine::Vector3 basePos = engine::Vector3::kZero;
    // 発生対象のワールドY軸回転をラジアンで保持する。
    float targetWorldY = 0.0f;

    engine::Object3D* posTarget = baseObject; // 位置の基準
    engine::Object3D* rotTarget = baseObject; // 向きの基準

    // 指定ボーンは位置に使う。向きは従来の基準がなければ指定ボーンを使う。
    if (j.contains("TargetName")) {
        SceneManager* sceneManager = SceneManager::GetInstance();
        // 指定した名前の実体が見つかれば、位置を合わせる対象として使う。
        if (auto* found = FindMeshEffectTarget(sceneManager ? sceneManager->GetCurrentScene() : nullptr,
                j["TargetName"].get<std::string>())) {
            posTarget = found;
            // もともとの向き基準がない場合は、見つかった追従先を向きの基準にも使う。
            if (!rotTarget) {
                rotTarget = found;
            }
        }
    }

    // 位置を合わせる対象を取得済みの場合は、そのワールド座標を基準にする。
    if (posTarget) {
        basePos = posTarget->GetWorldPosition();
    }

    // 向きの基準があれば、親をたどってルートのY軸回転を使う。
    if (rotTarget) {
        engine::Object3D* rootObj = rotTarget;

        // 親を辿って一番上のノード（プレイヤー本体など）を見つける
        while (rootObj && rootObj->GetParent()) {
            rootObj = rootObj->GetParent();
        }

        // ルートを取得できた場合は、そのローカル回転のY成分を保存する。
        if (rootObj) {
            targetWorldY = rootObj->GetRotation().y;
        }
    }

    // プリセットの位置・回転オフセットへ、呼び出し元の回転を加える。
    engine::Vector3 offsetPos = engine::Vector3::kZero;
    engine::Vector3 offsetRot = engine::Vector3::kZero;

    // 位置差が保存されていれば、対象基準で適用するオフセットに設定する。
    if (j.contains("Position"))
        offsetPos = { j["Position"][0], j["Position"][1], j["Position"][2] };
    // 回転差が保存されていれば、呼び出し側の回転を足す前の値として読み込む。
    if (j.contains("Rotation"))
        offsetRot = { j["Rotation"][0], j["Rotation"][1], j["Rotation"][2] };

    // シーケンサーからの追加オフセットを合成
    offsetPos.x += extOffset.x;
    offsetPos.y += extOffset.y;
    offsetPos.z += extOffset.z;

    offsetRot.x += extRot.x;
    offsetRot.y += extRot.y;
    offsetRot.z += extRot.z;

    // エディターで設定した発生位置のオフセットを、対象のワールドY軸回転に合わせる。
    float s = sinf(targetWorldY);
    float c = cosf(targetWorldY);
    engine::Vector3 rotatedOffset;
    rotatedOffset.x = offsetPos.x * c + offsetPos.z * s;
    rotatedOffset.y = offsetPos.y;
    rotatedOffset.z = -offsetPos.x * s + offsetPos.z * c;

    // VolumeModeは同じ演出を複数面に展開するため、ここで生成個数へ変換します。
    std::int32_t volumeMode = j.contains("VolumeMode") ? (std::int32_t)j["VolumeMode"] : 0;
    std::int32_t numSpawns = (volumeMode == 2) ? 3 : (volumeMode == 1 ? 2 : 1);
    float lifetime = j.contains("Lifetime") ? (float)j["Lifetime"] : 1.0f;

    // 立体表示で重ねる枚数だけ、同じプリセットからエフェクトを生成する。
    for (std::int32_t i = 0; i < numSpawns; ++i) {
        auto effect = std::make_unique<EffectObject3D>();
        effect->Initialize(common_);

        ApplySpawnVisualSettings(*effect, j, extScale);

        // 形状種別が設定されていれば、シェーダーの種別と実際のメッシュ生成を合わせる。
        if (j.contains("ProceduralType")) {
            std::int32_t procType = j["ProceduralType"];
            effect->SetProceduralType(procType);

            // 手続き形状を示す番号の場合だけ、寸法などを適用してメッシュを作る。
            if (procType >= 1) {
                effect->ReadProceduralMeshSettings(j);
                effect->UpdateProceduralMesh();
            }
        }

        // 衝突設定が保存されている場合は形状とマスクを復元し、有効なものを衝突管理へ登録する。
        if (j.contains("Collision")) {
            effect->SetMeshSetting(&EffectObject3D::MeshSettings::hasCollision, j["Collision"]["HasCollision"]);

            // 当たり判定を使う保存データでは、形状・寸法・中心も読み込む。
            if (effect->GetMeshSettings().hasCollision) {
                effect->SetMeshSetting(&EffectObject3D::MeshSettings::collisionShape, j["Collision"]["Shape"]);
                effect->SetMeshSetting(&EffectObject3D::MeshSettings::collisionSize, { j["Collision"]["Size"][0], j["Collision"]["Size"][1], j["Collision"]["Size"][2] });
                effect->SetMeshSetting(&EffectObject3D::MeshSettings::collisionOffset, { j["Collision"]["Offset"][0], j["Collision"]["Offset"][1], j["Collision"]["Offset"][2] });

                engine::ColliderType cType = engine::ColliderType::kNone;
                // 保存番号0は、生成後のコライダーを球として設定する。
                if (effect->GetMeshSettings().collisionShape == 0)
                    cType = engine::ColliderType::kSphere;
                // 保存番号1はワールド軸に揃う箱を使う。
                else if (effect->GetMeshSettings().collisionShape == 1) cType = engine::ColliderType::kAabb;
                // 保存番号2は回転を持つ箱を使う。
                else if (effect->GetMeshSettings().collisionShape == 2) cType = engine::ColliderType::kObb;

                // 今のコライダー設定を土台に、プリセット側で指定した形状・寸法・中心を反映する。
                engine::Object3D::ColliderConfig cConfig = effect->GetColliderConfig();
                cConfig.type = cType;
                cConfig.size = effect->GetMeshSettings().collisionSize;
                cConfig.center = effect->GetMeshSettings().collisionOffset;
                effect->SetColliderConfig(cConfig);

                // 有効な判定をプレイヤー攻撃として登録し、敵カテゴリだけとの接触を求めます。
                effect->SetCollisionAttribute(engine::collisionCategories::Get("player_attack"));
                effect->SetCollisionMask(engine::collisionCategories::Get("enemy"));

                engine::CollisionManager::GetInstance()->AddObject(effect.get());
            }
        }

        // 対象の位置に回転済みのオフセットを足して、発生位置を求める。
        engine::Vector3 finalPos = { basePos.x + rotatedOffset.x, basePos.y + rotatedOffset.y, basePos.z + rotatedOffset.z };

        // プリセットの傾きとひねりを保ち、Y軸回転へ対象の向きを加える。
        engine::Vector3 finalRot = { offsetRot.x, offsetRot.y + targetWorldY, offsetRot.z };

        engine::Vector3 localZ;
        localZ.x = sinf(finalRot.y) * cosf(finalRot.x);
        localZ.y = -sinf(finalRot.x);
        localZ.z = cosf(finalRot.y) * cosf(finalRot.x);

        // 二枚を交差させる方式では、二枚目だけX軸に90度回す。
        if (volumeMode == 1 && i == 1) {
            finalRot.x += (std::numbers::pi_v<float> * 0.5f); // 90度
        }
        // 三枚を並べる方式では、面の法線方向へ間隔を作る。
        else if (volumeMode == 2) {
            float gap = 0.02f;

            // 二枚目は法線の正方向にずらして、中央の面との間に間隔を作る。
            if (i == 1) {
                finalPos.x += localZ.x * gap;
                finalPos.y += localZ.y * gap;
                finalPos.z += localZ.z * gap;
            }
            // 三枚目は反対側へずらし、中央の面を挟む。
            if (i == 2) {
                finalPos.x -= localZ.x * gap;
                finalPos.y -= localZ.y * gap;
                finalPos.z -= localZ.z * gap;
            }
        }

        // 追従先がある場合は、以後の更新でも位置と回転の差を使えるよう設定する。
        if (posTarget) {
            effect->SetTargetObject(posTarget);
            effect->SetOffsets(offsetPos, offsetRot);
        }

        effect->SetTranslate(finalPos);
        effect->SetRotation(finalRot);

        // 発生位置の姿勢と初期の描画行列を確定し、更新と描画の一覧へ追加します。
        effect->Play(lifetime);
        effect->Update(0.0f);
        effect->UpdateLocalMatrix();
        effect->UpdateWorldMatrix();

        activeEffects_.push_back(std::move(effect));
    }
}

// 呼び出し元のワールド位置・回転を基準にエフェクトを生成します。
// 位置と回転は引数から取り、分割面、寿命、材質などはプリセットを参照します。
void MeshEffectManager::SpawnEffectAt(const std::string& jsonFilePath, const engine::Vector3& worldPos, const engine::Vector3& worldRot,
    const engine::Vector3& scale, float lifetimeOverride) {
    // ワールド座標で発生する入口でも、未接続の描画基盤を現在のシーンから探す。
    if (!common_) {
        SceneManager* sm = SceneManager::GetInstance();

        // シーン管理とシーンが存在する場合は、シーンが持つ3D描画基盤を使う。
        if (sm && sm->GetCurrentScene()) {
            common_ = sm->GetCurrentScene()->GetObject3DCommon();
        }
    }

    // 描画基盤が見つからなければ、プリセットを生成できないので戻る。
    if (!common_)
        return;

    json j;
    try {
        // プリセットファイルが開けなければ、個体を作る前に診断を出す。
        if (!LoadEffectJsonCached(jsonFilePath, j)) {
            DebugConsole::GetInstance()->AddLog(LogLevel::kError, "[MeshEffectManager] SpawnEffectAt: failed to open " + jsonFilePath);
            return;
        }
    } catch (const std::exception& e) {
        DebugConsole::GetInstance()->AddLog(LogLevel::kError, "[MeshEffectManager] SpawnEffectAt JSON error: " + jsonFilePath + " / " + e.what());
        return;
    }

    std::int32_t volumeMode = j.contains("VolumeMode") ? (std::int32_t)j["VolumeMode"] : 0;
    std::int32_t numSpawns  = (volumeMode == 2) ? 3 : (volumeMode == 1 ? 2 : 1);
    float lifetime = j.contains("Lifetime") ? (float)j["Lifetime"] : 1.0f;
    // 正の秒数が渡された場合、プリセットの寿命を呼び出し元指定の時間で上書きします。
    if (lifetimeOverride > 0.0f)
        lifetime = lifetimeOverride;

    // localZ ベクトル (volumeMode 2 の立体化オフセット計算用)
    engine::Vector3 localZ;
    localZ.x = sinf(worldRot.y) * cosf(worldRot.x);
    localZ.y = -sinf(worldRot.x);
    localZ.z = cosf(worldRot.y) * cosf(worldRot.x);

    for (std::int32_t i = 0; i < numSpawns; ++i) {
        std::unique_ptr<EffectObject3D> effect;
        // 事前確保した描画バッファを優先し、再利用した個体は素材を適用する前に前回の状態を消す。
        if (!recycledEffects_.empty()) {
            effect = std::move(recycledEffects_.back());
            recycledEffects_.pop_back();
            effect->ResetForReuse();
        // 再利用できる個体がなければ、描画バッファを持つ新しい個体を初期化する。
        } else {
            effect = std::make_unique<EffectObject3D>();
            effect->Initialize(common_);
        }

        ApplySpawnVisualSettings(*effect, j, scale);

        // プリセットに形状種別があれば、見た目の種別を設定する。
        if (j.contains("ProceduralType")) {
            std::int32_t procType = j["ProceduralType"];
            effect->SetProceduralType(procType);

            // 手続き形状を指す場合は、保存された寸法からメッシュを構築する。
            if (procType >= 1) {
                effect->ReadProceduralMeshSettings(j);
                effect->UpdateProceduralMesh();
            }
        }

        // --- 座標・回転を直接適用 (JSONのPosition/Rotationは無視) ---
        engine::Vector3 finalPos = worldPos;
        engine::Vector3 finalRot = worldRot;

        // 二枚を交差する指定では、二枚目の面をX軸に90度回す。
        if (volumeMode == 1 && i == 1) {
            finalRot.x += (std::numbers::pi_v<float> * 0.5f); // 90度クロス
        // 三枚を重ねる指定では、中央の面を基準に奥行きの間隔を取る。
        } else if (volumeMode == 2) {
            float gap = 0.02f;

            // 二枚目は面の法線方向へずらす。
            if (i == 1) {
                finalPos.x += localZ.x * gap;
                finalPos.y += localZ.y * gap;
                finalPos.z += localZ.z * gap;
            }
            // 三枚目は二枚目と反対側へずらす。
            if (i == 2) {
                finalPos.x -= localZ.x * gap;
                finalPos.y -= localZ.y * gap;
                finalPos.z -= localZ.z * gap;
            }
        }

        // 面ごとの位置と回転を反映し、再生開始時点の行列を即座に確定します。
        effect->SetTranslate(finalPos);
        effect->SetRotation(finalRot);
        effect->Play(lifetime);
        effect->Update(0.0f);
        effect->UpdateLocalMatrix();
        effect->UpdateWorldMatrix();

        // 衝突付きや親子付きのObjectは回収対象にしません。
        if (!effect->GetMeshSettings().hasCollision && !effect->GetParent())
            reusableEffects_.insert(effect.get());
        activeEffects_.push_back(std::move(effect));
    }
}

void MeshEffectManager::SpawnEffectAtScoped(
    EffectScopeId scopeId,
    const std::string& jsonFilePath,
    const engine::Vector3& worldPos,
    const engine::Vector3& worldRot,
    const engine::Vector3& scale,
    float lifetimeOverride) {
    // 1回の発生で複数の面が作られる場合も、今回追加された範囲の全てを同じ停止用IDに登録します。
    const size_t firstNewEffect = activeEffects_.size();
    SpawnEffectAt(jsonFilePath, worldPos, worldRot, scale, lifetimeOverride);

    // 無効なスコープ番号の場合は、生成済みの個体を一括停止の対象へ関連付けない。
    if (scopeId == kInvalidEffectScope) {
        return;
    }

    for (size_t index = firstNewEffect; index < activeEffects_.size(); ++index) {
        effectScopes_[activeEffects_[index].get()] = scopeId;
    }
}

// 指定位置へ拡大・減衰する加算合成のリング波紋を生成します。
void MeshEffectManager::SpawnRingWaveEffect(const engine::Vector3& position) {
    // 描画基盤がなければ、波紋のメッシュを作る前にシーンから取得を試す。
    if (!common_) {
        auto sm = SceneManager::GetInstance();

        // 現在のシーンが利用できれば、共通の3D描画基盤を取得する。
        if (sm->GetCurrentScene()) {
            common_ = sm->GetCurrentScene()->GetObject3DCommon();
        }
    }

    // 基盤を用意できなければ、波紋の生成を行わない。
    if (!common_)
        return;

    auto effect = std::make_unique<EffectObject3D>();
    effect->Initialize(common_);

    // 波紋の形状と合成を指定する前に、使わない描画オプションを解除します。
    effect->SetEnableNoiseTexture(false);
    effect->SetEnableColorRamp(false);
    effect->SetEnableDistortion(false);
    effect->SetEnableReveal(false);
    effect->SetDistortionStrength(0.0f);
    effect->SetEdgeFadeStrength(1.0f);

    // 波紋に使うリング形状を選び、UVを外側へ動かす設定へ進む。
    effect->SetProceduralType(10); // 10: Ring

    // UVスクロールで波紋を外側に広げる (V方向スクロール)
    // AddressV=CLAMP にしているので、1回の波で終わる
    effect->SetScrollSpeed({ 0.0f, -1.5f });

    // スケールを徐々に大きくする
    effect->SetStartScale({ 1.0f, 1.0f, 1.0f });
    effect->SetEndScale({ 6.0f, 6.0f, 6.0f });

    // 色と透明度（青白っぽく発光しながら消える）
    effect->SetStartColor({ 0.5f, 0.8f, 1.0f, 1.0f });
    effect->SetEndColor({ 0.5f, 0.8f, 1.0f, 0.0f });

    // 発光の強さ
    effect->SetIntensity(2.0f);

    // 加算合成（透けるように光らせる）
    effect->SetBlendMode(BlendMode::kAdd);

    // リングの幅を設定
    effect->SetMeshSetting(&EffectObject3D::MeshSettings::ringInnerRadius, 0.5f);
    effect->SetMeshSetting(&EffectObject3D::MeshSettings::ringOuterRadius, 1.5f);
    effect->SetMeshSetting(&EffectObject3D::MeshSettings::meshSegments, 32);

    // リングの寸法と分割数を設定し終えてから、描画メッシュを作成します。
    effect->UpdateProceduralMesh();

    // 波紋のメッシュ描画先が取得できた場合に、輪の模様を割り当てる。
    if (effect->GetMeshRenderer()) {
        effect->GetMeshRenderer()->SetTexture("Resources/sprite/effect/gradationLine.png");
    }

    // ワールド位置に水平な姿勢で置きます。
    effect->SetTranslate(position);
    effect->SetRotation({ 0.0f, 0.0f, 0.0f });

    // 0.6秒の波紋として再生を始め、初期行列を作って更新リストに登録します。
    effect->Play(0.6f);
    effect->Update(0.0f);
    effect->UpdateLocalMatrix();
    effect->UpdateWorldMatrix();

    activeEffects_.push_back(std::move(effect));
}

// 円筒面の横方向にUVを動かし、色と拡縮を時間で変えるポータルを生成する。
// 両面を表示しながら他のエフェクトを遮らないよう、描画パイプラインは面カリングと深度書き込みを無効にします。
void MeshEffectManager::SpawnPortalEffect(const engine::Vector3& position, float lifetime) {
    // 描画基盤が未接続なら、ポータルの生成前に現在のシーンから取得を試す。
    if (!common_) {
        auto sm = SceneManager::GetInstance();

        // 現在のシーンが利用できる場合は、共通の3D描画基盤を取得する。
        if (sm->GetCurrentScene()) {
            common_ = sm->GetCurrentScene()->GetObject3DCommon();
        }
    }

    // 基盤が取得できない場合は、円筒の描画を初期化できないので戻る。
    if (!common_)
        return;

    auto effect = std::make_unique<EffectObject3D>();
    effect->Initialize(common_);

    // 円筒に重ねない描画オプションを解除し、形状を設定します。
    effect->SetProceduralType(0);
    effect->SetEnableNoiseTexture(false);
    effect->SetEnableColorRamp(false);
    effect->SetEnableDistortion(false);
    effect->SetEnableReveal(false);        // Revealは使わない
    effect->SetDistortionStrength(0.0f);
    effect->SetEdgeFadeStrength(1.0f);

    // 円筒の半径・高さ・分割数を設定し、ポータルの側面を生成する。
    effect->SetProceduralType(5);
    effect->SetMeshSetting(&EffectObject3D::MeshSettings::cylinderRadius, 1.5f);   // 半径
    effect->SetMeshSetting(&EffectObject3D::MeshSettings::cylinderHeight, 3.0f);   // 高さ
    effect->SetMeshSetting(&EffectObject3D::MeshSettings::meshSegments, 32);

    // UVタイリング: U方向に4回タイリングで縞々模様に
    effect->SetMeshSetting(&EffectObject3D::MeshSettings::uvTiling, { 4.0f, 1.0f });
    effect->UpdateProceduralMesh();

    // 円筒の周方向に沿ってU座標を進める。
    effect->SetScrollSpeed({ 0.8f, 0.0f }); // 横方向スクロール

    // 演出の進行に合わせて青紫から水色へ変化させる。
    effect->SetStartColor({ 0.3f, 0.1f, 1.0f, 0.9f }); // 青紫
    effect->SetEndColor(  { 0.0f, 0.8f, 1.0f, 0.6f }); // 水色

    // 発光強度
    effect->SetIntensity(2.5f);

    // 半透明の縞を残せるよう、アルファのしきい値を0にする。
    effect->SetAlphaReference(0.0f);

    // 加算合成でポータルらしく光らせる
    effect->SetBlendMode(BlendMode::kAdd);

    // 円筒の描画先があれば、ポータルの縦縞を作る画像を設定する。
    if (effect->GetMeshRenderer()) {
        effect->GetMeshRenderer()->SetTexture("Resources/sprite/effect/gradationLine.png");
    }

    // 時間補間で高さを保ち、円筒の幅と奥行きを少し広げる。
    effect->SetStartScale({ 1.0f, 1.0f, 1.0f });
    effect->SetEndScale(  { 1.1f, 1.0f, 1.1f });

    // ワールド位置に回転なしで配置します。
    effect->SetTranslate(position);
    effect->SetRotation({ 0.0f, 0.0f, 0.0f });

    // 引数で指定された秒数だけ再生し、初期行列を算出して更新対象へ登録します。
    effect->Play(lifetime);
    effect->Update(0.0f);
    effect->UpdateLocalMatrix();
    effect->UpdateWorldMatrix();

    activeEffects_.push_back(std::move(effect));
}

#ifdef USE_IMGUI
void MeshEffectManager::BeginEditorSession() {
    // 編集セッションがすでに始まっていれば、退避した通常再生の情報を上書きしない。
    if (editorSession_) return;
    // プレビューで再生中の素材を置き換えられるよう、通常再生の個体・停止ID・初期オフセットを一緒に退避する。
    savedEditorScale_ = timeScale_;
    savedEditorDebugEffect_ = previewEffectForDebug_; previewEffectForDebug_ = nullptr;
    activeEffects_.swap(savedEditorEffects_); effectScopes_.swap(savedEditorScopes_);
    timelineScopes_.swap(savedEditorTimelineScopes_); timelineOffsets_.swap(savedEditorOffsets_);
    timeScale_ = 1; updatedThisFrame_ = false; editorSession_ = true;
}
void MeshEffectManager::EndEditorSession() {
    // 編集セッションが開始されていなければ、戻すプレビュー状態がないので終了する。
    if (!editorSession_) return;
    // 編集中の個体を先に解放し、退避していた通常再生とスコープ情報を一緒に戻します。
    ClearActiveEffects();
    activeEffects_.swap(savedEditorEffects_); effectScopes_.swap(savedEditorScopes_);
    timelineScopes_.swap(savedEditorTimelineScopes_); timelineOffsets_.swap(savedEditorOffsets_);
    timeScale_ = savedEditorScale_; updatedThisFrame_ = false; editorSession_ = false;
    // 借用していたデバッグ表示先は通常個体を戻してから復元する。
    previewEffectForDebug_ = savedEditorDebugEffect_; savedEditorDebugEffect_ = nullptr;
}
#endif
