#define NOMINMAX
#include <cstdint>
#include "EditorToolSettings.h"
#ifdef USE_IMGUI
#include "EditorCommandRegistry.h"
#include "InputManager.h"
#include "KeyConfig.h"
#include "imgui.h"
#include "IconsFontAwesome5.h"
#include "json.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {
constexpr const char* kPath = "Saved/Editor/editor_tools.json";
// メニューと設定画面で分類名と表示順を共有する。
struct VisibilitySection {
    EditorToolSettings::VisibilityGroup group;
    const char* label;
};
using VisibilityGroup = EditorToolSettings::VisibilityGroup;
constexpr VisibilitySection kVisibilitySections[] = {
    {VisibilityGroup::kCollisionEvents, ICON_FA_SHIELD_ALT " 衝突・イベント"},
    {VisibilityGroup::kSceneEditing, ICON_FA_CUBE " シーン編集"},
    {VisibilityGroup::kCamera, ICON_FA_VIDEO " カメラ"},
    {VisibilityGroup::kAnimationEffects, ICON_FA_FILM " アニメーション・演出"},
    {VisibilityGroup::kDiagnostics, ICON_FA_CHART_BAR " 診断"},
};

const std::vector<std::int32_t>& Keys() {
    // 操作設定で選べるキーコードを一度だけ構築し、予約コードを候補から除く。
    static const std::vector<std::int32_t> kKeys = [] {
        std::vector<std::int32_t> result{0};

        for (std::int32_t key = 2; key <= 53; ++key) {
            // 候補範囲内でも、Backspace・Tab・Enter・左 Ctrl・左 Shift のコードは除外する。
            if (key != 14 && key != 15 && key != 28 && key != 29 && key != 42)
                result.push_back(key);
        }

        for (std::int32_t key = 59; key <= 68; ++key)
            result.push_back(key);
        result.push_back(87);
        result.push_back(88);
        return result;
    }();
    return kKeys;
}
}

// メニューと設定画面が、同じ登録一覧と保存状態を使えるよう共有する。
EditorToolSettings& EditorToolSettings::Get() { static EditorToolSettings sValue; return sValue; }
void EditorToolSettings::Clear() {
    // 登録を捨てる前に、一時非表示にした表示設定とコマンド所有権を元へ戻す。
    RestoreHidden();
    EditorCommandRegistry::GetInstance()->UnregisterOwner(this);
    tools_.clear();
    visibility_.clear();
    ownedVisibility_.clear();
    open_ = false;
    status_.clear();
    saved_.clear();
}

void EditorToolSettings::AddTool(const std::string& id, const std::string& name,
    const std::string& category, std::function<void()> open, std::function<bool()> available) {
    // 編集対象一覧と実行用コマンドには同じ ID を渡す。終了時はこの設定を登録元としてまとめて解除する。
    tools_.push_back({id, name, category});
    EditorCommandRegistry::GetInstance()->Register({id, name, category, "",
        "エディターを開く", {name, category}, std::move(available), std::move(open), this});
}

void EditorToolSettings::AddVisibility(VisibilityGroup group, const std::string& id, const std::string& name, bool* value) {
    // 参照するフラグが指定された場合だけ、表示メニューの管理対象にする。
    if (value)
        visibility_.push_back({group, id, name, value});
}

void EditorToolSettings::AddVisibility(VisibilityGroup group, const std::string& id, const std::string& name) {
    // 登録時に借りるフラグのアドレスを保てるよう、設定側で管理する値は std::map に置く。
    AddVisibility(group, id, name, &ownedVisibility_.try_emplace(id, true).first->second);
}

// 一時非表示は個別設定より優先する。登録のない補助表示は通常時に有効として扱う。
bool EditorToolSettings::IsVisible(const std::string& id) const {
    // 一時非表示中は、個別の設定を読む前に非表示と判定する。
    if (hidden_)
        return false;
    for (const auto& value : visibility_)
        // 要求された ID が登録されていれば、そのフラグの値を返す。
        if (value.id == id)
            return *value.value;
    return true;
}

std::string EditorToolSettings::Label(const Tool& tool) const {
    // キーコード0は割当なしとして表示する。
    if (!tool.key)
        return "未設定";
    return std::string(tool.ctrl ? "Ctrl+" : "") + (tool.shift ? "Shift+" : "") +
        (tool.alt ? "Alt+" : "") + KeyConfig::GetInstance()->GetKeyName(tool.key);
}

