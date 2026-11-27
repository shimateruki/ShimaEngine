#include <cstdint>
#include "SelectedActorSession.h"
#include "engine/editor/core/GameEditorController.Internal.h"
#include "engine/editor/scene/DebugTeleportEditor.h"

#include "game/ui/SettingsMenuOverlay.h"
#include "game/system/scene/SceneGameplayAccess.h"
#ifdef USE_IMGUI
#include "engine/editor/help/ClassDiagramCommand.h"

using namespace gameEditorControllerImplementation;

#ifdef NDEBUG
namespace {
// USE_IMGUIかつNDEBUGのDevelopmentだけに存在する、保存しないデバッグスイッチ。
bool debugHugeHealth = false;
Player* DebugHealthPlayer(SceneManager* manager) {
    // シーン切替直後には対象が存在しないため、デバッグ操作側へ不在を返す。
    auto* scene = manager ? manager->GetCurrentScene() : nullptr;
    return GetScenePlayer(scene);
}

// 大量HPデバッグを解除し、現在HPを通常の最大HPへ収めます。
void DisableDebugHugeHealth(SceneManager* manager) {
    // 無敵デバッグを解除する際は、通常の最大HPを超えた値も戻す。
    if (debugHugeHealth) {
        // プレイヤーとパラメーターを取得できたときだけ、現在HPを通常の上限に収めます。
        if (auto* player = DebugHealthPlayer(manager); player && player->GetParameters().has_value()) {
            player->SetParameter(&engine::EntityParameter::hp, (std::min)(player->GetParameters()->hp, player->GetMaxHp()));
        }
    }

    debugHugeHealth = false;
}
}
#endif

GameEditorController::GameEditorController() = default;

// GameEditorControllerの既定デストラクタ。明示的な解放はFinalizeで行う。
GameEditorController::~GameEditorController() = default;

bool GameEditorController::CanEditScene() const {
	// 遷移中やPlay中に編集すると復元対象が食い違うため、編集を止める。
	if (!debugEditor_ || !sceneManager_ || sceneManager_->IsTransitioning()) {
		return false;
	}

	const bool isPlaying = activePlayState_ ? *activePlayState_ : sceneManager_->IsPlaying();
	return !isPlaying;
}

// 経路編集は選択 Object 自体を操作しないため、複製や削除の共通コマンドを無効にする。
bool GameEditorController::CanEditSelectedObject() const {
	return CanEditScene() && debugEditor_->GetSelectedObject() && !debugEditor_->GetIsPathEditMode();
}

bool GameEditorController::ExecuteEditorCommand(const char* commandId) {
#ifdef NDEBUG
    // Scene保存にデバッグHPを持ち込まない。
    if (commandId && std::string(commandId) == editorCommandId::kSceneSave)
        DisableDebugHugeHealth(sceneManager_);
#endif
	return commandId && EditorCommandRegistry::GetInstance()->Execute(commandId);
}

bool GameEditorController::DrawEditorCommandMenuItem(
	const char* commandId,
	const char* labelOverride,
	bool selected) {
	const EditorCommand* command = EditorCommandRegistry::GetInstance()->Find(commandId ? commandId : "");

	// コマンドIDが登録されていなければ、表示にも実行にも進みません。
	if (!command) {
		return false;
	}

	const char* label = labelOverride ? labelOverride : command->displayName.c_str();
	const char* shortcut = command->shortcut.empty() ? nullptr : command->shortcut.c_str();

	// 項目が選択されなかったか実行条件を満たさなければ、コマンドを呼びません。
	if (!ImGui::MenuItem(label, shortcut, selected, EditorCommandRegistry::GetInstance()->CanExecute(command->id))) {
		return false;
	}

	return ExecuteEditorCommand(command->id.c_str());
}

namespace {
// 次フレームの再生成を防ぐため、シーケンスを止めてから各演出を解放する。
void ClearPlayTransitionEffects() {
	VfxSequencer::ClearOneShots();
	MeshEffectManager::GetInstance()->Clear();
	GpuParticleManager::GetInstance()->ResetSimulation();
	DebrisEffectManager::GetInstance()->Clear();
}
}

void GameEditorController::StopPlay() {
	// 再生状態を受け取っていないか実行中でなければ、停止処理を重ねません。
	if (!activePlayState_ || !*activePlayState_) {
		return;
	}

#ifdef NDEBUG
    DisableDebugHugeHealth(sceneManager_);
#endif
	// 変更追跡が使える場合は、スナップショットによる復元の前に差分を収集します。
	if (playModeChangeTracker_) {
		// 復元すると実行中の値が消えるため、編集シーンに持ち帰れる候補だけを停止直前に控える。
		playModeChangeTracker_->CaptureRuntimeChanges();
	}

	// 同じシーンなら開始前のメモリ状態へ戻す。別シーンへ遷移して元の状態を適用できないときは後続の再読込へ進む。
	const bool restored = playModeSnapshot_ && playModeSnapshot_->Restore();
	simulationPaused_ = false;
	*activePlayState_ = false;

	// シーン管理が使える場合は、呼び出し元の状態に合わせて再生を止めます。
	if (sceneManager_) {
		sceneManager_->SetIsPlaying(false);
	}

	// 停止時に進行途中のシーケンスを残すと、次フレームに0秒更新されて
	// 消去後のメッシュエフェクトを再生成し、編集画面へ張り付いたように残ります。
	ClearPlayTransitionEffects();
	ClearSceneBoundEditorState();

	// 差分追跡が有効なら、編集状態を復元できたかを通知します。
	if (playModeChangeTracker_) {
		playModeChangeTracker_->OnSceneRestored(restored);
	}

	CameraEditor::GetInstance()->SetMode(CameraEditor::Mode::kEditor);

	// メモリ上の復元に成功した場合、再読込を予約せずに終了します。
	if (restored) {
		DebugConsole::GetInstance()->AddLog("Play In Editor: 編集開始時の状態へ復元しました。");
		playOriginSceneName_.clear();
		playOriginSceneLoadContext_ = {};
		playOriginSceneGeneration_ = 0;
		pendingPlayOriginReload_ = false;
		return;
	}

	// Play中にSceneが切り替わった場合、元インスタンスは破棄済みなので再ロードで戻します。
	pendingPlayOriginReload_ = true;
	ReloadPlayOriginScene();
}

void GameEditorController::ReloadPlayOriginScene() {
	// Play中に破棄された元シーンを、Asset指定または登録名から復元する。
	// 開始シーンの再読込予約があり、シーン管理が使えて遷移中でない場合に進めます。
	if (!pendingPlayOriginReload_ || !sceneManager_ || sceneManager_->IsTransitioning()) {
		return;
	}

	bool reloadRequested = false;

	// 開始時にScene Assetを編集中だった場合は、保存した読込条件で開き直します。
	if (playOriginSceneLoadContext_.IsSceneAsset()) {
		reloadRequested = sceneManager_->OpenSceneAsset(playOriginSceneLoadContext_);
	}
	// Assetでなく、開始時の登録名がまだ有効ならC++シーンを開き直します。
	else if (!playOriginSceneName_.empty() && sceneManager_->IsSceneRegistered(playOriginSceneName_)) {
		sceneManager_->ChangeScene(playOriginSceneName_);
		reloadRequested = true;
	}

	// 再読込を要求できた場合は理由をログに残します。
	if (reloadRequested) {
		DebugConsole::GetInstance()->AddLog(
			"Play In Editor: Play中にSceneが切り替わったため、開始Sceneを再ロードします。");
	}
	// 再読込要求を出せなかった場合は、開始シーンを復元できなかったことをログに残します。
	else {
		DebugConsole::GetInstance()->AddLog(
			"Play In Editor Warning: 開始Sceneを復元できませんでした。");
	}

	// 再読込の試行結果にかかわらず、同じ要求を繰り返さないよう記録を消します。
	pendingPlayOriginReload_ = false;
	playOriginSceneName_.clear();
	playOriginSceneLoadContext_ = {};
	playOriginSceneGeneration_ = 0;
}

