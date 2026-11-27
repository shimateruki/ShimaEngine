#include "game/actor/enemy/boss/prism/EnemyPrismSlime.Internal.h"

#include <cstdint>
#include "game/actor/enemy/boss/core/BossChantVisual.h"
#include "game/system/scene/SceneGameplayAccess.h"

using namespace enemyPrismSlimeImplementation;

// プリズムスライムの技ごとに、予告位置、演出の更新、命中、召喚個体の登録を扱う。
void EnemyPrismSlime::PreparePrismSpikePoints() {
    // 主棘の候補は標的の位置を優先し、射程より遠いときはボスからの水平距離を制限する。
    const EnemyAttackDefinition& attack = GetCurrentAttackDefinition();
    const engine::Vector3 casterPosition = GetWorldPosition();
    engine::Vector3 requestedCenter =
        casterPosition + lockedDirection_ * std::clamp(lockedTargetDistance_, 0.0f, (std::max)(attack.maxRange, 0.1f));
    // 標的が存在すれば現在位置から主棘の候補を決める。標的がいなければ攻撃開始時の方向と距離を使う。
    if (GetTarget()) {
        requestedCenter = GetTarget()->GetWorldPosition();
        engine::Vector3 fromCaster = requestedCenter - casterPosition;
        fromCaster.y = 0.0f;
        const float targetDistance = std::sqrt(fromCaster.x * fromCaster.x + fromCaster.z * fromCaster.z);

        // 水平距離が射程を超えていて向きを計算できる場合だけ、主棘を射程端まで手前に寄せる。
        if (targetDistance > attack.maxRange && targetDistance > 0.001f) {
            requestedCenter = casterPosition + fromCaster * (attack.maxRange / targetDistance);
        }
    }

    // 攻撃の原点と主棘の位置を地表へ投影し、半径と射程から扇形の角度を求める。
    prismSpellOrigin_ = FindGroundPoint(casterPosition);
    prismSpellCenter_ = FindGroundPoint(requestedCenter);
    prismSpikeSector_ = CreateSpellEffect(kPrismSpikeSectorEffect);
    const float sine = std::clamp(attack.radius / (std::max)(attack.maxRange, 0.1f), 0.0f, 0.95f);
    const float sectorAngle = 2.0f * std::asin(sine);

    // 扇形の表示を作れた場合に、判定と同じ全角を度へ変換してメッシュへ反映する。
    if (prismSpikeSector_) {
        prismSpikeSector_->SetMeshSetting(&EffectObject3D::MeshSettings::slashAngle, sectorAngle * (180.0f / kPi));
        prismSpikeSector_->UpdateProceduralMesh();
    }

    // 三重の弧と左右の境界線で、予告と同じ扇形を魔法陣として描きます。
    for (std::size_t sigilIndex = 0; sigilIndex < prismSpikeSigil_.size(); ++sigilIndex) {
        auto& sigil = prismSpikeSigil_[sigilIndex];
        sigil = CreateSpellEffect(kPrismSpikeSectorEffect);
        // 魔法陣の一部を作れなければ、その弧または境界線だけを省く。
        if (!sigil)
            continue;
        sigil->SetBlendMode(BlendMode::kAdd);
        sigil->SetMeshSetting(&EffectObject3D::MeshSettings::innerRadius, sigilIndex < 3 ? 0.975f : 0.06f);
        sigil->SetMeshSetting(&EffectObject3D::MeshSettings::slashAngle, sigilIndex < 3 ? sectorAngle * (180.0f / kPi) : 1.25f);
        sigil->UpdateProceduralMesh();
    }

    // 距離に比例して列を増やし、扇形の外周まで隙間を均等に埋めます。
    const float halfAngle = std::asin(std::clamp(attack.radius / (std::max)(attack.maxRange, 0.1f), 0.0f, 0.95f));
    std::size_t index = 0;

    for (std::int32_t row = 0; row < 8; ++row) {
        const std::int32_t columns = 4 + row * 2;
        const float distance = attack.maxRange * (0.14f + static_cast<float>(row) * 0.118f);

        for (std::int32_t column = 0; column < columns; ++column) {
            const float fraction =
                (static_cast<float>(column) + (row % 2 == 0 ? 0.5f : 0.22f)) / static_cast<float>(columns);
            const float angle = halfAngle * (fraction * 2.0f - 1.0f);
            const engine::Vector3 direction = RotatePlanar(lockedDirection_, angle);
            prismSpikePositions_[index++] = FindGroundPoint(prismSpellOrigin_ + direction * distance);
        }
    }

    // 最後の主棘は標的付近に置き、扇形の中で詠唱の焦点を作ります。
    prismSpikePositions_.back() = prismSpellCenter_;
}

std::unique_ptr<engine::Object3D> EnemyPrismSlime::CreatePrismSpikeObject(std::int32_t index) const {
    // 棘は攻撃の視覚部品であり、接触判定は別の扇形攻撃で処理する。
    // 3D共通基盤がなければ表示用モデルを初期化できないため、空を返す。
    if (!GetObjectCommon()) {
        return nullptr;
    }

    auto spike = std::make_unique<engine::Object3D>();
    spike->Initialize(GetObjectCommon());
    spike->SetName("PrismSpike_" + std::to_string(index));
    spike->SetClassName("EnemyVisualPart");
    spike->SetModel("Effects/prism_crystal_spike");
    spike->SetColliderType(engine::ColliderType::kNone);
    spike->SetCollisionAttribute(0);
    spike->SetCollisionMask(0);
    spike->SetMaterialType(kPrismCrystalMaterialType);

    constexpr std::array<engine::Vector4, 3> kCrystalTints = {
        engine::Vector4{0.34f, 0.20f, 0.74f, 1.0f},
        engine::Vector4{0.20f, 0.43f, 0.78f, 1.0f},
        engine::Vector4{0.66f, 0.22f, 0.70f, 1.0f},
    };

    spike->SetColor(kCrystalTints[static_cast<std::size_t>(index) % kCrystalTints.size()]);
    spike->SetMetallic(0.94f);
    spike->SetRoughness(0.11f);
    spike->SetEnableEnvMap(true);
    spike->SetEnvIntensity(1.12f);
    spike->SetEmissive(0.96f);
    spike->SetBlendMode(BlendMode::kNone);
    return spike;
}

void EnemyPrismSlime::SpawnNextPrismSpike() {
    // 予告済みの座標に一棘ずつ立て、最後の主棘だけ大きさと演出を強める。
    // 未設定や最後の棘を超えた番号では生成位置を取得できない。
    if (actionIndex_ < 0 || actionIndex_ >= static_cast<std::int32_t>(prismSpikePositions_.size())) {
        return;
    }

    PrismSpikeVisual visual;
    visual.object = CreatePrismSpikeObject(actionIndex_);

    // 棘を作れなければ、その地点の発生演出と到達距離の更新を省く。
    if (!visual.object) {
        return;
    }

    visual.index = actionIndex_;

    // 棘は地表より少し下へ置いておき、更新時に伸び上がるようにする。
    const bool isCenterSpike = actionIndex_ == static_cast<std::int32_t>(prismSpikePositions_.size()) - 1;
    const float heightVariation = isCenterSpike ? 1.58f : 0.78f + static_cast<float>((actionIndex_ * 5) % 7) * 0.075f;
    const float widthVariation = isCenterSpike ? 1.38f : 0.92f + static_cast<float>((actionIndex_ * 7) % 5) * 0.06f;
    visual.groundPosition = prismSpikePositions_[actionIndex_];
    visual.fullScale = {widthVariation, 1.22f * heightVariation, widthVariation * 1.86f};
    visual.object->SetTranslate(visual.groundPosition + engine::Vector3{0.0f, -0.24f, 0.0f});
    visual.object->SetRotation({
        0.0f,
        std::atan2(lockedDirection_.x, lockedDirection_.z) + static_cast<float>(actionIndex_ - 2) * 0.13f,
        0.0f,
    });
    visual.object->SetScale({visual.fullScale.x * 0.42f, 0.015f, visual.fullScale.z * 0.42f});
    visual.object->Update(0.0f);
    visual.object->UpdateWorldMatrix(true);

    // 地面の閃光と粒子を間引き、主棘だけは間引きの対象から外す。
    const float groundEffectScale = isCenterSpike ? 1.34f : 0.74f;

    // 地面の閃光は生成先があり、主棘または偶数番の棘の場合だけ出す。
    if ((isCenterSpike || actionIndex_ % 2 == 0) && MeshEffectManager::GetInstance()) {
        MeshEffectManager* effects = MeshEffectManager::GetInstance();
        effects->SpawnEffectAt(kPrismSpikeGroundEffect, visual.groundPosition + engine::Vector3{0.0f, 0.045f, 0.0f},
                               {0.0f, static_cast<float>(actionIndex_) * 0.37f, 0.0f},
                               {groundEffectScale, 1.0f, groundEffectScale});
    }

    const EnemyAttackDefinition& attack = GetCurrentAttackDefinition();
    const char* activePreset = attack.activeVfx.empty() ? kPrismSpikeBurstPreset : attack.activeVfx.c_str();

    // 粒子は主棘と3本ごとに出し、すべての棘に発生させない。
    if (isCenterSpike || actionIndex_ % 3 == 0) {
        EmitDirectedPreset(activePreset, visual.groundPosition + engine::Vector3{0.0f, 0.12f, 0.0f}, {0.0f, 1.0f, 0.0f},
                           isCenterSpike ? 1.04f : 0.68f);
    }

    // 現在の棘の位置より1.15ワールド単位先まで扇形の命中を調べ、棘を成長演出の更新一覧へ渡す。
    engine::Vector3 fromOrigin = visual.groundPosition - prismSpellOrigin_;
    fromOrigin.y = 0.0f;
    ApplyPrismSpellDamage(engine::Math::Length(fromOrigin) + 1.15f);
    prismSpikeVisuals_.push_back(std::move(visual));
}

