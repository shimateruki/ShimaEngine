#include "GpuParticleSystem.h"
#include "RenderStats.h"
#include "SrvManager.h"
#include <d3dcompiler.h>
#include <cassert>
#include "DebugConsole.h"
#include "WinApp.h"
#include "RootSignatureBuilder.h"
#include "GraphicsPipelineBuilder.h"
#include <TextureManager.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <mutex>

#pragma comment(lib, "d3dcompiler.lib")

// 粒子数を対応範囲に収め、描画と計算が参照するGPU資源を作成する。
void GpuParticleSystem::Initialize(engine::DirectXCommon* dxCommon, uint32_t maxParticles) {
    assert(dxCommon);
    dxCommon_ = dxCommon;
    maxParticles_ = std::clamp(maxParticles, kMinParticles, kMaxParticles);
    CreateBuffer();
    CreateComputePipeline();
}

// 粒子と管理用番号はGPUに置き、CPUから更新する定数はアップロード用の領域へ分ける。
void GpuParticleSystem::CreateBuffer() {
    auto device = dxCommon_->GetDevice();

    // UAVバッファ作成
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC resDesc{};
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resDesc.Width = sizeof(Particle) * maxParticles_;
    resDesc.Height = 1;
    resDesc.DepthOrArraySize = 1;
    resDesc.MipLevels = 1;
    resDesc.SampleDesc.Count = 1;
    resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&particleBuffer_)
    );
    assert(SUCCEEDED(hr));

    // 再利用可能な粒子枠の先頭を整数一つで管理し、枠番号の配列とは別のバッファにします。
    resDesc.Width = sizeof(int32_t);
    hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&freeListIndexBuffer_)
    );
    assert(SUCCEEDED(hr));

    // 空き枠番号と生存枠番号は別々に持ち、描画側は生存枠だけを参照する。
    resDesc.Width = sizeof(uint32_t) * maxParticles_;
    hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&freeListBuffer_)
    );
    assert(SUCCEEDED(hr));

    hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&aliveParticleIndexBuffer_)
    );
    assert(SUCCEEDED(hr));

    // ComputeがDrawInstancedと同じ引数形式を作り、描画時はGPUメモリのまま参照します。
    resDesc.Width = sizeof(D3D12_DRAW_ARGUMENTS);
    hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&indirectDrawArgumentBuffer_)
    );
    assert(SUCCEEDED(hr));

    // Computeのu0～u4を一つのDescriptor Tableで渡すため、以下の5つは連続した順序で確保する。
    uavIndex_ = engine::SrvManager::GetInstance()->Allocate();
    freeListIndexUav_ = engine::SrvManager::GetInstance()->Allocate();
    freeListUav_ = engine::SrvManager::GetInstance()->Allocate();
    aliveParticleIndexUav_ = engine::SrvManager::GetInstance()->Allocate();
    indirectDrawArgumentUav_ = engine::SrvManager::GetInstance()->Allocate();
    srvIndex_ = engine::SrvManager::GetInstance()->Allocate();
    aliveParticleIndexSrv_ = engine::SrvManager::GetInstance()->Allocate();

    // 書き込みビューは粒子データ・枠番号・描画引数の要素型と数に合わせる。
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = maxParticles_;
    uavDesc.Buffer.StructureByteStride = sizeof(Particle);
    device->CreateUnorderedAccessView(particleBuffer_.Get(), nullptr, &uavDesc, engine::SrvManager::GetInstance()->GetCpuDescriptorHandle(uavIndex_));

    uavDesc.Buffer.NumElements = 1;
    uavDesc.Buffer.StructureByteStride = sizeof(int32_t);
    device->CreateUnorderedAccessView(freeListIndexBuffer_.Get(), nullptr, &uavDesc, engine::SrvManager::GetInstance()->GetCpuDescriptorHandle(freeListIndexUav_));

    uavDesc.Buffer.NumElements = maxParticles_;
    uavDesc.Buffer.StructureByteStride = sizeof(uint32_t);
    device->CreateUnorderedAccessView(freeListBuffer_.Get(), nullptr, &uavDesc, engine::SrvManager::GetInstance()->GetCpuDescriptorHandle(freeListUav_));

    device->CreateUnorderedAccessView(aliveParticleIndexBuffer_.Get(), nullptr, &uavDesc, engine::SrvManager::GetInstance()->GetCpuDescriptorHandle(aliveParticleIndexUav_));

    uavDesc.Buffer.NumElements = 4;
    uavDesc.Buffer.StructureByteStride = sizeof(uint32_t);
    device->CreateUnorderedAccessView(indirectDrawArgumentBuffer_.Get(), nullptr, &uavDesc, engine::SrvManager::GetInstance()->GetCpuDescriptorHandle(indirectDrawArgumentUav_));

    // 粒子と生存枠番号は計算後に頂点シェーダーから索引できるよう読み取りビューも作る。
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.NumElements = maxParticles_;
    srvDesc.Buffer.StructureByteStride = sizeof(Particle);
    engine::SrvManager::GetInstance()->CreateSrvForResource(srvIndex_, particleBuffer_.Get(), srvDesc);

    srvDesc.Buffer.NumElements = maxParticles_;
    srvDesc.Buffer.StructureByteStride = sizeof(uint32_t);
    engine::SrvManager::GetInstance()->CreateSrvForResource(aliveParticleIndexSrv_, aliveParticleIndexBuffer_.Get(), srvDesc);

    // 0番は更新用、1番以降は各Emit用。CSConfigの256バイト境界により各CBVの先頭位置を揃える。
    configBuffer_ = dxCommon_->CreateBufferResource(sizeof(CSConfig) * (kMaxEmitRequests + 1));
    configBuffer_->Map(0, nullptr, reinterpret_cast<void**>(&configData_));
    configData_->deltaTime = 0.0f;
    configData_->time = 0.0f;
    configData_->emitCount = 0;
    configData_->maxParticles = maxParticles_;

    cameraBuffer_ = dxCommon_->CreateBufferResource(sizeof(CameraData));
    cameraBuffer_->Map(0, nullptr, reinterpret_cast<void**>(&cameraData_));

    // メッシュを持たない発生や更新でも、ルートシグネチャが要求する頂点・ボーンの入力を有効な代替資源で揃えます。
    dummyVertexBuffer_ = dxCommon_->CreateBufferResource(64);
    engine::Vector3 dummyPos = engine::Vector3::kZero;
    void* mapped = nullptr;
    dummyVertexBuffer_->Map(0, nullptr, &mapped);
    memcpy(mapped, &dummyPos, sizeof(engine::Vector3));
    dummyVertexBuffer_->Unmap(0, nullptr);

    dummyBoneBuffer_ = dxCommon_->CreateBufferResource(sizeof(engine::Matrix4x4));
    engine::Matrix4x4 identityMat = { 1.0f,0.0f,0.0f,0.0f, 0.0f,1.0f,0.0f,0.0f, 0.0f,0.0f,1.0f,0.0f, 0.0f,0.0f,0.0f,1.0f };
    void* boneMapped = nullptr;
    dummyBoneBuffer_->Map(0, nullptr, &boneMapped);
    memcpy(boneMapped, &identityMat, sizeof(engine::Matrix4x4));
    dummyBoneBuffer_->Unmap(0, nullptr);

    D3D12_SHADER_RESOURCE_VIEW_DESC boneSrvDesc{};
    boneSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    boneSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    boneSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    boneSrvDesc.Buffer.FirstElement = 0;
    boneSrvDesc.Buffer.NumElements = 1;
    boneSrvDesc.Buffer.StructureByteStride = sizeof(engine::Matrix4x4);
    boneSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    dummyBoneSrvIndex_ = engine::SrvManager::GetInstance()->Allocate();
    engine::SrvManager::GetInstance()->CreateSrvForResource(dummyBoneSrvIndex_, dummyBoneBuffer_.Get(), boneSrvDesc);

    // 個別テクスチャが無い場合も初期描画で有効なSRVを使えるよう、白テクスチャを読み込む。
    currentTextureHandle_ = engine::TextureManager::GetInstance()->Load("Resources/sprite/particle/white.png");

    CreateGraphicsPipeline();
}

