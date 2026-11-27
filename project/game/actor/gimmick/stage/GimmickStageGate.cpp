#include <cstdint>
#include "GimmickStageGate.h"
#include "Easing.h"
#include "ObjectPropertyDefinition.h"
#include "CollisionConfig.h"
#include "SceneManager.h"
#include "Player.h"
#include "PostEffect.h"
#include "game/system/progress/StageManager.h"
#include <algorithm>
#include <cassert>
#include <cmath>

namespace {
constexpr std::int32_t kGateModeStageSelectNode = 0;
constexpr std::int32_t kGateModeSceneTransition = 1;
constexpr std::int32_t kGateModeStageTransition = 2;
constexpr std::int32_t kGatePortalMaterialType = 22;

// ゲート演出の進行率を0～1へ制限します。
float Clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

// ゲート演出の二値を進行率で線形補間します。
float Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

// 指定した開始・終了位置の間で滑らかなゲート進行率を返します。
float Smoothstep(float edge0, float edge1, float value) {
    // 閾値の重複時もゼロ除算せず、境界で滑らかに変化させます。
    const float rawWidth = edge1 - edge0;
    const float width = rawWidth > 0.0001f ? rawWidth : 0.0001f;
    const float t = Clamp01((value - edge0) / width);
    return Easing::SmoothStep(t);
}
} // namespace

// ゲートのポータル表示と進入トリガーを設定し、3種類の動作モードを編集可能にする。
void GimmickStageGate::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    BaseGimmick::Initialize(common, modelName);

    // 未設定の遷移先はステージ選択にし、ポータル専用材質を割り当てます。
    SetClassName("Gimmick");
    SetGimmickType("StageGate");
    SetName("Gimmick_StageGate");

    // 旧シーンなどでパラメーターがなければ、遷移先の既定値を書き込めるようにする。
    if (!GetParameters().has_value()) {
        ResetParameters();
    }

    SetParameter(&engine::EntityParameter::gimmickType, "StageGate");
    SetParameter(&engine::EntityParameter::targetScene, GetParameters()->targetScene.empty() ? "SELECT" : GetParameters()->targetScene);
    SetParameter(&engine::EntityParameter::startActive, true);
    SetColor({0.35f, 0.75f, 1.0f, 1.0f});
    SetScale({1.4f, 1.4f, 1.4f});
    SetEmissive(1.8f);
    SetMaterialType(kGatePortalMaterialType);
    SetBlendMode(BlendMode::kNormal);
    SetEnableEnvMap(false);

    // プレイヤーだけがゲートへの入場を開始できます。
    SetCollisionAttribute(engine::collisionCategories::Get("trigger"));
    SetCollisionMask(engine::collisionCategories::Get("player"));
    SetColliderType(engine::ColliderType::kSphere);
    SetCollisionRadius(4.0f);

    // 展示・ステージ開始はTarget IDをステージ番号として使い、シーン転移だけはシーン名の入力欄を使う。
    using Self = GimmickStageGate;
    engine::ObjectPropertyDefinition<Self> properties(*this, kTypeId, "ステージゲート",
        "セレクト用ノードは接近して決定した時、転移・開始ゲートは接触時に作動します。未設定の転移先は実行時にSELECTとして扱います。");
    using Parameter = engine::Object3D::EntityParameter;

    properties.Accessor<std::int32_t>("ステージ番号（Target ID）", "stageIndex", "targetID",
        [](const Self& object) { return object.GetTargetId(); },
        [](Self& object, const std::int32_t& value) { object.SetTargetId(value); }, 0, 0,
        +[](const Self& object) { return !object.GetParameters() || object.GetParameters()->actionMode != 1; });
    properties.Parameter("ゲートモード", "actionMode", &Parameter::actionMode, 0.0f, 2.0f,
        nullptr, {"ステージセレクト用ノード", "シーン転移ゲート", "ステージ開始ゲート"});
    properties.Parameter("開始時に有効", "startActive", &Parameter::startActive);
    properties.Parameter("転移先シーン", "targetScene", &Parameter::targetScene, 0.0f, 0.0f,
        +[](const Self& object) { return object.GetParameters() && (object.GetParameters()->actionMode == 1); }, {"タイトル", "チュートリアル", "ステージセレクト", "ゲーム本編", "ゲームオーバー"}, {.values = {"TITLE", "TUTORIAL", "SELECT", "GAMEPLAY", "GAMEOVER"}});
}

