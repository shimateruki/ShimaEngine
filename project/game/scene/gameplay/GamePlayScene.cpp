#define NOMINMAX
#include "game/scene/gameplay/GamePlayScene.h"
#include "engine/system/scene/SceneEngineIncludes.h"
#include "engine/graphics/scene/SceneRenderCommands.h"
#include "engine/graphics/3d/light/SceneLighting.h"
#include "SceneController.h"
#include "game/scene/GameSceneDrawing.h"
#include "ParticleCommon.h"
#include "Skybox.h"
#include "Text.h"
#include "game/ui/ControlsGuideOverlay.h"
#include "game/actor/gimmick/trigger/GimmickGuideSign.h"
#include "game/ui/PauseMenuOverlay.h"
#include "game/ui/SettingsMenuOverlay.h"
#include "game/ui/SaveIndicatorOverlay.h"
#include "imgui.h"
#include <game/system/rules/EventManager.h>
#include "DebugConsole.h"
#include "ProfilerManager.h"
#include "RenderStats.h"
#include <cassert>
#include "game/system/combat/BulletManager.h"
#include "MoveStrategy3D.h"
#include "MoveStrategy2D.h"
#include "game/system/scene/LevelLoader.h"
#include "game/system/combat/LockOnSystem.h"
#include "game/system/rules/GameRule.h"
#include "game/system/scene/ObjectManager.h"
#include "MeshEffectManager.h"
#include "WinApp.h"
#include "IconsFontAwesome5.h"
#ifdef _DEBUG
#include "ParticleEditor.h"
#endif

// --- JSON (保存機能) ---
#include <fstream>
#include <string>
#include "json.hpp"
#include <numbers>
#include "engine/editor/graphics/camera/CameraEditor.h"
#include <BaseEnemy.h>
#include "game/actor/enemy/boss/prism/EnemyPrismSlime.h"
#include "game/actor/enemy/boss/false_king/EnemyFalseKingSlime.h"
#include <game/actor/enemy/spawn/EnemyFactory.h>
#include <game/actor/enemy/spawn/EnemySpawner.h>
#include <game/system/scene/GimmickFactory.h>
#include <game/system/scene/ItemFactory.h>
#include <LightEditor.h>
#include <ParticleManager.h>
#include <GpuParticleManager.h>
#include <SrvManager.h>
#include <PostEffect.h>
#include "Fade.h"
#include "game/system/progress/StageManager.h"
#include "game/system/progress/GameDataManager.h"
#include "game/actor/player/state/PlayerState.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

// シーンの寿命は本体が管理し、演出ファイルは既存メンバーを借用する。
GamePlayScene::GamePlayScene() {
    // シーン固有リソースの生成はInitializeへまとめる。
}

GamePlayScene::~GamePlayScene() {
    // シーン固有リソースの終了はFinalizeへまとめる。
}

