#pragma once
#include <cstdint>

// 担当：草を倒す現在位置と通過履歴を、描画用に保持・減衰する。
// 関連：MeshRenderer（描画データへ反映）。物理判定は変更しない。
// 主な入口：Reset / Update
#include <array>
#include <cmath>

// 草を倒す現在位置と短い通過履歴。物理判定には影響しない。
namespace foliageInteraction {
// 8点分の (ワールドX, Y, Z, 倒れの強さ)。先頭は現在位置、後続は減衰する通過履歴。
inline std::array<float, 32> samples{};
inline float sampleTimer = 0.0f;

/// <summary>草の倒れ状態と通過履歴を消去する。</summary>
inline void Reset() { samples.fill(0.0f); sampleTimer = 0.0f; }

/// <summary>現在位置を記録し、古い草の倒れ跡を時間経過で減衰させる。</summary>
inline void Update(float x, float y, float z, float deltaTime) {
    // 経過秒が非有限か負の場合は、履歴の減衰や位置を更新しない。
    if (!std::isfinite(deltaTime) || deltaTime < 0.0f)
        return;
    const float dx = x - samples[0], dz = z - samples[2];
    // 高さの変化を含めず、XZ平面の距離で通過履歴を判定します。64は8ワールド単位の二乗です。
    const float distanceSquared = dx * dx + dz * dz;
    // 前回位置から水平に8単位を超えて移動したら、ワープとして途中の履歴を消す。
    if (distanceSquared > 64.0f)
        Reset();
    // deltaTime は秒。履歴の強さだけ指数減衰させ、座標は記録地点に残す。
    const float decay = std::exp(-3.5f * deltaTime);
    sampleTimer += deltaTime;
    for (std::int32_t i = 1; i < 8; ++i)
        samples[i * 4 + 3] *= decay;
    // 前回の点が有効で、水平移動が0.01単位を超え、前回の記録から0.06秒以上経った場合だけ、前回値を履歴へ送る。
    if (samples[3] > 0.0f && distanceSquared > 0.0001f && sampleTimer >= 0.06f) {
        sampleTimer = 0.0f;
        // 古い側から移し、まだコピーしていない一つ前の履歴を上書きしないようにします。
        for (std::int32_t i = 7; i > 1; --i)
            for (std::int32_t j = 0; j < 4; ++j)
                samples[i * 4 + j] = samples[(i - 1) * 4 + j];
        for (std::int32_t j = 0; j < 4; ++j)
            samples[4 + j] = samples[j];
    }
    // 履歴へ前回値を移したあと、先頭だけを今回の位置と強さで更新します。
    samples[0] = x;
    samples[1] = y;
    samples[2] = z;
    samples[3] = 1.0f;
}
}
