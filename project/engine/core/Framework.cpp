#include "Framework.h"
#include "TextureManager.h"
#include "ModelManager.h"
#include "SrvManager.h" 
#include"ImGuiManager.h"
#include"LightManager.h"
#include "engine/system/scene/LoadingProfiler.h"

namespace engine {
void Framework::Initialize() {
	// ウィンドウ、DirectX、入力、音声、ImGui、各種マネージャを起動順に初期化する。
    StartupProfileScope frameworkProfile("Framework::Initialize");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // 描画先となるウィンドウを先に作り、DirectXのスワップチェーンへ渡す。
    winApp_ = std::make_unique<WinApp>();
    winApp_->Initialize(L"スライムの大冒険", 1280, 720);

    dxCommon_ = DirectXCommon::GetInstance();
    {
        StartupProfileScope phase("DirectX device");
        dxCommon_->Initialize(winApp_.get());
    }

    // ウィンドウに結び付く入力と、独立した音声サービスを起動する。
    engine::InputManager::GetInstance()->Initialize(winApp_->GetHwnd());

    audioPlayer_ = AudioPlayer::GetInstance();
    audioPlayer_->Initialize();

    // 先に描画用の記述子を確保できるようにし、深度・影のビューとImGuiの初期化を続ける。
    engine::SrvManager::GetInstance()->Initialize(dxCommon_);
    dxCommon_->CreateDepthSrv();
    dxCommon_->CreateShadowMap();
    ImGuiManager::GetInstance()->Initialize(winApp_.get(), dxCommon_);

    // モデル・テクスチャ・ライトの共有サービスに同じDirectX基盤を渡す。
    {
        StartupProfileScope phase("ModelManager::Initialize");
        ModelManager::GetInstance()->Initialize(dxCommon_);
    }

    engine::TextureManager::GetInstance()->Initialize(dxCommon_);
    LightManager::GetInstance()->Initialize(dxCommon_);

    // 重いGPU準備はSceneManagerが必要な範囲だけ行う。ここでは所有者とDeviceを結び付ける。
    renderCommonResources_ = std::make_unique<engine::RenderCommonResources>();
    renderCommonResources_->Initialize(dxCommon_);

}

void Framework::Finalize() {
	// DirectXの参照がある場合は、描画資源の解放前にGPUのコマンドキュー完了を待つ。
    if (dxCommon_) {
        dxCommon_->FlushCommandQueue();
    }

    // GPU待機後に音声・ImGui・モデルを停止し、後続の共有描画資源の解放に備える。
    AudioPlayer::GetInstance()->Finalize();
    ImGuiManager::GetInstance()->Finalize();
    ModelManager::GetInstance()->Finalize();

    // SceneとEditorの終了・GPU待機が完了してから、共有PSOをDeviceより先に破棄する。
    renderCommonResources_.reset();
    dxCommon_->Finalize();
    // OS側のCOMを使う描画基盤を解放してから、初期化時のCOM登録を解除する。
    CoUninitialize();
}

void Framework::Run() {
	// WinAppが終了を通知するまで更新の直後に描画する。初期化と終了はWinMainが呼び出す。
    while (winApp_->Update() == false) {
        Update();
        Draw();
    }
}

} // namespace engine
