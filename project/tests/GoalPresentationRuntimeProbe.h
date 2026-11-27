#pragma once
#include <cstdint>
#ifdef USE_IMGUI
#include "game/scene/gameplay/GamePlayScene.h"
#include "game/system/progress/StageManager.h"
#include "game/system/progress/GameDataManager.h"
#include "game/system/scene/SceneGameplayAccess.h"
#include "tests/GateEntryRuntimeProbe.h"
#include "MeshEffectManager.h"
#include "GpuParticleManager.h"
#include "SceneManager.h"
#include "WinApp.h"
#include "CollisionManager.h"
#include "CollisionConfig.h"
#include <fstream>
#include <stdexcept>

// 専用worktreeの明示起動で、実モデル・実カメラ・GPU描画とプレビュー復元を検証する。
namespace goalPresentationRuntimeProbe {
inline bool Enabled() { return GetEnvironmentVariableA("CG2_GOAL_PRESENTATION_TEST",nullptr,0)>0; }
/// <summary>通常ゴールのプレビューを開始・停止し、検証用にカメラ、経過時間、王冠を取得する。</summary>
struct Access {
    static void Begin(GamePlayScene* game) { game->StartGoalPresentationPreview(); }
    static void End(GamePlayScene* game) { game->StopGoalPresentationPreview(); }
    static bool HasSceneOwnedCamera(GamePlayScene* game) { return game->goalPresentationCamera_ != nullptr; }
    static float Time(GamePlayScene* game) { return game->goalPresentationTimer_.Elapsed(); }
    static float TitleStart(GamePlayScene* game) { return game->GetGoalResultTitleStartTime(); }
    static float Ready(GamePlayScene* game) { return game->GetGoalRewardReadyTime(); }
    static engine::Object3D* Crown(GamePlayScene* game) { return game->FindGoalCrownObject(); }
};
inline void Require(bool value,const char* error) { if (!value) throw std::runtime_error(error); }
inline void CheckFrame(engine::Object3D* object,engine::Camera* camera,float rightEdge) {
    const auto lo=object->GetModel()->GetLocalAabbMin(),hi=object->GetModel()->GetLocalAabbMax();
    for(std::int32_t i=0;i<8;++i) {
        const auto world=engine::Math::Transform({(i&1)?hi.x:lo.x,(i&2)?hi.y:lo.y,(i&4)?hi.z:lo.z},object->GetWorldMatrix());
        const auto view=engine::Math::Transform(world,camera->GetViewMatrix());
        Require(view.z>0.05f,"subject went behind camera");
        const auto ndc=engine::Math::Transform(world,camera->GetViewProjectionMatrix());
        Require(std::isfinite(ndc.x) && std::isfinite(ndc.y),"invalid projection");
        if(!(ndc.x>=-0.90f && ndc.x<=rightEdge && ndc.y>=-0.90f && ndc.y<=0.90f))
            throw std::runtime_error(object->GetName()+" ndc="+std::to_string(ndc.x)+","+std::to_string(ndc.y)+" eye="+std::to_string(camera->GetEye().x)+","+std::to_string(camera->GetEye().y)+","+std::to_string(camera->GetEye().z));
    }
}
inline float FootHeight(Player* p) {
    const auto lo=p->GetModel()->GetLocalAabbMin(),hi=p->GetModel()->GetLocalAabbMax();
    return engine::Math::Transform({(lo.x+hi.x)*0.5f,lo.y,(lo.z+hi.z)*0.5f},p->GetWorldMatrix()).y;
}
inline void AfterDraw(SceneManager* manager,const std::string& path) {
    static std::ofstream log(path,std::ios::trunc);
    static ULONGLONG sStarted=GetTickCount64();
    static std::int32_t sPhase=0,sStage=0,sFrames=0;
    static bool sDone=false,sLanding=false,sApex=false,sResult=false;
    static std::int32_t sCleared=0;
    static engine::Vector3 sCrownPos{},sCrownScale{},sCrownRotation{},sPlayerPos{},sPlayerRotation{};
    static engine::Camera* sOriginalCamera=nullptr;
    static engine::Camera* sPlaybackCamera=nullptr;
    static std::string idle1,sIdle2;
    static float sSettledFoot=0,sBaseRoughness=0,sBaseEnvIntensity=0;
    static bool sBaseEnvMap=false,sFootCaptured=false;
    if(sDone || !manager)return;
    auto finish=[&](const std::string& message){log<<message<<std::endl;sDone=true;WinApp::CloseNow();};
    try {
        Require(GetTickCount64()-sStarted<360000,"goal presentation test timeout");
        if(manager->IsTransitioning())return;
        if(sPhase==0) {
            StageManager::GetInstance()->SetCurrentStageById(sStage==0?"stage1":"stage2");
            manager->ChangeScene("GAMEPLAY");sPhase=1;sFrames=0;return;
        }
        auto* game=dynamic_cast<GamePlayScene*>(manager->GetCurrentScene());
        if(!game)return;
        auto* player=GetScenePlayer(game);auto* crown=Access::Crown(game);
        if(sPhase==1) {
            if(++sFrames<5 || !player || !player->IsControlActive() || player->IsCinematicLocked())return;
            Require(player && crown && player->GetModel() && crown->GetModel(),"missing goal scene models");
            sCrownPos=crown->GetTranslate();sCrownScale=crown->GetScale();sCrownRotation=crown->GetRotation();
            idle1=crown->GetMeshEffect1Name();sIdle2=crown->GetMeshEffect2Name();
            sBaseRoughness=crown->GetRoughness();sBaseEnvIntensity=crown->GetEnvIntensity();sBaseEnvMap=crown->GetEnableEnvMap();
            sOriginalCamera=CameraManager::GetInstance()->GetActiveCameraOverride();
            // 空中取得を再現し、配置サイズ1と2.8の両方を実アセットで通す。
            auto toward=player->GetWorldPosition()-crown->GetWorldPosition();toward.y=0;
            toward=engine::Math::Length(toward)>0.01f?engine::Math::Normalize(toward):engine::Vector3{-1,0,0};
            auto pickupPosition=crown->GetWorldPosition()+toward*2.0f;
            engine::PhysicsQueryFilter filter;filter.mask=engine::collisionCategories::Get("ground");filter.ignoredObject=player;
            const auto floor=engine::CollisionManager::GetInstance()->Raycast(pickupPosition+engine::Vector3{0,12,0},{0,-1,0},60.0f,filter);
            Require(floor.isHit,"no floor below crown approach");
            pickupPosition.y=floor.hitPoint.y-player->GetModel()->GetLocalAabbMin().y*player->GetScale().y+2.5f;
            player->SetTranslate(pickupPosition);player->SetVelocity({0,-5,0});player->UpdateWorldMatrix(true);
            sPlayerPos=player->GetTranslate();sPlayerRotation=player->GetRotation();
            sCleared=GameDataManager::GetInstance()->GetClearedStageCount();
            Access::Begin(game);Require(game->IsGoal(),"preview did not start");
            sPlaybackCamera = CameraManager::GetInstance()->GetActiveCameraOverride();
            Require(sPlaybackCamera && sPlaybackCamera != sOriginalCamera && !Access::HasSceneOwnedCamera(game),
                    "normal goal did not use a single player-owned camera");
            log<<"START stage="<<sStage+1<<" authoredCrownScale="<<sCrownScale.x<<std::endl;
            sPhase=2;sFrames=0;sLanding=sApex=sResult=false;return;
        }
        if(sPhase==2) {
            const float t=Access::Time(game);
            auto* camera=CameraManager::GetInstance()->GetActiveCamera();
            Require(camera && player && crown,"presentation lost actors or camera");
            Require(camera == sPlaybackCamera, "presentation camera changed before leaving the result screen");

            if(t>0.05f) {
                CheckFrame(player,camera,t>=Access::TitleStart(game)?0.15f:0.90f);
                CheckFrame(crown,camera,t>=Access::TitleStart(game)?0.15f:0.90f);
            }
            if(t>1.0f) Require(crown->GetMeshEffect1Name().empty() && crown->GetMeshEffect2Name().empty(),"idle crown effects were left running");
            if(t>=1.1f && !sFootCaptured) { sSettledFoot=FootHeight(player);sFootCaptured=true; }
            if(t>=2.35f && !sLanding) {
                gateEntryRuntimeProbe::Capture(path+".stage"+std::to_string(sStage+1)+".wear.png");sLanding=true;
                log<<"PASS crown moved and idle effects stopped"<<std::endl;
            }
            if(t>=3.08f && !sApex) {
                gateEntryRuntimeProbe::Capture(path+".stage"+std::to_string(sStage+1)+".jump.png");sApex=true;
            }
            if(t>=4.3f && !sResult) {
                Require(std::abs(FootHeight(player)-sSettledFoot)<0.10f,"result pose did not settle on the ground");
                const auto bodySize=player->GetModel()->GetLocalAabbMax()-player->GetModel()->GetLocalAabbMin();
                const auto crownSize=crown->GetModel()->GetLocalAabbMax()-crown->GetModel()->GetLocalAabbMin();
                const float ratio=crownSize.x*std::abs(crown->GetScale().x)/(bodySize.x*std::abs(player->GetScale().x));
                Require(ratio>0.65f && ratio<0.98f,"worn crown size did not match player");
                gateEntryRuntimeProbe::Capture(path+".stage"+std::to_string(sStage+1)+".result.png");sResult=true;
                log<<"PASS wearRatio="<<ratio<<" foot="<<FootHeight(player)<<" screen bounds fit with UI"<<std::endl;
            }
            if(t>=Access::Ready(game)+0.36f) {
                Require(sLanding && sApex && sResult,"key poses were not rendered");
                Access::End(game);
                Require(!game->IsGoal() && !player->IsCinematicLocked(),"preview retained cinematic state");
                Require(engine::Math::Length(crown->GetTranslate()-sCrownPos)<0.001f && engine::Math::Length(crown->GetScale()-sCrownScale)<0.001f,"crown pose was not restored");
                Require(engine::Math::Length(player->GetTranslate()-sPlayerPos)<0.001f,"player position was not restored");
                Require(crown->GetMeshEffect1Name()==idle1 && crown->GetMeshEffect2Name()==sIdle2,"idle effects were not restored");
                Require(crown->GetEnableEnvMap()==sBaseEnvMap && std::abs(crown->GetRoughness()-sBaseRoughness)<0.001f && std::abs(crown->GetEnvIntensity()-sBaseEnvIntensity)<0.001f,"crown material was not restored");
                Require(CameraManager::GetInstance()->GetActiveCameraOverride()==sOriginalCamera,"camera was not restored");
                Require(GameDataManager::GetInstance()->GetClearedStageCount()==sCleared,"preview changed progress");
                log<<"PASS restore and progress stage="<<sStage+1<<std::endl;
                if(++sStage==2){finish("PASS: goal clear crown fit, airborne pickup, safe camera, VFX lifecycle, stable result and preview restoration");return;}
                sSettledFoot=0;sFootCaptured=false;sPhase=0;return;
            }
            // 本編と同じ実更新・描画で次のフレームを待つ。
        }
    } catch(const std::exception& error) { gateEntryRuntimeProbe::Capture(path+".failed.png"); finish(std::string("FAIL: ")+error.what()); }
}
}
#endif
