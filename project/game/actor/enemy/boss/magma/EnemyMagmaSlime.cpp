#define NOMINMAX
#include <cstdint>
#include "game/actor/combat/CombatGroundQuery.h"
#include "game/actor/combat/CombatMath.h"
#include "game/actor/enemy/boss/magma/EnemyMagmaSlime.Internal.h"
#include "game/actor/enemy/boss/core/BossChantVisual.h"

#include "CollisionConfig.h"
#include "CollisionManager.h"
#include "game/system/rules/EventManager.h"
#include "GpuParticleManager.h"
#include "MeshRenderer.h"
#include "SceneManager.h"
#include "game/actor/enemy/animation/SlimeBounceAnimator.h"
#include "game/actor/enemy/animation/BossGroundHopVfx.h"
#include "VfxSequencer.h"

#include <algorithm>
#include <cassert>
#include <cmath>

using namespace enemyMagmaSlimeImplementation;

// マグマボスの攻撃状態を管理します。詠唱、攻撃、回復の段階で速度と当たり判定を切り替えます。

EnemyMagmaSlime::EnemyMagmaSlime() = default;
EnemyMagmaSlime::~EnemyMagmaSlime() = default;

void EnemyMagmaSlime::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    // 共通の敵として初期化した後、マグマ用の技・外観・衝突と火山プレスの専用クリップを設定します。
    BaseEnemy::Initialize(common, modelName);
    SetName("Enemy_MagmaSlime");
    SetEnemyType("MagmaSlime");
    ReloadAttackProfile();

    // 本体はOBJ内の溶岩・黒曜石・発光亀裂PBR材質をそのまま使います。
    SetMaterialType(0);
    SetColor({1.0f, 0.94f, 0.86f, 1.0f});
    SetEmissive(1.18f);
    SetMetallic(0.12f);
    SetRoughness(0.26f);
    SetEnableEnvMap(true);
    SetEnvIntensity(0.42f);
    SetDefaultColor(GetColor());

    SetCollisionAttribute(engine::collisionCategories::Get("enemy"));
    SetCollisionMask(engine::collisionCategories::Get("player") | engine::collisionCategories::Get("ground") | engine::collisionCategories::Get("player_attack") | engine::collisionCategories::Get("player_bullet"));
    SetColliderType(engine::ColliderType::kSphere);
    SyncCollisionRadius();

    // Body Clipは描画メッシュだけに掛け、物理座標と攻撃判定には混ぜない。
    const EnemyAttackDefinition* slamAttack = GetAttackProfile().FindAttack(kVolcanicSlamAttackId);
    // 技定義にアニメーションがありクリップを読み込めた場合だけ、プレスの専用姿勢を使います。
    if (slamAttack && !slamAttack->animation.empty() &&
        slamBodyClip_.Load(BodyAnimationClip::ResolveAssetPath(slamAttack->animation))) {
        slamVisualPose_ = std::make_shared<engine::Matrix4x4>(engine::Math::MakeIdentity4x4());
        // メッシュを取得できた場合に、描画専用の変形行列を渡します。
        if (GetMeshRenderer()) GetMeshRenderer()->SetTimelineVisualTransform(slamVisualPose_);
    }
}

void EnemyMagmaSlime::Update(float deltaTime) {
    // 遭遇の出現や休眠によって更新済みの場合は戻り、通常戦闘では設置済みの技を先に進めます。
    if (UpdateEncounterState(deltaTime)) {
        return;
    }

    // 既に場にある溶岩は、新しい技を選ぶ前に寿命・移動・命中を更新する。
    UpdateMagmaBlobs(deltaTime);
    UpdateMagmaPools(deltaTime);
    UpdateMagmaRings(deltaTime);
    UpdateMagmaPillars(deltaTime);
    UpdateMagmaSurges(deltaTime);
    UpdateMagmaFlood(deltaTime);

    // 撃破または非表示では、未消化の独自移動と場に残る攻撃を取り消して基底処理へ戻す。
    if (IsDead() || !GetIsVisible()) {
        locomotion_.Reset();
        HideAttackTelegraph();
        ClearTransientVisuals();
        BaseEnemy::Update(deltaTime);
        return;
    }

    // 休眠や投げからの復帰中はボスの攻撃を進めず、基底側の物理や回復を優先する。
    if (IsDormant() || IsThrowRecovering() || ShouldHandleDefeatEffect()) {
        locomotion_.Reset();
        HideAttackTelegraph();
        // 描画姿勢を保持している場合は、前のプレスの変形を消します。
        if (slamVisualPose_) *slamVisualPose_ = engine::Math::MakeIdentity4x4();
        BaseEnemy::Update(deltaTime);
        return;
    }

    // 配置倍率を先に確保し、ジャンプや溶岩攻撃の残り時間を今フレームへ進める。
    EnsureBaseScale();
    scriptedMovementThisFrame_ = false;
    idleTimer_ += deltaTime;
    locomotion_.TickCooldown(deltaTime);
    attackCooldown_ = (std::max)(0.0f, attackCooldown_ - deltaTime);
    landingPulseTimer_ = (std::max)(0.0f, landingPulseTimer_ - deltaTime);

    // 技の選択と向きの調整には、標的との水平距離と水平向きを使う。
    engine::Vector3 targetDirection = lockedDirection_;
    float targetDistance = 9999.0f;

    // 追跡対象がいる場合だけ水平距離を測り、設定済みの方向と距離の初期値を置き換えます。
    if (GetTarget()) {
        const engine::Vector3 toTarget = GetTarget()->GetWorldPosition() - GetWorldPosition();
        targetDirection = NormalizePlanar(toTarget);
        targetDistance = PlanarDistance(GetTarget()->GetWorldPosition(), GetWorldPosition());
    }

    UpdateBossTargetEngagement(targetDistance);

    const engine::Vector3 previousVelocity = GetVelocity();
    engine::Vector3 velocity = previousVelocity;
    velocity.x = 0.0f;
    velocity.z = 0.0f;
    UpdateBehavior(deltaTime, targetDirection, targetDistance, velocity);

    // AIが決めた速度を反映してから、ボスの姿勢とコライダーをそろえる。
    SetVelocity(velocity);
    ApplySlimeAnimation(deltaTime);
    UpdateSlamBodyPose();
    UpdateBodyHeat(deltaTime);
    SyncCollisionRadius();
    // 専用移動が位置を確定したフレームでは共通モーターによる二重移動を止める。
    SetMotorSuspended(scriptedMovementThisFrame_);
    BaseEnemy::Update(deltaTime);
    SetMotorSuspended(false);
    UpdateChantVisual(deltaTime);
}

// 本体の描画を基底処理へ渡します。詠唱陣などは透過物の描画で別に処理します。
void EnemyMagmaSlime::Draw(ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource) {
    // 不透明な本体を先に描き、加算の詠唱陣は地形の後へ送る。
    BaseEnemy::Draw(pointLightResource, spotLightResource);
}

void EnemyMagmaSlime::DrawOwnedTransparentVisuals(ID3D12Resource* pointLightResource,
                                                    ID3D12Resource* spotLightResource) {
    // 本体が描画対象でない場合、所有している透過演出も描画しません。
    if (!GetIsRenderVisible()) return;
    // 詠唱陣を生成済みなら、共有ライトを渡して描画します。
    if (chantVisual_) {
        chantVisual_->Draw(pointLightResource, spotLightResource);
    }

    // 各地点の魔法陣と接続線は、実体があり表示状態の場合だけ現在のカメラへ揃えて描きます。
    for (const auto& sigil : fireColumnSigils_) {
        if (sigil && sigil->GetIsVisible()) {
            sigil->RefreshRenderCameraData();
            sigil->Draw(pointLightResource, spotLightResource);
        }
    }

    for (const auto& link : fireColumnLinks_) {
        if (link && link->GetIsVisible()) {
            link->RefreshRenderCameraData();
            link->Draw(pointLightResource, spotLightResource);
        }
    }
}

