#define NOMINMAX
#include <cstdint>
#include "GimmickHazardRideFloor.h"
#include "ObjectPropertyDefinition.h"

#include "BaseScene.h"
#include "CollisionConfig.h"
#include "SceneManager.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace {
constexpr float kDefaultTravelSpeed = 7.5f;
constexpr float kDefaultTravelDistance = 46.0f;
constexpr float kDefaultStartDelay = 0.55f;
constexpr float kDefaultEndWarningDuration = 0.7f;
constexpr float kDefaultFallDuration = 2.0f;
constexpr float kDefaultFallGravity = 38.0f;
constexpr float kReturnDelaySeconds = 5.0f;
constexpr std::int32_t kDefaultHazardCount = 4;
} // namespace

// 通常時の床判定を用意し、走行と妨害、終点での落下を設定する。
void GimmickHazardRideFloor::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    BaseGimmick::Initialize(common, modelName);

    // プレイヤーを運ぶ床として衝突を持ち、移動距離と途中の危険イベント数を設定します。
    SetClassName("Gimmick");
    SetGimmickType("HazardRideFloor");
    SetName("Gimmick_HazardRideFloor");
    SetCollisionAttribute(engine::collisionCategories::Get("ground"));
    SetCollisionMask(engine::collisionCategories::GetDefaultGameplayMask());
    SetStatic(false);

    // パラメーターがなければ、床の移動と復帰設定の既定値を書き込めるように用意する。
    if (!GetParameters().has_value()) {
        ResetParameters();
    }

    SetParameter(&engine::EntityParameter::speed, kDefaultTravelSpeed);
    SetParameter(&engine::EntityParameter::moveAmount, kDefaultTravelDistance);
    SetParameter(&engine::EntityParameter::shakeDuration, kDefaultStartDelay);
    SetParameter(&engine::EntityParameter::interval, kDefaultEndWarningDuration);
    SetParameter(&engine::EntityParameter::fallDuration, kDefaultFallDuration);
    SetParameter(&engine::EntityParameter::gravity, kDefaultFallGravity);
    SetParameter(&engine::EntityParameter::maxCount, kDefaultHazardCount);
    SetParameter(&engine::EntityParameter::actionMode, 0);

    // 従来の一度きりの挙動を既定とし、必要なステージだけ明示的に復帰を有効化する。
    SetParameter(&engine::EntityParameter::returnOnOff, false);

    // 走行中はTarget IDから順番に通知する。妨害数、向き、落下後の復帰可否をInspectorから調整できるようにする。
    using Self = GimmickHazardRideFloor;
    engine::ObjectPropertyDefinition<Self> properties(*this, kTypeId, "妨害付き輸送床",
        "Target IDを先頭に、Target ID + 妨害番号を順番に起動します。");
    using Parameter = engine::Object3D::EntityParameter;
    properties.Parameter("移動速度", "speed", &Parameter::speed, 0.1f, 40.0f);
    properties.Parameter("移動距離", "moveAmount", &Parameter::moveAmount, 1.0f, 300.0f);
    properties.Parameter("発進前の予告", "shakeDuration", &Parameter::shakeDuration, 0.0f, 5.0f);
    properties.Parameter("終点の落下予告", "interval", &Parameter::interval, 0.1f, 5.0f);
    properties.Parameter("落下時間", "fallDuration", &Parameter::fallDuration, 0.1f, 10.0f);
    properties.Parameter("落下重力", "gravity", &Parameter::gravity, 1.0f, 200.0f);
    properties.Parameter("妨害数", "maxCount", &Parameter::maxCount, 0.0f, 16.0f);
    properties.Parameter("進行方向", "actionMode", &Parameter::actionMode, 0.0f, 1.0f,
        nullptr, {"ローカルX+", "ローカルX-"});
    properties.Parameter("終了後に復帰を許可", "returnOnOff", &Parameter::returnOnOff);
}

