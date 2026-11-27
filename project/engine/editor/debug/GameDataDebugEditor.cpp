#define NOMINMAX
#include <cstdint>
#include "GameDataDebugEditor.h"

#include "DebugConsole.h"
#include "game/system/progress/GameDataManager.h"
#include "game/scene/select/GameSelectScene.h"
#include "IconsFontAwesome5.h"
#include "SceneManager.h"
#include "game/system/progress/StageManager.h"
#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace {
std::string FormatTime(std::int32_t totalSeconds) {
    // セーブデータの経過秒数を画面表示用の時分秒へ整える。
    totalSeconds = std::max(totalSeconds, 0);
    const std::int32_t hours = totalSeconds / 3600;
    const std::int32_t minutes = (totalSeconds / 60) % 60;
    const std::int32_t seconds = totalSeconds % 60;

    char buffer[32]{};

    // 一時間以上なら、分・秒の前に時間も表示する。
    if (hours > 0) {
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", hours, minutes, seconds);
    // 一時間未満は分と秒だけを表示する。
    } else {
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d", minutes, seconds);
    }

    return buffer;
}

// 初期解放と前段ステージのクリア状態から、指定スロットで入れるか判定します。
bool IsStageUnlockedForSave(const StageData& stage, std::int32_t stageIndex, GameDataManager* save) {
    // セーブ管理器がなければ、現在のスロットの解放状態を判断できない。
    if (!save) {
        return false;
    }

    // 初期解放の設定があるか先頭ステージなら、前提のクリア状態を参照しない。
    if (stage.defaultUnlocked || stageIndex == 0) {
        return true;
    }

    // 初期解放されず前提ステージ番号も無効なら、解放しない。
    if (stage.unlockStageIndex < 0) {
        return false;
    }

    return save->IsStageCleared(stage.unlockStageIndex);
}
} // namespace

void GameDataDebugEditor::Initialize(SceneManager* sceneManager) {
    // セーブスロットとStage定義を先に同期し、編集対象を現在の実データへ向けます。
    sceneManager_ = sceneManager;
    StageManager::GetInstance()->Initialize();
    SyncSlotState();
}

void GameDataDebugEditor::DrawImGui() {
#ifdef USE_IMGUI
    // 画面を開くたびに現在のスロットを読み直し、外部の切り替えを反映します。
    SyncSlotState();

    ImGui::Text(ICON_FA_DATABASE " ゲーム内部データ編集");
    ImGui::TextWrapped("デバッグ用に、選択中のセーブスロットとステージ進行状態を直接編集します。変更は保存JSONへ即時反映されます。");
    ImGui::Separator();

    DrawSlotSelector();
    ImGui::Separator();
    DrawSlotSummary();
    ImGui::Separator();
    DrawCurrentSlotEditor();
    ImGui::Separator();
    DrawStageTable();

    ImGui::Separator();
    ImGui::TextWrapped("%s", lastStatus_.c_str());
#endif
}

void GameDataDebugEditor::SyncSlotState() {
    // 外部のセーブスロット切替に追従し、古いスロットの表示を残しません。
    GameDataManager* save = GameDataManager::GetInstance();
    const std::int32_t activeSlot = save->GetActiveSlot();

    // 同じスロットを編集中は入力した王冠数を保持し、編集対象が切り替わったときだけ同期します。
    if (lastActiveSlot_ != activeSlot) {
        lastActiveSlot_ = activeSlot;
        crownTarget_ = save->GetClearedStageCount();
    }
}

void GameDataDebugEditor::RefreshStageSelectScene() {
    // Stage Select表示中だけ、変更済みの進行状態を画面へ反映します。
    if (!sceneManager_ || !sceneManager_->GetCurrentScene()) {
        return;
    }

    // 現在のシーンがステージ選択の場合だけ、表示中の進行を再評価する。
    if (auto* selectScene = dynamic_cast<GameSelectScene*>(sceneManager_->GetCurrentScene())) {
        selectScene->RefreshDebugStageStates();
    }
}

