#define NOMINMAX
#include <cstdint>
#include "game/actor/player/animation/PlayerSlimeAnimator.h"
#include "Easing.h"
#include <numbers>
#include "game/actor/player/Player.h"
#include "game/actor/player/PlayerDefaults.h"
#include "VfxSequencer.h"
#include "InputManager.h"
#include "engine/utility/math/AnimationInterpolation.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kModeTransitionBlendDuration = 0.12f;
constexpr float kJumpTakeoffStretchDuration = 0.18f;
constexpr float kLandingHoldDuration = 0.08f;
constexpr float kLandingSquashDuration = 0.26f;
constexpr float kLandingReboundDuration = 0.32f;
constexpr float kGateReturnSlimeDuration = 1.12f;
constexpr const char* kPlayerSlimeControllerPath = "Resources/json/animator/player_slime.json";
constexpr const char* kLocomotionModeParameter = "LocomotionMode";



float HorizontalLength(const engine::Vector3& v) {
    // ジャンプ速度に影響されず走行アニメーションを選ぶため、XZ成分だけを測る。
    return std::sqrt(v.x * v.x + v.z * v.z);
}

} // namespace

void PlayerSlimeAnimator::Reset(const engine::Vector3& baseScale, Player* player) {
    // モード・着地反応・能力モーションを同じ初期姿勢へ戻す。
    // コントローラーが未読込なら、初期状態を指定する前に読み込みを試す。
    if (!controllerLoaded_) {
        ReloadController();
    }

    // 前回の鞭、移動履歴、一時変形、着地の時計を消して初回更新に備える。
    ClearWhipPose();
    baseScale_ = baseScale;
    mode_ = Mode::kIdle;
    modeTimer_ = 0.0f;
    runMotionTime_ = 0.0f;
    runSpeedRate_ = 0.0f;
    previousPosition_ = player ? player->GetTranslate() : engine::Vector3{};
    hasPositionHistory_ = player != nullptr;
    jumpChargeRate_ = 0.0f;
    pullProgress_ = 0.0f;
    abilityPose_ = AbilityPose::kNone;
    abilityPoseStrength_ = 0.0f;
    impulseTimer_ = 0.0f;
    impulseDuration_ = 0.0f;
    impulseScale_ = baseScale_;
    ClearAbilityMotion(player);
    modeTransitionTimer_ = 0.0f;
    modeTransitionCapturePending_ = false;
    modeTransitionStartScale_ = baseScale_;
    modeTransitionStartRotation_ = {};
    jumpTakeoffStretchTimer_ = 0.0f;
    landingHoldTimer_ = 0.0f;
    landingSquashTimer_ = 0.0f;
    landingReboundTimer_ = 0.0f;
    landingImpactRate_ = 1.0f;

    // コントローラーを使える場合は、待機状態から再生できるようにする。
    if (controllerLoaded_) {
        controllerRuntime_.SetController(&controllerAsset_, true);
        controllerRuntime_.Play(GetStateName(Mode::kIdle));

        // モードによる遷移を持つ素材の場合は、初期モードも整数パラメーターに入れる。
        if (useModeTransitions_) {
            controllerRuntime_.SetInt(kLocomotionModeParameter, static_cast<std::int32_t>(mode_));
        }
    }
}

void PlayerSlimeAnimator::SetMode(Mode mode) {
    // 同じ状態が毎フレーム通知されても、動作時刻や遷移をリセットしません。
    if (mode_ == mode) {
        return;
    }

    // 走行への切り替えでは、実移動で進めるクリップを先頭から再開する。
    if (mode == Mode::kRun) {
        runMotionTime_ = 0.0f;
    }

    // 素材が利用できれば、素材の状態と設定済みのブレンドを使う。
    if (controllerLoaded_) {
        if (useModeTransitions_ && mode != Mode::kDisabled) {
            // ゲームの行動状態だけを渡し、遷移先と補間時間はController Assetで決めます。
            controllerRuntime_.SetInt(kLocomotionModeParameter, static_cast<std::int32_t>(mode));
        } else {
            // Disabled中はUpdateしないため、ここで切り替えます。旧Assetも明示切替を維持します。
            controllerRuntime_.CrossFade(GetStateName(mode));
        }

        modeTransitionTimer_ = 0.0f;
    // 素材が使えない場合は、コード側の短い補間時間を予約する。
    } else {
        modeTransitionTimer_ = kModeTransitionBlendDuration;
    }

    // 次回更新で切り替え前の姿勢を退避し、遷移の始点に使います。
    modeTransitionCapturePending_ = true;
    mode_ = mode;
    modeTimer_ = 0.0f;
    jumpChargeRate_ = 0.0f;
}

void PlayerSlimeAnimator::SetMotionDirection(const engine::Vector3& direction) {
    // 縦方向を除いた単位ベクトルを保持し、停止時の向きにも再利用する。
    motionDirection_ = NormalizeOrForward(direction);
}

void PlayerSlimeAnimator::SetPullDirection(const engine::Vector3& direction) {
    // フックの方向は水平面で正規化し、引き寄せ姿勢の向きに使う。
    pullDirection_ = NormalizeOrForward(direction);
}

void PlayerSlimeAnimator::SetPullProgress(float progress) {
    // 開始から完了までの区間に制限し、引き寄せ姿勢の外挿を防ぐ。
    pullProgress_ = std::clamp(progress, 0.0f, 1.0f);
}

void PlayerSlimeAnimator::SetJumpCharge(float chargeRate) {
    // 溜め姿勢の強度は0〜1の割合だけ受け付ける。
    jumpChargeRate_ = std::clamp(chargeRate, 0.0f, 1.0f);
}

// 踏み切り時の伸びを次回の姿勢更新から一定秒数だけ適用する。
void PlayerSlimeAnimator::TriggerJumpTakeoff() {
    jumpTakeoffStretchTimer_ = kJumpTakeoffStretchDuration;
}

void PlayerSlimeAnimator::TriggerLanding(float impactRate) {
    // 衝撃がない接地では反応を始めず、着地の保持・潰れ・反発を順に再生する時間を設定します。
    if (impactRate <= 0.0f) {
        return;
    }
    landingImpactRate_ = std::clamp(impactRate, 0.0f, 1.65f);
    landingHoldTimer_ = kLandingHoldDuration;
    landingSquashTimer_ = kLandingSquashDuration;
    landingReboundTimer_ = kLandingReboundDuration;
}

void PlayerSlimeAnimator::SetAbilityPose(AbilityPose pose, float strength) {
    // 姿勢の寄与率は0〜1に収め、基本動作を過剰に変形させない。
    abilityPose_ = pose;
    abilityPoseStrength_ = std::clamp(strength, 0.0f, 1.0f);
}

