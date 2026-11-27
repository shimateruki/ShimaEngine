#include <cstdint>
#include "game/actor/enemy/boss/false_king/BossIntroSettings.h"
#define NOMINMAX
#include "game/scene/gameplay/GamePlayScene.h"
#include "game/ui/SaveIndicatorOverlay.h"
#include "engine/editor/graphics/camera/CameraEditor.h"
#include "CameraManager.h"
#include "DebugConsole.h"
#include "game/actor/enemy/spawn/EnemyFactory.h"
#include "BaseEnemy.h"
#include "game/actor/enemy/boss/prism/EnemyPrismSlime.h"
#include "game/actor/enemy/boss/false_king/EnemyFalseKingSlime.h"
#include "game/system/progress/StageManager.h"
#include "SceneManager.h"
#include "game/system/progress/GameDataManager.h"
#include "game/system/combat/LockOnSystem.h"
#include "game/actor/player/state/PlayerState.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#ifdef USE_IMGUI
#include "imgui.h"
#include "engine/editor/core/commands/EditorCommandRegistry.h"
#include "engine/editor/core/EditorManager.h"
#include "engine/editor/scene/DebugTeleportEditor.h"
#include "IconsFontAwesome5.h"
#endif

// ゲーム中のデバッグUIと、更新開始時に処理するステージ移動の予約を扱う。
namespace {
#ifdef USE_IMGUI
constexpr const char* kDebugPrismArenaAnchorName = "Stage1_Collision_V4_PreBossCheckpoint";
constexpr const char* kDebugPrismSlimeName = "Stage1_V4_PrismArena_Boss";
constexpr const char* kDebugGoalEntryAnchorName = "Stage1_Collision_V4_GoalApproach";
constexpr const char* kDebugGoalName = "goal";
constexpr float kDebugTeleportPlayerFloorClearance = 0.78f;
constexpr float kDebugTeleportCameraDistance = 16.0f;
constexpr float kDebugTeleportCameraHeight = 3.2f;
constexpr float kDebugTeleportCameraPitch = 0.32f;

const char* GetDebugMorphDisplayName(Player::EnemyMorphType type) {
    // 内部の変身種別を、デバッグUIで識別できる日本語名へ変換する。
    switch (type) {
    case Player::EnemyMorphType::kSlime:
        return "ピンクスライム";

    case Player::EnemyMorphType::kBomber:
        return "ボムスライム";

    case Player::EnemyMorphType::kFireSlime:
        return "炎スライム";

    case Player::EnemyMorphType::kThunderSlime:
        return "雷スライム";

    case Player::EnemyMorphType::kWindSlime:
        return "風スライム";

    case Player::EnemyMorphType::kNone:
        return "通常スライム";

    default:
        return "その他";
    }
}
#endif
} // namespace

