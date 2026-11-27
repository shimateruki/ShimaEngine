#include <cstdint>
#include "EnemyThunderSlime.h"
#include "Easing.h"
#include <numbers>
#include "game/actor/enemy/animation/SlimeBounceAnimator.h"
#include "CameraManager.h"
#include "CollisionConfig.h"
#include "CollisionManager.h"
#include "EffectObject3D.h"
#include "game/system/rules/EventManager.h"
#include "GpuParticleManager.h"
#include "MeshEffectManager.h"
#include <algorithm>
#include <cmath>

namespace {
// 雷スライムの放電範囲、タメ時間、常時オーラの調整値
constexpr const char* kShockAttackId = "radial_shock";
constexpr const char* kWildLineAttackId = "line_lightning";
constexpr float kMoveSpeedScale = 1.12f;
constexpr float kWildStrikeInterval = 0.13f;
constexpr float kWildStrikeStartDistance = 2.8f;
constexpr float kWildStrikeSpacing = 2.7f;
constexpr float kLightningStrikeHeight = 9.5f;
constexpr float kGroundCollisionWorldRadius = 0.82f;
constexpr float kThrownCollisionWorldRadius = 1.18f;
constexpr float kMoveHopInterval = 0.25f;
constexpr float kMoveHopPower = 5.0f;
constexpr float kThunderSlimeModelYawOffset = std::numbers::pi_v<float>;
constexpr const char* kDischargePreset = "shared/thunder_slime_discharge";
constexpr const char* kIdleSparkPreset = "shared/thunder_slime_idle_spark";
constexpr const char* kRadialChargePreset = "enemy/thunder_slime/thunder_slime_radial_charge";
constexpr const char* kLineChargePreset = "shared/thunder_slime_charge";
constexpr const char* kLightningStrikeParticlePreset = "shared/player_thunder_strike_impact";
constexpr const char* kConstantAuraEffectPath = "Resources/json/effect/shared/effect_thunder_slime_constant_aura.json";
constexpr const char* kLightningWarningEffectPath = "Resources/json/effect/shared/effect_player_thunder_warning.json";
constexpr const char* kLightningBoltEffectPath = "Resources/json/effect/shared/effect_player_thunder_bolt.json";
constexpr const char* kLightningCoreEffectPath = "Resources/json/effect/shared/effect_player_thunder_core.json";
constexpr const char* kLightningImpactEffectPath = "Resources/json/effect/shared/effect_player_thunder_impact_ring.json";
constexpr const char* kChargeGroundEffectPath = "Resources/json/effect/enemy/thunder_slime/effect_thunder_charge_ground.json";
constexpr const char* kScorchMarkEffectPath = "Resources/json/effect/shared/effect_thunder_scorch_mark.json";
constexpr float kIdleSparkInterval = 0.095f;
constexpr float kTwoPi = (2.0f * std::numbers::pi_v<float>);
constexpr float kHalfPi = (std::numbers::pi_v<float> * 0.5f);

engine::Vector3 NormalizePlanar(engine::Vector3 value) {
    // 落雷列の向きは水平面に固定し、対象と重なるときは前方を使う。
    value.y = 0.0f;
    const float length = std::sqrt(value.x * value.x + value.z * value.z);

    // 水平成分がほぼゼロなら、固定の+Z方向を返す。
    if (length <= 0.001f) {
        return {0.0f, 0.0f, 1.0f};
    }

    return {value.x / length, 0.0f, value.z / length};
}

engine::Object3D* FindEnemyDamageTarget(engine::Object3D* object) {
    // 子メッシュとの接触を本体の敵へたどり、被弾対象を統一する。
    for (engine::Object3D* current = object; current; current = current->GetParent()) {
        // 最初に見つかった敵の本体を、子に代わる命中対象として返す。
        if (dynamic_cast<BaseEnemy*>(current)) {
            return current;
        }
    }

    return nullptr;
}
} // namespace

// 本体が所有する常時オーラはメンバーの破棄時に解放する。
EnemyThunderSlime::~EnemyThunderSlime() = default;

void EnemyThunderSlime::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    // 常時オーラと地上時の球判定を、敵本体の初期化後に結び付ける。
    BaseEnemy::Initialize(common, modelName);
    SetName("Enemy_ThunderSlime");
    SetEnemyType("ThunderSlime");
    ReloadAttackProfile();
    SetColor({1.0f, 1.0f, 1.0f, 1.0f});
    SetDefaultColor(GetColor());

    SetCollisionAttribute(engine::collisionCategories::Get("enemy"));
    SetCollisionMask(engine::collisionCategories::Get("player") | engine::collisionCategories::Get("ground") | engine::collisionCategories::Get("player_attack") | engine::collisionCategories::Get("player_bullet"));
    SetColliderType(engine::ColliderType::kSphere);
    SyncGroundCollisionRadius();
    InitializeAuraEffect();
}

// 近距離の放電と中距離の落雷を選び、共通の移動後にオーラを追従させる。
void EnemyThunderSlime::Update(float deltaTime) {
    // 通常の行動を進めない状態を先に処理する。
    if (UpdateInactiveState(deltaTime)) {
        return;
    }

    // 配置時の大きさを姿勢計算の基準にし、攻撃の待ち時間と反動の残り時間を先に進める。
    EnsureBaseScale();
    UpdateWildTimers(deltaTime);

    engine::Vector3 velocity = GetVelocity();
    velocity.x = 0.0f;
    velocity.z = 0.0f;

    UpdateWildBehavior(deltaTime, velocity);
    ApplyGroundMovementAndAnimation(deltaTime, velocity);
    // 行動・姿勢を決めた後に共通物理を進め、移動後の本体にオーラを合わせる。
    BaseEnemy::Update(deltaTime);
    UpdateAuraEffect(deltaTime);
}

bool EnemyThunderSlime::UpdateInactiveState(float deltaTime) {
    // 本体が消えた場合は予兆・連続落雷・常時オーラをまとめて停止する。
    if (IsDead() || !GetIsVisible()) {
        HideAttackTelegraph();
        isCharging_ = false;
        chargeWarningTriggered_ = false;
        chargeTimer_ = 0.0f;
        chargeParticleTimer_ = 0.0f;
        ResetWildLightning();
        shockSquashTimer_ = 0.0f;
        idleSparkTimer_ = kIdleSparkInterval;
        HideAuraEffect();
        SetVelocity({0.0f, 0.0f, 0.0f});
        return true;
    }

    // 撃破中は雷の表示を外し、共通の撃破演出だけ進める。
    if (ShouldHandleDefeatEffect()) {
        HideAttackTelegraph();
        HideAuraEffect();
        BaseEnemy::Update(deltaTime);
        return true;
    }

    // 通常の本体が残っている間は、待機火花の時計を運搬や投擲の判定より先に進める。
    idleTimer_ += deltaTime;
    UpdateIdleSpark(deltaTime);

    // 運ばれている間は通常AIを止めるが、オーラは移動した本体へ追従させる。
    if (IsCarried()) {
        HideAttackTelegraph();
        UpdateAuraEffect(deltaTime);
        return true;
    }

    return UpdateThrowRecoveryState(deltaTime);
}