void PlayerSlimeAnimator::TriggerImpulse(const engine::Vector3& scale, float duration) {
    // 短い入力でもゼロ秒の衝撃にならないよう、最小継続時間を設ける。
    impulseScale_ = scale;
    impulseDuration_ = (std::max)(duration, 0.01f);
    impulseTimer_ = impulseDuration_;
}

bool PlayerSlimeAnimator::PlayAbilityMotion(const std::string& clipName, bool loop, float playbackSpeed,
                                            float blendInDuration, float blendOutDuration) {
    // 再生するクリップの名前が空なら、現在の能力モーションに触れず失敗を返す。
    if (clipName.empty()) {
        return false;
    }

    // 呼び出し側の名前をState IDとして扱います。Body Clipだけを差し替えても攻撃ロジックの分岐名は維持します。
    const AnimatorStateDefinition* state = controllerLoaded_ ? controllerAsset_.FindState(clipName) : nullptr;
    const std::string& bodyClipName = state && !state->bodyClipName.empty() ? state->bodyClipName : clipName;
    const float assetSpeed = state ? (std::max)(state->speed, 0.01f) : 1.0f;
    const float effectiveSpeed = (std::max)(playbackSpeed * assetSpeed, 0.01f);
    // 同じ能力クリップを続けて要求された場合は、先頭へ巻き戻さず再生速度とループ指定だけ変えます。
    if (abilityMotionActive_ && abilityMotionName_ == clipName &&
        abilityMotionClipAssetName_ == bodyClipName && !abilityMotionStopping_) {
        abilityMotionLoop_ = loop;
        abilityMotionPlaybackSpeed_ = effectiveSpeed;
        return true;
    }

    BodyAnimationClip clip;

    // 新しいクリップを読み込めなければ、現在の再生内容を保持する。
    if (!clip.Load(BodyAnimationClip::ResolveAssetPath(bodyClipName))) {
        return false;
    }

    // 新しい素材を読み込めた時点で現在のクリップを差し替え、開始・終了時に使う補間時間を設定します。
    abilityMotionClip_ = std::move(clip);
    abilityMotionName_ = clipName;
    abilityMotionClipAssetName_ = bodyClipName;
    abilityMotionTimer_ = 0.0f;
    abilityMotionPlaybackSpeed_ = effectiveSpeed;
    abilityMotionBlendInDuration_ = (std::max)(blendInDuration, 0.01f);
    abilityMotionBlendOutDuration_ = (std::max)(blendOutDuration, 0.01f);
    abilityMotionStopTimer_ = 0.0f;
    abilityMotionStopDuration_ = 0.0f;
    abilityMotionActive_ = true;
    abilityMotionLoop_ = loop;
    abilityMotionStopping_ = false;
    return true;
}

void PlayerSlimeAnimator::StopAbilityMotion(float blendOutDuration) {
    // すぐにClipを捨てず、指定秒数で現在の姿勢から戻す。
    // 能力のクリップが動いていない場合は、終了ブレンドを始めない。
    if (!abilityMotionActive_) {
        return;
    }

    abilityMotionStopDuration_ = (std::max)(blendOutDuration, 0.01f);
    abilityMotionStopTimer_ = abilityMotionStopDuration_;
    abilityMotionStopping_ = true;
}

void PlayerSlimeAnimator::ClearAbilityMotion(Player* player) {
    // 能力Clipの状態とMeshRenderer側の表示変形をまとめて初期化する。
    abilityMotionClip_.Clear();
    abilityMotionName_.clear();
    abilityMotionClipAssetName_.clear();
    abilityMotionTimer_ = 0.0f;
    abilityMotionStopTimer_ = 0.0f;
    abilityMotionStopDuration_ = 0.0f;
    abilityMotionActive_ = false;
    abilityMotionLoop_ = false;
    abilityMotionStopping_ = false;

    // 対象と描画担当が揃っていれば、前のクリップの描画変形をその場で戻す。
    if (player && player->GetMeshRenderer()) {
        player->GetMeshRenderer()->ResetVisualTransform();
    }
}

