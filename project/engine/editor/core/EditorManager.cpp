#include "EditorManager.h"
#include <imgui.h>

void EditorManager::DrawInspector() {
#ifdef USE_IMGUI


    // 選択中の編集対象に応じた入力欄を、共通のInspectorに表示する。
    ImGui::Begin("Inspector");

    // 編集対象が選択されている間は、表示名と区切りに続けてその対象の入力欄を描く。
    if (selectedObject_ != nullptr) {
        ImGui::Text("Selected: %s", selectedObject_->GetName().c_str());
        ImGui::Separator();
        ImGui::Spacing();

        // 入力欄の内容はIEditableの実装側が決める。
        selectedObject_->DrawImGui();

    } else {
        // 何も選択していない場合は入力欄を描かず、選択が必要なことを表示する。
        ImGui::Text("オブジェクトが選択されていません");
    }

    ImGui::End();
#endif // USE_IMGUI

}