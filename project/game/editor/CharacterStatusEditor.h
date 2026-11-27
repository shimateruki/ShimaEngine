#pragma once
#include <cstdint>
#include <string>
class BaseScene;

// 基本値の編集・保存と通常Sceneへの適用をゲーム側で担当します。
namespace characterStatusEditor {
void Register();
// 空のtypeはPlayer、"*"は全種類。Editor内部のダミーや複製には適用しません。
std::int32_t Apply(BaseScene* scene, const std::string& type);
}
