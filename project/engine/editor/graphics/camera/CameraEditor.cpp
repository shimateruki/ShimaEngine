#include <cstdint>
#include "engine/editor/graphics/camera/CameraEditor.Internal.h"
#include <numbers>

using namespace cameraEditorImplementation;

CameraEditor* CameraEditor::GetInstance() {
    // エディターのカメラ設定とプレビュー状態を一箇所で共有する。
    static CameraEditor sInstance;
    return &sInstance;
}

void CameraEditor::Initialize() {
    // 設定ファイルの選択一覧を作ってから、ゲームカメラと自由カメラの設定を別々に復元します。
    RefreshFileList();

    // デフォルトファイル（camera_settings.json）を読み込む
    LoadSettings();

    // 自由カメラ専用の状態を読み込む
    LoadEditorState();

    // 自由カメラの位置を復元
    engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();

    // メインカメラを取得できた場合に、保存済みの自由カメラの姿勢を初期表示へ反映します。
    if (camera) {
        camera->SetEye(settings_.editorCameraPos);
        camera->SetRotation(settings_.editorCameraAngle);
        camera->Update();
    }
}

void CameraEditor::SetObject3DCommon(engine::Object3DCommon* common) {
    // 描画基盤が切り替わったら、旧基盤で作ったカメラ模型を作り直す。
    if (object3dCommon_ == common) {
        return;
    }

    object3dCommon_ = common;
    gameCameraModelGizmo_.reset();
}

void CameraEditor::EnsureCameraModelGizmo(std::unique_ptr<engine::Object3D>& gizmo, const std::string& name) {
    // ガイド用模型は必要になったときだけ生成し、同じインスタンスを使い回す。
    // 描画基盤が未接続なら、ガイド模型を初期化できません。
    if (!object3dCommon_) {
        return;
    }

    // 模型がすでにあれば、新しく作らず現在の実体を使います。
    if (gizmo) {
        return;
    }

    gizmo = std::make_unique<engine::Object3D>();
    gizmo->Initialize(object3dCommon_);
    gizmo->SetModel(kCameraModelGizmoName);
    gizmo->SetName(name);
    gizmo->SetClassName("Model");
    gizmo->SetMaterialType(0);
    gizmo->SetBlendMode(BlendMode::kNormal);
    gizmo->SetIsVisible(true);
}

void CameraEditor::ApplyCameraModelGizmo(engine::Object3D* gizmo, const engine::Vector3& eye, const engine::Vector3& forward, float sizeScale, const engine::Vector4& color) {
    // 模型を取得できなければ、姿勢や色を更新しません。
    if (!gizmo) {
        return;
    }

    const float baseSize = (std::max)(0.05f, settings_.cameraGuideSize);
    const engine::Vector3 safeForward = NormalizeSafe(forward, { 0.0f, 0.0f, 1.0f });
    const engine::Vector3 rotation = MakeRotationFromForward(safeForward);
    const float modelScale = baseSize * sizeScale;

    // CameraEditorの可視化も、演出用カメラObjectと同じモデルを使って位置と向きを表示します。
    gizmo->SetTranslate(eye);
    gizmo->SetRotation(rotation);
    gizmo->SetScale({ modelScale, modelScale, modelScale });
    gizmo->SetColor(color);
    gizmo->SetEmissive(1.8f);
    gizmo->SetIsVisible(true);
    gizmo->UpdateLocalMatrix();
    gizmo->UpdateWorldMatrix();
}

void CameraEditor::DrawCameraModelGizmos(ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource) {
    // ガイド表示と選択状態を確認し、ゲームカメラの目印が必要な場合だけ描画します。
#ifdef USE_IMGUI
    // エディター全体でカメラ本体の表示を無効にしているときは描きません。
    if (!EditorToolSettings::Get().IsVisible("cameraBody"))
        return;
#endif
    // カメラガイドと本体が両方表示され、描画基盤がある場合にだけ進みます。
    if (!settings_.cameraGuideVisible || !settings_.cameraBodyVisible || !object3dCommon_) {
        return;
    }

    const bool isCameraEditorSelected =
        EditorManager::GetInstance()->GetSelectedObject() == this;
    // カメラ編集ツールもCamera Objectも選択していなければ、ガイドは不要です。
    if (!isCameraEditorSelected && GetSelectedCameraObject() == nullptr) {
        // 表示用モデルがある場合は、以降の描画に残らないよう非表示にします。
        if (gameCameraModelGizmo_) {
            gameCameraModelGizmo_->SetIsVisible(false);
        }

        return;
    }

    // ゲーム再生による非表示の条件に当たらなければ、保存されたゲームカメラの姿勢に模型を置きます。
    if (!ShouldHideGameCameraGuideDuringPlay()) {
        const engine::Vector3 gameEye = GetConfiguredCameraEye();
        const engine::Vector3 gameForward = GetConfiguredCameraForward();
        EnsureCameraModelGizmo(gameCameraModelGizmo_, "CameraEditor_GameCameraGizmo");
        ApplyCameraModelGizmo(gameCameraModelGizmo_.get(), gameEye, gameForward, 0.85f, { 1.0f, 0.82f, 0.18f, 1.0f });

        // 模型を用意できた場合だけ、シーンの共有ライトで描きます。
        if (gameCameraModelGizmo_) {
            gameCameraModelGizmo_->Draw(pointLightResource, spotLightResource);
        }
    // 再生中の表示条件を満たさなくなり模型が残っていれば、非表示にします。
    } else if (gameCameraModelGizmo_) {
        gameCameraModelGizmo_->SetIsVisible(false);
    }

}

