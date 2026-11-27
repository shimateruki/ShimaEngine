#pragma once

// 担当：既存JSONの指定キーを更新・削除し、ファイルへ保存する。
// 関連：Editorや設定保存の呼出側、nlohmann::json（JSON表現）
// 主な入口：UpdateObject

#include "json_fwd.hpp"
#include <filesystem>
#include <initializer_list>

namespace jsonFile {
// 更新対象が存在しないか、既存のJSONが構文不正またはオブジェクト以外なら、空オブジェクトから組み立て直す。
// 読み取れたオブジェクトは指定外のキーを保持する。既存ファイルを開けない場合や入出力に失敗した場合はfalseを返す。
// valuesはObject型が必要。removedKeysは値の更新後に削除するので、同名キーは最終的に残らない。
bool UpdateObject(const std::filesystem::path& path, const nlohmann::json& values,
    std::initializer_list<const char*> removedKeys = {});
}
