#include <cstdint>
#include "engine/graphics/effect/VfxAssetPath.h"
#include "BossIntroSettings.h"
#include "game/actor/enemy/boss/false_king/EnemyFalseKingSlime.Internal.h"
#include <cmath>
#include <numbers>

using namespace enemyFalseKingSlimeImplementation;

// Editorでは配置値を復元し、Play中の待機・登場だけ通常AIより先に処理します。
bool EnemyFalseKingSlime::UpdateEncounterState(float deltaTime) {
    // この関数が待機・登場を処理した場合は通常 AI の更新を抑止します。
    // 入口イベント管理の指定がない個体は、待機や登場を挟まず通常AIに渡します。
    if (!IsEncounterControlled()) {
        encounterState_ = EncounterState::kNormal;
        initializedForPlay_ = false;
        return false;
    }

    SceneManager* sceneManager = SceneManager::GetInstance();

    // 編集時は毎回配置姿勢へ戻し、プレイ中の状態を編集画面に持ち込みません。
    if (!sceneManager || !sceneManager->IsPlaying()) {
        ResetForEditor();
        return true;
    }

    // プレイ開始時だけ配置値を退避して、待機か登場かを選びます。
    if (!initializedForPlay_) {
        InitializeEncounterState();
    }

    // 入口イベントを待つ間は移動を止め、攻撃の表示を残しません。
    if (encounterState_ == EncounterState::kDormant) {
        SetVelocity({0.0f, 0.0f, 0.0f});
        ClearTransientVisuals();
        HideAttackTelegraph();
        return true;
    }

    // 登場を済ませた個体の毎フレーム処理は通常AIに任せます。
    if (encounterState_ != EncounterState::kAppearing) {
        return false;
    }

    // 登場中の姿勢はTimelineが担当し、ボス側は判定と戦闘状態を管理する。
    encounterTimer_ += deltaTime;
    idleTimer_ += deltaTime;
    const float progress = std::clamp(encounterTimer_ / GetAppearanceDuration(), 0.0f, 1.0f);
    SetIsVisible(true);
    SetCollisionAttribute(0);
    SetCollisionMask(0);
    SetVelocity({0.0f, 0.0f, 0.0f});
    // 透明度と発光は描画材質の状態なので、TimelineのTransformと競合しない。
    SetColor({
        authoredColor_.x,
        authoredColor_.y,
        authoredColor_.z,
        std::max(0.01f, SmoothStep01(std::min(progress / 0.30f, 1.0f))),
    });
    SetEmissive(1.0f + std::sin(std::min(progress / 0.72f, 1.0f) * kPi) * 2.2f);
    SyncCollisionBounds();

    // 専用ポータルの登場では配置時の材質を使い、Timeline再生中は戦闘開始を待ちます。
    if (introPortalEnabled_) { SetColor(authoredColor_); SetEmissive(authoredEmissive_); }
    // 登場尺を消化し、シーンのTimelineも終了した時点でFinishAppearance()へ渡す。
    if (progress >= 1.0f && !introPresentationActive_) {
        FinishAppearance();
    }

    return true;
}

// 配置時の姿勢と衝突設定を退避します。force はEditorへ戻る時の再取得用です。
void EnemyFalseKingSlime::CaptureAuthoredState(bool force) {
    // 実行中に変形した姿勢を保存値へ混ぜず、必要な時だけ再取得します。
    if (authoredStateCaptured_ && !force) {
        return;
    }

    authoredPosition_ = GetTranslate();
    authoredScale_ = GetScale();
    authoredRotation_ = GetRotation();
    authoredColor_ = GetColor();
    authoredCollisionAttribute_ = GetCollisionAttribute();
    authoredCollisionMask_ = GetCollisionMask();
    authoredMaterialType_ = GetMaterialType();
    authoredEmissive_ = GetEmissive();
    // 退避した衝突設定がゼロの場合は、戦闘開始後に使う敵属性と判定対象を補います。
    // 配置で敵属性が未指定なら、戦闘開始時に使う敵属性を補う。
    if (authoredCollisionAttribute_ == 0)
        authoredCollisionAttribute_ = engine::collisionCategories::Get("enemy");
    // 判定対象の指定が空なら、プレイヤー・地形・プレイヤーの攻撃と弾を対象にする。
    if (authoredCollisionMask_ == 0)
        authoredCollisionMask_ = engine::collisionCategories::Get("player") | engine::collisionCategories::Get("ground") | engine::collisionCategories::Get("player_attack") | engine::collisionCategories::Get("player_bullet");
    // 描画の伸縮とは別に、当たり判定用の基準スケールを記録します。
    baseScale_ = authoredScale_;
    hasBaseScale_ = true;
    authoredStateCaptured_ = true;
}

