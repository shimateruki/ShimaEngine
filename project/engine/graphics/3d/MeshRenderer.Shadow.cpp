#include "MeshRenderer.h"
#include "FoliageInteraction.h"
#include "DirectXCommon.h"
#include "ModelManager.h"
#include "CameraManager.h"
#include "LightManager.h"
#include "TextureManager.h"
#include "DebugConsole.h"
#include <cassert>
#include <SrvManager.h>

#include <algorithm>
#include <cmath>

namespace engine {


// ========================================================================
// MeshRenderer 影とローカルフォグ描画
// ------------------------------------------------------------------------
// シャドウマップ用描画と、深度を使ったローカルフォグ描画を担当する。
// 通常描画や特殊マテリアルとはパスが違うため、専用ファイルとして分ける。
// ========================================================================
void MeshRenderer::DrawShadow() {
    DrawShadowOnly();
}

void MeshRenderer::SetShadowCommonState() {
    // 共通描画設定が未接続の場合は、影用パイプラインを設定できないため終了する。
    if (!common_)
        return;
    common_->SetShadowGraphicsCommand();
    common_->SetShadowPipelineState();
}

void MeshRenderer::DrawShadowOnly() {
    Model* drawModel = ResolveDrawModel();
    // 描画モデル、共通設定、影用行列、マテリアルが揃わない場合は、影の頂点を送れないため終了する。
    if (!drawModel || !common_ || !shadowWvpResource_ || !materialResource_ || !materialData_)
        return;
    // 動くモデルでは、影の輪郭にも通常描画と同じスキニング後の頂点を使えるようにします。
    drawModel->PrepareComputeSkinning();

    // Compute実行でルートシグネチャとPSOが変わるため、影描画の状態を復元します。
    common_->SetShadowGraphicsCommand();
    const bool foliage = materialData_->materialType == 30 || materialData_->materialType == 31;
    common_->SetShadowPipelineState(foliage);
    // マテリアルと草の通過履歴をGPUへ渡し、材質に応じた影の輪郭を描く。
    auto* commandList = common_->GetDxCommon()->GetCommandList();
    commandList->SetGraphicsRootConstantBufferView(2, materialResource_->GetGPUVirtualAddress());
    commandList->SetGraphicsRoot32BitConstants(3, 32, foliageInteraction::samples.data(), 0);
    drawModel->DrawShadow(shadowWvpResource_.Get(), meshDrawIndex_);
}

void MeshRenderer::DrawLocalFog(uint32_t depthSrvHandle) {
    Model* drawModel = ResolveDrawModel();
    // 描画モデルと定数バッファのどれかが未準備なら、深度を参照するフォグ描画を省く。
    if (!drawModel || !common_ || !HasRequiredBuffers() || !localFogResource_)
        return;

    common_->SetLocalFogGraphicsCommand();
    ID3D12GraphicsCommandList* commandList = common_->GetDxCommon()->GetCommandList();


    // 不透明物の深度を読み、フォグの計算で不透明物の奥にある部分を切るために渡します。
    engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 2, depthSrvHandle);


    commandList->SetGraphicsRootConstantBufferView(3, localFogResource_->GetGPUVirtualAddress());

    // 個別モデルの描画では、DrawShadowがルートのWVPと必要なボーン行列を設定する。
    drawModel->DrawShadow(wvpResource_.Get(), meshDrawIndex_);
}

} // namespace engine
