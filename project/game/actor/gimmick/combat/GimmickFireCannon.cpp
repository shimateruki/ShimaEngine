#define NOMINMAX
#include "GimmickFireCannon.h"
#include "Easing.h"
#include <numbers>
#include "ObjectPropertyDefinition.h"
#include "game/system/scene/SceneGameplayAccess.h"
#include "BaseScene.h"
#include "game/system/combat/BulletManager.h"
#include "CollisionConfig.h"
#include "CollisionManager.h"
#include "MeshEffectManager.h"
#include "GpuParticleManager.h"
#include "Player.h"
#include "SceneManager.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kDefaultProjectileSpeed = 13.0f;
constexpr float kDefaultFireInterval = 1.35f;
constexpr float kDefaultProjectileRadius = 0.55f;
constexpr float kDefaultProjectileLifetime = 2.0f;
constexpr float kDefaultDetectionRange = 45.0f;
constexpr float kDefaultTurnSpeedDegrees = 360.0f;
} // namespace

// 砲台への参照が残らないよう、この砲台だけのプレビュー弾を破棄する。
GimmickFireCannon::~GimmickFireCannon() {
    BulletManager::GetInstance()->ClearPreviewForOwner(this);
}

// 地形としての当たり判定と着弾予告を準備し、4種類の発射モードを編集できるようにする。
void GimmickFireCannon::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    BaseGimmick::Initialize(common, modelName);

    // 砲台本体は地形扱い、発射する弾は BulletManager 側で別管理します。
    landingWarning_.Initialize(common);
    SetClassName("Gimmick");
    SetGimmickType("FireCannon");
    SetName("Gimmick_FireCannon");
    SetModel(modelName);
    SetColor({0.22f, 0.11f, 0.08f, 1.0f});
    SetRoughness(0.65f);
    SetMetallic(0.05f);
    SetEmissive(1.1f);
    SetScale({0.55f, 0.55f, 1.1f});
    SetCollisionAttribute(engine::collisionCategories::Get("ground"));
    SetCollisionMask(engine::collisionCategories::GetDefaultGameplayMask());
    SetStatic(false);

    engine::Object3D::ColliderConfig colConfig;
    colConfig.type = engine::ColliderType::kObb;
    colConfig.size = {1.0f, 1.0f, 1.0f};
    SetColliderConfig(colConfig);

    // 砲撃の編集値を持たない旧配置でも、既定値を保存できるようにする。
    if (!GetParameters().has_value()) {
        ResetParameters();
    }

    SetParameter(&engine::EntityParameter::speed, kDefaultProjectileSpeed);
    SetParameter(&engine::EntityParameter::interval, kDefaultFireInterval);
    SetParameter(&engine::EntityParameter::moveAmount, kDefaultProjectileRadius);
    SetParameter(&engine::EntityParameter::moveSpeed, kDefaultTurnSpeedDegrees);
    SetParameter(&engine::EntityParameter::detectionRange, kDefaultDetectionRange);
    SetParameter(&engine::EntityParameter::projectileLifetime, kDefaultProjectileLifetime);
    SetParameter(&engine::EntityParameter::actionMode, 1);
    SetParameter(&engine::EntityParameter::startActive, true);
    SetParameter(&engine::EntityParameter::returnOnOff, true);

    // 発射方向のモードごとに、索敵、旋回、弾道、予告や反動の欄を切り替える。
    using Self = GimmickFireCannon;
    engine::ObjectPropertyDefinition<Self> properties(*this, kTypeId, "火球砲台",
        "編集プレビューは無害で最短0.8秒間隔、同時数制限付きです。放物線は予告時に着弾点を固定します。大型砲弾は砲口方向（読込後の-Z）へ直進し、衝突で早く消えることがあります。通常の前方固定はローカルZ+です。");
    using Parameter = engine::Object3D::EntityParameter;
    properties.Parameter("発射方向", "actionMode", &Parameter::actionMode, 0.0f, 3.0f,
        nullptr, {"前方固定", "プレイヤーを狙う", "放物線砲弾（着弾爆発）", "大型砲弾（砲口に沿って直進）"});
    properties.Parameter("弾速", "speed", &Parameter::speed, 1.0f, 120.0f);
    properties.Parameter("発射間隔", "interval", &Parameter::interval, 0.08f, 20.0f);
    properties.Parameter("弾の半径", "moveAmount", &Parameter::moveAmount, 0.1f, (std::numeric_limits<float>::max)(),
        nullptr, {}, {.speed = 0.01f});
    properties.Parameter("索敵範囲", "detectionRange", &Parameter::detectionRange, 0.0f, 300.0f,
        +[](const Self& object) { return object.GetParameters() && (object.GetParameters()->actionMode != 3); });
    properties.Parameter("砲撃経路の起動余裕", "bombardActivationMargin", &Parameter::detectionRange, 0.0f, 300.0f,
        +[](const Self& object) { return object.GetParameters() && (object.GetParameters()->actionMode == 3); }, {},
        {.parameterId = "detectionRange", .help = +[](const Self&) { return std::string("弾の全経路からこの距離以内にプレイヤーがいると発射します。0で常時発射。周期と飛んでいる弾は止めません。"); }});
    // プレイヤーを追う通常弾では、旋回速度も編集できる。
    properties.Parameter("旋回速度（度/秒）", "moveSpeed", &Parameter::moveSpeed, 1.0f, 720.0f,
        +[](const Self& object) { return object.GetParameters() && (object.GetParameters()->actionMode == 1); });
    properties.Parameter("開始時に有効", "startActive", &Parameter::startActive);
    properties.Parameter("OFFで元に戻す", "returnOnOff", &Parameter::returnOnOff);
    properties.Parameter("編集停止中に発射をプレビュー", "editorPreviewFiring", &Parameter::editorPreviewFiring);
    properties.Parameter("弾道の上昇高さ", "jumpPower", &Parameter::jumpPower, 3.0f, 20.0f,
        +[](const Self& object) { return object.GetParameters() && (object.GetParameters()->actionMode == 2); });
    properties.Parameter("砲弾の重力", "gravity", &Parameter::gravity, 4.0f, 60.0f,
        +[](const Self& object) { return object.GetParameters() && (object.GetParameters()->actionMode == 2); });
    properties.Parameter("爆風半径", "healAmount", &Parameter::healAmount, 1.0f, 8.0f,
        +[](const Self& object) { return object.GetParameters() && (object.GetParameters()->actionMode == 2); });
    properties.Parameter("発射前の予告時間", "shakeDuration", &Parameter::shakeDuration, 0.35f, 4.0f,
        +[](const Self& object) { return object.GetParameters() && (object.GetParameters()->actionMode == 2); });
    // 大型砲弾では持続時間と爆風半径を使い、発射直前と直後の姿勢を別に調整できる。
    properties.Parameter("弾の最大持続時間", "projectileLifetime", &Parameter::projectileLifetime, 0.2f, 20.0f,
        +[](const Self& object) { return object.GetParameters() && (object.GetParameters()->actionMode == 3); }, {}, {.help = +[](const Self& object) { return std::string("最大飛距離の目安: ") + std::to_string(object.GetParameters()->speed * object.GetParameters()->projectileLifetime) + " m"; }});
    properties.Parameter("爆風半径", "healAmount_straight", &Parameter::healAmount, 0.5f, 8.0f,
        +[](const Self& object) { return object.GetParameters() && (object.GetParameters()->actionMode == 3); }, {}, {.parameterId = "healAmount"});
    const auto straightOnly = +[](const Self& object) { return object.GetParameters() && object.GetParameters()->actionMode == 3; };
    properties.Expose("直進砲台のため時間", ENGINE_PROPERTY(bombardAnticipationTime_), 0.0f, 0.6f, straightOnly);
    properties.Expose("直進砲台の反動の強さ", ENGINE_PROPERTY(bombardRecoilStrength_), 0.0f, 0.45f, straightOnly);
    properties.Expose("直進砲台の戻り時間", ENGINE_PROPERTY(bombardRecoveryTime_), 0.12f, 1.0f, straightOnly);
}

