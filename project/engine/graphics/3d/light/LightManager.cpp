#include <cstdint>
#include "LightManager.h"
#include "engine/graphics/3d/light/SceneLighting.h"
#include "Easing.h"
#include "Object3D.h" // 追従対象の座標を取得するために必要
#include "Camera.h"
#include "CameraManager.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <cmath>      // sin, cos
#include <algorithm>
#include "json.hpp"
#include "TextureManager.h"
#include "engine/graphics/core/ColorSpace.h"
#include "PostEffect.h"

namespace engine {

using json = nlohmann::json;

SceneLighting SceneLighting::Capture() {
    auto& lights = *LightManager::GetInstance();
    return SceneLighting(lights.GetPointLightResource(), lights.GetSpotLightResource());
}

namespace {
// 球が視錐台の全平面の外側にないことを確認し、ライトの描画対象を絞ります。
bool IntersectsFrustumSphere(const engine::Frustum& frustum, const engine::Vector3& center, float radius) {
    // いずれかの視錐台面より球が完全に外なら描画対象から除く。
    const float safeRadius = (std::max)(radius, 0.0f);

    for (const engine::Plane& plane : frustum.planes) {
        // 各平面からの符号付き距離が半径より外なら、光が届く球全体が視界の外にあると判断します。
        const float signedDistance =
            plane.normal.x * center.x +
            plane.normal.y * center.y +
            plane.normal.z * center.z +
            plane.distance;
        // 球の外周も平面の外にあれば、視錐台とは交差しません。
        if (signedDistance < -safeRadius) {
            return false;
        }
    }

    return true;
}
}

LightManager* LightManager::GetInstance() {
    // ライト設定とGPU転送の状態を全シーンで一つに保ちます。
    static LightManager sInstance;
    return &sInstance;
}

void LightManager::Initialize(DirectXCommon* dxCommon) {
    assert(dxCommon);
    dxCommon_ = dxCommon;
    uint32_t envHandle = engine::TextureManager::GetInstance()->Load(
        "Resources/output_skybox.dds",
        engine::TextureManager::TextureColorSpace::kSRGB);
    LightManager::GetInstance()->SetEnvironmentMapHandle(envHandle);
    skyboxTexturePath_ = "Resources/output_skybox.dds";
    skyboxTextureHandle_ = envHandle;

    // --- バッファ作成 (サイズはConstData構造体に合わせる) ---
    pointLightResource_ = dxCommon_->CreateBufferResource(sizeof(PointLightConstData));
    pointLightResource_->Map(0, nullptr, reinterpret_cast<void**>(&pointLightConstData_));
    pointLightConstData_->activeCount = 0;

    spotLightResource_ = dxCommon_->CreateBufferResource(sizeof(SpotLightConstData));
    spotLightResource_->Map(0, nullptr, reinterpret_cast<void**>(&spotLightConstData_));
    spotLightConstData_->activeCount = 0;

    directionalLightResource_ = dxCommon_->CreateBufferResource(sizeof(DirectionalLight));

    // 平行光源の初期設定
    directionalLightData_.color = { 1.0f, 0.96f, 0.88f, 1.0f };
    directionalLightData_.direction = { -0.35f, -0.82f, 0.45f };
    directionalLightData_.intensity = 0.86f;
    directionalLightData_.ambientColor = { 0.34f, 0.38f, 0.42f };
    directionalLightData_.fogStart = 10.0f;
    directionalLightData_.fogEnd = 1000.0f;
    directionalLightData_.fogColor = { 0.66f, 0.76f, 0.86f };
    sceneClearColor_ = { 0.52f, 0.68f, 0.84f, 1.0f };
    ApplySceneClearColor();

    pointLights_.clear();
    spotLights_.clear();
}

void LightManager::Update() {

    engine::Camera* activeCamera = CameraManager::GetInstance()->GetActiveCamera();
    const engine::Frustum* activeFrustum = activeCamera ? &activeCamera->GetFrustum() : nullptr;

    // 平行光の影行列を更新してから、GPU用の定数バッファに反映します。
    GetDirectionalShadowViewProjection(CameraManager::GetInstance()->GetActiveCamera());

    // 平行光の定数バッファが確保されている場合だけ書き込みを試みます。
    if (directionalLightResource_) {
        DirectionalLight* dirMap = nullptr;

        // Map が成功し、書き込み先を取得できた場合に転送します。
        if (SUCCEEDED(directionalLightResource_->Map(0, nullptr, reinterpret_cast<void**>(&dirMap))) && dirMap) {

            // 方向に十分な長さがあれば単位ベクトルにします。
            if (engine::Math::Length(directionalLightData_.direction) > 0.0001f) {
                directionalLightData_.direction = engine::Math::Normalize(directionalLightData_.direction);
            } else {
                // 方向がほぼゼロの場合は真下向きを使います。
                directionalLightData_.direction = { 0.0f, -1.0f, 0.0f };
            }

            // 保存用の色はそのままにし、GPUに渡す複製だけを描画用色空間に変換します。
            DirectionalLight gpuLight = directionalLightData_;
            gpuLight.color = engine::colorSpace::AuthoringToWorking(gpuLight.color);
            gpuLight.ambientColor = engine::colorSpace::AuthoringToWorking(gpuLight.ambientColor);
            gpuLight.fogColor = engine::colorSpace::AuthoringToWorking(gpuLight.fogColor);
            *dirMap = gpuLight;

            // 転送を終えた定数バッファのマッピングを解除します。
            directionalLightResource_->Unmap(0, nullptr);
        }
    }

    // 常設の点光源を先に処理し、描画対象だけをGPUの先頭から格納します。
    pointLightConstData_->activeCount = 0;

    for (auto& instance : pointLights_) {
        // GPUの枠が埋まったら、後続の常設灯の追従と点滅更新も打ち切ります。
        if (pointLightConstData_->activeCount >= kMaxPointLights)
            break;


        // 追従対象があれば GetTranslate() の座標に登録時のオフセットを足します。
        if (instance.target) {
            engine::Vector3 targetPos = instance.target->GetTranslate();
            instance.data.position = {
                targetPos.x + instance.offset.x,
                targetPos.y + instance.offset.y,
                targetPos.z + instance.offset.z
            };
        }

        // ランダム点滅では、更新ごとに基準強度の50〜99.5%を選びます。
        if (instance.mode == LightMode::kFlicker) {
            instance.timer += 1.0f / 60.0f * instance.speed;

            // ランダムなノイズ (故障したライト風)
            float noise = (float)(rand() % 100) / 100.0f;
            instance.data.intensity = instance.baseIntensity * (0.5f + noise * 0.5f);
        // サイン波モードは位相を進め、基準強度の50〜100%で明滅させます。
        } else if (instance.mode == LightMode::kSineWave) {
            instance.timer += 1.0f / 60.0f * instance.speed;

            // サイン波 (呼吸のような明滅)
            float wave = (std::sin(instance.timer) + 1.0f) * 0.5f;
            instance.data.intensity = instance.baseIntensity * (0.5f + wave * 0.5f);
        } else {
            // 通常時は基準値に戻す (モード切替時に暗くならないように)
            instance.data.intensity = instance.baseIntensity;
        }

        // 強さか半径が閾値以下なら、描画枠を消費せず次の常設灯へ進みます。
        if (instance.data.intensity <= 0.0001f || instance.data.radius <= 0.0001f) {
            continue;
        }

        // 視錐台があり、光の届く球全体がその外なら描画対象から外します。
        if (activeFrustum && !IntersectsFrustumSphere(*activeFrustum, instance.data.position, instance.data.radius)) {
            continue;
        }

        // 描画対象を詰めて格納し、送信用の色だけを変換します。
        std::int32_t index = pointLightConstData_->activeCount;
        MeshRenderer::PointLight gpuLight = instance.data;
        gpuLight.color = engine::colorSpace::AuthoringToWorking(gpuLight.color);
        pointLightConstData_->lights[index] = gpuLight;
        pointLightConstData_->activeCount++;
    }

    for (size_t index = 0; index < transientPointLights_.size();) {
        TransientPointLight& pulse = transientPointLights_[index];
        // 一時ライトは更新一回を1/60秒として進めます。残り割合を二乗し、終了が近いほど強度を滑らかに弱めます。
        pulse.age += 1.0f / 60.0f;
        const float duration = (std::max)(pulse.duration, 0.001f);
        const float progress = std::clamp(pulse.age / duration, 0.0f, 1.0f);
        const float envelope = (1.0f - progress) * (1.0f - progress);
        pulse.data.intensity = pulse.baseIntensity * envelope;

        // 寿命に達した発光は、そのフレームで一覧から除きます。
        if (progress >= 1.0f) {
            // 最後の要素を空いた位置に移すため、この周回では添字を進めず、移したライトも同じ更新で確認します。
            transientPointLights_[index] = transientPointLights_.back();
            transientPointLights_.pop_back();
            continue;
        }

        // 暗すぎる、半径が小さすぎる、または視錐台外の発光は寿命を管理しつつ描画だけ省きます。
        if (pulse.data.intensity <= 0.0001f || pulse.data.radius <= 0.0001f ||
            (activeFrustum && !IntersectsFrustumSphere(*activeFrustum, pulse.data.position, pulse.data.radius))) {
            ++index;
            continue;
        }

        // 常設灯の後に空き枠がある場合、一時発光もGPU用の点光源として登録します。
        if (pointLightConstData_->activeCount < kMaxPointLights) {
            std::int32_t transientIndex = pointLightConstData_->activeCount;
            MeshRenderer::PointLight gpuLight = pulse.data;
            gpuLight.color = engine::colorSpace::AuthoringToWorking(gpuLight.color);
            pointLightConstData_->lights[transientIndex] = gpuLight;
            pointLightConstData_->activeCount++;
        }

        ++index;
    }

    // スポットライトも追従と強度を更新し、描画対象だけを専用の領域に格納します。
    spotLightConstData_->activeCount = 0;

    for (auto& instance : spotLights_) {
        // 描画枠が上限に達したら、残りのライトの更新を打ち切ります。
        if (spotLightConstData_->activeCount >= kMaxSpotLights)
            break;


        // 追従対象があれば、GetTranslate() の座標にオフセットを足し、向きの候補も取得します。
        if (instance.target) {
            engine::Vector3 targetPos = instance.target->GetTranslate();
            instance.data.position = {
                targetPos.x + instance.offset.x,
                targetPos.y + instance.offset.y,
                targetPos.z + instance.offset.z
            };

            // 対象のワールド行列の第3列を、ライト方向の候補として使います。
            engine::Matrix4x4 worldMat = instance.target->GetWorldMatrix();
            engine::Vector3 forward = { worldMat.m[0][2], worldMat.m[1][2], worldMat.m[2][2] };

            // 方向候補の長さを求め、単位ベクトルへ変換できるか判定します。
            float length = std::sqrt(forward.x * forward.x + forward.y * forward.y + forward.z * forward.z);

            // 方向候補がほぼゼロの場合は、これまでの向きを保持します。
            if (length > 0.0001f) {
                instance.data.direction = { forward.x / length, forward.y / length, forward.z / length };
            }
        }

        // ランダム点滅では、更新ごとに基準強度の50〜99.5%を選びます。
        if (instance.mode == LightMode::kFlicker) {
            instance.timer += 1.0f / 60.0f * instance.speed;
            float noise = (float)(rand() % 100) / 100.0f;
            instance.data.intensity = instance.baseIntensity * (0.5f + noise * 0.5f);
        // サイン波モードでは、更新ごとに位相を進めて基準強度の50〜100%を使います。
        } else if (instance.mode == LightMode::kSineWave) {
            instance.timer += 1.0f / 60.0f * instance.speed;
            float wave = (std::sin(instance.timer) + 1.0f) * 0.5f;
            instance.data.intensity = instance.baseIntensity * (0.5f + wave * 0.5f);
        // どちらの明滅モードでもなければ基準強度へ戻します。
        } else {
            instance.data.intensity = instance.baseIntensity;
        }

        // 強さか到達距離が閾値以下なら、描画枠に載せません。
        if (instance.data.intensity <= 0.0001f || instance.data.distance <= 0.0001f) {
            continue;
        }

        // 視錐台があり、到達距離を半径とする球全体がその外ならGPU転送を省きます。
        if (activeFrustum && !IntersectsFrustumSphere(*activeFrustum, instance.data.position, instance.data.distance)) {
            continue;
        }

        // 送信する色を描画用に変換し、スポットライトを詰めて格納します。
        std::int32_t index = spotLightConstData_->activeCount;
        MeshRenderer::SpotLight gpuLight = instance.data;
        gpuLight.color = engine::colorSpace::AuthoringToWorking(gpuLight.color);
        spotLightConstData_->lights[index] = gpuLight;
        spotLightConstData_->activeCount++;
    }
}

const engine::Matrix4x4& LightManager::GetDirectionalShadowViewProjection(const engine::Camera* camera) {
    // 光源方向とカメラ位置を基準に影領域を決め、条件が変わった場合だけ行列を更新します。
    engine::Math math;
    engine::Vector3 lightDirection = directionalLightData_.direction;

    // 光の方向に十分な長さがあれば、影用の視点計算に使う単位ベクトルを得ます。
    if (engine::Math::Length(lightDirection) > 0.0001f) {
        lightDirection = engine::Math::Normalize(lightDirection);
    }
    // 方向がほぼゼロなら真下向きで影を作ります。
    else {
        lightDirection = { 0.0f, -1.0f, 0.0f };
    }

    directionalLightData_.direction = lightDirection;

    const engine::Vector3 target = camera ? camera->GetEye() : engine::Vector3{};
    const float areaSize = std::clamp(shadowAreaSize_, 20.0f, 240.0f);
    const auto changed = [](float lhs, float rhs) {
        return std::abs(lhs - rhs) > 0.0001f;
    };

    const bool eyeChanged =
        changed(target.x, cachedShadowEye_.x) ||
        changed(target.y, cachedShadowEye_.y) ||
        changed(target.z, cachedShadowEye_.z);
    const bool directionChanged =
        changed(lightDirection.x, cachedShadowDirection_.x) ||
        changed(lightDirection.y, cachedShadowDirection_.y) ||
        changed(lightDirection.z, cachedShadowDirection_.z);

    // 初回か、基準位置・光の向き・範囲が許容差を超えて変化した場合に再計算します。
    if (!shadowMatrixCacheValid_ || eyeChanged || directionChanged || changed(areaSize, cachedShadowAreaSize_)) {
        // 影の基準位置から光の進行方向と逆へ200ワールド単位離し、平行光用の正射影を組み立てます。
        const engine::Vector3 lightPosition = {
            target.x - lightDirection.x * 200.0f,
            target.y - lightDirection.y * 200.0f,
            target.z - lightDirection.z * 200.0f,
        };

        engine::Vector3 up = { 0.0f, 1.0f, 0.0f };

        // 光がほぼ垂直なときは、視線と上方向が平行に近づかないようZ軸を上方向にします。
        if (std::abs(lightDirection.x) < 0.001f && std::abs(lightDirection.z) < 0.001f) {
            up = { 0.0f, 0.0f, 1.0f };
        }

        const engine::Matrix4x4 lightView = math.MakeLookAtMatrix(lightPosition, target, up);
        const engine::Matrix4x4 lightProjection = math.MakeOrthographicMatrix(areaSize, areaSize, 1.0f, 400.0f);
        directionalLightData_.lightViewProj = math.Multiply(lightView, lightProjection);
        cachedShadowEye_ = target;
        cachedShadowDirection_ = lightDirection;
        cachedShadowAreaSize_ = areaSize;
        shadowMatrixCacheValid_ = true;
    }

    return directionalLightData_.lightViewProj;
}

std::int32_t LightManager::GetShadowMapResolution() const {
    // 描画基盤が未初期化の間は標準解像度で設定画面を表示します。
    return dxCommon_ ? dxCommon_->GetShadowMapResolution() : 2048;
}

void LightManager::SetShadowMapResolution(std::int32_t resolution) {
    // 実際のShadow Mapを所有する描画基盤へ変更要求を渡します。
    // 描画基盤が利用できる場合にだけ解像度変更を依頼します。
    if (dxCommon_) {
        dxCommon_->SetShadowMapResolution(resolution);
    }
}

void LightManager::SetShadowAreaSize(float size) {
    // 極端な影範囲を抑え、次回の影行列を再計算させます。
    shadowAreaSize_ = std::clamp(size, 20.0f, 240.0f);
    shadowMatrixCacheValid_ = false;
}

void LightManager::ApplySceneClearColor() {
    // Scene背景色をレンダーターゲットのクリア値へ反映します。
    // 描画基盤が未初期化なら、クリア値の反映をここでは行いません。
    if (!dxCommon_) {
        return;
    }

    dxCommon_->SetRenderClearColor(
        sceneClearColor_.x,
        sceneClearColor_.y,
        sceneClearColor_.z,
        sceneClearColor_.w);
}

// 既定の点光源を登録できればtrue。GPUへ転送する数の上限に達していれば、一覧は変更しない。
bool LightManager::AddPointLight() {
    // 常設点光源の登録枠が上限なら、新しいライトを生成できません。
    if (pointLights_.size() >= kMaxPointLights)
        return false;

    PointLightInstance instance;
    instance.name = "PointLight " + std::to_string(pointLights_.size());

    // デフォルト値
    instance.data.color = { 1.0f, 1.0f, 1.0f, 1.0f };
    instance.data.position = { 0.0f, 2.0f, 0.0f };
    instance.data.intensity = 1.0f;
    instance.data.radius = 10.0f;
    instance.data.decay = 1.0f;

    // 制御用データの初期化
    instance.baseIntensity = 1.0f;
    instance.mode = LightMode::kNone;
    instance.target = nullptr;

    pointLights_.push_back(instance);
    return true;
}

// 既定のスポットライトを登録できればtrue。上限超過時は対象の一覧だけを保持する。
bool LightManager::AddSpotLight() {
    // 常設スポットライトの登録枠を使い切ったら、一覧を変更せず false を返します。
    if (spotLights_.size() >= kMaxSpotLights)
        return false;

    SpotLightInstance instance;
    instance.name = "SpotLight " + std::to_string(spotLights_.size());
    instance.data.color = { 1.0f, 1.0f, 1.0f, 1.0f };
    instance.data.position = { 0.0f, 5.0f, 0.0f };
    instance.data.direction = { 0.0f, -1.0f, 0.0f };
    instance.data.intensity = 2.0f;
    instance.data.distance = 20.0f;
    instance.data.decay = 1.0f;
    instance.data.cosAngle = 0.9f;
    instance.data.cosFalloffStart = 0.95f;

    // 制御用データ
    instance.baseIntensity = 2.0f;
    instance.mode = LightMode::kNone;
    instance.target = nullptr;

    spotLights_.push_back(instance);
    return true;
}

bool LightManager::SetPointLight(std::size_t index, const PointLightInstance& value) {
    // 半径・減衰・基準強度を非負に補正してから保存します。
    // 指定した添字が一覧の範囲外なら差し替えません。
    if (index >= pointLights_.size())
        return false;
    pointLights_[index] = value;
    pointLights_[index].data.radius = (std::max)(0.0f, pointLights_[index].data.radius);
    pointLights_[index].data.decay = (std::max)(0.0f, pointLights_[index].data.decay);
    pointLights_[index].baseIntensity = (std::max)(0.0f, pointLights_[index].baseIntensity);
    return true;
}

bool LightManager::SetSpotLight(std::size_t index, const SpotLightInstance& value) {
    // 到達距離と減衰の負値を排除し、GPUへ渡す設定を保ちます。
    // 指定した添字が一覧の範囲外なら差し替えません。
    if (index >= spotLights_.size())
        return false;
    spotLights_[index] = value;
    spotLights_[index].data.distance = (std::max)(0.0f, spotLights_[index].data.distance);
    spotLights_[index].data.decay = (std::max)(0.0f, spotLights_[index].data.decay);
    spotLights_[index].baseIntensity = (std::max)(0.0f, spotLights_[index].baseIntensity);
    return true;
}

bool LightManager::RemovePointLight(std::size_t index) {
    // 不正インデックスでは一覧を変更せず、呼び出し側へ失敗を返します。
    if (index >= pointLights_.size())
        return false;
    pointLights_.erase(pointLights_.begin() + index);
    return true;
}

bool LightManager::RemoveSpotLight(std::size_t index) {
    // 不正インデックスでは一覧を変更せず、呼び出し側へ失敗を返します。
    if (index >= spotLights_.size())
        return false;
    spotLights_.erase(spotLights_.begin() + index);
    return true;
}

void LightManager::ClearPointLights() { pointLights_.clear(); }
void LightManager::ClearSpotLights() { spotLights_.clear(); }

void LightManager::ClearAllLights() {
    // 常設灯だけでなく一時的な発光演出も同時に破棄します。
    pointLights_.clear();
    spotLights_.clear();
    transientPointLights_.clear();
}

void LightManager::PlayPointLightPulse(
    const engine::Vector3& position,
    const engine::Vector4& color,
    float intensity,
    float radius,
    float duration,
    float decay) {
    // 位置と色を指定した一時発光を登録し、秒単位の寿命を最低0.001秒にします。Update() が減衰と破棄を担当します。
    TransientPointLight pulse;
    pulse.data.color = color;
    pulse.data.position = position;
    pulse.data.intensity = intensity;
    pulse.data.radius = radius;
    pulse.data.decay = decay;
    pulse.baseIntensity = intensity;
    pulse.duration = (std::max)(duration, 0.001f);
    transientPointLights_.push_back(pulse);
}

bool LightManager::SetSkyboxTexturePath(const std::string& texturePath) {
    // 空のパスは設定として受け付けません。
    if (texturePath.empty()) {
        return false;
    }

    try {
        // ファイルが存在しなければ、現在の設定を保持します。
        if (!std::filesystem::exists(texturePath)) {
            return false;
        }

        // 存在を確認できたパスを記録してから sRGB テクスチャを読み込みます。
        skyboxTexturePath_ = texturePath;
        skyboxTextureHandle_ = engine::TextureManager::GetInstance()->Load(
            texturePath,
            engine::TextureManager::TextureColorSpace::kSRGB);
        return true;
    } catch (...) {
        // パスの確認や読み込みで例外が出た場合は失敗を返します。パス記録後の例外では記録したパスが残ります。
        return false;
    }
}

bool LightManager::SaveState(const std::string& filename) {
    // 背景・影と常設ライトの現在値をJSONへまとめます。一時発光や追従先の参照は保存しません。
    json root;

    root["clearColor"] = { sceneClearColor_.x, sceneClearColor_.y, sceneClearColor_.z, sceneClearColor_.w };
    root["skybox"]["enabled"] = skyboxEnabled_;
    root["skybox"]["texture"] = skyboxTexturePath_;
    root["shadow"]["resolution"] = GetShadowMapResolution();
    root["shadow"]["areaSize"] = shadowAreaSize_;

    // 平行光の向きや強度とフォグ設定を保存します。
    root["directionalLight"]["color"] = { directionalLightData_.color.x, directionalLightData_.color.y, directionalLightData_.color.z, directionalLightData_.color.w };
    root["directionalLight"]["direction"] = { directionalLightData_.direction.x, directionalLightData_.direction.y, directionalLightData_.direction.z };
    root["directionalLight"]["intensity"] = directionalLightData_.intensity;
    root["directionalLight"]["ambientColor"] = { directionalLightData_.ambientColor.x, directionalLightData_.ambientColor.y, directionalLightData_.ambientColor.z };
    root["directionalLight"]["fogStart"] = directionalLightData_.fogStart;
    root["directionalLight"]["fogEnd"] = directionalLightData_.fogEnd;
    root["directionalLight"]["fogColor"] = { directionalLightData_.fogColor.x, directionalLightData_.fogColor.y, directionalLightData_.fogColor.z };
    root["directionalLight"]["fogHeightMin"] = directionalLightData_.fogHeightMin;
    root["directionalLight"]["fogHeightMax"] = directionalLightData_.fogHeightMax;
    root["directionalLight"]["volumetricIntensity"] = directionalLightData_.volumetricIntensity;
    root["directionalLight"]["volumetricSteps"] = directionalLightData_.volumetricSteps;
    root["directionalLight"]["enableFog"] = directionalLightData_.enableFog;

    // 点光源の描画値と点滅の基準値を別々に保存します。
    json pArray = json::array();

    for (const auto& instance : pointLights_) {
        json j;
        j["name"] = instance.name;

        // GPUデータ
        const auto& l = instance.data;
        j["position"] = { l.position.x, l.position.y, l.position.z };
        j["color"] = { l.color.x, l.color.y, l.color.z, l.color.w };
        j["intensity"] = l.intensity; // ここは現在の値を保存
        j["radius"] = l.radius;
        j["decay"] = l.decay;

        // 読み込み後も点滅と位置補正を再現できるよう制御値を保持します。
        j["baseIntensity"] = instance.baseIntensity;
        j["mode"] = static_cast<std::int32_t>(instance.mode);
        j["speed"] = instance.speed;
        j["offset"] = { instance.offset.x, instance.offset.y, instance.offset.z };

        pArray.push_back(j);
    }

    root["pointLights"] = pArray;

    // スポットライトでは向きとコーンの減衰角も保存します。
    json sArray = json::array();

    for (const auto& instance : spotLights_) {
        json j;
        j["name"] = instance.name;
        const auto& l = instance.data;
        j["position"] = { l.position.x, l.position.y, l.position.z };
        j["direction"] = { l.direction.x, l.direction.y, l.direction.z };
        j["color"] = { l.color.x, l.color.y, l.color.z, l.color.w };
        j["intensity"] = l.intensity;
        j["distance"] = l.distance;
        j["decay"] = l.decay;
        j["cosAngle"] = l.cosAngle;
        j["cosFalloffStart"] = l.cosFalloffStart;

        // 制御データ
        j["baseIntensity"] = instance.baseIntensity;
        j["mode"] = static_cast<std::int32_t>(instance.mode);
        j["speed"] = instance.speed;
        j["offset"] = { instance.offset.x, instance.offset.y, instance.offset.z };

        sArray.push_back(j);
    }

    root["spotLights"] = sArray;

    try {
        const std::filesystem::path path(filename);

        // 親フォルダーを含む保存先の場合は、書き込み前にフォルダーを作成します。
        if (!path.parent_path().empty()) {
            std::filesystem::create_directories(path.parent_path());
        }
    } catch (...) {
        return false;
    }

    std::ofstream file(filename, std::ios::trunc);

    // 保存先を開けない場合は、状態ファイルの記録を更新しません。
    if (!file.is_open()) {
        return false;
    }

    file << root.dump(4);
    const bool result = file.good();

    // 出力ストリームが正常な場合に保存先と成功状態を記録します。
    if (result) {
        currentStateFile_ = filename;
        lastLoadSucceeded_ = true;
    }

    return result;
}

bool LightManager::LoadState(const std::string& filename) {
    // 読み込みを試みたファイルを記録し、復元が終わるまでは失敗扱いにします。
    currentStateFile_ = filename;
    lastLoadSucceeded_ = false;

    std::ifstream file(filename);
    // 設定ファイルを開けなければ、現在のライトを保持します。
    if (!file.is_open())
        return false;

    json root;
    try { file >> root; }
    catch (...) { return false; }

    // ファイルの読み取りと解析が済んでから、前シーンの点光源・スポットライト・一時発光を取り除きます。
    ClearAllLights();

    // 背景色に配列の4成分があれば、RGBAとして反映します。
    if (root.contains("clearColor") && root["clearColor"].is_array() && root["clearColor"].size() >= 4) {
        sceneClearColor_.x = root["clearColor"][0];
        sceneClearColor_.y = root["clearColor"][1];
        sceneClearColor_.z = root["clearColor"][2];
        sceneClearColor_.w = root["clearColor"][3];
    // 背景色が無いか、配列としての成分数が足りなければ既定色を使います。
    } else {
        sceneClearColor_ = { 0.1f, 0.25f, 0.5f, 1.0f };
    }

    ApplySceneClearColor();

    skyboxEnabled_ = true;

    // 空の設定がJSONオブジェクトなら、指定されている項目を反映します。
    if (root.contains("skybox") && root["skybox"].is_object()) {
        const json& skybox = root["skybox"];

        // 表示フラグが保存されていれば、直前に設定した既定の有効状態を上書きします。
        if (skybox.contains("enabled")) {
            skyboxEnabled_ = skybox["enabled"].get<bool>();
        }

        // テクスチャパスが文字列として保存されていれば読み込みを試します。
        if (skybox.contains("texture") && skybox["texture"].is_string()) {
            const std::string texturePath = skybox["texture"].get<std::string>();

            // 指定画像を設定できない場合は、標準のスカイボックス画像へフォールバックします。
            if (!SetSkyboxTexturePath(texturePath)) {
                SetSkyboxTexturePath("Resources/output_skybox.dds");
            }
        }
    // 空の設定が無いかオブジェクトでなければ、標準の画像を設定します。
    } else {
        SetSkyboxTexturePath("Resources/output_skybox.dds");
    }

    std::int32_t shadowResolution = 2048;
    float shadowAreaSize = 80.0f;

    // 影設定がオブジェクトであれば、含まれる解像度と範囲を読み取ります。
    if (root.contains("shadow") && root["shadow"].is_object()) {
        const json& shadow = root["shadow"];
        shadowResolution = shadow.value("resolution", shadowResolution);
        shadowAreaSize = shadow.value("areaSize", shadowAreaSize);
    }

    SetShadowMapResolution(shadowResolution);
    SetShadowAreaSize(shadowAreaSize);

    // 平行光の項目が保存されている場合に復元し、項目が無ければその項目の現在値を保ちます。
    if (root.contains("directionalLight")) {
        json& d = root["directionalLight"];

        if (d.contains("color")) {
            directionalLightData_.color.x = d["color"][0];
            directionalLightData_.color.y = d["color"][1];
            directionalLightData_.color.z = d["color"][2];
            directionalLightData_.color.w = d["color"][3];
        }

        if (d.contains("direction")) {
            directionalLightData_.direction.x = d["direction"][0];
            directionalLightData_.direction.y = d["direction"][1];
            directionalLightData_.direction.z = d["direction"][2];
        }

        if (d.contains("intensity"))
            directionalLightData_.intensity = d["intensity"];
        if (d.contains("ambientColor")) {
            directionalLightData_.ambientColor.x = d["ambientColor"][0];
            directionalLightData_.ambientColor.y = d["ambientColor"][1];
            directionalLightData_.ambientColor.z = d["ambientColor"][2];
        }

        // フォグも、各項目のキーが存在する場合だけ距離・高さ・色・品質・有効状態を更新します。
        if (d.contains("fogStart"))
            directionalLightData_.fogStart = d["fogStart"];
        if (d.contains("fogEnd"))
            directionalLightData_.fogEnd = d["fogEnd"];
        if (d.contains("fogColor")) {
            directionalLightData_.fogColor.x = d["fogColor"][0];
            directionalLightData_.fogColor.y = d["fogColor"][1];
            directionalLightData_.fogColor.z = d["fogColor"][2];
        }

        if (d.contains("fogHeightMin"))
            directionalLightData_.fogHeightMin = d["fogHeightMin"];
        if (d.contains("fogHeightMax"))
            directionalLightData_.fogHeightMax = d["fogHeightMax"];

        if (d.contains("volumetricIntensity"))
            directionalLightData_.volumetricIntensity = d["volumetricIntensity"];
        if (d.contains("volumetricSteps"))
            directionalLightData_.volumetricSteps = d["volumetricSteps"];
        if (d.contains("enableFog"))
            directionalLightData_.enableFog = d["enableFog"];
    }

    // 点光源とスポットライトの共通項目は、各キーがある場合だけ新規ライトの既定値を上書きします。
    const auto readCommonLight = [](const nlohmann::json& j, auto& instance) {
        auto& l = instance.data;
        if (j.contains("name"))
            instance.name = j["name"];
        if (j.contains("position")) {
            l.position.x = j["position"][0];
            l.position.y = j["position"][1];
            l.position.z = j["position"][2];
        }
        if (j.contains("color")) {
            l.color.x = j["color"][0];
            l.color.y = j["color"][1];
            l.color.z = j["color"][2];
            l.color.w = j["color"][3];
        }
        if (j.contains("intensity"))
            l.intensity = j["intensity"];
        if (j.contains("decay"))
            l.decay = j["decay"];

        // 旧ファイルに基準強度がなければ、読み込んだ強さを点滅の基準にする。
        if (j.contains("baseIntensity"))
            instance.baseIntensity = j["baseIntensity"];
        // 基準強度の無い旧形式では、今回復元した強度を明滅にも使います。
        else instance.baseIntensity = l.intensity;

        if (j.contains("mode"))
            instance.mode = (LightMode)j["mode"];
        if (j.contains("speed"))
            instance.speed = j["speed"];
        if (j.contains("offset")) {
            instance.offset.x = j["offset"][0];
            instance.offset.y = j["offset"][1];
            instance.offset.z = j["offset"][2];
        }
    };

    // 点光源の配列が保存されていれば、既定値で登録してから各要素を反映します。
    if (root.contains("pointLights") && root["pointLights"].is_array()) {
        for (const auto& j : root["pointLights"]) {
            PointLightInstance* instance = AddPointLight() ? &pointLights_.back() : nullptr;

            // 登録枠に収まった要素だけを復元し、値を補正して一覧に残します。
            if (instance) {
                readCommonLight(j, *instance);
                auto& l = instance->data;
                // 半径が保存されている場合に既定の作用範囲を上書きします。
                if (j.contains("radius"))
                    l.radius = j["radius"];
                SetPointLight(pointLights_.size() - 1, *instance);
            }
        }
    }

    // スポットライトの配列が保存されていれば、方向や照射範囲も要素ごとに復元します。
    if (root.contains("spotLights") && root["spotLights"].is_array()) {
        for (const auto& j : root["spotLights"]) {
            SpotLightInstance* instance = AddSpotLight() ? &spotLights_.back() : nullptr;

            // 登録できた場合だけ共通項目とスポット固有の項目を復元します。固有項目が欠けていればそれぞれの既定値を保ちます。
            if (instance) {
                readCommonLight(j, *instance);
                auto& l = instance->data;
                if (j.contains("direction")) {
                    l.direction.x = j["direction"][0];
                    l.direction.y = j["direction"][1];
                    l.direction.z = j["direction"][2];
                }
                if (j.contains("distance"))
                    l.distance = j["distance"];
                if (j.contains("cosAngle"))
                    l.cosAngle = j["cosAngle"];
                if (j.contains("cosFalloffStart"))
                    l.cosFalloffStart = j["cosFalloffStart"];
                SetSpotLight(spotLights_.size() - 1, *instance);
            }
        }
    }

    // ライトの全項目を取り込めた時点で読み込み成功を記録します。
    lastLoadSucceeded_ = true;
    return true;
}

LightManager::EnvironmentProfileState LightManager::CaptureEnvironmentProfile() const {
    // 光源・空・PostEffectを一つの状態へ収集し、補間開始点や保存値に使います。
    EnvironmentProfileState profile;
    profile.directionalLight = directionalLightData_;
    profile.clearColor = sceneClearColor_;
    profile.shadowMapResolution = GetShadowMapResolution();
    profile.shadowAreaSize = shadowAreaSize_;
    profile.skyboxEnabled = skyboxEnabled_;
    profile.skyboxTexturePath = skyboxTexturePath_;

    PostEffect* postEffect = PostEffect::GetInstance();

    // PostEffect のパラメーターが取得できた場合だけ、描画補正の現在値も記録します。
    if (const PostEffect::Params* params = postEffect->GetParams()) {
        profile.post.bloomEnabled = postEffect->IsBloomEnabled();
        profile.post.bloomQuality = static_cast<std::int32_t>(postEffect->GetBloomQuality());
        profile.post.threshold = params->threshold;
        profile.post.bloomIntensity = params->bloomIntensity;
        profile.post.spread = params->spread;
        profile.post.enableToneMapping = params->enableToneMapping;
        profile.post.vignetteIntensity = params->vignetteIntensity;
        profile.post.vignettePower = params->vignettePower;
        profile.post.chromaticAberration = params->chromaticAberration;
        profile.post.filmGrainIntensity = params->filmGrainIntensity;
        profile.post.lutIntensity = params->lutIntensity;
        profile.post.lutTexturePath = postEffect->GetLUTTexturePath();
        profile.post.colorExposure = params->colorExposure;
        profile.post.colorContrast = params->colorContrast;
        profile.post.colorSaturation = params->colorSaturation;
        profile.post.colorTemperature = params->colorTemperature;
        profile.post.colorTint = params->colorTint;
    }

    return profile;
}

bool LightManager::SaveEnvironmentProfile(const std::string& filename) const {
    // 現在の環境をスキーマ付きJSONで保存します。保存先が空なら書き込みを開始しません。
    if (filename.empty()) {
        return false;
    }

    const EnvironmentProfileState profile = CaptureEnvironmentProfile();
    const DirectionalLight& light = profile.directionalLight;
    const EnvironmentPostSettings& post = profile.post;

    json root;
    root["schemaVersion"] = 1;
    root["directionalLight"] = {
        { "color", { light.color.x, light.color.y, light.color.z, light.color.w } },
        { "direction", { light.direction.x, light.direction.y, light.direction.z } },
        { "intensity", light.intensity },
        { "ambientColor", { light.ambientColor.x, light.ambientColor.y, light.ambientColor.z } },
        { "fogStart", light.fogStart },
        { "fogEnd", light.fogEnd },
        { "fogColor", { light.fogColor.x, light.fogColor.y, light.fogColor.z } },
        { "fogHeightMin", light.fogHeightMin },
        { "fogHeightMax", light.fogHeightMax },
        { "volumetricIntensity", light.volumetricIntensity },
        { "volumetricSteps", light.volumetricSteps },
        { "enableFog", light.enableFog }
    };

    root["clearColor"] = {
        profile.clearColor.x, profile.clearColor.y,
        profile.clearColor.z, profile.clearColor.w
    };

    root["shadow"] = {
        { "resolution", profile.shadowMapResolution },
        { "areaSize", profile.shadowAreaSize }
    };

    root["skybox"] = {
        { "enabled", profile.skyboxEnabled },
        { "texture", profile.skyboxTexturePath }
    };

    root["postEffect"] = {
        { "bloomEnabled", post.bloomEnabled },
        { "bloomQuality", post.bloomQuality },
        { "threshold", post.threshold },
        { "bloomIntensity", post.bloomIntensity },
        { "spread", post.spread },
        { "enableToneMapping", post.enableToneMapping },
        { "vignetteIntensity", post.vignetteIntensity },
        { "vignettePower", post.vignettePower },
        { "chromaticAberration", post.chromaticAberration },
        { "filmGrainIntensity", post.filmGrainIntensity },
        { "lutIntensity", post.lutIntensity },
        { "lutTexture", post.lutTexturePath },
        { "colorExposure", post.colorExposure },
        { "colorContrast", post.colorContrast },
        { "colorSaturation", post.colorSaturation },
        { "colorTemperature", post.colorTemperature },
        { "colorTint", post.colorTint }
    };

    const std::filesystem::path path(filename);
    std::error_code error;

    // 親フォルダーが指定されていれば、出力を開く前に作成を試します。
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);

