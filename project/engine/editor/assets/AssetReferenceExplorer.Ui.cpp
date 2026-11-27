#include <cstdint>
#include "AssetReferenceExplorer.h"

#ifdef USE_IMGUI

#include "DebugEditor.h"
#include "Object3D.h"
#include "imgui.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

// 参照の検出結果と修復候補だけを描画します。走査・変更処理は本体側に置きます。
void AssetReferenceExplorer::DrawAssetBrowser() {
    // 退避先を除いたAssetだけを表示し、パス文字列で絞り込みます。
    AssetDatabase* database = AssetDatabase::GetInstance();
    ImGui::InputTextWithHint("##AssetReferenceFilter", "Asset名・Pathで絞り込み", assetFilter_, sizeof(assetFilter_));
    ImGui::Separator();
    const std::string filter = Normalize(assetFilter_);
    ImGui::BeginChild("AssetReferenceBrowser", ImVec2(330.0f, 0.0f), true);

    for (const EditorAssetRecord& asset : database->GetAssets()) {
        // ゴミ箱に退避済みのパスは、通常のアセット一覧から除く。
        if (Normalize(asset.sourcePath).find("/.trash/") != std::string::npos)
            continue;
        // 検索文字が指定され、正規化したパスに含まれない場合は一覧から除く。
        if (!filter.empty() && Normalize(asset.sourcePath).find(filter) == std::string::npos)
            continue;
        const bool selected = asset.guid == selectedGuid_;

        // 行が選択されたら、右側で参照元を表示するGUIDを差し替える。
        if (ImGui::Selectable(asset.sourcePath.c_str(), selected)) {
            selectedGuid_ = asset.guid;
        }

        // アセットの行にカーソルがある場合だけ、種類とGUIDを表示する。
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s\nGUID: %s", AssetDatabase::GetAssetTypeName(asset.type), asset.guid.c_str());
        }
    }

    ImGui::EndChild();
}

void AssetReferenceExplorer::DrawUsageList(const std::vector<Usage>& usages, UsageKind kind) const {
    // 参照元とJSON Pointerを併記し、同名Assetのどの設定か追えるようにします。
    std::int32_t count = 0;

    for (const Usage& usage : usages) {
        // 指定した参照種別と異なる項目は、この一覧に出さない。
        if (usage.kind != kind)
            continue;
        ++count;
        ImGui::BulletText("[%s] %s", usage.ownerCategory.c_str(), usage.ownerPath.c_str());
        ImGui::Indent();
        ImGui::TextDisabled("%s = %s", usage.jsonPointer.c_str(), usage.value.c_str());
        ImGui::Unindent();
    }

    // 該当する参照種別が一件も無ければ、空の状態を表示する。
    if (count == 0)
        ImGui::TextDisabled("該当なし");
}

