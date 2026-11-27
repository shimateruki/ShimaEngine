#include <cstdint>
#include "EnemyGiantSlime.h"
#include "Easing.h"
#include <numbers>
#include "game/actor/enemy/animation/SlimeBounceAnimator.h"
#include "BaseScene.h"
#include "CollisionConfig.h"
#include "DebugConsole.h"
#include "game/actor/enemy/spawn/EnemyFactory.h"
#include "game/system/rules/EventManager.h"
#include "game/system/combat/HitEffectDirector.h"
#include "MeshRenderer.h"
#include "MeshEffectManager.h"
#include "ParticleSystem.h"
#include "SceneManager.h"
#include <algorithm>
#include <cmath>

namespace {
// 大型スライムの着地衝撃波とフック分裂の調整値
constexpr const char* kJumpPressAttackId = "jump_press";
constexpr float kHookSplitDuration = 1.85f;
constexpr float kHookSplitWeakDuration = 1.05f;
constexpr float kHookLandingSquashDuration = 0.24f;
constexpr float kTelegraphInterval = 0.18f;
constexpr float kHeavyHopDefaultJumpPower = 18.5f;
constexpr float kHeavyHopMinJumpPower = 14.0f;
constexpr float kHeavyHopMaxJumpPower = 21.0f;
constexpr float kHeavyHopMinSpeed = 10.0f;
constexpr float kHeavyHopMaxSpeed = 34.0f;
constexpr float kGroundCollisionWorldRadius = 1.15f;
constexpr float kThrownCollisionWorldRadius = 1.45f;
constexpr float kGroundCollisionWorldCenterY = 0.58f;
constexpr float kThrownCollisionWorldCenterY = 0.35f;
constexpr std::int32_t kSplitSlimeCount = 4;
constexpr float kSplitSlimeRadius = 3.1f;
constexpr float kGiantSlimeModelYawOffset = std::numbers::pi_v<float>;

engine::Vector3 NormalizePlanar(engine::Vector3 value) {
    // ジャンプとフックの向きは水平面だけで扱い、方向が消えた場合も前方を基準にする。
    value.y = 0.0f;
    const float length = std::sqrt(value.x * value.x + value.z * value.z);

    // 水平の長さがほぼゼロなら、固定の+Zを使う。
    if (length <= 0.001f) {
        return {0.0f, 0.0f, 1.0f};
    }

    return {value.x / length, 0.0f, value.z / length};
}


} // namespace

void EnemyGiantSlime::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    // モデルの基準姿勢と接地中の判定を先に揃え、以後のジャンプ・投げ状態の起点にする。
    BaseEnemy::Initialize(common, modelName);
    SetName("Enemy_GiantSlime");
    SetEnemyType("GiantSlime");
    ReloadAttackProfile();
    SetColor({1.0f, 1.0f, 1.0f, 1.0f});
    SetDefaultColor(GetColor());
    SetScale({2.0f, 2.0f, 2.0f});
    SetRotationY(kGiantSlimeModelYawOffset);

    SetCollisionAttribute(engine::collisionCategories::Get("enemy"));
    SetCollisionMask(engine::collisionCategories::Get("player") | engine::collisionCategories::Get("ground") | engine::collisionCategories::Get("player_attack") | engine::collisionCategories::Get("player_bullet"));
    SetColliderType(engine::ColliderType::kSphere);
    SyncGroundCollisionRadius();
}

// 拘束や復帰を先に扱い、通常時はジャンプ攻撃と着地衝撃波を進める。
void EnemyGiantSlime::Update(float deltaTime) {
    // フック拘束や投げられた状態は通常AIより優先し、その間は攻撃状態を進めない。
    if (UpdateInactiveState(deltaTime)) {
        return;
    }

    UpdateTimers(deltaTime);

    // 攻撃ジャンプの下降中に接地したら、次の状態を処理する前に着地衝撃を発生させます。
    if (state_ == State::kAirborne && IsGrounded() && GetVelocity().y <= 0.5f) {
        BeginLandingRecovery();
    }

    engine::Vector3 direction = {0.0f, 0.0f, 1.0f};
    float distance = 9999.0f;
    UpdateTargetFacing(direction, distance);
    UpdateBossTargetEngagement(distance);

    // 待機中の気付きは溜めに先行させ、見た目と共通物理だけを進めます。
    if (state_ == State::kIdle && UpdateNoticeReaction(deltaTime, distance, GetDetectionRange(), direction)) {
        SetVelocity({0.0f, GetVelocity().y, 0.0f});
        ApplySlimeAnimation(deltaTime);
        SyncGroundCollisionRadius();
        BaseEnemy::Update(deltaTime);
        return;
    }

    // このフレームの行動と見た目を確定してから、判定の半径と共通物理を更新する。
    UpdateStateMachine(deltaTime, direction, distance);

    SyncGroundCollisionRadius();
    BaseEnemy::Update(deltaTime);
}

bool EnemyGiantSlime::UpdateInactiveState(float deltaTime) {
    // 敗北・運搬・フック・投げの専用更新に入った場合は、このフレームの通常行動を止める。
    // 倒された演出中は、基底の撃破更新だけを行う。
    if (ShouldHandleDefeatEffect()) {
        BaseEnemy::Update(deltaTime);
        return true;
    }

    // 運搬中はプレイヤー側が位置を決めるので、自分の行動を進めない。
    if (IsCarried()) {
        return true;
    }

    EnsureBaseScale();

    // フックの拘束更新が終わったフレームは、投擲復帰や通常攻撃へ進まない。
    if (UpdateHookSplitState(deltaTime)) {
        return true;
    }

    return UpdateThrowRecoveryState(deltaTime);
}

bool EnemyGiantSlime::UpdateHookSplitState(float deltaTime) {
    // フックを受けていない場合は、この経路で通常行動を止めない。
    if (!isHookSplitPulled_) {
        return false;
    }

    idleTimer_ += deltaTime;

    // 空中で掴まれた場合は、接地するまで物理更新を続ける。
    if (hookSplitAwaitingLanding_) {
        // 着地判定があり上向きの勢いがほぼ無い場合に、地上の抵抗へ移る。
        if (IsGrounded() && GetVelocity().y <= 0.5f) {
            BeginGroundHookResistance(true);
        // 着地までの間は横の動きを抑えながら、基底の重力更新で落下を続ける。
        } else {
            engine::Vector3 velocity = GetVelocity();
            velocity.x *= 0.88f;
            velocity.z *= 0.88f;
            SetVelocity(velocity);
            BaseEnemy::Update(deltaTime);
        }
    // 着地待ちでなければ、地上の踏ん張りが物理に引きずられないよう速度を消す。
    } else {
        SetVelocity({0.0f, 0.0f, 0.0f});

        // 接地して踏ん張っている間はCharacter更新を止めるが、被弾反応の時間だけは進めます。
        UpdateDamageFeedbackTimers(deltaTime);
    }

    ApplyHookSplitVisual(deltaTime, hookSplitOwnerPosition_);

    // 着地を待つ間は投擲用の広い判定を使う。
    if (hookSplitAwaitingLanding_) {
        SyncThrownCollisionRadius();
    // 接地した拘束中は、見た目を伸ばしても通常の判定半径へ戻す。
    } else {
        SyncGroundCollisionRadius();
    }

    return true;
}

