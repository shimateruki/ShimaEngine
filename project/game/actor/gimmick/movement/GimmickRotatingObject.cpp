#include "GimmickRotatingObject.h"
#include <numbers>
#include "ObjectPropertyDefinition.h"
#include "CollisionConfig.h"
#include "GhostRecorder.h"
#include "BaseScene.h"
#include "SceneManager.h"
#include <algorithm>
#include <cmath>

void GimmickRotatingObject::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    // 回転軸と速度の旧既定値を用意し、地形として足場判定を有効にする。
    BaseGimmick::Initialize(common, modelName);

    SetClassName("Gimmick");
    SetGimmickType("RotatingFloor");
    SetName("Gimmick_RotatingFloor");
    SetCollisionAttribute(engine::collisionCategories::Get("ground"));
    SetCollisionMask(engine::collisionCategories::GetDefaultGameplayMask());
    SetColor({0.95f, 0.65f, 0.35f, 1.0f});
    SetScale({3.0f, 0.3f, 1.2f});
    SetStatic(false);

    // 配置用パラメーターが無ければ、回転軸や速度の格納先を用意する。
    if (!GetParameters().has_value())
        ResetParameters();
    SetParameter(&engine::EntityParameter::speed, 45.0f);
    SetParameter(&engine::EntityParameter::actionMode, 1);
    SetParameter(&engine::EntityParameter::startActive, true);
    SetParameter(&engine::EntityParameter::returnOnOff, true);

    // 回転軸、度毎秒の速度、開始時の有効状態、OFF入力の扱いを編集項目に登録する。
    using Self = GimmickRotatingObject;
    engine::ObjectPropertyDefinition<Self> properties(*this, kTypeId, "回転床・回転柱",
        "スイッチ連動で回転の開始・停止ができます。回転床と回転柱は同じクラスで動きます。");
    using Parameter = engine::Object3D::EntityParameter;
    properties.Parameter("回転軸", "actionMode", &Parameter::actionMode, 0.0f, 2.0f,
        nullptr, {"X", "Y", "Z"});
    properties.Parameter("回転速度（度/秒）", "speed", &Parameter::speed, -720.0f, 720.0f,
        nullptr, {}, {.speed = 1.0f});
    properties.Parameter("開始時に有効", "startActive", &Parameter::startActive);
    properties.Parameter("OFFで元に戻す", "returnOnOff", &Parameter::returnOnOff);
}