void CameraEditor::Update(engine::Object3D* player, bool isLockingOn, engine::Object3D* boss) {
    // 通常・ボス戦の設定を選び、攻撃ロックオンの対象はゲーム側に任せる。
    targetPlayer_ = player;
    engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();
    // メインカメラが無いフレームは、プレイヤーの参照だけを控えて終了します。
    if (!camera)
        return;

    const bool wasBossFraming = camera->IsBossFramingActive();
    // プレイヤーとボスの両方を参照できるゲーム中の誘導操作だけ、専用の構図を使う。
    const bool bossFraming = player && boss && settings_.bossFraming.enabled &&
        settings_.currentMode == Mode::kGame && settings_.gameplayStyle == engine::Camera::GameplayStyle::kGuided;
    // 追従対象の変更は構図の内部状態を破棄するため、対象設定より先に行う。
    if (bossFraming) camera->SetFollowTarget(player);
    settings_.bossFraming.Validate();
    camera->ConfigureBossFraming(settings_.bossFraming, bossFraming ? boss : nullptr);
    // ボス構図へ入る・抜けるフレームだけ、縦画角を 0.6 秒かけて切り替える。毎フレーム補間を再開しない。
    if (wasBossFraming != bossFraming)
        camera->BlendFovY(bossFraming ? settings_.bossFraming.fov : settings_.guidedFov, .6f);
    camera->SetJumpHeightStabilization(settings_.stabilizeJumpHeight, settings_.jumpHeightAllowance);
    camera->SetVisibilitySettings(settings_.visibility);
    camera->ConfigureGameplayStyle(settings_.currentMode == Mode::kGame ? settings_.gameplayStyle : engine::Camera::GameplayStyle::kFree,
        // 旋回するボス戦は壁の外へ出さず、既存の衝突回避で手前に寄せる。
        bossFraming ? engine::Camera::ObstacleResponse::kShortenDistance : settings_.obstacleResponse,
        settings_.guidedYawLimit, settings_.guidedPitchLimit,
        settings_.guidedReturnResponse, bossFraming ? settings_.bossFraming.fov : settings_.guidedFov);
    camera->ConfigureGuidedFraming(settings_.guidedLookAhead, settings_.guidedDeadZoneWidth,
        settings_.guidedDeadZoneHeight, settings_.guidedFramingResponse);
    // 基準角度を毎回渡し、手動補助やロックオンの現在角度で保存値を上書きしない。
    if (camera->IsGuidedGameplay()) {
        const auto& battle = settings_.bossFraming;
        camera->ConfigAimable(bossFraming ? battle.minDistance : settings_.distance,
            bossFraming ? battle.height : settings_.height,
            bossFraming ? engine::Vector3{battle.pitch, settings_.angle.y, 0} : settings_.angle);
    }

    // ゲームモードでロックオン中は、オフセットを更新した時点で終了して通常の追従設定が競合するのを避けます。
    if (settings_.currentMode == Mode::kGame && isLockingOn) {
        // ロックオン中もエディターで調整した視点オフセットだけは即時反映する。
        camera->SetLockOnOffset(settings_.lockOnOffset);
        return;
    }

    // ゲームモードでは誘導操作なら照準可能な追従にし、それ以外は選択中の追従方式を適用します。
    if (settings_.currentMode == Mode::kGame) {
        camera->SetFollowMode(camera->IsGuidedGameplay() ? engine::Camera::FollowMode::kAimable : settings_.gameFollowMode);
        camera->SetLockOnOffset(settings_.lockOnOffset);
        camera->ConfigFixedPoint(settings_.fixedPointPos, settings_.fixedPointAngle);

        // 選択した方式が周回の場合は、半径、高さ、周回速度と中心の補正を適用します。
        if (settings_.gameFollowMode == engine::Camera::FollowMode::kOrbit) {
            camera->SetOrbitParams(settings_.orbitRadius, settings_.orbitHeight, settings_.orbitSpeed);
            camera->SetOrbitCenterOffset(settings_.orbitCenterOffset);
            camera->SetOrbitCenterHeight(settings_.orbitCenterHeight);
        }

        // 追従するプレイヤーがいる場合は、入力状態に応じてゲームカメラとの設定同期を判断します。
        if (player) {
            camera->SetFollowTarget(player);
            engine::InputManager* input = engine::InputManager::GetInstance();

            // マウス移動とジャイロの状態を取り出し、手動入力があるか確認します。
            // マウスの移動量チェック
            engine::Vector2 mouseDelta = input->GetMouseMoveDelta();
            bool isMouseMoving = (std::abs(mouseDelta.x) > 0.0f || std::abs(mouseDelta.y) > 0.0f);

            // ジャイロ入力チェック (感度 0.05f)
            engine::Vector3 gyro = input->GetGyroscope();
            bool isGyroActive = (std::abs(gyro.x) > 0.05f || std::abs(gyro.y) > 0.05f || std::abs(gyro.z) > 0.05f);

            // 右ボタン・マウス移動・右スティック・ジャイロのどれかでカメラ操作中とみなします。
            bool isControllingCamera = input->IsMouseButtonPressed(1) || isMouseMoving ||
                (std::abs(input->GetRightStick().x) > 0.1f) ||
                (std::abs(input->GetRightStick().y) > 0.1f) ||
                isGyroActive;

            // 誘導操作でなければ、入力中かどうかと追従方式に応じて設定と実カメラの情報を同期します。
            if (camera->IsGuidedGameplay()) {
                // 自動構図の基準値は上で反映済み。入力はCamera内の補助角度だけに蓄積する。
            }
            else if (isControllingCamera) {
                // 操作中はカメラの値をEditorへ逆反映し、設定値の上書きは行わない。
                // 操作中に照準または一人称モードなら、実カメラのラジアンの角度を度へ直して設定に控えます。
                if (settings_.gameFollowMode == engine::Camera::FollowMode::kAimable ||
                    settings_.gameFollowMode == engine::Camera::FollowMode::kFirstPerson) {

                    engine::Vector3 currentRot = camera->GetRotation();
                    float toDeg = 180.0f / std::numbers::pi_v<float>;

                    settings_.angle.x = currentRot.x * toDeg;
                    settings_.angle.y = currentRot.y * toDeg;
                    settings_.angle.z = currentRot.z * toDeg;
                }
            }
            else {
                // 操作していない時だけ、モードに必要な設定値をカメラに流し込む
                // 入力のない間は、照準・固定追従・一人称のいずれかに保存済み設定を適用します。
                if (settings_.gameFollowMode == engine::Camera::FollowMode::kAimable ||
                    settings_.gameFollowMode == engine::Camera::FollowMode::kFixed ||
                    settings_.gameFollowMode == engine::Camera::FollowMode::kFirstPerson) {
                    camera->ConfigAimable(settings_.distance, settings_.height, settings_.angle);
                }
            }
        }
        // プレイヤーがいなければ、以前の追従先を残さないよう解除します。
        else {
            camera->SetFollowTarget(nullptr);
        }

    }
    // 編集モードではプレイヤーへの追従を外し、エディターの自由カメラへ切り替えます。
    else {
        camera->SetFollowTarget(nullptr);
        camera->SetFollowMode(engine::Camera::FollowMode::kAimable);

        // 演出などがカメラをオーバーライドしていない場合にだけ、自由操作で姿勢を更新します。
        if (!camera->IsOverridden()) {
            UpdateFreeCamera(camera);
        }
    }
}

