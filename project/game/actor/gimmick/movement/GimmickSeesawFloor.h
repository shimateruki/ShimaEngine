#pragma once
#include "BaseGimmick.h"

/// <summary>接触位置に応じて傾き、離れると元の角度に戻る床です。</summary>
// BaseGimmickの上面接触から床のローカルX位置を求め、傾きの目標角を更新する。
// 主な入口：Initialize / Update / OnCollision
class GimmickSeesawFloor : public BaseGimmick {
  public:
    // 表示名とは独立した、既存シーンの保存IDと生成モデルです。
    static constexpr const char* kTypeId = "SeesawFloor";
    static constexpr const char* kModelPath = "Stages/block";

    virtual ~GimmickSeesawFloor() = default;

    // シーソーの天面を地面として扱い、乗員を支えられるようにする。
    void Initialize(engine::Object3DCommon* common, const std::string& modelName) override;

    // 編集時の姿勢を基準角として保持し、Play中の傾きから復帰できるようにする。
    void Update(float deltaTime) override;

    // 上面を踏んだプレイヤーのローカルX位置で傾きを決める。戻り値は常にtrue。
    bool OnCollision(engine::Object3D* other) override;

    // 揺れの途中状態は持ち越さず、配置と設定から新しい床を作る。
    std::unique_ptr<engine::Object3D> Clone() const override;

  private:
    // 初回に確保した配置時の回転。傾きの追加と復帰に使う。
    engine::Vector3 baseRotation_ = engine::Vector3::kZero;
    float contactTimer_ = 0.0f; // 床やスイッチの接触保持までの残り秒数。
    // ローカルX位置から求めたZ軸の目標傾き。ラジアン。
    float targetTilt_ = 0.0f;
    // 最大傾きに達するローカルX方向の半幅。
    float halfLength_ = 5.0f;
    // 中央の姿勢から傾ける最大角度。ラジアン。
    float maxTilt_ = 0.35f;
    // 角度を目標へ近づける指数補間の係数。
    float tiltSpeed_ = 7.0f;
    bool initializedBase_ = false;
};
