#define NOMINMAX
#include "game/actor/player/state/PlayerState.h"
#include "game/actor/player/Player.h"
#include "InputManager.h"
#include "DebugConsole.h"
#include "engine/graphics/3d/camera/CameraManager.h"
#include "engine/utility/math/Math.h"
#include "BaseEnemy.h"
#include "EnemyGiantSlime.h"
#include "GimmickHookPullBlock.h"
#include "CollisionConfig.h"
#include "CollisionManager.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>

// フックでオブジェクトを引く状態をまとめています。

void PlayerStatePullObject::Enter(Player* player) {
    // フックの開始位置を確定し、発射中はプレイヤーの操作と速度を止める。
    DebugConsole::GetInstance()->AddLog("Enter: PullObject");
    // プレイヤーが無ければ、姿勢・速度・マーカーを更新できない。
    if (!player)
        return;

    player->SetIsControlActive(false);
    player->SetVelocity({0.0f, 0.0f, 0.0f});
    player->SetSlimeAnimationMode(PlayerSlimeAnimator::Mode::kShootHook);
    player->SetSlimeAnimationDirection(targetPos_ - player->GetWorldPosition());
    // 事前命中済みなら対象位置を、これから伸ばす場合はプレイヤー位置を使う。
    hookTipPos_ = alreadyHit_ && targetObject_ ? targetObject_->GetWorldPosition() : player->GetWorldPosition();
    timer_ = 0.0f;
    pullStarted_ = false;

    engine::Object3D* marker = player->GetHookMarker();

    // 専用の鞭表示が無く共通マーカーがあれば、先端にマーカーを置く。
    if (marker && !whipVisual_) {
        marker->SetIsVisible(true);
        marker->SetColor({0.35f, 0.9f, 1.0f, 1.0f});
        marker->SetTranslate(hookTipPos_);
    }
    // 専用の鞭が渡された場合は、遷移元が指定した命中先の位置に表示を合わせる。
    if (whipVisual_) whipVisual_->UpdatePull(player, targetObject_->GetWorldPosition(), 0.0f);
}

void PlayerStatePullObject::Update(Player* player, float deltaTime) {
    // シーンから除去された対象を操作しないよう、現在の登録リストと照合する。
    const auto& registered = engine::CollisionManager::GetInstance()->GetObjects();
    // 以前の対象が登録済み一覧に無ければ、参照を無効にする。
    if (targetObject_ && std::find(registered.begin(), registered.end(), targetObject_) == registered.end())
        targetObject_ = nullptr;

    // プレイヤーか対象が失われたら、この状態の更新を中止する。
    if (!player || !targetObject_) {
        // プレイヤーが残っていれば、待機へ戻して操作を復元する。
        if (player)
            player->ChangeState(std::make_unique<PlayerStateIdle>());
        return;
    }

    // 接触前は発射からの経過秒、接触後は引き寄せ開始からの経過秒として使う。
    timer_ += deltaTime;

    engine::Vector3 targetPos = targetObject_->GetWorldPosition();
    engine::Vector3 toTarget = targetPos - hookTipPos_;
    float distance = engine::Math::Length(toTarget);

    // 接触前は引き寄せを始めず、フックの発射姿勢を使う。
    if (!pullStarted_) {
        player->SetSlimeAnimationMode(PlayerSlimeAnimator::Mode::kShootHook);
        player->SetSlimeAnimationDirection(targetPos - player->GetWorldPosition());

        // 事前命中済みか、フック先端が対象から3単位未満まで近づけば接触扱いとする。
        if (alreadyHit_ || distance < 3.0f) {
            alreadyHit_ = false;
            hookTipPos_ = targetPos;

            // 専用ブロックだけが位置移動を持ち、通常オブジェクトには演出のみ行う。
            if (auto* block = dynamic_cast<GimmickHookPullBlock*>(targetObject_)) {
                block->StartHookPull(player->GetWorldPosition());
            }

            // ここを境に、タイマーを引き寄せ演出の経過秒数へ切り替える。
            pullStarted_ = true;
            timer_ = 0.0f;
        // まだ届いていなければ、フレーム時間に応じて先端を伸ばす。
        } else {
            engine::Vector3 dir = engine::Math::Normalize(toTarget);
            hookTipPos_ = hookTipPos_ + dir * (180.0f * deltaTime);
        }

        // 専用表示があれば、発射中も対象位置を渡して鞭を描く。
        if (whipVisual_) whipVisual_->UpdatePull(player, targetPos, deltaTime);
        // 専用表示が無ければ、共通マーカーを現在の先端まで伸ばす。
        else UpdateRopeMarker(player, hookTipPos_, 0.22f);
        return;
    }

    // 接触後は専用表示を対象の現在位置に追従させる。
    if (whipVisual_) whipVisual_->UpdatePull(player, targetObject_->GetWorldPosition(), deltaTime);
    // 専用表示が無ければ、共通マーカーを対象までの細いロープとして描く。
    else UpdateRopeMarker(player, targetObject_->GetWorldPosition(), 0.18f);
    // 接触後0.35秒の進行を0〜1に換算して、引き姿勢に使う。
    player->SetSlimeAnimationMode(PlayerSlimeAnimator::Mode::kPullEnemy);
    player->SetSlimePullDirection(targetObject_->GetWorldPosition() - player->GetWorldPosition());
    player->SetSlimePullProgress(std::clamp(timer_ / 0.35f, 0.0f, 1.0f));

    // 接触後の経過が0.35秒を超えたら、引き寄せ演出を終える。
    if (timer_ > 0.35f) {
        // 専用表示を使っていた場合は、通常状態へ戻る前に鞭の回収を予約する。
        if (whipVisual_) player->BeginWhipReturn(whipVisual_);
        // 引き寄せ演出の終了時に Idle へ戻し、Exit で入力とマーカーを復元する。
        player->ChangeState(std::make_unique<PlayerStateIdle>());
    }
}

