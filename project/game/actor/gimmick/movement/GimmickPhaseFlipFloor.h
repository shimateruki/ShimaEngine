#pragma once
#include <cstdint>
#include "BaseGimmick.h"

// フェーズ周期で足場の可視性と衝突属性を交互に切り替えます。
// 同じ周期でも配置ごとに異なる位相を割り当て、回転表示と足場の有効状態を切り替える。
// 主な入口：Initialize / Update
class GimmickPhaseFlipFloor : public BaseGimmick {
  public:
    // 表示名とは独立した、既存シーンの保存IDと生成モデルです。
    static constexpr const char* kTypeId = "PhaseFlipFloor";
    static constexpr const char* kModelPath = "Stages/block";

    virtual ~GimmickPhaseFlipFloor() = default;

    // 共通の床判定と、位相番号・位相数・切替間隔の編集項目を用意する。
    void Initialize(engine::Object3DCommon* common, const std::string& modelName) override;

    // 編集時は元の状態へ戻し、再生時だけ位相のタイマーを進めます。
    void Update(float deltaTime) override;

    // 複製先の基本資源を作ってから配置と周期設定を移します。
    std::unique_ptr<engine::Object3D> Clone() const override;

  private:
    // 自分の位相に入ったとき足場を有効にする。phaseProgressはその位相内の進行率。
    void ApplyState(bool isOwnPhase, float phaseProgress);

    // 位相番号を4色で繰り返し、0〜1のalphaを色に加える。
    engine::Vector4 GetPhaseColor(float alpha) const;

    // この床の位相番号。0から位相数-1の範囲に収める。
    std::int32_t GetPhaseIndex() const;

    // 周期あたりの位相の個数。最低1。
    std::int32_t GetPhaseCount() const;

    // 位相一つの所要時間。秒単位で最小0.1。
    float GetPhaseDuration() const;

    // 回転の開始と終了で角速度が急変しないようにします。
    float SmoothStep(float t) const;

    uint32_t originalCollisionAttribute_ = 0;
    uint32_t originalCollisionMask_ = 0;
    engine::Vector3 baseRotation_ = engine::Vector3::kZero;
    // 位相計算に使うプレイ中の経過秒数。
    float cycleTimer_ = 0.0f;
    bool initializedForPlay_ = false;
    bool wasOwnPhase_ = false;
    std::int32_t flipCount_ = 0;
};
