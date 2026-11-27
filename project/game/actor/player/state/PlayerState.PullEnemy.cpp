#define NOMINMAX
#include "game/actor/player/state/PlayerState.h"
#include <numbers>
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
#include "game/system/combat/HitEffectDirector.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
namespace {
engine::Vector3 GetWhipPullAnchor(engine::Object3D* enemy) {
    // 巨大スライムは専用の接続点を使い、他の敵は取得できる場合にモデルの中心をワールド座標へ変換します。
    if (const auto* giant = dynamic_cast<EnemyGiantSlime*>(enemy)) return giant->GetHookAttachmentPosition();
    // 専用の接続点を持たない敵でモデルを取得できる場合は、モデル中心をワールドへ変換する。
    if (enemy->GetModel()) return engine::Math::Transform(enemy->GetModel()->GetCenter(), enemy->GetWorldMatrix());
    return enemy->GetWorldPosition();
}

engine::Vector3 GetStablePullEnemyScale(engine::Object3D* enemy) {
    // 引き伸ばし中の一時的な縦横比ではなく、体積を保つ基準尺度で捕獲演出を始める。
    if (!enemy) {
        return {1.0f, 1.0f, 1.0f};
    }

    const engine::Vector3 scale = enemy->GetScale();

    // 敵クラス以外の対象は、演出用の体積補正をせずに現在のスケールを使う。
    if (!dynamic_cast<BaseEnemy*>(enemy)) {
        return scale;
    }

    const float absX = std::abs(scale.x);
    const float absY = std::abs(scale.y);
    const float absZ = std::abs(scale.z);
    const float minScale = (std::min)({absX, absY, absZ});
    const float maxScale = (std::max)({absX, absY, absZ});

    // ゼロに近い軸がある場合と、軸の倍率差が小さい場合は、補正せず元のスケールを返す。
    if (minScale <= 0.0001f || maxScale / minScale < 1.18f) {
        return scale;
    }

    // 三軸の倍率の積と同じ体積倍率になるよう立方根を取り、各軸に同じ倍率を使います。
    const float stableScale = std::cbrt(absX * absY * absZ);
    return {stableScale, stableScale, stableScale};
}
} // namespace


// 通常形態のフックで捕まえた敵を運搬状態へ移す。巨大スライムだけは専用の分裂処理を進める。

void PlayerStatePullEnemy::Enter(Player* player) {
    // プレイヤーと引き寄せ対象が揃わなければ、操作を止める前に終了する。
    if (!player || !targetEnemy_)
        return;

    // 引き寄せ中は操作と速度を止め、掴んだ敵へ向けてフックの姿勢を作る。
    player->SetIsControlActive(false);
    player->SetVelocity({0.0f, 0.0f, 0.0f}); // プレイヤー自身はその場で停止
    player->SetSlimeAnimationMode(PlayerSlimeAnimator::Mode::kShootHook);
    player->SetSlimeAnimationDirection(targetEnemy_->GetWorldPosition() - player->GetWorldPosition());
    // 重量級は専用の抵抗処理を使う。敵の姿勢と大きさは、拘束を解いたときに戻せるよう控える。
    isHeavyPullTarget_ = dynamic_cast<EnemyGiantSlime*>(targetEnemy_) != nullptr;
    enemyPullStartScale_ = targetEnemy_->GetScale();
    enemyBaseScale_ = GetStablePullEnemyScale(targetEnemy_);

    // 対象の変換を取得できる場合は、中断時のために回転と回転方式を保存する。
    if (const Transform* enemyTransform = targetEnemy_->GetTransform()) {
        enemyBaseRotation_ = enemyTransform->rotate;
        enemyBaseQuaternion_ = enemyTransform->quaternion;
        enemyBaseQuaternionMaster_ = enemyTransform->isQuaternionMaster;
    }

    // すでに当たっている場合は敵の位置、射出前なら手元を先端位置の基準にする。
    phase_ = Phase::kShootHook;
    hookTipPos_ = alreadyHit_ && targetEnemy_ ? targetEnemy_->GetWorldPosition() : player->GetWorldPosition();
    heavyPullBasePlayerPos_ = hookTipPos_;
    hasHeavyPullBasePlayerPos_ = false;
    normalPullBasePlayerPos_ = hookTipPos_;
    hasNormalPullBasePlayerPos_ = false;

    engine::Object3D* marker = player->GetHookMarker();

    // 鞭の曲線を引き継いでいない場合だけ簡易マーカーで先端を表示する。
    if (marker && !whipVisual_) {
        marker->SetIsVisible(true);
        marker->SetColor({0.10f, 0.82f, 1.0f, 1.0f});
        marker->SetTranslate(hookTipPos_);
    }
    // 既存の鞭表示を引き継いだ場合は、同じフレームで接続点を合わせる。
    if (whipVisual_) whipVisual_->UpdatePull(player, GetWhipPullAnchor(targetEnemy_), 0.0f);
}

