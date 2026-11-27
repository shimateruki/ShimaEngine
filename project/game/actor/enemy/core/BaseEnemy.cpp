#include <cstdint>
#include "BaseEnemy.h"
#include "Easing.h"
#include <numbers>
#include "game/actor/enemy/combat/AttackTelegraph.h"
#include "CollisionConfig.h" // enemyなどの定義を使うため
#include "Event.h"           //  DamageEventを使うため
#include "game/system/rules/EventManager.h"    //  イベントを発行(Dispatch)するため
#include "Player.h"          //  プレイヤーの状態を見るため
#include "game/actor/bullet/Bullet.h"

#include "CollisionManager.h"
#include "SceneManager.h"
#include "BaseScene.h"
#include "game/system/scene/SceneGameplayAccess.h"
#include "ParticleSystem.h"
#include "game/system/combat/HitEffectDirector.h"
#include "MeshEffectManager.h"
#include "VfxSequencer.h"
#include "GimmickCoin.h"
#include "game/system/combat/GroundEffectLocator.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string_view>
#include "ModelManager.h"
#include "engine/graphics/3d/MeshRenderer.h"

void BaseEnemy::BindSceneSpawnContext(BaseScene* scene) {
    // シーン登録後に目標を結び、生成順に依存してプレイヤー参照を失わないようにする。
    SetTarget(GetScenePlayer(scene));
}

namespace {
// 共通の徘徊、投げ物理、叩きつけ、撃破演出で使う調整値
constexpr float kWanderPi = std::numbers::pi_v<float>;
constexpr float kThrownMaxTime = 3.0f;
constexpr float kThrownSettleTime = 0.22f;
constexpr float kThrownSettleSpeed = 1.4f;
constexpr float kThrownGroundFriction = 0.90f;
constexpr float kThrownAirDrag = 0.995f;
constexpr float kThrownBounceMinSpeed = 5.0f;
constexpr float kThrownSlamMinSpeed = 13.0f;
constexpr float kThrownFollowUpSlamMinSpeed = 7.0f;
constexpr float kThrownGroundNormal = 0.55f;
constexpr float kThrowRecoveryDuration = 0.45f;
constexpr float kThrowRecoveryGroundFriction = 0.82f;
constexpr std::int32_t kMaxSlamImpacts = 2;
constexpr float kSlamImpactCooldown = 0.16f;
constexpr float kSlamRadiusBase = 4.0f;
constexpr float kSlamDamageBase = 46.0f;
constexpr float kSlamDamageScale = 0.70f;
constexpr float kDefeatEffectDuration = 1.05f;
constexpr float kDefeatRiseHeight = 1.45f;
constexpr float kDefeatSpinSpeed = 5.4f;
constexpr float kDefeatParticleInterval = 0.085f;
constexpr const char* kEnemyDefeatPopSequence = "shared/enemy_defeat_pop_cue";
constexpr const char* kEnemyDefeatCoreEffect = "Resources/json/effect/shared/effect_enemy_defeat_core_flash.json";
constexpr const char* kEnemyDefeatRingEffect = "Resources/json/effect/shared/effect_enemy_defeat_pop_ring.json";
constexpr const char* kEnemyDropCoinModel = "Gimmicks/koin";
constexpr engine::Vector3 kEnemyDropCoinScale = {0.055f, 0.055f, 0.014f};
constexpr float kEnemyDropCoinWorldCollectRadius = 0.50f;
constexpr float kEnemyDropCoinLifetime = 8.0f;
constexpr float kEnemyDropCoinBlinkStart = 2.2f;
constexpr const char* kEnemyDetectedMarkModel = "GeneratedText/text3d_240c8dec";
constexpr const char* kEnemyLostMarkModel = "GeneratedText/text3d_3a0cb08e";
constexpr float kEnemyNoticeDuration = 0.36f;
constexpr float kEnemyLostDuration = 1.55f;
constexpr float kEnemyNoticeCooldown = 1.25f;
constexpr float kDamageReactionMinDuration = 0.22f;
constexpr float kDamageReactionMaxDuration = 0.34f;
constexpr float kEnemyHurtFaceSeconds = 0.18f;

/// <summary>敵の通常モデルを名前またはファイル名で照合し、一時的に表示する被弾モデルを選ぶための対応表の一行。</summary>
struct EnemyHurtFaceModel {
    std::string_view baseName;
    std::string_view fileName;
    const char* hurtModel;
};

// 通常敵とボスの本体に対応する被弾素材。プリズムの目は固有クラスで切り替える。
constexpr EnemyHurtFaceModel kEnemyHurtFaceModels[] = {
    {"Characters/slime", "Characters/slime/slime.gltf", "Characters/slime/slime_hurt.gltf"},
    {"Characters/slime_pink", "Characters/slime_pink/slime_pink.gltf", "Characters/slime_pink/slime_pink_hurt.gltf"},
    {"Characters/slime_red", "Characters/slime_red/slime_red.gltf", "Characters/slime_red/slime_red_hurt.gltf"},
    {"Characters/slime_yellow", "Characters/slime_yellow/slime_yellow.gltf", "Characters/slime_yellow/slime_yellow_hurt.gltf"},
    {"Characters/slime_wind", "Characters/slime_wind/slime_wind.gltf", "Characters/slime_wind/slime_wind_hurt.gltf"},
    {"Characters/slime_black", "Characters/slime_black/slime_black.gltf", "Characters/slime_black/slime_black_hurt.gltf"},
    {"Characters/magma_slime", "Characters/magma_slime/magma_slime.obj", "Characters/magma_slime/magma_slime_hurt.obj"},
    {"Characters/false_king_slime", "Characters/false_king_slime/false_king_slime.obj",
     "Characters/false_king_slime/false_king_slime_hurt.obj"},
    {"Characters/false_king_slime/false_king_slime_without_emblem",
     "Characters/false_king_slime/false_king_slime_without_emblem.obj",
     "Characters/false_king_slime/false_king_slime_without_emblem_hurt.obj"},
};

// 保存済みのフォルダー名・ファイル名の両方を受け取り、対応素材がない場合はnullptrを返す。
const char* FindEnemyHurtFaceModel(std::string_view name) {
    for (const auto& model : kEnemyHurtFaceModels) {
        if (name == model.baseName || name == model.fileName) {
            return model.hurtModel;
        }
    }
    return nullptr;
}


float PlanarDistance(const engine::Vector3& a, const engine::Vector3& b) {
    // 高低差は索敵距離へ含めず、地上での追跡範囲を安定させる。
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}


float PlanarLength(const engine::Vector3& value) {
    // 投擲中の滑走速度は垂直速度と分けて測る。
    return std::sqrt(value.x * value.x + value.z * value.z);
}

float MaxAbsScaleComponent(const engine::Vector3& value) {
    // ゼロスケールでも表示半径の計算が壊れないよう下限を置く。
    return (std::max)({std::abs(value.x), std::abs(value.y), std::abs(value.z), 0.001f});
}

engine::Vector3 NormalizePlanarOr(const engine::Vector3& value, const engine::Vector3& fallback) {
    // 敵と目標が重なった時は既定方向を使い、ノックバックを失わない。
    engine::Vector3 result = {value.x, 0.0f, value.z};
    const float length = PlanarLength(result);

    if (length <= 0.001f) {
        return fallback;
    }

    return {result.x / length, 0.0f, result.z / length};
}



float CalculateSlamDamage(float impactSpeed) {
    // 着地の勢いを威力へ変換し、同じ値を自身と周囲の被害へ使う。
    return kSlamDamageBase + impactSpeed * kSlamDamageScale;
}

float NextDropRandom(uint32_t& seed) {
    // 局所的な乱数種を進め、複数のコインに異なる飛散方向を与える。
    seed = seed * 1664525u + 1013904223u;
    return static_cast<float>((seed >> 8) & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
}

std::int32_t GetCoinDropCountByEnemyType(const std::string& enemyType) {
    // Stage 3のボス報酬は専用王冠だけに絞り、通常コインで勝利演出を散らしません。
    if (enemyType == "FalseKingSlime") {
        return 0;
    }

    // クリスタルとマグマのボス撃破報酬を同じ枚数にそろえる。
    if (enemyType == "PrismSlime" || enemyType == "MagmaSlime") {
        return 12;
    }

    // 巨大スライムは8枚、属性スライムとBomberは4枚、その他は通常の3枚を落とす。
    if (enemyType == "GiantSlime") {
        return 8;
    }

    if (enemyType == "FireSlime" || enemyType == "ThunderSlime" || enemyType == "WindSlime" || enemyType == "Bomber") {
        return 4;
    }

    return 3;
}
} // namespace

// 共通の描画・判定・感知状態を初期化し、固有敵が攻撃定義や外見を設定できる状態にする。
void BaseEnemy::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    // 1. 親クラス(Character)の初期化
    Character::Initialize(common);

    // 2. モデルをセット
    SetModel(modelName);
    hurtFaceModel_ = nullptr;
    // 対応する被弾モデルがある場合に先読みし、初回の被弾時に初めて準備する処理を減らす。
    if (const char* hurtModel = FindEnemyHurtFaceModel(GetModelName())) {
        ModelManager::GetInstance()->LoadModel(hurtModel);
    }

    // 3. 当たり判定の設定
    SetCollisionAttribute(engine::collisionCategories::Get("enemy")); // 自分は「敵」グループ
    SetCollisionMask(engine::collisionCategories::Get("player") | engine::collisionCategories::Get("ground") | engine::collisionCategories::Get("player_bullet") | engine::collisionCategories::Get("player_attack"));
    SetClassName("Enemy");
    // 既定色を記録し、索敵・被弾の時計をリセットして予兆表示を準備する。
    defaultColor_ = GetColor();
    hasSpawnedDefeatCoinDrops_ = false;
    wasTargetDetected_ = false;
    bossTargetEngaged_ = false;
    isNoticeReactionActive_ = false;
    noticeReactionKind_ = NoticeReactionKind::kNone;
    noticeReactionTimer_ = 0.0f;
    noticeReactionCooldown_ = 0.0f;
    noticeDetectionRange_ = detectionRange_;
    noticeMarkYaw_ = 0.0f;
    damageReactionTimer_ = 0.0f;
    damageReactionDuration_ = 0.0f;
    damageReactionStrength_ = 0.0f;
    damageReactionLocalDirection_ = {0.0f, 0.0f, -1.0f};
    attackTelegraph_ = std::make_unique<AttackTelegraph>();
    attackTelegraph_->Initialize(common);
}

