#define NOMINMAX
#include "GimmickBossGate.h"
#include "Easing.h"
#include "ObjectPropertyDefinition.h"

#include "CollisionConfig.h"
#include "SceneManager.h"
#include "VfxSequencer.h"

#include <algorithm>
#include <cassert>

namespace {

} // namespace

void GimmickBossGate::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    // 閉鎖中の門を壁として扱い、開く方向と所要時間の既定値を用意する。
    BaseGimmick::Initialize(common, modelName);
    SetClassName("Gimmick");
    SetGimmickType("BossGate");
    SetName("Gimmick_BossGate");
    SetStatic(false);
    SetCastShadow(true);
    SetCollisionAttribute(engine::collisionCategories::Get("ground"));
    SetCollisionMask(0xffffffffu);

    engine::Object3D::ColliderConfig collider;
    collider.type = engine::ColliderType::kObb;
    collider.center = {0.0f, 3.5f, 0.0f};
    collider.size = {6.0f, 3.5f, 0.55f};
    SetColliderConfig(collider);

    // ゲート用の初期設定を登録できるよう、パラメーターがない場合は既定値を作る。
    if (!GetParameters().has_value())
        ResetParameters();
    SetParameter(&engine::EntityParameter::gimmickType, "BossGate");
    SetParameter(&engine::EntityParameter::moveSpeed, 0.58f);
    SetParameter(&engine::EntityParameter::moveAmount, 8.0f);
    SetParameter(&engine::EntityParameter::startActive, false);
    SetParameter(&engine::EntityParameter::returnOnOff, true);

    // 配置のTransformを閉鎖位置として使う。Inspectorには開放時の退避距離と開閉時間を公開する。
    using Self = GimmickBossGate;
    engine::ObjectPropertyDefinition<Self> properties(*this, kTypeId, "ボス闘技場・格子ゲート",
        "Transformは閉鎖位置です。演出中はタイムライン制御、通常時はイベント通知で開閉します。");
    using Parameter = engine::Object3D::EntityParameter;
    properties.Parameter("開門時の退避距離", "moveAmount", &Parameter::moveAmount, 0.0f, 100.0f);
    properties.Parameter("開閉にかかる時間", "moveSpeed", &Parameter::moveSpeed, 0.05f, 10.0f);
    properties.Parameter("開始時に有効", "startActive", &Parameter::startActive);
    properties.Parameter("OFFで元に戻す", "returnOnOff", &Parameter::returnOnOff);
}

void GimmickBossGate::Update(float deltaTime) {
    // エディターでは配置位置へ戻し、プレイ中のみ閉鎖率に応じて門を動かします。
    SceneManager* sceneManager = SceneManager::GetInstance();
    const bool isPlaying = sceneManager && sceneManager->IsPlaying();

    // 編集中は演出による移動を止め、位置と縮尺を編集配置へ戻す。
    if (!isPlaying) {
        ResetForEditor();
        BaseGimmick::Update(deltaTime);
        return;
    }

    // 演出から制御されている間は門の位置を動かさず、演出の種類に応じて接触判定だけ切り替えます。
    if (cinematicOverride_) {
        // 登場タイムラインなら、そのタイムラインが動かした位置から閉鎖位置までの差を調べる。
        if (introTimelineControl_) {
            // 見た目の移動はTimelineが担当し、接触判定だけ閉鎖位置への到達で決めます。
            const float remaining = GetTranslate().y - authoredPosition_.y;
            // 閉鎖要求があり、門が退避距離の18%以内まで下がったときに壁判定を有効にする。
            SetCollisionEnabled(targetClosure_ > 0.5f && remaining <= GetTravelDistance() * 0.18f);
        } else {
            // 登場用タイムライン以外の外部演出で動かす間は、門を通行の障害にしません。
            SetCollisionEnabled(false);
        }
        BaseGimmick::Update(deltaTime);
        return;
    }

    CaptureAuthoredState();

    // 初回は配置設定と発生済みのイベントを反映し、指定された始点へ即座に配置します。
    if (!initializedForPlay_) {
        initializedForPlay_ = true;
        targetClosure_ = activationRequested_ || (GetParameters().has_value() && GetParameters()->startActive) ? 1.0f : 0.0f;
        closure_ = targetClosure_;
        ApplyRuntimeState();
    }

    // 閉鎖率を時間に応じて目標へ近づけ、通常時の姿勢と当たり判定へ反映します。
    const float duration = GetTransitionDuration();
    const float step = duration > 0.0001f ? std::max(0.0f, deltaTime) / duration : 1.0f;

    // 閉鎖率が目標より低い場合は、目標を越えない範囲で門を下ろす。
    if (closure_ < targetClosure_) {
        closure_ = std::min(targetClosure_, closure_ + step);
    // 閉鎖率が目標より高い場合は、目標を下回らない範囲で門を上げる。
    } else if (closure_ > targetClosure_) {
        closure_ = std::max(targetClosure_, closure_ - step);
    }

    ApplyRuntimeState();
    BaseGimmick::Update(deltaTime);
}

void GimmickBossGate::OnTrigger() {
    // イベントからの起動はスイッチ ON と同じ閉鎖経路へ渡す。
    OnSwitchEvent(true);
}

