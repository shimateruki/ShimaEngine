#define NOMINMAX
#include "game/scene/select/GameSelectScene.h"
#include "Easing.h"
#include "engine/system/scene/SceneEngineIncludes.h"
#include "engine/graphics/3d/light/SceneLighting.h"
#include "game/scene/GameSceneDrawing.h"
#include "game/ui/PauseMenuOverlay.h"
#include "game/ui/SettingsMenuOverlay.h"
#include "ScenePreloader.h"

#include <algorithm>
#include <cmath>
#include "ParticleCommon.h"
#include "Player.h"
#include "Skybox.h"
#include "Text.h"
#include "imgui.h"
#include <game/system/rules/EventManager.h>
#include "DebugConsole.h"
#include <cassert>
#include "game/system/combat/BulletManager.h"
#include "MoveStrategy3D.h"
#include "MoveStrategy2D.h"
#include "game/system/scene/LevelLoader.h"
#include "game/system/combat/LockOnSystem.h"
#include "game/system/rules/GameRule.h"
#include "game/system/scene/ObjectManager.h"
#include "MeshEffectManager.h"
#include "VfxSequencer.h"
#include "WinApp.h"
#ifdef _DEBUG
#include "ParticleEditor.h"
#endif

#include <fstream>
#include <string>
#include "json.hpp"
#include <numbers>
#include "engine/editor/graphics/camera/CameraEditor.h"
#include <BaseEnemy.h>
#include <game/actor/enemy/spawn/EnemyFactory.h>
#include <game/actor/enemy/spawn/EnemySpawner.h>
#include <LightEditor.h>
#include <ParticleManager.h>
#include <GpuParticleManager.h>
#include <SrvManager.h>
#include <PostEffect.h>
#include "game/system/progress/StageManager.h"
#include "game/system/progress/GameDataManager.h"
#include "game/system/settings/GameAudioSettings.h"
#include "game/system/settings/GameSettingsManager.h"
#include "GimmickStageGate.h"
#include "CollisionConfig.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdint>
#include <sstream>

#include "game/scene/select/GameSelectScene.Internal.h"
using namespace stageSelectDetail;

// ステージ選択のゲート表示は保存済み進行を基準に決めます。入場操作と解放演出を同じフレームで競合させません。

GameSelectScene::GameSelectScene() {
    // 共有システムの初期化は先読み段階まで遅らせる。
}

GameSelectScene::~GameSelectScene() {
    // シーン固有のリソース解放は Finalize にまとめる。
}

SceneLoadManifest GameSelectScene::BuildAsyncLoadManifest() const {
    // ゲート名とスターを先読みし、入場時の画像読み込み停止を避ける。
    SceneLoadManifest manifest;
    manifest.AddObjectLayout(HasSceneAssetContext() && !GetSceneLoadContext().objectLayoutPath.empty()
                                 ? GetSceneLoadContext().objectLayoutPath
                                 : "Resources/json/3Dobject/stageSelect.json");
    manifest.AddSpriteLayout(HasSceneAssetContext() && !GetSceneLoadContext().spriteLayoutPath.empty()
                                 ? GetSceneLoadContext().spriteLayoutPath
                                 : "Resources/json/sprite/stageSelect_sprite.json");
    manifest.AddTexture("Resources/sprite/common/circle2.png");
    manifest.AddTexture("Resources/sprite/common/white.png");
    manifest.AddTexture("Resources/sprite/ui/hud/lockOn.png");
    manifest.AddTexture("Resources/sprite/ui/stage_select/stage1_gate_name.png");
    manifest.AddTexture("Resources/sprite/ui/stage_select/stage2_gate_name.png");
    manifest.AddTexture("Resources/sprite/ui/stage_select/stage3_gate_name.png");
    manifest.AddTexture("Resources/sprite/ui/stage_select/tutorial_gate_name.png");
    manifest.AddTexture("Resources/sprite/ui/hud/stage_star_filled.png");
    manifest.AddTexture("Resources/sprite/ui/hud/stage_star_empty.png");
    manifest.AddTexture(GetSceneLoadContext().skyboxPath.empty() ? "Resources/output_skybox.dds"
                                                                 : GetSceneLoadContext().skyboxPath);
    return manifest;
}

void GameSelectScene::Initialize() {
    // 同期初期化でも段階ロードと同じ順序を使用する。
    BeginLoadingInitialize();

    // 通常の同期初期化では、段階ロードを呼び出し内で完了させます。
    while (!InitializeLoadingStep()) {
    }
}

void GameSelectScene::BeginLoadingInitialize() {
    // 進捗表示用の作業数を、InitializeLoadingStep の段階数に合わせる。
    loadingInitializePhase_ = 0;
    loadingInitializeCompletedUnits_ = 0;
    loadingInitializeTotalUnits_ = 9;
}

