#define NOMINMAX
#include <cstdint>
#include "game/scene/tutorial/TutorialScene.h"
#include "engine/graphics/3d/light/SceneLighting.h"
#include "game/scene/GameSceneDrawing.h"
#include "GimmickStageGate.h"
#include "ScenePreloader.h"
#include "InputManager.h"
#include "AudioPlayer.h"
#include "Object3DCommon.h"
#include "SpriteCommon.h"
#include "Object3D.h"
#include "Sprite.h"
#include "ModelManager.h"
#include "TextureManager.h"
#include "CameraManager.h"
#include "CollisionManager.h"
#include "ParticleSystem.h"
#include "imgui.h"
#include "LightManager.h"
#include <game/system/rules/EventManager.h>
#include "SceneManager.h"
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
#include "DebrisEffectManager.h"
#include "MeshEffectManager.h"
#include "VfxSequencer.h"
#include "WinApp.h"
#include "IconsFontAwesome5.h"
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
#include "game/ui/MenuControls.h"
#include "game/ui/SpriteLayoutFactory.h"
#include "DirectXCommon.h"
#include "game/system/settings/GameSettingsManager.h"
#include "game/system/tutorial/TutorialDirector.h"

#include <algorithm>
#include <iterator>

namespace {
constexpr const char* kExitGuideName = "Tutorial_ExitGuideArrow";
constexpr const char* kExitGuideModel = "Stages/tutorial_gate_arrow/tutorial_gate_arrow.obj";
constexpr const char* kExitGuideSettings = "Resources/json/navigation/tutorial_exit_guide.json";

// シーンで管理する出口案内の描画物を探す。リプレイ上で除去済みのものは除く。
engine::Object3D* FindExitGuideArrow(const BaseScene& scene) {
    for (const auto& object : scene.GetObjects())
        // 名前が一致し、現在も有効な目印だけを返す。
        if (object && object->GetName() == kExitGuideName && !object->IsReplayRemoved())
            return object.get();
    return nullptr;
}

constexpr const char* kTutorialMeshEffectsToPreload[] = {
    "Resources/json/effect/shared/effect_pink_slime_charge_pulse_ring.json",
    "Resources/json/effect/player/copy/pink/effect_pink_slime_charge_core_flash.json",
    "Resources/json/effect/player/copy/pink/effect_pink_slime_charge_vortex_streak.json",
    "Resources/json/effect/player/copy/pink/effect_pink_slime_launch_kick_ring.json",
    "Resources/json/effect/player/copy/pink/effect_pink_slime_apex_focus_flash.json",
    "Resources/json/effect/shared/effect_pink_slime_dive_streak.json",
    "Resources/json/effect/shared/effect_pink_slime_landing_burst_ring.json",
    "Resources/json/effect/player/copy/pink/effect_pink_slime_landing_core_flash.json",
    "Resources/json/effect/player/copy/pink/effect_pink_slime_landing_shock_arc.json",
    "Resources/json/effect/player/copy/pink/effect_player_pink_straight_arc.json",
    "Resources/json/effect/player/copy/pink/effect_player_pink_straight_impact.json",
    "Resources/json/effect/player/copy/pink/effect_player_pink_bounce_launch.json",
    "Resources/json/effect/player/copy/pink/effect_player_pink_bounce_land.json",
    "Resources/json/effect/player/base/effect_player_base_bash_arc.json",
    "Resources/json/effect/player/base/effect_player_base_bash_impact.json",
    "Resources/json/effect/player/base/effect_player_base_press_impact.json",
    "Resources/json/effect/player/copy/pink/effect_player_pink_guard_start.json",
    "Resources/json/effect/player/copy/pink/effect_player_pink_guard_shell.json",
    "Resources/json/effect/player/copy/pink/effect_player_pink_guard_release.json"};

constexpr const char* kTutorialGpuParticlePresetsToPreload[] = {"shared/hit_pull_bind",
                                                                "shared/hit_pull_catch",
                                                                "shared/hit_slime_elastic",
                                                                "player/base/player_base_bash_droplets",
                                                                "player/base/player_base_press_splash",
                                                                "shared/hit_throw_slam_dust",
                                                                "shared/hit_enemy_ability",
                                                                "player/base/player_jump_dust",
                                                                "shared/player_land_dust",
                                                                "player/copy/pink/player_pink_straight_splash",
                                                                "player/copy/pink/player_pink_bounce_droplets"};

constexpr const char* kTutorialVfxSequencesToPreload[] = {"player/base/player_base_bash_hit_cue",
                                                          "player/base/player_base_press_enemy_hit_cue",
                                                          "player/base/player_base_press_land_cue",
                                                          "player/copy/pink/player_pink_bounce_slam_cue",
                                                          "shared/slime_elastic_hit_cue",
                                                          "shared/pull_bind_cue",
                                                          "shared/pull_catch_cue",
                                                          "shared/throw_slam_cue",
                                                          "shared/enemy_ability_hit_cue"};

constexpr const char* kTutorialDebrisPresetsToPreload[] = {"shared/throw_slam_pebble_burst", "player/copy/pink/pink_slime_charge_pebble_pull",
                                                           "player/copy/pink/pink_slime_landing_pebble_burst"};

constexpr const char* kTutorialPromptTexturesToPreload[] = {
    "Resources/sprite/ui/tutorial/tutorial_move.png",    "Resources/sprite/ui/tutorial/tutorial_look.png",
    "Resources/sprite/ui/tutorial/tutorial_jump.png",    "Resources/sprite/ui/tutorial/tutorial_lock_on.png",
    "Resources/sprite/ui/tutorial/tutorial_pull.png",    "Resources/sprite/ui/tutorial/tutorial_throw.png",
    "Resources/sprite/ui/tutorial/tutorial_absorb.png",  "Resources/sprite/ui/tutorial/tutorial_ability.png",
    "Resources/sprite/ui/tutorial/tutorial_release.png", "Resources/sprite/ui/tutorial/tutorial_controls.png",
    "Resources/sprite/ui/tutorial/tutorial_exit.png",    "Resources/sprite/ui/tutorial/tutorial_objective_marker.png",
    // 入力を切り替えたフレームに文字画像を読み込まないよう、パッド用も先に準備します。
    "Resources/sprite/ui/tutorial/tutorial_move_gamepad.png",
    "Resources/sprite/ui/tutorial/tutorial_look_gamepad.png",
    "Resources/sprite/ui/tutorial/tutorial_jump_gamepad.png",
    "Resources/sprite/ui/tutorial/tutorial_pull_gamepad.png",
    "Resources/sprite/ui/tutorial/tutorial_throw_gamepad.png",
    "Resources/sprite/ui/tutorial/tutorial_absorb_gamepad.png",
    "Resources/sprite/ui/tutorial/tutorial_release_gamepad.png",
    "Resources/sprite/ui/tutorial/tutorial_controls_gamepad.png"};
} // namespace

// チュートリアルのシーン側は入力・描画と手順管理の接続役です。課題の達成判定は TutorialDirector に委ねます。

TutorialScene::TutorialScene() {
    // 描画基盤の取得はシーン初期化まで遅らせる。
}

TutorialScene::~TutorialScene() {
    // シーン固有の解放はFinalizeへ集約する。
}

