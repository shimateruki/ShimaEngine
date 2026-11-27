#include "game/system/combat/LockOnSystem.h"

#include "Camera.h"
#include "Player.h"
#include "InputManager.h"
#include "CollisionManager.h"
#include "CollisionConfig.h"
#include "BaseEnemy.h"
#include "SceneManager.h"
#include <algorithm>
#include <cmath>
namespace {
// ロックオンの照準点として、ワールドAABBの中心を返します。
engine::Vector3 AimCenter(engine::Object3D* object) {
    // 原点や固定の高さではなく、サイズ・オフセットを反映した判定中心を狙います。
    const engine::Aabb bounds = object->GetAabb();
    return (bounds.min + bounds.max) * 0.5f;
}

bool Available(engine::Object3D* object) {
    // 照準候補は、敵属性を持つ表示中の敵で、運搬中・死亡・撃破演出中のいずれでもないものに限る。
    auto* enemy = dynamic_cast<BaseEnemy*>(object);
    return enemy && !enemy->IsDead() && enemy->GetIsVisible() && (enemy->GetCollisionAttribute() & engine::collisionCategories::Get("enemy")) &&
           !enemy->IsCarried() && !enemy->IsDefeatEffectPlaying();
}

bool Visible(Player* player, engine::Object3D* target) {
    // プレイヤーと対象の照準中心を結ぶレイで、地形が視線を遮るか調べる。
    const engine::Vector3 start = AimCenter(player);
    const engine::Vector3 delta = AimCenter(target) - start;
    const float distance = engine::Math::Length(delta);
    // 二つの照準点がほぼ同じ位置なら、ゼロに近い方向でレイを作らず見通せると扱う。
    if (distance < 0.001f)
        return true;
    // 視線の遮蔽は地形だけを調べ、照射元のプレイヤーは除外する。
    engine::PhysicsQueryFilter filter;
    filter.mask = engine::collisionCategories::Get("ground");
    filter.ignoredObject = player;
    return !engine::CollisionManager::GetInstance()->Raycast(start, delta / distance, distance, filter).isHit;
}
} // namespace

// カメラとプレイヤーから見える候補を評価し、ロック対象を選びます。対象消失時は照準を解除します。

LockOnSystem::LockOnSystem() = default;
LockOnSystem::~LockOnSystem() = default;
void LockOnSystem::Initialize(engine::InputManager* inputManager) {
    // 入力の所有権は移さず、ロックオン操作の参照先として保持する。
    inputManager_ = inputManager;
}

void LockOnSystem::ResetLockOn(engine::Camera* camera) {
    // 対象参照と見失い時計を同時に消し、カメラ側の追従も解除する。
    const bool active = isLockingOn_;
    isLockingOn_ = false;
    lockOnTarget_ = nullptr;
    lostSightTimer_ = 0;

    // 解除直前までロックオン中でカメラも存在する場合は、現在の見た目の回転を引き継ぎながら追従先を外す。
    if (camera && active) {
        camera->SyncRotationToCurrentView();
        camera->SetLockOnTarget(nullptr);
        // 現在の追従モードもロックオンなら、通常の照準操作へ戻す。
        if (camera->GetFollowMode() == engine::Camera::FollowMode::kLockOn)
            camera->SetFollowMode(engine::Camera::FollowMode::kAimable);
    }
}

void LockOnSystem::Update(const std::vector<std::unique_ptr<engine::Object3D>>& objects, engine::Camera* camera, Player* player,
                          float deltaTime) {
    // プレイヤー・カメラ・入力の参照のいずれかがない場合は、対象や操作を更新できない。
    if (!player || !camera || !inputManager_)
        return;
    // プレイヤー側の攻撃補助は先に無効化し、有効な対象を確認できたフレームだけ戻します。
    player->SetLockOn(false);
    player->SetLockOnAimPoint({}, false);
    // 編集停止中・死亡・演出ロック・ゲート帰還中は、今のロックを解除する。
    if (!SceneManager::GetInstance()->IsPlaying() || player->IsDead() || player->IsCinematicLocked() ||
        player->IsGateReturnAnimationActive()) {
        ResetLockOn(camera);
        return;
    }

    // 停止中や無効な経過時間では、入力切替と見失い判定を進めません。
    if (deltaTime <= 0 || !std::isfinite(deltaTime))
        return;
    // 対象の参照を持ち越している場合は、使う前に現在のシーンで条件を再確認する。
    if (lockOnTarget_) {
        // 対象を参照する前に、シーンに残っていることを確認します。
        const bool exists = std::any_of(objects.begin(), objects.end(),
                                        [&](const auto& object) { return object.get() == lockOnTarget_; });
        // 対象が消えた、照準できなくなった、または解除距離を超えた場合は、追従を解除する。
        if (!exists || !Available(lockOnTarget_) ||
            engine::Math::Length(lockOnTarget_->GetWorldPosition() - player->GetWorldPosition()) >
                kMaxLockOnDistance_ * 1.25f) {
            ResetLockOn(camera);
            return;
        }
    }

    // ロックオンの入力を受けた場合は、現在の状態に応じて開始か解除を選ぶ。
    if (inputManager_->IsActionTriggered("LockOn")) {
        // すでにロック中であれば、その入力は解除として扱う。
        if (isLockingOn_) {
            ResetLockOn(camera);
            return;
        }

        lockOnTarget_ = FindBestTarget(objects, camera, player);
        isLockingOn_ = lockOnTarget_ != nullptr;
    // ロック中に対象送りだけが押された場合は、今のロックを基準に次の候補を探す。
    } else if (isLockingOn_ && inputManager_->IsActionTriggered("LockOnNext")) {
        // 次の候補を見つけたときだけ、対象と見失い時間を更新する。
        if (auto* next = FindBestTarget(objects, camera, player, true)) {
            lockOnTarget_ = next;
            lostSightTimer_ = 0;
        }
    }

    // 有効な対象とのロックが成立していなければ、遮蔽判定やカメラ追従は行わない。
    if (!isLockingOn_ || !lockOnTarget_)
        return;
    const bool visible = Visible(player, lockOnTarget_);

    // 見通せるフレームで待ち時間をリセットし、遮られていた時間だけを連続して数える。
    lostSightTimer_ = visible ? 0 : lostSightTimer_ + deltaTime;

    // 見失った状態が0.5秒連続した場合は、遮蔽の猶予を終えてロックを解除する。
    if (lostSightTimer_ >= 0.5f) {
        ResetLockOn(camera);
        return;
    }

    // 遮蔽の猶予内ではカメラを対象へ向け、攻撃の照準補助は見通せる場合だけ設定します。
    camera->SetFollowMode(engine::Camera::FollowMode::kLockOn);
    camera->SetLockOnTarget(lockOnTarget_);
    player->SetLockOn(true);

    player->SetLockOnAimPoint(AimCenter(lockOnTarget_), visible);
}

