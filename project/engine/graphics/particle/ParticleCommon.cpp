#include <cstdint>
#include "ParticleCommon.h"
#include "DirectXCommon.h"
#include "RootSignatureBuilder.h"
#include "GraphicsPipelineBuilder.h"
#include <cassert>

void engine::ParticleCommon::Initialize(engine::DirectXCommon* dxCommon) {
    // 初期化済みの描画基盤から、粒子が使うルート構成と合成方式別のPSOを順に作る。
    assert(dxCommon);
    dxCommon_ = dxCommon;
    CreateRootSignature();
    CreatePipeline();
}


void engine::ParticleCommon::CreateRootSignature() {
    RootSignatureBuilder builder;

    // =================================================================
    // 1. ルートパラメータの設定
    // =================================================================

    // [0] Camera (CBV b0 - VertexShader用)
    builder.AddCbv(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);

    // [1] Texture (DescriptorTable t0 - PixelShader用)
    builder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // =================================================================
    // 2. サンプラーの設定
    // =================================================================

    // s0: テクスチャサンプラー (リニア補間、ラップ - PixelShader用)
    builder.AddStaticSampler(0, 0, D3D12_SHADER_VISIBILITY_PIXEL, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    // =================================================================
    // 3. ビルド実行
    // =================================================================
    builder.Build(dxCommon_->GetDevice(), rootSignature_.GetAddressOf());
}


void engine::ParticleCommon::SetPipeline(ID3D12GraphicsCommandList* commandList, ParticleBlendMode mode) {
    // 定義済みのモード数以上なら対応するPSOがないため、コマンドを設定せず戻る。
    if ((std::int32_t)mode >= (std::int32_t)ParticleBlendMode::kCount)
        return;

    // 粒子のインスタンス描画で使うパイプライン・ルート構成・三角形リストを設定する。
    commandList->SetPipelineState(graphicsPipelines_[(std::int32_t)mode].Get());
    commandList->SetGraphicsRootSignature(rootSignature_.Get());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}


void engine::ParticleCommon::CreatePipeline() {
    ID3D12Device* device = dxCommon_->GetDevice();

    // インスタンスを展開する頂点シェーダーと、色を合成するピクセルシェーダーをビルドする。
    auto vsBlob = dxCommon_->CompileShader(L"Resources/shader/particle/Particle.VS.hlsl", L"vs_6_0");
    auto psBlob = dxCommon_->CompileShader(L"Resources/shader/particle/Particle.PS.hlsl", L"ps_6_0");

    // =================================================================
    // 1. 入力レイアウト (インスタンシング対応の特殊レイアウト)
    // =================================================================
    // 頂点ごとに位置・UVを読み、粒子が一個進むたびに色とワールド行列を次の要素へ進めます。
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "WORLD",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "WORLD",    1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "WORLD",    2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "WORLD",    3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
    };

    // =================================================================
    // 2. PSOの共通設定
    // =================================================================
    GraphicsPipelineBuilder builder;
    builder.SetRootSignature(rootSignature_.Get());
    builder.SetInputLayout(inputLayout, _countof(inputLayout));
    builder.SetShaders(vsBlob.Get(), psBlob.Get());

    // カリングなし（両面描画）
    builder.SetRasterizerState(D3D12_CULL_MODE_NONE, D3D12_FILL_MODE_SOLID);

    // レンダーターゲットと深度フォーマット
    DXGI_FORMAT rtvFormats[] = { DXGI_FORMAT_R16G16B16A16_FLOAT };
    builder.SetRenderTargets(1, rtvFormats, DXGI_FORMAT_D24_UNORM_S8_UINT);

    // =================================================================
    // 3. ブレンドモードごとの生成ループ
    // =================================================================
    for (std::int32_t i = 0; i < (std::int32_t)ParticleBlendMode::kCount; ++i) {
        ParticleBlendMode pMode = static_cast<ParticleBlendMode>(i);

        // 粒子用のモードを、PSOビルダーが使う共通の合成方式へ変換する。
        ::BlendMode globalMode = ::BlendMode::kNormal;

        switch (pMode) {
        // 通常粒子は自身のアルファで背景色と混ぜる。
        case ParticleBlendMode::kAlpha:    globalMode = ::BlendMode::kNormal;
        break;

        // 光などの粒子を背景へ足し合わせる。
        case ParticleBlendMode::kAdd:      globalMode = ::BlendMode::kAdd;
        break;

        // 着色した粒子の分だけ背景から引く。
        case ParticleBlendMode::kSubtract: globalMode = ::BlendMode::kSubtract;
        break;

        // 粒子の色を背景へ掛ける。
        case ParticleBlendMode::kMultiply: globalMode = ::BlendMode::kMultiply;
        break;

        // 明るい背景ほど加算を弱めるスクリーン合成を選ぶ。
        case ParticleBlendMode::kScreen:   globalMode = ::BlendMode::kScreen;
        break;
        }

        builder.SetBlendMode(globalMode);

        // シーンの深度で粒子を隠し、重なる粒子同士の合成に備えて深度書き込みを無効にする。
     
        builder.SetDepthStencilState(true, D3D12_DEPTH_WRITE_MASK_ZERO, D3D12_COMPARISON_FUNC_LESS_EQUAL);

        builder.Build(device, graphicsPipelines_[i].GetAddressOf());
    }
}