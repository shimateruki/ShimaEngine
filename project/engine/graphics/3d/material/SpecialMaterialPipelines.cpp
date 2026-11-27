#include "SpecialMaterialPipelines.h"
#include "DirectXCommon.h"
#include "GraphicsPipelineBuilder.h"
#include "RootSignatureBuilder.h"

namespace engine {

/// <summary>特殊マテリアルごとに、専用生成関数またはシェーダーとブレンド・カリング・深度テストの設定を保持する。</summary>
struct SpecialMaterialPipelines::PipelineDescription {
    // 水・マグマ・氷・炎は専用の生成関数を使う。
    void (SpecialMaterialPipelines::*create)() = nullptr;
    const wchar_t* vertexShaderPath = nullptr;
    const wchar_t* pixelShaderPath = nullptr;
    BlendMode blendMode = BlendMode::kNone;
    D3D12_CULL_MODE cullMode = D3D12_CULL_MODE_NONE;
    // trueなら深度をシェーダーで読み、固定機能の深度テストを無効にする。
    bool shaderDepthTest = false;
};

// 材質の列挙値に対応する生成方法と描画条件を、固定した表から取得する。
const SpecialMaterialPipelines::PipelineDescription&
SpecialMaterialPipelines::GetPipelineDescription(PipelineKind kind) {
    static constexpr std::array<PipelineDescription, static_cast<std::size_t>(PipelineKind::kCount)> kDescriptions = {{
        {&SpecialMaterialPipelines::CreateWaterPipeline},
        {&SpecialMaterialPipelines::CreateMagmaPipeline},
        {&SpecialMaterialPipelines::CreateIcePipeline},
        {&SpecialMaterialPipelines::CreateFirePipeline},
        {nullptr, L"Resources/shader/materials/LaserBeam.VS.hlsl",
                  L"Resources/shader/materials/LaserBeam.PS.hlsl",
                  BlendMode::kAdd, D3D12_CULL_MODE_NONE, true},
        {nullptr, L"Resources/shader/materials/SlimeGel.VS.hlsl",
                  L"Resources/shader/materials/SlimeGel.PS.hlsl",
                  BlendMode::kNormal, D3D12_CULL_MODE_NONE},
        {nullptr, L"Resources/shader/materials/GroundShockwave.VS.hlsl",
                  L"Resources/shader/materials/GroundShockwave.PS.hlsl",
                  BlendMode::kNormal, D3D12_CULL_MODE_NONE},
        {nullptr, L"Resources/shader/materials/LiquidContact.VS.hlsl",
                  L"Resources/shader/materials/LiquidContact.PS.hlsl",
                  BlendMode::kNormal, D3D12_CULL_MODE_NONE},
        {nullptr, L"Resources/shader/materials/DamageCrack.VS.hlsl",
                  L"Resources/shader/materials/DamageCrack.PS.hlsl",
                  BlendMode::kNormal, D3D12_CULL_MODE_NONE},
        {nullptr, L"Resources/shader/materials/Updraft.VS.hlsl",
                  L"Resources/shader/materials/Updraft.PS.hlsl",
                  BlendMode::kNormal, D3D12_CULL_MODE_NONE},
        {nullptr, L"Resources/shader/materials/StunBind.VS.hlsl",
                  L"Resources/shader/materials/StunBind.PS.hlsl",
                  BlendMode::kAdd, D3D12_CULL_MODE_NONE},
        {nullptr, L"Resources/shader/materials/CrownUnlock.VS.hlsl",
                  L"Resources/shader/materials/CrownUnlock.PS.hlsl",
                  BlendMode::kAdd, D3D12_CULL_MODE_NONE},
        {nullptr, L"Resources/shader/materials/PoisonSpore.VS.hlsl",
                  L"Resources/shader/materials/PoisonSpore.PS.hlsl",
                  BlendMode::kNormal, D3D12_CULL_MODE_NONE},
        {nullptr, L"Resources/shader/materials/Cloud.VS.hlsl",
                  L"Resources/shader/materials/Cloud.PS.hlsl",
                  BlendMode::kNormal, D3D12_CULL_MODE_NONE},
        {nullptr, L"Resources/shader/materials/GatePortal.VS.hlsl",
                  L"Resources/shader/materials/GatePortal.PS.hlsl",
                  BlendMode::kNormal, D3D12_CULL_MODE_NONE},
        {nullptr, L"Resources/shader/materials/WindOrb.VS.hlsl",
                  L"Resources/shader/materials/WindOrb.PS.hlsl",
                  BlendMode::kNormal, D3D12_CULL_MODE_BACK},
        {nullptr, L"Resources/shader/materials/VisibilitySilhouette.VS.hlsl",
                  L"Resources/shader/materials/VisibilitySilhouette.PS.hlsl",
                  BlendMode::kNormal, D3D12_CULL_MODE_BACK, true},
    }};
    return kDescriptions.at(static_cast<std::size_t>(kind));
}

// 指定材質で作成した PSO を保持するスロットを返す。
SpecialMaterialPipelines::PipelineState&
SpecialMaterialPipelines::GetPipelineState(PipelineKind kind) {
    return pipelines_.at(static_cast<std::size_t>(kind));
}

ID3D12PipelineState* SpecialMaterialPipelines::PreparePipeline(PipelineKind kind) {
    auto& state = GetPipelineState(kind);
    // 要求された材質のPSOをまだ作成していない場合に限り、定義表から生成する。
    if (!state) {
        const auto& description = GetPipelineDescription(kind);
        // 専用の合成・深度条件がある四種類は個別の生成関数へ渡し、残りは定義表の設定で作ります。
        if (description.create) {
            (this->*description.create)();
        // 専用の生成関数を持たない材質は、定義表のシェーダーと描画設定を使う。
        } else {
            CreateSpecialMaterialPipeline(description, state.GetAddressOf());
        }
    }
    return state.Get();
}

void SpecialMaterialPipelines::PrepareForTitle() {
    // 共有のルートシグネチャがまだなければ、タイトル用のPSOより先に作る。
    if (!waterRootSignature_) CreateWaterRootSignature();
    // タイトルで使う材質だけを先に準備します。他の材質は共通ロードまたは初回描画で作られます。
    PreparePipeline(PipelineKind::kWater);
    PreparePipeline(PipelineKind::kFire);
    PreparePipeline(PipelineKind::kGatePortal);
}

bool SpecialMaterialPipelines::IsStepReady(std::size_t step) const {
    // 準備段階0では、共有のルートシグネチャが作成済みか返す。
    if (step == 0) return waterRootSignature_.Get() != nullptr;
    // 準備段階0はルートシグネチャ用なので、PSOの添字は段階番号から1を引きます。
    return pipelines_.at(step - 1).Get() != nullptr;
}

// ロード処理から一段ずつ呼ぶ。最初に共有ルートを用意し、その後は段階に対応するPSOを作る。
void SpecialMaterialPipelines::PrepareStep(std::size_t step) {
    static_assert(static_cast<std::size_t>(PipelineKind::kVisibilitySilhouette) + 1 == kPreparationStepCount);
    // 最初の段階では共有ルートを作り、PSOの作成は次の段階に回す。
    if (step == 0) {
        CreateWaterRootSignature();
        return;
    }
    PreparePipeline(static_cast<PipelineKind>(step - 1));
}

// 特殊材質で共用する定数・背景参照・補助画像のスロットを登録する。
void SpecialMaterialPipelines::CreateWaterRootSignature() {
    RootSignatureBuilder builder;

    // [0] wvp (b0レジスタ)
    builder.AddCbv(0, 0, D3D12_SHADER_VISIBILITY_ALL);

    // [1] 波パラメータ (b1レジスタ)
    builder.AddCbv(1, 0, D3D12_SHADER_VISIBILITY_ALL);

    // [2] マテリアル/色 (b2レジスタ)
    builder.AddCbv(2, 0, D3D12_SHADER_VISIBILITY_ALL);

    //  [3] 背景深度テクスチャ (t0レジスタ)
    builder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    // [4] 背景色のコピー (t1レジスタ)。特殊材質の描画先と別の画像を参照します。
    builder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // [5-7] ベイク済みの補助テクスチャ (t2-t4レジスタ)
    builder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    builder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    builder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // UV が範囲外へ出た場合は端の画素を使う。背景色や深度の反対側をサンプルしないため Clamp を指定する。
    builder.AddStaticSampler(0, 0, D3D12_SHADER_VISIBILITY_PIXEL, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    builder.Build(dxCommon_->GetDevice(), waterRootSignature_.GetAddressOf(), D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
}

// 水の表裏を背景に合成するパイプラインを作る。先に描いた不透明物の深度を使う。
void SpecialMaterialPipelines::CreateWaterPipeline() {
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    // 共通の頂点配置に、水の変形と色を担当するシェーダーを対応付ける。
    auto vsBlob = dxCommon_->CompileShader(L"Resources/shader/materials/Water.VS.hlsl", L"vs_6_0");
    auto psBlob = dxCommon_->CompileShader(L"Resources/shader/materials/Water.PS.hlsl", L"ps_6_0");

    GraphicsPipelineBuilder psoBuilder;
    psoBuilder.SetRootSignature(waterRootSignature_.Get());
    psoBuilder.SetInputLayout(inputLayout, _countof(inputLayout));
    psoBuilder.SetShaders(vsBlob.Get(), psBlob.Get());

    // 水が裏側（水中）からも見えるようにカリングをオフ
    psoBuilder.SetRasterizerState(D3D12_CULL_MODE_NONE, D3D12_FILL_MODE_SOLID);

    // 半透明合成（アルファブレンド）
    psoBuilder.SetBlendMode(BlendMode::kNormal);

    // 不透明物の後ろの水は深度で隠し、水面自身は深度を書き換えない。
    psoBuilder.SetDepthStencilState(true, D3D12_DEPTH_WRITE_MASK_ZERO, D3D12_COMPARISON_FUNC_LESS_EQUAL);

    // RenderTargetのフォーマット設定
    DXGI_FORMAT rtvFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoBuilder.SetRenderTargets(1, &rtvFormat, DXGI_FORMAT_D24_UNORM_S8_UINT);

    psoBuilder.Build(dxCommon_->GetDevice(), GetPipelineState(PipelineKind::kWater).GetAddressOf());
}

// 個別の生成関数を持たない材質は、共有ルートと定義表の描画条件を使う。
void SpecialMaterialPipelines::CreateSpecialMaterialPipeline(
    const PipelineDescription& description, ID3D12PipelineState** out) {
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    auto vsBlob = dxCommon_->CompileShader(description.vertexShaderPath, L"vs_6_0");
    auto psBlob = dxCommon_->CompileShader(description.pixelShaderPath, L"ps_6_0");

    GraphicsPipelineBuilder psoBuilder;
    psoBuilder.SetRootSignature(waterRootSignature_.Get());
    psoBuilder.SetInputLayout(inputLayout, _countof(inputLayout));
    psoBuilder.SetShaders(vsBlob.Get(), psBlob.Get());
    psoBuilder.SetRasterizerState(description.cullMode, D3D12_FILL_MODE_SOLID);
    psoBuilder.SetBlendMode(description.blendMode);

    // shaderDepthTestを選ぶ材質は固定機能の深度テストを止め、シェーダーで遮蔽を判定します。
    psoBuilder.SetDepthStencilState(!description.shaderDepthTest, D3D12_DEPTH_WRITE_MASK_ZERO, D3D12_COMPARISON_FUNC_LESS_EQUAL);

    DXGI_FORMAT rtvFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoBuilder.SetRenderTargets(1, &rtvFormat, DXGI_FORMAT_D24_UNORM_S8_UINT);
    psoBuilder.Build(dxCommon_->GetDevice(), out);
}

// ==========================================
// マグマ専用のパイプライン作成
// ==========================================
void SpecialMaterialPipelines::CreateMagmaPipeline() {
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    // 水系のルートシグネチャを共有しますが、頂点・ピクセルの処理はマグマ専用です。
    auto vsBlob = dxCommon_->CompileShader(L"Resources/shader/materials/Magma.VS.hlsl", L"vs_6_0");
    auto psBlob = dxCommon_->CompileShader(L"Resources/shader/materials/Magma.PS.hlsl", L"ps_6_0");

    GraphicsPipelineBuilder psoBuilder;

    // 波パラメーター・背景参照のバインド位置は、水と共通にする。
    psoBuilder.SetRootSignature(waterRootSignature_.Get());
    psoBuilder.SetInputLayout(inputLayout, _countof(inputLayout));
    psoBuilder.SetShaders(vsBlob.Get(), psBlob.Get());

    // 不透明なマグマは表側を描き、裏面をカリングする。
    psoBuilder.SetRasterizerState(D3D12_CULL_MODE_BACK, D3D12_FILL_MODE_SOLID);

    psoBuilder.SetBlendMode(BlendMode::kNone);

    // 手前の不透明なマグマが後ろの描画を隠すよう、深度テストと書き込みを有効にする。
    psoBuilder.SetDepthStencilState(true, D3D12_DEPTH_WRITE_MASK_ALL, D3D12_COMPARISON_FUNC_LESS_EQUAL);

    DXGI_FORMAT rtvFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoBuilder.SetRenderTargets(1, &rtvFormat, DXGI_FORMAT_D24_UNORM_S8_UINT);
    psoBuilder.Build(dxCommon_->GetDevice(), GetPipelineState(PipelineKind::kMagma).GetAddressOf());
}

// ==========================================
// 氷の表裏をアルファ合成する。深度はテストだけに使い、描画先には書き込まない。
// ==========================================
void SpecialMaterialPipelines::CreateIcePipeline() {
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    auto vsBlob = dxCommon_->CompileShader(L"Resources/shader/materials/Ice.VS.hlsl", L"vs_6_0");
    auto psBlob = dxCommon_->CompileShader(L"Resources/shader/materials/Ice.PS.hlsl", L"ps_6_0");

    GraphicsPipelineBuilder psoBuilder;
    psoBuilder.SetRootSignature(waterRootSignature_.Get());
    psoBuilder.SetInputLayout(inputLayout, _countof(inputLayout));
    psoBuilder.SetShaders(vsBlob.Get(), psBlob.Get());

    // 半透明の氷は表と裏を見せるため、面のカリングを無効にする。
    psoBuilder.SetRasterizerState(D3D12_CULL_MODE_NONE, D3D12_FILL_MODE_SOLID);

    // 氷の色を背景とアルファ合成する。
    psoBuilder.SetBlendMode(BlendMode::kNormal);

    // 先に描いた不透明物で隠れるよう深度テストを使い、合成先の深度は保持する。
    psoBuilder.SetDepthStencilState(true, D3D12_DEPTH_WRITE_MASK_ZERO, D3D12_COMPARISON_FUNC_LESS_EQUAL);

    DXGI_FORMAT rtvFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoBuilder.SetRenderTargets(1, &rtvFormat, DXGI_FORMAT_D24_UNORM_S8_UINT);
    psoBuilder.Build(dxCommon_->GetDevice(), GetPipelineState(PipelineKind::kIce).GetAddressOf());
}

// 炎を半透明で描き、シーン深度を参照する遮蔽はシェーダー側に任せる。
void SpecialMaterialPipelines::CreateFirePipeline() {
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    auto vsBlob = dxCommon_->CompileShader(L"Resources/shader/materials/Fire.VS.hlsl", L"vs_6_0");
    auto psBlob = dxCommon_->CompileShader(L"Resources/shader/materials/Fire.PS.hlsl", L"ps_6_0");

    GraphicsPipelineBuilder psoBuilder;
    psoBuilder.SetRootSignature(waterRootSignature_.Get());
    psoBuilder.SetInputLayout(inputLayout, _countof(inputLayout));
    psoBuilder.SetShaders(vsBlob.Get(), psBlob.Get());

    // 炎は裏側からも見えるようにカリングなし、半透明合成
    psoBuilder.SetRasterizerState(D3D12_CULL_MODE_NONE, D3D12_FILL_MODE_SOLID);
    psoBuilder.SetBlendMode(BlendMode::kNormal);

    // このパスはDSVを外して深度を読むため、遮蔽は炎シェーダー側で処理します。
    psoBuilder.SetDepthStencilState(false, D3D12_DEPTH_WRITE_MASK_ZERO, D3D12_COMPARISON_FUNC_ALWAYS);

    DXGI_FORMAT rtvFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoBuilder.SetRenderTargets(1, &rtvFormat, DXGI_FORMAT_D24_UNORM_S8_UINT);
    psoBuilder.Build(dxCommon_->GetDevice(), GetPipelineState(PipelineKind::kFire).GetAddressOf());
}

void SpecialMaterialPipelines::BindPipeline(PipelineKind kind) {
    // 初回の描画呼び出しでも使えるようにPSOの作成を済ませ、その後で今のコマンドリストへ束縛します。
    auto* state = PreparePipeline(kind);
    auto* commandList = dxCommon_->GetCommandList();
    commandList->SetGraphicsRootSignature(waterRootSignature_.Get());
    commandList->SetPipelineState(state);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

// 以下は材質別の描画入口。初回の準備とコマンド登録は BindPipeline() が共通で行う。
void SpecialMaterialPipelines::BindWater() { BindPipeline(PipelineKind::kWater); }
void SpecialMaterialPipelines::BindMagma() { BindPipeline(PipelineKind::kMagma); }
void SpecialMaterialPipelines::BindIce() { BindPipeline(PipelineKind::kIce); }
void SpecialMaterialPipelines::BindFire() { BindPipeline(PipelineKind::kFire); }
void SpecialMaterialPipelines::BindLaser() { BindPipeline(PipelineKind::kLaser); }
void SpecialMaterialPipelines::BindSlimeGel() { BindPipeline(PipelineKind::kSlimeGel); }
void SpecialMaterialPipelines::BindShockwave() { BindPipeline(PipelineKind::kShockwave); }
void SpecialMaterialPipelines::BindLiquidContact() { BindPipeline(PipelineKind::kLiquidContact); }
void SpecialMaterialPipelines::BindDamageCrack() { BindPipeline(PipelineKind::kDamageCrack); }
void SpecialMaterialPipelines::BindUpdraft() { BindPipeline(PipelineKind::kUpdraft); }
void SpecialMaterialPipelines::BindStunBind() { BindPipeline(PipelineKind::kStunBind); }
void SpecialMaterialPipelines::BindCrownUnlock() { BindPipeline(PipelineKind::kCrownUnlock); }
void SpecialMaterialPipelines::BindPoisonSpore() { BindPipeline(PipelineKind::kPoisonSpore); }
void SpecialMaterialPipelines::BindCloud() { BindPipeline(PipelineKind::kCloud); }
void SpecialMaterialPipelines::BindGatePortal() { BindPipeline(PipelineKind::kGatePortal); }
void SpecialMaterialPipelines::BindWindOrb() { BindPipeline(PipelineKind::kWindOrb); }
void SpecialMaterialPipelines::BindVisibilitySilhouette() { BindPipeline(PipelineKind::kVisibilitySilhouette); }

} // namespace engine
