#include <cstdint>
#include "SceneAssetPanel.h"

#include "DebugEditor.h"
#include "SceneController.h"
#include "SceneManager.h"
#include "SceneSerializer.h"

#ifdef USE_IMGUI
#include "EditorCommandRegistry.h"
#include "IconsFontAwesome5.h"
#include "imgui.h"
#endif

#include <algorithm>
#include <cstring>
#include <iterator>
#include <vector>

// 一覧操作と保存要求を委譲するDebugEditorを借用する。
void SceneAssetPanel::Initialize(DebugEditor* editor) {
    editor_ = editor;
}

void SceneAssetPanel::OpenCreateSceneDialog() {
    // 新規Sceneには専用Editorクラスを初期候補とし、誤った実行クラスを選びにくくします。
    sceneAssetStatusMessage_.clear();
    sceneAssetStatusIsError_ = false;

    // 編集機能が使える場合は、登録済みクラスのうちSCENE_EDITORを優先する。
    if (editor_) {
        const std::vector<std::string> runtimeScenes = editor_->GetRegisteredSceneNames();
        const auto editorScene = std::find(runtimeScenes.begin(), runtimeScenes.end(), "SCENE_EDITOR");
        createRuntimeSceneIndex_ = editorScene != runtimeScenes.end()
            ? static_cast<std::int32_t>(std::distance(runtimeScenes.begin(), editorScene))
            : 0;
    }

    requestCreateScenePopup_ = true;
}

