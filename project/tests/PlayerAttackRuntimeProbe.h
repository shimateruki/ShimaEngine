#pragma once
#include <cstdint>
#ifdef USE_IMGUI
#include "SceneManager.h"
#include <numbers>
#include "Player.h"
#include "IMoveStrategy.h"
#include "KeyConfig.h"
#include "game/system/combat/BulletManager.h"
#include "game/actor/enemy/spawn/EnemyFactory.h"
#include "BaseEnemy.h"
#include "WinApp.h"
#include <fstream>
#include <stdexcept>
#include <string_view>

// 専用起動だけで隔離Actorを生成し、本編と同じ入力・能力・移動処理を固定刻みで確認します。
namespace playerAttackRuntimeProbe {
inline bool Enabled() {
    // 通常プレイでは起動しません。
    static const bool kEnabled = GetEnvironmentVariableA("CG2_PLAYER_ATTACK_TEST", nullptr, 0) > 0;
    return kEnabled;
}
class MoveInput final : public IMoveStrategy {
  public:
    void SetValue(const engine::Vector3& value) { value_ = value; }
    engine::Vector3 CalculateVelocity(Player*) override {
        // 実機入力に依存しない移動速度を、通常のMoverへ渡します。
        return value_;
    }
  private:
    engine::Vector3 value_{10, 0, 0};
};
inline void AfterDraw(SceneManager* manager, const std::string& path) {
    static std::ofstream log(path, std::ios::trunc);
    static std::unique_ptr<Player> sActor;
    static std::unique_ptr<PlayerMover> sMover;
    static MoveInput* sInput = nullptr;
    static EnemyAttackProfile sProfile;
    static std::int32_t sPhase = 0, sFrame = 0;
    static bool sDone = false;
    if (sDone || !manager || manager->IsTransitioning()) return;
    auto require = [](bool result, const std::string& text) { if (!result) throw std::runtime_error(text); };
    auto shots = [] {
        std::int32_t count = 0;
        for (const auto& bullet : BulletManager::GetInstance()->GetBullets())
            if (bullet && bullet->GetName() == "__PlayerAttackProbe") ++count;
        return count;
    };
    auto finish = [&](const std::string& text) {
        // GPU・シーン基盤より先にActorとMoverを片付け、終了処理も検証します。
        log << text << std::endl;
        sMover.reset(); sActor.reset();
        sDone = true;
        WinApp::CloseNow();
    };
    try {
        if (sPhase == 0) { manager->ChangeScene("GAMEPLAY"); sPhase = 1; return; }
        if (manager->GetCurrentSceneName() != "GAMEPLAY") return;
        manager->SetIsPlaying(false);
        if (sPhase == 1) {
            if (++sFrame < 3) return;
            auto* scene = manager->GetCurrentScene();
            sActor = std::make_unique<Player>();
            sActor->Initialize(scene->GetObject3DCommon(), engine::InputManager::GetInstance(), scene->GetParticleSystem(), scene->GetSpriteCommon());
            sActor->SetEditorInternal(true);
            sActor->SetCollisionAttribute(0); sActor->SetCollisionMask(0);
            sActor->SetTranslate({0, 1000, 0}); sActor->SetGrounded(true);
            sActor->SetCopyPreviewInput({}); sActor->SetCopyPreviewGroundHeight(1000);
            sActor->SetGravity(50); sActor->SetMaxFallSpeed(60);
            require(sActor->ApplyStoredCopy("FireSlime"), "apply stored fire");
            std::string error;
            require(sProfile.LoadForEnemy("FireSlime", &error), error);
            for (auto& event : sProfile.FindAttack("player_fireball")->sequence.events)
                if (event.operation == "projectile") event.name = "__PlayerAttackProbe";
            sActor->SetCopyPreviewProfile(sProfile);
            sMover = std::make_unique<PlayerMover>();
            sMover->Initialize(sActor.get(), engine::InputManager::GetInstance(), scene->GetParticleSystem());
            auto strategy = std::make_unique<MoveInput>(); sInput = strategy.get();
            sMover->SetStrategy(std::move(strategy));
            sFrame = 0; sPhase = 2;
            return;
        }
        constexpr float kFixedDeltaTime = 1.0f / 60;
        if (sPhase == 2 || sPhase == 3) {
            // 開始フレームの次からMove→物理→攻撃の順に進め、実プレイヤーと更新順を揃えます。
            PlayerCopyAbilityInput ability;
            ability.specialTriggered = true;
            ability.secondaryTriggered = sFrame > 0 && sFrame < 34;
            if (sPhase == 3 && sFrame == 5) sInput->SetValue({-10, 0, 0});
            if (sPhase == 3 && sFrame == 8) sActor->SetMoveYaw(std::numbers::pi_v<float>);
            sMover->Update(kFixedDeltaTime);
            sActor->SetCopyPreviewInput(ability);
            sActor->Update(kFixedDeltaTime);
            const float elapsed = (sFrame + 1) * kFixedDeltaTime;
            const std::int32_t expectedBefore = sPhase == 2 ? 0 : 2;
            if (sFrame < 11) require(shots() == expectedBefore, "fireball emitted before windup");
            if (sFrame == 12) require(shots() == expectedBefore + 1, "fireball did not emit once");
            if (sFrame > 0 && sFrame < 26) {
                require(sActor->GetAttackMovement().enabled, "attack interrupted or restriction absent");
                if (sPhase == 2) require(std::abs(sActor->GetVelocity().x - 2.0f) < 0.01f, "ground slowdown missing");
                if (sPhase == 3 && sFrame == 1) require(sActor->GetVelocity().x > 8.5f && sActor->GetVelocity().x < 10, "air movement stopped abruptly");
            }
            if (sFrame == 29) {
                require(!sActor->GetAttackMovement().enabled, "movement restriction outlived window");
                require(shots() == expectedBefore + 1, "recovery canceled into another shot");
            }
            if (sFrame == 49) {
                require(shots() == expectedBefore + 2, "next legal shot did not start");
                if (sPhase == 3) {
                    require(sActor->GetVelocity().y < 0 && sActor->GetTranslate().y < 1030, "air attack suspended gravity");
                    require(sActor->GetVelocity().x < 0, "air steering disabled");
                }
                log << "OK " << (sPhase == 2 ? "ground" : "air") << " windup, spam rejection, release and next-shot; t=" << elapsed << std::endl;
                // 次の予備動作中にコピーを切り替え、古い発射イベントと減速を破棄する。
                sActor->SetCopyPreviewProfile(sProfile);
                require(!sActor->GetAttackMovement().enabled, "copy reset retained restriction");
                sActor->SetCopyPreviewInput({true, false, false, false, false}); sActor->Update(kFixedDeltaTime);
                for (std::int32_t i = 0; i < 5; ++i) { sActor->SetCopyPreviewInput({}); sActor->Update(kFixedDeltaTime); }
                sActor->SetCopyPreviewProfile(sProfile);
                for (std::int32_t i = 0; i < 40; ++i) { sActor->SetCopyPreviewInput({}); sActor->Update(kFixedDeltaTime); }
                require(shots() == expectedBefore + 2, "canceled attack fired later");
                if (sPhase == 2) {
                    sActor->SetTranslate({0, 1030, 0}); sActor->SetVelocity({10, 4, 0}); sActor->SetGrounded(false);
                    sInput->SetValue({10, 0, 0}); sFrame = 0; sPhase = 3;
                    return;
                }
                // 各能力の調整に同じ入力・移動経路を使う。別の能力へは前の制限を持ち越さない。
                sPhase = 4;
                return;
            }
            ++sFrame;
        }
        if (sPhase == 4) {
            auto equip = [&](const char* type) {
                require(sActor->ApplyStoredCopy(type, true), std::string("apply ") + type);
                sActor->SetTranslate({0, 1000, 0});
                sActor->SetGrounded(true);
                sActor->SetVelocity({10, 0, 0});
                sInput->SetValue({10, 0, 0});
                require(!sActor->GetAttackMovement().enabled, "previous restriction carried to new copy");
            };
            auto step = [&](PlayerCopyAbilityInput input = {}, int frames = 1, bool applyMoveInput = true) {
                for (int i = 0; i < frames; ++i) {
                    if (applyMoveInput) sMover->Update(kFixedDeltaTime);
                    sActor->SetCopyPreviewInput(input);
                    sActor->Update(kFixedDeltaTime);
                }
            };
            auto movement = [&](float scale, bool hard, const char* label) {
                const auto policy = sActor->GetAttackMovement();
                require(policy.enabled && std::abs(policy.groundSpeedScale - scale) < .0001f &&
                    policy.blocksMovementActions == hard, label);
            };
            const PlayerCopyAbilityInput special{.specialTriggered = true};
            const PlayerCopyAbilityInput primary{.primaryTriggered = true, .primaryHeld = true};
            const PlayerCopyAbilityInput hold{.primaryHeld = true};
            const PlayerCopyAbilityInput secondary{.secondaryTriggered = true, .secondaryHeld = true};

            equip("FireSlime");
            step(primary); movement(.10f, true, "fire breath commitment");
            step(hold); require(std::abs(sActor->GetVelocity().x - 1.0f) < .01f, "fire breath did not brake");
            step(); require(!sActor->GetAttackMovement().enabled, "breath release retained brake");
            step(secondary); step({}, 28); movement(.15f, true, "fire dash recovery");
            step({}, 15); require(!sActor->GetAttackMovement().enabled, "fire dash recovery did not end");

            equip("ThunderSlime");
            step(primary); movement(.45f, false, "thunder charge");
            step(hold, 6); step(); movement(.10f, true, "thunder discharge recovery");
            step(secondary); movement(.10f, true, "thunder evade cleared recovery");
            step({}, 15); require(!sActor->GetAttackMovement().enabled, "discharge recovery did not end");
            step(special); movement(.12f, true, "thunder line startup");
            // 同じジャンプ入力を硬直中と復帰後に渡し、通常のジャンプ予約が速度を上書きする境界を確認する。
            auto* inputManager = engine::InputManager::GetInstance();
            const auto originalInput = inputManager->CaptureReplayState();
            const auto* jumpBinding = KeyConfig::GetInstance()->GetBindData("Jump");
            require(jumpBinding && jumpBinding->keyCode > 0 &&
                jumpBinding->keyCode < static_cast<int>(originalInput.keys.size()), "jump binding unavailable");
            auto jumpInput = originalInput;
            jumpInput.gamepadMode = false;
            jumpInput.keys.fill(0);
            jumpInput.previousKeys.fill(0);
            jumpInput.keys[jumpBinding->keyCode] = 0x80;
            inputManager->ApplyReplayState(jumpInput);
            sMover->SampleJumpInput(kFixedDeltaTime, false);
            sMover->Update(kFixedDeltaTime);
            inputManager->ApplyReplayState(originalInput);
            require(sActor->GetVelocity().y <= 0.0f, "jump canceled the attack restriction");

            step({}, 12); require(!sActor->GetAttackMovement().enabled, "thunder remained rooted during later strikes");
            inputManager->ApplyReplayState(jumpInput);
            sMover->SampleJumpInput(kFixedDeltaTime, false);
            sMover->Update(kFixedDeltaTime);
            inputManager->ApplyReplayState(originalInput);
            require(sActor->GetVelocity().y > 0.0f, "jump did not return after recovery");

            equip("WindSlime");
            step(primary); movement(.60f, false, "tornado sustained movement");
            step(hold, 16); step(); movement(.22f, true, "tornado recovery");
            step(secondary); movement(.22f, true, "lift bypassed tornado recovery");
            step({}, 16); step(special); movement(.38f, false, "vortex release");
            step({}, 12); require(!sActor->GetAttackMovement().enabled, "vortex recovery did not end");

            // 記憶台の既定値だけでなく、敵個体のプロフィールを使う吸収後の経路も実行する。
            auto source = EnemyFactory::GetInstance()->CreateEnemy("Bomber", manager->GetCurrentScene()->GetObject3DCommon());
            require(source != nullptr, "bomb copy source missing");
            sActor->SetTranslate({0, 1000, 0});
            sActor->SetGrounded(true);
            sActor->SetVelocity({10, 0, 0});
            sActor->DebugForceEnemyMorph(source.get());
            step(primary);
            const auto absorbedBombMovement = sActor->GetAttackMovement();
            sActor->DebugClearEnemyMorph();
            require(absorbedBombMovement.enabled && std::abs(absorbedBombMovement.groundSpeedScale - .15f) < .0001f,
                "absorbed bomb did not use the authored movement profile");

            equip("Bomber");
            step(primary); movement(.15f, true, "bomb place recovery");
            step(special); movement(.15f, true, "throw bypassed bomb place recovery");
            step({}, 18); step(special); movement(.45f, true, "bomb throw recovery");
            step({}, 12); step(secondary); movement(0.0f, true, "bomb blast windup");
            step({}, 9); require(!sActor->GetAttackMovement().enabled && sActor->GetVelocity().y > 0, "bomb blast failed to restore flight");

            equip("Slime");
            auto pinkState = [&] { return std::string_view(sActor->GetCopyAbilityStateLabel()); };
            // ピンクの専用移動は能力が更新するため、隔離用の歩行入力で速度を上書きしない。
            step(primary, 1, false); require(pinkState() != "待機", "pink strike did not start");
            step({}, 47, false); require(pinkState() == "待機", "pink strike did not return to idle");
            step(special, 1, false);
            for (int i = 0; i < 100 && pinkState() != "E: 着地硬直" && pinkState() != "待機"; ++i)
                step({}, 1, false);
            require(pinkState() == "E: 着地硬直", "pink bounce did not reach landing");
            step({}, 13, false);
            require(pinkState() == "E: 着地硬直" && std::abs(sActor->GetVelocity().x) < .01f && std::abs(sActor->GetVelocity().z) < .01f,
                "pink recovered too early or slid");
            step({}, 5, false); require(pinkState() == "待機", "pink did not recover after landing");

            equip("Crown");
            step(primary); movement(.45f, false, "crown charge");
            step(hold, 28); step();
            for (int i = 0; i < 36 && !sActor->GetAttackMovement().blocksMovementActions; ++i) step();
            movement(.12f, true, "crown first lance recoil");
            step(secondary); movement(.12f, true, "crown warp bypassed lance recoil");
            step({}, 17); require(!sActor->GetAttackMovement().enabled, "crown recoil did not end");

            equip("ThunderSlime");
            step(special); movement(.12f, true, "missing restriction before damage");
            sActor->SetTutorialSafetyEnabled(false);
            sActor->StartDamageFeedback({1.0f, 0.0f, 0.0f}, 0.0f);
            require(!sActor->GetAttackMovement().enabled && !sActor->IsControlActive(), "damage left the ability restriction active");
            step(special, 1, false); require(!sActor->GetAttackMovement().enabled, "copy input restarted attack during damage");
            // プレビューは毎フレーム操作を切るため、被弾側が復帰を通知したフレームを検出する。
            bool restored = false;
            for (int i = 0; i < 90 && !restored; ++i) {
                step({}, 1, false);
                restored = sActor->IsControlActive();
            }
            require(restored, "control did not return after damage");
            finish("PASS: player fireball ground/air, six copies, jump before/after recovery, interruption and damage control restored");
        }
    } catch (const std::exception& error) { finish(std::string("FAIL: ") + error.what()); }
}
} // namespace playerAttackRuntimeProbe
#endif
