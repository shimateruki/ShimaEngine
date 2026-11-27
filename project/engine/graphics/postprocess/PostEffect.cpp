#include <cstdint>
#include "PostEffect.h"
#include "SrvManager.h"
#include"WinApp.h"
#include "RootSignatureBuilder.h"
#include "GraphicsPipelineBuilder.h"
#include "engine/graphics/core/ColorSpace.h"
#include "LightManager.h"
#include "RenderStats.h"
#include "TextureManager.h"
#include <algorithm>
#include <cassert>
#include <cmath>

namespace {
// 効果を解除するための既定値を作ります。経過時間と作業色空間に応じたトーンマップ設定は維持します。
PostEffect::Params MakeNeutralPostEffectParams(float time) {
    PostEffect::Params params{};
    params.time = time;
    params.threshold = 1.0f;
    params.bloomIntensity = 0.0f;
    params.spread = 1.0f;
    params.enableToneMapping = engine::colorSpace::WorkflowSettings::GetInstance().IsLinearWorkflowEnabled() ? 1 : 0;
    params.vignetteIntensity = 0.0f;
    params.chromaticAberration = 0.0f;
    params.filmGrainIntensity = 0.0f;
    params.vignettePower = 1.0f;
    params.radialCenterX = 0.5f;
    params.radialCenterY = 0.5f;
    params.radialIntensity = 0.0f;
    params.radialBlurSamples = 1;
    params.lutIntensity = 0.0f;
    params.colorExposure = 0.0f;
    params.colorContrast = 1.0f;
    params.colorSaturation = 1.0f;
    params.colorTemperature = 0.0f;
    params.colorTint = 0.0f;
    params.damageFlash = 0.0f;
    params.cinemaBarHeight = 0.0f;
    params.wobbleIntensity = 0.0f;
    params.scanlineIntensity = 0.0f;
    params.mosaicSize = 0.0f;
    params.dangerVignette = 0.0f;
    params.blackout = 0.0f;
    params.grayscaleIntensity = 0.0f;
    params.sepiaIntensity = 0.0f;
    params.boxFilterSize = 0;
    params.gaussianFilterSize = 0;
    params.gaussianSigma = 1.0f;
    params.luminanceOutlineIntensity = 0.0f;
    params.depthOutlineIntensity = 0.0f;
    params.dissolveThreshold = 0.0f;
    params.dissolveEdgeWidth = 0.02f;
    params.randomIntensity = 0.0f;
    params.linearWorkflowEnabled = engine::colorSpace::WorkflowSettings::GetInstance().IsLinearWorkflowEnabled() ? 1.0f : 0.0f;
    params.dissolveEdgeColor = { 1.0f, 0.4f, 0.3f };
    params.projectionInverse = engine::Math::MakeIdentity4x4();
    params.slimeFadeIntensity = 0.0f;
    params.slimeDensity = 1.0f;
    params.slimeColor = { 0.18f, 0.8f, 0.44f };
    params.irisFadeIntensity = 0.0f;
    params.irisCenterX = 0.5f;
    params.irisCenterY = 0.5f;
    return params;
}
}

