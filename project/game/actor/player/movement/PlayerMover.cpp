#include "game/actor/player/movement/PlayerMover.h"
#include "game/actor/player/Player.h"
#include "InputManager.h"
#include "ParticleSystem.h"
#include "IMoveStrategy.h"
#include "MoveStrategy3D.h"
#include "Object3D.h"
#include <algorithm>
#include <cmath>
#include <game/actor/player/state/PlayerState.h>
#include "game/system/rules/EventManager.h"
#include "DebugConsole.h"
#include "GpuParticleManager.h"
#include "game/system/combat/GroundEffectLocator.h"
#include "MeshEffectManager.h"
#include "game/system/rules/GameplayStatusManager.h"
#include "engine/utility/math/AnimationInterpolation.h"

// ジャンプ入力は Player が SampleJumpInput() に先に渡す。Update() はその予約と接地状態を使って速度を決める。

PlayerMover::PlayerMover() = default;

PlayerMover::~PlayerMover() {
    // プレイヤーを関連付けている場合は、破棄時にも子の衝突属性を復元する。
    if (player_) {
        for (auto& kv : childOriginalAttributes_) {
            engine::Object3D* child = kv.first;
            uint32_t attr = kv.second;
            // 保存した子の参照が有効な場合だけ、衝突属性を元へ戻す。
            if (child)
                child->SetCollisionAttribute(attr);
        }

        childOriginalAttributes_.clear();
    }
}

void PlayerMover::StopDashOnImpact() {
    // ダッシュ中に限り、残り時間・無敵・子の衝突属性を終了状態へ戻す。
    if (!isDashing_) {
        return;
    }

    isDashing_ = false;
    dashTimer_ = 0.0f;

    // プレイヤーが設定済みなら、ダッシュ中だけの無敵を解除する。
    if (player_) {
        player_->SetDashInvincible(false);
    }

    for (auto& kv : childOriginalAttributes_) {
        engine::Object3D* child = kv.first;

        // 復元先の子がある場合だけ、ダッシュ前の衝突属性を書き戻す。
        if (child) {
            child->SetCollisionAttribute(kv.second);
        }
    }

    childOriginalAttributes_.clear();
}

void PlayerMover::ResetTransientState() {
    // 乗っていた床や能力の速度を破棄し、次の移動更新では通常の入力速度から計算します。
    platformVelocity_ = {};
    platformAirVelocity_ = {};
    abilityAirVelocity_ = {};
    previousAddedAirVelocity_ = {};
    jumpRequest_.Reset();
    jumpStatus_ = "リセット";
    // ダッシュ終了に付随する無敵と衝突属性を戻してから、移動の予約や方向を初期化します。
    StopDashOnImpact();
    isJumpCharging_ = false;
    jumpChargeTimer_ = 0.0f;
    hasAirDashed_ = false;
    dashDirection_ = {};
    currentDashSpeed_ = dashSpeed_;
    currentDashDuration_ = dashDuration_;

    // 着地履歴とパネルの効果を消し、転送先で前の落下を着地として扱わないようにします。
    CancelDashPanelBoost();
    hasGroundedHistory_ = false;
    wasGroundedLastFrame_ = false;
    airborneDuration_ = 0.0f;
    airborneMaxDownSpeed_ = 0.0f;
    landingDustCooldown_ = 0.0f;

    // プレイヤーが設定済みなら、見た目の溜め量とダッシュ無敵も消す。
    if (player_) {
        player_->SetSlimeJumpCharge(0.0f);
        player_->SetDashInvincible(false);
    }
}

// プレイヤーと入力・演出の参照を受け取り、標準の3D移動戦略を用意します。
void PlayerMover::Initialize(Player* player, engine::InputManager* inputManager, engine::ParticleSystem* particleSystem) {
    player_ = player;
    inputManager_ = inputManager;
    particleSystem_ = particleSystem;

    strategy_ = std::make_unique<MoveStrategy3D>();

    baseScale_ = {2.0f, 2.0f, 2.0f};
    // プレイヤーが設定されている場合だけ、基準のスケールを反映する。
    if (player_)
        player_->SetScale(baseScale_);
}