bool EnemyThunderSlime::UpdateThrowRecoveryState(float deltaTime) {
    // 投げられた間は判定半径を広げ、通常姿勢へ戻る段階で地上半径に戻す。
    // 復帰中でなければ、呼び出し元で通常の行動を進められるようにする。
    if (!IsThrowRecovering()) {
        return false;
    }

    // 投擲物として動いている間だけ広い球を使う。
    if (IsThrownPhysics()) {
        SyncThrownCollisionRadius();
    // 投擲の物理挙動が終わり姿勢を戻す間は、地上用の半径を使う。
    } else {
        SyncGroundCollisionRadius();
    }

    BaseEnemy::Update(deltaTime);
    UpdateAuraEffect(deltaTime);
    return true;
}

void EnemyThunderSlime::EnsureBaseScale() {
    // 配置スケールを攻撃中の変形基準として一度だけ保存する。
    if (!hasBaseScale_) {
        baseScale_ = GetScale();
        hasBaseScale_ = true;
    }
}

void EnemyThunderSlime::UpdateWildTimers(float deltaTime) {
    // 攻撃後のつぶれと次の攻撃までの待ち時間を独立して進める。
    shockSquashTimer_ = (std::max)(0.0f, shockSquashTimer_ - deltaTime);
    attackCooldown_ = (std::max)(0.0f, attackCooldown_ - deltaTime);
}

void EnemyThunderSlime::UpdateWildBehavior(float deltaTime, engine::Vector3& velocity) {
    // 連続落雷中は新しい攻撃を選ばず、固定した方向への落雷を完了させる。
    if (wildLightningState_ != WildLightningState::kIdle) {
        UpdateFacing(wildLightningDirection_);
        UpdateWildLightning(deltaTime);
        return;
    }

    // 標的と固有パラメーターを参照できる場合だけ、距離で次の攻撃を選ぶ。
    if (GetTarget() && GetParameters().has_value()) {
        engine::Vector3 toTarget = GetTarget()->GetTranslate() - GetTranslate();
        engine::Vector3 direction = NormalizePlanar(toTarget);
        const float distance = std::sqrt(toTarget.x * toTarget.x + toTarget.z * toTarget.z);

        UpdateFacing(direction);
        UpdateCombatBehavior(deltaTime, velocity, direction, distance);
    }

    // 放電の溜めも落雷も行っていない場合は、前の予兆を残さない。
    if (!isCharging_ && wildLightningState_ == WildLightningState::kIdle) {
        HideAttackTelegraph();
    }
}

void EnemyThunderSlime::UpdateCombatBehavior(float deltaTime, engine::Vector3& velocity, const engine::Vector3& direction,
                                             float distance) {
    // 近距離放電・中距離落雷・追跡・巡回を射程と待機時間で振り分ける。
    const EnemyAttackDefinition& attack = GetAttackDefinition(kShockAttackId);
    const EnemyAttackDefinition& lineAttack = GetAttackDefinition(kWildLineAttackId);
    const bool forceShockPreview = debugPreviewAttackId_ == kShockAttackId;
    const bool forceLinePreview = debugPreviewAttackId_ == kWildLineAttackId;

    // 放電を溜めていないときに気付き演出が始まれば、向きだけ合わせて今回の攻撃選択を中止する。
    if (!isCharging_ && UpdateNoticeReaction(deltaTime, distance, GetDetectionRange(), direction)) {
        velocity.x = 0.0f;
        velocity.z = 0.0f;
        UpdateFacing(direction);
        return;
    }

    // 放電を溜めている間は、新しい行動を選ばず同じ攻撃を進める。
    if (isCharging_) {
        UpdateCloseRangeShock(deltaTime, direction);
    // 落雷専用のプレビューを除き、放電の最小・最大射程内で再使用可能なら近距離放電を選ぶ。
    } else if (!forceLinePreview && distance >= attack.minRange && distance <= attack.maxRange &&
               attackCooldown_ <= 0.0f) {
        lastShockDirection_ = direction;
        StartCloseRangeShock();
    // 放電専用のプレビューを除き、落雷の射程内で再使用可能なら連続落雷を選ぶ。
    } else if (!forceShockPreview && distance >= lineAttack.minRange && distance <= lineAttack.maxRange &&
               attackCooldown_ <= 0.0f) {
        StartWildLightning(direction);
    // どの技も始められなくても、標的が探知圏内なら方向へ移動する。
    } else if (distance <= GetDetectionRange()) {
        const float speed = (std::max)(0.0f, GetParameters()->speed) * kMoveSpeedScale;
        velocity.x = direction.x * speed;
        velocity.z = direction.z * speed;
    // 探知圏を外れた場合は、標的を追わず通常の徘徊にする。
    } else {
        UpdateWanderBehavior(deltaTime, velocity);
    }
}

void EnemyThunderSlime::SetDebugPreviewAttackId(const std::string& attackId) {
    // プレビューの選択変更時は進行中の攻撃を破棄し、指定技をすぐ確認できる状態にする。
    debugPreviewAttackId_ = attackId;
    attackCooldown_ = 0.0f;
    isCharging_ = false;
    chargeTimer_ = 0.0f;
    ResetWildLightning();
}

const char* EnemyThunderSlime::GetDebugAttackPhaseName() const {
    // 実際の攻撃状態をプレビュー表示用の短い日本語へ変換する。
    // 連続落雷の着弾が始まる前は、専用の溜めとして表示する。
    if (wildLightningState_ == WildLightningState::kCharging) {
        return "連続落雷・溜め";
    }

    // 着弾点を順番に処理する間は連続落雷の実行を表示する。
    if (wildLightningState_ == WildLightningState::kStriking) {
        return "連続落雷・落雷中";
    }

    // 近距離の電撃をまだ発生させていない間は、放電の溜めとして表示する。
    if (isCharging_) {
        return "近距離放電・溜め";
    }

    // 攻撃終了後の姿勢反応が残っている場合は放電後の反発を表示する。
    if (shockSquashTimer_ > 0.0f) {
        return "放電後・反発";
    }

    // 攻撃中でも反発中でもなく、再使用に時間が残るなら待ち時間と表示する。
    if (attackCooldown_ > 0.0f) {
        return "再使用待ち";
    }

    return "待機・移動";
}

