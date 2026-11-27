#define NOMINMAX
#include <cstdint>
#include "GimmickArenaEncounter.h"
#include "game/actor/enemy/boss/core/MidbossEncounterEffects.h"
#include "ObjectPropertyDefinition.h"

#include "BaseScene.h"
#include "CollisionConfig.h"
#include "game/actor/enemy/boss/prism/EnemyPrismSlime.h"
#include "BaseEnemy.h"
#include "game/actor/enemy/boss/false_king/EnemyFalseKingSlime.h"
#include "game/scene/gameplay/GamePlayScene.h"
#include "Player.h"
#include "SceneManager.h"
#include "VfxSequencer.h"
#include "game/system/progress/GameDataManager.h"
#include "game/system/progress/StageManager.h"

#include <algorithm>
#include <cassert>

namespace {
constexpr float kDefaultBossRevealDelay = 0.72f;
constexpr std::int32_t kDefaultBarrierCount = 4;
constexpr std::int32_t kHighCrownActionMode = 2;
constexpr const char* kHighCrownReleaseSequence = "shared/false_king_defeat_reward_cue";
} // namespace

// ボス戦の入口を初期化し、入場範囲とイベント接続、登場演出の編集項目を登録する。
void GimmickArenaEncounter::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    BaseGimmick::Initialize(common, modelName);

    // 入場範囲はプレイヤー専用のトリガーとし、ボスや壁はイベント ID で参照します。
    SetClassName("Gimmick");
    SetGimmickType("ArenaEncounter");
    SetName("Gimmick_ArenaEncounter");
    SetStatic(false);
    SetCastShadow(false);
    SetCollisionAttribute(engine::collisionCategories::Get("trigger"));
    SetCollisionMask(engine::collisionCategories::Get("player"));

    engine::Object3D::ColliderConfig collider;
    collider.type = engine::ColliderType::kObb;
    collider.center = {0.0f, 1.5f, 0.0f};
    collider.size = {3.0f, 3.0f, 8.0f};
    SetColliderConfig(collider);

    // パラメーターが未作成なら、壁数と登場時刻の既定値を設定できるようにする。
    if (!GetParameters().has_value()) {
        ResetParameters();
    }

    SetParameter(&engine::EntityParameter::gimmickType, "ArenaEncounter");
    SetParameter(&engine::EntityParameter::maxCount, kDefaultBarrierCount);
    SetParameter(&engine::EntityParameter::shakeDuration, kDefaultBossRevealDelay);
    SetParameter(&engine::EntityParameter::startActive, true);
    SetParameter(&engine::EntityParameter::returnOnOff, false);

    // Target IDでボスと壁の連番を参照する。この装置に割り当てたMy Event IDで撃破通知を受ける。
    using Self = GimmickArenaEncounter;
    engine::ObjectPropertyDefinition<Self> properties(*this, kTypeId, "中ボス遭遇管理",
        "Target IDは中ボスのMy Event ID、My Event IDは撃破通知の受信先です。障壁はTarget ID + 1から連番で指定します。");
    using Parameter = engine::Object3D::EntityParameter;
    properties.Parameter("障壁数", "maxCount", &Parameter::maxCount, 1.0f, 16.0f);
    properties.Parameter("ボス出現まで", "shakeDuration", &Parameter::shakeDuration, 0.05f, 3.0f);
    properties.Parameter("遭遇を有効にする", "startActive", &Parameter::startActive);
    properties.Parameter("登場演出JSON", "cinematicPath", &Parameter::cinematicPath, 0.0f, 0.0f,
        nullptr, {}, {.assetDirectory = "Resources/json/scenario", .assetEditor = EditorPropertyAssetEditor::kScenario});
}

void GimmickArenaEncounter::Update(float deltaTime) {
    // 編集中は進行を巻き戻し、再生中だけ封鎖からボス登場までの猶予を数えます。
    SceneManager* sceneManager = SceneManager::GetInstance();
    const bool isPlaying = sceneManager && sceneManager->IsPlaying();

    // プレイを停止したら入場判定と編集表示を待機状態へ戻し、戦闘の時計を進めない。
    if (!isPlaying) {
        ResetForEditor();
        BaseGimmick::Update(deltaTime);
        return;
    }

    // 再生開始後の最初の更新で、今回のボス戦と障壁を待機状態に揃える。
    if (!initializedForPlay_) {
        InitializeForPlay();
    }

    // 封鎖中だけ、ボス登場までの経過秒数を進める。
    if (state_ == State::kSealing) {
        stateTimer_ += (std::max)(0.0f, deltaTime);

        // 登場タイムラインの再生状態はシーン側で持つため、ここでは終了したかを確認する。
        if (introTimelineActive_) {
            auto* gameplay = dynamic_cast<GamePlayScene*>(sceneManager->GetCurrentScene());
            // ゲームプレイシーンがなくなったか登場演出が終了した場合は、時間条件による起動へ戻す。
            if (!gameplay || !gameplay->IsArenaBossIntroActive()) {
                introTimelineActive_ = false;
            }
        }

        // 封鎖からの待ち時間と登場演出の終了を両方満たしてから、ボスを戦闘へ参加させる。
        if (!introTimelineActive_ && stateTimer_ >= GetBossRevealDelay()) {
            SetBossActive(true);
            state_ = State::kFighting;
            stateTimer_ = 0.0f;
        }
    }

    BaseGimmick::Update(deltaTime);
}

