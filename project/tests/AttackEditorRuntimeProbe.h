#pragma once
#include <cstdint>
#ifdef USE_IMGUI
#include "game/editor/RegisterGameActorDebug.h"
#include "engine/editor/preview/EffectPreviewStage.h"
#include "engine/editor/core/EditorManager.h"
#include "engine/editor/core/DebugEditor.h"
#include "SceneManager.h"
#include "WinApp.h"
#include <fstream>

// Updateを直接呼ばず、通常のGame View→DebugEditor経路で進むことを検証します。
struct AttackEditorRuntimeProbe {
    static bool Enabled() {
        static const bool kEnabled = GetEnvironmentVariableA("CG2_ATTACK_EDITOR_TEST", nullptr, 0) > 0;
        return kEnabled;
    }

    // 通常描画後に段階を進める。専用起動の確認後にだけ、シーン切替とEditor操作を実行する。
    static void AfterDraw(SceneManager* manager, DebugEditor* editor, const std::string& path) {
        if (!Enabled() || !manager || !editor)
            return;
        static std::ofstream log(path, std::ios::trunc);
        static std::int32_t sPhase = 0, sFrames = 0;
        static float sStart = 0;
        static bool sDone = false;
        static ULONGLONG sBegan = GetTickCount64();
        if (sDone)
            return;
        auto finish = [&](const std::string& result) { log << result << std::endl; sDone = true; WinApp::CloseNow(); };
        if (GetTickCount64() - sBegan > 300000) {
            finish("FAIL: editor timeout");
            return;
        }
        if (manager->IsTransitioning())
            return;
        if (sPhase == 0) {
            manager->ChangeScene("GAMEPLAY");
            sPhase = 1;
            return;
        }
        if (manager->GetCurrentSceneName() != "GAMEPLAY")
            return;
        auto& preview = *GetGameActorDebugPanel();
        if (sPhase == 1) {
            // Scene切替直後の選択リセットが終わってから、ユーザーが開くのと同じ順序にします。
            if (++sFrames < 3)
                return;
            preview.enemyType_ = "FireSlime";
            preview.LoadSelectedProfile();
            for (std::int32_t i = 0; i < static_cast<std::int32_t>(preview.editableProfile_.attacks.size()); ++i)
                if (preview.editableProfile_.attacks[i].id == "fireball")
                    preview.profileAttackIndex_ = i;
            preview.ApplyRecommendedSettings();
            preview.loop_ = false;
            EditorManager::GetInstance()->SetSelectedObject(&preview);
            preview.PreviewDraft(false);
            sStart = preview.elapsedTime_;
            sFrames = 0;
            sPhase = 2;
            log << "START draft playback time=" << sStart << std::endl;
            return;
        }
        if (++sFrames < 30)
            return;
        sFrames = 0;
        if (sPhase == 2 || sPhase == 5 || sPhase == 7) {
            log << "PLAY phase=" << sPhase << " elapsed=" << preview.elapsedTime_ << std::endl;
            if (!preview.IsAuthoringViewportActive()) {
                finish("FAIL: attack editor selection lost");
                return;
            }
            if (preview.elapsedTime_ < sStart + 0.3f) {
                finish("FAIL: authoring playback clock did not advance");
                return;
            }
            std::int32_t gridObjects = 0;
            for (const auto& object : manager->GetCurrentScene()->GetObjects())
                if (object && object->GetIsVisible() && object->GetName().find("__Editor_EffectPreviewGrid") == 0)
                    ++gridObjects;
            log << "GRID visible=" << gridObjects << std::endl;
            if (!EffectPreviewStage::GetInstance()->IsEnabled() || gridObjects < 20) {
                finish("FAIL: isolated preview grid missing");
                return;
            }
            if (sPhase == 7) {
                finish("PASS: editor auto-play, pause, frame-step, resume, placement-to-play and visible grid");
                return;
            }
            if (sPhase == 5) {
                preview.selectedTrack_ = -2;
                preview.BeginPlacementPreview();
                sStart = preview.elapsedTime_;
                sPhase = 6;
                return;
            }
            preview.playing_ = false;
            sStart = preview.elapsedTime_;
            sPhase = 3;
            return;
        }
        if (sPhase == 3) {
            if (std::abs(preview.elapsedTime_ - sStart) > 0.001f) {
                finish("FAIL: pause advanced time");
                return;
            }
            preview.SeekPreview(sStart + 1.0f / 60.0f);
            if (std::abs(preview.elapsedTime_ - sStart - 1.0f / 60.0f) > 0.001f) {
                finish("FAIL: frame-step mismatch");
                return;
            }
            sStart = preview.elapsedTime_;
            sPhase = 4;
            log << "STEP elapsed=" << sStart << std::endl;
            return;
        }
        if (sPhase == 4) {
            if (std::abs(preview.elapsedTime_ - sStart) > 0.001f) {
                finish("FAIL: frame-step resumed playback");
                return;
            }
            preview.playing_ = true;
            sPhase = 5;
            return;
        }
        if (sPhase == 6) {
            if (!preview.placementPreview_ || std::abs(preview.elapsedTime_ - sStart) > 0.001f) {
                finish("FAIL: placement preview did not remain paused");
                return;
            }
            preview.playing_ = true;
            sStart = 0;
            sPhase = 7;
        }
    }
};
#endif