void TutorialScene::SetIsGoal(bool isGoal) {
    // 再挑戦などでゴール状態が解除されたら、保存済みフラグも戻す。
    // ゴール状態が変わっていなければ、保存の再試行状態もそのまま保持する。
    if (isGoal_ == isGoal) {
        return;
    }

    isGoal_ = isGoal;

    // ゴールを解除した場合は、次の挑戦で保存とエラー通知をやり直せるようにする。
    if (!isGoal_) {
        goalSavePerformed_ = false;
        goalSaveErrorShown_ = false;
        goalSaveRetryTimer_ = 0.0f;
    }
}

void TutorialScene::CollectReplaySprites(std::vector<engine::Sprite*>& replaySprites) {
    // チュートリアル専用の案内と保存表示もリプレイの描画対象へ加える。
    BaseScene::CollectReplaySprites(replaySprites);

    // 照準の表示物を生成済みなら、そのスプライトもリプレイ描画へ渡す。
    if (lockOnSprite_) {
        replaySprites.push_back(lockOnSprite_.get());
    }

    // 操作案内がある場合は、その表示物の登録を案内側へ任せる。
    if (controlsGuideOverlay_) {
        controlsGuideOverlay_->CollectReplaySprites(replaySprites);
    }

    // 保存通知がある場合は、その表示物も通知側から登録する。
    if (saveIndicatorOverlay_) {
        saveIndicatorOverlay_->CollectReplaySprites(replaySprites);
    }
}

void TutorialScene::CaptureReplaySceneState(json& state) const {
    // シーン進行だけでなく、案内画面と保存演出の表示状態も記録する。
    json tutorialState = json::object();
    json controlsGuideState = json::object();
    json saveIndicatorState = json::object();
    // 進行管理が利用できる場合だけ、その課題の達成状態を控える。
    if (tutorialDirector_)
        tutorialDirector_->CaptureReplayState(tutorialState);
    // 操作案内が存在する場合は、開閉やページの状態も記録する。
    if (controlsGuideOverlay_)
        controlsGuideOverlay_->CaptureReplayState(controlsGuideState);
    // 保存通知が存在する場合は、表示の進行状態を記録する。
    if (saveIndicatorOverlay_)
        saveIndicatorOverlay_->CaptureReplayState(saveIndicatorState);

    // シーンが持つゴール・ムービー・取得物・HUDと、委譲先の記録を一つのJSONへまとめる。
    state = {{"goal", {{"active", isGoal_}, {"savePerformed", goalSavePerformed_}}},
             {"movie",
              {{"state", static_cast<std::int32_t>(movieState_)},
               {"timer", movieTimer_},
               {"startCameraEye", {movieStartCameraEye_.x, movieStartCameraEye_.y, movieStartCameraEye_.z}},
               {"startCameraTarget", {movieStartCameraTarget_.x, movieStartCameraTarget_.y, movieStartCameraTarget_.z}},
               {"bridgeDropped", hasBridgeDropped_}}},
             {"sessionStarCoins", {sessionStarCoins_[0], sessionStarCoins_[1], sessionStarCoins_[2]}},
             {"healthHud", {{"previousHp", healthHud_.State().previousHp},
                            {"previousRate", healthHud_.State().previousRate},
                            {"hpDamageHoldTimer", healthHud_.State().damageHoldTimer},
                            {"hpDelayedRate", healthHud_.State().delayedRate}}},
             {"tutorial", std::move(tutorialState)},
             {"overlays",
              {{"controlsGuide", std::move(controlsGuideState)}, {"saveIndicator", std::move(saveIndicatorState)}}}};
}

void TutorialScene::RestoreReplaySceneState(const json& state) {
    // 任意項目を含む過去の記録でも復元できるよう、存在を確かめてから読む。
    // シーン固有の記録がオブジェクトでなければ、現在値を保つ。
    if (!state.is_object()) {
        return;
    }
    // 残量だけでなく被弾表示の保持時間も戻し、同じ時点のHPバーを再現します。
    if (const auto found = state.find("healthHud"); found != state.end() && found->is_object()) {
        auto& hp = healthHud_.State();
        hp.previousHp = found->value("previousHp", hp.previousHp);
        hp.previousRate = found->value("previousRate", hp.previousRate);
        hp.damageHoldTimer = found->value("hpDamageHoldTimer", hp.damageHoldTimer);
        hp.delayedRate = found->value("hpDelayedRate", hp.delayedRate);
    }


    auto restoreVector3 = [](const json& object, const char* key, engine::Vector3& value) {
        const auto found = object.find(key);

        // 添字0〜2を読み出せる配列だけを使い、項目が欠ける場合は現在値のままにする。
        if (found != object.end() && found->is_array() && found->size() >= 3) {
            value = {(*found)[0].get<float>(), (*found)[1].get<float>(), (*found)[2].get<float>()};
        }
    };

    // ゴールの記録が存在する場合だけ、進行中かと保存済みかを戻す。
    if (const auto found = state.find("goal"); found != state.end() && found->is_object()) {
        isGoal_ = found->value("active", false);
        goalSavePerformed_ = found->value("savePerformed", false);
    }

    // 演出の種類・時間・視点を記録から戻し、橋を落とした後の進行を保持します。
    if (const auto found = state.find("movie"); found != state.end() && found->is_object()) {
        const std::int32_t movieState = (std::clamp)(found->value("state", 0), 0, 1);
        movieState_ = static_cast<MovieState>(movieState);
        movieTimer_ = (std::max)(0.0f, found->value("timer", 0.0f));
        restoreVector3(*found, "startCameraEye", movieStartCameraEye_);
        restoreVector3(*found, "startCameraTarget", movieStartCameraTarget_);
        hasBridgeDropped_ = found->value("bridgeDropped", false);
    }

    // 記録と現在の配列で重なる範囲だけ、今回拾ったスターの目印を復元します。
    if (const auto found = state.find("sessionStarCoins"); found != state.end() && found->is_array()) {
        for (size_t index = 0; index < 3 && index < found->size(); ++index) {
            sessionStarCoins_[index] = (*found)[index].get<bool>();
        }
    }

    // 手順管理と記録が両方ある場合だけ、課題の状態を戻す。
    if (tutorialDirector_ && state.contains("tutorial")) {
        tutorialDirector_->RestoreReplayState(state["tutorial"]);
    }

    // 案内と通知の記録がオブジェクトとして存在する場合だけ、各UIへ引き渡す。
    if (const auto found = state.find("overlays"); found != state.end() && found->is_object()) {
        // 操作案内の実体と対応する記録が両方ある場合に復元する。
        if (controlsGuideOverlay_ && found->contains("controlsGuide")) {
            controlsGuideOverlay_->RestoreReplayState((*found)["controlsGuide"]);
        }

        // 保存通知の実体と対応する記録が両方ある場合に復元する。
        if (saveIndicatorOverlay_ && found->contains("saveIndicator")) {
            saveIndicatorOverlay_->RestoreReplayState((*found)["saveIndicator"]);
        }
    }
}

