#pragma once

#include "engine/utility/math/Math.h"
#include "Easing.h"
#include <numbers>

#include <algorithm>
#include <cmath>

// スライム系の通常移動に使う、共通のぷにぷに伸縮。
namespace slimeBounceAnimator {

// Paramsは、スライムの待機、移動、空中、チャージ時の伸縮量を調整する設定です。
struct Params {
    float speedForFullBounce = 2.5f;
    float idleAmplitude = 0.05f;
    float moveAmplitude = 0.15f;
    float hopFrequency = 8.5f;
    float horizontalSquash = 0.17f;
    float verticalStretch = 0.22f;
    float airborneStretch = 0.24f;
};

// PlanarSpeedは、Y軸を除いた水平移動速度を返します。
inline float PlanarSpeed(const engine::Vector3& velocity) {
    return std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
}


// 接地中の通常スライムが次に跳べるかを進め、跳躍開始フレームだけtrueを返します。
inline bool StepGroundHop(float& hopTimer, const engine::Vector3& velocity, bool grounded, float deltaTime, float interval,
                          float speedThreshold) {
    // 空中では跳躍を重ねず、次の地上跳躍の時刻も進めない。
    if (!grounded) {
        return false;
    }

    // 水平速度がしきい値以下なら、周期の72%で待機させ、移動を再開してから間もなく跳べるようにする。
    if (PlanarSpeed(velocity) <= speedThreshold) {
        hopTimer = interval * 0.72f;
        return false;
    }

    hopTimer += deltaTime;
    // 移動を続けても一周期に届くまでは、次の跳躍を開始しない。
    if (hopTimer < interval) {
        return false;
    }

    hopTimer = 0.0f;
    return true;
}

// 周期の最後にある予備動作区間を、0から1の滑らかな進行率へ変換します。
inline float BossHopAnticipation(float hopTimer, float interval, float anticipationDuration) {
    const float duration = (std::max)(anticipationDuration, 0.01f);
    const float progress = std::clamp((hopTimer - (interval - duration)) / duration, 0.0f, 1.0f);
    return Easing::SmoothStep(progress);
}


// ボスの移動意図を保ちながら、跳ぶ直前だけ足を止めて力をためます。
inline bool StepBossGroundHop(float& hopTimer, engine::Vector3& velocity, float intentSpeed, bool grounded, float deltaTime,
                              float interval, float anticipationDuration, float speedThreshold) {
    // ボスも空中では次の地上跳躍を始めず、時刻を進めない。
    if (!grounded) {
        return false;
    }
    // 移動指示がしきい値以下か、水平の実速度がほぼゼロなら、予備動作の開始位置で時間を止める。
    if (intentSpeed <= speedThreshold || PlanarSpeed(velocity) <= 0.001f) {
        hopTimer = (std::max)(0.0f, interval - anticipationDuration);
        return false;
    }
    hopTimer += (std::max)(deltaTime, 0.0f);
    // 周期を満たしたフレームでタイマーを0に戻し、呼び出し元に跳躍開始を知らせる。
    if (hopTimer >= interval) {
        hopTimer = 0.0f;
        return true;
    }
    // 周期が残っている間は、予備動作の進行に合わせて水平速度だけを落とす。
    const float anticipation = BossHopAnticipation(hopTimer, interval, anticipationDuration);
    const float movementScale = 1.0f - anticipation * 0.88f;
    velocity.x *= movementScale;
    velocity.z *= movementScale;
    return false;
}


inline engine::Vector3 MakeScale(const engine::Vector3& baseScale, const engine::Vector3& velocity, float timer, bool grounded,
                         const Params& params = {}) {
    // 空中では縦長、接地中は速度に応じた周期でつぶれと伸びを切り替える。
    const float speedRate =
        std::clamp(PlanarSpeed(velocity) / (std::max)(params.speedForFullBounce, 0.01f), 0.0f, 1.0f);
    const float idlePulse = std::sin(timer * 3.1f) * params.idleAmplitude;

    engine::Vector3 scale = baseScale;
    // 空中では縦方向の速度の大きさで伸びを決め、地上の伸縮は重ねない。
    if (!grounded) {
        const float fallRate = std::clamp(std::abs(velocity.y) / 22.0f, 0.0f, 1.0f);
        const float stretch = params.airborneStretch * (0.45f + fallRate * 0.55f);
        scale.x = baseScale.x * (1.0f - stretch * 0.42f);
        scale.y = baseScale.y * (1.0f + stretch);
        scale.z = baseScale.z * (1.0f - stretch * 0.42f);
        return scale;
    }

    // 地上では水平速度で周期と振幅を調整し、待機中は小さな呼吸だけを残します。
    const float phase = timer * (params.hopFrequency + speedRate * 4.0f);
    const float wave = std::sin(phase);
    const float squash = (std::max)(0.0f, -wave) * speedRate;
    const float stretch = (std::max)(0.0f, wave) * speedRate;
    const float sideWobble = std::sin(phase * 0.5f + 0.8f) * params.moveAmplitude * 0.22f * speedRate;
    const float movingBody = params.moveAmplitude * speedRate;

    scale.x = baseScale.x * (1.0f + idlePulse * 0.55f + movingBody * 0.35f + squash * params.horizontalSquash -
                             stretch * 0.06f + sideWobble);
    scale.y = baseScale.y * (1.0f - idlePulse * 0.85f - movingBody * 0.10f - squash * params.horizontalSquash * 0.92f +
                             stretch * params.verticalStretch);
    scale.z = baseScale.z * (1.0f + idlePulse * 0.55f + movingBody * 0.28f + squash * params.horizontalSquash * 0.78f -
                             stretch * 0.05f - sideWobble * 0.7f);
    return scale;
}


// 溜め率と強度に応じて身体をつぶし、X軸とZ軸に逆位相の小さな揺れを重ねます。
inline engine::Vector3 MakeChargeSquash(const engine::Vector3& baseScale, float chargeRate, float timer, float strength = 1.0f) {
    chargeRate = std::clamp(chargeRate, 0.0f, 1.0f);
    const float pulse = std::sin(timer * 18.0f) * 0.04f * chargeRate * strength;
    return {baseScale.x * (1.0f + chargeRate * 0.34f * strength + pulse),
            baseScale.y * (1.0f - chargeRate * 0.34f * strength),
            baseScale.z * (1.0f + chargeRate * 0.34f * strength - pulse)};
}


// 移動方向に応じて身体を傾け、横移動でもただ滑って見えないようにします。
inline void ApplyTravelPose(engine::Vector3& scale, engine::Vector3& rotation, const engine::Vector3& baseScale, const engine::Vector3& velocity,
                            float facingYaw, float fullSpeed, float forwardLean, float sideLean) {
    const float safeSpeed = (std::max)(fullSpeed, 0.01f);
    const float forward = velocity.x * std::sin(facingYaw) + velocity.z * std::cos(facingYaw);
    const float side = velocity.x * std::cos(facingYaw) - velocity.z * std::sin(facingYaw);
    const float forwardRate = std::clamp(forward / safeSpeed, -1.0f, 1.0f);
    const float sideRate = std::clamp(side / safeSpeed, -1.0f, 1.0f);
    rotation.x -= forwardRate * forwardLean;
    rotation.z -= sideRate * sideLean;
    scale.x += baseScale.x * std::abs(sideRate) * 0.025f;
    scale.z += baseScale.z * std::abs(forwardRate) * 0.035f;
}

// 跳躍前のためと着地後のつぶれを、実際の跳躍タイミングに合わせて加えます。
inline void ApplyGroundHopPose(engine::Vector3& scale, const engine::Vector3& baseScale, bool grounded, float hopTimer,
                               float hopInterval, float anticipationDuration, float landingTimer,
                               float landingDuration) {
    // 空中姿勢に地面を踏む前後のつぶれを重ねないよう、接地時だけ適用する。
    if (!grounded) {
        return;
    }

    const float windup = BossHopAnticipation(hopTimer, hopInterval, anticipationDuration);
    const float landing = std::clamp(landingTimer / (std::max)(landingDuration, 0.01f), 0.0f, 1.0f);
    const float landingAge = 1.0f - landing;
    const float rebound = std::sin(landingAge * (2.0f * std::numbers::pi_v<float>)) * landing * 0.10f;
    const float squash = windup * 0.29f + landing * landing * 0.26f + rebound;
    scale.x += baseScale.x * squash;
    scale.y -= baseScale.y * squash * 1.20f;
    scale.z += baseScale.z * squash;
}

// 減衰比を指定できる安定したスプリング補間です。1未満では少し反発し、1で臨界減衰になります。
inline void StepDampedSpring(engine::Vector3& value, engine::Vector3& velocity, const engine::Vector3& target, float deltaTime, float frequency,
                             float dampingRatio) {
    // 大きいフレーム間隔を制限し、各軸を同じ暗黙積分式で安定して追従させる。
    const float safeDelta = std::clamp(deltaTime, 0.0f, 0.05f);
    // 安全な刻み幅が0なら、位置と速度を更新しない。
    if (safeDelta <= 0.0f) {
        return;
    }

    const float omega = (std::max)(frequency, 0.01f);
    const float damping = (std::max)(dampingRatio, 0.0f);
    const float f = 1.0f + 2.0f * safeDelta * damping * omega;
    const float omegaSquared = omega * omega;
    const float deltaOmegaSquared = safeDelta * omegaSquared;
    const float deltaSquaredOmegaSquared = safeDelta * deltaOmegaSquared;
    // 位置と速度を同時に解く暗黙積分の共通分母を逆数で保持し、各軸で同じ係数を使います。
    const float inverseDeterminant = 1.0f / (f + deltaSquaredOmegaSquared);

    const auto stepComponent = [&](float& current, float& currentVelocity, float targetValue) {
        const float previous = current;
        current =
            (f * previous + safeDelta * currentVelocity + deltaSquaredOmegaSquared * targetValue) * inverseDeterminant;
        currentVelocity = (currentVelocity + deltaOmegaSquared * (targetValue - previous)) * inverseDeterminant;
    };

    stepComponent(value.x, velocity.x, target.x);
    stepComponent(value.y, velocity.y, target.y);
    stepComponent(value.z, velocity.z, target.z);
}

} // namespace slimeBounceAnimator
