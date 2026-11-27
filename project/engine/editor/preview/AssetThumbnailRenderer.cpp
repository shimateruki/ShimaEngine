#define NOMINMAX
#include <cstdint>
#include "AssetThumbnailRenderer.h"
#include "engine/editor/assets/PortraitExportRegistry.h"
#include "TextureManager.h"
#include <limits>
#include <cfloat>
#include <functional>
#include "ModelManager.h"
#include "engine/editor/assets/preset/PresetManager.h"
#include "game/system/rules/GameplayStatusManager.h"
#include "DebugConsole.h"
#include "DirectXCommon.h"
#include "SrvManager.h"
#include "BaseScene.h"
#include "Object3D.h"
#include "EffectObject3D.h"
#include "MeshRenderer.h"
#include "WinApp.h"
#include "CameraManager.h"
#include "Object3DCommon.h"
#include "engine/graphics/3d/light/Light.h"
#include "engine/graphics/3d/light/LightManager.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <utility>
#include <wincodec.h>


namespace fs = std::filesystem;

namespace {
// 大きなカタログでもSRVと描画先を増やし続けず、GPUが読み終えた画像を再利用します。
constexpr size_t kPresetThumbnailCacheLimit = 96;
constexpr const char* kPresetPreviewPrefix = "@preset-preview:";
constexpr size_t kPbrThumbnailCacheLimit = 64;

// 3種類のサムネイルで同じ描画形式を使う。背景色だけを各入口から渡す。
bool CreateThumbnailTarget(ID3D12Device* device, std::int32_t size, const float (&background)[4], auto& data) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = size;
    desc.Height = size;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = desc.Format;
    std::copy_n(background, 4, clear.Color);

    // 途中で確保に失敗したときは、呼び出し元が使う枠を更新しない。
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvView;
    // 描画用テクスチャを確保できなければ、呼び出し元のキャッシュを更新しません。
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&resource)))) {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeap{};
    rtvHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeap.NumDescriptors = 1;
    // RTV用のディスクリプターヒープを確保できない場合も、確保したテクスチャを公開しません。
    if (FAILED(device->CreateDescriptorHeap(&rtvHeap, IID_PPV_ARGS(&rtvView))))
        return false;
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = desc.Format;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(resource.Get(), &rtv, rtvView->GetCPUDescriptorHandleForHeapStart());
    // 描画先のビューを作ってから、サムネイルをUIで参照するためのSRVを登録します。
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = desc.Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    data.srvHandle = engine::SrvManager::GetInstance()->CreateSrv(resource.Get(), srv);
    data.resource = std::move(resource);
    data.rtvHeap = std::move(rtvView);
    data.isCaptured = false;
    return true;
}

void ApplyThumbnailLightOverride(engine::Object3D* object) {
    // サムネイル撮影ではシーン照明の差を抑え、子オブジェクトにも同じ表示設定を適用する。
    // 実体が無ければ、材質も子の階層も更新しません。
    if (!object) {
        return;
    }

    // 材質が使える実体であれば、シーン照明を無効にして最低限の発光を確保します。
    if (auto* material = object->GetMaterialData()) {
        object->SetEnableLighting(false);
        object->SetSelectedLighting(0);
        object->SetEmissive((std::max)(material->emissive, 1.0f));
    }

    for (engine::Object3D* child : object->GetChildren()) {
        ApplyThumbnailLightOverride(child);
    }
}

bool SavePng(const DirectX::Image& image, const fs::path& path) {
    // 生成したRGBA画像をWIC経由でPNGへ書き、失敗は呼び出し側へ返す。
    const std::wstring widePath = path.wstring();
    return SUCCEEDED(DirectX::SaveToWICFile(
        image,
        DirectX::WIC_FLAGS_NONE,
        GUID_ContainerFormatPng,
        widePath.c_str(),
        &GUID_WICPixelFormat32bppBGRA));
}

bool CreateHurtPortrait(const DirectX::Image& source, DirectX::ScratchImage& hurtImage) {
    // ピクセル形式がRGBA8でないか、幅・高さが0なら被弾版への変換を行いません。
    if (source.format != DXGI_FORMAT_R8G8B8A8_UNORM || source.width == 0 || source.height == 0) {
        return false;
    }

    // 横幅を維持して高さを通常版の 86% に縮める。出力寸法をそろえ、UI で差し替えても表示枠が動かないようにする。
    const size_t squashedHeight = (std::max)(size_t{ 1 }, static_cast<size_t>(std::lround(source.height * 0.86)));
    DirectX::ScratchImage squashed;
    // 画像の縦縮小に失敗した場合は、透明キャンバスへの転送へ進みません。
    if (FAILED(DirectX::Resize(
        source,
        source.width,
        squashedHeight,
        DirectX::TEX_FILTER_CUBIC,
        squashed))) {
        return false;
    }

    // 元画像と同じ寸法で出力先を確保できなければ、被弾版を作れません。
    if (FAILED(hurtImage.Initialize2D(
        DXGI_FORMAT_R8G8B8A8_UNORM,
        source.width,
        source.height,
        1,
        1))) {
        return false;
    }

    const DirectX::Image* squashedImage = squashed.GetImage(0, 0, 0);
    const DirectX::Image* destination = hurtImage.GetImage(0, 0, 0);

    // 縮小元と出力先の画素領域を両方取得できる場合だけ、各行を転送します。
    if (!squashedImage || !destination) {
        return false;
    }

    // 空いた領域を透明にし、縮めた画像を中央より 4 ピクセル下に重ねる。画像ごとに記録された行幅を使い、行末の余白を含めずに転送する。
    std::memset(destination->pixels, 0, destination->slicePitch);
    const size_t verticalOffset = (source.height - squashedHeight) / 2 + 4;
    const size_t copyHeight = (std::min)(squashedHeight, source.height - (std::min)(verticalOffset, source.height));
    const size_t copyBytes = (std::min)(source.width * 4, (std::min)(squashedImage->rowPitch, destination->rowPitch));

    for (size_t y = 0; y < copyHeight; ++y) {
        std::memcpy(
            destination->pixels + (verticalOffset + y) * destination->rowPitch,
            squashedImage->pixels + y * squashedImage->rowPitch,
            copyBytes);
    }

    // 被弾時は同じモデルの形を保ったまま、つぶれと赤みだけで反応を出します。
    for (size_t y = 0; y < destination->height; ++y) {
        uint8_t* row = destination->pixels + y * destination->rowPitch;

        for (size_t x = 0; x < destination->width; ++x) {
            uint8_t* pixel = row + x * 4;

            // 透明な画素は変更せず、描画された部分だけに赤みを加えます。
            if (pixel[3] == 0) {
                continue;
            }

            pixel[0] = static_cast<uint8_t>((std::min)(255.0f, pixel[0] * 0.88f + 42.0f));
            pixel[1] = static_cast<uint8_t>(pixel[1] * 0.76f);
            pixel[2] = static_cast<uint8_t>(pixel[2] * 0.76f);
        }
    }

    return true;
}
}

