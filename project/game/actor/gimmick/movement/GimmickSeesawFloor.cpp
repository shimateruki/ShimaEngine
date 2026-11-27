#include "GimmickSeesawFloor.h"
#include "ObjectPropertyDefinition.h"
#include "CollisionConfig.h"
#include <algorithm>
#include <cmath>

void GimmickSeesawFloor::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    // シーソーの天面を地面として扱い、乗員を支えられるようにする。
    BaseGimmick::Initialize(common, modelName);

    SetClassName("Gimmick");
    SetGimmickType("SeesawFloor");
    SetName("Gimmick_SeesawFloor");
    SetCollisionAttribute(engine::collisionCategories::Get("ground"));
    SetCollisionMask(engine::collisionCategories::GetDefaultGameplayMask());
    SetScale({4.0f, 0.35f, 1.4f});

    // 実行中の傾きと接触タイマーを除き、配置用の編集項目を登録する。
    using Self = GimmickSeesawFloor;
    engine::ObjectPropertyDefinition<Self> properties(*this, kTypeId, "シーソー床",
        "接触位置に応じて傾く床です。位置と大きさはTransformで調整します。実行中の接触時計や傾きは編集設定として公開しません。");
}

// 接触の猶予が切れたら配置時の基準角へ戻し、急な角度変化を経過秒で補間します。
void GimmickSeesawFloor::Update(float deltaTime) {
    // 編集時の姿勢を基準角として保持し、Play中の傾きから復帰できるようにする。
    if (!initializedBase_) {
        baseRotation_ = GetTransform()->rotate;
        initializedBase_ = true;
    }

    // 上面接触からの猶予が残る間は、最後に決めた目標角を維持する。
    if (contactTimer_ > 0.0f) {
        contactTimer_ -= deltaTime;
        // 今フレームで猶予を越えた場合は0秒で止める。
        if (contactTimer_ < 0.0f)
            contactTimer_ = 0.0f;
    // 更新開始時に猶予が無ければ、目標を配置時の基準角へ戻す。
    } else {
        targetTilt_ = 0.0f;
    }

    // フレーム時間に応じて現在の角度を基準姿勢と目標角へ近づけ、段差のない傾きにします。
    float t = 1.0f - std::exp(-tiltSpeed_ * deltaTime);
    engine::Vector3 rot = GetTransform()->rotate;
    rot.x = engine::Math::Lerp(rot.x, baseRotation_.x, t);
    rot.z = engine::Math::Lerp(rot.z, baseRotation_.z + targetTilt_, t);
    SetEulerAngles(rot);
    SetQuaternionMaster(false);

    BaseGimmick::Update(deltaTime);
}

// プレイヤーの接触位置を床のローカル左右方向へ変換して傾きを決めます。
bool GimmickSeesawFloor::OnCollision(engine::Object3D* other) {
    // 上面に乗ったPlayerの位置から左右どちらへ傾けるかを決める。
    if (other && other->GetClassName() == "Player") {
        engine::CollisionInfo info = CheckCollision(other);

        // 衝突が成立し、床側から見た法線のYが-0.5未満の接触だけ傾きに反映する。
        if (info.isColliding && info.normal.y < -0.5f) {
            // 床のヨーを打ち消してローカルXを求め、中央からの距離を最大角に対する割合へ変換します。
            engine::Vector3 diff = other->GetWorldPosition() - GetWorldPosition();
            float yaw = GetTransform()->rotate.y;
            float localX = diff.x * std::cos(yaw) - diff.z * std::sin(yaw);
            float amount = std::clamp(localX / halfLength_, -1.0f, 1.0f);

            targetTilt_ = -amount * maxTilt_;
            contactTimer_ = 0.12f;
        }
    }

    return true;
}

std::unique_ptr<engine::Object3D> GimmickSeesawFloor::Clone() const {
    // 揺れの途中状態は持ち越さず、配置と設定から新しい床を作る。
    auto newObj = std::make_unique<GimmickSeesawFloor>();
    assert(GetObjectCommon() != nullptr);
    newObj->Initialize(GetObjectCommon(), GetModelName());
    newObj->CopyFrom(this);
    return newObj;
}