// ゲーム中は入場判定だけを使い、編集時に範囲モデルを描画する。
void GimmickArenaEncounter::Draw(ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource) {
    SceneManager* sceneManager = SceneManager::GetInstance();

    if (!sceneManager || !sceneManager->IsPlaying()) {
        // 編集中だけ進入範囲を可視化し、ゲーム中は判定とイベント接続だけを残します。
        BaseGimmick::Draw(pointLightResource, spotLightResource);
    }
}

bool GimmickArenaEncounter::OnCollision(engine::Object3D* other) {
    // 待機中にプレイヤーが接触した場合だけ、新規の入場として設定を調べる。
    if (state_ != State::kWaiting || !other || other->GetClassName() != "Player") {
        return false;
    }

    // 遭遇が設定されていないか開始無効なら、接触しても開始しない。
    if (!GetParameters().has_value() || !GetParameters()->startActive) {
        return false;
    }

    BeginEncounter();
    return true;
}

void GimmickArenaEncounter::OnTrigger() {
    // My Event IDは中ボスの撃破通知専用です。
    // 誤った接触通知が届いても、実HPが残っている間は封鎖を解除しません。
    // 封鎖・戦闘段階にあり、撃破状態を確認できた場合だけ完了処理へ進む。
    if ((state_ == State::kSealing || state_ == State::kFighting) && IsBossDefeated()) {
        CompleteEncounter();
    }
}

std::unique_ptr<engine::Object3D> GimmickArenaEncounter::Clone() const {
    // 進行状態は持ち込まず、配置とイベント設定を複製します。
    auto clone = std::make_unique<GimmickArenaEncounter>();
    assert(GetObjectCommon() != nullptr);
    clone->Initialize(GetObjectCommon(), GetModelName());
    clone->CopyFrom(this);
    return clone;
}

void GimmickArenaEncounter::InitializeForPlay() {
    // 戦闘開始前はボスと隔離壁を停止し、報酬も隠しておきます。
    initializedForPlay_ = true;
    state_ = State::kWaiting;
    stateTimer_ = 0.0f;
    introTimelineActive_ = false;
    SetCollisionAttribute(engine::collisionCategories::Get("trigger"));
    SetCollisionMask(engine::collisionCategories::Get("player"));
    SetBossActive(false);
    SetBarriersActive(false);

    // 高冠戦だけは戦闘前の報酬を明示的に無効にする。
    if (IsHighCrownMode()) {
        SetRewardActive(false);
    }
}

void GimmickArenaEncounter::ResetForEditor() {
    // 戦闘後の衝突停止を編集画面へ持ち越さないようにします。
    initializedForPlay_ = false;
    state_ = State::kWaiting;
    stateTimer_ = 0.0f;
    introTimelineActive_ = false;
    SetIsVisible(true);
    SetCollisionAttribute(engine::collisionCategories::Get("trigger"));
    SetCollisionMask(engine::collisionCategories::Get("player"));
}

// 入場時に隔離壁を有効化し、ボスの起動は登場演出の信号または時間判定まで待ちます。
void GimmickArenaEncounter::BeginEncounter() {
    // 入場判定を即時に外し、同じフレームの重複開始を防ぎます。
    state_ = State::kSealing;
    stateTimer_ = 0.0f;
    introTimelineActive_ = false;
    SetCollisionAttribute(0);
    SetCollisionMask(0);
    SetBarriersActive(true);

    // 中ボスでは配置された敵の種別から封鎖効果を選び、高冠戦の登場はシーンの演出に任せます。
    if (!IsHighCrownMode()) {
        const auto& effects = IsMagmaMode() ? MidbossEncounterEffects::Magma() : MidbossEncounterEffects::Prism();
        VfxSequencer::PlayOneShot(effects.seal, GetWorldPosition(), {4.0f, 1.0f, 4.0f});
    }

    introTimelineActive_ = StartBossPresentation();
}