// Play開始時に攻撃履歴と形態を初期化し、配置設定に応じて待機または登場へ進めます。
void EnemyFalseKingSlime::InitializeEncounterState() {
    // 前回の王冠決戦と攻撃履歴を消し、配置設定から初期状態を決めます。
    ResetLocomotion();
    locomotionStepSerial_ = 0;
    ResetCrownFinale();
    initializedForPlay_ = true;
    CaptureAuthoredState(true);
    // 形態・技の巡回順・クールダウンは第一形態の初期状態から始めます。
    battlePhase_ = 1;
    pendingBattlePhase_ = 1;
    attackCyclePhase_ = 0;
    attackCycleCount_ = 0;
    attackCycleIndex_ = 0;
    lastAutomaticAttack_ = AttackKind::kNone;
    automaticAttackSerial_ = 0;
    attackState_ = AttackState::kIdle;
    currentAttack_ = AttackKind::kNone;
    attackCooldown_ = 1.05f;
    const bool startActive = GetParameters().has_value() && GetParameters()->startActive;

    // 配置で開始済みか、起動要求を受けていれば登場に進み、それ以外は入口を待ちます。
    if (startActive || activationRequested_) {
        BeginAppearance();
    // 配置も起動要求も開始を指定していなければ、入口イベントを待つ状態にする。
    } else {
        ApplyDormantState();
    }
}

// Play中の一時状態を破棄して、Editor上の配置姿勢へ戻します。
void EnemyFalseKingSlime::ResetForEditor() {
    // プレイ中の姿勢・材質・衝突を破棄して編集時の設定へ戻します。
    ResetLocomotion();
    locomotionStepSerial_ = 0;
    introPresentationActive_ = false;
    EnableIntroPortal(false);
    ClearBossAura();
    ResetCrownFinale();

    // 実際にプレイを開始していた場合だけ、プレイ前に退避した姿勢と材質を戻します。
    if (initializedForPlay_) {
        SetTranslate(authoredPosition_);
        SetScale(authoredScale_);
        SetRotation(authoredRotation_);
        SetColor(authoredColor_);
        SetMaterialType(authoredMaterialType_);
        SetEmissive(authoredEmissive_);
    }

    // 復元後の値を配置基準として取得し直し、次回のプレイで入口から始められるようにします。
    CaptureAuthoredState(true);
    initializedForPlay_ = false;
    activationRequested_ = false;
    encounterState_ = EncounterState::kDormant;
    encounterTimer_ = 0.0f;
    battlePhase_ = 1;
    pendingBattlePhase_ = 1;
    attackCyclePhase_ = 0;
    attackCycleCount_ = 0;
    attackCycleIndex_ = 0;
    lastAutomaticAttack_ = AttackKind::kNone;
    automaticAttackSerial_ = 0;
    attackState_ = AttackState::kIdle;
    currentAttack_ = AttackKind::kNone;
    SetVelocity({0.0f, 0.0f, 0.0f});
    SetCollisionAttribute(authoredCollisionAttribute_);
    SetCollisionMask(authoredCollisionMask_);
    // 編集画面で選択できるよう表示を戻し、残っていた予兆を消して当たり判定を同期します。
    SetIsVisible(true);
    ClearTransientVisuals();
    HideAttackTelegraph();
    SyncCollisionBounds();
}

// モデルを表示して登場状態に移す。判定は外したまま、専用ポータルを使わない場合のVFXを起動する。
void EnemyFalseKingSlime::BeginAppearance() {
    encounterState_ = EncounterState::kAppearing;
    encounterTimer_ = 0.0f;
    SetIsVisible(true);
    SetCollisionAttribute(0);
    SetCollisionMask(0);
    SetVelocity({0.0f, 0.0f, 0.0f});
    // 配置位置の少し上を、通常登場VFXの発生点にする。
    engine::Vector3 effectPosition = authoredPosition_;
    effectPosition.y += 0.2f;
    // 専用ポータルの登場ではTimeline側の粒子を使うため、通常の一回再生を省く。
    if (!introPortalEnabled_)
        VfxSequencer::PlayOneShot(kAppearanceSequence, effectPosition, {2.4f, 2.4f, 2.4f});
}

void EnemyFalseKingSlime::SetIntroPresentationActive(bool active, float duration) {
    // シーンから登場Timelineの再生状態と専用の尺を受け取る。durationは秒。
    introPresentationActive_ = active;
    // Timelineが終わるか中止されたら、同時に登場ポータルの描画を終了する。
    if (!active) EnableIntroPortal(false);
    // 再生を開始した場合だけ時間を保存し、進捗計算に使える正の値へ補正する。
    if (active) {
        introPresentationDuration_ = (std::max)(0.05f, duration);
    }
}

