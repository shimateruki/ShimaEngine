#include <cstdint>
#include "MeshRenderer.h"
#include <numbers>
#include "engine/graphics/3d/material/MaterialRenderPolicy.h"
#include "DirectXCommon.h"
#include "ModelManager.h"
#include "CameraManager.h"
#include "LightManager.h"
#include "TextureManager.h"
#include "DebugConsole.h"
#include <cassert>
#include <SrvManager.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace engine {

namespace {
namespace fs = std::filesystem;

using materialRenderPolicy::SpecialKind;

/// <summary>ゲームとプレビューで共有する特殊マテリアルの描画コマンド設定関数と、代理メッシュを使うかどうかを保持する。</summary>
struct SpecialMaterialDrawSettings {
    void (engine::Object3DCommon::*setGraphicsCommand)() = nullptr;
    bool useProxyModel = false;
};

// ゲームとプレビューで同じPSOと代理メッシュの指定を使う。
constexpr SpecialMaterialDrawSettings GetSpecialMaterialDrawSettings(SpecialKind kind) {
    using engine::Object3DCommon;
    switch (kind) {
    // 各エフェクトがモデルを使うか、描画専用の形状を使うかを決める。
    case SpecialKind::kLaser: return {&Object3DCommon::SetLaserGraphicsCommand};
    case SpecialKind::kSlimeGel: return {&Object3DCommon::SetSlimeGelGraphicsCommand};
    case SpecialKind::kShockwave: return {&Object3DCommon::SetShockwaveGraphicsCommand};
    case SpecialKind::kLiquidContact: return {&Object3DCommon::SetLiquidContactGraphicsCommand};
    case SpecialKind::kDamageCrack: return {&Object3DCommon::SetDamageCrackGraphicsCommand};
    case SpecialKind::kUpdraft: return {&Object3DCommon::SetUpdraftGraphicsCommand, true};
    case SpecialKind::kStunBind: return {&Object3DCommon::SetStunBindGraphicsCommand, true};
    case SpecialKind::kCrownUnlock: return {&Object3DCommon::SetCrownUnlockGraphicsCommand, true};
    case SpecialKind::kPoisonSpore: return {&Object3DCommon::SetPoisonSporeGraphicsCommand, true};
    case SpecialKind::kCloud: return {&Object3DCommon::SetCloudGraphicsCommand, true};
    case SpecialKind::kWindOrb: return {&Object3DCommon::SetWindOrbGraphicsCommand};
    // 個別の描画関数が担当する材質と通常材質は、共通設定を使わない。
    default: return {};
    }
}

// 特殊材質の三つのテクスチャスロットに渡す SRV 番号。画像自体の所有者は TextureManager。
struct BakedShaderTextureSet {
    uint32_t slot0 = 0;
    uint32_t slot1 = 0;
    uint32_t slot2 = 0;
};

// 水・炎・ポータル用の参照セットと代替画像を保持し、同じキャッシュでの再読込を避ける。
struct BakedShaderTextureCache {
    BakedShaderTextureSet fallback;
    BakedShaderTextureSet water;
    BakedShaderTextureSet fire;
    BakedShaderTextureSet gatePortal;
    bool loaded = false;
};

BakedShaderTextureCache& GetBakedShaderTextureCache() {
    // テクスチャ管理側の SRV 番号を一度読み、全材質で再利用する。
    static BakedShaderTextureCache sCache;
    return sCache;
}

// 焼き込み画像が欠けている場合は白画像へ退避してSRVを必ず取得します。
uint32_t LoadTextureWithFallback(const char* texturePath, bool isLinearData = true) {
    // 焼き込み画像が欠けていても白画像へ退避し、SRV の未束縛を避ける。
    const char* fallbackPath = "Resources/sprite/common/white.dds";
    const char* pathToLoad = fs::exists(texturePath) ? texturePath : fallbackPath;
    return engine::TextureManager::GetInstance()->Load(pathToLoad,
        isLinearData ? engine::TextureManager::TextureColorSpace::kLinear : engine::TextureManager::TextureColorSpace::kSRGB);
}

// 特殊材質が共有するマスク・色画像を色空間別に一度だけ読み込みます。
void EnsureBakedShaderTexturesLoaded() {
    // マスクは線形値、色画像は sRGB として読み込み、シェーダーへの解釈を揃える。
    BakedShaderTextureCache& cache = GetBakedShaderTextureCache();

    // 読み込み済みのテクスチャセットは再利用する。
    if (cache.loaded) {
        return;
    }

    const uint32_t fallback = LoadTextureWithFallback("Resources/sprite/common/white.dds");
    cache.fallback = { fallback, fallback, fallback };
    cache.water = {
        LoadTextureWithFallback("Resources/texture/BakedShader/water_foam_mask.dds"),
        LoadTextureWithFallback("Resources/texture/BakedShader/water_flow_noise.dds"),
        LoadTextureWithFallback("Resources/texture/BakedShader/water_surface_stylized.dds", false)
    };

    cache.fire = {
        LoadTextureWithFallback("Resources/texture/BakedShader/fire_flame_mask.dds"),
        LoadTextureWithFallback("Resources/texture/BakedShader/fire_orb_mask.dds"),
        fallback
    };

    cache.gatePortal = {
        LoadTextureWithFallback("Resources/texture/BakedShader/gate_swirl_mask.dds"),
        fallback,
        fallback
    };

    cache.loaded = true;
}

const BakedShaderTextureSet& GetDefaultBakedShaderTextures() {
    return GetBakedShaderTextureCache().fallback;
}

const BakedShaderTextureSet& GetWaterBakedShaderTextures() {
    return GetBakedShaderTextureCache().water;
}

const BakedShaderTextureSet& GetFireBakedShaderTextures() {
    return GetBakedShaderTextureCache().fire;
}

const BakedShaderTextureSet& GetGatePortalBakedShaderTextures() {
    return GetBakedShaderTextureCache().gatePortal;
}

// 特殊材質用の三つのテクスチャSRVを共通ルートスロットへ束縛します。
void BindBakedShaderTextures(ID3D12GraphicsCommandList* commandList, const BakedShaderTextureSet& textures) {
    // 水系の共有ルートにある t2～t4 相当の三スロットを常に埋める。
    EnsureBakedShaderTexturesLoaded();
    engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 5, textures.slot0);
    engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 6, textures.slot1);
    engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 7, textures.slot2);
}

}