// 初期化、描画数の初期化、更新、発生を別のComputeパイプラインとして作る。
void GpuParticleSystem::CreateComputePipeline() {
    auto device = dxCommon_->GetDevice();
    RootSignatureBuilder rsBuilder;
    rsBuilder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 5, 0, D3D12_SHADER_VISIBILITY_ALL);
    rsBuilder.AddCbv(0, 0, D3D12_SHADER_VISIBILITY_ALL);
    rsBuilder.AddSrv(0, 0, D3D12_SHADER_VISIBILITY_ALL);
    rsBuilder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, D3D12_SHADER_VISIBILITY_ALL);
    rsBuilder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 1, 0, D3D12_SHADER_VISIBILITY_ALL);
    rsBuilder.AddStaticSampler(0, 0, D3D12_SHADER_VISIBILITY_ALL, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    rsBuilder.Build(device, computeRootSignature_.GetAddressOf());

    // プリセットごとに同じ4本を再コンパイルするとロード時の処理が増える。
    // バイトコードだけ共有し、粒子バッファとPSOは各Systemで生成する。
    constexpr wchar_t kSourcePath[] = L"Resources/shader/particle/ParticleCS.hlsl";
    constexpr const char* kEntryPoints[] = {"InitCS", "ResetDrawArgumentsCS", "UpdateCS", "EmitCS"};
    static std::mutex sShaderCacheMutex;
    static std::filesystem::file_time_type sCachedSourceTime{};
    static std::array<Microsoft::WRL::ComPtr<ID3DBlob>, 4> sCachedBlobs{};
    std::array<Microsoft::WRL::ComPtr<ID3DBlob>, 4> shaderBlobs;
    {
        std::lock_guard<std::mutex> lock(sShaderCacheMutex);
        std::error_code timeError;
        const auto sourceTime = std::filesystem::last_write_time(kSourcePath, timeError);

        // 更新時刻を読めない、キャッシュが空、またはソース時刻が変わった場合は、全計算パスをコンパイルする。
        if (timeError || !sCachedBlobs[0] || sCachedSourceTime != sourceTime) {
            std::array<Microsoft::WRL::ComPtr<ID3DBlob>, 4> rebuiltBlobs;

            for (size_t index = 0; index < rebuiltBlobs.size(); ++index) {
                Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
                const HRESULT compileHr = D3DCompileFromFile(
                    kSourcePath, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                    kEntryPoints[index], "cs_5_0", 0, 0, &rebuiltBlobs[index], &errorBlob);
                // 一つでもコンパイルが失敗したら、取得できた診断を出し、計算パイプラインの作成を進めない。
                if (FAILED(compileHr)) {
                    OutputDebugStringA(errorBlob
                        ? static_cast<const char*>(errorBlob->GetBufferPointer())
                        : "ParticleCS compilation failed.\n");
                    assert(false);
                    return;
                }
            }

            // 有効な更新時刻を取得できた場合だけ、今回のバイトコードを時刻と組にして保存する。
            if (!timeError) {
                sCachedSourceTime = sourceTime;
                sCachedBlobs = rebuiltBlobs;
            }

            shaderBlobs = std::move(rebuiltBlobs);
        // ソース時刻に対応するバイトコードが揃っている場合は、再コンパイルなしで使用する。
        } else {
            shaderBlobs = sCachedBlobs;
        }
    }

    // 共有したバイトコードから、各パスのパイプラインをこのインスタンス用に作る。
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = computeRootSignature_.Get();

    psoDesc.CS = { shaderBlobs[0]->GetBufferPointer(), shaderBlobs[0]->GetBufferSize() };
    HRESULT hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&computePipelineStateInit_));
    assert(SUCCEEDED(hr));

    psoDesc.CS = { shaderBlobs[1]->GetBufferPointer(), shaderBlobs[1]->GetBufferSize() };
    hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&computePipelineStateResetDrawArguments_));
    assert(SUCCEEDED(hr));

    psoDesc.CS = { shaderBlobs[2]->GetBufferPointer(), shaderBlobs[2]->GetBufferSize() };
    hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&computePipelineStateUpdate_));
    assert(SUCCEEDED(hr));

    psoDesc.CS = { shaderBlobs[3]->GetBufferPointer(), shaderBlobs[3]->GetBufferSize() };
    hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&computePipelineStateEmit_));
    assert(SUCCEEDED(hr));
}

