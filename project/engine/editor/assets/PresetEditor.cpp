#include "PresetEditor.h"
#include <cstdint>
#include <numbers>

#include "ModelManager.h"
#include "engine/graphics/3d/material/MaterialRenderPolicy.h"
#include "imgui.h"
#include "IconsFontAwesome5.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <utility>

void PresetEditor::ClearPropertyEditors() {
    // 前のシーンで使った一時Objectを破棄し、次回の編集時に現シーンの描画基盤で作り直せるようにする。
    propertyEditors_.clear();
}

// ツール全体で同じ選択・検索状態を使うよう、共有のエディターを返す。
PresetEditor* PresetEditor::GetInstance() {
    static PresetEditor sInstance;
    return &sInstance;
}

std::string PresetEditor::GetName() {
    // ツール一覧に表示する名称は内部プリセット名と分ける。
    return "プリセットエディタ (Preset Editor)";
}

#ifdef USE_IMGUI

// このシーンで使う選択一覧を最新にして、以前の編集用インスタンスを破棄する。
void PresetEditor::Initialize() {
    ClearPropertyEditors();
    // カタログとモデル候補を先に読み込み、初回表示から配置候補を選べるようにする。
    PresetManager::GetInstance()->Initialize();
    RefreshModelList();
    // 旧既定名が残る場合だけ日本語の初期名へ置き換え、入力済みの名前を保持する。
    if (std::strcmp(newName_, "NewPreset") == 0)
        std::snprintf(newName_, sizeof(newName_), "%s", "新しいプリセット");
}

void PresetEditor::SetPlacePresetCallback(std::function<void(const std::string&)> callback) {
    // 配置操作の実行はシーン編集側へ委ね、パレットは選択名だけを渡す。
    placePresetCallback_ = std::move(callback);
}

void PresetEditor::SetBrushPresetCallback(std::function<void(const std::string&)> callback) {
    // ブラシ配置の開始はシーン編集側へ委ねる。
    brushPresetCallback_ = std::move(callback);
}

void PresetEditor::SetThumbnailProvider(std::function<uint64_t(const std::string&)> provider) {
    // サムネイルの描画資源は外部が所有し、ここでは取得手段だけ保持する。
    thumbnailProvider_ = std::move(provider);
}

void PresetEditor::SetPreviewProvider(std::function<uint64_t(const std::string&, float, float, float)> provider) {
    // 選択プリセットの名前、視点の角度と拡大率からプレビューを取得する関数を受け取る。
    previewProvider_ = std::move(provider);
}

void PresetEditor::SetSelectionCallback(std::function<void()> callback) {
    // 選択先を切り替えたときの Inspector 連携を呼び出し元へ通知する。
    selectionCallback_ = std::move(callback);
}

void PresetEditor::SetThumbnailInvalidator(std::function<void(const std::string&)> callback) {
    // 材質変更後のサムネイル再撮影を描画側へ依頼する。
    thumbnailInvalidator_ = std::move(callback);
}

std::string PresetEditor::GetDisplayName(const std::string& key, const json& data) const {
    // 表示名が無い旧データは元名、さらに無い場合は内部キーへ戻す。
    return ReadString(data, "presetDisplayName", ReadString(data, "name", key));
}

void PresetEditor::SelectPreset(const std::string& name, bool notify) {
    // 登録済みだけ選択し、Project から来た選択だけ Inspector へ通知する。
    // 登録に無い名前では、現在の選択と詳細表示を切り替えない。
    if (!PresetManager::GetInstance()->HasPreset(name))
        return;
    selectedName_ = name;
    selectionFromProject_ = notify;
    // 選択通知が必要な呼び出しで、通知先が登録済みの場合だけInspectorへ伝える。
    if (notify && selectionCallback_)
        selectionCallback_();
}

void PresetEditor::NotifyPresetUsed(const std::string& name) {
    // 最近使った一覧では同名を重複させず、上限を設ける。
    recentPresets_.erase(std::remove(recentPresets_.begin(), recentPresets_.end(), name), recentPresets_.end());
    recentPresets_.insert(recentPresets_.begin(), name);
    // 履歴が24件を超えた場合は、古いものから一覧の外へ落とす。
    if (recentPresets_.size() > 24)
        recentPresets_.resize(24);
}

void PresetEditor::DrawImGui() {
    // 選択中は詳細設定を表示し、未選択時は配置ライブラリを表示する。
    HandleDeferredDelete();
    ImGui::TextColored(ImVec4(0.35f, 0.9f, 1.0f, 1.0f), "配置プリセット");

    // 選択が無い場合は、詳細編集よりライブラリの一覧を優先する。
    if (selectedName_.empty()) {
        DrawLibrary();
    // 選択中はライブラリを広げる操作と、対象の詳細を描く。
    } else {
        // 下部の一覧を広げるボタンを押した場合は、次のドック更新へ要求を渡す。
        if (ImGui::Button("下部の一覧を広げる")) {
            requestLibraryDockExpand_ = true;
            // ドックを操作する側の通知があれば、広げる要求を伝える。
            if (selectionCallback_)
                selectionCallback_();
        }
        ImGui::SameLine();
        // 解除操作が選ばれたら、選択していたキーを消す。
        if (ImGui::Button("選択を解除"))
            selectedName_.clear();
        DrawSettingsPane();
    }
}

void PresetEditor::DrawPalettePane() {
    // パレットタブからも同じライブラリを開く。
    DrawLibrary();
}