// 編集停止中は無害な弾道プレビューだけを進め、Play 中は通常の攻撃を実行します。
void GimmickFireCannon::Update(float deltaTime) {
    // パラメーターがなければ、発射モードと開始状態を読む前に初期化する。
    if (!GetParameters().has_value()) {
        ResetParameters();
    }

    // モード変更やプレビュー停止時に、前フレームの描画補正を残しません。
    if (auto* renderer = GetMeshRenderer()) renderer->ResetVisualTransform();
    const bool playing = IsPlaying();
    suppressBombardTerrainContacts_ = playing && GetParameters()->actionMode == 3;
    bool editorPreview = false;
#ifdef USE_IMGUI
    editorPreview = !playing && GetParameters()->editorPreviewFiring;
#endif

    // プレビューの入切では退避した配置を必要に応じて戻し、この砲台のプレビュー弾と旧照準を片付けます。
    if (previewActive_ != editorPreview) {
        const bool wasPreview = previewActive_;
        // プレビューを終了する場合は、この砲台が生成したプレビュー弾を消す。
        if (wasPreview)
            BulletManager::GetInstance()->ClearPreviewForOwner(this);
        // プレビューへ切り替える直前に実戦用の姿勢を確保済みなら、配置姿勢へ戻す。
        if (initializedForPlay_ && !wasPreview) {
            SetTranslate(basePosition_);
            SetRotation(baseRotation_);
            SetScale(baseScale_);
        }
        initializedForPlay_ = false;
        targetLocked_ = false;
        flightTimer_ = recoilTimer_ = 0.0f;
        landingWarning_.Hide();
        previewActive_ = editorPreview;
    }

    // プレイもプレビューも停止中なら、発射と照準を初期化して共通更新だけ行う。
    if (!playing && !editorPreview) {
        // 直前の発射更新で配置姿勢を保存していれば、停止時に元へ戻す。
        if (initializedForPlay_) {
            SetTranslate(basePosition_);
            SetRotation(baseRotation_);
            SetScale(baseScale_);
        }

        landingWarning_.Hide();
        targetLocked_ = false;
        flightTimer_ = recoilTimer_ = 0.0f;
        initializedForPlay_ = false;
        active_ = GetParameters()->startActive;
        fireTimer_ = 0.0f;
        BaseGimmick::Update(deltaTime);
        return;
    }

    // 発射更新へ入る最初のフレームで配置姿勢と初弾の待ち時間を記録する。
    if (!initializedForPlay_) {
        basePosition_ = GetTranslate();
        baseRotation_ = GetRotation();
        baseScale_ = GetScale();
        active_ = editorPreview || GetParameters()->startActive;
        fireTimer_ = editorPreview ? (std::min)(0.15f, GetFireInterval() * 0.35f)
                                   : GetFireInterval() * 0.35f;
        initializedForPlay_ = true;
    }

    if (editorPreview) {
        // 編集中の Transform は操作対象そのものなので、毎フレーム現在値を発射基準にします。
        basePosition_ = GetTranslate();
        baseRotation_ = GetRotation();
        baseScale_ = GetScale();
        active_ = true;
    }

    // 迫撃と固定砲の弾幕は専用周期を使うため、直射の照準処理へは進みません。
    // 迫撃モードでは着弾予告と放物線計算を専用更新へ渡す。
    if (GetParameters()->actionMode == 2) {
        UpdateMortar(deltaTime);
        BaseGimmick::Update(deltaTime);
        return;
    }

    // 大型直進モードでは、砲口を固定して専用の反動と発射周期を更新する。
    if (GetParameters()->actionMode == 3) {
        // 発射周期は維持し、その終わりにため、発射直後に反動を重ねます。
        const float frameDeltaTime = (std::max)(0.0f, deltaTime);
        recoilTimer_ = (std::max)(0.0f, recoilTimer_ - frameDeltaTime);
        landingWarning_.Hide();
        bool fireNow = false;
        // 有効な砲台だけ、次弾までの残り秒数を減らす。
        if (active_) {
            fireTimer_ -= frameDeltaTime;
            // 次弾の時刻に達したら発射を予定し、発射周期と反動の時計を設定する。
            if (fireTimer_ <= 0.0f) {
                fireNow = true;
                fireTimer_ = GetFireInterval();
                recoilTimer_ = GetBombardRecoveryTime();
            }
        // 無効の間は発射せず、再開時の待ちを通常の発射間隔の35%以下にする。
        } else {
            fireTimer_ = (std::min)(fireTimer_, GetFireInterval() * 0.35f);
        }
        UpdateBombardPose();
        // 発射時刻に達した場合は、補正後の砲口から大型弾の生成を試みる。
        if (fireNow) FireBombard();
        BaseGimmick::Update(deltaTime);
        return;
    }

    landingWarning_.Hide();
    engine::Vector3 fireDirection = {};
    const bool canFire = active_ && FindFireDirection(fireDirection);

    // 実戦中の追尾砲は回転途中の砲身方向を使って発射し、プレビューでは配置の向きを変更しません。
    if (canFire && GetParameters()->actionMode == 1) {
        // 実戦では目標方向へ旋回し、そのフレームの砲身方向を発射に使う。
        if (!editorPreview) {
            RotateToward(fireDirection, deltaTime);
            fireDirection = GetForwardDirection();
        }
    }

    // 有効な照準を取得できた場合だけ、通常弾の発射時計を進める。
    if (canFire) {
        fireTimer_ -= deltaTime;

        // 待ち時間を使い切った時点で通常弾を1発生成し、周期を再開する。
        if (fireTimer_ <= 0.0f) {
            FireProjectile(fireDirection);
            fireTimer_ = GetFireInterval();
        }
    // 砲撃が無効か標的を解決できない間は発射せず、次の再開を待つ。
    } else {
        fireTimer_ = (std::min)(fireTimer_, GetFireInterval() * 0.35f);
    }

    BaseGimmick::Update(deltaTime);
}

