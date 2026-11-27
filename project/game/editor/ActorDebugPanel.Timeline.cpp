#ifdef USE_IMGUI
#include <cstdint>
#include "game/editor/ActorDebugPanel.h"
#include "game/editor/CopyAbilityPreviewRegistry.h"
#include "game/editor/ActorDebugPanel.Constants.h"
#include "game/crown/CrownFinaleTuning.h"
#include "game/scene/gameplay/GamePlayScene.h"

#include "BaseEnemy.h"
#include "BaseScene.h"
#include "game/actor/enemy/boss/prism/EnemyPrismSlime.h"
#include "Object3D.h"
#include "Player.h"
#include "SceneManager.h"

#ifdef USE_IMGUI
#include "imgui.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>

using enemyAttackPreviewConstants::kFixedPreviewStep;

namespace {
#ifdef USE_IMGUI
ImU32 GetPhaseColor(const std::string& phase, std::int32_t alpha = 230) {
    // 攻撃フェーズごとに帯の色を固定し、タイムライン内の境界を見分けやすくする。
    if (phase == "予兆")
        return IM_COL32(255, 158, 52, alpha);
    // 攻撃判定が出る区間は、予兆より赤みの強い色で示す。
    if (phase == "攻撃")
        return IM_COL32(255, 74, 28, alpha);
    // 攻撃後の硬直は、発動中の赤から落ち着いた色へ戻す。
    if (phase == "硬直")
        return IM_COL32(212, 126, 92, alpha);
    // 単独の溜めは、待機中の配色と区別して明るい橙色にする。
    if (phase == "溜め")
        return IM_COL32(255, 188, 64, alpha);
    // 放電と連続落雷の溜めは、同じ電撃系の準備として黄色を使う。
    if (phase == "近距離放電・溜め" || phase == "連続落雷・溜め")
        return IM_COL32(247, 210, 73, alpha);
    // 連続落雷の発生中は、準備の黄色と区別して水色にする。
    if (phase == "連続落雷・落雷中")
        return IM_COL32(103, 218, 255, alpha);
    // 三連風弾は、上昇と頭上での形成を共通の準備色で示す。
    if (phase == "三連風弾・風で上昇" || phase == "三連風弾・頭上に形成")
        return IM_COL32(128, 245, 211, alpha);
    // 三連風弾を投射している段階は、形成中と区別して青色にする。
    if (phase == "三連風弾・順番に投射")
        return IM_COL32(105, 218, 255, alpha);
    // 三連風弾の着地は、投射後の復帰を示す淡い緑にする。
    if (phase == "三連風弾・着地")
        return IM_COL32(164, 226, 207, alpha);
    // 放電直後の反発は、放電本体とは別の紫色で示す。
    if (phase == "放電後・反発")
        return IM_COL32(167, 126, 255, alpha);
    // 再使用を待つ時間は、攻撃の色より暗い青にする。
    if (phase == "再使用待ち")
        return IM_COL32(84, 126, 158, alpha);
    // 突進の前に上がる区間を、水色で示す。
    if (phase == "上昇")
        return IM_COL32(112, 205, 255, alpha);
    // 水平追尾を含む突進の区間は、上昇と区別して赤色にする。
    if (phase == "突進")
        return IM_COL32(255, 92, 116, alpha);
    // 着地後に跳ね返る区間は、空中の動きと分けて紫色で示す。
    if (phase == "着地反発")
        return IM_COL32(171, 116, 255, alpha);
    // 攻撃に入っていない待機と移動は共通の緑色にする。
    if (phase == "待機・移動")
        return IM_COL32(104, 190, 153, alpha);
    // 空中で拘束されて落下する区間を、接地前の青色で示す。
    if (phase == "空中拘束・落下")
        return IM_COL32(82, 172, 235, alpha);
    // 拘束後に接地して受け止める区間は、落下と区別して黄色にする。
    if (phase == "着地・受け止め")
        return IM_COL32(242, 185, 69, alpha);
    // 接地後に抵抗している間は、引きずられる前の緑色で示す。
    if (phase == "踏ん張り・抵抗")
        return IM_COL32(83, 188, 139, alpha);
    // 引きずられている区間は、抵抗と分けて橙色で示す。
    if (phase == "引きずられ")
        return IM_COL32(241, 132, 66, alpha);
    // 分裂が近い区間は、分裂後の紫に変わる前の赤色で示す。
    if (phase == "限界・分裂寸前")
        return IM_COL32(238, 76, 118, alpha);
    if (phase == "分裂完了" || phase == "分裂後")
        return IM_COL32(187, 105, 244, alpha);
    // 個別に色を指定していないフェーズには共通の青灰色を使う。
    return IM_COL32(128, 164, 196, alpha);
}

// 攻撃フェーズの開始・終了時刻を時間軸上の色付き矩形に変換します。
void DrawTimelineBlock(
    ImDrawList* drawList,
    const ImVec2& origin,
    float labelWidth,
    float pixelsPerSecond,
    float laneY,
    float laneHeight,
    float beginTime,
    float endTime,
    ImU32 color,
    const char* label) {
    // 描画先がない、または秒で指定された区間に長さがないときは帯を描かない。
    if (!drawList || endTime <= beginTime) {
        return;
    }

    // チャンネル名の幅を避け、時刻を秒から画面のX座標へ変換する。
    const float x0 = origin.x + labelWidth + beginTime * pixelsPerSecond;
    const float x1 = origin.x + labelWidth + endTime * pixelsPerSecond;
    drawList->AddRectFilled(
        ImVec2(x0 + 1.0f, laneY + 3.0f),
        ImVec2(x1 - 1.0f, laneY + laneHeight - 3.0f),
        color,
        3.0f);
    // 帯が狭い場合は名前を省き、表示する場合も帯の外へはみ出さないようにする。
    if (label && x1 - x0 >= 48.0f) {
        drawList->PushClipRect(
            ImVec2(x0 + 3.0f, laneY),
            ImVec2(x1 - 3.0f, laneY + laneHeight),
            true);
        drawList->AddText(
            ImVec2(x0 + 6.0f, laneY + 6.0f),
            IM_COL32(245, 248, 252, 255),
            label);
        drawList->PopClipRect();
    }
}
#endif
}

