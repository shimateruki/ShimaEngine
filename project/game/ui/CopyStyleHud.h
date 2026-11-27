#pragma once

#include "Player.h"
#include "Easing.h"
#include "Sprite.h"
#include "TextureManager.h"

#include <algorithm>
#include <array>
#include <cmath>

// 現在のコピー能力名を、チュートリアル・本編・プレビューで同じ条件で表示する。
// 位置と大きさは各シーンが所有する Sprite の配置設定から受け取る。
// 関連：各シーンのSpriteを借用し、Playerの現在形態に対応する表示へ切り替える。
// 主な入口：Bind / Update
class CopyStyleHud {
  public:
    // コピー能力ごとの表示画像。事前ロードは操作ガイドのUIデータから収集する。
    static constexpr std::array<const char*, 6> kTexturePaths = {
        "ui/hud/copy_name/pink.png", "ui/hud/copy_name/fire.png", "ui/hud/copy_name/thunder.png",
        "ui/hud/copy_name/wind.png", "ui/hud/copy_name/bomb.png", "ui/hud/copy_name/crown.png"};

    // 表示先Spriteを借用し、現在配置を表示の基準値として退避する。生成・所有は呼出側が担当する。
    void Bind(engine::Sprite* sprite) {
        // シーン所有のSpriteから基準座標と寸法だけ保持し、能力表示は一度隠す。
        sprite_ = sprite;
        previousType_ = Player::EnemyMorphType::kNone;
        revealTime_ = 0.0f;
        // 表示先を解除する呼び出しでは、使用するSpriteがないため基準位置を取得しない。
        if (!sprite_)
            return;
        basePosition_ = sprite_->GetPosition();
        baseSize_ = sprite_->GetSize();
        // 画面の縦横比が変わっても文字を潰さず、Editorで指定した範囲へ収める。
        const auto& metadata = engine::TextureManager::GetInstance()->GetMetadata(sprite_->GetTextureHandle());
        // 画像の幅と高さを取得できた場合は、配置の矩形内に縦横比を保って収める。
        if (metadata.width > 0 && metadata.height > 0) {
            const float width = static_cast<float>(metadata.width);
            const float height = static_cast<float>(metadata.height);
            const float fit = (std::min)(baseSize_.x / width, baseSize_.y / height);
            baseSize_ = {width * fit, height * fit};
        }

        sprite_->SetVisible(false);
    }

    // 現在のPlayer形態に対応する画像・表示状態へ切り替える。Playerは所有しない。
    void Update(const Player* player, float deltaTime) {
        // 表示先がまだ接続されていない場合は、表示と再生状態を更新しない。
        if (!sprite_)
            return;

        const Player::EnemyMorphType type = player && player->IsEnemyMorphed()
                                                ? player->GetEnemyMorphType()
                                                : Player::EnemyMorphType::kNone;
        const char* texturePath = ResolveTexture(type);
        // 変身していないか現在の能力に対応する画像がなければ、表示を隠して次回の切替に備える。
        if (!texturePath) {
            previousType_ = Player::EnemyMorphType::kNone;
            revealTime_ = 0.0f;
            sprite_->SetVisible(false);
            return;
        }

        // 能力が変わったフレームだけ画像を差し替え、フェードを最初から始める。
        if (type != previousType_) {
            previousType_ = type;
            revealTime_ = 0.0f;
            const uint32_t handle = engine::Sprite::LoadTexture(texturePath);
            sprite_->SetTextureHandle(handle);
            const auto& metadata = engine::TextureManager::GetInstance()->GetMetadata(handle);
            sprite_->SetTextureRect({0.0f, 0.0f},
                                    {static_cast<float>(metadata.width), static_cast<float>(metadata.height)});
        }

        // シーンが設定した基準位置と寸法を保ち、能力の切替から0.16秒で不透明にする。
        revealTime_ += (std::max)(deltaTime, 0.0f);
        const float t = std::clamp(revealTime_ / 0.16f, 0.0f, 1.0f);
        const float smooth = Easing::SmoothStep(t);
        sprite_->SetVisible(true);
        sprite_->SetPosition(basePosition_);
        sprite_->SetSize(baseSize_);
        sprite_->SetColor({1.0f, 1.0f, 1.0f, smooth});
        sprite_->Update();
    }

  private:
    // 現在の変身形態から表示用の画像パスを選ぶ。表示しない形態はnullptr。
    static const char* ResolveTexture(Player::EnemyMorphType type) {
        // コピー能力なし、または未対応の能力では案内画像を表示しない。
        switch (type) {
        // ピンクスライムの能力名画像。
        case Player::EnemyMorphType::kSlime: return kTexturePaths[0];
        // 炎スライムの能力名画像。
        case Player::EnemyMorphType::kFireSlime: return kTexturePaths[1];
        // 雷スライムの能力名画像。
        case Player::EnemyMorphType::kThunderSlime: return kTexturePaths[2];
        // 風スライムの能力名画像。
        case Player::EnemyMorphType::kWindSlime: return kTexturePaths[3];
        // ボムスライムの能力名画像。
        case Player::EnemyMorphType::kBomber: return kTexturePaths[4];
        // 王冠の能力名画像。
        case Player::EnemyMorphType::kCrown: return kTexturePaths[5];
        // 変身なしと未登録の形態は表示画像なし。
        default: return nullptr;
        }
    }

    engine::Sprite* sprite_ = nullptr; // 所有権は各シーンが持つ。
    engine::Vector2 basePosition_ = engine::Vector2::kZero;
    engine::Vector2 baseSize_ = engine::Vector2::kZero;
    Player::EnemyMorphType previousType_ = Player::EnemyMorphType::kNone;
    // 現在の能力名へ切り替わってからの経過秒数。
    float revealTime_ = 0.0f;
};
