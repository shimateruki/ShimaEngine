#include "EnemyBomber.h"
#include "Easing.h"
#include <numbers>
#include "game/system/scene/SceneGameplayAccess.h"
#include "game/actor/enemy/animation/SlimeBounceAnimator.h"
#include "CollisionConfig.h"
#include "CollisionManager.h"
#include "EnemyBomb.h"
#include "game/actor/enemy/spawn/EnemyFactory.h"
#include "game/system/rules/EventManager.h"
#include "MeshEffectManager.h"
#include "GpuParticleManager.h"
#include "SceneManager.h"
#include "BaseScene.h"
#include "Player.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace {
// ボマーの距離維持、予備動作、投げ演出に関する調整値。
constexpr float kMinThrowDistance = 1.0f;
constexpr float kBombSpawnHeight = 2.0f;
constexpr float kFootworkSpeed = 1.25f;
constexpr float kPreferredDistance = 13.0f;
constexpr float kCarryThrowInterval = 0.42f;
constexpr float kCarryBombForwardSpeed = 24.0f;
constexpr float kCarryBombUpSpeed = 8.0f;
constexpr float kThrowRecoilDuration = 0.36f;
constexpr float kThrowRecoverPoseDuration = 0.62f;
constexpr float kThrowLeapDuration = 0.78f;
constexpr float kThrowLandingDuration = 0.34f;
constexpr float kThrowReleaseProgress = 0.52f;
constexpr float kThrowJumpVelocity = 24.0f;
constexpr float kThrowTakeoffLift = 0.18f;
constexpr float kThrowHopForwardSpeed = 3.2f;
constexpr float kHeldBombScale = 0.30f;
constexpr float kBomberModelYawOffset = std::numbers::pi_v<float>;
constexpr const char* kHeldBombModelName = "Gimmicks/blob";
constexpr const char* kCarryBomberThrowEffect = "Resources/json/effect/shared/effect_carry_bomber_throw_burst.json";
constexpr const char* kCarryBomberSparkPreset = "shared/carry_bomber_throw_sparks";
constexpr float kCarryPlaceCooldown = 0.85f;
constexpr float kCarryBlastJumpCooldown = 1.10f;
constexpr float kCarryBlastJumpForwardSpeed = 8.5f;
constexpr float kCarryBlastJumpUpSpeed = 18.0f;
constexpr float kCarryBlastJumpInvincibleDuration = 0.32f;
constexpr float kCarryBlastJumpRadius = 3.2f;
constexpr float kCarryBlastJumpDamage = 1.0f;
constexpr float kCarryBlastTrailDuration = 0.28f;
constexpr float kCarryBlastTrailInterval = 0.055f;
constexpr const char* kCarryBombPlaceEffect = "Resources/json/effect/shared/effect_player_bomb_place.json";
constexpr const char* kCarryBombBlastJumpEffect = "Resources/json/effect/shared/effect_player_bomb_blast_jump.json";
constexpr const char* kCarryBombBlastJumpTrailEffect = "Resources/json/effect/shared/effect_player_bomb_blast_jump_trail.json";
constexpr const char* kCarryBombBlastJumpLandEffect = "Resources/json/effect/shared/effect_player_bomb_blast_jump_land.json";
constexpr const char* kCarryBombPlacePreset = "shared/player_bomb_place_fuse";
constexpr const char* kCarryBombBlastJumpPreset = "shared/player_bomb_blast_jump";
constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kTwoPi = kPi * 2.0f;

float Saturate(float value) {
    // 投擲演出の進行率がタイマーの境界を越えても描画値を一定範囲に保つ。
    return std::clamp(value, 0.0f, 1.0f);
}



float LerpFloat(float start, float end, float rate) {
    // 投げ動作内で指定した進行率を使い、位置や角度の目標値を補間する。
    return start + (end - start) * rate;
}

engine::Object3D* FindEnemyDamageTarget(engine::Object3D* object) {
    // 子オブジェクトに当たった場合も、被弾対象となる親の敵を探す。
    for (engine::Object3D* current = object; current; current = current->GetParent()) {
        // 親をたどる途中でBaseEnemyの実体を見つけたら、そこを被弾対象にする。
        if (dynamic_cast<BaseEnemy*>(current)) {
            return current;
        }
    }

    return nullptr;
}
} // namespace

void EnemyBomber::BindSceneSpawnContext(BaseScene* scene) {
    // 生成した爆弾を元のシーンへ登録する。シーン切替時は古い登録先を外す。
    BaseEnemy::BindSceneSpawnContext(scene);

    // シーンから外す場合は、それまでの登録先を使わないようコールバックを解除する。
    if (!scene) {
        SetSpawnCallback(nullptr);
        return;
    }

    SetSpawnCallback([scene](std::unique_ptr<BaseEnemy> spawned) {
        // 生成が成立しなかった場合は登録せず、実体があれば同じシーンとの紐付けも渡す。
        if (!spawned)
            return;
        spawned->BindSceneSpawnContext(scene);
        scene->AddObject(std::move(spawned));
    });
}