void GameEditorController::Initialize(SceneManager* sceneManager, engine::DirectXCommon* dxCommon) {
	// 各ツールへ同じシーン管理者を渡し、選択状態とプレビュー対象を共有する。
	sceneManager_ = sceneManager;
	postEffectEditor_ = std::make_unique<PostEffectEditor>();
	postEffectEditor_->Initialize(PostEffect::GetInstance());

	spriteDebugEditor_ = std::make_unique<SpriteDebugEditor>();
	spriteDebugEditor_->Initialize(sceneManager, engine::InputManager::GetInstance());

	ghostRecorder_ = std::make_unique<GhostRecorder>();
	ghostRecorder_->Initialize(sceneManager);

	debugEditor_ = std::make_unique<DebugEditor>();
	debugEditor_->Initialize(sceneManager, dxCommon);
    actorSession_ = std::make_unique<SelectedActorSession>();
    actorSession_->Initialize(sceneManager);
    debugEditor_->SetActorSession(actorSession_.get());
    // シーン切替前に DebugEditor が使える場合は、アニメーションプレビューを止めます。
    actorSession_->SetBeforeSceneChange([this] { if (debugEditor_) debugEditor_->GetAnimationClipEditor()->StopPreview(); });


	// シーンの管理者があれば、新規シーンにエディターを渡せるよう参照を登録します。
	if (sceneManager) {
		sceneManager->SetDebugEditor(debugEditor_.get());
	}

	particleEditor_ = std::make_unique<ParticleEditor>();
	particleEditor_->Initialize(sceneManager);

	gpuParticleEditor_ = std::make_unique<GpuParticleEditor>();
	gpuParticleEditor_->Initialize();

	vfxSequencerEditor_ = std::make_unique<VfxSequencerEditor>();
	vfxSequencerEditor_->Initialize();

	meshEffectEditor_ = std::make_unique<MeshEffectEditor>();
	meshEffectEditor_->Initialize(sceneManager);

	debrisEffectEditor_ = std::make_unique<DebrisEffectEditor>();
	debrisEffectEditor_->Initialize(sceneManager);

	trailEmitterEditor_ = std::make_unique<TrailEmitterEditor>();
	trailEmitterEditor_->Initialize(sceneManager);

	LightEditor::GetInstance()->Initialize();
	DebugConsole::GetInstance()->Initialize();

	// Director の初期化は、参照先の DebugEditor と演出ツールを作った後に行う。
	ghostDirector_ = std::make_unique<GhostDirector>();
	ghostDirector_->Initialize(sceneManager, debugEditor_.get());

	engineManualWindow_ = std::make_unique<EngineManualWindow>();
	playModeSnapshot_ = std::make_unique<PlayModeSnapshot>();
	playModeSnapshot_->Initialize(sceneManager);

	// シーン管理が利用できれば、エディター初期化より先に作られたシーンにも参照を登録します。
	if (sceneManager) {
		// すでにアクティブなシーンがある場合だけ、現在のシーンへ直接接続します。
		if (BaseScene* currentScene = sceneManager->GetCurrentScene()) {
			currentScene->SetDebugEditor(debugEditor_.get());
		}
	}

	// DebugEditor が利用できる場合に、各専用ウィンドウを開くための参照を揃えます。
	if (debugEditor_) {
		debugEditor_->SetEditors(
			postEffectEditor_.get(),
			spriteDebugEditor_.get(),
			particleEditor_.get(),
			gpuParticleEditor_.get(),
			vfxSequencerEditor_.get(),
			ghostRecorder_.get(),
			ghostDirector_.get(),
			LightEditor::GetInstance(),
			meshEffectEditor_.get(),
			debrisEffectEditor_.get(),
			trailEmitterEditor_.get());
	}


    sceneWorkspace_ = std::make_unique<SceneWorkspace>();
    sceneWorkspace_->Initialize(sceneManager_, debugEditor_.get());
    debugEditor_->SetSceneWorkspace(sceneWorkspace_.get());
    DebugTeleportEditor::GetInstance().Initialize(sceneManager_, debugEditor_.get());

    editorQuickSearch_ = std::make_unique<EditorQuickSearch>();
    editorQuickSearch_->Initialize(
        sceneManager_,
        debugEditor_.get(),
        [this]() { showDebugWindows_ = true; });

    assetReferenceExplorer_ = std::make_unique<AssetReferenceExplorer>();
    assetReferenceExplorer_->Initialize(debugEditor_.get());
    playModeChangeTracker_ = std::make_unique<PlayModeChangeTracker>();
    playModeChangeTracker_->Initialize(sceneManager_, debugEditor_.get());
    debugEditor_->SetPlayFromPositionCallback(
        [this](const engine::Vector3& position, const std::string& label) {
            RequestPlayFromPosition(position, label);
        });

	// 対象とコールバックをそろえた後でメニューの操作を公開する。ツール設定の読み込みも登録の後で行う。
	RegisterEditorCommands();
    RegisterEditorTools();
}

void GameEditorController::Finalize() {
    // アクターセッションが作成済みなら、終了処理を済ませてから参照を切ります。
    if (actorSession_) actorSession_->Finalize();
    // DebugEditor が残っていれば、セッションへの借用参照を外します。
    if (debugEditor_) debugEditor_->SetActorSession(nullptr);
    actorSession_.reset();

    // ツールへのポインターや this を捕捉したコマンドを解除してから、各ツールを破棄する。
    EditorToolSettings::Get().Clear();
    DebugTeleportEditor::GetInstance().Finalize();
	EditorCommandRegistry::GetInstance()->UnregisterOwner(this);
	activePlayState_ = nullptr;
	activeSceneName_.clear();
    // 検索ツールがあれば、登録を解除してから破棄します。
    if (editorQuickSearch_) {
        editorQuickSearch_->Finalize();
    }

    editorQuickSearch_.reset();

    // DebugEditor が使える間に、Workspace を指している参照を外します。
    if (debugEditor_) {
        debugEditor_->SetSceneWorkspace(nullptr);
    }

    // Workspace があれば、その終了処理を呼んでから破棄します。
    if (sceneWorkspace_) {
        sceneWorkspace_->Finalize();
    }

    sceneWorkspace_.reset();

    // DebugEditor があれば、この管理者に戻る地点指定コールバックを解除します。
    if (debugEditor_)
        debugEditor_->SetPlayFromPositionCallback({});
    // Play中の変更追跡を作成済みなら、保持状態を終了処理で片付けます。
    if (playModeChangeTracker_)
        playModeChangeTracker_->Finalize();
    playModeChangeTracker_.reset();
	// 再生前のスナップショットがあれば、内容とシーン参照を消去します。
	if (playModeSnapshot_) playModeSnapshot_->Reset();
    // アセット参照画面が作成済みなら、終了処理を呼んでから破棄します。
    if (assetReferenceExplorer_)
        assetReferenceExplorer_->Finalize();
    assetReferenceExplorer_.reset();
	sceneManager_ = nullptr;
	playModeSnapshot_.reset();
	trailEmitterEditor_.reset();
	debrisEffectEditor_.reset();
	meshEffectEditor_.reset();
	vfxSequencerEditor_.reset();
	gpuParticleEditor_.reset();
	ghostDirector_.reset();
	ghostRecorder_.reset();
	particleEditor_.reset();
	spriteDebugEditor_.reset();
	debugEditor_.reset();
	postEffectEditor_.reset();
	engineManualWindow_.reset();
	DebugConsole::GetInstance()->Finalize();
}

