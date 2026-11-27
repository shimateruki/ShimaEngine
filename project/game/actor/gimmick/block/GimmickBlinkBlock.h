#pragma once
#include <cstdint>
#include "BaseGimmick.h"
#include "game/system/rules/EventManager.h"

/// <summary>プレイヤーのジャンプに合わせて出現状態が切り替わるブロックです。</summary>
// 通常モデルは出現面と衝突を切り替え、反転板モデルは別々に回転コマと着地面を更新する。ジャンプはEventManagerから受け取る。
// 主な入口：Initialize / Update
class GimmickBlinkBlock : public BaseGimmick {
  public:
    // 表示名とは独立した、既存シーンの保存IDと生成モデルです。
    static constexpr const char* kTypeId = "BlinkBlock";
    static constexpr const char* kModelPath = "Stages/block";

    // ジャンプ通知の購読を解除する。
    ~GimmickBlinkBlock() override;

    // ジャンプ通知を購読し、初期色と反転速度の編集項目を登録する。
    void Initialize(engine::Object3DCommon* common, const std::string& modelName) override;

    // 通常モデルの表示か、反転板の表示・床判定を更新する。経過時間の単位は秒。
    void Update(float deltaTime) override;

    // 同じモデルで新しい実体を初期化してから、配置と設定値を複製する。
    std::unique_ptr<engine::Object3D> Clone() const override;

  private:
    // ジャンプ通知を受けて、ブロックの表示と衝突を切り替えます。
    void OnPlayerJump(const PlayerJumpEvent& event);

    // 通常モデルを有効時は不透明な床、無効時は衝突のない半透明表示にする。
    void UpdateAppearance();
    // 反転板の初期面を取得し、回転の途中状態を消す。両端モデルの判定は最初にだけ準備する。
    void ResetFlipPair();
    // プレイ状態や初期面の変更を反映し、再生中は目標角度への補間を進める。
    void UpdateFlipPair(float deltaTime);
    // 現在角度の表示モデルと、目標面の着地判定をそれぞれ適用する。
    void ApplyFlipPairAppearance();
    // モデル名が反転板の連番系統ならtrue。
    bool IsFlipPairModel() const;


    bool isActive_ = true;
    std::int32_t colorType_ = 0; // 0: Blue, 1: Red
    bool flipPairInitialized_ = false;
    bool flipPairWasPlaying_ = false;
    std::int32_t flipPairInitialSide_ = 0;
    // 現在描画する板の角度。ラジアン。
    float flipAngle_ = 0.0f;
    float flipFromAngle_ = 0.0f;
    // 目標面の角度。0が青、πが赤。
    float flipTargetAngle_ = 0.0f;
    // 最後に反転を開始してからの秒数。
    float flipElapsed_ = 0.0f;
    float flipDuration_ = 0.48f;
    // 描画に使っているコマ番号。-1は初回の差し替え前。
    std::int32_t flipFrame_ = -1;
    // 左手座標系へ変換済みの端点モデルから求めた、青・赤それぞれの床判定。
    engine::ColliderConfig flipPairColliders_[2]{};
    EventManager::SubscriptionId jumpSubscription_ = EventManager::kInvalidSubscriptionId;
};