SceneLoadManifest TutorialScene::BuildAsyncLoadManifest() const {
    // 操作案内とプレイヤー演出を初回表示前に読み込み、描画中の遅延を抑える。
    SceneLoadManifest manifest;
    manifest.AddModel(kExitGuideModel);
    manifest.AddObjectLayout(HasSceneAssetContext() && !GetSceneLoadContext().objectLayoutPath.empty()
                                 ? GetSceneLoadContext().objectLayoutPath
                                 : "Resources/json/3Dobject/tutorial.json");
    manifest.AddSpriteLayout(HasSceneAssetContext() && !GetSceneLoadContext().spriteLayoutPath.empty()
                                 ? GetSceneLoadContext().spriteLayoutPath
                                 : "Resources/json/sprite/tutorialScene.json");
    manifest.AddSpriteLayout("Resources/json/sprite/controlsGuide.json");

    // 操作ガイドとコピー能力HUDの画像はcontrolsGuide.jsonの参照から収集します。
    manifest.AddTexture("Resources/sprite/common/circle2.png");
    manifest.AddTexture("Resources/sprite/common/white.png");
    manifest.AddSpriteLayout("Resources/json/sprite/tutorialSkip.json");


    manifest.AddTexture("Resources/sprite/ui/settings/settings_footer_button_selected.png");
    manifest.AddTexture("Resources/sprite/ui/hud/lockOn.png");

    for (const char* path : kTutorialPromptTexturesToPreload) {
        manifest.AddTexture(path);
    }

    manifest.AddTexture(GetSceneLoadContext().skyboxPath.empty() ? "Resources/output_skybox.dds"
                                                                 : GetSceneLoadContext().skyboxPath);
    // シーン資産のBGMを優先し、未指定の場合は現在ステージの登録値を先読み対象にします。
    std::string preloadBgmPath = GetSceneLoadContext().bgmPath;

    // シーンにBGMが指定されていない場合だけ、現在選ばれているステージの設定を調べる。
    if (preloadBgmPath.empty()) {
        const auto& preloadStages = StageManager::GetInstance()->GetStages();
        const std::int32_t preloadStageIndex = StageManager::GetInstance()->GetCurrentStageIndex();

        // ステージ番号が登録の範囲内なら、そのBGMを代替として先読みする。
        if (preloadStageIndex >= 0 && preloadStageIndex < static_cast<std::int32_t>(preloadStages.size())) {
            preloadBgmPath = preloadStages[preloadStageIndex].bgmPath;
        }
    }

    // シーン側またはステージ側からBGMパスを得られた場合だけ登録する。
    if (!preloadBgmPath.empty()) {
        manifest.AddAudio(preloadBgmPath);
    }

    return manifest;
}

void TutorialScene::Initialize() {
    // 同じ段階初期化を同期呼び出しでも使い、読込画面の有無で処理を分けない。
    BeginLoadingInitialize();

    // 同期ロードでは、分割初期化が完了を返すまで順に進める。
    while (!InitializeLoadingStep()) {
    }
}

void TutorialScene::BeginLoadingInitialize() {
    // 段階ごとの完了数を初期化し、読み込み進捗へ反映する。
    loadingInitializePhase_ = 0;
    loadingInitializeItemIndex_ = 0;
    loadingInitializeCompletedUnits_ = 0;
    loadingInitializeTotalUnits_ =
        10 + std::size(kTutorialGpuParticlePresetsToPreload) + std::size(kTutorialMeshEffectsToPreload) +
        std::size(kTutorialDebrisPresetsToPreload) + std::size(kTutorialVfxSequencesToPreload);
}

