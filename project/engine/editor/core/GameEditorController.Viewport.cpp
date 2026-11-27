#include "engine/editor/core/GameEditorController.Internal.h"
#include <numbers>
#include <format>

#ifdef USE_IMGUI
#include "SelectedActorSession.h"
#include "game/ui/SettingsMenuOverlay.h"
using namespace gameEditorControllerImplementation;

// プレビュー描画から呼び出す、同じ実装ファイル内の補助処理です。
void DrawCameraObjectPreviewWindow();

namespace {

// Game Viewの画像の内側にラベルを収め、隣接するEditorパネルに重ねない。
void DrawCameraLabel(const GameViewArea& area, ImVec2 anchor, const char* text, ImU32 accent) {
    constexpr float kPadding = 6.0f;
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const float width = (std::min)(area.width, textSize.x + kPadding * 2.0f);
    const float height = textSize.y + kPadding * 2.0f;
    // 余白を除いて文字を置けない幅、または必要な高さに満たない場合はラベルを描かない。
    if (width <= kPadding * 2.0f || area.height < height) return;
    // ラベルの左上は、幅と高さを引いた分までしか動かせない。画像の右端や下端からはみ出さない位置へ丸める。
    const ImVec2 min(
        std::clamp(anchor.x, area.screenX, area.screenX + (area.width - width)),
        std::clamp(anchor.y, area.screenY, area.screenY + (area.height - height)));
    const ImVec2 max(min.x + width, min.y + height);
    auto* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(min, max, IM_COL32(18, 23, 30, 224), 4.0f);
    drawList->AddRectFilled(min, ImVec2(min.x + 3.0f, max.y), accent, 2.0f);
    drawList->PushClipRect(min, max, true);
    drawList->AddText(ImVec2(min.x + kPadding, min.y + kPadding), IM_COL32(244, 247, 250, 255), text);
    drawList->PopClipRect();
}

// 範囲の天面中央を投影し、選択したCamera Objectの名前と優先度を表示する。
void DrawSelectedCameraRegionLabel(const GameViewArea& area, engine::Camera* viewCamera,
    const engine::Object3D& object) {
    const auto& settings = object.GetSceneCameraSettings();
    // 投影用カメラがない場合や、表示中で範囲が有効な Camera Object 以外ではラベルを出さない。
    if (!viewCamera || !object.IsCameraObject() || !object.GetIsVisible() || !settings.region.enabled) return;
    const auto& region = settings.region;
    // 範囲のY中心に半分の高さを足し、ワールド座標で天面中央の目印を作る。
    const auto labelAnchor = region.center + engine::Vector3{0, region.size.y * 0.5f, 0};
    const auto viewProjection = viewCamera->GetViewProjectionMatrix();
    ImVec2 screen;
    const bool anchorVisible = ProjectWorldToGameView(labelAnchor, viewProjection, area, screen) &&
        screen.x >= area.screenX && screen.x <= area.screenX + area.width &&
        screen.y >= area.screenY && screen.y <= area.screenY + area.height;
    // 大きな範囲の天面が画面外でも、選択したカメラと優先度は確認できる。
    const ImVec2 labelPosition = anchorVisible
        ? ImVec2(screen.x + 6.0f, screen.y - ImGui::GetTextLineHeight() - 12.0f)
        : ImVec2(area.screenX + 10.0f, area.screenY + 10.0f);
    const std::string label = std::format("{}  |  優先度 {}{}", object.GetName(), region.priority,
        settings.enabled ? "" : "  |  無効");
    const auto& color = region.color;
    const ImU32 accent = ImGui::ColorConvertFloat4ToU32(ImVec4(color.x, color.y, color.z, color.w));
    DrawCameraLabel(area, labelPosition, label.c_str(), accent);
}

}