void GimmickFireCannon::OnTrigger() {
    // 単発トリガーは現在の発射状態を反転します。
    OnSwitchEvent(!active_);
}

void GimmickFireCannon::OnSwitchEvent(bool active) {
    // 停止時には照準と着弾予告も消し、旧弾の視覚情報を残しません。
    // 開始前に通知されても解除の可否を参照できるよう、設定がなければ用意する。
    if (!GetParameters().has_value()) {
        ResetParameters();
    }

    // オフで戻す設定を無効にしている場合は、停止通知を受けても現在の発射状態を保つ。
    if (!active && !GetParameters()->returnOnOff) {
        return;
    }

    active_ = active;

    // 発射を停止する場合は、次弾用に固定した着弾位置と着弾予告を解除する。
    if (!active) {
        targetLocked_ = false;
        landingWarning_.Hide();
    }
}

bool GimmickFireCannon::IsPlaying() const {
    // ゲーム用攻撃と編集用の無害な弾道プレビューを切り分けます。
    SceneManager* sceneManager = SceneManager::GetInstance();
    return sceneManager && sceneManager->IsPlaying();
}

bool GimmickFireCannon::FindFireDirection(engine::Vector3& outDirection) const {
    // 固定砲は前方へ、追尾砲は射程内のプレイヤー中心へ照準を向けます。
    if (!GetParameters().has_value() || GetParameters()->actionMode == 0) {
        outDirection = GetForwardDirection();
        return true;
    }

    SceneManager* sceneManager = SceneManager::GetInstance();

    // シーン管理がなければ追尾対象を解決できない。
    if (!sceneManager) {
        return false;
    }

    BaseScene* scene = sceneManager->GetCurrentScene();

    // 現在のシーンがなければ照準に使うプレイヤーを探せない。
    if (!scene) {
        return false;
    }

    Player* player = GetScenePlayer(scene);

    // 標的のいない編集プレビューでは、設置した向きで射線を確認できるようにします。
    if (!player) {
        // 編集プレビューだけは、プレイヤーがいなくても配置の前方を使う。
        if (previewActive_) {
            outDirection = GetForwardDirection();
            return true;
        }
        return false;
    }

    // 足元から少し上を狙い、砲口からの距離を索敵範囲と比較します。
    engine::Vector3 target = player->GetWorldPosition();
    target.y += 0.65f;

    const engine::Vector3 muzzle = GetMuzzlePosition(GetForwardDirection());
    engine::Vector3 diff = target - muzzle;
    const float distance = engine::Math::Length(diff);
    const float range = (GetParameters()->detectionRange > 0.0f) ? GetParameters()->detectionRange : kDefaultDetectionRange;

    // 標的が射程外、または距離がほぼ0で方向を求められない場合は、実戦では発射しない。
    if (distance > range || distance < 0.001f) {
        // 同じ条件でもプレビューでは砲台の前方を使い、弾道を確認できるようにする。
        if (previewActive_) {
            outDirection = GetForwardDirection();
            return true;
        }
        return false;
    }

    outDirection = engine::Math::Normalize(diff);
    return true;
}

