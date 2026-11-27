#define NOMINMAX
#include "game/actor/player/animation/PlayerGateReturnAnimation.h"
#include "Easing.h"

#include "game/actor/player/Player.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace {
constexpr float kDuration = 2.25f;
constexpr float kLandingPhaseEnd = 0.42f;
constexpr float kFirstBounceEnd = 0.72f;
constexpr float kGateExitNoGravityTime = 0.14f;
constexpr float kGravity = 18.0f;
constexpr float kLaunchHopHeight = 1.05f;
constexpr float kFirstBounceHeight = 0.48f;
constexpr float kSecondBounceHeight = 0.22f;
} // namespace

void PlayerGateReturnAnimation::Start(Player* player, const Route& route) {
    // 対象が存在し、前の演出による衝突停止が残っていれば復元してから開始する。
    if (player && collisionSuspended_) {
        RestoreCollision(player);
    }

    // 新しい経路の水平方向を単位化し、時計と保存する操作可否を初期化する。
    route_ = route;
    route_.direction = NormalizeDirection(route.direction);
    timer_ = 0.0f;
    active_ = true;
    finished_ = false;
    savedControlActive_ = player ? player->IsControlActive() : true;

    // 対象がいなければ、表示・入力・速度・当たり判定を変更できない。
    if (!player) {
        return;
    }

    // 演出の姿勢を直接適用できるよう、開始時に入力・速度・当たり判定を止めます。
    player->SetIsVisible(true);
    player->SetIsControlActive(false);
    player->SetVelocity({0.0f, 0.0f, 0.0f});
    SuspendCollision(player);
    player->SetMoveYaw(std::atan2(route_.direction.x, route_.direction.z));
    ApplyPose(player, 0.0f);
}

// 経過秒数から帰還の位置と姿勢を進める。自然終了時は終点と衝突を戻し、操作の復帰は呼出側の Stop() に任せる。
void PlayerGateReturnAnimation::Update(Player* player, float deltaTime) {
    // 演出中でないかプレイヤーがいなければ、時計と姿勢を進めない。
    if (!active_ || !player) {
        return;
    }

    timer_ += (std::max)(deltaTime, 0.0f);
    const float normalizedTime = Clamp01(timer_ / kDuration);
    ApplyPose(player, normalizedTime);

    // 2.25秒の経路を最後まで進んだら、演出を終了して終点の姿勢を確定する。
    if (normalizedTime >= 1.0f) {
        active_ = false;
        finished_ = true;

        // 終点の姿勢と衝突だけを戻す。選択シーンとゲーム本編の入口演出は、それぞれの終了時に操作を戻す。
        ApplyFinishedPose(player, false);
    }
}

void PlayerGateReturnAnimation::Stop(Player* player, bool restoreControl) {
    // 中断時も終点姿勢と接触設定を復元し、操作権だけ引数に従う。
    active_ = false;
    finished_ = false;
    ApplyFinishedPose(player, restoreControl);
}

engine::Vector3 PlayerGateReturnAnimation::NormalizeDirection(const engine::Vector3& direction) const {
    // 渡された方向から高さ成分を取り除く。
    engine::Vector3 flat{direction.x, 0.0f, direction.z};
    const float lengthSq = flat.x * flat.x + flat.z * flat.z;

    // 水平成分が小さく単位化できない場合は、正のZを既定方向に使う。
    if (lengthSq <= 0.000001f) {
        return {0.0f, 0.0f, 1.0f};
    }

    const float invLength = 1.0f / std::sqrt(lengthSq);
    return {flat.x * invLength, 0.0f, flat.z * invLength};
}