void EnemyMagmaSlime::DrawForCamera(engine::Camera* camera, ID3D12Resource* pointLightResource,
                                    ID3D12Resource* spotLightResource, std::int32_t previewBufferIndex) {
    // 指定カメラが無いか描画対象外なら、そのカメラ用の本体描画を省きます。
    if (!camera || !GetIsRenderVisible()) return;
    engine::Object3D::DrawForCamera(camera, pointLightResource, spotLightResource, previewBufferIndex);
}

void EnemyMagmaSlime::DrawOwnedTransparentVisualsForCamera(engine::Camera* camera, ID3D12Resource* pointLightResource,
                                                            ID3D12Resource* spotLightResource, std::int32_t previewBufferIndex) {
    // 指定カメラがあり本体も表示対象の場合だけ、透過演出をその視点に描きます。
    if (!camera || !GetIsRenderVisible()) return;
    // 詠唱陣があれば描き、地点の陣と接続線も生成済みかつ表示中のものだけ描きます。
    if (chantVisual_) chantVisual_->DrawForCamera(camera, pointLightResource, spotLightResource, previewBufferIndex);
    for (const auto& sigil : fireColumnSigils_) {
        if (sigil && sigil->GetIsVisible()) {
            sigil->DrawForCamera(camera, pointLightResource, spotLightResource, previewBufferIndex);
        }
    }
    for (const auto& link : fireColumnLinks_) {
        if (link && link->GetIsVisible()) {
            link->DrawForCamera(camera, pointLightResource, spotLightResource, previewBufferIndex);
        }
    }
}

void EnemyMagmaSlime::OnSwitchEvent(bool active) {
    // 遭遇制御下のスイッチは出現と休眠を切り替え、それ以外は通常の敵の動作に任せる。
    if (!IsEncounterControlled()) {
        BaseEnemy::OnSwitchEvent(active);
        return;
    }

    encounterRequestedActive_ = active;

    // 試遊時の遭遇状態が未初期化なら、オン・オフの遷移前に状態を作ります。
    if (!encounterInitializedForPlay_) {
        InitializeEncounterState();
    }

    // オンの要求を受け、ボスが休眠している場合は登場を始めます。
    if (active && encounterState_ == EncounterState::kDormant) {
        BeginEncounterAppearance();
    // オフの要求を受け、ボスが活動中または登場中なら、休眠状態へ戻します。
    } else if (!active && encounterState_ != EncounterState::kDormant) {
        ApplyDormantEncounterState();
    }
}

std::unique_ptr<engine::Object3D> EnemyMagmaSlime::Clone() const {
    // 複製先へ配置倍率と標的を渡し、攻撃中の一時状態は持ち越さない。
    auto clone = std::make_unique<EnemyMagmaSlime>();
    assert(GetObjectCommon() != nullptr);
    clone->Initialize(GetObjectCommon(), GetModelName());
    clone->CopyFrom(this);
    clone->SetTarget(GetTarget());
    clone->SetDetectionRange(GetDetectionRange());
    clone->baseScale_ = baseScale_;
    clone->hasBaseScale_ = true;
    return clone;
}

void EnemyMagmaSlime::ApplyManagedScale(const engine::Vector3& scale) {
    // エディターで調整した倍率をアニメーションの基準にし、当たり判定も同期する。
    baseScale_ = scale;
    hasBaseScale_ = true;
    SetScale(scale);
    SyncCollisionRadius();
}

bool EnemyMagmaSlime::HasOwnedSpecialMaterialVisuals() const {
    // 特殊マテリアルの別パスに送る一時エフェクトがあるときだけ描画処理を要求する。
    if (!GetIsVisible()) {
        return false;
    }

    return !magmaBlobs_.empty() || !magmaPools_.empty() || !magmaRings_.empty() || !magmaPillars_.empty() ||
           !magmaSurges_.empty() || !magmaFloodCells_.empty() || rushFlameVisuals_[0] != nullptr ||
           fireColumnVisuals_[0] != nullptr;
}

void EnemyMagmaSlime::DrawOwnedSpecialMaterialVisuals(uint32_t depthSrvHandle, uint32_t grabSrvHandle) {
    // 各種の演出は実体があり、表示中のものだけを最終カメラに合わせて描きます。溶岩、衝撃波、炎で描画パスが異なります。
    for (const MagmaBlob& blob : magmaBlobs_) {
        if (blob.visual && blob.visual->GetIsVisible()) {
            blob.visual->RefreshRenderCameraData();
            blob.visual->DrawMagma(depthSrvHandle, grabSrvHandle);
        }
    }

    for (const MagmaPool& pool : magmaPools_) {
        if (pool.visual && pool.visual->GetIsVisible()) {
            pool.visual->RefreshRenderCameraData();
            pool.visual->DrawMagma(depthSrvHandle, grabSrvHandle);
        }
    }

    for (const MagmaRing& ring : magmaRings_) {
        if (ring.visual && ring.visual->GetIsVisible()) {
            ring.visual->RefreshRenderCameraData();
            ring.visual->DrawShockwave(depthSrvHandle, grabSrvHandle);
        }
    }

    for (const MagmaPillar& pillar : magmaPillars_) {
        if (pillar.visual && pillar.visual->GetIsVisible()) {
            pillar.visual->RefreshRenderCameraData();
            pillar.visual->DrawFire(depthSrvHandle, grabSrvHandle);
        }
    }

    for (const MagmaSurge& surge : magmaSurges_) {
        if (surge.visual && surge.visual->GetIsVisible()) {
            surge.visual->RefreshRenderCameraData();
            surge.visual->DrawMagma(depthSrvHandle, grabSrvHandle);
        }

        if (surge.crestVisual && surge.crestVisual->GetIsVisible()) {
            surge.crestVisual->RefreshRenderCameraData();
            surge.crestVisual->DrawFire(depthSrvHandle, grabSrvHandle);
        }
    }

    for (const std::unique_ptr<engine::Object3D>& flame : rushFlameVisuals_) {
        if (flame && flame->GetIsVisible()) {
            flame->RefreshRenderCameraData();
            flame->DrawFire(depthSrvHandle, grabSrvHandle);
        }
    }

    for (const std::unique_ptr<engine::Object3D>& flame : fireColumnVisuals_) {
        if (flame && flame->GetIsVisible()) {
            flame->RefreshRenderCameraData();
            flame->DrawFire(depthSrvHandle, grabSrvHandle);
        }
    }

    for (const MagmaFloodCell& cell : magmaFloodCells_) {
        if (cell.surface && cell.surface->GetIsVisible()) {
            cell.surface->RefreshRenderCameraData();
            cell.surface->DrawMagma(depthSrvHandle, grabSrvHandle);
        }

        if (cell.crest && cell.crest->GetIsVisible()) {
            cell.crest->RefreshRenderCameraData();
            cell.crest->DrawMagma(depthSrvHandle, grabSrvHandle);
        }
    }
}

void EnemyMagmaSlime::SetDebugPreviewAttackId(const std::string& attackId) {
    // プレビューの技を変える際に、旧技の予告と残存エフェクトを消す。
    debugPreviewAttackId_ = attackId;
    locomotion_.Reset();
    currentAttack_ = AttackKind::kNone;
    attackState_ = AttackState::kIdle;
    attackCooldown_ = 0.05f;
    attackTimer_ = 0.0f;
    HideAttackTelegraph();
    ClearTransientVisuals();
}

const char* EnemyMagmaSlime::GetDebugAttackPhaseName() const {
    // 編集画面には内部状態を予兆・攻撃・硬直・待機の短い表示名で返す。
    switch (attackState_) {
    case AttackState::kWindup:
        return "予兆";

    case AttackState::kActive:
        return "攻撃";

    case AttackState::kRecovery:
        return "硬直";

    default:
        return "待機";
    }
}