        // フォルダー作成に失敗した場合は、書き込みに進みません。
        if (error) {
            return false;
        }
    }

    std::ofstream file(path, std::ios::binary);

    // ファイルを開けなければ保存失敗を返します。
    if (!file) {
        return false;
    }

    file << root.dump(2);
    return file.good();
}

bool LightManager::LoadEnvironmentProfile(
    const std::string& filename,
    EnvironmentProfileState& profile) const {
    // JSONにない項目は現行環境を初期値にし、古いプロファイルも読み込めるようにします。
    std::ifstream file(filename, std::ios::binary);

    // 入力を開けなければ、出力プロファイルを書き換えません。
    if (!file) {
        return false;
    }

    try {
        json root;
        file >> root;
        profile = CaptureEnvironmentProfile();

        const auto readVector3 = [](const json& value, engine::Vector3& destination) {
            // 配列に3成分以上あるときだけXYZまたはRGBを読み込みます。
            if (value.is_array() && value.size() >= 3) {
                destination = { value[0].get<float>(), value[1].get<float>(), value[2].get<float>() };
            }
        };

        const auto readVector4 = [](const json& value, engine::Vector4& destination) {
            // 配列に4成分以上あるときだけRGBAを読み込みます。
            if (value.is_array() && value.size() >= 4) {
                destination = {
                    value[0].get<float>(), value[1].get<float>(),
                    value[2].get<float>(), value[3].get<float>()
                };
            }
        };

        // 平行光の設定がオブジェクトとしてあれば、存在する保存項目を既定値へ重ねます。
        if (root.contains("directionalLight") && root["directionalLight"].is_object()) {
            const json& source = root["directionalLight"];
            DirectionalLight& light = profile.directionalLight;
            if (source.contains("color"))
                readVector4(source["color"], light.color);
            if (source.contains("direction"))
                readVector3(source["direction"], light.direction);
            light.intensity = source.value("intensity", light.intensity);
            if (source.contains("ambientColor"))
                readVector3(source["ambientColor"], light.ambientColor);
            light.fogStart = source.value("fogStart", light.fogStart);
            light.fogEnd = source.value("fogEnd", light.fogEnd);
            if (source.contains("fogColor"))
                readVector3(source["fogColor"], light.fogColor);
            light.fogHeightMin = source.value("fogHeightMin", light.fogHeightMin);
            light.fogHeightMax = source.value("fogHeightMax", light.fogHeightMax);
            light.volumetricIntensity = source.value("volumetricIntensity", light.volumetricIntensity);
            light.volumetricSteps = source.value("volumetricSteps", light.volumetricSteps);
            light.enableFog = source.value("enableFog", light.enableFog);
        }

        // 背景色が指定されている場合は、配列の成分数を確認してから反映します。
        if (root.contains("clearColor")) {
            readVector4(root["clearColor"], profile.clearColor);
        }

        // 影設定がオブジェクトの場合に解像度と範囲を取得します。
        if (root.contains("shadow") && root["shadow"].is_object()) {
            const json& source = root["shadow"];
            profile.shadowMapResolution = source.value("resolution", profile.shadowMapResolution);
            profile.shadowAreaSize = source.value("areaSize", profile.shadowAreaSize);
        }

        // 空の設定がオブジェクトの場合に表示フラグと画像パスを取得します。
        if (root.contains("skybox") && root["skybox"].is_object()) {
            const json& source = root["skybox"];
            profile.skyboxEnabled = source.value("enabled", profile.skyboxEnabled);
            profile.skyboxTexturePath = source.value("texture", profile.skyboxTexturePath);
        }

        // 描画補正の設定がオブジェクトの場合に、書かれているパラメーターを採用します。
        if (root.contains("postEffect") && root["postEffect"].is_object()) {
            const json& source = root["postEffect"];
            EnvironmentPostSettings& post = profile.post;
            post.bloomEnabled = source.value("bloomEnabled", post.bloomEnabled);
            post.bloomQuality = source.value("bloomQuality", post.bloomQuality);
            post.threshold = source.value("threshold", post.threshold);
            post.bloomIntensity = source.value("bloomIntensity", post.bloomIntensity);
            post.spread = source.value("spread", post.spread);
            post.enableToneMapping = source.value("enableToneMapping", post.enableToneMapping);
            post.vignetteIntensity = source.value("vignetteIntensity", post.vignetteIntensity);
            post.vignettePower = source.value("vignettePower", post.vignettePower);
            post.chromaticAberration = source.value("chromaticAberration", post.chromaticAberration);
            post.filmGrainIntensity = source.value("filmGrainIntensity", post.filmGrainIntensity);
            post.lutIntensity = source.value("lutIntensity", post.lutIntensity);
            post.lutTexturePath = source.value("lutTexture", post.lutTexturePath);
            post.colorExposure = source.value("colorExposure", post.colorExposure);
            post.colorContrast = source.value("colorContrast", post.colorContrast);
            post.colorSaturation = source.value("colorSaturation", post.colorSaturation);
            post.colorTemperature = source.value("colorTemperature", post.colorTemperature);
            post.colorTint = source.value("colorTint", post.colorTint);
        }

        // 影解像度と範囲、ブルーム品質を選択可能な範囲へ寄せて読み込みを終えます。
        profile.shadowMapResolution =
            profile.shadowMapResolution <= 1024 ? 1024 :
            (profile.shadowMapResolution <= 2048 ? 2048 : 4096);
        profile.shadowAreaSize = std::clamp(profile.shadowAreaSize, 20.0f, 240.0f);
        profile.post.bloomQuality = std::clamp(profile.post.bloomQuality, 0, 2);
        return true;
    }
    catch (...) {
        return false;
    }
}

