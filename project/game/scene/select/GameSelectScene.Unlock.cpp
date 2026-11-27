#define NOMINMAX
#include <cstdint>
#include "game/scene/select/GameSelectScene.h"
#include "Camera.h"
#include "Player.h"
#include "CameraManager.h"
#include "DebugConsole.h"
#include "game/system/progress/StageManager.h"
#include "SceneManager.h"
#include <algorithm>
#include "game/scene/select/GameSelectScene.Internal.h"
using namespace stageSelectDetail;

// 解放演出の開始・進行・復帰。入力とカメラの復帰、既読保存の条件を一組として維持する。
void GameSelectScene::BeginUnlockPresentation() {
    // 解放する対象が未指定か、この演出の準備が済んでいる場合は、再生設定を作り直さない。
    if (unlockingStageIndex_ < 0 || unlockPresentationInitialized_) {
        return;
    }

    unlockPresentationInitialized_ = true;
    unlockTargetGate_ = FindStageGateByIndex(unlockingStageIndex_);
    unlockKeyObject_ = FindObjectByName(std::string(kStageGateCrownKeyPrefix) + std::to_string(unlockingStageIndex_));
    unlockLockObject_ = FindObjectByName(std::string(kStageGateLockPrefix) + std::to_string(unlockingStageIndex_));

    // 鍵・錠の色、発光、表示は Director の見た目キーが管理する。

    // ゲート固有の開通状態に必要な2つの節目だけを Signal から読み取る。
    unlockTimelineLoaded_ = unlockPlayer_.Load("Resources/json/scenario/stage_select/stage_select_unlock_" +
                                               std::to_string(unlockingStageIndex_) + ".json",
                                               SceneManager::GetInstance());
    // シーケンスを読み込めた場合に限り、ゲート、鍵、錠、カメラの演出条件を検証する。
    if (unlockTimelineLoaded_) {
        const auto* sequence = unlockPlayer_.GetSequence();
        // ゲーム固有の節目と役割だけを確認し、対象の共通検証はPrepareへ任せる。
        std::vector<std::string> issues;
        auto signalTime = [sequence, &issues](const char* name) {
            for (const auto& signal : sequence->signals) {
                // 指定の名前が一致し、有効になっているSignalだけ、その時刻を使う。
                if (signal.enabled && signal.signal == name) {
                    return signal.time;
                }
            }
            issues.emplace_back(std::string("必須 Signal がありません: ") + name);
            return 0.0f;
        };
        // ゲートの点灯開始・完了の時刻を秒で保持し、再生中の発光の補間に使う。
        unlockGateActivationTime_ = signalTime("gate_activate");
        unlockGateActivationEndTime_ = signalTime("gate_active_full");
        // 鍵と南京錠は、対象のステージ名に合う既存オブジェクトを動かすトラックに限る。
        const auto hasTrack = [sequence](const engine::Object3D* object) {
            return object && std::any_of(sequence->transformTracks.begin(), sequence->transformTracks.end(),
                                         [object](const CinematicTransformTrack& track) {
                                             return track.enabled && !track.muted &&
                                                    track.binding.targetName == object->GetName();
                                         });
        };
        // 鍵と錠のどちらかに有効な対象トラックが無ければ、演出を準備できない。
        if (!hasTrack(unlockKeyObject_) || !hasTrack(unlockLockObject_)) {
            issues.emplace_back("鍵または南京錠の Transform Track が対象ステージを指していません。");
        }
        // 同じ対象に表示用クリップも要求する。配置済みの鍵と錠を使うため、生成クリップは対象から外す。
        for (const auto* object : {unlockKeyObject_, unlockLockObject_}) {
            // それぞれの配置対象が存在し、同名の生成なし表示クリップが有効である必要がある。
            if (!object || !std::any_of(sequence->presentation.objects.begin(),
                    sequence->presentation.objects.end(), [object](const CinematicObjectClip& clip) {
                        return clip.enabled && !clip.spawn && clip.name == object->GetName();
                    })) {
                issues.emplace_back("鍵または錠の見た目トラックがありません。");
            }
        }
        const std::string cameraName = "StageSelect_UnlockCamera_" + std::to_string(unlockingStageIndex_);
        // 対象ステージの解錠カメラを指す有効なショットが無ければ、不備として扱う。
        if (!std::any_of(sequence->cameraShots.begin(), sequence->cameraShots.end(),
                         [&cameraName](const CinematicCameraShot& shot) {
                             return shot.enabled && !shot.muted && shot.binding.targetName == cameraName;
                         })) {
            issues.emplace_back("解錠カメラの Shot が対象ステージを指していません。");
        }
        // ゲーム固有の条件に不備がある場合は、理由をログに残して準備を中止する。
        if (!issues.empty()) {
            unlockTimelineLoaded_ = false;
            for (const auto& issue : issues) {
                DebugConsole::GetInstance()->AddLog("Stage Select unlock timeline: " + issue);
            }
        // ゲーム側の検証を通った場合だけ、再生器にトラック共通の準備を任せる。
        } else {
            unlockTimelineLoaded_ = unlockPlayer_.Prepare();
        }
    // シーケンス自体を読み込めない場合は、失敗理由を残し、今回は再生しない。
    } else {
        DebugConsole::GetInstance()->AddLog("Stage Select: unlock timeline missing; presentation canceled for retry.");
    }

    // プレイヤーがいる場合だけ操作の状態を控え、演出中の操作と移動を止める。
    if (player_) {
        unlockHadPlayerControl_ = player_->IsControlActive();
        player_->SetIsControlActive(false);
        player_->SetVelocity({0.0f, 0.0f, 0.0f});
    }

    // 再生前の視点を保存する。ムービー中の視点は JSON の Camera Track が制御する。
    if (engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera()) {
        unlockCameraStartEye_ = camera->GetEye();
        unlockCameraStartTarget_ = camera->GetTargetPoint();
        camera->SetInputEnabled(false);
        camera->SetFollowTarget(nullptr);
        camera->SetFollowMode(engine::Camera::FollowMode::kFixedPoint);
        camera->ConfigFixedPoint(unlockCameraStartEye_,
                                 SelectLookAtRotation(unlockCameraStartEye_, unlockCameraStartTarget_));
    }

    if (!unlockTimelineLoaded_) {
        // 壊れたシナリオで操作不能にならないよう中断し、既読にはせず次回入場時に再試行する。
        FinishUnlockPresentation(false);
        return;
    }

    // 発光と完了判定で使う時計を再生器へ渡し、再入場を解錠の全尺と余韻の0.25秒だけ保留する。
    unlockPlayer_.Play(false, &unlockTime_);
    stageDecisionCooldown_ = unlockPlayer_.GetDuration() + 0.25f;
    DebugConsole::GetInstance()->AddLog("Stage Select: crown key unlock cinematic start.");
}