bool GameSelectScene::InitializeLoadingStep() {
    // リソース生成を段階に分け、先読み時に一フレームへ集中させない。
    auto completeUnit = [this]() { ++loadingInitializeCompletedUnits_; };

    switch (loadingInitializePhase_) {
    // 最初の段階で進行データと入力を準備し、前回の演出の状態をリセットします。
    case 0:
        inputManager_ = engine::InputManager::GetInstance();
        LOG("Game Select Initialized!");
        StageManager::GetInstance()->Initialize();
        // 進行中のステージを起点に、クリア直後は解放済みの次のゲートを初期選択する。
        selectedStageIndex_ = StageManager::GetInstance()->GetCurrentStageIndex();
        // 保存中の選択番号がステージ一覧外なら、初期ステージを候補にします。
        if (selectedStageIndex_ < 0 ||
            selectedStageIndex_ >= static_cast<std::int32_t>(StageManager::GetInstance()->GetStages().size())) {
            selectedStageIndex_ = 0;
        }

        // 現在ステージがクリア済みで、次が解放されていれば次の番号を選びます。
        if (GameDataManager::GetInstance()->IsStageCleared(selectedStageIndex_) &&
            IsStageUnlocked(selectedStageIndex_ + 1)) {
            ++selectedStageIndex_;
        }

        // 再入場時に前回のゲート操作や演出の参照を持ち越さないよう、選択以外の進行状態を戻す。
        previousSelectedStageIndex_ = -1;
        stageDecisionCooldown_ = 0.0f;
        isChangingStage_ = false;
        unlockingStageIndex_ = -1;
        unlockTime_ = {};
        unlockPresentationInitialized_ = false;
        unlockPresentationDebugReplay_ = false;
        unlockHadPlayerControl_ = true;
        unlockDebugPreviousSelectedStageIndex_ = -1;
        unlockTargetGate_ = nullptr;
        unlockKeyObject_ = nullptr;
        unlockLockObject_ = nullptr;
        stageSelectTime_ = 0.0f;
        crownCountPresentationActive_ = false;
        crownCountPresentationImpactDone_ = false;
        crownCountPresentationStageIndex_ = -1;
        crownCountPresentationFrom_ = 0;
        crownCountPresentationTo_ = 0;
        crownCountPresentationTimer_ = 0.0f;
        crownCountPresentationParticleTimer_ = 0.0f;
        gateReturnPresentationActive_ = false;
        gateReturnPresentationTimer_ = 0.0f;
        gateReturnStageIndex_ = -1;
        gateReturnTargetGate_ = nullptr;
        pendingStageClearRewardPresentation_ = {};
        ++loadingInitializePhase_;
        completeUnit();
        return false;

    case 1:
        // 配置を読む前に、カメラ入力とパーティクル、ステージ選択のルールを準備する。
        CameraManager::GetInstance()->Initialize();
        CameraManager::GetInstance()->SetInputManager(inputManager_);
        particleSystem_ = std::make_unique<engine::ParticleSystem>();
        particleSystem_->Initialize(GetParticleCommon(), "Resources/sprite/common/circle2.png");
        ParticleManager::GetInstance()->Initialize(particleSystem_.get());
        gameRule_ = std::make_unique<GameRule>();
        gameRule_->Initialize(this);
        LightEditor::GetInstance()->SetObject3DCommon(GetObject3DCommon());
        ++loadingInitializePhase_;
        completeUnit();
        return false;

    case 2: {
        // 配置とゲート判定に使う管理クラスを作り、ロックオンと案内の描画部品も確保する。
        objectManager_ = std::make_unique<ObjectManager>();
        lockOnSystem_ = std::make_unique<LockOnSystem>();
        lockOnSystem_->Initialize(inputManager_);
        const uint32_t lockOnTex = engine::TextureManager::GetInstance()->Load("Resources/sprite/ui/hud/lockOn.png");
        lockOnSprite_ = std::make_unique<engine::Sprite>();
        lockOnSprite_->Initialize(GetSpriteCommon(), lockOnTex);
        lockOnSprite_->SetAnchorPoint({0.5f, 0.5f});
        lockOnSprite_->SetSize({64.0f, 64.0f});
        BulletManager::GetInstance()->Initialize(GetObject3DCommon(), engine::CollisionManager::GetInstance());
        ++loadingInitializePhase_;
        completeUnit();
        return false;
    }

    case 3:
        // GPU 側を初期化してから、鍵とゲートで使用するプリセットを先読みする。
        PrepareGpuParticles();
        GpuParticleManager::GetInstance()->PreloadPresetSystems(
            {kCrownUnlockParticlePreset, kCrownUnlockAfterglowParticlePreset, kGateUnlockCoreParticlePreset,
             kGateUnlockSparkParticlePreset});
        gpuParticleTexHandle_ = engine::TextureManager::GetInstance()->Load("Resources/sprite/common/white.png");
        ++loadingInitializePhase_;
        completeUnit();
        return false;

    case 4:
        // このシーンの空の設定を優先し、個別指定がなければ既定の DDS を使う。
        skyboxTextureHandle_ =
            engine::TextureManager::GetInstance()->Load(ResolveSceneSkyboxPath("Resources/output_skybox.dds"));
        skybox_ = std::make_unique<Skybox>();
        skybox_->Initialize(GetObject3DCommon(), skyboxTextureHandle_);
        ++loadingInitializePhase_;
        completeUnit();
        return false;

    // 先に3D配置を読み込み、旧シーンで保存されていたエフェクトや静的プレイヤー設定を清掃します。
    case 5:
        levelLoader_ = std::make_unique<LevelLoader>();
        levelLoader_->LoadObjectLayout(this, "Resources/json/3Dobject/stageSelect.json");

        // 旧配置から読み込んだ中央の王冠の常時エフェクトも解除する。
        if (auto* crown = FindObjectByName("StageSelect_CrownCore")) {
            crown->RemoveParticleEmitterComponent();
            crown->RemoveMeshEffectComponent();
        }

        // 旧形式のSceneでPlayerが静的保存されていても、床との衝突解決対象に戻します。
        if (player_) {
            player_->SetStatic(false);
        }

        ++loadingInitializePhase_;
        completeUnit();
        return false;

    case 6:
        // HUD の元になる Sprite 配置を読み込み、実行時に使用する部品を紐づける。
        levelLoader_->LoadSpriteLayout(this, "Resources/json/sprite/stageSelect_sprite.json");
        // 旧レイアウトに保存された固定案内の部品も実行時の一覧から除く。
        std::erase_if(sprites_, [](const auto& sprite) {
            return sprite && sprite->GetName().starts_with("stage_gate_info_");
        });
        InitializeStageSelectHUD();
        InitializeStageGateNameLabels();
        LoadStageGateNameSettings();
        // ステージ選択用ポーズ画面と、そこから開く共通設定画面を準備する。
        pauseMenuOverlay_ = std::make_unique<PauseMenuOverlay>();
        pauseMenuOverlay_->Initialize(GetSpriteCommon(), "Resources/json/sprite/selectPauseMenu.json",
                                      PauseMenuOverlay::Mode::kStageSelect);
        settingsOverlay_ = std::make_unique<SettingsMenuOverlay>();
        settingsOverlay_->Initialize(GetSpriteCommon());
        ++loadingInitializePhase_;
        completeUnit();
        return false;

    case 7:
        // カメラ編集データを初期化し、配置に対応する保存済みカメラを適用する。
        CameraEditor::GetInstance()->Initialize();
        CameraEditor::GetInstance()->LoadFile(ResolveSceneCameraPath("game_camera.json"));
        ++loadingInitializePhase_;
        completeUnit();
        return false;

    // 最後に保存データから出迎えと報酬の要求を取り出し、必要なゲート表示を設定します。
    case 8: {
        auto* gameData = GameDataManager::GetInstance();
        gameData->MarkStageUnlockSeen(0);
        // クリア報酬を受け取り、帰還演出がなければ続けて報酬演出を始める。
        pendingStageClearRewardPresentation_ = gameData->ConsumeStageClearRewardPresentation();
        const bool startedReturnPresentation =
            StartStageReturnPresentation(gameData->ConsumeStageSelectReturnPresentation());
        // 帰還演出が始まらなかった場合は、保留した報酬演出をすぐに試して要求を消費します。
        if (!startedReturnPresentation) {
            StartStageClearRewardPresentation(pendingStageClearRewardPresentation_);
            pendingStageClearRewardPresentation_ = {};
        }

        // 帰還・報酬のいずれも動いていない場合に限り、新しいゲートの解放演出を予約する。
        unlockingStageIndex_ =
            (gateReturnPresentationActive_ || crownCountPresentationActive_) ? -1 : FindPendingUnlockStage();
        // 解放演出の予約番号が有効なら、視点の対象と選択を同じゲートへ揃えます。
        if (unlockingStageIndex_ >= 0) {
            selectedStageIndex_ = unlockingStageIndex_;
            DebugConsole::GetInstance()->AddLog("Stage Select: crown unlock presentation start.");
        }

        ApplyStageGateStates();
        UpdateStageSelectDecorations(0.0f);
        ++loadingInitializePhase_;
        completeUnit();
        return true;
    }

    // 全段階を終えた後に呼ばれた場合は、追加ロードをせず完了を返します。
    default:
        return true;
    }
}

float GameSelectScene::GetLoadingInitializeProgress() const {
    // 段階ロード未開始時にも 0 除算せず完了値を返す。
    return loadingInitializeTotalUnits_ == 0 ? 1.0f
                                             : std::clamp(static_cast<float>(loadingInitializeCompletedUnits_) /
                                                              static_cast<float>(loadingInitializeTotalUnits_),
                                                          0.0f, 1.0f);
}

void GameSelectScene::OnActivated() {
    // 共有ライト状態は先読み中ではなく、ステージセレクトが有効になった時点で適用する。
    engine::LightManager::GetInstance()->LoadState(ResolveSceneLightPath("Resources/json/light/stageSelect.json"));
    BaseScene::OnActivated();
}

