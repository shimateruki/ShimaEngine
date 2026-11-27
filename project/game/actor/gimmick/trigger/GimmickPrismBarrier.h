#pragma once

#include "BaseGimmick.h"

// 遭遇イベントに応じて展開・解除する発光障壁。展開中は高さによって衝突を有効にする。
// 遭遇管理からBaseScene経由の起動・解除を受け、壁の変形、材質、衝突を更新する。
// 主な入口：Initialize / Update / OnTrigger / OnSwitchEvent
class GimmickPrismBarrier : public BaseGimmick {
  public:
    // 表示名とは独立した、既存シーンの保存IDと生成モデルです。
    static constexpr const char* kTypeId = "PrismBarrier";
    static constexpr const char* kModelPath = "Gimmicks/portal_surface";

    // 障壁の材質と薄いOBBを初期化し、開閉と発光の編集項目を登録する。
    void Initialize(engine::Object3DCommon* common, const std::string& modelName) override;

    // 再生停止時は編集配置を復元し、実行時のみ開閉補間を進めます。
    void Update(float deltaTime) override;

    // 単発イベントは壁を出すスイッチのオンとして扱います。
    void OnTrigger() override;

    // 無効化の要求時に衝突を先に消し、見た目の縮退とは切り離します。
    void OnSwitchEvent(bool active) override;

    // 初期化後に編集設定を移し、壁の材質と判定を複製先で作り直します。
    std::unique_ptr<engine::Object3D> Clone() const override;

  private:
    // 編集位置・色・衝突を退避する。forceがtrueなら、退避済みでも編集側の値を取り直す。
    void CaptureAuthoredState(bool force = false);

    // 保存した位置・大きさ・色へ戻し、編集用に障壁の表示と衝突を消す。
    void ResetForEditor();

    // 底辺を固定して上へ伸ばす。解除は衝突を先に外し、縮む表示はしきい値まで残す。
    void ApplyRuntimeVisual();

    // 保存している属性とマスクを復元するか、両方を0にする。
    void SetCollisionEnabled(bool enabled);

    // 開閉にかかる秒数を返す。設定は0.05〜3.0秒、設定なしなら0.42秒。
    float GetTransitionDuration() const;

    engine::Vector3 authoredPosition_{}; // Editorで指定した位置。Play終了時の復元に使う。
    engine::Vector3 authoredScale_{1.0f, 1.0f, 1.0f}; // Editorで指定した倍率。表示の開閉変形と分離して退避する。
    engine::Vector4 authoredColor_{0.30f, 0.86f, 1.0f, 0.84f};
    uint32_t authoredCollisionAttribute_ = 0; // Play開始前の衝突属性。
    uint32_t authoredCollisionMask_ = 0; // Play開始前の衝突マスク。
    float activation_ = 0.0f; // 現在の出現率。0が非表示、1が出現完了。
    float targetActivation_ = 0.0f; // 外部通知から要求された出現率。
    float visualTime_ = 0.0f;
    bool authoredStateCaptured_ = false; // 復元用の配置・衝突情報を退避済みか。
    bool initializedForPlay_ = false; // 現在のPlayセッションで初期化済みか。
    bool activationRequestReceived_ = false;
};