// ゲームビューとプレビューの表示・操作。
EditorFrameState GameEditorController::DrawGameView(SceneManager* sceneManager, bool isPlaying) {
	EditorFrameState frameState;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::Begin("Game View", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	{
		const ImVec2 displaySize = ImGui::GetContentRegionAvail();
		const ImVec2 canvasScreenPos = ImGui::GetCursorScreenPos();

		// 表示領域に正の幅と高さがある場合だけ、画像の配置と入力を処理する。
		if (displaySize.x > 0.0f && displaySize.y > 0.0f) {
			const bool sceneTransitioning = sceneManager && sceneManager->IsTransitioning();
			DrawViewportLetterbox(canvasScreenPos, displaySize);
			// パネル内に縦横比固定の画像矩形を作る。以降の入力やラベルの基準は黒帯を除いた画像の領域。
			const GameViewArea area = CalculateFixedAspectArea(canvasScreenPos, displaySize);
			const uint32_t texHandle = PostEffect::GetInstance()->GetSrvHandle(1);
			const D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = engine::SrvManager::GetInstance()->GetGpuDescriptorHandle(texHandle);
			ImGui::SetCursorScreenPos(ImVec2(area.screenX, area.screenY));
			ImGui::Image((ImTextureID)gpuHandle.ptr, ImVec2(area.width, area.height));
			const ImVec2 imageMin = ImGui::GetItemRectMin();
			const ImVec2 imageMax = ImGui::GetItemRectMax();
			const bool imageHovered = ImGui::IsItemHovered();
			const ImVec2 mousePos = ImGui::GetIO().MousePos;
			const bool canEditViewport = !isPlaying && !sceneTransitioning;
            const bool actorToolsActive = debugEditor_ &&
                ((debugEditor_->GetActorSession() && debugEditor_->GetActorSession()->IsActive()) ||
                 debugEditor_->GetAnimationClipEditor()->IsEnabled());

            // 編集可能でアクター用セッションがある場合は、攻撃位置の確認用表示を画像上に重ねる。
            if (canEditViewport && debugEditor_ && debugEditor_->GetActorSession())
                debugEditor_->GetActorSession()->DrawOverlay(area.screenX, area.screenY, area.width, area.height);

            // ドロップ処理より先に、このフレームの矩形とマウスを確定する。
            // Prefabも他の素材と同じ座標を使い、リサイズ時の前フレーム値を残さない。
            // 編集器が接続されている場合は、ドロップが使う領域とマウス位置を先に渡す。
            if (debugEditor_) {
                debugEditor_->SetGameViewRegion({area.screenX, area.screenY}, {area.width, area.height});
                debugEditor_->SetGameViewMousePos({mousePos.x - area.screenX, mousePos.y - area.screenY});
            }
			// 編集可能でアクターやクリップのプレビュー操作中ではない場合だけ、素材のドロップを受け付ける。
			if (canEditViewport && !actorToolsActive) {
				HandleGameViewDropTargets(sceneManager, area);
			}

			// 編集器が接続されていれば、重ね描きや入力の基準に使う画像の範囲を更新する。
			if (debugEditor_) {
				debugEditor_->SetGameViewScreenRect(imageMin.x, imageMin.y, imageMax.x, imageMax.y);

				// 遷移中でない場合だけ、シーン編集器のフレーム更新と入力確認を行う。
				if (!sceneTransitioning) {
					debugEditor_->SetGameViewHovered(canEditViewport && imageHovered);

                    // 参照の整理は継続し、動作・クリップ確認中の配置入力を止めます。
                    // 再生時計はScene更新後のUpdateActorToolsで進めます。
                    debugEditor_->Update();
					const bool openCreateMenu = ImGui::IsKeyPressed(ImGuiKey_Tab, false) ||
						ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
					// 編集可能な画像上でギズモと重ならず、Tab またはダブルクリックが入ったら作成メニューを開く。
					if (canEditViewport && !actorToolsActive && imageHovered && !ImGuizmo::IsOver() && openCreateMenu) {
						debugEditor_->OpenGameViewCreateContextMenu();
					}

					// 編集可能でアクター系ツールが使っていない間だけ、作成メニューを描画する。
					if (canEditViewport && !actorToolsActive) {
						debugEditor_->DrawGameViewCreateContextMenu();
					}

					frameState.gizmoBusy = canEditViewport && ImGuizmo::IsUsing();
				}
				// シーン遷移中は編集を更新せず、マウスを画像上として扱う状態を解除する。
				else {
					debugEditor_->SetGameViewHovered(false);
				}
			}

            // 通常の編集が可能で、スプライトの補助表示が有効な場合だけ、スプライト用の入力座標を作る。
            if (spriteDebugEditor_ && canEditViewport && !actorToolsActive && EditorToolSettings::Get().IsVisible("spriteGuides")) {
				// ImGuiの画面座標から画像の左上を引き、表示倍率を戻してゲーム側のピクセル座標に換算する。
				const engine::Vector2 spriteLocalPos = {
					(mousePos.x - area.screenX) * (static_cast<float>(WinApp::GetClientWidth()) / area.width),
					(mousePos.y - area.screenY) * (static_cast<float>(WinApp::GetClientHeight()) / area.height),
				};

				spriteDebugEditor_->Update(spriteLocalPos, imageHovered);
				frameState.spriteEditorBusy = spriteDebugEditor_->IsMouseBusy();
			}

			// 編集可能なときに、ゴースト経路・カメラ範囲・選択オブジェクトのガイドを重ねる。
			if (canEditViewport) {
				DrawGhostPreview(false, area);
				engine::Camera* activeCamera = CameraManager::GetInstance()->GetActiveCamera();
				engine::Object3D* selectedObject = debugEditor_ && !actorToolsActive ? debugEditor_->GetSelectedObject() : nullptr;

				// 選択が現在シーンの所有する実体でなければ、この描画では使わない。
				if (!IsObjectInCurrentScene(sceneManager, selectedObject)) {
					selectedObject = nullptr;
				}

				CameraEditor* cameraEditor = CameraEditor::GetInstance();

				// 選択した実体が Camera Object の場合は、カメラ編集器へ共有する。
				if (selectedObject && selectedObject->IsCameraObject()) {
					cameraEditor->SetSelectedCameraObject(selectedObject);
				}
				// Camera Object を選んでおらず、カメラ編集器自身も選択されていない場合は、保持したカメラ選択を外す。
				else if (EditorManager::GetInstance()->GetSelectedObject() != cameraEditor) {
					cameraEditor->SetSelectedCameraObject(nullptr);
				}

                // 選択があり設定画面が閉じていて、共通と個別のカメラガイド設定が両方有効なら範囲名を描く。
                if (selectedObject && !SettingsMenuOverlay::IsAnyActive() &&
                    EditorToolSettings::Get().IsVisible("cameraGuide") &&
                    *cameraEditor->GetCameraGuideVisiblePtr()) {
                    DrawSelectedCameraRegionLabel(area, activeCamera, *selectedObject);
                }

                // 向きの補助表示が有効なら、選択物の方向を描く。
                if (EditorToolSettings::Get().IsVisible("objectOrientation"))
                    DrawSelectedObjectOrientation(area, activeCamera, selectedObject);
                    // シーン方向の補助表示が有効なら、選択物のガイドとは独立して軸を描く。
                    if (EditorToolSettings::Get().IsVisible("direction"))
                        DrawSceneDirectionGizmo(area, activeCamera);
			}
            // プレイ中に遷移も設定表示もなく、カメラガイドが有効な場合だけ、カメラ領域の状態を表示する。
            if (isPlaying && !sceneTransitioning && !SettingsMenuOverlay::IsAnyActive() &&
                EditorToolSettings::Get().IsVisible("cameraGuide")) {
                const auto& status = CameraManager::GetInstance()->GetRegionStatus();
                DrawCameraLabel(area, ImVec2(area.screenX + 10.0f, area.screenY + 10.0f),
                    status.c_str(), IM_COL32(110, 206, 244, 255));
            }
		}
	}

	ImGui::End();
	ImGui::PopStyleVar();

	// パネルの大きさではなく、制作画面の16:9をカメラへ常に適用します。
	if (engine::Camera* camera = CameraManager::GetInstance()->GetActiveCamera()) {
		camera->SetAspectRatio(kEditorViewportAspect);
		camera->UpdateProjectionMatrix();
	}

	// 編集時で遷移中でない場合に、選択した Camera Object の別ウィンドウを描く。
	if (!isPlaying && (!sceneManager || !sceneManager->IsTransitioning())) {
		DrawCameraObjectPreviewWindow();
	}

	return frameState;
}


// 選択中のCamera Objectが実際に描画する画面を、通常のEditorウィンドウとして表示する。
void DrawCameraObjectPreviewWindow() {
    // シーンカメラが選択され、プレビュー用の描画先がある間だけ専用ウィンドウを出す。
    // ツール共通の表示設定が無効なら、プレビューウィンドウを開かない。
    if (!EditorToolSettings::Get().IsVisible("sceneCameraPreview"))
        return;
	CameraEditor* cameraEditor = CameraEditor::GetInstance();
	engine::Object3D* cameraObject = cameraEditor ? cameraEditor->GetSelectedCameraObject() : nullptr;

	// 選択がない場合やプレビューとして描画できない場合は、専用ウィンドウを出さない。
	if (!cameraObject || !cameraEditor->HasSceneCameraPreviewTarget()) {
		return;
	}

	ImGui::SetNextWindowSize(ImVec2(560.0f, 390.0f), ImGuiCond_FirstUseEver);
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

	// ウィンドウの内容を描ける場合だけ、現在のカメラ名や画像を表示する。
	if (ImGui::Begin("Camera Object Preview", nullptr, flags)) {
		const SceneCameraSettings& settings = cameraObject->GetSceneCameraSettings();
		const char* roleLabel = settings.role == SceneCameraRole::kMain ? "Main" : "Cinematic";
		constexpr float kRadToDeg = 180.0f / std::numbers::pi_v<float>;

		ImGui::TextColored(
			ImVec4(0.35f, 0.85f, 1.0f, 1.0f),
			ICON_FA_VIDEO " %s",
			cameraObject->GetName().c_str());
		ImGui::SameLine();
		ImGui::TextDisabled("[%s]  FOV %.1f°", roleLabel, settings.fovY * kRadToDeg);
		ImGui::Separator();

		// 画像用の子ウィンドウを描画できる間だけ、表示寸法を計算する。
		if (ImGui::BeginChild(
			"CameraObjectPreviewCanvas",
			ImVec2(0.0f, 0.0f),
			true,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
			const ImVec2 canvasSize = ImGui::GetContentRegionAvail();

			// 表示領域の両辺が1ピクセルを超える場合だけ、画像を配置する。
			if (canvasSize.x > 1.0f && canvasSize.y > 1.0f) {
				// 描画結果の16:9比率を保ち、余白はキャンバス中央へ均等に置く。
				constexpr float kAspect = 16.0f / 9.0f;
				float previewWidth = canvasSize.x;
				float previewHeight = previewWidth / kAspect;

				// 幅基準で収まらない場合は高さを上限に再計算し、16:9 を保つ。
				if (previewHeight > canvasSize.y) {
					previewHeight = canvasSize.y;
					previewWidth = previewHeight * kAspect;
				}

				const ImVec2 canvasCursor = ImGui::GetCursorPos();
				const ImVec2 canvasScreenPos = ImGui::GetCursorScreenPos();
				ImGui::GetWindowDrawList()->AddRectFilled(
					canvasScreenPos,
					ImVec2(canvasScreenPos.x + canvasSize.x, canvasScreenPos.y + canvasSize.y),
					IM_COL32(18, 20, 25, 255));
				ImGui::SetCursorPos(ImVec2(
					canvasCursor.x + (canvasSize.x - previewWidth) * 0.5f,
					canvasCursor.y + (canvasSize.y - previewHeight) * 0.5f));

				// Game View と同じ SRV ではなく、選択カメラで別途描画したプレビュー専用テクスチャを表示する。
				const uint32_t textureHandle = PostEffect::GetInstance()->GetSrvHandle(
					PostEffect::kCinematicCameraPreviewTextureIndex);
				const D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle =
					engine::SrvManager::GetInstance()->GetGpuDescriptorHandle(textureHandle);
				ImGui::Image((ImTextureID)gpuHandle.ptr, ImVec2(previewWidth, previewHeight));
				// 折り畳みやクリップで画像が見えないときは、CameraEditor へ表示不要と通知する。
				cameraEditor->SetSceneCameraPreviewWindowVisible(ImGui::IsItemVisible());
			}
		}

		ImGui::EndChild();
	}

	ImGui::End();
}

void GameEditorController::HandleGameViewDropTargets(SceneManager* sceneManager, const GameViewArea& area) {
	// ドロップ座標はImGuiの画面座標なので、Sprite配置時だけゲームの設計解像度へ戻す。
	// Game View がドロップ先として有効でなければ、ペイロードを受け取らない。
	if (!ImGui::BeginDragDropTarget()) {
		return;
	}

	// スプライト画像のドロップは、画像矩形から求めたゲーム画面の座標に置く。
	if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SPRITE_FILE")) {
		const char* droppedFilename = static_cast<const char*>(payload->Data);

		// シーン管理器とドロップされた名前が揃った場合に、配置先を取得する。
		if (sceneManager && droppedFilename) {
			// 現在のシーンが存在する場合だけ、画像をスプライトとして追加する。
			if (BaseScene* currentScene = sceneManager->GetCurrentScene()) {
				engine::SpriteCommon* spriteCommon = currentScene->GetSpriteCommon();

				ImVec2 mousePos = ImGui::GetIO().MousePos;
				// 画像左上からの比率にゲーム画面の幅と高さを掛ける。縮小表示でドロップしても実際のSprite位置を同じ比率にする。
				engine::Vector2 dropPos = {
					(mousePos.x - area.screenX) * (static_cast<float>(WinApp::GetClientWidth()) / area.width),
					(mousePos.y - area.screenY) * (static_cast<float>(WinApp::GetClientHeight()) / area.height),
				};

				std::string fullPath = "Resources/sprite/" + std::string(droppedFilename);
				auto newSprite = std::make_unique<engine::Sprite>();
				uint32_t handle = engine::TextureManager::GetInstance()->Load(fullPath);

				newSprite->Initialize(spriteCommon, handle);
				newSprite->SetName(droppedFilename);
				newSprite->SetTextureName(droppedFilename);
				newSprite->SetPosition(dropPos);

				engine::Sprite* addedSprite = currentScene->AddSprite(std::move(newSprite));

				// スプライトの編集器があれば、追加結果をそのまま選択へ渡す。
				if (spriteDebugEditor_) {
					spriteDebugEditor_->SetSelectedSprite(addedSprite);
				}

				DebugConsole::GetInstance()->AddLog("Dropped Sprite: " + std::string(droppedFilename));
			}
		}
	}

	// モデルのドロップは、アセットのパスを取り出して3D配置へ渡す。
	if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MODEL_ASSET")) {
		// 3D素材はEditor側のレイキャスト配置へ渡し、Spriteとは別経路で位置を決める。
		const std::string droppedModelName = ReadEditorAssetDragPath(payload->Data, payload->DataSize);

		// 配置の窓口がありモデル名が空でない場合だけ、3Dモデルの生成を依頼する。
		if (debugEditor_ && !droppedModelName.empty()) {
			debugEditor_->InstantiateModelAtCursor(droppedModelName);
		}
	}

	// プリセットのドロップは、複数の配置物をまとめて生成する経路を選ぶ。
	if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PRESET_ASSET")) {
		// 複数Objectを持つプリセットはルート位置をEditor側で決めてから一括生成する。
		const char* droppedPresetName = static_cast<const char*>(payload->Data);

		// プリセット名と編集器が取れた場合だけ、生成と配置を依頼する。
		if (debugEditor_ && droppedPresetName) {
			debugEditor_->InstantiatePresetAtCursor(droppedPresetName);
		}
	}

	            // Prefab のドロップは、インスタンス情報付きの階層を作る専用経路へ送る。
	            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PREFAB_ASSET")) {
                const char* prefabName = static_cast<const char*>(payload->Data);

                // Prefab 内の親子構造を保ったまま複製する入口へ渡す。座標は同じフレームの Game View のドロップ位置を使う。
                if (debugEditor_ && prefabName) {
                    debugEditor_->InstantiatePrefabAtCursor(prefabName);
                }
            }

