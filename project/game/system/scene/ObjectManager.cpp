#ifdef USE_IMGUI
#include <cstdint>
#include "engine/editor/preview/SelectedActorSession.h"
#endif
#include "game/system/scene/ObjectManager.h"
#include "CollisionManager.h"
#include "CameraManager.h"
#include "LightManager.h"
#include "RenderStats.h"
#include "GimmickMovingFloor.h"
#include "GimmickSinkingFloor.h"
#include "game/actor/gimmick/stage/GimmickLiquidLevel.h"
#include "Player.h"
#include "engine/utility/math/Math.h"
#include <string>
#include <algorithm> // remove_if用
#include <unordered_set>

namespace {
// 透明面を除いたStageGateの枠だけを通常の不透明描画へ送ります。
bool ShouldDrawStageGateFrame(const std::unique_ptr<engine::Object3D>& obj) {
    // 物体を取得できない場合や、ゲート面と同じ材質番号22を使っていない場合は、この例外描画の対象外。
    if (!obj || obj->GetMaterialType() != 22)
        return false;
    // StageGateのモデルだけを枠か面かの追加判定へ渡す。
    if (obj->GetGimmickType() != "StageGate")
        return false;
    const std::string modelName = obj->GetModelName();
    // 名前があり、透明面を表すportal_surfaceを含まなければ、枠として扱う。
    return !modelName.empty() && modelName.find("portal_surface") == std::string::npos;
}
} // namespace

// シーン内オブジェクトの所有と更新順を管理します。生成済み参照を使う側との寿命の境界に注意します。

void ObjectManager::Update(float deltaTime) {
    bool selectedActorUpdating = false;
#ifdef USE_IMGUI
    selectedActorUpdating = SelectedActorSession::Current() && SelectedActorSession::Current()->IsActive();
#endif

    // 入力を処理しないフレームが挟まっても、古い床の速度を持ち越さない。
    for (auto& obj : objects_) {
        // リプレイで除外されていないプレイヤーだけ、前フレームの床速度を初期化する。
        if (auto* player = obj && !obj->IsReplayRemoved() ? dynamic_cast<Player*>(obj.get()) : nullptr)
            player->SetPlatformMotion({});
    }

    // 上昇する液面をPlayerの死亡判定より先に更新します。
    for (auto& obj : objects_) {
        // 更新対象の液面だけを先に進め、プレイヤーの死亡判定が今フレームの高さを使えるようにする。
        if (auto* liquid = obj && !obj->IsReplayRemoved() ? dynamic_cast<GimmickLiquidLevel*>(obj.get()) : nullptr)
            liquid->Update(deltaTime);
    }

    // 全移動床を乗員の物理より先に動かす。従来型も下降時の接地を保つ。
    for (auto& obj : objects_) {
        auto* floor = obj && !obj->IsReplayRemoved() ? dynamic_cast<GimmickMovingFloor*>(obj.get()) : nullptr;
        // 有効な移動床は、乗っている物体の物理処理より先に進める。
        if (floor)
            floor->Update(deltaTime);
        // 移動床に当てはまらない物体のうち、沈む床の駆動モード1もここで先行更新する。
        else if (auto* sinking =
                     obj && !obj->IsReplayRemoved() ? dynamic_cast<GimmickSinkingFloor*>(obj.get()) : nullptr;
                 sinking && sinking->GetParameters() && sinking->GetParameters()->actionMode == 1)
            sinking->Update(deltaTime);
    }

    // 先行更新した液面・移動床を除き、各オブジェクトのロジックを一度ずつ進めます。
    for (auto& obj : objects_) {
        // 空参照・リプレイで除外済み・固定刻みで進めるエディター内の物体は、通常の更新から外す。
        if (!obj || obj->IsReplayRemoved() || (selectedActorUpdating && obj->IsEditorInternal()))
            continue;
        // 移動床と液面はすでに先に進めたので、このループでは更新しない。
        if (dynamic_cast<GimmickMovingFloor*>(obj.get()) || dynamic_cast<GimmickLiquidLevel*>(obj.get()))
            continue;
        // 沈む床の駆動モード1も先行更新済みなので、二重に進めない。
        if (dynamic_cast<GimmickSinkingFloor*>(obj.get()) && obj->GetParameters() && obj->GetParameters()->actionMode == 1)
            continue;
        obj->Update(deltaTime);
    }


    // 全ロジック更新後に Transform と描画定数を確定します。
    // 親を持つObjectは親側の再帰更新に含め、同じMeshRenderer::Updateが
    // 1フレームに複数回走らないようにする。
    std::unordered_set<engine::Object3D*> managedObjects;
    managedObjects.reserve(objects_.size());

    for (const auto& obj : objects_) {
        // 実体のある管理物体だけを、親の参照が管理内にあるか調べる集合に入れる。
        if (obj) {
            managedObjects.insert(obj.get());
        }
    }

    for (auto& obj : objects_) {
        // 空参照やリプレイから除外された物体には、描画用の行列を計算しない。
        if (!obj || obj->IsReplayRemoved())
            continue;

        engine::Object3D* parent = obj->GetParent();
        const bool hierarchyRoot = parent == nullptr || managedObjects.find(parent) == managedObjects.end();

        // 親がいないか管理外にいる物体を起点に更新する。管理内の親を持つ子は親の再帰に任せる。
        if (hierarchyRoot) {
            obj->UpdateWorldMatrix();
        }
    }

    // 走査と行列更新が完了した時点で、フレーム中の追加・削除要求をまとめて反映します。
    ApplyPendingChanges();
}