// 中距離では、狙った方向へ予告を並べてから連続落雷を走らせます。
void EnemyThunderSlime::StartWildLightning(const engine::Vector3& direction) {
    // 射線上の着地点を先に固定し、予告と実際の雷が同じ地面を指すようにする。
    const EnemyAttackDefinition& attack = GetAttackDefinition(kWildLineAttackId);
    wildLightningState_ = WildLightningState::kCharging;
    wildLightningDirection_ = NormalizePlanar(direction);
    wildLightningTimer_ = (std::max)(0.12f, attack.windupDuration);
    wildLightningEffectTimer_ = 0.0f;
    wildStrikeIndex_ = 0;
    wildLightningHitTarget_ = false;
    chargeWarningTriggered_ = false;
    attackCooldown_ = (std::max)(0.1f, attack.cooldown);
    HideAttackTelegraph();

    const engine::Vector3 origin = GetWorldPosition();
    MeshEffectManager* meshEffects = MeshEffectManager::GetInstance();

    // 発生順に候補位置を並べ、各地点で地面を探して予告と着弾の共通座標にする。
    for (std::size_t index = 0; index < wildStrikePositions_.size(); ++index) {
        const float distance = kWildStrikeStartDistance + kWildStrikeSpacing * static_cast<float>(index);
        const engine::Vector3 samplePosition = origin + wildLightningDirection_ * distance;
        wildStrikePositions_[index] = FindStrikeGround(samplePosition, this, origin.y);

        // メッシュ演出が使える場合だけ、確定した着弾位置に予告を重ねる。
        if (meshEffects) {
            engine::Vector3 warningPosition = wildStrikePositions_[index];
            warningPosition.y += 0.045f;
            meshEffects->SpawnEffectAt(kLightningWarningEffectPath, warningPosition, {0.0f, 0.0f, 0.0f});
        }
    }

    // 射線上の着弾予告とは別に、雷を溜める本体の足元にも演出を出す。
    const engine::Vector3 chargeGround = FindStrikeGround(origin, this, origin.y);
    SpawnChargeGroundEffect(chargeGround, 1.12f);
    EmitLineChargeEffect(attack.windupVfx.empty() ? kLineChargePreset : attack.windupVfx.c_str(), 0.0f);
}

void EnemyThunderSlime::UpdateWildLightning(float deltaTime) {
    // 溜め中は全着弾点を予告し、放出後は未発生の地点だけを順番に残す。
    // 落雷を開始していない場合は、予定時刻も発生地点も進めない。
    if (wildLightningState_ == WildLightningState::kIdle) {
        return;
    }

    // 着弾の時計と粒子の時計を別々に進め、溜めの表示頻度が発生間隔に影響しないようにする。
    const EnemyAttackDefinition& attack = GetAttackDefinition(kWildLineAttackId);
    wildLightningTimer_ -= deltaTime;
    wildLightningEffectTimer_ -= deltaTime;

    // 溜め中は全着弾点の円を表示し、着弾を開始する条件だけを判定する。
    if (wildLightningState_ == WildLightningState::kCharging) {
        const float windupDuration = (std::max)(0.01f, attack.windupDuration);
        const float progress = 1.0f - std::clamp(wildLightningTimer_ / windupDuration, 0.0f, 1.0f);
        ShowAttackTelegraphImpactAreas(wildStrikePositions_.data(), wildStrikePositions_.size(), attack.radius,
                                       progress, {1.0f, 0.94f, 0.16f, 0.84f});
        // 攻撃設定の溜め素材を優先し、完了が近づくほど発生間隔を詰める。
        // 粒子の残り時間が切れたら、溜めの進み具合で次回の発生間隔を設定する。
        if (wildLightningEffectTimer_ <= 0.0f) {
            EmitLineChargeEffect(attack.windupVfx.empty() ? kLineChargePreset : attack.windupVfx.c_str(), progress);
            wildLightningEffectTimer_ = 0.072f - progress * 0.030f;
        }

        // 落雷に近い時刻で全地点の予告を一度だけ強調する。
        if (!chargeWarningTriggered_ && wildLightningTimer_ <= attack.warningLeadTime) {
            TriggerAttackTelegraphCue({0.92f, 0.98f, 0.20f, 1.0f});
            chargeWarningTriggered_ = true;
        }

        // 溜めの最後に状態だけ切り替え、次の更新で最初の着弾を出す。
        if (wildLightningTimer_ <= 0.0f) {
            wildLightningState_ = WildLightningState::kStriking;
            wildLightningTimer_ = 0.0f;
        }

        return;
    }

    // 複数の予定時刻を跨いだときも、用意した着弾先を飛ばさず処理する。
    while (wildLightningTimer_ <= 0.0f && wildStrikeIndex_ < static_cast<std::int32_t>(wildStrikePositions_.size())) {
        SpawnWildLightningStrike(wildStrikePositions_[wildStrikeIndex_]);
        ++wildStrikeIndex_;
        wildLightningTimer_ += kWildStrikeInterval;
    }

    // 発生済みの地点を予兆から外し、残っている地点だけ次の落雷間隔に合わせて表示する。
    if (wildStrikeIndex_ < static_cast<std::int32_t>(wildStrikePositions_.size())) {
        const std::size_t remainingCount = wildStrikePositions_.size() - static_cast<std::size_t>(wildStrikeIndex_);
        const float strikeProgress = 1.0f - std::clamp(wildLightningTimer_ / kWildStrikeInterval, 0.0f, 1.0f);
        ShowAttackTelegraphImpactAreas(wildStrikePositions_.data() + wildStrikeIndex_, remainingCount, attack.radius,
                                       strikeProgress, {1.0f, 0.94f, 0.16f, 0.76f});
    }

    // 最後の落雷後に表示・命中履歴を片付け、次の行動で前回の状態を引き継がないようにする。
    if (wildStrikeIndex_ >= static_cast<std::int32_t>(wildStrikePositions_.size())) {
        HideAttackTelegraph();
        wildLightningState_ = WildLightningState::kIdle;
        wildLightningTimer_ = 0.0f;
        wildLightningEffectTimer_ = 0.0f;
        wildStrikeIndex_ = 0;
        wildLightningHitTarget_ = false;
        shockSquashTimer_ = (std::max)(shockSquashTimer_, attack.recoveryDuration);
    }
}

void EnemyThunderSlime::SpawnWildLightningStrike(const engine::Vector3& groundPosition) {
    // 一つの着弾点で表示とダメージ判定を同時に発生させる。
    SpawnLightningStrikeVisual(groundPosition, wildLightningDirection_);
    DispatchWildLightningDamage(groundPosition);
}