// ========================================================================
// MeshRenderer 特殊マテリアル描画
// ------------------------------------------------------------------------
// 水、マグマ、炎、レーザー、ポータルなど専用パイプラインの描画を担当する。
// 焼き込みテクスチャと代理モデルもここに集約し、特殊表現の追加場所を明確にする。
// ========================================================================
// 通常画面の遮蔽状態と描画対象を確定し、必要なスキニングを記録してから材質の描画パイプラインを束縛する。
void MeshRenderer::Draw(ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource) {
    UpdateCameraVisibility(CameraManager::GetInstance()->GetActiveCamera());
    Model* drawModel = ResolveDrawModel();
    // モデル、3D描画の共通状態、頂点・インデックス以外の描画用バッファが揃っている場合だけ準備する。
    if (!drawModel || !common_ || !HasRequiredBuffers())
        return;
    drawModel->PrepareComputeSkinning();
    common_->SetGraphicsCommand();
    common_->SetMaterialPipelineState(blendMode_, GetMaterialType(), shaderMaterialId_);
    ID3D12GraphicsCommandList* commandList = common_->GetDxCommon()->GetCommandList();

    // [11] 影用のWVP行列 (b1) をセット
    // シャドウマップが用意されている場合だけ、シャドウ用の描画データを更新する。
    if (shadowWvpResource_) {
        commandList->SetGraphicsRootConstantBufferView(11, shadowWvpResource_->GetGPUVirtualAddress());
    }

    // [12] シャドウマップのテクスチャ (t5) をセット
    uint32_t shadowMapSrvHandle = common_->GetDxCommon()->GetShadowMapSrvHandle();
    engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 12, shadowMapSrvHandle);

    // ModelのDrawを呼ぶ
    drawModel->Draw(
        wvpResource_.Get(),
        isUiPreview_ && previewDirectionalLightResource_
            ? previewDirectionalLightResource_ : LightManager::GetInstance()->GetDirectionalLightResource(),
        cameraResource_.Get(),
        pointLightResource,
        spotLightResource,
        materialResource_.Get(), normalMapHandle_, ormMapHandle_, textureHandle_,
        1, 0, meshDrawIndex_
    );
}

void MeshRenderer::DrawForCamera(engine::Camera* camera, ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource, std::int32_t previewBufferIndex) {
    // 別カメラ用の定数バッファを使い、通常画面の wvp と視点情報を上書きしない。
    Model* drawModel = ResolveDrawModel();
    ID3D12Resource* previewWvpResource = nullptr;
    ID3D12Resource* previewCameraResource = nullptr;
    // プレビューに必要な描画リソースは、プレビュー用カメラと合わせて確認する。
    if (!drawModel || !common_ || !HasRequiredBuffers() ||
        !PreparePreviewCameraData(camera, previewBufferIndex, previewWvpResource, previewCameraResource)) {
        return;
    }

    drawModel->PrepareComputeSkinning();
    common_->SetGraphicsCommand();
    common_->SetMaterialPipelineState(blendMode_, GetMaterialType(), shaderMaterialId_);
    ID3D12GraphicsCommandList* commandList = common_->GetDxCommon()->GetCommandList();

    // シャドウマップがある場合は、プレビューでもシャドウ用の描画データを更新する。
    if (shadowWvpResource_) {
        commandList->SetGraphicsRootConstantBufferView(11, shadowWvpResource_->GetGPUVirtualAddress());
    }

    const uint32_t shadowMapSrvHandle = common_->GetDxCommon()->GetShadowMapSrvHandle();
    engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 12, shadowMapSrvHandle);

    drawModel->Draw(
        previewWvpResource,
        isUiPreview_ && previewDirectionalLightResource_
            ? previewDirectionalLightResource_ : LightManager::GetInstance()->GetDirectionalLightResource(),
        previewCameraResource,
        pointLightResource,
        spotLightResource,
        materialResource_.Get(), normalMapHandle_, ormMapHandle_, textureHandle_,
        1, 0, meshDrawIndex_
    );
}

