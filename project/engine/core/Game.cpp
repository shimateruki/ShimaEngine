#ifdef USE_IMGUI
#include <cstdint>
#include "engine/editor/preview/SelectedActorSession.h"
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#define NOMINMAX
#include "Game.h"

#include "engine/editor/graphics/camera/CameraEditor.h"
#include "AudioPlayer.h"
#include "CameraManager.h"
#include "DebrisEffectManager.h"
#include "DebugConsole.h"
#include "DirectXCommon.h"
#include "EditorCommandRegistry.h"
#include "game/system/settings/GameAudioSettings.h"
#include "game/system/progress/GameDataManager.h"
#include "game/system/settings/GameSettingsManager.h"
#include "InputManager.h"
#include "KeyConfig.h"
#include "LightManager.h"
#include "MeshEffectManager.h"
#include "ModelManager.h"
#include "Object3D.h"
#include "PostEffect.h"
#include "ProfilerManager.h"
#include "RenderStats.h"
#include "SceneFactory.h"
#include "game/scene/RegisterGameScenes.h"
#ifdef USE_IMGUI
#include "game/editor/RegisterGameEditorFactories.h"
#include "game/editor/BuiltInCreatePresetRegistry.h"
#include "game/actor/gimmick/combat/GimmickFireCannon.h"
#include "game/system/combat/BulletManager.h"
#endif
#include "SceneManager.h"
#include "engine/system/scene/LoadingProfiler.h"
#include "SrvManager.h"
#include "game/system/progress/StageManager.h"
#include "TextureManager.h"
#include "VfxSequencer.h"
#include "WinApp.h"
#include "GpuParticleManager.h"
#include "engine/graphics/postprocess/Fade.h"
#include "engine/graphics/core/ColorSpace.h"
#include "engine/graphics/3d/FoliageInteraction.h"
#include "json.hpp"

#include <Windows.h>
#include <array>
#include <chrono>
#include <fstream>
#ifdef USE_IMGUI
#include "tests/SceneTransitionRuntimeProbe.h"
#endif

namespace {
constexpr float kDeltaTimeClamp = 0.1f;
constexpr float kFixedDeltaTime = 1.0f / 60.0f;
constexpr std::int32_t kMaxFixedStepsPerFrame = 4;
constexpr std::int32_t kHistorySampleCount = 120;

// Post Effect設定から線形色空間の選択を読み、読込失敗時は現在値を維持します。
void LoadColorWorkflowSetting() {
	std::ifstream file("Resources/json/post_effect.json");

	// ファイルを開けない場合は、現在の色空間設定を維持する。
	if (!file.is_open()) {
		return;
	}

	try {
		nlohmann::json settings;
		file >> settings;

		// 設定に線形処理の項目があるときだけ、描画側の共通設定へ反映する。
		if (settings.contains("linearWorkflowEnabled")) {
			engine::colorSpace::WorkflowSettings::GetInstance().SetLinearWorkflowEnabled(
				settings["linearWorkflowEnabled"].get<bool>());
		}
	}
	catch (...) {
		// JSON解析や型変換に失敗した場合は、読込前の色空間設定を維持する。
	}
}

#ifdef USE_IMGUI
// 現在シーンの描画用カメラ情報を再反映し、プレビュー描画後の状態ずれを防ぐ。
void RefreshSceneRenderCameraData(SceneManager* sceneManager) {
	// プレビューのカメラ更新が通常シーンの描画定数へ残らないよう再反映する。
	// シーン管理が用意されていなければ、通常のカメラ情報を取得できない。
	if (!sceneManager) {
		return;
	}

	BaseScene* currentScene = sceneManager->GetCurrentScene();

	// 現在のシーンがなければ、描画定数を戻す相手がいない。
	if (!currentScene) {
		return;
	}

	currentScene->RefreshRenderCameraData();
}
#endif
}

void Game::Initialize() {
	// エンジン基盤、シーン、ポストエフェクト、エディタ、初期プレイ状態を順番に準備する。
	StartupProfileScope startupScope("Game::Initialize");
	{
		StartupProfileScope phase("engine services");
		InitializeEngineServices();
	}

	{
		StartupProfileScope phase("post effects");
		InitializePostProcess();
	}

	{
		StartupProfileScope phase("first scene");
		InitializeScene();
	}

	{
		StartupProfileScope phase("editor tools");
		InitializeEditorTools();
	}

	// 基盤・シーン・各ツールが揃ってから、編集か通常再生かの入力状態を決める。
	ConfigureInitialPlayState();

	CameraEditor::GetInstance()->Initialize();
	GetDirectX()->CreateRenderTexture();
	lastTime_ = std::chrono::high_resolution_clock::now();
	LogStartupProfileCheckpoint("Game::Initialize complete");
}

void Game::InitializeEngineServices() {
	// ゲーム全体で使うマネージャやアセットを初期化し、シーン生成の準備を行う。
	{
		StartupProfileScope phase("Framework::Initialize call");
		engine::Framework::Initialize();
	}

	// ゲーム内の進行、セーブ、操作と音量設定を、最初のシーン生成より先に準備する。
	ProfilerManager::GetInstance()->Initialize();
	StageManager::GetInstance()->Initialize();
	GameDataManager::GetInstance()->Initialize();
	GameSettingsManager::GetInstance()->Initialize();
	GameAudioSettings::GetInstance()->Initialize();

	// ゲーム固有のシーンを登録し、共通描画資源をSceneManagerへ渡す。
	auto gameSceneFactory = std::make_unique<SceneFactory>();
	RegisterGameScenes(*gameSceneFactory);
#ifdef USE_IMGUI
	RegisterGameEditorFactories();
#endif
	sceneFactory_ = std::move(gameSceneFactory);
	sceneManager_ = std::make_unique<SceneManager>();
	sceneManager_->SetRenderCommonResources(GetRenderCommonResources());
}

void Game::InitializeScene() {
	// 開始シーン名を決め、SceneManagerへ初期シーンを生成させる。
	currentSceneName_ = ResolveStartSceneName();
	initialSceneOverridesPending_ = true;
	sceneManager_->Initialize(sceneFactory_.get(), currentSceneName_);
	ApplyInitialSceneOverrides();
}

