#include "GimmickAppearingFloor.h"
#include "ObjectPropertyDefinition.h"
#include "CollisionConfig.h"
#include "SceneManager.h"
#include <algorithm>
#include <cmath>

void GimmickAppearingFloor::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    // 表示と当たり判定を同時に切り替える床として登録する。
    BaseGimmick::Initialize(common, modelName);

    SetClassName("Gimmick");
    SetGimmickType("AppearingFloor");
    SetName("Gimmick_AppearingFloor");
    SetCollisionAttribute(engine::collisionCategories::Get("ground"));
    SetCollisionMask(engine::collisionCategories::GetDefaultGameplayMask());
    SetColor({0.55f, 1.0f, 0.7f, 1.0f});
    SetStatic(false);

    // イベント受信後の出現時間をintervalとして登録する。
    using Self = GimmickAppearingFloor;
    engine::ObjectPropertyDefinition<Self> properties(*this, kTypeId, "出現床",
        "イベントを受けた後に出現します。0以下は従来どおり3秒として実行し、保存値自体は書き換えません。");
    using Parameter = engine::Object3D::EntityParameter;
    properties.Parameter("出現する時間", "interval", &Parameter::interval, 0.0f, 60.0f);
}

// Play中だけタイマーを進め、Editorでは足場を常に見える状態に戻します。
void GimmickAppearingFloor::Update(float deltaTime) {
    // 配置パラメーターがあれば、出現時間をそこから読み取る。
    if (GetParameters().has_value()) {
        appearDuration_ = GetParameters()->interval;
        // 設定値が0秒以下なら実行時の出現時間に3秒を使う。
        if (appearDuration_ <= 0.0f)
            appearDuration_ = 3.0f;
    }

    bool isPlaying = false;

    // シーン管理があれば、その実行状態を使う。取得できなければ編集中として扱う。
    if (SceneManager* sceneManager = SceneManager::GetInstance()) {
        isPlaying = sceneManager->IsPlaying();
    }

    // 編集に戻ったら足場を可視・有効にし、次のプレイ開始時に当たり判定を取り直します。
    if (!isPlaying) {
        initializedForPlay_ = false;
        SetFloorActive(true);
        BaseGimmick::Update(deltaTime);
        return;
    }

    // プレイへ入った最初の更新では、床の衝突属性を保存してから非表示・無効にする。
    if (!initializedForPlay_) {
        originalCollisionAttribute_ = GetCollisionAttribute();
        originalCollisionMask_ = GetCollisionMask();
        // 属性が設定されていなければ、出現時に使う地面属性を補う。
        if (originalCollisionAttribute_ == 0)
            originalCollisionAttribute_ = engine::collisionCategories::Get("ground");
        // マスクが設定されていなければ、出現時に標準のゲームプレイ用マスクを使う。
        if (originalCollisionMask_ == 0)
            originalCollisionMask_ = engine::collisionCategories::GetDefaultGameplayMask();

        visibleTimer_ = 0.0f;
        SetFloorActive(false);
        initializedForPlay_ = true;
    }

    // 秒単位で有効時間を減らす。点滅中もタイマーが残る間は衝突を有効に保ちます。
    if (visibleTimer_ > 0.0f) {
        visibleTimer_ -= deltaTime;
        // 今フレームで表示時間を使い切った場合は0秒で止める。
        if (visibleTimer_ < 0.0f)
            visibleTimer_ = 0.0f;
    }

    bool shouldBeActive = visibleTimer_ > 0.0f;
    SetFloorActive(shouldBeActive);

    // 有効時間が残っていれば、消滅前の点滅表示を判定する。
    if (shouldBeActive) {
        float warningTime = (std::min)(blinkWarningTime_, appearDuration_ * 0.5f);

        // 残りが予告時間以内なら表示だけを点滅させ、床の衝突は保持する。
        if (visibleTimer_ <= warningTime) {
            bool blinkOn = std::fmod(visibleTimer_, blinkInterval_ * 2.0f) >= blinkInterval_;
            SetIsVisible(blinkOn);
            SetColor(blinkOn ? engine::Vector4{0.9f, 1.0f, 0.35f, 1.0f} : engine::Vector4{0.55f, 1.0f, 0.7f, 1.0f});
        // 予告時間に入るまでは、常時表示して通常色を使う。
        } else {
            SetIsVisible(true);
            SetColor({0.55f, 1.0f, 0.7f, 1.0f});
        }
    }

    BaseGimmick::Update(deltaTime);
}

void GimmickAppearingFloor::OnTrigger() {
    // 再起動時は表示時間を延長し、床を再び有効にする。
    visibleTimer_ = appearDuration_;
    SetFloorActive(true);
    SetIsVisible(true);
}

void GimmickAppearingFloor::SetFloorActive(bool active) {
    // 実行状態と表示の両方が要求に一致していれば、衝突と表示を設定し直さない。
    if (isActive_ == active && GetIsVisible() == active)
        return;

    isActive_ = active;
    SetIsVisible(active);

    // 出現時は保存した属性とマスクを戻し、未設定値には既定を使う。
    if (active) {
        SetCollisionAttribute(originalCollisionAttribute_ != 0 ? originalCollisionAttribute_ : engine::collisionCategories::Get("ground"));
        SetCollisionMask(originalCollisionMask_ != 0 ? originalCollisionMask_ : engine::collisionCategories::GetDefaultGameplayMask());
        SetColor({0.55f, 1.0f, 0.7f, 1.0f});
    // 消える場合は、見えなくなった足場に触れないよう属性とマスクを0にする。
    } else {
        SetCollisionAttribute(0);
        SetCollisionMask(0);
    }
}

std::unique_ptr<engine::Object3D> GimmickAppearingFloor::Clone() const {
    // 新しい実体を初期化してから、出現時間と配置を引き継ぐ。
    auto newObj = std::make_unique<GimmickAppearingFloor>();
    assert(GetObjectCommon() != nullptr);
    newObj->Initialize(GetObjectCommon(), GetModelName());
    newObj->CopyFrom(this);
    return newObj;
}
