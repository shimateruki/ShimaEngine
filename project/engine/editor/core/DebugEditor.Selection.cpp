#define NOMINMAX
#include <cstdint>
#include "DebugEditor.h"
#include <numbers>
#include "SceneManager.h"    
#include "BaseScene.h"      
#include "Object3D.h"
#include "imgui.h"
#include <fstream>
#include <string>
#include <vector>
#include "json.hpp"
#include "ImGuizmo.h"
#include "CameraManager.h"
#include "WinApp.h"
#include "Math.h"
#include "DirectXCommon.h"
#include "CollisionConfig.h"
#include "ModelManager.h"      
#include "InputManager.h"    
#include <cmath>
#include <cassert> 
#include <algorithm>
#include <limits>
#include <chrono>
#include <cctype>
#include <ctime>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include "GhostRecorder.h" 
#include "game/system/cinematic/GhostDirector.h"
#include "engine/editor/graphics/camera/CameraEditor.h"
#include "Transform.h"
#include "ParticleManager.h"
#include "EditorManager.h"
#include "PostEffectEditor.h"
#include "SpriteDebugEditor.h"
#include "ParticleEditor.h"
#include "GpuParticleEditor.h"
#include "VfxSequencerEditor.h"
#include "LightEditor.h"      
#include "IconsFontAwesome5.h"

#include <DebugConsole.h>
#include <CollisionManager.h>
#include <filesystem>
#include <game/system/combat/BulletManager.h>
#include "engine/editor/assets/preset/PresetManager.h"
#include <PresetEditor.h>
#include <MeshEffectManager.h>
#include <unordered_map>
#include <utility>
namespace fs = std::filesystem;







void DebugEditor::SetSelectedObject(engine::Object3D* obj) {
    // 現在のSceneに属するObjectだけ選択し、古いSceneのポインタは保持しません。
    selectedObjects_.clear();

    // 有効な現在のシーンの実体なら、単独選択として保持する。
    if (obj && IsObjectInCurrentScene(obj)) {
        selectedObject_ = obj;
        selectedObjects_.push_back(obj);
    // 選択解除や他シーンの参照なら、主選択も空にする。
    } else {
        selectedObject_ = nullptr;
    }
}

void DebugEditor::SyncObjectSelectionToInspector() {
    // Ghost系Editorが選択中ならInspectorを奪わず、通常のObject選択だけ同期します。
    EditorManager* editorManager = EditorManager::GetInstance();
    // Ghostの再生・演出ツールがInspectorを使っている間は、選択先を変更しない。
    if ((ghostRecorder_ && editorManager->GetSelectedObject() == ghostRecorder_) ||
        (ghostDirector_ && editorManager->GetSelectedObject() == ghostDirector_)) {
        return;
    }

    // 主選択がある場合は、シーンオブジェクトを表示するInspectorの担当を選ぶ。
    if (selectedObject_) {
        editorManager->SetSelectedObject(this);
    // オブジェクトの主選択がなければ、Inspector側の選択を解除する。
    } else {
        editorManager->ClearSelection();
    }
}

bool DebugEditor::FocusSceneObject(engine::Object3D* object) {
    // フォーカス対象が現行Sceneに残っていることを確認してからカメラへ渡します。
    if (!object || !IsObjectInCurrentScene(object)) {
        return false;
    }

    return CameraEditor::GetInstance()->FocusSceneObject(object);
}

bool DebugEditor::IsObjectSelected(const engine::Object3D* object) const {
    // 選択配列に残った古いポインタを肯定しないようScene所属も調べます。
    // 有効な現在のシーンの実体でなければ、その参照が選択配列に残っていてもfalseを返す。
    if (!object || !IsObjectInCurrentScene(object))
        return false;

    for (engine::Object3D* selected : selectedObjects_) {
        // 現在の選択集合から同一の実体を見つけた場合にtrueを返す。
        if (selected == object)
            return true;
    }

    return false;
}

std::int32_t DebugEditor::GetSelectionOverlayMode() const {
    // UI保存用の整数値として選択表示モードを返します。
    return static_cast<std::int32_t>(selectionOverlayMode_);
}

void DebugEditor::SetSelectionOverlayMode(std::int32_t mode) {
    // 永続化データの範囲外値を無視し、表示モードを不正な列挙値にしません。
    if (mode < static_cast<std::int32_t>(SelectionOverlayMode::kCompact) ||
        mode > static_cast<std::int32_t>(SelectionOverlayMode::kHidden)) {
        return;
    }

    selectionOverlayMode_ = static_cast<SelectionOverlayMode>(mode);
}

void DebugEditor::ClearObjectSelection() {
    // グループ変形の開始状態も破棄し、次の選択へ前回の基準を持ち越しません。
    selectedObject_ = nullptr;
    selectedObjects_.clear();
    groupTransformStartStates_.clear();
}

void DebugEditor::AddSelectedObject(engine::Object3D* object) {
    // 同じObjectを重複登録せず、最後に追加したものを主選択にします。
    // 現在のシーンが保持する有効な参照だけ、複数選択に加えられる。
    if (!object || !IsObjectInCurrentScene(object))
        return;

    for (engine::Object3D* selected : selectedObjects_) {
        // すでに登録済みならその実体を主選択にし、配列の要素は増やさない。
        if (selected == object) {
            selectedObject_ = object;
            return;
        }
    }

    selectedObjects_.push_back(object);
    selectedObject_ = object;
}

