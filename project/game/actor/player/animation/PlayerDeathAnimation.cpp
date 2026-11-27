#define NOMINMAX
#include <cstdint>
#include "game/actor/player/animation/PlayerDeathAnimation.h"
#include "Easing.h"
#include <numbers>

#include "game/actor/player/Player.h"
#include "MeshEffectManager.h"
#include "ParticleSystem.h"
#include "GpuParticleManager.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kHitStopTime = 0.10f;
constexpr float kBurstTime = 0.38f;
constexpr float kFadeReadyTime = 0.88f;
constexpr float kFinishTime = 1.08f;
constexpr float kTwoPi = kPi * 2.0f;
constexpr const char* kDeathCoreFlashEffect = "Resources/json/effect/shared/effect_player_death_slime_core_flash.json";
constexpr const char* kDeathSplashRingEffect = "Resources/json/effect/shared/effect_player_death_slime_splash_ring.json";
constexpr const char* kDeathSplashArcEffect = "Resources/json/effect/shared/effect_player_death_slime_splash_arc.json";
constexpr const char* kDeathSplashStreakPreset = "shared/player_death_slime_splash_streaks";
constexpr const char* kDeathSoftMistPreset = "shared/player_death_slime_soft_mist";

// 死亡演出の補間率を0～1へ制限します。
float Clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}





engine::Vector3 MakeSafeScale(const engine::Vector3& scale) {
    // ほぼゼロの軸を既定値へ置き換え、死亡演出の拡縮が消失しないようにする。
    return {std::abs(scale.x) > 0.001f ? scale.x : 1.0f, std::abs(scale.y) > 0.001f ? scale.y : 1.0f,
            std::abs(scale.z) > 0.001f ? scale.z : 1.0f};
}
} // namespace

void PlayerDeathAnimation::Start(Player* player, bool finalDeath) {
    // 破裂前後の時計を0秒に戻し、最終残機かどうかを記録する。
    timer_ = 0.0f;
    puddleEmitTimer_ = 0.0f;
    active_ = true;
    finalDeath_ = finalDeath;
    burstEmitted_ = false;
    collisionStored_ = false;
    childVisible_.clear();

    // 対象がいなくても時計は初期化済み。姿勢や衝突の退避だけは行わない。
    if (!player) {
        return;
    }

    // スケールと回転・表示・衝突を復活用に退避し、演出中の物理接触を無効にします。
    baseScale_ = MakeSafeScale(player->GetScale());
    baseRotation_ = player->GetRotation();
    baseVisible_ = player->GetIsVisible();
    baseCollisionAttribute_ = player->GetCollisionAttribute();
    baseCollisionMask_ = player->GetCollisionMask();
    collisionStored_ = true;
    player->SetCollisionAttribute(0);
    player->SetCollisionMask(0);

    // 装飾の元の表示を記録し、破裂までは親と子を一緒に見せます。
    childVisible_.reserve(player->GetChildren().size());

    for (engine::Object3D* child : player->GetChildren()) {
        childVisible_.push_back(child ? child->GetIsVisible() : false);
    }

    player->SetIsVisible(true);

    for (engine::Object3D* child : player->GetChildren()) {
        // 有効な子モデルを破裂前の見た目に含めるため、開始時は表示する。
        if (child) {
            child->SetIsVisible(true);
        }
    }

    player->SetVelocity({0.0f, 0.0f, 0.0f});
}

void PlayerDeathAnimation::Update(Player* player, float deltaTime) {
    // 演出が有効でプレイヤーも存在する場合だけ、予兆・破裂・余韻の時計を進める。
    if (!active_ || !player) {
        return;
    }

    const float safeDeltaTime = std::max(0.0f, deltaTime);
    timer_ += safeDeltaTime;
    player->SetVelocity({0.0f, 0.0f, 0.0f});

    // 閾値までは身体を膨らませる準備段階だけを更新します。
    if (timer_ < kBurstTime) {
        ApplyPreBurstPose(player);
        return;
    }

    // 0.38秒に達した後、未発生なら一度だけ破裂を出して身体を隠す。
    if (!burstEmitted_) {
        burstEmitted_ = true;
        EmitBurst(player);
        HidePlayerBody(player);
    }

    // 破裂後の足元粒子は、暗転開始が可能になるまで間隔を空けて出す。
    puddleEmitTimer_ -= safeDeltaTime;

    // 暗転開始時刻の前で発生間隔を使い切ったときだけ、足元の余韻を足す。
    if (timer_ < kFadeReadyTime && puddleEmitTimer_ <= 0.0f) {
        EmitPuddle(player);
        puddleEmitTimer_ = finalDeath_ ? 0.065f : 0.085f;
    }
}

