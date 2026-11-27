#include "engine/editor/core/GameEditorController.Internal.h"
#include "engine/editor/scene/DebugTeleportEditor.h"

#ifdef USE_IMGUI
#include "engine/editor/help/ClassDiagramCommand.h"

#include <Windows.h>
#include <shellapi.h>

#include <filesystem>
#include <string>
#include <system_error>

using namespace gameEditorControllerImplementation;

namespace {
namespace fs = std::filesystem;
constexpr auto kClassDiagramPath = L"docs/editor/class-diagram.html";
constexpr auto kEngineManualPath = L"docs/editor/engine-manual.html";

// 指定ディレクトリから親をたどり、各階層の直下と project サブフォルダーにある資料を探す。
fs::path FindHelpDocument(fs::path directory, const fs::path& relativePath) {
    while (!directory.empty()) {
        for (const auto& project : { directory, directory / L"project" }) {
            const auto candidate = project / relativePath;
            std::error_code error;
            // 候補が通常ファイルとして確認できた最初の場所を返す。
            if (fs::is_regular_file(candidate, error)) {
                return candidate;
            }
        }
        const auto parent = directory.parent_path();
        // ファイルシステムのルートまで調べたら、同じ場所を繰り返さないよう終了する。
        if (parent == directory) {
            break;
        }
        directory = parent;
    }
    return {};
}

// ローカルのヘルプ資料を既定のブラウザーへ渡し、Windowsが起動要求を受け付けたかを返します。
bool OpenHelpDocument(const fs::path& relativePath, const char* label) {
    std::error_code error;
    auto guide = FindHelpDocument(fs::current_path(error), relativePath);

    // Visual Studio以外から起動すると作業フォルダーが異なるため、EXEの位置からも探します。
    if (guide.empty()) {
        std::wstring executable(32768, L'\0');
        const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        // 取得に成功し、パスがバッファーに収まった場合にだけ EXE の親から探す。
        if (length > 0 && length < executable.size()) {
            executable.resize(length);
            guide = FindHelpDocument(fs::path(executable).parent_path(), relativePath);
        }
    }
    // 両方の起点から探しても見つからなければ、相対パスをログに出して起動を中止する。
    if (guide.empty()) {
        const auto path = relativePath.generic_u8string();
        DebugConsole::GetInstance()->AddLog(std::string(label) + "が見つかりません: " + std::string(path.begin(), path.end()));
        return false;
    }

    // 32以下はWindowsからの起動エラーです。日本語を含むパスもワイド文字のまま渡します。
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", guide.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
        const auto path = guide.generic_u8string();
        DebugConsole::GetInstance()->AddLog(std::string(label) + "を開けません: " + std::string(path.begin(), path.end()));
        return false;
    }
    return true;
}
} // namespace

