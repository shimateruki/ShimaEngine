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
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include "SrvManager.h"
#include <DebugConsole.h>
#include <LightManager.h>

namespace engine {




// ========================================================================
// Model 描画補助と動的メッシュ生成
// ------------------------------------------------------------------------
// 影用描画、メッシュのみの描画、プリミティブ生成など、読み込み以外の
// メッシュ利用処理を担当する。
// ========================================================================

// シャドウマップ生成用に、マテリアル描画を省いた深度向けメッシュ描画を行う。
void Model::DrawShadow(ID3D12Resource* wvpResource, std::int32_t meshDrawIndex) {
    ID3D12GraphicsCommandList* commandList = common_->GetDxCommon()->GetCommandList();
    engine::RenderStats::GetInstance()->RecordObjectDraw();

    // [0] wvp
    commandList->SetGraphicsRootConstantBufferView(0, wvpResource->GetGPUVirtualAddress());

    // ボーンを持つモデルだけ、影の頂点変形で使うボーン行列のSRVを結び付ける。
    if (!modelData_.bones.empty()) {
        engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 1, boneSrvIndex_);
    }

    // 各メッシュの頂点を描画
    for (size_t meshIndex = 0; meshIndex < modelData_.meshes.size(); ++meshIndex) {
        // 特定のメッシュ番号を指定された場合は、それ以外の影を描かない。
        if (meshDrawIndex >= 0 && meshIndex != static_cast<size_t>(meshDrawIndex)) {
            continue;
        }

        auto& mesh = modelData_.meshes[meshIndex];

        // 頂点・インデックスのバッファが揃わないか面のインデックスが空なら、このメッシュの描画を飛ばす。
        if (!mesh.vertexResource || !mesh.indexResource || mesh.indices.empty()) {
            continue;
        }

        const D3D12_VERTEX_BUFFER_VIEW& activeVertexBufferView = GetActiveVertexBufferView(mesh);
        commandList->IASetVertexBuffers(0, 1, &activeVertexBufferView);
        commandList->IASetIndexBuffer(&mesh.indexBufferView);
        commandList->DrawIndexedInstanced(UINT(mesh.indices.size()), 1, 0, 0, 0);
        engine::RenderStats::GetInstance()->RecordIndexedDraw(static_cast<uint32_t>(mesh.indices.size()));
    }
}

// 既に外側で必要な描画状態を設定している前提で、メッシュ本体だけを描画する。
void Model::DrawMeshOnly(std::int32_t meshDrawIndex) {
    ID3D12GraphicsCommandList* commandList = common_->GetDxCommon()->GetCommandList();
    engine::RenderStats::GetInstance()->RecordObjectDraw();

    // 呼び出し元が用意したパイプラインへ、各メッシュの頂点とインデックスだけを渡す。
    for (size_t meshIndex = 0; meshIndex < modelData_.meshes.size(); ++meshIndex) {
        // 特定のメッシュ番号を指定された場合は、そのメッシュだけを描く。
        if (meshDrawIndex >= 0 && meshIndex != static_cast<size_t>(meshDrawIndex)) {
            continue;
        }

        const auto& mesh = modelData_.meshes[meshIndex];

        // 頂点とインデックスで描画できる状態のメッシュだけ、コマンドを記録する。
        if (!mesh.vertexResource || !mesh.indexResource || mesh.indices.empty()) {
            continue;
        }

        // 頂点バッファをセット
        const D3D12_VERTEX_BUFFER_VIEW& activeVertexBufferView = GetActiveVertexBufferView(mesh);
        commandList->IASetVertexBuffers(0, 1, &activeVertexBufferView);
        commandList->IASetIndexBuffer(&mesh.indexBufferView);
        commandList->DrawIndexedInstanced(UINT(mesh.indices.size()), 1, 0, 0, 0);
        engine::RenderStats::GetInstance()->RecordIndexedDraw(static_cast<uint32_t>(mesh.indices.size()));
    }
}