void PlayerMover::SampleJumpInput(float deltaTime, bool discard) {
    // 操作停止中も予約の寿命を管理し、無効な場面の入力だけを明示的に破棄する。
    // プレイヤーまたは入力管理が未準備なら、ジャンプ要求を採取できない。
    if (!player_ || !inputManager_)
        return;
    // 呼び出し側が入力破棄を指示した場合は、接地を調べる前に予約と溜めを消す。
    if (discard) {
        jumpRequest_.Reset();
        isJumpCharging_ = false;
        jumpChargeTimer_ = 0.0f;
        jumpStatus_ = "死亡・演出・コピー切替中：予約破棄";
        return;
    }

    // 計測時間が有限かつ正の場合だけ、予約の期限や溜めの進行を更新する。
    if (deltaTime <= 0.0f || !std::isfinite(deltaTime))
        return;
    // 現時点の入力、上昇中でない接地、ピンク形態かどうかを要求管理へ渡す。
    const bool hadRequest = jumpRequest_.BufferRemaining() > 0.0f;
    jumpRequest_.Advance(deltaTime, inputManager_->IsActionPressed("Jump"), inputManager_->IsActionTriggered("Jump"),
                         player_->IsGrounded() && player_->GetVelocity().y <= 0.0f, player_->IsPinkSlimeMorphed());
    jumpStatus_ =
        jumpRequest_.IsCharging()
            ? "溜め中"
            : (jumpRequest_.BufferRemaining() > 0.0f ? "入力予約：接地または操作復帰待ち" : "入力待ち／予約期限切れ");
#ifdef USE_IMGUI
    // ジャンプ入力が押されたフレームだけ、接地と操作許可の状態を記録する。
    if (inputManager_->IsActionTriggered("Jump")) {
        DebugConsole::GetInstance()->AddLog(std::string("[Jump] 入力受付 / ") +
                                            (player_->IsGrounded() ? "接地" : "空中") +
                                            (player_->IsControlActive() ? " / 操作有効" : " / 操作制限中"));
    }

    // 進める前は予約があり、更新後に残り時間がなくなった場合は期限切れを記録する。
    if (hadRequest && jumpRequest_.BufferRemaining() <= 0.0f) {
        DebugConsole::GetInstance()->AddLog("[Jump] 予約期限切れ：接地または操作復帰が間に合いませんでした");
    }
#endif
}

engine::Vector3 PlayerMover::GetMoveInputDirection() {
    // 速度を適用せず、現在の移動戦略と同じカメラ基準の向きだけを攻撃側へ渡す。
    // 対象または移動戦略がない場合は、移動方向なしを返す。
    if (!player_ || !strategy_) {
        return {};
    }

    engine::Vector3 input = strategy_->CalculateVelocity(player_);
    input.y = 0.0f;
    const float length = std::sqrt(input.x * input.x + input.z * input.z);
    return length > 0.001f ? input / length : engine::Vector3{};
}

