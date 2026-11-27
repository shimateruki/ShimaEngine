#include <cstdint>
#include "CopyAbilitySupport.h"
#include <array>
#include <unordered_map>
#include "WindCopySettings.h"
#include "game/actor/enemy/boss/magma/EnemyMagmaSlime.h"
#include "game/actor/enemy/boss/prism/EnemyPrismSlime.h"
#include "game/actor/enemy/heavy/EnemyGiantSlime.h"

namespace {
using namespace copyAbilityDetail;

// 左クリック竜巻、風跳び、独立して進む小型竜巻の状態を管理する。
// 移動はPlayerAttackMovement経由で既存Motorへ渡し、描画はMeshEffectManagerの専用スコープで管理する。
// PlayerCopyAbilityControllerが所有する風攻撃の実装。入力はProcessInput、進行はUpdate、一時表示の回収はCancelが担当する。
class WindSlimeCopyAbility final : public ICopyAbilitySession {
  public:
    explicit WindSlimeCopyAbility(EnemyAttackProfile profile) : ICopyAbilitySession(std::move(profile)) {
        // 同じ技の各描画レイヤーを後から個別に評価・停止できるよう、先にスコープIDを確保する。
        for (auto& scope : tornadoScopes_)
            scope = MeshEffectManager::GetInstance()->CreateEffectScope();
        windScope_ = MeshEffectManager::GetInstance()->CreateEffectScope();
        for (auto& scope : liftLaunchScopes_)
            scope = MeshEffectManager::GetInstance()->CreateEffectScope();
        for (auto& scope : vortexScopes_)
            scope = MeshEffectManager::GetInstance()->CreateEffectScope();
    }

    ~WindSlimeCopyAbility() override {
        // コピー能力の破棄だけでも竜巻のメッシュ演出を止める。
        ClearTornadoEffects();
        ClearWindEffects();
    }

    const char* GetStateLabel() const override {
        // 同時に状態が進んでいるときは、風跳び、小型竜巻、左クリックの竜巻の順に表示を優先する。
        // 風跳びで上向きの移動を続けている状態。
        if (liftPhase_ == LiftPhase::kRising) return "風跳び・上昇";
        // 風跳びで通常より落下が遅くなっている状態。
        if (liftPhase_ == LiftPhase::kGliding) return "風跳び・緩降下";
        // 小型竜巻が進行中か射程端で滞留している状態。
        if (VortexCanHit()) return "小型竜巻を展開中";
        // プレイヤー中心の竜巻が発動している状態。
        if (tornadoActive_) return "竜巻攻撃";
        return tornadoCooldown_ > 0.0f ? "竜巻・復帰待ち" : "風能力";
    }

    void ProcessInput(Player& player, const PlayerCopyAbilityInput& input) override {
        // 同時入力は左・右・Eの順で一つを選び、風跳びから左攻撃へ即座に接続する。
        primaryHeld_ = input.primaryHeld;
        if (recovery_.Active()) return; // 吹き飛ばし・射出直後の隙を風跳びで消さない。
        // 左入力が新たに押され、竜巻が停止中で再使用可能なら、風跳びを終えて左竜巻を始める。
        if (input.primaryTriggered && tornadoCooldown_ <= 0.0f && !tornadoActive_) {
            EndWindLift(player);
            actionMotionTimer_ = 0.0f;
            tornadoActive_ = true;
            tornadoTime_ = 0.0f;
            effectTimer_ = pushTimer_ = 0.0f;
            player.PlaySlimeAbilityMotion("player_ability_wind_tornado", true, 1.0f, 0.04f, 0.10f);
        // 右入力が新たに押され、風跳びが未発動で再使用可能かつ上昇していない接地中なら、踏み切る。
        } else if (input.secondaryTriggered && liftPhase_ == LiftPhase::kIdle && liftCooldown_ <= 0.0f &&
                   player.IsGrounded() && player.GetVelocity().y <= 0.0f) {
            BeginWindLift(player);
        // E入力が新たに押され、小型竜巻が消散まで終えて再使用可能なら、新しい小型竜巻を出す。
        } else if (input.specialTriggered && vortexPhase_ == VortexPhase::kIdle && vortexCooldown_ <= 0.0f) {
            BeginVortex(player);
        }
    }

    PlayerAttackMovement GetAttackMovement() const override {
        // 本体竜巻の持続中と反動中だけ水平移動を弱め、緩降下の鉛直制限と組み合わせる。
        auto movement = recovery_.Movement();
        if (!movement.enabled && tornadoActive_)
            movement = CopyAttackMovement(GetProfile(), "player_wind_tornado");
        if (liftPhase_ == LiftPhase::kGliding) {
            movement.maxFallSpeedOverride = liftSettings_.glideFallSpeed;
        }
        return movement;
    }

    void NotifyDamageStarted(Player& player) override {
        // 被弾の速度を能力が上書きしないよう、反動・風跳び・両方の竜巻を止める。
        recovery_.Clear();
        actionMotionTimer_ = 0.0f;
        EndWindLift(player);
        StopTornado(player);
        StopVortex();
    }

