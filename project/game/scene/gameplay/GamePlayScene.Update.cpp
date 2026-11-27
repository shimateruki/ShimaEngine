#define NOMINMAX
#include <cstdint>
#include "game/scene/gameplay/GamePlayScene.h"
#include "game/scene/gameplay/GoalCinematicVfx.h"
#include "Easing.h"
#include <numbers>
#include "game/ui/ControlsGuideOverlay.h"
#include "game/ui/PauseMenuOverlay.h"
#include "game/ui/SettingsMenuOverlay.h"
#include "game/ui/SaveIndicatorOverlay.h"
#include "game/actor/enemy/boss/false_king/EnemyFalseKingSlime.h"
#include "SceneController.h"

#include "game/system/combat/BulletManager.h"
#include "engine/editor/graphics/camera/CameraEditor.h"
#include "CameraManager.h"
#include "CollisionManager.h"
#include "DebugConsole.h"
#include "Fade.h"
#include "GpuParticleManager.h"
#include "game/system/progress/GameDataManager.h"
#include "game/system/settings/GameSettingsManager.h"
#include "game/system/rules/GameRule.h"
#include "InputManager.h"
#include "KeyConfig.h"
#include "LightEditor.h"
#include "game/system/combat/LockOnSystem.h"
#include "MeshEffectManager.h"
#include "game/system/scene/ObjectManager.h"
#include "ParticleSystem.h"
#include "PostEffect.h"
#include "ProfilerManager.h"
#include "SceneManager.h"
#include "game/system/progress/StageManager.h"
#include "VfxSequencer.h"
#include "WinApp.h"
#include "game/actor/gimmick/stage/GimmickStageGate.h"
#include "game/actor/gimmick/trigger/GimmickBossGate.h"
#include "engine/utility/math/AnimationInterpolation.h"
#ifdef USE_IMGUI
#include "imgui.h"
#endif

#include <algorithm>
#include <cmath>
#include <limits>

#include "game/scene/gameplay/GamePlayScene.PresentationMath.h"
using namespace gamePlayPresentationMath;

namespace {
constexpr float kStageEntrySetupRetryTimeout = 2.0f;
constexpr auto kGoalJumpTrackName = "GoalClearJump";

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kTwoPi = kPi * 2.0f;
constexpr const char* kGoalCrownSparklePreset = "player/copy/crown/crown_goal_idle_sparkle";
constexpr const char* kGoalCrownTwinklePreset = "shared/crown_idle_twinkle";


engine::Vector3 GoalWorldScale(engine::Object3D* object) {
    // 対象を取得できなければ、スケールの補正係数として等倍を返す。
    if (!object) return {1,1,1};
    const auto& m=object->GetWorldMatrix();
    return {engine::Math::Length({m.m[0][0],m.m[0][1],m.m[0][2]}),
        engine::Math::Length({m.m[1][0],m.m[1][1],m.m[1][2]}),
        engine::Math::Length({m.m[2][0],m.m[2][1],m.m[2][2]})};
}
engine::Vector3 GoalLocalPosition(engine::Object3D* object, const engine::Vector3& world) {
    // ワールド座標で求めた装着位置を、SetTranslateが受け取る親基準の座標へ変換する。
    return object && object->GetParent()
        ? engine::Math::Transform(world,engine::Math::Inverse(object->GetParent()->GetWorldMatrix())) : world;
}
std::array<engine::Vector3,8> GoalWorldCorners(engine::Object3D* object) {
    // モデルの境界箱の8頂点をワールドへ変換し、カメラの収まり判定で輪郭を考慮できるようにする。
    std::array<engine::Vector3,8> result{};
    const auto lo=object->GetModel() ? object->GetModel()->GetLocalAabbMin() : engine::Vector3{-0.5f,-0.5f,-0.5f};
    const auto hi=object->GetModel() ? object->GetModel()->GetLocalAabbMax() : engine::Vector3{0.5f,0.5f,0.5f};
    for (size_t i=0;i<8;++i) result[i]=engine::Math::Transform(
        {(i&1)?hi.x:lo.x,(i&2)?hi.y:lo.y,(i&4)?hi.z:lo.z},object->GetWorldMatrix());
    return result;
}

} // namespace

// 生存中のプレイヤーのゴール接触を受理し、クリア状態に入る前だけ演出開始を要求する。
bool GamePlayScene::TryStartGoalFromContact(engine::Object3D* actor, engine::Object3D* goal) {
    Player* player = dynamic_cast<Player*>(actor);

    // プレイヤー以外の接触、死亡済み、またはHPが尽きた時点の接触は受理しない。
    if (!player || player->IsDead() || player->GetHp() <= 0.0f) {
        return false;
    }

    // クリア状態に入っていなければStartGoalPresentation()を呼ぶ。戻り値のtrueは接触の受理を示す。
    if (!IsGoal()) {
        StartGoalPresentation(goal);
    }

    DebugConsole::GetInstance()->AddLog("GOAL! Stage Cleared. Returning to Select.");
    return true;
}

void GamePlayScene::CollectStarCoinFromContact(std::int32_t coinIndex, const engine::Vector3& worldPosition) {
    // 接触で得た番号と位置を収集管理へ渡し、取得履歴をログへ残す。
    CollectStarCoin(coinIndex, worldPosition);
    DebugConsole::GetInstance()->AddLog("Star Coin " + std::to_string(coinIndex) + " Collected!");
}

void GamePlayScene::RecordStageCoinPickup() {
    // クリア状態へ入る前の取得だけを、今回の挑戦のコイン数に加える。
    if (!isGoal_) {
        ++stageCoinPickups_;
    }
}

void GamePlayScene::SetIsGoal(bool isGoal) {
    // 要求されたゴール状態が現在と同じなら、演出状態を再初期化せず戻る。
    if (isGoal_ == isGoal) {
        return;
    }

    isGoal_ = isGoal;

    // クリアを解除した場合は演出を停止し、開始前の姿勢・操作・カメラを復元する。
    if (!isGoal_) {
        goalCinematicPlayer_.Stop(false);
        goalCinematicPlayer_.RestorePresentationCamera();
        goalClearPlayerAnimator_.RestoreInitialPose();

        // 動かした王冠が残っていて開始前の値も保存済みなら、配置・材質・待機演出を復元する。
        if (goalCrownObject_ && goalCrownSnapshotValid_) {
            goalCrownObject_->SetTranslate(goalCrownBasePosition_);
            goalCrownObject_->SetScale(goalCrownBaseScale_);
            goalCrownObject_->SetRotation(goalCrownBaseRotation_);
            goalCrownObject_->SetEnableEnvMap(goalCrownBaseEnvMap_);
            goalCrownObject_->SetEnvIntensity(goalCrownBaseEnvIntensity_);
            goalCrownObject_->SetRoughness(goalCrownBaseRoughness_);
            goalCrownObject_->SetMeshEffect1Name(goalCrownIdleEffect1_);
            goalCrownObject_->SetMeshEffect2Name(goalCrownIdleEffect2_);
            goalCrownObject_->UpdateLocalMatrix();
            goalCrownObject_->UpdateWorldMatrix();
            goalCrownObject_->UpdateAttachedEffects(0.0f);
        }

        // 演出でプレイヤーの操作を固定していた場合は、ここで固定を解除する。
        if (player_ && player_->IsCinematicLocked()) {
            player_->EndCinematicLock(true);
        }

        // 演出時計と一度きりの通知フラグを初期化し、次のゴールで同じ節目を再生できるようにする。
        goalSavePerformed_ = false;
        goalResultRevealed_ = false;
        goalResultCoins_ = 0;
        goalResultStars_.fill(GoalResultStar::kEmpty);
        goalWasStageCleared_ = false;
        goalPresentationState_ = GoalPresentationState::kInactive;
        goalPresentationTimer_.Start();
        goalStarEmitTimer_ = 0.0f;
        goalBurstEmitTimer_ = 0.0f;
        goalPlayerSnapshotValid_ = false;
        goalCrownSnapshotValid_ = false;
        goalCameraSnapshotValid_ = false;
        goalReturnFadeStarted_ = false;
        goalLandingCuePlayed_ = false;
        goalResultCuePlayed_ = false;
        goalCrownObject_ = nullptr;
        goalClearPlayerAnimator_.Reset();
        RestoreGoalPresentationCameraInput();
        // 代替の結果カメラを使った場合は、元の視点へ戻してから破棄する。
        auto* cameraManager = CameraManager::GetInstance();
        if (goalPresentationCamera_ && cameraManager->GetActiveCameraOverride() == goalPresentationCamera_.get()) {
            cameraManager->SetActiveCamera(goalCameraRestoreOverride_);
        }
        goalPresentationCamera_.reset();
        goalCameraRestoreOverride_ = nullptr;
    }
}