void EnemyThunderSlime::DispatchWildLightningDamage(const engine::Vector3& groundPosition) {
    // 一連の落雷で同じ対象を二度傷つけず、水平半径と高さを別々に判定する。
    // 現在の標的がいないか、この連続落雷ですでに命中を通知した場合は判定しない。
    if (!GetTarget() || wildLightningHitTarget_) {
        return;
    }

    const EnemyAttackDefinition& attack = GetAttackDefinition(kWildLineAttackId);
    const engine::Vector3 targetPosition = GetTarget()->GetWorldPosition();
    const engine::Vector3 horizontal = {
        targetPosition.x - groundPosition.x,
        0.0f,
        targetPosition.z - groundPosition.z,
    };

    const float verticalDistance = targetPosition.y - groundPosition.y;
    // 地上の円内でも雷の高さから外れていれば命中させず、水平距離と高さを独立して調べる。
    if (engine::Math::Length(horizontal) > attack.radius || verticalDistance < -0.65f ||
        verticalDistance > kLightningStrikeHeight) {
        return;
    }

    // 落雷の進行方向へ押し出す速度を付け、電気ダメージとして共通の命中処理へ渡す。
    DamageEvent damageEvent;
    damageEvent.target = GetTarget();
    damageEvent.attacker = this;
    damageEvent.damageAmount = attack.damage;
    damageEvent.damageType = DamageType::kElectric;
    damageEvent.knockbackVelocity = {
        wildLightningDirection_.x * 7.5f,
        6.5f,
        wildLightningDirection_.z * 7.5f,
    };

    // 命中イベントを共通処理へ送り、同じ連続落雷での再通知を防ぐ。
    EventManager::GetInstance()->Dispatch(damageEvent);
    wildLightningHitTarget_ = true;
}

void EnemyThunderSlime::ResetWildLightning() {
    // 連続落雷を待機状態に戻し、予約時刻、着弾順、命中済みの記録を消す。
    wildLightningState_ = WildLightningState::kIdle;
    wildLightningTimer_ = 0.0f;
    wildLightningEffectTimer_ = 0.0f;
    wildStrikeIndex_ = 0;
    wildLightningHitTarget_ = false;
}

void EnemyThunderSlime::UpdateWanderBehavior(float deltaTime, engine::Vector3& velocity) {
    // 検知範囲外では共通巡回速度を使い、移動方向へモデルを向ける。
    const float speed = (std::max)(0.65f, GetParameters()->speed * 0.48f);
    velocity = CalculateWanderVelocity(deltaTime, speed, 0.68f);
    UpdateFacing({velocity.x, 0.0f, velocity.z});
}

void EnemyThunderSlime::ApplyGroundMovementAndAnimation(float deltaTime, engine::Vector3& velocity) {
    // 詠唱中は足踏みを止め、移動中だけ跳ねる速度と地上判定半径を反映する。
    velocity.y = (std::min)(GetVelocity().y, 0.0f);

    // 近距離放電の溜めか連続落雷中は、水平移動と小跳びのタイマーを止める。
    if (isCharging_ || wildLightningState_ != WildLightningState::kIdle) {
        velocity.x = 0.0f;
        velocity.z = 0.0f;
        groundHopTimer_ = 0.0f;
    // 攻撃を続けていない場合に、接地・移動と時間の条件を満たした小跳びを適用する。
    } else if (slimeBounceAnimator::StepGroundHop(groundHopTimer_, velocity, IsGrounded(), deltaTime, kMoveHopInterval,
                                                  0.10f)) {
        velocity.y = (std::max)(velocity.y, kMoveHopPower);
    }

    SetVelocity(velocity);
    ApplySlimeAnimation(deltaTime);
    SyncGroundCollisionRadius();
}

void EnemyThunderSlime::Draw(ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource) {
    // オーラは本体と別のエフェクトなので可視時だけ追加描画する。
    BaseEnemy::Draw(pointLightResource, spotLightResource);

    // オーラを作成済みで表示も有効な場合だけ、本体に重ねる。
    if (auraEffect_ && auraEffect_->GetIsVisible()) {
        auraEffect_->Draw(pointLightResource, spotLightResource);
    }
}

std::unique_ptr<engine::Object3D> EnemyThunderSlime::Clone() const {
    // シーン上の複製では固有エフェクトを再初期化し、配置設定と対象を引き継ぐ。
    auto clone = std::make_unique<EnemyThunderSlime>();
    assert(GetObjectCommon() != nullptr);
    clone->Initialize(GetObjectCommon(), GetModelName());
    clone->CopyFrom(this);
    clone->SetTarget(GetTarget());
    clone->SetDetectionRange(GetDetectionRange());
    return clone;
}

void EnemyThunderSlime::BeginThrown(const engine::Vector3& initialVelocity) {
    // 雷攻撃を中断して予兆を消し、投擲中だけ広い接触半径に切り替える。
    // 投げられるまで大きさを記録していなければ、この時点の基準を確定する。
    if (!hasBaseScale_) {
        baseScale_ = GetScale();
        const float capturedMaxScale =
            (std::max)({std::abs(baseScale_.x), std::abs(baseScale_.y), std::abs(baseScale_.z)});
        // 配置のすべての拡縮成分が絶対値で1.2未満なら、投擲にも使える既定寸法を採用する。
        if (capturedMaxScale < 1.2f) {
            baseScale_ = {2.0f, 2.0f, 2.0f};
        }

        hasBaseScale_ = true;
    }

    // 攻撃のタイマーと予兆を消し、投げる前の変形を基準サイズへ戻す。
    isCharging_ = false;
    chargeWarningTriggered_ = false;
    chargeTimer_ = 0.0f;
    chargeParticleTimer_ = 0.0f;
    shockSquashTimer_ = 0.0f;
    ResetWildLightning();
    HideAttackTelegraph();
    SetScale(baseScale_);
    SyncThrownCollisionRadius();
    BaseEnemy::BeginThrown(initialVelocity);
}

engine::Vector3 EnemyThunderSlime::FindStrikeGround(const engine::Vector3& samplePosition, engine::Object3D* ignoredObject,
                                            float fallbackHeight) const {
    // 上から地形を探して雷を接地させ、地形がなければ指定高さを使う。
    engine::Vector3 rayStart = samplePosition;
    rayStart.y += 10.0f;

    engine::PhysicsQueryFilter filter;
    filter.mask = engine::collisionCategories::Get("ground");
    filter.ignoredObject = ignoredObject;
    const engine::RaycastHit hit = engine::CollisionManager::GetInstance()->Raycast(rayStart, {0.0f, -1.0f, 0.0f}, 22.0f, filter);

    // 床に当たった場合は実際の接点を返し、床を見つけられない場合は指定のYを使う。
    if (hit.isHit) {
        return hit.hitPoint;
    }

    engine::Vector3 fallback = samplePosition;
    fallback.y = fallbackHeight;
    return fallback;
}