void GamePlayScene::Draw() {
    // 近距離では自キャラのモデルが視界を塞ぐため、通常パス・半透明パス・特殊材質パスで
    // 同じ「プレイヤーとその子を描かない」判定を使います。王冠演出中は専用カメラなので除外します。
    bool isFirstPerson = false;
    engine::Camera* camera = CameraManager::GetInstance()->GetActiveCamera();
#ifndef _DEBUG
    // このビルドでは、追従先がある一人称カメラなら、距離を測らずプレイヤーを描画対象から外す。
    if (camera->GetFollowTarget() && camera->GetFollowMode() == engine::Camera::FollowMode::kFirstPerson) {
        isFirstPerson = true;
    }
#endif

    // 王冠演出中は距離による非表示を行わない。その他の未判定時は共通処理で、視点が胸元から3ワールド単位未満にあるかを調べる。
    if (!crownFinale_.active && !crownDefeat_.active && !crownDefeat_.completed && !isFirstPerson) {
        isFirstPerson = IsCameraNearViewActor(camera, player_);
    }

    // この描画で使うライトを一度借り、通常時と残機喪失演出の入口へ同じ値を渡す。
    const auto lighting = engine::SceneLighting::Capture();

    // 残機喪失中は演出専用の世界だけを描き、通常のオブジェクト群を重ねません。
    if (lifeLostState_.IsActive()) {
        DrawLifeLostPresentationWorld(lighting);
        return;
    }

    // 黒画面を保持する間はワールド描画を止め、UI側の遷移表示に任せます。
    if (lifeLostState_.HoldsBlack()) {
        return;
    }

    auto& objects = objectManager_->GetObjects();

    // 背景を先に描き、通常3Dの設定を適用します。
    BeginWorldObjectDrawing(skybox_.get(), camera);

    // 不透明パスでは、別のパイプラインを必要とする霧・流体・半透明を除きます。
    GetObject3DCommon()->SetPipelineState(BlendMode::kNone); // 不透明設定

    DrawSceneObjectPass(objects, materialRenderPolicy::Pass::kOpaque, lighting,
        player_, isFirstPerson, true, [this](engine::Object3D* object) { return IsVisible(object); });

    // 王冠の継承・撃破演出が所有するモデルは通常の配置集合と別に描きます。
    DrawCrownFinaleWorld(lighting);
    DrawCrownDefeatWorld(lighting);

    // ロック対象の印は本体の視点非表示判定から独立させます。
    if (player_ && player_->GetHookMarker()) {
        lighting.Draw(*player_->GetHookMarker(), &engine::Object3D::Draw);
    }

    // アニメーション確認用の立方体が用意されている場合だけ、通常の世界へ描く。
    if (animatedCube_) {
        lighting.Draw(*animatedCube_, &engine::Object3D::Draw);
    }

    // 弾と攻撃予兆は背景・地形の後、半透明の粒子より前のパスで描きます。
    DrawEnemyTelegraphs(*this, lighting);
    lighting.Draw(*BulletManager::GetInstance(), &BulletManager::Draw);
    LightEditor::GetInstance()->Draw3D();

    // 半透明モデルは不透明物の深度が決まった後に描画します。
    DrawSceneObjectPass(objects, materialRenderPolicy::Pass::kTransparent, lighting,
        player_, isFirstPerson, true, {});

    // 敵が所有する加算・半透明の攻撃表示は、床を含むシーンの深度が揃ってから重ねます。
    for (const auto& object : objects) {
        // 描画できる状態の実体にだけ、追加の半透明表示を描くよう依頼する。
        if (object && object->GetIsRenderVisible()) {
            lighting.Draw(*object, &engine::Object3D::DrawOwnedTransparentVisuals);
        }
    }
    particleSystem_->Draw();

    // ローカルフォグは通常モデルと分け、深度を参照する描画区間へつなぎます。
    DrawLocalFogObjects(objects, player_, isFirstPerson);

    // 描画コマンドを取得できない場合やカメラプレビュー中は、本編の背景色と深度を操作しない。
    auto* commands = GetRenderCommands();
    if (!commands || commands->IsCameraPreviewRendering()) {
        return;
    }

    const bool sceneColorCopied = DrawDepthReadingEffects(*commands, objects, *camera, isFirstPerson);
    DrawAttachedMeshEffects(*commands, objects, sceneColorCopied, lighting);
}

