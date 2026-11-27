#include <cstdint>
#include "game/actor/enemy/boss/false_king/EnemyFalseKingSlime.Internal.h"

#include "game/actor/combat/CombatGroundQuery.h"
#include "game/actor/enemy/boss/core/BossChantVisual.h"
#include "game/crown/CrownBarrageCamera.h"
#include <algorithm>

using namespace enemyFalseKingSlimeImplementation;

namespace {
BlendMode GetOwnedVisualBlendMode(engine::Object3D* visual) {
    // EffectObject3dは合成方式を基底とは別に保持するため、実際の描画設定で振り分ける。
    if (auto* effect = dynamic_cast<EffectObject3D*>(visual)) {
        return effect->GetBlendMode();
    }
    return visual->GetBlendMode();
}
} // namespace

// 偽王の戦闘フェーズと攻撃状態を進めます。登場・幕間・撃破中は通常AIへ流さず専用演出を優先します。

EnemyFalseKingSlime::EnemyFalseKingSlime() = default;
EnemyFalseKingSlime::~EnemyFalseKingSlime() {
    // 個別登録した結晶の衝突を所有物の破棄前に解除します。
    ResetCrownFinale();
}

void EnemyFalseKingSlime::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    // 本体の反射はモデル材質を尊重し、攻撃用の衝突を敵属性で登録します。
    ClearBossAura();
    BaseEnemy::Initialize(common, modelName);
    SetName("Enemy_FalseKingSlime");
    SetEnemyType("FalseKingSlime");
    ReloadAttackProfile();

    SetMaterialType(25);

    // 金属値はOBJの各マテリアルへ持たせ、身体と王冠を同じ反射にしません。
    SetMetallic(0.0f);
    SetRoughness(0.24f);
    SetEnableEnvMap(true);
    SetEnvIntensity(0.68f);
    SetEmissive(1.0f);
    SetCollisionAttribute(engine::collisionCategories::Get("enemy"));
    SetCollisionMask(engine::collisionCategories::Get("player") | engine::collisionCategories::Get("ground") | engine::collisionCategories::Get("player_attack") | engine::collisionCategories::Get("player_bullet"));
    SetColliderType(engine::ColliderType::kObb);
    SetDefaultColor(GetColor());
    SyncCollisionBounds();
}

void EnemyFalseKingSlime::Update(float deltaTime) {
    deltaTime = (std::max)(0.0f, deltaTime);
    UpdateState(deltaTime);
    // 早期returnを含む全状態の移動後に更新し、オーラが一フレーム遅れないようにします。
    UpdateBossAura(deltaTime);
}

void EnemyFalseKingSlime::UpdateState(float deltaTime) {
    // 登場・幕間・最終撃破中は通常 AI より専用の進行を優先します。
    deltaTime = (std::max)(0.0f, deltaTime);

    // プレイ中の最終決着は姿勢を維持する。編集へ戻ったら専用状態を解除して配置の復元へ進む。
    if (finalDefeatHeld_ && SceneManager::GetInstance()->IsPlaying())
        return;
    if (finalDefeatHeld_)
        CancelFinalDefeat();
    if (UpdateEncounterState(deltaTime)) {
        // 登場演出中はCharacterの重力を止めつつ、変更したTransformだけ描画へ反映します。
        engine::Object3D::Update(deltaTime);
        return;
    }

    // 外部の入場タイムラインが姿勢を担当している間は、速度を消して描画用の更新だけを通す。
    if (introPresentationActive_) {
        SetVelocity({});
        engine::Object3D::Update(deltaTime);
        return;
    }

    // 最終局面のHP消滅は通常の死亡処理へ渡す前に、専用決着演出へ引き継ぎます。
    if (finalArenaStarted_ && battlePhase_ == 3 && GetParameters() && GetParameters()->hp <= 0 && !IsDead()) {
        auto* scene=dynamic_cast<GamePlayScene*>(SceneManager::GetInstance()->GetCurrentScene());
        // 現在のシーンが決着演出を引き受けた場合だけ、ここで更新を終える。
        if (scene && scene->StartCrownDefeat(this))
            return;
    }

    // 幕間でも残った攻撃表現の寿命を進め、再開時に古い演出を残しません。
    UpdateCrownLances(deltaTime);
    UpdateRoyalWaves(deltaTime);
    UpdateRoyalBeams(deltaTime);
    armorOpenTimer_ = std::max(0.0f, armorOpenTimer_ - deltaTime);

    // 幕間の位置制御をシーンへ譲り、ボス側は表示行列だけを更新する。
    if (crownInterlude_) {
        SetVelocity({});
        engine::Object3D::Update(deltaTime);
        return;
    }

    // デバッグ要求は通常攻撃プレビューが終わってから一度だけ段階を切り替えます。
    if (pendingDebugSeals_ && debugPreviewAttackId_.empty()) {
        pendingDebugSeals_ = false;
        // 能力値がある場合だけHPを移行値に合わせ、第二形態の開始自体は要求に従って行う。
        if (GetParameters())
            SetParameter(&engine::EntityParameter::hp, GetEncounterMaximumHp() * 0.67f);
        BeginPhaseShift(2);
    }

    // 継承確認では結晶戦を準備してから、三つの結晶を破壊済みにする。
    if (pendingDebugInterlude_ && debugPreviewAttackId_.empty()) {
        pendingDebugInterlude_ = false;

        // 第二形態へ入っていないときだけ結晶を初期化し、既にある結晶は作り直さない。
        if (battlePhase_ != 2) {
            battlePhase_ = 2;
            InitializeCrownSeals();
        }

        for (std::int32_t i = 0; i < 3; ++i)
            TryBreakCrownSeal(crownSeals_[i].position, i, true);
    }

    // 封印段階は通常AIと移動を止め、封印・詠唱の進行だけを更新します。
    if (battlePhase_ == 2 &&
        (debugPreviewAttackId_.empty() || debugPreviewAttackId_ == kSealBarragePreviewId) && !IsDead()) {
        UpdateCrownSeals(deltaTime);
        SetVelocity({});
        ApplyBossAnimation(deltaTime);
        engine::Object3D::Update(deltaTime);
        UpdateSpellChant(deltaTime);
        return;
    }

    // 撃破または非表示では所有する攻撃を取り除き、基底クラスの終了処理を使う。
    if (IsDead() || !GetIsVisible()) {
        HideAttackTelegraph();
        ClearTransientVisuals();
        BaseEnemy::Update(deltaTime);
        return;
    }

    // 休眠・投げ復帰中は独自移動を止め、出現条件や基底クラスの状態処理を優先する。
    if (IsDormant() || IsThrowRecovering() || ShouldHandleDefeatEffect()) {
        HideAttackTelegraph();
        ResetLocomotion();
        BaseEnemy::Update(deltaTime);
        return;
    }

    EnsureBaseScale();
    scriptedMovementThisFrame_ = false;
    idleTimer_ += deltaTime;
    attackCooldown_ = (std::max)(0.0f, attackCooldown_ - deltaTime);
    locomotionCooldown_ = (std::max)(0.0f, locomotionCooldown_ - deltaTime);

    // 対象が失われたフレームは最後の向きを保ち、距離だけ到達不能値で扱います。
    engine::Vector3 targetDirection = lockedDirection_;
    float targetDistance = 9999.0f;

    if (GetTarget()) {
        targetDirection = NormalizePlanar(GetTarget()->GetWorldPosition() - GetWorldPosition());
        targetDistance = PlanarDistance(GetTarget()->GetWorldPosition(), GetWorldPosition());
    }

    UpdateBossTargetEngagement(targetDistance);

    // 鉛直速度を引き継ぎ、水平速度は個別行動の結果を使って組み直す。
    const engine::Vector3 previousVelocity = GetVelocity();
    engine::Vector3 velocity = previousVelocity;
    velocity.x = 0.0f;
    velocity.z = 0.0f;
    UpdateBehavior(deltaTime, targetDirection, targetDistance, velocity);

    // 待機中の追跡だけを平滑化し、地面のない方向へは次の一歩を出しません。
    if (attackState_ == AttackState::kIdle && locomotionPhase_ == LocomotionPhase::kNone && GetTarget() && GetParameters().has_value()) {
        velocity = combatMath::SmoothPlanarVelocity(previousVelocity, velocity, deltaTime, 4.5f);
        const engine::Vector3 candidate = GetTranslate() + engine::Vector3{velocity.x * 0.48f, 0.0f, velocity.z * 0.48f};

        if (IsGrounded() && !combatGroundQuery::CanWalkTo(GetTranslate(), candidate, this, {5.0f, 14.0f}, 1.4f)) {
            velocity.x = 0.0f;
            velocity.z = 0.0f;
        }
    }

    // 専用移動をしていない接地待機中は、移動距離を見た目の弾みの位相だけに使う。
    if (attackState_ == AttackState::kIdle && locomotionPhase_ == LocomotionPhase::kNone && IsGrounded()) {
        travelPhase_ += slimeBounceAnimator::PlanarSpeed(velocity) * deltaTime * 1.7f;
    }
    SetVelocity(velocity);
    ApplyBossAnimation(deltaTime);
    UpdateRoyalGlow(deltaTime);
    SyncCollisionBounds();
    // 演出や専用移動で座標を直接進めたフレームは、通常の重力・移動を重ねない。
    SetMotorSuspended(scriptedMovementThisFrame_);
    BaseEnemy::Update(deltaTime);
    SetMotorSuspended(false);
    UpdateSpellChant(deltaTime);
}

void EnemyFalseKingSlime::ForEachOwnedStandardVisual(const std::function<void(engine::Object3D*)>& visit) const {
    // 所有物の列挙を共通化し、不透明パスと半透明パスで描き漏れを作らない。
    for (const auto& seal : crownSeals_) {
        for (engine::Object3D* visual : {static_cast<engine::Object3D*>(seal.crystal.get()), seal.ring.get(),
                                         seal.link.get(), seal.gem.get()}) {
            visit(visual);
        }
    }

    // 飛行中の槍や波、光線は技本体が終わった後にも残りうるため、現在の技名で絞らず列挙する。
    for (const CrownLance& lance : crownLances_) {
        visit(lance.visual.get());
    }
    for (const RoyalWave& wave : royalWaves_) {
        visit(wave.visual.get());
        visit(wave.glow.get());
    }
    for (const RoyalBeam& beam : royalBeams_) {
        visit(beam.visual.get());
    }

    visit(rushWingsVisual_.get());
    visit(dominionSigilVisual_.get());
    visit(umbraFlameNetwork_.get());
    for (const UmbraFlamePortal& portal : umbraFlames_) {
        visit(portal.sigil.get());
    }

    for (const AstralReaverOrb& orb : astralReaverOrbs_) {
        for (EffectObject3D* effect : {orb.halo.get(), orb.blade.get(), orb.arc.get(), orb.core.get()}) {
            visit(effect);
        }
    }
    for (const auto& ring : astralReaverImpactRings_) {
        visit(ring.get());
    }
    visit(astralReaverTargetRing_.get());
    visit(astralReaverImpactCore_.get());

    for (EffectObject3D* effect :
         {nightfallLances_.skySigil.get(), nightfallLances_.skyCounterSigil.get(),
          nightfallLances_.skyOuterRing.get(), nightfallLances_.skyInnerRing.get(),
          nightfallLances_.targetSigil.get(), nightfallLances_.targetRing.get(), nightfallLances_.innerRing.get(),
          nightfallLances_.spearGlow.get(), nightfallLances_.spearCore.get(), nightfallLances_.impactRing.get(),
          nightfallLances_.impactCore.get(), nightfallLances_.groundArc.get(), nightfallLances_.impactBeam.get(),
          nightfallLances_.impactBeamHalo.get(), nightfallLances_.blastRing.get(), nightfallLances_.blastShell.get()}) {
        visit(effect);
    }
    for (const auto& barb : nightfallLances_.barbs) {
        visit(barb.get());
    }
    for (const auto& lightning : nightfallLances_.impactLightning) {
        visit(lightning.get());
    }
    for (const auto& lightning : nightfallLances_.groundLightning) {
        visit(lightning.get());
    }

    for (const auto& satellite : sealNightfallSatellites_) {
        for (EffectObject3D* effect : {satellite.sigil.get(), satellite.core.get(), satellite.glow.get()}) {
            visit(effect);
        }
    }

    // 結晶戦の障壁・地表・天空の各レイヤーも、ボス本体と別に合成方式を調べられるよう渡す。
    for (EffectObject3D* effect : {crownShieldRing_.get(), crownShieldShell_.get(),
                                   crownCastFloor_.get(), crownArenaRing_.get(), crownBarrageFlash_.get(),
                                   crownBarrageSkySigil_.get(), crownBarrageSkyCounterSigil_.get(),
                                   crownBarrageSkyOuterRing_.get(), crownBarrageSkyInnerRing_.get()}) {
        visit(effect);
    }
    for (const auto& effect : crownBarrageLayers_) {
        visit(effect.get());
    }
    for (const auto& spear : sealBarrage_) {
        visit(spear.model.get());
        for (EffectObject3D* effect : {spear.glow.get(), spear.portal.get(), spear.impactLightning.get(),
                                       spear.groundArc.get(), spear.impactArc.get(), spear.impactBeam.get()}) {
            visit(effect);
        }
    }
    for (const auto& seal : crownSeals_) {
        for (const auto& pulse : seal.energyPulses) {
            visit(pulse.get());
        }
    }
}

