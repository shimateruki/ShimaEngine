#pragma once

#include "BaseGimmick.h"
#include "LightManager.h"

class Player;

// 配置した範囲への進入・退出を、イベントやチェックポイントなどのゲーム処理へ変換します。
// BaseGimmickの衝突通知からプレイヤーの出入りを検知し、シーン・プレイヤー・音声・照明の各処理へ渡す。
// 主な入口：Initialize / Update / OnCollision / OnTrigger
class GimmickGameplayVolume : public BaseGimmick {
  public:
    // 表示名とは独立した、既存シーンの保存IDと生成モデルです。
    static constexpr const char* kTypeId = "GameplayVolume";
    static constexpr const char* kModelPath = "Primitives/cube";

    // プレイヤーだけを検知するOBBと、用途別の編集項目を初期化する。
    void Initialize(engine::Object3DCommon* common, const std::string& modelName) override;

    /// 進入・退出と再起動待ちを更新する。
    void Update(float deltaTime) override;

    /// 編集中の範囲表示を描画する。
    void Draw(ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource) override;

    // 有効な範囲とプレイヤーの接触を記録する。常にfalseを返して押し戻しを行わない。
    bool OnCollision(engine::Object3D* other) override;

    // 外部イベントは範囲検知を有効にする。効果の実行は設定された進入または退出で行う。
    void OnTrigger() override;

    /// スイッチに応じて範囲効果を有効・無効にする。
    void OnSwitchEvent(bool active) override;

    // 用途と配置を複製する。新しい範囲の検知履歴は初期化状態から始まる。
    std::unique_ptr<engine::Object3D> Clone() const override;

  private:
    enum class Mode {
        kEvent = 0,
        kCheckpoint,
        kFallDeath,
        kFeedbackCue,
        kBgm,
        kEnvironment,
    };

    /// Play 開始時の占有状態と環境設定を準備する。
    void InitializeForPlay();

    // 編集時の範囲表示に戻し、占有と発動の履歴を初期化する。
    void ResetForEditor();

    /// プレイヤーの範囲内への進入を処理する。
    void HandleEnter(Player* player);

    /// プレイヤーの範囲外への退出を処理する。
    void HandleExit(Player* player);

    /// 種別に応じたイベント、チェックポイント、音、環境を適用する。
    void Activate(Player* player);

    // イベントのOFF、BGM停止、保存した環境への復帰を、用途に応じて行う。
    void Deactivate();

    // 有効で待ち時間がなく、一回限りなら未発動の場合にtrueを返す。
    bool CanActivate() const;

    /// Scene JSON の設定から範囲効果の種別を返す。
    Mode GetMode() const;

    bool initializedForPlay_ = false;
    bool runtimeEnabled_ = true;
    bool occupied_ = false; // 前回更新時にプレイヤーが範囲内にいたか。出入りの差分判定に使う。
    bool collisionObserved_ = false; // 現在の更新で処理する接触通知の有無。
    bool triggered_ = false; // 一回限りの設定で再発火を抑えるための印。
    // 繰り返し発動できるまでの残り秒数。
    float rearmTimer_ = 0.0f;

    // 最後に接触したプレイヤーへの非所有参照。退出や範囲の無効化で解除する。
    Player* occupant_ = nullptr;
    engine::LightManager::EnvironmentProfileState previousEnvironment_{};
    bool hasPreviousEnvironment_ = false; // 退場時に戻す環境設定を退避してあるか。
};
