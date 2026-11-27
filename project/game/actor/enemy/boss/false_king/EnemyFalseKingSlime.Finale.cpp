#include <cstdint>
#include "game/actor/enemy/boss/false_king/EnemyFalseKingSlime.Internal.h"

#include "game/actor/enemy/boss/core/BossChantVisual.h"
#include "DebrisEffectManager.h"
#include "game/crown/CrownBarrageCamera.h"

using namespace enemyFalseKingSlimeImplementation;

namespace {
// 結晶の召喚間隔と障壁が完成するまでの演出時間。単位は秒。
constexpr float kSealStagger = .15f;
constexpr float kSealCrystalStart = .10f;
constexpr float kSealCrystalForm = .75f;
constexpr float kSealFirstTransfer = .38f;
constexpr float kSealBarrierStart = kSealCrystalStart + kSealCrystalForm + kSealFirstTransfer;
constexpr float kSealBarrierForm = .50f;
constexpr float kSealCrystalScale = 1.42f;
constexpr float kSealCrystalCenter = 2.05f;
constexpr float kSealCrystalHalfHeight = 1.10f;
constexpr float kSealBreakFlashSeconds = .055f;
constexpr float kSealSurgeRemaining = 3.0f; // 操作復帰後、床の電流が強まり始める残り秒数。
constexpr const char* kSealChantMotes = "enemy/false_king/false_king_seal_chant_motes";
constexpr const char* kSealCrystalBreakPreset = "shared/false_king_seal_crystal_break";
// ボスが所有する結晶。敵の当たり判定を使うので近接・弾・範囲攻撃を共通経路で受けます。
class CrownSealTarget final : public BaseEnemy {
  public:
    void Update(float deltaTime) override {
        // 結晶も通常の敵ダメージ間隔を使い、連続接触で一瞬に壊れないようにします。
        SetDamageCooldown(std::max(0.0f, GetDamageCooldown() - deltaTime));
        engine::Object3D::Update(deltaTime);
    }
};

void UpdateFinaleLayer(EffectObject3D* effect, float deltaTime, bool visible, const engine::Vector3& position,
                       const engine::Vector3& scale, const engine::Vector4& color, const engine::Vector3& rotation = {}) {
    // 生成された演出層だけ時間と姿勢を進め、表示切替の後で描画前の行列を確定する。
    if (!effect)
        return;
    effect->Update(deltaTime);
    effect->SetIsVisible(visible);
    effect->SetTranslate(position);
    effect->SetRotation(rotation);
    effect->SetScale(scale);
    effect->SetColor(color);
    effect->UpdateLocalMatrix();
    effect->UpdateWorldMatrix(true);
}

// XZ平面のオーラをカメラへ向け、横から見ても光の厚みを保ちます。
engine::Vector3 FinaleBillboardRotation(const engine::Vector3& center) {
    // アクティブカメラがあれば中心から視点への向きをラジアンで返す。なければ既定の姿勢を使う。
    if (const auto* camera = CameraManager::GetInstance()->GetActiveCamera()) {
        const engine::Vector3 toEye = camera->GetEye() - center;
        return {kPi * 0.5f - std::atan2(toEye.y, std::sqrt(toEye.x * toEye.x + toEye.z * toEye.z)),
                std::atan2(toEye.x, toEye.z), 0.0f};
    }

    return {kPi * 0.5f, 0.0f, 0.0f};
}
} // namespace

// 偽王の封印戦で結晶・障壁・槍雨を更新し、王冠の継承後に最終形態へ戦闘を引き渡す。
float EnemyFalseKingSlime::FilterFinaleDamage(float damage) {
    // 第一形態の HP 下限、結晶防護、最終形態の装甲倍率を一箇所で適用します。
    // 攻撃プレビューと入口イベント管理外では、決戦用のダメージ制限を適用しません。
    if (!debugPreviewAttackId_.empty() || !IsEncounterControlled())
        return damage;
    // 幕間や形態の切り替え中は、演出進行に必要なHPを維持します。
    if (crownInterlude_ || attackState_ == AttackState::kPhaseShift)
        return 0.0f;
    if (battlePhase_ == 2) {
        // 王本体は障壁に守られる。3個の結晶を直接破壊するまでHPを減らしません。
        return 0.0f;
    }

    // 第一形態で下限の67%を割らないよう、残りHPとの差分までに被害を制限します。
    if (battlePhase_ == 1) {
        return std::clamp(damage, 0.0f, std::max(0.0f, GetEncounterCurrentHp() - GetEncounterMaximumHp() * 0.67f));
    }

    // 最終形態では装甲が開いている間だけ全量を受け、閉じている間は3割に抑えます。
    return std::max(0.0f, damage) * (armorOpenTimer_ > 0.0f ? 1.0f : 0.30f);
}

void EnemyFalseKingSlime::OpenCrownArmor() {
    // 第3形態で幕間の外にいるときだけ弱点を開き、開放の開始をVFXで通知する。
    if (battlePhase_ != 3 || crownInterlude_)
        return;
    armorOpenTimer_ = CrownFinaleTuning::Get().armorOpenTime;
    VfxSequencer::PlayOneShot(kPhaseShiftSequence, GetWorldPosition(), {1.1f, 1.1f, 1.1f});
}

void EnemyFalseKingSlime::ResetCrownFinale() {
    // 第二形態の障壁と槍雨の表示部品を解放してから、結晶の登録を解除する。
    crownShieldRing_.reset();
    crownShieldShell_.reset();
    crownShieldBarrier_.reset();
    crownCastFloor_.reset();
    crownArenaRing_.reset();
    crownBarrageFlash_.reset();
    crownBarrageSkySigil_.reset();
    crownBarrageSkyCounterSigil_.reset();
    crownBarrageSkyOuterRing_.reset();
    crownBarrageSkyInnerRing_.reset();
    sealBarrage_.clear();

    for (auto& layer : crownBarrageLayers_)
        layer.reset();
    // 結晶が存在する場合は衝突管理から外す。全枠の破壊状態も初期値へ戻す。
    for (auto& seal : crownSeals_) {
        if (seal.crystal)
            engine::CollisionManager::GetInstance()->RemoveObject(seal.crystal.get());
        seal = CrownSeal{};
    }

    // 次の再開は結晶の出現から始め、詠唱時間・命中履歴・装甲の開放を引き継ぎません。
    sealState_ = SealState::kReveal;
    sealStateTimer_ = sealVfxTimer_ = 0.0f;
    sealAttackStage_ = 0;
    sealImpactPlayed_ = false;
    brokenSeals_ = 0;
    sealVisualAge_ = 0.0f;
    sealPressureDamage_ = 0.0f;
    sealAttackSerial_ = 0;
    armorOpenTimer_ = 0.0f;
    crownInterlude_ = false;
    finalArenaStarted_ = false;
    sealTargetsLocked_ = false;
    sealBarrageDamageApplied_ = false;
}