std::string Game::ResolveStartSceneName() const {
	// ビルド設定やエディタの最終シーン情報から、起動時に開くシーン名を決める。
#ifdef USE_IMGUI
	// シーン遷移の検証起動では履歴より優先してTITLEを開く。
	if (sceneTransitionRuntimeProbe::Enabled())
		return "TITLE";
#endif
	std::string startScene = "TITLE";

#if defined(USE_IMGUI) && !defined(DD)
	// 編集時の起動で、シーン管理から最後のシーンを問い合わせられる場合だけ復元を試す。
	if (sceneManager_) {
		std::string lastScene = sceneManager_->LoadLastSceneName();

		// 最後のシーン名が空でなければ初期値のTITLEを置き換える。
		if (!lastScene.empty()) {
			startScene = lastScene;
		}
	}
#endif

	return startScene;
}

void Game::ApplyInitialSceneOverrides() {
	// 起動直後に必要なシーン内オブジェクトの初期上書きを適用する。
	// 上書き済み、管理が未準備、または切替中の場合は上書きを実行しない。
	if (!initialSceneOverridesPending_ || !sceneManager_ || sceneManager_->IsTransitioning()) {
		return;
	}

	BaseScene* currentScene = sceneManager_->GetCurrentScene();

	// 読み込みが完了して現在のシーンを取得できるまで、上書きを保留する。
	if (!currentScene) {
		return;
	}

	for (auto& object : currentScene->GetObjects()) {
		// 名前がSkydomeの実体を最初に見つけた場合だけ、照明方式を切り替える。
		if (object && object->GetName() == "Skydome") {
			object->SetSelectedLighting(0);
#ifdef USE_IMGUI
			DebugConsole::GetInstance()->AddLog("Skydome settings have been overwritten.");
#endif
			break;
		}
	}

	initialSceneOverridesPending_ = false;
}

void Game::InitializePostProcess() {
	// ポストエフェクト、LUT、フェード、キー設定を初期化する。
	LoadColorWorkflowSetting();
	engine::LightManager::GetInstance()->ApplySceneClearColor();
	// 色空間とクリア色を先に読み、後処理の作成時点から同じ描画設定を使う。
	PostEffect::GetInstance()->Initialize(GetDirectX());
	PostEffect::GetInstance()->SetLUTTexturePath(
		"Resources/texture/lut/soft_adventure_lut.png");

	Fade::GetInstance()->Initialize();
#ifndef USE_IMGUI
	PostEffect::GetInstance()->ResetToNeutral();
#endif
	KeyConfig::GetInstance()->Initialize();
}

void Game::InitializeEditorTools() {
	// USE_IMGUI時に、ゲームビューや各種エディタを束ねるコントローラを初期化する。
#ifdef USE_IMGUI
	editorController_ = std::make_unique<GameEditorController>();
	editorController_->Initialize(sceneManager_.get(), GetDirectX());

	// 現在のシーンを取得できる場合は、その描画資源を使う編集用生成項目を登録する。
	if (sceneManager_ && sceneManager_->GetCurrentScene()) {
		BuiltInCreatePresetRegistry::EnsureRegistered(sceneManager_->GetCurrentScene()->GetObject3DCommon());
	}
#endif
}

void Game::ConfigureInitialPlayState() {
	// エディタ起動か通常実行かに応じて、再生状態とカメラ入力モードを設定する。
#ifdef USE_IMGUI
	isPlaying_ = false;
	CameraEditor::GetInstance()->SetMode(CameraEditor::Mode::kEditor);
#else
	isPlaying_ = true;
	WinApp::SetCursorVisibility(false);
	GetWindow()->SetCursorClipping(true);
	GetWindow()->SetCursorLocked(true);
	CameraEditor::GetInstance()->SetMode(CameraEditor::Mode::kGame);
#endif
}

void Game::Finalize() {
	// シーンを閉じる前に保留中の進行データを確定し、基盤側の終了へ渡す。
	GameDataManager::GetInstance()->FlushPendingSave();

	// シーン管理を作成できていた場合は、そのシーンの終了処理を先に実行する。
	if (sceneManager_) {
		sceneManager_->Finalize();
	}

	DebrisEffectManager::GetInstance()->Clear();

#ifdef USE_IMGUI
	// 編集ツールを作成できていた場合は基盤が生きている間に終了して解放する。
	if (editorController_) {
		editorController_->Finalize();
		editorController_.reset();
	}
#endif

	engine::Framework::Finalize();
}