    // 同時に進む技の時計を更新し、風跳びと小型竜巻の後にプレイヤー中心の竜巻を進める。
    void Update(Player& player, float deltaTime) override {
        // 正の経過秒がなければ、技の時計も判定も進めない。
        if (deltaTime <= 0.0f) return;
        recovery_.Update(deltaTime);
        liftCooldown_ = std::max(0.0f, liftCooldown_ - deltaTime);
        vortexCooldown_ = std::max(0.0f, vortexCooldown_ - deltaTime);
        tornadoCooldown_ = std::max(0.0f, tornadoCooldown_ - deltaTime);
        effectTimer_ -= deltaTime;
        pushTimer_ -= deltaTime;
        actionMotionTimer_ = std::max(0.0f, actionMotionTimer_ - deltaTime);
        // 風跳びと前へ進む竜巻は先に更新する。左クリックの竜巻だけ発動後のモーション待ちの影響を受ける。
        UpdateWindLift(player, deltaTime);
        UpdateVortex(player, deltaTime);
        // 終了モーションの残り時間がある間は、左クリックの竜巻の追加処理だけを省く。
        if (actionMotionTimer_ > 0.0f) return;

        // 左クリックの竜巻が停止中なら、下の描画と巻き込みを行わない。
        if (!tornadoActive_)
            return;
        tornadoTime_ += deltaTime;
        const engine::Vector3 center = player.GetWorldPosition() + engine::Vector3{0.0f, 1.0f, 0.0f};

        // 最低0.20秒の発動後にボタンを離すか、最大1.60秒に達した場合は吹き飛ばし判定で締める。
        if ((!primaryHeld_ && tornadoTime_ >= 0.20f) || tornadoTime_ >= 1.60f) {
            // 終了時だけ外へ吹き飛ばす。持続中は弱く巻き込み、攻撃範囲から逃がさない。
            DamageSphere(player, center, 3.3f, 8.0f, DamageType::kPhysical, 13.0f, 6.0f);
            if (!tornadoActive_) return; // 同期ダメージ通知で終了した場合は演出を再開しない。
            StopTornado(player);
            SpawnEffect("Resources/json/effect/player/copy/wind/effect_player_wind_tornado_finish.json",
                        player.GetWorldPosition() + engine::Vector3{0.0f, 0.25f, 0.0f}, {0.0f, tornadoTime_ * 15.0f, 0.0f});
            tornadoCooldown_ = 0.65f;
            actionMotionTimer_ = 0.24f;
            recovery_.Start(CopyAttackMovement(GetProfile(), "player_wind_tornado_recovery"));
            player.TriggerSlimeImpulse({1.7f, 0.55f, 1.7f}, 0.15f);
            return;
        }

        tornadoVisualAge_ += deltaTime;
        // 前回の見た目を採取してから間隔が経ったら、姿勢と位置の基準を取り直す。
        if (effectTimer_ <= 0.0f) {
            // 従来の発生間隔で姿勢とUVの起点を更新し、生成し直した見た目を保ちます。
            tornadoVisualAge_ = 0.0f;
            tornadoSampleTime_ = tornadoTime_;
            tornadoSamplePosition_ = player.GetWorldPosition();
            effectTimer_ = 0.045f;
        }

        // 外周と内周はそれぞれの高さと向きで再生し、風が互いに逆方向へ回るようにする。
        auto* effects = MeshEffectManager::GetInstance();
        for (size_t i = 0; i < tornadoScopes_.size(); ++i) {
            const bool inner = i >= 5;
            const float layer = static_cast<float>(inner ? i - 5 : i);
            // まだ素材を開始していない初回の描画でだけ、各スコープに外側または内側の素材を割り当てる。
            if (!tornadoVisualsReady_)
                effects->StartTimelineEffect(tornadoScopes_[i], inner
                    ? "Resources/json/effect/shared/effect_player_wind_tornado_inner.json"
                    : "Resources/json/effect/shared/effect_player_wind_tornado.json");

            const float radius = inner ? 1.6f + layer * 0.4f : 1.10f + layer * 0.45f;
            const auto position = tornadoSamplePosition_ + engine::Vector3{
                0.0f, inner ? 0.45f + layer * 0.75f : 0.18f + layer * 0.64f, 0.0f};
            const engine::Vector3 rotation = inner
                ? engine::Vector3{0.15f, -tornadoSampleTime_ * 23.0f + layer * kPi, 0.08f}
                : engine::Vector3{0.0f, tornadoSampleTime_ * 18.0f + layer * 1.8f, 0.0f};
            // 負の寿命指定ではJSONの寿命を使い、設定したスケールアニメーションも残します。
            effects->EvaluateTimelineEffect(tornadoScopes_[i], tornadoVisualAge_, -1.0f,
                position, rotation, {radius, inner ? 1.0f : 0.95f, radius}, true);
        }
        tornadoVisualsReady_ = true;

        // 持続中は終了時より弱い判定で、対象を竜巻の中心へ引き戻す。
        // 巻き込みの再判定間隔を満たしたら、終了時より弱い力で中心へ寄せる。
        if (pushTimer_ <= 0.0f) {
            DamageSphere(player, center, 3.0f, 3.0f, DamageType::kPhysical, -2.0f, 1.0f);
            // 同期ダメージ通知を受けて竜巻が終了した場合は、巻き込み間隔を設定し直さず戻る。
            if (!tornadoActive_) return;
            pushTimer_ = 0.20f;
        }
    }

    // コピー終了時は動作とエフェクトを停止し、前の押下状態を次の処理へ持ち越さない。
    void Cancel(Player& player) override {
        EndWindLift(player);
        StopTornado(player);
        StopVortex();
        ClearWindEffects();
        recovery_.Clear();
        player.ClearSlimeAbilityMotion();
        primaryHeld_ = false;
        actionMotionTimer_ = 0.0f;
        player.SetSlimeAbilityPose(PlayerSlimeAnimator::AbilityPose::kNone);
    }