void GameSelectScene::Finalize() {
    // シーンをまたいで入場演出や画面の帯が残らないよう解除する。
    gateEntryPresentation_.Reset();
    ResetGateCinematicBars();
    GimmickStageGate::ResetSceneEntries(*this);
    // 共有マネージャーのシーン固有状態を消してから所有リソースを解放する。
    unlockPlayer_.Stop(false);
    unlockTimelineLoaded_ = false;
    MeshEffectManager::GetInstance()->Clear();
    VfxSequencer::ClearOneShots();
    GpuParticleManager::GetInstance()->ClearSceneRuntime();
    engine::CollisionManager::GetInstance()->ClearObjects();
    BulletManager::GetInstance()->Finalize();
    particleSystem_.reset();
    sprites_.clear();
    stageGateNameLabels_.clear();
    stageGateStarIcons_.clear();

    // 両オーバーレイに終了処理を通してから、シーンのオブジェクト管理を破棄する。
    // 設定ツールが作成済みなら、その終了処理を呼んでから破棄します。
    if (settingsOverlay_) {
        settingsOverlay_->Finalize();
        settingsOverlay_.reset();
    }

    // ポーズ画面が作成済みなら、同様に終了処理を済ませます。
    if (pauseMenuOverlay_) {
        pauseMenuOverlay_->Finalize();
        pauseMenuOverlay_.reset();
    }

    objectManager_.reset();
    lockOnSystem_.reset();
}

void GameSelectScene::Update(float deltaTime) {
    // 編集モードへ戻ったらゲート演出の対象と遷移予約を破棄し、帯表示を片付ける。
    if (gateEntryCinematicActive_ && !SceneManager::GetInstance()->IsPlaying()) {
        gateEntryPresentation_.Reset();
        gateEntryCinematicActive_ = false;
        gateEntryTargetGate_ = nullptr;
        isChangingStage_ = false;
        ResetGateCinematicBars();
    }
    // ゲート側が入場を進めるフレームは、ステージ選択の操作を重ねない。
    if (GimmickStageGate::UpdateSceneEntry(*this, deltaTime)) return;
    // ポーズ中はゲーム側の時計と衝突処理を進めず、オーバーレイだけ更新する。
    if (HandlePauseOverlay(deltaTime)) {
        return;
    }

    GameDataManager::GetInstance()->AdvancePlayTime(deltaTime);

    // 演出のポスト効果を進め、現在描画に使う視点があれば逆投影行列を反映します。
    PostEffect::GetInstance()->Update(deltaTime);
    engine::Camera* activeCamera = CameraManager::GetInstance()->GetActiveCamera();

    // アクティブカメラがある場合に、深度を復元するポスト効果の投影データを更新します。
    if (activeCamera) {
        PostEffect::GetInstance()->GetParams()->projectionInverse = engine::Math::Inverse(activeCamera->GetProjectionMatrix());
    }

    static engine::Math sMath;
    LightEditor::GetInstance()->Update();

    engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();

    // 通常カメラを更新する前に、現在のロックオン対象を評価します。
    lockOnSystem_->Update(objectManager_->GetObjects(), camera, player_, deltaTime);

    // 解錠演出が視点を決めている間は、通常の追従カメラへ更新を渡さない。
    if (unlockingStageIndex_ < 0) {
        CameraEditor::GetInstance()->Update(player_, lockOnSystem_->IsLockingOn());
    }

    // ロックオンアイコン更新
    engine::Object3D* target = lockOnSystem_->GetTarget();

    // 対象が取得でき、ロックオンが有効な場合に画面上の照準位置を求めます。
    if (target && lockOnSystem_->IsLockingOn()) {
        isDrawLockOn_ = true;
        engine::Aabb aabb = target->GetAabb();
        engine::Vector3 targetCenter = (aabb.min + aabb.max) * 0.5f;
        engine::Matrix4x4 viewProj = sMath.Multiply(camera->GetViewMatrix(), camera->GetProjectionMatrix());
        float w = targetCenter.x * viewProj.m[0][3] + targetCenter.y * viewProj.m[1][3] +
                  targetCenter.z * viewProj.m[2][3] + viewProj.m[3][3];

        // カメラの手前にある対象だけ透視除算し、背面や除算できない位置ではアイコンを隠す。
        if (w > 0.001f) {
            engine::Vector3 ndc;
            ndc.x = (targetCenter.x * viewProj.m[0][0] + targetCenter.y * viewProj.m[1][0] +
                     targetCenter.z * viewProj.m[2][0] + viewProj.m[3][0]) /
                    w;
            ndc.y = (targetCenter.x * viewProj.m[0][1] + targetCenter.y * viewProj.m[1][1] +
                     targetCenter.z * viewProj.m[2][1] + viewProj.m[3][1]) /
                    w;

            float screenWidth = static_cast<float>(::WinApp::GetClientWidth());
            float screenHeight = static_cast<float>(::WinApp::GetClientHeight());
            float screenX = (ndc.x + 1.0f) * 0.5f * screenWidth;
            float screenY = (1.0f - ndc.y) * 0.5f * screenHeight;

            lockOnSprite_->SetPosition({screenX, screenY});

            // 対象の大きさと奥行きからアイコンを拡縮し、近すぎても遠すぎても視認できる範囲に収める。
            float objSizeX = aabb.max.x - aabb.min.x;
            float objSizeY = aabb.max.y - aabb.min.y;
            float objSizeZ = aabb.max.z - aabb.min.z;
            float maxObjSize = std::max({objSizeX, objSizeY, objSizeZ});

            float baseSize = maxObjSize * 25.0f;
            float distanceScale = 20.0f / w;
            float finalSize = std::max(32.0f, std::min(baseSize * distanceScale, 256.0f));

            lockOnSprite_->SetSize({finalSize, finalSize});
            lockOnSprite_->SetRotation(lockOnSprite_->GetRotation() + 2.0f * deltaTime);
            lockOnSprite_->Update();
        // 透視除算の分母が閾値以下なら、手前に描けない対象として照準を隠します。
        } else {
            isDrawLockOn_ = false;
        }
    // ロック対象を取得できないか、ロックオンが解除された場合も照準を隠します。
    } else {
        isDrawLockOn_ = false;
    }

    // 自由視点で編集している間は、ゲーム側の視点入力を重ねません。
    if (!CameraEditor::GetInstance()->IsEditorMode()) {
        engine::Camera::FollowMode currentMode = camera->GetFollowMode();

        // 照準操作または一人称モードの場合だけ、設定の感度でプレイヤーのカメラ入力を受け取ります。
        if (currentMode == engine::Camera::FollowMode::kAimable || currentMode == engine::Camera::FollowMode::kFirstPerson) {
            const auto* settings = GameSettingsManager::GetInstance();
            camera->SetRotationSensitivity(settings->GetCameraSensitivity());
            const engine::Vector2 lookDelta = inputManager_->GetCameraLookDelta(deltaTime);
            // 実際に入力された軸に動きがある場合にだけ、回転量を加えます。
            if (lookDelta.x != 0.0f || lookDelta.y != 0.0f)
                camera->AddRotation(settings->ApplyCameraLookInversion(lookDelta));
        }
    }

    // 入場または帰還の演出が進行している場合に、その演出の時間と姿勢を先に更新します。
    if (gateEntryCinematicActive_) {
        UpdateGateEntryCinematic(deltaTime);
    }

    // ゲートからの帰還演出が進行中なら、通常カメラやワールドの更新前に時刻を進めます。
    if (gateReturnPresentationActive_) {
        UpdateStageReturnPresentation(deltaTime);
    }

    CameraManager::GetInstance()->Update(deltaTime);
    particleSystem_->Update(deltaTime);
    const bool hadUnlockPresentation = unlockingStageIndex_ >= 0;
    UpdateStageGateSelection(deltaTime);
    objectManager_->Update(deltaTime);

    // このフレームに解放演出を開始・継続・終了した場合は、後から確定した姿勢を描画用カメラへ反映します。
    if (hadUnlockPresentation || unlockingStageIndex_ >= 0) {
        // 解放演出の固定カメラは通常カメラ更新後に設定されるため、
        // 同じフレームでもう一度更新して描画へ確実に反映します。
        CameraManager::GetInstance()->Update(0.0f);
    }

    // ワールド更新後の戻り位置をカメラへ反映する。時刻は0秒更新で据え置き、演出を二度進めない。
    if (gateReturnPresentationActive_) {
        UpdateStageReturnPresentation(0.0f);
        CameraManager::GetInstance()->Update(0.0f);
    }

    GpuParticleManager::GetInstance()->Update(deltaTime);

    for (auto& sprite : sprites_)
        sprite->Update();
    BulletManager::GetInstance()->Update(deltaTime);
    engine::CollisionManager::GetInstance()->Update();
    UpdateUi();
    UpdateStageGateNameLabels();
    UpdateGateCinematicBars(deltaTime);

    // アニメーションの確認用キューブが存在するときだけ、その時間を進めます。
    if (animatedCube_)
        animatedCube_->Update(deltaTime);
}