void PlayerSlimeAnimator::Update(Player* player, float deltaTime) {
    // 対象なし・時間進行なし・無効モードの間は姿勢を変更しない。
    if (!player || deltaTime <= 0.0f || mode_ == Mode::kDisabled) {
        return;
    }

    // 状態が切り替わった直後の姿勢を一度だけ保存し、新しい姿勢への補間をここから始めます。
    if (modeTransitionCapturePending_) {
        modeTransitionStartScale_ = player->GetScale();
        modeTransitionStartRotation_ = player->GetRotation();
        modeTransitionCapturePending_ = false;
    }

    // コントローラーを使用できるときだけ、素材が持つ遷移とイベントを更新する。
    if (controllerLoaded_) {
        if (useModeTransitions_) {
            // PreviewなどがParameterを変更しても、通常プレイではゲームの現在Modeを正とします。
            controllerRuntime_.SetInt(kLocomotionModeParameter, static_cast<std::int32_t>(mode_));
        }

        // コントローラーを進めてイベントを取り出し、その状態の経過秒数をモードの時計にする。
        controllerRuntime_.Update(deltaTime, [](const AnimatorStateDefinition&) { return 0.0f; });

        for (const AnimatorEventInstance& event : controllerRuntime_.ConsumeEvents()) {
            // 素材からFeedbackCueが届き参照キーもあるとき、プレイヤーに付く単発演出を発生させる。
            if (event.name == "FeedbackCue" && !event.payload.empty()) {
                VfxSequencer::PlayOneShotOnTarget(event.payload, player);
            }
        }

        modeTimer_ = controllerRuntime_.GetStateTime();
    // 素材が無い場合は、コード側でモードの経過時間をフレーム秒数だけ加算する。
    } else {
        modeTimer_ += deltaTime;
    }

    // 地上を走った実移動の距離だけを走行クリップの時刻へ換算し、速度に応じて揺れの強さを変えます。
    const bool isGrounded = player->IsGrounded();
    const engine::Vector3 position = player->GetTranslate();
    const float runBlend = 1.0f - std::exp(-18.0f * deltaTime);
    // 前回位置を持つ接地中の走行だけ、移動距離をクリップの時刻へ積む。
    if (hasPositionHistory_ && mode_ == Mode::kRun && isGrounded) {
        const engine::Vector3 displacement = position - previousPosition_;
        const float horizontalDistance = HorizontalLength(displacement);
        const float referenceSpeed = (std::max)(player->GetMoveSpeed(), 0.01f);
        // ワープや演出明けの座標差は歩行として扱いません。
        if (horizontalDistance <= referenceSpeed * deltaTime * 3.0f + 0.01f) {
            const auto* runState = controllerLoaded_ ? controllerAsset_.FindState(GetStateName(Mode::kRun)) : nullptr;
            const float playbackSpeed = runState ? (std::max)(runState->speed, 0.0f) : 1.0f;
            runMotionTime_ += horizontalDistance / referenceSpeed * playbackSpeed;
            const float speedRate = std::clamp(horizontalDistance / (referenceSpeed * deltaTime), 0.0f, 1.0f);
            runSpeedRate_ = engine::Math::Lerp(runSpeedRate_, speedRate, runBlend);
        // 想定速度から離れすぎた座標差は瞬間移動として扱い、このフレームの走行倍率をゼロにする。
        } else {
            runSpeedRate_ = 0.0f;
        }
    // 接地走行の条件が揃わない場合は、走行の寄与を滑らかにゼロへ戻す。
    } else {
        runSpeedRate_ = engine::Math::Lerp(runSpeedRate_, 0.0f, runBlend);
    }
    previousPosition_ = position;
    hasPositionHistory_ = true;
    const engine::InputManager* copyInput = engine::InputManager::GetInstance();
    const bool interruptCopyPose =
        copyInput && abilityMotionTimer_ > 0.0f &&
        (copyInput->IsActionTriggered("PrimaryAttack") || copyInput->IsActionTriggered("SecondaryAction") ||
         copyInput->IsActionTriggered("Ability"));

    // コピーの見せポーズはジャンプ・ダッシュ・フック・ガードより優先しない。
    // 攻撃クリップへ切替済みなら名前が異なるため解除しない。
    if (abilityMotionActive_ && abilityMotionName_ == "player_copy_acquire" &&
        (!isGrounded || (mode_ != Mode::kIdle && mode_ != Mode::kRun && mode_ != Mode::kCarry) ||
         abilityPose_ != AbilityPose::kNone || interruptCopyPose)) {
        ClearAbilityMotion(player);
    }

    // 着地の保持・潰れ・反発を同時に消費せず、前の段階が終わってから次の時計を進めます。
    // 着地姿勢の保持時間が残っている間は、そのタイマーだけを消費する。
    if (landingHoldTimer_ > 0.0f) {
        landingHoldTimer_ = (std::max)(0.0f, landingHoldTimer_ - deltaTime);
    // 保持が終わった後は、潰れから回復するタイマーを進める。
    } else if (landingSquashTimer_ > 0.0f) {
        landingSquashTimer_ = (std::max)(0.0f, landingSquashTimer_ - deltaTime);
    // 潰れを終えた後に、反動を弱めるタイマーを進める。
    } else if (landingReboundTimer_ > 0.0f) {
        landingReboundTimer_ = (std::max)(0.0f, landingReboundTimer_ - deltaTime);
    }

    // 踏み切りの反応が残っている間は、着地とは別の時計で減らす。
    if (jumpTakeoffStretchTimer_ > 0.0f) {
        jumpTakeoffStretchTimer_ = (std::max)(0.0f, jumpTakeoffStretchTimer_ - deltaTime);
    }

    // 編集した本体クリップを優先し、素材を評価できない状態だけコードの基本姿勢を使います。
    engine::Vector3 targetScale;
    engine::Vector3 targetRotation;
    engine::Vector3 visualOffset{};
    const bool usesAuthoredBodyClip = TryBuildAuthoredBodyPose(player, targetScale, targetRotation, visualOffset);

    // 現在モードの素材を評価できなければ、拡縮と回転をコードの基本姿勢から計算する。
    if (!usesAuthoredBodyClip) {
        targetScale = BuildModeScale(player, deltaTime);
        targetRotation = BuildModeRotation(player);
    // 素材の姿勢があり、着地で潰れる時間が残る間は、素材に追加の拡縮を重ねる。
    } else if (landingSquashTimer_ > 0.0f) {
        const float t = landingSquashTimer_ / kLandingSquashDuration;
        const float squash = std::clamp(t * t * 0.72f * landingImpactRate_, 0.0f, 0.82f);
        targetScale.x *= 1.0f + squash * 1.08f;
        targetScale.y *= (std::max)(0.38f, 1.0f - squash * 0.92f);
        targetScale.z *= 1.0f + squash * 1.08f;
    // 素材の姿勢で潰れが終わってから、反発を減衰しながら重ねる。
    } else if (usesAuthoredBodyClip && landingReboundTimer_ > 0.0f) {
        const float t = 1.0f - (landingReboundTimer_ / kLandingReboundDuration);
        const float damping = std::pow(1.0f - t, 1.25f);
        const float rebound = std::sin(t * kPi * 2.5f) * damping * 0.20f * landingImpactRate_;
        targetScale.x *= 1.0f - rebound * 0.45f;
        targetScale.y *= 1.0f + rebound;
        targetScale.z *= 1.0f - rebound * 0.45f;
    }

    // 一時的な衝撃変形を重ね、残り時間が短くなるにつれて基本姿勢へ戻します。
    if (impulseTimer_ > 0.0f) {
        impulseTimer_ = (std::max)(0.0f, impulseTimer_ - deltaTime);
        const float t = impulseTimer_ / (std::max)(impulseDuration_, 0.01f);
        const float rate = Easing::OutQuad01(t);
        targetScale = {engine::Math::Lerp(targetScale.x, impulseScale_.x, rate),
                       engine::Math::Lerp(targetScale.y, impulseScale_.y, rate),
                       engine::Math::Lerp(targetScale.z, impulseScale_.z, rate)};
    }

    // ガードと近接は専用キーだけで形を作り、待機の弾み・旧ガード姿勢を二重に重ねません。
    const bool pinkBodyPose = abilityMotionActive_ && (abilityMotionName_ == "player_ability_pink_guard" ||
                                                       abilityMotionName_ == "player_ability_pink_guard_hit" ||
                                                       abilityMotionName_ == "player_ability_pink_elastic_strike" ||
                                                       abilityMotionName_ == "player_ability_pink_combo_return" ||
                                                       abilityMotionName_ == "player_ability_pink_combo_press");
    // ピンクの専用クリップ中は、基準の大きさとYawに戻してからクリップの変形を加える。
    if (pinkBodyPose) {
        targetScale = baseScale_;
        targetRotation = {0.0f, player->GetRotation().y, 0.0f};
        visualOffset = {};
    // 専用クリップが優先されない場合は、基本姿勢へ能力の一時変形を加える。
    } else {
        ApplyAbilityPose(targetScale, targetRotation);
    }

    engine::Vector3 abilityVisualScale{1.0f, 1.0f, 1.0f};
    engine::Vector3 abilityVisualRotation{};
    engine::Vector3 abilityVisualShear{};
    float abilityShearPivotY = 0.0f;
    UpdateAbilityMotion(deltaTime, abilityVisualScale, abilityVisualRotation, visualOffset, abilityVisualShear,
                        abilityShearPivotY);
    // 鞭は物理Transformを潰さず、描画だけで溜め・ひねり・振り抜きを表現します。
    if (whipPoseActive_ && (whipReturnPoseActive_ || mode_ == Mode::kShootHook || mode_ == Mode::kPullEnemy)) {
        targetScale = baseScale_;
        targetRotation = {0.0f, player->GetRotation().y, 0.0f};
        abilityVisualScale = whipPose_.scale;
        abilityVisualRotation = whipPose_.rotate;
        abilityVisualShear = whipPose_.shear;
        // 足元の中心を支点にし、上半身だけが横へ流れる輪郭にします。
        engine::Vector3 foot{};
        // モデルが取得できる場合は、モデル座標の底面中央を変形の支点にする。
        if (player->GetModel()) {
            foot = player->GetModel()->GetCenter();
            foot.y -= player->GetModel()->GetSize().y * 0.5f;
        }
        abilityShearPivotY = foot.y;
        const auto affine = engine::Math::MakeAffineMatrix(abilityVisualScale, abilityVisualRotation, {});
        visualOffset = foot - engine::Math::Transform(foot, affine);
    }
    // 能力クリップと鞭の最終姿勢を描画側へ渡し、位置オフセットやせん断を当たり判定へ反映しません。
    if (player->GetMeshRenderer()) {
        player->GetMeshRenderer()->SetVisualTransform(abilityVisualScale, abilityVisualRotation, visualOffset);
        player->GetMeshRenderer()->SetVisualShear(abilityVisualShear, abilityShearPivotY);
    }

    // 能力側が毎フレーム更新した時だけ専用姿勢を維持し、解除後の姿勢残りを防ぎます。
    abilityPose_ = AbilityPose::kNone;
    abilityPoseStrength_ = 0.0f;

    const bool landingActive = landingSquashTimer_ > 0.0f || landingReboundTimer_ > 0.0f;
    const bool controllerTransition = controllerLoaded_ && controllerRuntime_.IsTransitioning();

    // 状態遷移中は開始姿勢から補間し、通常更新では着地や踏み切りの強さに応じて追従速度を変えます。
    if (controllerTransition || modeTransitionTimer_ > 0.0f) {
        float blend = controllerTransition ? controllerRuntime_.GetTransitionWeight() : 1.0f;

        // 素材の遷移ではない場合に限り、コード側タイマーから補間重みを求める。
        if (!controllerTransition) {
            const float progress = 1.0f - modeTransitionTimer_ / kModeTransitionBlendDuration;
            blend = animationInterpolation::ApplyEasing(progress, animationInterpolation::EasingType::kSmootherStep);
        }

        player->SetScale(animationInterpolation::Lerp(modeTransitionStartScale_, targetScale, blend));

        // スライムの傾きは小さいEuler角なので軸ごとに補間します。
        // クォータニオンからEuler角へ戻す際のY=180度付近の折り返しで、
        // 移動方向として使うY回転が0度側へ化けることを防ぎます。
        player->SetRotation(animationInterpolation::LerpEulerAxes(modeTransitionStartRotation_, targetRotation, blend));

        // 素材は自身が時間を管理するため、コード側の遷移時間を使った場合だけ減らす。
        if (!controllerTransition) {
            modeTransitionTimer_ = (std::max)(0.0f, modeTransitionTimer_ - deltaTime);
        }
    // 状態の切り替え中でないときは、現在姿勢から時間に応じて目標へ近づける。
    } else {
        float scaleFollowSpeed = 15.0f;

        // 着地の変形は短く目立たせるため、通常より優先して速く追従させる。
        if (landingActive) {
            scaleFollowSpeed = 92.0f;
        // 着地の反応が無い踏み切り中は、上へ伸びる反応をすぐ反映する。
        } else if (jumpTakeoffStretchTimer_ > 0.0f) {
            scaleFollowSpeed = 42.0f;
        // 短時間の反応がなくジャンプ中なら、通常より少し速く空中姿勢へ近づける。
        } else if (mode_ == Mode::kJump) {
            scaleFollowSpeed = 20.0f;
        }

        player->SetScale(animationInterpolation::Damp(player->GetScale(), targetScale, scaleFollowSpeed, deltaTime));
        player->SetRotation(
            animationInterpolation::DampEulerAxes(player->GetRotation(), targetRotation, 13.0f, deltaTime));
    }

    // 次に描画・位置を参照する処理へ、更新後の変換行列を渡す。
    player->UpdateLocalMatrix();
    player->UpdateWorldMatrix();
}

