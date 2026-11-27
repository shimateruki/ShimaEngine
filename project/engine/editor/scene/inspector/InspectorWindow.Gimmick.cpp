#include "engine/editor/scene/inspector/InspectorWindow.h"
#include "DebugEditor.h"
#include "Object3D.h"
#include "SceneManager.h"
#ifdef USE_IMGUI
#include "imgui.h"
#include <algorithm>
#endif

// 選択したギミックの種類を表示し、選択が変われば型交換を予約します。
void InspectorWindow::DrawGimmickTypeSelector() {
#ifdef USE_IMGUI
    auto* object = editor_->GetSelectedObject();
    // 対象が選ばれていなければ、型交換の欄を描かない。
    if (!object) return;
    using Registry = EditorObjectFactoryRegistry;
    const auto& types = Registry::GetTypes(Registry::Kind::kGimmick);
    const auto current = std::find_if(types.begin(), types.end(), [&](const auto& type) {
        return type.id == object->GetGimmickType();
    });
    const std::string preview = current != types.end() ? current->displayName : object->GetGimmickType();
    // ゲームの実行中は既存インスタンスが使われているため、型交換の操作を停止中に限ります。
    ImGui::BeginDisabled(editor_->GetSceneManager()->IsPlaying());
    // 登録された種類に実クラスの照合処理があり、現在の実体と一致しなければ補正を案内する。
    if (current != types.end() && current->matches && !current->matches(*object)) {
        ImGui::TextDisabled("種類名と実クラスが一致していません。");
        // 実クラスを更新するボタンが押された場合は、保存済みの種類で作り直す要求を出す。
        if (ImGui::Button("保存値を保って実クラスを更新"))
            editor_->RequestObjectTypeChange(object, Registry::Kind::kGimmick, current->id);
    }
    // 種類一覧を開いている間だけ、登録済みのギミックを列挙する。
    if (ImGui::BeginCombo("ギミックの種類", preview.c_str())) {
        for (const auto& type : types) {
            // 候補を選んだ時点では交換を予約する。描画中の実体の参照は Editor が次の更新まで保持する。
            if (ImGui::Selectable(type.displayName.c_str(), type.id == object->GetGimmickType()))
                editor_->RequestObjectTypeChange(object, Registry::Kind::kGimmick, type.id);
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
#endif
}