bool GameSelectScene::HandlePauseOverlay(float deltaTime) {
    // 解放・入場演出中にはポーズを開かず、ImGui の文字入力も奪わない。
    // 設定画面が開いている間は設定を更新し、通常のゲーム更新を抑止します。
    if (settingsOverlay_ && settingsOverlay_->IsActive()) {
        settingsOverlay_->Update(deltaTime);
        return true;
    }

    // ポーズ画面が開いている間は、その操作の結果だけを処理します。
    if (pauseMenuOverlay_ && pauseMenuOverlay_->IsActive()) {
        const auto action = pauseMenuOverlay_->Update(deltaTime);

        // 設定とタイトルへの操作は共有し、背面のステージ選択には入力を渡さない。
        if (HandleSharedPauseAction(action, *pauseMenuOverlay_, settingsOverlay_.get())) {
            return true;
        }

        // 選択画面から再開する場合は、ポーズを閉じて次のフレームに操作を戻す。
        if (action == PauseMenuOverlay::Action::kResume) {
            pauseMenuOverlay_->SetActive(false);
        }

        return true;
    }

    // 入力を捕捉できない場合やステージ遷移・各種演出が進行中の場合は、新規ポーズを受け付けません。
    if (!inputManager_ || isChangingStage_ || gateEntryCinematicActive_ || gateReturnPresentationActive_ ||
        crownCountPresentationActive_ || unlockingStageIndex_ >= 0) {
        return false;
    }
#ifdef USE_IMGUI
    ImGuiIO& io = ImGui::GetIO();

    // ImGuiがキーを捕捉しているか文字入力中なら、同じ入力でポーズを開きません。
    if (io.WantCaptureKeyboard || io.WantTextInput) {
        return false;
    }
#endif
    // キーボードのTabかゲームパッドのStartが押されたフレームにポーズを開きます。
    if (inputManager_->IsKeyTriggered(DIK_TAB) ||
        inputManager_->IsGamepadButtonTriggered(XINPUT_GAMEPAD_START)) {
        pauseMenuOverlay_->SetActive(true);
        return true;
    }

    return false;
}

void GameSelectScene::Draw() {
    // ゲートとプレイヤーを通常の3Dパスに描き、半透明・霧・特殊材質を後段へ回します。
    // 各パスで近距離のプレイヤーと子パーツを同じ条件で除外します。
    engine::Camera* camera = CameraManager::GetInstance()->GetActiveCamera();
    const bool isFirstPerson = IsCameraNearViewActor(camera, player_);

    // この描画で使う点光源とスポットライトを一度借り、後続の描画に渡す。
    const auto lighting = engine::SceneLighting::Capture();

    BeginWorldObjectDrawing(skybox_.get(), camera);

    auto& objects = objectManager_->GetObjects();

    // 背景の後で不透明物の深度を先に決め、霧と特殊材質はここでは描きません。
    DrawSceneObjectPass(objects, materialRenderPolicy::Pass::kOpaque, lighting,
        player_, isFirstPerson, true, [](engine::Object3D* object) { return object->GetIsVisible(); });

    // 確認用のアニメーション実体がある場合だけ、同じライトで描きます。
    if (animatedCube_) {
        lighting.Draw(*animatedCube_, &engine::Object3D::Draw);
    }

    // ゲート等の不透明物が済んでから、弾・予兆・編集用3Dガイドを描きます。
    DrawEnemyTelegraphs(*this, lighting);
    lighting.Draw(*BulletManager::GetInstance(), &BulletManager::Draw);
    LightEditor::GetInstance()->Draw3D();

    // 半透明と通常粒子は不透明物の後に重ねます。
    DrawSceneObjectPass(objects, materialRenderPolicy::Pass::kTransparent, lighting,
        player_, isFirstPerson, true, [](engine::Object3D* object) { return object->GetIsVisible(); });
    particleSystem_->Draw();

    // 霧の後、GrabTextureと深度を使う材質を描きます。GPU粒子はその画面コピーを再利用します。
    DrawLocalFogObjects(objects, player_, isFirstPerson);

    const bool grabUpdated =
        DrawSpecialMaterialObjects(objects, BulletManager::GetInstance(), player_, isFirstPerson);

    DrawGpuParticles(camera, gpuParticleTexHandle_, grabUpdated);
}

void GameSelectScene::DrawUi() {
    // ゲートの名札と収集済みスターは、3Dの投影後に画面座標へ重ねます。
    // 操作Spriteと照準の後、設定画面をポーズより前面に出します。
    BeginSpriteDrawing();

    // ゲート入退場と解放演出は、黒帯が閉じ終わるまで通常の案内を隠します。
    if (gateCinematicBarOverrideActive_) {
        return;
    }

    for (auto& sprite : sprites_)
        sprite->Draw();
    for (auto& label : stageGateNameLabels_) {
        // JSON に名札のないステージのスロットは空なので、描画対象から除きます。
        if (label) {
            label->Draw();
        }
    }

    for (auto& stars : stageGateStarIcons_) {
        for (auto& star : stars) {
            // 画像を用意した星だけを描き、未配置のスロットを飛ばします。
            if (star) {
                star->Draw();
            }
        }
    }

    // 照準を描く条件を満たし、照準の画像が用意されている場合に重ねます。
    if (isDrawLockOn_ && lockOnSprite_)
        lockOnSprite_->Draw();
    // 設定画面が開いていれば、ポーズより前にそちらの表示を優先します。
    if (settingsOverlay_ && settingsOverlay_->IsActive()) {
        settingsOverlay_->Draw();
    // 設定画面が閉じていてポーズが開いている場合に、ポーズ画面を描きます。
    } else if (pauseMenuOverlay_ && pauseMenuOverlay_->IsActive()) {
        pauseMenuOverlay_->Draw();
    }
}

void GameSelectScene::DrawShadow() {
    // 配置オブジェクトのうち、影を落とす対象の選別は ObjectManager に委ねる。
    // オブジェクト管理を使用できる場合に、影を描く対象の選別を委ねます。
    if (objectManager_)
        objectManager_->DrawShadow();
}

