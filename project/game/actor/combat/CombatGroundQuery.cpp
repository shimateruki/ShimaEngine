#include "game/actor/combat/CombatGroundQuery.h"
#include "CollisionConfig.h"
#include "CollisionManager.h"
#include <cmath>

engine::Vector3 combatGroundQuery::FindGroundPoint(const engine::Vector3& samplePosition, const engine::Object3D* owner, ProbeRange range,
                                           float fallbackHeight) {
    // 指定座標の上から下へ地面だけを探し、所有者自身は照会から外します。
    engine::Vector3 rayStart = samplePosition;
    rayStart.y += range.startHeight;
    engine::PhysicsQueryFilter filter;
    filter.mask = engine::collisionCategories::Get("ground");

    // Query APIは非constポインターを受け取りますが、除外対象を変更しません。
    filter.ignoredObject = const_cast<engine::Object3D*>(owner);
    const engine::RaycastHit hit =
        engine::CollisionManager::GetInstance()->Raycast(rayStart, {0.0f, -1.0f, 0.0f}, range.distance, filter);
    // 地面に当たった場合は、検索結果のワールド接触点をそのまま返す。
    if (hit.isHit) {
        return hit.hitPoint;
    }

    // 地面を拾えない場合も水平位置は維持し、高さだけ呼び出し側の既定値に置き換えます。
    engine::Vector3 fallback = samplePosition;
    fallback.y = fallbackHeight;
    return fallback;
}

bool combatGroundQuery::CanWalkTo(const engine::Vector3& currentPosition, const engine::Vector3& candidatePosition,
                                  const engine::Object3D* owner, ProbeRange range, float maxHeightDifference) {
    // 現在地と移動候補に同じ地面フィルターと下向きのレイを使います。
    engine::PhysicsQueryFilter filter;
    filter.mask = engine::collisionCategories::Get("ground");
    filter.ignoredObject = const_cast<engine::Object3D*>(owner);
    const auto probe = [&](const engine::Vector3& position) {
        engine::Vector3 start = position;
        start.y += range.startHeight;
        return engine::CollisionManager::GetInstance()->Raycast(start, {0.0f, -1.0f, 0.0f}, range.distance, filter);
    };

    const engine::RaycastHit current = probe(currentPosition);

    // 現在地の地面を取得できない場合は、候補との段差を比較できないため移動を許可する。
    if (!current.isHit) {
        return true;
    }

    // 移動先に地面があり、傾きと現在地からの段差がどちらも許容範囲なら通します。
    const engine::RaycastHit candidate = probe(candidatePosition);
    return candidate.isHit && candidate.normal.y >= 0.55f &&
           std::abs(candidate.hitPoint.y - current.hitPoint.y) <= maxHeightDifference;
}
