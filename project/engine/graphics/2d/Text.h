#pragma once
#include <cstdint>
#include "Sprite.h"
#include <string>
#include <vector>
#include <memory>

namespace engine { class SpriteCommon; }

// 担当：フォント画像を文字ごとのSpriteとして並べ、文字列を描画する。
// 関連：Sprite（文字の描画）、SpriteCommon（共有描画基盤）
// 主な入口：Initialize / SetString / Update / Draw
class Text {
public:
    /// <summary>フォント画像を読み込み、文字Spriteを生成できる状態にする。</summary>
    void Initialize(engine::SpriteCommon* common, const std::string& fontTextureName);

    /// <summary>文字列、座標、スケールの変更を各文字Spriteへ反映する。</summary>
    void Update();

    /// <summary>生成済みの文字Spriteを順に描画する。</summary>
    void Draw();

    /// <summary>
    /// 表示する文字列を設定する。フォント画像への割り当ては1バイトにつき1セル。
    /// </summary>
    void SetString(const std::string& str);

    /// <summary>
    /// 先頭文字のSpriteのアンカー位置を画面ピクセルで指定する。
    /// </summary>
    void SetPosition(const engine::Vector2& position);

    /// <summary>
    /// 各文字Spriteに適用する表示倍率を設定する。
    /// </summary>
    void SetScale(const engine::Vector2& scale);


private:
    engine::SpriteCommon* common_ = nullptr; // 文字Spriteが使う共有描画基盤。
    uint32_t fontTextureHandle_ = 0; // 文字を切り出すフォント画像のハンドル。

    // 文字列の各バイトに対応する描画用のSprite。
    std::vector<std::unique_ptr<engine::Sprite>> sprites_;

    std::string text_ = "";
    // 先頭文字のアンカーに使う画面座標。
    engine::Vector2 position_ = engine::Vector2::kZero;
    // 文字の大きさと横の間隔に適用する倍率。
    engine::Vector2 scale_ = { 1.0f, 1.0f };

    // フォントテクスチャの1文字分のサイズ
    std::int32_t fontWidth_ = 16;
    std::int32_t fontHeight_ = 16; // 一文字の元画像の高さ（ピクセル）。

    // フォントテクスチャの横に並んでいる文字数
    std::int32_t fontCharsPerRow_ = 7;
};