void EnemyBomber::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    // 最初の投擲だけ初期待機を使い、以後の投擲状態は待機から始める。
    BaseEnemy::Initialize(common, modelName);
    // 吸収時はこの個体の設定をコピーするため、投擲AIとは独立にコピー用の定義を読み込む。
    SetEnemyType("Bomber");
    ReloadAttackProfile();
    common_ = common;
    throwTimer_ = initialThrowDelay_;
    windupTimer_ = 0.0f;
    footworkTimer_ = 1.2f;
    footworkDirection_ = 1.0f;
    throwState_ = ThrowState::kIdle;
    throwRecoilTimer_ = 0.0f;
    throwRecoverPoseTimer_ = 0.0f;
    throwLeapTimer_ = 0.0f;
    throwLandingTimer_ = 0.0f;
    throwLeapBaseYaw_ = 0.0f;
    throwSpinDirection_ = 1.0f;
    throwBombReleased_ = false;

    SetCollisionAttribute(engine::collisionCategories::Get("enemy"));
    SetCollisionMask(engine::collisionCategories::Get("player") | engine::collisionCategories::Get("player_attack"));
}

// 距離を保ちながら、爆弾を取り出して跳び投げるAI。
void EnemyBomber::Update(float deltaTime) {
    // 運搬・撃破などの停止条件を先に処理し、通常時だけ距離維持と投擲を進める。
    if (UpdateBlockedState(deltaTime)) {
        return;
    }

    EnsureBaseScaleForAnimation();
    UpdateThrowTimers(deltaTime);

    float distance = 0.0f;
    engine::Vector3 direction{};
    const bool inRange = IsTargetInRange(&distance, &direction);

    // 対象が射程にいる間も、跳躍中は踏み切り時の向きを維持する。
    if (inRange && throwState_ != ThrowState::kLeapThrow) {
        UpdateFacing(direction);
    }

    // 待機中の発見リアクションが有効なフレームは、投擲を選ぶ前に足を止めて向きを合わせる。
    if (throwState_ == ThrowState::kIdle && UpdateNoticeReaction(deltaTime, distance, GetDetectionRange(), direction)) {
        UpdateFacing(direction);
        SetVelocity({0.0f, GetVelocity().y, 0.0f});
        ApplySlimeAnimation(deltaTime);
        BaseEnemy::Update(deltaTime);
        return;
    }

    // 通常フレームでは歩法と投擲段階を順に処理し、その結果を姿勢と共通更新へ渡す。
    UpdateCombatMovement(deltaTime, direction, distance, inRange);
    UpdateThrowState(deltaTime, inRange);
    ApplySlimeAnimation(deltaTime);
    BaseEnemy::Update(deltaTime);
}

bool EnemyBomber::UpdateBlockedState(float deltaTime) {
    // 固有AIが使えない状態では手持ち弾と予兆を消し、共通敵更新へ委ねる。
    // 撃破演出を処理する場合は手持ち表示を隠し、固有の投擲を止める。
    if (ShouldHandleDefeatEffect()) {
        HideHeldBombVisual();
        BaseEnemy::Update(deltaTime);
        return true;
    }

    // 運搬中は爆弾の取り出しを行わず、運搬時の共通更新に任せる。
    if (IsCarried()) {
        HideHeldBombVisual();
        BaseEnemy::Update(deltaTime);
        return true;
    }

    // 対象がいないか死亡済みなら、手持ち表示に加えて残っている予兆も解除する。
    if (!GetTarget() || IsDead()) {
        HideHeldBombVisual();
        HideAttackTelegraph();
        BaseEnemy::Update(deltaTime);
        return true;
    }

    // 投げられた後の硬直中は、共通処理だけを進めて新しい投擲へ入らない。
    if (IsThrowRecovering()) {
        HideHeldBombVisual();
        BaseEnemy::Update(deltaTime);
        return true;
    }

    return false;
}

void EnemyBomber::EnsureBaseScaleForAnimation() {
    // 配置された大きさを見た目の変形基準にし、小さすぎる初期値だけ補う。
    if (hasBaseScale_) {
        return;
    }

    baseScale_ = GetScale();
    const float capturedMaxScale = (std::max)({std::abs(baseScale_.x), std::abs(baseScale_.y), std::abs(baseScale_.z)});

    // 配置時の最大軸が1.2未満なら、通常の見た目に対応する倍率を設定する。
    if (capturedMaxScale < 1.2f) {
        baseScale_ = {2.0f, 2.0f, 2.0f};
        SetScale(baseScale_);
    }

    hasBaseScale_ = true;
}

void EnemyBomber::UpdateThrowTimers(float deltaTime) {
    // 反動と姿勢の余韻は投擲状態とは独立して減らし、待機へ戻っても残す。
    idleTimer_ += deltaTime;
    throwRecoilTimer_ = std::max(0.0f, throwRecoilTimer_ - deltaTime);
    throwRecoverPoseTimer_ = std::max(0.0f, throwRecoverPoseTimer_ - deltaTime);
}

