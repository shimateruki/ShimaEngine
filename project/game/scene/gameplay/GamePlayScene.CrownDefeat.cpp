#include "game/scene/gameplay/GamePlayScene.h"
#include "engine/graphics/3d/light/SceneLighting.h"
#include "game/actor/enemy/boss/false_king/EnemyFalseKingSlime.h"
#include "game/actor/gimmick/trigger/GimmickEventReceiver.h"
#include "game/system/progress/GameDataManager.h"
#include "game/system/progress/StageManager.h"
#include "SceneManager.h"
#include "CameraManager.h"
#include "PostEffect.h"
#include "CollisionConfig.h"
#include "Log.h"
#include <algorithm>
#include <cmath>

bool GamePlayScene::StartCrownDefeat(EnemyFalseKingSlime* boss) {
    // 資源と対象の検証を済ませてから、ゲーム側の操作と撃破判定を止める。
    // ボスや生存中のプレイヤーがいない場合、撃破演出が開始済みの場合、別の終幕やゴールが進行中の場合は開始しない。
    if (!boss || !player_ || player_->IsDead() || crownDefeat_.active || crownDefeat_.completed || crownFinale_.active || isGoal_)
        return false;
    // 演出アセットを準備できない場合は、プレイヤーを操作制限に入れる前に中止する。
    if (!crownDefeatVfxSequence_.Load("Resources/json/scenario/stage3/crown_defeat.json"))
        return false;
    // 最終闘技場へ移動済みの報酬Objectを、撃破中も同じインスタンスのまま使う。
    auto* reward = CinematicPlayer::ResolveSceneTarget(this, {kFalseKingRewardName, -1});
    auto& clips = crownDefeatVfxSequence_.presentation.objects;
    const auto crown = std::find_if(clips.begin(), clips.end(), [](const auto& clip) {
        return clip.name == kFalseKingRewardName;
    });
    if (crown != clips.end()) {
        crown->enabled = reward && reward->GetModel();
        if (crown->enabled) {
            // キーのXは頭から着地点への進行率、Yは軌道の高さ補正。着地姿勢はシーン配置から取る。
            const auto landing = reward->GetTranslate() - boss->GetTranslate();
            const auto bossScale = boss->GetScale();
            const auto bossRotation = boss->GetRotation();
            const float yaw = reward->GetRotation().y;
            const float turn = std::atan2(std::sin(yaw - bossRotation.y), std::cos(yaw - bossRotation.y));
            const engine::Vector3 head{0.0f, 2.86f * bossScale.y, 0.0f};
            for (auto& key : crown->keys) {
                const float travel = key.position.x;
                const float size = key.scale.x;
                key.position = engine::Math::Lerp(head, landing, travel) +
                    engine::Vector3{0.0f, key.position.y, 0.0f};
                key.rotation.y += bossRotation.y + turn * travel;
                key.scale = engine::Math::Lerp(bossScale, reward->GetScale(), size);
            }

            // 着地まで王冠が画面に残るよう、床へ向かう間だけ既存カットの注視点と距離を調整する。
            auto& presentation = crownDefeatVfxSequence_.presentation;
            const auto towardPlayer = player_->GetTranslate() - boss->GetTranslate();
            const auto viewYaw = presentation.viewWorldSpace ? 0.0f : std::atan2(towardPlayer.x, towardPlayer.z);
            const auto pan = CinematicAnchorPosition({landing.x * 0.72f, -0.72f, landing.z * 0.72f}, {}, -viewYaw);
            for (auto& key : presentation.view) {
                const float enter = (std::clamp)((key.time - 3.36f) / 1.60f, 0.0f, 1.0f);
                const float exit = (std::clamp)((7.15f - key.time) / 1.60f, 0.0f, 1.0f);
                const float weight = enter * enter * (3.0f - 2.0f * enter) * exit * exit * (3.0f - 2.0f * exit);
                key.focusOffset += pan * weight;
                key.eyeOffset = key.eyeOffset * (1.0f + 0.20f * weight);
            }
        }
    }

    // シーケンスの役割名を、この場で受け取ったボスと現プレイヤーへ結び付けます。
    crownDefeatVfxPlayer_.Initialize(SceneManager::GetInstance());
    crownDefeatVfxPlayer_.SetBindingResolver([this,boss,reward](const CinematicObjectBinding& binding) -> engine::Object3D* {
        // 演出内で $Actor を参照するトラックは、今回のボスへ結び付ける。
        if (binding.targetName=="$Actor") return boss;
        // 演出内で $Gate を参照するトラックは、今回のプレイヤーへ結び付ける。
        if (binding.targetName=="$Gate") return player_;
        if (binding.targetName==kFalseKingRewardName) return reward;
        return nullptr;
    });
    crownDefeatVfxPlayer_.SetSequence(&crownDefeatVfxSequence_);
    // 役者と勝利カットを全て確認してからプレイヤーをロックし、欠落時は演出を始めません。
    auto issues=crownDefeatVfxPlayer_.ValidateBindings();
    // 勝利カットのマーカーが正の時刻に無ければ、演出の準備失敗として登録する。
    if (GetCrownDefeatMarkerTime("victory")<=0)
        issues.push_back("勝利カットの節目がありません。");
    // 対象の結び付けやマーカーの検証に失敗した場合は記録を出し、プレイヤーをロックせず終了する。
    if (!issues.empty()) {
        for (const auto& issue:issues) LOG_ERROR("Crown defeat: {}",issue);
        crownDefeatVfxPlayer_.Stop(true);
        return false;
    }
    // 中断時に戻す値を退避し、勝利カットで使う向きをボスからプレイヤーへの方向で決めます。
    auto& s=crownDefeat_;
    s.boss=boss;
    s.hadControl=player_->IsControlActive();
    s.playerYaw=player_->GetMoveYaw();
    const auto forward=player_->GetTranslate()-boss->GetTranslate();
    // ボスからプレイヤーへ向かう水平ベクトルをラジアンへ変え、勝利カットの操作向きにする。
    s.victoryYaw=std::atan2(forward.x,forward.z);
    s.victoryTime=GetCrownDefeatMarkerTime("victory");
    // 本編のタイムは勝利カットの開始前に止め、エディターの撮影では更新しない。
    if (!crownMoviePreview_.active) {
        goalResultClearSeconds_ = static_cast<std::int32_t>(stageElapsedSeconds_);
    }
    player_->BeginCinematicLock(true);
    player_->SetVelocity({});
    // クラウン形態なら、演出のカメラに映すため材質を初期化して金色を戻す。
    if (player_->GetEnemyMorphType()==Player::EnemyMorphType::kCrown) {
        player_->SetMaterialType(0);
        player_->SetColor({1,.77f,.25f,1});
    }
    // 再生中に通常の敵更新で消えないよう、ボスの撃破確定を演出の終了まで保留する。
    boss->HoldFinalDefeat();
    if (auto* receiver = dynamic_cast<GimmickEventReceiver*>(reward); receiver && crown != clips.end() && crown->enabled) {
        receiver->SetCinematicRewardOwned(true);
    }
    s.active=true;
    // 一時部品・姿勢・見た目・粒子・カメラ・黒帯はエンジンの同じ時計で再生する。
    crownDefeatVfxPlayer_.Play(false);
    // 再生器が材質を退避した後に反射を有効にし、移動中も金属と宝石の輪郭を見せる。
    if (reward && crown != clips.end() && crown->enabled) {
        reward->SetEnableEnvMap(true);
        reward->SetEnvIntensity(0.85f);
        reward->SetRoughness(0.45f);
    }
    UpdateCrownDefeat(0);
    return true;
}

