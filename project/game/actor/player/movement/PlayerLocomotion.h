#pragma once
#include "engine/utility/math/Math.h"
#include <algorithm>
#include <cmath>

// Playerの最高速度を基準に、水平加速・制動と空中の操作応答を決める。主な入口はApply()。
struct PlayerLocomotionSettings {
    // 停止状態から最高速度へ達するまでの基準秒数。
    float accelerationTime = 0.4f;
    // 最高速度から停止するまでの基準秒数。
    float brakingTime = 0.1f;
    // 空中の加速と制動へ掛ける倍率。1なら地上と同じ。
    float airControl = 0.65f;

    // 有限値を対応範囲へ収め、無限大やNaNは項目ごとの既定値へ戻す。
    void Normalize() {
        accelerationTime = std::isfinite(accelerationTime) ? std::clamp(accelerationTime, 0.0f, 3.0f) : 0.4f;
        brakingTime = std::isfinite(brakingTime) ? std::clamp(brakingTime, 0.0f, 3.0f) : 0.1f;
        airControl = std::isfinite(airControl) ? std::clamp(airControl, 0.1f, 1.0f) : 0.65f;
    }

    // 現在の速度を入力の目標へ変化させた結果を返す。deltaTimeは秒で、上下速度は保持する。
    engine::Vector3 Apply(const engine::Vector3& current, const engine::Vector3& desired,
                         float maximumSpeed, bool grounded, float deltaTime) const {
        // 水平速度だけを計算し、鉛直速度は重力とジャンプの処理へ残します。
        auto result = current;
        // 経過秒数が有限で正、かつ最高速度が有限である場合だけ、水平速度を更新する。
        if (!std::isfinite(deltaTime) || deltaTime <= 0.0f || !std::isfinite(maximumSpeed))
            return result;
        // 許容する最高速度が0以下なら、上下速度を保って水平速度だけ停止させる。
        if (maximumSpeed <= 0.0f) {
            result.x = result.z = 0.0f;
            return result;
        }
        // 設定の補正はローカルのコピーで行い、目標の水平速度も最高速度以下へ収めます。
        auto settings = *this;
        settings.Normalize();
        float targetX = desired.x, targetZ = desired.z;
        const float desiredSpeed = std::hypot(targetX, targetZ);
        // 目標の水平速度が最高速度を超えていれば、向きを保ったまま上限へ切り詰める。
        if (desiredSpeed > maximumSpeed) {
            const float limit = maximumSpeed / desiredSpeed;
            targetX *= limit; targetZ *= limit;
        }
        // 接地中は等倍の応答を使い、空中では設定した倍率を加速と制動に掛ける。
        const float response = grounded ? 1.0f : settings.airControl;
        float remaining = deltaTime;
        float speed = std::hypot(result.x, result.z);

        // 現在の速さが十分あり目標ベクトルとの内積が負なら、進行方向を反転する前に制動する。
        if (speed > 0.0001f && result.x * targetX + result.z * targetZ < 0.0f) {
            // 制動時間は最高速から止まる場合の秒数。現在の速さに比例させ、効きの弱い空中では長くなります。
            const float stopTime = settings.brakingTime * speed / (maximumSpeed * response);
            // 今のフレームだけでは停止しきれないなら、残った速度を返し、逆向きへの加速をまだ行わない。
            if (stopTime > remaining) {
                const float keep = 1.0f - remaining / stopTime;
                result.x *= keep; result.z *= keep;
                return result;
            }
            // 停止できた場合は、制動に使った秒数を差し引き、残りを新しい向きへの加速に使う。
            remaining -= stopTime;
            result.x = result.z = 0.0f;
            speed = 0.0f;
        }
        // 目標の速さに応じて加速・制動を選び、逆入力で停止した場合は残りの秒数を使います。
        const float targetSpeed = std::hypot(targetX, targetZ);
        const float seconds = targetSpeed < speed ? settings.brakingTime : settings.accelerationTime;
        const float dx = targetX - result.x, dz = targetZ - result.z;
        const float distance = std::hypot(dx, dz);
        // 現在と目標の差がほぼ無いか加減速時間が0以下なら、目標速度を即時反映する。
        if (distance <= 0.0001f || seconds <= 0.0f) {
            result.x = targetX; result.z = targetZ;
            return result;
        }
        // 目標との差全体に同じ係数を掛け、今フレームで許される速度変化量まで近づける。
        const float blend = (std::min)(1.0f, maximumSpeed * response * remaining / (seconds * distance));
        result.x += dx * blend;
        result.z += dz * blend;
        return result;
    }
};