  private:
    // 接地中に一度だけ鉛直初速を与え、その後は通常の重力と衝突解決へ任せる。
    void BeginWindLift(Player& player) {
        StopTornado(player);
        liftSettings_ = WindCopySettings::Get();
        liftSettings_.Sanitize();
        liftPhase_ = LiftPhase::kRising;
        liftGlideRemaining_ = liftSettings_.glideDuration;
        liftVfxTimer_ = 0.0f;
        liftAge_ = 0.0f;
        actionMotionTimer_ = 0.0f;

        const auto position = player.GetWorldPosition();
        // 頭上に足場がある場合も、突風を接地中の足元から出す。
        const engine::Vector3 base{position.x, player.GetAabb().min.y + 0.07f, position.z};
        // 水平速度は踏み切り時だけゼロにし、次の更新から通常の空中操作に戻す。
        player.SetVelocity({0.0f, player.GetJumpPower() * liftSettings_.liftJumpScale, 0.0f});
        player.SetGrounded(false);
        player.StartEvasionInvincibility(liftSettings_.invincibleTime);
        player.PlaySlimeAbilityMotion("player_ability_wind_lift", false, 1.0f, 0.0f, 0.12f);
        SpawnScopedEffect(windScope_, "Resources/json/effect/player/copy/wind/effect_player_wind_lift_ring.json", base);
        StartWindLaunch(player, base);
        EmitPreset("player/copy/wind/player_wind_lift_burst", base);
    }

    void EndWindLift(Player& player) {
        // 風跳び中でなければ、再使用待ちやプレイヤーのモーションを変更しない。
        if (liftPhase_ == LiftPhase::kIdle) return;
        // 能力が確保した姿勢と風だけを解除し、下降速度は通常の移動処理へ戻す。
        liftPhase_ = LiftPhase::kIdle;
        liftCooldown_ = liftSettings_.liftCooldown;
        StopWindLaunch();
        player.StopSlimeAbilityMotion(0.12f);
        player.SetSlimeAbilityPose(PlayerSlimeAnimator::AbilityPose::kNone);
        // メッシュ演出の管理が使える場合は、踏み切りリングと緩降下の風もまとめて停止する。
        if (auto* effects = MeshEffectManager::GetInstance()) effects->StopEffectScope(windScope_);
    }

    // 物理更新後のY速度で頂点を検出する。天井で上向き速度が止まった時も下降へ移る。
    void UpdateWindLift(Player& player, float frameDeltaTime) {
        // 風跳びが始まっていないか終了済みの場合は、姿勢と表示を更新しない。
        if (liftPhase_ == LiftPhase::kIdle) return;
        const float verticalSpeed = player.GetVelocity().y;
        const bool landed = player.IsGrounded() && verticalSpeed <= 0.0f;
        // 緩降下中に足場などで鉛直速度が上向きになった場合も、以前の緩降下を解除する。
        const bool bounced = liftPhase_ == LiftPhase::kGliding && verticalSpeed > 0.0f;
        // 接地して上昇していないか、緩降下から跳ね返った場合に風跳びを終了する。
        if (landed || bounced) {
            EndWindLift(player);
            return;
        }

        player.ForceSlimeAnimationModeForNextUpdate(PlayerSlimeAnimator::Mode::kJump, player.GetForwardDirection());
        // 踏み切り後の上昇段階を処理し、鉛直速度が止まった時点で緩降下へ切り替える。
        if (liftPhase_ == LiftPhase::kRising) {
            // まだ上向きの間は離陸時の風を更新し、緩降下の時計を進めない。
            if (verticalSpeed > 0.0f) {
                UpdateWindLaunch(player, frameDeltaTime);
                return;
            }
            // 頂点へ達したら離陸の風帯を止め、緩降下の表示の起点を作る。
            StopWindLaunch();
            liftPhase_ = LiftPhase::kGliding;
            liftVfxTimer_ = 0.0f;
            liftAge_ = 0.0f;
            player.StopSlimeAbilityMotion(0.12f);
        // 既に緩降下中なら、初回に確保した緩降下時間を消費する。
        } else {
            liftGlideRemaining_ -= frameDeltaTime;
            // 緩降下の残り時間を使い切ったら、着地前でも落下上限の指定を外す。
            if (liftGlideRemaining_ <= 0.0f) {
                EndWindLift(player);
                return;
            }
        }

        // 緩降下の開始と終了で姿勢を滑らかに切り替え、左右の風筋を別の時計で発生させる。
        liftAge_ += frameDeltaTime;
        const float poseStrength = std::min(SmoothStep(liftAge_ / 0.16f), SmoothStep(liftGlideRemaining_ / 0.22f));
        player.SetSlimeAbilityPose(PlayerSlimeAnimator::AbilityPose::kSlowFall, poseStrength);

        liftVfxTimer_ -= frameDeltaTime;
        // 左右の風筋の発生間隔にまだ届かない場合は、緩降下の姿勢だけを更新して戻る。
        if (liftVfxTimer_ > 0.0f) return;
        liftVfxTimer_ = 0.14f;
        const auto bounds = player.GetModelWorldAabb();
        const auto center = (bounds.min + bounds.max) * 0.5f;
        const float radius = std::max(0.42f, (bounds.max.x - bounds.min.x) * 0.52f);
        const float yaw = player.GetMoveYaw() + liftAge_ * 2.4f;
        const engine::Vector3 offset{std::cos(yaw) * radius, -0.38f, std::sin(yaw) * radius};
        const engine::Vector3 opposite{-offset.x, offset.y, -offset.z};
        const engine::Vector3 up{0.0f, 1.0f, 0.0f};
        const char* ribbon = "Resources/json/effect/player/copy/wind/effect_player_wind_lift_glide.json";
        SpawnScopedEffect(windScope_, ribbon, center + offset, {0.0f, -yaw, 0.0f});
        SpawnScopedEffect(windScope_, ribbon, center + opposite, {0.0f, -yaw + kPi, 0.0f});
        EmitDirectedPreset("player/copy/wind/player_wind_lift_glide", center + offset, up);
        EmitDirectedPreset("player/copy/wind/player_wind_lift_glide", center + opposite, up);
    }

