#include <cstdint>
#include "game/actor/player/Player.Internal.h"
#include "game/actor/gimmick/movement/GimmickTrampoline.h"
#include "GhostRecorder.h"
#include "BaseScene.h"

using namespace playerImplementation;

Player::~Player() = default;

bool Player::IsImpactBreakActive() const {
    // 通常攻撃かコピー能力のいずれかが衝撃で壊すギミックに対応する段階ならtrue。破壊は接触側で判定する。
    const bool baseImpact = baseCombatController_ && baseCombatController_->CanBreakImpactGate();
    const bool copyImpact = copyAbilityController_ && copyAbilityController_->CanBreakImpactGate();
    return baseImpact || copyImpact;
}

bool Player::IsPinkBounceSlamImpactActive() const {
    // ピンク能力の跳ね上がり技で、着地衝撃が有効な間かを能力コントローラーへ問い合わせる。
    return copyAbilityController_ && copyAbilityController_->IsPinkBounceSlamImpactActive();
}

void Player::Initialize(engine::Object3DCommon* common, engine::InputManager* inputManager, engine::ParticleSystem* particleSystem,
                        engine::SpriteCommon* spriteCommon) {
    // Characterの共通初期化を通してから、入力・移動・攻撃・描画のPlayer固有状態を用意する。
    Character::Initialize(common);

    // 入力と粒子の共通基盤は借用し、通常攻撃とコピー能力のコントローラーはPlayerが所有する。
    inputManager_ = inputManager;
    particleSystem_ = particleSystem;
    baseCombatController_ = std::make_unique<PlayerBaseCombatController>();
    copyAbilityController_ = std::make_unique<PlayerCopyAbilityController>();

    // 自機としての基本設定
    SetClassName("Player");
    SetSaveCategory("Player");

    // 共通初期化でパラメーターが未作成の場合は、基本値を設定する。
    if (!GetParameters().has_value()) {
        ResetParameters();
    }

    LoadWhipSettings();
    jumpCount_ = 0;

    // スライムの潰れ・伸びは見た目だけに適用し、接地判定の高さを変化させません。
    // コライダーが描画scaleへ追従すると、歩行周期ごとに押し戻しが発生して
    // Player座標と追従カメラが上下してしまいます。
    // コライダーが存在する場合だけ、描画の伸縮と別の基準倍率を設定する。
    if (GetCollider()) {
        GetCollider()->SetScaleOverride(managedBaseScale_);
    }

    // 移動コンポーネントの構築
    mover_ = std::make_unique<PlayerMover>();
    mover_->Initialize(this, inputManager, particleSystem);
    auto* statusManager = GameplayStatusManager::GetInstance();
    statusManager->Initialize();
    statusManager->ApplyPlayerStatus(this, true);

    // サイズが設定値と同じ場合はApplyManagedScaleが省略されるため、
    // 移動アニメーションの初期化はプレイヤー自身でも必ず行います。
    slimeAnimator_.Reset(managedBaseScale_, this);

    // ステートマシン初期化 (待機状態からスタート)
    ChangeState(std::make_unique<PlayerStateIdle>());

    // 3D共通基盤がある場合に、フックの到達点とコピー解除時の残像を生成する。
    if (common) {
        hookMarker_ = std::make_unique<engine::Object3D>();
        hookMarker_->Initialize(common);
        hookMarker_->SetModel("Characters/slimeBody");

        // 緑の到達点を半透明で示し、フックを使うまでは隠す。
        hookMarker_->SetColor({0.3f, 1.0f, 0.5f, 0.6f});
        hookMarker_->SetIsVisible(false);

        // 表示用の目印が接触に参加しないよう両方の衝突ビットを外す。
        hookMarker_->SetCollisionAttribute(0);
        hookMarker_->SetCollisionMask(0);

        hookMarker_->SetScale({0.5f, 0.5f, 0.5f});

        // 解除時に排出するスライム成分。シーンへ登録せず、Playerが描画まで管理します。
        enemyMorphReleaseVisual_ = std::make_unique<engine::Object3D>();
        enemyMorphReleaseVisual_->Initialize(common);
        enemyMorphReleaseVisual_->SetName("EnemyMorphReleaseResidue");
        enemyMorphReleaseVisual_->SetClassName("Effect");
        enemyMorphReleaseVisual_->SetModel("Characters/slimeBody");
        enemyMorphReleaseVisual_->SetTexture("Resources/sprite/common/white.png");
        enemyMorphReleaseVisual_->SetMaterialType(0);
        enemyMorphReleaseVisual_->SetEmissive(1.35f);
        enemyMorphReleaseVisual_->SetCollisionAttribute(0);
        enemyMorphReleaseVisual_->SetCollisionMask(0);
        enemyMorphReleaseVisual_->SetIsVisible(false);
    }
}

void Player::ApplyManagedScale(const engine::Vector3& scale) {
    // 見た目、衝突形状、アニメーションの基準スケールを同時に更新する。
    managedBaseScale_ = scale;
    SetScale(scale);

    // 衝突形状がある場合は、描画の基準と同じ倍率を固定値として更新する。
    if (GetCollider()) {
        GetCollider()->SetScaleOverride(scale);
    }

    slimeAnimator_.Reset(scale, this);
}

engine::Vector3 Player::GetLockOnAttackDirection() const {
    // 演出中や照準が無効な間は、前回のロックオン位置を攻撃に使わない。
    if (!isLockingOn_ || !lockOnAimValid_ || IsDead() || isCinematicLocked_)
        return GetForwardDirection();
    engine::Vector3 direction = lockOnAimPoint_ - GetWorldPosition();
    direction.y = 0.0f;
    const float length = engine::Math::Length(direction);
    return length > 0.001f ? direction / length : GetForwardDirection();
}

engine::Vector3 Player::GetMoveInputDirection() {
    // 攻撃中の方向転換にも通常移動と同じ入力解釈を使う。
    return mover_ ? mover_->GetMoveInputDirection() : engine::Vector3{};
}

