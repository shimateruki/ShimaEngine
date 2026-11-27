#include "GimmickDashPanel.h"
#include "ObjectPropertyDefinition.h"
#include "CollisionConfig.h"
#include "Player.h"

#include <algorithm>
#include <cmath>

void GimmickDashPanel::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    // 地面として接触し、踏んだ時の光が読める材質を既定にする。
    BaseGimmick::Initialize(common, modelName);

    SetClassName("Gimmick");
    SetGimmickType("DashPanel");
    SetName("Gimmick_DashPanel");
    SetCollisionAttribute(engine::collisionCategories::Get("ground"));
    SetCollisionMask(engine::collisionCategories::GetDefaultGameplayMask());
    SetMaterialType(24);
    SetBlendMode(BlendMode::kNone);
    SetColor({1.0f, 0.38f, 0.04f, 1.0f});
    SetRoughness(0.42f);
    SetMetallic(0.28f);
    SetEmissive(1.2f);
    SetTextureTiling({1.0f, 1.0f});
    SetAutoTextureTiling(false);
    SetScale({2.0f, 0.25f, 1.2f});

    // 加速・解除の役割と、共通の配置項目をInspectorに表示する。
    using Self = GimmickDashPanel;
    engine::ObjectPropertyDefinition<Self> properties(*this, kTypeId, "ダッシュパネル",
        "上面を踏むと、ダッシュパネルは加速し、ダッシュ解除パネルはパネル加速を解除します。配置と向きはTransformで調整します。");
}

void GimmickDashPanel::Update(float deltaTime) {
    // オブジェクト更新の後に衝突を調べるため、前回の上面接触を繰り越す。
    touchedTopLastUpdate_ = touchedTopThisUpdate_;
    touchedTopThisUpdate_ = false;

    // 編集済みの基準色を最初に保存し、短い発光パルスの後に戻す。
    if (!visualBaseCaptured_) {
        baseColor_ = GetColor();
        baseEmissive_ = (std::max)(1.2f, GetEmissive());
        visualBaseCaptured_ = true;
    }

    // 再発動の待ち時間と点灯の減衰を別々に数え、表示の長さで次の発動を決めない。
    visualTime_ += deltaTime;
    activationCooldown_ = (std::max)(0.0f, activationCooldown_ - deltaTime);
    activationPulse_ = (std::max)(0.0f, activationPulse_ - deltaTime / 0.38f);

    // 待機時の明滅に発動の光を重ね、二乗で発動直後から滑らかに弱める。
    const float easedPulse = activationPulse_ * activationPulse_;
    const float idlePulse = 0.08f + std::sin(visualTime_ * 5.8f) * 0.045f;
    SetEmissive(baseEmissive_ + idlePulse + easedPulse * 3.4f);

    // 点灯の色で停止パネルと加速パネルを見分けられるようにし、透明度は配置時の値を使う。
    const float flashBlend = easedPulse * 0.62f;
    const engine::Vector4 flashColor = GetGimmickType() == kStopTypeId
        ? engine::Vector4{0.30f, 0.86f, 1.0f, baseColor_.w}
        : engine::Vector4{1.0f, 0.86f, 0.30f, baseColor_.w};
    SetColor({
        baseColor_.x + (flashColor.x - baseColor_.x) * flashBlend,
        baseColor_.y + (flashColor.y - baseColor_.y) * flashBlend,
        baseColor_.z + (flashColor.z - baseColor_.z) * flashBlend,
        baseColor_.w,
    });

    BaseGimmick::Update(deltaTime);
}

// 上面を踏んだプレイヤーのパネル加速を更新し、発光パルスは短いクールダウンで抑えます。
bool GimmickDashPanel::OnCollision(engine::Object3D* other) {
    Player* player = dynamic_cast<Player*>(other);
    // 対象が生存したプレイヤーでなければ、パネルの加速状態を変えない。
    if (!player || player->IsDead())
        return true;

    engine::CollisionInfo info = CheckCollision(other);

    // 衝突が成立し、床側から見た法線のYが-0.5未満の場合だけ、上面を踏んだと判定する。
    if (info.isColliding && info.normal.y < -0.5f) {
        const bool startedTouchingTop = !touchedTopLastUpdate_ && !touchedTopThisUpdate_;
        touchedTopThisUpdate_ = true;

        // 解除パネルの上面では、加速が残っていればここで解除する。
        if (GetGimmickType() == kStopTypeId) {
            player->CancelDashPanelBoost();
        // 加速パネルを踏み始めたか、すでに加速中の場合だけ倍率を適用する。
        } else if (startedTouchingTop || player->IsDashPanelBoostActive()) {
            player->ApplyDashPanelBoost(2.15f, 0.32f);
        // 乗ったまま被弾などで加速が切れた場合は、踏み直すまで再発動しない。
        } else {
            return true;
        }

        // 発光の待ち時間が切れた場合だけ点灯し、接触中でも毎フレーム最大光量に戻さない。
        if (activationCooldown_ <= 0.0f) {
            activationPulse_ = 1.0f;
            activationCooldown_ = 0.20f;
        }
    }

    return true;
}

std::unique_ptr<engine::Object3D> GimmickDashPanel::Clone() const {
    // 配置と調整値を複製し、実行時のパルス時計は新しい床に持ち込まない。
    auto newObj = std::make_unique<GimmickDashPanel>();
    assert(GetObjectCommon() != nullptr);
    newObj->Initialize(GetObjectCommon(), GetModelName());
    newObj->CopyFrom(this);
    return newObj;
}