void EnemyFalseKingSlime::ApplyDormantState() {
    EnableIntroPortal(false);
    ClearBossAura();
    // 入口イベント前はモデルも衝突も隠し、待機中の攻撃演出を消します。
    ResetLocomotion();
    encounterState_ = EncounterState::kDormant;
    encounterTimer_ = 0.0f;
    // 再登場の基準がずれないよう、配置位置・形・材質へ戻します。
    SetTranslate(authoredPosition_);
    SetScale(authoredScale_);
    SetRotation(authoredRotation_);
    SetColor(authoredColor_);
    SetMaterialType(authoredMaterialType_);
    SetEmissive(authoredEmissive_);
    SetVelocity({0.0f, 0.0f, 0.0f});
    // 開始前はボスに触れないよう衝突を無効にし、見た目と攻撃予兆も隠します。
    SetCollisionAttribute(0);
    SetCollisionMask(0);
    SetIsVisible(false);
    ClearTransientVisuals();
    HideAttackTelegraph();
}

// 登場終了後に配置時の衝突設定を戻して戦闘AIを再開します。
void EnemyFalseKingSlime::FinishAppearance() {
    ResetLocomotion();
    encounterState_ = EncounterState::kActive;
    encounterTimer_ = 0.0f;
    // Timelineで変わった姿勢を戦闘開始位置の設定に戻し、退避した衝突属性を復元します。
    SetTranslate(authoredPosition_);
    SetScale(authoredScale_);
    SetRotation(authoredRotation_);
    SetColor(authoredColor_);
    SetMaterialType(authoredMaterialType_);
    SetEmissive(authoredEmissive_);
    SetCollisionAttribute(authoredCollisionAttribute_);
    SetCollisionMask(authoredCollisionMask_);
    SetVelocity({0.0f, 0.0f, 0.0f});
    SetIsVisible(true);
    // 登場の直後に技が起きないよう待ち時間を置き、復元したスケールで判定を確定します。
    attackCooldown_ = 0.82f;
    SyncCollisionBounds();
}

void EnemyFalseKingSlime::SyncCollisionBounds() {
    // 描画用の伸縮で接地判定が上下しないよう、OBBは配置時のスケールへ固定します。
    if (engine::Collider* collider = GetCollider()) {
        collider->SetScaleOverride(hasBaseScale_ ? baseScale_ : GetScale());
    }

    // 登場時の伸縮中も本体用 Obb の基準寸法を設定します。
    engine::Object3D::ColliderConfig collider = GetColliderConfig();
    collider.type = engine::ColliderType::kObb;
    collider.center = {0.0f, 1.46f, 0.0f};
    collider.size = {2.82f, 1.46f, 2.28f};
    SetColliderConfig(collider);
}

bool EnemyFalseKingSlime::IsEncounterControlled() const {
    // 配置データのモードが入口イベント管理を指定しているか調べます。
    return GetParameters().has_value() && GetParameters()->actionMode == kEncounterControlledActionMode;
}

float EnemyFalseKingSlime::GetAppearanceDuration() const {
    // 専用Timelineの再生中はその尺を優先する。それ以外は配置設定を制限した秒数か既定値を使う。
    if (introPresentationActive_)
        return introPresentationDuration_;
    return GetParameters().has_value() ? std::clamp(GetParameters()->shakeDuration, 0.55f, 3.5f) : kDefaultAppearanceDuration;
}

bool EnemyFalseKingSlime::IsEncounterHudActive() const {
    // 待機中と撃破後は HP 表示を出しません。
    if (!IsEncounterControlled() || IsDead()) {
        return false;
    }

    return encounterState_ == EncounterState::kAppearing || encounterState_ == EncounterState::kActive;
}

float EnemyFalseKingSlime::GetEncounterCurrentHp() const {
    // UI へ渡す HP は負数にならないようにします。
    return GetParameters().has_value() ? std::max(0.0f, GetParameters()->hp) : 0.0f;
}

float EnemyFalseKingSlime::GetEncounterMaximumHp() const {
    // HP バーの除算でゼロを使わないようにします。
    return GetParameters().has_value() ? std::max(1.0f, GetParameters()->maxHp) : 1.0f;
}

float EnemyFalseKingSlime::GetEncounterAppearanceProgress() const {
    // 登場中だけ実時間の進捗を返し、戦闘開始後は完了扱いにします。
    if (encounterState_ == EncounterState::kActive) {
        return 1.0f;
    }

    // 戦闘開始前でも、待機など登場中でない状態なら進捗を0として返す。
    if (encounterState_ != EncounterState::kAppearing) {
        return 0.0f;
    }

    return std::clamp(encounterTimer_ / GetAppearanceDuration(), 0.0f, 1.0f);
}

