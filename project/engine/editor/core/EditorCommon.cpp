#include "EditorCommon.h"
#include "Object3D.h"
#include "json.hpp"
#include <numbers>

void editorCommon::WriteObjectParentReference(nlohmann::json& data, const engine::Object3D& object) {
    // 親を持つ場合は表示名と永続GUIDを記録する。親がいなければ両方を空にして、以前の参照を残さない。
    auto* parent = object.GetParent();
    data["parentName"] = parent ? parent->GetName() : "";
    data["parentGuid"] = parent ? parent->EnsurePersistentGuid() : "";
}
#ifdef USE_IMGUI
#include "imgui.h"
#include "ImGuizmo.h"

void editorCommon::DecomposeWorldPose(const engine::Matrix4x4& world, engine::Vector3& position,
    engine::Vector3& rotation, engine::Vector3& scale) {
    float degrees[3]{};
    ImGuizmo::DecomposeMatrixToComponents(&world.m[0][0], &position.x, degrees, &scale.x);
    // ImGuizmo の分解結果は度数法。エンジンの Transform に渡せるよう回転だけラジアンへ変換する。
    constexpr float kDegreesToRadians = std::numbers::pi_v<float> / 180.0f;
    rotation = {degrees[0] * kDegreesToRadians, degrees[1] * kDegreesToRadians, degrees[2] * kDegreesToRadians};
}
#endif
