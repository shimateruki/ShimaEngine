#pragma once

#include "Player.h"
#include <numbers>
#include "Model.h"
#include "game/actor/combat/CombatGroundQuery.h"
#include "engine/utility/math/AnimationInterpolation.h"

#include <algorithm>
#include <cmath>

// 王冠取得時のプレイヤー勝利モーションを管理します。
// 取得時の接地と復元を担当し、王冠装着後の姿勢は相対トラックへ引き継ぎます。
// 関連：ゴール演出の進行と基準姿勢を受け取り、Playerの見た目と動作を更新する。
// 主な入口：Start / Update / Reset
class GoalClearPlayerAnimator {
  public:
    // 王冠取得から勝利着地までの演出時刻と姿勢調整値。Time 系は演出開始からの秒。
    struct Tuning {
        float crownLandTime = 2.28f;
        float anticipationStartTime = 2.48f;
        float jumpStartTime = 2.72f;
        float apexTime = 3.08f;
        float resultUiTime = 3.10f;
        float victoryLandTime = 3.46f;
        float readyTime = 5.55f;

        float settleDuration = 0.45f;


    };

    // プレイヤーの操作権と開始姿勢を保存し、王冠または指定方向へ向けて勝利演出を開始します。
    // player がない場合は開始せず false を返します。
    bool Start(Player* player, const engine::Vector3& crownPosition, const engine::Vector3& preferredForward = engine::Vector3{}) {
        player_ = player;
        // プレイヤーが渡されていない場合は、演出を非アクティブにして開始に失敗したことを返す。
        if (!player_) {
            isActive_ = false;
            return false;
        }

        // 元の操作権・姿勢を保存し、ムービー中はこの基準から毎フレーム姿勢を計算します。
        savedControlActive_ = player_->IsControlActive();
        restorePosition_ = player_->GetTranslate();
        restoreRotation_ = player_->GetRotation();
        basePosition_ = restorePosition_;
        baseScale_ = player_->GetScale();
        baseRotation_ = player_->GetRotation();
        // 通常の姿勢安定化後にも空中で取った高さは残るため、足元だけを地面へなじませる。
        modelBottom_ = player_->GetModel() ? player_->GetModel()->GetLocalAabbMin().y : -0.5f;
        const auto world = player_->GetWorldPosition();
        const float footY = world.y + modelBottom_ * baseScale_.y;
        auto grounded = world;
        grounded.y = combatGroundQuery::FindGroundPoint(world, player_, {1.0f, 33.0f}, footY).y - modelBottom_ * baseScale_.y;
        // 親がある場合は、求めた接地位置を Player の SetTranslate() が使う親ローカル座標に戻す。
        if (player_->GetParent()) grounded = engine::Math::Transform(grounded, engine::Math::Inverse(player_->GetParent()->GetWorldMatrix()));
        basePosition_ = grounded;

        player_->SetIsControlActive(false);
        player_->SetVelocity({0.0f, 0.0f, 0.0f});

        // 指定された前方がない場合は王冠へ向きます。Visual Yaw の差を含めてモデル正面を合わせます。
        const engine::Vector3 crownForward =
            NormalizePlanarDirection({crownPosition.x - basePosition_.x, 0.0f, crownPosition.z - basePosition_.z},
                                     DirectionFromYaw(baseRotation_.y - player_->GetVisualYawOffset()));
        const engine::Vector3 desiredForward = NormalizePlanarDirection(preferredForward, crownForward);
        const float faceYaw = std::atan2(desiredForward.x, desiredForward.z);
        baseRotation_.y = NormalizeYaw(faceYaw + player_->GetVisualYawOffset());
        moveForward_ = desiredForward;
        moveRight_ = RightFromForward(moveForward_);

        player_->SetRotation(baseRotation_);
        isActive_ = true;
        return true;
    }

    // 保持している演出状態を初期化します。プレイヤーの姿勢は復元しません。
    void Reset() {
        player_ = nullptr;
        isActive_ = false;
        savedControlActive_ = true;
        basePosition_ = {};
        baseScale_ = {1.0f, 1.0f, 1.0f};
        baseRotation_ = {};
        moveForward_ = {0.0f, 0.0f, 1.0f};
        moveRight_ = {1.0f, 0.0f, 0.0f};
    }

    // 中断時に保存済みの開始姿勢をモデルへ戻し、ワールド行列まで更新します。
    void RestoreInitialPose() {
        // 演出対象がなく開始姿勢が有効でない場合は、復元できる物体がない。
        if (!player_ || !isActive_) {
            return;
        }

        // 演出を中断したときは開始姿勢と行列を戻し、次のScene更新へ古い変形を残しません。
        player_->SetTranslate(restorePosition_);
        player_->SetScale(baseScale_);
        player_->SetRotation(restoreRotation_);
        player_->SetVelocity({0.0f, 0.0f, 0.0f});
        player_->UpdateLocalMatrix();
        player_->UpdateWorldMatrix();
    }