// 被弾表示の期限を更新してから入力・状態・移動を進めます。演出中は通常操作へ流しません。
void Player::Update(float deltaTime) {
    // 操作を止める状態でも、被弾時の顔表示は先に更新する。
    UpdateHurtFace(deltaTime);
    // 接地通知は毎更新で消去する。死亡や演出の早期returnでも乗車が残らないようにする。
    if (!copyPreviewActive_ && deltaTime > 0.0f) {
        // 本編の時間が進む場合だけ、現シーンの録画パスに残った前フレームの乗車通知を外す。
        if (auto* scene = SceneManager::GetInstance()->GetCurrentScene()) {
            for (const auto& object : scene->GetObjects())
                if (object && object->GetRecorder()) object->GetRecorder()->SetPlayerRiding(false);
        }
    }
    const auto notifyRiding = [&](engine::Object3D* ground) {
        // 本編で生存し、時間が進んでいて上昇中でない場合だけ接地を通知する。子に乗った場合も祖先の録画パスまでたどる。
        if (copyPreviewActive_ || IsDead() || deltaTime <= 0.0f || GetVelocity().y > 0.1f) return;
        for (auto* object = ground; object; object = object->GetParent())
            if (object->GetRecorder()) object->GetRecorder()->SetPlayerRiding(true);
    };
    // ジャンプ入力は移動より先に記録し、演出や感電などで操作できない状態を入力側にも渡す。
    // 移動処理があり時間が進む場合だけ、制御制限と一緒にジャンプ入力を採取する。
    if (mover_ && deltaTime > 0.0f) {
        mover_->SampleJumpInput(deltaTime, IsDead() || isCinematicLocked_ || gateReturnAnimation_.IsActive() ||
                                               absorbEffectActive_ ||
                                               enemyMorphReleaseActive_ || copyPreviewActive_ ||
                                               electricShockControlLocked_);
    }

    // 隔離プレビューは固定刻み側だけが状態を進めます。Scene側のUpdate(0)で
    // 接地や保留入力を再評価すると、コマ送り結果が同じフレーム内で壊れます。
    if (copyPreviewActive_ && deltaTime <= 0.0f) {
        UpdateWorldMatrix(true);
        return;
    }

    // プレビューはエディターから渡した入力で進めるため、通常のプレイヤー操作を切る。
    if (copyPreviewActive_)
        SetIsControlActive(false);

    // 最初のフレームだけ、配置値を復活地点と姿勢の復元基準として控える。
    if (isFirstUpdate_) {
        respawnPosition_ = GetTranslate();
        baseRotation_ = ResolveTransformEuler(*GetTransform());
        SetRotation(baseRotation_);
        isFirstUpdate_ = false;
    }

    // ムービーが操作を握る間は速度も消し、通常の状態・物理更新へ進ませません。
    if (isCinematicLocked_) {
        CancelWhipReturn();
        SetIsControlActive(false);
        SetVelocity({0.0f, 0.0f, 0.0f});
        return;
    }

    // ゲート復帰は専用アニメーションが座標を決めるため、通常移動を重ねません。
    if (gateReturnAnimation_.IsActive()) {
        CancelWhipReturn();
        gateReturnAnimation_.Update(this, deltaTime);
        UpdateEnemyMorph(deltaTime);
        return;
    }

    // パネル走行をフックや硬直技で任意に中断できないよう、受付時点の状態を共有する。
    const bool dashPanelDriving = IsDashPanelBoostActive();

    // コピー能力とフックは同じ副操作を使うため、形態から今フレームの入力先を決めます。
    const bool copyAbilityInputMode =
        isEnemyMorphed_ && enemyMorphType_ != EnemyMorphType::kNone && !enemyMorphReleaseActive_ && !absorbEffectActive_;
    const bool hookTriggered =
        deltaTime > 0.0f && isControlActive_ && inputManager_ && inputManager_->IsActionTriggered("SecondaryAction") &&
        !dashPanelDriving && !copyAbilityInputMode && !carriedEnemy_ && !copyPreviewActive_ && !enemyMorphReleaseActive_ &&
        !absorbEffectActive_ && !IsDead();

    // コピー能力か運搬中はフックに入力を渡さないため、既存マーカーと照準参照を消す。
    if (copyAbilityInputMode || carriedEnemy_) {
        if (hookMarker_) {
            hookMarker_->SetIsVisible(false);
        }

        aimTargetObject_ = nullptr;
    }

    // 停止フレームでは入力・タイマー・状態遷移を進めず、表示用の更新だけを残します。
    if (deltaTime > 0.0f) {
        // 残りが正の間だけ被弾タイマーを進め、期限を超えたフレームで無敵を外す。
        if (damageCooldownTimer_ > 0.0f) {
            damageCooldownTimer_ -= deltaTime;

            if (damageCooldownTimer_ <= 0.0f) {
                damageCooldownTimer_ = 0.0f;
                SetDamageInvincible(false);
            }
        }

        guardContactCooldownTimer_ = (std::max)(0.0f, guardContactCooldownTimer_ - deltaTime);

        // 回避無敵は別の期限を持ち、期限が切れたら無敵と本体点滅用の色を戻す。
        if (evasionInvincibleTimer_ > 0.0f) {
            evasionInvincibleTimer_ -= deltaTime;

            if (evasionInvincibleTimer_ <= 0.0f) {
                evasionInvincibleTimer_ = 0.0f;
                isEvasionInvincible_ = false;
                // 本体色を変える種類の回避に限り、解除時の色を更新する。
                if (evasionFeedback_ == EvasionFeedback::kBodyFlash)
                    UpdateColor();
                evasionFeedback_ = EvasionFeedback::kBodyFlash;
            }
        }

        // 感電の拘束位置を移動処理の前に復元し、通常の入力移動へ進ませないようにします。
        if (electricShockControlLocked_) {
            SetIsControlActive(false);
            SetVelocity({0.0f, 0.0f, 0.0f});
            SetTranslate(electricShockLockedPosition_);
        }

        // 操作と移動が有効なら、鞭の発射を優先する。発射しないフレームでは通常の移動入力を処理する。
        if (isControlActive_ && mover_) {
            if (hookTriggered) {
                // 押したフレームで発射し、長押し・解除では追加発射しません。
                ChangeState(std::make_unique<PlayerStateWhip>(GetWhipDirection()));
            } else {
                mover_->Update(deltaTime);
            }
        }

        // 移動を決めた後に状態を進め、攻撃・被弾による制御変更を反映します。
        // 現在の状態が存在する場合だけ、攻撃や被弾を含む個別の状態を進める。
        if (state_) {
            state_->Update(this, deltaTime);
        }

        // チュートリアルの保護が有効でHPを保持できる場合、毎フレーム最大値へ戻す。
        if (tutorialSafetyEnabled_ && GetParameters().has_value()) {
            SetParameter(&engine::EntityParameter::maxHp, (std::max)(GetParameters()->maxHp, 1.0f));
            SetParameter(&engine::EntityParameter::hp, GetParameters()->maxHp);
        }

        // 保護外で生存し、落下退場に移る前に致死液面に触れた場合は、液面へ足元をそろえてHPをゼロにする。
        float liquidY = 0.0f;
        if (!IsDead() && !tutorialSafetyEnabled_ && !dynamic_cast<PlayerStateFallingOut*>(state_.get()) &&
            FindLethalLiquidSurface(*this, liquidY) && GetParameters().has_value()) {
            const float footY = GetModelWorldAabb().min.y;
            engine::Vector3 position = GetTranslate();
            position.y += liquidY - footY + 0.04f;
            SetTranslate(position);
            UpdateLocalMatrix();
            UpdateWorldMatrix();
            SetVelocity({0.0f, 0.0f, 0.0f});
            SetParameter(&engine::EntityParameter::hp, 0.0f);
        }

        // 死亡へ入る前に攻撃とコピーを解除し、イベントを一度だけ通知します。
        if (GetHp() <= 0.0f && !IsDead()) {
            // 通常攻撃の管理があれば、死亡状態へ入る前に攻撃も中断する。
            if (baseCombatController_) {
                baseCombatController_->Cancel(*this, false);
            }

            CancelEnemyMorph();
            SetDead(true);
            deathTimer_ = 0.0f;
            ChangeState(std::make_unique<PlayerStateDead>());

            PlayerDeathEvent deathEvent;
            deathEvent.player = this;
            EventManager::GetInstance()->Dispatch(deathEvent);
        }

        // 落下限界を下回った生存中の操作対象を処理する。チュートリアルの保護がある場合だけ残機を消費しない。
        if (GetTranslate().y < -18.0f && !IsDead() && isControlActive_) {
            if (baseCombatController_) {
                baseCombatController_->Cancel(*this, false);
            }

            // 保護中は復活位置へ直接戻す。通常の落下では専用状態へ移して残機を減らす。
            if (tutorialSafetyEnabled_) {
                SetTranslate(respawnPosition_);
                SetVelocity({0.0f, 0.0f, 0.0f});
            } else {
                ChangeState(std::make_unique<PlayerStateFallingOut>());
                GameDataManager::GetInstance()->SubtractLife();
            }
        }

        // 生存中でHPが0より大きく1以下なら赤い画面枠を出し、範囲から外れたフレームで消す。
        if (!IsDead() && GetHp() <= 1.0f && GetHp() > 0.0f) {
            PostEffect::GetInstance()->GetParams()->dangerVignette = 0.8f;
        } else {
            PostEffect::GetInstance()->GetParams()->dangerVignette = 0.0f;
        }
    }

    // 変身元への参照は形態が有効な間だけ渡し、解除後の能力処理へ持ち越しません。
    BaseEnemy* activeMorphSource = isEnemyMorphed_ ? enemyMorphSource_ : nullptr;

    // 長押し照準は廃止したため、鞭の入力中も設定済みの重力を維持します。
    const float configuredMaxFallSpeed = GetParameters().has_value() ? GetParameters()->maxFallSpeed : 60.0f;
    const float abilityFallSpeed = GetAttackMovement().maxFallSpeedOverride;
    const bool hasTemporaryPhysicsOverride = isControlActive_ && GetParameters().has_value() && abilityFallSpeed > 0.0f;
    // 操作可能でパラメーターがあり、能力の上限が正の間だけ一時的に落下速度を制限する。
    if (hasTemporaryPhysicsOverride) {
        // 能力の落下上限をこの物理更新だけ使い、元のステータスには残さない。
        SetParameter(&engine::EntityParameter::maxFallSpeed, std::min(configuredMaxFallSpeed, abilityFallSpeed));
    }

    // 状態と入力を確定してから物理を進め、能力入力に必要な接地を再確認します。
    // 死亡中は位置を動かさず表示の更新だけ行う。生存中はCharacterの物理更新へ進む。
    const float landingApproachVelocityY = GetVelocity().y;
    if (IsDead()) {
        CancelDashPanelBoost();
        SetVelocity({0.0f, 0.0f, 0.0f});
        engine::Object3D::Update(deltaTime);
    } else {
        Character::Update(deltaTime);

        // Sweep 移動で着地面の手前に停止すると重なり判定が発生しないため、
        // Motor が確定した接地相手からもキノコの反発を受け取る。
        const CharacterMotorResult& motorResult = GetCharacterMotor().GetResult();

        // 掃引は接触の手前で止まるため、通常のOnCollision()が発生しない場合もここで扱う。
        auto* horizontalHit = motorResult.horizontalHitObject;
        if (deltaTime > 0.0f && horizontalHit && mover_ &&
            (horizontalHit->GetCollisionAttribute() & engine::collisionCategories::Get("ground")) &&
            mover_->IsDashPanelWall(motorResult.horizontalHitNormal)) {
            auto* block = dynamic_cast<GimmickBreakableBlock*>(horizontalHit);
            // パネルで壊したガラスは通過扱いにし、次のフレームも前進する。
            if (!block || !block->TryBreakByPlayerImpact(this))
                mover_->StopDashPanelAtWall();
        }

        // プレビューではない更新でモーターが床を特定できたら、その床の録画パスへ乗車を伝える。
        if (!copyPreviewActive_ && motorResult.grounded && motorResult.groundObject) {
            notifyRiding(motorResult.groundObject);
            // 床がトランポリンなら、重なりイベントがなくても着地法線を使って反発を確認する。
            if (auto* trampoline = dynamic_cast<GimmickTrampoline*>(motorResult.groundObject)) {
                trampoline->TryBounce(this, motorResult.groundNormal);
            }
        }

        // 互換モーターは接地をリセットし、シーン末尾の衝突解決まで未接地になります。
        // 能力判定より先に実際の足元接触を確認します。押し戻し・衝突イベントは
        // 従来のCollisionManagerに任せ、ここでは位置や速度を変更しません。
        // 連続衝突を使わない本編の前進更新で、未接地かつ下降中の場合だけ当フレームの足元を確かめる。
        if (!copyPreviewActive_ && deltaTime > 0.0f && GetParameters().has_value() && !GetParameters()->motorContinuousCollision &&
            !IsGrounded() && GetVelocity().y <= 0.0f) {
            UpdateWorldMatrix(false);
            engine::PhysicsQueryFilter filter;
            filter.mask = engine::collisionCategories::Get("ground") & GetCollisionMask();
            filter.ignoredObject = this;
            const float maxSlope = std::clamp(GetParameters()->motorMaxSlopeDegrees, 0.0f, 89.9f);
            const float minimumGroundNormalY = std::cos(maxSlope * kPi / 180.0f);

            for (const engine::PhysicsOverlapHit& hit : engine::CollisionManager::GetInstance()->OverlapAabb(GetAabb(), filter)) {
                // 広域の箱判定だけで接地扱いにせず、自機本来のCollider形状で確認します。
                const engine::CollisionInfo contact = CheckCollision(hit.object);

                // 実形状で接触していて、傾斜の上限を守る法線の場合だけ床として採用する。
                if (contact.isColliding && contact.normal.y > minimumGroundNormalY) {
                    SetGrounded(true);
                    notifyRiding(hit.object);
                    break;
                }
            }
        }

        // 隔離空間には物理床がないため、モーターの更新後・能力入力の前に床を解決します。
        // 更新前のSetGroundedだけではBeginFrameで消され、ガードが開始できません。
        if (copyPreviewActive_) {
            const bool grounded = GetTranslate().y <= copyPreviewGroundHeight_ + 0.001f && GetVelocity().y <= 0.0f;

            // 簡易床の高さに達し下降中なら、足元とY速度を床にそろえる。
            if (grounded) {
                SetTranslateY(copyPreviewGroundHeight_);
                SetVelocityY(0.0f);
            }

            SetGrounded(grounded);
            UpdateWorldMatrix(true);
        }
    }

    // 接地の確定後に、物理更新直前の落下速度から着地演出の強さを決める。
    // Moverを作成済みのとき、状態ごとに着地反応を止める条件を渡す。
    if (mover_) {
        const bool allowLandingFeedback = !IsDead() && !isCinematicLocked_ && !gateReturnAnimation_.IsActive() &&
                                          !dynamic_cast<PlayerStateDamage*>(state_.get()) && !absorbEffectActive_ &&
                                          !enemyMorphReleaseActive_ && electricShockFeedbackTimer_ <= 0.0f;
        const float impactRate = mover_->UpdateLandingFeedback(landingApproachVelocityY, deltaTime, allowLandingFeedback);
        // 正の衝撃係数が返ったときだけ着地の潰れを開始する。
        if (impactRate > 0.0f) {
            slimeAnimator_.TriggerLanding(impactRate);
        }
    }

    // このフレームの落下速度上限を使い終えたら、保存対象の設定値へ戻す。
    if (hasTemporaryPhysicsOverride && GetParameters().has_value()) {
        SetParameter(&engine::EntityParameter::maxFallSpeed, configuredMaxFallSpeed);
    }

    // 感電中は物理更新後のずれも抑え、拘束位置に固定してから視覚効果を更新します。
    if (electricShockControlLocked_) {
        SetVelocity({0.0f, 0.0f, 0.0f});
        SetTranslate(electricShockLockedPosition_);
    }

    UpdateElectricShockFeedback(deltaTime);

    // 被弾、ダッシュ、本体点滅を伴う回避のいずれかが有効で時間が進む場合に点滅時計を進める。
    const bool evasionFlash = isEvasionInvincible_ && evasionFeedback_ == EvasionFeedback::kBodyFlash;
    const bool hasVisibleInvincibility = isDamageInvincible_ || isDashInvincible_ || evasionFlash;
    if (hasVisibleInvincibility && deltaTime > 0.0f) {
        invincibleBlinkTimer_ += deltaTime;
    }

    UpdateDamageInvincibleBlinkVisibility();

    // 感電の色を優先し、それ以外の無敵中は種別に応じた色を混ぜる。
    if (electricShockFeedbackTimer_ <= 0.0f && hasVisibleInvincibility && deltaTime > 0.0f) {
        const float blink = 0.5f + 0.5f * std::sin(invincibleBlinkTimer_ * 42.0f);
        const engine::Vector4 base = isEnemyMorphed_ ? GetEnemyMorphTint(enemyMorphType_) : engine::Vector4{1.0f, 1.0f, 1.0f, 1.0f};
        const engine::Vector4 flash = (isDashInvincible_ || evasionFlash)
                                          ? engine::Vector4{0.34f, 0.74f, 1.0f, 1.0f}
                                          : engine::Vector4{1.0f, 0.78f, 0.32f, 1.0f};
        const float flashRate = blink > 0.48f ? 0.72f : 0.12f;
        const engine::Vector4 color = {base.x * (1.0f - flashRate) + flash.x * flashRate,
                               base.y * (1.0f - flashRate) + flash.y * flashRate,
                               base.z * (1.0f - flashRate) + flash.z * flashRate, 1.0f};
        SetColor(color);

        // 通常形態では、有効な子パーツにも同じ色を反映する。
        if (!isEnemyMorphed_) {
            for (engine::Object3D* child : GetChildren()) {
                if (child) {
                    child->SetColor(color);
                }
            }
        }
    } else if (!hasVisibleInvincibility) {
        // 点滅対象の無敵が終わったら次の開始に備えて時計を戻す。
        invincibleBlinkTimer_ = 0.0f;
    }

    // 物理と接地の確定後に、通常入力かプレビュー入力から能力用のボタン状態を用意する。
    PlayerCopyAbilityInput abilityInput;
    bool morphReleaseTriggered = false;
    // パネル走行中は新しい能力入力を預からず、進行中の技には入力を離した状態を渡す。
    if (deltaTime > 0.0f && !dashPanelDriving) {
        if (copyPreviewActive_) {
            abilityInput = copyPreviewInput_;
        } else {
            abilityInput.specialTriggered = inputManager_->IsActionTriggered("Ability");
            abilityInput.secondaryTriggered = inputManager_->IsActionTriggered("SecondaryAction");
            abilityInput.secondaryHeld = inputManager_->IsActionPressed("SecondaryAction");
            abilityInput.primaryTriggered = inputManager_->IsActionTriggered("PrimaryAttack");
            abilityInput.primaryHeld = inputManager_->IsActionPressed("PrimaryAttack");
            morphReleaseTriggered = inputManager_->IsActionTriggered("MorphRelease");
        }
    }

    // 通常攻撃の入力は、鞭の開始フレームや引き寄せ状態では抑制する。運搬とコピーの入力先は別途優先順に選ぶ。
    const bool hookActionActive = dynamic_cast<PlayerStateWhip*>(state_.get()) ||
                                  dynamic_cast<PlayerStatePullEnemy*>(state_.get()) ||
                                  dynamic_cast<PlayerStatePullObject*>(state_.get());
    const bool baseAttackTriggered =
        deltaTime > 0.0f && !dashPanelDriving && !hookTriggered && !hookActionActive &&
        ((!copyPreviewActive_ && inputManager_->IsActionTriggered("Attack")) || abilityInput.primaryTriggered);
    // 通常攻撃の管理を初期化済みなら、入力の受理判定の後に攻撃を更新する。
    if (baseCombatController_) {
        baseCombatController_->ProcessInput(*this, baseAttackTriggered);
        baseCombatController_->Update(*this, deltaTime);
    }

    // 通常のコピー解除は、吸収・別の解除演出・死亡に割り込ませません。
    if (!absorbEffectActive_ && isEnemyMorphed_ && !enemyMorphReleaseActive_ && isControlActive_ && !IsDead() &&
        morphReleaseTriggered) {
        BeginEnemyMorphRelease(false);
    }

    // 投擲が許可され、未変身で運搬中、吸収・解除の外で攻撃ボタンを保持している場合に敵を投げる。
    if (!dashPanelDriving && tutorialCarryThrowEnabled_ && !enemyMorphReleaseActive_ && !absorbEffectActive_ && carriedEnemy_ &&
        !isEnemyMorphed_ && inputManager_->IsActionPressed("PrimaryAttack")) {
        ThrowCarriedEnemy();
    } else if (!enemyMorphReleaseActive_ && !absorbEffectActive_ && copyAbilityController_ &&
               copyAbilityController_->HandlesMorphType(static_cast<std::int32_t>(enemyMorphType_))) {
        // 被弾・感電中に新しい技を始めると、ノックバック中の操作や専用モーションを上書きしてしまう。
        if (!IsDead() && !electricShockControlLocked_ && !dynamic_cast<PlayerStateDamage*>(state_.get()))
            copyAbilityController_->ProcessInput(*this, abilityInput);
    } else if (!enemyMorphReleaseActive_ && !absorbEffectActive_ && activeMorphSource && abilityInput.specialTriggered) {
        // 共通の能力処理を選ばなかった場合は、変身元が存在し能力ボタンが押されたフレームに旧能力を呼ぶ。
        activeMorphSource->ExecuteAbility(this);
    } else if (tutorialCarryAbsorbEnabled_ && !enemyMorphReleaseActive_ && !absorbEffectActive_ && carriedEnemy_ &&
               abilityInput.specialTriggered) {
        // ここまでの操作が成立せず、吸収が許可されている運搬中に能力を押した場合は、保持している敵を調べる。
        BaseEnemy* enemyBase = dynamic_cast<BaseEnemy*>(carriedEnemy_);

        // 保持中の対象が敵の場合だけ吸収できる。未変身なら設定に応じ、演出を経由するか即時に変身する。
        if (enemyBase) {
            if (!isEnemyMorphed_) {
                if (ShouldPlayAbsorbEffect(enemyBase)) {
                    BeginAbsorbEffect(enemyBase);
                } else {
                    StartEnemyMorph(enemyBase);
                }
            } else {
                // 吸収済みのときだけ、拘束中の敵能力を発動する。
                enemyBase->ExecuteAbility(this);
            }
        }
    }

    // 吸収中は専用演出を進め、それ以外の運搬中は姿勢を追従させて保持状態を更新する。
    if (absorbEffectActive_) {
        UpdateAbsorbEffect(deltaTime);
    } else if (carriedEnemy_ && !isEnemyMorphed_) {
        SetSlimeAnimationMode(PlayerSlimeAnimator::Mode::kCarry);
        BaseEnemy* carriedBase = dynamic_cast<BaseEnemy*>(carriedEnemy_);

        // 保持中の敵がまだ運搬状態になっていなければ、その状態へ切り替える。
        if (carriedBase && !carriedBase->IsCarried()) {
            carriedBase->SetCarried(true);
        }

        carriedEnemy_->SetCollisionAttribute(0);
        carriedEnemy_->SetCollisionMask(0);

        engine::Vector3 playerPos = GetWorldPosition();

        static float sStruggleTimer = 0.0f;
        sStruggleTimer += deltaTime;

        // 運搬中の敵だけ小さく揺らし、拘束されていることを見た目で示します。
        float offsetX = std::sin(sStruggleTimer * 35.0f) * 0.15f;
        float offsetZ = std::cos(sStruggleTimer * 30.0f) * 0.15f;
        float offsetY = std::sin(sStruggleTimer * 45.0f) * 0.08f;

        carriedEnemy_->SetTranslate({playerPos.x + offsetX, playerPos.y + 1.55f + offsetY,
                                                    playerPos.z + offsetZ});

        engine::Vector3 rot;
        rot.x = std::sin(sStruggleTimer * 20.0f) * 0.2f;
        rot.y = GetRotation().y + std::sin(sStruggleTimer * 15.0f) * 0.4f;
        rot.z = std::cos(sStruggleTimer * 22.0f) * 0.2f;
        carriedEnemy_->SetEulerAngles(rot);
        carriedEnemy_->SetQuaternionMaster(false);

        // 揺れによる拡縮が毎フレーム累積しないよう、保存した元のスケールを基準にする。
        if (!hasCarriedEnemyBaseScale_) {
            carriedEnemyBaseScale_ = GetStableEnemyCarryScale(carriedEnemy_);
            hasCarriedEnemyBaseScale_ = true;
        }

        float stretch = std::sin(sStruggleTimer * 25.0f) * 0.05f;
        const engine::Vector3 targetScale = {carriedEnemyBaseScale_.x * (1.0f - stretch),
                                     carriedEnemyBaseScale_.y * (1.0f + stretch),
                                     carriedEnemyBaseScale_.z * (1.0f - stretch)};
        carriedEnemy_->SetScale(engine::Math::Lerp(carriedEnemy_->GetScale(), targetScale, std::clamp(deltaTime * 10.0f, 0.0f, 1.0f)));

        // 親の更新を待たずに行列を確定し、手元への追従を1フレーム遅らせません。
        carriedEnemy_->UpdateLocalMatrix();
        carriedEnemy_->UpdateWorldMatrix();

        // 通常の運搬では、対象が敵なら姿勢を更新した後で運搬中の能力を進める。
        if (carriedBase) {
            carriedBase->UpdateCarriedAbility(this, deltaTime);
        }
    // コピー中は元の敵を隠したまま、能力に必要な更新だけを残す。
    } else if (carriedEnemy_ && isEnemyMorphed_) {
        BaseEnemy* carriedBase = dynamic_cast<BaseEnemy*>(carriedEnemy_);
        carriedEnemy_->SetIsVisible(false);
        carriedEnemy_->SetCollisionAttribute(0);
        carriedEnemy_->SetCollisionMask(0);

        // 隠して保持している変身元が敵であれば、その能力の更新を続ける。
        if (carriedBase) {
            carriedBase->UpdateCarriedAbility(this, deltaTime);
        }
    }

    // コピー解除が始まったら能力の更新を止め、対応する形態だけ共通コントローラーへ渡す。
    if (copyAbilityController_ && copyAbilityController_->HandlesMorphType(static_cast<std::int32_t>(enemyMorphType_)) &&
        !enemyMorphReleaseActive_) {
        copyAbilityController_->Update(*this, deltaTime);
    } else if (activeMorphSource && activeMorphSource != carriedEnemy_ && !enemyMorphReleaseActive_) {
        // 旧能力の変身元が運搬対象と別である場合は、同じ個体を二重に更新せずここでだけ追従処理を行う。
        activeMorphSource->SetIsVisible(false);
        activeMorphSource->SetCollisionAttribute(0);
        activeMorphSource->SetCollisionMask(0);
        activeMorphSource->UpdateCarriedAbility(this, deltaTime);
    }

    // 攻撃側が指定した姿勢を今回の描画へ反映し、次の更新まで同じ要求を残しません。
    if (forcedSlimeAnimationModeActive_) {
        slimeAnimator_.SetMode(forcedSlimeAnimationMode_);
        slimeAnimator_.SetMotionDirection(forcedSlimeAnimationDirection_);
        forcedSlimeAnimationModeActive_ = false;
    }

    UpdateWhipReturn(deltaTime);

    // 被弾・吸収・解除などが独自の姿勢を決めている間は、通常アニメーションで上書きしません。
    if (!dynamic_cast<PlayerStateDamage*>(state_.get()) && !absorbEffectActive_ && !enemyMorphReleaseActive_ &&
        electricShockFeedbackTimer_ <= 0.0f && !IsDead()) {
        slimeAnimator_.Update(this, deltaTime);
    }

    UpdateEnemyMorph(deltaTime);
    // 本体の描画姿勢が確定してから接続し、付け根の一フレーム遅れを防ぎます。
    RefreshWhipVisual();

    // 能力による跳ね上がりも含めて接地確定後の状態を渡し、ゲーム固有の型はカメラへ持ち込まない。
    if (auto* camera = CameraManager::GetInstance()->GetMainCamera(); camera && camera->GetFollowTarget() == this)
        camera->SetFollowTargetGrounded(IsGrounded());
}