void GameSelectScene::DrawImGui() {
    // 保存を変えずにゲート入場・解放演出を試せる調整 UI を提供する。
#ifdef USE_IMGUI
    ImGui::Text("ステージセレクト設定");
    ImGui::Separator();

    // 名札はその場で調整し、保存ボタンを押したときだけ設定ファイルへ書き出します。
    if (ImGui::CollapsingHeader("ゲート名札")) {
        ImGui::DragFloat("完全表示する距離 (m)", &gateNameShowDistance_, 0.5f, 5.0f, 150.0f);
        ImGui::DragFloat("フェード幅 (m)", &gateNameFadeDistance_, 0.5f, 1.0f, 40.0f);
        ImGui::DragFloat("ゲート上の余白 (m)", &gateNameHeightOffset_, 0.1f, 0.0f, 10.0f);
        ImGui::TextDisabled("近くでも表示し、指定距離より遠くでフェードアウトします。");
        // 保存ボタンを押したときにだけ現在の名札設定を書き出し、成否を表示します。
        if (ImGui::Button("名札の表示設定を保存")) {
            DebugConsole::GetInstance()->AddLog(SaveStageGateNameSettings()
                ? "ゲート名札の表示設定を保存しました。" : "ゲート名札の表示設定を保存できませんでした。");
        }
    }

    // ゲート演出のデバッグ欄を展開した場合に、最寄りのゲートと試せる操作を表示します。
    if (ImGui::CollapsingHeader("ゲート演出デバッグ", ImGuiTreeNodeFlags_DefaultOpen)) {
        // 現在のプレイヤー位置から突入先を選び、他の演出と競合しないか確認します。
        float nearestDistance = std::numeric_limits<float>::max();
        engine::Object3D* nearestGate = FindNearestStageGate(&nearestDistance);
        const std::int32_t nearestGateStageIndex = GetStageGateIndex(nearestGate);
        const auto& stages = StageManager::GetInstance()->GetStages();
        const bool hasValidGate = player_ && nearestGate && nearestGateStageIndex >= 0 &&
                                  nearestGateStageIndex < static_cast<std::int32_t>(stages.size());
        const bool canPlayDebug = hasValidGate && !gateEntryCinematicActive_ && !gateReturnPresentationActive_ &&
                                  !crownCountPresentationActive_ && unlockingStageIndex_ < 0;

        // キーとボタンを同じ操作へ集約し、通常の接触判定を通さず演出だけ試せるようにする。
        if (hasValidGate) {
            ImGui::Text("対象ゲート: Stage %d", nearestGateStageIndex + 1);
            ImGui::Text("プレイヤーからの距離: %.2f", nearestDistance);
            ImGui::TextDisabled("通常の接触判定を使わず、同じゲート突入演出を強制再生します。");
        // 有効なゲートが見つからない場合は、強制再生の対象が無いことを案内します。
        } else {
            ImGui::TextDisabled("対象ゲート: なし");
            ImGui::TextDisabled("プレイヤーまたはステージゲートが見つからないため再生できません。");
        }

        ImGui::TextDisabled("ショートカット: G キー");

        const auto playDebugGateEntry = [&]() {
            selectedStageIndex_ = nearestGateStageIndex;
            StageManager::GetInstance()->SetCurrentStage(selectedStageIndex_);
            DebugConsole::GetInstance()->AddLog("Stage Select: debug gate entry cinematic.");
            StartGateEntryCinematic(selectedStageIndex_);
        };

        // テキスト入力中の G キーを、突入ショートカットとして拾わないようにします。
        const bool shortcutPressed =
            canPlayDebug && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_G, false);

        // 入力条件を満たしたGキーなら、ボタンと同じ演出開始処理を呼びます。
        if (shortcutPressed) {
            playDebugGateEntry();
        }

        // 対象が不適切か別演出中なら、ボタンを表示したまま入力だけ止めます。
        if (!canPlayDebug) {
            ImGui::BeginDisabled();
        }

        // 強制再生ボタンが押された場合は、選択を最寄りゲートへ移してから演出を始めます。
        if (ImGui::Button("最寄りゲートで突入演出を強制再生", ImVec2(-1.0f, 34.0f))) {
            playDebugGateEntry();
        }

        // ボタンを無効化した場合にだけ、その無効化スコープを閉じます。
        if (!canPlayDebug) {
            ImGui::EndDisabled();
        }

        // 入場演出がアクティブなら、専用演出の再生中表示を出します。
        if (gateEntryCinematicActive_) {
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.25f, 1.0f), "ゲート突入演出を再生中です。");
        }
    }

    // 王冠鍵のデバッグ欄が開いている場合だけ、解放用の選択と再生操作を表示します。
    if (ImGui::CollapsingHeader("王冠鍵 解放ムービーデバッグ", ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto& stages = StageManager::GetInstance()->GetStages();
        // 最初から入れるステージを候補から外し、選択をステージ一覧の範囲に収めます。
        static std::int32_t sDebugUnlockStageIndex = 1;
        const std::int32_t maxUnlockStageIndex = static_cast<std::int32_t>(stages.size()) - 1;
        sDebugUnlockStageIndex = std::clamp(sDebugUnlockStageIndex, 1, std::max(maxUnlockStageIndex, 1));

        // 2面目以降のステージが存在する場合に、解放演出の候補を表示します。
        if (maxUnlockStageIndex >= 1) {
            const std::string previewLabel = "Stage " + std::to_string(sDebugUnlockStageIndex + 1);

            // コンボを展開したときに、現行のステージ一覧から解放対象を選べるようにします。
            if (ImGui::BeginCombo("再生対象", previewLabel.c_str())) {
                for (std::int32_t stageIndex = 1; stageIndex <= maxUnlockStageIndex; ++stageIndex) {
                    const bool selected = stageIndex == sDebugUnlockStageIndex;
                    const std::string label = "Stage " + std::to_string(stageIndex + 1);

                    // 候補が選択されたら、次のプレビューで使うステージ番号を差し替えます。
                    if (ImGui::Selectable(label.c_str(), selected)) {
                        sDebugUnlockStageIndex = stageIndex;
                    }

                    // すでに選ばれている候補を、コンボを開いたときの既定のフォーカス先にします。
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }

                ImGui::EndCombo();
            }
        }

        // 解放プレビューにはゲート本体、鍵、南京錠の三つが必要です。
        engine::Object3D* previewGate = maxUnlockStageIndex >= 1 ? FindStageGateByIndex(sDebugUnlockStageIndex) : nullptr;
        engine::Object3D* previewKey =
            maxUnlockStageIndex >= 1
                ? FindObjectByName(std::string(kStageGateCrownKeyPrefix) + std::to_string(sDebugUnlockStageIndex))
                : nullptr;
        engine::Object3D* previewLock =
            maxUnlockStageIndex >= 1
                ? FindObjectByName(std::string(kStageGateLockPrefix) + std::to_string(sDebugUnlockStageIndex))
                : nullptr;
        const bool anotherCinematicActive = gateEntryCinematicActive_ || gateReturnPresentationActive_ ||
                                            crownCountPresentationActive_ || unlockingStageIndex_ >= 0;
        const bool canPlayUnlockPreview =
            maxUnlockStageIndex >= 1 && previewGate && previewKey && previewLock && !anotherCinematicActive;

        ImGui::TextDisabled("セーブデータを変更せず、南京錠と王冠鍵を一時表示して再生します。");

        // ゲート本体・鍵・南京錠のいずれかが無い場合に、モデル不足を表示します。
        if (!previewGate || !previewKey || !previewLock) {
            ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.32f, 1.0f),
                               "対象ゲートの鍵・南京錠・ゲート本体が不足しています。");
        }

        // 演出と競合するか必須の実体が揃っていない場合は、再生を無効化します。
        if (!canPlayUnlockPreview) {
            ImGui::BeginDisabled();
        }

        // 専用の再生入口を使い、進行データを更新せずに解放演出を確認します。
        if (ImGui::Button("王冠鍵の解放ムービーを再生", ImVec2(-1.0f, 38.0f))) {
            StartDebugUnlockPresentation(sDebugUnlockStageIndex);
        }

        // 再生ボタンを無効化した場合にだけ、そのスコープを閉じます。
        if (!canPlayUnlockPreview) {
            ImGui::EndDisabled();
        }

        // デバッグ再生として進行中の場合、セーブを伴う通常演出とは別に状態を表示します。
        if (unlockPresentationDebugReplay_) {
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.28f, 1.0f), "王冠鍵の解放ムービーを再生中です。");
        }
    }
#endif
}

