#define NOMINMAX
#include "game/scene/gameplay/GamePlayScene.h"
#include "game/scene/gameplay/GoalCinematicVfx.h"
#include "game/ui/InputGlyphResolver.h"
#include "engine/system/input/InputManager.h"
#include "Easing.h"
#include "BaseEnemy.h"
#include "CameraManager.h"
#include "DebugConsole.h"
#include "SceneManager.h"
#include "game/system/progress/StageManager.h"
#include "TextureManager.h"
#include "WinApp.h"
#include <algorithm>
#include <cmath>
#include <utility>
#include <numbers>
#include <fstream>
#include <cstdio>
#ifdef USE_IMGUI
#include "imgui.h"
#include "IconsFontAwesome5.h"
#endif

// ゴール演出のタイムライン、調整値、クリア結果の画面表示を扱う。ワールドの進行はゴール演出の更新側が担当する。
namespace {

constexpr float kRewardStarSequenceSeconds = 0.64f;
constexpr float kRewardStarStaggerSeconds = 0.12f;
constexpr float kRewardStarFallSeconds = 0.19f;
constexpr float kClearAtlasCellSize = 256.0f;
constexpr size_t kClearTwinkleCell = 10;
constexpr size_t kClearRimLightCell = 11;
constexpr size_t kRewardTwinkleStart = 3; // 先頭3枚はスライムの周囲に配置。
constexpr size_t kRimLightSprite = 6; // 最後の1枚は獲得枠の下辺に配置。
constexpr float kRewardStarSize = 94.0f;
constexpr float kRewardStarSpacing = 138.0f;
constexpr float kRewardPanelWidth = 548.0f;
constexpr float kRewardPanelStarHeight = 234.0f;
constexpr float kRewardPanelCompactHeight = 124.0f;
constexpr float kRewardSettleSeconds = 0.20f;
constexpr float kReturnPromptWidth = 332.0f; // 入力画像の枠を含めた帯の設計幅。
constexpr float kReturnPromptHeight = 51.0f;
constexpr const char* kGoalPresentationTuningPath = "Resources/json/cinematic/goal_clear.json";
constexpr const char* kGoalCinematicTimelinePath = "Resources/json/scenario/shared/goal_clear_timeline.json";

// リザルトの保存キーと、同じ値を編集・補正するための設定。
template <typename Tuning>
struct GoalResultUiField {
    const char* jsonKey;
    float Tuning::*value;
    float minimum;
    float maximum;
    const char* label;
    float dragSpeed;
    const char* format;
};

// JSON・表示位置や時間の補正・ImGuiが、同じキーと値の範囲を使う。
template <typename Tuning>
constexpr GoalResultUiField<Tuning> kGoalResultUiFields[] = {
    {"cardCenterX", &Tuning::resultUiCenterX, 0.58f, 0.80f, "獲得枠 X", 0.005f, "%.3f"},
    {"headingY", &Tuning::resultUiCenterY, 0.14f, 0.36f, "見出し Y", 0.005f, "%.3f"},
    {"panelTop", &Tuning::resultPanelTop, 0.33f, 0.63f, "獲得枠の上端 Y", 0.005f, "%.3f"},
    {"scale", &Tuning::resultUiScale, 0.60f, 1.35f, "UIスケール", 0.005f, "%.3f"},
    {"backdropAlpha", &Tuning::resultBackdropAlpha, 0.0f, 1.0f, "文字不透明度", 0.005f, "%.3f"},
    {"glowAlpha", &Tuning::resultGlowAlpha, 0.0f, 1.0f, "着地とスターの光", 0.005f, "%.3f"},
    {"letterStagger", &Tuning::resultLetterStagger, 0.02f, 0.20f, "文字間ディレイ", 0.001f, "%.3f 秒"},
    {"letterFallDuration", &Tuning::resultLetterFallDuration, 0.18f, 0.75f, "落下時間", 0.005f, "%.3f 秒"},
    {"letterDropHeight", &Tuning::resultLetterDropHeight, 80.0f, 520.0f, "落下距離", 1.0f, "%.0f px"},
    {"letterBounceHeight", &Tuning::resultLetterBounceHeight, 8.0f, 120.0f, "跳ね返り高さ", 0.5f, "%.1f px"},
    {"letterBounceDamping", &Tuning::resultLetterBounceDamping, 1.0f, 8.0f, "跳ね減衰", 0.05f, "%.2f"},
    {"letterBounceFrequency", &Tuning::resultLetterBounceFrequency, 4.0f, 18.0f, "跳ね速度", 0.05f, "%.2f"},
    {"coinCountDuration", &Tuning::resultCoinCountDuration, 0.20f, 1.80f, "コインを数える時間", 0.01f, "%.2f 秒"},
};

struct ClearLetterPose {
    float x; // 見出し中央からの横位置、設計px。
    float y; // 見出しの基準の高さからのずれ、設計px。
    float tilt; // バウンドが収まった後にも残す角度、rad。
};
constexpr std::array<ClearLetterPose, 7> kClearLetterPoses{{
    {-304.0f, 9.0f, -0.085f}, {-202.0f, -3.0f, -0.035f}, {-103.0f, -4.0f, 0.025f},
    {-2.0f, -13.0f, -0.045f}, {101.0f, -8.0f, 0.030f}, {202.0f, 0.0f, 0.065f},
    {304.0f, 9.0f, 0.083f},
}};

struct ClearTwinkle {
    engine::Vector2 offset; // 基準点からの開始位置、設計px。
    engine::Vector2 drift; // 消えるまでに移動する距離、設計px。
    float size; // 元のSpriteの幅と高さ、設計px。
    float delay; // 着地から発光までの秒数。
    float duration; // 発光してから消えるまでの秒数。
};
constexpr std::array<ClearTwinkle, 3> kSlimeTwinkles{{
    {{-159.0f, -15.0f}, {-18.0f, -24.0f}, 47.0f, 0.00f, 1.87f},
    {{-151.0f, 32.0f}, {-13.0f, -33.0f}, 23.0f, 0.16f, 1.74f},
    {{151.0f, 14.0f}, {15.0f, -28.0f}, 31.0f, 0.33f, 1.66f},
}};
constexpr std::array<ClearTwinkle, 3> kStarTwinkles{{
    {{-52.0f, -24.0f}, {-10.0f, -16.0f}, 39.0f, 0.00f, 0.61f},
    {{-32.0f, -48.0f}, {2.0f, -11.0f}, 24.0f, 0.07f, 0.48f},
    {{-57.0f, 1.0f}, {-10.0f, -7.0f}, 19.0f, 0.10f, 0.39f},
}};

// 4列のアトラスから、透明な余白を含む1セルを指定する。
void SetClearArtCell(engine::Sprite& sprite, size_t cell) {
    sprite.SetTextureRect({static_cast<float>(cell % 4) * kClearAtlasCellSize,
                           static_cast<float>(cell / 4) * kClearAtlasCellSize},
                          {kClearAtlasCellSize, kClearAtlasCellSize});
}

// 発生点からの秒数を直接受け取り、現在時刻から光の大きさ・移動・透明度を再現する。座標は画面ピクセル。
void UpdateClearTwinkle(engine::Sprite* sprite, const ClearTwinkle& twinkle, const engine::Vector2& origin,
                        float age, float scale, float opacity) {
    // 光のSpriteが生成されていなければ描画状態を更新しない。
    if (!sprite) return;

    const float localTime = age - twinkle.delay;
    // 透明度が正のときに限り、設定された遅延を過ぎて寿命を迎えるまで光を表示する。
    const bool active = opacity > 0.0f && localTime >= 0.0f && localTime < twinkle.duration;
    sprite->SetVisible(active);
    // 表示できる時間の外では、非表示だけ設定して姿勢更新を省く。
    if (!active) return;

    const float progress = localTime / twinkle.duration;
    const float pulse = std::sin(std::numbers::pi_v<float> * std::sqrt(progress));
    const float size = twinkle.size * (0.66f + 0.34f * pulse) * scale;
    sprite->SetPosition({origin.x + (twinkle.offset.x + twinkle.drift.x * progress) * scale,
                         origin.y + (twinkle.offset.y + twinkle.drift.y * progress) * scale});
    sprite->SetSize({size, size});
    sprite->SetColor({1.0f, 1.0f, 1.0f, opacity * pulse});
    sprite->Update();
}

float GoalClamp01(float value) {
    // 演出曲線を0〜1の時間区間に収める。
    return std::clamp(value, 0.0f, 1.0f);
}


} // namespace