void AssetReferenceExplorer::DrawSelectedAssetDetails() {
    // GUIDを索引へ引き直し、更新で消えたAssetの古いポインタは使いません。
    const EditorAssetRecord* asset = AssetDatabase::GetInstance()->FindByGuid(selectedGuid_);

    // 現在の索引でGUIDを解決できないときは、詳細を描かず選択を案内する。
    if (!asset) {
        ImGui::TextDisabled("左の一覧からAssetを選択してください。");
        return;
    }

    ImGui::TextUnformatted(asset->sourcePath.c_str());
    ImGui::TextDisabled("GUID: %s  /  Type: %s", asset->guid.c_str(), AssetDatabase::GetAssetTypeName(asset->type));
    const auto usageIterator = usagesByGuid_.find(asset->guid);
    const std::vector<Usage> empty;
    const std::vector<Usage>& usages = usageIterator == usagesByGuid_.end() ? empty : usageIterator->second;
    const std::int32_t directCount = static_cast<std::int32_t>(std::count_if(usages.begin(), usages.end(), [](const Usage& usage) { return usage.kind == UsageKind::kDirect; }));
    const std::int32_t dynamicCount = static_cast<std::int32_t>(usages.size()) - directCount;

    // 確定参照も動的参照候補もない場合に、未使用の候補として表示する。
    if (directCount == 0 && dynamicCount == 0) {
        ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.2f, 1.0f), "未使用候補（確定参照・動的参照候補ともになし）");
    // 確定参照が無くても動的な候補がある場合は、その可能性を表示する。
    } else if (directCount == 0) {
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "動的参照の可能性あり。未使用とは断定しません。");
    }

    ImGui::Separator();

    // 編集中の名前は毎フレーム戻さず、選択した GUID が変わったときだけ初期化する。
    if (renameBufferGuid_ != asset->guid) {
        renameBufferGuid_ = asset->guid;
        const std::string filename = fs::path(asset->sourcePath).filename().generic_string();
        std::snprintf(renameBuffer_, sizeof(renameBuffer_), "%s", filename.c_str());
    }

    ImGui::SetNextItemWidth(280.0f);
    ImGui::InputText("ファイル名", renameBuffer_, sizeof(renameBuffer_));
    ImGui::SameLine();

    // 名前変更のボタンが押されたら、選択アセットのパスを指定して変更を試みる。
    if (ImGui::Button("GUIDを維持して名前変更")) {
        std::string error;
        const std::string previousPath = asset->sourcePath;

        // 名前変更後のファイルを次回の索引から選び直す。参照の検出も新しいパスでやり直す。
        if (AssetDatabase::GetInstance()->RenameAsset(previousPath, renameBuffer_, &error)) {
            statusMessage_ = "名前を変更しました。.metaのGUIDは維持されます。Path参照は一覧で確認してください。";
            requestedAssetPath_ = (fs::path(previousPath).parent_path() / renameBuffer_).generic_string();
            rescanRequested_ = true;
        // 変更が失敗した場合は、選択と再走査状態を変えず理由を表示する。
        } else {
            statusMessage_ = "名前変更失敗: " + error;
        }
    }

    ImGui::SameLine();
    // ゴミ箱ボタンで直接移動せず、影響を表示する確認を予約する。
    if (ImGui::Button("ゴミ箱へ移動"))
        requestDeleteConfirm_ = true;

    // 確定参照の欄が開いているときだけ、確定した参照元を描画する。
    if (ImGui::CollapsingHeader(("確定参照  " + std::to_string(directCount)).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawUsageList(usages, UsageKind::kDirect);
    }

    // 動的参照候補の欄が開いているときだけ、該当する候補を描画する。
    if (ImGui::CollapsingHeader(("動的参照候補  " + std::to_string(dynamicCount)).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawUsageList(usages, UsageKind::kDynamicCandidate);
    }

    // 確認が予約されたフレームに、削除の影響を示すモーダルを開く。
    if (requestDeleteConfirm_) {
        ImGui::OpenPopup("Asset削除の影響確認");
        requestDeleteConfirm_ = false;
    }

    // 確認モーダルが表示中の場合だけ、選択アセットの影響と決定を描く。
    if (ImGui::BeginPopupModal("Asset削除の影響確認", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", asset->sourcePath.c_str());
        ImGui::Text("確定参照: %d 件 / 動的参照候補: %d 件", directCount, dynamicCount);

        // 確定参照が一件以上ある場合は、退避によって参照が解決できなくなる旨を示す。
        if (directCount > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "削除するとScene・Prefab・Effectの参照がMissingになります。");
        }

        // 確認の決定が押された場合にだけ、ファイルを復元可能なゴミ箱へ移す。
        if (ImGui::Button("影響を理解してゴミ箱へ移動")) {
            std::string recovered;
            std::string error;

            // 退避が成功したときにだけ選択を解除し、次回描画で索引を作り直す。
            if (AssetDatabase::GetInstance()->MoveAssetToTrash(asset->sourcePath, &recovered, &error)) {
                statusMessage_ = "Assetを復元可能なゴミ箱へ移動しました: " + recovered;
                selectedGuid_.clear();
                rescanRequested_ = true;
                ImGui::CloseCurrentPopup();
            // 退避に失敗した場合は選択を残し、エラーを表示する。
            } else {
                statusMessage_ = "削除失敗: " + error;
            }
        }

        ImGui::SameLine();
        // キャンセルの入力では、確認を閉じるだけでファイルを動かさない。
        if (ImGui::Button("キャンセル"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void AssetReferenceExplorer::DrawSelectedObjectReferences() {
    // 選択Objectをその場でJSON化して、保存前のAsset参照も確認します。
    engine::Object3D* object = editor_ ? editor_->GetSelectedObject() : nullptr;

    // 選択中のObjectが無ければ、現在の編集値を参照検査できないため選択を案内する。
    if (!object) {
        ImGui::TextDisabled("HierarchyまたはGameViewでObjectを選択してください。");
        return;
    }

    std::unordered_map<std::string, std::vector<Usage>> objectUsages;
    // 選択中Objectは確認用に走査し、ディスク上の未解決参照一覧には加えない。
    VisitJson(object->ExportToJson(), "Object: " + object->GetName(), "", "", objectUsages, false);
    ImGui::Text("%s が参照しているAsset", object->GetName().c_str());
    std::int32_t count = 0;

    for (const auto& [guid, usages] : objectUsages) {
        const EditorAssetRecord* asset = AssetDatabase::GetInstance()->FindByGuid(guid);
        // GUIDを現在の索引から解決できたものだけを、選択物の参照一覧に載せる。
        if (!asset)
            continue;
        ++count;
        // 参照先が選択されたら、アセットの詳細表示にも同じGUIDを渡す。
        if (ImGui::Selectable(asset->sourcePath.c_str()))
            selectedGuid_ = guid;
        for (const Usage& usage : usages) {
            ImGui::Indent();
            ImGui::TextDisabled("%s = %s", usage.jsonPointer.c_str(), usage.value.c_str());
            ImGui::Unindent();
        }
    }

    // 一覧へ出せる参照が無かった場合は、その旨を表示する。
    if (count == 0)
        ImGui::TextDisabled("Asset Databaseで解決できる参照はありません。");
}

void AssetReferenceExplorer::DrawMissingReferences() {
    // 未解決候補には名前参照も含むため、修復は自動一括ではなく個別に提示します。
    if (missingReferences_.empty()) {
        ImGui::TextDisabled("Missing参照候補はありません。");
        return;
    }

    ImGui::TextWrapped("Path・GUIDとして解決できない参照候補です。名前だけを使う独自設定も含むため、候補を確認してから1件ずつ修復してください。");
    ImGui::BeginChild("MissingReferenceList", ImVec2(0.0f, 0.0f), true);

    for (std::size_t index = 0; index < missingReferences_.size(); ++index) {
        MissingReference& missing = missingReferences_[index];
        ImGui::PushID(static_cast<std::int32_t>(index));
        ImGui::SeparatorText(missing.ownerPath.c_str());
        ImGui::TextWrapped("%s = %s", missing.jsonPointer.c_str(), missing.value.c_str());

        // 近いアセット候補が無ければ、修復を選ぶ入力欄を出さない。
        if (missing.candidateGuids.empty()) {
            ImGui::TextDisabled("近い修復候補なし");
        // 候補がある場合は、そのGUIDから現在の表示名を引き直す。
        } else {
            // 候補名を描画する直前に GUID を引き直し、更新済みの索引から消えた候補を無理に表示しない。
            const EditorAssetRecord* preview = AssetDatabase::GetInstance()->FindByGuid(missing.candidateGuids[missing.selectedCandidate]);

            // 候補のコンボが表示中の場合だけ、現在も選べる候補を並べる。
            if (ImGui::BeginCombo("修復候補", preview ? preview->sourcePath.c_str() : "候補なし")) {
                for (std::int32_t candidateIndex = 0; candidateIndex < static_cast<std::int32_t>(missing.candidateGuids.size()); ++candidateIndex) {
                    const EditorAssetRecord* candidate = AssetDatabase::GetInstance()->FindByGuid(missing.candidateGuids[candidateIndex]);
                    // 索引から消えた候補は、一覧へ出さない。
                    if (!candidate)
                        continue;
                    const bool selected = candidateIndex == missing.selectedCandidate;
                    // 候補を選んだら、修復要求が参照する候補番号を保持する。
                    if (ImGui::Selectable(candidate->sourcePath.c_str(), selected))
                        missing.selectedCandidate = candidateIndex;
                }

                ImGui::EndCombo();
            }

            // 確定が押されたら、選択した候補でこの参照一件の修復を依頼する。
            if (ImGui::Button("この候補で修復")) {
                RepairMissingReference(missing, statusMessage_);
            }
        }

        ImGui::PopID();
    }

    ImGui::EndChild();
}

void AssetReferenceExplorer::Draw() {
    // ウィンドウを閉じている間は、索引と参照一覧を更新しない。
    if (!open_)
        return;
    AssetDatabase* database = AssetDatabase::GetInstance();
    // 再走査要求がある場合や索引世代が変わった場合に、参照を集め直す。
    if (rescanRequested_ || indexedGeneration_ != database->GetGeneration())
        RefreshIndex();
    // パス指定で開いた場合は、索引の更新確認の後に対象を選択する。
    if (!requestedAssetPath_.empty()) {
        SelectAssetByPath(requestedAssetPath_);
        requestedAssetPath_.clear();
    }

    ImGui::SetNextWindowSize(ImVec2(1180.0f, 720.0f), ImGuiCond_FirstUseEver);

    // ウィンドウが折り畳みなどで描けない場合も、Beginに対応してEndする。
    if (!ImGui::Begin("Asset参照エクスプローラー", &open_)) {
        ImGui::End();
        return;
    }

    // 最初の索引を構築中なら、途中の参照一覧を出さず、進行状態だけを表示する。
    if (database->IsInitialIndexBuildInProgress()) {
        ImGui::TextDisabled("Asset Databaseの初期索引を作成中です。");
        ImGui::End();
        return;
    }

    // 再走査ボタンを押されたら、次の描画で索引を調べ直すよう予約する。
    if (ImGui::Button("参照を再スキャン"))
        rescanRequested_ = true;
    ImGui::SameLine();
    ImGui::TextDisabled("%s", statusMessage_.c_str());
    ImGui::Separator();

    // タブバーを描ける場合だけ、それぞれの参照画面を表示する。
    if (ImGui::BeginTabBar("AssetReferenceTabs")) {
        // 参照元タブを選択中なら、一覧と選択アセットの参照元を左右に並べる。
        if (ImGui::BeginTabItem("Assetの参照元")) {
            DrawAssetBrowser();
            ImGui::SameLine();
            ImGui::BeginChild("AssetReferenceDetails", ImVec2(0.0f, 0.0f), true);
            DrawSelectedAssetDetails();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        // 選択Objectのタブを開いている場合は、現在選択している実体の参照を表示する。
        if (ImGui::BeginTabItem("選択ObjectのAsset")) {
            DrawSelectedObjectReferences();
            ImGui::EndTabItem();
        }

        // 未解決参照タブを開いている場合は、件数と個別の修復候補を表示する。
        if (ImGui::BeginTabItem(("Missing参照 " + std::to_string(missingReferences_.size())).c_str())) {
            DrawMissingReferences();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}

#endif