void GamePlayScene::DrawImGui() {
#ifdef USE_IMGUI
    // ステージ切替、プレイヤー状態、各ボスの演出確認をこのSceneのデバッグ面へ集める。
    ImGui::Text(ICON_FA_INFO_CIRCLE " Scene: GamePlay");
    // 疾走線の調整欄が開いている場合に、描画の濃さと画面比の倍率を編集できる。
    if (ImGui::CollapsingHeader("ダッシュパネルの疾走線")) {
        ImGui::SliderFloat("最大不透明度", &dashSpeedLineMaxAlpha_, 0.0f, 1.0f);
        ImGui::SliderFloat("画面に対するサイズ", &dashSpeedLineScale_, 0.85f, 1.25f);
    }
    ImGui::Separator();

    // ステージ欄を開いている場合は、現行の配置情報と選択・再読込の操作を出す。
    if (ImGui::CollapsingHeader(ICON_FA_MAP " Stage Selection", ImGuiTreeNodeFlags_DefaultOpen)) {
        // 登録済みのステージだけを選べるようにし、ボタンは選択とScene再生成をまとめて行う。
        const auto& stages = StageManager::GetInstance()->GetStages();
        std::int32_t currentIndex = StageManager::GetInstance()->GetCurrentStageIndex();

        ImGui::Text("ステージJSON切り替え");
        constexpr std::int32_t kStageButtonCount = 5;

        for (std::int32_t stageIndex = 0; stageIndex < kStageButtonCount; ++stageIndex) {
            const bool registered = stageIndex < static_cast<std::int32_t>(stages.size());
            const bool isCurrent = stageIndex == currentIndex;

            // 登録されていない番号と現在プレイ中の番号のボタンは、選択を無効にする。
            if (!registered || isCurrent) {
                ImGui::BeginDisabled();
            }

            std::string label = std::string(ICON_FA_PLAY) + " Stage " + std::to_string(stageIndex + 1);

            // 登録済みの切替ボタンを押した場合は、番号の変更と GAMEPLAY の再生成を一度に要求する。
            if (ImGui::Button(label.c_str(), ImVec2(112.0f, 30.0f)) && registered) {
                StageManager::GetInstance()->SetCurrentStage(stageIndex);
                SceneManager::GetInstance()->ChangeScene("GAMEPLAY");
            }

            // 無効化したボタンの描画後は、次の番号へ状態を持ち越さないよう解除する。
            if (!registered || isCurrent) {
                ImGui::EndDisabled();
            }

            // 最後のボタン以外は次の番号を同じ行に置く。
            if (stageIndex < kStageButtonCount - 1) {
                ImGui::SameLine();
            }
        }

        // 選択番号が登録範囲内の場合だけ、ステージ名と二つの配置ファイルを表示する。
        if (currentIndex >= 0 && currentIndex < static_cast<std::int32_t>(stages.size())) {
            const StageData& currentStage = stages[currentIndex];
            ImGui::TextDisabled("現在: %s", currentStage.name.c_str());
            ImGui::TextDisabled("3D: %s", currentStage.levelPath.c_str());
            ImGui::TextDisabled("Sprite: %s", currentStage.spritePath.c_str());
        }

        ImGui::Separator();

        // Combo側は選択だけを更新し、明示的なReloadボタンでSceneを読み直す。
        std::vector<const char*> stageNames;

        for (const auto& s : stages)
            stageNames.push_back(s.name.c_str());

        // コンボの選択を変えた場合は番号だけ更新し、この時点ではシーンを再読込しない。
        if (ImGui::Combo("Select Stage", &currentIndex, stageNames.data(), (std::int32_t)stageNames.size())) {
            StageManager::GetInstance()->SetCurrentStage(currentIndex);
        }

        // 再読込ボタンで、その時点で選択済みのステージへ遷移を要求する。
        if (ImGui::Button(ICON_FA_SYNC " Reload Scene with Selected Stage", ImVec2(-1, 30))) {
            SceneManager::GetInstance()->ChangeScene("GAMEPLAY");
        }
    }

    // 進行欄を開いた場合に、セーブのクリア履歴とこのプレイの収集状況を表示する。
    if (ImGui::CollapsingHeader(ICON_FA_TROPHY " Stage Status", ImGuiTreeNodeFlags_DefaultOpen)) {
        // 永続的なクリア状態と、今回のプレイ中に拾ったスターを別々に表示する。
        std::int32_t currentStage = StageManager::GetInstance()->GetCurrentStageIndex();
        bool isCleared = GameDataManager::GetInstance()->IsStageCleared(currentStage);

        ImGui::Text("Stage ID: %d", currentStage);
        ImGui::SameLine();
        // 保存上クリア済みなら緑色で表示する。
        if (isCleared)
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "[ CLEARED ]");
        // 未クリアなら未到達の状態を灰色で表示する。
        else
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "[ NOT CLEARED ]");

        ImGui::Separator();
        ImGui::Text("Star Coins (Session):");

        for (std::int32_t i = 0; i < 3; i++) {
            ImGui::SameLine();

            // このプレイで対応するスターを拾っていれば、金色の印にする。
            if (sessionStarCoins_[i]) {
                ImGui::TextColored(ImVec4(1, 0.9f, 0, 1), ICON_FA_STAR);
            // 未取得の枠は進行中の収集対象として暗く表示する。
            } else {
                ImGui::TextDisabled(ICON_FA_STAR);
            }

            // ポインターが直前のスターに重なる間に、その枠の番号を表示する。
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Star Coin %d", i);
        }
    }

    // プレイヤー欄を開いた場合に、現在の実体で確認できる調整項目を表示する。
    if (ImGui::CollapsingHeader(ICON_FA_USER " Player Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
        // プレイヤーが生成済みの場合に、位置、HP、コピーなどの操作を使える。
        if (player_) {
            engine::Vector3 pos = player_->GetTranslate();
            ImGui::Text("Position: (%.2f, %.2f, %.2f)", pos.x, pos.y, pos.z);

            float hp = player_->GetHp();
            float maxHp = player_->GetMaxHp();
            ImGui::ProgressBar(hp / maxHp, ImVec2(-1, 0), "HP");

            // HPはデバッグ確認のため実データへ直接書き込み、残機とコインは管理器経由で戻す。
            // HP を1にする操作は、瀕死の状態を再現する。
            if (ImGui::Button(ICON_FA_AMBULANCE " HPを1にする (Debug)")) {
                // プレイヤーにステータスが登録されていれば、パラメーターへ直接反映する。
                if (player_->GetParameters().has_value()) {
                    player_->SetParameter(&engine::EntityParameter::hp, 1.0f);
                }
            }

            ImGui::SameLine();

            // HP を0にする操作は、通常更新で死亡条件を再現する。
            if (ImGui::Button(ICON_FA_SKULL " HPを0にする (Debug)")) {
                // ステータスが利用できなければ、値を書き込まない。
                if (player_->GetParameters().has_value()) {
                    player_->SetParameter(&engine::EntityParameter::hp, 0.0f);
                }
            }

            ImGui::Separator();
            std::int32_t lives = GameDataManager::GetInstance()->GetLives();
            std::int32_t coins = GameDataManager::GetInstance()->GetCoins();
            ImGui::Text(ICON_FA_HEART " Remaining Lives: %d", lives);
            ImGui::Text(ICON_FA_COINS " Coins: %d / 100", coins);

            // 残機のリセットボタンは、ゲームデータ側の初期値に戻す処理を呼ぶ。
            if (ImGui::Button("Reset Lives to 3")) {
                GameDataManager::GetInstance()->ResetLives();
            }

            ImGui::SameLine();

            // コインのリセットボタンは、ゲームデータに累積値の初期化を依頼する。
            if (ImGui::Button("Reset Coins")) {
                GameDataManager::GetInstance()->ResetCoins();
            }

            ImGui::Separator();

            // 移動エディターを開くボタンで、Inspector の対象をそのツールへ変える。
            if (ImGui::Button("デバッグ移動エディターを開く")) {
                EditorManager::GetInstance()->SetSelectedObject(&DebugTeleportEditor::GetInstance());
            }

            ImGui::Separator();

            // 吸収を待たずに実装済み能力を確認するため、変身型を直接選べるようにする。
            ImGui::Text("デバッグ変身（時間制限なし）");
            ImGui::TextDisabled("現在: %s", GetDebugMorphDisplayName(player_->GetEnemyMorphType()));

            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const float buttonWidth = (std::max)(90.0f, (ImGui::GetContentRegionAvail().x - spacing * 2.0f) / 3.0f);
            auto drawMorphButton = [this, buttonWidth](const char* label, const char* enemyType) {
                // 形態ボタンが押された場合だけ、指定の敵を元に無期限のコピーを試みる。
                if (ImGui::Button(label, ImVec2(buttonWidth, 30.0f))) {
                    ApplyDebugPlayerMorph(enemyType);
                }
            };

            drawMorphButton("ピンク##DebugMorph", "Slime");
            ImGui::SameLine();
            drawMorphButton("ボム##DebugMorph", "Bomber");
            ImGui::SameLine();
            drawMorphButton("炎##DebugMorph", "FireSlime");

            drawMorphButton("雷##DebugMorph", "ThunderSlime");
            ImGui::SameLine();
            drawMorphButton("風##DebugMorph", "WindSlime");
            ImGui::SameLine();

            const bool hasMorph = player_->IsEnemyMorphed();

            // コピー中でなければ、通常形態へ戻すボタンを無効にする。
            if (!hasMorph) {
                ImGui::BeginDisabled();
            }

            // 戻す操作ではプレイヤーのコピーとデバッグ用の元個体を片付ける。
            if (ImGui::Button("通常スライムへ戻す##DebugMorph", ImVec2(-1.0f, 30.0f))) {
                ClearDebugPlayerMorph();
            }

            // ボタンの無効化を始めた場合は、直後に表示スタックを戻す。
            if (!hasMorph) {
                ImGui::EndDisabled();
            }

            ImGui::TextDisabled("吸収演出を省略し、能力実装済みのスライムは固有技まで直接確認できます。");
        // プレイヤーの実体が無いときは操作を出さず、その理由を表示する。
        } else {
            ImGui::TextDisabled("Player not found");
        }
    }

    // プリズムの欄を開いている場合だけ、中ボスの確認と撃破操作を表示する。
    if (ImGui::CollapsingHeader(ICON_FA_GEM " クリスタルスライム Debug", ImGuiTreeNodeFlags_DefaultOpen)) {
        // 中ボスがScene内にいるときだけ、実際の撃破経路を使うテスト操作を出す。
        EnemyPrismSlime* prismBoss = FindPrismBossForHUD();

        // 対象ボスを解決できた場合に、遭遇 HUD と同じ HP 値を取得する。
        if (prismBoss) {
            const float bossHp = prismBoss->GetEncounterCurrentHp();
            const float bossMaxHp = prismBoss->GetEncounterMaximumHp();
            char bossHpLabel[64]{};
            std::snprintf(bossHpLabel, sizeof(bossHpLabel), "HP %.0f / %.0f", bossHp, bossMaxHp);
            ImGui::ProgressBar(std::clamp(bossHp / (std::max)(bossMaxHp, 1.0f), 0.0f, 1.0f), ImVec2(-1.0f, 0.0f),
                               bossHpLabel);

            // 撃破ボタンを押すと、ボス自身が持つ本編の撃破経路を実行する。
            if (ImGui::Button(ICON_FA_SKULL " 撃破演出を確認##DebugDefeatPrismSlime", ImVec2(-1.0f, 32.0f))) {
                prismBoss->TriggerDebugDefeat();
            }

            ImGui::TextDisabled("実際のHPを0にして、撃破演出からバリア解除までを通して再生します。");
        // 現行シーンからボスを解決できない場合は、操作の利用時期を表示する。
        } else {
            ImGui::TextDisabled("中ボス戦の開始後に使用できます。");
        }
    }

    // 偽王の欄を開いた場合に、登場の調整と利用可能な戦闘区間の移行を表示する。
    if (ImGui::CollapsingHeader(ICON_FA_CROWN " 偽王スライム Debug", ImGuiTreeNodeFlags_DefaultOpen)) {
        BossIntroSettings::Get().DrawEditor();
        // 各段階を飛ばす処理の可否はボス自身に問い合わせ、無効な操作を押せないようにする。
        EnemyFalseKingSlime* falseKing = FindFalseKingBossForHUD();

        // 偽王の実体がある場合だけ、HP・封印数と遷移操作を評価する。
        if (falseKing) {
            const float bossHp = falseKing->GetEncounterCurrentHp();
            const float bossMaxHp = falseKing->GetEncounterMaximumHp();
            char bossHpLabel[64]{};
            std::snprintf(bossHpLabel, sizeof(bossHpLabel), "HP %.0f / %.0f", bossHp, bossMaxHp);
            ImGui::ProgressBar(std::clamp(bossHp / (std::max)(bossMaxHp, 1.0f), 0.0f, 1.0f), ImVec2(-1.0f, 0.0f),
                               bossHpLabel);

            ImGui::Text("現在: 第%d形態 / 封印 %d / 3", falseKing->GetBattlePhase(), falseKing->GetBrokenSealCount());
            ImGui::TextDisabled("見たい区間を選ぶと、実際の形態遷移処理を通して再生します。");

            // 第一形態の終了、王冠の幕間、最終撃破を個別に確認する。
            const bool canEndPhaseOne = falseKing->CanTriggerDebugPhaseTwoTransition();
            // 第一形態から移行できない状態では、対応する操作を無効にする。
            if (!canEndPhaseOne)
                ImGui::BeginDisabled();
            // 第一形態の終了操作で、ボス自身の第二形態への遷移を要求する。
            if (ImGui::Button(ICON_FA_PLAY " 第一形態を終了（形態移行を見る）##DebugFalseKingPhase2",
                              ImVec2(-1.0f, 34.0f))) {
                falseKing->TriggerDebugPhaseTwoTransition();
            }

            // 第一形態のボタンを無効にした場合は、次の操作の前に解除する。
            if (!canEndPhaseOne)
                ImGui::EndDisabled();

            const bool canStartInterlude = falseKing->CanTriggerDebugCrownInterlude();
            // 王冠継承へ移れる状態でなければ、封印戦を終了する操作を無効にする。
            if (!canStartInterlude)
                ImGui::BeginDisabled();
            // 封印戦の終了操作で、王冠継承の幕間から脱出までの進行を要求する。
            if (ImGui::Button(ICON_FA_CROWN " 封印戦を終了（王冠継承・脱出を見る）##DebugFalseKingInterlude",
                              ImVec2(-1.0f, 34.0f))) {
                falseKing->TriggerDebugCrownInterlude();
            }

            // 封印戦のボタンだけに適用した無効化を解除する。
            if (!canStartInterlude)
                ImGui::EndDisabled();

            const bool canDefeatFinalPhase = falseKing->CanTriggerDebugFinalDefeat();
            // 最終撃破を受け付けない段階では、撃破の操作を無効にする。
            if (!canDefeatFinalPhase)
                ImGui::BeginDisabled();
            // 最終撃破の操作で、倒されたボスの本来の終了処理を要求する。
            if (ImGui::Button(ICON_FA_SKULL " 最終形態を撃破（撃破演出を見る）##DebugDefeatFalseKing",
                              ImVec2(-1.0f, 34.0f))) {
                falseKing->TriggerDebugDefeat();
            }

            // 最終撃破ボタン用の無効化を解除する。
            if (!canDefeatFinalPhase)
                ImGui::EndDisabled();
            ImGui::TextDisabled("上から順に確認できます。王冠継承・脱出は第1形態から直接再生することもできます。");
        // 偽王が未配置・未出現の場合には、操作が使える条件を表示する。
        } else {
            ImGui::TextDisabled("ステージ3のボス登場後に使用できます。");
        }
    }

    // シーンイベント欄を開いた場合に、橋落下の試し再生を表示する。
    if (ImGui::CollapsingHeader(ICON_FA_GHOST " Scene Events", ImGuiTreeNodeFlags_DefaultOpen)) {
        // 試し再生ボタンから、橋の演出を通常の開始処理で起動する。
        if (ImGui::Button(ICON_FA_PLAY " Start Bridge Drop Movie", ImVec2(-1, 30))) {
            StartBridgeDropMovie();
        }
    }

    // 保存通知の表示部品を用意済みで、欄も開いている場合にその編集を描く。
    if (saveIndicatorOverlay_ &&
        ImGui::CollapsingHeader(ICON_FA_SAVE " Save Indicator", ImGuiTreeNodeFlags_DefaultOpen)) {
        saveIndicatorOverlay_->DrawImGui();
    }

    // SceneイベントとHUDの編集面は、上の進行状態デバッグとは分けて表示する。
    DrawGoalPresentationEditor();

    ImGui::Separator();
    ImGui::TextDisabled("※この項目は GamePlayScene::DrawImGui() で編集可能です");
#endif
}