void EnemyFalseKingSlime::InitializeCrownSeals() {
    // 三つの結晶を通常の敵判定経路へ登録し、攻撃・描画・演出層をまとめて準備します。
    const auto& tuning = CrownFinaleTuning::Get();

    // 再生成時は既存の結晶を衝突管理から外し、古い判定を残さない。
    for (auto& seal : crownSeals_) {
        if (seal.crystal)
            engine::CollisionManager::GetInstance()->RemoveObject(seal.crystal.get());
        seal = CrownSeal{};
    }

    sealBarrage_.clear();
    sealBarrageDamageApplied_ = false;
    brokenSeals_ = 0;

    // 三基の位置を正三角形で決める。プレビューは現在地、本編は配置位置に調整済みのZオフセットを足した点が中心。
    const engine::Vector3 center = debugPreviewAttackId_ == kSealBarragePreviewId
                               ? GetTranslate()
                               : authoredPosition_ + engine::Vector3{0.0f, 0.0f, tuning.sealCenterOffsetZ};
    for (std::int32_t i = 0; i < 3; ++i) {
        auto& seal = crownSeals_[i];
        const float angle = -kPi / 2.0f + static_cast<float>(i) * kTwoPi / 3.0f;
        seal.position = FindGroundPoint(
            center + engine::Vector3{std::sin(angle) * tuning.sealRadius, 0.0f, std::cos(angle) * tuning.sealRadius});
        seal.broken = false;
        seal.breakAge = 0.0f;
        auto crystal = std::make_unique<CrownSealTarget>();
        crystal->Initialize(GetObjectCommon(), "Effects/false_king_seal_core");
        crystal->SetName("FalseKing_CrownSeal_" + std::to_string(i + 1));
        crystal->SetEnemyType("FalseKingSeal");
        crystal->SetCollisionAttribute(debugPreviewAttackId_ == kSealBarragePreviewId ? 0 : engine::collisionCategories::Get("enemy"));
        // 形が揃うまでは判定を入れず、描画の拡大に対して従来の世界半径を維持する。
        crystal->SetCollisionMask(0);
        crystal->SetColliderType(engine::ColliderType::kSphere);
        crystal->SetCollisionRadius(1.8f / kSealCrystalScale);
        crystal->SetEnableLighting(true);
        crystal->SetEnableEnvMap(true);
        crystal->SetEnvIntensity(0.72f);
        crystal->SetEmissive(1.8f);
        crystal->SetRoughness(0.22f);
        crystal->SetMetallic(0.55f);
        crystal->SetTranslate(seal.position + engine::Vector3{0.0f, 2.0f, 0.0f});
        crystal->SetScale({kSealCrystalScale, kSealCrystalScale, kSealCrystalScale});
        crystal->SetIsVisible(false);
        crystal->UpdateWorldMatrix(true);
        crystal->ResetParameters();
        crystal->SetParameter(&engine::EntityParameter::maxHp, tuning.sealCrystalHp);
        crystal->SetParameter(&engine::EntityParameter::hp, tuning.sealCrystalHp);
        seal.previousHp = tuning.sealCrystalHp;
        seal.crystal = std::move(crystal);
        engine::CollisionManager::GetInstance()->AddObject(seal.crystal.get());
        // 結晶のHPと衝突登録を済ませてから、王冠へ送電するための描画専用部品を作る。
        seal.link = CreateRoyalMesh("CrownSealLink", "Effects/false_king_seal_link", {.29f, .84f, 1.0f, 1.0f}, 1.4f);
        seal.gem = CreateRoyalMesh("CrownSealGem", "Effects/false_king_seal_core",
                                   {.55f, .90f, 1.0f, 1.0f}, 2.0f);
        // 作成できた送電部品だけを初期状態で非表示にし、光の柱には照明の影響を受けない材質を使う。
        for (auto* visual : {seal.link.get(), seal.gem.get()}) {
            if (visual) visual->SetIsVisible(false);
        }
        if (seal.link) {
            seal.link->SetBlendMode(BlendMode::kNone);
            seal.link->SetEnableLighting(false);
        }
    }

    // 封印戦の時計や命中履歴を初期化し、結晶が出現する段階から再開する。
    sealVisualAge_ = 0.0f;
    sealPressureDamage_ = 0.0f;
    sealAttackSerial_ = 0;
    sealTargetsLocked_ = false;
    sealState_ = SealState::kReveal;
    sealStateTimer_ = sealVfxTimer_ = 0.0f;
    sealAttackStage_ = 0;
    sealImpactPlayed_ = false;
    ClearTransientVisuals();
    // 封印中の詠唱はボスについて動く共通部品で準備し、読み込めなければ他の結晶演出だけを進める。
    BossChantStyle chantStyle;
    chantStyle.sigilEffectPath = "Resources/json/effect/enemy/false_king/effect_false_king_seal_chant.json";
    chantStyle.motePreset = kSealChantMotes;
    chantStyle.accentColor = {1.0f, .26f, .61f, 1.0f};
    chantStyle.sigilTint = {1.0f, .30f, .66f};
    chantStyle.radius = std::clamp(baseScale_.x * 1.80f, 3.3f, 5.0f);
    spellChant_ = std::make_unique<BossChantVisual>();
    if (!spellChant_->Start(GetObjectCommon(), std::move(chantStyle)))
        spellChant_.reset();
    // 読み込めた演出層だけを非表示で先に再生し、必要な時刻で表示を切り替えます。
    auto makeShield = [this](const char* path, const char* name) {
        auto effect = std::make_unique<EffectObject3D>();
        effect->Initialize(GetObjectCommon());
        if (!effect->LoadFromJson(path))
            return std::unique_ptr<EffectObject3D>{};
        effect->SetName(name);
        effect->SetIsVisible(false);
        effect->Play(100.0f);
        return effect;
    };

    for (auto& seal : crownSeals_) {
        // 召喚地点だけを示す光の輪。床に残る台座は置かない。
        auto ring = makeShield(kNightfallLancesRingEffect, "FalseKing_CrownSealRevealRing");
        // 召喚地点の輪を生成できた場合だけ、固定色と強度を設定して保存する。
        if (ring) {
            const engine::Vector4 tint{.17f, .85f, 1.0f, .55f};
            ring->SetStartColor(tint);
            ring->SetEndColor(tint);
            ring->SetIntensity(1.35f);
            seal.ring = std::move(ring);
        }
        for (auto& pulse : seal.energyPulses)
            pulse = makeShield(kCrownTransferPulseEffect, "FalseKing_CrownTransferPulse");
    }

    crownShieldRing_ = makeShield(kNightfallLancesRingEffect, "FalseKing_CrownShieldRing");
    crownShieldShell_ = makeShield(kAstralReaverOrbEffect, "FalseKing_CrownShieldShell");

    // 中ボスの封鎖壁と同じ GatePortal マテリアルを、円筒の代理形状で描画します。
    crownShieldBarrier_ = CreateRoyalVisual("FalseKing_CrownBarrier", "Gimmicks/portal_surface",
                                            kGatePortalMaterialType, {0.30f, 0.86f, 1.0f, 0.84f}, 2.4f);
    // 障壁の実体が作れた場合は隠しておき、描画時にカメラ遮蔽で薄くする設定を適用する。
    if (crownShieldBarrier_) {
        crownShieldBarrier_->SetBlendMode(BlendMode::kNormal);
        crownShieldBarrier_->SetCastShadow(false);
        // 敵本体や床の予告は残し、視界を塞ぐ王冠バリアだけを個別に透過許可する。
        crownShieldBarrier_->SetCameraOcclusionMode(engine::Object3D::CameraOcclusionMode::kFade);
        crownShieldBarrier_->SetIsVisible(false);

        // メッシュの専用パラメーターにアクセスできる場合だけ、円筒として描く初期値を渡す。
        if (engine::MeshRenderer* renderer = crownShieldBarrier_->GetMeshRenderer()) {
            if (engine::MeshRenderer::WaterParamForGpu* effect = renderer->GetWaterParamData()) {
                effect->waveSpeed = 1.25f;
                effect->waveHeight = 0.72f;
                effect->waveFrequency = 18.0f;
                effect->effectType = 4.0f;
                effect->effectScale = 1.0f;
                effect->effectSoftness = 0.0f; // 円筒モードでは王冠へ届いた光の強さ。
                effect->effectIntensity = 0.06f;
                effect->billboardScale = 1.0f;
                effect->effectScaleX = 1.0f;
                effect->effectScaleY = 1.0f;
                effect->effectScaleZ = 1.0f;
            }
        }
    }

    // 封印が残っている間の障壁とは別に、失敗後の地面と天空の槍雨用素材を用意します。
    crownCastFloor_ = makeShield(kUmbraFlameSigilEffect, "FalseKing_CrownCastFloor");
    crownArenaRing_ = makeShield("Resources/json/effect/enemy/false_king/effect_false_king_seal_ground_current.json", "FalseKing_CrownArenaCharge");
    crownBarrageFlash_ = makeShield("Resources/json/effect/enemy/false_king/effect_false_king_seal_ground_aura.json", "FalseKing_CrownBarrageFlash");
    crownBarrageSkySigil_ = makeShield("Resources/json/effect/enemy/false_king/effect_false_king_seal_sky_sigil.json", "FalseKing_CrownBarrageSkySigil");
    crownBarrageSkyCounterSigil_ = makeShield("Resources/json/effect/enemy/false_king/effect_false_king_seal_sky_sigil.json", "FalseKing_CrownBarrageSkyCounterSigil");
    crownBarrageSkyOuterRing_ = makeShield(kNightfallLancesRingEffect, "FalseKing_CrownBarrageSkyOuterRing");
    crownBarrageSkyInnerRing_ = makeShield(kNightfallLancesRingEffect, "FalseKing_CrownBarrageSkyInnerRing");
    const char* layerPaths[] = {
        "Resources/json/effect/enemy/false_king/effect_false_king_seal_ground_aura.json",
        "Resources/json/effect/enemy/false_king/effect_false_king_seal_red_aura.json",
        "Resources/json/effect/enemy/false_king/effect_false_king_seal_ground_current.json", "Resources/json/effect/enemy/false_king/effect_false_king_seal_ground_current.json",
        "Resources/json/effect/enemy/false_king/effect_false_king_seal_red_aura.json", kNightfallImpactBeamEffect};

    for (size_t index = 0; index < crownBarrageLayers_.size(); ++index)
        crownBarrageLayers_[index] = makeShield(layerPaths[index], "FalseKing_SealBarrageLayer");
    // 封印破壊や放電を最初に出すフレームのため、破片と粒子システムを先に準備します。
    DebrisEffectManager::GetInstance()->PrewarmPreset("shared/false_king_seal_shatter");
    DebrisEffectManager::GetInstance()->PrewarmPreset(kSealCrystalBreakPreset);
    GpuParticleManager::GetInstance()->PreloadPresetSystems(
        {"enemy/false_king/false_king_seal_embers", "enemy/false_king/false_king_seal_impact", "enemy/false_king/false_king_seal_dissolve",
         "enemy/false_king/false_king_seal_ground_sparks", "enemy/false_king/false_king_seal_air_aura",
         kSealChantMotes});
    HideAttackTelegraph();
}

