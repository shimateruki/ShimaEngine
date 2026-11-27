#pragma once

#include "BaseGimmick.h"

// プレイヤーの接触開始を既存の被弾処理へ渡す障害物。モデルと判定は配置側で変更できる。
// 主な入口：Initialize / OnTriggerEnter。接触の継続と再接触の判定はCollisionManagerが担当する。
class GimmickContactHazard final : public BaseGimmick {
  public:
    static constexpr const char* kTypeId = "ContactHazard";
    static constexpr const char* kModelPath = "Gimmicks/volcanic_spike_bar";

    // プレイヤー専用の接触トリガーを設定し、攻撃力の編集項目を登録する。
    void Initialize(engine::Object3DCommon* common, const std::string& modelName) override;
    // プレイ中に生存プレイヤーが触れ、正の攻撃力が設定されていれば被弾を通知する。
    void OnTriggerEnter(const engine::CollisionEvent& event) override;
    // 判定と攻撃設定を初期化した別の障害物へコピーする。
    std::unique_ptr<engine::Object3D> Clone() const override;
};