bool LightManager::ApplyEnvironmentProfile(const std::string& filename, float blendDuration) {
    // 読込失敗時は現在の環境を維持し、成功時だけ補間を開始します。
    EnvironmentProfileState profile;

    // 指定のプロファイルを読み込めなければ、適用に進みません。
    if (!LoadEnvironmentProfile(filename, profile)) {
        return false;
    }

    ApplyEnvironmentProfile(profile, blendDuration);
    currentEnvironmentProfileFile_ = filename;
    return true;
}

void LightManager::ApplyEnvironmentProfile(
    const EnvironmentProfileState& profile,
    float blendDuration) {
    // 連続値は時間で補間し、離散設定は補間の中間で切り替えます。
    currentEnvironmentProfileFile_.clear();
    environmentBlendFrom_ = CaptureEnvironmentProfile();
    environmentBlendTo_ = profile;
    environmentBlendElapsed_ = 0.0f;
    environmentBlendDuration_ = (std::max)(0.0f, blendDuration);
    environmentBlendActive_ = environmentBlendDuration_ > 0.0001f;
    environmentBlendDiscreteApplied_ = false;

    // 補間時間が0.0001秒以下なら、次フレームを待たず直接目標値を設定します。
    if (!environmentBlendActive_) {
        directionalLightData_ = profile.directionalLight;
        sceneClearColor_ = profile.clearColor;
        SetShadowMapResolution(profile.shadowMapResolution);
        SetShadowAreaSize(profile.shadowAreaSize);
        skyboxEnabled_ = profile.skyboxEnabled;

        // スカイボックスのパスが空でない場合だけ、画像の差し替えを試みます。
        if (!profile.skyboxTexturePath.empty()) {
            SetSkyboxTexturePath(profile.skyboxTexturePath);
        }

        ApplySceneClearColor();

        PostEffect* postEffect = PostEffect::GetInstance();

        // PostEffect のパラメーターが取れるときは描画補正も即時適用します。
        if (PostEffect::Params* params = postEffect->GetParams()) {
            const EnvironmentPostSettings& post = profile.post;
            postEffect->SetBloomEnabled(post.bloomEnabled);
            postEffect->SetBloomQuality(static_cast<PostEffect::BloomQuality>(post.bloomQuality));

            // LUT画像パスが空でない場合だけ、画像を差し替えます。
            if (!post.lutTexturePath.empty()) {
                postEffect->SetLUTTexturePath(post.lutTexturePath);
            }

            params->threshold = post.threshold;
            params->bloomIntensity = post.bloomIntensity;
            params->spread = post.spread;
            params->enableToneMapping = post.enableToneMapping;
            params->vignetteIntensity = post.vignetteIntensity;
            params->vignettePower = post.vignettePower;
            params->chromaticAberration = post.chromaticAberration;
            params->filmGrainIntensity = post.filmGrainIntensity;
            params->lutIntensity = post.lutIntensity;
            params->colorExposure = post.colorExposure;
            params->colorContrast = post.colorContrast;
            params->colorSaturation = post.colorSaturation;
            params->colorTemperature = post.colorTemperature;
            params->colorTint = post.colorTint;
        }
    }
}