float EnemyFalseKingSlime::GetPhaseTransitionProgress() const {
    // 形態移行中は経過時間、移行外は形態の一致状態を返します。
    if (attackState_ != AttackState::kPhaseShift) {
        return battlePhase_ == pendingBattlePhase_ ? 1.0f : 0.0f;
    }

    return std::clamp(phaseTransitionTimer_ / (std::max)(phaseTransitionDuration_, 0.01f), 0.0f, 1.0f);
}

void EnemyFalseKingSlime::TriggerDebugDefeat() {
    // プレビューの撃破操作では HP をゼロにし、残った攻撃表示を片付けます。
    // HPを持たない個体と、既に死亡または撃破演出中の個体には撃破を再要求しない。
    if (!GetParameters().has_value() || IsDead() || IsDefeatEffectPlaying()) {
        return;
    }

    SetParameter(&engine::EntityParameter::hp, 0.0f);
    SetVelocity({0.0f, 0.0f, 0.0f});
    ClearTransientVisuals();
    HideAttackTelegraph();
}

bool EnemyFalseKingSlime::CanTriggerDebugPhaseTwoTransition() const {
    // 第一形態の通常戦闘中だけデバッグ移行を受け付けます。
    return IsEncounterControlled() && encounterState_ == EncounterState::kActive && !IsDead() &&
           debugPreviewAttackId_.empty() && !crownInterlude_ && !finalArenaStarted_ && battlePhase_ == 1 &&
           attackState_ != AttackState::kPhaseShift;
}

bool EnemyFalseKingSlime::CanTriggerDebugCrownInterlude() const {
    // 王冠幕間の重複開始や最終決戦中の逆戻りを防ぎます。
    return IsEncounterControlled() && encounterState_ == EncounterState::kActive && !IsDead() &&
           debugPreviewAttackId_.empty() && !crownInterlude_ && !finalArenaStarted_ && battlePhase_ <= 2 &&
           attackState_ != AttackState::kPhaseShift;
}

bool EnemyFalseKingSlime::CanTriggerDebugFinalDefeat() const {
    // 最終決戦に到達している時だけ撃破プレビューを許可します。
    return IsEncounterControlled() && encounterState_ == EncounterState::kActive && !IsDead() && !crownInterlude_ &&
           finalArenaStarted_ && battlePhase_ == 3;
}

void EnemyFalseKingSlime::TriggerDebugPhaseTwoTransition() {
    // 実際の形態変更は更新側へ予約し、ここでは要求だけ記録します。
    // 第一形態の通常戦闘など、デバッグ移行を許可する条件が揃わなければ予約しない。
    if (!CanTriggerDebugPhaseTwoTransition())
        return;
    pendingDebugSeals_ = true;
}

void EnemyFalseKingSlime::TriggerDebugCrownInterlude() {
    // 更新途中で幕間へ飛ばず、次の進行タイミングへ予約します。
    // 幕間へ移行できる参加状態と形態でなければ、予約を出さない。
    if (!CanTriggerDebugCrownInterlude())
        return;
    pendingDebugInterlude_ = true;
}

void EnemyFalseKingSlime::HoldFinalDefeat() {
    // 勝利演出がボスを参照する間は死亡確定を保留し、攻撃判定だけ止めます。
    finalDefeatHeld_=true;
    finalDefeatCompleted_=false;
    SetVelocity({});
    SetCollisionAttribute(0);
    SetCollisionMask(0);
    HideAttackTelegraph();
    ClearTransientVisuals();
}

void EnemyFalseKingSlime::CompleteFinalDefeat() {
    // 演出終了後に一度だけ死亡を確定し、入口イベントへ撃破を通知します。
    if(finalDefeatCompleted_)
        return;
    finalDefeatCompleted_=true;
    SetDead(true);
    SetIsVisible(false);

    // 継承演出で使う本体はシーンに残し、通知先IDと現在のシーンが有効な場合だけ撃破を通知する。
    if(auto* scene=SceneManager::GetInstance()->GetCurrentScene();scene && GetTargetId()>0)
        scene->TriggerEvent(GetTargetId());
}

void EnemyFalseKingSlime::CancelFinalDefeat() {
    // 演出を中断する場合は死亡の保留と完了を解除し、死んでいない状態に戻す。
    finalDefeatHeld_=false;
    finalDefeatCompleted_=false;
    SetDead(false);
}