bool EnemyGiantSlime::UpdateThrowRecoveryState(float deltaTime) {
    // 投げられている間は判定を広げ、着地後の回復中は通常の大きさへ戻す。
    // 投擲後の状態でなければ、呼び出し元に通常更新を返す。
    if (!IsThrowRecovering()) {
        return false;
    }

    // 投擲中の物理が有効な間は投擲用の判定を適用する。
    if (IsThrownPhysics()) {
        SyncThrownCollisionRadius();
    // 投擲の物理が終了して復帰中の間は、地上向けの判定に戻す。
    } else {
        SyncGroundCollisionRadius();
    }

    BaseEnemy::Update(deltaTime);
    return true;
}

void EnemyGiantSlime::EnsureBaseScale() {
    // 配置時の拡大率を一度だけ保存し、攻撃演出後に元のサイズへ戻せるようにする。
    if (!hasBaseScale_) {
        baseScale_ = GetScale();
        hasBaseScale_ = true;
    }
}

void EnemyGiantSlime::UpdateTimers(float deltaTime) {
    // 着地の発光は残り時間だけ減らし、負の値を後続の演出へ渡さない。
    idleTimer_ += deltaTime;

    // 着地発光が有効な間だけ秒数を減らし、終了値をゼロに揃える。
    if (landingPulseTimer_ > 0.0f) {
        landingPulseTimer_ = (std::max)(0.0f, landingPulseTimer_ - deltaTime);
    }
}

void EnemyGiantSlime::UpdateTargetFacing(engine::Vector3& direction, float& distance) {
    // 高低差は照準に含めず、交戦前は索敵範囲に入った対象だけへ向きを変える。
    // 対象を取得できる場合だけ、水平方向と距離を更新する。
    if (GetTarget()) {
        engine::Vector3 toTarget = GetTarget()->GetWorldPosition() - GetWorldPosition();
        direction = NormalizePlanar(toTarget);
        distance = std::sqrt(toTarget.x * toTarget.x + toTarget.z * toTarget.z);

        // 既に交戦中か新たに探知圏に入った場合に限って、対象へ向き直る。
        if (IsBossTargetEngaged() || distance <= GetDetectionRange()) {
            const float targetYaw = std::atan2(direction.x, direction.z) + kGiantSlimeModelYawOffset;
            SetRotation({0.0f, engine::Math::LerpShortAngle(GetRotation().y, targetYaw, 0.08f), 0.0f});
        }
    }
}

void EnemyGiantSlime::UpdateStateMachine(float deltaTime, const engine::Vector3& direction, float distance) {
    // 滞空中は着地予測地点を表示し、落下時間に合わせて警告を一度だけ強める。
    const EnemyAttackDefinition& attack = GetAttackDefinition(kJumpPressAttackId);

    switch (state_) {
    case State::kChargeJump:
        // 跳躍前の溜めと踏み切りへの移行を UpdateJumpCharge() へ渡す。
        UpdateJumpCharge(deltaTime);
        break;

    case State::kRecovery:
        // 着地後の硬直と姿勢復帰を進め、次の攻撃までの間隔を作る。
        UpdateLandingRecovery(deltaTime);
        break;

    case State::kAirborne:
        // 予測した着地点を表示し、下降中だけ着地までの時間から警告を強調する。
        // 警告をまだ出しておらず、下降速度が十分になった場合だけ残りの着地時間を見積もる。
        if (!landingWarningTriggered_ && GetVelocity().y < -0.1f) {
            const float height = (std::max)(0.0f, GetWorldPosition().y - landingTelegraphPosition_.y);
            const float fallSpeed = (std::max)(1.0f, -GetVelocity().y);
            const float estimatedTimeToLanding = height / fallSpeed;

            // 見積もった残り時間が先行秒以下か、予告地点との高低差が0.65以下なら警告を強める。
            if (estimatedTimeToLanding <= attack.warningLeadTime || height <= 0.65f) {
                TriggerAttackTelegraphCue({1.0f, 0.32f, 0.78f, 1.0f});
                landingWarningTriggered_ = true;
            }
        }

        // 着地の警告は地表に置き、跳躍中の本体のアニメーションは別に進める。
        ShowAttackTelegraphCircle(HitEffectDirector::ResolveGroundEffectPosition(landingTelegraphPosition_),
                                  attack.radius, 1.0f, {1.0f, 0.36f, 0.82f, 0.82f});
        ApplySlimeAnimation(deltaTime);
        break;

    case State::kIdle:
    default:
        // 攻撃の途中でなければ、対象との距離と向きから次の行動を選ぶ。
        UpdateIdleState(deltaTime, direction, distance);
        break;
    }
}

// 接地中に次の攻撃または小跳びを選び、空中では現在のアニメーションだけ進める。
void EnemyGiantSlime::UpdateIdleState(float deltaTime, const engine::Vector3& direction, float distance) {
    const EnemyAttackDefinition& attack = GetAttackDefinition(kJumpPressAttackId);

    // 足が地面についている間は、前の移動を減衰させてから次の行動を選ぶ。
    if (IsGrounded()) {
        engine::Vector3 velocity = GetVelocity();
        velocity.x *= 0.65f;
        velocity.z *= 0.65f;
        SetVelocity(velocity);

        // 既に交戦しており、標的が最大射程に入っていれば攻撃の待ち時間を通常の速度で進める。
        if (IsBossTargetEngaged() && distance <= attack.maxRange) {
            jumpTimer_ += deltaTime;

            // 待ち時間が設定した秒数に達したら、ジャンプ攻撃の溜めを始める。
            if (jumpTimer_ >= attack.cooldown) {
                BeginJumpCharge(direction, distance);
            }
        // 未交戦か射程外なら、攻撃よりも遅い間隔で次の移動を待つ。
        } else {
            jumpTimer_ += deltaTime * 0.65f;
            engine::Vector3 wanderVelocity = CalculateWanderVelocity(deltaTime, 1.25f, 0.55f);
            engine::Vector3 wanderDirection = {wanderVelocity.x, 0.0f, wanderVelocity.z};

            if (IsBossTargetEngaged() && GetTarget()) {
                // 交戦後は射程外でも目標へ跳び、ランダム徘徊に戻らないようにします。
                wanderDirection = direction;
            }

            const float wanderLength = engine::Math::Length(wanderDirection);

            // 移動方向の長さが十分な場合だけ、正規化してモデルをその方向に向ける。
            if (wanderLength > 0.001f) {
                wanderDirection = wanderDirection / wanderLength;
                const float targetYaw = std::atan2(wanderDirection.x, wanderDirection.z) + kGiantSlimeModelYawOffset;
                SetRotation({0.0f, engine::Math::LerpShortAngle(GetRotation().y, targetYaw, 0.05f), 0.0f});
            }

            // 小跳びの時間が経過し、実際に向かう方向がある場合に飛び出す。
            if (jumpTimer_ >= 1.75f && wanderLength > 0.05f) {
                SetVelocity({wanderDirection.x * 4.6f, kHeavyHopDefaultJumpPower * 0.55f, wanderDirection.z * 4.6f});
                jumpTimer_ = 0.0f;
                SetGrounded(false);
            }
        }
    }

    ApplySlimeAnimation(deltaTime);
}

