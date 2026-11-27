#include "engine/editor/scene/inspector/InspectorWindow.h"
#include "DebugEditor.h"
#include "EditorObjectFactoryRegistry.h"
#include "SceneManager.h"
#include "BaseScene.h"
#include "Object3D.h"
#ifdef USE_IMGUI
#include "imgui.h"
#endif

// 敵の種類を選び、登録されている型のモデル・衝突・能力の初期値を反映します。
void InspectorWindow::DrawEnemyTypeSelector() {
#ifdef USE_IMGUI
    auto* object = editor_->GetSelectedObject();
    auto* scene = editor_->GetSceneManager() ? editor_->GetSceneManager()->GetCurrentScene() : nullptr;
    // 対象またはシーンが無ければ、既定値を作成して反映できない。
    if (!object || !scene) return;
    using Registry = EditorObjectFactoryRegistry;
    const auto& types = Registry::GetTypes(Registry::Kind::kEnemy);
    const std::string current = object->GetEnemyType();
    std::string label = current.empty() ? "(未設定)" : current;
    // 現在のIDが登録されていれば、候補欄の表示名に差し替える。
    for (const auto& type : types) if (type.id == current) label = type.displayName;
    // 種類一覧を開いた場合だけ、登録済みの候補を描く。
    if (ImGui::BeginCombo("敵の種類", label.c_str())) {
        for (const auto& type : types) {
            // 候補が押されていないか同じ型を選んだ場合は、現在の設定を作り直さない。
            if (!ImGui::Selectable(type.displayName.c_str(), current == type.id) || current == type.id) continue;
            // 実際のFactoryから表示・Collider・基本値を取得する。配置位置と親子関係は保つ。
            auto defaults = Registry::Create(Registry::Kind::kEnemy, type.id, scene->GetObject3DCommon());
            // 既定実体を作れないか指定型と一致しなければ、現在の実体を上書きしない。
            if (!defaults || defaults->GetEnemyType() != type.id) continue;
            // プレビューのセッションがある場合は、参照している旧設定を変更する前に停止する。
            if (auto* session = editor_->GetActorSession()) session->Stop();
            object->SetEnemyType(type.id);
            object->SetName("Enemy_" + type.id);
            object->SetModel(defaults->GetModelName());
            object->SetColor(defaults->GetColor());
            object->SetMaterialType(defaults->GetMaterialType());
            object->SetEmissive(defaults->GetEmissive());
            object->SetScale(defaults->GetScale());
            object->SetColliderConfig(defaults->GetColliderConfig());
            object->SetCollisionAttribute(defaults->GetCollisionAttribute());
            object->SetCollisionMask(defaults->GetCollisionMask());
            object->SetParameters(defaults->GetParameters());
            object->SetAnimationName(defaults->GetAnimationName()); object->SetAnimationLoop(defaults->IsAnimationLoop());
        }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("種類と初期値はゲーム側の登録を使用します。動作は下の調整欄で確認できます。");
#endif
}