#ifdef USE_IMGUI
// デバッグ用の敵を生成し、設定を使ってプレイヤーのコピーへ切り替える。
void GamePlayScene::ApplyDebugPlayerMorph(const char* enemyType) {
    // プレイヤー、描画基盤、空でない型名のどれかが欠けている場合は処理を始めない。
    if (!player_ || !GetObject3DCommon() || !enemyType || enemyType[0] == '\0') {
        return;
    }

    // プレイヤーが倒れているか演出に操作を預けている間は、現在の変身を触らず中止します。
    if (player_->IsDead() || player_->IsCinematicLocked()) {
        DebugConsole::GetInstance()->AddLog("Debug morph skipped: player is unavailable.");
        return;
    }

    // 先にプレイヤー側の参照を解除してから、以前のデバッグ用変身元を破棄します。
    ClearDebugPlayerMorph();

    std::unique_ptr<BaseEnemy> source = EnemyFactory::GetInstance()->CreateEnemy(enemyType, GetObject3DCommon());

    // Factory が有効な敵を返さなければ能力を取得できないため、指定名をログへ出す。
    if (!source || source->GetEnemyType().empty()) {
        DebugConsole::GetInstance()->AddLog(std::string("Debug morph create failed: ") + enemyType);
        return;
    }

    // 能力設定を供給する敵はシーンに登録せず、表示と当たり判定も無効にします。
    source->SetTarget(player_);
    source->SetTranslate(player_->GetWorldPosition());
    source->SetIsVisible(false);
    source->SetCollisionAttribute(0);
    source->SetCollisionMask(0);

    // Playerへ渡す前に変身元の所有権をシーンへ移し、変身が不成立なら破棄します。
    BaseEnemy* sourcePointer = source.get();
    debugPlayerMorphSource_ = std::move(source);
    player_->DebugForceEnemyMorph(sourcePointer);

    // 対象型をコピーできなかった場合は、一時的な元個体を所有メンバーから破棄する。
    if (!player_->IsEnemyMorphed()) {
        debugPlayerMorphSource_.reset();
        DebugConsole::GetInstance()->AddLog(std::string("Debug morph start failed: ") + enemyType);
        return;
    }

    DebugConsole::GetInstance()->AddLog(std::string("Debug morph selected: ") + enemyType);
}