void PlayerDeathAnimation::RestoreVisual(Player* player) {
    // 演出の進行を停止し、プレイヤーに残った死亡時の表示と衝突を戻す。
    active_ = false;

    // 対象がなければ、保存してある姿勢や衝突を復元できない。
    if (!player) {
        return;
    }

    // 子は演出開始時と同じ順で表示を戻し、新しく増えた子は表示を既定とします。
    player->SetIsVisible(baseVisible_);
    const auto& children = player->GetChildren();

    for (size_t i = 0; i < children.size(); ++i) {
        engine::Object3D* child = children[i];

        // 子の参照を取得できない位置は、表示を復元せず飛ばす。
        if (!child) {
            continue;
        }

        const bool visible = i < childVisible_.size() ? childVisible_[i] : true;
        child->SetIsVisible(visible);
        child->SetColor({1.0f, 1.0f, 1.0f, 1.0f});
    }

    // 親の変形を消すため、保存しておいたサイズと回転を戻す。
    player->SetScale(baseScale_);
    player->SetRotation(baseRotation_);

    // 開始時に衝突属性・マスクを保存できた場合だけ、その値を復元する。
    if (collisionStored_) {
        player->SetCollisionAttribute(baseCollisionAttribute_);
        player->SetCollisionMask(baseCollisionMask_);
    }

    // 色は演出前の控えを持たないため、親と子を白へ戻す。
    player->SetColor({1.0f, 1.0f, 1.0f, 1.0f});
    player->UpdateLocalMatrix();
    player->UpdateWorldMatrix();
}

// 死亡演出が有効で0.88秒経過した場合に、呼び出し側へ暗転可能と伝えます。
bool PlayerDeathAnimation::IsReadyForFade() const {
    return active_ && timer_ >= kFadeReadyTime;
}

// 演出が有効で1.08秒経過した場合に、モーション完了を返します。
bool PlayerDeathAnimation::IsFinished() const {
    return active_ && timer_ >= kFinishTime;
}

void PlayerDeathAnimation::ApplyPreBurstPose(Player* player) {
    // ヒットストップから膨張へ移る間の形状を、共通の演出時計で決める。
    // プレイヤーが存在しなければ、予兆のスケール・回転・色を反映できない。
    if (!player) {
        return;
    }

    const float hitT = Clamp01(timer_ / kHitStopTime);
    const float chargeT = Clamp01((timer_ - kHitStopTime) / (kBurstTime - kHitStopTime));
    const float charge = Easing::SmoothStep01(chargeT);
    const float pulse = std::sin(chargeT * kPi);
    const float tinyShake = std::sin(timer_ * 84.0f) * (1.0f - chargeT) * 0.035f;

    engine::Vector3 scale = baseScale_;

    // 最初の0.10秒は、衝撃に合わせて身体を一度縮める。
    if (timer_ < kHitStopTime) {
        const float flashPulse = std::sin(hitT * kPi);
        scale.x *= 1.0f + flashPulse * 0.10f;
        scale.y *= 1.0f - flashPulse * 0.08f;
        scale.z *= 1.0f + flashPulse * 0.10f;
    // 衝撃の区間が終われば、破裂に向けて身体を膨らませる。
    } else {
        scale.x *= 1.0f + charge * 0.42f + pulse * 0.08f;
        scale.y *= 1.0f + charge * 0.10f - pulse * 0.05f;
        scale.z *= 1.0f + charge * 0.38f + pulse * 0.06f;
    }

    // 基準角へ短い震えを重ね、最後に表示用のワールド行列まで確定する。
    engine::Vector3 rotation = baseRotation_;
    rotation.z += tinyShake;
    rotation.x += std::sin(timer_ * 50.0f) * (1.0f - chargeT) * 0.025f;

    // 時間とともに弱くなる閃光を親と子へ同じ色で適用します。
    const float flash = std::max(0.0f, 1.0f - timer_ / 0.20f);
    const engine::Vector4 color = {0.42f + flash * 0.58f, 0.92f + flash * 0.08f, 1.00f, 1.0f};

    player->SetScale(scale);
    player->SetRotation(rotation);
    player->SetColor(color);

    for (engine::Object3D* child : player->GetChildren()) {
        // 有効な子モデルにも親と同じ明滅色を反映する。
        if (child) {
            child->SetColor(color);
        }
    }

    player->UpdateLocalMatrix();
    player->UpdateWorldMatrix();
}

