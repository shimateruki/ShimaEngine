#pragma once

// 各ICopyAbilitySessionが使う照準・ダメージ通知・表示生成の補助。能力の入力状態や寿命は各セッションが管理する。
#include "game/actor/combat/CombatMath.h"
#include <numbers>
#include "CopyAbilitySession.h"
#define NOMINMAX
#include "game/actor/player/copy/PlayerCopyAbilityController.h"

#include "BaseEnemy.h"
#include "BaseScene.h"
#include "game/system/combat/BulletManager.h"
#include "CollisionConfig.h"
#include "CollisionManager.h"
#include "DebrisEffectManager.h"
#include "game/actor/enemy/combat/EnemyAttackProfile.h"
#include "EnemyBomb.h"
#include "game/actor/enemy/spawn/EnemyFactory.h"
#include "game/actor/enemy/boss/false_king/EnemyFalseKingSlime.h"
#include "game/crown/CrownFinaleTuning.h"
#include "game/system/rules/EventManager.h"
#include "GpuParticleManager.h"
#include "game/system/combat/HitEffectDirector.h"
#include "MeshEffectManager.h"
#include "Player.h"
#include "game/actor/player/state/PlayerState.h"
#include "SceneManager.h"
#include "VfxSequencer.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_set>
#include <utility>

// 命中・演出の共通処理。能力の状態は保持せず、各セッションが管理します。
namespace copyAbilityDetail {
constexpr float kPi = std::numbers::pi_v<float>;

using combatMath::NormalizePlanar;

// コピー用の設定を取得する。旧プロフィールに技がなければ従来の移動を維持する。
inline PlayerAttackMovement CopyAttackMovement(const EnemyAttackProfile& profile, const char* id) {
    const auto* attack = profile.FindAttack(id);
    return attack ? attack->playerMovement : PlayerAttackMovement{};
}

// 能力の反動区間だけを保持する。発動条件と被弾・解除時の処理は各セッションが決める。
class CopyAttackRecovery {
public:
    void Start(PlayerAttackMovement movement, bool blocksMovementActions = true) {
        movement.blocksMovementActions = movement.enabled && blocksMovementActions;
        movement_ = movement;
        remaining_ = movement.enabled ? movement.duration : 0.0f;
    }