engine::Vector3 EnemyFalseKingSlime::GetLastBrokenCrownSealPosition() const {
    // 最も最近壊れた結晶を復帰演出の基準にし、未破壊なら既定の位置を返します。
    const CrownSeal* latest = nullptr;

    for (const auto& seal : crownSeals_)
        if (seal.broken && (!latest || seal.breakAge < latest->breakAge))
            latest = &seal;
    return latest ? latest->position + engine::Vector3{0,2,0}
                  : GetTranslate() + engine::Vector3{-CrownFinaleTuning::Get().sealRadius,2,0};
}

void EnemyFalseKingSlime::TryBreakCrownSeal(const engine::Vector3& impact, std::int32_t attackStage, bool force) {
    // HPが尽きた結晶を破壊し、衝突登録を外す。forceによる破壊も形態・番号・着弾位置・重複の検査は行う。
    // 第二形態の本番で未破壊の結晶が残るときだけ処理する。
    if (battlePhase_ != 2 || !debugPreviewAttackId_.empty() || brokenSeals_ >= 3)
        return;
    // 対象番号が配列外なら対応する結晶がない。
    if (attackStage < 0 || attackStage >= static_cast<std::int32_t>(crownSeals_.size()))
        return;
    auto& seal = crownSeals_[attackStage];
    // 強制でない場合は結晶とパラメーターがあり、HPがゼロ以下であることが必要。
    if (!force && (!seal.crystal || !seal.crystal->GetParameters() || seal.crystal->GetParameters()->hp > 0.0f))
        return;
    // 破壊済みの結晶と、着弾点が水平の命中半径を外れるものは除く。
    if (seal.broken ||
        PlanarDistance(impact, seal.position) > CrownFinaleTuning::Get().sealHitRadius)
        return;
    // 描画済みの結晶の中心を破片の発生点に使います。
    const engine::Vector3 origin = seal.crystal ? seal.crystal->GetWorldPosition()
                                               : seal.position + engine::Vector3{0.0f, kSealCrystalCenter, 0.0f};
    seal.broken = true;
    seal.breakAge = 0.0f;
    seal.hitFlash = .16f;

    // 結晶の実体が残っていれば、登録と双方の衝突ビットを無効にする。
    if (seal.crystal) {
        engine::CollisionManager::GetInstance()->RemoveObject(seal.crystal.get());
        seal.crystal->SetCollisionAttribute(0);
        seal.crystal->SetCollisionMask(0);
    }

    // 主カメラがある場合に破壊をシェイクで示し、先に破壊した結晶の数に応じて強くする。
    if (engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera())
        camera->StartShake(0.32f, 0.11f + brokenSeals_ * 0.025f, 24.0f, {0.6f, 0.5f, 0.25f});
    // 結晶の足元を着地面として、破壊確定時に一度だけ破片を飛ばします。
    DebrisEffectManager::GetInstance()->SpawnOnGround(kSealCrystalBreakPreset, origin, seal.position.y);
    ++brokenSeals_;

    // 最後の結晶なら詠唱を止め、王冠継承を待つ段階へ移す。
    if (brokenSeals_ == 3) {
        sealState_ = SealState::kBroken;
        sealStateTimer_ = 0.0f;
        ClearTransientVisuals();
        HideAttackTelegraph();
    }
}

float EnemyFalseKingSlime::GetSealCastRemainingTime() const {
    // 詠唱中以外は UI の残り時間をゼロとして扱います。
    return battlePhase_ == 2 && sealState_ == SealState::kChant
               ? std::max(0.0f, CrownFinaleTuning::Get().sealCastTime - sealStateTimer_)
               : 0.0f;
}

