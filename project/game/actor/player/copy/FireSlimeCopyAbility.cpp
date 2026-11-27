#include <cstdint>
#include "CopyAbilitySupport.h"
#include "CameraManager.h"
#include "Camera.h"
#include "game/actor/combat/AttackSequence.h"
#include <vector>

namespace {
using namespace copyAbilityDetail;

// 炎コピーの入力と継続攻撃を管理し、専用 EffectScope で所有エフェクトをまとめて片付ける。
// PlayerCopyAbilityControllerが所有する炎ブレス・火球の実装。入力はProcessInput、進行はUpdate、一時表示の回収はCancelが担当する。
class FireSlimeCopyAbility final : public ICopyAbilitySession {
  public:
    explicit FireSlimeCopyAbility(EnemyAttackProfile profile) : ICopyAbilitySession(std::move(profile)) {
        // この能力が生成したメッシュ演出を、解除時にまとめて停止できるようスコープを確保する。
        if (MeshEffectManager* effects = MeshEffectManager::GetInstance()) {
            effectScope_ = effects->CreateEffectScope();
        }
    }

    ~FireSlimeCopyAbility() override {
        // セッションの破棄だけでも突進演出が残らないようにする。
        StopDashEffects();
    }

    const char* GetStateLabel() const override {
        // 優先度の高い継続行動を表示し、それ以外は単発攻撃の状態を使う。
        // 突進が継続中なら、短い単発攻撃より優先して突進を表示する。
        if (dashActive_)
            return "炎ダッシュ";
        // 突進しておらずブレスの姿勢を再生しているなら、ブレスを表示する。
        if (breathMotionActive_)
            return "炎ブレス";
        return fireballSequence_.GetStateLabel();
    }

    void SetPreviewAttackId(const std::string& id) override {
        // プレビューでは指定IDの単発攻撃だけを選択候補にする。
        previewAttackId_ = id;
    }

    void ProcessInput(Player& player, const PlayerCopyAbilityInput& input) override {
        // ブレスの押し続け状態を保存し、突進中は火球の開始を受け付けない。
        primaryHeld_ = input.primaryHeld;
        if (player.IsDead() || player.IsCinematicLocked() || recovery_.Active() ||
            actionMotionTimer_ > 0.0f || fireballSequence_.IsRunning())
            return; // 火球の予備動作・後隙を突進や次の火球で取り消さない。

        // 副入力が押されたフレームで、突進しておらず再使用待ちも無ければ炎ダッシュを開始する。
        if (input.secondaryTriggered && !dashActive_ && dashCooldown_ <= 0.0f) {
            BeginDash(player);
        // 炎ダッシュを開始しない場合に限り、必殺入力と火球の再使用条件が揃えば単発攻撃を試す。
        } else if (input.specialTriggered && fireballCooldown_ <= 0.0f && !dashActive_) {
            Fireball(player);
        }
    }

