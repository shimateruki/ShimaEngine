#include <cstdint>
#include "CopyAbilitySupport.h"

namespace {
using namespace copyAbilityDetail;

// 雷コピーの攻撃状態と火花表示を管理し、解除時に専用スコープの演出を消す。
// PlayerCopyAbilityControllerが所有する雷攻撃の実装。入力はProcessInput、進行はUpdate、一時表示の回収はCancelが担当する。
class ThunderSlimeCopyAbility final : public ICopyAbilitySession {
  public:
    explicit ThunderSlimeCopyAbility(EnemyAttackProfile profile) : ICopyAbilitySession(std::move(profile)) {
        // 溜め中に繰り返す火花を、発射・回避・解除時にまとめて消せるようにする。
        sparkScope_ = MeshEffectManager::GetInstance()->CreateEffectScope();
    }

    ~ThunderSlimeCopyAbility() override {
        // 能力が破棄されても残った火花を止める。
        ClearSparkEffects();
    }

    const char* GetStateLabel() const override {
        // 溜めが上限に達した時だけ「離して発射」を案内する。
        return charging_ ? (chargeTime_ >= 1.0f ? "帯電・溜め完了 / 離して発射" : "帯電中") : "雷能力";
    }

    void ProcessInput(Player& player, const PlayerCopyAbilityInput& input) override {
        // 回避・連続落雷・溜めを排他的に開始し、別行動へ移る際は溜めを解く。
        primaryHeld_ = input.primaryHeld;
        // 放電・落雷の反動が残る間は雷移動でキャンセルできない。
        if (recovery_.Active()) return;

        // 同時入力では回避、連続落雷、溜めの順に優先し、前二つでは溜めの表示も解除します。
        if (input.secondaryTriggered && evadeCooldown_ <= 0.0f) {
            StopCharge(player);
            Evade(player);
        // 回避が成立せず、特殊入力があり再使用待ちと進行中の落雷も無い場合は、連続落雷を予約する。
        } else if (input.specialTriggered && lineCooldown_ <= 0.0f && lineTimer_ <= 0.0f) {
            StopCharge(player);
            lineDirection_ = NormalizePlanar(player.GetLockOnAttackDirection());
            lineOrigin_ = player.GetWorldPosition();
            lineTimer_ = 0.50f;
            recovery_.Start(CopyAttackMovement(GetProfile(), "player_line_lightning"));
            strikeIndex_ = 0;
            // 射程全体を半径の1.8倍を目安に区切り、落雷数を2〜32発に収める。
            const auto& attack = LineAttack();
            const float span = std::max(0.0f, attack.maxRange - attack.minRange);
            strikeCount_ = std::clamp(static_cast<std::int32_t>(std::ceil(span / std::max(0.5f, attack.radius * 1.8f))) + 1, 2, 32);
            lineCooldown_ = 1.65f;
            lineHitTargets_.clear();
            player.PlaySlimeAbilityMotion("player_ability_thunder_cast", false, 1.0f, 0.04f, 0.08f);
            SpawnEffect("Resources/json/effect/shared/effect_player_thunder_warning.json",
                        player.GetWorldPosition() + lineDirection_ * LineAttack().minRange);
        // 先の二行動が成立せず、主入力があり放電の待ち時間・落雷・溜めが無い場合は、溜めを始める。
        } else if (input.primaryTriggered && dischargeCooldown_ <= 0.0f && lineTimer_ <= 0.0f && !charging_) {
            charging_ = true;
            chargeTime_ = sparkEffectTimer_ = sparkDamageTimer_ = 0.0f;
            sparkVisualTime_ = 0.0f;
            player.PlaySlimeAbilityMotion("player_ability_thunder_charge", true, 1.0f, 0.04f, 0.08f);
        }
    }