bool TutorialScene::InitializeLoadingStep() {
    // 一度の呼び出しでは一段階だけ進め、読込画面が途中経過を描けるようにする。
    auto completeUnit = [this]() { ++loadingInitializeCompletedUnits_; };

    for (;;) {
        switch (loadingInitializePhase_) {
        // 入力と共有カメラを先に接続し、配置を作る前の基盤を整えます。
        case 0: {
            inputManager_ = engine::InputManager::GetInstance();
            LOG("Tutorial Scene Initialized!");
            const StageData& currentStage = StageManager::GetInstance()->GetCurrentStage();
            CameraManager::GetInstance()->Initialize();
            CameraManager::GetInstance()->SetInputManager(inputManager_);
            ++loadingInitializePhase_;
            completeUnit();
            return false;
        }

        case 1:
            // Sceneが所有する描画基盤を先に作る。各Managerにはその後で借用ポインタを渡す。
            particleSystem_ = std::make_unique<engine::ParticleSystem>();
            particleSystem_->Initialize(GetParticleCommon(), "Resources/sprite/common/circle2.png");
            ParticleManager::GetInstance()->Initialize(particleSystem_.get());
            gameRule_ = std::make_unique<GameRule>();
            gameRule_->Initialize(this);
            LightEditor::GetInstance()->SetObject3DCommon(GetObject3DCommon());
            ++loadingInitializePhase_;
            completeUnit();
            return false;

        // 対象集合を用意した後で、ロックオン表示と弾の衝突管理を結び付けます。
        case 2: {
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
            // プリセットの先読みより前にGPU粒子の管理器と共有テクスチャを用意する。
            PrepareGpuParticles();
            gpuParticleTexHandle_ = engine::TextureManager::GetInstance()->Load("Resources/sprite/common/white.png");
            ++loadingInitializePhase_;
            loadingInitializeItemIndex_ = 0;
            completeUnit();
            return false;

        case 4:
            // 重い先読みは一項目ずつ返し、ロード画面の描画機会を残す。
            // 未処理のGPUプリセットがあれば一件だけ先読みし、以降は次回の呼び出しへ残す。
            if (loadingInitializeItemIndex_ < std::size(kTutorialGpuParticlePresetsToPreload)) {
                GpuParticleManager::GetInstance()->PreloadPresetSystem(
                    kTutorialGpuParticlePresetsToPreload[loadingInitializeItemIndex_++]);
                completeUnit();
                return false;
            }

            ++loadingInitializePhase_;
            loadingInitializeItemIndex_ = 0;
            continue;

        // メッシュエフェクトも一項目ずつ準備し、描画開始時の初回読込を減らします。
        case 5:
            // 未処理のメッシュ演出があれば一件だけ準備し、今回の呼び出しを戻す。
            if (loadingInitializeItemIndex_ < std::size(kTutorialMeshEffectsToPreload)) {
                MeshEffectManager::GetInstance()->PreloadEffect(
                    kTutorialMeshEffectsToPreload[loadingInitializeItemIndex_++]);
                completeUnit();
                return false;
            }

            ++loadingInitializePhase_;
            loadingInitializeItemIndex_ = 0;
            continue;

        // 破片の個別先読みより先に、プリセット定義全体を管理器へ読み込みます。
        case 6:
            DebrisEffectManager::GetInstance()->Initialize(GetObject3DCommon());
            DebrisEffectManager::GetInstance()->LoadAllPresets("Resources/json/debris/");
            ++loadingInitializePhase_;
            loadingInitializeItemIndex_ = 0;
            completeUnit();
            return false;

        // チュートリアルで使う破片だけを個別に先読みします。
        case 7:
            // 使う破片プリセットをすべて準備するまで、一件ごとに描画側へ制御を返す。
            if (loadingInitializeItemIndex_ < std::size(kTutorialDebrisPresetsToPreload)) {
                DebrisEffectManager::GetInstance()->PrewarmPreset(
                    kTutorialDebrisPresetsToPreload[loadingInitializeItemIndex_++]);
                completeUnit();
                return false;
            }

            ++loadingInitializePhase_;
            loadingInitializeItemIndex_ = 0;
            continue;

        // 技から参照されるVFXシーケンスを毎回一件ずつ読みます。
        case 8:
            // 残っているシーケンスを一件だけ読み、共有キャッシュへ入れる。
            if (loadingInitializeItemIndex_ < std::size(kTutorialVfxSequencesToPreload)) {
                VfxSequencer sequence;
                sequence.Load(kTutorialVfxSequencesToPreload[loadingInitializeItemIndex_++]);
                completeUnit();
                return false;
            }

            ++loadingInitializePhase_;
            continue;

        // 環境画像を取得し、配置を展開する前に背景用の空を準備します。
        case 9:
            skyboxTextureHandle_ =
                engine::TextureManager::GetInstance()->Load(ResolveSceneSkyboxPath("Resources/output_skybox.dds"));
            skybox_ = std::make_unique<Skybox>();
            skybox_->Initialize(GetObject3DCommon(), skyboxTextureHandle_);
            ++loadingInitializePhase_;
            completeUnit();
            return false;

        // 先に3D配置を展開し、次の段階でHUDからプレイヤーを参照できるようにします。
        case 10:
            levelLoader_ = std::make_unique<LevelLoader>();
            levelLoader_->LoadObjectLayout(this, "Resources/json/3Dobject/tutorial.json");
            ++loadingInitializePhase_;
            completeUnit();
            return false;

        // レイアウトにある能力表示とHPの名前を結び付け、チュートリアル中だけ保護を有効にします。
        case 11:
            levelLoader_->LoadSpriteLayout(this, "Resources/json/sprite/tutorialScene.json");
            copyStyleHud_.Bind(GetSpriteByName("hud_copy_style"));
            healthHud_.Bind(GetSpriteByName("hud_hp_icon"), GetSpriteByName("hud_hp_damage_fill"),
                            GetSpriteByName("hud_hp_fill"), GetSpriteByName("hud_hp_highlight"),
                            GetSpriteByName("hud_hp_frame"), player_);

            // 配置からプレイヤーを生成できたときだけ、練習中の被害防止を有効にする。
            if (player_) {
                player_->SetTutorialSafetyEnabled(true);
            }

            ++loadingInitializePhase_;
            completeUnit();
            return false;

        // 案内・保存表示と出口の目印を用意し、手順管理の初期化に成功したときだけ完了通知を受けます。
        case 12:
            controlsGuideOverlay_ = std::make_unique<ControlsGuideOverlay>();
            controlsGuideOverlay_->Initialize(GetSpriteCommon(), player_);
            saveIndicatorOverlay_ = std::make_unique<SaveIndicatorOverlay>();
            saveIndicatorOverlay_->Initialize(GetSpriteCommon());
            InitializeExitGuide();
            InitializeSkipUi();
            tutorialDirector_ = std::make_unique<TutorialDirector>();

            // 手順管理が正常に接続できた場合だけ、達成時の保存処理を登録する。
            if (tutorialDirector_->Initialize(this, player_, inputManager_)) {
                tutorialDirector_->SetCompletionCallback([this]() { HandleTutorialFlowCompleted(); });
            }

            ++loadingInitializePhase_;
            completeUnit();
            return false;

        // 照明とカメラの初期状態をそろえてから、ロード完了を呼び出し側へ通知します。
        case 13:
            engine::LightManager::GetInstance()->LoadState(ResolveSceneLightPath("Resources/json/light/light_layout.json"));
            CameraEditor::GetInstance()->Initialize();

            // 本編と同じ三人称カメラ設定を共有します。
            CameraEditor::GetInstance()->LoadFile("game_camera.json");
            CameraEditor::GetInstance()->SetMode(CameraEditor::Mode::kGame);

            // メインカメラを取得できたら、前シーンから引き継いだ追従の補間を消す。
            if (engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera()) {
                camera->ResetFollowSmoothing();
            }

            ++loadingInitializePhase_;
            completeUnit();
            return true;

        // 最終段階を通過済みの場合は、初期化済みとして返す。
        default:
            return true;
        }
    }
}

float TutorialScene::GetLoadingInitializeProgress() const {
    // 初期化前の分母ゼロは完了扱いにし、進捗表示を有効範囲に収める。
    return loadingInitializeTotalUnits_ == 0 ? 1.0f
                                             : std::clamp(static_cast<float>(loadingInitializeCompletedUnits_) /
                                                              static_cast<float>(loadingInitializeTotalUnits_),
                                                          0.0f, 1.0f);
}

void TutorialScene::OnActivated() {
    BaseScene::OnActivated();
#ifndef USE_IMGUI
    // 配布版は通常カーソルを中央固定するため、このシーンにいる間だけポインターの移動を許す。
    if (auto* window = engine::DirectXCommon::GetInstance()->GetWinApp()) {
        CURSORINFO cursor{sizeof(CURSORINFO)};
        skipPreviousCursorVisible_ = GetCursorInfo(&cursor) && (cursor.flags & CURSOR_SHOWING) != 0;
        window->SetCursorRecenteringSuppressed(true);
        WinApp::SetCursorVisibility(true);
        skipCursorEnabled_ = true;
    }
#endif
}

void TutorialScene::Finalize() {
#ifndef USE_IMGUI
    // ローディング途中で破棄された場合はカーソル状態を変更しない。
    if (skipCursorEnabled_) {
        if (auto* window = engine::DirectXCommon::GetInstance()->GetWinApp()) {
            window->SetCursorRecenteringSuppressed(false);
            WinApp::SetCursorVisibility(skipPreviousCursorVisible_);
        }
        skipCursorEnabled_ = false;
    }
#endif
    // 配置の管理を破棄する前に、出口とゲートが持つ一時状態を解除します。
    HideExitGuide();
    exitGuide_.Reset();
    exitGuideRequested_ = false;
    GimmickStageGate::ResetSceneEntries(*this);
    // Directorを先に終了し、演出が参照する共有Managerをその後に片付ける。
    if (tutorialDirector_) {
        tutorialDirector_->Finalize();
    }

    tutorialDirector_.reset();

    // 操作案内を作成していた場合は、その専用の終了処理を先に呼ぶ。
    if (controlsGuideOverlay_) {
        controlsGuideOverlay_->Finalize();
    }

    controlsGuideOverlay_.reset();
    MeshEffectManager::GetInstance()->Clear();
    VfxSequencer::ClearOneShots();
    GpuParticleManager::GetInstance()->ClearSceneRuntime();
    DebrisEffectManager::GetInstance()->Clear();
    engine::CollisionManager::GetInstance()->ClearObjects();
    BulletManager::GetInstance()->Finalize();
    // このシーンが所有する表示資源を解放します。HP表示の借用参照はSpriteを消す前に外します。
    saveIndicatorOverlay_.reset();
    for (auto& sprite : skipSprites_) sprite.reset();
    particleSystem_.reset();
    healthHud_.Reset();
    sprites_.clear();
    objectManager_.reset();
    lockOnSystem_.reset();
}

void TutorialScene::FixedUpdate(float fixedDeltaTime) {
    if (!skipMenuOpen_) BaseScene::FixedUpdate(fixedDeltaTime);
}

void TutorialScene::Update(float deltaTime) {
    // 途中ムービーや操作案内も、全体スキップの確認を開いている間は一時停止する。
    if (UpdateSkipUi()) return;
    // ゲート演出が時間を引き受ける間は、案内を隠し、通常のゲーム更新を行いません。
    if (GimmickStageGate::UpdateSceneEntry(*this, deltaTime)) {
        HideExitGuide();
        // ゲート演出に切り替わっても、保存の表示が存在する場合は進める。
        if (saveIndicatorOverlay_) saveIndicatorOverlay_->Update(deltaTime);
        UpdateUi();
        return;
    }
    // 手順完了後に保存できなければ出口を閉じたまま、一定間隔で再試行する。
    if (!isGoal_ && tutorialDirector_ && tutorialDirector_->IsCompleted() && !goalSavePerformed_) {
        goalSaveRetryTimer_ = (std::max)(0.0f, goalSaveRetryTimer_ - deltaTime);
        // 再試行の残り時間がゼロに達したとき、それまで閉じていた出口の保存を試す。
        if (goalSaveRetryTimer_ <= 0.0f)
            HandleTutorialFlowCompleted();
    }

    // ゴール後はクリアを一度だけ保存し、保存演出が終わるまで選択画面へ進めない。
    if (isGoal_) {
        goalSaveRetryTimer_ = (std::max)(0.0f, goalSaveRetryTimer_ - deltaTime);

        // ゴール済みで保存が未完了なら、再試行の待ちが終わった時点で書き込む。
        if (!goalSavePerformed_ && goalSaveRetryTimer_ <= 0.0f) {
            // ステージ番号-1で保存が成功した場合だけ、保存完了の状態にする。
            if (GameDataManager::GetInstance()->CommitStageClear(-1, {})) {
                // 通知の表示物があれば保存のアイコンを1.35秒表示する。
                if (saveIndicatorOverlay_)
                    saveIndicatorOverlay_->Play(1.35f);
                goalSavePerformed_ = true;
            // 保存に失敗したときは進行を保留し、5秒後へ再試行を予約する。
            } else {
                goalSaveRetryTimer_ = 5.0f;

                // 同じ失敗で毎回通知しないよう、まだ表示していない場合だけダイアログを開く。
                if (!goalSaveErrorShown_) {
                    ::MessageBoxW(nullptr, L"セーブに失敗しました。保存先を確認してください。5秒後に再試行します。",
                                  L"セーブエラー", MB_OK | MB_ICONERROR);
                    goalSaveErrorShown_ = true;
                }
            }
        }

        // 通知が設定されている間は、ゲーム側の時間を止める前に表示を進める。
        if (saveIndicatorOverlay_) {
            saveIndicatorOverlay_->Update(deltaTime);
        }

        deltaTime = 0.0f;

        // 保存と通知が終わった状態で確定が入力された場合だけ、ステージ選択へ進む。
        if (goalSavePerformed_ && (inputManager_->IsKeyTriggered(DIK_SPACE) || inputManager_->IsGamepadButtonTriggered(XINPUT_GAMEPAD_A)) &&
            (!saveIndicatorOverlay_ || !saveIndicatorOverlay_->IsPlaying())) {
            SceneManager::GetInstance()->ChangeScene("SELECT");
            return;
        }
    }

    // 操作ガイドが入力を受け持ったフレームは、保存通知の表示だけを進めます。
    if (HandleControlsGuideOverlay(deltaTime)) {
        HideExitGuide();
        // 操作案内の表示でも、保存通知が存在する場合は更新する。
        if (saveIndicatorOverlay_) {
            saveIndicatorOverlay_->Update(deltaTime);
        }

        return;
    }

    // ゴール前のフレームだけ、経過秒数をプレイ記録に足す。
    if (!isGoal_) {
        GameDataManager::GetInstance()->AdvancePlayTime(deltaTime);
    }

    // 描画のポストエフェクトとライトを更新し、現在の投影に合わせる。
    PostEffect::GetInstance()->Update(deltaTime);
    engine::Camera* activeCamera = CameraManager::GetInstance()->GetActiveCamera();

    // 投影を取得できる場合だけ、画面効果の深度逆変換へ渡す。
    if (activeCamera) {
        PostEffect::GetInstance()->GetParams()->projectionInverse = engine::Math::Inverse(activeCamera->GetProjectionMatrix());
    }

    static engine::Math sMath;
    LightEditor::GetInstance()->Update();

    engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();

    lockOnSystem_->Update(objectManager_->GetObjects(), camera, player_, deltaTime);
    CameraEditor::GetInstance()->Update(player_, lockOnSystem_->IsLockingOn());

    engine::Object3D* target = lockOnSystem_->GetTarget();

    // ロックオン先の当たり判定中心を投影し、奥行きに応じて印の大きさを変えます。
    if (target && lockOnSystem_->IsLockingOn()) {
        isDrawLockOn_ = true;
        engine::Aabb aabb = target->GetAabb();

        engine::Vector3 targetCenter;
        targetCenter.x = (aabb.min.x + aabb.max.x) * 0.5f;
        targetCenter.y = (aabb.min.y + aabb.max.y) * 0.5f;
        targetCenter.z = (aabb.min.z + aabb.max.z) * 0.5f;

        engine::Matrix4x4 viewProj = sMath.Multiply(camera->GetViewMatrix(), camera->GetProjectionMatrix());
        float w = targetCenter.x * viewProj.m[0][3] + targetCenter.y * viewProj.m[1][3] +
                  targetCenter.z * viewProj.m[2][3] + viewProj.m[3][3];

        // 透視除算の分母が正で十分大きい場合だけ、対象を画面へ投影する。
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

            float objSizeX = aabb.max.x - aabb.min.x;
            float objSizeY = aabb.max.y - aabb.min.y;
            float objSizeZ = aabb.max.z - aabb.min.z;
            float maxObjSize = std::max({objSizeX, objSizeY, objSizeZ});

            float baseSize = maxObjSize * 25.0f;
            float distanceScale = 20.0f / w;

            float finalSize = baseSize * distanceScale;
            finalSize = std::max(32.0f, std::min(finalSize, 256.0f));

            lockOnSprite_->SetSize({finalSize, finalSize});
            lockOnSprite_->SetRotation(lockOnSprite_->GetRotation() + 2.0f * deltaTime);
            lockOnSprite_->Update();
        // 対象が視点の後ろか投影できないほど近い場合はマーカーを隠す。
        } else {
            isDrawLockOn_ = false;
        }
    // 対象を取得できないかロック中でない場合もマーカーを隠す。
    } else {
        isDrawLockOn_ = false;
    }

    // ゲームカメラが操作を許すモードのときだけ、設定の感度を使って視点を回します。
    if (!CameraEditor::GetInstance()->IsEditorMode()) {
        engine::Camera::FollowMode currentMode = camera->GetFollowMode();

        // 三人称エイムまたは一人称を使う場合だけ、視点操作の設定を反映する。
        if (currentMode == engine::Camera::FollowMode::kAimable || currentMode == engine::Camera::FollowMode::kFirstPerson) {
            const auto* settings = GameSettingsManager::GetInstance();
            camera->SetRotationSensitivity(settings->GetCameraSensitivity());
            const engine::Vector2 lookDelta = inputManager_->GetCameraLookDelta(deltaTime);

            // カーソルかスティックの差分があるときだけ、視線の回転を依頼する。
            if (lookDelta.x != 0.0f || lookDelta.y != 0.0f) {
                camera->AddRotation(settings->ApplyCameraLookInversion(lookDelta));
            }
        }
    }

    ProfilerManager::GetInstance()->SetObjectList(&objectManager_->GetObjects());
    CameraManager::GetInstance()->Update(deltaTime);
    particleSystem_->Update(deltaTime);

    // 現在の対象を通知して手順を進め、以降で配置物とルールの更新を行います。
    if (tutorialDirector_) {
        tutorialDirector_->NotifyLockOnTarget(lockOnSystem_->IsLockingOn() ? lockOnSystem_->GetTarget() : nullptr);
        tutorialDirector_->Update(deltaTime);
    }

    objectManager_->Update(deltaTime);

    // ルールが用意できていれば、配置物の更新後に進行判定を動かす。
    if (gameRule_) {
        gameRule_->Update(deltaTime);
    }

    GpuParticleManager::GetInstance()->Update(deltaTime);
    copyStyleHud_.Update(player_, deltaTime);
    healthHud_.Update(player_, deltaTime);

    for (auto& sprite : sprites_) {
        sprite->Update();
    }

    // 保存通知があれば、通常のUI更新でも表示の時計を進める。
    if (saveIndicatorOverlay_) {
        saveIndicatorOverlay_->Update(deltaTime);
    }

    BulletManager::GetInstance()->Update(deltaTime);
    engine::CollisionManager::GetInstance()->Update();
    // 接地・衝突で修正された最新の足元へ追従し、移動に対する一フレームの遅れを作らない。
    UpdateExitGuide(deltaTime);
    UpdateUi();
}

