#include <cstdint>
#include "engine/graphics/effect/VfxAssetPath.h"
#include "Bullet.h"
#include "CameraManager.h"
#include "MeshEffectManager.h"
#include "DebrisEffectManager.h"
#include <unordered_set>
#include "CollisionConfig.h"
#include "CollisionManager.h"
#include "GpuParticleManager.h"
#include <game/system/rules/EventManager.h>
#include <algorithm>
#include <cmath>
#include <initializer_list>

// 共通の 3D オブジェクトを初期化する。発射ごとに変わる衝突設定は Fire() が受け取る。
void Bullet::Initialize(engine::Object3DCommon* common) {
    engine::Object3D::Initialize(common);

}

void Bullet::Fire(const engine::Vector3& position, const engine::Vector3& velocity, float life, uint32_t attribute, uint32_t mask,
                  float damage, const StatusEffectApplication& statusEffect, DamageType damageType) {
    // 発射ごとに、軌道・寿命・ダメージ・着弾演出の状態を初期値へ戻す。
    SetTranslate(position);
    velocity_ = velocity;
    impactNormal_ = {0.0f, 0.0f, 0.0f};
    gravity_ = 0.0f;
    explosionRadius_ = 0.0f;
    lifeTimer_ = life;
    damage_ = (std::max)(0.0f, damage);
    damageType_ = damageType;
    statusEffect_ = statusEffect;
    trailTimer_ = 0.0f;
    burningTimer_ = 0.0f;
    impactEmitted_ = false;
    isDead_ = false;

    SetCollisionAttribute(attribute);
    SetCollisionMask(mask);
}

void Bullet::ConfigureVfx(const std::string& trailPreset, const std::string& trailSecondaryPreset,
                          const std::string& impactPreset, const std::string& impactSecondaryPreset,
                          float trailInterval, float trailSpeedScale, bool trailReverseDirection, bool impactDirected) {
    // 尾の発生間隔は最低0.01秒、粒子速度倍率は最低0.05にして、プリセットごとの向きも保持する。
    trailPreset_ = trailPreset;
    trailSecondaryPreset_ = trailSecondaryPreset;
    impactPreset_ = impactPreset;
    impactSecondaryPreset_ = impactSecondaryPreset;
    trailInterval_ = (std::max)(0.01f, trailInterval);
    trailTimer_ = 0.0f;
    trailSpeedScale_ = (std::max)(0.05f, trailSpeedScale);
    trailReverseDirection_ = trailReverseDirection;
    impactDirected_ = impactDirected;
}

// 炎の発生設定を保存し、次の更新から発生間隔を数え直す。
void Bullet::ConfigureBurningVfx(const std::string& preset, float radius) {
    burningPreset_ = preset;
    burningRadius_ = radius;
    burningTimer_ = 0.0f;
}

// 着弾時の破片と、寿命切れ・固体への接触で着弾演出を再生する条件を設定する。
void Bullet::ConfigureDestructionVfx(const std::string& debrisPreset, const std::string& secondaryDebrisPreset,
                                     float debrisScale, bool impactOnExpiry, bool destroyOnSolidCastHit) {
    impactDebrisPreset_ = debrisPreset;
    impactDebrisSecondaryPreset_ = secondaryDebrisPreset;
    impactDebrisScale_ = (std::isfinite(debrisScale) && debrisScale > 0.0f) ? debrisScale : 1.0f;
    impactOnExpiry_ = impactOnExpiry;
    destroyOnSolidCastHit_ = destroyOnSolidCastHit;
}

void Bullet::Expire(bool playImpact) {
    // 終了済みの弾は状態も演出も処理済みなので、そのまま戻る。
    if (isDead_) {
        return;
    }
    // 寿命切れなど、衝突以外の終了でも必要なら着弾演出を一度だけ出す。
    if (playImpact) {
        EmitImpactVfx();
    }

    isDead_ = true;
}