void EnemyThunderSlime::SpawnLightningStrikeVisual(const engine::Vector3& groundPosition, const engine::Vector3& direction) {
    // 柱状の雷は着地点を基準に上へ伸ばし、床の閃光と焦げ跡は地面へ置く。
    engine::Vector3 strikeCenter = groundPosition;
    strikeCenter.y += kLightningStrikeHeight * 0.5f;
    const float strikeYaw = std::atan2(direction.x, direction.z);

    // メッシュ演出が使える場合は、雷の柱・枝・中心を同じ着弾点に揃える。
    if (MeshEffectManager* meshEffects = MeshEffectManager::GetInstance()) {
        meshEffects->SpawnEffectAt(kLightningBoltEffectPath, strikeCenter, {kHalfPi, strikeYaw, 0.0f});
        meshEffects->SpawnEffectAt(kLightningBoltEffectPath, strikeCenter, {kHalfPi, strikeYaw + kHalfPi, 0.0f},
                                   {0.82f, 1.0f, 0.94f});
        // 中心の稲妻に細い枝を左右へ足し、着弾位置の真上に電光を集める。
        const engine::Vector3 branchSide = {direction.z, 0.0f, -direction.x};
        meshEffects->SpawnEffectAt(kLightningBoltEffectPath, strikeCenter + branchSide * 0.34f + direction * 0.18f,
                                   {kHalfPi, strikeYaw + 0.31f, 0.0f}, {0.46f, 1.0f, 0.76f});
        meshEffects->SpawnEffectAt(kLightningBoltEffectPath, strikeCenter - branchSide * 0.30f - direction * 0.14f,
                                   {kHalfPi, strikeYaw - 0.27f, 0.0f}, {0.38f, 1.0f, 0.68f});
        meshEffects->SpawnEffectAt(kLightningCoreEffectPath, strikeCenter, {0.0f, 0.0f, 0.0f});

        // 地面すれすれに閃光を重ね、余韻の焦げ跡を同じ地点へ残す。
        engine::Vector3 impactPosition = groundPosition;
        impactPosition.y += 0.055f;
        meshEffects->SpawnEffectAt(kLightningImpactEffectPath, impactPosition, {0.0f, 0.0f, 0.0f});
        meshEffects->SpawnEffectAt(kLightningImpactEffectPath, impactPosition + engine::Vector3{0.0f, 0.025f, 0.0f},
                                   {0.0f, strikeYaw + 0.35f, 0.0f}, {1.32f, 1.0f, 1.32f});
        meshEffects->SpawnEffectAt(kScorchMarkEffectPath, impactPosition + engine::Vector3{0.0f, 0.012f, 0.0f},
                                   {0.0f, strikeYaw, 0.0f});
    }

    // メッシュが利用できない場合も、GPU粒子の着弾演出は発行する。
    engine::Vector3 particlePosition = groundPosition;
    particlePosition.y += 0.22f;
    EmitThunderPreset(kLightningStrikeParticlePreset, particlePosition);
    EmitThunderPreset(kLightningStrikeParticlePreset, particlePosition + engine::Vector3{0.22f, 0.08f, -0.18f});
    EmitThunderPreset(kDischargePreset, particlePosition);
}

void EnemyThunderSlime::SpawnChargeGroundEffect(const engine::Vector3& groundPosition, float scale) {
    // 地面との重なりを避ける小さな高さ補正を加えて詠唱模様を置く。
    // 描画管理が使える場合だけ、接点よりわずかに浮かせて模様を置く。
    if (MeshEffectManager* meshEffects = MeshEffectManager::GetInstance()) {
        engine::Vector3 effectPosition = groundPosition;
        effectPosition.y += 0.035f;
        meshEffects->SpawnEffectAt(kChargeGroundEffectPath, effectPosition, {0.0f, idleTimer_ * 0.8f, 0.0f},
                                   {scale, 1.0f, scale});
    }
}

void EnemyThunderSlime::UpdateFacing(const engine::Vector3& direction) {
    // モデルの正面補正を加え、最短角で標的へ向ける。
    const float lengthSq = direction.x * direction.x + direction.z * direction.z;
    // 水平成分の長さが十分でないときは現在の向きを維持する。
    if (lengthSq <= 0.0001f)
        return;
    const float targetYaw = std::atan2(direction.x, direction.z) + kThunderSlimeModelYawOffset;
    SetRotationY(engine::Math::LerpShortAngle(GetRotation().y, targetYaw, 0.16f));
}

void EnemyThunderSlime::StartCloseRangeShock() {
    // 近距離放電の予備動作を始め、地面の予告と外周の電弧を同じ時刻に出す。
    // まだ未設定の場合だけ、溜めで変形させる前に基準の大きさを保持する。
    if (!hasBaseScale_) {
        baseScale_ = GetScale();
        hasBaseScale_ = true;
    }

    isCharging_ = true;
    chargeWarningTriggered_ = false;
    const EnemyAttackDefinition& attack = GetAttackDefinition(kShockAttackId);
    chargeTimer_ = attack.windupDuration;
    chargeParticleTimer_ = 0.0f;
    attackCooldown_ = attack.cooldown;
    const engine::Vector3 origin = GetWorldPosition();
    SpawnChargeGroundEffect(FindStrikeGround(origin, this, origin.y), 1.18f);
    EmitOuterThunderEffect(attack.windupVfx.empty() ? kRadialChargePreset : attack.windupVfx.c_str(), 2, 0.18f);
    SyncGroundCollisionRadius();
}

void EnemyThunderSlime::UpdateCloseRangeShock(float deltaTime, const engine::Vector3& direction) {
    // 残り時間で円形予兆と電弧を強め、終了時に一度だけ放電する。
    const EnemyAttackDefinition& attack = GetAttackDefinition(kShockAttackId);
    lastShockDirection_ = direction;
    chargeTimer_ = (std::max)(0.0f, chargeTimer_ - deltaTime);
    chargeParticleTimer_ -= deltaTime;
    const float windupDuration = (std::max)(0.01f, attack.windupDuration);
    const float progress = 1.0f - (std::clamp)(chargeTimer_ / windupDuration, 0.0f, 1.0f);
    ShowAttackTelegraphCircle(GetTranslate(), attack.radius, progress, {1.0f, 0.95f, 0.16f, 0.82f});

    // 溜めが進むほど電弧を増やし、発生間隔を短くする。
    if (chargeParticleTimer_ <= 0.0f) {
        const std::int32_t arcCount = progress >= 0.68f ? 3 : 2;
        EmitOuterThunderEffect(attack.windupVfx.empty() ? kRadialChargePreset : attack.windupVfx.c_str(), arcCount,
                               0.55f + progress * 1.4f);
        chargeParticleTimer_ = 0.070f - progress * 0.032f;
    }

    // 発動直前の強調は通常の電弧と別に、一撃につき一度だけ出す。
    if (!chargeWarningTriggered_ && chargeTimer_ <= attack.warningLeadTime) {
        TriggerAttackTelegraphCue({1.0f, 0.92f, 0.12f, 1.0f});
        chargeWarningTriggered_ = true;
    }

    // 溜め切ったフレームで範囲判定と放電演出へ進む。
    if (chargeTimer_ <= 0.0f) {
        ReleaseShock(lastShockDirection_);
    }
}