AssetThumbnailRenderer::AssetThumbnailRenderer() = default;
AssetThumbnailRenderer::~AssetThumbnailRenderer() = default;

void AssetThumbnailRenderer::Initialize(engine::DirectXCommon* dxCommon) {
    dxCommon_ = dxCommon;

    // DirectXの共通基盤がある場合に、実シーンから独立した撮影用の描画基盤を作ります。
    if (dxCommon_) {
        previewObject3DCommon_ = std::make_unique<engine::Object3DCommon>();
        previewObject3DCommon_->Initialize(dxCommon_);

        // シーンの昼夜やフォグに左右されず、全PBR素材を同じ条件で比較する。
        pbrPreviewLightResource_ = dxCommon_->CreateBufferResource(sizeof(DirectionalLight));

        // 平行光のバッファが確保できた場合は、フォグを除いた一定の光を用意します。
        if (pbrPreviewLightResource_) {
            DirectionalLight light{};
            light.color = { 1.0f, 1.0f, 1.0f, 1.0f };
            light.direction = { -0.4f, -0.7f, 0.55f };
            light.intensity = 2.0f;
            light.ambientColor = { 0.24f, 0.26f, 0.29f };
            light.enableFog = 0;
            light.volumetricIntensity = 0.0f;
            light.volumetricSteps = 0;
            light.lightViewProj = engine::Math::MakeIdentity4x4();
            void* mapped = nullptr;

            // Mapに成功し書き込み先がある場合だけ、初期化したライトの定数を転送します。
            if (SUCCEEDED(pbrPreviewLightResource_->Map(0, nullptr, &mapped)) && mapped) {
                std::memcpy(mapped, &light, sizeof(light));
                pbrPreviewLightResource_->Unmap(0, nullptr);
            // GPU側へ書けなければ、撮影用の平行光バッファを無効にします。
            } else {
                pbrPreviewLightResource_.Reset();
            }
        }

        // ルートCBVの前回値を引き継がず、撮影中の局所ライトは常に0個にする。
        pbrPreviewPointLightsResource_ = dxCommon_->CreateBufferResource(sizeof(engine::LightManager::PointLightConstData));
        pbrPreviewSpotLightsResource_ = dxCommon_->CreateBufferResource(sizeof(engine::LightManager::SpotLightConstData));
        const auto clearLightBuffer = [](ID3D12Resource* resource, size_t bytes) {
            // バッファが確保できなかった場合、その領域には初期値を書きません。
            if (!resource)
                return;
            void* mapped = nullptr;

            // 書き込み先にマップできた場合だけ、ライト数と配列をクリアします。
            if (SUCCEEDED(resource->Map(0, nullptr, &mapped)) && mapped) {
                std::memset(mapped, 0, bytes);
                resource->Unmap(0, nullptr);
            }
        };

        clearLightBuffer(pbrPreviewPointLightsResource_.Get(), sizeof(engine::LightManager::PointLightConstData));
        clearLightBuffer(pbrPreviewSpotLightsResource_.Get(), sizeof(engine::LightManager::SpotLightConstData));
    }
}

uint64_t AssetThumbnailRenderer::GetModelThumbnailGpuPtr(const std::string& modelName) {
    // 描画基盤が無ければ有効なサムネイルを作れないため、ハンドル0を返します。
    if (!dxCommon_) return 0;
    // 描画先を初回参照で確保し、実際の撮影は CapturePendingThumbnails に任せる。
    if (thumbnailAlbum_.find(modelName) == thumbnailAlbum_.end()) {
        CreateThumbnailResource(modelName);
    }

    const auto found = thumbnailAlbum_.find(modelName);
    return found != thumbnailAlbum_.end()
        ? engine::SrvManager::GetInstance()->GetGpuDescriptorHandle(found->second.srvHandle).ptr : 0;
}

void AssetThumbnailRenderer::RenamePresetThumbnail(const std::string& oldName, const std::string& newName) {
    // 元と先の名前が同じか、新しい名前の枠がすでにある場合は移動しません。
    if (oldName == newName || presetThumbnailAlbum_.count(newName)) return;
    auto entry = presetThumbnailAlbum_.extract(oldName);
    // 旧名のサムネイルが存在しなければ、移す対象がありません。
    if (entry.empty()) return;
    entry.key() = newName;
    presetThumbnailAlbum_.insert(std::move(entry));

    // 改名しても、GPUが旧名の画像を読み終えるまでは再利用しない。
    auto lastUse = presetThumbnailUseFence_.extract(oldName);
    // 旧名の利用フェンスが残っている場合は、新しい名前へ引き継ぎます。
    if (!lastUse.empty()) {
        presetThumbnailUseFence_[newName] = lastUse.mapped();
    }
}

void AssetThumbnailRenderer::QueueHudPortraitExports(bool forceAll) {
    // 通常・被弾PNGの不足分だけ撮影対象とし、強制更新時は既存ファイルも撮り直す。
    for (const auto& spec : engine::PortraitExportRegistry::GetEntries()) {
        const fs::path outputDirectory(spec.outputDirectory);
        const fs::path normalPath = outputDirectory / (std::string(spec.fileStem) + ".png");
        const fs::path hurtPath = outputDirectory / (std::string(spec.fileStem) + "_hurt.png");

        // 通常版と被弾版の両方が存在し、強制更新でもなければ既存画像を使います。
        if (!forceAll && fs::exists(normalPath) && fs::exists(hurtPath)) {
            continue;
        }

        // 対象のプリセットがキャッシュに無い場合は、出力用の撮影枠をリクエストします。
        if (presetThumbnailAlbum_.find(spec.presetName) == presetThumbnailAlbum_.end()) {
            CreatePresetThumbnailResource(spec.presetName);
        }

        ThumbnailData& data = presetThumbnailAlbum_[spec.presetName];
        data.isHudPortrait = true;
        data.exportRequested = true;
        data.exportFileStem = spec.fileStem;
        data.exportDirectory = spec.outputDirectory;
        data.portraitYaw = spec.yaw;
        data.portraitPitch = spec.pitch;
        data.portraitScale = spec.scale;
        data.portraitOffsetY = spec.offsetY;
        data.isCaptured = false;
        data.previewObject.reset();
    }
}

// モデル写真用に背景色と描画先を登録し、保存先の作成が失敗した項目を残さないようにします。
void AssetThumbnailRenderer::CreateThumbnailResource(const std::string& modelName) {
    // 描画基盤がまだ無ければ、GPU上の撮影先を確保できません。
    if (!dxCommon_)
        return;
    const float background[] = { 0.15f, 0.15f, 0.15f, 1.0f };
    // 描画先の確保が失敗した場合は、空の項目を一覧から外します。
    if (!CreateThumbnailTarget(dxCommon_->GetDevice(), kThumbnailSize, background, thumbnailAlbum_[modelName])) {
        thumbnailAlbum_.erase(modelName);
    }
}

