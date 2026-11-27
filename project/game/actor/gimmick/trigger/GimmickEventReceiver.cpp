#include <cstdint>
#include "GimmickEventReceiver.h"
#include "Easing.h"
#include <numbers>
#include "ObjectPropertyDefinition.h"
#include "CollisionConfig.h"
#include "SceneManager.h"
#include <algorithm>
#include <cmath>

// スイッチと同じイベントIDを持つ受信側ギミックを初期化し、モード別の編集項目を登録する。
void GimmickEventReceiver::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    BaseGimmick::Initialize(common, modelName);

    // 移動できる地形の既定属性を設定する。プレイ開始後に使う基準姿勢と衝突設定は最初の Update() で控える。
    SetClassName("Gimmick");
    SetGimmickType("EventReceiver");
    SetName("Gimmick_EventReceiver");
    SetCollisionAttribute(engine::collisionCategories::Get("ground"));
    SetCollisionMask(engine::collisionCategories::GetDefaultGameplayMask());
    SetColor({0.65f, 1.0f, 0.65f, 1.0f});
    SetStatic(false);

    // 保存データから設定が渡されていなければ、編集項目が参照できる既定パラメーターを用意する。
    if (!GetParameters().has_value())
        ResetParameters();

    // 動作モードで編集欄を切り替える。王冠の移動距離と回転速度には、軸移動と同じ保存項目を使う。
    using Self = GimmickEventReceiver;
    engine::ObjectPropertyDefinition<Self> properties(*this, kTypeId, "イベント受信ギミック",
        "My Event IDとスイッチのTarget IDを合わせます。王冠落下は編集時は非表示で、撃破通知後にボス頭上から着地点へ落下します。");
    using Parameter = engine::Object3D::EntityParameter;
    properties.Parameter("動作モード", "actionMode", &Parameter::actionMode, 0.0f, 6.0f,
        nullptr, {"出現", "Y方向に移動", "X方向に移動", "Z方向に移動", "有効化", "無効化", "ボス王冠落下"});
    properties.Parameter("移動量", "moveAmount", &Parameter::moveAmount, -500.0f, 500.0f,
        +[](const Self& object) { return object.GetParameters() && (object.GetParameters()->actionMode >= 1 && object.GetParameters()->actionMode <= 3); });
    properties.Parameter("移動速度", "moveSpeed", &Parameter::moveSpeed, 0.1f, 60.0f,
        +[](const Self& object) { return object.GetParameters() && (object.GetParameters()->actionMode >= 1 && object.GetParameters()->actionMode <= 3); });
    properties.Parameter("ボスから着地点までの距離", "moveAmount_crown", &Parameter::moveAmount, 0.0f, 80.0f,
        +[](const Self& object) { return object.GetParameters() && (object.GetParameters()->actionMode == 6); }, {}, {.parameterId = "moveAmount"});
    properties.Parameter("落下演出時間", "fallDuration", &Parameter::fallDuration, 0.35f, 6.0f,
        +[](const Self& object) { return object.GetParameters() && (object.GetParameters()->actionMode == 6); });
    properties.Parameter("弧の高さ", "jumpPower", &Parameter::jumpPower, 0.0f, 12.0f,
        +[](const Self& object) { return object.GetParameters() && (object.GetParameters()->actionMode == 6); });
    properties.Parameter("王冠の回転速度", "moveSpeed_crown", &Parameter::moveSpeed, -20.0f, 20.0f,
        +[](const Self& object) { return object.GetParameters() && (object.GetParameters()->actionMode == 6); }, {}, {.parameterId = "moveSpeed"});
    properties.Parameter("開始時に有効", "startActive", &Parameter::startActive);
    properties.Parameter("OFFで元に戻す", "returnOnOff", &Parameter::returnOnOff);
}