void EnemyMagmaSlime::EnsureBaseScale() {
    // 攻撃時の伸縮から復帰できるよう、配置倍率を初回だけ保存する。
    if (hasBaseScale_) {
        return;
    }

    baseScale_ = GetScale();
    hasBaseScale_ = true;
}

// 攻撃中の状態機械を優先し、待機中は選択した技の射程へ移動します。
void EnemyMagmaSlime::UpdateBehavior(float deltaTime, const engine::Vector3& targetDirection, float targetDistance,
                                     engine::Vector3& velocity) {
    // 発動中の技を先に進め、次の技や移動はその技が終わるまで選び直さない。
    if (attackState_ != AttackState::kIdle) {
        UpdateAttack(deltaTime, targetDirection, targetDistance, velocity);
        return;
    }

    // 専用の移動が進行中であれば、次の行動を決める前に同じ移動を進めます。
    if (locomotion_.IsActive()) {
        UpdateLocomotion(deltaTime, targetDirection, velocity);
        return;
    }

    // 追跡の前提がそろわなければ、落下速度を保ったまま徘徊する。
    if (!GetTarget() || !GetParameters().has_value()) {
        const float verticalVelocity = velocity.y;
        velocity = CalculateWanderVelocity(deltaTime, 0.48f, 0.38f);
        velocity.y = verticalVelocity;
        UpdateFacing(velocity);
        return;
    }

    UpdateFacing(targetDirection);

    // 標的の発見リアクション中は、予告や新しい移動を開始しません。
    if (UpdateNoticeReaction(deltaTime, targetDistance, GetDetectionRange(), targetDirection)) {
        return;
    }

    // プレビュー指定を優先し、通常戦では距離をもとに巡回中の技を選ぶ。
    const AttackKind debugAttack = ResolveDebugAttack();
    const AttackKind plannedAttack =
        debugAttack != AttackKind::kNone ? debugAttack : ResolveAutomaticAttack(targetDistance);
    const EnemyAttackDefinition& planned = GetAttackDefinition(GetAttackId(plannedAttack));
    const bool inRange = targetDistance >= planned.minRange && targetDistance <= planned.maxRange;

    // 接地済みで標的を交戦対象として認識し、技の再使用待ちが無く射程内にいる場合に攻撃を開始します。
    if (IsGrounded() && IsBossTargetEngaged() && attackCooldown_ <= 0.0f && inRange) {
        StartAttack(plannedAttack, targetDirection);
        return;
    }

    // 固定技のプレビュー、空中、または移動の再使用待ち中は、次の移動を始めません。
    if (!debugPreviewAttackId_.empty() || !IsGrounded() || !locomotion_.CanBegin()) return;

    // 重い身体は長く溜めて短めに跳び、近距離では地面を離れず小さく位置を変える。
    const float preferredDistance = std::clamp(planned.recommendedTargetDistance, 4.5f, 11.0f);
    const bool farAway = targetDistance > preferredDistance + 2.3f;
    const bool tooClose = targetDistance < preferredDistance - 2.3f;
    const float side = (locomotionStepSerial_ & 1u) == 0u ? 1.0f : -1.0f;
    const engine::Vector3 lateral = {targetDirection.z * side, 0.0f, -targetDirection.x * side};
    const engine::Vector3 movement = farAway ? NormalizePlanar(targetDirection + lateral * 0.20f)
        : (tooClose ? NormalizePlanar(targetDirection * -1.0f + lateral * 0.18f) : lateral);
    const float stepDistance = farAway ? std::clamp(targetDistance - preferredDistance, 1.9f, 3.4f)
                                       : std::clamp(GetParameters()->speed * 0.55f, 0.65f, 1.15f);
    ++locomotionStepSerial_;

    BossGroundLocomotion::Style style;
    style.windup = farAway ? 0.25f : 0.18f;
    style.travelSpeed = farAway ? std::clamp(GetParameters()->speed * 3.8f, 3.7f, 7.5f)
                                  : std::clamp(GetParameters()->speed * 1.9f, 2.2f, 4.5f);
    style.minimumTravelTime = farAway ? 0.50f : 0.27f;
    style.maximumTravelTime = farAway ? 0.72f : 0.44f;
    style.landing = farAway ? 0.30f : 0.21f;
    style.cooldown = farAway ? 0.90f : 1.10f;
    style.arcHeight = farAway ? std::clamp(GetParameters()->jumpPower * 0.035f, 0.70f, 1.08f) : 0.0f;
    style.maximumHeightDifference = 1.2f;

    // 着地点を確保できない場合はすぐ再試行せず、短い待ち時間を置く。
    const engine::Vector3 current = GetTranslate();
    // 地形条件などで移動を開始できなかった場合は、0.38秒空けてから再試行します。
    if (!locomotion_.Begin(current, current + movement * stepDistance, farAway, this, style)) {
        locomotion_.Delay(0.38f);
    }
}

void EnemyMagmaSlime::UpdateLocomotion(float deltaTime, const engine::Vector3& targetDirection,
                                      engine::Vector3& velocity) {
    // 地上移動器の確定位置を反映し、基底モーターに位置を重ねて更新させない。
    const BossGroundLocomotion::Frame frame = locomotion_.Advance(deltaTime);
    // 移動器が位置を確定しない段階では、速度と接地状態をここで上書きしません。
    if (!frame.scripted) return;

    SetTranslate(frame.position);
    SetGrounded(!frame.airborne);
    scriptedMovementThisFrame_ = true;
    velocity = {};
    const bool landing = locomotion_.GetPhase() == BossGroundLocomotion::Phase::kLanding;
    UpdateFacing(locomotion_.IsLeap() && !landing ? locomotion_.GetDirection() : targetDirection);

    // 踏み切りと着地だけ地面を再取得して土煙を発生させる。
    if (frame.takeoff || frame.landed) {
        const engine::Vector3 ground = combatGroundQuery::FindGroundPoint(
            frame.position, this, {4.0f, 12.0f}, frame.position.y) + engine::Vector3{0.0f, 0.04f, 0.0f};
        bossGroundHopVfx::EmitPuff(ground, frame.landed ? engine::Vector4{1.0f, 0.52f, 0.18f, 0.78f}
                                                         : engine::Vector4{1.0f, 0.52f, 0.18f, 0.62f},
                                   frame.landed ? 2.6f : 2.3f, frame.landed);
    }
}