void Game::Update() {
	// 入力、終了要求、リサイズ、エディタ、ゲームシステムを1フレーム分更新する。
	engine::InputManager::GetInstance()->Update();

	// Escapeが押されたフレームで、ウィンドウ終了と同じ共通要求を登録する。
	if (engine::InputManager::GetInstance()->IsKeyTriggered(DIK_ESCAPE)) {
		WinApp::RequestClose();
	}

	// ウィンドウとEscapeからの要求を一本化し、ImGuiが使える場合は保存確認をエディターに渡す。
	if (WinApp::ConsumeCloseRequest()) {
#ifdef USE_IMGUI
		// 保存の確認を担当するエディターがいれば、ウィンドウを閉じる前に要求を渡す。
		if (editorController_) {
			editorController_->RequestExit();
		}
		// エディターが未生成の場合は、その確認を経ずウィンドウを閉じる。
		else {
			WinApp::CloseNow();
		}
#else
		WinApp::CloseNow();
#endif
	}

	// 描画基盤が準備できた場合だけ、保留されていたウィンドウサイズを反映する。
	if (GetDirectX()) {
		GetDirectX()->ProcessPendingResize();
	}

	// 音声回収・振動・ヒットストップの時計は実時間で進めてから、Scene用の時間に換算する。
	AudioEventSystem::GetInstance()->Update();
	float deltaTime = CalculateDeltaTime();
	engine::InputManager::GetInstance()->UpdateRumble(deltaTime);
	VfxSequencer::UpdateFeedbackRuntime(deltaTime);
	const bool sceneTransitioning = sceneManager_ && sceneManager_->IsTransitioning();

	// シーン切替が進んでいないフレームだけ、VFX管理に新しいフレームの境界を知らせる。
	if (!sceneTransitioning) {
		GpuParticleManager::GetInstance()->BeginFrame();
		MeshEffectManager::GetInstance()->BeginFrame();
	}

	// 再生中はプレビュー等で指定したGPU粒子の倍率を通常の1倍に戻す。
	if (isPlaying_) {
		GpuParticleManager::GetInstance()->SetTimeScale(1.0f);
	}

#ifdef USE_IMGUI
    {
        // シーン更新の計測より前に走るEditor処理を、個別のCPU時間として残す。
        PROFILE_SCOPE("Editor更新");
        UpdateEditorFrame(deltaTime);
    }
	// 攻撃またはいかだの検証が再生を要求したら、編集停止を解除してシーンも同時に進める。
	if (attackLifecycleRuntimeProbe::WantsPlay(sceneManager_.get()) || raftRuntimeProbe::WantsPlay(sceneManager_.get())) {
		isPlaying_ = true;
		sceneManager_->SetIsPlaying(true);
	}
#endif

#ifdef USE_IMGUI
    // ゴール・スターコイン・敵弾の検証が有効な場合は、ゲーム側の再生を許可する。
    if (goalPresentationRuntimeProbe::Enabled() || starCoinRuntimeProbe::Enabled() || enemyProjectileRuntimeProbe::Enabled()) { isPlaying_ = true; }
    // ゲート演出検証では要求に応じてゲーム・シーン・カメラの操作状態を揃える。
    if (gateEntryRuntimeProbe::Enabled()) {
        isPlaying_ = gateEntryRuntimeProbe::WantsPlay();
        sceneManager_->SetIsPlaying(isPlaying_);
        CameraEditor::GetInstance()->SetMode(isPlaying_ ? CameraEditor::Mode::kGame : CameraEditor::Mode::kEditor);
    }
#endif
#ifdef USE_IMGUI
    // チュートリアル退出検証では記録の準備状態に合わせ、ゲームとシーンの再生を揃える。
    if (tutorialExitGuideRuntimeProbe::Enabled()) {
        isPlaying_ = tutorialExitGuideRuntimeProbe::WantsPlay();
        sceneManager_->SetIsPlaying(isPlaying_);
    }
#endif
	const float feedbackTimeScale = VfxSequencer::GetGameplayTimeScale();
	// 実時間からゲーム時間を作る。停止中は0秒、再生中は編集側の倍率とヒットストップ側の倍率を掛ける。
	float finalDeltaTime = isPlaying_ ? deltaTime * timeScale_ * feedbackTimeScale : 0.0f;
#ifdef USE_IMGUI
	// 攻撃またはいかだの検証中は、入力時間にかかわらず一更新を1/60秒として進める。
	if (attackLifecycleRuntimeProbe::WantsPlay(sceneManager_.get()) || raftRuntimeProbe::WantsPlay(sceneManager_.get()))
		finalDeltaTime = 1.0f / 60.0f;
	// いかだ検証が一時停止を要求している区間では、その固定時間も0にする。
	if (raftRuntimeProbe::WantsPlay(sceneManager_.get()) && GetTickCount64() < raftRuntimeProbe::pauseUntil)
		finalDeltaTime = 0.0f;
	// エディターのシミュレーション停止が有効なら、ゲーム内で経過する時間を0にする。
	if (editorController_ && editorController_->IsSimulationPaused())
		finalDeltaTime = 0.0f;
#endif
	UpdateGameSystems(deltaTime, finalDeltaTime);

	// コインの連続取得を短時間まとめ、シーン切替中は保留分を確定する。
	GameDataManager::GetInstance()->TickAutosave(deltaTime);

	// シーンを切り替える間に未保存の進行が残らないよう、保留中の保存を確定する。
	if (sceneManager_ && sceneManager_->IsTransitioning()) {
		GameDataManager::GetInstance()->FlushPendingSave();
	}

	ApplyInitialSceneOverrides();
}

float Game::CalculateDeltaTime() {
	// 前フレームからの経過時間を求め、極端に大きい値は固定値へ丸める。
	auto currentTime = std::chrono::high_resolution_clock::now();
	// 秒単位へ変換し、ウィンドウ移動などで一時的に止まった後の大きな時間は固定刻みへ置き換える。
	std::chrono::duration<float> duration = currentTime - lastTime_;
	float deltaTime = duration.count() > kDeltaTimeClamp ? kFixedDeltaTime : duration.count();
	lastTime_ = currentTime;
	return deltaTime;
}

void Game::UpdateEditorFrame(float deltaTime) {
	// エディタUI、ゲームビュー、ツールウィンドウ、カメラ入力状態を更新する。
#ifdef USE_IMGUI
	// ツール群を管理するコントローラーがなければ、エディター入力と描画を更新しない。
	if (!editorController_) {
		return;
	}

	// F10を新たに押したフレームは、ポートフォリオ表示の切替を共有コマンドへ渡す。
	if (engine::InputManager::GetInstance()->IsKeyTriggered(DIK_F10)) {
		editorController_->ExecuteEditorCommand(editorCommandId::kViewPortfolio);
	}

	// 撮影表示では通常のメニューバーを組み立てず、ツール更新と入力状態だけを維持する。
	if (editorController_->IsPortfolioCaptureMode()) {
		editorController_->UpdateTools(deltaTime, isPlaying_, timeScale_);
		editorFrameState_ = {};
		editorController_->ApplyCameraInputState(editorFrameState_, isPlaying_);
		return;
	}

	editorController_->BeginFrame();
	editorFrameState_ = editorController_->DrawGameView(sceneManager_.get(), isPlaying_);

	// 有効なシーン名が取れた場合に、メニューやタイトルに使う現在名を更新する。
	if (sceneManager_ && !sceneManager_->GetCurrentSceneName().empty()) {
		currentSceneName_ = sceneManager_->GetCurrentSceneName();
	}

	editorController_->DrawMainMenuBar(sceneManager_.get(), isPlaying_, currentSceneName_);
	editorController_->UpdateTools(deltaTime, isPlaying_, timeScale_);
	editorController_->DrawToolWindows(
		timeScale_,
		sceneUpdateTimeMs_,
		cpuCmdTimeMs_,
		drawTimeMs_,
		updateTimeHistory_,
		drawTimeHistory_,
		timeHistoryIndex_);
	// シーン切替中でなければ、ゲームビューの操作状況に合わせてカメラ入力を更新する。
	if (!sceneManager_ || !sceneManager_->IsTransitioning()) {
		editorController_->ApplyCameraInputState(editorFrameState_, isPlaying_);
	}
	// 切替中は古いゲームビューの入力状態を次のフレームへ持ち越さない。
	else {
		editorFrameState_ = {};
	}
#else
	(void)deltaTime;
#endif
}