void EnemyPrismSlime::UpdatePrismSpikeVisuals(float deltaTime) {
    // 棘ごとの経過秒数に合わせて、隆起・待機・破砕を順に進める。
    auto spike = prismSpikeVisuals_.begin();

    while (spike != prismSpikeVisuals_.end()) {
        spike->age += (std::max)(deltaTime, 0.0f);

        engine::Vector3 scale = spike->fullScale;
        engine::Vector3 position = spike->groundPosition;

        // 隆起中は足元が地表へ届くまで高さと幅を増やす。
        if (spike->age < kPrismSpikeRiseDuration) {
            const float progress = SmoothStep01(spike->age / kPrismSpikeRiseDuration);
            const float overshoot = std::sin(progress * kPi) * 0.12f;
            scale.x *= std::lerp(0.42f, 1.0f, progress);
            scale.y *= (std::max)(0.015f, progress + overshoot);
            scale.z *= std::lerp(0.42f, 1.0f, progress);
            position.y -= (1.0f - progress) * 0.24f;
        } else if (spike->age < kPrismSpikeRiseDuration + kPrismSpikeHoldDuration) {
            // 伸び切った棘は輪郭をわずかに揺らし、設置中の状態を見せる。
            const float shimmer = std::sin((spike->age - kPrismSpikeRiseDuration) * 13.0f) * 0.012f;
            scale.x *= 1.0f + shimmer;
            scale.z *= 1.0f - shimmer;
        } else {
            // 壊れる直前に少し膨らませ、破砕の動きにつなぐ。
            const float fractureTime = spike->age - kPrismSpikeRiseDuration - kPrismSpikeHoldDuration;
            const float fracture = SmoothStep01(fractureTime / kPrismSpikeFractureDuration);
            const float tension = std::sin(fracture * kPi);
            scale.x *= 1.0f + tension * 0.075f;
            scale.y *= 1.0f + tension * 0.035f;
            scale.z *= 1.0f + tension * 0.075f;
            position.y += tension * 0.035f;
        }

        spike->object->SetTranslate(position);
        spike->object->SetScale(scale);
        spike->object->Update(deltaTime);

        // このObject3dはSceneの所有物ではないため、通常の一括GPU更新対象に含まれません。
        spike->object->UpdateWorldMatrix(true);

        // 最終姿勢を反映したら、破片を出して棘を一覧から取り除く。
        if (spike->age >= kPrismSpikeLifetime) {
            SpawnPrismSpikeShatter(*spike);
            spike = prismSpikeVisuals_.erase(spike);
        } else {
            // 寿命内の棘は残し、次の要素へ進む。
            ++spike;
        }
    }
}

// 棘の地面と高さを基準に、破砕時の粒子・破片・地面の閃光を出す。
void EnemyPrismSlime::SpawnPrismSpikeShatter(const PrismSpikeVisual& spike) {
    const bool isMainSpike = spike.index == static_cast<std::int32_t>(prismSpikePositions_.size()) - 1;
    const engine::Vector3 burstPosition = spike.groundPosition + engine::Vector3{
                                                             0.0f,
                                                             spike.fullScale.y * 1.18f,
                                                             0.0f,
                                                         };

    // 主棘と3本ごとに破砕演出を出す。破片と地面の閃光は対応する管理クラスがある場合だけ生成する。
    const bool showShatter = isMainSpike || spike.index % 3 == 0;

    if (showShatter && DebrisEffectManager::GetInstance()) {
        DebrisEffectManager* debris = DebrisEffectManager::GetInstance();
        debris->SpawnOnGround(kPrismSpikeShatterDebrisPreset, burstPosition, spike.groundPosition.y + 0.02f);

        // 主棘だけは高い位置にも破片を足し、大きな結晶が割れたことを示す。
        if (isMainSpike) {
            debris->SpawnOnGround(kPrismSpikeShatterDebrisPreset,
                                  burstPosition + engine::Vector3{0.0f, spike.fullScale.y * 0.82f, 0.0f},
                                  spike.groundPosition.y + 0.02f);
        }
    }

    // 粒子も破砕演出の対象にだけ出す。
    if (showShatter) {
        EmitDirectedPreset(kPrismSpikeBurstPreset, burstPosition, {0.0f, 1.0f, 0.0f}, isMainSpike ? 1.34f : 0.92f);
    }

    if (showShatter && MeshEffectManager::GetInstance()) {
        MeshEffectManager* effects = MeshEffectManager::GetInstance();
        const float groundFlashScale = isMainSpike ? 1.28f : 0.78f;
        effects->SpawnEffectAt(kPrismSpikeGroundEffect, spike.groundPosition + engine::Vector3{0.0f, 0.05f, 0.0f},
                               {0.0f, spike.age * 0.71f, 0.0f}, {groundFlashScale, 1.0f, groundFlashScale});
    }
}

void EnemyPrismSlime::PrepareCrystalLances() {
    // 前の斉射の部品と命中フラグを消してから、新しい槍と円形魔法陣を用意する。
    crystalLanceVisuals_.clear();
    crystalLanceVisuals_.reserve(kCrystalLanceCount);
    crystalVolleyDamageApplied_ = false;
    crystalCircle_ = CreateCrystalCircleEffect(false);
    crystalCircleGlow_ = CreateCrystalCircleEffect(true);

    for (std::int32_t index = 0; index < kCrystalLanceCount; ++index) {
        CrystalLanceVisual lance;
        lance.object = CreatePrismSpikeObject(index + 10);
        lance.slotIndex = index;

        // 表示部品が作れない枠は登録せず、元の配置番号は各槍に保持する。
        if (!lance.object) {
            continue;
        }

        lance.object->SetName("PrismCrystalLance_" + std::to_string(index));
        lance.object->SetScale({0.01f, 0.01f, 0.01f});
        lance.object->SetTranslate(GetWorldPosition());
        lance.object->Update(0.0f);
        lance.object->UpdateWorldMatrix(true);
        crystalLanceVisuals_.push_back(std::move(lance));
    }
}

std::unique_ptr<EffectObject3D> EnemyPrismSlime::CreateCrystalCircleEffect(bool glow) const {
    // 同じ JSON を通常層と加算発光層で使い、後段で別々の透明度を制御する。
    // 3D共通基盤がなければ通常層も加算層も作れない。
    if (!GetObjectCommon()) {
        return nullptr;
    }

    auto effect = std::make_unique<EffectObject3D>();
    effect->Initialize(GetObjectCommon());

    // 共通の魔法陣データを読めなければ、初期化途中の部品は返さない。
    if (!effect->LoadFromJson(kPrismCrystalLanceCircleEffect)) {
        return nullptr;
    }

    effect->SetName(glow ? "PrismCrystalCircleGlow" : "PrismCrystalCircle");

    // 発光層だけを加算合成とし、色は画像から取らない。
    if (glow) {
        effect->SetBlendMode(BlendMode::kAdd);
        effect->SetUseTextureColor(false);
    }

    effect->SetRevealProgress(0.0f);
    effect->SetColor({0.92f, 0.97f, 1.0f, 0.0f});
    return effect;
}

std::unique_ptr<EffectObject3D> EnemyPrismSlime::CreateSpellEffect(const char* path) const {
    // 演出データの再生状態だけ準備し、実際の表示時刻は攻撃状態が決める。
    // 3D共通基盤がなければ演出用オブジェクトを作れない。
    if (!GetObjectCommon()) {
        return nullptr;
    }

    auto effect = std::make_unique<EffectObject3D>();
    effect->Initialize(GetObjectCommon());

    // 指定したJSONを読めなければ、準備中の演出を破棄する。
    if (!effect->LoadFromJson(path)) {
        return nullptr;
    }

    // 演出は再生状態に入れて隠し、攻撃側で表示を切り替える。
    effect->Play(10.0f);
    effect->SetIsVisible(false);
    return effect;
}

