#define NOMINMAX
#include <fstream>
#include "Model.h"
#include "DirectXCommon.h"
#include "RenderStats.h"
#include "engine/utility/math/Math.h"
#include <sstream>
#include <cassert>
#include <filesystem>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include "SrvManager.h"
#include <DebugConsole.h>
#include <LightManager.h>

namespace engine {

namespace {
constexpr const char* kDefaultWhiteTexture = "Resources/sprite/common/white.png";

// 元の材質値を基準に倍率へ変換し、ゼロ付近では除算を避けます。
float CalculateMaterialMultiplier(float value, float reference) {
    constexpr float kEpsilon = 1.0e-5f;

    // 基準が十分に大きい場合だけ、材質値を基準に対する倍率として計算する。
    if (std::abs(reference) > kEpsilon) {
        return value / reference;
    }

    // 基準と値が両方ほぼ0なら変更なしの1を返し、基準だけが0に近いなら値をそのまま渡す。
    return std::abs(value) <= kEpsilon ? 1.0f : value;
}

// 共有画像の有効なハンドルを保持し、法線・ORMの代替で必要になったときに初回ロードする。
uint32_t GetDefaultWhiteTextureHandle() {
    static uint32_t sHandle = 0;

    // 共有の白画像をまだ取得していなければ、その参照を初めて使う時点でロードする。
    if (sHandle == 0) {
        sHandle = engine::TextureManager::GetInstance()->Load(kDefaultWhiteTexture);
    }

    return sHandle;
}


}




// モデル共通情報とファイル名を受け取り、モデルデータ・GPUリソース・スケルトンを初期化する。
void Model::Initialize(ModelCommon* common, const std::string& directoryPath, const std::string& filename) {
    InitializeFromCpuData(common, LoadFile(directoryPath, filename), filename);
}

// 描画リソースを作らず、ファイルの内容をCPU側で準備して返す。
Model::ModelData Model::LoadCpuData(const std::string& directoryPath, const std::string& filename) {
    return LoadFile(directoryPath, filename);
}

// CPU側のモデルからローカル境界、材質、メッシュやボーンのGPUリソースを準備する。
void Model::InitializeFromCpuData(
    ModelCommon* common,
    ModelData modelData,
    const std::string& sourceName) {
    assert(common);
    common_ = common;
    DirectXCommon* dxCommon = common_->GetDxCommon();

    // ワーカーまたは同期読込で準備済みのCPUデータを受け取る。
    modelData_ = std::move(modelData);
    // 頂点・インデックスのどちらかがないメッシュをあらかじめ除き、描画と境界計算を有効な形状だけに絞ります。
    modelData_.meshes.erase(
        std::remove_if(modelData_.meshes.begin(), modelData_.meshes.end(),
            [](const Mesh& mesh) {
                return mesh.vertices.empty() || mesh.indices.empty();
            }),
        modelData_.meshes.end());

    // 有効な全メッシュの頂点から、カリングなどで使うローカル座標の範囲を求める。
    engine::Vector3 min = { FLT_MAX, FLT_MAX, FLT_MAX };
    engine::Vector3 max = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    bool hasVertices = false;

    for (const auto& mesh : modelData_.meshes) {
        for (const auto& vertex : mesh.vertices) {
            min.x = (std::min)(min.x, vertex.position.x);
            min.y = (std::min)(min.y, vertex.position.y);
            min.z = (std::min)(min.z, vertex.position.z);
            max.x = (std::max)(max.x, vertex.position.x);
            max.y = (std::max)(max.y, vertex.position.y);
            max.z = (std::max)(max.z, vertex.position.z);
            hasVertices = true;
        }
    }

    // 有効な頂点が一個でもあれば、その範囲を描画用の外接箱として使う。
    if (hasVertices) {
        localAabbMin_ = min;
        localAabbMax_ = max;

        // 境界の中心と各軸の寸法も、配置と描画側が参照できるよう保持する。
        center_ = { (min.x + max.x) / 2.0f, (min.y + max.y) / 2.0f, (min.z + max.z) / 2.0f };
        size_ = { max.x - min.x, max.y - min.y, max.z - min.z };
    }
    // 有効な頂点がなければ、カリングなどに使う外接箱へ既定値を設定する。
    else {
        localAabbMin_ = { -0.5f, -0.5f, -0.5f };
        localAabbMax_ = { 0.5f, 0.5f, 0.5f };
        center_ = { 0.0f, 0.0f, 0.0f };
        size_ = { 1.0f, 1.0f, 1.0f };
    }

    // 材質画像を読み込み、ベース色の未指定は白、係数や法線は線形色空間で扱う。
    for (auto& material : modelData_.materials) {
        material.textureHandle = engine::TextureManager::GetInstance()->Load(
            material.textureFilePath.empty() ? kDefaultWhiteTexture : material.textureFilePath,
            engine::TextureManager::TextureColorSpace::kSRGB);
        // 法線マップを使う指定とパスが揃えば、ベクトル値を保持する線形画像として読み込む。
        if (material.hasNormalMap && !material.normalMapPath.empty()) {
            material.normalMapHandle = engine::TextureManager::GetInstance()->Load(
                material.normalMapPath,
                engine::TextureManager::TextureColorSpace::kLinear);
        }

        // ORM画像が指定済みなら、粗さなどの係数を線形値として読み込む。
        if (material.hasOrmMap && !material.ormMapPath.empty()) {
            material.ormMapHandle = engine::TextureManager::GetInstance()->Load(
                material.ormMapPath,
                engine::TextureManager::TextureColorSpace::kLinear);
        }
    }

    // メッシュ単位で頂点とインデックスを転送する。失敗したメッシュは描画側がリソースの有無で除外する。
    for (auto& mesh : modelData_.meshes) {
        // 頂点かインデックスが空のメッシュは描画できないため、GPUバッファを作らない。
        if (mesh.vertices.empty() || mesh.indices.empty()) {
            continue;
        }

        mesh.vertexResource = dxCommon->CreateBufferResource(sizeof(VertexData) * mesh.vertices.size());

        // 頂点バッファを確保できなければ、そのメッシュの転送を飛ばす。
        if (!mesh.vertexResource) {
            DebugConsole::GetInstance()->AddLog("Model vertex buffer creation failed: " + sourceName);
            continue;
        }

        mesh.vertexBufferView.BufferLocation = mesh.vertexResource->GetGPUVirtualAddress();
        mesh.vertexBufferView.SizeInBytes = UINT(sizeof(VertexData) * mesh.vertices.size());
        mesh.vertexBufferView.StrideInBytes = sizeof(VertexData);

        VertexData* vertexData = nullptr;
        HRESULT vertexMapResult = mesh.vertexResource->Map(0, nullptr, reinterpret_cast<void**>(&vertexData));

        // 頂点の書き込みアドレスが取れない場合は、GPUバッファを解放して次のメッシュへ進む。
        if (FAILED(vertexMapResult) || !vertexData) {
            DebugConsole::GetInstance()->AddLog("Model vertex buffer map failed: " + sourceName);
            mesh.vertexResource.Reset();
            continue;
        }

        // 頂点を転送したら、CPUから参照するためのマップを閉じる。
        std::memcpy(vertexData, mesh.vertices.data(), sizeof(VertexData) * mesh.vertices.size());
        mesh.vertexResource->Unmap(0, nullptr);

        // 頂点データの確定後、面の頂点順を格納するインデックス領域を作る。
        mesh.indexResource = dxCommon->CreateBufferResource(sizeof(uint32_t) * mesh.indices.size());

        // インデックスを確保できなければ、そのメッシュの頂点バッファも外す。
        if (!mesh.indexResource) {
            DebugConsole::GetInstance()->AddLog("Model index buffer creation failed: " + sourceName);
            mesh.vertexResource.Reset();
            continue;
        }

        mesh.indexBufferView.BufferLocation = mesh.indexResource->GetGPUVirtualAddress();
        mesh.indexBufferView.SizeInBytes = UINT(sizeof(uint32_t) * mesh.indices.size());
        mesh.indexBufferView.Format = DXGI_FORMAT_R32_UINT;

        uint32_t* indexData = nullptr;
        HRESULT indexMapResult = mesh.indexResource->Map(0, nullptr, reinterpret_cast<void**>(&indexData));

        // インデックスを書き込めない場合は、今回の頂点とインデックスをどちらも使用しない。
        if (FAILED(indexMapResult) || !indexData) {
            DebugConsole::GetInstance()->AddLog("Model index buffer map failed: " + sourceName);
            mesh.vertexResource.Reset();
            mesh.indexResource.Reset();
            continue;
        }

        std::memcpy(indexData, mesh.indices.data(), sizeof(uint32_t) * mesh.indices.size());
        mesh.indexResource->Unmap(0, nullptr);
    }

    // メッシュの転送後に、モデルの基準材質を送る定数バッファを用意する。
    materialData_ = nullptr;
    materialResource_ = dxCommon->CreateBufferResource(sizeof(Material));

    // 材質バッファを確保できない場合は、材質の初期値を書き込めないので終了する。
    if (!materialResource_) {
        DebugConsole::GetInstance()->AddLog("Model material buffer creation failed: " + sourceName);
        return;
    }

    HRESULT materialMapResult = materialResource_->Map(0, nullptr, reinterpret_cast<void**>(&materialData_));

    // 材質バッファをマップできない場合は、GPUリソースとCPU側の参照を初期状態に戻す。
    if (FAILED(materialMapResult) || !materialData_) {
        DebugConsole::GetInstance()->AddLog("Model material buffer map failed: " + sourceName);
        materialResource_.Reset();
        materialData_ = nullptr;
        return;
    }

    // 材質が指定されていないモデルでも使える既定の描画値を用意する。
    materialData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };
    materialData_->enableLighting = true;
    materialData_->selectedLighting = 2;
    materialData_->shininess = 50;
    materialData_->materialType = 0;
    materialData_->roughness = 0.5f;
    materialData_->metallic = 0.0f;
    materialData_->enableNormalMap = 1;
    materialData_->enableEnvMap = 0;
    materialData_->envIntensity = 1.0f;
    materialData_->emissive = 1.0f;
    materialData_->time = 0.0f;
    materialData_->portalClipEnabled = 0.0f;
    materialData_->portalClipProgress = 0.0f;
    materialData_->portalClipCenter = { 0.0f, 0.0f, 0.0f };
    materialData_->portalClipEdgeWidth = 0.12f;
    materialData_->portalClipNormal = { 0.0f, 0.0f, 1.0f };
    materialData_->portalClipDissolve = 0.18f;
    materialData_->portalClipColor = { 1.0f, 0.82f, 0.36f, 0.80f };
    materialData_->uvTransform = math_.MakeIdentity4x4();

