#include "game/actor/player/Player.Internal.h"
#include <cstdint>
#include "Easing.h"

#include "game/actor/item/DroppedCopyStar.h"
#include "game/actor/combat/CombatGroundQuery.h"
#include "BaseScene.h"

using namespace playerImplementation;

// コピー解除の進行率を0〜1で返す。演出が無効な場合は0。
float Player::GetEnemyMorphReleaseRate() const {
    // 解除演出が止まっている間は進行率を使わせない。
    if (!enemyMorphReleaseActive_ || enemyMorphReleaseDuration_ <= 0.0f) {
        return 0.0f;
    }

    return std::clamp(enemyMorphReleaseTimer_ / enemyMorphReleaseDuration_, 0.0f, 1.0f);
}

bool Player::IsPinkSlimeMorphed() const {
    // 通常のピンクコピーだけに専用ジャンプを許可する。
    return isEnemyMorphed_ && enemyMorphType_ == EnemyMorphType::kSlime;
}

void Player::ActivateCheckpoint(const engine::Vector3& position, const std::string& checkpointObjectName) {
    // 復活位置と同時に現在のコピーを記録し、再開時の能力を再現する。
    respawnPosition_ = position;
    checkpointActivated_ = true;
    checkpointObjectName_ = checkpointObjectName;
    const std::string currentMorph = GetEnemyTypeForCurrentMorph();

    // 保存できる通常コピーの型がある場合は、今回のチェックポイントへ記録する型を更新する。
    if (!currentMorph.empty()) {
        checkpointMorphEnemyType_ = currentMorph;
    }

    RefreshCheckpointMorphSnapshot();
}

bool Player::ApplyStoredCopy(const std::string& enemyType, bool unlimitedDuration) {
    // 記憶台や復帰処理は登録済みの能力だけを指定でき、演出中の取得は王冠だけ許可します。
    const PlayerCopyTypeDescriptor* descriptor = playerCopyTypeCatalog::Find(enemyType);
    const EnemyMorphType resolvedType = ResolveEnemyMorphType(enemyType);
    // 登録・変換できない型は拒否する。死亡、吸収、解除中にも開始できず、演出拘束を通せるのは王冠だけ。
    if (!descriptor || resolvedType == EnemyMorphType::kNone || IsDead() ||
        (isCinematicLocked_ && resolvedType != EnemyMorphType::kCrown) || absorbEffectActive_ ||
        enemyMorphReleaseActive_) {
        return false;
    }

    // 通常攻撃と既存のコピーを先に止め、新しい能力に前回の判定や姿勢を残しません。
    // 通常攻撃の管理があれば、能力を差し替える前に判定と操作状態を終了する。
    if (baseCombatController_) {
        baseCombatController_->Cancel(*this, true);
    }

    // すでにコピー中で能力セッションがあるなら、そのセッションの発生物と技を停止する。
    if (isEnemyMorphed_ && copyAbilityController_) {
        copyAbilityController_->Cancel(*this);
    }

    // 記憶台は敵を吸収する通常経路を通らないため、所持中の敵をここで正規解放します。
    // 参照だけを残すと変身解除後に敵が再び頭上へ追従し、カメラへ張り付いたように見えます。
    ReleaseCarriedEnemy(true);
    aimTargetObject_ = nullptr;

    // フックのマーカーが作られていれば、照準を取り消した表示へ揃える。
    if (hookMarker_) {
        hookMarker_->SetIsVisible(false);
    }

    // 現在の向きと通常形態の表示を控えてから、新しい能力の制限時間と動作を確定します。
    const float currentMoveYaw = GetMoveYaw();
    SaveEnemyMorphBaseAppearance(true);

    enemyMorphType_ = resolvedType;
    enemyMorphSource_ = nullptr;
    enemyMorphHasTimeLimit_ = !unlimitedDuration;
    enemyMorphDuration_ = kEnemyMorphDuration;
    enemyMorphTimer_ = enemyMorphHasTimeLimit_ ? enemyMorphDuration_ : 0.0f;
    enemyMorphEffectTimer_ = 0.0f;
    enemyMorphVisualTimer_ = 0.0f;
    enemyMorphTint_ = GetEnemyMorphTint(enemyMorphType_);

    // 能力の管理がある場合に、解決済みの形態から既定セッションを開く。
    if (copyAbilityController_) {
        copyAbilityController_->ActivateDefault(static_cast<std::int32_t>(enemyMorphType_));
    }

    isEnemyMorphed_ = true;
    CompleteEnemyMorphStart(currentMoveYaw);

    // 王冠とマントの装飾で被弾範囲が広がらないよう、体の判定を維持する。
    const engine::ColliderConfig bodyCollider = GetColliderConfig();
    SetModel(std::string(descriptor->modelName));
    // 王冠へ切り替える場合は、装飾の広がりを当たり判定へ反映しないよう、退避した Collider を戻す。
    if (resolvedType == EnemyMorphType::kCrown)
        SetColliderConfig(bodyCollider);
    SetTexture("");

    // 覇王モデルは顔・金属・布を別材質で持つため、全身ジェル化しない。
    SetMaterialType(resolvedType == EnemyMorphType::kCrown ? 0 : 25);
    SetEmissive(1.0f);
    SetAnimationName("");
    ResetAnimationTime();
    SetColor(descriptor->displayColor);
    EmitEnemyMorphStartEffects();

    DebugConsole::GetInstance()->AddLog("Stored copy applied: " + enemyType);
    return true;
}