void SceneAssetPanel::DrawList() {
#ifdef USE_IMGUI
    // 保存済みSceneと現在編集中のSceneを対比し、作成・複製・開く操作をまとめます。
    if (!editor_ || !ImGui::CollapsingHeader(
        ICON_FA_SAVE " Scene Asset",
        ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    const std::vector<SceneSerializer::SceneAssetInfo> assets = editor_->GetSceneAssets();
    const std::string currentFilename = editor_->GetCurrentSceneFilenameBuffer();
    const bool sceneTransitioning = editor_->GetSceneManager() && editor_->GetSceneManager()->IsTransitioning();

    const auto findAsset = [&](const std::string& filename) {
        return std::find_if(assets.begin(), assets.end(), [&](const SceneSerializer::SceneAssetInfo& asset) {
            return asset.filename == filename;
        });
    };

    // 選択Assetが消えた場合は編集中のScene、なければ先頭の候補へ選択を戻す。
    if (selectedSceneAssetFilename_.empty() || findAsset(selectedSceneAssetFilename_) == assets.end()) {
        // 現在編集中のファイルが一覧に残っていれば、その行を選ぶ。
        if (findAsset(currentFilename) != assets.end()) {
            selectedSceneAssetFilename_ = currentFilename;
        }
        // 現在のファイルも見つからなければ、一覧の先頭を選ぶ。
        else if (!assets.empty()) {
            selectedSceneAssetFilename_ = assets.front().filename;
        }
    }

    std::string selectedLabel = "Scene Assetがありません";
    const auto selectedAsset = findAsset(selectedSceneAssetFilename_);

    // 選択先を解決できた場合だけ、名前とIDを選択欄のラベルに使う。
    if (selectedAsset != assets.end()) {
        selectedLabel = selectedAsset->displayName + "  (" + selectedAsset->id + ")";
    }

    const bool selectedAssetIsActive = selectedAsset != assets.end() &&
        editor_->IsCurrentSceneAsset(selectedSceneAssetFilename_);

    // 実行設定を変更しただけでは現在のRuntimeへ反映されないため、再読込の必要性を示す。
    bool selectedRuntimeNeedsReload = false;

    // 選んだAssetと実行中のAssetが同じ場合に、保存設定と実行時の差を調べる。
    if (selectedAssetIsActive && editor_->GetSceneManager()) {
        const SceneLoadContext& activeContext = editor_->GetSceneManager()->GetActiveSceneLoadContext();
        selectedRuntimeNeedsReload =
            selectedAsset->runtimeScene != activeContext.runtimeScene ||
            selectedAsset->controllerName != activeContext.controllerName ||
            selectedAsset->bgmPath != activeContext.bgmPath ||
            selectedAsset->lightPath != activeContext.lightPath ||
            selectedAsset->cameraPath != activeContext.cameraPath ||
            selectedAsset->skyboxPath != activeContext.skyboxPath;
    }

    ImGui::TextColored(
        ImVec4(0.42f, 0.78f, 1.0f, 1.0f),
        ICON_FA_CIRCLE " Active: %s%s",
        currentFilename.c_str(),
        editor_->HasAnyDirty() ? " *" : "");

    // 一覧上の選択と実行中の Scene を分け、ダブルクリック時だけ読み込みを要求する。
    // 一覧が開いた間だけ、保存済みのAssetと現在実行中の印を描く。
    if (ImGui::BeginCombo(ICON_FA_FOLDER_OPEN " Scene一覧", selectedLabel.c_str())) {
        for (const SceneSerializer::SceneAssetInfo& asset : assets) {
            const bool isSelected = asset.filename == selectedSceneAssetFilename_;
            const bool isActive = editor_->IsCurrentSceneAsset(asset.filename);
            std::string label = isActive ? ICON_FA_CIRCLE " " : "   ";
            label += asset.displayName + "##" + asset.filename;

            // 行を選んだら操作対象を更新する。
            if (ImGui::Selectable(label.c_str(), isSelected)) {
                selectedSceneAssetFilename_ = asset.filename;

                // 実行中でない行を左ダブルクリックした場合だけ、クリックと同時に開く要求を出す。
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !isActive) {
                    RequestOpen(asset.filename);
                }
            }

            // 現在選択中の行を、リストを開いたときのフォーカス位置にする。
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }

            // 行にポインターが重なった場合だけ、実行クラスや配置ファイルの情報を出す。
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "%s\n実行クラス: %s\nController: %s\nObject: %s\nSprite: %s\nダブルクリックで開く",
                    asset.filename.c_str(),
                    asset.runtimeScene.c_str(),
                    asset.controllerName.c_str(),
                    asset.usesSplitFiles ? "分割JSON" : "単一JSON",
                    asset.hasSpriteLayout ? asset.spriteLayoutPath.c_str() : "なし");
            }
        }

        ImGui::EndCombo();
    }

    // JSON AssetとC++の実行Sceneクラスの対応を、この選択行から変更できる。
    if (selectedAsset != assets.end()) {
        const std::vector<std::string> runtimeScenes = editor_->GetRegisteredSceneNames();
        ImGui::BeginDisabled(sceneTransitioning || runtimeScenes.empty());

        // 実行クラス欄が開いたときにSceneFactoryに登録された候補を並べる。
        if (ImGui::BeginCombo("実行クラス", selectedAsset->runtimeScene.c_str())) {
            for (const std::string& runtimeScene : runtimeScenes) {
                const bool isSelected = runtimeScene == selectedAsset->runtimeScene;

                // 現在値と違うクラスを選んだ場合だけAssetの保存設定を書き換える。
                if (ImGui::Selectable(runtimeScene.c_str(), isSelected) && !isSelected) {
                    std::string errorMessage;
                    // 対応するクラスを保存できた場合は、選んだ名前を結果に表示する。
                    if (editor_->SetSceneAssetRuntimeScene(
                        selectedAsset->filename,
                        runtimeScene,
                        errorMessage)) {
                        sceneAssetStatusMessage_ = "実行クラスを変更しました: " + runtimeScene;
                        sceneAssetStatusIsError_ = false;
                    }
                    // 保存に失敗したら、DebugEditorから返された理由をエラー表示にする。
                    else {
                        sceneAssetStatusMessage_ = errorMessage;
                        sceneAssetStatusIsError_ = true;
                    }
                }

                // 現在設定されているクラスを、開いた一覧のフォーカス先にする。
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }

            ImGui::EndCombo();
        }

        ImGui::EndDisabled();
        ImGui::TextDisabled("Sceneを開くと、このC++ SceneクラスとAssetのJSONを同時に読み込みます。");

        // 実行設定を旧形式から解決しているAssetには、保存時に形式が変わることを示す。
        if (selectedAsset->usesLegacyMetadata) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.72f, 0.2f, 1.0f),
                ICON_FA_EXCLAMATION_TRIANGLE " 旧形式Scene。実行設定の保存でScene Asset形式へ更新します。");
        }

        // 実行設定はModalを開く前に現在値をバッファへ複写し、キャンセルなら保存しない。
        if (ImGui::Button(ICON_FA_COG " 実行設定", ImVec2(122.0f, 0.0f))) {
            const std::vector<std::string>& controllerNames =
                SceneControllerFactory::GetInstance()->GetRegisteredNames();
            const auto controller = std::find(
                controllerNames.begin(),
                controllerNames.end(),
                selectedAsset->controllerName);
            runtimeSettingsControllerIndex_ = controller != controllerNames.end()
                ? static_cast<std::int32_t>(std::distance(controllerNames.begin(), controller))
                : 0;
            strcpy_s(runtimeBgmPath_, sizeof(runtimeBgmPath_), selectedAsset->bgmPath.c_str());
            strcpy_s(runtimeLightPath_, sizeof(runtimeLightPath_), selectedAsset->lightPath.c_str());
            strcpy_s(runtimeCameraPath_, sizeof(runtimeCameraPath_), selectedAsset->cameraPath.c_str());
            strcpy_s(runtimeSkyboxPath_, sizeof(runtimeSkyboxPath_), selectedAsset->skyboxPath.c_str());
            requestRuntimeSettingsPopup_ = true;
        }

        ImGui::SameLine();

        // 検証ボタンでは選択中のファイルとその設定をチェックする。
        if (ImGui::Button(ICON_FA_CHECK_CIRCLE " 検証", ImVec2(102.0f, 0.0f))) {
            const SceneSerializer::SceneAssetValidationResult validation =
                editor_->ValidateSceneAsset(selectedAsset->filename);
            // エラーがあれば警告より優先して先頭の一件を出す。
            if (!validation.errors.empty()) {
                sceneAssetStatusMessage_ = "エラー: " + validation.errors.front();
                sceneAssetStatusIsError_ = true;
            }
            // エラーがなく警告だけあれば、先頭の警告を表示する。
            else if (!validation.warnings.empty()) {
                sceneAssetStatusMessage_ = "警告: " + validation.warnings.front();
                sceneAssetStatusIsError_ = false;
            }
            // 両方空の場合は検証成功を表示する。
            else {
                sceneAssetStatusMessage_ = "Scene Assetの検証に成功しました。";
                sceneAssetStatusIsError_ = false;
            }
        }
    }

    // 遷移中の作成・開く・複製・改名・削除は、Scene状態が変わるため無効化する。
    ImGui::BeginDisabled(sceneTransitioning);

    // 新規ボタンが押された場合は作成ダイアログを予約する。
    if (ImGui::Button(ICON_FA_PLUS " 新規", ImVec2(84.0f, 0.0f))) {
        OpenCreateSceneDialog();
    }

    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(sceneTransitioning || selectedAsset == assets.end() ||
        (selectedAssetIsActive && !selectedRuntimeNeedsReload));
    // 有効な「開く」または「再読込」ボタンから、未保存確認を含む共通経路へ進む。
    if (ImGui::Button(
        selectedRuntimeNeedsReload ? ICON_FA_SYNC " 再読込" : ICON_FA_FOLDER_OPEN " 開く",
        ImVec2(84.0f, 0.0f))) {
        RequestOpen(selectedSceneAssetFilename_);
    }

    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(sceneTransitioning || selectedAsset == assets.end());

    // 複製する場合は元のIDに接尾辞を付けた候補と、元の表示名をダイアログに用意する。
    if (ImGui::Button(ICON_FA_COPY " 複製", ImVec2(84.0f, 0.0f))) {
        sceneAssetStatusMessage_.clear();
        sceneAssetStatusIsError_ = false;
        const std::string duplicateId = selectedAsset->id + "_copy";
        strcpy_s(duplicateSceneId_, sizeof(duplicateSceneId_), duplicateId.c_str());
        strcpy_s(duplicateSceneDisplayName_, sizeof(duplicateSceneDisplayName_), selectedAsset->displayName.c_str());
        requestDuplicateScenePopup_ = true;
    }

    ImGui::EndDisabled();

    ImGui::BeginDisabled(sceneTransitioning || selectedAsset == assets.end());

    // 改名する場合は、選択中のIDと表示名を編集バッファにコピーする。
    if (ImGui::Button(ICON_FA_EDIT " 名前変更", ImVec2(122.0f, 0.0f))) {
        sceneAssetStatusMessage_.clear();
        sceneAssetStatusIsError_ = false;
        strcpy_s(renameSceneId_, sizeof(renameSceneId_), selectedAsset->id.c_str());
        strcpy_s(renameSceneDisplayName_, sizeof(renameSceneDisplayName_), selectedAsset->displayName.c_str());
        requestRenameScenePopup_ = true;
    }

    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool canDelete = selectedAsset != assets.end() && !editor_->IsCurrentSceneAsset(selectedSceneAssetFilename_);
    ImGui::BeginDisabled(sceneTransitioning || !canDelete);

    // 削除ボタンでは、実際に消す前に確認モーダルを開く。
    if (ImGui::Button(ICON_FA_TRASH_ALT " 削除", ImVec2(102.0f, 0.0f))) {
        sceneAssetStatusMessage_.clear();
        sceneAssetStatusIsError_ = false;
        requestDeleteScenePopup_ = true;
    }

    ImGui::EndDisabled();

    // 実行中のAssetに対する無効な削除ボタンへポインターを置いたとき、制限の理由を表示する。
    if (!canDelete &&
        selectedAsset != assets.end() &&
        editor_->IsCurrentSceneAsset(selectedSceneAssetFilename_) &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Active Sceneは削除できません。別のSceneを開いてください。");
    }

    // 処理から受け取った表示文がある場合は状態に応じた色で出す。
    if (!sceneAssetStatusMessage_.empty()) {
        ImGui::TextColored(
            sceneAssetStatusIsError_ ? ImVec4(1.0f, 0.38f, 0.32f, 1.0f) : ImVec4(0.45f, 1.0f, 0.58f, 1.0f),
            "%s",
            sceneAssetStatusMessage_.c_str());
    }

    // 保存単位ごとの操作とActive Scene全体の保存を、Dirty概要の下へ並べる。
    ImGui::SeparatorText("保存");

    // 未保存の変更がある場合は保存対象の概要を警告色で表示する。
    if (editor_->HasAnyDirty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.2f, 1.0f), ICON_FA_EXCLAMATION_TRIANGLE " %s", editor_->GetDirtySummaryText().c_str());
    }
    // 未保存の変更がなければ同じ位置に保存済みの状態を示す。
    else {
        ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.55f, 1.0f), ICON_FA_CHECK_CIRCLE " %s", editor_->GetDirtySummaryText().c_str());
    }

    ImGui::BeginDisabled(sceneTransitioning);
    // Playerを押した場合はプレイヤー配置だけを保存する。
    if (ImGui::Button(ICON_FA_USER " Player"))
        editor_->SaveScene(SaveMode::kPlayer);
    ImGui::SameLine();
    // Enemyを押した場合は敵配置だけを保存する。
    if (ImGui::Button(ICON_FA_SKULL " Enemy"))
        editor_->SaveScene(SaveMode::kEnemy);
    ImGui::SameLine();
    // Objectを押した場合はオブジェクト配置だけを保存する。
    if (ImGui::Button(ICON_FA_CUBE " Object"))
        editor_->SaveScene(SaveMode::kObject);
    ImGui::SameLine();
    // Cameraを押した場合はカメラ区分だけを保存する。
    if (ImGui::Button(ICON_FA_VIDEO " Camera"))
        editor_->SaveScene(SaveMode::kCamera);
    // 全体保存を押した場合は、メニュー等と共通のシーン保存コマンドを呼ぶ。
    if (ImGui::Button(ICON_FA_DOWNLOAD " Active Sceneを保存", ImVec2(-1.0f, 0.0f))) {
        EditorCommandRegistry::GetInstance()->Execute(editorCommandId::kSceneSave);
    }

    ImGui::EndDisabled();
    ImGui::TextDisabled("保存先: Resources/json/3Dobject/%s", currentFilename.c_str());