void GamePlayScene::InitializeGoalCinematicTimeline() {
    // タイムラインのアニメーションと合図をPlayer用処理へ結び付けてから読込む。
    goalCinematicPlayer_.Initialize(SceneManager::GetInstance());
    goalCinematicPlayer_.SetAnimationCallback(
        [this](engine::Object3D* target, const CinematicAnimationClipData& clip, float localTime, bool isPreview) {
            HandleGoalCinematicAnimation(target, clip, localTime, isPreview);
        });
    goalCinematicPlayer_.SetSignalCallback(
        [this](engine::Object3D* target, const CinematicSignalMarker& signal, bool isPreview) {
            HandleGoalCinematicSignal(target, signal, isPreview);
        });

    goalCinematicTimelineLoaded_ = goalCinematicSequence_.Load(kGoalCinematicTimelinePath);

    // タイムラインが読めない場合は調整値から合図とトラックを作る。読めた場合は記録された合図の時刻を採用する。
    if (!goalCinematicTimelineLoaded_) {
        DebugConsole::GetInstance()->AddLog(std::string("Goal Timeline load failed: ") + kGoalCinematicTimelinePath);
        goalCinematicSequence_.Clear();
        goalCinematicSequence_.name = "goal_clear_timeline";
        SyncGoalCinematicTimelineFromTuning();
        goalCinematicTimelineLoaded_ = true;
    } else {
        ApplyGoalCinematicTimingFromSequence();
    }

    // 用意したシーケンスを最後に再生器へ渡す。
    goalCinematicPlayer_.SetSequence(&goalCinematicSequence_);
}

void GamePlayScene::ApplyGoalCinematicTimingFromSequence() {
    // 有効な合図の時刻を秒で取り出し、対応する王冠やプレイヤーの調整値に反映する。
    GoalClearPlayerAnimator::Tuning animation = goalClearPlayerAnimator_.GetTuning();

    // VFXの開始・終了と同じ規則で合図を読み、タイムライン上の時刻を使う。
    goalPresentationTuning_.crownFocusEndTime = goalCinematicSequence_.GetSignalTime("goal.crown_focus_end", goalPresentationTuning_.crownFocusEndTime);
    goalPresentationTuning_.crownMoveStartTime = goalCinematicSequence_.GetSignalTime("goal.crown_move_start", goalPresentationTuning_.crownMoveStartTime);
    animation.crownLandTime = goalCinematicSequence_.GetSignalTime("goal.crown_land", animation.crownLandTime);
    animation.anticipationStartTime = goalCinematicSequence_.GetSignalTime("goal.anticipation", animation.anticipationStartTime);
    animation.jumpStartTime = goalCinematicSequence_.GetSignalTime("goal.jump", animation.jumpStartTime);
    animation.apexTime = goalCinematicSequence_.GetSignalTime("goal.apex", animation.apexTime);
    animation.resultUiTime = goalCinematicSequence_.GetSignalTime("goal.result", animation.resultUiTime);
    animation.victoryLandTime = goalCinematicSequence_.GetSignalTime("goal.victory_land", animation.victoryLandTime);
    animation.readyTime = goalCinematicSequence_.GetSignalTime("goal.ready", animation.readyTime);
    goalClearPlayerAnimator_.SetTuning(animation);
}

// 調整値を合図・アニメーション・VFXへ逆に反映する。対応する部分がなければ作る。
void GamePlayScene::SyncGoalCinematicTimelineFromTuning() {
    const GoalClearPlayerAnimator::Tuning& animation = goalClearPlayerAnimator_.GetTuning();
    auto setSignalTime = [this](const char* signalId, const char* displayName, float time) {
        for (auto& marker : goalCinematicSequence_.signals) {
            // 同じ信号IDがあれば新しい行を作らず、時刻を変更して有効にする。
            if (marker.signal == signalId) {
                marker.time = time;
                marker.enabled = true;
                return;
            }
        }

        CinematicSignalMarker marker;
        marker.signal = signalId;
        marker.name = displayName;
        marker.time = time;
        goalCinematicSequence_.signals.push_back(marker);
    };

    // 王冠の構図から帰還可能になるまでの節目を、同じ再生時計へ書き戻します。
    setSignalTime("goal.crown_focus_end", "Crown Focus End", goalPresentationTuning_.crownFocusEndTime);
    setSignalTime("goal.crown_move_start", "Crown Move Start", goalPresentationTuning_.crownMoveStartTime);
    setSignalTime("goal.crown_land", "Crown Land", animation.crownLandTime);
    setSignalTime("goal.anticipation", "Anticipation", animation.anticipationStartTime);
    setSignalTime("goal.jump", "Celebration Bounce", animation.jumpStartTime);
    setSignalTime("goal.apex", "First Bounce Apex", animation.apexTime);
    setSignalTime("goal.result", "Result UI", animation.resultUiTime);
    setSignalTime("goal.victory_land", "First Bounce Landing", animation.victoryLandTime);
    setSignalTime("goal.ready", "Ready To Return", animation.readyTime);

    // タイムラインを手編集した後も、プレイヤー姿勢を駆動するクリップの終端を結果待ち時刻へ合わせる。
    bool hasAnimationDriver = false;

    for (auto& clip : goalCinematicSequence_.animationClips) {
        if (clip.driver == "GoalClearPlayer") {
            clip.startTime = 0.0f;
            clip.duration = animation.readyTime;
            clip.binding.targetName = player_ ? player_->GetName() : "player";
            clip.binding.targetEventId = player_ ? player_->GetEventId() : -1;
            hasAnimationDriver = true;
        }
    }

    // プレイヤーの姿勢を駆動するクリップがなければ、演出開始から帰還準備までを受け持つものを作る。
    if (!hasAnimationDriver) {
        CinematicAnimationClipData clip;
        clip.name = "Player Goal Celebration";
        clip.driver = "GoalClearPlayer";
        clip.clipName = "goal_clear";
        clip.duration = animation.readyTime;
        clip.binding.targetName = player_ ? player_->GetName() : "player";
        clip.binding.targetEventId = player_ ? player_->GetEventId() : -1;
        goalCinematicSequence_.animationClips.push_back(clip);
    }

    // 調整値と既存 VFX の開始時刻を同期し、未設定の演出だけ補完する。
    const auto* crown = FindGoalCrownObject();
    auto syncVfxTrack = [this, crown](const goalCinematicVfx::Cue& cue, float time, const char* signal) {
        bool found = false;
        for (auto& track : goalCinematicSequence_.vfxTracks) {
            // 同じ場面を扱う演出がすでにあれば、種類や対象を保ったまま開始時刻を合わせる。
            if (goalCinematicVfx::FindCue(track.sequenceName) == &cue) {
                track.startTime = time;
                found = true;
            }
        }
        // 同じ場面の演出がなければ、王冠かプレイヤーへ既定のVFXを結び付ける。
        if (!found) {
            CinematicVfxTrackData track;
            const auto* target = cue.followsCrown ? crown : player_;
            track.name = cue.label;
            track.sequenceName = cue.gold;
            track.startTime = time;
            track.startSignal = signal;
            // 対象が取れた場合は名前とイベントIDを登録する。未取得なら演出の参照名で解決する。
            if (target) {
                track.binding.targetName = target->GetName();
                track.binding.targetEventId = target->GetEventId();
            } else {
                track.binding.targetName = cue.followsCrown ? "goal" : "player";
                track.binding.targetEventId = -1;
            }
            goalCinematicSequence_.vfxTracks.push_back(track);
        }
    };

    syncVfxTrack(goalCinematicVfx::kLanding, animation.crownLandTime, "goal.crown_land");
    syncVfxTrack(goalCinematicVfx::kResult, animation.apexTime, "goal.apex");
    syncVfxTrack(goalCinematicVfx::kVictory, animation.victoryLandTime, "goal.victory_land");

    // 帰還できる時刻までを全尺とし、追加したトラックを含めて時刻順を整えます。
    goalCinematicSequence_.duration = animation.readyTime;
    goalCinematicSequence_.Sort();
}

void GamePlayScene::HandleGoalCinematicAnimation(engine::Object3D* target, const CinematicAnimationClipData& clip,
                                                 float localTime, bool isPreview) {
    // ゴール用ドライバーでPlayerが存在し、対象未指定かPlayer自身に結び付くクリップだけを適用する。
    (void)isPreview;

    if (clip.driver != "GoalClearPlayer" || !player_ || (target && target != player_)) {
        return;
    }

    goalClearPlayerAnimator_.Update(localTime);
    goalPlayerPosePosition_ = player_->GetTranslate();
    // ゴール状態では、共通再生器が着地VFXを発火する前に同じ時刻へ王冠を動かす。
    if (isGoal_) {
        goalPresentationTimer_.Seek(clip.startTime+localTime/(std::max)(clip.playbackSpeed,0.001f));
        UpdateGoalCrownMotion(0.0f);
    }
}