    void Update(Player& player, float deltaTime) override {
        // 単発攻撃、突進、継続ブレスの順に更新し、同時実行を避ける。
        // 死亡または演出拘束中は、火球の時計とブレスの入力を止めて攻撃の更新を打ち切る。
        if (player.IsDead() || player.IsCinematicLocked()) {
            fireballSequence_.Cancel();
            recovery_.Clear();
            actionMotionTimer_ = 0.0f;
            primaryHeld_ = false;
            StopBreathMotion(player);
            return;
        }
        recovery_.Update(deltaTime);

        // 入力を止めている間も発射シーケンスの時計は進め、指定時刻の弾を放つ。
        if (fireballSequence_.IsRunning())
            fireballSequence_.Update(deltaTime, MakeSequenceContext(player));
        breathVisible_ = false;
        UpdateHeadVisual(player, deltaTime);
        fireballCooldown_ = std::max(0.0f, fireballCooldown_ - deltaTime);
        dashCooldown_ = std::max(0.0f, dashCooldown_ - deltaTime);
        breathDamageTimer_ -= deltaTime;
        breathEffectTimer_ -= deltaTime;
        dashEffectTimer_ -= deltaTime;
        dashGroundWakeTimer_ -= deltaTime;
        actionMotionTimer_ = std::max(0.0f, actionMotionTimer_ - deltaTime);

        // 突進中は専用移動と攻撃判定を優先し、この更新ではブレスへ入らない。
        if (dashActive_) {
            UpdateDash(player, deltaTime);
            return;
        }

        // 単発攻撃の姿勢時間が残っている間は、新しくブレスを開始しない。
        if (actionMotionTimer_ > 0.0f || recovery_.Active()) {
            return;
        }

        // 突進・火球の硬直がないフレームだけ、押下の継続に応じてブレスを開始または停止する。
        if (!primaryHeld_) {
            StopBreathMotion(player);
            return;
        }

        // ブレスの姿勢が未再生なら開始を試す。成功するまで再生済みにはしない。
        if (!breathMotionActive_) {
            breathMotionActive_ = player.PlaySlimeAbilityMotion("player_ability_fire_breath", true, 1.0f, 0.05f, 0.08f);
        }

        const engine::Vector3 direction = NormalizePlanar(player.GetLockOnAttackDirection());
        const engine::Vector3 origin = player.GetWorldPosition() + engine::Vector3{0.0f, 0.78f, 0.0f};
        player.ForceSlimeAnimationModeForNextUpdate(PlayerSlimeAnimator::Mode::kIdle, direction);
        UpdateBreathVisuals(player, origin, direction, deltaTime);

        // 粒子は短い間隔で追加し、ダメージは別の長い間隔で通知する。
        if (breathEffectTimer_ <= 0.0f) {
            EmitDirectedPreset("shared/fire_slime_breath", origin, direction, 1.0f);
            EmitDirectedPreset("shared/fire_slime_breath_embers", origin, direction, 0.85f);
            breathEffectTimer_ = 0.065f;
        }

        // ダメージの間隔が尽きた場合だけ敵を照会し、描画より長い0.22秒を次の周期にする。
        if (breathDamageTimer_ <= 0.0f) {
            DamageBreath(player, origin, direction);
            breathDamageTimer_ = 0.22f;
        }
    }

    void NotifyDamageStarted(Player& player) override {
        // 被弾側が操作を止める前に攻撃を中断する。頭部表示はコピーを解除するまで保持する。
        fireballSequence_.Cancel();
        StopBreathMotion(player);
        StopDashEffects();
        if (dashActive_) {
            player.SetIsControlActive(true);
            player.SetSlimeAbilityPose(PlayerSlimeAnimator::AbilityPose::kNone);
        }
        dashActive_ = primaryHeld_ = false;
        actionMotionTimer_ = 0.0f;
        recovery_.Clear();
        hitTargets_.clear();
        player.ClearSlimeAbilityMotion();
    }

    void Cancel(Player& player) override {
        // コピー解除時は演出・モーション・入力保持を消し、操作権を返す。
        fireballSequence_.Cancel();
        breathVisible_ = false;
        breathVisuals_.clear();
        recovery_.Clear();
        headVisual_.reset();
        StopDashEffects();
        player.ClearSlimeAbilityMotion();
        dashActive_ = false;
        primaryHeld_ = false;
        breathMotionActive_ = false;
        breathDamageTimer_ = 0.0f;
        breathEffectTimer_ = 0.0f;
        dashTimer_ = 0.0f;
        dashEffectTimer_ = 0.0f;
        dashGroundWakeTimer_ = 0.0f;
        actionMotionTimer_ = 0.0f;
        hitTargets_.clear();
        player.SetSlimeAbilityPose(PlayerSlimeAnimator::AbilityPose::kNone);
        player.SetIsControlActive(true);
    }

    PlayerAttackMovement GetAttackMovement() const override {
        // 火球は専用タイムラインの開始から指定秒数だけブレーキを掛ける。
        if (fireballSequence_.IsRunning()) {
            auto movement = fireballSequence_.GetDefinition().playerMovement;
            if (fireballSequence_.GetElapsed() >= movement.duration) return {};
            movement.blocksMovementActions = movement.enabled;
            return movement;
        }
        if (recovery_.Active()) return recovery_.Movement();

        // 吐いている間は水平入力を弱め、照準には通常の入力を残す。
        if (!dashActive_ && primaryHeld_ && actionMotionTimer_ <= 0.0f) {
            auto movement = CopyAttackMovement(GetProfile(), "player_fire_breath");
            movement.blocksMovementActions = movement.enabled;
            return movement;
        }
        return {};
    }

    bool CanBreakImpactGate() const override {
        // 突進している間だけ衝撃ゲートを破壊できる。
        return dashActive_;
    }

