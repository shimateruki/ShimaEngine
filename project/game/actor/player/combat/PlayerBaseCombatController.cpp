#define NOMINMAX
#include <cstdint>
#include "game/actor/player/combat/PlayerBaseCombatController.h"

#include "BaseEnemy.h"
#include "CollisionConfig.h"
#include "CollisionManager.h"
#include "game/system/rules/EventManager.h"
#include "GpuParticleManager.h"
#include "MeshEffectManager.h"
#include "game/actor/player/Player.h"
#include "VfxSequencer.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kBashWindupDuration = 0.07f;
constexpr float kBashActiveDuration = 0.21f;
constexpr float kBashRecoveryDuration = 0.16f;
constexpr float kPressWindupDuration = 0.09f;
constexpr float kPressMaximumFallDuration = 0.95f;
constexpr float kPressRecoveryDuration = 0.17f;
constexpr float kAttackInputBufferDuration = 0.18f;

engine::Vector3 NormalizePlanar(engine::Vector3 value) {
    // 上下成分を除き、攻撃に使う水平方向の長さを求める。
    value.y = 0.0f;
    const float length = std::sqrt(value.x * value.x + value.z * value.z);

    // 水平成分が短く単位化できない場合は、ワールドの正のZを使う。
    if (length <= 0.001f) {
        return {0.0f, 0.0f, 1.0f};
    }

    return {value.x / length, 0.0f, value.z / length};
}

engine::Object3D* FindEnemyRoot(engine::Object3D* object) {
    // 子Colliderで当たった場合も敵本体へまとめ、多重被弾を避ける。
    for (engine::Object3D* current = object; current; current = current->GetParent()) {
        // 最初にBaseEnemyへ到達した時点で、その実体をダメージ対象にする。
        if (dynamic_cast<BaseEnemy*>(current)) {
            return current;
        }
    }

    return nullptr;
}

// 指定したワールド位置、回転、倍率でメッシュエフェクトを発生させます。
void SpawnEffect(const char* path, const engine::Vector3& position, const engine::Vector3& rotation = {},
                 const engine::Vector3& scale = {1.0f, 1.0f, 1.0f}) {
    // メッシュエフェクト管理が取得できた場合だけ、出現要求を送る。
    if (MeshEffectManager* effects = MeshEffectManager::GetInstance()) {
        effects->SpawnEffectAt(path, position, rotation, scale);
    }
}

// 指定した方向を使う粒子をワールド位置に発生させます。
void EmitDirectedPreset(const char* preset, const engine::Vector3& position, const engine::Vector3& direction, float scale = 1.0f) {
    GpuParticleManager* particles = GpuParticleManager::GetInstance();

    // GPU粒子の管理を取得でき、初期化済みの場合だけ粒子を出す。
    if (particles && particles->IsInitialized()) {
        particles->EmitDirected(preset, position, direction, scale);
    }
}

void DispatchDamage(Player& player, engine::Object3D* target, float damage, const engine::Vector3& knockback) {
    // 直接HPを書き換えず、共通のDamageEvent経由で耐性や被弾処理へ渡す。
    DamageEvent event;
    event.target = target;
    event.attacker = &player;
    event.damageAmount = damage;
    event.damageType = DamageType::kPhysical;
    event.knockbackVelocity = knockback;
    EventManager::GetInstance()->Dispatch(event);
}
} // namespace

// 通常形態の攻撃状態を管理します。入力受理、攻撃判定、終了時の操作復帰を別の段階として扱います。

void PlayerBaseCombatController::ProcessInput(Player& player, bool attackTriggered) {
    // 攻撃の押下が無い、死亡、演出拘束、コピー形態、または敵の運搬中なら通常攻撃を受け付けない。
    if (!attackTriggered || player.IsDead() || player.IsCinematicLocked() || player.IsEnemyMorphed() ||
        player.GetCarriedEnemy()) {
        return;
    }

    // 実行中の技がある間は別の技を開始せず、押下を0.18秒だけ予約する。
    if (IsActive()) {
        inputBufferTimer_ = kAttackInputBufferDuration;
        return;
    }

    // 技を実行しておらず接地中なら、地上の体当たりを始める。
    if (player.IsGrounded()) {
        BeginBash(player);
    // 技を実行しておらず空中なら、踏みつけを始める。
    } else {
        BeginPress(player);
    }
}

