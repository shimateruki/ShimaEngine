#ifdef USE_IMGUI
#include <cstdint>
#include "AnimationClipEditor.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#ifdef USE_IMGUI
#include "imgui.h"
#endif

// シークでは未登録の姿勢を解除し、表示モデルとInspectorを同じ時刻へ揃える。
void AnimationClipEditor::SeekTimeline(float time) {
    currentTime_ = std::clamp(time, 0.0f, (std::max)(0.01f, duration_));
    play_ = false;
    bodyEditOverrideActive_ = false;
    // 入力中の一時姿勢を外してから秒指定の姿勢を適用し、その結果をInspectorへ読み戻す。
    ClearAllEditOverrides();
    ApplyTimelinePose();
    SyncBodyUiFromTimeline();
    // 関節行を選んでいる場合は、インスペクターも同じ関節に切り替える。
    if (!editBodyTransform_ && selectedJointIndex_ >= 0)
        SyncUiFromJoint(selectedJointIndex_);
}

void AnimationClipEditor::DrawTimelineControls() {
#ifdef USE_IMGUI
    // 終端で開始を選んだときだけ先頭へ戻す。途中から再開する場合も手動の姿勢上書きを解除する。
    // 再生ボタンを押したときに再生状態を反転し、仮編集の姿勢を解除する。
    if (ImGui::Button(play_ ? "停止" : "再生")) {
        // 停止状態から再生を始めるとき、すでに末尾に達していたら先頭から再生する。
        if (!play_ && currentTime_ >= duration_)
            SeekTimeline(0.0f);
        bodyEditOverrideActive_ = false;
        ClearAllEditOverrides();
        play_ = !play_;
    }

    ImGui::SameLine();
    // 先頭ボタンを押すと、再生ヘッドを時刻ゼロに戻す。
    if (ImGui::Button("先頭"))
        SeekTimeline(0.0f);
    ImGui::SameLine();
    // 1コマの移動量は設定FPSの逆数秒。クリップ自体の時間は秒のまま保持する。
    // 戻るボタンを押した場合、設定 FPS における1フレーム分だけ戻す。
    if (ImGui::Button("-1F"))
        SeekTimeline(currentTime_ - 1.0f / timelineFps_);
    ImGui::SameLine();
    // 進むボタンを押した場合、設定 FPS における1フレーム分だけ進める。
    if (ImGui::Button("+1F"))
        SeekTimeline(currentTime_ + 1.0f / timelineFps_);
    ImGui::SameLine();
    ImGui::Checkbox("ループ", &loop_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::DragFloat("速度", &playbackSpeed_, 0.05f, 0.05f, 4.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SameLine();
    ImGui::Text("%.3f 秒 / %d F", currentTime_, static_cast<std::int32_t>(std::round(currentTime_ * timelineFps_)));

    ImGui::SetNextItemWidth(110);

    // 尺を変更した場合は、既存の最後のキーやイベントが新しい範囲の外に出ないようにする。
    if (ImGui::DragFloat("長さ（秒）", &duration_, 0.05f, 0.05f, 600.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
        // 既存キーを範囲外へ隠す短縮は行わない。
        for (const auto& key : bodyKeys_)
            duration_ = (std::max)(duration_, key.time);

        for (const auto& key : keys_)
            duration_ = (std::max)(duration_, key.time);

        for (const auto& key : events_)
            duration_ = (std::max)(duration_, key.time);
        SeekTimeline(currentTime_);
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);

    // FPS の選択欄を開いている間だけ、選択肢を表示する。
    if (ImGui::BeginCombo("FPS", timelineFps_ == 30 ? "30" : "60")) {
        // 30 FPS を選択した場合は、以後のコマ送りやスナップの基準を切り替える。
        if (ImGui::Selectable("30", timelineFps_ == 30))
            timelineFps_ = 30;
        // 60 FPS を選択した場合も、同じフレーム間隔の基準を更新する。
        if (ImGui::Selectable("60", timelineFps_ == 60))
            timelineFps_ = 60;
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::Checkbox("フレーム吸着", &timelineSnap_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("拡大", &timelineZoom_, 30.0f, 1200.0f, "%.0f px/s");
    ImGui::SameLine();
    // ラベル分を引いた画面幅へクリップ全体を収めるよう、1 秒あたりの表示幅を再計算する。
    if (ImGui::Button("全体表示"))
        timelineZoom_ = std::clamp((ImGui::GetWindowWidth() - 220.0f) / (std::max)(0.05f, duration_), 1.0f, 1200.0f);
#endif
}

void AnimationClipEditor::DrawTimelinePanel() {
#ifdef USE_IMGUI
    // タイムラインの子領域を描画できない場合も EndChild() を呼び、行やキーの描画は省略する。
    if (!ImGui::BeginChild("クリップ時間軸", ImVec2(0, 320), true, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::EndChild();
        return;
    }

    ImGui::TextUnformatted("時間軸で選択 → 右側で姿勢を編集 → キー登録 → 再生・保存");
    DrawTimelineControls();
    // 身体・関節の編集対象がそろうまで、キー登録だけを無効にする。
    ImGui::BeginDisabled(!previewModel_ || (!editBodyTransform_ && selectedJointIndex_ < 0));

    // キー登録が押されたら、選択行に対応する姿勢を登録する。
    if (ImGui::Button("現在の姿勢をキー登録")) {
        // 本体行を選択している場合は、現在時刻に本体の変形を登録する。
        if (editBodyTransform_)
            AddOrUpdateBodyKey();
        // 関節行を選択している場合は、その関節の変形を登録する。
        else AddOrUpdateKey();
    }

    ImGui::EndDisabled();
    ImGui::SameLine();

    // 保存操作の後に、アセット一覧を再取得して表示に反映する。
    if (ImGui::Button("クリップ保存")) {
        SaveAuthoringJson();
        RefreshAssetFiles();
    }
    ImGui::SameLine();

    ImGui::TextDisabled("空白をドラッグ：時刻移動 / 菱形をドラッグ：キー移動 / Esc：移動取消 / Space：再生 / 左右：1F");
    // プレビュー用モデルが未選択なら、モデルの選択を案内する。
    if (!previewModel_)
        ImGui::TextWrapped("モデルのあるObjectを選択してください。");

    const bool keyboard = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput;

    // パネルにフォーカスがあり、文字入力や有効なキーのドラッグがない間だけショートカットを受け付ける。
    if (keyboard && timelineDragKind_ < 0) {
        // キー移動中はショートカットで再生位置を変えず、ドラッグ開始時刻を保つ。
        // Space が押されたら、再生と一時停止を切り替える。
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            // 停止中の再生位置が末尾なら、再生再開前に先頭へ戻す。
            if (!play_ && currentTime_ >= duration_)
                SeekTimeline(0);
            bodyEditOverrideActive_ = false;
            ClearAllEditOverrides();
            play_ = !play_;
        }

        // 左矢印が押された場合は、1フレーム分だけ時刻を戻す。
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
            SeekTimeline(currentTime_ - 1.0f / timelineFps_);
        // 右矢印が押された場合は、1フレーム分だけ時刻を進める。
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
            SeekTimeline(currentTime_ + 1.0f / timelineFps_);
    }

    // キーを持つボーンと現在編集中のボーンを独立した行で表示する。
    std::vector<std::int32_t> joints;

    for (const auto& key : keys_) {
        // 同じ関節に複数のキーがあっても、タイムラインには一行だけ作る。
        if (std::find(joints.begin(), joints.end(), key.jointIndex) == joints.end())
            joints.push_back(key.jointIndex);
    }

    // 選択中の関節が有効で一覧にない場合は、キーがゼロでも登録できるように行を用意する。
    if (selectedJointIndex_ >= 0 && std::find(joints.begin(), joints.end(), selectedJointIndex_) == joints.end())
        joints.push_back(selectedJointIndex_);
    std::sort(joints.begin(), joints.end());
    constexpr float kLabelWidth = 180.0f, kRowHeight = 32.0f, kRulerHeight = 28.0f;
    const std::int32_t rows = 2 + static_cast<std::int32_t>(joints.size());
    ImGui::BeginChild("AnimationTracks", {0, 0}, true, ImGuiWindowFlags_HorizontalScrollbar);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = (std::max)(ImGui::GetContentRegionAvail().x, kLabelWidth + duration_ * timelineZoom_ + 40);
    const float height = (std::max)(ImGui::GetContentRegionAvail().y, kRulerHeight + rows * kRowHeight);
    ImGui::InvisibleButton("TrackCanvas", {width, height});
    const bool clicked = ImGui::IsItemClicked();
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    const auto mouse = ImGui::GetIO().MousePos;
    auto* draw = ImGui::GetWindowDrawList();
    // 時刻から画面座標への変換と吸着をここで統一し、目盛りとキー操作のずれを防ぐ。
    const float timeOrigin = origin.x + kLabelWidth;
    auto screenX = [&](float time) { return timeOrigin + time * timelineZoom_; };
    // 秒数にFPSを掛けたコマ番号を丸め、再び秒に戻す。吸着を切った場合もクリップ内へ制限する。
    auto quantize = [&](float time) {
        // スナップが有効な場合だけ、マウス位置から求めた秒数をフレーム境界に丸める。
        if (timelineSnap_)
            time = std::round(time * timelineFps_) / timelineFps_;
        return std::clamp(time, 0.0f, duration_);
    };

    draw->AddRectFilled(origin, {origin.x + width, origin.y + height}, IM_COL32(26, 29, 36, 255));

    for (std::int32_t row = 0; row < rows; ++row) {
        const float y = origin.y + kRulerHeight + row * kRowHeight;
        draw->AddRectFilled({origin.x, y}, {origin.x + width, y + kRowHeight}, row % 2 ? IM_COL32(39,43,53,255) : IM_COL32(33,37,46,255));
        std::string label = row == 0 ? "身体全体" : "イベント";

        // 本体とイベントの間の行は、それぞれ一つの関節に対応する。
        if (row > 0 && row < rows - 1) {
            const std::int32_t joint = joints[row - 1];
            label = "ボーン " + std::to_string(joint);
            // プレビューモデルがあり関節インデックスが範囲内なら、モデルから表示名を取得する。
            if (previewModel_ && joint >= 0 && joint < static_cast<std::int32_t>(previewModel_->GetJoints().size()))
                label = previewModel_->GetJoints()[joint].name;
        }

        draw->PushClipRect({origin.x,y}, {timeOrigin-4,y+kRowHeight}, true);
        draw->AddText({origin.x+8,y+8}, IM_COL32(215,221,235,255), label.c_str());
        draw->PopClipRect();

        // ラベル列をクリックしている場合は、クリック位置の行を編集対象にする。
        if (clicked && mouse.x < timeOrigin && mouse.y >= y && mouse.y < y + kRowHeight) {
            // 最初の行は本体を編集する。
            if (row == 0)
                editBodyTransform_ = true;
            // 本体行とイベント行の間なら、行番号から対象の関節を選ぶ。
            else if (row < rows - 1) { editBodyTransform_ = false; selectedJointIndex_ = joints[row-1]; SyncUiFromJoint(selectedJointIndex_); }
        }
    }

    // 可視範囲だけ目盛りを描画し、長いクリップでも描画負荷を一定にする。
    // 1目盛りを約75ピクセル以上にする最小の2のべきの秒間隔を選び、ズームに合わせて増減させる。
    const float step = std::pow(2.0f, std::ceil(std::log2(75.0f / (std::max)(1.0f,timelineZoom_))));
    const float visibleStart = (std::max)(0.0f, (ImGui::GetScrollX()-kLabelWidth) / timelineZoom_);
    const float visibleEnd = (std::min)(duration_, (ImGui::GetScrollX()+ImGui::GetWindowWidth()) / timelineZoom_);

    for (float t = std::floor(visibleStart / step) * step; t <= visibleEnd; t += step) {
        const float x = screenX(t);
        draw->AddLine({x,origin.y}, {x,origin.y+height}, IM_COL32(68,73,86,255));
        char text[48];
        std::snprintf(text,sizeof(text),"%.2fs",t);
        draw->AddText({x+4,origin.y+4}, IM_COL32(181,189,204,255), text);
    }

    bool hit = false;
    // 本体・関節・イベントを同じヒット判定で扱い、選択時に各Inspectorの値も同期する。
    auto point = [&](std::int32_t kind, std::int32_t index, float time, std::int32_t row, ImU32 color) {
        const ImVec2 p{screenX(time), origin.y+kRulerHeight+(row+0.5f)*kRowHeight};
        const bool isNearKey = hovered && std::abs(mouse.x-p.x)<9 && std::abs(mouse.y-p.y)<12;
        const bool selected = std::abs(currentTime_-time)<0.001f &&
            (kind == 0 ? editBodyTransform_ : kind == 1 ? !editBodyTransform_ && selectedJointIndex_ == keys_[index].jointIndex : selectedEventIndex_ == index);
        draw->AddQuadFilled({p.x,p.y-6},{p.x+6,p.y},{p.x,p.y+6},{p.x-6,p.y},selected ? IM_COL32(255,255,255,255) : color);
        // マーカーの近くにカーソルがあるときだけ、正確な時刻を表示する。
        if (isNearKey)
            ImGui::SetTooltip("%.3f 秒 / %d F\nドラッグして時間を移動",time,static_cast<std::int32_t>(std::round(time*timelineFps_)));
        // 複数の項目がクリック範囲に重なっても、このフレームに一つだけ選択する。
        if (clicked && isNearKey && !hit) {
            hit = true;

            // 本体キーを押した場合は、本体のキーを選択する。
            if (kind == 0) {
                editBodyTransform_=true;
                selectedBodyKeyIndex_=index;
            }
            // 関節キーを押した場合は、そのキーの関節を選択する。
            if (kind == 1) {
                editBodyTransform_=false;
                selectedJointIndex_=keys_[index].jointIndex;
            }
            // イベントを押した場合は、名前・種別・オフセットを入力欄へ読み込む。
            if (kind == 2) {
                selectedEventIndex_=index;
                eventType_=events_[index].type;
                eventOffsetUi_=events_[index].offset;
                strncpy_s(eventNameBuffer_,events_[index].name.c_str(),_TRUNCATE);
            }
            SeekTimeline(time);
            timelineDragKind_=kind;
            timelineDragIndex_=index;
            timelineDragTime_=time;
            timelineDragMouseX_=mouse.x;
        }
    };

    for (std::int32_t i=0;i<static_cast<std::int32_t>(bodyKeys_.size());++i)
        point(0,i,bodyKeys_[i].time,0,IM_COL32(98,210,240,255));

    for (std::int32_t i=0;i<static_cast<std::int32_t>(keys_.size());++i) {
        const std::int32_t row=1+static_cast<std::int32_t>(std::find(joints.begin(),joints.end(),keys_[i].jointIndex)-joints.begin());
        point(1,i,keys_[i].time,row,IM_COL32(192,155,245,255));
    }

    for (std::int32_t i=0;i<static_cast<std::int32_t>(events_.size());++i)
        point(2,i,events_[i].time,rows-1,IM_COL32(244,193,93,255));

    // ドラッグ中のキーの種別が有効な間だけ、位置のプレビューや終了操作を処理する。
    if (timelineDragKind_ >= 0) {
        // 押し始めの数pxは元の時刻に固定し、クリックだけでキーが移動するのを防ぐ。
        const float target = std::abs(mouse.x-timelineDragMouseX_) < 3.0f ? timelineDragTime_ :
            quantize(timelineDragTime_ + (mouse.x-timelineDragMouseX_)/timelineZoom_);
        draw->AddLine({screenX(target),origin.y},{screenX(target),origin.y+height},IM_COL32(255,218,120,255),2);

        // -2はドラッグ取消後の解放待ち。押したままシーク操作へ切り替わるのを防ぐ。
        if (ImGui::IsKeyPressed(ImGuiKey_Escape,false)) {
            timelineDragKind_ = -2;
            timelineDragIndex_ = -1;
        }
        // キャンセルせず左ボタンを離したところで、プレビュー時刻を確定する。
        else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            // ドラッグ中には配列を並べ替えない。既存キーへの重なりは拒否し、上書き消失を防ぐ。
            const std::int32_t index=timelineDragIndex_, kind=timelineDragKind_;
            bool collision=false;

            // 身体は全体、ボーンは同じ関節内のキーとの重複を避ける。イベントは同時刻へ置ける。
            // 本体キーのドラッグで、対象のキーがまだ範囲内にある場合に更新する。
            if (kind == 0 && index < static_cast<std::int32_t>(bodyKeys_.size())) {
                for (std::int32_t i=0;i<static_cast<std::int32_t>(bodyKeys_.size());++i)
                    // ドラッグ中の本体キー自身を除き、0.001 秒未満の差で別のキーがあるか調べる。
                    if (i!=index && std::abs(bodyKeys_[i].time-target)<0.001f)
                        collision=true;

                // 同じ時刻に別の本体キーがない場合だけ移動し、時刻順に並べ直す。
                if (!collision) {
                    bodyKeys_[index].time=target;
                    SortBodyKeys();

                    for (std::int32_t i=0;i<static_cast<std::int32_t>(bodyKeys_.size());++i)
                        // 並べ替えで添字が変わるため、確定時刻から選択し直す。
                        if (std::abs(bodyKeys_[i].time-target)<0.001f)
                            selectedBodyKeyIndex_=i;
                }
            // 関節キーのドラッグで、対象のキーがまだ範囲内にある場合に更新する。
            } else if (kind == 1 && index < static_cast<std::int32_t>(keys_.size())) {
                for (std::int32_t i=0;i<static_cast<std::int32_t>(keys_.size());++i)
                    // 別のキーで、同じ関節かつ時刻がほぼ一致するものを衝突とする。
                    if (i!=index && keys_[i].jointIndex==keys_[index].jointIndex && std::abs(keys_[i].time-target)<0.001f)
                        collision=true;

                // 同じ関節のキーと衝突しない場合だけ移動し、時刻順に並べ直す。
                if (!collision) {
                    keys_[index].time=target;
                    SortKeys();
                }
            // イベントは時刻が重なっても登録できるので、添字が有効なら移動する。
            } else if (kind == 2 && index < static_cast<std::int32_t>(events_.size())) {
                events_[index].time=target;
            }

            // 移動に失敗したキーは元の時刻、確定したキーは新しい時刻でプレビューし直す。
            SeekTimeline(collision ? timelineDragTime_ : target);
            timelineDragKind_ = timelineDragIndex_ = -1;
        }
    // キーを掴んでいない時間領域のドラッグは、データを動かさずプレビュー時計だけを移動する。
    } else if (timelineDragKind_ == -1 && active && ImGui::IsMouseDown(ImGuiMouseButton_Left) && mouse.x >= timeOrigin && !hit) {
        SeekTimeline(quantize((mouse.x-timeOrigin)/timelineZoom_));
    }

    // キャンセル後は左ボタンを離すまで状態を保持し、押したままの操作を再処理しない。
    if (timelineDragKind_ == -2 && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        timelineDragKind_ = -1;
    const float playhead=screenX(currentTime_);
    draw->AddLine({playhead,origin.y},{playhead,origin.y+height},IM_COL32(255,107,120,255),2);
    draw->AddTriangleFilled({playhead-6,origin.y},{playhead+6,origin.y},{playhead,origin.y+10},IM_COL32(255,107,120,255));
    ImGui::EndChild();
    ImGui::EndChild();
#endif
}

#endif
