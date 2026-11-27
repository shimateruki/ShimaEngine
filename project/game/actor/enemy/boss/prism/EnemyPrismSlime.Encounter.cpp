#include "game/actor/enemy/boss/prism/EnemyPrismSlime.Internal.h"
#include "game/actor/enemy/boss/core/MidbossEncounterEffects.h"

using namespace enemyPrismSlimeImplementation;

// 遭遇イベントで起動するプリズムスライムの、編集時・登場・戦闘開始までの状態を管理します。
bool EnemyPrismSlime::UpdateEncounterState(float deltaTime) {
    // 配置が遭遇イベント制御を指定していなければ、通常の敵更新に処理を返します。
    if (!IsEncounterControlled()) {
        encounterState_ = EncounterState::kNormal;
        encounterInitializedForPlay_ = false;
        return false;
    }

    SceneManager* sceneManager = SceneManager::GetInstance();
    const bool isPlaying = sceneManager && sceneManager->IsPlaying();

    // シーンが再生中でなければ、編集状態へ戻し、このフレームの戦闘AIを止める。
    if (!isPlaying) {
        ResetEncounterStateForEditor();
        return true;
    }

    // 今回のPlayで遭遇をまだ初期化していなければ、配置と起動イベントから開始状態を決める。
    if (!encounterInitializedForPlay_) {
        InitializeEncounterState();
    }

    // 起動待ちでは速度と攻撃表示を止めます。登場自体の更新は Appearing の間だけ実行します。
    if (encounterState_ == EncounterState::kDormant) {
        SetVelocity({0.0f, 0.0f, 0.0f});
        ClearPrismAttackVisuals();
        HideAttackTelegraph();
        return true;
    }

    // 休眠にも登場にも該当しない状態では、通常の敵更新に処理を返す。
    if (encounterState_ != EncounterState::kAppearing) {
        return false;
    }

    // 配置位置の上から小さい姿で現れ、接地を維持しながら本来の大きさと色へ近づけます。
    encounterTimer_ += (std::max)(0.0f, deltaTime);
    idleTimer_ += (std::max)(0.0f, deltaTime);
    const float duration = GetEncounterAppearanceDuration();
    const float progress = std::clamp(encounterTimer_ / duration, 0.0f, 1.0f);
    const float eased = SmoothStep01(progress);
    const float settlePulse = std::sin(progress * kPi) * 0.13f;

    SetIsVisible(true);

    // 登場演出中も地面との衝突だけは維持し、床抜けを防ぎます。
    SetCollisionAttribute(kPrismActiveCollisionAttribute);
    SetCollisionMask(kPrismAppearanceCollisionMask);
    SetVelocity({0.0f, 0.0f, 0.0f});
    SetMaterialType(4);
    SetTranslate({
        encounterBasePosition_.x,
        encounterBasePosition_.y + (1.0f - eased) * kEncounterAppearanceRise,
        encounterBasePosition_.z,
    });
    const float scaleRate = 0.28f + eased * 0.72f + settlePulse;
    SetScale({
        encounterBaseScale_.x * scaleRate,
        encounterBaseScale_.y * (0.18f + eased * 0.82f + settlePulse * 0.72f),
        encounterBaseScale_.z * scaleRate,
    });
    SetColor({
        encounterBaseColor_.x,
        encounterBaseColor_.y,
        encounterBaseColor_.z,
        (std::max)(0.015f, eased),
    });
    SetEmissive(1.35f + (1.0f - std::abs(progress * 2.0f - 1.0f)) * 2.7f);
    SyncCollisionRadius();

    // 登場が終わった時点で、戦闘用の衝突設定を戻す処理へ進みます。
    if (progress >= 1.0f) {
        FinishEncounterAppearance();
    }

    return true;
}