void PlayerSlimeAnimator::ApplyAbilityPose(engine::Vector3& scale, engine::Vector3& rotation) const {
    // ガードなどの一時姿勢を基本モードの姿勢へ強度付きで重ねる。
    const float strength = std::clamp(abilityPoseStrength_, 0.0f, 1.0f);

    // 一時姿勢が指定されていないか強度が0なら、基本姿勢へ何も加えない。
    if (abilityPose_ == AbilityPose::kNone || strength <= 0.0f) {
        return;
    }

    switch (abilityPose_) {
    // ガードは胴を低く広げ、細かな揺れで踏ん張りを見せる。
    case AbilityPose::kGuard: {
        const float bracePulse = std::sin(modeTimer_ * 17.0f) * 0.035f * strength;
        const float shellWobble = std::sin(modeTimer_ * 8.5f) * 0.022f * strength;
        scale.x *= 1.0f + (0.34f + bracePulse) * strength;
        scale.y *= 1.0f - (0.30f + std::abs(bracePulse) * 0.55f) * strength;
        scale.z *= 1.0f + (0.30f - bracePulse * 0.55f) * strength;
        rotation.x *= 1.0f - strength * 0.82f;
        rotation.z = engine::Math::Lerp(rotation.z, shellWobble, strength);
        break;
    }

    // 炎ダッシュは体を進行方向へ伸ばし、速い揺れと移動方向への傾きを足す。
    case AbilityPose::kBlazeDash: {
        const float flameFlutter = std::sin(modeTimer_ * 54.0f) * 0.045f * strength;
        scale.x *= 1.0f - (0.18f - flameFlutter) * strength;
        scale.y *= 1.0f - (0.22f + std::abs(flameFlutter) * 0.45f) * strength;
        scale.z *= 1.0f + (0.38f - flameFlutter) * strength;
        rotation.x += -motionDirection_.z * 0.13f * strength;
        rotation.z += motionDirection_.x * 0.13f * strength;
        break;
    }

    // 落下を遅らせる姿勢は横に広がり、傘のようにゆっくり揺らす。
    case AbilityPose::kSlowFall: {
        const float canopyWave = std::sin(modeTimer_ * 9.0f) * 0.055f * strength;
        const float sideWave = std::cos(modeTimer_ * 6.5f) * 0.035f * strength;
        scale.x *= 1.0f + (0.34f + canopyWave) * strength;
        scale.y *= 1.0f - (0.28f + std::abs(canopyWave) * 0.35f) * strength;
        scale.z *= 1.0f + (0.31f - canopyWave * 0.65f) * strength;
        rotation.x = engine::Math::Lerp(rotation.x, canopyWave * 0.65f, strength);
        rotation.z = engine::Math::Lerp(rotation.z, sideWave, strength);
        break;
    }

    // 能力姿勢が無いか未対応なら基本姿勢を保つ。
    case AbilityPose::kNone:
    default:
        break;
    }
}

