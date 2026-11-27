#include <cstdint>
#include "MeshRenderer.h"
#include "DirectXCommon.h"
#include "ModelManager.h"
#include "CameraManager.h"
#include "LightManager.h"
#include "TextureManager.h"
#include "engine/graphics/core/ColorSpace.h"
#include "DebugConsole.h"
#include <cassert>
#include <SrvManager.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace engine {

namespace {

template <typename T>
bool CreateMappedBuffer(DirectXCommon* dxCommon, size_t size, Microsoft::WRL::ComPtr<ID3D12Resource>& resource, T*& data, const char* label) {
    // 作成・Mapの片方でも失敗した場合は両方を空へ戻し、不完全なGPU定数を使わない。
    data = nullptr;
    resource.Reset();

    // 描画基盤がなければ、出力を空に戻した状態で失敗を返す。
    if (!dxCommon) {
        return false;
    }

    resource = dxCommon->CreateBufferResource(size);

    // リソースを確保できなければ、バッファを識別できる名前とともに失敗を記録する。
    if (!resource) {
        DebugConsole::GetInstance()->AddLog(std::string("MeshRenderer buffer creation failed: ") + label);
        return false;
    }

    HRESULT hr = resource->Map(0, nullptr, reinterpret_cast<void**>(&data));

    // 確保済みでもマップに失敗するか書き込み先が得られなければ、両方の出力を空へ戻す。
    if (FAILED(hr) || !data) {
        DebugConsole::GetInstance()->AddLog(std::string("MeshRenderer buffer map failed: ") + label);
        resource.Reset();
        data = nullptr;
        return false;
    }

    return true;
}
}

// ========================================================================
// MeshRenderer 基本更新
// ------------------------------------------------------------------------
// GPUバッファ初期化、カメラ依存データ更新、通常更新を担当する。
// 材質設定、モデル、特殊マテリアル、影描画は別ファイルへ分けて見通しを保つ。
// ========================================================================
MeshRenderer::MeshRenderer(Transform* transform) {
    assert(transform);
    transform_ = transform;
}