void EnemyPrismSlime::CaptureEncounterAuthoredState(bool refreshVisualTransform) {
    // 配置値を保存済みで再取得の指定も無ければ、現在の演出姿勢で基準値を上書きしない。
    if (encounterAuthoredStateCaptured_ && !refreshVisualTransform) {
        return;
    }

    encounterBasePosition_ = GetTransform()->translate;
    encounterBaseScale_ = GetScale();
    encounterBaseColor_ = GetColor();
    encounterBaseEmissive_ = GetEmissive();
    encounterMaterialType_ = GetMaterialType();

    const uint32_t collisionAttribute = GetCollisionAttribute();
    const uint32_t collisionMask = GetCollisionMask();

    // 有効な衝突属性は配置値として記録し、登場後に戻せるようにする。
    if (collisionAttribute != 0) {
        encounterCollisionAttribute_ = collisionAttribute;
    // 属性が0で配置の初回取得なら、休眠保存の値を戦闘に持ち越さないよう既定値を用意する。
    } else if (!encounterAuthoredStateCaptured_) {
        encounterCollisionAttribute_ = kPrismActiveCollisionAttribute;
    }

    // 衝突マスクが0以外のときは、戦闘開始後に現在のマスクを戻すため記録する。
    if (collisionMask != 0) {
        encounterCollisionMask_ = collisionMask;
    // マスクが0で配置の初回取得なら、地面などと接触できる戦闘用の既定値を用意する。
    } else if (!encounterAuthoredStateCaptured_) {
        encounterCollisionMask_ = kPrismActiveCollisionMask;
    }

    baseScale_ = encounterBaseScale_;
    hasBaseScale_ = true;
    encounterAuthoredStateCaptured_ = true;
}

void EnemyPrismSlime::InitializeEncounterState() {
    // Play開始時に攻撃順を初期化し、配置設定に従って登場か休眠へ振り分ける。
    encounterInitializedForPlay_ = true;
    CaptureEncounterAuthoredState();
    attackCycleCount_ = 0;
    attackCycleIndex_ = 0;
    lastAutomaticAttack_ = AttackKind::kNone;

    const bool startActive = GetParameters().has_value() && GetParameters()->startActive;

    // 初回更新より先に起動イベントが届いていれば、配置時の開始フラグが無効でも登場します。
    if (startActive || encounterRequestedActive_) {
        BeginEncounterAppearance();
    // 配置の開始フラグと起動要求のどちらも無ければ、イベントを待つ休眠状態にする。
    } else {
        ApplyDormantEncounterState();
    }
}

void EnemyPrismSlime::ResetEncounterStateForEditor() {
    locomotion_.Reset();
    // Play中に初期化していた場合だけ、演出で変えた姿勢や材質を配置時の値へ戻す。
    if (encounterInitializedForPlay_) {
        SetTranslate(encounterBasePosition_);
        SetScale(encounterBaseScale_);
        SetColor(encounterBaseColor_);
        SetMaterialType(encounterMaterialType_);
        SetEmissive(encounterBaseEmissive_);
        SyncCollisionRadius();
    }

    // 非再生中も配置値は更新できるよう保持しつつ、ゲーム画面には待機中ボスを出しません。
    CaptureEncounterAuthoredState(true);
    encounterState_ = EncounterState::kDormant;
    encounterTimer_ = 0.0f;
    encounterInitializedForPlay_ = false;
    encounterRequestedActive_ = false;
    attackCycleCount_ = 0;
    attackCycleIndex_ = 0;
    lastAutomaticAttack_ = AttackKind::kNone;
    SetVelocity({0.0f, 0.0f, 0.0f});
    SetCollisionAttribute(0);
    SetCollisionMask(0);
    // 編集へ戻った後は待機姿勢を保持し、前回の攻撃表示を残しません。
    SetIsVisible(false);
    ClearPrismAttackVisuals();
    HideAttackTelegraph();
}

void EnemyPrismSlime::BeginEncounterAppearance() {
    locomotion_.Reset();
    encounterState_ = EncounterState::kAppearing;
    encounterTimer_ = 0.0f;
    attackCooldown_ = 1.0f;
    SetIsVisible(true);

    // 演出中は攻撃・プレイヤー接触を無効にし、地面との衝突だけ有効にします。
    SetCollisionAttribute(kPrismActiveCollisionAttribute);
    SetCollisionMask(kPrismAppearanceCollisionMask);
    SetVelocity({0.0f, 0.0f, 0.0f});
    ClearPrismAttackVisuals();
    HideAttackTelegraph();

    engine::Vector3 effectPosition = encounterBasePosition_;
    effectPosition.y += 0.15f;
    VfxSequencer::PlayOneShot(MidbossEncounterEffects::Prism().appearance, effectPosition, {1.65f, 1.65f, 1.65f});
}

