#include <cstdint>
#include "Text.h"
#include "SpriteCommon.h"
#include "TextureManager.h" // GetMetadata用

// Spriteの描画基盤を借用し、固定幅フォントの画像を読み込みます。
void Text::Initialize(engine::SpriteCommon* common, const std::string& fontTextureName) {
    common_ = common;
    fontTextureHandle_ = engine::Sprite::LoadTexture(fontTextureName);

}

// 文字列をバイト単位でフォント画像のセルへ割り当て、描画するSprite群を更新します。
void Text::SetString(const std::string& str) {
    // 内容が同じ場合はSpriteの割り当てと画像の切り出しをやり直さない。
    if (text_ == str)
        return;
    text_ = str;

    // 文字列の各バイトに対応する枠を確保する。短くなる場合は余ったSpriteを破棄する。
    sprites_.resize(text_.length());

    for (size_t i = 0; i < text_.length(); ++i) {
        // 既存のSpriteは再利用し、文字列が延びて新しい枠が増えたときだけ生成します。
        if (!sprites_[i]) {
            sprites_[i] = std::make_unique<engine::Sprite>();
            sprites_[i]->Initialize(common_, fontTextureHandle_);
        }

        // 1バイトを1文字として、フォント画像上の切り出し位置をピクセル単位で算出します。
        unsigned char charCode = static_cast<unsigned char>(text_[i]);

        // fontCharsPerRow_ はフォント画像1行あたりの文字セル数です。
        // 余りが列、商が行です。各セルのピクセル寸法を掛けて、切り出し矩形の左上を決めます。
        std::int32_t tx = (charCode % fontCharsPerRow_) * fontWidth_;
        std::int32_t ty = (charCode / fontCharsPerRow_) * fontHeight_;

        sprites_[i]->SetTextureRect(
            { (float)tx, (float)ty },             // ピクセル座標で指定する切り出し矩形の左上。
            { (float)fontWidth_, (float)fontHeight_ } // 1文字分のサイズ
        );

        sprites_[i]->SetSize({ (float)fontWidth_, (float)fontHeight_ });
    }

    // 文字数とセルを更新した後、現在の位置と倍率で横並びに配置する。
    Update();
}

// 先頭文字のアンカー位置を変え、現在の倍率で各文字を並べ直す。
void Text::SetPosition(const engine::Vector2& position) {
    position_ = position;
    Update();
}

// 各文字の倍率と文字間隔を更新します。
void Text::SetScale(const engine::Vector2& scale) {
    scale_ = scale;
    Update();
}

// 各文字の座標と大きさから描画用データを更新する。開始位置は先頭文字のアンカー。
void Text::Update() {
    for (size_t i = 0; i < sprites_.size(); ++i) {
        // 有効なSpriteの位置と寸法だけ、現在の文字列全体の設定から計算する。
        if (sprites_[i]) {
            // 先頭のアンカーから、拡大後の一文字幅に文字番号を掛けた分だけXを進める。
            float posX = position_.x + (i * fontWidth_ * scale_.x);
            sprites_[i]->SetPosition({ posX, position_.y });
            sprites_[i]->SetSize({ fontWidth_ * scale_.x, fontHeight_ * scale_.y });

            sprites_[i]->Update();
        }
    }
}

// 生成済みの文字Spriteを文字列の先頭から描く。
void Text::Draw() {
    for (auto& sprite : sprites_) {
        // 生成済みの文字Spriteだけを順番に描く。
        if (sprite) {
            sprite->Draw();
        }
    }
}