    bool HasOwnedVisuals() const override {
        // 能力側が所有する頭部とブレスを、専用描画パスへ渡す判定に使う。
        return headVisual_ != nullptr || (breathVisible_ && !breathVisuals_.empty());
    }

    void DrawOwnedVisuals(Player& player, uint32_t depth, uint32_t grab, engine::Camera* camera, std::int32_t bufferIndex) override {
        // 死亡・演出ロック中は能力固有の見た目を描かず、通常カメラを補助的に使う。
        if (player.IsDead() || player.IsCinematicLocked())
            return;
        // 描画カメラが渡されていない場合は、アクティブカメラで代用する。
        if (!camera)
            camera = CameraManager::GetInstance()->GetActiveCamera();
        // 代用できるビューも無ければ、描画用定数を確定できないため戻る。
        if (!camera)
            return;
        // 頭部の炎が有効なら、直前のプレイヤー姿勢を反映してから描く。
        if (headVisual_ && headVisual_->GetIsVisible()) {
            SyncHeadVisualTransform(player);
            headVisual_->DrawSpecialMaterialForCamera(camera, depth, grab, bufferIndex);
        }

        // このフレームでブレスが有効でなければ、保持中の炎を描き直さない。
        if (!breathVisible_)
            return;
        for (auto& flame : breathVisuals_) {
            flame->UpdateWorldMatrix(true);
            flame->DrawSpecialMaterialForCamera(camera, depth, grab, bufferIndex);
        }
    }

  private:
    void UpdateHeadVisual(Player& player, float frameDeltaTime) {
        // 頭部の炎は接触判定を持たない表示専用Objectとして遅延生成する。
        // 死亡または演出拘束中は、頭の炎を生成・更新しない。
        if (player.IsDead() || player.IsCinematicLocked()) {
            // 以前に表示用 Object を生成している場合は非表示にする。
            if (headVisual_)
                headVisual_->SetIsVisible(false);
            return;
        }

        // まだ表示用 Object が無い場合だけ生成する。一度作ったら位置と色の更新に使い回す。
        if (!headVisual_) {
            auto* scene = SceneManager::GetInstance()->GetCurrentScene();
            // シーンや描画の共通部が無ければ、生成できるフレームまで見送る。
            if (!scene || !scene->GetObject3DCommon())
                return;
            headVisual_ = std::make_unique<engine::Object3D>();
            headVisual_->Initialize(scene->GetObject3DCommon());
            headVisual_->SetModel("Primitives/sphere");
            headVisual_->SetColliderType(engine::ColliderType::kNone);
            headVisual_->SetCollisionAttribute(0);
            headVisual_->SetCollisionMask(0);
            headVisual_->SetMaterialType(11);
            headVisual_->SetBlendMode(BlendMode::kNormal);
            headVisual_->SetEnableLighting(false);
            headVisual_->SetEmissive(1.7f);

            // 特殊材質の定数があれば、頭部用の炎の揺れと強さを設定する。
            if (auto* p = headVisual_->GetMeshRenderer()->GetWaterParamData()) {
                p->effectType = 2.0f;
                p->waveSpeed = 1.75f;
                p->waveFrequency = 2.2f;
                p->waveHeight = 0.55f;
                p->effectScale = 0.95f;
                p->effectSoftness = 0.48f;
                p->effectIntensity = 0.84f;
                p->billboardScale = 0.80f;
                p->effectScaleX = 0.92f;
                p->effectScaleY = 1.0f;
                p->effectScaleZ = 0.92f;
            }
        }

        headVisual_->SetIsVisible(player.GetIsVisible());
        headVisual_->SetColor({1.0f, 0.26f, 0.04f, 0.75f});
        headVisual_->Update(frameDeltaTime);
        SyncHeadVisualTransform(player);
    }

    void SyncHeadVisualTransform(Player& player) {
        // 頭上の位置を本体ローカルで持ち、攻撃時の潰れ・傾き・移動もまとめて追従する。
        const auto* bodyRenderer = player.GetMeshRenderer();
        headVisualAnchor_.matWorld = bodyRenderer ? bodyRenderer->GetRenderWorldMatrix() : player.GetWorldMatrix();
        headVisual_->SetTransformAnchor(&headVisualAnchor_);
        headVisual_->SetTranslate({0.0f, 0.95f, 0.0f});
        headVisual_->SetRotation({0.0f, 0.0f, 0.0f});
        headVisual_->SetScale({0.73f, 0.70f, 0.73f});
        headVisual_->UpdateWorldMatrix(true);
    }