void GamePlayScene::ClearDebugPlayerMorph() {
    // Player本体とデバッグ用の変身元参照を同時に初期化する。
    // プレイヤーが存在する場合は、所有している元個体より先に、そのコピー側の参照を解除する。
    if (player_) {
        player_->DebugClearEnemyMorph();
    }

    debugPlayerMorphSource_.reset();
}

void GamePlayScene::RegisterDebugTeleportEditor() {
    // 選択されたSceneをその都度受け取るため、切り替え前のSceneのポインターを保持しません。
    using Point = DebugTeleportEditor::Point;
    DebugTeleportEditor::GetInstance().Configure({
        [](const BaseScene& scene) -> std::string {
            // ゲームプレイのシーン以外では、ステージ専用の登録キーを返さない。
            if (!dynamic_cast<const GamePlayScene*>(&scene)) return {};
            return "stage:" + StageManager::GetInstance()->GetCurrentStage().id;
        },
        [](BaseScene& scene) {
            auto& gameplay = static_cast<GamePlayScene&>(scene);
            std::vector<Point> points;
            const auto append = [&](const char* anchor, const engine::Vector3& offset, const char* facing, const char* label) {
                // 基準物の位置を解決できた操作だけ、移動先候補へ加える。
                if (const auto point = gameplay.ResolveDebugTeleportTarget(anchor, offset, facing, label))
                    points.push_back({point->label, point->position, point->yawRadians});
            };
            // ステージ1に限り、既知の中ボス前とゴール前を候補へ補う。
            if (StageManager::GetInstance()->GetCurrentStage().id == "stage1") {
                const char* prismAnchor = gameplay.FindDebugObjectByName("Stage1_MidbossTerrain_Checkpoint")
                    ? "Stage1_MidbossTerrain_Checkpoint" : kDebugPrismArenaAnchorName;
                append(prismAnchor, {18.0f, 0.0f, 5.0f}, kDebugPrismSlimeName, "中ボス前");
                // 現在の床と、旧Sceneに残るアンカーのどちらからでも開始地点を作ります。
                const char* goalAnchor = gameplay.FindDebugObjectByName(kDebugGoalEntryAnchorName)
                    ? kDebugGoalEntryAnchorName : "Stage1_TerrainPart_GoalApproach";
                append(goalAnchor, {}, kDebugGoalName, "ゴール前");
            }
            return points;
        },
        [](BaseScene& scene) -> std::optional<Point> {
            auto* player = static_cast<GamePlayScene&>(scene).player_;
            // 現在のプレイヤーがいなければ、座標と角度の取得に失敗したことを返す。
            if (!player) return std::nullopt;
            return Point{"", player->GetWorldPosition(), player->GetMoveYaw()};
        },
        [](BaseScene& scene, const Point& point) {
            return static_cast<GamePlayScene&>(scene).QueueDebugTeleport({point.position, point.yawRadians, point.name});
        }
    });
}

