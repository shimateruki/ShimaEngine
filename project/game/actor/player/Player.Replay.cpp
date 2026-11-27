#include <cstdint>
#include "Player.h"
#include "game/actor/player/copy/PlayerCopyAbilityController.h"

namespace {
json ToJson(const engine::Vector3& value) {
    // 要素順を復元側と揃え、リプレイの古い配列形式を維持する。
    return json::array({value.x, value.y, value.z});
}

json ToJson(const engine::Vector4& value) {
    // 色と姿勢の四成分を固定順で保存する。
    return json::array({value.x, value.y, value.z, value.w});
}

engine::Vector3 ReadVector3(const json& value, const engine::Vector3& fallback) {
    // JSON配列でないか3成分に足りない場合は、呼び出し元が渡した現在値を返す。
    if (!value.is_array() || value.size() < 3) {
        return fallback;
    }

    return {value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
}

engine::Vector4 ReadVector4(const json& value, const engine::Vector4& fallback) {
    // JSON配列でないか4成分に足りない場合は、元の色などを保持する。
    if (!value.is_array() || value.size() < 4) {
        return fallback;
    }

    return {value[0].get<float>(), value[1].get<float>(), value[2].get<float>(), value[3].get<float>()};
}

// Replayで復元できるコピー形態の数値範囲か検査します。
bool IsSupportedMorphTypeValue(std::int32_t value) {
    switch (static_cast<Player::EnemyMorphType>(value)) {
    // 未コピーと現行のコピー形態の値は復元可能とする。
    case Player::EnemyMorphType::kNone:
    case Player::EnemyMorphType::kSlime:
    case Player::EnemyMorphType::kBomber:
    case Player::EnemyMorphType::kFireSlime:
    case Player::EnemyMorphType::kThunderSlime:
    case Player::EnemyMorphType::kWindSlime:
    case Player::EnemyMorphType::kCrown:
        return true;

    // 廃止された値や未定義の数値は復元の対象外とする。
    default:
        return false;
    }
}
} // namespace

void Player::CaptureReplayCustomState(json& state) const {
    // 巻き戻し後の操作・被弾・変身が同じフレームから再開できる状態を記録する。
    Character::CaptureReplayCustomState(state);
    state["playerLockingOn"] = isLockingOn_;
    state["playerControlActive"] = isControlActive_;
    state["playerCinematicLocked"] = isCinematicLocked_;
    state["playerPendingAttack2"] = pendingAttack2_;
    // まだ消費していない攻撃入力も状態ごと保存し、巻き戻し時の次の入力受付を合わせます。
    const auto attackInput = attackInputBuffer_.Capture();
    state["playerComboWindowTimer"] = attackInput.comboWindowTimer;
    state["playerAttackInputBuffered"] = attackInput.buffered;
    state["playerAttackBufferUsed"] = attackInput.usedForStateStart;
    state["playerAttackInputBufferTimer"] = attackInput.inputBufferTimer;
    state["playerDamageCooldownTimer"] = damageCooldownTimer_;
    state["playerGuardContactCooldownTimer"] = guardContactCooldownTimer_;
    state["playerDamageInvincible"] = isDamageInvincible_;
    state["playerDashInvincible"] = isDashInvincible_;
    state["playerGuarding"] = isGuarding_;
    state["playerEvasionInvincibleTimer"] = evasionInvincibleTimer_;
    state["playerEvasionInvincible"] = isEvasionInvincible_;
    state["playerEvasionPortalEdge"] = evasionFeedback_ == EvasionFeedback::kPortalEdge;
    state["playerInvincibleBlinkTimer"] = invincibleBlinkTimer_;
    state["playerDeathTimer"] = deathTimer_;
    // 復帰位置とチェックポイント側に保持したコピー種別を、現在の変身とは別に記録します。
    state["playerRespawnPosition"] = ToJson(respawnPosition_);
    state["playerCheckpointActivated"] = checkpointActivated_;
    state["playerCheckpointObjectName"] = checkpointObjectName_;
    state["playerCheckpointMorphEnemyType"] = checkpointMorphEnemyType_;
    state["playerBaseRotation"] = ToJson(baseRotation_);
    state["playerFirstUpdate"] = isFirstUpdate_;
    state["playerJumpCount"] = jumpCount_;
    state["playerMoveYaw"] = GetMoveYaw();
    state["playerEnemyMorphType"] = static_cast<std::int32_t>(enemyMorphType_);
    state["playerEnemyMorphed"] = isEnemyMorphed_;
    state["playerMorphHasTimeLimit"] = enemyMorphHasTimeLimit_;
    state["playerMorphTimer"] = enemyMorphTimer_;
    state["playerMorphDuration"] = enemyMorphDuration_;
    state["playerMorphEffectTimer"] = enemyMorphEffectTimer_;
    state["playerMorphVisualTimer"] = enemyMorphVisualTimer_;
    // 変身解除の途中で巻き戻せるよう、進行時刻に加えて外観・位置の基準値も記録します。
    state["playerMorphReleaseActive"] = enemyMorphReleaseActive_;
    state["playerMorphReleaseBurstStarted"] = enemyMorphReleaseBurstStarted_;
    state["playerMorphReleaseExpired"] = enemyMorphReleaseExpired_;
    state["playerMorphReleaseTimer"] = enemyMorphReleaseTimer_;
    state["playerMorphReleaseDuration"] = enemyMorphReleaseDuration_;
    state["playerMorphReleaseBurstTime"] = enemyMorphReleaseBurstTime_;
    state["playerMorphReleaseParticleTimer"] = enemyMorphReleaseParticleTimer_;
    state["playerMorphReleasePlayerScale"] = ToJson(enemyMorphReleasePlayerScale_);
    state["playerMorphReleaseVisualScale"] = ToJson(enemyMorphReleaseVisualScale_);
    state["playerMorphReleaseStartPosition"] = ToJson(enemyMorphReleaseStartPosition_);
    state["playerMorphReleaseDirection"] = ToJson(enemyMorphReleaseDirection_);
    state["playerMorphReleaseTint"] = ToJson(enemyMorphReleaseTint_);
    state["playerElectricShockTimer"] = electricShockFeedbackTimer_;
    state["playerElectricShockLocked"] = electricShockControlLocked_;
    state["playerTutorialSafety"] = tutorialSafetyEnabled_;
    state["playerTutorialCarryThrowEnabled"] = tutorialCarryThrowEnabled_;
    state["playerTutorialCarryAbsorbEnabled"] = tutorialCarryAbsorbEnabled_;

    // 通常攻撃のコントローラーがあれば、体当たり・踏みつけの段階と残り時間も記録する。
    if (baseCombatController_) {
        const PlayerBaseCombatController::ReplayState combatState = baseCombatController_->CaptureReplayState();
        state["playerBaseCombatPhase"] = combatState.phase;
        state["playerBaseCombatDirection"] = ToJson(combatState.direction);
        state["playerBaseCombatTimer"] = combatState.timer;
        state["playerBaseCombatEffectTimer"] = combatState.effectTimer;
        state["playerBaseCombatInputBufferTimer"] = combatState.inputBufferTimer;
        state["playerBaseCombatPressHitEnemy"] = combatState.pressHitEnemy;
    }
}

void Player::RestoreReplayCustomState(const json& state) {
    // 共通の速度と接地を基底クラスに戻させてから、プレイヤー固有の操作や攻撃の保存値を反映します。
    Character::RestoreReplayCustomState(state);
    isLockingOn_ = state.value("playerLockingOn", isLockingOn_);
    isControlActive_ = state.value("playerControlActive", isControlActive_);
    isCinematicLocked_ = state.value("playerCinematicLocked", isCinematicLocked_);
    pendingAttack2_ = state.value("playerPendingAttack2", pendingAttack2_);
    // 保存済みの入力バッファに旧録画で欠けた項目だけ現在値を補い、まとめて戻します。
    auto attackInput = attackInputBuffer_.Capture();
    attackInput.comboWindowTimer = state.value("playerComboWindowTimer", attackInput.comboWindowTimer);
    attackInput.buffered = state.value("playerAttackInputBuffered", attackInput.buffered);
    attackInput.usedForStateStart = state.value("playerAttackBufferUsed", attackInput.usedForStateStart);
    attackInput.inputBufferTimer = state.value("playerAttackInputBufferTimer", attackInput.inputBufferTimer);
    attackInputBuffer_.Restore(attackInput);
    damageCooldownTimer_ = state.value("playerDamageCooldownTimer", damageCooldownTimer_);
    guardContactCooldownTimer_ = state.value("playerGuardContactCooldownTimer", 0.0f);
    isDamageInvincible_ = state.value("playerDamageInvincible", isDamageInvincible_);
    isDashInvincible_ = state.value("playerDashInvincible", isDashInvincible_);
    // 旧録画ではガード用のキー名を読み替える。回避表現は王冠用の縁表示にするか、本体の点滅にするかを別に戻す。
    isGuarding_ = state.value("playerGuarding", state.value("playerGuardInvincible", false));
    evasionInvincibleTimer_ = state.value("playerEvasionInvincibleTimer", evasionInvincibleTimer_);
    isEvasionInvincible_ = state.value("playerEvasionInvincible", isEvasionInvincible_);
    evasionFeedback_ = state.value("playerEvasionPortalEdge", false) ? EvasionFeedback::kPortalEdge : EvasionFeedback::kBodyFlash;
    invincibleBlinkTimer_ = state.value("playerInvincibleBlinkTimer", invincibleBlinkTimer_);
    deathTimer_ = state.value("playerDeathTimer", deathTimer_);

    // 復帰位置の記録がある場合だけ、ワールド座標を更新する。
    if (state.contains("playerRespawnPosition")) {
        respawnPosition_ = ReadVector3(state["playerRespawnPosition"], respawnPosition_);
    }

    checkpointActivated_ = state.value("playerCheckpointActivated", checkpointActivated_);
    checkpointObjectName_ = state.value("playerCheckpointObjectName", checkpointObjectName_);
    checkpointMorphEnemyType_ = state.value("playerCheckpointMorphEnemyType", checkpointMorphEnemyType_);

    // 基準回転の記録がある場合だけ、保存した3成分を読み込む。
    if (state.contains("playerBaseRotation")) {
        baseRotation_ = ReadVector3(state["playerBaseRotation"], baseRotation_);
    }

    isFirstUpdate_ = state.value("playerFirstUpdate", isFirstUpdate_);
    jumpCount_ = state.value("playerJumpCount", jumpCount_);

    // 視覚補正を含まない移動角の記録がある場合だけ、SetMoveYaw()を通して描画モデルにも反映する。
    if (state.contains("playerMoveYaw")) {
        SetMoveYaw(state["playerMoveYaw"].get<float>());
    }

    // 能力を作り直す前に保存値が現行のコピー種別か検証します。
    const std::int32_t morphType = state.value("playerEnemyMorphType", static_cast<std::int32_t>(enemyMorphType_));
    const bool supportedMorphType = IsSupportedMorphTypeValue(morphType);

    // 保存された数値が現行の形態に対応していれば、種別へ変換する。
    if (supportedMorphType) {
        enemyMorphType_ = static_cast<EnemyMorphType>(morphType);
    // 廃止された巨大コピーなど現行で使えない値は、未コピーへ戻す。
    } else {
        enemyMorphType_ = EnemyMorphType::kNone;
    }

    isEnemyMorphed_ = supportedMorphType && state.value("playerEnemyMorphed", isEnemyMorphed_);
    enemyMorphHasTimeLimit_ = state.value("playerMorphHasTimeLimit", enemyMorphHasTimeLimit_);
    enemyMorphTimer_ = state.value("playerMorphTimer", enemyMorphTimer_);
    enemyMorphDuration_ = state.value("playerMorphDuration", enemyMorphDuration_);
    enemyMorphEffectTimer_ = state.value("playerMorphEffectTimer", enemyMorphEffectTimer_);
    enemyMorphVisualTimer_ = state.value("playerMorphVisualTimer", enemyMorphVisualTimer_);

    // コピー能力の管理がある場合だけ、復元した形態に合わせて再構築か解除を行う。
    if (copyAbilityController_) {
        // 復元後も変身中なら、保存種別の既定プロファイルで能力を再構築する。
        if (isEnemyMorphed_) {
            copyAbilityController_->ActivateDefault(static_cast<std::int32_t>(enemyMorphType_));

            // 対応する能力が作成できた場合は、ライブのコピー元を指す参照を無効にする。
            if (copyAbilityController_->HandlesMorphType(static_cast<std::int32_t>(enemyMorphType_))) {
                enemyMorphSource_ = nullptr;
            }
        // 変身中でなければ、前のリプレイ時刻から残っている能力を解除する。
        } else {
            copyAbilityController_->Cancel(*this);
        }
    }

    // 解除の状態・時刻・発生位置をすべて戻してから、下の UpdateEnemyMorphRelease(0) でその時点の見た目を評価します。
    enemyMorphReleaseActive_ = state.value("playerMorphReleaseActive", false);
    enemyMorphReleaseBurstStarted_ = state.value("playerMorphReleaseBurstStarted", false);
    enemyMorphReleaseExpired_ = state.value("playerMorphReleaseExpired", false);
    enemyMorphReleaseTimer_ = state.value("playerMorphReleaseTimer", 0.0f);
    enemyMorphReleaseDuration_ = state.value("playerMorphReleaseDuration", enemyMorphReleaseDuration_);
    enemyMorphReleaseBurstTime_ = state.value("playerMorphReleaseBurstTime", enemyMorphReleaseBurstTime_);
    enemyMorphReleaseParticleTimer_ = state.value("playerMorphReleaseParticleTimer", 0.0f);

    // 解除開始時のプレイヤー本体のスケールがあれば復元する。
    if (state.contains("playerMorphReleasePlayerScale")) {
        enemyMorphReleasePlayerScale_ =
            ReadVector3(state["playerMorphReleasePlayerScale"], enemyMorphReleasePlayerScale_);
    }

    // 解除の表示に使う別のスケールが保存されていれば復元する。
    if (state.contains("playerMorphReleaseVisualScale")) {
        enemyMorphReleaseVisualScale_ =
            ReadVector3(state["playerMorphReleaseVisualScale"], enemyMorphReleaseVisualScale_);
    }

    // 解除開始地点の記録がある場合だけ、世界座標を更新する。
    if (state.contains("playerMorphReleaseStartPosition")) {
        enemyMorphReleaseStartPosition_ =
            ReadVector3(state["playerMorphReleaseStartPosition"], enemyMorphReleaseStartPosition_);
    }

    // 解除の演出方向が保存されていれば、3成分を復元する。
    if (state.contains("playerMorphReleaseDirection")) {
        enemyMorphReleaseDirection_ = ReadVector3(state["playerMorphReleaseDirection"], enemyMorphReleaseDirection_);
    }

    // 解除時の色が保存されていれば、アルファを含む4成分を復元する。
    if (state.contains("playerMorphReleaseTint")) {
        enemyMorphReleaseTint_ = ReadVector4(state["playerMorphReleaseTint"], enemyMorphReleaseTint_);
    }

    // 解除途中を復元した場合は、時計を進めずその時刻の見た目を評価する。
    if (enemyMorphReleaseActive_) {
        UpdateEnemyMorphRelease(0.0f);
    // 解除中でなく、前の時刻の解除用表示が残っている場合は隠す。
    } else if (enemyMorphReleaseVisual_) {
        enemyMorphReleaseVisual_->SetIsVisible(false);
    }

    // 解除演出の表示を確定した後、感電時間とチュートリアルで許可される操作を戻す。
    electricShockFeedbackTimer_ = state.value("playerElectricShockTimer", electricShockFeedbackTimer_);
    electricShockControlLocked_ = state.value("playerElectricShockLocked", electricShockControlLocked_);
    tutorialSafetyEnabled_ = state.value("playerTutorialSafety", tutorialSafetyEnabled_);
    tutorialCarryThrowEnabled_ = state.value("playerTutorialCarryThrowEnabled", tutorialCarryThrowEnabled_);
    tutorialCarryAbsorbEnabled_ = state.value("playerTutorialCarryAbsorbEnabled", tutorialCarryAbsorbEnabled_);

    // 通常攻撃の管理がある場合は、コピー能力とは別に攻撃段階と残り時間を復元する。
    if (baseCombatController_) {
        PlayerBaseCombatController::ReplayState combatState;
        combatState.phase = state.value("playerBaseCombatPhase", 0);

        // 通常攻撃の方向が保存されていれば復元する。旧記録で欠ける場合は既定方向を使う。
        if (state.contains("playerBaseCombatDirection")) {
            combatState.direction = ReadVector3(state["playerBaseCombatDirection"], combatState.direction);
        }

        combatState.timer = state.value("playerBaseCombatTimer", 0.0f);
        combatState.effectTimer = state.value("playerBaseCombatEffectTimer", 0.0f);
        combatState.inputBufferTimer = state.value("playerBaseCombatInputBufferTimer", 0.0f);
        combatState.pressHitEnemy = state.value("playerBaseCombatPressHitEnemy", false);
        baseCombatController_->RestoreReplayState(combatState);
    }
}