#endif
}

void SceneAssetPanel::RequestOpen(const std::string& filename) {
    // 現在のAssetを再指定した場合も、実行クラスの整合を確認して開きます。
    // 編集機能または対象ファイル名がなければ切替を要求できない。
    if (!editor_ || filename.empty()) {
        return;
    }

    // 現在のAssetを開き直す場合に限り、実行構成が保存内容と異なるか検査する。
    if (editor_->IsCurrentSceneAsset(filename)) {
        const std::vector<SceneSerializer::SceneAssetInfo> assets = editor_->GetSceneAssets();
        const auto asset = std::find_if(assets.begin(), assets.end(), [&](const SceneSerializer::SceneAssetInfo& candidate) {
            return candidate.filename == filename;
        });
        SceneManager* sceneManager = editor_->GetSceneManager();

        // 保存データか実行側の構成を参照できない場合は、現在のシーンを開き直さない。
        if (asset == assets.end() || !sceneManager) {
            return;
        }

        const SceneLoadContext& activeContext = sceneManager->GetActiveSceneLoadContext();
        const bool settingsMatch =
            asset->runtimeScene == activeContext.runtimeScene &&
            asset->controllerName == activeContext.controllerName &&
            asset->bgmPath == activeContext.bgmPath &&
            asset->lightPath == activeContext.lightPath &&
            asset->cameraPath == activeContext.cameraPath &&
            asset->skyboxPath == activeContext.skyboxPath;
        // 実行設定が同じ現在の Scene を選び直しても、再読み込みや保存確認を起こさない。
        if (settingsMatch)
            return;
    }

    // 現在のシーンに未保存変更がある場合は、切替を保留してモーダルで扱う。
    if (editor_->HasAnyDirty()) {
        // 切替前に編集中の変更を扱う必要があるため、ここではファイル名だけ保留する。
        pendingOpenSceneAssetFilename_ = filename;
        requestUnsavedOpenPopup_ = true;
        return;
    }

    std::string errorMessage;

    // 未保存確認を経ずに開ける場合は直接要求し、受付失敗の理由を表示する。
    if (!editor_->OpenSceneAsset(filename, false, errorMessage)) {
        sceneAssetStatusMessage_ = errorMessage;
        sceneAssetStatusIsError_ = true;
    }
    // 要求が受理された場合は、切替が進行中であることを示す。
    else {
        sceneAssetStatusMessage_ = "Sceneを開いています: " + filename;
        sceneAssetStatusIsError_ = false;
    }
}