void EnemyBomber::UpdateCombatMovement(float deltaTime, const engine::Vector3& direction, float distance, bool inRange) {
    // 歩法を更新するのは待機中だけです。跳躍と放出は投擲状態側の担当です。
    if (throwState_ == ThrowState::kIdle) {
        UpdateFootwork(deltaTime, direction, distance, inRange);
    } else if (throwState_ == ThrowState::kWindup) {
        // 投擲の予告中は水平移動を弱め、標的表示と実際の発射位置をずらしにくくする。
        engine::Vector3 velocity = GetVelocity();
        velocity.x *= 0.18f;
        velocity.z *= 0.18f;
        SetVelocity(velocity);
    // 着地中は新しい進行方向を選ばず、直前の跳躍の水平速度を弱めます。
    } else if (throwState_ == ThrowState::kLanding) {
        engine::Vector3 velocity = GetVelocity();
        velocity.x *= 0.58f;
        velocity.z *= 0.58f;
        SetVelocity(velocity);
    }
}

void EnemyBomber::UpdateThrowState(float deltaTime, bool inRange) {
    // 新しい投球を待っている間だけ、対象が射程を外れたら投擲の準備をリセットする。
    if (!inRange && throwState_ == ThrowState::kIdle) {
        ResetThrowStateAfterTargetLost();
        return;
    }

    switch (throwState_) {
    case ThrowState::kIdle:
        // 手持ち爆弾は隠し、再使用待ちが終わったら構えに入る。
        HideHeldBombVisual();
        throwTimer_ -= deltaTime;

        // 投擲の再使用待ちが満了してからBeginThrow()で構えに入る。
        if (throwTimer_ <= 0.0f) {
            BeginThrow();
        }

        break;

    case ThrowState::kWindup:
        // 手元に爆弾を見せ、溜め時間が尽きた時点で投球用の跳躍を始める。
        windupTimer_ -= deltaTime;
        {
            // この時点では着弾後の転がりを予測できない。誤った爆発予定地を示さない。
            HideAttackTelegraph();
            UpdateHeldBombVisual(deltaTime);
        }

        // 溜め時間を消化したら跳躍を開始する。実弾の放出はUpdateThrowLeap()が行う。
        if (windupTimer_ <= 0.0f) {
            BeginThrowLeap();
        }

        break;

    case ThrowState::kLeapThrow:
        // 跳躍中の移動と、爆弾を手放すタイミングは UpdateThrowLeap() が担当する。
        UpdateThrowLeap(deltaTime);
        break;

    case ThrowState::kLanding:
        // 保持表示と予兆を片付け、着地の硬直が終わってから次の投球待ちへ戻る。
        HideHeldBombVisual();
        HideAttackTelegraph();
        throwLandingTimer_ -= deltaTime;

        // 着地硬直を終えたら次の投擲までの待ち時間を設定し、再び歩法を許可する。
        if (throwLandingTimer_ <= 0.0f) {
            throwTimer_ = throwInterval_;
            throwState_ = ThrowState::kIdle;
            throwBombReleased_ = false;
        }

        break;
    }
}

void EnemyBomber::ResetThrowStateAfterTargetLost() {
    // 標的喪失で予告と手持ち弾を消し、次の投擲待機は初期待機以下に抑える。
    throwState_ = ThrowState::kIdle;
    windupTimer_ = 0.0f;
    throwLeapTimer_ = 0.0f;
    throwLandingTimer_ = 0.0f;
    throwBombReleased_ = false;
    throwTimer_ = std::min(throwTimer_, initialThrowDelay_);
    HideHeldBombVisual();
    HideAttackTelegraph();
}

void EnemyBomber::Draw(ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource) {
    // 投げる前の手持ち爆弾は本体とは別オブジェクトなので追加描画する。
    BaseEnemy::Draw(pointLightResource, spotLightResource);

    // 本体の描画が有効で、手持ち表示が生成済みかつ可視の場合だけ追加描画する。
    if (GetIsRenderVisible() && heldBombVisual_ && heldBombVisual_->GetIsVisible()) {
        // 個別所有の表示物はObjectManagerの最終更新に含まれない。
        // 本体の物理移動後の手元位置と、描画時のカメラをここで反映する。
        const float progress = throwState_ == ThrowState::kLeapThrow ? 1.0f : GetThrowProgress();
        heldBombVisual_->SetTranslate(ComputeHeldBombPosition(progress));
        heldBombVisual_->UpdateWorldMatrix(true);
        heldBombVisual_->Draw(pointLightResource, spotLightResource);
    }
}

void EnemyBomber::SetCarried(bool isCarried) {
    // 吸収・運搬へ切り替わる際に投擲段階を破棄し、解放後の途中再開を防ぐ。
    BaseEnemy::SetCarried(isCarried);
    throwState_ = ThrowState::kIdle;
    windupTimer_ = 0.0f;
    throwLeapTimer_ = 0.0f;
    throwLandingTimer_ = 0.0f;
    throwBombReleased_ = false;
    HideAttackTelegraph();
}