engine::Vector3 PlayerGateReturnAnimation::Lerp(const engine::Vector3& a, const engine::Vector3& b, float t) const {
    // 経路の端を越えないよう、補間率を0〜1に制限する。
    t = Clamp01(t);
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

engine::Vector3 PlayerGateReturnAnimation::Bezier(const engine::Vector3& p0, const engine::Vector3& p1, const engine::Vector3& p2, const engine::Vector3& p3,
                                          float t) const {
    // 開始位置と最初の接地点の間に二つの制御点を置き、ゲートを出る曲線を求める。
    t = Clamp01(t);
    const float inv = 1.0f - t;
    const float b0 = inv * inv * inv;
    const float b1 = 3.0f * inv * inv * t;
    const float b2 = 3.0f * inv * t * t;
    const float b3 = t * t * t;
    return {p0.x * b0 + p1.x * b1 + p2.x * b2 + p3.x * b3, p0.y * b0 + p1.y * b1 + p2.y * b2 + p3.y * b3,
            p0.z * b0 + p1.z * b1 + p2.z * b2 + p3.z * b3};
}

float PlayerGateReturnAnimation::Clamp01(float value) const {
    // 演出時間を経路の定義域へ収める。
    return std::clamp(value, 0.0f, 1.0f);
}

float PlayerGateReturnAnimation::EaseOutCubic(float value) const {
    // 補間曲線と入力範囲の扱いを共通のEasingへ委譲する。
    return Easing::OutCubic01(value);
}

float PlayerGateReturnAnimation::EaseInOutCubic(float value) const {
    // 補間曲線と入力範囲の扱いを共通のEasingへ委譲する。
    return Easing::InOutCubic01(value);
}

// 帰還経路の進行率を0〜1で受け取り、その時刻のワールド位置と変形を適用します。
void PlayerGateReturnAnimation::ApplyPose(Player* player, float normalizedTime) {
    // 対象がいなければ、その時刻の位置や姿勢を書き込めない。
    if (!player) {
        return;
    }

    // 最初の接地と一度目の跳ねが終わる位置を、帰還先と同じ地面高さにそろえます。
    engine::Vector3 landing = Lerp(route_.gateCenter, route_.end, 0.38f);
    landing.y = route_.end.y;
    engine::Vector3 firstBounceEnd = Lerp(landing, route_.end, 0.55f);
    firstBounceEnd.y = route_.end.y;

    engine::Vector3 position = route_.start;
    engine::Vector3 scale = route_.baseScale;
    float pitch = 0.0f;
    float roll = 0.0f;

    // 全体の42%まではゲートから最初の接地点へ向かい、水平の曲線と鉛直の動きを別々に求める。
    if (normalizedTime < kLandingPhaseEnd) {
        const float phaseT = Clamp01(normalizedTime / kLandingPhaseEnd);
        const float moveT = EaseOutCubic(phaseT);
        const engine::Vector3 controlA = Lerp(route_.start, route_.gateCenter, 0.46f);
        const engine::Vector3 controlB = Lerp(route_.gateCenter, landing, 0.58f);
        position = Bezier(route_.start, {controlA.x, controlA.y + 0.30f, controlA.z},
                          {controlB.x, controlB.y + 0.55f, controlB.z}, landing, moveT);
        const float groundY = route_.end.y;
        const float fallStartY = (std::max)({route_.start.y, route_.gateCenter.y, groundY + kLaunchHopHeight});

        // 最初の接地までの区間のうち冒頭14%では、重力による落下を始めず出口の高さへつなぐ。
        if (phaseT < kGateExitNoGravityTime) {
            const float gateExitT = EaseOutCubic(Clamp01(phaseT / kGateExitNoGravityTime));
            position.y = route_.start.y + (fallStartY - route_.start.y) * gateExitT;
        // ゲートを出た後は、最初の接地時刻に間に合う加速度で落下させる。
        } else {
            const float fallT = Clamp01((phaseT - kGateExitNoGravityTime) / (1.0f - kGateExitNoGravityTime));
            const float fallDuration =
                (std::max)(kDuration * kLandingPhaseEnd * (1.0f - kGateExitNoGravityTime), 0.01f);
            const float fallTime = fallT * fallDuration;
            const float fallHeight = (std::max)(fallStartY - groundY, 0.0f);
            const float landingGravity = (std::max)(kGravity, (fallHeight * 2.0f) / (fallDuration * fallDuration));

            // 落下区間の上向き初速は0とし、下向きの変位だけを積む。
            position.y = fallStartY - 0.5f * landingGravity * fallTime * fallTime;

            // 経路の地面を下回る場合は、その高さにそろえてめり込みを防ぐ。
            if (position.y < groundY) {
                position.y = groundY;
            }
        }

        // 出口での圧縮、空中の伸び、最初の着地でのつぶれを基準寸法へ重ねる。
        const float launchSquash = 1.0f - Clamp01(phaseT / 0.22f);
        const float airborneStretch = std::sin(phaseT * std::numbers::pi_v<float>);
        const float landingSquash = std::sin(Clamp01((phaseT - 0.78f) / 0.22f) * std::numbers::pi_v<float>);
        scale = {route_.baseScale.x * (1.0f + launchSquash * 0.20f - airborneStretch * 0.06f + landingSquash * 0.32f),
                 route_.baseScale.y * (1.0f - launchSquash * 0.22f + airborneStretch * 0.20f - landingSquash * 0.38f),
                 route_.baseScale.z * (1.0f + launchSquash * 0.24f - airborneStretch * 0.05f + landingSquash * 0.26f)};
        pitch = -route_.direction.z * std::sin(phaseT * std::numbers::pi_v<float>) * 0.18f;
        roll = route_.direction.x * std::sin(phaseT * std::numbers::pi_v<float>) * 0.16f;
    // 全体の42%から72%までは、最初の接地から大きく弾ませ、次の接地で縮める。
    } else if (normalizedTime < kFirstBounceEnd) {
        const float phaseT = Clamp01((normalizedTime - kLandingPhaseEnd) / (kFirstBounceEnd - kLandingPhaseEnd));
        const float moveT = EaseInOutCubic(phaseT);
        position = Lerp(landing, firstBounceEnd, moveT);
        position.y += std::sin(phaseT * std::numbers::pi_v<float>) * kFirstBounceHeight;

        const float stretch = std::sin(phaseT * std::numbers::pi_v<float>);
        const float landingSquash = std::sin(Clamp01((phaseT - 0.72f) / 0.28f) * std::numbers::pi_v<float>);
        scale = {route_.baseScale.x * (1.0f - stretch * 0.08f + landingSquash * 0.22f),
                 route_.baseScale.y * (1.0f + stretch * 0.18f - landingSquash * 0.28f),
                 route_.baseScale.z * (1.0f - stretch * 0.06f + landingSquash * 0.18f)};
        pitch = -route_.direction.z * std::sin(phaseT * std::numbers::pi_v<float>) * 0.12f;
        roll = route_.direction.x * std::sin(phaseT * std::numbers::pi_v<float>) * 0.10f;
    // 全体の72%以降は小さく弾ませながら、終点と基準姿勢へ近づける。
    } else {
        const float phaseT = Clamp01((normalizedTime - kFirstBounceEnd) / (1.0f - kFirstBounceEnd));
        const float moveT = EaseInOutCubic(phaseT);
        position = Lerp(firstBounceEnd, route_.end, moveT);
        position.y += std::sin(phaseT * std::numbers::pi_v<float>) * kSecondBounceHeight;

        const float stretch = std::sin(phaseT * std::numbers::pi_v<float>);
        const float landingSquash = std::sin(Clamp01((phaseT - 0.70f) / 0.30f) * std::numbers::pi_v<float>);
        const float recover = EaseInOutCubic(phaseT);
        scale = {route_.baseScale.x * (1.0f - stretch * 0.05f + landingSquash * 0.12f) * (1.0f - recover * 0.00f),
                 route_.baseScale.y * (1.0f + stretch * 0.10f - landingSquash * 0.16f),
                 route_.baseScale.z * (1.0f - stretch * 0.04f + landingSquash * 0.10f)};
        pitch = -route_.direction.z * std::sin(phaseT * std::numbers::pi_v<float>) * 0.06f;
        roll = route_.direction.x * std::sin(phaseT * std::numbers::pi_v<float>) * 0.05f;
    }

    // 通常移動を止めたまま演出で決めた位置・向き・形状を反映し、行列を確定します。
    player->SetIsVisible(true);
    player->SetIsControlActive(false);
    player->SetVelocity({0.0f, 0.0f, 0.0f});
    player->SetMoveYaw(std::atan2(route_.direction.x, route_.direction.z));

    player->SetTranslate(position);
    player->SetScale(scale);
    player->SetEulerX(pitch);
    player->SetEulerZ(roll);
    player->SetQuaternionMaster(false);
    player->UpdateLocalMatrix();
    player->UpdateWorldMatrix();
}

void PlayerGateReturnAnimation::ApplyFinishedPose(Player* player, bool restoreControl) {
    // プレイヤーが無ければ、演出中に変更した姿勢と衝突を復元できない。
    if (!player) {
        return;
    }

    player->SetVelocity({0.0f, 0.0f, 0.0f});
    player->SetIsVisible(true);
    RestoreCollision(player);

    // 操作を戻す指定がある場合だけ、演出前の操作可否を書き戻す。
    if (restoreControl) {
        player->SetIsControlActive(savedControlActive_);
    }

    // 中断でも途中の位置を残さず終点へ置き、横倒しになった演出角を消してから行列を確定します。
    player->SetTranslate(route_.end);
    player->SetScale(route_.baseScale);
    player->SetEulerX(0.0f);
    player->SetEulerZ(0.0f);
    player->SetQuaternionMaster(false);
    player->UpdateLocalMatrix();
    player->UpdateWorldMatrix();
}

void PlayerGateReturnAnimation::SuspendCollision(Player* player) {
    // 対象がいないか、すでに衝突を止めている場合は保存値を更新しない。
    if (!player || collisionSuspended_) {
        return;
    }

    // 帰還演出中はTransformを演出側で直接制御します。
    // ゲートの縁やトリガー判定に押し戻されると空中で跳ねて見えるため、一時的に衝突を止めます。
    savedCollisionAttribute_ = player->GetCollisionAttribute();
    savedCollisionMask_ = player->GetCollisionMask();
    player->SetCollisionAttribute(0);
    player->SetCollisionMask(0);
    collisionSuspended_ = true;
}

void PlayerGateReturnAnimation::RestoreCollision(Player* player) {
    // 対象が存在して衝突を止めている場合だけ、開始前の属性とマスクを戻す。
    if (!player || !collisionSuspended_) {
        return;
    }

    // 保存した属性とマスクを復元し、次回は再度保存できる状態にする。
    player->SetCollisionAttribute(savedCollisionAttribute_);
    player->SetCollisionMask(savedCollisionMask_);
    collisionSuspended_ = false;
}
