#pragma once
#include <cstdint>
#ifdef USE_IMGUI
#include "SceneManager.h"
#include "game/system/scene/SceneGameplayAccess.h"
#include "game/system/progress/StageManager.h"
#include "Player.h"
#include "BaseEnemy.h"
#include "WinApp.h"
#include <fstream>
#include "RollerRuntimeProbe.h"
#include "FlipPairRuntimeProbe.h"

namespace raftRuntimeProbe {
inline ULONGLONG pauseUntil = 0;
inline bool Enabled() {
    // 明示した検証起動だけで配置を変え、通常プレイには介入しない。
    static const bool kEnabled = GetEnvironmentVariableA("CG2_RAFT_TEST", nullptr, 0) > 0;
    return kEnabled || flipPairRuntimeProbe::Enabled();
}
inline bool WantsPlay(SceneManager* manager) {
    // エディターの停止設定から独立して、実際の接地・死亡・描画経路を検証する。
    return Enabled() && manager && !manager->IsTransitioning() && manager->GetCurrentSceneName() == "GAMEPLAY";
}
inline void AfterDraw(SceneManager* manager, const std::string& path) {
    if (flipPairRuntimeProbe::Enabled()) {
        flipPairRuntimeProbe::AfterDraw(manager, path);
        return;
    }
    // 同じ検証用起動経路で円柱も確認し、通常起動の更新処理を増やさない。
    if (GetEnvironmentVariableA("CG2_ROLLER_TEST", nullptr, 0)>0) {
        rollerRuntimeProbe::AfterDraw(manager, path);
        return;
    }
    // プレイヤーを最初だけ床へ置き、以後は通常のゲーム更新に任せる。
    static std::ofstream log(path, std::ios::trunc);
    static std::int32_t sPhase = 0, sFrames = 0;
    static ULONGLONG sBegan = GetTickCount64();
    static bool sSawSea = false, sSawPeak = false, sDone = false;
    if (sDone || !manager || manager->IsTransitioning()) return;
    auto finish = [&](const char* message) {
        log << message << std::endl;
        sDone = true;
        WinApp::CloseNow();
    };
    if (GetTickCount64() - sBegan > 180000) { finish("FAIL timeout"); return; }
    if (sPhase == 0) {
        StageManager::GetInstance()->SetCurrentStageById("stage1");
        manager->ChangeScene("GAMEPLAY");
        sPhase = 1;
        return;
    }
    auto* scene = manager->GetCurrentScene();
    auto* player = GetScenePlayer(scene);
    if (!player || manager->GetCurrentSceneName() != "GAMEPLAY") return;
    engine::Object3D* deck = nullptr;
    for (auto& object : scene->GetObjects())
        if (object && object->GetName() == "Stage1_Coaster_Deck") deck = object.get();
    if (!deck) { finish("FAIL missing deck"); return; }
    if (sPhase == 1) {
        if (++sFrames < 180 || player->IsCinematicLocked()) return;
        const auto top = deck->GetAabb().max.y;
        const float feetOffset = player->GetAabb().min.y - player->GetTranslate().y;
        auto position = deck->GetTranslate();
        position.y = top - feetOffset + .05f;
        player->SetTranslate(position);
        player->SetVelocity({});
        player->UpdateWorldMatrix(true);
        sPhase = 2;
        sFrames = 0;
        log << "START raft" << std::endl;
        return;
    }
    const auto d = deck->GetTranslate();
    const auto p = player->GetTranslate();
    // 輸送の接地検証では戦闘AIだけ休止する。落下乗り込みと地形判定は実処理を通す。
    for (auto& object : scene->GetObjects())
        if (object && object->GetName().find("Stage1_Coaster_Boarder_") == 0)
            if (auto* enemy = dynamic_cast<BaseEnemy*>(object.get())) enemy->SetDormant(true);
    player->SetDamageInvincible(true);
    if (++sFrames % 30 == 0)
        log << sFrames << " raft=" << d.x << ',' << d.y << ',' << d.z
            << " player=" << p.x << ',' << p.y << ',' << p.z
            << " grounded=" << player->IsGrounded() << " dead=" << player->IsDead() << std::endl;
    if (player->IsDead() || p.y < -15) { finish("FAIL rider lost"); return; }
    sSawSea |= d.y < -10;
    if (!sSawPeak && d.y > 21.9f) {
        sSawPeak = true;
        pauseUntil = GetTickCount64() + 15000;
        log << "PAUSE peak for visual inspection" << std::endl;
    }
    if (sSawSea && sSawPeak && d.y < 16.4f && d.x > 760 && d.z < 13.1f)
        finish("PASS sea travel, ascent and landing with live rider");
}
}
#endif
