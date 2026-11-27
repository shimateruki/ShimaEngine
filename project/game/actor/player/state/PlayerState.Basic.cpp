#define NOMINMAX
#include "game/actor/player/state/PlayerState.h"
#include "game/actor/player/Player.h"
#include "engine/utility/math/Math.h"
#include "DebugConsole.h"
#include <memory>

// 基本移動系の状態をまとめています。

// 待機状態 (Idle)
void PlayerStateIdle::Enter(Player* player) {
    DebugConsole::GetInstance()->AddLog("Enter: Idle");

    // 対象があれば、待機アニメーションを選んでジャンプの溜め表示を消す。
    if (player) {
        player->SetSlimeAnimationMode(PlayerSlimeAnimator::Mode::kIdle);
        player->SetSlimeJumpCharge(0.0f);
    }
}

void PlayerStateIdle::Update(Player* player, float deltaTime) {
    // 接地を先に見て、水平移動だけを走り状態への条件に使う。
    (void)deltaTime;
    // 対象が無ければ接地と速度を参照できない。
    if (!player)
        return;

    // 接地していない場合は、水平速度を判定する前に空中状態へ移る。
    if (!player->IsGrounded()) {
        player->ChangeState(std::make_unique<PlayerStateJump>());
        return;
    }

    // 地上で走り出したかは、上下の動きを除いた水平速度で判定する。
    engine::Vector3 vel = player->GetVelocity();
    vel.y = 0.0f;

    // 水平速度が0.1を超えれば、走行状態へ切り替える。
    if (engine::Math::Length(vel) > 0.1f) {
        player->ChangeState(std::make_unique<PlayerStateRun>());
    }
}

void PlayerStateIdle::Exit(Player* player) {
    // 待機状態には退出時に戻す一時的な値がない。
    (void)player;
}

// 走り状態 (Run)
void PlayerStateRun::Enter(Player* player) {
    // 走行モーションへ切り替え、移動速度はプレイヤー本体側で管理する。
    DebugConsole::GetInstance()->AddLog("Enter: Run");

    // 対象があれば走行姿勢をアニメーターへ渡す。
    if (player) {
        player->SetSlimeAnimationMode(PlayerSlimeAnimator::Mode::kRun);
    }
}

void PlayerStateRun::Update(Player* player, float deltaTime) {
    // 空中ではジャンプを優先し、水平速度が下がったら待機へ戻る。
    (void)deltaTime;
    // 対象が無ければ状態を変更できない。
    if (!player)
        return;

    // 足場を離れたフレームは、減速していても空中状態を優先する。
    if (!player->IsGrounded()) {
        player->ChangeState(std::make_unique<PlayerStateJump>());
        return;
    }

    engine::Vector3 vel = player->GetVelocity();
    vel.y = 0.0f;

    // 水平速度が0.1以下になったら、待機状態へ戻る。
    if (engine::Math::Length(vel) <= 0.1f) {
        player->ChangeState(std::make_unique<PlayerStateIdle>());
    }
}

void PlayerStateRun::Exit(Player* player) {
    // 速度は共有の移動処理が保持するため、この状態では変更しない。
    (void)player;
}

// ジャンプ状態 (Jump)
void PlayerStateJump::Enter(Player* player) {
    DebugConsole::GetInstance()->AddLog("Enter: Jump");

    // 対象があれば、空中アニメーションを選ぶ。
    if (player) {
        player->SetSlimeAnimationMode(PlayerSlimeAnimator::Mode::kJump);
    }
}

void PlayerStateJump::Update(Player* player, float deltaTime) {
    // 空中姿勢を維持し、着地したフレームで待機へ切り替える。
    (void)deltaTime;
    // 対象が無ければ姿勢変更も着地判定も行わない。
    if (!player)
        return;

    player->SetSlimeAnimationMode(PlayerSlimeAnimator::Mode::kJump);

    // 接地しており、上向きにも移動していないときに着地扱いとする。
    if (player->IsGrounded() && player->GetVelocity().y <= 0.0f) {
        player->ChangeState(std::make_unique<PlayerStateIdle>());
    }
}

void PlayerStateJump::Exit(Player* player) {
    // 着地後の姿勢変更は遷移先の Enter が担当する。
    (void)player;
}

// 回避ダッシュ状態 (Dash)
void PlayerStateDash::Enter(Player* player) {
    // ダッシュ開始時の進行方向を変形アニメーションへ渡す。
    DebugConsole::GetInstance()->AddLog("Enter: Dash");
    timer_ = 0.0f;

    // 対象があれば、現在の速度をダッシュのアニメーション方向に使う。
    if (player) {
        player->SetSlimeAnimationMode(PlayerSlimeAnimator::Mode::kDash);
        player->SetSlimeAnimationDirection(player->GetVelocity());
    }
}

void PlayerStateDash::Update(Player* player, float deltaTime) {
    // ダッシュ状態の滞在時間だけを計り、移動自体は本体に任せる。
    // 対象が無ければ待機への遷移を要求できない。
    if (!player)
        return;

    // deltaTimeを秒として加算し、この状態へ入ってからの経過を計る。
    timer_ += deltaTime;

    // 経過が0.2秒に達したら待機へ戻す。
    if (timer_ >= 0.2f) {
        player->ChangeState(std::make_unique<PlayerStateIdle>());
    }
}

void PlayerStateDash::Exit(Player* player) {
    // 対象がある場合だけ、ダッシュから通常姿勢へつなぐ弾性の反動を与える。
    if (player) {
        player->TriggerSlimeImpulse({2.55f, 1.25f, 2.55f}, 0.16f);
    }
}
