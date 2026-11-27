#include "ModelCommon.h"

#include "DirectXCommon.h"
#include "RootSignatureBuilder.h"

#include <cassert>

void ModelCommon::Initialize(engine::DirectXCommon* dxCommon) {
    // 初期化済みのDirectX基盤を借用し、頂点変形に使うComputeパイプラインを準備する。
    assert(dxCommon);
    dxCommon_ = dxCommon;
    CreateComputeSkinningPipeline();
}

void ModelCommon::CreateComputeSkinningPipeline() {
    // 再初期化時に古いPSOとルートシグネチャを持ち越さない。
    computeSkinningRootSignature_.Reset();
    computeSkinningPipelineState_.Reset();

    // 入力頂点・ボーン行列用のSRVと、変形後の頂点を書き込むUAVをシェーダーと同じ順に割り当てる。
    RootSignatureBuilder rootSignatureBuilder;
    rootSignatureBuilder.AddSrv(0);
    rootSignatureBuilder.AddSrv(1);
    rootSignatureBuilder.AddUav(0);

    // Root Descriptorは要素数を保持しないため、頂点数とボーン数を明示的に渡す。
    rootSignatureBuilder.AddConstants(0, 2);
    rootSignatureBuilder.Build(
        dxCommon_->GetDevice(),
        computeSkinningRootSignature_.GetAddressOf(),
        D3D12_ROOT_SIGNATURE_FLAG_NONE);

    // ルートパラメーターを確定してから、Computeシェーダーのバイトコードを取得する。
    auto computeShader = dxCommon_->CompileShader(
        L"Resources/shader/object3d/Skinning.CS.hlsl",
        L"cs_6_0");

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};
    pipelineDesc.pRootSignature = computeSkinningRootSignature_.Get();
    pipelineDesc.CS = {
        computeShader->GetBufferPointer(),
        computeShader->GetBufferSize()
    };

    // バイトコードとルート構成からGPU側のパイプラインを作り、生成失敗はアサートで検出する。
    const HRESULT result = dxCommon_->GetDevice()->CreateComputePipelineState(
        &pipelineDesc,
        IID_PPV_ARGS(computeSkinningPipelineState_.GetAddressOf()));
    assert(SUCCEEDED(result));
}