void GameEditorController::BeginFrame() {
    // ImGuiとImGuizmoのフレームを開始し、既定ドックスペースを準備する。
	ImGuiManager::GetInstance()->BeginFrame();
	ImGuizmo::BeginFrame();

	// シーン遷移中だけ、描画前に旧シーンの参照を外す状態を管理します。
	if (sceneManager_ && sceneManager_->IsTransitioning()) {
		// 遷移の最初のフレームだけ、旧シーンの参照を破棄します。
		if (!sceneTransitionStateCleared_) {
			// 旧Sceneの参照を、HierarchyやInspectorの描画より前に外します。
			ClearSceneBoundEditorState();
			sceneTransitionStateCleared_ = true;
		}
	// 遷移を終えたら、次の切り替えでも初回に参照を外せるようにします。
	} else {
		sceneTransitionStateCleared_ = false;
	}

	CameraEditor::GetInstance()->BeginPreviewUiFrame();
    EditorToolSettings::Get().HandleShortcuts(!SettingsMenuOverlay::IsAnyActive() && sceneManager_ && !sceneManager_->IsPlaying() && !sceneManager_->IsTransitioning());

    // Workspace があれば毎フレーム更新します。
    if (sceneWorkspace_) {
        sceneWorkspace_->Update();
        // 設定メニューが開いていない場合にだけ、Workspace のショートカットを処理します。
        if (!SettingsMenuOverlay::IsAnyActive())
            sceneWorkspace_->HandleHotkeys();
    }

    // 検索ツールがあれば、フレーム内の起動ショートカットを確認します。
    if (editorQuickSearch_) {
        // 設定メニューが入力を使っていない場合に検索を起動できます。
        if (!SettingsMenuOverlay::IsAnyActive())
            editorQuickSearch_->HandleShortcut();
    }

    IEditable* selectedObject = EditorManager::GetInstance()->GetSelectedObject();
    const bool wantsEffect = selectedObject == particleEditor_.get() || selectedObject == gpuParticleEditor_.get() ||
        selectedObject == vfxSequencerEditor_.get() || selectedObject == meshEffectEditor_.get() ||
        selectedObject == debrisEffectEditor_.get() || selectedObject == trailEmitterEditor_.get();
    // 共有のプレビュー空間は選択が切り替わったときだけ変更し、対象を切り替える前に動作確認を停止する。
    if (wantsEffect != showEffectPreviewTimeline_) {
        // キャラクターの確認セッションがあれば、プレビューの所有権を切り替える前に止めます。
        if (actorSession_) actorSession_->Stop();
        // エフェクトツールへ切り替わったら共有のプレビュー空間を使います。
        if (wantsEffect) EffectPreviewStage::GetInstance()->EnableForToolPreview();
        // エフェクトツールから離れたら、元のシーン表示を戻します。
        else EffectPreviewStage::GetInstance()->ReturnToScene();
        showEffectPreviewTimeline_ = wantsEffect;
        dockspaceInitialized_ = false;
    }
    const bool wantsCinematic = selectedObject == ghostDirector_.get();
    // 演出編集から別のツールへ移る際に、Studio が一時的に変えたカメラや表示を戻す。
    if (showCinematicTimeline_ && !wantsCinematic) ghostDirector_->EndStudio();
    // 演出編集への出入りで下段のタイムラインの配置を作り直します。
    if (wantsCinematic != showCinematicTimeline_) {
        showCinematicTimeline_ = wantsCinematic;
        dockspaceInitialized_ = false;
    }
    showEnemyAttackTimeline_ = showAnimationTimeline_ = false;
    // セッションと選択元がそろっている場合、現在の選択をプレビュー対象にします。
    if (actorSession_ && debugEditor_) actorSession_->SetTarget(debugEditor_->GetSelectedObject());
    // エディターが使える場合、アニメーションのプレビュー対象が変わったか確認します。
    if (debugEditor_) {
        auto* selected = debugEditor_->GetSelectedObject();
        auto* clip = debugEditor_->GetAnimationClipEditor();
        // クリッププレビューが有効で、現在の選択が無いかIDが異なる場合はプレビューを終了します。
        if (clip->IsEnabled() && (!selected || selected->GetReplayId() != clip->GetTargetId())) clip->StopPreview();
    }

	SetupDefaultDockspace();
    ProcessPendingWorkspaceLayout();
}

void GameEditorController::SetPortfolioCaptureMode(bool enabled) {
    // 撮影モードが現在と同じなら、入力範囲と切替ログの更新は不要です。
	if (portfolioCaptureMode_ == enabled) {
		return;
	}

	portfolioCaptureMode_ = enabled;
    // 撮影モードではGame Viewパネルの古い矩形を入力に使わないよう解除します。
    if (enabled)
        SettingsMenuOverlay::SetEditorViewport(0, 0, 0, 0);
	DebugConsole::GetInstance()->AddLog(
		portfolioCaptureMode_
			? "ポートフォリオ撮影モード: ON (F10でエディタ表示に戻ります)"
			: "ポートフォリオ撮影モード: OFF (エディタ表示を再開しました)");
}


void GameEditorController::DrawMainMenuBar(SceneManager* sceneManager, bool& isPlaying, const std::string& currentSceneName) {
    // メニュー操作の入口として、現在のシーンと Game 側の Play 状態を参照する。
	sceneManager_ = sceneManager;
	activePlayState_ = &isPlaying;
	activeSceneName_ = currentSceneName;
	ReloadPlayOriginScene();

	// メニューバーを描画できないフレームでは、項目や確認ポップアップへ進みません。
	if (!ImGui::BeginMainMenuBar()) {
		return;
	}

	// 再生中は停止コマンドを表示し、実行できる場合だけ操作を受け付けます。
	if (isPlaying) {
		ImGui::BeginDisabled(!EditorCommandRegistry::GetInstance()->CanExecute(editorCommandId::kStop));

		// 停止ボタンが押されたとき、同じ停止コマンドを実行します。
		if (ImGui::Button(ICON_FA_STOP " 停止")) {
			ExecuteEditorCommand(editorCommandId::kStop);
		}

		ImGui::EndDisabled();
	// 編集モード中は停止の代わりに、開始可能な再生コマンドを表示します。
	} else {
		ImGui::BeginDisabled(!EditorCommandRegistry::GetInstance()->CanExecute(editorCommandId::kPlay));

		// 再生ボタンが押されたとき、編集状態を退避して再生を開始するコマンドを呼びます。
		if (ImGui::Button(ICON_FA_PLAY " 再生")) {
			ExecuteEditorCommand(editorCommandId::kPlay);
		}

		ImGui::EndDisabled();
	}

	ImGui::Text(isPlaying ? " | 実行中" : " | 編集モード");

	// 再生中にだけ、停止とは別に進行を一時停止・再開する操作を表示します。
	if (isPlaying) {
        ImGui::SameLine();
        // 切替ボタンが押されたら、現在の一時停止状態に応じて同じコマンドで反転します。
        if (ImGui::Button(simulationPaused_ ? ICON_FA_PLAY " 再開" : ICON_FA_PAUSE " 一時停止")) {
            ExecuteEditorCommand(editorCommandId::kPauseResume);
        }
    }

	const ImGuiIO& menuIo = ImGui::GetIO();

	// 文字入力中を避け、Ctrl+Sを押した瞬間にシーン保存コマンドを呼びます。
	if (!menuIo.WantTextInput && menuIo.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
		ExecuteEditorCommand(editorCommandId::kSceneSave);
	}

	// 文字入力中を避け、Ctrl+Pを押した瞬間に再生と停止を切り替えます。
	if (!menuIo.WantTextInput && menuIo.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_P, false)) {
		ExecuteEditorCommand(isPlaying ? editorCommandId::kStop : editorCommandId::kPlay);
	}

	// ファイルメニューが展開されたときだけ、シーンの作成・保存・管理を並べます。
	if (ImGui::BeginMenu(ICON_FA_FILE " ファイル")) {
		DrawEditorCommandMenuItem(editorCommandId::kSceneNew, ICON_FA_PLUS " 新規Scene Asset");
		DrawEditorCommandMenuItem(editorCommandId::kSceneSave, ICON_FA_SAVE " Active Sceneを保存");
		ImGui::Separator();

		// 管理項目が選択されたら、シーン管理に使う編集パネルを表示します。
		if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN " Scene Asset管理を表示")) {
			showDebugWindows_ = true;
		}

		ImGui::EndMenu();
	}

	// 編集メニューを開いたときに、操作履歴の名称と選択対象に使えるコマンドを並べます。
	if (ImGui::BeginMenu("編集")) {
        DrawEditorCommandMenuItem(editorCommandId::kViewQuickSearch, ICON_FA_SEARCH " 統合コマンドパレット");
        ImGui::Separator();
		const std::string undoLabel = EditorTransactionManager::GetInstance()->CanUndo()
			? "元に戻す: " + EditorTransactionManager::GetInstance()->GetUndoLabel()
			: "元に戻す";
		const std::string redoLabel = EditorTransactionManager::GetInstance()->CanRedo()
			? "やり直す: " + EditorTransactionManager::GetInstance()->GetRedoLabel()
			: "やり直す";
		DrawEditorCommandMenuItem(editorCommandId::kEditUndo, undoLabel.c_str());
		DrawEditorCommandMenuItem(editorCommandId::kEditRedo, redoLabel.c_str());
		ImGui::Separator();
		DrawEditorCommandMenuItem(editorCommandId::kEditDuplicate);
		DrawEditorCommandMenuItem(editorCommandId::kEditDelete);
		DrawEditorCommandMenuItem(editorCommandId::kObjectDropToFloor);
		ImGui::EndMenu();
	}

	// 表示メニューが展開された場合は、キャプチャ表示や編集パネルの切替を扱います。
	if (ImGui::BeginMenu("表示")) {
        EditorToolSettings::Get().DrawVisibilityMenu();
		DrawEditorCommandMenuItem(editorCommandId::kViewPortfolio, nullptr, portfolioCaptureMode_);

		// 撮影モードの項目にカーソルがある間だけ、画面の戻し方を表示します。
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("ONにするとImGuiを描画せず、ゲーム画面だけを直接表示します。戻るときはF10です。");
		}

		ImGui::Separator();
		DrawEditorCommandMenuItem(editorCommandId::kViewEditorPanels, nullptr, showDebugWindows_);
        DrawEditorCommandMenuItem(editorCommandId::kViewAssetReferences);
        DrawEditorCommandMenuItem(
            editorCommandId::kViewIsolateSelection, nullptr,
            sceneWorkspace_ && sceneWorkspace_->IsIsolationActive());
        DrawEditorCommandMenuItem(editorCommandId::kViewRestoreTemporaryVisibility);
        ImGui::Separator();
		ImGui::Separator();
		DrawEditorCommandMenuItem(editorCommandId::kViewConsole, nullptr, showDebugConsole_);
		DrawEditorCommandMenuItem(editorCommandId::kViewStatus, nullptr, showTimeController_);
		ImGui::EndMenu();
	}

    DrawWorkspaceMenu();
    EditorToolSettings::Get().DrawToolsMenu();