std::string Player::GetEnemyTypeForCurrentMorph() const {
    // セーブデータには列挙値でなく安定した敵種別名を記録する。
    if (!isEnemyMorphed_) {
        return {};
    }

    switch (enemyMorphType_) {
    // ピンクは既定の Slime として保存する。
    case EnemyMorphType::kSlime:
        return "Slime";

    // ボム能力は運搬可能な爆弾と区別して Bomber として保存する。
    case EnemyMorphType::kBomber:
        return "Bomber";

    // 炎コピーは FireSlime から復元する。
    case EnemyMorphType::kFireSlime:
        return "FireSlime";

    // 雷コピーは ThunderSlime から復元する。
    case EnemyMorphType::kThunderSlime:
        return "ThunderSlime";

    // 風コピーは WindSlime から復元する。
    case EnemyMorphType::kWindSlime:
        return "WindSlime";

    // 王冠など通常の復帰・再取得に使わない形態は保存名を返さない。
    default:
        return {};
    }
}

void Player::ClearCrownPower() {
    // 王冠以外のコピーを解除せず、最終戦専用の能力だけ片付ける。
    if (enemyMorphType_ == EnemyMorphType::kCrown)
        CancelEnemyMorph();
}

void Player::RefreshCheckpointMorphSnapshot() {
    // 有効なチェックポイントだけ永続データへ反映する。
    if (!checkpointActivated_) {
        return;
    }

    const engine::Vector3 position = respawnPosition_;
    GameDataManager::GetInstance()->RequestStageCheckpointRespawn(StageManager::GetInstance()->GetCurrentStageIndex(),
                                                                  position.x, position.y, position.z,
                                                                  checkpointMorphEnemyType_, checkpointObjectName_);
}

