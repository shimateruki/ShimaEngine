#define NOMINMAX
#include <cstdint>
#include "CollisionManager.h"
#include "engine/utility/math/Math.h"
#include <cmath>
#include <algorithm> // std::find
#include <limits>
#include <unordered_map>

namespace engine {

CollisionManager* CollisionManager::GetInstance() {
    // シーン中の接触履歴とクエリ対象を共有する管理器を一つだけ返す。
    static CollisionManager sInstance;
    return &sInstance;
}

void CollisionManager::SetGridSize(float size) {
    // 無限大やNaN、0以下の幅、現在と同じ幅は受け付けず、静的グリッドもそのまま使う。
    if (!std::isfinite(size) || size <= 0.0f || size == gridSize_) {
        return;
    }

    // セル幅が変わると既存の静的オブジェクトのセル ID も変わります。
    gridSize_ = size;
    needsStaticGridRebuild_ = true;
}

namespace {
// 形状と双方向マスクを持ち、広域判定へ参加可能なObjectか判定します。
bool IsCollisionActive(engine::Object3D* object) {
    // オブジェクトまたはコライダーを取得できなければ、接触判定から外す。
    if (!object || !object->GetCollider()) {
        return false;
    }

    // コライダーがあっても形状が無効なら、交差判定の対象から外す。
    if (object->GetColliderType() == ColliderType::kNone) {
        return false;
    }

    // 属性またはマスクのどちらかが0なら、接触判定に参加させない。
    if (object->GetCollisionAttribute() == 0 || object->GetCollisionMask() == 0) {
        return false;
    }

    return true;
}

std::pair<engine::Object3D*, engine::Object3D*> MakeContactPair(engine::Object3D* objectA, engine::Object3D* objectB) {
    // ポインター順が逆なら入れ替え、検査順にかかわらず接触履歴を同じ組で引けるようにする。
    if (std::less<engine::Object3D*>{}(objectB, objectA)) {
        std::swap(objectA, objectB);
    }

    return { objectA, objectB };
}

bool IsTriggerObject(const engine::Object3D* object) {
    // カテゴリを使って物理接触と通知専用の接触を分ける。
    return object && (object->GetCollisionAttribute() & collisionCategories::Get("trigger")) != 0;
}

bool IsInIgnoredHierarchy(engine::Object3D* object, engine::Object3D* ignoredRoot) {
    // 自機本体を除外したクエリでは子のコライダーも一緒に除外する。
    for (engine::Object3D* current = object; current; current = current->GetParent()) {
        // 自分またはいずれかの親が除外の基点なら、そのオブジェクトも検索対象から外せる。
        if (current == ignoredRoot) {
            return true;
        }
    }

    return false;
}

engine::Vector3 SafeNormalize(const engine::Vector3& value) {
    // 長さゼロのレイで NaN を広めず、呼び出し側へ無効な方向を返す。
    const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);

    // 方向の長さがしきい値以下なら、除算せず無効な方向を示すゼロベクトルを返す。
    if (length <= 0.000001f) {
        return { 0.0f, 0.0f, 0.0f };
    }

    return value / length;
}
}

void CollisionManager::AddObject(engine::Object3D* object) {
    // 空参照と登録済みの参照を飛ばし、同じ物体が候補に重複しないようにする。
    if (!object || std::find(objects_.begin(), objects_.end(), object) != objects_.end()) {
        return;
    }

    objects_.push_back(object);

    // 次の更新で新しい配置を静的グリッドへ反映します。
    needsStaticGridRebuild_ = true;
}

void CollisionManager::RemoveObject(engine::Object3D* object) {
    auto it = std::find(objects_.begin(), objects_.end(), object);

    // 登録中の参照を見つけた場合に限り、リストを更新して静的グリッドの再構築を予約する。
    if (it != objects_.end()) {
        objects_.erase(it);

        needsStaticGridRebuild_ = true;
    }

    // 登録解除した参照が後の通知に残らないよう、前回分と今回分の両方から取り除く。
    const auto removeContacts = [object](auto& contacts) {
        for (auto it = contacts.begin(); it != contacts.end();) {
            // 登録を解除する物体が接触ペアのどちらかなら、今後の通知に参照を残さないよう消す。
            if (it->first.first == object || it->first.second == object) {
                it = contacts.erase(it);
            // 両方とも別の物体であれば、その履歴は残して次の組を調べる。
            } else {
                ++it;
            }
        }
    };

    removeContacts(previousContacts_);
    removeContacts(currentContacts_);
}

void CollisionManager::ClearObjects() {
    // シーン切り替えでは接触履歴も捨て、旧オブジェクトへの Exit 通知を防ぐ。
    objects_.clear();
    grid_.clear();
    checkedPairs_.clear();
    staticGrid_.clear();
    previousContacts_.clear();
    currentContacts_.clear();
    needsStaticGridRebuild_ = true;
}



// 座標オフセット。負の座標を正のインデックス空間にマップする
const float kGridOffset = 1048576.0f; // 2^20

// ワールド座標が属するセルのインデックスを求め、各軸を一つのIDにまとめる。
int64_t CollisionManager::GetGridId(const engine::Vector3& pos) {
    // 負座標を含む想定範囲を扱えるよう、各軸を同じセル数だけ正方向へずらす。
    float offsetX = pos.x + (kGridOffset * gridSize_);
    float offsetY = pos.y + (kGridOffset * gridSize_);
    float offsetZ = pos.z + (kGridOffset * gridSize_);

    std::int32_t x = static_cast<std::int32_t>(std::floor(offsetX / gridSize_));
    std::int32_t y = static_cast<std::int32_t>(std::floor(offsetY / gridSize_));
    std::int32_t z = static_cast<std::int32_t>(std::floor(offsetZ / gridSize_));

    return GetGridIdFromIndices(x, y, z);
}

// ワールド座標をAABBの登録範囲に使う三軸のセル番号へ変換します。
Vector3i CollisionManager::GetGridIndices(const engine::Vector3& pos) {
    // 負座標も同じセル幅で扱えるようオフセットしてから床関数で離散化する。
    float offsetX = pos.x + (kGridOffset * gridSize_);
    float offsetY = pos.y + (kGridOffset * gridSize_);
    float offsetZ = pos.z + (kGridOffset * gridSize_);

    std::int32_t x = static_cast<std::int32_t>(std::floor(offsetX / gridSize_));
    std::int32_t y = static_cast<std::int32_t>(std::floor(offsetY / gridSize_));
    std::int32_t z = static_cast<std::int32_t>(std::floor(offsetZ / gridSize_));
    return { x, y, z };
}