// ゴール演出の入力ロックと初期姿勢を確定してから、演出用カメラへ制御を渡します。
void GamePlayScene::StartGoalPresentation(engine::Object3D* crownObject) {
    // ゴール済みで非アクティブ以外の演出状態にいる場合は、同じ演出を始め直さない。
    if (isGoal_ && goalPresentationState_ != GoalPresentationState::kInactive) {
        return;
    }

    // シーンのプレイヤーが不在、死亡済み、またはHPが0以下なら、操作を固定せず終了する。
    if (!player_ || player_->IsDead() || player_->GetHp() <= 0.0f) {
        return;
    }

    // クリア記録を更新する前に初回・再取得を控え、後から同じタイムラインで金色・銀色を切り替えられるようにする。
    const std::int32_t currentStage = StageManager::GetInstance()->GetCurrentStageIndex();
    goalWasStageCleared_ = GameDataManager::GetInstance()->IsStageCleared(currentStage);

    // 実プレイ時だけ、リザルトに表示する今回の経過秒数を取り込む。
    if (!goalEditorPreviewMode_) {
        goalResultClearSeconds_ = static_cast<std::int32_t>(stageElapsedSeconds_);
    }
    player_->BeginCinematicLock();
    SetIsGoal(true);
    goalPresentationState_ = GoalPresentationState::kCelebrating;
    goalPresentationTimer_.Start();
    goalStarEmitTimer_ = 0.0f;
    goalBurstEmitTimer_ = 0.0f;
    goalReturnFadeStarted_ = false;
    goalLandingCuePlayed_ = false;
    goalResultCuePlayed_ = false;
    goalCrownObject_ = crownObject;
    goalCrownPosition_ = crownObject ? crownObject->GetWorldPosition()
                                     : (player_ ? player_->GetWorldPosition() : engine::Vector3::kZero);
    goalCrownSnapshotValid_ = false;

    // 演出に使う王冠が配置されている場合は、動かす前の姿勢と材質を保存する。
    if (crownObject) {
        goalCrownStartWorldPosition_ = crownObject->GetWorldPosition();
        goalCrownBaseEnvMap_=crownObject->GetEnableEnvMap();
        goalCrownBaseEnvIntensity_=crownObject->GetEnvIntensity();
        goalCrownBaseRoughness_=crownObject->GetRoughness();
        // 待機オーラを外しても金属の面が読めるよう、演出中は環境反射を有効にする。
        crownObject->SetEnableEnvMap(true);
        crownObject->SetEnvIntensity((std::max)(0.85f,goalCrownBaseEnvIntensity_));
        crownObject->SetRoughness((std::min)(0.45f,goalCrownBaseRoughness_));
        goalCrownIdleEffect1_ = crownObject->GetMeshEffect1Name();
        goalCrownIdleEffect2_ = crownObject->GetMeshEffect2Name();
        goalCrownBasePosition_ = crownObject->GetTranslate();
        goalCrownBaseScale_ = crownObject->GetScale();
        goalCrownBaseRotation_ = crownObject->GetRotation();
        goalCrownSnapshotValid_ = true;
    }

    // 開始時に映っていたカメラ側を向くよう、プレイヤーから視点への水平成分を初期向きの候補にする。
    engine::Vector3 preferredForward = engine::Vector3::kZero;

    // プレイヤーの参照を取得できた場合だけ、視点に向く初期方向を調べる。
    if (player_) {
        engine::Camera* activeCamera = CameraManager::GetInstance()->GetActiveCamera();

        // アクティブなカメラがあれば、プレイヤーから視点への水平成分を初期の向きに使う。
        if (activeCamera) {
            const engine::Vector3 playerPosition = player_->GetWorldPosition();
            const engine::Vector3 toCamera = {activeCamera->GetEye().x - playerPosition.x, 0.0f,
                                      activeCamera->GetEye().z - playerPosition.z};
            preferredForward = NormalizePlanarDirection(toCamera, preferredForward);
        }
    }

    // 喜びモーションの開始に成功したら、王冠とカメラで共用するプレイヤーの姿勢と前後左右を取り込む。
    if (goalClearPlayerAnimator_.Start(player_, goalCrownPosition_, preferredForward)) {
        goalSavedPlayerControlActive_ = goalClearPlayerAnimator_.WasControlActive();
        goalPlayerBasePosition_ = goalClearPlayerAnimator_.GetBasePosition();
        goalPlayerBaseScale_ = goalClearPlayerAnimator_.GetBaseScale();
        goalPlayerBaseRotation_ = goalClearPlayerAnimator_.GetBaseRotation();
        goalPlayerPosePosition_ = player_->GetTranslate();
        goalMoveForward_ = goalClearPlayerAnimator_.GetMoveForward();
        goalMoveRight_ = goalClearPlayerAnimator_.GetMoveRight();
        goalPlayerSnapshotValid_ = true;
    // プレイヤーの演出開始に失敗した場合は、以前の姿勢の記録を無効にする。
    } else {
        goalPlayerSnapshotValid_ = false;
    }

    // 王冠とプレイヤーの両方にモデルがあれば、頭の幅と親の拡縮から王冠の装着倍率を求める。
    if (crownObject && crownObject->GetModel() && player_->GetModel()) {
        const auto bodySize=player_->GetModel()->GetLocalAabbMax()-player_->GetModel()->GetLocalAabbMin();
        const auto bodyScale=GoalWorldScale(player_);
        const auto crownSize=crownObject->GetModel()->GetLocalAabbMax()-crownObject->GetModel()->GetLocalAabbMin();
        const float width=(std::max)(bodySize.x*bodyScale.x,bodySize.z*bodyScale.z)*goalPresentationTuning_.crownWearWidthRatio;
        const float uniform=width/(std::max)({0.001f,crownSize.x,crownSize.z});
        const auto parentScale=GoalWorldScale(crownObject->GetParent());
        goalCrownWearScale_={uniform/(std::max)(parentScale.x,0.001f),uniform/(std::max)(parentScale.y,0.001f),uniform/(std::max)(parentScale.z,0.001f)};
    // モデルの寸法が取れない場合は、保存した王冠のスケールを装着時にも使う。
    } else {
        goalCrownWearScale_=goalCrownBaseScale_;
    }
    // 演出でカメラを差し替える前に、終了時に戻す上書き先を控える。
    LockGoalPresentationCameraInput();

    // タイムラインが読み込まれているときは、現在のシーンに合わせて役と演出の参照を結び直す。
    if (goalCinematicTimelineLoaded_) {
        // 保存済みの視点キーに現在のプレイヤーと王冠を割り当てる。
        if (player_ && crownObject) {
            goalCinematicSequence_.roleBindings[0] = {player_->GetName(), player_->GetEventId()};
            goalCinematicSequence_.roleBindings[1] = {crownObject->GetName(), crownObject->GetEventId()};
        }

        auto& presentation = goalCinematicSequence_.presentation;
        // シーン内での再試行ではワールドへ変換済みのキーを再利用せず、作者の相対キーを取り直す。
        if (presentation.viewWorldSpace && !presentation.view.empty()) {
            CinematicSequence authored;
            if (authored.Load("Resources/json/scenario/shared/goal_clear_timeline.json")) {
                presentation.view = std::move(authored.presentation.view);
                presentation.viewWorldSpace = authored.presentation.viewWorldSpace;
            }
        }
        // 王冠への向きとプレイヤーの正面が異なる地点でも、設計した視点がプレイヤーの背面に回らないようにする。
        if (!presentation.viewWorldSpace && !presentation.view.empty()) {
            const auto toWorldOffset = [this](const engine::Vector3& offset) {
                return goalMoveRight_ * offset.x + goalMoveForward_ * offset.z + engine::Vector3{0, offset.y, 0};
            };
            for (auto& key : presentation.view) {
                key.focusOffset = toWorldOffset(key.focusOffset);
                key.eyeOffset = toWorldOffset(key.eyeOffset);
                key.jitter = toWorldOffset(key.jitter);
            }
            presentation.viewWorldSpace = true;
        }

        // 保存済み演出の役名を今のPlayerと王冠へ結び直す。再取得した王冠ではIDが変わり得る。
        for (auto& clip : goalCinematicSequence_.animationClips) {
            // GoalClearPlayer用のクリップは、プレイヤーが存在するときに今の名前とイベントIDを設定する。
            if (clip.driver == "GoalClearPlayer" && player_) {
                clip.binding.targetName = player_->GetName();
                clip.binding.targetEventId = player_->GetEventId();
            }
        }

        // 色違いの演出素材を選び、王冠を追うものとプレイヤーを追うものをそれぞれ結び直す。
        for (auto& track : goalCinematicSequence_.vfxTracks) {
            const auto* cue = goalCinematicVfx::FindCue(track.sequenceName);
            // 金・銀の切り替えに対応する演出が見つかった場合だけ、クリア履歴に合う名前へ差し替える。
            if (cue) {
                track.sequenceName = goalWasStageCleared_ ? cue->silver : cue->gold;
            }

            engine::Object3D* target = cue && cue->followsCrown ? crownObject : player_;

            // 追従対象の参照が取れたトラックだけ、今の名前とイベントIDに結び直す。
            if (target) {
                track.binding.targetName = target->GetName();
                track.binding.targetEventId = target->GetEventId();
            }
        }

        // 前回の再生用コピーを差し替え、作成時の前・横方向を今の取得方向へ合わせる。
        auto& tracks = goalCinematicSequence_.transformTracks;
        std::erase_if(tracks, [](const auto& track) { return track.name == kGoalJumpTrackName; });
        CinematicSequence jump;
        if (player_ && jump.Load("Resources/json/scenario/shared/goal_clear_jump_prototype.json") &&
            !jump.transformTracks.empty() && !jump.transformTracks.front().keys.empty()) {
            auto track = std::move(jump.transformTracks.front());
            track.name = kGoalJumpTrackName;
            track.binding = {player_->GetName(), player_->GetEventId()};
            const auto& timing = goalClearPlayerAnimator_.GetTuning();
            track.startTime = timing.crownLandTime;
            // 元のキー区間を、今のタイムラインの溜め・離陸・頂点・着地へ合わせる。
            const float source[] = {0.0f, 0.20f, 0.44f, 0.80f, 1.18f};
            const float destination[] = {0.0f, timing.anticipationStartTime - track.startTime,
                timing.jumpStartTime - track.startTime, timing.apexTime - track.startTime,
                timing.victoryLandTime - track.startTime};
            for (auto& key : track.keys) {
                auto segment = std::upper_bound(std::begin(source), std::end(source), key.time);
                // 軌道の着地後は、キーで作った着地の収まり時間を保持する。
                if (segment == std::end(source)) {
                    key.time += destination[4] - source[4];
                } else {
                    const auto index = (std::max)(segment - std::begin(source) - 1, std::ptrdiff_t{0});
                    const float rate = animationInterpolation::SegmentRate(key.time, source[index], source[index + 1]);
                    key.time = std::lerp(destination[index], destination[index + 1], rate);
                }
                const float lean = key.rotation.x;
                key.position = goalMoveRight_ * key.position.x + goalMoveForward_ * key.position.z +
                    engine::Vector3{0.0f, key.position.y, 0.0f};
                key.rotation.x = goalMoveForward_.z * lean;
                key.rotation.z = -goalMoveForward_.x * lean;
            }
            tracks.push_back(std::move(track));
        } else {
            DebugConsole::GetInstance()->AddLog("Goal jump track could not be loaded.");
        }

    }
    if (goalCinematicTimelineLoaded_) {
        // 編集用Sequenceは借用したまま、変更した対象とVFXを共通処理で準備する。
        goalCinematicTimelineLoaded_ = goalCinematicPlayer_.Prepare();
        // 再準備に成功した場合に再生を始め、ゴール演出の時計をタイムラインに合わせる。
        if (goalCinematicTimelineLoaded_) {
            // 相対キーの基準姿勢を床面で取得する。初回評価の接地処理が実際の取得高度を戻す。
            if (player_ && !goalCinematicSequence_.transformTracks.empty() &&
                goalCinematicSequence_.transformTracks.back().name == kGoalJumpTrackName) {
                player_->SetTranslate(goalClearPlayerAnimator_.GetBasePosition());
                player_->SetScale(goalClearPlayerAnimator_.GetBaseScale());
                player_->UpdateWorldMatrix(true);
            }
            goalCinematicPlayer_.SetPresentationCameraBlend(goalPresentationTuning_.crownFocusEndTime);
            goalCinematicPlayer_.Play(false);
            goalPresentationTimer_.Seek(goalCinematicPlayer_.GetCurrentTime());
        }
    }

    // 視点キーを使えないときだけ、従来の代替カメラで演出を始める。
    if (!goalCinematicPlayer_.GetPresentationCamera()) {
        SetupGoalPresentationCamera();
    } else {
        UpdateGoalPresentationCamera();
    }
    UpdateGoalPresentationOverlay();
}

