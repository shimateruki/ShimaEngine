#pragma once
#include "engine/utility/math/Math.h"

namespace engine { class InputManager; }
namespace engine { class Camera; }
struct Transform;
class Player;

/// <summary>
/// プレイヤーの移動戦略（アルゴリズム）を定義するインターフェース
/// </summary>
// 関連：PlayerMoverが現在の操作方式に合わせて移動速度の計算を呼び出す。
// 主な入口：CalculateVelocity
class IMoveStrategy {
  public:
    virtual ~IMoveStrategy() = default;

    /// <summary>
    /// 入力と状態から、プレイヤーに適用する水平速度の要求値を計算する。
    /// </summary>
    /// <param name="player">操作対象の有効な非所有参照。実装はこの引数から入力を取得する。</param>
    /// <returns>ワールド単位/秒の水平移動速度。2DではワールドX成分に限定する。</returns>
    virtual engine::Vector3 CalculateVelocity(Player* player) = 0;
};