void EnemyFalseKingSlime::Draw(ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource) {
    // 本体が描画対象の場合に、可視でブレンドなしの所有部品と本体を通常パスへ出す。
    if (!GetIsRenderVisible()) {
        return;
    }

    BaseEnemy::Draw(pointLightResource, spotLightResource);
    ForEachOwnedStandardVisual([&](engine::Object3D* visual) {
        if (visual && visual->GetIsVisible() && GetOwnedVisualBlendMode(visual) == BlendMode::kNone) {
            visual->RefreshRenderCameraData();
            visual->Draw(pointLightResource, spotLightResource);
        }
    });
}

void EnemyFalseKingSlime::DrawOwnedTransparentVisuals(ID3D12Resource* pointLightResource,
                                                       ID3D12Resource* spotLightResource) {
    // 本体が描画対象でない間は、このパスに所有物や詠唱を追加しない。
    if (!GetIsRenderVisible()) {
        return;
    }

    // 地形の描画後に、深度を書かない加算・半透明の攻撃表示を重ねる。
    ForEachOwnedStandardVisual([&](engine::Object3D* visual) {
        if (visual && visual->GetIsVisible() && GetOwnedVisualBlendMode(visual) != BlendMode::kNone) {
            visual->RefreshRenderCameraData();
            visual->Draw(pointLightResource, spotLightResource);
        }
    });

    // 詠唱が存在するときだけ、合成付き部品の後に重ねる。
    if (spellChant_) {
        spellChant_->Draw(pointLightResource, spotLightResource);
    }
}

void EnemyFalseKingSlime::DrawForCamera(engine::Camera* camera, ID3D12Resource* pointLightResource,
                                        ID3D12Resource* spotLightResource, std::int32_t previewBufferIndex) {
    // カメラがあり本体が描画対象なら、そのカメラで本体と可視の不透明部品を描く。
    if (!camera || !GetIsRenderVisible()) {
        return;
    }

    engine::Object3D::DrawForCamera(camera, pointLightResource, spotLightResource, previewBufferIndex);
    ForEachOwnedStandardVisual([&](engine::Object3D* visual) {
        if (visual && visual->GetIsVisible() && GetOwnedVisualBlendMode(visual) == BlendMode::kNone) {
            visual->DrawForCamera(camera, pointLightResource, spotLightResource, previewBufferIndex);
        }
    });
}

void EnemyFalseKingSlime::DrawOwnedTransparentVisualsForCamera(engine::Camera* camera,
                                                                ID3D12Resource* pointLightResource,
                                                                ID3D12Resource* spotLightResource,
                                                                std::int32_t previewBufferIndex) {
    // 指定カメラと本体の可視性が有効な場合に、可視のブレンド付き部品を描く。
    if (!camera || !GetIsRenderVisible()) {
        return;
    }

    ForEachOwnedStandardVisual([&](engine::Object3D* visual) {
        if (visual && visual->GetIsVisible() && GetOwnedVisualBlendMode(visual) != BlendMode::kNone) {
            visual->DrawForCamera(camera, pointLightResource, spotLightResource, previewBufferIndex);
        }
    });
}

void EnemyFalseKingSlime::OnSwitchEvent(bool active) {
    // 外部イベントによる登場要求は、遭遇状態の待機から戦闘への入口です。
    if (!IsEncounterControlled()) {
        BaseEnemy::OnSwitchEvent(active);
        return;
    }

    activationRequested_ = active;

    // プレイ用の遭遇状態をまだ作っていない場合、出現・休眠を決める前に初期化する。
    if (!initializedForPlay_) {
        InitializeEncounterState();
    }

    // 有効要求では休眠中だけ出現を始め、無効要求では休眠中以外を休眠へ戻す。
    if (active && encounterState_ == EncounterState::kDormant) {
        BeginAppearance();
    } else if (!active && encounterState_ != EncounterState::kDormant) {
        ApplyDormantState();
    }
}

std::unique_ptr<engine::Object3D> EnemyFalseKingSlime::Clone() const {
    // 描画と衝突資源を複製先で作り、配置設定だけを引き継ぎます。
    auto clone = std::make_unique<EnemyFalseKingSlime>();
    assert(GetObjectCommon() != nullptr);
    clone->Initialize(GetObjectCommon(), GetModelName());
    clone->CopyFrom(this);
    clone->SetTarget(GetTarget());
    clone->SetDetectionRange(GetDetectionRange());
    clone->baseScale_ = baseScale_;
    clone->hasBaseScale_ = true;
    return clone;
}

void EnemyFalseKingSlime::ApplyManagedScale(const engine::Vector3& scale) {
    // 通常の跳ねと攻撃姿勢が参照する基準スケールも同時に更新します。
    authoredScale_ = scale;
    baseScale_ = scale;
    hasBaseScale_ = true;
    authoredStateCaptured_ = true;
    SetScale(scale);
    SyncCollisionBounds();
}

// 特殊材質パスに渡す演出が一つでも可視なら true を返す。
bool EnemyFalseKingSlime::HasOwnedSpecialMaterialVisuals() const {
    // 登場の門・オーラ・衝撃は本体の可視状態にかかわらず調べる。
    for (const auto* visual : {introPortal_.get(), bossAura_.get(), introShockwave_.get()})
        if (visual && visual->GetIsVisible()) return true;
    // 残りの戦闘用演出は本体が可視の場合だけ調べる。
    if (!GetIsVisible()) {
        return false;
    }

    if (crownShieldBarrier_ && crownShieldBarrier_->GetIsVisible()) {
        return true;
    }

    for (const CrownLance& lance : crownLances_) {
        if (lance.portal && lance.portal->GetIsVisible())
            return true;
        if (lance.aura && lance.aura->GetIsVisible())
            return true;
    }

    if (shockwaveChargeVisual_ && shockwaveChargeVisual_->GetIsVisible()) {
        return true;
    }

    for (const RoyalBeam& beam : royalBeams_) {
        if (beam.glow && beam.glow->GetIsVisible())
            return true;
    }

    for (const UmbraFlamePortal& portal : umbraFlames_) {
        if (portal.flame && portal.flame->GetIsVisible())
            return true;
    }

    return false;
}

void EnemyFalseKingSlime::DrawOwnedSpecialMaterialVisuals(uint32_t depthSrvHandle, uint32_t grabSrvHandle) {
    // 本番カメラの向きへオーラを揃え、可視の特殊材質部品を材質別の描画経路へ出す。
    AlignBossAura(CameraManager::GetInstance()->GetActiveCamera());
    for (auto* visual : {introPortal_.get(), bossAura_.get(), introShockwave_.get()}) {
        if (visual && visual->GetIsVisible()) {
            visual->RefreshRenderCameraData();
            visual->DrawGatePortal(depthSrvHandle, grabSrvHandle);
        }
    }
    // 通常パスで描いた発光コアに、特殊材質の外周を追加します。
    if (crownShieldBarrier_ && crownShieldBarrier_->GetIsVisible()) {
        crownShieldBarrier_->RefreshRenderCameraData();
        crownShieldBarrier_->DrawGatePortal(depthSrvHandle, grabSrvHandle);
    }

    for (const CrownLance& lance : crownLances_) {
        if (lance.portal && lance.portal->GetIsVisible()) {
            lance.portal->RefreshRenderCameraData();
            lance.portal->DrawGatePortal(depthSrvHandle, grabSrvHandle);
        }

        if (lance.aura && lance.aura->GetIsVisible()) {
            lance.aura->RefreshRenderCameraData();
            lance.aura->DrawCrownUnlock(depthSrvHandle, grabSrvHandle);
        }
    }

    if (shockwaveChargeVisual_ && shockwaveChargeVisual_->GetIsVisible()) {
        shockwaveChargeVisual_->RefreshRenderCameraData();
        shockwaveChargeVisual_->DrawShockwave(depthSrvHandle, grabSrvHandle);
    }

    for (const RoyalBeam& beam : royalBeams_) {
        if (beam.glow && beam.glow->GetIsVisible()) {
            beam.glow->RefreshRenderCameraData();
            beam.glow->DrawLaser(depthSrvHandle, grabSrvHandle);
        }
    }

    for (const UmbraFlamePortal& portal : umbraFlames_) {
        if (portal.flame && portal.flame->GetIsVisible()) {
            portal.flame->RefreshRenderCameraData();
            portal.flame->DrawFire(depthSrvHandle, grabSrvHandle);
        }
    }
}

void EnemyFalseKingSlime::SetDebugPreviewAttackId(const std::string& attackId) {
    // プレビュー攻撃の切り替え時に前の予兆・所有演出を片付けます。
    if (!attackId.empty()) {
        // 隔離プレビューは原点から離れた場所に置かれ、床に衝突判定がない。
        // 登場演出を通らないため、ここで接地の代替高さと見た目の基準を記録する。
        CaptureAuthoredState(true);
    }

    debugPreviewAttackId_ = attackId;
    ResetLocomotion();
    currentAttack_ = AttackKind::kNone;
    attackState_ = AttackState::kIdle;
    attackCooldown_ = 0.05f;
    attackTimer_ = 0.0f;
    HideAttackTelegraph();
    ClearTransientVisuals();

    // 結晶戦プレビューは第二形態に入り、出現から詠唱、失敗後の墜槍までを進める状態を作る。
    if (attackId == kSealBarragePreviewId) {
        battlePhase_ = 2;
        InitializeCrownSeals();

    } else if (attackId == kCrownDominionAttackId || attackId == kUmbraFlameAttackId || attackId == kAstralReaverAttackId ||
        attackId == kNightfallLancesAttackId) {
        // 最終形態用の技は第三形態へ合わせる。
        battlePhase_ = 3;
    } else if (attackId == kRoyalCrossAttackId) {
        // 十字光線は第二形態以降の設定を使う。すでに第三形態ならそのままにする。
        battlePhase_ = (std::max)(battlePhase_, 2);
    }
}