void GamePlayScene::HandleGoalCinematicSignal(engine::Object3D* target, const CinematicSignalMarker& signal, bool isPreview) {
    // 祝福演出中にgoal.readyを受け取った場合だけ、帰還待ちへ進む。プレビューも同じ合図を使う。
    (void)target;
    (void)isPreview;

    if (signal.signal == "goal.ready" && goalPresentationState_ == GoalPresentationState::kCelebrating) {
        goalPresentationState_ = GoalPresentationState::kReadyToReturn;
    }
}

void GamePlayScene::InitializeGoalPresentationOverlay() {
    // Sprite基盤が揃ってからフラッシュ・火花・帰還案内を生成する。
    if (!GetSpriteCommon()) {
        return;
    }

    const auto rippleHandle = engine::Sprite::LoadTexture("ui/result/clear/japanese/landing_ripple.png");

    // カメラへ投影した足元に、地面側だけに広がる着地の波紋を描く。
    goalOverlayLandingRipple_ = std::make_unique<engine::Sprite>();
    goalOverlayLandingRipple_->Initialize(GetSpriteCommon(), rippleHandle);
    goalOverlayLandingRipple_->SetName("GoalClear_LandingRipple");
    goalOverlayLandingRipple_->SetAnchorPoint({0.5f, 0.5f});

    const auto clearArt = engine::Sprite::LoadTexture("ui/result/clear/japanese/clear_celebration.png");

    // 見出しの各セルを独立させ、一文字ずつ落下させる。
    for (size_t i = 0; i < goalOverlayStageClearLetters_.size(); ++i) {
        auto& letter = goalOverlayStageClearLetters_[i];
        letter = std::make_unique<engine::Sprite>();
        letter->Initialize(GetSpriteCommon(), clearArt);
        SetClearArtCell(*letter, i);
        letter->SetName("GoalClear_Letter_" + std::to_string(i));
        letter->SetAnchorPoint({0.5f, 0.5f});
    }

    for (size_t i = 0; i < goalOverlaySparkles_.size(); ++i) {
        auto& sparkle = goalOverlaySparkles_[i];
        sparkle = std::make_unique<engine::Sprite>();
        sparkle->Initialize(GetSpriteCommon(), clearArt);
        SetClearArtCell(*sparkle, i == kRimLightSprite ? kClearRimLightCell : kClearTwinkleCell);
        sparkle->SetName("GoalClear_CelebrationLight_" + std::to_string(i));
        sparkle->SetEmissive(i == kRimLightSprite ? 1.22f : 1.35f);
        sparkle->SetAnchorPoint({0.5f, 0.5f});
    }

    // HUDと同じコレクション画像で数字・アイコンの意味を揃える。
    auto makeRewardSprite = [this](std::uint32_t texture, const std::string& name) {
        auto sprite = std::make_unique<engine::Sprite>();
        sprite->Initialize(GetSpriteCommon(), texture);
        sprite->SetName(name);
        sprite->SetAnchorPoint({0.5f, 0.5f});
        return sprite;
    };
    // 帰還先の帯は固定画像、左側の操作アイコンは割り当てに合わせる。
    goalOverlayReturnPrompt_ = makeRewardSprite(
        engine::Sprite::LoadTexture("ui/result/clear/japanese/return_prompt.png"), "GoalClear_ReturnText");
    const auto keyboardGlyph = MakeKeyboardGlyph(0);
    goalOverlayReturnGlyph_ = makeRewardSprite(engine::Sprite::LoadTexture(keyboardGlyph.path), "GoalClear_ReturnGlyph");
    goalOverlayReturnGlyph_->SetTextureName(keyboardGlyph.path);

    goalOverlayResultPanel_ = makeRewardSprite(
        engine::Sprite::LoadTexture("ui/result/clear/japanese/rewards_champagne_frame.png"), "GoalClear_ResultPanel");
    goalOverlayClearTimeLabel_ = makeRewardSprite(
        engine::Sprite::LoadTexture("ui/result/clear/japanese/clear_time_label.png"), "GoalClear_TimeLabel");
    const auto timeDigitsTexture = engine::Sprite::LoadTexture("ui/result/clear/japanese/clear_time_digits.png");
    for (std::size_t i = 0; i < goalOverlayClearTimeGlyphs_.size(); ++i) {
        goalOverlayClearTimeGlyphs_[i] = makeRewardSprite(timeDigitsTexture, "GoalClear_TimeGlyph_" + std::to_string(i));
    }

    goalRewardFilledStarTexture_ = engine::Sprite::LoadTexture("ui/result/clear/japanese/result_star_gold.png");
    goalRewardEmptyStarTexture_ = engine::Sprite::LoadTexture("ui/result/clear/japanese/result_star_slate.png");
    for (std::size_t i = 0; i < goalOverlayRewardStars_.size(); ++i) {
        goalOverlayRewardStars_[i] = makeRewardSprite(goalRewardEmptyStarTexture_, "GoalClear_Star_" + std::to_string(i));
        goalOverlayNewStars_[i] = makeRewardSprite(goalRewardFilledStarTexture_, "GoalClear_NewStar_" + std::to_string(i));
    }
    const auto coinTexture = engine::Sprite::LoadTexture("ui/hud/coin_icon.png");
    goalOverlayRewardCoin_ = makeRewardSprite(coinTexture, "GoalClear_Coin");
    goalOverlayFalseKingCrown_ = makeRewardSprite(
        engine::Sprite::LoadTexture("ui/result/clear/japanese/result_false_king_crown.png"), "GoalClear_FalseKingCrown");
    for (std::size_t i = 0; i < goalOverlayCoinTrails_.size(); ++i) {
        goalOverlayCoinTrails_[i] = makeRewardSprite(coinTexture, "GoalClear_CoinTrail_" + std::to_string(i));
    }
    const auto digitsTexture = engine::Sprite::LoadTexture("ui/result/clear/japanese/digits_light.png");
    goalOverlayCoinMultiplier_ = makeRewardSprite(digitsTexture, "GoalClear_CoinMultiplier");
    goalOverlayCoinMultiplier_->SetTextureRect({960.0f, 0.0f}, {96.0f, 128.0f});
    for (std::size_t i = 0; i < goalOverlayRewardDigits_.size(); ++i) {
        goalOverlayRewardDigits_[i] = makeRewardSprite(digitsTexture, "GoalClear_CoinDigit_" + std::to_string(i));
    }

    UpdateGoalPresentationOverlay();
}

float GamePlayScene::GetGoalResultTitleStartTime() const {
    // 通常の喜びジャンプが着地してから、見出しの最初の文字を落とす。
    const auto& animation = goalClearPlayerAnimator_.GetTuning();
    return std::max(animation.resultUiTime, animation.victoryLandTime + 0.08f);
}

bool GamePlayScene::HasFalseKingGoalReward() const {
    return goalCrownObject_ && goalCrownObject_->GetName() == kFalseKingRewardName;
}

float GamePlayScene::GetGoalRewardCoinStartTime() const {
    // 最後の文字の着地を基準に、上段の報酬を表示する時間を取る。
    const auto& tuning = goalPresentationTuning_;
    const float lastLetterLanding = GetGoalResultTitleStartTime() +
        static_cast<float>(goalOverlayStageClearLetters_.size() - 1) * tuning.resultLetterStagger +
        tuning.resultLetterFallDuration;
    const bool hasStars = StageManager::GetInstance()->GetCurrentStage().hasStarCoins;
    return lastLetterLanding + 0.20f + (hasStars || HasFalseKingGoalReward() ? kRewardStarSequenceSeconds : 0.0f);
}

// 演出の帰還準備と報酬表示の遅い方を返す。コインがある本編とプレビューでは数え上げと静止まで待つ。
float GamePlayScene::GetGoalRewardReadyTime() const {
    const bool countsCoins = goalEditorPreviewMode_ || goalResultCoins_ > 0;
    const float countDuration = countsCoins ? goalPresentationTuning_.resultCoinCountDuration + kRewardSettleSeconds : 0.0f;
    return std::max(goalClearPlayerAnimator_.GetTuning().readyTime, GetGoalRewardCoinStartTime() + countDuration);
}