#ifdef NDEBUG
    // Developmentのデバッグメニューを展開したとき、利用可能なプレイヤーにHPの切替を表示します。
    if (ImGui::BeginMenu("デバッグ")) {
        auto* player = DebugHealthPlayer(sceneManager);
        const bool available = isPlaying && player && !player->IsDead() && player->GetHp() > 0.0f;

        // 生存中のプレイヤーがいて、項目が選択された場合にデバッグHPを切り替えます。
        if (ImGui::MenuItem("無敵（HP 1,000,000）", nullptr, debugHugeHealth, available)) {
            // 既に有効なら、通常の上限へHPを戻して解除します。
            if (debugHugeHealth)
                DisableDebugHugeHealth(sceneManager);
            // 無効だった場合は、後段の毎フレーム処理でHPを設定するために有効にします。
            else debugHugeHealth = true;
        }

        ImGui::TextDisabled("HPのみ増加。落下・即死は対象外。");
        ImGui::TextDisabled("OFF・再生停止・メニューからのScene保存で解除。");
        ImGui::EndMenu();
    }

    // ゲームが停止した場合は、次の試遊にデバッグHPを持ち越しません。
    if (!isPlaying)
        DisableDebugHugeHealth(sceneManager);
    // デバッグHPが有効でゲーム再生中の場合に、対象プレイヤーへ値を適用します。
    if (debugHugeHealth && isPlaying) {
        // シーン切替後の新しいPlayerにも適用。死亡済みのPlayerは復活させない。
        if (auto* player = DebugHealthPlayer(sceneManager);
            player && !player->IsDead() && player->GetParameters().has_value() && player->GetHp() > 0.0f) {
            player->SetParameter(&engine::EntityParameter::hp, 1000000.0f);
        }
    }
#endif

	// 実行メニューが展開されたら、差分候補と一時停止コマンドを表示します。
	if (ImGui::BeginMenu(ICON_FA_HISTORY " 実行")) {
		const std::size_t changeCount = playModeChangeTracker_ ? playModeChangeTracker_->GetPendingChangeCount() : 0;
		const std::string changeLabel = "Play中の変更を確認（" + std::to_string(changeCount) + "件）";

		// 差分が残りゲーム停止中で、項目が選ばれたときだけ候補を開きます。
		if (ImGui::MenuItem(changeLabel.c_str(), nullptr, false, changeCount > 0 && !isPlaying)) {
			playModeChangeTracker_->ShowPendingChanges();
		}

		ImGui::Separator();
		DrawEditorCommandMenuItem(editorCommandId::kPauseResume,
			simulationPaused_ ? "実行を再開" : "実行を一時停止");
		ImGui::EndMenu();
	}

	// シーン切り替えメニューが開いたとき、現在登録されている候補を一覧にします。
	if (ImGui::BeginMenu("シーン切り替え")) {
		// 現在のシーンを使用中の Play や遷移では切替を停止し、メニュー項目だけは参照できる状態にしておく。
		const bool canSwitchScene = sceneManager && !sceneManager->IsTransitioning() && !isPlaying;
		ImGui::BeginDisabled(!canSwitchScene);
		ImGui::TextDisabled("C++ Scene");

		// シーン管理者が使えるときだけ登録済みのC++シーンを取得します。
		if (sceneManager) {
			const bool hasUnsavedChanges = debugEditor_ && debugEditor_->HasAnyDirty();
			ImGui::BeginDisabled(hasUnsavedChanges);

			for (const std::string& sceneName : sceneManager->GetRegisteredSceneNames()) {
				// 編集用の内部シーンは通常シーンの候補から外します。
				if (sceneName == "SCENE_EDITOR") {
					continue;
				}

				// 切替候補が選ばれたら、現在の選択やプレビューを解除してから遷移を依頼します。
				if (ImGui::MenuItem(sceneName.c_str(), nullptr, sceneName == sceneManager->GetCurrentSceneName() && !sceneManager->HasActiveSceneAsset())) {
					ClearSceneBoundEditorState();
					sceneManager->ChangeScene(sceneName);
				}
			}

			ImGui::EndDisabled();

			// 未保存変更がある間は、切り替えの前に保存が必要なことを案内します。
			if (hasUnsavedChanges) {
				ImGui::TextDisabled("先にActive Sceneを保存してください");
			}
		}

		ImGui::Separator();
		// Asset 選択は実行クラスに加えて配置 JSON と環境を切り替えるため、未保存確認を持つ DebugEditor の入口へ渡す。
		ImGui::TextDisabled("Scene Asset");

		// DebugEditor が使える場合にScene Asset一覧と選択操作を用意します。
		if (debugEditor_) {
			const std::vector<SceneSerializer::SceneAssetInfo> sceneAssets = debugEditor_->GetSceneAssets();

			// 登録済みのScene Assetが空のときは、一覧の代わりに状態を伝えます。
			if (sceneAssets.empty()) {
				ImGui::TextDisabled("Scene Assetがありません");
			}

			for (const SceneSerializer::SceneAssetInfo& asset : sceneAssets) {
				const std::string label = asset.displayName + "##SceneAssetMenu_" + asset.filename;
				const std::string runtimeLabel = asset.runtimeScene + " / " + asset.controllerName;
				// Scene Assetが選択されたら、未保存確認のあるDebugEditorへ開く要求を渡します。
				if (ImGui::MenuItem(
					label.c_str(),
					runtimeLabel.c_str(),
					debugEditor_->IsCurrentSceneAsset(asset.filename))) {
					showDebugWindows_ = true;
					debugEditor_->RequestOpenSceneAssetFromMenu(asset.filename);
				}
			}
		}

		ImGui::EndDisabled();
		ImGui::EndMenu();
	}

	// ヘルプが展開されたときだけ、説明書と計測ツールのコマンドを並べます。
	if (ImGui::BeginMenu("ヘルプ")) {
		DrawEditorCommandMenuItem(editorCommandId::kHelpManual, ICON_FA_BOOK " エンジン説明書");
		DrawEditorCommandMenuItem(editorCommandId::kHelpClassDiagram);
		DrawEditorCommandMenuItem(editorCommandId::kHelpProfiler, ICON_FA_CHART_BAR " システムプロファイラ");
		ImGui::EndMenu();
	}

	ImGui::EndMainMenuBar();
	DrawUnsavedExitConfirmPopup();
}

bool GameEditorController::HasUnsavedEditorChanges() const {
    // エディタ上に未保存変更が残っているかを確認する。
	return debugEditor_ && debugEditor_->HasAnyDirty();
}

void GameEditorController::RequestExit() {
    // 終了要求時に未保存変更があれば確認ポップアップを開く。
	if (HasUnsavedEditorChanges()) {
		openUnsavedExitConfirm_ = true;
		return;
	}

	WinApp::CloseNow();
}

void GameEditorController::RequestPlay(SceneManager* sceneManager, bool& isPlaying, const std::string& currentSceneName) {
    // 現在の編集状態をメモリへ保持して、そのままゲーム再生を開始する。
	StartPlay(sceneManager, isPlaying, currentSceneName);
}