void EnemyFalseKingSlime::UpdateCrownSeals(float deltaTime) {
    // 結晶の HP と見た目を同期し、全破壊または詠唱完了までの進行を管理します。
    const auto& tuning = CrownFinaleTuning::Get();
    // 封印戦ではボスをその場に留め、戦闘段階と粒子の経過秒だけを進めます。
    idleTimer_ += deltaTime;
    sealVisualAge_ += deltaTime;
    sealStateTimer_ += deltaTime;
    sealVfxTimer_ += deltaTime;
    SetVelocity({});
    HideAttackTelegraph();

    // 結晶のHPは既存のダメージイベントが減らす。描画と衝突は同じ位置へ同期します。
    float crownArrivalGlow = 0.0f;
    for (std::int32_t i = 0; i < static_cast<std::int32_t>(crownSeals_.size()); ++i) {
        auto& seal = crownSeals_[i];
        // 足元の輪、結晶、送電を一つの時刻から評価し、3基の開始をずらす。
        const float age = sealVisualAge_ - static_cast<float>(i) * kSealStagger;
        const float ringRise = SmoothStep01(age / .28f);
        const float formation = SmoothStep01((age - kSealCrystalStart) / kSealCrystalForm);
        const bool crystalReady = age >= kSealCrystalStart + kSealCrystalForm;
        const float transferAge = std::max(0.0f, age - kSealCrystalStart - kSealCrystalForm);
        const float lineReveal = std::clamp(transferAge / kSealFirstTransfer, 0.0f, 1.0f);
        seal.hitFlash = std::max(0.0f, seal.hitFlash - deltaTime);
        // 未破壊でHPを参照できる結晶は、ゼロ以下なら破壊へ進め、減少だけなら発光させる。
        if (!seal.broken && seal.crystal && seal.crystal->GetParameters()) {
            const float hp = seal.crystal->GetParameters()->hp;
            if (hp <= 0.0f) {
                TryBreakCrownSeal(seal.position, i);
            } else if (hp < seal.previousHp) {
                // 結晶が残る命中は発光で伝えます。破壊時の発光と破片は TryBreakCrownSeal() が起動します。
                seal.hitFlash = 0.16f;
            }
            seal.previousHp = hp;
        }
        // 破壊後だけ、短い発光を終えるための経過秒を進める。
        if (seal.broken)
            seal.breakAge += deltaTime;

        // HPの反映後に描画を決め、結晶が壊れたフレームで王冠への送電を止めます。
        const bool transfersEnergy = crystalReady && !seal.broken && sealState_ != SealState::kRecover &&
            sealState_ != SealState::kBroken;
        const bool crystalVisible = formation > 0.0f && (!seal.broken || seal.breakAge < kSealBreakFlashSeconds);
        const float pulse = 1.0f + 0.07f * std::sin(idleTimer_ * 4.0f + static_cast<float>(i));
        const engine::Vector3 crystalPosition = seal.position +
            engine::Vector3{0.0f, kSealCrystalCenter + 0.10f * std::sin(idleTimer_ * 2.2f), 0.0f};

        // 実体が残る結晶に、出現・破壊時の拡縮と表示状態を渡す。
        if (seal.crystal) {
            seal.crystal->SetIsVisible(crystalVisible);
            seal.crystal->SetTranslate(crystalPosition);
            const float size = kSealCrystalScale * pulse;
            seal.crystal->SetScale({size, size, size});
            // 形成を終えた本番の未破壊結晶にだけ、プレイヤーの攻撃と弾の判定を有効にする。
            if (crystalReady && !seal.broken && seal.crystal->GetCollisionMask() == 0 &&
                debugPreviewAttackId_ != kSealBarragePreviewId) {
                seal.crystal->SetCollisionMask(engine::collisionCategories::Get("player_attack") |
                    engine::collisionCategories::Get("player_bullet"));
            }

            // 下端から上端を越えるまで切断面を上げ、光る境目に沿って結晶を形成する。
            if (!crystalReady && formation > 0.0f) {
                engine::Object3D::PortalClipSettings clip;
                clip.enabled = true;
                clip.normal = {0, 1, 0};
                clip.progress = .52f / 1.10f; // シェーダーの距離補正を相殺する。
                const float halfHeight = kSealCrystalHalfHeight * size + .16f;
                clip.center = crystalPosition + engine::Vector3{0, std::lerp(-halfHeight, halfHeight, formation), 0};
                clip.edgeWidth = .115f;
                clip.dissolve = .21f;
                clip.color = {.34f, 1.25f, 1.65f, 1.0f};
                seal.crystal->SetPortalClip(clip);
            } else {
                // 形成の途中でなければ、切断面を解除して通常の描画に戻す。
                seal.crystal->ClearPortalClip();
            }
            seal.crystal->SetRotation({0.0f, idleTimer_ * 0.75f, 0.0f});
            const float flash = SmoothStep01(seal.hitFlash / 0.16f);
            seal.crystal->SetColor({std::lerp(.49f, 1.0f, flash), std::lerp(.89f, 1.0f, flash), 1.0f, 1.0f});
            seal.crystal->SetEmissive(1.65f + 0.35f * pulse + flash * 1.5f);
            seal.crystal->Update(deltaTime);
        }

        // 足元の輪が生成済みなら、破壊前だけ召喚位置で現れて消える表示を更新する。
        if (seal.ring) {
            const float ringWeight = ringRise * (1.0f - SmoothStep01((age - .48f) / .35f));
            seal.ring->Update(deltaTime);
            seal.ring->SetIsVisible(!seal.broken && ringWeight > .015f);
            seal.ring->SetTranslate(seal.position + engine::Vector3{0, .12f, 0});
            const float radius = std::lerp(.10f, tuning.sealHitRadius * .69f, ringWeight);
            seal.ring->SetScale({radius, .48f, radius});
            seal.ring->UpdateWorldMatrix(true);
        }

        const engine::Vector3 crown = GetTranslate() + engine::Vector3{(i - 1) * 1.1f, baseScale_.y * 2.0f, 0.0f};
        const engine::Vector3 line = crown - crystalPosition;
        const float length = engine::Math::Length(line);

        // 送電線があれば、結晶から王冠までを描き上がり量に応じて伸ばす。
        if (seal.link) {
            seal.link->SetIsVisible(transfersEnergy && lineReveal > .01f);
            seal.link->SetTranslate(crystalPosition + line * (lineReveal * .5f));
            seal.link->SetScale({.09f, length * lineReveal, .09f});
            seal.link->SetRotation({std::atan2(std::sqrt(line.x * line.x + line.z * line.z), line.y),
                                    std::atan2(line.x, line.z), 0.0f});
            seal.link->SetColor({.25f, .79f, 1.0f, 1.0f});
            seal.link->SetEmissive(1.48f + 0.15f * pulse);
            seal.link->Update(deltaTime);
        }

        // 結晶から王冠へ3つの光点を送り、壊れた結晶からの供給は同じフレームで止める。
        const float trips = transferAge < kSealFirstTransfer ? transferAge / kSealFirstTransfer :
            1.0f + (transferAge - kSealFirstTransfer) * .58f;
        for (size_t pulseIndex = 0; pulseIndex < seal.energyPulses.size(); ++pulseIndex) {
            const float trip = trips - static_cast<float>(pulseIndex) / 3.0f;
            const float travel = std::fmod(std::max(0.0f, trip), 1.0f);
            const float fade = SmoothStep01(std::min(travel, 1.0f - travel) * 10.0f);
            const float size = .38f + .16f * std::sin(travel * kPi);
            UpdateFinaleLayer(seal.energyPulses[pulseIndex].get(), deltaTime, transfersEnergy && trip > 0.0f,
                              crystalPosition + line * travel, {size, size, size},
                              {.58f, 1.18f, 1.50f, fade});
            if (transfersEnergy && trip >= .92f) {
                // 到着前後だけ障壁へ発光を渡し、光点の周回境界で明るさが跳ねないようにする。
                const float arrival = 1.0f - SmoothStep01(std::min(travel, 1.0f - travel) / .075f);
                crownArrivalGlow = std::max(crownArrivalGlow, arrival);
            }
        }

        // 王冠側の宝石がある場合は、送電線が届いた後で点灯させる。
        if (seal.gem) {
            seal.gem->SetIsVisible(transfersEnergy && lineReveal >= 1.0f);
            seal.gem->SetTranslate(crown);
            seal.gem->SetScale({0.28f, 0.35f, 0.28f});
            seal.gem->SetRotation({0.0f, idleTimer_, 0.0f});
            seal.gem->Update(deltaTime);
        }
    }

    // 障壁は出現時に立ち上げ、全破壊後に閉じます。残る結晶の数で光の強さも変えます。
    const float barrierBuild = sealState_ == SealState::kReveal
                                   ? SmoothStep01((sealVisualAge_ - kSealBarrierStart) / kSealBarrierForm)
                                   : (sealState_ == SealState::kBroken
                                          ? 1.0f - SmoothStep01(sealStateTimer_ / 0.75f)
                                          : (sealState_ == SealState::kRecover ? 0.0f : 1.0f));
    const bool barrierVisible = barrierBuild > 0.01f;
    const float crystalPower = static_cast<float>(3 - brokenSeals_) / 3.0f;
    const float barrierAlpha = barrierBuild *
                               (0.54f + 0.20f * crystalPower + 0.04f * std::sin(idleTimer_ * 5.0f));
    const engine::Vector3 ground = FindGroundPoint(GetTranslate());

    // 円筒障壁を生成できていれば、地面からの出現量と残る結晶数を外観へ反映する。
    if (crownShieldBarrier_) {
        crownShieldBarrier_->SetIsVisible(barrierVisible);
        crownShieldBarrier_->SetTranslate(
            ground + engine::Vector3{0.0f, tuning.sealBarrierHeight * barrierBuild * 0.5f, 0.0f});
        crownShieldBarrier_->SetScale({tuning.sealBarrierRadius * barrierBuild,
                                      tuning.sealBarrierHeight * barrierBuild * 0.5f,
                                      tuning.sealBarrierRadius * barrierBuild});
        crownShieldBarrier_->SetColor({0.30f, 0.86f, 1.0f,
                                      0.84f * barrierBuild * (0.70f + 0.30f * crystalPower)});
        // 障壁の描画パラメーターを取得できた場合だけ、結晶の送電を材質の光に渡す。
        if (engine::MeshRenderer* renderer = crownShieldBarrier_->GetMeshRenderer()) {
            if (engine::MeshRenderer::WaterParamForGpu* effect = renderer->GetWaterParamData()) {
                effect->effectIntensity = 0.06f + barrierBuild * 1.72f;
                effect->effectSoftness = crownArrivalGlow * crystalPower;
                effect->uvOffsetY = barrierBuild;
                effect->uvOffsetX = sealState_ == SealState::kReveal ? 1.0f - barrierBuild : 0.0f;
            }
        }

        crownShieldBarrier_->Update(deltaTime);
    }

    // 円筒の障壁と同じ出現率で、本体の殻・地面の輪・詠唱紋章を重ねる。
    UpdateFinaleLayer(crownShieldShell_.get(), deltaTime, barrierVisible,
                      GetTranslate() + engine::Vector3{0.0f, baseScale_.y * 1.2f, 0.0f},
                      {baseScale_.x * 2.65f * barrierBuild, baseScale_.y * 2.65f * barrierBuild,
                       baseScale_.z * 2.65f * barrierBuild},
                      {0.28f, 0.10f, 0.8f, barrierAlpha * 0.38f});
    UpdateFinaleLayer(crownShieldRing_.get(), deltaTime, barrierVisible,
                      ground + engine::Vector3{0.0f, 0.18f, 0.0f},
                      {tuning.sealBarrierRadius * barrierBuild, 1.0f,
                       tuning.sealBarrierRadius * barrierBuild},
                      {0.68f, 0.21f, 1.0f, 0.80f});
    UpdateFinaleLayer(crownCastFloor_.get(), deltaTime, barrierVisible,
                      ground + engine::Vector3{0.0f, 0.12f, 0.0f}, {5.2f, 1.0f, 5.2f},
                      {0.76f, 0.34f, 1.0f, 0.66f});

    // ムービーで先に術式を起動し、制限時間の進行は Chant に入ってから別に数えます。
    const auto* scene = dynamic_cast<GamePlayScene*>(SceneManager::GetInstance()->GetCurrentScene());
    const float revealTime = scene && sealState_ == SealState::kReveal ? scene->GetSealRevealMovieTime() : 0.0f;
    const float groundAge = revealTime - SealRevealTiming::GroundSurge;
    const float groundSpread = SmoothStep01(groundAge / .56f);
    const float groundPulse = groundSpread * (1.0f - SmoothStep01((groundAge - .35f) / .65f));
    const float chantProgress = sealState_ == SealState::kChant
        ? std::clamp(sealStateTimer_ / tuning.sealCastTime, 0.0f, 1.0f)
        : (sealState_ == SealState::kAttack ? 1.0f : 0.0f);
    // 床の紋章がある場合は、専用の描き上がりに詠唱の進捗を渡す。
    if (crownCastFloor_)
        crownCastFloor_->SetRevealProgress(chantProgress);
    // 残り3秒から床の電流を一段強め、術の発動が近いことを知らせます。
    const float remaining = tuning.sealCastTime - sealStateTimer_;
    const float warningVisual = SmoothStep01((kSealSurgeRemaining - remaining) / .28f);
    const float groundCharge = sealState_ == SealState::kChant
        ? 0.12f + 0.56f * chantProgress * chantProgress + .24f * warningVisual
        : (sealState_ == SealState::kAttack ? 0.70f + 0.30f * SmoothStep01(sealStateTimer_ / 2.45f)
                                          : 0.12f * groundSpread + 0.52f * groundPulse);
    // 最初の帯電だけ王の足元から外周へ広げ、復帰後は闘技場全体に残します。
    const float chargedRadius = tuning.sealBarrageRadius *
        (revealTime > 0.0f ? std::lerp(.15f, 1.0f, groundSpread) : 1.0f);
    const float aftermath = sealState_ == SealState::kAttack
        ? 1.0f - SmoothStep01((sealStateTimer_ - tuning.sealBarrageMovieDuration + 0.45f) / 0.45f) : 1.0f;
    const float dischargeAge = sealStateTimer_ - 2.76f;
    const float discharge = sealState_ == SealState::kAttack && dischargeAge >= 0.0f
        ? 1.0f - SmoothStep01((sealStateTimer_ - tuning.sealBarrageMovieDuration + 0.38f) / 0.38f) : 0.0f;
    const engine::Vector3 strike = !sealBarrage_.empty() ? sealBarrage_.front().ground : ground;
    UpdateFinaleLayer(crownArenaRing_.get(), deltaTime, groundCharge > 0.01f,
        ground + engine::Vector3{0.0f, 0.18f, 0.0f},
        {chargedRadius, 1.0f, chargedRadius},
        {1.0f, 1.0f, 1.0f, groundCharge * 0.80f * aftermath});

    // UVを移動させず、地表に固定した電流の分岐・明滅・走行で圧力を増します。
    UpdateFinaleLayer(crownBarrageLayers_[0].get(), deltaTime, groundCharge > 0.01f,
        ground + engine::Vector3{0.0f, 0.10f, 0.0f},
        {chargedRadius, 1.0f, chargedRadius},
        {1.0f, 0.09f, 0.025f, (0.06f + groundCharge * 0.28f) * aftermath});
    const engine::Vector3 arenaAura = ground + engine::Vector3{0.0f, 4.2f, 0.0f};
    UpdateFinaleLayer(crownBarrageLayers_[1].get(), deltaTime, groundCharge > 0.01f,
        arenaAura, {tuning.sealBarrageRadius, 1.0f, 10.0f + groundCharge * 7.0f},
        {1.0f, 0.025f, 0.008f, (groundCharge * 0.20f + discharge * 0.24f) * aftermath},
        FinaleBillboardRotation(arenaAura));
    for (std::int32_t index = 2; index < 4; ++index) {
        // 主槍が刺さった瞬間、接地点だけでなく闘技場の全域を同時に放電させます。
        const float radius = tuning.sealBarrageRadius;
        UpdateFinaleLayer(crownBarrageLayers_[index].get(), deltaTime, discharge > 0.01f,
            ground + engine::Vector3{0.0f, 0.30f + index * 0.03f, 0.0f}, {radius, 1.0f, radius},
            {1.0f, 1.0f, 1.0f, discharge * 0.96f}, {0.0f, index * 1.7f, 0.0f});
    }

    const engine::Vector3 impactAura = strike + engine::Vector3{0.0f, 5.5f, 0.0f};
    UpdateFinaleLayer(crownBarrageLayers_[4].get(), deltaTime, discharge > 0.01f,
        impactAura, {13.0f, 1.0f, 14.0f},
        {1.0f, 0.035f, 0.008f, discharge * 0.52f}, FinaleBillboardRotation(impactAura));
    // 王冠に集まった光を上へ伸ばし、空の術式が王の詠唱で始まると示します。
    const float beamAge = revealTime - SealRevealTiming::Launch;
    const float beamRise = SmoothStep01(beamAge / .58f);
    const float beamAlpha = beamRise * (1.0f - SmoothStep01((beamAge - .58f) / .52f));
    const float beamBottom = GetTranslate().y + baseScale_.y * 2.0f;
    const float beamHeight = std::max(0.0f, ground.y + tuning.sealChantSkyHeight - beamBottom) * beamRise;
    UpdateFinaleLayer(crownBarrageLayers_[5].get(), deltaTime, beamAlpha > .01f,
        {ground.x, beamBottom + beamHeight * .5f, ground.z},
        {.72f, beamHeight / 10.0f, .72f}, {1.0f, .34f, .11f, beamAlpha});
    UpdateFinaleLayer(crownBarrageFlash_.get(), deltaTime, discharge > 0.01f,
        ground + engine::Vector3{0.0f, 0.24f, 0.0f},
        {tuning.sealBarrageRadius * 0.98f, 1.1f, tuning.sealBarrageRadius * 0.98f},
        {1.0f, 0.07f, 0.02f, discharge * 0.28f});

    // 上を向いた時点で全体が読めるよう、紋章は描き進めず明るさだけ立ち上げます。
    const float skyAlpha = sealState_ == SealState::kChant || sealState_ == SealState::kAttack
        ? 1.0f : SmoothStep01((revealTime - SealRevealTiming::SkyAppear) / .20f);
    const float skyPulseAge = revealTime - SealRevealTiming::SkyPulse;
    const float skyFlare = sealState_ == SealState::kAttack
        ? (1.0f - SmoothStep01((sealStateTimer_ - .12f) / .39f))
        : SmoothStep01(skyPulseAge / .11f) * (1.0f - SmoothStep01((skyPulseAge - .08f) / .42f));
    const float skyFade = sealState_ == SealState::kAttack
        ? 1.0f - SmoothStep01((sealStateTimer_ - 2.55f) / .65f) : 1.0f;
    const bool skyVisible = skyAlpha > .001f && skyFade > .01f;
    const float skyRadius = tuning.sealBarrageRadius * tuning.sealBarrageSkyRadiusRate;
    const engine::Vector3 skyCenter = ground + engine::Vector3{0.0f, tuning.sealChantSkyHeight, 0.0f};
    const float skyLight = skyAlpha * skyFade * (0.88f + 0.12f * std::sin(idleTimer_ * 6.0f));
    UpdateFinaleLayer(crownBarrageSkySigil_.get(), deltaTime, skyVisible, skyCenter,
                      {skyRadius / 1.2f, 1.0f, skyRadius / 1.2f},
                      {1.0f, .29f + .48f * skyFlare, .09f + .43f * skyFlare, (.63f + .52f * skyFlare) * skyLight},
                      {kPi, idleTimer_ * 0.18f, 0.0f});
    // 両方の天空紋章は、生成済みであれば描き上がりを完了状態に固定する。
    if (crownBarrageSkySigil_)
        crownBarrageSkySigil_->SetRevealProgress(1.0f);
    UpdateFinaleLayer(crownBarrageSkyCounterSigil_.get(), deltaTime, skyVisible,
                      skyCenter + engine::Vector3{0.0f, -0.10f, 0.0f},
                      {skyRadius * 0.40f, 1.0f, skyRadius * 0.40f},
                      {1.0f, 0.32f, 0.12f, .56f * skyLight},
                      {kPi, -idleTimer_ * 0.33f, 0.0f});
    if (crownBarrageSkyCounterSigil_)
        crownBarrageSkyCounterSigil_->SetRevealProgress(1.0f);
    UpdateFinaleLayer(crownBarrageSkyOuterRing_.get(), deltaTime, skyVisible,
                      skyCenter + engine::Vector3{0.0f, 0.06f, 0.0f},
                      {skyRadius, 1.0f, skyRadius},
                      {1.0f, .19f + .57f * skyFlare, .11f + .34f * skyFlare, (.55f + .42f * skyFlare) * skyLight});
    UpdateFinaleLayer(crownBarrageSkyInnerRing_.get(), deltaTime, skyVisible,
                      skyCenter + engine::Vector3{0.0f, -0.18f, 0.0f},
                      {skyRadius * 0.66f, 1.0f, skyRadius * 0.66f},
                      {0.54f, 0.80f, 1.0f, .46f * skyLight});

    // 全破壊後は余韻を1.2秒保ち、ゲームプレイシーンへ王冠継承を要求します。
    if (sealState_ == SealState::kBroken) {
        attackState_ = AttackState::kRecovery;

        // 全破壊から1.2秒以上たち、現在のシーンがゲーム本編であれば継承を予約する。
        if (sealStateTimer_ >= 1.2f) {
            auto* scene = dynamic_cast<GamePlayScene*>(SceneManager::GetInstance()->GetCurrentScene());

            if (scene)
                scene->RequestCrownFinale(this);
        }

        return;
    }

    // 結晶・障壁が出現する間は予備動作を維持し、このフレームの後続段階を進めない。
    if (sealState_ == SealState::kReveal) {
        attackState_ = AttackState::kWindup;

        // 障壁が完成してもカット中は時計を開始しない。Playerに操作を返してから詠唱へ進む。
        if (sealVisualAge_ >= kSealBarrierStart + kSealBarrierForm && (!scene || !scene->IsSealRevealMovieActive())) {
            sealState_ = SealState::kChant;
            sealStateTimer_ = 0.0f;
        }

        return;
    }

    // 詠唱中は攻撃タイマーを制限時間に合わせ、結晶を破壊できる時間をプレイヤーに残します。
    if (sealState_ == SealState::kChant) {
        attackState_ = AttackState::kWindup;
        currentAttack_ = AttackKind::kNightfallLances;
        attackStateDuration_ = tuning.sealCastTime;
        attackTimer_ = std::max(0.0f, remaining);

        // 粒子の発生間隔を過ぎたら、王の火花、周囲の火花、地表の電流を同じフレームで出す。
        if (sealVfxTimer_ >= 0.18f) {
            sealVfxTimer_ = 0.0f;
            EmitDirectedPreset("enemy/false_king/false_king_seal_embers",
                               GetTranslate() + engine::Vector3{0.0f, baseScale_.y * 1.5f, 0.0f},
                               {0.0f, 1.0f, 0.0f}, 1.2f + sealStateTimer_ / tuning.sealCastTime);

            // 空間に散らした赤い火花も詠唱後半ほど密になります。
            const std::int32_t emberSites = 2 + static_cast<std::int32_t>(chantProgress * 7.0f);

            for (std::int32_t index = 0; index < emberSites; ++index) {
                const float angle = index * 2.4f + idleTimer_ * 0.47f;
                const float radius = tuning.sealBarrageRadius * (0.18f + 0.09f * index);
                EmitDirectedPreset("enemy/false_king/false_king_seal_embers",
                    ground + engine::Vector3{radius * std::cos(angle), 0.24f, radius * std::sin(angle)},
                    {0.0f, 1.0f, 0.0f}, 0.6f + chantProgress * 1.4f);
            }

            // 地表の電流は火花とは別に円内へ分散し、詠唱の進行に合わせて密度と速さを上げる。
            const std::int32_t currentSites = 5 + static_cast<std::int32_t>(chantProgress * 14.0f);

            for (std::int32_t index = 0; index < currentSites; ++index) {
                const float angle = index * 2.39996f + std::floor(idleTimer_ * 5.0f) * 0.23f;
                const float radius = tuning.sealBarrageRadius * std::sqrt((index + 0.5f) / currentSites);
                EmitDirectedPreset("enemy/false_king/false_king_seal_ground_sparks",
                    ground + engine::Vector3{radius * std::cos(angle), 0.28f, radius * std::sin(angle)},
                    {std::cos(angle + 1.5f), 0.025f, std::sin(angle + 1.5f)}, 6.0f + chantProgress * 8.0f);
            }

        }

        // 制限時間が残る間は結晶を壊せる詠唱段階に留まる。
        if (sealStateTimer_ < tuning.sealCastTime)
            return;

        // 失敗が確定してから操作を止める。詠唱中の結晶破壊は最後まで可能です。
        auto* gameplay = dynamic_cast<GamePlayScene*>(SceneManager::GetInstance()->GetCurrentScene());
        const engine::Vector3 heroGround = GetTarget() ? FindGroundPoint(GetTarget()->GetWorldPosition()) : ground;
        // 本編のシーンに限って、対象の地表を基準に操作制限を伴う槍雨ムービーを始める。
        if (gameplay && debugPreviewAttackId_.empty())
            gameplay->StartSealBarrageMovie(this, heroGround);

        // 主役の大槍をプレイヤーの位置へ置き、小槍は広域攻撃の広がりだけを担います。
        sealState_ = SealState::kAttack;
        sealStateTimer_ = 0.0f;
        sealBarrageDamageApplied_ = false;
        sealBarrage_.clear();
        const auto makeEffect = [this](const char* path) {
            auto effect = std::make_unique<EffectObject3D>();
            effect->Initialize(GetObjectCommon());
            // 演出データを読めない層は空として返し、槍の進行は続ける。
            if (!effect->LoadFromJson(path))
                return std::unique_ptr<EffectObject3D>{};
            effect->SetIsVisible(false);
            effect->Play(8.0f);
            return effect;
        };

        // 主槍の着弾点を中心に固定し、残りは闘技場の内周と外周へ時差をつけて配置する。
        for (std::int32_t ring = 0; ring < 3; ++ring) {
            std::int32_t count = 1;
            float radius = 0.0f;
            if (ring == 1) {
                count = 6;
                radius = tuning.sealBarrageRadius * 0.42f;
            } else if (ring == 2) {
                count = 10;
                radius = tuning.sealBarrageRadius * 0.83f;
            }

            for (std::int32_t index = 0; index < count; ++index) {
                const float angle = count == 1 ? 0.0f : (static_cast<float>(index) + 0.5f * ring) * kTwoPi / count;
                SealBarrageSpear spear;
                spear.hero = ring == 0;
                spear.ground = spear.hero ? heroGround : FindGroundPoint(ground + engine::Vector3{
                    radius * std::cos(angle), 0.0f, radius * std::sin(angle)});
                spear.delay = spear.hero ? 2.13f : (ring == 1 ? 1.30f + index * 0.085f
                                                               : 1.47f + index * 0.050f);
                spear.size = tuning.sealBarrageSpearScale * (spear.hero ? 1.68f : (ring == 1 ? 0.86f : 0.64f));
                spear.model = CreateRoyalMesh("FalseKing_ShadowSpear_" + std::to_string(sealBarrage_.size()),
                                              kCrownBarrageSpearModel, {1.0f, 1.0f, 1.0f, 1.0f}, 0.45f);
                // 生成直後の槍は画面に出さず、表示後も影を落とさない。
                if (spear.model) {
                    spear.model->SetCastShadow(false);
                    spear.model->SetIsVisible(false);
                }

                spear.glow = makeEffect("Resources/json/effect/enemy/false_king/effect_false_king_seal_red_aura.json");
                spear.portal = makeEffect(kNightfallLancesRingEffect);
                spear.impactLightning = makeEffect("Resources/json/effect/enemy/false_king/effect_false_king_seal_discharge_burst.json");
                spear.groundArc = makeEffect("Resources/json/effect/enemy/false_king/effect_false_king_seal_ground_current.json");
                spear.impactArc = makeEffect("Resources/json/effect/enemy/false_king/effect_false_king_seal_ground_current.json");
                // 縦の着弾光は主槍だけに付け、どの槍も時差・位置と一緒に更新一覧へ渡す。
                if (spear.hero)
                    spear.impactBeam = makeEffect(kNightfallImpactBeamEffect);
                sealBarrage_.push_back(std::move(spear));
            }
        }
    }

    // 詠唱失敗後は、槍雨ムービーの時間を基準に各槍の落下と消散を進めます。
    if (sealState_ == SealState::kAttack) {
        attackState_ = AttackState::kActive;
        attackStateDuration_ = tuning.sealBarrageMovieDuration;
        attackTimer_ = std::max(0.0f, attackStateDuration_ - sealStateTimer_);
        const bool emitAirAura = sealVfxTimer_ >= 0.12f;
        // 槍雨中は0.12秒以上経過するたびに粒子の発生時計を戻す。
        if (emitAirAura)
            sealVfxTimer_ = 0.0f;
        const float dissolveStart = tuning.sealBarrageMovieDuration - 0.80f;

        // 粒子を出す時刻に限り地表を走る火花を更新し、全域放電中は空中にもオーラを立てる。
        if (emitAirAura) {
            const std::int32_t sites = discharge > 0.0f ? 36 : 20;

            for (std::int32_t index = 0; index < sites; ++index) {
                const float angle = index * 2.39996f + std::floor(idleTimer_ * 7.0f) * 0.31f;
                const float radius = tuning.sealBarrageRadius * std::sqrt((index + 0.5f) / sites);
                const engine::Vector3 site = ground + engine::Vector3{radius * std::cos(angle), 0.3f, radius * std::sin(angle)};
                EmitDirectedPreset("enemy/false_king/false_king_seal_ground_sparks", site,
                    {std::cos(angle), 0.03f, std::sin(angle)}, discharge > 0.0f ? 18.0f : 10.0f);
                if (discharge > 0.0f)
                    EmitDirectedPreset("enemy/false_king/false_king_seal_air_aura", site + engine::Vector3{0.0f, 1.6f, 0.0f},
                        {0.0f, 1.0f, 0.0f}, 4.0f);
            }
        }

        // 各槍の開始時刻を引いて落下の進行を求め、消散には全体で共通の時刻を使います。
        for (auto& spear : sealBarrage_) {
            const float age = sealStateTimer_ - spear.delay;
            const float fall = std::pow(std::clamp(age / 0.63f, 0.0f, 1.0f), 1.55f);
            const float reveal = SmoothStep01(sealStateTimer_ / (spear.hero ? 0.54f : 0.77f));
            const float dissolve = SmoothStep01((sealStateTimer_ - dissolveStart) / 0.70f);
            const float fade = 1.0f - dissolve;
            const bool visible = reveal > 0.01f && fade > 0.01f;
            const float spearScale = spear.size * std::max(0.05f, reveal);
            const float centerHeight = 4.90f * spearScale + tuning.sealBarrageSkyHeight * (1.0f - fall) - fall * 0.55f;
            const engine::Vector3 center = spear.ground + engine::Vector3{0.0f, centerHeight, 0.0f};
            const float spin = idleTimer_ * (spear.hero ? 0.40f : 0.78f) + spear.delay * 3.4f;

            // 槍のモデルがあれば落下位置と光を反映し、消散中は上から切り取る。
            if (spear.model) {
                spear.model->SetIsVisible(visible);
                spear.model->SetTranslate(center);
                spear.model->SetRotation({0.0f, spin, 0.0f});
                spear.model->SetScale({spearScale, spearScale, spearScale});
                spear.model->SetEmissive((spear.hero ? 0.65f : 0.42f) + 0.35f * (1.0f - fall));

                // 槍を上端からノイズ付きの赤い縁で崩し、破片と火花に引き継ぎます。
                {
                    engine::Object3D::PortalClipSettings clipSettings;
                    spear.model->SetMaterialTime(idleTimer_);
                    clipSettings.enabled = dissolve > 0.0f ? true : false;
                    clipSettings.progress = 0.5f;
                    clipSettings.center = spear.ground + engine::Vector3{0.0f,
                        (centerHeight + 4.3f * spearScale + 0.8f) * (1.0f - dissolve) - dissolve, 0.0f};
                    clipSettings.normal = {0.0f, 1.0f, 0.0f};
                    clipSettings.edgeWidth = 0.18f * spear.size;
                    clipSettings.dissolve = 0.7f;
                    clipSettings.color = {4.0f, 0.20f, 0.025f, 1.0f};
                    spear.model->SetPortalClip(clipSettings);
                }

                spear.model->UpdateLocalMatrix();
                spear.model->UpdateWorldMatrix(true);
            }

            // 着地を条件にせず、空中の形成段階から槍全体を赤いオーラで包みます。
            UpdateFinaleLayer(spear.glow.get(), deltaTime, visible, center,
                              {2.7f * spearScale, 1.0f, 6.4f * spearScale},
                              {1.0f, 0.018f, 0.008f, reveal * fade * (0.36f + 0.16f * fall)},
                              FinaleBillboardRotation(center));
            // 粒子の発生時刻で、見えている槍にだけ空中オーラと火花を足す。
            if (emitAirAura && visible) {
                EmitDirectedPreset("enemy/false_king/false_king_seal_air_aura", center,
                    {0.0f, 1.0f, 0.0f}, 1.0f);
                EmitPreset("enemy/false_king/false_king_seal_embers", center);
            }

            // 落下に合わせて空中の射出口を消し、地面には後続の放電を予告する電流を残します。
            const float portalFade = reveal * (1.0f - SmoothStep01(age / 0.45f));
            UpdateFinaleLayer(spear.portal.get(), deltaTime, portalFade > 0.01f,
                              spear.ground + engine::Vector3{0.0f, tuning.sealBarrageSkyHeight - 0.08f, 0.0f},
                              {spear.hero ? 4.1f : 2.4f, 1.0f, spear.hero ? 4.1f : 2.4f},
                              {1.0f, 0.20f, 0.40f, portalFade * 0.82f});

            // 予測線は使わず、地表を這う電流が時刻とともに広がります。
            const float charge = SmoothStep01(sealStateTimer_ / 2.35f);
            const float chargeRadius = (spear.hero ? 5.4f : 2.8f) * (0.96f + charge * 0.04f);
            const float chargeFlicker = 0.82f + 0.18f * std::sin(idleTimer_ * 17.0f + spear.delay * 7.0f);
            UpdateFinaleLayer(spear.groundArc.get(), deltaTime, charge > 0.01f,
                              spear.ground + engine::Vector3{0.0f, 0.20f, 0.0f},
                              {chargeRadius, 1.0f, chargeRadius},
                              {1.0f, 1.0f, 1.0f,
                               (0.18f + 0.46f * charge) * chargeFlicker * fade},
                              {0.0f, spear.delay * 3.4f, 0.0f});
            // 着地からの経過時間で稲妻と地面の輪を弱め、縦の光柱は主槍だけに出します。
            const float strikeAge = age - 0.63f;
            const float lightningFade = std::max(discharge * 0.78f,
                strikeAge >= 0.0f ? 1.0f - SmoothStep01(strikeAge / 0.90f) : 0.0f);
            const engine::Vector3 boltCenter = spear.ground + engine::Vector3{0.0f, spear.hero ? 4.0f : 2.8f, 0.0f};
            UpdateFinaleLayer(spear.impactLightning.get(), deltaTime, lightningFade > 0.01f,
                              boltCenter, {spear.hero ? 9.0f : 7.0f, 1.0f, spear.hero ? 7.0f : 5.0f},
                              {1.0f, 1.0f, 1.0f, lightningFade * 0.90f}, FinaleBillboardRotation(boltCenter));
            const float arcFade = strikeAge >= 0.0f ? 1.0f - SmoothStep01(strikeAge / 0.52f) : 0.0f;
            const float arcRadius = (2.2f + 5.2f * SmoothStep01(strikeAge / 0.36f)) *
                                    (spear.hero ? 1.30f : 0.78f);
            UpdateFinaleLayer(spear.impactArc.get(), deltaTime, arcFade > 0.01f,
                              spear.ground + engine::Vector3{0.0f, 0.28f, 0.0f},
                              {arcRadius, 1.0f, arcRadius},
                              {1.0f, 1.0f, 1.0f, arcFade * 0.82f}, {0.0f, spin, 0.0f});
            const float beamFade = spear.hero && strikeAge >= 0.0f
                                       ? 1.0f - SmoothStep01(strikeAge / 0.85f)
                                       : 0.0f;
            UpdateFinaleLayer(spear.impactBeam.get(), deltaTime, beamFade > 0.01f,
                              spear.ground + engine::Vector3{0.0f, 8.2f, 0.0f},
                              {2.8f, 1.8f, 2.8f}, {1.0f, 0.84f, 0.90f, beamFade});
            // 落下開始と着地で時刻をまたいだフレームだけ、それぞれの粒子を発生させます。
            if (age >= 0.0f && age - deltaTime < 0.0f)
                EmitDirectedPreset("enemy/false_king/false_king_seal_embers",
                                   spear.ground + engine::Vector3{0.0f, tuning.sealBarrageSkyHeight, 0.0f},
                                   {0.0f, -1.0f, 0.0f}, spear.hero ? 4.1f : 1.6f);
            if (strikeAge >= 0.0f && strikeAge - deltaTime < 0.0f) {
                EmitDirectedPreset("enemy/false_king/false_king_seal_impact", spear.ground,
                                   {0.0f, 1.0f, 0.0f}, spear.hero ? 4.5f : 1.7f);
                EmitPreset("enemy/false_king/false_king_seal_embers", spear.ground + engine::Vector3{0.0f, 0.24f, 0.0f});
            }
        }

        // 槍の消散が始まる瞬間に、全ての接地点で描画から破片と火花へ引き継ぎます。
        if (sealStateTimer_ >= dissolveStart && sealStateTimer_ - deltaTime < dissolveStart) {
            for (const auto& spear : sealBarrage_) {
                const engine::Vector3 breakup = spear.ground + engine::Vector3{0.0f, 3.4f * spear.size, 0.0f};
                DebrisEffectManager::GetInstance()->SpawnOnGround("shared/false_king_seal_shatter", breakup, spear.ground.y);
                EmitPreset("enemy/false_king/false_king_seal_dissolve", breakup);
            }
        }

        // 2.76秒の放電を一度だけ確定し、対象がいる場合は全域の致死ダメージを送ります。
        if (!sealBarrageDamageApplied_ && sealStateTimer_ >= 2.76f) {
            sealBarrageDamageApplied_ = true;

            // 標的が存在すれば、回避無敵を無視する致死ダメージをイベントで送る。
            if (GetTarget()) {
                DamageEvent event;
                event.target = GetTarget();
                event.attacker = this;
                event.damageAmount = tuning.sealBarrageDamage;
                event.lethalPlayerDamage = true;
                event.damageType = DamageType::kElectric;
                event.knockbackVelocity = {0.0f, 5.0f, 0.0f};
                event.bypassPlayerInvincibility = true;
                EventManager::GetInstance()->Dispatch(event);
            }

            // 主槍の着弾を合図に、全ての接地点から同時に放電粒子を噴き上げます。
            for (const auto& spear : sealBarrage_)
                EmitDirectedPreset("enemy/false_king/false_king_seal_impact", spear.ground + engine::Vector3{0.0f, 0.2f, 0.0f},
                    {0.0f, 1.0f, 0.0f}, spear.hero ? 2.0f : 1.0f);
            // 主カメラが利用できる場合は、全域放電に合わせて揺らす。
            if (engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera())
                camera->StartShake(0.65f, 0.22f, 23.0f, {0.7f, 0.7f, 0.35f});
        }

        // ムービー尺を使い切ったら槍の描画物を消し、次の結晶を作るまでの待機に入ります。
        if (sealStateTimer_ >= tuning.sealBarrageMovieDuration) {
            sealState_ = SealState::kRecover;
            sealStateTimer_ = 0.0f;
            sealBarrage_.clear();
        }

        return;
    }

    attackState_ = AttackState::kRecovery;
    // 失敗後の待ち時間を満たしたら、結晶・詠唱・予告を初期化して挑戦を再開します。
    if (sealState_ == SealState::kRecover && sealStateTimer_ >= tuning.sealBarrageRecovery)
        InitializeCrownSeals();
}

