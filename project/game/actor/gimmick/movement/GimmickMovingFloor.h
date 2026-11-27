#pragma once
#include "BaseGimmick.h"
class RidingPlatformController;

/// <summary>移動床の軌道と、乗っているキャラクターへの移動量を管理します。</summary>
// 通常の経路では床の変位を乗員へ渡す。乗車シーケンスが設定された場合はRidingPlatformControllerに進行を委ねる。
// 主な入口：Initialize / Update / OnCollision
class GimmickMovingFloor : public BaseGimmick {
  public:
    // 表示名とは独立した、既存シーンの保存IDと生成モデルです。
    static constexpr const char* kTypeId = "MovingFloor";
    static constexpr const char* kModelPath = "Stages/block";

    GimmickMovingFloor();

    ~GimmickMovingFloor() override;

    // PathMoverComponentに空でない乗車シーケンスが登録されている場合にtrueを返す。
    bool IsRidePlatform() const;

    // 複製時は配置と設定だけを引き継ぎ、乗員・周期・原点は再生時に再取得する。
    std::unique_ptr<engine::Object3D> Clone() const override;

    // 床として接地判定できる属性を与え、旧データの速度欠落を補う。
    void Initialize(engine::Object3DCommon* common, const std::string& modelName) override;

    // 移動前の接地で乗員を集め、床が動いた変位を適用する。deltaTimeは秒。
    void Update(float deltaTime) override;

    // 乗員を移動させる処理はUpdateで行う。この接触通知は共通の衝突解決を許可してtrueを返す。
    bool OnCollision(engine::Object3D* other) override;

    // 逃走ギミックで床を再利用するとき、移動周期を初期位置から再開します。
    void ResetEscapeCycle();

  private:
    std::unique_ptr<RidingPlatformController> ride_;
    engine::Vector3 startPosition_{};
    engine::Vector3 startRotation_{};
    // 床がこの更新で移動した量。乗員の位置と床速度の更新に使う。
    engine::Vector3 frameDelta_{};
    // 通常の移動周期に用いる経過秒数。
    float time_ = 0.0f;
    // 配置したXZで決まる開始位相。モード1で揺れをずらす。
    float phaseOffset_ = 0.0f;
    bool hasCapturedStartTransform_ = false;
};