void PlayerSlimeAnimator::UpdateAbilityMotion(float deltaTime, engine::Vector3& visualScale, engine::Vector3& visualRotation,
                                              engine::Vector3& visualOffset, engine::Vector3& visualShear, float& shearPivotY) {
    // Clipを評価した後に開始・終了時の重みを掛け、基本姿勢へ滑らかに戻す。
    // クリップが停止中または無効なら、呼び出し側の姿勢をそのまま使う。
    if (!abilityMotionActive_ || !abilityMotionClip_.IsValid()) {
        return;
    }

    abilityMotionTimer_ += deltaTime * abilityMotionPlaybackSpeed_;
    const float clipDuration = abilityMotionClip_.GetDuration();
    BodyAnimationClip::Sample sample;

    // 指定時刻を評価できない素材は、描画へ古いキーを渡さず終了します。
    if (!abilityMotionClip_.Evaluate(abilityMotionTimer_, abilityMotionLoop_, sample)) {
        ClearAbilityMotion();
        return;
    }

    float weight = std::clamp(abilityMotionTimer_ / abilityMotionBlendInDuration_, 0.0f, 1.0f);

    // 明示停止は停止要求からの実時間で弱め、通常の非ループ再生はクリップ終端までの残り秒で弱めます。
    if (abilityMotionStopping_) {
        abilityMotionStopTimer_ = (std::max)(0.0f, abilityMotionStopTimer_ - deltaTime);
        weight *= abilityMotionStopTimer_ / (std::max)(abilityMotionStopDuration_, 0.01f);
    // 明示停止でない単発クリップは終端へ向けて薄める。ガード被弾だけはその自然減衰を省く。
    } else if (!abilityMotionLoop_ && abilityMotionName_ != "player_ability_pink_guard_hit") {
        const float remainingSeconds =
            (std::max)(0.0f, clipDuration - abilityMotionTimer_) / (std::max)(abilityMotionPlaybackSpeed_, 0.01f);
        weight *= std::clamp(remainingSeconds / abilityMotionBlendOutDuration_, 0.0f, 1.0f);
    }

    // 補間済みの拡縮・回転・位置・せん断を描画用の参照引数へ書き込みます。
    visualScale = {engine::Math::Lerp(1.0f, sample.scale.x, weight), engine::Math::Lerp(1.0f, sample.scale.y, weight),
                   engine::Math::Lerp(1.0f, sample.scale.z, weight)};
    visualRotation = sample.rotate * weight;
    visualOffset += sample.translate * weight;
    visualShear = sample.shear * weight;
    shearPivotY = sample.shearPivotY;

    // 明示停止の残り時間がゼロ、または非ループの末尾に達したときは次回へ持ち越さない。
    if ((abilityMotionStopping_ && abilityMotionStopTimer_ <= 0.0f) ||
        (!abilityMotionLoop_ && abilityMotionTimer_ >= clipDuration)) {
        ClearAbilityMotion();
    }
}

bool PlayerSlimeAnimator::ReloadController() {
    // Controllerを読めない場合は遷移を無効にし、古いClipを残さない。
    AnimatorControllerAsset asset;

    // 素材の読み込みに失敗した場合は、以前の遷移やクリップを保持せずに手続き姿勢へ戻す。
    if (!asset.Load(kPlayerSlimeControllerPath)) {
        controllerLoaded_ = false;
        useModeTransitions_ = false;
        controllerRuntime_.SetController(nullptr, false);
        bodyClips_.clear();
        return false;
    }

    controllerAsset_ = std::move(asset);
    controllerLoaded_ = true;
    useModeTransitions_ = std::any_of(controllerAsset_.parameters.begin(), controllerAsset_.parameters.end(),
        [](const AnimatorParameterDefinition& parameter) {
            return parameter.name == kLocomotionModeParameter && parameter.type == AnimatorParameterType::kInt;
        });
    controllerRuntime_.SetController(&controllerAsset_, true);
    controllerRuntime_.Play(GetStateName(mode_));

    // 素材にモード遷移用の整数パラメーターがある場合だけ、現在モードの値を設定する。
    if (useModeTransitions_) {
        controllerRuntime_.SetInt(kLocomotionModeParameter, static_cast<std::int32_t>(mode_));
    }

    ReloadBodyClips();
    return true;
}

void PlayerSlimeAnimator::ReloadBodyClips() {
    // Stateが参照するBody Clipだけを一度ずつ読み込み、重複Assetを共有する。
    bodyClips_.clear();

    // コントローラーが読み込めていなければ、参照する素材の一覧が無い。
    if (!controllerLoaded_) {
        return;
    }

    for (const AnimatorStateDefinition& state : controllerAsset_.states) {
        // 参照名が無い状態と、読み込み済みのクリップ名は処理を省く。
        if (state.bodyClipName.empty() || bodyClips_.contains(state.bodyClipName)) {
            continue;
        }

        BodyAnimationClip clip;
        const std::string path = BodyAnimationClip::ResolveAssetPath(state.bodyClipName);

        // 正常に読み込めた素材だけを保存し、失敗した素材は基本姿勢へ任せる。
        if (clip.Load(path)) {
            bodyClips_.emplace(state.bodyClipName, std::move(clip));
        }
    }
}