void EnemyFalseKingSlime::OnCrownFinaleStarted() {
    // シーンが継承演出を開始した時点で幕間扱いにし、残っている攻撃表示を片付けます。
    crownInterlude_ = true;
    ClearTransientVisuals();
}

void EnemyFalseKingSlime::BeginFinalArena(const engine::Vector3& position) {
    // 王冠結晶戦から最終決戦へ移り、戦闘位置と残った演出を切り替えます。
    // 次の闘技場へ持ち越す前に、結晶の衝突登録を含む第二形態の所有物を破棄します。
    ResetCrownFinale();
    crownInterlude_ = false;
    finalArenaStarted_ = true;
    battlePhase_ = pendingBattlePhase_ = 3;
    attackState_ = AttackState::kIdle;
    currentAttack_ = AttackKind::kNone;
    attackCooldown_ = 2.5f;
    ClearTransientVisuals();
    // 新しい闘技場の位置を反映し、付近で攻撃を受けられるよう配置時の衝突を戻します。
    SetTranslate(position);
    SetScale(baseScale_);
    SetVelocity({});
    SetGrounded(true);
    SetCollisionAttribute(authoredCollisionAttribute_);
    SetCollisionMask(authoredCollisionMask_);
    // パラメーターが利用できる場合は最終形態のHPを最大値の34%へ変更し、色と判定も確定する。
    if (GetParameters())
        SetParameter(&engine::EntityParameter::hp, GetEncounterMaximumHp() * 0.34f);
    SetColor({0.78f, 0.65f, 1.0f, 1.0f});
    SyncCollisionBounds();
    UpdateWorldMatrix(true);
}