    // 左クリックの外周と内周のプリセットを、命中判定を持たない離陸演出として再生する。
    void StartWindLaunch(const Player& player, const engine::Vector3& base) {
        liftLaunchBase_ = base;
        liftLaunchStartY_ = player.GetTranslate().y;
        liftLaunchAge_ = 0.0f;
        // 描画の管理が利用できる場合だけ、踏み切りの位置を基準に全レイヤーを開始する。
        if (auto* effects = MeshEffectManager::GetInstance()) {
            for (std::size_t i = 0; i < liftLaunchScopes_.size(); ++i) {
                const bool inner = i == liftLaunchScopes_.size() - 1;
                effects->StartTimelineEffect(liftLaunchScopes_[i], inner
                    ? "Resources/json/effect/shared/effect_player_wind_tornado_inner.json"
                    : "Resources/json/effect/shared/effect_player_wind_tornado.json");
            }
        }
    }

    // 根元を踏み切り位置に残し、上側の風帯を実際に上がった高さへ追従させる。
    void UpdateWindLaunch(const Player& player, float frameDeltaTime) {
        // 立ち上りの演出が終わっていれば更新せず、終点を跨いだフレームは全レイヤーを停止する。
        // 離陸演出が停止済みであれば、描画時間を進めない。
        if (liftLaunchAge_ >= kLiftLaunchDuration) return;
        // 今回の時間で終了を跨ぐ場合は、途中の姿勢を描く代わりに全レイヤーを停止する。
        if (liftLaunchAge_ + frameDeltaTime >= kLiftLaunchDuration) {
            StopWindLaunch();
            return;
        }
        liftLaunchAge_ += frameDeltaTime;

        auto* effects = MeshEffectManager::GetInstance();
        // 描画の管理が利用できない場合は、時刻の更新だけで戻る。
        if (!effects) return;
        // 開始時の床からどこまで上がったかを使い、上昇風の高さと消え方を調整する。
        const float climb = std::clamp(player.GetTranslate().y - liftLaunchStartY_, 0.0f, 5.4f);
        const float burst = SmoothStep(liftLaunchAge_ / 0.070f);
        const float dissipate = SmoothStep((kLiftLaunchDuration - liftLaunchAge_) / 0.12f);
        const float envelope = (0.65f + 0.35f * burst) * dissipate;
        constexpr std::array<float, 4> kHeightFollow{0.0f, 0.29f, 0.66f, 0.54f};
        constexpr std::array<float, 4> kRadius{2.10f, 1.80f, 1.48f, 1.12f};
        constexpr std::array<float, 4> kHeight{1.08f, 1.72f, 1.70f, 1.42f};
        // 各風の層を別々に上昇・回転させ、内側だけ逆回転にする。
        for (std::size_t i = 0; i < liftLaunchScopes_.size(); ++i) {
            const bool inner = i == liftLaunchScopes_.size() - 1;
            const float layer = static_cast<float>(i);
            const float radius = kRadius[i] * envelope;
            const auto position = liftLaunchBase_ + engine::Vector3{
                0.0f, kHeightFollow[i] * climb + layer * 0.14f, 0.0f};
            const float spin = liftLaunchAge_ * (inner ? -25.0f : 22.0f) + layer * 1.65f;
            effects->EvaluateTimelineEffect(liftLaunchScopes_[i], liftLaunchAge_, kLiftLaunchDuration,
                position, {0.0f, spin, 0.0f}, {radius, kHeight[i] * envelope, radius}, true);
        }
    }

    // 踏み切りの風帯を停止し、次の呼出元が更新済みか判断できるよう時計を終端に置く。
    void StopWindLaunch() {
        // 既に終端に達している場合は停止要求を重ねない。
        if (liftLaunchAge_ >= kLiftLaunchDuration) return;
        liftLaunchAge_ = kLiftLaunchDuration;
        // 管理が利用できる場合だけ、確保した全レイヤーを停止する。
        if (auto* effects = MeshEffectManager::GetInstance())
            for (const auto scope : liftLaunchScopes_)
                effects->StopEffectScope(scope);
    }