bool PlayerSlimeAnimator::TryBuildAuthoredBodyPose(Player* player, engine::Vector3& scaleOut, engine::Vector3& rotationOut,
                                                   engine::Vector3& visualOffsetOut) const {
    // 対象、状態、参照クリップのどれかが欠ける場合は、呼び出し元へ手続き姿勢の使用を任せます。
    if (!player || !controllerLoaded_) {
        return false;
    }

    const AnimatorStateDefinition* state = controllerAsset_.FindState(GetStateName(mode_));

    // 現在モードに対応するStateか、そのBody Clip参照がなければ手続き姿勢へ戻す。
    if (!state || state->bodyClipName.empty()) {
        return false;
    }

    const auto clipIt = bodyClips_.find(state->bodyClipName);

    // 指定された参照先が読み込み済みでない場合は評価を行えない。
    if (clipIt == bodyClips_.end()) {
        return false;
    }

    float sampleTime = mode_ == Mode::kRun ? runMotionTime_ : modeTimer_;

    // ジャンプは時間でループせず、溜め・上昇・下降を鉛直速度からクリップ上の位置へ対応させます。
    if (mode_ == Mode::kJump) {
        // ジャンプの溜め中は、鉛直速度による補間よりクリップの先頭を優先する。
        if (jumpChargeRate_ > 0.0f) {
            sampleTime = 0.0f;
        // 溜めが終わったら、上昇・下降を速度の正負で判定する。
        } else {
            const float jumpPower = (std::max)(player->GetJumpPower(), 0.01f);
            const float verticalVelocity = player->GetVelocity().y;

            // 上昇中は初速が弱まるにつれて離陸用の位置から頂点用の位置へ近づける。
            if (verticalVelocity >= 0.0f) {
                const float riseRate = 1.0f - std::clamp(verticalVelocity / jumpPower, 0.0f, 1.0f);
                sampleTime = animationInterpolation::Lerp(0.12f, 0.48f, riseRate);
            // 下降中は落下速度に応じて頂点から接地準備側の位置へ近づける。
            } else {
                const float fallRate = std::clamp(-verticalVelocity / jumpPower, 0.0f, 1.0f);
                sampleTime = animationInterpolation::Lerp(0.48f, 0.82f, fallRate);
            }
        }
    }

    BodyAnimationClip::Sample sample;

    // 指定時刻のキーを評価できなければ出力せず、呼び出し元に基本姿勢の使用を任せる。
    if (!clipIt->second.Evaluate(sampleTime, mode_ == Mode::kJump ? false : state->loop, sample)) {
        return false;
    }

    // 実移動がほぼない場合は、走行クリップを再生していても拡縮と傾きの寄与を弱めます。
    if (mode_ == Mode::kRun) {
        const float weight = std::clamp(runSpeedRate_, 0.0f, 1.0f);
        sample.scale.x = engine::Math::Lerp(1.0f, sample.scale.x, weight);
        sample.scale.y = engine::Math::Lerp(1.0f, sample.scale.y, weight);
        sample.scale.z = engine::Math::Lerp(1.0f, sample.scale.z, weight);
        sample.translate = sample.translate * weight;
        sample.rotate = sample.rotate * weight;
    }

    scaleOut = {baseScale_.x * sample.scale.x, baseScale_.y * sample.scale.y, baseScale_.z * sample.scale.z};
    const engine::Vector3 proceduralRotation = BuildModeRotation(player);
    rotationOut = {proceduralRotation.x + sample.rotate.x, proceduralRotation.y + sample.rotate.y,
                   proceduralRotation.z + sample.rotate.z};

    // 位置キーは物理座標ではなく描画専用Transformへ渡します。
    visualOffsetOut = sample.translate;
    return true;
}

const char* PlayerSlimeAnimator::GetStateName(Mode mode) {
    // C++の移動モードをController側のState IDへ対応付ける。
    switch (mode) {
    case Mode::kIdle:
        return "Idle";

    case Mode::kRun:
        return "Run";

    case Mode::kJump:
        return "Jump";

    case Mode::kDash:
        return "Dash";

    case Mode::kAimHook:
        return "AimHook";

    case Mode::kShootHook:
        return "ShootHook";

    case Mode::kPullEnemy:
        return "PullEnemy";

    case Mode::kCarry:
        return "Carry";

    case Mode::kGateReturn:
        return "GateReturn";

    case Mode::kDisabled:
        return "Disabled";
    }

    // 未対応のモードを受け取った場合は、コントローラーで確実に参照できる待機名へ戻す。
    return "Idle";
}

engine::Vector3 PlayerSlimeAnimator::NormalizeOrForward(const engine::Vector3& direction) const {
    // 入力方向がほぼゼロでも、向き計算にゼロベクトルを渡さない。
    engine::Vector3 flat = {direction.x, 0.0f, direction.z};
    const float length = HorizontalLength(flat);

    // 水平成分がほぼ無い場合は、ワールドの前方+Zを返す。
    if (length <= 0.001f) {
        return {0.0f, 0.0f, 1.0f};
    }

    return {flat.x / length, 0.0f, flat.z / length};
}

