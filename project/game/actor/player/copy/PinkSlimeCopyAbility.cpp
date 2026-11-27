#include <cstdint>
#include "CopyAbilitySupport.h"
#include "engine/utility/math/AnimationInterpolation.h"

namespace {
using namespace copyAbilityDetail;

// ピンク能力の攻撃・ガード状態を制御し、ガード表示の寿命を能力セッションに結び付ける。
// PlayerCopyAbilityControllerが所有するピンク形態の移動と攻撃の実装。入力はProcessInput、進行はUpdate、一時表示の回収はCancelが担当する。
class PinkSlimeCopyAbility final : public ICopyAbilitySession {
  public:
    explicit PinkSlimeCopyAbility(EnemyAttackProfile profile) : ICopyAbilitySession(std::move(profile)) {
        // ガードの膜と輪を同じスコープに入れ、解除時に一括停止できるようにする。
        // メッシュ演出の管理が使える場合に限り、このセッション専用の停止単位を確保する。
        if (auto* effects = MeshEffectManager::GetInstance())
            guardScope_ = effects->CreateEffectScope();
    }

    ~PinkSlimeCopyAbility() override {
        // コピー解除後にガードの膜だけが残らないよう、セッションの寿命で停止する。
        StopGuardVisuals();
    }

    const char* GetStateLabel() const override {
        // 同じ攻撃状態でも段数ごとに表示を分け、プレビューから進行を判別できるようにする。
        switch (state_) {
        // 入力を待っている状態を表示する。
        case State::kIdle:
            return "待機";

        // 地上で防御を維持している間の表示。
        case State::kGuard:
            return "ガード中";

        // 近接攻撃中は現在の段数を表示する。
        case State::kElasticStrike:
            return comboStage_ == 2 ? "近接3段目: プレス"
                                    : (comboStage_ == 1 ? "近接2段目: 切り返し" : "近接1段目: ボディ打撃");

        // E技の上昇前に力を溜めている段階。
        case State::kBounceCharge:
            return "E: 溜め";

        // E技の上昇中で、まだ突進には入っていない段階。
        case State::kBounceRise:
            return "E: 跳ね上がり";

        // E技で前進しながら攻撃判定を出す段階。
        case State::kBounceRush:
            return "E: 空中突進";

        // E技で着地し、操作が戻るまで待つ段階。
        case State::kBounceRecovery:
            return "E: 着地硬直";
        }

        return "待機";
    }

    void ProcessInput(Player& player, const PlayerCopyAbilityInput& input) override {
        // 攻撃中の追加入力は移動や別技へ流さず、次段の予約として保持する。
        guardHeld_ = input.secondaryHeld;

        // 近接攻撃を実行中は、別の技やガードを始めず連撃だけを受け付ける。
        if (state_ == State::kElasticStrike) {
            // 左クリックが新たに押され、最終段より前で残り0.10秒以上のときだけ次の一段を予約する。
            if (input.primaryTriggered && comboStage_ < 2 && duration_ - timer_ >= 0.10f)
                comboQueued_ = true;
            return;
        }

        // E入力は別の行動が残っていても短時間だけ預かり、待機に戻った時点で成立条件を確認する。
        if (input.specialTriggered) {
            bounceInputBuffer_ = kBounceInputBufferDuration;
        }

        // 近接以外の技を実行中はE入力の保留までにとどめ、待機へ戻るまで新しい技を始めない。
        if (state_ != State::kIdle) {
            return;
        }

        // 地上ガードは移動入力を止め、防御判定・姿勢・表示をまとめて開始する。
        if (input.secondaryTriggered && player.IsGrounded() && guardCooldown_ <= 0.0f) {
            state_ = State::kGuard;
            guardTimer_ = 1.25f;
            guardHitMotionTimer_ = 0.0f;
            effectTimer_ = 0.0f;
            direction_ = NormalizePlanar(player.GetForwardDirection());
            player.SetVelocity({0.0f, player.GetVelocity().y, 0.0f});
            player.SetIsControlActive(false);
            player.SetGuarding(true);
            player.SetSlimeAbilityPose(PlayerSlimeAnimator::AbilityPose::kGuard);
            player.PlaySlimeAbilityMotion("player_ability_pink_guard", true, 1.0f, 0.05f, 0.08f);
            UpdateGuardVisuals(player);
            return;
        }

        // E入力の保留が残り、再使用と接地の条件が揃えば、通常の打撃を調べる前に溜めへ入る。
        if (bounceInputBuffer_ > 0.0f && CanStartBounceRush(player)) {
            BeginBounceCharge(player);
            return;
        }

        // 左クリックが新たに押され、接地中で近接技の再使用が可能な場合だけ1段目を始める。
        if (input.primaryTriggered && straightCooldown_ <= 0.0f && player.IsGrounded()) {
            BeginElasticStrike(player);
            straightCooldown_ = 0.68f;
        }
    }