// 頂点配列から一時的なモデルデータを構築し、プリミティブやデバッグ表示に使える形へ変換する。
void Model::CreateFromVertices(ModelCommon* common, const std::vector<VertexData>& vertices, const std::vector<uint32_t>& indices) {
    // 手作りメッシュへ置き換えるので、以前のモデルのスキニング済み頂点を参照しない状態へ戻します。
    computeSkinningAvailable_ = false;
    computeSkinningDirty_ = true;
    computeSkinningOutputReady_ = false;

    // 描画基盤か頂点・インデックスの入力が欠けていれば、前のメッシュを外して終了する。
    if (!common || vertices.empty() || indices.empty()) {
        modelData_.meshes.clear();
        return;
    }

    assert(common);

    common_ = common;
    DirectXCommon* dxCommon = common_->GetDxCommon();

    // 共通の描画基盤からDirectXを取得できなければ、GPUバッファを作れない。
    if (!dxCommon) {
        DebugConsole::GetInstance()->AddLog(LogLevel::kError, "[Model] CreateFromVertices: DirectX device is null.");
        modelData_.meshes.clear();
        return;
    }

    ID3D12Device* device = dxCommon->GetDevice();

    // DirectXのデバイスがない場合は、新しいメッシュを有効にできない。
    if (!device) {
        DebugConsole::GetInstance()->AddLog(LogLevel::kError, "[Model] CreateFromVertices: DirectX device is null.");
        modelData_.meshes.clear();
        return;
    }

    // エディターから作り直す場合も旧メッシュが残らないよう、現在の一覧を先に空にする。
    modelData_.meshes.clear();

    // 動的生成用のメッシュを1つ追加
    modelData_.meshes.emplace_back();
    auto& mesh = modelData_.meshes.back();

    // 頂点とインデックスのデータを保持
    mesh.vertices = vertices;
    mesh.indices = indices;
    mesh.materialIndex = 0;

    // 頂点数に1頂点のバイト数を掛け、CPUから直接書き込めるUpload Heapに確保します。
    UINT vbSize = static_cast<UINT>(sizeof(VertexData) * vertices.size());

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC vbDesc{};
    vbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    vbDesc.Width = vbSize;
    vbDesc.Height = 1;
    vbDesc.DepthOrArraySize = 1;
    vbDesc.MipLevels = 1;
    vbDesc.SampleDesc.Count = 1;
    vbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &vbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mesh.vertexResource)
    );

    // 頂点バッファの確保に失敗した場合は、不完全なメッシュを使用しない。
    if (FAILED(hr) || !mesh.vertexResource) {
        DebugConsole::GetInstance()->AddLog(LogLevel::kError, "[Model] CreateFromVertices: failed to create vertex buffer.");
        modelData_.meshes.clear();
        return;
    }

    // 頂点配列を書き込むため、確保したバッファをCPUのアドレス空間へマップする。
    void* mappedVerts = nullptr;
    hr = mesh.vertexResource->Map(0, nullptr, &mappedVerts);

    // CPUから書き込める頂点アドレスを取得できない場合は、頂点を転送せず終了する。
    if (FAILED(hr) || !mappedVerts) {
        DebugConsole::GetInstance()->AddLog(LogLevel::kError, "[Model] CreateFromVertices: failed to map vertex buffer.");
        modelData_.meshes.clear();
        return;
    }

    // 書き込み後はMapを解除し、描画が参照する番地と1頂点の間隔をビューへ登録します。
    memcpy(mappedVerts, vertices.data(), vbSize);
    mesh.vertexResource->Unmap(0, nullptr);

    mesh.vertexBufferView.BufferLocation = mesh.vertexResource->GetGPUVirtualAddress();
    mesh.vertexBufferView.SizeInBytes = vbSize;
    mesh.vertexBufferView.StrideInBytes = sizeof(VertexData);

    // インデックスは32ビット値です。この配列で頂点を共有しながら三角形の組み合わせを指定します。
    UINT ibSize = static_cast<UINT>(sizeof(uint32_t) * indices.size());

    D3D12_RESOURCE_DESC ibDesc = vbDesc;
    ibDesc.Width = ibSize;

    // 頂点と同様に、インデックスを書き込めるバッファを確保する。
    hr = device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &ibDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mesh.indexResource)
    );

    // インデックスバッファを確保できなければ、頂点だけの未完成メッシュを破棄する。
    if (FAILED(hr) || !mesh.indexResource) {
        DebugConsole::GetInstance()->AddLog(LogLevel::kError, "[Model] CreateFromVertices: failed to create index buffer.");
        modelData_.meshes.clear();
        return;
    }

    // 32ビットのインデックス配列を書き込むため、バッファをCPUへマップする。
    void* mappedIndices = nullptr;
    hr = mesh.indexResource->Map(0, nullptr, &mappedIndices);

    // インデックスの書き込み先を取得できなければ、そのメッシュを描画に渡さない。
    if (FAILED(hr) || !mappedIndices) {
        DebugConsole::GetInstance()->AddLog(LogLevel::kError, "[Model] CreateFromVertices: failed to map index buffer.");
        modelData_.meshes.clear();
        return;
    }

    // インデックスも転送したあと、GPUが32ビット単位で読むようビューの形式を揃えます。
    memcpy(mappedIndices, indices.data(), ibSize);
    mesh.indexResource->Unmap(0, nullptr);

    mesh.indexBufferView.BufferLocation = mesh.indexResource->GetGPUVirtualAddress();
    mesh.indexBufferView.SizeInBytes = ibSize;
    mesh.indexBufferView.Format = DXGI_FORMAT_R32_UINT;
}


} // namespace engine
