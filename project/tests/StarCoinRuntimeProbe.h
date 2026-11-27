#pragma once
#include <cstdint>
#ifdef USE_IMGUI
#include "game/scene/gameplay/GamePlayScene.h"
#include <numbers>
#include "game/system/progress/GameDataManager.h"
#include "game/system/progress/StageManager.h"
#include "game/system/progress/StarCoinPresentation.h"
#include "tests/GateEntryRuntimeProbe.h"
#include "GpuParticleManager.h"
#include "game/system/rules/EventManager.h"
#include "CollisionConfig.h"
#include "json.hpp"
#include <fstream>
#include <stdexcept>

// 明示起動した専用プロセスで、保存済みと未取得のモデル・粒子・再接触を確認する。
namespace starCoinRuntimeProbe {
inline bool Enabled() { return GetEnvironmentVariableA("CG2_SILVER_STAR_TEST",nullptr,0)>0; }
inline void Require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
inline engine::Object3D* Find(BaseScene* scene,std::int32_t index) {
    for(auto& object:scene->GetObjects())
        if(object && object->GetEventType()==EventType::kStarCoin && object->GetTargetId()==index)return object.get();
    return nullptr;
}
inline bool Near(float a,float b) { return std::abs(a-b)<0.001f; }
inline void AfterDraw(SceneManager* manager,const std::string& path) {
    static std::ofstream log(path,std::ios::trunc);
    static ULONGLONG sStart=GetTickCount64();
    static std::int32_t sPhase=0,sStage=0,sFrames=0,sSavedCount=0;
    static bool sDone=false;
    static std::unique_ptr<engine::Camera> sCamera;
    static engine::Camera* sPreviousCamera=nullptr;
    static engine::Object3D* sSilver=nullptr;
    static engine::Object3D* sGold=nullptr;
    auto finish=[&](const std::string& message){log<<message<<std::endl;sDone=true;CameraManager::GetInstance()->SetActiveCamera(sPreviousCamera);WinApp::CloseNow();};
    if(sDone || !manager)return;
    try {
        Require(GetTickCount64()-sStart<360000,"silver star test timeout");
        if(manager->IsTransitioning())return;
        auto* data=GameDataManager::GetInstance();
        if(sPhase==0) {
            const char* id=sStage==0?"stage1":"stage2";
            StageManager::GetInstance()->SetCurrentStageById(id);
            const std::int32_t stageIndex=StageManager::GetInstance()->GetCurrentStageIndex();
            for(std::int32_t i=0;i<3;++i)data->SetStarCoinCollected(stageIndex,i,i!=1);
            sSavedCount=data->GetCollectedStarCoinCount();
            manager->ChangeScene("GAMEPLAY");sPhase=1;sFrames=0;return;
        }
        auto* scene=dynamic_cast<GamePlayScene*>(manager->GetCurrentScene());
        if(!scene)return;
        auto* player=GetScenePlayer(scene);
        if(sPhase==1) {
            if(++sFrames<15 || !player || !player->IsControlActive())return;
            nlohmann::json authored;std::ifstream file("Resources/json/3Dobject/"+std::string(sStage==0?"stage1":"stage2")+"_object.json");file>>authored;
            for(std::int32_t i=0;i<3;++i) {
                auto* object=Find(scene,i);Require(object,"missing stage star ID");
                Require(starCoinPresentation::IsSilver(*object)==(i!=1),"save flag did not select model");
                if(i!=1) {
                    Require(object->GetGpuParticleName()==starCoinPresentation::kSilverIdle,"silver idle preset missing");
                    const auto c=object->GetColor();Require(c.z>=c.x && c.x>0.4f,"silver material still gold or too dark");
                    for(const auto& entry:authored.at("objects"))if(entry.value("name","")==object->GetName()) {
                        Require(object->GetCollisionAttribute()==entry.at("collisionAttribute").get<uint32_t>(),"authored collision attribute changed");
                        Require(object->GetCollisionMask()==entry.at("collisionMask").get<uint32_t>(),"authored collision mask changed");
                        const auto size=entry.at("collider").at("size");const auto actual=object->GetColliderConfig().size;
                        Require(Near(actual.x,size[0]) && Near(actual.y,size[1]) && Near(actual.z,size[2]),"model auto-fit changed pickup size");
                    }
                }
            }
            const auto& presets=GpuParticleManager::GetInstance()->GetPresets();
            Require(presets.count(starCoinPresentation::kSilverIdle) && presets.count(starCoinPresentation::kSilverGet),"silver GPU presets not loaded");
            sSilver=Find(scene,0);sGold=Find(scene,1);
            // 銀の状態で保存した配置でも、別セーブの未取得状態へ復元できる。
            const auto goldCollider=sGold->GetColliderConfig();
            starCoinPresentation::ApplyPreviouslyCollected(*sGold);
            starCoinPresentation::RestoreUncollectedIfSilver(*sGold);
            Require(!starCoinPresentation::IsSilver(*sGold) && sGold->GetModelName()==starCoinPresentation::kGoldModel,"silver authoring state leaked into fresh save");
            const auto restoredCollider=sGold->GetColliderConfig();
            Require(Near(restoredCollider.size.x,goldCollider.size.x) && Near(restoredCollider.size.y,goldCollider.size.y) && Near(restoredCollider.size.z,goldCollider.size.z),"gold restore changed collider");
            const auto center=player->GetWorldPosition()+engine::Vector3{0,3,8};
            for(auto* object:{sSilver,sGold}) {
                object->SetParent(nullptr,true);object->SetIsVisible(true);
                object->SetTranslate(center+engine::Vector3{object==sSilver?-2.6f:2.6f,0,0});
                object->SetRotation({(std::numbers::pi_v<float> * 0.5f),0,0});object->SetScale({0.05f,0.05f,0.05f});object->UpdateWorldMatrix(true);
            }
            sPreviousCamera=CameraManager::GetInstance()->GetActiveCameraOverride();
            sCamera=std::make_unique<engine::Camera>();sCamera->Initialize();sCamera->SetFovY(0.65f);
            engine::Camera::CameraOverrideParams view;view.duration=0;view.fixedEyePos=center+engine::Vector3{0,3,-17};
            view.fixedTargetPos=center;view.trackTargetX=view.trackTargetY=view.trackTargetZ=false;
            sCamera->StartOverride(view);sCamera->Update();CameraManager::GetInstance()->SetActiveCamera(sCamera.get());
            log<<"PASS stage="<<sStage+1<<" saved models, silver presets, fresh-save restore and authored collision preserved"<<std::endl;
            sPhase=2;sFrames=0;return;
        }
        if(sPhase==2) {
            if(++sFrames<45)return;
            gateEntryRuntimeProbe::Capture(path+".stage"+std::to_string(sStage+1)+".idle.png");
            // 実際のPlayer接触入口からGameRuleを通す。途中の再接触で寿命が戻らないことも確認する。
            for(auto* object:{sSilver,sGold}) {
                // 遭遇で出現するスターも、この取得テストの間だけ出現済みの状態へ揃える。
                object->SetCollisionAttribute(engine::collisionCategories::Get("trigger"));
                object->SetCollisionMask(engine::collisionCategories::Get("player"));
                player->SetTranslate(object->GetWorldPosition());player->SetVelocity({0,0,0});player->UpdateWorldMatrix(true);
                Require(player->OnCollision(object),"player contact rejected");
                Require(object->IsCollecting(),"contact did not start collection");
                Require(object->GetCollisionAttribute()==0 && object->GetCollisionMask()==0,"collection collision stayed enabled");
                Require(object->GetGpuParticleName().empty(),"idle particles remained during collection");
                object->Update(0.30f);
                PlayerHitEvent repeated;repeated.me=player;repeated.hitObject=object;EventManager::GetInstance()->Dispatch(repeated);
                object->Update(0.21f);
                Require(object->IsDead() && !object->GetIsVisible(),"repeated contact restarted collection");
            }
            Require(data->GetCollectedStarCoinCount()==sSavedCount,"recollect changed saved star count");
            log<<"PASS stage="<<sStage+1<<" gold/silver pickup, duplicate contact ignored, saved count unchanged"<<std::endl;
            sSilver=sGold=nullptr;sPhase=3;sFrames=0;return;
        }
        if(sPhase==3) {
            if(++sFrames<2)return;
            gateEntryRuntimeProbe::Capture(path+".stage"+std::to_string(sStage+1)+".pickup.png");
            CameraManager::GetInstance()->SetActiveCamera(sPreviousCamera);sCamera.reset();sPreviousCamera=nullptr;
            if(++sStage<2){sPhase=0;return;}
            finish("PASS: silver star models, idle/pickup effects, preserved collision, duplicate pickup and save count in Stage1/2");
        }
    }catch(const std::exception& error){gateEntryRuntimeProbe::Capture(path+".failed.png");finish(std::string("FAIL: ")+error.what());}
}
} // namespace starCoinRuntimeProbe
#endif