// GPUの粒子データを直接読んで描画するため、入力レイアウトなしの描画パイプラインを作る。
void GpuParticleSystem::CreateGraphicsPipeline() {
    auto device = dxCommon_->GetDevice();
    RootSignatureBuilder rsBuilder;
    rsBuilder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1, 0, D3D12_SHADER_VISIBILITY_ALL);
    rsBuilder.AddCbv(0, 0, D3D12_SHADER_VISIBILITY_ALL);
    rsBuilder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, D3D12_SHADER_VISIBILITY_ALL);
    rsBuilder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 1, 0, D3D12_SHADER_VISIBILITY_ALL);
    rsBuilder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 1, 0, D3D12_SHADER_VISIBILITY_ALL);
    rsBuilder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 1, 0, D3D12_SHADER_VISIBILITY_VERTEX);
    rsBuilder.AddStaticSampler(0, 0, D3D12_SHADER_VISIBILITY_PIXEL, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    rsBuilder.Build(device, graphicsRootSignature_.GetAddressOf());

    auto vsBlob = engine::DirectXCommon::GetInstance()->CompileShader(L"Resources/shader/particle/GPUParticleVS.hlsl", L"vs_6_0");
    auto psBlob = engine::DirectXCommon::GetInstance()->CompileShader(L"Resources/shader/particle/GPUParticlePS.hlsl", L"ps_6_0");

    GraphicsPipelineBuilder psoBuilder;
    psoBuilder.SetRootSignature(graphicsRootSignature_.Get());
    psoBuilder.SetInputLayout(nullptr, 0);
    psoBuilder.SetShaders(vsBlob.Get(), psBlob.Get());
    psoBuilder.SetRasterizerState(D3D12_CULL_MODE_NONE, D3D12_FILL_MODE_SOLID);

    // 深度はSRVで参照するため、遮蔽判定はピクセルシェーダー側で行います。
    psoBuilder.SetDepthStencilState(false, D3D12_DEPTH_WRITE_MASK_ZERO, D3D12_COMPARISON_FUNC_LESS_EQUAL);
    DXGI_FORMAT rtvFormats[] = { DXGI_FORMAT_R16G16B16A16_FLOAT };
    psoBuilder.SetRenderTargets(1, rtvFormats, DXGI_FORMAT_UNKNOWN);

    // 加算と通常のアルファ合成でパイプラインを分け、歪みの描画も通常のブレンド設定を使う。
    psoBuilder.SetBlendMode(::BlendMode::kAdd);
    psoBuilder.Build(device, graphicsPipelineStateAdd_.GetAddressOf());
    psoBuilder.SetBlendMode(::BlendMode::kNormal);
    psoBuilder.Build(device, graphicsPipelineStateAlpha_.GetAddressOf());
    psoBuilder.Build(device, graphicsPipelineStateDistortion_.GetAddressOf());

    // Computeが組み立てたD3D12_DRAW_ARGUMENTSを、そのまま間接描画命令として実行できるよう登録する。
    D3D12_INDIRECT_ARGUMENT_DESC argumentDesc{};
    argumentDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC commandSignatureDesc{};
    commandSignatureDesc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
    commandSignatureDesc.NumArgumentDescs = 1;
    commandSignatureDesc.pArgumentDescs = &argumentDesc;
    HRESULT hr = device->CreateCommandSignature(
        &commandSignatureDesc,
        nullptr,
        IID_PPV_ARGS(&indirectDrawCommandSignature_));
    assert(SUCCEEDED(hr));
}

