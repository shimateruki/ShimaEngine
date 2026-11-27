#define NOMINMAX
#include <cstdint>
#include "game/scene/gameplay/GamePlayScene.h"
#include "game/ui/ControlsGuideOverlay.h"
#include "game/ui/PauseMenuOverlay.h"
#include "game/ui/SettingsMenuOverlay.h"
#include "game/ui/SaveIndicatorOverlay.h"
#include "game/actor/enemy/spawn/EnemyFactory.h"
#include "game/system/scene/GimmickFactory.h"
#include "game/system/scene/ItemFactory.h"
#include "game/system/scene/LevelLoader.h"
#include "BaseEnemy.h"
#include "Object3D.h"
#include <algorithm>
#include <cmath>
#include <numbers>

// リプレイの物体を型情報から再生成し、ゴール演出や画面表示の途中状態もフレーム単位で保存・復元する。
std::unique_ptr<engine::Object3D> GamePlayScene::CreateReplayObject(const json& descriptor) {
    // 記録された型情報から実体を作り直し、シーンの描画基盤がない場合は復元しない。
    engine::Object3DCommon* common = GetObject3DCommon();

    // 描画用の共通情報がないか、記述が JSON オブジェクトでなければ生成しない。
    if (!common || !descriptor.is_object()) {
        return nullptr;
    }

    const std::string className = descriptor.value("className", std::string{});
    const std::string enemyType = descriptor.value("enemyType", std::string{});
    const std::string gimmickType = descriptor.value("gimmickType", std::string{});
    const std::string itemType = descriptor.value("itemType", std::string{});
    std::unique_ptr<engine::Object3D> object;

    // 型ごとの生成器を使います。敵とアイテムで記録と違う代替品になった場合は復元を中止します。
    // 敵の型名が記録されている場合は、敵の生成器で実体を作る。
    if (!enemyType.empty()) {
        object = EnemyFactory::GetInstance()->CreateEnemy(enemyType, common);

        // 生成できない場合や、記録と別の敵が返された場合は復元しない。
        if (!object || object->GetEnemyType() != enemyType) {
            return nullptr;
        }
    // 敵ではなくギミックの型名がある場合は、ギミックの生成器を選ぶ。
    } else if (!gimmickType.empty()) {
        object = GimmickFactory::GetInstance()->CreateGimmick(gimmickType, common);
    // 敵・ギミックでなくアイテムの型名がある場合は、アイテムの生成器を選ぶ。
    } else if (!itemType.empty()) {
        object = ItemFactory::GetInstance()->CreateItem(itemType, common);

        // 指定どおりのアイテムが得られなければ、別のアイテムで代用しない。
        if (!object || object->GetItemType() != itemType) {
            return nullptr;
        }
    // 専用の型名がなく、記録上 Model または Object の場合だけ基底の描画物を作る。
    } else if (className == "Model" || className == "Object") {
        object = std::make_unique<engine::Object3D>();
        object->Initialize(common);
    }

    // 対応する実体を用意できた場合だけ、再識別に使う名前や保存区分を記録から戻します。
    if (!object) {
        return nullptr;
    }

    object->SetName(descriptor.value("name", object->GetName()));
    object->SetClassName(className.empty() ? object->GetClassName() : className);
    object->SetSaveCategory(descriptor.value("saveCategory", object->GetSaveCategory()));
    // 敵の型名が記録されていれば、再生成した物体にそのタグを戻す。
    if (!enemyType.empty())
        object->SetEnemyType(enemyType);
    // ギミックの型名が記録されていれば、そのタグも戻す。
    if (!gimmickType.empty())
        object->SetGimmickType(gimmickType);
    // アイテムの型名が記録されていれば、そのタグも戻す。
    if (!itemType.empty())
        object->SetItemType(itemType);
    return object;
}

void GamePlayScene::OnReplayObjectsRecreated() {
    // 敵の移動対象など、生成後に必要なシーン参照を LevelLoader で結び直す。
    LevelLoader::ConfigureEnemyRuntimeReferences(this);
}