void PlayerDeathAnimation::EmitBurst(Player* player) {
    // プレイヤーが無ければ、身体中心や足元の粒子発生位置を決められない。
    if (!player) {
        return;
    }

    engine::ParticleSystem* particle = player->GetParticleSystem();
    const engine::Vector3 origin = player->GetWorldPosition();
    const engine::Vector3 center = origin + engine::Vector3{0.0f, baseScale_.y * 0.45f, 0.0f};
    const engine::Vector3 foot = origin + engine::Vector3{0.0f, 0.08f, 0.0f};

    // 通常の粒子を使える場合は、身体中心から上へ飛散させ、足元からは低く広げる。
    if (particle) {
        engine::Vector3 up = {0.0f, 1.0f, 0.0f};
        particle->SpawnParticles(center, finalDeath_ ? 24 : 18, finalDeath_ ? 5.6f : 4.8f, &up, 124.0f,
                                 finalDeath_ ? engine::Vector4{0.38f, 0.72f, 0.95f, 0.98f} : engine::Vector4{0.38f, 0.95f, 1.0f, 0.96f},
                                 finalDeath_ ? engine::Vector4{0.02f, 0.10f, 0.18f, 0.0f} : engine::Vector4{0.08f, 0.42f, 0.72f, 0.0f},
                                 0.22f, finalDeath_ ? 0.82f : 0.68f, 0.30f, 0.035f);

        engine::Vector3 low = {0.0f, 0.08f, 0.0f};
        particle->SpawnParticles(foot, finalDeath_ ? 14 : 10, finalDeath_ ? 2.7f : 2.25f, &low, 178.0f,
                                 finalDeath_ ? engine::Vector4{0.25f, 0.58f, 0.78f, 0.80f} : engine::Vector4{0.26f, 0.88f, 1.0f, 0.78f},
                                 engine::Vector4{0.04f, 0.22f, 0.34f, 0.0f}, 0.38f, 0.92f, 0.20f, 0.055f);
    }

    // メッシュエフェクトを発生できる場合は、閃光・輪・周囲へ飛ぶ弧を重ねる。
    if (MeshEffectManager::GetInstance()) {
        MeshEffectManager* meshEffects = MeshEffectManager::GetInstance();
        meshEffects->SpawnEffectAt(kDeathCoreFlashEffect, center, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
        meshEffects->SpawnEffectAt(kDeathSplashRingEffect, foot, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
        meshEffects->SpawnRingWaveEffect(foot);

        const std::int32_t arcCount = finalDeath_ ? 7 : 5;

        for (std::int32_t i = 0; i < arcCount; ++i) {
            const float ratio = static_cast<float>(i) / static_cast<float>(arcCount);
            // 弧のメッシュを等間隔で並べ、最後の残機での破裂だけ角度の始点をずらす。
            const float angle = ratio * kTwoPi + (finalDeath_ ? 0.23f : 0.0f);
            engine::Vector3 arcPos = center;
            arcPos.x += std::cos(angle) * 0.20f;
            arcPos.y += 0.08f + 0.05f * static_cast<float>(i % 2);
            arcPos.z += std::sin(angle) * 0.20f;

            engine::Vector3 arcRot = {0.32f + 0.08f * static_cast<float>(i % 2), angle, (i % 2 == 0 ? 0.26f : -0.22f)};
            const float arcScale = finalDeath_ ? 1.18f : 1.0f;
            meshEffects->SpawnEffectAt(kDeathSplashArcEffect, arcPos, arcRot, {arcScale, arcScale, arcScale});
        }
    }

    GpuParticleManager* gpuParticles = GpuParticleManager::GetInstance();

    // GPU粒子が初期化済みの場合は、中心の飛散する筋と足元の霧を重ねる。
    if (gpuParticles && gpuParticles->IsInitialized()) {
        gpuParticles->Emit(kDeathSplashStreakPreset, center);
        gpuParticles->Emit(kDeathSplashStreakPreset, center + engine::Vector3{0.0f, 0.22f, 0.0f});
        gpuParticles->Emit(kDeathSoftMistPreset, foot);
    }
}

void PlayerDeathAnimation::EmitPuddle(Player* player) {
    // プレイヤーと粒子システムの両方がある場合だけ、破裂後の足元に少量の飛沫を出す。
    if (!player || !player->GetParticleSystem()) {
        return;
    }

    engine::Vector3 low = {0.0f, 0.05f, 0.0f};
    const engine::Vector3 foot = player->GetWorldPosition() + engine::Vector3{0.0f, 0.06f, 0.0f};
    player->GetParticleSystem()->SpawnParticles(foot, finalDeath_ ? 6 : 5, finalDeath_ ? 1.25f : 1.05f, &low, 175.0f,
                                                finalDeath_ ? engine::Vector4{0.18f, 0.48f, 0.70f, 0.44f}
                                                            : engine::Vector4{0.20f, 0.82f, 1.0f, 0.46f},
                                                engine::Vector4{0.04f, 0.18f, 0.28f, 0.0f}, 0.34f, 0.76f, 0.22f, 0.04f);
}

void PlayerDeathAnimation::HidePlayerBody(Player* player) {
    // プレイヤーが無ければ、身体や子の表示を変更できない。
    if (!player) {
        return;
    }

    player->SetIsVisible(false);

    for (engine::Object3D* child : player->GetChildren()) {
        // 有効な子モデルも非表示にし、破裂後に装飾だけ残るのを防ぐ。
        if (child) {
            child->SetIsVisible(false);
        }
    }
}
