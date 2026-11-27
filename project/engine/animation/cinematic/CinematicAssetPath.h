#pragma once

#include <filesystem>

namespace cinematicAssetPath {

// 実在する指定先を優先し、旧形式のルート直下の名前だけをシーン別フォルダーから探す。
inline std::filesystem::path Resolve(const std::filesystem::path& root, const std::filesystem::path& requested) {
    namespace fs = std::filesystem;
    std::error_code error;
    // 指定先が実ファイルなら、ほかのフォルダーに同名の素材があっても指定先を使う。
    if (fs::is_regular_file(requested, error)) return requested;
    const auto relative = requested.lexically_normal().lexically_relative(root.lexically_normal());
    // シーン別検索へ進むのは素材ルート直下のJSON名だけです。下位フォルダーを明示した指定は、そのパスのまま扱います。
    if (relative.empty() || relative.has_parent_path() || relative.extension() != ".json") return requested;

    // 同名の候補が複数あれば誤った演出を再生しない。
    fs::path found;
    for (const auto* folder : {"stage1", "stage2", "stage3", "stage_select", "tutorial", "shared", "samples"}) {
        const auto candidate = root / folder / relative;
        // このシーンフォルダーに実ファイルがなければ、残りの候補を探す。
        if (!fs::is_regular_file(candidate, error)) continue;
        // 二つ目が見つかった場合はどちらかを選ばず、指定パスを返して呼び出し元に判断を任せる。
        if (!found.empty()) return requested;
        found = candidate;
    }
    // 移動先が一意に見つかった場合だけパスを読み替える。
    return found.empty() ? requested : found;
}

} // namespace cinematicAssetPath