void GamePlayScene::CollectReplaySprites(std::vector<engine::Sprite*>& replaySprites) {
    // 共通Spriteに加え、ゲーム中のUIも記録対象へ含める。
    BaseScene::CollectReplaySprites(replaySprites);
    auto add = [&replaySprites](engine::Sprite* sprite) {
        // 生成済みのスプライトだけを記録用一覧に追加する。
        if (sprite) {
            replaySprites.push_back(sprite);
        }
    };

    // シーンが直接持つロックオン表示とゴール演出を追加します。
    add(lockOnSprite_.get());
    add(goalOverlayLandingRipple_.get());
    add(goalOverlayReturnPrompt_.get());
    add(goalOverlayReturnGlyph_.get());

    for (const auto& letter : goalOverlayStageClearLetters_) {
        add(letter.get());
    }

    for (const auto& sparkle : goalOverlaySparkles_) {
        add(sparkle.get());
    }

    // 結果表示の途中へシークしても、背景・獲得物の位置と数字のアトラス範囲を一緒に戻せるようにする。
    add(goalOverlayResultPanel_.get());
    add(goalOverlayFalseKingCrown_.get());
    add(goalOverlayClearTimeLabel_.get());
    for (const auto& glyph : goalOverlayClearTimeGlyphs_) add(glyph.get());
    add(goalOverlayRewardCoin_.get());
    for (const auto& trail : goalOverlayCoinTrails_) add(trail.get());
    add(goalOverlayCoinMultiplier_.get());
    for (const auto& star : goalOverlayRewardStars_) add(star.get());
    for (const auto& star : goalOverlayNewStars_) add(star.get());
    for (const auto& digit : goalOverlayRewardDigits_) add(digit.get());

    // 各オーバーレイが持つ要素は、そのオーバーレイ自身に列挙を任せます。
    // ポーズメニューがあれば、その部品を列挙する。
    if (pauseMenuOverlay_) {
        pauseMenuOverlay_->CollectReplaySprites(replaySprites);
    }

    // 操作ガイドがあれば、その部品を列挙する。
    if (controlsGuideOverlay_) {
        controlsGuideOverlay_->CollectReplaySprites(replaySprites);
    }

    // 設定画面があれば、その部品を列挙する。
    if (settingsOverlay_) {
        settingsOverlay_->CollectReplaySprites(replaySprites);
    }

    // 保存表示があれば、その部品も列挙する。
    if (saveIndicatorOverlay_) {
        saveIndicatorOverlay_->CollectReplaySprites(replaySprites);
    }
}

// オブジェクト以外の進行状態も保存し、再生位置を変えた際に HUD や演出の途中へ戻せるようにします。
void GamePlayScene::CaptureReplaySceneState(json& state) const {
    // オーバーレイと演出の状態をまとめ、フレーム復元時のUI差を防ぐ。
    json pauseState = json::object();
    json controlsGuideState = json::object();
    json settingsState = json::object();
    json saveIndicatorState = json::object();
    // ポーズメニューを作成済みなら、開閉などの状態を記録する。
    if (pauseMenuOverlay_)
        pauseMenuOverlay_->CaptureReplayState(pauseState);
    // 操作ガイドを作成済みなら、その記録を追加する。
    if (controlsGuideOverlay_)
        controlsGuideOverlay_->CaptureReplayState(controlsGuideState);
    // 設定画面を作成済みなら、その記録を追加する。
    if (settingsOverlay_)
        settingsOverlay_->CaptureReplayState(settingsState);
    // 保存表示を作成済みなら、その記録を追加する。
    if (saveIndicatorOverlay_)
        saveIndicatorOverlay_->CaptureReplayState(saveIndicatorState);

    // ゴールの再生時刻とHUDの残り時間を保ち、シーク後の動きを揃えます。
    state = {
        {"goal",
         {{"active", isGoal_},
          {"wasStageCleared", goalWasStageCleared_},
          {"state", static_cast<std::int32_t>(goalPresentationState_)},
          {"timer", goalPresentationTimer_.Elapsed()},
          {"starEmitTimer", goalStarEmitTimer_},
          {"burstEmitTimer", goalBurstEmitTimer_},
          {"crownIdleTime", goalCrownIdleTime_},
          {"crownSparkleTimer", goalCrownSparkleTimer_},
          {"crownSparklePattern", goalCrownSparklePatternIndex_},
          {"returnFadeStarted", goalReturnFadeStarted_},
          {"landingCuePlayed", goalLandingCuePlayed_},
          {"resultCuePlayed", goalResultCuePlayed_},
          {"cinematicPlaying", goalCinematicPlayer_.IsPlaying()}}},
        {"hud",
         {{"stageStarPulseTimers", hudStageStarPulseTimers_},
          {"stageStarCollected", hudStageStarVisualCollected_},
          {"lifeGainPulseTimer", hudLifeGainPulseTimer_},
          {"coinPulseTimer", hudCoinPulseTimer_},
          {"previousLives", hudPreviousLives_},
          {"previousCoins", hudPreviousCoins_},
          {"previousHp", healthHud_.State().previousHp},
          {"damagePulseTimer", healthHud_.State().damagePulseTimer},
          {"hurtIconTimer", healthHud_.State().hurtIconTimer},
          {"hpDamageHoldTimer", healthHud_.State().damageHoldTimer},
           {"hpDelayedRate", healthHud_.State().delayedRate},
           {"hpAnimationTimer", healthHud_.State().animationTimer}}},
        {"lifeLost", lifeLostState_.CaptureReplay()},
        {"overlays",
         {{"pause", std::move(pauseState)},
          {"pauseGuideSession", pauseGuideSession_},
          {"pausePageSliding", pausePageSliding_},
          {"pausePageToMenu", pausePageToMenu_},
          {"pausePageSlideTime", pausePageSlideTime_},
          {"controlsGuide", std::move(controlsGuideState)},
          {"settings", std::move(settingsState)},
          {"saveIndicator", std::move(saveIndicatorState)}}}};
}