// 水平移動とジャンプ要求を確定します。重力・衝突を含む Character の更新は呼び出し側で行います。
void PlayerMover::Update(float deltaTime) {
    // 処理に必要な参照が未準備か、経過秒数が有限かつ正でなければ速度を更新しない。
    if (!player_ || !inputManager_ || !strategy_ || !std::isfinite(deltaTime) || deltaTime <= 0.0f)
        return;

    // HPを参照でき、その値が0以下なら、落下速度を保って水平移動だけ止める。
    if (player_->GetParameters().has_value() && player_->GetParameters()->hp <= 0.0f) {
        engine::Vector3 v = player_->GetVelocity();
        player_->SetVelocity({0.0f, v.y, 0.0f});
        return;
    }

    // 物理で確定済みの上下速度を土台に、入力・攻撃・ダッシュの水平移動を組み立てる。
    engine::Vector3 velocity = player_->GetVelocity();

    const auto attackMovement = player_->GetAttackMovement();
    const engine::Vector3 inputMove = strategy_->CalculateVelocity(player_);

    // 解除されるまでパネルの移動・旋回倍率を通常移動へ渡す。
    const bool isDashPanelActive = IsDashPanelBoostActive();
    const float moveSpeedMultiplier = isDashPanelActive ? dashPanelSpeedMultiplier_ : 1.0f;
    const float turnMultiplier = isDashPanelActive ? dashPanelTurnMultiplier_ : 1.0f;

    // 攻撃の強い硬直へ入ったら通常ダッシュの押し出しを終了し、下の攻撃制限を適用する。
    if (attackMovement.blocksMovementActions && isDashing_)
        StopDashOnImpact();

    // パネル中は入力から速度の大きさを作らず、踏んだ向きの前方だけ操舵する。
    if (isDashPanelActive) {
        constexpr float kMaxSteeringRadians = 0.52f;
        constexpr float kFullTurnRadians = 6.28318530718f;
        if (std::hypot(inputMove.x, inputMove.z) > 0.001f) {
            const float targetYaw = std::atan2(inputMove.x, inputMove.z);
            const float relativeYaw = std::remainder(targetYaw - dashPanelEntryYaw_, kFullTurnRadians);
            // 逆入力では現在の進行を保つ。横入力は最大で約30度まで寄せる。
            if (std::abs(relativeYaw) <= kFullTurnRadians * 0.25f) {
                const float steerYaw = dashPanelEntryYaw_ + std::clamp(relativeYaw, -kMaxSteeringRadians, kMaxSteeringRadians);
                dashPanelYaw_ = animationInterpolation::DampAngle(dashPanelYaw_, steerYaw, 6.5f * turnMultiplier, deltaTime);
            }
        }
        const float speed = player_->GetMoveSpeed() * moveSpeedMultiplier;
        velocity.x = std::sin(dashPanelYaw_) * speed;
        velocity.z = std::cos(dashPanelYaw_) * speed;
    // 通常ダッシュは記録済みの方向へ進み、残り時間の割合の二乗で減速する。
    } else if (isDashing_) {
        float tRatio = dashTimer_ / currentDashDuration_;
        float currentSpeed = currentDashSpeed_ * (tRatio * tRatio);

        velocity.x = dashDirection_.x * currentSpeed;
        velocity.z = dashDirection_.z * currentSpeed;

        dashTimer_ -= deltaTime;

        // ダッシュ速度の算出後、残り秒数が0以下なら無敵と衝突属性を戻す。
        if (dashTimer_ <= 0.0f) {
            StopDashOnImpact();
        }
    // 通常移動では、前回加えた外力を除いてから水平速度を組み立てる。
    } else {
        // 前回加えた床・爆風の速度を除き、通常の加速へ外力を二重に持ち込まない。
        {
            auto removeExternal = [](float current, float added) {
                // 壁で外力ごと停止した場合、差し引きで逆向きの速度を作らない。
                return current * added >= 0.0f && std::abs(current) <= std::abs(added) ? 0.0f : current - added;
            };
            velocity.x = removeExternal(velocity.x, previousAddedAirVelocity_.x);
            velocity.z = removeExternal(velocity.z, previousAddedAirVelocity_.z);
        }
        // 攻撃側が移動制御を有効にしていれば、その地上制限と空中補間を使う。
        if (attackMovement.enabled) {
            velocity = attackMovement.Apply(velocity, inputMove * moveSpeedMultiplier, player_->IsGrounded(), deltaTime);
        // 攻撃による制御が無ければ、地上・空中の通常加減速を使う。
        } else {
            const auto& movement = GameplayStatusManager::GetInstance()->GetPlayerLocomotion();
            velocity = movement.Apply(velocity, inputMove * moveSpeedMultiplier,
                                      player_->GetMoveSpeed() * moveSpeedMultiplier, player_->IsGrounded(), deltaTime);
        }
    }

    // 有効なロックオン照準がある場合は、横移動や後退中も照準方向へ向き直す。
    if (player_->HasLockOnAimPoint()) {
        const engine::Vector3 facing = player_->GetLockOnAttackDirection();
        const float targetAngle = std::atan2(facing.x, facing.z);
        const float currentY = player_->GetMoveYaw();
        player_->SetMoveYaw(animationInterpolation::DampAngle(currentY, targetAngle, 18.0f, deltaTime));
    // ロックオンが無く水平速度が十分なら、ダッシュパネルの旋回倍率込みで進行方向へ向く。
    } else if (std::abs(velocity.x) > 0.001f || std::abs(velocity.z) > 0.001f) {
        const float targetAngle = std::atan2(velocity.x, velocity.z);
        const float currentY = player_->GetMoveYaw();
        const float turnResponse = 6.5f * turnMultiplier;
        player_->SetMoveYaw(animationInterpolation::DampAngle(currentY, targetAngle, turnResponse, deltaTime));
    }

    bool isGrounded = player_->IsGrounded();

    // 接地している場合は足場の空中慣性を消し、空中ダッシュを再び使えるようにする。
    if (isGrounded) {
        platformAirVelocity_ = {};

        // 離陸直後は接地情報が一フレーム残るため、上昇速度がある間は爆風を消さない。
        if (velocity.y <= 0.0f)
            abilityAirVelocity_ = {};
        hasAirDashed_ = false;
    }

    // 入力側で判定済みの溜め状態を読み、ピンク形態では溜め量も見た目に反映する。
    const bool usePinkSlimeJumpAbility = player_->IsPinkSlimeMorphed();
    isJumpCharging_ = jumpRequest_.IsCharging();
    jumpChargeTimer_ = jumpRequest_.Charge();
    player_->SetSlimeJumpCharge(jumpChargeTimer_);

    // 溜め中は空中用の変形をアニメーターに指定する。
    if (isJumpCharging_) {
        player_->SetSlimeAnimationMode(PlayerSlimeAnimator::Mode::kJump);

        // ダッシュ中でなければ、溜め中の水平速度を2割に落とす。
        if (!isDashing_ && !isDashPanelActive) {
            velocity.x *= 0.2f;
            velocity.z *= 0.2f;
        }
    }

    float chargeRatio = 0.0f;

    // 攻撃の硬直が明けてから予約とジャンプ権を消費し、上向き速度と踏み切り演出をまとめて出す。
    if (!attackMovement.blocksMovementActions && jumpRequest_.Consume(chargeRatio)) {
        velocity.y = player_->GetJumpPower() * (usePinkSlimeJumpAbility ? (1.0f + chargeRatio * 0.65f) : 1.0f);
        player_->TriggerSlimeJumpTakeoff();
        player_->IncrementJumpCount();
        PlayerJumpEvent jumpEvent;
        jumpEvent.player = player_;
        EventManager::GetInstance()->Dispatch(jumpEvent);
        jumpStatus_ = "ジャンプ実行";
#ifdef USE_IMGUI
        DebugConsole::GetInstance()->AddLog("[Jump] ジャンプ実行");
#endif
        player_->SetSlimeJumpCharge(0.0f);

        // ピンク形態で専用エフェクトを呼べる場合は、地表に踏み切りのリングを出す。
        if (usePinkSlimeJumpAbility && MeshEffectManager::GetInstance()) {
            engine::Vector3 effectPos = GroundEffectLocator::ResolveGroundPosition(player_->GetWorldPosition());
            effectPos.y += 0.08f;
            MeshEffectManager::GetInstance()->SpawnEffectAt(
                "Resources/json/effect/player/copy/pink/effect_pink_slime_launch_kick_ring.json", effectPos, {0.0f, 0.0f, 0.0f},
                {1.25f, 0.9f, 1.25f});
        // 専用リングを使わない場合は、共通のジャンプ砂煙を出す。
        } else {
            EmitGroundDust("player/base/player_jump_dust", 0.035f);
        }


        // 踏み切り前の水平速度を保ち、足場速度の水平成分だけを0.55倍して空中へ持ち越す。
        platformAirVelocity_ = platformVelocity_ * 0.55f;
        platformAirVelocity_.y = 0.0f;
        const float carrySpeed = std::sqrt(platformAirVelocity_.x * platformAirVelocity_.x +
                                           platformAirVelocity_.z * platformAirVelocity_.z);
        // 足場由来の水平速度が24を超える場合は、方向を保ったまま上限へ収める。
        if (carrySpeed > 24.0f)
            platformAirVelocity_ = platformAirVelocity_ * (24.0f / carrySpeed);
        player_->ChangeState(std::make_unique<PlayerStateJump>());
    }

    // 歩行時の伸縮はPlayerSlimeAnimatorが担当し、物理の接地とジャンプ速度を保持する。

    previousAddedAirVelocity_ = {};
    // ダッシュ中以外は足場と能力の慣性を速度へ加え、次フレームに取り除く量も記録する。
    if (!isDashing_) {
        previousAddedAirVelocity_ = platformAirVelocity_ + abilityAirVelocity_;
        velocity += platformAirVelocity_;
        // 床の慣性は秒単位で指数減衰するので、フレーム刻みが変わっても同じ経過秒数で同程度に弱まります。
        platformAirVelocity_ = platformAirVelocity_ * std::exp(-0.8f * deltaTime);
        velocity += abilityAirVelocity_;
    }

    // 爆風の水平成分は急に切らずに減衰させ、空中の方向入力を妨げない。
    abilityAirVelocity_ = abilityAirVelocity_ * std::exp(-8.0f * deltaTime);
    platformVelocity_ = {};
    player_->SetVelocity(velocity);
}