// エディターのコマンドとツール登録。
void GameEditorController::RegisterEditorCommands() {
	EditorCommandRegistry* registry = EditorCommandRegistry::GetInstance();

	// 再初期化時に旧ラムダが this を保持し続けないよう、同じ Owner の登録を先に解除します。
	registry->UnregisterOwner(this);

	// Play/Stop と実行中の一時停止は Active Scene の実行状態を確認してから有効にします。
	registry->Register({
		editorCommandId::kPlay, "再生", "Game", "Ctrl+P",
		"Active Sceneをゲームモードで再生します。", { "play", "game", "実行" },
		[this]() {
			return activePlayState_ && !*activePlayState_ && sceneManager_ && !sceneManager_->IsTransitioning();
		},
		[this]() { RequestPlay(sceneManager_, *activePlayState_, activeSceneName_); }, this });

	registry->Register({
		editorCommandId::kStop, "停止", "Game", "Ctrl+P",
		"ゲームモードを停止して編集Sceneへ戻ります。", { "stop", "game", "停止" },
		[this]() { return activePlayState_ && *activePlayState_; },
		[this]() { StopPlay(); }, this });
    registry->Register({
        editorCommandId::kPauseResume, "実行を一時停止／再開", "Game", "",
        "ゲームの経過時間を止めるか再開します。", { "pause", "resume", "一時停止" },
        [this]() { return activePlayState_ && *activePlayState_; },
        [this]() { simulationPaused_ = !simulationPaused_; }, this });

	// Scene の保存と Undo/Redo は編集可能な状態に限定し、実操作は DebugEditor に委譲します。
	registry->Register({
		editorCommandId::kSceneNew, "新規Scene Asset", "Scene", "",
		"新しいScene Assetの作成画面を開きます。", { "scene", "new", "作成" },
		[this]() { return CanEditScene(); },
		[this]() { showDebugWindows_ = true; debugEditor_->OpenCreateSceneAssetDialog(); }, this });

	registry->Register({
		editorCommandId::kSceneSave, "Active Sceneを保存", "Scene", "Ctrl+S",
		"現在のScene Object、Camera、Spriteを保存します。", { "scene", "save", "保存" },
		[this]() { return CanEditScene(); },
		[this]() { debugEditor_->SaveScene(SaveMode::kAll); }, this });

	registry->Register({
		editorCommandId::kEditUndo, "元に戻す", "Edit", "Ctrl+Z",
		"直前のEditor操作を取り消します。", { "undo", "戻す" },
		[this]() { return CanEditScene() && EditorTransactionManager::GetInstance()->CanUndo(); },
		[this]() { debugEditor_->PerformUndo(); }, this });

	registry->Register({
		editorCommandId::kEditRedo, "やり直す", "Edit", "Ctrl+Y",
		"取り消したEditor操作を再適用します。", { "redo", "やり直す" },
		[this]() { return CanEditScene() && EditorTransactionManager::GetInstance()->CanRedo(); },
		[this]() { debugEditor_->PerformRedo(); }, this });

	// 選択 Object への操作は共通の CanEditSelectedObject で適用条件を揃えます。
	registry->Register({
		editorCommandId::kEditDuplicate, "選択Objectを複製", "Edit", "Ctrl+D / Ctrl+C",
		"選択中のObjectと選択中Hierarchyを複製します。", { "duplicate", "copy", "複製" },
		[this]() { return CanEditSelectedObject(); },
		[this]() { debugEditor_->DuplicateSelected(); }, this });

	registry->Register({
		editorCommandId::kEditDelete, "選択Objectを削除", "Edit", "Delete",
		"選択中のObjectをSceneから削除します。", { "delete", "remove", "削除" },
		[this]() { return CanEditSelectedObject(); },
		[this]() { debugEditor_->DeleteSelected(); }, this });

	registry->Register({
		editorCommandId::kObjectDropToFloor, "選択Objectを床へ移動", "Object", "End",
		"選択Objectを直下のColliderへ接地させます。", { "drop", "floor", "ground", "床" },
		[this]() { return CanEditSelectedObject(); },
		[this]() { debugEditor_->DropToFloor(); }, this });


    // 検索・参照確認・一時表示は Scene の保存内容を変えない閲覧操作として登録します。
    registry->Register({
        editorCommandId::kViewQuickSearch, "統合コマンドパレット", "View", "Ctrl+K",
        "操作、Scene Object、Asset、Scene、Preset、Editorウィンドウを横断検索します。",
        { "search", "command", "object", "asset", "検索" }, {},
        // 検索ウィンドウが生成済みの場合に開く。
        [this]() { if (editorQuickSearch_) editorQuickSearch_->Open(); }, this });

    registry->Register({
        editorCommandId::kViewAssetReferences, "Asset参照エクスプローラー", "View", "",
        "Assetの参照元、動的参照候補、Missing参照を横断表示します。",
        { "asset", "reference", "missing", "参照" },
        [this]() { return assetReferenceExplorer_ != nullptr; },
        [this]() { assetReferenceExplorer_->Open(); }, this });

    registry->Register({
        editorCommandId::kViewIsolateSelection, "選択Objectのみ表示", "View", "",
        "選択Objectとその子だけを作業用に表示します。Sceneデータは変更しません。",
        { "isolate", "selection", "隔離", "一時表示" },
        [this]() { return sceneWorkspace_ && CanEditSelectedObject(); },
        [this]() { sceneWorkspace_->IsolateSelection(); }, this });

    registry->Register({
        editorCommandId::kViewRestoreTemporaryVisibility, "一時非表示をすべて復元", "View", "",
        "隔離表示、Layer表示、個別の一時非表示をすべて解除します。",
        { "show", "restore", "visibility", "全表示" },
        [this]() { return sceneWorkspace_ && sceneWorkspace_->GetTemporaryHiddenCount() > 0; },
        [this]() { sceneWorkspace_->RestoreAllTemporaryVisibility(); }, this });

    // 作業レイアウトは同じコマンド窓口から切り替え、各ツールの配置を Workspace 側に任せます。
    registry->Register({
        editorCommandId::kWorkspaceVfx, "VFX編集ワークスペース", "Workspace", "",
        "VFX Sequencerを開き、保存済みのVFX編集レイアウトを適用します。",
        { "vfx", "effect", "layout", "エフェクト" },
        [this]() { return CanEditScene(); },
        [this]() { ApplyWorkspacePreset(WorkspacePreset::kVfx); }, this });
    registry->Register({
        editorCommandId::kWorkspaceAnimation, "アニメーション編集ワークスペース", "Workspace", "",
        "Animation Workbenchを開き、保存済みのアニメーション編集レイアウトを適用します。",
        { "animation", "motion", "layout", "アニメーション" },
        [this]() { return CanEditScene(); },
        [this]() { ApplyWorkspacePreset(WorkspacePreset::kAnimation); }, this });

	// ウィンドウ表示と Help は Scene 編集可否とは独立した操作として公開します。
	registry->Register({
		editorCommandId::kViewEditorPanels, "Hierarchy / Inspector表示", "View", "",
		"主要Editorパネルの表示を切り替えます。", { "hierarchy", "inspector", "window" }, {},
		[this]() { showDebugWindows_ = !showDebugWindows_; }, this });
	registry->Register({
		editorCommandId::kViewConsole, "デバッグログ", "View", "",
		"Debug Consoleの表示を切り替えます。", { "console", "log", "ログ" }, {},
		[this]() { showDebugConsole_ = !showDebugConsole_; }, this });
	registry->Register({
		editorCommandId::kViewStatus, "ステータス", "View", "",
		"CPU/GPUステータスの表示を切り替えます。", { "status", "performance", "fps" }, {},
		[this]() { showTimeController_ = !showTimeController_; }, this });
	registry->Register({
		editorCommandId::kViewPortfolio, "ポートフォリオ撮影モード", "View", "F10",
		"Editor UIを隠してゲーム画面だけを表示します。", { "capture", "portfolio", "撮影" }, {},
		[this]() { SetPortfolioCaptureMode(!portfolioCaptureMode_); }, this });

	registry->Register({
		editorCommandId::kHelpManual, "エンジン説明書", "Help", "",
		"エンジン説明書をブラウザーで開きます。", { "manual", "help", "説明書" }, {},
		[this]() {
			// HTMLを開けない環境でも、エディター内で従来の操作説明を読めるようにします。
			if (!OpenHelpDocument(kEngineManualPath, "エンジン説明書") && engineManualWindow_) {
				engineManualWindow_->Open();
			}
		}, this });
	registry->Register({
		editorCommandId::kHelpClassDiagram, "クラス図", "Help", "",
		"CG2のクラス設計ガイドをブラウザーで開きます。", { "class", "uml", "設計", "class diagram" }, {},
		[]() { OpenHelpDocument(kClassDiagramPath, "クラス図"); }, this });
	registry->Register({
		editorCommandId::kHelpProfiler, "システムプロファイラ", "Help", "",
		"CPU/GPUシステムプロファイラを開きます。", { "profiler", "performance", "性能" }, {},
		[]() { ProfilerManager::GetInstance()->Open(); }, this });
}