void MeshRenderer::Initialize(engine::Object3DCommon* common) {
    assert(common);
    common_ = common;
    DirectXCommon* dxCommon = common_->GetDxCommon();

    // 通常描画用の WVP を最初に確保する。失敗した描画器は共通基盤の参照を外して使用対象から除く。
    if (!CreateMappedBuffer(dxCommon, sizeof(TransformationMatrix), wvpResource_, wvpData_, "WVP")) {
        common_ = nullptr;
        return;
    }

    wvpData_->wvp = engine::Math::MakeIdentity4x4();
    wvpData_->world = engine::Math::MakeIdentity4x4();

    // 通常のカメラ定数を初期化する。確保またはマッピングが失敗した段階で後続の作成を打ち切る。
    if (!CreateMappedBuffer(dxCommon, sizeof(CameraForGpu), cameraResource_, cameraData_, "Camera")) {
        common_ = nullptr;
        return;
    }

    *cameraData_ = {};

    // Camera Previewでは通常描画用のWVP/Camera定数を上書きしないよう、専用バッファを使います。
    // 右パネルPreviewと演出用Previewを同じフレームで描くため、GPU実行前の上書きを避ける目的で2面分確保します。
    for (std::int32_t i = 0; i < kPreviewBufferCount; ++i) {
        // このプレビュー枠の変換定数が確保できなければ、初期化は最後まで成立しない。
        if (!CreateMappedBuffer(dxCommon, sizeof(TransformationMatrix), previewWvpResources_[i], previewWvpData_[i], "PreviewWVP")) {
            common_ = nullptr;
            return;
        }

        previewWvpData_[i]->wvp = engine::Math::MakeIdentity4x4();
        previewWvpData_[i]->world = engine::Math::MakeIdentity4x4();
        previewWvpData_[i]->worldInverseTranspose = engine::Math::MakeIdentity4x4();

        // プレビューごとのカメラ定数も必須。取得できない枠を残したまま描画できる状態にはしない。
        if (!CreateMappedBuffer(dxCommon, sizeof(CameraForGpu), previewCameraResources_[i], previewCameraData_[i], "PreviewCamera")) {
            common_ = nullptr;
            return;
        }

        *previewCameraData_[i] = {};
    }

    // 共通の材質バッファを用意し、作れない場合は初期化を打ち切る。
    if (!CreateMappedBuffer(dxCommon, sizeof(MaterialData), materialResource_, materialData_, "Material")) {
        common_ = nullptr;
        return;
    }

    materialColor_ = { 1.0f, 1.0f, 1.0f, 1.0f };
    materialData_->color = engine::colorSpace::AuthoringToWorking(materialColor_);
    materialData_->enableLighting = 1;
    materialData_->uvTransform = engine::Math::MakeIdentity4x4();
    materialData_->selectedLighting = 2;
    materialData_->shininess = 20.0f;
    materialData_->materialType = 0;
    materialData_->roughness = 0.5f; // 程よくザラザラ（光沢が広がる）
    materialData_->metallic = 0.0f;  // 非金属（景色を反射しない）
    materialData_->enableNormalMap = 0;
    materialData_->enableEnvMap = 0;     // デフォルトoff
    materialData_->envIntensity = 1.0f;  // デフォルト1.0倍
    materialData_->emissive = 1.0f;
    materialData_->time = 0.0f;
    // ゲート通過で使う断面表示は最初は無効にし、後で有効化する場合の既定値を用意する。
    materialData_->portalClipEnabled = 0.0f;
    materialData_->portalClipProgress = 0.0f;
    materialData_->portalClipCenter = { 0.0f, 0.0f, 0.0f };
    materialData_->portalClipEdgeWidth = 0.12f;
    materialData_->portalClipNormal = { 0.0f, 0.0f, 1.0f };
    materialData_->portalClipDissolve = 0.18f;
    materialData_->portalClipColor = { 1.0f, 0.82f, 0.36f, 0.80f };

    // 影描画用の変換定数が確保できない場合は、描画器を有効にしない。
    if (!CreateMappedBuffer(dxCommon, sizeof(TransformationMatrix), shadowWvpResource_, shadowWvpData_, "ShadowWVP")) {
        common_ = nullptr;
        return;
    }

    shadowWvpData_->wvp = engine::Math::MakeIdentity4x4();
    shadowWvpData_->world = engine::Math::MakeIdentity4x4();
    shadowWvpData_->worldInverseTranspose = engine::Math::MakeIdentity4x4();

    // 局所フォグの定数バッファを用意できなければ、カメラ依存の更新に進めない。
    if (!CreateMappedBuffer(dxCommon, sizeof(LocalFogData), localFogResource_, localFogData_, "LocalFog")) {
        common_ = nullptr;
        return;
    }

    localFogData_->fogColor = { 0.2f, 0.8f, 0.5f, 1.0f }; // 毒沼カラー
    localFogData_->fogDensity = 0.5f;

    // 液面・特殊材質の共用バッファを作成できた場合に限り、その既定値を設定する。
    if (!CreateMappedBuffer(dxCommon, sizeof(WaterParamForGpu), waterParamResource_, waterParamData_, "WaterParam")) {
        common_ = nullptr;
        return;
    }

    // 液面アニメーション、特殊演出、ライトと逆射影へ渡す初期値を設定する。
    *waterParamData_ = {};
    waterParamData_->time = 0.0f;
    waterParamData_->waveSpeed = 2.0f;
    waterParamData_->waveHeight = 0.5f;
    waterParamData_->waveFrequency = 1.5f;
    waterParamData_->flowSpeedX = 0.1f; // 緩やかに流れる
    waterParamData_->flowSpeedY = 0.1f;
    waterParamData_->uvOffsetX = 0.0f;
    waterParamData_->uvOffsetY = 0.0f;
    waterParamData_->effectType = 0.0f;
    waterParamData_->effectScale = 1.0f;
    waterParamData_->effectSoftness = 0.55f;
    waterParamData_->effectIntensity = 1.0f;
    waterParamData_->cameraWorldPosition = { 0.0f, 0.0f, -1.0f };
    waterParamData_->billboardScale = 0.55f;
    waterParamData_->effectScaleX = 1.0f;
    waterParamData_->effectScaleY = 1.0f;
    waterParamData_->effectScaleZ = 1.0f;
    waterParamData_->topDownDetail = 1.0f;
    waterParamData_->waterLightDirection = { -0.35f, -0.82f, 0.45f };
    waterParamData_->waterLightIntensity = 1.0f;
    waterParamData_->waterLightColor = { 1.0f, 0.96f, 0.88f };
    waterParamData_->waterParamPadding1 = engine::colorSpace::WorkflowSettings::GetInstance().IsLinearWorkflowEnabled() ? 1.0f : 0.0f;
    waterParamData_->waterInverseViewProjection = engine::Math::MakeIdentity4x4();
    waterParamData_->waterDebugView = 0.0f;

    for (float& padding : waterParamData_->waterDebugPadding)
        padding = 0.0f;

    // 各プレビューへ水面の定数を複製し、通常画面の水面用カメラを上書きしないようにする。
    for (std::int32_t i = 0; i < kPreviewBufferCount; ++i) {
        // この枠専用の定数バッファが作成できなければ、プレビュー用の準備も成立しない。
        if (!CreateMappedBuffer(dxCommon, sizeof(WaterParamForGpu),
            previewWaterResources_[i], previewWaterData_[i], "PreviewWater")) {
            common_ = nullptr;
            return;
        }

        *previewWaterData_[i] = *waterParamData_;
    }
    
}

