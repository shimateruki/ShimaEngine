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

namespace engine {

namespace {

// 非有限値と極端な繰り返し数を材質定数へ渡さないよう制限します。
// 有限な繰り返し数を0.01〜1000に収め、非有限値は等倍へ戻します。
float ClampTextureTiling(float value) {
    // 無効な入力はUVを壊さないよう等倍に戻す。
    if (!std::isfinite(value)) {
        return 1.0f;
    }

    return std::clamp(value, 0.01f, 1000.0f);
}

engine::Vector2 MakeAutoTilingFromScale(const engine::Vector3& scale) {
    // 材質設定のUV倍率はスケールの絶対値から大きい二軸を採用する。
    float a = std::abs(scale.x);
    float b = std::abs(scale.y);
    float c = std::abs(scale.z);

    // Y軸の絶対値がX軸より大きい場合は、最大値候補aへ移す。
    if (a < b)
        std::swap(a, b);
    // Z軸の絶対値が現在の最大候補aより大きければ、aを入れ替える。
    if (a < c)
        std::swap(a, c);
    // 残りの二値のうち大きい方をbに置き、最大の二値をUV倍率に使う。
    if (b < c)
        std::swap(b, c);

    return { (std::max)(a, 0.01f), (std::max)(b, 0.01f) };
}

}

// ========================================================================
// MeshRenderer マテリアル設定
// ------------------------------------------------------------------------
// 色、PBR値、法線・ORM・アルベドのテクスチャ、UVタイリング、環境反射を担当する。
// Inspectorから編集される見た目の値は、基本的にこのファイルへ集める。
// ========================================================================
// 演出前のGPU定数と、再計算に必要な入力色・合成方式を記録する。
MeshRenderer::MaterialState MeshRenderer::CaptureMaterialState() const {
    MaterialState state;
    // 材質定数を取得できる場合だけ復元可能な状態として記録する。
    if (materialData_) {
        state.parameters_ = *materialData_;
        state.authoringColor_ = materialColor_;
        state.blendMode_ = blendMode_;
        state.valid_ = true;
    }
    return state;
}

void MeshRenderer::RestoreMaterialState(const MaterialState& state) {
    // 復元先の材質定数が無い場合か、有効な退避が無い場合は適用しない。
    if (!materialData_ || !state.valid_) return;
    *materialData_ = state.parameters_;
    // 保持している入力色も先に戻し、現在の色空間設定でGPU用の色を計算し直します。
    SetColor(state.authoringColor_);
    SetBlendMode(state.blendMode_);
}

void MeshRenderer::SetPortalClip(const PortalClipSettings& settings) {
    // 初期化前はクリップ面を保持する材質定数が無いため、設定を適用しない。
    if (!materialData_) return;
    const auto finite = [](float value, float fallback) { return std::isfinite(value) ? value : fallback; };
    auto normal = settings.normal;
    const float length = engine::Math::Length(normal);
    // クリップ面までの距離が法線の長さに左右されないよう単位長へ揃えます。無効な法線は正面向きへ戻します。
    normal = std::isfinite(length) && length > 0.0001f ? normal * (1.0f / length) : engine::Vector3{0, 0, 1};
    materialData_->portalClipEnabled = settings.enabled ? 1.0f : 0.0f;
    materialData_->portalClipProgress = std::clamp(finite(settings.progress, 0.0f), 0.0f, 1.0f);
    materialData_->portalClipCenter = {finite(settings.center.x, 0), finite(settings.center.y, 0), finite(settings.center.z, 0)};
    materialData_->portalClipNormal = normal;
    materialData_->portalClipEdgeWidth = (std::max)(0.0001f, finite(settings.edgeWidth, 0.12f));
    materialData_->portalClipDissolve = (std::max)(0.0f, finite(settings.dissolve, 0.18f));
    // HDRの縁色は1を超える値も演出で使うため、上限を設けない。
    materialData_->portalClipColor = {finite(settings.color.x, 1), finite(settings.color.y, 1),
        finite(settings.color.z, 1), std::clamp(finite(settings.color.w, 1), 0.0f, 1.0f)};
}

void MeshRenderer::ClearPortalClip() {
    // 材質定数が準備できていれば、クリップの有効フラグを落とす。
    if (materialData_) materialData_->portalClipEnabled = 0.0f;
}

void MeshRenderer::SetMaterialTime(float time) {
    // 書き込み先があり、有限の秒数が渡された場合だけ材質の時刻を設定する。
    if (materialData_ && std::isfinite(time)) materialData_->time = time;
}

void MeshRenderer::AdvanceMaterialTime(float deltaTime) {
    // 材質定数があり、経過秒数が有限で正の場合だけ材質の時計を進める。
    if (materialData_ && std::isfinite(deltaTime) && deltaTime > 0.0f)
        SetMaterialTime(materialData_->time + deltaTime);
}

void MeshRenderer::SetShininess(float shininess) {
    // 材質定数がある場合は、有限な光沢指数だけを採用し、負の値を0へ補正する。
    if (materialData_ && std::isfinite(shininess)) materialData_->shininess = (std::max)(0.0f, shininess);
}

void MeshRenderer::SetSelectedLighting(int32_t type) {
    // 材質定数が準備できていれば、描画で使う照明方式を格納する。
    if (materialData_) materialData_->selectedLighting = type;
}

void MeshRenderer::SetColor(const engine::Vector4& color) {
    // 保存と編集用には入力色を保持する。GPUには現在の作業色空間へ変換した値を書く。
    materialColor_ = color;
    // 材質定数が準備できている場合に限り、作業色空間に変換した色をGPUへ渡す。
    if (materialData_)
        materialData_->color = engine::colorSpace::AuthoringToWorking(materialColor_);
}

void MeshRenderer::SetMaterialType(int32_t type) {
    // 材質定数が準備できている場合だけ、描画方式の選択を反映する。
    if (materialData_)
        materialData_->materialType = type;
}

void MeshRenderer::SetIntensity(float intensity) {
    // 平行光の定数が準備できている場合だけ、強さを上書きする。
    if (directionalLightData_)
        directionalLightData_->intensity = intensity;
}

engine::Vector4 MeshRenderer::GetColor() const {
    return materialColor_;
}

int32_t MeshRenderer::GetMaterialType() const {
    return materialData_ ? materialData_->materialType : 0;
}

void MeshRenderer::SetMetallic(float metallic) {
    // 材質定数が準備できていれば、金属度をシェーダーへ渡す。
    if (materialData_)
        materialData_->metallic = metallic;
}

void MeshRenderer::SetRoughness(float roughness) {
    // 材質定数が準備できていれば、粗さをシェーダーへ渡す。
    if (materialData_)
        materialData_->roughness = roughness;
}

float MeshRenderer::GetMetallic() const {
    return materialData_ ? materialData_->metallic : 0.0f;
}

float MeshRenderer::GetRoughness() const {
    return materialData_ ? materialData_->roughness : 0.3f;
}

void MeshRenderer::SetEnableNormalMap(bool enable) {
    // 材質定数が準備できている場合だけ、法線マップの有効値を0か1で書く。
    if (materialData_)
        materialData_->enableNormalMap = enable ? 1 : 0;
}

bool MeshRenderer::GetEnableNormalMap() const {
    return materialData_ ? (materialData_->enableNormalMap == 1) : false;
}

void MeshRenderer::SetNormalMap(const std::string& texturePath) {
    // 法線テクスチャは色変換せず線形空間で読み、解除時は旧ハンドルを使わない。
    normalMapPath_ = texturePath;

    // パスが指定されている場合は、法線の成分を線形値のままロードする。
    if (!texturePath.empty()) {
        normalMapHandle_ = engine::TextureManager::GetInstance()->Load(texturePath, engine::TextureManager::TextureColorSpace::kLinear);
    // 空パスを渡されたら、以前の法線マップのハンドルを解除する。
    } else {
        normalMapHandle_ = 0;
    }
}

void MeshRenderer::SetOrmMap(const std::string& texturePath) {
    // ORM の各成分は色ではなく係数なので線形空間で読み込む。
    ormMapPath_ = texturePath;

    // パスが指定されている場合は、ORMの各係数を線形値のままロードする。
    if (!texturePath.empty()) {
        ormMapHandle_ = engine::TextureManager::GetInstance()->Load(texturePath, engine::TextureManager::TextureColorSpace::kLinear);
    // 空パスを渡されたら、以前のORMのハンドルを解除する。
    } else {
        ormMapHandle_ = 0;
    }
}


void MeshRenderer::SetTexture(const std::string& texturePath) {
    // ベースカラーだけは sRGB として読み込み、空パスで参照を解除する。
    texturePath_ = texturePath;

    // パスが指定されている場合は、ベースカラーをsRGB画像としてロードする。
    if (!texturePath.empty()) {
        textureHandle_ = engine::TextureManager::GetInstance()->Load(texturePath, engine::TextureManager::TextureColorSpace::kSRGB);
    // 空パスを渡されたら、以前のベースカラーのハンドルを解除する。
    } else {
        textureHandle_ = 0;
    }
}

void MeshRenderer::SetTextureTiling(const engine::Vector2& tiling) {
    // 無効な繰り返し率を補正してから UV 行列へ反映する。
    textureTiling_ = {
        ClampTextureTiling(tiling.x),
        ClampTextureTiling(tiling.y)
    };

    UpdateUvTransform();
}

void MeshRenderer::SetAutoTextureTiling(bool enabled) {
    autoTextureTiling_ = enabled;
    UpdateUvTransform();
}

void MeshRenderer::UpdateUvTransform() {
    // 材質定数がまだ無ければ、UV行列への書き込みを見送る。
    if (!materialData_) {
        return;
    }

    engine::Vector2 effectiveTiling = {
        ClampTextureTiling(textureTiling_.x),
        ClampTextureTiling(textureTiling_.y)
    };

    // 自動タイリングが有効でスケールを取得できる場合に限り、手動倍率に大きさ由来の倍率を掛ける。
    if (autoTextureTiling_ && transform_) {
        engine::Vector2 scaleTiling = MakeAutoTilingFromScale(transform_->scale);
        effectiveTiling.x *= scaleTiling.x;
        effectiveTiling.y *= scaleTiling.y;
    }

    // 画像を横・縦に何回繰り返すかをUV座標へ掛けます。元メッシュのUVそのものは変更しません。
    materialData_->uvTransform = engine::Math::MakeScaleMatrix({ effectiveTiling.x, effectiveTiling.y, 1.0f });
}

void MeshRenderer::SetEnableEnvMap(bool enable) {
    // 材質定数が準備できている場合だけ、環境マップの有効値を0か1で書く。
    if (materialData_)
        materialData_->enableEnvMap = enable ? 1 : 0;
}

bool MeshRenderer::GetEnableEnvMap() const {
    return materialData_ ? (materialData_->enableEnvMap == 1) : false;
}

void MeshRenderer::SetEnvIntensity(float intensity) {
    // 材質定数が準備できていれば、環境反射の強さを反映する。
    if (materialData_)
        materialData_->envIntensity = intensity;
}

float MeshRenderer::GetEnvIntensity() const {
    return materialData_ ? materialData_->envIntensity : 1.0f;
}

void MeshRenderer::SetEmissive(float emissive) {
    // 材質定数が準備できていれば、発光の設定値を反映する。
    if (materialData_)
        materialData_->emissive = emissive;
}

float MeshRenderer::GetEmissive() const {
    return materialData_ ? materialData_->emissive : 1.0f;
}

} // namespace engine