void SceneAssetPanel::DrawDialogs() {
#ifdef USE_IMGUI
    // 作成・変更・削除の確認をモーダルに集約し、確定前のSceneを保護します。
    // 編集機能が未設定のときは各ダイアログの操作先がないため処理しない。
    if (!editor_) {
        return;
    }

    // メニューやボタンで立てた要求フラグを、一フレームだけOpenPopupへ渡す。
    // 新規作成の要求を消費して、作成モーダルを開く。
    if (requestCreateScenePopup_) {
        ImGui::OpenPopup("新規Scene Asset###CreateSceneAsset");
        requestCreateScenePopup_ = false;
    }

    // 複製の要求を消費して、複製元と作成先を確認する。
    if (requestDuplicateScenePopup_) {
        ImGui::OpenPopup("Scene Assetを複製###DuplicateSceneAsset");
        requestDuplicateScenePopup_ = false;
    }

    // 改名の要求を消費して、編集欄を表示する。
    if (requestRenameScenePopup_) {
        ImGui::OpenPopup("Scene Assetの名前変更###RenameSceneAsset");
        requestRenameScenePopup_ = false;
    }

    // 削除の要求を消費して、対象の確認を表示する。
    if (requestDeleteScenePopup_) {
        ImGui::OpenPopup("Scene Assetを削除###DeleteSceneAsset");
        requestDeleteScenePopup_ = false;
    }

    // 未保存のまま別のAssetを要求した場合は、扱いを選ぶモーダルを開く。
    if (requestUnsavedOpenPopup_) {
        ImGui::OpenPopup("未保存のScene変更###OpenSceneAssetUnsaved");
        requestUnsavedOpenPopup_ = false;
    }

    // 実行設定を編集する要求が来たら、準備済みの入力バッファを表示する。
    if (requestRuntimeSettingsPopup_) {
        ImGui::OpenPopup("Scene Asset実行設定###SceneAssetRuntimeSettings");
        requestRuntimeSettingsPopup_ = false;
    }

    // 実行設定はControllerと各参照ファイルをまとめて保存する。
    ImGui::SetNextWindowSize(ImVec2(620.0f, 0.0f), ImGuiCond_Appearing);
    // 実行設定モーダルが描画できる場合はControllerと参照ファイルを編集する。
    if (ImGui::BeginPopupModal(
        "Scene Asset実行設定###SceneAssetRuntimeSettings",
        nullptr,
        ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::vector<std::string>& controllerNames =
            SceneControllerFactory::GetInstance()->GetRegisteredNames();
        // 使えるControllerが登録済みなら、選択位置を補正して候補を提示する。
        if (!controllerNames.empty()) {
            // 一覧が変わっても古い選択位置で範囲外を参照しないよう、表示前に補正する。
            runtimeSettingsControllerIndex_ = std::clamp(
                runtimeSettingsControllerIndex_,
                0,
                static_cast<std::int32_t>(controllerNames.size()) - 1);
            // Controllerの選択欄が開いている間だけ登録名を並べる。
            if (ImGui::BeginCombo(
                "Scene Controller",
                controllerNames[runtimeSettingsControllerIndex_].c_str())) {
                for (std::int32_t index = 0; index < static_cast<std::int32_t>(controllerNames.size()); ++index) {
                    const bool isSelected = index == runtimeSettingsControllerIndex_;

                    // 選んだControllerの位置を保存操作用に覚える。
                    if (ImGui::Selectable(controllerNames[index].c_str(), isSelected)) {
                        runtimeSettingsControllerIndex_ = index;
                    }

                    // 現在のControllerを開いた一覧で最初にフォーカスする。
                    if (isSelected)
                        ImGui::SetItemDefaultFocus();
                }

                ImGui::EndCombo();
            }
        }

        ImGui::InputText("BGM", runtimeBgmPath_, sizeof(runtimeBgmPath_));
        ImGui::InputText("Light JSON", runtimeLightPath_, sizeof(runtimeLightPath_));
        ImGui::InputText("Camera JSON", runtimeCameraPath_, sizeof(runtimeCameraPath_));
        ImGui::InputText("Skybox", runtimeSkyboxPath_, sizeof(runtimeSkyboxPath_));
        ImGui::TextDisabled("空欄は選択したC++ Sceneクラスの既定値を使用します。");
        ImGui::TextDisabled("Camera JSONはResources/json/camera以下のファイル名を指定します。");
        ImGui::Separator();

        ImGui::BeginDisabled(controllerNames.empty());

        // 有効な保存ボタンを押した場合に、モーダルにある設定を一括で書き出す。
        if (ImGui::Button(ICON_FA_SAVE " 保存", ImVec2(120.0f, 0.0f))) {
            std::string errorMessage;
            // 選択中Assetの設定が保存できた場合だけ、成功を知らせて閉じる。
            if (editor_->SetSceneAssetRuntimeSettings(
                selectedSceneAssetFilename_,
                controllerNames[runtimeSettingsControllerIndex_],
                runtimeBgmPath_,
                runtimeLightPath_,
                runtimeCameraPath_,
                runtimeSkyboxPath_,
                errorMessage)) {
                sceneAssetStatusMessage_ = "Scene Assetの実行設定を保存しました。";
                sceneAssetStatusIsError_ = false;
                ImGui::CloseCurrentPopup();
            }
            // 保存を拒否された場合は、理由を表示して入力を残す。
            else {
                sceneAssetStatusMessage_ = errorMessage;
                sceneAssetStatusIsError_ = true;
            }
        }

        ImGui::EndDisabled();
        ImGui::SameLine();

        // キャンセル時は設定を書き込まず閉じる。
        if (ImGui::Button("キャンセル", ImVec2(100.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    // 新規Assetは空Sceneか現在Sceneの複製を選び、登録済みの実行クラスへ紐付ける。
    ImGui::SetNextWindowSize(ImVec2(480.0f, 0.0f), ImGuiCond_Appearing);

    // 新規作成モーダルが描画できる場合だけ、IDとテンプレートを編集する。
    if (ImGui::BeginPopupModal("新規Scene Asset###CreateSceneAsset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Scene IDはファイル名に使用します。表示名には日本語を使用できます。");
        ImGui::InputText("Scene ID", createSceneId_, sizeof(createSceneId_));
        ImGui::InputText("表示名", createSceneDisplayName_, sizeof(createSceneDisplayName_));
        const char* templates[] = { "空のScene", "現在のSceneを複製" };
        ImGui::Combo("テンプレート", &createSceneTemplate_, templates, IM_ARRAYSIZE(templates));
        const std::vector<std::string> runtimeScenes = editor_->GetRegisteredSceneNames();

        // 実行クラスが登録済みなら選択位置を補正して候補を出す。
        if (!runtimeScenes.empty()) {
            createRuntimeSceneIndex_ = std::clamp(
                createRuntimeSceneIndex_,
                0,
                static_cast<std::int32_t>(runtimeScenes.size()) - 1);
            // 実行クラス欄が開いている間だけ登録名を並べる。
            if (ImGui::BeginCombo("実行クラス", runtimeScenes[createRuntimeSceneIndex_].c_str())) {
                for (std::int32_t index = 0; index < static_cast<std::int32_t>(runtimeScenes.size()); ++index) {
                    const bool isSelected = index == createRuntimeSceneIndex_;

                    // 選んだ実行クラスを作成時の紐付け先にする。
                    if (ImGui::Selectable(runtimeScenes[index].c_str(), isSelected)) {
                        createRuntimeSceneIndex_ = index;
                    }

                    // 現在選択しているクラスへ初期フォーカスを合わせる。
                    if (isSelected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }

                ImGui::EndCombo();
            }
        }
        // 登録クラスがない場合は作成に必要な依存が不足していることを示す。
        else {
            ImGui::TextColored(ImVec4(1.0f, 0.38f, 0.32f, 1.0f), "登録済みの実行クラスがありません。");
        }

        // 現在のSceneを複製する選択では、メモリ上の内容を複製することを明示する。
        if (createSceneTemplate_ == 1) {
            ImGui::TextDisabled("現在のメモリ上のObjectと関連Spriteレイアウトを複製します。");
        }

        // 作成が失敗してエラー文がある場合は、モーダル内に表示する。
        if (sceneAssetStatusIsError_ && !sceneAssetStatusMessage_.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.38f, 0.32f, 1.0f), "%s", sceneAssetStatusMessage_.c_str());
        }

        ImGui::Separator();

        // 作成のみと作成後に開く操作で同じ検証・生成経路を使用する。
        const auto createAsset = [&](bool openAfterCreate) {
            // 有効な実行クラスを指定できなければ作成を行わない。
            if (runtimeScenes.empty()) {
                sceneAssetStatusMessage_ = "SceneFactoryに実行クラスが登録されていません。";
                sceneAssetStatusIsError_ = true;
                return false;
            }

            std::string createdFilename;
            std::string errorMessage;
            const SceneSerializer::SceneAssetTemplate sceneTemplate = createSceneTemplate_ == 0
                ? SceneSerializer::SceneAssetTemplate::kEmpty
                : SceneSerializer::SceneAssetTemplate::kCurrentScene;
            // 検証または作成に失敗した場合は、理由を表示して選択を切り替えない。
            if (!editor_->CreateSceneAsset(
                createSceneId_,
                createSceneDisplayName_,
                runtimeScenes[createRuntimeSceneIndex_],
                sceneTemplate,
                createdFilename,
                errorMessage)) {
                sceneAssetStatusMessage_ = errorMessage;
                sceneAssetStatusIsError_ = true;
                return false;
            }

            selectedSceneAssetFilename_ = createdFilename;
            sceneAssetStatusMessage_ = "Scene Assetを作成しました: " + createdFilename;
            sceneAssetStatusIsError_ = false;

            // 作成後に開く指定なら、未保存の扱いも含む共通の切替要求へ渡す。
            if (openAfterCreate) {
                RequestOpen(createdFilename);
            }

            return true;
        };

        // 「作成して開く」を押したら、作成成功後に開く要求も出す。
        if (ImGui::Button(ICON_FA_FOLDER_OPEN " 作成して開く", ImVec2(150.0f, 0.0f))) {
            // 作成まで成功した場合に作成モーダルを閉じる。
            if (createAsset(true)) {
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::SameLine();

        // 「作成のみ」を押したら、作成したAssetを選ぶところまで進める。
        if (ImGui::Button(ICON_FA_PLUS " 作成のみ", ImVec2(120.0f, 0.0f))) {
            // 作成に成功した場合だけモーダルを閉じる。
            if (createAsset(false)) {
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::SameLine();

        // 入力を確定しない場合は作成要求を出さず閉じる。
        if (ImGui::Button("キャンセル", ImVec2(100.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    // 複製・改名は成功後に選択中Assetを新しいファイル名へ切り替える。
    ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Appearing);

    // 複製モーダルが開いている間だけ、新しいIDと表示名を受け付ける。
    if (ImGui::BeginPopupModal("Scene Assetを複製###DuplicateSceneAsset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("複製元: %s", selectedSceneAssetFilename_.c_str());
        ImGui::InputText("新しいScene ID", duplicateSceneId_, sizeof(duplicateSceneId_));
        ImGui::InputText("新しい表示名", duplicateSceneDisplayName_, sizeof(duplicateSceneDisplayName_));

        // 複製を実行できなかった理由がある場合は再編集できるようモーダル内に表示する。
        if (sceneAssetStatusIsError_ && !sceneAssetStatusMessage_.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.38f, 0.32f, 1.0f), "%s", sceneAssetStatusMessage_.c_str());
        }

        ImGui::Separator();

        // 複製ボタンを押した時点で、入力されたIDと名前を使って複製する。
        if (ImGui::Button(ICON_FA_COPY " 複製", ImVec2(120.0f, 0.0f))) {
            std::string createdFilename;
            std::string errorMessage;
            // 複製が成功したら返された新しいファイルを選び、モーダルを閉じる。
            if (editor_->DuplicateSceneAsset(
                selectedSceneAssetFilename_,
                duplicateSceneId_,
                duplicateSceneDisplayName_,
                createdFilename,
                errorMessage)) {
                selectedSceneAssetFilename_ = createdFilename;
                sceneAssetStatusMessage_ = "Scene Assetを複製しました: " + createdFilename;
                sceneAssetStatusIsError_ = false;
                ImGui::CloseCurrentPopup();
            }
            // 複製に失敗したら、元の選択と入力を保持して理由を出す。
            else {
                sceneAssetStatusMessage_ = errorMessage;
                sceneAssetStatusIsError_ = true;
            }
        }

        ImGui::SameLine();

        // キャンセル時は複製せず閉じる。
        if (ImGui::Button("キャンセル", ImVec2(100.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Appearing);

    // 改名モーダル内で、作業対象のファイル名と保存後の表示名を編集する。
    if (ImGui::BeginPopupModal("Scene Assetの名前変更###RenameSceneAsset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::string sourceFilename = selectedSceneAssetFilename_;
        ImGui::InputText("Scene ID", renameSceneId_, sizeof(renameSceneId_));
        ImGui::InputText("表示名", renameSceneDisplayName_, sizeof(renameSceneDisplayName_));
        ImGui::TextDisabled("Active Sceneを変更した場合は、以降の保存先も新しい名前へ切り替わります。");

        // 改名が失敗して理由を受け取った場合は、入力欄の下に出す。
        if (sceneAssetStatusIsError_ && !sceneAssetStatusMessage_.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.38f, 0.32f, 1.0f), "%s", sceneAssetStatusMessage_.c_str());
        }

        ImGui::Separator();

        // 確定ボタンで保存済みAssetのIDと表示名を変更する。
        if (ImGui::Button(ICON_FA_EDIT " 名前変更", ImVec2(130.0f, 0.0f))) {
            std::string renamedFilename;
            std::string errorMessage;
            // 更新が成功した場合は、選択を新しいファイルへ移す。
            if (editor_->RenameSceneAsset(
                sourceFilename,
                renameSceneId_,
                renameSceneDisplayName_,
                renamedFilename,
                errorMessage)) {
                selectedSceneAssetFilename_ = renamedFilename;
                sceneAssetStatusMessage_ = "Scene Assetの名前を変更しました: " + renamedFilename;
                sceneAssetStatusIsError_ = false;
                ImGui::CloseCurrentPopup();
            }
            // 更新に失敗した場合は、選択を変更せずエラーを表示する。
            else {
                sceneAssetStatusMessage_ = errorMessage;
                sceneAssetStatusIsError_ = true;
            }
        }

        ImGui::SameLine();

        // キャンセル時は改名せず閉じる。
        if (ImGui::Button("キャンセル", ImVec2(100.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    // 削除は関連JSONも対象となるため、対象名を示した専用Modalで確定する。
    ImGui::SetNextWindowSize(ImVec2(440.0f, 0.0f), ImGuiCond_Appearing);

    // 削除モーダルを表示する場合、先に対象と関連ファイルが削除されることを示す。
    if (ImGui::BeginPopupModal("Scene Assetを削除###DeleteSceneAsset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.32f, 1.0f), ICON_FA_EXCLAMATION_TRIANGLE " この操作は元に戻せません。");
        ImGui::TextWrapped("%s と関連する分割JSON、Spriteレイアウトを削除します。", selectedSceneAssetFilename_.c_str());

        // 削除が失敗した場合はモーダルに理由を残す。
        if (sceneAssetStatusIsError_ && !sceneAssetStatusMessage_.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.38f, 0.32f, 1.0f), "%s", sceneAssetStatusMessage_.c_str());
        }

        ImGui::Separator();

        // 「削除する」を押した場合にだけ、対象の削除をDebugEditorに委ねる。
        if (ImGui::Button(ICON_FA_TRASH_ALT " 削除する", ImVec2(130.0f, 0.0f))) {
            const std::string deletedFilename = selectedSceneAssetFilename_;
            std::string errorMessage;

            // 削除に成功した場合は、失われたファイルへの選択を解除する。
            if (editor_->DeleteSceneAsset(deletedFilename, errorMessage)) {
                selectedSceneAssetFilename_.clear();
                sceneAssetStatusMessage_ = "Scene Assetを削除しました: " + deletedFilename;
                sceneAssetStatusIsError_ = false;
                ImGui::CloseCurrentPopup();
            }
            // 削除に失敗した場合は、選択を残し理由を表示する。
            else {
                sceneAssetStatusMessage_ = errorMessage;
                sceneAssetStatusIsError_ = true;
            }
        }

        ImGui::SameLine();

        // キャンセル時は削除を実行せず閉じる。
        if (ImGui::Button("キャンセル", ImVec2(100.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    // 未保存Sceneを開き直すときは保存・破棄・取消の三択をここで確定する。
    ImGui::SetNextWindowSize(ImVec2(480.0f, 0.0f), ImGuiCond_Appearing);

    // 未保存確認が開いた場合は、保留中の切替先と未保存箇所を表示する。
    if (ImGui::BeginPopupModal("未保存のScene変更###OpenSceneAssetUnsaved", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.28f, 1.0f), ICON_FA_EXCLAMATION_TRIANGLE " 未保存の変更があります。");
        ImGui::TextWrapped("%s を開く前に、現在のSceneを保存しますか？", pendingOpenSceneAssetFilename_.c_str());
        ImGui::TextDisabled("%s", editor_->GetDirtySummaryText().c_str());
        ImGui::Separator();

        // 保存確認と後続の Scene 切り替えは DebugEditor に渡し、このダイアログでは保留要求を解除する。
        if (ImGui::Button(ICON_FA_SAVE " 保存して開く", ImVec2(150.0f, 0.0f))) {
            editor_->SaveThenOpenSceneAsset(pendingOpenSceneAssetFilename_);
            pendingOpenSceneAssetFilename_.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        // 変更を破棄する選択では、保留名を消費して保存なしの切替を要求する。
        if (ImGui::Button(ICON_FA_FOLDER_OPEN " 保存せず開く", ImVec2(150.0f, 0.0f))) {
            const std::string filename = pendingOpenSceneAssetFilename_;
            pendingOpenSceneAssetFilename_.clear();
            std::string errorMessage;

            // 保存なしで開く要求が失敗した場合は、一覧側に理由を残して閉じる。
            if (!editor_->OpenSceneAsset(filename, true, errorMessage)) {
                sceneAssetStatusMessage_ = errorMessage;
                sceneAssetStatusIsError_ = true;
            }

            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        // 取消を押した場合は保留名を破棄し、現在のシーンで編集を続ける。
        if (ImGui::Button("キャンセル", ImVec2(100.0f, 0.0f))) {
            pendingOpenSceneAssetFilename_.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
#endif
}