void GameSelectScene::UpdateStageGateSelection(float deltaTime) {
    // 解放・帰還・入場の演出中はゲート接触による別ステージへの遷移を止める。
    stageSelectTime_ += deltaTime;

    // 入場の再受付待ちが残っている場合に、フレームの経過秒を差し引きます。
    if (stageDecisionCooldown_ > 0.0f) {
        stageDecisionCooldown_ -= deltaTime;
        // 差し引いた値が負になったらゼロに揃え、以降は受付可能とします。
        if (stageDecisionCooldown_ < 0.0f)
            stageDecisionCooldown_ = 0.0f;
    }

    // 接触を調べる前に、報酬・解放演出の終了や次の開始を処理する。
    UpdateStageClearRewardPresentation(deltaTime);
    UpdateUnlockPresentation(deltaTime);

    const bool blocksGateSelection = gateEntryCinematicActive_ || gateReturnPresentationActive_ ||
                                     crownCountPresentationActive_ || unlockingStageIndex_ >= 0;

    engine::Object3D* entryGate = nullptr;
    // 演出中は新しい入口を選び直さず、演出が指定したゲートの見た目を維持する。
    if (!blocksGateSelection) {
        entryGate = UpdateNearestGateSelection();
    }

    ApplyStageGateStates();
    UpdateStageSelectDecorations(deltaTime);

    // 演出中も表示は更新するが、選択ログと新しい入場要求は発行しない。
    if (blocksGateSelection) {
        return;
    }

    // 同じゲートに近付いたままのフレームで、選択ログを繰り返さない。
    if (selectedStageIndex_ != previousSelectedStageIndex_) {
        const auto& stages = StageManager::GetInstance()->GetStages();

        // 選択先がステージ一覧にある場合だけ、名前をログへ記録する。
        if (selectedStageIndex_ >= 0 && selectedStageIndex_ < static_cast<std::int32_t>(stages.size())) {
            const StageData& stage = stages[selectedStageIndex_];
            DebugConsole::GetInstance()->AddLog("Stage Select: " + stage.name);
        }

        previousSelectedStageIndex_ = selectedStageIndex_;
    }

    // 候補に選ばれただけでは入場せず、実際に門の薄い判定面へ触れたときだけ決定する。
    if (entryGate && IsPlayerTouchingStageGate(entryGate)) {
        EnterSelectedStage();
    }
}

engine::Object3D* GameSelectScene::UpdateNearestGateSelection() {
    float nearestDistance = std::numeric_limits<float>::max();
    engine::Object3D* nearestGate = FindNearestStageGate(&nearestDistance);

    // 選択半径の外からは選択中のステージを切り替えない。
    if (!nearestGate || !(nearestDistance <= gateSelectRadius_)) {
        return nullptr;
    }

    const std::int32_t stageIndex = GetStageGateIndex(nearestGate);

    // 有効なステージ番号を持つゲートだけを選択へ反映する。
    if (stageIndex < 0 || stageIndex >= static_cast<std::int32_t>(StageManager::GetInstance()->GetStages().size())) {
        return nullptr;
    }

    // 閉じたゲートも表示上は選択できるが、入場判定へは解放済みのゲートだけを渡す。
    selectedStageIndex_ = stageIndex;
    return IsStageUnlocked(stageIndex) ? nearestGate : nullptr;
}

void GameSelectScene::ApplyStageGateStates() {
    // セーブの解放状態と演出進度を分離し、解放演出が終わるまで閉じた見た目を保つ。
    if (!objectManager_) {
        return;
    }

    // チュートリアルのクリア記録は通常ステージと別の -1。入口も同じ青い状態へ反映します。
    if (auto* tutorialGate = dynamic_cast<GimmickStageGate*>(FindObjectByName("TutorialArrivalGate"))) {
        const bool tutorialCleared = GameDataManager::GetInstance()->IsStageCleared(-1);
        tutorialGate->SetGateState(false, true, tutorialCleared);
        tutorialGate->SetGateActivation(1.0f);
    }

    float pulse = (std::sin(stageSelectTime_ * 5.0f) + 1.0f) * 0.5f;

    for (auto& object : objectManager_->GetObjects()) {
        // 無効な参照やゲート以外の配置には、ステージの表示と判定を適用しません。
        if (!object || !IsStageGateObject(object.get())) {
            continue;
        }

        auto* gate = dynamic_cast<GimmickStageGate*>(object.get());
        std::int32_t stageIndex = GetStageGateIndex(object.get());

        // 番号がステージ定義の範囲外なら、進行データに対応付けられないため飛ばします。
        if (stageIndex < 0 || stageIndex >= static_cast<std::int32_t>(StageManager::GetInstance()->GetStages().size())) {
            continue;
        }

        bool unlocked = IsStageUnlocked(stageIndex);
        bool cleared = GameDataManager::GetInstance()->IsStageCleared(stageIndex);
        bool unlocking = stageIndex == unlockingStageIndex_;
        bool selected = stageIndex == selectedStageIndex_;
        const bool unlockTimelineReady = unlocking && unlockPresentationInitialized_;
        const float gateUnlockT =
            unlockTimelineReady ? SelectEaseInOutCubic(SelectClamp01(
                                      (unlockTime_.Elapsed() - unlockGateActivationTime_) /
                                      std::max(unlockGateActivationEndTime_ - unlockGateActivationTime_, 0.001f)))
                                : 0.0f;

        // カメラが正面へ到着するまでは、解放済みのセーブデータであっても閉じたゲートとして描画します。
        const bool gateVisualUnlocked =
            unlocking ? (unlockTimelineReady && unlockTime_.Elapsed() >= unlockGateActivationTime_) : unlocked;
        // 現在のステージ番号に対応する南京錠が配置されている場合に、その表示・衝突を整えます。
        if (engine::Object3D* lockSeal = FindObjectByName(std::string(kStageGateLockPrefix) + std::to_string(stageIndex))) {
            // 演出中は Director が表示と発光を所有し、通常の選択表示で上書きしない。
            if (!unlockTimelineReady) {
                lockSeal->SetIsVisible(unlocking || !unlocked);
                lockSeal->SetColor(selected ? engine::Vector4{0.86f, 0.68f, 0.34f, 1.0f}
                                            : engine::Vector4{0.64f, 0.52f, 0.30f, 1.0f});
                lockSeal->SetEmissive(selected ? 0.42f : 0.20f);
            }
            lockSeal->SetCollisionAttribute(0);
            lockSeal->SetCollisionMask(0);
            lockSeal->SetColliderType(engine::ColliderType::kNone);
        }

        // 鍵の実体がある場合に、タイムライン以外の通常表示では隠すよう設定します。
        if (engine::Object3D* crownKey = FindObjectByName(std::string(kStageGateCrownKeyPrefix) + std::to_string(stageIndex))) {
            // 解放のタイムラインが表示を制御していない場合は鍵を非表示にします。
            if (!unlockTimelineReady) {
                crownKey->SetIsVisible(false);
            }
            crownKey->SetCollisionAttribute(0);
            crownKey->SetCollisionMask(0);
            crownKey->SetColliderType(engine::ColliderType::kNone);
        }

        float gateActivation = unlocking ? 1.0f : 0.0f;

        // プレイヤーがいる場合は、水平距離から通常時のゲートの発光量を求めます。
        if (player_) {
            const engine::Vector3 playerPos = player_->GetWorldPosition();
            const engine::Vector3 gatePos = object->GetWorldPosition();
            const float dx = gatePos.x - playerPos.x;
            const float dz = gatePos.z - playerPos.z;
            const float distance = std::sqrt(dx * dx + dz * dz);
            const float nearRadius = gateSelectRadius_ * 0.55f;
            const float wakeRadius = gateSelectRadius_ * 1.55f;
            gateActivation =
                1.0f - engine::Math::Clamp((distance - nearRadius) / std::max(wakeRadius - nearRadius, 0.001f), 0.0f, 1.0f);
            gateActivation = Easing::SmoothStep(gateActivation);

            // 未解放の場合は、近付いたときの発光を弱めます。
            if (!unlocked) {
                gateActivation *= 0.18f;
            }
        }

        // 現在の入場または帰還演出が指定した門は、距離の結果にかかわらず最大の活性にします。
        if ((gateEntryCinematicActive_ && object.get() == gateEntryTargetGate_) ||
            (gateReturnPresentationActive_ && object.get() == gateReturnTargetGate_)) {
            gateActivation = 1.0f;
            selected = true;
        }

        // 解放演出の対象ゲートでは、距離の値をタイムラインの進行率で上書きします。
        if (unlocking) {
            gateActivation = gateUnlockT;
            selected = true;
        }

        // 専用クラスのゲートには、種類ごとの表示とコライダー制御を任せます。
        if (gate) {
            // GimmickStageGateのunlocking指定は即座に最大発光するため、演出では段階的なactivationを使います。
            gate->SetGateState(selected, gateVisualUnlocked, cleared, false);
            gate->SetGateActivation(gateActivation);
            continue;
        }

        // 汎用の旧ゲートは、解放状態で接触カテゴリを切り替え、表示色と発光を直接更新します。
        object->SetIsVisible(true);
        object->SetCollisionAttribute(gateVisualUnlocked ? engine::collisionCategories::Get("trigger") : 0);
        object->SetCollisionMask(gateVisualUnlocked ? engine::collisionCategories::Get("player") : 0);
        object->SetColliderType(engine::ColliderType::kSphere);
        object->SetCollisionRadius(gateSelectRadius_ * 0.55f);
        object->SetColor(GetStageGateModelColor(gateVisualUnlocked, selected, cleared, false, pulse));
        object->SetEmissive(GetStageGateModelEmissive(gateVisualUnlocked, selected, cleared, false, pulse));
    }
}