    // モデルの基準材質を取得できた場合は、既定値をインポートした材質値で上書きする。
    if (const MaterialData* primaryMaterial = GetPrimaryMaterialData()) {
        materialData_->color = primaryMaterial->baseColorFactor;
        materialData_->roughness = primaryMaterial->roughness;
        materialData_->metallic = primaryMaterial->metallic;
        materialData_->enableNormalMap = primaryMaterial->hasNormalMap ? 1 : 0;
    }

    // ボーン行列の参照を作ってから、それを入力とする計算スキニングの出力先を作る。
    CreateBoneBuffer();
    CreateComputeSkinningResources();
}

// スキニング用のボーン行列バッファを作成し、SRVとしてシェーダーから参照できる状態にする。
void Model::CreateBoneBuffer() {
    DirectXCommon* dxCommon = common_->GetDxCommon();

    // ボーンがないモデルへ切り替わった場合は、前の姿勢用の参照を残さない。
    if (modelData_.bones.empty()) {
        boneResource_.Reset();
        boneMappedData_ = nullptr;
        boneSrvIndex_ = 0;
        return;
    }

    // StructuredBufferで関節一つにつき一要素として扱います。GPUへ渡す要素間隔も同じ型のバイト数を使います。
    UINT sizeInBytes = sizeof(BoneForGpu) * static_cast<UINT>(modelData_.bones.size());
    boneResource_ = dxCommon->CreateBufferResource(sizeInBytes);

    // ボーン行列のバッファを確保できなければ、行列の転送先を無効にする。
    if (!boneResource_) {
        DebugConsole::GetInstance()->AddLog("Model bone buffer creation failed.");
        boneMappedData_ = nullptr;
        boneSrvIndex_ = 0;
        return;
    }

    // 毎回の姿勢更新をCPUから書けるよう、ボーン行列の領域をマップする。
    HRESULT boneMapResult = boneResource_->Map(0, nullptr, reinterpret_cast<void**>(&boneMappedData_));

    // ボーンの転送先をマップできない場合は、バッファも使わずに終了する。
    if (FAILED(boneMapResult) || !boneMappedData_) {
        DebugConsole::GetInstance()->AddLog("Model bone buffer map failed.");
        boneResource_.Reset();
        boneMappedData_ = nullptr;
        boneSrvIndex_ = 0;
        return;
    }

    // ボーン行列を単位行列で初期化し、最初のアニメーション更新前も元の頂点を描く。
    engine::Math math;

    for (size_t i = 0; i < modelData_.bones.size(); ++i) {
        boneMappedData_[i].finalMatrix = math.MakeIdentity4x4();
    }

    // ボーン行列を要素単位で読めるStructuredBufferとして登録する。
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = UINT(modelData_.bones.size());
    srvDesc.Buffer.StructureByteStride = sizeof(BoneForGpu);
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    boneSrvIndex_ = engine::SrvManager::GetInstance()->Allocate();
    engine::SrvManager::GetInstance()->CreateSrvForResource(boneSrvIndex_, boneResource_.Get(), srvDesc);
}