// 保存されたゴール・HUD・残機減少演出・各画面を復元する。ポーズの画面遷移は古いキー名も受け付ける。
void GamePlayScene::RestoreReplaySceneState(const json& state) {
    // トップレベルがオブジェクトの形式でなければ、状態を変更せず戻る。
    if (!state.is_object()) {
        return;
    }

    // ゴール演出の項目がオブジェクトとして存在する場合だけ、状態と時計を読み戻す。
    if (const auto found = state.find("goal"); found != state.end() && found->is_object()) {
        const json& goal = *found;
        // 旧記録のばね項目は参照せず、冠の装着位置は再生時刻から求める。
        isGoal_ = goal.value("active", isGoal_);
        goalWasStageCleared_ = goal.value("wasStageCleared", goalWasStageCleared_);
        // 記録の数値を有効な列挙値に収めてから、ゴール演出の経過秒を時計に戻す。
        const std::int32_t stateValue = std::clamp(goal.value("state", static_cast<std::int32_t>(goalPresentationState_)), 0, 3);
        goalPresentationState_ = static_cast<GoalPresentationState>(stateValue);
        goalPresentationTimer_.Seek(goal.value("timer", goalPresentationTimer_.Elapsed()));
        goalStarEmitTimer_ = goal.value("starEmitTimer", goalStarEmitTimer_);
        goalBurstEmitTimer_ = goal.value("burstEmitTimer", goalBurstEmitTimer_);
        goalCrownIdleTime_ = goal.value("crownIdleTime", goalCrownIdleTime_);
        goalCrownSparkleTimer_ = goal.value("crownSparkleTimer", goalCrownSparkleTimer_);
        goalCrownSparklePatternIndex_ = goal.value("crownSparklePattern", goalCrownSparklePatternIndex_);
        goalReturnFadeStarted_ = goal.value("returnFadeStarted", goalReturnFadeStarted_);
        goalLandingCuePlayed_ = goal.value("landingCuePlayed", goalLandingCuePlayed_);
        goalResultCuePlayed_ = goal.value("resultCuePlayed", goalResultCuePlayed_);

        // タイムラインを読み込み済みで、ゴール演出が有効な段階にある場合だけ再生器を動かす。
        if (goalCinematicTimelineLoaded_ && isGoal_ && goalPresentationState_ != GoalPresentationState::kInactive) {
            // 演出の時刻と再生/停止を別々に復元し、停止中のフレームへも戻せるようにする。
            const bool shouldPlay = goal.value("cinematicPlaying", goalCinematicPlayer_.IsPlaying());
            // 復元した秒数へ移動するときのfalseは、通過したイベントをもう一度発火させないために使う。
            goalCinematicPlayer_.SetTime(goalPresentationTimer_.Elapsed(), false);

            // 記録では再生中だが現状態が停止中の場合は、現在時刻から再開する。
            if (shouldPlay && !goalCinematicPlayer_.IsPlaying()) {
                goalCinematicPlayer_.Resume(false);
            // 記録で停止しており現在は再生中の場合は、記録時刻で止める。
            } else if (!shouldPlay && goalCinematicPlayer_.IsPlaying()) {
                goalCinematicPlayer_.Pause();
            }
        }
    }

    // スター表示は配列の重なる範囲だけ更新し、旧記録や表示個数の違いを許容します。
    // HUD の項目がオブジェクトの場合だけ、表示更新に使う前回値や残り時間を戻す。
    if (const auto found = state.find("hud"); found != state.end() && found->is_object()) {
        const json& hud = *found;

        // 点滅時間の有効な配列があるときは、記録と現行の両方にある桁だけ戻す。
        if (const auto values = hud.find("stageStarPulseTimers"); values != hud.end() && values->is_array()) {
            for (size_t i = 0; i < hudStageStarPulseTimers_.size() && i < values->size(); ++i) {
                hudStageStarPulseTimers_[i] = (*values)[i].get<float>();
            }
        }

        // 取得済みの表示についても、配列がある場合だけ共通範囲に反映する。
        if (const auto values = hud.find("stageStarCollected"); values != hud.end() && values->is_array()) {
            for (size_t i = 0; i < hudStageStarVisualCollected_.size() && i < values->size(); ++i) {
                hudStageStarVisualCollected_[i] = (*values)[i].get<bool>();
            }
        }

        hudLifeGainPulseTimer_ = hud.value("lifeGainPulseTimer", hudLifeGainPulseTimer_);
        hudCoinPulseTimer_ = hud.value("coinPulseTimer", hudCoinPulseTimer_);
        hudPreviousLives_ = hud.value("previousLives", hudPreviousLives_);
        hudPreviousCoins_ = hud.value("previousCoins", hudPreviousCoins_);
        // 直前HPと遅延帯、肖像の被弾タイマーを揃え、再開直後に差分を被弾と取り違えないようにする。
        healthHud_.State().previousHp = hud.value("previousHp", healthHud_.State().previousHp);
        healthHud_.State().damagePulseTimer = hud.value("damagePulseTimer", healthHud_.State().damagePulseTimer);
        healthHud_.State().hurtIconTimer = hud.value("hurtIconTimer", healthHud_.State().hurtIconTimer);
        healthHud_.State().damageHoldTimer = hud.value("hpDamageHoldTimer", healthHud_.State().damageHoldTimer);
        healthHud_.State().delayedRate = hud.value("hpDelayedRate", healthHud_.State().delayedRate);
        healthHud_.State().animationTimer = hud.value("hpAnimationTimer", healthHud_.State().animationTimer);
    }

    // 残機減少演出のオブジェクトが記録にあれば、専用の復元処理へ渡す。
    if (const auto found = state.find("lifeLost"); found != state.end() && found->is_object()) {
        lifeLostState_.RestoreReplay(*found);
    }

    // 画面の記録がオブジェクトである場合だけ、開閉状態とスライドを戻す。
    if (const auto found = state.find("overlays"); found != state.end() && found->is_object()) {
        const json& overlays = *found;
        // 古いリプレイのキー名も受け取り、ポーズから操作説明への往復状態を維持する。
        pauseGuideSession_ = overlays.value("pauseGuideSession", overlays.value("returnToPauseAfterGuide", false));

        // ポーズ画面の実体と記録の両方があれば、開閉などを復元する。
        if (pauseMenuOverlay_ && overlays.contains("pause")) {
            pauseMenuOverlay_->RestoreReplayState(overlays["pause"]);
        }

        // 操作説明の実体と記録の両方があれば、その表示状態を復元する。
        if (controlsGuideOverlay_ && overlays.contains("controlsGuide")) {
            controlsGuideOverlay_->RestoreReplayState(overlays["controlsGuide"]);
        }

        // 両画面の開閉を先に戻した後、スライドの向きと0〜0.34秒の位置を再現する。
        pausePageSliding_ = overlays.value("pausePageSliding", false);
        pausePageToMenu_ = overlays.value("pausePageToMenu", true);
        pausePageSlideTime_ = std::clamp(overlays.value("pausePageSlideTime", 0.0f), 0.0f, 0.34f);
        // 途中のスライドを記録していた場合は、時間を進めずその時点の座標を反映する。
        if (pausePageSliding_)
            UpdatePausePageSlide(0.0f);
        // 設定画面の実体と記録の両方があれば、内部状態を復元する。
        if (settingsOverlay_ && overlays.contains("settings")) {
            settingsOverlay_->RestoreReplayState(overlays["settings"]);
        }

        // 保存表示の実体と記録の両方があれば、内部状態を復元する。
        if (saveIndicatorOverlay_ && overlays.contains("saveIndicator")) {
            saveIndicatorOverlay_->RestoreReplayState(overlays["saveIndicator"]);
        }
    }
}