// 粒子アセットのドロップは、発生位置を調整できる編集用の物体を生成する。
if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PARTICLE_ASSET")) {
		const char* droppedParticleName = static_cast<const char*>(payload->Data);

		// 粒子名と編集器が取れた場合だけ、生成と配置を依頼する。
		if (debugEditor_ && droppedParticleName) {
			debugEditor_->InstantiateParticleAtCursor(droppedParticleName);
		}
	}

	ImGui::EndDragDropTarget();
}

// ゴーストレコーダーや選択オブジェクトの録画プレビューをゲームビュー上に描画する。

void GameEditorController::DrawGhostPreview(bool isPlaying, const GameViewArea& area) {
    // 実行中か経路ガイドが無効なら、ゴーストや録画経路を重ね描きしない。
    if (isPlaying || !EditorToolSettings::Get().IsVisible("ghostPath")) {
		return;
	}

	engine::Camera* camera = CameraManager::GetInstance()->GetActiveCamera();

	// 投影に使うカメラがなければ、経路のワールド座標を画面へ変換できない。
	if (!camera) {
		return;
	}

	// 共有ゴースト記録器自体を選択中の場合は、その記録の経路を描く。
	if (ghostRecorder_ && EditorManager::GetInstance()->GetSelectedObject() == ghostRecorder_.get()) {
		ghostRecorder_->DrawPreview(
			camera->GetViewProjectionMatrix(),
			engine::Vector2{ area.screenX, area.screenY },
			engine::Vector2{ area.width, area.height });
	}

	// シーン編集器があれば、現在の選択と配置物から個別経路も探す。
	if (debugEditor_) {
		engine::Object3D* selectedObject = debugEditor_->GetSelectedObject();
		SceneManager* editorSceneManager = debugEditor_->GetSceneManager();

		// シーン切り替えで無効になった選択参照を描画前に外し、旧録画器へアクセスするのを防ぐ。
		if (selectedObject && !IsObjectInCurrentScene(editorSceneManager, selectedObject)) {
			debugEditor_->SetSelectedObject(nullptr);

			// 無効な選択が編集器全体の選択にもなっていた場合は、そちらも解除する。
			if (EditorManager::GetInstance()->GetSelectedObject() == debugEditor_.get()) {
				EditorManager::GetInstance()->ClearSelection();
			}

			selectedObject = nullptr;
		}

		// 経路アセットを割り当て済みのObjectは選択されていなくても、それぞれの録画器のプレビューを重ねる。
		if (editorSceneManager && editorSceneManager->GetCurrentScene()) {
			for (const auto& object : editorSceneManager->GetCurrentScene()->GetObjects()) {
				// 経路名と記録器が揃っている物体だけ、保存された経路を表示する候補にする。
				if (!object || object->GetRecordPathName().empty() || !object->GetRecorder())
					continue;
				object->GetRecorder()->SetSceneManager(editorSceneManager);
				// 読み込んだプレビュー用の経路がなければ描画しない。
				if (!object->GetRecorder()->HasPreviewData())
					continue;
				object->GetRecorder()->DrawPreview(
					camera->GetViewProjectionMatrix(),
					engine::Vector2{ area.screenX, area.screenY },
					engine::Vector2{ area.width, area.height }, true);
			}
		}

		// Path未割当でも、選択中のObjectで編集中の経路は表示する。
		if (selectedObject && selectedObject->GetRecordPathName().empty() && selectedObject->GetRecorder()) {
			selectedObject->GetRecorder()->SetSceneManager(editorSceneManager);

			// 選択中の未割当の物体も、編集中の経路データがあるときだけ描画する。
			if (selectedObject->GetRecorder()->HasPreviewData()) {
				selectedObject->GetRecorder()->DrawPreview(
					camera->GetViewProjectionMatrix(),
					engine::Vector2{ area.screenX, area.screenY },
					engine::Vector2{ area.width, area.height }, true);
			}
		}
	}

	// Director を操作しているときは、録画器個別の経路に加えて編集中の演出プレビューを重ねる。
	if (ghostDirector_ && EditorManager::GetInstance()->GetSelectedObject() == ghostDirector_.get()) {
		ghostDirector_->DrawPreview(
			camera->GetViewProjectionMatrix(),
			engine::Vector2{ area.screenX, area.screenY },
			engine::Vector2{ area.width, area.height });
	}
}



#endif