// 演出モードは互いに排他的です。早期終了で通常入力と敵更新の二重実行を防ぎます。
void GamePlayScene::Update(float deltaTime) {
    // ゲートの入場処理がフレームを使う間は、HUDだけ進めて通常更新を待つ。
    if (GimmickStageGate::UpdateSceneEntry(*this, deltaTime)) {
        UpdateUi(deltaTime);
        return;
    }
    // ムービープレビューはEditorが時間とエフェクトを所有し、通常AI・衝突を進めません。
    if (crownMoviePreview_.active) {
        // エディターが停止中なら静止プレビューだけを反映し、このフレームの通常更新を終える。
        if (!SceneManager::GetInstance()->IsPlaying()) {
            StepCrownMoviePreview(0.0f);
            return;
        }

        // プレビューを開いた状態で本番の再生へ入ったら、プレビューを閉じて専用演出の優先判定へ進む。
        EndCrownMoviePreview();
    }

    // 偽王の撃破演出がフレームを使った場合は、継承や通常の進行へ進まない。
    if (UpdateCrownDefeatIfActive(deltaTime)) {
        return;
    }

    // 継承用に組み替えたシーンの状態は、エディターへ戻った時点で復元する。
    if (crownFinale_.prepared && !SceneManager::GetInstance()->IsPlaying()) {
        ResetCrownFinaleWorld();
    }

    // 継承・闘技場の演出で使う共通順序。ポスト効果、演出、シーン内の物体、UIの順に進める。
    const auto updatePresentationWithScene = [&](auto updatePresentation) {
        UpdatePostEffectState(deltaTime);
        updatePresentation();
        UpdateSceneSystems(deltaTime);
        UpdateUi(deltaTime);
    };

    // 継承中も演出のオブジェクトを進めるため、継承の更新後にシーンシステムを呼ぶ。
    if (crownFinale_.active) {
        updatePresentationWithScene([&] { UpdateCrownFinale(deltaTime); });
        return;
    }

    bool runtimePlaying = true;
#ifdef USE_IMGUI
    // シーン管理を取得できる場合は、エディターの再生状態を通常更新の判断に使う。
    if (SceneManager* sceneManager = GetSceneManager()) {
        runtimePlaying = sceneManager->IsPlaying();
    }

    // 再生中だけ、ImGui描画で予約された移動を通常更新より先に一度反映する。
    if (runtimePlaying) {
        ProcessPendingDebugTeleport();
    // 再生を止めた場合は、次のプレイで古い移動要求を使わないよう捨てる。
    } else {
        pendingDebugTeleport_.reset();
    }
#endif

    // ゲーム再生を停止したら、槍のカットを閉じてカメラの制御を戻す。
    if (!runtimePlaying) {
        FinishSealBarrageMovie();
    }
    // 入場演出がフレームを使った間は、ゴール処理や通常の操作へ進まない。
    if (UpdateStageEntryIfActive(deltaTime, runtimePlaying)) {
        return;
    }

    // ゴール処理が有効なフレームは、リザルト処理が時間と入力を受け持つ。
    if (UpdateGoalClearIfActive(deltaTime)) {
        return;
    }

    // ボス登場中は演出とシーン内オブジェクトを進め、通常の操作入力より先に抜ける。
    if (arenaBossIntroActive_) {
        updatePresentationWithScene([&] { UpdateArenaBossIntro(deltaTime); });
        return;
    }

    // 報酬中は受け取りの演出とシーン内オブジェクトを進め、通常の操作入力より先に抜ける。
    if (arenaBossRewardActive_) {
        updatePresentationWithScene([&] { UpdateArenaBossDefeatReward(deltaTime); });
        return;
    }

    // 操作ガイドが入力を受け取ったフレームは、ポーズやゲーム内操作に同じ入力を渡さない。
    if (HandleControlsGuideOverlay(deltaTime)) {
        return;
    }

    // ポーズがフレームを使った場合は、プレイ時間の加算と通常進行を保留する。
    if (HandlePauseOverlay(deltaTime)) {
        return;
    }

    // ここまで到達したフレームを全体のプレイ時間へ加算する。今回のステージ記録には下の条件を別に適用する。
    GameDataManager::GetInstance()->AdvancePlayTime(deltaTime);

    // 生存して操作可能な実プレイで、残機喪失中や演出ロック中を除き、有効な経過秒数だけ今回の時間に足す。
    if (runtimePlaying && player_ && player_->IsControlActive() && !player_->IsDead() &&
        !player_->IsCinematicLocked() && !lifeLostState_.IsActive() && std::isfinite(deltaTime) && deltaTime > 0.0f) {
        // 表示は最大5桁の分を収めるため、到達できる範囲の秒数に収める。
        stageElapsedSeconds_ = std::min(stageElapsedSeconds_ + deltaTime, 5999999.0);
    }

    // 入力を使うUIを抜け、シーン固有のコントローラーがあればその更新を呼ぶ。
    if (sceneController_) {
        sceneController_->OnUpdate(*this, deltaTime);
    }

    UpdatePostEffectState(deltaTime);

    // 残機喪失の処理中はUIを進めながら、通常のカメラとシーンの更新を待つ。
    if (lifeLostState_.IsActive()) {
        UpdateUi(deltaTime);
        return;
    }

    // このフレームのロックオン対象とカメラを更新してから、シーン内の物体を進める。
    UpdateLockOnAndCamera(deltaTime);
    UpdateGoalCrownIdleAnimation(deltaTime);
    UpdateSceneSystems(deltaTime);

    // 敵の更新で確定した槍の位置を使ってカットを進め、同じフレームでUIとマーカーに反映する。
    UpdateSealBarrageMovie(deltaTime);
    UpdateUi(deltaTime);

    // ロックオン中なら、対象と実際に使うカメラの更新結果でマーカーを画面に投影する。
    if (lockOnSystem_ && lockOnSystem_->IsLockingOn()) {
        UpdateLockOnSprite(CameraManager::GetInstance()->GetActiveCamera(), lockOnSystem_->GetTarget(), deltaTime);
    // 有効なロックオンがなければ、前のフレームのマーカーを表示しない。
    } else {
        isDrawLockOn_ = false;

        // マーカーのSpriteが作成されている場合は、その表示フラグも解除する。
        if (lockOnSprite_) {
            lockOnSprite_->SetVisible(false);
        }
    }

    UpdateEffectDebugShortcuts();

    // 確認用のアニメーションキューブを作成済みの場合は、通常更新の末尾で進める。
    if (animatedCube_) {
        animatedCube_->Update(deltaTime);
    }
}

bool GamePlayScene::UpdateCrownDefeatIfActive(float deltaTime) {
    // 編集モードへ戻ったら撃破演出を破棄し、再生中にだけ演出用の表示を更新する。
    if (!SceneManager::GetInstance()->IsPlaying() && (crownDefeat_.active || crownDefeat_.completed)) {
        ResetCrownDefeat();
    }
    // 撃破演出が再生中なら、専用演出と表示だけを更新してフレームを引き取る。
    if (crownDefeat_.active) {
        UpdatePostEffectState(deltaTime);
        UpdateCrownDefeat(deltaTime);

        // 撃破演出中もCPU粒子システムがあれば進める。衝突やAIはこの経路では呼ばない。
        if (particleSystem_) {
            particleSystem_->Update(deltaTime);
        }

        GpuParticleManager::GetInstance()->Update(deltaTime);

        for (auto& object : GetObjects()) {
            object->UpdateWorldMatrix(true);
        }

        UpdateUi(deltaTime);
        return true;
    }

    return false;
}

bool GamePlayScene::UpdateStageEntryIfActive(float deltaTime, bool runtimePlaying) {
    // 再生停止で入場を終了し、次回の再生切り替えを検出できるようにする。
    if (!runtimePlaying) {
        stageEntryRuntimeWasPlaying_ = false;

        // 停止した時点で入場演出が進行中なら、その演出を終了する。
        if (stageEntryPresentationActive_) {
            FinishStageEntryPresentation();
        }
    // 再生中は、入場演出の開始要求と準備の再試行を扱う。
    } else {
        // Play In Editor はSceneを再読み込みしないため、停止状態から再生へ変わった瞬間も
        // OnActivatedと同じ入口演出を明示的に要求します。
        if (!stageEntryRuntimeWasPlaying_) {
            stageEntryRuntimeWasPlaying_ = true;
            stageEntryPresentationCompleted_ = false;
            stageEntryHadPlayerControl_ = player_ ? player_->IsControlActive() : true;
            stageEntryPresentationPending_ = true;
            stageEntryPresentationRetryTimer_ = 0.0f;
        }

        // 開始要求があり、進行中でも完了済みでもなければ、必要な入口とプレイヤーを揃えられるまで再試行する。
        if (stageEntryPresentationPending_ && !stageEntryPresentationActive_ && !stageEntryPresentationCompleted_) {
            // 入口がまだ準備できない場合は、待った時間を加算し、通常更新を保留する。
            if (!StartStageEntryPresentation()) {
                stageEntryPresentationRetryTimer_ += std::max(deltaTime, 0.0f);

                // プレイヤーを取得できている場合は、待機中に移動や表示が進まないよう固定する。
                if (player_) {
                    player_->SetIsVisible(false);
                    player_->SetIsControlActive(false);
                    player_->SetVelocity({0.0f, 0.0f, 0.0f});
                }

                // 準備の待ち時間が上限に達するまでは、ポスト効果とUIだけを進めて次のフレームで再試行する。
                if (stageEntryPresentationRetryTimer_ < kStageEntrySetupRetryTimeout) {
                    UpdatePostEffectState(deltaTime);
                    UpdateUi(deltaTime);
                    return true;
                }

                // 時間切れでは入口なしでも通常開始できるよう、待機要求と時計を解除する。
                stageEntryPresentationPending_ = false;
                stageEntryPresentationCompleted_ = true;
                stageEntryPresentationRetryTimer_ = 0.0f;

                // 時間切れの時点でプレイヤーがいれば、表示と入場前の操作状態を戻す。
                if (player_) {
                    player_->SetIsVisible(true);
                    player_->SetIsControlActive(stageEntryHadPlayerControl_);
                }

                DebugConsole::GetInstance()->AddLog(
                    "Stage Entry Warning: entrance gate presentation could not be prepared.");
            }
        }
    }

    // 入口演出は通常のプレイヤー更新より先に進め、操作不能な状態を保ちます。
    if (stageEntryPresentationActive_) {
        UpdatePostEffectState(deltaTime);
        UpdateStageEntryPresentation(deltaTime);
        UpdateUi(deltaTime);
        return true;
    }

    return false;
}