bool MeshRenderer::PreparePreviewCameraData(engine::Camera* camera, std::int32_t previewBufferIndex, ID3D12Resource*& wvpResource, ID3D12Resource*& cameraResource) {
    // プレビュー番号に対応する独立バッファへ、そのカメラの行列と視点を記録する。
    wvpResource = nullptr;
    cameraResource = nullptr;

    // カメラを指定できないプレビューは呼び出し元で中止できるようにする。
    if (!camera) {
        return false;
    }

    // プレビューごとの定数に書き、番号が範囲外なら利用可能な最も近い枠を使う。
    const std::int32_t safePreviewIndex = std::clamp(previewBufferIndex, 0, kPreviewBufferCount - 1);
    wvpResource = previewWvpResources_[safePreviewIndex].Get();
    cameraResource = previewCameraResources_[safePreviewIndex].Get();
    TransformationMatrix* previewWvpData = previewWvpData_[safePreviewIndex];
    CameraForGpu* previewCameraData = previewCameraData_[safePreviewIndex];

    // プレビュー用の描画バッファを取得できなければ描画先の準備が成立しない。
    if (!wvpResource || !cameraResource || !previewWvpData || !previewCameraData) {
        return false;
    }

    engine::Math math;
    const engine::Matrix4x4 worldMatrix = BuildRenderWorldMatrix();
    previewWvpData->wvp = math.Multiply(worldMatrix, camera->GetViewProjectionMatrix());
    previewWvpData->world = worldMatrix;
    previewWvpData->worldInverseTranspose = GetCachedWorldInverseTranspose(worldMatrix);
    // プレビューには本編の遮蔽マスクを持ち込まないよう、定数を初期化して視点位置だけを設定する。
    *previewCameraData = {};
    previewCameraData->worldPosition = camera->GetEye();
    return true;
}

void MeshRenderer::DrawSpecialMaterial(uint32_t depthSrvHandle, uint32_t colorSrvHandle) {
    UpdateCameraVisibility(CameraManager::GetInstance()->GetActiveCamera());
    // 通常画面の材質選択を描画コンポーネントへ集約する。各描画関数は既存のPSOを使う。
    const auto kind = materialRenderPolicy::GetSpecialKind(GetMaterialType());
    const auto settings = GetSpecialMaterialDrawSettings(kind);
    // 共通設定に登録されている材質は、このあとに続く個別描画の分岐を通さない。
    if (settings.setGraphicsCommand) {
        DrawSpecialMaterial(depthSrvHandle, colorSrvHandle, settings.setGraphicsCommand, settings.useProxyModel);
        return;
    }

    switch (kind) {
    case SpecialKind::kWater: DrawWater(depthSrvHandle, colorSrvHandle);
    break;

    case SpecialKind::kMagma: DrawMagma(depthSrvHandle, colorSrvHandle);
    break;

    case SpecialKind::kIce: DrawIce(depthSrvHandle, colorSrvHandle);
    break;

    case SpecialKind::kFire: DrawFire(depthSrvHandle, colorSrvHandle);
    break;

    case SpecialKind::kGatePortal: DrawGatePortal(depthSrvHandle, colorSrvHandle);
    break;

    default: break;
    }
}

void MeshRenderer::DrawSpecialMaterialForCamera(std::int32_t materialType, engine::Camera* camera, uint32_t depthSrvHandle, uint32_t colorSrvHandle, std::int32_t previewBufferIndex) {
    ID3D12Resource* previewWvpResource = nullptr;
    ID3D12Resource* previewCameraResource = nullptr;
    // プレビュー用の描画リソースが揃わない場合はコマンドを発行しない。
    if (!common_ || !HasRequiredBuffers() ||
        !PreparePreviewCameraData(camera, previewBufferIndex, previewWvpResource, previewCameraResource)) {
        return;
    }

    const auto kind = materialRenderPolicy::GetSpecialKind(materialType);
    const auto settings = GetSpecialMaterialDrawSettings(kind);
    // 共通設定を持つ材質は、プレビューのWVPを渡して共通の描画経路に任せる。
    if (settings.setGraphicsCommand) {
        DrawSpecialMaterialWithWvp(previewWvpResource, depthSrvHandle, colorSrvHandle,
            settings.setGraphicsCommand, settings.useProxyModel);
        return;
    }

    switch (kind) {
    case SpecialKind::kWater:
    {
        // 別カメラの描画で通常画面の定数を上書きしないようにします。
        const std::int32_t index = std::clamp(previewBufferIndex, 0, kPreviewBufferCount - 1);
        // 水系のプレビューは、元の材質値とプレビュー専用の定数バッファが揃った場合だけ描く。
        if (!waterParamData_ || !previewWaterData_[index])
            return;
        *previewWaterData_[index] = *waterParamData_;
        previewWaterData_[index]->cameraWorldPosition = camera->GetEye();
        previewWaterData_[index]->waterInverseViewProjection = camera->GetInverseViewProjectionMatrix();
        DrawWaterWithWvp(previewWvpResource, depthSrvHandle, colorSrvHandle, previewWaterResources_[index].Get());
        break;
    }

    case SpecialKind::kMagma:
        DrawMagmaWithWvp(previewWvpResource, depthSrvHandle, colorSrvHandle);
        break;

    case SpecialKind::kIce:
        DrawIceWithWvp(previewWvpResource, depthSrvHandle, colorSrvHandle);
        break;

    case SpecialKind::kFire:
        DrawFireWithWvp(previewWvpResource, depthSrvHandle, colorSrvHandle);
        break;

    case SpecialKind::kGatePortal:
    {
        // プレビューでは別の定数を使い、実画面の透過円を持ち込まない。
        const std::int32_t index = std::clamp(previewBufferIndex, 0, kPreviewBufferCount - 1);
        // ゲートのプレビューも、実環境の材質値を複製できる場合だけ描く。
        if (!waterParamData_ || !previewWaterData_[index]) return;
        *previewWaterData_[index] = *waterParamData_;
        previewWaterData_[index]->visibilityParameters = {};
        previewWaterData_[index]->foregroundBarrierCenter = {};
        previewWaterData_[index]->foregroundBarrierParameters = {};
        previewWaterData_[index]->cameraWorldPosition = camera->GetEye();
        previewWaterData_[index]->waterInverseViewProjection = camera->GetInverseViewProjectionMatrix();
        DrawSpecialMaterialWithWvp(previewWvpResource, depthSrvHandle, colorSrvHandle,
            &engine::Object3DCommon::SetGatePortalGraphicsCommand, true, 1, previewWaterResources_[index].Get());
        break;
    }

    default:
        break;
    }
}

