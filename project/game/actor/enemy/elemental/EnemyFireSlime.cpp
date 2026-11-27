#include <cstdint>
#include "EnemyFireSlime.h"
#include <numbers>
#include "game/actor/enemy/animation/SlimeBounceAnimator.h"
#include "game/system/combat/BulletManager.h"
#include "CollisionConfig.h"
#include "CollisionManager.h"
#include "game/system/rules/EventManager.h"
#include "GpuParticleManager.h"
#include "game/system/combat/HitEffectDirector.h"
#include "MeshEffectManager.h"
#include "SceneManager.h"
#include "VfxSequencer.h"
#include <algorithm>
#include <cmath>

namespace {
// 火炎ブレスと火球のID、跳ねと衝突の調整値、頭上とブレスの炎に使う素材。
constexpr const char* kBreathAttackId = "flame_breath";
constexpr const char* kFireballAttackId = "fireball";
constexpr float kGroundCollisionWorldRadius = 0.82f;
constexpr float kThrownCollisionWorldRadius = 1.18f;
constexpr float kMoveHopInterval = 0.28f;
constexpr float kMoveHopPower = 4.7f;
constexpr float kFireSlimeModelYawOffset = std::numbers::pi_v<float>;
constexpr float kHeadFlameHeight = 0.95f;
constexpr float kHeadFlameBaseScale = 0.78f;
constexpr std::int32_t kHeadFlameMaterialType = 11;
constexpr float kHeadFlameEffectType = 2.0f;
constexpr float kBreathFlameEffectType = 5.0f;
constexpr std::int32_t kBreathFlameVisualCount = 7;
constexpr const char* kBreathPreset = "shared/fire_slime_breath";
constexpr const char* kBreathEmberPreset = "shared/fire_slime_breath_embers";
constexpr const char* kCastPreset = "shared/fire_slime_cast";
constexpr const char* kHeadFlamePreset = "enemy/fire_slime/fire_slime_head_flame";
constexpr const char* kHeadEmberPreset = "enemy/fire_slime/fire_slime_head_embers";

engine::Vector3 NormalizePlanar(engine::Vector3 value) {
    // 目標と重なって方向を失った場合も、攻撃とエフェクトに有効な前方を返す。
    value.y = 0.0f;
    const float length = std::sqrt(value.x * value.x + value.z * value.z);

    // 水平距離が閾値以下なら、ゼロ除算を避けてZの正方向を返します。
    if (length <= 0.001f) {
        return {0.0f, 0.0f, 1.0f};
    }

    return {value.x / length, 0.0f, value.z / length};
}

// 火球の材質、軌跡、着弾エフェクトを指定倍率で構築します。
BulletVisualConfig MakeFireVisual(float scale) {
    // 火球の軌跡と着弾は弾の寿命に紐付け、単発発射でも残像を管理する。
    BulletVisualConfig visual;
    // シーケンスを使わない火球も、床・壁で停止したまま残さない。
    visual.destroyOnSolidCastHit = true;
    visual.materialType = 11;
    visual.blendMode = BlendMode::kNormal;
    visual.color = {1.0f, 0.34f, 0.07f, 0.96f};
    visual.emissive = 2.8f;
    visual.visualScale = scale;
    visual.effectType = 1.0f;
    visual.effectScale = 1.15f;
    visual.effectSoftness = 0.42f;
    visual.effectIntensity = 0.96f;
    visual.billboardScale = 0.68f;
    visual.trailPreset = "shared/fireball_trail_flame";
    visual.trailSecondaryPreset = "shared/fireball_trail_embers";
    visual.impactPreset = "shared/fireball_impact_flame";
    visual.impactSecondaryPreset = "shared/fireball_impact_sparks";
    visual.trailInterval = 0.030f;
    visual.trailSpeedScale = 1.0f;
    visual.trailReverseDirection = true;
    visual.impactDirected = false;
    return visual;
}

// 攻撃定義の燃焼時間・間隔・VFXを弾へ渡す状態異常設定に変換します。
StatusEffectApplication MakeStatusEffect(const EnemyAttackDefinition& attack) {
    // ダメージとは別に、攻撃 JSON で指定された燃焼の継続条件を弾へ渡す。
    StatusEffectApplication status;

    // 技定義が burning を指定した場合だけ、状態異常の種類を燃焼に設定します。
    if (attack.statusEffectType == "burning") {
        status.type = StatusEffectType::kBurning;
    }

    status.duration = attack.statusDuration;
    status.tickInterval = attack.statusTickInterval;
    status.tickDamage = attack.statusTickDamage;
    status.vfxPreset = attack.statusVfx;
    return status;
}

engine::Object3D* FindEnemyDamageTarget(engine::Object3D* object) {
    // 子メッシュとの接触も所有する敵本体への命中として扱う。
    for (engine::Object3D* current = object; current; current = current->GetParent()) {
        // 触れた実体から親を辿り、最初に敵本体と判定できた個体を返します。
        if (dynamic_cast<BaseEnemy*>(current)) {
            return current;
        }
    }

    return nullptr;
}
} // namespace

// 炎スライムの攻撃定義、描画クリップと球形の接触判定を準備します。
void EnemyFireSlime::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    BaseEnemy::Initialize(common, modelName);
    SetName("Enemy_FireSlime");
    SetEnemyType("FireSlime");
    ReloadAttackProfile();

    // 本体のIdle/移動/攻撃姿勢はControllerのBody Clipから再生する。
    // 読み込み失敗時は従来の手続きアニメーションへ戻す。
    SetAnimatorController("enemy_fire_slime");
    SetColor({1.0f, 1.0f, 1.0f, 1.0f});
    SetDefaultColor(GetColor());

    SetCollisionAttribute(engine::collisionCategories::Get("enemy"));
    SetCollisionMask(engine::collisionCategories::Get("player") | engine::collisionCategories::Get("ground") | engine::collisionCategories::Get("player_attack") | engine::collisionCategories::Get("player_bullet"));
    SetColliderType(engine::ColliderType::kSphere);
    SyncGroundCollisionRadius();
}

// 近距離ブレスと中距離火球を選び、物理の更新後に頭上の炎を追従させます。
void EnemyFireSlime::Update(float deltaTime) {
    // 無効・投擲中は通常 AI を進めず、解除すべき攻撃シーケンスを先に止める。
    if (UpdateInactiveState(deltaTime)) {
        fireballSequence_.Cancel();
        fireballWindupTimer_ = 0;
        return;
    }

    idleTimer_ += deltaTime;

    // 投擲・復帰を処理中なら攻撃シーケンスを止め、頭上の炎だけを新しい位置に合わせます。
    if (UpdateThrowRecoveryState(deltaTime)) {
        fireballSequence_.Cancel();
        fireballWindupTimer_ = 0;
        UpdateHeadFlame(deltaTime);
        return;
    }

    // 物理移動と姿勢を更新する前に、復元用のスケールと攻撃の残り時間を揃える。
    EnsureBaseScale();
    UpdateWildTimers(deltaTime);

    engine::Vector3 velocity = GetVelocity();
    velocity.x = 0.0f;
    velocity.z = 0.0f;

    UpdateWildBehavior(deltaTime, velocity);
    // 行動で決めた速度から跳ねと姿勢を反映し、共通の物理更新後に頭上の炎を追従させる。
    ApplyGroundMovementAndAnimation(deltaTime, velocity);
    BaseEnemy::Update(deltaTime);
    UpdateHeadFlame(deltaTime);
}