    // 照準方向に沿って七つの表示用の炎を配置し、位置ごとに色・大きさ・材質を変える。
    void UpdateBreathVisuals(Player& player, const engine::Vector3& origin, const engine::Vector3& direction, float frameDeltaTime) {
        // 死亡・演出拘束中は、ブレスの見た目を有効にしない。
        if (player.IsDead() || player.IsCinematicLocked())
            return;
        // 表示用の炎が未生成の場合だけ、七つの Object と固有の材質を確保する。
        if (breathVisuals_.empty()) {
            auto* scene = SceneManager::GetInstance()->GetCurrentScene();
            // シーンか描画共通部を取得できなければ、表示の生成を見送る。
            if (!scene || !scene->GetObject3DCommon())
                return;

            // 立体ブレスは能力側で所有し、能力解除時にまとめて破棄します。
            for (std::int32_t i = 0; i < 7; ++i) {
                auto flame = std::make_unique<engine::Object3D>();
                flame->Initialize(scene->GetObject3DCommon());
                flame->SetModel("Primitives/sphere");
                flame->SetColliderType(engine::ColliderType::kNone);
                flame->SetCollisionAttribute(0);
                flame->SetCollisionMask(0);
                flame->SetMaterialType(11);
                flame->SetBlendMode(BlendMode::kNormal);
                flame->SetEnableLighting(false);
                flame->SetEmissive(1.8f);

                // 特殊材質の定数がある炎は、各個体の UV をずらして同じ模様に見えないようにする。
                if (auto* p = flame->GetMeshRenderer()->GetWaterParamData()) {
                    p->effectType = 5.0f;
                    p->waveSpeed = 2.45f;
                    p->effectScale = 0.92f;
                    p->effectSoftness = 0.58f;
                    p->effectIntensity = 0.92f;
                    p->billboardScale = 0.72f;
                    p->effectScaleX = 1.0f;
                    p->effectScaleY = 0.90f;
                    p->effectScaleZ = 1.15f;
                    p->uvOffsetX = 3.71f * (i + 1);
                    p->uvOffsetY = 6.13f * (i + 1);
                }

                breathVisuals_.push_back(std::move(flame));
            }
        }

        EnemyAttackDefinition fallback;
        fallback.maxRange = 8.0f;
        fallback.radius = 1.4f;
        const auto& attack = FindAttackOrFallback(GetProfile(), "flame_breath", fallback);
        // 表示の長さと太さは当たり判定側と同じ下限を使い、射線上へ七つの炎を配置する。
        const float range = std::max(attack.maxRange, 4.0f);
        const float radius = std::max(attack.radius, 1.0f);
        breathVisible_ = true;
        breathVisualTime_ += frameDeltaTime;

        for (size_t i = 0; i < breathVisuals_.size(); ++i) {
            auto& flame = breathVisuals_[i];
            const float step = (static_cast<float>(i) + 0.5f) / breathVisuals_.size();
            const float pulse = std::sin(breathVisualTime_ * 9.0f + step * 7.0f) * 0.06f;
            const float width = std::max(range / 7.0f, radius * (0.65f + step * 0.55f));
            flame->SetTranslate(origin + direction * (step * range));
            flame->SetRotation({0.0f, std::atan2(direction.x, direction.z), 0.0f});
            flame->SetScale({width, (std::max)(radius * (0.48f + step * 0.22f + pulse), width * 0.68f), width * 1.12f});
            flame->SetColor({1.0f, 0.30f + step * 0.06f, 0.045f, 0.82f - step * 0.12f});

            // 材質定数が使える場合に、射線上の位置に応じて流れと横幅を調整する。
            if (auto* p = flame->GetMeshRenderer()->GetWaterParamData()) {
                p->flowSpeedY = 0.82f + step * 0.42f;
                p->effectScaleX = 1.08f + step * 0.56f;
                p->effectScaleY = 0.90f;
                p->effectScaleZ = 1.15f;
            }

            flame->Update(frameDeltaTime);
        }
    }