// 運搬中の敵を一時的な投擲へ切り替え、最後にPlayerの保持参照を解放する。
void Player::ThrowCarriedEnemy() {
    BaseEnemy* enemyBase = dynamic_cast<BaseEnemy*>(carriedEnemy_);

    // 運搬対象が敵型の場合にだけ、変身解除と投擲初速の設定を行う。
    if (enemyBase) {
        CancelEnemyMorph();

        // カメラの水平方向を投擲方向に使い、見下ろし角に応じて上向きの初速を調整します。
        engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();
        engine::Vector3 throwForward = GetForwardDirection();
        float throwUpSpeed = 17.0f;

        // 主カメラがあればヨーを水平の投擲方向とし、ピッチで上向き初速を10～21へ調整する。
        if (camera) {
            engine::Vector3 camRot = camera->GetRotation();
            throwForward = {std::sin(camRot.y), 0.0f, std::cos(camRot.y)};
            throwUpSpeed = (std::clamp)(17.0f - std::sin(camRot.x) * 7.0f, 10.0f, 21.0f);
        }

        float forwardLength = std::sqrt(throwForward.x * throwForward.x + throwForward.z * throwForward.z);

        // 水平方向を取得できれば正規化し、長さがほぼゼロなら+Zを選ぶ。
        if (forwardLength > 0.001f) {
            throwForward.x /= forwardLength;
            throwForward.z /= forwardLength;
        } else {
            throwForward = {0.0f, 0.0f, 1.0f};
        }

        // 解除直後の当たり判定がプレイヤーに重ならないよう、前方に離してから投げます。
        engine::Vector3 playerPos = GetWorldPosition();
        enemyBase->SetTranslate({playerPos.x + throwForward.x * 2.2f, playerPos.y + 2.2f,
                                                playerPos.z + throwForward.z * 2.2f});

        // 運搬状態を解除してから、元の大きさと投擲初速を復元します。
        enemyBase->SetCarried(false);

        enemyBase->SetScale(hasCarriedEnemyBaseScale_ ? carriedEnemyBaseScale_ : enemyBase->GetScale());
        enemyBase->SetEulerAngles({0.0f, 0.0f, 0.0f});
        enemyBase->SetQuaternionMaster(true);

        engine::Vector3 throwVelocity = {throwForward.x * 32.0f, throwUpSpeed, throwForward.z * 32.0f};
        enemyBase->BeginThrown(throwVelocity);

        TriggerSlimeImpulse({1.4f, 3.6f, 1.4f}, 0.18f);

        // CPU粒子が使える場合だけ、投擲と同方向の衝撃粒子を出す。
        if (particleSystem_) {
            engine::Vector3 effectDir = {throwForward.x, 0.35f, throwForward.z};
            float effectDirLength =
                std::sqrt(effectDir.x * effectDir.x + effectDir.y * effectDir.y + effectDir.z * effectDir.z);
            // 粒子の向きが非ゼロなら、上向き成分も含めて正規化する。
            if (effectDirLength > 0.001f) {
                effectDir = effectDir / effectDirLength;
            }

            particleSystem_->SpawnParticles(playerPos + engine::Vector3{0.0f, 2.5f, 0.0f}, 24, 1.6f, &effectDir, 32.0f,
                                            {1.0f, 1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 0.0f}, 0.1f, 0.3f, 0.8f,
                                            0.1f);
        }
    }

    // プレイヤーの所有参照を最後に外し、敵の投擲更新へ引き継ぎます。
    SetCarriedEnemy(nullptr);
    DebugConsole::GetInstance()->AddLog("Throw Enemy!");
}