void CameraEditor::UpdateFreeCamera(engine::Camera* camera) {
    engine::InputManager* input = engine::InputManager::GetInstance();

    // 現在のカメラ情報を取得
    engine::Vector3 eye = camera->GetEye();
    engine::Vector3 rotation = camera->GetRotation();

    // マウスの右ボタンを押している間だけ、移動量でピッチとヨーを変えます。
    if (input->IsMouseButtonPressed(1)) { // 1 = Right Click
        engine::Vector2 mouseDelta = input->GetMouseMoveDelta();

        // 感度をかけて加算
        rotation.y += mouseDelta.x * settings_.mouseSensitivity;
        rotation.x += mouseDelta.y * settings_.mouseSensitivity;

        // マウスで加算する回転はラジアン。上下を制限し、真上や真下をまたいで操作方向が反転するのを避ける。
        const float pitchLimit = 1.5f; // 約85度
        // ピッチが正方向の上限を超えた場合は上限へ戻します。
        if (rotation.x > pitchLimit)
            rotation.x = pitchLimit;
        // ピッチが負方向の限界を超えた場合も、限界内に保ちます。
        if (rotation.x < -pitchLimit)
            rotation.x = -pitchLimit;

        camera->SetRotation(rotation);
    }

    // 回転後の前・右・上の軸を求め、カメラから見た方向に移動できるようにします。
    engine::Vector3 forward, right, up;

    // 前方ベクトル (カメラが向いている方向)
    forward.x = std::sin(rotation.y) * std::cos(rotation.x);
    forward.y = -std::sin(rotation.x);
    forward.z = std::cos(rotation.y) * std::cos(rotation.x);

    // 右ベクトル (カメラから見た水平の右方向)
    right.x = std::cos(rotation.y);
    right.y = 0.0f;
    right.z = -std::sin(rotation.y);

    // 上ベクトル (前方と右の外積で、カメラから見た「本当の上」を計算)
    up.x = forward.y * right.z - forward.z * right.y;
    up.y = forward.z * right.x - forward.x * right.z;
    up.z = forward.x * right.y - forward.y * right.x;

    // キーとホイールの入力を、この更新で加える移動量へまとめます。
    engine::Vector3 moveVelocity = engine::Vector3::kZero;

    // GameViewをホバーしているか、右クリック中のみ移動・ズームを許可
    if (isGameViewHovered_ || input->IsMouseButtonPressed(1)) {

        // Shiftキーで加速
        float speed = (input->IsKeyPressed(DIK_LSHIFT) || input->IsKeyPressed(DIK_RSHIFT))
            ? settings_.boostSpeed
            : settings_.moveSpeed;

        // W/S: カメラの視線方向へ前進・後退
        if (input->IsKeyPressed(DIK_W)) {
            moveVelocity.x += forward.x * speed;
            moveVelocity.y += forward.y * speed;
            moveVelocity.z += forward.z * speed;
        }

        // Sキーを押している間は、視線の逆方向へ移動します。
        if (input->IsKeyPressed(DIK_S)) {
            moveVelocity.x -= forward.x * speed;
            moveVelocity.y -= forward.y * speed;
            moveVelocity.z -= forward.z * speed;
        }

        // A/D: カメラの左右へ平行移動
        if (input->IsKeyPressed(DIK_D)) {
            moveVelocity.x += right.x * speed;
            moveVelocity.y += right.y * speed;
            moveVelocity.z += right.z * speed;
        }

        // Aキーを押している間は、右軸とは逆方向へ平行移動します。
        if (input->IsKeyPressed(DIK_A)) {
            moveVelocity.x -= right.x * speed;
            moveVelocity.y -= right.y * speed;
            moveVelocity.z -= right.z * speed;
        }

        // Q/E: 空間の上下へ移動
        if (input->IsKeyPressed(DIK_E)) {
            moveVelocity.y += speed; // Eで上昇
        }

        // Qキーを押している間は、ワールドYの負方向へ下がります。
        if (input->IsKeyPressed(DIK_Q)) {
            moveVelocity.y -= speed; // Qで下降
        }

        // ホイール回転: ズーム移動
        float wheelDelta = input->GetMouseWheelDelta();

        // ホイールに入力があれば、符号に応じて視線方向へ近づくか離れます。
        if (wheelDelta != 0.0f) {
            float wheelDir = (wheelDelta > 0.0f) ? 1.0f : -1.0f;
            float zoomSpeed = speed * 3.0f;

            moveVelocity.x += forward.x * zoomSpeed * wheelDir;
            moveVelocity.y += forward.y * zoomSpeed * wheelDir;
            moveVelocity.z += forward.z * zoomSpeed * wheelDir;
        }
    }

    // 中クリック (ホイール押し込み) : パン(平行)移動
    // GameViewホバー中、または中クリック押下中のみ許可
    if (isGameViewHovered_ || input->IsMouseButtonPressed(2)) {
        // 実際に中ボタンを押している場合だけ、マウスの動きを平行移動に変えます。
        if (input->IsMouseButtonPressed(2)) { // 2 = Middle Click
            engine::Vector2 mouseDelta = input->GetMouseMoveDelta();
            float panSpeed = settings_.moveSpeed * 0.1f;

            // X移動: マウスの動きと逆方向に右ベクトルを使って移動
            moveVelocity.x -= right.x * mouseDelta.x * panSpeed;
            moveVelocity.y -= right.y * mouseDelta.x * panSpeed;
            moveVelocity.z -= right.z * mouseDelta.x * panSpeed;

            // Y移動: マウスの動きに合わせて上ベクトルを使って移動
            moveVelocity.x += up.x * mouseDelta.y * panSpeed;
            moveVelocity.y += up.y * mouseDelta.y * panSpeed;
            moveVelocity.z += up.z * mouseDelta.y * panSpeed;
        }
    }

    // キー・ホイール・パンから合算したこの更新の移動量を直接加える。秒単位の速度として時間差は掛けない。
    eye.x += moveVelocity.x;
    eye.y += moveVelocity.y;
    eye.z += moveVelocity.z;

    camera->SetEye(eye);

    // 保存が抑止されていない場合にだけ、自由カメラの姿勢を保存用設定へコピーします。
    if (!IsEditorStateSaveBlocked()) {
        const bool stateChanged =
            eye.x != settings_.editorCameraPos.x ||
            eye.y != settings_.editorCameraPos.y ||
            eye.z != settings_.editorCameraPos.z ||
            rotation.x != settings_.editorCameraAngle.x ||
            rotation.y != settings_.editorCameraAngle.y ||
            rotation.z != settings_.editorCameraAngle.z;
        settings_.editorCameraPos = eye;
        settings_.editorCameraAngle = rotation;

        // 最後に姿勢が変わってから 500 ミリ秒後を保存期限にし、カメラを動かし続ける間はファイルへ書かない。
        if (stateChanged) {
            editorStateDirty_ = true;
            editorStateSaveReadyAt_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        }

        // 未保存の姿勢があり、変更が止まってから保存期限を迎えたときに書き出します。
        if (editorStateDirty_ && std::chrono::steady_clock::now() >= editorStateSaveReadyAt_) {
            SaveEditorState();
        }
    }

    // 移動後の視点から前方10ワールド単位を、新しい注視点にします。
    engine::Vector3 newTarget;
    newTarget.x = eye.x + forward.x * 10.0f;
    newTarget.y = eye.y + forward.y * 10.0f;
    newTarget.z = eye.z + forward.z * 10.0f;

    camera->SetTarget(newTarget);
}

void CameraEditor::BeginPreviewUiFrame() {
    // 各ウィンドウがこのフレームで開いたかを再判定し、隠れたプレビューの描画を止める。
    cameraPreviewPanelVisibleThisFrame_ = false;
    sceneCameraPreviewWindowVisibleThisFrame_ = false;
}

void CameraEditor::SetSceneCameraPreviewWindowVisible(bool visible) {
    // シーンカメラのプレビュー描画を UI の可視状態に連動させる。
    sceneCameraPreviewWindowVisibleThisFrame_ = visible;
}

bool CameraEditor::HasSceneCameraPreviewTarget() const {
    // 選択中の Camera Object が現シーンに存在し、姿勢を解決できる場合だけ描画する。
    engine::Vector3 eye{};
    engine::Vector3 target{};
    return GetSelectedCameraObject() != nullptr && ResolveCinematicPreviewPose(eye, target);
}

bool CameraEditor::ShouldRenderCameraPreview() const {
    // 表示中でも設定 FPS に達していなければオフスクリーン描画を省く。
#ifdef USE_IMGUI
    // ツール設定で通常プレビューが非表示なら、描画テクスチャを更新しません。
    if (!EditorToolSettings::Get().IsVisible("cameraPreview"))
        return false;
#endif
    return settings_.cameraPreviewVisible &&
        cameraPreviewPanelVisibleThisFrame_ &&
        IsPreviewUpdateDue(lastCameraPreviewRenderTime_, hasRenderedCameraPreview_);
}

bool CameraEditor::ShouldRenderSceneCameraPreview() const {
    // 専用ウィンドウ、対象オブジェクト、更新間隔の三条件を満たした場合だけ描く。
#ifdef USE_IMGUI
    // ツール設定でシーンカメラのプレビューが非表示なら、対象を描画しません。
    if (!EditorToolSettings::Get().IsVisible("sceneCameraPreview"))
        return false;
#endif
    return sceneCameraPreviewWindowVisibleThisFrame_ &&
        HasSceneCameraPreviewTarget() &&
        IsPreviewUpdateDue(lastSceneCameraPreviewRenderTime_, hasRenderedSceneCameraPreview_);
}

void CameraEditor::NotifyCameraPreviewRendered(bool sceneCameraPreview) {
    // 通常プレビューとシーンカメラプレビューの時刻を別々に記録する。
    const auto now = std::chrono::steady_clock::now();

    // シーンカメラ側が描画された場合、その専用プレビューの最終更新を記録します。
    if (sceneCameraPreview) {
        hasRenderedSceneCameraPreview_ = true;
        lastSceneCameraPreviewRenderTime_ = now;
    }
    // 通常プレビューの完了時刻はシーンカメラ側とは別に保存します。
    else {
        hasRenderedCameraPreview_ = true;
        lastCameraPreviewRenderTime_ = now;
    }
}