void GimmickStageGate::Update(float deltaTime) {
    // 目標開放度に滑らかに追従させ、開放の閾値をまたいだ瞬間だけ発光を強めます。
    // 現在の縮尺を記録し、表示の出現率を目標へ追従させる。
    CaptureBaseScale();
    baseScale_ = GetScale();

    pulseTimer_ += deltaTime;
    const float response = Clamp01(deltaTime * 6.5f);
    const float previousActivation = activation_;
    activation_ = Lerp(activation_, targetActivation_, response);

    // 解放済みで目標が0.82を超える開放時、出現率が0.62を下から越えたフレームに発光を最大にする。
    if (isUnlocked_ && targetActivation_ > 0.82f && previousActivation < 0.62f && activation_ >= 0.62f) {
        activationBurst_ = 1.0f;
    }

    // 解放や進入直後の一時的な光を減衰させ、通常のゲート状態色へ戻します。
    activationBurst_ = activationBurst_ > deltaTime * 1.15f ? activationBurst_ - deltaTime * 1.15f : 0.0f;
    const float visibleActivation = isUnlocked_ ? activation_ : 0.06f;

    lastAppliedPulse_ = 1.0f;
    engine::Vector4 gateColor = GetTargetColor();

    // ゲートが解放済みで一時発光が残っていれば、通常の状態色に明るい色を重ねる。
    if (activationBurst_ > 0.0f && isUnlocked_) {
        const float burstTint = Clamp01(activationBurst_ * 0.82f);
        const engine::Vector4 burstColor = isCleared_ ? engine::Vector4{0.58f, 1.0f, 1.0f, 1.0f} : engine::Vector4{1.0f, 0.88f, 0.40f, 1.0f};
        gateColor.x = Lerp(gateColor.x, burstColor.x, burstTint);
        gateColor.y = Lerp(gateColor.y, burstColor.y, burstTint);
        gateColor.z = Lerp(gateColor.z, burstColor.z, burstTint);
        gateColor.w = Lerp(gateColor.w, 1.0f, burstTint);
    }

    SetColor(gateColor);
    // 選択やクリアには別の発光を与え、解放中は専用の明滅を優先します。
    float emissive = isUnlocked_ ? Lerp(0.62f, 1.65f, visibleActivation) : 0.34f;
    // 解放済みのゲートが選択されている場合に、選択時の明るさへ寄せる。
    if (isSelected_ && isUnlocked_)
        emissive = Lerp(emissive, 2.7f, visibleActivation);
    // クリア済みの状態には、クリア時の発光の強さも反映する。
    if (isCleared_)
        emissive = Lerp(emissive, 1.9f, visibleActivation);
    // 解放演出中の発光は周期的な明滅で上書きする。
    if (isUnlocking_)
        emissive = 4.0f + std::sin(pulseTimer_ * 10.0f) * 0.9f;
    // 解放済みなら、過渡的な発光を最後に足す。
    if (isUnlocked_) {
        emissive += activationBurst_ * 2.8f;
    }

    SetEmissive(emissive);
    UpdatePortalMaterial();

    BaseGimmick::Update(deltaTime);
    // 進入演出がゲートを通り切った後、シーン切り替えを一度だけ要求します。
    // 進入演出を開始済みのゲートだけ、経過時間を進める。
    if (entryPresentation_.IsActive()) {
        entryPresentation_.Update(deltaTime);
        // 演出が完了し、遷移の予約がまだなければ初めてシーン変更を要求する。
        if (entryPresentation_.IsFinished() && !transitionQueued_)
            TriggerTransition();
    }
}