void GpuParticleSystem::Update(float deltaTime) {
    // CPU側は時間だけを蓄積する。Drawを挟まない複数回のUpdateは、GPU上の複数ステップとは異なる。
    float scaledDeltaTime = deltaTime * timeScale_;
    totalTime_ += scaledDeltaTime;
    frameDeltaTime_ += scaledDeltaTime;

    // 最後に発生してからの秒数は、Drawで更新と描画を省く判定に使う。
    lastEmitTimer_ += scaledDeltaTime;
}

void GpuParticleSystem::RequestSimulationReset() {
    // プリセット再利用時は次回のウォームアップを残し、粒子の時刻だけ初期状態へ戻す。
    emitRequests_.clear();
    emitCountThisFrame_ = 0;
    lastConfig_ = {};
    totalTime_ = 0.0f;
    frameDeltaTime_ = 0.0f;
    lastEmitTimer_ = 0.0f;
    isInitialized_ = false;
    warmupRequested_ = true;
}

void GpuParticleSystem::ResetForSceneTransition() {
    // シーン切替時は旧シーンの発生要求を破棄し、自動ウォームアップも持ち越さない。
    emitRequests_.clear();
    emitCountThisFrame_ = 0;
    lastConfig_ = {};
    totalTime_ = 0.0f;
    frameDeltaTime_ = 0.0f;
    lastEmitTimer_ = activeLifetimeWindow_ + 0.001f;
    isInitialized_ = false;
    warmupRequested_ = false;
}

