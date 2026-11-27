#define NOMINMAX
#include "GimmickPrismBarrier.h"
#include "Easing.h"
#include <numbers>
#include "ObjectPropertyDefinition.h"

#include "CollisionConfig.h"
#include "SceneManager.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace {
constexpr float kDefaultTransitionDuration = 0.42f;
constexpr std::int32_t kRoundTowardPositiveZ = 1;
constexpr std::int32_t kRoundTowardNegativeZ = 2;
constexpr std::int32_t kRoundForgeTowardPositiveZ = 3;
constexpr std::int32_t kRoundForgeTowardNegativeZ = 4;
// 溶岩砦の床上面は角が約4ワールド単位切れているため、円弧をその内側に通す。
constexpr float kForgeCornerRadius = 7.25f;
constexpr float kRoundedBarrierEffect = 3.25f;

} // namespace

// 障壁の薄い形状、発光材質、非表示から始める再生設定を用意する。
void GimmickPrismBarrier::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    BaseGimmick::Initialize(common, modelName);

    // 通常の地面属性を持つ薄いOBBを使い、再生中の衝突の切り替えはApplyRuntimeVisual()に任せる。
    SetClassName("Gimmick");
    SetGimmickType("PrismBarrier");
    SetName("Gimmick_PrismBarrier");
    SetStatic(false);
    SetCastShadow(false);
    SetMaterialType(22);
    SetBlendMode(BlendMode::kNormal);
    SetEnableEnvMap(false);
    SetColor({0.30f, 0.86f, 1.0f, 0.84f});
    SetEmissive(2.4f);
    SetCollisionAttribute(engine::collisionCategories::Get("ground"));
    SetCollisionMask(0xffffffffu);

    engine::Object3D::ColliderConfig collider;
    collider.type = engine::ColliderType::kObb;
    collider.center = {0.0f, 0.0f, 0.0f};
    collider.size = {1.0f, 1.0f, 0.45f};
    SetColliderConfig(collider);

    // まだパラメーターがなければ、障壁用の初期値を書き込むために作成する。
    if (!GetParameters().has_value()) {
        ResetParameters();
    }

    SetParameter(&engine::EntityParameter::gimmickType, "PrismBarrier");
    SetParameter(&engine::EntityParameter::moveSpeed, kDefaultTransitionDuration);
    SetParameter(&engine::EntityParameter::interval, 2.4f);
    SetParameter(&engine::EntityParameter::startActive, false);
    SetParameter(&engine::EntityParameter::returnOnOff, true);

    // 遭遇から受け取るイベントと、開閉時間・発光周期・開始状態の調整項目を登録する。
    using Self = GimmickPrismBarrier;
    engine::ObjectPropertyDefinition<Self> properties(*this, kTypeId, "プリズム障壁",
        "My Event IDは遭遇管理のTarget ID + 連番へ接続します。");
    using Parameter = engine::Object3D::EntityParameter;
    properties.Parameter("格子の角", "actionMode", &Parameter::actionMode, 0.0f, 2.0f,
        nullptr, {"直角", "ローカル +Z側へ丸める", "ローカル -Z側へ丸める"});
    properties.Parameter("展開・解除時間", "moveSpeed", &Parameter::moveSpeed, 0.05f, 3.0f);
    properties.Parameter("発光パルス速度", "interval", &Parameter::interval, 0.1f, 8.0f);
    properties.Parameter("開始時に有効", "startActive", &Parameter::startActive);
    properties.Parameter("OFFで元に戻す", "returnOnOff", &Parameter::returnOnOff);
}

// 編集時の配置値を基準にして開閉を補間します。再生を止めた後も元の配置へ戻せるようにします。
void GimmickPrismBarrier::Update(float deltaTime) {
    SceneManager* sceneManager = SceneManager::GetInstance();
    const bool isPlaying = sceneManager && sceneManager->IsPlaying();

    // プレイ中でなければ編集値を復元する。開閉の進行と発光時刻は進めない。
    if (!isPlaying) {
        // 前回から編集状態なら、別の配置に編集された場合に備えて基準値を取り直す。
        if (!initializedForPlay_) {
            CaptureAuthoredState(true);
        }

        ResetForEditor();
        BaseGimmick::Update(deltaTime);
        return;
    }

    CaptureAuthoredState();

    // プレイ開始前に受け取ったイベントを優先し、未受信なら配置設定を開始状態に使います。
    if (!initializedForPlay_) {
        initializedForPlay_ = true;

        // 初回のプレイ更新までに開閉通知を受けていない場合だけ、配置時の開始設定を使う。
        if (!activationRequestReceived_) {
            targetActivation_ = GetParameters().has_value() && GetParameters()->startActive ? 1.0f : 0.0f;
        }

        activation_ = targetActivation_;
        ApplyRuntimeVisual();
    }

    // 発光用の時刻と展開率を別に進めます。展開率は指定時間で開閉の目標に到達します。
    visualTime_ += (std::max)(0.0f, deltaTime);
    const float duration = GetTransitionDuration();
    const float step = duration > 0.0001f ? deltaTime / duration : 1.0f;

    // 現在の展開率が目標より低い場合は、目標を越えない範囲で壁を伸ばす。
    if (activation_ < targetActivation_) {
        activation_ = (std::min)(targetActivation_, activation_ + step);
    // 現在の展開率が目標より高い場合は、目標を下回らない範囲で壁を縮める。
    } else if (activation_ > targetActivation_) {
        activation_ = (std::max)(targetActivation_, activation_ - step);
    }

    ApplyRuntimeVisual();
    BaseGimmick::Update(deltaTime);
}

