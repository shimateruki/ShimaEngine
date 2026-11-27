#pragma once

// 担当：水・特殊材質のGPU設定とJSONの変換を共通化する。
// 関連：MeshRenderer::WaterParamForGpu、Object3D（保存・読込）
// 主な入口：Read / Write

#include "MeshRenderer.h"
#include "json.hpp"

namespace waterParamJsonCodec {

// シーンと単体Objectの保存経路で、同じ authored パラメータだけを書き出します。
inline nlohmann::json Write(const engine::MeshRenderer::WaterParamForGpu& water) {
    // GPU定数全体ではなく、利用者が調整する項目だけをシーンへ保存する。
    nlohmann::json result;
    result["waveSpeed"] = water.waveSpeed;
    result["waveHeight"] = water.waveHeight;
    result["waveFrequency"] = water.waveFrequency;
    result["flowSpeedX"] = water.flowSpeedX;
    result["flowSpeedY"] = water.flowSpeedY;
    result["effectType"] = water.effectType;
    result["effectScale"] = water.effectScale;
    result["effectScaleX"] = water.effectScaleX;
    result["effectScaleY"] = water.effectScaleY;
    result["effectScaleZ"] = water.effectScaleZ;
    result["topDownDetail"] = water.topDownDetail;
    result["effectSoftness"] = water.effectSoftness;
    result["effectIntensity"] = water.effectIntensity;
    result["billboardScale"] = water.billboardScale;
    return result;
}

// 旧JSONで省略された項目は、既に設定されている値を維持します。
inline void Read(const nlohmann::json& value, engine::MeshRenderer::WaterParamForGpu& water) {
    // 保存されている項目だけ反映する。省略項目には呼び出し前の設定をそのまま使う。
    // 波の時間変化を指定する速度があれば反映する。
    if (value.contains("waveSpeed"))
        water.waveSpeed = value["waveSpeed"];
    // 波の上下幅が指定されている場合だけ、その大きさを更新する。
    if (value.contains("waveHeight"))
        water.waveHeight = value["waveHeight"];
    // 波の繰り返しを決める周波数は、キーがある場合だけ読む。
    if (value.contains("waveFrequency"))
        water.waveFrequency = value["waveFrequency"];
    // X方向の流れの速度があれば適用する。
    if (value.contains("flowSpeedX"))
        water.flowSpeedX = value["flowSpeedX"];
    // Y方向の流れの速度は、Xの設定とは独立して適用する。
    if (value.contains("flowSpeedY"))
        water.flowSpeedY = value["flowSpeedY"];
    // 効果を指定する番号が保存されている場合は、その材質効果を選ぶ。
    if (value.contains("effectType"))
        water.effectType = value["effectType"];
    // 効果全体の倍率が保存されている場合だけ読み込む。
    if (value.contains("effectScale"))
        water.effectScale = value["effectScale"];
    // X軸の効果倍率は、項目がある場合だけ全体倍率と併せて使う値を更新する。
    if (value.contains("effectScaleX"))
        water.effectScaleX = value["effectScaleX"];
    // Y軸の倍率が保存されている場合は、軸別の値を復元する。
    if (value.contains("effectScaleY"))
        water.effectScaleY = value["effectScaleY"];
    // Z軸の倍率は、Z用の指定が存在するときだけ更新する。
    if (value.contains("effectScaleZ"))
        water.effectScaleZ = value["effectScaleZ"];
    // 上方からの見え方に使う細部の調整値があれば復元する。
    if (value.contains("topDownDetail"))
        water.topDownDetail = value["topDownDetail"];
    // 効果の柔らかさの指定がある場合だけ、現在の値を更新する。
    if (value.contains("effectSoftness"))
        water.effectSoftness = value["effectSoftness"];
    // 効果の強さに項目があれば復元する。
    if (value.contains("effectIntensity"))
        water.effectIntensity = value["effectIntensity"];
    // ビルボードの大きさを指定する倍率があれば適用する。
    if (value.contains("billboardScale"))
        water.billboardScale = value["billboardScale"];
}

} // namespace waterParamJsonCodec
