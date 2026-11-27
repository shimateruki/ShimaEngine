#pragma once
#include <cstdint>

#include "BaseGimmick.h"

/// <summary>
/// プレイヤーの乗車で動き始め、道中の妨害を順番に起動して終点で落下する輸送床。
/// </summary>
// BaseGimmickの衝突通知でプレイヤーを運ぶ。途中の妨害はBaseSceneへ連番IDで送る。
// 主な入口：Initialize / Update / OnCollision / OnTrigger
class GimmickHazardRideFloor : public BaseGimmick {
  public:
    // 表示名とは独立した、既存シーンの保存IDと生成モデルです。
    static constexpr const char* kTypeId = "HazardRideFloor";
    static constexpr const char* kModelPath = "Stages/star_garden_ride_platform";

    // 移動床の接地判定と、走行・妨害・落下の編集項目を用意する。
    void Initialize(engine::Object3DCommon* common, const std::string& modelName) override;

    /// 乗車から終点落下までの状態を進める。
    void Update(float deltaTime) override;

    // プレイヤーの上面接触で発進し、そのフレームの床の変位を渡す。常にtrueを返す。
    bool OnCollision(engine::Object3D* other) override;

    // 外部イベントでも、待機中の床なら発進予告へ移れる。
    void OnTrigger() override;

    /// 配置用の輸送床複製を作る。
    std::unique_ptr<engine::Object3D> Clone() const override;

  private:
    enum class State {
        kWaiting,
        kStarting,
        kMoving,
        kWarning,
        kFalling,
        kHidden,
    };

    /// Editor で配置した初期位置と回転を記録する。
    void CaptureStartTransform();

    /// 乗車演出と移動を開始する。
    void BeginRide();

    // 進捗を0〜1の割合で受け、通過した地点の妨害を起動する。
    void TriggerPendingHazards(float progress);

    /// 再試行時に開始位置と妨害の進行を戻す。
    void ResetForRetry();

    /// 次の床状態とその経過時間を切り替える。
    void ChangeState(State nextState);

    /// 乗車イベントの終了後に初期位置へ戻すべきか返す。
    bool ShouldReturnAfterRide() const;

    /// 配置回転から進行方向を求める。
    engine::Vector3 GetTravelDirection() const;

    // 走行速度。ワールド単位/秒で最小0.1。
    float GetTravelSpeed() const;

    // 出発点から終点までの距離。ワールド単位で最小0.1。
    float GetTravelDistance() const;

    // 発進前の予告時間。秒単位で0を許可する。
    float GetStartDelay() const;

    // 終点に着いてから落下を始めるまでの秒数。
    float GetEndWarningDuration() const;

    // 落下開始から消失までの秒数。
    float GetFallDuration() const;

    // 鉛直速度から毎秒差し引く落下加速度。
    float GetFallGravity() const;

    // 走行中に通知する妨害の数。0〜16に制限する。
    std::int32_t GetHazardCount() const;

  private:
    State state_ = State::kWaiting;
    engine::Vector3 startPosition_{};
    engine::Vector3 startRotation_{};
    // 直前のUpdateで床が動いた分。上面のプレイヤーへ適用する。
    engine::Vector3 frameDelta_{};
    float stateTimer_ = 0.0f; // 現在の状態に使う経過秒数。
    float travelledDistance_ = 0.0f;
    float fallVelocityY_ = 0.0f;
    // 次に送る妨害の番号。Target IDに加える0始まりの値。
    std::int32_t nextHazardIndex_ = 0;
    bool hasCapturedStartTransform_ = false;
};
