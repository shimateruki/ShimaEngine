#pragma once
#include <cstdint>
#ifdef USE_IMGUI
#include "game/actor/gimmick/movement/GimmickDashPanel.h"
#include "game/actor/player/Player.h"
#include "game/system/rules/EventManager.h"
#include "game/actor/player/movement/PlayerMover.h"
#include "game/component/IMoveStrategy.h"
#include "game/system/rules/GameplayStatusManager.h"
#include "Collider.h"
#include "CollisionManager.h"
#include "game/actor/gimmick/block/GimmickBreakableBlock.h"
#include "SceneManager.h"
#include "WinApp.h"
#include <fstream>
#include <stdexcept>

// 隔離したPlayerと実際のMoverで、ジャンプ・外力・Editor設定の保存まで確認する。
namespace playerLocomotionRuntimeProbe {
inline bool Enabled() { return GetEnvironmentVariableA("CG2_PLAYER_LOCOMOTION_TEST", nullptr, 0) > 0; }
/// <summary>移動検証で入力待ちのジャンプ要求をPlayerMoverへ直接投入する。</summary>
struct Access {
    static void QueueJump(PlayerMover& mover) { mover.jumpRequest_.Advance(1.0f / 60, true, true, true, false); }
};
class MoveInput final : public IMoveStrategy {
  public:
    void SetValue(const engine::Vector3& value) { value_ = value; }
    engine::Vector3 CalculateVelocity(Player*) override { return value_; }
  private:
    engine::Vector3 value_{};
};
inline void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 隔離したパネルとPlayerで、実際の上面接触と共通被ダメージを再現する。
inline void CheckDashPanelRelease(BaseScene* scene, std::ostream& log) {
    Player actor;
    actor.Initialize(scene->GetObject3DCommon(), engine::InputManager::GetInstance(), scene->GetParticleSystem(), scene->GetSpriteCommon());
    actor.SetParameter(&engine::EntityParameter::speed, 10.0f);
    GimmickDashPanel panel;
    panel.Initialize(scene->GetObject3DCommon(), GimmickDashPanel::kModelPath);
    panel.SetScale({1, 1, 1});
    panel.SetTranslate({0, 1000, 0});
    engine::Object3D::ColliderConfig box;
    box.type = engine::ColliderType::kAabb;
    box.size = {2, 0.5f, 2};
    panel.SetColliderConfig(box);
    panel.UpdateWorldMatrix(true);

    actor.SetEditorInternal(false);
    actor.SetCollisionAttribute(engine::collisionCategories::Get("player"));
    actor.SetCollisionMask(engine::collisionCategories::Get("ground"));
    actor.SetScale({1, 1, 1});
    engine::Object3D::ColliderConfig sphere;
    sphere.type = engine::ColliderType::kSphere;
    sphere.size = {0.5f, 0.5f, 0.5f};
    actor.SetColliderConfig(sphere);
    actor.SetCollisionRadius(0.5f);
    actor.GetCollider()->SetScaleOverride({1, 1, 1});
    actor.SetTranslate({0, 1000.6f, 0});
    actor.SetVelocity({10.0f, 0.0f, 0.0f});
    actor.UpdateWorldMatrix(true);
    actor.SetParameter(&engine::EntityParameter::maxHp, 20.0f);
    actor.SetParameter(&engine::EntityParameter::hp, 20.0f);

    // HPが減らない攻撃と無敵中の攻撃は加速を消さない。
    panel.OnCollision(&actor);
    Require(actor.IsDashPanelBoostActive(), "orange panel top did not boost");
    DamageEvent damage;
    damage.target = &actor;
    damage.damageAmount = 0.0f;
    EventManager::GetInstance()->Dispatch(damage);
    Require(actor.GetHp() == 20.0f && actor.IsDashPanelBoostActive(), "zero damage ended boost");
    actor.SetDamageInvincible(true);
    damage.damageAmount = 1.0f;
    EventManager::GetInstance()->Dispatch(damage);
    Require(actor.GetHp() == 20.0f && actor.IsDashPanelBoostActive(), "blocked damage ended boost");

    // 実ダメージ中は押し戻されるが、移動操作が戻ると記録した方向へ走り出す。
    actor.SetDamageInvincible(false);
    damage.knockbackVelocity = {-12.0f, 0.0f, 0.0f};
    EventManager::GetInstance()->Dispatch(damage);
    Require(actor.GetHp() == 19.0f && actor.IsDashPanelBoostActive() && !actor.IsControlActive(),
            "real damage ended boost or failed to interrupt control");
    Require(actor.GetVelocity().x < 0.0f, "panel suppressed knockback");
    for (int i = 0; i < 45; ++i) actor.Update(1.0f / 60);
    Require(actor.IsControlActive() && actor.GetVelocity().x > 20.0f, "boost did not resume after damage");
    log << "OK real damage interrupts motion and boost resumes" << std::endl;

    // 普通の床とガラスを経由しても状態を保つ。壁はガラスと同じ配置・形状で比較する。
    engine::Object3D wall;
    wall.Initialize(scene->GetObject3DCommon());
    wall.SetScale({1, 1, 1});
    wall.SetCollisionAttribute(engine::collisionCategories::Get("ground"));
    wall.SetCollisionMask(engine::collisionCategories::Get("player"));
    box.size = {0.5f, 3.0f, 2.0f};
    wall.SetColliderConfig(box);
    wall.SetTranslate({0.55f, 1000.6f, 0});
    wall.UpdateWorldMatrix(true);
    auto placeActor = [&] {
        actor.SetTranslate({0, 1000.6f, 0});
        actor.SetVelocity({10.0f, 0.0f, 0.0f});
        actor.UpdateWorldMatrix(true);
    };
    placeActor();
    actor.OnCollision(&panel);
    Require(actor.IsDashPanelBoostActive(), "floor contact ended boost");
    GimmickBreakableBlock glass;
    glass.Initialize(scene->GetObject3DCommon(), "Stages/block");
    glass.SetScale({1, 1, 1});
    glass.SetColliderConfig(box);
    glass.SetTranslate(wall.GetTranslate());
    glass.ResetParameters();
    glass.SetParameter(&engine::EntityParameter::actionMode, GimmickBreakableBlock::kDashPanelOnly);
    glass.UpdateWorldMatrix(true);
    placeActor();
    Require(actor.OnCollision(&glass) && glass.IsDead() && actor.IsDashPanelBoostActive(),
            "dash glass blocked or ended boost");
    placeActor();
    Require(actor.OnCollision(&wall) && !actor.IsDashPanelBoostActive() && std::abs(actor.GetVelocity().x) < 0.01f,
            "wall overlap did not stop boost");
    log << "OK floor and breakable glass retain boost, overlapping wall stops it" << std::endl;

    // 掃引が接触手前で止まる設定でも、重なりの通知なしで解除できる。
    placeActor();
    actor.ApplyDashPanelBoost(2.15f, 0.32f);
    actor.SetParameter(&engine::EntityParameter::motorContinuousCollision, true);
    actor.SetParameter(&engine::EntityParameter::motorStepHeight, 0.0f);
    wall.SetTranslate({2.0f, 1000.6f, 0});
    wall.UpdateWorldMatrix(true);
    auto* collisions = engine::CollisionManager::GetInstance();
    GimmickBreakableBlock sweptGlass;
    sweptGlass.Initialize(scene->GetObject3DCommon(), "Stages/block");
    sweptGlass.SetScale({1, 1, 1});
    sweptGlass.SetColliderConfig(box);
    sweptGlass.SetTranslate(wall.GetTranslate());
    sweptGlass.ResetParameters();
    sweptGlass.SetParameter(&engine::EntityParameter::actionMode, GimmickBreakableBlock::kDashPanelOnly);
    sweptGlass.UpdateWorldMatrix(true);
    collisions->AddObject(&sweptGlass);
    actor.Update(0.12f);
    collisions->RemoveObject(&sweptGlass);
    Require(sweptGlass.IsDead() && actor.IsDashPanelBoostActive(), "swept dash glass did not break or ended boost");

    placeActor();
    collisions->AddObject(&wall);
    actor.Update(0.12f);
    collisions->RemoveObject(&wall);
    Require(!actor.IsDashPanelBoostActive() && std::abs(actor.GetVelocity().x) < 0.01f,
            "wall sweep did not stop boost");

    // パネルの上面を踏み直すと再発動し、解除パネルに乗ると通常移動へ戻る。
    panel.Update(1.0f / 60);
    panel.Update(1.0f / 60);
    placeActor();
    panel.OnCollision(&actor);
    Require(actor.IsDashPanelBoostActive(), "orange panel did not rearm after leaving");
    panel.SetGimmickType(GimmickDashPanel::kStopTypeId);
    panel.OnCollision(&actor);
    Require(!actor.IsDashPanelBoostActive(), "blue panel did not end boost");
    log << "OK damage and recovery, floor, dash glass, overlap wall, swept wall and blue panel" << std::endl;
}

inline void Run(BaseScene* scene, std::ostream& log) {
    auto* settings = GameplayStatusManager::GetInstance();
    const auto original = settings->GetPlayerLocomotion();
    settings->GetMutablePlayerLocomotion() = {};
    auto actor = std::make_unique<Player>();
    actor->Initialize(scene->GetObject3DCommon(), engine::InputManager::GetInstance(), scene->GetParticleSystem(), scene->GetSpriteCommon());
    actor->SetEditorInternal(true); actor->SetCollisionAttribute(0); actor->SetCollisionMask(0);
    actor->SetCopyPreviewInput({}); actor->SetCopyPreviewGroundHeight(1000);
    actor->SetParameter(&engine::EntityParameter::speed, 10); actor->SetParameter(&engine::EntityParameter::gravity, 50); actor->SetParameter(&engine::EntityParameter::maxFallSpeed, 60);
    auto mover = std::make_unique<PlayerMover>();
    mover->Initialize(actor.get(), engine::InputManager::GetInstance(), scene->GetParticleSystem());
    auto strategy = std::make_unique<MoveInput>(); auto* input = strategy.get(); mover->SetStrategy(std::move(strategy));
    constexpr float kFixedDeltaTime = 1.0f / 60;
    auto reset = [&] {
        mover->ResetTransientState(); actor->SetTranslate({0, 1000, 0}); actor->SetVelocity({}); actor->SetGrounded(true);
        actor->UpdateWorldMatrix(true); input->SetValue({});
    };
    auto step = [&] { mover->Update(kFixedDeltaTime); actor->SetCopyPreviewInput({}); actor->Update(kFixedDeltaTime); };
    reset(); input->SetValue({10, 0, 0}); step();
    Require(actor->GetVelocity().x > 0 && actor->GetVelocity().x < 1, "runtime startup snapped to full speed");
    for (std::int32_t i = 0; i < 23; ++i) step();
    Require(std::abs(actor->GetVelocity().x - 10) < 0.001f, "runtime maximum was not reached");
    input->SetValue({}); for (std::int32_t i = 0; i < 6; ++i) step();
    Require(std::abs(actor->GetVelocity().x) < 0.001f, "runtime release failed to stop");
    log << "OK actual Player/Mover acceleration and quick braking" << std::endl;
    reset(); Access::QueueJump(*mover); step();
    Require(actor->GetVelocity().y > 0 && std::abs(actor->GetVelocity().x) < 0.001f && std::abs(actor->GetVelocity().z) < 0.001f,
            "standing jump injected full horizontal speed");
    reset(); input->SetValue({10, 0, 0}); step(); step(); Access::QueueJump(*mover); step();
    Require(actor->GetVelocity().y > 0 && actor->GetVelocity().x > 0 && actor->GetVelocity().x < 4,
            "jump bypassed acceleration");
    log << "OK standing and accelerating jumps preserve horizontal momentum" << std::endl;
    reset(); mover->SetPlatformMotion({10, 0, 0}); Access::QueueJump(*mover); step();
    const float carried = actor->GetVelocity().x;
    Require(std::abs(carried - 5.5f) < 0.01f, "moving floor takeoff momentum changed");
    step(); Require(actor->GetVelocity().x > 0 && actor->GetVelocity().x < carried, "moving floor momentum doubled");
    reset(); actor->SetGrounded(false); actor->SetVelocity({8.5f, 18, 0}); mover->SetAbilityAirImpulse({8.5f, 0, 0}); step();
    Require(std::abs(actor->GetVelocity().x - 8.5f) < 0.01f, "blast jump impulse doubled");
    for (std::int32_t i = 0; i < 60; ++i) step();
    Require(std::abs(actor->GetVelocity().x) < 0.05f, "blast momentum became permanent locomotion");
    log << "OK moving floor and blast jump impulses are added once and decay" << std::endl;
    reset(); input->SetValue({10, 0, 0}); step();
    const auto before = actor->GetVelocity(); mover->Update(0);
    Require(actor->GetVelocity().x == before.x, "paused mover changed velocity");
    actor->SetVelocity({}); step();
    Require(actor->GetVelocity().x < 1, "wall stop retained hidden full speed");
    // 隔離プレビューは通常入力を止めるため、踏み込み時だけ操作可能な本編の状態を再現する。
    actor->SetIsControlActive(true);
    mover->ApplyDashPanelBoost(1.5f, 1);
    Require(std::abs(actor->GetVelocity().x - 15) < 0.01f, "dash panel impulse was reduced");
    input->SetValue({});
    for (std::int32_t i = 0; i < 120; ++i) step();
    Require(mover->IsDashPanelBoostActive() && std::abs(actor->GetVelocity().x - 15) < 0.01f,
            "released input stopped dash");
    input->SetValue({-10, 0, 0});
    for (std::int32_t i = 0; i < 120; ++i) step();
    Require(std::abs(actor->GetVelocity().x - 15) < 0.01f, "reverse input stopped or reversed dash");
    input->SetValue({0, 0, 10});
    for (std::int32_t i = 0; i < 120; ++i) step();
    const auto steered = actor->GetVelocity();
    Require(steered.x > 12.5f && steered.z > 6.0f &&
            std::abs(std::hypot(steered.x, steered.z) - 15.0f) < 0.01f, "dash steering lost speed or oversteered");
    Access::QueueJump(*mover);
    step();
    Require(actor->GetVelocity().y > 0.0f && std::abs(std::hypot(actor->GetVelocity().x, actor->GetVelocity().z) - 15.0f) < 0.01f,
            "jump lost forward boost");
    log << "OK persistent forward dash after release, reverse, bounded lateral input and jump" << std::endl;
    auto& movement = settings->GetMutablePlayerLocomotion();
    movement.accelerationTime = 0.41f; movement.brakingTime = 0.12f; movement.airControl = 0.42f;
    settings->MarkDirty(); Require(settings->Save(), "locomotion settings save failed");
    movement = {}; Require(settings->Reload(), "locomotion settings reload failed");
    const auto reopened = settings->GetPlayerLocomotion();
    Require(std::abs(reopened.accelerationTime - 0.41f) < 0.001f && std::abs(reopened.brakingTime - 0.12f) < 0.001f &&
            std::abs(reopened.airControl - 0.42f) < 0.001f, "locomotion editor settings were not preserved");
    settings->GetMutablePlayerLocomotion() = original; settings->MarkDirty(); Require(settings->Save(), "restore settings failed");
    log << "OK Editor settings save/reload round trip" << std::endl;
    CheckDashPanelRelease(scene, log);
}
inline void AfterDraw(SceneManager* manager, const std::string& path) {
    static std::ofstream sLog(path, std::ios::trunc); static std::int32_t sPhase = 0, sFrames = 0; static bool sDone = false;
    if (sDone || !manager || manager->IsTransitioning()) return;
    if (sPhase == 0) { manager->ChangeScene("GAMEPLAY"); sPhase = 1; return; }
    if (manager->GetCurrentSceneName() != "GAMEPLAY") return;
    manager->SetIsPlaying(false);
    if (++sFrames < 3) return;
    try { Run(manager->GetCurrentScene(), sLog); sLog << "PASS: player locomotion startup, braking, jump, impulses, pause and settings" << std::endl; }
    catch (const std::exception& error) { sLog << "FAIL: " << error.what() << std::endl; }
    sDone = true; WinApp::CloseNow();
}
} // namespace playerLocomotionRuntimeProbe
#endif