void MeshRenderer::DrawWater(uint32_t depthSrvHandle, uint32_t colorSrvHandle) {
    // 通常画面の wvp を使って水材質の共通描画経路へ渡す。
    DrawWaterWithWvp(wvpResource_.Get(), depthSrvHandle, colorSrvHandle);
}

// 描画対象と水面描画に必要な共通状態をそろえられなければ描画しない。
void MeshRenderer::DrawWaterWithWvp(ID3D12Resource* wvpResource, uint32_t depthSrvHandle, uint32_t colorSrvHandle, ID3D12Resource* waterResource) {
    Model* drawModel = ResolveDrawModel();
    // 水面の描画にはモデルと共通の描画バッファに加え、材質値とWVPの定数バッファが必要になる。
    if (!drawModel || !common_ || !HasRequiredBuffers() || !waterParamResource_ || !wvpResource)
        return;

    // 0/1は従来、2/3はイラスト調。それぞれ偶数が専用グリッドです。
    // 波で頂点を動かす設定だけを専用格子へ差し替え、他の水面は配置したメッシュ形状を保持します。
    const bool useWaterGrid = !waterParamData_ || waterParamData_->effectType < 0.5f ||
        (waterParamData_->effectType >= 1.5f && waterParamData_->effectType < 2.5f);
    // 水面用のモードが専用グリッドを使うときだけ、未生成ならここで作成する。
    if (useWaterGrid && !waterProxyModel_) {
        InitializeWaterProxyModel();
    }

    common_->SetWaterGraphicsCommand();
    ID3D12GraphicsCommandList* commandList = common_->GetDxCommon()->GetCommandList();

    commandList->SetGraphicsRootConstantBufferView(0, wvpResource->GetGPUVirtualAddress());
    commandList->SetGraphicsRootConstantBufferView(1, (waterResource ? waterResource : waterParamResource_.Get())->GetGPUVirtualAddress());
    commandList->SetGraphicsRootConstantBufferView(2, materialResource_->GetGPUVirtualAddress());
    engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 3, depthSrvHandle);

    // ルートスロット4に、屈折などで参照する描画済みのシーン色を設定する。
    //  4番目にカラーテクスチャをセット
    engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 4, colorSrvHandle);
    BindBakedShaderTextures(commandList, GetWaterBakedShaderTextures());

    drawModel = (useWaterGrid && waterProxyModel_) ? waterProxyModel_.get() : drawModel;
    drawModel->DrawMeshOnly((useWaterGrid && waterProxyModel_) ? -1 : meshDrawIndex_);
}

void MeshRenderer::DrawMagma(uint32_t depthSrvHandle, uint32_t colorSrvHandle) {
    // 通常画面の wvp を使って不透明マグマの共通描画経路へ渡す。
    DrawMagmaWithWvp(wvpResource_.Get(), depthSrvHandle, colorSrvHandle);
}

void MeshRenderer::DrawMagmaWithWvp(ID3D12Resource* wvpResource, uint32_t depthSrvHandle, uint32_t colorSrvHandle) {
    // 水と同じ定数配置でマグマ専用 PSO を使い、深度と背景色を渡す。
    Model* drawModel = ResolveDrawModel();
    // モデルとマグマ描画に必要な共通状態が揃わなければ描画しない。
    if (!drawModel || !common_ || !HasRequiredBuffers() || !waterParamResource_ || !wvpResource)
        return;

    common_->SetMagmaGraphicsCommand();
    ID3D12GraphicsCommandList* commandList = common_->GetDxCommon()->GetCommandList();

    commandList->SetGraphicsRootConstantBufferView(0, wvpResource->GetGPUVirtualAddress());
    commandList->SetGraphicsRootConstantBufferView(1, waterParamResource_->GetGPUVirtualAddress());
    commandList->SetGraphicsRootConstantBufferView(2, materialResource_->GetGPUVirtualAddress());
    engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 3, depthSrvHandle);
    engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 4, colorSrvHandle);
    BindBakedShaderTextures(commandList, GetDefaultBakedShaderTextures());

    drawModel->DrawMeshOnly(meshDrawIndex_);
}

void MeshRenderer::DrawIce(uint32_t depthSrvHandle, uint32_t colorSrvHandle) {
    // 通常画面の wvp を使って氷材質の共通描画経路へ渡す。
    DrawIceWithWvp(wvpResource_.Get(), depthSrvHandle, colorSrvHandle);
}

void MeshRenderer::DrawIceWithWvp(ID3D12Resource* wvpResource, uint32_t depthSrvHandle, uint32_t colorSrvHandle) {
    // 半透明の氷も水系ルートの定数とテクスチャ配置を共用する。
    Model* drawModel = ResolveDrawModel();
    // モデルと氷描画に必要な共通状態が揃わなければ描画しない。
    if (!drawModel || !common_ || !HasRequiredBuffers() || !waterParamResource_ || !wvpResource)
        return;

    common_->SetIceGraphicsCommand(); 

    // (以下、DrawMagmaと全く同じ)
    ID3D12GraphicsCommandList* commandList = common_->GetDxCommon()->GetCommandList();
    commandList->SetGraphicsRootConstantBufferView(0, wvpResource->GetGPUVirtualAddress());
    commandList->SetGraphicsRootConstantBufferView(1, waterParamResource_->GetGPUVirtualAddress());
    commandList->SetGraphicsRootConstantBufferView(2, materialResource_->GetGPUVirtualAddress());
    engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 3, depthSrvHandle);
    engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 4, colorSrvHandle);
    BindBakedShaderTextures(commandList, GetDefaultBakedShaderTextures());
    drawModel->DrawMeshOnly(meshDrawIndex_);
}

