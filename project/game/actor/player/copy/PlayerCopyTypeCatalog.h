#pragma once

#include "engine/utility/math/Math.h"

#include <array>
#include <string_view>

// コピー能力として保存・再適用できるスライム種の共通定義です。
// プレイヤー、コピー記憶台、エディターで同じ識別子と見た目を共有します。
struct PlayerCopyTypeDescriptor {
    std::string_view enemyType;
    std::string_view modelName;
    engine::Vector4 displayColor;
};

namespace playerCopyTypeCatalog {
// 記憶台や編集UIで一覧にする通常5種を返す。
const std::array<PlayerCopyTypeDescriptor, 5>& GetAll();
// 王冠を含む種別名から定義を検索し、対象外ならnullptrを返す。
const PlayerCopyTypeDescriptor* Find(std::string_view enemyType);
// 指定名がコピー能力の定義に含まれるか返す。
bool IsSupported(std::string_view enemyType);

// 記憶台とドロップで、能力を示す同じ星モデルを使います。
std::string_view GetEssenceModel(std::string_view enemyType);
} // namespace playerCopyTypeCatalog