// 撃破を確定し、モードに応じて壁の解除か報酬の受け取りへ進める。
void GimmickArenaEncounter::CompleteEncounter() {
    // 中ボス戦は同じステージのチェックポイント復帰で再戦を防ぐため、撃破を一時記録する。
    // 最終ボスの報酬演出は別経路で管理するため、この一時記録には含めない。
    if (!IsHighCrownMode())
        GameDataManager::GetInstance()->RememberCheckpointEncounter(StageManager::GetInstance()->GetCurrentStageIndex(),
                                                                    GetEventId());
    state_ = State::kCleared;
    stateTimer_ = 0.0f;
    introTimelineActive_ = false;
    SetCollisionAttribute(0);
    SetCollisionMask(0);

    // 中ボス戦は壁を開きます。高冠戦では報酬を有効化し、専用の受け取り演出に進みます。
    if (IsHighCrownMode()) {
        SetRewardActive(true);
    // 中ボス戦は報酬演出に進まず、連動する隔離壁を開放する。
    } else {
        SetBarriersActive(false);
    }

    // 演出の基準は遭遇地点です。高冠戦で報酬を取得できる場合はその位置へ切り替えます。
    engine::Vector3 cuePosition = GetWorldPosition();

    // 高冠戦では連番イベントから報酬を解決し、その位置と報酬受け取り演出を使う。
    if (IsHighCrownMode()) {
        BaseScene* scene = SceneManager::GetInstance() ? SceneManager::GetInstance()->GetCurrentScene() : nullptr;
        const std::int32_t rewardEventId = GetTargetId() + GetBarrierCount() + 1;

        // シーンが取得できた場合だけ、連番で指定した報酬を探す。
        if (scene) {
            // 報酬を見つけた場合は、効果の基準を遭遇地点から報酬位置に切り替える。
            if (engine::Object3D* reward = scene->FindObjectByEventId(rewardEventId)) {
                cuePosition = reward->GetTranslate();

                // ゲームプレイのシーンから呼ばれた場合は、報酬受け取り演出を開始する。
                if (auto* gameplay = dynamic_cast<GamePlayScene*>(scene)) {
                    gameplay->StartArenaBossDefeatReward(reward);
                }
            }
        }
    }

    // 高冠戦は報酬用の専用VFXを発生させる。
    if (IsHighCrownMode()) {
        VfxSequencer::PlayOneShot(kHighCrownReleaseSequence, cuePosition, {1.25f, 1.25f, 1.25f});
    // 中ボス戦の解除は、対象ボスの種別に合わせた解除VFXを発生させる。
    } else {
        const auto& effects = IsMagmaMode() ? MidbossEncounterEffects::Magma() : MidbossEncounterEffects::Prism();
        VfxSequencer::PlayOneShot(effects.release, cuePosition, {4.0f, 1.0f, 4.0f});
    }
}

bool GimmickArenaEncounter::IsBossDefeated() const {
    // 最終撃破演出の保留中は HP がゼロでも解放を始めません。
    BaseScene* scene = SceneManager::GetInstance() ? SceneManager::GetInstance()->GetCurrentScene() : nullptr;

    // 現在のシーンか正のボスIDがなければ、撃破を確認できない。
    if (!scene || GetTargetId() <= 0) {
        return false;
    }

    auto* boss = dynamic_cast<BaseEnemy*>(scene->FindObjectByEventId(GetTargetId()));

    // 指定したIDが敵オブジェクトを指さなければ、撃破にはしない。
    if (!boss) {
        return false;
    }

    // 最終撃破演出を保留している高冠ボスは、HP0でも封鎖を解除できる状態にはしない。
    if (auto* king=dynamic_cast<EnemyFalseKingSlime*>(boss); king && king->IsFinalDefeatPending())
        return false;
    const float hp = boss->GetParameters().has_value() ? boss->GetParameters()->hp : 1.0f;
    return boss->IsDead() || hp <= 0.0f;
}

void GimmickArenaEncounter::SetBossActive(bool active) {
    // Target ID が指すボスへ有効状態を送ります。
    BaseScene* scene = SceneManager::GetInstance() ? SceneManager::GetInstance()->GetCurrentScene() : nullptr;

    // シーンと有効なボスIDを取得できなければ、状態通知を送らない。
    if (!scene || GetTargetId() <= 0) {
        return;
    }

    scene->SetEventActive(GetTargetId(), active);
}