void GamePlayScene::UpdateCrownDefeat(float deltaTime) {
    // 撃破演出が動いている間だけ、演出の進行と完了を確認する。
    if (!crownDefeat_.active) return;
    // 必要な役者がいない状態で演出を進めず、一時的な演出状態を解放する。
    if (!player_ || !crownDefeat_.boss || player_->IsDead()) {
        ResetCrownDefeat();
        return;
    }
    if (crownMoviePreview_.active) {
        // プレビューは完了しても画面を保持し、ゲームの撃破・クリア通知を出さない。
        // プレビューでは次の再生秒数を直接指定し、正の時間を進めた場合だけ通過したイベントを出す。
        crownDefeatVfxPlayer_.SetTime(crownDefeatVfxPlayer_.GetCurrentTime()+(std::max)(0.0f,deltaTime),deltaTime>0);
    // 本番では、通常の演出経過時間でシーケンスを進める。
    } else {
        crownDefeatVfxPlayer_.Update((std::max)(0.0f,deltaTime));
        // 本編で終了に達する前に再生が止まった場合は、撃破完了の扱いにせず解除する。
        if (!crownDefeatVfxPlayer_.IsPlaying() && !crownDefeatVfxPlayer_.IsFinished()) {
            ResetCrownDefeat();
            return;
        }
    }
    // 操作用の向きはPlayer固有の前方軸を使うため、ゲーム側で通知する。
    player_->SetMoveYaw(crownDefeatVfxPlayer_.GetCurrentTime()>=crownDefeat_.victoryTime ? crownDefeat_.victoryYaw : crownDefeat_.playerYaw);
    player_->UpdateWorldMatrix(true);
    // 本編の再生完了だけを通常のクリア処理へ渡す。
    if (crownDefeatVfxPlayer_.IsFinished() && !crownMoviePreview_.active) FinishCrownDefeat();
}

