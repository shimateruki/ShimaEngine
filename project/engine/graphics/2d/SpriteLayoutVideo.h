#pragma once

#include "Sprite.h"
#include "TextureManager.h"
#include "VideoSpritePlayer.h"

#include <filesystem>

// レイアウト中の動画を既存の再生器につなぎ、映像の準備中は同じSpriteの静止画を使います。
namespace spriteLayoutVideo {

// Resources/video内の実在するMP4を正規化して返す。入力が無効なら空パス。
inline std::filesystem::path Resolve(const std::string& value) {
    // 動画の入力が空なら、静止画表示のまま使う。
    if (value.empty())
        return {};
    const std::filesystem::path input(std::u8string(value.begin(), value.end()));
    // ルートを含むパスや拡張子が.mp4でない指定は、動画の対象にしない。
    if (input.has_root_path() || input.extension() != L".mp4")
        return {};
    std::error_code error;
    const auto root = std::filesystem::canonical("Resources/video", error);
    // 動画フォルダーを解決できない場合は、動画ファイルを開かない。
    if (error)
        return {};
    const auto file = std::filesystem::canonical(root / input, error);
    // 指定ファイルを解決でき、通常ファイルである場合だけ進める。
    if (error || !std::filesystem::is_regular_file(file, error))
        return {};
    // 正規化後も動画フォルダー内にあることを確認する。範囲外に解決された場合は空を返す。
    const auto relative = file.lexically_relative(root);
    return relative.empty() || relative.is_absolute() || *relative.begin() == L".." ? std::filesystem::path{} : file;
}

// 画像の切替で位置とサイズは維持し、UVだけを転送先の画像全体へ合わせます。
inline void SetTexture(engine::Sprite& sprite, uint32_t handle) {
    // 有効な画像ハンドルがまだ得られていなければ、現在の画像を保持する。
    if (handle == 0)
        return;
    const auto& texture = engine::TextureManager::GetInstance()->GetMetadata(handle);
    sprite.SetTextureHandle(handle);
    sprite.SetTextureRect({0, 0}, {static_cast<float>(texture.width), static_cast<float>(texture.height)});
}

} // namespace spriteLayoutVideo
