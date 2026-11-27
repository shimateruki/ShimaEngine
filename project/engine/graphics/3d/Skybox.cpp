#include "Skybox.h"
#include "RenderStats.h"
#include "SrvManager.h"
#ifdef USE_IMGUI
#include "DebugConsole.h"
#endif
#include <cassert>

void Skybox::Initialize(engine::Object3DCommon* common, uint32_t textureHandle) {
    // 描画基盤とテクスチャを確定してから、空を囲むメッシュを一度だけ作る。
    assert(common);
    common_ = common;
    textureHandle_ = textureHandle;

    CreateMesh();

#ifdef USE_IMGUI
    DebugConsole::GetInstance()->AddLog("Skybox initialized. TextureHandle: " + std::to_string(textureHandle_));
#endif
}

// 内側から見える立方体の頂点とインデックスを、描画用のバッファに作ります。
void Skybox::CreateMesh() {
    auto device = common_->GetDxCommon()->GetDevice();

    // 1. 頂点データ (1辺2の立方体、原点中心)
    VertexData vertices[8] = {
        { {-1.0f,  1.0f, -1.0f, 1.0f} }, // 0:左上奥
        { { 1.0f,  1.0f, -1.0f, 1.0f} }, // 1:右上奥
        { {-1.0f, -1.0f, -1.0f, 1.0f} }, // 2:左下奥
        { { 1.0f, -1.0f, -1.0f, 1.0f} }, // 3:右下奥
        { {-1.0f,  1.0f,  1.0f, 1.0f} }, // 4:左上手前
        { { 1.0f,  1.0f,  1.0f, 1.0f} },  // 5:右上手前
        { {-1.0f, -1.0f,  1.0f, 1.0f} }, // 6:左下手前
        { { 1.0f, -1.0f,  1.0f, 1.0f} }, // 7:右下手前
    };

    // 2. インデックスデータ (内側から見えるように面を構成)
    uint32_t indices[36] = {
        0, 1, 2, 2, 1, 3, // 奥面
        4, 6, 5, 5, 6, 7, // 前面
        0, 4, 1, 1, 4, 5, // 上面
        2, 3, 6, 6, 3, 7, // 下面
        0, 2, 4, 4, 2, 6, // 左面
        1, 5, 3, 3, 5, 7  // 右面
    };

    // 頂点を GPU バッファへ転送し、描画で参照するアドレスと頂点間隔を控える。
    vertexResource_ = common_->GetDxCommon()->CreateBufferResource(sizeof(vertices));
    VertexData* vertexMap = nullptr;
    vertexResource_->Map(0, nullptr, reinterpret_cast<void**>(&vertexMap));
    std::copy(std::begin(vertices), std::end(vertices), vertexMap);
    vertexResource_->Unmap(0, nullptr);

    vertexBufferView_.BufferLocation = vertexResource_->GetGPUVirtualAddress();
    vertexBufferView_.SizeInBytes = sizeof(vertices);
    vertexBufferView_.StrideInBytes = sizeof(VertexData);

    // 面の頂点番号を転送し、32ビットインデックスとして描画へ渡す。
    indexResource_ = common_->GetDxCommon()->CreateBufferResource(sizeof(indices));
    uint32_t* indexMap = nullptr;
    indexResource_->Map(0, nullptr, reinterpret_cast<void**>(&indexMap));
    std::copy(std::begin(indices), std::end(indices), indexMap);
    indexResource_->Unmap(0, nullptr);

    indexBufferView_.BufferLocation = indexResource_->GetGPUVirtualAddress();
    indexBufferView_.SizeInBytes = sizeof(indices);
    indexBufferView_.Format = DXGI_FORMAT_R32_UINT;
}

void Skybox::Draw(ID3D12Resource* vpResource) {
    // 描画基盤またはビュー・射影のバッファが欠ける場合は、GPUへ無効な定数バッファアドレスを渡さないよう中止します。
    if (!common_ || !vpResource) {
#ifdef USE_IMGUI
        static bool sLoggedInvalidDraw = false;

        // 最初の無効な描画要求だけ、ImGuiを使う構成でログへ残す。
        if (!sLoggedInvalidDraw) {
            DebugConsole::GetInstance()->AddLog("[WARN] Skybox draw skipped: invalid common or camera buffer.");
            sLoggedInvalidDraw = true;
        }
#endif
        return;
    }

#ifdef USE_IMGUI
    static bool sLoggedDraw = false;

    // 有効な描画経路へ初めて到達した場合だけ記録する。
    if (!sLoggedDraw) {
        DebugConsole::GetInstance()->AddLog("Skybox draw reached.");
        sLoggedDraw = true;
    }
#endif

    auto commandList = common_->GetDxCommon()->GetCommandList();

    // 空のパイプラインとルートシグネチャを選んでから、後続の定数と画像を接続する。
    commandList->SetPipelineState(common_->GetSkyboxPipelineState());
    commandList->SetGraphicsRootSignature(common_->GetSkyboxRootSignature());

    // キューブマップのSRVを参照できるヒープを接続する。
    engine::SrvManager::GetInstance()->SetDescriptorHeaps(commandList);

    // RootParameter 0: ViewProjection (定数バッファ)
    commandList->SetGraphicsRootConstantBufferView(0, vpResource->GetGPUVirtualAddress());

    // RootParameter 1: キューブマップテクスチャ
    engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 1, textureHandle_);

    // 立方体の頂点と面を組み合わせ、内側の面を描けるようにする。
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 1, &vertexBufferView_);
    commandList->IASetIndexBuffer(&indexBufferView_);

    // 12枚の三角形を描き、描画統計にも記録する。
    commandList->DrawIndexedInstanced(36, 1, 0, 0, 0);
    engine::RenderStats::GetInstance()->RecordIndexedDraw(36);
}