void Game::UpdateGameSystems(float deltaTime, float finalDeltaTime) {
	// シーン、ライト、エフェクト、フェード、ポストエフェクト時刻を更新する。
	auto startUpdate = std::chrono::high_resolution_clock::now();
	bool simulationPaused = false;
	const bool sceneTransitioning = sceneManager_ && sceneManager_->IsTransitioning();
#ifdef USE_IMGUI
	// エディターが存在する場合だけ、物理や経過秒を止める設定を取得する。
	if (editorController_) {
		simulationPaused = editorController_->IsSimulationPaused();
	}
#endif

	// 固定刻みの物理更新を先に消化し、そのあと現在のSceneへフレーム分の更新を渡す。
	RunFixedUpdates(finalDeltaTime, simulationPaused, sceneTransitioning);

	{
		PROFILE_SCOPE("シーン");

		// シーン管理を利用できる場合は、次の判定でどの経過秒を渡せるか決める。
		if (sceneManager_) {
			// 遷移画面は停止やヒットストップから独立した時間で進むため、元の経過秒を使う。
			const float sceneDeltaTime = sceneManager_->IsTransitioning() ? deltaTime : finalDeltaTime;

			// 通常は停止中の更新を省くが、シーン遷移中は停止中でも切替処理を進める。
			if (!simulationPaused || sceneManager_->IsTransitioning()) {
				sceneManager_->Update(sceneDeltaTime);
			}
		}
	}
#ifdef USE_IMGUI
    // 停止中の編集画面では、一部の表示と砲台のプレビューだけを実時間で進める。選択アクター専用の再生中はその処理に任せる。
    if (!isPlaying_ && !simulationPaused && !sceneTransitioning && sceneManager_ &&
        !(SelectedActorSession::Current() && SelectedActorSession::Current()->IsActive())) {
        // 現在のシーンが取得できた場合だけ、プレビュー対象のオブジェクトを探す。
        if (BaseScene* scene = sceneManager_->GetCurrentScene()) {
            for (auto& object : scene->GetObjects()) {
                // ゲーム時間が0で、再生から取り除かれていない砲台の実体だけを、実時間で更新する。
                if (auto* cannon = finalDeltaTime == 0.0f && object && !object->IsReplayRemoved()
                    ? dynamic_cast<GimmickFireCannon*>(object.get()) : nullptr) {
                    cannon->Update(deltaTime);
                }

                // 水材質と描画器を持つ実体には、実時間による波の時計を渡す。
                if (object && object->GetMaterialType() == 8 && object->GetMeshRenderer()) {
                    object->GetMeshRenderer()->AdvanceWaterAnimation(deltaTime);
                }

                // 材質番号30または31を使う実体の描画器には、材質の時計だけを進める。
                if (object && (object->GetMaterialType() == 30 || object->GetMaterialType() == 31)
                    && object->GetMeshRenderer()) {
                    object->GetMeshRenderer()->AdvanceMaterialTime(deltaTime);
                }
            }
            // 通常のゲーム時間が止まっている場合は、テスト弾のプレビューを実時間で進める。
            if (finalDeltaTime == 0.0f) {
                BulletManager::GetInstance()->UpdatePreview(deltaTime);
                // プレビュー弾が残っている場合に限り、同じフレーム内の粒子も専用経路で進める。
                if (BulletManager::GetInstance()->HasPreviewBullets()) {
                    // シーン側の時間0更新後でも、このプレビューだけは粒子の寿命を進めます。
                    GpuParticleManager::GetInstance()->UpdateEditorPreviewStep(deltaTime);
                }
            }
        }
    }
#endif
#ifdef USE_IMGUI
    // 編集コントローラーがあれば、選択アクターなどのツールの再生を実時間で更新する。
    if (editorController_) editorController_->UpdateActorTools(deltaTime);
#endif
    // シーン変更時は草への接触履歴を消し、現プレイヤーだけ反映する。
    {
        static const BaseScene* sFoliageScene = nullptr;
        BaseScene* scene = sceneManager_ ? sceneManager_->GetCurrentScene() : nullptr;

        // シーンが変わったか切替中になったら、前のシーンの草の押し分け履歴を消す。
        if (scene != sFoliageScene || sceneTransitioning) {
            foliageInteraction::Reset();
            sFoliageScene = scene;
        }

        bool found = false;

        // 切替が落ち着いて現在のシーンを参照できる場合だけ、草と接触する位置を求める。
        if (scene && !sceneTransitioning) {
            for (auto& object : scene->GetObjects()) {
                // 有効なPlayerを見つけたらその一体のワールド座標を草のシェーダー用に渡す。
                if (object && object->GetClassName() == "Player") {
                    const auto pos = object->GetWorldPosition();
                    // 停止中も現在位置を渡す。経過秒は0とし、倒れ跡の時間による減衰を止める。
                    float foliageDeltaTime = 0.0f;
                    if (!simulationPaused) {
                        foliageDeltaTime = isPlaying_ ? finalDeltaTime : deltaTime;
                    }
                    foliageInteraction::Update(pos.x, pos.y, pos.z, foliageDeltaTime);
                    found = true;
                    break;
                }
            }
        }

        // 対象のPlayerがいないフレームでは、古い接触点が残らないようリセットする。
        if (!found)
            foliageInteraction::Reset();
    }

	// 遷移中は通常のシーン照明を更新せず、それ以外で現在のライトを反映する。
	if (!sceneTransitioning) {
		PROFILE_SCOPE("ライト");
		engine::LightManager::GetInstance()->Update();
	}

	// 遷移中を除き、単発VFXと粒子を進める。再生中の経過秒は停止やヒットストップを反映する。
	if (!sceneTransitioning) {
		PROFILE_SCOPE("パーティクル");
		const float effectDeltaTime = isPlaying_ && !simulationPaused ? finalDeltaTime : 0.0f;
		VfxSequencer::UpdateOneShots(effectDeltaTime);
		GpuParticleManager::GetInstance()->Update(effectDeltaTime);
#ifndef USE_IMGUI
		MeshEffectManager::GetInstance()->Update(effectDeltaTime);
#endif
	}

	// 通常シーンが利用できる間にだけ、破片の共通資源と再生を更新する。
	if (sceneManager_ && !sceneManager_->IsTransitioning()) {
		// 現在のシーンを取得できれば、破片が借りる共通資源をそのシーンに揃える。
		if (BaseScene* currentScene = sceneManager_->GetCurrentScene()) {
			DebrisEffectManager::GetInstance()->Initialize(currentScene->GetObject3DCommon());
		}

		#ifdef USE_IMGUI
        // 選択アクターの専用セッションが動いている場合は、破片の時計をそちらに任せる。
        if (!(SelectedActorSession::Current() && SelectedActorSession::Current()->IsActive()))
#endif
        DebrisEffectManager::GetInstance()->Update(simulationPaused ? 0.0f : deltaTime);
	}

	// 別スレッドでシーン初期化中のときはフェードの時刻を進めない。
	if (!sceneManager_ || !sceneManager_->IsSceneInitializingAsync()) {
		PROFILE_SCOPE("フェード");
		Fade::GetInstance()->Update(simulationPaused ? 0.0f : deltaTime);
	}

	// 後処理の見た目の時計は実時間で進めるが、シミュレーション停止時は保持する。
	PostEffect::GetInstance()->GetParams()->time += simulationPaused ? 0.0f : deltaTime;

	// 現在の管理に同じ再生フラグを渡し、次のシーン処理へ反映する。
	if (sceneManager_) {
		sceneManager_->SetIsPlaying(isPlaying_);
	}

#ifdef USE_IMGUI
	// ツールが使え、シーン遷移中でない場合に限り撮影や編集用のカメラを適用する。
	if (editorController_ && (!sceneManager_ || !sceneManager_->IsTransitioning())) {
		editorController_->ApplyCameraOverrides();
	}
#endif

	RecordUpdateProfile(startUpdate);
}

