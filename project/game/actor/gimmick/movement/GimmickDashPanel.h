#pragma once
#include "BaseGimmick.h"

/// <summary>上面を踏んだとき、種類に応じてパネル加速を開始または解除し、発光します。</summary>
// 上面と接触したPlayerにパネル加速の開始・解除を依頼し、接触履歴で同じ踏み込みの再発動を防ぐ。
// 主な入口：Initialize / Update / OnCollision
class GimmickDashPanel : public BaseGimmick {
  public:
    // 表示名とは独立した、既存シーンの保存IDと生成モデルです。
    static constexpr const char* kTypeId = "DashPanel";
    static constexpr const char* kStopTypeId = "DashPanelStop";
    static constexpr const char* kModelPath = "Stages/block";

    virtual ~GimmickDashPanel() = default;

    // 地面として接触し、踏んだ時の光が読める材質を既定にする。
    void Initialize(engine::Object3DCommon* common, const std::string& modelName) override;

    // 編集済みの基準色を最初に保存し、短い発光パルスの後に戻す。
    void Update(float deltaTime) override;

    // 上面接触で加速を更新する。被弾で加速が解除された場合は、一度床を離れるまで新規の加速を始めない。
    bool OnCollision(engine::Object3D* other) override;

    // 配置と調整値を複製し、実行時のパルス時計は新しい床に持ち込まない。
    std::unique_ptr<engine::Object3D> Clone() const override;

  private:
    bool touchedTopLastUpdate_ = false; // 前回のパネル更新後、上面に接触したか。
    bool touchedTopThisUpdate_ = false; // 今回の更新後の衝突で上面に接触したか。
    bool visualBaseCaptured_ = false;
    float visualTime_ = 0.0f; // 見た目の周期に使う経過秒数。
    // 踏んだ直後の一時発光の強さ。Updateで減衰する。
    float activationPulse_ = 0.0f;
    // 次の一時発光を始められるまでの残り秒数。
    float activationCooldown_ = 0.0f;
    // 初回の更新時に保存する、パルス前の発光量。
    float baseEmissive_ = 1.2f;
    engine::Vector4 baseColor_ = {1.0f, 0.38f, 0.04f, 1.0f};
};