void DebugEditor::ToggleSelectedObject(engine::Object3D* object) {
    // 除外後の主選択を最後の残存Objectへ移し、選択配列と同期します。
    // 有効なオブジェクトに対してだけ、選択状態を切り替える。
    if (!object || !IsObjectInCurrentScene(object))
        return;

    for (auto it = selectedObjects_.begin(); it != selectedObjects_.end(); ++it) {
        // すでに選択中の場合は外し、残っている最後の要素を主選択にする。
        if (*it == object) {
            selectedObjects_.erase(it);
            selectedObject_ = selectedObjects_.empty() ? nullptr : selectedObjects_.back();
            return;
        }
    }

    // まだ選ばれていない場合は、通常の追加経路で主選択にする。
    AddSelectedObject(object);
}

void DebugEditor::PruneInvalidSelectedObjects() {
    // Sceneの再読込や削除で無効になった選択を除き、重複も整理します。
    std::vector<engine::Object3D*> validObjects;
    validObjects.reserve(selectedObjects_.size());

    for (engine::Object3D* object : selectedObjects_) {
        // 無効な参照やシーンをまたいだ参照は新しい選択集合へ残さない。
        if (!object || !IsObjectInCurrentScene(object))
            continue;

        bool alreadyAdded = false;

        for (engine::Object3D* added : validObjects) {
            // 同じ実体が既に新しい集合にあれば、重複として記録する。
            if (added == object) {
                alreadyAdded = true;
                break;
            }
        }

        // 現シーンにあり、まだ保存していない実体だけ選択集合へ書き戻す。
        if (!alreadyAdded) {
            validObjects.push_back(object);
        }
    }

    selectedObjects_ = validObjects;

    // 主選択を失った場合は、有効な最後の選択へ移す。残りがなければ解除する。
    if (!selectedObject_ || !IsObjectInCurrentScene(selectedObject_)) {
        selectedObject_ = selectedObjects_.empty() ? nullptr : selectedObjects_.back();
    }
}