void AssetThumbnailRenderer::CreatePresetThumbnailResource(const std::string& presetName) {
    // 描画基盤を取得できない場合はキャッシュを変更しません。
    if (!dxCommon_)
        return;
    // キャッシュが上限に達した場合は、新規確保せず使用済み枠の再利用を試します。
    if (presetThumbnailAlbum_.size() >= kPresetThumbnailCacheLimit) {
        const uint64_t completed = dxCommon_->GetFence()->GetCompletedValue();
        auto oldest = presetThumbnailAlbum_.end();
        uint64_t oldestFence = UINT64_MAX;

        for (auto it = presetThumbnailAlbum_.begin(); it != presetThumbnailAlbum_.end(); ++it) {
            // HUDの画像および保存要求のある枠は、他のプリセットに割り当てません。
            if (it->second.isHudPortrait || it->second.exportRequested)
                continue;
            const uint64_t lastUse = presetThumbnailUseFence_[it->first];

            // GPUが使用を完了しており、見つかった候補の中で利用時刻が最も古い枠を残します。
            if (lastUse <= completed && lastUse < oldestFence) {
                oldest = it;
                oldestFence = lastUse;
            }
        }

        // 書き出し対象と GPU 使用中の枠を除いた結果、空きがなければ持ち越す。描画先や SRV をその場で奪わない。
        if (oldest == presetThumbnailAlbum_.end())
            return;
        ThumbnailData reused = std::move(oldest->second);
        presetThumbnailUseFence_.erase(oldest->first);
        presetThumbnailAlbum_.erase(oldest);
        reused.previewObjects.clear();
        reused.previewObject.reset();
        reused.retiredPreviewObjects.clear();
        reused.retiredPreviewFence = 0;
        reused.isCaptured = false;
        reused.contentDirty = true;
        reused.previewHasBounds = false;
        presetThumbnailAlbum_[presetName] = std::move(reused);
        return;
    }

    const float background[] = { 0.2f, 0.2f, 0.25f, 1.0f };
    // 新規の描画先が作れなかった場合、プリセット名に空のキャッシュ項目を残しません。
    if (!CreateThumbnailTarget(dxCommon_->GetDevice(), kThumbnailSize, background, presetThumbnailAlbum_[presetName])) {
        presetThumbnailAlbum_.erase(presetName);
    }
}

void AssetThumbnailRenderer::CreatePbrThumbnailResource(const std::string& name) {
    // 描画基盤が無いか名前が空なら、新しいPBR用の枠を作りません。
    if (!dxCommon_ || name.empty())
        return;

    // GPUがまだ参照する画像を破棄しないよう、完了済みの最古の枠だけを再利用する。
    if (pbrThumbnailAlbum_.size() >= kPbrThumbnailCacheLimit) {
        const uint64_t completed = dxCommon_->GetFence()->GetCompletedValue();
        auto oldest = pbrThumbnailAlbum_.end();

        for (auto it = pbrThumbnailAlbum_.begin(); it != pbrThumbnailAlbum_.end(); ++it) {
            // GPUが参照を終え、確認済みの候補より利用フェンスが古い枠だけを選びます。
            if (it->second.lastUseFence <= completed &&
                (oldest == pbrThumbnailAlbum_.end() || it->second.lastUseFence < oldest->second.lastUseFence)) {
                oldest = it;
            }
        }

        // 再利用できる枠が一つも無い場合は撮影を次回に持ち越します。
        if (oldest == pbrThumbnailAlbum_.end())
            return;
        PbrThumbnailData reused = std::move(oldest->second);
        pbrThumbnailAlbum_.erase(oldest);
        reused.thumbnail.previewObject.reset();
        reused.thumbnail.isCaptured = false;
        pbrThumbnailAlbum_[name] = std::move(reused);
        return;
    }

    const float background[] = { 0.18f, 0.19f, 0.22f, 1.0f };
    // 描画先を作成できなければ、作りかけのPBR項目を除きます。
    if (!CreateThumbnailTarget(dxCommon_->GetDevice(), kThumbnailSize, background, pbrThumbnailAlbum_[name].thumbnail)) {
        pbrThumbnailAlbum_.erase(name);
    }
}

uint64_t AssetThumbnailRenderer::GetPbrThumbnailGpuPtr(const std::string& name,
    const std::string& albedoPath, const std::string& normalPath, const std::string& ormPath) {
    // 表示名と3種類の画像パスが揃わなければ、サムネイルを要求しません。
    if (name.empty() || albedoPath.empty() || normalPath.empty() || ormPath.empty())
        return 0;
    // 名前に対応する描画先が無ければ、作成または再利用を試します。
    if (pbrThumbnailAlbum_.find(name) == pbrThumbnailAlbum_.end())
        CreatePbrThumbnailResource(name);
    auto it = pbrThumbnailAlbum_.find(name);
    // キャッシュの項目、画像またはSRVを確保できなければ、表示できないことを0で返します。
    if (it == pbrThumbnailAlbum_.end() || !it->second.thumbnail.resource || !it->second.thumbnail.srvHandle)
        return 0;
    PbrThumbnailData& data = it->second;

    // 同じ名前でも参照テクスチャが変われば撮影済み画像を無効にします。
    if (data.albedoPath != albedoPath || data.normalPath != normalPath || data.ormPath != ormPath) {
        data.albedoPath = albedoPath;
        data.normalPath = normalPath;
        data.ormPath = ormPath;
        data.thumbnail.isCaptured = false;
    }

    // このフレームの利用を記録し、GPUが読み終える前にキャッシュから外さないようにします。
    data.lastUseFence = dxCommon_->GetFenceValue() + 1;
    return data.thumbnail.isCaptured
        ? engine::SrvManager::GetInstance()->GetGpuDescriptorHandle(data.thumbnail.srvHandle).ptr : 0;
}

// PBR用のキャッシュ枠を維持し、各画像を次回の撮影対象へ戻します。
void AssetThumbnailRenderer::InvalidatePbrThumbnails() {
    for (auto& [name, data] : pbrThumbnailAlbum_)
        data.thumbnail.isCaptured = false;
}

uint64_t AssetThumbnailRenderer::GetPresetThumbnailGpuPtr(const std::string& presetName) {
    // サムネイルを必要時に確保し、GPU が参照し終えるまで利用フェンスを延長する。
    // プリセット名が空の場合は、対応する画像を用意しません。
    if (presetName.empty()) {
        return 0;
    }

    // まだキャッシュに無い名前なら、対応する描画先を作ります。
    if (presetThumbnailAlbum_.find(presetName) == presetThumbnailAlbum_.end()) {
        CreatePresetThumbnailResource(presetName);
    }

    auto it = presetThumbnailAlbum_.find(presetName);

    // キャッシュ枠かSRVを用意できていなければ、有効なハンドルを返せません。
    if (it == presetThumbnailAlbum_.end() || it->second.srvHandle == 0) {
        return 0;
    }

    presetThumbnailUseFence_[presetName] = dxCommon_->GetFenceValue() + 1;
    // エフェクトは撮影完了後だけ表示ハンドルを渡す。未撮影の間は表示側が 0 を受け取る。
    if (presetName.starts_with("@effect:") && !it->second.isCaptured)
        return 0;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = engine::SrvManager::GetInstance()->GetGpuDescriptorHandle(it->second.srvHandle);
    return static_cast<uint64_t>(gpuHandle.ptr);
}

