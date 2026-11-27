#include "engine/editor/core/playmode/PlayModeSnapshot.h"

#ifdef USE_IMGUI

#include "BaseScene.h"
#include "Camera.h"
#include "CameraManager.h"
#include "CollisionManager.h"
#include "DebrisEffectManager.h"
#include "GpuParticleManager.h"
#include "MeshEffectManager.h"
#include "ParticleSystem.h"
#include "SceneManager.h"
#include "game/system/combat/BulletManager.h"

#include <unordered_map>
#include <unordered_set>

BaseScene* PlayModeSnapshot::CurrentScene() const {
    // アドレスが以前のシーンと同じでも、世代が違えば退避した状態を適用しない。
    if (!sceneManager_ || !scene_ || sceneManager_->IsTransitioning() ||
        sceneManager_->GetSceneGeneration() != sceneGeneration_ ||
        sceneManager_->GetCurrentScene() != scene_) {
        return nullptr;
    }
    return scene_;
}

void PlayModeSnapshot::Reset() {
    // 保存した時点と同じシーン世代に限り、保持していたオブジェクトに後片付けを適用する。
    if (BaseScene* scene = CurrentScene()) {
        // 退避中に実体を残していたObjectを通常管理へ戻し、削除済みの印が残るものだけ遅延削除へ渡す。
        for (auto& object : scene->GetObjects()) {
            // 参照が無いオブジェクトと、現在もシーンに残すオブジェクトは飛ばす。
            if (!object || !object->IsReplayRetained()) continue;
            object->SetReplayRetained(false);
            // 再生中に削除されたオブジェクトだけを、安全なシーン削除処理へ渡す。
            if (object->IsReplayRemoved()) scene->RequestRemoveObject(object.get());
        }
        scene->ReleaseReplaySprites();
    }
    // 後片付けの対象が無い場合も、保持していたスナップショットを破棄する。
    objects_.clear();
    sprites_.clear();
    sceneState_ = json::object();
    camera_ = {};
    scene_ = nullptr;
    sceneGeneration_ = 0;
}

bool PlayModeSnapshot::Capture() {
    Reset();
    // シーン管理が無いか遷移中なら、切替前後の状態が混ざるため記録を始めない。
    if (!sceneManager_ || sceneManager_->IsTransitioning()) return false;
    scene_ = sceneManager_->GetCurrentScene();
    // 現在のシーンを取得できない場合は、記録する対象がない。
    if (!scene_) return false;
    sceneGeneration_ = sceneManager_->GetSceneGeneration();

    // 保存前の編集内容も含める。ディスクのScene JSONへは書き込まない。
    for (auto& object : scene_->GetObjects()) {
        // 無効な参照とエディター専用オブジェクトを除き、ゲーム側のオブジェクトを記録する。
        if (!object || object->IsEditorInternal()) continue;
        // プレイ中に削除されても停止後に戻せるように実体を保持し、一覧順ではなくIDで識別する。
        object->SetReplayRetained(true);
        objects_.push_back({object->EnsureReplayId(), object->CaptureReplayState()});
    }
    std::vector<engine::Sprite*> currentSprites;
    scene_->CollectReplaySprites(currentSprites);
    // シーンとUIが同じSpriteを列挙する場合に備え、同じ実体の退避は一度に限る。
    std::unordered_set<engine::Sprite*> uniqueSprites;
    for (engine::Sprite* sprite : currentSprites) {
        // スプライトの無効な参照と、同じIDで記録済みのものを飛ばす。
        if (!sprite || !uniqueSprites.insert(sprite).second) continue;
        sprite->SetReplayRetained(true);
        sprites_.push_back({sprite->EnsureReplayId(), sprite->CaptureReplayState()});
    }
    scene_->CaptureReplaySceneState(sceneState_);
    // 開始前に実際に描いていた視点を控える。画角はラジアン、クリップ距離はワールド単位で保持する。
    // 操作中のカメラがある場合だけ、そのIDを復元用に控える。
    if (engine::Camera* camera = CameraManager::GetInstance()->GetActiveCamera()) {
        camera_.valid = true;
        camera_.eye = camera->GetEye();
        camera_.target = camera->GetTargetPoint();
        camera_.rotation = camera->GetRotation();
        camera_.fovY = camera->GetFovY();
        camera_.nearClip = camera->GetNearClip();
        camera_.farClip = camera->GetFarClip();
    }
    return true;
}