// 物理更新後の接地で着地を判定する。prePhysicsVelocityY は衝突で上書きされる前の鉛直速度で、戻り値は見た目用の強度。
float PlayerMover::UpdateLandingFeedback(float prePhysicsVelocityY, float deltaTime, bool allowFeedback) {
    // プレイヤーが未設定か経過時間が0以下の場合は着地履歴を進めない。
    if (!player_ || deltaTime <= 0.0f) {
        return 0.0f;
    }

    // 砂煙の再発生を抑えながら、空中で観測した最大下降速度を接地まで保持する。
    landingDustCooldown_ = (std::max)(0.0f, landingDustCooldown_ - deltaTime);
    const bool grounded = player_->IsGrounded();
    const float downwardSpeed = (std::max)({-prePhysicsVelocityY, -player_->GetVelocity().y, 0.0f});
    // まだ空中にいれば、滞空秒数と最大の下降速度を記録する。
    if (!grounded) {
        airborneDuration_ += deltaTime;
        airborneMaxDownSpeed_ = (std::max)(airborneMaxDownSpeed_, downwardSpeed);
    }

    float impactRate = 0.0f;
    // 演出が許可され、接地履歴を持ち、0.06秒以上の空中状態から接地に変わった場合にだけ着地強度を求める。
    if (allowFeedback && hasGroundedHistory_ && !wasGroundedLastFrame_ && grounded &&
        airborneDuration_ >= 0.06f) {
        // 衝突解決がPlayer更新後に行われても、前フレームの下降速度を失わないようにします。
        const float impactSpeed = (std::max)(airborneMaxDownSpeed_, downwardSpeed);
        const float jumpPower = (std::max)(player_->GetJumpPower(), 0.01f);
        // 小さな着地を切り捨てるしきい値を引き、現在のジャンプ初速を基準に演出用の強度へ換算します。
        const float impactRange = (std::max)(jumpPower - 3.5f, 1.0f);
        impactRate = std::clamp((impactSpeed - 3.5f) / impactRange, 0.0f, 1.65f);
        // 着地強度が0.18以上で再発生の待機時間も終わっていれば、砂煙を出す。
        if (impactRate >= 0.18f && landingDustCooldown_ <= 0.0f) {
            EmitGroundDust("shared/player_land_dust", 0.035f);
            landingDustCooldown_ = 0.12f;
        }
    }

    // 接地した場合は空中の計測値を消し、次に地面を離れる時点から集め直す。
    if (grounded) {
        airborneDuration_ = 0.0f;
        airborneMaxDownSpeed_ = 0.0f;
    }
    wasGroundedLastFrame_ = grounded;
    hasGroundedHistory_ = true;
    return impactRate;
}

