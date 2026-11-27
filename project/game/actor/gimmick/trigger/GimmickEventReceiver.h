#pragma once
#include "BaseGimmick.h"

// スイッチからの有効/無効イベントを、配置物の位置・表示・衝突状態へ反映します。
// BaseSceneから受けた通知をモード別に反映する。軸移動と王冠の落下はUpdateが進める。
// 主な入口：Initialize / Update / OnTrigger / OnSwitchEvent
class GimmickEventReceiver : public BaseGimmick {
  public:
    // 表示名とは独立した、既存シーンの保存IDと生成モデルです。
    static constexpr const char* kTypeId = "EventReceiver";
    static constexpr const char* kModelPath = "Stages/block";

    virtual ~GimmickEventReceiver() = default;

    // 受信物の地形判定と、出現・移動・有効化・無効化・王冠の編集項目を登録する。
    void Initialize(engine::Object3DCommon* common, const std::string& modelName) override;

    // 編集時の配置を再生基準にし、ボス報酬はイベント発火まで隠します。
    void Update(float deltaTime) override;

    // 単発トリガーはスイッチのオンイベントとして扱います。
    void OnTrigger() override;

    // 最初の再生更新より前の通知は保留する。解除の通知を受け付けるかは配置設定に従う。
    void OnSwitchEvent(bool active) override;

    // 演出の配置変更で基準座標を移す。移動中は現在位置と目標も同じ量ずらす。
    void RelocateRuntimeBase(const engine::Vector3& position);

    // 撃破演出が王冠の姿勢を決める間は、受信イベントによる落下を止める。
    void SetCinematicRewardOwned(bool owned);

    // 複製先を初期化してから、イベント設定と配置をコピーします。
    std::unique_ptr<engine::Object3D> Clone() const override;

  private:
    // モード別に表示・衝突・移動先を切り替え、報酬出現時は着地まで衝突を遅らせます。
    void ApplyActiveState(bool active);

    // 有効なら保存した属性とマスクを戻し、無効なら両方を0にする。
    void SetCollisionEnabled(bool enabled);

    // プレイ開始時に確保する配置位置。移動や王冠の着地の基準。
    engine::Vector3 basePosition_ = engine::Vector3::kZero;
    // 軸移動モードで通知に合わせて切り替える移動目標。
    engine::Vector3 targetPosition_ = engine::Vector3::kZero;
    engine::Vector3 baseScale_ = {1.0f, 1.0f, 1.0f};
    engine::Vector3 baseRotation_ = engine::Vector3::kZero;

    uint32_t originalCollisionAttribute_ = 0;
    uint32_t originalCollisionMask_ = 0;

    engine::Vector4 originalColor_ = {0.65f, 1.0f, 0.65f, 1.0f};

    // 王冠の落下を開始してからの経過秒数。
    float activationTimer_ = 0.0f;

    bool cinematicRewardOwned_ = false;
    bool initializedForPlay_ = false;
    bool active_ = false;

    // 最初の再生更新で適用する通知を受け取っているか。
    bool hasPendingActive_ = false;
    // 保留している最後の有効・無効の通知。
    bool pendingActive_ = false;
};