void EnemyGiantSlime::BeginThrown(const engine::Vector3& initialVelocity) {
    // 投げ状態へ移る際にジャンプ警告とフック変形を消し、判定を投擲用へ切り替える。
    // 拡縮の基準をまだ控えていなければ、投擲前の値を保存する。
    if (!hasBaseScale_) {
        baseScale_ = GetScale();
        hasBaseScale_ = true;
    }

    // 攻撃中の時間とフック拘束を解除し、投げられる前の見た目へ戻す。
    state_ = State::kIdle;
    stateTimer_ = 0.0f;
    telegraphTimer_ = 0.0f;
    landingPulseTimer_ = 0.0f;
    isJumpingAttack_ = false;
    landingWarningTriggered_ = false;
    isHookSplitPulled_ = false;
    hookSplitAwaitingLanding_ = false;
    hookSplitPhase_ = HookSplitPhase::kNone;
    ResetHookSplitVisual();
    HideAttackTelegraph();
    SetColor(GetDefaultColor());
    SetScale(baseScale_);
    SyncThrownCollisionRadius();
    BaseEnemy::BeginThrown(initialVelocity);
}

std::unique_ptr<engine::Object3D> EnemyGiantSlime::Clone() const {
    // 複製先に配置設定と追跡対象を引き継ぎ、攻撃状態の一時値は初期化し直す。
    auto clone = std::make_unique<EnemyGiantSlime>();
    assert(GetObjectCommon() != nullptr);
    clone->Initialize(GetObjectCommon(), GetModelName());
    clone->CopyFrom(this);
    clone->SetTarget(GetTarget());
    clone->SetDetectionRange(GetDetectionRange());
    return clone;
}

void EnemyGiantSlime::BeginJumpCharge(const engine::Vector3& direction, float distance) {
    // 射程に収めた着地点を予告し、溜め中の移動を止めて回避の猶予を作る。
    state_ = State::kChargeJump;
    stateTimer_ = 0.0f;
    telegraphTimer_ = kTelegraphInterval;
    jumpTimer_ = 0.0f;
    isJumpingAttack_ = false;
    landingWarningTriggered_ = false;
    SetVelocity({0.0f, 0.0f, 0.0f});

    SetRotation({0.0f, GetRotation().y, 0.0f});

    // 標的までの距離を技の射程に収め、円が示す位置をまず決める。
    const EnemyAttackDefinition& attack = GetAttackDefinition(kJumpPressAttackId);
    const float jumpDistance = std::clamp(distance * 0.95f, attack.minRange, attack.maxRange);
    landingTelegraphPosition_ = GetTranslate() + direction * jumpDistance;
    landingTelegraphPosition_.y = GetTranslate().y;
    SpawnLandingTelegraph();
    SetColor({1.0f, 0.62f, 0.86f, 1.0f});
}

void EnemyGiantSlime::UpdateJumpCharge(float deltaTime) {
    // 溜め中は対象の移動を追って着地点を更新し、発射後は予告地点を固定する。
    const EnemyAttackDefinition& attack = GetAttackDefinition(kJumpPressAttackId);
    stateTimer_ += deltaTime;
    telegraphTimer_ += deltaTime;
    SetVelocity({0.0f, 0.0f, 0.0f});

    // 標的がいる場合は、まだ離陸していないので予告をその動きへ追従させる。
    if (GetTarget()) {
        engine::Vector3 toTarget = GetTarget()->GetWorldPosition() - GetWorldPosition();
        toTarget.y = 0.0f;
        const float targetDistance = engine::Math::Length(toTarget);

        // 水平距離がほぼゼロでなければ、向きと射程を更新できる。
        if (targetDistance > 0.001f) {
            engine::Vector3 direction = toTarget / targetDistance;
            const float jumpDistance = std::clamp(targetDistance * 0.95f, attack.minRange, attack.maxRange);
            landingTelegraphPosition_ = GetTranslate() + direction * jumpDistance;
            landingTelegraphPosition_.y = GetTranslate().y;
            const float targetYaw = std::atan2(direction.x, direction.z) + kGiantSlimeModelYawOffset;
            SetRotation({0.0f, engine::Math::LerpShortAngle(GetRotation().y, targetYaw, 0.12f), 0.0f});
        }
    }

    // 追従する着地円と別に、床へ立ち上がる予兆リングを一定間隔で出します。
    if (telegraphTimer_ >= kTelegraphInterval) {
        SpawnLandingTelegraph();
        telegraphTimer_ = 0.0f;
    }

    // 持続表示の円と溜め中のつぶれを、同じ進行率で強める。
    const float windupDuration = (std::max)(0.01f, attack.windupDuration);
    const float t = std::clamp(stateTimer_ / windupDuration, 0.0f, 1.0f);
    ShowAttackTelegraphCircle(HitEffectDirector::ResolveGroundEffectPosition(landingTelegraphPosition_), attack.radius,
                              t, {1.0f, 0.36f, 0.82f, 0.82f});
    const float pulse = std::sin(t * std::numbers::pi_v<float> * 5.0f) * 0.05f;
    SetScale({baseScale_.x * (1.0f + t * 0.34f + pulse), baseScale_.y * (1.0f - t * 0.34f),
              baseScale_.z * (1.0f + t * 0.34f - pulse)});

    // 予備動作の最後に着地点を基準とした方向と距離へ変換し、移動を LaunchJump() に渡します。
    if (stateTimer_ >= windupDuration) {
        engine::Vector3 toLanding = landingTelegraphPosition_ - GetTranslate();
        toLanding.y = 0.0f;
        const float landingDistance = engine::Math::Length(toLanding);
        engine::Vector3 jumpDirection = landingDistance > 0.001f ? toLanding / landingDistance : engine::Vector3{0.0f, 0.0f, 1.0f};
        LaunchJump(jumpDirection, landingDistance);
    }
}

void EnemyGiantSlime::BeginLandingRecovery() {
    // 着地の瞬間だけ衝撃波とエフェクトを発生させ、回復状態へ移す。
    state_ = State::kRecovery;
    stateTimer_ = 0.0f;
    isJumpingAttack_ = false;
    landingPulseTimer_ = 0.35f;

    // 滞空中に警告を強調できなかった場合は、着地時に一度だけ出す。
    if (!landingWarningTriggered_) {
        TriggerAttackTelegraphCue({1.0f, 0.32f, 0.78f, 1.0f});
        landingWarningTriggered_ = true;
    }

    // 予兆を外して着地点で本体を停止し、命中判定と表示を起動する。
    HideAttackTelegraph();
    SetVelocity({0.0f, 0.0f, 0.0f});
    SetColor({1.0f, 0.44f, 0.78f, 1.0f});
    DispatchLandingShockwave();
    SpawnLandingEffects();
}

void EnemyGiantSlime::UpdateLandingRecovery(float deltaTime) {
    // 潰れた姿を回復時間に沿って元へ戻し、終了時に色とサイズを復元する。
    stateTimer_ += deltaTime;
    SetVelocity({0.0f, 0.0f, 0.0f});

    const float recoveryDuration = (std::max)(0.01f, GetAttackDefinition(kJumpPressAttackId).recoveryDuration);
    const float t = std::clamp(stateTimer_ / recoveryDuration, 0.0f, 1.0f);
    const float wobble = std::sin(stateTimer_ * 28.0f) * (1.0f - t) * 0.06f;
    SetScale({baseScale_.x * (1.24f - t * 0.24f + wobble), baseScale_.y * (0.58f + t * 0.42f),
              baseScale_.z * (1.24f - t * 0.24f - wobble)});

    // 硬直の秒数を使い切ったら、待機色と基準スケールに戻す。
    if (stateTimer_ >= recoveryDuration) {
        state_ = State::kIdle;
        stateTimer_ = 0.0f;
        jumpTimer_ = 0.0f;
        SetColor(GetDefaultColor());
        SetScale(baseScale_);
    }
}