void EnemyPrismSlime::UpdatePrismSpikeSectorVisual(float deltaTime) {
    // 結晶棘の予備動作・実行中だけ描く。他の技または段階では作成済みの面、弧、境界線を隠す。
    if (currentAttack_ != AttackKind::kPrismSpikes ||
        (attackState_ != AttackState::kWindup && attackState_ != AttackState::kActive)) {
        if (prismSpikeSector_)
            prismSpikeSector_->SetIsVisible(false);
        for (auto& sigil : prismSpikeSigil_)
            if (sigil)
                sigil->SetIsVisible(false);
        return;
    }

    const bool windup = attackState_ == AttackState::kWindup;
    const float progress =
        windup ? 1.0f - std::clamp(attackTimer_ / (std::max)(attackStateDuration_, 0.001f), 0.0f, 1.0f) : 1.0f;
    const float fade = windup ? 1.0f : 0.48f + 0.52f * SmoothStep01(std::clamp(attackTimer_ / 0.24f, 0.0f, 1.0f));
    const float yaw = std::atan2(lockedDirection_.x, lockedDirection_.z);
    const float extent = GetCurrentAttackDefinition().maxRange;

    // 扇形の面があれば、詠唱時の描き上がりと発動後の減光を表示へ反映する。
    if (prismSpikeSector_) {
        prismSpikeSector_->Update(deltaTime);
        prismSpikeSector_->SetIsVisible(progress > 0.01f && fade > 0.01f);
        prismSpikeSector_->SetTranslate(prismSpellOrigin_ + engine::Vector3{0.0f, 0.045f, 0.0f});
        prismSpikeSector_->SetRotation({0.0f, yaw, 0.0f});
        prismSpikeSector_->SetScale({extent, 1.0f, extent});
        prismSpikeSector_->SetColor({0.32f, 0.68f, 1.0f, fade * (0.12f + progress * 0.18f)});
        prismSpikeSector_->UpdateLocalMatrix();
        prismSpikeSector_->UpdateWorldMatrix();
    }

    const float halfAngle =
        std::asin(std::clamp(GetCurrentAttackDefinition().radius / (std::max)(extent, 0.1f), 0.0f, 0.95f));
    for (std::size_t index = 0; index < prismSpikeSigil_.size(); ++index) {
        auto& sigil = prismSpikeSigil_[index];
        // 未作成の弧や境界線は更新しない。
        if (!sigil)
            continue;
        sigil->Update(deltaTime);
        const float assembly =
            SmoothStep01(std::clamp((progress - static_cast<float>(index) * 0.08f) / 0.60f, 0.0f, 1.0f));
        // 先頭2枚は近距離に置き、両端の2枚だけ基準の向きから左右に開く。
        float distanceScale = 1.0f;
        float rotationOffset = 0.0f;
        if (index == 0) {
            distanceScale = 0.38f;
        } else if (index == 1) {
            distanceScale = 0.68f;
        } else if (index == 3) {
            rotationOffset = -halfAngle;
        } else if (index == 4) {
            rotationOffset = halfAngle;
        }
        const float distance = extent * distanceScale;
        const float rotation = yaw + rotationOffset;
        sigil->SetIsVisible(assembly > 0.01f && fade > 0.01f);
        sigil->SetTranslate(prismSpellOrigin_ + engine::Vector3{0.0f, 0.072f + static_cast<float>(index) * 0.002f, 0.0f});
        sigil->SetRotation({0.0f, rotation, 0.0f});
        sigil->SetScale({distance, 1.0f, distance});
        sigil->SetColor({0.47f, 0.86f, 1.0f, assembly * fade * (index == 2 ? 0.80f : 0.55f)});
        sigil->SetIntensity(1.65f);
        sigil->UpdateLocalMatrix();
        sigil->UpdateWorldMatrix();
    }
}

void EnemyPrismSlime::UpdateWindSpellVisual(float deltaTime) {
    // 竜巻は風の予備動作と実行中だけ更新する。対象外の段階では生成済みの陣・内層・外層を隠す。
    if (currentAttack_ != AttackKind::kWindWave ||
        (attackState_ != AttackState::kWindup && attackState_ != AttackState::kActive)) {
        for (WindVortexVisual& vortex : windVortices_) {
            if (vortex.circle)
                vortex.circle->SetIsVisible(false);
            if (vortex.circleGlow)
                vortex.circleGlow->SetIsVisible(false);
            for (auto& effect : vortex.outer)
                if (effect)
                    effect->SetIsVisible(false);
            for (auto& effect : vortex.inner)
                if (effect)
                    effect->SetIsVisible(false);
        }

        return;
    }

    const bool assembling = attackState_ == AttackState::kWindup;
    const float progress =
        assembling ? 1.0f - std::clamp(attackTimer_ / (std::max)(attackStateDuration_, 0.001f), 0.0f, 1.0f) : 1.0f;
    const float fade = assembling ? 1.0f : 0.55f + 0.45f * SmoothStep01(std::clamp(attackTimer_ / 0.26f, 0.0f, 1.0f));

    for (std::size_t vortexIndex = 0; vortexIndex < windVortices_.size(); ++vortexIndex) {
        WindVortexVisual& vortex = windVortices_[vortexIndex];
        const bool primary = vortexIndex == 0;
        const float delayedProgress =
            std::clamp((progress - static_cast<float>(vortexIndex) * 0.11f) / 0.78f, 0.0f, 1.0f);
        const float reveal = SmoothStep01(delayedProgress);
        const float circleRadius = GetCurrentAttackDefinition().radius * (primary ? 1.0f : 0.72f);

        // 陣は召喚地点に固定し、移動する竜巻とは別の存在として見せます。
        const engine::Vector3 circlePosition = vortex.origin;
        auto updateCircle = [&](std::unique_ptr<EffectObject3D>& circle, bool glow) {
            // 通常層と発光層はそれぞれ、作成済みの場合だけ召喚位置に置く。
            if (!circle)
                return;
            circle->Update(deltaTime);
            circle->SetRevealProgress(reveal);
            circle->SetIsVisible(reveal > 0.01f);
            circle->SetTranslate(circlePosition + engine::Vector3{0.0f, glow ? 0.077f : 0.065f, 0.0f});
            circle->SetRotation({0.0f, idleTimer_ * (glow ? -0.70f : 0.46f), 0.0f});
            const float radius = circleRadius * (0.72f + reveal * 0.28f);
            circle->SetScale({radius, 1.0f, radius});
            circle->SetColor(glow ? engine::Vector4{0.45f, 1.0f, 0.78f, reveal * fade * 0.24f}
                                  : engine::Vector4{0.88f, 1.0f, 0.94f, reveal * fade * 0.78f});
            circle->UpdateLocalMatrix();
            circle->UpdateWorldMatrix();
        };

        updateCircle(vortex.circle, false);
        updateCircle(vortex.circleGlow, true);

        // 詠唱中は地面の魔法陣だけを描き、竜巻本体の層は発動後に順次開く。
        if (assembling)
            continue;
        const float localAge = windTornadoAge_ - static_cast<float>(vortexIndex) * 0.15f;
        const float appear = SmoothStep01(std::clamp(localAge / 0.26f, 0.0f, 1.0f));

        // 生成済みの外層を高さの異なる帯として重ね、発生後は渦の中心に追従させる。
        for (std::size_t layer = 0; layer < vortex.outer.size(); ++layer) {
            auto& effect = vortex.outer[layer];
            if (!effect)
                continue;
            effect->Update(deltaTime);
            effect->SetIsVisible(appear > 0.01f);
            const float height = (primary ? 0.42f : 0.32f) + static_cast<float>(layer) * (primary ? 1.42f : 1.08f);
            const float radius =
                (primary ? 1.18f + static_cast<float>(layer) * 0.74f : 0.86f + static_cast<float>(layer) * 0.49f) *
                appear;
            effect->SetTranslate(vortex.position + engine::Vector3{0.0f, height, 0.0f});
            effect->SetRotation(
                {0.0f, localAge * (primary ? 14.0f : -16.0f) + static_cast<float>(layer) * 1.65f, 0.0f});
            effect->SetScale({radius, primary ? 2.2f : 1.8f, radius});
            effect->SetColor({0.74f, 1.0f, 0.92f, fade * appear * 0.72f});
            effect->UpdateLocalMatrix();
            effect->UpdateWorldMatrix();
        }

        // 生成済みの内層は色と回転速度を変え、外層と区別する。
        for (auto& effect : vortex.inner) {
            if (!effect)
                continue;
            effect->Update(deltaTime);
            effect->SetIsVisible(appear > 0.01f);
            effect->SetTranslate(vortex.position + engine::Vector3{0.0f, primary ? 2.90f : 2.15f, 0.0f});
            effect->SetRotation({0.0f, -localAge * 20.0f + static_cast<float>(vortexIndex) * 1.4f, 0.0f});
            const float radius = (primary ? 2.62f : 1.78f) * appear;
            effect->SetScale({radius, primary ? 3.4f : 2.6f, radius});
            effect->SetColor({0.38f, 0.86f, 1.0f, fade * appear * 0.60f});
            effect->UpdateLocalMatrix();
            effect->UpdateWorldMatrix();
        }
    }
}

