#pragma once
#include <cstdint>

#include "BaseGimmick.h"
#include "game/system/cinematic/GateEntryPresentation.h"

class BaseScene;

/// <summary>ステージ選択ノードと入場ゲートの表示・遷移を管理します。</summary>
// 画面の選択状態はGameSelectSceneなどから受け取り、接触演出はGateEntryPresentation、遷移要求はSceneManagerに委ねる。
// 主な入口：Initialize / Update / OnCollision
class GimmickStageGate : public BaseGimmick {
  public:
    // 表示名とは独立した、既存シーンの保存IDと生成モデルです。
    static constexpr const char* kTypeId = "StageGate";
    static constexpr const char* kModelPath = "Gimmicks/portal_surface";

    ~GimmickStageGate() override = default;

    // ポータル材質、プレイヤー専用トリガーと移動先を指定する編集項目を初期化する。
    void Initialize(engine::Object3DCommon* common, const std::string& modelName) override;

    // 目標開放度に滑らかに追従させ、開放の閾値をまたいだ瞬間だけ発光を強めます。
    void Update(float deltaTime) override;

    // プレイヤーが接触し、進入演出を開始できた場合にtrueを返す。展示ノードは接触で作動しない。
    bool OnCollision(engine::Object3D* other) override;

    // 複製先の描画資源を初期化した後で、ゲート設定をコピーします。
    std::unique_ptr<engine::Object3D> Clone() const override;

    // エディターで設定された行き先情報とノードの動作モードを取得します。
    std::int32_t GetStageIndex() const;

    // 配置のモード番号。未設定の場合はステージ選択の展示ノードを表す0。
    std::int32_t GetGateMode() const;

    // 遷移先の登録名。空または未設定ならSELECTを使う。
    const std::string& GetTargetSceneName() const;

    // ステージ選択の展示ノードならtrue。接触では遷移しない。
    bool IsStageSelectNodeMode() const;

    /// <summary>選択・解放・クリア状態を設定し、次の更新でゲート表示へ反映します。</summary>
    /// <param name="selected">現在選択中かどうか。</param>
    /// <param name="unlocked">入場可能かどうか。</param>
    /// <param name="cleared">クリア済みかどうか。</param>
    /// <param name="unlocking">解放演出中かどうか。</param>
    void SetGateState(bool selected, bool unlocked, bool cleared, bool unlocking = false);

    // 発光に使う目標出現率を0〜1で指定する。現在値はUpdateで追従する。
    void SetGateActivation(float activation);

    // 接触で遷移できるか切り替える。無効化すると進行中の演出を解除する。
    void SetTransitionEnabled(bool enabled);

    // ゲートに入った際の発光反応を開始します。
    void TriggerEntryReaction();

    // UI上の解放状態を返す。接触による遷移の可否はCanTriggerTransitionで別途判定する。
    bool IsUnlocked() const { return isUnlocked_; }

    // このゲートの進入演出がアクティブならtrue。
    bool IsEntryPresentationActive() const { return entryPresentation_.IsActive(); }
    // 進入演出が持つ現在の進行率を返す。
    float GetEntryPresentationProgress() const { return entryPresentation_.GetProgress(); }

    // 有効な進入演出を一つ進めたらtrueを返す。呼び出し側はそのフレームの通常更新を止める。
    static bool UpdateSceneEntry(BaseScene& scene, float deltaTime);
    // Player とカメラが生存している Finalize の先頭で、借用参照をすべて外します。
    static void ResetSceneEntries(BaseScene& scene);

  private:
    // 初回の縮尺を保存して初期化済みにする。Updateでは現在の縮尺も記録する。
    void CaptureBaseScale();

    // 描画側が水系の材質パラメーターを持つ場合に、波と発光の値を反映する。
    void UpdatePortalMaterial();

    // 動作モード、開始設定、進行中や要求済みのフラグから接触時の遷移可否を決める。
    bool CanTriggerTransition() const;

    // Editor で演出中に行き先が変更されても、存在しないシーンへの要求で操作不能にしません。
    void TriggerTransition();

    // 解放状態、解放演出、クリア、選択の優先順で色と不透明度を返す。
    engine::Vector4 GetTargetColor() const;

    engine::Vector3 baseScale_ = {1.0f, 1.0f, 1.0f};
    // ゲートの明滅に使う経過秒数。
    float pulseTimer_ = 0.0f;

    float activation_ = 1.0f; // 現在のゲート出現率。
    float targetActivation_ = 1.0f; // 解錠・演出から要求された出現率。
    // 入場や開放直後の一時的な発光の強さ。毎フレーム0へ減衰する。
    float activationBurst_ = 0.0f;
    float lastAppliedPulse_ = 1.0f;

    GateEntryPresentation entryPresentation_;
    bool transitionQueued_ = false; // 重複したシーン遷移要求を送らないための予約状態。

    bool hasBaseScale_ = false;
    bool hasTriggeredTransition_ = false; // この入口からシーン遷移を一度要求済みか。

    bool isSelected_ = false;
    bool isUnlocked_ = true;
    bool isCleared_ = false;
    bool isUnlocking_ = false;
};
