#pragma once
#include "IMoveStrategy.h"

// 横スクロール風の 2D 入力に制限するためのプレイヤー移動戦略
// 関連：PlayerMoverから入力を受け、移動可能な平面に沿った速度を返す。
// 主な入口：CalculateVelocity
class MoveStrategy2D : public IMoveStrategy {
  public:
    // A・D入力をワールドX方向の速度にする。両方押した場合はDを優先する。
    engine::Vector3 CalculateVelocity(Player* player) override;

  private:
    engine::Math math_;
};