// 各軸のインデックスを下位21ビットに収め、一つのセルIDに詰める。
int64_t CollisionManager::GetGridIdFromIndices(std::int32_t x, std::int32_t y, std::int32_t z) {
    int64_t cellX = static_cast<int64_t>(x);
    int64_t cellY = static_cast<int64_t>(y);
    int64_t cellZ = static_cast<int64_t>(z);

    const int64_t mask = 0x1FFFFF; // 21ビットマスク

    int64_t maskedX = cellX & mask;
    int64_t maskedY = cellY & mask;
    int64_t maskedZ = cellZ & mask;

    // 各軸を21ビットずつ詰め、表現範囲内で軸同士が重ならないセルIDを作る。
    return (maskedZ << 42) | (maskedY << 21) | maskedX;
}



// 既存の静的グリッドを捨て、衝突対象の静的物体だけを現在の位置で登録します。
void CollisionManager::BuildStaticGrid() {

    staticGrid_.clear();

    for (engine::Object3D* obj : objects_) {
        // 静的であり、形状・属性・マスクが有効なオブジェクトだけを静的グリッドに登録する。
        if (!obj->IsStatic() || !IsCollisionActive(obj)) {
            continue;
        }

        Aabb aabb = obj->GetAabb();
        Vector3i minCell = GetGridIndices(aabb.min);
        Vector3i maxCell = GetGridIndices(aabb.max);

        // 中心セルだけでなく、外接箱が占有する全セルに登録して境界の接触も拾う。
        for (std::int32_t z = minCell.z; z <= maxCell.z; ++z) {
            for (std::int32_t y = minCell.y; y <= maxCell.y; ++y) {
                for (std::int32_t x = minCell.x; x <= maxCell.x; ++x) {
                    int64_t cellId = GetGridIdFromIndices(x, y, z);
                    staticGrid_[cellId].push_back(obj);
                }
            }
        }
    }

    needsStaticGridRebuild_ = false;
}


// 静的グリッドは必要時に再構築し、動的な物体の接触を今回分として収集します。
void CollisionManager::Update() {

    currentContacts_.clear();

    // 追加・登録解除・セル幅変更などで静的グリッドが無効になったときだけ作り直す。
    if (needsStaticGridRebuild_) {
        BuildStaticGrid();
    }

    // 動的グリッドと判定済みの組は、そのフレームの移動と接触から作り直します。
    grid_.clear();
    checkedPairs_.clear();

    // 移動し得る物体を現在の外接箱が覆う全セルに登録します。
    for (engine::Object3D* objA : objects_) {
        // 静的な物体は静的側に任せ、衝突が無効な物体も動的グリッドへ入れない。
        if (objA->IsStatic() || !IsCollisionActive(objA)) {
            continue;
        }

        Aabb aabb = objA->GetAabb();
        Vector3i minCell = GetGridIndices(aabb.min);
        Vector3i maxCell = GetGridIndices(aabb.max);

        for (std::int32_t z = minCell.z; z <= maxCell.z; ++z) {
            for (std::int32_t y = minCell.y; y <= maxCell.y; ++y) {
                for (std::int32_t x = minCell.x; x <= maxCell.x; ++x) {
                    int64_t cellId = GetGridIdFromIndices(x, y, z);
                    grid_[cellId].push_back(objA);
                }
            }
        }
    }

    // 動的な物体を起点に、同じセルにいる動的・静的候補を調べます。
    for (engine::Object3D* objA : objects_) {
        // 動く物体を起点に判定する。静的な物体と衝突無効な物体は起点にしない。
        if (objA->IsStatic() || !IsCollisionActive(objA)) {
            continue;
        }

        Aabb aabb = objA->GetAabb();
        Vector3i minCell = GetGridIndices(aabb.min);
        Vector3i maxCell = GetGridIndices(aabb.max);

        for (std::int32_t z = minCell.z; z <= maxCell.z; ++z) {
            for (std::int32_t y = minCell.y; y <= maxCell.y; ++y) {
                for (std::int32_t x = minCell.x; x <= maxCell.x; ++x) {
                    int64_t cellId = GetGridIdFromIndices(x, y, z);

                    // 動的な相手とは、複数のセルにまたがっても組ごとに一度だけ詳しく判定します。
                    auto dynamicIt = grid_.find(cellId);

                    // 同じセルに動的な物体が登録されていれば、そのリストを調べる。
                    if (dynamicIt != grid_.end()) {
                        std::list<engine::Object3D*>& cellObjects = dynamicIt->second;

                        for (engine::Object3D* objB : cellObjects) {

                            // 自分自身と、判定時点で無効になった相手は組み合わせから外す。
                            if (objA == objB || !IsCollisionActive(objB))
                                continue;

                            const auto pair = MakeContactPair(objA, objB);
                            engine::Object3D* pairA = pair.first;
                            engine::Object3D* pairB = pair.second;

                            // 同じ組を別のセルや逆順で見つけても、動的同士の判定はフレーム中に一度だけ行う。
                            if (checkedPairs_.count({ pairA, pairB })) {
                                continue;
                            }

                            CheckCollisionPair(pairA, pairB);
                            checkedPairs_.insert({ pairA, pairB });
                        }
                    }

                    // 静的な相手とは、外接箱が重なる各セルから既存の地形押し戻しを評価します。
                    auto staticIt = staticGrid_.find(cellId);

                    // 同じセルに静的な物体が登録されていれば、動的な物体と組み合わせる。
                    if (staticIt != staticGrid_.end()) {
                        std::list<engine::Object3D*>& cellObjects = staticIt->second;

                        for (engine::Object3D* objB : cellObjects) {
                            // 静的グリッドに参照が残っていても、判定時点で形状やマスクが無効なら飛ばす。
                            if (!IsCollisionActive(objB))
                                continue;

                            // 既存の地形押し戻しは動的オブジェクトを先に通知し、
                            // 占有セルごとに再評価する順序を前提に調整されています。
                            CheckCollisionPair(objA, objB);
                        }
                    }
                }
            }
        }
    }

    // 全セルの判定を集めてからイベントを通知し、通知後に今回分を次回比較用に保存する。
    DispatchContactEvents();
    previousContacts_ = currentContacts_;
}


