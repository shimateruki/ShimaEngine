#pragma once
#include <cstdint>

// 担当：VFX編集画面で使うAsset候補の検索・選択UIを共通化する。
// 関連：GpuParticleEditor / MeshEffectEditorなど（利用）

#ifdef USE_IMGUI

#include <imgui.h>
#include <algorithm>
#include <string>
#include <vector>

namespace vfxAssetPicker {

// ファイル名とフォルダー名の両方を検索し、候補が多くても必要な行だけ描画します。
inline bool Matches(const std::string& value, const char* query) {
    // 検索文字列を指定しないか空欄の場合は、すべての候補が一致する。
    if (!query || !*query)
        return true;
    std::string lowerValue = value;
    std::string lowerQuery = query;

    // UTF-8 の日本語バイト列は触らず、英字だけを大文字小文字を区別せず比較します。
    const auto toLowerAscii = [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    };
    std::transform(lowerValue.begin(), lowerValue.end(), lowerValue.begin(), toLowerAscii);
    std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), toLowerAscii);
    return lowerValue.find(lowerQuery) != std::string::npos;
}

// selectedIndex は元の entries の添字で、未選択は -1。変更した場合だけ true を返す。
inline bool DrawCombo(const char* label, const std::vector<std::string>& entries,
                      std::int32_t& selectedIndex, char* query, size_t queryCapacity,
                      const char* emptyLabel = "未選択", bool allowNone = false) {
    const bool validSelection = selectedIndex >= 0 && selectedIndex < static_cast<std::int32_t>(entries.size());
    const char* preview = validSelection ? entries[selectedIndex].c_str() : emptyLabel;
    bool changed = false;
    // 一覧を開いていない場合は選択を変えず、現在の長いパスを確認できるだけにする。
    if (!ImGui::BeginCombo(label, preview, ImGuiComboFlags_HeightLargest)) {
        // 現在の選択が一覧内にあり、閉じた欄へカーソルを重ねた場合だけツールチップに全パスを出す。
        if (validSelection && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", preview);
        return false;
    }

    // ポップアップを開いたフレームに限り、先頭の検索欄へキーボードフォーカスを移す。
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##AssetSearch", "名前・フォルダーで検索", query, queryCapacity);
    // 検索は元の一覧を並べ替えず、表示する添字だけ集める。絞り込み中に選んでも同じ Asset を指せる。
    std::vector<std::int32_t> visibleIndices;
    visibleIndices.reserve(entries.size());
    for (std::int32_t i = 0; i < static_cast<std::int32_t>(entries.size()); ++i) {
        // 検索語が名前またはフォルダーに含まれる行の元番号だけを集める。
        if (Matches(entries[i], query))
            visibleIndices.push_back(i);
    }
    ImGui::TextDisabled("%zu / %zu 件", visibleIndices.size(), entries.size());
    ImGui::BeginChild("##AssetResults", ImVec2(0.0f, 220.0f), false);
    // 未選択を許可する欄で空の選択肢を押した場合は、元の番号と検索を消して閉じる。
    if (allowNone && ImGui::Selectable(emptyLabel, selectedIndex == -1)) {
        selectedIndex = -1;
        query[0] = '\0';
        changed = true;
        ImGui::CloseCurrentPopup();
    }
    // 絞り込みで候補がなくなった場合は、一覧内に一致なしと表示する。
    if (visibleIndices.empty())
        ImGui::TextDisabled("一致する項目はありません");
    // 候補の画面外の行は生成しない。ファイル名表示とホバー用のフルパスは、実際に見える行だけ計算する。
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<std::int32_t>(visibleIndices.size()));
    while (clipper.Step()) {
        for (std::int32_t row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const std::int32_t index = visibleIndices[row];
            const std::string& fullName = entries[index];
            const size_t slash = fullName.find_last_of("/\\");
            const std::string displayName = slash == std::string::npos
                ? fullName : fullName.substr(slash + 1) + "  —  " + fullName.substr(0, slash);
            ImGui::PushID(index);
            // 表示中の行を選んだら、検索結果内の行番号ではなく元一覧の番号を書き戻す。
            if (ImGui::Selectable(displayName.c_str(), selectedIndex == index)) {
                selectedIndex = index;
                query[0] = '\0';
                changed = true;
                ImGui::CloseCurrentPopup();
            }
            // 省略されたフォルダーやファイル名を確認できるよう、行へのホバーでは元のパスを表示する。
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", fullName.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::EndCombo();
    return changed;
}

} // namespace vfxAssetPicker

#endif
