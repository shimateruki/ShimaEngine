#pragma once
#include "Object3D.h"
#include "Easing.h"
#include <numbers>
#include "engine/utility/math/Math.h"
#include <algorithm>
#include <cmath>

// 倒れ姿勢 → 起き上がり → 画面右への退場を一つの状態機械で管理する。
// 関連：GameOverSceneが進行を渡し、倒れ・震えなどの表示姿勢を計算する。
// 主な入口：Reset / Update
class GameOverSlimeAnimator {
  public:
    // 倒れたスライムの基準姿勢と床の高さを記録し、演出を最初から始めます。
    void Reset(engine::Object3D* slime) {
        // 現在のモデルAABBから床の高さを取って、形状の違うスライムも接地させる。
        timer_ = exitTimer_ = 0.0f;
        phase_ = Phase::kDowned;
        hasBaseTransform_ = false;
        // 対象のスライムが無い場合は、保持している演出状態だけを初期化する。
        if (!slime)
            return;
        basePosition_ = slime->GetTranslate();
        baseScale_ = slime->GetScale();
        baseRotation_ = slime->GetRotation();
        slime->UpdateWorldMatrix(false);
        floorY_ = slime->GetModelWorldAabb().min.y;
        currentPosition_ = basePosition_;
        currentScale_ = baseScale_;
        currentRotation_ = baseRotation_;
        hasBaseTransform_ = true;
        Update(slime, 0.0f);
    }

    // 倒れ姿勢または退場モーションを進め、床に接する姿勢を反映します。
    void Update(engine::Object3D* slime, float deltaTime) {
        // スライムまたは基準姿勢を取得できない場合は、ダウン姿勢を適用できない。
        if (!slime || !hasBaseTransform_)
            return;
        const float frameDeltaTime = std::isfinite(deltaTime) ? (std::max)(deltaTime, 0.0f) : 0.0f;
        timer_ += frameDeltaTime;
        // ダウン済みの段階でなければ、ダウン姿勢の更新を行わない。
        if (phase_ != Phase::kDowned) {
            UpdateExitRight(slime, frameDeltaTime);
            return;
        }
        const float breathe = std::sin(timer_ * 1.7f);
        currentPosition_ = basePosition_;

        // 顔(-Z)を床へ伏せ、床の法線になるローカルZを潰す。
        // 上下反転だけでは開いた目が見えるため、倒れ姿勢はうつ伏せにする。
        currentScale_ = {baseScale_.x * (1.32f - breathe * 0.003f), baseScale_.y * 1.08f,
                         baseScale_.z * (0.22f + breathe * 0.003f)};
        currentRotation_ = {-kPi * 0.5f, baseRotation_.y, 0.0f};
        ApplyGroundedPose(slime, 0.0f);
    }

    // 倒れ状態から起き上がり、水平の指定方向へ退場する動きを開始します。
    // 方向が無効なら +X、距離が無効なら 10 を採用します。
    void StartExitRight(engine::Object3D* slime, const engine::Vector3& direction, float distance) {
        // ダウン済みの場合だけ、起き上がりから退場までの演出に切り替える。
        if (phase_ != Phase::kDowned)
            return;
        // 対象か基準スケールが無い場合は、退場を再生できないため完了扱いにする。
        if (!slime || !hasBaseTransform_) {
            phase_ = Phase::kExitFinished;
            return;
        }
        exitTimer_ = 0.0f;
        exitStartScale_ = currentScale_;
        exitStartRotation_ = currentRotation_;
        exitDirection_ = {direction.x, 0.0f, direction.z};
        const float length = std::sqrt(direction.x * direction.x + direction.z * direction.z);
        // 水平方向の長さが有限で0.001より大きい場合は、長さを1にして退場方向に使う。
        if (std::isfinite(length) && length > 0.001f) {
            exitDirection_.x /= length;
            exitDirection_.z /= length;
        // 方向を正規化できない場合は、ワールドのX軸正方向へ退場させる。
        } else
            exitDirection_ = {1.0f, 0.0f, 0.0f};
        // 有限の退場距離が負なら0に抑え、開始位置から逆向きへ進めない。
        exitDistance_ = std::isfinite(distance) ? (std::max)(distance, 0.0f) : 10.0f;
        phase_ = Phase::kExitRight;
    }

    bool IsExiting() const { return phase_ != Phase::kDowned; }

    bool IsExitAnimationFinished() const { return phase_ == Phase::kExitFinished; }

    float GetDizzyAlpha() const {
        return phase_ == Phase::kDowned ? 0.70f : 0.70f * (1.0f - Clamp01(exitTimer_ / 0.22f));
    }

    engine::Vector3 GetDizzyAnchorWorld() const { return dizzyAnchor_; }

    float GetTimer() const { return timer_; }

    static constexpr float GetImpactTime() { return 0.0f; }