engine::Vector3 EnemyPrismSlime::GetChantSigilCenter() const {
    // 詠唱の印を本体の前方に置き、攻撃先の予告とは区別する。
    return GetWorldPosition() + lockedDirection_ * std::clamp(baseScale_.x * 1.75f, 1.7f, 2.8f) +
           engine::Vector3{0.0f, baseScale_.y * 0.55f + 1.05f, 0.0f};
}

void EnemyPrismSlime::UpdateChantSigilVisual(float deltaTime) {
    // 攻撃状態から詠唱共通部品の進行率を作り、見た目を本体に追従させる。
    // 共通詠唱部品が生成されていなければ、攻撃状態から表示値を組み立てる必要はない。
    if (!chantVisual_) {
        return;
    }

    BossChantFrame frame;
    frame.casterPosition = GetWorldPosition();
    frame.groundPosition = FindGroundPoint(frame.casterPosition);
    const engine::Aabb bodyBounds = GetModelWorldAabb();
    frame.casterRadius = (std::max)({(bodyBounds.max.x - bodyBounds.min.x) * 0.5f,
                                     (bodyBounds.max.z - bodyBounds.min.z) * 0.5f, baseScale_.x * 0.84f});
    frame.casterHeight = (std::max)(bodyBounds.max.y - bodyBounds.min.y, baseScale_.y * 1.20f);
    frame.sigilCenter = GetChantSigilCenter();
    frame.forward = lockedDirection_;
    frame.casting = attackState_ == AttackState::kWindup;
    frame.releasing = attackState_ == AttackState::kActive;
    frame.windupProgress =
        frame.casting ? 1.0f - std::clamp(attackTimer_ / (std::max)(attackStateDuration_, 0.001f), 0.0f, 1.0f) : 1.0f;
    frame.activeElapsed = frame.releasing ? (std::max)(0.0f, attackStateDuration_ - attackTimer_) : 0.0f;
    frame.time = idleTimer_;
    frame.deltaTime = deltaTime;
    chantVisual_->Update(frame);
}

void EnemyPrismSlime::UpdateCrystalCircleVisual(float deltaTime) {
    // 陣の通常層を作れなければ発光層も進めず、晶槍の予兆・実行中だけ表示する。
    if (!crystalCircle_) {
        return;
    }

    // 晶槍の予備動作・実行中以外では、通常層と作成済みの加算層を隠す。
    if (currentAttack_ != AttackKind::kCrystalLanceVolley ||
        (attackState_ != AttackState::kWindup && attackState_ != AttackState::kActive)) {
        crystalCircle_->SetIsVisible(false);

        if (crystalCircleGlow_) {
            crystalCircleGlow_->SetIsVisible(false);
        }

        return;
    }

    const bool assembling = attackState_ == AttackState::kWindup;
    const float windupProgress =
        assembling ? 1.0f - std::clamp(attackTimer_ / (std::max)(attackStateDuration_, 0.001f), 0.0f, 1.0f) : 1.0f;

    // 予備動作で描き上げ、発射後の余光を短く残します。
    const float assembly = SmoothStep01(std::clamp((windupProgress - 0.04f) / 0.84f, 0.0f, 1.0f));
    const float activeElapsed = assembling ? 0.0f : (std::max)(0.0f, attackStateDuration_ - attackTimer_);
    const float fade = assembling ? 1.0f : 1.0f - SmoothStep01(std::clamp(activeElapsed / 0.32f, 0.0f, 1.0f));

    // 本体足元の地面を取り直し、陣を水平に置く。
    crystalCircle_->Update(deltaTime);
    crystalCircle_->SetRevealProgress(assembly);
    crystalCircle_->SetIsVisible(fade > 0.01f && assembly > 0.001f);
    const engine::Vector3 ground = FindGroundPoint(GetWorldPosition());
    const float radiusScale = std::clamp(baseScale_.x * 1.45f, 1.8f, 3.1f);
    const float completionPulse = SmoothStep01(std::clamp((windupProgress - 0.82f) / 0.18f, 0.0f, 1.0f));
    crystalCircle_->SetTranslate(ground + engine::Vector3{0.0f, 0.08f, 0.0f});
    crystalCircle_->SetRotation({0.0f, idleTimer_ * 0.11f, 0.0f});
    crystalCircle_->SetScale(
        {radiusScale * (0.91f + assembly * 0.09f), 1.0f, radiusScale * (0.91f + assembly * 0.09f)});
    crystalCircle_->SetColor({0.92f, 0.97f, 1.0f, (0.35f + assembly * 0.40f + completionPulse * 0.15f) * fade});
    crystalCircle_->UpdateLocalMatrix();
    crystalCircle_->UpdateWorldMatrix();

    // 加算層があれば通常層より上へ置き、描き上がりが24%を超えてから発光させる。
    if (crystalCircleGlow_) {
        crystalCircleGlow_->Update(deltaTime);
        crystalCircleGlow_->SetRevealProgress(assembly);
        crystalCircleGlow_->SetIsVisible(fade > 0.01f && assembly > 0.24f);
        crystalCircleGlow_->SetTranslate(ground + engine::Vector3{0.0f, 0.092f, 0.0f});
        crystalCircleGlow_->SetRotation({0.0f, idleTimer_ * 0.11f, 0.0f});
        crystalCircleGlow_->SetScale(
            {radiusScale * (0.94f + assembly * 0.09f), 1.0f, radiusScale * (0.94f + assembly * 0.09f)});
        crystalCircleGlow_->SetColor({0.42f, 0.78f, 1.0f, (0.10f + assembly * 0.12f + completionPulse * 0.11f) * fade});
        crystalCircleGlow_->SetIntensity(1.7f);
        crystalCircleGlow_->UpdateLocalMatrix();
        crystalCircleGlow_->UpdateWorldMatrix();
    }
}

void EnemyPrismSlime::LaunchNextCrystalLance() {
    // 発射番号に対応する槍があり、まだ飛行を始めていない場合だけ射出する。
    if (actionIndex_ < 0 || actionIndex_ >= static_cast<std::int32_t>(crystalLanceVisuals_.size())) {
        return;
    }

    CrystalLanceVisual& lance = crystalLanceVisuals_[static_cast<std::size_t>(actionIndex_)];

    // 表示する実体がないか飛行開始済みなら、射出を重複させない。
    if (!lance.object || lance.launched) {
        return;
    }

    // 発射時の標的の高さを使って照準を確定し、標的が無ければ固定方向を使う。
    const EnemyAttackDefinition& attack = GetCurrentAttackDefinition();
    engine::Vector3 aimPosition = lance.position + lockedDirection_ * (std::max)(attack.maxRange, 1.0f);

    if (GetTarget()) {
        aimPosition = GetTarget()->GetWorldPosition() + engine::Vector3{0.0f, 0.72f, 0.0f};
    }

    engine::Vector3 launchDirection = aimPosition - lance.position;

    // 射出口と狙い点がほぼ同じなら固定方向を使い、十分離れていれば方向を正規化する。
    if (engine::Math::Length(launchDirection) <= 0.001f) {
        launchDirection = lockedDirection_;
    } else {
        launchDirection = engine::Math::Normalize(launchDirection);
    }

    // 発射方向から速度と初期姿勢を確定し、未発射の組み立て更新から飛行更新に切り替える。
    lance.velocity = launchDirection * (std::max)(12.0f, attack.maxSpeed);
    lance.launched = true;
    lance.age = 0.0f;
    lance.trailTimer = 0.0f;
    lance.object->SetRotation(MakeUpAxisAimRotation(launchDirection));
    lance.object->SetScale({0.42f, 0.72f, 0.42f});
    lance.object->SetEmissive(3.1f);

    // 陣を抜ける位置で閃光を出し、晶槍が魔法陣から射出されたようにつなぎます。
    EmitDirectedPreset(kPrismLanceReleasePreset, lance.position + lockedDirection_ * 0.40f, launchDirection, 1.0f);
    const char* trailPreset = attack.activeVfx.empty() ? kPrismLanceTrailPreset : attack.activeVfx.c_str();
    EmitDirectedPreset(trailPreset, lance.position, launchDirection * -1.0f, 1.25f);
}

