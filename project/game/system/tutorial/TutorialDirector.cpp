#define NOMINMAX
#include <cstdint>
#include "game/system/tutorial/TutorialDirector.h"

#include "BaseScene.h"
#include "BaseEnemy.h"
#include "game/actor/enemy/spawn/EnemyFactory.h"
#include "CollisionManager.h"
#include "CollisionConfig.h"
#include "Camera.h"
#include "CameraManager.h"
#include "DebugConsole.h"
#include "GimmickStageGate.h"
#include "InputManager.h"
#include "game/ui/InputGlyphResolver.h"
#include "Player.h"
#include "SceneManager.h"
#include "Sprite.h"
#include "TextureManager.h"
#include "WinApp.h"

#include <utility>

#ifdef USE_IMGUI
#include "imgui.h"
#endif

#include <algorithm>
#include <cmath>
#include <fstream>

using json = nlohmann::json;

namespace {
constexpr float kPromptFadeResponse = 9.0f;
constexpr float kObjectiveFadeResponse = 10.0f;
constexpr float kObjectiveScreenMargin = 54.0f;

float HorizontalDistance(const engine::Vector3& a, const engine::Vector3& b) {
    // 移動課題の進捗には段差の高さを含めず、地面上の距離を使う。
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

void SetFullTextureRect(engine::Sprite* sprite, const std::string& texturePath, const engine::Vector2& maxSize) {
    // Spriteと画像パスが揃う場合、画像全体を使い枠内に縦横比を保って収める。maxSizeは画面ピクセル。
    if (!sprite || texturePath.empty()) {
        return;
    }

    const uint32_t handle = engine::Sprite::LoadTexture(texturePath);

    // 画像を取得できない場合は手元のUVとサイズを維持する。
    if (handle == 0) {
        return;
    }

    sprite->SetTextureHandle(handle);
    sprite->SetTextureName(texturePath);
    const auto& metadata = engine::TextureManager::GetInstance()->GetMetadata(handle);
    sprite->SetTextureRect({0.0f, 0.0f}, {static_cast<float>(metadata.width), static_cast<float>(metadata.height)});

    const float textureWidth = static_cast<float>((std::max)(metadata.width, size_t{1}));
    const float textureHeight = static_cast<float>((std::max)(metadata.height, size_t{1}));
    const float fitScale = (std::min)(maxSize.x / textureWidth, maxSize.y / textureHeight);
    sprite->SetSize({textureWidth * fitScale, textureHeight * fitScale});
}
} // namespace

// チュートリアルのJSON手順を実行します。表示、課題判定、敵の有効化をステップ遷移に合わせて同期します。

// シーン・Player・入力を借用し、手順と表示を準備できた場合にtrueを返す。過去の実行状態は先に終了する。
bool TutorialDirector::Initialize(BaseScene* scene, Player* player, engine::InputManager* inputManager,
                                  const std::string& flowPath) {
    Finalize();
    scene_ = scene;
    player_ = player;
    inputManager_ = inputManager;
    flowPath_ = flowPath;

    // 依存する参照が欠けているか、手順を読み込めない場合は準備を中止する。
    if (!scene_ || !player_ || !inputManager_ || !LoadFlow(flowPath_)) {
        DebugConsole::GetInstance()->AddLog(std::string("[Tutorial] Flowの初期化に失敗しました: ") + flowPath_);
        return false;
    }

    // UI参照とゲートを結んでから、課題で順番に出す敵の初期状態を退避します。
    BindPromptSprites();
    exitGate_ = dynamic_cast<GimmickStageGate*>(FindObjectByName(exitGateObjectName_));
    SetGateTransitionEnabled(false);
    spawnEnemies_.clear();

    for (const std::string& objectName : spawnEnemyObjectNames_) {
        auto* enemy = dynamic_cast<BaseEnemy*>(FindObjectByName(objectName));

        // 名前で見つからないか敵型ではない対象は登録せず、名前をログに残す。
        if (!enemy) {
            DebugConsole::GetInstance()->AddLog("[Tutorial] Spawn enemy not found: " + objectName);
            continue;
        }

        SpawnEnemySlot slot;
        slot.objectName = objectName;
        slot.initialDefinition = enemy->ExportToJson();
        slot.initialCustomState = enemy->CaptureReplayState().custom;
        slot.initialPosition = enemy->GetTranslate();

        // 衝突属性とマスクは配置値を利用する。値が0なら、出現時に使う既定の敵カテゴリと対象を補う。
        slot.collisionAttribute = enemy->GetCollisionAttribute() != 0 ? enemy->GetCollisionAttribute() : engine::collisionCategories::Get("enemy");
        slot.collisionMask = enemy->GetCollisionMask() != 0
                                 ? enemy->GetCollisionMask()
                                 : (engine::collisionCategories::Get("player") | engine::collisionCategories::Get("ground") | engine::collisionCategories::Get("player_bullet") | engine::collisionCategories::Get("player_attack"));
        const auto hpOverride = spawnEnemyMaxHpOverrides_.find(objectName);

        // 名前に対応するHPの上書きがあれば、再出現後にも適用できるよう控える。
        if (hpOverride != spawnEnemyMaxHpOverrides_.end()) {
            slot.maxHpOverride = hpOverride->second;
        }

        const auto detectionOverride = spawnEnemyDetectionRangeOverrides_.find(objectName);

        // 同じ名前に検知距離の指定があれば、配置値と別に控える。
        if (detectionOverride != spawnEnemyDetectionRangeOverrides_.end()) {
            slot.detectionRangeOverride = detectionOverride->second;
        }

        spawnEnemies_.push_back(std::move(slot));
    }

    // 最初の更新より前に描画されても、チュートリアル用の敵が見えない状態にします。
    SetAllSpawnEnemiesActive(false);

    // 敵を隠した状態で演出の再生器を接続し、最初のPlay更新を待ちます。
    cinematicPlayer_.Initialize(SceneManager::GetInstance());
    cinematicPlayer_.SetSequence(&cinematicSequence_);
    loaded_ = true;
    return true;
}

void TutorialDirector::Finalize() {
    // 演出中のカメラ拘束と操作制限を先に戻し、次のシーンへ持ち越さない。
    if (cinematicActive_) {
        EndStepCinematic(true);
    }

    // Playerへの参照が残る場合は、練習用に制限した投擲・吸収を通常の許可状態へ戻す。
    if (player_) {
        player_->SetTutorialCarryActionPermissions(true, true);
    }

    // 表示要求を消した後、シーン内Objectへの参照と課題の経過状態をすべて破棄します。
    SetPromptVisible(false);
    SetObjectiveMarkerVisible(false);
    steps_.clear();
    currentStepIndex_ = -1;
    scene_ = nullptr;
    player_ = nullptr;
    inputManager_ = nullptr;
    exitGate_ = nullptr;
    flowPath_.clear();
    spawnEnemyObjectName_ = "Tutorial_PinkSlime";
    spawnEnemyObjectNames_.clear();
    spawnEnemyMaxHpOverrides_.clear();
    spawnEnemyDetectionRangeOverrides_.clear();
    spawnEnemies_.clear();
    started_ = false;
    loaded_ = false;
    completed_ = false;
    cinematicActive_ = false;
    controlsGuideOpened_ = false;
    lockedTargetName_.clear();
    stepConditionSatisfied_ = false;
    conditionSatisfiedTimer_ = 0.0f;
    morphWasActiveOnStepEntry_ = false;
    carriedEnemyOnStepEntryName_.clear();
    promptPanel_ = {};
    promptLabel_ = {};
    promptIcons_ = {};
    promptAlpha_ = 0.0f;
    promptRequestedVisible_ = false;
    promptGamepadMode_ = false;
    promptBindingSignature_.clear();
    objectiveMarkers_ = {};
    objectiveTargetNames_ = {};
    objectiveOffsetY_ = 0.75f;
    objectiveAlphas_ = {};
    objectiveAnimationTime_ = 0.0f;
    objectiveRequestedVisible_ = false;
    completionCallback_ = {};
    cinematicSequence_.Clear();
}

// Play中の手順、案内表示と演出を進め、条件達成後も最短表示時間と待機時間を満たしてから次へ進みます。
void TutorialDirector::Update(float deltaTime) {
    // 読込前、完了後、またはPlayer不在なら、手順を進めず残った画面表示だけを更新する。
    if (!loaded_ || completed_ || !player_) {
        UpdatePrompt(deltaTime);
        UpdateObjectiveMarker(deltaTime);
        return;
    }

    // 編集停止中は最初のシネマティックを開始せず、自由カメラを維持する。
    if (!started_ && deltaTime <= 0.0f) {
        UpdatePrompt(0.0f);
        UpdateObjectiveMarker(0.0f);
        return;
    }

    // Play開始まで敵を出さず、最初の有効な更新でステップ0へ入ります。
    BeginFlowIfNeeded();

    // 現在の番号が手順配列の外なら、そのフレームでは条件を評価しない。
    if (currentStepIndex_ < 0 || currentStepIndex_ >= static_cast<std::int32_t>(steps_.size())) {
        return;
    }

    // ステップの経過と演出を先に進め、演出の完了を同じフレームの達成判定で参照できるようにする。
    stepTimer_ += (std::max)(0.0f, deltaTime);
    UpdateCinematic(deltaTime);

    // 手順が続いている間も、最後に使用した入力方式へ案内を切り替えます。
    if (inputManager_ &&
        (promptGamepadMode_ != inputManager_->IsGamepadMode() ||
         promptBindingSignature_ != GetInputGlyphBindingSignature())) {
        // 達成後のフェード中も説明を揃え、切替によって消した案内を再表示しません。
        const bool wasVisible = promptRequestedVisible_;
        ApplyStepPrompt(&steps_[static_cast<size_t>(currentStepIndex_)]);
        SetPromptVisible(wasVisible);
    }

    UpdatePrompt(deltaTime);
    UpdateObjectiveMarker(deltaTime);

    // 運搬対象の取り違えを先に回復し、そのフレームは達成判定へ進めません。
    const Step& step = steps_[static_cast<size_t>(currentStepIndex_)];

    // 時間が進んでいる間は練習敵の消失から復旧する。運搬の誤操作から復旧したフレームも進行判定を見送る。
    if ((deltaTime > 0.0f && RecoverMissingPracticeEnemy(step)) || RecoverFromWrongCarriedAction(step)) {
        return;
    }

    // 達成は一度だけ記録します。表示時間と達成後の待ち時間は独立して満たします。
    if (!stepConditionSatisfied_ && EvaluateCurrentStep()) {
        stepConditionSatisfied_ = true;
        conditionSatisfiedTimer_ = 0.0f;
        SetPromptVisible(false);
    }

    // 達成したフレーム以降だけ、次へ移るまでの待ち時間を数える。
    if (stepConditionSatisfied_) {
        conditionSatisfiedTimer_ += (std::max)(0.0f, deltaTime);
    }

    // 達成済みで、手順の最低表示時間と達成後の待ち時間の両方を経過したら次へ進む。
    if (stepConditionSatisfied_ && stepTimer_ >= step.minimumDisplayTime &&
        conditionSatisfiedTimer_ >= step.completionDelay) {
        AdvanceStep();
    }
}

void TutorialDirector::NotifyControlsGuideOpened() {
    // 操作案内を開く課題の達成は、Sceneから通知された事実で判定する。
    controlsGuideOpened_ = true;
}

void TutorialDirector::NotifyLockOnTarget(const engine::Object3D* target) {
    // 生ポインタの寿命に依存せず、目標名だけを課題判定へ渡す。
    lockedTargetName_ = target ? target->GetName() : std::string{};
}

const std::string& TutorialDirector::GetCurrentStepId() const {
    // 開始前・完了後にも参照可能な固定IDを返す。
    static const std::string kNone = "none";

    if (currentStepIndex_ < 0 || currentStepIndex_ >= static_cast<std::int32_t>(steps_.size())) {
        return kNone;
    }

    return steps_[static_cast<size_t>(currentStepIndex_)].id;
}

// 出口へ向かう課題の進行中だけ、解放済みで入場演出のないゲートを方向表示の対象として借用する。
engine::Object3D* TutorialDirector::GetDirectionGuideTarget() const {
    if (!loaded_ || completed_ || cinematicActive_ || stepConditionSatisfied_ ||
        GetCurrentStepId() != "reach_exit_gate")
        return nullptr;
    // Editorで削除・差し替えられたゲートへの古いポインターを持ち越さない。
    auto* gate = dynamic_cast<GimmickStageGate*>(FindObjectByName(exitGateObjectName_));
    return gate && gate->IsUnlocked() && !gate->IsEntryPresentationActive() ? gate : nullptr;
}

void TutorialDirector::CaptureReplayState(json& state) const {
    // 対象物はポインタでなく名前を保存し、復元時に現シーンから解決する。
    const auto objectName = [this](const engine::Object3D* object) {
        for (const auto& candidate : scene_->GetObjects())
            if (candidate && candidate.get() == object) return candidate->GetName();
        return std::string{};
    };

    // 出現状態が有効の練習敵だけ、名前を記録する。
    json activeEnemies = json::array();
    for (const auto& slot : spawnEnemies_) if (slot.active) activeEnemies.push_back(slot.objectName);
    state = {{"currentStepIndex", currentStepIndex_},
             {"activeSpawnEnemies", std::move(activeEnemies)},
             {"stepTimer", stepTimer_},
             {"conditionSatisfiedTimer", conditionSatisfiedTimer_},
             {"stepStartPosition", {stepStartPosition_.x, stepStartPosition_.y, stepStartPosition_.z}},
             {"started", started_},
             {"completed", completed_},
             {"cinematicActive", cinematicActive_},
             {"cinematicTime", cinematicPlayer_.GetCurrentTime()},
             {"controlsGuideOpened", controlsGuideOpened_},
             {"stepConditionSatisfied", stepConditionSatisfied_},
             {"morphWasActiveOnStepEntry", morphWasActiveOnStepEntry_},
             {"carriedEnemyOnStepEntry", carriedEnemyOnStepEntryName_},
             {"playerCarriedEnemy", player_ ? objectName(player_->GetCarriedEnemy()) : std::string{}},
             {"promptAlpha", promptAlpha_},
             {"promptRequestedVisible", promptRequestedVisible_},
             {"objectiveTargetNames", objectiveTargetNames_},
             {"objectiveOffsetY", objectiveOffsetY_},
             {"objectiveAlphas", objectiveAlphas_},
             {"objectiveAnimationTime", objectiveAnimationTime_},
             {"objectiveRequestedVisible", objectiveRequestedVisible_}};
}

// 手順の状態を復元し、有効な手順と記録が揃う場合だけ編集前後の参照と表示を組み直す。
void TutorialDirector::RestoreReplayState(const json& state) {
    // 復元先の手順設定が未読込か記録がオブジェクトでなければ、現在値を保持する。
    if (!loaded_ || !state.is_object()) {
        return;
    }

    // シネマティックのカメラトラックを残さず、保存状態から組み直します。
    cinematicPlayer_.Stop(false);
    cinematicActive_ = false;

    // Playerがまだ演出で拘束されている場合は、以前の再生の操作ロックも解除する。
    if (player_ && player_->IsCinematicLocked()) {
        player_->EndCinematicLock(true);
    }

    // 手順番号を有効範囲へ収め、開始前と完了後も区別したまま経過時間を読み戻します。
    const std::int32_t maxStepIndex = static_cast<std::int32_t>(steps_.size());
    currentStepIndex_ = (std::clamp)(state.value("currentStepIndex", -1), -1, maxStepIndex);
    stepTimer_ = (std::max)(0.0f, state.value("stepTimer", 0.0f));
    conditionSatisfiedTimer_ = (std::max)(0.0f, state.value("conditionSatisfiedTimer", 0.0f));

    // 開始位置を記録した項目がある場合だけ、座標を復元する。
    if (const auto found = state.find("stepStartPosition"); found != state.end()) {
        ReadVector3(*found, stepStartPosition_);
    }

    // 記録された名前を照合し、練習敵スロットの出現フラグを現在の構成へ戻します。
    if (state.contains("activeSpawnEnemies") && state["activeSpawnEnemies"].is_array()) {
        const auto& active = state["activeSpawnEnemies"];
        for (auto& slot : spawnEnemies_)
            slot.active = std::find(active.begin(), active.end(), slot.objectName) != active.end();
    }
    started_ = state.value("started", false);
    completed_ = state.value("completed", false);
    controlsGuideOpened_ = state.value("controlsGuideOpened", false);
    stepConditionSatisfied_ = state.value("stepConditionSatisfied", false);
    morphWasActiveOnStepEntry_ = state.value("morphWasActiveOnStepEntry", false);
    carriedEnemyOnStepEntryName_ = state.value("carriedEnemyOnStepEntry", std::string{});

    const Step* currentStep = currentStepIndex_ >= 0 && currentStepIndex_ < maxStepIndex
                                  ? &steps_[static_cast<size_t>(currentStepIndex_)]
                                  : nullptr;
    // 現在の手順から表示の内容を作り、その後で記録時点の透明度や表示要求を戻します。
    ApplyStepPrompt(currentStep);
    ApplyStepObjective(currentStep);

    promptAlpha_ = (std::clamp)(state.value("promptAlpha", 0.0f), 0.0f, 1.0f);
    promptRequestedVisible_ = state.value("promptRequestedVisible", false);
    objectiveOffsetY_ = state.value("objectiveOffsetY", objectiveOffsetY_);
    objectiveAnimationTime_ = (std::max)(0.0f, state.value("objectiveAnimationTime", 0.0f));
    objectiveRequestedVisible_ = state.value("objectiveRequestedVisible", false);

    // 目標名の配列は既存の表示枠の数まで読み、文字列の要素だけを上書きする。
    if (const auto found = state.find("objectiveTargetNames"); found != state.end() && found->is_array()) {
        for (size_t index = 0; index < objectiveTargetNames_.size() && index < found->size(); ++index) {
            if ((*found)[index].is_string()) {
                objectiveTargetNames_[index] = (*found)[index].get<std::string>();
            }
        }
    }

    // 透明度の配列があれば記録できた枠まで読み、0～1に収める。
    if (const auto found = state.find("objectiveAlphas"); found != state.end() && found->is_array()) {
        for (size_t index = 0; index < objectiveAlphas_.size() && index < found->size(); ++index) {
            objectiveAlphas_[index] = (std::clamp)((*found)[index].get<float>(), 0.0f, 1.0f);
        }
    }

    // Playerの参照が有効な場合は、記録した手順と運搬対象に合わせて操作許可を復元する。
    if (player_) {
        if (!started_) {
            // Play In Editor停止時に、投擲・吸収の参照も編集開始前へ戻します。
            player_->ResetTutorialCarryActionState();
            player_->SetTutorialCarryActionPermissions(true, true);
        // 開始済みの有効な手順では、その手順の許可操作を戻してから運搬相手を名前で解決する。
        } else if (currentStep) {
            player_->SetTutorialCarryActionPermissions(currentStep->allowCarriedThrow, currentStep->allowCarriedAbsorb);

            const std::string carriedEnemyName = state.value("playerCarriedEnemy", std::string{});
            engine::Object3D* savedCarriedEnemy = FindObjectByName(carriedEnemyName);

            // 対象の実体が残る場合は再接続する。記録の相手がいなくなった場合は既存の運搬を解除する。
            if (savedCarriedEnemy) {
                player_->SetCarriedEnemy(savedCarriedEnemy);
            } else if (player_->GetCarriedEnemy()) {
                player_->ReleaseCarriedEnemy(true);
            }
        } else {
            // 開始済みでも有効な手順がないときは、手順専用の操作制限を解除する。
            player_->SetTutorialCarryActionPermissions(true, true);
        }
    }

    // 保存時点で演出を再生中だった手順だけ再構築し、同じ秒位置へ移動して操作を再ロックします。
    const bool shouldRestoreCinematic =
        currentStep && !currentStep->cinematicPath.empty() && state.value("cinematicActive", false);
    cinematicSequence_.Clear();

    if (shouldRestoreCinematic && cinematicSequence_.Load(currentStep->cinematicPath)) {
        cinematicPlayer_.SetSequence(&cinematicSequence_);
        cinematicPlayer_.Play(false);
        cinematicPlayer_.SetTime((std::max)(0.0f, state.value("cinematicTime", 0.0f)), false);
        cinematicActive_ = cinematicPlayer_.IsPlaying();

        // 復元した演出が実際に再生中で、Playerがまだ拘束されていなければ操作を止める。
        if (cinematicActive_ && player_ && !player_->IsCinematicLocked()) {
            player_->BeginCinematicLock(true);
        }
    }
}

void TutorialDirector::Restart() {
    // 再挑戦では敵の出現、ゲート、運搬状態を手順開始前へ戻す。
    // 手順を未読込なら再挑戦できないため、現在の状態に触れない。
    if (!loaded_) {
        return;
    }

    // 現在のカメラ演出と操作制限を解除してから、開始前の課題状態と安全設定へ戻します。
    LeaveCurrentStep();
    player_->ResetTutorialCarryActionState();
    player_->SetTutorialSafetyEnabled(true);
    player_->SetTutorialCarryActionPermissions(true, true);
    completed_ = false;
    started_ = false;
    currentStepIndex_ = -1;
    stepTimer_ = 0.0f;
    stepStartPosition_ = player_->GetWorldPosition();
    controlsGuideOpened_ = false;
    stepConditionSatisfied_ = false;
    conditionSatisfiedTimer_ = 0.0f;
    morphWasActiveOnStepEntry_ = false;
    carriedEnemyOnStepEntryName_.clear();
    promptAlpha_ = 0.0f;
    objectiveAlphas_ = {};
    objectiveAnimationTime_ = 0.0f;
    // 次の有効な更新で手順を開始できるよう、出口と練習敵はこの時点では無効にしておきます。
    SetGateTransitionEnabled(false);
    SetAllSpawnEnemiesActive(false);
}

bool TutorialDirector::JumpToStep(const std::string& stepId) {
    // 途中再開時は先行手順の敵状態を再適用してから目標ステップへ入る。
    for (size_t index = 0; index < steps_.size(); ++index) {
        if (steps_[index].id != stepId) {
            continue;
        }

        LeaveCurrentStep();
        started_ = true;
        completed_ = false;
        RestoreSpawnEnemyStateBeforeStep(static_cast<std::int32_t>(index));
        EnterStep(static_cast<std::int32_t>(index));
        return true;
    }

    return false;
}

bool TutorialDirector::LoadFlow(const std::string& flowPath) {
    // 手順本体は一時配列で読み、空や不正な手順を実行リストへ入れない。
    std::ifstream file(flowPath);

    // フローのファイルを開けなければ設定の読み込みを行わない。
    if (!file.is_open()) {
        return false;
    }

    json root;
    try {
        file >> root;
    } catch (...) {
        // ファイルをJSONとして解析できなければ、進行を開始できない。
        return false;
    }

    // 進行にはステップ配列が必須。形式が違うファイルは開始前に弾く。
    if (!root.contains("steps") || !root["steps"].is_array()) {
        return false;
    }

    // 旧形式で省略可能なゲートと練習敵の名前を、場面の既定名で補う。
    exitGateObjectName_ = root.value("exitGateObject", "Gimmick_StageGate");
    spawnEnemyObjectName_ = root.value("spawnEnemyObject", "Tutorial_PinkSlime");
    spawnEnemyObjectNames_.clear();

    // 複数指定がない旧フローでも単体指定の敵を使用できるようにする。
    if (root.contains("spawnEnemyObjects") && root["spawnEnemyObjects"].is_array()) {
        for (const auto& value : root["spawnEnemyObjects"]) {
            // 配列中の文字列だけを練習敵の名前として採用する。
            if (value.is_string()) {
                spawnEnemyObjectNames_.push_back(value.get<std::string>());
            }
        }
    }

    // 有効な複数指定がなければ、従来の単体指定を使う。
    if (spawnEnemyObjectNames_.empty()) {
        spawnEnemyObjectNames_.push_back(spawnEnemyObjectName_);
    }

    // 敵のHP上書きはオブジェクト名で保持し、練習敵が生存して開始できる下限を設ける。
    spawnEnemyMaxHpOverrides_.clear();

    // HPの指定がオブジェクトなら、数値の項目だけを下限1で保存する。
    if (root.contains("spawnEnemyMaxHp") && root["spawnEnemyMaxHp"].is_object()) {
        for (const auto& [objectName, value] : root["spawnEnemyMaxHp"].items()) {
            if (value.is_number()) {
                spawnEnemyMaxHpOverrides_[objectName] = (std::max)(1.0f, value.get<float>());
            }
        }
    }

    // 敵の検知距離もオブジェクトごとに読み、負値は0へ補正する。
    spawnEnemyDetectionRangeOverrides_.clear();

    // 検知距離がオブジェクトなら、数値の項目だけを下限0で保存する。
    if (root.contains("spawnEnemyDetectionRange") && root["spawnEnemyDetectionRange"].is_object()) {
        for (const auto& [objectName, value] : root["spawnEnemyDetectionRange"].items()) {
            if (value.is_number()) {
                spawnEnemyDetectionRangeOverrides_[objectName] = (std::max)(0.0f, value.get<float>());
            }
        }
    }

    std::vector<Step> loadedSteps;
    try {
        // 個別ステップでは省略可能な項目に従来値を補い、最後にまとめて公開する。
        for (const auto& item : root["steps"]) {
            // オブジェクト以外の配列要素はステップに変換しない。
            if (!item.is_object()) {
                continue;
            }

            Step step;
            step.id = item.value("id", "step_" + std::to_string(loadedSteps.size()));
            step.condition = ParseCondition(item.value("condition", "timer"));
            step.action = item.value("action", "");
            step.cinematicPath = item.value("cinematic", "");
            step.promptLabel = item.value("promptLabel", "");
            step.promptLabelGamepad = item.value("promptLabelGamepad", "");
            step.objectiveTarget = item.value("objectiveTarget", "");
            // 達成条件のしきい値と、目標マーカーの半径・表示高さを取得する。
            step.threshold = item.value("threshold", 0.0f);
            step.radius = (std::max)(0.1f, item.value("radius", 3.0f));
            step.objectiveOffsetY = item.value("objectiveOffsetY", 0.75f);
            // 案内を出す最低時間と、条件を満たしてから次へ移る待ち時間を別々に読む。
            step.minimumDisplayTime = (std::max)(0.0f, item.value("minimumDisplayTime", 0.12f));
            step.completionDelay = (std::max)(0.0f, item.value("completionDelay", 0.0f));
            step.showPrompt = item.value("showPrompt", true);
            step.requireMorph = item.value("requireMorph", false);
            // ゲートの見せ方、練習敵の出現、運搬中の許可操作をステップ単位で指定する。
            step.previewGateUnlock = item.value("previewGateUnlock", false);
            step.unlockGateOnComplete = item.value("unlockGateOnComplete", false);
            step.spawnEnemy = item.value("spawnEnemy", false);
            step.spawnEnemiesDormant = item.value("spawnEnemiesDormant", false);
            step.allowCarriedThrow = item.value("allowCarriedThrow", true);
            step.allowCarriedAbsorb = item.value("allowCarriedAbsorb", true);
            step.spawnEnemyObject = item.value("spawnEnemyObject", "");
            step.despawnEnemyObject = item.value("despawnEnemyObject", "");
            step.wrongActionRetryStep = item.value("wrongActionRetryStep", "");
            // 固定の目標座標と復帰地点は、座標を正しく読めた場合だけ有効にする。
            step.hasTargetPosition = ReadVector3(item.value("targetPosition", json::array()), step.targetPosition);
            step.hasCheckpoint = ReadVector3(item.value("checkpoint", json::array()), step.checkpointPosition);

            // キーボード用とパッド用のアイコンを別々に読み、文字列のパスだけを保存する。
            for (const auto& icon : item.value("promptIcons", json::array())) {
                if (icon.is_string()) {
                    step.promptIcons.push_back(icon.get<std::string>());
                }
            }

            for (const auto& icon : item.value("promptIconsGamepad", json::array())) {
                if (icon.is_string()) {
                    step.promptIconsGamepad.push_back(icon.get<std::string>());
                }
            }

            // 目標表示、撃破判定、生成と退場の各リストは、有効な文字列だけをオブジェクト名として保存する。
            for (const auto& objectName : item.value("objectiveTargets", json::array())) {
                if (objectName.is_string()) {
                    step.objectiveTargets.push_back(objectName.get<std::string>());
                }
            }

            for (const auto& objectName : item.value("targetEnemyObjects", json::array())) {
                if (objectName.is_string()) {
                    step.targetEnemyObjects.push_back(objectName.get<std::string>());
                }
            }

            for (const auto& objectName : item.value("spawnEnemyObjects", json::array())) {
                if (objectName.is_string()) {
                    step.spawnEnemyObjects.push_back(objectName.get<std::string>());
                }
            }

            for (const auto& objectName : item.value("despawnEnemyObjects", json::array())) {
                if (objectName.is_string()) {
                    step.despawnEnemyObjects.push_back(objectName.get<std::string>());
                }
            }

            loadedSteps.push_back(std::move(step));
        }
    } catch (...) {
        // 型違いなどで手順の途中までしか読めない場合に、実行リストへ反映しない。
        return false;
    }

    // 有効な手順が一つもないフローは開始できない。
    if (loadedSteps.empty()) {
        return false;
    }

    // ステップが一件以上読めた時点で進行用の配列に反映する。
    steps_ = std::move(loadedSteps);
    return true;
}

TutorialDirector::CompletionCondition TutorialDirector::ParseCondition(const std::string& conditionName) {
    // 未知の条件は時間待ちへフォールバックし、読込時に無効な列挙値を作らない。
    if (conditionName == "cinematic_finished")
        return CompletionCondition::kCinematicFinished;
    if (conditionName == "move_distance")
        return CompletionCondition::kMoveDistance;
    if (conditionName == "action_triggered")
        return CompletionCondition::kActionTriggered;
    if (conditionName == "target_locked")
        return CompletionCondition::kTargetLocked;
    if (conditionName == "reach_position")
        return CompletionCondition::kReachPosition;
    if (conditionName == "carry_enemy")
        return CompletionCondition::kCarryEnemy;
    if (conditionName == "enemy_thrown")
        return CompletionCondition::kEnemyThrown;
    // 単数形と複数形の旧キーを同じ撃破条件として受け付ける。
    if (conditionName == "enemy_defeated" || conditionName == "enemies_defeated") {
        return CompletionCondition::kEnemiesDefeated;
    }

    if (conditionName == "morph_active")
        return CompletionCondition::kMorphActive;
    if (conditionName == "morph_released")
        return CompletionCondition::kMorphReleased;
    if (conditionName == "controls_guide_opened")
        return CompletionCondition::kControlsGuideOpened;
    return CompletionCondition::kTimer;
}

bool TutorialDirector::ReadVector3(const json& value, engine::Vector3& destination) {
    // 3成分未満なら既存値を保持し、壊れた座標で復元しない。
    if (!value.is_array() || value.size() < 3) {
        return false;
    }

    destination = {value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
    return true;
}

void TutorialDirector::BeginFlowIfNeeded() {
    // 最初の更新までは敵を隠し、Play開始時だけ手順0へ入る。
    // 開始済みかステップが空なら、最初の手順を二重に開かない。
    if (started_ || steps_.empty()) {
        return;
    }

    SetAllSpawnEnemiesActive(false);
    started_ = true;
    EnterStep(0);
}

// 手順開始時の状態を記録し、敵・ゲート・案内・カメラ演出をこの手順の設定へ切り替えます。
void TutorialDirector::EnterStep(std::int32_t stepIndex) {
    // 有効な次のステップや操作対象がなければ、案内を続けず終了処理へ進みます。
    if (stepIndex < 0 || stepIndex >= static_cast<std::int32_t>(steps_.size()) || !player_) {
        CompleteFlow();
        return;
    }

    // 達成条件に使う「開始時」の姿勢と運搬対象は、敵の出現を切り替える前に保存します。
    currentStepIndex_ = stepIndex;
    stepTimer_ = 0.0f;
    conditionSatisfiedTimer_ = 0.0f;
    stepConditionSatisfied_ = false;
    stepStartPosition_ = player_->GetWorldPosition();
    morphWasActiveOnStepEntry_ = player_->IsEnemyMorphed();
    carriedEnemyOnStepEntryName_.clear();
    for (const auto& object : scene_->GetObjects())
        if (object && object.get() == player_->GetCarriedEnemy()) carriedEnemyOnStepEntryName_ = object->GetName();

    Step& step = steps_[static_cast<size_t>(currentStepIndex_)];

    // ガイドを開く課題へ入った場合は、手順前の開閉通知を達成に数えない。
    if (step.condition == CompletionCondition::kControlsGuideOpened) {
        controlsGuideOpened_ = false;
    }

    // 課題に合わせて投げと吸収の許可を切り替え、必要なら失敗後の復帰地点も変更します。
    player_->SetTutorialCarryActionPermissions(step.allowCarriedThrow, step.allowCarriedAbsorb);

    // 新しい復活座標が指定された場合だけ、今後の落下復帰地点を上書きする。
    if (step.hasCheckpoint) {
        player_->SetRespawnPosition(step.checkpointPosition);
    }

    UpdateGateStateForStep(step);

    // 退場指定があれば先に対象を隠し、今回の練習敵を有効にした後で行動可否を設定する。
    if (!step.despawnEnemyObject.empty()) {
        SetSpawnEnemyActive(step.despawnEnemyObject, false);
    }

    for (const std::string& objectName : step.despawnEnemyObjects) {
        SetSpawnEnemyActive(objectName, false);
    }

    // 個別の出現名を優先し、未指定で単体出現が有効な場合はフローの既定名を使う。
    if (!step.spawnEnemyObject.empty()) {
        SetSpawnEnemyActive(step.spawnEnemyObject, true);
    } else if (step.spawnEnemy) {
        SetSpawnEnemyActive(spawnEnemyObjectName_, true);
    }

    for (const std::string& objectName : step.spawnEnemyObjects) {
        SetSpawnEnemyActive(objectName, true);
    }

    // 練習相手を配置してから、指定された手順では行動を止めた状態へそろえます。
    if (step.spawnEnemiesDormant) {
        SetStepSpawnEnemiesDormant(step, true);
    }

    // 案内文と目標を更新し、演出が指定された手順だけカメラ再生を開始します。
    ApplyStepPrompt(&step);
    ApplyStepObjective(&step);

    if (!step.cinematicPath.empty()) {
        BeginStepCinematic(step);
    }

    DebugConsole::GetInstance()->AddLog("[Tutorial] Step: " + step.id);
}

void TutorialDirector::LeaveCurrentStep() {
    // ステップ固有のシネマティックと案内表示を次へ持ち越さない。
    // 演出が再生中なら強制終了してカメラと操作権を戻す。
    if (cinematicActive_) {
        EndStepCinematic(true);
    }

    SetPromptVisible(false);
    ApplyStepObjective(nullptr);
}

void TutorialDirector::AdvanceStep() {
    // 達成時のゲート解放を先に反映し、最後の手順ならFlowを完了する。
    if (currentStepIndex_ < 0 || currentStepIndex_ >= static_cast<std::int32_t>(steps_.size())) {
        return;
    }

    const Step completedStep = steps_[static_cast<size_t>(currentStepIndex_)];

    // 完了で出口を解放する指定がある場合だけ、その場でゲートを切り替える。
    if (completedStep.unlockGateOnComplete) {
        SetGateTransitionEnabled(true);
    }

    LeaveCurrentStep();

    const std::int32_t nextStep = currentStepIndex_ + 1;

    // 次の番号が末尾を超えたら新しい課題を開始せず、完了処理を行う。
    if (nextStep >= static_cast<std::int32_t>(steps_.size())) {
        CompleteFlow();
        return;
    }

    EnterStep(nextStep);
}

bool TutorialDirector::EvaluateCurrentStep() const {
    // 開始前や終了後は対象ステップを参照できないため、達成扱いにしない。
    if (!player_ || currentStepIndex_ < 0 || currentStepIndex_ >= static_cast<std::int32_t>(steps_.size())) {
        return false;
    }

    const Step& step = steps_[static_cast<size_t>(currentStepIndex_)];

    // 手順ごとの条件より先に、コピー形態という共通の前提を確認します。
    if (step.requireMorph && !player_->IsEnemyMorphed()) {
        return false;
    }

    switch (step.condition) {
    // 演出の更新側が再生中フラグを落とした時点で、操作手順へ進める。
    case CompletionCondition::kCinematicFinished:
        return !cinematicActive_;

    // ステップ開始位置からの水平距離を測る。ジャンプによる高さは移動距離に含めない。
    case CompletionCondition::kMoveDistance:
        return HorizontalDistance(player_->GetWorldPosition(), stepStartPosition_) >= step.threshold;

    // ステップで指定されたアクションの押下を、このフレームの入力から確認する。
    case CompletionCondition::kActionTriggered:
        return IsActionTriggered(step.action);

    // ロックオン通知で記録した名前が、課題で指定された対象と一致するか確認する。
    case CompletionCondition::kTargetLocked:
        return !step.objectiveTarget.empty() && lockedTargetName_ == step.objectiveTarget;

    case CompletionCondition::kReachPosition: {
        // 目標Objectがある課題ではEditorで移動した実体を追う。固定座標だけの課題は従来どおり。
        const auto* target = FindObjectByName(step.objectiveTarget);
        if (target)
            return HorizontalDistance(player_->GetWorldPosition(), target->GetWorldPosition()) <= step.radius;
        return step.hasTargetPosition &&
               HorizontalDistance(player_->GetWorldPosition(), step.targetPosition) <= step.radius;
    }

    // 対象の指定がある課題では、運搬中の敵が指定された実体かを確認する。
    case CompletionCondition::kCarryEnemy:
        return player_->GetCarriedEnemy() != nullptr &&
               (step.objectiveTarget.empty() || player_->GetCarriedEnemy() == FindObjectByName(step.objectiveTarget));

    case CompletionCondition::kEnemyThrown: {
        // 開始時に運んでいた敵を優先して追い、手元を離れた後に投擲状態へ入ったことを確認する。
        auto* enemy = dynamic_cast<BaseEnemy*>(FindObjectByName(carriedEnemyOnStepEntryName_.empty() ? step.objectiveTarget : carriedEnemyOnStepEntryName_));
        return enemy && player_->GetCarriedEnemy() == nullptr && !player_->IsEnemyMorphed() && enemy->IsThrownPhysics();
    }

    // 複数の練習敵を対象にできるため、登録した全対象の撃破確認を共通関数へ渡す。
    case CompletionCondition::kEnemiesDefeated:
        return AreTargetEnemiesDefeated(step);

    // コピーの入力だけで進めず、実際にコピー形態へ入ったことを確認する。
    case CompletionCondition::kMorphActive:
        return player_->IsEnemyMorphed();

    // 解除の入力だけで進めず、解除演出が終わって通常形態に戻るまで待つ。
    case CompletionCondition::kMorphReleased:
        return !player_->IsEnemyMorphed() && !player_->IsEnemyMorphReleasing();

    // ガイドを開いた通知を記録しておき、判定の時点で表示されているかには依存しない。
    case CompletionCondition::kControlsGuideOpened:
        return controlsGuideOpened_;

    case CompletionCondition::kTimer:
    // 時間条件では、このステップに入ってからの経過時間を基準にする。
    default:
        return stepTimer_ >= step.threshold;
    }
}

bool TutorialDirector::AreTargetEnemiesDefeated(const Step& step) const {
    // 出現済みの対象について、HP切れ・吸収・消去を倒し方によらず達成へ数える。
    // 対象名の指定がなければ、撃破の条件は成立しない。
    if (step.targetEnemyObjects.empty()) {
        return false;
    }

    for (const std::string& objectName : step.targetEnemyObjects) {
        // まだ出現していない対象を、名前がないだけで撃破済みにしない。
        const auto slot = std::find_if(spawnEnemies_.begin(), spawnEnemies_.end(),
                                      [&](const SpawnEnemySlot& value) { return value.objectName == objectName; });
        if (slot == spawnEnemies_.end() || !slot->active) return false;
        engine::Object3D* object = FindObjectByName(objectName);

        // すでに出現済みと分かっている対象がシーンから消えていれば、残りの対象を確認する。
        if (!object) {
            continue;
        }

        const auto* enemy = dynamic_cast<const BaseEnemy*>(object);

        // 同名の対象が敵型でないか、生存した実体なら全体の達成を待つ。
        if (!enemy || (!enemy->IsDead() && !enemy->IsReplayRemoved() &&
                       (!enemy->GetParameters() || enemy->GetParameters()->hp > 0.0f))) {
            return false;
        }
    }

    return true;
}

// 練習対象を失った課題を再開し、再試行ステップへ移れた場合にtrueを返す。
bool TutorialDirector::RecoverMissingPracticeEnemy(const Step& step) {
    // 達成済み、演出中、名前指定なしの場合は敵の不足を再評価しない。
    if (stepConditionSatisfied_ || cinematicActive_ || step.objectiveTarget.empty()) return false;
    // 自由移動などのステップでは敵を作り直さず、指定の敵による操作が必要な課題だけを対象にします。
    const bool practice = step.condition == CompletionCondition::kTargetLocked ||
                          step.condition == CompletionCondition::kCarryEnemy ||
                          step.condition == CompletionCondition::kEnemyThrown ||
                          step.condition == CompletionCondition::kMorphActive;
    // 対象必須の練習でない場合と、コピーが既に成立した場合は再作成しない。
    if (!practice || (step.condition == CompletionCondition::kMorphActive && player_->IsEnemyMorphed())) return false;
    auto* enemy = dynamic_cast<BaseEnemy*>(FindObjectByName(step.objectiveTarget));
    // 正しく投げた直後に手元から離れていても、対象の消失と扱わず達成判定へ進みます。
    if (step.condition == CompletionCondition::kEnemyThrown && enemy && enemy->IsThrownPhysics() &&
        !player_->GetCarriedEnemy() && !player_->IsEnemyMorphed()) return false;
    // 削除や撃破だけでなく、投げる前の運搬状態が失われた場合もやり直しの対象です。
    const bool lost = !enemy || enemy->IsReplayRemoved() || enemy->IsDead() ||
                      (enemy->GetParameters() && enemy->GetParameters()->hp <= 0.0f);
    const bool lostCarry = step.condition == CompletionCondition::kEnemyThrown &&
                           player_->GetCarriedEnemy() != enemy;
    // 実体も必要な運搬状態も残っていれば、ステップをやり直さない。
    if (!lost && !lostCarry) return false;
    // 参照先がなくなった運搬・吸収演出も解放してから、必要な操作だけをやり直す。
    player_->ResetTutorialCarryActionState();
    lockedTargetName_.clear();
    return JumpToStep(step.wrongActionRetryStep.empty() ? step.id : step.wrongActionRetryStep);
}

bool TutorialDirector::RecoverFromWrongCarriedAction(const Step& step) {
    // 投擲と吸収を取り違えた場合に、保持状態を戻して再試行へ移れるか確認する。
    if (!player_ || step.wrongActionRetryStep.empty()) {
        return false;
    }

    const bool absorbedDuringThrowPractice = !step.allowCarriedAbsorb && player_->IsEnemyMorphed();

    bool thrownDuringAbsorbPractice = false;

    // 投擲を許可していない手順では、開始時の相手が手を離れて実際に投擲状態になったかも調べる。
    if (!step.allowCarriedThrow) {
        auto* enemy = dynamic_cast<BaseEnemy*>(FindObjectByName(carriedEnemyOnStepEntryName_.empty() ? step.objectiveTarget : carriedEnemyOnStepEntryName_));
        thrownDuringAbsorbPractice =
            enemy && player_->GetCarriedEnemy() == nullptr && !player_->IsEnemyMorphed() && enemy->IsThrownPhysics();
    }

    // 禁止した吸収も投擲も成立していなければ、再試行せず通常の判定へ戻る。
    if (!absorbedDuringThrowPractice && !thrownDuringAbsorbPractice) {
        return false;
    }

    player_->ResetTutorialCarryActionState();
    DebugConsole::GetInstance()->AddLog("[Tutorial] 異なる操作を検出したため工程を再試行します: " + step.id);
    return JumpToStep(step.wrongActionRetryStep);
}

bool TutorialDirector::IsActionTriggered(const std::string& action) const {
    // マウス操作の課題名を共通Actionへ変換し、パッド操作でも達成可能にする。
    if (!inputManager_) {
        return false;
    }

    // 旧フローのマウス左右の名前は、割り当て可能な通常攻撃と副操作の押下として扱う。
    if (action == "MouseLeft")
        return inputManager_->IsActionTriggered("PrimaryAttack");
    if (action == "MouseRight")
        return inputManager_->IsActionTriggered("SecondaryAction");
    // 視点移動の課題は、マウスか右スティックのどちらかが閾値を超えたら達成とする。
    if (action == "MouseMove") {
        const engine::Vector2 delta = inputManager_->GetMouseMoveDelta();
        const engine::Vector2 stick = inputManager_->GetRightStick();
        return std::abs(delta.x) + std::abs(delta.y) > 0.5f || std::abs(stick.x) + std::abs(stick.y) > 0.25f;
    }

    return !action.empty() && inputManager_->IsActionTriggered(action);
}

void TutorialDirector::CompleteFlow() {
    // 完了通知と出口解放を一度だけ行い、操作制限を解除する。
    if (completed_) {
        return;
    }

    completed_ = true;
    currentStepIndex_ = static_cast<std::int32_t>(steps_.size());

    // Playerがいて練習用の許可状態を持つ場合は、投げと吸収を通常どおり許可する。
    if (player_) {
        player_->SetTutorialCarryActionPermissions(true, true);
    }

    SetPromptVisible(false);
    SetGateTransitionEnabled(true);

    // シーンが完了コールバックを登録している場合に、ゲート解放後の完了を知らせる。
    if (completionCallback_) {
        completionCallback_();
    }

    DebugConsole::GetInstance()->AddLog("[Tutorial] Flow completed.");
}

void TutorialDirector::BeginStepCinematic(const Step& step) {
    // 手順ごとの演出を読み、再生中はプレイヤーの操作を拘束する。
    cinematicSequence_.Clear();

    // 指定された演出素材を読めなければ、操作を拘束せず戻る。
    if (!cinematicSequence_.Load(step.cinematicPath)) {
        DebugConsole::GetInstance()->AddLog("[Tutorial] Cinematicの読み込みに失敗しました: " + step.cinematicPath);
        cinematicActive_ = false;
        return;
    }

    cinematicPlayer_.SetSequence(&cinematicSequence_);
    cinematicPlayer_.Play(false);
    cinematicActive_ = cinematicPlayer_.IsPlaying();

    // 再生が開始しPlayerが存在するときは、獲得済みのコピーを保持して演出専用の操作ロックを掛ける。
    if (cinematicActive_ && player_) {
        player_->BeginCinematicLock(true);
    }

    SetPromptVisible(false);
}

void TutorialDirector::EndStepCinematic(bool skipped) {
    // 自然終了時は IsPlaying() が先に false になりますが、カメラトラックは
    // Stop() を呼ぶまで残るため、必ず停止して通常カメラへブレンド復帰させます。
    cinematicPlayer_.Stop(false);
    cinematicActive_ = false;

    // 演出で操作を拘束したPlayerが残っていれば解除する。
    if (player_ && player_->IsCinematicLocked()) {
        player_->EndCinematicLock(true);
    }

    // まだ有効なステップがあり休眠指定があれば、登場していた練習敵のAIを戻す。
    if (currentStepIndex_ >= 0 && currentStepIndex_ < static_cast<std::int32_t>(steps_.size())) {
        const Step& step = steps_[static_cast<size_t>(currentStepIndex_)];

        if (step.spawnEnemiesDormant) {
            // 登場演出中だけ休眠させ、操作開始後の投擲速度を毎フレーム消さないようにします。
            SetStepSpawnEnemiesDormant(step, false);
        }
    }

    // 入力スキップや強制終了として呼ばれた場合だけログを残す。
    if (skipped) {
        DebugConsole::GetInstance()->AddLog("[Tutorial] Cinematic skipped.");
    }
}

void TutorialDirector::UpdateCinematic(float deltaTime) {
    // 開始直後の決定入力で飛ばさないよう、短い猶予の後だけスキップを受け付ける。
    // 演出を実行していないときは、再生器もスキップ入力も更新しない。
    if (!cinematicActive_) {
        return;
    }

    cinematicPlayer_.Update((std::max)(0.0f, deltaTime));
    const bool canSkip = stepTimer_ >= 0.18f;

    // 手順開始から0.18秒以上たちJumpが押された場合に、通常の終了処理でスキップする。
    if (canSkip && inputManager_ && inputManager_->IsActionTriggered("Jump")) {
        EndStepCinematic(true);
        return;
    }

    // 入力によるスキップと自然終了のどちらでも、共通の終了処理でカメラと操作を戻します。
    if (!cinematicPlayer_.IsPlaying()) {
        EndStepCinematic(false);
    }
}

void TutorialDirector::UpdateGateStateForStep(const Step& step) {
    // 予告表示は可能にしても、課題完了前のステージ遷移は許可しない。
    if (!exitGate_ || !step.previewGateUnlock) {
        return;
    }

    exitGate_->SetTransitionEnabled(false);
    exitGate_->SetGateState(false, false, false, true);
    exitGate_->SetGateActivation(0.72f);
}

void TutorialDirector::SetGateTransitionEnabled(bool enabled) {
    // 遷移可否と門の見た目を同時に更新し、解放時は反応演出を起動する。
    // 出口ゲートを借用できない場合は表示や遷移の状態を変更しない。
    if (!exitGate_) {
        return;
    }

    exitGate_->SetTransitionEnabled(enabled);
    exitGate_->SetGateState(false, enabled, false, false);
    exitGate_->SetGateActivation(enabled ? 1.0f : 0.0f);

    // ゲートを解放したときだけ、入場できることを反応演出で知らせる。
    if (enabled) {
        exitGate_->TriggerEntryReaction();
    }
}

// 課題用の敵を初期位置と物理状態へ戻してから表示を切り替え、運搬中なら先に手放します。
void TutorialDirector::SetSpawnEnemyActive(const std::string& objectName, bool active) {
    const auto it = std::find_if(spawnEnemies_.begin(), spawnEnemies_.end(),
                                 [&objectName](const SpawnEnemySlot& slot) { return slot.objectName == objectName; });
    // 指定名が練習敵の登録に含まれない場合は、通常配置の敵を変更しない。
    if (it == spawnEnemies_.end()) {
        return;
    }

    SpawnEnemySlot& slot = *it;
    auto* enemy = dynamic_cast<BaseEnemy*>(FindObjectByName(objectName));
    if (!enemy && active) {
        // 通常の撃破処理で実体が削除されても、配置時の定義から同じ練習対象を作り直す。
        auto created = EnemyFactory::GetInstance()->CreateEnemy(slot.initialDefinition.value("enemyType", "Slime"), scene_->GetObject3DCommon());
        // ファクトリーが敵を作れたら保存済みの定義を戻し、現シーンに所有権を渡す。
        if (created) {
            created->ImportFromJson(slot.initialDefinition);
            created->SetName(slot.objectName);
            enemy = dynamic_cast<BaseEnemy*>(scene_->AddObjectImmediate(std::move(created)));
        }
    }
    // 出現フラグを先に記録する。実体がなく更新できない場合は終了し、Playerが同じ敵を保持中なら先に手放す。
    slot.active = active;
    if (!enemy) return;
    if (player_ && player_->GetCarriedEnemy() == enemy) player_->ReleaseCarriedEnemy(true);
    if (active) {
        // 消滅演出・潰れ・投擲速度も戻し、再出現直後に再び消える状態を持ち越さない。
        enemy->ImportFromJson(slot.initialDefinition);
        auto state = enemy->CaptureReplayState();
        state.custom = slot.initialCustomState;
        state.dead = false;
        state.replayRemoved = false;
        enemy->RestoreReplayState(state);
        enemy->SetTarget(player_);
    }
    enemy->SetDead(false);
    enemy->SetCarried(false);
    enemy->SetTranslate(slot.initialPosition);
    enemy->SetVelocity({0.0f, 0.0f, 0.0f});
    // 出現先にパラメーターがある場合だけ練習用の値を重ねる。HPは正の指定、索敵距離は0以上を使う。
    if (active && enemy->GetParameters()) {
        if (slot.maxHpOverride > 0.0f) enemy->SetParameter(&engine::EntityParameter::hp, enemy->SetParameter(&engine::EntityParameter::maxHp, slot.maxHpOverride));
        if (slot.detectionRangeOverride >= 0.0f) {
            enemy->SetParameter(&engine::EntityParameter::detectionRange, slot.detectionRangeOverride);
            enemy->SetDetectionRange(slot.detectionRangeOverride);
        }
    }
    // 出現状態と休眠・表示・衝突を揃え、非表示の練習敵が接触を起こさないようにします。
    enemy->SetDormant(!active);
    enemy->SetIsVisible(active);
    enemy->SetCollisionAttribute(active ? slot.collisionAttribute : 0);
    enemy->SetCollisionMask(active ? slot.collisionMask : 0);
    enemy->UpdateWorldMatrix(true);
    // 有効化した場合だけ衝突管理へ登録し、表示と判定を再開する。
    if (active) {
        engine::CollisionManager::GetInstance()->AddObject(enemy);
        DebugConsole::GetInstance()->AddLog("[Tutorial] Enemy spawned: " + slot.objectName);
    }
}

void TutorialDirector::SetSpawnEnemyDormant(const std::string& objectName, bool dormant) {
    // 登場演出中のみ敵AIを止め、位置や表示状態は変えない。
    const auto it = std::find_if(spawnEnemies_.begin(), spawnEnemies_.end(),
                                 [&objectName](const SpawnEnemySlot& slot) { return slot.objectName == objectName; });
    // 登録外の名前に休眠設定を適用しない。
    if (it == spawnEnemies_.end()) {
        return;
    }

    BaseEnemy* enemy = dynamic_cast<BaseEnemy*>(FindObjectByName(objectName));

    // 同名の敵型オブジェクトを取得できた場合だけ、AIの休眠を切り替える。
    if (enemy) {
        enemy->SetDormant(dormant);
    }
}

void TutorialDirector::SetStepSpawnEnemiesDormant(const Step& step, bool dormant) {
    // 単体の明示名を優先し、未指定なら有効な旧単体設定の名前を使う。複数指定にも同じ休眠状態を渡す。
    if (!step.spawnEnemyObject.empty()) {
        SetSpawnEnemyDormant(step.spawnEnemyObject, dormant);
    } else if (step.spawnEnemy) {
        SetSpawnEnemyDormant(spawnEnemyObjectName_, dormant);
    }

    for (const std::string& objectName : step.spawnEnemyObjects) {
        SetSpawnEnemyDormant(objectName, dormant);
    }
}

void TutorialDirector::SetAllSpawnEnemiesActive(bool active) {
    // 最初のPlay前やリスタート時に、管理対象の敵を一括で隠す。
    for (const SpawnEnemySlot& slot : spawnEnemies_) {
        SetSpawnEnemyActive(slot.objectName, active);
    }
}

void TutorialDirector::RestoreSpawnEnemyStateBeforeStep(std::int32_t stepIndex) {
    // 指定ステップまでの出現・消去履歴を再適用し、途中再開でも敵の状態を一致させる。
    SetAllSpawnEnemiesActive(false);
    const std::int32_t endIndex = (std::clamp)(stepIndex, 0, static_cast<std::int32_t>(steps_.size()));

    for (std::int32_t index = 0; index < endIndex; ++index) {
        const Step& step = steps_[static_cast<size_t>(index)];

        // 通過済みの手順にある退場指定を先に適用し、続けて出現指定を戻す。
        if (!step.despawnEnemyObject.empty()) {
            SetSpawnEnemyActive(step.despawnEnemyObject, false);
        }

        for (const std::string& objectName : step.despawnEnemyObjects) {
            SetSpawnEnemyActive(objectName, false);
        }

        // 単体は明示名を優先し、未指定で出現が有効ならフローの既定名を使う。
        if (!step.spawnEnemyObject.empty()) {
            SetSpawnEnemyActive(step.spawnEnemyObject, true);
        } else if (step.spawnEnemy) {
            SetSpawnEnemyActive(spawnEnemyObjectName_, true);
        }

        for (const std::string& objectName : step.spawnEnemyObjects) {
            SetSpawnEnemyActive(objectName, true);
        }

        // 再現するのは通過済みの手順なので、登場演出中だけの休眠は残しません。
        if (step.spawnEnemiesDormant) {
            SetStepSpawnEnemiesDormant(step, false);
        }
    }
}

engine::Object3D* TutorialDirector::FindObjectByName(const std::string& name) const {
    // シーンと検索名が揃う場合に、実在する最初の同名Objectを借用して返す。
    if (!scene_ || name.empty()) {
        return nullptr;
    }

    for (const auto& object : scene_->GetObjects()) {
        if (object && object->GetName() == name) {
            return object.get();
        }
    }

    return nullptr;
}

void TutorialDirector::BindPromptSprites() {
    // Sceneが所有するSpriteの初期位置・色・大きさを退避して、手順ごとに再利用する。
    // シーンがなければ、配置された表示部品を探せないので終了する。
    if (!scene_) {
        return;
    }

    auto bind = [this](const std::string& name) {
        PromptVisual visual;
        visual.sprite = scene_->GetSpriteByName(name);

        // 指定の画像が配置されていれば、基準値を控えてから手順開始まで隠す。
        if (visual.sprite) {
            visual.basePosition = visual.sprite->GetPosition();
            visual.baseSize = visual.sprite->GetSize();
            visual.baseColor = visual.sprite->GetColor();
            visual.sprite->SetVisible(false);
        }

        return visual;
    };

    promptPanel_ = bind("tutorial_prompt_panel");
    promptPanel_.requestedVisible = true;
    promptLabel_ = bind("tutorial_prompt_label");

    for (size_t index = 0; index < promptIcons_.size(); ++index) {
        promptIcons_[index] = bind("tutorial_prompt_icon_" + std::to_string(index));
    }

    objectiveMarkers_[0] = bind("tutorial_objective_marker");

    for (size_t index = 1; index < objectiveMarkers_.size(); ++index) {
        objectiveMarkers_[index] = bind("tutorial_objective_marker_" + std::to_string(index));
    }
}

void TutorialDirector::ApplyStepPrompt(const Step* step) {
    // 固定操作表から入力機器に合う画像を選び、ゲーム内操作と案内を一致させる。
    promptGamepadMode_ = inputManager_ && inputManager_->IsGamepadMode();
    promptBindingSignature_ = GetInputGlyphBindingSignature();
    std::vector<InputGlyphSpec> glyphs;

    // 共通画像を現在の入力方式へ変換します。対応表にない画像だけ、パッド専用の個別指定を使います。
    if (step && !step->promptIcons.empty()) {
        // 移動のWASD画像はパッドのときだけ左スティック画像に置き換える。
        if (step->promptIcons[0] == "ui/input/combo/combo_wasd.png") {
            if (promptGamepadMode_)
                glyphs.push_back({"ui/input/controller/pad_l_stick.png"});
            else glyphs.push_back({"ui/input/combo/combo_wasd.png"});
        } else {
            // その他の画像は現在の割り当てを引き、パッド用に変換できない場合は手順の個別画像を使う。
            for (size_t i = 0; i < step->promptIcons.size(); ++i) {
                InputGlyphSpec glyph = ResolveInputGlyph(step->promptIcons[i], promptGamepadMode_);
                if (promptGamepadMode_ && glyph.path == step->promptIcons[i] &&
                    i < step->promptIconsGamepad.size()) {
                    glyph = {step->promptIconsGamepad[i]};
                }

                glyphs.push_back(std::move(glyph));
            }
        }
    } else if (step && promptGamepadMode_) {
        // 共通の画像指定がなくパッドの場合は、手順のパッド用画像をそのまま使う。
        for (const std::string& path : step->promptIconsGamepad)
            glyphs.push_back({path});
    }

    // アイコンと説明を同じ入力モードで選びます。共通説明だけの手順は従来の画像を使います。
    std::string labelPath;
    if (step) {
        labelPath = promptGamepadMode_ && !step->promptLabelGamepad.empty()
            ? step->promptLabelGamepad : step->promptLabel;
    }
    const bool show = step && step->showPrompt && (!glyphs.empty() || !labelPath.empty());
    SetPromptVisible(show);
    promptLabel_.requestedVisible = false;

    for (PromptVisual& visual : promptIcons_) {
        visual.requestedVisible = false;
    }

    // 表示設定が無効か案内の素材がない場合は、全要素の要求を解除した状態で戻る。
    if (!show) {
        return;
    }

    // 説明のパスと表示部品が両方ある場合だけ、アイコン数に合わせて説明の大きさと横位置を決める。
    const bool hasLabel = !labelPath.empty() && promptLabel_.sprite;

    if (hasLabel) {
        promptLabel_.requestedVisible = true;
        promptLabel_.sprite->SetVisible(true);
        const bool fourKeys = glyphs.size() >= 4;
        SetFullTextureRect(promptLabel_.sprite, labelPath, {fourKeys ? 360.0f : 470.0f, 76.0f});
        promptLabel_.basePosition.x = glyphs.empty() ? 960.0f : (fourKeys ? 1140.0f : 1070.0f);
        promptLabel_.baseSize = promptLabel_.sprite->GetSize();
    }

    // 用意済みの表示枠を上限とし、画像が多い手順では幅と間隔を縮めます。
    const size_t iconCount = (std::min)(glyphs.size(), promptIcons_.size());
    const float kPromptIconSpacing = iconCount >= 4 ? 90.0f : 132.0f;
    const float iconGroupCenterX = iconCount >= 4 ? 745.0f : (hasLabel ? 690.0f : 960.0f);
    const float firstIconX = iconGroupCenterX - kPromptIconSpacing * static_cast<float>(iconCount - 1) * 0.5f;

    for (size_t index = 0; index < promptIcons_.size(); ++index) {
        PromptVisual& visual = promptIcons_[index];

        // アイコンの配置部品がない枠は描画の更新を行わない。
        if (!visual.sprite) {
            continue;
        }

        const bool hasIcon = index < iconCount;
        visual.requestedVisible = hasIcon;
        visual.sprite->SetVisible(hasIcon);

        // 使用画像がある枠だけ、新しい画像範囲と基準位置を計算する。
        if (hasIcon) {
            const InputGlyphSpec& glyph = glyphs[index];
            SetFullTextureRect(visual.sprite, glyph.path, {iconCount >= 4 ? 90.0f : 190.0f, 118.0f});

            // アトラスから一部を使う画像は、切り出した範囲の縦横比で表示サイズを調整します。
            if (glyph.useCrop) {
                visual.sprite->SetTextureRect({glyph.cropX, glyph.cropY}, {glyph.width, glyph.height});
                const float fit = (std::min)(visual.sprite->GetSize().x / glyph.width,
                                            visual.sprite->GetSize().y / glyph.height);
                visual.sprite->SetSize({glyph.width * fit, glyph.height * fit});
            }

            visual.basePosition.x = firstIconX + kPromptIconSpacing * static_cast<float>(index);
            visual.baseSize = visual.sprite->GetSize();
        }
    }
}

void TutorialDirector::UpdatePrompt(float deltaTime) {
    // 指数応答で表示と非表示を補間し、フレーム時間が変わっても速度を揃える。
    const float targetAlpha = promptRequestedVisible_ ? 1.0f : 0.0f;
    // 応答係数に経過秒を掛け、そのフレームで残りの透明度差を何割詰めるか求める。
    const float response = 1.0f - std::exp(-kPromptFadeResponse * (std::max)(0.0f, deltaTime));
    promptAlpha_ += (targetAlpha - promptAlpha_) * response;

    // 目標との透明度差が0.002未満なら、その値にそろえて補間を終える。
    if (std::abs(targetAlpha - promptAlpha_) < 0.002f) {
        promptAlpha_ = targetAlpha;
    }

    // 十分に透明になってから非表示にする。表示中は透明度と一緒に最大18ピクセルのずれを戻す。
    const bool visible = promptAlpha_ > 0.002f;
    auto updateVisual = [this, visible](PromptVisual& visual) {
        // 配置されなかった案内部品はフェードや位置の更新を省く。
        if (!visual.sprite) {
            return;
        }

        visual.sprite->SetVisible(visible && visual.requestedVisible);
        engine::Vector4 color = visual.baseColor;
        color.w *= promptAlpha_;
        visual.sprite->SetColor(color);
        visual.sprite->SetPosition({visual.basePosition.x, visual.basePosition.y + (1.0f - promptAlpha_) * 18.0f});
        visual.sprite->SetSize(visual.baseSize);
    };

    updateVisual(promptPanel_);
    updateVisual(promptLabel_);

    for (size_t index = 0; index < promptIcons_.size(); ++index) {
        PromptVisual& icon = promptIcons_[index];
        updateVisual(icon);
    }
}

void TutorialDirector::SetPromptVisible(bool visible) {
    // フェードアウト中は即座に非表示にせず、透明になってからSpriteを隠す。
    promptRequestedVisible_ = visible;

    // 案内の表示を要求され背景があれば、次のフェード更新を待たず表示可能にする。
    if (visible && promptPanel_.sprite) {
        promptPanel_.sprite->SetVisible(true);
    }

    // 消す指定で透明度が閾値以下なら、生成済みの背景・説明・アイコンをまとめて隠す。
    if (!visible && promptAlpha_ <= 0.002f) {
        if (promptPanel_.sprite)
            promptPanel_.sprite->SetVisible(false);
        if (promptLabel_.sprite)
            promptLabel_.sprite->SetVisible(false);
        for (PromptVisual& icon : promptIcons_) {
            if (icon.sprite)
                icon.sprite->SetVisible(false);
        }
    }
}

void TutorialDirector::ApplyStepObjective(const Step* step) {
    // 課題の目標名を入れ替え、シーンに実在する対象だけマーカーを表示する。
    objectiveTargetNames_ = {};
    objectiveAlphas_ = {};
    objectiveOffsetY_ = 0.75f;
    objectiveAnimationTime_ = 0.0f;

    // 現手順がある場合は目標の高さと名前を読み直す。
    if (step) {
        objectiveOffsetY_ = step->objectiveOffsetY;

        // 複数指定がある場合に優先して枠を割り当て、単一指定しかなければ先頭枠へ入れる。
        if (!step->objectiveTargets.empty()) {
            const size_t targetCount = (std::min)(step->objectiveTargets.size(), objectiveTargetNames_.size());

            for (size_t index = 0; index < targetCount; ++index) {
                objectiveTargetNames_[index] = step->objectiveTargets[index];
            }
        } else if (!step->objectiveTarget.empty()) {
            objectiveTargetNames_[0] = step->objectiveTarget;
        }
    }

    const bool hasTarget =
        std::any_of(objectiveTargetNames_.begin(), objectiveTargetNames_.end(), [this](const std::string& targetName) {
            return !targetName.empty() && FindObjectByName(targetName) != nullptr;
        });
    SetObjectiveMarkerVisible(hasTarget);
}

void TutorialDirector::UpdateObjectiveMarker(float deltaTime) {
    // 目標の頭上を画面座標へ投影し、背面や画面外ではマーカーを消す。
    objectiveAnimationTime_ += (std::max)(0.0f, deltaTime);
    const float response = 1.0f - std::exp(-kObjectiveFadeResponse * (std::max)(0.0f, deltaTime));
    // 演出中はゲームの目標を映さないため、投影に使うカメラを取得しません。
    engine::Camera* camera =
        objectiveRequestedVisible_ && !cinematicActive_ ? CameraManager::GetInstance()->GetActiveCamera() : nullptr;
    const engine::Matrix4x4 viewProjection = camera ? camera->GetViewProjectionMatrix() : engine::Matrix4x4{};

    for (size_t index = 0; index < objectiveMarkers_.size(); ++index) {
        PromptVisual& marker = objectiveMarkers_[index];

        // 配置のないマーカーは投影と描画を省く。
        if (!marker.sprite) {
            continue;
        }

        // オブジェクトの消失や撃破を反映できるよう、保存した名前から現在の実体を探します。
        engine::Object3D* target =
            objectiveTargetNames_[index].empty() ? nullptr : FindObjectByName(objectiveTargetNames_[index]);
        bool canShow = camera && target && target->GetIsVisible();

        // 対象が死亡した敵なら、配置が残っていても表示しない。
        if (const auto* enemy = dynamic_cast<const BaseEnemy*>(target); enemy && enemy->IsDead()) {
            canShow = false;
        }

        engine::Vector2 screenPosition = marker.basePosition;

        // カメラと可視の対象が揃った場合だけ、対象の境界上端を投影する。
        if (canShow) {
            const engine::Aabb bounds = target->GetModelWorldAabb();
            const engine::Vector3 markerWorld = {(bounds.min.x + bounds.max.x) * 0.5f, bounds.max.y + objectiveOffsetY_,
                                         (bounds.min.z + bounds.max.z) * 0.5f};
            // 投影の分母が正のときだけ画面位置を計算し、カメラ背面にある点は棄却します。
            const float w = markerWorld.x * viewProjection.m[0][3] + markerWorld.y * viewProjection.m[1][3] +
                            markerWorld.z * viewProjection.m[2][3] + viewProjection.m[3][3];

            if (!std::isfinite(w) || w <= 0.001f) {
                canShow = false;
            // 分母が有効な場合に正規化座標を求め、画面周辺の許容範囲に入るか確認する。
            } else {
                const float ndcX = (markerWorld.x * viewProjection.m[0][0] + markerWorld.y * viewProjection.m[1][0] +
                                    markerWorld.z * viewProjection.m[2][0] + viewProjection.m[3][0]) /
                                   w;
                const float ndcY = (markerWorld.x * viewProjection.m[0][1] + markerWorld.y * viewProjection.m[1][1] +
                                    markerWorld.z * viewProjection.m[2][1] + viewProjection.m[3][1]) /
                                   w;

                canShow = std::isfinite(ndcX) && std::isfinite(ndcY) && ndcX >= -1.10f && ndcX <= 1.10f &&
                          ndcY >= -1.10f && ndcY <= 1.10f;
                // 描画領域の少し外までを候補として、表示位置を画面内の余白に収めます。
                if (canShow) {
                    const float screenWidth = static_cast<float>(WinApp::GetClientWidth());
                    const float screenHeight = static_cast<float>(WinApp::GetClientHeight());
                    // 中央が0の投影座標を左上原点の画素へ変換する。上が正の投影Yは、下が正の画面Yへ反転する。
                    screenPosition.x = (ndcX + 1.0f) * 0.5f * screenWidth;
                    screenPosition.y = (1.0f - ndcY) * 0.5f * screenHeight;
                    screenPosition.x =
                        (std::clamp)(screenPosition.x, kObjectiveScreenMargin, screenWidth - kObjectiveScreenMargin);
                    screenPosition.y =
                        (std::clamp)(screenPosition.y, kObjectiveScreenMargin, screenHeight - kObjectiveScreenMargin);
                }
            }
        }

        // 表示できる目標には透明度を追従させ、投影できなくなった場合は画面から外します。
        const float targetAlpha = canShow ? 1.0f : 0.0f;
        float& alpha = objectiveAlphas_[index];
        alpha += (targetAlpha - alpha) * response;

        // 透明度が目標へ十分近づいたら、目標値へそろえる。
        if (std::abs(targetAlpha - alpha) < 0.002f) {
            alpha = targetAlpha;
        }

        const bool visible = alpha > 0.002f && canShow;
        marker.sprite->SetVisible(visible);

        // 薄すぎるか投影条件を満たさない場合は表示と姿勢の更新を止める。
        if (!visible) {
            continue;
        }

        // 複数のマーカーが同期して動かないよう位相をずらし、上下の揺れと拡縮を加えます。
        const float phaseOffset = static_cast<float>(index) * 0.45f;
        const float bounce = std::sin(objectiveAnimationTime_ * 4.8f + phaseOffset);
        const float pulse = 1.0f + std::sin(objectiveAnimationTime_ * 3.6f + phaseOffset) * 0.045f;
        marker.sprite->SetPosition({screenPosition.x, screenPosition.y - 8.0f - bounce * 7.0f});
        marker.sprite->SetSize({marker.baseSize.x * pulse, marker.baseSize.y * pulse});
        engine::Vector4 color = marker.baseColor;
        color.w *= alpha;
        marker.sprite->SetColor(color);
    }
}

void TutorialDirector::SetObjectiveMarkerVisible(bool visible) {
    // 配置済みのマーカーに対して、表示要求があり透明度が閾値を超える場合だけ表示を許可する。
    objectiveRequestedVisible_ = visible;

    for (size_t index = 0; index < objectiveMarkers_.size(); ++index) {
        PromptVisual& marker = objectiveMarkers_[index];

        if (marker.sprite) {
            marker.sprite->SetVisible(visible && objectiveAlphas_[index] > 0.002f);
        }
    }
}

#ifdef USE_IMGUI
void TutorialDirector::DrawImGui() {
    // 実行中の手順と演出を確認し、開発時は任意の手順へ移動できる。
    // 押し込みが届いているかを、対象選択の成否とは分けて確認できます。
    if (inputManager_) {
        ImGui::Text("押し込み状態: L3 %s / R3 %s",
                    inputManager_->IsGamepadButtonPressed(XINPUT_GAMEPAD_LEFT_THUMB) ? "ON" : "OFF",
                    inputManager_->IsGamepadButtonPressed(XINPUT_GAMEPAD_RIGHT_THUMB) ? "ON" : "OFF");
        ImGui::TextDisabled("R3: ロックオン/解除、L3: 対象切替");
    }

    ImGui::Text("Flow: %s", loaded_ ? "Ready" : "Load Failed");
    ImGui::Text("Step: %d / %d", currentStepIndex_ + 1, static_cast<std::int32_t>(steps_.size()));
    ImGui::Text("ID: %s", GetCurrentStepId().c_str());
    ImGui::Text("Time: %.2f sec", stepTimer_);
    ImGui::Text("Cinematic: %s", cinematicActive_ ? "Playing" : "Stopped");
    ImGui::Text("Gate: %s", completed_ ? "Unlocked" : "Locked");

    // 先頭からの操作が押された場合、練習対象とゲートを開始前へ戻す。
    if (ImGui::Button("Flowを最初から")) {
        Restart();
    }

    ImGui::SameLine();

    // 前後ボタンを押し手順一覧がある場合は、端を超えない範囲で移動先を選ぶ。
    if (ImGui::Button("前のStep") && !steps_.empty()) {
        JumpToStep(steps_[static_cast<size_t>((std::max)(0, currentStepIndex_ - 1))].id);
    }

    ImGui::SameLine();

    if (ImGui::Button("次のStep") && !steps_.empty()) {
        const std::int32_t next = (std::min)(static_cast<std::int32_t>(steps_.size()) - 1, currentStepIndex_ + 1);
        JumpToStep(steps_[static_cast<size_t>(next)].id);
    }

    // 再初期化すると借用参照と完了通知が消えるため、再読込前に控えて渡し直します。
    if (ImGui::Button("Flow JSONを再読み込み")) {
        const std::string path = flowPath_;
        const CompletionCallback callback = completionCallback_;
        BaseScene* scene = scene_;
        Player* player = player_;
        engine::InputManager* inputManager = inputManager_;
        Initialize(scene, player, inputManager, path);
        completionCallback_ = callback;
    }

    // 直接移動の一覧を開いた場合だけIDを列挙し、選択された手順へ移る。
    if (ImGui::BeginCombo("Stepへ移動", GetCurrentStepId().c_str())) {
        for (const Step& step : steps_) {
            if (ImGui::Selectable(step.id.c_str(), step.id == GetCurrentStepId())) {
                JumpToStep(step.id);
            }
        }

        ImGui::EndCombo();
    }
}
#endif