bool EnemyBomber::IsTargetInRange(float* outDistance, engine::Vector3* outDirection) const {
    // 水平距離と方向を同時に計算し、近すぎる対象には投擲を開始しない。
    // 対象が設定されていなければ距離や方向の出力を変更せずfalseを返す。
    if (!GetTarget())
        return false;

    engine::Vector3 toTarget = GetTarget()->GetTranslate() - GetTranslate();
    toTarget.y = 0.0f;

    const float distance = std::sqrt(toTarget.x * toTarget.x + toTarget.z * toTarget.z);

    // 距離を受け取る引数が指定されている場合だけ、計算した水平距離を書き込む。
    if (outDistance) {
        *outDistance = distance;
    }

    // 十分な距離があるときはXZの差を単位方向に揃える。
    if (distance > 0.001f) {
        toTarget.x /= distance;
        toTarget.z /= distance;
    // 同じ位置にいる場合は方向が作れないため、既定のZ正方向を返す。
    } else {
        toTarget = {0.0f, 0.0f, 1.0f};
    }

    // 方向を受け取る引数が指定されている場合だけ、計算した水平向きを書き込む。
    if (outDirection) {
        *outDirection = toTarget;
    }

    return distance <= GetDetectionRange() && distance >= kMinThrowDistance;
}

void EnemyBomber::UpdateFacing(const engine::Vector3& direction) {
    // モデルの正面補正を加え、最短角で対象へ振り向く。
    const float lengthSq = direction.x * direction.x + direction.z * direction.z;

    // 有効な水平向きが得られた場合だけ回し、方向がほぼ0なら現在の角度を保つ。
    if (lengthSq > 0.0001f) {
        const float targetYaw = std::atan2(direction.x, direction.z) + kBomberModelYawOffset;
        SetRotationY(engine::Math::LerpShortAngle(GetRotation().y, targetYaw, 0.16f));
    }
}

void EnemyBomber::ApplySlimeAnimation(float deltaTime) {
    // 歩行・溜め・跳躍・着地の変形を共通スケールから作り、被弾姿勢を最後に重ねる。
    slimeBounceAnimator::Params params;
    params.speedForFullBounce = 1.15f;
    params.idleAmplitude = 0.075f;
    params.moveAmplitude = 0.28f;
    params.hopFrequency = 9.8f;
    params.horizontalSquash = 0.28f;
    params.verticalStretch = 0.34f;
    params.airborneStretch = 0.30f;

    engine::Vector3 targetScale = slimeBounceAnimator::MakeScale(baseScale_, GetVelocity(), idleTimer_, IsGrounded(), params);

    // 溜め・空中投擲・着地では歩行用の基準形状を上書きし、技の状態を見分けられるようにします。
    // 構え中は溜めの進行に合わせてモデルを押しつぶす。
    if (throwState_ == ThrowState::kWindup) {
        const float chargeRate = GetThrowProgress();
        targetScale = slimeBounceAnimator::MakeChargeSquash(baseScale_, chargeRate, idleTimer_, 0.92f);
    // 跳躍中は空中で伸ばし、爆弾を離した後の反動を形状へ足す。
    } else if (throwState_ == ThrowState::kLeapThrow) {
        const float progress = GetThrowLeapProgress();
        const float arc = std::sin(progress * kPi);
        const float release = Easing::SmoothStep01((progress - kThrowReleaseProgress) / (1.0f - kThrowReleaseProgress));
        const float recoil = std::sin(release * kPi);
        targetScale.x = baseScale_.x * (1.0f - arc * 0.12f + recoil * 0.08f);
        targetScale.y = baseScale_.y * (1.0f + arc * 0.24f - recoil * 0.12f);
        targetScale.z = baseScale_.z * (1.0f + arc * 0.30f + recoil * 0.16f);
    // 着地中は硬直の残り時間から押しつぶしと跳ね返りを作る。
    } else if (throwState_ == ThrowState::kLanding) {
        const float remainRate = Saturate(throwLandingTimer_ / kThrowLandingDuration);
        const float impact = Easing::SmoothStep01(remainRate);
        const float rebound = std::sin((1.0f - remainRate) * kPi) * 0.08f;
        targetScale.x = baseScale_.x * (1.0f + impact * 0.20f - rebound * 0.25f);
        targetScale.y = baseScale_.y * (1.0f - impact * 0.24f + rebound);
        targetScale.z = baseScale_.z * (1.0f + impact * 0.22f - rebound * 0.25f);
    }

    // 待機へ戻った後もしばらく反動と揺れを重ね、投げた直後に通常形状へ飛ばないようにします。
    // 待機へ戻っても反動タイマーが残っていれば、形状へ一時的な伸縮を足す。
    if (throwRecoilTimer_ > 0.0f && throwState_ == ThrowState::kIdle) {
        const float recoilRate = 1.0f - Saturate(throwRecoilTimer_ / kThrowRecoilDuration);
        const float kick = std::sin(recoilRate * kPi);
        targetScale.x *= 1.0f - kick * 0.10f;
        targetScale.y *= 1.0f + kick * 0.12f;
        targetScale.z *= 1.0f + kick * 0.26f;
    }

    // 待機へ戻った後に復帰タイマーが残る場合は、減衰する揺れを形状へ重ねる。
    if (throwRecoverPoseTimer_ > 0.0f && throwState_ == ThrowState::kIdle) {
        const float remainingRate = Saturate(throwRecoverPoseTimer_ / kThrowRecoverPoseDuration);
        const float wave = std::sin((1.0f - remainingRate) * kPi * 3.0f) * remainingRate;
        targetScale.x *= 1.0f + remainingRate * 0.10f + wave * 0.04f;
        targetScale.y *= 1.0f - remainingRate * 0.12f;
        targetScale.z *= 1.0f + remainingRate * 0.18f + wave * 0.06f;
    }

    // 形状と別に傾きを作り、空中投擲では踏み切り時の向きを軸に一回転させます。
    engine::Vector3 animatedRotation = GetRotation();
    float yaw = animatedRotation.y;
    float pitch = 0.0f;
    float roll = 0.0f;

    // 構え中は取り出しの進みに合わせて軽く反り、歩法の方向へ傾ける。
    if (throwState_ == ThrowState::kWindup) {
        const float progress = GetThrowProgress();
        const float liftRate = Easing::SmoothStep01(progress / 0.55f);
        pitch += LerpFloat(0.0f, -0.20f, liftRate);
        roll += std::sin(progress * kPi) * 0.10f * footworkDirection_;
    // 跳躍中は踏み切りで保持したヨーを使い、投げる回転をピッチへ適用する。
    } else if (throwState_ == ThrowState::kLeapThrow) {
        const float progress = GetThrowLeapProgress();
        const float spinRate = Easing::SmoothStep01(progress);
        const float arcRate = std::sin(progress * kPi);
        yaw = throwLeapBaseYaw_;
        pitch = -spinRate * kTwoPi - 0.08f * arcRate;
        roll = std::sin(progress * kPi) * 0.06f * throwSpinDirection_;
    // 着地中は硬直の残り時間から前後の傾きと横揺れを作る。
    } else if (throwState_ == ThrowState::kLanding) {
        const float remainRate = Saturate(throwLandingTimer_ / kThrowLandingDuration);
        pitch = -0.12f * remainRate;
        roll = std::sin((1.0f - remainRate) * kPi) * 0.08f * throwSpinDirection_;
    }

    // 跳躍の回転中を除き、反動が残る場合は体を反らせて横に傾ける。
    if (throwRecoilTimer_ > 0.0f && throwState_ != ThrowState::kLeapThrow) {
        const float recoilRate = 1.0f - Saturate(throwRecoilTimer_ / kThrowRecoilDuration);
        const float kick = std::sin(recoilRate * kPi);
        pitch -= kick * 0.22f;
        roll += kick * 0.12f * footworkDirection_;
    }

    // 待機に戻ってから残る復帰時間は、横揺れにも反映する。
    if (throwRecoverPoseTimer_ > 0.0f && throwState_ == ThrowState::kIdle) {
        const float remainingRate = Saturate(throwRecoverPoseTimer_ / kThrowRecoverPoseDuration);
        roll += std::sin((1.0f - remainingRate) * kPi * 2.0f) * remainingRate * 0.10f;
    }

    // 被弾の姿勢を最後に重ね、スケールは前フレームから補間して反映します。
    engine::Vector3 targetRotation = {pitch, yaw, roll};
    ApplyDamageReactionPose(targetScale, targetRotation);
    SetRotation(targetRotation);
    SetScale(engine::Math::Lerp(GetScale(), targetScale, (std::min)(1.0f, deltaTime * 12.0f)));
}

