#pragma once
#include "BaseGimmick.h"

// 乗ると揺れ、落下・非表示を経て再出現する足場です。時間は param_ があればその設定を使います。
// BaseGimmickから上面の接触通知を受け、揺れ・落下・消滅・再出現を状態ごとに進める。
// 主な入口：Initialize / Update / OnCollision
class GimmickChikuwaBlock : public BaseGimmick {
  public:
    // 表示名とは独立した、既存シーンの保存IDと生成モデルです。
    static constexpr const char* kTypeId = "ChikuwaBlock";
    static constexpr const char* kModelPath = "Stages/block";

    enum class State {
        kIdle,    // 待機中（足場として存在）
        kShaking, // プレイヤーが乗って震えている
        kFalling, // 落下中
        kHidden   // 消滅中（リスポーン待ち）
    };

    // 踏める床としての衝突と、各状態に使う時間と重力を設定する。
    void Initialize(engine::Object3DCommon* common, const std::string& modelName) override;

    // 揺れと落下を進め、消滅後の待ち時間が終わったら配置を戻す。
    void Update(float deltaTime) override;

    // 待機中のみプレイヤーの搭乗を判定
    bool OnCollision(engine::Object3D* other) override;

    // 同じモデルで新しい実体を初期化してから、配置と設定値を複製する。
    std::unique_ptr<engine::Object3D> Clone() const override;

  private:
    // 指定の段階へ移るときに経過時間を初期化し、可視状態と衝突も切り替える。
    void ChangeState(State newState);

    State state_ = State::kIdle;
    // 現在の段階の経過秒数。
    float timer_ = 0.0f;
    engine::Vector3 startPos_;
    // 落下中の鉛直速度。ワールド単位/秒。
    float velocityY_ = 0.0f;

    // パラメーターがなければ、揺れは1秒を使う。
    float GetShakeDuration() const { return GetParameters().has_value() ? GetParameters()->shakeDuration : 1.0f; }

    // 落下を続ける秒数。パラメーターがなければ2秒。
    float GetFallDuration() const { return GetParameters().has_value() ? GetParameters()->fallDuration : 2.0f; }

    // 消滅から再出現までの秒数。パラメーターがなければ3秒。
    float GetRespawnDuration() const { return GetParameters().has_value() ? GetParameters()->interval : 3.0f; }

    // 下方向に与える加速度。パラメーターがなければ30ワールド単位/秒²。
    float GetGravity() const { return GetParameters().has_value() ? GetParameters()->gravity : 30.0f; }
};