// 未発射の槍の組み立てと、発射済みの槍の移動・命中・破砕を更新する。deltaTimeは秒。
void EnemyPrismSlime::UpdateCrystalLanceVisuals(float deltaTime) {
    // 今回更新する晶槍がなければ、攻撃設定の取得も省く。
    if (crystalLanceVisuals_.empty()) {
        return;
    }

    // 予備動作中は残り時間から結晶の組み上がり率を作り、発射済みとは別の更新へ進めます。
    const EnemyAttackDefinition& attack = GetAttackDefinition(kCrystalLanceVolleyAttackId);
    float revealProgress = 1.0f;

    if (currentAttack_ == AttackKind::kCrystalLanceVolley && attackState_ == AttackState::kWindup) {
        revealProgress = 1.0f - std::clamp(attackTimer_ / (std::max)(attackStateDuration_, 0.001f), 0.0f, 1.0f);
    }

    const engine::Vector3 side = {lockedDirection_.z, 0.0f, -lockedDirection_.x};

    auto lance = crystalLanceVisuals_.begin();

    while (lance != crystalLanceVisuals_.end()) {
        // 表示部品を失った記録は、移動させる前に一覧から取り除く。
        if (!lance->object) {
            lance = crystalLanceVisuals_.erase(lance);
            continue;
        }

        // 未発射の槍は陣の奥で育て、標的へ照準を合わせながら発光と周囲の粒子を増やします。
        if (!lance->launched) {
            const float centeredSlot =
                static_cast<float>(lance->slotIndex) - static_cast<float>(kCrystalLanceCount - 1) * 0.5f;
            const float arch = 1.0f - std::abs(centeredSlot) * 0.17f;
            const float bob = std::sin(idleTimer_ * 4.6f + static_cast<float>(lance->slotIndex) * 0.9f) * 0.10f;
            const float buildStart = 0.10f + static_cast<float>(lance->slotIndex) * 0.095f;
            const float lanceBuild = SmoothStep01(std::clamp((revealProgress - buildStart) / 0.50f, 0.0f, 1.0f));

            // 陣の奥で結晶を作り、完成するにつれて射出口へ寄せます。
            lance->position = GetChantSigilCenter() - lockedDirection_ * (0.82f - lanceBuild * 0.42f) +
                              side * centeredSlot * 1.02f + engine::Vector3{0.0f, arch * 0.22f + bob, 0.0f};

            engine::Vector3 aimPosition = lance->position + lockedDirection_ * (std::max)(attack.maxRange, 1.0f);

            // 標的がいる間は、組み立て中もその胴体付近へ照準を向ける。
            if (GetTarget()) {
                aimPosition = GetTarget()->GetWorldPosition() + engine::Vector3{0.0f, 0.72f, 0.0f};
            }

            engine::Vector3 aimDirection = aimPosition - lance->position;

            // 槍と狙い点が近すぎて向きを決められなければ、攻撃開始時の向きに戻す。
            if (engine::Math::Length(aimDirection) <= 0.001f) {
                aimDirection = lockedDirection_;
            }

            const float pulse = std::sin(idleTimer_ * 7.2f + static_cast<float>(lance->slotIndex)) * 0.018f;
            const float charge = lanceBuild * lanceBuild;
            lance->object->SetTranslate(lance->position);
            lance->object->SetRotation(MakeUpAxisAimRotation(aimDirection));
            lance->object->SetScale({
                (0.38f + pulse + charge * 0.035f) * lanceBuild,
                (0.67f - pulse + charge * 0.08f) * lanceBuild,
                (0.38f + pulse + charge * 0.035f) * lanceBuild,
            });
            lance->object->SetEmissive(0.65f + charge * 2.05f);
            // 晶槍の予備動作中で、槍の組み立てが16%を超えた場合だけ周回粒子を出す。
            if (currentAttack_ == AttackKind::kCrystalLanceVolley && attackState_ == AttackState::kWindup &&
                lanceBuild > 0.16f) {
                lance->trailTimer -= deltaTime;

                // 間隔が経過したら粒子を1つ出し、成長に応じて次の発生を早める。
                if (lance->trailTimer <= 0.0f) {
                    const float orbit = idleTimer_ * 5.2f + static_cast<float>(lance->slotIndex) * 2.4f;
                    const engine::Vector3 motePosition =
                        lance->position +
                        engine::Vector3{std::cos(orbit) * 0.48f, std::sin(orbit * 1.3f) * 0.32f, std::sin(orbit) * 0.48f};
                    EmitPreset(kPrismSpellMotesPreset, motePosition);
                    lance->trailTimer += 0.17f - charge * 0.045f;
                }
            }

            lance->object->Update(deltaTime);
            lance->object->UpdateWorldMatrix(true);
            ++lance;
            continue;
        }

        // 発射後は速度で進め、前位置と現位置を使って速い槍の接触すり抜けを防ぎます。
        lance->age += (std::max)(deltaTime, 0.0f);

        // 発射の閃光だけを強くし、飛行中は結晶の輪郭が読める明るさへ戻します。
        lance->object->SetEmissive(1.28f + 1.82f * std::exp(-lance->age * 13.0f));
        lance->trailTimer -= deltaTime;
        const engine::Vector3 previousPosition = lance->position;
        lance->position += lance->velocity * deltaTime;

        // 飛行中の発生間隔を過ぎたら、現在位置から進行方向の逆へ軌跡を出す。
        if (lance->trailTimer <= 0.0f) {
            const char* trailPreset = attack.activeVfx.empty() ? kPrismLanceTrailPreset : attack.activeVfx.c_str();
            EmitDirectedPreset(trailPreset, lance->position, lance->velocity * -1.0f, 1.0f);
            lance->trailTimer += kCrystalLanceTrailInterval;
        }

        bool hitTarget = false;

        // 標的がいるときだけ、前位置から現位置までの線分と標的中心との距離を判定する。
        if (GetTarget()) {
            const engine::Vector3 targetCenter = GetTarget()->GetWorldPosition() + engine::Vector3{0.0f, 0.72f, 0.0f};
            const float hitRadius = (std::max)(0.90f, GetTarget()->GetCollisionRadius() + 0.34f);
            hitTarget =
                DistanceSquaredToSegment(targetCenter, previousPosition, lance->position) <= hitRadius * hitRadius;
        }

        // 一斉射の複数の槍が重なっても、PlayerへのDamageEventは一度だけ送ります。
        if (hitTarget && !crystalVolleyDamageApplied_) {
            const engine::Vector3 knockbackDirection = NormalizePlanar(lance->velocity);
            DamageEvent event;
            event.target = GetTarget();
            event.attacker = this;
            event.damageAmount = attack.damage;
            event.damageType = DamageType::kPhysical;
            event.knockbackVelocity = {
                knockbackDirection.x * 5.5f,
                4.2f,
                knockbackDirection.z * 5.5f,
            };

            EventManager::GetInstance()->Dispatch(event);
            crystalVolleyDamageApplied_ = true;
        }

        // 命中と寿命終了のどちらでも破片を出し、配列から消して次の更新対象にしません。
        const bool expired = lance->age >= (std::max)(0.3f, attack.lifetime);

        if (hitTarget || expired) {
            SpawnCrystalLanceShatter(*lance);
            lance = crystalLanceVisuals_.erase(lance);
            continue;
        }

        // 命中も寿命切れもしていない槍は、現在の位置と速度を描画へ反映して残す。
        lance->object->SetTranslate(lance->position);
        lance->object->SetRotation(MakeUpAxisAimRotation(lance->velocity));
        lance->object->Update(deltaTime);
        lance->object->UpdateWorldMatrix(true);
        ++lance;
    }
}

void EnemyPrismSlime::SpawnCrystalLanceShatter(const CrystalLanceVisual& lance) {
    // 命中または寿命切れの位置で破砕エフェクトを出す。破片の着地高さには真下の地面を使う。
    const EnemyAttackDefinition& attack = GetAttackDefinition(kCrystalLanceVolleyAttackId);
    const engine::Vector3 groundPosition = FindGroundPoint(lance.position);

    // 破片の管理クラスがある場合だけ、地面の高さを指定して破片を出す。
    if (DebrisEffectManager* debris = DebrisEffectManager::GetInstance()) {
        debris->SpawnOnGround(kPrismSpikeShatterDebrisPreset, lance.position, groundPosition.y + 0.02f);
    }

    const char* impactPreset = attack.impactVfx.empty() ? kPrismSpikeBurstPreset : attack.impactVfx.c_str();
    EmitDirectedPreset(impactPreset, lance.position, lance.velocity * -1.0f, 1.08f);
}

void EnemyPrismSlime::ClearPrismAttackVisuals() {
    // 死亡・遭遇演出・プレビューの再生成では、未発射のイベントも取り消します。
    attackSequence_.Cancel();
    prismSpikeVisuals_.clear();
    crystalLanceVisuals_.clear();
    crystalCircle_.reset();
    crystalCircleGlow_.reset();
    prismSpikeSector_.reset();

    for (auto& sigil : prismSpikeSigil_)
        sigil.reset();
    for (WindVortexVisual& vortex : windVortices_) {
        vortex.circle.reset();
        vortex.circleGlow.reset();

        for (auto& effect : vortex.outer)
            effect.reset();
        for (auto& effect : vortex.inner)
            effect.reset();
    }

    chantVisual_.reset();
    summonPortalVisuals_.clear();
}