void PlayerMover::EmitGroundDust(const char* presetName, float yOffset) const {
    // 対象のプレイヤーか粒子設定名が無ければ、発生位置と粒子を決定できない。
    if (!player_ || !presetName)
        return;

    GpuParticleManager* particleManager = GpuParticleManager::GetInstance();
    // GPU粒子の管理が未初期化なら、砂煙の発生要求を送らない。
    if (!particleManager || !particleManager->IsInitialized())
        return;

    // プレイヤーの中心位置から接地面を求め、指定の高さを足して粒子を出す。
    engine::Vector3 emitPos = GroundEffectLocator::ResolveGroundPosition(player_->GetWorldPosition());
    emitPos.y += yOffset;
    particleManager->Emit(presetName, emitPos);
}

void PlayerMover::SetStrategy(std::unique_ptr<IMoveStrategy> strategy) {
    // 入力計算だけを差し替え、足場補正やダッシュの状態は保持する。
    strategy_ = std::move(strategy);
}

// ダッシュパネルの加速と旋回倍率を等倍へ戻します。
void PlayerMover::CancelDashPanelBoost() {
    dashPanelBoostActive_ = false;
    dashPanelSpeedMultiplier_ = 1.0f;
    dashPanelTurnMultiplier_ = 1.0f;
}