// ワールド座標の線分上で、選択または配置に使える最も手前の面を探す。交差位置は0～1の線分比率。
bool DebugEditor::RaycastSceneSurface(const engine::Ray& ray, engine::RayResult& result,
    engine::Object3D** hitObject, const engine::Object3D* ignore, bool forSelection,
    const std::unordered_set<std::string>* ignoredRootGuids) {

    result = {};
    // 当たった実体を受け取る引数がある場合は、未命中の状態に初期化する。
    if (hitObject) *hitObject = nullptr;
    const auto finite = [](const engine::Vector3& v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    };
    // 現在のシーンがないか、線分が非有限または長さがほぼ0なら判定できない。
    if (!sceneManager_ || !sceneManager_->GetCurrentScene() || !finite(ray.origin) || !finite(ray.diff) ||
        engine::Math::Dot(ray.diff, ray.diff) < 1e-12f)
        return false;
    float bestDistance = 1.0f;
    engine::Object3D* hit = nullptr;

    auto accept = [&](engine::Object3D* object, float distance, const engine::Vector3& localNormal,
                      const engine::Matrix4x4& inverseWorld) {
        auto normal = engine::Math::TransformNormal(localNormal, engine::Math::Transpose(inverseWorld));
        // 逆転置後の法線が無効なら、位置だけが正しくても候補を採用しない。
        if (!finite(normal) || engine::Math::Dot(normal, normal) < 1e-12f) return;
        normal = engine::Math::Normalize(normal);
        // 表裏を区別せず交差を許すため、法線はレイの進行方向と反対を向くよう揃える。
        if (engine::Math::Dot(normal, ray.diff) > 0) normal = -normal;
        bestDistance = distance; hit = object;
        result = { true, distance, ray.origin + ray.diff * distance, normal };
    };
    auto& objects = sceneManager_->GetCurrentScene()->GetObjects();

    for (auto& obj : objects) {
        // 空の枠とエディター内部のオブジェクトを判定から外す。
        if (!obj || obj->IsEditorInternal())
            continue;
        // カーソルや描画補助の線を名前で除外する。
        if (obj->GetName() == "Cursor" || obj->GetName() == "Line")
            continue;
        // 描画されない物は除外する。ロックは選択用途の場合にだけ対象外とする。
        if (!obj->GetIsRenderVisible() || (forSelection && obj->GetIsLocked()))
            continue;
        bool ignored = false;
        for (auto* ancestor = obj.get(); ancestor; ancestor = ancestor->GetParent())
            // オブジェクト自身または祖先が除外対象なら、その階層の面を判定しない。
            if (ancestor == ignore || (ignoredRootGuids && ignoredRootGuids->contains(ancestor->GetPersistentGuid()))) {
                ignored = true; break;
            }
        // 除外された階層に加え、配置に使う面の検索ではモデルのない物も飛ばす。
        if (ignored || (!forSelection && !obj->GetModel()))
            continue;

        const engine::Matrix4x4 worldMatrix = obj->GetWorldMatrix();

        const engine::Vector3 axisX = { worldMatrix.m[0][0], worldMatrix.m[0][1], worldMatrix.m[0][2] };
        const engine::Vector3 axisY = { worldMatrix.m[1][0], worldMatrix.m[1][1], worldMatrix.m[1][2] };
        const engine::Vector3 axisZ = { worldMatrix.m[2][0], worldMatrix.m[2][1], worldMatrix.m[2][2] };
        // 変換のいずれかの軸がほぼ0なら逆変換できないため、その物体を調べない。
        if (engine::Math::Dot(axisX, axisX) < 1e-12f ||
            engine::Math::Dot(axisY, axisY) < 1e-12f ||
            engine::Math::Dot(axisZ, axisZ) < 1e-12f) continue;

        const engine::Matrix4x4 inverseWorld = engine::Math::Inverse(worldMatrix);
        // 始点は座標、差分は方向としてローカル空間へ移す。正規化せず、交差位置を元のレイと同じ 0～1 の比率で比較する。
        const engine::Ray localRay = {
            engine::Math::Transform(ray.origin, inverseWorld),
            engine::Math::TransformNormal(ray.diff, inverseWorld)
        };

        // ローカルへ変換した結果が非有限なら、その物体との交差を判定しない。
        if (!std::isfinite(localRay.origin.x) || !std::isfinite(localRay.origin.y) || !std::isfinite(localRay.origin.z) ||
            !std::isfinite(localRay.diff.x) || !std::isfinite(localRay.diff.y) || !std::isfinite(localRay.diff.z)) continue;
        engine::Vector3 localMin = { -0.5f, -0.5f, -0.5f };
        engine::Vector3 localMax = { 0.5f, 0.5f, 0.5f };
        engine::Model* model = obj->GetModel();

        // モデルがある場合はモデルのローカル境界を使い、モデルなし選択では既定の単位箱を使う。
        if (model) {
            localMin = model->GetLocalAabbMin();
            localMax = model->GetLocalAabbMax();
        }

        // 境界値が非有限か最小値と最大値が逆の場合は、交差区間を作らない。
        if (!std::isfinite(localMin.x) || !std::isfinite(localMin.y) || !std::isfinite(localMin.z) ||
            !std::isfinite(localMax.x) || !std::isfinite(localMax.y) || !std::isfinite(localMax.z) ||
            localMin.x > localMax.x || localMin.y > localMax.y || localMin.z > localMax.z) continue;
        const float origins[3] = { localRay.origin.x, localRay.origin.y, localRay.origin.z };
        const float directions[3] = { localRay.diff.x, localRay.diff.y, localRay.diff.z };
        const float mins[3] = { localMin.x, localMin.y, localMin.z };
        const float maxs[3] = { localMax.x, localMax.y, localMax.z };
        float entry = 0.0f;
        float exit = 1.0f;
        bool startsInside = true;
        bool intersects = true;

        // 各軸で箱に入っているレイ区間を重ねる。区間が重ならなければ、箱を通過する点は存在しない。
        for (std::int32_t axis = 0; axis < 3; ++axis) {
            // どれかの軸で始点が箱の外にあれば、始点を外部とする。
            if (origins[axis] < mins[axis] || origins[axis] > maxs[axis])
                startsInside = false;
            // 面とほぼ平行な軸では、始点がその軸の範囲内かだけ確認する。
            if (std::abs(directions[axis]) < 1e-6f) {
                // 平行に近く、始点がその軸の範囲外なら、この線分は箱の内側へ入れない。
                if (origins[axis] < mins[axis] || origins[axis] > maxs[axis])
                    intersects = false;
                continue;
            }

            float nearT = (mins[axis] - origins[axis]) / directions[axis];
            float farT = (maxs[axis] - origins[axis]) / directions[axis];
            // 逆向きの軸では二つの交点が逆順で出るため、入口と出口を入れ替える。
            if (nearT > farT)
                std::swap(nearT, farT);
            entry = (std::max)(entry, nearT);
            exit = (std::min)(exit, farT);

            // 現在までに求めた入口が出口を越えたら、全軸で共通する交差区間はない。
            if (entry > exit) {
                intersects = false;
                break;
            }
        }

        // 箱を通過しないか既に見つけた面より手前に入口がない場合は、三角形の計算を省く。
        if (!intersects || entry > bestDistance)
            continue;

        // 静的モデルは面との交差を調べ、Aabb の空間部分で手前の物体を選ばない。
        // アニメーション中の頂点はCPU側と一致しないため、従来の箱判定を使う。
        const bool useMeshSurface = model && !model->GetModelData().hasSkinning &&
            !model->GetModelData().usesNodeAnimationProxy;
        // モデルがあり、ボーンでもノードでも頂点が変形しない場合に、実際の面で選別する。
        if (useMeshSurface) {
            const auto& meshes = model->GetModelData().meshes;
            const std::int32_t drawIndex = obj->GetMeshDrawIndex();

            for (size_t meshIndex = 0; meshIndex < meshes.size(); ++meshIndex) {
                // 描画メッシュを指定されている場合、表示対象外のメッシュの三角形は調べない。
                if (drawIndex >= 0 && meshIndex != static_cast<size_t>(drawIndex))
                    continue;
                const auto& mesh = meshes[meshIndex];

                for (size_t index = 0; index + 2 < mesh.indices.size(); index += 3) {
                    const uint32_t i0 = mesh.indices[index];
                    const uint32_t i1 = mesh.indices[index + 1];
                    const uint32_t i2 = mesh.indices[index + 2];
                    // 三点のいずれかが頂点配列に存在しない面は交差を計算できない。
                    if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() || i2 >= mesh.vertices.size())
                        continue;
                    const auto& a = mesh.vertices[i0].position;
                    const auto& b = mesh.vertices[i1].position;
                    const auto& c = mesh.vertices[i2].position;
                    const engine::Vector3 edge1 = { b.x - a.x, b.y - a.y, b.z - a.z };
                    const engine::Vector3 edge2 = { c.x - a.x, c.y - a.y, c.z - a.z };
                    const engine::Vector3 p = engine::Math::Cross(localRay.diff, edge2);
                    // レイが面と平行に近い場合は分母が0に近くなるため、交差の計算を進めない。
                    const float determinant = engine::Math::Dot(edge1, p);
                    // 行列式の絶対値がしきい値より小さい場合は、その面を飛ばす。
                    if (std::abs(determinant) < 1e-8f)
                        continue;
                    const float inverseDeterminant = 1.0f / determinant;
                    const engine::Vector3 fromA = { localRay.origin.x - a.x, localRay.origin.y - a.y, localRay.origin.z - a.z };
                    const float u = engine::Math::Dot(fromA, p) * inverseDeterminant;
                    // 最初の辺の比率が0から1の範囲を外れた場合は三角形の外。
                    if (u < 0.0f || u > 1.0f)
                        continue;
                    const engine::Vector3 q = engine::Math::Cross(fromA, edge1);
                    const float v = engine::Math::Dot(localRay.diff, q) * inverseDeterminant;
                    // もう一方の比率が負か、二つの比率の和が1を超えた場合も三角形の外。
                    if (v < 0.0f || u + v > 1.0f)
                        continue;
                    const float distance = engine::Math::Dot(edge2, q) * inverseDeterminant;

                    // 交差位置が線分内で、現在の最寄り候補より手前の場合だけ登録を試みる。
                    if (distance >= 0.0f && distance <= 1.0f && distance < bestDistance) {
                        accept(obj.get(), distance, engine::Math::Cross(edge1, edge2), inverseWorld);
                    }
                }
            }
        // 頂点が変形するモデルやモデルなし選択では、三角形の代わりにローカル境界箱を使う。
        } else {
            // 箱の内側から始まるレイは出口を比較し、周囲の大きなObjectを優先しない。
            const float distance = startsInside ? exit : entry;

            // 入出口のうち使う点が線分内で、現在の候補より手前なら、触れた箱の面を調べる。
            if (distance >= 0.0f && distance < bestDistance) {
                const auto point = localRay.origin + localRay.diff * distance;
                engine::Vector3 normal{};
                float nearest = (std::numeric_limits<float>::max)();
                const float coords[3] = {point.x, point.y, point.z};
                for (std::int32_t axis = 0; axis < 3; ++axis) {
                    for (std::int32_t side = 0; side < 2; ++side) {
                        const float error = std::abs(coords[axis] - (side ? maxs[axis] : mins[axis]));
                        // 交差点と面の座標との差が最小になった軸と側から、その外向き法線を選ぶ。
                        if (error < nearest) {
                            nearest = error; normal = {};
                            // 差が最小の面がXに垂直なら、X成分だけを外側へ向ける。
                            if (axis == 0) normal.x = side ? 1.f : -1.f;
                            // 差が最小の面がYに垂直なら、Y成分だけを外側へ向ける。
                            if (axis == 1) normal.y = side ? 1.f : -1.f;
                            // 差が最小の面がZに垂直なら、Z成分だけを外側へ向ける。
                            if (axis == 2) normal.z = side ? 1.f : -1.f;
                        }
                    }
                }
                accept(obj.get(), distance, normal, inverseWorld);
            }
        }
    }

    // 出力先が指定された場合は最寄りの実体を渡し、未命中ならnullptrのままにする。
    if (hitObject) *hitObject = hit;
    return result.isHit;
}

