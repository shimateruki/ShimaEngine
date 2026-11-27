#include "CopyAbilitySupport.h"
#include <numbers>
#include "game/system/combat/GroundEffectLocator.h"

namespace {
using namespace copyAbilityDetail;
constexpr float kBlastWindupDuration = 0.07f;
constexpr float kBlastJetDuration = 0.24f;
constexpr float kBlastJetPitch = (std::numbers::pi_v<float> * 0.5f);

// コピー能力の爆弾設置・投擲・爆風ジャンプを担当する。各操作の再使用待ちは Update()、中断時の後片付けは Cancel() が扱う。
class BomberCopyAbility final : public ICopyAbilitySession {
  public:
    using ICopyAbilitySession::ICopyAbilitySession;

    void ProcessInput(Player& player, const PlayerCopyAbilityInput& input) override {
        // 爆風ジャンプの予備動作中は、次のジャンプや爆弾の設置・投擲を受け付けない。
        if (blastWindupActive_ || recovery_.Active())
            return;
        // 二次操作が押され、爆風ジャンプの再使用待ちが終わっていれば最優先で開始する。
        if (input.secondaryTriggered && blastCooldown_ <= 0.0f)
            BeginBlastJump(player);
        // 爆風ジャンプを始めず、特殊入力と投擲の待機条件を満たす場合は投げる。
        else if (input.specialTriggered && throwCooldown_ <= 0.0f)
            SpawnBomb(player, false);
        // 先の2操作が成立せず、主入力と設置の待機条件を満たす場合は配置する。
        else if (input.primaryTriggered && placeCooldown_ <= 0.0f)
            SpawnBomb(player, true);
    }

    void Update(Player& player, float deltaTime) override {
        // 投擲・設置・爆風ジャンプの再使用時間を独立に進める。
        recovery_.Update(deltaTime);
        throwCooldown_ = std::max(0.0f, throwCooldown_ - deltaTime);
        placeCooldown_ = std::max(0.0f, placeCooldown_ - deltaTime);
        // 予備動作の開始に使う爆風ジャンプの待ち時間も、投擲や設置とは独立して減らす。
        blastCooldown_ = std::max(0.0f, blastCooldown_ - deltaTime);
        trailTimer_ -= deltaTime;

        // 爆風ジャンプの予備動作中は、発動までの残り秒数を進める。
        if (blastWindupActive_) {
            blastWindupTimer_ -= deltaTime;

            // 予備動作時間が尽きたフレームに、爆風と上昇を同時に始める。
            if (blastWindupTimer_ <= 0.0f) {
                blastWindupActive_ = false;
                LaunchBlastJump(player);
            }
        }

        // 爆発後の状態に入っていなければ、噴流と着地の判定は行わない。
        if (!blastActive_)
            return;
        blastElapsed_ += deltaTime;

        // 離陸後0.24秒以内で粒子の間隔が満了した場合だけ、足元に下向きの噴流を足す。
        if (blastElapsed_ <= kBlastJetDuration && trailTimer_ <= 0.0f) {
            const engine::Vector3 position = player.GetWorldPosition() - blastDirection_ * 0.18f + engine::Vector3{0.0f, 0.12f, 0.0f};

            // 突き形状の軸を下へ向け、上昇する体の下側に噴流を残す。
            SpawnEffect("Resources/json/effect/shared/effect_player_bomb_blast_jump_trail.json", position,
                        {kBlastJetPitch, 0.0f, 0.0f});
            EmitDirectedPreset("shared/player_bomb_blast_jump", position, {0.0f, -1.0f, 0.0f}, 0.62f);
            trailTimer_ = 0.075f;
        }

        // 離陸から0.16秒を過ぎ、接地して上向きの速度も無い場合に着地を確定する。
        if (blastElapsed_ > 0.16f && player.IsGrounded() && player.GetVelocity().y <= 0.0f) {
            SpawnEffect("Resources/json/effect/shared/effect_player_bomb_blast_jump_land.json", player.GetWorldPosition());
            blastActive_ = false;
        }
    }