// 経過秒に応じて演出・寿命を先に更新し、生存中の弾だけをこのフレームの移動区間で掃引する。
void Bullet::Update(float deltaTime) {
    // 終了済みの弾は、BulletManagerの削除処理まで寿命と位置の更新を行わない。
    if (isDead_) {
        return;
    }

    // 尾と弾を覆う炎は別々の間隔で出し、編集プレビューでは尾を省きます。
    trailTimer_ -= deltaTime;

    // 実戦中で尾の発生時刻に達した場合は、尾を一回出して次の発生間隔を足す。
    if (!previewOnly_ && trailTimer_ <= 0.0f) {
        EmitTrailVfx();
        trailTimer_ += trailInterval_;
    }

    // 炎のプリセットが指定されている弾だけ、炎の発生間隔を更新する。
    if (!burningPreset_.empty()) {
        burningTimer_ -= deltaTime;
        // 次の発生時刻に達したら炎を出し、プレビュー時は実戦時より発生間隔を長く取る。
        if (burningTimer_ <= 0.0f) {
            EmitBurningVfx();
            burningTimer_ += previewOnly_ ? 0.05f : 0.03f;
        }
    }

    // 移動の前に残り寿命を減らす。
    lifeTimer_ -= deltaTime;

    // 寿命が0秒以下になったら必要な終了演出を出し、このフレームの移動と命中判定を打ち切る。
    if (lifeTimer_ <= 0.0f) {
        Expire(impactOnExpiry_);
        return;
    }

    // マグマ弾用の見た目を指定された場合だけ、鉛直速度に応じて表示の縦横比を変える。
    if (magmaHopperPresentation_) {
        // 見た目だけを伸縮し、衝突半径は発射時の値を維持する。
        const float base = GetColliderConfig().size.x * 1.3f;
        const float stretch = 0.92f + 0.40f * std::clamp(std::abs(velocity_.y) / 20.0f, 0.0f, 1.0f);
        const float width = 1.0f / std::sqrt(stretch);
        SetScale({base * width, base * stretch, base * width});

        // 現在のカメラを取得できた場合だけ、表示面の向きをカメラに合わせる。
        if (auto* camera = CameraManager::GetInstance()->GetActiveCamera()) {
            const engine::Vector3 towardCamera = camera->GetEye() - GetTranslate();
            // XZ平面の距離二乗がしきい値を超えたときだけ、水平方向の角度を確定する。
            if (towardCamera.x * towardCamera.x + towardCamera.z * towardCamera.z > 0.001f)
                SetRotation({0.0f, std::atan2(-towardCamera.x, -towardCamera.z), 0.0f});
        }
    }

    // 放物線のフレーム間変位を計算し、既存のSphereCastで地形との衝突を調べる。
    // 一定重力の変位は現在速度×経過秒数に落下加速度の項を足します。次回使う鉛直速度はそのあと更新します。
    const engine::Vector3 displacement = velocity_ * deltaTime + engine::Vector3{0.0f, -0.5f * gravity_ * deltaTime * deltaTime, 0.0f};
    velocity_.y -= gravity_ * deltaTime;
    // 編集専用の弾は変位だけを適用し、命中やダメージ処理に入る前に戻る。
    if (previewOnly_) {
        SetTranslate(GetTranslate() + displacement);
        return;
    }
    const float travelDistance = engine::Math::Length(displacement);

    // 方向を正規化できないほど移動が小さければ掃引を行いません。
    if (travelDistance <= 0.000001f) {
        return;
    }

    // 1フレームの移動区間を球で走査し、高速弾が薄い対象を飛び越えないようにします。
    engine::PhysicsQueryFilter filter;
    filter.mask = GetCollisionMask();
    filter.ignoredObject = this;
    filter.ignoreDescendants = true;
    filter.includeTriggers = !destroyOnSolidCastHit_;
    filter.refineSphereCastHit = true;

    const engine::Vector3 direction = displacement / travelDistance;
    const engine::RaycastHit hit = engine::CollisionManager::GetInstance()->SphereCast(GetTranslate(), GetCollisionRadius(),
                                                                       direction, travelDistance, filter);
    // 掃引で命中し、相手の参照も得られた場合は、弾をそこまで進めて着弾を処理する。
    if (hit.isHit && hit.hitObject) {
        // 球の最初の接触点よりわずかに手前へ置き、着弾エフェクトの中心が壁へ潜らないようにします。
        SetTranslate(GetTranslate() + direction * (std::max)(0.0f, hit.distance - 0.002f));
        UpdateWorldMatrix(true); // ガード方向と被弾演出にも、停止した命中位置を渡す。
        impactNormal_ = hit.normal;
        const uint32_t attribute = GetCollisionAttribute();
        const uint32_t targetAttribute = hit.hitObject->GetCollisionAttribute();
        const bool isDamageTarget =
            ((attribute & engine::collisionCategories::Get("enemy_attack")) && (targetAttribute & engine::collisionCategories::Get("player"))) ||
            ((attribute & (engine::collisionCategories::Get("player_attack") | engine::collisionCategories::Get("player_bullet"))) &&
             (targetAttribute & engine::collisionCategories::Get("enemy")));
        // 爆発半径を持つ弾は、接触相手を直撃候補としてExplode()へ渡す。
        if (explosionRadius_ > 0.0f)
            Explode(hit.hitObject);
        // 通常弾は敵味方の攻撃属性の組み合わせが成立し、相手のマスクがこの弾を許可した場合だけ単発ダメージを通知する。
        else if (isDamageTarget && (hit.hitObject->GetCollisionMask() & attribute)) {
            // Castは重なる直前に停止するため、受け手のOnCollisionを待たずに命中を確定する。
            // 通常の接触判定が同じ弾を処理しないよう、通知する前に衝突属性とマスクを外す。
            isDead_ = true;
            SetCollisionAttribute(0);
            SetCollisionMask(0);
            EmitImpactVfx();
            DamageEvent event;
            event.target = hit.hitObject;
            event.attacker = this;
            event.damageAmount = damage_;
            // Playerの通常接触と同じ被弾無敵を、Castからの通知にも引き継ぐ。
            event.invincibilityDuration = (targetAttribute & engine::collisionCategories::Get("player")) ? 1.0f : 0.0f;
            event.damageType = damageType_;
            event.statusEffect = statusEffect_;
            EventManager::GetInstance()->Dispatch(event);
            EventManager::GetInstance()->Dispatch(BulletHitEvent{hit.hitObject, this});
        // ダメージが成立しなかった命中でも、固体で消える設定の弾が地形やマップブロックに当たれば着弾演出付きで終了する。
        } else if (destroyOnSolidCastHit_ &&
                 (hit.hitObject->GetCollisionAttribute() &
                  (engine::collisionCategories::Get("ground") | engine::collisionCategories::Get("map_block"))))
            Expire(true);
        return;
    }

    // 移動区間に接触がないときだけ、計算したフレーム末尾の位置まで進めます。
    SetTranslate(GetTranslate() + displacement);
}