// 登場表示をまとめて所有し、途中終了とEditor停止でも残さないようにします。
void EnemyFalseKingSlime::EnableIntroPortal(bool enabled) {
    // 開始・再開に備えて粒子タイマーと前回再生時刻をリセットします。
    introPortalEnabled_ = enabled;
    bossAuraTimer_ = 0.0f;
    introPreviousTime_ = 0.0f;
    // ポータルの表示を開始するときは以前の戦闘オーラを解放し、登場用に作り直せるようにする。
    if (enabled) ClearBossAura();
    // 終了時はボスにかかったクリップを外し、ゲートと着地輪を破棄します。
    if (!enabled) {
        ClearPortalClip();
        introPortal_.reset();
        introShockwave_.reset();
    }
}

void EnemyFalseKingSlime::ConfigureIntroTiming(float start, float landing, float pose, float closing, float auraEnd) {
    // 時刻はTimeline開始からの秒数です。着地・ポーズ・閉鎖の順序を保ち、オーラの引き継ぎもポーズ以降にします。
    introStartTime_ = start;
    introLandTime_ = (std::max)(start + .7f, landing);
    introPoseTime_ = (std::max)(introLandTime_ + .1f, pose);
    introCloseTime_ = (std::max)(introPoseTime_, closing);
    introAuraEndTime_ = (std::max)(introPoseTime_, auraEnd);
}