const char* EnemyFalseKingSlime::GetDebugAttackPhaseName() const {
    // プレビュー表示用に攻撃状態を短い日本語へ対応付ける。
    switch (attackState_) {
    case AttackState::kWindup:
        return "予兆";

    case AttackState::kActive:
        return "攻撃";

    case AttackState::kRecovery:
        return "硬直";

    case AttackState::kPhaseShift:
        return "形態移行";

    default:
        return "待機";
    }
}

const char* EnemyFalseKingSlime::GetDebugAttackBodyName() const {
    // 専用プレビュー ID があれば優先し、なければ実行中の攻撃名を返す。
    if (debugPreviewAttackId_ == kSealBarragePreviewId)
        return "Crown Seal Barrage";
    AttackKind kind = currentAttack_;

    if (kind == AttackKind::kNone) {
        kind = ResolveDebugAttack();
    }

    switch (kind) {
    case AttackKind::kCrownLanceRain:
        return "Crown Lance Mesh";

    case AttackKind::kRoyalShockwave:
        return "Spiked Royal Wave Mesh";

    case AttackKind::kKingRush:
        return "Rush Wings + Boss Body";

    case AttackKind::kRoyalCross:
        return "Sweeping Royal Blade Mesh";

    case AttackKind::kCrownDominion:
        return "Dominion Sigil + Blades + Lances";

    case AttackKind::kUmbraFlame:
        return "Umbra Flame Sigils + Violet Flame Volumes";

    case AttackKind::kAstralReaver:
        return "Astral Reaver Ring Cutters + Orbital Impact Rings";

    case AttackKind::kNightfallLances:
        return "Nightfall Lance + Target Sigil";

    default:
        return "None";
    }
}

size_t EnemyFalseKingSlime::GetDebugVisibleAttackVisualCount() const {
    // プレビューの表示確認用に可視の演出を数える。波と炎ポータルは、構成部品の片方以上が見えていれば一体と数える。
    size_t visibleCount = 0;

    for (const CrownLance& lance : crownLances_) {
        if (lance.visual && lance.visual->GetIsVisible()) {
            ++visibleCount;
        }
    }

    for (const RoyalWave& wave : royalWaves_) {
        if ((wave.visual && wave.visual->GetIsVisible()) || (wave.glow && wave.glow->GetIsVisible())) {
            ++visibleCount;
        }
    }

    for (const RoyalBeam& beam : royalBeams_) {
        if (beam.visual && beam.visual->GetIsVisible()) {
            ++visibleCount;
        }
    }

    if (rushWingsVisual_ && rushWingsVisual_->GetIsVisible()) {
        ++visibleCount;
    }

    if (dominionSigilVisual_ && dominionSigilVisual_->GetIsVisible()) {
        ++visibleCount;
    }

    if (umbraFlameNetwork_ && umbraFlameNetwork_->GetIsVisible()) {
        ++visibleCount;
    }

    if (spellChant_) {
        visibleCount += static_cast<size_t>(spellChant_->GetVisibleLayerCount());
    }

    for (const UmbraFlamePortal& portal : umbraFlames_) {
        if ((portal.sigil && portal.sigil->GetIsVisible()) || (portal.flame && portal.flame->GetIsVisible()))
            ++visibleCount;
    }

    // 回転刃以降の重ね描きは、生成済みかつ可視の部品を個別に数える。
    for (const AstralReaverOrb& orb : astralReaverOrbs_) {
        for (EffectObject3D* effect : {orb.halo.get(), orb.blade.get(), orb.arc.get(), orb.core.get()}) {
            if (effect && effect->GetIsVisible())
                ++visibleCount;
        }
    }

    for (const auto& ring : astralReaverImpactRings_) {
        if (ring && ring->GetIsVisible())
            ++visibleCount;
    }

    for (EffectObject3D* effect : {astralReaverTargetRing_.get(), astralReaverImpactCore_.get()}) {
        if (effect && effect->GetIsVisible())
            ++visibleCount;
    }

    for (EffectObject3D* effect :
         {nightfallLances_.skySigil.get(), nightfallLances_.skyCounterSigil.get(),
          nightfallLances_.skyOuterRing.get(), nightfallLances_.skyInnerRing.get(),
          nightfallLances_.targetSigil.get(), nightfallLances_.targetRing.get(), nightfallLances_.innerRing.get(),
          nightfallLances_.spearCore.get(), nightfallLances_.spearGlow.get(), nightfallLances_.impactRing.get(),
          nightfallLances_.impactCore.get(), nightfallLances_.groundArc.get(), nightfallLances_.impactBeam.get(),
          nightfallLances_.impactBeamHalo.get(), nightfallLances_.blastRing.get(), nightfallLances_.blastShell.get()}) {
        if (effect && effect->GetIsVisible())
            ++visibleCount;
    }

    for (const auto& barb : nightfallLances_.barbs) {
        if (barb && barb->GetIsVisible())
            ++visibleCount;
    }

    for (const auto& lightning : nightfallLances_.impactLightning) {
        if (lightning && lightning->GetIsVisible())
            ++visibleCount;
    }

    for (const auto& lightning : nightfallLances_.groundLightning) {
        if (lightning && lightning->GetIsVisible())
            ++visibleCount;
    }

    for (const auto& effect : crownBarrageLayers_) {
        if (effect && effect->GetIsVisible())
            ++visibleCount;
    }

    for (const auto& spear : sealBarrage_) {
        if (spear.model && spear.model->GetIsVisible())
            ++visibleCount;
        for (EffectObject3D* effect : {spear.glow.get(), spear.portal.get(), spear.impactLightning.get(),
                                       spear.groundArc.get(), spear.impactArc.get(), spear.impactBeam.get()}) {
            if (effect && effect->GetIsVisible())
                ++visibleCount;
        }
    }

    for (EffectObject3D* effect : {crownBarrageSkySigil_.get(), crownBarrageSkyCounterSigil_.get(),
                                   crownBarrageSkyOuterRing_.get(), crownBarrageSkyInnerRing_.get()}) {
        if (effect && effect->GetIsVisible())
            ++visibleCount;
    }

    return visibleCount;
}

void EnemyFalseKingSlime::EnsureBaseScale() {
    // 初回だけ配置時の寸法を取得し、アニメーションの復帰先にします。
    if (hasBaseScale_) {
        return;
    }

    baseScale_ = GetScale();
    authoredScale_ = baseScale_;
    hasBaseScale_ = true;
}

// 攻撃と形態移行を優先し、待機中だけ次の技の射程に合わせて位置を調整します。
void EnemyFalseKingSlime::UpdateBehavior(float deltaTime, const engine::Vector3& targetDirection, float targetDistance,
                                         engine::Vector3& velocity) {
    // 形態移行、攻撃中、待機中の順に処理し、同じフレームで二つの行動を進めません。
    if (attackState_ == AttackState::kPhaseShift) {
        UpdatePhaseShift(deltaTime, velocity);
        return;
    }

    // 形態移行以外の攻撃状態が残っていれば、待機ロジックを実行せずその攻撃を進める。
    if (attackState_ != AttackState::kIdle) {
        UpdateAttack(deltaTime, targetDirection, targetDistance, velocity);
        return;
    }

    // 専用移動を始めている間は、次の技や形態移行を選ばず移動を完了する。
    if (locomotionPhase_ != LocomotionPhase::kNone) {
        UpdateLocomotion(deltaTime, targetDirection, velocity);
        return;
    }

    const std::int32_t desiredPhase = ResolveDesiredBattlePhase();

    // 要求形態が上がった通常戦では次の技より形態移行を優先する。固定技プレビューは選択状態を維持する。
    if (desiredPhase > battlePhase_ && debugPreviewAttackId_.empty()) {
        BeginPhaseShift(desiredPhase);
        return;
    }

    // 追跡対象か能力値が無い場合は、鉛直速度を維持しながら徘徊へ切り替える。
    if (!GetTarget() || !GetParameters().has_value()) {
        const float verticalVelocity = velocity.y;
        velocity = CalculateWanderVelocity(deltaTime, 0.34f, 0.28f);
        velocity.y = verticalVelocity;
        UpdateFacing(velocity, 0.06f);
        return;
    }

    UpdateFacing(targetDirection);

    // 攻撃プレビューでは発見リアクションを挟まず、選択した攻撃の予兆へ直行します。
    if (debugPreviewAttackId_.empty() &&
        UpdateNoticeReaction(deltaTime, targetDistance, GetDetectionRange(), targetDirection)) {
        return;
    }

    const AttackKind debugAttack = ResolveDebugAttack();
    const AttackKind plannedAttack =
        debugAttack != AttackKind::kNone ? debugAttack : ResolveAutomaticAttack(targetDistance);
    const EnemyAttackDefinition& planned = GetAttackDefinition(GetAttackId(plannedAttack));
    const bool inRange = targetDistance >= planned.minRange && targetDistance <= planned.maxRange;

    // 交戦済み、接地中、クールダウンなし、射程内の条件が揃ったときに技を始める。
    if (IsBossTargetEngaged() && attackCooldown_ <= 0.0f && inRange && IsGrounded()) {
        StartAttack(plannedAttack, targetDirection);
        return;
    }

    // 固定技プレビュー、空中、または移動の待ち時間中は新しい位置取りを始めない。
    if (!debugPreviewAttackId_.empty() || !IsGrounded() || locomotionCooldown_ > 0.0f) {
        return;
    }

    // 推奨距離より遠ければ斜め前、近ければ斜め後ろ、適正なら横へ移動する。
    const float preferredDistance = (std::clamp)(planned.recommendedTargetDistance, 7.0f, 15.0f);
    const float side = (locomotionStepSerial_ & 1u) == 0u ? 1.0f : -1.0f;
    const engine::Vector3 lateral = {targetDirection.z * side, 0.0f, -targetDirection.x * side};
    const bool farAway = targetDistance > preferredDistance + 3.0f;
    const bool tooClose = targetDistance < preferredDistance - 3.0f;
    const engine::Vector3 moveDirection = farAway
        ? NormalizePlanar(targetDirection + lateral * 0.32f)
        : (tooClose ? NormalizePlanar(targetDirection * -1.0f + lateral * 0.18f) : lateral);
    const float distance = farAway
        ? std::clamp(targetDistance - preferredDistance, 2.4f, 4.6f)
        : std::clamp(GetParameters()->speed * 0.55f, 0.80f, 1.35f);

    ++locomotionStepSerial_;
    if (!BeginLocomotion(moveDirection, distance, farAway)) {
        // 崖や段差で着地点を取れない場合は、次フレームに同じ移動を連打しません。
        locomotionCooldown_ = 0.35f;
    }
}