// 見出しの下に獲得物を並べる。時刻はゴール演出開始からの秒、座標は画面ピクセル。
void GamePlayScene::UpdateGoalRewardOverlay(float time, float centerX, float rewardTop, float scale, bool visible) {
    const bool hasStars = StageManager::GetInstance()->GetCurrentStage().hasStarCoins;
    const float coinStart = GetGoalRewardCoinStartTime();
    const float starStart = coinStart - kRewardStarSequenceSeconds;
    const float appear = Easing::OutCubic01((time - starStart) / 0.18f);
    const bool hasCrown = HasFalseKingGoalReward();
    const bool hasTopReward = hasStars || hasCrown;
    const float panelStart = hasTopReward ? starStart : coinStart;
    const float panelIn = Easing::OutCubic01((time - panelStart) / 0.24f);
    const float panelTop = rewardTop + (1.0f - panelIn) * 12.0f * scale;
    const float panelHeight = hasTopReward ? kRewardPanelStarHeight : kRewardPanelCompactHeight;
    const float starY = panelTop + 77.0f * scale;
    const float coinY = panelTop + (hasTopReward ? 173.0f : 0.5f * panelHeight) * scale;
    const bool showStars = visible && hasStars && !hasCrown && time >= starStart;
    float latestStarImpactAge = -1.0f;
    float latestStarX = centerX;

    // 既取得は静止させ、新規スターだけ空枠へ落として着地の幅と輝きを変える。
    for (std::size_t i = 0; i < goalOverlayRewardStars_.size(); ++i) {
        auto& slot = goalOverlayRewardStars_[i];
        auto& reward = goalOverlayNewStars_[i];
        // 空枠と新規スターの両部品が揃わない枠は、表示の切替をできないので省く。
        if (!slot || !reward) continue;
        const auto earned = goalEditorPreviewMode_ && i < 2
            ? (i == 0 ? GoalResultStar::kNew : GoalResultStar::kOwned) : goalResultStars_[i];
        const float arrivalTime = time - starStart - 0.10f - static_cast<float>(i) * kRewardStarStaggerSeconds;
        const float age = arrivalTime - kRewardStarFallSeconds;
        const float fall = GoalClamp01(arrivalTime / kRewardStarFallSeconds);
        const float impact = GoalClamp01(age / 0.15f);
        const float pulse = std::sin(impact * std::numbers::pi_v<float>);
        const float x = centerX + (static_cast<float>(i) - 1.0f) * kRewardStarSpacing * scale;
        // 複数の新規スターが続けて着地すると、光は最後に着地した星へ移る。
        if (showStars && earned == GoalResultStar::kNew && age >= 0.0f &&
            (latestStarImpactAge < 0.0f || age < latestStarImpactAge)) {
            latestStarImpactAge = age;
            latestStarX = x;
        }
        slot->SetVisible(showStars && (earned != GoalResultStar::kNew || fall < 1.0f));
        slot->SetTextureHandle(earned == GoalResultStar::kOwned ? goalRewardFilledStarTexture_ : goalRewardEmptyStarTexture_);
        slot->SetPosition({x, starY});
        slot->SetSize({kRewardStarSize * scale, kRewardStarSize * scale});
        const float tone = earned == GoalResultStar::kOwned ? 0.91f : 1.0f;
        const float slotAlpha = earned == GoalResultStar::kOwned ? appear : appear * 0.88f;
        slot->SetColor({tone, tone, tone, slotAlpha});
        slot->Update();

        reward->SetVisible(showStars && earned == GoalResultStar::kNew && arrivalTime >= 0.0f);
        reward->SetPosition({x, starY - (55.0f * (1.0f - fall * fall) + 8.0f * pulse) * scale});
        reward->SetSize({(kRewardStarSize + 14.0f * pulse) * scale, (kRewardStarSize + 14.0f * pulse) * scale});
        reward->SetColor({1.0f, 1.0f, 1.0f, appear * Easing::OutCubic01(arrivalTime / 0.08f)});
        reward->SetEmissive(1.0f + 0.65f * pulse);
        reward->Update();
    }

    // 獲得枠が生成済みなら、報酬が現れる時刻に合わせて位置と透明度を変える。
    // 偽王の王冠は、スターと同じ着地の時計で獲得欄の中央に一個だけ置く。
    if (goalOverlayFalseKingCrown_) {
        const float arrival = time - starStart - 0.10f;
        const float age = arrival - kRewardStarFallSeconds;
        const float fall = GoalClamp01(arrival / kRewardStarFallSeconds);
        const float pulse = std::sin(GoalClamp01(age / 0.15f) * std::numbers::pi_v<float>);
        auto& crown = *goalOverlayFalseKingCrown_;
        const bool show = visible && hasCrown && arrival >= 0.0f;
        crown.SetVisible(show);
        if (show) {
            const float width = (150.0f + 10.0f * pulse) * scale;
            crown.SetPosition({centerX, starY - (48.0f * (1.0f - fall * fall) + 6.0f * pulse) * scale});
            crown.SetSize({width, width * (2.0f / 3.0f)});
            crown.SetColor({1.0f, 1.0f, 1.0f, appear * Easing::OutCubic01(arrival / 0.08f)});
            crown.Update();
            if (age >= 0.0f) latestStarImpactAge = age;
        }
    }

    if (goalOverlayResultPanel_) {
        goalOverlayResultPanel_->SetVisible(visible && panelIn > 0.0f);
        goalOverlayResultPanel_->SetPosition({centerX, panelTop + panelHeight * 0.5f * scale});
        goalOverlayResultPanel_->SetSize({kRewardPanelWidth * scale, panelHeight * scale});
        goalOverlayResultPanel_->SetColor({1.0f, 1.0f, 1.0f, panelIn});
        goalOverlayResultPanel_->Update();
    }

    // 金の十字光は星の左上、水色の反射は枠の下辺へ配置する。
    const float effectOpacity = visible && hasTopReward && !goalResultRevealed_ ? goalPresentationTuning_.resultGlowAlpha : 0.0f;
    for (size_t i = 0; i < kStarTwinkles.size(); ++i) {
        UpdateClearTwinkle(goalOverlaySparkles_[kRewardTwinkleStart + i].get(), kStarTwinkles[i],
                           {latestStarX, starY}, latestStarImpactAge, scale, effectOpacity);
    }
    // 枠の下辺用の反射があれば、最後に着地した新規スターの経過時間で表示を決める。
    if (auto* rimLight = goalOverlaySparkles_[kRimLightSprite].get()) {
        const bool active = effectOpacity > 0.0f && latestStarImpactAge >= 0.0f && latestStarImpactAge < 0.57f;
        rimLight->SetVisible(active);
        // 演出が有効でスターの着地から0.57秒未満の間だけ、反射を横へ広げる。
        if (active) {
            const float progress = latestStarImpactAge / 0.57f;
            const float pulse = std::sin(std::numbers::pi_v<float> * std::sqrt(progress));
            rimLight->SetPosition({centerX, panelTop + (panelHeight - 8.0f) * scale});
            rimLight->SetSize({(185.0f + 96.0f * progress) * scale, 52.0f * scale});
            rimLight->SetColor({1.0f, 1.0f, 1.0f, effectOpacity * pulse});
            rimLight->Update();
        }
    }

    // 最終桁数の幅を先に確保し、数を増やす途中でアイコンや桁の基準がずれないようにする。
    const float coinTime = time - coinStart;
    const std::int32_t totalCoins = goalEditorPreviewMode_ ? 24 : goalResultCoins_;
    const float duration = goalPresentationTuning_.resultCoinCountDuration;
    const float countRate = GoalClamp01(coinTime / duration);
    const std::int32_t count = countRate >= 1.0f ? totalCoins
        : static_cast<std::int32_t>(static_cast<double>(totalCoins) * Easing::OutCubic01(countRate));
    const std::string digits = std::to_string(count);
    const auto maxDigits = std::to_string(totalCoins).size();
    const float digitStep = std::min(25.0f, 109.0f / static_cast<float>(maxDigits));
    const float coinX = centerX - (160.0f + 0.5f * static_cast<float>(maxDigits) * digitStep) * scale;
    const float spin = totalCoins > 0 ? GoalClamp01(coinTime / 0.27f) : 1.0f;
    const float width = 0.20f + 0.80f * std::abs(std::cos((spin - 1.0f) * std::numbers::pi_v<float>));
    const float pop = totalCoins > 0 ? std::sin(GoalClamp01((coinTime - duration) / kRewardSettleSeconds) *
                               std::numbers::pi_v<float>) : 0.0f;
    const float alpha = Easing::OutCubic01(coinTime / 0.12f);
    const bool coinVisible = visible && coinTime >= 0.0f;

    // 獲得枚数の範囲内にある残像だけを時差で飛ばし、0.26秒でコインへ合流させる。
    for (std::size_t i = 0; i < goalOverlayCoinTrails_.size(); ++i) {
        auto& trail = goalOverlayCoinTrails_[i];
        const float index = static_cast<float>(i);
        const float age = coinTime - index * 0.055f;
        // 残像の描画部品が無ければ飛行位置を計算しない。
        if (!trail) continue;
        const bool inFlight = coinVisible && totalCoins > static_cast<std::int32_t>(i) && age >= 0.0f && age < 0.26f;
        trail->SetVisible(inFlight);
        // 表示開始前・終了後・枚数超過の残像は、非表示だけ指定して位置などの更新を省く。
        if (!inFlight) continue;
        const float progress = GoalClamp01(age / 0.26f);
        const float remaining = 1.0f - progress * progress * progress;
        trail->SetPosition({coinX - (58.0f - index * 20.0f) * remaining * scale,
                            coinY - (19.0f + index * 23.0f) * remaining * scale});
        const float size = (20.0f - 6.0f * progress) * scale;
        trail->SetSize({size, size});
        trail->SetColor({1.0f, 1.0f, 1.0f, std::sin(progress * std::numbers::pi_v<float>) * 0.90f});
        trail->Update();
    }

    // 桁数が増えても、数の右端と掛け記号は同じ位置を使う。生成済みのコインには回転と最後の跳ねを加える。
    if (goalOverlayRewardCoin_) {
        const float iconSize = (42.0f + 7.0f * pop) * scale;
        goalOverlayRewardCoin_->SetVisible(coinVisible);
        goalOverlayRewardCoin_->SetPosition({coinX, coinY - 5.0f * pop * scale});
        goalOverlayRewardCoin_->SetSize({iconSize * width, iconSize});
        goalOverlayRewardCoin_->SetColor({1.0f, 1.0f, 1.0f, alpha});
        goalOverlayRewardCoin_->Update();
    }
    // 掛け記号がある場合は、コインの右に置いて数と同時に見せる。
    if (goalOverlayCoinMultiplier_) {
        goalOverlayCoinMultiplier_->SetVisible(coinVisible);
        goalOverlayCoinMultiplier_->SetPosition({coinX + 36.0f * scale, coinY});
        goalOverlayCoinMultiplier_->SetSize({22.0f * scale, 35.0f * scale});
        goalOverlayCoinMultiplier_->SetColor({1.0f, 1.0f, 1.0f, alpha});
        goalOverlayCoinMultiplier_->Update();
    }
    // 作成済みで、現在の枚数表示に必要な桁だけ画像の切り出しと座標を更新する。
    for (std::size_t i = 0; i < goalOverlayRewardDigits_.size(); ++i) {
        auto& digit = goalOverlayRewardDigits_[i];
        if (!digit) continue;
        digit->SetVisible(coinVisible && i < digits.size());
        if (!coinVisible || i >= digits.size()) continue;
        digit->SetTextureRect({static_cast<float>(digits[i] - '0') * 96.0f, 0.0f}, {96.0f, 128.0f});
        digit->SetPosition({coinX + (59.0f + static_cast<float>(maxDigits - digits.size() + i) * digitStep) * scale, coinY});
        digit->SetSize({(digitStep + 3.0f) * scale, 37.0f * scale});
        digit->SetColor({1.0f, 1.0f, 1.0f, alpha});
        digit->Update();
    }

    // クリア秒を分と秒へ分け、秒を2桁にそろえる。プレビューは例として4分38秒を表示する。
    const std::int32_t seconds = goalEditorPreviewMode_ ? 278 : goalResultClearSeconds_;
    const std::string clock = std::to_string(seconds / 60) + (seconds % 60 < 10 ? ":0" : ":") +
                              std::to_string(seconds % 60);
    // クリアタイムの見出しがあれば、コインと同時に結果枠の右へ表示する。
    if (goalOverlayClearTimeLabel_) {
        goalOverlayClearTimeLabel_->SetVisible(coinVisible);
        goalOverlayClearTimeLabel_->SetPosition({centerX + 33.0f * scale, coinY});
        goalOverlayClearTimeLabel_->SetSize({128.0f * scale, 30.0f * scale});
        goalOverlayClearTimeLabel_->SetColor({1.0f, 1.0f, 1.0f, alpha});
        goalOverlayClearTimeLabel_->Update();
    }

    const float glyphStep = std::min(26.0f, 135.0f / static_cast<float>(clock.size()));
    float glyphLeft = centerX + (178.0f - 0.5f * (static_cast<float>(clock.size()) - 0.5f) * glyphStep) * scale;
    // 必要な文字数に対応する部品だけを更新し、区切り記号には数字より狭い送り幅を使う。
    for (std::size_t i = 0; i < goalOverlayClearTimeGlyphs_.size(); ++i) {
        auto& glyph = goalOverlayClearTimeGlyphs_[i];
        if (!glyph) continue;
        glyph->SetVisible(coinVisible && i < clock.size());
        if (!coinVisible || i >= clock.size()) continue;
        const bool separator = clock[i] == ':';
        const float cell = separator ? 10.0f : static_cast<float>(clock[i] - '0');
        const float glyphWidth = glyphStep * (separator ? 0.50f : 1.0f) * scale;
        glyph->SetTextureRect({cell * 96.0f, 0.0f}, {96.0f, 128.0f});
        glyph->SetPosition({glyphLeft + glyphWidth * 0.5f, coinY});
        glyphLeft += glyphWidth;
        glyph->SetSize({(glyphStep + 2.0f) * scale, 42.0f * scale});
        glyph->SetColor({1.0f, 1.0f, 1.0f, alpha});
        glyph->Update();
    }
}