#ifdef USE_IMGUI
void GameSelectScene::StartDebugUnlockPresentation(std::int32_t stageIndex) {
    // セーブの既読状態を変えず、必要なモデルが揃う場合だけ演出を試す。
    const auto& stages = StageManager::GetInstance()->GetStages();

    // 先頭ステージには解放演出がなく、範囲外の選択も受け付けません。
    if (stageIndex <= 0 || stageIndex >= static_cast<std::int32_t>(stages.size())) {
        return;
    }

    // ゲートの出入りや報酬の演出中は、同じカメラとプレイヤーを使う再生を重ねません。
    if (gateEntryCinematicActive_ || gateReturnPresentationActive_ || crownCountPresentationActive_ ||
        unlockingStageIndex_ >= 0) {
        return;
    }

    engine::Object3D* gate = FindStageGateByIndex(stageIndex);
    engine::Object3D* key = FindObjectByName(std::string(kStageGateCrownKeyPrefix) + std::to_string(stageIndex));
    engine::Object3D* lock = FindObjectByName(std::string(kStageGateLockPrefix) + std::to_string(stageIndex));

    // ゲート・鍵・錠が一つでも見つからなければ、デバッグ再生を開始しない。
    if (!gate || !key || !lock) {
        DebugConsole::GetInstance()->AddLog("Stage Select: debug crown unlock cinematic assets are missing.");
        return;
    }

    // プレビュー終了時に選択を元へ戻せるよう、対象ステージへの切り替え前に保存します。
    unlockDebugPreviousSelectedStageIndex_ = selectedStageIndex_;
    unlockPresentationDebugReplay_ = true;
    unlockPresentationInitialized_ = false;
    unlockingStageIndex_ = stageIndex;
    unlockTime_ = {};
    selectedStageIndex_ = stageIndex;
    ApplyStageGateStates();
    DebugConsole::GetInstance()->AddLog("Stage Select: debug crown unlock cinematic requested.");
}
#endif