bool EnemyFalseKingSlime::BeginLocomotion(const engine::Vector3& direction, float distance, bool leap) {
    // 着地点だけでなく経路の床も確認し、段差や穴へ直接移動しません。
    const engine::Vector3 planarDirection = NormalizePlanar(direction);
    // 方向を決められないほど水平成分が小さい場合は移動を組まない。
    if (planarDirection.x * planarDirection.x + planarDirection.z * planarDirection.z <= 0.0001f) {
        return false;
    }

    // 現在位置と移動先の地面を測る。判定では自身を除いて、床の法線と高さを確認する。
    const engine::Vector3 start = GetTranslate();
    engine::PhysicsQueryFilter filter;
    filter.mask = engine::collisionCategories::Get("ground");
    filter.ignoredObject = this;
    const auto probeGround = [&](const engine::Vector3& position) {
        return engine::CollisionManager::GetInstance()->Raycast(position + engine::Vector3{0.0f, 5.0f, 0.0f},
                                                        {0.0f, -1.0f, 0.0f}, 14.0f, filter);
    };
    const engine::RaycastHit startHit = probeGround(start);
    // 現在地点に下向きの探索で床が見つからないか、法線が急なら移動を開始しない。
    if (!startHit.isHit || startHit.normal.y < 0.55f) return false;

    engine::Vector3 candidate = start + planarDirection * distance;
    engine::RaycastHit endHit;
    bool safeRoute = false;
    // 終点と中間が安全なときだけ採用し、失敗時は半分の距離でもう一度調べる。
    for (std::int32_t attempt = 0; attempt < 2; ++attempt) {
        endHit = probeGround(candidate);
        const engine::RaycastHit midpointHit = probeGround((start + candidate) * 0.5f);
        safeRoute = endHit.isHit && midpointHit.isHit && endHit.normal.y >= 0.55f &&
                    midpointHit.normal.y >= 0.55f &&
                    std::abs(endHit.hitPoint.y - startHit.hitPoint.y) <= 1.4f &&
                    std::abs(midpointHit.hitPoint.y - startHit.hitPoint.y) <= 1.4f;
        if (safeRoute) break;
        candidate = start + planarDirection * (distance * 0.5f);
    }
    // 終点と中間の条件を満たせない場合は、既存の位置と状態を保ったまま失敗を返す。
    if (!safeRoute) return false;

    // 着地位置は地面の高低差を足して確定し、跳躍と横移動それぞれの速度で所要秒数を決める。
    locomotionStart_ = start;
    locomotionEnd_ = {candidate.x, start.y + endHit.hitPoint.y - startHit.hitPoint.y, candidate.z};
    locomotionDirection_ = planarDirection;
    locomotionLeap_ = leap;
    locomotionArcHeight_ = leap ? std::clamp(GetParameters()->jumpPower * 0.055f, 0.95f, 1.50f) : 0.0f;
    const float travelDistance = PlanarDistance(start, locomotionEnd_);
    locomotionTravelDuration_ = leap
        ? std::clamp(travelDistance / (std::max)(6.5f, GetParameters()->speed * 3.0f), 0.44f, 0.65f)
        : std::clamp(travelDistance / (std::max)(3.0f, GetParameters()->speed * 1.8f), 0.24f, 0.36f);
    locomotionTimer_ = 0.0f;
    locomotionPhase_ = LocomotionPhase::kWindup;
    return true;
}

void EnemyFalseKingSlime::UpdateLocomotion(float deltaTime, const engine::Vector3& targetDirection,
                                           engine::Vector3& velocity) {
    velocity.x = 0.0f;
    velocity.z = 0.0f;
    locomotionTimer_ += deltaTime;
    const float turnFollow = 1.0f - std::exp(-deltaTime * (locomotionLeap_ ? 9.0f : 12.0f));

    // 踏み切りまでは水平速度を止めて向きだけを整え、出発フレームの現在位置を軌道の始点にする。
    if (locomotionPhase_ == LocomotionPhase::kWindup) {
        UpdateFacing(locomotionLeap_ ? locomotionDirection_ : targetDirection, turnFollow);
        // 構えの時間を使い切ったフレームにだけ軌道始点を取り直し、跳躍なら接地を解除する。
        if (locomotionTimer_ >= (locomotionLeap_ ? 0.16f : 0.12f)) {
            locomotionPhase_ = LocomotionPhase::kTravel;
            locomotionTimer_ = 0.0f;
            locomotionEnd_.y += GetTranslate().y - locomotionStart_.y;
            locomotionStart_ = GetTranslate();
            // 移動開始フレームの重力だけが先行して軌道の始点をずらすのを防ぎます。
            scriptedMovementThisFrame_ = true;
            velocity.y = 0.0f;
            if (locomotionLeap_) SetGrounded(false);
        }
        return;
    }

    // 用意した経路を補間し、跳躍だけに高さを足して移動する。
    if (locomotionPhase_ == LocomotionPhase::kTravel) {
        const float progress = std::clamp(locomotionTimer_ / locomotionTravelDuration_, 0.0f, 1.0f);
        engine::Vector3 position = LerpVector3(locomotionStart_, locomotionEnd_, SmoothStep01(progress));
        position.y += std::sin(progress * kPi) * locomotionArcHeight_;
        SetTranslate(position);
        scriptedMovementThisFrame_ = true;
        velocity.y = 0.0f;
        if (locomotionLeap_) SetGrounded(false);
        UpdateFacing(locomotionLeap_ ? locomotionDirection_ : targetDirection, turnFollow);

        // 着地点を確定してから接地に戻し、連続で跳ばないよう次の移動まで待つ。
        if (progress >= 1.0f) {
            locomotionPhase_ = LocomotionPhase::kLanding;
            locomotionTimer_ = 0.0f;
            SetTranslate(locomotionEnd_);
            SetGrounded(true);
            locomotionCooldown_ = locomotionLeap_ ? 0.76f : 1.08f;
        }
        return;
    }

    // 着地と横移動の停止中は足元を固定し、次の攻撃に向けて標的へ向き直ります。
    SetTranslate(locomotionEnd_);
    SetGrounded(true);
    scriptedMovementThisFrame_ = true;
    velocity = {};
    UpdateFacing(targetDirection, turnFollow);
    // 着地姿勢を見せる時間を使い切ったら、専用移動を解除する。
    if (locomotionTimer_ >= (locomotionLeap_ ? 0.22f : 0.17f)) {
        locomotionPhase_ = LocomotionPhase::kNone;
        locomotionTimer_ = 0.0f;
    }
}

// 休眠や技開始で専用移動を中断し、位相・時間・再開待ちを初期値へ戻す。
void EnemyFalseKingSlime::ResetLocomotion() {
    locomotionPhase_ = LocomotionPhase::kNone;
    locomotionTimer_ = 0.0f;
    locomotionCooldown_ = 0.0f;
    locomotionTravelDuration_ = 0.0f;
    locomotionArcHeight_ = 0.0f;
    locomotionLeap_ = false;
}

// 予兆開始時に照準と各演出タイマーを固定し、技ごとの配置物を準備します。
void EnemyFalseKingSlime::StartAttack(AttackKind kind, const engine::Vector3& targetDirection) {
    // 予兆開始時に照準を固定し、技ごとの見た目を攻撃時間に先立って準備します。
    if (kind == AttackKind::kNone) {
        return;
    }

    ResetLocomotion();

    // 標的の地面位置と攻撃開始位置を固定し、予兆や連射に使う時刻・番号を初期化する。
    currentAttack_ = kind;
    attackState_ = AttackState::kWindup;
    lockedDirection_ = NormalizePlanar(targetDirection);
    lockedTargetPosition_ = GetTarget() ? FindGroundPoint(GetTarget()->GetWorldPosition())
                                    : FindGroundPoint(GetTranslate() + lockedDirection_ * 9.0f);
    attackStartPosition_ = FindGroundPoint(GetTranslate());
    attackEndPosition_ = attackStartPosition_;
    const EnemyAttackDefinition& attack = GetCurrentAttackDefinition();
    attackStateDuration_ = (std::max)(0.18f, attack.windupDuration);
    attackTimer_ = attackStateDuration_;
    actionTimer_ = 0.0f;
    effectTimer_ = 0.0f;
    actionIndex_ = 0;
    lanceSpawnSerial_ = 0;
    warningTriggered_ = false;
    rushDamageApplied_ = false;
    shockwaveLanded_ = false;

    // 形態で槍の本数を選び、予兆中から射出口と発射の時刻を個別に決める。
    if (kind == AttackKind::kCrownLanceRain) {
        PrepareLanceTargets(false);
        const std::int32_t count = battlePhase_ == 1 ? 10 : (battlePhase_ == 2 ? 14 : 18);
        const float firstGateDelay = attackStateDuration_ * 0.08f;
        const float gateSequenceDuration = attackStateDuration_ * 0.62f;

        for (std::int32_t index = 0; index < count; ++index) {
            const float sequenceRate = count > 1 ? static_cast<float>(index) / static_cast<float>(count - 1) : 0.0f;
            const float gateDelay = firstGateDelay + gateSequenceDuration * sequenceRate;
            const float launchAt = attackStateDuration_ + static_cast<float>(index) * kLanceInterval;
            const float readyDuration = (std::max)(0.08f, launchAt - gateDelay - 0.24f);
            SpawnCrownLance(lanceTargets_[static_cast<size_t>(index)], gateDelay, readyDuration);
        }

        actionIndex_ = count;
    // 衝撃波は収束輪を、領域技は追加で落とす槍の対象地点を準備する。
    } else if (kind == AttackKind::kRoyalShockwave) {
        PrepareRoyalShockwave();
    } else if (kind == AttackKind::kCrownDominion) {
        PrepareLanceTargets(true);
    } else if (kind == AttackKind::kUmbraFlame || kind == AttackKind::kAstralReaver ||
               kind == AttackKind::kNightfallLances) {
        // 炎は地面の陣、回転刃は刃と着弾演出、夜墜槍は固有の標的と表示を準備する。
        if (kind == AttackKind::kUmbraFlame) {
            PrepareUmbraFlames();
        } else if (kind == AttackKind::kAstralReaver) {
            PrepareAstralReaver();
        } else {
            PrepareNightfallLances();
        }

        // 炎・回転刃・夜墜槍は共通の詠唱を使い、技ごとに色と消え始める時刻を替える。
        BossChantStyle style;
        style.sigilEffectPath = kUmbraFlameSigilEffect;
        style.motePreset =
            kind == AttackKind::kNightfallLances ? kNightfallLancesChargePreset : "enemy/false_king/false_king_chant_motes";
        style.accentColor = kind == AttackKind::kNightfallLances
                                ? engine::Vector4{0.90f, 0.08f, 0.24f, 1.0f}
                                : (kind == AttackKind::kAstralReaver ? engine::Vector4{0.82f, 0.22f, 1.0f, 1.0f}
                                                                    : engine::Vector4{0.80f, 0.30f, 1.0f, 1.0f});
        style.radius = std::clamp(baseScale_.x * 1.45f, 2.4f, 4.2f);
        style.fadeStart = kind == AttackKind::kAstralReaver || kind == AttackKind::kNightfallLances
                              ? 0.16f
                              : (std::max)(0.0f, attack.activeDuration - 0.24f);
        spellChant_ = std::make_unique<BossChantVisual>();

        // 詠唱部品を作れなければ表示だけを断念し、技自体の開始は取り消さない。
        if (!spellChant_->Start(GetObjectCommon(), std::move(style))) {
            spellChant_.reset();
        }
    }

    // 巡回の順番と前回の技は実戦でだけ記録する。
    if (debugPreviewAttackId_.empty()) {
        lastAutomaticAttack_ = kind;
        ++attackCycleIndex_;
        ++automaticAttackSerial_;
    }
}

void EnemyFalseKingSlime::PrepareRoyalShockwave() {
    // 白い柔らかな輪は攻撃判定ではなく、外から中心へ力が集まる充填層に限定します。
    shockwaveChargeVisual_ = CreateRoyalVisual("FalseKing_ShockwaveCharge", "Primitives/cylinder",
                                               kShockwaveMaterialType, {1.0f, 0.92f, 0.76f, 0.0f}, 1.65f);
    // 充填層を作成できたら最初は隠す。描画データもある場合のみ収束の周期と強さを設定する。
    if (shockwaveChargeVisual_) {
        shockwaveChargeVisual_->SetIsVisible(false);

        if (engine::MeshRenderer* renderer = shockwaveChargeVisual_->GetMeshRenderer()) {
            if (engine::MeshRenderer::WaterParamForGpu* charge = renderer->GetWaterParamData()) {
                charge->waveSpeed = 0.82f;
                charge->waveFrequency = 3.2f;
                charge->effectType = 3.0f;
                charge->effectScale = 0.92f;
                charge->effectSoftness = 0.22f;
                charge->effectIntensity = 1.88f;
            }
        }

        shockwaveChargeVisual_->UpdateWorldMatrix(true);
    }
}