bool BaseEnemy::ReloadAttackProfile(std::string* errorMessage) {
    // 種別に対応する攻撃設定を読み直し、失敗理由は編集画面へ返す。
    const std::string enemyType = GetEnemyType();

    // 敵タイプが未設定だと読込対象を決められない。通知先がある場合は理由も返す。
    if (enemyType.empty()) {
        if (errorMessage) {
            *errorMessage = "敵タイプが未設定のため攻撃プロファイルを読み込めません。";
        }

        return false;
    }

    return EnemyAttackProfile::LoadCachedForEnemy(enemyType, attackProfile_, errorMessage);
}

const EnemyAttackDefinition& BaseEnemy::GetAttackDefinition(const std::string& attackId) const {
    // 指定攻撃が欠けても参照の寿命を保てる代替定義を返す。
    if (const EnemyAttackDefinition* attack = attackProfile_.FindAttack(attackId)) {
        return *attack;
    }

    static const EnemyAttackDefinition kFallback;
    return kFallback;
}

// 徘徊目標の管理
void BaseEnemy::CaptureWanderOrigin() {
    // 初回の位置を巡回中心に固定し、追跡中の移動で範囲が流れないようにする。
    if (hasWanderOrigin_) {
        return;
    }

    wanderOrigin_ = GetTranslate();
    wanderTarget_ = wanderOrigin_;
    wanderSeed_ ^= static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this));
    wanderSeed_ ^= static_cast<uint32_t>((std::abs(wanderOrigin_.x) + 1.0f) * 73856093.0f);
    wanderSeed_ ^= static_cast<uint32_t>((std::abs(wanderOrigin_.z) + 1.0f) * 19349663.0f);

    // 種がゼロになった場合は、初期化時の既定値を使う。
    if (wanderSeed_ == 0u) {
        wanderSeed_ = 0x12345678u;
    }

    hasWanderOrigin_ = true;
    PickWanderTarget(0.65f, 0.0f);
}

void BaseEnemy::ResetWanderOrigin() {
    // 再配置後は古い巡回地点と待機時間を使い回さない。
    hasWanderOrigin_ = false;
    wanderWaitTimer_ = 0.0f;
    wanderRetargetTimer_ = 0.0f;
}

