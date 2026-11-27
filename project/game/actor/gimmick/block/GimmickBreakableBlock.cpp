#include <cstdint>
#include "GimmickBreakableBlock.h"
#include "ObjectPropertyDefinition.h"
#include "CollisionConfig.h"
#include "EnemyBomb.h"
#include "Player.h"
#include <DebrisEffectManager.h>
#include <MeshEffectManager.h>
#include <GpuParticleManager.h>
#include <DebugConsole.h>
#include <cmath>

void GimmickBreakableBlock::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    BaseGimmick::Initialize(common, modelName);

    // 専用ブロックのテクスチャと材質はモデルに対応する場合だけ上書きします。
    if (modelName == "Stages/bomb_break_block") {
        SetTexture("Resources/3DModel/Stages/bomb_break_block/bomb_break_block_albedo.png");
        SetEnableNormalMap(true);
        SetNormalMap("Resources/3DModel/Stages/bomb_break_block/bomb_break_block_normal.png");
        SetOrmMap("Resources/3DModel/Stages/bomb_break_block/bomb_break_block_orm.png");
        SetMaterialType(0);
        SetColor({1.0f, 1.0f, 1.0f, 1.0f});
        SetMetallic(0.0f);
        SetRoughness(0.72f);
        SetEnableEnvMap(false);
        SetEmissive(1.0f);
    }

    // 地面属性と標準のゲームプレイ用マスクで足場にする。
    SetCollisionAttribute(engine::collisionCategories::Get("ground"));
    SetCollisionMask(engine::collisionCategories::GetDefaultGameplayMask());

    // ブロックの接触は回転を持てるOBBとして扱う。
    SetColliderType(engine::ColliderType::kObb);

    isBroken_ = false;
    SetClassName("Gimmick");
    SetGimmickType("BreakableBlock");

    // 配置するブロックごとに、どの衝撃を破壊条件にするか選べるようにする。
    using Self = GimmickBreakableBlock;
    engine::ObjectPropertyDefinition<Self> properties(*this, kTypeId, "破壊ブロック",
        "爆風や体当たりで壊れる条件を指定します。");
    using Parameter = engine::Object3D::EntityParameter;
    properties.Parameter("破壊条件", "actionMode", &Parameter::actionMode, 0.0f, static_cast<float>(kDashPanelOnly),
        nullptr, {"爆風のみ", "汎用衝撃でも破壊", "ピンクのバウンド落下でも破壊", "ダッシュパネル加速中のみ"},
        {.values = {0, 6, 7, kDashPanelOnly}});
}

void GimmickBreakableBlock::Update(float deltaTime) {
    // 破壊済みの物体には通常の更新を行わず、表示・衝突を外した死亡状態を保つ。
    if (isBroken_) {
        SetCollisionAttribute(0);
        SetCollisionMask(0);
        SetIsVisible(false);
        SetDead(true);
        return;
    }

    BaseGimmick::Update(deltaTime);
}

bool GimmickBreakableBlock::OnCollision(engine::Object3D* other) {
    // 既に破壊済みなら、同じ接触で破壊を重ねない。
    if (isBroken_)
        return false;

    const auto& parameters = GetParameters();
    // 加速パネル専用の設定では、爆風経路からの破壊を判定しない。
    if (parameters && parameters->actionMode == kDashPanelOnly) {
        return BaseGimmick::OnCollision(other);
    }

    const uint32_t attribute = other->GetCollisionAttribute();
    const auto* bomb = dynamic_cast<const EnemyBomb*>(other);

    // プレイヤー所有の爆弾が起爆済みで、攻撃属性の球判定になった場合だけ爆風で壊す。
    if (bomb && bomb->IsPlayerOwned() && bomb->IsExploded() &&
        (attribute & engine::collisionCategories::Get("player_attack")) &&
        other->GetColliderType() == engine::ColliderType::kSphere) {

        Break();
        return true;
    }

    // 破壊条件を満たさない接触は、基底の衝突通知へ渡す。
    return BaseGimmick::OnCollision(other);
}

