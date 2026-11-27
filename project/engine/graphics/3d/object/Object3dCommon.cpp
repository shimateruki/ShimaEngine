#include <cstdint>
#include "Object3DCommon.h"
#include "engine/graphics/3d/material/SpecialMaterialPipelines.h"
#include "engine/graphics/3d/material/ShaderMaterialRegistry.h"
#include "../FoliageInteraction.h"
#include "RootSignatureBuilder.h"
#include "GraphicsPipelineBuilder.h" 
#include "engine/system/scene/LoadingProfiler.h"
#include "DebugConsole.h"
#include <cassert>
#include <algorithm>
#include <filesystem>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxcompiler.lib")

const size_t engine::Object3DCommon::kInitializationStepCount =
    kCommonPreparationStepCount + SpecialMaterialPipelines::kPreparationStepCount + 1; // 末尾はスカイボックス

engine::Object3DCommon::Object3DCommon() = default;
engine::Object3DCommon::~Object3DCommon() = default;

void engine::Object3DCommon::Initialize(engine::DirectXCommon* dxCommon) {
    // 描画パスごとに専用 PSO を先に作り、実行中の描画切り替えでは再コンパイルしない。
    StartupProfileScope pipelineProfile("Object3dCommon pipelines");
    BeginInitialize(dxCommon);

    // 通常起動では、分割可能な初期化処理を完了まで続ける。
    while (!InitializeStep()) {
    }
}

void engine::Object3DCommon::InitializeForTitle(engine::DirectXCommon* dxCommon) {
    // タイトル背景で使う通常描画、影、水、炎、ゲート、空だけを初回描画前に準備する。
    // 未使用の材質は各描画入口で初回使用時に生成し、配置アセットの追加にも対応する。
    StartupProfileScope pipelineProfile("Object3dCommon title pipelines");
    BeginInitialize(dxCommon);
    // 通常の3D描画で使う共有ルートを、まだ作っていない場合だけ用意する。
    if (!rootSignature_) CreateRootSignature();
    // 影の頂点シェーダー用ルートも、タイトルの軽量準備に含める。
    if (!shadowRootSignature_) CreateShadowRootSignature();
    CreatePipelineStatesInternal(true);
    specialMaterialPipelines_->PrepareForTitle();
    // 空の描画状態が未作成なら、タイトルで使用する前に準備する。
    if (!skyboxPipelineState_) CreateSkyboxPipeline();
    initializationStep_ = kInitializationStepCount;
}

void engine::Object3DCommon::EnsurePipelineState(
    Microsoft::WRL::ComPtr<ID3D12PipelineState>& state,
    void (engine::Object3DCommon::*createPipeline)()) {
    // 要求されたPSOを取得できない場合だけ、対応する生成関数を呼び出す。
    if (!state) {
        (this->*createPipeline)();
    }
}

// 準備の巡回位置を先頭へ戻す。タイトルで作成済みのPSOは保持し、InitializeStepで不足分だけを補う。
void engine::Object3DCommon::BeginInitialize(engine::DirectXCommon* dxCommon) {
    assert(dxCommon);
    dxCommon_ = dxCommon;
    // 特殊材質の管理器がなければ、DirectXを接続する前に作成する。
    if (!specialMaterialPipelines_) {
        specialMaterialPipelines_ = std::make_unique<SpecialMaterialPipelines>();
    }
    specialMaterialPipelines_->SetDirectX(*dxCommon);
    initializationStep_ = 0;
}