bool CameraEditor::IsPreviewUpdateDue(
    const std::chrono::steady_clock::time_point& lastRenderTime,
    bool hasRenderedFrame) const {
    // 初回は即時描画し、以降は設定 FPS から求めた間隔で更新する。
    if (!hasRenderedFrame) {
        return true;
    }

    const std::int32_t previewFps = std::clamp(settings_.cameraPreviewFps, 1, 60);
    const float updateInterval = 1.0f / static_cast<float>(previewFps);
    const std::chrono::duration<float> elapsed = std::chrono::steady_clock::now() - lastRenderTime;
    return elapsed.count() >= updateInterval;
}

void CameraEditor::InvalidateCameraPreviews() {
    // 次の表示時に古いテクスチャを使わず、両プレビューを描き直す。
    hasRenderedCameraPreview_ = false;
    hasRenderedSceneCameraPreview_ = false;
}

engine::Camera* CameraEditor::PreparePreviewCamera(float aspectRatio) {
    // ゲームカメラの投影条件を借り、エディターの視点だけを専用カメラに設定する。
    const engine::Vector3 eye = GetPreviewCameraEye();
    const engine::Vector3 target = eye + GetPreviewCameraForward() * 10.0f;
    float fovY = 0.45f;
    float nearClip = 0.1f;
    float farClip = 1000.0f;

    // メインカメラがある場合は、その縦画角と前後のクリップ距離で初期値を上書きします。
    if (engine::Camera* mainCamera = CameraManager::GetInstance()->GetMainCamera()) {
        fovY = mainCamera->GetFovY();
        nearClip = mainCamera->GetNearClip();
        farClip = mainCamera->GetFarClip();
    }

    return ConfigurePreviewCamera(eye, target, aspectRatio, fovY, nearClip, farClip);
}

engine::Camera* CameraEditor::PrepareCinematicPreviewCamera(float aspectRatio) {
    // 選択した Camera Object の画角とクリップ距離で独立したプレビューを作る。
    engine::Vector3 eye{};
    engine::Vector3 target{};

    // 選択中のカメラの姿勢を解決できない場合は、描画用カメラを返しません。
    if (!ResolveCinematicPreviewPose(eye, target)) {
        return nullptr;
    }

    float fovY = 0.45f;
    float nearClip = 0.1f;
    float farClip = 1000.0f;

    // 選択中のカメラがある場合は、そのシーン設定から投影条件を読みます。
    if (engine::Object3D* selectedCamera = GetSelectedCameraObject()) {
        const SceneCameraSettings& settings = selectedCamera->GetSceneCameraSettings();
        fovY = settings.fovY;
        nearClip = settings.nearClip;
        farClip = settings.farClip;
    }
    // 選択設定を取得できずメインカメラがあれば、そちらの投影条件を代わりに使います。
    else if (engine::Camera* mainCamera = CameraManager::GetInstance()->GetMainCamera()) {
        fovY = mainCamera->GetFovY();
        nearClip = mainCamera->GetNearClip();
        farClip = mainCamera->GetFarClip();
    }

    return ConfigurePreviewCamera(eye, target, aspectRatio, fovY, nearClip, farClip);
}

engine::Camera* CameraEditor::ConfigurePreviewCamera(
    const engine::Vector3& eye,
    const engine::Vector3& target,
    float aspectRatio,
    float fovY,
    float nearClip,
    float farClip) {
    // 操作と追従を持たないプレビュー専用カメラへ、カットの視点と投影を設定します。
    // このインスタンスで最初にプレビューを準備する場合だけ、専用カメラを初期化します。
    if (!previewCameraInitialized_) {
        previewCamera_.Initialize();
        previewCameraInitialized_ = true;
    }

    const engine::Vector3 forward = NormalizeSafe(target - eye, { 0.0f, 0.0f, 1.0f });
    const engine::Vector3 rotation = MakeRotationFromForward(forward);
    previewCamera_.SetInputEnabled(false);
    previewCamera_.SetFollowTarget(nullptr);
    previewCamera_.SetFollowMode(engine::Camera::FollowMode::kFixedPoint);
    previewCamera_.ConfigFixedPoint(eye, rotation);
    previewCamera_.SetEye(eye);
    previewCamera_.SetTarget(target);
    previewCamera_.SetRotation(rotation);
    previewCamera_.SetFovY(fovY);
    previewCamera_.SetClipRange(nearClip, farClip);
    previewCamera_.SetLookAtPreviewView(eye, target, aspectRatio);
    return &previewCamera_;
}

void CameraEditor::ApplyConfiguredCameraPreview(engine::Camera* camera) const {
    // 設定値を渡されたカメラへ反映する際は追従を解除し、固定視点にする。
    // 反映先のカメラを渡されていなければ操作を変更しません。
    if (!camera) {
        return;
    }

    const engine::Vector3 eye = GetConfiguredCameraEye();
    const engine::Vector3 forward = GetConfiguredCameraForward();
    const engine::Vector3 rotation = MakeRotationFromForward(forward);

    camera->SetFollowTarget(nullptr);
    camera->SetFollowMode(engine::Camera::FollowMode::kFixedPoint);
    camera->ConfigFixedPoint(eye, rotation);
    camera->SetEye(eye);
    camera->SetTarget(eye + forward * 10.0f);
    camera->SetRotation(rotation);
    camera->Update();
}

void CameraEditor::SetMode(Mode mode) {
    settings_.currentMode = mode;
}

void CameraEditor::SetEditorStateSaveBlocker(uint32_t blocker, bool enabled) {
    // ブックマーク復元など保存すべきでない操作をビット単位で重ねて抑止する。
    // 抑止要求を有効にした場合、その理由に対応するビットを加えます。
    if (enabled) {
        editorStateSaveBlockers_ |= blocker;
    }
    // 抑止解除は指定ビットだけを消し、別の理由による抑止を保持します。
    else {
        editorStateSaveBlockers_ &= ~blocker;
    }
}

void CameraEditor::SetEditorCameraTransform(const engine::Vector3& position, const engine::Vector3& rotation) {
    const bool stateChanged =
        position.x != settings_.editorCameraPos.x ||
        position.y != settings_.editorCameraPos.y ||
        position.z != settings_.editorCameraPos.z ||
        rotation.x != settings_.editorCameraAngle.x ||
        rotation.y != settings_.editorCameraAngle.y ||
        rotation.z != settings_.editorCameraAngle.z;
    settings_.editorCameraPos = position;
    settings_.editorCameraAngle = rotation;

    // 前の姿勢と異なり、状態保存が許可されている場合だけ500ミリ秒後の保存を予約します。
    if (stateChanged && !IsEditorStateSaveBlocked()) {
        editorStateDirty_ = true;
        editorStateSaveReadyAt_ =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    }

    // ブックマーク復帰後も設定値と実カメラのTransformを一致させます。

    engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();

    // メインカメラがあれば、保存用の姿勢と同じ位置・回転へ動かします。
    if (camera) {
        camera->SetEye(position);
        camera->SetRotation(rotation);
        engine::Vector3 forward;
        forward.x = std::sin(rotation.y) * std::cos(rotation.x);
        forward.y = -std::sin(rotation.x);
        forward.z = std::cos(rotation.y) * std::cos(rotation.x);
        camera->SetTarget(position + forward * 10.0f);

        // 設定した視点を追従で動かされないよう、追従対象を外します。
        camera->SetFollowTarget(nullptr);
    }
}