float BaseEnemy::NextWanderRandom() {
    // 敵ごとの巡回用乱数種を一段進め、候補方角や待機時間の抽選用に0以上1未満の値を返す。
    wanderSeed_ = wanderSeed_ * 1664525u + 1013904223u;
    return static_cast<float>((wanderSeed_ >> 8) & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
}

float BaseEnemy::GetWanderRadius(float radiusScale) const {
    // 索敵範囲を基準に巡回半径を決め、設定値が小さくても最低限の距離を確保する。
    const float range = GetParameters().has_value() ? GetParameters()->detectionRange : detectionRange_;
    const float baseRadius = (std::max)(3.0f, range * radiusScale);
    return (std::clamp)(baseRadius, 3.0f, 18.0f);
}

void BaseEnemy::PickWanderTarget(float radiusScale, float verticalOffset) {
    // 記録した中心から次の目標を選び、無制限に遠くへ歩かないようにする。
    const float radius = GetWanderRadius(radiusScale);
    const float angle = NextWanderRandom() * kWanderPi * 2.0f;
    const float distance = radius * (0.25f + NextWanderRandom() * 0.75f);

    wanderTarget_.x = wanderOrigin_.x + std::sin(angle) * distance;
    wanderTarget_.y = wanderOrigin_.y + verticalOffset;
    wanderTarget_.z = wanderOrigin_.z + std::cos(angle) * distance;
    wanderRetargetTimer_ = 2.4f + NextWanderRandom() * 2.6f;
}

engine::Vector3 BaseEnemy::GetWanderTargetPosition(float deltaTime, float radiusScale, float verticalOffset) {
    // 目標到着後に短い待機を挟み、毎フレームの再抽選を防ぐ。
    CaptureWanderOrigin();

    if (wanderWaitTimer_ > 0.0f) {
        wanderWaitTimer_ = (std::max)(0.0f, wanderWaitTimer_ - deltaTime);
        return wanderTarget_;
    }

    wanderRetargetTimer_ = (std::max)(0.0f, wanderRetargetTimer_ - deltaTime);
    const float radius = GetWanderRadius(radiusScale);
    const float distanceToTarget = PlanarDistance(GetTranslate(), wanderTarget_);
    const float distanceFromOrigin = PlanarDistance(GetTranslate(), wanderOrigin_);

    if (distanceFromOrigin > radius * 1.08f) {
        // 中心から離れすぎた場合は再抽選せず、中心へ向かう目標に置き換えます。
        engine::Vector3 toOrigin = wanderOrigin_ - GetTranslate();
        toOrigin.y = 0.0f;
        const float length = (std::max)(0.001f, std::sqrt(toOrigin.x * toOrigin.x + toOrigin.z * toOrigin.z));
        wanderTarget_ = GetTranslate() + engine::Vector3{toOrigin.x / length, 0.0f, toOrigin.z / length} * (radius * 0.45f);
        wanderTarget_.y = wanderOrigin_.y + verticalOffset;
        wanderRetargetTimer_ = 1.2f;
    } else if (distanceToTarget < 0.85f) {
        // 目標へ着いたら休止に入り、休止後に向かう地点を決めます。
        wanderWaitTimer_ = 0.45f + NextWanderRandom() * 1.0f;
        PickWanderTarget(radiusScale, verticalOffset);
    } else if (wanderRetargetTimer_ <= 0.0f) {
        // 障害物などで到達できないまま時間が経った場合も、新しい目標を選びます。
        PickWanderTarget(radiusScale, verticalOffset);
    }

    return wanderTarget_;
}

engine::Vector3 BaseEnemy::CalculateWanderVelocity(float deltaTime, float moveSpeed, float radiusScale, float verticalOffset,
                                           bool includeVertical) {
    // 巡回目標への速度だけ計算し、最終的な衝突解決は各敵の更新に任せる。
    engine::Vector3 velocity = GetVelocity();
    const engine::Vector3 target = GetWanderTargetPosition(deltaTime, radiusScale, verticalOffset);

    // 待機中は水平移動を止め、上下も自分で制御する敵だけ鉛直速度を止める。
    if (wanderWaitTimer_ > 0.0f) {
        velocity.x = 0.0f;
        velocity.z = 0.0f;

        if (includeVertical) {
            velocity.y = 0.0f;
        }

        return velocity;
    }

    engine::Vector3 toTarget = target - GetTranslate();

    // 地上の敵は高さを追わず、水平の距離だけで速度を決める。
    if (!includeVertical) {
        toTarget.y = 0.0f;
    }

    const float distance = std::sqrt(toTarget.x * toTarget.x + toTarget.y * toTarget.y + toTarget.z * toTarget.z);

    // 目標とほぼ同じ位置では方向を正規化せず、担当する軸の速度をゼロにする。
    if (distance <= 0.001f) {
        velocity.x = 0.0f;
        velocity.z = 0.0f;

        if (includeVertical) {
            velocity.y = 0.0f;
        }

        return velocity;
    }

    // 目標へ近づくほど速度を落とし、上下を追う設定では Y にも同じ減速を適用する。
    const float speed = (std::max)(0.0f, moveSpeed);
    const engine::Vector3 direction = toTarget / distance;
    const float slowRate = (std::clamp)(distance / 2.0f, 0.25f, 1.0f);
    velocity.x = direction.x * speed * slowRate;
    velocity.z = direction.z * speed * slowRate;

    if (includeVertical) {
        velocity.y = direction.y * speed * slowRate;
    }

    return velocity;
}

// 持ち上げ後に投げられた敵の物理挙動
void BaseEnemy::BeginThrown(const engine::Vector3& initialVelocity) {
    // 運搬状態から物理投擲へ移る際に、攻撃予告と気付き演出を先に解除する。
    CancelNoticeReaction();
    // 投げる直前の向きを起き上がりの終点として保存する。
    throwRecoveryTargetRotation_ = GetRotation();

    // 運搬中なら先に解除し、運搬用の判定状態のまま飛ばさない。
    if (isCarried_) {
        SetCarried(false);
    }

    // 前回の着地回数とタイマーを消し、今回の初速から投擲を始める。
    isThrownPhysics_ = true;
    isThrowRotationRecovering_ = false;
    slamImpactCount_ = 0;
    slamImpactCooldownTimer_ = 0.0f;
    thrownTimer_ = 0.0f;
    thrownSettleTimer_ = 0.0f;
    throwRecoveryRotateTimer_ = 0.0f;
    throwRecoveryTimer_ = 0.0f;
    SetVelocity(initialVelocity);

    // 未設定の能力値を補い、投擲中に被害を扱える状態にする。
    if (!GetParameters().has_value()) {
        ResetParameters();
    }

    // 投げた向きと速さから回転を作る。上下の回転量には上限を設ける。
    const float planarSpeed = PlanarLength(initialVelocity);
    thrownAngularVelocity_ = {(std::clamp)(initialVelocity.z * 0.08f, -7.5f, 7.5f),
                              (std::clamp)(planarSpeed * 0.08f, 1.5f, 8.0f),
                              (std::clamp)(-initialVelocity.x * 0.08f, -7.5f, 7.5f)};

    // 運搬中に止めていた当たり判定を戻し、地面やプレイヤーの攻撃に反応できるようにする。
    SetCollisionAttribute(engine::collisionCategories::Get("enemy"));
    SetCollisionMask(engine::collisionCategories::Get("player") | engine::collisionCategories::Get("ground") | engine::collisionCategories::Get("player_bullet") | engine::collisionCategories::Get("player_attack"));
}

void BaseEnemy::UpdateThrownPhysics(float deltaTime) {
    // 衝突後の滑走を減衰させ、十分に止まった時だけ起き上がりへ移す。
    thrownTimer_ += deltaTime;
    slamImpactCooldownTimer_ = (std::max)(0.0f, slamImpactCooldownTimer_ - deltaTime);

    // 空中でも水平方向を減衰させ、地面では滑走用の摩擦をさらに適用する。
    SetVelocityX(GetVelocity().x * (kThrownAirDrag));
    SetVelocityZ(GetVelocity().z * (kThrownAirDrag));

    if (IsGrounded()) {
        SetVelocityX(GetVelocity().x * (kThrownGroundFriction));
        SetVelocityZ(GetVelocity().z * (kThrownGroundFriction));

        // 水平と上下の速度がどちらも小さい間だけ、静止へ近づいた時間を加算する。
        if (PlanarLength(GetVelocity()) <= kThrownSettleSpeed && std::abs(GetVelocity().y) <= 1.0f) {
            // 両軸がしきい値以下で止まり続けた時間を積算する。
            thrownSettleTimer_ += deltaTime;
        } else {
            thrownSettleTimer_ = 0.0f;
        }

        // 着地衝撃を一度以上出した後、十分に静止が続いたら起き上がりへ進む。
        if (slamImpactCount_ > 0 && thrownSettleTimer_ >= kThrownSettleTime) {
            EndThrownPhysics();
        }
    }

    // 衝撃が発生しなかった場合も、最大時間で投擲を終える。
    if (thrownTimer_ >= kThrownMaxTime) {
        EndThrownPhysics();
    }

    // このフレームで起き上がりへ移ったら、投擲中の回転は重ねない。
    if (!isThrownPhysics_) {
        return;
    }

    // 投擲が続く間は、開始時に決めた角速度で姿勢を更新する。
    engine::Vector3 rotation = GetRotation();
    rotation.x += thrownAngularVelocity_.x * deltaTime;
    rotation.y += thrownAngularVelocity_.y * deltaTime;
    rotation.z += thrownAngularVelocity_.z * deltaTime;
    SetRotation(rotation);
}

void BaseEnemy::HandleThrownCollision(const engine::CollisionInfo& info, uint32_t attribute) {
    // 衝突法線方向の入射速度で着地衝撃と反発を分け、横滑りだけでは着地扱いしない。
    const engine::Vector3 incomingVelocity = GetVelocity();
    // 法線は面から敵へ向くため、内積の符号を反転すると面へ向かう速さが正になります。
    const float normalSpeed = -engine::Math::Dot(incomingVelocity, info.normal);
    const bool isGroundLike = info.normal.y > kThrownGroundNormal;

    // 着地判定に使う速度は補正前に取得しておき、実際の押し戻しと接地は共通処理へ渡す。
    ApplyPhysicsCollision(info, attribute);

    // 面へ向かう速さがゼロ以下なら、離れている接触なので衝撃も反発も追加しない。
    if (normalSpeed <= 0.0f) {
        return;
    }

    // 初回と再着地で別のしきい値を使い、接触が連続しても発生回数と間隔を制限する。
    const float slamSpeedThreshold = slamImpactCount_ == 0 ? kThrownSlamMinSpeed : kThrownFollowUpSlamMinSpeed;
    if (isGroundLike && slamImpactCount_ < kMaxSlamImpacts && slamImpactCooldownTimer_ <= 0.0f &&
        normalSpeed >= slamSpeedThreshold) {
        ++slamImpactCount_;
        slamImpactCooldownTimer_ = kSlamImpactCooldown;
        OnSlamImpact(GetTranslate(), normalSpeed);
    }

    // 入射速度が小さい接触では押し戻しだけを残し、新しい反発速度は与えない。
    if (normalSpeed < kThrownBounceMinSpeed) {
        return;
    }

    // 面に沿う速度と法線方向への反発を分け、床と壁で減衰率を変える。
    const float normalDot = engine::Math::Dot(incomingVelocity, info.normal);
    const engine::Vector3 tangentVelocity = incomingVelocity - info.normal * normalDot;
    const float tangentKeep = isGroundLike ? 0.62f : 0.78f;
    const float bounceRate = isGroundLike ? 0.34f : 0.36f;

    // 滑りの残りと面から離れる速度を合成し、床では壁より両方を弱めます。
    SetVelocity(tangentVelocity * tangentKeep + info.normal * (normalSpeed * bounceRate));

    // 着地衝撃の余地が残る間は、小さい反跳でも地面から離れる高さを確保する。
    if (isGroundLike && GetVelocity().y < 3.2f && slamImpactCount_ < kMaxSlamImpacts) {
        SetVelocityY(3.2f);
    }
}

void BaseEnemy::EndThrownPhysics() {
    // 投擲状態を終え、起き上がりと巡回中心の再設定へ移る。
    if (!isThrownPhysics_) {
        return;
    }

    isThrownPhysics_ = false;
    thrownTimer_ = 0.0f;
    thrownSettleTimer_ = 0.0f;
    thrownAngularVelocity_ = {0.0f, 0.0f, 0.0f};
    StartThrowRecovery();
    ResetWanderOrigin();
}

void BaseEnemy::StartThrowRecovery() {
    // 着地時の向きから起き上がり姿勢へ補間する時計を開始する。
    throwRecoveryTimer_ = kThrowRecoveryDuration;
    throwRecoveryRotateTimer_ = 0.0f;
    throwRecoveryStartRotation_ = GetRotation();
    isThrowRotationRecovering_ = true;

    // すでに接地している場合にだけ、滑走の水平速度を止める。
    if (IsGrounded()) {
        SetVelocityX(0.0f);
        SetVelocityZ(0.0f);
    }
}

void BaseEnemy::UpdateThrowRecovery(float deltaTime) {
    // 起き上がりの途中では攻撃 AI を再開せず、姿勢の補間だけ進める。
    if (throwRecoveryTimer_ > 0.0f) {
        throwRecoveryTimer_ = (std::max)(0.0f, throwRecoveryTimer_ - deltaTime);
    }

    // 接地中に残った水平速度だけを落とし、空中ではこの摩擦を適用しない。
    if (IsGrounded()) {
        SetVelocityX(GetVelocity().x * (kThrowRecoveryGroundFriction));
        SetVelocityZ(GetVelocity().z * (kThrowRecoveryGroundFriction));
    }

    // 姿勢復帰が終了していれば、摩擦や待ち時間だけの更新を終えて戻る。
    if (!isThrowRotationRecovering_) {
        return;
    }

    // 衝突直後の向きから投げる前の向きへ、クォータニオンで姿勢を補間する。
    throwRecoveryRotateTimer_ += deltaTime;
    const float t = Easing::OutCubic01(throwRecoveryRotateTimer_ / kThrowRecoveryDuration);
    const engine::Quaternion start = engine::Math::EulerToQuaternion(throwRecoveryStartRotation_);
    const engine::Quaternion target = engine::Math::EulerToQuaternion(throwRecoveryTargetRotation_);
    const engine::Quaternion current = engine::Math::Slerp(start, target, t);
    SetRotation(engine::Math::MatrixToEuler(engine::Math::MakeRotateQuaternionMatrix(current)));

    // 時間を使い切ったら目的の角度に確定し、今後の姿勢更新から起き上がりを外す。
    if (throwRecoveryRotateTimer_ >= kThrowRecoveryDuration) {
        SetRotation(throwRecoveryTargetRotation_);
        isThrowRotationRecovering_ = false;
        throwRecoveryRotateTimer_ = 0.0f;
    }
}

// 投げ衝突の叩きつけダメージと演出
void BaseEnemy::OnSlamImpact(const engine::Vector3& impactPosition, float impactSpeed) {
    // 投擲の着地を自身の被害と周囲への衝撃に分けて通知する。
    // Bombは固有の着地処理に任せるため、自分への叩きつけダメージだけ省く。
    if (GetEnemyType() != "Bomb") {
        DamageEvent selfDamage;
        selfDamage.target = this;
        selfDamage.attacker = this;
        selfDamage.damageAmount = CalculateSlamDamage(impactSpeed);
        EventManager::GetInstance()->Dispatch(selfDamage);
    }

    SpawnSlamImpactEffect(impactPosition, impactSpeed);
    DamageSlamTargets(impactPosition, impactSpeed);
}

void BaseEnemy::SpawnSlamImpactEffect(const engine::Vector3& impactPosition, float impactSpeed) {
    // 衝撃の表示は敵の中心でなく接地面へ合わせ、落下高さによる浮きを防ぐ。
    const engine::Vector3 groundImpactPosition = HitEffectDirector::ResolveGroundEffectPosition(impactPosition);
    HitEffectDirector::SpawnThrowSlamShockwave(groundImpactPosition, impactSpeed);

    SceneManager* sceneManager = SceneManager::GetInstance();

    // 衝撃波は先に出す。シーンまたは粒子基盤が無ければ追加の土煙だけ省く。
    if (!sceneManager || !sceneManager->GetCurrentScene()) {
        return;
    }

    engine::ParticleSystem* particleSystem = sceneManager->GetCurrentScene()->GetParticleSystem();

    if (!particleSystem) {
        return;
    }

    engine::Vector3 up = {0.0f, 1.0f, 0.0f};
    const float power = (std::clamp)(impactSpeed / 22.0f, 0.65f, 1.35f);
    particleSystem->SpawnParticles(groundImpactPosition + engine::Vector3{0.0f, 0.04f, 0.0f}, static_cast<std::int32_t>(28.0f * power),
                                   5.5f * power, &up, 85.0f, {0.95f, 0.9f, 0.78f, 1.0f}, {0.55f, 0.48f, 0.4f, 0.0f},
                                   0.18f, 0.45f, 0.7f * power, 0.05f);
}

void BaseEnemy::DamageSlamTargets(const engine::Vector3& impactPosition, float impactSpeed) {
    // 着地点の近傍を一度だけ調べ、投げた敵以外へ速度に応じた威力を配る。
    engine::CollisionManager* collisionManager = engine::CollisionManager::GetInstance();

    if (!collisionManager) {
        return;
    }

    const float radius = kSlamRadiusBase + (std::clamp)((impactSpeed - kThrownSlamMinSpeed) * 0.06f, 0.0f, 1.8f);
    const float damage = CalculateSlamDamage(impactSpeed);

    // 判定候補から無効・自身・撃破済み・敵カテゴリ以外を除いて衝撃の距離を測る。
    for (engine::Object3D* object : collisionManager->GetObjects()) {
        if (!object || object == this || object->IsDead()) {
            continue;
        }

        if (!(object->GetCollisionAttribute() & engine::collisionCategories::Get("enemy"))) {
            continue;
        }

        // 高さ方向の距離を軽く扱い、衝撃の中心から遠い敵ほど水平の押し出しと威力を弱めます。
        engine::Vector3 diff = object->GetWorldPosition() - impactPosition;
        diff.y *= 0.45f;
        const float distance = engine::Math::Length(diff);

        // 高さを軽く扱っても衝撃半径を超える敵にはイベントを送らない。
        if (distance > radius) {
            continue;
        }

        engine::Vector3 knockbackDir = NormalizePlanarOr(diff, {0.0f, 0.0f, 1.0f});
        // 衝撃の中心で1、判定半径の端で0。端でも威力は55%残しますが、水平方向の吹き飛びはなくなります。
        const float distanceRate = 1.0f - (std::clamp)(distance / radius, 0.0f, 1.0f);

        DamageEvent damageEvent;
        damageEvent.target = object;
        damageEvent.attacker = this;
        damageEvent.damageAmount = damage * (0.55f + distanceRate * 0.45f);
        damageEvent.knockbackVelocity = {knockbackDir.x * (10.0f + impactSpeed * 0.22f) * distanceRate,
                                         7.0f + impactSpeed * 0.10f,
                                         knockbackDir.z * (10.0f + impactSpeed * 0.22f) * distanceRate};
        EventManager::GetInstance()->Dispatch(damageEvent);
    }
}

void BaseEnemy::UpdateDamageFeedbackTimers(float deltaTime) {
    // 多段接触でも被弾演出とダメージを過密に繰り返さないよう、各猶予を減らす。
    if (damageCooldownTimer_ > 0.0f) {
        damageCooldownTimer_ -= deltaTime;
    }

    // 赤みを表示する時間だけ色を上書きし、期限を跨いだときに既定色を戻す。
    if (colorResetTimer_ > 0.0f) {
        colorResetTimer_ -= deltaTime;
        SetColor({1.0f, defaultColor_.y * 0.32f + 0.12f, defaultColor_.z * 0.32f + 0.12f, defaultColor_.w});

        if (colorResetTimer_ <= 0.0f) {
            SetColor(defaultColor_);
        }
    }

    // 被弾姿勢の時計を減らし、被弾顔の表示条件を外れたらモデル上書きを解除する。
    if (damageReactionTimer_ > 0.0f) {
        damageReactionTimer_ = (std::max)(0.0f, damageReactionTimer_ - deltaTime);
    }
    if (hurtFaceModel_ && !IsHurtFaceActive()) {
        ClearHurtFaceModel();
    }
}

void BaseEnemy::PlayDamageReaction(engine::Object3D* attacker, const engine::Vector3& knockbackVelocity, float damage) {
    // 撃破演出中は新しいリアクションで姿勢を上書きしない。
    if (IsDead() || isDefeatEffectPlaying_ || isDefeatEffectFinished_) {
        ClearHurtFaceModel();
        return;
    }

    // 吹き飛び方向がなければ、攻撃元から離れる方向、次に自分の後方を使います。
    engine::Vector3 worldDirection = {knockbackVelocity.x, 0.0f, knockbackVelocity.z};
    float planarLength = std::sqrt(worldDirection.x * worldDirection.x + worldDirection.z * worldDirection.z);

    if (planarLength <= 0.001f && attacker) {
        worldDirection = GetWorldPosition() - attacker->GetWorldPosition();
        worldDirection.y = 0.0f;
        planarLength = std::sqrt(worldDirection.x * worldDirection.x + worldDirection.z * worldDirection.z);
    }

    if (planarLength <= 0.001f) {
        const float yaw = GetRotation().y;
        worldDirection = {-std::sin(yaw), 0.0f, -std::cos(yaw)};
        planarLength = 1.0f;
    }

    worldDirection.x /= planarLength;
    worldDirection.z /= planarLength;

    const float yaw = GetRotation().y;
    // 敵の前と右に投影し、固有アニメーションに重ねやすいローカル方向へ変換します。
    const engine::Vector3 localRight = {std::cos(yaw), 0.0f, -std::sin(yaw)};
    const engine::Vector3 localForward = {std::sin(yaw), 0.0f, std::cos(yaw)};
    damageReactionLocalDirection_ = {worldDirection.x * localRight.x + worldDirection.z * localRight.z, 0.0f,
                                     worldDirection.x * localForward.x + worldDirection.z * localForward.z};

    // 吹き飛びとダメージから変形の強さを分けて算出し、顔と赤い点滅の開始時刻を揃えます。
    const float knockbackStrength = std::clamp(planarLength / 18.0f, 0.0f, 1.0f);
    const float damageStrength = std::clamp(damage / 24.0f, 0.0f, 1.0f);
    damageReactionStrength_ = std::clamp(0.72f + knockbackStrength * 0.28f + damageStrength * 0.34f, 0.72f, 1.30f);
    damageReactionDuration_ =
        kDamageReactionMinDuration + (kDamageReactionMaxDuration - kDamageReactionMinDuration) * damageStrength;
    damageReactionTimer_ = damageReactionDuration_;
    colorResetTimer_ = (std::max)(colorResetTimer_, 0.11f);

    if (!IsHurtFaceActive()) {
        ClearHurtFaceModel();
        return;
    }

    // プリズムは別描画の目で同じ時間を使うため、ここでは本体を差し替えない。
    if (auto* renderer = GetMeshRenderer(); renderer &&
        (!renderer->GetVisualModelOverride() || renderer->GetVisualModelOverride() == hurtFaceModel_)) {
        if (const char* hurtModel = FindEnemyHurtFaceModel(GetModelName())) {
            hurtFaceModel_ = ModelManager::GetInstance()->LoadModel(hurtModel);
            if (hurtFaceModel_) {
                renderer->SetVisualModelOverride(hurtFaceModel_);
            }
        }
    }
}

// 被弾直後の短い期間で、生存・活動中の敵だけが専用の顔を表示できる。
bool BaseEnemy::IsHurtFaceActive() const {
    const auto& parameters = GetParameters();
    return damageReactionTimer_ > 0.0f && damageReactionDuration_ - damageReactionTimer_ < kEnemyHurtFaceSeconds &&
           !IsDead() && !isDormant_ && !isCarried_ && !isDefeatEffectPlaying_ && !isDefeatEffectFinished_ &&
           (!parameters || parameters->hp > 0.0f);
}

void BaseEnemy::ClearHurtFaceModel() {
    // 現在もこの処理が設定したモデルが描画中の場合だけ、描画側の上書きを解除する。
    if (auto* renderer = GetMeshRenderer(); hurtFaceModel_ && renderer &&
        renderer->GetVisualModelOverride() == hurtFaceModel_) {
        renderer->SetVisualModelOverride(nullptr);
    }
    hurtFaceModel_ = nullptr;
}

float BaseEnemy::GetDamageReactionWeight() const {
    // 被弾姿勢は時間経過で通常姿勢へ戻る重みとして返す。
    if (damageReactionDuration_ <= 0.0001f || damageReactionTimer_ <= 0.0f) {
        return 0.0f;
    }

    return std::clamp(damageReactionTimer_ / damageReactionDuration_, 0.0f, 1.0f);
}

EnemyVisualReactionPose BaseEnemy::GetDamageReactionPose() const {
    // ダメージ方向と残り時間から、各敵に重ねる共通の見た目補正を作る。
    EnemyVisualReactionPose pose;

    if (damageReactionDuration_ <= 0.0001f || damageReactionTimer_ <= 0.0f) {
        return pose;
    }

    const float progress = 1.0f - std::clamp(damageReactionTimer_ / damageReactionDuration_, 0.0f, 1.0f);
    float response = 0.0f;

    if (progress < 0.18f) {
        // 衝撃の直後は変形を立ち上げます。
        const float t = progress / 0.18f;
        response = Easing::SmoothStep(t);
    } else if (progress < 0.52f) {
        // 変形が最大になったあと逆方向へ戻して反動を見せます。
        const float t = (progress - 0.18f) / 0.34f;
        const float eased = Easing::SmoothStep(t);
        response = 1.0f + (-0.38f - 1.0f) * eased;
    } else {
        // 残り時間で揺れを減らし、通常姿勢へ戻します。
        const float t = (progress - 0.52f) / 0.48f;
        response = -0.38f * (1.0f - t) * std::cos(t * kWanderPi * 2.0f);
    }

    // ローカルの衝撃方向に応じて、縮む軸・傾き・移動量へ変換します。
    const float impulse = response * damageReactionStrength_;
    const float localX = damageReactionLocalDirection_.x;
    const float localZ = damageReactionLocalDirection_.z;
    const float alongX = std::abs(localX);
    const float alongZ = std::abs(localZ);

    pose.scale.x = 1.0f - impulse * 0.14f * alongX + impulse * 0.075f * alongZ;
    pose.scale.y = 1.0f + impulse * 0.075f;
    pose.scale.z = 1.0f - impulse * 0.14f * alongZ + impulse * 0.075f * alongX;
    pose.rotation.x = -localZ * impulse * 0.13f;
    pose.rotation.z = localX * impulse * 0.13f;
    pose.offset = {localX * impulse * 0.055f, std::abs(impulse) * 0.018f, localZ * impulse * 0.055f};
    pose.weight = std::abs(impulse);
    return pose;
}

void BaseEnemy::ApplyDamageReactionPose(engine::Vector3& scale, engine::Vector3& rotation, engine::Vector3* offset, float intensity) const {
    // 元のアニメーション姿勢へリアクションを加算し、敵固有の動きを消さない。
    const EnemyVisualReactionPose reaction = GetDamageReactionPose();
    const float safeIntensity = (std::max)(0.0f, intensity);
    scale.x *= 1.0f + (reaction.scale.x - 1.0f) * safeIntensity;
    scale.y *= 1.0f + (reaction.scale.y - 1.0f) * safeIntensity;
    scale.z *= 1.0f + (reaction.scale.z - 1.0f) * safeIntensity;
    rotation.x += reaction.rotation.x * safeIntensity;
    rotation.y += reaction.rotation.y * safeIntensity;
    rotation.z += reaction.rotation.z * safeIntensity;

    // オフセットの出力先が指定されている場合だけ、表示用の位置差分も加算する。
    if (offset) {
        offset->x += reaction.offset.x * safeIntensity;
        offset->y += reaction.offset.y * safeIntensity;
        offset->z += reaction.offset.z * safeIntensity;
    }
}

void BaseEnemy::EnsureNoticeMarkObject() {
    // 発見マークを初回だけ生成し、索敵の度にシーンへ増殖させない。
    if (noticeMarkObject_ || !GetObjectCommon()) {
        return;
    }

    noticeMarkObject_ = std::make_unique<engine::Object3D>();
    noticeMarkObject_->Initialize(GetObjectCommon());
    noticeMarkObject_->SetName("__EnemyNoticeMark");
    noticeMarkObject_->SetClassName("Effect");
    noticeMarkObject_->SetModel(kEnemyDetectedMarkModel);
    noticeMarkObject_->SetTexture("Resources/sprite/common/white.png");
    noticeMarkObject_->SetMaterialType(0);
    noticeMarkObject_->SetBlendMode(BlendMode::kNormal);
    noticeMarkObject_->SetColor({1.0f, 0.88f, 0.16f, 1.0f});
    noticeMarkObject_->SetEmissive(4.0f);
    noticeMarkObject_->SetCollisionAttribute(0);
    noticeMarkObject_->SetCollisionMask(0);
    noticeMarkObject_->SetIsVisible(false);
}

void BaseEnemy::ConfigureNoticeMarkForReaction() {
    // 気付き演出の開始時だけマークのサイズと表示位置を整える。
    EnsureNoticeMarkObject();

    // 生成を試しても部品が無ければ、見失い・発見用の素材や色は設定できない。
    if (!noticeMarkObject_) {
        return;
    }

    const bool isLost = noticeReactionKind_ == NoticeReactionKind::kLost;
    noticeMarkObject_->SetModel(isLost ? kEnemyLostMarkModel : kEnemyDetectedMarkModel);
    noticeMarkObject_->SetColor(isLost ? engine::Vector4{0.30f, 0.90f, 1.0f, 1.0f} : engine::Vector4{1.0f, 0.88f, 0.16f, 1.0f});
    noticeMarkObject_->SetEmissive(isLost ? 4.4f : 4.0f);
}

void BaseEnemy::BeginNoticeReaction(NoticeReactionKind kind) {
    // 発見・見失い中に一時停止して反応を見せる。反応の指定がなければ開始しない。
    if (kind == NoticeReactionKind::kNone) {
        return;
    }

    // 先行する反応があれば元の姿勢へ戻してから、新しい反応の基準を記録する。
    if (isNoticeReactionActive_) {
        EndNoticeReaction(true);
    }

    noticeBaseScale_ = GetScale();
    noticeBaseRotation_ = GetRotation();
    noticeBaseColor_ = GetColor();
    isNoticeReactionActive_ = true;
    noticeReactionKind_ = kind;
    noticeReactionTimer_ = kind == NoticeReactionKind::kLost ? kEnemyLostDuration : kEnemyNoticeDuration;

    // 発見に入った場合だけ再発見のクールダウンを設定する。
    if (kind == NoticeReactionKind::kDetected) {
        noticeReactionCooldown_ = kEnemyNoticeCooldown;
    }

    noticeMarkYaw_ = 0.0f;

    SetVelocity({0.0f, 0.0f, 0.0f});
    TriggerAttackTelegraphCue(kind == NoticeReactionKind::kLost ? engine::Vector4{0.20f, 0.82f, 1.0f, 0.82f}
                                                               : engine::Vector4{1.0f, 0.82f, 0.12f, 0.9f});
    ConfigureNoticeMarkForReaction();

    // 部品が生成済みなら、選んだ反応のマークを表示する。
    if (noticeMarkObject_) {
        noticeMarkObject_->SetIsVisible(true);
    }
}

void BaseEnemy::EndNoticeReaction(bool restoreVisual) {
    // スケールを復元し、見回しで向きを変えた場合だけ演出前の回転も戻します。
    const NoticeReactionKind finishedKind = noticeReactionKind_;

    if (restoreVisual && isNoticeReactionActive_) {
        SetScale(noticeBaseScale_);

        if (finishedKind == NoticeReactionKind::kLost) {
            SetRotation(noticeBaseRotation_);
        }
    }

    isNoticeReactionActive_ = false;
    noticeReactionKind_ = NoticeReactionKind::kNone;
    noticeReactionTimer_ = 0.0f;

    if (noticeMarkObject_) {
        noticeMarkObject_->SetIsVisible(false);
    }
}

void BaseEnemy::CancelNoticeReaction() {
    // 目標喪失時は再発見判定を戻し、古い反応を継続しない。
    EndNoticeReaction(true);
    wasTargetDetected_ = false;
    noticeReactionCooldown_ = 0.0f;
    noticeDetectionRange_ = detectionRange_;
}

void BaseEnemy::UpdateNoticeMark(float deltaTime, float progress) {
    // 本体の大きさに合わせてマークを配置し、敵ごとの差を吸収する。
    EnsureNoticeMarkObject();

    if (!noticeMarkObject_) {
        return;
    }

    const float bodyScale = MaxAbsScaleComponent(GetScale());
    const bool isLost = noticeReactionKind_ == NoticeReactionKind::kLost;
    const float pop =
        1.0f + std::sin((std::clamp)(progress / (isLost ? 0.32f : 0.55f), 0.0f, 1.0f) * kWanderPi) * 0.34f;
    const float hover = std::sin(progress * kWanderPi * (isLost ? 4.0f : 2.0f)) * (isLost ? 0.12f : 0.18f);
    const float markScale = (std::clamp)(bodyScale * 0.34f, 0.32f, 1.15f) * pop;

    engine::Vector3 position = GetWorldPosition();
    position.y += (std::clamp)(bodyScale * 1.45f, 1.10f, 4.80f) + hover;
    noticeMarkYaw_ += deltaTime * (isLost ? 0.75f : 1.6f);

    noticeMarkObject_->SetTranslate(position);
    noticeMarkObject_->SetScale({markScale, markScale, markScale});
    noticeMarkObject_->SetRotation({0.0f, noticeMarkYaw_, 0.0f});
    noticeMarkObject_->SetColor(isLost ? engine::Vector4{0.24f, 0.82f + std::sin(progress * kWanderPi) * 0.16f, 1.0f, 1.0f}
                                       : engine::Vector4{1.0f, 0.82f + std::sin(progress * kWanderPi) * 0.14f, 0.12f, 1.0f});
    noticeMarkObject_->SetIsVisible(true);
    noticeMarkObject_->UpdateLocalMatrix();
    noticeMarkObject_->UpdateWorldMatrix();
}

void BaseEnemy::UpdateBossTargetEngagement(float targetDistance) {
    // 一度接敵したボスは距離が離れても交戦開始の事実を保持する。
    if (target_ && targetDistance <= detectionRange_) {
        bossTargetEngaged_ = true;
    }
}

// 固有AIから受け取った検知距離で新規発見を判定し、反応がAIを止める間は true を返す。
bool BaseEnemy::UpdateNoticeReaction(float deltaTime, float targetDistance, float detectRange,
                                     const engine::Vector3& targetDirection) {
    (void)deltaTime;
    (void)targetDirection;

    noticeDetectionRange_ = (std::max)(0.0f, detectRange);
    const bool detected = target_ && (bossTargetEngaged_ || targetDistance <= noticeDetectionRange_);

    if (!detected) {
        // 見失いへの遷移は、固有AIの状態に左右されない共通Updateで一度だけ処理します。
        return isNoticeReactionActive_;
    }

    // 未発見から範囲内に入ったとき、見失い中または反応・再開待ちが無い場合に発見を開始する。
    if (!wasTargetDetected_) {
        if (noticeReactionKind_ == NoticeReactionKind::kLost ||
            (!isNoticeReactionActive_ && noticeReactionCooldown_ <= 0.0f)) {
            BeginNoticeReaction(NoticeReactionKind::kDetected);
        }

        wasTargetDetected_ = true;
    }

    return isNoticeReactionActive_;
}

// 共通更新で範囲外への遷移と、見失いの途中での再発見を判定する。
void BaseEnemy::UpdateNoticeAwareness(float deltaTime) {
    noticeReactionCooldown_ = (std::max)(0.0f, noticeReactionCooldown_ - deltaTime);

    const bool detected =
        target_ && (bossTargetEngaged_ || PlanarDistance(GetWorldPosition(), target_->GetWorldPosition()) <=
                                              (std::max)(0.0f, noticeDetectionRange_));

    // 発見済みから範囲外へ出たフレームに、見失いを一度だけ開始する。
    if (wasTargetDetected_ && !detected) {
        wasTargetDetected_ = false;
        BeginNoticeReaction(NoticeReactionKind::kLost);
    } else if (!wasTargetDetected_ && detected && noticeReactionKind_ == NoticeReactionKind::kLost) {
        // 見渡している最中に再発見した場合は、疑問符を残さず感知リアクションへ切り替えます。
        wasTargetDetected_ = true;
        BeginNoticeReaction(NoticeReactionKind::kDetected);
    }

    UpdateNoticeReactionAnimation(deltaTime);
}

void BaseEnemy::UpdateNoticeReactionAnimation(float deltaTime) {
    // 感知・見失い演出中は通常移動を止め、開始時の姿勢を基準に一時的な変形を重ねる。
    if (!isNoticeReactionActive_) {
        return;
    }

    const float duration = noticeReactionKind_ == NoticeReactionKind::kLost ? kEnemyLostDuration : kEnemyNoticeDuration;
    noticeReactionTimer_ = (std::max)(0.0f, noticeReactionTimer_ - deltaTime);
    const float progress = 1.0f - noticeReactionTimer_ / duration;
    const float clampedProgress = (std::clamp)(progress, 0.0f, 1.0f);

    SetVelocity({0.0f, 0.0f, 0.0f});

    // 見失い中は左右へ向き直り、発見中は向きを動かさず驚きの変形を加える。
    if (noticeReactionKind_ == NoticeReactionKind::kLost) {
        const float rampIn = (std::clamp)(clampedProgress / 0.12f, 0.0f, 1.0f);
        const float rampOut = (std::clamp)((1.0f - clampedProgress) / 0.15f, 0.0f, 1.0f);
        const float envelope = (std::min)(rampIn, rampOut);
        const float lookYaw = std::sin(clampedProgress * kWanderPi * 3.0f) * 0.62f * envelope;
        const float searchPulse = std::sin(clampedProgress * kWanderPi * 6.0f) * envelope;

        SetRotation({noticeBaseRotation_.x, noticeBaseRotation_.y + lookYaw, noticeBaseRotation_.z - lookYaw * 0.10f});
        SetScale({noticeBaseScale_.x * (1.0f + std::abs(searchPulse) * 0.035f),
                  noticeBaseScale_.y * (1.0f - std::abs(searchPulse) * 0.025f),
                  noticeBaseScale_.z * (1.0f + std::abs(searchPulse) * 0.035f)});
    } else {
        const float squash = std::sin(clampedProgress * kWanderPi);
        const float hopPop = std::sin(clampedProgress * kWanderPi * 2.0f) * 0.035f;
        SetScale({noticeBaseScale_.x * (1.0f + squash * 0.10f - hopPop),
                  noticeBaseScale_.y * (1.0f + squash * 0.12f + hopPop),
                  noticeBaseScale_.z * (1.0f + squash * 0.10f - hopPop)});
    }

    UpdateNoticeMark(deltaTime, clampedProgress);

    // 残り時間が無くなったら、マークを隠して反応の開始姿勢へ戻す。
    if (noticeReactionTimer_ <= 0.0f) {
        EndNoticeReaction(true);
    }
}

// HPが尽きた時の共通撃破演出
bool BaseEnemy::ShouldHandleDefeatEffect() const {
    // 終了済みと固有処理を持つBombは除外する。進行中、または生存状態でHPが尽きた敵を共通演出に回す。
    if (isDefeatEffectFinished_) {
        return false;
    }

    if (GetEnemyType() == "Bomb") {
        return false;
    }

    if (isDefeatEffectPlaying_) {
        return true;
    }

    return GetParameters().has_value() && GetParameters()->hp <= 0.0f && !IsDead();
}

engine::ParticleSystem* BaseEnemy::GetCurrentParticleSystem() const {
    // シーン切替中は描画先がないため、演出呼び出し側に欠落を伝える。
    SceneManager* sceneManager = SceneManager::GetInstance();

    if (!sceneManager || !sceneManager->GetCurrentScene()) {
        return nullptr;
    }

    return sceneManager->GetCurrentScene()->GetParticleSystem();
}

void BaseEnemy::SpawnDefeatCoinDrops() {
    // 死亡更新が複数フレーム続いてもドロップは一度だけ生成する。
    if (hasSpawnedDefeatCoinDrops_) {
        return;
    }

    hasSpawnedDefeatCoinDrops_ = true;

    SceneManager* sceneManager = SceneManager::GetInstance();
    BaseScene* scene = sceneManager ? sceneManager->GetCurrentScene() : nullptr;

    // 登録先シーンか生成基盤が無い場合、二重生成防止の記録だけを残して戻る。
    if (!scene || !scene->GetObject3DCommon()) {
        return;
    }

    const std::int32_t dropCount = GetCoinDropCountByEnemyType(GetEnemyType());

    // 専用報酬のみの敵などコイン数がゼロのときは何も配置しない。
    if (dropCount <= 0) {
        return;
    }

    // 本体の大きさから生成高を、周囲の地形からコインが落ちる地面高を決める。
    const engine::Vector3 basePosition = GetWorldPosition();
    const engine::Vector3 baseScale = GetScale();
    const float rawBodyScale = (std::max)({0.8f, std::abs(baseScale.x), std::abs(baseScale.y), std::abs(baseScale.z)});
    const float bodyScale =
        GetEnemyType() == "GiantSlime" ? std::min(rawBodyScale, 3.6f) : std::min(rawBodyScale, 1.45f);
    const float groundY = GroundEffectLocator::ResolveGroundPosition(basePosition).y;
    const float spawnY = basePosition.y + (std::max)(0.65f, bodyScale * 0.32f);

    uint32_t seed = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this));
    seed ^= static_cast<uint32_t>((std::abs(basePosition.x) + 13.0f) * 8191.0f);
    seed ^= static_cast<uint32_t>((std::abs(basePosition.z) + 7.0f) * 131071.0f);

    if (seed == 0u) {
        seed = 0xC01A5EEDu;
    }

    // 外周に等間隔の起点を取り、小さな乱数差を加えた方向と初速で一枚ずつ飛ばす。
    for (std::int32_t i = 0; i < dropCount; ++i) {
        auto coin = std::make_unique<GimmickCoin>();
        coin->Initialize(scene->GetObject3DCommon(), kEnemyDropCoinModel);

        const float angleBase = (kWanderPi * 2.0f / static_cast<float>(dropCount)) * static_cast<float>(i);
        const float angle = angleBase + (NextDropRandom(seed) - 0.5f) * 0.72f;
        const float radialOffset = 0.18f + NextDropRandom(seed) * 0.24f;
        const float speed = 4.2f + NextDropRandom(seed) * 2.8f;
        const float upSpeed = 6.4f + NextDropRandom(seed) * 2.7f;

        engine::Vector3 spawnPosition = {basePosition.x + std::sin(angle) * radialOffset, spawnY,
                                 basePosition.z + std::cos(angle) * radialOffset};
        engine::Vector3 velocity = {std::sin(angle) * speed, upSpeed, std::cos(angle) * speed};

        coin->SetName("DropCoin_" + GetEnemyType() + "_" + std::to_string(i));
        coin->SetTranslate(spawnPosition);
        coin->SetScale(kEnemyDropCoinScale);
        coin->SetCollisionRadius(kEnemyDropCoinWorldCollectRadius / kEnemyDropCoinScale.x);
        coin->ConfigureTemporaryDrop(velocity, kEnemyDropCoinLifetime, kEnemyDropCoinBlinkStart, groundY);
        scene->AddObject(std::move(coin));
    }
}