engine::Vector3 GimmickFireCannon::GetForwardDirection() const {
    // ピッチとヨーから発射方向を作り、零ベクトル時は前方を既定にします。
    const engine::Vector3 rot = GetRotation();
    const float cosPitch = std::cos(rot.x);
    engine::Vector3 forward = {std::sin(rot.y) * cosPitch, -std::sin(rot.x), std::cos(rot.y) * cosPitch};

    // 方向の長さが0.001未満なら正規化せず、ワールド+Zを既定方向にする。
    if (engine::Math::Length(forward) < 0.001f) {
        return {0.0f, 0.0f, 1.0f};
    }

    return engine::Math::Normalize(forward);
}

engine::Vector3 GimmickFireCannon::GetMuzzlePosition(const engine::Vector3& direction) const {
    // モデルの大きさに応じて砲口を前上方へずらし、本体から弾が出るようにします。
    const engine::Vector3 scale = GetScale();
    engine::Vector3 muzzle = GetWorldPosition();
    muzzle += direction * ((std::max)(scale.z, 0.5f) * 0.95f);
    muzzle.y += (std::max)(scale.y, 0.5f) * 0.18f;
    return muzzle;
}

void GimmickFireCannon::RotateToward(const engine::Vector3& direction, float deltaTime) {
    // 短い角度の経路で回転し、角度境界をまたいでも大回りしません。
    const float horizontalLength = std::sqrt(direction.x * direction.x + direction.z * direction.z);

    // 水平成分がほぼ0なら向くヨー角を決められないため、現在の向きを保つ。
    if (horizontalLength < 0.001f) {
        return;
    }

    const float targetYaw = std::atan2(direction.x, direction.z);
    const float targetPitch = std::atan2(-direction.y, horizontalLength);
    const float t = std::clamp(GetTurnSpeedRadians() * deltaTime, 0.0f, 1.0f);

    engine::Vector3 rot = GetRotation();
    rot.y = engine::Math::LerpShortAngle(rot.y, targetYaw, t);
    rot.x = engine::Math::LerpShortAngle(rot.x, targetPitch, t);
    SetRotation(rot);
}