bool EnemyFireSlime::UpdateInactiveState(float deltaTime) {
    // 死亡・運搬・撃破演出では炎と予告の寿命を通常攻撃から切り離す。
    // 死亡または非表示なら、予告や炎を取り除き、次の有効化に攻撃の残りを持ち越しません。
    if (IsDead() || !GetIsVisible()) {
        HideAttackTelegraph();
        breathTimer_ = 0.0f;
        fireballWindupTimer_ = 0.0f;
        attackTimer_ = 0.0f;
        breathWarningTriggered_ = false;
        fireballWarningTriggered_ = false;
        fireballAimLocked_ = false;
        RequestRemoveHeadFlameVisual();
        RequestRemoveBreathFlameVisuals();
        SetVelocity({0.0f, 0.0f, 0.0f});
        return true;
    }

    // 運搬中は炎を再利用できるよう保持し、表示と攻撃予告だけを止める。
    if (IsCarried()) {
        HideAttackTelegraph();
        // 頭上の炎があれば、削除せず運搬中の描画だけ止めます。
        if (headFlameVisual_)
            headFlameVisual_->SetIsVisible(false);
        HideBreathFlameVisuals();
        idleTimer_ += deltaTime;
        return true;
    }

    // 撃破演出では炎を削除し、通常AIへ戻らずBaseEnemyの演出更新を続ける。
    if (ShouldHandleDefeatEffect()) {
        HideAttackTelegraph();
        RequestRemoveHeadFlameVisual();
        RequestRemoveBreathFlameVisuals();
        BaseEnemy::Update(deltaTime);
        return true;
    }

    return false;
}

bool EnemyFireSlime::UpdateThrowRecoveryState(float deltaTime) {
    // 投げられた間は着地前後で衝突半径を切り替え、攻撃 AI を休止する。
    if (IsThrowRecovering()) {
        // 投擲の物理で移動している間は、投擲用の大きい衝突半径を使います。
        if (IsThrownPhysics()) {
            SyncThrownCollisionRadius();
        // 投擲の物理が終わった後の復帰では、通常の地上半径へ戻します。
        } else {
            SyncGroundCollisionRadius();
        }

        BaseEnemy::Update(deltaTime);
        return true;
    }

    return false;
}

void EnemyFireSlime::EnsureBaseScale() {
    // 初回に配置スケールを保存し、投擲や攻撃後の復元基準にする。
    if (!hasBaseScale_) {
        baseScale_ = GetScale();
        hasBaseScale_ = true;
    }
}

void EnemyFireSlime::UpdateWildTimers(float deltaTime) {
    // 行動間隔と攻撃中の時間は負値を残さず、次の行動選択へ渡す。
    attackCooldown_ = (std::max)(0.0f, attackCooldown_ - deltaTime);
    attackTimer_ = (std::max)(0.0f, attackTimer_ - deltaTime);
}

void EnemyFireSlime::UpdateWildBehavior(float deltaTime, engine::Vector3& velocity) {
    // 本体を制御するシーケンス中に対象を失った場合は、代替の前方を使って終了まで進めます。
    if (!GetTarget() && fireballSequence_.ControlsActor()) {
        // 対象を失っても時計を止めず、条件評価と終了・後片付けまで進めます。
        UpdateFireballAttack(deltaTime, velocity, {0, 0, 1}, 0);
        return;
    }

    // 対象と行動パラメーターがある場合に、双方の配置座標から水平距離と向きを決めます。
    if (GetTarget() && GetParameters().has_value()) {
        engine::Vector3 toTarget = GetTarget()->GetTranslate() - GetTranslate();
        engine::Vector3 direction = NormalizePlanar(toTarget);
        const float distance = std::sqrt(toTarget.x * toTarget.x + toTarget.z * toTarget.z);

        // シーケンスが姿勢を制御していない場合にだけ、AI側で対象の方向へ旋回します。
        if (!fireballSequence_.ControlsActor())
            UpdateFacing(direction);
        UpdateCombatBehavior(deltaTime, velocity, direction, distance);
    }
}

void EnemyFireSlime::UpdateCombatBehavior(float deltaTime, engine::Vector3& velocity, const engine::Vector3& direction,
                                          float distance) {
    // 編集済みシーケンスを優先し、使わない攻撃だけ従来の距離別行動へ戻す。
    const EnemyAttackDefinition& breathAttack = GetAttackDefinition(kBreathAttackId);
    const EnemyAttackDefinition& fireballAttack = GetAttackDefinition(kFireballAttackId);
    const EnemyAttackDefinition* sequenceAttack = SelectSequenceAttack(distance);
    // 攻撃を準備・実行しておらず、発見リアクションが続く間は、水平移動と新しい攻撃を開始しません。
    if (breathTimer_ <= 0.0f && fireballWindupTimer_ <= 0.0f && !fireballSequence_.IsRunning() &&
        UpdateNoticeReaction(deltaTime, distance, GetDetectionRange(), direction)) {
        velocity.x = 0.0f;
        velocity.z = 0.0f;
        UpdateFacing(direction);
        return;
    }

    // 火球の溜めが残るかシーケンスが進行中なら、別の技は始めず火球を進めます。
    if (fireballWindupTimer_ > 0.0f || fireballSequence_.IsRunning()) {
        UpdateFireballAttack(deltaTime, velocity, direction, distance);
    // 火球が進行しておらずブレスの時間が残っていれば、その攻撃を継続します。
    } else if (breathTimer_ > 0.0f) {
        UpdateBreath(deltaTime, direction, distance);
    // 使用可能なシーケンス攻撃があり、再使用までの時間が終わっていれば優先して開始します。
    } else if (sequenceAttack && attackCooldown_ <= 0.0f) {
        selectedFireball_ = *sequenceAttack;
        StartFireballWindup(direction, distance);
    // シーケンス攻撃を選ばず、ブレスの距離範囲と再使用条件を満たす場合はブレスを始めます。
    } else if (distance >= breathAttack.minRange && distance <= breathAttack.maxRange && attackCooldown_ <= 0.0f) {
        StartBreath();
    // 実戦で従来方式の火球を使う設定なら、索敵範囲・火球の射程・再使用条件を確認して溜めを始めます。
    } else if (distance <= (std::min)(GetDetectionRange(), fireballAttack.maxRange) &&
               distance >= fireballAttack.minRange && attackCooldown_ <= 0.0f &&
               !fireballAttack.sequence.enabled && previewAttackId_.empty()) {
        selectedFireball_ = fireballAttack;
        StartFireballWindup(direction, distance);
    // 攻撃を開始できなくても索敵内なら、ブレスの間合いより遠い場合は接近し、近い場合は後退します。
    } else if (distance <= GetDetectionRange()) {
        const float speed = (std::max)(0.0f, GetParameters()->speed);
        const float approach = distance > breathAttack.maxRange * 0.78f ? 1.0f : -0.55f;
        velocity.x = direction.x * speed * approach;
        velocity.z = direction.z * speed * approach;
    // 索敵から外れた場合は、徘徊へ切り替えます。
    } else {
        UpdateWanderBehavior(deltaTime, velocity);
    }
}

void EnemyFireSlime::UpdateWanderBehavior(float deltaTime, engine::Vector3& velocity) {
    // 索敵外は攻撃速度より遅い巡回速度で動き、進行方向へ体を向ける。
    const float speed = (std::max)(0.55f, GetParameters()->speed * 0.42f);
    velocity = CalculateWanderVelocity(deltaTime, speed, 0.72f);
    UpdateFacing({velocity.x, 0.0f, velocity.z});
}

void EnemyFireSlime::ApplyGroundMovementAndAnimation(float deltaTime, engine::Vector3& velocity) {
    // 詠唱中の跳ね動作は止め、飛び道具の発射姿勢と移動を競合させない。
    velocity.y = (std::min)(GetVelocity().y, 0.0f);
    // シーケンス制御もブレスも無く、接地などの跳ね条件を満たした場合に、上向きの速度を加えます。
    if (!fireballSequence_.ControlsActor() && breathTimer_ <= 0.0f && slimeBounceAnimator::StepGroundHop(groundHopTimer_, velocity, IsGrounded(), deltaTime,
                                                                   kMoveHopInterval, 0.10f)) {
        velocity.y = (std::max)(velocity.y, kMoveHopPower);
    }

    SetVelocity(velocity);
    ApplySlimeAnimation(deltaTime);
    SyncGroundCollisionRadius();
}