// 通常描画で最低限使う三つのリソースと書き込み先が揃っているか調べる。
bool MeshRenderer::HasRequiredBuffers() const {
    return wvpResource_ && wvpData_ &&
        cameraResource_ && cameraData_ &&
        materialResource_ && materialData_;
}

// 当たり判定に使う Transform は保ち、描画だけに重ねる拡縮・回転・位置差を保持する。
void MeshRenderer::SetVisualTransform(const engine::Vector3& scale, const engine::Vector3& rotation, const engine::Vector3& offset) {
    visualScale_ = scale;
    visualRotation_ = rotation;
    visualOffset_ = offset;
    worldInverseTransposeCacheValid_ = false;
}

// ローカル Y の支点からの高さに応じ、X・Z 方向だけに傾きを加える。
void MeshRenderer::SetVisualShear(const engine::Vector3& horizontalShear, float pivotY) {
    visualShear_ = { horizontalShear.x, 0.0f, horizontalShear.z };
    visualShearPivotY_ = pivotY;
    worldInverseTransposeCacheValid_ = false;
}

// 描画だけに重ねた姿勢と傾きを両方とも既定値へ戻す。
void MeshRenderer::ResetVisualTransform() {
    SetVisualTransform({ 1.0f, 1.0f, 1.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f });
    SetVisualShear({ 0.0f, 0.0f, 0.0f }, 0.0f);
}

