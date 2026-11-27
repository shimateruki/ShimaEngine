#include <cstdint>
#include "BaseEnemy.h"

namespace {
json ToJson(const engine::Vector3& value) {
    // ベクトルは要素順を固定した配列にし、リプレイ保存形式を安定させる。
    return json::array({value.x, value.y, value.z});
}

json ToJson(const engine::Vector4& value) {
    // 色のアルファ成分も含め、復元後の表示を録画時と揃える。
    return json::array({value.x, value.y, value.z, value.w});
}

engine::Vector3 ReadVector3(const json& value, const engine::Vector3& fallback) {
    // 古い録画や不完全な値では既存状態を保ち、配列アクセスを避ける。
    if (!value.is_array() || value.size() < 3) {
        return fallback;
    }

    return {value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
}

engine::Vector4 ReadVector4(const json& value, const engine::Vector4& fallback) {
    // 色の4要素が揃わない録画は現在の値を使う。
    if (!value.is_array() || value.size() < 4) {
        return fallback;
    }

    return {value[0].get<float>(), value[1].get<float>(), value[2].get<float>(), value[3].get<float>()};
}
} // namespace

void BaseEnemy::CaptureReplayCustomState(json& state) const {
    // 再生で行動がずれないよう、基本姿勢だけでなく AI と被弾状態も保存します。
    Character::CaptureReplayCustomState(state);
    state["enemyDamageCooldown"] = damageCooldownTimer_;
    state["enemyColorResetTimer"] = colorResetTimer_;
    state["enemyDamageReactionTimer"] = damageReactionTimer_;
    state["enemyDamageReactionDuration"] = damageReactionDuration_;
    state["enemyDamageReactionStrength"] = damageReactionStrength_;
    state["enemyDamageReactionDirection"] = ToJson(damageReactionLocalDirection_);
    state["enemyDefaultColor"] = ToJson(defaultColor_);
    // 捕獲・投擲・移動先の乱数を記録し、巻き戻し後の経路を再現できるようにします。
    state["enemyCarried"] = isCarried_;
    state["enemyDormant"] = isDormant_;
    state["enemyThrowRecoveryTimer"] = throwRecoveryTimer_;
    state["enemyThrownPhysics"] = isThrownPhysics_;
    state["enemyWanderOrigin"] = ToJson(wanderOrigin_);
    state["enemyWanderTarget"] = ToJson(wanderTarget_);
    state["enemyWanderWaitTimer"] = wanderWaitTimer_;
    state["enemyWanderRetargetTimer"] = wanderRetargetTimer_;
    state["enemyWanderSeed"] = wanderSeed_;
    state["enemyHasWanderOrigin"] = hasWanderOrigin_;
    state["enemyThrownAngularVelocity"] = ToJson(thrownAngularVelocity_);
    state["enemyThrownTimer"] = thrownTimer_;
    state["enemyThrownSettleTimer"] = thrownSettleTimer_;
    state["enemyThrowRecoveryRotateTimer"] = throwRecoveryRotateTimer_;
    state["enemySlamCooldown"] = slamImpactCooldownTimer_;
    state["enemySlamCount"] = slamImpactCount_;
    state["enemyRotationRecovering"] = isThrowRotationRecovering_;
    // 撃破演出の段階とドロップ済みかどうかを保存し、再開しても演出やコインを重複させません。
    state["enemyDefeatPlaying"] = isDefeatEffectPlaying_;
    state["enemyDefeatFinished"] = isDefeatEffectFinished_;
    state["enemyCoinDropsSpawned"] = hasSpawnedDefeatCoinDrops_;
    state["enemyTargetDetected"] = wasTargetDetected_;
    state["enemyBossTargetEngaged"] = bossTargetEngaged_;
    state["enemyNoticeActive"] = isNoticeReactionActive_;
    state["enemyNoticeKind"] = static_cast<std::int32_t>(noticeReactionKind_);
    state["enemyNoticeTimer"] = noticeReactionTimer_;
    state["enemyNoticeCooldown"] = noticeReactionCooldown_;
    state["enemyNoticeDetectionRange"] = noticeDetectionRange_;
    state["enemyNoticeYaw"] = noticeMarkYaw_;
    state["enemyNoticeBaseScale"] = ToJson(noticeBaseScale_);
    state["enemyNoticeBaseRotation"] = ToJson(noticeBaseRotation_);
    state["enemyNoticeBaseColor"] = ToJson(noticeBaseColor_);
    state["enemyDefeatTimer"] = defeatEffectTimer_;
    state["enemyDefeatParticleTimer"] = defeatEffectParticleTimer_;
    state["enemyDefeatBasePosition"] = ToJson(defeatBasePosition_);
    state["enemyDefeatBaseScale"] = ToJson(defeatBaseScale_);
    state["enemyDefeatBaseColor"] = ToJson(defeatBaseColor_);
}

void BaseEnemy::RestoreReplayCustomState(const json& state) {
    // 録画にない項目は現在値を維持し、古いリプレイも読めるようにします。
    Character::RestoreReplayCustomState(state);
    damageCooldownTimer_ = state.value("enemyDamageCooldown", damageCooldownTimer_);
    colorResetTimer_ = state.value("enemyColorResetTimer", colorResetTimer_);
    damageReactionTimer_ = state.value("enemyDamageReactionTimer", damageReactionTimer_);
    damageReactionDuration_ = state.value("enemyDamageReactionDuration", damageReactionDuration_);
    damageReactionStrength_ = state.value("enemyDamageReactionStrength", damageReactionStrength_);

    // 被弾方向が記録されている場合だけ、ローカル方向を復元する。
    if (state.contains("enemyDamageReactionDirection")) {
        damageReactionLocalDirection_ =
            ReadVector3(state["enemyDamageReactionDirection"], damageReactionLocalDirection_);
    }

    // 既定色が記録されている場合だけ、リアクション後に戻すRGBAを復元する。
    if (state.contains("enemyDefaultColor"))
        defaultColor_ = ReadVector4(state["enemyDefaultColor"], defaultColor_);
    // 姿勢に続いて捕獲・投擲・徘徊の状態を戻し、途中からでも同じ速度や移動先で再開できるようにします。
    isCarried_ = state.value("enemyCarried", isCarried_);
    isDormant_ = state.value("enemyDormant", isDormant_);
    throwRecoveryTimer_ = state.value("enemyThrowRecoveryTimer", throwRecoveryTimer_);
    isThrownPhysics_ = state.value("enemyThrownPhysics", isThrownPhysics_);
    // 徘徊の起点が記録されている場合は、行動範囲の基準を戻す。
    if (state.contains("enemyWanderOrigin"))
        wanderOrigin_ = ReadVector3(state["enemyWanderOrigin"], wanderOrigin_);
    // 徘徊の目的地が記録されている場合は、追従先を途中から再開できるよう戻す。
    if (state.contains("enemyWanderTarget"))
        wanderTarget_ = ReadVector3(state["enemyWanderTarget"], wanderTarget_);
    wanderWaitTimer_ = state.value("enemyWanderWaitTimer", wanderWaitTimer_);
    wanderRetargetTimer_ = state.value("enemyWanderRetargetTimer", wanderRetargetTimer_);
    wanderSeed_ = state.value("enemyWanderSeed", wanderSeed_);
    hasWanderOrigin_ = state.value("enemyHasWanderOrigin", hasWanderOrigin_);
    // 回転速度が記録されている場合は、投げられた後の回転を続けられるよう戻す。
    if (state.contains("enemyThrownAngularVelocity"))
        thrownAngularVelocity_ = ReadVector3(state["enemyThrownAngularVelocity"], thrownAngularVelocity_);
    thrownTimer_ = state.value("enemyThrownTimer", thrownTimer_);
    thrownSettleTimer_ = state.value("enemyThrownSettleTimer", thrownSettleTimer_);
    throwRecoveryRotateTimer_ = state.value("enemyThrowRecoveryRotateTimer", throwRecoveryRotateTimer_);
    slamImpactCooldownTimer_ = state.value("enemySlamCooldown", slamImpactCooldownTimer_);
    slamImpactCount_ = state.value("enemySlamCount", slamImpactCount_);
    isThrowRotationRecovering_ = state.value("enemyRotationRecovering", isThrowRotationRecovering_);
    isDefeatEffectPlaying_ = state.value("enemyDefeatPlaying", isDefeatEffectPlaying_);
    isDefeatEffectFinished_ = state.value("enemyDefeatFinished", isDefeatEffectFinished_);
    hasSpawnedDefeatCoinDrops_ = state.value("enemyCoinDropsSpawned", hasSpawnedDefeatCoinDrops_);
    wasTargetDetected_ = state.value("enemyTargetDetected", wasTargetDetected_);
    bossTargetEngaged_ = state.value("enemyBossTargetEngaged", bossTargetEngaged_);
    isNoticeReactionActive_ = state.value("enemyNoticeActive", isNoticeReactionActive_);
    // 反応種別がない旧録画では、反応中フラグが立っていれば発見として読み替えます。
    const std::int32_t noticeKindValue = state.value("enemyNoticeKind", isNoticeReactionActive_ ? 1 : 0);

    switch (noticeKindValue) {
    // 記録値1は標的を発見したリアクションとして復元する。
    case 1:
        noticeReactionKind_ = NoticeReactionKind::kDetected;
        break;

    // 記録値2は標的を見失ったリアクションとして復元する。
    case 2:
        noticeReactionKind_ = NoticeReactionKind::kLost;
        break;

    // 値0や未対応の番号は、リアクション種別なしとして扱う。
    default:
        noticeReactionKind_ = NoticeReactionKind::kNone;
        break;
    }

    noticeReactionTimer_ = state.value("enemyNoticeTimer", noticeReactionTimer_);
    noticeReactionCooldown_ = state.value("enemyNoticeCooldown", noticeReactionCooldown_);
    noticeDetectionRange_ = state.value("enemyNoticeDetectionRange", noticeDetectionRange_);
    noticeMarkYaw_ = state.value("enemyNoticeYaw", noticeMarkYaw_);
    // 反応開始時の拡縮が残っている場合だけ、演出終了時の復元先に使う。
    if (state.contains("enemyNoticeBaseScale"))
        noticeBaseScale_ = ReadVector3(state["enemyNoticeBaseScale"], noticeBaseScale_);
    // 反応開始時の回転が記録されている場合だけ、その基準姿勢を戻す。
    if (state.contains("enemyNoticeBaseRotation"))
        noticeBaseRotation_ = ReadVector3(state["enemyNoticeBaseRotation"], noticeBaseRotation_);
    // 反応開始時の色が記録されている場合だけ、表示復元用の色に使う。
    if (state.contains("enemyNoticeBaseColor"))
        noticeBaseColor_ = ReadVector4(state["enemyNoticeBaseColor"], noticeBaseColor_);
    defeatEffectTimer_ = state.value("enemyDefeatTimer", defeatEffectTimer_);
    defeatEffectParticleTimer_ = state.value("enemyDefeatParticleTimer", defeatEffectParticleTimer_);
    // 撃破を開始した位置が記録されている場合だけ、消滅演出の基準座標を戻す。
    if (state.contains("enemyDefeatBasePosition"))
        defeatBasePosition_ = ReadVector3(state["enemyDefeatBasePosition"], defeatBasePosition_);
    // 撃破を開始した拡縮が記録されている場合だけ、消滅演出の基準サイズを戻す。
    if (state.contains("enemyDefeatBaseScale"))
        defeatBaseScale_ = ReadVector3(state["enemyDefeatBaseScale"], defeatBaseScale_);
    // 撃破を開始した色が記録されている場合だけ、消滅演出の基準色を戻す。
    if (state.contains("enemyDefeatBaseColor"))
        defeatBaseColor_ = ReadVector4(state["enemyDefeatBaseColor"], defeatBaseColor_);

    // 保存データからリアクション用の表示を構成し、リプレイで除外された敵のマークは表示しません。
    if (isNoticeReactionActive_) {
        ConfigureNoticeMarkForReaction();
    }

    // マークを保持している場合は、リアクション中で、リプレイ上も存在する敵に限り表示する。
    if (noticeMarkObject_) {
        noticeMarkObject_->SetIsVisible(isNoticeReactionActive_ && !IsReplayRemoved());
    }
}