engine::Vector3 PlayerSlimeAnimator::BuildModeScale(Player* player, float deltaTime) const {
    // 速度とモード時計から伸縮を作り、Idle・Run・Jumpなどの質感を分ける。
    (void)deltaTime;
    const engine::Vector3 velocity = player ? player->GetVelocity() : engine::Vector3{};
    const float speed = HorizontalLength(velocity);
    const float moveSpeed = player ? (std::max)(player->GetMoveSpeed(), 0.01f) : 1.0f;
    const float speedRate = std::clamp(speed / moveSpeed, 0.0f, 1.8f);
    engine::Vector3 scale = baseScale_;

    switch (mode_) {
    case Mode::kIdle: {
        // 縦を縮める位相で横を広げ、静止中も柔らかい体積感を保つ。
        const float breath = std::sin(modeTimer_ * 2.6f) * 0.066f;
        const float softPulse = std::sin(modeTimer_ * 5.6f + 0.7f) * 0.024f;
        const float skinRipple = std::sin(modeTimer_ * 10.5f + 1.1f) * 0.010f;
        scale.x = baseScale_.x * (1.0f + breath * 0.78f + softPulse + skinRipple);
        scale.y = baseScale_.y * (1.0f - breath * 1.12f - softPulse * 0.55f - std::abs(skinRipple) * 0.35f);
        scale.z = baseScale_.z * (1.0f + breath * 0.72f - softPulse * 0.38f - skinRipple * 0.60f);
        break;
    }

    case Mode::kRun: {
        // 実移動距離で位相を進め、壁などで止まった時は姿勢も基準形へ戻します。
        const float phase = runMotionTime_ * (10.0f + runSpeedRate_ * 4.5f);
        const float wave = std::sin(phase);
        const float squash = (std::max)(0.0f, -wave) * runSpeedRate_;
        const float stretch = (std::max)(0.0f, wave) * runSpeedRate_;
        const float sideWobble = std::sin(phase * 0.5f) * 0.044f * runSpeedRate_;
        const float delayedRipple = std::sin(phase * 0.35f + 0.8f) * 0.026f * runSpeedRate_;
        scale.x =
            baseScale_.x * (1.0f + 0.055f * runSpeedRate_ + squash * 0.20f - stretch * 0.09f + sideWobble + delayedRipple);
        scale.y = baseScale_.y *
                  (1.0f - 0.040f * runSpeedRate_ - squash * 0.22f + stretch * 0.28f - std::abs(delayedRipple) * 0.38f);
        scale.z = baseScale_.z * (1.0f + 0.060f * runSpeedRate_ + squash * 0.15f - stretch * 0.06f - sideWobble * 0.70f -
                                  delayedRipple * 0.35f);
        break;
    }

    case Mode::kJump: {
        // 速度の絶対値から頂点付近を推定し、溜め・上昇・下降で別の伸縮を作る。
        const float jumpPower = player ? (std::max)(player->GetJumpPower(), 0.01f) : playerDefaults::kJumpPower;
        const float apexRate = 1.0f - std::clamp(std::abs(velocity.y) / (jumpPower * 0.82f), 0.0f, 1.0f);
        const float airWave = std::sin(modeTimer_ * 12.5f);
        const float airRipple = std::sin(modeTimer_ * 21.0f + 0.8f);
        const float apexSquash = apexRate * (0.14f + std::abs(airWave) * 0.070f);
        const float airJelly = airWave * (0.050f + apexRate * 0.090f);
        const float sideJelly = airRipple * (0.034f + apexRate * 0.060f);

        // 溜めがある間は飛ぶ前の圧縮を優先し、溜め率に応じて横へ広がる。
        if (jumpChargeRate_ > 0.0f) {
            const float pulse = std::sin(modeTimer_ * 22.0f) * 0.05f * jumpChargeRate_;
            const float squash = jumpChargeRate_ * 0.95f;
            scale.x = baseScale_.x * (1.0f + squash * 0.34f + pulse);
            scale.y = baseScale_.y * (1.0f - squash * 0.42f - std::abs(pulse) * 0.35f);
            scale.z = baseScale_.z * (1.0f + squash * 0.34f - pulse * 0.65f);
        // 鉛直速度が0以上の間は、上に進む勢いで縦に伸びる。
        } else if (velocity.y >= 0.0f) {
            const float stretch = std::clamp(velocity.y * 0.066f, 0.0f, 0.95f);
            scale.x = baseScale_.x * (1.0f - stretch * 0.32f + apexSquash + sideJelly);
            scale.y = baseScale_.y * (1.0f + stretch * 0.62f - apexSquash * 0.42f + airJelly * 0.72f);
            scale.z = baseScale_.z * (1.0f - stretch * 0.32f + apexSquash - sideJelly * 0.70f);
        // 下降中は着地に備え、落下速度に応じて胴を低く横へ広げる。
        } else {
            const float prepare = std::clamp(-velocity.y * 0.033f, 0.0f, 0.75f);
            const float fallRipple = std::sin(modeTimer_ * 13.5f) * 0.045f * prepare;
            scale.x = baseScale_.x * (1.0f + prepare * 0.42f + apexSquash * 0.65f + fallRipple + sideJelly);
            scale.y = baseScale_.y *
                      (1.0f - prepare * 0.34f - apexSquash * 0.28f - std::abs(fallRipple) * 0.58f + airJelly * 0.55f);
            scale.z =
                baseScale_.z * (1.0f + prepare * 0.42f + apexSquash * 0.65f - fallRipple * 0.65f - sideJelly * 0.70f);
        }

        break;
    }

    // 開始直後の縦方向の押し潰れを強くし、奥行きの伸びと振動で勢いを見せます。
    case Mode::kDash: {
        const float startKick = 1.0f - std::clamp(modeTimer_ / 0.18f, 0.0f, 1.0f);
        const float flutter = std::sin(modeTimer_ * 42.0f) * 0.072f;
        scale.x = baseScale_.x * (0.78f + flutter);
        scale.y = baseScale_.y * (0.68f - startKick * 0.10f - std::abs(flutter) * 0.20f);
        scale.z = baseScale_.z * (1.46f + startKick * 0.25f - flutter);
        break;
    }

    // 照準中は低い姿勢を保ち、止まった輪郭に小さな呼吸を重ねます。
    case Mode::kAimHook: {
        const float breath = std::sin(modeTimer_ * 7.0f) * 0.035f;
        scale.x = baseScale_.x * (1.13f + breath);
        scale.y = baseScale_.y * (0.73f - breath * 0.3f);
        scale.z = baseScale_.z * (1.13f + breath);
        break;
    }

    // 発射直後だけ体を大きく広げ、短い反動を入れます。
    case Mode::kShootHook: {
        const float kick = 1.0f - std::clamp(modeTimer_ / 0.16f, 0.0f, 1.0f);
        scale.x = baseScale_.x * (1.12f + kick * 0.18f);
        scale.y = baseScale_.y * (0.82f - kick * 0.16f);
        scale.z = baseScale_.z * (1.10f + kick * 0.22f);
        break;
    }

    // 引き合いの細かな揺れと強い引きの山を重ね、完了に近づくと一部の揺れを弱めます。
    case Mode::kPullEnemy: {
        const float strainPulse = std::abs(std::sin(modeTimer_ * 32.0f));
        const float tugWave = std::sin(modeTimer_ * 20.0f);
        const float pullSnap = std::sin(modeTimer_ * 46.0f) * 0.030f * (1.0f - pullProgress_);
        const float yankWave = (std::max)(0.0f, std::sin(modeTimer_ * 18.0f));
        const float yankPulse = yankWave * yankWave * (1.0f - pullProgress_ * 0.45f);
        const float strain = (0.22f + strainPulse * 0.18f) * (1.0f - pullProgress_ * 0.18f);
        scale.x = baseScale_.x * (1.0f + strain * 0.20f + tugWave * 0.024f + pullSnap - yankPulse * 0.045f);
        scale.y = baseScale_.y * (1.0f - strain * 0.42f - std::abs(tugWave) * 0.034f + yankPulse * 0.14f);
        scale.z = baseScale_.z * (1.0f + strain * 0.16f - tugWave * 0.016f - pullSnap * 0.46f - yankPulse * 0.045f);
        break;
    }

    // 運搬中は横に広がった形を基準に、小さな上下動と力みを加えます。
    case Mode::kCarry: {
        const float wobble = std::sin(modeTimer_ * 11.0f) * 0.075f;
        const float strain = std::abs(std::sin(modeTimer_ * 5.5f)) * 0.045f;
        scale.x = baseScale_.x * (1.05f + strain - wobble * 0.45f);
        scale.y = baseScale_.y * (0.95f - strain * 0.4f + wobble);
        scale.z = baseScale_.z * (1.05f + strain - wobble * 0.45f);
        break;
    }

    case Mode::kGateReturn: {
        // 出現直後の押し潰れ、伸び、収束を同じ進行率でつなぎ、途中からの再生でも姿勢を再現する。
        const float t = std::clamp(modeTimer_ / kGateReturnSlimeDuration, 0.0f, 1.0f);
        const float squeeze = 1.0f - std::clamp(t / 0.36f, 0.0f, 1.0f);
        const float stretch = std::sin(std::clamp((t - 0.18f) / 0.48f, 0.0f, 1.0f) * kPi);
        const float settle = std::sin(std::clamp((t - 0.55f) / 0.45f, 0.0f, 1.0f) * kPi * 2.0f) * (1.0f - t);

        scale.x = baseScale_.x * (1.0f + squeeze * 0.24f + stretch * 0.08f + settle * 0.045f);
        scale.y = baseScale_.y * (1.0f - squeeze * 0.30f + stretch * 0.18f - settle * 0.060f);
        scale.z = baseScale_.z * (1.0f + squeeze * 0.32f + stretch * 0.10f - settle * 0.035f);
        break;
    }

    // 無効時はモードによる個別の変形を加えない。
    case Mode::kDisabled:
        break;
    }

    // モードの基本姿勢に、踏み切りと着地の短い反応を追加します。
    // 踏み切り直後の反応がある場合は、減っていくタイマーに合わせて縦の伸びを重ねる。
    if (jumpTakeoffStretchTimer_ > 0.0f) {
        const float t = jumpTakeoffStretchTimer_ / kJumpTakeoffStretchDuration;
        const float stretch = t * t * 0.34f;
        scale.x *= 1.0f - stretch * 0.38f;
        scale.y *= 1.0f + stretch * 0.82f;
        scale.z *= 1.0f - stretch * 0.38f;
    }

    // 着地で潰れる時間中は、衝撃の強さに応じて低く広げる。
    if (landingSquashTimer_ > 0.0f) {
        const float t = landingSquashTimer_ / kLandingSquashDuration;
        const float squash = std::clamp(t * t * 0.72f * landingImpactRate_, 0.0f, 0.82f);
        scale.x *= 1.0f + squash * 1.08f;
        scale.y *= (std::max)(0.38f, 1.0f - squash * 0.92f);
        scale.z *= 1.0f + squash * 1.08f;
    // 潰れが終わって反発が残っていれば、上下の弾みを徐々に小さくする。
    } else if (landingReboundTimer_ > 0.0f) {
        const float t = 1.0f - (landingReboundTimer_ / kLandingReboundDuration);
        const float damping = std::pow(1.0f - t, 1.25f);
        const float rebound = std::sin(t * kPi * 2.5f) * damping * 0.20f * landingImpactRate_;
        scale.x *= 1.0f - rebound * 0.45f;
        scale.y *= 1.0f + rebound;
        scale.z *= 1.0f - rebound * 0.45f;
    }

    // 極端な変形が重なっても表示の各軸が潰れ切らないよう下限を設けます。
    scale.x = (std::max)(scale.x, 0.25f);
    scale.y = (std::max)(scale.y, 0.25f);
    scale.z = (std::max)(scale.z, 0.25f);
    return scale;
}

