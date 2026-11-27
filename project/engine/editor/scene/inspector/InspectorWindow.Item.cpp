#include <cstdint>
#include "engine/editor/scene/inspector/InspectorWindow.h"
#include "CollisionConfig.h"
#include "SceneManager.h"
#include "imgui.h"

#include <string>

// JSONのカテゴリ一覧で既知ビットを編集し、未登録ビットは変更せず保持します。
void InspectorWindow::DrawAttributeSelector(const char* label, uint32_t* attribute) {
#ifdef USE_IMGUI
    // カテゴリのツリーが開いた場合だけ、定義済みのビットの編集欄を描く。
    if (ImGui::TreeNode(label)) {
        // 定義が正常に読み込めていなければ、代替カテゴリで表示していることを知らせる。
        if (!engine::collisionCategories::IsDefinitionLoaded()) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
                "カテゴリ定義JSONを読めません。既存カテゴリで一時的に動作しています。");
        }

        // 現在値を出発点にして、一覧で扱うカテゴリのビットだけを編集する。
        std::int32_t flags = static_cast<std::int32_t>(*attribute);

        for (const auto& category : engine::collisionCategories::GetDefinitions()) {
            const std::string itemLabel = category.displayName + " (" + category.id + ")##" + category.id;
            ImGui::CheckboxFlags(itemLabel.c_str(), &flags, static_cast<std::int32_t>(category.bit));
        }

        *attribute = static_cast<uint32_t>(flags);
        // 追加ボタンの押下で、新規定義の入力欄を開く。
        if (ImGui::Button("カテゴリを追加"))
            ImGui::OpenPopup("AddCollisionCategory");
        // ポップアップが開いている間だけ、入力と保存操作を受け付ける。
        if (ImGui::BeginPopup("AddCollisionCategory")) {
            static char sIdBuffer[64]{};
            static char sNameBuffer[128]{};
            static std::string sError;
            ImGui::InputText("ID (英小文字)", sIdBuffer, sizeof(sIdBuffer));
            ImGui::InputText("表示名", sNameBuffer, sizeof(sNameBuffer));
            const bool isPlaying = SceneManager::GetInstance() && SceneManager::GetInstance()->IsPlaying();

            // Play中はJSONを書き換えず、停止後に新しいカテゴリを登録させる。
            ImGui::BeginDisabled(isPlaying);

            // プレイ停止中に保存ボタンを押した場合、入力された定義の登録を試す。
            if (ImGui::Button("追加してJSONへ保存")) {
                // 登録と保存に成功した場合だけ入力を消してポップアップを閉じる。
                if (engine::collisionCategories::Add(sIdBuffer, sNameBuffer, &sError)) {
                    sIdBuffer[0] = sNameBuffer[0] = '\0';
                    ImGui::CloseCurrentPopup();
                }
            }

            ImGui::EndDisabled();
            // 実行中は保存が無効な理由を表示する。
            if (isPlaying)
                ImGui::TextDisabled("Playを停止してから追加してください。");
            // 保存処理からエラーが返った場合は、同じ入力欄で内容を表示する。
            if (!sError.empty())
                ImGui::TextWrapped("%s", sError.c_str());
            ImGui::EndPopup();
        }

        ImGui::TreePop();
    }
#endif
}
