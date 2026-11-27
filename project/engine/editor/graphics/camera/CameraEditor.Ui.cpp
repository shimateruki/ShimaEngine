#include <cstdint>
#include "engine/editor/graphics/camera/CameraEditor.Internal.h"

using namespace cameraEditorImplementation;

// 視点のモード・保存ファイル・Camera Objectの編集入口と、別視点プレビューを描画する。
void CameraEditor::DrawImGui() {
#ifdef USE_IMGUI
    ImGui::Text(ICON_FA_VIDEO " --- カメラエディタ (Camera Editor) ---");

    // ファイル管理を展開している場合だけ、選択と読み書きの操作を描く。
    if (ImGui::CollapsingHeader(ICON_FA_SAVE " ファイル管理 (File Manager)", ImGuiTreeNodeFlags_DefaultOpen)) {
        static std::int32_t sCurrentItem = -1;

        // 一覧を開いたときだけ、登録済みのファイルを列挙する。
        if (ImGui::BeginCombo(ICON_FA_HISTORY " ファイル選択", "Choose from list...")) {
            for (std::int32_t i = 0; i < (std::int32_t)fileList_.size(); i++) {
                bool isSelected = (sCurrentItem == i);

                // 一覧選択では次の読み書きのファイル名だけを変える。現在の視点を変えるのはロードを押したとき。
                if (ImGui::Selectable(fileList_[i].c_str(), isSelected)) {
                    sCurrentItem = i;
                    strcpy_s(fileNameBuffer_, sizeof(fileNameBuffer_), fileList_[i].c_str());
                }

                // 一覧を出したときは、現在選ばれている行へ既定フォーカスを置く。
                if (isSelected)
                    ImGui::SetItemDefaultFocus();
            }

            ImGui::EndCombo();
        }

        ImGui::InputText(ICON_FA_FILE_SIGNATURE " ファイル名(.json)", fileNameBuffer_, sizeof(fileNameBuffer_));

        // ロードが押された場合だけ入力欄のファイルから視点設定を読み直す。
        if (ImGui::Button(ICON_FA_UPLOAD " ロード"))
            LoadSettings();
        ImGui::SameLine();
        // セーブが押された場合は現在の設定を指定ファイルへ書く。
        if (ImGui::Button(ICON_FA_DOWNLOAD " セーブ"))
            SaveSettings();
        ImGui::SameLine();
        // 更新が押された場合は選択候補だけを作り直す。
        if (ImGui::Button(ICON_FA_SYNC " 更新"))
            RefreshFileList();
    }

    ImGui::Separator();
    ImGui::Spacing();

    const char* modeNames[] = { "ゲームカメラ (Game)", "自由カメラ (Editor)" };
    std::int32_t currentModeInt = static_cast<std::int32_t>(settings_.currentMode);

    // モードが選び直された場合は、以下の詳細設定とプレビューの役割も切り替わる。
    if (ImGui::Combo(ICON_FA_COGS " メインモード", &currentModeInt, modeNames, IM_ARRAYSIZE(modeNames))) {
        settings_.currentMode = static_cast<Mode>(currentModeInt);
    }

    ImGui::Separator();
    DrawCameraPreviewPanel();
    ImGui::Separator();

    // ゲーム視点のときは、遮蔽物の処理と追従の設定を調整できる。
    if (settings_.currentMode == Mode::kGame) {
        // 遮蔽物設定を開いたときだけ、円の寸法・透過・カメラの衝突と復帰の値を表示する。
        if (ImGui::TreeNode("遮蔽物とカメラの視界確保")) {
            auto& v = settings_.visibility;
            ImGui::Checkbox("プレイヤー周辺の遮蔽物を透過", &v.enabled);
            ImGui::TextUnformatted("隠れた自機の補助色 / 不透明度");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::ColorEdit4("##隠れた自機の補助色 / 不透明度", v.silhouetteColor.data());
            ImGui::TextWrapped("補助表示を消す場合は色のAを0にします。");
            ImGui::TextUnformatted("透過円の半径");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragFloat("##透過円の半径", &v.radius, 0.05f, 0.3f, 8.0f, "%.2f m");
            ImGui::TextUnformatted("残す不透明度");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::SliderFloat("##残す不透明度", &v.minimumOpacity, 0.0f, 0.8f, "%.2f");
            ImGui::TextUnformatted("円の境界の柔らかさ");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::SliderFloat("##円の境界の柔らかさ", &v.feather, 0.05f, 0.9f, "%.2f");
            ImGui::TextUnformatted("透過までの時間");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragFloat("##透過までの時間", &v.fadeIn, 0.01f, 0.02f, 2.0f, "%.2f s");
            ImGui::TextUnformatted("不透明へ戻す時間");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragFloat("##不透明へ戻す時間", &v.fadeOut, 0.01f, 0.02f, 3.0f, "%.2f s");
            ImGui::TextUnformatted("カメラの衝突半径");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragFloat("##カメラの衝突半径", &v.collisionRadius, 0.01f, 0.1f, 1.0f, "%.2f m");
            ImGui::TextUnformatted("距離復帰の滑らかさ");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragFloat("##距離復帰の滑らかさ", &v.recoveryTime, 0.01f, 0.05f, 3.0f, "%.2f s");
            ImGui::TextUnformatted("手動操作後の自動復帰待ち");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragFloat("##手動操作後の自動復帰待ち", &v.manualReturnDelay, 0.1f, 0.0f, 10.0f, "%.1f s");
            ImGui::TextWrapped("足元の上面・プレイヤー・敵・アイテムは保持します。通常地形はディザ透過、バリアは半透明になります。設定は上のファイル管理から保存できます。");
            v.Validate();
            ImGui::TreePop();
        }
        ImGui::Checkbox("ジャンプ中の高さ追従を抑える", &settings_.stabilizeJumpHeight);
        // ジャンプの高さを安定させる場合だけ、無視する上方向の高低差を調整する。
        if (settings_.stabilizeJumpHeight) {
            ImGui::DragFloat("上昇時の許容高低差", &settings_.jumpHeightAllowance, 0.1f, 1.0f, 30.0f);
            ImGui::TextDisabled("通常のジャンプは高さを保持し、大きな落下・上昇時は追従します。");
        }

        // ゲーム視点の方式を選び、方式によって表示する追従設定を切り替える。
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), ICON_FA_GAMEPAD " カメラ挙動設定 (Game)");

        const char* followModeNames[] = {
            "固定 (Fixed)",
            "3人称 (Aimable)",
            "1人称 (FPS)",
            "ロックオン",
            "周回 (Orbit)",
            "定点注視 (FixedPoint)"
        };

        std::int32_t style = static_cast<std::int32_t>(settings_.gameplayStyle);
        const char* styleNames[] = {"自由操作（従来）", "自動構図＋補助操作"};
        // 操作方式の選択が変わったときだけ、常時追従するモードの値を更新する。
        if (ImGui::Combo("操作方式", &style, styleNames, IM_ARRAYSIZE(styleNames)))
            settings_.gameplayStyle = static_cast<engine::Camera::GameplayStyle>(style);
        // 自動構図を選んだ場合は、手動でずらせる範囲と自動復帰の値を調整する。
        if (settings_.gameplayStyle == engine::Camera::GameplayStyle::kGuided) {
            ImGui::TextWrapped("基準角度からの見回しだけを許可します。入力を離すと基準構図へ戻ります。");
            ImGui::SliderFloat("左右の補助角度", &settings_.guidedYawLimit, 0, 30, "%.1f deg");
            ImGui::SliderFloat("上下の補助角度", &settings_.guidedPitchLimit, 0, 15, "%.1f deg");
            ImGui::SliderFloat("補助角度の復帰速度", &settings_.guidedReturnResponse, 0.5f, 20);
            ImGui::SliderFloat("通常画角（rad）", &settings_.guidedFov, 0.35f, 1.2f);
            ImGui::TextUnformatted("進行先と追従の余白");
            auto framingSlider = [](const char* label, const char* id, float* value, float lo, float hi, const char* format) {
                ImGui::TextUnformatted(label); ImGui::SetNextItemWidth(-1);
                ImGui::SliderFloat(id, value, lo, hi, format);
            };
            framingSlider("先読み距離（最大）", "##GuidedLead", &settings_.guidedLookAhead, 0, 15, "%.1f m");
            framingSlider("左右の余白（画面半幅の比率）", "##GuidedDeadWidth", &settings_.guidedDeadZoneWidth, 0, .3f, "%.2f");
            framingSlider("奥行きの余白（画面半高の比率）", "##GuidedDeadHeight", &settings_.guidedDeadZoneHeight, 0, .3f, "%.2f");
            framingSlider("先読み・道追従の応答", "##GuidedResponse", &settings_.guidedFramingResponse, .5f, 15, "%.1f");
            ImGui::TextWrapped("通常視点は停止時に半分、移動時に最大距離を先読みします。道に沿うCameraでは、指定距離だけ先の構図を使います。");
            std::int32_t obstacles = static_cast<std::int32_t>(settings_.obstacleResponse);
            const char* responses[] = {"障害物の手前まで距離を詰める", "構図を維持（遮蔽物を透過）"};
            // 障害物対応を選び直した場合は、視点を手前へ寄せるか構図を保つかの設定を変更する。
            if (ImGui::Combo("障害物への対応", &obstacles, responses, IM_ARRAYSIZE(responses)))
                settings_.obstacleResponse = static_cast<engine::Camera::ObstacleResponse>(obstacles);
        }
        std::int32_t currentFollow = static_cast<std::int32_t>(settings_.gameFollowMode);

        // 自動構図は自由操作の追従形式を使わないため、切替欄は自由操作でだけ表示する。
        if (settings_.gameplayStyle == engine::Camera::GameplayStyle::kFree && ImGui::Combo(ICON_FA_EYE " View Type", &currentFollow, followModeNames, IM_ARRAYSIZE(followModeNames))) {
            settings_.gameFollowMode = static_cast<engine::Camera::FollowMode>(currentFollow);

            // 自由操作で周回へ切り替えた直後は、保存されている開始角度を適用する。
            if (settings_.gameFollowMode == engine::Camera::FollowMode::kOrbit) {
                ApplyOrbitStartAngle(CameraManager::GetInstance()->GetMainCamera(), true);
            }
        }

        // ボス戦用の項目を展開した場合は、通常追従とは別の距離・旋回・構図を編集する。
        if (ImGui::TreeNode("ボス戦の自動構図")) {
            auto& battle = settings_.bossFraming;
            ImGui::TextWrapped("ステージ3のボス戦で使用。攻撃ロックオンとは独立し、ボスへ緩やかに旋回します。");
            ImGui::Checkbox("有効##BossFraming", &battle.enabled);
            ImGui::SliderFloat("基本距離##BossFraming", &battle.minDistance, 8, 60);
            ImGui::SliderFloat("最大距離##BossFraming", &battle.maxDistance, battle.minDistance, 80);
            ImGui::SliderFloat("注視高さ##BossFraming", &battle.height, 0, 10);
            ImGui::SliderFloat("見下ろし角度##BossFraming", &battle.pitch, 20, 50, "%.1f deg");
            ImGui::SliderFloat("画角##BossFraming", &battle.fov, .45f, 1.1f, "%.2f rad");
            ImGui::SliderFloat("ボス側へ寄せる比率##BossFraming", &battle.targetWeight, 0, .5f);
            ImGui::SliderFloat("追従・壁回避の旋回速度##BossFraming", &battle.turnSpeed, 10, 120, "%.1f deg/s");
            ImGui::TextWrapped("壁際では回避方向を保持し、移動途中の壁も確認しながら通常視点へ戻ります。");
            ImGui::SliderFloat("追従の応答##BossFraming", &battle.response, .5f, 8);
            battle.Validate();
            ImGui::TreePop();
        }
        // ロックオンを展開した場合は、狙う視点の横・上・後ろのオフセットを編集する。
        if (ImGui::TreeNode("ロックオン視点")) {
            ImGui::DragFloat("左右##LockOn", &settings_.lockOnOffset.x, 0.05f, -10.0f, 10.0f);
            ImGui::DragFloat("高さ##LockOn", &settings_.lockOnOffset.y, 0.05f, 0.0f, 15.0f);
            ImGui::DragFloat("後方距離##LockOn", &settings_.lockOnOffset.z, 0.1f, -40.0f, -3.0f);
            ImGui::TreePop();
        }

        // 自動構図・3人称・固定視点では、対象からの基本距離と高さを使う。
        if (settings_.gameplayStyle == engine::Camera::GameplayStyle::kGuided ||
            settings_.gameFollowMode == engine::Camera::FollowMode::kAimable ||
            settings_.gameFollowMode == engine::Camera::FollowMode::kFixed) {
            ImGui::DragFloat(ICON_FA_ARROWS_ALT " 距離 (Distance)", &settings_.distance, 0.1f, 1.0f, 100.0f);
            ImGui::DragFloat(ICON_FA_ARROWS_ALT_V " 高さ (Height)", &settings_.height, 0.1f, 0.0f, 50.0f);
            // 自動構図の場合は見下ろしと基準の水平角を個別に調整する。
            if (settings_.gameplayStyle == engine::Camera::GameplayStyle::kGuided) {
                ImGui::SliderFloat("見下ろし角度", &settings_.angle.x, 20.0f, 65.0f, "%.1f deg");
                ImGui::SliderFloat("基準方向", &settings_.angle.y, -180.0f, 180.0f, "%.1f deg");
            // その他の対応視点ではXYZの角度をまとめて入力する。
            } else {
            ImGui::DragFloat3(ICON_FA_SYNC " 角度 (X/Y/Z)", &settings_.angle.x, 0.1f, -180.0f, 180.0f);
            }
        }

        // 周回形式では、周回の半径・高さ・速度を編集する。
        if (settings_.gameFollowMode == engine::Camera::FollowMode::kOrbit) {
            ImGui::Separator();
            ImGui::Text(ICON_FA_REDO " 周回設定");
            ImGui::DragFloat(" 半径 (Radius)", &settings_.orbitRadius, 0.1f, 1.0f, 100.0f);
            ImGui::DragFloat(" 高さ (Height)", &settings_.orbitHeight, 0.1f, -10.0f, 50.0f);
            ImGui::DragFloat(ICON_FA_TACHOMETER_ALT " 回転速度 (Speed)", &settings_.orbitSpeed, 0.0001f, -0.1f, 0.1f, "%.4f");
        }

        // 周回形式では、中心と開始位置のガイドを分けて調整できる。
        if (settings_.gameFollowMode == engine::Camera::FollowMode::kOrbit) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "周回ガイド");
            ImGui::Checkbox("ガイドを表示", &settings_.orbitGuideVisible);
            ImGui::Checkbox("自由カメラ編集ギズモを表示", &settings_.orbitCenterGizmoVisible);
            const char* editGizmoTargets[] = { "周回中心", "開始カメラ位置" };
            ImGui::Combo("自由カメラ時のギズモ対象", &orbitEditGizmoTarget_, editGizmoTargets, IM_ARRAYSIZE(editGizmoTargets));
            ImGui::TextDisabled("自由カメラで外から見ながら、周回カメラの中心や開始位置を調整します。");
            ImGui::DragFloat("注視点の高さ", &settings_.orbitCenterHeight, 0.1f, -10.0f, 50.0f);

            // 中心オフセットを変えたときは、新しい中心で開始位置を計算し直す。
            if (ImGui::DragFloat3("周回中心オフセット", &settings_.orbitCenterOffset.x, 0.1f, -80.0f, 80.0f)) {
                ApplyOrbitStartAngle(CameraManager::GetInstance()->GetMainCamera(), true);
            }

            // リセットを選んだ場合は中心の補正をゼロにして適用し、設定ファイルも更新する。
            if (ImGui::Button("周回中心オフセットをリセット")) {
                settings_.orbitCenterOffset = { 0.0f, 0.0f, 0.0f };
                ApplyOrbitStartAngle(CameraManager::GetInstance()->GetMainCamera(), true);
                SaveSettings();
            }

            // 開始角度を編集したときは、現在のカメラへ反映して見え方を確認できるようにする。
            if (ImGui::DragFloat("開始角度", &settings_.orbitStartAngleDeg, 0.5f, -360.0f, 360.0f, "%.1f deg")) {
                ApplyOrbitStartAngle(CameraManager::GetInstance()->GetMainCamera(), true);
            }

            ImGui::DragFloat("ガイド点の大きさ", &settings_.orbitGuideMarkerSize, 0.01f, 0.1f, 2.0f);

            engine::Vector3 center = GetOrbitCenter();

            // 現在位置を選んだ場合は、メインカメラを基準に周回の開始設定を取得する。
            if (ImGui::Button(ICON_FA_REDO " 現在位置を周回開始位置にセット")) {
                CaptureOrbitStartFromCurrentCamera(CameraManager::GetInstance()->GetMainCamera());
            }

            ImGui::TextDisabled("周回中心: %.2f, %.2f, %.2f", center.x, center.y, center.z);

            // 現在の方向だけを保存する操作では、半径と高さを保持する。
            if (ImGui::Button("現在の角度を開始角度にする")) {
                engine::Camera* cam = CameraManager::GetInstance()->GetMainCamera();

                // メインカメラが存在する場合だけ、中心から現在の視点までの水平角を求める。
                if (cam) {
                    engine::Vector3 diff = cam->GetEye() - center;
                    // 中心から視点への水平角だけを保存し、開始の半径と高さを変更しない。
                    settings_.orbitStartAngleDeg = RadToDeg(std::atan2(diff.z, diff.x));
                    ApplyOrbitStartAngle(cam, true);
                }
            }

            ImGui::SameLine();

            // 開始位置を確認するボタンが押された場合は、編集済みの角度から位置を適用する。
            if (ImGui::Button("開始位置をプレビュー")) {
                ApplyOrbitStartAngle(CameraManager::GetInstance()->GetMainCamera(), true);
            }
        }

        // 定点注視では、追従距離ではなく個別のカメラ座標と角度を編集する。
        if (settings_.gameFollowMode == engine::Camera::FollowMode::kFixedPoint) {
            ImGui::Separator();
            ImGui::Text(ICON_FA_MAP_MARKER_ALT " 定点カメラ設定");
            ImGui::DragFloat3(" カメラ座標", &settings_.fixedPointPos.x, 0.1f);
            ImGui::DragFloat3(" カメラ角度", &settings_.fixedPointAngle.x, 0.05f);

            // 現在位置のボタンが押された場合は、メインカメラから座標を取得する。
            if (ImGui::Button(ICON_FA_CROSSHAIRS " 現在のカメラ位置をセット")) {
                engine::Camera* cam = CameraManager::GetInstance()->GetMainCamera();
                // メインカメラがまだ無ければ、現在の定点座標を変更しない。
                if (cam)
                    settings_.fixedPointPos = cam->GetEye();
            }
        }
    }
    else {
        // 自由カメラでは入力による移動と感度、編集用の視点の保存・復元を扱う。
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), ICON_FA_MOUSE_POINTER " 自由操作設定 (Editor)");
        ImGui::TextDisabled("右クリック押下 + WASD で移動\nQ/E で上下移動 | Shift でブースト");

        ImGui::Spacing();
        ImGui::SliderFloat(ICON_FA_TACHOMETER_ALT " 移動速度", &settings_.moveSpeed, 0.1f, 5.0f);
        ImGui::SliderFloat(ICON_FA_FAST_FORWARD " 加速速度", &settings_.boostSpeed, 1.0f, 10.0f);
        ImGui::SliderFloat(ICON_FA_MOUSE " マウス感度", &settings_.mouseSensitivity, 0.001f, 0.05f);

        engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();

        // メインカメラが用意できている場合だけ、現在姿勢を読み取る操作を表示する。
        if (camera) {
            engine::Vector3 pos = camera->GetEye();
            engine::Vector3 rot = camera->GetRotation();
            ImGui::TextDisabled(ICON_FA_LOCATION_ARROW " 現在座標: %.2f, %.2f, %.2f", pos.x, pos.y, pos.z);
            ImGui::TextDisabled(ICON_FA_SYNC " 現在角度: %.2f, %.2f, %.2f", rot.x, rot.y, rot.z);

            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), ICON_FA_SAVE " 自由カメラ保存");
            ImGui::TextDisabled("保存済み座標: %.2f, %.2f, %.2f", settings_.editorCameraPos.x, settings_.editorCameraPos.y, settings_.editorCameraPos.z);
            ImGui::TextDisabled("保存済み角度: %.2f, %.2f, %.2f", settings_.editorCameraAngle.x, settings_.editorCameraAngle.y, settings_.editorCameraAngle.z);

            // 保存を選んだ場合は、現在の座標と姿勢を自由カメラ専用の状態へ書く。
            if (ImGui::Button(ICON_FA_SAVE " 現在の自由カメラ位置を保存")) {
                settings_.editorCameraPos = pos;
                settings_.editorCameraAngle = rot;
                SaveEditorState();
                DebugConsole::GetInstance()->AddLog("Editor free camera position saved.");
            }

            ImGui::SameLine();

            // 復元を選んだ場合は、自由カメラの保存値を読み直してカメラを更新する。
            if (ImGui::Button(ICON_FA_UNDO " 保存済み自由カメラへ戻す")) {
                LoadEditorState();
                SetEditorCameraTransform(settings_.editorCameraPos, settings_.editorCameraAngle);
                camera->Update();
                DebugConsole::GetInstance()->AddLog("Editor free camera position restored.");
            }

            ImGui::Spacing();

            // 周回の開始として取り込む場合は、現在の自由カメラの位置から周回設定を求める。
            if (ImGui::Button(ICON_FA_REDO " 現在位置を「周回カメラ」の開始位置にセット")) {
                CaptureOrbitStartFromCurrentCamera(camera);
            }

            // 定点視点に取り込む場合は、表示中の座標と角度を設定ファイルへ保存する。
            if (ImGui::Button(ICON_FA_MAP_MARKER_ALT " 現在位置を「定点カメラ」の座標・角度にセット")) {
                settings_.fixedPointPos = pos;
                settings_.fixedPointAngle = rot;
                SaveSettings();
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();

    // Camera Objectが演出カメラ設定の唯一の編集・保存単位です。
    // 一覧を開いた場合だけ、現在シーンのカメラを列挙してテスト操作を出す。
    if (ImGui::CollapsingHeader(ICON_FA_VIDEO " Camera Objects", ImGuiTreeNodeFlags_DefaultOpen)) {
        SceneManager* sceneManager = SceneManager::GetInstance();
        BaseScene* scene = sceneManager ? sceneManager->GetCurrentScene() : nullptr;
        std::int32_t cameraCount = 0;

        // 編集中のシーンがあり、リストを開けた場合だけ、シーンに属する候補を探す。
        if (scene && ImGui::BeginListBox("##SceneCameraObjectList", ImVec2(-FLT_MIN, 120.0f))) {
            for (const auto& object : scene->GetObjects()) {
                // 空の項目とカメラ以外のオブジェクトは、カメラの候補として扱わない。
                if (!object || !object->IsCameraObject()) {
                    continue;
                }

                ++cameraCount;
                const bool selected = object.get() == selectedCameraObject_;
                const SceneCameraSettings& cameraSettings = object->GetSceneCameraSettings();
                const char* roleLabel = cameraSettings.role == SceneCameraRole::kMain ? "Main" : "Cinematic";
                const std::string label = object->GetName() + "  [" + roleLabel + "]";

                // 行が選び直された場合、Inspectorやプレビューが参照するカメラを変更する。
                if (ImGui::Selectable(label.c_str(), selected)) {
                    SetSelectedCameraObject(object.get());
                }

                // 選択中のカメラを表示したときは、その行を既定フォーカスにする。
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }

            ImGui::EndListBox();
        }

        // 一覧にカメラを表示できなかった場合は、Hierarchyからの作成を案内する。
        if (cameraCount == 0) {
            ImGui::TextDisabled("Hierarchyの作成メニューからCamera Objectを追加してください。");
        }
        // 実際にカメラが選択されている場合だけ、切り替え時間と再生操作を表示する。
        else if (engine::Object3D* selectedCameraObject = GetSelectedCameraObject()) {
            const SceneCameraSettings& cameraSettings = selectedCameraObject->GetSceneCameraSettings();
            ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.45f, 1.0f), "選択中: %s", selectedCameraObject->GetName().c_str());
            ImGui::TextDisabled("位置・注視・追従・EasingはCamera ObjectのInspectorで編集します。");
            ImGui::TextDisabled("Blend In %.2fs / Out %.2fs", cameraSettings.blendInDuration, cameraSettings.blendOutDuration);

            // テスト再生を選んだら、シーンの保存値から演出カメラへの切り替えを試す。
            if (ImGui::Button(ICON_FA_PLAY " Camera Objectをテスト再生")) {
                PlaySceneObjectCamera(CameraManager::GetInstance()->GetMainCamera(), selectedCameraObject);
            }

            ImGui::SameLine();

            // 停止を選んだら、テスト再生のカメラを終了させる。
            if (ImGui::Button(ICON_FA_STOP " 停止")) {
                StopSceneObjectCamera(CameraManager::GetInstance()->GetMainCamera());
            }
        }

        // 旧設定をロードしている場合は、互換用として残っている件数を知らせる。
        if (!overrideParamsMap_.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("旧形式の演出カメラ設定を%zu件読み込みました。新規編集には使用しません。", overrideParamsMap_.size());
        }
    }

    // 旧形式は既存JSONの読み込み互換だけを残し、編集UIには表示しません。
    if (kShowLegacyCameraEditor && ImGui::CollapsingHeader(ICON_FA_VIDEO " Legacy Cinematic Camera")) {

        ImGui::InputText("新規カメラ名", newOverrideNameBuffer_, sizeof(newOverrideNameBuffer_));
        ImGui::SameLine();

        // 追加が押されたら、入力した名前を旧形式に登録できるか確認する。
        if (ImGui::Button(ICON_FA_PLUS " 追加")) {
            std::string nName = newOverrideNameBuffer_;

            // 空の名前と既存名を除き、新規データを作ってその場で保存する。
            if (!nName.empty() && overrideParamsMap_.find(nName) == overrideParamsMap_.end()) {
                overrideParamsMap_[nName] = engine::Camera::CameraOverrideParams();
                selectedOverrideName_ = nName;
                newOverrideNameBuffer_[0] = '\0';
                SaveSettings();
            }
        }

        // 旧形式の一覧は選択と削除対象を受け付け、行へポインターを置くと視点の情報を表示する。
        ImGui::Text(ICON_FA_LIST " 保存済みカメラ一覧:");
        ImGui::TextDisabled("Scene上の番号タグと同じ順番です。緑が選択中、青が未選択です。");
        std::string deleteOverrideName;

        // リストが描画できる場合だけ、保存されているデータを順に表示する。
        if (ImGui::BeginListBox("##CameraList", ImVec2(-FLT_MIN, 100))) {
            std::int32_t cameraIndex = 1;

            for (auto& [name, param] : overrideParamsMap_) {
                bool isSelected = (selectedOverrideName_ == name);
                ImGui::PushID(name.c_str());
                ImGui::PushStyleColor(ImGuiCol_Text, isSelected ? ImVec4(0.2f, 1.0f, 0.35f, 1.0f) : ImVec4(0.45f, 0.70f, 1.0f, 1.0f));
                const std::string listLabel = "#" + std::to_string(cameraIndex) + "  " + name + (isSelected ? "  [選択中]" : "");
                const float deleteButtonWidth = 28.0f;
                const float selectableWidth = (std::max)(80.0f, ImGui::GetContentRegionAvail().x - deleteButtonWidth - ImGui::GetStyle().ItemSpacing.x);

                // 行を押したときは詳細欄の編集対象を切り替える。
                if (ImGui::Selectable(listLabel.c_str(), isSelected, 0, ImVec2(selectableWidth, 0.0f))) {
                    selectedOverrideName_ = name;
                }

                const bool rowHovered = ImGui::IsItemHovered();
                ImGui::PopStyleColor();

                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));

                // Xボタンを押した行の名前だけを控え、走査後に削除する。
                if (ImGui::SmallButton("X")) {
                    deleteOverrideName = name;
                }

                ImGui::PopStyleColor();

                // その行にマウスが重なっている場合は、解決後の視点と注視点を表示する。
                if (rowHovered) {
                    const engine::Vector3 eye = ResolveOverrideEye(param);
                    const engine::Vector3 target = ResolveOverrideTarget(param);
                    ImGui::BeginTooltip();
                    ImGui::Text("%s", name.c_str());
                    ImGui::Text("Eye: %.2f, %.2f, %.2f", eye.x, eye.y, eye.z);
                    ImGui::Text("Target: %.2f, %.2f, %.2f", target.x, target.y, target.z);
                    ImGui::Text("移行時間: %.2f 秒", param.duration);
                    ImGui::EndTooltip();
                }

                ImGui::PopID();
                ++cameraIndex;
            }

            ImGui::EndListBox();
        }

        // 一覧の走査中には消さず、対象名を控えて描画終了後に削除と保存を行う。
        if (!deleteOverrideName.empty()) {
            overrideParamsMap_.erase(deleteOverrideName);

            // 削除した名前が現在の選択だった場合は、無効な詳細欄を開かないよう選択を外す。
            if (selectedOverrideName_ == deleteOverrideName) {
                selectedOverrideName_.clear();
            }

            SaveSettings();
        }

        // 旧形式の編集欄では、実際にフィールドが変わった場合に保存のフラグを立てる。
        if (!selectedOverrideName_.empty() && overrideParamsMap_.find(selectedOverrideName_) != overrideParamsMap_.end()) {
            auto& p = overrideParamsMap_[selectedOverrideName_];
            bool changed = false;

            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "編集中: [%s]", selectedOverrideName_.c_str());
            ImGui::Indent();
            ImGui::Spacing();

            // 現在のカメラをコピーする操作では、視点と注視点を固定値として取得する。
            if (ImGui::Button(ICON_FA_CAMERA " 現在のカメラ視点をここにコピー (Copy Current View)")) {
                engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();

                // メインカメラを取得できる場合だけ、固定位置と追従設定を上書きする。
                if (camera) {
                    p.fixedEyePos = camera->GetEye();
                    p.fixedTargetPos = camera->GetTargetPoint();
                    p.eyeSource = engine::Camera::OverrideEyeSource::kFixed;
                    p.targetSource = engine::Camera::OverrideTargetSource::kFixed;

                    // その場所・その角度に完全に固定するため、追従フラグはすべてOFFにする
                    p.trackEyeX = false;
                    p.trackEyeY = false;
                    p.trackEyeZ = false;
                    p.trackTargetX = false;
                    p.trackTargetY = false;
                    p.trackTargetZ = false;

                    changed = true;
                }
            }

            ImGui::Spacing();
            ImGui::Separator();
            // 開始の補間時間が変わった場合は保存対象にする。
            if (ImGui::DragFloat("開始ブレンド時間 (秒)", &p.duration, 0.05f, 0.0f, 10.0f))
                changed = true;
            // 通常視点へ戻る補間時間が変わった場合は保存対象にする。
            if (ImGui::DragFloat("終了ブレンド時間 (秒)", &p.exitDuration, 0.05f, 0.0f, 10.0f))
                changed = true;
            const char* easingNames[] = { "Linear", "Ease In", "Ease Out", "Ease In Out", "Smoother Step" };
            std::int32_t easing = static_cast<std::int32_t>(p.easing);

            // 補間曲線を選び直した場合は、選択番号に対応する列挙値を保存する。
            if (ImGui::Combo("切替Easing", &easing, easingNames, IM_ARRAYSIZE(easingNames))) {
                p.easing = static_cast<engine::Camera::OverrideEasing>(easing);
                changed = true;
            }

            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), ICON_FA_EYE " カメラ位置 (Eye)");
            // Eyeの座標元を固定値とScene上のCamera Objectで切り替え、追従設定は選択した方式にだけ出す。
            const char* eyeSourceNames[] = { "固定座標", "Scene Camera Object" };
            std::int32_t eyeSource = static_cast<std::int32_t>(p.eyeSource);

            // 視点の取得元を選び直した場合は、座標を固定値から読むかオブジェクトから読むかを切り替える。
            if (ImGui::Combo("Eye取得元", &eyeSource, eyeSourceNames, IM_ARRAYSIZE(eyeSourceNames))) {
                p.eyeSource = static_cast<engine::Camera::OverrideEyeSource>(eyeSource);
                changed = true;
            }

            // 固定座標では、座標とXYZの追従を別々に設定できる。
            if (p.eyeSource == engine::Camera::OverrideEyeSource::kFixed) {
                // X軸の追従設定が変わったときに保存対象にする。
                if (ImGui::Checkbox("X軸追従##Eye", &p.trackEyeX))
                    changed = true;
                ImGui::SameLine();
                // Y軸の追従設定が変わったときに保存対象にする。
                if (ImGui::Checkbox("Y軸追従##Eye", &p.trackEyeY))
                    changed = true;
                ImGui::SameLine();
                // Z軸の追従設定が変わったときに保存対象にする。
                if (ImGui::Checkbox("Z軸追従##Eye", &p.trackEyeZ))
                    changed = true;
                // 基準とする固定座標が変わったときに保存対象にする。
                if (ImGui::DragFloat3("固定座標##Eye", &p.fixedEyePos.x, 0.1f))
                    changed = true;
            }
            // オブジェクトから視点を取得する場合は、カメラの名前と相対位置を指定する。
            else {
                const char* eyeObjectLabel = p.eyeObjectName.empty() ? "(未選択)" : p.eyeObjectName.c_str();

                // 視点候補を展開したときだけ、現在シーンのカメラを調べる。
                if (ImGui::BeginCombo("Camera Object##Eye", eyeObjectLabel)) {
                    SceneManager* sceneManager = SceneManager::GetInstance();

                    // シーンが取得できる場合だけ、一覧に入れる候補を列挙する。
                    if (sceneManager && sceneManager->GetCurrentScene()) {
                        for (const auto& object : sceneManager->GetCurrentScene()->GetObjects()) {
                            // カメラとして使えるシーン内オブジェクトだけを候補にする。
                            if (!object || !object->IsCameraObject())
                                continue;
                            const bool selected = p.eyeObjectName == object->GetName();

                            // 候補の名前を選んだ場合は視点の参照を変更する。
                            if (ImGui::Selectable(object->GetName().c_str(), selected)) {
                                p.eyeObjectName = object->GetName();
                                changed = true;
                            }
                        }
                    }

                    ImGui::EndCombo();
                }

                // 選んだカメラに対する視点オフセットの変更を保存対象にする。
                if (ImGui::DragFloat3("Objectオフセット##Eye", &p.eyeObjectOffset.x, 0.05f))
                    changed = true;
                const char* followModeNames[] = { "完全追従", "遅延追従" };
                std::int32_t followMode = static_cast<std::int32_t>(p.eyeFollowMode);

                // 視点の追従方式が変わったら、その選択を記録する。
                if (ImGui::Combo("Eye追従", &followMode, followModeNames, IM_ARRAYSIZE(followModeNames))) {
                    p.eyeFollowMode = static_cast<engine::Camera::OverrideFollowMode>(followMode);
                    changed = true;
                }

                // 遅延追従のときに限って、応答の変更を保存対象にする。
                if (p.eyeFollowMode == engine::Camera::OverrideFollowMode::kSmooth &&
                    ImGui::DragFloat("Eye追従レスポンス", &p.eyeFollowResponse, 0.1f, 0.1f, 40.0f, "%.2f")) {
                    changed = true;
                }
            }

            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), ICON_FA_CROSSHAIRS " 注視点 (Target)");
            // TargetはEyeとは独立に選び、固定・Object・Cameraの前方を組み合わせられる。
            const char* targetSourceNames[] = { "固定座標", "Scene Objectを注視", "Camera Objectの前方" };
            std::int32_t targetSource = static_cast<std::int32_t>(p.targetSource);

            // 注視点の取得元が選び直されたら、以下の対象と設定欄を切り替える。
            if (ImGui::Combo("Target取得元", &targetSource, targetSourceNames, IM_ARRAYSIZE(targetSourceNames))) {
                p.targetSource = static_cast<engine::Camera::OverrideTargetSource>(targetSource);
                changed = true;
            }

            // 固定注視点では、軸ごとの追従と固定座標を調整する。
            if (p.targetSource == engine::Camera::OverrideTargetSource::kFixed) {
                // X軸の追従設定が変わったときに保存対象にする。
                if (ImGui::Checkbox("X軸追従##Tgt", &p.trackTargetX))
                    changed = true;
                ImGui::SameLine();
                // Y軸の追従設定が変わったときに保存対象にする。
                if (ImGui::Checkbox("Y軸追従##Tgt", &p.trackTargetY))
                    changed = true;
                ImGui::SameLine();
                // Z軸の追従設定が変わったときに保存対象にする。
                if (ImGui::Checkbox("Z軸追従##Tgt", &p.trackTargetZ))
                    changed = true;
                // 基準の注視点が変わったときに保存対象にする。
                if (ImGui::DragFloat3("固定座標##Tgt", &p.fixedTargetPos.x, 0.1f))
                    changed = true;
            }
            // シーン内の対象へ注視する場合は、対象名とそこからの位置を指定する。
            else if (p.targetSource == engine::Camera::OverrideTargetSource::kSceneObject) {
                const char* targetObjectLabel = p.targetObjectName.empty() ? "(未選択)" : p.targetObjectName.c_str();

                // 対象一覧を開いた場合だけ、シーンにある候補を列挙する。
                if (ImGui::BeginCombo("追従Object##Target", targetObjectLabel)) {
                    SceneManager* sceneManager = SceneManager::GetInstance();

                    // 現在シーンを参照できる場合に限って物体を探す。
                    if (sceneManager && sceneManager->GetCurrentScene()) {
                        for (const auto& object : sceneManager->GetCurrentScene()->GetObjects()) {
                            // エディター内部の物体と名前のない物体は注視点の選択肢から外す。
                            if (!object || object->IsEditorInternal() || object->GetName().empty())
                                continue;
                            const bool selected = p.targetObjectName == object->GetName();

                            // 候補を選んだら、保存する注視対象の名前を変える。
                            if (ImGui::Selectable(object->GetName().c_str(), selected)) {
                                p.targetObjectName = object->GetName();
                                changed = true;
                            }
                        }
                    }

                    ImGui::EndCombo();
                }

                // 対象に対する相対座標が変わった場合は保存対象にする。
                if (ImGui::DragFloat3("注視オフセット##Target", &p.targetObjectOffset.x, 0.05f))
                    changed = true;
            }
            // カメラの前方を注視する形式では、前方距離と追加オフセットを指定する。
            else {
                // 前を向く距離が変わった場合は保存対象にする。
                if (ImGui::DragFloat("前方距離##Target", &p.eyeForwardDistance, 0.1f, 0.1f, 100.0f))
                    changed = true;
                // 前方の注視点へ加えるオフセットが変わった場合は保存対象にする。
                if (ImGui::DragFloat3("前方注視オフセット##Target", &p.targetObjectOffset.x, 0.05f))
                    changed = true;
            }

            // 固定座標以外では、注視点をどの速さで追従させるかも指定できる。
            if (p.targetSource != engine::Camera::OverrideTargetSource::kFixed) {
                const char* followModeNames[] = { "完全追従", "遅延追従" };
                std::int32_t followMode = static_cast<std::int32_t>(p.targetFollowMode);

                // 追従方式が選び直された場合は注視点側の列挙値だけを変える。
                if (ImGui::Combo("Target追従", &followMode, followModeNames, IM_ARRAYSIZE(followModeNames))) {
                    p.targetFollowMode = static_cast<engine::Camera::OverrideFollowMode>(followMode);
                    changed = true;
                }

                // 遅延追従の場合に限って、応答の変更を記録する。
                if (p.targetFollowMode == engine::Camera::OverrideFollowMode::kSmooth &&
                    ImGui::DragFloat("Target追従レスポンス", &p.targetFollowResponse, 0.1f, 0.1f, 40.0f, "%.2f")) {
                    changed = true;
                }
            }

            ImGui::Unindent();

            ImGui::Spacing();

            // 旧設定のテスト操作が選ばれたら、現在の名前の演出を開始する。
            if (ImGui::Button(ICON_FA_PLAY " テスト再生 (Test Play)")) {
                engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();
                // メインカメラがある場合だけテスト再生を開始できる。
                if (camera)
                    PlayOverrideCamera(camera, selectedOverrideName_);
            }

            ImGui::SameLine();

            // 停止が選ばれたら、保存された終了の補間時間を使う。
            if (ImGui::Button(ICON_FA_STOP " 停止 (Stop)")) {
                engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();
                // メインカメラがあるときだけ、通常の視点への終了処理を依頼する。
                if (camera)
                    camera->EndOverride(p.exitDuration);
            }

            // このフレームに旧設定を編集した場合は保存を実行する。
            if (changed) {
                SaveSettings();
            }

            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));

            // 削除のボタンが押された場合は、編集中の項目を削除して選択も外す。
            if (ImGui::Button(ICON_FA_TRASH " 選択中のカメラを削除")) {
                overrideParamsMap_.erase(selectedOverrideName_);
                selectedOverrideName_ = "";
                SaveSettings();
            }

            ImGui::PopStyleColor();
        }
    }