    void Update(Player& player, float deltaTime) override {
        // 攻撃ごとの再使用待ちと接地猶予を先に更新し、この後の状態判断に最新の時間を使う。
        straightCooldown_ = std::max(0.0f, straightCooldown_ - deltaTime);
        bounceCooldown_ = std::max(0.0f, bounceCooldown_ - deltaTime);
        guardCooldown_ = std::max(0.0f, guardCooldown_ - deltaTime);
        guardImpactCooldown_ = std::max(0.0f, guardImpactCooldown_ - deltaTime);
        bounceInputBuffer_ = std::max(0.0f, bounceInputBuffer_ - deltaTime);
        groundedGraceTimer_ =
            player.IsGrounded() ? kBounceGroundedGraceDuration : std::max(0.0f, groundedGraceTimer_ - deltaTime);
        visualTime_ += deltaTime;

        // 入力猶予は入力したフレームだけでなく、着地するまで毎フレーム再評価します。
        if (state_ == State::kIdle && bounceInputBuffer_ > 0.0f && CanStartBounceRush(player)) {
            BeginBounceCharge(player);
            return;
        }

        // ガード成功の単発モーションが終わったら、防御のループ姿勢へ戻す。
        const bool guardHitMotionWasActive = guardHitMotionTimer_ > 0.0f;
        guardHitMotionTimer_ = std::max(0.0f, guardHitMotionTimer_ - deltaTime);

        // 防御を続けたまま防御成功モーションの時間が切れた場合だけ、ガードのループを再生する。
        if (guardHitMotionWasActive && guardHitMotionTimer_ <= 0.0f && state_ == State::kGuard) {
            player.PlaySlimeAbilityMotion("player_ability_pink_guard", true, 1.0f, 0.035f, 0.08f);
        }

        effectTimer_ -= deltaTime;
        storedPowerEffectTimer_ -= deltaTime;

        // 待機中で弾性の蓄積が残っていれば、次の発光時刻に脈動を出して強化を示す。
        if (state_ == State::kIdle && storedBouncePower_ > 0.0f && storedPowerEffectTimer_ <= 0.0f) {
            const float effectScale = 0.56f + storedBouncePower_ * 0.34f;
            SpawnEffect("Resources/json/effect/shared/effect_pink_slime_charge_pulse_ring.json",
                        player.GetWorldPosition() + engine::Vector3{0.0f, 0.62f, 0.0f}, {},
                        {effectScale, effectScale, effectScale});
            storedPowerEffectTimer_ = 0.32f;
        }

        // 右クリックを離した、持続時間が尽きた、または足場を外れた時点でガードを終了する。
        if (state_ == State::kGuard) {
            guardTimer_ = std::max(0.0f, guardTimer_ - deltaTime);

            // 右入力の解除、ガード時間の満了、接地の喪失のいずれかで通常状態へ戻る。
            if (!guardHeld_ || guardTimer_ <= 0.0f || !player.IsGrounded()) {
                Finish(player);
                return;
            }

            player.SetVelocity({0.0f, player.GetVelocity().y, 0.0f});
            player.SetIsControlActive(false);
            player.SetGuarding(true);
            player.SetSlimeAbilityPose(PlayerSlimeAnimator::AbilityPose::kGuard);
            player.ForceSlimeAnimationModeForNextUpdate(PlayerSlimeAnimator::Mode::kIdle, direction_);
            // 表示の更新間隔を満たしたときだけ、膜と輪を現在の体格・向きに合わせ直す。
            if (effectTimer_ <= 0.0f)
                UpdateGuardVisuals(player);
            return;
        }

        // 飛び上がる前の溜めでは座標を保つため水平速度と上向き速度を抑え、足元へ脈動を重ねる。
        if (state_ == State::kBounceCharge) {
            timer_ = std::max(0.0f, timer_ - deltaTime);
            player.SetVelocity({0.0f, std::min(player.GetVelocity().y, 0.0f), 0.0f});
            player.SetIsControlActive(false);
            player.SetMoveYaw(std::atan2(direction_.x, direction_.z));

            // 溜め中の表示間隔が終わるたびに、足元に強さに応じた波紋を出す。
            if (effectTimer_ <= 0.0f) {
                const float pulseScale = 0.72f + activeBouncePower_ * 0.28f;
                SpawnEffect("Resources/json/effect/shared/effect_pink_slime_charge_pulse_ring.json",
                            player.GetWorldPosition() + engine::Vector3{0.0f, 0.12f, 0.0f}, {},
                            {pulseScale, pulseScale, pulseScale});
                effectTimer_ = 0.065f;
            }

            // 溜め時間を使い切ったら、接地を外して上昇速度を与える。
            if (timer_ <= 0.0f) {
                LaunchBounceRise(player);
            }

            return;
        }

        // 上昇中は前進を補助し、上昇速度が落ちるか規定時間に達したら突進へ切り替える。
        if (state_ == State::kBounceRise) {
            timer_ = std::max(0.0f, timer_ - deltaTime);
            engine::Vector3 velocity = player.GetVelocity();
            const float riseForwardSpeed = 6.2f + activeBouncePower_ * 1.4f;
            velocity.x = direction_.x * riseForwardSpeed;
            velocity.z = direction_.z * riseForwardSpeed;
            player.SetVelocity(velocity);
            player.SetIsControlActive(false);
            player.SetMoveYaw(std::atan2(direction_.x, direction_.z));
            player.ForceSlimeAnimationModeForNextUpdate(PlayerSlimeAnimator::Mode::kJump, direction_);

            // 規定時間に達するか上昇速度が7.5以下になったら、水平突進へ切り替える。
            if (timer_ <= 0.0f || velocity.y <= 7.5f) {
                BeginBounceRush(player);
            }

            return;
        }

        // 突進は前へ進みながら専用の重力で降下し、経過時間で水平速度を少しずつ落とす。
        if (state_ == State::kBounceRush) {
            timer_ = std::max(0.0f, timer_ - deltaTime);
            const float rushProgress = 1.0f - timer_ / std::max(duration_, 0.001f);
            const float rushSpeed = (34.0f + activeBouncePower_ * 6.0f) * (1.0f - SmoothStep(rushProgress) * 0.24f);
            engine::Vector3 velocity = player.GetVelocity();
            velocity.x = direction_.x * rushSpeed;
            velocity.z = direction_.z * rushSpeed;
            const float rushGravity = 68.0f + activeBouncePower_ * 12.0f;
            const float rushTerminalDownwardSpeed = 34.0f + activeBouncePower_ * 6.0f;
            velocity.y = std::max(velocity.y - rushGravity * deltaTime, -rushTerminalDownwardSpeed);
            player.SetVelocity(velocity);
            player.SetIsControlActive(false);
            player.SetMoveYaw(std::atan2(direction_.x, direction_.z));
            player.ForceSlimeAnimationModeForNextUpdate(PlayerSlimeAnimator::Mode::kDash, direction_);
            DamageRush(player);
            if (state_ != State::kBounceRush) return; // 同期の被弾通知で中断した後に着地硬直を開始しない。

            // 突進の発生位置から後方へ軌跡を引き、命中判定とは別の周期で更新する。
            if (effectTimer_ <= 0.0f) {
                const engine::Vector3 trailPosition =
                    player.GetWorldPosition() - direction_ * 0.62f + engine::Vector3{0.0f, 0.52f, 0.0f};
                SpawnEffect("Resources/json/effect/player/copy/pink/effect_player_pink_straight_arc.json", trailPosition,
                            {0.0f, std::atan2(direction_.x, direction_.z), 0.0f}, {0.88f, 0.78f, 1.12f});
                EmitDirectedPreset("player/copy/pink/player_pink_bounce_droplets", trailPosition,
                                   direction_ * -1.0f + engine::Vector3{0.0f, 0.16f, 0.0f}, 0.82f);
                effectTimer_ = 0.055f;
            }

            // 突進直後の接地を拾わないよう、進行が6%を越えて接地した場合だけ着地処理へ進む。
            if (player.IsGrounded() && rushProgress > 0.06f) {
                FinishBounceRush(player);
            } else if (timer_ <= 0.0f) {
                // 地面へ届かなかった場合は空中で衝撃を発生させず、操作だけ安全に返します。
                Finish(player);
            }

            return;
        }

        // 着地後は短い硬直を消化してから操作を返し、突進判定は更新しない。
        if (state_ == State::kBounceRecovery) {
            // 接地後に突進の水平速度を持ち越さない。鉛直は地形との衝突処理に任せる。
            if (player.IsGrounded())
                player.SetVelocity({0.0f, player.GetVelocity().y, 0.0f});
            timer_ = std::max(0.0f, timer_ - deltaTime);

            // 着地硬直が終わったタイミングで操作を返す。
            if (timer_ <= 0.0f) {
                Finish(player);
            }

            return;
        }

        // ここまでの各技に該当せず、近接攻撃中でもなければ、打撃判定を評価しない。
        if (state_ != State::kElasticStrike) {
            return;
        }

        const float previousElapsed = duration_ - timer_;
        timer_ = std::max(0.0f, timer_ - deltaTime);
        const float elapsed = duration_ - timer_;

        // 攻撃中は移動を止めたまま、方向入力だけを受け付ける。
        // 見た目・踏み込み・エフェクト・攻撃判定が別方向にならないよう、共通の方向を更新する。
        const engine::Vector3 moveInput = player.GetMoveInputDirection();

        // 水平入力が十分に大きい場合にだけ、打撃方向を入力へ滑らかに寄せる。
        if (moveInput.x * moveInput.x + moveInput.z * moveInput.z > 0.001f) {
            const float currentYaw = std::atan2(direction_.x, direction_.z);
            const float targetYaw = std::atan2(moveInput.x, moveInput.z);
            const float yaw = animationInterpolation::DampAngle(currentYaw, targetYaw, 14.0f, deltaTime);
            direction_ = {std::sin(yaw), 0.0f, std::cos(yaw)};
        }

        const float hitStart = comboStage_ == 2 ? 0.40f : 0.22f;
        const float hitEnd = comboStage_ == 2 ? 0.49f : 0.36f;

        // 位置の直接書き換えはせず、通常の移動・壁衝突を通す短い踏み込み。
        const float stepOverlap = std::max(0.0f, std::min(elapsed, 0.34f) - std::max(previousElapsed, 0.18f));
        const float stepSpeed = (comboStage_ == 2 ? 0.0f : 8.0f) * stepOverlap / std::max(deltaTime, 0.0001f);
        engine::Vector3 velocity = player.GetVelocity();
        velocity.x = direction_.x * stepSpeed;
        velocity.z = direction_.z * stepSpeed;
        // 接地中に下向き速度が残っていた場合は、地面へ押し込み続けないよう0にする。
        if (player.IsGrounded() && velocity.y < 0.0f)
            velocity.y = 0.0f;
        player.SetVelocity(velocity);
        player.SetIsControlActive(false);
        player.SetMoveYaw(std::atan2(direction_.x, direction_.z));
        player.ForceSlimeAnimationModeForNextUpdate(PlayerSlimeAnimator::Mode::kIdle, direction_);

        // 打点に達してから接地中に一度だけ軌跡を起動する。命中の有無は下の範囲判定で調べる。
        if (!strikeDidDamage_ && elapsed >= hitStart && player.IsGrounded()) {
            strikeDidDamage_ = true;

            // 空振りでは振り抜きの軌跡のみ。命中の閃光は実際のダメージ処理に任せます。
            const engine::Vector3 origin =
                player.GetWorldPosition() + direction_ * 0.55f + engine::Vector3{0.0f, comboStage_ == 2 ? 0.08f : 0.65f, 0.0f};
            const float effectYaw = std::atan2(direction_.x, direction_.z);

            if (comboStage_ == 2) {
                // 3段目は打点から広がる水平の衝撃線と、立ち上がる短い衝撃を組み合わせる。
                for (std::int32_t i = 0; i < 7; ++i) {
                    const float yaw = effectYaw + (static_cast<float>(i) - 3.0f) * 0.48f;
                    const engine::Vector3 offset{std::sin(yaw) * 0.30f, 0.05f, std::cos(yaw) * 0.30f};
                    SpawnEffect("Resources/json/effect/player/copy/pink/effect_player_pink_combo_press.json", origin + offset,
                                {-0.20f - 0.08f * static_cast<float>(i % 2), yaw, 0.0f});
                }

                // 打点の縦方向にも短い衝撃を立て、地面の線だけに見えないようにします。
                for (std::int32_t i = -1; i <= 1; ++i) {
                    SpawnEffect("Resources/json/effect/player/copy/pink/effect_player_pink_guard_impact.json", origin,
                                {-kPi * 0.5f + i * 0.30f, effectYaw, 0.0f}, {1.3f, 1.3f, 1.8f});
                }
            } else {
                // 1段目はボディ打撃、2段目は反対方向へ振り抜く切り返しの軌跡を使う。
                SpawnEffect(comboStage_ == 1 ? "Resources/json/effect/player/copy/pink/effect_player_pink_return_sweep.json"
                                             : "Resources/json/effect/player/copy/pink/effect_player_pink_body_sweep.json",
                            origin, {0.0f, effectYaw, comboStage_ == 1 ? kPi - 0.28f : 0.28f});
            }

            // 地面を叩く3段目だけ、打点から短命の立体破片を出す。
            if (comboStage_ == 2)
                SpawnDebrisOnGround("player/copy/pink/player_pink_press_chips", origin);
        }

        // この更新区間が打撃の有効時間と重なり、現在接地していれば敵の重なりを調べる。
        if (elapsed >= hitStart && previousElapsed <= hitEnd && player.IsGrounded()) {
            // 有効時間中の前方薙ぎ払い。同じ敵には1振りにつき1回だけ命中します。
            const engine::Vector3 scale = player.GetScale();
            const float bodySize = std::max(0.1f, std::max(std::abs(scale.x), std::abs(scale.z)));
            const engine::Vector3 center = player.GetWorldPosition() + direction_ * (bodySize * 0.70f) +
                                   engine::Vector3{0.0f, std::abs(scale.y) * 0.34f, 0.0f};
            engine::PhysicsQueryFilter filter;
            filter.mask = engine::collisionCategories::Get("enemy");
            filter.ignoredObject = &player;

            for (const engine::PhysicsOverlapHit& hit : engine::CollisionManager::GetInstance()->OverlapSphere(
                     center, bodySize * (comboStage_ == 2 ? 1.05f : 0.78f), filter)) {
                engine::Object3D* target = FindEnemyRoot(hit.object);
                // 敵本体を解決できない当たりや、同じ段で処理済みの敵は重複して攻撃しない。
                if (!target || !hitTargets_.insert(target).second)
                    continue;
                const engine::Vector3 push = NormalizePlanar(target->GetWorldPosition() - player.GetWorldPosition());
                DamageEnemy(player, target, rushDamage_, DamageType::kPhysical,
                            {push.x * (comboStage_ == 2 ? 10.0f : 4.0f), comboStage_ == 2 ? 5.0f : 1.0f,
                             push.z * (comboStage_ == 2 ? 10.0f : 4.0f)});
                if (state_ != State::kElasticStrike) return; // 同期の被弾通知後にコンボを進めない。
            }
        }

        // 段末で予約と接地を確認し、次の段か通常操作への復帰を選ぶ。
        if (timer_ <= 0.0f) {
            // 予約があり、3段目より前で接地を保っていれば、その向きを引き継いで次段へ進む。
            if (comboQueued_ && comboStage_ < 2 && player.IsGrounded()) {
                BeginElasticStrike(player, comboStage_ + 1);
                return;
            }

            // 次段の条件を満たさない場合は、残っている攻撃状態を終える。
            Finish(player);
        }
    }