bool engine::Object3DCommon::InitializeStep() {
    // 全段階を処理済みなら、新しいPSOを作らず完了を返す。
    if (initializationStep_ >= kInitializationStepCount) return true;

    const std::size_t stepIndex = initializationStep_++;
    const bool complete = initializationStep_ == kInitializationStepCount;
    constexpr const char* kProfilePrefix = "Object3dCommon pipeline step ";

    // 最初の段階群では、通常、影、フォグなどの共有描画資源を順に調べる。
    if (stepIndex < kCommonPreparationStepCount) {
        using Step = void (engine::Object3DCommon::*)();
        static constexpr std::array<Step, kCommonPreparationStepCount> kSteps = {
            &Object3DCommon::CreateRootSignature,
            &Object3DCommon::CreateShadowRootSignature,
            &Object3DCommon::CreatePipelineStates,
            &Object3DCommon::CreateLocalFogPipeline,
            &Object3DCommon::CreateEffectRootSignature,
            &Object3DCommon::CreateEffectPipeline,
        };
        const auto allReady = [](const auto& states) {
            return std::all_of(states.begin(), states.end(), [](const auto& state) { return state.Get() != nullptr; });
        };
        // タイトルなどで準備済みのPSOは段階初期化でも使い回し、不足分だけを各段で調べます。
        const std::array<bool, kCommonPreparationStepCount> ready = {
            rootSignature_.Get() != nullptr,
            shadowRootSignature_.Get() != nullptr,
            allReady(graphicsPipelineStates_) && shadowPipelineState_ && foliageShadowPipelineState_,
            localFogPipelineState_.Get() != nullptr,
            effectRootSignature_.Get() != nullptr,
            allReady(effectPipelineStates_),
        };
        // 要求された段階の資源が揃っていなければ、計測を付けて生成する。
        if (!ready[stepIndex]) {
            StartupProfileScope stepProfile(kProfilePrefix + std::to_string(stepIndex));
            (this->*kSteps[stepIndex])();
        }
    // 共通描画の段階を過ぎ、最後の空描画に達していなければ特殊材質を準備する。
    } else if (!complete) {
        const std::size_t materialStep = stepIndex - kCommonPreparationStepCount;
        // 指定段階の特殊材質がまだ準備されていない場合だけ作成する。
        if (!specialMaterialPipelines_->IsStepReady(materialStep)) {
            StartupProfileScope stepProfile(kProfilePrefix + std::to_string(stepIndex));
            specialMaterialPipelines_->PrepareStep(materialStep);
        }
    // 最後の段階では、空のPSOが未作成の場合だけ生成する。
    } else if (!skyboxPipelineState_) {
        StartupProfileScope stepProfile(kProfilePrefix + std::to_string(stepIndex));
        CreateSkyboxPipeline();
    }
    return complete;
}

// 完了した準備段階の割合を返す。各段階のコンパイル時間は含めない。
float engine::Object3DCommon::GetInitializationProgress() const {
    return static_cast<float>(initializationStep_) / static_cast<float>(kInitializationStepCount);
}