void GameSelectScene::UpdateUnlockPresentation(float deltaTime) {
    // 鍵の出現・挿入・回転・錠の落下・ゲート点灯を同じ時計で同期する。
    if (unlockingStageIndex_ < 0) {
        return;
    }

    // 初回にだけ準備し、シナリオが不正で中断された場合は同じフレームで再生しません。
    BeginUnlockPresentation();
    // 準備中に中断して対象が解放された場合は、このフレームで時計を進めない。
    if (unlockingStageIndex_ < 0) {
        return;
    }
    // このフレームの分だけ再生器を進めてから同じ時計の経過時間を読む。
    unlockPlayer_.Update(deltaTime);
    selectedStageIndex_ = unlockingStageIndex_;

    // プレイヤーが存在する間は、演出中に操作や移動を再開させない。
    if (player_) {
        player_->SetIsControlActive(false);
        player_->SetVelocity({0.0f, 0.0f, 0.0f});
    }

    // 全尺を再生して初めて完了とし、途中中断では既読にしません。
    if (unlockTime_.Elapsed() >= unlockPlayer_.GetDuration()) {
        FinishUnlockPresentation();
    }
}

void GameSelectScene::FinishUnlockPresentation(bool markSeen) {
    // デバッグ再生時は既読にせず、鍵・錠・カメラと操作権を開始前へ戻す。
    if (unlockingStageIndex_ < 0) {
        return;
    }

    const std::int32_t unlockedStageIndex = unlockingStageIndex_;
    const bool debugReplay = unlockPresentationDebugReplay_;

    RestoreUnlockPresentationState();

    // 既読の記録は、本番で記録を要求された場合だけ行う。
    if (markSeen && !debugReplay) {
        GameDataManager::GetInstance()->MarkStageUnlockSeen(unlockedStageIndex);
    }

    // 次の解放演出へ対象ポインタと再生時計を引き継がないよう初期化する。
    unlockingStageIndex_ = -1;
    unlockTime_ = {};
    unlockPresentationInitialized_ = false;
    unlockTargetGate_ = nullptr;
    unlockKeyObject_ = nullptr;
    unlockLockObject_ = nullptr;
    unlockPresentationDebugReplay_ = false;

    // 確認用の再生で開始前の選択が記録されていた場合は、その選択へ戻す。
    if (debugReplay && unlockDebugPreviousSelectedStageIndex_ >= 0) {
        selectedStageIndex_ = unlockDebugPreviousSelectedStageIndex_;
    }

    unlockDebugPreviousSelectedStageIndex_ = -1;
    stageDecisionCooldown_ = 0.35f;
    ApplyStageGateStates();
    DebugConsole::GetInstance()->AddLog("Stage Select: crown key unlock cinematic finished.");
}

void GameSelectScene::RestoreUnlockPresentationState() {
    // Director が記録した姿勢を戻し、見た目は現在のゲート状態で更新する。
    // 鍵や錠の一時状態を片付ける前に、トラックが変更した配置と表示を元に戻す。
    if (unlockTimelineLoaded_) {
        unlockPlayer_.Stop(true);
    }
    // 復元後も鍵や錠が画面に残らないように隠し、鍵が持っていた粒子も停止します。
    unlockTimelineLoaded_ = false;
    // 鍵の実体を取得済みであれば、見た目に加え付属粒子の再生も止める。
    if (unlockKeyObject_) {
        unlockKeyObject_->SetIsVisible(false);
        unlockKeyObject_->SetGpuParticleName("");
    }
    // 南京錠の実体を取得済みであれば、演出後に残らないよう非表示にする。
    if (unlockLockObject_) {
        unlockLockObject_->SetIsVisible(false);
    }

    // プレイヤーが存在する場合は、演出前の操作可否へ戻し、移動は停止したままにする。
    if (player_) {
        player_->SetIsControlActive(unlockHadPlayerControl_);
        player_->SetVelocity({0.0f, 0.0f, 0.0f});
    }

    // 再生前の視点を戻してから追従を再接続し、補間を初期化して操作を返します。
    if (engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera()) {
        camera->SetEye(unlockCameraStartEye_);
        camera->SetTarget(unlockCameraStartTarget_);
        camera->SyncRotationToCurrentView();
        // プレイヤーがいる場合は、通常の追従と照準操作に戻す準備を行う。
        if (player_) {
            camera->SetFollowTarget(player_);
            camera->SetFollowMode(engine::Camera::FollowMode::kAimable);
            camera->ResetFollowSmoothing();
        }
        // 位置と追従先を整え、補間を初期化した後で初めてカメラ操作を受け付ける。
        camera->SetInputEnabled(true);
    }
}