void GamePlayScene::UpdateGoalCrownIdleAnimation(float deltaTime) {
    // 物体管理がない場合やクリア演出に入った場合は、王冠の待機モーションを更新しない。
    if (!objectManager_ || isGoal_) {
        return;
    }

    constexpr float kRotationSpeed = 0.62f;
    constexpr float kBobAngularSpeed = 1.75f;
    constexpr float kBobAmplitude = 0.14f;
    constexpr float kFarSparkleInterval = 0.34f;
    constexpr float kNearSparkleInterval = 0.18f;
    constexpr float kNearDistance = 6.0f;
    constexpr float kFarDistance = 18.0f;
    constexpr std::array<engine::Vector3, 8> kSparkleOffsets = {engine::Vector3{0.00f, 0.52f, -0.05f}, engine::Vector3{-0.58f, 0.39f, -0.08f},
                                                        engine::Vector3{0.58f, 0.39f, -0.08f}, engine::Vector3{-0.92f, 0.05f, -0.10f},
                                                        engine::Vector3{0.92f, 0.05f, -0.10f}, engine::Vector3{-0.70f, -0.20f, 0.16f},
                                                        engine::Vector3{0.70f, -0.20f, 0.16f}, engine::Vector3{0.00f, 0.28f, 0.58f}};

    // 上下動は前フレームとの差分だけ加え、配置の高さに揺れを積み重ねない。
    const float previousTime = goalCrownIdleTime_;
    goalCrownIdleTime_ += deltaTime;
    goalCrownSparkleTimer_ -= deltaTime;

    const float previousBob = std::sin(previousTime * kBobAngularSpeed) * kBobAmplitude;
    const float currentBob = std::sin(goalCrownIdleTime_ * kBobAngularSpeed) * kBobAmplitude;
    const float bobDelta = currentBob - previousBob;
    const bool emitSparkle = goalCrownSparkleTimer_ <= 0.0f;
    const float emissivePulse = (std::sin(goalCrownIdleTime_ * 2.4f) + 1.0f) * 0.5f;
    const bool isCleared =
        GameDataManager::GetInstance()->IsStageCleared(StageManager::GetInstance()->GetCurrentStageIndex());
    bool foundGoalCrown = false;
    float strongestNearRate = 0.0f;

    for (auto& object : objectManager_->GetObjects()) {
        // 偽王の報酬はイベント受信側で動くため、通常の浮遊・発光を重ねない。
        if (!object || object->GetEventType() != EventType::kGoal ||
            object->GetName() == kFalseKingRewardName) {
            continue;
        }

        foundGoalCrown = true;

        // 接近時は発光を強め、場面内で最も近い王冠を粒子の発生頻度の基準にする。
        float nearRate = 0.0f;

        // プレイヤーがいる場合は、距離から発光の強さと粒子の発生間隔に使う接近率を求める。
        if (player_) {
            const engine::Vector3 toCrown = object->GetWorldPosition() - player_->GetWorldPosition();
            const float distance = std::sqrt(toCrown.x * toCrown.x + toCrown.y * toCrown.y + toCrown.z * toCrown.z);
            nearRate = 1.0f - std::clamp((distance - kNearDistance) / (kFarDistance - kNearDistance), 0.0f, 1.0f);
            strongestNearRate = std::max(strongestNearRate, nearRate);
        }

        // 配置の姿勢へ上下動と回転を足し、今フレームのワールド位置を先に確定する。
        engine::Vector3 position = object->GetTranslate();
        position.y += bobDelta;
        object->SetTranslate(position);

        engine::Vector3 rotation = object->GetRotation();
        rotation.y += kRotationSpeed * deltaTime;
        object->SetRotation(rotation);
        object->SetEmissive(isCleared ? 1.20f + emissivePulse * 0.38f + nearRate * 0.14f
                                      : 1.62f + emissivePulse * 0.62f + nearRate * 0.48f);
        object->UpdateLocalMatrix();
        object->UpdateWorldMatrix();

        const engine::Matrix4x4& crownWorld = object->GetWorldMatrix();

        // 粒子の発生時刻に達したときは、王冠の回転後のワールド位置に輝きを出す。
        if (emitSparkle) {
            const engine::Vector3& localOffset = kSparkleOffsets[goalCrownSparklePatternIndex_ % kSparkleOffsets.size()];
            GpuParticleManager::GetInstance()->Emit(kGoalCrownSparklePreset, engine::Math::Transform(localOffset, crownWorld));

            const bool emitHeroTwinkle =
                goalCrownSparklePatternIndex_ % 4 == 0 || (nearRate >= 0.55f && goalCrownSparklePatternIndex_ % 2 == 0);
            // 4回ごとの発生、または十分に近づいたときの偶数回では、別の位置に強い輝きも出す。
            if (emitHeroTwinkle) {
                const engine::Vector3& twinkleOffset =
                    kSparkleOffsets[(goalCrownSparklePatternIndex_ + 3) % kSparkleOffsets.size()];
                GpuParticleManager::GetInstance()->Emit(kGoalCrownTwinklePreset,
                                                        engine::Math::Transform(twinkleOffset, crownWorld));
            }
        }
    }

    // 王冠が見つからなければ、粒子の次回時刻やオフセットの巡回を進めない。
    if (!foundGoalCrown) {
        return;
    }

    // 粒子を出したフレームだけ、最も近い王冠に合わせて次回時刻を決め、発生位置を次へ進める。
    if (emitSparkle) {
        goalCrownSparkleTimer_ +=
            kFarSparkleInterval + (kNearSparkleInterval - kFarSparkleInterval) * strongestNearRate;
        ++goalCrownSparklePatternIndex_;
    }
}

bool GamePlayScene::HandleControlsGuideOverlay(float deltaTime) {
    // スライド中は両画面の位置を更新し、場面が落ち着くまで他の操作に入力を渡さない。
    if (pausePageSliding_) {
        UpdatePausePageSlide(deltaTime);
        return true;
    }

    // 操作ガイドが開いている間はゲーム入力を消費し、通常の行動更新へ通さない。
    if (controlsGuideOverlay_ && controlsGuideOverlay_->IsActive()) {
        // ポーズから開いたガイドでSTART入力を受けたときは、ガイドとポーズのセッションを閉じる。
        if (pauseGuideSession_ && inputManager_ &&
            inputManager_->IsGamepadButtonTriggered(XINPUT_GAMEPAD_START)) {
            controlsGuideOverlay_->SetActive(false);
            pauseGuideSession_ = false;
            return true;
        }

        controlsGuideOverlay_->SetPlayer(player_);
        controlsGuideOverlay_->Update(deltaTime);

        // ポーズから開いたガイドだけ、能力ページの端からポーズメニューへ続けて移動できる。
        if (pauseGuideSession_ && controlsGuideOverlay_->ConsumeMenuPageRequest()) {
            BeginPausePageSlide(true);
            return true;
        }

        // ガイド側の更新で閉じられた場合は、開き方を記録していたフラグも消す。
        if (!controlsGuideOverlay_->IsActive()) {
            pauseGuideSession_ = false;
        }

        return true;
    }

    return false;
}

void GamePlayScene::BeginPausePageSlide(bool toMenu) {
    // スライドには移動元と移動先の両方が必要なので、どちらかがなければ開始しない。
    if (!controlsGuideOverlay_ || !pauseMenuOverlay_)
        return;
    pausePageSliding_ = true;
    pausePageToMenu_ = toMenu;
    pausePageSlideTime_ = 0.0f;
    // ポーズメニューへ向かう場合は、移動先のメニューを先に開く。
    if (toMenu)
        pauseMenuOverlay_->SetActive(true);
    // 能力ページへ戻る場合は、移動先のガイドを先に開く。
    else controlsGuideOverlay_->OpenAbilityPage(false);
    UpdatePausePageSlide(0.0f);
}

void GamePlayScene::UpdatePausePageSlide(float deltaTime) {
    // スライド途中で片方の画面を取得できなくなったら、進行中の状態を解除する。
    if (!controlsGuideOverlay_ || !pauseMenuOverlay_) {
        pausePageSliding_ = false;
        return;
    }

    // 二つのページを0.34秒で同時に動かす。完了フレームで移動元だけを閉じる。
    pausePageSlideTime_ += std::max(0.0f, deltaTime);
    const float t = std::clamp(pausePageSlideTime_ / 0.34f, 0.0f, 1.0f);
    const float eased = Easing::SmoothStep(t);
    const float distance = static_cast<float>(WinApp::GetClientWidth()) * 0.86f;
    controlsGuideOverlay_->Update(deltaTime, false);
    pauseMenuOverlay_->Update(deltaTime, false);
    controlsGuideOverlay_->SetMenuTransitionOffset(-distance * (pausePageToMenu_ ? eased : 1.0f - eased), true);
    pauseMenuOverlay_->SetGuideTransitionOffset(distance * (pausePageToMenu_ ? 1.0f - eased : eased), true);

    // 遷移の最後に両ページの座標を戻し、出ていく側だけを閉じて次の入力を受け付ける。
    if (t >= 1.0f) {
        pausePageSliding_ = false;
        controlsGuideOverlay_->SetMenuTransitionOffset(0.0f, false);
        pauseMenuOverlay_->SetGuideTransitionOffset(0.0f, false);
        // ガイドからメニューへ着いた場合は、移動元のガイドを閉じる。
        if (pausePageToMenu_)
            controlsGuideOverlay_->SetActive(false);
        // メニューからガイドへ着いた場合は、移動元のメニューを閉じる。
        else pauseMenuOverlay_->SetActive(false);
    }
}