void GameEditorController::StartPlay(SceneManager* sceneManager, bool& isPlaying, const std::string& currentSceneName) {
    // 確認セッションが動いている場合に備え、再生の準備前に終了します。
    if (actorSession_) actorSession_->Stop();

    // Scene再読み込みは行わず、Play開始直前の状態を保持してゲームモードへ切り替える。
	// 差分追跡があれば、後で比較する編集時点の値を控えます。
	if (playModeChangeTracker_) {
		playModeChangeTracker_->CaptureBaseline();
	}

	ClearSceneBoundEditorState();
	playOriginSceneName_ = sceneManager ? sceneManager->GetCurrentSceneName() : currentSceneName;
	playOriginSceneGeneration_ = sceneManager ? sceneManager->GetSceneGeneration() : 0;
	playOriginSceneLoadContext_ = sceneManager ? sceneManager->GetActiveSceneLoadContext() : SceneLoadContext{};
	pendingPlayOriginReload_ = false;

	simulationPaused_ = false;
	const bool snapshotCaptured = playModeSnapshot_ && playModeSnapshot_->Capture();

	// スナップショットを取得できない場合も再生は開始します。停止時に再読込を使うことを記録します。
	if (!snapshotCaptured) {
		DebugConsole::GetInstance()->AddLog(
			"Play In Editor Warning: 編集状態を保持できなかったため、停止時はScene再ロードを使用します。");
	}

	// 指定スタート地点を使う場合も、停止時に開始位置を戻せるようスナップショットを先に取得する。
	ApplyPendingPlayStartPosition();

	// エディターのプレビューや直前のPlayから残った一時演出を、再生開始前に破棄します。
	// シーケンスを先に止めることで、消去済みエフェクトが後から再生成されるのを防ぎます。
	ClearPlayTransitionEffects();
	isPlaying = true;

	// シーン管理者が指定されていれば、呼び出し元と同じ再生状態へ更新します。
	if (sceneManager) {
		sceneManager->SetIsPlaying(true);
	}

	CameraEditor::GetInstance()->SetMode(CameraEditor::Mode::kGame);
}

// GameView等で指定された地点を保持し、そのままPlayを開始します。


void GameEditorController::RequestPlayFromPosition(const engine::Vector3& position, const std::string& label) {
	// 指定座標を予約し、StartPlay が編集状態を退避した後で既存の Player に反映する。
	if (!activePlayState_ || *activePlayState_ || !sceneManager_ || sceneManager_->IsTransitioning()) {
		return;
	}

	hasPendingPlayStartPosition_ = true;
	pendingPlayStartPosition_ = position;
	pendingPlayStartLabel_ = label;
	RequestPlay(sceneManager_, *activePlayState_, activeSceneName_);
}

void GameEditorController::ApplyPendingPlayStartPosition() {
	// 開始位置と再出現位置を合わせ、直前の速度がプレビューに残らないようにする。
	// 地点指定が無いか、シーンを取得できない場合は適用しません。
	if (!hasPendingPlayStartPosition_ || !sceneManager_ || !sceneManager_->GetCurrentScene())
		return;
	Player* player = nullptr;

	for (const auto& object : sceneManager_->GetCurrentScene()->GetObjects()) {
		// 無効な実体と、クラス名が Player 以外の配置物を除きます。
		if (!object || object->GetClassName() != "Player")
			continue;
		player = dynamic_cast<Player*>(object.get());
		// Player 型の実体へ変換できたところで、検索を終えます。
		if (player)
			break;
	}

	// Player が見つからなければ、失敗を記録して地点の予約を破棄します。
	if (!player) {
		DebugConsole::GetInstance()->AddLog("Play開始位置を適用できません: Playerが見つかりません。");
		hasPendingPlayStartPosition_ = false;
		pendingPlayStartLabel_.clear();
		return;
	}

	// 試遊時の現在位置と再出現位置をそろえ、速度を止めてから操作を有効にします。
	player->SetTranslate(pendingPlayStartPosition_);
	player->SetRespawnPosition(pendingPlayStartPosition_);
	player->SetVelocity({ 0.0f, 0.0f, 0.0f });
	player->SetIsControlActive(true);
	player->UpdateLocalMatrix();
	player->UpdateWorldMatrix();
	DebugConsole::GetInstance()->AddLog(
		"Play開始位置: " + pendingPlayStartLabel_ + " (" +
		std::to_string(pendingPlayStartPosition_.x) + ", " +
		std::to_string(pendingPlayStartPosition_.y) + ", " +
		std::to_string(pendingPlayStartPosition_.z) + ")");
	hasPendingPlayStartPosition_ = false;
	pendingPlayStartLabel_.clear();
}

// シーンに紐づく選択状態やゴースト対象をクリアし、Play前後の参照不整合を防ぐ。
void GameEditorController::ClearSceneBoundEditorState() {
    // 選択アクターのセッションがあれば、シーンと切り離す前に止めます。
    if (actorSession_) actorSession_->Stop();

    // 作成済みの各ツールだけを終了・解除し、旧シーンのプレビュー、表示補助、選択参照を残さないようにします。
    if (ghostDirector_)
        ghostDirector_->EndStudio();
    if (debugEditor_)
        debugEditor_->GetAnimationClipEditor()->StopPreview();
    if (sceneWorkspace_) {
        sceneWorkspace_->RestoreAllTemporaryVisibility();
    }

	if (playModeSnapshot_) {
		playModeSnapshot_->Reset();
	}

	if (ghostRecorder_) {
		ghostRecorder_->ClearTarget();
	}

	if (debugEditor_) {
		debugEditor_->SetSelectedObject(nullptr);
	}

	if (spriteDebugEditor_) {
		spriteDebugEditor_->ClearSceneSelection();
	}

	EditorManager::GetInstance()->ClearSelection();
}

void GameEditorController::DrawUnsavedExitConfirmPopup() {
    // 未保存変更がある状態で終了しようとした時の確認ポップアップを描画する。
	const char* popupName = "未保存の変更があります###UnsavedExitConfirm";

	// 確認が予約されていれば、次の描画で一度だけポップアップを開きます。
	if (openUnsavedExitConfirm_) {
		ImGui::OpenPopup(popupName);
		openUnsavedExitConfirm_ = false;
	}

	ImGui::SetNextWindowSize(ImVec2(430.0f, 0.0f), ImGuiCond_Appearing);

	// 確認画面が表示されていない場合は、内容と終了操作を描きません。
	if (!ImGui::BeginPopupModal(popupName, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		return;
	}

	ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.32f, 1.0f), ICON_FA_EXCLAMATION_TRIANGLE " この変更はまだ保存されていません。");
	ImGui::Spacing();
	ImGui::TextWrapped("保存されていない変更があります。このまま終了すると、未保存の編集内容が失われる可能性があります。");

	// DebugEditor が利用できる場合は、どの種類の変更が残っているかも表示します。
	if (debugEditor_) {
		ImGui::Spacing();
		ImGui::TextDisabled("未保存: %s", debugEditor_->GetDirtySummaryText().c_str());
	}

	ImGui::Separator();

	// 破棄を選択された場合は、保存処理を挟まずアプリを終了します。
	if (ImGui::Button(ICON_FA_STOP " 保存せず終了", ImVec2(170.0f, 0.0f))) {
		DebugConsole::GetInstance()->AddLog("Exit Warning: 未保存の変更を破棄して終了しました。");
		WinApp::CloseNow();
		ImGui::CloseCurrentPopup();
	}

	ImGui::SameLine();

	// キャンセルが押された場合は、編集状態を保ったまま確認を閉じます。
	if (ImGui::Button(ICON_FA_TIMES " キャンセル", ImVec2(130.0f, 0.0f))) {
		DebugConsole::GetInstance()->AddLog("Exit Warning: 未保存のため終了をキャンセルしました。");
		ImGui::CloseCurrentPopup();
	}

	ImGui::EndPopup();
}