void EnemyFalseKingSlime::UpdateRoyalShockwave(bool windup, float progress, float deltaTime, engine::Vector3& velocity) {
    // ボス直下に収束する予兆を出し、実行中は UpdateShockwaveJump() が跳躍と衝撃波を進める。
    if (windup) {
        // 溜めはボス直下の一層だけに絞り、着地後の攻撃リングと混同させません。
        const engine::Vector3 chargeCenter = FindGroundPoint(GetTranslate()) + engine::Vector3{0.0f, 0.06f, 0.0f};
        const float contraction = SmoothStep01(progress);
        const float pulse = 1.0f + std::sin(progress * kPi * 8.0f) * 0.06f * progress;

        // 収束輪が生成済みなら、予兆の進み具合で地面の半径・透明度・回転を更新する。
        if (shockwaveChargeVisual_) {
            const float radius = engine::Math::Lerp(5.8f, 2.75f, contraction) * pulse;
            shockwaveChargeVisual_->SetIsVisible(progress > 0.02f);
            shockwaveChargeVisual_->SetTranslate(chargeCenter);
            shockwaveChargeVisual_->SetScale({radius, 0.045f + progress * 0.035f, radius});
            shockwaveChargeVisual_->SetRotationY(-idleTimer_ * (0.45f + progress * 1.35f));
            shockwaveChargeVisual_->SetColor(
                {1.0f, battlePhase_ >= 3 ? 0.78f : 0.94f, battlePhase_ >= 3 ? 1.0f : 0.78f, 0.34f + progress * 0.46f});
            shockwaveChargeVisual_->Update(deltaTime);
            shockwaveChargeVisual_->UpdateWorldMatrix(true);
        }

        // 間隔を過ぎたときだけ輪の上から内向きに粒子を出す。特殊材質を省くプレビューにも予兆を残す。
        if (effectTimer_ <= 0.0f) {
            constexpr std::int32_t kChargePointCount = 12;
            const float chargeRadius = engine::Math::Lerp(5.8f, 2.75f, contraction) * pulse;
            const float rotationOffset = idleTimer_ * (0.55f + progress * 0.65f);

            for (std::int32_t pointIndex = 0; pointIndex < kChargePointCount; ++pointIndex) {
                const float angle =
                    rotationOffset + static_cast<float>(pointIndex) * kTwoPi / static_cast<float>(kChargePointCount);
                const engine::Vector3 radial{std::sin(angle), 0.0f, std::cos(angle)};
                const engine::Vector3 particlePosition = chargeCenter + radial * chargeRadius + engine::Vector3{0.0f, 0.14f, 0.0f};
                EmitDirectedPreset(kShockwaveChargeRingPreset, particlePosition,
                                   {-radial.x, 0.16f + progress * 0.10f, -radial.z}, 0.46f + progress * 0.38f);
            }

            effectTimer_ += 0.13f;
        }
    } else {
        UpdateShockwaveJump(deltaTime, velocity);
    }
}

void EnemyFalseKingSlime::UpdateAttack(float deltaTime, const engine::Vector3& targetDirection, float, engine::Vector3& velocity) {
    // 予兆と実行は段階別の技更新へ渡す。硬直は横移動を止め、タイマーが尽きたら待機へ戻す。
    if (attackState_ == AttackState::kWindup) {
        UpdateAttackPattern(AttackState::kWindup, deltaTime, targetDirection, velocity);
        return;
    }

    if (attackState_ == AttackState::kActive) {
        UpdateAttackPattern(AttackState::kActive, deltaTime, targetDirection, velocity);
        return;
    }

    if (attackState_ == AttackState::kRecovery) {
        velocity.x = 0.0f;
        velocity.z = 0.0f;
        attackTimer_ = (std::max)(0.0f, attackTimer_ - deltaTime);
        HideAttackTelegraph();

        if (attackTimer_ <= 0.0f) {
            FinishAttack();
        }
    }
}

void EnemyFalseKingSlime::UpdateAttackPattern(AttackState phase, float deltaTime, const engine::Vector3& targetDirection, engine::Vector3& velocity) {
    // このフレームの段階を引数で受け取り、技の処理が終わってから次の段階へ移る。
    const bool windup = phase == AttackState::kWindup;
    const EnemyAttackDefinition* attack = windup ? &GetCurrentAttackDefinition() : nullptr;
    // 現在の段階の残り時間を減らす。連続攻撃の間隔は実行中だけ進める。
    attackTimer_ = (std::max)(0.0f, attackTimer_ - deltaTime);
    if (!windup) {
        actionTimer_ -= deltaTime;
    }
    effectTimer_ -= deltaTime;

    // 予兆の進行率は0から1へ進め、形態によって警告色を切り替える。
    const float progress = windup
        ? 1.0f - std::clamp(attackTimer_ / (std::max)(attackStateDuration_, 0.01f), 0.0f, 1.0f)
        : 0.0f;
    const engine::Vector4 warningColor =
        battlePhase_ >= 3 ? engine::Vector4{0.90f, 0.18f, 1.0f, 0.86f} : engine::Vector4{1.0f, 0.72f, 0.12f, 0.86f};

    if (windup) {
        // 予兆が確定するまでは追尾し、確定済みの地点を使う技は上書きしない。
        if (attackTimer_ > attack->warningLeadTime && GetTarget() && currentAttack_ != AttackKind::kCrownLanceRain &&
            currentAttack_ != AttackKind::kUmbraFlame && currentAttack_ != AttackKind::kAstralReaver &&
            currentAttack_ != AttackKind::kNightfallLances) {
            lockedDirection_ = NormalizePlanar(targetDirection);
            lockedTargetPosition_ = FindGroundPoint(GetTarget()->GetWorldPosition());

            // 外側の追尾許可条件は CrownLanceRain を除外しているため、通常の更新ではここで着弾点を作り直さない。
            if (currentAttack_ == AttackKind::kCrownLanceRain) {
                PrepareLanceTargets(false);
            }
        }

        UpdateFacing(lockedDirection_, 0.18f);
    }

    // 同じ技の予兆と実行を並べ、表示した範囲と実際に発生する攻撃を照合できるようにする。
    switch (currentAttack_) {
    case AttackKind::kCrownLanceRain:
        // 準備済みの着弾点を表示する。実行側は未処理の番号が残るときだけ経過した間隔分の発生を進める。
        if (windup) {
            const size_t count = static_cast<size_t>(battlePhase_ == 1 ? 10 : (battlePhase_ == 2 ? 14 : 18));
            ShowAttackTelegraphReadableImpactAreas(lanceTargets_.data(), count, attack->radius, progress, warningColor);
        } else {
            const std::int32_t count = battlePhase_ == 1 ? 10 : (battlePhase_ == 2 ? 14 : 18);

            while (actionIndex_ < count && actionTimer_ <= 0.0f) {
                SpawnNextCrownLance(false);
                ++actionIndex_;
                actionTimer_ += kLanceInterval;
            }
        }
        break;

    case AttackKind::kRoyalShockwave:
        UpdateRoyalShockwave(windup, progress, deltaTime, velocity);
        break;

    case AttackKind::kKingRush:
        // 固定した向きに射線を予告し、発動してからの移動は UpdateRush() へ渡す。
        if (windup) {
            ShowAttackTelegraphLine(FindGroundPoint(GetTranslate()), lockedDirection_,
                                    (std::min)(attack->maxRange, 13.0f + static_cast<float>(battlePhase_) * 2.6f),
                                    attack->radius * 2.0f, progress, warningColor);
        } else {
            UpdateRush(deltaTime, targetDirection, velocity);
        }
        break;

    case AttackKind::kRoyalCross:
    case AttackKind::kCrownDominion:
        // 二つの技で放射状の予兆を共有する。光線は BeginActive() で起動し、領域技だけ実行中の追加処理を行う。
        if (windup) {
            const bool dominion = currentAttack_ == AttackKind::kCrownDominion;
            const std::int32_t count = RoyalBeamCount(dominion, battlePhase_);
            const float step = kTwoPi / static_cast<float>(count);
            const float fanCenter = std::atan2(lockedDirection_.x, lockedDirection_.z) +
                                    static_cast<float>(count - 1) * 0.5f * step;

            // 横ずらしをなくし、ボスの足元から実際の初期射線へ放射状に予告します。
            ShowAttackTelegraphLaneFan(FindGroundPoint(GetTranslate()),
                                       {std::sin(fanCenter), 0.0f, std::cos(fanCenter)}, attack->maxRange,
                                       RoyalBeamHalfWidth(dominion, attack->radius) * 2.0f,
                                       count, 0.0f, step, progress, warningColor);
        } else {
            velocity.x = 0.0f;
            velocity.z = 0.0f;
            if (currentAttack_ == AttackKind::kCrownDominion) {
                UpdateDominion(deltaTime);
            }
        }
        break;

    case AttackKind::kUmbraFlame:
        // 魔法陣と炎は UpdateUmbraFlames() が担当する。実行中はボスの水平移動を止める。
        if (!windup) {
            velocity.x = 0.0f;
            velocity.z = 0.0f;
        }
        UpdateUmbraFlames(deltaTime, !windup);
        break;

    case AttackKind::kAstralReaver:
        // 回転刃の準備と発動を同じ関数へ渡し、実行中はボスをその位置に留める。
        if (!windup) {
            velocity.x = 0.0f;
            velocity.z = 0.0f;
        }
        UpdateAstralReaver(deltaTime, !windup);
        break;

    case AttackKind::kNightfallLances:
        // 槍の予兆と発動は UpdateNightfallLances() に任せ、実行中の水平移動を止める。
        if (!windup) {
            velocity.x = 0.0f;
            velocity.z = 0.0f;
        }
        UpdateNightfallLances(deltaTime, !windup);
        break;

    default:
        break;
    }

    if (windup) {
        // 専用の溜め演出を持つ技を除き、発動へ近づくにつれて共通の粒子を短い間隔で出す。
        if (effectTimer_ <= 0.0f && currentAttack_ != AttackKind::kRoyalShockwave &&
            currentAttack_ != AttackKind::kUmbraFlame && currentAttack_ != AttackKind::kAstralReaver &&
            currentAttack_ != AttackKind::kNightfallLances) {
            EmitDirectedPreset(currentAttack_ == AttackKind::kCrownDominion ? kDominionPreset : kChargePreset,
                               GetWorldPosition() + engine::Vector3{0.0f, baseScale_.y * 1.5f, 0.0f},
                               {-lockedDirection_.x * 0.12f, 1.0f, -lockedDirection_.z * 0.12f}, 0.92f + progress * 0.78f);
            effectTimer_ += (std::max)(0.12f, 0.24f - progress * 0.08f);
        }

        // 残り時間がしきい値に達したフレームだけ、発動前の警告を強調する。
        if (!warningTriggered_ && attackTimer_ <= attack->warningLeadTime) {
            TriggerAttackTelegraphCue(warningColor);
            warningTriggered_ = true;
        }

        if (attackTimer_ <= 0.0f) {
            // このフレームの予兆と警告を更新し終えてから、判定や技の実体を起動する。
            BeginActive();
        }
    } else if (attackTimer_ <= 0.0f) {
        // 今フレームに予定された技の処理を済ませてから硬直へ移る。
        BeginRecovery();
    }
}