// 通常描画の共通ルート、三角形の入力方式、草へ渡す接触履歴を現在のコマンドに設定する。
void engine::Object3DCommon::SetGraphicsCommand() {
    // ルート番号 15 は通常描画と影描画で共有する草の接触履歴に対応する。
    ID3D12GraphicsCommandList* commandList = dxCommon_->GetCommandList();
    commandList->SetGraphicsRootSignature(rootSignature_.Get());
    commandList->SetGraphicsRoot32BitConstants(15, 32, foliageInteraction::samples.data(), 0);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void engine::Object3DCommon::SetPipelineState(BlendMode blendMode) {
    // 指定された合成方式でまだ描画できない場合は、通常3DのPSOを必要な範囲まで用意する。
    if (!graphicsPipelineStates_[static_cast<size_t>(blendMode)]) {
        CreatePipelineStatesInternal(false);
    }

    ID3D12GraphicsCommandList* commandList = dxCommon_->GetCommandList();
    commandList->SetPipelineState(graphicsPipelineStates_[static_cast<size_t>(blendMode)].Get());
}

void engine::Object3DCommon::SetMaterialPipelineState(BlendMode blendMode, std::int32_t materialType, const std::string& shaderMaterialId) {
    // 合成方式の番号が定義範囲を外れた場合は、配列を引く前に通常合成へ揃える。
    if (blendMode < BlendMode::kNone || blendMode >= BlendMode::kCount)
        blendMode = BlendMode::kNormal;
    // 通常材質でシェーダーのIDが指定されたときは、台帳から個別の描画設定を探す。
    if (materialType == 0 && !shaderMaterialId.empty()) {
        const auto* definition = ShaderMaterialRegistry::Instance().Find(shaderMaterialId);
        // 台帳で定義が見つかった場合は、そのピクセルシェーダーを使うPSOを調べる。
        if (definition) {
            const std::string& shaderPath = definition->pixelShader;
            // シェーダーの定義を再読込した場合に古いPSOや作成失敗を使い回さないよう、台帳の世代をキーに含めます。
            const std::string cacheKey = shaderPath + "#" + std::to_string(ShaderMaterialRegistry::Instance().Generation());
            const std::string failureKey = cacheKey + "#" + std::to_string(static_cast<std::int32_t>(blendMode));
            auto& states = shaderMaterialPipelineStates_[cacheKey];
            auto& state = states[static_cast<size_t>(blendMode)];
            // PSOがなく、今の台帳の世代で失敗していない組だけ、作成を試す。
            if (!state && !failedShaderMaterials_.contains(failureKey)) {
                const std::wstring widePath = std::filesystem::path(std::u8string(shaderPath.begin(), shaderPath.end())).wstring();
                // シェーダーからPSOを作れなければ、同じ組で毎フレーム再試行しないよう記録する。
                if (!CreateMaterialVariantPipeline(widePath.c_str(), blendMode, state.GetAddressOf()))
                    failedShaderMaterials_.insert(failureKey);
            }
            // 個別のPSOが使用可能になっていれば、通常の材質判定へ進まず、そのPSOを設定する。
            if (state) {
                dxCommon_->GetCommandList()->SetPipelineState(state.Get());
                return;
            }
        }
        // 未登録・コンパイル失敗時は通常材質へ戻し、描画欠落を避ける。
        SetPipelineState(blendMode);
        return;
    }
    const wchar_t* shaderPath = nullptr;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, static_cast<size_t>(BlendMode::kCount)>* states = nullptr;

    // 材質番号24のダッシュパネルは、専用ピクセルシェーダーを使う。
    if (materialType == 24) {
        shaderPath = L"Resources/shader/object3d/Object3d.DashPanel.PS.hlsl";
        states = &dashPanelPipelineStates_;
    // 材質番号28のデカールも、専用ピクセルシェーダーを使う。
    } else if (materialType == 28) {
        shaderPath = L"Resources/shader/object3d/Object3d.Decal.PS.hlsl";
        states = &decalPipelineStates_;
    }

    // 個別の描画状態を選ばなかった材質は、通常3Dの合成方式で描く。
    if (!states) {
        SetPipelineState(blendMode);
        return;
    }

    auto& pipeline = (*states)[static_cast<size_t>(blendMode)];

    // 専用PSOをまだ作成していない方式だけ、ここで作成を試す。
    if (!pipeline)
        CreateMaterialVariantPipeline(shaderPath, blendMode, pipeline.GetAddressOf());
    // 専用PSOが使用可能なら、描画コマンドへ設定する。
    if (pipeline)
        dxCommon_->GetCommandList()->SetPipelineState(pipeline.Get());
    // 専用PSOが使えない場合は、通常描画のPSOへ戻す。
    else
        SetPipelineState(blendMode);
}

bool engine::Object3DCommon::CreateMaterialVariantPipeline(const wchar_t* pixelShaderPath, BlendMode blendMode, ID3D12PipelineState** outPipelineState) {
    // 通常描画と同じ頂点入力・ルート・深度設定を使い、ピクセルシェーダだけを差し替える。
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "WEIGHT",   0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "INDEX",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    auto vertexShader = dxCommon_->CompileShader(L"Resources/shader/object3d/Object3d.VS.hlsl", L"vs_6_0");
    std::string shaderError;
    // 外部のピクセルシェーダーをコンパイルし、作成失敗の理由を受け取る。
    auto pixelShader = dxCommon_->TryCompileShader(pixelShaderPath, L"ps_6_0", L"main", shaderError);
    // コンパイルに失敗した場合は、診断を出して共通材質に戻れるよう失敗を返す。
    if (!pixelShader) {
        DebugConsole::GetInstance()->AddLog("Shader Material compile error: " + shaderError);
        return false;
    }

    GraphicsPipelineBuilder builder;
    builder.SetRootSignature(rootSignature_.Get());
    builder.SetInputLayout(inputLayout, _countof(inputLayout));
    builder.SetShaders(vertexShader.Get(), pixelShader.Get());
    builder.SetRasterizerState(D3D12_CULL_MODE_NONE, D3D12_FILL_MODE_SOLID);
    DXGI_FORMAT rtvFormats[] = { DXGI_FORMAT_R16G16B16A16_FLOAT };
    builder.SetRenderTargets(1, rtvFormats, DXGI_FORMAT_D24_UNORM_S8_UINT);
    builder.SetDepthStencilState(true);
    builder.SetBlendMode(blendMode);
    const HRESULT result = builder.TryBuild(dxCommon_->GetDevice(), outPipelineState);
    // PSO作成が失敗した場合も、HRESULTを記録して呼び出し元に失敗を返す。
    if (FAILED(result)) {
        DebugConsole::GetInstance()->AddLog("Shader Material pipeline error: " + std::to_string(result));
        return false;
    }
    return outPipelineState && *outPipelineState;
}

// 通常3Dの描画で使うルート番号とシェーダーレジスタを対応付ける。
void engine::Object3DCommon::CreateRootSignature() {
    RootSignatureBuilder builder;

    // 番号を描画側の束縛先と揃え、材質の補正と草の変形用に13～15を使う。

    // [0] Material (CBV b0 - Pixel)
    builder.AddCbv(0, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // [1] TransformationMatrix (CBV b0 - Vertex)
    builder.AddCbv(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);

    // [2] Texture (DescriptorTable t0 - Pixel)
    builder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // [3] DirectionalLight (CBV b1 - Pixel)
    builder.AddCbv(1, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // [4] Camera (CBV b2 - Pixel)
    builder.AddCbv(2, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // [5] PointLight (CBV b3 - Pixel)
    builder.AddCbv(3, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // [6] SpotLight (CBV b4 - Pixel)
    builder.AddCbv(4, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // [7] Skinning Matrix (DescriptorTable t1 - Vertex)
    builder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, D3D12_SHADER_VISIBILITY_VERTEX);

    // [8] Environment Map (DescriptorTable t2 - Pixel)
    builder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // [9] Normal Map (DescriptorTable t3 - Pixel)
    builder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // [10] ORM Map (DescriptorTable t4 - Pixel)
    builder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // [11] Shadow wvp (CBV b1 - Vertex)
    builder.AddCbv(1, 0, D3D12_SHADER_VISIBILITY_VERTEX);

    // [12] Shadow Map (DescriptorTable t5 - Pixel)
    builder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // [13] Mesh Material Factors (Root Constants b5 - Pixel)
    builder.AddConstants(5, 6, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // [14] 草の頂点アニメーション用マテリアル。既存の番号は維持する。
    builder.AddCbv(6, 0, D3D12_SHADER_VISIBILITY_VERTEX);

    // [15] プレイヤーの草への接触履歴。Root Parameter合計60 DWORD。
    builder.AddConstants(7, 32, 0, D3D12_SHADER_VISIBILITY_VERTEX);

    // 色画像と影マップはアドレス範囲外の扱いが違うため、別々のサンプラーを登録する。

    // s0: テクスチャ用の標準サンプラー (リニア補間、ラップ)
    builder.AddStaticSampler(0, 0, D3D12_SHADER_VISIBILITY_PIXEL, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    // s1: 影マップは点サンプリング。Uの範囲外は白、Vの範囲外は端の値を使う。
    D3D12_STATIC_SAMPLER_DESC shadowSampler{};
    shadowSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    shadowSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    shadowSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    shadowSampler.MaxLOD = D3D12_FLOAT32_MAX;
    shadowSampler.ShaderRegister = 1; // s1
    shadowSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    shadowSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    builder.AddStaticSamplerCustom(shadowSampler);

    builder.Build(dxCommon_->GetDevice(), rootSignature_.GetAddressOf());
}

// すべての合成方式を対象にして、共通PSOの不足分を準備する。
void engine::Object3DCommon::CreatePipelineStates() {
    CreatePipelineStatesInternal(false);
}

// 通常の合成と影描画のPSOを作る。タイトル初期化からは合成方式を絞って呼ぶ。
void engine::Object3DCommon::CreatePipelineStatesInternal(bool titleBlendModesOnly) {
    ID3D12Device* device = dxCommon_->GetDevice();

    // 通常描画と影で使う、モデルの頂点入力を定義する。
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "WEIGHT",   0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "INDEX",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    auto vsBlob = dxCommon_->CompileShader(L"Resources/shader/object3d/Object3d.VS.hlsl", L"vs_6_0");
    auto psBlob = dxCommon_->CompileShader(L"Resources/shader/object3d/Object3d.PS.hlsl", L"ps_6_0");

    // 通常描画はHDRの描画先と深度を使い、合成方式ごとに状態を分ける。
    GraphicsPipelineBuilder builder;
    builder.SetRootSignature(rootSignature_.Get());
    builder.SetInputLayout(inputLayout, _countof(inputLayout));
    builder.SetShaders(vsBlob.Get(), psBlob.Get());
    builder.SetRasterizerState(D3D12_CULL_MODE_NONE, D3D12_FILL_MODE_SOLID);

    // レンダーターゲットと深度の設定 (HDR用 16bit Float + Depth 24bit)
    DXGI_FORMAT rtvFormats[] = { DXGI_FORMAT_R16G16B16A16_FLOAT };
    builder.SetRenderTargets(1, rtvFormats, DXGI_FORMAT_D24_UNORM_S8_UINT);
    builder.SetDepthStencilState(true);

    for (size_t i = 0; i < static_cast<size_t>(BlendMode::kCount); ++i) {
        // タイトルの軽量準備では不透明と通常合成だけを先に揃え、その他の合成は後の準備へ回します。
        if ((titleBlendModesOnly && i > static_cast<size_t>(BlendMode::kNormal)) ||
            graphicsPipelineStates_[i]) {
            continue;
        }

        BlendMode mode = static_cast<BlendMode>(i);

        // 合成方式に対応するブレンドと深度書き込み設定を、パイプラインビルダーへ反映する。
        builder.SetBlendMode(mode);
        builder.Build(device, graphicsPipelineStates_[i].GetAddressOf());
    }

    // シャドウマップの描画先では、通常モデルと草で異なる深度バイアスを使う。
    auto shadowVsBlob = dxCommon_->CompileShader(L"Resources/shader/object3d/Shadow.VS.hlsl", L"vs_6_0");

    GraphicsPipelineBuilder shadowBuilder;
    shadowBuilder.SetRootSignature(shadowRootSignature_.Get());
    shadowBuilder.SetInputLayout(inputLayout, _countof(inputLayout));

    // 影のパスは深度へ書き込むため、頂点シェーダーだけを設定する。
    shadowBuilder.SetShaders(shadowVsBlob.Get(), nullptr);
    shadowBuilder.SetRasterizerState(D3D12_CULL_MODE_NONE, D3D12_FILL_MODE_SOLID);

    // シャドウマップで自己遮蔽の縞が出にくいよう、影パスに深度バイアスを加える。
    shadowBuilder.SetDepthBias(10000, 0.0f, 1.0f);
    shadowBuilder.SetDepthStencilState(true, D3D12_DEPTH_WRITE_MASK_ALL);

    // 色の描画先を使わず、32ビット浮動小数点の深度へ書き込む。
    shadowBuilder.SetRenderTargets(0, nullptr, DXGI_FORMAT_D32_FLOAT);

    // 通常の影用PSOは、一度作成した状態を以後の準備でも使う。
    if (!shadowPipelineState_) {
        shadowBuilder.Build(device, shadowPipelineState_.GetAddressOf());
    }

    // 草の薄い葉が大きな深度バイアスで地面の裏へ消えない専用設定。
    shadowBuilder.SetDepthBias(128, 0.0f, 0.35f);

    // 草の影専用のPSOがまだなければ、小さな深度バイアスの設定で作る。
    if (!foliageShadowPipelineState_) {
        shadowBuilder.Build(device, foliageShadowPipelineState_.GetAddressOf());
    }
}

void engine::Object3DCommon::SetShadowPipelineState(bool foliage) {
    // 葉の影だけ深度バイアスを弱め、薄い面が地面の裏へ消えるのを防ぐ。
    ID3D12GraphicsCommandList* commandList = dxCommon_->GetCommandList();
    commandList->SetPipelineState(foliage ? foliageShadowPipelineState_.Get() : shadowPipelineState_.Get());
}

void engine::Object3DCommon::SetShadowGraphicsCommand() {
    // 影パスは色ターゲットを持たないため、通常描画とは別のルート構成を使う。
    ID3D12GraphicsCommandList* commandList = dxCommon_->GetCommandList();
    commandList->SetGraphicsRootSignature(shadowRootSignature_.Get());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

// 変換と骨を頂点処理へ送り、草の変形も通常画面と同じ条件で計算できるようにする。
void engine::Object3DCommon::CreateShadowRootSignature() {
    RootSignatureBuilder builder;


    // [0] WVP行列 (CBV b0 - VertexShader用)
    builder.AddCbv(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);

    // [1] ボーン情報 (DescriptorTable t0 - VertexShader用)
    builder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1, 0, D3D12_SHADER_VISIBILITY_VERTEX);

    // [2,3] 通常描画と同じ風・踏み分けを影にも反映する。
    builder.AddCbv(6, 0, D3D12_SHADER_VISIBILITY_VERTEX);
    builder.AddConstants(7, 32, 0, D3D12_SHADER_VISIBILITY_VERTEX);

    builder.Build(dxCommon_->GetDevice(), shadowRootSignature_.GetAddressOf());
}

// 霧のモデルと描画済みシーン深度を使うローカルフォグ専用のルートとPSOを作る。
void engine::Object3DCommon::CreateLocalFogPipeline() {
    auto device = dxCommon_->GetDevice();

    RootSignatureBuilder rsBuilder;

    // [0] b0: WVP行列 (Model::DrawShadow と一致させる)
    rsBuilder.AddCbv(0, 0, D3D12_SHADER_VISIBILITY_ALL);

    // ルート番号1はボーン用SRV。Model::DrawShadow()が必要に応じてこの位置へ束縛する。
    rsBuilder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, D3D12_SHADER_VISIBILITY_VERTEX);

    // [2] t0: デプステクスチャ (ピクセルシェーダー用)
    rsBuilder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // [3] b1: ローカルフォグ設定
    rsBuilder.AddCbv(1, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // s0: テクスチャサンプラー (Clamp設定)
    rsBuilder.AddStaticSampler(0, 0, D3D12_SHADER_VISIBILITY_PIXEL, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    rsBuilder.Build(device, localFogRootSignature_.GetAddressOf());

    // フォグの箱を描くモデル入力と、シーン深度で霧量を求めるシェーダーを指定する。
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "WEIGHT",   0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "INDEX",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    auto vsBlob = dxCommon_->CompileShader(L"Resources/shader/materials/LocalFog.VS.hlsl", L"vs_6_0");
    auto psBlob = dxCommon_->CompileShader(L"Resources/shader/materials/LocalFog.PS.hlsl", L"ps_6_0");

    GraphicsPipelineBuilder psoBuilder;
    psoBuilder.SetRootSignature(localFogRootSignature_.Get());
    psoBuilder.SetInputLayout(inputLayout, _countof(inputLayout));
    psoBuilder.SetShaders(vsBlob.Get(), psBlob.Get());

    // カメラがフォグの箱の内部に入っても体積を描けるよう、箱の裏面を描く。
    psoBuilder.SetRasterizerState(D3D12_CULL_MODE_FRONT, D3D12_FILL_MODE_SOLID);

    psoBuilder.SetBlendMode(BlendMode::kNormal);

    // 霧の奥行きはピクセルシェーダーがシーン深度を読み取って決めるため、固定機能の深度テストと書き込みを無効にする。
    psoBuilder.SetDepthStencilState(false, D3D12_DEPTH_WRITE_MASK_ZERO);

    // レンダーターゲット (デプスバッファは使わないので DSVFormat は UNKNOWN にする)
    DXGI_FORMAT rtvFormats[] = { DXGI_FORMAT_R16G16B16A16_FLOAT };
    psoBuilder.SetRenderTargets(1, rtvFormats, DXGI_FORMAT_UNKNOWN);

    psoBuilder.Build(device, localFogPipelineState_.GetAddressOf());
}

void engine::Object3DCommon::SetLocalFogGraphicsCommand() {
    // 深度から霧量を再構成する専用パスへ切り替える。
    EnsurePipelineState(localFogPipelineState_, &engine::Object3DCommon::CreateLocalFogPipeline);
    ID3D12GraphicsCommandList* commandList = dxCommon_->GetCommandList();
    commandList->SetGraphicsRootSignature(localFogRootSignature_.Get());
    commandList->SetPipelineState(localFogPipelineState_.Get());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

// 段階準備を省いた場合は初回描画でルートとPSOを用意し、指定した合成方式の状態を選ぶ。
void engine::Object3DCommon::SetEffectGraphicsCommand(BlendMode blendMode) {
    // 指定の合成方式が使える状態でなければ、エフェクト用のルートとPSOを用意する。
    if (!effectPipelineStates_[static_cast<size_t>(blendMode)]) {
        // エフェクト用の共有ルートがまだなければ、PSOを作る前に生成する。
        if (!effectRootSignature_) {
            CreateEffectRootSignature();
        }

        CreateEffectPipeline();
    }

    ID3D12GraphicsCommandList* commandList = dxCommon_->GetCommandList();
    commandList->SetGraphicsRootSignature(effectRootSignature_.Get());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    commandList->SetPipelineState(effectPipelineStates_[static_cast<size_t>(blendMode)].Get());
}

// メッシュエフェクトの頂点変換と材質、背景色を参照するための描画入力を定義する。
void engine::Object3DCommon::CreateEffectRootSignature() {
    RootSignatureBuilder builder;

    // ルート番号0には頂点変換を格納するb0を割り当てる。
    builder.AddCbv(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);

    // パラメータ1: 頂点側のb1を予約する。現在の描画経路はWVPを渡し、Effect3d.VSでは参照しない。
    builder.AddCbv(1, 0, D3D12_SHADER_VISIBILITY_VERTEX);

    // パラメータ2: CBV (b0, ピクセルシェーダー用) -> EffectMaterial (色・UVスクロール時間など)
    builder.AddCbv(0, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // パラメータ3: DescriptorTable (t0, ピクセルシェーダー用) -> エフェクト用メインテクスチャ
    builder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // パラメータ4: DescriptorTable (t1, ピクセルシェーダー用) -> 背景Grabテクスチャ
    builder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // ルート番号5はノイズ画像のt2。
    builder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // ルート番号6は色のランプ画像を参照するt3。
    builder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // エフェクト画像をリニア補間し、UVが範囲を超えたら繰り返す共通のs0を使う。
    builder.AddStaticSampler(0, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // ビルド実行 (頂点レイアウトを使用するフラグを立てる)
    builder.Build(
        dxCommon_->GetDevice(),
        effectRootSignature_.GetAddressOf(),
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
    );
}

// メッシュエフェクト用のパイプラインをブレンドモードごとに作る。
void engine::Object3DCommon::CreateEffectPipeline() {
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
         { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
         { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
         { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
         { "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
         { "WEIGHT",   0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
         { "INDEX",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    // メッシュエフェクト用の頂点とピクセルのシェーダーをコンパイルする。
    auto vsBlob = dxCommon_->CompileShader(L"Resources/shader/effects/Effect3d.VS.hlsl", L"vs_6_0");
    auto psBlob = dxCommon_->CompileShader(L"Resources/shader/effects/Effect3d.PS.hlsl", L"ps_6_0");

    GraphicsPipelineBuilder psoBuilder;
    psoBuilder.SetRootSignature(effectRootSignature_.Get());
    psoBuilder.SetInputLayout(inputLayout, _countof(inputLayout));
    psoBuilder.SetShaders(vsBlob.Get(), psBlob.Get());

    // 斬撃などの板状のメッシュを裏側からも見せるため、面のカリングを無効にする。
    psoBuilder.SetRasterizerState(D3D12_CULL_MODE_NONE, D3D12_FILL_MODE_SOLID);

    // 手前の地形に隠れるよう深度テストを有効にする。深度書き込みはこの後に合成方式ごとの設定を適用する。
    psoBuilder.SetDepthStencilState(true, D3D12_DEPTH_WRITE_MASK_ZERO);

    // HDRの描画先と通常シーンの深度形式へ合わせる。
    DXGI_FORMAT rtvFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoBuilder.SetRenderTargets(1, &rtvFormat, DXGI_FORMAT_D24_UNORM_S8_UINT);


    for (size_t i = 0; i < static_cast<size_t>(BlendMode::kCount); ++i) {
        // すでに作成済みの合成方式は、同じPSOを再作成しない。
        if (effectPipelineStates_[i]) continue;
        BlendMode mode = static_cast<BlendMode>(i);

        psoBuilder.SetBlendMode(mode);

        psoBuilder.Build(dxCommon_->GetDevice(), effectPipelineStates_[i].GetAddressOf());
    }
}

// 水材質のPSOと共有ルートを、現在のコマンドリストへ束縛します。
void engine::Object3DCommon::SetWaterGraphicsCommand() {
    specialMaterialPipelines_->BindWater();
}

void engine::Object3DCommon::SetMagmaGraphicsCommand() {
    specialMaterialPipelines_->BindMagma();
}

void engine::Object3DCommon::SetIceGraphicsCommand() {
    specialMaterialPipelines_->BindIce();
}

void engine::Object3DCommon::SetFireGraphicsCommand() {
    specialMaterialPipelines_->BindFire();
}

void engine::Object3DCommon::SetLaserGraphicsCommand() {
    specialMaterialPipelines_->BindLaser();
}

void engine::Object3DCommon::SetSlimeGelGraphicsCommand() {
    specialMaterialPipelines_->BindSlimeGel();
}

void engine::Object3DCommon::SetShockwaveGraphicsCommand() {
    specialMaterialPipelines_->BindShockwave();
}

void engine::Object3DCommon::SetLiquidContactGraphicsCommand() {
    specialMaterialPipelines_->BindLiquidContact();
}

void engine::Object3DCommon::SetDamageCrackGraphicsCommand() {
    specialMaterialPipelines_->BindDamageCrack();
}

void engine::Object3DCommon::SetUpdraftGraphicsCommand() {
    specialMaterialPipelines_->BindUpdraft();
}

void engine::Object3DCommon::SetStunBindGraphicsCommand() {
    specialMaterialPipelines_->BindStunBind();
}

void engine::Object3DCommon::SetCrownUnlockGraphicsCommand() {
    specialMaterialPipelines_->BindCrownUnlock();
}

void engine::Object3DCommon::SetPoisonSporeGraphicsCommand() {
    specialMaterialPipelines_->BindPoisonSpore();
}

void engine::Object3DCommon::SetCloudGraphicsCommand() {
    specialMaterialPipelines_->BindCloud();
}

void engine::Object3DCommon::SetGatePortalGraphicsCommand() {
    specialMaterialPipelines_->BindGatePortal();
}

void engine::Object3DCommon::SetWindOrbGraphicsCommand() {
    specialMaterialPipelines_->BindWindOrb();
}

void engine::Object3DCommon::SetVisibilitySilhouetteGraphicsCommand() {
    specialMaterialPipelines_->BindVisibilitySilhouette();
}

// シーンの背面に描く空の描画状態を用意する。入力は頂点位置とキューブマップ。
void engine::Object3DCommon::CreateSkyboxPipeline() {
    // 背景はシーン側で不透明物より先に描く。後から配置物を重ねられるよう、深度テストと書き込みは無効にする。
    RootSignatureBuilder rsBuilder;
    rsBuilder.AddCbv(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
    rsBuilder.AddSimpleDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    rsBuilder.AddStaticSampler(0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    rsBuilder.Build(dxCommon_->GetDevice(), skyboxRootSignature_.GetAddressOf());
    auto vsBlob = dxCommon_->CompileShader(L"Resources/shader/skybox/Skybox.VS.hlsl", L"vs_6_0");
    auto psBlob = dxCommon_->CompileShader(L"Resources/shader/skybox/Skybox.PS.hlsl", L"ps_6_0");

    GraphicsPipelineBuilder pipelineBuilder;
    pipelineBuilder.SetRootSignature(skyboxRootSignature_.Get());
    pipelineBuilder.SetShaders(vsBlob.Get(), psBlob.Get());

    pipelineBuilder.SetRasterizerState(D3D12_CULL_MODE_NONE);

    pipelineBuilder.SetBlendMode(BlendMode::kNone);

    pipelineBuilder.SetDepthStencilState(false, D3D12_DEPTH_WRITE_MASK_ZERO, D3D12_COMPARISON_FUNC_ALWAYS);

    D3D12_INPUT_ELEMENT_DESC inputElements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    pipelineBuilder.SetInputLayout(inputElements, _countof(inputElements));

    DXGI_FORMAT rtvFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    pipelineBuilder.SetRenderTargets(1, &rtvFormat, DXGI_FORMAT_D24_UNORM_S8_UINT);
    pipelineBuilder.Build(dxCommon_->GetDevice(), skyboxPipelineState_.GetAddressOf());
}

