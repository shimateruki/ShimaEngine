#pragma once
#include "BaseGimmick.h"

/// <summary>トリガー後に一定時間だけ出現し、消滅前に点滅する床です。</summary>
// BaseSceneから受け取った起動イベントで床の衝突を戻し、Updateで消えるまでの時間と点滅を管理する。
// 主な入口：Initialize / Update / OnTrigger
class GimmickAppearingFloor : public BaseGimmick {
  public:
    // 表示名とは独立した、既存シーンの保存IDと生成モデルです。
    static constexpr const char* kTypeId = "AppearingFloor";
    static constexpr const char* kModelPath = "Stages/block";

    virtual ~GimmickAppearingFloor() = default;

    // 表示と当たり判定を同時に切り替える床として登録する。
    void Initialize(engine::Object3DCommon* common, const std::string& modelName) override;

    // 表示時間が残り少ない場合は衝突を保ったまま点滅し、終了時に無効にする。
    void Update(float deltaTime) override;

    // 表示時間を設定値に戻し、すでに点滅している場合も画面と衝突を有効にする。
    void OnTrigger() override;

    // 新しい実体を初期化してから、出現時間と配置を引き継ぐ。
    std::unique_ptr<engine::Object3D> Clone() const override;

  private:
    // 床の稼働状態を切り替え、保存した衝突情報を復元するか無効にする。
    void SetFloorActive(bool active);

    // 床が無効になるまでの残り秒数。
    float visibleTimer_ = 0.0f;
    // 起動通知で設定する表示時間。秒単位。
    float appearDuration_ = 3.0f;
    // 点滅を始める残り秒数のしきい値。
    float blinkWarningTime_ = 1.0f;
    // 点滅の切り替え間隔。秒単位。
    float blinkInterval_ = 0.12f;
    uint32_t originalCollisionAttribute_ = 0;
    uint32_t originalCollisionMask_ = 0;
    bool initializedForPlay_ = false;
    bool isActive_ = true;
};