// 現在のスケルトン行列をGPU側のボーンバッファへ転送する。
void Model::UpdateBoneBuffer() {
    // ボーン行列を転送できるCPUアドレスがなければ、姿勢の更新を行わない。
    if (!boneMappedData_) {
        return;
    }

    // スキニングもノードアニメーションの代替処理も使わないモデルでは、頂点を変形しない単位行列で埋めます。
    if (!modelData_.hasSkinning && !modelData_.usesNodeAnimationProxy) {
        for (size_t i = 0; i < modelData_.bones.size(); ++i) {
            boneMappedData_[i].finalMatrix = math_.MakeIdentity4x4();
        }

        computeSkinningDirty_ = true;
        return;
    }

    // ボーン名に対応するジョイントを探し、それぞれ現在の姿勢へ変換する。
    for (size_t i = 0; i < modelData_.bones.size(); ++i) {
        auto it = modelData_.skeleton.jointMap.find(modelData_.bones[i].name);

        // ボーンに対応するジョイントがあれば、現在の姿勢と逆バインド姿勢を合成する。
        if (it != modelData_.skeleton.jointMap.end()) {
            // バインド姿勢を一度打ち消し、現在のジョイント姿勢を適用する順に掛けます。
            const Joint& joint = modelData_.skeleton.joints[it->second];
            boneMappedData_[i].finalMatrix =
                math_.Multiply(modelData_.bones[i].inverseBindPoseMatrix, joint.skeletonSpaceMatrix);
        }
        // 姿勢側に対応するジョイントがないボーンには、頂点を変形しない単位行列を送る。
        else {
            boneMappedData_[i].finalMatrix = math_.MakeIdentity4x4();
        }
    }

    // 行列を更新したため、次の描画前に計算スキニングの出力を作り直す。
    computeSkinningDirty_ = true;
}