void EnemyThunderSlime::ReleaseShock(const engine::Vector3& direction) {
    // 放電時に予兆を消し、範囲内ダメージと反動姿勢を開始する。
    const EnemyAttackDefinition& attack = GetAttackDefinition(kShockAttackId);
    isCharging_ = false;

    // 直前の予告強調を出していない場合は、発動時に一度だけ補う。
    if (!chargeWarningTriggered_) {
        TriggerAttackTelegraphCue({1.0f, 0.92f, 0.12f, 1.0f});
        chargeWarningTriggered_ = true;
    }

    // 床の予兆を終えた時点から反動を始め、接触半径も地上用に揃える。
    HideAttackTelegraph();
    shockSquashTimer_ = attack.recoveryDuration;
    SyncGroundCollisionRadius();

    // 放電の中心を本体の底面より上に置き、先に周囲へ電弧を出す。
    engine::Vector3 center = GetTranslate();
    center.y += (std::max)(0.5f, baseScale_.y * 0.22f);
    EmitOuterThunderEffect(attack.activeVfx.empty() ? kDischargePreset : attack.activeVfx.c_str(), 5, 0.35f);

    // メッシュ演出を生成できる場合は、地面の接点へ焦げ跡を残す。
    if (MeshEffectManager* meshEffects = MeshEffectManager::GetInstance()) {
        const engine::Vector3 origin = GetWorldPosition();
        engine::Vector3 scorchPosition = FindStrikeGround(origin, this, origin.y);
        scorchPosition.y += 0.045f;
        meshEffects->SpawnEffectAt(kScorchMarkEffectPath, scorchPosition, {0.0f, GetRotation().y, 0.0f},
                                   {1.25f, 1.0f, 1.25f});
    }

    DispatchShockDamage(center, direction, attack.radius, attack.damage);
}

void EnemyThunderSlime::DispatchShockDamage(const engine::Vector3& center, const engine::Vector3& direction, float radius,
                                            float damage) {
    // 高さ方向の差を軽く見て球状範囲を判定し、中心に近いほど押し出しを強める。
    // 標的がいないときは円の距離計算とダメージ通知を省く。
    if (!GetTarget()) {
        return;
    }

    engine::Vector3 diff = GetTarget()->GetTranslate() - center;
    diff.y *= 0.45f;
    const float distance = engine::Math::Length(diff);

    // 高さを弱めた距離でも半径を超える対象には命中を通知しない。
    if (distance > radius) {
        return;
    }

    const float distanceRate = 1.0f - (std::clamp)(distance / radius, 0.0f, 1.0f);
    DamageEvent damageEvent;
    damageEvent.target = GetTarget();
    damageEvent.attacker = this;
    damageEvent.damageAmount = damage;
    damageEvent.damageType = DamageType::kElectric;
    damageEvent.knockbackVelocity = {direction.x * (8.0f + distanceRate * 5.0f), 5.8f + distanceRate * 2.0f,
                                     direction.z * (8.0f + distanceRate * 5.0f)};
    EventManager::GetInstance()->Dispatch(damageEvent);
}

void EnemyThunderSlime::UpdateIdleSpark(float deltaTime) {
    // 攻撃の溜め中は専用電弧だけを使い、待機火花との二重発生で輪郭が潰れるのを防ぎます。
    if (isCharging_ || wildLightningState_ == WildLightningState::kCharging) {
        idleSparkTimer_ = kIdleSparkInterval;
        return;
    }

    idleSparkTimer_ -= deltaTime;

    // 前回の火花から発生間隔を使い切るまでは、新しい火花を出さない。
    if (idleSparkTimer_ > 0.0f) {
        return;
    }

    EmitOuterThunderEffect(kIdleSparkPreset, isCharging_ ? 3 : 2);

    // 放電の溜めでは冒頭で戻るため、ここまで進んだ場合は運搬中か通常時の間隔が選ばれる。
    if (isCharging_) {
        idleSparkTimer_ = kIdleSparkInterval * 0.62f;
    // 運搬中は通常時より短い発生間隔を設定する。
    } else if (IsCarried()) {
        idleSparkTimer_ = kIdleSparkInterval * 0.76f;
    // 通常時は基準の発生間隔へ戻す。
    } else {
        idleSparkTimer_ = kIdleSparkInterval;
    }
}

void EnemyThunderSlime::InitializeAuraEffect() {
    // 常時オーラを表示専用として用意し、JSON読込失敗時は本体の動作を続ける。
    if (!GetObjectCommon() || auraEffect_) {
        return;
    }

    auraEffect_ = std::make_unique<EffectObject3D>();
    auraEffect_->Initialize(GetObjectCommon());
    auraEffect_->SetName("ThunderSlime_RoundAura");

    // 素材の読み込みに失敗したら不完全な表示物を破棄し、本体は動作させる。
    if (!auraEffect_->LoadFromJson(kConstantAuraEffectPath)) {
        auraEffect_.reset();
        return;
    }

    auraEffect_->SetIsVisible(false);
    auraEffect_->Play(99999.0f);
}