void GameSelectScene::RefreshDebugStageStates() {
    // デバッグで進行を変更したら、途中演出のキー・カメラ・操作権を元に戻す。
    const auto& stages = StageManager::GetInstance()->GetStages();

    // ステージ定義が空なら、選択や演出を再構成する対象がありません。
    if (stages.empty()) {
        return;
    }

    // デバッグ操作で進行状態を変更した場合も、途中の解放演出を確実に片付けます。
    if (unlockPresentationInitialized_) {
        RestoreUnlockPresentationState();
    }

    // デバッグ再生を開始する前の有効な選択を控えている場合は、その選択へ戻します。
    if (unlockPresentationDebugReplay_ && unlockDebugPreviousSelectedStageIndex_ >= 0) {
        selectedStageIndex_ = unlockDebugPreviousSelectedStageIndex_;
    }

    unlockPresentationInitialized_ = false;
    unlockPresentationDebugReplay_ = false;
    unlockDebugPreviousSelectedStageIndex_ = -1;
    unlockTargetGate_ = nullptr;
    unlockKeyObject_ = nullptr;
    unlockLockObject_ = nullptr;

    // 選択が範囲外か現在は未解放なら、解放済みの先頭候補を改めて探します。
    if (selectedStageIndex_ < 0 || selectedStageIndex_ >= static_cast<std::int32_t>(stages.size()) ||
        !IsStageUnlocked(selectedStageIndex_)) {
        selectedStageIndex_ = 0;

        for (std::int32_t stageIndex = 0; stageIndex < static_cast<std::int32_t>(stages.size()); ++stageIndex) {
            // 解放済みのステージに最初に出会った時点で、選択先を決めます。
            if (IsStageUnlocked(stageIndex)) {
                selectedStageIndex_ = stageIndex;
                break;
            }
        }
    }

    previousSelectedStageIndex_ = -1;
    unlockingStageIndex_ = FindPendingUnlockStage();
    unlockTime_ = {};
    ApplyStageGateStates();
    UpdateStageSelectDecorations(0.0f);
}

bool GameSelectScene::IsStageUnlocked(std::int32_t stageIndex) const {
    // 初期解放か、前提ステージのクリア記録がある時だけ入場を許可する。
    const auto& stages = StageManager::GetInstance()->GetStages();

    // ステージ番号が保存対象の範囲外なら、入場を許可しません。
    if (stageIndex < 0 || stageIndex >= static_cast<std::int32_t>(stages.size())) {
        return false;
    }

    const StageData& stage = stages[stageIndex];

    // ステージ0または既定で解放される設定のステージは、クリア条件を要求しません。
    if (stage.defaultUnlocked || stageIndex == 0) {
        return true;
    }

    // 初期解放されず前提ステージの指定も無ければ、未解放のままです。
    if (stage.unlockStageIndex < 0) {
        return false;
    }

    return GameDataManager::GetInstance()->IsStageCleared(stage.unlockStageIndex);
}

std::int32_t GameSelectScene::FindPendingUnlockStage() const {
    // セーブ上は解放済みでも、解放演出をまだ見ていない最初のゲートを探す。
    const auto& stages = StageManager::GetInstance()->GetStages();
    auto* save = GameDataManager::GetInstance();

    for (std::int32_t stageIndex = 1; stageIndex < static_cast<std::int32_t>(stages.size()); ++stageIndex) {
        // 解放条件を満たし未視聴の最初のステージを、演出の候補にします。
        if (IsStageUnlocked(stageIndex) && !save->IsStageUnlockSeen(stageIndex)) {
            return stageIndex;
        }
    }

    return -1;
}

engine::Object3D* GameSelectScene::FindObjectByName(const std::string& name) const {
    // 配置 JSON で名前を変更した演出部品は見つからないものとして扱う。
    // シーンの配置を管理する一覧が無い場合は名前で探せません。
    if (!objectManager_) {
        return nullptr;
    }

    for (const auto& object : objectManager_->GetObjects()) {
        // 指定の名前と一致した有効なオブジェクトを見つけたら、その実体を返します。
        if (object && object->GetName() == name) {
            return object.get();
        }
    }

    return nullptr;
}

engine::Vector3 GameSelectScene::GetStageNodePosition(std::int32_t stageIndex) const {
    // 実際に配置したゲート位置を優先し、未配置時だけ旧レイアウトの座標を使う。
    if (objectManager_) {
        for (const auto& object : objectManager_->GetObjects()) {
            // ステージ番号が一致する有効なゲートの実位置を優先して返します。
            if (object && IsStageGateObject(object.get()) && GetStageGateIndex(object.get()) == stageIndex) {
                return object->GetWorldPosition();
            }
        }
    }

    static const engine::Vector3 kFallbackPositions[] = {
        {-8.0f, 1.4f, 3.0f},
        {0.0f, 1.4f, -4.2f},
        {8.0f, 1.4f, 3.0f},
    };

    constexpr std::int32_t kFallbackCount = static_cast<std::int32_t>(sizeof(kFallbackPositions) / sizeof(kFallbackPositions[0]));

    // 固定の代替座標が定義された番号であれば、その位置を使います。
    if (stageIndex >= 0 && stageIndex < kFallbackCount) {
        return kFallbackPositions[stageIndex];
    }

    return {0.0f, 1.4f, 0.0f};
}

