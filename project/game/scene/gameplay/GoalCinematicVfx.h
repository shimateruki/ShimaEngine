#pragma once

#include <array>
#include <string_view>

namespace goalCinematicVfx {

// トラック補完と再生開始で共有する素材名・対象。
struct Cue {
    std::string_view prefix;
    const char* label;
    const char* gold;
    const char* silver;
    bool followsCrown;
};

inline constexpr Cue kFocus{"crown_focus", "Crown Focus VFX", "shared/crown_focus_cue", "shared/crown_focus_silver_cue", true};
inline constexpr Cue kLanding{"crown_get", "Crown Landing VFX", "shared/crown_get_clear_cue", "shared/crown_get_clear_silver_cue", true};
inline constexpr Cue kResult{"crown_result", "Result Burst VFX", "shared/crown_result_slime_cue", "shared/crown_result_slime_cue", false};
inline constexpr Cue kVictory{"crown_victory_land", "Victory Landing VFX", "shared/crown_victory_land_slime_cue",
                              "shared/crown_victory_land_slime_cue", false};
inline constexpr std::array kCues{&kFocus, &kLanding, &kResult, &kVictory};

inline const Cue* FindCue(std::string_view sequenceName) {
    // 保存済みトラックの共有フォルダー名を除き、素材名で演出の役割を判定する。
    const auto slash = sequenceName.find_last_of("/\\");
    // 実際のファイル名だけを比較するため、区切りがあればその直後からの名前を使う。
    if (slash != std::string_view::npos) sequenceName.remove_prefix(slash + 1);
    for (const auto* cue : kCues) {
        // 共有素材名がこの演出の接頭辞と一致したら、その役割と発生先を返す。
        if (sequenceName.starts_with(cue->prefix)) {
            return cue;
        }
    }
    return nullptr;
}

} // namespace goalCinematicVfx