void EnemyGiantSlime::SpawnLandingTelegraph() {
    // 予告リングを地面より少し上に置き、地形との重なりで消えないようにする。
    engine::Vector3 effectPos = HitEffectDirector::ResolveGroundEffectPosition(landingTelegraphPosition_);
    effectPos.y += 0.04f;

    // 演出管理が利用できる場合だけ、予告のリングを発生させる。
    if (auto* meshEffect = MeshEffectManager::GetInstance()) {
        meshEffect->SpawnRingWaveEffect(effectPos);
    }
}

void EnemyGiantSlime::SpawnLandingEffects() {
    // 衝撃波・攻撃設定のメッシュVFX・粒子を同じ着地点から再生する。
    engine::Vector3 effectPos = HitEffectDirector::ResolveGroundEffectPosition(GetTranslate());
    effectPos.y += 0.04f;
    HitEffectDirector::SpawnThrowSlamShockwave(effectPos, 24.0f);

    const EnemyAttackDefinition& attack = GetAttackDefinition(kJumpPressAttackId);

    // 攻撃定義が追加の着地素材を指定しているときだけ読み込む。
    if (!attack.impactVfx.empty()) {
        // メッシュ演出の管理先がある場合に、指定素材を衝撃波へ重ねる。
        if (auto* meshEffect = MeshEffectManager::GetInstance()) {
            meshEffect->SpawnEffectAt(attack.impactVfx, effectPos, {0.0f, 0.0f, 0.0f}, {2.0f, 1.0f, 2.0f});
        }
    }

    engine::ParticleSystem* particleSystem = nullptr;

    // 現在のシーンに接続できる場合は、CPU粒子を発生させる入口を取得する。
    if (BaseScene* scene = SceneManager::GetInstance() ? SceneManager::GetInstance()->GetCurrentScene() : nullptr) {
        particleSystem = scene->GetParticleSystem();
    }

    // CPU粒子を使える場合だけ、着地点の上方へ発生させる。
    if (particleSystem) {
        engine::Vector3 up = {0.0f, 1.0f, 0.0f};
        particleSystem->SpawnParticles(effectPos, 42, 4.5f, &up, 42.0f, {1.0f, 0.55f, 0.84f, 1.0f},
                                       {1.0f, 0.86f, 0.95f, 0.0f}, 0.18f, 0.48f, 0.65f, 0.08f);
    }
}

void EnemyGiantSlime::BeginHookSplitPull(const engine::Vector3& hookOwnerPos) {
    // フックを受けた時点の姿勢を保存し、空中なら着地してから分裂の抵抗時間を始める。
    if (hasSplit_)
        return;

    EnsureBaseScale();

    // 攻撃を止めて拘束を開始し、向きと基準位置を記録して表示のばねを初期化する。
    isHookSplitPulled_ = true;
    HideAttackTelegraph();
    hookSplitWeakPoint_ = state_ == State::kRecovery;
    hookSplitPullTimer_ = 0.0f;
    hookSplitPhaseTimer_ = 0.0f;
    hookSplitOwnerPosition_ = hookOwnerPos;
    hookSplitBasePosition_ = GetTranslate();
    hookSplitBaseScale_ = baseScale_;
    hookSplitDirection_ = NormalizePlanar(hookOwnerPos - GetTranslate());
    hookVisualScale_ = {1.0f, 1.0f, 1.0f};
    hookVisualScaleVelocity_ = {0.0f, 0.0f, 0.0f};
    hookVisualRotation_ = {0.0f, 0.0f, 0.0f};
    hookVisualRotationVelocity_ = {0.0f, 0.0f, 0.0f};
    hookVisualOffset_ = {0.0f, 0.0f, 0.0f};
    hookVisualOffsetVelocity_ = {0.0f, 0.0f, 0.0f};
    hookVisualShear_ = {0.0f, 0.0f, 0.0f};
    hookVisualShearVelocity_ = {0.0f, 0.0f, 0.0f};
    state_ = State::kIdle;
    stateTimer_ = 0.0f;
    isJumpingAttack_ = false;
    jumpTimer_ = 0.0f;
    SetScale(baseScale_);

    hookSplitAwaitingLanding_ = !IsGrounded();

    // 空中で掴んだら水平方向と上向きの速度を抑え、着地までの拘束段階へ移す。
    if (hookSplitAwaitingLanding_) {
        hookSplitPhase_ = HookSplitPhase::kAirborneTether;
        engine::Vector3 velocity = GetVelocity();
        velocity.x *= 0.20f;
        velocity.z *= 0.20f;
        velocity.y = (std::min)(velocity.y, 1.5f);
        SetVelocity(velocity);
        SyncThrownCollisionRadius();
    // 接地中に掴んだ場合は、着地の反動を挟まず地上の抵抗を始める。
    } else {
        BeginGroundHookResistance(false);
    }

    SetColor(hookSplitWeakPoint_ ? engine::Vector4{1.0f, 0.48f, 0.86f, 1.0f} : engine::Vector4{0.42f, 0.88f, 1.0f, 1.0f});
}

bool EnemyGiantSlime::UpdateHookSplitPull(float deltaTime, const engine::Vector3& hookOwnerPos,
                                          engine::ParticleSystem* particleSystem) {
    // 引き先の変化を追いながら、着地・踏ん張り・滑り・分裂の段階を進める。
    // すでに分裂した場合は、引き継ぐ分裂更新が無いことを成功として通知する。
    if (hasSplit_) {
        return true;
    }

    // 拘束がまだ開始されていない入力の場合は、その場で拘束を初期化する。
    if (!isHookSplitPulled_) {
        BeginHookSplitPull(hookOwnerPos);
    }

    hookSplitOwnerPosition_ = hookOwnerPos;
    engine::Vector3 toOwner = hookOwnerPos - GetTranslate();
    const engine::Vector3 nextDirection = NormalizePlanar(toOwner);

    // 有効な長さが得られた方向を、フックの水平基準に設定する。
    if (engine::Math::Length(nextDirection) > 0.001f) {
        hookSplitDirection_ = nextDirection;
    }

    // 空中では引き寄せの姿勢だけを更新し、地上へ下りるまで分裂ゲージを進めません。
    if (hookSplitAwaitingLanding_) {
        // 落下待ちで着地を検出した場合は、抵抗の前に着地の反動を始める。
        if (IsGrounded() && GetVelocity().y <= 0.5f) {
            BeginGroundHookResistance(true);
        }

        ApplyHookSplitVisual(deltaTime, hookOwnerPos);
        return false;
    }

    // 着地直後の反動は独立して数え、踏ん張り・滑り・破断はその終了後に進めます。
    if (hookSplitPhase_ == HookSplitPhase::kLandingSquash) {
        hookSplitPhaseTimer_ += deltaTime;

        // 着地の反動が所定の時間まで進んだら、踏ん張りの段階へ切り替える。
        if (hookSplitPhaseTimer_ >= kHookLandingSquashDuration) {
            hookSplitPhase_ = HookSplitPhase::kBrace;
            hookSplitPhaseTimer_ = 0.0f;
        }
    // 着地の反動以外の地上段階では、引かれてからの累積秒数を使って分裂の段階を決める。
    } else {
        hookSplitPullTimer_ += deltaTime;
        const float progress = GetHookSplitProgress();

        // 分裂の進行が84%以上なら、本体が破れる直前の段階として扱う。
        if (progress >= 0.84f) {
            hookSplitPhase_ = HookSplitPhase::kTear;
        // 55%以上で破断直前に届いていない場合は、滑って引かれる段階にする。
        } else if (progress >= 0.55f) {
            hookSplitPhase_ = HookSplitPhase::kSlip;
        // 進行が55%未満なら、足を止めて踏ん張る段階を選ぶ。
        } else {
            hookSplitPhase_ = HookSplitPhase::kBrace;
        }
    }

    ApplyHookSplitVisual(deltaTime, hookOwnerPos);
    const float progress = GetHookSplitProgress();

    // 末尾の姿勢を更新してから分裂を確定し、呼び出し元に捕獲終了を通知します。
    if (progress >= 1.0f) {
        SplitIntoSmallSlimes(particleSystem);
        return true;
    }

    return false;
}

