#include <cstdint>
#include "engine/editor/graphics/camera/CameraEditor.Internal.h"
#include "engine/editor/core/DebugEditor.h"

using namespace cameraEditorImplementation;

// 編集カメラを通して、周回の中心または開始視点をワールド軸で移動する。
void CameraEditor::DrawOrbitCenterGizmo(const engine::Vector2& gameViewOffset, const engine::Vector2& gameViewSize, bool snapEnabled, float snapValue) {
#ifdef USE_IMGUI
    // ツール一覧でギズモを非表示にしている間は入力を受けない。
    if (!EditorToolSettings::Get().IsVisible("orbitGizmo"))
        return;
    // エディター視点のときだけ周回設定を操作できる。
    if (settings_.currentMode != Mode::kEditor)
        return;
    // 本編の追従方式が周回でない場合は、このギズモを出さない。
    if (settings_.gameFollowMode != engine::Camera::FollowMode::kOrbit)
        return;
    // 軌道ガイドと中心ギズモの両方が有効であることを要求する。
    if (!settings_.orbitGuideVisible || !settings_.orbitCenterGizmoVisible)
        return;
    // 描画領域が未確定または小さすぎる場合は、投影と入力座標を作らない。
    if (gameViewSize.x <= 1.0f || gameViewSize.y <= 1.0f)
        return;
    // 新しい操作は画像の中から始める。ドラッグ中はポインターが画像の外へ出ても終了まで操作を見届ける。
    if (!isGameViewHovered_ && !isDraggingOrbitCenterGizmo_)
        return;

    engine::Camera* camera = CameraManager::GetInstance()->GetActiveCamera();
    // 操作用ビューのカメラがなければ行列を取得できない。
    if (!camera)
        return;

    engine::Math math;
    orbitEditGizmoTarget_ = (std::clamp)(orbitEditGizmoTarget_, 0, 1);
    const bool editStartEye = orbitEditGizmoTarget_ == 1;

    // 同じギズモを中心位置と開始視点で使い回し、編集先だけ切り替える。
    engine::Vector3 gizmoPosition = editStartEye ? GetOrbitStartEye() : GetOrbitCenter();
    engine::Matrix4x4 world = math.MakeTranslateMatrix(gizmoPosition);
    engine::Matrix4x4 view = camera->GetViewMatrix();
    engine::Matrix4x4 projection = camera->GetProjectionMatrix();
    // ギズモはワールド軸上を動くので、吸着幅も三軸共通のワールド距離を使う。
    float snap[3] = { snapValue, snapValue, snapValue };

    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(gameViewOffset.x, gameViewOffset.y, gameViewSize.x, gameViewSize.y);
    ImGuizmo::Manipulate(
        &view.m[0][0],
        &projection.m[0][0],
        ImGuizmo::TRANSLATE,
        ImGuizmo::WORLD,
        &world.m[0][0],
        nullptr,
        (snapEnabled && snapValue > 0.0f) ? snap : nullptr);

    if (ImGuizmo::IsUsing()) {
        // ドラッグ中はカメラへ即時反映し、操作を離した時だけ設定を保存する。
        isDraggingOrbitCenterGizmo_ = true;
        // ギズモが編集した行列の平行移動成分を取り出し、開始視点または軌道中心の設定へ変換する。
        engine::Vector3 editedCenter = { world.m[3][0], world.m[3][1], world.m[3][2] };

        // 開始視点を編集中の場合は、新しい視点から周回半径と高さなどを再計算する。
        if (editStartEye) {
            SetOrbitStartFromWorld(editedCenter);
        // 中心を編集中の場合は、開始視点ではなく周回の基準点を更新する。
        } else {
            SetOrbitCenterFromWorld(editedCenter);
        }

        camera->SetOrbitCenterOffset(settings_.orbitCenterOffset);
        camera->SetOrbitCenterHeight(settings_.orbitCenterHeight);
        camera->SetOrbitParams(settings_.orbitRadius, settings_.orbitHeight, settings_.orbitSpeed);
    }
    // 前のフレームまでドラッグしていた場合は、解放時に一度だけ設定を保存する。
    else if (isDraggingOrbitCenterGizmo_) {
        isDraggingOrbitCenterGizmo_ = false;
        SaveSettings();
        DebugConsole::GetInstance()->AddLog("Orbit camera center gizmo saved.");
    }
#else
    (void)gameViewOffset;
    (void)gameViewSize;
    (void)snapEnabled;
    (void)snapValue;
#endif
}