void GimmickFireCannon::FireProjectile(const engine::Vector3& direction) {
    // 射程は標的選択に使い、弾の消滅時刻は Editor の持続時間に従います。
    const float radius = GetProjectileRadius();
    const float speed = GetProjectileSpeed();
    const float life = GetProjectileLifetime();

    // 岩の芯と炎の見た目を弾の判定半径から作り、プレビュー弾には生成元の砲台を付けます。
    BulletVisualConfig visual;
    visual.collisionRadiusIsWorld = true;
    visual.materialType = 0;
    visual.blendMode = BlendMode::kNormal;
    visual.color = {1.0f, 1.0f, 1.0f, 1.0f};
    visual.emissive = 1.7f;
    visual.visualScale = radius * 0.60f / 1.043f;
    visual.burningPreset = "shared/cannon_burning_flame";
    visual.trailSecondaryPreset = "shared/fireball_trail_embers";
    visual.impactPreset = "shared/fireball_impact_flame";
    visual.impactSecondaryPreset = "shared/fireball_impact_sparks";
    visual.impactDebrisPreset = "shared/cannon_shell_basalt_shatter";
    visual.impactDebrisSecondaryPreset = "shared/cannon_shell_ember_shatter";
    visual.impactDebrisScale = radius;
    visual.impactOnExpiry = true;
    visual.trailInterval = 0.06f;
    visual.trailReverseDirection = true;
    visual.impactDirected = false;
    visual.previewOnly = previewActive_;
    visual.previewOwner = this;

    // 壁や地形では弾を壊さず、プレイヤー命中か寿命終了で消滅させます。
    BulletManager::GetInstance()->Fire(GetMuzzlePosition(direction) + direction * (radius + 0.16f), direction * speed,
                                       engine::collisionCategories::Get("enemy_attack"), engine::collisionCategories::Get("player"),
                                       "Gimmicks/volcanic_bombard_shell", radius, life, visual, 1.0f, {}, DamageType::kFire);
}

float GimmickFireCannon::GetBombardRecoveryTime() const {
    // 反動時間が有限なら0.12〜1.0秒に収める。反動が次の弾までに収まるよう周期の65%も上限にする。
    const float configured = std::isfinite(bombardRecoveryTime_) ? bombardRecoveryTime_ : 0.42f;
    return (std::min)(std::clamp(configured, 0.12f, 1.0f), GetFireInterval() * 0.65f);
}

// 大型直進砲の発射直前と発射後に、見た目だけを伸縮・後退させる。
void GimmickFireCannon::UpdateBombardPose() {
    auto* renderer = GetMeshRenderer();
    // 描画メッシュがない場合は見た目の補正を適用できない。
    if (!renderer) return;
    const float strength = std::isfinite(bombardRecoilStrength_) ? std::clamp(bombardRecoilStrength_, 0.0f, 0.45f) : 0.22f;
    float squash = 0.0f;
    float retreat = 0.0f;
    // 反動の残り秒数がある間は、発射後の減衰する揺れを優先する。
    if (recoilTimer_ > 0.0f) {
        const float t = std::clamp(1.0f - recoilTimer_ / GetBombardRecoveryTime(), 0.0f, 1.0f);
        // 一度だけ小さく伸び返し、末尾の速度も0へ収束させます。
        const float spring = (1.0f - t) * (1.0f - t) * std::cos(2.0f * kPi * t);
        squash = strength * spring;
        retreat = strength * 2.0f * spring;
    // 反動から戻った後は、次弾の直前だけ発射前のため姿勢へ移ります。
    } else if (active_) {
        const float configured = std::isfinite(bombardAnticipationTime_) ? bombardAnticipationTime_ : 0.20f;
        const float duration = (std::min)(std::clamp(configured, 0.0f, 0.6f), GetFireInterval() * 0.30f);
        // 有効なため時間がある場合にだけ、その区間の現在位置を計算する。
        if (duration > 0.0001f) {
            float t = std::clamp(1.0f - fireTimer_ / duration, 0.0f, 1.0f);
            t = Easing::SmoothStep(t);
            squash = strength * 0.35f * t;
            retreat = strength * 0.45f * t;
        }
    }
    // 読込後の砲口はローカル-Zなので、反動は+Zへ加える。補正は描画メッシュにだけ渡す。
    renderer->SetVisualTransform({1.0f + squash * 0.35f, 1.0f + squash * 0.35f, 1.0f - squash}, {}, {0.0f, 0.0f, retreat});
}

uint32_t GimmickFireCannon::GetContactCollisionMask() const {
    const uint32_t mask = GetCollisionMask();
    // 本体のground属性は残し、プレイヤーからの接触と着地は従来どおり受け付けます。
    return suppressBombardTerrainContacts_ ? mask & ~engine::collisionCategories::Get("ground") : mask;
}

// プレイヤーが最大飛距離までの射線の近くにいればtrueを返す。設定無効と対象不在ではtrue。
bool GimmickFireCannon::IsBombardPathRelevant(const engine::Vector3& start, const engine::Vector3& forward) const {
    const float margin = GetParameters()->detectionRange;
    // 起動余裕が有限の正数でない場合は、距離で発射を制限しない。
    if (!std::isfinite(margin) || margin <= 0.0f) return true;
    auto* manager = SceneManager::GetInstance();
    auto* player = manager ? GetScenePlayer(manager->GetCurrentScene()) : nullptr;
    // プレイヤーが未配置でも発射を許可する。
    if (!player) return true;

    // 寿命内に届く線分へ投影し、遠方でもこちらへ飛んでくる砲台は有効にします。
    const float length = GetProjectileSpeed() * GetProjectileLifetime();
    const engine::Vector3 position = player->GetWorldPosition();
    const float along = std::clamp(engine::Math::Dot(position - start, forward), 0.0f, length);
    const engine::Vector3 offset = position - (start + forward * along);
    const engine::Aabb bounds = player->GetAabb();
    const float playerRadius = engine::Math::Length((bounds.max - bounds.min) * 0.5f);
    const float distance = margin + GetProjectileRadius() + std::clamp(GetParameters()->healAmount, 0.5f, 8.0f) + playerRadius;
    return !std::isfinite(distance) || engine::Math::Dot(offset, offset) <= distance * distance;
}