    bool HasBaseTransform() const { return hasBaseTransform_; }

  private:
    enum class Phase { kDowned, kExitRight, kExitFinished };

    static constexpr float kPi = std::numbers::pi_v<float>;

    static float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

    static float Smooth(float v) {
        v = Clamp01(v);
        return Easing::SmoothStep(v);
    }

    // 現在の変形を反映し、モデル境界の最下点を床面と跳ね高さに合わせます。
    void ApplyGroundedPose(engine::Object3D* slime, float hop) {
        slime->SetScale(currentScale_);
        slime->SetRotation(currentRotation_);
        slime->SetTranslate(currentPosition_);
        slime->UpdateWorldMatrix(false);

        // 回転中もモデル境界の最下点を床へ合わせ、中心点基準の浮き沈みを防ぐ。
        currentPosition_.y += floorY_ + hop - slime->GetModelWorldAabb().min.y;
        slime->SetTranslate(currentPosition_);
        slime->SetEnableLighting(false);
        slime->SetBlendMode(BlendMode::kNormal);
        slime->SetColor({0.88f, 0.97f, 1.0f, 1.0f});
        slime->SetEmissive(1.0f);
        slime->UpdateWorldMatrix(false);
        const engine::Aabb bounds = slime->GetModelWorldAabb();
        dizzyAnchor_ = {(bounds.min.x + bounds.max.x) * 0.5f, bounds.max.y + 0.14f,
                        (bounds.min.z + bounds.max.z) * 0.5f};
        slime->RefreshRenderCameraData();
    }

    // 起き上がり、進行方向への旋回、加速移動の順に退場姿勢を更新します。
    void UpdateExitRight(engine::Object3D* slime, float frameDeltaTime) {
        // 退場が完了済みなら、それ以上姿勢や位置を書き換えない。
        if (phase_ == Phase::kExitFinished)
            return;
        exitTimer_ += frameDeltaTime;
        constexpr float kStandDurationSeconds = 0.80f;
        constexpr float kTurnDurationSeconds = 0.22f;
        constexpr float kRunDurationSeconds = 1.35f;
        float hop = 0.0f;
        currentPosition_ = basePosition_;
        // 退場開始から0.8秒までは、ダウン姿勢から起き上がり姿勢へ補間する。
        if (exitTimer_ < kStandDurationSeconds) {
            const float u = Smooth(exitTimer_ / kStandDurationSeconds);
            currentRotation_ = engine::Math::Lerp(exitStartRotation_, baseRotation_, u);
            currentScale_ = engine::Math::Lerp(exitStartScale_, baseScale_, u);
            hop = std::sin(u * kPi) * 0.08f;
        // 起き上がった後に向きを変え、0.22秒の振り向きが終わってから移動を進める。
        } else {
            const float turn = Smooth((exitTimer_ - kStandDurationSeconds) / kTurnDurationSeconds);

            // スライムモデルの正面は-Z。通常プレイヤーと同じ180度補正で進行方向を向く。
            const float yaw = std::atan2(exitDirection_.x, exitDirection_.z) + kPi;
            const float yawDelta = std::atan2(std::sin(yaw - baseRotation_.y), std::cos(yaw - baseRotation_.y));
            currentRotation_ = {baseRotation_.x, baseRotation_.y + yawDelta * turn, baseRotation_.z};
            currentScale_ = baseScale_;
            const float u = Clamp01((exitTimer_ - kStandDurationSeconds - kTurnDurationSeconds) / kRunDurationSeconds);

            // 加速して画面外まで進む。起き上がり完了前に横移動を始めない。
            const float move = u * u;
            currentPosition_ = basePosition_ + exitDirection_ * (exitDistance_ * move);
            const float bounce = std::sin(u * kPi * 4.0f);
            hop = std::abs(bounce) * 0.10f;
            currentScale_.x *= 1.0f - std::abs(bounce) * 0.035f;
            currentScale_.y *= 1.0f + std::abs(bounce) * 0.06f;
        }
        ApplyGroundedPose(slime, hop);
        // 起き上がり、振り向き、移動の各時間を使い切ったら退場完了にする。
        if (exitTimer_ >= kStandDurationSeconds + kTurnDurationSeconds + kRunDurationSeconds)
            phase_ = Phase::kExitFinished;
    }

    engine::Vector3 basePosition_{}, baseScale_{1, 1, 1}, baseRotation_{};
    engine::Vector3 currentPosition_{}, currentScale_{1, 1, 1}, currentRotation_{}, dizzyAnchor_{};
    engine::Vector3 exitStartScale_{1, 1, 1}, exitStartRotation_{}, exitDirection_{1, 0, 0};
    float floorY_ = 0.0f, exitDistance_ = 10.0f, timer_ = 0.0f, exitTimer_ = 0.0f;
    bool hasBaseTransform_ = false;
    Phase phase_ = Phase::kDowned;
};