bool GimmickStageGate::OnCollision(engine::Object3D* other) {
    // プレイヤー属性を持つ相手の接触だけ、入場候補として調べる。
    if (!other || !(other->GetCollisionAttribute() & engine::collisionCategories::Get("player"))) {
        return false;
    }

    // 展示ノードや無効な状態、演出中・遷移要求済みのゲートでは新しい演出を始めない。
    if (!CanTriggerTransition()) {
        return false;
    }

    // 転移先が有効でシーン遷移の競合がないことを確認し、操作を引き取る進入演出を始めます。
    auto* player = dynamic_cast<Player*>(other);
    auto* manager = SceneManager::GetInstance();
    const std::string destination = GetGateMode() == kGateModeStageTransition ? "GAMEPLAY" : GetTargetSceneName();
    // Player型、プレイ中、遷移なし、登録済みの行き先をすべて満たし、進入演出も開始できた場合だけ接触を成立させる。
    if (!player || !manager->IsPlaying() || manager->IsTransitioning() || !manager->IsSceneRegistered(destination) ||
        !entryPresentation_.Start(player, this)) {
        return false;
    }

    // 接触時に遷移を予約せず、同じディゾルブを最後まで描画してから要求します。
    hasTriggeredTransition_ = true;
    TriggerEntryReaction();
    return true;
}

std::unique_ptr<engine::Object3D> GimmickStageGate::Clone() const {
    // 複製先の描画資源を初期化した後で、ゲート設定をコピーします。
    auto newObj = std::make_unique<GimmickStageGate>();
    assert(GetObjectCommon() != nullptr);
    newObj->Initialize(GetObjectCommon(), GetModelName());
    newObj->CopyFrom(this);
    return newObj;
}

std::int32_t GimmickStageGate::GetStageIndex() const {
    // ステージ番号はシーンの Target ID と共有します。
    return GetTargetId();
}

std::int32_t GimmickStageGate::GetGateMode() const {
    // 古い配置データにモードがなくても、遷移しない展示ノードとして扱います。
    if (!GetParameters().has_value()) {
        return kGateModeStageSelectNode;
    }

    return GetParameters()->actionMode;
}

const std::string& GimmickStageGate::GetTargetSceneName() const {
    // 遷移先が未設定ならステージ選択画面へ戻します。
    static const std::string kDefaultScene = "SELECT";

    // 遷移先を記録していない旧配置や空の設定値は、ステージ選択画面に解決する。
    if (!GetParameters().has_value() || GetParameters()->targetScene.empty()) {
        return kDefaultScene;
    }

    return GetParameters()->targetScene;
}

bool GimmickStageGate::IsStageSelectNodeMode() const {
    // 展示ノードと実際の遷移ゲートを呼び出し側で区別します。
    return GetGateMode() == kGateModeStageSelectNode;
}

void GimmickStageGate::SetGateState(bool selected, bool unlocked, bool cleared, bool unlocking) {
    // 選択・解放・クリア状態は Update の色と発光量へまとめて反映します。
    isSelected_ = selected;
    isUnlocked_ = unlocked;
    isCleared_ = cleared;
    isUnlocking_ = unlocking;
}

void GimmickStageGate::SetGateActivation(float activation) {
    // 呼び出し側の入力を正規化し、現在値には Update で追従させます。
    targetActivation_ = Clamp01(activation);
}

void GimmickStageGate::SetTransitionEnabled(bool enabled) {
    // 再有効化時は一度きりの遷移記録を戻し、再入場を許可します。
    // 有効状態を保存するパラメーターが作られていなければ、先に初期化する。
    if (!GetParameters().has_value()) {
        ResetParameters();
    }

    SetParameter(&engine::EntityParameter::startActive, enabled);
    // 進入中に無効化されたら演出をリセットし、もう一度有効にした時だけ要求済みフラグを戻します。
    if (!enabled && entryPresentation_.IsActive())
        entryPresentation_.Reset();

    // 有効化されていて演出が進行中でなければ、次の接触を受け付けるよう予約済みの状態を消す。
    if (enabled && !entryPresentation_.IsActive()) {
        transitionQueued_ = false;
        hasTriggeredTransition_ = false;
    }
}