// 生成済みツールを設定画面へ登録し、起動時の対象切り替えを共通化する。

void GameEditorController::RegisterEditorTools() {
    auto& settings = EditorToolSettings::Get();
    settings.Clear();

    // Scene 切替中にはツールの対象が入れ替わるため、共通の可用性判定で開く操作を止めます。
    auto available = [this]() { return debugEditor_ && sceneManager_ && !sceneManager_->IsTransitioning(); };
    auto add = [&](const char* id, const char* name, const char* category, IEditable* target, bool preserve = false) {
        // 対象のインスタンスがないツールは起動一覧へ載せない。
        if (!target)
            return;
        settings.AddTool(std::string("tool.") + id, name, category, [this, target, preserve]() {
            showDebugWindows_ = true;
			// Inspector をツールへ切り替える前に、preserve を指定しないツールでは Object の選択を解除する。
			// Cinematic Timeline とデバッグ移動エディターは preserve を指定して、現在の Object 選択を保持する。
            if (!preserve)
                debugEditor_->SetSelectedObject(nullptr);
            EditorManager::GetInstance()->SetSelectedObject(target);
        }, available);
    };

    // 共通の起動経路を使う Editor ツールをカテゴリごとに登録します。
    add("camera", "カメラ設定", "ステージ", CameraEditor::GetInstance());
    add("debugTeleport", "デバッグ移動エディター", "ステージ", &DebugTeleportEditor::GetInstance(), true);
    add("light", "ライティング設定", "ステージ", LightEditor::GetInstance());
    add("post", "ポストエフェクト", "演出", postEffectEditor_.get());
    add("gpu", "GPUパーティクル", "演出", gpuParticleEditor_.get());
    add("particle", "通常パーティクル", "演出", particleEditor_.get());
    add("vfx", "VFXシーケンサー", "演出", vfxSequencerEditor_.get());
    add("director", "演出タイムライン（Cinematic Timeline）", "演出", ghostDirector_.get(), true);
    add("mesh", "メッシュエフェクト", "演出", meshEffectEditor_.get());
    add("debris", "3D破片エフェクト", "演出", debrisEffectEditor_.get());
    add("trail", "トレイルエミッター", "演出", trailEmitterEditor_.get());

    add("animator", "Animator Controller", "演出", debugEditor_->GetAnimatorControllerEditor());
    add("text", "テキストPNG生成", "素材", debugEditor_->GetTextSpriteGenerator());
    add("assets", "アセット監査", "検証・設定", debugEditor_->GetAssetAuditWindow());
    add("audio", "音声設定", "演出", debugEditor_->GetAudioSettingsWindow());
    add("package", "実行ファイルセット", "検証・設定", debugEditor_->GetExecutablePackageWindow());
    add("capture", "キャプチャツール", "検証・設定", debugEditor_->GetCaptureToolWindow());
    add("gameData", "内部データ編集", "検証・設定", debugEditor_->GetGameDataDebugEditor());

    settings.AddTool("tool.actor", "選択対象の動作・データ調整", "戦闘", [this]() {
        showDebugWindows_ = true;
        // 選択対象の編集セッションが存在する場合だけ、選択中の Object を渡す。
        if (auto* session = debugEditor_->GetActorSession())
            session->SetTarget(debugEditor_->GetSelectedObject());
    }, [this, available]() { return available() && debugEditor_->GetSelectedObject(); });

    // Ghost Recorder は現在の選択 Object を編集対象として受け渡します。
    settings.AddTool("tool.recorder", "移動軌跡の記録（Ghost Recorder）", "演出", [this]() {
        showDebugWindows_ = true;
        // 選択がある場合だけ Recorder の記録対象を差し替える。
        if (auto* object = debugEditor_->GetSelectedObject())
            ghostRecorder_->SetTarget(object);
        debugEditor_->SetSelectedObject(nullptr);
        EditorManager::GetInstance()->SetSelectedObject(ghostRecorder_.get());
    }, available);
    settings.AddTool("tool.game", "ゲーム設定", "戦闘", [this]() {
        showDebugWindows_ = true;
        debugEditor_->SetSelectedObject(nullptr);
        EditorManager::GetInstance()->SetSelectedObject(sceneManager_->GetCurrentScene());
    }, [this, available]() { return available() && sceneManager_->GetCurrentScene(); });

    // 可視化フラグは各ツールが持つ実際の状態へのポインタを渡し、設定画面と表示を同期します。
    using VisibilityGroup = EditorToolSettings::VisibilityGroup;
    settings.AddVisibility(VisibilityGroup::kCollisionEvents, "collision", "当たり判定の補助線", debugEditor_->GetDrawCollidersPtr());
    settings.AddVisibility(VisibilityGroup::kCollisionEvents, "invisibleBounds", "透明な壁の補助線", debugEditor_->GetDrawInvisibleBoundsPtr());
    settings.AddVisibility(VisibilityGroup::kSceneEditing, "sceneIcons", "シーン内の盾・カメラ等のアイコン", debugEditor_->GetDrawSceneIconsPtr());
    settings.AddVisibility(VisibilityGroup::kSceneEditing, "helperModels", "透明壁・パーティクル配置用の仮モデル");
    settings.AddVisibility(VisibilityGroup::kSceneEditing, "selectionBounds", "選択オブジェクトの枠・ラベル", debugEditor_->GetDrawSelectionBoundsPtr());
    settings.AddVisibility(VisibilityGroup::kCollisionEvents, "eventIds", "イベントID", debugEditor_->GetDrawEventIdsPtr());
    settings.AddVisibility(VisibilityGroup::kSceneEditing, "light", "ライトの目印", LightEditor::GetInstance()->GetGizmoVisiblePtr());
    auto* camera = CameraEditor::GetInstance();
    settings.AddVisibility(VisibilityGroup::kCamera, "cameraPreview", "カメラ設定内の映像プレビュー", camera->GetCameraPreviewVisiblePtr());
    settings.AddVisibility(VisibilityGroup::kCamera, "sceneCameraPreview", "選択カメラの映像");
    settings.AddVisibility(VisibilityGroup::kSceneEditing, "objectOrientation", "選択オブジェクトの向き・軸ラベル");
    settings.AddVisibility(VisibilityGroup::kSceneEditing, "transformGizmo", "移動・回転・拡縮の操作ギズモ");
    settings.AddVisibility(VisibilityGroup::kSceneEditing, "placementPreview", "配置・ブラシのプレビューと補助線");
    settings.AddVisibility(VisibilityGroup::kSceneEditing, "spriteGuides", "Sprite編集の選択枠・操作ハンドル");
    settings.AddVisibility(VisibilityGroup::kAnimationEffects, "meshEffectGuides", "メッシュエフェクトの補助表示");
    settings.AddVisibility(VisibilityGroup::kDiagnostics, "diagnosticWindows", "デバッグログ・性能・イベント計測ウィンドウ");
    settings.AddVisibility(VisibilityGroup::kCamera, "cameraGuide", "視野ガイド", camera->GetCameraGuideVisiblePtr());
    settings.AddVisibility(VisibilityGroup::kCamera, "cameraBody", "カメラ本体", camera->GetCameraBodyVisiblePtr());
    settings.AddVisibility(VisibilityGroup::kCamera, "orbitGuide", "カメラ旋回ガイド", camera->GetOrbitGuideVisiblePtr());
    settings.AddVisibility(VisibilityGroup::kCamera, "orbitGizmo", "カメラ中心ギズモ", camera->GetOrbitGizmoVisiblePtr());
    settings.AddVisibility(VisibilityGroup::kAnimationEffects, "ghostPath", "ゴースト軌跡の表示（全体）", &ghostPathVisible_);
    settings.AddVisibility(VisibilityGroup::kAnimationEffects, "ghostModels", "ゴースト残像モデル（全体）", &ghostModelVisible_);
    settings.AddVisibility(VisibilityGroup::kAnimationEffects, "directorTrack", "ムービーの選択トラック軌跡", ghostDirector_->GetTrackPreviewPtr());
    settings.AddVisibility(VisibilityGroup::kAnimationEffects, "directorPose", "ムービーの姿勢ガイド", ghostDirector_->GetOrientationPreviewPtr());
    settings.AddVisibility(VisibilityGroup::kSceneEditing, "direction", "右上の方向ギズモ", &sceneDirectionVisible_);
    auto* animation = debugEditor_->GetAnimationClipEditor();
    settings.AddVisibility(VisibilityGroup::kAnimationEffects, "bones", "アニメーションのボーン", animation->GetBoneOverlayPtr());
    settings.AddVisibility(VisibilityGroup::kAnimationEffects, "boneNames", "ボーン名", animation->GetBoneNamesPtr());
    settings.AddVisibility(VisibilityGroup::kAnimationEffects, "boneAxes", "ボーンの選択軸", animation->GetBoneAxesPtr());
    auto* stage = debugEditor_->GetEffectPreviewStage();
    settings.AddVisibility(VisibilityGroup::kAnimationEffects, "previewGrid", "エフェクト確認ステージのグリッド", stage->GetGridVisiblePtr());
    settings.AddVisibility(VisibilityGroup::kAnimationEffects, "previewAxes", "エフェクト確認ステージの座標軸", stage->GetAxesVisiblePtr());

    // 登録後に保存済みの設定を読み、対象フラグへ反映します。
    settings.Load();
}


#endif