void EnemyGiantSlime::CancelHookSplitPull() {
    // 分裂が確定していない場合だけ、フックの変形とAI状態を通常姿勢へ戻す。
    if (hasSplit_)
        return;
    isHookSplitPulled_ = false;
    HideAttackTelegraph();
    hookSplitWeakPoint_ = false;
    hookSplitAwaitingLanding_ = false;
    hookSplitPullTimer_ = 0.0f;
    hookSplitPhaseTimer_ = 0.0f;
    hookSplitPhase_ = HookSplitPhase::kNone;
    state_ = State::kIdle;
    stateTimer_ = 0.0f;
    SetColor(GetDefaultColor());
    SetScale(baseScale_);
    engine::Vector3 rot = GetRotation();
    rot.x = 0.0f;
    rot.z = 0.0f;
    SetRotation(rot);
    ResetHookSplitVisual();
}

float EnemyGiantSlime::GetHookSplitProgress() const {
    // 空中と着地直後は進捗を止め、回復直後の弱点状態では抵抗時間を短くする。
    if (hookSplitAwaitingLanding_ || hookSplitPhase_ == HookSplitPhase::kLandingSquash) {
        return 0.0f;
    }

    const float duration = hookSplitWeakPoint_ ? kHookSplitWeakDuration : kHookSplitDuration;
    return std::clamp(hookSplitPullTimer_ / duration, 0.0f, 1.0f);
}

const char* EnemyGiantSlime::GetDebugHookSplitPhaseName() const {
    // プレビューには内部状態の段階名を返し、分裂済みを最優先で表示する。
    if (hasSplit_)
        return "分裂完了";
    switch (hookSplitPhase_) {
    case HookSplitPhase::kAirborneTether:
        return "空中拘束・落下";

    case HookSplitPhase::kLandingSquash:
        return "着地・受け止め";

    case HookSplitPhase::kBrace:
        return "踏ん張り・抵抗";

    case HookSplitPhase::kSlip:
        return "引きずられ";

    case HookSplitPhase::kTear:
        return "限界・分裂寸前";

    case HookSplitPhase::kNone:
    default:
        return "通常";
    }
}

engine::Vector3 EnemyGiantSlime::GetHookAttachmentPosition() const {
    // 見た目のせん断・拡縮を反映した位置を返し、フックの線が変形前の胴体に残らないようにする。
    engine::MeshRenderer* renderer = GetMeshRenderer();
    engine::Model* model = renderer ? renderer->GetModel() : nullptr;

    // モデルの基準座標を取得できなければ、本体の位置を引く方向と上へ補正した点を使う。
    if (!model) {
        return GetWorldPosition() + hookSplitDirection_ * 0.8f + engine::Vector3{0.0f, 0.7f, 0.0f};
    }

    const engine::Vector3 modelCenter = model->GetCenter();
    const engine::Vector3 modelSize = model->GetSize();
    const float shearPivotY = modelCenter.y - modelSize.y * 0.5f;
    const engine::Vector3 localAttachment = {modelCenter.x, modelCenter.y + modelSize.y * 0.18f,
                                     modelCenter.z - modelSize.z * 0.32f};

    // 接点にモデルの見た目と同じせん断・拡縮を順に適用してから、ワールドへ変換する。
    engine::Matrix4x4 shearLocal = engine::Math::MakeIdentity4x4();
    shearLocal.m[1][0] = hookVisualShear_.x;
    shearLocal.m[1][2] = hookVisualShear_.z;
    shearLocal.m[3][0] = -shearPivotY * hookVisualShear_.x;
    shearLocal.m[3][2] = -shearPivotY * hookVisualShear_.z;
    const engine::Matrix4x4 visualAffine = engine::Math::MakeAffineMatrix(hookVisualScale_, hookVisualRotation_, hookVisualOffset_);
    const engine::Matrix4x4 visualLocal = engine::Math::Multiply(shearLocal, visualAffine);
    return engine::Math::Transform(engine::Math::Transform(localAttachment, visualLocal), GetWorldMatrix());
}

void EnemyGiantSlime::BeginGroundHookResistance(bool landedFromAir) {
    // 接地後に移動を止め、空中から来た場合は着地の潰れ演出を先に再生する。
    hookSplitAwaitingLanding_ = false;
    hookSplitBasePosition_ = GetTranslate();
    hookSplitBaseScale_ = baseScale_;
    hookSplitPullTimer_ = 0.0f;
    hookSplitPhaseTimer_ = 0.0f;
    hookSplitPhase_ = landedFromAir ? HookSplitPhase::kLandingSquash : HookSplitPhase::kBrace;
    SetGrounded(true);
    SetVelocity({0.0f, 0.0f, 0.0f});
    SyncGroundCollisionRadius();
}