void GimmickStageGate::TriggerEntryReaction() {
    // プレイヤーが触れた瞬間に、ゲートを強く反応させます。
    targetActivation_ = 1.0f;
    activation_ = (std::max)(activation_, 0.92f);
    activationBurst_ = 1.0f;
}

void GimmickStageGate::UpdatePortalMaterial() {
    // 描画メッシュの共通材質を使って、出現率・解放演出・入場発光を表現する。
    auto* renderer = GetMeshRenderer();

    // 描画側か対応パラメーターを取得できない場合は、材質更新を省く。
    if (!renderer || !renderer->GetWaterParamData()) {
        return;
    }

    // 水系の共通パラメータをゲートの波・発光表現にも利用します。
    auto* portal = renderer->GetWaterParamData();
    const float active = isUnlocking_ ? 1.0f : (isUnlocked_ ? activation_ : 0.0f);
    const float selectedBoost = (isSelected_ && isUnlocked_) ? 1.0f : 0.0f;
    const float entryBurst = isUnlocked_ ? activationBurst_ : 0.0f;

    portal->waveSpeed = Lerp(0.28f, 2.25f, active) + entryBurst * 2.30f;
    portal->waveHeight = Lerp(0.18f, 1.35f, active) + entryBurst * 0.72f;
    portal->waveFrequency = Lerp(4.0f, 22.0f, active) + entryBurst * 9.0f;
    portal->effectSoftness = Lerp(0.28f, 0.66f, active) + entryBurst * 0.10f;
    portal->effectIntensity =
        isUnlocked_ ? Lerp(0.34f, 1.58f + selectedBoost * 0.44f, active) + entryBurst * 1.85f : 0.12f;
    portal->effectScale = Lerp(0.78f, 1.18f, active) + entryBurst * 0.18f;
    portal->effectType = isCleared_ ? 2.0f : 1.0f;
    portal->flowSpeedX = 0.0f;
    portal->flowSpeedY = 0.0f;
    portal->uvOffsetX = activationBurst_;
    portal->uvOffsetY = active;
}

// 見た目の開放状態と実際に遷移できる条件を分け、演出途中の誤入場を防ぎます。
bool GimmickStageGate::CanTriggerTransition() const {
    // 接触で開始できる条件を判定する。行き先の登録や他のシーン遷移との競合はOnCollisionで確認する。
    const std::int32_t mode = GetGateMode();

    // 展示ノード、要求済み、または既に演出中なら同じ接触から開始しない。
    if (mode == kGateModeStageSelectNode || hasTriggeredTransition_ || entryPresentation_.IsActive()) {
        return false;
    }

    // パラメーターに無効が指定されていれば接触を受け付けない。
    if (GetParameters().has_value() && !GetParameters()->startActive) {
        return false;
    }

    return true;
}

void GimmickStageGate::TriggerTransition() {
    // Editor で演出中に行き先が変更されても、存在しないシーンへの要求で操作不能にしません。
    auto* manager = SceneManager::GetInstance();
    const std::string destination = GetGateMode() == kGateModeStageTransition ? "GAMEPLAY" : GetTargetSceneName();
    // 演出中に行き先が無効になった場合は、操作ロックを解除して新しい接触を受け付ける状態へ戻す。
    if (!manager->IsSceneRegistered(destination)) {
        entryPresentation_.Reset();
        hasTriggeredTransition_ = false;
        transitionQueued_ = false;
        return;
    }
    // 衝突が続くフレームでも ChangeScene を一度だけ要求します。
    hasTriggeredTransition_ = true;
    transitionQueued_ = true;

    // ステージ入場ではステージの番号を先に反映し、シーン転移は選択された行き先だけを使います。
    if (GetGateMode() == kGateModeStageTransition) {
        StageManager::GetInstance()->SetCurrentStage(GetStageIndex());
        SceneManager::GetInstance()->ChangeScene("GAMEPLAY");
        return;
    }

    SceneManager::GetInstance()->ChangeScene(GetTargetSceneName());
}

