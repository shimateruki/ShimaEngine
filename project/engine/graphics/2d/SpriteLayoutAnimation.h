#pragma once
#include <cstdint>

#include "Sprite.h"
#include "TextureManager.h"
#include "game/ui/SpriteLayoutScaler.h"

// スプライトレイアウトのコマ設定を、Editorプレビューとゲームで同じ時刻から再生します。
namespace spriteLayoutAnimation {

/// <summary>レイアウトの横一列のコマ画像を再生するため、コマ数・毎秒のコマ数・ループ設定を保持する。</summary>
struct Settings {
    std::int32_t frameCount = 1; // 横一列に並んだコマ数。
    // 1秒あたりのコマ数。
    float fps = 12.0f;
    // 末尾から先頭に戻るか。falseでは最終コマを表示し続ける。
    bool loop = true;
};

// 画像ごとのアニメーション設定を読む。フレーム数は1〜128、速度は1〜60fpsに収める。
inline Settings Read(const nlohmann::json& row) {
    const auto animation = row.find("animation");
    // animationがなければ、またはオブジェクト型でなければ、静止画の既定設定を返す。
    if (animation == row.end() || !animation->is_object())
        return {};
    return {
        std::clamp(spriteLayoutScaler::ReadInt(*animation, "frameCount", 1), 1, 128),
        std::clamp(spriteLayoutScaler::ReadFloat(*animation, "fps", 12.0f), 1.0f, 60.0f),
        spriteLayoutScaler::ReadBool(*animation, "loop", true)
    };
}

// 経過秒は再生開始を0とし、周回または最終コマへ収めてUVだけを設定します。
inline void Apply(engine::Sprite& sprite, const Settings& settings, double seconds) {
    const auto handle = sprite.GetTextureHandle();
    // 1コマ以下の画像や未設定のテクスチャには、切り出し範囲を設定しない。
    if (settings.frameCount <= 1 || handle == 0)
        return;
    const auto& texture = engine::TextureManager::GetInstance()->GetMetadata(handle);
    // コマ数分の幅を確保でき、高さが正で、横幅をコマ数で等分できる場合だけ切り出す。
    if (texture.width < static_cast<size_t>(settings.frameCount) || texture.height == 0 ||
        texture.width % static_cast<size_t>(settings.frameCount) != 0)
        return;

    // 経過秒を整数のコマ番号へ換算し、ループなら剰余、非ループなら最終コマで上限を決める。
    const double index = std::floor((std::max)(0.0, seconds) * settings.fps);
    const double frame = settings.loop ? std::fmod(index, settings.frameCount)
                                       : (std::min)(index, static_cast<double>(settings.frameCount - 1));
    const float width = static_cast<float>(texture.width / static_cast<size_t>(settings.frameCount));
    sprite.SetTextureRect({static_cast<float>(frame) * width, 0.0f}, {width, static_cast<float>(texture.height)});
}

} // namespace spriteLayoutAnimation