void Player::BeginEnemyMorphRelease(bool expired) {
    // 王冠の欠片は最終戦中の専用装備。任意解除による攻略不能を防ぎます。
    if (enemyMorphType_ == EnemyMorphType::kCrown && !expired)
        return;
    // コピーしていないか解除を開始済みなら、途中の状態と時計を再設定しない。
    if (!isEnemyMorphed_ || enemyMorphReleaseActive_) {
        return;
    }

    // 管理が存在する場合は、解除の姿勢を作る前に実行中のコピー能力を止める。
    if (copyAbilityController_) {
        copyAbilityController_->Cancel(*this);
    }

    evasionInvincibleTimer_ = 0.0f;
    isEvasionInvincible_ = false;
    UpdateColor();

    // 任意解除と期限切れで異なる長さを設定し、粒子の発火時刻を同じ時計へ登録します。
    enemyMorphReleaseActive_ = true;
    ClearHurtFace();
    enemyMorphReleaseBurstStarted_ = false;
    enemyMorphReleaseExpired_ = expired;
    enemyMorphReleaseTimer_ = 0.0f;
    enemyMorphReleaseDuration_ = expired ? kEnemyMorphExpiredReleaseDuration : kEnemyMorphReleaseDuration;
    enemyMorphReleaseBurstTime_ = expired ? kEnemyMorphExpiredReleaseBurstTime : kEnemyMorphReleaseBurstTime;
    enemyMorphReleaseParticleTimer_ = 0.0f;
    enemyMorphReleasePlayerScale_ = GetScale();
    enemyMorphReleaseTint_ = GetEnemyMorphTint(enemyMorphType_);

    // 開始時に背後の方向を固定し、外へ出す残像は通常形態の大きさを基準にします。
    const float yaw = GetMoveYaw();
    enemyMorphReleaseDirection_ = {-std::sin(yaw), 0.0f, -std::cos(yaw)};
    const float playerScale =
        (std::max)({std::abs(savedMorphScale_.x), std::abs(savedMorphScale_.y), std::abs(savedMorphScale_.z), 0.1f});
    const float residueScale = std::clamp(playerScale * 0.32f, 0.38f, 0.82f);
    enemyMorphReleaseVisualScale_ = {residueScale, residueScale, residueScale};

    // 残像の表示物を作成済みなら、分離時刻までは見えない大きさと表示状態にする。
    if (enemyMorphReleaseVisual_) {
        enemyMorphReleaseVisual_->SetIsVisible(false);
        enemyMorphReleaseVisual_->SetColor(enemyMorphReleaseTint_);
        enemyMorphReleaseVisual_->SetScale({0.0f, 0.0f, 0.0f});
    }

    // 解除中に前形態の攻撃を実行しないよう保留入力を破棄します。
    pendingAttack2_ = false;
    attackInputBuffer_.ClearPendingInput();
    DebugConsole::GetInstance()->AddLog(expired ? "Enemy morph release: expired." : "Enemy morph release: manual.");
}

