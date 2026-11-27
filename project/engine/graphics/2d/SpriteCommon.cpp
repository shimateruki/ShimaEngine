#include "SpriteCommon.h"
#include "DirectXCommon.h"
#include"SrvManager.h"
#include <cassert>

namespace engine {

// DirectX基盤を借用し、ルートシグネチャを作ってからSprite用のパイプラインを生成する。
void SpriteCommon::Initialize(engine::DirectXCommon* dxCommon) {
    assert(dxCommon);
    dxCommon_ = dxCommon;

    // パイプラインはルートシグネチャを参照するため、先に接続定義を作る。
    CreateRootSignature();
    CreatePipeline();
}

// Sprite共通のヒープ、パイプライン、三角形リストを指定した描画コマンドリストへ設定する。
void SpriteCommon::SetPipeline(ID3D12GraphicsCommandList* commandList) {
    // 画像SRVの番号を解決できるよう、Spriteのルートテーブルが参照する共有ヒープを先に設定します。
    engine::SrvManager::GetInstance()->SetDescriptorHeaps(commandList);

    // 描画前にパイプラインとルートシグネチャを設定する
    commandList->SetPipelineState(pipelineState_.Get());
    commandList->SetGraphicsRootSignature(rootSignature_.Get());

    // スプライト描画なので、プリミティブ形状は TRIANGLELIST に固定
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

// 頂点変換、色、画像のバインド先とサンプラーを定義し、GPUのルートシグネチャを作る。
void SpriteCommon::CreateRootSignature() {
    ID3D12Device* device = dxCommon_->GetDevice();

    // 頂点入力レイアウトを使うため、Input Assemblerを許可する。
    D3D12_ROOT_SIGNATURE_DESC descriptionRootSignature{};
    descriptionRootSignature.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    D3D12_ROOT_PARAMETER rootParameters[3] = {};
    // 頂点シェーダーのb0へ変換行列、ピクセルシェーダーのb1へ色などの材質データを渡します。
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rootParameters[0].Descriptor.ShaderRegister = 0;
    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[1].Descriptor.ShaderRegister = 1;

    // 1枚のスプライト画像をピクセルシェーダーのt0へ渡すテーブル。
    D3D12_DESCRIPTOR_RANGE descriptorRange[1] = {};
    descriptorRange[0].BaseShaderRegister = 0;
    descriptorRange[0].NumDescriptors = 1;
    descriptorRange[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptorRange[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[2].DescriptorTable.pDescriptorRanges = descriptorRange;
    rootParameters[2].DescriptorTable.NumDescriptorRanges = 1;

    descriptionRootSignature.pParameters = rootParameters;
    descriptionRootSignature.NumParameters = _countof(rootParameters);

    // ピクセルシェーダーのs0で、線形補間と繰り返しのサンプリングを使う。
    D3D12_STATIC_SAMPLER_DESC staticSamplers[1] = {};
    staticSamplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    staticSamplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    staticSamplers[0].ShaderRegister = 0;
    staticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    descriptionRootSignature.pStaticSamplers = staticSamplers;
    descriptionRootSignature.NumStaticSamplers = 1;

    // 接続定義をバイナリに変換し、そのデータからデバイスへ登録する。
    Microsoft::WRL::ComPtr<ID3DBlob> signatureBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&descriptionRootSignature, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob, &errorBlob);

    // 接続定義をシリアライズできない場合は、デバッグのassertで失敗を知らせる。
    if (FAILED(hr)) {
        assert(false);
    }

    hr = device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(), IID_PPV_ARGS(&rootSignature_));
    assert(SUCCEEDED(hr));
}

// 頂点形式、アルファ合成、深度を使わない描画とSprite用シェーダーを組み合わせてPSOを作る。
void SpriteCommon::CreatePipeline() {
    ID3D12Device* device = dxCommon_->GetDevice();

    // 頂点入力として、同次座標とテクスチャ座標を1つの頂点から読み取る。
    D3D12_INPUT_ELEMENT_DESC inputElementDescs[2] = {};
    inputElementDescs[0] = { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    inputElementDescs[1] = { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    D3D12_INPUT_LAYOUT_DESC inputLayoutDesc = { inputElementDescs, _countof(inputElementDescs) };

    // RGBは画像のアルファで背景と合成し、出力先にすでにあるアルファは保持します。
    D3D12_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;

    // 表裏の両方を描けるよう、三角形のカリングを無効にする。
    D3D12_RASTERIZER_DESC rasterizerDesc{};
    rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;

    D3D12_DEPTH_STENCIL_DESC depthStencilDesc{};
    // 3Dの深度を参照しないため、重なり順はSpriteの描画順で決まります。
    depthStencilDesc.DepthEnable = false;

    // Sprite描画専用の頂点・ピクセルシェーダーを、対応するプロファイルでコンパイルする。
    auto vsBlob = dxCommon_->CompileShader(L"Resources/shader/sprite/Sprite.VS.hlsl", L"vs_6_0");
    auto psBlob = dxCommon_->CompileShader(L"Resources/shader/sprite/Sprite.PS.hlsl", L"ps_6_0");

    // 画面色の描画先に合わせて16ビット浮動小数点のRTV形式を指定する。
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSignature_.Get();
    psoDesc.InputLayout = inputLayoutDesc;
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.BlendState = blendDesc;
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.DepthStencilState = depthStencilDesc;
    psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;

    // 組み立てた設定でデバイスにパイプラインを作り、作成の成否をassertで確認する。
    HRESULT hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState_));
    assert(SUCCEEDED(hr));
}

} // namespace engine