engine::Matrix4x4 MeshRenderer::BuildRenderWorldMatrix() const {
    // 表示専用の傾きと姿勢をローカルで合成してから、通常のワールド変換へ重ねる。
    // Transform がなければ合成の基準がないので単位行列を返す。
    if (!transform_) {
        return engine::Math::MakeIdentity4x4();
    }

    engine::Matrix4x4 shearLocal = engine::Math::MakeIdentity4x4();
    shearLocal.m[1][0] = visualShear_.x;
    shearLocal.m[1][2] = visualShear_.z;
    // 高さに比例した横ずれから支点の高さ分を引き、胴体を傾けても支点の位置を動かしません。
    shearLocal.m[3][0] = -visualShearPivotY_ * visualShear_.x;
    shearLocal.m[3][2] = -visualShearPivotY_ * visualShear_.z;

    const engine::Matrix4x4 visualAffine = engine::Math::MakeAffineMatrix(visualScale_, visualRotation_, visualOffset_);
    engine::Matrix4x4 visualLocal = engine::Math::Multiply(shearLocal, visualAffine);
    // 演出側がまだ姿勢を保持している場合だけ表示行列へ重ねます。演出終了後は通常の表示補正だけに戻ります。
    if (const auto timelinePose = timelineVisualPose_.lock())
        visualLocal = engine::Math::Multiply(*timelinePose, visualLocal);
    return engine::Math::Multiply(visualLocal, transform_->matWorld);
}

const engine::Matrix4x4& MeshRenderer::GetCachedWorldInverseTranspose(const engine::Matrix4x4& worldMatrix) {
    // 法線変換行列はワールド行列が変わったフレームだけ計算し直す。
    if (!worldInverseTransposeCacheValid_ ||
        std::memcmp(&cachedWorldMatrix_, &worldMatrix, sizeof(engine::Matrix4x4)) != 0) {
        cachedWorldMatrix_ = worldMatrix;
        // 不均等な拡縮があっても面に対する法線の向きを保つため、位置用の行列とは別に逆転置を使います。
        cachedWorldInverseTranspose_ = engine::Math::Transpose(engine::Math::Inverse(worldMatrix));
        worldInverseTransposeCacheValid_ = true;
    }

    return cachedWorldInverseTranspose_;
}

void MeshRenderer::RefreshCameraDependentData() {
    // カメラ切り替え後はWVPだけでなく、水面・霧の逆ViewProjectionも同じ視点へ揃える。
    // 基盤・変換・材質・水面・霧・影の書き込み先が揃わなければ、途中まで更新しない。
    if (!common_ || !HasRequiredBuffers() || !shadowWvpData_ || !localFogData_ || !waterParamData_ || !transform_) {
        return;
    }

    engine::Math math;
    engine::Camera* camera = CameraManager::GetInstance()->GetActiveCamera();

    // カメラがある場合は、通常描画の変換に加え水面と霧のカメラ位置も揃える。
    if (camera) {
        const engine::Matrix4x4& viewProj = camera->GetViewProjectionMatrix();
        const engine::Matrix4x4 worldMatrix = BuildRenderWorldMatrix();

        wvpData_->wvp = math.Multiply(worldMatrix, viewProj);
        wvpData_->world = worldMatrix;
        wvpData_->worldInverseTranspose = GetCachedWorldInverseTranspose(worldMatrix);
        cameraData_->worldPosition = camera->GetEye();
        UpdateCameraVisibility(camera);
        waterParamData_->cameraWorldPosition = camera->GetEye();
        waterParamData_->waterInverseViewProjection = camera->GetInverseViewProjectionMatrix();
        localFogData_->cameraPos = camera->GetEye();
        localFogData_->inverseViewProj = camera->GetInverseViewProjectionMatrix();
    // カメラ不在時は、WVP とワールドの定数に単位行列を渡す。
    } else {
        wvpData_->wvp = engine::Math::MakeIdentity4x4();
        wvpData_->world = engine::Math::MakeIdentity4x4();
    }

    // UI プレビューではライトからのワールド影を求めず、影変換を単位行列でそろえる。
    if (isUiPreview_) {
        shadowWvpData_->wvp = engine::Math::MakeIdentity4x4();
        shadowWvpData_->world = engine::Math::MakeIdentity4x4();
        shadowWvpData_->worldInverseTranspose = engine::Math::MakeIdentity4x4();
        return;
    }

    const engine::Matrix4x4& lightVP =
        LightManager::GetInstance()->GetDirectionalShadowViewProjection(camera);
    const engine::Matrix4x4 shadowWorld = BuildRenderWorldMatrix();
    shadowWvpData_->wvp = math.Multiply(shadowWorld, lightVP);
    shadowWvpData_->world = shadowWorld;
    shadowWvpData_->worldInverseTranspose = GetCachedWorldInverseTranspose(shadowWorld);
}