// 形状が重なった組に即時通知し、フレーム末尾の接触イベント用にも記録する。
void CollisionManager::CheckCollisionPair(engine::Object3D* objA, engine::Object3D* objB) {
    // 片方でも判定形状・衝突属性・マスクが無効なら、この組の判定を打ち切る。
    if (!IsCollisionActive(objA) || !IsCollisionActive(objB)) {
        return;
    }

    // Aの接触マスクがBの属性を許可し、Bの接触マスクもAの属性を許可する組だけを調べる。
    if (!((objA->GetContactCollisionMask() & objB->GetCollisionAttribute()) &&
        (objB->GetContactCollisionMask() & objA->GetCollisionAttribute()))) {
        return;
    }

    // 形状同士の詳しい重なり判定に使うコライダーを取得する。
    engine::Collider* colliderA = objA->GetCollider();
    engine::Collider* colliderB = objB->GetCollider();

    // 片方でもコライダーを取得できなければ、形状の重なりを判定できない。
    if (!colliderA || !colliderB) {
        return;
    }

    const CollisionInfo collisionInfo = colliderA->CheckCollision(colliderB);

    // 同じセルの候補でも、形状が重なっていなければ通知も履歴への登録も行わない。
    if (!collisionInfo.isColliding) {
        return;
    }

    // 判定時の順序で双方へ即時通知する。Enter・Stay・Exitは全候補の判定後にまとめて通知する。
    objA->OnCollision(objB);
    objB->OnCollision(objA);

    // AとBの検査順が変わっても同じ組として照合できるよう、ポインター順を揃える。
    const auto pair = MakeContactPair(objA, objB);
    ContactState state;
    state.firstInfo = collisionInfo;

    // ペアキーが判定時の順と逆になる場合も、法線をキーの先頭側へ向けて保存する。
    if (pair.first != objA) {
        state.firstInfo.normal = state.firstInfo.normal * -1.0f;
    }

    // どちらかがトリガー属性なら、後段で双方へトリガー用のイベントを送る。
    state.isTrigger = IsTriggerObject(objA) || IsTriggerObject(objB);
    currentContacts_[pair] = state;
}

void CollisionManager::DispatchContactEvents() {
    // 前フレームとの集合差で Enter・Stay・Exit を決め、両当事者へ通知する。
    const auto dispatch = [](engine::Object3D* self, engine::Object3D* other, CollisionInfo info,
        CollisionEventPhase phase, bool isTrigger) {
        // 自分と相手の両方が揃ったときだけ、接触イベントを組み立てる。
        if (!self || !other)
            return;
        CollisionEvent event;
        event.self = self;
        event.other = other;
        event.collision = info;
        event.phase = phase;
        event.isTrigger = isTrigger;

        // どちらかがトリガー属性ならトリガー向け、両方が通常の形状なら衝突向けの通知を呼ぶ。
        if (isTrigger) {
            // 今回初めて重なったトリガーは開始を通知する。
            if (phase == CollisionEventPhase::kEnter)
                self->OnTriggerEnter(event);
            // 前回から重なっているトリガーは継続、離れたトリガーは終了を通知する。
            else if (phase == CollisionEventPhase::kStay) self->OnTriggerStay(event);
            // 前回だけにあったトリガー接触には終了を通知する。
            else self->OnTriggerExit(event);
        // トリガー属性を持つ物体が組に含まれない場合は通常の接触として通知する。
        } else {
            // 今回初めて重なった通常接触は開始を通知する。
            if (phase == CollisionEventPhase::kEnter)
                self->OnCollisionEnter(event);
            // 前回から重なっている通常接触は継続、離れた通常接触は終了を通知する。
            else if (phase == CollisionEventPhase::kStay) self->OnCollisionStay(event);
            // 前回だけにあった通常接触には終了を通知する。
            else self->OnCollisionExit(event);
        }
    };

    // 今回も存在する組のうち、前回にもあれば継続、今回だけなら開始として扱う。
    for (const auto& [pair, state] : currentContacts_) {
        const CollisionEventPhase phase = previousContacts_.count(pair)
            ? CollisionEventPhase::kStay : CollisionEventPhase::kEnter;
        dispatch(pair.first, pair.second, state.firstInfo, phase, state.isTrigger);
        // 記録はペアの先頭側が基準なので、後ろ側へ渡す法線は逆向きにする。
        CollisionInfo secondInfo = state.firstInfo;
        secondInfo.normal = secondInfo.normal * -1.0f;
        dispatch(pair.second, pair.first, secondInfo, phase, state.isTrigger);
    }

    // 前回だけにある接触は終了。最後に得た接触情報を両者に向けて返す。
    for (const auto& [pair, state] : previousContacts_) {
        // 今回も重なっている組には上のループで継続を送ったため、終了を送らない。
        if (currentContacts_.count(pair))
            continue;
        dispatch(pair.first, pair.second, state.firstInfo, CollisionEventPhase::kExit, state.isTrigger);
        CollisionInfo secondInfo = state.firstInfo;
        secondInfo.normal = secondInfo.normal * -1.0f;
        dispatch(pair.second, pair.first, secondInfo, CollisionEventPhase::kExit, state.isTrigger);
    }
}


/// <summary>
/// ワールド軸に平行な箱とレイの交差を、各軸に入る区間の共通部分から求める。
/// </summary>
/// <returns>最初の接触のレイ係数。方向が単位ベクトルならワールド距離。未命中はFLT_MAX。</returns>
float IntersectRayAabb(const engine::Vector3& start, const engine::Vector3& direction, const Aabb& aabb) {

    // ほぼ平行な軸では成分の代わりに正の微小値を使い、方向の逆数を計算します。
    engine::Vector3 invDir = {
        1.0f / (std::abs(direction.x) < 1e-6f ? 1e-6f : direction.x),
        1.0f / (std::abs(direction.y) < 1e-6f ? 1e-6f : direction.y),
        1.0f / (std::abs(direction.z) < 1e-6f ? 1e-6f : direction.z)
    };

    // 各軸の境界に達するレイ係数を、方向成分の逆数を掛けて求める。
    engine::Vector3 tMin = {
        (aabb.min.x - start.x) * invDir.x,
        (aabb.min.y - start.y) * invDir.y,
        (aabb.min.z - start.z) * invDir.z
    };

    engine::Vector3 tMax = {
        (aabb.max.x - start.x) * invDir.x,
        (aabb.max.y - start.y) * invDir.y,
        (aabb.max.z - start.z) * invDir.z
    };

    // (direction がマイナスの場合、tMin と tMax が逆転するので入れ替える)
    engine::Vector3 tNear = {
        std::min(tMin.x, tMax.x),
        std::min(tMin.y, tMax.y),
        std::min(tMin.z, tMax.z)
    };

    engine::Vector3 tFar = {
        std::max(tMin.x, tMax.x),
        std::max(tMin.y, tMax.y),
        std::max(tMin.z, tMax.z)
    };

    // 3軸すべてで重なっている領域の最も近い点と遠い点を求める
    float tEnter = std::max({ tNear.x, tNear.y, tNear.z });
    float tExit = std::min({ tFar.x, tFar.y, tFar.z });

    // 全軸の区間が重ならない場合や、出口まで始点の後方にある場合は未命中とする。
    if (tEnter > tExit || tExit < 0.0f) {
        return std::numeric_limits<float>::max(); // 衝突しない
    }

    // 入口が始点の後方で出口が前方に残るときは、始点から箱に重なっているので0を返す。
    if (tEnter < 0.0f) {
        return 0.0f;
    }

    // 始点の前方で最初に箱に入る位置を返す。
    return tEnter;
}