void GimmickArenaEncounter::SetBarriersActive(bool active) {
    // ボスの後続 ID を隔離壁の連番として扱います。
    BaseScene* scene = SceneManager::GetInstance() ? SceneManager::GetInstance()->GetCurrentScene() : nullptr;

    // シーンかボスIDが有効でなければ、そこから壁の位置を求められない。
    if (!scene || GetTargetId() <= 0) {
        return;
    }

    const std::int32_t firstBarrierEventId = GetTargetId() + 1;

    for (std::int32_t index = 0; index < GetBarrierCount(); ++index) {
        scene->SetEventActive(firstBarrierEventId + index, active);
    }
}

void GimmickArenaEncounter::SetRewardActive(bool active) {
    // 報酬はボスと壁の連番の直後に配置されます。
    BaseScene* scene = SceneManager::GetInstance() ? SceneManager::GetInstance()->GetCurrentScene() : nullptr;

    // シーンかボスIDが有効でなければ、そこから報酬IDを求められない。
    if (!scene || GetTargetId() <= 0) {
        return;
    }

    const std::int32_t rewardEventId = GetTargetId() + GetBarrierCount() + 1;
    scene->SetEventActive(rewardEventId, active);
}

bool GimmickArenaEncounter::StartBossPresentation() {
    // 配置Objectの演出JSONを使う。未設定・読み込み失敗時は通常の遅延起動へ戻す。
    if (!IsHighCrownMode() || !GetParameters().has_value() || GetParameters()->cinematicPath.empty()) {
        return false;
    }

    BaseScene* scene = SceneManager::GetInstance() ? SceneManager::GetInstance()->GetCurrentScene() : nullptr;
    auto* gameplay = dynamic_cast<GamePlayScene*>(scene);
    // 登場演出を扱うGamePlaySceneが取得できなければ、通常の遅延起動に任せる。
    if (!gameplay) {
        return false;
    }

    // 指定されたボスと最初の壁をタイムラインへ渡し、再生を開始できたか返す。
    engine::Object3D* boss = scene->FindObjectByEventId(GetTargetId());
    engine::Object3D* gate = scene->FindObjectByEventId(GetTargetId() + 1);
    return gameplay->StartArenaBossIntro(boss, gate, GetParameters()->cinematicPath);
}

void GimmickArenaEncounter::ActivateBossFromTimeline() {
    // 対象の遭遇が封鎖中のときだけ、DirectorのSignalで戦闘を始める。
    SceneManager* manager = SceneManager::GetInstance();
    // 有効なプレイ中の高冠戦が封鎖段階にあり、登場演出が進行中の場合だけ信号を受け付ける。
    if (!manager || !manager->IsPlaying() || !IsHighCrownMode() || state_ != State::kSealing ||
        !introTimelineActive_) {
        return;
    }

    SetBossActive(true);
    state_ = State::kFighting;
    stateTimer_ = 0.0f;
    introTimelineActive_ = false;
}

float GimmickArenaEncounter::GetBossRevealDelay() const {
    // Timelineが使えない場合と通常ボスは、配置時の遅延設定で起動する。
    return GetParameters().has_value() ? (std::max)(0.05f, GetParameters()->shakeDuration) : kDefaultBossRevealDelay;
}

std::int32_t GimmickArenaEncounter::GetBarrierCount() const {
    // ID の連番探索が暴走しないよう壁数に上限を設けます。
    return GetParameters().has_value() ? (std::clamp)(GetParameters()->maxCount, 1, 16) : kDefaultBarrierCount;
}

bool GimmickArenaEncounter::IsHighCrownMode() const {
    // 高冠戦だけ報酬や登場の進行を切り替えます。
    return GetParameters().has_value() && GetParameters()->actionMode == kHighCrownActionMode;
}

bool GimmickArenaEncounter::IsMagmaMode() const {
    // 配置先ボスの型から封鎖・解放 VFX を選びます。
    BaseScene* scene = SceneManager::GetInstance() ? SceneManager::GetInstance()->GetCurrentScene() : nullptr;
    const engine::Object3D* boss = scene && GetTargetId() > 0 ? scene->FindObjectByEventId(GetTargetId()) : nullptr;
    return boss && boss->GetEnemyType() == "MagmaSlime";
}

void GimmickArenaEncounter::RestoreCheckpointClear() {
    // チェックポイント復帰後は戦闘を再発火させず、壁を開いた状態へ戻します。
    initializedForPlay_ = true;
    state_ = State::kCleared;
    stateTimer_ = 0.0f;
    introTimelineActive_ = false;
    SetCollisionAttribute(0);
    SetCollisionMask(0);
    SetBossActive(false);
    SetBarriersActive(false);
    // 高冠戦のクリアを復元する場合は、報酬も有効な状態へ揃える。
    if (IsHighCrownMode())
        SetRewardActive(true);
}