void EnemyFalseKingSlime::AbortCrownFinale() {
    // 継承を中断したら第二形態へ戻し、結晶を再生成できる状態にする。
    crownInterlude_ = false;
    finalArenaStarted_ = false;
    battlePhase_ = pendingBattlePhase_ = 2;
    attackState_ = AttackState::kIdle;
    // 封印戦の基準地点へ戻し、第二形態のHPに戻して三つの結晶を再生成します。
    SetTranslate(FindGroundPoint(authoredPosition_ +
                                 engine::Vector3{0.0f, 0.0f, CrownFinaleTuning::Get().sealCenterOffsetZ}));
    SetScale(baseScale_);
    // HPを保持するパラメーターがあれば、第二形態の開始値に戻す。
    if (GetParameters())
        SetParameter(&engine::EntityParameter::hp, GetEncounterMaximumHp() * 0.67f);
    InitializeCrownSeals();
}

void EnemyFalseKingSlime::DrawFinaleEditor() {
    // 形態と詠唱を確認し、槍雨や最終戦の調整を保存・再読込するImGuiの入口。
#ifdef USE_IMGUI
    if (!ImGui::CollapsingHeader("王冠最終戦 / 第2形態・脱出", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    auto& t = CrownFinaleTuning::Get();
    ImGui::Text("形態 %d / 封印 %d / 3", battlePhase_, brokenSeals_);
    ImGui::TextWrapped("中央で詠唱する王を守る3個の結晶を制限時間内に直接破壊します。失敗すると全域へ黒晶墜槍が降ります。");
    // 第二形態のときだけ、現在の詠唱の残り秒数を表示する。
    if (battlePhase_ == 2)
        ImGui::Text("詠唱残り %.1f 秒", GetSealCastRemainingTime());
    // 封印戦で使う配置・HP・制限時間と障壁の寸法を調整する。
    ImGui::SliderFloat("王の中央位置 Z オフセット", &t.sealCenterOffsetZ, 5.0f, 24.0f);
    ImGui::SliderFloat("封印の配置半径", &t.sealRadius, 8.0f, 22.0f);
    ImGui::SliderFloat("封印の命中半径", &t.sealHitRadius, 1.5f, 5.0f);
    ImGui::SliderFloat("結晶のHP", &t.sealCrystalHp, 5.0f, 200.0f);
    ImGui::SliderFloat("詠唱制限時間", &t.sealCastTime, 8.0f, 40.0f);
    ImGui::SliderFloat("バリア半径", &t.sealBarrierRadius, 3.0f, 12.0f);
    ImGui::SliderFloat("バリア高さ", &t.sealBarrierHeight, 5.0f, 20.0f);
    // 詠唱失敗時の槍雨は、規模・カメラ・再詠唱までの待ち時間を個別に調整する。
    ImGui::SliderFloat("全域槍の演出半径", &t.sealBarrageRadius, 25.0f, 44.0f);
    ImGui::SliderFloat("詠唱の大魔法陣の高さ", &t.sealChantSkyHeight, 24.0f, 42.0f, "%.1f m");
    ImGui::SliderFloat("槍の出現高度", &t.sealBarrageSkyHeight, 8.0f, 18.0f, "%.1f m");
    ImGui::SliderFloat("槍雨の空中魔法陣の広さ", &t.sealBarrageSkyRadiusRate, 0.45f, 0.95f);
    ImGui::SliderFloat("槍雨の槍の大きさ", &t.sealBarrageSpearScale, 0.65f, 1.45f);
    ImGui::SliderFloat("槍雨ムービーの長さ", &t.sealBarrageMovieDuration, 3.1f, 5.0f);
    ImGui::SliderFloat("槍雨カメラの距離", &t.sealBarrageCameraDistance, 0.7f, 1.5f);
    ImGui::SliderFloat("槍雨カメラの高さ", &t.sealBarrageCameraHeight, 0.7f, 1.5f);
    ImGui::TextUnformatted("全域放電：HPと回避無敵に関係なく即死");
    ImGui::SliderFloat("失敗後の再詠唱待ち", &t.sealBarrageRecovery, 1.0f, 8.0f);
    ImGui::TextWrapped("プレイヤーの飛行・着地、王冠片の移動・大きさ、継承のカメラ・時刻は、演出タイムラインの crown_finale で編集します。");
    // 継承を終えた後の闘技場と、最終形態の攻撃・装甲の調整を扱う。
    ImGui::SliderFloat("欠片とプレイヤーの距離", &t.movieFragmentDistance, 1.5f, 5.5f, "%.2f m");
    ImGui::SliderFloat("最終闘技場 X", &t.destinationX, 130.0f, 240.0f);
    ImGui::SliderFloat("王冠槍ダメージ", &t.lanceDamage, 1.0f, 40.0f);
    ImGui::SliderFloat("王威波ダメージ", &t.waveDamage, 1.0f, 60.0f);
    ImGui::SliderFloat("王翼突進ダメージ", &t.dashDamage, 1.0f, 60.0f);
    ImGui::SliderFloat("装甲が開く秒数", &t.armorOpenTime, 2.0f, 12.0f);
    // スライダー入力を有効範囲へ整えてから、保存または設定の再読込を受け付けます。
    t.Sanitize();
    static std::int32_t sSaveResult = 0;
    if (ImGui::Button("最終戦設定を保存"))
        sSaveResult = t.Save() ? 1 : -1;
    ImGui::SameLine();
    if (ImGui::Button("設定を再読込"))
        sSaveResult = t.Load() ? 1 : -1;
    // 保存か再読込を試した後は、その成否を表示する。
    if (sSaveResult)
        ImGui::TextUnformatted(sSaveResult > 0 ? "設定を反映しました" : "設定ファイルを開けませんでした");
    // 再生中で幕間・最終闘技場・技プレビューのどれにも入っていない場合だけ、進行の確認操作を出す。
    if (SceneManager::GetInstance()->IsPlaying() && !crownInterlude_ && !finalArenaStarted_ &&
        debugPreviewAttackId_.empty()) {
        if (ImGui::Button("第2形態から確認"))
            TriggerDebugPhaseTwoTransition();
        ImGui::SameLine();
        if (ImGui::Button("王冠継承・脱出を確認"))
            TriggerDebugCrownInterlude();
    }
#endif
}
