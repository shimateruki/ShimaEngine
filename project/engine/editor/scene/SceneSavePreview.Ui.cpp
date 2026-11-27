#define NOMINMAX
#include <cstdint>
#include "SceneSavePreview.h"

#ifdef USE_IMGUI
#include "IconsFontAwesome5.h"
#include "imgui.h"
#endif

#include <algorithm>
#include <iomanip>
#include <sstream>

using json = nlohmann::json;

// 保存前の差分を確認し、保存・取消の判断を受け付けます。
SceneSavePreview::Action SceneSavePreview::Draw() {
#ifdef USE_IMGUI
    // 保存候補の差分を確認してから、保存・取消の選択だけ呼び出し側へ返します。
    if (!isOpen_)
        return Action::kNone;

    const char* popupName = "Scene Save Preview###SceneSavePreview";

    // 開く要求が保留中の場合だけモーダルを開き、同じ要求を次のフレームへ持ち越さない。
    if (requestOpenPopup_) {
        ImGui::OpenPopup(popupName);
        requestOpenPopup_ = false;
    }

    ImGui::SetNextWindowSize(ImVec2(980.0f, 680.0f), ImGuiCond_Appearing);
    bool keepOpen = true;
    Action action = Action::kNone;

    // モーダルを描けるフレームに限り、差分と決定ボタンを出す。
    if (ImGui::BeginPopupModal(popupName, &keepOpen, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(ImVec4(0.35f, 0.8f, 1.0f, 1.0f), ICON_FA_SAVE " %s", title_.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("保存前にJSON差分を確認します");
        ImGui::Separator();

        DrawSummaryCards();
        ImGui::Spacing();
        DrawFileSummaryTable();

        ImGui::Separator();
        ImGui::Checkbox("変更なしのオブジェクトも表示", &showUnchanged_);
        ImGui::SameLine();

        // ファイルやオブジェクトの増減・変更が無い場合は、その旨を表示する。
        if (!summary_.HasChanges()) {
            ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.6f, 1.0f), ICON_FA_CHECK_CIRCLE " 差分はありません");
        }
        // 差分がある場合は、詳細行を開くよう案内する。
        else {
            ImGui::TextDisabled("詳細を開くと、変更されたプロパティを確認できます");
        }

        // 差分一覧の子領域を描ける場合は、ファイルごとの内容を並べる。
        if (ImGui::BeginChild("SceneSavePreviewDiffList", ImVec2(0, -52.0f), true)) {
            for (auto& diff : fileDiffs_) {
                DrawFileDiff(diff);
            }
        }

        ImGui::EndChild();

        ImGui::Separator();

        // キャンセルが押された場合は、保存せず閉じる結果を返す。
        if (ImGui::Button(ICON_FA_TIMES " キャンセル", ImVec2(160.0f, 34.0f))) {
            action = Action::kCancel;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        float buttonSpacer = std::max(0.0f, ImGui::GetContentRegionAvail().x - 330.0f);
        ImGui::Dummy(ImVec2(buttonSpacer, 0.0f));
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.45f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.60f, 0.32f, 1.0f));

        // ここでは確定を返すだけ。ファイルの書き込みには事前に組み立てた保存候補を呼び出し側が使う。
        if (ImGui::Button(ICON_FA_CHECK " この内容で保存", ImVec2(170.0f, 34.0f))) {
            action = Action::kConfirm;
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleColor(2);

        ImGui::EndPopup();
    }

    if (!keepOpen && action == Action::kNone) {
        // タイトルバーの閉じる操作も明示的なキャンセルと同じ結果にする。
        action = Action::kCancel;
    }

    return action;
#else
    return Action::kNone;
#endif
}

