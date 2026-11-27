#define NOMINMAX
#include "game/actor/player/state/PlayerState.h"
#include <numbers>
#include "game/actor/player/Player.h"
#include "DebugConsole.h"
#include "engine/utility/math/Math.h"
#include <cmath>
#include <memory>

// 被弾時のノックバックとスライムらしい変形演出をまとめています。

// 被弾・ノックバック状態 (Damage)
void PlayerStateDamage::Enter(Player* player) {
    DebugConsole::GetInstance()->AddLog("Enter: Damage (Knockback)");

    // 対象がいる場合だけ、運搬中の敵を手放して通常操作を止める。
    if (player) {
        player->ReleaseCarriedEnemy(true);
        player->SetIsControlActive(false);

        // 経過時間を0秒に戻し、演出前の大きさと回転を保存する。
        timer_ = 0.0f;
        baseScale_ = player->GetScale();
        baseRotation_ = player->GetRotation();

        // ノックバック方向の高さ成分を除き、水平方向の長さを求める。
        knockbackDir_.y = 0.0f;
        float dirLength = engine::Math::Length(knockbackDir_);

        // 水平成分が十分あれば、方向によらず押し出す強さをそろえるため単位化する。
        if (dirLength > 0.001f) {
            knockbackDir_ = knockbackDir_ * (1.0f / dirLength);
        // 水平成分がほぼ無ければ、プレイヤーの前方を反転して背後へ飛ばす。
        } else {
            engine::Vector3 forward = player->GetForwardDirection();
            knockbackDir_ = {-forward.x, 0.0f, -forward.z};
        }

        // 水平の吹き飛ばしとは別に上向きの初速を与え、地面から跳ねる反応にします。
        float force = 18.0f;
        engine::Vector3 v = knockbackDir_ * force;
        v.y = 9.0f;
        player->SetVelocity(v);
    }
}

// 被弾後の経過秒数から、つぶれ・跳ね・揺れの段階を選びます。
void PlayerStateDamage::Update(Player* player, float deltaTime) {
    // 操作対象が無ければ、タイマーも姿勢も更新しません。
    if (!player)
        return;

    timer_ += deltaTime;

    // 経過秒数を演出時間で割り、演出全体の進行率を求める。
    float t = timer_ / duration_;
    // 経過が演出時間を超えた場合は、進行率を1で止める。
    if (t > 1.0f)
        t = 1.0f;

    engine::Vector3 targetScale = baseScale_;
    engine::Vector3 targetRotation = baseRotation_;
    const float roll = t * (2.0f * std::numbers::pi_v<float>);
    const float rollEaseOut = 1.0f - (1.0f - t) * (1.0f - t);

    // 最初の16%は横に広げて縦をつぶし、衝撃を見せる。
    if (t < 0.16f) {
        float p = t / 0.16f;
        float ease = 1.0f - (1.0f - p) * (1.0f - p);
        targetScale = {baseScale_.x * (1.0f + 0.62f * ease), baseScale_.y * (1.0f - 0.52f * ease),
                       baseScale_.z * (1.0f + 0.62f * ease)};
    // 16%から48%までは、つぶした形から縦に伸ばして跳ね返りを表す。
    } else if (t < 0.48f) {
        float p = (t - 0.16f) / 0.32f;
        float ease = 1.0f - (1.0f - p) * (1.0f - p);
        targetScale = {engine::Math::Lerp(baseScale_.x * 1.62f, baseScale_.x * 0.72f, ease),
                       engine::Math::Lerp(baseScale_.y * 0.48f, baseScale_.y * 1.48f, ease),
                       engine::Math::Lerp(baseScale_.z * 1.62f, baseScale_.z * 0.72f, ease)};
    // 48%以降は揺れを弱めながら、元の大きさへ近づける。
    } else {
        float p = (t - 0.48f) / 0.52f;
        // 終盤区間の進行率が1を超えた場合は、揺れの終点で止める。
        if (p > 1.0f)
            p = 1.0f;
        float wobble = std::sin(p * std::numbers::pi_v<float> * 4.0f) * (1.0f - p);
        targetScale = {baseScale_.x * (1.0f + wobble * 0.28f), baseScale_.y * (1.0f - wobble * 0.22f),
                       baseScale_.z * (1.0f + wobble * 0.28f)};
    }

    // ノックバック方向に応じて転がす軸を変え、回転に合わせて横の伸縮も加える。
    targetRotation.x += knockbackDir_.z * roll;
    targetRotation.z -= knockbackDir_.x * roll;
    targetRotation.y += std::sin(t * std::numbers::pi_v<float>) * 0.35f * (knockbackDir_.x + knockbackDir_.z);
    targetScale.x += std::sin(rollEaseOut * std::numbers::pi_v<float> * 2.0f) * baseScale_.x * 0.08f;
    targetScale.z += std::cos(rollEaseOut * std::numbers::pi_v<float> * 2.0f) * baseScale_.z * 0.06f;

    // 変形と回転を一緒に反映してから、時間を満たしたときだけ通常操作へ戻します。
    player->SetScale(targetScale);
    player->SetRotation(targetRotation);

    // 経過が演出時間を満たしたら、操作を再開し待機状態へ移る。
    if (timer_ >= duration_) {
        player->SetIsControlActive(true);
        player->ChangeState(std::make_unique<PlayerStateIdle>());
    }
}

void PlayerStateDamage::Exit(Player* player) {
    // 対象がある場合は、操作と演出前の形・回転を復元する。
    if (player) {
        player->SetIsControlActive(true);
        player->SetScale(baseScale_);
        player->SetRotation(baseRotation_);
    }
}
