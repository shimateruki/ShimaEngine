#pragma once

// 動く床に乗っているかを、足元と床の上面から判定する。移動経路と移動量の適用は各ギミックが担当する。
#include "Character.h"
#include "GimmickMovingFloor.h"
#include <cmath>

// 原点ではなく当たり判定の足元で乗車を判定する。
// 前フレーム末尾の押し戻しは行列へ未反映の場合がある。接地中の生存キャラクターを対象に、判定前に双方の行列を更新する。
inline bool IsStandingOnMovingFloor(engine::Object3D& floor, Character& actor) {
    // 死亡中・未接地・上方向の速度が0.1を超える相手は、足元の位置が近くても乗員に含めない。
    if (actor.IsDead() || !actor.IsGrounded() || actor.GetVelocity().y > 0.1f)
        return false;
    floor.UpdateLocalMatrix();
    floor.UpdateWorldMatrix(false);
    actor.UpdateLocalMatrix();
    actor.UpdateWorldMatrix(false);
    const auto deck = floor.GetAabb();
    const auto body = actor.GetAabb();
    const float gap = body.min.y - deck.max.y;
    // 足元と天面の高低差が±0.2を外れるか、XZの重なりが端から0.02以内なら、その床に乗っていない。
    if (gap < -0.2f || gap > 0.2f || body.max.x <= deck.min.x + 0.02f ||
        body.min.x >= deck.max.x - 0.02f || body.max.z <= deck.min.z + 0.02f ||
        body.min.z >= deck.max.z - 0.02f) return false;
    const engine::Collider* floorCollider = floor.GetCollider();

    // 多角形フットプリントがある床はAABB通過後に実形状でも確認する。
    if (floorCollider && floorCollider->GetConfig().footprint.size() >= 3) {
        return floorCollider->OverlapsFootprint(actor.GetCollider());
    }
    // 多角形の追加判定がない床は、高さとXZの条件を満たした時点で乗員とする。
    return true;
}