void Player::EmitEnemyMorphReleaseBurst() {
    // 解除の山場は一度だけ発火し、残像と回収可能な星を重複生成しない。
    if (enemyMorphReleaseBurstStarted_) {
        return;
    }

    enemyMorphReleaseBurstStarted_ = true;
    enemyMorphReleaseParticleTimer_ = 0.0f;
    enemyMorphReleaseStartPosition_ = GetWorldPosition() + engine::Vector3{0.0f, 0.82f, 0.0f};

    // 残像が使える場合は、分離する位置と背後を向いた姿勢を確定して表示する。
    if (enemyMorphReleaseVisual_) {
        enemyMorphReleaseVisual_->SetTranslate(enemyMorphReleaseStartPosition_);
        enemyMorphReleaseVisual_->SetRotation({0.0f, GetMoveYaw() + kPi, 0.0f});
        enemyMorphReleaseVisual_->SetScale({enemyMorphReleaseVisualScale_.x * 1.18f,
                                            enemyMorphReleaseVisualScale_.y * 0.58f,
                                            enemyMorphReleaseVisualScale_.z * 1.18f});
        enemyMorphReleaseVisual_->SetColor(enemyMorphReleaseTint_);
        enemyMorphReleaseVisual_->SetIsVisible(true);
        enemyMorphReleaseVisual_->UpdateLocalMatrix();
        enemyMorphReleaseVisual_->UpdateWorldMatrix();
    }

    engine::Vector3 burstDirection = {enemyMorphReleaseDirection_.x, 0.42f, enemyMorphReleaseDirection_.z};
    const float burstLength = engine::Math::Length(burstDirection);

    // 長さが十分なら、粒子が飛ぶ向きを単位化して距離倍率と独立させる。
    if (burstLength > 0.001f) {
        burstDirection = burstDirection / burstLength;
    }

    // CPU 粒子が利用できる場合に、期限切れか任意解除かで粒子量を変えて発生させる。
    if (particleSystem_) {
        particleSystem_->SpawnParticles(
            enemyMorphReleaseStartPosition_, enemyMorphReleaseExpired_ ? 20 : 28, 1.45f, &burstDirection,
            enemyMorphReleaseExpired_ ? 20.0f : 25.0f, enemyMorphReleaseTint_,
            {enemyMorphReleaseTint_.x, enemyMorphReleaseTint_.y, enemyMorphReleaseTint_.z, 0.0f}, 0.10f, 0.34f, 0.32f,
            0.04f);
    }

    // 雷コピーは、形態の大きさに合わせた専用の放電を出す。
    if (enemyMorphType_ == EnemyMorphType::kThunderSlime) {
        EmitThunderMorphBurst(enemyMorphReleaseStartPosition_, enemyMorphReleaseVisualScale_);
    // ピンクコピーは、分離位置で専用の破裂粒子を出す。
    } else if (enemyMorphType_ == EnemyMorphType::kSlime) {
        EmitCopyMorphPreset(kPinkSlimeMorphBurstPreset, enemyMorphReleaseStartPosition_);
    // ボムコピーは、分離位置に衝撃波とオーラを重ねる。
    } else if (enemyMorphType_ == EnemyMorphType::kBomber) {
        SpawnCopyMorphPulse(kBomberMorphPulseEffectPath, enemyMorphReleaseStartPosition_, {1.24f, 1.24f, 1.24f});
        EmitCopyMorphPreset(kBomberMorphAuraPreset, enemyMorphReleaseStartPosition_);
    // 炎コピーは、分離位置に炎用の発光と粒子を重ねる。
    } else if (enemyMorphType_ == EnemyMorphType::kFireSlime) {
        SpawnCopyMorphPulse(kFireMorphPulseEffectPath, enemyMorphReleaseStartPosition_, {1.28f, 1.28f, 1.28f});
        EmitCopyMorphPreset(kFireMorphAuraPreset, enemyMorphReleaseStartPosition_);
    // 風コピーは、横に広がる風のパルスとオーラを重ねる。
    } else if (enemyMorphType_ == EnemyMorphType::kWindSlime) {
        SpawnCopyMorphPulse(kWindMorphPulseEffectPath, enemyMorphReleaseStartPosition_, {1.32f, 1.08f, 1.32f});
        EmitCopyMorphPreset(kWindMorphAuraPreset, enemyMorphReleaseStartPosition_);
    }

    // 型情報を失う前に、任意解除分だけ回収可能な星へ移します。
    const std::string droppedType = GetEnemyTypeForCurrentMorph();
    auto* sceneManager = SceneManager::GetInstance();
    // 任意解除・保存可能な能力・描画基盤・プレイ中のシーンがすべて揃った場合だけ、背後の接地位置へ回収用の星を追加する。
    if (!enemyMorphReleaseExpired_ && !droppedType.empty() && GetObjectCommon() && sceneManager && sceneManager->IsPlaying() &&
        sceneManager->GetCurrentScene()) {
        engine::Vector3 landing = GetWorldPosition() + enemyMorphReleaseDirection_ * 2.25f;
        landing = combatGroundQuery::FindGroundPoint(landing, this, {16.0f, 34.0f}, GetWorldPosition().y);
        landing.y += 0.85f;
        auto star = std::make_unique<DroppedCopyStar>();
        star->InitializeDrop(GetObjectCommon(), droppedType, !enemyMorphHasTimeLimit_, enemyMorphReleaseStartPosition_, landing);
        sceneManager->GetCurrentScene()->AddObject(std::move(star));
    }

    RestoreEnemyMorphAppearance();
}