uint64_t AssetThumbnailRenderer::GetPresetPreviewGpuPtr(const std::string& presetName, float yaw, float pitch, float zoom) {
    // 名前が空か該当プリセットが無ければ、操作用のプレビューを表示できません。
    if (presetName.empty() || !PresetManager::GetInstance()->HasPreset(presetName))
        return 0;

    // 一覧写真・HUDアイコンとは別の描画先を使い、視点操作がそれらへ波及しないようにする。
    const std::string key = std::string(kPresetPreviewPrefix) + presetName;
    const uint64_t texture = GetPresetThumbnailGpuPtr(key);
    const auto it = presetThumbnailAlbum_.find(key);
    // 専用のキャッシュ枠が確保できなければ、プレビュー結果はありません。
    if (it == presetThumbnailAlbum_.end())
        return 0;
    ThumbnailData& data = it->second;
    // 向きかズームのいずれかが閾値を超えて変わったら、同じ描画先を次の撮影対象に戻します。
    if (std::abs(data.previewYaw - yaw) > 0.0001f ||
        std::abs(data.previewPitch - pitch) > 0.0001f ||
        std::abs(data.previewZoom - zoom) > 0.0001f) {
        data.previewYaw = yaw;
        data.previewPitch = pitch;
        data.previewZoom = zoom;
        data.isCaptured = false;
    }

    return texture;
}

void AssetThumbnailRenderer::InvalidatePresetThumbnail(const std::string& presetName) {
    // 保存値が変わったときは一覧写真と操作用プレビューの両方でObjectを組み直す。
    for (const std::string key : {presetName, std::string(kPresetPreviewPrefix) + presetName}) {
        const auto it = presetThumbnailAlbum_.find(key);

        // 対応するキャッシュ枠がある場合だけ、画像と撮影用の実体に再生成の印を付けます。
        if (it != presetThumbnailAlbum_.end()) {
            it->second.isCaptured = false;
            it->second.contentDirty = true;
        }
    }
}