void GimmickFireCannon::FireBombard() {
    // OBJ の +Z 砲口は左手座標への読み込みで -Z になるため、描画された砲口側へ発射します。
    UpdateWorldMatrix();
    auto world = GetWorldMatrix();
    // 描画姿勢を取得できる場合は、その補正もワールド行列へ含める。
    if (const auto* renderer = GetMeshRenderer()) {
        // 弾と発射光は、反動中も実際に描かれる砲口から出します。
        const auto pose = engine::Math::MakeAffineMatrix(renderer->GetVisualScale(), renderer->GetVisualRotation(), renderer->GetVisualOffset());
        world = engine::Math::Multiply(pose, world);
    }
    engine::Vector3 forward = {-world.m[2][0], -world.m[2][1], -world.m[2][2]};
    // 変換した砲口方向の長さが0.001以下なら、発射方向を決められないため中止する。
    if (engine::Math::Length(forward) <= 0.001f) {
        return;
    }
    forward = engine::Math::Normalize(forward);

    const float radius = GetProjectileRadius();
    const float speed = GetProjectileSpeed();
    const float life = GetProjectileLifetime();

    // 読み込み後の砲口中心はローカル(0, 2.73, -2.55)。球の後端が砲縁に重ならない位置から出します。
    const engine::Vector3 muzzle = {
        world.m[3][0] + world.m[1][0] * 2.73f - world.m[2][0] * 2.55f,
        world.m[3][1] + world.m[1][1] * 2.73f - world.m[2][1] * 2.55f,
        world.m[3][2] + world.m[1][2] * 2.73f - world.m[2][2] * 2.55f};

    const engine::Vector3 launch = muzzle + forward * (radius + 0.16f);
    // 周期の更新は続け、経路から遠いときは新しい弾と発射エフェクトだけを生成しません。
    if (!previewActive_ && !IsBombardPathRelevant(launch, forward)) return;

    // 大型弾の爆風・着弾破片を設定し、配置された砲口の向きから直進させます。
    BulletVisualConfig visual;
    visual.visualScale = radius * 0.60f / 1.043f; // 岩の芯を小さくし、外側の炎に輪郭を任せます。
    visual.collisionRadiusIsWorld = true;
    visual.explosionRadius = std::clamp(GetParameters()->healAmount, 0.5f, 8.0f);
    visual.materialType = 0;
    visual.blendMode = BlendMode::kNormal;
    visual.color = {1.0f, 1.0f, 1.0f, 1.0f};
    visual.emissive = 1.7f;
    visual.burningPreset = "shared/cannon_burning_flame";
    visual.trailPreset = "shared/fireball_trail_flame";
    visual.trailSecondaryPreset = "shared/fireball_trail_embers";
    visual.impactPreset = "shared/fireball_impact_flame";
    visual.impactSecondaryPreset = "shared/fireball_impact_sparks";
    visual.impactDebrisPreset = "shared/cannon_shell_basalt_shatter";
    visual.impactDebrisSecondaryPreset = "shared/cannon_shell_ember_shatter";
    visual.impactDebrisScale = radius;
    visual.impactOnExpiry = true;
    visual.trailInterval = 0.04f;
    visual.trailReverseDirection = true;
    visual.impactDirected = false;
    visual.previewOnly = previewActive_;
    visual.previewOwner = this;
    BulletManager::GetInstance()->Fire(launch, forward * speed,
                                       engine::collisionCategories::Get("enemy_attack"),
                                       engine::collisionCategories::Get("player"),
                                       "Gimmicks/volcanic_bombard_shell", radius, life, visual, 1.0f, {},
                                       DamageType::kExplosion);

    // 編集停止中は演出時間も停止しているため、蓄積する発射エフェクトは生成しません。
    if (!previewActive_) {
        // メッシュ効果の管理機能がある場合は、発射光を砲口に出す。
        if (auto* effects = MeshEffectManager::GetInstance()) {
            effects->SpawnEffectAt("Resources/json/effect/shared/effect_bomb_core_flash.json", muzzle,
                                   {0, 0, 0}, {0.5f, 0.5f, 0.5f});
        }
        // GPU粒子が初期化済みなら、発射方向へ火花を出す。
        if (auto* particles = GpuParticleManager::GetInstance(); particles && particles->IsInitialized()) {
            particles->EmitDirected("shared/hit_bomb_fire_sparks", muzzle, forward, 0.7f);
        }
    }
}