    void Update(Player& player, float deltaTime) override {
        // 連続落雷は経過時間で配置し、フレーム落ちでも未発射分を追いつかせる。
        recovery_.Update(deltaTime);
        lineCooldown_ = std::max(0.0f, lineCooldown_ - deltaTime);
        dischargeCooldown_ = std::max(0.0f, dischargeCooldown_ - deltaTime);
        evadeCooldown_ = std::max(0.0f, evadeCooldown_ - deltaTime);
        // 帯電を開始済みの場合だけ、入力保持や時間に応じた溜め更新を行う。
        if (charging_)
            UpdateCharge(player, deltaTime);
        // 連続落雷の進行が無ければ、今フレームの着弾位置を計算しない。
        if (lineTimer_ <= 0.0f)
            return;
        lineTimer_ = std::max(0.0f, lineTimer_ - deltaTime);
        const float elapsed = 0.50f - lineTimer_;

        // 経過時刻までに複数の発生予定を通過していれば、残りの発数を超えない範囲で順番に発生させる。
        while (strikeIndex_ < strikeCount_ &&
               elapsed + 0.00001f >= 0.10f + 0.40f * static_cast<float>(strikeIndex_) / static_cast<float>(strikeCount_ - 1)) {
            const auto& attack = LineAttack();
            const float progress = static_cast<float>(strikeIndex_) / static_cast<float>(strikeCount_ - 1);
            const float distance = attack.minRange + (attack.maxRange - attack.minRange) * progress;
            // 操作復帰後に移動しても、予兆を出した位置から順に落雷させる。
            const engine::Vector3 ground = lineOrigin_ + lineDirection_ * distance;
            SpawnStrike(player, ground);
            if (strikeIndex_ >= strikeCount_) return; // 被弾で残りの予約が消された場合は終了する。
            ++strikeIndex_;
        }
    }

    PlayerAttackMovement GetAttackMovement() const override {
        if (recovery_.Active()) return recovery_.Movement();
        return charging_ ? CopyAttackMovement(GetProfile(), "player_thunder_charge") : PlayerAttackMovement{};
    }

    void NotifyDamageStarted(Player& player) override { Cancel(player); }

    void Cancel(Player& player) override {
        // 能力解除後に溜めや遅延落雷が再開しないよう、入力と時計を消す。
        StopCharge(player);
        primaryHeld_ = false;
        player.ClearSlimeAbilityMotion();
        recovery_.Clear();
        lineTimer_ = 0.0f;
        strikeCount_ = 0;
        lineHitTargets_.clear();
    }

  private:
    const EnemyAttackDefinition& LineAttack() const {
        // 敵用の落雷設定を変えず、コピー用の射程・半径・威力をEditorから調整する。
        static const EnemyAttackDefinition kFallback = [] {
            EnemyAttackDefinition attack;
            attack.minRange = 2.8f;
            attack.maxRange = 22.0f;
            attack.radius = 1.45f;
            attack.damage = 10.0f;
            return attack;
        }();
        return FindAttackOrFallback(GetProfile(), "player_line_lightning", kFallback);
    }

    void ClearSparkEffects() {
        // この能力のEffectScopeだけを止め、他の効果へ干渉しない。
        if (auto* effects = MeshEffectManager::GetInstance())
            effects->StopEffectScope(sparkScope_);
    }

    void StopCharge(Player& player) {
        // 溜め動作が実行中ならモーションも止め、火花を回収する。
        if (charging_)
            player.StopSlimeAbilityMotion(0.08f);
        charging_ = false;
        ClearSparkEffects();
    }