void PlayerBaseCombatController::Update(Player& player, float deltaTime) {
    // 進行中の技が無いか経過秒数が正でなければ、攻撃を進めない。
    if (!IsActive() || deltaTime <= 0.0f) {
        return;
    }

    // 死亡・演出拘束・コピー状態へ移った場合は、操作を戻さずに通常攻撃を中断する。
    if (player.IsDead() || player.IsCinematicLocked() || player.IsEnemyMorphed()) {
        Cancel(player, false);
        return;
    }

    // 技の進行より先に入力猶予を減らす。同じフレームで猶予が切れた追加入力は、技の終了時に実行しない。
    inputBufferTimer_ = std::max(0.0f, inputBufferTimer_ - deltaTime);

    // 現在の段階に応じて、体当たりと踏みつけのどちらの時計を進めるか選ぶ。
    switch (phase_) {
    // 体当たりの各段階では、突進と命中を管理するUpdateBash()へ渡す。
    case Phase::kBashWindup:
    case Phase::kBashActive:
    case Phase::kBashRecovery:
        UpdateBash(player, deltaTime);
        break;

    // 踏みつけの各段階では、落下と着地を管理するUpdatePress()へ渡す。
    case Phase::kPressWindup:
    case Phase::kPressFall:
    case Phase::kPressRecovery:
        UpdatePress(player, deltaTime);
        break;

    // 待機または範囲外の値では、攻撃の更新を呼ばない。
    case Phase::kIdle:
    default:
        break;
    }
}

void PlayerBaseCombatController::Cancel(Player& player, bool restoreControl) {
    // 進行中の技が無ければ、解除すべき通常攻撃のモーションがない。
    if (!IsActive()) {
        return;
    }

    player.StopSlimeAbilityMotion(0.05f);
    player.SetSlimeAbilityPose(PlayerSlimeAnimator::AbilityPose::kNone);

    // 呼び出し側が操作の復元を指定し、死亡や演出拘束も無い場合に限り操作を再開する。
    if (restoreControl && !player.IsDead() && !player.IsCinematicLocked()) {
        player.SetIsControlActive(true);
    }

    // 攻撃段階と入力予約、命中済みの記録を消す。
    phase_ = Phase::kIdle;
    timer_ = 0.0f;
    effectTimer_ = 0.0f;
    inputBufferTimer_ = 0.0f;
    pressHitEnemy_ = false;
    hitTargets_.clear();
}

void PlayerBaseCombatController::BeginBash(Player& player) {
    // ロックオン方向を攻撃開始時に固定し、予備動作中の入力で判定が跳ねないようにする。
    direction_ = NormalizePlanar(player.GetLockOnAttackDirection());
    phase_ = Phase::kBashWindup;
    timer_ = kBashWindupDuration;
    effectTimer_ = 0.0f;
    hitTargets_.clear();
    // 予備動作中は通常入力と水平移動を止め、鉛直速度を保ったまま固定した攻撃方向を向く。
    player.SetIsControlActive(false);
    player.SetMoveYaw(std::atan2(direction_.x, direction_.z));
    player.SetVelocity({0.0f, player.GetVelocity().y, 0.0f});
    player.PlaySlimeAbilityMotion("player_base_bash", false, 1.0f, 0.025f, 0.06f);
    player.TriggerSlimeImpulse({1.12f, 0.86f, 0.94f}, 0.07f);
}

void PlayerBaseCombatController::BeginPress(Player& player) {
    // 踏みつけ開始時の方向と各ヒット状態を初期化する。
    direction_ = NormalizePlanar(player.GetLockOnAttackDirection());
    phase_ = Phase::kPressWindup;
    timer_ = kPressWindupDuration;
    effectTimer_ = 0.0f;
    pressHitEnemy_ = false;
    hitTargets_.clear();
    // 通常入力を止めて水平速度を落とし、踏みつけに移る前の上向き速度を最低2.5にする。
    player.SetIsControlActive(false);
    player.SetVelocity(
        {player.GetVelocity().x * 0.35f, std::max(player.GetVelocity().y, 2.5f), player.GetVelocity().z * 0.35f});
    player.PlaySlimeAbilityMotion("player_base_press", false, 1.0f, 0.025f, 0.06f);
    player.TriggerSlimeImpulse({0.72f, 1.55f, 0.72f}, 0.11f);
}