/// <summary>
/// 回転箱を各ローカル軸で挟む二面とレイを交差させ、全軸で箱に入る区間を求める。
/// </summary>
float IntersectRayObb(const engine::Vector3& start, const engine::Vector3& direction, const Obb& obb) {
    engine::Math math;
    float tMin = -std::numeric_limits<float>::max();
    float tMax = std::numeric_limits<float>::max();

    // レイ始点から箱の中心へのベクトルを、各ローカル軸に投影するために求める。
    engine::Vector3 p = obb.center - start;

    // OBBの3つのローカル軸（X, Y, Z）について調べる
    for (std::int32_t i = 0; i < 3; ++i) {
        engine::Vector3 axis = obb.orientations[i];
        float e = math.Dot(axis, p);
        float f = math.Dot(direction, axis);

        // 各軸の半分のサイズを取得
        float size = (i == 0) ? obb.size.x : (i == 1) ? obb.size.y : obb.size.z;

        // 軸方向の成分が十分あれば、その軸に垂直な二面までの係数を割り算で求められる。
        if (std::abs(f) > 1e-6f) {
            float t1 = (e + size) / f;
            float t2 = (e - size) / f;

            // レイの向きによって二面を横切る順序が逆になる場合は、入口と出口を入れ替える。
            if (t1 > t2)
                std::swap(t1, t2);

            // この軸の入口が後なら、全軸で箱の内側に入る時点を後ろへずらす。
            if (t1 > tMin)
                tMin = t1;
            // この軸の出口が先なら、全軸で箱の内側にいられる区間を手前で切る。
            if (t2 < tMax)
                tMax = t2;

            // 入口が出口を追い越した時点で、三軸が同時に箱の内側になる位置はない。
            if (tMin > tMax)
                return std::numeric_limits<float>::max(); // 衝突しない
            // 出口が始点より後方なら、その区間はレイの前方に入らない。
            if (tMax < 0.0f)
                return std::numeric_limits<float>::max(); // レイの始点より後ろにある
        }
        // 軸方向成分がほぼ0なら、レイはこの軸の二面とほぼ平行になる。
        else {
            // 始点の投影位置が軸の両端の外にあれば、レイは箱に入れない。
            if (-e - size > 0 || -e + size < 0) {
                return std::numeric_limits<float>::max();
            }
        }
    }

    // 入口が始点より後方で出口が前方に残る場合は、開始時の重なりとして0を返す。
    if (tMin < 0.0f)
        return 0.0f;

    return tMin;
}

float IntersectRaySphere(const engine::Vector3& start, const engine::Vector3& direction, const engine::Vector3& center, float radius) {
    // 球面とレイの交差を二次方程式で求め、始点が球の中にある場合は0を返す。
    engine::Vector3 m = start - center;
    float a = direction.x * direction.x + direction.y * direction.y + direction.z * direction.z;

    // 方向の長さ二乗がしきい値以下なら、レイを進められないので未命中にする。
    if (a <= 1e-6f) {
        return std::numeric_limits<float>::max();
    }

    // レイ上の点と球中心の距離二乗が半径二乗になる条件を二次式に展開する。
    float b = 2.0f * (m.x * direction.x + m.y * direction.y + m.z * direction.z);
    float c = (m.x * m.x + m.y * m.y + m.z * m.z) - radius * radius;
    float discriminant = b * b - 4.0f * a * c;

    // 判別式が負なら球面に達する実数解がなく、当たらない。
    if (discriminant < 0.0f) {
        return std::numeric_limits<float>::max();
    }

    float sqrtDiscriminant = std::sqrt(discriminant);
    float invDenominator = 1.0f / (2.0f * a);
    float t0 = (-b - sqrtDiscriminant) * invDenominator;
    float t1 = (-b + sqrtDiscriminant) * invDenominator;

    // 手前側の交点が始点以降なら、球に入る位置としてその解を採る。
    if (t0 >= 0.0f) {
        return t0;
    }

    // 手前の交点が後方にあり、奥の交点が始点以降なら、球内からの開始なので0を返す。
    if (t1 >= 0.0f) {
        return 0.0f;
    }

    return std::numeric_limits<float>::max();
}