bool GimmickBreakableBlock::TryBreakByPlayerImpact(const Player* player) {
    // 破壊済み、プレイヤー不在、設定未登録の場合は、接触を理由とする破壊を行わない。
    if (isBroken_ || !player || !GetParameters().has_value()) {
        return false;
    }

    // 配置時の破壊条件ごとにプレイヤーのどの技を有効とするか判定します。爆風は OnCollision() が確認します。
    const std::int32_t actionMode = GetParameters()->actionMode;

    // パネル加速限定では、接触時の加速状態を確認する。
    if (actionMode == kDashPanelOnly) {
        // パネルの加速状態を確認し、通常の移動や別の技による衝突は受け付けません。
        if (!player->IsDashPanelBoostActive()) {
            return false;
        }
    // ピンクの着地衝撃に対応する設定では、能力側の着地衝撃フラグを確認する。
    } else if (actionMode == 7) {
        // ピンクのバウンドで地面へ向かう衝撃だけを受け付けます。
        if (!player->IsPinkBounceSlamImpactActive()) {
            return false;
        }
    // 汎用衝撃に対応する設定では、プレイヤー側の破壊フラグを確認する。
    } else if (actionMode == 6) {
        // プレイヤー側が汎用の破壊フラグを有効にしている間だけ受け付けます。
        if (!player->IsImpactBreakActive()) {
            return false;
        }
    // ほかの設定ではプレイヤー本体に当たっても足場のまま扱います。
    } else {
        return false;
    }

    Break();
    return true;
}

// 同じフレームの後続判定で通過できるよう衝突を外し、材質に応じた破片を出します。
void GimmickBreakableBlock::Break() {
    isBroken_ = true;

    // 突進のSphereCastと同じフレームで通過できるよう、破壊時点で判定を外す。
    SetCollisionAttribute(0);
    SetCollisionMask(0);
    SetIsVisible(false);
    SetDead(true);

    // 親の地形を含む座標から破片と閃光を出し、ローカル原点へのずれを防ぎます。
    engine::Vector3 myPos = GetWorldPosition();

    // 屈折マテリアルのガラスは、炎を残さず透明破片と短い閃光で割れた瞬間を見せる。
    if (GetMaterialType() == 10 || GetModelName() == "Stages/star_garden_glass_panel") {
        const bool isGiantGate = GetModelName() == "Stages/star_garden_giant_gate";

        // 巨大な門だけは破片の着地高さをモデルの下端に合わせます。
        if (isGiantGate) {
            const engine::Aabb bounds = GetModelWorldAabb();
            const float groundY = std::isfinite(bounds.min.y) ? bounds.min.y : myPos.y - 4.2f;
            DebrisEffectManager::GetInstance()->SpawnOnGround("shared/star_garden_gate_glass_shatter", myPos, groundY);
        // 巨大な門以外は、通常サイズのガラス片をワールド位置から出す。
        } else {
            DebrisEffectManager::GetInstance()->Spawn("shared/star_garden_glass_shatter", myPos);
        }

        // GPU粒子を呼べる場合は、ガラス片に閃光と飛散を重ねる。
        if (GpuParticleManager::GetInstance()) {
            GpuParticleManager::GetInstance()->Emit("shared/star_garden_glass_shards", myPos);
            GpuParticleManager::GetInstance()->Emit("shared/hit_bomb_flash_core", myPos);
        }

        // ガラスの演出が終わったら戻り、石ブロック用の爆発を重ねません。
        return;
    }

    // 石の破壊では、メッシュ演出を呼べる場合に小規模な爆発を出す。
    if (MeshEffectManager::GetInstance()) {
        MeshEffectManager::GetInstance()->SpawnEffectAt("Resources/json/effect/shared/effect_bakuhatu.json", myPos,
                                                        {0.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f});
    }

    DebrisEffectManager::GetInstance()->Spawn("shared/bomb_hit_fragment_burst", myPos);

    // GPU粒子を呼べる場合は、石片に火花を重ねる。
    if (GpuParticleManager::GetInstance()) {
        GpuParticleManager::GetInstance()->Emit("shared/star_sparkleGet", myPos);
    }
}

std::unique_ptr<engine::Object3D> GimmickBreakableBlock::Clone() const {
    // 同じモデルで新しい実体を初期化してから、配置と破壊設定を複製する。
    auto newObj = std::make_unique<GimmickBreakableBlock>();
    assert(GetObjectCommon() != nullptr);
    newObj->Initialize(GetObjectCommon(), GetModelName());
    newObj->CopyFrom(this);
    return newObj;
}