    void NotifyDamageStarted(Player& player) override {
        // 被弾の吹き飛びを次回の突進・着地更新で上書きしない。
        bounceInputBuffer_ = 0.0f;
        Finish(player);
    }

    void Cancel(Player& player) override {
        // コピー解除時は蓄積した強化とモーションを破棄し、通常操作へ復帰させる。
        player.ClearSlimeAbilityMotion();
        storedBouncePower_ = 0.0f;
        activeBouncePower_ = 0.0f;
        guardHitMotionTimer_ = 0.0f;
        Finish(player);
    }

  private:
    enum class State { kIdle, kElasticStrike, kGuard, kBounceCharge, kBounceRise, kBounceRush, kBounceRecovery };

  public:
    bool CanBreakImpactGate() const override {
        // 扉を壊せるのは打撃の命中時間か空中突進中に限定する。
        const float elapsed = duration_ - timer_;
        return (state_ == State::kElasticStrike && elapsed >= (comboStage_ == 2 ? 0.40f : 0.22f) &&
                elapsed <= (comboStage_ == 2 ? 0.49f : 0.36f)) ||
               state_ == State::kBounceRush;
    }

    bool IsPinkBounceSlamImpactActive() const override {
        // 着地後の硬直中に突進の接触判定を残さない。
        return state_ == State::kBounceRush;
    }

