#include "SceneRenderCommands.h"
#include "DirectXCommon.h"
#include "SpriteCommon.h"
#include "Camera.h"
#include "GpuParticleManager.h"

namespace engine {
// DirectXCommonを借用する。描画コマンドは各呼び出し時に取得したコマンドリストへ記録する。
SceneRenderCommands::SceneRenderCommands(engine::DirectXCommon& dxCommon) : dxCommon_(dxCommon) {}

// 現在のコマンドリストにSprite描画用のパイプラインを設定します。
void SceneRenderCommands::BindSprites(SpriteCommon& sprites) {
    sprites.SetPipeline(dxCommon_.GetCommandList());
}

// 現在の描画先がカメラプレビューかを返します。
bool SceneRenderCommands::IsCameraPreviewRendering() const {
    return dxCommon_.IsCameraPreviewRendering();
}

// 屈折などの後段で読めるよう、描画途中のシーン色を退避用テクスチャへコピーします。
void SceneRenderCommands::CopySceneColor() {
    dxCommon_.UpdateGrabTexture();
}

// 深度テストとシェーダーからの深度参照を両立する読み取り専用状態へ切り替えます。
void SceneRenderCommands::BeginDepthRead() {
    dxCommon_.PreDrawLocalFog();
}

// 深度を通常の書き込み状態へ戻し、後続の描画へ渡します。
void SceneRenderCommands::EndDepthRead() {
    dxCommon_.PostDrawLocalFog();
}

// 現在の描画基盤が公開する深度テクスチャのSRV番号を返します。
uint32_t SceneRenderCommands::GetDepthSrvHandle() const {
    return dxCommon_.GetDepthSrvHandle();
}

// CopySceneColor()で退避した背景色を参照するSRV番号を返します。
uint32_t SceneRenderCommands::GetSceneColorSrvHandle() const {
    return dxCommon_.GetGrabSrvHandle();
}

void SceneRenderCommands::RecordGpuParticles(GpuParticleManager& particles, const engine::Camera& camera, uint32_t textureHandle) {
    // 今記録しているパスのコマンドリストへ粒子を積みます。指定カメラの行列と、描画基盤が公開している深度SRVを渡します。
    particles.Draw(dxCommon_.GetCommandList(), camera.GetViewMatrix(), camera.GetProjectionMatrix(),
        textureHandle, dxCommon_.GetDepthSrvHandle());
}
} // namespace engine