void Game::RunFixedUpdates(float finalDeltaTime, bool simulationPaused, bool sceneTransitioning) {
	// 停止・切替中の余った時間は破棄し、再開したシーンへ前のシーンの固定更新を持ち越さない。
	if (!sceneManager_ || simulationPaused || sceneTransitioning || finalDeltaTime <= 0.0f) {
		fixedUpdateAccumulator_ = 0.0f;
		return;
	}

	BaseScene* currentScene = sceneManager_->GetCurrentScene();

	// 管理は存在しても現在のシーンがなければ、累積時間を破棄し物理更新を呼ばない。
	if (!currentScene) {
		fixedUpdateAccumulator_ = 0.0f;
		return;
	}

	// ゲーム時間を秒で貯め、固定刻みを一回分確保できるたびに物理更新へ渡す。
	fixedUpdateAccumulator_ += (std::min)(finalDeltaTime, kDeltaTimeClamp);
	std::int32_t stepCount = 0;

	while (fixedUpdateAccumulator_ >= kFixedDeltaTime && stepCount < kMaxFixedStepsPerFrame) {
		currentScene->FixedUpdate(kFixedDeltaTime);
		fixedUpdateAccumulator_ -= kFixedDeltaTime;
		++stepCount;
	}

	// 長い停止後に古い時間を持ち越さず、次フレームの処理急増を防ぎます。
	if (stepCount == kMaxFixedStepsPerFrame) {
		fixedUpdateAccumulator_ = (std::min)(fixedUpdateAccumulator_, kFixedDeltaTime);
	}
}

void Game::RecordUpdateProfile(const std::chrono::high_resolution_clock::time_point& startUpdate) {
	// 更新処理にかかったCPU時間を履歴とProfilerManagerへ記録する。
	auto endUpdate = std::chrono::high_resolution_clock::now();
	std::chrono::duration<float, std::milli> updateDuration = endUpdate - startUpdate;

	sceneUpdateTimeMs_ = updateDuration.count();
	updateTimeHistory_[timeHistoryIndex_] = sceneUpdateTimeMs_;
	timeHistoryIndex_ = (timeHistoryIndex_ + 1) % kHistorySampleCount;

	ProfilerManager::GetInstance()->RecordCpuTime("更新処理", sceneUpdateTimeMs_);
}

void Game::Draw() {
	// GPUプロファイルを読み戻し、エディタまたは通常実行の描画フローを実行する。
	PostEffect* postEffect = PostEffect::GetInstance();
	// 描画指示を記録する前に、GPUから回収できる計測結果を更新する。
	GetDirectX()->ReadAllGpuProfiles();
#ifdef USE_IMGUI
	// 環境変数が指定されたときだけ描画区間の実測値をCSVに記録します。
	static bool sGpuTraceInitialized = false;
	static std::ofstream sGpuTrace;
	static std::uint32_t sGpuTraceFrame = 0;

	// 最初の描画時にだけ診断用の出力先を環境変数から取得する。
	if (!sGpuTraceInitialized) {
		sGpuTraceInitialized = true;
		char tracePath[1024] = {};
		const DWORD pathLength = GetEnvironmentVariableA("CG2_GPU_PROFILE_CSV", tracePath, sizeof(tracePath));

		// 有効な長さのパスが指定された場合だけ、診断用のCSVを開く。
		if (pathLength > 0 && pathLength < sizeof(tracePath)) {
			sGpuTrace.open(tracePath, std::ios::out | std::ios::trunc);

			// 出力先を開けた場合に、フレーム・描画区間ごとの列名を書き出す。
			if (sGpuTrace) {
				sGpuTrace << "frame,total_ms,thumbnail_ms,thumbnail_cpu_ms,shadow_ms,scene_ms,game_ui_ms,debug_ms,post_ms,editor_ui_ms\n";
			}
		}
	}

	// CSVが使用可能な場合は30描画ごとに、取得済みの区間計測を一行書き出す。
	if (sGpuTrace && ++sGpuTraceFrame % 30 == 0) {
		const auto* profiler = ProfilerManager::GetInstance();
		sGpuTrace << sGpuTraceFrame << ','
			<< profiler->GetLatestGpuTime("Total") << ','
			<< profiler->GetLatestGpuTime("プリセット撮影") << ','
			<< profiler->GetLatestCpuTime("プリセット撮影 CPU") << ','
			<< profiler->GetLatestGpuTime("影描画") << ','
			<< profiler->GetLatestGpuTime("  3Dシーン") << ','
			<< profiler->GetLatestGpuTime("  ゲームUI") << ','
			<< profiler->GetLatestGpuTime("  デバッグ") << ','
			<< profiler->GetLatestGpuTime("後処理") << ','
			<< profiler->GetLatestGpuTime("エディタUI") << '\n';
		sGpuTrace.flush();
	}
#endif
	engine::RenderStats* renderStats = engine::RenderStats::GetInstance();
	renderStats->BeginFrame();

	// 遷移中以外は、有効な点光源とスポットライトを描画統計に載せる。
	if (!sceneManager_ || !sceneManager_->IsTransitioning()) {
		renderStats->SetActiveLightCounts(
			engine::LightManager::GetInstance()->GetActivePointLightCount(),
			engine::LightManager::GetInstance()->GetActiveSpotLightCount());
	}
	// 遷移中は通常のシーンのライトを統計へ含めず、両方とも0として扱う。
	else {
		renderStats->SetActiveLightCounts(0, 0);
	}

	auto startDraw = std::chrono::high_resolution_clock::now();

#ifdef USE_IMGUI
    // チュートリアル退出検証が有効な場合は、描画前に検証用の視点を確定する。
    if (tutorialExitGuideRuntimeProbe::Enabled())
        tutorialExitGuideRuntimeProbe::PrepareCaptureCamera(sceneManager_.get());
	DrawEditorFrame(postEffect);
#else
	DrawRuntimeFrame(postEffect);
#endif

	renderStats->EndFrame();
	RecordDrawProfile(startDraw);
	RecordFixedFpsProfile();
#ifdef USE_IMGUI
	sceneTransitionRuntimeProbe::AfterDraw(sceneManager_.get(), editorController_ ? editorController_->GetDebugEditor() : nullptr);
#endif
}

