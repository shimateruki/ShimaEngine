#pragma once

#include <string>
#include <string_view>

// 敵タイプから設定フォルダーと互換読込用の名前を決める。JSONの読み書きは各設定クラスが担当する。
namespace enemySettingsPaths {

// ファイル名として扱える敵タイプだけを変換する。例: FalseKingSlime → false_king_slime。
inline std::string FolderName(std::string_view enemyType) {
    std::string name;
    for (char character : enemyType) {
        // 大文字は小文字に直し、単語の区切りとして扱う。
        if (character >= 'A' && character <= 'Z') {
            // 出力の先頭では区切り文字を付けない。
            if (!name.empty()) {
                name += '_';
            }
            name += static_cast<char>(character - 'A' + 'a');
        // 小文字の英字と数字はファイル名の構成文字としてそのまま残す。
        } else if ((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9')) {
            name += character;
        // 記号、空白などがあれば安全な設定名へ変換できないため、空文字を返す。
        } else {
            return {};
        }
    }
    return name;
}

inline std::string Directory(std::string_view enemyType) {
    const std::string name = FolderName(enemyType);
    // 名前を作れなかった場合は、設定ルートのパスを組み立てず空のまま返す。
    return name.empty() ? std::string{} : "Resources/json/enemy/" + name + "/";
}

} // namespace enemySettingsPaths