void GpuParticleSystem::Draw(ID3D12GraphicsCommandList* commandList, const engine::Matrix4x4& viewMatrix, const engine::Matrix4x4& projectionMatrix, uint32_t dummyTex, uint32_t depthSrvHandle) {
    // コマンドリストを取得できない場合は、GPU計算も描画も記録できない。
    if (!commandList)
        return;

    // 最後の発生から活動期間を過ぎ、準備の要求もなければ、このフレームの更新・描画コマンドを出さない。
    if (lastEmitTimer_ > activeLifetimeWindow_ && !warmupRequested_) {
        frameDeltaTime_ = 0.0f;
        return;
    }

    const bool warmupOnly = warmupRequested_ && emitRequests_.empty();
    const bool rebuiltSimulationThisDraw = !isInitialized_;

    // Computeで参照する資源を結び付けてから、必要な計算パスを順に発行する。
    engine::SrvManager::GetInstance()->SetDescriptorHeaps(commandList);
    commandList->SetComputeRootSignature(computeRootSignature_.Get());
    engine::SrvManager::GetInstance()->SetComputeRootDescriptorTable(commandList, 0, uavIndex_);

    // GPU粒子の状態が未初期化の場合は、後続の更新や発生より先にバッファの初期化を記録する。
    if (!isInitialized_) {
        commandList->SetPipelineState(computePipelineStateInit_.Get());
        CSConfig initConfig{};
        initConfig.maxParticles = maxParticles_;
        configData_[0] = initConfig;
        commandList->SetComputeRootConstantBufferView(1, configBuffer_->GetGPUVirtualAddress());
        UINT groupCountInit = (maxParticles_ + 1023) / 1024;
        commandList->Dispatch(groupCountInit, 1, 1);
        engine::RenderStats::GetInstance()->RecordComputeDispatch(groupCountInit);
        
        // 初期化の書き込みを、更新・発生が同じ資源を読む前に完了させる。
        D3D12_RESOURCE_BARRIER barriers[3] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[0].UAV.pResource = particleBuffer_.Get();
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[1].UAV.pResource = freeListIndexBuffer_.Get();
        barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[2].UAV.pResource = freeListBuffer_.Get();
        commandList->ResourceBarrier(3, barriers);

        isInitialized_ = true;
    }

    // ウォームアップ要求があり発生要求が空の場合は、準備フラグを消費する。GPUの描画経路はこのフレームも通す。
    if (warmupOnly) {
        warmupRequested_ = false;
        lastEmitTimer_ = activeLifetimeWindow_ + 0.001f;

    }

    // ビューを投影と合成し、深度から位置を戻す更新・発生パスの座標変換に使う。
    static engine::Math sMath;
    engine::Matrix4x4 vp = sMath.Multiply(viewMatrix, projectionMatrix);

    D3D12_RESOURCE_BARRIER resetArgumentBarrier{};
    resetArgumentBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    resetArgumentBarrier.UAV.pResource = indirectDrawArgumentBuffer_.Get();
    D3D12_RESOURCE_BARRIER updateBarriers[5] = {};
    updateBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    updateBarriers[0].UAV.pResource = particleBuffer_.Get();
    updateBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    updateBarriers[1].UAV.pResource = freeListIndexBuffer_.Get();
    updateBarriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    updateBarriers[2].UAV.pResource = freeListBuffer_.Get();
    updateBarriers[3].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    updateBarriers[3].UAV.pResource = aliveParticleIndexBuffer_.Get();
    updateBarriers[4].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    updateBarriers[4].UAV.pResource = indirectDrawArgumentBuffer_.Get();

    // 生存数を0に戻して容量全体を走査し、描画対象番号を詰め直す。粒子本体の配列順は変えない。
    // dt=0でも色・サイズの再評価と深度衝突判定を通るため、純粋な番号列コピーではない。
    const auto rebuildAliveParticleList = [&](float simulationDeltaTime) {
        commandList->SetPipelineState(computePipelineStateResetDrawArguments_.Get());
        commandList->Dispatch(1, 1, 1);
        engine::RenderStats::GetInstance()->RecordComputeDispatch(1);
        // 生存粒子を数える更新シェーダーが古い描画引数へ加算しないよう、カウンターのゼロ書き込みを先に完了させます。
        commandList->ResourceBarrier(1, &resetArgumentBarrier);

        commandList->SetPipelineState(computePipelineStateUpdate_.Get());
        CSConfig updateConfig = lastConfig_;
        updateConfig.deltaTime = simulationDeltaTime;
        updateConfig.time = totalTime_;
        updateConfig.viewProj = vp;
        updateConfig.inverseViewProj = sMath.Inverse(vp);
        updateConfig.screenSize = { (float)WinApp::GetClientWidth(), (float)WinApp::GetClientHeight() };
        updateConfig.maxParticles = maxParticles_;
        configData_[0] = updateConfig;

        commandList->SetComputeRootConstantBufferView(1, configBuffer_->GetGPUVirtualAddress());
        commandList->SetComputeRootShaderResourceView(2, dummyVertexBuffer_->GetGPUVirtualAddress());
        const uint32_t updateBoneSrv = (dummyBoneSrvIndex_ > 0) ? dummyBoneSrvIndex_ : 0;
        engine::SrvManager::GetInstance()->SetComputeRootDescriptorTable(commandList, 3, updateBoneSrv);

        // 有効な深度SRVがある場合は、粒子の更新にシーンの深度を使う。
        if (depthSrvHandle > 0) {
            engine::SrvManager::GetInstance()->SetComputeRootDescriptorTable(commandList, 4, depthSrvHandle);
        // 深度SRVがない場合は、更新パスの参照枠にダミーのSRVを結び付ける。
        } else {
            engine::SrvManager::GetInstance()->SetComputeRootDescriptorTable(commandList, 4, dummyTex);
        }

        const UINT groupCountUpdate = (maxParticles_ + 255) / 256;
        commandList->Dispatch(groupCountUpdate, 1, 1);
        engine::RenderStats::GetInstance()->RecordComputeDispatch(groupCountUpdate);
        commandList->ResourceBarrier(5, updateBarriers);
    };

    // このフレームの発生より先に、既存粒子の時間を進めて生存枠を数え直す。
    rebuildAliveParticleList(frameDeltaTime_);

    const bool hasEmitRequests = !emitRequests_.empty();

    // 発生要求があるフレームだけ、GPUの発生パスを要求ごとに記録する。
    if (hasEmitRequests) {
        commandList->SetPipelineState(computePipelineStateEmit_.Get());

        for (size_t i = 0; i < emitRequests_.size(); ++i) {
            auto& req = emitRequests_[i];
            req.config.viewProj = vp;
            req.config.inverseViewProj = sMath.Inverse(vp);
            req.config.screenSize = { (float)WinApp::GetClientWidth(), (float)WinApp::GetClientHeight() };
            req.config.deltaTime = 0.0f; // 発生パスでは物理時間を進めない。

            // シーク中にCPU側へ溜めた要求は、初回Drawで年齢を付けて生成する。
            // Shaderは重力・風・減衰による近似復元で、途中の衝突やFieldの履歴を再実行しない。
            req.config.initialAge = rebuiltSimulationThisDraw ?
                (std::max)(0.0f, totalTime_ - req.config.time) : 0.0f;
            req.config.meshVertexCount = req.vb ? req.vCount : 1;
            req.config.meshVertexStride = req.vb ? req.vStride : sizeof(engine::Vector3);
            req.config.maxParticles = maxParticles_;

            // 更新用に0番を残し、発生要求一件につき1番以降の定数枠を一つ使う。
            configData_[i + 1] = req.config;
            D3D12_GPU_VIRTUAL_ADDRESS cbvAddress = configBuffer_->GetGPUVirtualAddress() + ((i + 1) * sizeof(CSConfig));
            commandList->SetComputeRootConstantBufferView(1, cbvAddress);

            ID3D12Resource* targetVB = req.vb ? req.vb : dummyVertexBuffer_.Get();
            commandList->SetComputeRootShaderResourceView(2, targetVB->GetGPUVirtualAddress());

            uint32_t boneSrv = (req.boneSrv > 0) ? req.boneSrv : dummyBoneSrvIndex_;
            engine::SrvManager::GetInstance()->SetComputeRootDescriptorTable(commandList, 3, boneSrv);

            // 深度SRVが有効なら、発生パスにもシーンの深度を渡す。
            if (depthSrvHandle > 0) {
                engine::SrvManager::GetInstance()->SetComputeRootDescriptorTable(commandList, 4, depthSrvHandle);
            // 深度が利用できない場合は、発生パスの深度枠へダミーを渡す。
            } else {
                engine::SrvManager::GetInstance()->SetComputeRootDescriptorTable(commandList, 4, dummyTex);
            }

            // 1グループ256スレッドで端数を切り上げ、要求数が256の倍数でなくても末尾まで処理します。
            UINT groupCountEmit = (req.config.emitCount + 255) / 256;
            commandList->Dispatch(groupCountEmit, 1, 1);
            engine::RenderStats::GetInstance()->RecordComputeDispatch(groupCountEmit);

            // 次の発生が同じ空き枠や粒子データを参照する前に、この要求の書き込みを完了させる。
            commandList->ResourceBarrier(5, updateBarriers);
        }
        
        // 次の更新で環境設定を使えるよう、最後の発生要求の定数を保持する。
        lastConfig_ = emitRequests_.back().config;
    }

    // 粒子バッファの初期化と粒子の発生を同じ描画で行った場合は、このフレームから新しい粒子を表示できるよう生存リストを作り直す。
    if (rebuiltSimulationThisDraw && hasEmitRequests) {
        rebuildAliveParticleList(0.0f);
        warmupRequested_ = false;
    }

    // 記録した発生要求と、この描画で消化した経過時間をクリアする。
    emitRequests_.clear();
    emitCountThisFrame_ = 0;
    frameDeltaTime_ = 0.0f;

    // 逆ビューからカメラの向きを取り出します。平行移動を消すと、各粒子の位置を保ったまま板だけをカメラに向けられます。
    engine::Matrix4x4 billboard = sMath.Inverse(viewMatrix);
    billboard.m[3][0] = 0.0f;
    billboard.m[3][1] = 0.0f;
    billboard.m[3][2] = 0.0f;

    cameraData_->viewProj = vp;
    cameraData_->billboardMatrix = billboard;
    cameraData_->projection = projectionMatrix;
    cameraData_->softParticleFade = softParticleFade_;
    cameraData_->blendMode = blendModeIndex_;
    cameraData_->screenSize = { (float)WinApp::GetClientWidth(), (float)WinApp::GetClientHeight() };
    cameraData_->spriteSheetColumns = spriteSheetColumns_;
    cameraData_->spriteSheetRows = spriteSheetRows_;
    cameraData_->spriteSheetFrameCount = spriteSheetFrameCount_;
    cameraData_->spriteSheetFps = spriteSheetFps_;
    cameraData_->spriteSheetLoop = spriteSheetLoop_;
    cameraData_->spriteSheetRandomStart = spriteSheetRandomStart_;
    cameraData_->alignToVelocity = alignToVelocity_;
    cameraData_->velocityStretch = velocityStretch_;
    cameraData_->particleType = particleType_;
    cameraData_->trailLength = trailLength_;
    cameraData_->receiveLighting = receiveLighting_;
    cameraData_->lightingStrength = lightingStrength_;
    cameraData_->lightDirection = lightDirection_;
    cameraData_->lightColor = lightColor_;

    // Compute用の書き込み状態から、粒子本体と生存番号は頂点シェーダー入力、描画引数は間接命令入力へ切り替えます。
    D3D12_RESOURCE_BARRIER drawBarriers[3] = {};
    drawBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    drawBarriers[0].Transition.pResource = particleBuffer_.Get();
    drawBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    drawBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    drawBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    drawBarriers[1].Transition.pResource = aliveParticleIndexBuffer_.Get();
    drawBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    drawBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    drawBarriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    drawBarriers[2].Transition.pResource = indirectDrawArgumentBuffer_.Get();
    drawBarriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    drawBarriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    commandList->ResourceBarrier(3, drawBarriers);

    // 加算を指定した粒子には、明るさを加えるブレンドのパイプラインを使う。
    if (blendModeIndex_ == 0) {
        commandList->SetPipelineState(graphicsPipelineStateAdd_.Get());
    }
    // 半透明を指定した粒子には、背景とアルファで混ぜるパイプラインを使う。
    else if (blendModeIndex_ == 1) {
        commandList->SetPipelineState(graphicsPipelineStateAlpha_.Get());
    }
    // 歪みを指定した粒子には、背景を参照するパイプラインを使う。
    else if (blendModeIndex_ == 2) {
        commandList->SetPipelineState(graphicsPipelineStateDistortion_.Get());
    }

    commandList->SetGraphicsRootSignature(graphicsRootSignature_.Get());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    // GPU粒子は頂点・インデックスバッファを使わないため、直前のメッシュ描画の入力を外す。
    commandList->IASetVertexBuffers(0, 0, nullptr);
    commandList->IASetIndexBuffer(nullptr);

    engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 0, srvIndex_);
    commandList->SetGraphicsRootConstantBufferView(1, cameraBuffer_->GetGPUVirtualAddress());


    uint32_t texToUse = (currentTextureHandle_ > 0) ? currentTextureHandle_ : dummyTex;

    // 選択できたテクスチャSRVが有効なら、描画のテクスチャ枠に結び付ける。
    if (texToUse > 0) {
        engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 2, texToUse);
    // 選択後も有効なハンドルが得られない場合は、渡されたダミーテクスチャを使う。
    } else {
        engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 2, dummyTex);
    }

    // 深度のハンドルがあれば、粒子描画時にシーンの深度を参照できるよう結び付ける。
    if (depthSrvHandle > 0) {
        engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 3, depthSrvHandle);
    // 深度がない描画経路では、同じ枠をダミーテクスチャで埋める。
    } else {
        engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 3, dummyTex);
    }

    uint32_t grabSrvHandle = dxCommon_->GetGrabSrvHandle();

    // 背景コピーのSRVがある場合は、画面の歪みに使えるよう描画時に結び付ける。
    if (grabSrvHandle > 0) {
        engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 4, grabSrvHandle);
    // 背景コピーがない場合も、参照枠にはダミーテクスチャを設定する。
    } else {
        engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 4, dummyTex);
    }

    engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 5, aliveParticleIndexSrv_);

    // 生存数をGPUが描画引数へ書くため、CPUへ読み戻すことなく生きている粒子分だけ描画します。
    commandList->ExecuteIndirect(
        indirectDrawCommandSignature_.Get(),
        1,
        indirectDrawArgumentBuffer_.Get(),
        0,
        nullptr,
        0);
    engine::RenderStats::GetInstance()->RecordNonIndexedIndirectDraw();
    engine::RenderStats::GetInstance()->RecordGpuParticleSystem(maxParticles_);

    // 次回の粒子更新に備え、描画用に読み取りへ切り替えた三つの資源をUAVへ戻します。
    for (D3D12_RESOURCE_BARRIER& barrier : drawBarriers) {
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    }

    commandList->ResourceBarrier(3, drawBarriers);
}