void GameDataDebugEditor::DrawSlotSelector() {
#ifdef USE_IMGUI
    // 編集先スロットを明示し、別セーブへの誤操作を避けます。
    GameDataManager* save = GameDataManager::GetInstance();
    std::int32_t activeSlot = save->GetActiveSlot();

    ImGui::Text("編集対象スロット");

    for (std::int32_t slot = 0; slot < GameDataManager::kSaveSlotCount; ++slot) {
        ImGui::PushID(slot);
        const std::string label = "Slot " + std::to_string(slot + 1);

        // ラジオボタンを押したときだけ、セーブの編集先をその枠へ切り替える。
        if (ImGui::RadioButton(label.c_str(), activeSlot == slot)) {
            save->SetActiveSlot(slot);
            SyncSlotState();
            RefreshStageSelectScene();
            lastStatus_ = "編集対象を Slot " + std::to_string(slot + 1) + " に切り替えました。";
        }

        // 次の枠がある間は、ラジオボタンを同じ行に並べる。
        if (slot + 1 < GameDataManager::kSaveSlotCount) {
            ImGui::SameLine();
        }

        ImGui::PopID();
    }

    ImGui::TextDisabled("保存先: %s", save->GetSaveFilePath().c_str());

    // 保存ボタンでは、現在のスロットのデータを書き出す。
    if (ImGui::Button(ICON_FA_SAVE " 保存")) {
        save->Save();
        lastStatus_ = "現在のスロットを保存しました。";
    }

    ImGui::SameLine();

    // 再読み込みを押すとディスクの内容から読み直し、ステージ表示も更新する。
    if (ImGui::Button(ICON_FA_SYNC " 再読み込み")) {
        save->Load();
        SyncSlotState();
        RefreshStageSelectScene();
        lastStatus_ = "現在のスロットを保存JSONから再読み込みしました。";
    }

    ImGui::SameLine();

    // 初期化を押した時点では値を変更せず、確認を開く。
    if (ImGui::Button(ICON_FA_TRASH " 初期化")) {
        ImGui::OpenPopup("ResetActiveSaveSlot");
    }

    ImGui::SameLine();

    // 削除を押した時点ではファイルに触れず、削除の確認を開く。
    if (ImGui::Button(ICON_FA_TIMES_CIRCLE " 削除")) {
        ImGui::OpenPopup("DeleteActiveSaveSlot");
    }

    // 初期化は保存ファイルを残して値を戻す操作。下の削除確認とは経路を分ける。
    if (ImGui::BeginPopupModal("ResetActiveSaveSlot", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("現在のスロットを初期状態に戻します。");
        ImGui::TextDisabled("ファイルは残り、値だけリセットされます。");

        // 初期化を確定してから、現在の枠の内容を既定値へ戻す。
        if (ImGui::Button("初期化する", ImVec2(140.0f, 0.0f))) {
            save->ResetAll();
            SyncSlotState();
            RefreshStageSelectScene();
            lastStatus_ = "現在のスロットを初期化しました。";
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        // 取り消した場合は、セーブを変更せず初期化の確認を閉じる。
        if (ImGui::Button("キャンセル", ImVec2(120.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    // 削除の確認が開いている間だけ、消去と取消の操作を出す。
    if (ImGui::BeginPopupModal("DeleteActiveSaveSlot", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("現在のスロットの保存ファイルを削除します。");
        ImGui::TextDisabled("削除後は初期状態として扱われます。");

        // 削除を確定した場合に、その時点の有効スロットを削除する。
        if (ImGui::Button("削除する", ImVec2(140.0f, 0.0f))) {
            const std::int32_t slot = save->GetActiveSlot();
            const bool removed = save->DeleteSlot(slot);
            SyncSlotState();
            RefreshStageSelectScene();
            lastStatus_ = removed ? "保存ファイルを削除しました。" : "保存ファイルは見つかりませんでした。初期状態で続行します。";
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        // 取り消した場合は、ファイルを残して削除の確認を閉じる。
        if (ImGui::Button("キャンセル", ImVec2(120.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
#endif
}

void GameDataDebugEditor::DrawSlotSummary() {
#ifdef USE_IMGUI
    // 全スロットを一表で比較し、現在の編集先と保存済み値を確認できます。
    GameDataManager* save = GameDataManager::GetInstance();

    // 全枠の比較表を表示できる場合だけ、列を設定して要約を並べる。
    if (ImGui::BeginTable("GameDataSlotSummary", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 52.0f);
        ImGui::TableSetupColumn("状態", ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn("残機", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("コイン", ImGuiTableColumnFlags_WidthFixed, 64.0f);
        ImGui::TableSetupColumn("王冠", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("スター", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("プレイ時間", ImGuiTableColumnFlags_WidthFixed, 92.0f);
        ImGui::TableSetupColumn("チュートリアル", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (std::int32_t slot = 0; slot < GameDataManager::kSaveSlotCount; ++slot) {
            const auto summary = save->GetSlotSummary(slot);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Slot %d", slot + 1);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(summary.exists ? "あり" : "なし");
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d", summary.lives);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%d", summary.coins);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%d", summary.clearedStageCount);
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%d", summary.collectedStarCoins);
            ImGui::TableSetColumnIndex(6);
            ImGui::TextUnformatted(FormatTime(summary.playTimeSeconds).c_str());
            ImGui::TableSetColumnIndex(7);
            ImGui::TextUnformatted(summary.tutorialCleared ? "完了" : "未完了");
        }

        ImGui::EndTable();
    }
#endif
}

void GameDataDebugEditor::DrawCurrentSlotEditor() {
#ifdef USE_IMGUI
    // 現在スロットの値だけを変更し、入力ごとにセーブ管理へ反映します。
    GameDataManager* save = GameDataManager::GetInstance();

    ImGui::Text("現在スロットの基本値");

    std::int32_t lives = save->GetLives();
    ImGui::SetNextItemWidth(140.0f);

    // 残機欄の値が変わった場合は、この枠の残機へ反映する。
    if (ImGui::DragInt("残機", &lives, 1.0f, 0, 99)) {
        save->SetLives(lives);
        lastStatus_ = "残機を変更しました。";
    }

    std::int32_t coins = save->GetCoins();
    ImGui::SetNextItemWidth(140.0f);

    // コイン欄の値が変わった場合は、この枠の所持数へ反映する。
    if (ImGui::DragInt("コイン", &coins, 1.0f, 0, 9999)) {
        save->SetCoins(coins);
        lastStatus_ = "コイン数を変更しました。";
    }

    // 保存形式の秒数を時分秒へ分け、いずれかが編集されたフレームだけ合計秒数へ戻す。
    std::int32_t totalSeconds = save->GetPlayTimeSeconds();
    std::int32_t hours = totalSeconds / 3600;
    std::int32_t minutes = (totalSeconds / 60) % 60;
    std::int32_t seconds = totalSeconds % 60;
    bool timeChanged = false;
    ImGui::SetNextItemWidth(90.0f);
    timeChanged |= ImGui::DragInt("時", &hours, 1.0f, 0, 99);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    timeChanged |= ImGui::DragInt("分", &minutes, 1.0f, 0, 59);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    timeChanged |= ImGui::DragInt("秒", &seconds, 1.0f, 0, 59);

    // 時・分・秒のいずれかを変えた場合、各範囲に収めたうえで合計秒に直して保存側へ渡す。
    if (timeChanged) {
        hours = std::clamp(hours, 0, 99);
        minutes = std::clamp(minutes, 0, 59);
        seconds = std::clamp(seconds, 0, 59);
        save->SetPlayTimeSeconds(hours * 3600 + minutes * 60 + seconds);
        lastStatus_ = "プレイ時間を変更しました。";
    }

    // 進行管理では通常ステージと同じ入口を使い、チュートリアルは予約番号−1で参照します。
    bool tutorialCleared = save->IsStageCleared(-1);

    // チュートリアル完了の選択を変更した場合だけ、進行を保存側とステージ選択へ反映する。
    if (ImGui::Checkbox("チュートリアル完了", &tutorialCleared)) {
        save->SetStageCleared(-1, tutorialCleared);
        RefreshStageSelectScene();
        lastStatus_ = "チュートリアル進行状態を変更しました。";
    }

    // 王冠数の入力は適用ボタンまで保持し、確定時に先頭からのクリア状態へ変換する。
    const std::int32_t stageCount = static_cast<std::int32_t>(StageManager::GetInstance()->GetStages().size());
    ImGui::Text("王冠数: %d / %d", save->GetClearedStageCount(), stageCount);
    ImGui::SameLine();

    // 現在値へ同期を押した場合は、未適用の入力欄を実際に取得した王冠数へ戻す。
    if (ImGui::Button("現在値へ同期")) {
        crownTarget_ = save->GetClearedStageCount();
    }

    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragInt("王冠数を指定", &crownTarget_, 1.0f, 0, std::max(stageCount, 0));
    crownTarget_ = std::clamp(crownTarget_, 0, std::max(stageCount, 0));
    ImGui::SameLine();

    // 反映ボタンを押した段階で、入力した王冠数に合わせてステージのクリア状態を変更する。
    if (ImGui::Button("王冠数を反映")) {
        ApplyCrownCountToStages();
    }
#endif
}

void GameDataDebugEditor::DrawStageTable() {
#ifdef USE_IMGUI
    // Stage定義順にクリア・収集状態を並べ、進行デバッグを行います。
    GameDataManager* save = GameDataManager::GetInstance();
    const auto& stages = StageManager::GetInstance()->GetStages();

    // ステージ定義が空なら、一括編集と表を表示せず確認先を案内する。
    if (stages.empty()) {
        ImGui::TextDisabled("ステージ定義がありません。Resources/json/stage_select/stages.json を確認してください。");
        return;
    }

    // 一括クリア操作は各ステージの保存値を変え、選択Sceneの解放状態も再評価する。
    if (ImGui::Button("全ステージクリア")) {
        for (std::int32_t stageIndex = 0; stageIndex < static_cast<std::int32_t>(stages.size()); ++stageIndex) {
            save->SetStageCleared(stageIndex, true);
        }

        SyncSlotState();
        RefreshStageSelectScene();
        lastStatus_ = "全ステージをクリア済みにしました。";
    }

    ImGui::SameLine();

    // 全未クリアが押された場合は、通常ステージをすべて未クリアに戻して解放表示も再評価する。
    if (ImGui::Button("全ステージ未クリア")) {
        for (std::int32_t stageIndex = 0; stageIndex < static_cast<std::int32_t>(stages.size()); ++stageIndex) {
            save->SetStageCleared(stageIndex, false);
        }

        SyncSlotState();
        RefreshStageSelectScene();
        lastStatus_ = "全ステージを未クリアに戻しました。";
    }

    ImGui::SameLine();

    // スターはクリア状態と独立した保存値として更新し、ここでは解放状態の再評価を呼ばない。
    if (ImGui::Button("全スター取得")) {
        for (std::int32_t stageIndex = 0; stageIndex < static_cast<std::int32_t>(stages.size()); ++stageIndex) {
            for (std::int32_t coinIndex = 0; coinIndex < 3; ++coinIndex) {
                save->SetStarCoinCollected(stageIndex, coinIndex, true);
            }
        }

        lastStatus_ = "全スターを取得済みにしました。";
    }

    ImGui::SameLine();

    // 全未取得が押された場合、クリア状態を維持してスターの取得フラグだけを戻す。
    if (ImGui::Button("全スター未取得")) {
        for (std::int32_t stageIndex = 0; stageIndex < static_cast<std::int32_t>(stages.size()); ++stageIndex) {
            for (std::int32_t coinIndex = 0; coinIndex < 3; ++coinIndex) {
                save->SetStarCoinCollected(stageIndex, coinIndex, false);
            }
        }

        lastStatus_ = "全スターを未取得に戻しました。";
    }

    // 表を表示できる場合だけ、定義順にステージの操作欄を配置する。
    if (ImGui::BeginTable("GameDataStageTable", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 360.0f))) {
        ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed, 52.0f);
        ImGui::TableSetupColumn("ステージ", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("解放条件", ImGuiTableColumnFlags_WidthFixed, 86.0f);
        ImGui::TableSetupColumn("解放中", ImGuiTableColumnFlags_WidthFixed, 64.0f);
        ImGui::TableSetupColumn("クリア", ImGuiTableColumnFlags_WidthFixed, 64.0f);
        ImGui::TableSetupColumn("解放演出済み", ImGuiTableColumnFlags_WidthFixed, 104.0f);
        ImGui::TableSetupColumn("スター", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableHeadersRow();

        for (std::int32_t stageIndex = 0; stageIndex < static_cast<std::int32_t>(stages.size()); ++stageIndex) {
            const StageData& stage = stages[stageIndex];
            // 解放中かどうかは直接保存せず、ステージ定義と前提ステージのクリア値から表示する。
            const bool unlocked = IsStageUnlockedForSave(stage, stageIndex, save);

            ImGui::PushID(stageIndex);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", stageIndex);

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(stage.name.c_str());

            // ステージに説明が設定されている場合だけ、名前の下に表示する。
            if (!stage.description.empty()) {
                ImGui::TextDisabled("%s", stage.description.c_str());
            }

            ImGui::TextDisabled("%s", stage.levelPath.c_str());

            ImGui::TableSetColumnIndex(2);

            // 初期解放の設定があるか定義上の先頭なら、前提番号の代わりに初期解放と表示する。
            if (stage.defaultUnlocked || stageIndex == 0) {
                ImGui::TextUnformatted("初期解放");
            // 初期解放されない場合は、進行に必要な前提ステージ番号を表示する。
            } else {
                ImGui::Text("Stage %d", stage.unlockStageIndex);
            }

            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(unlocked ? "はい" : "いいえ");

            ImGui::TableSetColumnIndex(4);
            bool cleared = save->IsStageCleared(stageIndex);

            // この行のクリア状態を変えた場合は、保存値を変更して解放状態を再評価する。
            if (ImGui::Checkbox("##cleared", &cleared)) {
                save->SetStageCleared(stageIndex, cleared);
                SyncSlotState();
                RefreshStageSelectScene();
                lastStatus_ = stage.name + " のクリア状態を変更しました。";
            }

            // 解放演出を見たかどうかはクリア値と別に変更でき、選択Sceneの演出状態も更新する。
            ImGui::TableSetColumnIndex(5);
            bool seen = save->IsStageUnlockSeen(stageIndex);

            // この行の解放演出済み状態を変えた場合は、そのフラグを保存して現在シーンの表示を更新する。
            if (ImGui::Checkbox("##seen", &seen)) {
                save->SetStageUnlockSeen(stageIndex, seen);
                RefreshStageSelectScene();
                lastStatus_ = stage.name + " の解放演出済み状態を変更しました。";
            }

            ImGui::TableSetColumnIndex(6);

            // ステージIDの内側にスター番号のIDを重ね、同じラベルのチェック欄を別々に識別する。
            for (std::int32_t coinIndex = 0; coinIndex < 3; ++coinIndex) {
                ImGui::PushID(coinIndex);
                bool collected = save->IsStarCoinCollected(stageIndex, coinIndex);

                // 各スターの個別欄が変わった場合だけ、そのステージと番号の取得状態を書き換える。
                if (ImGui::Checkbox("##star", &collected)) {
                    save->SetStarCoinCollected(stageIndex, coinIndex, collected);
                    lastStatus_ = stage.name + " のスター取得状態を変更しました。";
                }

                // 三つのスターのうち、次の項目がある場合は同じ行に並べる。
                if (coinIndex < 2) {
                    ImGui::SameLine();
                }

                ImGui::PopID();
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }
#endif
}

void GameDataDebugEditor::ApplyCrownCountToStages() {
    // 指定した王冠数に合わせ、先頭ステージから順にクリア状態を更新します。
    GameDataManager* save = GameDataManager::GetInstance();
    const auto& stages = StageManager::GetInstance()->GetStages();
    const std::int32_t target = std::clamp(crownTarget_, 0, static_cast<std::int32_t>(stages.size()));

    for (std::int32_t stageIndex = 0; stageIndex < static_cast<std::int32_t>(stages.size()); ++stageIndex) {
        save->SetStageCleared(stageIndex, stageIndex < target);
    }

    // 入力欄を適用後の範囲内の値へ揃え、セーブの要約とステージ選択の表示を更新します。
    crownTarget_ = target;
    SyncSlotState();
    RefreshStageSelectScene();
    lastStatus_ = "王冠数を " + std::to_string(target) + " に合わせました。先頭ステージから順にクリア扱いにしています。";
    DebugConsole::GetInstance()->AddLog("GameData Debug: crown count applied.");
}