void EnemyBomber::UpdateHeldBombVisual(float deltaTime) {
    // 本物の爆弾を出す前だけ仮のモデルを表示し、取り出しと振りかぶりに追従させる。
    // 構え・跳躍以外、運搬中、または爆弾を離した後は専用モデルを隠して戻る。
    if ((throwState_ != ThrowState::kWindup && throwState_ != ThrowState::kLeapThrow) || IsCarried() ||
        throwBombReleased_) {
        HideHeldBombVisual();
        return;
    }

    EnsureHeldBombVisual();

    // 作成を試みても表示物がない場合は、その後の姿勢・表示を更新しない。
    if (!heldBombVisual_) {
        return;
    }

    const float progress = (throwState_ == ThrowState::kLeapThrow) ? 1.0f : GetThrowProgress();
    const float appearRate = Easing::SmoothStep01(progress / 0.28f);

    // 取り出し始めはまだ手元に出さず、可視化できる進行率になるのを待つ。
    if (appearRate <= 0.03f) {
        heldBombVisual_->SetIsVisible(false);
        return;
    }

    // 取り出しと投げの進行率に揺れを重ね、現在の手元位置と表示用の姿勢を反映する。
    const float holdPulse = std::sin(idleTimer_ * 16.0f) * 0.035f;
    const float throwRate = (throwState_ == ThrowState::kLeapThrow)
                                ? Easing::SmoothStep01(GetThrowLeapProgress() / kThrowReleaseProgress)
                                : Easing::SmoothStep01((progress - 0.72f) / 0.28f);
    const float visualScale = kHeldBombScale * (0.36f + appearRate * 0.64f) * (1.0f + holdPulse + throwRate * 0.08f);
    const float yaw = GetRotation().y;

    heldBombVisual_->SetIsVisible(true);
    heldBombVisual_->SetTranslate(ComputeHeldBombPosition(progress));
    heldBombVisual_->SetScale({visualScale, visualScale, visualScale});
    heldBombVisual_->SetRotation({progress * 7.0f + idleTimer_ * 0.7f, yaw + progress * 5.0f + throwRate * kPi,
                                  progress * 1.2f + throwRate * 1.6f});
    heldBombVisual_->Update(deltaTime);
}