void GameSelectScene::EnterSelectedStage() {
    // 二重入場を防ぎ、進行記録と選択ステージを確定してから入場演出を始める。
    if (isChangingStage_ || stageDecisionCooldown_ > 0.0f) {
        return;
    }

    const auto& stages = StageManager::GetInstance()->GetStages();

    // ステージ番号が一覧外なら、遷移先を指定できないため入場しません。
    if (selectedStageIndex_ < 0 || selectedStageIndex_ >= static_cast<std::int32_t>(stages.size())) {
        return;
    }

    const StageData& stage = stages[selectedStageIndex_];

    // 保存上まだ解放されていない場合は入場せず、短い再判定待ちを入れてログの連続表示を抑えます。
    if (!IsStageUnlocked(selectedStageIndex_)) {
        DebugConsole::GetInstance()->AddLog("Stage Select: " + stage.name + " is locked.");
        stageDecisionCooldown_ = 0.35f;
        return;
    }

    StageManager::GetInstance()->SetCurrentStage(selectedStageIndex_);
    DebugConsole::GetInstance()->AddLog("Stage Select: enter " + stage.name);
    StartGateEntryCinematic(selectedStageIndex_);
}

bool GameSelectScene::IsStageGateObject(const engine::Object3D* object) const {
    // 新しい Gimmick 型と旧レイアウトの名前・モデル名の両方をゲートとして認識する。
    // 実体が無い場合はゲートとみなしません。
    if (!object) {
        return false;
    }

    // 専用のゲート型の場合は、ステージセレクトでの入口モードかを調べます。
    if (const auto* gate = dynamic_cast<const GimmickStageGate*>(object)) {
        return gate->IsStageSelectNodeMode();
    }

    // 旧配置でギミック種がStageGateなら、選択用の動作モードか判定します。
    if (object->GetGimmickType() == "StageGate") {
        // 旧データの個体パラメーターが無ければ、選択用のゲートとして扱います。
        if (!object->GetParameters().has_value()) {
            return true;
        }

        return object->GetParameters()->actionMode == 0;
    }

    const std::string& modelName = object->GetModelName();
    const std::string& objectName = object->GetName();
    return modelName.find("portal_gate") != std::string::npos ||
           modelName.find("portal_surface") != std::string::npos ||
           objectName.find("portal_gate") != std::string::npos ||
           objectName.find("portal_surface") != std::string::npos || objectName.find("StageGate") != std::string::npos;
}


std::int32_t GameSelectScene::GetStageGateIndex(const engine::Object3D* object) const {
    // 新型は専用フィールド、旧型は汎用 TargetID からステージ番号を読む。
    // 参照が無効なら、選択可能なステージ番号が無いことを -1 で返します。
    if (!object) {
        return -1;
    }

    // 専用型の実体であれば、通常のイベント対象番号ではなくステージ専用番号を返します。
    if (const auto* gate = dynamic_cast<const GimmickStageGate*>(object)) {
        return gate->GetStageIndex();
    }

    return object->GetTargetId();
}

engine::Object3D* GameSelectScene::FindStageGateByIndex(std::int32_t stageIndex) const {
    // 解放演出などが対象とするステージ番号のゲートを探す。
    // 配置の管理一覧が無ければ、ステージ番号から実体を特定できません。
    if (!objectManager_) {
        return nullptr;
    }

    for (const auto& object : objectManager_->GetObjects()) {
        // 配置が無いかゲート以外なら、ステージ番号を調べません。
        if (!object || !IsStageGateObject(object.get())) {
            continue;
        }

        // 要求番号に対応するゲートを見つけた時点で、そのポインターを返します。
        if (GetStageGateIndex(object.get()) == stageIndex) {
            return object.get();
        }
    }

    return nullptr;
}

engine::Object3D* GameSelectScene::FindNearestStageGate(float* outDistance) const {
    // 高低差ではなく水平距離で選び、画面上の近いゲートと選択を合わせる。
    // 距離の出力先が渡された場合は、見つからなかったときに使う最大値で初期化します。
    if (outDistance) {
        *outDistance = std::numeric_limits<float>::max();
    }

    // 配置の管理かプレイヤーが無ければ、距離を比べるための情報がありません。
    if (!objectManager_ || !player_) {
        return nullptr;
    }

    const engine::Vector3 playerPos = player_->GetWorldPosition();
    engine::Object3D* nearestGate = nullptr;
    float nearestDistanceSq = std::numeric_limits<float>::max();

    for (const auto& object : objectManager_->GetObjects()) {
        // 有効な選択用ゲート以外は距離の比較から外します。
        if (!object || !IsStageGateObject(object.get())) {
            continue;
        }

        // 有効なステージを持たない負の番号は、最近傍の候補にしません。
        if (GetStageGateIndex(object.get()) < 0) {
            continue;
        }

        const engine::Vector3 gatePos = object->GetWorldPosition();
        float dx = gatePos.x - playerPos.x;
        float dz = gatePos.z - playerPos.z;
        float distanceSq = dx * dx + dz * dz;

        // 既知の最小水平距離より近い場合にだけ、最近傍を差し替えます。
        if (distanceSq < nearestDistanceSq) {
            nearestDistanceSq = distanceSq;
            nearestGate = object.get();
        }
    }

    // 距離を要求され、最近傍が見つかった場合に、二乗値からワールド単位の距離を返します。
    if (outDistance && nearestGate) {
        *outDistance = std::sqrt(nearestDistanceSq);
    }

    return nearestGate;
}

bool GameSelectScene::IsPlayerTouchingStageGate(engine::Object3D* gate) const {
    // 門の向きに沿う薄い平面判定を使い、旧配置の X/Z 軸向きも許容する。
    // ゲートかプレイヤーが存在しなければ、接触の計算を行いません。
    if (!gate || !player_) {
        return false;
    }

    const engine::Vector3 gatePos = gate->GetWorldPosition();
    const engine::Vector3 playerPos = player_->GetWorldPosition();
    const engine::Vector3 rel = {playerPos.x - gatePos.x, 0.0f, playerPos.z - gatePos.z};
    const float distanceSq = rel.x * rel.x + rel.z * rel.z;

    // 門の横幅と厚みをスケールから求め、極端に小さい・大きい配置も上下限で抑える。
    const engine::Vector3 gateScale = gate->GetScale();
    const float gateSize = std::max(1.0f, std::max(std::fabs(gateScale.x), std::fabs(gateScale.z)));
    const float halfWidth = std::clamp(gateSize * 0.36f, kGateEntryMinHalfWidth, kGateEntryMaxHalfWidth);
    const float planeThickness =
        std::clamp(gateSize * 0.045f, kGateEntryMinPlaneThickness, kGateEntryMaxPlaneThickness) +
        kGateEntryPlayerTouchRadius;

    float yaw = 0.0f;

    // 配置のTransformがあれば門のY回転を使い、無ければ正面をワールド軸に合わせます。
    if (const Transform* transform = gate->GetTransform()) {
        yaw = transform->rotate.y;
    }

    // 相対位置を門の正面と横へ射影し、それぞれ厚みと半幅の範囲に入るか判定する。
    const engine::Vector3 forward = {std::sin(yaw), 0.0f, std::cos(yaw)};
    const engine::Vector3 right = {std::cos(yaw), 0.0f, -std::sin(yaw)};
    const float planeDistance = rel.x * forward.x + rel.z * forward.z;
    const float sideDistance = rel.x * right.x + rel.z * right.z;
    const bool yawPlaneTouch = std::fabs(planeDistance) <= planeThickness && std::fabs(sideDistance) <= halfWidth;

    // 軸の向きを異なる規約で保存していた旧配置も、X/Z の両方を試して受け付ける。
    const bool xPlaneTouch = std::fabs(rel.x) <= planeThickness && std::fabs(rel.z) <= halfWidth;
    const bool zPlaneTouch = std::fabs(rel.z) <= planeThickness && std::fabs(rel.x) <= halfWidth;
    return yawPlaneTouch || xPlaneTouch || zPlaneTouch;
}