#ifdef USE_IMGUI
void SceneSavePreview::DrawSummaryCards() const {
    // ファイル数とObject差分を先に示し、詳細を見る前に変更量を判断できます。
    if (ImGui::BeginTable("SceneSavePreviewSummaryCards", 4, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("Files");
        ImGui::Text("%d", summary_.fileCount);
        ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.0f, 1.0f), "+%d new / %d changed", summary_.addedFiles, summary_.modifiedFiles);

        ImGui::TableSetColumnIndex(1);
        ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.55f, 1.0f), ICON_FA_PLUS_CIRCLE " Added");
        ImGui::Text("%d objects", summary_.addedObjects);

        ImGui::TableSetColumnIndex(2);
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), ICON_FA_MINUS_CIRCLE " Removed");
        ImGui::Text("%d objects", summary_.removedObjects);

        ImGui::TableSetColumnIndex(3);
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.25f, 1.0f), ICON_FA_EDIT " Modified");
        ImGui::Text("%d objects", summary_.modifiedObjects);

        ImGui::EndTable();
    }
}

void SceneSavePreview::DrawFileSummaryTable() const {
    // 保存対象ごとの増減を一覧化し、意図しないファイルの更新を見つけやすくします。
    if (!ImGui::BeginTable("SceneSavePreviewFiles", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        return;
    }

    ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("+", ImGuiTableColumnFlags_WidthFixed, 42.0f);
    ImGui::TableSetupColumn("-", ImGuiTableColumnFlags_WidthFixed, 42.0f);
    ImGui::TableSetupColumn("~", ImGuiTableColumnFlags_WidthFixed, 42.0f);
    ImGui::TableSetupColumn("=", ImGuiTableColumnFlags_WidthFixed, 42.0f);
    ImGui::TableHeadersRow();

    for (const auto& diff : fileDiffs_) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(diff.target.path.c_str());

        ImGui::TableSetColumnIndex(1);

        // 保存前に存在しない場合は追加、読めない場合は無効を優先し、通常差分と区別する。
        if (!diff.oldFileExists) {
            DrawChangeBadge(ChangeKind::kFileAdded);
        }
        // 置き換え前のファイルがあってもJSONが無効なら、読込不能の状態を表示する。
        else if (!diff.oldJsonValid) {
            DrawChangeBadge(ChangeKind::kFileInvalid);
        }
        // 有効な旧ファイルに対してオブジェクトの追加・削除・変更があれば、変更済みとして表示する。
        else if (diff.addedCount > 0 || diff.removedCount > 0 || diff.modifiedCount > 0) {
            DrawChangeBadge(ChangeKind::kFileModified);
        }
        // 以上のどれにも当てはまらなければ、変更なしを表示する。
        else {
            DrawChangeBadge(ChangeKind::kFileUnchanged);
        }

        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%d", diff.addedCount);
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%d", diff.removedCount);
        ImGui::TableSetColumnIndex(4);
        ImGui::Text("%d", diff.modifiedCount);
        ImGui::TableSetColumnIndex(5);
        ImGui::Text("%d", diff.unchangedCount);
    }

    ImGui::EndTable();
}