// 計算シェーダーが出力するスキニング済み頂点のバッファをメッシュごとに準備する。
void Model::CreateComputeSkinningResources() {
    // 再生成前に旧出力を捨て、未対応モデルではCPU側の頂点バッファを使える状態へ戻す。
    computeSkinningAvailable_ = false;
    computeSkinningDirty_ = true;
    computeSkinningOutputReady_ = false;

    for (auto& mesh : modelData_.meshes) {
        mesh.computeSkinnedVertexResource.Reset();
        mesh.computeSkinnedVertexBufferView = {};
        mesh.computeSkinnedVertexState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    // GPU計算に必要な共有基盤、スキニング設定、ボーンバッファが揃ってから出力を作る。
    if (!common_ || !common_->IsComputeSkinningAvailable() ||
        !modelData_.hasSkinning || !boneResource_ || modelData_.bones.empty()) {
        return;
    }

    ID3D12Device* device = common_->GetDxCommon()->GetDevice();
    bool hasSkinnedMesh = false;

    for (auto& mesh : modelData_.meshes) {
        // 頂点バッファと内容を持つメッシュだけ、スキニング済みの出力先を作る。
        if (!mesh.vertexResource || mesh.vertices.empty()) {
            continue;
        }

        // スキニング結果を頂点数分だけGPUのUAVへ書き込めるように確保する。
        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heapProperties.CreationNodeMask = 1;
        heapProperties.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC resourceDesc{};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDesc.Width = sizeof(VertexData) * mesh.vertices.size();
        resourceDesc.Height = 1;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        const HRESULT result = device->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(mesh.computeSkinnedVertexResource.GetAddressOf()));
        // 出力バッファの作成に失敗した場合は、一部のメッシュだけがGPU計算を使う状態を避ける。
        if (FAILED(result)) {
            DebugConsole::GetInstance()->AddLog("Compute skinning output buffer creation failed.");

            for (auto& resetMesh : modelData_.meshes) {
                resetMesh.computeSkinnedVertexResource.Reset();
                resetMesh.computeSkinnedVertexBufferView = {};
                resetMesh.computeSkinnedVertexState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }

            return;
        }

        mesh.computeSkinnedVertexBufferView.BufferLocation =
            mesh.computeSkinnedVertexResource->GetGPUVirtualAddress();
        mesh.computeSkinnedVertexBufferView.SizeInBytes =
            static_cast<UINT>(sizeof(VertexData) * mesh.vertices.size());
        mesh.computeSkinnedVertexBufferView.StrideInBytes = sizeof(VertexData);
        hasSkinnedMesh = true;
    }

    computeSkinningAvailable_ = hasSkinnedMesh;
}

