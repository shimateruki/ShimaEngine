#pragma once
#include <cstdint>
#ifdef USE_IMGUI
#include "SceneManager.h"
#include "game/system/scene/SceneGameplayAccess.h"
#include "game/actor/enemy/spawn/EnemyFactory.h"
#include "EnemyFireSlime.h"
#include "game/actor/enemy/combat/ISequencePreviewEnemy.h"
#include "Player.h"
#include "game/system/combat/BulletManager.h"
#include "game/system/rules/EventManager.h"
#include "WinApp.h"
#include <fstream>

// 明示的な検証起動だけで実GAMEPLAYのAI、Bullet、Collision、GameRuleを通します。
// プロファイルの復元は起動スクリプトがfinallyで担当します。
namespace attackLifecycleRuntimeProbe {
inline bool Enabled() {
    static const bool kEnabled = GetEnvironmentVariableA("CG2_ATTACK_LIFECYCLE_TEST", nullptr, 0) > 0;
    return kEnabled;
}
inline bool WantsPlay(SceneManager* manager) {
    return Enabled() && manager && !manager->IsTransitioning() && manager->GetCurrentSceneName() == "GAMEPLAY";
}
inline void AfterDraw(SceneManager* manager, const std::string& path) {
    if (!Enabled() || !manager)
        return;
    static std::ofstream log(path, std::ios::trunc);
    static ULONGLONG sBegan = GetTickCount64();
    static std::int32_t sPhase = 0, sFrames = 0;
    static BaseEnemy* sEnemy = nullptr;
    static float sHpBefore = 0;
    static std::int32_t sDamageEvents = 0;
    static EventManager::SubscriptionId sDamageSubscription = EventManager::kInvalidSubscriptionId;
    static bool sSawBullet = false, sSawAction = false, sEnded = false;
    static EnemyAttackProfile sAuthored;
    static bool sDone = false;
    if (sDone)
        return;
    auto finish = [&](const std::string& message) {
        EventManager::GetInstance()->Unsubscribe(sDamageSubscription);
        sDamageSubscription = EventManager::kInvalidSubscriptionId;
        log << message << std::endl;
        sDone = true;
        WinApp::CloseNow();
    };
    if (GetTickCount64() - sBegan > 300000) {
        finish("FAIL: lifecycle timeout");
        return;
    }
    if (manager->IsTransitioning())
        return;
    if (sPhase == 0) {
        manager->ChangeScene("GAMEPLAY");
        sPhase = 1;
        log << "REQUEST GAMEPLAY" << std::endl;
        return;
    }
    auto* scene = manager->GetCurrentScene();
    if (!scene || manager->GetCurrentSceneName() != "GAMEPLAY")
        return;
    auto* player = GetScenePlayer(scene);
    if (!player || !player->GetParameters()) {
        finish("FAIL: no runtime player");
        return;
    }
    if (sPhase == 1) {
        // 入場ムービーの無敵・操作ロックが解けてから通常の被弾を検証します。
        if (++sFrames < 240 || player->IsCinematicLocked() || !player->IsControlActive())
            return;
        for (auto& object : scene->GetObjects())
            if (auto* other = dynamic_cast<BaseEnemy*>(object.get())) {
                other->SetIsVisible(false);
                other->SetCollisionMask(0);
            }
        std::string error;
        if (!sAuthored.LoadForEnemy("FireSlime", &error)) {
            finish("FAIL: " + error);
            return;
        }
        auto attack = *sAuthored.FindAttack("fireball");
        attack.id = "runtime_authored_attack";
        attack.displayName = "保存再読込の検証用攻撃";
        attack.sequence.priority = 999;
        attack.cooldown = 30;
        attack.statusEffectType = "none"; // 継続ダメージと直撃の結果を分離します。
        attack.sequence.events.front().projectile.damage = 2;
        attack.sequence.events.front().name = "RuntimeAuthoredProjectile";
        sAuthored.attacks.push_back(attack);
        sDamageSubscription = EventManager::GetInstance()->Subscribe([](const DamageEvent& event) {
            const auto* bullet = dynamic_cast<const Bullet*>(event.attacker);
            if (bullet && bullet->GetName() == "RuntimeAuthoredProjectile")
                ++sDamageEvents;
        });
        sPhase = 2;
        sFrames = 0;
    }
    if (sPhase == 2 || sPhase == 4) {
        const bool hitEnabled = sPhase == 2;
        if (sEnemy) {
            sEnemy->SetIsVisible(false);
            sEnemy->SetCollisionMask(0);
        }
        auto* attack = sAuthored.FindAttack("runtime_authored_attack");
        attack->sequence.events.front().projectile.collisionEnabled = hitEnabled;
        const auto profilePath = EnemyAttackProfile::GetDefaultPath("FireSlime");
        std::string error;
        if (!sAuthored.SaveToFile(profilePath, &error)) {
            finish("FAIL save: " + error);
            return;
        }
        EnemyAttackProfile reopened;
        if (!reopened.LoadFromFile(profilePath, &error)) {
            finish("FAIL reopen: " + error);
            return;
        }
        const auto* saved = reopened.FindAttack(attack->id);
        if (!saved || saved->sequence.events.front().projectile.collisionEnabled != hitEnabled ||
            !saved->sequence.actorControl.enabled) { finish("FAIL: saved data mismatch"); return; }
        EnemyAttackProfile::InvalidateCache("FireSlime");
        auto created = EnemyFactory::GetInstance()->CreateEnemy("FireSlime", scene->GetObject3DCommon());
        if (!created || !created->GetParameters()) {
            finish("FAIL: runtime enemy initialization");
            return;
        }

        // ステージの障害物から離し、実PlayerのCollider設定は変更しません。
        player->SetTranslate({0, 100, 0});
        player->SetVelocity({});
        player->SetGravity(0);
        player->SetMaxFallSpeed(0);
        player->SetParameter(&engine::EntityParameter::gravity, player->SetParameter(&engine::EntityParameter::maxFallSpeed, 0));
        player->SetParameter(&engine::EntityParameter::hp, player->SetParameter(&engine::EntityParameter::maxHp, 20));
        player->SetTutorialSafetyEnabled(false);
        player->SetDamageInvincible(false);
        player->SetDashInvincible(false);
        player->SetGuarding(false);
        player->UpdateWorldMatrix(true);
        created->SetTranslate({0, 100, -8});
        created->SetVelocity({});
        created->SetGravity(0);
        created->SetMaxFallSpeed(0);
        created->SetParameter(&engine::EntityParameter::gravity, created->SetParameter(&engine::EntityParameter::maxFallSpeed, created->SetParameter(&engine::EntityParameter::speed, 0)));
        created->SetParameter(&engine::EntityParameter::detectionRange, 30);
        created->SetDetectionRange(30);
        created->SetTarget(player);
        created->ApplyManagedScale({2, 2, 2});
        created->SetGrounded(true);
        created->UpdateWorldMatrix(true);
        sEnemy = created.get();
        scene->AddObject(std::move(created));
        sHpBefore = player->GetHp();
        sDamageEvents = 0;
        sSawBullet = sSawAction = sEnded = false;
        sFrames = 0;
        ++sPhase;
        log << "SAVED REOPENED SPAWNED collision=" << hitEnabled << " hp=" << sHpBefore << std::endl;
        return;
    }
    ++sFrames;
    const auto* sequence = dynamic_cast<const ISequencePreviewEnemy*>(sEnemy);
    const char* stateLabel = sequence ? sequence->GetSequenceStateLabel() : nullptr;
    const std::string state = stateLabel ? stateLabel : "";
    if (state.find("実行中") != std::string::npos)
        sSawAction = true;
    if (sSawAction && state.find("終了") != std::string::npos)
        sEnded = true;
    for (const auto& bullet : BulletManager::GetInstance()->GetBullets())
        if (bullet->GetName() == "RuntimeAuthoredProjectile")
            sSawBullet = true;
    if (sFrames % 120 == 0)
        log << "FRAME " << sFrames << " hp=" << player->GetHp() << " state=" << state << " bullet=" << sSawBullet << std::endl;
    if (!sEnded || sFrames < 420)
        return;
    if (!sSawBullet || !sSawAction) {
        finish("FAIL: runtime did not execute authored attack");
        return;
    }
    const float lost = sHpBefore - player->GetHp();
    log << "RESULT collision=" << (sPhase == 3) << " hpLost=" << lost << " damageEvents=" << sDamageEvents << std::endl;
    if (sPhase == 3) {
        if (std::abs(lost - 2.0f) > 0.01f || sDamageEvents != 1) {
            finish("FAIL: runtime projectile damage mismatch");
            return;
        }
        sPhase = 4;
    } else {
        if (lost != 0 || sDamageEvents != 0) {
            finish("FAIL: disabled collider caused damage");
            return;
        }
        for (const auto& bullet : BulletManager::GetInstance()->GetBullets())
            if (bullet->GetName() == "RuntimeAuthoredProjectile") {
                finish("FAIL: lifetime did not retire projectile");
                return;
            }
        finish("PASS: save/reopen, AI selection, windup/body clip, shot, real player damage, end, collider OFF and lifetime");
    }
}
}
#endif