void Game::DrawEditorFrame(PostEffect* postEffect) {
	// エディタ用にシーン、ポストエフェクト、カメラプレビュー、ImGuiを描画する。
#ifdef USE_IMGUI
	// ポートフォリオ表示中はエディターUIを重ねず、通常画面を作って終了する。
	if (editorController_ && editorController_->IsPortfolioCaptureMode()) {
		DrawRuntimeFrame(postEffect);
		return;
	}

	// 先にゲーム画面を別のテクスチャで完成させ、バックバッファではその画像とEditor UIを合成する。
	DrawSceneToRenderTexture(true);
	ApplyPostEffectPipeline(postEffect, true);

	// シーンの遷移中はプレビュー用の別カメラを描画しない。
	if (!sceneManager_ || !sceneManager_->IsTransitioning()) {
		DrawCameraEditorPreview(postEffect);
	}

	GetDirectX()->StartGpuProfile("エディタUI");
	GetDirectX()->PreDrawBackBuffer();

	// エディターが利用可能なら、ゲーム映像を表示するUIをバックバッファへ重ねる。
	if (editorController_) {
		editorController_->DrawBackBufferUi();
	}

	GetDirectX()->EndGpuProfile("エディタUI");

	GetDirectX()->EndGpuProfile("Total");
	prePostDrawTime_ = std::chrono::high_resolution_clock::now();
	GetDirectX()->PostDraw();

	// エディターが存在する場合は、描画確定後に予約されている画像を出力しフレームを閉じる。
	if (editorController_) {
		editorController_->ExportCapturedHudPortraits();
		editorController_->EndFrame();
	}
#else
	(void)postEffect;
#endif
}

void Game::DrawRuntimeFrame(PostEffect* postEffect) {
	// 通常実行用にシーンとポストエフェクトを描画し、バックバッファへ表示する。
	DrawSceneToRenderTexture(false);
	ApplyPostEffectPipeline(postEffect, false);

	GetDirectX()->EndGpuProfile("Total");
	prePostDrawTime_ = std::chrono::high_resolution_clock::now();
	GetDirectX()->PostDraw();
}