bool GamePlayScene::HandlePauseOverlay(float deltaTime) {
    // ポーズ中はUIだけ進め、ゲーム世界の更新と入力を止める。
    // 設定を重ねている間は設定画面が入力を受け取り、背面のポーズメニューを動かさない。
    if (settingsOverlay_ && settingsOverlay_->IsActive()) {
        settingsOverlay_->Update(deltaTime);
        return true;
    }

    // 設定画面が入力を取っておらずポーズメニューが開いていれば、メニューの選択を処理する。
    if (pauseMenuOverlay_ && pauseMenuOverlay_->IsActive()) {
        const auto action = pauseMenuOverlay_->Update(deltaTime);

        // 設定とタイトルへの操作は共有し、このフレームのワールド更新を止めたままにする。
        if (HandleSharedPauseAction(action, *pauseMenuOverlay_, settingsOverlay_.get())) {
            return true;
        }

        switch (action) {
        // ポーズのセッションを閉じ、次のフレームからゲーム操作を受け付ける。
        case PauseMenuOverlay::Action::kResume:
            pauseMenuOverlay_->SetActive(false);
            pauseGuideSession_ = false;
            break;

        // ポーズ中のガイドとして開き、二画面間のスライドを始める。
        case PauseMenuOverlay::Action::kOpenGuide:
            // ガイドを表示できる場合に限り、ポーズ中の能力ページを移動先として開く。
            if (controlsGuideOverlay_) {
                controlsGuideOverlay_->SetPlayer(player_);
                controlsGuideOverlay_->SetPauseMode(true);
                BeginPausePageSlide(false);
                pauseGuideSession_ = true;
            }

            break;

        // 現在ステージをロード画面なしで読み直す。
        case PauseMenuOverlay::Action::kRetry:
            pauseMenuOverlay_->SetActive(false);
            SceneManager::GetInstance()->ChangeSceneWithoutLoading("GAMEPLAY");
            break;

        // ポーズを閉じてステージ選択へ移る。
        case PauseMenuOverlay::Action::kStageSelect:
            pauseMenuOverlay_->SetActive(false);
            SceneManager::GetInstance()->ChangeScene("SELECT");
            break;

        case PauseMenuOverlay::Action::kNone:
        default:
            break;
        }

        return true;
    }

    // ポーズを新しく開く入力があった場合は、画面の表示先を選ぶ。
    if (IsPauseOpenTriggered()) {
        // ガイドが用意されていれば、ポーズ中の能力ページを入口にする。
        if (controlsGuideOverlay_) {
            controlsGuideOverlay_->SetPlayer(player_);
            controlsGuideOverlay_->SetPauseMode(true);
            controlsGuideOverlay_->OpenAbilityPage();
            pauseGuideSession_ = true;
        // ガイドがない構成でメニューがあれば、メニューを直接開く。
        } else if (pauseMenuOverlay_) {
            pauseMenuOverlay_->SetActive(true);
        }

        return true;
    }

    return false;
}

bool GamePlayScene::IsPauseOpenTriggered() const {
    // 入力管理を使えない場合は、新しいポーズ要求を検出できない。
    if (!inputManager_) {
        return false;
    }

#ifdef USE_IMGUI
    ImGuiIO& io = ImGui::GetIO();

    // ImGuiがキーボードや文字入力を使用中なら、同じ入力をポーズに渡さない。
    if (io.WantCaptureKeyboard || io.WantTextInput) {
        return false;
    }
#endif

    // 残機喪失の処理中・暗転保持中・ゴール中は、演出側が画面を制御するのでポーズを開かない。
    if (lifeLostState_.IsActive() || lifeLostState_.HoldsBlack() || isGoal_) {
        return false;
    }

    return inputManager_->IsKeyTriggered(DIK_TAB) ||
           inputManager_->IsGamepadButtonTriggered(XINPUT_GAMEPAD_START);
}

bool GamePlayScene::UpdateGoalClearIfActive(float deltaTime) {
    // ゴール状態に入っていなければ、通常の更新に処理を戻す。
    if (!isGoal_) {
        return false;
    }

    SaveGoalResultIfNeeded();
    UpdateGoalClearPresentation(deltaTime);
    HandleGoalResultConfirmInput();

    // 暗転完了後にステージ選択へ切り替える。暗転なしの経路はすぐに予約する。
    if (goalPresentationState_ == GoalPresentationState::kReturning &&
        (!goalReturnFadeStarted_ || Fade::GetInstance()->IsFinished())) {
        SceneManager::GetInstance()->ChangeSceneAfterFade("SELECT");
    }

    // 保存結果にかかわらず、ゴール中は通常更新へ進めない。
    return true;
}

void GamePlayScene::SaveGoalResultIfNeeded() {
    // 保存成功後と帰還開始後は、記録や報酬通知を重複して処理しない。
    if (goalSavePerformed_ || goalPresentationState_ == GoalPresentationState::kReturning) {
        return;
    }

    std::int32_t currentStage = StageManager::GetInstance()->GetCurrentStageIndex();
    auto* gameData = GameDataManager::GetInstance();
    const bool wasStageCleared = gameData->IsStageCleared(currentStage);
    const std::int32_t previousCrownCount = gameData->GetClearedStageCount();
    std::vector<std::int32_t> newStarCoinIndices;

    // 保存前に既取得と今回初取得を区別し、リザルト表示と帰還時の獲得通知に使う。
    for (std::int32_t i = 0; i < 3; ++i) {
        // 保存済みのスターコインは、今回のリザルトで取得済みとして表示する。
        if (gameData->IsStarCoinCollected(currentStage, i)) {
            goalResultStars_[i] = GoalResultStar::kOwned;
        // 今回だけの取得は新規として表示し、帰還先へ通知する候補にも入れる。
        } else if (sessionStarCoins_[i]) {
            goalResultStars_[i] = GoalResultStar::kNew;
            newStarCoinIndices.push_back(i);
        }
    }
    goalResultCoins_ = stageCoinPickups_;

    const std::array<bool, GameDataManager::kStarCoinsPerStage> collected = {
        sessionStarCoins_[0], sessionStarCoins_[1], sessionStarCoins_[2]};
    // クリア記録と今回のスターを保存できなければ、再試行か帰還を選ぶダイアログを出す。
    if (!gameData->CommitStageClear(currentStage, collected)) {
        const std::int32_t choice = ::MessageBoxW(
            nullptr,
            L"ステージクリアを保存できませんでした。保存先を確認してください。\n\n"
            L"「再試行」: もう一度保存します。\n"
            L"「キャンセル」: クリア記録と今回のスターコインを保存せず、ステージ選択へ戻ります。",
            L"セーブエラー", MB_RETRYCANCEL | MB_ICONERROR);
        // 再試行を選んだ場合は次のフレームで再保存する。ほかの結果なら保存せず帰還する。
        if (choice != IDRETRY) {
            RequestGoalReturnToSelect();
        }
    // 保存できた場合は、更新後の王冠数から帰還先で見せる演出を決める。
    } else {
        const std::int32_t newCrownCount = gameData->GetClearedStageCount();

        // 王冠数が増えた初クリアでだけ解放用の演出を予約する。
        if (!wasStageCleared && newCrownCount > previousCrownCount) {
            gameData->RequestStageClearRewardPresentation(currentStage, previousCrownCount, newCrownCount);
        }

        // 初回か再クリアかにかかわらず、保存した取得内容をステージ選択の帰還演出へ渡す。
        gameData->RequestStageSelectReturnPresentation(currentStage, previousCrownCount, newCrownCount,
                                                       newStarCoinIndices);

        // セーブ表示を用意できている場合は、完了を画面にも短時間表示する。
        if (saveIndicatorOverlay_) {
            saveIndicatorOverlay_->Play(1.35f);
        }

        DebugConsole::GetInstance()->AddLog("Stage clear data saved.");
        // 報酬と帰還の通知を登録してから保存済みにし、同じクリアの再送を防ぐ。
        goalSavePerformed_ = true;
    }
}

void GamePlayScene::UpdateGoalClearPresentation(float deltaTime) {
    // 通常進行を止めたまま、演出で変更した姿勢や状態をポストエフェクト、粒子、UIへ反映する。
    UpdateGoalPresentation(deltaTime);
    UpdatePostEffectState(deltaTime);

    // CPU粒子システムがある場合は、通常進行を停止している間もリザルト用の粒子を進める。
    if (particleSystem_) {
        particleSystem_->Update(deltaTime);
    }

    GpuParticleManager::GetInstance()->Update(deltaTime);
    UpdateUi(deltaTime);
}

void GamePlayScene::HandleGoalResultConfirmInput() {
    // 実プレイで保存済み、帰還前、かつ決定入力を取得した場合だけ、結果の早送りや帰還を受け付ける。
    if (goalSavePerformed_ && !goalEditorPreviewMode_ && goalPresentationState_ != GoalPresentationState::kReturning &&
        inputManager_ && inputManager_->IsActionTriggered("Jump")) {
        const float elapsed = goalPresentationTimer_.Elapsed();
        // 結果を公開済みか帰還可能な時刻に達していれば、暗転とステージ選択への帰還を要求する。
        if (goalResultRevealed_ || elapsed >= GetGoalRewardReadyTime()) {
            RequestGoalReturnToSelect();
        // タイトル表示の時刻以降なら、まだ帰還できなくても結果だけ先に表示する。
        } else if (elapsed >= GetGoalResultTitleStartTime()) {
            goalResultRevealed_ = true;
            UpdateGoalPresentationOverlay();
        }
    }
}

void GamePlayScene::UpdateGoalPresentation(float deltaTime) {
    // 報酬がないシーンで撃破から直接リザルトに入った場合だけ、ワールド演出を省略する。
    if (crownDefeat_.completed && !goalCrownSnapshotValid_) {
        // 撃破後の結果画面からすでに帰還を始めていれば、カメラと表示時刻を進めない。
        if (goalPresentationState_ == GoalPresentationState::kReturning)
            return;
        goalPresentationTimer_.Update(deltaTime);
        UpdateGoalPresentationCamera();
        // 撃破後のリザルト時刻が帰還可能な時刻に達した場合は、帰還待ち状態にする。
        if (goalPresentationTimer_.Elapsed() >= goalClearPlayerAnimator_.GetTuning().readyTime)
            goalPresentationState_ = GoalPresentationState::kReadyToReturn;
        UpdateGoalPresentationOverlay();
        return;
    }

    // 通常ゴールの演出が未開始か帰還に入った場合は、王冠の動作と表示時刻の更新を止める。
    if (goalPresentationState_ == GoalPresentationState::kInactive ||
        goalPresentationState_ == GoalPresentationState::kReturning) {
        return;
    }

    // 再生終了フレームを含めて時刻を同期できるよう、更新前に再生中だったかを控える。
    const bool wasTimelinePlaying = goalCinematicTimelineLoaded_ && goalCinematicPlayer_.IsPlaying();
    // タイムラインを読み込めている場合は、再生中かどうかを控えてから一フレーム進める。
    if (goalCinematicTimelineLoaded_) {
        goalCinematicPlayer_.Update(deltaTime);
    }

    // このフレームの冒頭で再生中だった場合は、再生が終わったフレームもタイムラインの時刻を採る。
    if (wasTimelinePlaying) {
        goalPresentationTimer_.Seek(goalCinematicPlayer_.GetCurrentTime());
    // 再生を終えた後やタイムラインを使えない場合は、リザルトの時計を経過秒数で進める。
    } else {
        goalPresentationTimer_.Update(deltaTime);
    }

    UpdateGoalPlayerCelebration(deltaTime);
    UpdateGoalCrownMotion(deltaTime);
    // AIや物理を再開せず、動かした表示物の付属エフェクトだけを同フレームで追従させる。
    // 王冠が配置されていれば、装着位置へその付属エフェクトを追従させる。
    if (goalCrownObject_) goalCrownObject_->UpdateAttachedEffects(deltaTime);
    // プレイヤーが存在する場合は、喜びモーションで移動した姿勢に付属エフェクトを合わせる。
    if (player_) player_->UpdateAttachedEffects(deltaTime);
    // 再生器の視点キーを使う間は、動かした直後の王冠とプレイヤーで構図を補正する。
    if (goalCinematicPlayer_.GetPresentationCamera()) {
        // 終端を過ぎてUIだけ進む間は、再生器が保持した最後の構図を維持する。
        if (wasTimelinePlaying) {
            UpdateGoalPresentationCamera();
        } else {
            RefreshRenderCameraData();
        }
    // 途中で視点キーの対象を失った場合は、代替カメラに引き継ぐ。
    } else if (!goalPresentationCamera_) {
        SetupGoalPresentationCamera();
    } else {
        UpdateGoalPresentationCamera();
    }

    // タイムラインを準備できなかった時だけC++側から粒子の節目を発火する。
    if (!goalCinematicTimelineLoaded_) {
        EmitGoalPresentationEffects(deltaTime);
    }

    UpdateGoalPresentationOverlay();

    // 喜びモーション中に規定時刻へ到達したら、どちらの再生方式でも帰還待ち状態へ進める。
    if (goalPresentationState_ == GoalPresentationState::kCelebrating &&
        goalPresentationTimer_.Elapsed() >= goalClearPlayerAnimator_.GetTuning().readyTime) {
        goalPresentationState_ = GoalPresentationState::kReadyToReturn;
    }
}