void Player::DrawUi() {
    // HUD はシーン側で描画するため、Player 自身は UI 描画を持たない。
}

float Player::GetVisualYawOffset() const {
    // 変身中はコピー元モデルに応じた角度、通常はPlayerモデルの既定補正角をラジアンで返す。
    if (isEnemyMorphed_) {
        return GetEnemyMorphModelYawOffset();
    }

    return kPlayerModelYawOffset;
}

float Player::GetEnemyMorphModelYawOffset() const {
    // コピー元モデルの前方軸が Player と逆向きの種類だけ半回転させる。
    switch (enemyMorphType_) {
    case EnemyMorphType::kSlime:
    case EnemyMorphType::kFireSlime:
    case EnemyMorphType::kThunderSlime:
    case EnemyMorphType::kWindSlime:
    case EnemyMorphType::kBomber:
    case EnemyMorphType::kCrown:
        return kPi;

    default:
        return 0.0f;
    }
}

float Player::GetMoveYaw() const {
    // モデルの向き補正を除いた角度を、移動・攻撃側の共通方向とする。
    return NormalizeYaw(GetRotation().y - GetVisualYawOffset());
}

engine::Vector3 Player::GetForwardDirection() const {
    // 上下成分を持たない方向にし、地上攻撃の照準に使う。
    const float yaw = GetMoveYaw();
    return {std::sin(yaw), 0.0f, std::cos(yaw)};
}