    void UpdateCharge(Player& player, float frameDeltaTime) {
        // 溜め量は上限で止めても、電撃の走る時間は止めない。
        sparkVisualTime_ += frameDeltaTime;
        chargeTime_ = std::min(1.0f, chargeTime_ + frameDeltaTime);
        sparkEffectTimer_ -= frameDeltaTime;
        sparkDamageTimer_ -= frameDeltaTime;
        const engine::Vector3 center = player.GetWorldPosition() + engine::Vector3{0.0f, 0.85f, 0.0f};

        // 主入力を離した場合は、上限に達していなくても、現在の溜め量で弾を出す。
        if (!primaryHeld_) {
            // 弾の寿命と衝突は共通BulletManagerに任せ、能力解除後も安全に完走させます。
            const engine::Vector3 direction = NormalizePlanar(player.GetLockOnAttackDirection());
            const float radius = 0.45f + chargeTime_ * 0.40f;
            engine::Vector3 muzzle = center;
            engine::PhysicsQueryFilter groundFilter;
            groundFilter.mask = engine::collisionCategories::Get("ground");
            groundFilter.ignoredObject = &player;
            const auto floor = engine::CollisionManager::GetInstance()->Raycast(center, {0.0f, -1.0f, 0.0f}, radius + 1.0f, groundFilter);
            // 足元で上向きの床を検出した場合は、弾半径と余裕を含めて発射点を床より上にする。
            if (floor.isHit && floor.normal.y > 0.5f)
                muzzle.y = std::max(muzzle.y, floor.hitPoint.y + radius + 0.10f);
            BulletVisualConfig visual;
            // 表示倍率2倍を衝突半径へ掛けず、足元の床と重ならない高さから撃つ。
            visual.collisionRadiusIsWorld = true;
            visual.destroyOnSolidCastHit = true;
            visual.materialType = 18;
            visual.effectType = 2.0f;
            visual.color = {1.0f, 0.95f, 0.3f, 1.0f};
            visual.emissive = 2.0f;
            visual.effectIntensity = 2.4f;
            visual.visualScale = 2.0f;
            visual.billboardScale = 1.2f;
            visual.trailPreset = "player/copy/thunder/player_thunder_evade_sparks";
            visual.impactPreset = "shared/player_thunder_strike_impact";
            BulletManager::GetInstance()->Fire(muzzle, direction * 28.0f, engine::collisionCategories::Get("player_attack"), engine::collisionCategories::Get("enemy") | engine::collisionCategories::Get("ground"),
                                               "Primitives/sphere", radius, 0.9f, visual,
                                               7.0f + 13.0f * chargeTime_, {}, DamageType::kElectric);
            StopCharge(player);
            player.PlaySlimeAbilityMotion("player_ability_thunder_cast", false, 1.0f, 0.02f, 0.08f);
            player.TriggerSlimeImpulse({1.6f, 0.55f, 1.6f}, 0.16f);
            EmitPreset("player/copy/thunder/player_thunder_discharge_burst", center);
            recovery_.Start(CopyAttackMovement(GetProfile(), "player_thunder_discharge"));
            dischargeCooldown_ = 0.45f;
            return;
        }

        // 押し続けている間だけ火花を更新します。前の火花を同じスコープから消して重なりを防ぎます。
        if (sparkEffectTimer_ <= 0.0f) {
            ClearSparkEffects();
            const float radius = 1.05f;
            const std::int32_t arcCount = 3;

            // 経過時間を段階に分け、短い電撃が身体の周囲で飛ぶ位置を切り替える。
            const float phase = std::floor(sparkVisualTime_ * 10.0f);
            const float pulse = chargeTime_ >= 1.0f ? 0.5f + 0.5f * std::sin(sparkVisualTime_ * 8.0f) : 0.0f;

            for (std::int32_t i = 0; i < arcCount; ++i) {
                const float angle = i * 2.15f + phase * 2.4f;
                const float flicker = std::sin(phase * 1.7f + i * 2.3f);
                SpawnScopedEffect(
                    sparkScope_, "Resources/json/effect/player/copy/thunder/effect_player_thunder_spark_arc.json",
                    center + engine::Vector3{std::sin(angle) * radius, 0.10f + flicker * 0.20f, std::cos(angle) * radius},
                    {0.12f * flicker, angle, kPi * 0.5f + flicker * 0.28f},
                    {0.8f + pulse * 0.12f, 0.85f + chargeTime_ * 0.15f, 1.0f});
            }

            // 満充電になり明滅の位相が強いときだけ、頭上に完了を知らせる火花を足す。
            if (chargeTime_ >= 1.0f && pulse > 0.70f) {
                SpawnScopedEffect(sparkScope_, "Resources/json/effect/player/copy/thunder/effect_player_thunder_spark_arc.json",
                                  center + engine::Vector3{0.0f, 0.65f, 0.0f}, {kPi * 0.5f, phase * 1.3f, 0.0f},
                                  {0.75f, 0.85f, 1.0f});
            }

            sparkEffectTimer_ = 0.10f;
        }

        // 見た目の更新間隔とは別の時計で、近接した敵への帯電ダメージを間引きます。
        if (sparkDamageTimer_ <= 0.0f) {
            DamageSphere(player, center, 2.2f, 3.0f, DamageType::kElectric, 2.0f, 0.5f);
            sparkDamageTimer_ = 0.28f;
        }
    }