bool PlayModeSnapshot::Restore() {
    BaseScene* scene = CurrentScene();
    // シーン世代が変わっていた場合は、別のシーンへ保存状態を適用せず記録を捨てる。
    if (!scene) {
        Reset();
        return false;
    }

    // シーン固有の再生・UI状態を先に戻し、その後に現在の登録一覧から個体を照合する。
    scene->RestoreReplaySceneState(sceneState_);
    auto& currentObjects = scene->GetObjects();
    std::unordered_map<uint64_t, engine::Object3D*> objectsById;
    for (auto& object : currentObjects) {
        // 現在も有効なゲームオブジェクトだけを、IDから引けるようにする。
        if (object && !object->IsEditorInternal())
            objectsById[object->EnsureReplayId()] = object.get();
    }
    std::unordered_set<uint64_t> originalObjectIds;
    engine::CollisionManager* collisionManager = engine::CollisionManager::GetInstance();
    for (const ObjectState& saved : objects_) {
        originalObjectIds.insert(saved.id);
        const auto found = objectsById.find(saved.id);
        // 保存したIDに対応するオブジェクトが見つからない場合は、状態を適用できない。
        if (found == objectsById.end()) continue;
        engine::Object3D* object = found->second;
        // 復元前後で削除状態が変わったObjectだけ衝突管理の登録を更新する。
        const bool wasRemoved = object->IsReplayRemoved();
        object->RestoreReplayState(saved.state);
        // 再生中の削除状態から復帰したオブジェクトは、衝突判定にも再登録する。
        if (wasRemoved && !object->IsReplayRemoved()) collisionManager->AddObject(object);
        // 復元によって削除状態へ戻ったオブジェクトは、衝突判定から登録を外す。
        if (!wasRemoved && object->IsReplayRemoved()) collisionManager->RemoveObject(object);
    }

    // Play中に生成したObjectだけを撤去する。配列走査中の削除は避ける。
    std::vector<engine::Object3D*> runtimeObjects;
    for (const auto& object : currentObjects) {
        // 有効なゲームオブジェクトのうち保存IDが無いものは、再生中に増えたため削除候補にする。
        if (object && !object->IsEditorInternal() &&
            !originalObjectIds.contains(object->EnsureReplayId()))
            runtimeObjects.push_back(object.get());
    }
    for (engine::Object3D* object : runtimeObjects) scene->DestroyObject(object);

    // 親子Transformのローカル値を戻してからルートの行列を更新する。
    std::unordered_set<engine::Object3D*> managedObjects;
    for (const auto& object : currentObjects)
        // シーン管理に属する現在のオブジェクトだけを、親子関係の更新対象にする。
        if (object && !object->IsReplayRemoved()) managedObjects.insert(object.get());
    for (const auto& object : currentObjects) {
        // 元のオブジェクトを検索できない場合や削除されている場合は、更新しない。
        if (!object || object->IsReplayRemoved()) continue;
        engine::Object3D* parent = object->GetParent();
        // 親が復元対象内にいないオブジェクトを起点にし、配下まで階層を更新する。
        if (!parent || !managedObjects.contains(parent)) object->UpdateWorldMatrix();
    }

    // Spriteの各状態を先に全件戻し、後続の走査で親IDを解決して描画用行列を更新する。
    std::vector<engine::Sprite*> currentSprites;
    scene->CollectReplaySprites(currentSprites);
    std::unordered_map<uint64_t, engine::Sprite*> spritesById;
    for (engine::Sprite* sprite : currentSprites)
        // 有効なスプライトだけを、復元時にIDから参照できるようにする。
        if (sprite) spritesById[sprite->EnsureReplayId()] = sprite;
    std::unordered_set<uint64_t> originalSpriteIds;
    for (const SpriteState& saved : sprites_) {
        originalSpriteIds.insert(saved.id);
        const auto found = spritesById.find(saved.id);
        // 保存IDに対応するスプライトを現在も参照できる場合は、個別の状態を戻す。
        if (found != spritesById.end()) found->second->RestoreReplayState(saved.state);
    }
    for (const SpriteState& saved : sprites_) {
        const auto found = spritesById.find(saved.id);
        // スプライトが見つからない場合は、親の付け直しも飛ばす。
        if (found == spritesById.end()) continue;
        engine::Sprite* parent = nullptr;
        // 親IDが0以外の場合は、対応する親スプライトを現在の一覧から探す。
        if (saved.state.parentReplayId != 0) {
            const auto parentFound = spritesById.find(saved.state.parentReplayId);
            // 親IDが現在の一覧にある場合だけ、そのスプライトを親にする。
            if (parentFound != spritesById.end()) parent = parentFound->second;
        }
        // 復元したローカル座標を保つため、親の再接続でワールド位置を維持する補正は掛けない。
        found->second->SetParent(parent, false);
        found->second->RefreshAfterReplayRestore();
    }
    std::vector<engine::Sprite*> runtimeSprites;
    for (const auto& [id, sprite] : spritesById)
        // 再生前のID集合に無いスプライトを再生中の生成物として扱う。
        if (!originalSpriteIds.contains(id)) runtimeSprites.push_back(sprite);
    for (engine::Sprite* sprite : runtimeSprites) {
        // シーンが直接所有するSpriteは破棄する。個別UIの所有物は外から破棄せず、親接続と可視状態を解除する。
        if (scene->DestroySprite(sprite)) continue;
        sprite->SetParent(nullptr, false);
        sprite->SetReplayRemoved(true);
        sprite->SetVisible(false);
    }
    // 再生開始時のカメラIDが記録されている場合だけ、以前の選択に戻す。
    if (camera_.valid) {
        // 同じIDのカメラが現在も見つかる場合だけ、現在のカメラを選び直す。
        if (engine::Camera* camera = CameraManager::GetInstance()->GetActiveCamera()) {
            camera->ApplyReplayView(camera_.eye, camera_.target, camera_.rotation,
                camera_.fovY, camera_.nearClip, camera_.farClip);
        }
    }

    // 元の配置とカメラへ戻してから、プレイ中だけ残る弾や粒子を掃除して保持状態を解除する。
    BulletManager::GetInstance()->Clear();
    MeshEffectManager::GetInstance()->Clear();
    DebrisEffectManager::GetInstance()->Clear();
    GpuParticleManager::GetInstance()->ClearAllAutoEmitters();
    // CPUパーティクルの管理側がある場合は、再生中に残った粒子を消去する。
    if (engine::ParticleSystem* particles = scene->GetParticleSystem()) particles->Clear();
    Reset();
    return true;
}

#endif