    void Update(float deltaTime) { remaining_ = std::max(0.0f, remaining_ - deltaTime); }
    void Clear() { remaining_ = 0.0f; }
    bool Active() const { return remaining_ > 0.0f; }
    PlayerAttackMovement Movement() const { return Active() ? movement_ : PlayerAttackMovement{}; }

private:
    PlayerAttackMovement movement_;
    float remaining_ = 0.0f; // 発射や着地から反動が終わるまでの残り秒。
};

inline float SmoothStep(float value) { return combatMath::SmoothStep01(value); }

inline void SpawnEffect(const char* path, const engine::Vector3& position, const engine::Vector3& rotation = {},
                        const engine::Vector3& scale = {1.0f, 1.0f, 1.0f}) {
    // エフェクト管理が利用できる場合に限り、開始した再生のIDを受け取る。
    if (MeshEffectManager* effects = MeshEffectManager::GetInstance()) {
        effects->SpawnEffectAt(path, position, rotation, scale);
    }
}

inline void SpawnScopedEffect(MeshEffectManager::EffectScopeId scopeId, const char* path, const engine::Vector3& position,
                              const engine::Vector3& rotation = {}, const engine::Vector3& scale = {1.0f, 1.0f, 1.0f}) {
    // 管理側が利用できる場合は、後で一括停止するために再生をこのコピー能力の管理範囲へ登録する。
    if (MeshEffectManager* effects = MeshEffectManager::GetInstance()) {
        effects->SpawnEffectAtScoped(scopeId, path, position, rotation, scale);
    }
}

inline void EmitPreset(const char* preset, const engine::Vector3& position) {
    // 初期化前に能力演出が要求されても GPU エミッタへは渡さない。
    GpuParticleManager* particles = GpuParticleManager::GetInstance();
    // 管理側が利用できる場合に限り、登録していた再生を停止する。
    if (particles && particles->IsInitialized()) {
        particles->Emit(preset, position);
    }
}

inline void EmitDirectedPreset(const char* preset, const engine::Vector3& position, const engine::Vector3& direction,
                               float scale = 1.0f) {
    GpuParticleManager* particles = GpuParticleManager::GetInstance();
    // 管理側が存在し、初期化も済んでいる場合だけ、エフェクトの生成を依頼する。
    if (particles && particles->IsInitialized()) {
        particles->EmitDirected(preset, position, direction, scale);
    }
}

inline void SpawnDebrisOnGround(const char* presetName, const engine::Vector3& position, float lift = 0.04f) {
    // 地面を標本化して破片を少し浮かせ、床への埋まり込みを避ける。
    const engine::Vector3 groundPosition = HitEffectDirector::ResolveGroundEffectPosition(position);
    engine::Vector3 effectPosition = groundPosition;
    effectPosition.y += lift;
    // 破片エフェクトの管理側を取得できた場合に限り、地面から浮かせた位置へ生成する。
    if (DebrisEffectManager* debris = DebrisEffectManager::GetInstance()) {
        debris->SpawnOnGround(presetName, effectPosition, groundPosition.y);
    }
}

inline engine::Object3D* FindEnemyRoot(engine::Object3D* object) {
    // 当たった子オブジェクトから親をたどり、ダメージを受ける敵本体を返す。
    for (engine::Object3D* current = object; current; current = current->GetParent()) {
        // 親方向にたどり、最初に見つかった敵本体を衝突先として扱う。
        if (dynamic_cast<BaseEnemy*>(current)) {
            return current;
        }
    }
    return nullptr;
}

inline void DamageEnemy(Player& player, engine::Object3D* target, float damage, DamageType type, const engine::Vector3& knockback,
                        const StatusEffectApplication& status = {}) {
    // 敵本体が見つからない場合は、ダメージ通知を送らない。
    if (!target) {
        return;
    }
    // 送信元と攻撃情報を組み立て、その場でダメージを通知する。
    DamageEvent event;
    event.target = target;
    event.attacker = &player;
    event.damageAmount = damage;
    event.damageType = type;
    event.knockbackVelocity = knockback;
    event.statusEffect = status;
    EventManager::GetInstance()->Dispatch(event);
}

// 重複 Collider を持つ敵には一度だけダメージを送ります。
inline void DamageSphere(Player& player, const engine::Vector3& center, float radius, float damage, DamageType type,
                         float horizontalKnockback, float verticalKnockback) {
    // 敵カテゴリに対象を絞り、子Colliderの重複は本体ごとに除く。
    engine::PhysicsQueryFilter filter;
    filter.mask = engine::collisionCategories::Get("enemy");
    filter.ignoredObject = &player;
    std::unordered_set<engine::Object3D*> damaged;
    for (const engine::PhysicsOverlapHit& hit : engine::CollisionManager::GetInstance()->OverlapSphere(center, radius, filter)) {
        engine::Object3D* target = FindEnemyRoot(hit.object);
        // 敵本体が見つからない衝突と、同じ敵の別コライダーへの重複命中を飛ばす。
        if (!target || !damaged.insert(target).second) {
            continue;
        }
        // 球の中心から外へ押し出す水平成分と、指定された上向き成分を命中通知へ渡す。
        const engine::Vector3 direction = NormalizePlanar(target->GetWorldPosition() - center);
        DamageEnemy(player, target, damage, type,
                    {direction.x * horizontalKnockback, verticalKnockback, direction.z * horizontalKnockback});
    }
}

inline const EnemyAttackDefinition& FindAttackOrFallback(const EnemyAttackProfile& profile, const char* id,
                                                         const EnemyAttackDefinition& fallback) {
    const EnemyAttackDefinition* attack = profile.FindAttack(id);
    // 指定された攻撃データを使用し、省略された場合は呼び出し元の既定値を使用する。
    return attack ? *attack : fallback;
}

inline StatusEffectApplication MakeBurnStatus(const EnemyAttackDefinition& attack) {
    // 継続時間、ダメージ間隔、一回分のダメージ、表示プリセットは攻撃定義から引き継ぐ。
    StatusEffectApplication status;
    // 燃焼指定がある場合だけ、状態異常の種類を燃焼に設定する。
    if (attack.statusEffectType == "burning") {
        status.type = StatusEffectType::kBurning;
    }
    status.duration = attack.statusDuration;
    status.tickInterval = attack.statusTickInterval;
    status.tickDamage = attack.statusTickDamage;
    status.vfxPreset = attack.statusVfx;
    return status;
}

// 炎スライムのコピー能力が発射する弾の見た目と着弾演出を組み立てます。
inline BulletVisualConfig MakeFireVisual() {
    BulletVisualConfig visual;
    visual.materialType = 11;
    visual.color = {1.0f, 0.34f, 0.07f, 0.96f};
    visual.emissive = 2.8f;
    visual.visualScale = 1.28f;
    visual.effectType = 1.0f;
    visual.effectScale = 1.15f;
    visual.effectSoftness = 0.42f;
    visual.effectIntensity = 0.96f;
    visual.billboardScale = 0.68f;
    visual.trailPreset = "shared/fireball_trail_flame";
    visual.trailSecondaryPreset = "shared/fireball_trail_embers";
    visual.impactPreset = "shared/fireball_impact_flame";
    visual.impactSecondaryPreset = "shared/fireball_impact_sparks";
    visual.trailInterval = 0.025f;
    visual.trailSpeedScale = 1.08f;
    visual.trailReverseDirection = true;
    visual.impactDirected = false;
    return visual;
}

} // namespace copyAbilityDetail