    bool NotifyGuardedHit(Player& player, const engine::Vector3& sourcePosition) override {
        // 同じ接触の連続通知で反撃エフェクトと蓄積値が重複しないよう間隔を置く。
        // ガード中でない場合と前回の受け止めから0.22秒経っていない場合は、蓄積と演出を更新しない。
        if (state_ != State::kGuard || guardImpactCooldown_ > 0.0f) {
            return false;
        }

        // 防御成功で蓄積を最大1まで増やし、攻撃が来た側へ向けて専用モーションを再生する。
        guardImpactCooldown_ = 0.22f;
        guardHitMotionTimer_ = 0.24f;
        storedBouncePower_ = std::min(1.0f, storedBouncePower_ + 0.36f);
        direction_ = NormalizePlanar(sourcePosition - player.GetWorldPosition());
        player.SetMoveYaw(std::atan2(direction_.x, direction_.z));
        player.PlaySlimeAbilityMotion("player_ability_pink_guard_hit", false, 1.0f, 0.015f, 0.045f);
        // 防御で受けた向きの体表に接触点を置き、中央と両脇へ衝撃を出す。
        const engine::Aabb bounds = player.GetAabb();
        const float radius = std::max(0.35f, (bounds.max.x - bounds.min.x) * 0.5f);
        const engine::Vector3 contact = player.GetWorldPosition() + direction_ * radius +
                                engine::Vector3{0.0f, (bounds.max.y - bounds.min.y) * 0.32f, 0.0f};
        const float yaw = std::atan2(direction_.x, direction_.z);

        for (std::int32_t i = -1; i <= 1; ++i) {
            SpawnEffect("Resources/json/effect/player/copy/pink/effect_player_pink_guard_impact.json", contact,
                        {-0.25f, yaw + i * 0.65f, 0.0f});
        }

        UpdateGuardVisuals(player);
        return true;
    }

