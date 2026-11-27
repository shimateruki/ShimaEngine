#define NOMINMAX
#include "CharacterMotor.h"
#include <numbers>

#include "CollisionConfig.h"
#include "CollisionManager.h"
#include "Object3D.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kMinimumDistance = 0.0001f;
constexpr float kDegreesToRadians = std::numbers::pi_v<float> / 180.0f;

float Dot(const engine::Vector3& lhs, const engine::Vector3& rhs) {
    // 衝突法線に対する速度成分を得るための局所的な内積。
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

float Length(const engine::Vector3& value) {
    // Sweep の移動距離をワールド空間で測る。
    return std::sqrt(Dot(value, value));
}

engine::Vector3 NormalizeOrZero(const engine::Vector3& value) {
    // ほぼゼロの変位は方向を作らず、SphereCast に不正な値を渡さない。
    const float length = Length(value);
    return length > kMinimumDistance ? value * (1.0f / length) : engine::Vector3{};
}

// 与えられた位置にColliderの設定上の中心オフセットを足して、掃引の始点を返します。
engine::Vector3 ResolveQueryCenter(const engine::Object3D& owner, const engine::Vector3& position) {
    const engine::Object3D::ColliderConfig& collider = owner.GetColliderConfig();
    return position + collider.center;
}

float ResolveQueryRadius(const engine::Object3D& owner, float skinWidth) {
    // 接触面にわずかな隙間を残して次フレームの押し戻しを安定させる。
    const engine::Collider* collider = owner.GetCollider();
    const float radius = collider ? collider->GetRadius() : 0.5f;
    return (std::max)(0.05f, radius - skinWidth);
}
} // namespace

void CharacterMotor::SetSettings(const CharacterMotorSettings& settings) {
    // 傾斜角と段差などを物理計算が扱える範囲に丸める。
    settings_ = settings;
    settings_.maxSlopeDegrees = std::clamp(settings_.maxSlopeDegrees, 0.0f, 89.9f);
    settings_.stepHeight = (std::max)(0.0f, settings_.stepHeight);
    settings_.groundProbeDistance = (std::max)(0.0f, settings_.groundProbeDistance);
    settings_.skinWidth = std::clamp(settings_.skinWidth, 0.0f, 0.25f);
}

void CharacterMotor::BeginFrame(bool& grounded) {
    // 接地結果は毎フレーム再判定し、前フレームの床を持ち越さない。
    result_.grounded = false;
    result_.blocked = false;
    result_.steppedUp = false;
    result_.groundNormal = {0.0f, 1.0f, 0.0f};
    result_.groundObject = nullptr;
    result_.horizontalHitObject = nullptr;
    result_.horizontalHitNormal = {};
    grounded = false;
}

void CharacterMotor::ApplyGravity(engine::Vector3& velocity, float gravity, float maxFallSpeed, float deltaTime) const {
    // 落下速度の上限で長い落下後の Sweep 距離を抑える。
    // 重力は下向き加速度の大きさ、deltaTimeは秒。掛け合わせた速度変化を鉛直速度から引きます。
    velocity.y -= gravity * deltaTime;

    // 重力を反映した鉛直速度が落下上限を下回れば、下向きの符号を保って上限に丸める。
    if (velocity.y < -maxFallSpeed) {
        velocity.y = -maxFallSpeed;
    }
}

void CharacterMotor::Move(engine::Object3D& owner, engine::Vector3& position, engine::Vector3& velocity, bool& grounded, float deltaTime) {
    // 時刻が進まないフレームでは位置・速度・接地のいずれも変更しません。
    if (deltaTime <= 0.0f) {
        return;
    }

    const engine::Vector3 displacement = velocity * deltaTime;

    // 連続衝突を無効にした設定では、掃引を行わず変位を一度だけ位置に足す。
    if (!settings_.continuousCollision) {
        position += displacement;
        return;
    }

    MoveSwept(owner, position, velocity, grounded, displacement);
}

void CharacterMotor::ResolveCollision(engine::Vector3& position, engine::Vector3& velocity, bool& grounded, const engine::CollisionInfo& info,
                                      std::uint32_t attribute) {
    // 地面以外の接触では押し戻しや接地状態を変更しない。
    if (!(attribute & engine::collisionCategories::Get("ground"))) {
        return;
    }

    // 食い込んだ分だけ法線方向へ戻し、面に向かう速度成分だけを打ち消して接線方向の速度を残します。
    position += info.normal * info.penetration;

    const float velocityIntoSurface = Dot(velocity, info.normal);

    // 内積が負なら速度に面へ入る成分があるので、その成分だけ打ち消す。
    if (velocityIntoSurface < 0.0f) {
        velocity = velocity - info.normal * velocityIntoSurface;
    }

    result_.blocked = true;

    // 接触の法線が歩行可能な傾きの場合だけ、この接触を床として記録する。
    if (IsWalkable(info.normal)) {
        grounded = true;
        result_.grounded = true;
        result_.groundNormal = info.normal;

        // 床に触れた後も下向きの速度が残っていれば、鉛直速度を0にする。
        if (velocity.y < 0.0f) {
            velocity.y = 0.0f;
        }
    }
}