bool GamePlayScene::QueueDebugTeleport(DebugTeleportRequest request) {
    // 終了中や予約済みなら再生状態を切り替えず、同じフレームの二重予約を防ぎます。
    auto* manager = GetSceneManager();
    // 遷移中・死亡・ゴール後・最終戦演出中・予約済みでは受け付けない。必要な管理とプレイヤーが無い場合も拒否する。
    if (!manager || manager->IsTransitioning() || !player_ || player_->IsDead() || isGoal_ || crownFinale_.active || crownDefeat_.active || pendingDebugTeleport_) {
        return false;
    }

    // 停止中は通常のPlayを通し、移動前の編集状態をスナップショットへ保存します。
    if (!manager->IsPlaying() &&
        (!EditorCommandRegistry::GetInstance()->Execute(editorCommandId::kPlay) || !manager->IsPlaying())) {
        return false;
    }

    pendingDebugTeleport_ = std::move(request);
    return true;
}

void GamePlayScene::ProcessPendingDebugTeleport() {
    // 要求を消費してから演出や座標を更新し、失敗時も再実行しません。
    auto request = std::move(pendingDebugTeleport_);
    pendingDebugTeleport_.reset();
    // 予約が無い、プレイヤーが不在か死亡、またはゴール後なら予約を消費したところで終える。
    if (!request || !player_ || player_->IsDead() || isGoal_) return;

    // ステージ入口演出が開始位置に座標を戻さないよう、移動前に終了扱いにします。
    if (stageEntryPresentationActive_) FinishStageEntryPresentation();
    stageEntryPresentationPending_ = false;
    stageEntryPresentationCompleted_ = true;
    stageEntryRuntimeWasPlaying_ = true;
    stageEntryPresentationRetryTimer_ = 0.0f;

    ApplyDebugTeleport(*request);
}

