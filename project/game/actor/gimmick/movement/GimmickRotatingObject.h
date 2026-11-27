#pragma once
#include "BaseGimmick.h"
#include <cstdint>

/// <summary>トリガーやスイッチによって回転状態を切り替えるオブジェクトです。</summary>
// スイッチから回転の有効状態を受け、単独の軌道を使うか、下の移動床に追従するかを選ぶ。
// 主な入口：Initialize / Update / OnTrigger / OnSwitchEvent
class GimmickRotatingObject : public BaseGimmick {
  public:
    // 表示名とは独立した、既存シーンの保存IDと生成モデルです。
    static constexpr const char* kTypeId = "RotatingFloor";
    static constexpr const char* kModelPath = "Stages/block";

    virtual ~GimmickRotatingObject() = default;

    // 回転軸と速度の旧既定値を用意し、地形として足場判定を有効にする。
    void Initialize(engine::Object3DCommon* common, const std::string& modelName) override;

    // 編集中は回転を進めず、Play開始時に初期姿勢を取り直す。
    void Update(float deltaTime) override;

    // トリガーはスイッチの ON と同じ経路で扱う。
    void OnTrigger() override;

    // OFF で停止するかは設定値で決め、スイッチの状態を床の回転へ反映する。
    void OnSwitchEvent(bool active) override;

    // 回転開始時刻は複製せず、保存済みの配置と設定だけ引き継ぐ。
    std::unique_ptr<engine::Object3D> Clone() const override;

  private:
    bool initializedForPlay_ = false;
    bool active_ = true;
    // 配置時や追従先の回転に足す、選択軸まわりの角度。ラジアン。
    float pathRotationOffset_ = 0.0f;
    // 親子関係を直接持たず追従する支持床。プレイ開始時に探す。
    engine::Object3D* supportFloor_ = nullptr;
    // 支持床を基準としたオブジェクトの位置。
    engine::Vector3 supportLocalPosition_{};
    // 追従開始時の支持床の角度。以後の支持床の回転差を求める基準。
    engine::Vector3 supportStartRotation_{};
    engine::Vector3 startPosition_{};
    engine::Vector3 startRotation_{};
    // 再生中に一時停止した判定を戻すために保存する、開始時の衝突属性。
    uint32_t initialCollisionAttribute_ = 0;
    // 一時非表示から戻すために保存する、開始時の可視状態。
    bool initialVisible_ = true;
};