void GamePlayScene::UpdateGoalPresentationOverlay() {
    // ワールドの演出時計から、見出し・獲得欄・帰還案内の表示順を決める。
    const bool visible = isGoal_ && goalPresentationState_ != GoalPresentationState::kInactive;
    const float screenW = static_cast<float>(WinApp::GetClientWidth());
    const float screenH = static_cast<float>(WinApp::GetClientHeight());
    const float returnStartTime = GetGoalRewardReadyTime();
    // 早送りは表示用の時刻に限定し、カメラやプレイヤーの演出時計には触れない。
    const float t = goalResultRevealed_ ? std::max(goalPresentationTimer_.Elapsed(), returnStartTime + 0.34f)
                                       : goalPresentationTimer_.Elapsed();
    const float titleStart = GetGoalResultTitleStartTime();
    const GoalPresentationTuning& presentation = goalPresentationTuning_;
    const float baseScale = (std::min)(screenW / 1280.0f, screenH / 720.0f);
    const float uiScale = baseScale * presentation.resultUiScale;
    // 見出しを画面全体の中央、獲得枠を右側に置き、共通スケールで画面内に収める。
    const float panelMargin = (0.5f * kRewardPanelWidth + 30.0f) * uiScale;
    const float centerX = std::clamp(screenW * presentation.resultUiCenterX, panelMargin, screenW - panelMargin);
    const bool hasStars = StageManager::GetInstance()->GetCurrentStage().hasStarCoins;
    const float panelHeight = hasStars || HasFalseKingGoalReward() ? kRewardPanelStarHeight : kRewardPanelCompactHeight;
    const float rewardTop = std::min(screenH * presentation.resultPanelTop,
                                     screenH - (panelHeight + 78.0f) * uiScale);
    const float baselineY = std::min(screenH * presentation.resultUiCenterY, rewardTop - 69.0f * uiScale);

    // 波紋Spriteがあれば、通常の祝福着地から0.44秒の表示を更新する。早送りや撃破演出経由では表示しない。
    if (goalOverlayLandingRipple_) {
        const float contactTime = goalClearPlayerAnimator_.GetTuning().victoryLandTime;
        const float age = goalPresentationTimer_.Elapsed() - contactTime;
        auto* camera = CameraManager::GetInstance()->GetActiveCamera();
        const bool show = visible && !goalResultRevealed_ &&
                          (!crownDefeat_.completed || goalCrownSnapshotValid_) &&
                          player_ && player_->GetModel() && camera && age >= 0.0f && age < 0.44f;
        goalOverlayLandingRipple_->SetVisible(show);
        // 表示条件が揃う場合だけ、プレイヤーモデルの足元をカメラで画面へ投影する。
        if (show) {
            const auto lo = player_->GetModel()->GetLocalAabbMin();
            const auto hi = player_->GetModel()->GetLocalAabbMax();
            const auto foot = engine::Math::Transform({(lo.x + hi.x) * 0.5f, lo.y, (lo.z + hi.z) * 0.5f},
                                                       player_->GetWorldMatrix());
            const auto ndc = engine::Math::Transform(foot, camera->GetViewProjectionMatrix());
            const float progress = GoalClamp01(age / 0.44f);
            const float spread = Easing::OutCubic01(progress);
            goalOverlayLandingRipple_->SetPosition({(ndc.x + 1.0f) * screenW * 0.5f,
                                                    (1.0f - ndc.y) * screenH * 0.5f + 4.0f * baseScale});
            goalOverlayLandingRipple_->SetSize({(234.0f + 172.0f * spread) * baseScale,
                                                (19.0f + 27.0f * spread) * baseScale});
            goalOverlayLandingRipple_->SetColor({1.0f, 1.0f, 1.0f, 0.95f * (1.0f - progress)});
            goalOverlayLandingRipple_->Update();
        }
    }

    // スライムの胴の少し上を投影し、ワールドの着地に合わせて周りに金の光を散らす。
    const float heroAge = goalPresentationTimer_.Elapsed() - goalClearPlayerAnimator_.GetTuning().victoryLandTime;
    auto* camera = CameraManager::GetInstance()->GetActiveCamera();
    const bool showHeroGlints = visible && !goalResultRevealed_ &&
                          (!crownDefeat_.completed || goalCrownSnapshotValid_) &&
                               player_ && player_->GetModel() && camera && heroAge >= 0.0f && heroAge < 2.0f;
    engine::Vector2 heroCenter{};
    // 通常の着地から2秒以内でモデルとカメラが揃う場合にだけ、胴の少し上を光の基準として投影する。
    if (showHeroGlints) {
        const auto lo = player_->GetModel()->GetLocalAabbMin();
        const auto hi = player_->GetModel()->GetLocalAabbMax();
        const auto center = engine::Math::Transform({(lo.x + hi.x) * 0.5f, std::lerp(lo.y, hi.y, 0.62f),
                                                     (lo.z + hi.z) * 0.5f}, player_->GetWorldMatrix());
        const auto ndc = engine::Math::Transform(center, camera->GetViewProjectionMatrix());
        heroCenter = {(ndc.x + 1.0f) * screenW * 0.5f, (1.0f - ndc.y) * screenH * 0.5f};
    }
    for (size_t i = 0; i < kSlimeTwinkles.size(); ++i) {
        UpdateClearTwinkle(goalOverlaySparkles_[i].get(), kSlimeTwinkles[i], heroCenter, heroAge,
                           baseScale, showHeroGlints ? presentation.resultGlowAlpha : 0.0f);
    }

    // 文字ごとの着地点をゆるい弧にし、バウンドの終点でも高さと傾きを残す。
    constexpr float kGlyphSize = 126.0f;
    for (size_t i = 0; i < goalOverlayStageClearLetters_.size(); ++i) {
        engine::Sprite* letter = goalOverlayStageClearLetters_[i].get();
        const auto& pose = kClearLetterPoses[i];
        const float letterCenterX = screenW * 0.5f + pose.x * uiScale;
        const float settledY = baselineY + (pose.y - 7.0f) * uiScale;
        const float localTime = t - titleStart - static_cast<float>(i) * presentation.resultLetterStagger;
        // 各文字に時間差を与え、落下前・接地後・静止後の姿勢を同じ時計から再現する。
        float letterY = settledY - presentation.resultLetterDropHeight * 0.44f * uiScale;
        float scaleX = 0.82f;
        float scaleY = 1.18f;
        float rotation = pose.tilt * 2.0f;
        float letterAlpha = 0.0f;

        // 文字ごとの開始時刻より前は透明を保ち、開始後にだけ落下と着地反応を計算する。
        if (localTime >= 0.0f) {
            letterAlpha = Easing::OutCubic01(localTime / 0.10f);

            // 落下時間内は下方向へ加速させながら、最終の大きさと傾きへ近づける。
            if (localTime < presentation.resultLetterFallDuration) {
                const float fallRate = GoalClamp01(localTime / presentation.resultLetterFallDuration);
                const float acceleratedFall = fallRate * fallRate;
                letterY = settledY - presentation.resultLetterDropHeight * 0.44f * (1.0f - acceleratedFall) * uiScale;
                const float approach = Easing::SmoothStep01(fallRate);
                scaleX = 0.82f + 0.18f * approach;
                scaleY = 1.18f - 0.18f * approach;
                rotation = pose.tilt * (2.0f - approach);
            } else {
                // 接地した一文字を横に広げ、跳ね返りを減衰させて読みやすい位置に止める。
                const float bounceTime = localTime - presentation.resultLetterFallDuration;
                const float damping = std::exp(-bounceTime * (presentation.resultLetterBounceDamping + 2.4f));
                const float bouncePhase = bounceTime * presentation.resultLetterBounceFrequency;
                const float rebound = std::abs(std::sin(bouncePhase)) * damping;
                const float contactWave = (std::max)(0.0f, std::cos(bouncePhase));
                const float contact = std::pow(contactWave, 7.0f) * damping;
                letterY = settledY - presentation.resultLetterBounceHeight * 0.37f * rebound * uiScale;
                scaleX = 1.0f + contact * 0.34f - rebound * 0.075f;
                scaleY = 1.0f - contact * 0.26f + rebound * 0.11f;
                rotation = pose.tilt + 0.07f * std::sin(bouncePhase) * damping;
            }
        }

        // 文字のSpriteが用意されていれば、求めた姿勢を反映する。リザルトが無効なら透明にする。
        if (letter) {
            letter->SetPosition({letterCenterX, letterY});
            letter->SetSize({kGlyphSize * scaleX * uiScale, kGlyphSize * scaleY * uiScale});
            letter->SetRotation(rotation);
            letter->SetColor({1.0f, 1.0f, 1.0f,
                              visible ? letterAlpha * presentation.resultBackdropAlpha : 0.0f});
            letter->Update();
        }

    }

    UpdateGoalRewardOverlay(t, centerX, rewardTop, uiScale, visible);

    // 記録を保存した後、報酬表示が揃うか早送り済みの場合に帰還案内を出す。すでに帰還中の場合は隠す。
    const bool returnReady = visible && goalSavePerformed_ &&
                             goalPresentationState_ != GoalPresentationState::kReturning &&
                             (goalResultRevealed_ || t >= returnStartTime);
    const float reveal = GoalClamp01((t - returnStartTime) / 0.34f);
    const float fade = Easing::OutCubic01(reveal);
    const float pop = std::sin(std::numbers::pi_v<float> * reveal);
    const float promptScale = uiScale * (1.0f + 0.031f * pop);
    const float promptY = rewardTop + (panelHeight + 39.0f + (1.0f - fade) * 10.0f - pop * 4.0f) * uiScale;
    // 帯のSpriteが生成済みなら表示可否とフェードを反映する。
    if (goalOverlayReturnPrompt_) {
        goalOverlayReturnPrompt_->SetVisible(returnReady);
        goalOverlayReturnPrompt_->SetPosition({centerX, promptY});
        goalOverlayReturnPrompt_->SetSize({kReturnPromptWidth * promptScale, kReturnPromptHeight * promptScale});
        goalOverlayReturnPrompt_->SetColor({1.0f, 1.0f, 1.0f, fade});
        goalOverlayReturnPrompt_->Update();
    }

    // 操作画像があり、帰還できる状態で入力機器が使える場合に、決定処理と同じJumpの割り当てを表示する。
    if (goalOverlayReturnGlyph_) {
        const bool showGlyph = returnReady && inputManager_;
        goalOverlayReturnGlyph_->SetVisible(showGlyph);
        if (showGlyph) {
            const auto glyph = ResolveActionGlyph("Jump", inputManager_->IsGamepadMode());
            // 画像パスが変わった場合だけ再読み込みし、同じアトラス内のキー変更でも切り抜きは更新する。
            if (goalOverlayReturnGlyph_->GetTextureName() != glyph.path) {
                goalOverlayReturnGlyph_->SetTextureHandle(engine::Sprite::LoadTexture(glyph.path));
                goalOverlayReturnGlyph_->SetTextureName(glyph.path);
            }

            // キーキャップと円形のパッド画像を同じ表示領域へ縦横比を保って収める。
            const auto& metadata = engine::TextureManager::GetInstance()->GetMetadata(goalOverlayReturnGlyph_->GetTextureHandle());
            const float width = glyph.useCrop ? glyph.width : static_cast<float>(metadata.width);
            const float height = glyph.useCrop ? glyph.height : static_cast<float>(metadata.height);
            const float fit = std::min(66.0f / width, 40.0f / height) * promptScale;
            goalOverlayReturnGlyph_->SetTextureRect({glyph.cropX, glyph.cropY}, {width, height});
            goalOverlayReturnGlyph_->SetPosition({centerX - 115.0f * promptScale, promptY});
            goalOverlayReturnGlyph_->SetSize({width * fit, height * fit});
            goalOverlayReturnGlyph_->SetColor({1.0f, 1.0f, 1.0f, fade});
            goalOverlayReturnGlyph_->Update();
        }
    }
}

