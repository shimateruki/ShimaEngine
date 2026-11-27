#pragma once

// GamePlaySceneの演出で使う補間・角度計算。演出の開始条件とタイマーはシーン側が保持する。
#include "engine/utility/math/Math.h"
#include <cmath>

// ワールド座標の水平成分とカメラの向きの計算を演出間で共有する。
namespace gamePlayPresentationMath {
inline engine::Vector3 NormalizePlanarDirection(const engine::Vector3& direction, const engine::Vector3& fallback) {
    engine::Vector3 result = {direction.x, 0.0f, direction.z};
    const float length = std::sqrt(result.x * result.x + result.z * result.z);
    // 水平成分がほぼ0で方向を単位化できない場合は、呼び出し側の代替方向を使う。
    if (length <= 0.0001f) {
        return fallback;
    }
    result.x /= length;
    result.z /= length;
    return result;
}

inline engine::Vector3 MakeLookAtEuler(const engine::Vector3& eye, const engine::Vector3& target, float roll = 0.0f) {
    // 視点から注視点までの距離からピッチとヨーをラジアンで求める。ロールは指定値を保持する。
    const float dx = target.x - eye.x;
    const float dy = target.y - eye.y;
    const float dz = target.z - eye.z;
    const float horizontal = std::sqrt(dx * dx + dz * dz);
    return {std::atan2(-dy, horizontal), std::atan2(dx, dz), roll};
}

} // namespace gamePlayPresentationMath
