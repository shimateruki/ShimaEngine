#define NOMINMAX
#include "game/actor/combat/CombatGroundQuery.h"
#include "game/actor/enemy/boss/core/MidbossEncounterEffects.h"
#include "game/actor/combat/CombatMath.h"
#include "game/actor/enemy/boss/magma/EnemyMagmaSlime.Internal.h"
#include "game/actor/enemy/boss/core/BossChantVisual.h"

#include "CollisionConfig.h"
#include "CollisionManager.h"
#include "game/system/rules/EventManager.h"
#include "GpuParticleManager.h"
#include "MeshRenderer.h"
#include "SceneManager.h"
#include "game/actor/enemy/animation/SlimeBounceAnimator.h"
#include "game/actor/enemy/animation/BossGroundHopVfx.h"
#include "VfxSequencer.h"

#include <algorithm>
#include <cassert>
#include <cmath>

using namespace enemyMagmaSlimeImplementation;

// 遭遇制御中は通常AIに先行し、Editor復帰・待機・登場の状態を処理します。
bool EnemyMagmaSlime::UpdateEncounterState(float deltaTime) {
    // 配置が遭遇イベント制御を指定していなければ、通常の敵更新に処理を返します。
    if (!IsEncounterControlled()) {
        encounterState_ = EncounterState::kNormal;
        encounterInitializedForPlay_ = false;
        return false;
    }

    SceneManager* sceneManager = SceneManager::GetInstance();

    // シーン管理を使えないか再生中でなければ、編集状態へ戻して戦闘AIを止める。
    if (!sceneManager || !sceneManager->IsPlaying()) {
        ResetEncounterStateForEditor();
        return true;
    }

    // 今回のPlayで遭遇を初期化していなければ、配置値と起動要求を参照して準備する。
    if (!encounterInitializedForPlay_) {
        InitializeEncounterState();
    }

    // 起動待ちでは速度と攻撃表示を止めます。登場自体の更新は Appearing の間だけ実行します。
    if (encounterState_ == EncounterState::kDormant) {
        SetVelocity({0.0f, 0.0f, 0.0f});
        ClearTransientVisuals();
        HideAttackTelegraph();
        return true;
    }

    // 休眠にも登場にも該当しない状態では、通常の敵更新に処理を返す。
    if (encounterState_ != EncounterState::kAppearing) {
        return false;
    }

    // 配置位置の上から回転しながら降り、登場時計に合わせて形と発光を通常の姿へ近づけます。
    encounterTimer_ += (std::max)(0.0f, deltaTime);
    idleTimer_ += (std::max)(0.0f, deltaTime);
    const float progress = std::clamp(encounterTimer_ / GetEncounterAppearanceDuration(), 0.0f, 1.0f);
    const float eased = SmoothStep01(progress);
    const float landingPulse = std::sin(progress * kPi) * 0.16f;
    const float horizontalScale = 0.24f + eased * 0.76f + landingPulse;
    const float verticalScale = 0.12f + eased * 0.88f + landingPulse * 0.55f;

    // 小さく浮いた姿から配置時の大きさへ戻す間、衝突判定はまだ有効にしません。
    SetIsVisible(true);
    SetCollisionAttribute(0);
    SetCollisionMask(0);
    SetVelocity({0.0f, 0.0f, 0.0f});
    SetTranslate({
        encounterBasePosition_.x,
        encounterBasePosition_.y + (1.0f - eased) * kEncounterAppearanceRise,
        encounterBasePosition_.z,
    });
    SetRotation({
        encounterBaseRotation_.x,
        encounterBaseRotation_.y + (1.0f - eased) * kPi * 0.36f,
        encounterBaseRotation_.z,
    });
    SetScale({
        encounterBaseScale_.x * horizontalScale,
        encounterBaseScale_.y * verticalScale,
        encounterBaseScale_.z * horizontalScale,
    });
    SetColor({
        encounterBaseColor_.x,
        encounterBaseColor_.y,
        encounterBaseColor_.z,
        (std::max)(0.01f, eased),
    });
    SetEmissive(encounterBaseEmissive_ + (1.0f - std::abs(progress * 2.0f - 1.0f)) * 1.75f);
    SyncCollisionRadius();

    // 登場が終わった時点で、戦闘用の衝突設定を戻す処理へ進みます。
    if (progress >= 1.0f) {
        FinishEncounterAppearance();
    }

    return true;
}