void GimmickPrismBarrier::OnTrigger() {
    // 単発イベントは壁を出すスイッチのオンとして扱います。
    OnSwitchEvent(true);
}

void GimmickPrismBarrier::OnSwitchEvent(bool active) {
    // 無効化の要求時に衝突を先に消し、見た目の縮退とは切り離します。
    // 開始前に通知が届いても解除可否を判断できるよう、パラメーターがなければ用意する。
    if (!GetParameters().has_value()) {
        ResetParameters();
    }

    // 解除通知でも、OFFで元に戻す設定が無効なら現在の壁を保つ。
    if (!active && !GetParameters()->returnOnOff) {
        return;
    }

    CaptureAuthoredState();
    activationRequestReceived_ = true;
    targetActivation_ = active ? 1.0f : 0.0f;

    // 起動通知では展開の開始前から壁を表示できるようにする。
    if (active) {
        SetIsVisible(true);
    // 解除通知では縮む演出を残しつつ壁判定をその場で外す。
    } else {
        SetCollisionEnabled(false);

        // プレイ用の初期化前か、現在の展開率が0.005以下なら、その場で完全に隠す。
        // 一度展開した後の解除だけは Update 側の縮退アニメーションを残します。
        if (!initializedForPlay_ || activation_ <= 0.005f) {
            activation_ = 0.0f;
            ApplyRuntimeVisual();
        }
    }
}

std::unique_ptr<engine::Object3D> GimmickPrismBarrier::Clone() const {
    // 初期化後に編集設定を移し、壁の材質と判定を複製先で作り直します。
    auto clone = std::make_unique<GimmickPrismBarrier>();
    assert(GetObjectCommon() != nullptr);
    clone->Initialize(GetObjectCommon(), GetModelName());
    clone->CopyFrom(this);
    return clone;
}

void GimmickPrismBarrier::CaptureAuthoredState(bool force) {
    // 再生中の変形値で編集時の基準を上書きしないよう、初回のみ記録します。
    // 退避済みで強制更新も要求されていないときは、変形後の状態で元の配置を上書きしない。
    if (authoredStateCaptured_ && !force) {
        return;
    }

    authoredPosition_ = GetTransform()->translate;
    authoredScale_ = GetScale();
    authoredColor_ = GetColor();
    const uint32_t collisionAttribute = GetCollisionAttribute();
    const uint32_t collisionMask = GetCollisionMask();

    // フェード中に解除した属性やマスクがゼロでも、後で戻すために最後の設定値を保持します。
    // 属性が0でなければ編集値を更新する。初回だけは0でも取得し、後続で既定値を補う。
    if (collisionAttribute != 0 || !authoredStateCaptured_) {
        authoredCollisionAttribute_ = collisionAttribute;
    }

    // マスクが0でなければ編集値を更新する。初回だけは0でも取得し、後続で既定値を補う。
    if (collisionMask != 0 || !authoredStateCaptured_) {
        authoredCollisionMask_ = collisionMask;
    }

    // 復元用の属性が0なら、地面としての既定属性を補う。
    if (authoredCollisionAttribute_ == 0) {
        authoredCollisionAttribute_ = engine::collisionCategories::Get("ground");
    }

    // 復元用のマスクが0なら、全ビットを許可する既定マスクを補う。
    if (authoredCollisionMask_ == 0) {
        authoredCollisionMask_ = 0xffffffffu;
    }

    authoredStateCaptured_ = true;
}