void GimmickBossGate::OnSwitchEvent(bool active) {
    // シネマティック制御中は外部スイッチで門を上書きしない。
    if (cinematicOverride_)
        return;
    // OFFで元に戻す設定が無効なら、開放通知では現在の閉鎖目標を保つ。
    if (!active && GetParameters().has_value() && !GetParameters()->returnOnOff) {
        return;
    }

    CaptureAuthoredState();
    activationRequested_ = active;
    targetClosure_ = active ? 1.0f : 0.0f;

    // 閉じる要求ではまず門を表示し、開き切った状態からの開始だけ効果を出します。開ける要求では先に壁判定を解除します。
    if (active) {
        SetIsVisible(true);

        // 開放側の端点にいる門を閉じる場合だけ、閉鎖位置の少し上に予告効果を出す。
        if (closure_ <= 0.001f) {
            engine::Vector3 cue = authoredPosition_;
            cue.y += 0.1f;
            VfxSequencer::PlayOneShot("shared/high_crown_gate_close_cue", cue, {1.0f, 1.0f, 1.0f});
        }
    // 開放通知では、門が見えている間も先に通行できるよう壁判定を解除する。
    } else {
        SetCollisionEnabled(false);
    }
}

std::unique_ptr<engine::Object3D> GimmickBossGate::Clone() const {
    // 編集配置を複製し、実行時の閉鎖率は新しい門で計算する。
    auto clone = std::make_unique<GimmickBossGate>();
    assert(GetObjectCommon() != nullptr);
    clone->Initialize(GetObjectCommon(), GetModelName());
    clone->CopyFrom(this);
    return clone;
}

// 閉鎖位置を演出側に渡す。最初の取得で、配置時の位置を保存する。
engine::Vector3 GimmickBossGate::GetClosedPosition() {
    // 閉鎖位置の基準は現在の編集配置から一度だけ確定する。
    CaptureAuthoredState();
    return authoredPosition_;
}

void GimmickBossGate::SetIntroTimelineControl(bool enabled) {
    // 演出に位置を譲る前に、元の閉鎖位置を退避します。
    CaptureAuthoredState();
    introTimelineControl_ = enabled;
    cinematicOverride_ = enabled;

    // タイムライン制御を終えるときは、現在の閉鎖要求へ閉鎖率を揃えて通常更新へ渡す。
    if (!enabled) {
        // Timelineが初期姿勢へ復元した後も、封鎖済みの進行状態を保持します。
        closure_ = targetClosure_;
        ApplyRuntimeState();
    }
}

void GimmickBossGate::CaptureAuthoredState(bool force) {
    // 実行時の移動や衝突切替で、編集時の Transform とマスクを失わないよう保存する。
    // 保存済みで強制取得の指定がなければ、開放時の位置で閉鎖位置を上書きしない。
    if (authoredStateCaptured_ && !force) {
        return;
    }

    authoredPosition_ = GetTranslate();
    authoredScale_ = GetScale();
    authoredCollisionAttribute_ = GetCollisionAttribute();
    authoredCollisionMask_ = GetCollisionMask();
    // 退避した衝突属性が0なら、閉鎖時に使う地面属性を補う。
    if (authoredCollisionAttribute_ == 0)
        authoredCollisionAttribute_ = engine::collisionCategories::Get("ground");
    // 退避したマスクが0なら、全ビットを許可する既定値を補う。
    if (authoredCollisionMask_ == 0)
        authoredCollisionMask_ = 0xffffffffu;
    authoredStateCaptured_ = true;
}

void GimmickBossGate::ResetForEditor() {
    // 再生停止時は演出上書きと閉鎖率を消し、編集で決めた位置を復元する。
    cinematicOverride_ = false;
    introTimelineControl_ = false;

    // 直前まで通常再生していた場合は、退避位置を戻してから編集側の基準を確定します。
    if (initializedForPlay_) {
        SetTranslate(authoredPosition_);
        SetScale(authoredScale_);
    }

    CaptureAuthoredState(true);
    // 次の開始に閉鎖要求を持ち越さず、編集時は門を表示して壁の判定を外します。
    initializedForPlay_ = false;
    activationRequested_ = false;
    closure_ = 0.0f;
    targetClosure_ = 0.0f;
    SetTranslate(authoredPosition_);
    SetScale(authoredScale_);
    SetIsVisible(true);
    SetCollisionEnabled(false);
}

void GimmickBossGate::ApplyRuntimeState() {
    // 閉鎖率に応じて門を上下させ、十分閉じるまで壁の接触を有効にしない。
    const float eased = Easing::SmoothStep01(closure_);
    engine::Vector3 position = authoredPosition_;
    position.y += (1.0f - eased) * GetTravelDistance();
    SetTranslate(position);
    SetScale(authoredScale_);
    SetIsVisible(closure_ > 0.001f || targetClosure_ > 0.0f);
    // 閉鎖要求があり閉鎖率が0.82以上のとき、壁として通行を遮る。
    SetCollisionEnabled(targetClosure_ > 0.5f && closure_ >= 0.82f);

    // 開放を要求され、閉鎖率が0.001以下まで下がっていれば完全に非表示にする。
    if (closure_ <= 0.001f && targetClosure_ <= 0.0f) {
        SetIsVisible(false);
    }
}

void GimmickBossGate::SetCollisionEnabled(bool enabled) {
    // 編集時の属性・マスクを復元する形で壁判定を切り替える。
    SetCollisionAttribute(enabled ? authoredCollisionAttribute_ : 0);
    SetCollisionMask(enabled ? authoredCollisionMask_ : 0);
}

float GimmickBossGate::GetTransitionDuration() const {
    // 既存のmoveSpeedを所要秒数として0.12〜2.5に収める。設定がなければ0.58秒。
    return GetParameters().has_value() ? std::clamp(GetParameters()->moveSpeed, 0.12f, 2.5f) : 0.58f;
}

float GimmickBossGate::GetTravelDistance() const {
    // 閉鎖位置から上方向への退避距離。設定値の下限は2、設定がなければ8ワールド単位。
    return GetParameters().has_value() ? std::max(2.0f, GetParameters()->moveAmount) : 8.0f;
}
