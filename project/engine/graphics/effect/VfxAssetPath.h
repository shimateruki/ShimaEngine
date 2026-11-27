#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace vfxAssetPath {

// 旧アセット名は同じ素材種別内で末尾名が一意な場合だけ解決する。
template <class Map>
auto FindPreset(Map& presets, const std::string& key) {
    const auto exact = presets.find(key);
    // 完全一致があればそのまま返す。フォルダーを明示したキーも別フォルダーへ読み替えない。
    if (exact != presets.end() || key.find_first_of("/\\") != std::string::npos) return exact;
    auto found = presets.end();
    for (auto it = presets.begin(); it != presets.end(); ++it) {
        const std::string_view name(it->first);
        const auto slash = name.find_last_of('/');
        // 旧名との照合では、フォルダー付き登録名の末尾だけを比較し、異なるものを除く。
        if (slash == name.npos || name.substr(slash + 1) != key) continue;
        // 同じ末尾が二つ以上あればどの素材か特定できないため、未発見と同じ結果を返す。
        if (found != presets.end()) return presets.end();
        found = it;
    }
    return found;
}

inline std::filesystem::path Resolve(const std::filesystem::path& root, const std::filesystem::path& requested) {
    namespace fs = std::filesystem;
    const auto relative = requested.lexically_normal().lexically_relative(root.lexically_normal());
    // 補完対象は素材ルート直下のJSON名だけ。そこから外れるパスは指定のまま返す。
    if (relative.empty() || relative.has_parent_path() || relative.extension() != ".json") return requested;
    std::error_code error;
    // 旧形式の位置に実ファイルが残っていれば、移動先の探索より優先する。
    if (fs::is_regular_file(requested, error)) return requested;

    fs::path found;
    for (fs::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        // 同一ファイル名の通常ファイルだけを、移動先の候補に含める。
        if (it->path().filename() != relative || !it->is_regular_file(error)) continue;
        // 同名の候補が複数あるときは、誤った別素材を読み込まないよう元のパスを返します。
        if (!found.empty()) return requested;
        found = it->path();
    }
    // 走査中にエラーが起きたか候補がなかった場合は、指定パスを維持して呼び出し側へ返す。
    return error || found.empty() ? requested : found;
}

// シーンの先読みでも同じ旧パス解決を使う。
inline std::filesystem::path ResolveDependency(const std::filesystem::path& path) {
    const auto root = path.parent_path();
    // Resources/json直下の種別フォルダーにあるパスだけ、先読み時の補完を試す。
    if (root.parent_path() != std::filesystem::path("Resources/json")) return path;
    const auto kind = root.filename();
    // 旧配置の補完に対応するVFX種別に限って、素材ルート以下の候補を検索する。
    if (kind == "gpu_particles" || kind == "particle" || kind == "effect" || kind == "debris" ||
        kind == "vfx_sequence" || kind == "trail_emitter") return Resolve(root, path);
    return path;
}

} // namespace vfxAssetPath