// 水用の時刻と UV を秒単位で進める。通常の変換定数の更新からは独立している。
void MeshRenderer::AdvanceWaterAnimation(float deltaTime) {
    // 定数を書ける状態で、正かつ有限の経過秒だけを加える。
    if (!waterParamData_ || !std::isfinite(deltaTime) || deltaTime <= 0.0f)
        return;
    waterParamData_->time += deltaTime;
    // 毎秒のUV移動量に実経過秒を掛け、水面画像の流れを通常の行列更新頻度から分離します。
    waterParamData_->uvOffsetX += waterParamData_->flowSpeedX * deltaTime;
    waterParamData_->uvOffsetY += waterParamData_->flowSpeedY * deltaTime;
}

void MeshRenderer::Update() {
    // 描画と特殊材質に必要な定数を用意できていない場合は、時計や行列の更新を行わない。
    if (!common_ || !HasRequiredBuffers() || !shadowWvpData_ || !localFogData_ || !waterParamData_) {
        return;
    }

    // 通常材質とフォグの旧時計は更新1回を1/60秒とみなす。水面の実時間はAdvanceWaterAnimationで別に進める。
    time_ += 1.0f / 60.0f;

    // 材質データがある場合は、作業色空間へ変換した色を定数に渡す。
    if (materialData_) {
        // 草木の材質番号30・31は別の経路で実時間を進めるため、ここでは固定刻みの値で上書きしない。
        if (materialData_->materialType != 30 && materialData_->materialType != 31) {
            materialData_->time = time_;
        }

        materialData_->color = engine::colorSpace::AuthoringToWorking(materialColor_);
    }

    UpdateUvTransform();

    // 液面・特殊材質用の定数がある場合は、アニメーションと太陽光の値を更新する。
    if (waterParamData_) {
        // 材質番号8の水以外は、共用する定数の時刻と UV を従来どおり1/60秒刻みで進める。
        if (!materialData_ || materialData_->materialType != 8) {
            waterParamData_->time = time_;
            waterParamData_->uvOffsetX += waterParamData_->flowSpeedX * (1.0f / 60.0f);
            waterParamData_->uvOffsetY += waterParamData_->flowSpeedY * (1.0f / 60.0f);
        }

        auto& sun = LightManager::GetInstance()->GetDirectionalLight();
        waterParamData_->waterParamPadding1 = engine::colorSpace::WorkflowSettings::GetInstance().IsLinearWorkflowEnabled() ? 1.0f : 0.0f;
        const engine::Vector4 sunColor = engine::colorSpace::AuthoringToWorking(sun.color);
        waterParamData_->waterLightDirection = sun.direction;
        waterParamData_->waterLightIntensity = sun.intensity;
        waterParamData_->waterLightColor = { sunColor.x, sunColor.y, sunColor.z };
    }

    // 局所フォグの定数があれば、同じ固定刻みの時計と太陽光を反映する。
    if (localFogData_) {
        localFogData_->time = time_;
        auto& sun = LightManager::GetInstance()->GetDirectionalLight();
        localFogData_->lightDirection = sun.direction;

        // 光の色を作業色空間へ変換してから強度を掛け、霧のシェーダーへ渡す。
        const engine::Vector4 sunColor = engine::colorSpace::AuthoringToWorking(sun.color);
        localFogData_->lightColor = {
            sunColor.x * sun.intensity,
            sunColor.y * sun.intensity,
            sunColor.z * sun.intensity
        };

    }

    // 通常描画とカメラ切り替えで、描画・影・霧・水面の視点を同じ手順で揃える。
    RefreshCameraDependentData();
}