void PlayerBaseCombatController::UpdateBash(Player& player, float deltaTime) {
    // 予備動作と攻撃中の時計を分け、判定が有効な区間だけ敵へ当てる。
    timer_ -= deltaTime;
    player.SetIsControlActive(false);
    player.SetMoveYaw(std::atan2(direction_.x, direction_.z));

    // 予備動作中は横移動を止め、満了したフレームで突進速度と開始演出を与えます。
    if (phase_ == Phase::kBashWindup) {
        player.SetVelocity({0.0f, player.GetVelocity().y, 0.0f});

        // 予備動作が終わった場合は突進区間へ進み、初速と軌跡の開始演出を反映する。
        if (timer_ <= 0.0f) {
            phase_ = Phase::kBashActive;
            timer_ = kBashActiveDuration;
            player.SetVelocity({direction_.x * 22.0f, std::max(player.GetVelocity().y, 0.0f), direction_.z * 22.0f});
            player.TriggerSlimeImpulse({0.84f, 1.12f, 1.92f}, 0.14f);
            SpawnEffect("Resources/json/effect/player/base/effect_player_base_bash_arc.json",
                        player.GetWorldPosition() + direction_ * 0.7f + engine::Vector3{0.0f, 0.58f, 0.0f},
                        {0.0f, std::atan2(direction_.x, direction_.z), 0.0f}, {0.82f, 0.82f, 1.10f});
            EmitDirectedPreset("player/base/player_base_bash_droplets",
                               player.GetWorldPosition() - direction_ * 0.34f + engine::Vector3{0.0f, 0.34f, 0.0f},
                               direction_ * -1.0f, 0.78f);
            effectTimer_ = 0.08f;
        }

        return;
    }

    // 突進中は時間に応じて減速しながら、前方への命中判定を毎フレーム行います。
    if (phase_ == Phase::kBashActive) {
        engine::Vector3 velocity = player.GetVelocity();
        velocity.x = direction_.x * std::max(8.0f, 22.0f * (timer_ / kBashActiveDuration));
        velocity.z = direction_.z * std::max(8.0f, 22.0f * (timer_ / kBashActiveDuration));
        player.SetVelocity(velocity);
        player.ForceSlimeAnimationModeForNextUpdate(PlayerSlimeAnimator::Mode::kDash, direction_);
        DamageBash(player);
        // 突進中のしずくは命中とは別の時計で間引き、移動の軌跡を見せます。
        effectTimer_ -= deltaTime;

        // 軌跡粒子の発生間隔を使い切った場合だけ、次のしずくを出す。
        if (effectTimer_ <= 0.0f) {
            EmitDirectedPreset("player/base/player_base_bash_droplets",
                               player.GetWorldPosition() - direction_ * 0.38f + engine::Vector3{0.0f, 0.30f, 0.0f},
                               direction_ * -1.0f, 0.46f);
            effectTimer_ = 0.085f;
        }

        // 突進が終わったら前進の勢いを減らし、判定を出さない復帰時間に切り替えます。
        if (timer_ <= 0.0f) {
            phase_ = Phase::kBashRecovery;
            timer_ = kBashRecoveryDuration;
            velocity.x *= 0.20f;
            velocity.z *= 0.20f;
            player.SetVelocity(velocity);
        }

        return;
    }

    // 復帰時間の満了後に、操作と次の入力処理を Finish() に渡します。
    if (timer_ <= 0.0f) {
        Finish(player);
    }
}