void BaseEnemy::BeginDefeatEffect() {
    // 撃破中の接触・運搬を止め、元の姿勢と色を退場アニメーション用に保存する。
    CancelNoticeReaction();
    isDefeatEffectPlaying_ = true;
    isDefeatEffectFinished_ = false;
    SpawnDefeatCoinDrops();
    defeatEffectTimer_ = 0.0f;
    defeatEffectParticleTimer_ = 0.0f;
    defeatBasePosition_ = GetTranslate();
    defeatBaseScale_ = GetScale();
    defeatBaseColor_ = GetColor();

    // 演出中の物理、運搬、通常の接触を止め、退場用の発光と描画設定に切り替える。
    isCarried_ = false;
    isThrownPhysics_ = false;
    isThrowRotationRecovering_ = false;
    throwRecoveryTimer_ = 0.0f;
    thrownTimer_ = 0.0f;
    thrownSettleTimer_ = 0.0f;
    slamImpactCooldownTimer_ = 0.0f;
    SetVelocity({0.0f, 0.0f, 0.0f});
    SetCollisionAttribute(0);
    SetCollisionMask(0);
    SetSelectedLighting(2);
    SetMaterialType(4);
    SetBlendMode(BlendMode::kNormal);
    SetEmissive(2.4f);
    SetColor({1.0f, 0.96f, 0.72f, 1.0f});

    // 身体の中心寄りで開始の光を出し、メッシュ描画基盤があれば地面寄りのリングも出す。
    engine::Vector3 cuePosition = GetWorldPosition();
    cuePosition.y += (std::max)(0.42f, defeatBaseScale_.y * 0.48f);
    VfxSequencer::PlayOneShot(kEnemyDefeatPopSequence, cuePosition);

    const float rawBodyScale =
        (std::max)({0.9f, std::abs(defeatBaseScale_.x), std::abs(defeatBaseScale_.y), std::abs(defeatBaseScale_.z)});
    const float bodyScale =
        GetEnemyType() == "GiantSlime" ? std::min(rawBodyScale, 3.6f) : std::min(rawBodyScale, 1.45f);
    if (auto* meshEffect = MeshEffectManager::GetInstance()) {
        meshEffect->SpawnEffectAt(kEnemyDefeatCoreEffect, cuePosition, {0.0f, 0.0f, 0.0f},
                                  {bodyScale, bodyScale, bodyScale});

        engine::Vector3 ringPosition = GetWorldPosition();
        ringPosition.y += (std::max)(0.12f, defeatBaseScale_.y * 0.12f);
        meshEffect->SpawnEffectAt(kEnemyDefeatRingEffect, ringPosition, {0.0f, 0.0f, 0.0f},
                                  {bodyScale, bodyScale, bodyScale});
    }

    SpawnDefeatStartParticles();
}