    void Fireball(Player& player) {
        // player_specialに紐づく攻撃から優先度最大の定義を選び、シーケンスを開始する。
        const EnemyAttackDefinition* selected = nullptr;

        for (const auto& candidate : GetProfile().attacks) {
            // 有効なシーケンスで、プレイヤーの必殺入力に割り当てられた攻撃を候補にする。
            if (!candidate.sequence.enabled || candidate.sequence.binding != "player_special")
                continue;
            // プレビューで攻撃IDを指定している場合は、他の技を候補から外す。
            if (!previewAttackId_.empty() && candidate.id != previewAttackId_)
                continue;
            // 最初の候補か、現在より優先度が高い候補で採用技を更新する。同値では先の定義を保つ。
            if (!selected || candidate.sequence.priority > selected->sequence.priority)
                selected = &candidate;
        }

        // 入力に対応する攻撃がなければ、ブレスを中止せず戻る。
        if (!selected)
            return;
        StopBreathMotion(player);
        // 素材や時刻の検証でシーケンスを開始できなければ、火球の姿勢時間と再使用待ちを設定しない。
        if (!fireballSequence_.Start(*selected, MakeSequenceContext(player)))
            return;
        // 開始できた技について、専用の本体トラックがあるか調べる。
        const bool authoredBody = std::any_of(selected->sequence.tracks.begin(), selected->sequence.tracks.end(),
            [](const auto& track) { return track.enabled && track.type == "body_animation"; });

        // 本体トラックがある攻撃では旧単発モーションを重ねて二重に変形させません。
        if (!authoredBody && !selected->animation.empty())
            player.PlaySlimeAbilityMotion(selected->animation, false, 1.0f, 0.035f, 0.08f);
        actionMotionTimer_ = selected->sequence.duration;
        fireballCooldown_ = (std::max)(selected->cooldown, selected->sequence.duration);
    }

    AttackSequenceContext MakeSequenceContext(Player& player) const {
        // ロックオン方向と接地状態を渡し、データ駆動の攻撃イベントから利用できるようにする。
        AttackSequenceContext context;
        context.owner = &player;
        context.position = player.GetWorldPosition();
        context.forward = NormalizePlanar(player.GetLockOnAttackDirection());
        context.playerOwned = true;
        context.grounded = player.IsGrounded();
        return context;
    }

    void BeginDash(Player& player) {
        fireballSequence_.Cancel();

        // 前回の突進が異常終了していても、新しい炎を重ねる前に必ず回収します。
        StopDashEffects();
        StopBreathMotion(player);
        dashDirection_ = NormalizePlanar(player.GetLockOnAttackDirection());
        player.PlaySlimeAbilityMotion("player_ability_fire_dash", false, 1.0f, 0.025f, 0.055f);
        // 突進の時計と命中履歴を初期化し、直前の攻撃状態を持ち越さない。
        dashActive_ = true;
        dashTimer_ = 0.44f;
        dashCooldown_ = 1.05f;
        dashEffectTimer_ = 0.0f;
        dashGroundWakeTimer_ = 0.0f;
        actionMotionTimer_ = 0.0f;
        hitTargets_.clear();
        const engine::Vector3 position = player.GetWorldPosition();
        SpawnScopedEffect(effectScope_, "Resources/json/effect/player/copy/fire/effect_player_fire_blaze_burst.json",
                          position + engine::Vector3{0.0f, 0.18f, 0.0f});
        SpawnScopedEffect(effectScope_, "Resources/json/effect/player/copy/fire/effect_player_fire_blaze_body.json",
                          position + engine::Vector3{0.0f, 0.60f, 0.0f},
                          {0.0f, std::atan2(dashDirection_.x, dashDirection_.z), 0.0f});
        EmitPreset("player/copy/fire/player_fire_blaze_burst", position + engine::Vector3{0.0f, 0.42f, 0.0f});
        // 自動移動に切り替え、短い無敵と専用姿勢を開始する。
        player.SetVelocity(
            {dashDirection_.x * 38.0f, std::max(player.GetVelocity().y, 0.0f), dashDirection_.z * 38.0f});
        player.SetMoveYaw(std::atan2(dashDirection_.x, dashDirection_.z));
        player.SetIsControlActive(false);
        player.StartEvasionInvincibility(0.50f);
        player.SetSlimeAbilityPose(PlayerSlimeAnimator::AbilityPose::kBlazeDash);
        player.TriggerSlimeImpulse({1.08f, 1.16f, 3.62f}, 0.17f);
    }