bool CharacterMotor::IsWalkable(const engine::Vector3& normal) const {
    // 法線の Y 成分を傾斜角のコサインと比較して歩ける面を決める。
    const float minimumNormalY = std::cos(settings_.maxSlopeDegrees * kDegreesToRadians);
    return normal.y > minimumNormalY;
}

bool CharacterMotor::TryStep(engine::Object3D& owner, engine::Vector3& position, engine::Vector3& velocity, bool& grounded,
                             const engine::Vector3& planarDisplacement, float queryRadius) {
    // 段差越えの高さが0以下か、水平変位がしきい値以下なら、持ち上げの判定を始めない。
    if (settings_.stepHeight <= 0.0f || Length(planarDisplacement) <= kMinimumDistance) {
        return false;
    }

    engine::PhysicsQueryFilter filter;
    filter.mask = settings_.collisionMask;
    filter.ignoredObject = &owner;
    filter.includeTriggers = false;

    // 段差の上に相当する高さに上げ、同じ水平移動を遮る物がないか確認します。
    const float planarDistance = Length(planarDisplacement);
    const engine::Vector3 direction = NormalizeOrZero(planarDisplacement);
    const engine::Vector3 raisedPosition = position + engine::Vector3{0.0f, settings_.stepHeight, 0.0f};
    const engine::Vector3 raisedCenter = ResolveQueryCenter(owner, raisedPosition);
    const engine::RaycastHit forwardHit = engine::CollisionManager::GetInstance()->SphereCast(
        raisedCenter, queryRadius, direction, planarDistance + settings_.skinWidth, filter);
    // 持ち上げた高さでも水平経路が遮られていれば、位置を動かす前に段差越えを中止する。
    if (forwardHit.isHit) {
        return false;
    }

    // 持ち上げた位置から候補地点へ水平移動し、その地点の下に床を探す。
    position = raisedPosition + planarDisplacement;
    SnapToGround(owner, position, velocity, grounded, queryRadius,
                 settings_.stepHeight + settings_.groundProbeDistance);
    // 接地補正後もgroundedがfalseなら、追加した高さだけを戻す。水平移動分は位置に残る。
    if (!grounded) {
        position = position - engine::Vector3{0.0f, settings_.stepHeight, 0.0f};
        return false;
    }

    // 接地状態が得られた経路を段差越えとして呼び出し元へ報告する。
    result_.steppedUp = true;
    return true;
}