void EnemyThunderSlime::UpdateAuraEffect(float deltaTime) {
    // 本体の大きさに合わせてオーラを変形し、カメラへ向く角度を毎フレーム決める。
    // まだオーラを持たない場合は、今回の更新で素材の準備を試す。
    if (!auraEffect_) {
        InitializeAuraEffect();
    }

    // 再試行後もオーラがなければ、表示物の更新は省く。
    if (!auraEffect_) {
        return;
    }

    // 基準の大きさが未設定なら、本体の現在の大きさから確定する。
    if (!hasBaseScale_) {
        baseScale_ = GetScale();
        const float capturedMaxScale =
            (std::max)({std::abs(baseScale_.x), std::abs(baseScale_.y), std::abs(baseScale_.z)});
        // 現在の倍率の最大成分が1.2未満の個体には、基準寸法2を補う。
        if (capturedMaxScale < 1.2f) {
            baseScale_ = {2.0f, 2.0f, 2.0f};
        }

        hasBaseScale_ = true;
    }

    engine::Vector3 auraPos{};
    float horizontalDiameter = 1.0f;
    float verticalDiameter = 1.0f;
    CalculateAuraShape(auraPos, horizontalDiameter, verticalDiameter);

    // 水平面でだけカメラへ向け、正面を求められない場合は本体の向きを使う。
    float yaw = GetRotation().y;
    engine::Camera* camera = CameraManager::GetInstance() ? CameraManager::GetInstance()->GetActiveCamera() : nullptr;

    // 描画カメラを取得できれば、その視点への水平方向を求める。
    if (camera) {
        const engine::Vector3 toCamera = camera->GetEye() - auraPos;

        // カメラとのXZの成分差が十分なときだけ、板のYawを視点に向け直す。
        if (std::abs(toCamera.x) + std::abs(toCamera.z) > 0.001f) {
            yaw = std::atan2(toCamera.x, toCamera.z);
        }
    }

    // 本体の座標・サイズを表示用オーラへ反映し、そのフレームの描画行列を更新する。
    auraEffect_->SetIsVisible(true);
    auraEffect_->Update(deltaTime);
    auraEffect_->SetTranslate(auraPos);
    auraEffect_->SetRotation({(std::numbers::pi_v<float> * 0.5f), yaw, 0.0f});
    auraEffect_->SetScale({horizontalDiameter, 1.0f, verticalDiameter});
    auraEffect_->UpdateLocalMatrix();
    auraEffect_->UpdateWorldMatrix();
}

void EnemyThunderSlime::HideAuraEffect() {
    // 撃破や非表示で本体が消えた後にオーラだけ残るのを防ぐ。
    if (auraEffect_) {
        auraEffect_->SetIsVisible(false);
    }
}

void EnemyThunderSlime::CalculateAuraShape(engine::Vector3& center, float& horizontalDiameter, float& verticalDiameter) const {
    // 横幅と高さを本体スケールから別々に求め、つぶれた姿勢にも追従する。
    const engine::Vector3 scale = GetScale();
    const float maxXZ = (std::max)({std::abs(scale.x), std::abs(scale.z), 1.0f});
    const float yScale = (std::max)(std::abs(scale.y), 0.18f);

    horizontalDiameter = (std::max)(2.55f, maxXZ * 1.35f);
    verticalDiameter = (std::max)(0.46f, yScale * 1.28f);

    center = GetWorldPosition();
    center.y += (std::max)(0.22f, yScale * 0.43f);
}

void EnemyThunderSlime::EmitOuterThunderEffect(const char* presetName, std::int32_t count, float phaseOffset) {
    // 電弧を楕円周上に分散し、個数と位相で待機・詠唱・放電を使い分ける。
    // 発生する個数が0以下なら、周の等間隔分割を行わない。
    if (count <= 0) {
        return;
    }

    engine::Vector3 center{};
    float horizontalDiameter = 1.0f;
    float verticalDiameter = 1.0f;
    CalculateAuraShape(center, horizontalDiameter, verticalDiameter);

    const float horizontalRadius = horizontalDiameter * 0.52f;
    const float verticalRadius = verticalDiameter * 0.38f;
    const float phase = idleTimer_ * 3.4f + phaseOffset;

    for (std::int32_t i = 0; i < count; ++i) {
        const float angle = phase + (kTwoPi * static_cast<float>(i) / static_cast<float>(count));
        engine::Vector3 pos = center;
        pos.x += std::cos(angle) * horizontalRadius;
        pos.z += std::sin(angle) * horizontalRadius;
        pos.y += std::sin(angle * 1.37f) * verticalRadius;
        EmitThunderPreset(presetName, pos);
    }
}

// 中距離落雷では、外周の電弧を上方へ絞り込み、次に縦方向の雷が来ることを形で示します。
void EnemyThunderSlime::EmitLineChargeEffect(const char* presetName, float progress) {
    // 落雷の発生が近づくほど電弧を中心・上方へ集める。
    // 粒子の参照名が空の場合は、発生位置を計算せず終了する。
    if (!presetName || presetName[0] == '\0') {
        return;
    }

    engine::Vector3 center{};
    float horizontalDiameter = 1.0f;
    float verticalDiameter = 1.0f;
    CalculateAuraShape(center, horizontalDiameter, verticalDiameter);

    const float clampedProgress = std::clamp(progress, 0.0f, 1.0f);
    const float radius = horizontalDiameter * (0.42f - clampedProgress * 0.17f);
    const std::int32_t arcCount = clampedProgress >= 0.62f ? 3 : 2;
    const float phase = idleTimer_ * 4.8f + clampedProgress * 2.3f;

    for (std::int32_t index = 0; index < arcCount; ++index) {
        const float angle = phase + kTwoPi * static_cast<float>(index) / static_cast<float>(arcCount);
        engine::Vector3 position = center;
        position.x += std::cos(angle) * radius;
        position.z += std::sin(angle) * radius;
        position.y += verticalDiameter * (0.24f + 0.16f * std::sin(angle * 1.7f));
        position.y += 0.18f + clampedProgress * (0.32f + 0.16f * static_cast<float>(index));
        EmitThunderPreset(presetName, position);
    }

    // 溜めが74%以上に達した場合は、周囲の電弧に加えて頭上にも一つ出す。
    if (clampedProgress >= 0.74f) {
        engine::Vector3 crownPosition = center;
        crownPosition.y += verticalDiameter * 0.58f + 0.54f + clampedProgress * 0.42f;
        EmitThunderPreset(presetName, crownPosition);
    }
}

void EnemyThunderSlime::EmitThunderPreset(const char* presetName, const engine::Vector3& position) {
    // GPU粒子が未初期化のプレビューでも本体の攻撃は継続する。
    GpuParticleManager* particles = GpuParticleManager::GetInstance();

    // 管理オブジェクトが無いか初期化前の場合は、粒子の発生だけを省く。
    if (!particles || !particles->IsInitialized()) {
        return;
    }

    particles->Emit(presetName, position);
}