void EnemyFalseKingSlime::BeginActive() {
    // 予兆から実攻撃へ進め、命中判定の開始時刻を揃えます。
    const EnemyAttackDefinition& attack = GetCurrentAttackDefinition();
    attackState_ = AttackState::kActive;
    attackStateDuration_ = (std::max)(0.20f, attack.activeDuration);
    attackTimer_ = attackStateDuration_;
    actionTimer_ = 0.0f;
    effectTimer_ = 0.0f;
    actionIndex_ =
        currentAttack_ == AttackKind::kCrownLanceRain ? (battlePhase_ == 1 ? 10 : (battlePhase_ == 2 ? 14 : 18)) : 0;
    HideAttackTelegraph();

    switch (currentAttack_) {
    case AttackKind::kRoyalShockwave:
        // 跳躍を始め、着地時の衝撃波は毎フレームの更新へ引き渡す。
        BeginShockwaveJump();
        break;

    case AttackKind::kKingRush:
        // 突進の初回の進行方向を確定し、移動を始める。
        BeginRush();
        break;

    case AttackKind::kRoyalCross:
        // この技は放射状の射線だけを開始する。
        SpawnRoyalBeams(false);
        break;

    case AttackKind::kCrownDominion:
        // 領域技は射線に加え、紋章を作って後続の槍を落とす準備をする。
        SpawnRoyalBeams(true);
        dominionSigilVisual_ =
            CreateRoyalMesh("FalseKing_DominionSigil", kDominionSigilModel, {1.0f, 1.0f, 1.0f, 1.0f}, 2.15f);
        // 紋章の作成に成功したら、領域更新で表示するまでは非表示にしておく。
        if (dominionSigilVisual_) {
            dominionSigilVisual_->SetIsVisible(false);
            dominionSigilVisual_->UpdateWorldMatrix(true);
        }

        dominionLanceTimer_ = 0.32f;
        VfxSequencer::PlayOneShot(kDominionSequence, FindGroundPoint(GetTranslate()), {2.4f, 2.4f, 2.4f});
        break;

    case AttackKind::kUmbraFlame:
        // 魔法陣ごとに開始時刻をずらし、今回の扇形への命中を未判定にする。
        umbraFlameFanHit_ = false;

        for (UmbraFlamePortal& portal : umbraFlames_) {
            portal.activeAge = -portal.delay * 0.55f;
            portal.particleTimer = 0.0f;
        }

        break;

    case AttackKind::kAstralReaver:
        // 回転刃を発動の最初へ戻し、着弾を未発生にする。
        astralReaverElapsed_ = 0.0f;
        astralReaverImpactAge_ = -1.0f;
        astralReaverDamageApplied_ = false;
        break;

    case AttackKind::kNightfallLances:
        // 夜墜槍の落下・粒子・命中を最初から進められる状態にする。
        nightfallLances_.activeAge = 0.0f;
        nightfallLances_.particleTimer = 0.0f;
        nightfallLances_.damageApplied = false;
        break;

    default:
        break;
    }
}

void EnemyFalseKingSlime::BeginRecovery() {
    // 攻撃後の硬直では予告を消し、次の攻撃までの間を作ります。
    attackState_ = AttackState::kRecovery;
    const EnemyAttackDefinition& attack = GetCurrentAttackDefinition();
    attackStateDuration_ = (std::max)(0.18f, attack.recoveryDuration);
    attackTimer_ = attackStateDuration_;
    HideAttackTelegraph();
    SetVelocity({0.0f, GetVelocity().y, 0.0f});
    rushWingsVisual_.reset();
    dominionSigilVisual_.reset();

    // 炎を使った場合は各ポータルと陣を硬直開始時に破棄する。
    if (currentAttack_ == AttackKind::kUmbraFlame) {
        umbraFlames_.clear();
        umbraFlameNetwork_.reset();
    }

    // 回転刃を使った場合は軌道と着弾の表示部品を破棄する。
    if (currentAttack_ == AttackKind::kAstralReaver) {
        for (AstralReaverOrb& orb : astralReaverOrbs_) {
            orb.halo.reset();
            orb.blade.reset();
            orb.arc.reset();
            orb.core.reset();
        }

        for (auto& ring : astralReaverImpactRings_)
            ring.reset();
        astralReaverTargetRing_.reset();
        astralReaverImpactCore_.reset();
    }

    // 夜墜槍を使った場合は技専用の表示と経過状態を初期化する。詠唱はどの技でも終了する。
    if (currentAttack_ == AttackKind::kNightfallLances) {
        nightfallLances_ = {};
    }

    spellChant_.reset();
}

void EnemyFalseKingSlime::FinishAttack() {
    // 形態に応じた再使用待ちを設定し、攻撃選択・時間・発生番号を待機状態へ戻す。
    const EnemyAttackDefinition& attack = GetCurrentAttackDefinition();
    attackCooldown_ =
        (std::max)(0.35f, attack.cooldown * (battlePhase_ == 1 ? 1.0f : (battlePhase_ == 2 ? 0.88f : 0.78f)));
    currentAttack_ = AttackKind::kNone;
    attackState_ = AttackState::kIdle;
    attackTimer_ = 0.0f;
    actionIndex_ = 0;
    // 通常物理へ戻した後の接地はMotorに任せ、攻撃終了フレームの位置跳びを防ぐ。
}

void EnemyFalseKingSlime::UpdateSpellChant(float deltaTime) {
    // ボスの現在の寸法と向きから、詠唱 VFX の中心と進行度を組み立てます。
    if (!spellChant_)
        return;
    BossChantFrame frame;
    frame.casterPosition = GetWorldPosition();
    frame.groundPosition = FindGroundPoint(frame.casterPosition);
    const engine::Aabb bounds = GetModelWorldAabb();
    frame.casterRadius =
        (std::max)({(bounds.max.x - bounds.min.x) * 0.5f, (bounds.max.z - bounds.min.z) * 0.5f, baseScale_.x * 0.84f});
    frame.casterHeight = (std::max)(bounds.max.y - bounds.min.y, baseScale_.y * 1.2f);
    frame.sigilCenter = frame.casterPosition + lockedDirection_ * std::clamp(frame.casterRadius * 1.55f, 2.5f, 5.4f) +
                        engine::Vector3{0.0f, frame.casterHeight * 0.48f, 0.0f};
    frame.forward = lockedDirection_;
    if (battlePhase_ == 2 &&
        (debugPreviewAttackId_.empty() || debugPreviewAttackId_ == kSealBarragePreviewId)) {
        // 封印のムービーと実戦は別の時計を使います。紋章は最初に組み立て、操作復帰で描き直しません。
        const auto* scene = dynamic_cast<GamePlayScene*>(SceneManager::GetInstance()->GetCurrentScene());
        const float movieTime = scene && sealState_ == SealState::kReveal ? scene->GetSealRevealMovieTime() : 0.0f;
        frame.casting = movieTime >= SealRevealTiming::ChantStart || sealState_ == SealState::kChant;
        frame.windupProgress = sealState_ == SealState::kChant ? 1.0f :
            std::clamp((movieTime - SealRevealTiming::ChantStart) /
                           (SealRevealTiming::Launch - SealRevealTiming::ChantStart), 0.0f, 1.0f);

        // ムービーのカメラは敵の更新後に切り替わるので、カットで使う視点の方向を先に渡します。
        const auto* camera = CameraManager::GetInstance()->GetActiveCamera();
        const engine::Vector3 offset = movieTime > 0.0f ? SealRevealTiming::CasterShotOffset :
            (camera ? camera->GetEye() - frame.casterPosition : frame.forward);
        const engine::Vector3 towardView{offset.x, 0.0f, offset.z};
        // 視点への水平成分が十分ある場合だけ、正規化して紋章の向きに使う。
        if (engine::Math::Length(towardView) > .001f)
            frame.forward = engine::Math::Normalize(towardView);
        const float barrierRadius = CrownFinaleTuning::Get().sealBarrierRadius;
        frame.casterRadius = std::max(frame.casterRadius, barrierRadius * .88f);
        // 白い身体と紋章が重ならないよう、視点側から見て少し横と上に出します。
        const engine::Vector3 side{frame.forward.z, 0.0f, -frame.forward.x};
        frame.sigilCenter = frame.groundPosition + frame.forward * (barrierRadius + .55f) + side * (barrierRadius * .84f) +
                           engine::Vector3{0.0f, frame.casterHeight * .76f, 0.0f};
    } else {
        // 通常は三つの詠唱技だけ、予兆の進行率または実行開始からの経過秒を演出へ渡す。
        frame.casting = (currentAttack_ == AttackKind::kUmbraFlame || currentAttack_ == AttackKind::kAstralReaver ||
                         currentAttack_ == AttackKind::kNightfallLances) &&
                        attackState_ == AttackState::kWindup;
        frame.releasing = (currentAttack_ == AttackKind::kUmbraFlame || currentAttack_ == AttackKind::kAstralReaver ||
                           currentAttack_ == AttackKind::kNightfallLances) &&
                          attackState_ == AttackState::kActive;
        frame.windupProgress =
            frame.casting ? 1.0f - std::clamp(attackTimer_ / (std::max)(attackStateDuration_, 0.001f), 0.0f, 1.0f) : 1.0f;
        frame.activeElapsed = frame.releasing ? (std::max)(0.0f, attackStateDuration_ - attackTimer_) : 0.0f;
    }
    frame.time = idleTimer_;
    frame.deltaTime = deltaTime;
    spellChant_->Update(frame);
}

void EnemyFalseKingSlime::PrepareAttackCycle() {
    // 形態別の候補をシャッフルし、直前と同じ技が先頭に続くのを避けます。
    static constexpr std::array<AttackKind, 3> kPhaseOne = {AttackKind::kCrownLanceRain, AttackKind::kRoyalShockwave,
                                                           AttackKind::kKingRush};
    static constexpr std::array<AttackKind, 4> kPhaseTwo = {AttackKind::kRoyalCross, AttackKind::kCrownLanceRain,
                                                           AttackKind::kKingRush, AttackKind::kRoyalShockwave};
    static constexpr std::array<AttackKind, 8> kPhaseThree = {
        AttackKind::kCrownDominion,   AttackKind::kKingRush,       AttackKind::kUmbraFlame, AttackKind::kAstralReaver,
        AttackKind::kNightfallLances, AttackKind::kCrownLanceRain, AttackKind::kRoyalCross, AttackKind::kRoyalShockwave};

    // 段階ごとの攻撃順と件数は同じ配列から取得する。
    const auto setAttackCycle = [this](const auto& cycle) {
        attackCycleCount_ = cycle.size();
        std::copy(cycle.begin(), cycle.end(), attackCycle_.begin());
    };
    if (battlePhase_ <= 1) {
        setAttackCycle(kPhaseOne);
    } else if (battlePhase_ == 2) {
        setAttackCycle(kPhaseTwo);
    } else {
        setAttackCycle(kPhaseThree);
    }
    enemyAttackSelection::Shuffle(std::span{attackCycle_}.first(attackCycleCount_), lastAutomaticAttack_, attackRandom_);

    attackCycleIndex_ = 0;
    attackCyclePhase_ = battlePhase_;
}

EnemyFalseKingSlime::AttackKind EnemyFalseKingSlime::ResolveAutomaticAttack(float targetDistance) {
    // 残り候補の中から現在の射程で使える技を前へ寄せます。
    if (attackCyclePhase_ != battlePhase_ || attackCycleIndex_ >= attackCycleCount_) {
        PrepareAttackCycle();
    }

    return enemyAttackSelection::SelectInRange(
        std::span{attackCycle_}.first(attackCycleCount_), attackCycleIndex_, lastAutomaticAttack_,
        [&](AttackKind kind) {
            const auto& attack = GetAttackDefinition(GetAttackId(kind));
            return targetDistance >= attack.minRange && targetDistance <= attack.maxRange;
        });
}