    void UpdateDash(Player& player, float deltaTime) {
        // 突進中は向きと水平速度を固定し、終了時に制御と残速度を戻す。
        dashTimer_ = std::max(0.0f, dashTimer_ - deltaTime);
        const float progress = 1.0f - dashTimer_ / 0.44f;
        const float speed = 38.0f * (1.0f - progress * 0.36f);
        engine::Vector3 velocity = player.GetVelocity();
        velocity.x = dashDirection_.x * speed;
        velocity.z = dashDirection_.z * speed;
        // 接地中に下向きの速度が残っている場合は、床へ押し込む鉛直速度を0にする。
        if (player.IsGrounded() && velocity.y < 0.0f)
            velocity.y = 0.0f;
        player.SetVelocity(velocity);
        player.SetIsControlActive(false);
        player.SetMoveYaw(std::atan2(dashDirection_.x, dashDirection_.z));
        player.ForceSlimeAnimationModeForNextUpdate(PlayerSlimeAnimator::Mode::kDash, dashDirection_);
        player.SetSlimeAbilityPose(PlayerSlimeAnimator::AbilityPose::kBlazeDash);
        DamageDash(player);
        if (!dashActive_) return; // 同期の被弾通知で中断した場合、反動を開始し直さない。

        // 移動の後方に火の軌跡を残し、放出する粒子も進行方向と逆へ向ける。
        if (dashEffectTimer_ <= 0.0f) {
            const engine::Vector3 trail = player.GetWorldPosition() - dashDirection_ * 0.72f + engine::Vector3{0.0f, 0.12f, 0.0f};
            SpawnScopedEffect(effectScope_, "Resources/json/effect/player/copy/fire/effect_player_fire_blaze_trail.json", trail,
                              {0.0f, std::atan2(dashDirection_.x, dashDirection_.z), 0.0f});
            SpawnScopedEffect(effectScope_, "Resources/json/effect/player/copy/fire/effect_player_fire_blaze_core.json",
                              trail + engine::Vector3{0.0f, 0.36f, 0.0f});
            EmitDirectedPreset("player/copy/fire/player_fire_blaze_trail", trail, dashDirection_ * -1.0f, 0.85f);
            dashEffectTimer_ = 0.055f;
        }

        // 床の炎は着地している間だけ別の周期で出す。
        if (player.IsGrounded() && dashGroundWakeTimer_ <= 0.0f) {
            const engine::Vector3 wake = player.GetWorldPosition() - dashDirection_ * 0.42f + engine::Vector3{0.0f, 0.04f, 0.0f};
            SpawnScopedEffect(effectScope_, "Resources/json/effect/player/copy/fire/effect_player_fire_blaze_ground_wake.json", wake,
                              {0.0f, std::atan2(dashDirection_.x, dashDirection_.z), 0.0f}, {0.92f, 0.92f, 1.16f});
            dashGroundWakeTimer_ = 0.10f;
        }

        // 持続時間終了で操作を返し、加速したまま走り続けないよう残速度を減らす。
        if (dashTimer_ <= 0.0f) {
            StopDashEffects();
            dashActive_ = false;
            recovery_.Start(CopyAttackMovement(GetProfile(), "player_fire_dash_recovery"));
            player.SetSlimeAbilityPose(PlayerSlimeAnimator::AbilityPose::kNone);
            player.StopSlimeAbilityMotion(0.055f);
            player.SetIsControlActive(true);
            velocity.x *= 0.30f;
            velocity.z *= 0.30f;
            player.SetVelocity(velocity);
            hitTargets_.clear();
            EmitPreset("player/copy/fire/player_fire_blaze_burst", player.GetWorldPosition() + engine::Vector3{0.0f, 0.20f, 0.0f});
        }
    }

    void StopDashEffects() {
        // この能力が所有する突進演出スコープだけを停止する。
        if (MeshEffectManager* effects = MeshEffectManager::GetInstance()) {
            effects->StopEffectScope(effectScope_);
        }
    }

    void StopBreathMotion(Player& player) {
        // 押し続け解除時は見た目とループモーションを同時に止める。
        breathVisible_ = false;

        // ループ姿勢の開始に成功していない場合は、別の能力モーションを誤って止めない。
        if (!breathMotionActive_) {
            return;
        }

        player.StopSlimeAbilityMotion(0.08f);
        breathMotionActive_ = false;
    }

