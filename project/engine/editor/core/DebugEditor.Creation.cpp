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
#include <chrono>
#include <cctype>
#include <ctime>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include "GhostRecorder.h" 
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
#include <filesystem> // ファイル操作用
#include <game/system/combat/BulletManager.h>
#include "engine/editor/assets/preset/PresetManager.h"
#include <PresetEditor.h>
#include <MeshEffectManager.h>
#include <unordered_map>
#include <utility>
namespace fs = std::filesystem;






// ========================================================================
// シーン内の床合わせ・モデル分割・GameViewへの配置
// ========================================================================

void DebugEditor::DropToFloor() {
    // 選択物または編集中のシーンがなければ、下に落とす対象を確定できない。
    if (!selectedObject_ || !sceneManager_->GetCurrentScene())
        return;

    engine::Math math;
    auto& objects = sceneManager_->GetCurrentScene()->GetObjects();

    // 1. 現在のワールド座標を取得
    engine::Matrix4x4 wm = selectedObject_->GetWorldMatrix();
    engine::Vector3 currentPos = { wm.m[3][0], wm.m[3][1], wm.m[3][2] };

    // 2. 真下に向かってレイ(光線)を飛ばす
    engine::Ray ray;
    ray.origin = currentPos;
    ray.diff = { 0.0f, -1000.0f, 0.0f }; // 下方向へ1000メートル

    engine::RayResult bestHit;
    bestHit.isHit = false;
    bestHit.distance = 1e5f;

    // 3. めり込み防止のための「足元までのオフセット(高さの半分)」を計算
    float yOffset = 0.0f;
    engine::ColliderType type = selectedObject_->GetColliderType();

    // 箱型コライダーには設定の Y 半径を使い、中心から底面までの距離を求める。
    if (type == engine::ColliderType::kAabb || type == engine::ColliderType::kObb) {
        yOffset = selectedObject_->GetColliderConfig().size.y;
    }
    // 球コライダーでは X に保持する半径を中心から底面までの距離に使う。
    else if (type == engine::ColliderType::kSphere) {
        yOffset = selectedObject_->GetColliderConfig().size.x; // 球体はXを半径としている想定
    }
    else {
        // コライダーが無い場合はスケールのYを基準にする
        yOffset = selectedObject_->GetTransform()->scale.y;
    }

    // 4. 真下にある足場（他のオブジェクト）を探す
    for (auto& obj : objects) {
        // 選択中の物体自身は、下向きレイの足場の候補から除く。
        if (obj.get() == selectedObject_)
            continue; // 自分自身は無視
        // 編集用の Cursor と Line は、足場に含めない。
        if (obj->GetName() == "Cursor" || obj->GetName() == "Line")
            continue;
        // 描画が非表示の物体は、床合わせの足場に含めない。
        if (!obj->GetIsRenderVisible())
            continue; // 非表示オブジェクトはすり抜ける

        engine::Matrix4x4 targetWm = obj->GetWorldMatrix();
        engine::Vector3 wp = { targetWm.m[3][0], targetWm.m[3][1], targetWm.m[3][2] };
        engine::Vector3 ws = obj->GetTransform()->scale;

        engine::RayResult tmp;

        // 各 Object の中心とスケールで作った簡易箱を使う。交差した候補のうち、レイの始点から最も近い面を選ぶ。
        if (math.IntersectRayAabb(ray, wp - ws, wp + ws, &tmp)) {
            // 前の候補よりレイの始点に近ければ、この面を新しい床の候補にする。
            if (tmp.distance < bestHit.distance) {
                bestHit = tmp;
            }
        }
    }

    // 5. Undo(Ctrl+Z) 用に移動前の状態を保存
    nlohmann::json beforeState = CaptureObjectState(selectedObject_);

    // 6. 実際の移動処理
    if (bestHit.isHit) {
        // 真下にオブジェクトがあった場合、その表面に乗る
        selectedObject_->SetTranslateY(bestHit.point.y + yOffset);
        DebugConsole::GetInstance()->AddLog("Dropped to Object!");
    }
    else {
        // 真下に何もない場合は、Y=0 の床に乗る
        selectedObject_->SetTranslateY(yOffset);
        DebugConsole::GetInstance()->AddLog("Dropped to Floor (Y=0)!");
    }

    selectedObject_->UpdateLocalMatrix();
    selectedObject_->UpdateWorldMatrix();
    RegisterObjectEdited(selectedObject_, beforeState, "Drop To Floor");
}