// 本編・配置カメラの視点、視錐台、領域を、残りプリミティブ枠の範囲でワールドに描く。
void CameraEditor::DrawCameraGuide(PrimitiveDrawer& primitiveDrawer, ID3D12GraphicsCommandList* commandList, std::int32_t& instanceCount, std::int32_t maxDrawLimit) {
#ifdef USE_IMGUI
    // ツール一覧でカメラガイドを非表示にしている場合は描かない。
    if (!EditorToolSettings::Get().IsVisible("cameraGuide"))
        return;
    // カメラ設定側でガイドを無効にした場合も表示しない。
    if (!settings_.cameraGuideVisible)
        return;
    const bool isCameraEditorSelected =
        EditorManager::GetInstance()->GetSelectedObject() == this;
    // カメラエディターか配置カメラを選んでいる場合だけ、編集用ガイドを出す。
    if (!isCameraEditorSelected && GetSelectedCameraObject() == nullptr)
        return;
    // 他ガイドで描画枠を使い切っている場合は、追加の頂点を準備しない。
    if (instanceCount >= maxDrawLimit)
        return;

    engine::Math math;
    const float markerSize = (std::max)(0.05f, settings_.cameraGuideSize);
    const float frustumLength = (std::max)(0.5f, settings_.cameraFrustumLength);

    // 各描画ヘルパーで共通の上限を確認し、Editorガイドがプリミティブ枠を超えないようにする。
    auto drawSphere = [&](const engine::Vector3& pos, float size, const engine::Vector4& color) {
        // 球の描画に使うインスタンスが上限に達した場合は省く。
        if (instanceCount >= maxDrawLimit)
            return;
        engine::Matrix4x4 world = math.Multiply(math.MakeScaleMatrix({ size, size, size }), math.MakeTranslateMatrix(pos));
        primitiveDrawer.DrawWireSphere(commandList, world, color, instanceCount++);
    };

    auto drawCubeLine = [&](const engine::Vector3& start, const engine::Vector3& end, float thickness, const engine::Vector4& color) {
        // 線用の箱にも同じ上限を適用し、使用枠を一つずつ増やす。
        if (instanceCount >= maxDrawLimit)
            return;
        primitiveDrawer.DrawWireCube(commandList, MakeLineBoxMatrix(start, end, thickness), color, instanceCount++);
    };

    auto drawWireBox = [&](const engine::Vector3& center, const engine::Vector3& right, const engine::Vector3& up, const engine::Vector3& forward,
        float halfRight, float halfUp, float halfForward, float thickness, const engine::Vector4& color) {
        // 辺を構成するための枠が残っていなければ、箱全体の準備を省く。
        if (instanceCount >= maxDrawLimit)
            return;

        // カメラ基底から八隅を求め、12本の辺だけをワイヤーとして描く。
        auto corner = [&](float sx, float sy, float sz) {
            return center + right * (halfRight * sx) + up * (halfUp * sy) + forward * (halfForward * sz);
        };

        const engine::Vector3 lbf = corner(-1.0f, -1.0f, 1.0f);
        const engine::Vector3 rbf = corner(1.0f, -1.0f, 1.0f);
        const engine::Vector3 ltf = corner(-1.0f, 1.0f, 1.0f);
        const engine::Vector3 rtf = corner(1.0f, 1.0f, 1.0f);
        const engine::Vector3 lbb = corner(-1.0f, -1.0f, -1.0f);
        const engine::Vector3 rbb = corner(1.0f, -1.0f, -1.0f);
        const engine::Vector3 ltb = corner(-1.0f, 1.0f, -1.0f);
        const engine::Vector3 rtb = corner(1.0f, 1.0f, -1.0f);

        drawCubeLine(lbf, rbf, thickness, color);
        drawCubeLine(rbf, rtf, thickness, color);
        drawCubeLine(rtf, ltf, thickness, color);
        drawCubeLine(ltf, lbf, thickness, color);

        drawCubeLine(lbb, rbb, thickness, color);
        drawCubeLine(rbb, rtb, thickness, color);
        drawCubeLine(rtb, ltb, thickness, color);
        drawCubeLine(ltb, lbb, thickness, color);

        drawCubeLine(lbf, lbb, thickness, color);
        drawCubeLine(rbf, rbb, thickness, color);
        drawCubeLine(ltf, ltb, thickness, color);
        drawCubeLine(rtf, rtb, thickness, color);
    };

    auto drawCameraBody = [&](const engine::Vector3& eye, const engine::Vector3& forward, float sizeScale, const engine::Vector4& bodyColor, const engine::Vector4& accentColor) {
        // 本体表示が無効な場合かプリミティブの枠が残っていない場合は、筐体を描かない。
        if (!settings_.cameraBodyVisible || instanceCount >= maxDrawLimit)
            return;
        // 実モデルを描ける場合は別経路がカメラ本体を描く。ワイヤーの筐体は共通描画設定がないときだけ使う。
        if (object3dCommon_)
            return;

        const engine::Vector3 safeForward = NormalizeSafe(forward, { 0.0f, 0.0f, 1.0f });
        const engine::Vector3 right = GetConfiguredCameraRight(safeForward);
        const engine::Vector3 up = GetConfiguredCameraUp(safeForward, right);
        const float size = markerSize * sizeScale;
        const float line = (std::max)(0.018f, size * 0.045f);

        // 筐体・レンズ・取っ手・三脚を、カメラのローカル基底に沿ったワイヤー形状で描く。
        const engine::Vector3 bodyCenter = eye - safeForward * (size * 0.16f);
        drawWireBox(bodyCenter, right, up, safeForward, size * 0.82f, size * 0.48f, size * 0.42f, line, bodyColor);

        const engine::Vector3 lensCenter = eye + safeForward * (size * 0.45f);
        drawWireBox(lensCenter, right, up, safeForward, size * 0.36f, size * 0.28f, size * 0.24f, line * 0.9f, accentColor);

        const engine::Vector3 hoodCenter = eye + safeForward * (size * 0.78f);
        drawWireBox(hoodCenter, right, up, safeForward, size * 0.52f, size * 0.38f, size * 0.11f, line * 0.8f, accentColor);

        const engine::Vector3 handleCenter = bodyCenter + up * (size * 0.66f) - safeForward * (size * 0.05f);
        drawWireBox(handleCenter, right, up, safeForward, size * 0.42f, size * 0.13f, size * 0.22f, line * 0.75f, bodyColor);

        const engine::Vector3 tripodRoot = bodyCenter - up * (size * 0.52f);
        const engine::Vector3 tripodCenter = tripodRoot - up * (size * 0.55f);
        drawCubeLine(tripodRoot, tripodCenter, line * 0.75f, bodyColor);
        drawCubeLine(tripodCenter, tripodCenter + right * (size * 0.58f) - up * (size * 0.22f), line * 0.65f, bodyColor);
        drawCubeLine(tripodCenter, tripodCenter - right * (size * 0.58f) - up * (size * 0.22f), line * 0.65f, bodyColor);
        drawCubeLine(tripodCenter, tripodCenter - safeForward * (size * 0.50f) - up * (size * 0.18f), line * 0.65f, bodyColor);
    };

    auto drawCameraBadge = [&](const engine::Vector3& eye, const engine::Vector3& forward, std::int32_t badgeNumber, float sizeScale, const engine::Vector4& color, bool selected) {
        // 描画枠を使い切っている場合は番号タグを省く。
        if (instanceCount >= maxDrawLimit)
            return;

        const engine::Vector3 safeForward = NormalizeSafe(forward, { 0.0f, 0.0f, 1.0f });
        const engine::Vector3 right = GetConfiguredCameraRight(safeForward);
        const engine::Vector3 up = GetConfiguredCameraUp(safeForward, right);
        const float size = markerSize * sizeScale;
        const float line = (std::max)(0.014f, size * 0.035f);
        const engine::Vector3 tagCenter = eye + up * (selected ? size * 1.45f : size * 1.12f) - safeForward * (size * 0.28f);

        // リスト上の番号とScene上のカメラ位置を対応させるため、小さなタグを描きます。
        drawWireBox(tagCenter, right, up, safeForward, size * 0.44f, size * 0.20f, size * 0.06f, line, color);

        // 番号を持たない本編ビューは、点の列の代わりに線の記号を出す。
        if (badgeNumber <= 0) {
            drawCubeLine(tagCenter - right * (size * 0.22f), tagCenter + right * (size * 0.22f), line * 1.2f, color);
            drawCubeLine(tagCenter, tagCenter + up * (size * 0.28f), line * 0.9f, color);
            return;
        }

        const std::int32_t dotCount = (std::min)(badgeNumber, 5);
        const float start = -0.24f;
        const float step = dotCount > 1 ? 0.12f : 0.0f;

        for (std::int32_t i = 0; i < dotCount; ++i) {
            const engine::Vector3 dotPos = tagCenter + right * (size * (start + step * i));
            drawSphere(dotPos, size * (selected ? 0.075f : 0.060f), color);
        }

        // 5を超える番号は点を5個までに抑え、上限を超えたことを短い斜線で示す。
        if (badgeNumber > 5) {
            drawCubeLine(tagCenter + right * (size * 0.28f) - up * (size * 0.08f), tagCenter + right * (size * 0.34f) + up * (size * 0.08f), line * 0.75f, color);
        }
    };

    auto drawFrustum = [&](const engine::Vector3& eye, const engine::Vector3& forward, float fovY, float sizeScale, const engine::Vector4& eyeColor, const engine::Vector4& rayColor, const engine::Vector4& frameColor) {
        // 視錐台に割り当てられる描画枠が無い場合は、四隅の計算を省く。
        if (instanceCount >= maxDrawLimit)
            return;

        const engine::Vector3 safeForward = NormalizeSafe(forward, { 0.0f, 0.0f, 1.0f });
        const engine::Vector3 right = GetConfiguredCameraRight(safeForward);
        const engine::Vector3 up = GetConfiguredCameraUp(safeForward, right);
        const engine::Vector3 target = eye + safeForward * frustumLength;
        const float safeFovY = std::clamp(fovY, DegToRad(1.0f), DegToRad(179.0f));
        // 縦画角の半分と前方距離から、遠方の断面の半高を求める。tanの入力はラジアン。
        const float halfHeight = frustumLength * std::tan(safeFovY * 0.5f);

        // 縦FOVと固定の16:9比から、視線先のフラスタム四隅を復元する。
        const float halfWidth = halfHeight * (16.0f / 9.0f);
        const float localMarkerSize = markerSize * sizeScale;
        const engine::Vector3 topLeft = target + up * halfHeight - right * halfWidth;
        const engine::Vector3 topRight = target + up * halfHeight + right * halfWidth;
        const engine::Vector3 bottomLeft = target - up * halfHeight - right * halfWidth;
        const engine::Vector3 bottomRight = target - up * halfHeight + right * halfWidth;

        drawCameraBody(eye, safeForward, sizeScale, eyeColor, frameColor);
        drawSphere(eye, settings_.cameraBodyVisible ? localMarkerSize * 0.24f : localMarkerSize, eyeColor);
        drawSphere(target, localMarkerSize * 0.45f, frameColor);
        drawCubeLine(eye, target, 0.035f * sizeScale, rayColor);
        drawCubeLine(eye, topLeft, 0.025f * sizeScale, frameColor);
        drawCubeLine(eye, topRight, 0.025f * sizeScale, frameColor);
        drawCubeLine(eye, bottomLeft, 0.025f * sizeScale, frameColor);
        drawCubeLine(eye, bottomRight, 0.025f * sizeScale, frameColor);
        drawCubeLine(topLeft, topRight, 0.03f * sizeScale, frameColor);
        drawCubeLine(topRight, bottomRight, 0.03f * sizeScale, frameColor);
        drawCubeLine(bottomRight, bottomLeft, 0.03f * sizeScale, frameColor);
        drawCubeLine(bottomLeft, topLeft, 0.03f * sizeScale, frameColor);
    };

    // プレイ中の表示ルールで本編ガイドが隠されていない場合、設定から固定のガイドを描く。
    if (!ShouldHideGameCameraGuideDuringPlay()) {
        // ゲーム/3人称カメラの位置をScene上に表示します。
        const engine::Vector3 gameEye = GetConfiguredCameraEye();
        const engine::Vector3 gameForward = GetConfiguredCameraForward();
        const engine::Vector4 gameEyeColor = { 1.0f, 0.82f, 0.18f, 1.0f };
        const engine::Vector4 gameRayColor = { 1.0f, 0.72f, 0.18f, 0.95f };
        const engine::Vector4 gameFrameColor = { 1.0f, 0.92f, 0.35f, 0.88f };
        drawFrustum(gameEye, gameForward, 0.45f, 0.85f, gameEyeColor, gameRayColor, gameFrameColor);
        drawCameraBadge(gameEye, gameForward, 0, 0.85f, gameFrameColor, true);

        // 追従対象のプレイヤーがいる場合は、高さ補正後の注視点と視線も示す。
        if (targetPlayer_) {
            engine::Vector3 lookTarget = targetPlayer_->GetWorldPosition();
            lookTarget.y += settings_.height;
            drawSphere(lookTarget, markerSize * 0.38f, gameFrameColor);
            drawCubeLine(gameEye, lookTarget, 0.024f, gameRayColor);
        }
    }

    // 選択中の範囲を先に描き、プリミティブ上限に達しても編集中の範囲を優先する。
    auto* selectedCamera = GetSelectedCameraObject();
    // 本編の再生中は編集用のカメラ範囲を重ねない。
    if (!SceneManager::GetInstance()->IsPlaying()) {
        auto drawRegion = [&](const engine::Object3D* object, bool selected) {
            // カメラが存在し、配置カメラ型で、表示中の場合だけ範囲を評価する。
            if (!object || !object->IsCameraObject() || !object->GetIsVisible()) return;
            const auto& region = object->GetSceneCameraSettings().region;
            // 範囲が無効なら境界箱や経路を描かない。
            if (!region.enabled) return;

            engine::Vector4 color = region.color;
            const bool dim = selectedCamera && !selected;
            // 別のカメラを編集中の場合は、非選択の範囲の不透明度を落とす。
            if (dim) color.w *= 0.35f;
            drawWireBox(region.center, {1,0,0}, {0,1,0}, {0,0,1},
                region.size.x * 0.5f, region.size.y * 0.5f, region.size.z * 0.5f,
                selected ? 0.075f : (dim ? 0.02f : 0.035f), color);

            // 選択した範囲のみ、箱の上辺中央とカメラの位置を線で結ぶ。
            if (selected) {
                const auto labelAnchor = region.center + engine::Vector3{0, region.size.y * 0.5f, 0};
                drawCubeLine(labelAnchor, object->GetWorldPosition(), 0.045f, color);
            }
            // 経路は地面の範囲枠と重ならないよう 1.5 ワールド単位上に描き、終点を箱で示す。
            if (region.routeEnabled) {
                const auto lift = engine::Vector3{0,1.5f,0};
                drawCubeLine(region.pathStart+lift, region.pathEnd+lift, dim ? .04f : .08f, color);
                drawWireBox(region.pathEnd+lift,{1,0,0},{0,1,0},{0,0,1},.3f,.3f,.3f,.06f,color);
            }
        };
        drawRegion(selectedCamera, true);
        // 編集対象のシーンを取得できれば、選択外の範囲も薄く表示する。
        if (const auto* scene = GetCurrentScene()) {
            for (const auto& object : scene->GetObjects()) {
                // 選択中の範囲は先に描画済みなので、同じ個体を二度描かない。
                if (object.get() != selectedCamera) drawRegion(object.get(), false);
            }
        }
    }

    // Camera Object選択中は、そのObject自身のFOV・位置・注視先でCamera Editorと同じガイドを描きます。
    if (engine::Object3D* cameraObject = selectedCamera) {
        // Scene内カメラは保存されたFOVと姿勢を使い、ゲームカメラとは色を分ける。
        engine::Vector3 eye{};
        engine::Vector3 target{};

        // 視点と注視点を解決できる場合に、選択カメラの構図を専用色で描く。
        if (ResolveSceneCameraPose(cameraObject, eye, target)) {
            const SceneCameraSettings& cameraSettings = cameraObject->GetSceneCameraSettings();
            const engine::Vector3 forward = NormalizeSafe(target - eye, GetObjectWorldForward(cameraObject));
            const engine::Vector4 eyeColor = { 0.2f, 1.0f, 0.35f, 1.0f };
            const engine::Vector4 rayColor = { 0.2f, 1.0f, 0.35f, 0.95f };
            const engine::Vector4 frameColor = { 0.75f, 1.0f, 0.35f, 0.95f };

            drawFrustum(eye, forward, cameraSettings.fovY, 0.95f, eyeColor, rayColor, frameColor);
            drawCameraBadge(eye, forward, 1, 0.95f, frameColor, true);
            drawSphere(target, markerSize * 0.32f, frameColor);
            drawCubeLine(eye, target, 0.032f, rayColor);
        }
    }

#else
    (void)primitiveDrawer;
    (void)commandList;
    (void)instanceCount;
    (void)maxDrawLimit;
#endif
}