// 照準と攻撃タイマーを確定し、設置技の予告位置と詠唱演出を準備します。
void EnemyMagmaSlime::StartAttack(AttackKind kind, const engine::Vector3& targetDirection) {
    // 攻撃種別が未決定なら、移動と攻撃タイマーの状態を変更しません。
    if (kind == AttackKind::kNone) {
        return;
    }

    locomotion_.Reset();

    currentAttack_ = kind;
    attackState_ = AttackState::kWindup;
    lockedDirection_ = NormalizePlanar(targetDirection);
    lockedTargetPosition_ = GetTarget() ? FindGroundPoint(GetTarget()->GetWorldPosition())
                                    : FindGroundPoint(GetTranslate() + lockedDirection_ * 7.0f);
    attackStartPosition_ = GetTranslate();
    attackEndPosition_ = attackStartPosition_;

    // 詠唱の残り秒数と連射番号を初期化し、前回の命中済み状態を持ち越さない。
    const EnemyAttackDefinition& attack = GetCurrentAttackDefinition();
    attackStateDuration_ = (std::max)(0.12f, attack.windupDuration);
    attackTimer_ = attackStateDuration_;
    actionTimer_ = 0.0f;
    effectTimer_ = 0.0f;
    actionIndex_ = 0;
    warningTriggered_ = false;
    attackDamageApplied_ = false;
    rushImpactSpawned_ = false;
    slamImpactSpawned_ = false;

    // 砲撃と噴火は予定地点を先に作り、予兆と実行で同じ候補を使う。
    if (kind == AttackKind::kMagmaMortar) {
        mortarPatternPhase_ =
            std::fmod(idleTimer_ * 1.731f + static_cast<float>(automaticAttackSerial_) * 0.917f, kPi * 2.0f);
        PrepareMortarTargets();
    // 噴火技の場合は、砲撃の弾道とは別に火柱候補の地点を準備します。
    } else if (kind == AttackKind::kEruptionField) {
        PrepareEruptionTargets();
    }

    // 火柱は地上の位置に専用魔法陣を配置する。
    if (kind == AttackKind::kFireColumn) {
        PrepareFireColumnTargets();
        CreateFireColumnSigils();
    }

    // 突進とプレス以外の術式には、共通の術者中心の詠唱演出を使います。
    if (kind == AttackKind::kMagmaMortar || kind == AttackKind::kEruptionField || kind == AttackKind::kLavaSpiral ||
        kind == AttackKind::kFireColumn || kind == AttackKind::kMagmaFlood) {
        BossChantStyle style;
        style.sigilEffectPath = kind == AttackKind::kFireColumn ? kFireChantEffect : kMagmaChantEffect;
        style.motePreset = kFireChantMotesPreset;
        style.releasePreset = kind == AttackKind::kMagmaFlood ? magmaAttackVfx::kFlood.Resolve(&attack) : "";
        style.accentColor =
            kind == AttackKind::kFireColumn ? engine::Vector4{1.0f, 0.42f, 0.08f, 1.0f} : engine::Vector4{0.95f, 0.20f, 0.035f, 1.0f};
        style.radius = std::clamp(baseScale_.x * 1.35f, 2.4f, 4.0f);
        style.fadeStart = (std::max)(0.0f, attack.activeDuration - 0.22f);
        chantVisual_ = std::make_unique<BossChantVisual>();

        // 詠唱演出の準備に失敗したら表示実体を外し、攻撃状態の進行を続けます。
        if (!chantVisual_->Start(GetObjectCommon(), std::move(style))) {
            chantVisual_.reset();
        }
    }

    // 技の選択順は実戦時だけ進め、固定技のプレビューでは変えない。
    if (debugPreviewAttackId_.empty()) {
        lastAutomaticAttack_ = kind;
        ++attackCycleIndex_;
        ++automaticAttackSerial_;
    }
}

void EnemyMagmaSlime::UpdateAttack(float deltaTime, const engine::Vector3& targetDirection, float targetDistance,
                                   engine::Vector3& velocity) {
    // 警告の終盤で照準を固定し、実行・硬直中は各段階の処理だけを進める。
    if (attackState_ == AttackState::kWindup) {
        // 警告までの残り時間より前で対象がいる場合に、方向と着弾地点を追い直します。
        if (attackTimer_ > GetCurrentAttackDefinition().warningLeadTime && GetTarget()) {
            lockedDirection_ = NormalizePlanar(targetDirection);
            lockedTargetPosition_ = FindGroundPoint(GetTarget()->GetWorldPosition());

            // 砲撃の照準を追っている間は、その照準から落下地点も組み直します。
            if (currentAttack_ == AttackKind::kMagmaMortar) {
                PrepareMortarTargets();
            // 噴火技の照準更新では、発生候補の地点を置き直します。
            } else if (currentAttack_ == AttackKind::kEruptionField) {
                PrepareEruptionTargets();
            }
        }

        UpdateFacing(lockedDirection_);
        UpdateAttackPattern(AttackState::kWindup, deltaTime, targetDistance, velocity);
        return;
    }

    // 攻撃の実行中は、すでに確定した照準を使って技の本体を進めます。
    if (attackState_ == AttackState::kActive) {
        UpdateAttackPattern(AttackState::kActive, deltaTime, targetDistance, velocity);
        return;
    }

    // 硬直中は水平移動と予告を止め、タイマーだけを進めます。
    if (attackState_ == AttackState::kRecovery) {
        attackTimer_ = (std::max)(0.0f, attackTimer_ - deltaTime);
        velocity.x = 0.0f;
        velocity.z = 0.0f;
        HideAttackTelegraph();

        // 硬直が終了した時点で、攻撃専用のフラグと詠唱表示を片付けます。
        if (attackTimer_ <= 0.0f) {
            FinishAttack();
        }
    }
}

void EnemyMagmaSlime::UpdateSlamBodyPose() {
    // 火山プレスの描画行列が用意されていなければ、姿勢はコード側の変形に任せます。
    if (!slamVisualPose_) return;
    // 毎回初期化し、火山プレスをやめた後に直前の変形が残らないようにする。
    *slamVisualPose_ = engine::Math::MakeIdentity4x4();
    // 火山プレス以外では初期化した行列のまま戻ります。
    if (currentAttack_ != AttackKind::kVolcanicSlam) return;

    // 技の実時間を、予兆・実行・硬直それぞれを1秒分で扱うクリップの時刻へ写す。
    float phaseTime = 0.0f;
    const float progress = 1.0f - std::clamp(attackTimer_ / (std::max)(attackStateDuration_, 0.01f), 0.0f, 1.0f);
    if (attackState_ == AttackState::kWindup) phaseTime = progress;
    else if (attackState_ == AttackState::kActive) phaseTime = 1.0f + progress;
    else if (attackState_ == AttackState::kRecovery) phaseTime = 2.0f + progress;
    // 待機状態には専用クリップを適用しません。
    else return;

    BodyAnimationClip::Sample pose;
    // クリップから姿勢を取得できなければ、変形の無い行列を保ちます。
    if (!slamBodyClip_.Evaluate(phaseTime, false, pose)) return;
    // 指定した高さを支点に傾けられるよう、せん断で生じる位置ずれを相殺する。
    engine::Matrix4x4 shear = engine::Math::MakeIdentity4x4();
    shear.m[1][0] = pose.shear.x;
    shear.m[1][2] = pose.shear.z;
    shear.m[3][0] = -pose.shearPivotY * pose.shear.x;
    shear.m[3][2] = -pose.shearPivotY * pose.shear.z;
    *slamVisualPose_ = engine::Math::Multiply(
        shear, engine::Math::MakeAffineMatrix(pose.scale, pose.rotate, pose.translate));
}