// 選択したモデルのメッシュを描画用の子へ分け、選択をルートに戻す。
// ========================================================================
void DebugEditor::SplitSelectedModelIntoMeshChildren() {
    // 元Objectは親として残し、Meshごとの描画Objectだけを子へ切り分ける。
    if (!selectedObject_ || !sceneManager_ || !sceneManager_->GetCurrentScene()) {
        return;
    }

    BaseScene* currentScene = sceneManager_->GetCurrentScene();
    engine::Object3DCommon* objectCommon = currentScene->GetObject3DCommon();
    engine::Model* sourceModel = selectedObject_->GetModel();
    const std::string sourceModelName = selectedObject_->GetModelName();

    // 描画の共通情報、元モデル、モデル名のいずれかが取れなければ分割できない。
    if (!objectCommon || !sourceModel || sourceModelName.empty()) {
        DebugConsole::GetInstance()->AddLog("Mesh split skipped: selected object has no model.");
        return;
    }

    const uint32_t meshCount = sourceModel->GetMeshCount();

    // メッシュが一つ以下なら分割先を作る必要がない。
    if (meshCount <= 1) {
        DebugConsole::GetInstance()->AddLog("Mesh split skipped: model has only one mesh.");
        return;
    }

    // すでに単一メッシュの表示に絞っている物体は、分割済みの部品として扱う。
    if (selectedObject_->IsMeshDrawFiltered()) {
        DebugConsole::GetInstance()->AddLog("Mesh split skipped: selected object is already a mesh part.");
        return;
    }

    engine::Object3D* rootObject = selectedObject_;
    const std::string rootName = rootObject->GetName().empty() ? "MeshRoot" : rootObject->GetName();
    const nlohmann::json beforeState = CaptureObjectState(rootObject);

    // 分割後も見た目が変わらないよう、ルートのMaterial設定を各Meshへ複製する。
    const std::string saveCategory = rootObject->GetSaveCategory();
    const engine::Vector4 color = rootObject->GetColor();
    const BlendMode blendMode = rootObject->GetBlendMode();
    const int32_t materialType = rootObject->GetMaterialType();
    const float metallic = rootObject->GetMetallic();
    const float roughness = rootObject->GetRoughness();
    const bool enableNormalMap = rootObject->GetEnableNormalMap();
    const std::string normalMapPath = rootObject->GetNormalMapPath();
    const std::string ormMapPath = rootObject->GetOrmMapPath();
    const std::string texturePath = rootObject->GetTexturePath();
    const engine::Vector2 textureTiling = rootObject->GetTextureTiling();
    const bool autoTextureTiling = rootObject->GetAutoTextureTiling();
    const bool enableLighting = rootObject->GetEnableLighting();
    const bool enableEnvMap = rootObject->GetEnableEnvMap();
    const float envIntensity = rootObject->GetEnvIntensity();
    const float emissive = rootObject->GetEmissive();

    bool hasWaterParam = false;
    engine::MeshRenderer::WaterParamForGpu waterParam{};

    // ルートに材質パラメーターがあれば、子へ複製できるよう現在値を控える。
    if (rootObject->GetMeshRenderer() && rootObject->GetMeshRenderer()->GetWaterParamData()) {
        waterParam = *rootObject->GetMeshRenderer()->GetWaterParamData();
        hasWaterParam = true;
    }

    std::vector<std::unique_ptr<engine::Object3D>> meshChildren;
    std::vector<std::string> reservedNames;
    meshChildren.reserve(meshCount);

    for (uint32_t meshIndex = 0; meshIndex < meshCount; ++meshIndex) {
        // 子は該当Meshだけ描画し、移動・回転はルートから継承させる。
        auto child = std::make_unique<engine::Object3D>();
        child->Initialize(objectCommon);

        const std::string childBaseName = rootName + "_mesh" + std::to_string(meshIndex);
        const std::string childName = MakeUniquePresetObjectName(currentScene, childBaseName, reservedNames);
        reservedNames.push_back(childName);

        child->SetName(childName);
        child->SetClassName("MeshPart");
        child->SetSaveCategory(saveCategory);
        child->SetModel(sourceModelName);
        child->SetMeshDrawIndex(static_cast<std::int32_t>(meshIndex));
        child->SetColor(color);
        child->SetBlendMode(blendMode);
        child->SetMaterialType(materialType);
        child->SetMetallic(metallic);
        child->SetRoughness(roughness);
        child->SetEnableNormalMap(enableNormalMap);
        child->SetNormalMap(normalMapPath);
        child->SetOrmMap(ormMapPath);
        child->SetTexture(texturePath);
        child->SetTextureTiling(textureTiling);
        child->SetAutoTextureTiling(autoTextureTiling);
        child->SetEnableLighting(enableLighting);
        child->SetEnableEnvMap(enableEnvMap);
        child->SetEnvIntensity(envIntensity);
        child->SetEmissive(emissive);

        // 元から値を取得済みで、子に書き込み先がある場合だけ、材質パラメーターを複製する。
        if (hasWaterParam && child->GetMeshRenderer() && child->GetMeshRenderer()->GetWaterParamData()) {
            *child->GetMeshRenderer()->GetWaterParamData() = waterParam;
        }

        // 子は描画専用。見た目を移した後も、旧モデルの当たり判定は子へ複製しない。
        child->SetColliderType(engine::ColliderType::kNone);
        child->SetCollisionAttribute(0);
        child->SetCollisionMask(0);
        child->SetStatic(rootObject->IsStatic());
        // 分割前と同じ座標に描くため、子自身はローカル原点・単位スケールに置き、ルートの姿勢を継承する。
        child->SetTranslate({ 0.0f, 0.0f, 0.0f });
        child->SetRotation({ 0.0f, 0.0f, 0.0f });
        child->SetScale({ 1.0f, 1.0f, 1.0f });
        child->SetParent(rootObject);
        child->UpdateLocalMatrix();
        child->UpdateWorldMatrix();

        meshChildren.push_back(std::move(child));
    }

    rootObject->SetClassName("MeshRoot");

    // ルートの描画とColliderを外し、子との二重描画・二重判定を防ぐ。
    rootObject->SetModel(nullptr);
    rootObject->SetMeshDrawIndex(-1);
    rootObject->SetColliderType(engine::ColliderType::kNone);
    rootObject->SetCollisionAttribute(0);
    rootObject->SetCollisionMask(0);
    rootObject->UpdateLocalMatrix();
    rootObject->UpdateWorldMatrix();
    RegisterObjectEdited(rootObject, beforeState, "Split Model Root");

    // ルートの変更を登録してから子をシーンへ渡し、階層で元の Object を選択した状態へ戻す。
    AddEditorObjects(std::move(meshChildren), "Split Model Mesh Parts");
    SetSelectedObject(rootObject);
    EditorManager::GetInstance()->SetSelectedObject(this);
    MarkDirtyForObject(rootObject);

    DebugConsole::GetInstance()->AddLog("Split model into mesh children: " + rootName + " (" + std::to_string(meshCount) + " meshes)");
}