void BaseEnemy::UpdateDefeatEffect(float deltaTime) {
    // 保存した姿勢から縮小・上昇・発光を進め、終了時にイベント通知と除去を行う。
    defeatEffectTimer_ += deltaTime;
    defeatEffectParticleTimer_ -= deltaTime;

    const float progress = (std::clamp)(defeatEffectTimer_ / kDefeatEffectDuration, 0.0f, 1.0f);
    const float eased = Easing::OutCubic01(progress);
    const float pulse = 1.0f + std::sin(progress * kWanderPi * 6.0f) * 0.06f * (1.0f - progress);
    const float shrink = 1.0f - eased * 0.34f;

    // 退場開始時の大きさを基準に縮め、序盤だけ脈動を重ねます。
    engine::Vector3 scale = defeatBaseScale_;
    scale.x *= shrink * pulse;
    scale.z *= shrink * pulse;
    scale.y *= (1.0f - eased * 0.18f) * (1.0f + std::sin(progress * kWanderPi * 3.0f) * 0.10f * (1.0f - progress));
    SetScale(scale);

    // 高度は開始位置から補間し、回転はこのフレームの時間分だけ進めます。
    engine::Vector3 position = defeatBasePosition_;
    position.y += kDefeatRiseHeight * eased;
    SetTranslate(position);

    engine::Vector3 rotation = GetRotation();
    rotation.y += (kDefeatSpinSpeed + progress * 4.0f) * deltaTime;
    rotation.x += 0.55f * (1.0f - progress) * deltaTime;
    SetRotation(rotation);

    // 発光色を明るくしながら、ディゾルブのしきい値を下げて消していきます。
    const float dissolveThreshold = 1.0f - (Easing::SmoothStep(progress));
    const engine::Vector3 glowColor = {defeatBaseColor_.x * (1.0f - eased) + 1.0f * eased,
                               defeatBaseColor_.y * (1.0f - eased) + 0.92f * eased,
                               defeatBaseColor_.z * (1.0f - eased) + 0.48f * eased};
    SetColor({glowColor.x, glowColor.y, glowColor.z, dissolveThreshold});

    // 粒子の間隔が過ぎており、終了直前でなければ退場中の粒子を足す。
    if (defeatEffectParticleTimer_ <= 0.0f && progress < 0.96f) {
        SpawnDefeatLoopParticles();
        defeatEffectParticleTimer_ = kDefeatParticleInterval;
    }

    // 最後に撃破の通知を送り、シーンの安全な除去時点へこの実体を渡します。
    if (progress >= 1.0f) {
        isDefeatEffectPlaying_ = false;
        isDefeatEffectFinished_ = true;
        SetDead(true);
        SetIsVisible(false);

        // 終了時にシーンがあれば、正のイベントIDを通知してから実体の除去を予約する。
        if (BaseScene* scene = SceneManager::GetInstance() ? SceneManager::GetInstance()->GetCurrentScene() : nullptr) {
            const std::int32_t defeatEventId = GetTargetId();

            if (defeatEventId > 0) {
                scene->TriggerEvent(defeatEventId);
            }

            scene->RequestRemoveObject(this);
        }
    }
}

