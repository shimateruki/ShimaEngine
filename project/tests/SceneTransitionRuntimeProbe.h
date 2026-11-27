#pragma once
#include <cstdint>

#ifdef USE_IMGUI
#include "SceneManager.h"
#include "SharedRenderCommonsRuntimeProbe.h"
#include <span>
#include "StatusPreviewRuntimeProbe.h"
#include "PreviewCompositionRuntimeProbe.h"
#include "AttackPreviewRegistryRuntimeProbe.h"
#include "EditorEventRuntimeProbe.h"
#include "MeshEffectManager.h"
#include "WinApp.h"
#include "SdlStickRuntimeProbe.h"
#include "TutorialPromptRuntimeProbe.h"
#include <Windows.h>
#include <fstream>
#include <string>
#include "AttackLifecycleRuntimeProbe.h"
#include "AttackEditorRuntimeProbe.h"
#include "RaftRuntimeProbe.h"
#include "CinematicRuntimeProbe.h"
#include "GimmickPropertyRuntimeProbe.h"
#include "GateEntryRuntimeProbe.h"
#include "TutorialExitGuideRuntimeProbe.h"
#include "PlayerAttackRuntimeProbe.h"
#include "PlayerLocomotionRuntimeProbe.h"
#include "CannonDamageRuntimeProbe.h"
#include "GoalPresentationRuntimeProbe.h"
#include "StarCoinRuntimeProbe.h"
#include "EnemyProjectileRuntimeProbe.h"
#include "CarryHookRuntimeProbe.h"
#include "CameraBarrierRuntimeProbe.h"