void EnemyFireSlime::BeginThrown(const engine::Vector3& initialVelocity) {
    // 運搬から投擲へ移る前に詠唱・予告・炎を片付け、通常サイズへ戻す。
    fireballSequence_.Cancel();

    // まだ配置倍率を記録していなければ、投擲前に現在の倍率から復元値を控えます。
    if (!hasBaseScale_) {
        baseScale_ = GetScale();
        const float capturedMaxScale =
            (std::max)({std::abs(baseScale_.x), std::abs(baseScale_.y), std::abs(baseScale_.z)});
        // 最大軸の倍率が1.2未満なら、投擲前に小さくなった姿勢を引き継がず既定の大きさにします。
        if (capturedMaxScale < 1.2f) {
            baseScale_ = {2.0f, 2.0f, 2.0f};
        }

        hasBaseScale_ = true;
    }

    breathTimer_ = 0.0f;
    fireballWindupTimer_ = 0.0f;
    breathParticleTimer_ = 0.0f;
    breathEmberTimer_ = 0.0f;
    breathParticleCursor_ = 0;
    attackTimer_ = 0.0f;
    breathDamageDone_ = false;
    breathWarningTriggered_ = false;
    fireballWarningTriggered_ = false;
    fireballAimLocked_ = false;
    HideAttackTelegraph();
    HideBreathFlameVisuals();
    SetColor(GetDefaultColor());
    SetScale(baseScale_);
    SyncThrownCollisionRadius();
    BaseEnemy::BeginThrown(initialVelocity);
}

std::unique_ptr<engine::Object3D> EnemyFireSlime::Clone() const {
    // シーン配置の複製は初期化後に設定を写し、目標と索敵範囲を引き継ぐ。
    auto clone = std::make_unique<EnemyFireSlime>();
    assert(GetObjectCommon() != nullptr);
    clone->Initialize(GetObjectCommon(), GetModelName());
    clone->CopyFrom(this);
    clone->SetTarget(GetTarget());
    clone->SetDetectionRange(GetDetectionRange());
    return clone;
}

// ブレス攻撃、火球、炎演出の補助処理
void EnemyFireSlime::UpdateFacing(const engine::Vector3& direction) {
    // 停止中の微小ベクトルで向きを跳ねさせず、モデルの前方軸だけ補正する。
    const float lengthSq = direction.x * direction.x + direction.z * direction.z;
    // 水平成分の二乗が閾値以下なら、不定な向きで回転を更新しません。
    if (lengthSq <= 0.0001f)
        return;
    const float targetYaw = std::atan2(direction.x, direction.z) + kFireSlimeModelYawOffset;
    SetRotationY(engine::Math::LerpShortAngle(GetRotation().y, targetYaw, 0.16f));
}

void EnemyFireSlime::StartBreath() {
    // ブレス開始時に前の火球選択を消し、判定を一度発生させるための時間とフラグを初期化します。
    selectedFireball_ = {};
    const EnemyAttackDefinition& attack = GetAttackDefinition(kBreathAttackId);
    breathTimer_ = attack.activeDuration;
    breathParticleTimer_ = 0.0f;
    breathEmberTimer_ = 0.0f;
    breathParticleCursor_ = 0;
    breathDamageDone_ = false;
    breathWarningTriggered_ = false;
    attackCooldown_ = attack.cooldown;
    attackTimer_ = attack.activeDuration;
    SetColor({1.0f, 0.82f, 0.62f, 1.0f});
}

void EnemyFireSlime::UpdateBreath(float deltaTime, const engine::Vector3& direction, float distance) {
    // 攻撃判定より先に扇形の予告を見せ、判定発生後は予告を消す。
    const EnemyAttackDefinition& attack = GetAttackDefinition(kBreathAttackId);
    const float activeDuration = (std::max)(0.01f, attack.activeDuration);
    breathTimer_ = (std::max)(0.0f, breathTimer_ - deltaTime);
    breathParticleTimer_ -= deltaTime;
    breathEmberTimer_ -= deltaTime;
    const float progress = attack.GetPhaseProgressFromRemaining(AttackAbilityPhase::kActive, breathTimer_);
    const float damageTriggerTime = activeDuration * 0.62f;
    const float damageTriggerProgress = 1.0f - damageTriggerTime / activeDuration;
    const float timingProgress = std::clamp(progress / (std::max)(damageTriggerProgress, 0.01f), 0.0f, 1.0f);

    // 判定を出す前までは、残り時間に応じた扇形の予告を表示します。
    if (!breathDamageDone_) {
        ShowAttackTelegraphCone(GetTranslate(), direction, attack.maxRange + 0.55f, 0.62f, attack.radius * 2.0f,
                                timingProgress, {1.0f, 0.30f, 0.05f, 0.78f});
    // 判定を処理した後は、炎の描画だけ継続し予告は表示しません。
    } else {
        HideAttackTelegraph();
    }

    // 炎の表示は命中判定の前後を通して更新する。予告の強調は判定時刻より先に一度だけ出す。
    UpdateBreathFlameVisuals(direction, deltaTime);

    // 警告をまだ出しておらず設定された先行時刻に達した場合に、予告を一度強調します。
    if (!breathWarningTriggered_ && breathTimer_ <= damageTriggerTime + attack.warningLeadTime) {
        TriggerAttackTelegraphCue({1.0f, 0.28f, 0.04f, 1.0f});
        breathWarningTriggered_ = true;
    }

    // ブレスの判定は一度だけ発行する。発行後も炎は終了時刻まで表示する。
    if (!breathDamageDone_ && breathTimer_ <= damageTriggerTime) {
        DispatchBreathDamage(direction, distance);
        breathDamageDone_ = true;
        HideAttackTelegraph();
    }

    // 炎の流れと火の粉は、命中判定と独立した発生間隔で追加する。
    if (breathParticleTimer_ <= 0.0f) {
        engine::Vector3 origin = GetTranslate();
        origin.y += 0.72f;
        EmitBreathParticles(origin, direction);
        breathParticleTimer_ += 0.065f;
    }

    // 持続時間が終わったら予告と炎を隠し、次の行動に備えて本体色を戻す。
    if (breathTimer_ <= 0.0f) {
        HideAttackTelegraph();
        SetColor(GetDefaultColor());
        HideBreathFlameVisuals();
    }
}

void EnemyFireSlime::StartFireballWindup(const engine::Vector3& direction, float distance) {
    const EnemyAttackDefinition& attack = selectedFireball_;

    // シーケンスが本体を制御する設定なら共通の時計へ任せ、個別の溜め時間を動かしません。
    if (attack.sequence.enabled && attack.sequence.actorControl.enabled) {
        // 開始に失敗したデータを毎フレーム再試行しません。失敗理由は状態表示に残ります。
        if (!fireballSequence_.Start(attack, MakeSequenceContext(direction, distance)))
            attackCooldown_ = (std::max)(0.25f, attack.cooldown);
        fireballWindupTimer_ = attackTimer_ = 0;
        return;
    }

    fireballWindupTimer_ = (std::max)(0.01f, attack.windupDuration);
    pendingFireballDirection_ = direction;
    pendingFireballDistance_ = distance;
    fireballWarningTriggered_ = false;
    fireballAimLocked_ = false;
    attackTimer_ = fireballWindupTimer_;
    SetColor({1.0f, 0.72f, 0.48f, 1.0f});

    // シーケンスが発射だけを担当する設定なら、共通の再生時間で溜めの追跡を開始します。
    if (attack.sequence.enabled) {
        fireballSequence_.Start(attack, MakeSequenceContext(direction, distance));
        fireballWindupTimer_ = (std::max)(0.0001f, attack.sequence.duration);
    }
}