void TutorialScene::Draw() {
    // カメラがプレイヤーに接近したら身体と子パーツを各3Dパスから除き、視界を確保します。
    // チュートリアルは専用の演出パスを持たず、通常モデル→半透明→深度参照表現の順です。
    bool isFirstPerson = false;
    engine::Camera* camera = CameraManager::GetInstance()->GetActiveCamera();

    // 本体と視点の両方を得られた場合に限り、身体を描くか距離で判定する。
    if (player_ && camera) {
        engine::Vector3 pPos = player_->GetWorldPosition();
        pPos.y += 1.0f;
        engine::Vector3 cPos = camera->GetEye();
        engine::Vector3 toCam = {cPos.x - pPos.x, cPos.y - pPos.y, cPos.z - pPos.z};
        // 足元から1ワールド単位上を頭側の目安とし、視点が3ワールド単位より近いときに本体を描画から外す。
        float distance = std::sqrt(toCam.x * toCam.x + toCam.y * toCam.y + toCam.z * toCam.z);

        // 視点から基準点まで3ワールド単位未満の場合は、一人称と同じように身体を非表示にする。
        if (distance < 3.0f) {
            isFirstPerson = true;
        }
    }

    // この描画で使う点光源とスポットライトを一度借り、後続の描画に渡す。
    const auto lighting = engine::SceneLighting::Capture();

    auto& objects = objectManager_->GetObjects();

    // 背景と不透明モデルで深度を確定してから、弾と半透明を描画します。
    BeginWorldObjectDrawing(skybox_.get(), camera);
    GetObject3DCommon()->SetPipelineState(BlendMode::kNone);

    DrawSceneObjectPass(objects, materialRenderPolicy::Pass::kOpaque, lighting,
        player_, isFirstPerson, true, [this](engine::Object3D* object) { return IsVisible(object); });

    // 身体を隠した近距離でも、操作の目印であるフックマーカーは残します。
    if (player_ && player_->GetHookMarker()) {
        lighting.Draw(*player_->GetHookMarker(), &engine::Object3D::Draw);
    }

    DrawEnemyTelegraphs(*this, lighting);
    lighting.Draw(*BulletManager::GetInstance(), &BulletManager::Draw);
    LightEditor::GetInstance()->Draw3D();

    // 半透明は不透明パスから外しておき、地形の後に重ねます。
    DrawSceneObjectPass(objects, materialRenderPolicy::Pass::kTransparent, lighting,
        player_, isFirstPerson, true, {});
    particleSystem_->Draw();

    // 霧と特殊材質は画面色・深度を読むため、通常のObject描画とは分けます。
    DrawLocalFogObjects(objects, player_, isFirstPerson);

    bool hasGpuParticles = !GpuParticleManager::GetInstance()->IsEmpty();
    bool grabUpdated =
        DrawSpecialMaterialObjects(objects, BulletManager::GetInstance(), player_, isFirstPerson);
    // GPU粒子の描画対象がある場合だけ、特殊材質が準備した画面コピーの有無を渡して描く。
    if (hasGpuParticles) {
        DrawGpuParticles(camera, gpuParticleTexHandle_, grabUpdated);
    }
}