void Game::DrawSceneToRenderTexture(bool editorMode) {
	// 影、3Dシーン、UI、フェードをレンダーテクスチャへまとめて描画する。
	GetDirectX()->PreDrawRenderTexture();

	// サムネイル撮影も同じフレームのGPU仕事なので、Total計測に含めます。
	GetDirectX()->StartGpuProfile("Total");

#ifdef USE_IMGUI
	// 編集描画で撮影ツールがあり、シーン遷移中でない場合だけ、予約サムネイルを処理する。
	if (editorMode && editorController_ && (!sceneManager_ || !sceneManager_->IsTransitioning())) {
		GetDirectX()->StartGpuProfile("プリセット撮影");
		const auto thumbnailStart = std::chrono::high_resolution_clock::now();
		editorController_->CapturePendingThumbnails();
		const auto thumbnailEnd = std::chrono::high_resolution_clock::now();
		ProfilerManager::GetInstance()->RecordCpuTime("プリセット撮影 CPU",
			std::chrono::duration<float, std::milli>(thumbnailEnd - thumbnailStart).count());
		GetDirectX()->EndGpuProfile("プリセット撮影");
	}
#else
	(void)editorMode;
#endif
	GetDirectX()->PreDrawShadow();
	engine::SrvManager::GetInstance()->SetDescriptorHeaps(GetDirectX()->GetCommandList());

	GetDirectX()->StartGpuProfile("影描画");
	{
		engine::ScopedRenderPass renderPass(engine::RenderPass::kShadow);

		// シーン管理を参照できる場合は、影を落とすシーン内の形状を描く。
		if (sceneManager_) {
			sceneManager_->DrawShadow();
		}
	}

	GetDirectX()->EndGpuProfile("影描画");
	// 影マップの描画を終了し、続く3D描画では結果を参照する側へ切り替える。
	GetDirectX()->PostDrawShadow();

	GetDirectX()->StartGpuProfile("メイン描画");

#ifdef USE_IMGUI
	// エディター表示では3D、ゲームUI、操作表示を独立したパスに分ける。
	if (editorMode) {
		GetDirectX()->StartGpuProfile("  3Dシーン");
		{
			engine::ScopedRenderPass renderPass(engine::RenderPass::kMainScene);
			const bool sceneTransitioning = sceneManager_ && sceneManager_->IsTransitioning();
			ID3D12Resource* pointLight = sceneTransitioning ? nullptr : engine::LightManager::GetInstance()->GetPointLightResource();
			ID3D12Resource* spotLight = sceneTransitioning ? nullptr : engine::LightManager::GetInstance()->GetSpotLightResource();

			// シーン管理を使える場合に3Dシーンの描画を委譲する。
			if (sceneManager_) {
				sceneManager_->Draw();
			}

			// ツールが利用でき通常のシーン描画中であれば、編集中の対象を描く。
			if (editorController_ && !sceneTransitioning) {
				editorController_->DrawScenePreview(pointLight, spotLight);
			}

			// 通常シーンの描画中は、破片とメッシュエフェクトを3D描画へ追加する。
			if (!sceneManager_ || !sceneManager_->IsTransitioning()) {
				DebrisEffectManager::GetInstance()->Draw(pointLight, spotLight);

				// 描画済みの背景色を読むエフェクトがある場合にだけ、その手前までの画面をコピーする。
				if (MeshEffectManager::GetInstance()->RequiresSceneColorCopy()) {
					GetDirectX()->UpdateGrabTexture();
				}

				MeshEffectManager::GetInstance()->Draw(pointLight, spotLight);
			}
		}

		GetDirectX()->EndGpuProfile("  3Dシーン");

		GetDirectX()->StartGpuProfile("  ゲームUI");
		{
			engine::ScopedRenderPass renderPass(engine::RenderPass::kGameUi);

			// シーン管理がある場合だけ、ゲーム用のUIを別の描画パスに出す。
			if (sceneManager_) {
				sceneManager_->DrawUi();
			}
		}

		GetDirectX()->EndGpuProfile("  ゲームUI");

		GetDirectX()->StartGpuProfile("  デバッグ");
		{
			engine::ScopedRenderPass renderPass(engine::RenderPass::kEditorOverlay);

			// エディターが存在し、遷移画面を表示していなければ、操作用の表示を重ねる。
			if (editorController_ && (!sceneManager_ || !sceneManager_->IsTransitioning())) {
				editorController_->DrawSceneDebug(GetDirectX()->GetCommandList());
			}
		}

		GetDirectX()->EndGpuProfile("  デバッグ");
	// 通常表示はエディター専用のプレビューと操作用の描画を省く。
	} else
#endif
	{
		// シーンを描画できる管理がある場合に、3DとゲームUIを順に出す。
		if (sceneManager_) {
			// 配布実行時も描画順は3D→UI→Fade。Editor専用の重ね描きだけ省く。
			{
				engine::ScopedRenderPass renderPass(engine::RenderPass::kMainScene);
				sceneManager_->Draw();
				ID3D12Resource* pointLight = nullptr;
				ID3D12Resource* spotLight = nullptr;

				// 遷移中の画面には通常の破片とメッシュ演出を混ぜない。
				if (!sceneManager_->IsTransitioning()) {
					pointLight = engine::LightManager::GetInstance()->GetPointLightResource();
					spotLight = engine::LightManager::GetInstance()->GetSpotLightResource();
					DebrisEffectManager::GetInstance()->Draw(pointLight, spotLight);

					// 先に描いたシーン色を使う演出があれば、演出を出す前にコピーを更新する。
					if (MeshEffectManager::GetInstance()->RequiresSceneColorCopy()) {
						GetDirectX()->UpdateGrabTexture();
					}

					MeshEffectManager::GetInstance()->Draw(pointLight, spotLight);
				}
			}

			{
				engine::ScopedRenderPass renderPass(engine::RenderPass::kGameUi);
				sceneManager_->DrawUi();
			}
		}
	}

	GetDirectX()->StartGpuProfile("Fade");

	// シーンの非同期初期化中を除き、フェードをゲームUIと同じ系統の描画へ重ねる。
	if (!sceneManager_ || !sceneManager_->IsSceneInitializingAsync()) {
		engine::ScopedRenderPass renderPass(engine::RenderPass::kGameUi);
		Fade::GetInstance()->Draw();
	}

	GetDirectX()->EndGpuProfile("Fade");

	GetDirectX()->EndGpuProfile("メイン描画");
	GetDirectX()->PostDrawRenderTexture();
}

void Game::DrawCameraEditorPreview(PostEffect* postEffect) {
	// カメラエディタ用の別視点プレビューを専用テクスチャへ描画する。
#ifdef USE_IMGUI
	// 後処理かシーン管理が欠けていれば、別カメラの描画先を確定できない。
	if (!postEffect || !sceneManager_) {
		return;
	}

	CameraEditor* cameraEditor = CameraEditor::GetInstance();

	// カメラエディターを参照できなければ、プレビューに必要なカメラを作れない。
	if (!cameraEditor) {
		return;
	}

	const bool renderEditorPreview = cameraEditor->ShouldRenderCameraPreview();
	const bool renderSceneCameraPreview = cameraEditor->ShouldRenderSceneCameraPreview();

	// 一覧と演出のどちらからも要求がなければ、GPUのプレビュー描画状態に入らない。
	if (!renderEditorPreview && !renderSceneCameraPreview) {
		return;
	}

	ID3D12GraphicsCommandList* commandList = GetDirectX()->GetCommandList();
	engine::ScopedRenderPass renderPass(engine::RenderPass::kCameraPreview);
	GetDirectX()->SetCameraPreviewRendering(true);

	// 専用テクスチャへ描画し、SRVに戻してからエディタへ描画完了を通知する。
	const auto drawPreview = [&](engine::Camera* camera, std::int32_t textureIndex, std::int32_t viewIndex) {
		// カメラを用意できない場合は、前回のプレビューテクスチャを保持する。
		if (!camera) {
			return;
		}

		postEffect->PreDrawSceneWithDepth(commandList, textureIndex, true);
		// 現在シーンを取得できる場合だけ、開始済みのプレビュー描画先へ専用視点の映像を出す。
		if (BaseScene* currentScene = sceneManager_->GetCurrentScene()) {
			currentScene->DrawCameraPreview(camera, viewIndex);
		}
		postEffect->TransitionToSrv(commandList, textureIndex);
		cameraEditor->NotifyCameraPreviewRendered(viewIndex == 1);
	};

	// エディター内の通常プレビューを表示中の場合だけ、通常プレビュー用のテクスチャに出す。
	if (renderEditorPreview) {
		drawPreview(cameraEditor->PreparePreviewCamera(16.0f / 9.0f), PostEffect::kCameraPreviewTextureIndex, 0);
	}
	// 演出カメラを表示している場合は、通常のプレビューと分けたテクスチャに出す。
	if (renderSceneCameraPreview) {
		drawPreview(cameraEditor->PrepareCinematicPreviewCamera(16.0f / 9.0f), PostEffect::kCinematicCameraPreviewTextureIndex, 1);
	}

	// 二つのプレビューで使った描画フラグを戻し、後続のバックバッファ描画へ持ち越さない。
	GetDirectX()->SetCameraPreviewRendering(false);
#else
	(void)postEffect;
#endif
}