bool CameraEditor::FocusSceneObject(engine::Object3D* object) {
    // モデルの外接範囲を基準に画角を決め、表示モデルのない物体は判定範囲を使う。
    // 配置物が無いか現在のシーンに属さない場合は、フォーカスできません。
    if (!object || !IsObjectInCurrentScene(object)) {
        return false;
    }

    object->UpdateWorldMatrix();

    engine::Aabb focusBounds = object->GetModelWorldAabb();
    const engine::Aabb collisionBounds = object->GetAabb();
	if (object->GetModelName().empty() && object->GetCollider() && IsValidFocusBounds(collisionBounds)) {
        // 見た目を持たない当たり判定Objectは、実際の判定範囲を画角の基準にします。
        focusBounds = collisionBounds;
    }

    // モデルまたは当たり判定から有効な範囲を得られなければ、中心と距離を求められません。
    if (!IsValidFocusBounds(focusBounds)) {
        return false;
    }

    const engine::Vector3 focusTarget = {
        (focusBounds.min.x + focusBounds.max.x) * 0.5f,
        (focusBounds.min.y + focusBounds.max.y) * 0.5f,
        (focusBounds.min.z + focusBounds.max.z) * 0.5f,
    };

    const engine::Vector3 halfExtent = {
        (focusBounds.max.x - focusBounds.min.x) * 0.5f,
        (focusBounds.max.y - focusBounds.min.y) * 0.5f,
        (focusBounds.max.z - focusBounds.min.z) * 0.5f,
    };

    // モデル境界の最大半幅をワールド距離の目安にし、小さな Object でも視点が埋まらない距離から見る。
    const float objectRadius = (std::max)({ halfExtent.x, halfExtent.y, halfExtent.z, 0.5f });

    const engine::Vector3 worldForward = GetObjectWorldForward(object);
    const engine::Vector3 frontDirection = NormalizeSafe(
        { worldForward.x, 0.0f, worldForward.z },
        { 0.0f, 0.0f, 1.0f });
    const float focusDistance = std::clamp(objectRadius * 2.8f + 2.0f, 8.0f, 120.0f);
    const float focusHeight = std::clamp(objectRadius * 0.3f, 2.0f, 24.0f);
    const engine::Vector3 focusEye = focusTarget + frontDirection * focusDistance + engine::Vector3{ 0.0f, focusHeight, 0.0f };
    const engine::Vector3 rotation = MakeRotationFromForward(
        NormalizeSafe(focusTarget - focusEye, { 0.0f, 0.0f, -1.0f }));

    settings_.editorCameraPos = focusEye;
    settings_.editorCameraAngle = rotation;
    SetMode(Mode::kEditor);
    SetEditorCameraTransform(focusEye, rotation);

    engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();

    // メインカメラを取得できたら、設定した編集姿勢の注視点を境界の中心へ合わせます。
    if (camera) {
        camera->SetTarget(focusTarget);
        camera->Update();
    }

    DebugConsole::GetInstance()->AddLog("Focused Scene Object: " + object->GetName());
    return true;
}

bool CameraEditor::FocusSelectedCameraObject() {
    engine::Object3D* cameraObject = GetSelectedCameraObject();
    engine::Vector3 eye{};
    engine::Vector3 target{};

    // 選択が無いかカメラの視点・注視点を解決できなければ、フォーカスを開始しません。
    if (!cameraObject || !ResolveSceneCameraPose(cameraObject, eye, target)) {
        return false;
    }

    const engine::Vector3 forward = NormalizeSafe(target - eye, { 0.0f, 0.0f, 1.0f });
    const engine::Vector3 worldUp = { 0.0f, 1.0f, 0.0f };
    const float focusDistance = (std::max)(4.0f, settings_.cameraGuideSize * 8.0f);
    const float focusHeight = (std::max)(2.0f, settings_.cameraGuideSize * 4.0f);
    const engine::Vector3 focusTarget = eye + worldUp * (settings_.cameraGuideSize * 0.6f);
    const engine::Vector3 focusEye = eye - forward * focusDistance + worldUp * focusHeight;
    const engine::Vector3 focusForward = NormalizeSafe(focusTarget - focusEye, forward);
    const engine::Vector3 rotation = MakeRotationFromForward(focusForward);

    // Camera Object本体を確認できるように、実カメラ位置より少し後ろ上へ寄せる。
    settings_.editorCameraPos = focusEye;
    settings_.editorCameraAngle = rotation;
    SetMode(Mode::kEditor);
    SetEditorCameraTransform(focusEye, rotation);

    engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();

    // メインカメラが有効なら、模型を捉える注視点へ合わせて行列を更新します。
    if (camera) {
        camera->SetTarget(focusTarget);
        camera->Update();
    }

    DebugConsole::GetInstance()->AddLog("Focused Camera Object: " + cameraObject->GetName());
    return true;
}

void CameraEditor::ApplyOrbitStartAngle(engine::Camera* camera, bool resetSmoothing) {
    // 開始角度は設定ファイル上の度から Camera API が使うラジアンへ変換する。
    if (!camera)
        return;

    float angle = DegToRad(settings_.orbitStartAngleDeg);
    camera->SetOrbitCenterOffset(settings_.orbitCenterOffset);
    camera->SetOrbitCenterHeight(settings_.orbitCenterHeight);
    camera->SetOrbitParams(settings_.orbitRadius, settings_.orbitHeight, settings_.orbitSpeed);
    camera->SetOrbitAngle(angle);

    // 開始角へ旧姿勢の補間を持ち越したくない場合は、追従補間をリセットします。
    if (resetSmoothing) {
        camera->ResetFollowSmoothing();
    }
}

void CameraEditor::CaptureOrbitStartFromCurrentCamera(engine::Camera* camera) {
    // 現在位置を中心との差分に変換して、再現可能な半径・高さ・開始角として保存する。
    if (!camera)
        return;

    const engine::Vector3 center = GetOrbitCenter();
    const engine::Vector3 diff = camera->GetEye() - center;
    const float horizontalDistance = std::sqrt(diff.x * diff.x + diff.z * diff.z);

    // 中心から十分な水平差がある場合だけ、水平距離と開始角を求めます。
    if (horizontalDistance > 0.001f) {
        settings_.orbitRadius = (horizontalDistance < 1.0f) ? 1.0f : horizontalDistance;
        settings_.orbitStartAngleDeg = RadToDeg(std::atan2(diff.z, diff.x));
    }
    // 中心とほぼ重なる場合は開始角を維持し、周回半径だけを1単位にします。
    else {
        settings_.orbitRadius = 1.0f;
    }

    settings_.orbitHeight = diff.y;
    ApplyOrbitStartAngle(camera, true);
    SaveSettings();
    DebugConsole::GetInstance()->AddLog("Orbit camera start position captured from current camera.");
}

void CameraEditor::SetOrbitCenterFromWorld(const engine::Vector3& center) {
    // プレイヤー追従時はワールド位置をプレイヤー相対オフセットに変換する。
    if (targetPlayer_) {
        engine::Vector3 targetPos = targetPlayer_->GetWorldPosition();
        settings_.orbitCenterOffset = center - targetPos;
        settings_.orbitCenterOffset.y -= settings_.orbitCenterHeight;
        return;
    }

    settings_.fixedPointPos = center - settings_.orbitCenterOffset;
}

void CameraEditor::SetOrbitStartFromWorld(const engine::Vector3& startEye) {
    // ワールド座標の開始視点から周回半径と角度を逆算する。
    const engine::Vector3 center = GetOrbitCenter();
    const engine::Vector3 diff = startEye - center;
    const float horizontalDistance = std::sqrt(diff.x * diff.x + diff.z * diff.z);

    // 開始視点が周回中心から水平方向に離れている場合にだけ、半径と角度を上書きします。
    if (horizontalDistance > 0.001f) {
        settings_.orbitRadius = (horizontalDistance < 1.0f) ? 1.0f : horizontalDistance;
        settings_.orbitStartAngleDeg = RadToDeg(std::atan2(diff.z, diff.x));
    }

    settings_.orbitHeight = diff.y;
}