/// <summary>
/// 衝突属性のビットマスクを使い、プレイヤー階層を除外してレイに最も近い対象を探す。
/// </summary>
RaycastHit CollisionManager::Raycast(const engine::Vector3& start, const engine::Vector3& direction,
    float maxDistance, uint32_t mask) {
    RaycastHit closestHit;
    closestHit.isHit = false;
    closestHit.distance = maxDistance;

    // 互換用のマスク指定では登録済みの全候補を調べます。方向は呼び出し側の値をそのまま使います。
    for (engine::Object3D* object : objects_) {
        // 形状・属性・マスクが無効な物体は、レイと交差させる対象から外す。
        if (!IsCollisionActive(object)) {
            continue;
        }

        // 本体と子パーツを視線や地面を調べるレイから除外するため、親を順にたどる。
        bool isPlayerPart = false;
        engine::Object3D* current = object;

        while (current) {
            // 自分または祖先がPlayerクラスか、名前にPlayerを含む場合はプレイヤー階層と判定する。
            if (current->GetClassName() == "Player" ||
                current->GetName().find("Player") != std::string::npos) {
                isPlayerPart = true;
                break;
            }

            current = current->GetParent();
        }

        // プレイヤー階層に属する物体を候補から外す。
        if (isPlayerPart) {
            continue;
        }

        // レイの検索マスクと相手の衝突属性に共通するビットがある物だけを調べる。
        if (!((object->GetCollisionAttribute()) & mask)) {
            continue; // 対象外
        }

            // 古いマスク版レイキャストはAABB・OBB・球の三形状を扱う。
            ColliderType colType = object->GetColliderType();
            // 三形状のどれにも該当しなければ、このオーバーロードに交差計算がないので飛ばす。
            if (colType != ColliderType::kAabb &&
                colType != ColliderType::kObb &&
                colType != ColliderType::kSphere) {
                continue;
            }

            float distance = std::numeric_limits<float>::max();

            // ワールドの座標軸に平行な箱は、その最小・最大座標を使う。
            if (colType == ColliderType::kAabb) {
                Aabb aabb = object->GetAabb();
                distance = IntersectRayAabb(start, direction, aabb);
            }
            // 回転箱の場合は、箱のローカル軸を使った交差距離を求める。
            else if (colType == ColliderType::kObb) {
                Obb obb = object->GetObb();
                distance = IntersectRayObb(start, direction, obb);
            }
            // 球の場合は、物体のワールド位置を中心にして半径との交差を求める。
            else if (colType == ColliderType::kSphere) {
                distance = IntersectRaySphere(
                    start,
                    direction,
                    object->GetWorldPosition(),
                    object->GetCollisionRadius());
            }

            engine::Vector3 surfaceNormal{};
            const bool useModelVolume = object->GetCollider() && object->GetCollider()->HasModelVolume();
            // モデル体積を使う対象で現時点の最短距離までに交差しなければ、簡易形状での命中も採用しない。
            if (useModelVolume && !object->GetCollider()->RaycastModelVolume(
                    start, direction, closestHit.distance, distance, surfaceNormal)) continue;
            const bool useModelSurface = object->GetCollider() && object->GetCollider()->HasModelSurface() &&
                !useModelVolume &&
                direction.y < 0.0f && -direction.y > std::sqrt(direction.x * direction.x + direction.z * direction.z);
            // 体積形状がなく急な下向きレイでモデル上面を使う場合は、その上面に当たった候補だけ残す。
            if (useModelSurface && !object->GetCollider()->RaycastModelSurface(
                    start, direction, closestHit.distance, distance, surfaceNormal)) {
                continue;
            }

            // 現在の命中より厳密に手前にある場合だけ結果を更新できる。同距離なら先に見つけた物を残す。
            if (distance < closestHit.distance) {
                // 体積形状を使わず輪郭が3点以上ある場合は、接触点がその輪郭の外にないことも確かめる。
                if (!useModelVolume && object->GetCollider() && object->GetCollider()->GetConfig().footprint.size() >= 3 &&
                    !object->GetCollider()->ContainsFootprintPoint(start + direction * distance)) continue;
                closestHit.isHit = true;
                closestHit.distance = distance;
                closestHit.hitObject = object;
                closestHit.hitPoint = start + direction * distance;

                // モデル面またはモデル体積を使った場合は、その形状から得た法線を返す。
                if (useModelSurface || useModelVolume) {
                    closestHit.normal = surfaceNormal;
                // 球の場合は中心から接触点への方向を法線の候補にする。
                } else if (colType == ColliderType::kSphere) {
                    engine::Vector3 normal = closestHit.hitPoint - object->GetWorldPosition();
                    float length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);

                    // 法線の長さがしきい値を超えたときだけ正規化し、ゼロに近い長さでの除算を防ぐ。
                    if (length > 1e-6f) {
                        closestHit.normal = normal / length;
                    }
                }

            }
        }

        // ヒットが無かった場合は、初期化した最大距離と未命中状態を返します。
        return closestHit;
    }

RaycastHit CollisionManager::Raycast(const engine::Vector3& start, const engine::Vector3& direction,
    float maxDistance, const PhysicsQueryFilter& filter) {
    // 一般向けレイは自機も候補に含め、必要な除外だけフィルタで指定する。
    return RaycastInternal(start, direction, maxDistance, filter, false);
}

bool CollisionManager::PassesQueryFilter(engine::Object3D* object, const PhysicsQueryFilter& filter) const {
#ifdef USE_IMGUI
    // エディターの検索フィルターが有効で対象を拒否した場合は、物理設定の判定へ進まない。
    if (object && editorQueryFilter_ && !editorQueryFilter_(object)) return false;
#endif

    // 実体と形状が揃わない対象を先に除外し、後段で安全に形状を参照できるようにする。
    if (!object || !object->GetCollider() || object->GetColliderType() == ColliderType::kNone) {
        return false;
    }

    // 無効物の検索を指定していないとき、衝突属性が0の物体を除外する。
    if (!filter.includeDisabled && object->GetCollisionAttribute() == 0) {
        return false;
    }

    // 属性を持つ物体には検索マスクを適用し、共通ビットがなければ通さない。
    if (object->GetCollisionAttribute() != 0 &&
        (object->GetCollisionAttribute() & filter.mask) == 0) {
        return false;
    }

    // 通知用の形状を含める指定がなければ、トリガー属性を持つ物体を除外する。
    if (!filter.includeTriggers && IsTriggerObject(object)) {
        return false;
    }

    // 除外の基点が指定されたときだけ、参照の一致と親子関係を調べる。
    if (filter.ignoredObject) {
        // 検索の発行元など、基点として指定した物体自身への命中を除外する。
        if (object == filter.ignoredObject)
            return false;
        // 子孫も除外する指定がある場合は、親をたどって基点に届く物体も除外する。
        if (filter.ignoreDescendants && IsInIgnoredHierarchy(object, filter.ignoredObject))
            return false;
    }

    // 呼び出し元の追加フィルターがあれば、その最終判定まで通った候補だけを使う。
    return !filter.acceptsObject || filter.acceptsObject(object);
}