void SceneSavePreview::DrawFileDiff(FileDiff& diff) {
    // 変更Objectを優先表示し、必要な場合だけ未変更行も展開します。
    std::int32_t visibleChanges = 0;

    for (const auto& change : diff.objects) {
        // 未変更を含める設定か変更行の場合だけ、一覧に現れる件数に加える。
        if (showUnchanged_ || change.kind != ChangeKind::kUnchanged) {
            visibleChanges++;
        }
    }

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;

    // 差分や追加ファイル、読み込めない既存ファイルがある場合は、詳細を初期状態で開く。
    if (diff.addedCount > 0 || diff.removedCount > 0 || diff.modifiedCount > 0 || !diff.oldFileExists || !diff.oldJsonValid) {
        flags |= ImGuiTreeNodeFlags_DefaultOpen;
    }

    std::string header = diff.target.label + " - " + diff.target.path;
    header += "  (+";
    header += std::to_string(diff.addedCount);
    header += " / -";
    header += std::to_string(diff.removedCount);
    header += " / ~";
    header += std::to_string(diff.modifiedCount);
    header += ")";

    // ファイルの見出しが閉じていれば、内容とオブジェクト行を描画しない。
    if (!ImGui::CollapsingHeader(header.c_str(), flags)) {
        return;
    }

    for (const std::string& detail : diff.fileDetails) {
        ImGui::TextWrapped("%s", detail.c_str());
    }

    // オブジェクト配列のないファイルは、ヘッダー直下に出したファイル全体の差分で表示を終える。
    if (!diff.hasObjectList) {
        return;
    }

    // フィルターを通った差分が無ければ、状態だけを示して空の表は作らない。
    if (visibleChanges == 0) {
        ImGui::TextDisabled("表示対象の変更はありません。");
        return;
    }

    // 表を開始できない場合は、オブジェクトの行を出さない。
    if (!ImGui::BeginTable(("SceneSavePreviewObjects_" + diff.target.path).c_str(), 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
        return;
    }

    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 92.0f);
    ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 82.0f);
    ImGui::TableSetupColumn("Object", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    std::int32_t rowId = 0;

    for (const auto& change : diff.objects) {
        // 未変更を隠す設定なら、未変更の行を飛ばす。
        if (!showUnchanged_ && change.kind == ChangeKind::kUnchanged)
            continue;

        ImGui::PushID(rowId++);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        DrawChangeBadge(change.kind);

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(change.category.c_str());

        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(change.name.c_str());

        ImGui::TableSetColumnIndex(3);

        // 項目別の説明が空なら、詳細欄には空欄の目印だけを置く。
        if (change.details.empty()) {
            ImGui::TextDisabled("-");
        }
        // 説明があれば、最初の一行を表に出す。
        else {
            // 差分の一行目は常時表示し、残りはセルを展開した場合だけ描く。
            ImGui::TextWrapped("%s", change.details[0].c_str());

            // 補足が二行目以降にもある場合にだけ、展開できる詳細欄を置く。
            if (change.details.size() > 1) {
                // 詳細欄が開いているフレームだけ、二行目以降を箇条書きで描く。
                if (ImGui::TreeNode("詳細")) {
                    for (size_t i = 1; i < change.details.size(); ++i) {
                        ImGui::BulletText("%s", change.details[i].c_str());
                    }

                    ImGui::TreePop();
                }
            }
        }

        ImGui::PopID();
    }

    ImGui::EndTable();
}

void SceneSavePreview::DrawChangeBadge(ChangeKind kind) const {
    // 追加・削除・変更を同じ色規則で表し、各差分表の読み方を揃えます。
    ImVec4 color = ImVec4(0.75f, 0.75f, 0.75f, 1.0f);

    switch (kind) {
    case ChangeKind::kAdded:
    case ChangeKind::kFileAdded:
        color = ImVec4(0.35f, 1.0f, 0.55f, 1.0f);
        break;

    case ChangeKind::kRemoved:
        color = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
        break;

    case ChangeKind::kModified:
    case ChangeKind::kFileModified:
        color = ImVec4(1.0f, 0.78f, 0.22f, 1.0f);
        break;

    case ChangeKind::kFileInvalid:
        color = ImVec4(1.0f, 0.25f, 0.75f, 1.0f);
        break;

    case ChangeKind::kUnchanged:
    case ChangeKind::kFileUnchanged:
    default:
        color = ImVec4(0.72f, 0.72f, 0.72f, 1.0f);
        break;
    }

    ImGui::TextColored(color, "%s", GetKindLabel(kind));
}

const char* SceneSavePreview::GetKindLabel(ChangeKind kind) const {
    // 差分種別を画面内で共通の短いラベルへ変換します。
    switch (kind) {
    case ChangeKind::kAdded:
        return "Added";

    case ChangeKind::kRemoved:
        return "Removed";

    case ChangeKind::kModified:
        return "Modified";

    case ChangeKind::kFileAdded:
        return "New File";

    case ChangeKind::kFileModified:
        return "Changed";

    case ChangeKind::kFileInvalid:
        return "Invalid";

    case ChangeKind::kUnchanged:
    case ChangeKind::kFileUnchanged:
    default:
        return "No Change";
    }
}
#endif