std::string EditorToolSettings::Conflict(const Tool& tool) const {
    // 割当がないツールには競合相手がない。
    if (!tool.key)
        return {};

    // 修飾キーを確認しない既存の編集操作とは、修飾付きでも競合させません。
    if (tool.key == DIK_E || tool.key == DIK_F || tool.key == DIK_G || tool.key == DIK_R ||
        tool.key == DIK_S || tool.key == DIK_T || tool.key == DIK_F10 ||
        (tool.ctrl && (tool.key == DIK_C || tool.key == DIK_D || tool.key == DIK_Z ||
        tool.key == DIK_Y || tool.key == DIK_P || tool.key == DIK_K ||
        (tool.key >= DIK_1 && tool.key <= DIK_9))) || (tool.alt && tool.key == DIK_F4))
        return "既存操作の予約キー（割当不可）";

    // 他のツールとキー・修飾キーが完全一致する割当も拒否する。
    for (const Tool& other : tools_) {
        // 同じツールを除き、キーとすべての修飾キーが一致する登録を調べる。
        if (other.id != tool.id && other.key == tool.key && other.ctrl == tool.ctrl &&
            other.shift == tool.shift && other.alt == tool.alt) return "重複: " + other.name;
    }

    return {};
}

void EditorToolSettings::RefreshCommand(const Tool& tool) {
    auto* registry = EditorCommandRegistry::GetInstance();

    // 登録元と実行関数を維持し、検索・メニューで使うキー表記だけを更新する。競合中は表示欄を空にする。
    if (const auto* existing = registry->Find(tool.id)) {
        auto command = *existing;
        command.shortcut = tool.key && Conflict(tool).empty() ? Label(tool) : "";
        registry->Register(std::move(command));
    }
}

std::string EditorToolSettings::Snapshot() const {
    // 一時非表示中は見た目のfalseではなく、隠す前の個別設定を保存する。
    nlohmann::json data;
    data["version"] = 1;

    for (const auto& tool : tools_) data["shortcuts"][tool.id] = {
        {"key",tool.key}, {"ctrl",tool.ctrl}, {"shift",tool.shift}, {"alt",tool.alt}};
    for (const auto& value : visibility_)
        data["visibility"][value.id] = hidden_ ? value.beforeHide : *value.value;
    return data.dump(2);
}

void EditorToolSettings::Load() {
    try {
        // 読込失敗時には既存設定を保持するため、まず複製した設定に検証結果を集める。
        std::ifstream file(kPath);

        // 設定を開けない場合は、現在の状態を保存済み表示の基準として使う。
        if (!file) {
            saved_ = Snapshot();
            status_ = "保存設定がないため、現在の設定を使用します。";
            return;
        }
        const auto data = nlohmann::json::parse(file);
        // 対応するバージョンでなければ、設定の反映を始めず読み込み失敗として扱う。
        if (data.at("version").get<std::int32_t>() != 1)
            throw std::runtime_error("unsupported version");
        auto next = tools_;
        std::vector<bool> flags;

        for (auto& tool : next) {
            // 保存ファイルにこのツールの割当がなければ、読み込み前の値を残す。
            if (!data.contains("shortcuts") || !data["shortcuts"].contains(tool.id))
                continue;
            const auto& bind = data["shortcuts"][tool.id];
            tool.key = bind.at("key").get<std::int32_t>();
            // 割当なしを含む候補表にないコードは、不正なデータとして読み込み全体を中断する。
            if (std::find(Keys().begin(), Keys().end(), tool.key) == Keys().end())
                throw std::runtime_error("invalid key");
            tool.ctrl = bind.at("ctrl").get<bool>();
            tool.shift = bind.at("shift").get<bool>();
            tool.alt = bind.at("alt").get<bool>();
        }

        for (const auto& value : visibility_) flags.push_back(data.contains("visibility") && data["visibility"].contains(value.id)
            ? data["visibility"][value.id].get<bool>() : (hidden_ ? value.beforeHide : *value.value));

        // すべて検証した後にまとめて反映し、不正JSONで半端な状態を残しません。
        RestoreHidden();
        tools_ = std::move(next);

        for (size_t i = 0; i < flags.size(); ++i)
            *visibility_[i].value = flags[i];

        for (const auto& tool : tools_)
            RefreshCommand(tool);
        saved_ = Snapshot();
        status_ = "設定を読み込みました。競合がある割当は発動しません。";
    } catch (const std::exception& e) { status_ = "読込失敗（現在の設定を維持）: " + std::string(e.what()); }
}