// 設定されたIDを技へ解決し、対応表に無ければ自動選択に任せるため技なしを返す。
EnemyFalseKingSlime::AttackKind EnemyFalseKingSlime::ResolveDebugAttack() const {
    return enemyAttackSelection::FindKind(GetAttackEntries(), debugPreviewAttackId_, AttackKind::kNone);
}

std::span<const enemyAttackSelection::Entry<EnemyFalseKingSlime::AttackKind>>
EnemyFalseKingSlime::GetAttackEntries() const {
    // 本編のID参照とエディターの攻撃指定で同じ対応を使う。
    static constexpr enemyAttackSelection::Entry<AttackKind> kEntries[] = {
        {AttackKind::kCrownLanceRain, kCrownLanceRainAttackId},
        {AttackKind::kRoyalShockwave, kRoyalShockwaveAttackId},
        {AttackKind::kKingRush, kKingRushAttackId},
        {AttackKind::kRoyalCross, kRoyalCrossAttackId},
        {AttackKind::kCrownDominion, kCrownDominionAttackId},
        {AttackKind::kUmbraFlame, kUmbraFlameAttackId},
        {AttackKind::kAstralReaver, kAstralReaverAttackId},
        {AttackKind::kNightfallLances, kNightfallLancesAttackId},
    };
    return kEntries;
}

const char* EnemyFalseKingSlime::GetAttackId(AttackKind kind) const {
    // 対応表に無い種類は、王冠槍雨の定義を参照する既定IDへ解決する。
    return enemyAttackSelection::FindId(GetAttackEntries(), kind, kCrownLanceRainAttackId);
}

const EnemyAttackDefinition& EnemyFalseKingSlime::GetCurrentAttackDefinition() const {
    // 現在技のパラメータは攻撃プロファイルを参照します。
    return GetAttackDefinition(GetAttackId(currentAttack_));
}

std::int32_t EnemyFalseKingSlime::ResolveDesiredBattlePhase() const {
    // HP が閾値まで減った後は結晶戦へ、最終闘技場への移動後は第三形態へ進めます。
    const float maximumHp = GetEncounterMaximumHp();
    const float hpRate = maximumHp > 0.0f ? GetEncounterCurrentHp() / maximumHp : 1.0f;
    // 最終闘技場が既に始まった場合はHPに関係なく第三形態を要求する。
    if (finalArenaStarted_)
        return 3;
    // 移動前は残HPが約67%以下で第二形態、それより上は第一形態とする。
    if (hpRate <= 0.6701f)
        return 2;
    return 1;
}

void EnemyFalseKingSlime::BeginPhaseShift(std::int32_t nextPhase) {
    // 古い攻撃の予告と一時描画物を消してから形態移行演出を開始します。
    ResetLocomotion();
    pendingBattlePhase_ = (std::clamp)(nextPhase, 1, 3);
    attackState_ = AttackState::kPhaseShift;
    currentAttack_ = AttackKind::kNone;
    phaseTransitionTimer_ = 0.0f;
    phaseShiftStartPosition_ = GetTranslate();
    attackTimer_ = 0.0f;
    ClearTransientVisuals();
    HideAttackTelegraph();
    SetVelocity({0.0f, 0.0f, 0.0f});
    // 本番で第二形態に入るとき、現在がゲームプレイシーンなら結晶召喚のカメラ演出を依頼する。
    if (pendingBattlePhase_ == 2 && debugPreviewAttackId_.empty()) {
        if (auto* scene = dynamic_cast<GamePlayScene*>(SceneManager::GetInstance()->GetCurrentScene())) {
            const auto center = FindGroundPoint(authoredPosition_ +
                engine::Vector3{0, 0, CrownFinaleTuning::Get().sealCenterOffsetZ});
            scene->StartSealRevealMovie(this, center);
        }
    }
    VfxSequencer::PlayOneShot(kPhaseShiftSequence, FindGroundPoint(GetTranslate()) + engine::Vector3{0.0f, 0.15f, 0.0f},
                              {2.1f + static_cast<float>(pendingBattlePhase_) * 0.25f,
                               2.1f + static_cast<float>(pendingBattlePhase_) * 0.25f,
                               2.1f + static_cast<float>(pendingBattlePhase_) * 0.25f});
}

void EnemyFalseKingSlime::UpdatePhaseShift(float deltaTime, engine::Vector3& velocity) {
    // 第二形態では結晶の中心へ先に移動し、移行完了後に結晶を生成します。
    phaseTransitionTimer_ += deltaTime;
    velocity = {0.0f, 0.0f, 0.0f};
    const float progress = GetPhaseTransitionProgress();

    if (pendingBattlePhase_ == 2) {
        // 形態変化の間に舞台中央へ移動し、結晶召喚より先に立ち位置を確定させます。
        const engine::Vector3 center = FindGroundPoint(
            authoredPosition_ + engine::Vector3{0.0f, 0.0f, CrownFinaleTuning::Get().sealCenterOffsetZ});
        SetTranslate(LerpVector3(phaseShiftStartPosition_, center, SmoothStep01(progress)));
        scriptedMovementThisFrame_ = true;
    }

    effectTimer_ -= deltaTime;

    // 移行中の粒子は発生間隔を満たしたときだけ出し、形態と進行に応じて飛散量を決める。
    if (effectTimer_ <= 0.0f) {
        const float angle = idleTimer_ * (4.0f + static_cast<float>(pendingBattlePhase_));
        const engine::Vector3 direction = {std::sin(angle) * 0.45f, 1.0f, std::cos(angle) * 0.45f};
        EmitDirectedPreset(kDominionPreset, GetWorldPosition() + engine::Vector3{0.0f, 1.4f, 0.0f}, direction,
                           1.0f + progress * 0.9f);
        effectTimer_ += 0.14f;
    }

    // 移行時間が過ぎたら形態を確定し、本番の第二形態では結晶を初期化してから接地待機へ戻る。
    if (phaseTransitionTimer_ >= phaseTransitionDuration_) {
        battlePhase_ = pendingBattlePhase_;
        if (battlePhase_ == 2 && debugPreviewAttackId_.empty())
            InitializeCrownSeals();
        attackState_ = AttackState::kIdle;
        phaseTransitionTimer_ = phaseTransitionDuration_;
        attackCooldown_ = 0.72f;
        SetTranslate(FindGroundPoint(GetTranslate()));
        SetGrounded(true);
    }
}

void EnemyFalseKingSlime::UpdateFacing(const engine::Vector3& direction, float follow) {
    // モデル固有の前方オフセットを足し、最短角度で目標へ向けます。
    // 水平成分が小さすぎる入力では、現在の方位を維持する。
    if (direction.x * direction.x + direction.z * direction.z <= 0.0001f) {
        return;
    }

    const float targetYaw = std::atan2(direction.x, direction.z) + kModelYawOffset;
    SetRotationY(engine::Math::LerpShortAngle(GetRotation().y, targetYaw, (std::clamp)(follow, 0.0f, 1.0f)));
}