    void BeginVortex(Player& player) {
        // この発動だけの設定と発射点を確定し、以前の竜巻で命中した敵を引き継がない。
        vortexSettings_ = WindCopySettings::Get(); vortexSettings_.Sanitize();
        vortexDirection_ = NormalizePlanar(player.GetLockOnAttackDirection());
        vortexPosition_ = player.GetWorldPosition() + engine::Vector3{0,1.0f,0};
        vortexPhase_ = VortexPhase::kTraveling;
        vortexPhaseRemaining_ = vortexSettings_.vortexLifetime;
        vortexAge_ = vortexVfxTimer_ = 0.0f;
        vortexNextHit_.clear();
        vortexCooldown_ = vortexSettings_.vortexCooldown;
        recovery_.Start(CopyAttackMovement(GetProfile(), "player_wind_vortex"), false);
        // 本体の縮みと足元の風を、離れて進む竜巻の起点にする。
        player.TriggerSlimeImpulse({1.22f, 0.72f, 1.22f}, 0.13f);
        auto* effects = MeshEffectManager::GetInstance();
        // 外側の帯、逆回転する内側、専用の芯を別素材として開始する。
        for (std::size_t layer = 0; layer < vortexScopes_.size(); ++layer) {
            const char* path = "Resources/json/effect/player/copy/wind/effect_player_wind_vortex_core.json";
            // 外側のレイヤーは共通の外周プリセットを使う。
            if (layer < kVortexOuterLayers)
                path = "Resources/json/effect/shared/effect_player_wind_tornado.json";
            // 外側の直後のレイヤーは共通の内側用を使い、最後の芯は初期値の専用素材を使う。
            else if (layer == kVortexOuterLayers)
                path = "Resources/json/effect/shared/effect_player_wind_tornado_inner.json";
            effects->StartTimelineEffect(vortexScopes_[layer], path);
        }
        EmitDirectedPreset("shared/wind_slime_charge", vortexPosition_ - engine::Vector3{0.0f, 0.65f, 0.0f},
                           vortexDirection_ + engine::Vector3{0.0f, 0.65f, 0.0f}, 0.65f);
        EmitDirectedPreset("shared/wind_slime_breath_stream", vortexPosition_, vortexDirection_, 0.75f);
    }

    // 被弾や解除では、飛行中・消散中にかかわらず表示を即座に回収する。
    void StopVortex() {
        vortexPhase_ = VortexPhase::kIdle;
        vortexPhaseRemaining_ = 0.0f;
        vortexNextHit_.clear();
        // 描画の管理が使える場合だけ、飛行用と消散用の風帯をすべて停止する。
        if (auto* effects = MeshEffectManager::GetInstance()) {
            for (auto scope : vortexScopes_)
                effects->StopEffectScope(scope);
        }
    }

    // 移動中と滞留中は攻撃可能。待機・消散中は見た目が残っていても判定しない。
    bool VortexCanHit() const {
        return vortexPhase_ == VortexPhase::kTraveling || vortexPhase_ == VortexPhase::kHolding;
    }

    // 壁では滞留を省き、最後の有効座標で描画だけを残す。
    void BeginVortexDissipation(bool hitWall) {
        vortexPhase_ = VortexPhase::kDissipating;
        vortexPhaseRemaining_ = kVortexDissipationDuration;
        const auto direction = hitWall ? engine::Vector3{0.0f, 1.0f, 0.0f} : vortexDirection_;
        EmitDirectedPreset("shared/wind_slime_breath_stream", vortexPosition_, direction, 0.65f);
    }

    // 判定中心にクリスタル由来の風帯とE専用の芯を重ねる。releaseは消散の進行率（0～1）。
    void DrawVortexLayers(float release) {
        auto* effects = MeshEffectManager::GetInstance();
        // 消える間も回転を止めず、消失の進捗に合わせて帯の半径と高さを落とす。
        const float spinTime = vortexAge_ + release * kVortexDissipationDuration;
        const float dissolve = SmoothStep(release);
        const float height = vortexSettings_.vortexVisualHeight;
        const float width = vortexSettings_.vortexRadius * vortexSettings_.vortexVisualWidth;
        // 移動時間が終わってから中心へ少し絞り、消失の収縮と合わせて見せる。
        const float sinceArrival = std::max(0.0f, vortexAge_ - vortexSettings_.vortexLifetime);
        const float tighten = 1.0f - 0.16f * SmoothStep(sinceArrival / 0.10f);
        const float shrink = (1.0f + 0.28f * dissolve) * (1.0f - dissolve);

        // 外周・内側・芯を別々に成長させ、半径と出現時刻に差を付ける。
        for (std::size_t layer = 0; layer < vortexScopes_.size(); ++layer) {
            const bool core = layer == kVortexOuterLayers + 1;
            const bool inner = layer == kVortexOuterLayers;
            const float level = (inner || core) ? 0.5f : static_cast<float>(layer) / (kVortexOuterLayers - 1);
            const float grow = std::max(0.01f, SmoothStep((vortexAge_ - 0.065f * level) / 0.18f));
            // 外周は層の高さに応じて広がり、芯と内側の帯は決まった幅・高さを使う。
            float radiusScale = 0.48f + level * 0.47f;
            float heightScale = 0.53f;
            if (core) {
                radiusScale = 0.52f;
                heightScale = 0.66f;
            } else if (inner) {
                radiusScale = 0.68f;
                heightScale = 0.69f;
            }
            const float radius = width * radiusScale * grow * tighten * shrink;
            const float lift = height * (0.15f + level * 0.67f) * grow +
                               dissolve * (0.12f + level * 0.32f);
            const auto position = vortexPosition_ + engine::Vector3{0.0f, lift - 1.0f, 0.0f};
            // 帯ごとの向きにずれを付け、内側だけ反対に速く回す。
            const float yaw = spinTime * vortexSettings_.vortexSpinRate * (inner ? -1.35f : 1.0f) +
                              static_cast<float>(layer) * 1.65f;
            const float bandHeight = height * heightScale * grow * (1.0f - dissolve);
            effects->EvaluateTimelineEffect(vortexScopes_[layer], spinTime, 5.0f,
                position, {0.0f, yaw, dissolve * 0.32f}, {radius, bandHeight, radius});
        }
    }