void GamePlayScene::UpdateGoalPlayerCelebration(float deltaTime) {
    // プレイヤーと開始姿勢があり、共通タイムラインが使えない場合にだけ代替の喜びモーションを適用する。
    (void)deltaTime;

    // プレイヤーがいることと開始姿勢が記録済みであることを、動かす前に確認する。
    if (!player_ || !goalPlayerSnapshotValid_) {
        return;
    }

    // タイムラインが使えない場合は、C++の代替モーションを同じ演出時刻で更新する。
    if (!goalCinematicTimelineLoaded_) {
        goalClearPlayerAnimator_.Update(goalPresentationTimer_.Elapsed());
        goalPlayerPosePosition_ = player_->GetTranslate();
    }
}

void GamePlayScene::UpdateGoalCrownMotion(float /*deltaTime*/) {
    if (!goalCrownObject_ || !goalCrownObject_->GetModel() || !player_ || !player_->GetModel() ||
        !goalCrownSnapshotValid_ || !goalPlayerSnapshotValid_) {
        return;
    }

    // そのフレームの体の形から、王冠の内周が載る頭の高さを求める。
    const auto& animation = goalClearPlayerAnimator_.GetTuning();
    const auto& tuning = goalPresentationTuning_;
    const float time = goalPresentationTimer_.Elapsed();
    auto& crown = *goalCrownObject_;
    const auto& bodyWorld = player_->GetWorldMatrix();
    const auto bodyMin = player_->GetModel()->GetLocalAabbMin();
    const auto bodyMax = player_->GetModel()->GetLocalAabbMax();
    const auto bodyScale = GoalWorldScale(player_);
    const auto up = engine::Math::Normalize(engine::Math::TransformNormal({0, 1, 0}, bodyWorld));
    const auto top = engine::Math::Transform(
        {(bodyMin.x + bodyMax.x) * 0.5f, bodyMax.y, (bodyMin.z + bodyMax.z) * 0.5f}, bodyWorld);
    const auto crownMin = crown.GetModel()->GetLocalAabbMin();
    const auto crownMax = crown.GetModel()->GetLocalAabbMax();
    const engine::Vector3 bottom{(crownMin.x + crownMax.x) * 0.5f, crownMin.y, (crownMin.z + crownMax.z) * 0.5f};
    const auto parentScale = GoalWorldScale(crown.GetParent());
    const float bodyWidth = (std::max)({0.001f, (bodyMax.x - bodyMin.x) * bodyScale.x,
                                               (bodyMax.z - bodyMin.z) * bodyScale.z});
    const float crownWidth = (std::min)((crownMax.x - crownMin.x) * goalCrownWearScale_.x * parentScale.x,
                                       (crownMax.z - crownMin.z) * goalCrownWearScale_.z * parentScale.z);
    // 宝石を含む外幅から内周を控えめに見積もり、伸びた頭でも底面を深く沈めない。
    const float ratio = std::clamp(crownWidth * 0.72f / bodyWidth, 0.0f, 0.68f);
    const float headDrop = (bodyMax.y - bodyMin.y) * bodyScale.y * 0.5f * (1.0f - std::sqrt(1.0f - ratio * ratio));
    const auto seat = top - up * (std::min)(tuning.crownSeatDepth, headDrop);
    const auto playerRotation = player_->GetRotation();
    const engine::Vector3 wearRotation{playerRotation.x,
        goalCrownBaseRotation_.y + 0.52f + playerRotation.y - goalPlayerBaseRotation_.y, playerRotation.z};

    const float move = animationInterpolation::SegmentRate(time, tuning.crownMoveStartTime, animation.crownLandTime);
    const float fit = Easing::SmoothStep01(move);
    const auto scale = animationInterpolation::Lerp(goalCrownBaseScale_, goalCrownWearScale_, fit);
    auto idleRotation = goalCrownBaseRotation_;
    idleRotation.y += (std::min)(time, tuning.crownMoveStartTime) * 0.34f;
    const auto rotation = animationInterpolation::SlerpEuler(idleRotation, wearRotation, fit);
    auto bottomOffset = [&crown, bottom](const engine::Vector3& size, const engine::Vector3& angle) {
        auto offset = engine::Math::TransformNormal(bottom, engine::Math::MakeAffineMatrix(size, angle, {}));
        if (const auto* parent = crown.GetParent()) {
            offset = engine::Math::TransformNormal(offset, parent->GetWorldMatrix());
        }
        return offset;
    };

    auto position = goalCrownStartWorldPosition_;
    if (time >= tuning.crownMoveStartTime) {
        // 底面を先に引き上げ、体の高さを越えてから横に運ぶ。最後は頭の真上から降ろす。
        crown.SetMeshEffect1Name("");
        crown.SetMeshEffect2Name("");
        constexpr float kOverheadTime = 0.68f;
        const auto overhead = seat + engine::Vector3{0.0f, tuning.crownDropHeight, 0.0f};
        auto bottomTarget = seat;
        if (move < kOverheadTime) {
            const auto start = goalCrownStartWorldPosition_ + bottomOffset(goalCrownBaseScale_, idleRotation);
            const float upRate = Easing::SmoothStep01(move / 0.36f);
            const float across = Easing::SmoothStep01((move - 0.27f) / (kOverheadTime - 0.27f));
            bottomTarget = engine::Math::Lerp(start, overhead, across);
            bottomTarget.y = std::lerp(start.y, overhead.y, upRate);
        } else if (move < 1.0f) {
            const float down = Easing::SmoothStep01((move - kOverheadTime) / (1.0f - kOverheadTime));
            bottomTarget = engine::Math::Lerp(overhead, seat, down);
        }
        // 装着後は同じフレームの接触面を使う。体が急に伸びても冠の位置だけ取り残されない。
        position = bottomTarget - bottomOffset(scale, rotation);
    }

    crown.SetTranslate(GoalLocalPosition(&crown, position));
    crown.SetScale(scale);
    crown.SetRotation(rotation);
    crown.UpdateWorldMatrix(true);
    goalCrownPosition_ = crown.GetWorldPosition();
}

void GamePlayScene::EmitGoalPresentationEffects(float deltaTime) {
    // 王冠とプレイヤーの位置に合わせて、演出段階ごとの粒子を発生させる。
    const float t = goalPresentationTimer_.Elapsed();
    const GoalClearPlayerAnimator::Tuning& animation = goalClearPlayerAnimator_.GetTuning();

    // 装着時に王冠の位置と大きさを使って、取得の一度きりの光を出す。
    if (!goalLandingCuePlayed_ && t >= animation.crownLandTime) {
        const float crownScale = std::clamp(std::max({std::abs(goalCrownBaseScale_.x), std::abs(goalCrownBaseScale_.y),
                                                      std::abs(goalCrownBaseScale_.z)}) *
                                                0.72f,
                                            0.80f, 1.80f);
        VfxSequencer::PlayOneShot(goalWasStageCleared_ ? goalCinematicVfx::kLanding.silver : goalCinematicVfx::kLanding.gold, goalCrownPosition_,
                                  {crownScale, crownScale, crownScale});
        goalLandingCuePlayed_ = true;
    }

    // ジャンプ頂点では王冠の輝きだけを一度出し、その後の着地の波紋と分ける。
    if (!goalResultCuePlayed_ && t >= animation.apexTime) {
        VfxSequencer::PlayOneShot(goalCinematicVfx::kResult.gold, goalPlayerPosePosition_);
        goalResultCuePlayed_ = true;
    }

    // 代替再生でも着地時刻をまたいだ一回だけ、モデルの接地面からしぶきを出す。
    if (player_ && t - deltaTime < animation.victoryLandTime && t >= animation.victoryLandTime) {
        auto contact = player_->GetWorldPosition();
        const auto corners = GoalWorldCorners(player_);
        contact.y = std::min_element(corners.begin(), corners.end(),
                                     [](const auto& a, const auto& b) { return a.y < b.y; })->y;
        VfxSequencer::PlayOneShot(goalCinematicVfx::kVictory.gold, contact);
    }
}