engine::Vector3 CameraEditor::GetOrbitCenter() const {
    // プレイヤーがいる場合は、その座標に高さとオフセットを加えた点を周回中心にします。
    if (targetPlayer_) {
        engine::Vector3 center = targetPlayer_->GetWorldPosition();
        center.x += settings_.orbitCenterOffset.x;
        center.y += settings_.orbitCenterHeight + settings_.orbitCenterOffset.y;
        center.z += settings_.orbitCenterOffset.z;
        return center;
    }

    return settings_.fixedPointPos + settings_.orbitCenterOffset;
}

engine::Vector3 CameraEditor::GetOrbitStartEye() const {
    // 保存した半径・高さ・角度から周回開始時のワールド視点を復元する。
    const engine::Vector3 center = GetOrbitCenter();
    const float startAngle = DegToRad(settings_.orbitStartAngleDeg);
    return {
        center.x + settings_.orbitRadius * std::cos(startAngle),
        center.y + settings_.orbitHeight,
        center.z + settings_.orbitRadius * std::sin(startAngle)
    };
}

engine::Vector3 CameraEditor::GetConfiguredCameraEye() const {
    // 固定・周回・追従で視点の算出方法が異なり、対象不在時は現在視点を維持する。
    // 固定点方式では、設定したワールド位置をそのまま視点にします。
    if (settings_.gameFollowMode == engine::Camera::FollowMode::kFixedPoint) {
        return settings_.fixedPointPos;
    }

    // 周回方式では、設定した開始角から復元した周回の始点を視点にします。
    if (settings_.gameFollowMode == engine::Camera::FollowMode::kOrbit) {
        return GetOrbitStartEye();
    }

    engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();
    const engine::Vector3 fallback = camera ? camera->GetEye() : settings_.fixedPointPos;

    // プレイヤーが見つからない追従方式では、メインカメラの現在視点か固定点を返します。
    if (!targetPlayer_) {
        return fallback;
    }

    engine::Vector3 target = targetPlayer_->GetWorldPosition();
    target.y += settings_.height;
    engine::Vector3 forward = GetConfiguredCameraForward();
    return target - forward * settings_.distance;
}

engine::Vector3 CameraEditor::GetConfiguredCameraForward() const {
    // 固定・周回・通常追従の各設定からプレビュー用の前方を揃えて返す。
    // 固定点方式は、保存した固定角の前方を使います。
    if (settings_.gameFollowMode == engine::Camera::FollowMode::kFixedPoint) {
        return NormalizeSafe(CalculateEditorForward(settings_.fixedPointAngle), { 0.0f, 0.0f, 1.0f });
    }

    // 周回方式は、周回開始点から中心への向きを使います。
    if (settings_.gameFollowMode == engine::Camera::FollowMode::kOrbit) {
        return NormalizeSafe(GetOrbitCenter() - GetOrbitStartEye(), { 0.0f, 0.0f, 1.0f });
    }

    engine::Vector3 rotation = {
        DegToRad(settings_.angle.x),
        DegToRad(settings_.angle.y),
        DegToRad(settings_.angle.z)
    };

    return NormalizeSafe(CalculateEditorForward(rotation), { 0.0f, 0.0f, 1.0f });
}

engine::Vector3 CameraEditor::GetPreviewCameraEye() const {
    // ゲーム実行中はエディター自由カメラを映し、編集時はゲームカメラ設定を映す。
    if (settings_.currentMode == Mode::kGame) {
        return settings_.editorCameraPos;
    }

    return GetConfiguredCameraEye();
}

engine::Vector3 CameraEditor::GetPreviewCameraForward() const {
    // 視点と同じモード切り替えで向きを選び、混在したプレビューを防ぐ。
    if (settings_.currentMode == Mode::kGame) {
        return NormalizeSafe(CalculateEditorForward(settings_.editorCameraAngle), { 0.0f, 0.0f, 1.0f });
    }

    return GetConfiguredCameraForward();
}

const char* CameraEditor::GetPreviewCameraLabel() const {
    // 現在の操作モードとプレビュー側の視点名を対応させる。
    return settings_.currentMode == Mode::kGame ? "自由カメラ" : "ゲームカメラ";
}

engine::Object3D* CameraEditor::FindSceneObjectByName(const std::string& name) const {
    // 保存済みの名前から現シーンの実体を探し、シーン切り替え後の古いポインタを使わない。
    // 名前が空欄なら、同名の対象を探しません。
    if (name.empty()) {
        return nullptr;
    }

    BaseScene* scene = GetCurrentScene();

    // 現シーンが取得できなければ、探索する配置一覧がありません。
    if (!scene) {
        return nullptr;
    }

    for (const auto& object : scene->GetObjects()) {
        // 有効な配置物の名前が一致した時点で、その実体を返します。
        if (object && object->GetName() == name) {
            return object.get();
        }
    }

    return nullptr;
}

void CameraEditor::SetSelectedCameraObject(engine::Object3D* cameraObject) {
    // 現シーンの Camera Object だけを保持し、選択変更時はプレビューを無効化する。
    engine::Object3D* newSelection = cameraObject &&
        cameraObject->IsCameraObject() &&
        IsObjectInCurrentScene(cameraObject)
        ? cameraObject
        : nullptr;
    // 選択先が実際に変わった場合にだけ、別のカメラ用描画をやり直します。
    if (selectedCameraObject_ != newSelection) {
        selectedCameraObject_ = newSelection;
        hasRenderedSceneCameraPreview_ = false;
    }
}

engine::Object3D* CameraEditor::GetSelectedCameraObject() const {
    // 以前のシーンの選択ポインタは返さない。
    if (!selectedCameraObject_ || !IsObjectInCurrentScene(selectedCameraObject_)) {
        return nullptr;
    }

    return selectedCameraObject_->IsCameraObject() ? selectedCameraObject_ : nullptr;
}

bool CameraEditor::ResolveSceneCameraPose(const engine::Object3D* cameraObject, engine::Vector3& eye, engine::Vector3& target) const {
    const auto* scene = SceneManager::GetInstance()->GetCurrentScene();
    return cameraObject && scene && CameraManager::ResolveSceneCameraPose(*cameraObject, scene->GetObjects(), eye, target);
}

engine::Camera::CameraOverrideParams CameraEditor::MakeRuntimeOverrideParams(const engine::Object3D* cameraObject) const {
    // 編集用設定を Camera の実行用形式へ変換し、追従対象の名前を現シーンで解決する。
    engine::Camera::CameraOverrideParams runtime;

    // カメラオブジェクトを指定されなければ、既定のオーバーライド値を返します。
    if (!cameraObject) {
        return runtime;
    }

    const SceneCameraSettings& settings = cameraObject->GetSceneCameraSettings();
    runtime.duration = settings.blendInDuration;
    runtime.exitDuration = settings.blendOutDuration;
    runtime.easing = static_cast<engine::Camera::OverrideEasing>(settings.easing);
    runtime.eyeSource = engine::Camera::OverrideEyeSource::kSceneObject;

    // カメラ自身以外が視点なら、シーン内の実体を後で解決できるように名前を控えます。
    if (settings.eyeSource != SceneCameraEyeSource::kCameraObject) {
        runtime.eyeObjectName = settings.eyeObjectName;
    }

    // 別のオブジェクトとの相対位置を使う方式には、方向基準の参照名も渡します。
    if (settings.eyeSource == SceneCameraEyeSource::kSceneObjectRelativeToReference) {
        runtime.eyeSource = engine::Camera::OverrideEyeSource::kSceneObjectRelativeToReference;
        runtime.eyeReferenceObjectName = settings.eyeReferenceObjectName;
    }

    runtime.eyeObjectOffset = settings.eyeOffset;
    runtime.eyeFollowMode = static_cast<engine::Camera::OverrideFollowMode>(settings.eyeFollowMode);
    runtime.eyeFollowResponse = settings.eyeFollowResponse;

    switch (settings.targetMode) {
    case SceneCameraTargetMode::kFixedPoint:
        // 固定点を見続ける方式では、ワールド座標を使い各軸の追従を解除します。
        runtime.targetSource = engine::Camera::OverrideTargetSource::kFixed;
        runtime.fixedTargetPos = settings.fixedTarget;
        runtime.trackTargetX = false;
        runtime.trackTargetY = false;
        runtime.trackTargetZ = false;
        break;

    case SceneCameraTargetMode::kSceneObject:
        // 対象を追う方式では、名前で調べるオブジェクトと位置の差を渡します。
        runtime.targetSource = engine::Camera::OverrideTargetSource::kSceneObject;
        runtime.targetObjectName = settings.targetObjectName;
        runtime.targetObjectOffset = settings.targetOffset;
        break;

    case SceneCameraTargetMode::kCameraForward:
    default:
        // カメラの前方を見る方式では、視点から進む距離と加算する位置の差を渡します。
        runtime.targetSource = engine::Camera::OverrideTargetSource::kEyeObjectForward;
        runtime.eyeForwardDistance = settings.forwardDistance;
        runtime.targetObjectOffset = settings.targetOffset;
        break;
    }

    runtime.targetFollowMode = static_cast<engine::Camera::OverrideFollowMode>(settings.targetFollowMode);
    runtime.targetFollowResponse = settings.targetFollowResponse;
    // 名前指定の視点・基準・注視先は保存済みOverrideと同じ手順で解決する。
    // Camera自身が視点なら空名を渡し、同名Objectに差し替わらないようにする。
    runtime = MakeRuntimeOverrideParams(runtime, const_cast<engine::Object3D*>(cameraObject));
    // カメラ自身が視点の場合は実体を確定した後で、設定の表示に使う名前を補います。
    if (settings.eyeSource == SceneCameraEyeSource::kCameraObject) {
        runtime.eyeObjectName = cameraObject->GetName();
    }
    return runtime;
}