void Player::UpdateEnemyMorphRelease(float deltaTime) {
    // 予備動作、成分の分離、復帰を同じ時計で進めて見た目と操作復帰を同期する。
    if (!enemyMorphReleaseActive_) {
        return;
    }

    // 正の経過秒を受けた場合だけ、解除時計と次の発生までの待ち時間を進める。
    if (deltaTime > 0.0f) {
        enemyMorphReleaseTimer_ += deltaTime;
        enemyMorphReleaseParticleTimer_ -= deltaTime;
    }

    // 分離前はコピー形態の身体を縮める予備動作だけを見せます。
    if (!enemyMorphReleaseBurstStarted_ && enemyMorphReleaseTimer_ < enemyMorphReleaseBurstTime_) {
        const float t =
            std::clamp(enemyMorphReleaseTimer_ / (std::max)(enemyMorphReleaseBurstTime_, 0.001f), 0.0f, 1.0f);
        const float eased = Easing::SmoothStep(t);
        SetScale({enemyMorphReleasePlayerScale_.x * (1.0f + eased * 0.18f),
                  enemyMorphReleasePlayerScale_.y * (1.0f - eased * 0.38f),
                  enemyMorphReleasePlayerScale_.z * (1.0f + eased * 0.18f)});
        SetColor(engine::Math::Lerp(GetColor(), {1.0f, 1.0f, 1.0f, 1.0f}, eased * 0.22f));
        UpdateLocalMatrix();
        UpdateWorldMatrix();
        return;
    }

    // 予備区間を通過し、まだ分離していない場合にだけ残像や星を発生させる。
    if (!enemyMorphReleaseBurstStarted_) {
        EmitEnemyMorphReleaseBurst();
    }

    // 分離を境に時計を読み替え、通常形態の揺れを戻しながら残像を外へ飛ばします。
    const float flightDuration = (std::max)(enemyMorphReleaseDuration_ - enemyMorphReleaseBurstTime_, 0.001f);
    const float flightTime = (std::max)(0.0f, enemyMorphReleaseTimer_ - enemyMorphReleaseBurstTime_);
    const float t = std::clamp(flightTime / flightDuration, 0.0f, 1.0f);
    const float eased = Easing::OutCubic(t);
    const float wobble = std::sin(t * kPi * 4.0f) * std::pow(1.0f - t, 1.35f);

    SetScale({savedMorphScale_.x * (1.0f + wobble * 0.11f), savedMorphScale_.y * (1.0f - wobble * 0.09f),
              savedMorphScale_.z * (1.0f + wobble * 0.11f)});
    UpdateLocalMatrix();
    UpdateWorldMatrix();

    // 残像は弧を描いて背後へ進め、終盤の縮小と透明化で消します。
    if (enemyMorphReleaseVisual_) {
        engine::Vector3 position = enemyMorphReleaseStartPosition_;
        position.x += enemyMorphReleaseDirection_.x * 2.25f * eased;
        position.z += enemyMorphReleaseDirection_.z * 2.25f * eased;
        position.y += std::sin(t * kPi) * 0.92f - t * 0.34f;

        float scaleRate = 1.0f;

        // 飛行の最初の2割では、残像の倍率を小さい値から立ち上げる。
        if (t < 0.20f) {
            scaleRate = 0.72f + (t / 0.20f) * 0.38f;
        // 飛行が68%を超えたら、終点で倍率が0になるよう縮める。
        } else if (t > 0.68f) {
            scaleRate = std::clamp((1.0f - t) / 0.32f, 0.0f, 1.0f);
        }

        const float residueWobble = std::sin(t * kPi * 5.0f) * (1.0f - t) * 0.18f;
        enemyMorphReleaseVisual_->SetTranslate(position);
        enemyMorphReleaseVisual_->SetScale(
            {enemyMorphReleaseVisualScale_.x * scaleRate * (1.0f + residueWobble),
             enemyMorphReleaseVisualScale_.y * scaleRate * (1.0f - residueWobble * 0.75f),
             enemyMorphReleaseVisualScale_.z * scaleRate * (1.0f + residueWobble)});
        enemyMorphReleaseVisual_->SetColor({enemyMorphReleaseTint_.x, enemyMorphReleaseTint_.y,
                                            enemyMorphReleaseTint_.z, std::clamp((1.0f - t) * 1.8f, 0.0f, 1.0f)});
        enemyMorphReleaseVisual_->Update(deltaTime);
        enemyMorphReleaseVisual_->UpdateLocalMatrix();
        enemyMorphReleaseVisual_->UpdateWorldMatrix();
    }

    // 期限切れではすぐ、任意解除では残像が離れてから尾の粒子を発生させます。
    if (enemyMorphReleaseParticleTimer_ <= 0.0f && t < 0.82f) {
        // 期限切れならすぐ、任意解除なら残像が離れるまで進行率0.18を待つ。
        if (enemyMorphReleaseExpired_ || t > 0.18f) {
            // CPU 粒子が使える場合に、残像の後ろへ尾を置く。
            if (particleSystem_) {
                engine::Vector3 trailDirection = {-enemyMorphReleaseDirection_.x, 0.12f, -enemyMorphReleaseDirection_.z};
                engine::Vector3 trailPosition = enemyMorphReleaseVisual_ ? enemyMorphReleaseVisual_->GetWorldPosition()
                                                                 : enemyMorphReleaseStartPosition_;
                particleSystem_->SpawnParticles(
                    trailPosition, 4, 0.55f, &trailDirection, 6.0f, enemyMorphReleaseTint_,
                    {enemyMorphReleaseTint_.x, enemyMorphReleaseTint_.y, enemyMorphReleaseTint_.z, 0.0f}, 0.04f, 0.14f,
                    0.16f, 0.01f);
            }

            enemyMorphReleaseParticleTimer_ = 0.055f;
        }
    }

    // 解除演出が終わったら状態を片付け、通常形態の最後の反動だけを発生させます。
    if (enemyMorphReleaseTimer_ >= enemyMorphReleaseDuration_) {
        const engine::Vector3 reboundScale = {savedMorphScale_.x * 1.08f, savedMorphScale_.y * 0.92f,
                                      savedMorphScale_.z * 1.08f};
        ResetEnemyMorphRelease();
        TriggerSlimeImpulse(reboundScale, 0.16f);
    }
}

