#pragma once
#include <cstdint>
#ifdef USE_IMGUI
#include "game/scene/gameplay/GamePlayScene.h"
#include "game/system/progress/StageManager.h"
#include "game/system/progress/GameDataManager.h"
#include "game/system/scene/SceneGameplayAccess.h"
#include "SceneManager.h"
#include "CameraManager.h"
#include "PostEffect.h"
#include "WinApp.h"
#include "Player.h"
#include "game/crown/CrownFinaleTuning.h"
#include <fstream>
#include <cmath>

// 明示した検証起動だけで、実Stage3の複製を生成しGPU描画・巻戻し・片付けを通す。
namespace cinematicRuntimeProbe {
inline bool Enabled() { return GetEnvironmentVariableA("CG2_CINEMATIC_TEST",nullptr,0)>0; }
inline void AfterDraw(SceneManager* manager,const std::string& path) {
    static std::ofstream log(path,std::ios::trunc);
    static ULONGLONG sStarted=GetTickCount64();
    static std::int32_t sPhase=0,sFrames=0;
    static size_t sObjectCount=0;
    static Player* sOriginalPlayer=nullptr;
    static engine::Camera* sOriginalCamera=nullptr;
    static engine::Vector3 sInitialEye{},sInitialPlayer{};
    static float sBars=0;
    static std::int32_t sCrowns=0;
    static bool sDone=false;
    static bool sDefeat=false;
    if (sDone || !manager) return;
    auto finish=[&](const char* message) { log<<message<<std::endl;sDone=true;WinApp::CloseNow(); };
    if (GetTickCount64()-sStarted>240000) { finish("FAIL: cinematic timeout");return; }
    if (manager->IsTransitioning()) return;
    if (sPhase==0) {
        StageManager::GetInstance()->SetCurrentStageById("stage3");manager->ChangeScene("GAMEPLAY");sPhase=1;return;
    }
    auto* game=dynamic_cast<GamePlayScene*>(manager->GetCurrentScene());
    if (!game) return;
    if (sPhase==1) {
        if (++sFrames<5) return;
        sObjectCount=game->GetObjects().size(); sOriginalPlayer=GetScenePlayer(game);
        sOriginalCamera=CameraManager::GetInstance()->GetActiveCameraOverride();
        sBars=PostEffect::GetInstance()->GetParams()->cinemaBarHeight;
        sCrowns=GameDataManager::GetInstance()->GetClearedStageCount();
        // 準備後の再生・プレビュー開始で、部品の対象解決が繰り返されないことを確かめる。
        engine::CinematicSequence fixture;
        fixture.duration=0.25f;
        engine::CinematicObjectClip part;
        part.name="__CinematicProbePart"; part.source="$Actor"; part.keys.push_back({});
        part.followPosition=part.followRotation=part.followScale=part.inheritVisibility=true;
        part.rotationMask={0,1,0};
        fixture.presentation.objects.push_back(part);
        engine::CinematicPlayer probe;
        std::int32_t resolutions=0;
        auto resolveActor=[&](const auto& binding) -> engine::Object3D* {
            if (binding.targetName!="$Actor") return nullptr;
            ++resolutions;
            return sOriginalPlayer;
        };
        probe.Initialize(manager);
        probe.SetBindingResolver(resolveActor);
        probe.SetSequence(&fixture);
        const std::int32_t prepared=resolutions;
        probe.Play(false);
        if (!probe.IsPlaying() || resolutions!=prepared) { finish("FAIL: Play rebuilt prepared objects");return; }
        // 退避値のまま止まらず、現在の位置・拡縮・表示を借り、指定軸だけを継承する。
        const auto savedTransform=static_cast<const engine::Object3D*>(sOriginalPlayer)->GetTransform();
        const bool savedVisible=sOriginalPlayer->GetIsVisible();
        sOriginalPlayer->SetTranslate(savedTransform.translate+engine::Vector3{2,3,4});
        sOriginalPlayer->SetRotation({.7f,.4f,.3f});
        sOriginalPlayer->SetScale(savedTransform.scale*2);
        sOriginalPlayer->SetIsVisible(false);
        probe.SetTime(.1f,false);
        auto* followed=probe.ResolveBinding({part.name,-1});
        const bool followedCorrectly=followed &&
            engine::Math::Length(followed->GetTranslate()-sOriginalPlayer->GetTranslate())<.0001f &&
            engine::Math::Length(followed->GetScale()-sOriginalPlayer->GetScale())<.0001f &&
            engine::Math::Length(followed->GetRotation()-engine::Vector3{0,.4f,0})<.0001f && !followed->GetIsVisible();
        sOriginalPlayer->SetTranslate(savedTransform.translate);
        sOriginalPlayer->SetRotation(savedTransform.rotate);
        sOriginalPlayer->SetScale(savedTransform.scale);
        sOriginalPlayer->SetIsVisible(savedVisible);
        sOriginalPlayer->UpdateWorldMatrix(true);
        if (!followedCorrectly) { finish("FAIL: live attachment used stale pose or incorrect rotation axes");return; }
        probe.Pause(); probe.Resume(false); probe.Update(0.3f);
        if (!probe.IsFinished()) { finish("FAIL: shared playback did not finish");return; }
        probe.Stop(true);
        probe.SetSequence(&fixture);
        const std::int32_t previewPrepared=resolutions;
        probe.BeginPreview();
        if (!probe.IsPreviewing() || resolutions!=previewPrepared) { finish("FAIL: BeginPreview rebuilt prepared objects");return; }
        probe.EndPreview(true);
        probe.SetSequence(&fixture);
        const std::int32_t beforeResolverChange=resolutions;
        probe.SetBindingResolver(resolveActor);
        probe.Play(false);
        if (!probe.IsPlaying() || resolutions==beforeResolverChange) { finish("FAIL: changed resolver kept stale bindings");return; }
        probe.Stop(true);
        fixture.presentation.reference=fixture.presentation.lookToward="$Actor";
        fixture.presentation.view.push_back({}); fixture.cameraShots.push_back({});
        probe.SetSequence(&fixture);
        const auto cameraIssues=probe.ValidateBindings();
        if (std::none_of(cameraIssues.begin(),cameraIssues.end(),[](const auto& issue) {
            return issue.find("視点キーとCamera Shot")!=std::string::npos;
        })) { finish("FAIL: mixed camera controllers were accepted");return; }
        probe.Stop(true);
        log<<"PREPARATION reused by Play and BeginPreview; pause/resume/completion passed"<<std::endl;
        if (!game->BeginCrownMoviePreview(sDefeat)) { finish("FAIL: could not begin crown preview");return; }
        const float duration=sDefeat?game->GetCrownDefeatDuration():game->GetCrownFinaleTiming().movieDuration;
        if (duration<9) { finish("FAIL: wrong cinematic duration");return; }
        sInitialEye=CameraManager::GetInstance()->GetActiveCamera()->GetEye();
        sInitialPlayer=GetScenePlayer(game)->GetTranslate();
        log<<(sDefeat?"DEFEAT":"INHERITANCE")<<" BEGIN duration="<<duration<<" objects="<<game->GetObjects().size()<<std::endl;
        sPhase=2;sFrames=0;return;
    }
    if (sPhase==2) {
        game->StepCrownMoviePreview(1.0f/60.0f);
        if (++sFrames<(sDefeat?300:900)) return;
        auto delta=CameraManager::GetInstance()->GetActiveCamera()->GetEye()-sInitialEye;
        if (delta.x*delta.x+delta.y*delta.y+delta.z*delta.z<0.01f) { finish("FAIL: cinematic camera did not move");return; }
        game->RewindCrownMoviePreview();
        delta=CameraManager::GetInstance()->GetActiveCamera()->GetEye()-sInitialEye;
        const auto playerDelta=GetScenePlayer(game)->GetTranslate()-sInitialPlayer;
        if (delta.x*delta.x+delta.y*delta.y+delta.z*delta.z>0.001f ||
            playerDelta.x*playerDelta.x+playerDelta.y*playerDelta.y+playerDelta.z*playerDelta.z>0.001f) {
            finish("FAIL: rewind did not restore initial pose/camera");return;
        }
        log<<"REWIND after "<<sFrames<<" drawn frames"<<std::endl;sPhase=3;sFrames=0;return;
    }
    if (sPhase==3) {
        // 一巡目の巻戻しは60fps、通し再生は30fpsの刻みでも節目を落とさないことを確かめる。
        game->StepCrownMoviePreview(1.0f/30.0f);
        const float duration=sDefeat?game->GetCrownDefeatDuration():game->GetCrownFinaleTiming().movieDuration;
        if (++sFrames<static_cast<std::int32_t>(duration*30)+3) return;
        if (!sDefeat && GetScenePlayer(game)->GetEnemyMorphType()!=Player::EnemyMorphType::kCrown) {
            finish("FAIL: inheritance did not grant Crown at the formation marker");return;
        }
        if (GameDataManager::GetInstance()->GetClearedStageCount()!=sCrowns) { finish("FAIL: preview changed game progress");return; }
        game->EndCrownMoviePreview();
        if (game->GetObjects().size()!=sObjectCount || GetScenePlayer(game)!=sOriginalPlayer ||
            CameraManager::GetInstance()->GetActiveCameraOverride()!=sOriginalCamera ||
            std::abs(PostEffect::GetInstance()->GetParams()->cinemaBarHeight-sBars)>0.0001f) {
            finish("FAIL: preview cleanup did not restore scene/player/camera/bars");return;
        }
        log<<"END restored objects="<<sObjectCount<<std::endl;sPhase=4;return;
    }
    if (!sDefeat) { sDefeat=true;sPhase=1;sFrames=0;return; }
    finish("PASS: inheritance and defeat rendered, rewound, held final poses and restored editor state without progress changes");
}
}
#endif