void AssetThumbnailRenderer::CapturePendingThumbnails() {
    // 描画基盤が未接続の場合は、撮影を開始できません。
    if (!dxCommon_)
        return;
    // HUD 用の不足画像チェックは最初の撮影フレームだけ行う。個別の撮影要求はそれ以降もキューから処理する。
    if (!hudPortraitAutoScanDone_) {
        hudPortraitAutoScanDone_ = true;
        QueueHudPortraitExports(false);
    }

    auto device = dxCommon_->GetDevice();
    auto commandList = dxCommon_->GetCommandList();

    // 撮影用に独立したObject3DCommonを使い、現在のシーンの切り替えや進行に依存させません。
    engine::Object3DCommon* objCommon = previewObject3DCommon_.get();
    // 撮影用の描画基盤を取得できなければ、カメラや描画先を差し替える前に終了します。
    if (!objCommon)
        return;

    // 撮影用の深度がまだ無ければ、書き込み用と読み取り専用のDSVおよびSRVを用意します。
    if (!studioDepth_) {
        D3D12_RESOURCE_DESC depthDesc{};
        depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depthDesc.Width = kThumbnailSize;
        depthDesc.Height = kThumbnailSize;
        depthDesc.DepthOrArraySize = 1;
        depthDesc.MipLevels = 1;
        depthDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
        depthDesc.SampleDesc.Count = 1;
        depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        ;
        clearValue.DepthStencil.Depth = 1.0f;

        device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue, IID_PPV_ARGS(&studioDepth_));

        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvHeapDesc.NumDescriptors = 2;
        device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&studioDsvHeap_));

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(studioDepth_.Get(), &dsvDesc, studioDsvHeap_->GetCPUDescriptorHandleForHeapStart());
        // 特殊材質の撮影中は深度をSRVとDSVの両方から読み、通常の深度テストを維持する。
        dsvDesc.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL;
        D3D12_CPU_DESCRIPTOR_HANDLE readOnlyDsv = studioDsvHeap_->GetCPUDescriptorHandleForHeapStart();
        readOnlyDsv.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        device->CreateDepthStencilView(studioDepth_.Get(), &dsvDesc, readOnlyDsv);
        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv{};
        depthSrv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrv.Texture2D.MipLevels = 1;
        studioDepthSrv_ = engine::SrvManager::GetInstance()->CreateSrv(studioDepth_.Get(), depthSrv);
    }

    D3D12_VIEWPORT originalViewport{};
    originalViewport.Width = (float)WinApp::GetClientWidth();
    originalViewport.Height = (float)WinApp::GetClientHeight();
    originalViewport.TopLeftX = 0;
    originalViewport.TopLeftY = 0;
    originalViewport.MinDepth = 0.0f;
    originalViewport.MaxDepth = 1.0f;

    D3D12_RECT originalScissor{};
    originalScissor.left = 0;
    originalScissor.right = WinApp::GetClientWidth();
    originalScissor.top = 0;
    originalScissor.bottom = WinApp::GetClientHeight();

    // 撮影用カメラがまだ初期化されていなければ、入力を受けない専用カメラを作ります。
    if (!isStudioCameraInitialized_) {
        studioCamera_.Initialize();
        studioCamera_.SetInputEnabled(false);
        isStudioCameraInitialized_ = true;
    }

    studioCamera_.Update();

    // 以前のカメラ上書きを保持し、撮影中だけスタジオ用のカメラを有効にします。
    engine::Camera* previousCameraOverride = CameraManager::GetInstance()->GetActiveCameraOverride();
    CameraManager::GetInstance()->SetActiveCamera(&studioCamera_);


    // モデル一覧から未撮影の枠を選び、描画できる状態へバリアで切り替えます。
    for (auto& pair : thumbnailAlbum_) {
        std::string modelName = pair.first;
        ThumbnailData& data = pair.second;
        // すでに描画が済んだモデルは次回も同じ写真を使います。
        if (data.isCaptured)
            continue;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = data.resource.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        commandList->ResourceBarrier(1, &barrier);

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = data.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = studioDsvHeap_->GetCPUDescriptorHandleForHeapStart();
        commandList->OMSetRenderTargets(1, &rtvHandle, false, &dsvHandle);

        const float clearColor[] = { 0.15f, 0.15f, 0.15f, 1.0f };
        commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        D3D12_VIEWPORT viewport{};
        viewport.Width = (float)kThumbnailSize;
        viewport.Height = (float)kThumbnailSize;
        viewport.TopLeftX = 0;
        viewport.TopLeftY = 0;
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        commandList->RSSetViewports(1, &viewport);

        D3D12_RECT scissorRect{};
        scissorRect.left = 0;
        scissorRect.right = kThumbnailSize;
        scissorRect.top = 0;
        scissorRect.bottom = kThumbnailSize;
        commandList->RSSetScissorRects(1, &scissorRect);

        objCommon->SetGraphicsCommand();
        objCommon->SetPipelineState(BlendMode::kNormal);

        // モデル用の撮影実体が未生成の場合にだけ、画角への収まりを計算します。
        if (!data.previewObject) {
            data.previewObject = std::make_shared<engine::Object3D>();
            data.previewObject->Initialize(objCommon);
            data.previewObject->SetIsUiPreview(true);

            engine::Model* model = ModelManager::GetInstance()->LoadModel(modelName);
            data.previewObject->SetModel(modelName);

            // 境界を取得できなければ初期値の位置と倍率を使います。
            float autoScale = 1.0f;
            engine::Vector3 autoTranslate = engine::Vector3::kZero;

            // モデルを読み込めた場合に、外接サイズと中心を取得します。
            if (model) {
                engine::Vector3 modelSize = model->GetSize();
                engine::Vector3 modelCenter = model->GetCenter();

                float maxDim = (std::max)({ modelSize.x, modelSize.y, modelSize.z });

                // 最大寸法がほぼゼロでなければ、外接サイズから収まる倍率と原点の補正を求めます。
                if (maxDim > 0.001f) {
                    // モデルのサイズと中心点に基づき、フィットするスケールとオフセットを計算
                    // 最長辺を 8.5 ワールド単位にそろえ、拡大後のモデル中心を打ち消して撮影位置へ寄せる。
                    autoScale = 8.5f / maxDim;
                    autoTranslate.x = -modelCenter.x * autoScale;
                    autoTranslate.y = -modelCenter.y * autoScale + 1.1f;
                    autoTranslate.z = -modelCenter.z * autoScale;
                }
            }

            // 計算したスケールと位置をセット（これ以降は保持される）
            data.previewObject->SetScale({ autoScale, autoScale, autoScale });
            data.previewObject->SetTranslate(autoTranslate);

            // スタジオカメラの角度に合わせ、モデルを上向きにして正面を向くように調整
            data.previewObject->SetEulerX(-0.2f);
        }

        auto& targetObj = data.previewObject;
        ApplyThumbnailLightOverride(targetObj.get());

        // 撮影時にモデルのY回転を0.02ラジアン進めます。
        targetObj->SetEulerY(targetObj->GetRotation().y + 0.02f);

        // 行列の更新と描画
        targetObj->UpdateLocalMatrix();
        targetObj->UpdateWorldMatrix();

        targetObj->Draw(nullptr, nullptr);

        // 撮影後のテクスチャをUIから読める状態へ戻し、その枠を撮影済みにします。
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        commandList->ResourceBarrier(1, &barrier);

        data.isCaptured = true;
    }

    // プリセットは一覧・操作用・HUDで別条件を使い、HUD以外の連続撮影数を制限します。
    std::int32_t capturedPresetCount = 0;

    for (auto& pair : presetThumbnailAlbum_) {
        std::string presetName = pair.first;
        ThumbnailData& data = pair.second;
        const bool interactivePreview = presetName.starts_with(kPresetPreviewPrefix);
        const std::string sourcePresetName = interactivePreview
            ? presetName.substr(std::strlen(kPresetPreviewPrefix)) : presetName;
        // 退避時に記録したフェンスまでGPUが完了した場合だけ、旧プレビューの実体を破棄します。
        if (data.retiredPreviewFence != 0 && dxCommon_->GetFence()->GetCompletedValue() >= data.retiredPreviewFence) {
            data.retiredPreviewObjects.clear();
            data.retiredPreviewFence = 0;
        }

        // 撮影済みのプリセットは、画像が無効化されるまでは再撮影しません。
        if (data.isCaptured)
            continue;

        // 表示した項目だけ生成し、初回撮影によるフレーム停止を抑えます。
        if (!data.isHudPortrait && capturedPresetCount >= 4)
            continue;
        ++capturedPresetCount;

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = data.resource.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        commandList->ResourceBarrier(1, &barrier);

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = data.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = studioDsvHeap_->GetCPUDescriptorHandleForHeapStart();
        commandList->OMSetRenderTargets(1, &rtvHandle, false, &dsvHandle);

        const float clearColor[] = {
            // HUD用の画像は背景も透明にし、一覧画像には不透明な背景色を入れます。
            data.isHudPortrait ? 0.0f : 0.2f,
            data.isHudPortrait ? 0.0f : 0.2f,
            data.isHudPortrait ? 0.0f : 0.25f,
            data.isHudPortrait ? 0.0f : 1.0f
        };

        commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        D3D12_VIEWPORT viewport = { 0, 0, (float)kThumbnailSize, (float)kThumbnailSize, 0.0f, 1.0f };
        commandList->RSSetViewports(1, &viewport);
        D3D12_RECT scissor = { 0, 0, kThumbnailSize, kThumbnailSize };
        commandList->RSSetScissorRects(1, &scissor);

        objCommon->SetGraphicsCommand();
        objCommon->SetPipelineState(BlendMode::kNormal);

        // 背景のないポートレートと、通常の一覧・操作用プレビューでは描画元と照明が異なるため、素材種別ごとに撮影する。
        // エフェクトを表す名前の場合に、専用のJSONプリセットを実体化して一枚撮影します。
        if (presetName.starts_with("@effect:")) {
            // GPUで使用中の旧Objectを破棄せず、既存のFence付き退避へ渡します。
            // 旧撮影の実体が残っていたら、使用完了まで保持する配列へ移します。
            if (data.previewObject) {
                data.retiredPreviewObjects.push_back(std::move(data.previewObject));
                data.retiredPreviewFence = dxCommon_->GetFenceValue() + 1;
            }

            auto effect = std::make_shared<EffectObject3D>();
            effect->Initialize(objCommon);
            data.previewObject = effect;
            try {
                // エフェクトのJSONを読み込めた場合だけ、衝突やループを外し撮影用に再生します。
                if (effect->LoadFromJson(presetName.substr(8))) {
                    effect->SetIsUiPreview(true);
                    effect->SetEditorInternal(true);
                    effect->SetMeshSetting(&EffectObject3D::MeshSettings::hasCollision, false);
                    effect->SetCollisionAttribute(0);
                    effect->SetCollisionMask(0);
                    effect->SetTargetObject(nullptr);
                    effect->SetAutoLoop(false);

                    // 背景歪みは撮影先のSceneカラーを持たないため、素材の色と形を撮ります。
                    effect->SetEnableDistortion(false);
                    const float lifetime = (std::max)(0.01f, effect->GetLifetime());
                    effect->Play(lifetime);
                    // 出現直後の空画像を避け、全寿命の 40% まで進めた時点を一覧写真として固定する。
                    effect->Update(lifetime * 0.4f);

                    // 手続き生成メッシュはModelNameが空でもRendererに実体があります。
                    // 描画に使うメッシュがあれば、最大寸法を6単位へ合わせてフレーム内に収めます。
                    if (auto* model = effect->GetMeshRenderer() ? effect->GetMeshRenderer()->GetModel() : nullptr) {
                        auto scale = effect->GetScale();
                        const auto size = model->GetSize();
                        const float extent = (std::max)({std::abs(size.x * scale.x), std::abs(size.y * scale.y), std::abs(size.z * scale.z), 0.001f});
                        scale = scale * (6.0f / extent);
                        effect->SetScale(scale);
                        effect->SetRotation({-0.35f, 0.3f, 0});
                        const auto center = engine::Math::Transform(model->GetCenter(), engine::Math::MakeAffineMatrix(scale, effect->GetRotation(), {}));
                        effect->SetTranslate({-center.x, 1.1f - center.y, -center.z});
                    }

                    effect->UpdateLocalMatrix();
                    effect->UpdateWorldMatrix();
                    effect->Draw(nullptr, nullptr);
                }
            } catch (const std::exception& error) {
                DebugConsole::GetInstance()->AddLog(std::string("Effect thumbnail: ") + error.what());
            }
        // HUDポートレートを要求されている場合は、登録済みの角度・倍率で被写体を整えます。
        } else if (data.isHudPortrait) {
            // 撮影用の実体が無ければ、プリセットを適用するためのオブジェクトを作ります。
            if (!data.previewObject) {
                data.previewObject = std::make_shared<engine::Object3D>();
                data.previewObject->Initialize(objCommon);
                data.previewObject->SetIsUiPreview(true);
            }

            // 通常シーンとは別の撮影用実体へ、指定プリセットを適用します。
            PresetManager::GetInstance()->ApplyPresetToObject(presetName, data.previewObject.get());

            // HUDには個別に指定されたヨーとピッチを使います。
            portraitRotationY_ += 0.02f;
            data.previewObject->SetEulerY(data.isHudPortrait ? data.portraitYaw : portraitRotationY_);
            data.previewObject->SetEulerX(data.isHudPortrait ? data.portraitPitch : -0.2f);

            // モデルの自動フィット計算 (モデルが設定されている場合のみ)
            // 実体にモデル名が設定されている場合だけ、境界に合わせた配置を試みます。
            if (!data.previewObject->GetModelName().empty()) {
                engine::Model* model = ModelManager::GetInstance()->LoadModel(data.previewObject->GetModelName());

                // モデルを取得できた場合に、寸法と原点を求めて撮影サイズを決めます。
                if (model) {
                    engine::Vector3 modelSize = model->GetSize();
                    engine::Vector3 modelCenter = model->GetCenter();
                    float maxDim = (std::max)({ modelSize.x, modelSize.y, modelSize.z });

                    // モデルの最大寸法が有効なら、カメラに収まる倍率へ補正します。
                    if (maxDim > 0.001f) {
                        float autoScale = 8.0f / maxDim;

                        // HUD用の撮影では登録値の倍率補正も掛ける。
                        if (data.isHudPortrait) {
                            autoScale *= data.portraitScale;
                        }

                        data.previewObject->SetScale({ autoScale, autoScale, autoScale });
                        data.previewObject->SetTranslateX(-modelCenter.x * autoScale);
                        data.previewObject->SetTranslateY(-modelCenter.y * autoScale + 1.1f + (data.isHudPortrait ? data.portraitOffsetY : 0.0f));
                        data.previewObject->SetTranslateZ(-modelCenter.z * autoScale);
                    }
                }
            }

            ApplyThumbnailLightOverride(data.previewObject.get());

            data.previewObject->UpdateLocalMatrix();
            data.previewObject->UpdateWorldMatrix();
            data.previewObject->Draw(nullptr, nullptr);

        } else {
            // Sceneの実体やギミックのUpdateを使わず、描画専用Objectで階層を再現します。
            // プリセットが更新されたか初回のため実体がなければ、描画専用の階層を構築します。
            if (data.contentDirty || !data.previewObject) {
            for (auto& node : data.previewObjects)
                data.retiredPreviewObjects.push_back(std::move(node));
            data.previewObjects.clear();
            // 旧親オブジェクトも存在する場合は、子と同様に使用完了まで保持します。
            if (data.previewObject)
                data.retiredPreviewObjects.push_back(std::move(data.previewObject));
            // 退避した描画物が実在する場合に、破棄できる最初の完了フェンスを設定します。
            if (!data.retiredPreviewObjects.empty())
                data.retiredPreviewFence = dxCommon_->GetFenceValue() + 1;
            data.previewObject = std::make_shared<engine::Object3D>();
            data.previewObject->Initialize(objCommon);
            data.previewObject->SetIsUiPreview(true);
            const auto& presets = PresetManager::GetInstance()->GetPresets();
            const auto presetIt = presets.find(sourcePresetName);

            // 現在のプリセットが登録されていた場合だけ、保存された根と子を展開します。
            if (presetIt != presets.end()) {
                // 撮影専用の親子を構築し、32 階層を超える子は展開しない。Scene の実体は使わない。
                std::function<void(const nlohmann::json&, engine::Object3D*, std::int32_t)> build = [&](const nlohmann::json& source, engine::Object3D* parent, std::int32_t depth) {
                    // オブジェクト型でない項目か階層の深さが32を超えた項目は展開しません。
                    if (!source.is_object() || depth > 32)
                        return;
                    auto node = std::make_shared<engine::Object3D>();
                    node->Initialize(objCommon);
                    nlohmann::json visual = source;
                    visual.erase("children");
                    visual.erase("guid");
                    visual.erase("prefabInstance");
                    visual["type"] = "Model";

                    // 一覧は正面に統一し、操作用プレビューだけ配置時の回転を表示します。
                    if (depth == 0 && !interactivePreview) { visual["rotate"] = {0.0f, 0.0f, 0.0f}; visual["quaternion"] = {0.0f, 0.0f, 0.0f, 1.0f}; }

                    // Recorder・衝突地形・Componentを読み込まず、描画設定だけを復元します。
                    visual.erase("recorder");
                    visual.erase("terrainCollisionPath");
                    visual.erase("components");
                    node->ImportFromJson(visual);
                    // 敵として保存されたプリセットでは、撮影に必要なモデルと拡縮を共通設定から補います。
                    if (source.value("type", std::string{}) == "Enemy") {
                        std::string enemyType = source.value("enemyType", std::string{});
                        // 旧形式で直下の敵種が空なら、個体パラメーターのオブジェクトも調べます。
                        if (enemyType.empty() && source.contains("param") && source["param"].is_object()) {
                            enemyType = source["param"].value("enemyType", std::string{});
                        }

                        // 敵のモデルとスケールはプリセットに保存されないため、撮影時だけ共通ステータスから補います。
                        auto* statusManager = GameplayStatusManager::GetInstance();
                        statusManager->Initialize();
                        // 登録済みの敵種を確認できた場合だけ、撮影実体に既定の大きさと外観を適用します。
                        if (const auto* status = statusManager->FindEnemyStatus(enemyType)) {
                            // 共通設定がモデル名を持つ場合だけ、読み込んでいたモデルを差し替えます。
                            if (!status->modelName.empty()) {
                                node->SetModel(status->modelName);
                            }
                            node->SetScale(status->scale);
                        }
                    }
                    const bool invisible = source.value("type", "") == "InvisibleBox" || source.value("presetUsage", "") == "当たり判定";
                    // 判定箱または衝突用プリセットの場合は、表示モデルを外して撮影に映しません。
                    if (invisible)
                        node->SetModel("");
                    node->SetIsUiPreview(true);
                    node->SetParent(parent);
                    node->UpdateWorldMatrix();
                    data.previewObjects.push_back(node);
                    // 子を持つ配列があれば、現在の撮影実体を親として一段深く展開します。
                    if (source.contains("children") && source["children"].is_array())
                        for (const auto& child : source["children"])
                            build(child, node.get(), depth + 1);
                };

                build(presetIt->second, data.previewObject.get(), 0);
            }

            engine::Vector3 low = {FLT_MAX,FLT_MAX,FLT_MAX}, high = {-FLT_MAX,-FLT_MAX,-FLT_MAX};
            bool hasBounds = false;

            for (const auto& node : data.previewObjects) {
                // 実際に表示モデルを持つノードの範囲だけを合成します。
                if (!node->GetModel())
                    continue;
                const engine::Aabb bounds = node->GetModelWorldAabb();
                low.x = (std::min)(low.x,bounds.min.x);
                low.y = (std::min)(low.y,bounds.min.y);
                low.z = (std::min)(low.z,bounds.min.z);
                high.x = (std::max)(high.x,bounds.max.x);
                high.y = (std::max)(high.y,bounds.max.y);
                high.z = (std::max)(high.z,bounds.max.z);
                hasBounds = true;
            }

            data.previewHasBounds = hasBounds;

            // 表示物の境界を一つ以上取得できた場合に、全体の中心と寸法を記録します。
            if (hasBounds) {
                data.previewCenter = {(low.x+high.x)*.5f,(low.y+high.y)*.5f,(low.z+high.z)*.5f};
                data.previewExtent = {high.x-low.x,high.y-low.y,high.z-low.z};
            }

            data.contentDirty = false;
            }

            // 合成した境界がある場合は、表示種別に応じたフレーミングを適用します。
            if (data.previewHasBounds) {
                const engine::Vector3 center = data.previewCenter;
                const engine::Vector3 extent = data.previewExtent;
                const float maximum = (std::max)({extent.x,extent.y,extent.z,.001f});
                // 合成した境界の最長辺を 6 ワールド単位にそろえ、操作用画像だけ利用者のズームを掛ける。
                const float fit = 6.0f / maximum * (interactivePreview ? data.previewZoom : 1.0f);
                Transform framingValue = *data.previewObject->GetTransform();
                Transform* framing = &framingValue;
                framing->scale = {fit,fit,fit};

                // 正面形状と床の上面が読める、固定された斜め上の構図を使います。
                framing->rotate = {-.28f,.32f,0};
                // 全体の高さが水平寸法に比べて薄い場合は、上面が読めるよう俯角を深くします。
                if (extent.y < (std::max)(extent.x,extent.z)*.12f)
                    framing->rotate.x = -.55f;
                // 操作用のプレビューだけ、ユーザーのピッチとヨーを構図に重ねます。
                if (interactivePreview) {
                    framing->rotate.x += data.previewPitch;
                    framing->rotate.y += data.previewYaw;
                }

                framing->isQuaternionMaster = false;
                const engine::Matrix4x4 orientation = engine::Math::MakeAffineMatrix(framing->scale,framing->rotate,{});
                const engine::Vector3 offset = engine::Math::Transform(center,orientation);
                framing->translate = {-offset.x,1.1f-offset.y,-offset.z};
                data.previewObject->SetLocalTransform(framingValue);
            }

            // プリセット写真は保存済みの照明・PBR値を維持し、共通の撮影ライトだけ差し替える。
            for (const auto& node : data.previewObjects) {
                // 各ノードが描画実体を持つ場合に、同じ撮影専用光源を参照させます。
                if (auto* renderer = node->GetMeshRenderer()) {
                    renderer->SetPreviewDirectionalLightResource(pbrPreviewLightResource_.Get());
                }
            }

            data.previewObject->UpdateWorldMatrix();

            for (const auto& node : data.previewObjects) {
                const std::int32_t material = node->GetMaterialType();
                // 通常の材質番号だけ標準の描画へ渡し、特殊材質は次の深度参照パスへ回します。
                if (material < 8 || (material > 22 && material != 26))
                    node->Draw(pbrPreviewPointLightsResource_.Get(), pbrPreviewSpotLightsResource_.Get());
            }

            // 不透明物体の深度を特殊材質へ渡し、読み取り専用DSVで深度テストも維持します。
            commandList->OMSetRenderTargets(0, nullptr, false, nullptr);
            D3D12_RESOURCE_BARRIER depthRead{};
            depthRead.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            depthRead.Transition.pResource = studioDepth_.Get();
            depthRead.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            depthRead.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            commandList->ResourceBarrier(1,&depthRead);
            D3D12_CPU_DESCRIPTOR_HANDLE readOnlyDsv = studioDsvHeap_->GetCPUDescriptorHandleForHeapStart();
            readOnlyDsv.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
            commandList->OMSetRenderTargets(1, &rtvHandle, false, &readOnlyDsv);
            const uint32_t neutralColor = engine::TextureManager::GetInstance()->Load("Resources/sprite/common/white.dds");

            for (const auto& node : data.previewObjects) {
                const std::int32_t material = node->GetMaterialType();
                // 深度を必要とする材質番号だけを、撮影カメラと読み取り専用深度で描画します。
                if ((material >= 8 && material <= 22) || material == 26)
                    node->DrawSpecialMaterialForCamera(&studioCamera_,studioDepthSrv_,neutralColor);
            }

            // 次の撮影のクリアに備え、読取専用の深度を再び書き込み状態に戻します。
            commandList->OMSetRenderTargets(0, nullptr, false, nullptr);
            std::swap(depthRead.Transition.StateBefore,depthRead.Transition.StateAfter);
            commandList->ResourceBarrier(1,&depthRead);
        }

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        commandList->ResourceBarrier(1, &barrier);

        data.isCaptured = true;
    }


    // PBR一覧は撮影用の球だけを描く。1フレーム2枚までに抑えてUI停止を防ぐ。
    std::int32_t capturedPbrCount = 0;

    for (auto& [name, entry] : pbrThumbnailAlbum_) {
        ThumbnailData& data = entry.thumbnail;
        // 撮影済み、必要な描画先が無い、またはPBR画像が不足している枠は飛ばします。
        if (data.isCaptured || !data.resource || !data.rtvHeap ||
            entry.albedoPath.empty() || entry.normalPath.empty() || entry.ormPath.empty()) continue;
        // 未撮影の対象をこのフレームで2枚処理済みなら、残りは次のフレームへ回します。
        if (capturedPbrCount++ >= 2)
            break;

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = data.resource.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        commandList->ResourceBarrier(1, &barrier);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = data.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = studioDsvHeap_->GetCPUDescriptorHandleForHeapStart();
        commandList->OMSetRenderTargets(1, &rtv, false, &dsv);
        const float clearColor[] = { 0.18f, 0.19f, 0.22f, 1.0f };
        commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
        commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        const D3D12_VIEWPORT viewport = { 0, 0, static_cast<float>(kThumbnailSize),
            static_cast<float>(kThumbnailSize), 0.0f, 1.0f };
        const D3D12_RECT scissor = { 0, 0, kThumbnailSize, kThumbnailSize };
        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissor);

        // PBR素材を初めて撮影するときは、比較用の球と固定の材質条件を用意します。
        if (!data.previewObject) {
            data.previewObject = std::make_shared<engine::Object3D>();
            data.previewObject->Initialize(objCommon);
            data.previewObject->SetIsUiPreview(true);
            data.previewObject->SetModel("Primitives/sphere");

            // 球モデルから寸法を取得できた場合に、撮影位置と大きさを計算します。
            if (engine::Model* sphere = ModelManager::GetInstance()->LoadModel("Primitives/sphere")) {
                const engine::Vector3 size = sphere->GetSize();
                const engine::Vector3 center = sphere->GetCenter();
                const float extent = (std::max)({ size.x, size.y, size.z, 0.001f });
                const float scale = 8.5f / extent;
                data.previewObject->SetScale({ scale, scale, scale });
                data.previewObject->SetTranslate({
                    -center.x * scale, 1.1f - center.y * scale, -center.z * scale });
            }

            data.previewObject->SetEulerX(-0.2f);
            data.previewObject->SetMaterialType(0);
            data.previewObject->SetColor({ 1.0f, 1.0f, 1.0f, 1.0f });
            data.previewObject->SetEmissive(1.0f);
            data.previewObject->SetRoughness(1.0f);
            data.previewObject->SetMetallic(1.0f);
            data.previewObject->SetEnableLighting(true);
            data.previewObject->SetSelectedLighting(2);
            data.previewObject->SetEnableNormalMap(true);
            data.previewObject->SetAutoTextureTiling(false);
            data.previewObject->SetTextureTiling({ 1.0f, 1.0f });

            // 球に描画実体があれば、比較用の専用光源を設定します。
            if (auto* renderer = data.previewObject->GetMeshRenderer()) {
                renderer->SetPreviewDirectionalLightResource(pbrPreviewLightResource_.Get());
            }
        }

        // 現在のアルベド・法線・ORMを反映してから、固定の撮影条件で描きます。
        data.previewObject->SetTexture(entry.albedoPath);
        data.previewObject->SetNormalMap(entry.normalPath);
        data.previewObject->SetOrmMap(entry.ormPath);
        data.previewObject->UpdateLocalMatrix();
        data.previewObject->UpdateWorldMatrix();
        data.previewObject->Draw(pbrPreviewPointLightsResource_.Get(), pbrPreviewSpotLightsResource_.Get());

        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        commandList->ResourceBarrier(1, &barrier);
        data.isCaptured = true;
        entry.lastUseFence = dxCommon_->GetFenceValue() + 1;
    }

    // 全撮影後に元のカメラ上書き、Viewport、Scissor を戻し、次のゲーム描画に撮影設定を持ち込まない。
    CameraManager::GetInstance()->SetActiveCamera(previousCameraOverride);

    commandList->RSSetViewports(1, &originalViewport);
    commandList->RSSetScissorRects(1, &originalScissor);
}