void EnemyBomber::EnsureHeldBombVisual() {
    // 手持ち用は表示専用なので衝突を持たせず、初回使用時だけ作る。
    // 既に表示物がある場合は再利用し、描画基盤がない場合は生成を待つ。
    if (heldBombVisual_ || !common_) {
        return;
    }

    heldBombVisual_ = std::make_unique<engine::Object3D>();
    heldBombVisual_->Initialize(common_);
    heldBombVisual_->SetModel(kHeldBombModelName);
    heldBombVisual_->SetCollisionAttribute(0);
    heldBombVisual_->SetCollisionMask(0);
    heldBombVisual_->SetScale({kHeldBombScale, kHeldBombScale, kHeldBombScale});
    heldBombVisual_->SetIsVisible(false);
}

void EnemyBomber::HideHeldBombVisual() {
    // 発射・中断後は仮モデルを隠し、実際に生成した爆弾と二重表示しない。
    // 仮の表示物を生成済みの場合だけ非表示にし、次の投擲に再利用する。
    if (heldBombVisual_) {
        heldBombVisual_->SetIsVisible(false);
    }
}

engine::Vector3 EnemyBomber::ComputeHeldBombPosition(float progress) const {
    // 本体の前後・左右・高さを段階ごとに補間し、取り出しから投げ出しまでの手元位置を作る。
    progress = Saturate(progress);
    const float yaw = GetRotation().y - kBomberModelYawOffset;
    const engine::Vector3 forward = {std::sin(yaw), 0.0f, std::cos(yaw)};
    const engine::Vector3 right = {std::cos(yaw), 0.0f, -std::sin(yaw)};
    const float extractRate = Easing::SmoothStep01(progress / 0.22f);
    const float liftRate = Easing::SmoothStep01((progress - 0.08f) / 0.32f);
    const float holdBackRate = Easing::SmoothStep01((progress - 0.40f) / 0.24f);
    const float throwRate = Easing::SmoothStep01((progress - 0.74f) / 0.26f);

    engine::Vector3 position = GetTranslate();
    const float forwardOffset = 1.25f + extractRate * 0.45f - holdBackRate * 0.25f + throwRate * 1.45f;
    const float sideOffset = std::sin(progress * kPi) * 0.16f * footworkDirection_;
    const float heightOffset = 1.45f + liftRate * 1.35f - holdBackRate * 0.12f + throwRate * 0.18f;
    position.x += forward.x * forwardOffset + right.x * sideOffset;
    position.y += heightOffset;
    position.z += forward.z * forwardOffset + right.z * sideOffset;
    return position;
}

float EnemyBomber::GetThrowProgress() const {
    // 残り時間を0から1の進行率へ変換し、予兆と手持ち弾の表示に共用する。
    return 1.0f - std::clamp(windupTimer_ / (std::max)(throwWindup_, 0.01f), 0.0f, 1.0f);
}

float EnemyBomber::GetThrowLeapProgress() const {
    // 跳躍中の放出時刻と着地姿勢を同じ進行率で判定する。
    return 1.0f - std::clamp(throwLeapTimer_ / kThrowLeapDuration, 0.0f, 1.0f);
}

void EnemyBomber::UpdateFootwork(float deltaTime, const engine::Vector3& direction, float distance, bool inRange) {
    // 射程内では横移動しながら理想距離を保ち、射程外では巡回速度を使う。
    engine::Vector3 velocity = GetVelocity();
    velocity.x = 0.0f;
    velocity.z = 0.0f;

    // 射程内では時間ごとに横移動を反転し、同じ側へ回り続けないようにします。
    if (inRange) {
        footworkTimer_ -= deltaTime;

        // 横移動の区間が終わったら左右を反転し、次の区間を始める。
        if (footworkTimer_ <= 0.0f) {
            footworkDirection_ *= -1.0f;
            footworkTimer_ = 1.4f;
        }

        engine::Vector3 side = {direction.z * footworkDirection_, 0.0f, -direction.x * footworkDirection_};
        velocity.x += side.x * kFootworkSpeed;
        velocity.z += side.z * kFootworkSpeed;

        // 投げ終わりは通常の横移動に後退を足し、技の反動を足運びにも出します。
        if (throwRecoverPoseTimer_ > 0.0f) {
            const float recoverRate = Saturate(throwRecoverPoseTimer_ / kThrowRecoverPoseDuration);
            velocity.x -= direction.x * recoverRate * 1.15f;
            velocity.z -= direction.z * recoverRate * 1.15f;
            velocity.x += side.x * recoverRate * 0.45f;
            velocity.z += side.z * recoverRate * 0.45f;
        }

        // 横移動は維持したまま、近すぎると後退し、十分に離れると少し近づきます。
        if (distance < kPreferredDistance) {
            velocity.x -= direction.x * 0.8f;
            velocity.z -= direction.z * 0.8f;
        // 理想距離に4を足した位置より遠いときだけ、横移動に接近の速度を足す。
        } else if (distance > kPreferredDistance + 4.0f) {
            velocity.x += direction.x * 0.6f;
            velocity.z += direction.z * 0.6f;
        }
    // 射程条件を満たさない場合は発見後の横移動をやめ、巡回速度を取得する。
    } else {
        velocity = CalculateWanderVelocity(deltaTime, kFootworkSpeed * 0.65f, 0.7f);
        engine::Vector3 wanderDirection = {velocity.x, 0.0f, velocity.z};
        const float lengthSq = wanderDirection.x * wanderDirection.x + wanderDirection.z * wanderDirection.z;

        // 水平に移動しているときだけ、巡回方向へ向きを寄せる。
        if (lengthSq > 0.0001f) {
            const float targetYaw = std::atan2(wanderDirection.x, wanderDirection.z) + kBomberModelYawOffset;
            SetRotationY(engine::Math::LerpShortAngle(GetRotation().y, targetYaw, 0.08f));
        }
    }

    SetVelocity(velocity);
}