void EnemyFireSlime::UpdateFireballAttack(float deltaTime, engine::Vector3& velocity, const engine::Vector3& direction,
                                          float distance) {
    // 火球の更新は、シーケンスが本体を制御する場合、発射だけ任せる場合、従来処理の三通り。
    const EnemyAttackDefinition& attack = selectedFireball_;

    // 本体制御付きでは姿勢や動きをシーケンスが進めるため、火球側の照準・予兆経路へは入らない。
    if (attack.sequence.enabled && attack.sequence.actorControl.enabled) {
        fireballSequence_.Update(deltaTime, MakeSequenceContext(direction, distance));
        // 本体制御側に移動停止が設定されている場合だけ、水平速度をゼロにします。
        if (attack.sequence.actorControl.stopMovement)
            velocity.x = velocity.z = 0;
        // 共通シーケンスを終えた場合は、次の攻撃の再使用待ちを始めます。
        if (!fireballSequence_.IsRunning())
            attackCooldown_ = attack.cooldown;
        return;
    }

    velocity.x = 0.0f;
    velocity.z = 0.0f;

    // 発射だけ任せる設定では、照準と予兆をこの関数で管理し、発射時刻は共通時計を使う。
    if (attack.sequence.enabled) {
        const float elapsed = fireballSequence_.GetElapsed();

        // 警告で固定されるまでは、向きと予測距離を対象の現在位置へ合わせる。
        if (!fireballAimLocked_) {
            pendingFireballDirection_ = direction;
            pendingFireballDistance_ = distance;
            UpdateFacing(direction);
        }

        // シーケンスの時刻が溜め時間未満の場合だけ、固定前後の照準に沿って予告を更新します。
        if (elapsed < attack.windupDuration) {
            ShowAttackTelegraphLine(GetWorldPosition(), pendingFireballDirection_,
                std::clamp(pendingFireballDistance_, attack.minRange, attack.maxRange), attack.radius * 2,
                elapsed / (std::max)(0.001f, attack.windupDuration), {1, 0.34f, 0.06f, 0.8f});
            // 次の時計更新で警告時刻を跨ぐフレームに通知し、同時に向きと距離を固定する。
            if (!fireballWarningTriggered_ && elapsed + deltaTime >= attack.windupDuration - attack.warningLeadTime) {
                TriggerAttackTelegraphCue({1, 0.32f, 0.04f, 1});
                fireballWarningTriggered_ = fireballAimLocked_ = true;
            }
        // 溜め時間を過ぎた場合は、火球を放つ位置の予告を消します。
        } else HideAttackTelegraph();
        fireballSequence_.Update(deltaTime, MakeSequenceContext(pendingFireballDirection_, distance));

        // 既存の溜め姿勢も共通時計へ追従させ、残り時間を二重に積算しません。
        fireballWindupTimer_ = (std::max)(0.0f, attack.windupDuration - fireballSequence_.GetElapsed());

        // シーケンスの終了後に硬直・再使用待ちを始め、予兆と溜め色を片付ける。
        if (!fireballSequence_.IsRunning()) {
            fireballWindupTimer_ = 0;
            attackCooldown_ = attack.cooldown;
            attackTimer_ = attack.recoveryDuration;
            HideAttackTelegraph();
            SetColor(GetDefaultColor());
        }

        return;
    }

    // 従来設定でも予告する直前まで対象を追う。発射には確定した値を使う。
    if (!fireballAimLocked_) {
        pendingFireballDirection_ = direction;
        pendingFireballDistance_ = distance;
        UpdateFacing(direction);
    }

    // シーケンスがない場合だけ、火球自身の予兆タイマーを減らす。
    fireballWindupTimer_ = (std::max)(0.0f, fireballWindupTimer_ - deltaTime);
    const float windupDuration = (std::max)(0.01f, attack.windupDuration);
    const float progress = attack.GetPhaseProgressFromRemaining(AttackAbilityPhase::kWindup, fireballWindupTimer_);
    const float predictedDistance =
        std::clamp(pendingFireballDistance_ + attack.radius, attack.minRange, attack.maxRange);
    ShowAttackTelegraphLine(GetWorldPosition(), pendingFireballDirection_, predictedDistance, attack.radius * 2.0f,
                            progress, {1.0f, 0.34f, 0.06f, 0.80f});
    // 旧方式で警告をまだ出しておらず先行時刻に達した場合に、強調と狙いの固定を行います。
    if (!fireballWarningTriggered_ && fireballWindupTimer_ <= attack.warningLeadTime) {
        TriggerAttackTelegraphCue({1.0f, 0.32f, 0.04f, 1.0f});
        fireballWarningTriggered_ = true;
        fireballAimLocked_ = true;
    }

    // 従来方式の溜め時間がまだ残る場合は、発射を次の更新へ持ち越します。
    if (fireballWindupTimer_ > 0.0f) {
        return;
    }

    // 溜めが終わったら確定済みの向きと距離で発射し、通常の色と次の攻撃待ちへ戻す。
    FireFireball(pendingFireballDirection_, pendingFireballDistance_);
    HideAttackTelegraph();
    attackCooldown_ = attack.cooldown;
    attackTimer_ = attack.recoveryDuration;
    fireballAimLocked_ = false;
    SetColor(GetDefaultColor());
}

void EnemyFireSlime::DispatchBreathDamage(const engine::Vector3& direction, float distance) {
    // 扇形に加え、炎メッシュの高さと遮蔽物を確認してから命中を通知する。
    const EnemyAttackDefinition& attack = GetAttackDefinition(kBreathAttackId);

    // 対象を失った場合と、余裕を含む最大射程を超えた場合は判定を出しません。
    if (!GetTarget() || distance > attack.maxRange + 0.55f) {
        return;
    }

    engine::Vector3 toTarget = NormalizePlanar(GetTarget()->GetTranslate() - GetTranslate());
    const float facingDot = direction.x * toTarget.x + direction.z * toTarget.z;

    // 水平面で正面との内積が0.34未満なら、ブレスの広がりの外です。
    if (facingDot < 0.34f) {
        return;
    }

    // 平面上の扇に入っていても、炎の高さと相手の当たり箱が重ならなければ除外する。
    const float bodyHeight = (std::max)(0.7f, std::abs(GetScale().y));
    const float visualCenterY = GetTranslate().y + 0.68f * bodyHeight;
    const float visualHalfHeight = 0.75f * (std::max)(0.7f, std::abs(GetScale().x));
    const engine::Aabb targetBounds = GetTarget()->GetAabb();
    // 対象の上下端が炎の高さと交差していなければ、炎は届いていません。
    if (targetBounds.max.y < visualCenterY - visualHalfHeight ||
        targetBounds.min.y > visualCenterY + visualHalfHeight) {
        return;
    }

    // 炎の放出口から相手の中心まで遮蔽物を調べ、壁越しの命中を防ぐ。
    const engine::Vector3 rayStart = GetWorldPosition() + engine::Vector3{0.0f, 0.72f * bodyHeight, 0.0f};
    const engine::Vector3 targetCenter = (targetBounds.min + targetBounds.max) * 0.5f;
    const engine::Vector3 rayDelta = targetCenter - rayStart;
    const float rayDistance = engine::Math::Length(rayDelta);

    // 炎の出口と対象中心が十分離れている場合にだけ、途中の障害物を検索します。
    if (rayDistance > 0.01f) {
        engine::PhysicsQueryFilter filter;
        filter.mask = engine::collisionCategories::Get("ground") | engine::collisionCategories::Get("map_block");
        filter.ignoredObject = this;
        const engine::RaycastHit wall = engine::CollisionManager::GetInstance()->Raycast(
            rayStart, rayDelta / rayDistance, rayDistance, filter);
        // 対象まで届く前の位置で地形かマップ部品に当たったら、壁で炎が遮られた扱いにします。
        if (wall.isHit && wall.distance < rayDistance - 0.1f) {
            return;
        }
    }

    // 全条件を通った相手に、炎ダメージと攻撃定義の燃焼・無敵時間を通知する。
    DamageEvent damageEvent;
    damageEvent.target = GetTarget();
    damageEvent.attacker = this;
    damageEvent.damageAmount = attack.damage;
    damageEvent.knockbackVelocity = attack.ResolveKnockback(direction, {direction.x * 9.5f, 4.4f, direction.z * 9.5f});
    damageEvent.damageType = DamageType::kFire;
    damageEvent.statusEffect = MakeStatusEffect(attack);
    damageEvent.invincibilityDuration = attack.ResolveInvincibilityDuration();
    EventManager::GetInstance()->Dispatch(damageEvent);

    // 技定義に被弾の効果が登録されている場合に、その場で一度再生します。
    if (!attack.feedbackCue.empty()) {
        VfxSequencer::PlayOneShot(attack.feedbackCue, GetTarget()->GetWorldPosition());
    }
}