void GamePlayScene::SetupGoalPresentationCamera() {
    // 視点キーを使えないゴールと撃破直後の結果表示だけ、代替カメラを作る。
    goalCameraRestoreOverride_ = CameraManager::GetInstance()->GetActiveCameraOverride();
    engine::Camera* sourceCamera = goalLockedPrimaryCamera_;

    // 入力を固定した通常カメラを控えていない場合は、現在の描画カメラを開始姿勢の候補にする。
    if (!sourceCamera) {
        sourceCamera = CameraManager::GetInstance()->GetActiveCamera();
    }

    // 現在の描画カメラも取れなければ、シーンのメインカメラを候補にする。
    if (!sourceCamera) {
        sourceCamera = CameraManager::GetInstance()->GetMainCamera();
    }

    // 操作を止めた時点の通常視点を補間の始点にする。取得できなければプレイヤー相対の既定視点を使う。
    const GoalPresentationTuning& camera = goalPresentationTuning_;
    goalCameraGameplayEye_ =
        sourceCamera ? sourceCamera->GetEye() : goalPlayerBasePosition_ + engine::Vector3{0.0f, 3.0f, 8.0f};
    goalCameraGameplayTarget_ =
        sourceCamera ? sourceCamera->GetTargetPoint() : goalPlayerBasePosition_ + engine::Vector3{0.0f, 1.0f, 0.0f};
    goalCameraGameplayFov_ = sourceCamera ? sourceCamera->GetFovY() : camera.resultFov;

    // 操作や追従を無効化した固定カメラを開始姿勢へ置き、演出中の描画先にする。
    goalPresentationCamera_ = std::make_unique<engine::Camera>();
    goalPresentationCamera_->Initialize();
    goalPresentationCamera_->SetInputEnabled(false);
    // 通常カメラを取得済みなら、近・遠クリップ距離を演出カメラにも引き継ぐ。
    if (sourceCamera) goalPresentationCamera_->SetClipRange(sourceCamera->GetNearClip(), sourceCamera->GetFarClip());
    goalPresentationCamera_->SetFollowTarget(nullptr);
    goalPresentationCamera_->SetLockOnTarget(nullptr);
    goalPresentationCamera_->SetFollowMode(engine::Camera::FollowMode::kFixedPoint);
    goalPresentationCamera_->SetFovY(goalCameraGameplayFov_);
    goalPresentationCamera_->SetEye(goalCameraGameplayEye_);
    goalPresentationCamera_->SetTarget(goalCameraGameplayTarget_);
    goalPresentationCamera_->SetRotation(MakeLookAtEuler(goalCameraGameplayEye_, goalCameraGameplayTarget_));
    goalPresentationCamera_->ConfigFixedPoint(goalCameraGameplayEye_,
                                              MakeLookAtEuler(goalCameraGameplayEye_, goalCameraGameplayTarget_));
    goalPresentationCamera_->Update();

    goalCameraSnapshotValid_ = true;
    CameraManager::GetInstance()->SetActiveCamera(goalPresentationCamera_.get());
    UpdateGoalPresentationCamera();
}

void GamePlayScene::UpdateGoalPresentationCamera() {
    // 王冠とプレイヤーの位置を見ながら、節目ごとの視点と画角を決める。
    auto* authoredCamera = goalCinematicPlayer_.GetPresentationCamera();
    auto* presentationCamera = authoredCamera ? authoredCamera : goalPresentationCamera_.get();

    // 視点キーも有効な代替視点も取れなければ、描画先を変更しない。
    if (!presentationCamera || (!authoredCamera && !goalCameraSnapshotValid_)) {
        return;
    }

    const float t = goalPresentationTimer_.Elapsed();
    const GoalPresentationTuning& camera = goalPresentationTuning_;
    engine::Vector3 eye = authoredCamera ? authoredCamera->GetEye() : goalCameraGameplayEye_;
    engine::Vector3 target = authoredCamera ? authoredCamera->GetTargetPoint() : goalCameraGameplayTarget_;
    float fov = authoredCamera ? authoredCamera->GetFovY() : goalCameraGameplayFov_;

    const auto& projection = presentationCamera->GetProjectionMatrix();
    const float aspect = std::abs(projection.m[0][0]) > 0.0001f
        ? std::abs(projection.m[1][1] / projection.m[0][0]) : 16.0f / 9.0f;

    // 報酬がない場合の直行リザルトは、勝利カットから結果用の構図へつなぐ。
    if (crownDefeat_.completed && !goalCrownSnapshotValid_ && player_) {
        const float rate = animationInterpolation::SegmentRate(t, GetGoalResultTitleStartTime() - 0.50f,
                                                               GetGoalResultTitleStartTime());
        const float blend = Easing::SmoothStep01(rate);
        const auto points = GoalWorldCorners(player_);
        auto resultTarget = player_->GetWorldPosition();
        const auto direction = goalCameraGameplayTarget_ - goalCameraGameplayEye_;
        engine::Vector3 horizontal{direction.x, 0.0f, direction.z};
        // 勝利カットから水平の向きを取れないときは、正のZ軸をリザルトの向きの基準にする。
        if (engine::Math::Length(horizontal) < 0.001f) horizontal = {0.0f, 0.0f, 1.0f};
        auto resultEye = resultTarget - engine::Math::Normalize(horizontal) * camera.resultCameraDistance +
                         engine::Vector3{0.0f, camera.resultCameraHeight, 0.0f};
        engine::CinematicPlayer::FrameCameraForOverlay(points, camera.resultFov, aspect, camera.cameraSafeMargin, 1.0f,
                        camera.resultUiCenterX, camera.resultTargetSide, resultEye, resultTarget);
        eye = animationInterpolation::Lerp(eye, resultEye, blend);
        target = animationInterpolation::Lerp(target, resultTarget, blend);
        fov = animationInterpolation::Lerp(fov, camera.resultFov, blend);
        PostEffect::GetInstance()->GetParams()->cinemaBarHeight = goalDefeatResultBars_ * (1.0f - blend);
        const auto rotation = MakeLookAtEuler(eye, target);
        presentationCamera->ConfigFixedPoint(eye, rotation);
        presentationCamera->ApplyReplayView(eye, target, rotation, fov,
                                             presentationCamera->GetNearClip(), presentationCamera->GetFarClip());
        // 結果画面では通常更新を止めているため、描画定数だけを新しいカメラへ揃える。
        CameraManager::GetInstance()->SetActiveCamera(presentationCamera);
        RefreshRenderCameraData();
        return;
    }

    // プレイヤーと王冠の両方があるときは、外接箱を一緒に収めてリザルト文字の余白を取る。
    if (player_ && goalCrownObject_) {
        const auto body = GoalWorldCorners(player_);
        const auto crown = GoalWorldCorners(goalCrownObject_);
        std::array<engine::Vector3, 16> points;
        std::copy(body.begin(), body.end(), points.begin());
        std::copy(crown.begin(), crown.end(), points.begin() + body.size());
        const float titleStart = GetGoalResultTitleStartTime();
        const float reveal = Easing::SmoothStep01(animationInterpolation::SegmentRate(t, titleStart - 0.55f, titleStart));
        engine::CinematicPlayer::FrameCameraForOverlay(points, fov, aspect, camera.cameraSafeMargin, reveal,
                        camera.resultUiCenterX, camera.resultTargetSide, eye, target);
    }
    // 視点キーか代替視点を評価したカメラへ、収まり補正を直接反映する。
    const auto rotation = MakeLookAtEuler(eye, target);
    presentationCamera->ConfigFixedPoint(eye, rotation);
    presentationCamera->ApplyReplayView(eye, target, rotation, fov,
                                        presentationCamera->GetNearClip(), presentationCamera->GetFarClip());
    // キャラと背景を同じカメラで描き、切替前の地形や水面が残らないようにする。
    CameraManager::GetInstance()->SetActiveCamera(presentationCamera);
    RefreshRenderCameraData();
}

void GamePlayScene::LockGoalPresentationCameraInput() {
    // 演出中にプレイヤー入力で視点が動かないようカメラ操作を一時停止する。
    RestoreGoalPresentationCameraInput();

    CameraManager* cameraManager = CameraManager::GetInstance();

    // カメラ管理を取得できなければ、この演出で無効にするカメラを特定できない。
    if (!cameraManager) {
        return;
    }

    goalLockedPrimaryCamera_ = cameraManager->GetActiveCamera();

    // 現在の描画カメラが取れなければ、メインカメラを一台目の候補にする。
    if (!goalLockedPrimaryCamera_) {
        goalLockedPrimaryCamera_ = cameraManager->GetMainCamera();
    }

    goalLockedSecondaryCamera_ = cameraManager->GetMainCamera();

    // メインカメラが一台目と同じ場合は、同じ参照を二重に扱わない。
    if (goalLockedSecondaryCamera_ == goalLockedPrimaryCamera_) {
        goalLockedSecondaryCamera_ = nullptr;
    }

    // 一台目の参照を取得できた場合は、プレイヤー入力を無効にする。
    if (goalLockedPrimaryCamera_) {
        goalLockedPrimaryCamera_->SetInputEnabled(false);
    }

    // 別のメインカメラも取得できた場合は、切り替わったときの入力に備えて無効にする。
    if (goalLockedSecondaryCamera_) {
        goalLockedSecondaryCamera_->SetInputEnabled(false);
    }
}

void GamePlayScene::RestoreGoalPresentationCameraInput() {
    // 一台目の入力を止めた参照が残っていれば、そのカメラの入力を再び有効にする。
    if (goalLockedPrimaryCamera_) {
        goalLockedPrimaryCamera_->SetInputEnabled(true);
    }

    // 二台目も無効にしていた場合は、同様に入力を有効にする。
    if (goalLockedSecondaryCamera_) {
        goalLockedSecondaryCamera_->SetInputEnabled(true);
    }

    goalLockedPrimaryCamera_ = nullptr;
    goalLockedSecondaryCamera_ = nullptr;
}

void GamePlayScene::RequestGoalReturnToSelect() {
    // 演出の終了を受けてフェードとステージ選択への遷移を一度だけ要求する。
    if (goalPresentationState_ == GoalPresentationState::kReturning) {
        return;
    }

    goalPresentationState_ = GoalPresentationState::kReturning;
    goalCinematicPlayer_.Stop(false);
    goalReturnFadeStarted_ = true;
    Fade::GetInstance()->StartFadeOut(0.55f);
    goalClearPlayerAnimator_.RestoreControl();
}

void GamePlayScene::UpdatePostEffectState(float deltaTime) {
    // ポスト効果の時間を進め、深度再構築に使う逆射影行列を現カメラへ合わせる。
    PostEffect::GetInstance()->Update(deltaTime);

    engine::Camera* activeCamera = CameraManager::GetInstance()->GetActiveCamera();

    // 現在の描画カメラがあるときだけ、カメラに合わせて深度再構築用の逆射影行列を更新する。
    if (activeCamera) {
        PostEffect::GetInstance()->GetParams()->projectionInverse = engine::Math::Inverse(activeCamera->GetProjectionMatrix());
    }
}

