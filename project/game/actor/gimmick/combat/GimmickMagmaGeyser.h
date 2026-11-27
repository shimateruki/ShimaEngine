#pragma once

#include <cstdint>
#include "game/actor/enemy/combat/AttackTelegraph.h"
#include "BaseGimmick.h"

#include <memory>
#include <array>

// 噴出口の周期と接触攻撃、単発の火山弾、移動床に追従する水柱を管理する。
// 噴出から得た上昇量を移動床へ渡し、弾はBulletManager、特殊材質はシーンの専用描画パスを使う。
// 主な入口：Initialize / Update / OnCollision / OnTrigger
class GimmickMagmaGeyser : public BaseGimmick {
  public:
    // 表示名とは独立した、既存シーンの保存IDと生成モデルです。
    static constexpr const char* kTypeId = "MagmaGeyser";
    static constexpr const char* kModelPath = "Stages/magma_vent";

    /// 噴出口のモデル、周期、攻撃予告を初期化する。
    void Initialize(engine::Object3DCommon* common, const std::string& modelName) override;

    /// 警告、噴出、休止の周期を進める。
    void Update(float deltaTime) override;

    /// 噴出口本体と通常材質の演出を描画する。
    void Draw(ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource) override;

    /// 指定カメラのプレビューへ噴出口を描画する。
    void DrawForCamera(engine::Camera* camera, ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource,
                       std::int32_t previewBufferIndex = 0) override;

    // 噴射ダメージモードで噴出中のプレイヤーへの接触にだけダメージを通知する。常にtrueを返す。
    bool OnCollision(engine::Object3D* other) override;

    // 単発トリガーで稼働状態の反転を要求する。火山弾の単発発射はRequestFireballEruptionを使う。
    void OnTrigger() override;

    /// スイッチ状態に応じて周期動作を有効・無効にする。
    void OnSwitchEvent(bool active) override;

    /// 特殊材質で描く噴出表示を所有するか返す。
    bool HasOwnedSpecialMaterialVisuals() const override;

    /// 噴出表示を通常カメラの特殊材質パスへ描く。
    void DrawOwnedSpecialMaterialVisuals(uint32_t depthSrvHandle, uint32_t grabSrvHandle) override;

    /// 噴出表示をプレビューカメラの特殊材質パスへ描く。
    void DrawOwnedSpecialMaterialVisualsForCamera(engine::Camera* camera, uint32_t depthSrvHandle, uint32_t grabSrvHandle,
                                                  std::int32_t previewBufferIndex = 0) override;

    /// 配置用の噴出口複製を作る。
    std::unique_ptr<engine::Object3D> Clone() const override;

    // 移動床は同じ周期から上昇量を取得し、独立したタイマーを持たない。
    // ワールド距離の上昇量と、0～1 に収まる予兆/圧力の進行率を同一周期から返します。
    float GetLiftOffset() const;

    float GetWarningProgress() const;

    float GetPressureEnvelope() const;

    /// 逃走イベント向けに噴出周期と有効状態を初期値へ戻す。
    void ResetEscapeCycle(bool active);

    // タイムラインから単発火山弾を要求する。プレイ外・既に処理中・水柱の場合は受け付けない。
    void RequestFireballEruption();

  private:
    /// 水版は接続した移動床の実高さへ柱を同期し、炎やダメージを発生させない。
    void UpdateWaterJet(float deltaTime);

    /// 単発噴出の火球を生成する。
    void FireMagmaProjectiles();

    /// 単発噴出の予告から火球発射までを進める。
    void UpdateSingleEruption(float deltaTime);

    bool singleEruption_ = false;
    bool singleFired_ = false;
    // 単発の予告が始まってからの経過秒数。発射後の待ち時間にも使う。
    float singleTimer_ = 0.0f;

    /// 単発噴出で飛ぶ火球の移動時間を返す。
    float GetFireballFlightDuration() const;

    enum class Phase {
        kWarning,
        kErupting,
        kCooldown,
    };