void EnemyFireSlime::FireFireball(const engine::Vector3& direction, float distance) {
    // 弾の衝突属性・威力・燃焼設定を JSON の攻撃定義から同じ発射要求にまとめる。
    // 発射時点で対象が無い場合は弾道を決められないため、旧方式の火球を生成しません。
    if (!GetTarget())
        return;

    // 確定した方向へ発射元を出し、そこから対象の現在座標を狙う。
    engine::Vector3 spawnPos = GetTranslate() + direction * 0.65f;
    spawnPos.y += 1.05f;

    engine::Vector3 aim = GetTarget()->GetTranslate() - spawnPos;
    aim.y += 0.35f;

    // 放出位置と照準先がほぼ同じなら、確定した水平向きと少し上向きの成分を使います。
    if (engine::Math::Length(aim) <= 0.001f) {
        aim = {direction.x, 0.12f, direction.z};
    }

    aim = engine::Math::Normalize(aim);

    // 予測距離で弾速を変え、攻撃定義の最小・最大速度に収める。
    const EnemyAttackDefinition& attack = GetAttackDefinition(kFireballAttackId);
    const float speed = std::clamp(distance * 1.65f, attack.minSpeed, attack.maxSpeed);
    BulletManager::GetInstance()->Fire(spawnPos, aim * speed, engine::collisionCategories::Get("enemy_attack"), engine::collisionCategories::Get("player") | engine::collisionCategories::Get("ground"), "Primitives/sphere",
                                       0.5f, attack.lifetime, MakeFireVisual(1.2f), attack.damage,
                                       MakeStatusEffect(attack), DamageType::kFire);

    // 弾の軌跡や着弾とは別に、発射元へ攻撃開始の粒子を出す。
    EmitFirePreset(attack.activeVfx.empty() ? kCastPreset : attack.activeVfx.c_str(), spawnPos);
}

AttackSequenceContext EnemyFireSlime::MakeSequenceContext(const engine::Vector3& direction, float distance) {
    // シーケンスの条件評価へ、現在の向き・接地・目標位置を一度に渡す。
    AttackSequenceContext context;
    context.owner = this;
    context.position = GetWorldPosition();
    context.forward = direction;
    context.hasTarget = GetTarget() != nullptr;
    // 対象が実在するときにだけ現在のワールド位置を条件評価へ渡します。
    if (GetTarget())
        context.targetPosition = GetTarget()->GetWorldPosition();
    context.targetDistance = distance;
    context.grounded = IsGrounded();
    return context;
}

const EnemyAttackDefinition* EnemyFireSlime::SelectSequenceAttack(float distance) const {
    // プレビューでは指定攻撃、実戦では距離と索敵範囲に合う攻撃を選ぶ。
    return GetAttackProfile().SelectSequence(distance, GetDetectionRange(), previewAttackId_);
}

void EnemyFireSlime::UpdateHeadFlame(float deltaTime) {
    // 頭上の炎は AI と別の更新経路で維持し、待機中も動かす。
    UpdateHeadFlameVisual(deltaTime);
}

void EnemyFireSlime::EnsureHeadFlameVisual() {
    // 頭上の炎が作成済みか削除を予約済みなら、新しい実体を作りません。
    if (headFlameVisual_ || headFlameRemoveRequested_) {
        return;
    }

    SceneManager* sceneManager = SceneManager::GetInstance();
    BaseScene* scene = sceneManager ? sceneManager->GetCurrentScene() : nullptr;

    // 現在のシーンと描画の共通設定を取得できる場合にだけ、シーン所有の炎を作ります。
    if (!scene || !scene->GetObject3DCommon()) {
        return;
    }

    auto flame = std::make_unique<engine::Object3D>();
    flame->Initialize(scene->GetObject3DCommon());
    flame->SetName(GetName() + "_HeadFlameShader");
    flame->SetClassName("Effect");

    // 敵に追従する一時演出はシーン保存の対象にしない。
    flame->SetEditorInternal(true);
    flame->SetModel("Primitives/sphere");
    flame->SetColliderType(engine::ColliderType::kNone);
    flame->SetCollisionAttribute(0);
    flame->SetCollisionMask(0);
    flame->SetBlendMode(BlendMode::kNormal);
    flame->SetMaterialType(kHeadFlameMaterialType);
    flame->SetSelectedLighting(0);
    flame->SetEnableLighting(false);
    flame->SetColor({1.0f, 0.24f, 0.04f, 0.82f});
    flame->SetEmissive(1.7f);

    // 炎のMeshRendererを取得できたときに、専用材質の領域を確認します。
    if (auto* renderer = flame->GetMeshRenderer()) {
        // 材質用の領域がある場合に、波とゆらぎの初期値を設定します。
        if (auto* water = renderer->GetWaterParamData()) {
            water->effectType = kHeadFlameEffectType;
            water->waveSpeed = 1.75f;
            water->effectScale = 0.95f;
            water->waveFrequency = 2.2f;
            water->waveHeight = 0.55f;
            water->effectSoftness = 0.48f;
            water->effectIntensity = 0.84f;
            water->billboardScale = 0.80f;
            water->effectScaleX = 0.92f;
            water->effectScaleY = 1.0f;
            water->effectScaleZ = 0.92f;
            water->uvOffsetX = 2.73f;
            water->uvOffsetY = 5.17f;
        }
    }

    // シーンへ所有権を渡し、敵側は追従更新のための参照だけを保持します。
    headFlameVisual_ = flame.get();
    scene->AddObject(std::move(flame));
}