void MeshRenderer::DrawFire(uint32_t depthSrvHandle, uint32_t colorSrvHandle) {
    // 通常画面の wvp を使って炎材質の共通描画経路へ渡す。
    DrawFireWithWvp(wvpResource_.Get(), depthSrvHandle, colorSrvHandle);
}

void MeshRenderer::DrawFireWithWvp(ID3D12Resource* wvpResource, uint32_t depthSrvHandle, uint32_t colorSrvHandle) {
    Model* drawModel = ResolveDrawModel();
    // モデルと炎描画に必要な共通状態が揃わなければ描画しない。
    if (!drawModel || !common_ || !HasRequiredBuffers() || !waterParamResource_ || !wvpResource)
        return;

    // かがり火、まとい炎、火炎放射、炎柱は閉じた立体内の密度を積分します。
    const bool useVolumetricProxy = waterParamData_ &&
        ((waterParamData_->effectType >= 3.5f && waterParamData_->effectType < 7.5f) ||
         (waterParamData_->effectType >= 1.5f && waterParamData_->effectType < 2.5f));
    // 体積を持つ炎は立方体、それ以外の炎は平面を使う。
    if (useVolumetricProxy) {
        // 体積用の立方体は、必要になった最初の描画で生成する。
        if (!volumetricFireProxyModel_) {
            InitializeVolumetricFireProxyModel();
        }
    // 平面を使うモードで板形状が無ければ生成する。
    } else if (!fireProxyModel_) {
        InitializeFireProxyModel();
    }

    common_->SetFireGraphicsCommand();
    ID3D12GraphicsCommandList* commandList = common_->GetDxCommon()->GetCommandList();
    commandList->SetGraphicsRootConstantBufferView(0, wvpResource->GetGPUVirtualAddress());
    commandList->SetGraphicsRootConstantBufferView(1, waterParamResource_->GetGPUVirtualAddress());
    commandList->SetGraphicsRootConstantBufferView(2, materialResource_->GetGPUVirtualAddress());
    engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 3, depthSrvHandle);
    engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 4, colorSrvHandle);
    BindBakedShaderTextures(commandList, GetFireBakedShaderTextures());
    // 立体型には箱、その他には平面を使います。代理形状を作れない場合でも元モデルから描画します。
    Model* proxyModel = useVolumetricProxy ? volumetricFireProxyModel_.get() : fireProxyModel_.get();
    drawModel = proxyModel ? proxyModel : drawModel;
    drawModel->DrawMeshOnly(proxyModel ? -1 : meshDrawIndex_);
}

void MeshRenderer::DrawSpecialMaterial(uint32_t depthSrvHandle, uint32_t colorSrvHandle, void (engine::Object3DCommon::*setGraphicsCommand)(), bool useProxyModel, std::int32_t bakedTextureMode, ID3D12Resource* effectResource) {
    // 通常画面用の wvp を添えて各特殊材質の共有描画処理へ渡す。
    DrawSpecialMaterialWithWvp(wvpResource_.Get(), depthSrvHandle, colorSrvHandle, setGraphicsCommand, useProxyModel, bakedTextureMode, effectResource);
}

void MeshRenderer::DrawSpecialMaterialWithWvp(
    ID3D12Resource* wvpResource,
    uint32_t depthSrvHandle,
    uint32_t colorSrvHandle,
    void (engine::Object3DCommon::*setGraphicsCommand)(),
    bool useProxyModel,
    std::int32_t bakedTextureMode, ID3D12Resource* effectResource) {
    // 代理形状が必要な演出だけ差し替え、定数と SRV のルート番号は全材質で統一する。
    Model* drawModel = ResolveDrawModel();
    // モデル、描画共通状態、コマンドリストが揃っている場合だけ描画する。
    if (!drawModel || !common_ || !HasRequiredBuffers() || !waterParamResource_ || !setGraphicsCommand || !wvpResource)
        return;

    Model* proxyModel = nullptr;

    // 共通設定が専用形状を指定した材質だけ、モデルのメッシュを置き換える。
    if (useProxyModel) {
        // ベイク済みゲート用のテクスチャセットでは、ゲート専用の形状を選択する。
        if (bakedTextureMode == 1) {
            // ポータル用の素材は通常の前後平面と、結界用の円筒を設定で選びます。
            const bool cylindricalBarrier = waterParamData_ && waterParamData_->effectType >= 3.5f;

            // ゲートのエフェクト種別が3.5以上なら円筒形状を使う。
            if (cylindricalBarrier) {
                // 円筒形状が未生成なら、このフレームの描画前に作成する。
                if (!gateBarrierCylinderProxyModel_) {
                    InitializeGateBarrierCylinderProxyModel();
                }

                proxyModel = gateBarrierCylinderProxyModel_.get();
            // 円筒を使わないゲートには、二層の平面形状を使用する。
            } else {
                // ゲート用の平面が未生成なら、このフレームの描画前に作成する。
                if (!gatePortalProxyModel_) {
                    InitializeGatePortalProxyModel();
                }

                proxyModel = gatePortalProxyModel_.get();
            }
        // ゲート以外の専用形状には、炎と共用する平面を使う。
        } else {
            // 共用する平面は、最初に必要になったときに作成する。
            if (!fireProxyModel_) {
                InitializeFireProxyModel();
            }

            proxyModel = fireProxyModel_.get();
        }
    }

    (common_->*setGraphicsCommand)();
    ID3D12GraphicsCommandList* commandList = common_->GetDxCommon()->GetCommandList();
    commandList->SetGraphicsRootConstantBufferView(0, wvpResource->GetGPUVirtualAddress());
    commandList->SetGraphicsRootConstantBufferView(1, (effectResource ? effectResource : waterParamResource_.Get())->GetGPUVirtualAddress());
    commandList->SetGraphicsRootConstantBufferView(2, materialResource_->GetGPUVirtualAddress());
    engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 3, depthSrvHandle);
    engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 4, colorSrvHandle);
    const BakedShaderTextureSet& bakedTextures =
        (bakedTextureMode == 1) ? GetGatePortalBakedShaderTextures() : GetDefaultBakedShaderTextures();
    BindBakedShaderTextures(commandList, bakedTextures);
    // 材質個別のパラメーターを使えない場合は、共通の既定値を設定する。
    drawModel = (useProxyModel && proxyModel) ? proxyModel : drawModel;
    drawModel->DrawMeshOnly((useProxyModel && proxyModel) ? -1 : meshDrawIndex_);
}