RaycastHit CollisionManager::RaycastInternal(
    const engine::Vector3& start, const engine::Vector3& direction, float maxDistance,
    const PhysicsQueryFilter& filter, bool excludePlayerHierarchy) const {
    // 有効な形状だけを調べ、最も手前に当たった物へ結果を更新する。
    RaycastHit closestHit;
    closestHit.distance = (std::max)(0.0f, maxDistance);
    // 方向を単位ベクトルに揃え、交差パラメーターをワールド距離として扱う。
    const engine::Vector3 rayDirection = SafeNormalize(direction);
    // 向きを作れないレイと負の検索距離は無効として、候補を調べずに未命中を返す。
    if (engine::Math::Length(rayDirection) <= 0.000001f || maxDistance < 0.0f)
        return closestHit;

    for (engine::Object3D* object : objects_) {
        // マスク・トリガー・除外対象などの共通フィルターを通った物体だけ交差を調べる。
        if (!PassesQueryFilter(object, filter))
            continue;

        // プレイヤー階層を除外する指定がある場合は、対象から親への系列を調べる。
        if (excludePlayerHierarchy) {
            bool isPlayerPart = false;

            for (engine::Object3D* current = object; current; current = current->GetParent()) {
                // 自分またはいずれかの親がPlayerクラスか、名前にPlayerを含めばプレイヤー階層と判定する。
                if (current->GetClassName() == "Player" ||
                    current->GetName().find("Player") != std::string::npos) {
                    isPlayerPart = true;
                    break;
                }
            }

            // プレイヤー階層と確認できた候補にはレイを当てない。
            if (isPlayerPart)
                continue;
        }

        const ColliderType colliderType = object->GetColliderType();
        float distance = std::numeric_limits<float>::max();

        // ワールド軸に平行な箱は、その境界座標から交差を求める。
        if (colliderType == ColliderType::kAabb) {
            distance = IntersectRayAabb(start, rayDirection, object->GetAabb());
        // 回転箱は箱のローカル軸に沿った二面を使って交差を求める。
        } else if (colliderType == ColliderType::kObb) {
            distance = IntersectRayObb(start, rayDirection, object->GetObb());
        // 球は外接箱の中点を中心にし、半径とレイの交差を調べる。
        } else if (colliderType == ColliderType::kSphere) {
            const Aabb sphereBounds = object->GetAabb();
            const engine::Vector3 sphereCenter = (sphereBounds.min + sphereBounds.max) * 0.5f;
            distance = IntersectRaySphere(
                start, rayDirection, sphereCenter, object->GetCollisionRadius());
        // 三形状以外のコライダーはこのレイ判定の対象外とする。
        } else {
            continue;
        }

        engine::Vector3 surfaceNormal{};
        // 簡易形状の距離は初期候補。モデル体積を持つ場合は凸包の交差で距離と法線を求め直す。
        const bool useModelVolume = object->GetCollider()->HasModelVolume();
        // 凸包を使う場合は、現在の最短距離までに凸包と交差しなければ候補から外す。
        if (useModelVolume && !object->GetCollider()->RaycastModelVolume(
                start, rayDirection, closestHit.distance, distance, surfaceNormal)) continue;
        // 上面専用のモデル形状は、下向き成分が水平成分より大きいレイにだけ使う。
        const bool useModelSurface = object->GetCollider()->HasModelSurface() &&
            !useModelVolume &&
            rayDirection.y < 0.0f && -rayDirection.y >
            std::sqrt(rayDirection.x * rayDirection.x + rayDirection.z * rayDirection.z);
        // 急な下向きレイでモデル上面を使う場合は、上面に交差しなければ候補から外す。
        if (useModelSurface && !object->GetCollider()->RaycastModelSurface(
                start, rayDirection, closestHit.distance, distance, surfaceNormal)) continue;
        // 現在の命中より奥にある候補を捨てる。同距離にある候補は結果を更新できる。
        if (distance > closestHit.distance)
            continue;
        // モデル体積がなく3点以上の輪郭を持つ場合は、その輪郭の外にある交点を捨てる。
        if (!useModelVolume && object->GetCollider() && object->GetCollider()->GetConfig().footprint.size() >= 3 &&
            !object->GetCollider()->ContainsFootprintPoint(start + rayDirection * distance)) continue;

        closestHit.isHit = true;
        closestHit.distance = distance;
        closestHit.hitObject = object;
        closestHit.hitPoint = start + rayDirection * distance;

        // モデル形状で当たったときは交差計算が返した法線を採用する。
        if (useModelSurface || useModelVolume) {
            closestHit.normal = surfaceNormal;
        // 球では中心から接触点へ向かう方向を単位法線にする。
        } else if (colliderType == ColliderType::kSphere) {
            const Aabb sphereBounds = object->GetAabb();
            closestHit.normal = SafeNormalize(
                closestHit.hitPoint - (sphereBounds.min + sphereBounds.max) * 0.5f);
        // ワールド軸に平行な箱では、接触点と六面の距離を比べて一番近い面を選ぶ。
        } else if (colliderType == ColliderType::kAabb) {
            const Aabb bounds = object->GetAabb();
            const float faceDistances[6] = {
                std::abs(closestHit.hitPoint.x - bounds.min.x),
                std::abs(closestHit.hitPoint.x - bounds.max.x),
                std::abs(closestHit.hitPoint.y - bounds.min.y),
                std::abs(closestHit.hitPoint.y - bounds.max.y),
                std::abs(closestHit.hitPoint.z - bounds.min.z),
                std::abs(closestHit.hitPoint.z - bounds.max.z),
            };

            const engine::Vector3 normals[6] = {
                { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 },
                { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 },
            };

            std::int32_t bestFace = 0;

            for (std::int32_t face = 1; face < 6; ++face) {
                // 調べた面が現在の候補より接触点に近ければ、その面の法線を使う。
                if (faceDistances[face] < faceDistances[bestFace])
                    bestFace = face;
            }

            closestHit.normal = normals[bestFace];
        // 残る回転箱は各ローカル軸への投影で最近面を求め、接触点側へ向く法線を使う。
        } else {
            const Obb bounds = object->GetObb();
            const engine::Vector3 local = closestHit.hitPoint - bounds.center;
            float bestDistance = std::numeric_limits<float>::max();

            for (std::int32_t axisIndex = 0; axisIndex < 3; ++axisIndex) {
                const float halfSize = axisIndex == 0 ? bounds.size.x : axisIndex == 1 ? bounds.size.y : bounds.size.z;
                const float projection = engine::Math::Dot(local, bounds.orientations[axisIndex]);
                const float surfaceDistance = std::abs(halfSize - std::abs(projection));

                // 接触点により近い面を見つけたときだけ、法線の軸と向きを更新する。
                if (surfaceDistance < bestDistance) {
                    bestDistance = surfaceDistance;
                    closestHit.normal = bounds.orientations[axisIndex] * (projection >= 0.0f ? 1.0f : -1.0f);
                }
            }
        }
    }

    return closestHit;
}

// 指定のワールド座標と半径で、登録済みの全オブジェクトから重なりを探します。
std::vector<PhysicsOverlapHit> CollisionManager::OverlapSphere(
    const engine::Vector3& center, float radius, const PhysicsQueryFilter& filter) const {
    return OverlapSphereCandidates(center, radius, filter, nullptr);
}