void EnemyFireSlime::UpdateHeadFlameVisual(float deltaTime) {
    EnsureHeadFlameVisual();

    // 炎を用意できない場合、姿勢や粒子を更新しません。
    if (!headFlameVisual_) {
        return;
    }

    // 水平速度を平滑化して、炎の根元を進行方向の反対へ少し引く。
    const engine::Vector3 scale = GetScale();
    const float bodyScale = (std::max)({0.7f, std::abs(scale.x), std::abs(scale.y), std::abs(scale.z)});
    engine::Vector3 velocity = GetVelocity();
    velocity.y = 0.0f;
    const float velocityResponse = 1.0f - std::exp(-(std::max)(0.0f, deltaTime) * 9.0f);
    smoothedFlameVelocity_ = engine::Math::Lerp(smoothedFlameVelocity_, velocity, velocityResponse);
    const float moveSpeed = engine::Math::Length(smoothedFlameVelocity_);
    engine::Vector3 pos = GetTranslate();
    pos.y += kHeadFlameHeight * (std::max)(0.65f, std::abs(scale.y));

    // 平滑化後の水平速度が十分大きい場合にだけ、移動方向と逆へ炎を引きます。
    if (moveSpeed > 0.001f) {
        const engine::Vector3 trailDirection = engine::Math::Normalize(smoothedFlameVelocity_) * -1.0f;
        pos = pos + trailDirection * (std::min)(0.32f, moveSpeed * 0.022f);
    }

    // 炎の向きを本体へ固定し、カメラ移動時にも内部の奥行きが保たれるようにします。
    const float yaw = GetRotation().y;
    const engine::Vector3 localRight = {std::cos(yaw), 0.0f, -std::sin(yaw)};
    const float flowX = (std::clamp)(-engine::Math::Dot(smoothedFlameVelocity_, localRight) * 0.12f, -1.0f, 1.0f);

    // 待機中は周期的に揺らし、移動時は速度に応じて炎を広げる。
    const float pulse = std::sin(idleTimer_ * 8.0f) * 0.06f;
    const float speedRate = (std::clamp)(moveSpeed * 0.055f, 0.0f, 1.0f);
    const float width = kHeadFlameBaseScale * (0.94f + pulse + speedRate * 0.06f) * bodyScale;
    const float height = kHeadFlameBaseScale * (0.82f - pulse * 0.16f + speedRate * 0.08f) * bodyScale;

    headFlameVisual_->SetIsVisible(GetIsVisible() && !IsDead());
    headFlameVisual_->SetTranslate(pos);
    headFlameVisual_->SetRotation({0.0f, yaw, 0.0f});
    headFlameVisual_->SetScale({width, height, width});
    headFlameVisual_->SetColor({1.0f, 0.24f + std::sin(idleTimer_ * 5.3f) * 0.035f, 0.04f, 0.80f});

    // 本体の右方向に対する移動量を炎の流れへ渡し、移動時の揺れも強める。
    // 炎の描画実体を取得できた場合に、材質の動きを更新できるか調べます。
    if (auto* renderer = headFlameVisual_->GetMeshRenderer()) {
        // 材質領域がある場合だけ、水平の流れと上への揺れを設定します。
        if (auto* water = renderer->GetWaterParamData()) {
            water->flowSpeedX = flowX;
            water->flowSpeedY = speedRate;
            water->waveSpeed = 1.75f + std::sin(idleTimer_ * 1.7f) * 0.16f + speedRate * 0.42f;
            water->effectIntensity = 0.82f + std::sin(idleTimer_ * 4.1f) * 0.045f + speedRate * 0.07f;
            water->effectScaleX = 0.92f + speedRate * 0.10f;
            water->effectScaleY = 1.0f + speedRate * 0.08f;
        }
    }

    headFlameVisual_->UpdateLocalMatrix();
    headFlameVisual_->UpdateWorldMatrix();

    // メッシュとは別の周期で炎と火の粉を発生させる。先に表示状態を適用してから出す。
    headFlameParticleTimer_ -= deltaTime;
    headEmberParticleTimer_ -= deltaTime;

    // 頭上の炎を描画している間にだけ、付随する粒子を出します。
    if (headFlameVisual_->GetIsVisible()) {
        engine::Vector3 particleDirection = {-smoothedFlameVelocity_.x * 0.075f, 0.78f, -smoothedFlameVelocity_.z * 0.075f};

        // 炎粒子の周期に達したら、発生位置を円周方向へずらして一回発生させます。
        if (headFlameParticleTimer_ <= 0.0f) {
            constexpr float kGoldenAngle = 2.39996323f;
            const float phase = idleTimer_ * 2.8f + static_cast<float>(headFlameParticleCursor_) * kGoldenAngle;
            const float ringRadius = bodyScale * (0.10f + 0.025f * std::sin(phase * 1.7f));
            engine::Vector3 emitPosition = pos;
            emitPosition.x += std::cos(phase) * ringRadius;
            emitPosition.z += std::sin(phase) * ringRadius;
            emitPosition.y += bodyScale * (0.18f + 0.06f * std::sin(phase * 1.31f));
            EmitDirectedFirePreset(kHeadFlamePreset, emitPosition, particleDirection, 0.90f + speedRate * 0.34f);
            ++headFlameParticleCursor_;
            headFlameParticleTimer_ = 0.12f;
        }

        // 火の粉用の周期を迎えたときは、炎本体から外側に散る粒子も発生させます。
        if (headEmberParticleTimer_ <= 0.0f) {
            const float phase = idleTimer_ * 4.2f + static_cast<float>(headFlameParticleCursor_) * 1.37f;
            engine::Vector3 emitPosition = pos;
            emitPosition.x += std::cos(phase) * bodyScale * 0.26f;
            emitPosition.z += std::sin(phase) * bodyScale * 0.26f;
            emitPosition.y += 0.12f;
            EmitDirectedFirePreset(kHeadEmberPreset, emitPosition, particleDirection, 1.02f + speedRate * 0.44f);
            headEmberParticleTimer_ = 0.24f;
        }
    }
}

void EnemyFireSlime::RequestRemoveHeadFlameVisual() {
    // シーン所有の表示物は削除要求を一度だけ送り、手元の参照を直ちに外す。
    if (headFlameRemoveRequested_) {
        return;
    }

    headFlameRemoveRequested_ = true;

    // まだ実体が無い場合は、再生成を止めるフラグだけを保持します。
    if (!headFlameVisual_) {
        return;
    }

    headFlameVisual_->SetIsVisible(false);
    SceneManager* sceneManager = SceneManager::GetInstance();
    BaseScene* scene = sceneManager ? sceneManager->GetCurrentScene() : nullptr;

    // シーンが取得できる場合は、非表示にした炎の削除も予約します。
    if (scene) {
        scene->RequestRemoveObject(headFlameVisual_);
    }

    headFlameVisual_ = nullptr;
}

void EnemyFireSlime::EnsureBreathFlameVisuals() {
    // 削除予約を出した後は、炎の枠を作り直しません。
    if (breathFlameRemoveRequested_) {
        return;
    }

    // 既存の炎は再利用し、欠けている枠があるときだけシーンから描画基盤を取得する。
    bool needsCreate = false;

    for (std::int32_t i = 0; i < kBreathFlameVisualCount; ++i) {
        // 使用する枠に一つでも欠けがあれば、描画設定を取得する準備へ進みます。
        if (!breathFlameVisuals_[i]) {
            needsCreate = true;
            break;
        }
    }

    // 全ての枠に炎があれば、そのまま使い回します。
    if (!needsCreate) {
        return;
    }

    SceneManager* sceneManager = SceneManager::GetInstance();
    BaseScene* scene = sceneManager ? sceneManager->GetCurrentScene() : nullptr;

    // 現在シーンか3Dの描画基盤が無いときは、欠けた炎を生成できません。
    if (!scene || !scene->GetObject3DCommon()) {
        return;
    }

    for (std::int32_t i = 0; i < kBreathFlameVisualCount; ++i) {
        // 個々の枠がすでに生成済みなら飛ばし、空き枠だけに新しい実体を割り当てます。
        if (breathFlameVisuals_[i]) {
            continue;
        }

        auto flame = std::make_unique<engine::Object3D>();
        flame->Initialize(scene->GetObject3DCommon());
        flame->SetName(GetName() + "_BreathFlameShader_" + std::to_string(i));
        flame->SetClassName("Effect");

        // ブレスの一時演出も配置オブジェクトとして保存しない。
        flame->SetEditorInternal(true);
        flame->SetModel("Primitives/sphere");
        flame->SetColliderType(engine::ColliderType::kNone);
        flame->SetCollisionAttribute(0);
        flame->SetCollisionMask(0);
        flame->SetBlendMode(BlendMode::kNormal);
        flame->SetMaterialType(kHeadFlameMaterialType);
        flame->SetSelectedLighting(0);
        flame->SetEnableLighting(false);
        flame->SetIsVisible(false);
        flame->SetColor({1.0f, 0.31f, 0.05f, 0.0f});
        flame->SetEmissive(1.8f);

        // 炎ごとに異なるUVオフセットを使い、隣の炎と揺れが揃わないようにする。
        // 炎の描画実体が利用できる場合に、材質の調整領域を調べます。
        if (auto* renderer = flame->GetMeshRenderer()) {
            // 材質領域があれば、枠ごとに別のUV開始位置とブレス用の設定を入れます。
            if (auto* water = renderer->GetWaterParamData()) {
                water->effectType = kBreathFlameEffectType;
                water->waveSpeed = 2.45f;
                water->effectScale = 0.92f;
                water->effectSoftness = 0.58f;
                water->effectIntensity = 0.92f;
                water->billboardScale = 0.72f;
                water->effectScaleX = 1.0f;
                water->effectScaleY = 0.90f;
                water->effectScaleZ = 1.15f;
                water->uvOffsetX = 3.71f * static_cast<float>(i + 1);
                water->uvOffsetY = 6.13f * static_cast<float>(i + 1);
            }
        }

        // 炎はシーンに追加し、敵は位置と表示を更新する参照を控えます。
        breathFlameVisuals_[i] = flame.get();
        scene->AddObject(std::move(flame));
    }
}