engine::Camera::CameraOverrideParams CameraEditor::MakeRuntimeOverrideParams(
    const engine::Camera::CameraOverrideParams& params,
    engine::Object3D* fallbackEyeObject) const {
    // 保存されたポインタを破棄して名前から再解決し、視点だけは予備対象を使えるようにする。
    engine::Camera::CameraOverrideParams runtime = params;
    runtime.eyeObject = nullptr;
    runtime.eyeReferenceObject = nullptr;
    runtime.targetFollowObject = nullptr;

    // シーンのオブジェクトを視点にする設定では、古い参照の代わりに現在のシーンから探します。
    if (runtime.eyeSource == engine::Camera::OverrideEyeSource::kSceneObject ||
        runtime.eyeSource == engine::Camera::OverrideEyeSource::kSceneObjectRelativeToReference) {
        runtime.eyeObject = FindSceneObjectByName(runtime.eyeObjectName);

        // 視点用の名前を解決できなければ、呼び出し側が用意した実体を使います。
        if (!runtime.eyeObject) {
            runtime.eyeObject = fallbackEyeObject;
        }
    }

    // 相対視点では、視点の実体に加えて方向基準の実体も名前で解決します。
    if (runtime.eyeSource == engine::Camera::OverrideEyeSource::kSceneObjectRelativeToReference) {
        runtime.eyeReferenceObject = FindSceneObjectByName(runtime.eyeReferenceObjectName);
    }

    // 注視先がオブジェクト方式であれば、現在のシーンで参照し直します。
    if (runtime.targetSource == engine::Camera::OverrideTargetSource::kSceneObject) {
        runtime.targetFollowObject = FindSceneObjectByName(runtime.targetObjectName);
    }

    return runtime;
}

engine::Vector3 CameraEditor::ResolveOverrideEye(const engine::Camera::CameraOverrideParams& params) const {
    // 動的な視点対象が見つからなければ保存された固定座標を使う。
    const engine::Camera::CameraOverrideParams runtime = MakeRuntimeOverrideParams(params);

    // 相対方式で視点と方向基準がそろっている場合に、実行時と同じ相対視点を求めます。
    if (runtime.eyeSource == engine::Camera::OverrideEyeSource::kSceneObjectRelativeToReference &&
        runtime.eyeObject && runtime.eyeReferenceObject) {
        return engine::Camera::ResolveRelativeEye(*runtime.eyeObject, *runtime.eyeReferenceObject, runtime.eyeObjectOffset);
    }
    // 直接追従の視点方式で実体があれば、そのワールド位置へオフセットを加えます。
    if (runtime.eyeSource == engine::Camera::OverrideEyeSource::kSceneObject && runtime.eyeObject) {
        return runtime.eyeObject->GetWorldPosition() + runtime.eyeObjectOffset;
    }

    return params.fixedEyePos;
}

engine::Vector3 CameraEditor::ResolveOverrideTarget(const engine::Camera::CameraOverrideParams& params) const {
    // ターゲットの種類に応じて追従対象・視点前方・固定点を選ぶ。
    const engine::Camera::CameraOverrideParams runtime = MakeRuntimeOverrideParams(params);

    // 注視対象がシーンの実体として見つかった場合は、ワールド位置にオフセットを加えます。
    if (runtime.targetSource == engine::Camera::OverrideTargetSource::kSceneObject && runtime.targetFollowObject) {
        return runtime.targetFollowObject->GetWorldPosition() + runtime.targetObjectOffset;
    }

    // 視点オブジェクトの前方を使う方式で視点が見つかれば、その前方に注視点を作ります。
    if (runtime.targetSource == engine::Camera::OverrideTargetSource::kEyeObjectForward && runtime.eyeObject) {
        const engine::Vector3 eye = runtime.eyeObject->GetWorldPosition() + runtime.eyeObjectOffset;
        return eye
            + GetObjectWorldForward(runtime.eyeObject) * (std::max)(runtime.eyeForwardDistance, 0.01f)
            + runtime.targetObjectOffset;
    }

    return params.fixedTargetPos;
}

bool CameraEditor::ResolveCinematicPreviewPose(engine::Vector3& eye, engine::Vector3& target) const {
    // 演出プレビューは現シーンで選択中の Camera Object の姿勢だけを使用する。
    engine::Object3D* selectedCamera = GetSelectedCameraObject();
    return selectedCamera && ResolveSceneCameraPose(selectedCamera, eye, target);
}

engine::Vector3 CameraEditor::GetConfiguredCameraRight(const engine::Vector3& forward) const {
    // 真上を向いて外積がゼロに近い場合はワールド X を代替軸とする。
    const engine::Vector3 worldUp = { 0.0f, 1.0f, 0.0f };
    engine::Vector3 right = Cross(worldUp, forward);

    // 前方がワールドの上方向とほぼ平行で右の外積がゼロに近い場合、X軸を右方向の代替にします。
    if (Length(right) < 0.0001f) {
        right = { 1.0f, 0.0f, 0.0f };
    }

    return NormalizeSafe(right, { 1.0f, 0.0f, 0.0f });
}

engine::Vector3 CameraEditor::GetConfiguredCameraUp(const engine::Vector3& forward, const engine::Vector3& right) const {
    // 前方と右方向から直交する上方向を求める。
    return NormalizeSafe(Cross(forward, right), { 0.0f, 1.0f, 0.0f });
}

engine::Matrix4x4 CameraEditor::MakeLineBoxMatrix(const engine::Vector3& start, const engine::Vector3& end, float thickness) const {
    // 線分を細い箱で描くため、中点・長さ・向きからワールド行列を組む。
    engine::Math math;
    engine::Vector3 diff = end - start;
    float length = Length(diff);

    // 始点と終点がほぼ同じでも描画箱がつぶれないよう、長さを最小値にします。
    if (length < 0.001f) {
        length = 0.001f;
    }

    engine::Vector3 center = (start + end) * 0.5f;
    float yaw = std::atan2(diff.x, diff.z);
    float horizontalLength = std::sqrt(diff.x * diff.x + diff.z * diff.z);
    float pitch = std::atan2(-diff.y, horizontalLength);

    engine::Matrix4x4 scale = math.MakeScaleMatrix({ thickness, thickness, length });
    engine::Matrix4x4 rotate = math.Multiply(math.MakeRotateXMatrix(pitch), math.MakeRotateYMatrix(yaw));
    engine::Matrix4x4 translate = math.MakeTranslateMatrix(center);
    return math.Multiply(math.Multiply(scale, rotate), translate);
}