// ゲームビューのピクセル位置を選択用の線分に変換し、最前面の操作対象を返す。
engine::Object3D* DebugEditor::PickObjectAtGameViewPos(const engine::Vector2& mousePos) {
    engine::RayResult result{};
    engine::Object3D* hit = nullptr;
    RaycastSceneSurface(ScreenPointToRay(mousePos), result, &hit, nullptr, true);
    // 地形編集グループは見た目の子をクリックしても、当たり判定を含む区画単位で選択する。
    // 子メッシュだけを調整したい場合はHierarchyから直接選択できる。
    if (hit && hit->GetParent() && hit->GetParent()->GetTag() == "TerrainEditGroup") {
        return hit->GetParent();
    }

    return hit;
}

engine::Vector3 DebugEditor::GetObjectWorldPositionForSelection(engine::Object3D* object) {
    // モデルのワールドAABBが有効なら、その中心を画面選択の代表点にします。
    // 実体を参照できない場合は代表点を原点として返す。
    if (!object)
        return { 0.0f, 0.0f, 0.0f };

    engine::Aabb modelAabb = object->GetModelWorldAabb();

    // モデルのワールド境界が有効な場合は、その中心を代表点にする。
    if (IsValidAabb(modelAabb)) {
        return {
            (modelAabb.min.x + modelAabb.max.x) * 0.5f,
            (modelAabb.min.y + modelAabb.max.y) * 0.5f,
            (modelAabb.min.z + modelAabb.max.z) * 0.5f
        };
    }

    // モデル境界が取れない場合は、実体のワールド行列の並進を代表点にする。
    engine::Matrix4x4 worldMatrix = object->GetWorldMatrix();
    return { worldMatrix.m[3][0], worldMatrix.m[3][1], worldMatrix.m[3][2] };
}

