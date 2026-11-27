#pragma once

// 担当：Property登録と保存JSONの値を共通の規則で結び付ける。
// 関連：EditorPropertyRegistry（定義）、Object3D（読込・保存）
#include "EditorPropertyRegistry.h"
#include "Object3D.h"

namespace editorPropertyJsonBinding {
// 省略されたparamへ前のPresetの値を持ち越さず、生成時の既定値から読み直します。
inline void Restore(engine::Object3D& object, const nlohmann::json& defaults, const nlohmann::json& source) {
    object.ClearParameters();
    object.ImportFromJson(defaults);
    object.ImportFromJson(source);
}

// 実クラスで変更を適用し、変わった保存値だけをPresetへ反映します。
// Export全体で置き換えないため、未知項目・children・Prefab情報を保持できます。
inline bool Apply(engine::Object3D& object, const EditorPropertyDescriptor& property,
                  nlohmann::json& source, const nlohmann::json& value) {
    const auto before = object.ExportToJson();
    // 登録したSetterが編集を受け付けなければ、保存用JSONを更新しない。
    if (!EditorPropertyRegistry::GetInstance()->SetValue(&object, property.path, value)) return false;
    // 実際の保存値を前後で比べ、Setter が連動して変えた項目も保存差分に含める。
    const auto changes = nlohmann::json::diff(before, object.ExportToJson());
    for (const auto& change : changes) {
        const nlohmann::json::json_pointer path(change.at("path").get<std::string>());
        // 追加または置換の差分では、Setterが確定した値を保存先へ反映する。
        if (change.at("op") != "remove") {
            // 新しい親Objectを追加する差分でも、Preset側にある未知の子キーは保持します。
            const auto& incoming = change.at("value");
            // 追加される値と保存側の値が両方オブジェクトなら、保存側にだけある項目を残してマージする。
            if (change.at("op") == "add" && incoming.is_object() && source.contains(path) && source[path].is_object())
                source[path].update(incoming, true);
            // マージの対象でない差分は、指定されたパスの値を置き換える。
            else
                source[path] = incoming;
        // 削除差分は親が保存データに残っているときだけ適用し、辞書のキーと配列の添字を区別します。
        } else if (source.contains(path.parent_pointer())) {
            auto& parent = source[path.parent_pointer()];
            // 親がオブジェクトなら、差分が指す名前でキーを削除する。
            if (parent.is_object()) parent.erase(path.back());
            // 親が配列なら末尾のパス成分を添字へ変換する。
            else if (parent.is_array()) {
                const auto index = static_cast<std::size_t>(std::stoull(path.back()));
                // その添字が保存配列の範囲内に残っている場合だけ削除する。
                if (index < parent.size()) parent.erase(index);
            }
        }
    }
    return !changes.empty();
}
} // namespace editorPropertyJsonBinding