void Player::RestoreEnemyMorphAppearance() {
    // コピー前のモデル・材質・衝突範囲と向きをまとめて復元する。
    if (!isEnemyMorphed_) {
        return;
    }

    const float currentMoveYaw = GetMoveYaw();
    BaseEnemy* morphSource = enemyMorphSource_;

    // 能力管理が残っている場合は、外観を戻す前に付随する技や演出を止める。
    if (copyAbilityController_) {
        copyAbilityController_->Cancel(*this);
    }

    // 通常モデル名を保存できていた場合は、そのモデルを再設定する。
    if (!savedMorphModelName_.empty()) {
        SetModel(savedMorphModelName_);
    }

    // 元のテクスチャ指定がある場合は、そのパスに戻す。
    if (!savedMorphTexturePath_.empty()) {
        SetTexture(savedMorphTexturePath_);
    // 元のテクスチャ指定が空なら、コピー解除後にも新しい指定を残さない。
    } else {
        SetTexture("");
    }

    SetMaterialType(savedMorphMaterialType_);
    SetEmissive(savedMorphEmissive_);
    SetColor(savedMorphColor_);
    SetScale(savedMorphScale_);
    SetAnimationName(savedMorphAnimName_);
    SetAnimationLoop(savedMorphAnimLoop_);
    ResetAnimationTime();

    // コピー前に控えた可視性を子ごとに戻し、後から加わった子は表示します。
    const auto& children = GetChildren();

    for (size_t i = 0; i < children.size(); ++i) {
        engine::Object3D* child = children[i];

        // 子の参照が有効な場合だけ、親と同じ元の色と可視性を適用する。
        if (child) {
            child->SetColor(savedMorphColor_);

            // コピー前に対応する子を記録していれば、そのときの可視性に戻す。
            if (i < savedMorphChildVisible_.size()) {
                child->SetIsVisible(savedMorphChildVisible_[i]);
            // 保存時の末尾より後にある子は、通常表示へ戻す。
            } else {
                child->SetIsVisible(true);
            }
        }
    }

    // コピー元がまだ生きている時だけ表示を戻し、運搬参照が同じ実体なら離します。
    if (morphSource && !morphSource->IsDead() && !morphSource->IsDefeatEffectPlaying()) {
        morphSource->SetIsVisible(savedMorphSourceVisible_);
    }

    // 運搬中の参照がコピー元と同一なら、コピー元を再び追従させないよう参照を外す。
    if (carriedEnemy_ == morphSource) {
        SetCarriedEnemy(nullptr);
    }

    // 能力の型と時間を消し、カメラの照準制限を外しても移動方向は保ちます。
    isEnemyMorphed_ = false;
    enemyMorphType_ = EnemyMorphType::kNone;
    enemyMorphSource_ = nullptr;
    enemyMorphHasTimeLimit_ = true;
    enemyMorphTimer_ = 0.0f;
    enemyMorphEffectTimer_ = 0.0f;
    enemyMorphVisualTimer_ = 0.0f;
    savedMorphChildVisible_.clear();
    savedMorphSourceVisible_ = true;

    // 本編のプレイヤーを元へ戻した場合だけ、共有メインカメラの照準制限を外す。
    if (engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera(); camera && !IsEditorInternal()) {
        camera->SetAimCameraSuppressed(false);
    }

    // 通常形態へ戻した後で移動角を適用し、通常モデルの正面補正で見た目の角度を再計算します。
    SetMoveYaw(currentMoveYaw);
    DebugConsole::GetInstance()->AddLog("Enemy morph end.");
}