void BaseEnemy::SpawnDefeatStartParticles() {
    // 本体スケールで煙と閃光の広がりを調整し、小型敵と大型敵で演出を共用する。
    engine::ParticleSystem* particleSystem = GetCurrentParticleSystem();

    if (!particleSystem) {
        return;
    }

    engine::Vector3 center = GetWorldPosition();
    center.y += (std::max)(0.35f, defeatBaseScale_.y * 0.45f);
    engine::Vector3 up = {0.0f, 1.0f, 0.0f};
    const float rawBodyScale =
        (std::max)({0.9f, std::abs(defeatBaseScale_.x), std::abs(defeatBaseScale_.y), std::abs(defeatBaseScale_.z)});
    const float bodyScale =
        GetEnemyType() == "GiantSlime" ? std::min(rawBodyScale, 3.6f) : std::min(rawBodyScale, 1.45f);

    const engine::Vector3 smokeOffsets[] = {{0.0f, 0.0f, 0.0f},
                                    {0.42f, 0.08f, 0.10f},
                                    {-0.38f, 0.16f, -0.08f},
                                    {0.12f, 0.34f, -0.36f},
                                    {-0.18f, 0.52f, 0.30f}};

    for (const engine::Vector3& offset : smokeOffsets) {
        engine::Vector3 smokePos = center + offset * bodyScale;
        particleSystem->SpawnParticles(smokePos, 8, 0.85f + offset.y * 0.28f, &up, 0.9f, {1.0f, 0.96f, 0.82f, 0.90f},
                                       {0.74f, 0.72f, 0.60f, 0.0f}, 0.46f, 0.82f, 0.34f * bodyScale, 1.05f * bodyScale);
    }

    particleSystem->SpawnParticles(center, 30, 4.9f, nullptr, 0.0f, {1.0f, 0.86f, 0.24f, 1.0f},
                                   {0.52f, 1.0f, 1.0f, 0.0f}, 0.22f, 0.62f, 0.22f * bodyScale, 0.02f);
    particleSystem->SpawnParticles(center, 16, 2.4f, &up, 1.0f, {1.0f, 1.0f, 0.92f, 0.95f}, {1.0f, 0.72f, 0.20f, 0.0f},
                                   0.18f, 0.48f, 0.20f * bodyScale, 0.0f);
}