void EnemyPrismSlime::ApplyPrismSpellDamage(float reachedDistance) {
    // 標的がいないかこの技ですでに命中していれば終了する。標的がいれば、伸びた扇形の内側かを調べる。
    if (!GetTarget() || damageApplied_) {
        return;
    }

    const EnemyAttackDefinition& attack = GetCurrentAttackDefinition();
    engine::Vector3 difference = GetTarget()->GetWorldPosition() - prismSpellOrigin_;
    difference.y = 0.0f;
    const float distance = std::sqrt(difference.x * difference.x + difference.z * difference.z);
    const float forward = engine::Math::Dot(difference, lockedDirection_);
    const float halfAngle = std::asin(std::clamp(attack.radius / (std::max)(attack.maxRange, 0.1f), 0.0f, 0.95f));
    // 最大射程・到達距離・角度のどれかを外れた標的を除く。原点付近は角度を計算せず通す。
    if (distance > attack.maxRange || distance > reachedDistance ||
        (distance > 0.001f && forward / distance < std::cos(halfAngle))) {
        return;
    }

    // 扇形内で最も近い棘の地面を基準にし、棘より高い足場への貫通を防ぐ。
    float nearestDistanceSq = (std::numeric_limits<float>::max)();
    float nearbyGroundY = prismSpellOrigin_.y;

    for (std::int32_t i = 0; i <= actionIndex_ && i < static_cast<std::int32_t>(prismSpikePositions_.size()); ++i) {
        const engine::Vector3& spike = prismSpikePositions_[i];
        const float dx = spike.x - GetTarget()->GetWorldPosition().x;
        const float dz = spike.z - GetTarget()->GetWorldPosition().z;
        const float distanceSq = dx * dx + dz * dz;

        // 水平距離が最小の発生点を更新し、高さ判定に使う地面を選ぶ。
        if (distanceSq < nearestDistanceSq) {
            nearestDistanceSq = distanceSq;
            nearbyGroundY = spike.y;
        }
    }

    const engine::Aabb targetBounds = GetTarget()->GetAabb();
    constexpr float kSpikeReachHeight = 2.4f;
    // 標的の上下端が地面から-0.3～+2.4の範囲と重ならなければ命中させない。
    if (targetBounds.min.y > nearbyGroundY + kSpikeReachHeight ||
        targetBounds.max.y < nearbyGroundY - 0.3f) {
        return;
    }

    const engine::Vector3 direction = distance > 0.001f ? NormalizePlanar(difference) : lockedDirection_;
    DamageEvent event;
    event.target = GetTarget();
    event.attacker = this;
    event.damageAmount = attack.damage;
    event.damageType = DamageType::kPhysical;
    event.knockbackVelocity = {
        direction.x * 5.5f,
        10.5f,
        direction.z * 5.5f,
    };

    EventManager::GetInstance()->Dispatch(event);
    damageApplied_ = true;
}

void EnemyPrismSlime::FireNextFanProjectile() {
    // 中央を基準に角度をずらし、同じ攻撃定義から各弾の威力と射程を取る。
    const EnemyAttackDefinition& attack = GetCurrentAttackDefinition();
    const float centerIndex = static_cast<float>(kFireFanProjectileCount - 1) * 0.5f;
    const float fanAngle = (static_cast<float>(actionIndex_) - centerIndex) * 0.13f;
    engine::Vector3 direction = RotatePlanar(lockedDirection_, fanAngle);

    engine::Vector3 spawnPosition = GetWorldPosition() + direction * (baseScale_.x * 0.48f);
    spawnPosition.y += baseScale_.y * 0.48f + std::abs(static_cast<float>(actionIndex_) - centerIndex) * 0.10f;
    const float speed = std::clamp(lockedTargetDistance_ * 1.55f + 4.0f, (std::max)(11.0f, attack.minSpeed),
                                   (std::max)(22.0f, attack.maxSpeed));
    const float lifetime = attack.lifetime > 0.0f ? attack.lifetime : 2.8f;

    BulletManager::GetInstance()->Fire(spawnPosition, direction * speed + engine::Vector3{0.0f, 0.45f, 0.0f}, engine::collisionCategories::Get("enemy_attack"),
                                       engine::collisionCategories::Get("player") | engine::collisionCategories::Get("ground"), "Primitives/sphere", (std::max)(0.42f, attack.radius),
                                       lifetime, MakePrismFireVisual(), attack.damage, MakeBurningStatus(attack),
                                       DamageType::kFire);
    EmitDirectedPreset(kFireCastPreset, spawnPosition, direction, 1.15f);
}

void EnemyPrismSlime::StrikeNextThunderPoint() {
    // 雷の見た目は地面から上へ伸ばし、判定は水平半径と柱の高さで限定する。
    // 予告地点の配列外の番号では雷を出さない。
    if (actionIndex_ < 0 || actionIndex_ >= static_cast<std::int32_t>(thunderStrikePositions_.size())) {
        return;
    }

    const engine::Vector3 groundPosition = thunderStrikePositions_[actionIndex_];
    const float yaw = std::atan2(lockedDirection_.x, lockedDirection_.z);
    constexpr float kStrikeHeight = 10.5f;
    engine::Vector3 strikeCenter = groundPosition;
    strikeCenter.y += kStrikeHeight * 0.5f;

    // メッシュ演出が使える場合は、交差する雷柱と地面の着弾跡を重ねる。
    if (MeshEffectManager* effects = MeshEffectManager::GetInstance()) {
        effects->SpawnEffectAt(kThunderBoltEffect, strikeCenter, {kHalfPi, yaw, 0.0f}, {1.15f, 1.18f, 1.15f});
        effects->SpawnEffectAt(kThunderBoltEffect, strikeCenter, {kHalfPi, yaw + kHalfPi, 0.0f}, {0.76f, 1.18f, 0.90f});
        effects->SpawnEffectAt(kThunderCoreEffect, strikeCenter, {0.0f, 0.0f, 0.0f}, {1.18f, 1.18f, 1.18f});
        effects->SpawnEffectAt(kThunderImpactEffect, groundPosition + engine::Vector3{0.0f, 0.055f, 0.0f}, {0.0f, yaw, 0.0f},
                               {1.35f, 1.0f, 1.35f});
        effects->SpawnEffectAt(kThunderScorchEffect, groundPosition + engine::Vector3{0.0f, 0.04f, 0.0f}, {0.0f, yaw, 0.0f});
    }

    EmitPreset(kThunderImpactPreset, groundPosition + engine::Vector3{0.0f, 0.22f, 0.0f});

    // 標的がいないか今回の連鎖ですでに命中していれば、エフェクトだけで終える。
    if (!GetTarget() || damageApplied_) {
        return;
    }

    const EnemyAttackDefinition& attack = GetCurrentAttackDefinition();
    engine::Vector3 difference = GetTarget()->GetWorldPosition() - groundPosition;
    const float verticalDistance = difference.y;
    difference.y = 0.0f;

    // 標的の位置が水平半径か高さ範囲を外れていればダメージを送らない。
    if (engine::Math::Length(difference) > attack.radius || verticalDistance < -0.6f || verticalDistance > kStrikeHeight) {
        return;
    }

    DamageEvent event;
    event.target = GetTarget();
    event.attacker = this;
    event.damageAmount = attack.damage;
    event.damageType = DamageType::kElectric;
    event.knockbackVelocity = {lockedDirection_.x * 8.0f, 7.5f, lockedDirection_.z * 8.0f};
    EventManager::GetInstance()->Dispatch(event);
    damageApplied_ = true;
}