void ActorDebugPanel::DrawTimelineWindow() {
    // 設定上の攻撃時間と実際の AI フェーズを同じ時間軸に重ねて比較します。
#ifdef USE_IMGUI
    // 王冠ムービーでは専用トラックを使い、通常の敵攻撃トラックとは表示を切り替えます。
    if (crownMovieMode_) {
        DrawCrownMovieTimeline();
        return;
    }

    constexpr const char* kWindowName = "攻撃・能力プレビュー - Timeline";
    const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove;

    // 子ウィンドウの内容を描けない場合も、BeginChildに対応するEndChildを呼ぶ。
    if (!ImGui::BeginChild(kWindowName, ImVec2(0, 420), true, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::EndChild();
        return;
    }

    // 配置先のシーンが準備できていなければ、案内だけ描いて編集操作を止める。
    if (!IsCurrentSceneReady()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.28f, 1.0f), "Object3dを配置できるSceneが必要です。");
        ImGui::EndChild();
        return;
    }

    // ゲーム再生中は攻撃や能力の編集プレビューを操作できない。
    if (sceneManager_->IsPlaying()) {
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.20f, 1.0f), "編集モードへ戻すと敵攻撃タイムラインを使用できます。");
        ImGui::EndChild();
        return;
    }

    // まずトラックを描き、その下に現在のモードに応じた操作を置く。
    DrawTrackTimeline();

    // 部品配置中は専用の操作だけを描き、攻撃全体を操作するツールバーまで進まない。
    if (placementPreview_) {
        ImGui::TextWrapped("配置モード: 選択部品を単独表示しています。攻撃全体の再生はInspectorの「保存せず動作確認」を使います。");
        DrawPlacementControls();
        ImGui::EndChild();
        return;
    }

    // プレビュー本体が無効なら、起動操作だけを出す。
    if (!enabled_) {
        ImGui::TextDisabled("このパネルのボタンからプレビューを有効にしてください。");
        ImGui::SameLine();

        // 開始が押されたときだけ有効化とプレビュー用オブジェクトの生成を行う。
        if (ImGui::Button("プレビューを開始")) {
            enabled_ = true;
            CreatePreviewObjects();
        }

        ImGui::EndChild();
        return;
    }

    // タイムラインが入力を受けるときだけ Space と矢印キーをシークへ割り当てます。
    const bool timelineFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    if (timelineFocused && !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive()) {
        // Spaceの押下で再生と一時停止を切り替える。
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            playing_ = !playing_;
        }

        // 矢印は通常1ステップ、Shift併用では10ステップ分を固定刻みの秒数としてシークする。
        const std::int32_t stepFrames = ImGui::GetIO().KeyShift ? 10 : 1;

        // 左矢印では再生を止め、固定刻みで前の時刻へ移動する。
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) {
            playing_ = false;
            SeekPreview(elapsedTime_ - kFixedPreviewStep * static_cast<float>(stepFrames));
        }

        // 右矢印では再生を止め、固定刻みで後の時刻へ移動する。
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) {
            playing_ = false;
            SeekPreview(elapsedTime_ + kFixedPreviewStep * static_cast<float>(stepFrames));
        }

        // Homeが押されたときはRestartPreviewで先頭から初期化する。
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
            RestartPreview();
        }
    }

    // キーボード操作と同じ SeekPreview をボタンでも使い、再生位置の計算を揃えます。
    ImGui::PushID("EnemyAttackTimelineToolbar");
    // コピー能力の確認中は、敵名の代わりに能力とアクションの登録名を表示する。
    if (playerCopyMode_) {
        const auto* copy = GetCopyDescriptor();
        const auto* action = GetCopyAction();
        ImGui::Text("プレイヤー / %s / %s", copy ? copy->displayName.c_str() : "未登録",
            action ? action->displayName.c_str() : "未選択");
    }
    // 現在の状態に対応するボタンを押したときだけ、再生状態を反転する。
    if (ImGui::Button(playing_ ? "■ 一時停止" : "▶ 再生", ImVec2(108.0f, 0.0f))) {
        playing_ = !playing_;
    }

    ImGui::SameLine();
    // 先頭ボタンはHomeと同じ再開処理を使う。
    if (ImGui::Button("|<"))
        RestartPreview();
    ImGui::SameLine();

    // フレーム移動ボタンは、押された方向・幅だけ進めて再生を止める。
    if (ImGui::Button("-10F")) {
        playing_ = false;
        SeekPreview(elapsedTime_ - kFixedPreviewStep * 10.0f);
    }

    ImGui::SameLine();

    // 1フレーム戻るボタンでは固定刻みを一つ引く。
    if (ImGui::Button("-1F")) {
        playing_ = false;
        SeekPreview(elapsedTime_ - kFixedPreviewStep);
    }

    ImGui::SameLine();

    // 1フレーム進むボタンでは固定刻みを一つ足す。
    if (ImGui::Button("+1F")) {
        playing_ = false;
        SeekPreview(elapsedTime_ + kFixedPreviewStep);
    }

    ImGui::SameLine();

    // 10フレーム進むボタンでは固定刻みを十個足す。
    if (ImGui::Button("+10F")) {
        playing_ = false;
        SeekPreview(elapsedTime_ + kFixedPreviewStep * 10.0f);
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(96.0f);
    ImGui::SliderFloat("速度", &playbackSpeed_, 0.05f, 2.0f, "%.2fx");
    ImGui::SameLine();
    ImGui::Checkbox("ループ", &loop_);
    ImGui::SameLine();

    // カメラ初期化ボタンは表示されるが、押下時の処理はここにはない。
    if (ImGui::Button("カメラを初期位置へ")) {
    }

    ImGui::PopID();

    // 集計欄は最後に取得した実測 Sample を表示します。設定値と混同しないよう別列にします。
    const TimelineSample* currentSample = timelineSamples_.empty() ? nullptr : &timelineSamples_.back();

    // 表を開けた場合だけ、時刻・状態名・速度・高さを列ごとに描く。
    if (ImGui::BeginTable("EnemyAttackTimelineSummary", 4, ImGuiTableFlags_SizingStretchSame)) {
        const char* labels[] = { "時間 / Frame", playerCopyMode_ ? "入力フェーズ" : "AIフェーズ", "速度", "接地からの高さ" };
        char values[4][64]{};
        sprintf_s(values[0], "%.3f / %.2f 秒  |  %dF", elapsedTime_, loopDuration_, static_cast<std::int32_t>(std::round(elapsedTime_ / kFixedPreviewStep)));

        // 状態名は日本語や実行件数を含み64バイトを超えます。固定長バッファへコピーしません。
        sprintf_s(values[2], "%.2f m/s", currentSample ? currentSample->speed : 0.0f);
        sprintf_s(values[3], "%.2f m", currentSample ? currentSample->height : 0.0f);

        for (std::int32_t column = 0; column < 4; ++column) {
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", labels[column]);

            // 状態名の列だけは数値用バッファを使わず、長い名前を折り返して表示する。
            if (column == 1) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(GetCurrentPhaseName());
                ImGui::PopTextWrapPos();
            // 他の列は事前に整形した実測値を表示する。
            } else ImGui::TextUnformatted(values[column]);
        }

        ImGui::EndTable();
    }

    // プリズムの詠唱専用表示は、レイヤー数と負値で返る生成状態を分けて知らせます。
    if (const auto* prismSlime = dynamic_cast<const EnemyPrismSlime*>(previewEnemy_)) {
        const std::int32_t chantLayers = prismSlime->GetDebugChantVisibleLayerCount();

        // -2はまだ詠唱の描画待ちなので、エラーの表示を出さない。
        if (chantLayers == -2) {
            ImGui::TextDisabled("詠唱エフェクト: 待機中");
        // 待機以外の負値は生成失敗を表す。
        } else if (chantLayers < 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.28f, 1.0f),
                "詠唱エフェクト: 生成失敗（Debug Console を確認）");
        // 0以上は正常に取得した表示対象レイヤーの数を表す。
        } else {
            ImGui::Text("詠唱エフェクト: 表示対象 %d / 7 レイヤー", chantLayers);
        }
    }

    ImGui::SetNextItemWidth(170.0f);
    ImGui::SliderFloat("タイムライン拡大", &timelinePixelsPerSecond_, 60.0f, 280.0f, "%.0f px/s");
    ImGui::SameLine();
    ImGui::Checkbox("再生位置を追従", &timelineFollowPlayhead_);
    ImGui::SameLine();
    ImGui::TextDisabled("Space: 再生/停止  ←/→: 1F  Shift+←/→: 10F  Home: 先頭");

    // 左側のラベル幅と右側の秒数スケールから、横スクロール可能な Canvas を作ります。
    constexpr float kLabelWidth = 118.0f;
    constexpr float kHeaderHeight = 28.0f;
    constexpr float kLaneHeight = 30.0f;
    constexpr std::int32_t kLaneCount = 10;
    constexpr float kBottomPadding = 8.0f;
    const float timelineHeight = kHeaderHeight + kLaneHeight * static_cast<float>(kLaneCount) + kBottomPadding;
    const float duration = (std::max)(loopDuration_, kFixedPreviewStep);
    const float visibleWidth = (std::max)(ImGui::GetContentRegionAvail().x, 320.0f);
    const float contentWidth = (std::max)(visibleWidth, kLabelWidth + duration * timelinePixelsPerSecond_ + 32.0f);

    ImGui::BeginChild(
        "##EnemyAttackTimelineScroll",
        ImVec2(0.0f, timelineHeight + ImGui::GetStyle().ScrollbarSize + 6.0f),
        true,
        ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##EnemyAttackTimelineCanvas", ImVec2(contentWidth, timelineHeight));
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 canvasEnd(origin.x + contentWidth, origin.y + timelineHeight);

    drawList->AddRectFilled(origin, canvasEnd, IM_COL32(22, 25, 31, 255), 4.0f);
    drawList->AddRectFilled(origin, ImVec2(origin.x + kLabelWidth, canvasEnd.y), IM_COL32(34, 38, 47, 255), 4.0f);
    drawList->AddLine(
        ImVec2(origin.x + kLabelWidth, origin.y),
        ImVec2(origin.x + kLabelWidth, canvasEnd.y),
        IM_COL32(105, 115, 135, 255));

    // 機能に応じて一段目の名称だけ差し替え、以後の実測レーン配置は共有します。
    const bool giantHookPreview = IsGiantHookSplitPreview();
    const char* laneNames[kLaneCount] = {
        giantHookPreview ? "専用モーション" : "設定タイミング",
        "実測AIフェーズ",
        "移動速度",
        "ジャンプ高さ",
        "変形量",
        "被弾リアクション",
        "GPU Particle系統",
        "通常Particle数",
        "Mesh Effect数",
        "Debris数"
    };

    for (std::int32_t lane = 0; lane < kLaneCount; ++lane) {
        const float y0 = origin.y + kHeaderHeight + kLaneHeight * static_cast<float>(lane);
        const float y1 = y0 + kLaneHeight;

        // 偶数行だけ背景を少し明るくして隣同士を見分ける。
        if ((lane % 2) == 0) {
            drawList->AddRectFilled(ImVec2(origin.x, y0), ImVec2(canvasEnd.x, y1), IM_COL32(255, 255, 255, 6));
        }

        drawList->AddLine(ImVec2(origin.x, y1), ImVec2(canvasEnd.x, y1), IM_COL32(62, 68, 80, 190));
        drawList->AddText(ImVec2(origin.x + 8.0f, y0 + 7.0f), IM_COL32(205, 213, 226, 255), laneNames[lane]);
    }

    // 拡大率に応じて目盛りを細かくし、一秒境界のみ時刻ラベルを表示します。
    float tickStep = 1.0f;
    if (timelinePixelsPerSecond_ >= 220.0f)
        tickStep = 0.25f;
    // 中間の拡大率は0.5秒間隔とし、それ未満は既定の1秒間隔を使う。
    else if (timelinePixelsPerSecond_ >= 110.0f) tickStep = 0.5f;
    const std::int32_t tickCount = static_cast<std::int32_t>(std::ceil(duration / tickStep));

    for (std::int32_t tick = 0; tick <= tickCount; ++tick) {
        const float time = static_cast<float>(tick) * tickStep;
        const float x = origin.x + kLabelWidth + time * timelinePixelsPerSecond_;
        const bool major = std::fmod(time + 0.001f, 1.0f) < 0.01f;
        drawList->AddLine(
            ImVec2(x, origin.y + (major ? 0.0f : 14.0f)),
            ImVec2(x, canvasEnd.y),
            major ? IM_COL32(80, 88, 105, 220) : IM_COL32(60, 66, 78, 130));
        // 秒の境界の線だけラベルを添えて、補助目盛りと区別する。
        if (major) {
            char label[24]{};
            sprintf_s(label, "%.0fs", time);
            drawList->AddText(ImVec2(x + 4.0f, origin.y + 5.0f), IM_COL32(185, 194, 210, 255), label);
        }
    }

    // 設定レーンには共通命令、巨大フック専用演出、通常の攻撃フェーズを排他的に描きます。
    const EnemyAttackDefinition* sequenceAttack = GetSelectedAttackDefinition();

    // 選択攻撃に共通命令が設定されている場合は、通常の攻撃時間より先に描く。
    if (sequenceAttack && sequenceAttack->sequence.enabled) {
        const auto& sequence = sequenceAttack->sequence;
        const float laneY = origin.y + kHeaderHeight;

        // 設定イベントはランタイム開始からの相対時刻。Prismは詠唱完了が原点で、AI待機は含みません。
        DrawTimelineBlock(drawList, origin, kLabelWidth, timelinePixelsPerSecond_, laneY, kLaneHeight,
            0, (std::min)(duration, sequence.duration), IM_COL32(50, 100, 125, 160), "共通命令 (命令開始=0)");
        // 共通命令がアクター制御を持つ場合は、予備動作と後隙を別の帯で示す。
        if (sequence.actorControl.enabled) {
            DrawTimelineBlock(drawList, origin, kLabelWidth, timelinePixelsPerSecond_, laneY, kLaneHeight,
                0, sequence.actorControl.telegraphEnd, IM_COL32(170, 140, 65, 230), "予備動作");
            DrawTimelineBlock(drawList, origin, kLabelWidth, timelinePixelsPerSecond_, laneY, kLaneHeight,
                sequence.actorControl.telegraphEnd, sequence.duration, IM_COL32(100, 120, 170, 230), "発射後・後隙");
        }

        for (const auto& event : sequence.events) {
            const float x = origin.x + kLabelWidth + event.time * timelinePixelsPerSecond_;
            drawList->AddLine(ImVec2(x, laneY), ImVec2(x, laneY + kLaneHeight), IM_COL32(255, 200, 60, 255), 2);

            // イベントの線付近を指した場合だけ、実行条件と不成立時の扱いを表示する。
            if (ImGui::IsMouseHoveringRect(ImVec2(x - 5, laneY), ImVec2(x + 5, laneY + kLaneHeight))) {
                ImGui::BeginTooltip();
                ImGui::Text("%.3f秒: %s / %s", event.time, event.operation.c_str(), event.condition.c_str());
                ImGui::TextUnformatted(event.stopOnFalse ? "条件不成立: 後続命令も中止" : "条件不成立: スキップ");
                ImGui::EndTooltip();
            }
        }
    // 共通命令が無い巨大フックのプレビューでは、拘束から分裂までの流れを専用の帯で示す。
    } else if (giantHookPreview) {
        const float laneY = origin.y + kHeaderHeight;
        DrawTimelineBlock(
            drawList,
            origin,
            kLabelWidth,
            timelinePixelsPerSecond_,
            laneY,
            kLaneHeight,
            0.0f,
            duration,
            IM_COL32(70, 105, 132, 190),
            IsAirHookSplitPreview()
                ? "空中拘束 → 着地 → 抵抗 → 分裂"
                : "踏ん張り → 引きずられ → 分裂");
    // 専用表示に該当せず攻撃定義を取得できた場合は、設定時間を順に積み上げて描く。
    } else if (const EnemyAttackDefinition* attack = GetSelectedAttackDefinition()) {
        const float laneY = origin.y + kHeaderHeight;
        float cursorTime = 0.0f;
        const float windupEnd = (std::min)(duration, cursorTime + attack->windupDuration);
        DrawTimelineBlock(drawList, origin, kLabelWidth, timelinePixelsPerSecond_, laneY, kLaneHeight, cursorTime, windupEnd, IM_COL32(214, 155, 51, 210), "予備動作");
        cursorTime = windupEnd;
        const float activeEnd = (std::min)(duration, cursorTime + attack->activeDuration);
        DrawTimelineBlock(drawList, origin, kLabelWidth, timelinePixelsPerSecond_, laneY, kLaneHeight, cursorTime, activeEnd, IM_COL32(223, 72, 92, 220), "攻撃");
        cursorTime = activeEnd;
        const float recoveryEnd = (std::min)(duration, cursorTime + attack->recoveryDuration);
        DrawTimelineBlock(drawList, origin, kLabelWidth, timelinePixelsPerSecond_, laneY, kLaneHeight, cursorTime, recoveryEnd, IM_COL32(126, 91, 205, 215), "硬直");
        cursorTime = recoveryEnd;
        const float cooldownEnd = (std::min)(duration, cursorTime + attack->cooldown);
        DrawTimelineBlock(drawList, origin, kLabelWidth, timelinePixelsPerSecond_, laneY, kLaneHeight, cursorTime, cooldownEnd, IM_COL32(75, 120, 145, 190), "再使用待ち");
    }

    // 実測レーンは同じ AI フェーズの連続区間を一つのブロックにまとめます。
    if (!timelineSamples_.empty()) {
        const float phaseLaneY = origin.y + kHeaderHeight + kLaneHeight;
        std::size_t segmentBegin = 0;

        for (std::size_t index = 1; index <= timelineSamples_.size(); ++index) {
            const bool segmentEnds = index == timelineSamples_.size() || timelineSamples_[index].phase != timelineSamples_[segmentBegin].phase;

            // 実測フェーズが継続している間は帯を確定せず、次のサンプルを確認する。
            if (!segmentEnds) {
                continue;
            }

            const float startTime = std::clamp(timelineSamples_[segmentBegin].time, 0.0f, duration);
            const float rawEndTime = index < timelineSamples_.size()
                ? timelineSamples_[index].time
                : (std::max)(elapsedTime_, startTime + kFixedPreviewStep);
            const float endTime = std::clamp(rawEndTime, 0.0f, duration);
            DrawTimelineBlock(
                drawList,
                origin,
                kLabelWidth,
                timelinePixelsPerSecond_,
                phaseLaneY,
                kLaneHeight,
                startTime,
                endTime,
                GetPhaseColor(timelineSamples_[segmentBegin].phase),
                timelineSamples_[segmentBegin].phase.c_str());
            segmentBegin = index;
        }

        // グラフは取得済み Sample の最大値で正規化し、設定上の最大速度も縦軸へ含めます。
        float maxSpeed = 1.0f;
        float maxHeight = 1.0f;
        float maxDeformation = 0.25f;
        float maxGpuParticleSystems = 1.0f;
        float maxCpuParticles = 1.0f;
        float maxMeshEffects = 1.0f;
        float maxDebrisPieces = 1.0f;

        for (const TimelineSample& sample : timelineSamples_) {
            maxSpeed = (std::max)(maxSpeed, sample.speed);
            maxHeight = (std::max)(maxHeight, sample.height);
            maxDeformation = (std::max)(maxDeformation, sample.deformation);
            maxGpuParticleSystems = (std::max)(maxGpuParticleSystems, sample.gpuParticleSystems);
            maxCpuParticles = (std::max)(maxCpuParticles, sample.cpuParticles);
            maxMeshEffects = (std::max)(maxMeshEffects, sample.meshEffects);
            maxDebrisPieces = (std::max)(maxDebrisPieces, sample.debrisPieces);
        }

        // 攻撃定義があるときは設定上の最大速度も含めて縦軸の上端を決める。
        if (const EnemyAttackDefinition* attack = GetSelectedAttackDefinition()) {
            maxSpeed = (std::max)(maxSpeed, attack->maxSpeed);
        }

        // 各値を時刻に沿う折れ線として描き、粒子・破片の系統数も同じ関数で表示します。
        const auto drawGraph = [&](std::int32_t lane, float TimelineSample::* valueMember, float maxValue, ImU32 color) {
            const float laneTop = origin.y + kHeaderHeight + kLaneHeight * static_cast<float>(lane) + 4.0f;
            const float laneBottom = laneTop + kLaneHeight - 8.0f;

            for (std::size_t index = 1; index < timelineSamples_.size(); ++index) {
                const TimelineSample& previous = timelineSamples_[index - 1];
                const TimelineSample& current = timelineSamples_[index];
                const float x0 = origin.x + kLabelWidth + std::clamp(previous.time, 0.0f, duration) * timelinePixelsPerSecond_;
                const float x1 = origin.x + kLabelWidth + std::clamp(current.time, 0.0f, duration) * timelinePixelsPerSecond_;
                const float y0 = laneBottom - std::clamp((previous.*valueMember) / maxValue, 0.0f, 1.0f) * (laneBottom - laneTop);
                const float y1 = laneBottom - std::clamp((current.*valueMember) / maxValue, 0.0f, 1.0f) * (laneBottom - laneTop);
                drawList->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), color, 2.0f);
            }
        };

        drawGraph(2, &TimelineSample::speed, maxSpeed, IM_COL32(75, 210, 255, 255));
        drawGraph(3, &TimelineSample::height, maxHeight, IM_COL32(255, 196, 72, 255));
        drawGraph(4, &TimelineSample::deformation, maxDeformation, IM_COL32(241, 121, 196, 255));
        drawGraph(5, &TimelineSample::hitReaction, 1.0f, IM_COL32(255, 92, 92, 255));
        drawGraph(6, &TimelineSample::gpuParticleSystems, maxGpuParticleSystems, IM_COL32(255, 224, 74, 255));
        drawGraph(7, &TimelineSample::cpuParticles, maxCpuParticles, IM_COL32(90, 226, 255, 255));
        drawGraph(8, &TimelineSample::meshEffects, maxMeshEffects, IM_COL32(255, 139, 72, 255));
        drawGraph(9, &TimelineSample::debrisPieces, maxDebrisPieces, IM_COL32(190, 157, 112, 255));

        char scaleText[64]{};
        sprintf_s(scaleText, "max %.1f", maxSpeed);
        drawList->AddText(
            ImVec2(origin.x + kLabelWidth - 55.0f, origin.y + kHeaderHeight + kLaneHeight * 2.0f + 7.0f),
            IM_COL32(120, 194, 220, 255),
            scaleText);
        sprintf_s(scaleText, "max %.1f", maxHeight);
        drawList->AddText(
            ImVec2(origin.x + kLabelWidth - 55.0f, origin.y + kHeaderHeight + kLaneHeight * 3.0f + 7.0f),
            IM_COL32(224, 181, 102, 255),
            scaleText);
        sprintf_s(scaleText, "max %.2f", maxDeformation);
        drawList->AddText(
            ImVec2(origin.x + kLabelWidth - 62.0f, origin.y + kHeaderHeight + kLaneHeight * 4.0f + 7.0f),
            IM_COL32(224, 137, 201, 255),
            scaleText);
        const auto drawCountScale = [&](std::int32_t lane, float maxValue, ImU32 color) {
            sprintf_s(scaleText, "max %.0f", maxValue);
            drawList->AddText(
                ImVec2(origin.x + kLabelWidth - 55.0f, origin.y + kHeaderHeight + kLaneHeight * static_cast<float>(lane) + 7.0f),
                color,
                scaleText);
        };

        drawCountScale(6, maxGpuParticleSystems, IM_COL32(226, 205, 98, 255));
        drawCountScale(7, maxCpuParticles, IM_COL32(112, 205, 224, 255));
        drawCountScale(8, maxMeshEffects, IM_COL32(224, 153, 112, 255));
        drawCountScale(9, maxDebrisPieces, IM_COL32(190, 166, 130, 255));
    }

    // 現在位置は全レーンを縦断する線で示し、Canvas 上のクリックやドラッグでシークします。
    // 表示ヘッドは記録から選んだ標本の時刻ではなく、現在の再生時刻そのものに置きます。
    const float playheadTime = std::clamp(elapsedTime_, 0.0f, duration);
    const float playheadX = origin.x + kLabelWidth + playheadTime * timelinePixelsPerSecond_;
    drawList->AddLine(ImVec2(playheadX, origin.y), ImVec2(playheadX, canvasEnd.y), IM_COL32(255, 82, 82, 255), 2.0f);
    drawList->AddTriangleFilled(
        ImVec2(playheadX - 6.0f, origin.y),
        ImVec2(playheadX + 6.0f, origin.y),
        ImVec2(playheadX, origin.y + 9.0f),
        IM_COL32(255, 82, 82, 255));

    // タイムラインの描画領域上にポインターがある間だけ、時間軸の操作を受け付ける。
    if (ImGui::IsItemHovered()) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;

        // 左のラベル欄を除き、時間軸上の位置だけを時刻に変換する。
        if (mouse.x >= origin.x + kLabelWidth) {
            const float hoverTime = std::clamp(
                (mouse.x - origin.x - kLabelWidth) / timelinePixelsPerSecond_,
                0.0f,
                duration);
            ImGui::BeginTooltip();
            ImGui::Text("%.3f 秒  |  %dF", hoverTime, static_cast<std::int32_t>(std::round(hoverTime / kFixedPreviewStep)));

            // 記録済みの値がある場合は、マウス時刻に最も近いサンプルの実測値を表示する。
            if (!timelineSamples_.empty()) {
                const auto nearest = (std::min_element)(
                    timelineSamples_.begin(),
                    timelineSamples_.end(),
                    [hoverTime](const TimelineSample& lhs, const TimelineSample& rhs) {
                        return std::abs(lhs.time - hoverTime) < std::abs(rhs.time - hoverTime);
                    });
                ImGui::Text("フェーズ: %s", nearest->phase.c_str());
                ImGui::Text("速度 %.2f m/s  |  高さ %.2f m", nearest->speed, nearest->height);
                ImGui::Text("変形 %.2f  |  被弾 %.2f", nearest->deformation, nearest->hitReaction);
                ImGui::Text("GPU系統 %.0f  |  通常粒子 %.0f", nearest->gpuParticleSystems, nearest->cpuParticles);
                ImGui::Text("Mesh Effect %.0f  |  Debris %.0f", nearest->meshEffects, nearest->debrisPieces);
            }

            ImGui::EndTooltip();

            // 手動シーク中に再生や自動スクロールで位置が動かないよう、両方を止めます。
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                timelineFollowPlayhead_ = false;
                playing_ = false;
                SeekPreview(hoverTime);
            }
        }

        // 通常のホイール操作と区別し、Ctrlを押したときだけ秒あたりの横幅を調整します。
        if (ImGui::GetIO().KeyCtrl && std::abs(ImGui::GetIO().MouseWheel) > 0.0f) {
            timelinePixelsPerSecond_ = std::clamp(
                timelinePixelsPerSecond_ + ImGui::GetIO().MouseWheel * 18.0f,
                60.0f,
                280.0f);
        }
    }

    // 自動追従は再生中に限り、右側へ進む余裕を残して横スクロールさせます。
    if (playing_ && timelineFollowPlayhead_) {
        const float playheadLocalX = kLabelWidth + playheadTime * timelinePixelsPerSecond_;
        const float desiredScroll = (std::max)(0.0f, playheadLocalX - visibleWidth * 0.75f);
        ImGui::SetScrollX((std::min)(desiredScroll, ImGui::GetScrollMaxX()));
    }

    ImGui::EndChild();

    ImGui::EndChild();