void EnemyPrismSlime::ApplyDormantEncounterState() {
    locomotion_.Reset();
    // 未登場中は配置姿勢を保ちながら表示と衝突を止め、攻撃の残像も消す。
    encounterState_ = EncounterState::kDormant;
    encounterTimer_ = 0.0f;
    SetTranslate(encounterBasePosition_);
    SetScale(encounterBaseScale_);
    SetColor(encounterBaseColor_);
    SetMaterialType(encounterMaterialType_);
    SetVelocity({0.0f, 0.0f, 0.0f});
    repositionTimer_ = 0.0f;
    repositionDuration_ = 0.0f;
    repositionJumpPending_ = false;
    SetCollisionAttribute(0);
    SetCollisionMask(0);
    SetIsVisible(false);
    ClearPrismAttackVisuals();
    HideAttackTelegraph();
}

void EnemyPrismSlime::FinishEncounterAppearance() {
    // 登場演出中に抑えていた判定を配置時の値へ戻して戦闘を始める。
    encounterState_ = EncounterState::kActive;
    encounterTimer_ = 0.0f;
    SetTranslate(encounterBasePosition_);
    SetScale(encounterBaseScale_);
    SetColor(encounterBaseColor_);
    SetMaterialType(encounterMaterialType_);
    SetEmissive(encounterBaseEmissive_);
    SetCollisionAttribute(encounterCollisionAttribute_);
    SetCollisionMask(encounterCollisionMask_);
    SetVelocity({0.0f, 0.0f, 0.0f});
    SetIsVisible(true);
    SyncCollisionRadius();
}

bool EnemyPrismSlime::IsEncounterControlled() const {
    // 通常AIではなくステージ演出で起動する個体だけ登場状態を管理する。
    return GetParameters().has_value() && GetParameters()->actionMode == kEncounterControlledActionMode;
}

float EnemyPrismSlime::GetEncounterAppearanceDuration() const {
    // ステージ側の揺れ時間を登場演出に使い、極端な設定値は制限する。
    return GetParameters().has_value() ? (std::clamp)(GetParameters()->shakeDuration, 0.35f, 3.0f) : kDefaultEncounterAppearanceDuration;
}

bool EnemyPrismSlime::IsEncounterHudActive() const {
    // 登場前と撃破後はボスHP表示を消し、演出開始から戦闘中だけ表示する。
    if (!IsEncounterControlled() || IsDead()) {
        return false;
    }

    return encounterState_ == EncounterState::kAppearing || encounterState_ == EncounterState::kActive;
}

float EnemyPrismSlime::GetEncounterCurrentHp() const {
    // HP未設定時もHUDに負数を渡さない。
    return GetParameters().has_value() ? (std::max)(0.0f, GetParameters()->hp) : 0.0f;
}

float EnemyPrismSlime::GetEncounterMaximumHp() const {
    // 最大HPは表示割合の分母に使うため、最低1を保証する。
    return GetParameters().has_value() ? (std::max)(1.0f, GetParameters()->maxHp) : 1.0f;
}

float EnemyPrismSlime::GetEncounterAppearanceProgress() const {
    // 休眠は0、戦闘中は1とし、登場中だけ演出タイマーから進行率を返す。
    if (encounterState_ == EncounterState::kActive) {
        return 1.0f;
    }

    // 戦闘開始前で登場中でもない場合は、登場の進行を0として返す。
    if (encounterState_ != EncounterState::kAppearing) {
        return 0.0f;
    }

    return std::clamp(encounterTimer_ / GetEncounterAppearanceDuration(), 0.0f, 1.0f);
}

void EnemyPrismSlime::TriggerDebugDefeat() {
    // HP設定が無い場合や撃破済み、撃破演出中の場合は、デバッグ撃破の状態を作らない。
    if (!GetParameters().has_value() || IsDead() || IsDefeatEffectPlaying()) {
        return;
    }

    // HPを0にしたら、移動と攻撃を片付けて通常の撃破更新へ引き継ぐ。
    SetParameter(&engine::EntityParameter::hp, 0.0f);
    locomotion_.Reset();
    attackState_ = AttackState::kIdle;
    currentAttack_ = AttackKind::kNone;
    attackTimer_ = 0.0f;
    repositionTimer_ = 0.0f;
    repositionDuration_ = 0.0f;
    repositionJumpPending_ = false;
    SetVelocity({0.0f, 0.0f, 0.0f});
    ClearPrismAttackVisuals();
    HideAttackTelegraph();
}