// HDR描画先、縮小Bloom用描画先、プレビュー用描画先と共通GPU定数を準備します。
void PostEffect::Initialize(engine::DirectXCommon* dxCommon) {
    assert(dxCommon);
    dxCommon_ = dxCommon;
    renderWidth_ = WinApp::GetClientWidth();
    renderHeight_ = WinApp::GetClientHeight();
    renderTextures_.resize(8);
    CreateConstBuffer();
    CreateRootSignature();
    CreatePipelineState();

	// [0] シーン描画用 (HDR / 元サイズ)
    CreateRenderTexture(0, WinApp::GetClientWidth(), WinApp::GetClientHeight(), DXGI_FORMAT_R16G16B16A16_FLOAT);

    // [1] トーンマップ後のエディター表示用。ウィンドウと同じサイズの16ビット浮動小数点テクスチャを使う。
    CreateRenderTexture(1, WinApp::GetClientWidth(), WinApp::GetClientHeight(),  DXGI_FORMAT_R16G16B16A16_FLOAT);

    // [2] 高輝度抽出用 (HDR / 1/2サイズで少し軽くする)
    CreateRenderTexture(2, WinApp::GetClientWidth() / 2, WinApp::GetClientHeight() / 2, DXGI_FORMAT_R16G16B16A16_FLOAT);

    // [3] 縮小ブラー用1 (HDR / 1/4サイズ)
    CreateRenderTexture(3, WinApp::GetClientWidth() / 4, WinApp::GetClientHeight() / 4, DXGI_FORMAT_R16G16B16A16_FLOAT);

    // [4] 縮小ブラー用2 (HDR / 1/8サイズ)
    CreateRenderTexture(4, WinApp::GetClientWidth() / 8, WinApp::GetClientHeight() / 8, DXGI_FORMAT_R16G16B16A16_FLOAT);

    // [5] 縮小ブラー用3 (HDR / 1/16サイズ)
    CreateRenderTexture(5, WinApp::GetClientWidth() / 16, WinApp::GetClientHeight() / 16, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ResizeCameraPreviewTextures();
}

// 環境の補間結果を取り込み、ノイズ用の経過時間をフレーム時間（秒）だけ進めます。
void PostEffect::Update(float deltaTime) {
    // 環境補間でこの定数の色調や発光値が変わるため、描画用の値を使う前に今フレームの環境状態を反映する。
    engine::LightManager::GetInstance()->UpdateEnvironmentProfile(deltaTime);

    paramsData_->time += deltaTime;
    // 入力を作業色空間で扱う設定をシェーダーにも渡し、色調整の経路と揃える。
    paramsData_->linearWorkflowEnabled =
        engine::colorSpace::WorkflowSettings::GetInstance().IsLinearWorkflowEnabled() ? 1.0f : 0.0f;
}

// 色補正用のLUTをsRGBとして読み込みます。設定できたときはtrueを返します。
bool PostEffect::SetLUTTexturePath(const std::string& path) {
    // パスが空なら読み込みを試さず、設定済みのLUTを維持する。
    if (path.empty()) {
        return false;
    }

    const uint32_t handle = engine::TextureManager::GetInstance()->Load(
        path,
        engine::TextureManager::TextureColorSpace::kSRGB);
    // 有効なテクスチャハンドルが得られなかった場合も、設定済みのLUTを維持する。
    if (handle == 0) {
        return false;
    }

    lutSrvHandle_ = handle;
    lutTexturePath_ = path;
    return true;
}

// GPUへ渡す加工パラメーターを既定値へ戻します。
void PostEffect::ResetToNeutral() {
    // 定数バッファをまだマップしていない場合は、書き込み先がないため終了する。
    if (!paramsData_) {
        return;
    }

    // 時間を引き継ぎ、ノイズのアニメーション時刻がリセットされるのを防ぐ。
    const float currentTime = paramsData_->time;
    *paramsData_ = MakeNeutralPostEffectParams(currentTime);
}

void PostEffect::SetBloomQuality(BloomQuality quality) {
    // 不正な列挙値は高品質へ寄せ、パス数を定義済みの範囲に保ちます。
    switch (quality) {
    // 定義済みの品質は指定どおり採用する。
    case BloomQuality::kLow:
    case BloomQuality::kMedium:
    case BloomQuality::kHigh:
        bloomQuality_ = quality;
        break;

    // 列挙範囲外の値を受け取った場合は、4段の高品質設定を使う。
    default:
        bloomQuality_ = BloomQuality::kHigh;
        break;
    }
}

bool PostEffect::IsBloomActive() const {
    // ごく小さい強度は実質無効として扱い、不要なパスを省きます。
    constexpr float kBloomIntensityEpsilon = 0.0001f;
    return bloomEnabled_ && paramsData_ && paramsData_->bloomIntensity > kBloomIntensityEpsilon;
}

std::int32_t PostEffect::GetBloomLevelCount() const {
    // 品質設定を縮小解像度の段数へ対応させます。
    switch (bloomQuality_) {
    // 低品質では、縮小画像を1段だけ使う。
    case BloomQuality::kLow: return 1;

    // 中品質では、縮小画像を2段使う。
    case BloomQuality::kMedium: return 2;

    // 高品質では、縮小画像を4段使う。
    case BloomQuality::kHigh: return 4;

    // 不明な品質値は高品質と同じ段数にする。
    default: return 4;
    }
}

std::int32_t PostEffect::GetExpectedPostEffectPassCount() const {
    // Bloomが無効なら、高輝度抽出や縮小を省き、最終パス1回で描く。
    if (!IsBloomActive()) {
        return 1;
    }

    // 高輝度抽出と最終合成に加え、選択した段数に応じた縮小・加算分を数えます。
    return GetBloomLevelCount() * 2 + 2;
}

// カメラプレビューの解像度を、元の描画サイズに対する倍率で指定します。
void PostEffect::SetCameraPreviewResolutionScale(float scale) {
    // 倍率を元サイズの25〜100%に制限する。
    const float clampedScale = std::clamp(scale, 0.25f, 1.0f);

    // 制限後の倍率が現在値とほぼ同じなら、テクスチャの再作成を省く。
    if (std::abs(cameraPreviewResolutionScale_ - clampedScale) < 0.0001f) {
        return;
    }

    cameraPreviewResolutionScale_ = clampedScale;

    // デバイス、描画先の枠、正の描画サイズが揃っていれば、設定した倍率を直ちに反映する。
    if (dxCommon_ && renderTextures_.size() >= 8 && renderWidth_ > 0 && renderHeight_ > 0) {
        ResizeCameraPreviewTextures();
    }
}

// 画面サイズに合わせてフル解像度と段階縮小の描画先を作り直します。
void PostEffect::ResizeRenderTextures(std::int32_t width, std::int32_t height) {
    // DirectXの初期化または描画先8枠の確保が済んでいなければ、再作成を見送る。
    if (!dxCommon_ || renderTextures_.size() < 8) {
        return;
    }

    // 最小のBloom段でも幅と高さを確保できるよう、元サイズを各16ピクセル以上にする。
    width = (std::max)(width, 16);
    height = (std::max)(height, 16);
    renderWidth_ = width;
    renderHeight_ = height;

    // 画面サイズ変更時は、ポストエフェクト用RTも同じタイミングで作り直す。
    CreateRenderTexture(0, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    CreateRenderTexture(1, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    CreateRenderTexture(2, (std::max)(width / 2, 1), (std::max)(height / 2, 1), DXGI_FORMAT_R16G16B16A16_FLOAT);
    CreateRenderTexture(3, (std::max)(width / 4, 1), (std::max)(height / 4, 1), DXGI_FORMAT_R16G16B16A16_FLOAT);
    CreateRenderTexture(4, (std::max)(width / 8, 1), (std::max)(height / 8, 1), DXGI_FORMAT_R16G16B16A16_FLOAT);
    CreateRenderTexture(5, (std::max)(width / 16, 1), (std::max)(height / 16, 1), DXGI_FORMAT_R16G16B16A16_FLOAT);
    ResizeCameraPreviewTextures();
}

// 通常カメラと演出カメラのプレビュー用描画先を、現在の倍率で再作成します。
void PostEffect::ResizeCameraPreviewTextures() {
    // デバイス、描画先8枠、正の描画サイズが揃うまではプレビューを再作成できない。
    if (!dxCommon_ || renderTextures_.size() < 8 || renderWidth_ <= 0 || renderHeight_ <= 0) {
        return;
    }

    // 原則として16:9に収め、両辺の最小サイズとピクセル単位の丸めも適用する。
    constexpr float kPreviewAspect = 16.0f / 9.0f;
    std::int32_t width = (std::max)(static_cast<std::int32_t>(renderWidth_ * cameraPreviewResolutionScale_), 16);
    std::int32_t height = (std::max)(static_cast<std::int32_t>(renderHeight_ * cameraPreviewResolutionScale_), 16);

    // 16:9より横長の候補は、高さを基準に幅を縮める。片辺は16ピクセルを下限とする。
    if (static_cast<float>(width) / static_cast<float>(height) > kPreviewAspect) {
        width = (std::max)(static_cast<std::int32_t>(height * kPreviewAspect), 16);
    }
    // 16:9以下の候補は、幅を基準に高さを決める。
    else {
        height = (std::max)(static_cast<std::int32_t>(width / kPreviewAspect), 16);
    }

    cameraPreviewWidth_ = width;
    cameraPreviewHeight_ = height;
    CreateRenderTexture(kCameraPreviewTextureIndex, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    CreateRenderTexture(kCinematicCameraPreviewTextureIndex, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
}


// ポストエフェクトのPSが読む定数、画像、サンプラーの割り当てを作ります。
void PostEffect::CreateRootSignature() {
    RootSignatureBuilder builder;


    // [0] 定数バッファ (CBV b0 - PixelShader用)
    builder.AddCbv(0, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // [1] メイン画像 (DescriptorTable t0 - PixelShader用)
    builder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // [2] LUT画像 (DescriptorTable t1 - PixelShader用)
    builder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // [3] 深度テクスチャ (DescriptorTable t2 - PixelShader用)
    builder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // [4] ノイズテクスチャ (DescriptorTable t3 - PixelShader用)
    builder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);


    // s0: テクスチャサンプラー (リニア補間、クランプ - PixelShader用)
    builder.AddStaticSampler(0, 0, D3D12_SHADER_VISIBILITY_PIXEL, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    // s1: ポイントサンプラー (ポイント補間、クランプ - PixelShader用)
    builder.AddStaticSampler(1, 0, D3D12_SHADER_VISIBILITY_PIXEL, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    builder.Build(dxCommon_->GetDevice(), rootSignature_.GetAddressOf());
}

// 共通の全画面頂点シェーダーと各パスのピクセルシェーダーを組み合わせてPSOを用意します。
void PostEffect::CreatePipelineState() {
    ID3D12Device* device = dxCommon_->GetDevice();

    // 全画面パスの VS を共有し、抽出・縮小・加算・合成ごとに PS を切り替えます。
    auto vsBlob = dxCommon_->CompileShader(L"Resources/shader/postprocess/PostEffect.VS.hlsl", L"vs_6_0", L"main");
    auto psCopy = dxCommon_->CompileShader(L"Resources/shader/postprocess/PostEffect.PS.hlsl", L"ps_6_0", L"mainCopy");
    auto psExtract = dxCommon_->CompileShader(L"Resources/shader/postprocess/PostEffect.PS.hlsl", L"ps_6_0", L"mainExtract");
    auto psDownsample = dxCommon_->CompileShader(L"Resources/shader/postprocess/PostEffect.PS.hlsl", L"ps_6_0", L"mainDownsample");
    auto psAdd = dxCommon_->CompileShader(L"Resources/shader/postprocess/PostEffect.PS.hlsl", L"ps_6_0", L"mainAdd");
    auto psComposite = dxCommon_->CompileShader(L"Resources/shader/postprocess/PostEffect.PS.hlsl", L"ps_6_0", L"mainComposite");

    GraphicsPipelineBuilder builder;
    builder.SetRootSignature(rootSignature_.Get());
    builder.SetInputLayout(nullptr, 0); // 頂点を SV_VertexID から生成する全画面パスです。

    // 全画面を覆う合成なので、裏面カリングと深度テストは使わない。
    builder.SetRasterizerState(D3D12_CULL_MODE_NONE, D3D12_FILL_MODE_SOLID);
    builder.SetDepthStencilState(false, D3D12_DEPTH_WRITE_MASK_ZERO);

    // HDR出力 (16bit Float)
    DXGI_FORMAT rtvFormats[] = { DXGI_FORMAT_R16G16B16A16_FLOAT };
    builder.SetRenderTargets(1, rtvFormats, DXGI_FORMAT_UNKNOWN);

    // 基本はブレンドなし（不透明描画/上書き）
    builder.SetBlendMode(::BlendMode::kNone);

    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;

    // [PSO 0] Copy用 (HDR出力)
    builder.SetShaders(vsBlob.Get(), psCopy.Get());
    builder.Build(device, pso.ReleaseAndGetAddressOf());
    pipelineStates_.push_back(pso);

    // [PSO 1] プレビュー向けのトーンマップ・最終合成用。描画先の形式は16ビット浮動小数点。
    builder.SetShaders(vsBlob.Get(), psComposite.Get());
    builder.Build(device, pso.ReleaseAndGetAddressOf());
    pipelineStates_.push_back(pso);

    // [PSO 2] 抽出用 (HDR出力)
    builder.SetShaders(vsBlob.Get(), psExtract.Get());
    builder.Build(device, pso.ReleaseAndGetAddressOf());
    pipelineStates_.push_back(pso);

    // [PSO 3] 縮小ブラー用 (HDR出力)
    builder.SetShaders(vsBlob.Get(), psDownsample.Get());
    builder.Build(device, pso.ReleaseAndGetAddressOf());
    pipelineStates_.push_back(pso);

    // [PSO 4] 加算合成用 (HDR出力)
    builder.SetShaders(vsBlob.Get(), psAdd.Get());
    builder.SetBlendMode(::BlendMode::kAdd); 
    builder.Build(device, pso.ReleaseAndGetAddressOf());
    pipelineStates_.push_back(pso);

    // 画面へ直接出力するときは、DirectXCommonのバックバッファ形式に合わせて別のPSOを作る。
    const DXGI_FORMAT backBufferFormat[] = { dxCommon_->GetRtvFormat() };
    builder.SetRenderTargets(1, backBufferFormat, DXGI_FORMAT_UNKNOWN);
    builder.SetBlendMode(::BlendMode::kNone);
    builder.SetShaders(vsBlob.Get(), psComposite.Get());
    builder.Build(device, pso.ReleaseAndGetAddressOf());
    pipelineStates_.push_back(pso);
}

// 同一テクスチャを描画先とサンプリング元に使い分けるため、現在状態を追跡して遷移します。
void PostEffect::TransitionToRtv(ID3D12GraphicsCommandList* commandList, std::int32_t texIndex) {
    RenderTexture& rt = renderTextures_[texIndex];
    // すでに描画先の状態なら、同じ状態への遷移命令を積まずに終了する。
    if (rt.currentState == D3D12_RESOURCE_STATE_RENDER_TARGET)
        return;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = rt.resource.Get();
    barrier.Transition.StateBefore = rt.currentState;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    commandList->ResourceBarrier(1, &barrier);
    rt.currentState = D3D12_RESOURCE_STATE_RENDER_TARGET;
}

// 次の描画パスが結果をサンプルできるよう、描画先をSRV状態へ遷移します。
void PostEffect::TransitionToSrv(ID3D12GraphicsCommandList* commandList, std::int32_t texIndex) {
    RenderTexture& rt = renderTextures_[texIndex];
    // すでにピクセルシェーダーから参照できる状態なら、遷移を省く。
    if (rt.currentState == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
        return;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = rt.resource.Get();
    barrier.Transition.StateBefore = rt.currentState;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    commandList->ResourceBarrier(1, &barrier);
    rt.currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}


// 指定した描画先をRTVとして接続し、そのテクスチャの実寸に描画範囲を合わせます。
void PostEffect::PreDrawScene(ID3D12GraphicsCommandList* commandList, std::int32_t targetTexIndex, bool clear) {
    // 指定されたテクスチャを描画先(RTV)に切り替える
    TransitionToRtv(commandList, targetTexIndex);

    RenderTexture& rt = renderTextures_[targetTexIndex];
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rt.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    commandList->OMSetRenderTargets(1, &rtvHandle, false, nullptr);

    // 初期化が指定された場合だけ色をクリアし、加算パスでは前段の結果を保持する。
    if (clear) {
        float clearColor[] = { 0.1f, 0.1f, 0.1f, 1.0f };
        commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    }

    // ウィンドウサイズではなく、このレンダーターゲットの実寸を描画範囲に使います。
    D3D12_RESOURCE_DESC resDesc = rt.resource->GetDesc();
    float width = (float)resDesc.Width;
    float height = (float)resDesc.Height;

    D3D12_VIEWPORT viewport = { 0.0f, 0.0f, width, height, 0.0f, 1.0f };
    D3D12_RECT scissorRect = { 0, 0, (LONG)resDesc.Width, (LONG)resDesc.Height };
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissorRect);
}

// 深度テストを行う描画へ、サイズを揃えた色と深度の描画先を渡します。
void PostEffect::PreDrawSceneWithDepth(ID3D12GraphicsCommandList* commandList, std::int32_t targetTexIndex, bool clear) {
    // 色と深度のRender Targetを同時に束ね、特殊パスが深度判定を使えるようにします。
    TransitionToRtv(commandList, targetTexIndex);

    RenderTexture& rt = renderTextures_[targetTexIndex];
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rt.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = rt.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    commandList->OMSetRenderTargets(1, &rtvHandle, false, &dsvHandle);

    // クリア指定時は環境の背景色で色を初期化し、深度を1.0に戻す。
    if (clear) {
        const engine::Vector4& sceneClearColor = engine::LightManager::GetInstance()->GetSceneClearColor();
        float clearColor[] = {
            sceneClearColor.x,
            sceneClearColor.y,
            sceneClearColor.z,
            sceneClearColor.w
        };

        commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    }

    // プレビューの実寸をビューポートとシザーに設定する。
    D3D12_RESOURCE_DESC resDesc = rt.resource->GetDesc();
    D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)resDesc.Width, (float)resDesc.Height, 0.0f, 1.0f };
    D3D12_RECT scissorRect = { 0, 0, (LONG)resDesc.Width, (LONG)resDesc.Height };
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissorRect);

    // 続く描画でテクスチャを参照できるよう、SRVのヒープを設定する。
    ID3D12DescriptorHeap* descriptorHeaps[] = { engine::SrvManager::GetInstance()->GetDescriptorHeap() };
    commandList->SetDescriptorHeaps(1, descriptorHeaps);
}

// カメラプレビューの特殊マテリアル描画を準備します。描画先と読み取り用の深度を接続できた場合はtrueを返します。
bool PostEffect::BeginCameraPreviewSpecialPass(ID3D12GraphicsCommandList* commandList, std::int32_t targetTexIndex) {
    // コマンドリストがない、または描画先番号が範囲外の場合はGPU命令を積まない。
    if (!commandList || targetTexIndex < 0 || targetTexIndex >= static_cast<std::int32_t>(renderTextures_.size())) {
        return false;
    }

    RenderTexture& rt = renderTextures_[targetTexIndex];
    // 背景コピー、深度、双方を参照するビューのいずれかが未作成なら、通常描画の状態を維持する。
    if (!rt.resource || !rt.depthResource || !rt.grabResource ||
        !rt.rtvHeap || !rt.dsvHeap || rt.depthSrvHandle == 0 || rt.grabSrvHandle == 0) {
        return false;
    }

    // 背景色をコピーできるよう描画先を外し、色を転送元、退避先を転送先へ切り替える。
    commandList->OMSetRenderTargets(0, nullptr, false, nullptr);

    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = rt.resource.Get();
    barriers[0].Transition.StateBefore = rt.currentState;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = rt.grabResource.Get();
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    commandList->ResourceBarrier(2, barriers);

    // 屈折で読む背景色を別のテクスチャへ退避し、特殊パスで描き足す色との同時読み書きを避けます。
    commandList->CopyResource(rt.grabResource.Get(), rt.resource.Get());

    // コピー後は元の色を描画先に、退避した背景色をピクセルシェーダーの入力に切り替える。
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    commandList->ResourceBarrier(2, barriers);
    rt.currentState = D3D12_RESOURCE_STATE_RENDER_TARGET;

    constexpr D3D12_RESOURCE_STATES kReadOnlyDepthState =
        D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    // 深度が両方の読み取り用途を満たす状態でなければ、深度テストとPS参照を同時に許す状態へ遷移する。
    if (rt.depthState != kReadOnlyDepthState) {
        D3D12_RESOURCE_BARRIER depthBarrier{};
        depthBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        depthBarrier.Transition.pResource = rt.depthResource.Get();
        depthBarrier.Transition.StateBefore = rt.depthState;
        depthBarrier.Transition.StateAfter = kReadOnlyDepthState;
        commandList->ResourceBarrier(1, &depthBarrier);
        rt.depthState = kReadOnlyDepthState;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rt.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    // 同じ深度を深度テストとシェーダーの両方で読むので、書き込み用DSVの次に作成した読み取り専用DSVを使います。
    D3D12_CPU_DESCRIPTOR_HANDLE readOnlyDsv = rt.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    readOnlyDsv.ptr += dxCommon_->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    commandList->OMSetRenderTargets(1, &rtvHandle, false, &readOnlyDsv);
    return true;
}

// BeginCameraPreviewSpecialPass()で設定した読み取り用深度から、通常描画の書き込み用深度へ戻します。
void PostEffect::EndCameraPreviewSpecialPass(ID3D12GraphicsCommandList* commandList, std::int32_t targetTexIndex) {
    // コマンドリストがない、または描画先番号が範囲外なら、通常の描画先を設定できないため終了する。
    if (!commandList || targetTexIndex < 0 || targetTexIndex >= static_cast<std::int32_t>(renderTextures_.size())) {
        return;
    }

    RenderTexture& rt = renderTextures_[targetTexIndex];

    // 色・深度のリソースやビューが欠けている場合は、通常描画用の再接続を行えない。
    if (!rt.resource || !rt.depthResource || !rt.rtvHeap || !rt.dsvHeap) {
        return;
    }

    // 特殊パスの読み取り専用ビューを外してから、深度の状態を変更する。
    commandList->OMSetRenderTargets(0, nullptr, false, nullptr);
    // 読み取り専用DSVを外した後、深度がすでに書き込み可能でなければ状態を戻す。
    if (rt.depthState != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
        D3D12_RESOURCE_BARRIER depthBarrier{};
        depthBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        depthBarrier.Transition.pResource = rt.depthResource.Get();
        depthBarrier.Transition.StateBefore = rt.depthState;
        depthBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        commandList->ResourceBarrier(1, &depthBarrier);
        rt.depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }

    // 色と書き込み用の深度ビューを再接続し、通常描画を続けられるようにする。
    const D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rt.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = rt.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    commandList->OMSetRenderTargets(1, &rtvHandle, false, &dsvHandle);
}

// 接続済みの描画先へ、指定されたPSOと入力画像で全画面三角形を1枚描き、パス数を記録します。
void PostEffect::Draw(ID3D12GraphicsCommandList* commandList, uint32_t srvHandle, std::int32_t psoIndex) {
    commandList->SetGraphicsRootSignature(rootSignature_.Get());
    commandList->SetPipelineState(pipelineStates_[psoIndex].Get());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); // 3頂点で全画面を覆う

    // 全画面三角形の頂点はShader側で生成するため、頂点バッファを設定しない。
    commandList->SetGraphicsRootConstantBufferView(0, constBuffer_->GetGPUVirtualAddress());

    // [1] t0 のセット (メイン画像)
    engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 1, srvHandle);

    // [2] t1 のセット (LUT画像) 
    // LUTまたはノイズが未設定でもテーブルは有効なSRVを指すよう、主画像のSRVを代わりに束縛します。
    uint32_t currentLutHandle = (lutSrvHandle_ > 0) ? lutSrvHandle_ : srvHandle;
    engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 2, currentLutHandle);

    // [3] t2 のセット (深度テクスチャ)
    engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 3, dxCommon_->GetDepthSrvHandle());

    // [4] t3 のセット (ノイズテクスチャ)
    uint32_t currentNoiseHandle = (noiseSrvHandle_ > 0) ? noiseSrvHandle_ : srvHandle;
    engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 4, currentNoiseHandle);

    // 全画面三角形を描画し、描画回数とポストエフェクトのパス数に記録する。
    commandList->DrawInstanced(3, 1, 0, 0);
    engine::RenderStats::GetInstance()->RecordNonIndexedDraw(3, 1, 1);
    engine::RenderStats::GetInstance()->RecordPostProcessPass();
}