    PlayerAttackMovement GetAttackMovement() const override {
        if (blastWindupActive_) {
            auto movement = CopyAttackMovement(GetProfile(), "player_bomb_blast_jump");
            movement.blocksMovementActions = movement.enabled;
            return movement;
        }
        return recovery_.Movement();
    }

    void NotifyDamageStarted(Player& player) override { Cancel(player); }

    void Cancel(Player& player) override {
        // 被弾などで爆発待ちと噴流を止め、移動側に渡した空中の押し出しも解除します。
        recovery_.Clear();
        blastWindupActive_ = false;
        blastActive_ = false;
        player.SetAbilityAirImpulse({});
    }

  private:
    void SpawnBomb(Player& player, bool placed) {
        // 現在のシーンと描画基盤から、この爆弾を生成できるかを確かめる。
        SceneManager* manager = SceneManager::GetInstance();
        BaseScene* scene = manager ? manager->GetCurrentScene() : nullptr;
        // シーンかオブジェクトの描画基盤が無ければ、生成を行わない。
        if (!scene || !scene->GetObject3DCommon())
            return;
        auto bomb = EnemyFactory::GetInstance()->CreateEnemy("Bomb", scene->GetObject3DCommon());
        // ファクトリーから爆弾を取得できない場合は、配置と再使用待ちの設定へ進まない。
        if (!bomb)
            return;
        // 狙い方向を水平にし、設置は足元寄り、投擲は少し前方かつ上から始めます。
        const engine::Vector3 forward = NormalizePlanar(player.GetLockOnAttackDirection());
        engine::Vector3 position = player.GetWorldPosition() + forward * (placed ? 1.55f : 2.1f);
        position.y += placed ? 0.42f : 2.15f;
        bomb->SetTranslate(position);
        bomb->SetRotationY(std::atan2(forward.x, forward.z));
        bomb->SetTarget(&player);

        // エディターのプレビューから生成する場合は、プレビュー実体として識別して編集をロックする。
        if (player.IsEditorInternal()) {
            bomb->SetName("__Editor_EnemyAttackPreview_CopyBomb");
            bomb->SetEditorInternal(true);
            bomb->SetIsLocked(true);
        }

        // 生成した敵がEnemyBombならプレイヤー所有の爆弾にし、設置・投擲で異なる導火時間を設定する。
        if (EnemyBomb* enemyBomb = dynamic_cast<EnemyBomb*>(bomb.get())) {
            enemyBomb->SetPlayerOwned(true);
            enemyBomb->Ignite(placed ? 1.35f : 2.25f);
        }

        // 設置では足元で小さく跳ねさせ、投擲では前方と上方向に初速を与える。
        bomb->SetCarried(false);
        bomb->SetVelocity(placed ? engine::Vector3{0.0f, 1.8f, 0.0f} : engine::Vector3{forward.x * 24.0f, 8.0f, forward.z * 24.0f});
        SpawnEffect(placed ? "Resources/json/effect/shared/effect_player_bomb_place.json"
                           : "Resources/json/effect/shared/effect_carry_bomber_throw_burst.json",
                    position);
        EmitPreset(placed ? "shared/player_bomb_place_fuse" : "shared/carry_bomber_throw_sparks", position);

        // エディターのプレビューは、通常シーンの一覧へ入れずプレイヤーのプレビュー管理へ渡す。
        if (player.IsEditorInternal())
            player.AddCopyPreviewObject(std::move(bomb));
        // ゲーム実行中に生成した爆弾は、シーンに登録して通常更新へ渡す。
        else
            scene->AddObject(std::move(bomb));
        // 爆弾を登録できた時点から、設置または投擲の反動を始める。
        recovery_.Start(CopyAttackMovement(GetProfile(), placed ? "player_bomb_place" : "player_bomb_throw"));
        // 設置した場合は設置の待機時間を設定し、足元への押し込みを変形で表す。
        if (placed) {
            placeCooldown_ = 0.85f;
            player.TriggerSlimeImpulse({1.62f, 0.78f, 1.62f}, 0.13f);
        // 投げた場合は、投擲の再使用待ちだけを設定する。
        } else {
            throwCooldown_ = 0.42f;
        }
    }