void MeshRenderer::UpdateCameraVisibility(engine::Camera* camera) {
    // 通常側と特殊材質側に書き込み先が揃っていなければ、遮蔽物の透過設定を更新しない。
    if (!cameraData_ || !waterParamData_) return;
    auto* manager = CameraManager::GetInstance();
    const auto& frame = manager->GetVisibilityFrame();
    const bool allowed = !isUiPreview_ && manager->CanFadeObject(visibilityOwner_, camera);
    // プレビューや透過対象外の視点では、前の視点で設定した透過量を解除する。
    if (!allowed) {
        visibilityAmount_ = 0.0f;
        // 透過を適用できないカメラへ切り替えた直後も、前の画面の透過量をGPUに残しません。
        cameraData_->visibilityParameters = {};
        waterParamData_->visibilityParameters = {};
        waterParamData_->foregroundBarrierCenter = {};
        waterParamData_->foregroundBarrierParameters = {};
        return;
    }
    // 新しい透過フレームで一度だけ、遮蔽物の候補を絞って透過量を進める。
    if (visibilitySerial_ != frame.serial) {
        // モデル境界で候補だけ絞る。最後の前後関係と円形範囲は画素ごとに判定する。
        const auto bounds = visibilityOwner_->GetModelWorldAabb();
        const engine::Vector3 center{frame.centerRadius.x, frame.centerRadius.y, frame.centerRadius.z};
        const auto eye = camera->GetEye();
        const auto delta = center - eye;
        const float length = engine::Math::Length(delta);
        bool intersects = false;
        // 視点と対象が十分に離れている場合は、視線とモデルの境界から遮蔽物の候補を求める。
        if (length > frame.parameters.w + 0.1f) {
            const auto direction = delta * (1.0f / length);
            const auto boxCenter = (bounds.min + bounds.max) * 0.5f;
            const float boxRadius = engine::Math::Length(bounds.max - bounds.min) * 0.5f;
            const float along = engine::Math::Dot(boxCenter - eye, direction);
            const float t = std::clamp(along, 0.0f, length);
            // プレイヤー位置を底とする透過範囲を視点からの距離で縮め、手前の候補ほど狭い範囲で判定します。
            const float radius = frame.centerRadius.w * t / length;
            intersects = along + boxRadius > 0.0f && along - boxRadius < length - frame.parameters.w &&
                engine::Math::Length(boxCenter - (eye + direction * t)) <= boxRadius + radius;
        }
        // 専用バリアは代理メッシュで描画するため、元モデルの境界では候補を棄却しない。
        if (GetMaterialType() == 22) intersects = true;
        visibilitySerial_ = frame.serial;
        const auto& settings = camera->GetVisibilitySettings();
        const float duration = intersects ? settings.fadeIn : settings.fadeOut;
        const float alpha = 1.0f - std::exp(-frame.deltaTime * 3.0f / duration);
        visibilityAmount_ += ((intersects ? 1.0f : 0.0f) - visibilityAmount_) * alpha;
    }
    cameraData_->visibilityCenterRadius = frame.centerRadius;
    cameraData_->visibilityParameters = frame.parameters;
    cameraData_->visibilityParameters.x *= visibilityAmount_;
    waterParamData_->visibilityCenterRadius = cameraData_->visibilityCenterRadius;
    waterParamData_->visibilityParameters = cameraData_->visibilityParameters;
    // 特殊バリアだけが参照する定数。通常区画では有効な重みが0になります。
    waterParamData_->foregroundBarrierCenter = frame.foregroundBarrierCenter;
    waterParamData_->foregroundBarrierCenter.w *= visibilityAmount_;
    waterParamData_->foregroundBarrierParameters = frame.foregroundBarrierParameters;
}

} // namespace engine