#endif
}

void CameraEditor::DrawCameraPreviewPanel() {
#ifdef USE_IMGUI
    // カメラの描画負荷を調整できるよう、更新頻度と解像度をプレビュー設定にまとめる。
    if (!ImGui::CollapsingHeader(ICON_FA_CAMERA " カメラ可視化 / プレビュー", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    ImGui::Checkbox("編集時にカメラガイドを表示", &settings_.cameraGuideVisible);
    ImGui::SameLine();
    ImGui::Checkbox("カメラ本体モデルを表示", &settings_.cameraBodyVisible);
    ImGui::SameLine();

    // 画像の表示設定が変わったら、再開時に以前の画像を使わないようキャッシュを無効化する。
    if (ImGui::Checkbox("プレビュー枠を表示", &settings_.cameraPreviewVisible)) {
        InvalidateCameraPreviews();
    }

    ImGui::DragFloat("ガイド本体の大きさ", &settings_.cameraGuideSize, 0.01f, 0.1f, 3.0f, "%.2f");
    ImGui::DragFloat("視錐台の長さ", &settings_.cameraFrustumLength, 0.1f, 1.0f, 50.0f, "%.1f");
    ImGui::DragFloat("プレビュー高さ", &settings_.cameraPreviewHeight, 1.0f, 80.0f, 480.0f, "%.0f");

    constexpr std::int32_t kPreviewFpsValues[] = { 10, 15, 30, 60 };
    const char* previewFpsLabels[] = { "10 FPS (軽量)", "15 FPS (推奨)", "30 FPS", "60 FPS" };
    std::int32_t previewFpsIndex = 1;

    for (std::int32_t i = 0; i < IM_ARRAYSIZE(kPreviewFpsValues); ++i) {
        // 現在の更新頻度と一致する選択肢があれば、既定の選択番号を置き換える。
        if (settings_.cameraPreviewFps == kPreviewFpsValues[i]) {
            previewFpsIndex = i;
            break;
        }
    }

    // 別の更新頻度を選んだら、プレビューを更新し直すよう設定して保存する。
    if (ImGui::Combo("プレビュー更新頻度", &previewFpsIndex, previewFpsLabels, IM_ARRAYSIZE(previewFpsLabels))) {
        settings_.cameraPreviewFps = kPreviewFpsValues[previewFpsIndex];
        InvalidateCameraPreviews();
        SaveSettings();
    }

    // 描画先の解像度倍率は表示枠のピクセル寸法から独立させ、周囲のレイアウトを変えずに負荷を調整する。
    constexpr float kPreviewResolutionScales[] = { 0.25f, 0.5f, 1.0f };
    const char* previewResolutionLabels[] = { "25% (軽量)", "50% (推奨)", "100% (高精細)" };
    std::int32_t previewResolutionIndex = 1;
    float nearestScaleDistance = 10.0f;

    for (std::int32_t i = 0; i < IM_ARRAYSIZE(kPreviewResolutionScales); ++i) {
        const float distance = std::abs(settings_.cameraPreviewResolutionScale - kPreviewResolutionScales[i]);

        // いままでで最も近い倍率なら、一覧の表示候補を更新する。
        if (distance < nearestScaleDistance) {
            nearestScaleDistance = distance;
            previewResolutionIndex = i;
        }
    }

    // 解像度の選択が変わった場合は、描画先を作り直す処理に倍率を渡す。
    if (ImGui::Combo(
        "プレビュー解像度",
        &previewResolutionIndex,
        previewResolutionLabels,
        IM_ARRAYSIZE(previewResolutionLabels))) {
        settings_.cameraPreviewResolutionScale = kPreviewResolutionScales[previewResolutionIndex];

        // RenderTargetサイズの変更後は古いプレビューを無効化する。
        PostEffect::GetInstance()->SetCameraPreviewResolutionScale(settings_.cameraPreviewResolutionScale);
        InvalidateCameraPreviews();
        SaveSettings();
    }

    const PostEffect* postEffect = PostEffect::GetInstance();
    ImGui::TextDisabled(
        "実描画: %d x %d / 非表示・別タブ・折り畳み中は更新停止",
        postEffect->GetCameraPreviewWidth(),
        postEffect->GetCameraPreviewHeight());

    engine::Object3D* selectedCameraObject = GetSelectedCameraObject();

    // カメラオブジェクトを選択している場合は、その情報を別の視点として表示する。
    if (selectedCameraObject) {
        ImGui::TextColored(
            ImVec4(0.35f, 0.85f, 1.0f, 1.0f),
            "Camera Object: %s",
            selectedCameraObject->GetName().c_str());
        engine::Vector3 selectedEye{};
        engine::Vector3 selectedTarget{};

        // 選択物から視点と注視点を計算できた場合だけ、数値を表示する。
        if (ResolveSceneCameraPose(selectedCameraObject, selectedEye, selectedTarget)) {
            ImGui::TextDisabled("Object Eye: %.2f, %.2f, %.2f", selectedEye.x, selectedEye.y, selectedEye.z);
            ImGui::TextDisabled("Object Target: %.2f, %.2f, %.2f", selectedTarget.x, selectedTarget.y, selectedTarget.z);
        }
    }
    // 選択物がない場合は、Hierarchyからの選び方を案内する。
    else {
        ImGui::TextDisabled("HierarchyでCamera Objectを選択すると、専用プレビューを表示します。");
    }

    const engine::Vector3 eye = GetPreviewCameraEye();
    const engine::Vector3 forward = GetPreviewCameraForward();
    ImGui::TextColored(ImVec4(0.5f, 0.9f, 1.0f, 1.0f), "プレビュー対象: %s", GetPreviewCameraLabel());
    ImGui::TextDisabled("カメラ位置: %.2f, %.2f, %.2f", eye.x, eye.y, eye.z);
    ImGui::TextDisabled("視線方向: %.2f, %.2f, %.2f", forward.x, forward.y, forward.z);

    ImGui::Spacing();
    ImGui::TextDisabled("Scene表示の見方:");
    ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.18f, 1.0f), "  黄色: ゲーム/3人称カメラ");
    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.35f, 1.0f), "  緑色: 選択中のCamera Object");

    // 可視化の設定を保存するボタンを押した場合だけ、現在値を書き出す。
    if (ImGui::Button(ICON_FA_SAVE " 可視化設定を保存")) {
        SaveSettings();
    }

    if (!settings_.cameraPreviewVisible || !EditorToolSettings::Get().IsVisible("cameraPreview")) {
        // 折り畳み・非表示中は実描画側も更新を止められるよう、Imageを登録しない。
        return;
    }

    ImGui::Spacing();

    // ゲーム視点の調整中は、逆の役割である自由カメラを表示する旨を示す。
    if (settings_.currentMode == Mode::kGame) {
        ImGui::TextDisabled("下の画像は自由カメラ視点です。ゲームカメラ調整中に、外側から構図を確認できます。");
    // 自由カメラで編集している間は、ゲーム視点のプレビューであることを示す。
    } else {
        ImGui::TextDisabled("下の画像はゲームカメラ視点です。自由カメラ中に、3人称などの実際の見え方を確認できます。");
    }

    const float availableWidth = (std::max)(120.0f, ImGui::GetContentRegionAvail().x);
    const float aspect = 16.0f / 9.0f;
    float height = std::clamp(settings_.cameraPreviewHeight, 80.0f, 480.0f);
    float width = height * aspect;

    if (width > availableWidth) {
        // 縦横比を崩さずInspectorの幅に収める。
        width = availableWidth;
        height = width / aspect;
    }

    const float indent = (std::max)(0.0f, (availableWidth - width) * 0.5f);
    uint32_t textureHandle = PostEffect::GetInstance()->GetSrvHandle(PostEffect::kCameraPreviewTextureIndex);
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = engine::SrvManager::GetInstance()->GetGpuDescriptorHandle(textureHandle);

    // 画像が表示領域より狭い場合は、余白の半分だけずらして中央へ置く。
    if (indent > 0.0f) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
    }

    ImGui::Image((ImTextureID)gpuHandle.ptr, ImVec2(width, height));
    // 描画側はこのフラグで、タブの裏側など実際に画像が見えない間の不要な更新を省く。
    cameraPreviewPanelVisibleThisFrame_ = ImGui::IsItemVisible();
#endif
}
