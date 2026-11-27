#pragma once

#include <algorithm>
#include <filesystem>
#include <string>

namespace engine {

// 実ロードと配布選別で同じ探索順を使います。rootはResources/3DModelに相当します。
// 未検出時にもdirectoryを返し、従来のLoadModelのフォールバック先を維持します。
inline bool ResolveModelAssetPath(const std::filesystem::path& root, const std::string& modelName,
                                  std::filesystem::path& directory, std::filesystem::path& file) {
    namespace fs = std::filesystem;
    std::string normalized = modelName;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    const fs::path path = fs::path(std::u8string(normalized.begin(), normalized.end()));
    const fs::path parent = path.parent_path();
    const fs::path stem = path.stem();
    directory = parent.empty() ? root / stem : root / parent / stem;
    file.clear();

    // 拡張子の明示指定では、名前と同じフォルダー、親直下の順を守ります。
    if (path.has_extension()) {
        file = path.filename();
        // 明示したファイルがモデルと同名のフォルダーにあれば、親直下を探さず採用する。
        if (fs::is_regular_file(directory / file)) return true;
        // 専用フォルダーになく親フォルダー直下にあれば、参照元のディレクトリも親へ切り替える。
        if (fs::is_regular_file(root / parent / file)) {
            directory = root / parent;
            return true;
        }
        return false;
    }

    // 同名の複数形式がある場合も、ゲームが優先するOBJ→glTF→GLBだけを選びます。
    for (const auto* extension : {".obj", ".gltf", ".glb"}) {
        fs::path candidate = stem;
        candidate += extension;
        // 優先順で最初に見つかった同名の実ファイルを採用する。
        if (fs::is_regular_file(directory / candidate)) {
            file = candidate;
            return true;
        }
    }
    // 同名の対応ファイルが見つからなくても、専用フォルダーを開ける場合は別名の候補を探す。
    if (fs::is_directory(directory)) {
        for (const auto& entry : fs::directory_iterator(directory)) {
            const auto extension = entry.path().extension();
            // 通常ファイルかつ読み込み対象の拡張子であれば、列挙で最初に見つかった名前を採用する。
            if (entry.is_regular_file() && (extension == ".obj" || extension == ".gltf" || extension == ".glb")) {
                file = entry.path().filename();
                break;
            }
        }
    }
    return !file.empty() && fs::is_regular_file(directory / file);
}

} // namespace engine