void EnemyPrismSlime::ApplyWindTornado() {
    // 標的がいて未命中の場合に、実体化済みの渦から半径に対する中心までの比率が最も小さいものを選ぶ。
    if (!GetTarget() || damageApplied_) {
        return;
    }

    const EnemyAttackDefinition& attack = GetCurrentAttackDefinition();
    engine::Vector3 difference{};
    float distance = 0.0f;
    float hitRadius = 0.0f;
    bool hit = false;

    // 渦ごとの立ち上がり時刻を過ぎた候補について、半径と上下方向の範囲を調べる。
    for (std::size_t index = 0; index < windVortices_.size(); ++index) {
        if (windTornadoAge_ < 0.16f + static_cast<float>(index) * 0.15f)
            continue;
        const WindVortexVisual& vortex = windVortices_[index];
        engine::Vector3 candidate = GetTarget()->GetWorldPosition() - vortex.position;
        const float verticalDistance = candidate.y;
        candidate.y = 0.0f;
        const float candidateDistance = engine::Math::Length(candidate);
        const float candidateRadius = attack.radius * (index == 0 ? 1.0f : 0.72f);
        // 当たり範囲内で、最初の候補か、それまでの候補より半径に対する距離が小さい場合に採用する。
        if (candidateDistance <= candidateRadius && verticalDistance >= -0.6f &&
            verticalDistance <= (index == 0 ? 6.2f : 5.0f) &&
            (!hit || candidateDistance / candidateRadius < distance / hitRadius)) {
            difference = candidate;
            distance = candidateDistance;
            hitRadius = candidateRadius;
            hit = true;
        }
    }

    // どの渦の当たり範囲にも入らなければ、外力とダメージは発生しない。
    if (!hit) {
        return;
    }

    // 中心では方向が定まらないため攻撃開始時の向きを使い、外側ほど押し出しを弱めます。
    const engine::Vector3 pushDirection = distance > 0.001f ? NormalizePlanar(difference) : lockedDirection_;
    const float falloff = 1.0f - std::clamp(distance / (std::max)(hitRadius, 0.1f), 0.0f, 1.0f);

    // 標的が外力を受けられるCharacterなら、ダメージ通知とは別に吹き上げを加える。
    if (Character* character = dynamic_cast<Character*>(GetTarget())) {
        character->ApplyExternalImpulse(
            {
                pushDirection.x * (13.0f + falloff * 6.0f),
                4.8f + falloff * 2.0f,
                pushDirection.z * (13.0f + falloff * 6.0f),
            },
            0.25f);
    }

    EmitDirectedPreset(kWindImpactPreset, GetTarget()->GetWorldPosition() + engine::Vector3{0.0f, 0.72f, 0.0f}, pushDirection,
                       1.25f);

    // 外力とDamageEventは別経路で渡し、既に命中した後は再送しません。
    DamageEvent event;
    event.target = GetTarget();
    event.attacker = this;
    event.damageAmount = attack.damage;
    event.damageType = DamageType::kPhysical;
    event.knockbackVelocity = {pushDirection.x * 15.0f, 7.0f, pushDirection.z * 15.0f};
    EventManager::GetInstance()->Dispatch(event);
    damageApplied_ = true;
}

void EnemyPrismSlime::PrepareThunderPoints() {
    // 開始時の向きに沿って落雷地点を固定し、各地点へ先に予告を出す。
    const EnemyAttackDefinition& attack = GetAttackDefinition(kThunderChainAttackId);
    const engine::Vector3 origin = GetWorldPosition();
    const float spacing = (std::max)(2.0f, attack.maxRange / 6.4f);

    for (std::size_t index = 0; index < thunderStrikePositions_.size(); ++index) {
        const float distance = 3.2f + spacing * static_cast<float>(index);
        thunderStrikePositions_[index] = FindGroundPoint(origin + lockedDirection_ * distance);

        // 表示用メッシュが使える場合だけ、命中半径に合わせた予告を地面へ出す。
        if (MeshEffectManager* effects = MeshEffectManager::GetInstance()) {
            effects->SpawnEffectAt(kThunderWarningEffect, thunderStrikePositions_[index] + engine::Vector3{0.0f, 0.045f, 0.0f},
                                   {0.0f, std::atan2(lockedDirection_.x, lockedDirection_.z), 0.0f},
                                   {attack.radius, 1.0f, attack.radius});
        }
    }
}

void EnemyPrismSlime::PrepareSummonPortals() {
    // 古い陣を破棄し、今回の召喚番号から配置と敵種の乱数を作る。
    summonPortalVisuals_.clear();
    summonPortalVisuals_.reserve(kSummonPortalCount);

    const EnemyAttackDefinition& attack = GetCurrentAttackDefinition();
    const engine::Vector3 center = GetWorldPosition();
    const float placementRadius = (std::max)(3.6f, attack.radius);
    std::uint32_t randomState = 0x9e3779b9u ^ ((summonRandomSerial_ + 1u) * 0x85ebca6bu);
    const float startAngle = NextSummonRandom01(randomState) * kPi * 2.0f;

    // 術者の周囲を分割し、角度と距離を小さくずらして陣を置く。
    for (std::int32_t index = 0; index < kSummonPortalCount; ++index) {
        const float spreadAngle = (kPi * 2.0f * static_cast<float>(index)) / static_cast<float>(kSummonPortalCount);
        const float angleJitter = (NextSummonRandom01(randomState) - 0.5f) * 0.58f;
        // 各召喚点は角度に加えて距離もずらし、中心から配置半径の62～94%の範囲で地面を探します。
        const float distanceRate = 0.62f + NextSummonRandom01(randomState) * 0.32f;
        const float angle = startAngle + spreadAngle + angleJitter;

        engine::Vector3 samplePosition = center + engine::Vector3{
                                              std::sin(angle) * placementRadius * distanceRate,
                                              0.0f,
                                              std::cos(angle) * placementRadius * distanceRate,
                                          };

        SummonPortalVisual portal;
        portal.groundPosition = FindGroundPoint(samplePosition);
        portal.effect = CreateSummonPortalEffect(false);
        portal.glow = CreateSummonPortalEffect(true);

        // 通常層と発光層のうち、生成できた部品を非展開から始める。
        if (portal.effect) {
            portal.effect->SetRevealProgress(0.0f);
        }

        if (portal.glow) {
            portal.glow->SetRevealProgress(0.0f);
        }

        // 発動前に召喚する種類を確定し、対応する発光色で予告する。
        const std::int32_t kindIndex = (std::min)(3, static_cast<std::int32_t>(NextSummonRandom01(randomState) * 4.0f));
        portal.slimeKind = static_cast<SummonSlimeKind>(kindIndex);

        switch (portal.slimeKind) {
        case SummonSlimeKind::kFire:
            // 炎を示す赤橙色で出現する種類を予告する。
            portal.color = {1.0f, 0.20f, 0.035f, 0.90f};
            break;

        case SummonSlimeKind::kBomber:
            // 暗い敵色は発光層で読めなくなるため、紫のハイライトで種類を示します。
            portal.color = {0.62f, 0.33f, 0.92f, 0.90f};
            break;

        case SummonSlimeKind::kPink:
            // ピンクスライムの色を召喚の予告に使う。
            portal.color = {1.0f, 0.24f, 0.68f, 0.90f};
            break;

        case SummonSlimeKind::kThunder:
            // 雷を示す黄色で召喚を予告する。
            portal.color = {1.0f, 0.86f, 0.055f, 0.94f};
            break;
        }

        // 通常層を作れた場合は地面の少し上へ置き、最初は小さく透明にしておく。
        if (portal.effect) {
            portal.effect->SetTranslate(portal.groundPosition + engine::Vector3{0.0f, 0.065f, 0.0f});
            portal.effect->SetScale({0.48f, 1.0f, 0.48f});
            portal.effect->SetColor({1.0f, 1.0f, 1.0f, 0.0f});
            portal.effect->UpdateLocalMatrix();
            portal.effect->UpdateWorldMatrix();
        }

        // 表示素材の成否とは別に、召喚地点と敵の種類を発動処理へ渡す。
        summonPortalVisuals_.push_back(std::move(portal));
    }

    // 次回の召喚では乱数の種を替え、同じ色と配置が繰り返されないようにする。
    ++summonRandomSerial_;
}

std::unique_ptr<EffectObject3D> EnemyPrismSlime::CreateSummonPortalEffect(bool glow) const {
    // 召喚陣は結晶円の材質を使い、名前だけを用途に合わせて区別する。
    auto effect = CreateCrystalCircleEffect(glow);

    // 生成に成功した部品には、発光層か通常層かを判別できる名前を付ける。
    if (effect) {
        effect->SetName(glow ? "PrismSlimeSummonGlow" : "PrismSlimeSummonCircle");
    }

    return effect;
}

