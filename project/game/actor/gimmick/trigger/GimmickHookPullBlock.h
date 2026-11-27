#pragma once
#include "BaseGimmick.h"

/// <summary>フックに引かれた位置へ移動するブロックです。</summary>
// フック側から引き寄せ先を受け取り、移動をUpdate、地形による押し戻しをOnCollisionで行う。
// 主な入口はInitialize、StartHookPull、Update、OnCollision。
class GimmickHookPullBlock : public BaseGimmick {
  public:
    // 表示名とは独立した、既存シーンの保存IDと生成モデルです。
    static constexpr const char* kTypeId = "HookPullBlock";
    static constexpr const char* kModelPath = "Stages/block";

    virtual ~GimmickHookPullBlock() = default;

    // 共通ギミックとして登録した後、フック対象として識別できる型を設定する。
    void Initialize(engine::Object3DCommon* common, const std::string& modelName) override;

    // 引き寄せ時間中は目標へ移動し、その後は速度を減らしながら進める。
    void Update(float deltaTime) override;

    // 地面属性以外との接触では引き寄せ中の位置補正を行わない。
    bool OnCollision(engine::Object3D* other) override;

    // 新しい実体を初期化してから配置とフック設定を引き継ぐ。
    std::unique_ptr<engine::Object3D> Clone() const override;

    /// <summary>フックを持つキャラクターへ向けてブロックの移動を開始します。</summary>
    /// <param name="hookOwnerPos">引き寄せ先となるフック使用者のワールド座標。</param>
    void StartHookPull(const engine::Vector3& hookOwnerPos);

  private:
    // 移動時の速度。ワールド単位/秒。
    engine::Vector3 velocity_ = engine::Vector3::kZero;
    engine::Vector3 pullTarget_ = engine::Vector3::kZero;
    float pullTimer_ = 0.0f; // 目標へ引き寄せる誘導が続く残り秒数。
};