void GimmickHazardRideFloor::Update(float deltaTime) {
    // 乗車後に予告、移動、終点の警告、落下の順で進めます。
    CaptureStartTransform();

    const bool isPlaying = SceneManager::GetInstance() && SceneManager::GetInstance()->IsPlaying();

    // プレイ中でなければ移動・妨害の状態を初期化し、元の位置で共通更新だけ行う。
    if (!isPlaying) {
        ResetForRetry();
        BaseGimmick::Update(deltaTime);
        return;
    }

    const engine::Vector3 previousPosition = GetTransform()->translate;
    stateTimer_ += deltaTime;

    switch (state_) {
    // プレイヤーが乗るか外部イベントが届くまで、床を出発位置に残します。
    case State::kWaiting:
        break;

    // 発進予告では震えを強め、始動時に一度出発位置へ戻してから走行します。
    case State::kStarting: {
        const float delay = GetStartDelay();
        const float normalized = delay > 0.0f ? (std::min)(stateTimer_ / delay, 1.0f) : 1.0f;
        const float intensity = 0.025f + normalized * 0.045f;
        SetTranslateX(startPosition_.x + (static_cast<float>(std::rand()) / RAND_MAX * 2.0f - 1.0f) * intensity);
        SetTranslateZ(startPosition_.z + (static_cast<float>(std::rand()) / RAND_MAX * 2.0f - 1.0f) * intensity);

        // 発進前の予告時間が終わったら、揺れによる変位を戻して走行へ移る。
        if (stateTimer_ >= delay) {
            SetTranslate(startPosition_);
            ChangeState(State::kMoving);
        }

        break;
    }

    // 進んだ距離の割合で連動する妨害を起動し、終点を超えないよう走行距離を制限します。
    case State::kMoving: {
        travelledDistance_ = (std::min)(GetTravelDistance(), travelledDistance_ + GetTravelSpeed() * deltaTime);
        const float progress = GetTravelDistance() > 0.0f ? travelledDistance_ / GetTravelDistance() : 1.0f;
        SetTranslate(startPosition_ + GetTravelDirection() * travelledDistance_);
        SetTranslateY(GetTranslate().y + std::sin(progress * 18.8495559f) * 0.055f);
        TriggerPendingHazards(progress);

        // 設定距離まで進んだ場合は、揺れによる落下の予告に移る。
        if (travelledDistance_ >= GetTravelDistance()) {
            ChangeState(State::kWarning);
        }

        break;
    }

    // 終点に着いたことを揺れで伝え、落下が始まるまで床の高さを保ちます。
    case State::kWarning: {
        const float intensity = 0.04f + (std::min)(stateTimer_ / GetEndWarningDuration(), 1.0f) * 0.08f;
        const engine::Vector3 endPosition = startPosition_ + GetTravelDirection() * GetTravelDistance();
        SetTranslate(endPosition);
        SetTranslateX(GetTranslate().x + (static_cast<float>(std::rand()) / RAND_MAX * 2.0f - 1.0f) * intensity);
        SetTranslateZ(GetTranslate().z + (static_cast<float>(std::rand()) / RAND_MAX * 2.0f - 1.0f) * intensity);

        // 終点の予告時間が経過したら、鉛直速度を0にして落下へ進む。
        if (stateTimer_ >= GetEndWarningDuration()) {
            ChangeState(State::kFalling);
        }

        break;
    }

    // 落下中は鉛直速度に重力を積分し、床の傾きを加えます。所定時間の後に非表示へ移します。
    case State::kFalling:
        fallVelocityY_ -= GetFallGravity() * deltaTime;
        SetTranslateY(GetTranslate().y + fallVelocityY_ * deltaTime);
        SetEulerX(GetRotation().x + 0.8f * deltaTime);
        SetEulerZ(GetRotation().z + 0.55f * deltaTime);
        SetQuaternionMaster(false);

        // 落下を続ける秒数に達したら、ChangeState()で表示と衝突を無効化する。
        if (stateTimer_ >= GetFallDuration()) {
            ChangeState(State::kHidden);
        }

        break;

    // 繰り返し使える設定の場合だけ、落下後の待機を終えて元の位置へ復帰します。
    case State::kHidden:
        // 復帰を許可され、消失してから5秒が経過した場合だけ出発位置へ戻す。
        if (ShouldReturnAfterRide() && stateTimer_ >= kReturnDelaySeconds) {
            ResetForRetry();
        }

        break;
    }

    // 床の移動前後の差を保存し、上面に触れたプレイヤーへOnCollision()で渡す。
    frameDelta_ = GetTransform()->translate - previousPosition;
    BaseGimmick::Update(deltaTime);
}