void TutorialScene::DrawUi() {
    // 背景Sprite→照準→プレイヤーUI→操作案内→保存通知の順で重ねます。
    // 案内と保存通知はゲーム本体のSpriteより手前に置き、表示中に読めるようにします。
    BeginSpriteDrawing();

    // チュートリアルのムービー中は画面上の操作UIを描かず、終了後に戻します。
    if (tutorialDirector_ && tutorialDirector_->IsCinematicPlaying()) {
        DrawSkipUi();
        return;
    }

    for (auto& sprite : sprites_)
        // HUDが個別に順序を決めて描くHPの部品は、汎用のスプライト列から除く。
        if (!healthHud_.Owns(sprite.get())) sprite->Draw();
    healthHud_.Draw();
    // ロックオンが投影可能で照準の実体がある場合に照準を重ねる。
    if (isDrawLockOn_ && lockOnSprite_)
        lockOnSprite_->Draw();
    // プレイヤーが存在する場合は、プレイヤー側にUIの描画機会を渡す。
    if (player_)
        player_->DrawUi();
    // 操作案内が開いている間だけ、通常UIより前に重ねる。
    if (controlsGuideOverlay_ && controlsGuideOverlay_->IsActive()) {
        controlsGuideOverlay_->Draw();
    }

    // 保存通知が表示中の場合だけ、最後に重ねて読めるようにする。
    if (saveIndicatorOverlay_ && saveIndicatorOverlay_->IsActive())
        saveIndicatorOverlay_->Draw();

    DrawSkipUi();
}

void TutorialScene::InitializeSkipUi() {
    // レイアウトを読み、通常ボタンと確認画面の描画部品を初期化する。
    json layout;
    constexpr const char* layoutPath = "Resources/json/sprite/tutorialSkip.json";
    if (!TakePreparedJson(layoutPath, layout)) {
        std::ifstream input(layoutPath);
        if (input) layout = json::parse(input, nullptr, false);
    }
    if (!layout.is_object() || !layout.contains("sprites") || !layout["sprites"].is_array()) return;

    constexpr std::array<const char*, static_cast<size_t>(SkipUiPart::kCount)> names{
        "skip_open", "skip_open_label", "skip_backdrop", "skip_panel", "skip_title",
        "skip_description", "skip_save_error", "skip_cancel", "skip_cancel_label", "skip_confirm", "skip_confirm_label", "skip_hint"
    };

    // Sprite Editorの基準解像度とアンカーを描画・クリックの両方で使用する。
    const auto scale = spriteLayoutScaler::Make(layout);
    for (const auto& entry : layout["sprites"]) {
        const auto name = spriteLayoutScaler::ReadString(entry, "name", "");
        const auto found = std::find(names.begin(), names.end(), name);
        if (found == names.end()) continue;
        auto sprite = spriteLayoutFactory::Create(GetSpriteCommon(), entry, scale, *found);
        const auto crop = entry.find("textureCrop");
        if (crop != entry.end() && crop->is_array() && crop->size() == 4 &&
            std::all_of(crop->begin(), crop->end(), [](const auto& value) { return value.is_number(); })) {
            sprite->SetTextureRect({(*crop)[0].get<float>(), (*crop)[1].get<float>()},
                                   {(*crop)[2].get<float>(), (*crop)[3].get<float>()});
        }
        skipSprites_[static_cast<size_t>(found - names.begin())] = std::move(sprite);
    }
    skipButtonNormal_ = engine::Sprite::LoadTexture("ui/settings/settings_footer_button.png");
    skipButtonSelected_ = engine::Sprite::LoadTexture("ui/settings/settings_footer_button_selected.png");
    skipMenuOpen_ = skipButtonHovered_ = skipConfirmSelected_ = skipSaveFailed_ = false;
}