void Game::ApplyPostEffectPipeline(PostEffect* postEffect, bool outputForEditorGameView) {
	// レンダーテクスチャへ複数段のポストエフェクトを適用し、最終出力先へ渡す。
	GetDirectX()->StartGpuProfile("後処理");
	engine::ScopedRenderPass renderPass(engine::RenderPass::kPostProcess);
	ID3D12GraphicsCommandList* commandList = GetDirectX()->GetCommandList();
	const uint32_t renderTextureHandle = GetDirectX()->GetRenderTextureSrvHandle();

	auto drawFinalComposite = [&](uint32_t sourceHandle) {
		// EditorはGame Viewの参照用テクスチャ、配布実行はウィンドウのバックバッファを最終出力にする。
		// Game Viewで使う画像は専用の後処理テクスチャへ描き、UIが読めるSRVへ戻す。
		if (outputForEditorGameView) {
			postEffect->PreDrawScene(commandList, 1);
			postEffect->Draw(commandList, sourceHandle, PostEffect::CompositeHdrPipeline);
			postEffect->TransitionToSrv(commandList, 1);
		// 通常表示では完成画像をウィンドウのバックバッファへ直接合成する。
		} else {
			GetDirectX()->PreDraw();
			// 黒帯は後処理を掛けず、完成したゲーム映像だけを等比で配置します。
			const auto area = WinApp::CalculateGameViewRect(WinApp::GetWindowWidth(), WinApp::GetWindowHeight());
			D3D12_VIEWPORT viewport{area.x, area.y, area.width, area.height, 0.0f, 1.0f};
			commandList->RSSetViewports(1, &viewport);
			postEffect->Draw(commandList, sourceHandle, PostEffect::CompositeBackBufferPipeline);
		}
	};

	// Bloomが無効、または強度が実質0なら、抽出・縮小・加算をすべて省略する。
	// Tone Mappingや色調補正などは最終Compositeで引き続き適用される。
	if (!postEffect->IsBloomActive()) {
		drawFinalComposite(renderTextureHandle);
		GetDirectX()->EndGpuProfile("後処理");
		return;
	}

	constexpr std::array<std::int32_t, 4> kBloomTextureIndices = { 2, 3, 4, 5 };
	// 有効な段数分だけ、直前に縮小した明部テクスチャから次のレベルを作る。
	const std::int32_t bloomLevelCount = postEffect->GetBloomLevelCount();

	postEffect->PreDrawScene(commandList, kBloomTextureIndices[0]);
	postEffect->Draw(commandList, renderTextureHandle, PostEffect::ExtractPipeline);
	postEffect->TransitionToSrv(commandList, kBloomTextureIndices[0]);

	for (std::int32_t level = 1; level < bloomLevelCount; ++level) {
		const std::int32_t sourceIndex = kBloomTextureIndices[level - 1];
		const std::int32_t targetIndex = kBloomTextureIndices[level];
		postEffect->PreDrawScene(commandList, targetIndex);
		postEffect->Draw(commandList, postEffect->GetSrvHandle(sourceIndex), PostEffect::DownsamplePipeline);
		postEffect->TransitionToSrv(commandList, targetIndex);
	}

	postEffect->PreDrawScene(commandList, 0);
	postEffect->Draw(commandList, renderTextureHandle, PostEffect::CopyPipeline);

	// シーン色を入れた合成先をクリアせず、各段の明部を加算するために描画先として開き直す。
	postEffect->PreDrawScene(commandList, 0, false);

	for (std::int32_t level = 0; level < bloomLevelCount; ++level) {
		postEffect->Draw(
			commandList,
			postEffect->GetSrvHandle(kBloomTextureIndices[level]),
			PostEffect::AddPipeline);
	}

	postEffect->TransitionToSrv(commandList, 0);

	// 明部を加算した画像を入力にして、トーンマッピングと最終出力を行う。
	drawFinalComposite(postEffect->GetSrvHandle(0));

	GetDirectX()->EndGpuProfile("後処理");
}

void Game::RecordDrawProfile(const std::chrono::high_resolution_clock::time_point& startDraw) {
	// 描画全体、CPUコマンド発行、GPU待ち時間をProfilerManagerへ記録する。
	auto endDraw = std::chrono::high_resolution_clock::now();
	std::chrono::duration<float, std::milli> drawDuration = endDraw - startDraw;

	drawTimeMs_ = drawDuration.count();
	drawTimeHistory_[timeHistoryIndex_] = drawTimeMs_;

	ProfilerManager::GetInstance()->RecordCpuTime("描画 (合計)", drawTimeMs_);

	std::chrono::duration<float, std::milli> cmdDuration = prePostDrawTime_ - startDraw;
	// PostDrawの直前までを命令発行とし、Presentや後続UI処理を含む残り時間を「GPU待機」欄へ記録する。
	float cmdTimeMs = cmdDuration.count();
	cpuCmdTimeMs_ = cmdTimeMs;

	// 命令発行の計測値が全体時間の内側に収まるフレームだけ、二つの内訳を記録する。
	if (cmdTimeMs > 0.0f && cmdTimeMs < drawTimeMs_) {
		ProfilerManager::GetInstance()->RecordCpuTime("  命令発行", cmdTimeMs);
		ProfilerManager::GetInstance()->RecordCpuTime("  GPU待機", drawTimeMs_ - cmdTimeMs);
	}
}

void Game::RecordFixedFpsProfile() {
	// 固定FPS待機時間とフレーム間隔を計測し、負荷表示に反映する。
	auto preFixFps = std::chrono::high_resolution_clock::now();
	GetDirectX()->UpdateFixedFps();
	auto postFixFps = std::chrono::high_resolution_clock::now();

	static std::chrono::high_resolution_clock::time_point sLastFrameStart;
	float frameDelta = std::chrono::duration<float, std::milli>(postFixFps - sLastFrameStart).count();
	sLastFrameStart = postFixFps;

	// 最初の未初期化の時間差や長い中断を除き、通常のフレーム間隔だけを履歴へ入れる。
	if (frameDelta > 0.0f && frameDelta < 200.0f) {
		ProfilerManager::GetInstance()->RecordCpuTime("フレーム間隔", frameDelta);
	}

	float fixFpsWait = std::chrono::duration<float, std::milli>(postFixFps - preFixFps).count();
	ProfilerManager::GetInstance()->RecordCpuTime("  FPS固定待ち", fixFpsWait);
}