void DebugEditor::SelectObjectsInGameViewRect(const engine::Vector2& start, const engine::Vector2& end, bool additive) {
    // Game View内のドラッグ矩形を画面座標へ変換してObjectの投影位置と比べます。
    // 現在のシーンがなければ投影する実体がないため、選択を変更しない。
    if (!sceneManager_ || !sceneManager_->GetCurrentScene())
        return;

    // ドラッグ方向にかかわらず同じ矩形を得るため、始点と終点から各軸の最小・最大を取る。
    const float localLeft = start.x < end.x ? start.x : end.x;
    const float localRight = start.x < end.x ? end.x : start.x;
    const float localTop = start.y < end.y ? start.y : end.y;
    const float localBottom = start.y < end.y ? end.y : start.y;

    const float screenLeft = gameViewOffset_.x + localLeft;
    const float screenRight = gameViewOffset_.x + localRight;
    const float screenTop = gameViewOffset_.y + localTop;
    const float screenBottom = gameViewOffset_.y + localBottom;

    // 既存の選択に追加する指定がなければ、ドラッグ前の選択を解除する。
    if (!additive) {
        ClearObjectSelection();
    }

    auto& objects = sceneManager_->GetCurrentScene()->GetObjects();

    for (auto& obj : objects) {
        // 空の枠とエディター内部の実体を矩形選択から外す。
        if (!obj || obj->IsEditorInternal())
            continue;
        // 名前で識別するカーソルと線の補助物を除く。
        if (obj->GetName() == "Cursor" || obj->GetName() == "Line")
            continue;
        // 非表示の実体と編集をロックした実体を選択に加えない。
        if (!obj->GetIsRenderVisible() || obj->GetIsLocked())
            continue;

        // 境界が矩形に少しかかるだけでは選択しない。モデルの代表点を投影し、矩形に収まる Object を選ぶ。
        engine::Vector3 screenPosition = WorldToScreen(GetObjectWorldPositionForSelection(obj.get()));
        // 視点の後方へ投影された代表点は、画面上の矩形に含めない。
        if (screenPosition.z < 0.0f)
            continue;

        // 代表点がドラッグ矩形の上下左右の内側か境界にあれば選ぶ。
        if (screenPosition.x >= screenLeft && screenPosition.x <= screenRight &&
            screenPosition.y >= screenTop && screenPosition.y <= screenBottom) {
            AddSelectedObject(obj.get());
        }
    }

    // 主選択がある場合か、前の選択を置換した場合に、Inspectorの表示を同期する。
    if (selectedObject_ || !additive) {
        SyncObjectSelectionToInspector();
    }
}

void DebugEditor::DrawSelectionRectangleOverlay() {
#ifdef USE_IMGUI
    // ドラッグ中だけ矩形をGame Viewに重ね、選択確定前の範囲を示します。
    if (!isSelectionRectDragging_ || !isSelectionRectReady_)
        return;

    const float minX = selectionRectStart_.x < selectionRectEnd_.x ? selectionRectStart_.x : selectionRectEnd_.x;
    const float maxX = selectionRectStart_.x < selectionRectEnd_.x ? selectionRectEnd_.x : selectionRectStart_.x;
    const float minY = selectionRectStart_.y < selectionRectEnd_.y ? selectionRectStart_.y : selectionRectEnd_.y;
    const float maxY = selectionRectStart_.y < selectionRectEnd_.y ? selectionRectEnd_.y : selectionRectStart_.y;

    ImVec2 rectMin = ImVec2(gameViewOffset_.x + minX, gameViewOffset_.y + minY);
    ImVec2 rectMax = ImVec2(gameViewOffset_.x + maxX, gameViewOffset_.y + maxY);

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    drawList->AddRectFilled(rectMin, rectMax, IM_COL32(80, 160, 255, 45));
    drawList->AddRect(rectMin, rectMax, IM_COL32(80, 190, 255, 230), 0.0f, 0, 2.0f);
#endif
}