void EnemyFireSlime::UpdateBreathFlameVisuals(const engine::Vector3& direction, float deltaTime) {
    // 複数の炎メッシュを射線上に配し、発生・消滅のアルファを一つの進行率で揃える。
    (void)deltaTime;
    EnsureBreathFlameVisuals();

    const EnemyAttackDefinition& attack = GetAttackDefinition(kBreathAttackId);
    const float activeDuration = (std::max)(0.01f, attack.activeDuration);
    const engine::Vector3 scale = GetScale();
    const engine::Vector3 origin = GetTranslate();
    const float bodyScale = (std::max)({0.7f, std::abs(scale.x), std::abs(scale.y), std::abs(scale.z)});
    const float progress = 1.0f - (std::clamp)(breathTimer_ / activeDuration, 0.0f, 1.0f);
    const float fadeIn = (std::clamp)(progress / 0.16f, 0.0f, 1.0f);
    const float fadeOut = (std::clamp)(breathTimer_ / (activeDuration * 0.18f), 0.0f, 1.0f);
    const float breathAlpha = fadeIn * fadeOut;
    const engine::Vector3 side = {-direction.z, 0.0f, direction.x};

    for (std::int32_t i = 0; i < kBreathFlameVisualCount; ++i) {
        engine::Object3D* flame = breathFlameVisuals_[i];

        // 描画用の炎が存在しない枠では、姿勢と材質の更新を飛ばします。
        if (!flame) {
            continue;
        }

        // 射線上での順番に応じて前後位置を決め、左右へ異なる位相で揺らす。
        const float step = (static_cast<float>(i) + 0.5f) / static_cast<float>(kBreathFlameVisualCount);
        const float distance = 0.50f + step * (attack.maxRange * 0.84f);
        const float wobble = std::sin(idleTimer_ * (6.2f + step * 1.4f) + step * 8.3f) * (0.025f + step * 0.07f);
        engine::Vector3 pos = origin + direction * distance + side * wobble;
        pos.y += (0.62f + step * 0.12f) * (std::max)(0.7f, std::abs(scale.y));

        const float yaw = std::atan2(direction.x, direction.z);

        // 遠い炎ほど横幅を広げ、共通のフェード値から各炎の透明度を計算する。
        const float pulse = std::sin(idleTimer_ * (8.2f + step * 2.3f) + step * 5.7f) * 0.035f;
        const float width = (0.64f + step * 0.78f + pulse) * bodyScale;
        const float height = (0.34f + step * 0.23f - pulse * 0.18f) * bodyScale;
        const float alpha = breathAlpha * (0.78f - step * 0.12f);

        flame->SetIsVisible(alpha > 0.02f);
        flame->SetTranslate(pos);
        flame->SetRotation({0.0f, yaw, 0.0f});
        flame->SetScale({width, (std::max)(height, width * 0.68f), width * 1.12f});
        flame->SetColor({1.0f, 0.30f + step * 0.06f, 0.045f, alpha});

        // 描画実体を取得できた炎について、材質の時間変化を確認します。
        if (auto* renderer = flame->GetMeshRenderer()) {
            // 材質領域を取得できる場合は、射線上の位置と横揺れに合わせて流れを更新します。
            if (auto* water = renderer->GetWaterParamData()) {
                water->flowSpeedX = wobble;
                water->flowSpeedY = 0.82f + step * 0.42f;
                water->waveSpeed = 2.35f + step * 0.58f;
                water->effectIntensity = 0.88f + step * 0.10f;
                water->effectScaleX = 1.08f + step * 0.56f;
                water->effectScaleY = 0.90f;
                water->effectScaleZ = 1.15f;
            }
        }

        flame->UpdateLocalMatrix();
        flame->UpdateWorldMatrix();
    }
}

void EnemyFireSlime::HideBreathFlameVisuals() {
    // 運搬や攻撃終了時はシーンから消す前にまず描画を止める。
    for (engine::Object3D* flame : breathFlameVisuals_) {
        // 生成済みの炎だけを非表示にし、後続のブレスで再利用できるよう保持します。
        if (flame) {
            flame->SetIsVisible(false);
        }
    }
}

void EnemyFireSlime::RequestRemoveBreathFlameVisuals() {
    // シーンが実体を保持するため、各メッシュへ削除要求を送り参照だけ破棄する。
    if (breathFlameRemoveRequested_) {
        return;
    }

    breathFlameRemoveRequested_ = true;
    SceneManager* sceneManager = SceneManager::GetInstance();
    BaseScene* scene = sceneManager ? sceneManager->GetCurrentScene() : nullptr;

    for (engine::Object3D*& flame : breathFlameVisuals_) {
        // 作っていない円の枠は、可視状態や削除要求を変更しません。
        if (!flame) {
            continue;
        }

        flame->SetIsVisible(false);

        // シーンがまだ有効な場合だけ削除を依頼し、借用している炎の参照は直後に外します。
        if (scene) {
            scene->RequestRemoveObject(flame);
        }

        flame = nullptr;
    }
}

void EnemyFireSlime::EmitBreathParticles(const engine::Vector3& origin, const engine::Vector3& direction) {
    // 放出口を順に前へ進め、メッシュの炎と同じ距離帯に火の粉を散らす。
    const EnemyAttackDefinition& attack = GetAttackDefinition(kBreathAttackId);
    const float step = (static_cast<float>(breathParticleCursor_ % kBreathFlameVisualCount) + 0.5f) /
                       static_cast<float>(kBreathFlameVisualCount);
    engine::Vector3 emitPosition = origin + direction * (0.48f + step * attack.maxRange * 0.82f);
    emitPosition.y += std::sin(idleTimer_ * 8.0f + step * 9.1f) * 0.08f;

    engine::Vector3 streamDirection = direction;
    streamDirection.y = 0.10f + step * 0.10f;
    streamDirection = engine::Math::Normalize(streamDirection);
    const char* bodyPreset = attack.activeVfx.empty() ? kBreathPreset : attack.activeVfx.c_str();
    EmitDirectedFirePreset(bodyPreset, emitPosition, streamDirection, 0.82f + step * 0.34f);

    // 火の粉の間隔に達すると、通常の炎と同じ位置・向きに補助粒子を追加します。
    if (breathEmberTimer_ <= 0.0f) {
        EmitDirectedFirePreset(kBreathEmberPreset, emitPosition, streamDirection, 0.95f + step * 0.42f);
        breathEmberTimer_ += 0.14f;
    }

    ++breathParticleCursor_;
}

void EnemyFireSlime::EmitFirePreset(const char* presetName, const engine::Vector3& position) {
    // 未初期化の GPU パーティクル管理には要求を送らない。
    GpuParticleManager* particles = GpuParticleManager::GetInstance();

    // 粒子用の管理器が無いか初期化前なら、通常の発生要求も送りません。
    if (!particles || !particles->IsInitialized()) {
        return;
    }

    particles->Emit(presetName, position);
}

void EnemyFireSlime::EmitDirectedFirePreset(const char* presetName, const engine::Vector3& position, const engine::Vector3& direction,
                                            float speedScale) {
    // 火の粉の流れる向きを指定できる場合だけ方向付きプリセットを使う。
    GpuParticleManager* particles = GpuParticleManager::GetInstance();

    // GPU粒子が初期化されていない場合は、方向付きの発生を依頼しません。
    if (!particles || !particles->IsInitialized()) {
        return;
    }

    particles->EmitDirected(presetName, position, direction, speedScale);
}