// 通常のカメラガイドに、周回軌道・中心・開始視点・現在位置を追加する。
void CameraEditor::DrawOrbitGuide(PrimitiveDrawer& primitiveDrawer, ID3D12GraphicsCommandList* commandList, std::int32_t& instanceCount, std::int32_t maxDrawLimit) {
#ifdef USE_IMGUI
    DrawCameraGuide(primitiveDrawer, commandList, instanceCount, maxDrawLimit);
    // 周回ガイドのツールが非表示なら、通常ガイドの描画だけで終える。
    if (!EditorToolSettings::Get().IsVisible("orbitGuide"))
        return;
    // ワールドを編集しているカメラモードの場合だけ軌道を追加する。
    if (settings_.currentMode != Mode::kEditor)
        return;
    // 周回側の可視設定が無効なら軌道を描かない。
    if (!settings_.orbitGuideVisible)
        return;
    // 本編の追従方式が周回の場合に限り、周回半径を可視化する。
    if (settings_.gameFollowMode != engine::Camera::FollowMode::kOrbit)
        return;
    // 通常ガイドで共通の描画枠を使い切った場合は追加描画を省く。
    if (instanceCount >= maxDrawLimit)
        return;

    engine::Math math;
    const engine::Vector3 center = GetOrbitCenter();
    const float radius = settings_.orbitRadius;
    const float markerSize = settings_.orbitGuideMarkerSize;
    // 保存設定の度をラジアンへ換算し、水平リング上の開始位置を求める。
    const float startAngle = DegToRad(settings_.orbitStartAngleDeg);

    auto drawSphere = [&](const engine::Vector3& pos, float size, const engine::Vector4& color) {
        // マーカー用のプリミティブ枠が残っている場合だけ球を描く。
        if (instanceCount >= maxDrawLimit)
            return;
        engine::Matrix4x4 world = math.Multiply(math.MakeScaleMatrix({ size, size, size }), math.MakeTranslateMatrix(pos));
        primitiveDrawer.DrawWireSphere(commandList, world, color, instanceCount++);
        };

    auto drawCubeLine = [&](const engine::Vector3& start, const engine::Vector3& end, float thickness, const engine::Vector4& color) {
        // 線の描画でも、通常ガイドと共有するインスタンス上限を守る。
        if (instanceCount >= maxDrawLimit)
            return;
        primitiveDrawer.DrawWireCube(commandList, MakeLineBoxMatrix(start, end, thickness), color, instanceCount++);
        };

    // 半径が0.01を超え、描画枠が残っている場合に水平リングを作る。
    if (radius > 0.01f && instanceCount < maxDrawLimit) {
        // Orbit中心と半径を水平リングにし、開始位置は別の線と点で示す。
        engine::Matrix4x4 ringWorld = math.Multiply(
            math.MakeScaleMatrix({ radius * 2.0f, 0.04f, radius * 2.0f }),
            math.MakeTranslateMatrix(center));
        primitiveDrawer.DrawWireCylinder(commandList, ringWorld, { 0.1f, 0.8f, 1.0f, 0.85f }, instanceCount++);
    }

    engine::Vector3 startGround = {
        center.x + radius * std::cos(startAngle),
        center.y,
        center.z + radius * std::sin(startAngle)
    };

    // リング上の点は軌道の高さを含まない。そこへ設定の高さを足した位置が視点の開始位置。
    engine::Vector3 startEye = startGround;
    startEye.y += settings_.orbitHeight;

    drawSphere(center, markerSize, { 1.0f, 0.9f, 0.1f, 1.0f });
    drawSphere(startEye, markerSize, { 1.0f, 0.15f, 0.15f, 1.0f });
    drawCubeLine(center, startGround, 0.04f, { 0.1f, 0.9f, 1.0f, 0.9f });
    drawCubeLine(startGround, startEye, 0.04f, { 0.3f, 1.0f, 0.2f, 0.9f });

    engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();

    // メインカメラが存在すれば、設定上の開始点と区別して現在位置も表示する。
    if (camera) {
        drawSphere(camera->GetEye(), markerSize * 0.75f, { 0.75f, 0.35f, 1.0f, 1.0f });
    }
#else
    (void)primitiveDrawer;
    (void)commandList;
    (void)instanceCount;
    (void)maxDrawLimit;
#endif
}