void EnemyMagmaSlime::UpdateAttackPattern(AttackState phase, float deltaTime, float targetDistance, engine::Vector3& velocity) {
    // このフレームの段階を引数で受け取り、技の処理が終わってから次の段階へ移る。
    const bool windup = phase == AttackState::kWindup;
    const EnemyAttackDefinition* attack = windup ? &GetCurrentAttackDefinition() : nullptr;

    // 現在の段階の残り時間を減らす。発射間隔は実行中だけ進め、演出の間隔は両方の段階で進める。
    attackTimer_ = (std::max)(0.0f, attackTimer_ - deltaTime);
    // 発射や設置の間隔は、予兆が終わって実行に入った後にだけ減らします。
    if (!windup) {
        actionTimer_ -= deltaTime;
    }
    effectTimer_ -= deltaTime;

    // 予兆の表示に、開始を0・終了を1とした進行率を渡す。
    const float progress = windup
        ? 1.0f - std::clamp(attackTimer_ / (std::max)(attackStateDuration_, 0.01f), 0.0f, 1.0f)
        : 0.0f;
    const engine::Vector4 warningColor = {1.0f, 0.19f, 0.015f, 0.84f};

    // 同じ技の予兆と実行を並べ、表示した範囲と実際に発生する攻撃を照合できるようにする。
    switch (currentAttack_) {
    case AttackKind::kMagmaMortar:
        // 予兆と砲撃で同じ着弾候補を使う。発射側は固定済みの地点を順に参照する。
        if (windup) {
            ShowAttackTelegraphImpactAreas(mortarTargets_.data(), mortarTargets_.size(), attack->radius, progress,
                                           warningColor);
        } else {
            // 低フレームレートでも発射を取りこぼさないよう、時刻を過ぎた弾を着弾地点の順に撃つ。
            while (actionIndex_ < static_cast<std::int32_t>(mortarTargets_.size()) && actionTimer_ <= 0.0f) {
                LaunchNextMagmaBlob();
                ++actionIndex_;
                actionTimer_ += kMortarInterval;
            }
        }
        break;

    case AttackKind::kLavaRush:
        // 予兆は進行方向を線で示す。実行中は UpdateRush() が開始時に決めた終点まで動かす。
        if (windup) {
            ShowAttackTelegraphLine(GetTranslate(), lockedDirection_, attack->maxRange, attack->radius * 2.0f, progress,
                                    warningColor);
        // 予告を終えた実行中に、突進の位置と攻撃処理を進めます。
        } else {
            UpdateRush(deltaTime, targetDistance, velocity);
        }
        break;

    case AttackKind::kVolcanicSlam:
        // 固定した地点を円で予告し、実行中は UpdateVolcanicSlam() が跳躍の着地に衝撃を合わせる。
        if (windup) {
            ShowAttackTelegraphCircle(lockedTargetPosition_, attack->radius, progress, warningColor);
        // 予告を終えたら、跳んで接地するまでの位置と衝撃を更新します。
        } else {
            UpdateVolcanicSlam(deltaTime, velocity);
        }
        break;

    case AttackKind::kEruptionField:
        // 予告した地点へ順番に火柱を立てる。遅いフレームでは予定時刻を過ぎた分まで生成する。
        if (windup) {
            ShowAttackTelegraphImpactAreas(eruptionTargets_.data(), eruptionTargets_.size(), attack->radius, progress,
                                           warningColor);
        // 噴火の実行中は、火柱ごとの発生間隔を過ぎた数を順に処理します。
        } else {
            while (actionIndex_ < static_cast<std::int32_t>(eruptionTargets_.size()) && actionTimer_ <= 0.0f) {
                SpawnNextMagmaPillar();
                ++actionIndex_;
                actionTimer_ += kEruptionInterval;
            }
        }
        break;

    case AttackKind::kLavaSpiral:
        // 渦は BeginActive() から起動する。発動中はボスの水平移動を止める。
        if (windup) {
            ShowAttackTelegraphCircle(GetTranslate(), attack->maxRange, progress, warningColor);
        // 渦の実行中は着弾判定をここで生成せず、術者の水平速度を止めます。
        } else {
            velocity.x = 0.0f;
            velocity.z = 0.0f;
        }
        break;

    case AttackKind::kFireColumn:
        // 予兆の魔法陣は switch の後で UpdateFireColumnSigils() が更新する。
        if (!windup) {
            velocity.x = 0.0f;
            velocity.z = 0.0f;
            UpdateFireColumn(deltaTime);
        }
        break;

    case AttackKind::kMagmaFlood:
        // 溶岩流は BeginActive() から起動する。ここでは発動中のボスをその場に留める。
        if (windup) {
            ShowAttackTelegraphLine(GetTranslate(), lockedDirection_, attack->maxRange, attack->radius * 2.0f, progress,
                                    warningColor);
        // 溶岩流の実行中は、前進する溶岩とは別にボスの水平速度を止めます。
        } else {
            velocity.x = 0.0f;
            velocity.z = 0.0f;
        }
        break;

    default:
        break;
    }

    if (windup) {
        // 火柱は専用の魔法陣で発生地点を示すため、後続の共通の溜め粒子と警告通知は使わない。
        if (currentAttack_ == AttackKind::kFireColumn) {
            PrepareFireColumnTargets();
            UpdateFireColumnSigils(deltaTime);
        }

        // 技の指定素材を優先し、未指定なら共通素材を使う。発動が近づくほど粒子の間隔を詰める。
        if (effectTimer_ <= 0.0f && currentAttack_ != AttackKind::kFireColumn) {
            const char* preset = attack->windupVfx.empty() ? kChargePreset : attack->windupVfx.c_str();
            EmitDirectedPreset(preset, GetWorldPosition() + engine::Vector3{0.0f, baseScale_.y * 0.55f, 0.0f},
                               {-lockedDirection_.x * 0.18f, 1.0f, -lockedDirection_.z * 0.18f}, 0.82f + progress * 0.72f);
            effectTimer_ += (std::max)(0.055f, 0.14f - progress * 0.075f);
        }

        // 残り時間が警告のしきい値に達したら、その攻撃の警告を一度だけ処理する。
        if (!warningTriggered_ && attackTimer_ <= attack->warningLeadTime) {
            // 火柱以外の技だけ、共通の警告を発生させます。
            if (currentAttack_ != AttackKind::kFireColumn) {
                TriggerAttackTelegraphCue({1.0f, 0.18f, 0.015f, 1.0f});
            }

            warningTriggered_ = true;
        }

        // このフレームの予兆と警告を更新し終えてから、攻撃を起動する。
        if (attackTimer_ <= 0.0f) {
            BeginActive();
        }
    } else if (attackTimer_ <= 0.0f) {
        // 今フレーム分の発射や判定を処理した後で、硬直へ移る。
        BeginRecovery();
    }
}

void EnemyMagmaSlime::BeginActive() {
    // 技定義の有効時間を始め、種類に応じた設置・突進・プレスの初回処理を起動する。
    const EnemyAttackDefinition& attack = GetCurrentAttackDefinition();
    attackState_ = AttackState::kActive;
    attackStateDuration_ = (std::max)(0.16f, attack.activeDuration);
    attackTimer_ = attackStateDuration_;
    actionTimer_ = 0.0f;
    effectTimer_ = 0.0f;
    actionIndex_ = 0;
    HideAttackTelegraph();

    // 溶岩突進では発動時に進路と突進終点を確定します。
    if (currentAttack_ == AttackKind::kLavaRush) {
        BeginRush();
    // 火山プレスでは、指定地点へ向かう跳躍を始めます。
    } else if (currentAttack_ == AttackKind::kVolcanicSlam) {
        BeginVolcanicSlam();
    // 渦は発動の境目に溶岩を起動し、以降は溶岩側の更新で進めます。
    } else if (currentAttack_ == AttackKind::kLavaSpiral) {
        BeginLavaSpiral();
    // 火柱は予告で用意した位置の演出を実行状態にします。
    } else if (currentAttack_ == AttackKind::kFireColumn) {
        BeginFireColumn();
    // 溶岩流は発動時の向きと位置を基準に展開します。
    } else if (currentAttack_ == AttackKind::kMagmaFlood) {
        BeginMagmaFlood();
    }
}

void EnemyMagmaSlime::BeginRecovery() {
    // 技定義の硬直と再使用待ちを設定し、火柱の一時描画物を片付ける。
    const EnemyAttackDefinition& attack = GetCurrentAttackDefinition();
    attackState_ = AttackState::kRecovery;
    attackStateDuration_ = (std::max)(0.10f, attack.recoveryDuration);
    attackTimer_ = attackStateDuration_;
    attackCooldown_ = (std::max)(0.22f, attack.cooldown);
    landingPulseTimer_ = 0.34f;
    SetGrounded(true);
    HideAttackTelegraph();

    for (auto& flame : fireColumnVisuals_)
        flame.reset();
    for (auto& sigil : fireColumnSigils_)
        sigil.reset();
    for (auto& link : fireColumnLinks_)
        link.reset();
}

void EnemyMagmaSlime::FinishAttack() {
    // 次の技へ前回の警告・着弾フラグを持ち越さないよう、攻撃状態を初期化する。
    currentAttack_ = AttackKind::kNone;
    attackState_ = AttackState::kIdle;
    attackTimer_ = 0.0f;
    actionTimer_ = 0.0f;
    effectTimer_ = 0.0f;
    actionIndex_ = 0;
    warningTriggered_ = false;
    attackDamageApplied_ = false;
    rushImpactSpawned_ = false;
    slamImpactSpawned_ = false;
    chantVisual_.reset();
}

