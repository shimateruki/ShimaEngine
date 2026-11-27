#include "Game.h"
#include "Framework.h"
#include "engine/system/scene/LoadingProfiler.h"
#include <memory>

// Windowsアプリでのエントリーポイント
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {

    // 起動処理を計測できるように、サービスを作る前にプロファイルを開始する。
    BeginStartupProfile();

    // Frameworkクラスのポインタで、Gameクラスのインスタンスを生成
    std::unique_ptr<engine::Framework> game = std::make_unique<Game>();

    // Gameの初期化を通して共通サービス、シーン、描画効果を準備する。
    game->Initialize();

    // ウィンドウの終了通知まで更新と描画を繰り返す。
    game->Run();

    // ループを抜けたらGameからシーンと共通サービスの終了処理を呼ぶ。
    game->Finalize();

    return 0;
}