void BaseEnemy::SpawnDefeatLoopParticles() {
    // 退場中も粒子を補い、上昇する本体から煙が途切れないようにする。
    engine::ParticleSystem* particleSystem = GetCurrentParticleSystem();

    if (!particleSystem) {
        return;
    }

    const float progress = (std::clamp)(defeatEffectTimer_ / kDefeatEffectDuration, 0.0f, 1.0f);
    engine::Vector3 center = GetWorldPosition();
    center.y += (std::max)(0.2f, defeatBaseScale_.y * (0.2f + progress * 0.45f));
    engine::Vector3 up = {0.0f, 1.0f, 0.0f};
    const float rawBodyScale =
        (std::max)({0.9f, std::abs(defeatBaseScale_.x), std::abs(defeatBaseScale_.y), std::abs(defeatBaseScale_.z)});
    const float bodyScale =
        GetEnemyType() == "GiantSlime" ? std::min(rawBodyScale, 3.6f) : std::min(rawBodyScale, 1.45f);
    particleSystem->SpawnParticles(center, 7, 1.05f + progress * 0.95f, &up, 0.78f, {1.0f, 0.96f, 0.82f, 0.72f},
                                   {0.72f, 0.72f, 0.62f, 0.0f}, 0.28f, 0.62f, 0.24f * bodyScale, 0.72f * bodyScale);
    particleSystem->SpawnParticles(center, 5, 2.2f + progress * 0.7f, nullptr, 0.0f, {0.75f, 1.0f, 0.98f, 0.86f},
                                   {1.0f, 0.44f, 0.92f, 0.0f}, 0.14f, 0.32f, 0.12f * bodyScale, 0.0f);
}

void BaseEnemy::SetDormant(bool dormant) {
    // 休眠に入るフレームで予兆と感知演出を消し、残留速度による移動を防ぐ。
    isDormant_ = dormant;

    if (dormant) {
        HideAttackTelegraph();
        CancelNoticeReaction();
        SetVelocity({0.0f, 0.0f, 0.0f});
    }
}

// 個別AIが速度と状態を決めた後の共通更新です。休眠、撃破、運搬は通常物理より優先します。
void BaseEnemy::Update(float deltaTime) {
    // 共通状態の分岐へ入る前に、表示期限を過ぎた被弾モデルを外す。
    if (hurtFaceModel_ && !IsHurtFaceActive()) {
        ClearHurtFaceModel();
    }
    // 休眠中は予兆と移動を止め、描画に必要な Object3D の更新だけを進める。
    if (isDormant_) {
        HideAttackTelegraph();
        SetVelocity({0.0f, 0.0f, 0.0f});
        engine::Object3D::Update(deltaTime);
        return;
    }

    // 予兆が生成済みの敵だけ、表示や通知の残り時間を進める。
    if (attackTelegraph_) {
        attackTelegraph_->Update(deltaTime);
    }

    // 撃破演出に入った敵は予兆を消し、演出の開始と更新を通常物理より優先する。
    if (ShouldHandleDefeatEffect()) {
        HideAttackTelegraph();

        // 撃破へ移る初回だけ報酬と開始時の基準姿勢を確定する。以後は演出を進める。
        if (!isDefeatEffectPlaying_) {
            BeginDefeatEffect();
        }

        UpdateDefeatEffect(deltaTime);
        engine::Object3D::Update(deltaTime);
        return;
    }

    if (isCarried_) {
        HideAttackTelegraph();
        return; // 運搬中の位置と時間はプレイヤー側が管理します。
    }

    // 調整用JSONの検知距離を、共通の見渡し処理へ反映します。
    if (GetParameters().has_value()) {
        detectionRange_ = GetParameters()->detectionRange;
    }

    // 固有AIが追跡・攻撃状態へ移っていても、範囲外への遷移と見渡しを共通で更新します。
    UpdateNoticeAwareness(deltaTime);

    // 投げられている間は投擲用の速度で物理を動かし、通常の移動補正は通さない。
    if (isThrownPhysics_) {
        UpdateThrownPhysics(deltaTime);
        Character::Update(deltaTime);
        UpdateDamageFeedbackTimers(deltaTime);
        return;
    }

    // 投擲復帰とナビ補正を終えてから Character の物理を進めます。
    if (throwRecoveryTimer_ > 0.0f || isThrowRotationRecovering_) {
        UpdateThrowRecovery(deltaTime);
    }

    ApplyNavigationSteering(deltaTime);
    Character::Update(deltaTime);
    UpdateDamageFeedbackTimers(deltaTime);
}

void BaseEnemy::ApplyNavigationSteering(float deltaTime) {
    // 追従対象がいないときは、個別AIが設定した速度を使う。
    if (!target_) {
        return;
    }

    NavAgentComponent* navAgent = GetNavAgentComponent();
    // 経路探索を設定した敵にだけ補正を掛ける。
    if (!navAgent || !navAgent->IsEnabled()) {
        return;
    }

    engine::Vector3 velocity = GetVelocity();
    const float planarSpeed = std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
    // 水平速度がほぼゼロなら、新たに経路方向へ動かさない。
    if (!(planarSpeed > 0.01f)) {
        return;
    }

    const engine::Vector3 direction =
        navAgent->CalculateDirection(this, GetWorldPosition(), target_->GetWorldPosition(), deltaTime);
    velocity.x = direction.x * planarSpeed;
    velocity.z = direction.z * planarSpeed;
    SetVelocity(velocity);
}

void BaseEnemy::Draw(ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource) {
    // 感知マークは本体とは別オブジェクトなので、可視状態のときだけ追加で描画する。
    Character::Draw(pointLightResource, spotLightResource);

    if (noticeMarkObject_ && noticeMarkObject_->GetIsVisible()) {
        noticeMarkObject_->Draw(pointLightResource, spotLightResource);
    }
}

void BaseEnemy::DrawAttackTelegraph(ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource) {
    // 本体が非表示の敵では、床の予兆だけが残らないよう描画を抑える。
    if (GetIsRenderVisible() && attackTelegraph_) {
        attackTelegraph_->DrawGround(pointLightResource, spotLightResource);
        attackTelegraph_->DrawWarning(pointLightResource, spotLightResource);
    }
}

void BaseEnemy::ShowAttackTelegraphCircle(const engine::Vector3& center, float radius, float progress, const engine::Vector4& color) {
    // 通常は Initialize() で用意した予兆を使い、未生成なら共通描画資源から補います。
    if (!attackTelegraph_ && GetObjectCommon()) {
        attackTelegraph_ = std::make_unique<AttackTelegraph>();
        attackTelegraph_->Initialize(GetObjectCommon());
    }

    // 用意できた予兆へ中心・半径・進行率を渡す。資源が無ければ表示しない。
    if (attackTelegraph_) {
        attackTelegraph_->ShowCircle(center, radius, progress, color);
    }
}

void BaseEnemy::ShowAttackTelegraphDecalCircle(const engine::Vector3& center, float radius, float progress,
                                               const engine::Vector4& color, const std::string& texturePath) {
    // テクスチャ付き円も通常の予兆と同じ描画資源を遅延生成して使う。
    if (!attackTelegraph_ && GetObjectCommon()) {
        attackTelegraph_ = std::make_unique<AttackTelegraph>();
        attackTelegraph_->Initialize(GetObjectCommon());
    }

    // 予兆を確保できた場合だけ、指定テクスチャのデカールを出す。
    if (attackTelegraph_) {
        attackTelegraph_->ShowDecalCircle(center, radius, progress, color, texturePath);
    }
}