void GimmickPrismBarrier::ResetForEditor() {
    // 再生中の高さや衝突を持ち越さず、保存された配置へ戻します。
    initializedForPlay_ = false;
    activation_ = 0.0f;
    targetActivation_ = 0.0f;
    visualTime_ = 0.0f;
    activationRequestReceived_ = false;
    SetTranslate(authoredPosition_);
    SetScale(authoredScale_);
    SetColor(authoredColor_);
    SetCollisionEnabled(false);
    SetIsVisible(false);
}

void GimmickPrismBarrier::ApplyRuntimeVisual() {
    // 補間後の高さに合わせて原点も上下させ、配置時の底辺を固定する。衝突の有効化は後で条件を評価する。
    const float eased = Easing::SmoothStep01(activation_);
    const float minimumScaleY = (std::max)(0.015f, authoredScale_.y * 0.012f);
    const float currentScaleY = engine::Math::Lerp(minimumScaleY, authoredScale_.y, eased);
    const float authoredBottom = authoredPosition_.y - authoredScale_.y;
    SetTranslate({authoredPosition_.x, authoredBottom + currentScaleY, authoredPosition_.z});
    SetScale({authoredScale_.x, currentScaleY, authoredScale_.z});

    // 保存済みの色に展開率を掛け、発光側だけに周期的な明滅を重ねます。
    const float pulseSpeed = GetParameters().has_value() ? (std::max)(0.1f, GetParameters()->interval) : 2.4f;
    const float pulse = 0.5f + 0.5f * std::sin(visualTime_ * pulseSpeed * (2.0f * std::numbers::pi_v<float>));
    SetColor({
        authoredColor_.x,
        authoredColor_.y,
        authoredColor_.z,
        authoredColor_.w * eased,
    });
    SetEmissive((1.45f + pulse * 0.72f) * eased);

    // 材質には展開率と開閉の向きに応じたUV座標を渡し、表面の演出を形状と同期させます。
    // 描画側に材質パラメーターがある場合だけ、展開率と向きに応じた表面の演出値を渡す。
    if (auto* renderer = GetMeshRenderer(); renderer && renderer->GetWaterParamData()) {
        auto* effect = renderer->GetWaterParamData();
        effect->waveSpeed = 1.25f;
        effect->waveHeight = 0.72f;
        effect->waveFrequency = 18.0f;
        const auto cornerMode = GetParameters() ? GetParameters()->actionMode : 0;
        const bool forgeCorners = cornerMode == kRoundForgeTowardPositiveZ ||
            cornerMode == kRoundForgeTowardNegativeZ;
        const bool roundTowardNegativeZ = cornerMode == kRoundTowardNegativeZ ||
            cornerMode == kRoundForgeTowardNegativeZ;
        const bool roundCorners = forgeCorners || cornerMode == kRoundTowardPositiveZ ||
            roundTowardNegativeZ;
        effect->effectType = roundCorners ? kRoundedBarrierEffect : 3.0f;
        effect->effectScale = 1.0f;
        effect->effectSoftness = 0.48f;
        effect->effectIntensity = 0.06f + eased * 1.72f;
        effect->billboardScale = 1.0f;
        effect->effectScaleX = 1.0f;
        effect->effectScaleY = 1.0f;
        // 丸角では符号が内側の向き。溶岩砦では絶対値でワールド単位の半径も渡す。
        const float cornerRadius = forgeCorners ? kForgeCornerRadius : 1.0f;
        effect->effectScaleZ = roundCorners ? (roundTowardNegativeZ ? -cornerRadius : cornerRadius) : 0.08f;
        effect->uvOffsetX = targetActivation_ > activation_ ? 1.0f - activation_ : activation_ * 0.18f;
        effect->uvOffsetY = eased;
    }

    // 展開目標が0.5を超え、現在の展開率が0.22以上なら衝突を有効にする。解除中の衝突は無効。
    const bool shouldCollide = targetActivation_ > 0.5f && activation_ >= 0.22f;
    SetCollisionEnabled(shouldCollide);

    // 目標が解除で現在の展開率も0.005以下になったら、表示を止める。
    if (activation_ <= 0.005f && targetActivation_ <= 0.0f) {
        SetIsVisible(false);
    }
}

void GimmickPrismBarrier::SetCollisionEnabled(bool enabled) {
    // ゼロ化した衝突設定を再有効化時に編集値へ戻します。
    SetCollisionAttribute(enabled ? authoredCollisionAttribute_ : 0);
    SetCollisionMask(enabled ? authoredCollisionMask_ : 0);
}

float GimmickPrismBarrier::GetTransitionDuration() const {
    // 設定の開閉時間を0.05〜3.0秒に収める。設定がなければ0.42秒。
    return GetParameters().has_value() ? (std::clamp)(GetParameters()->moveSpeed, 0.05f, 3.0f) : kDefaultTransitionDuration;
}