void MeshRenderer::DrawLaser(uint32_t depthSrvHandle, uint32_t colorSrvHandle) {
    const auto settings = GetSpecialMaterialDrawSettings(SpecialKind::kLaser);
    DrawSpecialMaterial(depthSrvHandle, colorSrvHandle, settings.setGraphicsCommand, settings.useProxyModel);
}

void MeshRenderer::DrawShockwave(uint32_t depthSrvHandle, uint32_t colorSrvHandle) {
    const auto settings = GetSpecialMaterialDrawSettings(SpecialKind::kShockwave);
    DrawSpecialMaterial(depthSrvHandle, colorSrvHandle, settings.setGraphicsCommand, settings.useProxyModel);
}

void MeshRenderer::DrawStunBind(uint32_t depthSrvHandle, uint32_t colorSrvHandle) {
    const auto settings = GetSpecialMaterialDrawSettings(SpecialKind::kStunBind);
    DrawSpecialMaterial(depthSrvHandle, colorSrvHandle, settings.setGraphicsCommand, settings.useProxyModel);
}

void MeshRenderer::DrawCrownUnlock(uint32_t depthSrvHandle, uint32_t colorSrvHandle) {
    const auto settings = GetSpecialMaterialDrawSettings(SpecialKind::kCrownUnlock);
    DrawSpecialMaterial(depthSrvHandle, colorSrvHandle, settings.setGraphicsCommand, settings.useProxyModel);
}

void MeshRenderer::DrawGatePortal(uint32_t depthSrvHandle, uint32_t colorSrvHandle) {
    // ポータルは専用焼き込みマスクと代理形状を組み合わせる。
    DrawSpecialMaterial(depthSrvHandle, colorSrvHandle, &engine::Object3DCommon::SetGatePortalGraphicsCommand, true, 1);
}

void MeshRenderer::DrawWindOrb(uint32_t depthSrvHandle, uint32_t colorSrvHandle) {
    const auto settings = GetSpecialMaterialDrawSettings(SpecialKind::kWindOrb);
    DrawSpecialMaterial(depthSrvHandle, colorSrvHandle, settings.setGraphicsCommand, settings.useProxyModel);
}

void MeshRenderer::InitializeWaterProxyModel() {
    // 水面シェーダー用に細分化した格子を作り、波の頂点変位を表現できるようにする。
    ModelCommon* modelCommon = ModelManager::GetInstance()->GetModelCommon();

    // ModelCommonが存在しない段階では頂点・インデックスの準備を行えない。
    if (!modelCommon) {
        return;
    }

    // 水面メッシュを、Y=0でX・Zが-1から1までの格子として生成する。
    waterProxyModel_ = std::make_unique<Model>();

    constexpr std::int32_t kSegments = 128;
    // 分割数は区間数なので、両端を含む一行の頂点数は1多くなります。
    constexpr std::int32_t kRow = kSegments + 1;
    std::vector<Model::VertexData> vertices;
    vertices.reserve(kRow * kRow);

    for (std::int32_t iz = 0; iz <= kSegments; ++iz) {
        const float v = static_cast<float>(iz) / static_cast<float>(kSegments);
        const float z = -1.0f + v * 2.0f;

        for (std::int32_t ix = 0; ix <= kSegments; ++ix) {
            const float u = static_cast<float>(ix) / static_cast<float>(kSegments);
            const float x = -1.0f + u * 2.0f;

            Model::VertexData vertex{};
            vertex.position = { x, 0.0f, z, 1.0f };
            vertex.texcoord = { u, v };
            vertex.normal = { 0.0f, 1.0f, 0.0f };
            vertex.tangent = { 1.0f, 0.0f, 0.0f };
            vertex.boneWeights = { 0.0f, 0.0f, 0.0f, 0.0f };
            vertex.boneIndices = { 0.0f, 0.0f, 0.0f, 0.0f };
            vertices.push_back(vertex);
        }
    }

    std::vector<uint32_t> indices;
    indices.reserve(kSegments * kSegments * 6);

    // 格子の各セルを2枚の三角形に分けてインデックスを作る。
    for (std::int32_t iz = 0; iz < kSegments; ++iz) {
        for (std::int32_t ix = 0; ix < kSegments; ++ix) {
            const uint32_t i0 = static_cast<uint32_t>(iz * kRow + ix);
            const uint32_t i1 = i0 + 1;
            const uint32_t i2 = i0 + static_cast<uint32_t>(kRow);
            const uint32_t i3 = i2 + 1;
            indices.insert(indices.end(), { i0, i2, i1, i1, i2, i3 });
        }
    }

    waterProxyModel_->CreateFromVertices(modelCommon, vertices, indices);
}