engine::Object3D* LockOnSystem::FindBestTarget(const std::vector<std::unique_ptr<engine::Object3D>>& objects, engine::Camera* camera,
                                       Player* player, bool next) {
    // ロックオン候補の借用参照と選択用スコア。候補を集めてから順序付けする。
    struct Candidate {
        engine::Object3D* object;
        float score;
    };

    std::vector<Candidate> candidates;
    // カメラの視線を正規化し、視点と注視点が重なる場合はワールドの+Zを前にします。
    engine::Vector3 forward = camera->GetTargetPoint() - camera->GetEye();
    const float length = engine::Math::Length(forward);
    forward = length > 0.001f ? forward / length : engine::Vector3{0, 0, 1};

    for (const auto& object : objects) {
        // 敵以外、死亡・非表示・運搬中・撃破演出中などは、照準候補の評価から外す。
        if (!Available(object.get()))
            continue;
        // プレイヤー自身にぶら下がっているオブジェクトを、親階層をたどって候補から外します。
        bool owned = false;

        for (auto* parent = object.get(); parent; parent = parent->GetParent())
            // 候補から親をたどった先にプレイヤーがいれば、プレイヤーの所有物と判定する。
            if (parent == player) {
                owned = true;
                break;
            }

        // プレイヤーの親子階層に含まれる物体を、ロック対象には選ばない。
        if (owned)
            continue;
        // プレイヤーからの距離で先に絞り、カメラ正面かつ地面に遮られない候補を採点します。
        const float distance = engine::Math::Length(object->GetWorldPosition() - player->GetWorldPosition());
        // 最大射程を超えるか、プレイヤーに近すぎる対象は、角度や遮蔽の評価を省く。
        if (distance > kMaxLockOnDistance_ || distance < 0.1f)
            continue;
        const engine::Vector3 delta = object->GetWorldPosition() - camera->GetEye();
        const float cameraDistance = engine::Math::Length(delta);
        // 視点と対象がほぼ重なる場合は正規化できる視線が作れないので飛ばす。
        if (cameraDistance < 0.001f)
            continue;
        const float dot = engine::Math::Dot(forward, delta / cameraDistance);
        // カメラの視線から約60度を超えて外れるか、プレイヤーとの間が地形で遮られていれば候補から外す。
        if (dot < 0.5f || !Visible(player, object.get()))
            continue;
        // 角度のずれを2倍、距離を最大射程で割って足す。小さいほど画面中央寄りで近い候補。
        candidates.push_back({object.get(), (1 - dot) * 2 + distance / kMaxLockOnDistance_});
    }

    // 距離・角度・遮蔽を通った候補がなければ、対象なしを返す。
    if (candidates.empty())
        return nullptr;

    // 手動切替は安定したシーン順で巡回し、カメラ移動で同じ2体を往復させません。
    if (next) {
        for (size_t i = 0; i < candidates.size(); ++i)
            // 現在の対象が一覧に残っていれば次の要素を返し、末尾からは先頭へ戻る。
            if (candidates[i].object == lockOnTarget_)
                return candidates[(i + 1) % candidates.size()].object;
    }

    // 通常のロック開始時と、対象送りで現在の対象を見つけられない場合は、スコアの小さい順に候補を選ぶ。
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const auto& a, const auto& b) { return a.score < b.score; });

    // 正面に近く、かつプレイヤーから近い候補を優先する。
    return candidates.front().object;
}