void BaseEnemy::ShowAttackTelegraphLine(const engine::Vector3& center, const engine::Vector3& direction, float length, float width,
                                        float progress, const engine::Vector4& color) {
    // 共通の予兆描画で、指定された進行方向と幅の線を表示します。
    // 予兆が未生成なら共通描画基盤から補い、基盤が無ければ線を描かない。
    if (!attackTelegraph_ && GetObjectCommon()) {
        attackTelegraph_ = std::make_unique<AttackTelegraph>();
        attackTelegraph_->Initialize(GetObjectCommon());
    }

    if (attackTelegraph_) {
        attackTelegraph_->ShowLine(center, direction, length, width, progress, color);
    }
}

void BaseEnemy::TriggerAttackTelegraphCue(const engine::Vector4& color) {
    // 予兆資源を使えるときに、現在の体格に合わせた頭上の警告を出す。
    if (!attackTelegraph_ && GetObjectCommon()) {
        attackTelegraph_ = std::make_unique<AttackTelegraph>();
        attackTelegraph_->Initialize(GetObjectCommon());
    }

    if (attackTelegraph_) {
        const engine::Vector3 scale = GetScale();
        const float horizontalScale = (std::max)({0.75f, std::abs(scale.x), std::abs(scale.z)});
        const float verticalScale = (std::max)(0.75f, std::abs(scale.y));

        // 雷スライムなど横潰れする敵は現在Scaleが極端に広がるため、見た目上の体格へ補正します。
        const float bodyScale = (std::min)(horizontalScale, (std::max)(1.5f, verticalScale * 2.4f));
        const engine::Vector3 bodyOffset = {0.0f, (std::clamp)(verticalScale * 0.62f, 0.55f, 3.8f), 0.0f};

        // 床予兆が既に見えている場合だけ床を強調する。頭上の警告は床の有無に関係なく出す。
        if (attackTelegraph_->IsVisible()) {
            attackTelegraph_->TriggerCue(color);
        }

        attackTelegraph_->TriggerWarningCue(this, bodyOffset, (std::clamp)(bodyScale * 1.18f, 1.0f, 5.2f), color);
    }
}

void BaseEnemy::ShowAttackTelegraphCone(const engine::Vector3& origin, const engine::Vector3& direction, float length, float startWidth,
                                        float endWidth, float progress, const engine::Vector4& color) {
    // 扇形攻撃の向きと幅は呼び出し元のAIが決め、共通予兆へ渡す。
    // 未生成で共通描画基盤がある場合は予兆を補い、使用可能になったときに扇を出す。
    if (!attackTelegraph_ && GetObjectCommon()) {
        attackTelegraph_ = std::make_unique<AttackTelegraph>();
        attackTelegraph_->Initialize(GetObjectCommon());
    }

    if (attackTelegraph_) {
        attackTelegraph_->ShowCone(origin, direction, length, startWidth, endWidth, progress, color);
    }
}

void BaseEnemy::ShowAttackTelegraphImpactAreas(const engine::Vector3* centers, std::size_t centerCount, float radius,
                                               float progress, const engine::Vector4& color) {
    // 複数着弾点を一つの予兆にまとめ、各攻撃側で個別オブジェクトを持たずに済ませる。
    if (!attackTelegraph_ && GetObjectCommon()) {
        attackTelegraph_ = std::make_unique<AttackTelegraph>();
        attackTelegraph_->Initialize(GetObjectCommon());
    }

    // 共通の描画資源を用意できた場合に、中心配列と件数をまとめて渡す。
    if (attackTelegraph_) {
        attackTelegraph_->ShowImpactAreas(centers, centerCount, radius, progress, color);
    }
}

void BaseEnemy::ShowAttackTelegraphReadableImpactAreas(const engine::Vector3* centers, std::size_t centerCount, float radius,
                                                       float progress, const engine::Vector4& color) {
    // 着弾点が重なる攻撃でも見分けられる描画形式を共通予兆へ委譲する。
    if (!attackTelegraph_ && GetObjectCommon()) {
        attackTelegraph_ = std::make_unique<AttackTelegraph>();
        attackTelegraph_->Initialize(GetObjectCommon());
    }

    // 予兆を確保できなければ共通処理を呼ばず、着弾点の表示を省く。
    if (attackTelegraph_) {
        attackTelegraph_->ShowReadableImpactAreas(centers, centerCount, radius, progress, color);
    }
}

void BaseEnemy::ShowAttackTelegraphLaneFan(const engine::Vector3& origin, const engine::Vector3& direction, float length, float width,
                                           std::int32_t laneCount, float lateralSpacing, float angleStep, float progress,
                                           const engine::Vector4& color) {
    // レーンの数・幅・角度差と軸方向を受け取り、描画資源が使える場合に床へ予兆を表示する。
    if (!attackTelegraph_ && GetObjectCommon()) {
        attackTelegraph_ = std::make_unique<AttackTelegraph>();
        attackTelegraph_->Initialize(GetObjectCommon());
    }

    if (attackTelegraph_) {
        attackTelegraph_->ShowLaneFan(origin, direction, length, width, laneCount, lateralSpacing, angleStep, progress,
                                      color);
    }
}

void BaseEnemy::HideAttackTelegraph() {
    // 攻撃中断・休眠・運搬などの経路から同じ後片付けを呼べるようにする。
    if (attackTelegraph_) {
        attackTelegraph_->Hide();
    }
}

float BaseEnemy::GetActiveBodyAttackDamage() const {
    // 中断前のAI状態が残っていても、演出中・運搬中の本体は攻撃にしません。
    if (IsDead() || isDormant_ || isCarried_ || IsThrowRecovering() || isDefeatEffectPlaying_ ||
        (GetParameters().has_value() && GetParameters()->hp <= 0.0f)) {
        return 0.0f;
    }

    return (std::max)(0.0f, GetBodyAttackDamage());
}

// 有効な接触を攻撃または地形に振り分け、接触を受理したときに true を返す。
bool BaseEnemy::OnCollision(engine::Object3D* other) {
    // 休眠中か接触相手が無い場合は、衝突処理を始めない。
    if (isDormant_ || !other) {
        return false;
    }

    // 運搬中の接触は処理せず、属性とマスクも引き続き無効にする。
    if (isCarried_) {
        SetCollisionAttribute(0);
        SetCollisionMask(0);
        return false;
    }

    uint32_t attribute = other->GetCollisionAttribute();
    engine::CollisionInfo info = CheckCollision(other);

    // 判定結果が非接触なら、属性が一致していてもイベントや押し戻しを行わない。
    if (!info.isColliding) {
        return false;
    }

    // ========================================================
    // プレイヤーの攻撃（剣など）に当たった時の処理
    // ========================================================
    if (attribute & engine::collisionCategories::Get("player_attack")) {

        // 同じ相手へ接触が続く間のダメージ通知を短時間だけ間引きます。
        if (damageCooldownTimer_ > 0.0f) {
            return true;
        }

        // ダメージイベントの発行
        DamageEvent dmgEvent;
        dmgEvent.target = this;
        dmgEvent.attacker = other;

        // Bulletなら威力・属性・状態異常をコピーし、それ以外の攻撃では固定威力を送る。
        if (const Bullet* attackBullet = dynamic_cast<const Bullet*>(other)) {
            dmgEvent.damageAmount = attackBullet->GetDamage();
            dmgEvent.damageType = attackBullet->GetDamageType();
            dmgEvent.statusEffect = attackBullet->GetStatusEffect();
        } else {
            dmgEvent.damageAmount = 10.0f;
        }

        EventManager::GetInstance()->Dispatch(dmgEvent);

        damageCooldownTimer_ = 0.5f; // player_attack の持続接触では、0.5秒間再通知しない。
        colorResetTimer_ = 0.15f;    // 0.15秒間だけ赤くする

        SetColor({1.0f, 0.0f, 0.0f, 1.0f});

        return true;
    }

    // 地面や壁（ground）なら、物理的な押し戻しを実行
    if (attribute & engine::collisionCategories::Get("ground")) {
        // 投擲中だけ着地衝撃と反発を計算し、通常は地形の押し戻しと接地処理を使う。
        if (isThrownPhysics_) {
            HandleThrownCollision(info, attribute);
        } else {
            ApplyPhysicsCollision(info, attribute);
        }
    }

    return true;
}

// ==========================================
// 持ち運び状態の切り替え
// ==========================================
void BaseEnemy::SetCarried(bool isCarried) {
    const bool wasCarried = isCarried_;
    isCarried_ = isCarried;

    if (isCarried_) {
        // 捕獲が始まったら前の投擲と警戒を止め、速度や接触をプレイヤー側の管理へ譲ります。
        CancelNoticeReaction();
        isThrownPhysics_ = false;
        isThrowRotationRecovering_ = false;
        slamImpactCount_ = 0;
        slamImpactCooldownTimer_ = 0.0f;
        thrownTimer_ = 0.0f;
        thrownSettleTimer_ = 0.0f;
        throwRecoveryRotateTimer_ = 0.0f;
        thrownAngularVelocity_ = {0.0f, 0.0f, 0.0f};
        throwRecoveryTargetRotation_ = GetRotation();

        // 運搬中の敵を攻撃や地形の判定候補から外します。
        SetCollisionAttribute(0);
        SetCollisionMask(0);

        // 運搬中の位置更新はプレイヤー側が担当するため、敵の残留速度を消す。
        SetVelocity({0.0f, 0.0f, 0.0f});
    } else {
        // 敵としての接触を戻し、実際に運搬を解除した場合だけ復帰猶予と巡回の再設定を行います。
        SetCollisionAttribute(engine::collisionCategories::Get("enemy"));

        if (wasCarried) {
            throwRecoveryTimer_ = 0.75f;
            ResetWanderOrigin();
        }

        SetCollisionMask(engine::collisionCategories::Get("player") | engine::collisionCategories::Get("ground") | engine::collisionCategories::Get("player_bullet") | engine::collisionCategories::Get("player_attack")); // 通常の対プレイヤー・地形・プレイヤー攻撃の接触を復帰させる。
    }
}

void BaseEnemy::ExecuteAbility(Player* player) {
    // コピー能力を持たない敵の既定動作は何もしない。必要な敵だけで上書きする。
    (void)player;
}

void BaseEnemy::UpdateCarriedAbility(Player* player, float deltaTime) {
    // 運搬中の固有能力を持たない敵は共通更新に追加処理を行わない。
    (void)player;
    (void)deltaTime;
}