    void SpawnStrike(Player& player, const engine::Vector3& ground) {
        // 見た目と球状判定を同じ着地点へ置き、連続落雷中は敵ごとに一度だけ当てる。
        const float yaw = std::atan2(lineDirection_.x, lineDirection_.z);
        SpawnEffect("Resources/json/effect/shared/effect_player_thunder_bolt.json", ground + engine::Vector3{0.0f, 4.75f, 0.0f},
                    {kPi * 0.5f, yaw, 0.0f});
        SpawnEffect("Resources/json/effect/shared/effect_player_thunder_impact_ring.json",
                    ground + engine::Vector3{0.0f, 0.06f, 0.0f});
        EmitPreset("shared/player_thunder_strike_impact", ground + engine::Vector3{0.0f, 0.22f, 0.0f});
        // 落雷ごとの重なりを敵本体へ解決し、同じ連続攻撃ですでに当てた敵は飛ばします。
        engine::PhysicsQueryFilter filter;
        filter.mask = engine::collisionCategories::Get("enemy");
        filter.ignoredObject = &player;

        for (const engine::PhysicsOverlapHit& hit :
             engine::CollisionManager::GetInstance()->OverlapSphere(ground + engine::Vector3{0.0f, 1.1f, 0.0f}, LineAttack().radius, filter)) {
            engine::Object3D* target = FindEnemyRoot(hit.object);
            // 敵本体が見つからないか、同じ連続落雷ですでに命中した敵は飛ばす。
            if (!target || !lineHitTargets_.insert(target).second)
                continue;
            DamageEnemy(player, target, LineAttack().damage, DamageType::kElectric,
                        {lineDirection_.x * 6.5f, 7.0f, lineDirection_.z * 6.5f});
            if (strikeCount_ == 0) return;
        }
    }

    void Evade(Player& player) {
        // 進行方向へ最大9.5の球を掃引し、地形に当たった場合の回避距離を計算する。
        const engine::Vector3 direction = NormalizePlanar(player.GetForwardDirection());
        const engine::Vector3 start = player.GetWorldPosition();
        engine::PhysicsQueryFilter filter;
        filter.mask = engine::collisionCategories::Get("ground");
        filter.ignoredObject = &player;
        const engine::RaycastHit hit = engine::CollisionManager::GetInstance()->SphereCast(start + engine::Vector3{0.0f, 0.65f, 0.0f}, 0.48f,
                                                                           direction, 9.5f, filter);
        // 非接触時は9.5、接触時は接触距離から0.75を引いた値と1.15の大きい方を使う。
        const float distance = hit.isHit ? std::max(1.15f, hit.distance - 0.75f) : 9.5f;
        const engine::Vector3 destination = start + direction * distance;
        // 移動前後に別の表示を置き、到着地点で無敵と範囲攻撃を開始します。
        SpawnEffect("Resources/json/effect/player/copy/thunder/effect_player_thunder_evade_cross.json", start + engine::Vector3{0.0f, 0.62f, 0.0f});
        SpawnEffect("Resources/json/effect/player/copy/thunder/effect_player_thunder_evade_seal.json",
                    destination + engine::Vector3{0.0f, 0.08f, 0.0f});
        EmitDirectedPreset("player/copy/thunder/player_thunder_evade_sparks", start, direction, 1.0f);
        player.SetTranslate(destination);
        player.SetMoveYaw(std::atan2(direction.x, direction.z));
        player.PlaySlimeAbilityMotion("player_ability_thunder_evade", false, 1.0f, 0.02f, 0.06f);
        player.StartEvasionInvincibility(0.24f);
        player.TriggerSlimeImpulse({1.95f, 0.68f, 1.95f}, 0.13f);
        DamageSphere(player, destination + engine::Vector3{0.0f, 0.55f, 0.0f}, 2.25f, 5.0f, DamageType::kElectric, 9.0f, 5.0f);
        evadeCooldown_ = 0.85f;
    }

    engine::Vector3 lineDirection_{0.0f, 0.0f, 1.0f};
    std::unordered_set<engine::Object3D*> lineHitTargets_;
    float lineCooldown_ = 0.0f;
    float dischargeCooldown_ = 0.0f;
    float evadeCooldown_ = 0.0f;
    CopyAttackRecovery recovery_;
    engine::Vector3 lineOrigin_{}; // 落雷の予兆を出したフレームのワールド位置。
    float lineTimer_ = 0.0f;
    MeshEffectManager::EffectScopeId sparkScope_ = MeshEffectManager::kInvalidEffectScope;
    bool primaryHeld_ = false;
    bool charging_ = false;
    float chargeTime_ = 0.0f;
    float sparkVisualTime_ = 0.0f;
    float sparkEffectTimer_ = 0.0f;
    float sparkDamageTimer_ = 0.0f;
    std::int32_t strikeIndex_ = 0;
    std::int32_t strikeCount_ = 9;
};
} // namespace

namespace copyAbilityDetail {
std::unique_ptr<ICopyAbilitySession> CreateThunderSlimeCopyAbility(const EnemyAttackProfile& profile) {
    // コピー獲得ごとに溜め・連続落雷の状態を分ける。
    return std::make_unique<ThunderSlimeCopyAbility>(profile);
}
} // namespace copyAbilityDetail