bool Bullet::OnCollision(engine::Object3D* other) {
    // 終了済み・プレビュー中・相手の参照がない通知からは、着弾を発生させない。
    if (isDead_ || previewOnly_ || !other)
        return false;

    engine::CollisionInfo info = CheckCollision(other);

    // 現在の形状を再検査して重なりがなければ、着弾として扱わない。
    if (!info.isColliding) {
        return false;
    }

    // 爆発半径が設定されている場合は法線を保存し、範囲判定と演出をExplode()へ任せる。
    if (explosionRadius_ > 0.0f) {
        impactNormal_ = info.normal;
        Explode(other);
        return true;
    }

    // 通常の接触は法線を保存して弾を終了し、直接命中のイベントを送る。
    impactNormal_ = info.normal;
    EmitImpactVfx();
    isDead_ = true;
    EventManager::GetInstance()->Dispatch(BulletHitEvent{other, this});
    return true;
}

// 速度から尾の放出方向を求め、設定済みの主と副の粒子を同じ位置から発生させる。
void Bullet::EmitTrailVfx() {
    // 主・副の両方の尾プリセットが空なら、発生させる粒子がないので戻る。
    if (trailPreset_.empty() && trailSecondaryPreset_.empty()) {
        return;
    }

    GpuParticleManager* particles = GpuParticleManager::GetInstance();

    // GPUパーティクルが未生成または未初期化なら、粒子の発生要求を送れない。
    if (!particles || !particles->IsInitialized()) {
        return;
    }

    engine::Vector3 direction = velocity_;

    // 弾の速度がほぼ0なら、尾の既定方向として正のZ軸を使う。
    if (engine::Math::Length(direction) <= 0.001f) {
        direction = {0.0f, 0.0f, 1.0f};
    // 速度から方向を取れる場合は、速さの影響を除いた単位方向を渡す。
    } else {
        direction = engine::Math::Normalize(direction);
    }

    // 進行方向の反対へ尾を出す設定なら、発生方向をここで反転する。
    if (trailReverseDirection_) {
        direction = direction * -1.0f;
    }

    // 主プリセットが設定されている場合は、弾の現在位置で主の尾を発生させる。
    if (!trailPreset_.empty()) {
        particles->EmitDirected(trailPreset_, GetTranslate(), direction, trailSpeedScale_);
    }

    // 副プリセットも設定されていれば、同じ位置と方向で副の尾も重ねる。
    if (!trailSecondaryPreset_.empty()) {
        particles->EmitDirected(trailSecondaryPreset_, GetTranslate(), direction, trailSpeedScale_);
    }
}