void Player::ResetEnemyMorphRelease() {
    // 中断された解除演出も、残像と伸縮を残さず初期化する。
    // 残像を生成済みの場合は、中断後のフレームに残らないよう非表示にする。
    if (enemyMorphReleaseVisual_) {
        enemyMorphReleaseVisual_->SetIsVisible(false);
        enemyMorphReleaseVisual_->SetScale({0.0f, 0.0f, 0.0f});
    }

    // 解除中にすでに通常形態へ戻っている場合は、反動中の色と大きさも通常時に戻す。
    if (enemyMorphReleaseActive_ && !isEnemyMorphed_) {
        SetScale(savedMorphScale_);
        SetColor(savedMorphColor_);
        UpdateLocalMatrix();
        UpdateWorldMatrix();
    }

    enemyMorphReleaseActive_ = false;
    enemyMorphReleaseBurstStarted_ = false;
    enemyMorphReleaseExpired_ = false;
    enemyMorphReleaseTimer_ = 0.0f;
    enemyMorphReleaseParticleTimer_ = 0.0f;
}

void Player::CancelEnemyMorph() {
    // 吸収演出とコピー解除が重なっても、古い照準や無敵状態を持ち越さない。
    if (absorbEffectActive_) {
        CancelAbsorbEffect(true);
    }

    ResetEnemyMorphRelease();
    RestoreEnemyMorphAppearance();

    // 照準マーカーがあれば、強制解除後に表示が残らないよう閉じる。
    if (hookMarker_) {
        hookMarker_->SetIsVisible(false);
    }

    aimTargetObject_ = nullptr;
    evasionInvincibleTimer_ = 0.0f;
    isEvasionInvincible_ = false;
    UpdateColor();
}

