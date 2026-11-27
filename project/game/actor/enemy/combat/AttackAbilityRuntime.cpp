#include <cstdint>
#include "game/actor/enemy/combat/EnemyAttackProfile.h"

// 攻撃プロフィールの定義値と、実行中のクールダウン・発生段階を分離して保持します。

#include <algorithm>

bool AttackAbilityRuntime::Start(const AttackAbilityDefinition& definition) {
    // 再使用可能な段階以外では技を上書きしない。
    if (!IsReady()) {
        return false;
    }

    // 攻撃定義をコピーし、負の秒数は段階移行で即時に通過する0秒へそろえる。
    definition_ = definition;
    definition_.windupDuration = (std::max)(0.0f, definition_.windupDuration);
    definition_.activeDuration = (std::max)(0.0f, definition_.activeDuration);
    definition_.recoveryDuration = (std::max)(0.0f, definition_.recoveryDuration);
    definition_.cooldown = (std::max)(0.0f, definition_.cooldown);
    hasDefinition_ = true;
    phase_ = AttackAbilityPhase::kWindup;
    phaseTime_ = 0.0f;
    return true;
}

AttackAbilityUpdateResult AttackAbilityRuntime::Update(float deltaTime) {
    // 1フレームで複数段階を越える場合も遷移通知を落とさないよう残り時間を持ち回る。
    AttackAbilityUpdateResult result;
    result.previousPhase = phase_;
    result.currentPhase = phase_;

    // 未開始・再使用可能な状態では位相を進めず、遷移のない結果を返します。
    if (!hasDefinition_ || phase_ == AttackAbilityPhase::kReady) {
        return result;
    }

    float remainingTime = (std::max)(0.0f, deltaTime);

    for (std::int32_t transitionCount = 0; transitionCount < 5; ++transitionCount) {
        const float duration = definition_.GetPhaseDuration(phase_);
        const float timeToBoundary = (std::max)(0.0f, duration - phaseTime_);

        // 現在の段階内で使い切るフレームは経過時間だけを加え、段階を変えません。
        if (duration > 0.0f && remainingTime < timeToBoundary) {
            phaseTime_ += remainingTime;
            remainingTime = 0.0f;
            break;
        }

        // 現在の段階が正の長さなら境界までの時間を消費し、残りを次の段階へ渡す。
        if (duration > 0.0f) {
            remainingTime = (std::max)(0.0f, remainingTime - timeToBoundary);
        }

        // 残り時間が境界に届いた、または長さがゼロの段階を抜け、通過した段階を通知に積みます。
        phase_ = NextPhase(phase_);
        phaseTime_ = 0.0f;

        result.enteredActive |= phase_ == AttackAbilityPhase::kActive;
        result.enteredRecovery |= phase_ == AttackAbilityPhase::kRecovery;
        result.enteredCooldown |= phase_ == AttackAbilityPhase::kCooldown;
        result.becameReady |= phase_ == AttackAbilityPhase::kReady;

        // 再使用可能になった場合か、次の有時間段階を進める余りが無くなった場合に止めます。
        if (phase_ == AttackAbilityPhase::kReady ||
            (remainingTime <= 0.0f && definition_.GetPhaseDuration(phase_) > 0.0f)) {
            break;
        }
    }

    result.currentPhase = phase_;
    return result;
}

void AttackAbilityRuntime::Cancel(bool enterCooldown) {
    // 定義をまだ保持していない場合は、再使用待ちの長さもないため初期状態へ戻す。
    if (!hasDefinition_) {
        Reset();
        return;
    }

    // 定義を保持していても、再使用待ちを要求され、かつ正の待ち時間が設定された場合だけクールダウンへ移る。
    phaseTime_ = 0.0f;
    phase_ = enterCooldown && definition_.cooldown > 0.0f ? AttackAbilityPhase::kCooldown : AttackAbilityPhase::kReady;
}

void AttackAbilityRuntime::Reset() {
    // 定義ごと捨て、次の技を即座に開始できる初期状態へ戻す。
    definition_ = {};
    phase_ = AttackAbilityPhase::kReady;
    phaseTime_ = 0.0f;
    hasDefinition_ = false;
}

float AttackAbilityRuntime::GetPhaseProgress() const {
    // 未開始時に段階の長さを参照しないよう、進捗0を返す。
    return hasDefinition_ ? definition_.GetPhaseProgressFromElapsed(phase_, phaseTime_) : 0.0f;
}

float AttackAbilityRuntime::GetElapsedActionTime() const {
    // 定義が未設定の場合は、進行中の動作がないため0秒を返す。
    if (!hasDefinition_) {
        return 0.0f;
    }

    switch (phase_) {
    // 予備動作は攻撃の先頭なので、この段階の経過秒をそのまま返す。
    case AttackAbilityPhase::kWindup:
        return phaseTime_;

    // 攻撃持続中は、予備動作に使った時間を足して攻撃全体の経過秒へ戻す。
    case AttackAbilityPhase::kActive:
        return definition_.windupDuration + phaseTime_;

    // 硬直中は既に終えた予備動作と攻撃持続の秒数を加える。
    case AttackAbilityPhase::kRecovery:
        return definition_.windupDuration + definition_.activeDuration + phaseTime_;

    // 再使用待ちの経過は動作の尺に加えず、攻撃の終端時刻を返す。
    case AttackAbilityPhase::kCooldown:
        return definition_.GetActionDuration();

    // 再使用可能または未知の段階に、進行中の攻撃時刻はない。
    case AttackAbilityPhase::kReady:
    default:
        return 0.0f;
    }
}

bool AttackAbilityRuntime::CanCancel() const {
    // キャンセル窓は定義内の時刻で判定するため、現在段階を経過時刻へ変換する。
    return hasDefinition_ && definition_.IsCancelWindowOpen(GetElapsedActionTime());
}

AttackAbilityPhase AttackAbilityRuntime::NextPhase(AttackAbilityPhase phase) const {
    // 予備動作から硬直・再使用待ちまでの順序をここで一本化する。
    switch (phase) {
    // 予備動作の境界を越えたら攻撃を有効化する段階へ移す。
    case AttackAbilityPhase::kWindup:
        return AttackAbilityPhase::kActive;

    // 攻撃の有効時間が終わったら硬直へ移る。
    case AttackAbilityPhase::kActive:
        return AttackAbilityPhase::kRecovery;

    // 硬直を抜けたら、再発動の待ち時間へ移る。
    case AttackAbilityPhase::kRecovery:
        return AttackAbilityPhase::kCooldown;

    // 再使用待ちまで終わった時点、既に待機中、未知の段階はいずれも再使用可能へ戻す。
    case AttackAbilityPhase::kCooldown:
    case AttackAbilityPhase::kReady:
    default:
        return AttackAbilityPhase::kReady;
    }
}