// 炎プリセットの表示範囲を弾の半径に合わせ、進む弾では炎を後方へ流す。
void Bullet::EmitBurningVfx() {
    GpuParticleManager* particles = GpuParticleManager::GetInstance();
    // GPUパーティクルを使える状態になってから、炎の発生設定を調べる。
    if (!particles || !particles->IsInitialized()) {
        return;
    }

    const auto it = vfxAssetPath::FindPreset(particles->GetPresets(), burningPreset_);
    // 炎のプリセットが登録されていなければ、設定を作れないので発生を見送る。
    if (it == particles->GetPresets().end()) {
        return;
    }

    // 専用プリセットの色と寿命は Editor で調整し、空間サイズだけ弾の半径へ追従させます。
    GpuParticleConfig config = it->second;
    const float radius = (std::max)(0.1f, burningRadius_);
    config.emitPos = GetTranslate();
    config.shapeRadius *= radius;
    config.emitArea = config.emitArea * radius;
    config.baseSize *= radius;
    config.midSize *= radius;
    config.endSize *= radius;
    config.velocityVariance *= radius;
    config.emitCount = static_cast<std::int32_t>((std::min)(12.0f, 3.0f + std::sqrt(radius) * 2.0f));

    // 炎を進行方向の後ろへなびかせ、弾を単なる発光球に見せないようにします。
    const float speed = engine::Math::Length(config.emitVelocity) * std::sqrt(radius);
    // 進行方向を取れる場合は、プリセットの速さを使って炎を弾の後方へ流す。
    if (engine::Math::Length(velocity_) > 0.001f) {
        config.emitVelocity = engine::Math::Normalize(velocity_) * -speed;
    }
    particles->EmitFromConfig(config);
}

