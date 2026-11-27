#define NOMINMAX
#include "GimmickMagmaHazard.h"
#include "ObjectPropertyDefinition.h"

#include "CollisionConfig.h"
#include "game/system/rules/EventManager.h"
#include "Player.h"
#include "SceneManager.h"

#include <algorithm>
#include <cmath>

// 静的なマグマ面の表示と、プレイヤー用のトリガー範囲を初期化します。
void GimmickMagmaHazard::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    BaseGimmick::Initialize(common, modelName);

    SetClassName("Gimmick");
    SetGimmickType("MagmaHazard");
    SetName("Gimmick_MagmaHazard");
    SetMaterialType(9);
    SetColor({1.0f, 0.24f, 0.015f, 1.0f});
    SetEmissive(1.8f);
    SetRoughness(0.38f);
    SetMetallic(0.0f);
    SetCollisionAttribute(engine::collisionCategories::Get("trigger"));
    SetCollisionMask(engine::collisionCategories::Get("player"));

    // マグマ面は移動しないため、巨大Colliderを静的グリッドへ一度だけ登録する。
    SetStatic(true);

    engine::Object3D::ColliderConfig collider;
    collider.type = engine::ColliderType::kObb;
    collider.center = {0.0f, 0.0f, 0.0f};
    collider.size = {1.0f, 1.0f, 1.0f};
    SetColliderConfig(collider);

    // 配置データがまだ無ければ、共有の設定欄を生成する。
    if (!GetParameters().has_value()) {
        ResetParameters();
    }

    SetParameter(&engine::EntityParameter::speed, 12.0f);
    SetParameter(&engine::EntityParameter::interval, 0.8f);

    // 既存の配置・編集との互換用に speed と interval を公開する。実際に液面に触れた判定は Player 側で行う。
    using Self = GimmickMagmaHazard;
    engine::ObjectPropertyDefinition<Self> properties(*this, kTypeId, "マグマダメージ床",
        "触れたプレイヤーを炎上させ、上方向へ弾き返します。");
    using Parameter = engine::Object3D::EntityParameter;
    properties.Parameter("接触ダメージ", "speed", &Parameter::speed, 0.0f, 100.0f);
    properties.Parameter("ダメージ間隔", "interval", &Parameter::interval, 0.1f, 10.0f);
}

void GimmickMagmaHazard::Update(float deltaTime) {
    // 配置データがない場合も既定値で危険域を更新できるようにする。
    if (!GetParameters().has_value()) {
        ResetParameters();
    }

    const bool isPlaying = SceneManager::GetInstance() && SceneManager::GetInstance()->IsPlaying();

    // 編集中は発光を元に戻し、次のプレイで実行時の時計を持ち越さないようにします。
    if (!isPlaying) {
        initializedForPlay_ = false;
        damageCooldownTimer_ = 0.0f;
        SetEmissive(baseEmissive_);
        BaseGimmick::Update(deltaTime);
        return;
    }

    // プレイ開始時のみ現在の発光を記録し、明滅の基準にします。
    if (!initializedForPlay_) {
        baseEmissive_ = (std::max)(0.1f, GetEmissive());
        visualTimer_ = 0.0f;
        damageCooldownTimer_ = 0.0f;
        initializedForPlay_ = true;
    }

    // 旧設定と同じクールダウン値を減らし、基準発光にはゆっくり変わる周期を掛ける。
    damageCooldownTimer_ = (std::max)(0.0f, damageCooldownTimer_ - deltaTime);
    visualTimer_ += deltaTime;
    const float pulse = 0.92f + std::sin(visualTimer_ * 2.6f) * 0.08f;
    SetEmissive(baseEmissive_ * pulse);

    BaseGimmick::Update(deltaTime);
}

bool GimmickMagmaHazard::OnCollision(engine::Object3D* /*other*/) {
    // 液面の波を含めた死亡判定はPlayer側が担当する。ここでは接触を受け取ったことだけ返す。
    return true;
}

float GimmickMagmaHazard::GetDamage() const {
    // 共通パラメーターのspeedを0以上のダメージ設定値として返す。設定が無ければ12。
    return GetParameters().has_value() ? (std::max)(0.0f, GetParameters()->speed) : 12.0f;
}

float GimmickMagmaHazard::GetDamageInterval() const {
    // 共通パラメーターのintervalを0.1秒以上の設定値として返す。設定が無ければ0.8秒。
    return GetParameters().has_value() ? (std::max)(0.1f, GetParameters()->interval) : 0.8f;
}

std::unique_ptr<engine::Object3D> GimmickMagmaHazard::Clone() const {
    // 複製先を初期化してから危険域の設定と配置を移す。
    auto clone = std::make_unique<GimmickMagmaHazard>();
    assert(GetObjectCommon() != nullptr);
    clone->Initialize(GetObjectCommon(), GetModelName());
    clone->CopyFrom(this);
    return clone;
}