void GamePlayScene::DrawGoalPresentationOverlay() {
    // ゴール中で演出の状態が有効な場合だけ、作成済みのオーバーレイ部品を順に描く。可視性は各Spriteが判定する。
    if (!isGoal_ || goalPresentationState_ == GoalPresentationState::kInactive) {
        return;
    }

    // 波紋をワールドの手前に描き、獲得枠は報酬アイコンの下に重ねる。
    if (goalOverlayLandingRipple_) {
        goalOverlayLandingRipple_->Draw();
    }

    for (size_t i = 0; i < kRewardTwinkleStart; ++i) {
        if (goalOverlaySparkles_[i]) goalOverlaySparkles_[i]->Draw();
    }

    for (auto& letter : goalOverlayStageClearLetters_) {
        if (letter) {
            letter->Draw();
        }
    }

    if (goalOverlayResultPanel_) goalOverlayResultPanel_->Draw();
    if (goalOverlayFalseKingCrown_) goalOverlayFalseKingCrown_->Draw();
    for (const auto& star : goalOverlayRewardStars_) {
        if (star) star->Draw();
    }
    for (const auto& star : goalOverlayNewStars_) {
        if (star) star->Draw();
    }
    // 枠とスターより手前に着地光を重ね、コイン・タイムの文字を読みやすくする。
    for (size_t i = kRewardTwinkleStart; i < goalOverlaySparkles_.size(); ++i) {
        if (goalOverlaySparkles_[i]) goalOverlaySparkles_[i]->Draw();
    }
    for (const auto& trail : goalOverlayCoinTrails_) {
        if (trail) trail->Draw();
    }
    if (goalOverlayRewardCoin_) goalOverlayRewardCoin_->Draw();
    if (goalOverlayCoinMultiplier_) goalOverlayCoinMultiplier_->Draw();
    for (const auto& digit : goalOverlayRewardDigits_) {
        if (digit) digit->Draw();
    }
    if (goalOverlayClearTimeLabel_) goalOverlayClearTimeLabel_->Draw();
    for (const auto& glyph : goalOverlayClearTimeGlyphs_) {
        if (glyph) glyph->Draw();
    }
    if (goalOverlayReturnPrompt_) goalOverlayReturnPrompt_->Draw();
    if (goalOverlayReturnGlyph_) goalOverlayReturnGlyph_->Draw();
}