void EnemyMagmaSlime::PrepareAttackCycle() {
    // 全技の巡回順を混ぜ、直前の技が次巡の先頭に続かないようにする。
    attackCycle_ = {
        AttackKind::kMagmaMortar, AttackKind::kLavaRush,   AttackKind::kVolcanicSlam, AttackKind::kEruptionField,
        AttackKind::kLavaSpiral,  AttackKind::kFireColumn, AttackKind::kMagmaFlood,
    };

    enemyAttackSelection::Shuffle(std::span{attackCycle_}.first(attackCycle_.size()), lastAutomaticAttack_, attackRandom_);

    attackCycleIndex_ = 0;
}

EnemyMagmaSlime::AttackKind EnemyMagmaSlime::ResolveAutomaticAttack(float targetDistance) {
    // 現在の距離で届く技を巡回順から探し、使用可能な候補を先頭へ繰り上げる。
    // 巡回配列の末尾を使い切っていたら、次の巡の選択順を作ります。
    if (attackCycleIndex_ >= attackCycle_.size()) {
        PrepareAttackCycle();
    }

    return enemyAttackSelection::SelectInRange(
        std::span{attackCycle_}.first(attackCycle_.size()), attackCycleIndex_, lastAutomaticAttack_,
        [&](AttackKind kind) {
            const auto& attack = GetAttackDefinition(GetAttackId(kind));
            return targetDistance >= attack.minRange && targetDistance <= attack.maxRange;
        });
}

EnemyMagmaSlime::AttackKind EnemyMagmaSlime::ResolveDebugAttack() const {
    return enemyAttackSelection::FindKind(GetAttackEntries(), debugPreviewAttackId_, AttackKind::kNone);
}

std::span<const enemyAttackSelection::Entry<EnemyMagmaSlime::AttackKind>>
EnemyMagmaSlime::GetAttackEntries() const {
    // 本編のID参照とエディターの攻撃指定で同じ対応を使う。
    static constexpr enemyAttackSelection::Entry<AttackKind> kEntries[] = {
        {AttackKind::kMagmaMortar, kMagmaMortarAttackId},
        {AttackKind::kLavaRush, kLavaRushAttackId},
        {AttackKind::kVolcanicSlam, kVolcanicSlamAttackId},
        {AttackKind::kEruptionField, kEruptionFieldAttackId},
        {AttackKind::kLavaSpiral, kLavaSpiralAttackId},
        {AttackKind::kFireColumn, kFireColumnAttackId},
        {AttackKind::kMagmaFlood, kMagmaFloodAttackId},
    };
    return kEntries;
}

const char* EnemyMagmaSlime::GetAttackId(AttackKind kind) const {
    // 未対応の種類に対する既定IDも敵ごとの従来値を維持する。
    return enemyAttackSelection::FindId(GetAttackEntries(), kind, "");
}

const EnemyAttackDefinition& EnemyMagmaSlime::GetCurrentAttackDefinition() const {
    // 実行中の技が参照する攻撃時間・射程・ダメージの定義を返す。
    return GetAttackDefinition(GetAttackId(currentAttack_));
}

std::unique_ptr<engine::Object3D> EnemyMagmaSlime::CreateMagmaVisual(const std::string& name, const std::string& model,
                                                             std::int32_t materialType, const engine::Vector4& color,
                                                             float emissive) const {
    // 一時演出は敵本体の衝突対象にせず、種類に応じてマグマ・炎・衝撃波の材質を設定する。
    // 3Dの共通資源が使えない場合は、表示専用の実体を生成しません。
    if (!GetObjectCommon()) {
        return nullptr;
    }

    auto visual = std::make_unique<engine::Object3D>();
    visual->Initialize(GetObjectCommon());
    visual->SetName(name);
    visual->SetClassName("EnemyVisualPart");
    visual->SetModel(model);
    visual->SetColliderType(engine::ColliderType::kNone);
    visual->SetCollisionAttribute(0);
    visual->SetCollisionMask(0);
    visual->SetMaterialType(materialType);
    visual->SetBlendMode(BlendMode::kNormal);
    visual->SetSelectedLighting(0);
    visual->SetEnableLighting(false);
    visual->SetEnableEnvMap(false);
    visual->SetColor(color);
    visual->SetEmissive(emissive);
    visual->SetRoughness(0.20f);
    visual->SetMetallic(0.0f);

    // 表示を担うメッシュがある場合は、材質向けのGPU領域を確認します。
    if (engine::MeshRenderer* renderer = visual->GetMeshRenderer()) {
        // 材質パラメーターを取得できた場合に、衝撃波・炎・溶岩ごとの波と強度を設定します。
        if (engine::MeshRenderer::WaterParamForGpu* parameter = renderer->GetWaterParamData()) {
            const bool isShockwave = materialType == kShockwaveMaterialType;
            const bool isFire = materialType == kFireMaterialType;
            // 通常の溶岩材質を基準にし、衝撃波と炎では波・模様・強度をまとめて選ぶ。
            float waveSpeed = 3.2f;
            float waveHeight = 0.42f;
            float effectScale = 1.06f;
            float effectIntensity = 1.34f;
            if (isShockwave) {
                waveSpeed = 1.8f;
                waveHeight = 0.16f;
                effectScale = 0.74f;
                effectIntensity = 1.55f;
            } else if (isFire) {
                waveSpeed = 3.4f;
                waveHeight = 0.58f;
                effectScale = 1.0f;
                effectIntensity = 1.48f;
            }
            parameter->waveSpeed = waveSpeed;
            parameter->waveHeight = waveHeight;
            parameter->waveFrequency = isShockwave ? 5.4f : 2.8f;
            parameter->flowSpeedX = 0.06f;
            parameter->flowSpeedY = isFire ? 1.04f : 0.22f;
            parameter->effectType = isShockwave ? 1.0f : 0.0f;
            parameter->effectScale = effectScale;
            parameter->effectSoftness = isFire ? 0.48f : 0.36f;
            parameter->effectIntensity = effectIntensity;
            parameter->billboardScale = 1.0f;

            // 炎の場合は、模様の軸方向の倍率と開始UVも設定します。
            if (isFire) {
                parameter->effectScaleX = 0.46f;
                parameter->effectScaleY = 1.0f;
                parameter->effectScaleZ = 1.0f;
                parameter->uvOffsetX = 2.17f;
                parameter->uvOffsetY = 5.31f;
            }
        }
    }

    return visual;
}

engine::Vector3 EnemyMagmaSlime::FindGroundPoint(const engine::Vector3& samplePosition) const {
    // 予告と着弾が地形から浮かないよう、指定地点の下にある接地面を探す。
    return combatGroundQuery::FindGroundPoint(samplePosition, this, {12.0f, 28.0f}, GetTranslate().y);
}

void EnemyMagmaSlime::DamageTargetAt(const engine::Vector3& center, float radius, float damage, const engine::Vector3& knockback,
                                     float burnDuration, float burnTickInterval, float burnTickDamage) {
    // 平面半径と高さ差の両方を満たした対象へ、火傷を含む一回の被弾イベントを送る。
    // 対象がいないか、死亡または非表示の場合は被弾イベントを送らないようにします。
    if (!GetTarget() || GetTarget()->IsDead() || !GetTarget()->GetIsVisible()) {
        return;
    }

    const engine::Vector3 targetPosition = GetTarget()->GetWorldPosition();
    // 許容の高低差か水平半径のどちらかを超えた対象は攻撃範囲外です。
    if (std::abs(targetPosition.y - center.y) > kPoolVerticalTolerance + 1.0f ||
        PlanarDistance(targetPosition, center) > radius) {
        return;
    }

    DamageEvent event;
    event.target = GetTarget();
    event.attacker = this;
    event.damageAmount = damage;
    event.damageType = DamageType::kFire;
    event.knockbackVelocity = knockback;
    event.statusEffect = MakeBurningStatus(burnDuration, burnTickInterval, burnTickDamage, "shared/status_burning_flame");
    EventManager::GetInstance()->Dispatch(event);
}