#endif
}

void ActorDebugPanel::DrawCrownMovieTimeline() {
#ifdef USE_IMGUI
    // 同じ下段ドックを使い、InspectorとGame Viewを残して確認します。
    // 子ウィンドウの内容が見えない場合も、BeginChildに対応するEndChildを呼ぶ。
    if (!ImGui::BeginChild("演出の時間軸", ImVec2(0, 420), true, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::EndChild();
        return;
    }

    ImGui::PushID("CrownMovieTimeline");
    ImGui::TextUnformatted(crownDefeatMode_
        ? "最終撃破ムービー / 60 fps"
        : "王冠継承・崩壊ムービー / 60 fps");
    // プレビューが未開始なら開始場所だけ案内し、後続の再生操作を表示しません。
    if (!enabled_) {
        ImGui::TextDisabled("右のInspectorから「ムービープレビューを開始」を押してください。");
        ImGui::PopID();
        ImGui::EndChild();
        return;
    }

    // 再生終端から再開する場合は先頭へ戻し、時間スライダーも同じ Seek 処理を使います。
    const auto* game = sceneManager_ ? dynamic_cast<GamePlayScene*>(sceneManager_->GetCurrentScene()) : nullptr;
    const auto finaleTiming = game ? game->GetCrownFinaleTiming() : CrownFinaleTuning::Get();

    // 再生ボタンが押された場合だけ状態を切り替える。
    if (ImGui::Button(playing_ ? "一時停止" : "再生")) {
        // 停止中で終端とみなせる位置なら、再生を有効にする前に先頭へ戻す。
        if (!playing_ && elapsedTime_ >= loopDuration_ - .001f) {
            SeekPreview(0);
        }

        playing_ = !playing_;
    }

    ImGui::SameLine();

    // 先頭ボタンはシークの時刻に0秒を渡す。
    if (ImGui::Button("先頭")) {
        SeekPreview(0);
    }

    // プレビューの固定刻みに合わせ、移動フレーム数を秒へ換算してシークします。
    const std::int32_t frames[] = { -10, -1, 1, 10 };

    for (std::int32_t frame : frames) {
        ImGui::SameLine();
        char label[16];
        std::snprintf(label, sizeof(label), "%+dF", frame);

        // 押されたボタンに対応するフレーム数を固定刻みの秒数に換算して移動する。
        if (ImGui::Button(label)) {
            SeekPreview(elapsedTime_ + frame * kFixedPreviewStep);
        }
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(95);
    ImGui::SliderFloat("速度", &playbackSpeed_, .1f, 2.0f, "%.2fx");
    ImGui::SameLine();
    ImGui::Checkbox("ループ", &loop_);

    float seek = elapsedTime_;
    ImGui::SetNextItemWidth(-1);

    // 時間スライダーが動いた場合だけ、指定された秒位置へシークする。
    if (ImGui::SliderFloat("##Time", &seek, 0, loopDuration_, "%.3f 秒")) {
        SeekPreview(seek);
    }

    ImGui::Text("%.3f / %.2f 秒 | %dF", elapsedTime_, loopDuration_,
        static_cast<std::int32_t>(std::round(elapsedTime_ * 60)));

    // 最終撃破は章ごとのジャンプボタンを表示して終了し、継承ムービーのレーンは作りません。
    if (crownDefeatMode_) {
        const auto* game=sceneManager_?dynamic_cast<GamePlayScene*>(sceneManager_->GetCurrentScene()):nullptr;
        // 撃破側はシーンのマーカーから秒位置を取得し、シーンがないときは全尺に対する割合で補います。
        const auto point=[&](const char* marker,float fallback){return game?game->GetCrownDefeatMarkerTime(marker):fallback*loopDuration_;};
        const char* names[] = { "最後の一撃", "王冠の亀裂", "王冠破砕", "力の喪失", "小スライム", "勝利", "終了" };
        const float points[] = { 0, .14f*loopDuration_, point("rupture",.28f)+.015f*loopDuration_,
            point("shrink",.43f), point("settle",.64f)+.025f*loopDuration_, point("victory",.76f)+.035f*loopDuration_, loopDuration_ };

        for (std::int32_t index = 0; index < 7; ++index) {
            // 最初以外のボタンは直前のボタンと同じ行に置く。
            if (index) {
                ImGui::SameLine();
            }

            // 章ボタンを押したときは、対応する実秒位置から確認できる。
            if (ImGui::Button(names[index])) {
                SeekPreview(points[index]);
            }
        }

        ImGui::Separator();
        ImGui::TextWrapped("最後の一撃 → 王冠の亀裂と再集束 → 破砕・放電 → 力が抜けて小スライムへ → 王の勝利");
        ImGui::TextDisabled("本番と同じ演出素材で進行します。編集は演出タイムラインで行います。");
        ImGui::PopID();
        ImGui::EndChild();
        return;
    }

    // 継承側は調整値が持つ0〜1の章位置を使い、ボタンを二段に並べる。
    const char* names[] = { "破断", "欠片着地", "暴走", "吸い込み", "体内模様", "金色の体", "王冠", "翼", "脱出", "最終ステージ" };
    const float points[] = { finaleTiming.movieFragmentBreakTime, finaleTiming.movieFragmentLandTime, finaleTiming.movieRageTime,
        finaleTiming.movieFragmentPullTime, finaleTiming.movieFragmentAbsorbTime, finaleTiming.movieBodyTime, finaleTiming.movieCrownTime,
        finaleTiming.movieWingsTime, finaleTiming.movieEscapeTime, finaleTiming.movieArriveTime };
    for (std::int32_t index = 0; index < 10; ++index) {
        // 各行の先頭である0番と5番だけ改行のまま描く。
        if (index && index != 5) {
            ImGui::SameLine();
        }

        // 選んだ章の調整値をそのままSeekPreviewに渡す。
        if (ImGui::Button(names[index])) {
            SeekPreview(points[index]);
        }
    }

    // テキスト入力中はショートカットを奪わず、固定フレーム単位でシークします。
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive()) {
        // Spaceを押したときだけ再生状態を反転する。
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            playing_ = !playing_;
        }

        const std::int32_t step = ImGui::GetIO().KeyShift ? 10 : 1;

        // 左矢印の押下時は固定刻みで前の位置を指定する。
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) {
            SeekPreview(elapsedTime_ - step * kFixedPreviewStep);
        }

        // 右矢印の押下時は固定刻みで後の位置を指定する。
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) {
            SeekPreview(elapsedTime_ + step * kFixedPreviewStep);
        }

        // Homeの押下時は0秒を指定する。
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
            SeekPreview(0);
        }
    }

    // 秒の目盛りを引き、王冠・プレイヤー・ステージ・カメラの進行を別レーンで示します。
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float labelWidth = 90;
    const float pixels = (std::max)(10.0f, (width - labelWidth) / loopDuration_);
    ImDrawList* draw = ImGui::GetWindowDrawList();

    for (std::int32_t second = 0; second <= static_cast<std::int32_t>(loopDuration_); ++second) {
        float x = origin.x + labelWidth + second * pixels;
        char label[16];
        std::snprintf(label, sizeof(label), "%ds", second);
        draw->AddLine({ x, origin.y + 19 }, { x, origin.y + 158 }, IM_COL32(115, 123, 140, 90));
        draw->AddText({ x + 2, origin.y }, IM_COL32(225, 230, 240, 255), label);
    }

    const char* rows[] = { "王冠の欠片", "プレイヤー", "ステージ", "カメラ" };

    for (std::int32_t row = 0; row < 4; ++row) {
        draw->AddText({ origin.x, origin.y + 28 + row * 32 }, IM_COL32(230, 233, 239, 255), rows[row]);
    }

    auto block = [&](std::int32_t row, float start, float end, ImU32 color, const char* label) {
        DrawTimelineBlock(draw, origin, labelWidth, pixels, origin.y + 23 + row * 32,
            30, start * loopDuration_, end * loopDuration_, color, label);
    };

    // 以下の区間は Tuning 側の 0〜1 の時刻を実秒へ変換して重ねます。
    block(0, 0, finaleTiming.movieFragmentBreakTime, IM_COL32(192, 153, 70, 240), "亀裂");
    block(0, finaleTiming.movieFragmentBreakTime, finaleTiming.movieFragmentLandTime, IM_COL32(245, 169, 75, 240), "落下");
    block(0, finaleTiming.movieFragmentLandTime, finaleTiming.movieFragmentPullTime, IM_COL32(185, 172, 100, 240), "着地");
    block(0, finaleTiming.movieFragmentPullTime, finaleTiming.movieFragmentAbsorbTime, IM_COL32(85, 191, 221, 240), "吸収");

    block(1, finaleTiming.movieFragmentAbsorbTime, finaleTiming.movieBodyTime, IM_COL32(114, 218, 223, 240), "体内模様");
    block(1, finaleTiming.movieBodyTime, finaleTiming.movieCrownTime, IM_COL32(239, 190, 56, 240), "金色の体");
    block(1, finaleTiming.movieCrownTime, finaleTiming.movieWingsTime, IM_COL32(239, 164, 56, 240), "王冠");
    block(1, finaleTiming.movieWingsTime, finaleTiming.movieEscapeTime, IM_COL32(235, 213, 156, 240), "翼");
    block(1, finaleTiming.movieEscapeTime, finaleTiming.movieArriveTime, IM_COL32(125, 177, 236, 240), "飛行脱出");
    block(1, finaleTiming.movieArriveTime, 1, IM_COL32(112, 205, 168, 240), "着地");

    block(2, finaleTiming.movieRageTime, finaleTiming.movieRageTime + .07f, IM_COL32(236, 190, 116, 240), "衝撃");
    block(2, finaleTiming.movieRageTime + .02f, finaleTiming.movieArriveTime, IM_COL32(218, 95, 91, 240), "床・壁の崩壊");

    block(3, 0, finaleTiming.movieFragmentLandTime, IM_COL32(134, 145, 222, 240), "装飾を追う");
    block(3, finaleTiming.movieFragmentLandTime, finaleTiming.movieEscapeTime, IM_COL32(152, 135, 206, 240), "吸収・変身");
    block(3, finaleTiming.movieEscapeTime, 1, IM_COL32(109, 159, 204, 240), "脱出・最終戦");

    // 再生ヘッドを全レーンに通し、Dummy で描画領域の高さを ImGui へ通知します。
    float x = origin.x + labelWidth + elapsedTime_ * pixels;
    draw->AddLine({ x, origin.y + 18 }, { x, origin.y + 158 }, IM_COL32(255, 117, 116, 255), 2);
    ImGui::Dummy({ width, 162 });
    ImGui::TextDisabled("Space: 再生/停止 / ←→: 1F / Shift+←→: 10F / Home: 先頭");
    ImGui::PopID();
    ImGui::EndChild();
#endif
}

#endif