// 現在選択中の実体を、通常・詳細の表示方式に合う印や枠でゲームビューに重ねる。
void DebugEditor::DrawSelectedObjectBoundsOverlay() {
#ifdef USE_IMGUI
    // 選択表示を無効にしていれば、境界の投影を行わない。
    if (!drawSelectionBounds_)
        return;
    PruneInvalidSelectedObjects();
    // 無効な選択を除いた後、表示対象が空か非表示モードなら描画しない。
    if (selectedObjects_.empty() || selectionOverlayMode_ == SelectionOverlayMode::kHidden)
        return;
    // ゲームビューの幅か高さがほぼない場合は、座標変換とクリップを省く。
    if (gameViewSize_.x <= 1.0f || gameViewSize_.y <= 1.0f)
        return;

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    const std::size_t selectedCount = selectedObjects_.size();
    const bool showTemporaryDetails =
        selectionOverlayMode_ == SelectionOverlayMode::kCompact && ImGui::GetIO().KeyAlt;
    const ImVec2 viewMin = ImVec2(gameViewOffset_.x, gameViewOffset_.y);
    const ImVec2 viewMax = ImVec2(
        gameViewOffset_.x + gameViewSize_.x,
        gameViewOffset_.y + gameViewSize_.y);

    auto clampFloat = [](float value, float minimum, float maximum) {
        return (std::max)(minimum, (std::min)(value, maximum));
    };

    // 副選択の点だけ表示する場合にも共通の中心マーカーを使う。主選択だけ半径と枠線を少し大きくする。
    auto drawPivotMarker = [&](const ImVec2& center, ImU32 color, bool primary) {
        const float radius = primary ? 5.0f : 4.5f;
        drawList->AddCircleFilled(center, radius + 2.0f, IM_COL32(10, 24, 36, 190), 16);
        drawList->AddCircle(center, radius, color, 16, primary ? 2.0f : 1.7f);
        drawList->AddLine(
            ImVec2(center.x - 2.5f, center.y),
            ImVec2(center.x + 2.5f, center.y),
            color,
            1.3f);
        drawList->AddLine(
            ImVec2(center.x, center.y - 2.5f),
            ImVec2(center.x, center.y + 2.5f),
            color,
            1.3f);
    };

    // 矩形の角を二本ずつの短い線で示し、背景用の太い線を先に描く。
    auto drawCornerBrackets = [&](const ImVec2& minimum, const ImVec2& maximum, ImU32 color, float thickness) {
        const float width = maximum.x - minimum.x;
        const float height = maximum.y - minimum.y;
        // 枠の短辺に比例させ、8～18 ピクセルに制限する。小さい枠は角を見失わず、大きい枠も辺を覆いすぎない。
        const float bracketLength = clampFloat((std::min)(width, height) * 0.22f, 8.0f, 18.0f);
        const ImU32 shadowColor = IM_COL32(8, 18, 28, 210);
        const float shadowThickness = thickness + 2.0f;
        const ImVec2 starts[8] = {
            { minimum.x, minimum.y }, { minimum.x, minimum.y },
            { maximum.x, minimum.y }, { maximum.x, minimum.y },
            { minimum.x, maximum.y }, { minimum.x, maximum.y },
            { maximum.x, maximum.y }, { maximum.x, maximum.y }
        };

        const ImVec2 ends[8] = {
            { minimum.x + bracketLength, minimum.y }, { minimum.x, minimum.y + bracketLength },
            { maximum.x - bracketLength, minimum.y }, { maximum.x, minimum.y + bracketLength },
            { minimum.x + bracketLength, maximum.y }, { minimum.x, maximum.y - bracketLength },
            { maximum.x - bracketLength, maximum.y }, { maximum.x, maximum.y - bracketLength }
        };

        for (std::int32_t i = 0; i < 8; ++i) {
            drawList->AddLine(starts[i], ends[i], shadowColor, shadowThickness);
        }

        for (std::int32_t i = 0; i < 8; ++i) {
            drawList->AddLine(starts[i], ends[i], color, thickness);
        }
    };

    drawList->PushClipRect(viewMin, viewMax, true);

    for (engine::Object3D* object : selectedObjects_) {
        // 実体が無いか現在のシーンに属さない場合は、表示対象にしません。
        if (!object || !IsObjectInCurrentScene(object))
            continue;

        engine::Vector3 corners[8] = {};
        engine::Vector3 center = GetObjectWorldPositionForSelection(object);
        bool hasOrientedModelBounds = false;

        // ワールドAABBを再投影せず、ローカル境界を直接変換して回転物の過剰な膨らみを防ぐ。
        if (engine::Model* model = object->GetModel()) {
            const engine::Vector3 localMin = model->GetLocalAabbMin();
            const engine::Vector3 localMax = model->GetLocalAabbMax();
            const bool isFiniteBounds =
                std::isfinite(localMin.x) && std::isfinite(localMin.y) && std::isfinite(localMin.z) &&
                std::isfinite(localMax.x) && std::isfinite(localMax.y) && std::isfinite(localMax.z);
            // ローカル境界の全成分が有限で、範囲の順序も有効な場合だけ8頂点を変換する。
            if (isFiniteBounds &&
                localMax.x >= localMin.x && localMax.y >= localMin.y && localMax.z >= localMin.z) {
                const engine::Vector3 localCorners[8] = {
                    { localMin.x, localMin.y, localMin.z },
                    { localMax.x, localMin.y, localMin.z },
                    { localMin.x, localMax.y, localMin.z },
                    { localMax.x, localMax.y, localMin.z },
                    { localMin.x, localMin.y, localMax.z },
                    { localMax.x, localMin.y, localMax.z },
                    { localMin.x, localMax.y, localMax.z },
                    { localMax.x, localMax.y, localMax.z }
                };

                const engine::Matrix4x4 worldMatrix = object->GetWorldMatrix();

                for (std::int32_t i = 0; i < 8; ++i) {
                    corners[i] = engine::Math::Transform(localCorners[i], worldMatrix);
                }

                const engine::Vector3 localCenter = {
                    (localMin.x + localMax.x) * 0.5f,
                    (localMin.y + localMax.y) * 0.5f,
                    (localMin.z + localMax.z) * 0.5f
                };

                center = engine::Math::Transform(localCenter, worldMatrix);
                hasOrientedModelBounds = true;
            }
        }

        if (!hasOrientedModelBounds) {
            // モデル境界を取得できないObjectも選択可能にするため、World AABBかTransformスケールで補う。
            engine::Aabb aabb = object->GetModelWorldAabb();

            // ワールドの境界が無効であれば、基準位置と拡縮値から最小寸法付きの箱を作る。
            if (!IsValidAabb(aabb)) {
                const engine::Vector3 scale = object->GetTransform()->scale;
                const engine::Vector3 halfSize = {
                    (std::max)(std::abs(scale.x), 0.5f),
                    (std::max)(std::abs(scale.y), 0.5f),
                    (std::max)(std::abs(scale.z), 0.5f)
                };

                aabb.min = center - halfSize;
                aabb.max = center + halfSize;
            }

            corners[0] = { aabb.min.x, aabb.min.y, aabb.min.z };
            corners[1] = { aabb.max.x, aabb.min.y, aabb.min.z };
            corners[2] = { aabb.min.x, aabb.max.y, aabb.min.z };
            corners[3] = { aabb.max.x, aabb.max.y, aabb.min.z };
            corners[4] = { aabb.min.x, aabb.min.y, aabb.max.z };
            corners[5] = { aabb.max.x, aabb.min.y, aabb.max.z };
            corners[6] = { aabb.min.x, aabb.max.y, aabb.max.z };
            corners[7] = { aabb.max.x, aabb.max.y, aabb.max.z };
            center = GetAabbCenter(aabb);
        }

        const bool isPrimary = object == selectedObject_;
        const ImU32 frameColor =
            isPrimary ? IM_COL32(255, 226, 72, 255) : IM_COL32(80, 210, 255, 235);
        const bool showBounds = isPrimary ||
            selectionOverlayMode_ == SelectionOverlayMode::kDetailed ||
            showTemporaryDetails;

        // 主選択以外で詳細表示を要求していない場合は、中心マーカーだけを描く。
        if (!showBounds) {
            const engine::Vector3 screen = WorldToScreen(center);
            // 中心がカメラ後方にあるか画面座標が無効なら、中心マーカーも描かない。
            if (screen.z < 0.0f || !std::isfinite(screen.x) || !std::isfinite(screen.y))
                continue;

            const ImVec2 markerCenter = ImVec2(
                clampFloat(screen.x, viewMin.x + 7.0f, viewMax.x - 7.0f),
                clampFloat(screen.y, viewMin.y + 7.0f, viewMax.y - 7.0f));
            drawPivotMarker(markerCenter, frameColor, false);
            continue;
        }

        bool hasScreenPoint = false;
        std::int32_t visibleCornerCount = 0;
        engine::Vector3 projectedCorners[8] = {};
        bool cornerVisible[8] = {};
        ImVec2 rectMin = ImVec2(0.0f, 0.0f);
        ImVec2 rectMax = ImVec2(0.0f, 0.0f);

        for (std::int32_t i = 0; i < 8; ++i) {
            projectedCorners[i] = WorldToScreen(corners[i]);
            const engine::Vector3& screen = projectedCorners[i];
            // 画面に投影できない角は、矩形や詳細な辺の基準点にしない。
            if (screen.z < 0.0f || !std::isfinite(screen.x) || !std::isfinite(screen.y))
                continue;

            cornerVisible[i] = true;
            ++visibleCornerCount;

            // 最初に投影できた角を、画面上の矩形の最小・最大の初期点とする。
            if (!hasScreenPoint) {
                rectMin = ImVec2(screen.x, screen.y);
                rectMax = rectMin;
                hasScreenPoint = true;
            // 二つ目以降の投影点は既存の矩形を広げる。
            } else {
                rectMin.x = (std::min)(rectMin.x, screen.x);
                rectMin.y = (std::min)(rectMin.y, screen.y);
                rectMax.x = (std::max)(rectMax.x, screen.x);
                rectMax.y = (std::max)(rectMax.y, screen.y);
            }
        }

        const engine::Vector3 centerScreen = WorldToScreen(center);
        const bool hasCenterScreen = centerScreen.z >= 0.0f &&
            std::isfinite(centerScreen.x) && std::isfinite(centerScreen.y);
        // 角も中心も投影できなければ、そのオブジェクトには何も重ねない。
        if (!hasScreenPoint && !hasCenterScreen)
            continue;

        const bool intersectsView = hasScreenPoint &&
            rectMax.x >= viewMin.x && rectMin.x <= viewMax.x &&
            rectMax.y >= viewMin.y && rectMin.y <= viewMax.y;
        const float rawWidth = hasScreenPoint ? (std::max)(rectMax.x - rectMin.x, 0.0f) : 0.0f;
        const float rawHeight = hasScreenPoint ? (std::max)(rectMax.y - rectMin.y, 0.0f) : 0.0f;
        const float widthCoverage = rawWidth / gameViewSize_.x;
        const float heightCoverage = rawHeight / gameViewSize_.y;
        const float areaCoverage = widthCoverage * heightCoverage;
        const bool projectionIsUnstable = visibleCornerCount < 8;
        const bool isOversized =
            widthCoverage > 0.70f || heightCoverage > 0.70f || areaCoverage > 0.32f;
        // 一部の角が投影できない箱や、画面を大きく覆う箱は枠が見づらいため、Compact では中心だけ示す。
        const bool useCompactMarker =
            selectionOverlayMode_ == SelectionOverlayMode::kCompact &&
            !showTemporaryDetails &&
            (projectionIsUnstable || isOversized || !intersectsView);

        // 通常表示で投影が不安定、または枠が大きい・画面と重ならない場合は、枠を中心マーカーに切り替える。
        if (useCompactMarker) {
            // 中心点を投影できなければ、代わりのマーカーも表示できない。
            if (!hasCenterScreen)
                continue;

            const ImVec2 markerCenter = ImVec2(
                clampFloat(centerScreen.x, viewMin.x + 9.0f, viewMax.x - 9.0f),
                clampFloat(centerScreen.y, viewMin.y + 9.0f, viewMax.y - 9.0f));
            drawPivotMarker(markerCenter, frameColor, true);

            std::string label = object->GetName().empty() ? "Selected" : object->GetName();

            // 複数選択中なら、表示名にほかの選択数も付ける。
            if (selectedCount > 1) {
                label += " (+" + std::to_string(selectedCount - 1) + ")";
            }

            const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
            ImVec2 labelPos = ImVec2(markerCenter.x + 10.0f, markerCenter.y - textSize.y * 0.5f);
            labelPos.x = clampFloat(labelPos.x, viewMin.x + 4.0f, viewMax.x - textSize.x - 4.0f);
            labelPos.y = clampFloat(labelPos.y, viewMin.y + 4.0f, viewMax.y - textSize.y - 4.0f);
            drawList->AddText(
                ImVec2(labelPos.x + 1.0f, labelPos.y + 1.0f),
                IM_COL32(0, 0, 0, 220),
                label.c_str());
            drawList->AddText(labelPos, IM_COL32(255, 245, 150, 255), label.c_str());
            continue;
        }

        // 投影可能な角がなく画面枠を作れないか、矩形がビューに重ならない場合は枠を省く。
        if (!hasScreenPoint || !intersectsView)
            continue;

        rectMin.x = clampFloat(rectMin.x, viewMin.x + 2.0f, viewMax.x - 2.0f);
        rectMin.y = clampFloat(rectMin.y, viewMin.y + 2.0f, viewMax.y - 2.0f);
        rectMax.x = clampFloat(rectMax.x, viewMin.x + 2.0f, viewMax.x - 2.0f);
        rectMax.y = clampFloat(rectMax.y, viewMin.y + 2.0f, viewMax.y - 2.0f);
        // ビュー内へ丸めた結果の幅か高さが0以下なら、描画できる矩形が残っていない。
        if (rectMax.x <= rectMin.x || rectMax.y <= rectMin.y)
            continue;

        // 遠距離の小さい物体でも選択表示が潰れない最低サイズを確保する。
        constexpr float kMinimumDisplaySize = 16.0f;
        const ImVec2 rectCenter = ImVec2(
            (rectMin.x + rectMax.x) * 0.5f,
            (rectMin.y + rectMax.y) * 0.5f);
        // 幅が最低サイズ未満なら、ビュー内で中心を基準に左右へ広げる。
        if (rectMax.x - rectMin.x < kMinimumDisplaySize) {
            rectMin.x = clampFloat(
                rectCenter.x - kMinimumDisplaySize * 0.5f,
                viewMin.x + 2.0f,
                viewMax.x - 2.0f);
            rectMax.x = clampFloat(
                rectCenter.x + kMinimumDisplaySize * 0.5f,
                viewMin.x + 2.0f,
                viewMax.x - 2.0f);
        }

        // 高さが最低サイズ未満なら、ビュー内で中心を基準に上下へ広げる。
        if (rectMax.y - rectMin.y < kMinimumDisplaySize) {
            rectMin.y = clampFloat(
                rectCenter.y - kMinimumDisplaySize * 0.5f,
                viewMin.y + 2.0f,
                viewMax.y - 2.0f);
            rectMax.y = clampFloat(
                rectCenter.y + kMinimumDisplaySize * 0.5f,
                viewMin.y + 2.0f,
                viewMax.y - 2.0f);
        }

        // 詳細表示は投影できた角同士を線で結ぶ。通常表示は画面座標で求めた矩形の角だけ描く。
        const bool showDetailedBounds =
            selectionOverlayMode_ == SelectionOverlayMode::kDetailed || showTemporaryDetails;
        // 詳細モードか一時的な詳細表示の場合は、投影した箱の辺をつなぐ。
        if (showDetailedBounds) {
            constexpr std::int32_t kEdges[12][2] = {
                { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 },
                { 4, 5 }, { 5, 7 }, { 7, 6 }, { 6, 4 },
                { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }
            };

            const float thickness = isPrimary ? 2.0f : 1.5f;

            for (const auto& edge : kEdges) {
                // 辺の両端が投影できない場合は、その辺を線で結ばない。
                if (!cornerVisible[edge[0]] || !cornerVisible[edge[1]])
                    continue;
                const ImVec2 start =
                    ImVec2(projectedCorners[edge[0]].x, projectedCorners[edge[0]].y);
                const ImVec2 end =
                    ImVec2(projectedCorners[edge[1]].x, projectedCorners[edge[1]].y);
                drawList->AddLine(start, end, IM_COL32(8, 18, 28, 210), thickness + 2.0f);
                drawList->AddLine(start, end, frameColor, thickness);
            }
        // 通常の枠表示では、投影範囲から計算した矩形の角だけを描く。
        } else {
            drawCornerBrackets(rectMin, rectMax, frameColor, isPrimary ? 2.2f : 1.7f);
        }

        // 詳細枠・角枠のどちらでも、主選択にだけ名前を重ねる。
        if (isPrimary) {
            std::string label = object->GetName().empty() ? "Selected" : object->GetName();

            // 主選択以外にも選んでいる実体があれば、その数を表示名の横に置く。
            if (selectedCount > 1) {
                label += " (+" + std::to_string(selectedCount - 1) + ")";
            }

            const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
            ImVec2 labelPos = ImVec2(rectMin.x, rectMin.y - textSize.y - 4.0f);

            // 上に名前を置けないほど枠が画面上端に近ければ、枠内へ移す。
            if (labelPos.y < viewMin.y + 3.0f) {
                labelPos.y = rectMin.y + 4.0f;
            }

            labelPos.x = clampFloat(labelPos.x, viewMin.x + 4.0f, viewMax.x - textSize.x - 4.0f);
            labelPos.y = clampFloat(labelPos.y, viewMin.y + 3.0f, viewMax.y - textSize.y - 3.0f);
            drawList->AddText(
                ImVec2(labelPos.x + 1.0f, labelPos.y + 1.0f),
                IM_COL32(0, 0, 0, 220),
                label.c_str());
            drawList->AddText(labelPos, IM_COL32(255, 245, 150, 255), label.c_str());
        }
    }

    drawList->PopClipRect();
#endif
}