void EnemyFireSlime::ApplySlimeAnimation(float deltaTime) {
    // Animator Controllerを読み込めた場合は、描画姿勢の状態選択をControllerへ渡します。
    if (auto* controller = GetAnimatorControllerAsset()) {
        // 攻撃中に使うクリップを優先し、通常時は水平速度で待機と移動を選ぶ。
        std::string stateName = "Idle";

        // 火球の再生か溜めが有効な間は、その技のアニメーション名を優先します。
        if (fireballSequence_.IsRunning() || fireballWindupTimer_ > 0.0f) {
            stateName = selectedFireball_.animation;
        // 火球が進行中でなくブレスが続いていれば、ブレスのアニメーションを選びます。
        } else if (breathTimer_ > 0.0f) {
            stateName = GetAttackDefinition(kBreathAttackId).animation;
        // 攻撃中でない場合は、水平速度から待機または移動の状態を選びます。
        } else {
            const engine::Vector3 velocity = GetVelocity();
            // 水平速度の二乗が0.04を超える場合は移動状態として扱います。
            if (velocity.x * velocity.x + velocity.z * velocity.z > 0.04f)
                stateName = "Move";
        }

        // 状態配列の並び替えでは値が変わらないよう、Controllerの遷移条件から入力値を読む。
        std::int32_t motionMode = 0;

        for (const auto& transition : controller->transitions) {
            // 今回適用する状態へつながらない遷移からは、入力値を読みません。
            if (transition.toState != stateName)
                continue;
            for (const auto& condition : transition.conditions) {
                // MotionModeを等値比較している条件に遭遇したら、そのしきい値を入力として使います。
                if (condition.parameter == "MotionMode" && condition.mode == AnimatorConditionMode::kEquals) {
                    motionMode = static_cast<std::int32_t>(condition.threshold);
                    break;
                }
            }
        }

        // Controllerの実行器が利用できるときに、求めた状態番号を渡します。
        if (auto* runtime = GetAnimatorControllerRuntime())
            runtime->SetInt("MotionMode", motionMode);

        // 当たり判定の物理Transformは基準値に保ち、Controllerの姿勢は描画だけに適用する。
        engine::Vector3 scale = baseScale_;
        engine::Vector3 rotation = {0.0f, GetRotation().y, 0.0f};
        ApplyDamageReactionPose(scale, rotation);
        const float poseRate = (std::min)(1.0f, deltaTime * 13.0f);
        SetScale(engine::Math::Lerp(GetScale(), scale, poseRate));
        const engine::Vector3 current = GetRotation();
        SetRotation({current.x + (rotation.x - current.x) * poseRate, current.y,
                     current.z + (rotation.z - current.z) * poseRate});
        return;
    }

    engine::Vector3 targetScale = baseScale_;
    engine::Vector3 targetRotation = {0.0f, GetRotation().y, 0.0f};

    // Controllerを取得できずシーケンスが本体を制御中の場合は、シーケンスの時刻を姿勢の基準にします。
    if (fireballSequence_.ControlsActor()) {
        // Controllerを読めない環境でも、共通シーケンスの時計に合わせて溜めと反動を見せる。
        const float elapsed = fireballSequence_.GetElapsed();
        const float windup = (std::max)(0.01f, selectedFireball_.windupDuration);

        // 火球を放つまでの溜め区間では、時間に応じて押し縮んだ姿勢を作ります。
        if (elapsed < windup) {
            const float charge = std::clamp(elapsed / windup, 0.0f, 1.0f);
            targetScale.x = baseScale_.x * (1.0f + charge * 0.18f);
            targetScale.y = baseScale_.y * (1.0f - charge * 0.24f);
            targetScale.z = baseScale_.z * (1.0f - charge * 0.08f);
            targetRotation.x = charge * 0.12f;
        // 溜め区間を過ぎた場合は、硬直時間に合わせて火球を放った反動を見せます。
        } else {
            const float recovery = (std::max)(0.01f, selectedFireball_.recoveryDuration);
            const float recoil = std::sin(std::clamp((elapsed - windup) / recovery, 0.0f, 1.0f) * std::numbers::pi_v<float>);
            targetScale.x = baseScale_.x * (1.0f - recoil * 0.08f);
            targetScale.y = baseScale_.y * (1.0f + recoil * 0.12f);
            targetScale.z = baseScale_.z * (1.0f + recoil * 0.20f);
            targetRotation.x = -recoil * 0.16f;
        }
    } else if (breathTimer_ > 0.0f) {
        // ブレスは吸い込みから吐き出しへ進み、本体の奥行きと前傾を変える。
        const float activeDuration = (std::max)(0.01f, GetAttackDefinition(kBreathAttackId).activeDuration);
        const float t = std::clamp(1.0f - (breathTimer_ / activeDuration), 0.0f, 1.0f);
        const float inhale = std::sin(std::clamp(t / 0.24f, 0.0f, 1.0f) * std::numbers::pi_v<float>);
        const float exhale = std::clamp((t - 0.16f) / 0.24f, 0.0f, 1.0f);
        const float pulse = std::sin(t * 22.0f) * 0.035f * exhale;
        targetScale.x = baseScale_.x * (1.0f + inhale * 0.12f + exhale * 0.08f + pulse);
        targetScale.y = baseScale_.y * (1.0f + inhale * 0.10f - exhale * 0.20f - pulse * 0.30f);
        targetScale.z = baseScale_.z * (1.0f - inhale * 0.12f + exhale * 0.30f - pulse * 0.55f);
        targetRotation.x = inhale * 0.09f - exhale * 0.07f;
    } else if (fireballWindupTimer_ > 0.0f) {
        // 従来の火球では残りの予備時間から縮みと震えを計算する。
        const float windupDuration = (std::max)(0.01f, selectedFireball_.windupDuration);
        const float charge = std::clamp(1.0f - fireballWindupTimer_ / windupDuration, 0.0f, 1.0f);
        const float tremble = std::sin(idleTimer_ * 31.0f) * 0.025f * charge;
        targetScale.x = baseScale_.x * (1.0f + charge * 0.18f + tremble);
        targetScale.y = baseScale_.y * (1.0f - charge * 0.24f);
        targetScale.z = baseScale_.z * (1.0f - charge * 0.08f - tremble * 0.6f);
        targetRotation.x = charge * 0.12f;
        targetRotation.z = tremble * 1.6f;
    } else if (attackTimer_ > 0.0f) {
        // 火球の発射後は硬直時間に合わせ、後ろへ反る反動を減衰させる。
        const float recoveryDuration = (std::max)(0.01f, selectedFireball_.id.empty()
            ? GetAttackDefinition(kFireballAttackId).recoveryDuration : selectedFireball_.recoveryDuration);
        const float remaining = std::clamp(attackTimer_ / recoveryDuration, 0.0f, 1.0f);
        const float recoil = std::sin((1.0f - remaining) * std::numbers::pi_v<float>);
        targetScale.x = baseScale_.x * (1.0f - recoil * 0.08f);
        targetScale.y = baseScale_.y * (1.0f + recoil * 0.12f);
        targetScale.z = baseScale_.z * (1.0f + recoil * 0.20f);
        targetRotation.x = -recoil * 0.16f;
    } else {
        // 攻撃中以外は接地と移動速度を使って待機・移動の弾みを計算する。
        slimeBounceAnimator::Params params;
        params.speedForFullBounce = 1.85f;
        params.idleAmplitude = 0.075f;
        params.moveAmplitude = 0.27f;
        params.hopFrequency = 10.4f;
        params.horizontalSquash = 0.28f;
        params.verticalStretch = 0.35f;
        params.airborneStretch = 0.34f;
        targetScale = slimeBounceAnimator::MakeScale(baseScale_, GetVelocity(), idleTimer_, IsGrounded(), params);
    }

    // 選んだ姿勢へ被弾の反応を重ね、急に姿勢が変わらないよう補間する。
    ApplyDamageReactionPose(targetScale, targetRotation);
    const float poseRate = (std::min)(1.0f, deltaTime * 13.0f);
    engine::Vector3 scale = engine::Math::Lerp(GetScale(), targetScale, poseRate);
    SetScale(scale);
    const engine::Vector3 currentRotation = GetRotation();
    SetRotation({currentRotation.x + (targetRotation.x - currentRotation.x) * poseRate, currentRotation.y,
                 currentRotation.z + (targetRotation.z - currentRotation.z) * poseRate});
}

void EnemyFireSlime::SyncWorldCollisionRadius(float worldRadius) {
    // 見た目の伸縮でワールド空間の当たり半径が変わらないよう逆スケールする。
    const engine::Vector3 scale = GetScale();
    const float maxScale = (std::max)({0.001f, std::abs(scale.x), std::abs(scale.y), std::abs(scale.z)});
    SetCollisionRadius(worldRadius / maxScale);
}

void EnemyFireSlime::SyncGroundCollisionRadius() {
    // 地上では小さめの本体判定へ戻す。
    SyncWorldCollisionRadius(kGroundCollisionWorldRadius);
}

void EnemyFireSlime::SyncThrownCollisionRadius() {
    // 投擲時は接触を拾いやすい広い判定を使う。
    SyncWorldCollisionRadius(kThrownCollisionWorldRadius);
}