void DebugEditor::InstantiateModelAtCursor(const std::string& modelName) {
    // 配置先のシーンがなければモデルを生成しない。
    if (!sceneManager_ || !sceneManager_->GetCurrentScene())
        return;
    BaseScene* currentScene = sceneManager_->GetCurrentScene();

    // ドロップされたアセットを、独立したモデルオブジェクトとして読み込む。
    auto newObj = std::make_unique<engine::Object3D>();
    newObj->Initialize(currentScene->GetObject3DCommon());
    ModelManager::GetInstance()->LoadModel(modelName);
    newObj->SetModel(modelName);
    newObj->SetClassName("Model");
    newObj->SetName(modelName + "_" + std::to_string(currentScene->GetObjects().size()));

    // マウス下の面を探し、モデルの底面が合う配置を計算する。
    newObj->UpdateWorldMatrix();
    const auto placement = CalculateGameViewPlacement(newObj.get(), gameViewMousePos_, true);
    // 配置できる点が見つからなければ、仮生成したモデルをシーンへ追加しない。
    if (!placement.found) return;
    ApplyGameViewPlacement(newObj.get(), placement, newObj.get()->IsDecal());

    // 配置済みオブジェクトをシーンに登録する。
    AddEditorObject(std::move(newObj), "Drop 3D Model");

    DebugConsole::GetInstance()->AddLog("Dropped 3D Model: " + modelName);
}