void AssetThumbnailRenderer::ExportCapturedHudPortraits() {
    // GPU上の撮影済みサムネイルだけをCPUへ読み戻してPNG化する。
    if (!dxCommon_) {
        return;
    }

    for (auto& [presetName, data] : presetThumbnailAlbum_) {
        // 書き出し要求があり、撮影済みの引き渡せる画像がある項目だけ処理します。
        if (!data.exportRequested || !data.isCaptured || !data.resource) {
            continue;
        }

        const fs::path outputDirectory(data.exportDirectory);
        std::error_code directoryError;
        fs::create_directories(outputDirectory, directoryError);
        // 出力先のフォルダーを作れない場合は要求を保持し、次回に試します。
        if (directoryError) {
            DebugConsole::GetInstance()->AddLog("HUD icon export failed: output directory could not be created.");
            continue; // 保存先を作れない間は、既存どおり出力要求を残す。
        }
        data.exportRequested = false;

        // GPUリソースを取得し、PNG保存に使えるRGBA8へ変換する。
        DirectX::ScratchImage captured;
        // 読み戻しの前後で同じシェーダー参照用の状態を指定し、出力後も撮影済みサムネイルを表示できるようにする。
        HRESULT hr = DirectX::CaptureTexture(
            dxCommon_->GetCommandQueue(),
            data.resource.Get(),
            false,
            captured,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        // GPUテクスチャを読み戻せなかった場合は、その画像の出力を打ち切ります。
        if (FAILED(hr)) {
            DebugConsole::GetInstance()->AddLog("HUD icon export failed while reading GPU texture: " + presetName);
            continue;
        }

        const DirectX::Image* source = captured.GetImage(0, 0, 0);

        // キャプチャに画素データが無ければ、保存する元画像を取得できません。
        if (!source) {
            DebugConsole::GetInstance()->AddLog("HUD icon export failed: captured image is empty: " + presetName);
            continue;
        }

        DirectX::ScratchImage converted;
        // 被弾版は 1 ピクセルを RGBA の 4 バイトとして加工するため、撮影時の形式にかかわらずここで RGBA8 にそろえる。
        hr = DirectX::Convert(
            *source,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            DirectX::TEX_FILTER_DEFAULT,
            DirectX::TEX_THRESHOLD_DEFAULT,
            converted);
        const DirectX::Image* normalImage = converted.GetImage(0, 0, 0);

        // RGBA8への変換が失敗したか変換後画像を取れない場合、どちらのPNGも保存しません。
        if (FAILED(hr) || !normalImage) {
            DebugConsole::GetInstance()->AddLog("HUD icon export failed while converting image: " + presetName);
            continue;
        }

        const fs::path normalPath = outputDirectory / (data.exportFileStem + ".png");

        // 通常版と被弾版を対で書き出し、両方保存できた場合だけ成功とする。
        const fs::path hurtPath = outputDirectory / (data.exportFileStem + "_hurt.png");
        DirectX::ScratchImage hurtImage;
        const bool savedNormal = SavePng(*normalImage, normalPath);
        const bool madeHurt = CreateHurtPortrait(*normalImage, hurtImage);
        const DirectX::Image* hurt = madeHurt ? hurtImage.GetImage(0, 0, 0) : nullptr;
        const bool savedHurt = hurt && SavePng(*hurt, hurtPath);

        // 通常と被弾の両方が保存できた場合だけ成功として記録します。
        if (savedNormal && savedHurt) {
            DebugConsole::GetInstance()->AddLog("HUD icons exported from preset: " + presetName);
        // 片方でも用意できなかった場合は、どのプリセットの書き出しかをログに記録します。
        } else {
            DebugConsole::GetInstance()->AddLog("HUD icon PNG save failed: " + presetName);
        }
    }
}