  private:
    void StopGuardVisuals() {
        // ガード用スコープだけを止め、ほかの能力エフェクトには触れない。
        // セッションが専用の停止単位を取得できていた場合だけ、対応する表示を止める。
        if (guardScope_ != 0) {
            // 停止要求を受け取るメッシュ演出の管理が生きている場合にだけ操作する。
            if (auto* effects = MeshEffectManager::GetInstance())
                effects->StopEffectScope(guardScope_);
        }
    }

    void UpdateGuardVisuals(Player& player) {
        // 以前の膜と輪を止め、現在のモデルのワールド境界に合わせてガード表示を組み直す。
        StopGuardVisuals();
        const engine::Aabb bounds = player.GetModelWorldAabb();
        const float radius = std::max(0.35f, std::max(bounds.max.x - bounds.min.x, bounds.max.z - bounds.min.z) * 0.5f);
        const float height = bounds.max.y - bounds.min.y;
        const float yaw = std::atan2(direction_.x, direction_.z);
        const engine::Vector3 position{(bounds.min.x + bounds.max.x) * 0.5f, bounds.min.y + height * 0.34f,
                               (bounds.min.z + bounds.max.z) * 0.5f};
        const engine::Vector3 size{radius * 1.62f, std::max(height * 0.64f, radius), radius * 1.62f};
        SpawnScopedEffect(guardScope_, "Resources/json/effect/player/copy/pink/effect_player_pink_guard_barrier.json", position, {},
                          size);
        SpawnScopedEffect(guardScope_, "Resources/json/effect/player/copy/pink/effect_player_pink_guard_barrier_rim.json", position,
                          {0.0f, yaw, 0.0f}, size);
        SpawnScopedEffect(guardScope_, "Resources/json/effect/player/copy/pink/effect_player_pink_guard_barrier_rim.json", position,
                          {kPi * 0.5f, yaw, 0.0f}, {size.x, size.z, size.y});
        effectTimer_ = 0.12f;
    }