void GameEditorController::UpdateTools(float deltaTime, bool isPlaying, float timeScale) {
    // パーティクル、VFX、メッシュ、デブリ、トレイルなどのエディタツールを更新する。
	SceneManager* sceneManager = SceneManager::GetInstance();
	const bool sceneTransitioning = sceneManager && sceneManager->IsTransitioning();
	const auto* session = actorSession_.get();

	// 敵プレビュー中は同じGPU/Mesh/Particle管理系を敵タイムラインが所有します。
	// 非選択のVFXツールが時間倍率を戻したり実時間更新したりすると、一時停止中の短寿命粒子が消えるため更新を競合させません。
	const bool enemyPreviewOwnsEffects = session && session->IsActive() && !isPlaying;

	const bool simulationPaused = isPlaying && simulationPaused_;

	// シーン遷移中またはゲームが一時停止中なら、演出とエフェクトの時間を進めません。
	if (!sceneTransitioning && !simulationPaused) {
		// Director が利用できる場合は、そのシーン演出の時間を更新します。
		if (ghostDirector_) {
			// ゲーム再生中は時間倍率を合わせ、編集プレビューの時計は通常の経過秒で進める。
			PROFILE_SCOPE("Editor / 演出プレビュー更新");
			ghostDirector_->Update(isPlaying ? deltaTime * timeScale : deltaTime);
		}

		// 敵プレビューがエフェクト管理系を使っていない場合だけ、初期化済みの各エフェクトツールを更新します。
		if (!enemyPreviewOwnsEffects) {
			if (particleEditor_) {
				PROFILE_SCOPE("Editor / CPUパーティクル更新");
				particleEditor_->Update(deltaTime, isPlaying);
			}

			if (gpuParticleEditor_) {
				PROFILE_SCOPE("Editor / GPUパーティクル更新");
				gpuParticleEditor_->Update(deltaTime, isPlaying);
			}

			if (vfxSequencerEditor_) {
				PROFILE_SCOPE("Editor / VFXシーケンサー更新");
				vfxSequencerEditor_->Update(deltaTime);
			}

			if (meshEffectEditor_) {
				PROFILE_SCOPE("Editor / メッシュエフェクト更新");
				meshEffectEditor_->Update(deltaTime);
			}

			if (debrisEffectEditor_) {
				PROFILE_SCOPE("Editor / デブリエフェクト更新");
				debrisEffectEditor_->Update(deltaTime);
			}

			if (trailEmitterEditor_) {
				PROFILE_SCOPE("Editor / トレイル更新");
				trailEmitterEditor_->Update(deltaTime);
			}
		}
	}

	if (debugEditor_ && debugEditor_->GetCaptureToolWindow()) {
		// 録画・撮影操作の入力はシミュレーション停止中にも確認できるよう、演出の更新条件から外している。
		debugEditor_->GetCaptureToolWindow()->SetForceGameViewClientCapture(portfolioCaptureMode_);
		debugEditor_->GetCaptureToolWindow()->UpdateHotkeys();
	}

	// ゲーム再生中で遷移も一時停止もしていない間、実作用のエフェクトをゲーム時間で更新します。
	if (isPlaying && !sceneTransitioning && !simulationPaused) {
		MeshEffectManager::GetInstance()->Update(deltaTime * timeScale);
		GpuParticleManager::GetInstance()->Update(deltaTime * timeScale);
	}
}

void GameEditorController::DrawToolWindows(
	float& timeScale,
	float sceneUpdateTimeMs,
	float cpuCmdTimeMs,
	float drawTimeMs,
	float* updateTimeHistory,
	float* drawTimeHistory,
	std::int32_t timeHistoryIndex) {
    // Hierarchy、Inspector、Project、Console、Statusなどの補助ウィンドウを描画する。
	EditorToolSettings::Get().DrawWindow();

	// 遷移中は通常パネルの描画を止め、参照に依存しない案内と診断だけを出します。
	if (sceneManager_ && sceneManager_->IsTransitioning()) {
		// Sceneの入れ替え中もドックを維持し、旧Sceneを参照する編集UIは描画しません。
		// 編集パネルが表示設定の場合は、空白にせず遷移中の案内を出します。
		if (showDebugWindows_) {
			ImGui::Begin(ICON_FA_SITEMAP " Hierarchy###Hierarchy");
			ImGui::TextDisabled("シーンを切り替えています...");
			ImGui::End();
			ImGui::Begin(ICON_FA_LIST_UL " Sprite Hierarchy");
			ImGui::TextDisabled("シーンを切り替えています...");
			ImGui::End();
			ImGui::Begin("Inspector");
			ImGui::TextDisabled("新しいシーンを読み込み中です");
			ImGui::End();
			ImGui::Begin(ICON_FA_INFO_CIRCLE " Sprite Inspector");
			ImGui::TextDisabled("新しいシーンを読み込み中です");
			ImGui::End();
		}

		ImGui::Begin("Project (Assets)");
		ImGui::TextDisabled("シーン切り替え後にアセット一覧を表示します");
		ImGui::End();

		// コンソールと診断の表示が両方有効なら、遷移中もコンソールを描きます。
		if (showDebugConsole_ && EditorToolSettings::Get().IsVisible("diagnosticWindows")) {
			DebugConsole::GetInstance()->DrawImGui();
		}

		// ステータスと診断の表示が両方有効なら、遷移中も時間計測を描きます。
		if (showTimeController_ && EditorToolSettings::Get().IsVisible("diagnosticWindows")) {
			DrawStatusWindow(timeScale, sceneUpdateTimeMs, cpuCmdTimeMs, drawTimeMs,
				updateTimeHistory, drawTimeHistory, timeHistoryIndex);
		}

		return;
	}

    const bool effectPreviewWorkspaceActive = showEffectPreviewTimeline_ && debugEditor_;
    const bool expandedBottomWorkspace = effectPreviewWorkspaceActive || showCinematicTimeline_;
    // 編集パネルが有効であれば、現在のシーンの階層と選択項目を表示します。
    if (showDebugWindows_) {
        // DebugEditor があれば、下段のタイムライン有無に応じてProjectの表示を切り替えます。
        if (debugEditor_) {
            debugEditor_->SetProjectWindowVisible(!expandedBottomWorkspace);
            debugEditor_->DrawHierarchy();
        }

		EditorManager::GetInstance()->DrawInspector();

		// Sprite編集ツールがある場合は、3Dとは別のSprite階層と詳細を描画します。
		if (spriteDebugEditor_) {
			spriteDebugEditor_->DrawHierarchyWindow();
			spriteDebugEditor_->DrawInspectorWindow();

			// 下段をタイムラインに使っていなければ、Sprite側のProjectも表示します。
			if (!expandedBottomWorkspace) {
				spriteDebugEditor_->DrawProjectWindow();
			}
		}
	}

    // 下段の通常レイアウトでコンソールと診断が有効なときに表示します。
    if (!expandedBottomWorkspace && showDebugConsole_ && EditorToolSettings::Get().IsVisible("diagnosticWindows")) {
		DebugConsole::GetInstance()->DrawImGui();
	}

    // 下部をタイムラインに切り替えても、負荷の計測結果は表示できるようにする。
    if (showTimeController_ && EditorToolSettings::Get().IsVisible("diagnosticWindows")) {
		DrawStatusWindow(timeScale, sceneUpdateTimeMs, cpuCmdTimeMs, drawTimeMs, updateTimeHistory, drawTimeHistory, timeHistoryIndex);
	}



	// 共有エフェクトプレビューが有効なら、下段をそのタイムラインに使います。
	if (effectPreviewWorkspaceActive) {
		PROFILE_SCOPE("Editor / エフェクトタイムラインUI");
		EffectPreviewStage::GetInstance()->DrawTimelineWindow();
	}


    // 演出ツールが選択されDirectorが使える場合は、演出タイムラインを描画します。
    if (showCinematicTimeline_ && ghostDirector_) {
        PROFILE_SCOPE("Editor / 演出タイムラインUI");
        ghostDirector_->DrawTimelinePanel();
    }


	// マニュアルウィンドウを生成済みの場合は、表示状態に応じる描画を呼び出します。
	if (engineManualWindow_) {
		engineManualWindow_->Draw();
	}

    // 診断の表示が有効な場合だけプロファイラを描画対象にします。
    if (EditorToolSettings::Get().IsVisible("diagnosticWindows"))
        ProfilerManager::GetInstance()->DrawImGui();
    // 検索ツールがあれば、検索の開閉と表示はツール側で処理します。
    if (editorQuickSearch_) {
        editorQuickSearch_->Draw();
    }

    // アセット参照ツールがあれば、自身の表示状態で描画するよう依頼します。
    if (assetReferenceExplorer_) {
        assetReferenceExplorer_->Draw();
    }

    // Play中の変更追跡がある場合に、候補の確認UIを描画します。
    if (playModeChangeTracker_) {
        playModeChangeTracker_->Draw();
    }
}