    /// Play 中の更新か判定する。
    bool IsPlaying() const;

    // プレイヤーがいないか、噴出口との水平距離が稼働範囲内の場合にtrueを返す。
    bool IsPlayerWithinSimulationRange() const;

    /// Play 開始時の周期と演出を設定する。
    void BeginPlayCycle();

    /// Play 外の配置表示へ戻す。
    void ResetOutsidePlay();

    /// 攻撃予告段階へ切り替える。
    void BeginWarning();

    // 噴出段階に入り、火山弾または開始エフェクトを出す。柱と接触の更新はUpdateEruptionVisualが担当する。
    void BeginEruption();

    /// 指定時間の休止段階へ切り替える。
    void BeginCooldown(float duration);

    /// 現在段階の終了後に次の段階へ進める。
    void AdvancePhase();

    /// 警告表示の進行率を噴出周期へ同期する。
    void UpdateWarningVisual();

    /// 噴出柱の形状と表示を更新する。
    void UpdateEruptionVisual(float deltaTime);

    /// 休止中の見た目を更新する。
    void UpdateCooldownVisual();

    /// 圧力に応じて煮え立つ泡を更新する。
    void UpdateBoilingVisual(float deltaTime, float pressure, bool visible);

    /// 泡の表示をすべて隠す。
    void HideBoilingVisuals();

    /// 噴出高さに応じて当たり判定を有効・無効化する。
    void ApplyCollisionState(bool enabled, float activeHeight);

    /// 噴出開始時の一回限りの演出を発生させる。
    void SpawnEruptionBurst();

    /// 噴出中の粒子を発生させる。
    void EmitEruptionParticles();

    // 発射口のワールド座標を返す。接続した上昇液面があれば火山弾の発射高さに使う。
    engine::Vector3 GetMouthPosition() const;

    // 警告の所要秒数。設定値は0.2秒以上。
    float GetWarningDuration() const;

    // 噴出段階の所要秒数。設定値は0.2秒以上。
    float GetEruptionDuration() const;

    // 次の警告までの休止秒数。設定値は0.2秒以上。
    float GetCooldownDuration() const;

    // プレイ開始または脱出開始時の待ち秒数。0〜30秒に収める。
    float GetInitialDelay() const;

    // 噴射と火山弾で使うダメージ量。負の値は0。
    float GetDamage() const;

    // 最大噴出高をワールド単位で返す。
    float GetGeyserHeight() const;

    // 円柱の描画と接触判定の半径をワールド単位で返す。
    float GetGeyserRadius() const;

    // 被弾時に与える水平方向の速度の大きさ。
    float GetHorizontalKnockback() const;

    // 被弾時に与える上向きの速度の大きさ。
    float GetVerticalKnockback() const;

    // シミュレーションを行うプレイヤーとの水平距離。20〜500ワールド単位。
    float GetSimulationWakeDistance() const;

    AttackTelegraph warningTelegraph_;
    std::unique_ptr<engine::Object3D> eruptionVisual_;

    // 泡は使い回し、予告中に生成・破棄を繰り返さない。
    std::array<std::unique_ptr<engine::Object3D>, 4> boilingVisuals_;
    float boilingClock_ = 0.0f;
    bool ventInLiquid_ = false;

    Phase phase_ = Phase::kWarning;
    // 現在の段階に入ってからの経過秒数。
    float phaseTimer_ = 0.0f;
    float phaseDuration_ = 1.0f;

    // 再び接触ダメージを与えられるまでの残り秒数。
    float damageCooldownTimer_ = 0.0f;

    // 次に噴出粒子を生成するまでの残り秒数。
    float particleTimer_ = 0.0f;
    float visualTimer_ = 0.0f;
    std::int32_t particleBandIndex_ = 0;

    bool initializedForPlay_ = false;
    bool active_ = true;
    bool simulationSleeping_ = false;

    engine::Vector4 baseColor_ = engine::colors::kWhite;
    float baseEmissive_ = 1.0f;
};