engine::Vector3 PlayerSlimeAnimator::BuildModeRotation(Player* player) const {
    // 移動中は進行方向へ傾け、停止中は小さな揺れだけを残す。
    engine::Vector3 current = player ? player->GetRotation() : engine::Vector3{};
    engine::Vector3 rotation = {0.0f, current.y, 0.0f};
    const engine::Vector3 velocity = player ? player->GetVelocity() : engine::Vector3{};
    const float speed = HorizontalLength(velocity);

    switch (mode_) {
    // 待機中は水平の向きを保ち、小さな前後左右の揺れだけを加える。
    case Mode::kIdle:
        rotation.x = std::sin(modeTimer_ * 2.2f) * 0.014f;
        rotation.z = std::cos(modeTimer_ * 2.0f) * 0.014f;
        break;

    // 走行とダッシュは同じ軸へ傾け、ダッシュでは速度にかかわらず強めに傾けます。
    case Mode::kRun:
    case Mode::kDash: {
        engine::Vector3 dir = speed > 0.01f ? NormalizeOrForward(velocity) : motionDirection_;
        const float lean = mode_ == Mode::kDash ? 0.18f : std::clamp(speed * 0.016f, 0.0f, 0.16f);
        rotation.x = -dir.z * lean;
        rotation.z = dir.x * lean;
        break;
    }

    // 移動方向への傾きに、鉛直速度が小さい頂点付近で強まる揺れを足します。
    case Mode::kJump: {
        engine::Vector3 dir = speed > 0.01f ? NormalizeOrForward(velocity) : motionDirection_;
        const float lean = std::clamp(speed * 0.008f, 0.0f, 0.12f);
        const float airTilt = 1.0f - std::clamp(std::abs(velocity.y) / 24.0f, 0.0f, 1.0f);
        rotation.x = -dir.z * lean + std::sin(modeTimer_ * 7.0f) * (0.038f + airTilt * 0.040f);
        rotation.z = dir.x * lean + std::cos(modeTimer_ * 6.5f) * (0.036f + airTilt * 0.036f);
        break;
    }

    // 照準が決めた前後の傾きだけ維持し、横のロールは戻します。
    case Mode::kAimHook:
        rotation.x = current.x;
        rotation.z = 0.0f;
        break;

    // フック発射中は、入力済みの水平移動方向へ一定量傾ける。
    case Mode::kShootHook:
        rotation.x = -motionDirection_.z * 0.08f;
        rotation.z = motionDirection_.x * 0.08f;
        break;

    // 敵を引く間は正面を保ち、踏ん張りと引き込みを前後の動きで見せます。
    case Mode::kPullEnemy: {
        const float yankWave = (std::max)(0.0f, std::sin(modeTimer_ * 18.0f));
        const float yankPulse = yankWave * yankWave * (1.0f - pullProgress_ * 0.45f);
        const float brace = 1.0f - pullProgress_ * 0.55f;
        const float nodShake = std::sin(modeTimer_ * 30.0f) * 0.035f * brace;
        rotation.x = 0.16f * brace + yankPulse * 0.36f + nodShake;
        rotation.z = 0.0f;
        break;
    }

    // 運搬中は前後と横の小さな揺れで荷重を表す。
    case Mode::kCarry:
        rotation.x = std::sin(modeTimer_ * 7.0f) * 0.025f;
        rotation.z = std::cos(modeTimer_ * 6.0f) * 0.025f;
        break;

    // ゲートから出た方向に沿って、出現直後の傾きを減衰させます。
    case Mode::kGateReturn: {
        const float t = std::clamp(modeTimer_ / kGateReturnSlimeDuration, 0.0f, 1.0f);
        const float lean = std::sin(std::clamp(t / 0.70f, 0.0f, 1.0f) * kPi) * (1.0f - t * 0.55f) * 0.16f;
        rotation.x = -motionDirection_.z * lean;
        rotation.z = motionDirection_.x * lean;
        break;
    }

    // 専用の傾きを持たないモードは水平の向きだけを返す。
    default:
        break;
    }

    return rotation;
}