bool GimmickHazardRideFloor::OnCollision(engine::Object3D* other) {
    // 上面へ乗ったプレイヤーだけを運び、横からの接触では発進させません。
    // プレイヤー以外や相手のない通知では、発進と移動量の引き継ぎを行わない。
    if (!other || other->GetClassName() != "Player") {
        return true;
    }

    const engine::CollisionInfo info = CheckCollision(other);

    // 実際に接触していて床から見た法線のYが-0.5未満の場合だけ、上面に乗ったとみなす。
    if (!info.isColliding || info.normal.y >= -0.5f) {
        return true;
    }

    // 待機中の上面接触で、発進予告を1回だけ始める。
    if (state_ == State::kWaiting) {
        BeginRide();
    }

    // 消失していない床に乗っているプレイヤーへ、このフレームの変位を渡す。
    if (state_ != State::kHidden) {
        other->SetTranslate(other->GetTranslate() + frameDelta_);
    }

    return true;
}

void GimmickHazardRideFloor::OnTrigger() {
    // 外部イベントからも乗車と同じ発進手順を開始できます。
    BeginRide();
}

std::unique_ptr<engine::Object3D> GimmickHazardRideFloor::Clone() const {
    // 複製先を初期化してから走行設定と配置を引き継ぎます。
    auto clone = std::make_unique<GimmickHazardRideFloor>();
    assert(GetObjectCommon() != nullptr);
    clone->Initialize(GetObjectCommon(), GetModelName());
    clone->CopyFrom(this);
    return clone;
}

void GimmickHazardRideFloor::CaptureStartTransform() {
    // 編集時の配置を一度だけ保存し、再挑戦時の復帰先にします。
    if (hasCapturedStartTransform_) {
        return;
    }

    startPosition_ = GetTransform()->translate;
    startRotation_ = GetTransform()->rotate;
    hasCapturedStartTransform_ = true;
}

void GimmickHazardRideFloor::BeginRide() {
    // 二重の開始通知でタイマーが巻き戻らないよう待機中だけ開始します。
    if (state_ != State::kWaiting) {
        return;
    }

    CaptureStartTransform();
    ChangeState(State::kStarting);
}

void GimmickHazardRideFloor::TriggerPendingHazards(float progress) {
    // 走行距離を均等分割した地点で、対応する危険イベントを一度ずつ発火します。
    const std::int32_t count = GetHazardCount();

    // 長いフレームで複数の発火地点を越えた場合も、通過した順番で通知します。
    while (nextHazardIndex_ < count) {
        const float triggerProgress = static_cast<float>(nextHazardIndex_ + 1) / static_cast<float>(count + 1);

        // まだ次の妨害地点に達していなければ、後続を通知せず次フレームを待つ。
        if (progress + 0.0001f < triggerProgress) {
            break;
        }

        const std::int32_t baseEventId = GetTargetId();

        // Target IDが正のときに限り、連番で接続先を指定する。
        if (baseEventId > 0) {
            // シーンを取得できた場合だけイベントを送る。送れない場合も通過済みとして番号を進める。
            if (BaseScene* scene = SceneManager::GetInstance()->GetCurrentScene()) {
                scene->TriggerEvent(baseEventId + nextHazardIndex_);
            }
        }

        ++nextHazardIndex_;
    }
}