void PresetEditor::DrawLibrary(bool fromProject) {
    // ステージ・用途・検索結果を絞ってから必要な行だけ描画する。
    HandleDeferredDelete();
    ImGui::PushID(fromProject ? "ProjectPresetLibrary" : "InspectorPresetLibrary");
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth((std::max)(110.0f, width - 185.0f));
    ImGui::InputTextWithHint("##PresetSearch", "名前・用途・元の名前で検索...", searchBuffer_, sizeof(searchBuffer_));
    ImGui::SameLine();

    // 検索行のボタンからも、下部ドックを広げる要求を出せる。
    if (ImGui::Button("下部を広げる")) {
        requestLibraryDockExpand_ = true;
        // 通知先が設定されている場合は、ドックの切り替えを呼び出し側へ伝える。
        if (selectionCallback_)
            selectionCallback_();
    }
    ImGui::SameLine();
    // 管理ボタンの押下で、登録と素材の更新を選ぶメニューを開く。
    if (ImGui::Button("管理"))
        ImGui::OpenPopup("LibraryManagement");
    // 管理のポップアップが開いている間だけ、作成と再読み込みを表示する。
    if (ImGui::BeginPopup("LibraryManagement")) {
        ImGui::PushID("CreatePreset");
        DrawCreateButtons();
        ImGui::PopID();
        // 再読込が選ばれた場合は、登録済みプリセットを読み直す。
        if (ImGui::Button("プリセット再読込"))
            PresetManager::GetInstance()->Initialize();
        // モデル一覧の更新を選んだ場合は、選択候補を作り直す。
        if (ImGui::Button("モデル一覧更新"))
            RefreshModelList();
        ImGui::EndPopup();
    }

    const ImVec4 dangerColor(0.90f, 0.27f, 0.30f, 1.0f);
    bool openBulkDeleteConfirmation = false;

    // 削除モードは通常の配置導線を使わず、一覧項目を削除候補として選ぶ。
    if (!bulkDeleteMode_) {
        ImGui::PushStyleColor(ImGuiCol_Text, dangerColor);

        // 通常表示から一括削除へ切り替える時点で、古い候補を消す。
        if (ImGui::Button(ICON_FA_TRASH_ALT " まとめて削除")) {
            bulkDeleteMode_ = true;
            bulkDeleteSelection_.clear();
        }

        ImGui::PopStyleColor();
    // 削除候補を集めているときは、現在の候補件数と確定・終了の操作を描く。
    } else {
        ImGui::TextColored(dangerColor, "削除モード: %d件選択", static_cast<std::int32_t>(bulkDeleteSelection_.size()));
        ImGui::SameLine();
        ImGui::BeginDisabled(bulkDeleteSelection_.empty());
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.15f, 0.18f, 1.0f));
        // 削除候補がありボタンが押された場合は、操作をすぐ実行せず確認を開く。
        if (ImGui::Button(ICON_FA_TRASH_ALT " 選択したプリセットを削除..."))
            openBulkDeleteConfirmation = true;
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        // 横幅が660ピクセル未満の場合は、削除操作の残りを次の行に置く。
        if (width < 660.0f)
            ImGui::NewLine();
        // 幅に余裕がある場合は、同じ行のまま描画する。
        else ImGui::SameLine();
        // 候補の解除を選ぶと削除モードは継続したまま全選択を外す。
        if (ImGui::Button("選択解除"))
            bulkDeleteSelection_.clear();
        ImGui::SameLine();

        // 終了を選んだ場合は候補も外して通常の一覧へ戻す。
        if (ImGui::Button("終了")) {
            bulkDeleteSelection_.clear();
            bulkDeleteMode_ = false;
        }
    }

    const char* collections[] = {"", "Tutorial", "Stage1", "Stage2", "Stage3", "Stage4", "Stage5", "StageSelect"};
    const char* labels[] = {"全ステージ", "チュートリアル", "ステージ1", "ステージ2", "ステージ3", "ステージ4", "ステージ5", "ステージ選択"};
    std::int32_t stage = 0;

    for (std::int32_t i = 0; i < IM_ARRAYSIZE(collections); ++i)
        // 保存されているステージの識別子に合う項目を、一覧の表示番号にする。
        if (stageCollection_ == collections[i])
            stage = i;
    ImGui::SetNextItemWidth(width < 600.0f ? 140.0f : 170.0f);
    // ステージを選び直した場合だけ、表示候補を集める条件を変える。
    if (ImGui::Combo("##LibraryStage", &stage, labels, IM_ARRAYSIZE(labels)))
        stageCollection_ = collections[stage];
    ImGui::SameLine();
    ImGui::SetNextItemWidth(width < 600.0f ? 110.0f : 160.0f);
    ImGui::SliderInt("サイズ", &thumbnailSize_, 96, 192, "%d px");
    // 幅が600ピクセル未満なら表示切り替えを次の行へ置く。
    if (width < 600.0f)
        ImGui::NewLine();
    // 幅が十分なら表示切り替えを同じ行に置く。
    else ImGui::SameLine();
    // 表示形式のボタンを押したときだけ、一覧とグリッドを切り替える。
    if (ImGui::Button(listView_ ? "グリッド表示" : "リスト表示"))
        listView_ = !listView_;
    ImGui::Separator();
    const char* categories[] = {"すべて", "お気に入り", "最近使ったもの", "地形・足場", "装飾", "ギミック", "ゲート", "アイテム", "当たり判定", "エフェクト", "敵"};
    // 表示幅で分類の入口を切り替える。横幅が足りないときも同じ選択状態を使う。
    const bool sidebar = width >= 650.0f;

    // 横幅が650ピクセル以上なら、一覧の左へ独立した用途欄を置く。
    if (sidebar) {
        ImGui::BeginChild("LibraryCategories", ImVec2(155.0f, 0.0f), true);

        for (std::int32_t i = 0; i < IM_ARRAYSIZE(categories); ++i) {
            // 用途の分類に入る直前で、検索用の分類と区切る。
            if (i == 3)
                ImGui::Separator();
            // 見たい用途が選ばれた場合だけ、同じライブラリ内の絞り込みを変える。
            if (ImGui::Selectable(categories[i], libraryCategory_ == i))
                libraryCategory_ = i;
        }

        ImGui::EndChild();
        ImGui::SameLine();
    // 横幅が小さければ、横の分類欄の代わりに一覧上に選択欄を置く。
    } else {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::Combo("用途", &libraryCategory_, categories, IM_ARRAYSIZE(categories));
    }

    ImGui::BeginChild("LibraryGallery", ImVec2(0.0f, 0.0f), false);
    const auto& presets = PresetManager::GetInstance()->GetPresets();
    std::vector<std::string> entries;

    for (const auto& [key, data] : presets) {
        // ステージを絞っている場合、所属が一致しないプリセットは表示しない。
        if (!stageCollection_.empty() && ReadString(data, "presetCollection", "") != stageCollection_)
            continue;
        // 検索キーに一致しないプリセットは、用途に合っていても表示しない。
        if (!MatchesSearch(key, data))
            continue;
        // お気に入りを選んだ場合は、登録されていない項目を除く。
        if (libraryCategory_ == 1 && !ReadBool(data, "presetFavorite", false))
            continue;
        // 最近使った一覧では、今回の履歴に無い項目を除く。
        if (libraryCategory_ == 2 && std::find(recentPresets_.begin(), recentPresets_.end(), key) == recentPresets_.end())
            continue;
        // 用途で絞っている場合は、その用途と設定が一致するときだけ残す。
        if (libraryCategory_ >= 3 && ReadString(data, "presetUsage", "装飾") != categories[libraryCategory_])
            continue;
        entries.push_back(key);
    }

    // 最近使った一覧は履歴順、通常はおすすめを先頭にして、判定専用プリセットをその後ろへ回す。
    std::stable_sort(entries.begin(), entries.end(), [&](const std::string& a, const std::string& b) {
        // 最近使った表示に限り、使用履歴で前にある方を先にする。
        if (libraryCategory_ == 2)
            return std::find(recentPresets_.begin(), recentPresets_.end(), a) < std::find(recentPresets_.begin(), recentPresets_.end(), b);
        const json& left = presets.at(a);
        const json& right = presets.at(b);
        const bool l = ReadBool(left, "presetRecommended", false), r = ReadBool(right, "presetRecommended", false);
        // おすすめの有無が異なるときは、おすすめの方を先にする。
        if (l != r)
            return l;
        const bool lc = ReadString(left, "presetUsage", "") == "当たり判定", rc = ReadString(right, "presetUsage", "") == "当たり判定";
        // 他の優先条件が同じ場合は、当たり判定だけの素材を後ろにする。
        if (lc != rc)
            return !lc;
        return GetDisplayName(a, left) < GetDisplayName(b, right);
    });

    // 絞り込みを跨いで候補を保持するが、一括選択は現在表示中の結果だけを対象にする。
    bulkDeleteSelection_.erase(std::remove_if(bulkDeleteSelection_.begin(), bulkDeleteSelection_.end(),
        [&](const std::string& key) { return presets.find(key) == presets.end(); }), bulkDeleteSelection_.end());
    ImGui::TextDisabled("表示中 %d件 / 全%d件", static_cast<std::int32_t>(entries.size()), static_cast<std::int32_t>(presets.size()));
    ImGui::SameLine();
    ImGui::TextDisabled(bulkDeleteMode_ ? "クリックで削除候補を選択" : "クリックで詳細 / ドラッグで配置");

    // 削除モードのときだけ、表示中の候補をまとめて扱う操作を出す。
    if (bulkDeleteMode_) {
        // 現在の絞り込み結果を候補へすべて追加するボタン。
        if (ImGui::Button("表示中をすべて選択")) {
            for (const std::string& key : entries) {
                // 過去の絞り込みで選んだ同じ項目は、重複して記録しない。
                if (std::find(bulkDeleteSelection_.begin(), bulkDeleteSelection_.end(), key) == bulkDeleteSelection_.end())
                    bulkDeleteSelection_.push_back(key);
            }
        }

        ImGui::SameLine();

        // 表示中だけの選択解除が押された場合は、表示外の候補を保持する。
        if (ImGui::Button("表示中を選択解除")) {
            for (const std::string& key : entries) {
                bulkDeleteSelection_.erase(std::remove(bulkDeleteSelection_.begin(), bulkDeleteSelection_.end(), key), bulkDeleteSelection_.end());
            }
        }
    }

    // 絞り込みの結果が0件の場合は、条件を変える場所を案内する。
    if (entries.empty())
        ImGui::TextWrapped("条件に合うプリセットがありません。検索やステージ・用途の条件を変更してください。");
    const float tile = static_cast<float>(thumbnailSize_);
    const std::int32_t columns = listView_ ? 1 : (std::max)(1, static_cast<std::int32_t>(ImGui::GetContentRegionAvail().x / (tile + 18.0f)));
    const float rowHeight = tile + ImGui::GetTextLineHeightWithSpacing() * (listView_ ? 1.0f : 3.0f) + 14.0f;
    bool openLibraryDeleteConfirmation = false;
    // 絞り込んだ件数を行へ換算し、スクロール内に見える行だけ描画する。
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<std::int32_t>((entries.size() + columns - 1) / columns), rowHeight);

    while (clipper.Step()) {
        for (std::int32_t row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const ImVec2 origin = ImGui::GetCursorPos();

            for (std::int32_t col = 0; col < columns; ++col) {
                const size_t index = static_cast<size_t>(row * columns + col);
                // 最後の行が列数に満たない場合は、空の列を描かない。
                if (index >= entries.size())
                    break;
                const std::string& key = entries[index];
                const json& data = presets.at(key);
                const std::string display = GetDisplayName(key, data);
                ImGui::SetCursorPos(ImVec2(origin.x + col * (tile + 18.0f), origin.y));
                ImGui::PushID(key.c_str());
                ImGui::BeginGroup();
                const bool selected = bulkDeleteMode_
                    ? std::find(bulkDeleteSelection_.begin(), bulkDeleteSelection_.end(), key) != bulkDeleteSelection_.end()
                    : selectedName_ == key;
                const ImVec4 selectionColor = bulkDeleteMode_ ? ImVec4(0.58f,0.16f,0.19f,1.0f) : ImVec4(0.18f,0.38f,0.60f,1.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, selected ? selectionColor : ImVec4(0.14f,0.15f,0.18f,1.0f));
                DrawThumbnailButton(key, GetCategoryColor(DetectCategory(data)), tile);
                ImGui::PopStyleColor();
                // 現在のモードで選択されている項目だけ、強調色の枠を重ねる。
                if (selected) ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                    bulkDeleteMode_ ? IM_COL32(255,75,82,255) : IM_COL32(80,180,255,255), 2.0f, 0, 2.0f);
                // サムネイルのクリックを、選択か削除候補の切り替えとして処理する。
                if (ImGui::IsItemClicked()) {
                    // 削除モードでは詳細を開かず、押すたびに候補へ入れるか外す。
                    if (bulkDeleteMode_) {
                        const auto it = std::find(bulkDeleteSelection_.begin(), bulkDeleteSelection_.end(), key);
                        // 未選択の項目は候補配列へ追加する。
                        if (it == bulkDeleteSelection_.end())
                            bulkDeleteSelection_.push_back(key);
                        // すでに候補なら、再度のクリックで解除する。
                        else bulkDeleteSelection_.erase(it);
                    // 通常モードではクリックしたプリセットを詳細へ連携する。
                    } else SelectPreset(key, fromProject);
                }

                // 通常モードで対象へダブルクリックし、配置担当が接続済みの場合だけプレビューを開始する。
                if (!bulkDeleteMode_ && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && placePresetCallback_) {
                    NotifyPresetUsed(key);
                    placePresetCallback_(key);
                }

                // ドラッグ先で生成するため、画像や設定全体ではなく登録キーを渡す。
                if (!bulkDeleteMode_ && ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("PRESET_ASSET", key.c_str(), key.size() + 1);
                    ImGui::TextUnformatted(display.c_str());
                    NotifyPresetUsed(key);
                    ImGui::EndDragDropSource();
                }

                // サムネイルにポインターを置いているときだけ、用途・倍率・識別子を補足する。
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(display.c_str());
                    ImGui::TextUnformatted(ReadString(data, "presetDescription", "").c_str());
                    const engine::Vector3 scale = ReadVector3(data, "scale", {1,1,1});
                    ImGui::Text("スケール: %.2f / %.2f / %.2f", scale.x,scale.y,scale.z);
                    ImGui::TextDisabled("元の名前: %s", ReadString(data,"name",key).c_str());
                    ImGui::TextDisabled("%s", key.c_str());
                    ImGui::EndTooltip();
                }

                // 項目のコンテキストメニューを開けた場合だけ、その項目に対する操作を表示する。
                if (ImGui::BeginPopupContextItem("LibraryPresetMenu")) {
                    // 削除モードでは通常の編集コマンドの代わりに、候補の切り替えを表示する。
                    if (bulkDeleteMode_) {
                        // 候補を切り替える操作が選ばれた場合だけ一覧の選択を変更する。
                        if (ImGui::MenuItem(selected ? "削除候補から外す" : "削除候補に追加")) {
                            const auto it = std::find(bulkDeleteSelection_.begin(), bulkDeleteSelection_.end(), key);
                            // 候補にまだ含まれない名前を登録する。
                            if (it == bulkDeleteSelection_.end())
                                bulkDeleteSelection_.push_back(key);
                            // 候補に含まれていれば、その項目を外す。
                            else bulkDeleteSelection_.erase(it);
                        }
                    // 通常表示では詳細編集・配置・撮影用の操作を出す。
                    } else {
                        // 詳細編集が選ばれたらそのプリセットへ選択を移す。
                        if (ImGui::MenuItem("詳細・表示名を編集"))
                            SelectPreset(key, fromProject);
                        // お気に入りの操作を選ぶたびに現在の値を反転して保存する。
                        if (ImGui::MenuItem(ReadBool(data,"presetFavorite",false) ? "お気に入りから外す" : "お気に入りに追加")) {
                            PresetManager::GetInstance()->GetPreset(key)["presetFavorite"] = !ReadBool(data,"presetFavorite",false);
                            PresetManager::GetInstance()->SaveAll();
                        }

                        // 単発プレビューを選び配置担当が有効なら、使用履歴へ記録して依頼する。
                        if (ImGui::MenuItem("配置プレビュー") && placePresetCallback_) {
                            NotifyPresetUsed(key);
                            placePresetCallback_(key);
                        }
                        // ブラシが選ばれ配置担当が有効なら、使用履歴へ記録して連続配置を始める。
                        if (ImGui::MenuItem("ブラシ配置") && brushPresetCallback_) {
                            NotifyPresetUsed(key);
                            brushPresetCallback_(key);
                        }
                        // 再撮影を選び通知先が存在する場合は、現在のサムネイルを無効化する。
                        if (ImGui::MenuItem("サムネイルを再撮影") && thumbnailInvalidator_)
                            thumbnailInvalidator_(key);
                    }

                    ImGui::PushStyleColor(ImGuiCol_Text, dangerColor);

                    // 単体削除を選んだときは、走査後に確認できるよう対象名だけを控える。
                    if (ImGui::MenuItem(ICON_FA_TRASH_ALT " このプリセットを削除...")) {
                        pendingLibraryDeleteName_ = key;
                        openLibraryDeleteConfirmation = true;
                    }

                    ImGui::PopStyleColor();
                    ImGui::EndPopup();
                }

                // 表示形式がリストなら、名称をサムネイルと同じ行に置く。
                if (listView_)
                    ImGui::SameLine();
                ImGui::BeginGroup();
                const float textWidth = listView_ ? (std::max)(80.0f, ImGui::GetContentRegionAvail().x) : tile;

                // UTF-8の文字境界で省略し、内部キーの長さで一覧の高さが変わらないようにします。
                std::string caption = display;

                while (caption.size() > 3 && ImGui::CalcTextSize(caption.c_str()).x > textWidth) {
                    size_t cut = caption.size() - 1;
                    while (cut > 0 && (static_cast<unsigned char>(caption[cut]) & 0xc0) == 0x80)
                        --cut;
                    caption.resize(cut);
                }

                ImGui::TextUnformatted(caption.c_str());
                ImGui::TextColored(GetCategoryColor(DetectCategory(data)), "%s%s", ReadBool(data,"presetFavorite",false) ? "★ " : "", ReadString(data,"presetUsage",GetCategoryLabel(DetectCategory(data))).c_str());
                // リストには詳細文の領域もあるため、その場合だけ説明を加える。
                if (listView_)
                    ImGui::TextWrapped("%s", ReadString(data,"presetDescription","").c_str());
                ImGui::EndGroup();
                ImGui::EndGroup();
                ImGui::PopID();
            }

            ImGui::SetCursorPos(ImVec2(origin.x, origin.y + rowHeight - 1.0f));
            ImGui::Dummy(ImVec2(0.0f, 1.0f));
        }
    }

    ImGui::EndChild();
    // 一括削除を要求されたフレームは、確認ダイアログを開く。
    if (openBulkDeleteConfirmation)
        ImGui::OpenPopup("選択したプリセットをまとめて削除");
    // 確認を描ける場合だけ、現在の候補と確定・取消ボタンを表示する。
    if (ImGui::BeginPopupModal("選択したプリセットをまとめて削除", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(dangerColor, "次の%d件を登録一覧から削除します。", static_cast<std::int32_t>(bulkDeleteSelection_.size()));
        ImGui::TextDisabled("配置済みのObjectとモデルファイルは残ります。");
        ImGui::BeginChild("削除対象一覧", ImVec2(440.0f, 200.0f), true);

        for (const std::string& key : bulkDeleteSelection_) {
            const auto it = presets.find(key);
            // 候補のキーがまだ登録されている場合だけ、削除対象として名前を描く。
            if (it != presets.end())
                ImGui::TextUnformatted(GetDisplayName(key, it->second).c_str());
        }

        ImGui::EndChild();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.15f, 0.18f, 1.0f));

        // 一覧の登録と最近使った履歴を整え、削除対象だった詳細選択も解除する。
        if (ImGui::Button(ICON_FA_TRASH_ALT " まとめて削除する")) {
            const bool selectedPresetRemoved = std::find(bulkDeleteSelection_.begin(), bulkDeleteSelection_.end(), selectedName_) != bulkDeleteSelection_.end();
            PresetManager::GetInstance()->RemovePresets(bulkDeleteSelection_);
            // 詳細に表示していた項目も削除した場合は、その選択を外す。
            if (selectedPresetRemoved)
                selectedName_.clear();
            for (const std::string& key : bulkDeleteSelection_) {
                recentPresets_.erase(std::remove(recentPresets_.begin(), recentPresets_.end(), key), recentPresets_.end());
            }

            bulkDeleteSelection_.clear();
            bulkDeleteMode_ = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleColor();
        ImGui::SameLine();
        // 一括削除を取り消したら、候補を変更せず確認だけを閉じる。
        if (ImGui::Button("キャンセル"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // 反復中にプリセット集合を変更すると一覧の参照が無効になるため、削除は一覧の後で確定する。
    if (openLibraryDeleteConfirmation)
        ImGui::OpenPopup("一覧からプリセットを削除");
    // 単体の削除確認が開いている場合だけ、控えていた対象名を使う。
    if (ImGui::BeginPopupModal("一覧からプリセットを削除", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("「%s」を登録一覧から削除します。", pendingLibraryDeleteName_.c_str());
        ImGui::TextDisabled("配置済みのObjectとモデルファイルは残ります。");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.15f, 0.18f, 1.0f));

        // 確認で削除を選んだ場合に限り、対象の登録と履歴を消す。
        if (ImGui::Button(ICON_FA_TRASH_ALT " 削除する")) {
            PresetManager::GetInstance()->RemovePreset(pendingLibraryDeleteName_);
            // 削除対象が詳細の選択中だった場合だけ、編集する名前も消す。
            if (selectedName_ == pendingLibraryDeleteName_)
                selectedName_.clear();
            recentPresets_.erase(std::remove(recentPresets_.begin(), recentPresets_.end(), pendingLibraryDeleteName_), recentPresets_.end());
            pendingLibraryDeleteName_.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleColor();
        ImGui::SameLine();

        // 取り消した場合は保留した名前を消し、登録には触れず閉じる。
        if (ImGui::Button("キャンセル")) {
            pendingLibraryDeleteName_.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::PopID();
}

void PresetEditor::DrawCategoryButton(const char* label, Category category) {
    // 選択中の用途だけ強調し、クリック後の一覧フィルターへ反映する。
    bool selected = activeCategory_ == category;

    // 現在のカテゴリを描くときだけ、専用の通常色とホバー色を積む。
    if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.38f, 0.72f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.44f, 0.82f, 1.0f));
    }

    // カテゴリボタンが押された場合に、現在の一覧の用途を変更する。
    if (ImGui::Button(label)) {
        activeCategory_ = category;
    }

    // 色を積んだカテゴリの描画後だけ、二つ分を元に戻す。
    if (selected) {
        ImGui::PopStyleColor(2);
    }
}

void PresetEditor::DrawCreateButtons() {
    // 用途ごとに空のテンプレートを作り、後から詳細を編集できるようにする。
    ImGui::TextDisabled("新規テンプレート");
    ImGui::PushItemWidth(-1.0f);
    ImGui::InputText("##NewPresetName", newName_, sizeof(newName_));
    ImGui::PopItemWidth();

    // 敵のボタンを押したら、敵種別を後で設定するテンプレートを作る。
    if (ImGui::Button("敵", ImVec2(72.0f, 0.0f)))
        AddBlankPreset(Category::kEnemy);
    ImGui::SameLine();
    // ギミックのボタンを押したら、ギミック用のテンプレートを作る。
    if (ImGui::Button("ギミック", ImVec2(92.0f, 0.0f)))
        AddBlankPreset(Category::kGimmick);
    ImGui::SameLine();
    // モデルのボタンを押したら、一般配置用のテンプレートを作る。
    if (ImGui::Button("モデル", ImVec2(72.0f, 0.0f)))
        AddBlankPreset(Category::kModel);
}

void PresetEditor::DrawPresetList() {
    const auto& presets = PresetManager::GetInstance()->GetPresets();
    ImVec2 listSize = ImGui::GetContentRegionAvail();

    // 一覧を表示できない場合も、BeginChildにはEndChildを対応させる。
    if (!ImGui::BeginChild("PresetAssetList", listSize, true)) {
        ImGui::EndChild();
        return;
    }

    // 一覧の並べ替え用に元の名前と JSON を借用する。元コンテナーの要素が有効な間だけ使う。
    struct PresetListEntry {
        const std::string* name = nullptr;
        const json* data = nullptr;
    };

    std::vector<PresetListEntry> entries;
    entries.reserve(presets.size());

    for (const auto& [name, data] : presets) {
        // 現在の用途と検索の両方に一致する項目だけ候補へ入れる。
        if (!MatchesCategory(data, activeCategory_) || !MatchesSearch(name, data)) {
            continue;
        }

        entries.push_back({ &name, &data });
    }

    // ステージ用コレクションを先にし、セクション順と個別順が同じときだけ名前順にする。
    std::stable_sort(entries.begin(), entries.end(), [this](const PresetListEntry& lhs, const PresetListEntry& rhs) {
        const bool lhsStage1 = IsStageLibraryPreset(*lhs.data);
        const bool rhsStage1 = IsStageLibraryPreset(*rhs.data);
        // 片方だけステージ用なら、その候補を先に表示する。
        if (lhsStage1 != rhsStage1) {
            return lhsStage1;
        }

        const std::int32_t lhsSection = ReadInt(*lhs.data, "presetSectionOrder", 10000);
        const std::int32_t rhsSection = ReadInt(*rhs.data, "presetSectionOrder", 10000);
        // 区分番号が異なれば、個別の並びより区分の小さい方を優先する。
        if (lhsSection != rhsSection) {
            return lhsSection < rhsSection;
        }

        const std::int32_t lhsOrder = ReadInt(*lhs.data, "presetOrder", 10000);
        const std::int32_t rhsOrder = ReadInt(*rhs.data, "presetOrder", 10000);
        // 区分が同じ場合は、個別に指定された順番を優先する。
        if (lhsOrder != rhsOrder) {
            return lhsOrder < rhsOrder;
        }
        return *lhs.name < *rhs.name;
    });

    ImGui::TextDisabled("表示中: %d / 登録: %d", static_cast<std::int32_t>(entries.size()), static_cast<std::int32_t>(presets.size()));
    std::string currentSection;

    for (const PresetListEntry& entry : entries) {
        // ステージ1を選択している場合だけ、一覧に区分見出しを表示する。
        if (activeCategory_ == Category::kStage1) {
            const std::string section = ReadString(*entry.data, "presetSection", "その他");

            // 前の行と区分が変わったところだけで、新しい見出しを表示する。
            if (section != currentSection) {
                // 最初の見出しを除き、区分間の余白を追加する。
                if (!currentSection.empty()) {
                    ImGui::Spacing();
                }

                currentSection = section;
                const ImVec4 sectionColor = GetStage1SectionColor(ReadInt(*entry.data, "presetSectionOrder", 0));
                ImGui::TextColored(sectionColor, "%s", currentSection.c_str());
                ImGui::Separator();
            }
        }

        DrawPresetListItem(*entry.name, *entry.data);
    }

    ImGui::EndChild();
}

void PresetEditor::DrawPresetListItem(const std::string& name, const json& data) {
    // 一覧行にサムネイルと用途を置き、ドラッグでシーン配置へ渡す。
    ImGui::PushID(name.c_str());

    bool selected = selectedName_ == name;
    Category category = DetectCategory(data);
    ImVec4 accent = GetCategoryColor(category);
    const bool stage1Preset = IsStageLibraryPreset(data);
    const float rowHeight = stage1Preset ? 128.0f : 96.0f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, selected ? ImVec4(0.18f, 0.25f, 0.38f, 1.0f) : ImVec4(0.11f, 0.12f, 0.14f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, selected ? ImVec4(0.70f, 0.86f, 1.0f, 1.0f) : ImVec4(0.25f, 0.26f, 0.30f, 1.0f));

    ImGui::BeginChild("PresetRow", ImVec2(0.0f, rowHeight), true);
    DrawThumbnailButton(name, accent);

    // サムネイルからドラッグを始めた場合は、配置側に登録済みのキーを渡す。
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        ImGui::SetDragDropPayload("PRESET_ASSET", name.c_str(), name.size() + 1);
        ImGui::Text("配置: %s", name.c_str());
        ImGui::TextDisabled("%s", ReadString(data, "modelName", "(なし)").c_str());
        ImGui::EndDragDropSource();
    }

    // サムネイルのクリックで、この行を詳細の対象にする。
    if (ImGui::IsItemClicked()) {
        selectedName_ = name;
    }

    ImGui::SameLine();
    ImGui::BeginGroup();
    std::string displayName = GetDisplayName(name, data);

    // 名前の行が選ばれた場合に選択を更新し、同時に配置操作かを確認する。
    if (ImGui::Selectable(displayName.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, 22.0f))) {
        selectedName_ = name;

        // ダブルクリックで配置先の処理も設定されている場合だけ、配置を依頼する。
        if (ImGui::IsMouseDoubleClicked(0) && placePresetCallback_) {
            placePresetCallback_(selectedName_);
        }
    }

    ImGui::TextColored(accent, "%s", GetCategoryLabel(category));

    // ステージ用の中でおすすめフラグがある項目だけ、用途の隣に表示する。
    if (stage1Preset && ReadBool(data, "presetRecommended", false)) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.76f, 0.24f, 1.0f), "よく使う");
    }

    ImGui::TextDisabled("Model: %s", ShortModelName(ReadString(data, "modelName", "(なし)")).c_str());

    // ステージ用では説明文を折り返して表示する。
    if (stage1Preset) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextDisabled("%s", ReadString(data, "presetDescription", "").c_str());
        ImGui::PopTextWrapPos();
    // 通常の一覧では説明文の代わりに、配置値のスケールを表示する。
    } else {
        engine::Vector3 scale = ReadVector3(data, "scale", { 1.0f, 1.0f, 1.0f });
        ImGui::TextDisabled("Scale: %.2f / %.2f / %.2f", scale.x, scale.y, scale.z);
    }

    ImGui::EndGroup();

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopID();
}