void MeshRenderer::InitializeFireProxyModel() {
    // 平面型の炎で使う代理メッシュを必要時に生成する。
    ModelCommon* modelCommon = ModelManager::GetInstance()->GetModelCommon();

    // ModelCommonを使えない場合は炎用の平面を生成しない。
    if (!modelCommon) {
        return;
    }

    fireProxyModel_ = std::make_unique<Model>();

    // Z=0のXY平面に、UVを持つ4頂点の板を作る。
    std::vector<Model::VertexData> vertices(4);
    auto setVertex = [](Model::VertexData& vertex, float x, float y, float u, float v) {
        vertex.position = { x, y, 0.0f, 1.0f };
        vertex.texcoord = { u, v };
        vertex.normal = { 0.0f, 0.0f, 1.0f };
        vertex.tangent = { 1.0f, 0.0f, 0.0f };
        vertex.boneWeights = { 0.0f, 0.0f, 0.0f, 0.0f };
        vertex.boneIndices = { 0.0f, 0.0f, 0.0f, 0.0f };
    };

    setVertex(vertices[0], -1.0f, -1.0f, 0.0f, 1.0f);
    setVertex(vertices[1], -1.0f, 1.0f, 0.0f, 0.0f);
    setVertex(vertices[2], 1.0f, 1.0f, 1.0f, 0.0f);
    setVertex(vertices[3], 1.0f, -1.0f, 1.0f, 1.0f);

    const std::vector<uint32_t> indices = { 0, 1, 2, 0, 2, 3 };
    fireProxyModel_->CreateFromVertices(modelCommon, vertices, indices);
}

void MeshRenderer::InitializeVolumetricFireProxyModel() {
    ModelCommon* modelCommon = ModelManager::GetInstance()->GetModelCommon();

    // ModelCommonを使えない場合は体積用のメッシュを生成しない。
    if (!modelCommon) {
        return;
    }

    volumetricFireProxyModel_ = std::make_unique<Model>();

    std::vector<Model::VertexData> vertices;
    std::vector<uint32_t> indices;
    vertices.reserve(24);
    indices.reserve(36);

    // 箱の面は表示せず、内側の炎だけを描きます。面法線で二重描画を防ぎます。
    for (std::int32_t face = 0; face < 6; ++face) {
        const std::int32_t axis = face / 2;
        const float sign = (face % 2 == 0) ? -1.0f : 1.0f;
        const uint32_t firstVertex = static_cast<uint32_t>(vertices.size());

        for (std::int32_t corner = 0; corner < 4; ++corner) {
            const float u = (corner == 2 || corner == 3) ? 1.0f : -1.0f;
            const float v = (corner == 1 || corner == 2) ? 1.0f : -1.0f;
            float p[3]{};
            float n[3]{};
            p[axis] = sign;
            p[(axis + 1) % 3] = u;
            p[(axis + 2) % 3] = v;
            n[axis] = sign;
            Model::VertexData vertex{};
            vertex.position = { p[0], p[1], p[2], 1.0f };
            vertex.texcoord = { u * 0.5f + 0.5f, v * 0.5f + 0.5f };
            vertex.normal = { n[0], n[1], n[2] };
            vertex.tangent = { 1.0f, 0.0f, 0.0f };
            vertices.push_back(vertex);
        }

        indices.insert(indices.end(), {
            firstVertex, firstVertex + 1, firstVertex + 2,
            firstVertex, firstVertex + 2, firstVertex + 3
        });
    }

    volumetricFireProxyModel_->CreateFromVertices(modelCommon, vertices, indices);
}

void MeshRenderer::InitializeGatePortalProxyModel() {
    // ポータルを描くための代理形状を元モデルとは別に保持する。
    ModelCommon* modelCommon = ModelManager::GetInstance()->GetModelCommon();

    // ModelCommonを使えない場合はゲート用の平面を生成しない。
    if (!modelCommon) {
        return;
    }

    gatePortalProxyModel_ = std::make_unique<Model>();

    constexpr std::int32_t kSegments = 24;
    constexpr float kHalfDepth = 1.0f;
    const std::int32_t row = kSegments + 1;

    std::vector<Model::VertexData> vertices;
    vertices.reserve(row * row * 2);

    auto addVertex = [&vertices](float x, float y, float z, float u, float v) {
        Model::VertexData vertex{};
        vertex.position = { x, y, z, 1.0f };
        vertex.texcoord = { u, v };
        vertex.normal = { 0.0f, 0.0f, z >= 0.0f ? 1.0f : -1.0f };
        vertex.tangent = { 1.0f, 0.0f, 0.0f };
        vertex.boneWeights = { 0.0f, 0.0f, 0.0f, 0.0f };
        vertex.boneIndices = { 0.0f, 0.0f, 0.0f, 0.0f };
        vertices.push_back(vertex);
    };

    // Zが異なる2層を格子で作り、視線が通るゲートの前後を表す。
    for (std::int32_t layer = 0; layer < 2; ++layer) {
        const float z = (layer == 0) ? -kHalfDepth : kHalfDepth;

        for (std::int32_t iy = 0; iy <= kSegments; ++iy) {
            const float v = static_cast<float>(iy) / static_cast<float>(kSegments);
            const float y = -1.0f + v * 2.0f;

            for (std::int32_t ix = 0; ix <= kSegments; ++ix) {
                const float u = static_cast<float>(ix) / static_cast<float>(kSegments);
                const float x = -1.0f + u * 2.0f;
                addVertex(x, y, z, u, v);
            }
        }
    }

    std::vector<uint32_t> indices;
    indices.reserve(kSegments * kSegments * 6 * 2);

    for (std::int32_t layer = 0; layer < 2; ++layer) {
        const uint32_t base = static_cast<uint32_t>(layer * row * row);

        for (std::int32_t iy = 0; iy < kSegments; ++iy) {
            for (std::int32_t ix = 0; ix < kSegments; ++ix) {
                const uint32_t i0 = base + static_cast<uint32_t>(iy * row + ix);
                const uint32_t i1 = i0 + 1;
                const uint32_t i2 = i0 + static_cast<uint32_t>(row);
                const uint32_t i3 = i2 + 1;

                // 手前側と奥側で面の向きが反対になるようにインデックスの順序を変える。
                if (layer == 0) {
                    indices.insert(indices.end(), { i0, i3, i1, i0, i2, i3 });
                // 反対側の面は、反対向きの法線に合わせて三角形の頂点順を逆にする。
                } else {
                    indices.insert(indices.end(), { i0, i1, i3, i0, i3, i2 });
                }
            }
        }
    }

    gatePortalProxyModel_->CreateFromVertices(modelCommon, vertices, indices);
}