// マーカーを取得できない場合は、未指定を表す0秒を返す。
float GamePlayScene::GetCrownDefeatMarkerTime(const std::string& name) const {
    return crownDefeatVfxSequence_.GetSignalTime(name);
}

void GamePlayScene::FinishCrownDefeat() {
    auto& s=crownDefeat_;
    // 本編の撃破演出が有効な場合だけ完了を確定する。
    if (!s.active || crownMoviePreview_.active) return;
    // 撃破演出の最終ポーズを維持したまま再生を止め、完了を一回だけ確定します。
    crownDefeatVfxPlayer_.Stop(false);
    s.active=false;
    s.completed=true;
    // 完了状態を先に立て、旧報酬ムービーを重ねず通常のクリア演出へ渡す。
    s.boss->CompleteFinalDefeat();

    // 撃破を確定したフレームでコピーを解除し、操作が戻る前に通常形態を反映する。
    player_->ClearCrownPower();
    player_->UpdateWorldMatrix(true);

    // 着地済みの王冠は姿勢を保つ。イベント受信側の落下を再生せず、取得判定だけを有効にする。
    auto* reward = CinematicPlayer::ResolveSceneTarget(this, {kFalseKingRewardName, -1});
    if (reward && reward->GetIsVisible()) {
        reward->SetCollisionAttribute(engine::collisionCategories::Get("trigger"));
        reward->SetCollisionMask(engine::collisionCategories::Get("player"));
        CameraManager::GetInstance()->SetActiveCamera(nullptr);
        PostEffect::GetInstance()->GetParams()->cinemaBarHeight = 0.0f;
        player_->SetVelocity({});
        player_->EndCinematicLock(s.hadControl);
        RefreshRenderCameraData();
        return;
    }

    // 王冠を配置していない検証用シーンでは、既存の撃破リザルトへ進む。
    goalWasStageCleared_=GameDataManager::GetInstance()->IsStageCleared(StageManager::GetInstance()->GetCurrentStageIndex());
    SetIsGoal(true);
    goalPresentationState_=GoalPresentationState::kCelebrating;
    // 勝利カットの画角と黒帯を受け継ぎ、見出しの前に結果用の構図へ切り替える。
    goalPresentationTimer_.Seek((std::max)(0.0f, GetGoalResultTitleStartTime() - 0.50f));
    goalPresentationTimer_.Resume();
    goalPlayerSnapshotValid_ = goalCrownSnapshotValid_ = false;
    goalCinematicPlayer_.Stop(false);
    goalCameraRestoreOverride_ = CameraManager::GetInstance()->GetActiveCameraOverride();
    goalDefeatResultBars_ = PostEffect::GetInstance()->GetParams()->cinemaBarHeight;
    SetupGoalPresentationCamera();
    UpdateGoalPresentationOverlay();
}

void GamePlayScene::ResetCrownDefeat() {
    auto& s=crownDefeat_;
    // 復元対象のボスを登録していない場合は、元の姿勢や状態を戻す処理へ進まない。
    if (!s.boss) return;
    // 結果カメラから撃破カメラへ一度戻してから、再生器の停止で元の視点へ復帰する。
    // 撃破完了後に結果用カメラを設定済みの場合は、ゴール状態を解除して撃破演出の視点へ戻す。
    if (s.completed && goalCameraSnapshotValid_) {
        SetIsGoal(false);
        goalCameraRestoreOverride_ = nullptr;
    }

    // 共通再生器が保存した姿勢・材質・表示・カメラを戻してから、ゲームのロックを解除する。
    crownDefeatVfxPlayer_.Stop(true);
    if (auto* reward = CinematicPlayer::ResolveSceneTarget(this, {kFalseKingRewardName, -1})) {
        // エディターへ戻る場合は取得判定と、イベント受信を止めていた状態を解除する。
        if (auto* receiver = dynamic_cast<GimmickEventReceiver*>(reward)) {
            receiver->SetCinematicRewardOwned(false);
        }
        reward->SetCollisionAttribute(0);
        reward->SetCollisionMask(0);
    }
    for (const auto& object:GetObjects())
        // 登録したボスが現在のシーンに残っていることを確認してから、最終撃破状態を解除する。
        if (object.get()==s.boss) { s.boss->CancelFinalDefeat(); break; }
    // プレイヤーを取得できた場合は、移動の向きと演出開始前の操作状態を復元する。
    if (player_) {
        player_->SetMoveYaw(s.playerYaw);
        player_->EndCinematicLock(s.hadControl);
    }
    s=CrownDefeatState{};
}

void GamePlayScene::DrawCrownDefeatWorld(const engine::SceneLighting& lighting) {
    // Drawで取得した共有ライトを渡し、直前の描画が残したGPU状態に依存させない。
    lighting.Draw(crownDefeatVfxPlayer_, &engine::CinematicPlayer::DrawPresentation);
}