    // 小型竜巻の移動と遮蔽は地形だけを調べ、発動したプレイヤーを対象から外す。
    engine::PhysicsQueryFilter MakeVortexTerrainFilter(Player& player) const {
        engine::PhysicsQueryFilter terrain;
        terrain.mask = engine::collisionCategories::Get("ground");
        terrain.ignoredObject = &player;
        return terrain;
    }

    // ワールド距離distanceだけ進む前に壁を調べる。停止した場合はfalseを返す。
    bool AdvanceVortex(Player& player, float distance) {
        auto wall = MakeVortexTerrainFilter(player);
        wall.refineSphereCastHit = true;
        wall.acceptsSphereContact = [](const engine::Object3D*, const engine::CollisionInfo& c) {
            return std::abs(c.normal.y) < 0.65f;
        };
        auto* collisions = engine::CollisionManager::GetInstance();
        // 進路上で壁向きの法線に当たった場合は座標を進めず、呼出元に停止を知らせる。
        if (collisions->SphereCast(vortexPosition_, 0.4f, vortexDirection_, distance, wall).isHit)
            return false;

        // 進める場合は移動先の床を調べ、坂や段差で竜巻が床に沿うよう中心の高さを合わせる。
        vortexPosition_ += vortexDirection_ * distance;
        const auto floor = collisions->Raycast(
            vortexPosition_ + engine::Vector3{0,0.5f,0}, {0,-1,0}, 2.0f, wall);
        // 下向きのレイが床として使える上向き法線に当たったら、接地点より1上へ合わせる。
        if (floor.isHit && floor.normal.y > 0.65f)
            vortexPosition_.y = floor.hitPoint.y + 1.0f;
        return true;
    }

    // 実体の型でボスを除外し、既存のボス分類と封印結晶も引き寄せ対象から外す。
    static bool CanPullVortexTarget(const BaseEnemy& target) {
        // 実体が四種のボスのいずれかなら、タイプ名の設定にかかわらず引き寄せを無効にする。
        if (dynamic_cast<const EnemyGiantSlime*>(&target) || dynamic_cast<const EnemyPrismSlime*>(&target) ||
            dynamic_cast<const EnemyMagmaSlime*>(&target) || dynamic_cast<const EnemyFalseKingSlime*>(&target))
            return false;
        const auto& type = target.GetEnemyType();
        return type != "GiantSlime" && type != "PrismSlime" && type != "MagmaSlime" &&
               type != "FalseKingSlime" && type != "FalseKingSeal";
    }

    // 範囲内にいる間は短い外力を更新し、離れた後はMotor側で減衰させる。
    void ApplyVortexPull(BaseEnemy& enemy, float pullScale) const {
        // 待機中の敵や、動かせないボス・封印結晶に対して外力は与えない。
        if (enemy.IsDormant() || !CanPullVortexTarget(enemy)) return;

        auto offset = vortexPosition_ - enemy.GetWorldPosition();
        offset.y = 0.0f;
        const float distance = engine::Math::Length(offset);
        const float speed = vortexSettings_.vortexPull * pullScale;
        constexpr float kImpulseSeconds = 0.075f;
        // 引く設定が無効、または中心から0.10以内なら、方向の正規化と外力を省く。
        if (speed <= 0.0f || distance <= 0.10f) return;

        const auto horizontal = offset * (std::min(speed, distance / kImpulseSeconds) / distance);
        enemy.ApplyExternalImpulse({horizontal.x, enemy.GetVelocity().y, horizontal.z}, kImpulseSeconds);
    }