// 変形済み頂点が利用できる状態ならtrueを返す。必要な場合だけGPU計算の命令を積む。
bool Model::PrepareComputeSkinning() {
    // 計算用の基盤やボーンバッファが使えない場合は、スキニング済み頂点を用意できない。
    if (!computeSkinningAvailable_ || !common_ ||
        !common_->IsComputeSkinningAvailable() || !boneResource_) {
        return false;
    }

    // ボーン更新がなく前回の出力が有効なら、スキニングを再実行せず完了とする。
    if (!computeSkinningDirty_ && computeSkinningOutputReady_) {
        return true;
    }

    ID3D12GraphicsCommandList* commandList = common_->GetDxCommon()->GetCommandList();
    commandList->SetComputeRootSignature(common_->GetComputeSkinningRootSignature());
    commandList->SetPipelineState(common_->GetComputeSkinningPipelineState());
    commandList->SetComputeRootShaderResourceView(1, boneResource_->GetGPUVirtualAddress());

    bool dispatched = false;

    for (auto& mesh : modelData_.meshes) {
        // 入力頂点と計算の出力先が両方揃ったメッシュだけ、GPU計算へ送る。
        if (!mesh.vertexResource || !mesh.computeSkinnedVertexResource || mesh.vertices.empty()) {
            continue;
        }

        // 出力先がGPU書き込み状態にない場合だけ、UAVとして書ける状態へ切り替える。
        if (mesh.computeSkinnedVertexState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            D3D12_RESOURCE_BARRIER toUnorderedAccess{};
            toUnorderedAccess.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toUnorderedAccess.Transition.pResource = mesh.computeSkinnedVertexResource.Get();
            toUnorderedAccess.Transition.StateBefore = mesh.computeSkinnedVertexState;
            toUnorderedAccess.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            toUnorderedAccess.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            commandList->ResourceBarrier(1, &toUnorderedAccess);
            mesh.computeSkinnedVertexState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }

        // 元の頂点とボーンを読み取り、現在のメッシュに対応する出力へ書き込む。
        commandList->SetComputeRootShaderResourceView(0, mesh.vertexResource->GetGPUVirtualAddress());
        commandList->SetComputeRootUnorderedAccessView(
            2,
            mesh.computeSkinnedVertexResource->GetGPUVirtualAddress());

        const uint32_t dispatchConstants[2] = {
            static_cast<uint32_t>(mesh.vertices.size()),
            static_cast<uint32_t>(modelData_.bones.size())
        };

        commandList->SetComputeRoot32BitConstants(
            3,
            static_cast<UINT>(std::size(dispatchConstants)),
            dispatchConstants,
            0);

        const uint32_t groupCount =
            (static_cast<uint32_t>(mesh.vertices.size()) + 63u) / 64u;

        // 1グループ64頂点で計算し、UAV完了を待ってから描画用頂点バッファへ遷移する。
        commandList->Dispatch(groupCount, 1, 1);
        engine::RenderStats::GetInstance()->RecordComputeDispatch(groupCount);

        D3D12_RESOURCE_BARRIER unorderedAccessBarrier{};
        unorderedAccessBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        unorderedAccessBarrier.UAV.pResource = mesh.computeSkinnedVertexResource.Get();
        commandList->ResourceBarrier(1, &unorderedAccessBarrier);

        D3D12_RESOURCE_BARRIER toVertexBuffer{};
        toVertexBuffer.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toVertexBuffer.Transition.pResource = mesh.computeSkinnedVertexResource.Get();
        toVertexBuffer.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toVertexBuffer.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        toVertexBuffer.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &toVertexBuffer);
        mesh.computeSkinnedVertexState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        dispatched = true;
    }

    // 有効な計算命令を記録できた場合は、ボーン更新で無効になるまで結果を再利用する。
    computeSkinningOutputReady_ = dispatched;
    computeSkinningDirty_ = !dispatched;
    return dispatched;
}