void GimmickHazardRideFloor::ResetForRetry() {
    // 再挑戦では移動・落下・発火済み数を消し、元の足場へ戻します。
    state_ = State::kWaiting;
    stateTimer_ = 0.0f;
    travelledDistance_ = 0.0f;
    fallVelocityY_ = 0.0f;
    nextHazardIndex_ = 0;
    frameDelta_ = {};
    SetIsVisible(true);
    SetCollisionAttribute(engine::collisionCategories::Get("ground"));
    SetCollisionMask(engine::collisionCategories::GetDefaultGameplayMask());
    SetTranslate(startPosition_);
    SetEulerAngles(startRotation_);
    SetQuaternionMaster(false);
}

void GimmickHazardRideFloor::ChangeState(State nextState) {
    // 段階の経過を0秒に戻す。落下開始時は鉛直速度を初期化し、消失時は見た目と衝突を除去する。
    state_ = nextState;
    stateTimer_ = 0.0f;

    // 落下する段階に入れば、静止状態から重力で加速させる。
    if (state_ == State::kFalling) {
        fallVelocityY_ = 0.0f;
    // 落下が終わって消失する段階では、表示と衝突を無効にする。
    } else if (state_ == State::kHidden) {
        SetIsVisible(false);
        SetCollisionAttribute(0);
        SetCollisionMask(0);
        frameDelta_ = {};
    }
}

bool GimmickHazardRideFloor::ShouldReturnAfterRide() const {
    // 復帰はステージ側で明示的に有効化した床に限ります。
    return GetParameters().has_value() && GetParameters()->returnOnOff;
}

engine::Vector3 GimmickHazardRideFloor::GetTravelDirection() const {
    // 配置時のヨー角を進行方向に変え、逆走モードでは符号を反転します。
    const float yaw = startRotation_.y;
    engine::Vector3 direction = {std::cos(yaw), 0.0f, std::sin(yaw)};

    // 設定があり、モードがローカルX-なら水平方向を反転する。
    if (GetParameters().has_value() && GetParameters()->actionMode == 1) {
        direction = direction * -1.0f;
    }

    return direction;
}

float GimmickHazardRideFloor::GetTravelSpeed() const {
    // 負値やゼロでも床が停止しないよう最低速度を保証します。
    return GetParameters().has_value() ? (std::max)(0.1f, std::abs(GetParameters()->speed)) : kDefaultTravelSpeed;
}

float GimmickHazardRideFloor::GetTravelDistance() const {
    // 走行距離は正の値として扱います。
    return GetParameters().has_value() ? (std::max)(0.1f, std::abs(GetParameters()->moveAmount)) : kDefaultTravelDistance;
}

float GimmickHazardRideFloor::GetStartDelay() const {
    // 発進前の予告はゼロ秒まで許可します。
    return GetParameters().has_value() ? (std::max)(0.0f, GetParameters()->shakeDuration) : kDefaultStartDelay;
}

float GimmickHazardRideFloor::GetEndWarningDuration() const {
    // 終点の警告には最低時間を設け、即落下を避けます。
    return GetParameters().has_value() ? (std::max)(0.1f, GetParameters()->interval) : kDefaultEndWarningDuration;
}

float GimmickHazardRideFloor::GetFallDuration() const {
    // 落下演出の時間を0.1秒以上にする。パラメーターがなければ2秒。
    return GetParameters().has_value() ? (std::max)(0.1f, GetParameters()->fallDuration) : kDefaultFallDuration;
}

float GimmickHazardRideFloor::GetFallGravity() const {
    // 重力の下限は0.1ワールド単位/秒²、パラメーターがない場合は38。
    return GetParameters().has_value() ? (std::max)(0.1f, GetParameters()->gravity) : kDefaultFallGravity;
}

std::int32_t GimmickHazardRideFloor::GetHazardCount() const {
    // 途中イベントの数を上限内に収め、過剰な発火を防ぎます。
    return GetParameters().has_value() ? (std::clamp)(GetParameters()->maxCount, 0, 16) : kDefaultHazardCount;
}
