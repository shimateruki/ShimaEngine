#pragma once
#include <cstdint>
#ifdef USE_IMGUI
#include "SceneManager.h"
#include "game/system/scene/SceneGameplayAccess.h"
#include "game/system/progress/StageManager.h"
#include "Player.h"
#include "CollisionManager.h"
#include "WinApp.h"
#include <cmath>
#include <fstream>

namespace flipPairRuntimeProbe {
inline bool Enabled() {
    // 専用の検証起動だけでプレイヤーを移動し、通常プレイには介入しない。
    return GetEnvironmentVariableA("CG2_FLIP_PAIR_TEST", nullptr, 0) > 0;
}

inline void AfterDraw(SceneManager* manager, const std::string& path) {
    // 実シーンの３組で高速落下と青→赤→青→赤の着地を行い、通常の衝突更新で確認する。
    static std::ofstream log(path, std::ios::trunc);
    static std::int32_t sPhase = 0, sFrames = 0, sPair = 0, sJump = 0, sStableFrames = 0;
    static ULONGLONG sBegan = GetTickCount64();
    static engine::Vector3 sDeckTop{};
    if (sPhase == 4 || !manager)
        return;
    const auto finish = [&](const char* result) {
        log << result << std::endl;
        sPhase = 4;
        WinApp::CloseNow();
    };
    if (GetTickCount64() - sBegan > 180000) {
        finish("FAIL: flip-pair test timeout");
        return;
    }
    if (manager->IsTransitioning())
        return;
    if (sPhase == 0) {
        StageManager::GetInstance()->SetCurrentStageById("stage1");
        manager->ChangeScene("GAMEPLAY");
        sPhase = 1;
        return;
    }
    auto* scene = manager->GetCurrentScene();
    auto* player = GetScenePlayer(scene);
    if (!player || manager->GetCurrentSceneName() != "GAMEPLAY")
        return;
    if (sPhase == 1) {
        if (++sFrames < 180 || player->IsCinematicLocked() || !player->IsControlActive())
            return;
        sPhase = 2;
    }
    engine::Object3D* floor = nullptr;
    const std::string name = "Stage1_RingCourt_EntryFlipPair_" + std::to_string(sPair);
    for (const auto& object : scene->GetObjects()) {
        if (object && object->GetName() == name)
            floor = object.get();
    }
    if (!floor) {
        finish("FAIL: flip pair missing from stage1");
        return;
    }
    if (sPhase == 2) {
        player->DebugPrepareForTeleport();
        player->ResetJumpCount();
        PlayerJumpEvent reset;
        reset.player = player;
        EventManager::GetInstance()->Dispatch(reset);
        sJump = 0;
        sStableFrames = sFrames = 0;
        // Assimpの左手座標変換後、青板はローカル+Zにある。判定設定とは独立した期待位置。
        sDeckTop = engine::Math::Transform({0.0f, 0.27f, 5.0f}, floor->GetTransform()->matWorld);
        const float feetOffset = player->GetAabb().min.y - player->GetWorldPosition().y;
        player->SetTranslate({sDeckTop.x, sDeckTop.y - feetOffset + 2.0f, sDeckTop.z});
        player->SetGrounded(false);
        player->SetVelocity({0.0f, -60.0f, 0.0f});
        player->UpdateWorldMatrix(false);
        sPhase = 3;
        log << "DROP " << name << " top=" << sDeckTop.x << ',' << sDeckTop.y << ',' << sDeckTop.z << std::endl;
        return;
    }
    player->SetIsControlActive(false);
    player->SetDamageInvincible(true);
    const float feetY = player->GetAabb().min.y;
    if (player->IsDead() || feetY < sDeckTop.y - 0.3f || ++sFrames > 240) {
        const auto& collider = floor->GetColliderConfig();
        log << "CONTACT pair=" << sPair << " jump=" << sJump << " feet=" << feetY
            << " expectedTop=" << sDeckTop.y << " centerZ=" << collider.center.z
            << " attr=" << floor->GetCollisionAttribute() << " model=" << floor->GetModelName() << std::endl;
        finish("FAIL: player passed through visible board or did not land");
        return;
    }
    sStableFrames = player->IsGrounded() && std::abs(feetY - sDeckTop.y) < 0.08f ? sStableFrames + 1 : 0;
    if (sStableFrames < 20)
        return;

    // 接地高さだけで成功にせず、描画位置にあるこの床自身が衝突検索へ登録されているか確かめる。
    engine::PhysicsQueryFilter filter;
    filter.mask = engine::collisionCategories::Get("ground");
    filter.ignoredObject = player;
    bool foundFloor = false;
    for (const auto& hit : engine::CollisionManager::GetInstance()->OverlapSphere(
             sDeckTop + engine::Vector3{0.0f, 0.2f, 0.0f}, 0.25f, filter)) {
        foundFloor |= hit.object == floor && hit.collision.normal.y > 0.9f;
    }
    if (!foundFloor) {
        finish("FAIL: visible board missing from collision query");
        return;
    }

    // 板のない反対側へ箱を広げて問題を隠していないことも、実Colliderで検証する。
    Transform emptyTransform;
    emptyTransform.translate = engine::Math::Transform(
        {0.0f, 0.47f, sJump % 2 == 0 ? -5.0f : 5.0f}, floor->GetTransform()->matWorld);
    emptyTransform.UpdateMatrix();
    engine::Collider emptyProbe(&emptyTransform);
    engine::ColliderConfig probeConfig;
    probeConfig.type = engine::ColliderType::kSphere;
    probeConfig.size = {0.25f, 0.25f, 0.25f};
    emptyProbe.SetConfig(probeConfig);
    if (emptyProbe.CheckCollision(floor->GetCollider()).isColliding) {
        finish("FAIL: empty board side has collision");
        return;
    }
    log << "LANDED pair=" << sPair << " side=" << (sJump % 2 == 0 ? "blue" : "red")
        << " jump=" << sJump << " feet=" << feetY << " expectedTop=" << sDeckTop.y
        << " localColliderZ=" << floor->GetColliderConfig().center.z << std::endl;
    if (++sJump == 4) {
        if (++sPair == 3) {
            finish("PASS: all 3 flip pairs, 12 landings and empty sides verified in live gameplay");
        } else {
            sPhase = 2;
        }
        return;
    }

    // 実際のジャンプ通知で全床を切り替え、垂直移動・重力・着地は通常のPlayer更新へ任せる。
    const auto position = player->GetTranslate();
    player->IncrementJumpCount();
    PlayerJumpEvent event;
    event.player = player;
    EventManager::GetInstance()->Dispatch(event);
    sDeckTop = engine::Math::Transform({0.0f, 0.27f, sJump % 2 == 0 ? 5.0f : -5.0f},
                                     floor->GetTransform()->matWorld);
    player->SetTranslate({sDeckTop.x, position.y, sDeckTop.z});
    player->SetGrounded(false);
    player->SetVelocity({0.0f, player->GetJumpPower(), 0.0f});
    player->UpdateWorldMatrix(false);
    sFrames = sStableFrames = 0;
}
} // namespace flipPairRuntimeProbe
#endif
