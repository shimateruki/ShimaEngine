#pragma once
#ifdef USE_IMGUI
#include <cstdint>
#include "SceneManager.h"
#include "Object3DCommon.h"
#include "DirectXCommon.h"
#include "DebrisEffectManager.h"
#include "WinApp.h"
#include <fstream>
#include <stdexcept>
#include <string>

// 実Deviceで軽量→全体の準備と、ロード画面を挟む実Scene遷移での資源再利用を調べる。
// 通常起動では何も生成・変更しない。既存のシーン遷移検証から明示的に使う。
namespace sharedRenderCommonsRuntimeProbe {
inline bool Enabled() { return GetEnvironmentVariableA("CG2_SHARED_COMMONS_TEST", nullptr, 0) != 0; }
inline bool CheckAfterDraw(SceneManager* manager, const std::string& path) {
    if (!Enabled()) return true;
    static std::ofstream log(path + ".commons", std::ios::trunc);
    static bool sInitialized = false;
    static bool sFinished = false;
    static engine::Object3DCommon* sObject = nullptr;
    static engine::SpriteCommon* sSprite = nullptr;
    static engine::ParticleCommon* sParticle = nullptr;
    static ID3D12PipelineState* sSkybox = nullptr;
    static uint64_t sGeneration = 0;
    static std::uint32_t sEntered = 0;
    static bool sSawLoading = false;
    if (sFinished) return false;
    auto require = [](bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    };
    try {
        auto* resources = manager->GetRenderCommonResources();
        require(resources != nullptr, "engine resources missing");
        if (!sInitialized) {
            // Sceneで使う実体とは別の未描画資源で、Release向け軽量準備も実Device上で検証する。
            engine::RenderCommonResources local;
            local.Initialize(engine::DirectXCommon::GetInstance());
            local.Prepare(engine::RenderPreparation::kSpritesOnly);
            auto* localSprite = local.GetSpriteCommon();
            require(localSprite && !local.GetObject3DCommon(), "loading profile initialized unnecessary 3D resources");
            local.Prepare(engine::RenderPreparation::kMinimal);
            auto* localObject = local.GetObject3DCommon();
            auto* minimalRoot = localObject->GetSkyboxRootSignature();
            auto* minimalPso = localObject->GetSkyboxPipelineState();
            auto* localParticle = local.GetParticleCommon();
            require(!local.IsReady(engine::RenderPreparation::kFull), "minimal was treated as full");
            std::uint32_t steps = 0;
            while (!local.PrepareStep(engine::RenderPreparation::kFull)) require(++steps < 30, "full preparation did not finish");
            require(local.GetSpriteCommon() == localSprite && local.GetObject3DCommon() == localObject &&
                    local.GetParticleCommon() == localParticle, "common instance replaced during upgrade");
            require(localObject->GetSkyboxRootSignature() == minimalRoot &&
                    localObject->GetSkyboxPipelineState() == minimalPso, "ready GPU resources recreated during upgrade");
            require(local.PrepareStep(engine::RenderPreparation::kFull) &&
                    local.PrepareStep(engine::RenderPreparation::kMinimal), "repeated preparation did not finish immediately");
            log << "PASS sprites-only -> minimal -> full; retained instances and skybox GPU resources" << std::endl;
            engine::RenderCommonResources interrupted;
            interrupted.Initialize(engine::DirectXCommon::GetInstance());
            interrupted.PrepareStep(engine::RenderPreparation::kFull);
            interrupted.PrepareStep(engine::RenderPreparation::kFull);
            interrupted.Prepare(engine::RenderPreparation::kMinimal);
            steps = 0;
            while (!interrupted.PrepareStep(engine::RenderPreparation::kFull)) require(++steps < 30, "interrupted full preparation lost its state");
            log << "PASS partial full -> minimal -> full" << std::endl;
            sObject = resources->GetObject3DCommon();
            sSprite = resources->GetSpriteCommon();
            sParticle = resources->GetParticleCommon();
            require(sObject && sSprite && sParticle, "initial scene resources missing");
            sSkybox = sObject->GetSkyboxPipelineState();
            sInitialized = true;
        }
        require(resources->GetObject3DCommon() == sObject && resources->GetSpriteCommon() == sSprite &&
                resources->GetParticleCommon() == sParticle, "engine common instance changed during transition");
        require(sObject->GetSkyboxPipelineState() == sSkybox, "scene transition recreated a ready PSO");
        auto* scene = manager->GetCurrentScene();
        require(scene && scene->GetSpriteCommon() == sSprite, "scene does not borrow engine sprite common");
        if (scene->GetRenderPreparation() == engine::RenderPreparation::kSpritesOnly) {
            require(scene->GetObject3DCommon() == nullptr, "loading scene exposes 3D render objects");
            if (!sSawLoading) log << "PASS loading scene shares sprite common" << std::endl;
            sSawLoading = true;
        } else {
            require(scene->GetObject3DCommon() == sObject && scene->GetParticleCommon() == sParticle,
                    "scene does not borrow engine 3D/particle commons");
            if (!manager->IsTransitioning() && sGeneration != manager->GetSceneGeneration()) {
                sGeneration = manager->GetSceneGeneration();
                // 同じCommonを使い回しても、前のSceneの実行中状態が残らないことを確認する。
                auto* debris = DebrisEffectManager::GetInstance();
                require(debris->GetActivePieceCount() == 0, "debris from previous scene survived");
                DebrisEffectConfig marker;
                marker.spawnCount = 1;
                marker.lifetimeMin = marker.lifetimeMax = 120.0f;
                marker.gravity = 0.0f;
                marker.speedMin = marker.speedMax = 0.0f;
                marker.collideGround = false;
                debris->SpawnFromConfig(marker, {0.0f, -1000.0f, 0.0f});
                require(debris->GetActivePieceCount() > 0, "debris lifetime fixture did not spawn");
                log << "BORROW " << manager->GetCurrentSceneName() << " generation=" << sGeneration << std::endl;
                if (++sEntered == 8) {
                    require(sSawLoading, "loading transition was not observed");
                    log << "PASS all scene types and direct reload retain shared commons" << std::endl;
                }
            }
        }
        return true;
    } catch (const std::exception& exception) {
        log << "FAIL " << exception.what() << std::endl;
        sFinished = true;
        WinApp::CloseNow();
        return false;
    }
}
} // namespace sharedRenderCommonsRuntimeProbe
#endif