// 編集時の表示を管理し、再生時は最後に受け取ったイベントの状態で移動や落下を進める。
void GimmickEventReceiver::Update(float deltaTime) {
    // 保存されたパラメーターがない場合も、動作モードを参照できるよう既定値を用意する。
    if (!GetParameters().has_value())
        ResetParameters();

    bool isPlaying = false;

    // シーン管理が取得できるときだけ再生状態を問い合わせる。
    if (SceneManager* sceneManager = SceneManager::GetInstance()) {
        isPlaying = sceneManager->IsPlaying();
    }

    // 編集に戻ったら保留中のイベントを消し、次の再生で配置状態を取り直す。
    if (!isPlaying) {
        initializedForPlay_ = false;
        hasPendingActive_ = false;
        const bool isBossReward = GetParameters()->actionMode == 6;
        SetIsVisible(!isBossReward);
        SetCollisionEnabled(!isBossReward);
        BaseGimmick::Update(deltaTime);
        return;
    }

    // 最初の再生フレームで元の見た目と衝突設定を記録します。
    if (!initializedForPlay_) {
        basePosition_ = GetTransform()->translate;
        targetPosition_ = basePosition_;
        baseScale_ = GetScale();
        baseRotation_ = GetRotation();
        originalCollisionAttribute_ = GetCollisionAttribute();
        originalCollisionMask_ = GetCollisionMask();
        originalColor_ = GetColor();
        // 編集時に衝突を無効にしていた場合は、再生中の有効化で使う地形属性を既定値で補う。
        if (originalCollisionAttribute_ == 0)
            originalCollisionAttribute_ = engine::collisionCategories::Get("ground");
        // 保存されたマスクが0の場合は、再生中に衝突させる相手を既定マスクで補う。
        if (originalCollisionMask_ == 0)
            originalCollisionMask_ = engine::collisionCategories::GetDefaultGameplayMask();

        // 初回更新より先に届いたイベントを優先し、受信していない場合は設定値から始める。
        active_ = hasPendingActive_ ? pendingActive_ : GetParameters()->startActive;
        hasPendingActive_ = false;
        ApplyActiveState(active_);
        initializedForPlay_ = true;
    }

    // 表示と衝突を切り替えるモードは ApplyActiveState() で処理済み。時間で変わる位置と姿勢をここで更新する。
    std::int32_t actionMode = GetParameters()->actionMode;

    // 軸移動は受信時に設定した位置へ近づける。補間率はフレーム時間に応じて求める。
    if (actionMode >= 1 && actionMode <= 3) {
        float speed = (std::max)(0.01f, GetParameters()->moveSpeed);
        float t = 1.0f - std::exp(-speed * deltaTime);
        engine::Vector3 pos = GetTransform()->translate;
        pos.x = engine::Math::Lerp(pos.x, targetPosition_.x, t);
        pos.y = engine::Math::Lerp(pos.y, targetPosition_.y, t);
        pos.z = engine::Math::Lerp(pos.z, targetPosition_.z, t);
        SetTranslate(pos);
    // モード6が有効な場合だけ、王冠の移動と取得判定の時計を進める。
    } else if (actionMode == 6 && active_) {
        // 王冠を上方の開始位置から着地点へ補間し、途中の弧はサイン波で加える。
        activationTimer_ += (std::max)(0.0f, deltaTime);
        const float duration = (std::max)(0.35f, GetParameters()->fallDuration);
        const float progress = (std::clamp)(activationTimer_ / duration, 0.0f, 1.0f);
        const float eased = Easing::SmoothStep(progress);
        // 着地点を基準にY方向へ4.05、Z方向へ距離設定分だけ手前から始め、途中にサイン波の高さを加える。
        const engine::Vector3 startPosition = basePosition_ + engine::Vector3{0.0f, 4.05f, -GetParameters()->moveAmount};
        engine::Vector3 position = startPosition * (1.0f - eased) + basePosition_ * eased;
        position.y += std::sin(progress * std::numbers::pi_v<float>) * (std::max)(0.0f, GetParameters()->jumpPower);
        SetTranslate(position);

        // 配置時の回転に、経過時間に比例するY回転と途中だけのZ傾きを重ねる。
        engine::Vector3 rotation = baseRotation_;
        rotation.y += activationTimer_ * GetParameters()->moveSpeed;
        rotation.z += std::sin(progress * std::numbers::pi_v<float>) * 0.52f;
        SetRotation(rotation);

        // 倍率を1.55から1へ戻し、所定時間に達したフレームから衝突を有効にする。
        const float startScale = 1.55f;
        const float scaleRate = startScale + (1.0f - startScale) * eased;
        SetScale(baseScale_ * scaleRate);
        SetCollisionEnabled(progress >= 1.0f);
    }

    BaseGimmick::Update(deltaTime);
}

void GimmickEventReceiver::RelocateRuntimeBase(const engine::Vector3& position) {
    // 実行中の相対移動量を保ったまま、演出全体の基準位置だけを移します。
    const engine::Vector3 offset = position - (initializedForPlay_ ? basePosition_ : GetTranslate());
    basePosition_ = position;
    targetPosition_ = initializedForPlay_ ? targetPosition_ + offset : position;
    SetTranslate(GetTranslate() + offset);
    UpdateWorldMatrix(true);
}

void GimmickEventReceiver::SetCinematicRewardOwned(bool owned) {
    cinematicRewardOwned_ = owned;
    // 演出へ姿勢を渡す際に、先に届いていた落下要求も破棄する。
    if (owned) {
        active_ = false;
        hasPendingActive_ = false;
    }
}