    // 演出開始前の操作可能状態を戻します。
    void RestoreControl() {
        // 演出対象が残っている場合だけ、開始時に退避した操作許可を復元する。
        if (player_) {
            player_->SetIsControlActive(savedControlActive_);
        }
    }

    // 演出開始からの絶対時刻で姿勢を評価します。シークしても前フレームには依存しません。
    void Update(float time) {
        // 相対トラックの開始前にだけ、取得高度から接地位置へ滑らかに下ろす。
        if (!player_ || !isActive_ || time >= tuning_.crownLandTime) {
            return;
        }

        const float settle = animationInterpolation::ApplyEasing(
            time / (std::max)(0.05f, tuning_.settleDuration), animationInterpolation::EasingType::kSmootherStep);
        const float breathe = std::sin(time * 3.8f) * 0.5f + 0.5f;
        const engine::Vector3 scale = {baseScale_.x * (1.0f + breathe * 0.012f),
            baseScale_.y * (1.0f - breathe * 0.009f), baseScale_.z * (1.0f + breathe * 0.012f)};
        auto position = basePosition_;
        position.y += (restorePosition_.y - basePosition_.y) * (1.0f - settle);
        position.y += modelBottom_ * (baseScale_.y - scale.y);
        player_->SetTranslate(position);
        player_->SetRotation(baseRotation_);
        player_->SetScale(scale);
        player_->UpdateLocalMatrix();
        player_->UpdateWorldMatrix();
    }

    // 次の姿勢評価に使うタイミングと形状の設定を差し替える。
    void SetTuning(const Tuning& tuning) { tuning_ = tuning; }

    const Tuning& GetTuning() const { return tuning_; }

    // エディターなどから設定をその場で更新できるよう、変更可能な参照を返す。
    Tuning& EditTuning() { return tuning_; }

    bool IsActive() const { return isActive_; }

    bool WasControlActive() const { return savedControlActive_; }

    const engine::Vector3& GetBasePosition() const { return basePosition_; }

    const engine::Vector3& GetBaseScale() const { return baseScale_; }

    const engine::Vector3& GetBaseRotation() const { return baseRotation_; }

    // 直近に反映した姿勢の位置を親ローカル座標で返す。


    const engine::Vector3& GetMoveForward() const { return moveForward_; }

    const engine::Vector3& GetMoveRight() const { return moveRight_; }

  private:
    static constexpr float kPi = std::numbers::pi_v<float>;
    static constexpr float kTwoPi = kPi * 2.0f;

    // ヨーをラジアンの −π〜π に折り返し、同じ向きの角度を揃える。
    static float NormalizeYaw(float yaw) {
        while (yaw > kPi)
            yaw -= kTwoPi;
        while (yaw < -kPi)
            yaw += kTwoPi;
        return yaw;
    }

    // ヨーを水平単位ベクトルへ変換する。角度ゼロは +Z を正面にする。
    static engine::Vector3 DirectionFromYaw(float yaw) { return {std::sin(yaw), 0.0f, std::cos(yaw)}; }

    // 高さ成分を捨て、XZ 平面の方向を単位ベクトルにする。
    static engine::Vector3 NormalizePlanarDirection(const engine::Vector3& direction, const engine::Vector3& fallback) {
        engine::Vector3 result = {direction.x, 0.0f, direction.z};
        const float length = std::sqrt(result.x * result.x + result.z * result.z);
        // 水平成分がほぼゼロなら正規化できないので、呼出側で指定した予備方向を使う。
        if (length <= 0.0001f) {
            return fallback;
        }
        return {result.x / length, 0.0f, result.z / length};
    }

    // 水平な前方を 90 度回し、ジャンプの横弧を作る右方向を求める。
    static engine::Vector3 RightFromForward(const engine::Vector3& forward) { return {forward.z, 0.0f, -forward.x}; }

  private:
    Player* player_ = nullptr;
    Tuning tuning_{};

    bool isActive_ = false;
    bool savedControlActive_ = true;

    engine::Vector3 restorePosition_{}, restoreRotation_{};
    float modelBottom_ = -0.5f;
    engine::Vector3 basePosition_{};
    engine::Vector3 baseScale_ = {1.0f, 1.0f, 1.0f};
    engine::Vector3 baseRotation_{};


    engine::Vector3 moveForward_ = {0.0f, 0.0f, 1.0f};
    engine::Vector3 moveRight_ = {1.0f, 0.0f, 0.0f};
};
