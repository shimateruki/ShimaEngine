#include "GimmickTrampoline.h"
#include "ObjectPropertyDefinition.h"
#include "Player.h"
#include "CollisionConfig.h"
#include "game/actor/player/state/PlayerState.h"
#include <algorithm>
#include <cmath>

namespace {
constexpr float kBounceCooldown = 0.28f;
constexpr float kSquashStiffness = 110.0f;
constexpr float kSquashDamping = 16.0f;
}

// プレイヤーの着地で反発する傘の判定と、反発速度の設定を初期化します。
void GimmickTrampoline::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    BaseGimmick::Initialize(common, modelName);
    SetClassName("Gimmick");
    SetGimmickType("Trampoline");
    // 共通パラメーターが無ければ、ジャンプ力を書き込めるよう作成する。
    if (!GetParameters())
        ResetParameters();
    SetParameter(&engine::EntityParameter::jumpPower, 30.0f);

    // 傘全体を床と側面に使い、モデルの曲面から着地高さを求める。
    // 柄と根元はシーン側の子コライダーに分け、反発対象にはしない。
    engine::ColliderConfig config = GetColliderConfig();
    config.type = engine::ColliderType::kObb;
    config.center = {0.0f, 1.20f, 0.0f};
    config.size = {1.40f, 0.40f, 1.40f};
    config.useModelSurface = true;
    SetColliderConfig(config);
    SetCollisionAttribute(engine::collisionCategories::Get("ground"));
    SetCollisionMask(engine::collisionCategories::Get("player"));

    // 着地時に与える上向き速度をInspectorのジャンプ力として公開する。
    using Self = GimmickTrampoline;
    engine::ObjectPropertyDefinition<Self> properties(*this, kTypeId, "トランポリン",
        "");
    using Parameter = engine::Object3D::EntityParameter;
    properties.Parameter("ジャンプ力", "jumpPower", &Parameter::jumpPower, 0.0f, 100.0f,
        nullptr, {}, {.speed = 1.0f});
}

void GimmickTrampoline::Update(float deltaTime) {
    // 初回だけ配置時の大きさと回転を保存し、以後の変形の基準にする。
    if (!authoredTransformCaptured_) {
        authoredScale_ = GetScale();
        authoredRotation_ = GetRotation();
        authoredTransformCaptured_ = true;

        // 見た目のつぶれで着地面まで縮まないよう、衝突形状は配置時の大きさに固定する。
        if (GetCollider())
            GetCollider()->SetScaleOverride(authoredScale_);
    }

    // 長いフレームでも表示用ばねの更新幅を最大0.05秒に抑え、急な伸縮を避けます。
    const float frameDeltaTime = (std::clamp)(deltaTime, 0.0f, 0.05f);
    bounceCooldown_ = (std::max)(0.0f, bounceCooldown_ - frameDeltaTime);
    wobbleTime_ = (std::max)(0.0f, wobbleTime_ - frameDeltaTime);
    // 縮みを基準形へ戻すばねの力と、振動を抑える速度抵抗を合成し、速度を更新してから縮み量へ反映します。
    squashVelocity_ += (-kSquashStiffness * squash_ - kSquashDamping * squashVelocity_) * frameDeltaTime;
    squash_ += squashVelocity_ * frameDeltaTime;

    // 縮みとその速度が十分に小さくなったら、描画が微振動し続けないよう静止させます。
    if (std::abs(squash_) < 0.001f && std::abs(squashVelocity_) < 0.01f) {
        squash_ = 0.0f;
        squashVelocity_ = 0.0f;
    }

    // 縦に縮んだ分は横に広げ、着地直後の横揺れは配置角度へ加えます。
    const float spread = 1.0f - squash_ * 0.28f;
    SetScale({authoredScale_.x * spread, authoredScale_.y * (1.0f + squash_), authoredScale_.z * spread});
    const float wobble = std::sin((0.42f - wobbleTime_) * 38.0f) * wobbleTime_ * 0.13f;
    SetEulerAngles({authoredRotation_.x, authoredRotation_.y, authoredRotation_.z + wobble});
    BaseGimmick::Update(deltaTime);
}

bool GimmickTrampoline::OnCollision(engine::Object3D* other) {
    Player* player = dynamic_cast<Player*>(other);
    // 相手がプレイヤーでなければ、反発しなかったことを返す。
    if (!player)
        return false;
    const engine::CollisionInfo info = CheckCollision(other);

    // 実際に衝突していなければ反発しない。衝突した場合は法線をプレイヤー側の接地方向へ反転する。
    if (!info.isColliding)
        return false;
    return TryBounce(player, info.normal * -1.0f);
}

bool GimmickTrampoline::TryBounce(Player* player, const engine::Vector3& playerGroundNormal) {
    // 対象なし、再反発の待機中、法線が上向きでない、上昇中のいずれかなら跳ね直さない。
    if (!player || bounceCooldown_ > 0.0f || playerGroundNormal.y <= 0.5f ||
        player->GetVelocity().y > 0.0f) return false;

    const engine::Collider* capCollider = GetCollider();
    // モデル上面を調べる判定が無ければ、着地面を確認できないため反発しない。
    if (!capCollider)
        return false;
    const engine::Aabb playerBounds = player->GetAabb();
    const engine::Vector3 footCenter = {
        (playerBounds.min.x + playerBounds.max.x) * 0.5f,
        playerBounds.min.y,
        (playerBounds.min.z + playerBounds.max.z) * 0.5f
    };
    float capHeight = 0.0f;
    engine::Vector3 capNormal{};
    // 互換モーターの１フレーム分の沈み込みは許し、傘の下から来た接触は除く。
    const float landingTolerance = (std::max)(0.15f, std::abs(authoredScale_.y) * 0.20f);
    // 曲面を取得できない、足元が許容範囲より下、または面が急すぎる場合は着地とみなさない。
    if (!capCollider->SampleModelSurface(footCenter, capHeight, capNormal) ||
        footCenter.y < capHeight - landingTolerance || capNormal.y <= 0.5f)
        return false;

    // 指定の上昇速度を Y に設定し、接地を解除する。水平速度はそのまま引き継ぐ。
    engine::Vector3 velocity = player->GetVelocity();
    velocity.y = GetParameters() ? GetParameters()->jumpPower : 30.0f;
    player->SetVelocity(velocity);
    player->SetGrounded(false);
    player->ChangeState(std::make_unique<PlayerStateJump>());
    bounceCooldown_ = kBounceCooldown;
    squash_ = -0.28f;
    squashVelocity_ = -1.4f;
    wobbleTime_ = 0.42f;
    return true;
}

std::unique_ptr<engine::Object3D> GimmickTrampoline::Clone() const {
    // 複製後も専用の衝突・反発動作を保つ。
    auto clone = std::make_unique<GimmickTrampoline>();
    clone->Initialize(GetObjectCommon(), GetModelName());
    clone->CopyFrom(this);
    return clone;
}