    // 足元から風帯上端までを円柱で照会し、同じ敵の複数Colliderは一体分として処理する。
    void UpdateVortexContacts(Player& player, float pullScale) {
        const float radius = vortexSettings_.vortexRadius;
        const float bottom = vortexPosition_.y - 1.0f;
        const float height = vortexSettings_.vortexVisualHeight;
        const auto center = engine::Vector3{vortexPosition_.x, bottom + height * 0.5f, vortexPosition_.z};
        const auto extent = engine::Vector3{radius, height * 0.5f, radius};
        Transform transform;
        transform.translate = center;
        transform.UpdateMatrix();
        engine::Collider cylinder(&transform);
        engine::ColliderConfig config;
        config.type = engine::ColliderType::kCylinder;
        config.size = extent;
        cylinder.SetConfig(config);

        auto* collisions = engine::CollisionManager::GetInstance();
        const auto wall = MakeVortexTerrainFilter(player);
        engine::PhysicsQueryFilter enemies;
        enemies.mask = engine::collisionCategories::Get("enemy");
        enemies.ignoredObject = &player;
        // まず外接箱で候補を絞り、円柱の当たり形状と壁の遮蔽を通った敵だけ処理する。
        std::unordered_set<BaseEnemy*> processed;
        for (const auto& hit : collisions->OverlapAabb({center - extent, center + extent}, enemies)) {
            auto* enemy = static_cast<BaseEnemy*>(FindEnemyRoot(hit.object));
            // 敵本体が有効かつ未処理で、生存して運搬されておらず、円柱にも当たる候補だけを扱う。
            if (!enemy || processed.contains(enemy) || enemy->IsDead() ||
                enemy->IsCarried() || !enemy->GetParameters() || enemy->GetParameters()->hp <= 0.0f ||
                !cylinder.CheckCollision(hit.object->GetCollider()).isColliding)
                continue;

            // 触れたColliderの高さを使い、低い敵や空中の敵に根元から誤って射線を引かない。
            const auto bounds = hit.object->GetCollider()->GetAabb();
            // 箱形Colliderに対する円柱近似で、渦の横幅を外れた角に当たらないようにする。
            const float dx = std::clamp(vortexPosition_.x, bounds.min.x, bounds.max.x) - vortexPosition_.x;
            const float dz = std::clamp(vortexPosition_.z, bounds.min.z, bounds.max.z) - vortexPosition_.z;
            // 外接箱上の最近接点が水平半径の外にあれば、箱の角だけに当たる候補を除く。
            if (dx * dx + dz * dz > radius * radius) continue;
            auto aim = (bounds.min + bounds.max) * 0.5f;
            aim.y = std::clamp(vortexPosition_.y, bounds.min.y, bounds.max.y);
            const auto offset = aim - vortexPosition_;
            const float length = engine::Math::Length(offset);
            // 中心と接触部に十分な距離があり、その途中が地形に遮られる場合は敵へ届かない。
            if (length > 0.001f &&
                collisions->Raycast(vortexPosition_, offset * (1.0f / length), length, wall).isHit)
                continue;

            processed.insert(enemy);
            ApplyVortexPull(*enemy, pullScale);
            // ダメージ量が非正なら引き寄せだけを行い、命中時刻を更新しない。
            if (vortexSettings_.vortexDamage <= 0.0f) continue;

            // 初めて触れた敵は即時命中。飛行から滞留へ移っても再命中時刻は持ち越す。
            auto& nextHit = vortexNextHit_[enemy];
            // 同じ敵を再び攻撃できる時刻に達するまでは、引き寄せだけを維持する。
            if (vortexAge_ < nextHit) continue;
            nextHit = vortexAge_ + vortexSettings_.vortexHitInterval;
            DamageEnemy(player, enemy, vortexSettings_.vortexDamage, DamageType::kPhysical, {});
            // 同期通知で被弾やコピー解除が起きた場合は、次の対象を処理しない。
            if (!VortexCanHit()) return;
        }
    }

    // らせんの放出点から上向きの風筋を流し、滞留中は根元から巻き上げる。
    void EmitVortexWind(float frameDeltaTime) {
        vortexVfxTimer_ -= frameDeltaTime;
        // 前回から発生間隔に届いていなければ、新しい風筋を出さない。
        if (vortexVfxTimer_ > 0.0f) return;
        const bool holding = vortexPhase_ == VortexPhase::kHolding;
        const float angle = vortexAge_ * vortexSettings_.vortexSpinRate;
        const float radius = vortexSettings_.vortexRadius * vortexSettings_.vortexVisualWidth *
                             (holding ? 0.46f : 0.72f);
        const float height = vortexSettings_.vortexVisualHeight;
        for (std::int32_t i = 0; i < 2; ++i) {
            const float azimuth = angle + i * 3.14159265f;
            const float lift = holding ? 0.2f : 0.2f + height * std::fmod(vortexAge_ * 1.7f + i * 0.5f, 1.0f);
            const auto position = vortexPosition_ + engine::Vector3{
                std::cos(azimuth) * radius, lift - 1.0f, std::sin(azimuth) * radius};
            EmitDirectedPreset("player/copy/wind/player_wind_vortex_rise", position,
                               {-std::sin(azimuth), 1.65f, std::cos(azimuth)}, holding ? 1.1f : 0.9f);
        }
        vortexVfxTimer_ = holding ? 0.040f : 0.060f;
    }

    void UpdateVortex(Player& player, float frameDeltaTime) {
        // 段階の終端を跨がない刻みで進め、余った時間を次の段階へ渡す。
        float remaining = frameDeltaTime;
        float visualDt = 0.0f;
        while (remaining > 0.0f && vortexPhase_ != VortexPhase::kIdle) {
            const float step = std::min({remaining, vortexPhaseRemaining_, 1.0f / 60.0f});
            remaining -= step;
            vortexPhaseRemaining_ -= step;

            // 消散中は残り時間だけを進め、敵を巻き込む判定には入らない。
            if (vortexPhase_ == VortexPhase::kDissipating) {
                // 消散の残り時間がなくなったら判定状態を待機へ戻し、風帯を停止する。
                if (vortexPhaseRemaining_ <= 0.0f) StopVortex();
                continue;
            }

            const bool traveling = vortexPhase_ == VortexPhase::kTraveling;
            // 飛行先が壁に塞がれていればその位置で消散を始め、射程端の滞留を省く。
            if (traveling && !AdvanceVortex(player, vortexSettings_.vortexSpeed * step)) {
                BeginVortexDissipation(true);
                break;
            }

            vortexAge_ += step;
            visualDt += step;
            UpdateVortexContacts(player, traveling ? 1.0f : vortexSettings_.vortexHoldPullScale);
            // 同期ダメージ通知によって判定状態が解除された場合、以後の移動と描画を行わない。
            if (!VortexCanHit()) return;

            // 射程に達した場合だけ、設定された時間だけ滞留してから消える。
            if (vortexPhaseRemaining_ <= 0.0f) {
                // 正常な飛行の終端に達し、滞留時間が正のときだけ射程端で判定を維持する。
                if (traveling && vortexSettings_.vortexHoldDuration > 0.0f) {
                    vortexPhase_ = VortexPhase::kHolding;
                    vortexPhaseRemaining_ = vortexSettings_.vortexHoldDuration;
                    vortexVfxTimer_ = 0.0f;
                // 滞留を設定していない場合と滞留が終了した場合は、判定を止め消散に切り替える。
                } else {
                    BeginVortexDissipation(false);
                }
            }
        }

        // 当たり判定のある段階では風を追加し、消散へ移ったら既存の風帯を縮める。
        if (VortexCanHit()) {
            DrawVortexLayers(0.0f);
            EmitVortexWind(visualDt);
        // 消散中は既にある風帯だけを残り時間に応じて絞り、新規の風筋を出さない。
        } else if (vortexPhase_ == VortexPhase::kDissipating) {
            DrawVortexLayers(1.0f - vortexPhaseRemaining_ / kVortexDissipationDuration);
        }
    }