void ObjectManager::ApplyPendingChanges() {
    // 走査中に生成した物体があれば、走査を終えた後で管理リストへ移す。
    if (!pendingObjects_.empty()) {
        for (auto& pendingObj : pendingObjects_) {
            // 既存Objectの行列確定後に追加されるため、生成した位置・縮小率を初回描画前に転送する。
            // Updateは呼ばず、AI・物理・導火線を同じフレームに二度進めない。
            pendingObj->UpdateWorldMatrix();
            objects_.push_back(std::move(pendingObj));
        }

        pendingObjects_.clear();
    }

    // 削除予約も走査の最後にまとめて反映します。
    ProcessRemovals();
}

void ObjectManager::Draw(ID3D12Resource* pointLight, ID3D12Resource* spotLight) {
    // 描画側が通常パスを設定した後に、管理物体を不透明と透明の順に描く。

    // 不透明描画
    for (auto& obj : objects_) {
        // 非表示の物体とカメラ用オブジェクトは、不透明パスへ渡さない。
        if (!obj->GetIsVisible() || obj->IsCameraObject())
            continue;
        const std::int32_t materialType = obj->GetMaterialType();
        const bool drawStageGateFrame = ShouldDrawStageGateFrame(obj);

        // ゲートの枠と不透明パスで扱う材質を描く。スライム用の25もここに含める。
        // 偽王のDrawは所有する槍・光刃も描くため、ここで除外すると攻撃だけが消える。
        if (drawStageGateFrame ||
            (materialType != 1 && materialType != 7 &&
             (materialType < 8 || materialType == 23 || materialType == 24 || materialType == 25))) {
            obj->Draw(pointLight, spotLight);
        }
    }

    // 透明描画
    for (auto& obj : objects_) {
        // 透明パスでも、非表示の物体とカメラ用オブジェクトを飛ばす。
        if (!obj->GetIsVisible() || obj->IsCameraObject())
            continue;
        // この透明パスは材質番号1を描く。特殊材質のパスは描画側で別に進む。
        if (obj->GetMaterialType() == 1) {
            obj->Draw(pointLight, spotLight);
        }
    }
}

void ObjectManager::AddObject(std::unique_ptr<engine::Object3D> object) {
    // 追加する実体がなければ、衝突登録も追加予約も行わない。
    if (!object)
        return;

    // 衝突判定に登録
    engine::CollisionManager::GetInstance()->AddObject(object.get());

    // 追加待ちリストへ
    pendingObjects_.push_back(std::move(object));
}

engine::Object3D* ObjectManager::AddObjectImmediate(std::unique_ptr<engine::Object3D> object) {
    // 即時追加する実体がなければ、利用側に渡せる参照もないのでnullを返す。
    if (!object)
        return nullptr;
    // 即時登録も、次の通常更新を待たず正しい姿勢で描画できる状態にする。
    object->UpdateWorldMatrix();
    engine::Object3D* raw = object.get();
    engine::CollisionManager::GetInstance()->AddObject(raw);
    objects_.push_back(std::move(object));
    return raw;
}

std::unique_ptr<engine::Object3D> ObjectManager::TakeObjectImmediate(engine::Object3D* object) {
    // 移譲する物体の参照がなければ、現在の管理リストを変更せず返す。
    if (!object)
        return nullptr;
    auto takeFrom = [object](std::vector<std::unique_ptr<engine::Object3D>>& storage) -> std::unique_ptr<engine::Object3D> {
        auto it = std::find_if(storage.begin(), storage.end(), [object](const auto& candidate) {
            return candidate.get() == object;
        });
        // 指定した参照がこのリストにない場合は、取り出せる所有権がないことを返す。
        if (it == storage.end())
            return nullptr;
        std::unique_ptr<engine::Object3D> result = std::move(*it);
        storage.erase(it);
        return result;
    };

    std::unique_ptr<engine::Object3D> result = takeFrom(objects_);
    // 通常の管理リストにない場合は、同じフレームの追加待ちリストにも探しに行く。
    if (!result)
        result = takeFrom(pendingObjects_);
    // どちらのリストにもなければ、衝突と削除予約を変更せず返す。
    if (!result)
        return nullptr;
    // 所有権を渡す前に、この管理器の衝突登録と削除予約から同じ参照を外す。
    engine::CollisionManager::GetInstance()->RemoveObject(object);
    removalList_.erase(std::remove(removalList_.begin(), removalList_.end(), object), removalList_.end());
    return result;
}