// 選択中の適用範囲を移動・拡縮する。通常ギズモやクリック選択を抑えるフレームでは true。
bool CameraEditor::DrawRegionGizmo(const engine::Vector2& offset, const engine::Vector2& size,
    bool snapEnabled, float snap, DebugEditor* editor) {
#ifdef USE_IMGUI
    auto* object = GetSelectedCameraObject();
    // 停止した編集画面で、範囲が有効な未ロックの選択物と描画領域が揃う場合だけ操作できる。
    if (!editRegionGizmo_ || !object || !editor || editor->GetSelectedObject() != object ||
        !object->GetSceneCameraSettings().region.enabled || object->GetIsLocked() ||
        SceneManager::GetInstance()->IsPlaying() || !IsEditorMode() || size.x <= 1.0f || size.y <= 1.0f) {
        regionGizmoBefore_ = nullptr;
        return false;
    }
    // 選択を変えたときはUndoの開始状態も入れ替え、別のカメラに古い範囲を適用しない。
    if (regionGizmoObjectId_ != object->GetPersistentGuid()) {
        regionGizmoBefore_ = nullptr; regionGizmoObjectId_ = object->GetPersistentGuid();
    }
    auto* camera = CameraManager::GetInstance()->GetActiveCamera();
    // 変換用のカメラを取得できなければ、入力を占有しない。
    if (!camera) return false;
    auto& region = object->GetSceneCameraSettings().region;
    engine::Math math;
    // 適用範囲はワールド軸に平行。行列の拡大で各辺の長さ、平行移動で箱の中心を渡す。
    auto world = math.MakeScaleMatrix(region.size) * math.MakeTranslateMatrix(region.center);
    auto view = camera->GetViewMatrix(); auto projection = camera->GetProjectionMatrix();
    float snapValues[] = {snap, snap, snap};
    const auto before = editor->CaptureObjectState(object);
    ImGuizmo::SetDrawlist(); ImGuizmo::SetRect(offset.x, offset.y, size.x, size.y);
    ImGuizmo::PushID(28451);
    const bool modified = ImGuizmo::Manipulate(&view.m[0][0], &projection.m[0][0],
        regionGizmoScale_ ? ImGuizmo::SCALE : ImGuizmo::TRANSLATE, ImGuizmo::WORLD, &world.m[0][0],
        nullptr, snapEnabled && snap > 0.0f ? snapValues : nullptr);
    const bool usingGizmo = ImGuizmo::IsUsing();
    ImGuizmo::PopID();
    // 離したフレームでもManipulateが最終値を返すため、IsUsingだけで変更を捨てない。
    if (usingGizmo || modified) {
        // ドラッグの開始時だけ、変更前の状態を後の Undo 用に退避する。
        if (!regionGizmoBefore_.is_object()) regionGizmoBefore_ = before;
        region.center = {world.m[3][0], world.m[3][1], world.m[3][2]};
        // 対角成分の絶対値を辺の長さにし、反転やゼロ幅でも範囲の各軸を0.1以上に保つ。
        region.size = {(std::max)(0.1f, std::abs(world.m[0][0])),
            (std::max)(0.1f, std::abs(world.m[1][1])), (std::max)(0.1f, std::abs(world.m[2][2]))};
        editor->MarkDirtyForObject(object);
    }
    // ドラッグ中の全フレームを個別に記録せず、開始時の状態と確定結果を一回の履歴にする。
    if (!usingGizmo && regionGizmoBefore_.is_object()) {
        editor->RegisterObjectEdited(object, regionGizmoBefore_, "カメラ適用範囲をギズモ編集");
        regionGizmoBefore_ = nullptr;
    }
    // 編集中の領域を通常の Transform ギズモやクリック選択で上書きしないよう、そのフレームの入力を占有したと返す。
    return true;
#else
    (void)offset; (void)size; (void)snapEnabled; (void)snap; (void)editor;
    return false;
#endif
}