void GamePlayScene::LoadGoalPresentationTuning() {
    // ゴール演出のJSONから存在する設定欄を読み、ファイル不在時や解析例外時も現在の値を範囲内に補正する。
    std::ifstream file(kGoalPresentationTuningPath, std::ios::binary);

    if (!file.is_open()) {
        SanitizeGoalPresentationTuning();
        return;
    }

    try {
        nlohmann::json root;
        file >> root;
        GoalClearPlayerAnimator::Tuning animation = goalClearPlayerAnimator_.GetTuning();

        // timelineがオブジェクトなら、動作と表示の開始時刻を現在値へ読み込む。
        if (const auto it = root.find("timeline"); it != root.end() && it->is_object()) {
            const auto& j = *it;
            goalPresentationTuning_.crownFocusEndTime =
                j.value("crownFocusEndTime", goalPresentationTuning_.crownFocusEndTime);
            goalPresentationTuning_.crownMoveStartTime =
                j.value("crownMoveStartTime", goalPresentationTuning_.crownMoveStartTime);
            animation.crownLandTime = j.value("crownLandTime", animation.crownLandTime);
            animation.anticipationStartTime = j.value("anticipationStartTime", animation.anticipationStartTime);
            animation.jumpStartTime = j.value("jumpStartTime", animation.jumpStartTime);
            animation.apexTime = j.value("apexTime", animation.apexTime);
            animation.resultUiTime = j.value("resultUiTime", animation.resultUiTime);
            animation.victoryLandTime = j.value("victoryLandTime", animation.victoryLandTime);
            animation.readyTime = j.value("readyTime", animation.readyTime);
        }

        // playerがオブジェクトなら、着地から祝福までのプレイヤーの移動と変形を読み込む。
        if (const auto it = root.find("player"); it != root.end() && it->is_object()) {
            const auto& j = *it;
            animation.settleDuration = j.value("settleDuration", animation.settleDuration);
        }

        // crownがオブジェクトなら、降り始める高さ、頭への沈み込み、体幅に対する大きさを読み込む。
        if (const auto it = root.find("crown"); it != root.end() && it->is_object()) {
            const auto& j = *it;
            goalPresentationTuning_.crownDropHeight = j.value("dropHeight", goalPresentationTuning_.crownDropHeight);
            goalPresentationTuning_.crownSeatDepth = j.value("seatDepth", goalPresentationTuning_.crownSeatDepth);
            goalPresentationTuning_.crownWearWidthRatio = j.value("wearWidthRatio", goalPresentationTuning_.crownWearWidthRatio);
        }

        // cameraがオブジェクトなら、カットごとの距離・位置・画角を読み込む。
        if (const auto it = root.find("camera"); it != root.end() && it->is_object()) {
            const auto& j = *it;
            goalPresentationTuning_.cameraSafeMargin = j.value("safeMargin", goalPresentationTuning_.cameraSafeMargin);
            goalPresentationTuning_.resultCameraDistance =
                j.value("resultDistance", goalPresentationTuning_.resultCameraDistance);
            goalPresentationTuning_.resultCameraHeight =
                j.value("resultHeight", goalPresentationTuning_.resultCameraHeight);
            goalPresentationTuning_.resultTargetSide =
                j.value("resultTargetSide", goalPresentationTuning_.resultTargetSide);
            goalPresentationTuning_.resultFov = j.value("resultFov", goalPresentationTuning_.resultFov);
        }

        // uiがオブジェクトなら、位置と時間などを共通の設定表に従って読み込む。キーがない欄は現在値を保つ。
        if (const auto it = root.find("ui"); it != root.end() && it->is_object()) {
            for (const auto& field : kGoalResultUiFields<GoalPresentationTuning>) {
                float& value = goalPresentationTuning_.*field.value;
                value = it->value(field.jsonKey, value);
            }
        }

        goalClearPlayerAnimator_.SetTuning(animation);
        SanitizeGoalPresentationTuning();
    } catch (const std::exception& e) {
        DebugConsole::GetInstance()->AddLog(std::string("Goal presentation settings load failed: ") + e.what());
        SanitizeGoalPresentationTuning();
    }
}

void GamePlayScene::SaveGoalPresentationTuning() const {
    // 編集した時間と見た目の値を同じJSONへ保存し、次回の演出へ引き継ぐ。
    const GoalClearPlayerAnimator::Tuning& a = goalClearPlayerAnimator_.GetTuning();
    const GoalPresentationTuning& g = goalPresentationTuning_;
    nlohmann::json root;
    root["version"] = 1;
    root["timeline"] = {{"crownFocusEndTime", g.crownFocusEndTime},
                        {"crownMoveStartTime", g.crownMoveStartTime},
                        {"crownLandTime", a.crownLandTime},
                        {"anticipationStartTime", a.anticipationStartTime},
                        {"jumpStartTime", a.jumpStartTime},
                        {"apexTime", a.apexTime},
                        {"resultUiTime", a.resultUiTime},
                        {"victoryLandTime", a.victoryLandTime},
                        {"readyTime", a.readyTime}};
    // 時間の節目と見た目の補正値を、読み込み時と同じ区分・キーで書き出します。
    root["player"] = {{"settleDuration", a.settleDuration}};
    root["crown"] = {{"dropHeight", g.crownDropHeight}, {"seatDepth", g.crownSeatDepth}, {"wearWidthRatio", g.crownWearWidthRatio}};
    root["camera"] = {{"safeMargin", g.cameraSafeMargin}, {"resultDistance", g.resultCameraDistance},
                      {"resultHeight", g.resultCameraHeight}, {"resultTargetSide", g.resultTargetSide},
                      {"resultFov", g.resultFov}};
    auto& ui = root["ui"];
    for (const auto& field : kGoalResultUiFields<GoalPresentationTuning>) {
        ui[field.jsonKey] = g.*field.value;
    }

    std::ofstream file(kGoalPresentationTuningPath, std::ios::binary);

    // 書き込み先が開けた場合だけ、4スペースで整形したJSONを出力する。
    if (file.is_open()) {
        file << root.dump(4);
    }
}