void PresetEditor::DrawThumbnailButton(const std::string& name, const ImVec4& accent, float thumbnailSize) {
    const ImVec2 size(thumbnailSize, thumbnailSize);
    const auto& data = PresetManager::GetInstance()->GetPreset(name);
    const bool colliderOnly = ReadString(data, "presetUsage", "") == "当たり判定";
    // 画面内の表示対象だけに画像を要求し、判定専用プリセットはワイヤー表示へ分ける。
    const uint64_t gpuPtr = !colliderOnly && ImGui::IsRectVisible(size) && thumbnailProvider_ ? thumbnailProvider_(name) : 0;

    // 有効な画像ハンドルがある場合は、ボタンの面に画像を描く。
    if (gpuPtr) {
        ImGui::ImageButton("##Thumbnail", ImTextureRef(static_cast<ImTextureID>(gpuPtr)), size);
    // 画像を取得できない場合も、種類に応じた代替表示のボタンを置く。
    } else {
        ImGui::Button(colliderOnly ? "##ColliderThumbnail" : "3D##Thumbnail", size);

        if (colliderOnly) {
            // 判定専用Objectに空白画像を使わず、箱のワイヤーを識別記号として描画します。
            const ImVec2 min = ImGui::GetItemRectMin();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const float t = thumbnailSize;
            ImVec2 v[8] = {{min.x+t*.22f,min.y+t*.30f},{min.x+t*.63f,min.y+t*.30f},{min.x+t*.63f,min.y+t*.72f},{min.x+t*.22f,min.y+t*.72f},
                           {min.x+t*.37f,min.y+t*.18f},{min.x+t*.78f,min.y+t*.18f},{min.x+t*.78f,min.y+t*.60f},{min.x+t*.37f,min.y+t*.60f}};
            const std::int32_t edges[12][2]={{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};

            for (const auto& e : edges)
                draw->AddLine(v[e[0]],v[e[1]],IM_COL32(170,210,245,255),1.5f);
            draw->AddText(ImVec2(min.x+8,min.y+t-20),IM_COL32(190,215,235,255),"判定のみ");
        }
    }
}

void PresetEditor::DrawSettingsPane() {
    PresetManager* manager = PresetManager::GetInstance();

    // 選択が空か登録が無くなった場合は、データを参照せず選択を案内する。
    if (selectedName_.empty() || !manager->HasPreset(selectedName_)) {
        ImGui::TextDisabled("左の作成パレットから編集したいプリセットを選択してください。");
        return;
    }

    json& data = manager->GetPreset(selectedName_);
    Category category = DetectCategory(data);
    bool changed = false;

    // 敵はステータス側で共通管理される値を含めないため、保存データから該当キーを整理する。
    if (category == Category::kEnemy) {
        changed |= RemoveManagedEnemyFields(data);
    }

    std::string displayName = GetDisplayName(selectedName_, data);
    ImGui::TextColored(GetCategoryColor(category), "%s", displayName.c_str());

    // 選択したプリセットが変わったときだけ、プレビューの視点を既定位置へ戻す。
    if (previewSelectionName_ != selectedName_) {
        previewSelectionName_ = selectedName_;
        previewYaw_ = 0.0f;
        previewPitch_ = 0.0f;
        previewZoom_ = 1.0f;
    }

    const float previewSize = (std::min)(300.0f, ImGui::GetContentRegionAvail().x - 12.0f);

    // 画像を表示できる十分な幅があるときだけ、3Dプレビューを用意する。
    if (previewSize > 48.0f) {
        const bool colliderOnly = ReadString(data, "presetUsage", "") == "当たり判定";
        const uint64_t texture = !colliderOnly && previewProvider_
            ? previewProvider_(selectedName_, previewYaw_, previewPitch_, previewZoom_) : 0;
        // プレビュー画像を取得できれば、画像に対する視点操作も受け付ける。
        if (texture) {
            ImGui::ImageButton("##Preset3dPreview", ImTextureRef(static_cast<ImTextureID>(texture)),
                ImVec2(previewSize, previewSize));

            // 画像上の操作は視点だけに適用し、配置済みObjectとJSONの回転を変えない。
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                const ImVec2 delta = ImGui::GetIO().MouseDelta;
                // マウス移動量をラジアンへ換算し、水平方向は一周で循環、上下は見下ろしの範囲に制限する。
                previewYaw_ = std::remainder(previewYaw_ + delta.x * 0.01f, (2.0f * std::numbers::pi_v<float>));
                previewPitch_ = std::clamp(previewPitch_ + delta.y * 0.01f, -1.2f, 1.2f);
            }

            // 画像上にポインターがありホイールを動かしたときは、プレビューだけの倍率を変える。
            if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
                previewZoom_ = std::clamp(previewZoom_ + ImGui::GetIO().MouseWheel * 0.1f, 0.5f, 2.5f);
            }
        // 画像がない場合は、当たり判定のみかモデルを準備中かをボタンで示す。
        } else {
            ImGui::Button(colliderOnly ? "判定のみ##Preset3dPreview" : "モデルを読み込み中##Preset3dPreview",
                ImVec2(previewSize, previewSize));
        }

        // 判定専用の素材を除き、補助の視点操作を出す。
        if (!colliderOnly) {
            ImGui::TextDisabled("左ドラッグ: 視点を回す / ホイール: 拡大縮小");
            ImGui::SliderAngle("水平角##PresetPreview", &previewYaw_, -180.0f, 180.0f);
            ImGui::SliderAngle("上下角##PresetPreview", &previewPitch_, -68.0f, 68.0f);
            ImGui::SliderFloat("拡大率##PresetPreview", &previewZoom_, 0.5f, 2.5f, "%.1f 倍");

            // 視点を初期化する操作では、水平・上下の角度と拡大率だけを標準へ戻す。
            if (ImGui::SmallButton("視点を初期位置へ")) {
                previewYaw_ = 0.0f;
                previewPitch_ = 0.0f;
                previewZoom_ = 1.0f;
            }

            ImGui::TextDisabled("配置時の向きは下の「回転」で編集・保存します。");
        }
    }

    ImGui::TextDisabled("%s", ReadString(data, "presetUsage", GetCategoryLabel(category)).c_str());
    bool favorite = ReadBool(data, "presetFavorite", false);

    // お気に入りが実際に変更された場合だけ、区分の値を書き出す。
    if (ImGui::Checkbox("お気に入り", &favorite)) {
        data["presetFavorite"] = favorite;
        manager->SaveAll();
    }
    // 用途の補足を開いた場合だけ、元の配置や内部の識別名を表示する。
    if (ImGui::CollapsingHeader("用途・元データ")) {
        ImGui::TextWrapped("%s", ReadString(data,"presetDescription","").c_str());
        ImGui::TextWrapped("内部名: %s", selectedName_.c_str());
        ImGui::TextWrapped("元の名前: %s", ReadString(data,"name","").c_str());
        ImGui::TextWrapped("元の配置: %s", ReadString(data,"presetSourceObject","").c_str());
    }

    // 配置担当が接続済みでボタンを押した場合だけ、単発の配置を開始する。
    if (placePresetCallback_ && ImGui::Button("配置プレビュー")) {
        NotifyPresetUsed(selectedName_);
        placePresetCallback_(selectedName_);
    }

    ImGui::SameLine();

    // ブラシ担当が接続済みでボタンを押した場合だけ、連続配置を開始する。
    if (brushPresetCallback_ && ImGui::Button("ブラシ配置")) {
        NotifyPresetUsed(selectedName_);
        brushPresetCallback_(selectedName_);
    }

    // 明示して保存した場合は、現在のプリセット集合をまとめて出力する。
    if (ImGui::Button("保存")) {
        manager->SaveAll();
    }

    ImGui::SameLine();
    // 削除が選ばれたときは、編集中の対象名で確認を要求する。
    if (ImGui::Button("このプリセットを削除..."))
        ImGui::OpenPopup("プリセット削除の確認");
    // 確認が開いている場合だけ、削除予約を受け付ける。
    if (ImGui::BeginPopupModal("プリセット削除の確認", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("「%s」を登録一覧から削除します。", displayName.c_str());
        ImGui::TextDisabled("配置済みのObjectとモデルファイルは残ります。");

        // 確認で削除を選んだときは、一覧を描き終わった後に実行するよう予約する。
        if (ImGui::Button("削除する")) {
            requestDelete_ = true;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        // 取り消した場合は削除を予約せずダイアログを閉じる。
        if (ImGui::Button("キャンセル"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::Separator();

    changed |= DrawStringField(data, "表示名", "presetDisplayName", displayName);
    const char* usageLabels[] = {"地形・足場", "装飾", "ギミック", "ゲート", "アイテム", "当たり判定", "エフェクト", "敵"};
    const std::string usage = ReadString(data, "presetUsage", "装飾");

    // 用途の一覧を開いた場合だけ、決められた候補から選べる。
    if (ImGui::BeginCombo("用途", usage.c_str())) {
        for (const char* label : usageLabels)
            // 変更先を選んだ場合は絞り込みに使う用途の保存値を変更する。
            if (ImGui::Selectable(label, usage == label)) {
                data["presetUsage"] = label;
                changed = true;
            }
        ImGui::EndCombo();
    }

    // 保存済みのPBR値をInspectorから見直せるよう、親と子で同じ入力欄を使う。
    const auto drawMaterialFields = [&](json& material) {
        bool fieldChanged = false;
        fieldChanged |= DrawColorField(material, "色", "color", {1.0f, 1.0f, 1.0f, 1.0f});
        fieldChanged |= DrawIntField(material, "マテリアルタイプ", "materialType", 0);
        const std::int32_t selectedMaterialType = ReadInt(material, "materialType", 0);
        const auto* selectedMaterial = materialRenderPolicy::Find(selectedMaterialType);
        const char* selectedName = selectedMaterial ? selectedMaterial->displayName : "未登録の番号";
        // 登録済み材質の一覧を開いた場合だけ、固定IDに対応する番号を列挙する。
        if (ImGui::BeginCombo("登録済み材質", selectedName)) {
            for (const auto& definition : materialRenderPolicy::kLegacyMaterials) {
                // 別の登録済み材質を選んだら、その番号を材質の保存値に設定する。
                if (ImGui::Selectable(definition.displayName, selectedMaterialType == definition.type)) {
                    material["materialType"] = definition.type;
                    fieldChanged = true;
                }
            }
            ImGui::EndCombo();
        }
        // 現在の材質番号が登録済みの場合だけ、対応する固定IDを表示する。
        if (selectedMaterial)
            ImGui::TextDisabled("固定ID: %.*s", static_cast<std::int32_t>(selectedMaterial->id.size()), selectedMaterial->id.data());
        fieldChanged |= DrawFloatField(material, "金属度", "metallic", 0.0f, 0.01f);
        fieldChanged |= DrawFloatField(material, "粗さ", "roughness", 0.5f, 0.01f);
        fieldChanged |= DrawFloatField(material, "発光", "emissive", 1.0f, 0.05f);
        const auto drawBool = [&](const char* label, const char* key, bool fallback) {
            bool value = ReadBool(material, key, fallback);

            // チェックが切り替わった場合だけJSONへ真偽値を書き、変更ありを返す。
            if (ImGui::Checkbox(label, &value)) {
                material[key] = value;
                return true;
            }
            return false;
        };

        fieldChanged |= drawBool("ライティング", "enableLighting", true);
        fieldChanged |= drawBool("環境マップ", "enableEnvMap", false);
        fieldChanged |= DrawFloatField(material, "環境マップ強度", "envIntensity", 1.0f, 0.01f);
        fieldChanged |= DrawStringField(material, "カラーテクスチャ", "texturePath", "");
        fieldChanged |= drawBool("法線マップを使う", "enableNormalMap", false);
        fieldChanged |= DrawStringField(material, "法線マップ", "normalMapPath", "");
        fieldChanged |= DrawStringField(material, "ORMマップ", "ormMapPath", "");
        fieldChanged |= DrawStringField(material, "マテリアルアセット", "materialInstance", "");

        // 材質の参照先がある場合は、配置時に適用される値の優先順位を示す。
        if (!ReadString(material, "materialInstance", "").empty()) {
            ImGui::TextDisabled("アセット参照がある場合、配置時はアセットの値が優先されます。");
        }

        return fieldChanged;
    };

    // 材質・モデル欄を開いたときだけ、種類に応じた設定を並べる。
    if (ImGui::CollapsingHeader("材質・モデル")) {
        // 敵だけはモデルを共通のステータス側で変えるため、専用画面を示す。
        if (category == Category::kEnemy) {
            ImGui::TextDisabled("敵のモデルとタイプ共通スケールは「敵の調整・攻撃プレビュー」の「基本ステータス」で設定します。");
        // 敵以外はプリセット固有のモデルをこの欄から指定する。
        } else {
            changed |= DrawModelField(data);
        }

        changed |= drawMaterialFields(data);
    }

    const auto drawRotationDegrees = [&](json& node, const char* label) {
        constexpr float kRadiansToDegrees = (180.0f / std::numbers::pi_v<float>);
        constexpr float kDegreesToRadians = (std::numbers::pi_v<float> / 180.0f);
        const engine::Vector3 radians = ReadVector3(node, "rotate", {0.0f, 0.0f, 0.0f});
        float degrees[3] = {radians.x * kRadiansToDegrees, radians.y * kRadiansToDegrees, radians.z * kRadiansToDegrees};
        // 表示している度数に変更が無ければ、保存済みの回転とクォータニオンを維持する。
        if (!ImGui::DragFloat3(label, degrees, 1.0f, 0.0f, 0.0f, "%.1f°"))
            return false;

        // JSONと実行時のTransformはラジアンを使うため、表示単位だけ度へ変換する。
        node["rotate"] = {degrees[0] * kDegreesToRadians, degrees[1] * kDegreesToRadians, degrees[2] * kDegreesToRadians};
        node.erase("quaternion");
        return true;
    };

    // 配置Transformを開いているときだけ、このプリセットから生成する位置と回転を指定する。
    if (ImGui::CollapsingHeader("配置時Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= DrawVector3Field(data, "位置オフセット", "translate", { 0.0f, 0.0f, 0.0f }, 0.1f);
        changed |= drawRotationDegrees(data, "回転 (度)");

        // 敵のスケールは個別値ではなく、タイプ共通値を読むため調整場所を示す。
        if (category == Category::kEnemy) {
            ImGui::TextDisabled("スケールは「基本ステータス」の敵タイプ共通値を使用します。");
        // 共通値で制御しない種類は、個体の基準スケールを編集できる。
        } else {
            changed |= DrawVector3Field(data, "スケール", "scale", { 1.0f, 1.0f, 1.0f }, 0.05f);
        }
    }

    // 敵向けの種類と設定だけをここで描く。
    if (category == Category::kEnemy) {
        changed |= DrawEnemySettings(data);
    }
    // ギミックの場合は、その動作の種類別設定を描く。
    else if (category == Category::kGimmick) {
        changed |= DrawGimmickSettings(data);
    }

    // 衝突設定を開いた場合だけ、形状・自分の属性・相手側のマスクを描く。
    if (ImGui::CollapsingHeader("当たり判定")) {
        changed |= DrawCollider(data);
        changed |= DrawIntField(data, "自分の属性", "collisionAttribute", 0);
        changed |= DrawIntField(data, "衝突対象", "collisionMask", 0);
    }

    // 接触イベントの送信元と送信先を設定し、登録済み項目は共通UIで編集する。
    if (ImGui::CollapsingHeader("イベント連携")) {
        changed |= DrawIntField(data, "自分ID", "myEventID", -1);
        changed |= DrawIntField(data, "送信先ID", "targetID", -1);
        changed |= DrawEventSettings(data);
    }

    // 親が保存された子の配列を持つ場合に限り、各子の編集欄を展開する。
    if (data.contains("children") && data["children"].is_array()) {
        ImGui::Separator();
        ImGui::TextDisabled("組み合わせ: 親を配置すると、以下の子も同時に配置されます。");

        // ゲートの枠・ポータルや物理床を、配置前から個別に調整できます。
        // 親と同じ材質入力を使い、子の位置・回転・拡縮は親基準として編集する。
        std::function<void(json&, std::int32_t)> drawChildren = [&](json& node, std::int32_t depth) {
            // 深い構造でも描画が続きすぎないよう、深さ16までの子配列だけを扱う。
            if (depth > 16 || !node.contains("children") || !node["children"].is_array())
                return;
            for (size_t index = 0; index < node["children"].size(); ++index) {
                json& child = node["children"][index];
                // オブジェクトでない値は配置する子にならないため飛ばす。
                if (!child.is_object())
                    continue;
                ImGui::PushID(static_cast<std::int32_t>(index));

                // 子の欄が開いている場合は、子の設定を編集してさらに下の階層を表示する。
                if (ImGui::TreeNode(GetDisplayName("子オブジェクト", child).c_str())) {
                    changed |= DrawModelField(child);
                    changed |= DrawVector3Field(child, "相対位置", "translate", {}, 0.05f);
                    changed |= drawRotationDegrees(child, "相対回転 (度)");
                    changed |= DrawVector3Field(child, "相対スケール", "scale", {1.0f, 1.0f, 1.0f}, 0.02f);
                    changed |= drawMaterialFields(child);
                    changed |= DrawCollider(child);
                    // 子にギミックが含まれる場合は、親と同様に動作設定を用意する。
                    if (DetectCategory(child) == Category::kGimmick)
                        changed |= DrawGimmickSettings(child);
                    drawChildren(child, depth + 1);
                    ImGui::TreePop();
                }

                ImGui::PopID();
            }
        };

        drawChildren(data, 0);
    }

    if (changed) {
        // 変更があったときに保存用の分類を揃え、次の一覧表示でサムネイルを撮り直す。
        ApplyHiddenCategoryFields(data, category);
        manager->SaveAll();
        // サムネイル側へ通知できる場合は、変更後の内容で次回描けるよう既存画像を無効化する。
        if (thumbnailInvalidator_)
            thumbnailInvalidator_(selectedName_);
    }
}

#else

// ImGuiを使わない構成では接続用の入口だけを用意し、表示や保存操作は行わない。
void PresetEditor::Initialize() {}

void PresetEditor::SetPlacePresetCallback(std::function<void(const std::string&)>) {}

void PresetEditor::SetBrushPresetCallback(std::function<void(const std::string&)>) {}

void PresetEditor::SetThumbnailProvider(std::function<uint64_t(const std::string&)>) {}
void PresetEditor::SetPreviewProvider(std::function<uint64_t(const std::string&, float, float, float)>) {}

void PresetEditor::DrawImGui() {}
void PresetEditor::DrawLibrary(bool) {}
void PresetEditor::SetSelectionCallback(std::function<void()>) {}
void PresetEditor::SetThumbnailInvalidator(std::function<void(const std::string&)>) {}
void PresetEditor::SelectPreset(const std::string&, bool) {}
void PresetEditor::NotifyPresetUsed(const std::string&) {}
std::string PresetEditor::GetDisplayName(const std::string& key, const json& data) const { return data.value("presetDisplayName", data.value("name", key)); }

#endif