// timeは登場Timelineの再生秒、dtは経過秒。有効かつ開始時刻以降のポータル・着地演出を更新します。
void EnemyFalseKingSlime::UpdateIntroPortal(float time, float frameDeltaTime) {
    // ゲートが無効か登場時刻より前なら、描画物の生成と再生を省きます。
    if (!introPortalEnabled_ || time < introStartTime_) return;
    const auto& style = BossIntroSettings::Get();
    // 最初の全身寸法を保持し、本体の伸縮でゲートまで揺れないようにします。
    if (introWidth_ <= 0.0f) {
        const auto bounds = GetModelWorldAabb();
        introWidth_ = (std::max)(2.0f, bounds.max.x - bounds.min.x);
        introHeight_ = (std::max)(2.0f, bounds.max.y - bounds.min.y);
    }
    // 配置位置と初回に記録した全身寸法を起点に、ゲートと着地輪を必要になった時だけ生成します。
    const float width = introWidth_, height = introHeight_;
    const engine::Vector3 center = authoredPosition_ + engine::Vector3{0, height * .72f, -4};
    auto ensureVisual = [this](std::unique_ptr<engine::Object3D>& visual) {
        // ポータルと着地輪は別々に保持し、作成済みならその描画物をそのまま再利用する。
        if (visual) return;
        visual = std::make_unique<engine::Object3D>();
        visual->Initialize(GetObjectCommon());
        visual->SetModel("Gimmicks/portal_surface");
        visual->SetColliderType(engine::ColliderType::kNone);
        visual->SetCollisionAttribute(0);
        visual->SetMaterialType(22);
        visual->SetBlendMode(BlendMode::kNormal);
        visual->SetEnableLighting(false);
        visual->SetEmissive(1.0f);
        visual->SetCameraOcclusionMode(engine::Object3D::CameraOcclusionMode::kKeepVisible);
    };
    ensureVisual(introPortal_);
    ensureVisual(introShockwave_);

    // 開口で横幅を広げ、閉鎖は材質にも進行率を渡して見た目と同期させます。
    const float opening = SmoothStep01((time - introStartTime_) / .65f);
    const float closing = SmoothStep01((time - introCloseTime_) / style.closeDuration);
    introPortal_->SetIsVisible(closing < 1.0f && opening > .001f);
    introPortal_->SetTranslate(center);
    introPortal_->SetScale({width * .80f * style.gateScale * opening, height * .98f * style.gateScale, 1});
    introPortal_->SetColor({style.gateColor[0], style.gateColor[1], style.gateColor[2], .65f});
    // 水系の定数バッファが使える場合は、ポータルの開閉率と表面の流れをシェーダーに渡す。
    if (auto* v = introPortal_->GetMeshRenderer()->GetWaterParamData()) {
        v->effectType = -2; v->waveSpeed = 1.1f; v->waveFrequency = 16; v->waveHeight = .85f;
        v->effectIntensity = 1.4f; v->effectSoftness = .6f; v->effectScale = 1.12f;
        v->effectScaleX = v->effectScaleY = 1; v->effectScaleZ = .12f;
        v->uvOffsetX = opening; v->uvOffsetY = 1; v->flowSpeedX = closing; v->billboardScale = 1;
    }
    // 着地直後までボス本体をゲートの面で切り取り、通り抜けた後はクリップを解除します。
    if (time < introLandTime_ + .1f) {
        engine::Object3D::PortalClipSettings clip;
        clip.enabled = true; clip.center = center; clip.normal = {0, 0, -1};
        clip.progress = .47f; clip.edgeWidth = .14f; clip.dissolve = .08f;
        clip.color = {.65f, .42f, 1.7f, 1};
        SetPortalClip(clip);
    // 着地後の猶予を過ぎたら、クリップ面が本体を切り続けないよう解除する。
    } else ClearPortalClip();

    // 登場の強さをゼロへ落とさず、0.8秒で通常戦闘の濃さへ引き継ぎます。
    const float handoff = SmoothStep01((time - introAuraEndTime_) / .8f);
    const float aura = SmoothStep01((time - introStartTime_ - .9f) / .65f) *
        std::lerp(1.0f, style.idleAuraStrength, handoff);
    const float poseBurst = std::exp(-std::pow((time - introPoseTime_) / .25f, 2.0f));
    bossAuraStrength_ = aura;
    UpdateAuraVisual(aura, poseBurst, frameDeltaTime);

    // 着地時刻から0.8秒間だけ輪を表示し、半径を広げながら透明にする。
    const float impact = std::clamp((time - introLandTime_) / .8f, 0.0f, 1.0f);
    const float radius = width * (.28f + 1.25f * (1 - (1-impact)*(1-impact))) * style.impactScale;
    introShockwave_->SetIsVisible(time >= introLandTime_ && impact < 1);
    // 着地輪を表示する間だけ地面を調べ、床と重ならない高さへ置く。
    if (introShockwave_->GetIsVisible())
        introShockwave_->SetTranslate(FindGroundPoint(authoredPosition_) + engine::Vector3{0, .08f, 0});
    // 標準のπ/2から0方向へ1段階寄せ、従来の衝撃波の回転値を維持します。
    const float shockwaveRotationX = std::nextafter(std::numbers::pi_v<float> / 2.0f, 0.0f);
    introShockwave_->SetRotation({shockwaveRotationX, 0, 0});
    introShockwave_->SetScale({radius, radius, 1});
    introShockwave_->SetColor({.58f, .36f, 1.0f, (1-impact)*(1-impact) * .9f});
    // 特殊材質の定数があれば、輪に使う描画モードと板の倍率を設定する。
    if (auto* v = introShockwave_->GetMeshRenderer()->GetWaterParamData()) {
        v->effectType = -4; v->billboardScale = 1; v->effectScaleX = v->effectScaleY = v->effectScaleZ = 1;
    }
    // 発生ごとに登録済みの粒子設定をコピーし、位置・速度・大きさだけを演出に合わせます。
    auto* particles = GpuParticleManager::GetInstance();
    auto emit = [&](const char* name, const engine::Vector3& position, const engine::Vector3& velocity,
                    float size, std::int32_t count, const engine::Vector3& area) {
        // GPU粒子の管理が初期化されていなければこの発生要求を飛ばす。
        if (!particles || !particles->IsInitialized()) return;
        const auto found = vfxAssetPath::FindPreset(particles->GetPresets(), name);
        // 名前に対応するプリセットを解決できなければ、発生設定を作らない。
        if (found == particles->GetPresets().end()) return;
        auto config = found->second;
        config.emitPos = position; config.emitVelocity = velocity; config.emitArea = area;
        config.baseSize *= size; config.midSize *= size; config.endSize *= size;
        config.emitCount = count; config.isLooping = false;
        particles->EmitFromConfig(config);
    };
    // 着地時刻をまたいだフレームにだけ、地面上の12方向へ粒子を放つ。
    if (introPreviousTime_ < introLandTime_ && time >= introLandTime_) {
        constexpr float kFullTurn = 2.0f * std::numbers::pi_v<float>; // 1周分の角度（ラジアン）。
        for (std::int32_t i = 0; i < 12; ++i) {
            const float angle = i * kFullTurn / 12;
            const engine::Vector3 direction{std::cos(angle), 0, std::sin(angle)};
            emit("enemy/false_king/false_king_intro_gold", FindGroundPoint(authoredPosition_) + direction * width * .3f + engine::Vector3{0,.12f,0},
                 direction * 5.0f + engine::Vector3{0, 1.5f, 0}, style.impactScale, 4, {.1f,.1f,.1f});
        }
    }
    // 着地とは別に、ポーズの時刻をまたいだときにボスの体から上向きの粒子を出します。
    if (introPreviousTime_ < introPoseTime_ && time >= introPoseTime_)
        emit("enemy/false_king/false_king_intro_gold", GetWorldPosition() + engine::Vector3{0,height*.52f,0},
             {0,4,0}, 1.35f * style.auraScale, 32, {width*.35f,height*.2f,.4f});
    // 閉鎖開始後かつ閉じ切る前に、再生が前進して12Hzの区切りをまたいだフレームで縁から中心へ粒子を出す。
    if (time > introCloseTime_ && closing < .98f && introPreviousTime_ < time &&
        std::int32_t(time * 12) != std::int32_t(introPreviousTime_ * 12)) {
        for (std::int32_t i = 0; i < 5; ++i) {
            const float angle = time * 1.7f + i * 1.256637f;
            const engine::Vector3 offset{std::cos(angle)*width*.48f, std::sin(angle)*height*.48f, .12f};
            emit("enemy/false_king/false_king_intro_gold", center + offset, offset * -1.4f,
                 1-closing*.65f, 2, {.08f,.08f,.08f});
        }
    }
    // 次の更新で時刻をまたいだか判断できるようにし、最後に二つの描画物へ変形を反映します。
    introPreviousTime_ = time;
    for (auto* visual : {introPortal_.get(), introShockwave_.get()}) {
        visual->Update(frameDeltaTime);
        visual->UpdateWorldMatrix(true);
    }
}