void LightManager::UpdateEnvironmentProfile(float deltaTime) {
    // 補間開始からの秒数を使って照明と背景色を更新します。補間の要求が無ければ処理しません。
    if (!environmentBlendActive_) {
        return;
    }

    environmentBlendElapsed_ += (std::max)(0.0f, deltaTime);
    const float linearT = std::clamp(
        environmentBlendElapsed_ / (std::max)(environmentBlendDuration_, 0.0001f),
        0.0f,
        1.0f);
    const float t = Easing::SmoothStep(linearT);
    const auto lerp = [t](float from, float to) { return from + (to - from) * t; };
    const auto lerp3 = [&](const engine::Vector3& from, const engine::Vector3& to) {
        return engine::Vector3{ lerp(from.x, to.x), lerp(from.y, to.y), lerp(from.z, to.z) };
    };

    const auto lerp4 = [&](const engine::Vector4& from, const engine::Vector4& to) {
        return engine::Vector4{ lerp(from.x, to.x), lerp(from.y, to.y), lerp(from.z, to.z), lerp(from.w, to.w) };
    };

    const DirectionalLight& fromLight = environmentBlendFrom_.directionalLight;
    const DirectionalLight& toLight = environmentBlendTo_.directionalLight;
    directionalLightData_.color = lerp4(fromLight.color, toLight.color);
    directionalLightData_.direction = lerp3(fromLight.direction, toLight.direction);

    // 補間結果の光の方向がほぼゼロでない場合は正規化します。
    if (engine::Math::Length(directionalLightData_.direction) > 0.0001f) {
        directionalLightData_.direction = engine::Math::Normalize(directionalLightData_.direction);
    }

    directionalLightData_.intensity = lerp(fromLight.intensity, toLight.intensity);
    directionalLightData_.ambientColor = lerp3(fromLight.ambientColor, toLight.ambientColor);
    directionalLightData_.fogStart = lerp(fromLight.fogStart, toLight.fogStart);
    directionalLightData_.fogEnd = lerp(fromLight.fogEnd, toLight.fogEnd);
    directionalLightData_.fogColor = lerp3(fromLight.fogColor, toLight.fogColor);
    directionalLightData_.fogHeightMin = lerp(fromLight.fogHeightMin, toLight.fogHeightMin);
    directionalLightData_.fogHeightMax = lerp(fromLight.fogHeightMax, toLight.fogHeightMax);
    directionalLightData_.volumetricIntensity =
        lerp(fromLight.volumetricIntensity, toLight.volumetricIntensity);
    directionalLightData_.volumetricSteps = linearT < 0.5f
        ? fromLight.volumetricSteps
        : toLight.volumetricSteps;
    directionalLightData_.enableFog = linearT < 0.5f ? fromLight.enableFog : toLight.enableFog;

    sceneClearColor_ = lerp4(environmentBlendFrom_.clearColor, environmentBlendTo_.clearColor);
    shadowAreaSize_ = lerp(environmentBlendFrom_.shadowAreaSize, environmentBlendTo_.shadowAreaSize);
    ApplySceneClearColor();

    PostEffect* postEffect = PostEffect::GetInstance();

    // 描画補正のパラメーターを取得できる場合は、連続値を同じ進捗で更新します。
    if (PostEffect::Params* params = postEffect->GetParams()) {
        const EnvironmentPostSettings& from = environmentBlendFrom_.post;
        const EnvironmentPostSettings& to = environmentBlendTo_.post;
        params->threshold = lerp(from.threshold, to.threshold);
        params->bloomIntensity = lerp(from.bloomIntensity, to.bloomIntensity);
        params->spread = lerp(from.spread, to.spread);
        params->vignetteIntensity = lerp(from.vignetteIntensity, to.vignetteIntensity);
        params->vignettePower = lerp(from.vignettePower, to.vignettePower);
        params->chromaticAberration = lerp(from.chromaticAberration, to.chromaticAberration);
        params->filmGrainIntensity = lerp(from.filmGrainIntensity, to.filmGrainIntensity);
        params->lutIntensity = lerp(from.lutIntensity, to.lutIntensity);
        params->colorExposure = lerp(from.colorExposure, to.colorExposure);
        params->colorContrast = lerp(from.colorContrast, to.colorContrast);
        params->colorSaturation = lerp(from.colorSaturation, to.colorSaturation);
        params->colorTemperature = lerp(from.colorTemperature, to.colorTemperature);
        params->colorTint = lerp(from.colorTint, to.colorTint);
    }

    // 画像や解像度のように連続補間できない値は、補間の中間で一度だけ切り替えます。
    if (!environmentBlendDiscreteApplied_ && linearT >= 0.5f) {
        environmentBlendDiscreteApplied_ = true;
        skyboxEnabled_ = environmentBlendTo_.skyboxEnabled;

        // 新しいスカイボックスのパスが空でない場合に、画像も切り替えます。
        if (!environmentBlendTo_.skyboxTexturePath.empty()) {
            SetSkyboxTexturePath(environmentBlendTo_.skyboxTexturePath);
        }

        SetShadowMapResolution(environmentBlendTo_.shadowMapResolution);

        // 新しいLUT画像の指定がある場合に、色変換用の画像を切り替えます。
        if (!environmentBlendTo_.post.lutTexturePath.empty()) {
            postEffect->SetLUTTexturePath(environmentBlendTo_.post.lutTexturePath);
        }

        postEffect->SetBloomEnabled(environmentBlendTo_.post.bloomEnabled);
        postEffect->SetBloomQuality(
            static_cast<PostEffect::BloomQuality>(environmentBlendTo_.post.bloomQuality));
        // 描画補正のパラメーターが取れる場合、トーンマッピングの有効状態も切り替えます。
        if (PostEffect::Params* params = postEffect->GetParams()) {
            params->enableToneMapping = environmentBlendTo_.post.enableToneMapping;
        }
    }

    // 経過率が100%に達したら、目標値を確定して補間を終えます。
    if (linearT >= 1.0f) {
        const std::string completedProfileFile =
            currentEnvironmentProfileFile_;
        // 補間の丸め誤差を残さないよう目標値を即時適用し直し、この適用で消えるファイル名を戻します。
        ApplyEnvironmentProfile(environmentBlendTo_, 0.0f);
        currentEnvironmentProfileFile_ = completedProfileFile;
    }
}

float LightManager::GetEnvironmentProfileBlendProgress() const {
    // 補間していない状態は完了扱いにし、UIの進捗を1へ固定します。
    if (!environmentBlendActive_) {
        return 1.0f;
    }

    return std::clamp(
        environmentBlendElapsed_ / (std::max)(environmentBlendDuration_, 0.0001f),
        0.0f,
        1.0f);
}

} // namespace engine