void EnemyGiantSlime::ApplyHookSplitVisual(float deltaTime, const engine::Vector3& hookOwnerPos) {
    // 掴まれている最中にだけ見た目を変え、分裂し終えた本体には触れない。
    if (!isHookSplitPulled_ || hasSplit_) {
        return;
    }

    engine::Vector3 toOwner = hookOwnerPos - GetTranslate();
    // フックを引く方向は主に水平面から求め、向きと左右の揺れに同じ基準を使う。
    const float planarDistance = std::sqrt(toOwner.x * toOwner.x + toOwner.z * toOwner.z);
    engine::Vector3 direction = NormalizePlanar(toOwner);
    hookSplitDirection_ = direction;
    const engine::Vector3 side = {-direction.z, 0.0f, direction.x};
    const float targetYaw = std::atan2(direction.x, direction.z) + kGiantSlimeModelYawOffset;

    engine::Vector3 targetVisualScale = {1.0f, 1.0f, 1.0f};
    engine::Vector3 targetVisualRotation = engine::Vector3::kZero;
    engine::Vector3 targetVisualShear = engine::Vector3::kZero;
    engine::Vector3 targetPosition = GetTranslate();
    bool pinGroundAnchor = false;

    // 空中では落下速度とフックの角度を姿勢へ反映し、地上の踏ん張りとは分ける。
    if (hookSplitAwaitingLanding_) {
        const float fallRate = std::clamp(std::abs(GetVelocity().y) / 24.0f, 0.0f, 1.0f);
        const float tetherPulse = std::sin(idleTimer_ * 13.0f);
        targetVisualScale = {1.0f - fallRate * 0.025f, 1.0f + fallRate * 0.045f, 1.0f - fallRate * 0.020f};
        const float tetherPitch = std::atan2(toOwner.y, (std::max)(planarDistance, 0.001f));
        targetVisualRotation.x = std::clamp(-tetherPitch * 0.34f, -0.28f, 0.28f);
        targetVisualRotation.z = tetherPulse * 0.055f;
        targetVisualShear.z = -0.055f + tetherPulse * 0.018f;
    } else if (hookSplitPhase_ == HookSplitPhase::kLandingSquash) {
        // 接地した瞬間は一度潰し、その後の反動までを着地用の時計で進める。
        const float t = Easing::SmoothStep01(hookSplitPhaseTimer_ / kHookLandingSquashDuration);

        // 接地演出の前半は衝撃で体を下へつぶす。
        if (t < 0.48f) {
            const float impact = Easing::SmoothStep01(t / 0.48f);
            targetVisualScale = {1.0f + impact * 0.34f, 1.0f - impact * 0.42f, 1.0f + impact * 0.22f};
        // 後半はその姿勢から反動で体を起こす。
        } else {
            const float rebound = Easing::SmoothStep01((t - 0.48f) / 0.52f);
            targetVisualScale = {1.34f - rebound * 0.30f, 0.58f + rebound * 0.48f, 1.22f - rebound * 0.16f};
        }

        targetVisualRotation.z = std::sin(t * std::numbers::pi_v<float>) * 0.08f;
        pinGroundAnchor = true;
    // 落下待ちと着地反動を終えた地上段階は、引かれる動きと踏み戻す動きを交互に重ねる。
    } else {
        const float progress = GetHookSplitProgress();
        const float tearProgress = Easing::SmoothStep01((progress - 0.82f) / 0.18f);
        // 地上では一周期を引かれる動きと踏み戻す動きへ分け、同じ足元で抵抗して見えるようにする。
        const float pullCycle = std::fmod(hookSplitPullTimer_ * 2.55f, 1.0f);
        const float yankRise = Easing::SmoothStep01(pullCycle / 0.14f);
        const float yankFall = 1.0f - Easing::SmoothStep01((pullCycle - 0.14f) / 0.28f);
        const float yankPulse = yankRise * yankFall;
        const float braceRise = Easing::SmoothStep01((pullCycle - 0.30f) / 0.18f);
        const float braceFall = 1.0f - Easing::SmoothStep01((pullCycle - 0.50f) / 0.30f);
        const float bracePulse = braceRise * braceFall;
        const float struggleSide = std::sin(hookSplitPullTimer_ * 22.0f) * (0.018f + tearProgress * 0.035f);

        // 上側を引いて底面側を踏み戻す反復を、拡縮・傾き・せん断で表す。
        targetVisualScale = {1.0f + bracePulse * 0.055f + yankPulse * 0.025f + tearProgress * 0.025f,
                             1.0f - bracePulse * 0.075f - yankPulse * 0.025f - tearProgress * 0.035f,
                             1.0f + yankPulse * 0.025f - bracePulse * 0.020f + tearProgress * 0.025f};
        targetVisualRotation.x = bracePulse * 0.018f - yankPulse * 0.025f;
        targetVisualRotation.z = struggleSide + (bracePulse - yankPulse) * 0.025f;
        targetVisualShear.z = -0.055f - yankPulse * (0.18f + progress * 0.14f) +
                              bracePulse * 0.085f * (1.0f - progress) -
                              std::abs(std::sin(hookSplitPullTimer_ * 28.0f)) * tearProgress * 0.11f;
        targetVisualShear.x = struggleSide * (0.8f + tearProgress * 0.7f);

        const engine::Vector3 baseToOwner = hookOwnerPos - hookSplitBasePosition_;
        const float basePlanarDistance = std::sqrt(baseToOwner.x * baseToOwner.x + baseToOwner.z * baseToOwner.z);
        // 引っ張る方向へ滑る量は初期距離に応じて決め、分裂に近づくにつれて段階的に増やす。
        const float maximumSlip = (std::min)(basePlanarDistance * 0.12f, 1.65f);
        // 分裂進捗50%、68%、84%から滑りを順に足し、三段階が終わると最大移動量に達します。
        const float slipStep1 = Easing::SmoothStep01((progress - 0.50f) / 0.10f);
        const float slipStep2 = Easing::SmoothStep01((progress - 0.68f) / 0.09f);
        const float slipStep3 = Easing::SmoothStep01((progress - 0.84f) / 0.09f);
        const float permanentSlip = maximumSlip * (slipStep1 * 0.28f + slipStep2 * 0.32f + slipStep3 * 0.40f);
        const float temporaryYank = yankPulse * (0.055f + progress * 0.12f);
        const float pushBack = bracePulse * 0.045f * (1.0f - progress);
        // 累積で滑った距離へ一時的な引きと踏み戻しを足し、足元の高さは変えない。
        const float pullDistance = (std::max)(0.0f, permanentSlip + temporaryYank - pushBack);
        targetPosition = hookSplitBasePosition_ + direction * pullDistance;
        targetPosition += side * struggleSide * (0.45f + tearProgress * 0.55f);
        targetPosition.y = hookSplitBasePosition_.y;
        pinGroundAnchor = true;
    }

    // 揺れや潰れが重なっても行き過ぎないよう、軸ごとに大きさを制限する。
    targetVisualScale.x = std::clamp(targetVisualScale.x, 0.82f, 1.38f);
    targetVisualScale.y = std::clamp(targetVisualScale.y, 0.56f, 1.16f);
    targetVisualScale.z = std::clamp(targetVisualScale.z, 0.82f, 1.28f);

    // 各変形へ減衰ばねをかけ、フックの方向や段階が変わっても姿勢を滑らかにつなぎます。
    slimeBounceAnimator::StepDampedSpring(hookVisualScale_, hookVisualScaleVelocity_, targetVisualScale, deltaTime,
                                          20.0f, 0.62f);
    slimeBounceAnimator::StepDampedSpring(hookVisualRotation_, hookVisualRotationVelocity_, targetVisualRotation,
                                          deltaTime, 18.0f, 0.70f);
    slimeBounceAnimator::StepDampedSpring(hookVisualShear_, hookVisualShearVelocity_, targetVisualShear, deltaTime,
                                          24.0f, 0.64f);

    engine::Vector3 targetVisualOffset =
        pinGroundAnchor ? CalculateHookAnchorOffset(hookVisualScale_, hookVisualRotation_) : engine::Vector3::kZero;
    slimeBounceAnimator::StepDampedSpring(hookVisualOffset_, hookVisualOffsetVelocity_, targetVisualOffset, deltaTime,
                                          22.0f, 0.78f);

    // 被弾は今回の描画姿勢だけに重ね、踏ん張り用のばねと当たり判定の基準を維持します。
    engine::Vector3 renderedScale = hookVisualScale_;
    engine::Vector3 renderedRotation = hookVisualRotation_;
    engine::Vector3 renderedOffset = hookVisualOffset_;
    ApplyDamageReactionPose(renderedScale, renderedRotation, &renderedOffset, 0.72f);

    // 見た目の変形と別に、実体の大きさは基準へ戻してフック側を向かせる。
    SetScale(baseScale_);
    SetRotation({0.0f, targetYaw, 0.0f});

    // 地上の段階だけは計算した滑り位置を実体へ反映する。空中の位置は物理側に任せる。
    if (!hookSplitAwaitingLanding_) {
        SetTranslate(targetPosition);
    }

    // 足元を固定するため、せん断の支点は描画モデルの下側に合わせる。
    if (engine::MeshRenderer* renderer = GetMeshRenderer()) {
        renderer->SetVisualTransform(renderedScale, renderedRotation, renderedOffset);
        float shearPivotY = 0.0f;

        // モデルを取得できる場合は底面の高さを使い、それ以外ではローカルYの0を支点にする。
        if (engine::Model* model = renderer->GetModel()) {
            shearPivotY = model->GetCenter().y - model->GetSize().y * 0.5f;
        }

        renderer->SetVisualShear(hookVisualShear_, shearPivotY);
    }

    // 分裂の進み具合と引かれる揺れを色に反映し、次の描画に使う行列を更新する。
    const float progress = GetHookSplitProgress();
    const float strainPulse = std::abs(std::sin(hookSplitPullTimer_ * 18.0f));
    SetColor({0.42f + progress * 0.52f, 0.88f - progress * 0.30f + strainPulse * 0.06f, 1.0f - progress * 0.10f, 1.0f});
    UpdateLocalMatrix();
    UpdateWorldMatrix();
}