void GpuParticleSystem::EmitFromConfig(const GpuParticleConfig& config) {
    // 要求ごとに定数バッファの一枠を使うため、予約した枠数に達したフレームでは追加を受け付けません。
    if (emitRequests_.size() >= kMaxEmitRequests)
        return;

    // 寿命が2秒を超えるプリセットも途中で描画停止しないよう、活動期間を延長する。
    const float requestedLifetimeWindow = (std::max)(config.emitLife + 0.1f, 2.0f);

    // 以前の粒子がすべて寿命を終えた後なら、今回の設定の寿命を活動期間に使う。
    if (lastEmitTimer_ > activeLifetimeWindow_) {
        activeLifetimeWindow_ = requestedLifetimeWindow;
    }
    // 以前の粒子が残っている間は、新しい要求が来ても現在の活動期間を短くしない。
    else {
        activeLifetimeWindow_ = (std::max)(activeLifetimeWindow_, requestedLifetimeWindow);
    }

    lastEmitTimer_ = 0.0f;

    // 発生時の座標、寿命、変化曲線、力場を一件の定数として複製する。
    CSConfig reqConfig = {};
    reqConfig.time = totalTime_;
    reqConfig.emitPos = config.emitPos;
    reqConfig.emitArea = config.emitArea;
    reqConfig.emitVelocity = config.emitVelocity;
    reqConfig.emitLife = config.emitLife;
    reqConfig.velocityVariance = config.velocityVariance;
    reqConfig.baseColor = config.baseColor;
    reqConfig.gravity = config.envGravity;
    reqConfig.drag = config.envDrag;
    reqConfig.wind = config.envWind;
    reqConfig.turbulence = config.envTurbulence;
    reqConfig.baseSize = config.baseSize;
    reqConfig.endSize = config.endSize;
    reqConfig.rotSpeedVariance = config.rotSpeed;
    reqConfig.endColor = config.endColor;
    reqConfig.shapeType = static_cast<uint32_t>(config.shapeType);
    reqConfig.shapeRadius = config.shapeRadius;
    reqConfig.shapeAngle = config.shapeAngle;
    reqConfig.midColor = config.midColor;
    reqConfig.colorMidTime = config.colorMidTime;
    reqConfig.midSize = config.midSize;
    reqConfig.sizeMidTime = config.sizeMidTime;
    reqConfig.emitCount = config.emitCount;
    reqConfig.sizeEaseType = config.sizeEaseType;
    reqConfig.colorEaseType = config.colorEaseType;
    reqConfig.emitterWorldMatrix = config.emitterWorldMatrix;
    reqConfig.enableCollision = config.enableCollision;
    reqConfig.restitution = config.restitution;
    reqConfig.colorIntensity = config.colorIntensity;
    reqConfig.maxParticles = maxParticles_;
    reqConfig.fieldType = static_cast<uint32_t>(std::clamp(config.fieldType, 0, 3));
    reqConfig.fieldStrength = config.fieldStrength;
    reqConfig.fieldRadius = (std::max)(config.fieldRadius, 0.001f);
    reqConfig.fieldFalloff = (std::max)(config.fieldFalloff, 0.01f);
    // プリセット上の相対位置を発生地点に足し、力場の中心をワールド座標にします。
    reqConfig.fieldPosition = config.emitPos + config.fieldPosition;

    // 発生要求にその時点のメッシュ入力を結び付け、Drawまで保留する。
    EmitRequest request;
    request.config = reqConfig;
    request.vb = emitterVertexBuffer_;
    request.vCount = emitterVertexCount_;
    request.vStride = emitterVertexStride_;
    request.boneSrv = emitterBoneSrvIndex_;

    emitRequests_.push_back(request);

    // 描画設定は要求別の定数とは別に持ち、このインスタンスの粒子に共通で適用する。
    blendModeIndex_ = static_cast<uint32_t>(std::clamp(config.blendModeIndex, 0, 2));
    softParticleFade_ = config.softParticleFade;
    spriteSheetColumns_ = config.spriteSheetColumns > 0 ? static_cast<uint32_t>(config.spriteSheetColumns) : 1u;
    spriteSheetRows_ = config.spriteSheetRows > 0 ? static_cast<uint32_t>(config.spriteSheetRows) : 1u;
    spriteSheetFrameCount_ = config.spriteSheetFrameCount > 0 ? static_cast<uint32_t>(config.spriteSheetFrameCount) : 1u;
    spriteSheetFps_ = config.spriteSheetFps > 0.0f ? config.spriteSheetFps : 0.0f;
    spriteSheetLoop_ = config.spriteSheetLoop != 0 ? 1u : 0u;
    spriteSheetRandomStart_ = config.spriteSheetRandomStart != 0 ? 1u : 0u;
    // 整列モードは0〜2で扱い、固定整列を指定した場合も値2をそのままシェーダーへ渡す。
    alignToVelocity_ = static_cast<uint32_t>(std::clamp(config.alignToVelocity, 0, 2));
    velocityStretch_ = (std::max)(config.velocityStretch, 0.0f);
    particleType_ = config.particleType == 1 ? 1u : 0u;
    trailLength_ = (std::max)(config.trailLength, 0.0f);
    receiveLighting_ = config.receiveLighting != 0 ? 1u : 0u;
    lightingStrength_ = std::clamp(config.lightingStrength, 0.0f, 1.0f);
    lightDirection_ = config.lightDirection;
    lightColor_ = config.lightColor;
}