bool DebugEditor::IsObjectInCurrentScene(const engine::Object3D* object) const {
    // 内部用 Object を除き、現行シーンが保持する Object とアドレスが一致するか確認する。
    if (!object || object->IsEditorInternal() || !sceneManager_ || !sceneManager_->GetCurrentScene()) {
        return false;
    }

    const auto& objects = sceneManager_->GetCurrentScene()->GetObjects();

    for (const auto& candidate : objects) {
        // シーンの所有リストに同一の実体が見つかったら、選択可能な所属として返す。
        if (candidate.get() == object) {
            return true;
        }
    }

    return false;
}

void DebugEditor::ClearInvalidSelectedObject() {
    // 選択が失われたら変形操作も止め、Inspectorに無効Objectを残しません。
    const bool editorWasSelected = EditorManager::GetInstance()->GetSelectedObject() == this;
    PruneInvalidSelectedObjects();

    // 無効な項目を除いても主選択が残っていれば、編集状態を継続する。
    if (selectedObject_) {
        return;
    }

    ClearObjectSelection();
    isDraggingTransform_ = false;
    hasInspectorEditStart_ = false;
    inspectorEditStartStates_.clear();
    tempObjectStateStart_.clear();

    // Inspectorがこの編集ツールを表示していた場合だけ、無効になった選択表示を解く。
    if (editorWasSelected) {
        EditorManager::GetInstance()->ClearSelection();
    }
}