void PlayerStatePullEnemy::Update(Player* player, float deltaTime) {
    // シーンから除去された対象を逆参照せず、通常操作へ戻します。
    const auto& registered = engine::CollisionManager::GetInstance()->GetObjects();
    // 対象が衝突管理から外れていれば、残っている借用参照を解除する。
    if (targetEnemy_ && std::find(registered.begin(), registered.end(), targetEnemy_) == registered.end())
        targetEnemy_ = nullptr;

    // プレイヤーがいなければ、攻撃状態や表示を更新できない。
    if (!player) {
        return;
    }

    // 引き寄せ先がなくなったら、フック状態を終了して通常操作に戻る。
    if (!targetEnemy_) {
        player->ChangeState(std::make_unique<PlayerStateIdle>());
        return;
    }

    engine::Vector3 playerPos = player->GetWorldPosition();

    // フックを発射した段階では、先端が対象に届くまで敵を引き寄せない。
    if (phase_ == Phase::kShootHook) {
        player->SetSlimeAnimationMode(PlayerSlimeAnimator::Mode::kShootHook);
        player->SetSlimeAnimationDirection(targetEnemy_->GetWorldPosition() - playerPos);

        // フックの先端を対象へ進め、到達した時点で引き寄せ可否を判定します。
        engine::Vector3 enemyPos = targetEnemy_->GetTransform()->translate;
        engine::Vector3 toTarget = enemyPos - hookTipPos_;
        float distance = engine::Math::Length(toTarget);

        // 命中済みの通知があるか先端が対象から5未満まで近づいたら、拘束へ進めるか調べる。
        if (alreadyHit_ || distance < 5.0f) {
            alreadyHit_ = false;
            hookTipPos_ = enemyPos;

            // 拘束に耐性のある敵には命中表示だけを出し、位置や衝突設定は変更せずに通常操作へ戻します。
            if (auto* enemyBase = dynamic_cast<BaseEnemy*>(targetEnemy_); enemyBase && enemyBase->IsPullImmune()) {
                HitEffectDirector::SpawnPullBindHit(enemyPos);

                // 簡易表示の参照が残っている場合は、耐性のある相手に当たった時点で非表示にする。
                if (engine::Object3D* marker = player->GetHookMarker()) {
                    marker->SetIsVisible(false);
                }

                player->SetSlimePullProgress(0.0f);
                player->ChangeState(std::make_unique<PlayerStateIdle>());
                return;
            }

            phase_ = Phase::kPullEnemy;
            enemyStartPos_ = enemyPos;
            HitEffectDirector::SpawnPullBindHit(enemyPos);
            pullTimer_ = -0.12f; // 命中後の0.12秒は引き寄せの移動を始めない。

            // 巨大スライムに当たった場合は、運搬せず専用の引き裂き動作へ進む。
            if (auto* giantSlime = dynamic_cast<EnemyGiantSlime*>(targetEnemy_)) {
                isHeavyPullTarget_ = true;
                giantSlime->BeginHookSplitPull(playerPos);
            // ほかの対象は、通常の引き寄せとして衝突と運搬状態を切り替える。
            } else {
                // 引き寄せ中の敵は運搬状態へ移し、プレイヤーや地形との二重衝突を避けます。
                targetEnemy_->SetCollisionAttribute(0);
                targetEnemy_->SetCollisionMask(0);

                BaseEnemy* enemyBase = dynamic_cast<BaseEnemy*>(targetEnemy_);

                // 運搬フラグを持つ敵には、引き寄せ中の運搬状態も通知する。
                if (enemyBase) {
                    enemyBase->SetCarried(true);
                }
            }

            // CPU粒子の描画先を取得できる場合に、拘束開始の飛沫を出す。
            if (player->GetParticleSystem()) {
                engine::Vector3 toPlayer = engine::Math::Normalize(playerPos - enemyPos);
                player->GetParticleSystem()->SpawnParticles(enemyPos, 30, 2.0f, &toPlayer, 30.0f,
                                                            {0.55f, 0.95f, 1.0f, 1.0f}, {0.25f, 0.75f, 1.0f, 0.0f},
                                                            0.2f, 0.4f, 0.8f, 0.1f);
            }
        // 先端が対象に届いていない場合は、対象への方向に先端を進める。
        } else {
            engine::Vector3 dir = engine::Math::Normalize(toTarget);
            hookTipPos_ = hookTipPos_ + dir * (150.0f * deltaTime);
        }

        engine::Object3D* marker = player->GetHookMarker();

        // 曲線の鞭表示を持たない場合だけ、簡易マーカーを先端とプレイヤーの間に伸ばす。
        if (marker && !whipVisual_) {
            engine::Vector3 diff = hookTipPos_ - playerPos;
            marker->SetTranslate({playerPos.x + diff.x * 0.5f, playerPos.y + diff.y * 0.5f,
                                                 playerPos.z + diff.z * 0.5f});
            float angleY = std::atan2(diff.x, diff.z);
            float angleX = std::atan2(-diff.y, std::sqrt(diff.x * diff.x + diff.z * diff.z));
            marker->SetEulerAngles({angleX, angleY, 0.0f});
            marker->SetQuaternionMaster(false);
            marker->SetScale({0.5f, 0.5f, engine::Math::Length(diff)});
            marker->SetColor({0.10f, 0.82f, 1.0f, 1.0f});

            // 長さと向きを更新した後にワールド行列を確定します。
            marker->UpdateLocalMatrix();
            marker->UpdateWorldMatrix();
        }
        // 引き継いだ鞭表示があれば、先端の接続点と今フレームの動きを渡す。
        if (whipVisual_) whipVisual_->UpdatePull(player, GetWhipPullAnchor(targetEnemy_), deltaTime);
    // 命中後は拘束反応を終えるまで引き寄せを待ち、次いで相手の種類に応じて移動か分裂を進める。
    } else if (phase_ == Phase::kPullEnemy) {
        pullTimer_ += deltaTime;
        player->SetSlimeAnimationMode(PlayerSlimeAnimator::Mode::kPullEnemy);
        player->SetSlimePullDirection(targetEnemy_->GetWorldPosition() - playerPos);
        player->SetSlimePullProgress(0.0f);

        // タイマーが負の間は手元への移動を止め、命中地点での拘束反応を表示する。
        if (pullTimer_ < 0.0f) {
            engine::Vector3 tetherEnemyPos = targetEnemy_->GetWorldPosition();
            float shakeX = 0.0f;
            float shakeY = 0.0f;
            float shakeZ = 0.0f;

            // ヒットストップ中の巨大スライムは、専用動作で後端を固定したまま抵抗させる。
            if (isHeavyPullTarget_) {
                // 巨大スライムの実体を取得できれば、分裂用の姿勢と鞭の接続点を更新する。
                if (auto* giantSlime = dynamic_cast<EnemyGiantSlime*>(targetEnemy_)) {
                    giantSlime->UpdateHookSplitPull(0.0f, playerPos, player->GetParticleSystem());
                    tetherEnemyPos = giantSlime->GetHookAttachmentPosition();
                }
            // 通常の敵では、命中時の位置を中心に小さな乱数の揺れを付ける。
            } else {
                shakeX = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * 0.4f;
                shakeY = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * 0.4f;
                shakeZ = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * 0.4f;
                tetherEnemyPos = {enemyStartPos_.x + shakeX, enemyStartPos_.y + shakeY, enemyStartPos_.z + shakeZ};
                targetEnemy_->SetTranslate(tetherEnemyPos);
                targetEnemy_->SetQuaternionMaster(false);
                targetEnemy_->UpdateLocalMatrix();
                targetEnemy_->UpdateWorldMatrix();
            }

            engine::Object3D* marker = player->GetHookMarker();

            // 簡易表示を使う場合は、ヒットストップ中も揺れる相手までの線を更新する。
            if (marker && !whipVisual_) {
                engine::Vector3 diff = tetherEnemyPos - playerPos;
                marker->SetTranslate({playerPos.x + diff.x * 0.5f, playerPos.y + diff.y * 0.5f,
                                                     playerPos.z + diff.z * 0.5f});

                // 拘束直後は簡易フックの横幅を0.13と0.10に絞り、線が張った状態を示す。
                float length = engine::Math::Length(diff);
                marker->SetScale({0.13f, 0.10f, length});
                marker->SetColor({0.10f, 0.82f, 1.0f, 1.0f});

                marker->UpdateLocalMatrix();
                marker->UpdateWorldMatrix();
            }

            // 鞭表示を引き継いでいる場合は、ヒットストップ中も接続点へ追従させる。
            if (whipVisual_) whipVisual_->UpdatePull(player, GetWhipPullAnchor(targetEnemy_), deltaTime);
            return;
        }

        // 巨大スライムの経路は分裂の進行を扱い、通常の敵を手元に引き寄せる処理へは進まない。
        if (isHeavyPullTarget_) {
            auto* giantSlime = dynamic_cast<EnemyGiantSlime*>(targetEnemy_);

            // 専用の相手が消失したか、すでに分裂済みであれば通常状態へ戻る。
            if (!giantSlime || giantSlime->HasSplit()) {
                player->ChangeState(std::make_unique<PlayerStateIdle>());
                return;
            }

            bool hasSplit = giantSlime->UpdateHookSplitPull(deltaTime, playerPos, player->GetParticleSystem());
            float progress = giantSlime->GetHookSplitProgress();

            // 分裂に抵抗する間の足元を一度だけ控え、細かな踏ん張りをその位置に足します。
            if (!hasHeavyPullBasePlayerPos_) {
                heavyPullBasePlayerPos_ = playerPos;
                hasHeavyPullBasePlayerPos_ = true;
            }

            engine::Vector3 enemyCurrentPos = targetEnemy_->GetTransform()->translate;
            const engine::Vector3 hookAttachmentPos = giantSlime->GetHookAttachmentPosition();
            engine::Vector3 playerToEnemy = enemyCurrentPos - heavyPullBasePlayerPos_;
            engine::Vector3 pullDir = {playerToEnemy.x, 0.0f, playerToEnemy.z};
            float pullDirLength = engine::Math::Length(pullDir);
            pullDir = pullDirLength > 0.001f ? pullDir / pullDirLength : engine::Vector3{0.0f, 0.0f, 1.0f};
            const float bracePulse = std::abs(std::sin(pullTimer_ * 38.0f));
            const float braceAmount = (0.10f + bracePulse * 0.16f) * (1.0f - progress * 0.25f);
            engine::Vector3 visualPlayerPos = heavyPullBasePlayerPos_ - pullDir * braceAmount;
            visualPlayerPos.y += std::sin(pullTimer_ * 52.0f) * 0.035f * (1.0f - progress * 0.25f);
            player->SetTranslate(visualPlayerPos);
            player->UpdateLocalMatrix();
            player->UpdateWorldMatrix();
            playerPos = visualPlayerPos;

            player->SetSlimePullDirection(hookAttachmentPos - playerPos);
            player->SetSlimePullProgress(progress);

            engine::Object3D* marker = player->GetHookMarker();

            // 曲線表示を使わない場合は、巨大スライムの接続点まで簡易表示を伸ばす。
            if (marker && !whipVisual_) {
                engine::Vector3 diff = hookAttachmentPos - playerPos;
                float length = engine::Math::Length(diff);
                // 簡易表示の長さがほぼ0なら、表示の最小の長さを確保する。
                if (length < 0.01f)
                    length = 0.01f;

                marker->SetTranslate(playerPos + diff * 0.5f);
                marker->SetEulerAngles({std::atan2(-diff.y, std::sqrt(diff.x * diff.x + diff.z * diff.z)),
                                                  std::atan2(diff.x, diff.z), 0.0f});
                marker->SetQuaternionMaster(false);

                float pulse = std::sin(pullTimer_ * 72.0f);
                float tension = pulse * (1.0f - progress * 0.35f) * 0.22f;
                float thickness = engine::Math::Lerp(0.30f, 0.12f, progress);
                marker->SetScale({(std::max)(0.06f, thickness + tension),
                                                 (std::max)(0.06f, thickness - tension * 0.55f),
                                                 length * (1.0f + std::abs(pulse) * 0.018f)});
                marker->SetColor({0.10f, 0.82f + progress * 0.06f, 1.0f, 1.0f});
                marker->UpdateLocalMatrix();
                marker->UpdateWorldMatrix();
            }

            // 鞭表示が残っていれば、巨大スライムの分裂動作で移動した接続点に合わせる。
            if (whipVisual_) whipVisual_->UpdatePull(player, hookAttachmentPos, deltaTime);
            // このフレームで分裂が成立したら、プレイヤーの位置と鞭表示を戻して拘束を終える。
            if (hasSplit) {
                // 踏ん張り前の位置を保存済みなら、通常状態へ移る前に移動を戻す。
                if (hasHeavyPullBasePlayerPos_) {
                    player->SetTranslate(heavyPullBasePlayerPos_);
                    player->UpdateLocalMatrix();
                    player->UpdateWorldMatrix();
                }

                // 曲線の鞭を使っていた場合は、拘束終了後の巻き戻しへ引き継ぐ。
                if (whipVisual_) player->BeginWhipReturn(whipVisual_);
                // 曲線表示を持たない場合は、プレイヤー側の反動を出す。
                else player->TriggerSlimeImpulse({2.4f, 0.45f, 2.4f}, 0.24f);
                player->ChangeState(std::make_unique<PlayerStateIdle>());
            }

            return;
        }

        // 通常の敵はここから手元へ運び、分裂用の進捗は使いません。
        const float kPullDuration = 0.62f; // 反動を見せた後、運搬位置に到着するまでの秒数。
        float t = pullTimer_ / kPullDuration;
        // 引き寄せ時間を過ぎた場合は、位置と大きさの補間を終点で止める。
        if (t > 1.0f)
            t = 1.0f;

        player->SetSlimePullDirection(enemyStartPos_ - playerPos);
        player->SetSlimePullProgress(t);

        // 通常の引き寄せ開始位置は初回に保存し、各フレームの踏ん張りの基点として使う。
        if (!hasNormalPullBasePlayerPos_) {
            normalPullBasePlayerPos_ = playerPos;
            hasNormalPullBasePlayerPos_ = true;
        }

        // 踏ん張りは敵と開始位置を結ぶ水平線に沿わせ、元の高さは保ちます。
        engine::Vector3 flatPullDir = enemyStartPos_ - normalPullBasePlayerPos_;
        flatPullDir.y = 0.0f;
        float flatPullLength = engine::Math::Length(flatPullDir);
        flatPullDir = flatPullLength > 0.001f ? flatPullDir / flatPullLength : engine::Vector3{0.0f, 0.0f, 1.0f};
        const float bracePulse = std::abs(std::sin(pullTimer_ * 36.0f));
        const float yankWave = (std::max)(0.0f, std::sin(pullTimer_ * 22.0f));
        const float yankPulse = yankWave * yankWave * (1.0f - t * 0.35f);
        const float braceAmount = (0.20f + bracePulse * 0.24f + yankPulse * 0.28f) * (1.0f - t * 0.45f);
        engine::Vector3 bracedPlayerPos = normalPullBasePlayerPos_ - flatPullDir * braceAmount;
        bracedPlayerPos.y = normalPullBasePlayerPos_.y;
        player->SetTranslate(bracedPlayerPos);
        player->UpdateLocalMatrix();
        player->UpdateWorldMatrix();
        playerPos = bracedPlayerPos;

        // 進行率を三乗し、初動は遅く、捕獲直前に速くなる引き寄せを作る。
        float easeT = t * t * t;

        // 前回のオイラー角へ、各軸の時間刻みに応じた回転を足す。
        engine::Vector3 rot = targetEnemy_->GetTransform()->rotate;
        rot.x += 20.0f * deltaTime;
        rot.y += 35.0f * deltaTime;
        rot.z += 15.0f * deltaTime;
        targetEnemy_->SetEulerAngles(rot);
        // この演出で設定したオイラー角を、モデルの回転へ反映する。
        targetEnemy_->SetQuaternionMaster(false);

        // 捕獲時の大きさを基準に、頭上へ近づくほど82%まで縮める。
        const float scaleRate = engine::Math::Lerp(1.0f, 0.82f, easeT);
        targetEnemy_->SetScale({enemyBaseScale_.x * scaleRate, enemyBaseScale_.y * scaleRate,
                                               enemyBaseScale_.z * scaleRate});

        // 最終位置は、そのフレームで踏ん張ったプレイヤーの頭上1.55単位。
        engine::Vector3 headPos = {playerPos.x, playerPos.y + 1.55f, playerPos.z};

        // 着弾点と捕獲位置の間を、加速させた進行率で補間する。
        engine::Vector3 basePos = {engine::Math::Lerp(enemyStartPos_.x, headPos.x, easeT),
                           engine::Math::Lerp(enemyStartPos_.y, headPos.y, easeT),
                           engine::Math::Lerp(enemyStartPos_.z, headPos.z, easeT)};

        // 移動速度を決める Ease-In と高さを分け、補間前の進行率で正弦波の弧を加える。
        float arcHeight = 7.8f;
        basePos.y += std::sin(t * std::numbers::pi_v<float>) * arcHeight;

        targetEnemy_->SetTranslate(basePos);
        player->SetSlimePullDirection(basePos - playerPos);

        // 通常の敵が手元へ届いたら踏ん張り前の位置を戻し、運搬側へ敵を引き渡します。
        if (t >= 1.0f) {
            HitEffectDirector::SpawnPullCatchHit(headPos);

            // CPU粒子を使える場合は、通常の敵を受け止めた位置に飛沫を出す。
            if (player->GetParticleSystem()) {
                engine::Vector3 headPos = {playerPos.x, playerPos.y + 1.55f, playerPos.z};
                player->GetParticleSystem()->SpawnParticles(headPos, 20, 1.5f, nullptr, 20.0f,
                                                            {0.55f, 0.95f, 1.0f, 1.0f}, {0.25f, 0.75f, 1.0f, 0.0f},
                                                            0.2f, 0.4f, 0.6f, 0.05f);
            }

            // 引き継いだ鞭表示は、通常の捕獲後も巻き戻しまで使う。
            if (whipVisual_) player->BeginWhipReturn(whipVisual_);
            // 鞭の曲線表示がない場合は、プレイヤーの短い反動で捕獲を示す。
            else player->TriggerSlimeImpulse({2.2f, 0.4f, 2.2f}, 0.24f);

            // 踏ん張り前の位置を保存していれば、敵を運搬へ渡す前に戻す。
            if (hasNormalPullBasePlayerPos_) {
                player->SetTranslate(normalPullBasePlayerPos_);
                player->UpdateLocalMatrix();
                player->UpdateWorldMatrix();
            }

            targetEnemy_->SetScale(enemyBaseScale_);
            player->SetCarriedEnemy(targetEnemy_);
            player->ChangeState(std::make_unique<PlayerStateIdle>());
            return;
        }

        engine::Object3D* marker = player->GetHookMarker();

        // 簡易表示を使う場合は、引き寄せ中の敵の座標へ線の長さと向きをそろえる。
        if (marker && !whipVisual_) {
            engine::Vector3 enemyCurrentPos = targetEnemy_->GetTransform()->translate;
            engine::Vector3 diff = enemyCurrentPos - playerPos;
            marker->SetTranslate({playerPos.x + diff.x * 0.5f, playerPos.y + diff.y * 0.5f,
                                                 playerPos.z + diff.z * 0.5f});
            float angleY = std::atan2(diff.x, diff.z);
            float angleX = std::atan2(-diff.y, std::sqrt(diff.x * diff.x + diff.z * diff.z));
            marker->SetEulerAngles({angleX, angleY, 0.0f});
            marker->SetQuaternionMaster(false);

            // 捕獲位置へ近づくほど、簡易フックの横幅を通常の太さへ戻す。
            float length = engine::Math::Length(diff);
            float thickness = engine::Math::Lerp(0.12f, 0.22f, easeT);

            // 引き寄せ初期の線の揺れを、捕獲へ近づくにつれて小さくする。
            float wobble = std::sin(pullTimer_ * 58.0f) * (1.0f - easeT) * 0.050f;
            float tensionPulse = std::abs(std::sin(pullTimer_ * 44.0f)) * (1.0f - easeT) * 0.040f;
            marker->SetScale({(std::max)(0.09f, thickness + wobble + tensionPulse),
                                             (std::max)(0.09f, thickness - wobble * 0.55f),
                                             length * (1.0f + tensionPulse * 0.35f)});
            marker->SetColor({0.10f, 0.82f, 1.0f, 1.0f});

            marker->UpdateLocalMatrix();
            marker->UpdateWorldMatrix();
        }
        // 回転するモデルの中心へ同フレームで追従し、足元の原点から先端が浮くのを防ぎます。
        if (whipVisual_) {
            targetEnemy_->UpdateLocalMatrix();
            targetEnemy_->UpdateWorldMatrix();
            whipVisual_->UpdatePull(player, GetWhipPullAnchor(targetEnemy_), deltaTime);
        }
    }
}

