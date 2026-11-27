#pragma once
#include "BaseGimmick.h"

// スイッチ操作で液面を移動し、脱出中のシーンオブジェクトも制御します。
// ObjectManagerが液面を先に更新し、Player側が現在の液面から水・マグマへの接触を調べる。脱出中はシーンイベントで床や妨害を制御する。
// 主な入口：Initialize / Update / OnTrigger / OnSwitchEvent
class GimmickLiquidLevel : public BaseGimmick {
  public:
    // 表示名とは独立した、既存シーンの保存IDと生成モデルです。
    static constexpr const char* kTypeId = "LiquidLevel";
    static constexpr const char* kModelPath = "Stages/block";

    virtual ~GimmickLiquidLevel() = default;

    // 物理衝突のない液面を配置し、材質と通常昇降・脱出の調整項目を登録する。
    void Initialize(engine::Object3DCommon* common, const std::string& modelName) override;

    // 脱出シーケンスだけは通常のスイッチ昇降と別の時間軸で進めます。
    void Update(float deltaTime) override;

    // 脱出用液面はエリア進行で動くため、通常のトリガー入力を無視します。
    void OnTrigger() override;

    // 通常昇降モードのオンで目標を変更する。オフで戻す設定がある場合は元の高さを目標にする。
    void OnSwitchEvent(bool active) override;

    // 複製先の描画資源を作った後で、液面の設定を引き継ぎます。
    std::unique_ptr<engine::Object3D> Clone() const override;

  private:
    // 脱出演出の開始・終了に合わせて関連オブジェクトを切り替えます。
    void UpdateEscape(float deltaTime);

    // 液面に接続された床・噴出口・砲台、連番IDのスイッチ・入口を脱出の開始状態へ揃える。
    void ResetEscapeObjects(bool active);

    bool escapeStarted_ = false;
    bool escapeFinished_ = false;
    // 脱出開始からの経過秒数。予告時間を含む。
    float escapeTime_ = 0.0f;

    // オンでは配置位置からY方向に設定量ずらした位置、オフでは配置位置を目標にする。
    void ApplyTarget(bool active);

    // 色種別に合わせて水とマグマの材質・透過色を切り替えます。
    void ApplyLiquidVisual();

    engine::Vector3 basePosition_ = engine::Vector3::kZero;
    engine::Vector3 targetPosition_ = engine::Vector3::kZero;
    bool initializedForPlay_ = false;
    bool active_ = false;
};
