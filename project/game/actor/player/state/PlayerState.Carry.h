#pragma once

#include "IAnimationState.h"
#include "engine/utility/math/Math.h"

/// <summary>敵を持ち運んでいる間の姿勢と、運搬対象の変形の復元を管理します。</summary>
// 関連：Playerが保持する敵の運搬姿勢を更新する。投げ・吸収入力の判断はPlayer側が担当する。
// 主な入口：Enter / Update / Exit
class PlayerStateCarry : public IAnimationState {
  public:
    // 運搬モードに入り、保持対象の元の大きさを退避する。
    void Enter(Player* player) override;

    // 経過秒から保持対象の揺れを計算し、プレイヤーの頭上へ配置する。
    void Update(Player* player, float deltaTime) override;

    // 運搬中に変えた敵の縮尺を戻す。投げた後の状態へ変形を持ち越さない。
    void Exit(Player* player) override;

  private:
    float struggleTimer_ = 0.0f; // 運搬中のもがきに使う経過秒数。
    engine::Vector3 carriedBaseScale_ = {1.0f, 1.0f, 1.0f};
};