void Player::SetMoveYaw(float yaw) {
    // 操作上の正面をモデル固有の前方軸へ変換して配置する。
    SetRotationY(NormalizeYaw(yaw + GetVisualYawOffset()));
}

void Player::StepCopyPreviewObjects(float deltaTime, float groundY) {
    // 編集用の仮オブジェクトだけを進め、本編シーンの敵は更新しない。
    if (!IsEditorInternal())
        return;
    for (auto& object : copyPreviewObjects_) {
        object->Update(deltaTime);

        // 仮オブジェクトが敵型の場合だけ、簡易床で接地を解決する。
        if (auto* enemy = dynamic_cast<BaseEnemy*>(object.get())) {
            engine::Vector3 position = enemy->GetTranslate();
            engine::Vector3 velocity = enemy->GetVelocity();

            // 床の高さに達して下降中ならY速度を止め、水平速度を緩める。
            if (position.y < groundY + 0.48f && velocity.y <= 0) {
                position.y = groundY + 0.48f;
                velocity.y = 0;
                velocity.x *= 0.96f;
                velocity.z *= 0.96f;
                enemy->SetTranslate(position);
                enemy->SetVelocity(velocity);
                enemy->SetGrounded(true);
                enemy->UpdateWorldMatrix(true);
            }
        }
    }
}

void Player::UpdateCopyAdornment() {
    // コピー装飾を同期し、被弾モデルに含まれる間とコピー解除中は別描画を省く。
    const char* model = nullptr;

    const auto* renderer = GetMeshRenderer();
    const bool bandInHurtModel = hurtFaceIncludesBand_ && hurtFaceModel_ && renderer &&
                                 renderer->GetVisualModelOverride() == hurtFaceModel_;
    if (isEnemyMorphed_ && !enemyMorphReleaseActive_ && !bandInHurtModel) {
        switch (enemyMorphType_) {
        case EnemyMorphType::kSlime:
            model = "Characters/copy_band_pink";
            break;

        case EnemyMorphType::kFireSlime:
            model = "Characters/copy_band_fire";
            break;

        case EnemyMorphType::kThunderSlime:
            model = "Characters/copy_band_thunder";
            break;

        case EnemyMorphType::kWindSlime:
            model = "Characters/copy_band_wind";
            break;

        case EnemyMorphType::kBomber:
            model = "Characters/copy_band_bomb";
            break;

        default:
            break; // 覇王は既存モデルに王冠を含みます。
        }
    }

    // 専用バンドがない形態や本体が隠れている間は、既存の装飾だけ隠して再利用します。
    if (!model || !GetIsVisible()) {
        if (copyAdornment_)
            copyAdornment_->SetIsVisible(false);
        return;
    }

    // 能力切替では同じ描画物のモデルを入れ替え、装飾用の衝突形状は作りません。
    if (!copyAdornment_) {
        copyAdornment_ = std::make_unique<engine::Object3D>();
        copyAdornment_->Initialize(GetObjectCommon());
        copyAdornment_->SetColliderType(engine::ColliderType::kNone);
        copyAdornment_->SetCollisionAttribute(0);
        copyAdornment_->SetCollisionMask(0);
        copyAdornment_->SetMaterialType(0);
        copyAdornment_->SetEnableLighting(true);
    }

    // 同じ形態の装飾モデルなら読み直さず、形態が変わった場合にのみ差し替える。
    if (copyAdornment_->GetModelName() != model)
        copyAdornment_->SetModel(model);
    copyAdornment_->SetIsVisible(true);

    // 本体と同じ最終描画行列を親にする。個別コピーでは漏れる演出姿勢やシアーも一致させる。
    const auto* bodyRenderer = GetMeshRenderer();
    copyAdornmentAnchor_.matWorld = bodyRenderer ? bodyRenderer->GetRenderWorldMatrix() : GetWorldMatrix();
    copyAdornment_->SetTransformAnchor(&copyAdornmentAnchor_);
    copyAdornment_->UpdateWorldMatrix(true);
}

void Player::DrawForCamera(engine::Camera* camera, ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource,
                           std::int32_t previewBufferIndex) {
    // 能力装飾と編集用オブジェクトも、本体と同じプレビューカメラで描く。
    Character::DrawForCamera(camera, pointLightResource, spotLightResource, previewBufferIndex);
    // 本体が非表示なら、プレビュー用カメラの追加描画を行わない。
    if (!GetIsVisible())
        return;
    // 装飾と解除残像は実体があり可視の場合、能力描画はコントローラーがある場合に呼ぶ。
    UpdateCopyAdornment();
    if (copyAdornment_ && copyAdornment_->GetIsVisible())
        copyAdornment_->DrawForCamera(camera, pointLightResource, spotLightResource, previewBufferIndex);
    if (copyAbilityController_)
        copyAbilityController_->Draw(*this, pointLightResource, spotLightResource, camera, previewBufferIndex);
    if (enemyMorphReleaseVisual_ && enemyMorphReleaseVisual_->GetIsVisible())
        enemyMorphReleaseVisual_->DrawForCamera(camera, pointLightResource, spotLightResource, previewBufferIndex);
    for (auto& object : copyPreviewObjects_)
        object->DrawForCamera(camera, pointLightResource, spotLightResource, previewBufferIndex);
}

void Player::DrawSpecialMaterialForCamera(engine::Camera* camera, uint32_t depthSrvHandle, uint32_t grabSrvHandle,
                                          std::int32_t previewBufferIndex) {
    // 屈折描画を使う解除残像とプレビュー物を、通常描画とは別のパスへ渡す。
    Character::DrawSpecialMaterialForCamera(camera, depthSrvHandle, grabSrvHandle, previewBufferIndex);
    // 本体が非表示なら、解除残像と仮オブジェクトのプレビューも省く。
    if (!GetIsVisible())
        return;
    auto* lights = engine::LightManager::GetInstance();
    // 解除残像が存在して可視のときは、共通ライトを借りて専用カメラへ描く。
    if (enemyMorphReleaseVisual_ && enemyMorphReleaseVisual_->GetIsVisible())
        enemyMorphReleaseVisual_->DrawForCamera(camera, lights->GetPointLightResource(), lights->GetSpotLightResource(),
                                                previewBufferIndex);
    for (auto& object : copyPreviewObjects_)
        object->DrawForCamera(camera, lights->GetPointLightResource(), lights->GetSpotLightResource(),
                              previewBufferIndex);
}

bool Player::HasOwnedSpecialMaterialVisuals() const {
    // コピー能力の専用描画物は Player の描画パスからまとめて問い合わせる。
    return copyAbilityController_ && copyAbilityController_->HasOwnedVisuals();
}

void Player::DrawOwnedSpecialMaterialVisuals(uint32_t depthSrvHandle, uint32_t grabSrvHandle) {
    // 本体が非表示の演出中に能力の専用エフェクトだけが浮かないようにする。
    if (copyAbilityController_ && GetIsVisible()) {
        copyAbilityController_->DrawOwnedVisuals(*this, depthSrvHandle, grabSrvHandle);
    }
}

void Player::DrawOwnedSpecialMaterialVisualsForCamera(engine::Camera* camera, uint32_t depthSrvHandle, uint32_t grabSrvHandle,
                                                      std::int32_t previewBufferIndex) {
    // プレビュー用カメラにも同じ可視条件を適用する。
    if (copyAbilityController_ && GetIsVisible()) {
        copyAbilityController_->DrawOwnedVisuals(*this, depthSrvHandle, grabSrvHandle, camera, previewBufferIndex);
    }
}