#ifdef USE_IMGUI
void Player::DebugForceEnemyMorph(BaseEnemy* enemy) {
    // コピー元がいない、死亡中、または演出に拘束されている場合は、強制付与を行わない。
    if (!enemy || IsDead() || isCinematicLocked_) {
        return;
    }

    // 通常の吸収・運搬状態を先に解消し、能力側に古い敵参照を残さないようにします。
    CancelEnemyMorph();
    ReleaseCarriedEnemy(true);
    StartEnemyMorph(enemy);

    // デバッグ中は時間切れで解除されないようにし、任意の能力を継続して確認できます。
    if (isEnemyMorphed_) {
        enemyMorphHasTimeLimit_ = false;
        enemyMorphTimer_ = 0.0f;
    }
}

void Player::DebugClearEnemyMorph() {
    // デバッグ操作も通常の解除経路を通し、後片付けを共通化する。
    CancelEnemyMorph();
}

void Player::DebugPrepareForTeleport() {
    // 移動系演出を先に止め、保存されていた衝突設定を通常状態へ戻します。
    if (mover_) {
        mover_->ResetTransientState();
    }

    // ゲート帰還がワープ後にも座標を動かさないよう、再生中なら途中で止める。
    if (gateReturnAnimation_.IsActive()) {
        gateReturnAnimation_.Stop(this, false);
    }

    // 演出に拘束中なら、位置を指定する前に拘束状態を終了する。
    if (isCinematicLocked_) {
        EndCinematicLock(false);
    }

    // 攻撃・コピー能力の更新が次フレームに残らないよう、各セッションを明示的に破棄します。
    // 通常攻撃の管理があれば、操作権を戻さない指定で技を止める。
    if (baseCombatController_) {
        baseCombatController_->Cancel(*this, false);
    }

    // コピー攻撃の管理があれば、発動中の攻撃と表示を停止する。
    if (copyAbilityController_) {
        copyAbilityController_->Cancel(*this);
    }

    CancelEnemyMorph();
    ReleaseCarriedEnemy(true);

    // 感電は固定座標を毎フレーム再適用するため、ワープ座標を設定する前に必ず解除します。
    if (electricShockFeedbackTimer_ > 0.0f || electricShockControlLocked_) {
        electricShockPendingInvincibleDuration_ = 0.0f;
        EndElectricShockFeedback();
    }

    RestoreDamageBlinkVisibility();
    damageCooldownTimer_ = 0.0f;
    guardContactCooldownTimer_ = 0.0f;
    evasionInvincibleTimer_ = 0.0f;
    invincibleBlinkTimer_ = 0.0f;
    isEvasionInvincible_ = false;
    SetDamageInvincible(false);
    SetDashInvincible(false);
    SetGuarding(false);

    pendingAttack2_ = false;
    attackInputBuffer_.ClearPendingInput();
    forcedSlimeAnimationModeActive_ = false;
    aimTargetObject_ = nullptr;

    // フックの表示物が作られていれば、ワープ前の照準を隠す。
    if (hookMarker_) {
        hookMarker_->SetIsVisible(false);
    }

    // 待機動作と基準姿勢へ揃え、呼出側が新しい座標を設定するまで速度と入力を止める。
    ClearSlimeAbilityMotion();
    ChangeState(std::make_unique<PlayerStateIdle>());
    slimeAnimator_.Reset(managedBaseScale_, this);
    slimeAnimator_.SetMode(PlayerSlimeAnimator::Mode::kIdle);

    engine::Vector3 stableRotation = GetRotation();
    stableRotation.x = baseRotation_.x;
    stableRotation.z = baseRotation_.z;
    SetScale(managedBaseScale_);
    SetRotation(stableRotation);
    SetVelocity({0.0f, 0.0f, 0.0f});
    SetIsControlActive(false);
    SetIsVisible(true);
    UpdateColor();
}
#endif