void EnemyBomber::BeginThrow() {
    // 予備動作へ入る時に前回の跳躍・放出状態を消し、手持ち弾を用意する。
    throwState_ = ThrowState::kWindup;
    windupTimer_ = throwWindup_;
    throwLeapTimer_ = 0.0f;
    throwLandingTimer_ = 0.0f;
    throwBombReleased_ = false;
    EnsureHeldBombVisual();
    UpdateHeldBombVisual(0.0f);
}

void EnemyBomber::BeginThrowLeap() {
    // 跳躍開始時の向きを固定し、回転演出中も発射方向が振れないようにする。
    throwState_ = ThrowState::kLeapThrow;
    throwLeapTimer_ = kThrowLeapDuration;
    throwLandingTimer_ = 0.0f;
    throwBombReleased_ = false;
    throwLeapBaseYaw_ = GetRotation().y;
    const float throwMoveYaw = throwLeapBaseYaw_ - kBomberModelYawOffset;
    throwSpinDirection_ = (footworkDirection_ >= 0.0f) ? 1.0f : -1.0f;

    // 放出に十分な鉛直初速と前進速度を与え、接地状態から明示的に離す。
    engine::Vector3 velocity = GetVelocity();
    velocity.y = (std::max)(velocity.y, kThrowJumpVelocity);
    velocity.x = std::sin(throwMoveYaw) * kThrowHopForwardSpeed;
    velocity.z = std::cos(throwMoveYaw) * kThrowHopForwardSpeed;
    SetVelocity(velocity);
    engine::Vector3 position = GetTranslate();
    position.y += kThrowTakeoffLift;
    SetTranslate(position);
    SetGrounded(false);
    UpdateHeldBombVisual(0.0f);
}

void EnemyBomber::UpdateThrowLeap(float deltaTime) {
    // 進行率が放出点を越えたフレームだけ本物の爆弾を生成する。
    throwLeapTimer_ -= deltaTime;

    // まだ放出を試みていない間は、空中で移動する手持ち表示を更新する。
    if (!throwBombReleased_) {
        UpdateHeldBombVisual(deltaTime);
    }

    const float progress = GetThrowLeapProgress();

    // 最初に放出点へ達した更新でだけThrowBomb()を呼び、失敗時も同じ跳躍では再試行しない。
    if (!throwBombReleased_ && progress >= kThrowReleaseProgress) {
        ThrowBomb();
        throwBombReleased_ = true;
    }

    // 投擲後に早く接地しても最低限の空中姿勢を見せ、時間超過では接地を待たず硬直へ進めます。
    if (throwBombReleased_ && ((IsGrounded() && progress >= 0.68f) || throwLeapTimer_ <= -0.32f)) {
        BeginThrowLanding();
    }
}

void EnemyBomber::BeginThrowLanding() {
    // 手持ち表示と予兆を消して着地硬直へ移り、姿勢の余韻だけ残す。
    throwState_ = ThrowState::kLanding;
    throwLandingTimer_ = kThrowLandingDuration;
    throwLeapTimer_ = 0.0f;
    throwRecoverPoseTimer_ = kThrowRecoverPoseDuration;
    HideHeldBombVisual();
    HideAttackTelegraph();
}