// 通常の描画では本体に続き、生成済みで描画条件を満たす装飾、能力、解除残像、感電を描く。
void Player::Draw(ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource) {
    Character::Draw(pointLightResource, spotLightResource);
    // 装飾は描画姿勢に同期してから、形態と本体の可視性に応じて重ねる。
    UpdateCopyAdornment();
    if (copyAdornment_ && copyAdornment_->GetIsVisible())
        copyAdornment_->Draw(pointLightResource, spotLightResource);
    for (auto& object : copyPreviewObjects_)
        object->Draw(pointLightResource, spotLightResource);
    // 能力の描画はコントローラーがあり本体が可視のときだけ委ねる。
    if (copyAbilityController_ && GetIsVisible())
        copyAbilityController_->Draw(*this, pointLightResource, spotLightResource);
    if (enemyMorphReleaseVisual_ && enemyMorphReleaseVisual_->GetIsVisible()) {
        enemyMorphReleaseVisual_->Draw(pointLightResource, spotLightResource);
    }

    // 感電オーラが有効で可視なら最後に描く。
    if (electricShockAuraEffect_ && electricShockAuraEffect_->GetIsVisible()) {
        electricShockAuraEffect_->Draw(pointLightResource, spotLightResource);
    }

    // フックの到達点は本体の視錐台判定と独立に、GamePlayScene::Draw() が描く。
}


bool Player::OnCollision(engine::Object3D* other) {
    // 死亡・演出中はゲームプレイの接触を止め、運搬中の敵も判定から外します。
    if (IsDead() || isCinematicLocked_) {
        return false;
    }

    // 相手を取得できない場合は形状を比較できない。
    if (!other)
        return false;

    // 自分が保持している相手は、接触の対象として扱わない。
    if (other == carriedEnemy_) {
        return false;
    }

    // 敵型の相手が運搬中なら衝突属性とマスクを止める。
    if (BaseEnemy* enemy = dynamic_cast<BaseEnemy*>(other)) {
        if (enemy->IsCarried()) {
            enemy->SetCollisionAttribute(0);
            enemy->SetCollisionMask(0);
            return false;
        }
    }

    // コライダーがある相手について、接触の分類に使う属性を読み取る。
    uint32_t attribute = 0;

    if (other->GetCollider()) {
        attribute = other->GetCollider()->GetAttribute();
    }

    // 本体同士の接触を調べ、子パーツの判定と比較する初期値にする。
    engine::CollisionInfo info = CheckCollision(other);

    // 存在する子パーツも比較し、最も深くめり込んだ接触を押し出しの基準にする。
    for (engine::Object3D* child : GetChildren()) {
        if (!child)
            continue;
        engine::CollisionInfo childInfo = child->CheckCollision(other);

        if (childInfo.isColliding) {
            // 本体側が未接触か、このパーツのめり込みがより深い場合に法線と量を差し替える。
            if (!info.isColliding || childInfo.penetration > info.penetration) {
                info = childInfo;
            }
        }
    }

    // 本体もパーツも当たっていなければ終了
    if (!info.isColliding) {
        return false;
    }

    // 体当たりで割れるブロックやガラスは、押し戻しより前に破壊条件を判定します。
    // 先に物理解決すると、破壊フレームだけ壁へ大きく押し戻されるためです。
    // 破壊可能なブロックでPlayerの衝撃条件が成立した場合は、通常の押し戻し・通知へ進まない。
    if (auto* breakableBlock = dynamic_cast<GimmickBreakableBlock*>(other)) {
        if (breakableBlock->TryBreakByPlayerImpact(this)) {
            return true;
        }
    }

    // 床や壁は無敵中も物理解決する。トランポリンなら先に反発を確認する。
    if (auto* trampoline = dynamic_cast<GimmickTrampoline*>(other)) {
        // 通常の床解決より先に着地を反発へ変える。呼び出し順に依存しないようにする。
        trampoline->TryBounce(this, info.normal);
    }

    // 地面属性を持つ相手にだけ、採用した接触法線とめり込み量で物理解決する。
    if (attribute & engine::collisionCategories::Get("ground")) {
        // 重なりによる解決でも、破壊可否を確定してから固体の壁だけで加速を解除する。
        if (mover_ && !(attribute & engine::collisionCategories::Get("trigger")) && mover_->IsDashPanelWall(info.normal))
            mover_->StopDashPanelAtWall();
        ApplyPhysicsCollision(info, attribute);
    }

    // 無敵などで被弾を見送っても、下のギミックへの接触通知は続ける。
    ApplyContactDamage(*other, attribute, info.normal);

    // ワープやスイッチなど、ダメージ以外の判定のために通知します。
    PlayerHitEvent hitEvent;
    hitEvent.me = this;
    hitEvent.hitObject = other;
    hitEvent.normal = info.normal;
    EventManager::GetInstance()->Dispatch(hitEvent);

    return true;
}

// 敵本体または敵攻撃に触れたとき、被弾の可否を確認して通知と次の状態を選ぶ。
void Player::ApplyContactDamage(engine::Object3D& other, std::uint32_t attribute, const engine::Vector3& normal) {
    const auto* bodyEnemy = (attribute & engine::collisionCategories::Get("enemy"))
                                ? dynamic_cast<const BaseEnemy*>(&other)
                                : nullptr;
    const float bodyAttackDamage = bodyEnemy ? bodyEnemy->GetActiveBodyAttackDamage() : 0.0f;

    // 敵本体との接触は、体当たりの威力が有効な間だけ攻撃として扱う。
    if (!(attribute & engine::collisionCategories::Get("enemy_attack")) && !(bodyAttackDamage > 0.0f)) {
        return;
    }

    // 被弾直後や無敵中はダメージを重ねない。床との物理解決は呼び出し元で済んでいる。
    if (!(damageCooldownTimer_ <= 0.0f) || IsInvincible()) {
        return;
    }

    const bool guarded = CanGuardAttackFrom(other.GetWorldPosition());

    // ガード中に同じ攻撃と重なり続けても、連続接触の間隔内は再通知しない。
    if (guarded && !(guardContactCooldownTimer_ <= 0.0f)) {
        return;
    }

    const auto* attackBullet = dynamic_cast<const Bullet*>(&other);
    const bool isElectricHit = other.GetEnemyType() == "ThunderSlime" ||
                               (attackBullet && attackBullet->GetDamageType() == DamageType::kElectric);

    DamageEvent damageEvent;
    damageEvent.target = this;
    damageEvent.attacker = &other;
    // 弾は弾自身のダメージ・属性・状態異常を渡す。接触攻撃は威力設定がなければ1にする。
    if (attackBullet) {
        damageEvent.damageAmount = attackBullet->GetDamage();
        damageEvent.damageType = attackBullet->GetDamageType();
        damageEvent.statusEffect = attackBullet->GetStatusEffect();
    } else {
        damageEvent.damageAmount = bodyAttackDamage > 0.0f ? bodyAttackDamage : 1.0f;
    }

    EventManager::GetInstance()->Dispatch(damageEvent);

    // ガード成立時は連続接触の間隔だけを設定し、吹き飛ばしに移らない。
    if (guarded) {
        constexpr float kGuardContactHitInterval = 0.22f;
        guardContactCooldownTimer_ = kGuardContactHitInterval;
        return;
    }

    // 感電は専用の拘束処理を使うため、接触の間隔だけを確保する。
    if (isElectricHit) {
        damageCooldownTimer_ = 0.8f;
        return;
    }

    // 通常被弾は1秒の間隔と無敵を設定し、接触面の法線方向へ吹き飛ばす。
    damageCooldownTimer_ = 1.0f;
    SetDamageInvincible(true);
    ChangeState(std::make_unique<PlayerStateDamage>(normal));
}

// 移動方式とギミックから受けた速度をPlayerMoverへ渡す入口。

void Player::SetMoveStrategy(std::unique_ptr<IMoveStrategy> strategy) {
    // 移動方式の差し替えは Mover に集約し、Player の状態を作り直さない。
    if (mover_) {
        mover_->SetStrategy(std::move(strategy));
    }
}

void Player::ApplyDashPanelBoost(float speedMultiplier, float turnMultiplier) {
    // パネルの加速量と旋回補正は通常移動と同じ速度管理へ渡す。
    if (mover_) {
        mover_->ApplyDashPanelBoost(speedMultiplier, turnMultiplier);
    }
}

// 生存中でMoverがあり、加速パネルによる速度補正が残っている場合にtrue。
bool Player::IsDashPanelBoostActive() const {
    return !IsDead() && mover_ && mover_->IsDashPanelBoostActive();
}

// Moverが初期化済みの場合に、現在の加速パネル補正を終了する。
void Player::CancelDashPanelBoost() {
    if (mover_)
        mover_->CancelDashPanelBoost();
}

void Player::SetPlatformMotion(const engine::Vector3& velocity) {
    // 床から受けた速度を次の移動更新で一度だけ合成する。
    if (mover_)
        mover_->SetPlatformMotion(velocity);
}

void Player::SetAbilityAirImpulse(const engine::Vector3& velocity) {
    // 能力の押し出しを通常の移動入力と同じ更新へ渡し、次フレームでの上書きを防ぐ。
    if (mover_)
        mover_->SetAbilityAirImpulse(velocity);
}

// Playerの状態遷移と、描画に使うSlimeAnimatorへの操作。