// PlayとEditorの切り替えに備え、配置時の姿勢・マテリアル・衝突設定を保持します。
void EnemyMagmaSlime::CaptureEncounterAuthoredState(bool refreshVisualTransform) {
    // 配置値を保存済みで再取得の指定も無ければ、現在の演出姿勢で基準値を上書きしない。
    if (encounterAuthoredStateCaptured_ && !refreshVisualTransform) {
        return;
    }

    encounterBasePosition_ = GetTransform()->translate;
    encounterBaseScale_ = GetScale();
    encounterBaseRotation_ = GetRotation();
    encounterBaseColor_ = GetColor();
    encounterBaseEmissive_ = GetEmissive();
    encounterMaterialType_ = GetMaterialType();
    const uint32_t collisionAttribute = GetCollisionAttribute();
    const uint32_t collisionMask = GetCollisionMask();

    // 衝突属性が0以外なら、登場後に復元する戦闘用の配置値として保存する。
    if (collisionAttribute != 0) {
        encounterCollisionAttribute_ = collisionAttribute;
    // 属性が0で配置の初回取得なら、休眠時の無効値を引き継がず敵属性を使う。
    } else if (!encounterAuthoredStateCaptured_) {
        encounterCollisionAttribute_ = engine::collisionCategories::Get("enemy");
    }

    // 衝突マスクが0以外の場合は、配置で指定された相手を戦闘開始後も使う。
    if (collisionMask != 0) {
        encounterCollisionMask_ = collisionMask;
    // マスクが0で配置の初回取得なら、戦闘用のプレイヤー、地面、攻撃、弾を既定で登録する。
    } else if (!encounterAuthoredStateCaptured_) {
        encounterCollisionMask_ = engine::collisionCategories::Get("player") | engine::collisionCategories::Get("ground") | engine::collisionCategories::Get("player_attack") | engine::collisionCategories::Get("player_bullet");
    }

    baseScale_ = encounterBaseScale_;
    hasBaseScale_ = true;
    encounterAuthoredStateCaptured_ = true;
}