bool CameraEditor::PlayOverrideCamera(engine::Camera* camera, const std::string& cameraName) {
    // 再生先のカメラが無ければ、オーバーライドを開始しません。
    if (!camera)
        return false;

    // 登録されているカメラ名を探す
    auto it = overrideParamsMap_.find(cameraName);

    // 指定名が登録されている場合に、現在のシーンで参照を解決して再生します。
    if (it != overrideParamsMap_.end()) {
        // Object参照はシーン切替で無効になるため、再生開始時に名前から解決します。
        camera->StartOverride(MakeRuntimeOverrideParams(it->second));
        return true;
    }

    // 見つからなかった場合はエラーを出す
    DebugConsole::GetInstance()->AddLog("Error: Camera Override '" + cameraName + "' not found!");
    return false;
}

bool CameraEditor::ValidateSceneObjectCamera(const engine::Object3D* cameraObject, std::string& issue) const {
    // 検証は配置や視点を変えない。現在の取得方式で必要な参照だけを照合し、最初の問題を issue に返す。
    issue.clear();
    // 参照が無い、カメラ型でない、または設定が無効なら、必要な視点を検証できません。
    if (!cameraObject || !cameraObject->IsCameraObject() || !cameraObject->GetSceneCameraSettings().enabled) {
        issue = "有効なCamera Objectがありません。";
        return false;
    }

    const SceneCameraSettings& settings = cameraObject->GetSceneCameraSettings();
    // 外部オブジェクトを視点にする方式でその実体が無ければ、まず視点の不足を返します。
    if (settings.eyeSource != SceneCameraEyeSource::kCameraObject &&
        !FindSceneObjectByName(settings.eyeObjectName)) {
        issue = "Eyeの参照Objectが見つかりません: " + settings.eyeObjectName;
        return false;
    }
    // 相対視点方式で方向基準が見つからなければ、その参照名をエラーに示します。
    if (settings.eyeSource == SceneCameraEyeSource::kSceneObjectRelativeToReference &&
        !FindSceneObjectByName(settings.eyeReferenceObjectName)) {
        issue = "Eye方向の参照Objectが見つかりません: " + settings.eyeReferenceObjectName;
        return false;
    }
    // オブジェクトに注視を設定した方式では、対象を解決できる場合だけ有効です。
    if (settings.targetMode == SceneCameraTargetMode::kSceneObject &&
        !FindSceneObjectByName(settings.targetObjectName)) {
        issue = "注視対象Objectが見つかりません: " + settings.targetObjectName;
        return false;
    }
    return true;
}

bool CameraEditor::PlaySceneObjectCamera(engine::Camera* camera, engine::Object3D* cameraObject) {
    // 個別指定がない場合は Camera Object に保存された補間条件で再生する。
    if (!cameraObject) {
        return false;
    }

    const SceneCameraSettings& settings = cameraObject->GetSceneCameraSettings();
    return PlaySceneObjectCamera(
        camera,
        cameraObject,
        settings.blendInDuration,
        settings.blendOutDuration,
        static_cast<std::int32_t>(settings.easing));
}

bool CameraEditor::PlaySceneObjectCamera(
    engine::Camera* camera,
    engine::Object3D* cameraObject,
    float blendInDuration,
    float blendOutDuration,
    std::int32_t easing) {
    // 投影行列はオーバーライド外のため、切り替え前に退避して終了時に戻す。
    std::string issue;
    // 反映するカメラが無いかScene Cameraの必須参照が欠けていれば、開始を拒否します。
    if (!camera || !ValidateSceneObjectCamera(cameraObject, issue)) {
        return false;
    }

    const SceneCameraSettings& settings = cameraObject->GetSceneCameraSettings();

    // 設定が無効の場合は、そのカットの投影と視点を適用しません。
    if (!settings.enabled) {
        return false;
    }

    engine::Camera::CameraOverrideParams runtime = MakeRuntimeOverrideParams(cameraObject);
    runtime.duration = std::max(0.0f, blendInDuration);
    runtime.exitDuration = std::max(0.0f, blendOutDuration);
    runtime.easing = static_cast<engine::Camera::OverrideEasing>(std::clamp(easing, 0, 4));

    SetSelectedCameraObject(cameraObject);
    activeSceneCameraExitDuration_ = runtime.exitDuration;
    activeSceneCameraBlendFov_ = settings.blendFov;

    // タイムラインの途中のカットで退避値を上書きせず、演出開始前の投影条件を終了まで保持する。
    if (!hasSceneCameraProjectionBackup_) {
        sceneCameraBackupFovY_ = camera->GetFovY();
        sceneCameraBackupNearClip_ = camera->GetNearClip();
        sceneCameraBackupFarClip_ = camera->GetFarClip();
        hasSceneCameraProjectionBackup_ = true;
    }

    // 画角補間が指定されている場合に、ブレンドインと同じ秒数と曲線で画角を変えます。
    if (settings.blendFov) {
        camera->BlendFovY(settings.fovY, runtime.duration, runtime.easing);
    // 画角補間を指定していない場合は、カットの画角を即時に反映します。
    } else {
        camera->SetFovY(settings.fovY);
    }
    // 投影条件を先に更新した後、視点と注視点を切り替える Override を開始する。
    camera->SetClipRange(settings.nearClip, settings.farClip);
    camera->UpdateProjectionMatrix();
    camera->StartOverride(runtime);
    return true;
}

void CameraEditor::StopSceneObjectCamera(engine::Camera* camera, bool switchingToNextShot) {
    // 視点補間を終了し、開始前の画角・クリップ距離を復元する。
    if (!camera) {
        return;
    }

    camera->EndOverride(activeSceneCameraExitDuration_);

    // 同じTimeline内の次カットへ移る時は現在の画角と開始前のバックアップを保つ。
    if (switchingToNextShot) {
        return;
    }

    // 演出開始前の投影を保存済みの場合だけ、画角とクリップ範囲を復元します。
    if (hasSceneCameraProjectionBackup_) {
        // カットの画角を補間していた場合は、終了までの時間で元の画角へ戻します。
        if (activeSceneCameraBlendFov_) {
            camera->BlendFovY(sceneCameraBackupFovY_, activeSceneCameraExitDuration_);
        // 補間を使わなかった場合は、保存した画角を即時に戻します。
        } else {
            camera->SetFovY(sceneCameraBackupFovY_);
        }
        camera->SetClipRange(sceneCameraBackupNearClip_, sceneCameraBackupFarClip_);
        camera->UpdateProjectionMatrix();
        hasSceneCameraProjectionBackup_ = false;
    }
    activeSceneCameraBlendFov_ = false;
}

void CameraEditor::InvalidatePreviewForSceneChange() {
    CameraManager::GetInstance()->ResetCameraRegions();
    editRegionGizmo_ = false;
    regionGizmoBefore_ = nlohmann::json();
    // シーンをまたぐ Camera Object と描画基盤への参照をまとめて無効化する。
    selectedCameraObject_ = nullptr;
    hasRenderedCameraPreview_ = false;
    hasRenderedSceneCameraPreview_ = false;
    cameraPreviewPanelVisibleThisFrame_ = false;
    sceneCameraPreviewWindowVisibleThisFrame_ = false;
    lastCameraPreviewRenderTime_ = {};
    lastSceneCameraPreviewRenderTime_ = {};
    SetObject3DCommon(nullptr);
}
