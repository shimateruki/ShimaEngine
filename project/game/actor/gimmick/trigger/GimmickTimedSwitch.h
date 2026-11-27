#pragma once
#include "BaseGimmick.h"

// 上面接触の通知で接続先へ単発イベントを送り、押下色を短時間保つスイッチ。
// BaseGimmickの衝突通知を使い、イベントの接続先はBaseSceneへ渡す。
// 主な入口：Initialize / Update / OnCollision
class GimmickTimedSwitch : public BaseGimmick {
  public:
    // 表示名とは独立した、既存シーンの保存IDと生成モデルです。
    static constexpr const char* kTypeId = "TimedSwitch";
    static constexpr const char* kModelPath = "Stages/block";

    virtual ~GimmickTimedSwitch() = default;

    // 上面を踏める地面として登録し、待機時の色を設定する。
    void Initialize(engine::Object3DCommon* common, const std::string& modelName) override;

    // 押下色の保持時間を減らし、0になったら待機色へ戻す。
    void Update(float deltaTime) override;

    // プレイヤーが上面を踏むと0.12秒の表示保持を更新する。接続先とシーンがあればイベントも送る。
    bool OnCollision(engine::Object3D* other) override;

    // 押下タイマーは持ち込まず、配置と接続先の設定だけを複製する。
    std::unique_ptr<engine::Object3D> Clone() const override;

  private:
    float contactTimer_ = 0.0f; // 床やスイッチの接触保持までの残り秒数。
};