void GimmickStageGate::CaptureBaseScale() {
    // 編集時の元スケールを初回だけ保存し、演出による変化の基準にします。
    if (hasBaseScale_) {
        return;
    }

    baseScale_ = GetScale();
    hasBaseScale_ = true;
}

engine::Vector4 GimmickStageGate::GetTargetColor() const {
    // 開放・クリア・選択の優先順位で色を決めます。
    if (!isUnlocked_) {
        // 未解放のゲートに使う、輪郭が見える青灰色を返す。
        return {0.30f, 0.54f, 0.64f, 0.92f};
    }

    const float active = isUnlocking_ ? 1.0f : activation_;

    // 解放演出中はクリアや選択の色より先に高い不透明度の金色を使う。
    if (isUnlocking_) {
        return {1.0f, 0.72f, 0.28f, 1.0f};
    }

    // クリア済みなら水色系で、出現率に合わせて不透明度を変える。
    if (isCleared_) {
        const float alpha = Lerp(0.70f, 0.98f, active);

        // 選択中のクリア済みゲートは、同じ不透明度のまま明るい水色にする。
        if (isSelected_) {
            return {0.48f, 0.96f, 1.0f, alpha};
        }

        return {0.28f, 0.82f, 1.0f, alpha};
    }

    // 未クリアで選択中なら、通常の未クリア色より明るい橙色を使う。
    if (isSelected_) {
        return {1.0f, 0.62f, 0.24f, Lerp(0.78f, 1.0f, active)};
    }

    // 解放済みで未クリアかつ未選択の通常色を返す。
    return {1.0f, 0.52f, 0.20f, Lerp(0.68f, 0.94f, active)};
}

// ゲートが演出を所有し、シーン側は通常更新の入口でこの処理を呼ぶだけにします。
bool GimmickStageGate::UpdateSceneEntry(BaseScene& scene, float deltaTime) {
    for (const auto& object : scene.GetObjects()) {
        auto* gate = dynamic_cast<GimmickStageGate*>(object.get());
        // 演出を再生しているステージゲートだけ、通常更新を止める候補にする。
        if (!gate || !gate->entryPresentation_.IsActive()) continue;
        // 編集中へ戻った場合は操作・遷移の一時状態を解除して、他に有効なゲートがないか調べ続けます。
        if (!SceneManager::GetInstance()->IsPlaying()) {
            gate->entryPresentation_.Reset();
            gate->hasTriggeredTransition_ = false;
            gate->transitionQueued_ = false;
            continue;
        }
        // この経路では進入演出とポストエフェクトだけを進め、通常のゲーム進行は止めます。
        PostEffect::GetInstance()->Update(deltaTime);
        gate->Update(deltaTime);
        // ワールドを停止していても、カメラ移動後の地形・背景の描画行列は更新します。
        for (const auto& sceneObject : scene.GetObjects())
            sceneObject->RefreshRenderCameraData();
        return true;
    }
    return false;
}

void GimmickStageGate::ResetSceneEntries(BaseScene& scene) {
    // Player とカメラが生存している Finalize の先頭で、借用参照をすべて外します。
    for (const auto& object : scene.GetObjects()) {
        // シーン内のステージゲートごとに演出を解除し、予約と要求済みフラグを消す。
        if (auto* gate = dynamic_cast<GimmickStageGate*>(object.get())) {
            gate->entryPresentation_.Reset();
            gate->hasTriggeredTransition_ = false;
            gate->transitionQueued_ = false;
        }
    }
}