void GameEditorController::DrawStatusWindow(
	float& timeScale,
	float sceneUpdateTimeMs,
	float cpuCmdTimeMs,
	float drawTimeMs,
	float* updateTimeHistory,
	float* drawTimeHistory,
	std::int32_t timeHistoryIndex) {
    // FPS、CPU/GPU負荷、時間倍率などを確認するステータスウィンドウを描画する。
	// ウィンドウが折り畳まれているか非表示なら、Beginに対応するEndを呼んで終了します。
	if (!ImGui::Begin("ステータス", &showTimeController_, ImGuiWindowFlags_HorizontalScrollbar)) {
		ImGui::End();
		return;
	}

	float fps = ImGui::GetIO().Framerate;
	ImGui::Text("FPS: %.1f", fps);
	ImGui::SliderFloat("時間倍率", &timeScale, 0.0f, 2.0f);

	ImGui::Separator();
	ImGui::Text("--- CPU パフォーマンス ---");
	// 完了したEditor更新・シーン更新・描画命令発行を合わせ、60fpsの予算と比較する。
	const float editorUpdateMs = ProfilerManager::GetInstance()->GetLatestCpuTime("Editor更新");
    const float cpuTotalWorkMs = editorUpdateMs + sceneUpdateTimeMs + cpuCmdTimeMs;
    ImGui::Text("Editor更新: %.3f ms", editorUpdateMs);
	ImGui::Text("CPU 稼働合計: %.3f ms", cpuTotalWorkMs);
	ImGui::ProgressBar(cpuTotalWorkMs / 16.66f, ImVec2(0.0f, 0.0f));

	// CPU内訳を展開した場合は、シーン更新と描画命令発行を分けて表示します。
	if (ImGui::TreeNode("詳細内訳 (CPU)")) {
		ImGui::Text("  シーン更新  : %.3f ms", sceneUpdateTimeMs);
		ImGui::Text("  描画命令発行: %.3f ms", cpuCmdTimeMs);
		ImGui::TreePop();
	}

	ImGui::PlotLines(
		"##UpdateGraph",
		updateTimeHistory,
		120,
		timeHistoryIndex,
		"シーン更新の推移 (ms)",
		0.0f,
		16.66f,
		ImVec2(ImGui::GetContentRegionAvail().x, 60.0f));

	ImGui::Separator();
	ImGui::Text("--- GPU パフォーマンス ---");
	// GPU計測値はCPU待機時間とは別に示し、律速箇所を混同しないようにする。
	float gpuTotalMs = engine::DirectXCommon::GetInstance()->GetGpuDrawTimeMs();
	ImGui::Text("GPU 描画時間: %.3f ms (60fps予算比 %.0f%%)", gpuTotalMs, gpuTotalMs / 16.66f * 100.0f);
	ImGui::ProgressBar(gpuTotalMs / 16.66f, ImVec2(0.0f, 0.0f));

	// GPU内訳を展開した場合は、撮影や各描画区間の所要時間を表示します。
	if (ImGui::TreeNode("描画区間別 (GPU)")) {
		const auto* profiler = ProfilerManager::GetInstance();
		ImGui::Text("プリセット撮影 GPU: %.3f ms / CPU: %.3f ms",
			profiler->GetLatestGpuTime("プリセット撮影"), profiler->GetLatestCpuTime("プリセット撮影 CPU"));
		for (const char* name : {"影描画", "  3Dシーン", "  ゲームUI", "  デバッグ", "後処理", "エディタUI"}) {
			ImGui::Text("%s: %.3f ms", name, profiler->GetLatestGpuTime(name));
		}

		ImGui::TreePop();
	}

	float gpuWaitMs = drawTimeMs - cpuCmdTimeMs;
	// フレーム全体から命令発行時間を引いた値には、VSyncやGPU完了待ちも含む。
	ImGui::Text("CPU 待機時間: %.3f ms (VSync待ち含む)", std::max(gpuWaitMs, 0.0f));

	ImGui::PlotLines(
		"##DrawGraph",
		drawTimeHistory,
		120,
		timeHistoryIndex,
		"フレーム全体 (ms)",
		0.0f,
		16.66f,
		ImVec2(ImGui::GetContentRegionAvail().x, 60.0f));

	ImGui::Separator();

	// 描画統計の欄が開いている場合は、最後に完了したフレームの各パスを集計します。
	if (ImGui::CollapsingHeader("描画統計 (Render Stats)", ImGuiTreeNodeFlags_DefaultOpen)) {
		// パス別に記録された直前の完了フレームを合算し、編集中のフレームと混ぜない。
		const engine::RenderFrameStats& frame = engine::RenderStats::GetInstance()->GetLastCompletedFrame();
		uint64_t totalDrawCalls = 0;
		uint64_t totalIndirectDrawCalls = 0;
		uint64_t totalObjects = 0;
		uint64_t totalCulled = 0;
		uint64_t totalTriangles = 0;
		uint64_t totalInstances = 0;
		uint64_t totalDispatches = 0;
		uint64_t totalThreadGroups = 0;

		for (const engine::RenderPassStats& pass : frame.passes) {
			totalDrawCalls += pass.drawCalls;
			totalIndirectDrawCalls += pass.indirectDrawCalls;
			totalObjects += pass.submittedObjects;
			totalCulled += pass.culledObjects;
			totalTriangles += pass.submittedTriangles;
			totalInstances += pass.submittedInstances;
			totalDispatches += pass.computeDispatches;
			totalThreadGroups += pass.computeThreadGroups;
		}

		// 計測フレームが確定していない場合は、未計測として表示します。
		if (frame.frameNumber == 0) {
			ImGui::TextDisabled("描画統計を準備中です。");
		// フレームの計測値がある場合は集計を表示し、表を開ければパス別の値も表示します。
		} else {
			ImGui::Text("計測フレーム: %llu", static_cast<unsigned long long>(frame.frameNumber));
			ImGui::Text(
				"Draw: %llu (Indirect %llu)  Object送信: %llu  カリング: %llu",
				static_cast<unsigned long long>(totalDrawCalls),
				static_cast<unsigned long long>(totalIndirectDrawCalls),
				static_cast<unsigned long long>(totalObjects),
				static_cast<unsigned long long>(totalCulled));
			ImGui::Text(
				"送信Triangle: %llu  Instance: %llu",
				static_cast<unsigned long long>(totalTriangles),
				static_cast<unsigned long long>(totalInstances));

			// ImGuiが表を開始できた場合だけ、描画パス別の行を追加します。
			if (ImGui::BeginTable(
				"RenderStatsByPass",
				6,
				ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
				ImGui::TableSetupColumn("描画パス");
				ImGui::TableSetupColumn("Draw");
				ImGui::TableSetupColumn("Object");
				ImGui::TableSetupColumn("Cull");
				ImGui::TableSetupColumn("Triangle");
				ImGui::TableSetupColumn("Instance");
				ImGui::TableHeadersRow();

				for (size_t passIndex = 0; passIndex < engine::kRenderPassCount; ++passIndex) {
					const engine::RenderPass passType = static_cast<engine::RenderPass>(passIndex);
					const engine::RenderPassStats& pass = frame.passes[passIndex];
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(engine::RenderStats::GetPassDisplayName(passType));
					ImGui::TableSetColumnIndex(1);
					ImGui::Text("%llu", static_cast<unsigned long long>(pass.drawCalls));
					ImGui::TableSetColumnIndex(2);
					ImGui::Text("%llu", static_cast<unsigned long long>(pass.submittedObjects));
					ImGui::TableSetColumnIndex(3);
					ImGui::Text("%llu", static_cast<unsigned long long>(pass.culledObjects));
					ImGui::TableSetColumnIndex(4);
					ImGui::Text("%llu", static_cast<unsigned long long>(pass.submittedTriangles));
					ImGui::TableSetColumnIndex(5);
					ImGui::Text("%llu", static_cast<unsigned long long>(pass.submittedInstances));
				}

				ImGui::EndTable();
			}

			ImGui::Spacing();
			ImGui::Text(
				"有効ライト: Point %u / %zu  Spot %u / %zu",
				frame.activePointLights,
				engine::LightManager::GetInstance()->GetPointLights().size(),
				frame.activeSpotLights,
				engine::LightManager::GetInstance()->GetSpotLights().size());
			ImGui::Text(
				"Shadow Map: %d x %d / 範囲 %.0f",
				engine::DirectXCommon::GetInstance()->GetShadowMapResolution(),
				engine::DirectXCommon::GetInstance()->GetShadowMapResolution(),
				engine::LightManager::GetInstance()->GetShadowAreaSize());
			ImGui::Text(
				"GPU Particle: %u systems / System容量合計 %llu",
				frame.gpuParticleSystems,
				static_cast<unsigned long long>(frame.gpuParticleCapacity));
			ImGui::Text(
				"CPU Particle: %llu / PostEffect Pass: %u",
				static_cast<unsigned long long>(frame.cpuParticleCount),
				frame.postProcessPasses);
			ImGui::Text(
				"Compute Dispatch: %llu / Thread Group: %llu",
				static_cast<unsigned long long>(totalDispatches),
				static_cast<unsigned long long>(totalThreadGroups));
			ImGui::TextDisabled("Draw数はエンジンが発行した命令です。ImGui内部描画は含みません。");
			ImGui::TextDisabled("GPU ParticleのSystem容量合計は生存数ではなく、各プリセットが確保・Compute更新する上限枠の合計です。");
			ImGui::TextDisabled("Indirect Particleの実Instance/Triangle数はGPUが決めるため、上のCPU集計には含みません。");
		}
	}

	ImGui::End();
}

void GameEditorController::CapturePendingThumbnails() {
    // ProjectWindowから要求されたサムネイル撮影を処理する。
	if (debugEditor_ && debugEditor_->GetProjectWindow()) {
		debugEditor_->GetProjectWindow()->CapturePendingThumbnails();
	}
}

void GameEditorController::ExportCapturedHudPortraits() {
	// 撮影したHUD用画像の書き出し処理はProjectWindowに集約する。
	if (debugEditor_ && debugEditor_->GetProjectWindow()) {
		debugEditor_->GetProjectWindow()->ExportCapturedHudPortraits();
	}
}

void GameEditorController::DrawScenePreview(ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource) {
    // 演出ツールがあれば、共有ライトを使って演出用の3Dオブジェクトを描きます。
    if (ghostDirector_) ghostDirector_->DrawSceneObjects(pointLightResource, spotLightResource);
    // エディタ選択中のプレビューやゴースト表示を3Dシーン上に描画する。
	// DebugEditor が利用できる場合だけ、選択とシーンに紐付くプレビューを確認します。
	if (debugEditor_) {
		SceneManager* editorSceneManager = debugEditor_->GetSceneManager();
		const bool isPlaying = editorSceneManager && editorSceneManager->IsPlaying();

		// ゲーム再生中は編集用のゴースト、配置プレビュー、カメラの目印を重ねません。
		if (!isPlaying) {
			engine::Object3D* selectedObject = debugEditor_->GetSelectedObject();

			// 選択中の実体が現在のシーンに含まれなければ、破棄済み参照に触れないよう選択を解除します。
			if (selectedObject && !IsObjectInCurrentScene(editorSceneManager, selectedObject)) {
				debugEditor_->SetSelectedObject(nullptr);

				// 全体のInspectorがDebugEditorを表示中なら、同時にその選択も解除します。
				if (EditorManager::GetInstance()->GetSelectedObject() == debugEditor_.get()) {
					EditorManager::GetInstance()->ClearSelection();
				}

				selectedObject = nullptr;
			}

			// 有効な選択対象にRecorderが付いていれば、参照解決に現在のシーン管理を渡します。
			if (selectedObject && selectedObject->GetRecorder()) {
				selectedObject->GetRecorder()->SetSceneManager(editorSceneManager);
			}

            // ゴーストの表示が有効で、選択対象に再生可能なプレビューデータがある場合だけ描きます。
            if (EditorToolSettings::Get().IsVisible("ghostModels") &&
                selectedObject &&
                selectedObject->GetRecorder() &&
                selectedObject->GetRecorder()->HasPreviewData()) {
				selectedObject->GetRecorder()->DrawObjectGhostPreview(pointLightResource, spotLightResource);
			}

            // 配置補助が表示設定の場合だけ、配置予定の実体を描きます。
            if (EditorToolSettings::Get().IsVisible("placementPreview"))
                debugEditor_->DrawPreview(pointLightResource, spotLightResource);
			// 現在のシーンが取得できたら、描画設定を渡してカメラの位置を示す目印を描きます。
			if (BaseScene* currentScene = editorSceneManager ? editorSceneManager->GetCurrentScene() : nullptr) {
				CameraEditor* cameraEditor = CameraEditor::GetInstance();
				cameraEditor->SetObject3DCommon(currentScene->GetObject3DCommon());
				cameraEditor->DrawCameraModelGizmos(pointLightResource, spotLightResource);
			}
		}
	}

	SceneManager* editorSceneManager = debugEditor_ ? debugEditor_->GetSceneManager() : nullptr;
	const bool isPlaying = editorSceneManager && editorSceneManager->IsPlaying();
    // 編集中でゴースト表示が有効かつ録画ツールを選択している場合は、ツール側のゴーストを描きます。
    if (EditorToolSettings::Get().IsVisible("ghostModels") &&
        !isPlaying &&
        ghostRecorder_ &&
        EditorManager::GetInstance()->GetSelectedObject() == ghostRecorder_.get()) {
		ghostRecorder_->SetSceneManager(editorSceneManager);
		ghostRecorder_->DrawObjectGhostPreview(pointLightResource, spotLightResource);
	}
}

void GameEditorController::DrawSceneDebug(ID3D12GraphicsCommandList* commandList) {
    // DebugEditorやMeshEffectEditorのデバッグ描画を実行する。
    // 編集ツールが一時的に非表示なら、デバッグの補助描画も全て省きます。
    if (EditorToolSettings::Get().IsTemporarilyHidden())
        return;
	// DebugEditor が作成済みなら、その共通のデバッグ描画を呼びます。
	if (debugEditor_) {
		debugEditor_->DrawDebug(commandList);
	}

    // メッシュエフェクト編集を選択中でガイド表示も有効な場合に専用形状を描きます。
    if (meshEffectEditor_ &&
        EditorToolSettings::Get().IsVisible("meshEffectGuides") &&
        EditorManager::GetInstance()->GetSelectedObject() == meshEffectEditor_.get()) {
		meshEffectEditor_->Draw();
	}
    // Spriteの編集矢印もゲームと同じ座標・HDR描画先へ描き、Game Viewと一緒に拡縮します。
    if (spriteDebugEditor_ && (!sceneManager_ || !sceneManager_->IsPlaying()) &&
        EditorToolSettings::Get().IsVisible("spriteGuides")) {
        spriteDebugEditor_->Draw();
    }
}

void GameEditorController::DrawBackBufferUi() {
    // Editor UIだけを実ウィンドウへ描画します。
    ImGuiManager::GetInstance()->Draw();
}

void GameEditorController::EndFrame() {
    // ImGuiのフレームを終了し、描画コマンドを発行できる状態にする。
	ImGuiManager::GetInstance()->EndFrame();
}

void GameEditorController::ApplyCameraInputState(const EditorFrameState& frameState, bool isPlaying) {
    // ギズモやSprite編集中はカメラ入力を止め、操作の競合を防ぐ。
	// アクティブなカメラがある場合に、ゲーム中か編集操作と競合していないときだけ入力を有効にします。
	if (engine::Camera* mainCamera = CameraManager::GetInstance()->GetActiveCamera()) {
		mainCamera->SetInputEnabled(isPlaying || !(frameState.spriteEditorBusy || frameState.gizmoBusy));
	}
}

void GameEditorController::ApplyCameraOverrides() {
    // エフェクトプレビューやアニメーション作業台のカメラ上書きを現在フレームへ反映する。
    // ゲームの一時停止中は、姿勢とプレビュー視点の上書きを進めません。
    if (simulationPaused_) {
        return;
    }

    // 通常アニメーションに上書きされた演出の身体姿勢を描画直前に反映する。
    if (ghostDirector_) {
        ghostDirector_->ApplyBodyPose();
    }

	// DebugEditorからエフェクトのプレビュー空間を取得できる場合は、その視点を反映します。
	if (debugEditor_ && debugEditor_->GetEffectPreviewStage()) {
		debugEditor_->GetEffectPreviewStage()->ApplyCameraOverride();
	}


}

void GameEditorController::SaveAllEditors() {
    // すべてのエディタデータ保存処理の入口。現在はログ出力のみを行う。
	DebugConsole::GetInstance()->AddLog("--- Auto Saving All Editor Data... ---");
	DebugConsole::GetInstance()->AddLog("--- Save Complete! ---");
}

void GameEditorController::UpdateActorTools(float deltaTime) {
    // 選択対象の動作確認を優先する。同じ実体をアニメーションクリップのプレビューからも進めない。
    if (actorSession_ && actorSession_->IsActive() && debugEditor_)
        debugEditor_->GetAnimationClipEditor()->StopPreview();
    // キャラクター確認セッションが作成済みなら、経過秒を渡して進行を更新します。
    if (actorSession_) {
        PROFILE_SCOPE("Editor / キャラクタープレビュー更新");
        actorSession_->Update(deltaTime);
    }
    // DebugEditor が作成済みなら、クリップ編集側の更新も呼びます。
    if (debugEditor_) {
        PROFILE_SCOPE("Editor / アニメーションプレビュー更新");
        debugEditor_->GetAnimationClipEditor()->Update(deltaTime);
    }
}

#endif
