#include <cstdint>
#include "GimmickPhaseFlipFloor.h"
#include "Easing.h"
#include <numbers>
#include "ObjectPropertyDefinition.h"
#include "CollisionConfig.h"
#include "SceneManager.h"
#include <algorithm>
#include <cmath>

void GimmickPhaseFlipFloor::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    BaseGimmick::Initialize(common, modelName);

    // 床の衝突を維持したまま、色別の順番で回転する初期値を用意します。
    SetClassName("Gimmick");
    SetGimmickType("PhaseFlipFloor");
    SetName("Gimmick_PhaseFlipFloor");
    SetCollisionAttribute(engine::collisionCategories::Get("ground"));
    SetCollisionMask(engine::collisionCategories::GetDefaultGameplayMask());
    SetColor({0.35f, 0.75f, 1.0f, 1.0f});
    SetScale({2.0f, 0.25f, 2.0f});
    SetStatic(false);

    // 配置用パラメーターが無ければ床番号と周期の格納先を用意する。
    if (!GetParameters().has_value())
        ResetParameters();
    SetParameter(&engine::EntityParameter::colorType, 0);
    SetParameter(&engine::EntityParameter::maxCount, 3);
    SetParameter(&engine::EntityParameter::interval, 1.0f);
    SetParameter(&engine::EntityParameter::startActive, true);

    // 床番号、床の総数、1フェーズの時間、回転方向を登録する。
    using Self = GimmickPhaseFlipFloor;
    engine::ObjectPropertyDefinition<Self> properties(*this, kTypeId, "順番反転床",
        "床番号1→2→3…の順に、当たり判定を残したまま180度回転します。");
    using Parameter = engine::Object3D::EntityParameter;

    // 表示だけ1始まりにし、従来のcolorType（0始まり）をそのまま保存します。
    properties.Accessor<std::int32_t>("床番号", "colorType", "param.colorType",
        [](const Self& object) { return object.GetParameters() ? object.GetParameters()->colorType : 0; },
        [](Self& object, const std::int32_t& value) {
            // パラメーターが無ければ、床番号を変更する前に作成する。
            if (!object.GetParameters()) object.ResetParameters();
            object.SetParameter(&engine::EntityParameter::colorType, std::clamp(value, 0, (std::max)(1, object.GetParameters()->maxCount) - 1));
        }, 0, 15, nullptr, {}, {.integerDisplayOffset = 1, .maximum = +[](const Self& object) {
            return static_cast<float>(object.GetParameters() ? (std::max)(1, object.GetParameters()->maxCount) - 1 : 0);
        }});
    properties.Accessor<std::int32_t>("全体の床数", "maxCount", "param.maxCount",
        [](const Self& object) { return object.GetParameters() ? object.GetParameters()->maxCount : 3; },
        [](Self& object, const std::int32_t& value) {
            // パラメーターが無ければ、床数と番号を編集する前に作成する。
            if (!object.GetParameters()) object.ResetParameters();
            object.SetParameter(&engine::EntityParameter::maxCount, std::clamp(value, 1, 16));
            object.SetParameter(&engine::EntityParameter::colorType, std::clamp(object.GetParameters()->colorType, 0, object.GetParameters()->maxCount - 1));
        }, 1, 16);
    properties.Parameter("1フェーズの時間", "interval", &Parameter::interval, 0.1f, 30.0f);
    properties.Parameter("正方向に回転", "startActive", &Parameter::startActive);
}

// 周期から現在の位相を求め、自分の番だけ床を半回転させる。物理判定は全区間で維持する。
void GimmickPhaseFlipFloor::Update(float deltaTime) {
    // パラメーターが無い配置は、位相を調べる前に格納先を用意する。
    if (!GetParameters().has_value())
        ResetParameters();

    bool isPlaying = false;

    // シーン管理が利用できる場合は、プレイ中かを調べる。
    if (SceneManager* sceneManager = SceneManager::GetInstance()) {
        isPlaying = sceneManager->IsPlaying();
    }

    // 再生を止めると周期の履歴を破棄し、エディターで選べる床表示へ戻す。
    if (!isPlaying) {
        initializedForPlay_ = false;
        cycleTimer_ = 0.0f;
        flipCount_ = 0;
        wasOwnPhase_ = false;
        SetCollisionAttribute(engine::collisionCategories::Get("ground"));
        SetCollisionMask(engine::collisionCategories::GetDefaultGameplayMask());
        SetIsVisible(true);
        SetColor(GetPhaseColor(1.0f));
        BaseGimmick::Update(deltaTime);
        return;
    }

    // 最初の再生フレームに、配置された向きと回転後も使う衝突設定を保存する。
    if (!initializedForPlay_) {
        originalCollisionAttribute_ = GetCollisionAttribute();
        originalCollisionMask_ = GetCollisionMask();
        // 元の属性が未設定なら、回転中に保つ地面属性を補う。
        if (originalCollisionAttribute_ == 0)
            originalCollisionAttribute_ = engine::collisionCategories::Get("ground");
        // 元のマスクが未設定なら、標準のゲームプレイ用マスクを使う。
        if (originalCollisionMask_ == 0)
            originalCollisionMask_ = engine::collisionCategories::GetDefaultGameplayMask();
        baseRotation_ = GetTransform()->rotate;
        cycleTimer_ = 0.0f;
        flipCount_ = 0;
        wasOwnPhase_ = false;
        initializedForPlay_ = true;
    }

    cycleTimer_ += deltaTime;

    // 設定された位相数と1フェーズの秒数から、すべての床の順番が一巡する長さを求める。
    const std::int32_t phaseCount = GetPhaseCount();
    const float phaseDuration = GetPhaseDuration();
    const float cycleLength = phaseDuration * static_cast<float>(phaseCount);

    // 一周期分の秒数に達したら、周期内の余りに戻す。
    if (cycleLength > 0.0f && cycleTimer_ >= cycleLength) {
        cycleTimer_ = std::fmod(cycleTimer_, cycleLength);
    }

    // 自分の位相へ入るたびに半回転を一回加え、途中経過は滑らかに補間します。
    const std::int32_t currentPhase = static_cast<std::int32_t>(cycleTimer_ / phaseDuration) % phaseCount;
    // 現在の位相で経過した秒数だけを取り出し、位相の長さで割って半回転の進み具合に使います。
    const float phaseProgress = std::fmod(cycleTimer_, phaseDuration) / phaseDuration;
    const bool isOwnPhase = currentPhase == GetPhaseIndex();

    // 前回は別の位相で今回自分の番になった場合だけ、半回転数を増やす。
    if (isOwnPhase && !wasOwnPhase_) {
        flipCount_++;
    }

    wasOwnPhase_ = isOwnPhase;

    // 現在の位相に対応する回転と表示を反映してから、共通のオブジェクト更新を行う。
    ApplyState(isOwnPhase, phaseProgress);
    BaseGimmick::Update(deltaTime);
}