// ゲートとは独立した戦闘表示。撃破・待機・Editor停止では状態ごと解放します。
void EnemyFalseKingSlime::ClearBossAura() {
    bossAura_.reset();
    bossAuraStrength_ = bossAuraTime_ = bossAuraTimer_ = 0.0f;
    introWidth_ = introHeight_ = 0.0f;
}

// 通常戦闘中の状態から表示の強さを選ぶ。登場中の表示更新はUpdateIntroPortal()から行う。
void EnemyFalseKingSlime::UpdateBossAura(float frameDeltaTime) {
    // 編集時や撃破処理中は戦闘オーラを破棄し、描画・粒子の更新を止めます。
    const auto* manager = SceneManager::GetInstance();
    // 停止・不可視・撃破に該当する間は、戦闘用オーラの実体とタイマーを解放する。
    if (!manager || !manager->IsPlaying() || IsDead() || !GetIsVisible() ||
        finalDefeatHeld_ || ShouldHandleDefeatEffect()) {
        ClearBossAura();
        return;
    }
    // 登場中の更新はTimelineが担当し、同じフレームに二重更新しません。
    if (introPresentationActive_) return;
    // 入口や戦闘状態が待機中なら、登場前のオーラが表示されないよう片付ける。
    if (IsDormant() || (IsEncounterControlled() && encounterState_ == EncounterState::kDormant)) {
        ClearBossAura();
        return;
    }
    // 待機・攻撃・形態移行・幕間で目標の強さを選び、急に明るさを切り替えないよう補間します。
    const auto& style = BossIntroSettings::Get();
    const bool sealAttacking = battlePhase_ == 2 &&
        (sealState_ == SealState::kChant || sealState_ == SealState::kAttack);
    const bool attacking = attackState_ == AttackState::kWindup || attackState_ == AttackState::kActive || sealAttacking;
    const float target = crownInterlude_ ? style.idleAuraStrength * .55f :
        attackState_ == AttackState::kPhaseShift ? (std::max)(1.0f, style.attackAuraStrength) :
        attacking ? style.attackAuraStrength : style.idleAuraStrength;
    bossAuraStrength_ += (target - bossAuraStrength_) * (1.0f - std::exp(-5.0f * frameDeltaTime));
    UpdateAuraVisual(bossAuraStrength_, 0.0f, frameDeltaTime);
}

void EnemyFalseKingSlime::AlignBossAura(engine::Camera* camera) {
    // 描画物または視点が用意できなければ、板の向きと位置を更新しない。
    if (!bossAura_ || !camera) return;
    // カメラ方向を水平面に投影し、縦に立てたまま板を視点へ向けます。
    const auto position = GetWorldPosition();
    auto forward = camera->GetEye() - position;
    forward.y = 0;
    const float length = std::sqrt(forward.x * forward.x + forward.z * forward.z);
    forward = length > .001f ? forward / length : engine::Vector3{0,0,1};
    bossAura_->SetRotation({0, std::atan2(forward.x, forward.z), 0});
    // カメラから見て体の奥へ置き、顔を覆わず左右の輪郭へ流れを残します。
    bossAura_->SetTranslate(position + engine::Vector3{0,introHeight_*.34f,0} - forward*.25f);
    bossAura_->UpdateLocalMatrix();
    bossAura_->UpdateWorldMatrix(true);
}

