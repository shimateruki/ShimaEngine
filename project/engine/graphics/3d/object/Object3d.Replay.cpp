#include "Object3D.h"
#include "EditorPropertyRegistry.h"

#include <atomic>

namespace engine {

namespace {
std::atomic<uint64_t> gNextReplayId{ 1 };
}

uint64_t Object3D::EnsureReplayId() {
    // リプレイ用IDが未採番の0の場合だけ発行する。IDの一意性に加え、他のメンバーの順序をそろえる必要はない。
    if (replayId_ == 0) {
        replayId_ = gNextReplayId.fetch_add(1, std::memory_order_relaxed);
    }

    return replayId_;
}

Object3D::ReplayState Object3D::CaptureReplayState() const {
    // TransformだけでなくAnimator・衝突属性・素材と各Componentも同じフレームへ保存する。
    ReplayState state;
    state.scale = transform_.scale;
    state.rotation = transform_.rotate;
    state.translation = transform_.translate;
    state.quaternion = transform_.quaternion;
    state.quaternionMaster = transform_.isQuaternionMaster;

    state.visible = isVisible_;
    state.dead = isDead_;
    state.collecting = isCollecting_;
    state.collectTimer = collectTimer_;
    state.animationTime = animationTime_;
    state.animationName = animName_;
    state.animationLoop = isAnimLoop_;
    state.animatorControllerPath = animatorControllerPath_;
    state.animatorSnapshot = animatorControllerRuntime_.CaptureSnapshot();

    state.hasParameter = param_.has_value();

    // 個別パラメーターを持つ場合は、存在フラグに加えて中身も記録する。
    if (param_) {
        state.parameter = *param_;
    }

    state.collisionAttribute = GetCollisionAttribute();
    state.collisionMask = GetCollisionMask();

    state.modelName = GetModelName();
    state.texturePath = GetTexturePath();
    state.materialType = GetMaterialType();
    state.color = GetColor();
    state.emissive = GetEmissive();

    state.particleEmitterComponent = particleEmitterComponent_;
    state.meshEffectComponent = meshEffectComponent_;
    state.pathMoverComponent = pathMoverComponent_;
    state.gameplayLinkComponent = gameplayLinkComponent_;
    state.navAgentComponent = navAgentComponent_;

    state.replayRemoved = replayRemoved_;

    // 派生Object固有の状態は共通スナップショットの末尾へ委譲する。
    CaptureReplayCustomState(state.custom);
    // クラスの編集設定も巻き戻します。派生側の再生状態とは別の予約キーに保持します。
    nlohmann::json properties = nlohmann::json::object();
    EditorPropertyRegistry::GetInstance()->WriteClassProperties(*this, properties);
    // 登録済みのクラス設定があるObjectだけ、派生の再生情報とは別の予約キーへ格納する。
    if (!properties.empty()) state.custom["__classProperties"] = std::move(properties);
    return state;
}

void Object3D::RestoreReplayState(const ReplayState& state) {
    // 置き方と回転の優先方式を戻し、可視・取得・アニメーションの時計を記録値にする。
    transform_.scale = state.scale;
    transform_.rotate = state.rotation;
    transform_.translate = state.translation;
    transform_.quaternion = state.quaternion;
    transform_.isQuaternionMaster = state.quaternionMaster;

    isVisible_ = state.visible;
    isDead_ = state.dead;
    isCollecting_ = state.collecting;
    collectTimer_ = state.collectTimer;
    animationTime_ = state.animationTime;
    animName_ = state.animationName;
    isAnimLoop_ = state.animationLoop;

    // Snapshotを戻す前に対象Controllerを揃え、別Assetの状態を誤適用しない。
    if (state.animatorControllerPath != animatorControllerPath_) {
        SetAnimatorController(state.animatorControllerPath);
    }

    // 有効なAnimatorの状態が保存されている場合だけ、状態と現在の表示姿勢を復元する。
    if (state.animatorSnapshot.valid) {
        animatorControllerRuntime_.RestoreSnapshot(state.animatorSnapshot);
        ApplyAnimatorControllerPose(GetModel(), true);
    }

    // パラメーターを保持していたフレームでは、記録した内容を復元する。
    if (state.hasParameter) {
        param_ = state.parameter;
    // パラメーター未設定だったフレームでは、現在の個別値を残さない。
    } else {
        param_.reset();
    }

    SetCollisionAttribute(state.collisionAttribute);
    SetCollisionMask(state.collisionMask);

    // 保存モデル名がある場合はモデルを先に揃え、その初期化で上書きされる材質値を後で復元します。
    if (GetModelName() != state.modelName && !state.modelName.empty()) {
        SetModel(state.modelName);
    }

    // 記録に空でない画像パスがあり、現在と異なる場合だけ画像を差し替える。
    if (GetTexturePath() != state.texturePath && !state.texturePath.empty()) {
        SetTexture(state.texturePath);
    }

    SetMaterialType(state.materialType);
    SetColor(state.color);
    SetEmissive(state.emissive);

    // コンポーネントを上書きする前に、実行中のインスタンスが依存するアセットの変更を判定します。
    const bool particleAssetChanged =
        particleEmitterComponent_.has_value() != state.particleEmitterComponent.has_value() ||
        (particleEmitterComponent_ && state.particleEmitterComponent &&
            (particleEmitterComponent_->GetCpuParticle() != state.particleEmitterComponent->GetCpuParticle() ||
             particleEmitterComponent_->GetGpuParticle() != state.particleEmitterComponent->GetGpuParticle()));
    const bool meshEffectChanged =
        meshEffectComponent_.has_value() != state.meshEffectComponent.has_value() ||
        (meshEffectComponent_ && state.meshEffectComponent &&
            (meshEffectComponent_->GetPrimaryEffect() != state.meshEffectComponent->GetPrimaryEffect() ||
             meshEffectComponent_->GetSecondaryEffect() != state.meshEffectComponent->GetSecondaryEffect()));

    particleEmitterComponent_ = state.particleEmitterComponent;
    meshEffectComponent_ = state.meshEffectComponent;
    pathMoverComponent_ = state.pathMoverComponent;
    gameplayLinkComponent_ = state.gameplayLinkComponent;
    navAgentComponent_ = state.navAgentComponent;

    // 粒子コンポーネントの有無またはプリセットが変わった場合だけ、既存の発生器を破棄する。
    if (particleAssetChanged) {
        gpuEmitter_.reset();
    }

    // メッシュエフェクトの有無や設定名が変わった場合は、旧素材で生成した二つの枠を両方消す。
    if (meshEffectChanged) {
        currentMeshEffect1_.clear();
        currentMeshEffect2_.clear();
        attachedEffects1_.clear();
        attachedEffects2_.clear();
    }

    replayRemoved_ = state.replayRemoved;
    // 旧記録にクラス設定がない場合も、派生側のデータはこの後で復元する。
    // 予約キーを含むオブジェクト形式だけ、クラスの調整値を先に読み戻す。
    if (state.custom.is_object() && state.custom.contains("__classProperties"))
        EditorPropertyRegistry::GetInstance()->ReadClassProperties(*this, state.custom["__classProperties"]);
    // クラスの調整値を先に読み、その値を使う派生クラスの実行状態を後から復元します。
    RestoreReplayCustomState(state.custom);
    // 復元し終えた姿勢をこのフレームの描画へ反映するため、ローカル・ワールドの順で行列を更新する。
    UpdateLocalMatrix();
    UpdateWorldMatrix();
}

void Object3D::CaptureReplayCustomState(json& state) const {
    state = json::object();
}

void Object3D::RestoreReplayCustomState(const json& state) {
    (void)state;
}

} // namespace engine