bool GamePlayScene::DrawDepthReadingEffects(
    engine::SceneRenderCommands& commands, const std::vector<std::unique_ptr<engine::Object3D>>& objects, const engine::Camera& camera, bool isFirstPerson) {
    // 画面色のコピーは高コストなので、流体または歪み粒子が必要なフレームだけ行います。
    // hasSpecialMaterialDrawsにはObjectが所有する追加の特殊材質と弾の特殊描画も含めます。
    bool hasSpecialMaterialDraws = BulletManager::GetInstance()->HasSpecialMaterialBullets();

    for (auto& obj : objects) {
        // 本体か所有する表示に特殊材質を使う物が一つでもあれば、特殊描画が必要になる。
        if (IsSpecialMaterialType(obj->GetMaterialType()) || obj->HasOwnedSpecialMaterialVisuals()) {
            hasSpecialMaterialDraws = true;
            break;
        }
    }

    // GPU粒子には背景色が必要な種類と深度だけを読む種類があるため、コピー要否を分けます。
    GpuParticleManager* gpuParticleManager = GpuParticleManager::GetInstance();
    const bool hasGpuParticles = !gpuParticleManager->IsEmpty();
    const bool gpuParticlesNeedGrab = hasGpuParticles && gpuParticleManager->RequiresSceneColorCopy();
    bool sceneColorCopied = false;

    // 特殊材質またはGPU粒子を描く場合は、深度を読み取る区間を用意する。
    if (hasSpecialMaterialDraws || hasGpuParticles) {
        // 特殊マテリアルか歪みパーティクルが背景色を使う時だけ、画面全体をコピーする。
        if (hasSpecialMaterialDraws || gpuParticlesNeedGrab) {
            commands.CopySceneColor();
            sceneColorCopied = true;
        }

        // 水・炎・GPU粒子は深度をSRVとして読むため、この区間ではDSVを外します。
        // EndDepthReadで描画先を戻すまでは、通常の3D描画を挟まない。
        commands.BeginDepthRead();

        // 画面の色を使う特殊材質がある場合に、地形・所有する表示・弾の専用描画を行う。
        if (hasSpecialMaterialDraws) {
            const uint32_t depthSrvHandle = commands.GetDepthSrvHandle();
            const uint32_t sceneColorSrvHandle = commands.GetSceneColorSrvHandle();
            for (auto& obj : objects) {
                // 近接視点で非表示となる本体と子は、特殊材質パスにも描かない。
                if (IsHiddenByFirstPerson(obj.get(), player_, isFirstPerson)) {
                    continue;
                }

                // 描画順と画面色・深度の準備はシーンで管理し、材質ごとの選択は描画側に委ねる。
                obj->DrawSpecialMaterial(depthSrvHandle, sceneColorSrvHandle);
                obj->DrawOwnedSpecialMaterialVisuals(depthSrvHandle, sceneColorSrvHandle);
            }

            BulletManager::GetInstance()->DrawSpecial(depthSrvHandle, sceneColorSrvHandle);
        }

        // GPU粒子が稼働している場合だけ、現カメラの行列と深度を渡して描く。
        if (hasGpuParticles) {
            commands.RecordGpuParticles(*gpuParticleManager, camera, gpuParticleTexHandle_);
        }

        commands.EndDepthRead();
    }

    return sceneColorCopied;
}

void GamePlayScene::DrawAttachedMeshEffects(
    engine::SceneRenderCommands& commands, const std::vector<std::unique_ptr<engine::Object3D>>& objects, bool sceneColorCopied,
    const engine::SceneLighting& lighting) {
    bool hasMeshEffects = false;

    for (auto& obj : objects) {
        // 付属エフェクト欄のいずれかが設定された物を見つけたら、付属描画が必要と判断する。
        if (!obj->GetMeshEffect1Name().empty() || !obj->GetMeshEffect2Name().empty()) {
            hasMeshEffects = true;
            break;
        }
    }

    // 付属表示が必要なフレームだけ、背景を準備して各Objectを描く。
    if (hasMeshEffects) {
        // 深度を読む描画で背景をコピーしていない場合だけ、付属エフェクトの直前にコピーする。
        if (!sceneColorCopied) {
            commands.CopySceneColor();
        }

        // 各オブジェクトのアタッチ済みエフェクトを描画
        for (auto& obj : objects) {
            // 非表示のObjectは、取り付けたエフェクトも描かない。
            if (!obj->GetIsVisible())
                continue;
            lighting.Draw(*obj, &engine::Object3D::DrawAttachedEffects);
        }
    }
}