engine::Object3D* GamePlayScene::FindDebugObjectByName(const char* objectName) const {
    // デバッグ操作用に現在のシーン配置から名前を解決する。
    if (!objectManager_ || !objectName || objectName[0] == '\0') {
        return nullptr;
    }

    std::vector<engine::Object3D*> objectsToVisit;
    objectsToVisit.reserve(objectManager_->GetObjects().size());

    for (const auto& object : objectManager_->GetObjects()) {
        // 有効なルートだけを探索の候補へ積む。
        if (object) {
            objectsToVisit.push_back(object.get());
        }
    }

    // ルートの配置だけでは見つからない名前も扱うため、子オブジェクトを順に探索します。
    while (!objectsToVisit.empty()) {
        engine::Object3D* object = objectsToVisit.back();
        objectsToVisit.pop_back();

        // 一致した名前が見つかった場合は、この探索順で最初の実体を返す。
        if (object->GetName() == objectName) {
            return object;
        }

        for (engine::Object3D* child : object->GetChildren()) {
            // 未探索の子が存在すれば、ワールドの基準名を探す候補に加える。
            if (child) {
                objectsToVisit.push_back(child);
            }
        }
    }

    return nullptr;
}

std::optional<GamePlayScene::DebugTeleportRequest> GamePlayScene::ResolveDebugTeleportTarget(
    const char* anchorName, const engine::Vector3& anchorOffset, const char* facingObjectName, const char* label) {
    // 階層の行列を更新し、従来のアンカー上面とオフセットから着地点を決めます。
    auto* anchor = FindDebugObjectByName(anchorName);
    // 着地点の基準物が無い場合は、代替地点を作らない。
    if (!anchor) return std::nullopt;
    auto* root = anchor;
    while (root->GetParent()) root = root->GetParent();
    root->UpdateWorldMatrix(false);

    auto position = anchor->GetWorldPosition() + anchorOffset;
    const auto bounds = anchor->GetAabb();
    // 境界の高さが正の場合は、配置位置の Y より上面と足元の余白を使う。
    if (bounds.max.y > bounds.min.y) position.y = bounds.max.y + kDebugTeleportPlayerFloorClearance;

    // 向き対象が欠けていれば、旧ボタンと同じワールド+Xを使います。
    float yaw = std::numbers::pi_v<float> * 0.5f;
    // 方向を合わせる対象が見つかった場合だけ、既定角の更新を試みる。
    if (auto* facing = FindDebugObjectByName(facingObjectName)) {
        const auto direction = facing->GetWorldPosition() - position;
        // 水平距離が十分ある場合に、ワールドの向きからヨー角を確定する。
        if (std::hypot(direction.x, direction.z) > 0.001f) yaw = std::atan2(direction.x, direction.z);
    }
    return DebugTeleportRequest{position, yaw, label};
}