void EnemyMagmaSlime::ApplySlimeAnimation(float deltaTime) {
    // 移動の弾みと技ごとの変形を合成し、配置時の倍率を基準に戻す。
    slimeBounceAnimator::Params params;
    params.speedForFullBounce = 2.8f;
    params.idleAmplitude = 0.045f;
    params.moveAmplitude = 0.16f;
    params.hopFrequency = 4.9f;
    params.horizontalSquash = 0.19f;
    params.verticalStretch = 0.24f;
    params.airborneStretch = 0.34f;

    engine::Vector3 targetScale = slimeBounceAnimator::MakeScale(baseScale_, GetVelocity(), idleTimer_, IsGrounded(), params);
    engine::Vector3 targetRotation = {0.0f, GetRotation().y, 0.0f};

    // 専用移動は踏み切り前の圧縮、空中の伸び、着地後の圧縮を別の形で見せる。
    if (attackState_ == AttackState::kIdle && locomotion_.IsActive()) {
        const float progress = locomotion_.GetProgress();
        // 移動前の溜めでは、移動の進捗と跳躍の有無に合わせて体を押し縮めます。
        if (locomotion_.GetPhase() == BossGroundLocomotion::Phase::kWindup) {
            const float squash = SmoothStep01(progress) * (locomotion_.IsLeap() ? 0.24f : 0.10f);
            targetScale = {baseScale_.x * (1.0f + squash * 0.65f), baseScale_.y * (1.0f - squash),
                           baseScale_.z * (1.0f + squash * 0.65f)};
            targetRotation.x -= squash * 0.30f;
        // 移動区間では、姿勢を跳躍用か地上移動用に分けます。
        } else if (locomotion_.GetPhase() == BossGroundLocomotion::Phase::kTravel) {
            const float arc = std::sin(progress * kPi);
            // 跳躍中は体を細く伸ばし、弧の頂点付近で小さく傾けます。
            if (locomotion_.IsLeap()) {
                targetScale = {baseScale_.x * 0.96f, baseScale_.y * (1.07f + arc * 0.05f), baseScale_.z * 0.96f};
                targetRotation.x -= arc * 0.055f;
            // 地上をずれる場合は高さを保ち、水平の形と傾きを変えます。
            } else {
                targetScale = {baseScale_.x * (1.0f + arc * 0.035f), baseScale_.y,
                               baseScale_.z * (1.0f - arc * 0.020f)};
                targetRotation.z -= arc * 0.06f;
            }
        // 着地段階は、接地時の潰れを徐々に元へ戻します。
        } else {
            const float squash = (1.0f - SmoothStep01(progress)) * (locomotion_.IsLeap() ? 0.30f : 0.11f);
            targetScale = {baseScale_.x * (1.0f + squash * 0.75f), baseScale_.y * (1.0f - squash),
                           baseScale_.z * (1.0f + squash * 0.75f)};
        }
    } else if (attackState_ == AttackState::kIdle && IsGrounded()) {
        // 停止中は縦倍率を固定し、呼吸だけで足元が上下しないようにする。
        const float speedRate = std::clamp(slimeBounceAnimator::PlanarSpeed(GetVelocity()) / 2.8f, 0.0f, 1.0f);
        const float breath = std::sin(idleTimer_ * 1.8f) * 0.020f * (1.0f - speedRate);
        targetScale = {baseScale_.x * (1.0f + breath * 0.5f), baseScale_.y,
                       baseScale_.z * (1.0f + breath * 0.5f)};
    }

    // 予備動作は技ごとに異なる圧縮と震えを作り、攻撃前の種類を見分けられるようにする。
    if (slamVisualPose_ && currentAttack_ == AttackKind::kVolcanicSlam && attackState_ != AttackState::kIdle) {
        // 火山プレスはBody Clipを唯一の姿勢源にし、コード側の伸縮を重ねない。
        targetScale = baseScale_;
    // 専用クリップを使用していない技の予備動作は、コード側で技別に変形します。
    } else if (attackState_ == AttackState::kWindup) {
        const float progress = SmoothStep01(1.0f - attackTimer_ / (std::max)(attackStateDuration_, 0.01f));
        const float tremble = std::sin(progress * kPi * 15.0f) * progress * 0.035f;

        // 火山プレスの溜めは強く横に広げ、跳躍前の重さを見せます。
        if (currentAttack_ == AttackKind::kVolcanicSlam) {
            targetScale = {
                baseScale_.x * (1.0f + progress * 0.42f + tremble),
                baseScale_.y * (1.0f - progress * 0.42f),
                baseScale_.z * (1.0f + progress * 0.34f - tremble),
            };
        // 突進の溜めは低い構えと前への傾きを作ります。
        } else if (currentAttack_ == AttackKind::kLavaRush) {
            targetScale = {
                baseScale_.x * (1.0f + progress * 0.24f + tremble),
                baseScale_.y * (1.0f - progress * 0.29f),
                baseScale_.z * (1.0f + progress * 0.18f - tremble),
            };

            targetRotation.x = -progress * 0.10f;
        // 連続噴火の溜めは横に広がる姿勢へ小さなひねりを加えます。
        } else if (currentAttack_ == AttackKind::kEruptionField) {
            targetScale = {
                baseScale_.x * (1.0f + progress * 0.34f + tremble),
                baseScale_.y * (1.0f - progress * 0.30f),
                baseScale_.z * (1.0f + progress * 0.34f - tremble),
            };

            targetRotation.z = tremble * 1.8f;
        // 溶岩の渦の溜めは、左右の幅を交互に変えてねじりを表現します。
        } else if (currentAttack_ == AttackKind::kLavaSpiral) {
            const float coil = std::sin(progress * kPi * 6.0f) * progress;
            targetScale = {
                baseScale_.x * (1.0f + progress * 0.22f + coil * 0.06f),
                baseScale_.y * (1.0f - progress * 0.18f),
                baseScale_.z * (1.0f + progress * 0.22f - coil * 0.06f),
            };

            targetRotation.z = coil * 0.12f;
        // 個別の予備姿勢を持たない技は、共通の溜め姿勢を使います。
        } else {
            targetScale = slimeBounceAnimator::MakeChargeSquash(baseScale_, progress, idleTimer_, 0.92f);
            targetRotation.z = tremble * 1.2f;
        }
    } else if (attackState_ == AttackState::kActive) {
        // 発動中は着地・突進・連射の動きに合わせた形へ切り替える。
        const float progress = 1.0f - attackTimer_ / (std::max)(attackStateDuration_, 0.01f);

        // 連続砲撃を、本体が上下に反動を繰り返す形で見せる。
        if (currentAttack_ == AttackKind::kMagmaMortar) {
            const float recoil = std::sin(progress * kPi * 6.0f);
            targetScale = {
                baseScale_.x * (1.14f + std::abs(recoil) * 0.08f),
                baseScale_.y * (0.84f - recoil * 0.06f),
                baseScale_.z * (1.10f + recoil * 0.04f),
            };
        } else if (currentAttack_ == AttackKind::kLavaRush) {
            // 突進中は進行方向に伸ばして低く構え、小刻みに震わせる。
            const float speedPulse = std::sin(progress * kPi * 10.0f) * 0.035f;
            targetScale = {
                baseScale_.x * (1.18f + speedPulse),
                baseScale_.y * 0.72f,
                baseScale_.z * (1.42f - speedPulse),
            };

            targetRotation.x = -0.12f;
        } else if (currentAttack_ == AttackKind::kVolcanicSlam) {
            // プレスが体のクリップを使えないときは、跳躍で伸び、着地時に潰す。
            const float airborne = std::sin(progress * kPi);
            const float landing = SmoothStep01(std::clamp((progress - 0.86f) / 0.14f, 0.0f, 1.0f));
            targetScale = {
                baseScale_.x * (0.88f + landing * 0.45f),
                baseScale_.y * (1.34f + airborne * 0.22f - landing * 0.62f),
                baseScale_.z * (0.88f + landing * 0.45f),
            };

            targetRotation.x = -airborne * 0.075f;
        } else if (currentAttack_ == AttackKind::kEruptionField) {
            // 連続噴火の発動中は速い脈動と左右への傾きを加える。
            const float eruptionPulse = std::sin(progress * kPi * 18.0f);
            targetScale = {
                baseScale_.x * (1.18f + std::abs(eruptionPulse) * 0.09f),
                baseScale_.y * (0.78f - eruptionPulse * 0.08f),
                baseScale_.z * (1.18f + std::abs(eruptionPulse) * 0.09f),
            };

            targetRotation.z = eruptionPulse * 0.065f;
        } else if (currentAttack_ == AttackKind::kLavaSpiral) {
            // 渦の発動中は左右の幅と傾きを交互に変え、ねじる動きを加える。
            const float spiralPulse = std::sin(progress * kPi * 12.0f);
            targetScale = {
                baseScale_.x * (1.22f + spiralPulse * 0.08f),
                baseScale_.y * (0.76f - std::abs(spiralPulse) * 0.06f),
                baseScale_.z * (1.22f - spiralPulse * 0.08f),
            };

            targetRotation.z = std::sin(progress * kPi * 4.0f) * 0.16f;
        }
    } else if (attackState_ == AttackState::kRecovery || landingPulseTimer_ > 0.0f) {
        // 硬直と着地後の反動を同じ減衰で処理し、通常形状へ戻す。
        const float remaining = attackState_ == AttackState::kRecovery
                                    ? std::clamp(attackTimer_ / (std::max)(attackStateDuration_, 0.01f), 0.0f, 1.0f)
                                    : std::clamp(landingPulseTimer_ / 0.34f, 0.0f, 1.0f);
        const float bounce = std::sin((1.0f - remaining) * kPi * 2.6f) * remaining;
        targetScale = {
            baseScale_.x * (1.0f + bounce * 0.18f),
            baseScale_.y * (1.0f - bounce * 0.25f),
            baseScale_.z * (1.0f + bounce * 0.18f),
        };
    }

    // 攻撃も専用移動もない場合だけ、通常移動の速度を姿勢へ重ねます。
    if (attackState_ == AttackState::kIdle && !locomotion_.IsActive()) {
        slimeBounceAnimator::ApplyTravelPose(targetScale, targetRotation, baseScale_, GetVelocity(),
                                             GetRotation().y - kMagmaSlimeModelYawOffset, 3.4f, 0.065f, 0.045f);
    }

    // 計算した姿勢に被弾反応を重ね、フレーム時間で補間して急な形の変化を抑える。
    ApplyDamageReactionPose(targetScale, targetRotation);
    const float rate = 1.0f - std::exp(-deltaTime * 12.0f);
    const engine::Vector3 currentScale = GetScale();
    SetScale({
        engine::Math::Lerp(currentScale.x, targetScale.x, rate),
        engine::Math::Lerp(currentScale.y, targetScale.y, rate),
        engine::Math::Lerp(currentScale.z, targetScale.z, rate),
    });
    const engine::Vector3 currentRotation = GetRotation();
    SetRotation({
        engine::Math::Lerp(currentRotation.x, targetRotation.x, rate),
        currentRotation.y,
        engine::Math::Lerp(currentRotation.z, targetRotation.z, rate),
    });
}