bool TutorialScene::UpdateSkipUi() {
    // 編集停止中やゴール確定済みの画面からは、スキップを開始しない。
    auto* sceneManager = SceneManager::GetInstance();
    if (!sceneManager->IsPlaying() || isGoal_ || !inputManager_) {
        skipMenuOpen_ = false;
        skipButtonHovered_ = false;
        return false;
    }

    // ウィンドウの黒帯や、Game View以外のエディター領域はクリック対象から外す。
    auto pointer = inputManager_->GetMousePosition();
    auto area = WinApp::CalculateGameViewRect(WinApp::GetWindowWidth(), WinApp::GetWindowHeight());
#ifdef USE_IMGUI
    if (auto* editor = GetDebugEditor()) {
        std::int32_t left = 0, top = 0, right = 0, bottom = 0;
        POINT screen{};
        if (editor->GetGameViewScreenRect(left, top, right, bottom) && GetCursorPos(&screen)) {
            area = {static_cast<float>(left), static_cast<float>(top),
                static_cast<float>(right - left), static_cast<float>(bottom - top)};
            pointer = {static_cast<float>(screen.x), static_cast<float>(screen.y)};
        } else {
            area = {};
        }
    }
#endif
    WinApp::ClientToGamePosition(pointer.x, pointer.y, area, pointer.x, pointer.y);
    const auto hit = [&](SkipUiPart part) {
        const auto& sprite = skipSprites_[static_cast<size_t>(part)];
        if (!sprite) return false;
        const auto position = sprite->GetPosition();
        const auto anchor = sprite->GetAnchorPoint();
        const auto size = sprite->GetSize();
        const float x = position.x - anchor.x * size.x;
        const float y = position.y - anchor.y * size.y;
        return pointer.x >= x && pointer.x < x + size.x && pointer.y >= y && pointer.y < y + size.y;
    };

    // 開いた入力を決定に使い回さず、初期選択は「もどる」にする。
    const bool clicked = inputManager_->IsMouseButtonTriggered(0);
    const bool openButton = inputManager_->IsKeyTriggered(DIK_F8) ||
                            inputManager_->IsGamepadButtonTriggered(XINPUT_GAMEPAD_START);
    if (!skipMenuOpen_) {
        skipButtonHovered_ = hit(SkipUiPart::kOpen);
        if (!skipSprites_[static_cast<size_t>(SkipUiPart::kPanel)] ||
            !(openButton || (clicked && skipButtonHovered_))) return false;
        skipConfirmSelected_ = false;
        skipSaveFailed_ = false;
        skipMenuOpen_ = true;
        return true;
    }

    // マウスは各ボタンを直接選び、キー・パッドは既存メニューと同じ左右・決定操作を使う。
    const auto menu = menuControls::Read(menuControls::Screen::kPause, *inputManager_);
    const bool clickCancel = clicked && hit(SkipUiPart::kCancel);
    const bool clickConfirm = clicked && hit(SkipUiPart::kConfirm);
    if (menu.Horizontal() != 0) skipConfirmSelected_ = menu.Horizontal() > 0;
    if (clickCancel || menu.cancel || inputManager_->IsKeyTriggered(DIK_BACK) || openButton ||
        (menu.confirm && !skipConfirmSelected_ && !clickConfirm)) {
        skipMenuOpen_ = false;
        return true;
    }

    // 保存に失敗した場合は確認画面に留まり、再試行と取消を受け付ける。
    if (clickConfirm || menu.confirm) {
        auto* saveData = GameDataManager::GetInstance();
        if (!saveData->IsStageCleared(-1) && !saveData->CommitStageClear(-1, {})) {
            skipSaveFailed_ = true;
            return true;
        }
        sceneManager->ChangeScene("SELECT");
    }
    return true;
}

void TutorialScene::DrawSkipUi() {
    if (isGoal_ || !SceneManager::GetInstance()->IsPlaying()) return;
    const auto draw = [&](SkipUiPart part, uint32_t texture = 0) {
        const auto& sprite = skipSprites_[static_cast<size_t>(part)];
        if (!sprite) return;
        if (texture && texture != sprite->GetTextureHandle()) sprite->SetTextureHandle(texture);
        sprite->SetVisible(true);
        sprite->Update();
        sprite->Draw();
    };
    if (!skipMenuOpen_) {
        draw(SkipUiPart::kOpen, skipButtonHovered_ ? skipButtonSelected_ : skipButtonNormal_);
        draw(SkipUiPart::kOpenLabel);
        return;
    }

    // 保存に失敗した場合もボタンの位置を保ち、説明だけをエラー表示に切り替える。
    draw(SkipUiPart::kBackdrop);
    draw(SkipUiPart::kPanel);
    draw(SkipUiPart::kTitle);
    draw(skipSaveFailed_ ? SkipUiPart::kSaveError : SkipUiPart::kDescription);
    draw(SkipUiPart::kCancel, skipConfirmSelected_ ? skipButtonNormal_ : skipButtonSelected_);
    draw(SkipUiPart::kCancelLabel);
    draw(SkipUiPart::kConfirm, skipConfirmSelected_ ? skipButtonSelected_ : skipButtonNormal_);
    draw(SkipUiPart::kConfirmLabel);
    draw(SkipUiPart::kHint);
}

void TutorialScene::DrawShadow() {
    // 動的物体の影はObjectManagerに登録された描画対象から作る。
    if (objectManager_)
        objectManager_->DrawShadow();
}

void TutorialScene::UpdateUi() {
    // 専用のUI更新はUpdate内で行うため、共通フックでは追加処理をしない。
}

void TutorialScene::DrawImGui() {
#ifdef USE_IMGUI
    // 開発時だけ進行手順と保存演出を直接確認できるようにする。
    ImGui::Text(ICON_FA_INFO_CIRCLE " Scene: Tutorial");
    ImGui::Separator();

    // 開発画面から選択画面へのボタンを押した場合、直ちに遷移を要求する。
    if (ImGui::Button("Back to SELECT")) {
        SceneManager::GetInstance()->ChangeScene("SELECT");
    }

    // 保存通知が用意され、設定欄も開いている場合は調整画面を描く。
    if (saveIndicatorOverlay_ && ImGui::CollapsingHeader("Save Indicator", ImGuiTreeNodeFlags_DefaultOpen)) {
        saveIndicatorOverlay_->DrawImGui();
    }

    // 手順管理が用意され、進行タブを開いた場合は達成状態の調整を委譲する。
    if (tutorialDirector_ && ImGui::CollapsingHeader("Tutorial Flow", ImGuiTreeNodeFlags_DefaultOpen)) {
        tutorialDirector_->DrawImGui();
    }

    // 出口案内の項目を開いた場合だけ、矢印の設定を描く。
    if (ImGui::CollapsingHeader("出口への矢印", ImGuiTreeNodeFlags_DefaultOpen)) {
        exitGuide_.DrawImGui(kExitGuideSettings);
        ImGui::TextDisabled("最後の移動課題 reach_exit_gate で表示します。");
    }

    ImGui::Separator();
    ImGui::TextDisabled("※この項目は TutorialScene::DrawImGui() で編集可能です");
#endif
}