void PlayerStatePullEnemy::Exit(Player* player) {
    // 対象の状態を復元する前に借用参照を確認し、プレイヤーへ操作権を返す。
    const auto& registered = engine::CollisionManager::GetInstance()->GetObjects();
    // 対象が衝突管理に残っていなければ、終了処理で参照しないよう解除する。
    if (targetEnemy_ && std::find(registered.begin(), registered.end(), targetEnemy_) == registered.end())
        targetEnemy_ = nullptr;

    // プレイヤーの参照がない場合は、終了時の操作と表示を変更できない。
    if (!player) {
        return;
    }

    // 鞭表示の参照があれば Finish() に渡す。帰還を予約済みのときは Finish() が表示を保持する。
    if (whipVisual_) { whipVisual_->Finish(player); whipVisual_.reset(); }
    player->SetIsControlActive(true);
    {
        // 運搬に渡す前に中断した敵だけ、拘束を外して開始時の姿勢へ戻します。
        if (targetEnemy_ && player->GetCarriedEnemy() != targetEnemy_) {
            // 中断した対象が敵クラスなら、拘束中に使った運搬フラグを確認する。
            if (auto* enemyBase = dynamic_cast<BaseEnemy*>(targetEnemy_)) {
                // この拘束で運搬扱いになっていれば、運搬を解除し開始時の姿勢や衝突設定を戻す。
                if (enemyBase->IsCarried()) {
                    enemyBase->SetCarried(false);
                    {
                        targetEnemy_->SetScale(enemyBaseScale_);
                        targetEnemy_->SetEulerAngles(enemyBaseRotation_);
                        targetEnemy_->SetQuaternion(enemyBaseQuaternion_);
                        targetEnemy_->SetQuaternionMaster(enemyBaseQuaternionMaster_);
                        targetEnemy_->UpdateLocalMatrix();
                        targetEnemy_->UpdateWorldMatrix();
                    }
                }
            }
        }

        // 踏ん張りで動かしたプレイヤーの位置を戻し、分裂が未成立なら巨大スライムの抵抗演出も取り消します。
        if (isHeavyPullTarget_) {
            // 巨大スライム相手に踏ん張り位置を保存していれば、プレイヤーをその位置へ戻す。
            if (hasHeavyPullBasePlayerPos_) {
                player->SetTranslate(heavyPullBasePlayerPos_);
                player->UpdateLocalMatrix();
                player->UpdateWorldMatrix();
            }

            // 終了時も巨大スライムの実体が残る場合は、その分裂状態を確認する。
            if (auto* giantSlime = dynamic_cast<EnemyGiantSlime*>(targetEnemy_)) {
                // まだ分裂していない場合は、拘束の途中で始めた抵抗動作を取り消す。
                if (!giantSlime->HasSplit()) {
                    giantSlime->CancelHookSplitPull();
                }
            }
        // 通常の敵の引き寄せが中断され、開始位置を控えていれば、プレイヤーをそこへ戻す。
        } else if (hasNormalPullBasePlayerPos_) {
            player->SetTranslate(normalPullBasePlayerPos_);
            player->UpdateLocalMatrix();
            player->UpdateWorldMatrix();
        }

        engine::Object3D* marker = player->GetHookMarker();

        // マーカーが残っている場合は、巻き戻し用の曲線とは別に、非表示と基準姿勢へ戻す。
        if (marker && !whipVisual_) {
            marker->SetIsVisible(false);
            marker->SetScale({1.0f, 1.0f, 1.0f});
            marker->SetEulerAngles({0.0f, 0.0f, 0.0f});
            marker->SetQuaternionMaster(true); // 回転モードの復帰
            marker->UpdateLocalMatrix();
            marker->UpdateWorldMatrix();
        }
    }
}