// シェーダー定数のアップロード用バッファを作り、経過時間0秒の既定値を書き込みます。
void PostEffect::CreateConstBuffer() {
    // サイズへ255を足して下位8ビットを消し、次の256バイト境界まで切り上げます。
    constBuffer_ = dxCommon_->CreateBufferResource((sizeof(Params) + 0xff) & ~0xff);
    constBuffer_->Map(0, nullptr, reinterpret_cast<void**>(&paramsData_));
    *paramsData_ = MakeNeutralPostEffectParams(0.0f);
}

// 描画先の色・深度と各ビューを用意します。プレビュー用では深度参照と背景コピーのSRVも設定します。
void PostEffect::CreateRenderTexture(std::int32_t texIndex, std::int32_t width, std::int32_t height, DXGI_FORMAT format) {
    ID3D12Device* device = dxCommon_->GetDevice();

    RenderTexture& rt = renderTextures_[texIndex];
    const bool needsPreviewSampling =
        texIndex == kCameraPreviewTextureIndex ||
        texIndex == kCinematicCameraPreviewTextureIndex;

    // 指定されたピクセル寸法で、描画先として使える1枚の色テクスチャを作る。
    D3D12_RESOURCE_DESC resDesc = {};
    resDesc.Width = width;
    resDesc.Height = height;
    resDesc.MipLevels = 1;
    resDesc.DepthOrArraySize = 1;
    // 作成する描画先の形式は、呼び出し元が指定した形式に合わせる。
    resDesc.Format = format;
    resDesc.SampleDesc.Count = 1;
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    // 最適化クリア値はPreDrawSceneで指定する不透明のグレーに合わせる。
    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = format;
    clearValue.Color[0] = 0.1f;
    clearValue.Color[1] = 0.1f;
    clearValue.Color[2] = 0.1f;
    clearValue.Color[3] = 1.0f;

    D3D12_HEAP_PROPERTIES heapProps = { D3D12_HEAP_TYPE_DEFAULT };
    Microsoft::WRL::ComPtr<ID3D12Resource> newResource;
    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,
        IID_PPV_ARGS(&newResource)
    );

    // 色リソースの作成に失敗した場合は、ビューを作らずに終了する。
    if (FAILED(hr) || !newResource) {
        return;
    }

    // 色の1枚分の描画ビューを、CPUから設定できる専用のヒープに作る。
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 1;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> newRtvHeap;
    hr = device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&newRtvHeap));

    // 色を描画先として接続するヒープを確保できなければ、色のビューを設定できない。
    if (FAILED(hr) || !newRtvHeap) {
        return;
    }

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = format;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(newResource.Get(), &rtvDesc, newRtvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.MipLevels = 1;
    depthDesc.DepthOrArraySize = 1;

    // DSVとSRVの両方から参照するため、Resource本体はTypelessで作成します。
    depthDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE depthClearValue = {};
    depthClearValue.Format = dxCommon_->GetDsvFormat();
    depthClearValue.DepthStencil.Depth = 1.0f;
    depthClearValue.DepthStencil.Stencil = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> newDepthResource;
    hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClearValue,
        IID_PPV_ARGS(&newDepthResource)
    );

    // 色と同じサイズの深度を確保できなければ、描画先としての登録を見送る。
    if (FAILED(hr) || !newDepthResource) {
        return;
    }

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 2;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> newDsvHeap;
    hr = device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&newDsvHeap));

    // 通常描画と特殊パスの深度ビューを置くヒープがなければ、描画先を登録できない。
    if (FAILED(hr) || !newDsvHeap) {
        return;
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = dxCommon_->GetDsvFormat();
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(newDepthResource.Get(), &dsvDesc, newDsvHeap->GetCPUDescriptorHandleForHeapStart());

    // 深度を画像として読むパスでも、深度テストに使う読み取り専用DSVを接続します。
    dsvDesc.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL;
    D3D12_CPU_DESCRIPTOR_HANDLE readOnlyDsv = newDsvHeap->GetCPUDescriptorHandleForHeapStart();
    readOnlyDsv.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    device->CreateDepthStencilView(newDepthResource.Get(), &dsvDesc, readOnlyDsv);

    // カメラプレビュー2種では、特殊シェーダーの深度参照用SRVを用意する。
    if (needsPreviewSampling) {
        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc{};
        depthSrvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Texture2D.MipLevels = 1;
        depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

        // 解像度変更で深度SRVがすでにあれば、その番号のビューを更新する。
        if (rt.depthSrvHandle != 0) {
            engine::SrvManager::GetInstance()->CreateSrvForResource(rt.depthSrvHandle, newDepthResource.Get(), depthSrvDesc);
        }
        // 深度SRVをまだ持たなければ、新しい番号を確保する。
        else {
            rt.depthSrvHandle = engine::SrvManager::GetInstance()->CreateSrv(newDepthResource.Get(), depthSrvDesc);
        }
    }

    // 出力色を後段のシェーダーやUIから読むためのビューを設定する。
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    // 再生成では従来のSRV番号を書き換え、ImGuiなどが保持する表示用ハンドルを維持します。
    if (rt.srvHandle != 0) {
        engine::SrvManager::GetInstance()->CreateSrvForResource(rt.srvHandle, newResource.Get(), srvDesc);
    // 初回の生成では、出力色を参照するためにSRV番号を確保する。
    } else {
        rt.srvHandle = engine::SrvManager::GetInstance()->CreateSrv(newResource.Get(), srvDesc);
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> newGrabResource;

    // カメラプレビュー2種に限り、特殊マテリアルが読む背景色のコピー先を作る。
    if (needsPreviewSampling) {
        D3D12_RESOURCE_DESC grabDesc = resDesc;
        grabDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        hr = device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &grabDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            nullptr,
            IID_PPV_ARGS(&newGrabResource));
        // 背景コピー先の確保に失敗したら保持するリソースの差し替え前に中断する。色と深度のSRVの設定はここより先に行われている。
        if (FAILED(hr) || !newGrabResource) {
            return;
        }

        // 背景コピーのSRVがすでにあれば、その番号のビューを新しいリソースへ向け直す。
        if (rt.grabSrvHandle != 0) {
            engine::SrvManager::GetInstance()->CreateSrvForResource(rt.grabSrvHandle, newGrabResource.Get(), srvDesc);
        }
        // 背景コピーのSRVをまだ持たなければ、新しい番号を確保する。
        else {
            rt.grabSrvHandle = engine::SrvManager::GetInstance()->CreateSrv(newGrabResource.Get(), srvDesc);
        }
    }

    // 用意したリソースを保存し、色と深度の状態追跡をそれぞれの生成直後の状態に揃える。
    rt.resource = newResource;
    rt.rtvHeap = newRtvHeap;
    rt.depthResource = newDepthResource;
    rt.dsvHeap = newDsvHeap;
    rt.grabResource = newGrabResource;
    rt.currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    rt.depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
}