    bool CanStartBounceRush(Player& player) const {
        // 段差から離れた直後も短い接地猶予内なら入力を受け付ける。
        return bounceCooldown_ <= 0.0f && (player.IsGrounded() || groundedGraceTimer_ > 0.0f);
    }

    // この段の秒数と攻撃方向を決め、前段の入力予約と命中記録を破棄する。
    void BeginElasticStrike(Player& player, std::int32_t stage = 0) {
        state_ = State::kElasticStrike;
        comboStage_ = stage;
        comboQueued_ = false;
        // 段ごとの持続時間、ダメージ、モーションを一緒に切り替える。
        duration_ = 0.68f;
        float damage = 10.0f;
        const char* motion = "player_ability_pink_elastic_strike";
        if (stage == 1) {
            duration_ = 0.64f;
            damage = 11.0f;
            motion = "player_ability_pink_combo_return";
        } else if (stage == 2) {
            duration_ = 0.86f;
            damage = 18.0f;
            motion = "player_ability_pink_combo_press";
        }
        straightCooldown_ = duration_;
        timer_ = duration_;

        // 2段目以降は前段で操作した向きを引き継ぎ、ロックオンへ急に向き直らない。
        if (stage == 0)
            direction_ = NormalizePlanar(player.GetLockOnAttackDirection());
        rushDamage_ = damage;
        effectTimer_ = 0.0f;
        // 各段で初回の軌跡と命中判定を使えるようにしてから、操作を止めて専用モーションを始める。
        strikeDidDamage_ = false;
        hitTargets_.clear();
        player.SetIsControlActive(false);
        player.SetMoveYaw(std::atan2(direction_.x, direction_.z));
        player.SetVelocity({0.0f, player.GetVelocity().y, 0.0f});
        player.PlaySlimeAbilityMotion(motion, false, 1.0f, 0.025f, 0.055f);
    }

    void BeginBounceCharge(Player& player) {
        // ガードで貯めた強化量はここで消費し、次回の突進へ持ち越さない。
        state_ = State::kBounceCharge;
        bounceInputBuffer_ = 0.0f;
        direction_ = NormalizePlanar(player.GetLockOnAttackDirection());
        duration_ = 0.18f;
        timer_ = duration_;
        effectTimer_ = 0.0f;
        activeBouncePower_ = storedBouncePower_;
        storedBouncePower_ = 0.0f;
        storedPowerEffectTimer_ = 0.0f;
        // この突進で新たに命中を数え、足元の溜め演出と操作停止を開始する。
        hitTargets_.clear();
        player.SetIsControlActive(false);
        player.SetMoveYaw(std::atan2(direction_.x, direction_.z));
        player.SetVelocity({0.0f, std::min(player.GetVelocity().y, 0.0f), 0.0f});
        player.PlaySlimeAbilityMotion("player_ability_pink_bounce_charge", false, 1.0f, 0.015f, 0.035f);
        player.TriggerSlimeImpulse({2.82f, 0.74f, 2.82f}, 0.11f);
        SpawnDebrisOnGround("player/copy/pink/pink_slime_charge_pebble_pull", player.GetWorldPosition());
        bounceCooldown_ = 1.15f;
    }