size_t GpuParticleSystem::GetEstimatedMemoryBytes() const {
    // CPU側の放出要求とGPUリソースの確保幅を合算し、診断表示用の概算値にする。
    size_t bytes = emitRequests_.capacity() * sizeof(EmitRequest);
    ID3D12Resource* resources[] = {
        particleBuffer_.Get(),
        freeListIndexBuffer_.Get(),
        freeListBuffer_.Get(),
        aliveParticleIndexBuffer_.Get(),
        indirectDrawArgumentBuffer_.Get(),
        configBuffer_.Get(),
        cameraBuffer_.Get(),
        dummyVertexBuffer_.Get(),
        dummyBoneBuffer_.Get()
    };

    for (ID3D12Resource* resource : resources) {
        // 作成されたGPUバッファだけ、確保しているバイト数の見積もりに含める。
        if (resource) {
            bytes += static_cast<size_t>(resource->GetDesc().Width);
        }
    }

    return bytes;
}

void GpuParticleSystem::SetCurrentTexture(const std::string& path) {
    // テクスチャの指定がある場合だけ、現在の粒子用テクスチャを切り替える。
    if (!path.empty()) {
        // 先読みしたSRVを優先し、未登録で画像が存在する場合だけ通常のロードを試します。
        uint32_t handle = engine::TextureManager::GetInstance()->GetSrvHandle(path);

        // SRVが未登録で画像を見つけた場合は、その場で読み込んで登録されたハンドルを引き直す。
        if (handle == 0 && std::filesystem::exists(path)) {
            engine::TextureManager::GetInstance()->Load(path);
            handle = engine::TextureManager::GetInstance()->GetSrvHandle(path);
        }

        // 指定した画像の有効なSRVを取得できれば、粒子描画にその画像を使う。
        if (handle > 0) {
            currentTextureHandle_ = handle;
        // 指定画像を使えない場合は、標準の粒子画像から有効なSRVを探す。
        } else {
            uint32_t fallback = engine::TextureManager::GetInstance()->GetSrvHandle("Resources/sprite/particle/particle.png");
            // 標準の粒子画像も未登録であれば、白テクスチャを候補にする。
            if (fallback == 0)
                fallback = engine::TextureManager::GetInstance()->GetSrvHandle("Resources/sprite/particle/white.png");
            // 代替画像の有効なSRVを取得できた場合だけ、現在の描画設定を更新する。
            if (fallback > 0)
                currentTextureHandle_ = fallback;
        }
    }
}