const D3D12_VERTEX_BUFFER_VIEW& Model::GetActiveVertexBufferView(const Mesh& mesh) const {
    // スキニング済みで更新待ちがなく、出力先も有効なメッシュでは、変形後の頂点を返す。
    if (computeSkinningAvailable_ && computeSkinningOutputReady_ && !computeSkinningDirty_ &&
        mesh.computeSkinnedVertexResource) {
        return mesh.computeSkinnedVertexBufferView;
    }

    return mesh.vertexBufferView;
}

// 通常描画の入力を設定し、メッシュごとの材質と頂点を束縛してインスタンスを描画する。
void Model::Draw(
    ID3D12Resource* wvpResource,
    ID3D12Resource* directionalLightResource,
    ID3D12Resource* cameraResource,
    ID3D12Resource* pointLightResource,
    ID3D12Resource* spotLightResource,
    ID3D12Resource* overrideMaterialResource,
    uint32_t normalMapHandle,
    uint32_t ormMapHandle,
    uint32_t overrideTextureHandle,
    uint32_t instanceCount,
    uint32_t startInstanceLocation,
    std::int32_t meshDrawIndex) {
    ID3D12GraphicsCommandList* commandList = common_->GetDxCommon()->GetCommandList();
    engine::RenderStats::GetInstance()->RecordObjectDraw();

    // 呼び出し元から上書き材質を渡された場合は、両方の描画用スロットに同じ材質を送る。
    if (overrideMaterialResource) {
        commandList->SetGraphicsRootConstantBufferView(0, overrideMaterialResource->GetGPUVirtualAddress());
        commandList->SetGraphicsRootConstantBufferView(14, overrideMaterialResource->GetGPUVirtualAddress());
    }
    // 上書き材質を指定されなかった場合は、モデル自身の材質バッファを使用する。
    else if (materialResource_) {
        commandList->SetGraphicsRootConstantBufferView(0, materialResource_->GetGPUVirtualAddress());
        commandList->SetGraphicsRootConstantBufferView(14, materialResource_->GetGPUVirtualAddress());
    }

    // WVPバッファを受け取った場合は、その変換行列を頂点描画へ結び付ける。
    if (wvpResource)
        commandList->SetGraphicsRootConstantBufferView(1, wvpResource->GetGPUVirtualAddress());

    // 平行光のバッファが指定されている場合だけ、その定数を渡す。
    if (directionalLightResource)
        commandList->SetGraphicsRootConstantBufferView(3, directionalLightResource->GetGPUVirtualAddress());
    // カメラのバッファが指定されている場合だけ、視点の定数を渡す。
    if (cameraResource)
        commandList->SetGraphicsRootConstantBufferView(4, cameraResource->GetGPUVirtualAddress());
    // 点光源のバッファが指定されている場合だけ、その定数を渡す。
    if (pointLightResource)
        commandList->SetGraphicsRootConstantBufferView(5, pointLightResource->GetGPUVirtualAddress());
    // スポットライトのバッファが指定されている場合だけ、その定数を渡す。
    if (spotLightResource)
        commandList->SetGraphicsRootConstantBufferView(6, spotLightResource->GetGPUVirtualAddress());


    // ボーン情報と実際に参照できるSRVの両方があるモデルだけ、ボーン行列を描画へ渡す。
    if (!modelData_.bones.empty() && boneResource_ && boneSrvIndex_ != 0) {
        engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 7, boneSrvIndex_);
    }

    uint32_t envMapHandle = LightManager::GetInstance()->GetEnvironmentMapHandle();

    // 環境マップを登録済みの場合だけ、反射用の画像を記述子テーブルへ追加する。
    if (envMapHandle != 0) {
        engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 8, envMapHandle);
    }

    // 対象メッシュごとに材質の差分、画像、頂点を確定して描画する。
    for (size_t meshIndex = 0; meshIndex < modelData_.meshes.size(); ++meshIndex) {
        // 特定のメッシュ番号を指定された場合は、その番号以外を飛ばす。負値は全メッシュを対象にする。
        if (meshDrawIndex >= 0 && meshIndex != static_cast<size_t>(meshDrawIndex)) {
            continue;
        }

        const auto& mesh = modelData_.meshes[meshIndex];

        // 頂点・インデックスのリソースと面データが揃うメッシュだけ描画する。
        if (!mesh.vertexResource || !mesh.indexResource || mesh.indices.empty()) {
            continue;
        }

        const MaterialData* meshMaterial = nullptr;

        // インポートした材質の添字が有効な場合だけ、メッシュ固有の設定を取得する。
        if (mesh.materialIndex < modelData_.materials.size()) {
            meshMaterial = &modelData_.materials[mesh.materialIndex];
        }

        std::array<float, 6> meshMaterialFactors = {
            1.0f, 1.0f, 1.0f, 1.0f,
            1.0f,
            0.0f
        };

        const MaterialData* primaryMaterial = GetPrimaryMaterialData();
        const bool usesModelTexture =
            overrideTextureHandle == 0 || overrideTextureHandle >= DirectXCommon::kMaxSrvCount;
        // 上書き画像を使わず、基準・メッシュの両方の材質があれば、色と粗さの倍率と金属度の差を作る。
        if (usesModelTexture && meshMaterial && primaryMaterial) {
            meshMaterialFactors[0] = CalculateMaterialMultiplier(
                meshMaterial->baseColorFactor.x, primaryMaterial->baseColorFactor.x);
            meshMaterialFactors[1] = CalculateMaterialMultiplier(
                meshMaterial->baseColorFactor.y, primaryMaterial->baseColorFactor.y);
            meshMaterialFactors[2] = CalculateMaterialMultiplier(
                meshMaterial->baseColorFactor.z, primaryMaterial->baseColorFactor.z);
            meshMaterialFactors[3] = CalculateMaterialMultiplier(
                meshMaterial->baseColorFactor.w, primaryMaterial->baseColorFactor.w);
            meshMaterialFactors[4] = CalculateMaterialMultiplier(
                meshMaterial->roughness, primaryMaterial->roughness);
            meshMaterialFactors[5] = meshMaterial->metallic - primaryMaterial->metallic;
        }

        commandList->SetGraphicsRoot32BitConstants(
            13,
            static_cast<UINT>(meshMaterialFactors.size()),
            meshMaterialFactors.data(),
            0);

        // オブジェクト側の上書きを優先し、なければメッシュ材質、どちらも使えなければ共有の白画像を束縛します。ORMも同じ優先順です。
        uint32_t meshNormalHandle = normalMapHandle;
        // オブジェクト側の法線画像を使えないときは、有効なメッシュ材質の法線画像を優先する。
        if ((meshNormalHandle == 0 || meshNormalHandle >= DirectXCommon::kMaxSrvCount) &&
            meshMaterial && meshMaterial->hasNormalMap &&
            meshMaterial->normalMapHandle > 0 && meshMaterial->normalMapHandle < DirectXCommon::kMaxSrvCount) {
            meshNormalHandle = meshMaterial->normalMapHandle;
        }

        // どちらの法線画像も利用できなければ、共有の白画像をSRVとして使用する。
        if (meshNormalHandle == 0 || meshNormalHandle >= DirectXCommon::kMaxSrvCount) {
            meshNormalHandle = GetDefaultWhiteTextureHandle();
        }

        engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 9, meshNormalHandle);

        uint32_t meshOrmHandle = ormMapHandle;
        // オブジェクト側のORM画像を使えない場合は、メッシュ材質のORM画像を確認する。
        if ((meshOrmHandle == 0 || meshOrmHandle >= DirectXCommon::kMaxSrvCount) &&
            meshMaterial && meshMaterial->hasOrmMap &&
            meshMaterial->ormMapHandle > 0 && meshMaterial->ormMapHandle < DirectXCommon::kMaxSrvCount) {
            meshOrmHandle = meshMaterial->ormMapHandle;
        }

        // 両方のORM画像が無効なら、記述子を空にしないよう共有の白画像を使う。
        if (meshOrmHandle == 0 || meshOrmHandle >= DirectXCommon::kMaxSrvCount) {
            meshOrmHandle = GetDefaultWhiteTextureHandle();
        }

        engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 10, meshOrmHandle);

        // 利用可能なら計算スキニング済みの頂点を使い、描画に必要な元のインデックスを対にする。
        const D3D12_VERTEX_BUFFER_VIEW& activeVertexBufferView = GetActiveVertexBufferView(mesh);
        commandList->IASetVertexBuffers(0, 1, &activeVertexBufferView);
        commandList->IASetIndexBuffer(&mesh.indexBufferView);

        // 上書きテクスチャが有効であれば、メッシュ固有の画像より先に採用する。
        if (overrideTextureHandle > 0 && overrideTextureHandle < DirectXCommon::kMaxSrvCount) {
            engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 2, overrideTextureHandle);
        }
        // 有効な上書きがなく、メッシュの材質が存在する場合は、インポートした画像を使う。
        else if (mesh.materialIndex < modelData_.materials.size()) {
            uint32_t handle = modelData_.materials[mesh.materialIndex].textureHandle;
            engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 2, handle);
        }

        commandList->DrawIndexedInstanced(UINT(mesh.indices.size()), instanceCount, 0, 0, startInstanceLocation);
        engine::RenderStats::GetInstance()->RecordIndexedDraw(
            static_cast<uint32_t>(mesh.indices.size()), instanceCount);
    }
}

// モデル全体の頂点数を返し、統計表示や軽量化確認で使えるようにする。
uint32_t Model::GetVertexCount() const {
    uint32_t count = 0;

    for (const auto& mesh : modelData_.meshes)
        count += static_cast<uint32_t>(mesh.vertices.size());
    return count;
}

// インデックス数から概算ポリゴン数を返し、描画負荷の目安にする。
uint32_t Model::GetPolygonCount() const {
    uint32_t count = 0;

    for (const auto& mesh : modelData_.meshes)
        count += static_cast<uint32_t>(mesh.indices.size() / 3);
    return count;
}


} // namespace engine
