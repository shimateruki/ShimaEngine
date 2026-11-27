#include "engine/editor/scene/inspector/InspectorWindow.h"
#include "DebugEditor.h"
#include "Object3D.h"
#include "EditorObjectFactoryRegistry.h"
#include "imgui.h"

void InspectorWindow::DrawSpawnerSettings() {
    // スポナーの種別と発生条件を、選択中オブジェクトのパラメータへ反映します。
#ifdef USE_IMGUI
    engine::Object3D* selectedObject = editor_->GetSelectedObject();
    // 選択中のオブジェクトが無ければ、表示・更新する設定がない。
    if (!selectedObject)
        return;

    ImGui::Separator();
    ImGui::Indent();
    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "[ Spawner Config ]");

    // 既存の設定が無ければ、スポナーのパラメーターを書き込める状態にする。
    if (!selectedObject->GetParameters().has_value()) {
        selectedObject->ResetParameters();
    }

    // 編集用コピーへ変更を集め、発生間隔の秒数や最大数と一緒に選択Objectへ書き戻します。
    auto p = selectedObject->GetParameters().value();

    using Registry = EditorObjectFactoryRegistry;
    const auto types = Registry::GetTypes(Registry::Kind::kEnemy, Registry::TypeList::kSpawner);
    // 選択中IDは候補外でもそのまま表示し、開いただけで保存値を置き換えない。
    std::string preview = p.enemyType.empty() ? "(未設定)" : p.enemyType;
    for (const auto& type : Registry::GetTypes(Registry::Kind::kEnemy)) {
        // 登録IDと一致する定義があれば、表示に使う名前だけを取得する。
        if (type.id == p.enemyType) { preview = type.displayName; break; }
    }
    // 種類の一覧を開いている場合だけ、発生対象を選ぶ行を描く。
    if (ImGui::BeginCombo("Spawn Type", preview.c_str())) {
        for (const auto* type : types) {
            const bool selected = p.enemyType == type->id;
            ImGui::PushID(type->id.c_str());
            // クリックした候補のIDを編集中の値にする。
            if (ImGui::Selectable(type->displayName.c_str(), selected))
                p.enemyType = type->id;
            // 現在の候補には、一覧を開いたときのフォーカスを合わせる。
            if (selected) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    ImGui::DragFloat("Interval (sec)", &p.interval, 0.1f, 0.1f, 60.0f, "%.1f s");
    ImGui::InputInt("Max Count", &p.maxCount);

    selectedObject->SetParameters(p);
    ImGui::Unindent();
#endif
}