float GimmickFireCannon::GetProjectileSpeed() const {
    // 設定ミスで停止弾を作らないよう最小速度を保証します。
    return GetParameters().has_value() ? (std::max)(1.0f, GetParameters()->speed) : kDefaultProjectileSpeed;
}

float GimmickFireCannon::GetFireInterval() const {
    // ゼロ秒連射による弾の増殖を防ぎます。
    const float interval = GetParameters().has_value() ? (std::max)(0.08f, GetParameters()->interval) : kDefaultFireInterval;
    return previewActive_ ? (std::max)(0.8f, interval) : interval;
}

float GimmickFireCannon::GetProjectileRadius() const {
    // 大型化を妨げないよう上限は設けず、不正値と負値だけを除きます。
    const float configured = GetParameters().has_value() ? GetParameters()->moveAmount : kDefaultProjectileRadius;
    return std::isfinite(configured) ? (std::max)(0.1f, configured) : kDefaultProjectileRadius;
}

// 弾の寿命は秒。有限の設定値を0.2〜20に収め、非有限なら既定値を使う。
float GimmickFireCannon::GetProjectileLifetime() const {
    const float configured = GetParameters().has_value() ? GetParameters()->projectileLifetime : kDefaultProjectileLifetime;
    return std::isfinite(configured) ? std::clamp(configured, 0.2f, 20.0f) : kDefaultProjectileLifetime;
}

float GimmickFireCannon::GetTurnSpeedRadians() const {
    // エディター設定は度毎秒、回転計算はラジアンで扱います。
    const float degrees = GetParameters().has_value() ? (std::max)(1.0f, GetParameters()->moveSpeed) : kDefaultTurnSpeedDegrees;
    return degrees * (kPi / 180.0f);
}

std::unique_ptr<engine::Object3D> GimmickFireCannon::Clone() const {
    // 複製先の予告表示と判定資源を初期化してから設定をコピーします。
    auto newObj = std::make_unique<GimmickFireCannon>();
    assert(GetObjectCommon() != nullptr);
    newObj->Initialize(GetObjectCommon(), GetModelName());
    newObj->CopyFrom(this);
    return newObj;
}

void GimmickFireCannon::Draw(ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource) {
    // 砲台に続けて、迫撃弾の地面上の着弾予告を描画します。
    BaseGimmick::Draw(pointLightResource, spotLightResource);
    landingWarning_.DrawGround(pointLightResource, spotLightResource);
}