void PlayerMover::ApplyDashPanelBoost(float speedMultiplier, float turnMultiplier) {
    // 同じパネルとの接触が続いても進行基準を取り直さず、操舵範囲を維持する。
    const bool starting = !dashPanelBoostActive_;
    dashPanelBoostActive_ = true;
    dashPanelSpeedMultiplier_ = (std::max)(1.0f, speedMultiplier);
    dashPanelTurnMultiplier_ = std::clamp(turnMultiplier, 0.05f, 1.0f);
    if (starting && player_) {
        StopDashOnImpact();
        const auto velocity = player_->GetVelocity();
        dashPanelEntryYaw_ = std::hypot(velocity.x, velocity.z) > 0.05f
            ? std::atan2(velocity.x, velocity.z) : player_->GetMoveYaw();
        dashPanelYaw_ = dashPanelEntryYaw_;
    }

    // プレイヤーが未設定なら倍率だけ保存し、現在速度へは適用しない。
    if (!player_)
        return;

    // 踏んだ瞬間だけ速度を入れる。被弾中は吹き飛びを優先し、復帰後にUpdate()が再開する。
    if (!starting || !player_->IsControlActive())
        return;
    auto velocity = player_->GetVelocity();
    const float speed = (std::max)(std::hypot(velocity.x, velocity.z), player_->GetMoveSpeed() * dashPanelSpeedMultiplier_);
    velocity.x = std::sin(dashPanelYaw_) * speed;
    velocity.z = std::cos(dashPanelYaw_) * speed;
    player_->SetVelocity(velocity);
}

bool PlayerMover::IsDashPanelWall(const engine::Vector3& normal) const {
    // 上下面や坂、ほぼ平行な壁沿いの接触は停止の原因にしない。
    if (!dashPanelBoostActive_ || std::abs(normal.y) >= 0.60f)
        return false;
    return std::sin(dashPanelYaw_) * normal.x + std::cos(dashPanelYaw_) * normal.z
        < -0.20f * std::hypot(normal.x, normal.z);
}

void PlayerMover::StopDashPanelAtWall() {
    // 壁接触で残る接線方向の速度も止める。ジャンプや落下の縦速度は保持する。
    CancelDashPanelBoost();
    if (player_)
        player_->SetVelocity({0.0f, player_->GetVelocity().y, 0.0f});
}