// 着弾の粒子と破片を一度だけ生成し、破片は接触法線が取れればその面から離れる側へ出す。
void Bullet::EmitImpactVfx() {
    // すでに着弾を出した弾や、粒子・破片のどちらも設定されていない弾は処理しない。
    if (impactEmitted_ || (impactPreset_.empty() && impactSecondaryPreset_.empty() &&
                           impactDebrisPreset_.empty() && impactDebrisSecondaryPreset_.empty())) {
        return;
    }

    impactEmitted_ = true;
    engine::Vector3 direction = velocity_;

    // 速度から進行方向を求められないときは、上向きを着弾方向にする。
    if (engine::Math::Length(direction) <= 0.001f) {
        direction = {0.0f, 1.0f, 0.0f};
    // 進行方向が取れるときは、速度を単位方向にして粒子へ渡す。
    } else {
        direction = engine::Math::Normalize(direction);
    }

    // GPUパーティクルが初期化済みのとき、着弾の主・副粒子を順に調べる。
    if (auto* particles = GpuParticleManager::GetInstance(); particles && particles->IsInitialized()) {
        for (const std::string* preset : {&impactPreset_, &impactSecondaryPreset_}) {
            // 空の着弾プリセットは未設定なので、この枠の発生を飛ばす。
            if (preset->empty()) {
                continue;
            }

            // 方向付きに設定されている場合は、進行方向と速度倍率も指定して発生させる。
            if (impactDirected_) {
                particles->EmitDirected(*preset, GetTranslate(), direction, trailSpeedScale_);
            // 方向付きの指定がなければ、位置だけを渡してプリセットの発生方向を使う。
            } else {
                particles->Emit(*preset, GetTranslate());
            }
        }
    }

    // 破片の管理器を取得できた場合は、主・副の破片設定を調べる。
    if (auto* debris = DebrisEffectManager::GetInstance()) {
        for (const std::string* preset : {&impactDebrisPreset_, &impactDebrisSecondaryPreset_}) {
            // 破片のプリセット名が設定されていない枠は処理しない。
            if (preset->empty()) {
                continue;
            }

            DebrisEffectConfig config;
            const auto found = vfxAssetPath::FindPreset(debris->GetPresets(), *preset);
            // 読み込み済みの破片プリセットを見つけたときは、その設定を複製して使う。
            if (found != debris->GetPresets().end()) {
                config = found->second;
            // 未登録のプリセットをファイルからも読み込めなければ、この破片は発生できない。
            } else if (!debris->LoadConfig("Resources/json/debris/" + *preset + ".json", config)) {
                continue;
            // 新しく読み込めた場合は、次回そのファイルを読み直さないよう登録する。
            } else {
                debris->RegisterPreset(*preset, config);
            }

            // 破片のプリセットへ発射時に指定された倍率を掛け、寸法と初速を着弾の大きさへ合わせる。
            const float scale = impactDebrisScale_;
            config.scaleMin *= scale;
            config.scaleMax *= scale;
            config.spawnOffset = config.spawnOffset * scale;
            config.spawnBoxHalfExtents = config.spawnBoxHalfExtents * scale;
            config.speedMin *= std::sqrt(scale);
            config.speedMax *= std::sqrt(scale);
            // 壁面の法線が取れたときは破片を壁から離れる側へ寄せ、飛散幅も狭めます。
            engine::Vector3 scatterDirection = {-direction.x, 0.0f, -direction.z};
            // 衝突法線に方向がある場合だけ、水平成分から壁面側の発生方向を求める。
            if (engine::Math::Length(impactNormal_) > 0.001f) {
                engine::Vector3 wallNormal = {impactNormal_.x, 0.0f, impactNormal_.z};
                // 法線の水平成分が十分にあれば、水平方向へ正規化して飛散に使う。
                if (engine::Math::Length(wallNormal) > 0.001f) {
                    wallNormal = engine::Math::Normalize(wallNormal);
                    // 水平法線が初期の飛散方向と反対を向いていれば、弾が飛んできた側へ向きを揃える。
                    if (wallNormal.x * scatterDirection.x + wallNormal.z * scatterDirection.z < 0.0f)
                        wallNormal = wallNormal * -1.0f;
                    scatterDirection = wallNormal;
                    config.horizontalSpread *= 0.45f;
                }
            }
            config.baseDirection = scatterDirection;
            debris->SpawnFromConfig(config, GetTranslate());
        }
    }
}