void PlayerBaseCombatController::UpdatePress(Player& player, float deltaTime) {
    // 落下から着地へ移る時計で、空中判定と着地衝撃を切り替える。
    timer_ -= deltaTime;
    player.SetIsControlActive(false);

    // 踏みつけの予備動作中は、その残り時間の終了を待つ。
    if (phase_ == Phase::kPressWindup) {
        // 予備動作が終わったら下向きの速度を設定し、落下時間の上限を計り始める。
        if (timer_ <= 0.0f) {
            phase_ = Phase::kPressFall;
            timer_ = kPressMaximumFallDuration;
            player.SetVelocity({direction_.x * 3.0f, -25.0f, direction_.z * 3.0f});
            player.PlaySlimeAbilityMotion("player_base_press_fall", true, 1.0f, 0.025f, 0.04f);
        }

        return;
    }

    // 下向きの速度を保ち、着地処理より先に直接踏んだ敵を判定します。
    if (phase_ == Phase::kPressFall) {
        engine::Vector3 velocity = player.GetVelocity();
        velocity.y = std::min(velocity.y, -25.0f);
        player.SetVelocity(velocity);
        player.ForceSlimeAnimationModeForNextUpdate(PlayerSlimeAnimator::Mode::kJump, direction_);
        player.SetSlimeAbilityPose(PlayerSlimeAnimator::AbilityPose::kNone);
        DamagePress(player, false);

        // 落下中に敵へ直接当たった場合は、着地判定より先に高く跳ね返って復帰へ移る。
        if (pressHitEnemy_) {
            phase_ = Phase::kPressRecovery;
            timer_ = kPressRecoveryDuration;
            player.SetVelocity({direction_.x * 2.0f, 14.0f, direction_.z * 2.0f});
            player.PlaySlimeAbilityMotion("player_base_press_land", false, 1.0f, 0.02f, 0.05f);
            player.TriggerSlimeImpulse({1.55f, 0.68f, 1.55f}, 0.14f);
        // 直接命中せず、接地したか落下の時間上限に達した場合は、広い着地衝撃を出す。
        } else if (player.IsGrounded() || timer_ <= 0.0f) {
            DamagePress(player, true);
            VfxSequencer::PlayOneShot("player/base/player_base_press_land_cue",
                                      player.GetWorldPosition() + engine::Vector3{0.0f, 0.06f, 0.0f});
            phase_ = Phase::kPressRecovery;
            timer_ = kPressRecoveryDuration;
            player.SetVelocity({0.0f, 4.0f, 0.0f});
            player.PlaySlimeAbilityMotion("player_base_press_land", false, 1.0f, 0.02f, 0.05f);
            player.TriggerSlimeImpulse({1.85f, 0.52f, 1.85f}, 0.16f);
        }

        return;
    }

    // 復帰時間が尽きたら、入力猶予を確認する Finish() へ渡します。
    if (timer_ <= 0.0f) {
        Finish(player);
    }
}

void PlayerBaseCombatController::DamageBash(Player& player) {
    // 前方の球状領域で敵を探し、一回の振りで同じ敵を重複して傷つけない。
    engine::PhysicsQueryFilter filter;
    filter.mask = engine::collisionCategories::Get("enemy");
    filter.ignoredObject = &player;
    const engine::Vector3 center = player.GetWorldPosition() + direction_ * 1.0f + engine::Vector3{0.0f, 0.62f, 0.0f};

    for (const engine::PhysicsOverlapHit& hit : engine::CollisionManager::GetInstance()->OverlapSphere(center, 1.38f, filter)) {
        engine::Object3D* target = FindEnemyRoot(hit.object);

        // 敵本体を解決できないか、この技で既に命中した場合はダメージを送らない。
        if (!target || !hitTargets_.insert(target).second) {
            continue;
        }

        DispatchDamage(player, target, 9.0f, {direction_.x * 10.0f, 4.5f, direction_.z * 10.0f});
        VfxSequencer::PlayOneShot("player/base/player_base_bash_hit_cue", target->GetWorldPosition() + engine::Vector3{0.0f, 0.55f, 0.0f});
    }
}

