#pragma once

#include "BaseGimmick.h"

/// <summary>マグマ面の発光を更新し、旧配置の接触ダメージ設定を保持します。</summary>
// マグマの材質と発光を管理する。液面の波を含む死亡判定はPlayerが担当する。
// 主な入口：Initialize / Update / OnCollision
class GimmickMagmaHazard : public BaseGimmick {
  public:
    // 表示名とは独立した、既存シーンの保存IDと生成モデルです。
    static constexpr const char* kTypeId = "MagmaHazard";
    static constexpr const char* kModelPath = "Stages/block";

    // マグマ材質と、旧配置から読み込む接触ダメージの設定を用意する。
    void Initialize(engine::Object3DCommon* common, const std::string& modelName) override;

    // ばらつく発光と波に使う実行時の表示を更新する。
    void Update(float deltaTime) override;

    // 液面接触の死亡判定はPlayerに任せ、接触通知では常にtrueを返す。
    bool OnCollision(engine::Object3D* other) override;

    // 複製先を初期化してから危険域の設定と配置を移す。
    std::unique_ptr<engine::Object3D> Clone() const override;

  private:
    // 旧配置が使うダメージ設定を0以上で返す。未設定なら12。
    float GetDamage() const;

    // 旧配置が使うダメージ間隔を秒で返す。未設定なら0.8秒。
    float GetDamageInterval() const;

    float damageCooldownTimer_ = 0.0f;
    float visualTimer_ = 0.0f; // 見た目の周期に使う経過秒数。
    float baseEmissive_ = 1.8f;
    bool initializedForPlay_ = false;
};