void PlayerStatePullObject::Exit(Player* player) {
    const auto& registered = engine::CollisionManager::GetInstance()->GetObjects();
    // 退出時にも登録を確認し、既に除去された対象の参照を破棄する。
    if (targetObject_ && std::find(registered.begin(), registered.end(), targetObject_) == registered.end())
        targetObject_ = nullptr;

    // プレイヤーが失われていれば、操作とマーカーを復元する対象がない。
    if (!player)
        return;

    // 専用表示がある場合はFinish()を呼んで、この状態の参照を手放す。
    if (whipVisual_) { whipVisual_->Finish(player); whipVisual_.reset(); }
    player->SetIsControlActive(true);
    engine::Object3D* marker = player->GetHookMarker();

    // 専用の鞭への参照は先に解放済み。共通マーカーがあれば通常の表示・回転設定に戻す。
    if (marker && !whipVisual_) {
        marker->SetIsVisible(false);
        marker->SetScale({1.0f, 1.0f, 1.0f});
        marker->SetEulerAngles({0.0f, 0.0f, 0.0f});
        marker->SetQuaternionMaster(true);
        marker->UpdateLocalMatrix();
        marker->UpdateWorldMatrix();
    }
}

void PlayerStatePullObject::UpdateRopeMarker(Player* player, const engine::Vector3& endPos, float thickness) {
    // プレイヤーが無ければ、ロープの始点を決められない。
    if (!player)
        return;

    engine::Object3D* marker = player->GetHookMarker();
    // 共通マーカーを取得できなければ、描画するロープモデルがない。
    if (!marker)
        return;

    engine::Vector3 startPos = player->GetWorldPosition();
    engine::Vector3 diff = endPos - startPos;
    float length = engine::Math::Length(diff);
    // 両端の距離が0.01未満の場合は、モデルの奥行きがゼロになるのを避ける。
    if (length < 0.01f)
        length = 0.01f;

    // 両端の中点に置き、差分からワールド空間でのピッチとヨーを求める。
    marker->SetIsVisible(true);
    marker->SetColor({0.35f, 0.9f, 1.0f, 1.0f});
    marker->SetTranslate(startPos + diff * 0.5f);
    marker->SetEulerAngles({std::atan2(-diff.y, std::sqrt(diff.x * diff.x + diff.z * diff.z)),
                                      std::atan2(diff.x, diff.z), 0.0f});
    // 求めたオイラー角で回し、指定された太さと両端の距離を寸法に使う。
    marker->SetQuaternionMaster(false);
    marker->SetScale({thickness, thickness, length});
    marker->UpdateLocalMatrix();
    marker->UpdateWorldMatrix();
}