// 所有中のObjectだけ削除予約し、同フレームの描画を防ぐため先に非表示へします。
void ObjectManager::RequestRemove(engine::Object3D* object) {
    // 参照が空なら、削除のための検索や表示変更は行わない。
    if (!object)
        return;

    const auto matches = [object](const std::unique_ptr<engine::Object3D>& candidate) {
        return candidate && candidate.get() == object;
    };

    // 現在の管理分と追加待ちのどちらかで所有しているかを調べる。
    const bool exists = std::any_of(objects_.begin(), objects_.end(), matches) ||
                        std::any_of(pendingObjects_.begin(), pendingObjects_.end(), matches);
    // どちらでも所有していない物体は、この管理器で削除予約できない。
    if (!exists)
        return;

    // すでに同じ物体を予約した場合は、非表示化と予約を繰り返さない。
    if (std::find(removalList_.begin(), removalList_.end(), object) != removalList_.end()) {
        return;
    }

    // 同じフレームの残りで描画されないよう、所有権を外す前に非表示にする。
    object->SetIsVisible(false);
    removalList_.push_back(object);
}

// 削除予約を重複排除し、衝突登録を外してから所有コンテナから取り除きます。
// リプレイ保持中のObjectは破棄せず、除外状態にして復元可能な寿命を保ちます。
void ObjectManager::ProcessRemovals() {
    // 削除予約がないフレームは、管理リストや衝突登録を走査しない。
    if (removalList_.empty())
        return;

    engine::CollisionManager* colManager = engine::CollisionManager::GetInstance();

    std::sort(removalList_.begin(), removalList_.end());
    removalList_.erase(std::unique(removalList_.begin(), removalList_.end()), removalList_.end());

    auto isRequestedForRemoval = [this](engine::Object3D* object) {
        return std::binary_search(removalList_.begin(), removalList_.end(), object);
    };

    // CollisionManagerから登録解除
    for (engine::Object3D* obj : removalList_) {
        colManager->RemoveObject(obj);
    }

    // 削除要求が処理される時点で、追加待ちリストに残っている予約対象も取り除く。
    pendingObjects_.erase(
        std::remove_if(pendingObjects_.begin(), pendingObjects_.end(),
                       [&](const std::unique_ptr<engine::Object3D>& p) { return p && isRequestedForRemoval(p.get()); }),
        pendingObjects_.end());

    auto it = std::remove_if(objects_.begin(), objects_.end(), [&](const std::unique_ptr<engine::Object3D>& p) {
        // 参照が空か削除予約に含まれない場合は、この削除処理で取り除かない。
        if (!p || !isRequestedForRemoval(p.get())) {
            return false;
        }
        // リプレイで保持する物体は、死亡・非表示・リプレイ除外を記録したうえで所有し続ける。
        if (p->IsReplayRetained()) {
            p->SetReplayRemoved(true);
            p->SetIsVisible(false);
            p->SetDead(true);
            return false;
        }
        return true;
    });
    objects_.erase(it, objects_.end());

    removalList_.clear();
}

void ObjectManager::DrawShadow() {
    // 管理物体がなければ、ライト視錐台の計算と描画を行わない。
    if (objects_.empty())
        return;

    engine::Camera* camera = CameraManager::GetInstance()->GetActiveCamera();
    // 使用中のカメラから影投影の基準を取れない場合は、影を描けないので戻る。
    if (!camera)
        return;
    const engine::Matrix4x4& lightViewProjection = engine::LightManager::GetInstance()->GetDirectionalShadowViewProjection(camera);
    const engine::Frustum shadowFrustum = engine::Math::ExtractFrustumPlanes(lightViewProjection);

    // 影を実際に描く最初の物体でパスの共通状態を設定するため、初回を記録する。
    bool isFirst = true;

    // 影を落とす物体をライト視錐台で絞り、残った物体をシャドウマップへ描く。
    for (auto& obj : objects_) {
        // 非表示の物体とカメラの配置用オブジェクトには影を描かない。
        if (!obj->GetIsVisible() || obj->IsCameraObject())
            continue;
        // 影を落とす設定が無効な物体は、パスの対象から外す。
        if (!obj->GetCastShadow())
            continue;
        // 影パスで扱わない材質番号7の物体を飛ばす。
        if (obj->GetMaterialType() == 7)
            continue;

        // ライトの視錐台外にある物体はシャドウマップへ書き込まれないため除外する。
        // 画面外から影だけを落とす物体は残るので、メインカメラ基準より見た目も正確になる。
        engine::Aabb worldAabb = obj->GetModelWorldAabb();

        // モデルの外接箱がライト視錐台の外なら、除外数を記録して影の描画を飛ばす。
        if (!engine::Math::IntersectFrustumAabb(shadowFrustum, worldAabb.min, worldAabb.max)) {
            engine::RenderStats::GetInstance()->RecordCulledObject();
            continue;
        }

        // 視錐台を通過した最初の物体で影パスの共通状態を設定し、後続はその状態を使います。
        if (isFirst) {
            obj->SetShadowCommonState();
            isFirst = false;
        }

        obj->DrawShadowOnly();
    }
}