std::vector<PhysicsOverlapHit> CollisionManager::OverlapSphereCandidates(
    const engine::Vector3& center, float radius, const PhysicsQueryFilter& filter,
    const std::vector<engine::Object3D*>* candidates) const {
    // 一時的な球コライダーで既存の形状判定を再利用し、重なった物を列挙する。
    std::vector<PhysicsOverlapHit> hits;
    // 有効な半径を持たない球は形状を作らず、空の結果を返す。
    if (radius <= 0.0f)
        return hits;

    Transform queryTransform;
    queryTransform.translate = center;
    queryTransform.UpdateMatrix();
    engine::Collider queryCollider(&queryTransform);
    engine::ColliderConfig queryConfig;
    queryConfig.type = ColliderType::kSphere;
    queryConfig.size = { radius, radius, radius };
    queryCollider.SetConfig(queryConfig);

    const auto checkObject = [&](engine::Object3D* object) {
        // 形状、マスク、無効物や除外対象の指定を満たさない物体には、球との交差計算を行わない。
        if (!PassesQueryFilter(object, filter))
            return;
        CollisionInfo collision = queryCollider.CheckCollision(object->GetCollider());

        // 球が重なり、接触内容を確認する追加フィルターも通る場合だけ結果に入れる。
        if (collision.isColliding &&
            (!filter.acceptsSphereContact || filter.acceptsSphereContact(object, collision)))
            hits.push_back({ object, collision });
    };

    // 候補の順序はobjects_の登録順を保ち、同じ深さの接触の選ばれ方も維持します。
    // SphereCastから候補リストを渡されたときは、そのリストだけを検索する。
    if (candidates) {
        for (engine::Object3D* object : *candidates) checkObject(object);
    // 候補指定のない通常の球検索では、登録中の全オブジェクトを候補にする。
    } else {
        for (engine::Object3D* object : objects_) checkObject(object);
    }
    return hits;
}

std::vector<PhysicsOverlapHit> CollisionManager::OverlapAabb(
    const Aabb& bounds, const PhysicsQueryFilter& filter) const {
    // ワールド軸に平行な箱の中心と半サイズを一時コライダーに変換します。
    std::vector<PhysicsOverlapHit> hits;
    const engine::Vector3 halfSize = (bounds.max - bounds.min) * 0.5f;
    // いずれかの軸で最大値が最小値を下回る箱は作らず、空の結果を返す。
    if (halfSize.x < 0.0f || halfSize.y < 0.0f || halfSize.z < 0.0f)
        return hits;

    Transform queryTransform;
    queryTransform.translate = (bounds.min + bounds.max) * 0.5f;
    queryTransform.UpdateMatrix();
    engine::Collider queryCollider(&queryTransform);
    engine::ColliderConfig queryConfig;
    queryConfig.type = ColliderType::kAabb;
    queryConfig.size = halfSize;
    queryCollider.SetConfig(queryConfig);

    for (engine::Object3D* object : objects_) {
        // 共通フィルターで対象外になった物体には、箱との交差計算を行わない。
        if (!PassesQueryFilter(object, filter))
            continue;
        CollisionInfo collision = queryCollider.CheckCollision(object->GetCollider());

        // 検索用の箱と実際に重なる物体だけ、接触情報を結果に入れる。
        if (collision.isColliding)
            hits.push_back({ object, collision });
    }

    return hits;
}

std::vector<PhysicsOverlapHit> CollisionManager::OverlapCapsule(
    const engine::Vector3& pointA, const engine::Vector3& pointB, float radius,
    const PhysicsQueryFilter& filter) const {
    // 線分上の球を重ねて近似し、同じ物への複数命中は最大侵入量にまとめる。
    std::unordered_map<engine::Object3D*, CollisionInfo> uniqueHits;

    // 半径が0以下なら軸に沿った検索球を作らず、空の結果を返す。
    if (radius <= 0.0f)
        return {};

    const engine::Vector3 segment = pointB - pointA;
    const float length = engine::Math::Length(segment);
    // 端から端までをおおむね半径単位で分割する。半径が小さいときは刻みを最低0.05ワールド単位にする。
    const std::int32_t sampleCount = (std::max)(1, static_cast<std::int32_t>(std::ceil(length / (std::max)(radius, 0.05f))));

    for (std::int32_t sampleIndex = 0; sampleIndex <= sampleCount; ++sampleIndex) {
        const float t = static_cast<float>(sampleIndex) / static_cast<float>(sampleCount);

        for (const PhysicsOverlapHit& hit : OverlapSphere(pointA + segment * t, radius, filter)) {
            auto found = uniqueHits.find(hit.object);

            // 初めて当たった物体か、同じ物体に対する前の球より深く重なった場合だけ接触情報を記録する。
            if (found == uniqueHits.end() || hit.collision.penetration > found->second.penetration) {
                uniqueHits[hit.object] = hit.collision;
            }
        }
    }

    std::vector<PhysicsOverlapHit> hits;
    hits.reserve(uniqueHits.size());

    for (const auto& [object, collision] : uniqueHits)
        hits.push_back({ object, collision });
    return hits;
}