void EditorToolSettings::Save() {
    // 予約キーと重複が残る設定はディスクへ保存しない。
    for (const auto& tool : tools_) if (!Conflict(tool).empty()) {
        status_ = "保存できません。予約キー・重複のある行を修正してください。";
        return;
    }

    try {
        std::filesystem::create_directories(std::filesystem::path(kPath).parent_path());
        const auto contents = Snapshot();

        // 一時ファイルを書き切ってから置換し、途中失敗で既存の設定を失わないようにする。
        const std::string temporary = std::string(kPath) + ".tmp";
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << contents;
        file.close();
        // 一時ファイルの出力に失敗した場合、または確定先を置き換えられない場合は保存済み状態を更新しない。
        if (!file || !MoveFileExW(std::filesystem::path(temporary).c_str(), std::filesystem::path(kPath).c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("write/replace failed");
        saved_ = contents;
        status_ = "保存しました。次回起動時にも適用されます。";
    } catch (const std::exception& e) { status_ = "保存失敗: " + std::string(e.what()); }
}

void EditorToolSettings::RestoreHidden() {
    // 一時非表示を使っていない場合は、退避値を個別設定に戻さない。
    if (!hidden_)
        return;
    for (auto& value : visibility_)
        *value.value = value.beforeHide;
    hidden_ = false;
}

// 一時的に隠す前の状態を先に復元し、その後で全項目の常用設定を一括変更する。
void EditorToolSettings::SetAllVisible(bool visible) {
    RestoreHidden();

    for (auto& value : visibility_)
        *value.value = visible;
}

void EditorToolSettings::HandleShortcuts(bool editing) {
    // 文字入力・ポップアップ・マウス操作中のキーをツール起動へ横取りしない。
    const auto& io = ImGui::GetIO();
    if (!editing || open_ || io.WantTextInput || io.WantCaptureKeyboard || ImGui::IsAnyItemActive() ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) ||
        ImGui::IsMouseDown(0) || ImGui::IsMouseDown(1) || ImGui::IsMouseDown(2)) return;
    // キーを押したフレームで修飾キーも完全一致した操作だけを候補にする。
    for (const auto& tool : tools_) {
        // 割当済みで修飾キーが一致し、このフレームで押され、競合がない場合にだけ起動する。
        if (tool.key && io.KeyCtrl == tool.ctrl && io.KeyShift == tool.shift && io.KeyAlt == tool.alt &&
            engine::InputManager::GetInstance()->IsKeyTriggered(static_cast<BYTE>(tool.key)) && Conflict(tool).empty()) {
            EditorCommandRegistry::GetInstance()->Execute(tool.id);
            break;
        }
    }
}

void EditorToolSettings::DrawToolsMenu() {
    // ツールを用途別にまとめ、実行可否はコマンド登録側の状態へ合わせる。
    // ツールメニューが開いている間だけ、分類と各コマンドを描く。
    if (!ImGui::BeginMenu("ツール"))
        return;
    for (const char* category : {"戦闘", "演出", "ステージ", "素材", "検証・設定"}) {
        // 分類のサブメニューが開いていない場合は、個々のツールを列挙しない。
        if (!ImGui::BeginMenu(category))
            continue;
        // 表示中の分類に属するツールだけを、次のメニュー項目にする。
        for (const auto& tool : tools_) if (tool.category == category) {
            const auto label = tool.key ? Label(tool) : "";
            // 実行可能なコマンドの項目が選ばれた場合は、同じ ID で起動する。
            if (ImGui::MenuItem(tool.name.c_str(), label.c_str(), false, EditorCommandRegistry::GetInstance()->CanExecute(tool.id)))
                EditorCommandRegistry::GetInstance()->Execute(tool.id);
        }

        ImGui::EndMenu();
    }

    ImGui::Separator();
    // 設定のメニュー項目を選んだら、ショートカットと表示を編集する画面を開く。
    if (ImGui::MenuItem("ショートカット・補助表示の設定..."))
        Open();
    ImGui::EndMenu();
}

void EditorToolSettings::DrawVisibilityMenu() {
    // 補助表示メニューが開いている間だけ、フラグを選べるようにする。
    if (!ImGui::BeginMenu("補助表示"))
        return;
    // すべて ON を選んだ場合は、一時非表示を解除して常用設定も表示へ揃える。
    if (ImGui::MenuItem("すべてON"))
        SetAllVisible(true);
    // すべて OFF を選んだ場合は、一時非表示を解除して常用設定も非表示へ揃える。
    if (ImGui::MenuItem("すべてOFF"))
        SetAllVisible(false);
    ImGui::Separator();

    // 一時非表示は各項目の設定を記憶してから全て隠し、解除時に戻す。
    if (ImGui::MenuItem(hidden_ ? "一時非表示を元に戻す" : "下記の補助表示を一時的にすべて隠す")) {
        // すでに一時非表示中なら、隠す前に控えた各項目の値へ戻す。
        if (hidden_)
            RestoreHidden();
        // 通常表示中なら、各フラグを保存してから一時的にすべて非表示にする。
        else { for (auto& value : visibility_) { value.beforeHide = *value.value; *value.value = false; } hidden_ = true; }
    }

    ImGui::TextDisabled("有効なツール・選択対象の補助表示に適用");
    ImGui::BeginDisabled(hidden_);

    for (const auto& section : kVisibilitySections) {
        // 分類のサブメニューが閉じているときは、配下の表示項目を出さない。
        if (!ImGui::BeginMenu(section.label))
            continue;

        for (auto& value : visibility_) {
            // 現在表示中の分類に属するフラグだけを編集できるようにする。
            if (value.group == section.group)
                ImGui::MenuItem(value.name.c_str(), nullptr, value.value);
        }
        ImGui::EndMenu();
    }
    ImGui::EndDisabled();
    ImGui::Separator();
    // 保存を選んだ場合は、表示とショートカットを共通の設定ファイルへ保存する。
    if (ImGui::MenuItem("表示設定を保存"))
        Save();
    // 設定画面を開く操作は、ツールメニューと同じ画面へつなぐ。
    if (ImGui::MenuItem("設定画面を開く..."))
        Open();
    // 直近の保存・読み込み結果があれば、メニュー内に表示する。
    if (!status_.empty())
        ImGui::TextWrapped("%s", status_.c_str());
    ImGui::EndMenu();
}

void EditorToolSettings::DrawWindow() {
    // 個別パネルで変更されても、一時非表示中は補助表示を出さないようにします。
    if (hidden_)
        for (auto& value : visibility_)
            *value.value = false;
    // 設定画面自体が閉じている間は、補助表示の維持だけで描画を終える。
    if (!open_)
        return;
    ImGui::SetNextWindowSize(ImVec2(900, 610), ImGuiCond_FirstUseEver);

    // ウィンドウの中身を出せない場合も End() を呼び、編集欄は描かない。
    if (!ImGui::Begin("エディター設定：ショートカット・補助表示", &open_)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("ゲーム操作とは別の設定です。ショートカットは編集中のみ有効です。文字入力・ポップアップ・マウス操作中は発動しません。");

    // 保存済みへの復元は現在の編集内容を捨てるため、確認Popupを通す。
    // 保存ボタンを押すと、競合を検証して設定を書き出す。
    if (ImGui::Button("設定を保存"))
        Save();
    ImGui::SameLine();
    // 復元ボタンはその場で変更を捨てず、読み直す前の確認を開く。
    if (ImGui::Button("保存済みに戻す"))
        ImGui::OpenPopup("保存済み設定へ戻す確認");
    // 確認画面が開いている間だけ、復元と取り消しのボタンを出す。
    if (ImGui::BeginPopupModal("保存済み設定へ戻す確認", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("未保存の設定変更を破棄して読み込みますか？");

        // 復元を確定してから、保存した設定を読み直す。
        if (ImGui::Button("戻す")) {
            Load();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        // 取り消された場合は、設定には触れず確認画面を閉じる。
        if (ImGui::Button("キャンセル"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    ImGui::TextUnformatted(Snapshot() == saved_ ? "保存済み" : "未保存の変更あり");
    ImGui::TextDisabled("保存先: %s", kPath);
    // 保存・読込が行われて状態文がある場合は、その結果を表示する。
    if (!status_.empty())
        ImGui::TextWrapped("%s", status_.c_str());
    ImGui::InputText("対象を検索", filter_, sizeof(filter_));

    // ショートカットの編集と、補助表示のON/OFFを別タブへ配置する。
    // タブを表示できる場合だけ、選択中の設定内容を出す。
    if (ImGui::BeginTabBar("EditorSettingsTabs")) {
        // ショートカットタブを選択中なら、ツールごとの割当を編集する。
        if (ImGui::BeginTabItem("エディターのショートカット")) {
            ImGui::TextWrapped("キーと修飾キーを選択してください。既存操作の予約キーと重複は発動・保存を禁止します。未設定で割当解除できます。");

            // 表を表示できる場合に、列とツールごとの行を設定する。
            if (ImGui::BeginTable("Bindings", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0, -4))) {
                ImGui::TableSetupColumn("対象エディター", ImGuiTableColumnFlags_WidthStretch, 2.0f);
                ImGui::TableSetupColumn("キー", ImGuiTableColumnFlags_WidthFixed, 100.0f);

                for (const char* name : {"Ctrl", "Shift", "Alt"})
                    ImGui::TableSetupColumn(name, ImGuiTableColumnFlags_WidthFixed, 45.0f);
                ImGui::TableSetupColumn("状態", ImGuiTableColumnFlags_WidthStretch, 1.5f);
                ImGui::TableSetupScrollFreeze(0,1);
                ImGui::TableHeadersRow();

                // 変更時にコマンド側の表示ショートカットも更新する。
                for (auto& tool : tools_) {
                    // 検索文字を入力している場合は、名前も分類も一致しないツールを隠す。
                    if (filter_[0] && tool.name.find(filter_) == std::string::npos && tool.category.find(filter_) == std::string::npos)
                        continue;
                    ImGui::PushID(tool.id.c_str());
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextWrapped("%s", tool.name.c_str());
                    ImGui::TextDisabled("%s", tool.category.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::SetNextItemWidth(-1);
                    const auto keyName = tool.key ? KeyConfig::GetInstance()->GetKeyName(tool.key) : "未設定";
                    bool changed = false;

                    // キーの選択欄を開いたときだけ、割当なしを含む候補を列挙する。
                    if (ImGui::BeginCombo("##key", keyName.c_str())) {
                        for (std::int32_t key : Keys()) {
                            const auto name = key ? KeyConfig::GetInstance()->GetKeyName(key) : "未設定";

                            // 候補を選ぶと、このツールのコードを差し替えて競合の再判定へ進む。
                            if (ImGui::Selectable(name.c_str(), tool.key == key)) {
                                tool.key = key;
                                changed = true;
                            }
                        }

                        ImGui::EndCombo();
                    }

                    ImGui::TableSetColumnIndex(2);
                    changed |= ImGui::Checkbox("##ctrl", &tool.ctrl);
                    ImGui::TableSetColumnIndex(3);
                    changed |= ImGui::Checkbox("##shift", &tool.shift);
                    ImGui::TableSetColumnIndex(4);
                    changed |= ImGui::Checkbox("##alt", &tool.alt);
                    // 一件の変更で他の割当の競合も増減するため、コマンドに表示する全ツールのキー表記を更新する。
                    if (changed)
                        for (const auto& value : tools_)
                            RefreshCommand(value);
                    ImGui::TableSetColumnIndex(5);
                    const auto conflict = Conflict(tool);
                    // 競合がなければ現在のキー表記を表示する。
                    if (conflict.empty())
                        ImGui::TextWrapped("%s", Label(tool).c_str());
                    // 予約キーまたは重複がある場合は、キー表記の代わりに理由を強調表示する。
                    else ImGui::TextColored(ImVec4(1,0.45f,0.25f,1), "%s", conflict.c_str());
                    ImGui::PopID();
                }

                ImGui::EndTable();
            }

            ImGui::EndTabItem();
        }

        // 補助表示タブを選択中なら、分類ごとの表示フラグを編集する。
        if (ImGui::BeginTabItem("補助表示")) {
            // 全項目を表示に揃える操作。
            if (ImGui::Button("すべてON"))
                SetAllVisible(true);
            ImGui::SameLine();
            // 全項目を非表示に揃える操作。
            if (ImGui::Button("すべてOFF"))
                SetAllVisible(false);
            ImGui::TextWrapped("実際のライト・攻撃・当たり判定は無効にしません。個別エディターと共通の表示設定です。");
            ImGui::BeginDisabled(hidden_);

            for (const auto& section : kVisibilitySections) {
                const bool showGroup = !filter_[0] || std::string_view(section.label).find(filter_) != std::string_view::npos;
                bool showHeading = true;
                for (auto& value : visibility_) {
                    // 分類外の項目と、分類名にも項目名にも検索が当たらない項目は表示しない。
                    if (value.group != section.group || (!showGroup && value.name.find(filter_) == std::string::npos))
                        continue;

                    // この分類で条件を満たした最初の項目の前にだけ、見出しを出す。
                    if (showHeading) {
                        ImGui::SeparatorText(section.label);
                        showHeading = false;
                    }
                    ImGui::Checkbox(value.name.c_str(), value.value);
                }
            }
            ImGui::EndDisabled();
            // 一時非表示中に復帰ボタンを押した場合は、保存していた個別設定を戻す。
            if (hidden_ && ImGui::Button("一時非表示を元に戻す"))
                RestoreHidden();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}
#endif