void GamePlayScene::ApplyDebugTeleport(const DebugTeleportRequest& request) {
    // デバッグ移動では進行中の移動演出も明示的に止め、次フレームの座標上書きを防ぎます。
    if (arenaBossIntroActive_) {
        FinishArenaBossIntro();
    }

    // プレイヤー内部の各コントローラーを一括解除し、次フレームの座標上書きを防ぎます。
    player_->DebugPrepareForTeleport();
    debugPlayerMorphSource_.reset();
    player_->SetVelocity({0.0f, 0.0f, 0.0f});
    player_->SetGrounded(false);
    player_->SetTranslate(request.position);
    player_->SetRespawnPosition(request.position);
    player_->SetMoveYaw(request.yawRadians);
    player_->SetIsVisible(true);
    player_->SetIsControlActive(true);
    player_->UpdateLocalMatrix();
    player_->UpdateWorldMatrix();

    // 旧ロックオンとカメラ演出の重ね掛けを解除し、新しい位置を通常の三人称視点で表示します。
    engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();

    // ロックオン管理がある場合は、カメラを移動させる前に既存の標的を解除する。
    if (lockOnSystem_) {
        lockOnSystem_->ResetLockOn(camera);
    }

    player_->SetLockOn(false);

    // メインカメラが使える場合は、移動先で三人称の姿勢・追従・入力状態を設定する。
    if (camera) {
        CameraEditor::GetInstance()->SetMode(CameraEditor::Mode::kGame);
        camera->EndOverride(0.0f);
        camera->ClearPresentationLayers();
        CameraManager::GetInstance()->SetActiveCamera(camera);
        camera->SetInputEnabled(true);
        camera->SetFollowTarget(player_);
        camera->SetFollowMode(engine::Camera::FollowMode::kAimable);
        camera->SetRotation({kDebugTeleportCameraPitch, request.yawRadians, 0.0f});
        camera->SnapToThirdPerson(kDebugTeleportCameraDistance, kDebugTeleportCameraHeight, kDebugTeleportCameraPitch);
        camera->Update(0.0f);
    }

    DebugConsole::GetInstance()->AddLog("デバッグ移動: " + request.label);
}
#endif