    // 風跳びの離陸帯と小型竜巻、その他の風跳び用表示を回収する。
    void ClearWindEffects() {
        StopWindLaunch();
        StopVortex();
        // 管理が残っている場合は、踏み切りの輪や緩降下の風を保持するスコープも閉じる。
        if (auto* effects = MeshEffectManager::GetInstance()) effects->StopEffectScope(windScope_);
    }

    void StopTornado(Player& player) {
        // 竜巻の停止とモーションの停止を同時に行い、再使用待ちへ入れる。
        // 竜巻が発動している場合だけ停止し、再使用時間とモーションの終了を設定する。
        if (tornadoActive_) {
            tornadoActive_ = false;
            tornadoCooldown_ = 0.65f;
            ClearTornadoEffects();
            player.StopSlimeAbilityMotion(0.10f);
        }


    }

    void ClearTornadoEffects() {
        // 能力専用スコープを閉じ、他のプレイヤー演出へ影響させない。
        // 描画管理を取得できる場合だけ、七つの竜巻レイヤーを停止する。
        if (auto* effects = MeshEffectManager::GetInstance())
            for (auto scope : tornadoScopes_)
                effects->StopEffectScope(scope);
        tornadoVisualsReady_ = false;
        tornadoVisualAge_ = 0.0f;
    }

    std::array<MeshEffectManager::EffectScopeId, 7> tornadoScopes_{};
    // 一定間隔で採取する見た目の起点と、そこから次の採取までの経過秒数。
    engine::Vector3 tornadoSamplePosition_{};
    float tornadoSampleTime_ = 0.0f;
    float tornadoVisualAge_ = 0.0f;
    bool tornadoVisualsReady_ = false;
    bool tornadoActive_ = false;
    float tornadoTime_ = 0.0f;
    float tornadoCooldown_ = 0.0f;
    WindCopySettings liftSettings_{}, vortexSettings_{}; // 発動時の設定。Editor操作中も実行中の値を固定する。
    static constexpr std::size_t kVortexOuterLayers = 3;
    static constexpr float kVortexDissipationDuration = 0.22f; // 判定終了から表示を回収するまでの秒数。
    enum class VortexPhase { kIdle, kTraveling, kHolding, kDissipating };
    VortexPhase vortexPhase_ = VortexPhase::kIdle;
    float vortexPhaseRemaining_ = 0.0f; // 現在の段階の残り秒数。
    std::array<MeshEffectManager::EffectScopeId, kVortexOuterLayers + 2> vortexScopes_{};
    MeshEffectManager::EffectScopeId windScope_{};
    static constexpr float kLiftLaunchDuration = 0.34f; // 打ち上げ竜巻の表示時間（秒）。
    std::array<MeshEffectManager::EffectScopeId, 4> liftLaunchScopes_{};
    engine::Vector3 liftLaunchBase_{}; // ワールド座標の踏み切り位置。
    float liftLaunchStartY_ = 0.0f; // 高さ追従に使う踏み切り時のプレイヤー座標。
    float liftLaunchAge_ = kLiftLaunchDuration; // 表示開始からの秒数。寿命と同値なら停止中。
    engine::Vector3 vortexDirection_{0,0,1}, vortexPosition_{}; // 方向と竜巻中心はワールド座標。
    enum class LiftPhase { kIdle, kRising, kGliding };
    LiftPhase liftPhase_ = LiftPhase::kIdle;
    float liftGlideRemaining_ = 0.0f; // 頂点に達してからの緩降下残り時間（秒）。
    float liftCooldown_ = 0.0f; // 技終了からの再使用待ち（秒）。
    float liftAge_ = 0.0f, liftVfxTimer_ = 0.0f; // 緩降下の経過時間と次の風筋までの秒数。
    float vortexCooldown_ = 0, vortexAge_ = 0, vortexVfxTimer_ = 0; // 小型竜巻の時計（秒）。
    std::unordered_map<const BaseEnemy*, float> vortexNextHit_; // 発動中だけ保持する敵ごとの次回命中時刻（秒）。
    // 巻き込みや表示の更新間隔と、終了モーションで左クリックの竜巻を待たせる残り秒数。
    CopyAttackRecovery recovery_;
    float effectTimer_ = 0, pushTimer_ = 0, actionMotionTimer_ = 0;
    bool primaryHeld_ = false;
};
} // namespace

namespace copyAbilityDetail {
std::unique_ptr<ICopyAbilitySession> CreateWindSlimeCopyAbility(const EnemyAttackProfile& profile) {
    // コピー獲得ごとに風跳びと二種類の竜巻の状態を初期化する。
    return std::make_unique<WindSlimeCopyAbility>(profile);
}
} // namespace copyAbilityDetail
