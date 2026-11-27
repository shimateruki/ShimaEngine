#define NOMINMAX
#include "GimmickContactHazard.h"

#include "CollisionConfig.h"
#include "Event.h"
#include "ObjectPropertyDefinition.h"
#include "Player.h"
#include "SceneManager.h"
#include "game/system/rules/EventManager.h"

#include <algorithm>
#include <cmath>

// 接触開始でプレイヤーに反応する固定トリガーを作ります。
void GimmickContactHazard::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    BaseGimmick::Initialize(common, modelName);

    SetClassName("Gimmick");
    SetGimmickType(kTypeId);
    SetName("Gimmick_ContactHazard");
    SetStatic(true);
    // 固定された障害物として扱うが、反応する相手はプレイヤーだけにする。
    SetCollisionAttribute(engine::collisionCategories::Get("trigger"));
    SetCollisionMask(engine::collisionCategories::Get("player"));

    // 配置時の向きを使えるように、回転付きの箱を初期の接触範囲にする。
    engine::Object3D::ColliderConfig collider;
    collider.type = engine::ColliderType::kObb;
    collider.center = {0.0f, 0.0f, 0.0f};
    collider.size = {4.32f, 0.91f, 0.91f};
    SetColliderConfig(collider);

    // 共通パラメーターが無い場合は、ダメージと押し返しを入れる容器を作る。
    if (!GetParameters())
        ResetParameters();

    // 接触時のダメージと押し返しを既定値に戻し、同じ値をInspectorから調整できるよう登録する。
    using Parameter = engine::EntityParameter;
    SetParameter(&Parameter::attackPower, 10.0f);
    SetParameter(&Parameter::moveAmount, 4.0f);

    engine::ObjectPropertyDefinition<GimmickContactHazard> properties(
        *this, kTypeId, "接触ダメージ障害物", "触れ始めに一度だけダメージを与え、軽く弾き返します。");
    properties.Parameter("接触ダメージ", "attackPower", &Parameter::attackPower, 0.0f, 100.0f);
    properties.Parameter("押し返す速さ", "moveAmount", &Parameter::moveAmount, 0.0f, 12.0f);
}

void GimmickContactHazard::OnTriggerEnter(const engine::CollisionEvent& event) {
    auto* player = dynamic_cast<Player*>(event.other);
    auto* scenes = SceneManager::GetInstance();
    // 接触したのが生存プレイヤーで、シーンが実行中の場合だけダメージを検討する。
    if (!player || player->IsDead() || !scenes || !scenes->IsPlaying())
        return;

    const auto& parameters = GetParameters();
    // 設定がない、ダメージが非有限、または0以下なら、ダメージイベントを送らない。
    if (!parameters || !std::isfinite(parameters->attackPower) || parameters->attackPower <= 0.0f)
        return;

    // 進入方向を反転し、ダッシュ走路で横に大きく飛ばされないようにする。
    const auto velocity = player->GetVelocity();
    const auto offset = player->GetWorldPosition() - GetWorldPosition();
    engine::Vector3 away{-velocity.x, 0.0f, -velocity.z};
    float length = std::hypot(away.x, away.z);
    const float outward = away.x * offset.x + away.z * offset.z;
    // 反転した速度が非有限・ほぼゼロ・内向きのいずれかなら、障害物から相手への水平差を代わりに使う。
    if (!std::isfinite(length) || length < 0.01f || outward <= 0.0f) {
        away = {offset.x, 0.0f, offset.z};
        length = std::hypot(away.x, away.z);
    }
    // 水平の押し返し方向に十分な長さがあれば、速度の強さと分離するため単位化する。
    if (std::isfinite(length) && length >= 0.01f)
        away = away * (1.0f / length);
    // 代替でも水平の向きを決められなければ、水平には押し返さない。
    else
        away = {0.0f, 0.0f, 0.0f};

    // 押し返しの水平速度を Inspector の許容範囲に収め、わずかな上向きの速度を別に加えます。
    const float recoil = std::isfinite(parameters->moveAmount)
                             ? std::clamp(parameters->moveAmount, 0.0f, 12.0f) : 0.0f;
    DamageEvent damage;
    damage.target = player;
    damage.attacker = this;
    // GameRule が攻撃側のattackPowerを掛けるため、基準値1がEditor指定のダメージになる。
    damage.damageAmount = 1.0f;
    damage.knockbackVelocity = {away.x * recoil, 1.2f, away.z * recoil};
    EventManager::GetInstance()->Dispatch(damage);
}

// 当たり判定とInspector用パラメーターを持つ新しい障害物へ、元の設定を複製します。
std::unique_ptr<engine::Object3D> GimmickContactHazard::Clone() const {
    auto clone = std::make_unique<GimmickContactHazard>();
    clone->Initialize(GetObjectCommon(), GetModelName());
    clone->CopyFrom(this);
    return clone;
}
