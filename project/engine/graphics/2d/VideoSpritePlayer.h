#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

struct ID3D12GraphicsCommandList;

// 案内看板と操作ガイドなどの動画をデコードし、GPU転送に使う再生時刻とフレームを管理する。転送先はTextureManagerが共有する。
// 主な入口：RequestOpen / Update / PrepareTextureForDraw / Close
class VideoSpritePlayer {
public:
    // デコードとGPU転送の出力解像度。単位はピクセル。
    static constexpr uint32_t kWidth = 640;
    static constexpr uint32_t kHeight = 360;

    // 再生状態を確保する。デコードスレッドは開く要求で起動する。
    VideoSpritePlayer();
    // 共有テクスチャを返却し、ワーカーの終了を待ってから再生状態を破棄する。
    ~VideoSpritePlayer();

    // ローカルの動画を先頭から非同期で読み込む。
    void RequestOpen(const std::filesystem::path& path);
    // 表示を止め、処理中の古い読み込み結果を無効にする。
    void Close();
    // 呼び出し元からの経過秒数で再生時刻を進め、現在の表示時刻に対応する画像を選ぶ。
    void Update(double deltaSeconds);
    // 描画コマンド記録中に呼び、転送済みの画像を参照するSRV番号を返す。準備中や失敗時は空。
    std::optional<uint32_t> PrepareTextureForDraw(ID3D12GraphicsCommandList& commands);
    // 現在のデコード要求が失敗した場合にtrueを返す。
    bool HasError() const;

private:
    struct Playback;
    std::unique_ptr<Playback> playback_;
};
