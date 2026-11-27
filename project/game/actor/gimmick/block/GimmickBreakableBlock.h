#pragma once
#include <cstdint>
#include "BaseGimmick.h"

class Player;

// ブロックやガラスの破壊条件を判定し、破片と閃光を出します。
// 爆風は衝突通知から、プレイヤーの体当たりは押し戻し前の問い合わせから判定する。破壊後は表示と衝突を止めて演出を出す。
// 主な入口：Initialize / Update / OnCollision
class GimmickBreakableBlock : public BaseGimmick {
  public:
    // 表示名とは独立した、既存シーンの保存IDと生成モデルです。
    static constexpr const char* kTypeId = "BreakableBlock";
    static constexpr const char* kModelPath = "Stages/bomb_break_block";

    // ダッシュパネルによる加速中の体当たり専用モードの保存値です。
    static constexpr std::int32_t kDashPanelOnly = 8;

    virtual ~GimmickBreakableBlock() = default;

    // 地形として配置し、破壊を許可する攻撃の種類を編集項目に登録する。
    void Initialize(engine::Object3DCommon* common, const std::string& modelName) override;

    // 破壊済みなら表示と衝突を無効にし、死亡状態にする。
    void Update(float deltaTime) override;

    // 起爆したプレイヤー所有の爆弾の爆風で壊せるか調べる。体当たりはTryBreakByPlayerImpactで扱う。
    bool OnCollision(engine::Object3D* other) override;

    // プレイヤーの押し戻し前に呼び、今回の衝突で破壊したときだけtrueを返します。
    bool TryBreakByPlayerImpact(const Player* player);

    // 同じモデルで新しい実体を初期化してから、配置と破壊設定を複製する。
    std::unique_ptr<engine::Object3D> Clone() const override;

  private:
    // 破壊時点で衝突を外し、そのフレームのプレイヤー移動を通せるようにする。続けて破片と閃光を出す。
    void Break(); // 破壊処理

  private:
    bool isBroken_ = false;
};
