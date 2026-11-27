#pragma once
#include <cstdint>

// 偽王の槍攻撃とシーン演出が共用するカメラ姿勢の計算。Cameraの所有や切替は呼出側が担当する。

#include "game/crown/CrownFinaleTuning.h"
#include "Easing.h"
#include "engine/utility/math/Math.h"

#include <algorithm>
#include <cmath>

// 第二形態の封印失敗演出で、実戦と敵攻撃プレビューが同じカットを使います。
struct CrownBarrageCameraPose {
    engine::Vector3 eye{};
    engine::Vector3 target{};
    float fov = 0.7f;
};

// 召喚カット内の秒数。カメラとボスの詠唱描画は同じ時刻に合わせます。
namespace SealRevealTiming {
inline constexpr float ChantStart = 3.76f;
inline constexpr engine::Vector3 CasterShotOffset{16.0f, 15.0f, 24.0f};
inline constexpr float Launch = 4.48f;
inline constexpr float SkyAppear = 4.84f;
inline constexpr float SkyPulse = 5.88f;
inline constexpr float GroundSurge = 6.40f;
inline constexpr float Duration = 7.75f;
}

inline CrownBarrageCameraPose EvaluateCrownBarrageCamera(float seconds, const engine::Vector3& boss,
                                                          const engine::Vector3& strike, const CrownFinaleTuning& tuning) {
    // ボスから着弾地点への水平線を基準とし、ほぼ同じ位置ならワールドの+Zを前向きに使います。
    const engine::Vector3 horizontal{strike.x - boss.x, 0.0f, strike.z - boss.z};
    const float distance = std::sqrt(horizontal.x * horizontal.x + horizontal.z * horizontal.z);
    const engine::Vector3 forward = distance > 0.01f
                                ? engine::Vector3{horizontal.x / distance, 0.0f, horizontal.z / distance}
                                : engine::Vector3{0.0f, 0.0f, 1.0f};
    const engine::Vector3 right{forward.z, 0.0f, -forward.x};
    // 各カットの前後・横と高さに別々の調整倍率を適用します。
    const auto offset = [&](const engine::Vector3& origin, float along, float side, float height) {
        return origin + forward * (along * tuning.sealBarrageCameraDistance) +
               right * (side * tuning.sealBarrageCameraDistance) +
               engine::Vector3{0.0f, height * tuning.sealBarrageCameraHeight, 0.0f};
    };
    const CrownBarrageCameraPose shots[] = {
        // プレイヤー越しに失敗の舞台と頭上の術式を映す。
        {offset(boss, 44.0f, 24.0f, 10.0f), boss + engine::Vector3{0.0f, tuning.sealChantSkyHeight, 0.0f}, 0.94f},
        // 紋章が完成してから視線を下げ、続く大槍のカットへつなぐ。
        {offset(boss, 38.0f, 25.0f, 6.0f), boss + engine::Vector3{0.0f, 16.0f, 0.0f}, 0.88f},
        // 大槍を画面中央に保ちながら落下へつなぐ。
        {offset(strike, 10.0f, 7.0f, 3.0f), strike + engine::Vector3{0.0f, 7.4f, 0.0f}, 0.62f},
        // 着弾の瞬間は低い位置で止め、槍・閃光・地表から跳ねる雷撃を重ねて見せる。
        {offset(strike, 8.0f, 6.0f, 2.4f), strike + engine::Vector3{0.0f, 3.6f, 0.0f}, 0.87f},
        // 放電が全域へ満ちた後に引き、闘技場全体の被害と消滅を見せる。
        {offset(strike, 28.0f, 18.0f, 12.0f), boss + engine::Vector3{0.0f, 2.5f, 0.0f}, 0.90f},
    };
    // 演出開始からの経過秒でカットを選び、最初のカットには補間を掛けません。
    const float boundaries[] = {0.0f, 0.56f, 1.18f, 2.62f, 3.65f};
    std::int32_t shot = 0;
    for (std::int32_t index = 1; index < 5; ++index)
        // 経過秒が切り替え時刻に達した境界を順に確認し、最後に到達したカットを選ぶ。
        if (seconds >= boundaries[index])
            shot = index;
    // まだ先頭カットの場合は補間元がないので、定義済みの初期構図をそのまま返す。
    if (shot == 0)
        return shots[0];
    // 切替直後の0.16秒だけ、視点・注視点・画角を前カットから一緒に補間します。
    const float blend = std::clamp((seconds - boundaries[shot]) / 0.16f, 0.0f, 1.0f);
    // 補間の始点と終点で速度を緩め、視点と注視点の位置、およびラジアンの画角を同じ進行率で合わせる。
    const float eased = Easing::SmoothStep(blend);
    return {shots[shot - 1].eye + (shots[shot].eye - shots[shot - 1].eye) * eased,
            shots[shot - 1].target + (shots[shot].target - shots[shot - 1].target) * eased,
            shots[shot - 1].fov + (shots[shot].fov - shots[shot - 1].fov) * eased};
}
