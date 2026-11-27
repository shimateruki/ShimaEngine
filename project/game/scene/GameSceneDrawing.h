#pragma once

// ゲームシーンの特殊表示を共通の描画窓口へ接続する。シーンの進行状態や描画物は所有しない。

#include "BaseScene.h"
#include "BaseEnemy.h"
#include "Camera.h"
#include "RenderStats.h"
#include "engine/graphics/3d/material/MaterialRenderPolicy.h"
#include "engine/graphics/3d/light/SceneLighting.h"

#include <functional>

// カメラ近傍は表示を優先し、遠方だけ視錐台で間引きます。
inline bool IsSceneObjectVisibleByFrustum(engine::Object3D* object, engine::Camera* camera) {
    // 対象が存在しないか不可視に設定されている場合は、視錐台を調べる前に除外する。
    if (!object || !object->GetIsVisible()) {
        return false;
    }
    // 視点を取得できない場合は視錐台も作れないため、可視対象を描画候補に残す。
    if (!camera) {
        return true;
    }

    const engine::Aabb worldAabb = object->GetModelWorldAabb();
    constexpr float kNearObjectCullBypassDistance = 24.0f;
    // カメラから境界箱までが24単位以内なら、大きな近接モデルが視錐台端で消えないよう判定を省く。
    if (engine::Math::DistanceSquaredPointAabb(camera->GetEye(), worldAabb.min, worldAabb.max) <=
        kNearObjectCullBypassDistance * kNearObjectCullBypassDistance) {
        return true;
    }

    const bool visible = engine::Math::IntersectFrustumAabb(camera->GetFrustum(), worldAabb.min, worldAabb.max);
    // 遠方の境界箱が視錐台と交差しない場合は、統計にも間引きを記録する。
    if (!visible) {
        engine::RenderStats::GetInstance()->RecordCulledObject();
    }
    return visible;
}

// シーン固有の可視条件を適用し、指定された材質パスのオブジェクトを描く。
inline void DrawSceneObjectPass(const std::vector<std::unique_ptr<engine::Object3D>>& objects,
    materialRenderPolicy::Pass pass, const engine::SceneLighting& lighting,
    engine::Object3D* viewActor, bool isFirstPerson, bool hideDescendants,
    const std::function<bool(engine::Object3D*)>& filter = {}) {
    // 一人称視点では本人と必要に応じて子Objectを除き、材質別パスに振り分ける。
    for (const auto& object : objects) {
        // 対象なし、一時非表示、またはシーン固有の条件を満たさない場合は、付属パーツも含めて描画を呼ばない。
        if (!object || object->IsEditorTemporarilyHidden() || (filter && !filter(object.get()))) {
            continue;
        }

        // 一人称で視点を持つObjectを取得できた場合は、カメラに重なる本人や指定された子の描画を避ける。
        if (isFirstPerson && viewActor) {
            engine::Object3D* current = object.get();
            while (current) {
                // 本人に到達したら、その枝は非表示と確定する。子も対象の場合は、見つかるまで親をたどる。
                if (current == viewActor) {
                    break;
                }
                current = hideDescendants ? current->GetParent() : nullptr;
            }
            // 親の走査中に本人へ一致した場合だけ、選択中のObjectも描画から除く。
            if (current) {
                continue;
            }
        }

        // 材質の分類が現在の描画パスと一致する場合だけ、メッシュを描く。
        if (materialRenderPolicy::GetPass(object->GetMaterialType()) == pass) {
            lighting.Draw(*object, &engine::Object3D::Draw);
        }
    }
}

// ゲーム固有の敵の攻撃予告を、本体とは別の描画パスで共通のライトを使って描く。
inline void DrawEnemyTelegraphs(const BaseScene& scene, const engine::SceneLighting& lighting) {
    for (const auto& object : scene.GetObjects()) {
        // BaseEnemyの派生型だけが攻撃予告を持つため、対象型を確かめてから呼び出す。
        if (auto* enemy = dynamic_cast<BaseEnemy*>(object.get())) {
            lighting.Draw(*enemy, &BaseEnemy::DrawAttackTelegraph);
        }
    }
}