void TutorialScene::StartBridgeDropMovie() {
    // 橋の演出開始を固定の手順IDへ変換し、進行管理に委ねる。
    if (tutorialDirector_) {
        tutorialDirector_->JumpToStep("path_reveal");
    }
}

bool TutorialScene::HandleControlsGuideOverlay(float deltaTime) {
    // 操作案内を開いている間は通常ゲーム更新を止め、案内への入力を優先する。
    if (controlsGuideOverlay_ && controlsGuideOverlay_->IsActive()) {
        controlsGuideOverlay_->SetPlayer(player_);
        controlsGuideOverlay_->Update(deltaTime);
        return true;
    }

    // ガイドを開く入力がなければ、このフレームを通常更新に返す。
    if (!IsControlsGuideOpenTriggered()) {
        return false;
    }

    // 案内の実体がある場合は、現在のプレイヤーを渡して表示を有効にする。
    if (controlsGuideOverlay_) {
        controlsGuideOverlay_->SetPlayer(player_);
        controlsGuideOverlay_->SetActive(true);
    }

    // 進行管理がある場合は、操作案内を開く課題へ通知する。
    if (tutorialDirector_) {
        tutorialDirector_->NotifyControlsGuideOpened();
    }

    return true;
}

bool TutorialScene::IsControlsGuideOpenTriggered() const {
    // エディターがキー入力を受け取っている間は案内を開かない。
    // 入力基盤の未初期化時とゴール後は、案内を開かない。
    if (!inputManager_ || isGoal_) {
        return false;
    }

#ifdef USE_IMGUI
    ImGuiIO& io = ImGui::GetIO();

    // ImGuiがキーボードや文字入力を使っている場合は、ページを開くキーを奪わない。
    if (io.WantCaptureKeyboard || io.WantTextInput) {
        return false;
    }
#endif

    return inputManager_->IsKeyTriggered(DIK_TAB) || inputManager_->IsGamepadButtonTriggered(XINPUT_GAMEPAD_BACK);
}

// 案内の調整値を読み込み、描画用モデルを非表示でシーンへ登録する。
void TutorialScene::InitializeExitGuide() {
    exitGuide_.Initialize({});
    exitGuide_.LoadSettings(kExitGuideSettings);
    exitGuideRequested_ = false;
    auto arrow = std::make_unique<engine::Object3D>();
    arrow->Initialize(GetObject3DCommon());
    arrow->SetModel(kExitGuideModel);
    arrow->SetName(kExitGuideName);
    // 実行中に作る案内をシーン保存へ混ぜず、衝突・影にも参加させない。
    arrow->SetEditorInternal(true);
    arrow->SetCollisionAttribute(0);
    arrow->SetCollisionMask(0);
    arrow->SetCastShadow(false);
    arrow->SetEnableLighting(true);
    arrow->SetEmissive(1.15f);
    arrow->SetIsVisible(false);
    AddObjectImmediate(std::move(arrow));
}

void TutorialScene::HideExitGuide() {
    // シーンに案内の実体が残っている場合だけ、表示を消す。
    if (auto* arrow = objectManager_ ? FindExitGuideArrow(*this) : nullptr)
        exitGuide_.Hide(*arrow);
}

void TutorialScene::UpdateExitGuide(float deltaTime) {
    auto* arrow = FindExitGuideArrow(*this);
    // シーンから案内の表示物を取得できなければ、追従の更新は不要。
    if (!arrow)
        return;
    auto* target = tutorialDirector_ ? tutorialDirector_->GetDirectionGuideTarget() : nullptr;
    const bool alive = player_ && player_->GetParameters() && player_->GetParameters()->hp > 0.0f;
    // 手順が誘導先を指定し、生存して動ける場面に限って矢印を出します。
    const bool requested = target && alive && !isGoal_ && !player_->IsCinematicLocked();
    // 誘導条件を満たさなくなったら表示を消し、次回の開始で状態を初期化できるようにする。
    if (!requested) {
        exitGuide_.Hide(*arrow);
        exitGuideRequested_ = false;
        return;
    }
    if (!std::isfinite(deltaTime) || deltaTime <= 0.0f) {
        // Editor停止中は非表示にし、到着済みの状態は維持する。
        exitGuide_.Hide(*arrow);
        return;
    }
    // 誘導開始時にだけ到着状態をリセットし、その後はプレイヤーの足元へ追従させます。
    if (!exitGuideRequested_) {
        exitGuide_.Reset();
        exitGuideRequested_ = true;
    }
    auto anchor = player_->GetWorldPosition();
    // ワールド境界箱の下端を矢印の基準にし、プレイヤー原点や形態の違いで足元から浮くのを防ぐ。
    anchor.y = player_->GetModelWorldAabb().min.y;
    exitGuide_.Update(*arrow, anchor, target->GetWorldPosition(), deltaTime);
}

void TutorialScene::HandleTutorialFlowCompleted() {
    // 進行達成による保存をゴール接触時の保存と重複させない。
    if (goalSavePerformed_) {
        return;
    }

    // 保存に成功した場合だけ出口を通れるようにし、失敗時は閉じたまま再試行を予約します。
    // チュートリアルはステージ番号-1で保存する。終了時にスターコインは加算しない。
    // クリアの書き込みに成功したときだけ、出口を開けて重複保存を防ぐ。
    if (GameDataManager::GetInstance()->CommitStageClear(-1, {})) {
        // 進行管理がある場合は、保存後に出口の通行許可を渡す。
        if (tutorialDirector_)
            tutorialDirector_->SetCompletionGateEnabled(true);
        // 保存通知の表示物を持つ場合は、完了の表示を開始する。
        if (saveIndicatorOverlay_)
            saveIndicatorOverlay_->Play(1.35f);
        DebugConsole::GetInstance()->AddLog("Tutorial flow completed. Exit gate unlocked.");
        goalSavePerformed_ = true;
    // 保存できない場合は、出口を閉じて再試行を予約する。
    } else {
        // 進行管理がある場合は、まだクリア扱いで出口を通過できないようにする。
        if (tutorialDirector_)
            tutorialDirector_->SetCompletionGateEnabled(false);
        goalSaveRetryTimer_ = 5.0f;

        // 最初の失敗だけ通知し、次の再試行でも同じ警告を続けて出さない。
        if (!goalSaveErrorShown_) {
            ::MessageBoxW(nullptr, L"セーブに失敗しました。保存先を確認してください。ゴールで再試行します。",
                          L"セーブエラー", MB_OK | MB_ICONERROR);
            goalSaveErrorShown_ = true;
        }
    }
}

// 現在の描画カメラで、対象の境界が視錐台に入るかを共通処理へ問い合わせる。
bool TutorialScene::IsVisible(engine::Object3D* obj) {
    return IsSceneObjectVisibleByFrustum(obj, CameraManager::GetInstance()->GetActiveCamera());
}