// 明示的に環境変数を設定した実行だけで、実際の更新・GPU描画を通して遷移を検証します。
// 通常起動ではシーン選択、エフェクト、保存データに介入しません。
namespace sceneTransitionRuntimeProbe {
inline const std::string& LogPath() {
    static const std::string kPath = [] {
        char value[2048]{};
        const auto length = GetEnvironmentVariableA("CG2_SCENE_TRANSITION_TEST_LOG", value, sizeof(value));
        return length && length < sizeof(value) ? std::string(value) : std::string{};
    }();
    return kPath;
}
inline bool Enabled() { return !LogPath().empty(); }
inline void AfterDraw(SceneManager* manager, DebugEditor* editor = nullptr) {
    if (!Enabled() || !manager)
        return;
    if (!sharedRenderCommonsRuntimeProbe::CheckAfterDraw(manager, LogPath())) return;
    if (StatusPreviewRuntimeProbe::Enabled()) {
        StatusPreviewRuntimeProbe::AfterDraw(manager, editor, LogPath());
        return;
    }
    if (PreviewCompositionRuntimeProbe::Enabled()) {
        PreviewCompositionRuntimeProbe::AfterDraw(manager, editor, LogPath());
        return;
    }
    if (AttackPreviewRegistryRuntimeProbe::Enabled()) {
        AttackPreviewRegistryRuntimeProbe::AfterDraw(manager, editor, LogPath());
        return;
    }
    if (editorEventRuntimeProbe::Enabled()) {
        editorEventRuntimeProbe::AfterDraw(manager, editor, LogPath());
        return;
    }
    if (cameraBarrierRuntimeProbe::Enabled()) {
        cameraBarrierRuntimeProbe::AfterDraw(manager, LogPath());
        return;
    }
    if (carryHookRuntimeProbe::Enabled()) {
        carryHookRuntimeProbe::AfterDraw(manager, LogPath());
        return;
    }
    if (enemyProjectileRuntimeProbe::Enabled()) {
        enemyProjectileRuntimeProbe::AfterDraw(manager,LogPath());
        return;
    }
    if (starCoinRuntimeProbe::Enabled()) {
        starCoinRuntimeProbe::AfterDraw(manager,LogPath());
        return;
    }
    if (goalPresentationRuntimeProbe::Enabled()) {
        goalPresentationRuntimeProbe::AfterDraw(manager,LogPath());
        return;
    }
    if (sdlStickRuntimeProbe::Enabled()) {
        sdlStickRuntimeProbe::AfterDraw(LogPath());
        return;
    }
    if (gateEntryRuntimeProbe::Enabled()) {
        gateEntryRuntimeProbe::AfterDraw(manager, LogPath());
        return;
    }
    if (cannonDamageRuntimeProbe::Enabled()) {
        cannonDamageRuntimeProbe::AfterDraw(manager, LogPath());
        return;
    }
    if (tutorialExitGuideRuntimeProbe::Enabled()) {
        tutorialExitGuideRuntimeProbe::AfterDraw(manager, LogPath());
        return;
    }
    if (tutorialPromptRuntimeProbe::Enabled()) {
        tutorialPromptRuntimeProbe::AfterDraw(manager, LogPath());
        return;
    }
    if (playerLocomotionRuntimeProbe::Enabled()) {
        playerLocomotionRuntimeProbe::AfterDraw(manager, LogPath());
        return;
    }
    if (playerAttackRuntimeProbe::Enabled()) {
        playerAttackRuntimeProbe::AfterDraw(manager, LogPath());
        return;
    }
    if (gimmickPropertyRuntimeProbe::Enabled()) {
        gimmickPropertyRuntimeProbe::AfterDraw(manager, editor, LogPath());
        return;
    }
    if (cinematicRuntimeProbe::Enabled()) {
        cinematicRuntimeProbe::AfterDraw(manager, LogPath());
        return;
    }
    if (raftRuntimeProbe::Enabled()) {
        raftRuntimeProbe::AfterDraw(manager, LogPath());
        return;
    }
    if (AttackEditorRuntimeProbe::Enabled()) {
        AttackEditorRuntimeProbe::AfterDraw(manager, editor, LogPath());
        return;
    }
    if (attackLifecycleRuntimeProbe::Enabled()) {
        attackLifecycleRuntimeProbe::AfterDraw(manager, LogPath());
        return;
    }
    static std::ofstream log(LogPath(), std::ios::trunc);
    static ULONGLONG sPhaseStarted = GetTickCount64();
    static std::int32_t sStep = 0;
    static std::int32_t sFrames = 0;
    static bool sFinished = false;
    static uint64_t sGeneration = 0;
    if (sFinished)
        return;
    auto finish = [&](const char* message) {
        log << message << std::endl;
        sFinished = true;

        // 検証専用プロセスなので保存確認UIを挟まず、通常のFinalizeへ進めます。
        WinApp::CloseNow();
    };

    // 複数回の実アセット読込を合算せず、各遷移が進まなくなった場合を検出します。
    if (GetTickCount64() - sPhaseStarted > 180000) {
        finish("FAIL: transition timeout");
        return;
    }
    if (manager->IsTransitioning())
        return;
    const char* normalScenes[] = {"TITLE", "GAMEPLAY", "SELECT", "GAMEPLAY", "GAMEPLAY"};
    const char* commonScenes[] = {"TITLE", "GAMEPLAY", "SELECT", "GAMEOVER", "TUTORIAL", "SCENE_EDITOR", "GAMEPLAY", "GAMEPLAY"};
    const std::span<const char* const> scenes = sharedRenderCommonsRuntimeProbe::Enabled()
        ? std::span<const char* const>(commonScenes) : std::span<const char* const>(normalScenes);
    if (manager->GetCurrentSceneName() != scenes[sStep]) {
        finish("FAIL: unexpected scene");
        return;
    }
    if (sFrames == 0) {
        if (sGeneration == manager->GetSceneGeneration())
            return;
        sGeneration = manager->GetSceneGeneration();
        log << "ENTER " << sStep << ' ' << scenes[sStep] << " generation=" << sGeneration << std::endl;
        for (const auto& effect : MeshEffectManager::GetInstance()->GetActiveEffects()) {
            if (effect && effect->GetName() == "SceneTransitionLifetimeProbe") {
                finish("FAIL: previous scene effect survived its owner");
                return;
            }
        }

        // 遷移時にも生存する長寿命エフェクトで、描画基盤への古い参照を検出します。
        MeshEffectManager::GetInstance()->SpawnPortalEffect({0, 1, 0}, 120.0f);
        if (MeshEffectManager::GetInstance()->GetActiveEffects().empty()) {
            finish("FAIL: mesh effect was not created");
            return;
        }
        MeshEffectManager::GetInstance()->GetActiveEffects().back()->SetName("SceneTransitionLifetimeProbe");
    }
    if (++sFrames < 120)
        return;
    log << "DRAWN " << scenes[sStep] << " frames=" << sFrames
        << " activeMesh=" << MeshEffectManager::GetInstance()->GetActiveEffects().size() << std::endl;
    if (++sStep == static_cast<std::int32_t>(scenes.size())) {
        finish("PASS: loading transitions and direct reload rendered successfully");
        return;
    }
    sFrames = 0;
    sPhaseStarted = GetTickCount64();
    if (sStep == static_cast<std::int32_t>(scenes.size()) - 1)
        manager->ChangeSceneWithoutLoading(scenes[sStep]);
    else manager->ChangeScene(scenes[sStep]);
    log << "REQUEST " << scenes[sStep] << std::endl;
}
}
#endif