void MeshRenderer::InitializeGateBarrierCylinderProxyModel() {
    // 結界の設定では平面ではなく円筒の代理形状を使う。
    ModelCommon* modelCommon = ModelManager::GetInstance()->GetModelCommon();

    // ModelCommonを使えない場合はゲート用の円筒を生成しない。
    if (!modelCommon) {
        return;
    }

    constexpr std::int32_t kSegments = 96;
    std::vector<Model::VertexData> vertices;
    std::vector<uint32_t> indices;
    vertices.reserve((kSegments + 1) * 2);
    indices.reserve(kSegments * 6);

    // 円周の始点と終点の位置を重複させ、つなぎ目の両側に別のUVを持たせる。
    for (std::int32_t row = 0; row <= 1; ++row) {
        const float y = row == 0 ? -1.0f : 1.0f;

        for (std::int32_t segment = 0; segment <= kSegments; ++segment) {
            const float u = static_cast<float>(segment) / static_cast<float>(kSegments);
            const float angle = u * (2.0f * std::numbers::pi_v<float>);
            const float x = std::sin(angle);
            const float z = std::cos(angle);
            Model::VertexData vertex{};
            vertex.position = {x, y, z, 1.0f};
            vertex.texcoord = {u, static_cast<float>(row)};
            vertex.normal = {x, 0.0f, z};
            vertex.tangent = {z, 0.0f, -x};
            vertices.push_back(vertex);
        }
    }

    for (std::int32_t segment = 0; segment < kSegments; ++segment) {
        const uint32_t lower = static_cast<uint32_t>(segment);
        // 上下のリングを対応させ、周方向に隣接する二組の頂点で側面の四角形を二枚の三角形に分けます。
        const uint32_t upper = lower + kSegments + 1;
        indices.insert(indices.end(), {lower, upper, lower + 1, lower + 1, upper, upper + 1});
    }

    gateBarrierCylinderProxyModel_ = std::make_unique<Model>();
    gateBarrierCylinderProxyModel_->CreateFromVertices(modelCommon, vertices, indices);
}


// 実際の描画カメラと既定値のバッファを参照できる場合だけシルエットを描く。
void MeshRenderer::DrawVisibilitySilhouette(uint32_t depthSrvHandle, uint32_t colorSrvHandle) {
    auto* camera = CameraManager::GetInstance()->GetActiveCamera();
    // シルエットは実環境で、カメラ、共通状態、材質値、描画バッファが揃った場合にだけ描く。
    if (!common_ || !camera || !waterParamData_ || !HasRequiredBuffers() || isUiPreview_) return;
    const auto& color = camera->GetVisibilitySettings().silhouetteColor;
    // アルファが0以下なら画面に重ねる必要がないため描画しない。
    if (color[3] <= 0.0f) return;
    // 自機だけが必要になった時に確保する。実画面や別カメラのCBVを流用すると、
    // 同じフレーム内の後続描画による書換えが先行コマンドにも見えてしまう。
    // 別の描画に値が上書きされないよう、シルエット専用の定数バッファを確保する。
    if (!silhouetteResource_) {
        silhouetteResource_ = common_->GetDxCommon()->CreateBufferResource(sizeof(WaterParamForGpu));
        // 定数バッファを確保またはマップできなければ、参照を破棄して今回の描画を中止する。
        if (!silhouetteResource_ || FAILED(silhouetteResource_->Map(0, nullptr, reinterpret_cast<void**>(&silhouetteData_)))) {
            silhouetteResource_.Reset(); silhouetteData_ = nullptr; return;
        }
    }
    // 通常材質の基礎定数をコピーし、シルエットの色・強さと現在のカメラ情報だけを専用枠に上書きします。
    *silhouetteData_ = *waterParamData_;
    silhouetteData_->cameraWorldPosition = camera->GetEye();
    silhouetteData_->waterInverseViewProjection = camera->GetInverseViewProjectionMatrix();
    silhouetteData_->waterLightColor = {color[0], color[1], color[2]};
    silhouetteData_->visibilityParameters.x = color[3];
    // 遮蔽時の輪郭を実際に描画される姿勢に揃えるため、通常の描画と同じスキニング済み形状を選ぶ。
    // モデルがある場合は、共通の描画関数に渡すデータを先に更新する。
    if (auto* model = ResolveDrawModel()) model->PrepareComputeSkinning();
    DrawSpecialMaterialWithWvp(wvpResource_.Get(), depthSrvHandle, colorSrvHandle,
        &engine::Object3DCommon::SetVisibilitySilhouetteGraphicsCommand, false, 0, silhouetteResource_.Get());
}

} // namespace engine