void GimmickPhaseFlipFloor::ApplyState(bool isOwnPhase, float phaseProgress) {
    // 回転中も元の衝突設定を維持し、床が抜けないようにします。
    SetCollisionAttribute(originalCollisionAttribute_ != 0 ? originalCollisionAttribute_ : engine::collisionCategories::Get("ground"));
    SetCollisionMask(originalCollisionMask_ != 0 ? originalCollisionMask_ : engine::collisionCategories::GetDefaultGameplayMask());
    SetIsVisible(true);

    // 床番号に対応する位相中だけ、済んだ半回転数から今回の終点へ補間します。
    const float direction = GetParameters().has_value() && !GetParameters()->startActive ? -1.0f : 1.0f;
    float turn = static_cast<float>(flipCount_);

    // 自分の番では前回までの半回転数に、今の区間の進み具合を足す。
    if (isOwnPhase) {
        turn = static_cast<float>((std::max)(0, flipCount_ - 1)) + SmoothStep(phaseProgress);
    }

    SetColor(GetPhaseColor(1.0f));

    // 半回転数をラジアンへ換算して配置時のX軸回転に加え、オイラー角を描画に使います。
    engine::Vector3 rot = baseRotation_;
    rot.x += direction * turn * std::numbers::pi_v<float>;
    SetEulerAngles(rot);
    SetQuaternionMaster(false);
}

engine::Vector4 GimmickPhaseFlipFloor::GetPhaseColor(float alpha) const {
    // 床番号を4色で繰り返し、渡されたアルファを全色に反映する。
    switch (GetPhaseIndex() % 4) {
    // 4で割った余りが0なら水色を使う。
    case 0:
        return {0.35f, 0.75f, 1.0f, alpha};

    // 余りが1なら橙色を使う。
    case 1:
        return {1.0f, 0.65f, 0.25f, alpha};

    // 余りが2なら緑色を使う。
    case 2:
        return {0.45f, 1.0f, 0.45f, alpha};

    // 残りの位相には紫色を使う。
    default:
        return {0.95f, 0.55f, 1.0f, alpha};
    }
}

std::int32_t GimmickPhaseFlipFloor::GetPhaseIndex() const {
    // 設定値が位相数を超えても存在する位相へ収めます。
    std::int32_t phaseCount = GetPhaseCount();
    std::int32_t index = GetParameters().has_value() ? GetParameters()->colorType : 0;
    return std::clamp(index, 0, phaseCount - 1);
}

std::int32_t GimmickPhaseFlipFloor::GetPhaseCount() const {
    // ゼロで割らないよう、周期の位相数は一以上にします。
    std::int32_t count = GetParameters().has_value() ? GetParameters()->maxCount : 3;
    return (std::max)(1, count);
}

float GimmickPhaseFlipFloor::GetPhaseDuration() const {
    // 時間設定がゼロでも位相が進むよう、最短時間を設けます。
    float duration = GetParameters().has_value() ? GetParameters()->interval : 1.0f;
    return (std::max)(0.1f, duration);
}

float GimmickPhaseFlipFloor::SmoothStep(float t) const {
    // 補間曲線と入力範囲の扱いを共通のEasingへ委譲する。
    return Easing::SmoothStep01(t);
}

std::unique_ptr<engine::Object3D> GimmickPhaseFlipFloor::Clone() const {
    // 複製先の基本資源を作ってから配置と周期設定を移します。
    auto newObj = std::make_unique<GimmickPhaseFlipFloor>();
    assert(GetObjectCommon() != nullptr);
    newObj->Initialize(GetObjectCommon(), GetModelName());
    newObj->CopyFrom(this);
    return newObj;
}