void EnemyMagmaSlime::UpdateFacing(const engine::Vector3& direction) {
    // 水平方向が有効なときだけ向きを補間し、停止時の姿勢は保持する。
    if (direction.x * direction.x + direction.z * direction.z <= 0.0001f) {
        return;
    }

    const float targetYaw = std::atan2(direction.x, direction.z) + kMagmaSlimeModelYawOffset;
    SetRotationY(engine::Math::LerpShortAngle(GetRotation().y, targetYaw, 0.10f));
}

void EnemyMagmaSlime::UpdateBodyHeat(float deltaTime) {
    // 詠唱や着地の段階に応じて本体の熱・発光を変化させる。
    ambientEmberTimer_ -= deltaTime;
    const bool attacking = attackState_ == AttackState::kWindup || attackState_ == AttackState::kActive;
    const float attackGlow = attacking ? 0.58f : 0.0f;
    const float pulse = std::sin(idleTimer_ * (attacking ? 9.0f : 3.8f)) * (attacking ? 0.16f : 0.08f);
    SetEmissive(1.16f + attackGlow * 0.62f + pulse * 0.45f);
    SetDefaultColor({
        1.0f,
        0.94f + attackGlow * 0.05f,
        0.84f + attackGlow * 0.07f + pulse * 0.04f,
        1.0f,
    });

    SetColor(GetDefaultColor());

    // 火の粉の周期が来たら本体の上に粒子を出し、攻撃中は強さと頻度を増やします。
    if (ambientEmberTimer_ <= 0.0f) {
        EmitDirectedPreset(kCoreEmberPreset, GetWorldPosition() + engine::Vector3{0.0f, baseScale_.y * 0.58f, 0.0f},
                           {0.0f, 1.0f, 0.0f}, attacking ? 1.22f : 0.78f);
        ambientEmberTimer_ += attacking ? 0.085f : 0.19f;
    }
}

void EnemyMagmaSlime::EmitPreset(const char* presetName, const engine::Vector3& position) const {
    // GPU粒子が利用できる場合だけ指定プリセットを発生させる。
    GpuParticleManager* particles = GpuParticleManager::GetInstance();

    // 粒子初期化済みでプリセット名が空でない場合だけ、指定位置で発生させます。
    if (particles && particles->IsInitialized() && presetName && presetName[0] != '\0') {
        particles->Emit(presetName, position);
    }
}

void EnemyMagmaSlime::EmitDirectedPreset(const char* presetName, const engine::Vector3& position, const engine::Vector3& direction,
                                         float speedScale) const {
    // 発射方向と速度倍率を持つ粒子だけを、GPU粒子管理器が有効な場合に放出する。
    GpuParticleManager* particles = GpuParticleManager::GetInstance();

    // 粒子が初期化され空でない名前が指定された場合に、向きと速度倍率を渡します。
    if (particles && particles->IsInitialized() && presetName && presetName[0] != '\0') {
        particles->EmitDirected(presetName, position, direction, speedScale);
    }
}

void EnemyMagmaSlime::SyncCollisionRadius() {
    // 現在の描画倍率の最大成分で割り、見た目を伸縮しても一定のワールド判定半径にします。
    const engine::Vector3 scale = GetScale();
    const float maximumScale = (std::max)({0.001f, std::abs(scale.x), std::abs(scale.y), std::abs(scale.z)});
    SetCollisionRadius(kGroundCollisionWorldRadius / maximumScale);
}

void EnemyMagmaSlime::ClearTransientVisuals() {
    // 敗北や技切替の後に設置物・火柱・詠唱陣が残らないよう、一時演出を消す。
    if (slamVisualPose_) *slamVisualPose_ = engine::Math::MakeIdentity4x4();
    ClearRushFlameVisuals();

    for (auto& flame : fireColumnVisuals_)
        flame.reset();
    for (auto& sigil : fireColumnSigils_)
        sigil.reset();
    for (auto& link : fireColumnLinks_)
        link.reset();
    chantVisual_.reset();
    magmaBlobs_.clear();
    magmaPools_.clear();
    magmaRings_.clear();
    magmaPillars_.clear();
    magmaSurges_.clear();
    magmaFloodCells_.clear();
}