// 弾を停止し、直撃したプレイヤーと爆風内にいるプレイヤーへ重複しないよう通知する。
void Bullet::Explode(engine::Object3D* directHit) {
    // すでに終了した弾は、爆発の演出やダメージが重複しないよう処理を止める。
    if (isDead_)
        return;

    // 大きい弾でも爆風半径は広げず、実際に接触した相手だけを直撃として保証する。
    const bool hasDirectPlayerHit = directHit &&
        (directHit->GetCollisionAttribute() & engine::collisionCategories::Get("player")) &&
        (GetCollisionMask() & directHit->GetCollisionAttribute()) &&
        (directHit->GetCollisionMask() & GetCollisionAttribute());

    // 直撃と爆風が同じフレームに二重加算されないよう、先に弾を無効化する。
    isDead_ = true;
    SetCollisionAttribute(0);
    SetCollisionMask(0);
    const engine::Vector3 center = GetTranslate();
    EmitImpactVfx();

    // メッシュ演出の管理器があるとき、爆発位置に閃光を置き、衝撃波は見つけた地面へ移す。
    if (auto* effects = MeshEffectManager::GetInstance()) {
        const float core = explosionRadius_ / 3.0f;
        const float ring = explosionRadius_ / 5.8f;
        effects->SpawnEffectAt("Resources/json/effect/shared/effect_bomb_core_flash.json", center, {0, 0, 0},
                               {core, core, core});
        engine::PhysicsQueryFilter groundFilter;
        groundFilter.mask = engine::collisionCategories::Get("ground");
        groundFilter.ignoredObject = this;
        const auto ground =
            engine::CollisionManager::GetInstance()->Raycast(center, {0, -1, 0}, explosionRadius_ + 1.0f, groundFilter);
        const engine::Vector3 ringPosition = ground.isHit ? ground.hitPoint + engine::Vector3{0, 0.06f, 0} : center;
        effects->SpawnEffectAt("Resources/json/effect/shared/effect_bomb_shockwave_ring.json", ringPosition, {0, 0, 0},
                               {ring, ring, ring});
    }

    // GPUパーティクルが初期化済みなら、閃光と衝撃波に炎・火花・煙を重ねる。
    if (auto* particles = GpuParticleManager::GetInstance(); particles && particles->IsInitialized()) {
        particles->Emit("shared/hit_bomb_fire_core", center);
        particles->Emit("shared/hit_bomb_fire_sparks", center);
        particles->Emit("shared/hit_bomb_orange_smoke", center);
    }

    engine::PhysicsQueryFilter filter;
    filter.mask = engine::collisionCategories::Get("player");
    filter.ignoredObject = this;
    std::unordered_set<engine::Object3D*> damaged;

    auto damageTarget = [&](engine::Object3D* target, bool direct) {
        // 対象の参照がない場合や、この爆発ですでに処理した相手は通知しない。
        if (!target || !damaged.insert(target).second)
            return;
        engine::Vector3 delta = target->GetWorldPosition() - center;
        engine::PhysicsQueryFilter wallFilter;
        wallFilter.mask = engine::collisionCategories::Get("ground");
        wallFilter.ignoredObject = this;
        const float distance = engine::Math::Length(delta);

        // 直撃ではない候補だけ、爆発中心と対象の間に遮る地面がないか確認します。
        if (!direct && distance > 0.05f) {
            const auto wall = engine::CollisionManager::GetInstance()->Raycast(center, delta / distance, distance, wallFilter);
            // 火点のすぐ近くや対象の直前で拾った面を除き、両者の間に壁がある場合は爆風を遮ります。
            if (wall.isHit && wall.distance > 0.05f && wall.distance < distance - 0.1f)
                return;
        }

        DamageEvent event;
        event.target = target;
        event.attacker = this;
        event.damageAmount = damage_;
        event.damageType = DamageType::kExplosion;
        event.invincibilityDuration = 1.0f; // 爆風の被弾後も、通常の砲弾と同じ無敵時間を設ける。
        EventManager::GetInstance()->Dispatch(event);
    };

    // 相手がプレイヤーで双方のマスクが接触を許可した場合は、範囲照会より先に直撃を処理する。
    if (hasDirectPlayerHit)
        damageTarget(directHit, true);
    // 直撃で通知済みの相手も照会に含まれ得るため、同じ damageTarget で重複を除く。
    for (const auto& hit : engine::CollisionManager::GetInstance()->OverlapSphere(center, explosionRadius_, filter))
        damageTarget(hit.object, false);
}