engine::Vector3 EnemyGiantSlime::CalculateHookAnchorOffset(const engine::Vector3& visualScale, const engine::Vector3& visualRotation) const {
    engine::MeshRenderer* renderer = GetMeshRenderer();
    engine::Model* model = renderer ? renderer->GetModel() : nullptr;

    // モデルが無ければ、Y・Zの倍率だけから代用の補正量を返す。
    if (!model) {
        return {0.0f, 0.5f * (1.0f - visualScale.y), 0.5f * (1.0f - visualScale.z)};
    }

    const engine::Vector3 modelCenter = model->GetCenter();
    const engine::Vector3 modelSize = model->GetSize();

    // 描画姿勢が変わっても、プレイヤーと反対側の接地点を同じ場所に保ちます。
    const engine::Vector3 rearGroundAnchor = {modelCenter.x, modelCenter.y - modelSize.y * 0.5f,
                                      modelCenter.z + modelSize.z * 0.5f};
    const engine::Matrix4x4 visualMatrix = engine::Math::MakeAffineMatrix(visualScale, visualRotation, {0.0f, 0.0f, 0.0f});
    const engine::Vector3 deformedAnchor = engine::Math::Transform(rearGroundAnchor, visualMatrix);
    return rearGroundAnchor - deformedAnchor;
}

void EnemyGiantSlime::ResetHookSplitVisual() {
    // フック用のばね変形をすべて消し、通常アニメーションに残留させない。
    hookVisualScale_ = {1.0f, 1.0f, 1.0f};
    hookVisualScaleVelocity_ = {0.0f, 0.0f, 0.0f};
    hookVisualRotation_ = {0.0f, 0.0f, 0.0f};
    hookVisualRotationVelocity_ = {0.0f, 0.0f, 0.0f};
    hookVisualOffset_ = {0.0f, 0.0f, 0.0f};
    hookVisualOffsetVelocity_ = {0.0f, 0.0f, 0.0f};
    hookVisualShear_ = {0.0f, 0.0f, 0.0f};
    hookVisualShearVelocity_ = {0.0f, 0.0f, 0.0f};

    // 描画担当が存在する場合だけ、表示中の変形も初期状態へ戻す。
    if (engine::MeshRenderer* renderer = GetMeshRenderer()) {
        renderer->ResetVisualTransform();
    }
}

void EnemyGiantSlime::LaunchJump(const engine::Vector3& direction, float distance) {
    // 重力から滞空時間を概算し、予告地点まで届く水平速度を上限付きで決める。
    const float gravity = GetParameters().has_value() ? (std::max)(1.0f, GetParameters()->gravity) : 70.0f;
    const float configuredJumpPower =
        GetParameters().has_value() && GetParameters()->jumpPower > 0.0f ? GetParameters()->jumpPower : kHeavyHopDefaultJumpPower;
    const float jumpPower = std::clamp(configuredJumpPower, kHeavyHopMinJumpPower, kHeavyHopMaxJumpPower);
    // 飛び上がった位置と同じ高さへ戻る時間は上向き初速の2倍を重力で割った値。ここでは調整範囲に収めます。
    const float flightTime = std::clamp((jumpPower * 2.0f) / gravity, 0.42f, 0.68f);
    const float horizontalSpeed = std::clamp(distance / flightTime, kHeavyHopMinSpeed, kHeavyHopMaxSpeed);
    SetVelocity({direction.x * horizontalSpeed, jumpPower, direction.z * horizontalSpeed});
    state_ = State::kAirborne;
    stateTimer_ = 0.0f;
    isJumpingAttack_ = true;
    SetGrounded(false);
    SetColor(GetDefaultColor());
}

float EnemyGiantSlime::GetBodyAttackDamage() const {
    // 上昇中・フック拘束中は無害。下降中の踏みつけだけ本体に攻撃力を持たせます。
    return state_ == State::kAirborne && isJumpingAttack_ && GetVelocity().y < 0.0f && !isHookSplitPulled_ &&
                   !hookSplitAwaitingLanding_ && !hasSplit_
               ? GetAttackDefinition(kJumpPressAttackId).damage
               : 0.0f;
}

void EnemyGiantSlime::DispatchLandingShockwave() {
    // 着地地点から水平方向の距離で対象を選び、衝撃波のダメージと押し出しを配る。
    // 標的がいない場合は、着地の表示だけで衝撃の通知を送らない。
    if (!GetTarget())
        return;

    engine::Vector3 diff = GetTarget()->GetTranslate() - GetTranslate();
    diff.y = 0.0f;
    const float distance = engine::Math::Length(diff);
    const EnemyAttackDefinition& attack = GetAttackDefinition(kJumpPressAttackId);
    // 標的の水平距離が衝撃波の半径より外なら命中を送らない。
    if (distance > attack.radius)
        return;

    engine::Vector3 direction = distance > 0.001f ? diff / distance : engine::Vector3{0.0f, 0.0f, 1.0f};
    DamageEvent damageEvent;
    damageEvent.target = GetTarget();
    damageEvent.attacker = this;
    damageEvent.damageAmount = attack.damage;
    damageEvent.knockbackVelocity = {direction.x * 18.0f, 13.0f, direction.z * 18.0f};
    EventManager::GetInstance()->Dispatch(damageEvent);
}