void CharacterMotor::MoveSwept(engine::Object3D& owner, engine::Vector3& position, engine::Vector3& velocity, bool& grounded,
                               const engine::Vector3& displacement) {
    // 自分自身とトリガーを除外し、固体だけを連続衝突の対象にします。
    engine::PhysicsQueryFilter filter;
    filter.mask = settings_.collisionMask;
    filter.ignoredObject = &owner;
    filter.includeTriggers = false;

    const float queryRadius = ResolveQueryRadius(owner, settings_.skinWidth);
    const engine::Vector3 planarDisplacement = {displacement.x, 0.0f, displacement.z};
    const float planarDistance = Length(planarDisplacement);

    // 水平の移動距離がしきい値を超えるときだけ、横方向の掃引を行う。
    if (planarDistance > kMinimumDistance) {
        // 横移動を先に Sweep し、遮蔽物があれば段差越えを試してから表面方向へ逃がす。
        const engine::Vector3 planarDirection = planarDisplacement * (1.0f / planarDistance);
        const engine::RaycastHit hit =
            engine::CollisionManager::GetInstance()->SphereCast(ResolveQueryCenter(owner, position), queryRadius,
                                                        planarDirection, planarDistance + settings_.skinWidth, filter);
        // 横方向に当たる物が見つからなければ、予定した水平変位をそのまま適用する。
        if (!hit.isHit) {
            position += planarDisplacement;
        // 段差を越えられなければ接触直前まで進み、残りの変位と速度から面へ向かう成分を除きます。
        } else if (!TryStep(owner, position, velocity, grounded, planarDisplacement, queryRadius)) {
            result_.horizontalHitObject = hit.hitObject;
            result_.horizontalHitNormal = hit.normal;
            const float safeDistance = (std::max)(0.0f, hit.distance - settings_.skinWidth);
            const float travelledDistance = (std::min)(safeDistance, planarDistance);
            position += planarDirection * travelledDistance;

            engine::Vector3 remaining = planarDisplacement - planarDirection * travelledDistance;
            const float intoSurface = Dot(remaining, hit.normal);

            // 残りの変位が壁へ向かう場合は、法線方向の成分を引いて壁に沿う移動を残す。
            if (intoSurface < 0.0f) {
                remaining = remaining - hit.normal * intoSurface;
            }

            position += remaining;

            // 残り変位の補正を終えたら、次の移動に引き継ぐ速度も壁の法線と比較する。
            const float velocityIntoSurface = Dot(velocity, hit.normal);

            // 次回に持ち越す速度がまだ壁へ向いていれば、壁向きの成分だけを取り除く。
            if (velocityIntoSurface < 0.0f) {
                velocity = velocity - hit.normal * velocityIntoSurface;
            }

            result_.blocked = true;
        }
    }

    // 横方向を解決してから、その地点で上方向・下方向の移動を検査します。
    const float verticalDistance = std::abs(displacement.y);

    // 鉛直変位がしきい値を超える場合に限り、予定した上下方向へ掃引する。
    if (verticalDistance > kMinimumDistance) {
        const engine::Vector3 verticalDirection = {0.0f, displacement.y > 0.0f ? 1.0f : -1.0f, 0.0f};
        const engine::RaycastHit hit = engine::CollisionManager::GetInstance()->SphereCast(
            ResolveQueryCenter(owner, position), queryRadius, verticalDirection, verticalDistance + settings_.skinWidth,
            filter);
        // 上下の経路に衝突がなければ、予定していた高さまでそのまま移動する。
        if (!hit.isHit) {
            position.y += displacement.y;
        // 上下の経路に当たる物があれば、接触面と隙間を保てる範囲で高さを変える。
        } else {
            const float safeDistance = (std::max)(0.0f, hit.distance - settings_.skinWidth);
            position.y += verticalDirection.y * (std::min)(safeDistance, verticalDistance);

            // 下向きに移動して当たった時だけ床とみなし、天井や急斜面では接地判定を立てません。
            if (verticalDirection.y < 0.0f && IsWalkable(hit.normal)) {
                grounded = true;
                result_.grounded = true;
                result_.groundNormal = hit.normal;
                result_.groundObject = hit.hitObject;
            }

            // 上下方向で接触した場合は、床として使える面かにかかわらず鉛直速度を止める。
            velocity.y = 0.0f;
            result_.blocked = true;
        }
    }

    // 床への吸着が有効で速度が水平または下降中なら、下向きの接地補正を行う。
    if (settings_.snapToGround && velocity.y <= 0.0f) {
        SnapToGround(owner, position, velocity, grounded, queryRadius, settings_.groundProbeDistance);
    }
}

void CharacterMotor::SnapToGround(engine::Object3D& owner, engine::Vector3& position, engine::Vector3& velocity, bool& grounded,
                                  float queryRadius, float maximumDistance) {
    // 吸着できる距離が0以下なら、下向きの検索や位置補正を行わない。
    if (maximumDistance <= 0.0f) {
        return;
    }

    engine::PhysicsQueryFilter filter;
    filter.mask = settings_.collisionMask;
    filter.ignoredObject = &owner;
    filter.includeTriggers = false;

    const engine::RaycastHit hit =
        engine::CollisionManager::GetInstance()->SphereCast(ResolveQueryCenter(owner, position), queryRadius,
                                                    {0.0f, -1.0f, 0.0f}, maximumDistance + settings_.skinWidth, filter);
    // 足元に何もないか、当たった面が歩行可能な傾きを超える場合は吸着しない。
    if (!hit.isHit || !IsWalkable(hit.normal)) {
        return;
    }

    // 面までの距離から接触に残す隙間を引いて、最大距離を超えない範囲で下げます。
    const float snapDistance = (std::max)(0.0f, hit.distance - settings_.skinWidth);
    position.y -= (std::min)(snapDistance, maximumDistance);
    grounded = true;
    result_.grounded = true;
    result_.groundNormal = hit.normal;
    result_.groundObject = hit.hitObject;

    // 吸着で床に乗った後は、残っている下向きの速度を打ち消す。
    if (velocity.y < 0.0f) {
        velocity.y = 0.0f;
    }
}