void DebugEditor::InstantiatePresetAtCursor(const std::string& presetName) {
    // プリセットに含まれる親子関係を保ったまま、ルートだけ配置点へ移動する。
    if (!sceneManager_ || !sceneManager_->GetCurrentScene())
        return;
    BaseScene* currentScene = sceneManager_->GetCurrentScene();

    engine::Object3DCommon* common = currentScene->GetObject3DCommon();
    // シーンに描画用の共通情報がなければ、プリセットの物体を生成できない。
    if (!common)
        return;

    auto presetObjects = PresetManager::GetInstance()->CreateObjectsFromPreset(presetName, common);
    // プリセットから配置物が得られなければ、名前付けや座標合わせを行わない。
    if (presetObjects.empty())
        return;

    AssignPresetInstanceNames(currentScene, presetName, presetObjects, PresetRootName::kPathLeaf);

    // 親子関係を保った配置候補を求める。ルートがデカールなら面への向きも合わせる。
    const auto placement = PlaceGameViewObjects(presetObjects, gameViewMousePos_, true);
    // 配置点が得られないときは、プリセット全体を追加せず破棄する。
    if (!placement.found) return;

    AddEditorObjects(std::move(presetObjects), "Drop Preset");

    DebugConsole::GetInstance()->AddLog("Dropped Preset: " + presetName);
}

void DebugEditor::InstantiatePrefabAtCursor(const std::string& prefabName) {
    // Prefabもプリセットと同じRay配置規則を使い、インスタンスの親子構造を維持する。
    if (!sceneManager_ || !sceneManager_->GetCurrentScene())
        return;
    BaseScene* currentScene = sceneManager_->GetCurrentScene();

    engine::Object3DCommon* common = currentScene->GetObject3DCommon();
    // 描画用の共通情報を取得できる場合にだけ、Prefab の物体を生成する。
    if (!common)
        return;

    auto prefabObjects = PresetManager::GetInstance()->CreateObjectsFromPrefab(prefabName, common);
    // Prefab から配置物が得られなければ、名前付けや座標合わせを行わない。
    if (prefabObjects.empty())
        return;

    AssignPresetInstanceNames(currentScene, prefabName, prefabObjects, PresetRootName::kPathLeaf);

    // ルートを基準にグループの底面を配置点へ合わせる。ルートがデカールなら面への向きも合わせる。
    const auto placement = PlaceGameViewObjects(prefabObjects, gameViewMousePos_, true);
    // 配置点が得られないときは、Prefab 全体を追加せず破棄する。
    if (!placement.found) return;

    AddEditorObjects(std::move(prefabObjects), "Drop Prefab");

    DebugConsole::GetInstance()->AddLog("Dropped Prefab: " + prefabName);
}


void DebugEditor::InstantiateParticleAtCursor(const std::string& particleName) {
    // 配置先のシーンがなければ、粒子の編集用オブジェクトを生成しない。
    if (!sceneManager_ || !sceneManager_->GetCurrentScene())
        return;
    BaseScene* currentScene = sceneManager_->GetCurrentScene();

    // GPU 粒子の設定名を保持する編集用オブジェクトを用意する。
    auto newObj = std::make_unique<engine::Object3D>();
    newObj->Initialize(currentScene->GetObject3DCommon());
    
    // 粒子そのものに選択できるメッシュがないため、編集時の配置目印として仮モデルを持たせる。
    newObj->SetModel("Stages/block"); 
    newObj->SetName("VFX_" + particleName + "_" + std::to_string(currentScene->GetObjects().size())); 
    newObj->SetClassName("GPUParticle");
    newObj->SetGpuParticleName(particleName);
    newObj->SetColor({1.0f, 0.5f, 0.0f, 0.5f});
    newObj->SetBlendMode(BlendMode::kNormal);

    // 配置用モデルの底面を基準に、マウス下の面へ置ける位置を求める。
    newObj->UpdateWorldMatrix();
    const auto placement = CalculateGameViewPlacement(newObj.get(), gameViewMousePos_, true);
    // 配置点が得られなければ、粒子の編集用オブジェクトを追加しない。
    if (!placement.found) return;
    ApplyGameViewPlacement(newObj.get(), placement, newObj.get()->IsDecal());

    // 位置を確定した編集用オブジェクトを登録する。
    AddEditorObject(std::move(newObj), "Drop Particle");

    DebugConsole::GetInstance()->AddLog("Dropped Particle: " + particleName);

}