// 着弾位置の予告、予告時間後の発射、飛行中の予告維持を順に行う。
void GimmickFireCannon::UpdateMortar(float deltaTime) {
    // 台座のピッチを変えず、弾の鉛直軌道だけに重力を使う。
    const float frameDeltaTime = (std::max)(0.0f, deltaTime);
    const float blast = std::clamp(GetParameters()->healAmount, 1.0f, 8.0f);
    const float warning = std::clamp(GetParameters()->shakeDuration, 0.35f, 4.0f);
    const float gravity = std::clamp(GetParameters()->gravity, 4.0f, 60.0f);
    const float radius = GetProjectileRadius();
    recoilTimer_ = (std::max)(0.0f, recoilTimer_ - frameDeltaTime);
    const float squash = std::sin(recoilTimer_ / 0.28f * kPi) * 0.06f;
    // 実戦中にだけ台座の縮尺を反動で変える。編集中の配置縮尺を保持する。
    if (!previewActive_)
        SetScale({baseScale_.x * (1.0f + squash), baseScale_.y * (1.0f - squash), baseScale_.z});
    landingWarning_.Update(frameDeltaTime);

    // 砲弾が飛んでいる間は着弾地点の表示を維持し、次弾の照準を取り直しません。
    if (flightTimer_ > 0.0f) {
        flightTimer_ -= frameDeltaTime;
        // 想定した飛行時間が尽きたら、地面の着弾予告を消す。
        if (flightTimer_ <= 0.0f)
            landingWarning_.Hide();
        return;
    }

    // 砲撃を停止している間は、照準の固定と予告を解除して待機する。
    if (!active_) {
        landingWarning_.Hide();
        targetLocked_ = false;
        return;
    }

    // 到達可能な着弾候補だけを選び、プレビューでは対象が近すぎる・いない場合も前方の仮目標で確認できるようにします。
    if (!targetLocked_) {
        fireTimer_ -= frameDeltaTime;
        // 次弾の時刻に達するまでは、着弾点の探索を始めない。
        if (fireTimer_ > 0.0f)
            return;
        auto* scene = SceneManager::GetInstance()->GetCurrentScene();
        auto* player = GetScenePlayer(scene);
        // 実戦ではプレイヤーが見つからなければ発射を待つ。
        if (!player && !previewActive_)
            return;
        const engine::Vector3 forwardTarget = GetWorldPosition() + GetForwardDirection() *
            (std::max)(12.0f, (std::min)(GetParameters()->detectionRange, 20.0f));
        engine::Vector3 target = player ? player->GetWorldPosition() : forwardTarget;
        const engine::Vector3 targetOffset = target - GetWorldPosition();
        // プレビューで対象が水平10単位未満にいる場合は、確認しやすい前方の仮目標に置き換える。
        if (previewActive_ && targetOffset.x * targetOffset.x + targetOffset.z * targetOffset.z < 100.0f)
            target = forwardTarget;
        // 実戦では中心から標的までの距離が索敵範囲を超える場合は発射を待つ。
        if (!previewActive_ && engine::Math::Length(target - GetWorldPosition()) > GetParameters()->detectionRange)
            return;
        // 砲台自身を除外し、標的の上から地面へレイを飛ばして着弾点を求める。
        engine::PhysicsQueryFilter filter;
        filter.mask = engine::collisionCategories::Get("ground");
        filter.ignoredObject = this;
        const auto ground =
            engine::CollisionManager::GetInstance()->Raycast(target + engine::Vector3{0, 1, 0}, {0, -1, 0}, 18.0f, filter);
        // 実戦では地面を取得できなければ着弾地点を固定しない。プレビューでは仮目標を使える。
        if (!ground.isHit && !previewActive_)
            return;
        lockedTarget_ = ground.isHit ? ground.hitPoint : target;
        engine::Vector3 direction = lockedTarget_ - GetWorldPosition();
        // 着弾地点が水平10単位未満の場合は、砲台の近くに落ちる弾を生成しない。
        if (direction.x * direction.x + direction.z * direction.z < 100.0f)
            return;

        // 予告開始時に狙いを固定し、回避後のプレイヤーを追いかけない。
        if (!previewActive_)
            SetRotation({0.0f, std::atan2(direction.x, direction.z), 0.0f});
        targetLocked_ = true;
        warningTimer_ = warning;
    }

    // 固定済みの位置に危険範囲を表示し、予告時間が終わってから同じ地点へ飛ぶ軌道を計算します。
    warningTimer_ -= frameDeltaTime;
    landingWarning_.ShowCircle(lockedTarget_ + engine::Vector3{0, 0.08f, 0}, blast,
                               std::clamp(1.0f - warningTimer_ / warning, 0.0f, 1.0f), {1.0f, 0.34f, 0.05f, 0.65f});
    // 予告時間が残っている間は範囲表示を続け、発射計算を行わない。
    if (warningTimer_ > 0.0f)
        return;
    const engine::Vector3 forward = GetForwardDirection();
    const engine::Vector3 muzzle = GetWorldPosition() + forward * (2.55f * baseScale_.z) + engine::Vector3{0, 2.73f * baseScale_.y, 0};
    const engine::Vector3 target = lockedTarget_ + engine::Vector3{0, radius + 0.03f, 0};

    // 頂点高度から上向き初速と飛行時間を逆算し、水平速度をロック済み着弾点へ合わせます。
    const float apex = (std::max)(muzzle.y, target.y) + std::clamp(GetParameters()->jumpPower, 3.0f, 20.0f);
    const float vy = std::sqrt(2.0f * gravity * (apex - muzzle.y));
    // 頂点までの秒数と頂点から標的の高さまで落ちる秒数を足し、着弾までの時間を求めます。
    const float flight = vy / gravity + std::sqrt(2.0f * (apex - target.y) / gravity);
    const engine::Vector3 velocity = {(target.x - muzzle.x) / flight, vy, (target.z - muzzle.z) / flight};
    // 弾の半径と爆風を指定し、編集プレビューの場合は無害な弾として発生元も登録する。
    BulletVisualConfig visual;
    visual.visualScale = radius;
    visual.explosionRadius = blast;
    visual.color = {1, 1, 1, 1};
    visual.emissive = 1.0f;
    visual.previewOnly = previewActive_;
    visual.previewOwner = this;
    BulletManager::GetInstance()->Fire(muzzle, velocity, engine::collisionCategories::Get("enemy_attack"),
                                       engine::collisionCategories::Get("player") | engine::collisionCategories::Get("ground"), "Gimmicks/volcanic_shell", radius,
                                       flight + 2.0f, visual, 1.0f, {}, DamageType::kExplosion, gravity);
    // 実戦用の砲口演出はプレビューで増やさず、発射後は飛行・再発射・反動それぞれの時計を設定します。
    if (!previewActive_) {
        // 実戦でメッシュ効果を利用できれば、砲口に一時的な発射光を出す。
        if (auto* effects = MeshEffectManager::GetInstance())
            effects->SpawnEffectAt("Resources/json/effect/shared/effect_bomb_core_flash.json", muzzle, {0, 0, 0}, {.4f, .4f, .4f});
        // 実戦でGPU粒子を利用できれば、砲口から発射方向へ火花を出す。
        if (auto* particles = GpuParticleManager::GetInstance(); particles && particles->IsInitialized())
            particles->EmitDirected("shared/hit_bomb_fire_sparks", muzzle, forward, 0.6f);
    }
    // 次弾で狙いを取り直せる状態に戻し、着弾までの時間と反動の時計を保存する。
    targetLocked_ = false;
    flightTimer_ = flight;
    fireTimer_ = GetFireInterval();
    recoilTimer_ = 0.28f;
}