    void DamageDash(Player& player) {
        // 重なるColliderを持つ敵でも、突進一回につき根Objectへ一度だけ与える。
        engine::PhysicsQueryFilter filter;
        filter.mask = engine::collisionCategories::Get("enemy");
        filter.ignoredObject = &player;
        const engine::Vector3 center = player.GetWorldPosition() + engine::Vector3{0.0f, 0.72f, 0.0f};

        for (const engine::PhysicsOverlapHit& hit : engine::CollisionManager::GetInstance()->OverlapSphere(center, 1.55f, filter)) {
            engine::Object3D* target = FindEnemyRoot(hit.object);
            // 敵のルートを得られないか、この突進ですでに命中したルートなら省く。
            if (!target || !hitTargets_.insert(target).second)
                continue;
            EnemyAttackDefinition fallback;
            const EnemyAttackDefinition& attack = FindAttackOrFallback(GetProfile(), "flame_breath", fallback);
            DamageEnemy(player, target, 12.0f, DamageType::kFire,
                        {dashDirection_.x * 11.0f, 5.5f, dashDirection_.z * 11.0f}, MakeBurnStatus(attack));
            VfxSequencer::PlayOneShot("player/copy/fire/player_fire_dash_hit_cue",
                                      target->GetWorldPosition() + engine::Vector3{0.0f, 0.58f, 0.0f});
        }
    }

    void DamageBreath(Player& player, const engine::Vector3& origin, const engine::Vector3& direction) {
        // 見た目に沿うカプセル形状で敵を探し、同じ更新内の多重ヒットを除く。
        EnemyAttackDefinition fallback;
        fallback.maxRange = 8.0f;
        fallback.radius = 1.4f;
        fallback.damage = 0.5f;
        const EnemyAttackDefinition& attack = FindAttackOrFallback(GetProfile(), "flame_breath", fallback);
        engine::PhysicsQueryFilter filter;
        filter.mask = engine::collisionCategories::Get("enemy");
        filter.ignoredObject = &player;
        std::unordered_set<engine::Object3D*> damaged;
        const engine::Vector3 end = origin + direction * std::max(attack.maxRange, 4.0f);

        for (const engine::PhysicsOverlapHit& hit :
             engine::CollisionManager::GetInstance()->OverlapCapsule(origin, end, std::max(attack.radius, 1.0f), filter)) {
            engine::Object3D* target = FindEnemyRoot(hit.object);
            // このダメージ周期内ですでに選んだ敵と、ルートを解決できない接触を除く。
            if (!target || !damaged.insert(target).second)
                continue;
            DamageEnemy(player, target, std::max(attack.damage, 2.5f), DamageType::kFire,
                        {direction.x * 5.0f, 2.5f, direction.z * 5.0f}, MakeBurnStatus(attack));
        }
    }

    engine::Vector3 dashDirection_{0.0f, 0.0f, 1.0f};
    AttackSequence fireballSequence_;
    std::string previewAttackId_;
    // 表示Objectより先に確保し、親参照より長く生存させる。
    Transform headVisualAnchor_;
    std::unique_ptr<engine::Object3D> headVisual_;
    std::vector<std::unique_ptr<engine::Object3D>> breathVisuals_;
    bool breathVisible_ = false;
    float breathVisualTime_ = 0.0f;
    std::unordered_set<engine::Object3D*> hitTargets_;
    float fireballCooldown_ = 0.0f;
    float dashCooldown_ = 0.0f;
    float dashTimer_ = 0.0f;
    float dashEffectTimer_ = 0.0f;
    float breathDamageTimer_ = 0.0f;
    float breathEffectTimer_ = 0.0f;
    float dashGroundWakeTimer_ = 0.0f;
    CopyAttackRecovery recovery_;
    float actionMotionTimer_ = 0.0f;
    MeshEffectManager::EffectScopeId effectScope_ = MeshEffectManager::kInvalidEffectScope;
    bool dashActive_ = false;
    bool primaryHeld_ = false;
    bool breathMotionActive_ = false;
};
} // namespace

namespace copyAbilityDetail {
std::unique_ptr<ICopyAbilitySession> CreateFireSlimeCopyAbility(const EnemyAttackProfile& profile) {
    // 獲得ごとに独立したクールダウンと演出スコープを持つセッションを返す。
    return std::make_unique<FireSlimeCopyAbility>(profile);
}
} // namespace copyAbilityDetail