void EnemyFalseKingSlime::ApplyBossAnimation(float deltaTime) {
    // 共通の跳ね姿勢を基準に、予兆・攻撃・形態移行の伸縮を重ねます。
    slimeBounceAnimator::Params params;
    params.speedForFullBounce = 2.8f;
    params.idleAmplitude = 0.050f;
    params.moveAmplitude = 0.17f;
    params.hopFrequency = 3.2f + static_cast<float>(battlePhase_) * 0.45f;
    params.horizontalSquash = 0.18f;
    params.verticalStretch = 0.23f;
    params.airborneStretch = 0.33f;

    engine::Vector3 targetScale = slimeBounceAnimator::MakeScale(baseScale_, GetVelocity(), idleTimer_, IsGrounded(), params);
    engine::Vector3 targetRotation = {authoredRotation_.x, GetRotation().y, authoredRotation_.z};

    if (attackState_ == AttackState::kIdle && locomotionPhase_ != LocomotionPhase::kNone) {
        // 移動姿勢は開始・移動・着地の進行率で作り、経過時間だけの跳ねを重ねません。
        if (locomotionPhase_ == LocomotionPhase::kWindup) {
            const float duration = locomotionLeap_ ? 0.16f : 0.12f;
            const float brace = SmoothStep01(locomotionTimer_ / duration);
            const float squash = brace * (locomotionLeap_ ? 0.20f : 0.07f);
            targetScale = {baseScale_.x * (1.0f + squash * 0.55f), baseScale_.y * (1.0f - squash),
                           baseScale_.z * (1.0f + squash * 0.55f)};
            targetRotation.x -= brace * (locomotionLeap_ ? 0.075f : 0.025f);
        } else if (locomotionPhase_ == LocomotionPhase::kTravel) {
            const float progress = std::clamp(locomotionTimer_ / locomotionTravelDuration_, 0.0f, 1.0f);
            const float arc = std::sin(progress * kPi);
            // 跳躍中は縦に伸ばし、横移動中はローカルの横向きに応じて傾ける。
            if (locomotionLeap_) {
                targetScale = {baseScale_.x * 0.96f, baseScale_.y * (1.08f + arc * 0.07f), baseScale_.z * 0.96f};
                targetRotation.x -= arc * 0.08f;
            } else {
                const float yaw = GetRotation().y - kModelYawOffset;
                const float side = locomotionDirection_.x * std::cos(yaw) - locomotionDirection_.z * std::sin(yaw);
                targetScale = {baseScale_.x * (1.0f + arc * 0.035f), baseScale_.y,
                               baseScale_.z * (1.0f - arc * 0.02f)};
                targetRotation.z -= side * arc * 0.10f;
            }
        } else {
            const float duration = locomotionLeap_ ? 0.22f : 0.17f;
            const float settle = 1.0f - SmoothStep01(locomotionTimer_ / duration);
            const float squash = settle * (locomotionLeap_ ? 0.24f : 0.08f);
            targetScale = {baseScale_.x * (1.0f + squash * 0.75f), baseScale_.y * (1.0f - squash),
                           baseScale_.z * (1.0f + squash * 0.75f)};
        }
    } else if (attackState_ == AttackState::kIdle && IsGrounded()) {
        // 移動距離に合わせて接地した体を伸縮させ、停止中は小さな呼吸だけを残す。
        const float speedRate = std::clamp(slimeBounceAnimator::PlanarSpeed(GetVelocity()) / 3.4f, 0.0f, 1.0f);
        const float wave = std::sin(travelPhase_);
        const float squash = (std::max)(0.0f, -wave) * speedRate;
        const float stretch = (std::max)(0.0f, wave) * speedRate;
        const float breath = std::sin(idleTimer_ * 2.2f) * 0.018f;
        targetScale = {
            baseScale_.x * (1.0f + breath * 0.5f + squash * 0.12f - stretch * 0.045f),
            // 通常移動では高さを固定し、上下動は実際の跳躍と攻撃動作だけに任せます。
            baseScale_.y,
            baseScale_.z * (1.0f + breath * 0.5f + squash * 0.10f - stretch * 0.04f),
        };
    }

    // 予兆中は体を低く広げ、攻撃が近づくほど左右の震えを大きくする。
    if (attackState_ == AttackState::kWindup) {
        const float progress = SmoothStep01(1.0f - attackTimer_ / (std::max)(attackStateDuration_, 0.01f));
        const float tremble = std::sin(progress * kPi * 18.0f) * progress * 0.045f;
        targetScale = {
            baseScale_.x * (1.0f + progress * 0.35f + tremble),
            baseScale_.y * (1.0f - progress * 0.32f),
            baseScale_.z * (1.0f + progress * 0.35f - tremble),
        };

        targetRotation.z += tremble * 1.7f;
    } else if (attackState_ == AttackState::kActive) {
        const float progress = 1.0f - attackTimer_ / (std::max)(attackStateDuration_, 0.01f);

        // 衝撃波は空中で体を縦に伸ばし、着地したら横に潰れてから戻す。
        if (currentAttack_ == AttackKind::kRoyalShockwave) {
            const float jumpProgress =
                std::clamp(shockwaveJumpTimer_ / (std::max)(shockwaveJumpDuration_, 0.01f), 0.0f, 1.0f);
            if (!shockwaveLanded_) {
                const float airStretch = std::sin(jumpProgress * kPi);
                targetScale = {
                    baseScale_.x * (0.92f - airStretch * 0.08f),
                    baseScale_.y * (1.18f + airStretch * 0.28f),
                    baseScale_.z * (0.92f - airStretch * 0.08f),
                };

                targetRotation.x -= std::sin(jumpProgress * kPi) * 0.12f;
            } else {
                const float landingAge = (std::max)(0.0f, shockwaveJumpTimer_ - shockwaveJumpDuration_);
                const float settle = std::clamp(landingAge / 0.42f, 0.0f, 1.0f);
                const float rebound = std::sin(settle * kPi * 2.0f) * (1.0f - settle) * 0.12f;
                targetScale = {
                    baseScale_.x * (1.42f - settle * 0.24f + rebound),
                    baseScale_.y * (0.54f + settle * 0.26f - rebound * 0.55f),
                    baseScale_.z * (1.42f - settle * 0.24f - rebound),
                };
            }
        } else if (currentAttack_ == AttackKind::kKingRush) {
            // 突進は体を低く前後に伸ばし、小刻みに揺らして勢いを見せる。
            const float pulse = std::sin(progress * kPi * 14.0f) * 0.04f;
            targetScale = {
                baseScale_.x * (1.20f + pulse),
                baseScale_.y * 0.70f,
                baseScale_.z * (1.48f - pulse),
            };

            targetRotation.x -= 0.13f;
        } else {
            // それ以外の発動中は詠唱の反動で体を揺らす。
            const float castPulse = std::sin(progress * kPi * 10.0f);
            targetScale = {
                baseScale_.x * (1.18f + std::abs(castPulse) * 0.08f),
                baseScale_.y * (0.80f - castPulse * 0.07f),
                baseScale_.z * (1.18f + std::abs(castPulse) * 0.08f),
            };

            targetRotation.z += castPulse * 0.055f;
        }
    } else if (attackState_ == AttackState::kPhaseShift) {
        // 形態移行中は途中で一度大きく伸ばし、完了に向けて基準倍率へ収束させる。
        const float progress = GetPhaseTransitionProgress();
        const float surge = std::sin(progress * kPi);
        const float tremble = std::sin(progress * kPi * 24.0f) * surge * 0.05f;
        targetScale = {
            baseScale_.x * (1.0f + surge * 0.34f + tremble),
            baseScale_.y * (1.0f + surge * 0.42f),
            baseScale_.z * (1.0f + surge * 0.34f - tremble),
        };

        targetRotation.z += tremble * 1.8f;
    } else if (attackState_ == AttackState::kRecovery) {
        // 硬直中の揺れは残り時間で弱め、次の待機姿勢へつなげる。
        const float remaining = std::clamp(attackTimer_ / (std::max)(attackStateDuration_, 0.01f), 0.0f, 1.0f);
        const float bounce = std::sin((1.0f - remaining) * kPi * 2.6f) * remaining;
        targetScale = {
            baseScale_.x * (1.0f + bounce * 0.20f),
            baseScale_.y * (1.0f - bounce * 0.27f),
            baseScale_.z * (1.0f + bounce * 0.20f),
        };
    }

    // 攻撃中は動作の姿勢を優先し、待機時だけ通常移動に応じる追加姿勢を合成する。
    if (attackState_ == AttackState::kIdle) {
        slimeBounceAnimator::ApplyTravelPose(targetScale, targetRotation, baseScale_, GetVelocity(),
                                             GetRotation().y - kModelYawOffset, 4.2f, 0.045f, 0.060f);
    }

    // 被弾姿勢を最終目標へ重ね、進行方向のY回転には触れずに補間する。
    ApplyDamageReactionPose(targetScale, targetRotation);
    const float follow = 1.0f - std::exp(-deltaTime * 11.0f);
    const engine::Vector3 currentScale = GetScale();
    SetScale({
        engine::Math::Lerp(currentScale.x, targetScale.x, follow),
        engine::Math::Lerp(currentScale.y, targetScale.y, follow),
        engine::Math::Lerp(currentScale.z, targetScale.z, follow),
    });
    const engine::Vector3 currentRotation = GetRotation();
    SetRotation({
        engine::Math::Lerp(currentRotation.x, targetRotation.x, follow),
        currentRotation.y,
        engine::Math::Lerp(currentRotation.z, targetRotation.z, follow),
    });
}

void EnemyFalseKingSlime::UpdateRoyalGlow(float deltaTime) {
    // 形態色と装甲開放状態を発光へ反映し、攻撃中だけ周囲の粒を放出します。
    ambientSparkTimer_ -= deltaTime;
    const bool attacking = attackState_ == AttackState::kWindup || attackState_ == AttackState::kActive ||
                           attackState_ == AttackState::kPhaseShift;
    const float pulse = 0.5f + std::sin(idleTimer_ * (attacking ? 8.2f : 3.4f)) * 0.5f;
    engine::Vector4 phaseColor = authoredColor_;

    // 第二形態は白紫、第三形態は装甲の開放中だけ金色、閉じている間は紫にする。
    if (battlePhase_ == 2) {
        phaseColor = {1.0f, 0.90f, 1.0f, authoredColor_.w};
    } else if (battlePhase_ >= 3) {
        phaseColor = armorOpenTimer_ > 0.0f ? engine::Vector4{1.0f, 0.92f, 0.50f, authoredColor_.w}
                                            : engine::Vector4{0.74f, 0.58f, 1.0f, authoredColor_.w};
    }

    SetDefaultColor(phaseColor);
    SetColor(phaseColor);
    SetEmissive(authoredEmissive_ + static_cast<float>(battlePhase_ - 1) * 0.34f + (attacking ? 0.52f : 0.0f) +
                pulse * 0.12f);
    // 攻撃・形態移行以外では粒子の時計をリセットし、次の開始ですぐ粒子を出せるようにする。
    if (!attacking) {
        ambientSparkTimer_ = 0.0f;
        return;
    }

    // 次の粒子を出せる時刻になったら、第三形態の領域用と通常の溜め用を使い分ける。
    if (ambientSparkTimer_ <= 0.0f) {
        EmitDirectedPreset(battlePhase_ >= 3 ? kDominionPreset : kChargePreset,
                           GetWorldPosition() + engine::Vector3{0.0f, baseScale_.y * 1.28f, 0.0f}, {0.0f, 1.0f, 0.0f}, 1.12f);
        ambientSparkTimer_ += battlePhase_ >= 3 ? 0.14f : 0.18f;
    }
}

void EnemyFalseKingSlime::EmitPreset(const char* presetName, const engine::Vector3& position) const {
    // 粒子管理が未初期化のプレビューでは放出を省略します。
    GpuParticleManager* particles = GpuParticleManager::GetInstance();

    if (particles && particles->IsInitialized() && presetName && presetName[0] != '\0') {
        particles->Emit(presetName, position);
    }
}

void EnemyFalseKingSlime::EmitDirectedPreset(const char* presetName, const engine::Vector3& position, const engine::Vector3& direction,
                                             float speedScale) const {
    // 方向付き粒子も管理側が使える時だけ発行します。
    GpuParticleManager* particles = GpuParticleManager::GetInstance();

    if (particles && particles->IsInitialized() && presetName && presetName[0] != '\0') {
        particles->EmitDirected(presetName, position, direction, speedScale);
    }
}

void EnemyFalseKingSlime::ClearTransientVisuals() {
    // 技を中断した後も攻撃物が残らないよう、所有演出とタイマーをまとめて破棄します。
    crownLances_.clear();
    lanceSpawnSerial_ = 0;
    royalWaves_.clear();
    royalBeams_.clear();
    umbraFlames_.clear();
    umbraFlameNetwork_.reset();
    spellChant_.reset();

    for (AstralReaverOrb& orb : astralReaverOrbs_) {
        orb.halo.reset();
        orb.blade.reset();
        orb.arc.reset();
        orb.core.reset();
    }

    for (auto& ring : astralReaverImpactRings_)
        ring.reset();
    astralReaverTargetRing_.reset();
    astralReaverImpactCore_.reset();
    astralReaverElapsed_ = 0.0f;
    astralReaverImpactAge_ = -1.0f;
    astralReaverDamageApplied_ = false;
    nightfallLances_ = {};
    sealNightfallSatellites_ = {};
    shockwaveChargeVisual_.reset();
    rushWingsVisual_.reset();
    dominionSigilVisual_.reset();
    shockwaveJumpTimer_ = 0.0f;
    shockwaveLanded_ = false;
}

void EnemyFalseKingSlime::DrawOwnedSpecialMaterialVisualsForCamera(engine::Camera* camera, uint32_t depthSrvHandle,
                                                                   uint32_t grabSrvHandle, std::int32_t previewBufferIndex) {
    // 指定カメラ用に特殊材質の所有物を描き、別視点プレビューへも対応します。
    // カメラがあればオーラをその視点へ向けて描く。描画直後に通常カメラの向きへ戻す。
    if (camera) {
        AlignBossAura(camera);
        for (auto* visual : {introPortal_.get(), bossAura_.get(), introShockwave_.get()})
            if (visual && visual->GetIsVisible())
                visual->DrawSpecialMaterialForCamera(camera, depthSrvHandle, grabSrvHandle, previewBufferIndex);
        // プレビューの向きを通常描画へ持ち越さないようにします。
        AlignBossAura(CameraManager::GetInstance()->GetActiveCamera());
    }
    if (!camera) {
        return;
    }

    // 戦闘演出の各部品も、生成済みで可視のものだけを指定カメラへ描く。
    if (crownShieldBarrier_ && crownShieldBarrier_->GetIsVisible()) {
        crownShieldBarrier_->DrawSpecialMaterialForCamera(camera, depthSrvHandle, grabSrvHandle, previewBufferIndex);
    }

    for (const CrownLance& lance : crownLances_) {
        for (engine::Object3D* visual : {lance.portal.get(), lance.aura.get()}) {
            if (visual && visual->GetIsVisible()) {
                visual->DrawSpecialMaterialForCamera(camera, depthSrvHandle, grabSrvHandle, previewBufferIndex);
            }
        }
    }

    if (shockwaveChargeVisual_ && shockwaveChargeVisual_->GetIsVisible()) {
        shockwaveChargeVisual_->DrawSpecialMaterialForCamera(camera, depthSrvHandle, grabSrvHandle, previewBufferIndex);
    }

    for (const RoyalBeam& beam : royalBeams_) {
        if (beam.glow && beam.glow->GetIsVisible()) {
            beam.glow->DrawSpecialMaterialForCamera(camera, depthSrvHandle, grabSrvHandle, previewBufferIndex);
        }
    }

    for (const UmbraFlamePortal& portal : umbraFlames_) {
        if (portal.flame && portal.flame->GetIsVisible()) {
            portal.flame->DrawSpecialMaterialForCamera(camera, depthSrvHandle, grabSrvHandle, previewBufferIndex);
        }
    }
}