void EnemyThunderSlime::ApplySlimeAnimation(float deltaTime) {
    // 放電・落雷・反動で異なる目標姿勢を作り、通常時は跳ねる変形へ戻す。
    engine::Vector3 targetScale = baseScale_;
    engine::Vector3 targetRotation = {0.0f, GetRotation().y, 0.0f};

    if (isCharging_) {
        // 近距離放電は溜めが深くなるほど体をつぶし、細かい色と傾きの変化を重ねる。
        const float windupDuration = (std::max)(0.01f, GetAttackDefinition(kShockAttackId).windupDuration);
        const float t = std::clamp(1.0f - (chargeTimer_ / windupDuration), 0.0f, 1.0f);
        const float slam = Easing::SmoothStep(t);
        const float chargePulse = std::sin(t * kTwoPi * 3.0f) * (0.018f + slam * 0.035f);
        const float finalClamp = std::clamp((t - 0.76f) / 0.24f, 0.0f, 1.0f);
        targetScale.x = baseScale_.x * (1.0f + slam * 0.30f + finalClamp * 0.18f + chargePulse);
        targetScale.y = baseScale_.y * (1.0f - slam * 0.18f - finalClamp * 0.22f - std::abs(chargePulse) * 0.25f);
        targetScale.z = baseScale_.z * (1.0f + slam * 0.27f + finalClamp * 0.16f - chargePulse);
        targetRotation.x = chargePulse * 0.8f;
        targetRotation.z = std::sin(t * kTwoPi * 4.0f) * 0.035f * slam;
        SetColor({1.0f, 0.98f, 0.70f + std::sin(t * 58.0f) * 0.08f, 1.0f});
    } else if (wildLightningState_ != WildLightningState::kIdle) {
        // 連続落雷は発動前には残りの溜め時間、発動後には着弾済みの数を姿勢の進行率にする。
        const EnemyAttackDefinition& lineAttack = GetAttackDefinition(kWildLineAttackId);
        const float windupDuration = (std::max)(0.01f, lineAttack.windupDuration);
        const bool isWindup = wildLightningState_ == WildLightningState::kCharging;
        const float progress =
            isWindup
                ? std::clamp(1.0f - wildLightningTimer_ / windupDuration, 0.0f, 1.0f)
                : std::clamp(static_cast<float>(wildStrikeIndex_) / static_cast<float>(wildStrikePositions_.size()),
                             0.0f, 1.0f);
        const float pulse = std::sin(progress * kTwoPi * (isWindup ? 3.5f : 5.0f));
        const float gather = Easing::SmoothStep(progress);
        targetScale = {
            baseScale_.x * (1.0f + gather * 0.24f + pulse * 0.045f),
            baseScale_.y * (1.0f - gather * 0.17f - std::abs(pulse) * 0.025f),
            baseScale_.z * (1.0f + gather * 0.22f - pulse * 0.038f),
        };

        targetRotation.x = pulse * 0.045f;
        targetRotation.z = std::sin(progress * kTwoPi * 4.0f) * 0.055f * (0.35f + gather * 0.65f);
        SetColor({1.0f, 0.98f, 0.72f + std::abs(pulse) * 0.18f, 1.0f});
    } else if (shockSquashTimer_ > 0.0f) {
        // 放電の反動は押しつぶし、縦の伸び、小さな反発を経て基準サイズへ戻す。
        const float recoveryDuration = (std::max)(0.01f, GetAttackDefinition(kShockAttackId).recoveryDuration);
        const float remaining = (std::clamp)(shockSquashTimer_ / recoveryDuration, 0.0f, 1.0f);
        const float elapsed = 1.0f - remaining;
        engine::Vector3 multiplier = {1.0f, 1.0f, 1.0f};

        // 反動の序盤では、横につぶれた姿勢から一度縦に伸びる。
        if (elapsed < 0.28f) {
            const float phase = elapsed / 0.28f;
            const float eased = Easing::SmoothStep(phase);
            multiplier = {1.82f + (0.90f - 1.82f) * eased, 0.48f + (1.25f - 0.48f) * eased,
                          1.76f + (0.94f - 1.76f) * eased};
        // 中盤は反発で縦の伸びを戻し、横を少し広げる。
        } else if (elapsed < 0.64f) {
            const float phase = (elapsed - 0.28f) / 0.36f;
            const float eased = Easing::SmoothStep(phase);
            multiplier = {0.90f + (1.12f - 0.90f) * eased, 1.25f + (0.86f - 1.25f) * eased,
                          0.94f + (1.10f - 0.94f) * eased};
        // 反動の後半は全軸を基準の1倍へ戻す。
        } else {
            const float phase = (elapsed - 0.64f) / 0.36f;
            const float eased = Easing::SmoothStep(phase);
            multiplier = {1.12f + (1.0f - 1.12f) * eased, 0.86f + (1.0f - 0.86f) * eased,
                          1.10f + (1.0f - 1.10f) * eased};
        }

        targetScale = {baseScale_.x * multiplier.x, baseScale_.y * multiplier.y, baseScale_.z * multiplier.z};
        targetRotation.z = std::sin(elapsed * kTwoPi * 1.5f) * remaining * 0.07f;
        SetColor({1.0f, 0.98f, 0.70f + remaining * 0.16f, 1.0f});
    } else {
        // 通常時は移動・接地で弾みを決め、攻撃色を既定色へ戻す。
        slimeBounceAnimator::Params params;
        params.speedForFullBounce = 2.1f;
        params.idleAmplitude = 0.085f;
        params.moveAmplitude = 0.30f;
        params.hopFrequency = 11.0f;
        params.horizontalSquash = 0.30f;
        params.verticalStretch = 0.38f;
        params.airborneStretch = 0.36f;
        targetScale = slimeBounceAnimator::MakeScale(baseScale_, GetVelocity(), idleTimer_, IsGrounded(), params);
        SetColor(GetDefaultColor());
    }

    // 攻撃または移動の姿勢に被弾反応を重ね、雷の攻撃中は通常時より早く目標姿勢へ寄せる。
    ApplyDamageReactionPose(targetScale, targetRotation);
    engine::Vector3 scale = GetScale();
    const float lerpRate = isCharging_ || wildLightningState_ != WildLightningState::kIdle || shockSquashTimer_ > 0.0f
                               ? (std::min)(1.0f, deltaTime * 24.0f)
                               : (std::min)(1.0f, deltaTime * 11.0f);
    scale = engine::Math::Lerp(scale, targetScale, lerpRate);
    SetScale(scale);
    // 向き付けで更新したYawは保持し、反動による前後と横の傾きだけを補間する。
    const engine::Vector3 currentRotation = GetRotation();
    SetRotation({currentRotation.x + (targetRotation.x - currentRotation.x) * lerpRate, currentRotation.y,
                 currentRotation.z + (targetRotation.z - currentRotation.z) * lerpRate});
}

void EnemyThunderSlime::SyncWorldCollisionRadius(float worldRadius) {
    // 見た目のスケール変化に左右されないワールド半径になるよう局所半径へ換算する。
    const engine::Vector3 scale = GetScale();
    const float maxScale = (std::max)({0.001f, std::abs(scale.x), std::abs(scale.y), std::abs(scale.z)});
    SetCollisionRadius(worldRadius / maxScale);
}

void EnemyThunderSlime::SyncGroundCollisionRadius() {
    // 地上の通常行動では小さい接触半径を使う。
    SyncWorldCollisionRadius(kGroundCollisionWorldRadius);
}

void EnemyThunderSlime::SyncThrownCollisionRadius() {
    // 投擲時は回転しても当たりやすい半径に切り替える。
    SyncWorldCollisionRadius(kThrownCollisionWorldRadius);
}