void GamePlayScene::SanitizeGoalPresentationTuning() {
    // 王冠着地から結果UIまでの時刻順を保ち、編集値でイベントが逆転しないようにする。
    GoalClearPlayerAnimator::Tuning a = goalClearPlayerAnimator_.GetTuning();
    GoalPresentationTuning& g = goalPresentationTuning_;
    g.crownFocusEndTime = std::clamp(g.crownFocusEndTime, 0.20f, 1.50f);
    g.crownMoveStartTime = std::max(g.crownMoveStartTime, g.crownFocusEndTime + 0.12f);
    a.crownLandTime = std::max(a.crownLandTime, g.crownMoveStartTime + 0.70f);
    a.anticipationStartTime = std::max(a.anticipationStartTime, a.crownLandTime + 0.06f);
    a.jumpStartTime = std::max(a.jumpStartTime, a.anticipationStartTime + 0.10f);
    a.apexTime = std::max(a.apexTime, a.jumpStartTime + 0.36f);
    a.resultUiTime = std::max(a.resultUiTime, a.apexTime + 0.02f);
    a.victoryLandTime = std::max(a.victoryLandTime, a.apexTime + 0.36f);
    a.readyTime = std::max(a.readyTime, a.victoryLandTime + 0.55f);
    // イベントの順序を決めた後、姿勢・画角・UIの値を描画可能な範囲へ制限します。
    a.settleDuration = std::clamp(a.settleDuration, 0.05f, 0.65f);
    g.crownWearWidthRatio = std::clamp(g.crownWearWidthRatio, 0.35f, 1.10f);
    g.cameraSafeMargin = std::clamp(g.cameraSafeMargin, 0.04f, 0.20f);
    g.crownDropHeight = std::clamp(g.crownDropHeight, 0.30f, 4.0f);
    g.crownSeatDepth = std::clamp(g.crownSeatDepth, 0.0f, 0.45f);
    g.resultFov = std::clamp(g.resultFov, 0.30f, 1.10f);
    for (const auto& field : kGoalResultUiFields<GoalPresentationTuning>) {
        float& value = g.*field.value;
        value = std::clamp(value, field.minimum, field.maximum);
    }
    goalClearPlayerAnimator_.SetTuning(a);
}

void GamePlayScene::DrawGoalPresentationEditor() {
    // Development/Debugの調整UIで変更した値を演出へ反映する。
#ifdef USE_IMGUI
    // シーケンスの見出しを開いた場合だけ、編集と再生の欄を描く。
    if (!ImGui::CollapsingHeader(ICON_FA_FILM " ゴール演出シーケンス", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    GoalClearPlayerAnimator::Tuning& a = goalClearPlayerAnimator_.EditTuning();
    GoalPresentationTuning& g = goalPresentationTuning_;
    bool changed = false;
    ImGui::Separator();
    ImGui::TextUnformatted("プレイヤー");
    changed |= ImGui::DragFloat("取得姿勢を整える時間", &a.settleDuration, 0.01f, 0.05f, 0.65f, "%.2f 秒");

    ImGui::Separator();
    ImGui::TextUnformatted("王冠");
    changed |= ImGui::DragFloat("降下開始高さ", &g.crownDropHeight, 0.01f, 0.30f, 4.0f, "%.2f");
    changed |= ImGui::DragFloat("頭への沈み", &g.crownSeatDepth, 0.005f, 0.0f, 0.45f, "%.3f");
    changed |= ImGui::DragFloat("体幅に対する王冠の幅", &g.crownWearWidthRatio, 0.01f, 0.35f, 1.10f, "%.2f");

    ImGui::Separator();
    ImGui::TextUnformatted("リザルトの構図補正");
    changed |= ImGui::DragFloat("画面端の余白", &g.cameraSafeMargin, 0.005f, 0.04f, 0.20f, "%.3f");
    changed |= ImGui::DragFloat("注視点の横ずらし", &g.resultTargetSide, 0.01f, -4.0f, 4.0f, "%.2f");
    changed |= ImGui::DragFloat("撃破直後のリザルト距離", &g.resultCameraDistance, 0.01f, 1.0f, 16.0f, "%.2f");
    changed |= ImGui::DragFloat("撃破直後のリザルト高さ", &g.resultCameraHeight, 0.01f, -4.0f, 12.0f, "%.2f");
    changed |= ImGui::DragFloat("撃破直後のリザルトFOV", &g.resultFov, 0.005f, 0.30f, 1.10f, "%.3f");

    ImGui::Separator();
    ImGui::TextUnformatted("クリアUI");
    for (const auto& field : kGoalResultUiFields<GoalPresentationTuning>) {
        changed |= ImGui::DragFloat(field.label, &(g.*field.value), field.dragSpeed, field.minimum, field.maximum, field.format);
    }

    // 見た目の値を有効範囲へ戻し、演出時計にはDirectorで保存した合図を使う。
    if (changed) {
        SanitizeGoalPresentationTuning();
        ApplyGoalCinematicTimingFromSequence();
    }

    // GhostDirectorで保存した最新データを、実ステージでの確認に読み込む。
    ImGui::BeginDisabled(isGoal_ && !goalEditorPreviewMode_);
    if (ImGui::Button(ICON_FA_PLAY " 実ステージでプレビュー")) {
        CinematicSequence edited;
        // 保存の途中などで読めなければ、動作中のシーケンスを維持する。
        if (edited.Load(kGoalCinematicTimelinePath)) {
            StopGoalPresentationPreview();
            goalCinematicSequence_ = std::move(edited);
            ApplyGoalCinematicTimingFromSequence();
            goalCinematicPlayer_.SetSequence(&goalCinematicSequence_);
            goalCinematicTimelineLoaded_ = true;
            StartGoalPresentationPreview();
        } else {
            DebugConsole::GetInstance()->AddLog(std::string("Goal Timeline reload failed: ") + kGoalCinematicTimelinePath);
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();

    // 停止を押したらプレビューだけを閉じ、利用したシーン状態の復元へ進む。
    if (ImGui::Button(ICON_FA_STOP " プレビュー停止")) {
        StopGoalPresentationPreview();
    }

    // 見た目の設定は従来のJSONへ保存する。タイムラインはGhostDirectorが保存する。
    if (ImGui::Button(ICON_FA_SAVE " JSON保存")) {
        SanitizeGoalPresentationTuning();
        ApplyGoalCinematicTimingFromSequence();
        SaveGoalPresentationTuning();
    }

    ImGui::SameLine();

    // 設定の再読込を押したら調整用JSONを読み、現在のシーケンスにも対応する時刻を反映する。
    if (ImGui::Button(ICON_FA_FOLDER_OPEN " JSON再読込")) {
        LoadGoalPresentationTuning();
        ApplyGoalCinematicTimingFromSequence();
    }


#endif
}

engine::Object3D* GamePlayScene::FindGoalCrownObject() const {
    // ObjectManagerがあれば、最初のゴールイベント付きObjectの参照を返す。見つからなければnullptr。
    if (!objectManager_) {
        return nullptr;
    }

    for (const auto& object : objectManager_->GetObjects()) {
        if (object && object->GetEventType() == EventType::kGoal) {
            return object.get();
        }
    }

    return nullptr;
}

void GamePlayScene::StartGoalPresentationPreview() {
    // 本編のゴール進行中は重ねて開始せず、編集プレビューだけ再始動を許す。
    if (isGoal_) {
        if (!goalEditorPreviewMode_) {
            DebugConsole::GetInstance()->AddLog("Goal presentation is already running in gameplay.");
            return;
        }

        // 既に編集中のプレビューなら、一度終了して演出開始前の状態へ戻す。
        StopGoalPresentationPreview();
    }

    engine::Object3D* crown = FindGoalCrownObject();

    // 王冠かPlayerを取得できなければ、開始せずログに必要な対象を示す。
    if (!crown || !player_) {
        DebugConsole::GetInstance()->AddLog("Goal presentation preview requires player and crown.");
        return;
    }

    // プレビューとして開始し、リザルトを表示できる状態にする。クリア保存の実行は本編側でプレビューを除外する。
    goalEditorPreviewMode_ = true;
    StartGoalPresentation(crown);
    goalSavePerformed_ = true;
}

void GamePlayScene::StopGoalPresentationPreview() {
    // プレビューフラグが立っている場合だけゴール状態を解除し、SetIsGoal(false)で操作・カメラ・姿勢を復元する。
    if (!goalEditorPreviewMode_) {
        return;
    }

    SetIsGoal(false);
    goalEditorPreviewMode_ = false;
}