void GamePlayScene::UpdateLockOnAndCamera(float deltaTime) {
    // ロックオン対象の変化をカメラへ渡し、通常操作中の視点を更新する。
    LightEditor::GetInstance()->Update();

    engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();

    // 通常時のメインカメラがなければ、ロックオン対象と画面マーカーを更新できない。
    if (!camera) {
        isDrawLockOn_ = false;
        return;
    }

    lockOnSystem_->Update(objectManager_->GetObjects(), camera, player_, deltaTime);
    engine::Object3D* framingBoss = nullptr;
    // ステージ3で、生存して演出に固定されていないプレイヤーを追っている場合に、既に登場を終えた偽王を探す。
    if (StageManager::GetInstance()->GetCurrentStage().id == "stage3" && player_ &&
        !player_->IsDead() && !player_->IsCinematicLocked()) {
        for (const auto& object : objectManager_->GetObjects()) {
            auto* boss = dynamic_cast<EnemyFalseKingSlime*>(object.get());
            // 戦闘HUDが有効で登場を終えた偽王を見つけたら、構図の基準として採用する。
            if (boss && boss->IsEncounterHudActive() && boss->GetEncounterAppearanceProgress() >= 1.0f) {
                framingBoss = boss;
                break;
            }
        }
    }
    CameraEditor::GetInstance()->Update(player_, lockOnSystem_->IsLockingOn(), framingBoss);

    // カメラをエディターで操作していないときだけ、ゲーム入力をカメラへ送る。
    if (!CameraEditor::GetInstance()->IsEditorMode()) {
        engine::Camera::FollowMode currentMode = camera->GetFollowMode();

        // 照準操作と一人称視点だけ、マウスの回転操作とユーザー設定の感度を反映する。
        if (currentMode == engine::Camera::FollowMode::kAimable || currentMode == engine::Camera::FollowMode::kFirstPerson) {
            const auto* settings = GameSettingsManager::GetInstance();
            camera->SetRotationSensitivity(settings->GetCameraSensitivity());
            const engine::Vector2 lookDelta = inputManager_->GetCameraLookDelta(deltaTime);

            // 視点入力のどちらかの軸が動いたときだけ、そのフレームの回転を加える。
            if (lookDelta.x != 0.0f || lookDelta.y != 0.0f) {
                camera->AddRotation(settings->ApplyCameraLookInversion(lookDelta));
            }
        }
    }
}

void GamePlayScene::UpdateLockOnSprite(engine::Camera* camera, engine::Object3D* target, float deltaTime) {
    isDrawLockOn_ = false;
    // Spriteを作成済みなら、今フレームに投影できるか確認するまで非表示にする。
    if (lockOnSprite_)
        lockOnSprite_->SetVisible(false);
    // カメラ・対象・Spriteのいずれかが欠けていれば、表示を作れないので戻る。
    if (!camera || !target || !lockOnSprite_) {
        isDrawLockOn_ = false;
        return;
    }

    const auto& objects = objectManager_->GetObjects();

    // 対象の参照が管理リストから消えていれば、位置や表示状態を参照する前に戻る。
    if (std::none_of(objects.begin(), objects.end(), [target](const auto& object) { return object.get() == target; }))
        return;
    // 対象そのものが非表示なら、位置を強調するマーカーも出さない。
    if (!target->GetIsVisible())
        return;

    // 敵の配置原点ではなく、描画対象に対応する境界箱の中心をマーカーの基準にする。
    const engine::Aabb aabb = target->GetAabb();
    engine::Vector3 targetCenter;
    targetCenter.x = (aabb.min.x + aabb.max.x) * 0.5f;
    targetCenter.y = (aabb.min.y + aabb.max.y) * 0.5f;
    targetCenter.z = (aabb.min.z + aabb.max.z) * 0.5f;

    engine::Math math;
    const engine::Matrix4x4 viewProj = math.Multiply(camera->GetViewMatrix(), camera->GetProjectionMatrix());
    const float w = targetCenter.x * viewProj.m[0][3] + targetCenter.y * viewProj.m[1][3] +
                    targetCenter.z * viewProj.m[2][3] + viewProj.m[3][3];

    // 視点の後ろや射影できない位置では除算する前に表示を中止する。
    if (w <= 0.001f) {
        isDrawLockOn_ = false;
        return;
    }

    engine::Vector3 ndc;
    ndc.x = (targetCenter.x * viewProj.m[0][0] + targetCenter.y * viewProj.m[1][0] + targetCenter.z * viewProj.m[2][0] +
             viewProj.m[3][0]) /
            w;
    ndc.y = (targetCenter.x * viewProj.m[0][1] + targetCenter.y * viewProj.m[1][1] + targetCenter.z * viewProj.m[2][1] +
             viewProj.m[3][1]) /
            w;
    const float depth = (targetCenter.x * viewProj.m[0][2] + targetCenter.y * viewProj.m[1][2] +
                         targetCenter.z * viewProj.m[2][2] + viewProj.m[3][2]) /
                        w;
    // 投影値が有限かつ描画範囲内に入った場合だけ、画面座標へ変換して表示する。
    if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y) || !std::isfinite(depth) || depth < 0.0f || depth > 1.0f ||
        std::abs(ndc.x) > 1.0f || std::abs(ndc.y) > 1.0f)
        return;

    // NDCをクライアント座標に戻し、Y軸の向きを画面用に反転する。
    const float screenWidth = static_cast<float>(::WinApp::GetClientWidth());
    const float screenHeight = static_cast<float>(::WinApp::GetClientHeight());
    const float screenX = (ndc.x + 1.0f) * 0.5f * screenWidth;
    const float screenY = (1.0f - ndc.y) * 0.5f * screenHeight;
    lockOnSprite_->SetPosition({screenX, screenY});

    const float objSizeX = aabb.max.x - aabb.min.x;
    const float objSizeY = aabb.max.y - aabb.min.y;
    const float objSizeZ = aabb.max.z - aabb.min.z;
    const float maxObjSize = std::max({objSizeX, objSizeY, objSizeZ});
    const float baseSize = maxObjSize * 25.0f;
    const float distanceScale = 20.0f / w;

    // 元画像は64px。遠距離でも細い赤線が潰れず、近距離でも敵を覆いすぎないサイズ。
    const float finalSize = std::clamp(baseSize * distanceScale, 72.0f, 112.0f);

    lockOnSprite_->SetSize({finalSize, finalSize});
    lockOnSprite_->SetRotation(0.0f);
    lockOnSprite_->SetColor({1.0f, 1.0f, 1.0f, 1.0f});
    lockOnSprite_->SetVisible(true);
    isDrawLockOn_ = true;
    lockOnSprite_->Update();
}

void GamePlayScene::UpdateSceneSystems(float deltaTime) {
    // カメラとCPU粒子、配置物、ステージ固有規則、GPU粒子、UIの描画状態、弾を更新し、最後に衝突を解決する。
    ProfilerManager::GetInstance()->SetObjectList(&objectManager_->GetObjects());
    CameraManager::GetInstance()->Update(deltaTime);
    particleSystem_->Update(deltaTime);
    objectManager_->Update(deltaTime);
    // 走査が終わってから、闘技場の破片を即時登録できる演出を始める。
    StartPendingCrownFinale();

    // ゲーム規則を持つシーンでは、物体の更新後に継続効果や進行状態を進める。
    if (gameRule_) {
        gameRule_->Update(deltaTime);
    }

    GpuParticleManager::GetInstance()->Update(deltaTime);

    for (auto& sprite : sprites_) {
        sprite->Update();
    }

    // 弾の位置も今フレームへ進めてから、更新後の物体同士を衝突判定する。
    BulletManager::GetInstance()->Update(deltaTime);
    {
        PROFILE_SCOPE("衝突判定");
        engine::CollisionManager::GetInstance()->Update();
    }
}

void GamePlayScene::UpdateEffectDebugShortcuts() {
    // デバッグ用のエフェクト操作は通常のゲーム入力と分離して処理する。
#ifdef USE_IMGUI
    ImGuiIO& io = ImGui::GetIO();

    // ImGuiが文字やキーボード入力を使用中なら、デバッグ用のエフェクト入力を無視する。
    if (io.WantCaptureKeyboard || io.WantTextInput) {
        return;
    }

    // 左右いずれのCtrlも押していない場合は、デバッグ操作を起動しない。
    if (!inputManager_->IsKeyPressed(DIK_LCONTROL) && !inputManager_->IsKeyPressed(DIK_RCONTROL)) {
        return;
    }
#else
    return;
#endif

    // CtrlとSpaceで、同じ位置に星と斬撃の粒子を重ねる。
    if (inputManager_->IsKeyTriggered(DIK_SPACE)) {
        engine::Vector3 effectPos = {0.0f, 2.0f, 0.0f};
        particleSystem_->SpawnStarHitEffect(effectPos);
        particleSystem_->SpawnSlashEffect(effectPos);
    }

    // CtrlとEnterで、確認用の位置に星の粒子だけを出す。
    if (inputManager_->IsKeyTriggered(DIK_RETURN)) {
        engine::Vector3 effectPos = {0.0f, 2.0f, 0.0f};
        particleSystem_->SpawnStarHitEffect(effectPos);
    }

    // CtrlとEで、確認用の位置に斬撃の粒子だけを出す。
    if (inputManager_->IsKeyTriggered(DIK_E)) {
        engine::Vector3 effectPos = {0.0f, 2.0f, 0.0f};
        particleSystem_->SpawnSlashEffect(effectPos);
    }

    // CtrlとRで、確認用の位置にリング状の波を出す。
    if (inputManager_->IsKeyTriggered(DIK_R)) {
        engine::Vector3 effectPos = {0.0f, 2.0f, 0.0f};
        MeshEffectManager::GetInstance()->SpawnRingWaveEffect(effectPos);
    }

    // CtrlとTで、確認用の位置にポータルを出す。
    if (inputManager_->IsKeyTriggered(DIK_T)) {
        engine::Vector3 effectPos = {0.0f, 1.5f, 0.0f};
        MeshEffectManager::GetInstance()->SpawnPortalEffect(effectPos, 5.0f);
    }

    // CtrlとYで、床と柱を組み合わせたワープゲートのエフェクトを出す。
    if (inputManager_->IsKeyTriggered(DIK_Y)) {
        engine::Vector3 spawnPos = {0.0f, 0.01f, 10.0f};

        // プレイヤーがいれば前方を発生位置にし、いなければ固定の確認位置を使う。
        if (player_) {
            engine::Vector3 playerPos = player_->GetWorldPosition();
            engine::Vector3 forward = player_->GetForwardDirection();
            spawnPos = {playerPos.x + forward.x * 3.0f, playerPos.y, playerPos.z + forward.z * 3.0f};
        }

        MeshEffectManager::GetInstance()->SpawnEffectAt("Resources/json/effect/shared/effect_warp_gate_floor.json", spawnPos,
                                                        {0.0f, 0.0f, 0.0f});
        MeshEffectManager::GetInstance()->SpawnEffectAt("Resources/json/effect/shared/effect_warp_gate_pillar.json", spawnPos,
                                                        {0.0f, 0.0f, 0.0f});
    }
}
