#include <cstdint>
#include "GimmickTimedSwitch.h"
#include "ObjectPropertyDefinition.h"
#include "CollisionConfig.h"
#include "SceneManager.h"
#include <cmath>

void GimmickTimedSwitch::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    // 上面を踏める地面として登録し、待機時の色を設定する。
    BaseGimmick::Initialize(common, modelName);

    SetClassName("Gimmick");
    SetGimmickType("TimedSwitch");
    SetName("Gimmick_TimedSwitch");
    SetCollisionAttribute(engine::collisionCategories::Get("ground"));
    SetCollisionMask(engine::collisionCategories::GetDefaultGameplayMask());
    SetColor({0.25f, 0.75f, 1.0f, 1.0f});
    SetScale({1.5f, 0.25f, 1.5f});

    // 上面の接触で通知する宛先を編集できるよう、既存の共通設定を登録する。
    using Self = GimmickTimedSwitch;
    engine::ObjectPropertyDefinition<Self> properties(*this, kTypeId, "時限スイッチ床",
        "上面を踏んでいる間、Target IDへ通知します。保持時間は実装側の固定値です。長い時限動作は汎用スイッチの「一定時間だけ」を使います。");
}

void GimmickTimedSwitch::Update(float deltaTime) {
    // 保持時間が残っている間だけ、前回の上面接触からの経過秒数を引く。
    if (contactTimer_ > 0.0f) {
        contactTimer_ -= deltaTime;
        // このフレームで保持時間を使い切った場合は0秒で止める。
        if (contactTimer_ < 0.0f)
            contactTimer_ = 0.0f;
    }

    // 接触の通知が一瞬途切れても保持時間中は押下色を保ち、失効したら待機色へ戻します。
    bool isPressed = contactTimer_ > 0.0f;

    // 保持時間が正なら、押下状態の色にパルスを重ねる。
    if (isPressed) {
        float pulse = 0.85f + std::sin(contactTimer_ * 35.0f) * 0.15f;
        SetColor({0.2f, 1.0f, 0.45f + pulse * 0.2f, 1.0f});
    // 保持時間が尽きた場合は、青い待機色へ戻す。
    } else {
        SetColor({0.25f, 0.75f, 1.0f, 1.0f});
    }

    BaseGimmick::Update(deltaTime);
}

// 上面への接触中だけタイマーを更新し、接続先イベントを通知します。
bool GimmickTimedSwitch::OnCollision(engine::Object3D* other) {
    // プレイヤー以外との接触では、タイマーもイベントも更新しない。
    if (!other || other->GetClassName() != "Player")
        return true;

    engine::CollisionInfo info = CheckCollision(other);

    // 衝突が成立し、床側から見た法線のYが-0.5未満の上面接触なら、0.12秒の保持を更新する。
    if (info.isColliding && info.normal.y < -0.5f) {
        contactTimer_ = 0.12f;

        std::int32_t targetId = GetTargetId();

        // 接続先が設定されている場合は、現在のシーンへスイッチの通知を送ります。
        if (targetId != -1) {
            // 現在のシーンがある場合だけ、設定された宛先IDへイベントを送る。
            if (BaseScene* scene = SceneManager::GetInstance()->GetCurrentScene()) {
                scene->TriggerEvent(targetId);
            }
        }
    }

    return true;
}

std::unique_ptr<engine::Object3D> GimmickTimedSwitch::Clone() const {
    // 押下タイマーは持ち込まず、配置と接続先の設定だけを複製する。
    auto newObj = std::make_unique<GimmickTimedSwitch>();
    assert(GetObjectCommon() != nullptr);
    newObj->Initialize(GetObjectCommon(), GetModelName());
    newObj->CopyFrom(this);
    return newObj;
}