// 単発トリガーはスイッチのオンイベントとして扱います。
void GimmickEventReceiver::OnTrigger() {
    OnSwitchEvent(true);
}

void GimmickEventReceiver::OnSwitchEvent(bool active) {
    // 撃破完了通知が届いても、ムービーで着地した王冠を落下開始位置へ戻さない。
    if (cinematicRewardOwned_) return;

    // 初回更新前にスイッチが届いた場合も、OFF を受け入れる設定を参照できるようにする。
    if (!GetParameters().has_value())
        ResetParameters();

    // オフで元に戻す設定が無効なら、解除通知では現在の状態を保つ。
    if (!active && !GetParameters()->returnOnOff)
        return;
    // 再生時の基準姿勢をまだ保存していなければ、最後の通知を初回Updateまで保留する。
    if (!initializedForPlay_) {
        pendingActive_ = active;
        hasPendingActive_ = true;
        return;
    }

    ApplyActiveState(active);
}

void GimmickEventReceiver::ApplyActiveState(bool active) {
    // モード別に表示・衝突・移動先を切り替え、報酬出現時は着地まで衝突を遅らせます。
    active_ = active;
    activationTimer_ = 0.0f;
    std::int32_t actionMode = GetParameters().has_value() ? GetParameters()->actionMode : 0;
    float moveAmount = GetParameters().has_value() ? GetParameters()->moveAmount : 10.0f;

    // 出現モードは通知に合わせて表示と衝突を切り替え、控えた色も復元する。
    if (actionMode == 0) {
        SetIsVisible(active);
        SetCollisionEnabled(active);

        // エディタで設定した色を尊重し、報酬王冠などを汎用緑色へ上書きしません。
        SetColor(originalColor_);
    // 軸移動は移動先だけ設定し、実際の位置は Update() が時間をかけて動かします。
    } else if (actionMode >= 1 && actionMode <= 3) {
        engine::Vector3 offset{};
        // モード1では配置時の位置からY軸へ移動する。
        if (actionMode == 1)
            offset.y = moveAmount;
        // モード2では配置時の位置からX軸へ移動する。
        if (actionMode == 2)
            offset.x = moveAmount;
        // モード3では配置時の位置からZ軸へ移動する。
        if (actionMode == 3)
            offset.z = moveAmount;
        // オンでは設定した軸の先を目標にし、オフでは元の配置に戻す。
        targetPosition_ = active ? basePosition_ + offset : basePosition_;
        SetIsVisible(true);
        SetCollisionEnabled(true);
    // 有効化モードは通知に合わせて表示と衝突を切り替える。
    } else if (actionMode == 4) {
        SetIsVisible(active);
        SetCollisionEnabled(active);
    // 無効化モードはオン通知で隠し、オフ通知で再表示する。
    } else if (actionMode == 5) {
        SetIsVisible(!active);
        SetCollisionEnabled(!active);
    // 王冠を演出開始位置へ置きます。着地までの時間と衝突の切り替えは毎フレーム更新側で扱います。
    } else if (actionMode == 6) {
        SetScale(baseScale_);
        SetRotation(baseRotation_);

        // オン通知では王冠を演出位置に置く。床やプレイヤーとの衝突はUpdate()の演出終了まで止める。
        if (active) {
            SetTranslate(basePosition_ + engine::Vector3{0.0f, 4.05f, -moveAmount});
            SetIsVisible(true);
            SetCollisionEnabled(false);
        // オフ通知では王冠を着地点に戻し、表示と衝突を止める。
        } else {
            SetTranslate(basePosition_);
            SetIsVisible(false);
            SetCollisionEnabled(false);
        }
    }
}

void GimmickEventReceiver::SetCollisionEnabled(bool enabled) {
    // 衝突を有効にする場合は控えた属性とマスクを戻す。ゼロの場合には再生用の既定値を補う。
    if (enabled) {
        SetCollisionAttribute(originalCollisionAttribute_ != 0 ? originalCollisionAttribute_ : engine::collisionCategories::Get("ground"));
        SetCollisionMask(originalCollisionMask_ != 0 ? originalCollisionMask_ : engine::collisionCategories::GetDefaultGameplayMask());
    // 無効化時は属性とマスクの両方を0にして、他オブジェクトとの衝突対象から外す。
    } else {
        SetCollisionAttribute(0);
        SetCollisionMask(0);
    }
}

std::unique_ptr<engine::Object3D> GimmickEventReceiver::Clone() const {
    // 複製先を初期化してから、イベント設定と配置をコピーします。
    auto newObj = std::make_unique<GimmickEventReceiver>();
    assert(GetObjectCommon() != nullptr);
    newObj->Initialize(GetObjectCommon(), GetModelName());
    newObj->CopyFrom(this);
    return newObj;
}