// Play開始時に攻撃候補をリセットし、配置設定で登場開始の有無を決めます。
void EnemyMagmaSlime::InitializeEncounterState() {
    // Playの開始時に攻撃履歴を消し、配置値の起動設定を登場状態へ反映する。
    encounterInitializedForPlay_ = true;
    CaptureEncounterAuthoredState();
    attackCycleIndex_ = attackCycle_.size();
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

// 再生中の演出を片付け、Editorで調整した配置値へ戻します。
void EnemyMagmaSlime::ResetEncounterStateForEditor() {
    locomotion_.Reset();
    // Play中の演出姿勢と衝突を解除し、編集時の配置状態へ戻す。
    if (encounterInitializedForPlay_) {
        SetTranslate(encounterBasePosition_);
        SetScale(encounterBaseScale_);
        SetRotation(encounterBaseRotation_);
        SetColor(encounterBaseColor_);
        SetMaterialType(encounterMaterialType_);
        SetEmissive(encounterBaseEmissive_);
        SyncCollisionRadius();
    }

    // 編集で姿勢を変更できるよう基準値を取り直し、休眠または次のPlay開始に備える。
    CaptureEncounterAuthoredState(true);
    encounterState_ = EncounterState::kDormant;
    encounterTimer_ = 0.0f;
    encounterInitializedForPlay_ = false;
    encounterRequestedActive_ = false;
    attackCycleIndex_ = attackCycle_.size();
    lastAutomaticAttack_ = AttackKind::kNone;
    SetVelocity({0.0f, 0.0f, 0.0f});
    SetCollisionAttribute(0);
    SetCollisionMask(0);
    // 編集へ戻った後は待機姿勢を保持し、前回の攻撃表示を残しません。
    SetIsVisible(false);
    ClearTransientVisuals();
    HideAttackTelegraph();
}

// 登場中の衝突を止め、開始位置に出現VFXを再生します。
void EnemyMagmaSlime::BeginEncounterAppearance() {
    locomotion_.Reset();
    // 出現VFX中の被弾を避けるため、戦闘用の衝突を一時的に無効にする。
    encounterState_ = EncounterState::kAppearing;
    encounterTimer_ = 0.0f;
    attackCooldown_ = 1.1f;
    SetIsVisible(true);
    SetCollisionAttribute(0);
    SetCollisionMask(0);
    SetVelocity({0.0f, 0.0f, 0.0f});
    ClearTransientVisuals();
    HideAttackTelegraph();
    VfxSequencer::PlayOneShot(MidbossEncounterEffects::Magma().appearance, encounterBasePosition_ + engine::Vector3{0.0f, 0.18f, 0.0f},
                              {1.55f, 1.55f, 1.55f});
}

void EnemyMagmaSlime::ApplyDormantEncounterState() {
    locomotion_.Reset();
    // ステージから起動されるまで非表示にし、前回攻撃の演出を残さない。
    encounterState_ = EncounterState::kDormant;
    encounterTimer_ = 0.0f;
    SetTranslate(encounterBasePosition_);
    SetScale(encounterBaseScale_);
    SetRotation(encounterBaseRotation_);
    SetColor(encounterBaseColor_);
    SetMaterialType(encounterMaterialType_);
    SetEmissive(encounterBaseEmissive_);
    SetVelocity({0.0f, 0.0f, 0.0f});
    SetCollisionAttribute(0);
    SetCollisionMask(0);
    SetIsVisible(false);
    ClearTransientVisuals();
    HideAttackTelegraph();
}

// 登場完了時に配置時の衝突設定を戻し、戦闘状態へ切り替えます。
void EnemyMagmaSlime::FinishEncounterAppearance() {
    // 戦闘開始の状態を設定し、そのフレームで配置時の材質・姿勢・判定を復元する。
    encounterState_ = EncounterState::kActive;
    encounterTimer_ = 0.0f;
    SetTranslate(encounterBasePosition_);
    SetScale(encounterBaseScale_);
    SetRotation(encounterBaseRotation_);
    SetColor(encounterBaseColor_);
    SetMaterialType(encounterMaterialType_);
    SetEmissive(encounterBaseEmissive_);
    SetCollisionAttribute(encounterCollisionAttribute_);
    SetCollisionMask(encounterCollisionMask_);
    SetVelocity({0.0f, 0.0f, 0.0f});
    SetIsVisible(true);
    SyncCollisionRadius();
}

bool EnemyMagmaSlime::IsEncounterControlled() const {
    // ステージ演出で起動する個体だけ登場状態を管理する。
    return GetParameters().has_value() && GetParameters()->actionMode == kEncounterControlledActionMode;
}

float EnemyMagmaSlime::GetEncounterAppearanceDuration() const {
    // 既存の調整値を登場時間へ流用し、極端に短い・長い演出時間を制限する。
    return GetParameters().has_value() ? (std::clamp)(GetParameters()->shakeDuration, 0.45f, 3.0f) : kDefaultEncounterAppearanceDuration;
}

bool EnemyMagmaSlime::IsEncounterHudActive() const {
    // 登場前と撃破後はHPを隠し、登場演出から戦闘中だけ表示する。
    if (!IsEncounterControlled() || IsDead()) {
        return false;
    }

    return encounterState_ == EncounterState::kAppearing || encounterState_ == EncounterState::kActive;
}

float EnemyMagmaSlime::GetEncounterCurrentHp() const {
    // 表示側に負のHPを渡さない。
    return GetParameters().has_value() ? (std::max)(0.0f, GetParameters()->hp) : 0.0f;
}

float EnemyMagmaSlime::GetEncounterMaximumHp() const {
    // 表示割合のゼロ除算を避けるため、最大HPの下限を1にする。
    return GetParameters().has_value() ? (std::max)(1.0f, GetParameters()->maxHp) : 1.0f;
}

float EnemyMagmaSlime::GetEncounterAppearanceProgress() const {
    // 休眠・登場中・戦闘中を0から1の進行率としてHUDへ渡す。
    if (encounterState_ == EncounterState::kActive) {
        return 1.0f;
    }

    // 休眠など、戦闘前で登場も進んでいない場合は進行率を0として返す。
    if (encounterState_ != EncounterState::kAppearing) {
        return 0.0f;
    }

    return std::clamp(encounterTimer_ / GetEncounterAppearanceDuration(), 0.0f, 1.0f);
}