// プレイ中だけ指定軸を自転させる。床への追従または記録経路があれば、その姿勢に自転を重ねます。
void GimmickRotatingObject::Update(float deltaTime) {
    // 配置にパラメーターがなければ、プレイ判定に先立って用意する。
    if (!GetParameters().has_value())
        ResetParameters();

    bool isPlaying = false;

    // シーン管理を参照できる場合だけ、プレイ中か調べる。
    if (SceneManager* sceneManager = SceneManager::GetInstance()) {
        isPlaying = sceneManager->IsPlaying();
    }

    // 床追従で変わった姿勢・表示・衝突を配置時の値へ戻し、次のプレイで床を取得し直します。
    if (!isPlaying) {
        // 追従先の床があった場合は、追従で変わった姿勢・表示・衝突を戻す。
        if (supportFloor_) {
            SetTranslate(startPosition_);
            SetEulerAngles(startRotation_);
            SetIsVisible(initialVisible_);
            SetCollisionAttribute(initialCollisionAttribute_);
            supportFloor_ = nullptr;
        }
        initializedForPlay_ = false;
        pathRotationOffset_ = 0.0f;
        BaseGimmick::Update(deltaTime);
        return;
    }

    // 最初のプレイ更新で開始時の有効状態を取り込み、追従を終えるときに戻す姿勢・表示・衝突を控える。
    if (!initializedForPlay_) {
        active_ = GetParameters()->startActive;
        pathRotationOffset_ = 0.0f;
        startPosition_ = GetTranslate();
        startRotation_ = GetRotation();
        initialCollisionAttribute_ = GetCollisionAttribute();
        initialVisible_ = GetIsVisible();
        initializedForPlay_ = true;
    }

    // Recorderの移動姿勢を先に確定し、経路を持つ物の自転を上書きされないようにする。
    BaseGimmick::Update(deltaTime);

    // 宛先IDが設定され、追従先が未確定の場合だけ床を探す。
    if (GetTargetId() >= 0 && !supportFloor_) {
        // 現在のシーンがあれば、そのオブジェクト一覧を調べる。
        if (auto* scene = SceneManager::GetInstance()->GetCurrentScene()) {
            for (const auto& object : scene->GetObjects()) {
                // 自分以外の実体でイベントIDが一致したとき、追従先に決める。
                if (object && object.get() != this && object->GetEventId() == GetTargetId()) {
                    supportFloor_ = object.get();
                    supportFloor_->UpdateLocalMatrix();
                    supportFloor_->UpdateWorldMatrix(false);
                    // ワールド位置を床のローカル座標へ移し、床が傾いても同じ取り付け位置を使います。
                    supportLocalPosition_ = engine::Math::Transform(
                        GetWorldPosition(), engine::Math::Inverse(supportFloor_->GetWorldMatrix()));
                    supportStartRotation_ = supportFloor_->GetRotation();
                    break;
                }
            }
        }
    }

    // 速度の保存単位は度/秒です。秒経過を掛けてこのフレームの回転量をラジアンで求めます。
    const float step = GetParameters()->speed * std::numbers::pi_v<float> / 180.0f * (std::max)(0.0f, deltaTime);
    const bool playingPath = GetRecorder() && GetRecorder()->GetState() == GhostRecorder::State::kPlaying &&
                             !GetRecordPathName().empty();
    // 床に対する相対位置を保ち、床の角度変化と自転を重ねます。床が消えれば表示と衝突も連動します。
    if (supportFloor_) {
        // 追従中も回転が有効の場合に限り、累積角を進める。
        if (active_)
            pathRotationOffset_ = std::fmod(pathRotationOffset_ + step, 2.0f * std::numbers::pi_v<float>);
        supportFloor_->UpdateWorldMatrix(false);
        SetTranslate(engine::Math::Transform(supportLocalPosition_, supportFloor_->GetWorldMatrix()));
        const auto floorRotation = supportFloor_->GetRotation();
        engine::Vector3 rotation = {
            startRotation_.x + floorRotation.x - supportStartRotation_.x,
            startRotation_.y + floorRotation.y - supportStartRotation_.y,
            startRotation_.z + floorRotation.z - supportStartRotation_.z};
        // X軸指定なら、床の角度補正後のXに自転を足す。
        if (GetParameters()->actionMode == 0) rotation.x += pathRotationOffset_;
        // Z軸指定なら、補正後のZに自転を足す。
        else if (GetParameters()->actionMode == 2) rotation.z += pathRotationOffset_;
        // それ以外は、補正後のYに自転を足す。
        else rotation.y += pathRotationOffset_;
        SetEulerAngles(rotation);
        SetIsVisible(initialVisible_ && supportFloor_->GetIsVisible());
        SetCollisionAttribute(supportFloor_->GetCollisionAttribute() ? initialCollisionAttribute_ : 0);
        UpdateLocalMatrix();
        UpdateWorldMatrix(false);
    // 記録の姿勢は毎フレーム上書きされるため、１フレーム分でなく累積した自転角を足します。
    } else if (playingPath) {
        // 記録経路で移動中でも、自転を有効にしているときだけ累積角を進める。
        if (active_) {
            pathRotationOffset_ = std::fmod(pathRotationOffset_ + step, 2.0f * std::numbers::pi_v<float>);
        }
        engine::Vector3 rot = GetRotation();
        // X軸を選んだ場合は経路の姿勢のXに累積角を加える。
        if (GetParameters()->actionMode == 0) rot.x += pathRotationOffset_;
        // Z軸を選んだ場合は経路の姿勢のZに累積角を加える。
        else if (GetParameters()->actionMode == 2) rot.z += pathRotationOffset_;
        // それ以外は経路の姿勢のYに累積角を加える。
        else rot.y += pathRotationOffset_;
        SetEulerAngles(rot);
    // 追従床も再生中の経路も無く、回転が有効ならその場で回す。
    } else if (active_) {
        engine::Vector3 rot = GetRotation();
        // X軸指定なら現在のXに1フレーム分を足す。
        if (GetParameters()->actionMode == 0) rot.x += step;
        // Z軸指定なら現在のZに1フレーム分を足す。
        else if (GetParameters()->actionMode == 2) rot.z += step;
        // それ以外は現在のYに1フレーム分を足す。
        else rot.y += step;
        SetEulerAngles(rot);
    }
}

void GimmickRotatingObject::OnTrigger() {
    // トリガーはスイッチの ON と同じ経路で扱う。
    OnSwitchEvent(true);
}

// OFF を受け付ける設定なら回転の更新を停止する。現在の角度はその場でリセットしない。
void GimmickRotatingObject::OnSwitchEvent(bool active) {
    // 配置に設定が無ければ、スイッチ入力を処理する前に共通パラメーターを用意する。
    if (!GetParameters().has_value())
        ResetParameters();
    // OFFの通知を無視する設定なら、現在の回転状態を保つ。
    if (!active && !GetParameters()->returnOnOff)
        return;
    active_ = active;
}

std::unique_ptr<engine::Object3D> GimmickRotatingObject::Clone() const {
    // 回転開始時刻は複製せず、保存済みの配置と設定だけ引き継ぐ。
    auto newObj = std::make_unique<GimmickRotatingObject>();
    assert(GetObjectCommon() != nullptr);
    newObj->Initialize(GetObjectCommon(), GetModelName());
    newObj->CopyFrom(this);
    return newObj;
}