void Player::ChangeState(std::unique_ptr<IAnimationState> newState) {
    // 入力以外の経路でも、保持中の敵を離してフックへ切り替えない。
    // 現在状態のExitより前に拒否し、運搬側の解除処理も発生させない。
    if (carriedEnemy_ && (dynamic_cast<PlayerStateWhip*>(newState.get()) ||
                          dynamic_cast<PlayerStatePullEnemy*>(newState.get()) ||
                          dynamic_cast<PlayerStatePullObject*>(newState.get()))) {
        return;
    }

    // 通常移動への遷移は余韻を維持し、次の技・被弾などは即座に解除します。
    if (!dynamic_cast<PlayerStateIdle*>(newState.get()) && !dynamic_cast<PlayerStateRun*>(newState.get()) &&
        !dynamic_cast<PlayerStateJump*>(newState.get())) CancelWhipReturn();

    // 現在の状態があれば、切替前に終了処理で入力や演出を片付ける。
    if (state_) {
        state_->Exit(this);
    }

    // 新しい状態の所有権を取り、状態が存在すれば開始する。空の状態が渡された場合はログに記録する。
    state_ = std::move(newState);

    if (state_) {
        state_->Enter(this);
        UpdateWhipReturn(0.0f);
    } else {
        DebugConsole::GetInstance()->AddLog("[ERROR] Failed to set new state.");
    }
}

void Player::PlayAnimation(const std::string& animName, bool loop) {
    // 既に同じアニメーションが再生中ならリセットしない (滑らかな遷移のため)
    if (GetAnimationName() != animName) {
        SetAnimationName(animName);
        ResetAnimationTime();
    }

    SetAnimationLoop(loop);
}

void Player::SetSlimeAnimationMode(PlayerSlimeAnimator::Mode mode) {
    // モーション要求の入口を統一し、Animator の内部状態を外へ公開しない。
    slimeAnimator_.SetMode(mode);
}

void Player::SetSlimeAnimationDirection(const engine::Vector3& direction) {
    // 移動方向は変形アニメーションの傾きにも使う。
    slimeAnimator_.SetMotionDirection(direction);
}

void Player::SetSlimePullDirection(const engine::Vector3& direction) {
    // 引き寄せ演出の向きだけを Animator へ渡す。
    slimeAnimator_.SetPullDirection(direction);
}

void Player::SetSlimePullProgress(float progress) {
    // 演出の進行率と身体の伸びを同期する。
    slimeAnimator_.SetPullProgress(progress);
}

void Player::SetSlimeJumpCharge(float chargeRate) {
    // 溜め量に応じた見た目は Animator に任せ、物理ジャンプとは分離する。
    slimeAnimator_.SetJumpCharge(chargeRate);
}

// 離陸の伸びをAnimatorへ通知する。ジャンプ速度は移動側が設定する。
void Player::TriggerSlimeJumpTakeoff() {
    slimeAnimator_.TriggerJumpTakeoff();
}

void Player::SetSlimeAbilityPose(PlayerSlimeAnimator::AbilityPose pose, float strength) {
    // コピー能力のポーズを通常移動アニメーションへ重ねる。
    slimeAnimator_.SetAbilityPose(pose, strength);
}

void Player::TriggerSlimeImpulse(const engine::Vector3& scale, float duration) {
    // 命中時などの一時的な伸縮は基準スケールへ書き込まない。
    slimeAnimator_.TriggerImpulse(scale, duration);
}

bool Player::PlaySlimeAbilityMotion(const std::string& clipName, bool loop, float playbackSpeed, float blendInDuration,
                                    float blendOutDuration) {
    // コピー能力のクリップ再生と補間を Animator に集約する。
    return slimeAnimator_.PlayAbilityMotion(clipName, loop, playbackSpeed, blendInDuration, blendOutDuration);
}

void Player::StopSlimeAbilityMotion(float blendOutDuration) {
    // 能力終了時は指定時間で通常の身体アニメーションへ戻す。
    slimeAnimator_.StopAbilityMotion(blendOutDuration);
}

void Player::ClearSlimeAbilityMotion() {
    // コピー解除では補間待ちを残さず能力クリップを破棄する。
    slimeAnimator_.ClearAbilityMotion(this);
}

void Player::ForceSlimeAnimationModeForNextUpdate(PlayerSlimeAnimator::Mode mode, const engine::Vector3& direction) {
    // 能力側の姿勢指定を、次回の通常 Animator 更新に限って優先する。
    forcedSlimeAnimationModeActive_ = true;
    forcedSlimeAnimationMode_ = mode;
    forcedSlimeAnimationDirection_ = direction;
}

void Player::StartGateReturnAnimation(const PlayerGateReturnAnimation::Route& route) {
    // ゲート帰還の経路と操作権は専用の演出オブジェクトに任せる。
    gateReturnAnimation_.Start(this, route);
}

bool Player::IsGateReturnAnimationActive() const {
    // 更新側が帰還演出中の通常入力を止めるための状態を返す。
    return gateReturnAnimation_.IsActive();
}

bool Player::IsGateReturnAnimationFinished() const {
    // シーン遷移は帰還演出の完了を待ってから開始する。
    return gateReturnAnimation_.IsFinished();
}

void Player::StopGateReturnAnimation(bool restoreControl) {
    // 中断理由に応じて操作権を戻すかを呼び出し側に選ばせる。
    gateReturnAnimation_.Stop(this, restoreControl);
}

void Player::StepCinematicPullVisual(float deltaTime, const engine::Vector3& direction, float progress) {
    // 操作ロック中だけ、引き寄せの見た目を演出タイムラインから進める。
    if (!isCinematicLocked_)
        return;
    slimeAnimator_.SetMode(PlayerSlimeAnimator::Mode::kPullEnemy);
    slimeAnimator_.SetPullDirection(direction);
    slimeAnimator_.SetPullProgress(progress);
    slimeAnimator_.Update(this, deltaTime);
}

void Player::EndCinematicPullVisual() {
    // 演出で使った伸縮を残さず、通常姿勢へ復帰する。
    slimeAnimator_.Reset(managedBaseScale_, this);
    slimeAnimator_.SetMode(PlayerSlimeAnimator::Mode::kDisabled);
    // フックの到達点マーカーが存在する場合は、演出終了後に残らないよう隠す。
    if (hookMarker_)
        hookMarker_->SetIsVisible(false);
}

// 通常の移動・衝突と状態更新を停止し、演出終了時に必要な設定を控える。
void Player::BeginCinematicLock(bool preserveCopyAbility) {
    // すでに演出で操作を止めている場合や死亡中は、保存済みの状態を取り直さない。
    if (isCinematicLocked_ || IsDead()) {
        return;
    }

    // ゲート回避・突進で一時停止した操作を先に返し、演出終了後の復元値を確定します。
    if (enemyMorphType_ == EnemyMorphType::kCrown && copyAbilityController_)
        copyAbilityController_->Interrupt(*this);

    // 同じ衝突フレームで後続の被弾イベントが来ても、先に演出専用状態へ切り替えます。
    isCinematicLocked_ = true;
    ClearHurtFace();
    cinematicSavedControlActive_ = isControlActive_;
    cinematicSavedCollisionAttribute_ = GetCollisionAttribute();
    cinematicSavedCollisionMask_ = GetCollisionMask();

    // 通常攻撃を管理している場合は、演出に入る前に実行中の攻撃をやめる。
    if (baseCombatController_) {
        baseCombatController_->Cancel(*this, false);
    }

    // ボス導入ではコピー能力を維持し、入力と戦闘状態だけを止めます。
    if (!preserveCopyAbility) {
        CancelEnemyMorph();
    }

    ReleaseCarriedEnemy(true);

    // 感電位置への固定・点滅・無敵を解除してから、演出側が扱う身体へ戻します。
    if (electricShockFeedbackTimer_ > 0.0f || electricShockControlLocked_) {
        electricShockPendingInvincibleDuration_ = 0.0f;
        EndElectricShockFeedback();
    }

    RestoreDamageBlinkVisibility();
    damageCooldownTimer_ = 0.0f;
    evasionInvincibleTimer_ = 0.0f;
    isEvasionInvincible_ = false;
    invincibleBlinkTimer_ = 0.0f;
    SetDamageInvincible(false);
    SetDashInvincible(false);
    pendingAttack2_ = false;
    attackInputBuffer_.ClearPendingInput();

    // Damage Stateなどの終了処理で基準姿勢へ戻してから、演出側が姿勢を取得します。
    ChangeState(std::make_unique<PlayerStateIdle>());
    slimeAnimator_.Reset(managedBaseScale_, this);
    slimeAnimator_.SetMode(PlayerSlimeAnimator::Mode::kDisabled);

    engine::Vector3 stableRotation = GetRotation();
    stableRotation.x = baseRotation_.x;
    stableRotation.z = baseRotation_.z;

    // コピーを保持しないか現在コピー中でなければ、通常の基準倍率へ戻す。
    if (!preserveCopyAbility || !isEnemyMorphed_) {
        SetScale(managedBaseScale_);
    }

    // 最後に速度と衝突を無効にし、演出の開始位置と姿勢を読み取れるよう行列を確定します。
    SetRotation(stableRotation);
    SetVelocity({0.0f, 0.0f, 0.0f});
    SetIsControlActive(false);
    SetCollisionAttribute(0);
    SetCollisionMask(0);
    SetIsVisible(true);
    UpdateLocalMatrix();
    UpdateWorldMatrix();
}

void Player::EndCinematicLock(bool restoreControl) {
    // 演出開始前の衝突設定を復元し、死亡中だけは操作権を戻さない。
    if (!isCinematicLocked_) {
        return;
    }

    isCinematicLocked_ = false;
    SetCollisionAttribute(cinematicSavedCollisionAttribute_);
    SetCollisionMask(cinematicSavedCollisionMask_);
    SetVelocity({0.0f, 0.0f, 0.0f});
    slimeAnimator_.Reset(managedBaseScale_, this);
    ChangeState(std::make_unique<PlayerStateIdle>());
    // Idleの開始処理を通してから、呼び出し側の指定と演出前の許可を使って操作可否を最後に確定します。
    SetIsControlActive(restoreControl && cinematicSavedControlActive_ && !IsDead());
}