    void LaunchBounceRise(Player& player) {
        // 接地を先に外し、上昇中に地面側の移動処理が速度を打ち消さないようにする。
        player.SetGrounded(false);
        state_ = State::kBounceRise;
        duration_ = 0.40f;
        timer_ = duration_;
        effectTimer_ = 0.0f;
        player.SetIsControlActive(false);
        player.SetMoveYaw(std::atan2(direction_.x, direction_.z));
        // 蓄えた弾性を上昇と前進の初速へ反映する。既に速く上昇していればその速度を保つ。
        const float launchForwardSpeed = 7.5f + activeBouncePower_ * 1.8f;
        const float launchVerticalSpeed = 19.5f + activeBouncePower_ * 4.0f;
        player.SetVelocity({direction_.x * launchForwardSpeed, std::max(player.GetVelocity().y, launchVerticalSpeed),
                            direction_.z * launchForwardSpeed});
        player.StartEvasionInvincibility(0.68f);
        player.PlaySlimeAbilityMotion("player_ability_pink_bounce_rise", false, 1.0f, 0.018f, 0.045f);
        player.TriggerSlimeImpulse({0.72f, 1.70f, 0.72f}, 0.16f);
        SpawnEffect(
            "Resources/json/effect/player/copy/pink/effect_player_pink_bounce_launch.json",
            player.GetWorldPosition() + engine::Vector3{0.0f, 0.08f, 0.0f}, {},
            {1.0f + activeBouncePower_ * 0.24f, 1.0f + activeBouncePower_ * 0.24f, 1.0f + activeBouncePower_ * 0.24f});
        EmitDirectedPreset("player/copy/pink/player_pink_bounce_droplets", player.GetWorldPosition() + engine::Vector3{0.0f, 0.15f, 0.0f},
                           {0.0f, -1.0f, 0.0f}, 0.78f + activeBouncePower_ * 0.28f);
    }

    void BeginBounceRush(Player& player) {
        // 上昇から突進へ切り替える際、命中済み集合を消してこの技の判定を始める。
        state_ = State::kBounceRush;
        duration_ = 0.38f;
        timer_ = duration_;
        effectTimer_ = 0.0f;
        rushDamage_ = 16.0f + activeBouncePower_ * 5.0f;
        hitTargets_.clear();
        engine::Vector3 velocity = player.GetVelocity();
        const float launchSpeed = 34.0f + activeBouncePower_ * 6.0f;
        velocity.x = direction_.x * launchSpeed;
        velocity.z = direction_.z * launchSpeed;
        velocity.y = std::max(velocity.y, 3.8f);
        player.SetVelocity(velocity);
        player.SetIsControlActive(false);
        player.SetMoveYaw(std::atan2(direction_.x, direction_.z));
        // 降下突進の開始と同時に無敵と専用モーションへ切り替え、軌跡の発生を始める。
        player.SetDashInvincible(true);
        player.StartEvasionInvincibility(duration_ + 0.12f);
        player.ForceSlimeAnimationModeForNextUpdate(PlayerSlimeAnimator::Mode::kDash, direction_);
        player.PlaySlimeAbilityMotion("player_ability_pink_rush", false, 0.78f / duration_, 0.02f, 0.05f);
        player.TriggerSlimeImpulse({1.56f, 1.04f, 1.56f}, 0.12f);
        SpawnEffect("Resources/json/effect/player/copy/pink/effect_pink_slime_apex_focus_flash.json",
                    player.GetWorldPosition() + engine::Vector3{0.0f, 0.72f, 0.0f}, {}, {0.82f, 0.82f, 0.82f});
    }

    void FinishBounceRush(Player& player) {
        // 地面に届いた場合だけ着地演出を出し、短い硬直を経て操作を返す。
        const engine::Vector3 groundPosition = HitEffectDirector::ResolveGroundEffectPosition(player.GetWorldPosition());

        // 現在接地している場合だけ、地面位置に破片と衝撃を出して着地姿勢を適用する。
        if (player.IsGrounded()) {
            SpawnDebrisOnGround("player/copy/pink/pink_slime_landing_pebble_burst", groundPosition);
            SpawnEffect("Resources/json/effect/player/copy/pink/effect_player_pink_straight_impact.json",
                        groundPosition + engine::Vector3{0.0f, 0.10f, 0.0f}, {}, {0.72f, 0.72f, 0.72f});
            player.TriggerSlimeImpulse({2.36f, 0.42f, 2.36f}, 0.12f);
        }

        // 衝撃の有無にかかわらず、状態を硬直に切り替えて突進の攻撃判定を閉じる。
        state_ = State::kBounceRecovery;
        const auto movement = CopyAttackMovement(GetProfile(), "player_pink_bounce_recovery");
        timer_ = player.IsGrounded() && movement.enabled ? movement.duration : 0.18f;
        if (player.IsGrounded())
            player.SetVelocity({0.0f, player.GetVelocity().y, 0.0f});
    }