// 共有している板状のオーラを描画し、強さとポーズによる増幅に応じて追加粒子を発生させる。
void EnemyFalseKingSlime::UpdateAuraVisual(float strength, float poseBurst, float frameDeltaTime) {
    const auto& style = BossIntroSettings::Get();
    // 登場演出を経ずに戦闘に来た場合も、先にモデルからオーラの基準寸法を取得します。
    if (introWidth_ <= 0) {
        const auto bounds = GetModelWorldAabb();
        introWidth_ = (std::max)(2.0f,bounds.max.x-bounds.min.x);
        introHeight_ = (std::max)(2.0f,bounds.max.y-bounds.min.y);
    }
    // オーラの板は初回だけ作り、当たり判定を持たない専用材質として扱います。
    if (!bossAura_) {
        bossAura_ = std::make_unique<engine::Object3D>();
        bossAura_->Initialize(GetObjectCommon());
        bossAura_->SetModel("Gimmicks/portal_surface");
        bossAura_->SetColliderType(engine::ColliderType::kNone);
        bossAura_->SetCollisionAttribute(0);
        bossAura_->SetMaterialType(22);
        bossAura_->SetBlendMode(BlendMode::kNormal);
        bossAura_->SetEnableLighting(false);
        bossAura_->SetEmissive(1);
        bossAura_->SetCameraOcclusionMode(engine::Object3D::CameraOcclusionMode::kKeepVisible);
        auto* v = bossAura_->GetMeshRenderer()->GetWaterParamData();
        v->effectType=-3;v->billboardScale=1;v->effectScaleX=v->effectScaleY=v->effectScaleZ=1;
    }
    // 現在の強さとポーズの瞬間的な強調から、不透明度と横幅を調整します。
    bossAuraTime_ += frameDeltaTime;
    const float breath = introPresentationActive_ ? 1.0f : .96f + .04f*std::sin(bossAuraTime_*2);
    bossAura_->SetIsVisible(strength*style.auraScale*style.auraOpacity > .001f);
    bossAura_->SetScale({introWidth_*(.87f+poseBurst*.10f)*style.auraScale,
                        introHeight_*.72f*style.auraScale,1});
    bossAura_->SetColor({.35f,.13f,.92f,strength*style.auraOpacity*(.85f+.25f*poseBurst)*breath});
    bossAura_->Update(frameDeltaTime);
    // 板を視点に向けてから粒子の発生を判定し、不可視・停止中・間隔待ちでは粒子を出しません。
    auto* camera=CameraManager::GetInstance()->GetActiveCamera();
    AlignBossAura(camera);
    bossAuraTimer_ -= frameDeltaTime;
    // 非表示、強さ不足、時間停止、発生間隔のいずれかに該当するフレームは描画更新だけで戻る。
    if (!bossAura_->GetIsVisible() || strength < .01f || frameDeltaTime <= 0 || bossAuraTimer_ > 0) return;
    // 通常戦闘では発生間隔と個数も抑え、攻撃予兆と視界を確保します。
    bossAuraTimer_ = introPresentationActive_ ? .07f : .14f;
    auto forward = camera ? camera->GetEye()-GetWorldPosition() : engine::Vector3{0,0,1};
    forward.y=0;
    const float length=std::sqrt(forward.x*forward.x+forward.z*forward.z);
    forward=length>.001f?forward/length:engine::Vector3{0,0,1};
    const engine::Vector3 right{forward.z,0,-forward.x};
    auto* particles=GpuParticleManager::GetInstance();
    // GPU粒子が使えない間は、板の表示を維持して粒子生成だけ中止する。
    if (!particles || !particles->IsInitialized()) return;
    // カメラに対して左右の輪郭に発生位置を置き、各プリセットを現在のオーラの強さに合わせます。
    for (float side:{-1.0f,1.0f}) {
        const auto pos=GetWorldPosition()+right*(side*introWidth_*(.43f+.04f*std::sin(bossAuraTime_*2.6f+side)))+
            engine::Vector3{0,introHeight_*.18f,0}+forward*.12f;
        for (const char* name:{"enemy/false_king/false_king_intro_wisp","enemy/false_king/false_king_intro_gold"}) {
            const auto found=vfxAssetPath::FindPreset(particles->GetPresets(), name);
            // 登録のない粒子は飛ばし、その側で残っている別のプリセットは引き続き処理する。
            if(found==particles->GetPresets().end())continue;
            auto config=found->second;
            config.emitPos=pos;config.emitVelocity=right*(-side*.6f)+engine::Vector3{0,3.5f,0};
            config.emitArea={introWidth_*.08f,introHeight_*.08f,.12f};
            const float size=strength*style.auraScale;
            config.baseSize*=size;config.midSize*=size;config.endSize*=size;
            config.baseColor.w*=style.auraOpacity;config.midColor.w*=style.auraOpacity;config.endColor.w*=style.auraOpacity;
            config.emitCount=introPresentationActive_?3:1;config.isLooping=false;
            particles->EmitFromConfig(config);
        }
    }
}