    void BeginBlastJump(Player& player) {
        // 地上だけ短く沈み込ませる。空中では入力遅延を増やさず即座に爆発させる。
        blastDirection_ = NormalizePlanar(player.GetForwardDirection());
        blastCooldown_ = 1.10f;
        const auto movement = CopyAttackMovement(GetProfile(), "player_bomb_blast_jump");
        const float windup = movement.enabled ? movement.duration : kBlastWindupDuration;
        blastWindupTimer_ = player.IsGrounded() ? windup : 0.0f;
        player.StartEvasionInvincibility(0.32f + blastWindupTimer_);

        // 接地から開始して予備動作時間がある場合は、沈み込む姿勢で爆発を待つ。
        if (blastWindupTimer_ > 0.0f) {
            player.TriggerSlimeImpulse({1.70f, 0.64f, 1.70f}, blastWindupTimer_ + 0.03f);
            blastWindupActive_ = true;
        // 空中など予備動作時間が無い場合は、その場で爆風を開始する。
        } else {
            LaunchBlastJump(player);
        }
    }

    void LaunchBlastJump(Player& player) {
        // 爆発を起点に上へ跳ねさせ、水平の押し出しは移動処理にも短時間渡す。
        const engine::Vector3 center = player.GetWorldPosition();
        // 接地中は地面から、空中では現在位置から爆発の見た目を出す。
        engine::Vector3 blastOrigin = player.IsGrounded() ? GroundEffectLocator::ResolveGroundPosition(center) : center;
        blastOrigin.y += 0.08f;
        player.SetVelocity({blastDirection_.x * 8.5f, 18.0f, blastDirection_.z * 8.5f});
        player.SetAbilityAirImpulse({blastDirection_.x * 8.5f, 0.0f, blastDirection_.z * 8.5f});
        player.SetMoveYaw(std::atan2(blastDirection_.x, blastDirection_.z));
        player.ForceSlimeAnimationModeForNextUpdate(PlayerSlimeAnimator::Mode::kJump, blastDirection_);
        player.TriggerSlimeImpulse({0.78f, 2.70f, 0.78f}, 0.16f);
        DamageSphere(player, center + engine::Vector3{0.0f, 0.58f, 0.0f}, 3.2f, 8.0f, DamageType::kExplosion, 13.0f, 9.5f);
        SpawnEffect("Resources/json/effect/shared/effect_player_bomb_blast_jump.json", blastOrigin);
        EmitPreset("shared/player_bomb_blast_jump", blastOrigin);

        // 離陸の瞬間にも一筋出し、次フレームの更新を待たずに爆風の向きを見せる。
        SpawnEffect("Resources/json/effect/shared/effect_player_bomb_blast_jump_trail.json", blastOrigin,
                    {kBlastJetPitch, 0.0f, 0.0f});
        // 離陸時の噴流を出した後、粒子の間隔と着地判定の時計を初期化する。
        trailTimer_ = 0.075f;
        blastElapsed_ = 0.0f;
        blastActive_ = true;
    }

    engine::Vector3 blastDirection_{0.0f, 0.0f, 1.0f};
    CopyAttackRecovery recovery_;
    float throwCooldown_ = 0.0f;
    float placeCooldown_ = 0.0f;
    float blastCooldown_ = 0.0f;
    float trailTimer_ = 0.0f;
    float blastElapsed_ = 0.0f;
    float blastWindupTimer_ = 0.0f;
    bool blastWindupActive_ = false;
    bool blastActive_ = false;
};

} // namespace

namespace copyAbilityDetail {
std::unique_ptr<ICopyAbilitySession> CreateBomberCopyAbility(const EnemyAttackProfile& profile) {
    // 個々のコピー獲得ごとにクールダウン状態を持つセッションを作る。
    return std::make_unique<BomberCopyAbility>(profile);
}
} // namespace copyAbilityDetail