    void DamageRush(Player& player) {
        // 複数の子Colliderが重なっても、敵本体には突進1回につき1回だけ当てる。
        engine::PhysicsQueryFilter filter;
        filter.mask = engine::collisionCategories::Get("enemy");
        filter.ignoredObject = &player;
        const engine::Vector3 center = player.GetWorldPosition() + engine::Vector3{0.0f, 0.68f, 0.0f};

        for (const engine::PhysicsOverlapHit& hit : engine::CollisionManager::GetInstance()->OverlapSphere(center, 1.35f, filter)) {
            engine::Object3D* target = FindEnemyRoot(hit.object);

            // 敵本体が見つからない場合と、この突進で処理済みの場合は攻撃を繰り返さない。
            if (!target || !hitTargets_.insert(target).second) {
                continue;
            }

            DamageEnemy(player, target, rushDamage_, DamageType::kPhysical,
                        {direction_.x * 12.0f, 5.5f, direction_.z * 12.0f});
            if (state_ != State::kBounceRush) return;
            SpawnEffect("Resources/json/effect/player/copy/pink/effect_player_pink_straight_impact.json",
                        target->GetWorldPosition() + engine::Vector3{0.0f, 0.55f, 0.0f});
        }
    }

    // 技共通の後片付けで入力を解放し、打撃や突進の残り時間と一時判定を消す。
    void Finish(Player& player) {
        // 既に待機中なら復元対象がないため、プレイヤーの操作や速度を触らない。
        if (state_ == State::kIdle) {
            return;
        }

        // ガードの解除の場合は専用表示を止め、再び防御できるまでの時間を確保する。
        if (state_ == State::kGuard) {
            StopGuardVisuals();

            // 防御中に再使用待ちを消化させず、解除後に必ず隙を作ります。
            guardCooldown_ = std::max(guardCooldown_, 0.82f);
        }

        // 攻撃中に停止した操作と一時フラグを戻し、残った水平速度を減らして通常状態にする。
        player.SetGuarding(false);
        player.SetDashInvincible(false);
        player.SetSlimeAbilityPose(PlayerSlimeAnimator::AbilityPose::kNone);
        player.StopSlimeAbilityMotion(0.06f);
        player.SetIsControlActive(true);
        engine::Vector3 velocity = player.GetVelocity();
        velocity.x *= 0.30f;
        velocity.z *= 0.30f;
        player.SetVelocity(velocity);
        state_ = State::kIdle;
        timer_ = 0.0f;
        guardTimer_ = 0.0f;
        guardHitMotionTimer_ = 0.0f;
        activeBouncePower_ = 0.0f;
        diving_ = false;
        hitTargets_.clear();
    }

    State state_ = State::kIdle;
    // ガードの膜と輪を一括停止する識別子。0は専用スコープ未作成。
    MeshEffectManager::EffectScopeId guardScope_ = 0;
    engine::Vector3 direction_{0.0f, 0.0f, 1.0f};
    std::unordered_set<engine::Object3D*> hitTargets_;
    float timer_ = 0.0f;
    float duration_ = 0.0f;
    float startSpeed_ = 0.0f;
    float effectTimer_ = 0.0f;
    float straightCooldown_ = 0.0f;
    float bounceCooldown_ = 0.0f;
    float guardCooldown_ = 0.0f;
    float guardTimer_ = 0.0f;
    float guardImpactCooldown_ = 0.0f;
    float guardHitMotionTimer_ = 0.0f;
    float storedPowerEffectTimer_ = 0.0f;
    // 防御で蓄えた強化量と、現在のE技が消費した強化量。両方とも0～1。
    float storedBouncePower_ = 0.0f;
    float activeBouncePower_ = 0.0f;
    // E入力を保留する残り秒数と、離地直後でもEを許す残り秒数。
    float bounceInputBuffer_ = 0.0f;
    float groundedGraceTimer_ = 0.0f;
    float visualTime_ = 0.0f;
    float rushDamage_ = 10.0f;
    bool guardHeld_ = false;
    bool diving_ = false;
    // 現在の段で振り抜きの軌跡を開始済みか。命中した敵はhitTargets_で別に管理する。
    bool strikeDidDamage_ = false;
    std::int32_t comboStage_ = 0;
    bool comboQueued_ = false;

    static constexpr float kBounceInputBufferDuration = 0.36f;
    static constexpr float kBounceGroundedGraceDuration = 0.16f;
};
} // namespace

namespace copyAbilityDetail {
std::unique_ptr<ICopyAbilitySession> CreatePinkSlimeCopyAbility(const EnemyAttackProfile& profile) {
    // 呼び出し元ごとに独立した攻撃・ガード状態を持つセッションを作る。
    return std::make_unique<PinkSlimeCopyAbility>(profile);
}
} // namespace copyAbilityDetail