void EnemyGiantSlime::ApplySlimeAnimation(float deltaTime) {
    // 攻撃段階ごとの潰れ・伸びと被弾反応を合成し、配置時の大きさを基準に描画する。
    engine::Vector3 targetScale = baseScale_;
    engine::Vector3 targetRotation = {0.0f, GetRotation().y, 0.0f};

    // 着地の発光時間が残っている間は、着地で潰れた姿勢を優先する。
    if (landingPulseTimer_ > 0.0f) {
        const float t = landingPulseTimer_ / 0.35f;
        targetScale.x = baseScale_.x * (1.35f - t * 0.25f);
        targetScale.y = baseScale_.y * (0.62f + t * 0.25f);
        targetScale.z = baseScale_.z * (1.35f - t * 0.25f);
    // 着地が終わって空中にいる場合は、縦方向の伸びを持つ共通アニメーションを使う。
    } else if (!IsGrounded()) {
        slimeBounceAnimator::Params params;
        params.speedForFullBounce = 6.0f;
        params.idleAmplitude = 0.045f;
        params.moveAmplitude = 0.18f;
        params.hopFrequency = 6.8f;
        params.horizontalSquash = 0.20f;
        params.verticalStretch = 0.26f;
        params.airborneStretch = 0.30f;
        targetScale = slimeBounceAnimator::MakeScale(baseScale_, GetVelocity(), idleTimer_, false, params);
    // 接地中に次の跳躍が近いときは、飛び出す前につぶれる姿勢を表示する。
    } else if (jumpTimer_ > 0.95f) {
        const float chargeRate = std::clamp((jumpTimer_ - 0.95f) / 0.32f, 0.0f, 1.0f);
        targetScale = slimeBounceAnimator::MakeChargeSquash(baseScale_, chargeRate, idleTimer_, 1.08f);
    // 通常の接地移動と待機では、速度に応じた地上の弾みを適用する。
    } else {
        slimeBounceAnimator::Params params;
        params.speedForFullBounce = 3.8f;
        params.idleAmplitude = 0.060f;
        params.moveAmplitude = 0.21f;
        params.hopFrequency = 6.6f;
        params.horizontalSquash = 0.22f;
        params.verticalStretch = 0.27f;
        params.airborneStretch = 0.28f;
        targetScale = slimeBounceAnimator::MakeScale(baseScale_, GetVelocity(), idleTimer_, true, params);
    }

    // 最後に被弾の反応を加え、現在姿勢から目標へ補間する。
    ApplyDamageReactionPose(targetScale, targetRotation, nullptr, 0.82f);
    const float poseRate = (std::min)(1.0f, deltaTime * 8.0f);
    SetScale(engine::Math::Lerp(GetScale(), targetScale, poseRate));
    const engine::Vector3 currentRotation = GetRotation();
    SetRotation({currentRotation.x + (targetRotation.x - currentRotation.x) * poseRate, currentRotation.y,
                 currentRotation.z + (targetRotation.z - currentRotation.z) * poseRate});
}

void EnemyGiantSlime::SyncWorldCollisionRadius(float worldRadius, float worldCenterY) {
    // 表示スケールが変化しても判定のワールド半径が一定になるよう逆算する。
    const engine::Vector3 scale = GetScale();
    const float maxScale = (std::max)({0.001f, std::abs(scale.x), std::abs(scale.y), std::abs(scale.z)});
    const float yScale = (std::max)(0.001f, std::abs(scale.y));
    engine::ColliderConfig config = GetColliderConfig();
    // 中心の高さにはY軸倍率、球の半径には最大軸倍率が掛かるため、それぞれ別の倍率で割り戻します。
    config.center.y = worldCenterY / yScale;
    SetColliderConfig(config);
    SetCollisionRadius(worldRadius / maxScale);
}

void EnemyGiantSlime::SyncGroundCollisionRadius() {
    // 接地中はプレイヤーと接触する通常の胴体範囲にする。
    SyncWorldCollisionRadius(kGroundCollisionWorldRadius, kGroundCollisionWorldCenterY);
}

void EnemyGiantSlime::SyncThrownCollisionRadius() {
    // 投げられた胴体は横方向へ広がるため、専用の半径と中心を使う。
    SyncWorldCollisionRadius(kThrownCollisionWorldRadius, kThrownCollisionWorldCenterY);
}

void EnemyGiantSlime::SplitIntoSmallSlimes(engine::ParticleSystem* particleSystem) {
    // 分裂は一度だけ確定させ、子スライムを周囲に生成して親を退場させる。
    if (hasSplit_)
        return;
    hasSplit_ = true;
    isHookSplitPulled_ = false;
    hookSplitAwaitingLanding_ = false;
    HideAttackTelegraph();
    hookSplitWeakPoint_ = false;
    state_ = State::kIdle;

    engine::Vector3 splitCenter = GetTranslate();

    // CPU粒子を使える場合は、子の生成前に中心から分裂の表示を出す。
    if (particleSystem) {
        engine::Vector3 up = {0.0f, 1.0f, 0.0f};
        particleSystem->SpawnParticles(splitCenter, 48, 2.0f, &up, 34.0f, {0.35f, 0.85f, 1.0f, 1.0f},
                                       {0.35f, 0.85f, 1.0f, 0.0f}, 0.25f, 0.65f, 0.85f, 0.08f);
    }

    BaseScene* scene = SceneManager::GetInstance() ? SceneManager::GetInstance()->GetCurrentScene() : nullptr;

    // 子の実体は現在のシーンに生成し、分裂点を中心に等間隔で外へ飛ばします。
    if (scene && scene->GetObject3DCommon()) {
        for (std::int32_t i = 0; i < kSplitSlimeCount; ++i) {
            auto slime = EnemyFactory::GetInstance()->CreateEnemy("Slime", scene->GetObject3DCommon());
            // 子を一体生成できない場合はその角度だけ省き、残る子の作成を試す。
            if (!slime)
                continue;

            const float angle = ((2.0f * std::numbers::pi_v<float>) / static_cast<float>(kSplitSlimeCount)) * static_cast<float>(i);
            engine::Vector3 dir = {std::sin(angle), 0.0f, std::cos(angle)};
            engine::Vector3 pos = splitCenter + dir * kSplitSlimeRadius;
            pos.y += 0.45f;

            const float yaw = std::atan2(dir.x, dir.z) + kGiantSlimeModelYawOffset;
            slime->SetTranslate(pos);
            slime->SetRotation({0.0f, yaw, 0.0f});
            slime->SetScale({1.35f, 1.35f, 1.35f});
            slime->SetName(GetName() + "_SplitSlime_" + std::to_string(i));
            slime->SetTarget(GetTarget());
            slime->SetDetectionRange(18.0f);
            slime->SetVelocity({dir.x * 4.0f, 4.0f, dir.z * 4.0f});

            // 隔離プレビューで生成する場合は、子も編集用としてロックし、通常の衝突を無効にします。
            if (IsEditorInternal()) {
                slime->SetEditorInternal(true);
                slime->SetIsLocked(true);
                slime->SetCollisionAttribute(0);
                slime->SetCollisionMask(0);
            }

            // 子に変更可能なパラメーターがあれば、分裂個体用のHP・移動値を適用する。
            if (slime->GetParameters().has_value()) {
                slime->SetParameter(&engine::EntityParameter::hp, 25.0f);
                slime->SetParameter(&engine::EntityParameter::maxHp, 25.0f);
                slime->SetParameter(&engine::EntityParameter::speed, 0.22f);
                slime->SetParameter(&engine::EntityParameter::jumpPower, 5.0f);
                slime->SetParameter(&engine::EntityParameter::detectionRange, 18.0f);
            }

            scene->AddObject(std::move(slime));
        }
    }

    // 子を登録した後で元の大型個体の表示と接触を止め、シーンに取り除くよう要求します。
    SetCollisionAttribute(0);
    SetCollisionMask(0);
    SetIsVisible(false);
    SetDead(true);

    // 現在のシーンを取得できる場合は、表示と衝突を止めた親の実体を削除予約する。
    if (BaseScene* scene = SceneManager::GetInstance() ? SceneManager::GetInstance()->GetCurrentScene() : nullptr) {
        scene->RequestRemoveObject(this);
    }

    DebugConsole::GetInstance()->AddLog("GiantSlime split into small slimes!");
}