void PlayerBaseCombatController::DamagePress(Player& player, bool landingImpact) {
    // 落下中と着地時で判定中心と半径を変える。
    engine::PhysicsQueryFilter filter;
    filter.mask = engine::collisionCategories::Get("enemy");
    filter.ignoredObject = &player;
    const engine::Vector3 center = player.GetWorldPosition() + engine::Vector3{0.0f, landingImpact ? 0.32f : -0.10f, 0.0f};
    const float radius = landingImpact ? 2.35f : 1.48f;

    for (const engine::PhysicsOverlapHit& hit : engine::CollisionManager::GetInstance()->OverlapSphere(center, radius, filter)) {
        engine::Object3D* target = FindEnemyRoot(hit.object);

        // 敵本体が無いか、直接命中と着地衝撃のいずれかで既に当てた相手は飛ばす。
        if (!target || !hitTargets_.insert(target).second) {
            continue;
        }

        // 判定中心から外向きに押し出す。直接踏んだ場合は着地衝撃より強いダメージと上向き速度を使う。
        engine::Vector3 knockback = NormalizePlanar(target->GetWorldPosition() - center);
        DispatchDamage(player, target, landingImpact ? 8.0f : 12.0f,
                       {knockback.x * 7.5f, landingImpact ? 7.0f : 10.0f, knockback.z * 7.5f});
        VfxSequencer::PlayOneShot("player/base/player_base_press_enemy_hit_cue",
                                  target->GetWorldPosition() + engine::Vector3{0.0f, 0.45f, 0.0f},
                                  landingImpact ? engine::Vector3{1.15f, 1.15f, 1.15f} : engine::Vector3{0.86f, 0.86f, 0.86f});
        // 直接命中だけを、跳ね返りへ切り替える印として保持する。
        pressHitEnemy_ = !landingImpact;
    }
}

void PlayerBaseCombatController::Finish(Player& player) {
    // 入力猶予中の追加入力は、通常操作へ戻した後に次の攻撃へつなぐ。
    const bool consumeBufferedAttack = inputBufferTimer_ > 0.0f && !player.IsDead() && !player.IsCinematicLocked() &&
                                       !player.IsEnemyMorphed() && !player.GetCarriedEnemy();

    player.SetSlimeAbilityPose(PlayerSlimeAnimator::AbilityPose::kNone);
    player.StopSlimeAbilityMotion(0.06f);

    // 死亡も演出拘束も無い場合だけ、攻撃が止めていた通常操作を戻す。
    if (!player.IsDead() && !player.IsCinematicLocked()) {
        player.SetIsControlActive(true);
    }

    // 今の攻撃の命中履歴とタイマーを先に消し、次の攻撃に持ち越さないようにします。
    phase_ = Phase::kIdle;
    timer_ = 0.0f;
    effectTimer_ = 0.0f;
    inputBufferTimer_ = 0.0f;
    pressHitEnemy_ = false;
    hitTargets_.clear();

    // 予約が有効で今も通常攻撃を開始できる場合だけ、後続の攻撃を始める。
    if (consumeBufferedAttack) {
        // 接地していれば後続は体当たりにする。
        if (player.IsGrounded()) {
            BeginBash(player);
        // 空中なら後続は踏みつけにする。
        } else {
            BeginPress(player);
        }
    }
}

bool PlayerBaseCombatController::CanBreakImpactGate() const {
    // 実際に勢いが乗っている突進と落下区間だけゲート破壊を許す。
    return phase_ == Phase::kBashActive || phase_ == Phase::kPressFall;
}

PlayerBaseCombatController::ReplayState PlayerBaseCombatController::CaptureReplayState() const {
    // 攻撃位相・方向・入力猶予を記録し、復元後も同じタイミングから進める。
    ReplayState state;
    state.phase = static_cast<std::int32_t>(phase_);
    state.direction = direction_;
    state.timer = timer_;
    state.effectTimer = effectTimer_;
    state.inputBufferTimer = inputBufferTimer_;
    state.pressHitEnemy = pressHitEnemy_;
    return state;
}

// 記録された攻撃段階・方向・時計を現在の状態へ戻します。
void PlayerBaseCombatController::RestoreReplayState(const ReplayState& state) {
    // 段階番号を列挙の範囲へ収め、方向は水平の単位方向に戻す。
    phase_ = static_cast<Phase>(
        std::clamp(state.phase, static_cast<std::int32_t>(Phase::kIdle), static_cast<std::int32_t>(Phase::kPressRecovery)));
    direction_ = NormalizePlanar(state.direction);
    timer_ = std::max(0.0f, state.timer);
    effectTimer_ = state.effectTimer;
    inputBufferTimer_ = std::max(0.0f, state.inputBufferTimer);
    pressHitEnemy_ = state.pressHitEnemy;

    // リプレイ復元後に既に命中済みの実体参照を持ち越さないようにします。
    hitTargets_.clear();
}