// ====================================================================
// UI描画専用の関数
// ====================================================================
void GamePlayScene::DrawUi() {
    // ワールド描画の後に2Dを重ねます。早期returnする演出は通常HUDと同時表示しません。
    BeginSpriteDrawing();

    const auto drawGuideSigns = [&](engine::Camera* camera) {
        // 配置管理が無ければ案内看板を列挙できないため、処理を省く。
        if (!objectManager_)
            return;
        for (const auto& object : objectManager_->GetObjects())
            // 案内看板型に限り、現在のカメラ指定を渡す。null指定の場合は通常案内を表示しない。
            if (auto* sign = dynamic_cast<GimmickGuideSign*>(object.get()))
                sign->DrawGuide(camera);
    };

    // 残機演出は専用表示が含まれるHUD経路に任せ、通常UIを別に重ねません。
    if (lifeLostState_.IsActive() || lifeLostState_.HoldsBlack()) {
        drawGuideSigns(nullptr);
        DrawGameplayHUD();
        return;
    }

    // クリア演出中は通常HUDを隠し、カメラとリザルトへ視線を集中させます。
    if (isGoal_) {
        drawGuideSigns(nullptr);
        DrawGoalPresentationOverlay();
        return;
    }

    // ボス登場中は名前だけを描き、通常HUDは下の分岐で隠します。
    if (arenaBossIntroActive_) {
        DrawCrownFinaleUi();
    }

    // 黒帯を使うムービーでは通常HUD、照準、配置Sprite、案内をまとめて隠します。
    // 演出状態が解除された次のフレームから通常描画へ戻ります。
    if (stageEntryPresentationActive_ || arenaBossIntroActive_ || arenaBossRewardActive_ ||
        sealBarrageMovie_.active || crownFinale_.active || crownDefeat_.active) {
        drawGuideSigns(nullptr);
        return;
    }

    // 疾走線をワールドの上、照準やHUDの下に重ねる。
    const bool modalOpen = pausePageSliding_ || (controlsGuideOverlay_ && controlsGuideOverlay_->IsActive()) ||
                           (pauseMenuOverlay_ && pauseMenuOverlay_->IsActive()) ||
                           (settingsOverlay_ && settingsOverlay_->IsActive());
    // 図鑑などが動画を再生する前に、看板側の共有転送先を解放します。
    drawGuideSigns(modalOpen ? nullptr : CameraManager::GetInstance()->GetActiveCamera());
    // モーダルが無い間だけ、疾走線をHUDの背後に重ねる。
    if (!modalOpen) {
        for (const auto& line : dashSpeedLineSprites_)
            // 存在し、現在のフレームで表示が有効な線だけを描く。
            if (line && line->IsVisible())
                line->Draw();
    }

    // JSON配置のSpriteとプレイヤーUIを描いた後、照準・HUD・モーダルの順に重ねます。
    for (auto& sprite : sprites_) {
        // HUDで専用に描くものを除き、実体のある配置Spriteを先に描く。
        if (sprite && !IsGameplayHUDSprite(sprite.get())) {
            sprite->Draw();
        }
    }

    // プレイヤー固有のUIはJSON配置の背景より手前、HUDとモーダルより奥に置きます。
    if (player_) {
        player_->DrawUi();
    }

    // プレイヤーの照準UIに埋もれないよう、選択対象のマーカーを後から描きます。
    if (isDrawLockOn_ && lockOnSprite_) {
        lockOnSprite_->Draw();
    }

    DrawGameplayHUD();
    DrawCrownFinaleUi();

    // 設定画面はポーズより手前に置き、保存通知はモーダルの後段へ描画します。
    if (controlsGuideOverlay_ && controlsGuideOverlay_->IsActive()) {
        controlsGuideOverlay_->Draw();
    }

    // ポーズを表示中で、設定画面が前面に出ていない場合にだけ描く。
    if (pauseMenuOverlay_ && pauseMenuOverlay_->IsActive() && !(settingsOverlay_ && settingsOverlay_->IsActive())) {
        pauseMenuOverlay_->Draw();
    }

    // 設定画面が開いている場合に、共通HUDより手前へ重ねる。
    if (settingsOverlay_ && settingsOverlay_->IsActive()) {
        settingsOverlay_->Draw();
    }

    // 保存通知が有効な場合は、メニューの描画後に重ねる。
    if (saveIndicatorOverlay_ && saveIndicatorOverlay_->IsActive()) {
        saveIndicatorOverlay_->Draw();
    }

    DrawGoalPresentationOverlay();
}

void GamePlayScene::DrawShadow() {
    // 配置オブジェクトを影パスへ渡し、通常描画と同じ管理集合を使う。
    if (objectManager_) {
        objectManager_->DrawShadow();
    }
}

void GamePlayScene::StartBridgeDropMovie() {
    // 互換用の呼び出し口。現時点ではこの経路で演出を開始しない。
}

bool GamePlayScene::IsVisible(engine::Object3D* obj) {
    // 実体が無い場合か非表示指定の場合は、画角の検査をする前に除外する。
    if (!obj || !obj->GetIsVisible()) {
        return false;
    }

    // 所有する槍・封印と王冠Morph中のプレイヤーは本体のAABBだけで除外しません。
    if (dynamic_cast<EnemyFalseKingSlime*>(obj) ||
        (obj == player_ && player_->GetEnemyMorphType() == Player::EnemyMorphType::kCrown)) {
        return true;
    }

    return IsSceneObjectVisibleByFrustum(obj, CameraManager::GetInstance()->GetActiveCamera());
}
