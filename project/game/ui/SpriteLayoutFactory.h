#pragma once

// 各画面が配置JSONからSpriteを生成し、名前と基本姿勢を設定する共通入口。表示条件や入力処理は画面側が担当する。

#include "Sprite.h"
#include "SpriteCommon.h"
#include "game/ui/SpriteLayoutScaler.h"

#include <memory>

namespace spriteLayoutFactory {

// spriteDataは一枚分の設定オブジェクト。切り抜きや表示条件は呼び出し側で設定し、最後にUpdateする。
// 生成したSpriteの所有権を返す。初期状態は非表示で、表示とUpdateは呼び出し側で確定する。
inline std::unique_ptr<engine::Sprite> Create(engine::SpriteCommon* spriteCommon, const nlohmann::json& spriteData,
    const spriteLayoutScaler::LayoutScale& layoutScale, const char* fallbackName) {
    // 名前と画像を決める。画像名が空の場合も白画像を初期テクスチャとして使う。
    const std::string name = spriteLayoutScaler::ReadString(spriteData, "name", fallbackName);
    const std::string texture = spriteLayoutScaler::ReadString(spriteData, "texture", "common/white.png");
    const uint32_t textureHandle = engine::Sprite::LoadTexture(texture.empty() ? "common/white.png" : texture);

    auto sprite = std::make_unique<engine::Sprite>();
    sprite->Initialize(spriteCommon, textureHandle);
    sprite->SetName(name);
    sprite->SetTextureName(texture);
    // 位置と大きさへ設計解像度の倍率を適用し、アンカーと色はレイアウトに記録された値を使う。
    sprite->SetPosition(spriteLayoutScaler::ScalePosition(
        spriteLayoutScaler::ReadVector2(spriteData.value("position", nlohmann::json::array()), sprite->GetPosition()),
        layoutScale));
    sprite->SetSize(spriteLayoutScaler::ScaleSize(
        spriteLayoutScaler::ReadVector2(spriteData.value("size", nlohmann::json::array()), sprite->GetSize()), layoutScale));
    sprite->SetAnchorPoint(
        spriteLayoutScaler::ReadVector2(spriteData.value("anchor", nlohmann::json::array()), sprite->GetAnchorPoint()));
    sprite->SetColor(spriteLayoutScaler::ReadColor(spriteData.value("color", nlohmann::json::array()), sprite->GetColor()));
    sprite->SetEmissive(spriteLayoutScaler::ReadFloat(spriteData, "emissive", 1.0f));
    sprite->SetVisible(false);
    return sprite;
}

} // namespace spriteLayoutFactory