// 召喚陣を予備動作中に組み立て、敵を出した後の回復段階で閉じる。deltaTimeは秒。
void EnemyPrismSlime::UpdateSummonPortalVisuals(float deltaTime) {
    // 陣の記録が空なら描画更新を省く。
    if (summonPortalVisuals_.empty()) {
        return;
    }

    // 予備動作の経過に合わせて魔法陣の組み上がり率を作り、発光層にも同じ進行を渡します。
    float revealProgress = 1.0f;

    if (currentAttack_ == AttackKind::kSlimeSummon && attackState_ == AttackState::kWindup) {
        revealProgress = 1.0f - std::clamp(attackTimer_ / (std::max)(attackStateDuration_, 0.001f), 0.0f, 1.0f);
    }

    const float easedReveal = SmoothStep01(revealProgress);

    for (std::size_t index = 0; index < summonPortalVisuals_.size(); ++index) {
        SummonPortalVisual& portal = summonPortalVisuals_[index];

        // 通常層がない陣は見た目の進行を省く。敵を出す処理は SpawnNextSummonedSlime() が担当する。
        if (!portal.effect) {
            continue;
        }

        // 通常層の時間を進め、発光層があれば同じ時間と描き上がり率を渡す。
        portal.age += deltaTime;
        portal.effect->Update(deltaTime);

        if (portal.glow) {
            portal.glow->Update(deltaTime);
        }

        const float assembly = SmoothStep01(std::clamp((revealProgress - 0.04f) / 0.84f, 0.0f, 1.0f));
        portal.effect->SetRevealProgress(assembly);

        if (portal.glow) {
            portal.glow->SetRevealProgress(assembly);
        }

        // 通常層と発光層の不透明度を分け、完成に向けて明るさを増やします。
        float scale = 1.36f * (0.42f + easedReveal * 0.58f);
        float alpha = 0.26f + assembly * 0.58f;
        float glowAlpha = portal.color.w * (0.06f + assembly * 0.18f);
        float intensity = 1.05f + assembly * 0.68f;

        if (portal.spawned) {
            // 敵が出現しても陣を残し、召喚動作が終わる回復フェーズで閉じます。
            if (attackState_ == AttackState::kRecovery) {
                portal.spawnedAge += deltaTime;
            }

            const float fade = std::clamp(portal.spawnedAge / kSummonPortalFadeDuration, 0.0f, 1.0f);
            scale *= 1.0f + fade * 0.36f;
            alpha *= (1.0f - fade) * (1.0f - fade);
            glowAlpha *= (1.0f - fade) * (1.0f - fade);
            intensity += (1.0f - fade) * 0.55f;

            // フェードが最後まで進んだら、存在する両方の層を隠して更新を終了する。
            if (fade >= 1.0f) {
                portal.effect->SetIsVisible(false);

                if (portal.glow) {
                    portal.glow->SetIsVisible(false);
                }

                continue;
            }
        } else {
            // 召喚前は小さな呼吸と周回粒子を続け、ゲートが動いていることを示します。
            const float pulse = std::sin(portal.age * (5.5f + easedReveal * 7.0f) + static_cast<float>(index) * 1.7f);
            scale *= 1.0f + pulse * (0.015f + easedReveal * 0.035f);
            alpha *= 0.91f + pulse * 0.09f;
            glowAlpha *= 0.85f + pulse * 0.15f;
            intensity += (0.5f + pulse * 0.5f) * assembly * 0.30f;
            // 召喚技の詠唱中で、陣が十分に開いた場合だけ周回粒子の発生間隔を進める。
            if (currentAttack_ == AttackKind::kSlimeSummon && attackState_ == AttackState::kWindup &&
                easedReveal > 0.10f) {
                portal.moteTimer -= deltaTime;

                // 発生間隔が経過したら陣の縁から粒子を出し、描き上がりに応じて間隔を縮める。
                if (portal.moteTimer <= 0.0f) {
                    const float orbit = portal.age * 4.0f + static_cast<float>(index) * 2.1f;
                    const float orbitRadius = scale * 0.78f;
                    EmitPreset(kPrismSpellMotesPreset,
                               portal.groundPosition +
                                   engine::Vector3{std::cos(orbit) * orbitRadius, 0.13f, std::sin(orbit) * orbitRadius});
                    portal.moteTimer += 0.23f - easedReveal * 0.12f;
                }
            }
        }

        // 通常層を地面に置き、作成済みの発光層は少し高く大きく重ねる。
        portal.effect->SetIsVisible(assembly > 0.001f);
        portal.effect->SetTranslate(portal.groundPosition + engine::Vector3{0.0f, 0.065f, 0.0f});
        portal.effect->SetRotation(
            {0.0f, portal.age * (0.46f + easedReveal * 0.92f) * (index == 1 ? -1.0f : 1.0f), 0.0f});
        portal.effect->SetScale({scale, 1.0f, scale});
        portal.effect->SetColor({0.94f, 0.98f, 1.0f, alpha});
        portal.effect->SetIntensity(intensity);
        portal.effect->UpdateLocalMatrix();
        portal.effect->UpdateWorldMatrix();

        if (portal.glow) {
            portal.glow->SetIsVisible(assembly > 0.24f);
            portal.glow->SetTranslate(portal.groundPosition + engine::Vector3{0.0f, 0.077f, 0.0f});
            portal.glow->SetRotation(
                {0.0f, portal.age * (0.46f + easedReveal * 0.92f) * (index == 1 ? -1.0f : 1.0f), 0.0f});
            portal.glow->SetScale({scale * 1.035f, 1.0f, scale * 1.035f});
            portal.glow->SetColor({portal.color.x, portal.color.y, portal.color.z, glowAlpha});
            portal.glow->SetIntensity(1.65f + assembly * 0.50f);
            portal.glow->UpdateLocalMatrix();
            portal.glow->UpdateWorldMatrix();
        }
    }
}

void EnemyPrismSlime::SpawnNextSummonedSlime() {
    // 予兆時に選んだスライム種を実体化し、同じ門から二重召喚しない。
    // 生成対象の番号が陣の一覧外なら敵の種類と出現位置を取得できない。
    if (actionIndex_ < 0 || actionIndex_ >= static_cast<std::int32_t>(summonPortalVisuals_.size())) {
        return;
    }

    SummonPortalVisual& portal = summonPortalVisuals_[static_cast<std::size_t>(actionIndex_)];

    // 出現処理が済んだ陣では敵を増やさない。
    if (portal.spawned) {
        return;
    }

    // 陣が保持する種類をファクトリーのIDと召喚時の粒子へ変換する。
    const char* enemyType = "Slime";
    const char* nameSuffix = "Pink";
    const char* spawnPreset = kPrismPhasePulsePreset;

    switch (portal.slimeKind) {
    case SummonSlimeKind::kFire:
        // 炎はFireSlimeを使い、火炎の発生粒子で出現を見せる。
        enemyType = "FireSlime";
        nameSuffix = "Fire";
        spawnPreset = kFireCastPreset;
        break;

    case SummonSlimeKind::kBomber:
        // ボムはBomberを使い、結晶棘の予告粒子を出現に流用する。
        enemyType = "Bomber";
        nameSuffix = "Bomb";
        spawnPreset = kPrismSpikeWarningPreset;
        break;

    case SummonSlimeKind::kPink:
        // ピンクは既定のSlimeと発生粒子をそのまま使う。
        break;

    case SummonSlimeKind::kThunder:
        // 雷はThunderSlimeを使い、帯電の粒子で出現を見せる。
        enemyType = "ThunderSlime";
        nameSuffix = "Thunder";
        spawnPreset = kThunderChargePreset;
        break;
    }

    std::unique_ptr<BaseEnemy> enemy = EnemyFactory::GetInstance()->CreateEnemy(enemyType, GetObjectCommon());

    // Factoryが敵を生成できなければ、陣は未発動のまま戻る。
    if (!enemy) {
        return;
    }

    // 地面に置き、ボスと同じ標的を与えて駆け出せる状態にする。
    enemy->SetName(std::string("PrismSummon_") + nameSuffix + "_" + std::to_string(summonRandomSerial_) + "_" +
                   std::to_string(actionIndex_));
    enemy->SetTranslate(portal.groundPosition + engine::Vector3{0.0f, 0.12f, 0.0f});
    enemy->SetVelocity({0.0f, 0.0f, 0.0f});
    enemy->SetGrounded(true);
    enemy->SetDetectionRange(24.0f);
    enemy->SetTarget(GetTarget());

    // 召喚に成功した陣だけを発動済みに変え、描画側の閉じる処理が動けるようにする。
    portal.spawned = true;
    portal.spawnedAge = 0.0f;
    EmitPreset(spawnPreset, portal.groundPosition + engine::Vector3{0.0f, 0.24f, 0.0f});

    for (std::int32_t mote = 0; mote < 4; ++mote) {
        const float angle = (kPi * 2.0f * static_cast<float>(mote)) / 4.0f;
        EmitPreset(kPrismSpellMotesPreset,
                   portal.groundPosition + engine::Vector3{std::cos(angle) * 0.72f, 0.18f, std::sin(angle) * 0.72f});
    }

    // 設定済みの個体の所有権を、プレビューの登録先または実行中のシーンへ渡す。
    SpawnSummonedSlime(std::move(enemy));
}

void EnemyPrismSlime::SpawnSummonedSlime(std::unique_ptr<BaseEnemy> enemy) {
    // プレビューはコールバックへ、本番は現シーンへ所有権を移す。
    // 生成した敵がなければ、プレビューにもシーンにも渡せない。
    if (!enemy) {
        return;
    }

    // プレビュー用の登録先が指定されていれば、実行中のシーンへは登録しない。
    if (spawnCallback_) {
        spawnCallback_(std::move(enemy));
        return;
    }

    SceneManager* sceneManager = SceneManager::GetInstance();

    // 本番の登録先シーンがまだ有効でなければ、未登録の敵をそのまま破棄する。
    if (!sceneManager || !sceneManager->GetCurrentScene()) {
        return;
    }

    enemy->SetTarget(GetScenePlayer(sceneManager->GetCurrentScene()));
    sceneManager->GetCurrentScene()->AddObject(std::move(enemy));
}
