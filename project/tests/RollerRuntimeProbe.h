#pragma once
#include <cstdint>
#ifdef USE_IMGUI
#include "SceneManager.h"
#include "game/system/scene/SceneGameplayAccess.h"
#include "game/system/progress/StageManager.h"
#include "Player.h"
#include "WinApp.h"
#include "CollisionManager.h"
#include <fstream>
#include <cmath>

namespace rollerRuntimeProbe {
inline void AfterDraw(SceneManager* manager, const std::string& path) {
    // 専用起動だけで乗車し、入力なしで回転・離脱・固定軸・処理時間を記録する。
    static std::ofstream log(path, std::ios::trunc);
    static std::int32_t sPhase=0, sFrames=0;
    static ULONGLONG sBegan=GetTickCount64(), sLast=sBegan;
    static engine::Vector3 sOrigin{};
    static bool sMoved=false, sDetached=false;
    if (sPhase==3 || !manager || manager->IsTransitioning()) return;
    auto finish=[&](const char* text) { log<<text<<std::endl; sPhase=3; WinApp::CloseNow(); };
    if (GetTickCount64()-sBegan>120000) { finish("FAIL timeout"); return; }
    if (sPhase==0) {
        StageManager::GetInstance()->SetCurrentStageById("stage2");
        manager->ChangeScene("GAMEPLAY"); sPhase=1; return;
    }
    auto* scene=manager->GetCurrentScene();
    auto* player=GetScenePlayer(scene);
    if (!player || manager->GetCurrentSceneName()!="GAMEPLAY") return;
    engine::Object3D* roller=nullptr;
    for (const auto& o:scene->GetObjects())
        if (o && o->GetName()=="Stage2_Route_11_RollingBasalt") roller=o.get();
    if (!roller) { finish("FAIL missing roller"); return; }
    if (sPhase==1) {
        // 入場演出は操作ロック解除後も続くため、通常の物理更新へ戻るまで待つ。
        if (++sFrames<600 || player->IsCinematicLocked()) return;
        sOrigin=roller->GetWorldPosition();
        for (const auto& o:scene->GetObjects()) {
            if (o && o->GetName()=="Stage2_Route_11_RollingBasalt_Collision") {
                const auto c=o->GetWorldPosition(); const auto bounds=o->GetAabb();
                log<<"COLLIDER position="<<c.x<<','<<c.y<<','<<c.z<<" attr="<<o->GetCollisionAttribute()
                   <<" volume="<<o->GetCollider()->HasModelVolume()<<" top="<<bounds.max.y<<std::endl;
            }
        }
        auto position=sOrigin;
        position.y=roller->GetAabb().max.y-(player->GetAabb().min.y-player->GetWorldPosition().y)+0.05f;
        player->DebugPrepareForTeleport();
        player->SetIsControlActive(true);
        player->SetGrounded(false);
        player->SetTranslate(position); player->SetVelocity({}); player->UpdateWorldMatrix(false);
        player->SetDamageInvincible(true);
        sPhase=2; sFrames=0; sLast=GetTickCount64();
        log<<"START origin="<<sOrigin.x<<','<<sOrigin.y<<','<<sOrigin.z
           <<" spawn="<<position.x<<','<<position.y<<','<<position.z<<std::endl;
        return;
    }
    const auto center=roller->GetWorldPosition(), r=roller->GetRotation(), p=player->GetWorldPosition();
    if (sFrames<5) for (const auto& o:scene->GetObjects()) {
        if (o && o->GetName()=="Stage2_Route_11_RollingBasalt_Collision") {
            float distance=0; engine::Vector3 normal{};
            const bool ray=o->GetCollider()->RaycastModelVolume(sOrigin+engine::Vector3{0,10,0},{0,-1,0},20,distance,normal);
            const auto contact=player->CheckCollision(o.get());
            const auto a=o->GetAabb(), b=player->GetAabb();
            engine::PhysicsQueryFilter filter;
            filter.mask=engine::collisionCategories::Get("ground");
            filter.ignoredObject=player;
            const auto overlaps=engine::CollisionManager::GetInstance()->OverlapAabb(b,filter);
            log<<"BOUNDS floor="<<a.min.x<<','<<a.max.x<<','<<a.min.y<<','<<a.max.y<<','<<a.min.z<<','<<a.max.z
               <<" player="<<b.min.x<<','<<b.max.x<<','<<b.min.y<<','<<b.max.y<<','<<b.min.z<<','<<b.max.z
               <<" overlaps="<<overlaps.size()<<" groundBit="<<engine::collisionCategories::Get("ground")<<std::endl;
            log<<"PLAYER registered="<<(std::find(engine::CollisionManager::GetInstance()->GetObjects().begin(),
                engine::CollisionManager::GetInstance()->GetObjects().end(),player)!=engine::CollisionManager::GetInstance()->GetObjects().end())
               <<" types="<<static_cast<std::int32_t>(player->GetColliderType())<<','<<static_cast<std::int32_t>(o->GetColliderType())
               <<" playing="<<manager->IsPlaying()<<" singleton="<<SceneManager::GetInstance()->IsPlaying()<<std::endl;
            log<<"QUERY hull="<<o->GetCollider()->GetWorldHull().size()<<" ray="<<ray<<" distance="<<distance
               <<" contact="<<contact.isColliding<<" normal="<<normal.y
               <<" push="<<contact.normal.y<<" mask="<<player->GetCollisionMask()
               <<" otherMask="<<o->GetCollisionMask()<<" attr="<<player->GetCollisionAttribute()
               <<" static="<<o->IsStatic()<<" pStatic="<<player->IsStatic()
               <<" locked="<<player->IsCinematicLocked()
               <<" registered="<<(std::find(engine::CollisionManager::GetInstance()->GetObjects().begin(),
                    engine::CollisionManager::GetInstance()->GetObjects().end(),o.get())!=engine::CollisionManager::GetInstance()->GetObjects().end())<<std::endl;
        }
    }
    if (std::abs(center.x-sOrigin.x)+std::abs(center.y-sOrigin.y)+std::abs(center.z-sOrigin.z)>0.02f) {
        finish("FAIL axis translated"); return;
    }
    sMoved |= std::abs(r.x)>0.05f;
    sDetached |= sMoved && !player->IsGrounded() && p.y<sOrigin.y+2;
    if (++sFrames<=15 || sFrames%30==0) {
        const auto now=GetTickCount64();
        log<<sFrames<<" rot="<<r.x<<','<<r.y<<','<<r.z<<" player="<<p.x<<','<<p.y<<','<<p.z
           <<" grounded="<<player->IsGrounded()<<" dead="<<player->IsDead()<<" avgMs="<<(now-sLast)/(sFrames<=15 ? 1.0:30.0)<<std::endl;
        sLast=now;
    }
    if (sDetached) { finish("PASS fixed axis, rotation and rider fall"); return; }
    if (!sMoved && p.y<-6) { finish("FAIL passed through cylinder"); return; }
    if (sFrames>=480) finish(sMoved ? "FAIL rider never fell" : "FAIL rotation never started");
}
}
#endif