void EnemyBomber::ThrowBomb() {
    // 狙った位置までの飛行時間から放物線の初速を求め、点火済み爆弾をシーンへ渡す。
    HideAttackTelegraph();
    HideHeldBombVisual();
    throwRecoilTimer_ = kThrowRecoilDuration;
    throwRecoverPoseTimer_ = 0.0f;
    // 手持ち表示と反動は先に更新します。爆弾の追加先や目標が失われた場合は実体を生成しません。
    if (!spawnCallback_ || !common_ || !GetTarget())
        return;

    const engine::Vector3 bodyPos = GetTranslate();
    engine::Vector3 targetDirection = GetTarget()->GetTranslate() - bodyPos;
    targetDirection.y = 0.0f;
    float directionLength = std::sqrt(targetDirection.x * targetDirection.x + targetDirection.z * targetDirection.z);

    // 対象まで水平距離があれば、放出点を置くために水平差を正規化する。
    if (directionLength > 0.001f) {
        targetDirection.x /= directionLength;
        targetDirection.z /= directionLength;
    // 対象と水平位置が重なる場合は、本体の現在角度を基準に放出点を決める。
    } else {
        targetDirection = {std::sin(GetRotation().y), 0.0f, std::cos(GetRotation().y)};
    }

    // 対象への水平向きを使ってモデルの前上方に放出点を置き、そこから投擲の方向を計算し直します。
    const engine::Vector3 releasePos = {bodyPos.x + targetDirection.x * 2.95f, bodyPos.y + 2.85f,
                                bodyPos.z + targetDirection.z * 2.95f};

    auto bomb = EnemyFactory::GetInstance()->CreateEnemy("Bomb", common_);
    // Bombの実体を生成できなければ、軌道・点火とシーン登録は行わない。
    if (!bomb)
        return;

    engine::Vector3 spawnPos = releasePos;
    bomb->SetTranslate(spawnPos);
    bomb->SetTarget(GetTarget());

    engine::Vector3 toTarget = GetTarget()->GetTranslate() - spawnPos;
    toTarget.y = 0.0f;
    float distance = std::sqrt(toTarget.x * toTarget.x + toTarget.z * toTarget.z);

    // 放出点から対象への水平差が有効なら、爆弾と放出演出の向きに使う。
    if (distance > 0.001f) {
        toTarget.x /= distance;
        toTarget.z /= distance;
    // 放出点と対象が重なるときは本体から決めた方向を引き継ぐ。
    } else {
        toTarget = targetDirection;
        distance = directionLength;
    }

    bomb->SetRotationY(std::atan2(toTarget.x, toTarget.z));
    bomb->SetCarried(false);

    const engine::Vector3 targetPos = GetTarget()->GetTranslate();
    engine::Vector3 deltaToTarget = {targetPos.x - spawnPos.x, (targetPos.y + 0.35f) - spawnPos.y, targetPos.z - spawnPos.z};
    const float horizontalDistance = std::sqrt(deltaToTarget.x * deltaToTarget.x + deltaToTarget.z * deltaToTarget.z);
    // 水平距離から到達秒数を決め、重力で落ちる量を補った鉛直初速を与えます。
    const float flightTime = std::clamp(horizontalDistance / 12.0f, 0.85f, 1.35f);
    const float gravity = 60.0f;
    const float velocityX = deltaToTarget.x / flightTime;
    const float velocityZ = deltaToTarget.z / flightTime;
    // 到達時の高さ差を vY×時間－重力×時間²/2 と置いて初速を逆算し、極端な仰角を避けるため直後に制限します。
    float velocityY = (deltaToTarget.y + 0.5f * gravity * flightTime * flightTime) / flightTime;
    velocityY = std::clamp(velocityY, 14.0f, 28.0f);
    bomb->SetVelocity({velocityX, velocityY, velocityZ});

    // 着火操作に対応するEnemyBombの実体なら2.8秒の導火時間で点火する。
    if (auto* enemyBomb = dynamic_cast<EnemyBomb*>(bomb.get())) {
        enemyBomb->Ignite(2.8f);
    }

    // メッシュ演出の管理が使える場合は、放出位置に投げ始めの表示を加える。
    if (MeshEffectManager::GetInstance()) {
        MeshEffectManager::GetInstance()->SpawnEffectAt(kCarryBomberThrowEffect, releasePos,
                                                        {0.0f, std::atan2(toTarget.x, toTarget.z), 0.0f},
                                                        {0.65f, 0.65f, 0.65f});
    }

    // GPU粒子が初期化されている場合だけ、放出位置に火花を発生させる。
    if (auto* gpuParticleManager = GpuParticleManager::GetInstance(); gpuParticleManager->IsInitialized()) {
        gpuParticleManager->Emit(kCarryBomberSparkPreset, releasePos);
    }

    // 起爆と軌道を確定した実体の所有権を、登録済みの追加先へ移します。
    spawnCallback_(std::move(bomb));
}

void EnemyBomber::SpawnBombObject(std::unique_ptr<BaseEnemy> bomb) {
    // 専用コールバックがある場合はそちらへ渡し、なければ現在シーンへ直接登録する。
    // 渡された爆弾が無効ならシーンを探さず戻る。
    if (!bomb) {
        return;
    }

    // 登録先のコールバックがある場合は、所有権を移し直接登録の経路には進まない。
    if (spawnCallback_) {
        spawnCallback_(std::move(bomb));
        return;
    }

    SceneManager* sceneManager = SceneManager::GetInstance();

    // コールバックがなく現在のシーンも取得できない場合は、追加先がないため戻る。
    if (!sceneManager || !sceneManager->GetCurrentScene()) {
        return;
    }

    // 直接登録する爆弾が運ばれていなければ、現在のシーンのプレイヤーを対象にする。
    if (!bomb->IsCarried()) {
        bomb->SetTarget(GetScenePlayer(sceneManager->GetCurrentScene()));
    }

    sceneManager->GetCurrentScene()->AddObject(std::move(bomb));
}