// 無敵とガードの状態を切り替え、各状態の色を反映する。
void Player::SetDamageInvincible(bool inv) {
    isDamageInvincible_ = inv;
    UpdateColor(); // 状態が変わったら色を更新
}

void Player::SetDashInvincible(bool inv) {
    isDashInvincible_ = inv;
    UpdateColor(); // 状態が変わったら色を更新
}

void Player::SetGuarding(bool guarding) {
    isGuarding_ = guarding;
    UpdateColor();
}

bool Player::CanGuardAttackFrom(const engine::Vector3& sourcePosition) const {
    // ガード姿勢でなければ、攻撃元の角度を調べず通常の被弾として扱う。
    if (!isGuarding_) {
        return false;
    }

    engine::Vector3 toSource = sourcePosition - GetWorldPosition();
    toSource.y = 0.0f;
    const float distance = engine::Math::Length(toSource);
    // 攻撃元の水平位置が自分とほぼ同じなら正面判定ができないため、ガードを成立させない。
    if (distance <= 0.001f) {
        return false;
    }

    // 正面を中心とした約 120 度のみ軽減し、背面・側面と攻撃元不明の判定は通常被弾にする。
    const engine::Vector3 forward = GetForwardDirection();
    constexpr float kGuardFrontConeDot = 0.5f;
    return (forward.x * toSource.x + forward.z * toSource.z) / distance >= kGuardFrontConeDot;
}

// コピー能力の管理クラスがある場合に、成立したガードの攻撃元ワールド位置を通知する。
void Player::NotifyGuardedHit(const engine::Vector3& sourcePosition) {
    if (copyAbilityController_) {
        copyAbilityController_->NotifyGuardedHit(*this, sourcePosition);
    }
}

void Player::SetTutorialSafetyEnabled(bool enabled) {
    // チュートリアル開始時に残り HP と死亡状態を安全な初期値へ戻す。
    tutorialSafetyEnabled_ = enabled;

    // 保護を有効にしてHPが利用できるときは、最大HP・死亡状態・被弾無敵を初期化する。
    if (enabled && GetParameters().has_value()) {
        SetParameter(&engine::EntityParameter::maxHp, (std::max)(GetParameters()->maxHp, 1.0f));
        SetParameter(&engine::EntityParameter::hp, GetParameters()->maxHp);
        SetDead(false);
        deathTimer_ = 0.0f;
        SetDamageInvincible(false);
    }
}

void Player::RestoreHpToFull() {
    // 最大HPは現在のプレイヤー設定を使い、取得時に即座にHUDへ反映します。
    if (!GetParameters().has_value() || IsDead()) {
        return;
    }

    SetParameter(&engine::EntityParameter::maxHp, (std::max)(GetParameters()->maxHp, 1.0f));
    SetParameter(&engine::EntityParameter::hp, GetParameters()->maxHp);
}

void Player::SetTutorialCarryActionPermissions(bool allowThrow, bool allowAbsorb) {
    tutorialCarryThrowEnabled_ = allowThrow;
    tutorialCarryAbsorbEnabled_ = allowAbsorb;

    // 吸収演出の途中で投擲専用工程へ切り替わった場合は、拘束状態へ戻します。
    if (!allowAbsorb && absorbEffectActive_) {
        CancelAbsorbEffect(true);
    }
}

void Player::ResetTutorialCarryActionState() {
    // 誤操作済みの編集再開や工程ジャンプでも再試行できる状態へ戻します。
    CancelEnemyMorph();
    ReleaseCarriedEnemy(true);
}

void Player::StartEvasionInvincibility(float duration, EvasionFeedback feedback) {
    // 重なる回避受付で既存の無敵時間を短縮しない。
    evasionInvincibleTimer_ = (std::max)(evasionInvincibleTimer_, (std::max)(0.0f, duration));
    isEvasionInvincible_ = evasionInvincibleTimer_ > 0.0f;
    evasionFeedback_ = feedback;
    // ゲート回避は断面の白金光で無敵を示し、モデル本来の材質色を使います。
    if (evasionFeedback_ == EvasionFeedback::kBodyFlash)
        UpdateColor();
}

void Player::RestoreDamageBlinkVisibility() {
    // 点滅開始前の本体・子の可視状態に戻し、元から非表示の物を誤表示しない。
    if (!damageBlinkVisibilityApplied_) {
        return;
    }

    SetIsVisible(damageBlinkBodyVisible_);

    // 通常形態だけ子の表示も復元し、点滅前の記録がない子の表示は保持する。
    if (!isEnemyMorphed_) {
        const auto& children = GetChildren();

        for (size_t i = 0; i < children.size(); ++i) {
            engine::Object3D* child = children[i];

            if (!child) {
                continue;
            }

            if (i < damageBlinkChildVisible_.size()) {
                child->SetIsVisible(damageBlinkChildVisible_[i]);
            }
        }
    }

    damageBlinkVisibilityApplied_ = false;
    damageBlinkBodyVisible_ = true;
    damageBlinkChildVisible_.clear();
}

void Player::UpdateDamageInvincibleBlinkVisibility() {
    // 感電演出を優先し、通常の被弾点滅が表示状態を上書きしない。
    if (electricShockFeedbackTimer_ > 0.0f) {
        RestoreDamageBlinkVisibility();
        return;
    }

    // 被弾無敵の間だけ、本体の可視性を周期的に切り替える。
    if (isDamageInvincible_) {
        // 点滅直前の可視性を一度だけ控え、後続フレームで非表示側の値を記録し直さないようにします。
        if (!damageBlinkVisibilityApplied_) {
            damageBlinkVisibilityApplied_ = true;
            damageBlinkBodyVisible_ = GetIsVisible();
            damageBlinkChildVisible_.clear();

            // 通常形態で開始した場合は、子ごとの可視性も同じ添字順で控える。
            if (!isEnemyMorphed_) {
                damageBlinkChildVisible_.reserve(GetChildren().size());

                for (engine::Object3D* child : GetChildren()) {
                    damageBlinkChildVisible_.push_back(child ? child->GetIsVisible() : false);
                }
            }
        }

        // 元から表示している部分だけを点滅周期に乗せ、コピー中の子の可視性は変更しません。
        const float blinkPhase = std::fmod(invincibleBlinkTimer_, 0.16f);
        const bool blinkVisible = blinkPhase < 0.095f;
        SetIsVisible(damageBlinkBodyVisible_ && blinkVisible);

        if (!isEnemyMorphed_) {
            const auto& children = GetChildren();

            for (size_t i = 0; i < children.size(); ++i) {
                engine::Object3D* child = children[i];

                // 対応する実体がなくなった子の表示は更新しない。
                if (!child) {
                    continue;
                }

                const bool originalVisible =
                    i < damageBlinkChildVisible_.size() ? damageBlinkChildVisible_[i] : child->GetIsVisible();
                child->SetIsVisible(originalVisible && blinkVisible);
            }
        }

        return;
    }

    // 点滅を適用した記録がなければ、復元する表示状態はない。
    if (!damageBlinkVisibilityApplied_) {
        return;
    }

    // 被弾無敵が終了したら、点滅のために変更した表示だけを復元します。
    RestoreDamageBlinkVisibility();
}

// ガード色を優先し、ガード外でダッシュか本体点滅の回避が有効なら回避色を使う。
void Player::UpdateColor() {
    engine::Vector4 targetColor =
        isEnemyMorphed_ ? GetEnemyMorphTint(enemyMorphType_) : engine::Vector4{1.0f, 1.0f, 1.0f, 1.0f}; // 基本は白(通常色)

    if (isGuarding_) {
        // 通常のピンクと区別できる、身を固めた濃いローズ色。
        targetColor = {0.78f, 0.12f, 0.34f, 1.0f};
    } else if (isDashInvincible_ || (isEvasionInvincible_ && evasionFeedback_ == EvasionFeedback::kBodyFlash)) {
        targetColor = {0.18f, 0.82f, 1.0f, 1.0f}; // 回避中は雷を思わせる水色を設定
    }

    // 本体を更新し、通常形態の有効な子パーツにも同じ色を渡す。
    SetColor(targetColor);

    if (!isEnemyMorphed_) {
        for (engine::Object3D* child : GetChildren()) {
            if (child)
                child->SetColor(targetColor);
        }
    }
}

// コンボの追加入力を受ける時間窓。
void Player::StartComboWindow(float duration) {
    // 次段へ進める時間窓は攻撃入力バッファに管理させる。
    attackInputBuffer_.StartComboWindow(duration);
}

bool Player::IsComboWindowActive() const {
    // 現在の技が追加入力を受け取れるかを状態側へ返す。
    return attackInputBuffer_.IsComboWindowActive();
}

// 次の攻撃に使う押下入力の記録と消費。
void Player::RecordAttackInput(float duration) {
    // 次の状態開始までに押された攻撃を短時間だけ予約する。
    attackInputBuffer_.Record(duration);
}

void Player::MarkAttackBufferUsedForStateStart() {
    // 状態開始に使った入力を次段の追加入力として二重に数えない。
    attackInputBuffer_.MarkUsedForStateStart();
}

bool Player::ConsumeBufferedAttackInput() {
    // 予約は一度だけ消費し、長押しで連続段が進まないようにする。
    return attackInputBuffer_.Consume();
}