RaycastHit CollisionManager::SphereCast(
    const engine::Vector3& start, float radius, const engine::Vector3& direction, float maxDistance,
    const PhysicsQueryFilter& filter) const {
    // 球を経路に沿って進め、最初に重なる検査位置から接触を求める。距離はワールド単位。
    RaycastHit hit;
    hit.distance = (std::max)(0.0f, maxDistance);
    const engine::Vector3 castDirection = SafeNormalize(direction);
    // 半径が正でない、方向を取れない、または最大距離が負なら未命中を返す。
    if (radius <= 0.0f || engine::Math::Length(castDirection) <= 0.000001f || maxDistance < 0.0f)
        return hit;

    // 前フレームの空間グリッドは使わず、移動直後の形状から経路全体の候補を集めます。
    const engine::Vector3 end = start + castDirection * maxDistance;
    const engine::Vector3 padding = {radius + 0.001f, radius + 0.001f, radius + 0.001f};
    const Aabb sweptBounds = {
        {(std::min)(start.x, end.x) - padding.x, (std::min)(start.y, end.y) - padding.y, (std::min)(start.z, end.z) - padding.z},
        {(std::max)(start.x, end.x) + padding.x, (std::max)(start.y, end.y) + padding.y, (std::max)(start.z, end.z) + padding.z}};
    std::vector<engine::Object3D*> candidates;
    for (engine::Object3D* object : objects_) {
        // 実体や判定形状がなければ、どの位置でも重なりを調べられない。
        if (!object || !object->GetCollider()) continue;
        const uint32_t attribute = object->GetCollisionAttribute();
        // 属性があり、検索マスクとの共通ビットがない物は候補から外す。属性0は後段のフィルターに委ねる。
        if (attribute != 0 && (attribute & filter.mask) == 0) continue;

        const engine::Collider* collider = object->GetCollider();
        const ColliderType type = collider->GetType();
        // モデル面やモデル体積を持たないAABB・OBBだけ、箱の外接範囲で候補を絞る。
        if ((type == ColliderType::kAabb || type == ColliderType::kObb) &&
            !collider->HasModelSurface() && !collider->HasModelVolume()) {
            const Obb box = collider->GetObb();
            // 軸同士の内積が許容値を超える箱は、外接範囲で除外せず既存の重なり判定へ渡す。
            if (std::abs(engine::Math::Dot(box.orientations[0], box.orientations[1])) > 0.0001f ||
                std::abs(engine::Math::Dot(box.orientations[0], box.orientations[2])) > 0.0001f ||
                std::abs(engine::Math::Dot(box.orientations[1], box.orientations[2])) > 0.0001f) {
                candidates.push_back(object);
                continue;
            }
            const Aabb bounds = collider->GetAabb();
            // X・Y・Zのどれか一軸でも経路の外接範囲と離れていれば、候補から外す。
            if (bounds.max.x < sweptBounds.min.x || bounds.min.x > sweptBounds.max.x ||
                bounds.max.y < sweptBounds.min.y || bounds.min.y > sweptBounds.max.y ||
                bounds.max.z < sweptBounds.min.z || bounds.min.z > sweptBounds.max.z) continue;
        }
        candidates.push_back(object);
    }

    // 半径の半分、最低0.05ワールド単位の間隔で調べます。開始地点を含め、最後は指定距離に揃えます。
    const float stepLength = (std::max)(0.05f, radius * 0.5f);
    const std::int32_t stepCount = (std::max)(1, static_cast<std::int32_t>(std::ceil(maxDistance / stepLength)));

    for (std::int32_t stepIndex = 0; stepIndex <= stepCount; ++stepIndex) {
        const float distance = (std::min)(maxDistance, stepLength * static_cast<float>(stepIndex));
        const engine::Vector3 center = start + castDirection * distance;
        const auto overlaps = OverlapSphereCandidates(center, radius, filter, &candidates);
        // この位置でフィルターを通る重なりがなければ、次の位置へ進む。
        if (overlaps.empty())
            continue;

        const PhysicsOverlapHit* deepest = &overlaps.front();

        for (const PhysicsOverlapHit& overlap : overlaps) {
            // 同じ位置で複数に触れたら、食い込みがより深い相手へ更新する。同じ深さなら先に見つかった相手を使う。
            if (overlap.collision.penetration > deepest->collision.penetration)
                deepest = &overlap;
        }

        PhysicsOverlapHit contact = *deepest;
        float hitDistance = distance;
        // 直前の検査位置があり、絞り込み指定またはモデル形状がある場合だけ接触距離を再計算する。
        if (stepIndex > 0 && contact.object && contact.object->GetCollider() &&
            (filter.refineSphereCastHit || contact.object->GetCollider()->HasModelSurface() ||
             contact.object->GetCollider()->HasModelVolume())) {
            // 粗い計算で命中した位置と、その一つ手前を接触位置の探索区間にします。
            float lower = stepLength * static_cast<float>(stepIndex - 1);
            float upper = distance;

            // 非接触と接触の間を二分し、同じ物体と初めて重なる距離に近づける。指定時は反復を増やす。
            const std::int32_t refinementCount = filter.refineSphereCastHit ? 8 : 5;
            for (std::int32_t iteration = 0; iteration < refinementCount; ++iteration) {
                const float middle = (lower + upper) * 0.5f;
                const auto refined = OverlapSphereCandidates(start + castDirection * middle, radius, filter, &candidates);
                const auto sameObject = std::find_if(refined.begin(), refined.end(),
                    [&contact](const PhysicsOverlapHit& candidate) { return candidate.object == contact.object; });
                // 同じ物体に届かなければ手前の境界を、届けば奥の境界と接触情報を更新する。
                if (sameObject == refined.end()) {
                    lower = middle;
                // この中間点で同じ物体に接触した場合は、接触側の境界を狭めて法線なども更新する。
                } else {
                    upper = middle;
                    contact = *sameObject;
                }
            }

            hitDistance = upper;
        }

        hit.isHit = true;
        hit.hitObject = contact.object;
        hit.distance = hitDistance;
        hit.normal = contact.collision.normal;
        // 命中距離で得られるのは球の中心。接触法線と逆に半径分戻した点を接触点として返す。
        hit.hitPoint = start + castDirection * hitDistance - hit.normal * radius;
        return hit;
    }

    // 終点まで接触がなければ、初期値の未命中と最大距離を返す。
    return hit;
}

RaycastHit CollisionManager::CapsuleCast(
    const engine::Vector3& pointA, const engine::Vector3& pointB, float radius,
    const engine::Vector3& direction, float maxDistance,
    const PhysicsQueryFilter& filter) const {
    // ワールド座標で受け取った軸の両端を同じ距離だけ進め、最初に重なる検査位置を探す。
    RaycastHit hit;
    hit.distance = (std::max)(0.0f, maxDistance);
    const engine::Vector3 castDirection = SafeNormalize(direction);
    // 半径が正でない、方向を取れない、または最大距離が負なら未命中を返す。
    if (radius <= 0.0f || engine::Math::Length(castDirection) <= 0.000001f || maxDistance < 0.0f)
        return hit;

    // 半径の半分を目安に、最低0.05ワールド単位ずつ進める。開始地点も調べ、最後は指定距離に合わせる。
    const float stepLength = (std::max)(0.05f, radius * 0.5f);
    const std::int32_t stepCount = (std::max)(1, static_cast<std::int32_t>(std::ceil(maxDistance / stepLength)));

    for (std::int32_t stepIndex = 0; stepIndex <= stepCount; ++stepIndex) {
        const float distance = (std::min)(maxDistance, stepLength * static_cast<float>(stepIndex));
        const engine::Vector3 offset = castDirection * distance;
        const auto overlaps = OverlapCapsule(pointA + offset, pointB + offset, radius, filter);
        // 軸に沿って並べた球がどれも対象と重ならなければ、次の位置へ進む。
        if (overlaps.empty())
            continue;

        const PhysicsOverlapHit* deepest = &overlaps.front();

        for (const PhysicsOverlapHit& overlap : overlaps) {
            // この位置で食い込みがより深い相手へ更新する。同じ深さなら先に列挙された相手を使う。
            if (overlap.collision.penetration > deepest->collision.penetration)
                deepest = &overlap;
        }

        // 最初に重なった検査位置の移動距離を、接触距離として返す。
        hit.isHit = true;
        hit.hitObject = deepest->object;
        hit.distance = distance;
        hit.normal = deepest->collision.normal;
        // カプセル軸の中点を半径分ずらした代表点を返す。重なりを検出した球の中心は保持しない。
        hit.hitPoint = (pointA + pointB) * 0.5f + offset - hit.normal * radius;
        return hit;
    }

    // 終点まで接触がなければ、初期値の未命中と最大距離を返す。
    return hit;
}

} // namespace engine
