#pragma once
#include <cstdint>

#include "BaseGimmick.h"

// 入場でボス戦を封鎖し、撃破時に壁を開くか高冠戦の報酬を出す。
// 衝突通知はBaseGimmick、ボス・壁・報酬の状態通知はBaseScene、登場タイムラインはGamePlaySceneを使う。
// 主な入口：Initialize / Update / OnCollision / OnTrigger
class GimmickArenaEncounter : public BaseGimmick {
  public:
    // 表示名とは独立した、既存シーンの保存IDと生成モデルです。
    static constexpr const char* kTypeId = "ArenaEncounter";
    static constexpr const char* kModelPath = "Primitives/cube";

    // 入場範囲の判定と、ボス・壁・報酬をイベントIDで指定するための設定を用意する。
    void Initialize(engine::Object3DCommon* common, const std::string& modelName) override;

    /// 進入、封鎖、戦闘、撃破後の状態を進める。
    void Update(float deltaTime) override;

    // プレイしていないときに進入範囲を表示する。
    void Draw(ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource) override;

    // 待機中の範囲にプレイヤーが触れ、遭遇開始を許可されていれば開始してtrueを返す。
    bool OnCollision(engine::Object3D* other) override;

    // 撃破通知を受ける。封鎖・戦闘中でボスの撃破が確定している場合に完了処理へ進む。
    void OnTrigger() override;

    /// 配置用の遭遇装置複製を作る。
    std::unique_ptr<engine::Object3D> Clone() const override;

    // 保存済みチェックポイントから再開する際、ボス戦を再開せず障壁と報酬をクリア状態へ戻します。
    void RestoreCheckpointClear();

    /// <summary>Directorの指定時刻に高冠ボスを起動する。</summary>
    void ActivateBossFromTimeline();

  private:
    enum class State {
        kWaiting,
        kSealing,
        kFighting,
        kCleared,
    };

    /// Play 開始時のボスと障壁を待機状態へ設定する。
    void InitializeForPlay();

    // 待機段階と編集用の範囲表示へ戻し、プレイヤー検知を再度有効にする。
    void ResetForEditor();

    // 入場検知を止めて壁を有効にし、登場演出か待ち時間によるボス起動へ移る。
    void BeginEncounter();

    // 撃破を記録し、中ボス戦は壁の解除、高冠戦は報酬の有効化と受け取り演出へ進む。
    void CompleteEncounter();

    // 死亡かHP0以下で撃破とする。高冠戦の最終撃破演出が保留中なら確定を待つ。
    bool IsBossDefeated() const;

    /// ボス、障壁、報酬の実体を個別に有効化する。
    void SetBossActive(bool active);

    // Target IDの次から、設定された数だけ壁の状態を送る。
    void SetBarriersActive(bool active);

    // 壁の連番直後のIDに報酬の有効状態を送る。
    void SetRewardActive(bool active);

    /// ボス出現のカメラ演出を開始する。
    bool StartBossPresentation();

    // 通常の遅延起動に使う秒数。設定の下限は0.05秒。
    float GetBossRevealDelay() const;

    // 隔離壁の個数。1〜16の範囲に収める。
    std::int32_t GetBarrierCount() const;

    // 配置のモード2が高冠戦に対応する。
    bool IsHighCrownMode() const;

    // 接続先の敵タイプがMagmaSlimeの場合にtrueを返す。
    bool IsMagmaMode() const;

    State state_ = State::kWaiting;
    // 封鎖開始からの経過秒数。段階が変わるたびに0へ戻す。
    float stateTimer_ = 0.0f;
    bool initializedForPlay_ = false;
    bool introTimelineActive_ = false;
};
