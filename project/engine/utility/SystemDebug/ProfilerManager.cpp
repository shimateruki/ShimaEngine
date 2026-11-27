#include <cstdint>
#include "ProfilerManager.h"
#include "DirectXCommon.h"
#include <algorithm>

#ifdef USE_IMGUI
#include "imgui.h"
#include "IconsFontAwesome5.h"
#include "Object3D.h"
#include "Model.h"
#endif


ProfilerManager* ProfilerManager::GetInstance() {
    static ProfilerManager sInstance;
    return &sInstance;
}

// ロード計測とGPUの直近値を初期化する。計測スレッドと同じロックで保護する。
void ProfilerManager::Initialize() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    loadDataMap_.clear();
    latestGpuTimeMap_.clear();
}

// 指定カテゴリへ素材のロード時間をミリ秒で追記する。
void ProfilerManager::RecordLoadTime(const std::string& category, const std::string& name, float timeMs) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    loadDataMap_[category].push_back({ name, timeMs });
}

// GPU 計測をミリ秒で記録する。詳細計測と全体グラフ用の履歴は用途ごとに更新する。
void ProfilerManager::RecordGpuTime(const std::string& name, float timeMs) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    latestGpuTimeMap_[name] = timeMs;

    std::string displayName = name;
    // GPU パス名 Total は全体グラフ用の表示名に変換する。詳細計測と直近値のキーは元の名前。
    if (name == "Total")
        displayName = "全体";

    // 詳細サンプリング中だけ、個別項目の経過時間と観測回数を積算する。
    if (isGpuSampling_) {
        gpuSampleAccum_[name] += timeMs;
        gpuSampleCount_[name]++;
    }

    // --- タイムライン（全体グラフ）に表示する項目を制限 ---
    // ここに含まれない名前（個別のオブジェクト名など）はグラフに出さない
    static const std::vector<std::string> kGlobalNames = {
        "全体", "影描画", "メイン描画", "  3Dシーン", "  ゲームUI", "  エフェクト", "  デバッグ", "後処理", "エディタUI"
    };
    
    bool isGlobal = false;

    for(const auto& gn : kGlobalNames) {
        // 全体グラフの登録名が見つかれば、その名前で履歴を記録できる。
        if(displayName == gn) {
            isGlobal = true;
            break;
        }
    }

    if (!isGlobal)
        return; // グローバル項目以外は以下の履歴記録をスキップ

    auto& data = gpuDataMap_[displayName];
    data.current = timeMs;

    // 指数移動平均 (α=0.1 → より安定させ、ノイズを除去)
    data.smoothed = data.smoothed * 0.9f + timeMs * 0.1f;

    // 履歴リングバッファに記録
    data.history[data.historyIndex] = timeMs;
    data.historyIndex = (data.historyIndex + 1) % kHistorySize;
}

void ProfilerManager::RecordCpuTime(const std::string& name, float timeMs) {
    // 直近値、平滑値、リング履歴を同じ計測値から更新する。
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto& data = cpuDataMap_[name];
    data.current = timeMs;
    // 時間はミリ秒です。現在のサンプルを10％混ぜ、グラフに残る短時間の変動を抑えます。
    data.smoothed = data.smoothed * 0.9f + timeMs * 0.1f;
    // 要素数を増やさず、末尾の次は先頭へ戻って最も古い値を上書きします。
    data.history[data.historyIndex] = timeMs;
    data.historyIndex = (data.historyIndex + 1) % kHistorySize;
}

// 名前に対応する最新の GPU 計測をミリ秒で返す。まだ計測が無ければ0。
float ProfilerManager::GetLatestGpuTime(const std::string& name) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto it = latestGpuTimeMap_.find(name);
    return it != latestGpuTimeMap_.end() ? it->second : 0.0f;
}

// 名前に対応する最新の CPU 計測をミリ秒で返す。まだ計測が無ければ0。
float ProfilerManager::GetLatestCpuTime(const std::string& name) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto it = cpuDataMap_.find(name);
    return it != cpuDataMap_.end() ? it->second.current : 0.0f;
}

void ProfilerManager::DrawImGui() {
#ifdef USE_IMGUI
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // プロファイラーが閉じている間は、ウィンドウと表を描かない。
    if (!isOpen_)
        return;

    // ウィンドウの初期サイズを設定
    ImGui::SetNextWindowSize(ImVec2(850, 600), ImGuiCond_FirstUseEver);

    // ウィンドウを表示できる場合にだけ、左の選択と右の詳細を作る。
    if (ImGui::Begin(ICON_FA_CHART_BAR " システムプロファイラ (System Profiler)", &isOpen_)) {
        
        // ==========================================================
        // 左ペイン：メニュー
        // ==========================================================
        ImGui::BeginChild("LeftPane", ImVec2(200, 0), true);
        
        const char* menuItems[] = {
            ICON_FA_DOWNLOAD " ロード時間 (Assets)",
            ICON_FA_CLOCK " 処理時間 (Timeline)",
            ICON_FA_CUBE " オブジェクト負荷 (Objects)"
        };

        for (std::int32_t i = 0; i < IM_ARRAYSIZE(menuItems); i++) {
            // クリックした項目を、右側に表示する情報の種類へ反映する。
            if (ImGui::Selectable(menuItems[i], selectedIndex_ == i)) {
                selectedIndex_ = i;
            }
        }
        
        ImGui::EndChild();

        ImGui::SameLine();

        // ==========================================================
        // 右ペイン：詳細表示
        // ==========================================================
        ImGui::BeginChild("RightPane", ImVec2(0, 0), true);

        // ロード時間の項目が選択中なら、アセットをカテゴリごとに表示する。
        if (selectedIndex_ == 0) {
            // ---------------------------------------------------------
            // ロード時間表示
            // ---------------------------------------------------------
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "[ アセット・ロード時間 ]");
            ImGui::Separator();
            ImGui::Spacing();

            for (auto& pair : loadDataMap_) {
                const std::string& category = pair.first;
                auto& records = pair.second;

                // 開いているカテゴリにだけ、素材ごとの計測表を表示する。
                if (ImGui::CollapsingHeader(category.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    // 表を開始できた場合に、素材名・時間・比率の列と計測を描く。
                    if (ImGui::BeginTable(category.c_str(), 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                        ImGui::TableSetupColumn("アセット名", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("時間 (ms)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                        ImGui::TableSetupColumn("グラフ", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableHeadersRow();

                        // カテゴリ内で最も長いロードをバーの全長にして、同じカテゴリ内の待ち時間を比べます。
                        float maxTime = 0.0f;

                        for (auto& record : records)
                            // 現在の最大値を超える計測だけ、バーの基準を更新する。
                            if (record.timeMs > maxTime)
                                maxTime = record.timeMs;

                        for (auto& record : records) {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::TextUnformatted(record.name.c_str());
                            ImGui::TableSetColumnIndex(1); 
                            // 16.6msを超えるロードは、時間の文字を赤にして識別できるようにする。
                            if (record.timeMs > 16.6f)
                                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%.2f ms", record.timeMs);
                            // しきい値以下の計測値は既定の文字色で表示する。
                            else ImGui::Text("%.2f ms", record.timeMs);

                            ImGui::TableSetColumnIndex(2);
                            float fraction = (maxTime > 0) ? (record.timeMs / maxTime) : 0.0f;
                            ImVec4 color = ImVec4(0.2f, 0.7f, 0.2f, 1.0f);
                            // 時間が10msを超えるロードの棒を黄色にする。
                            if (record.timeMs > 10.0f)
                                color = ImVec4(0.8f, 0.8f, 0.2f, 1.0f);
                            // 30msを超えた場合は黄色より赤を優先する。
                            if (record.timeMs > 30.0f)
                                color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);

                            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
                            ImGui::PushID(record.name.c_str());
                            ImGui::ProgressBar(fraction, ImVec2(-1, 0), "");
                            ImGui::PopID();
                            ImGui::PopStyleColor();
                        }

                        ImGui::EndTable();
                    }
                }
            }
        }
        // 処理時間が選択中なら、GPU と CPU の現在値・移動平均・履歴を表示する。
        else if (selectedIndex_ == 1) {

            // ============================
            // GPU セクション
            // ============================
            ImGui::TextColored(ImVec4(0.3f, 0.7f, 1.0f, 1.0f), ICON_FA_MICROCHIP "  GPU タイムライン");
            ImGui::Separator();
            ImGui::Spacing();

            // GPU の表を開始できた場合に、全体パスの計測とグラフを描く。
            if (ImGui::BeginTable("GpuTimeline", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("パス名", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupColumn("現在値 (ms)", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableSetupColumn("平均 (ms)", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableSetupColumn("履歴 (120フレーム)", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                for (auto& pair : gpuDataMap_) {
                    const std::string& name = pair.first;
                    auto& data = pair.second;

                    ImGui::TableNextRow();
                    
                    // Pass名
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(name.c_str());

                    // 現在値（生値）
                    ImGui::TableSetColumnIndex(1);
                    ImVec4 valColor = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
                    // GPU の直近値が10msを超えたら黄色にする。
                    if (data.current > 10.0f)
                        valColor = ImVec4(0.8f, 0.8f, 0.2f, 1.0f);
                    // 直近値が16.66msを超えた場合は赤で表示する。
                    if (data.current > 16.66f)
                        valColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
                    ImGui::TextColored(valColor, "%.3f", data.current);

                    // 平均値（スムージング後）
                    ImGui::TableSetColumnIndex(2);
                    ImVec4 avgColor = ImVec4(0.5f, 0.9f, 0.5f, 1.0f);
                    // GPU の平滑値が10msを超えたら黄色にする。
                    if (data.smoothed > 10.0f)
                        avgColor = ImVec4(0.8f, 0.8f, 0.2f, 1.0f);
                    // 平滑値が16.66msを超えた場合は赤を優先する。
                    if (data.smoothed > 16.66f)
                        avgColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
                    ImGui::TextColored(avgColor, "%.3f", data.smoothed);

                    // 履歴グラフ（折れ線）
                    ImGui::TableSetColumnIndex(3);

                    // リングバッファを正しい順序で描画するため配列を並べ直す
                    float plotData[kHistorySize];

                    for (std::int32_t i = 0; i < kHistorySize; i++) {
                        plotData[i] = data.history[(data.historyIndex + i) % kHistorySize];
                    }

                    ImGui::PushID(name.c_str());
                    ImGui::PlotLines("", plotData, kHistorySize, 0, nullptr, 0.0f, 20.0f, ImVec2(-1, 30));
                    ImGui::PopID();
                }

                ImGui::EndTable();
            }

            ImGui::Spacing();
            ImGui::Spacing();

            // ============================
            // CPU セクション
            // ============================
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), ICON_FA_MEMORY "  CPU タイムライン");
            ImGui::Separator();
            ImGui::Spacing();

            // CPU の計測項目がある場合は、履歴と現在値を表示する表を作る。
            if (!cpuDataMap_.empty()) {
                // CPU の表を開始できた場合にだけ記録ごとの行を描く。
                if (ImGui::BeginTable("CpuTimeline", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                    ImGui::TableSetupColumn("セクション", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                    ImGui::TableSetupColumn("現在値 (ms)", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                    ImGui::TableSetupColumn("平均 (ms)", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                    ImGui::TableSetupColumn("履歴 (120フレーム)", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();

                    for (auto& pair : cpuDataMap_) {
                        const std::string& name = pair.first;
                        auto& data = pair.second;

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(name.c_str());
                        
                        ImGui::TableSetColumnIndex(1);
                        ImVec4 valColor = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
                        // CPU の直近値が10msを超えたら黄色にする。
                        if (data.current > 10.0f)
                            valColor = ImVec4(0.8f, 0.8f, 0.2f, 1.0f);
                        // 直近値が16.66msを超えた場合は赤で表示する。
                        if (data.current > 16.66f)
                            valColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
                        ImGui::TextColored(valColor, "%.3f", data.current);

                        ImGui::TableSetColumnIndex(2);
                        ImVec4 avgColor = ImVec4(0.5f, 0.9f, 0.5f, 1.0f);
                        // CPU の平滑値が10msを超えたら黄色にする。
                        if (data.smoothed > 10.0f)
                            avgColor = ImVec4(0.8f, 0.8f, 0.2f, 1.0f);
                        // 平滑値が16.66msを超えた場合は赤を優先する。
                        if (data.smoothed > 16.66f)
                            avgColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
                        ImGui::TextColored(avgColor, "%.3f", data.smoothed);

                        ImGui::TableSetColumnIndex(3);
                        float plotData[kHistorySize];

                        for (std::int32_t i = 0; i < kHistorySize; i++) {
                            plotData[i] = data.history[(data.historyIndex + i) % kHistorySize];
                        }

                        ImGui::PushID(name.c_str());
                        ImGui::PlotLines("", plotData, kHistorySize, 0, nullptr, 0.0f, 20.0f, ImVec2(-1, 30));
                        ImGui::PopID();
                    }

                    ImGui::EndTable();
                }
            // CPU の計測をまだ記録していない場合は、空の表を出さずその旨を示す。
            } else {
                ImGui::TextDisabled("CPU計測データがありません");
            }

            ImGui::Spacing();
            ImGui::TextDisabled("(16.66ms = 60FPS基準)  現在値 = 生データ / 平均 = 移動平均");
        }
        // 個別負荷が選択中なら、GPU の収集操作、カテゴリ集計、各 Object の計測を表示する。
        else if (selectedIndex_ == 2) {
            // ---------------------------------------------------------
            // オブジェクト別負荷表示
            // ---------------------------------------------------------
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "[ オブジェクト別・GPU/CPU負荷 ]");
            ImGui::Separator();
            ImGui::Spacing();

            // --- サンプリング操作パネル ---
            // 詳細計測中は、個別負荷パネルの描画ごとに残りフレームを減らす。
            if (isGpuSampling_) {
                remainingSamplingFrames_--;
                ImGui::TextColored(ImVec4(1, 1, 0, 1), ICON_FA_SYNC " GPUサンプリング中... 残り %d フレーム", remainingSamplingFrames_);

                // 残りが0以下になった描画で収集を止め、項目ごとの平均を確定する。
                if (remainingSamplingFrames_ <= 0) {
                    isGpuSampling_ = false;

                    // 平均計算
                    // 期間内の総フレーム数ではなく、その項目を計測できた回数で割って1サンプルあたりのミリ秒を出します。
                    gpuSampleResult_.clear();

                    for (auto& pair : gpuSampleAccum_) {
                        gpuSampleResult_[pair.first] = pair.second / static_cast<float>(gpuSampleCount_[pair.first]);
                    }
                }
            }
            // 詳細計測が停止中の場合は開始操作を表示する。
            else {
                // 開始ボタンを押すと、GPU のクエリの記録を初期化して新しく収集する。
                if (ImGui::Button(ICON_FA_PLAY " GPU詳細サンプリング開始 (60フレーム)", ImVec2(-1, 30))) {
                    engine::DirectXCommon::GetInstance()->ResetGpuProfiles(); // クエリ上限対策でリセット
                    StartGpuSampling();
                }
            }

            ImGui::Spacing();

            // 対象リストが無いか空の場合は、詳細を集計できないことを表示する。
            if (!currentObjects_ || currentObjects_->empty()) {
                ImGui::TextDisabled("オブジェクトが登録されていません。");
            }
            // 対象が登録済みなら、カテゴリと個体ごとに内訳を並べる。
            else {
                // --- カテゴリ別の合計計算 ---
                std::map<std::string, float> catGpuTotal;
                std::map<std::string, float> catCpuTotal;
                std::map<std::string, std::int32_t> catCount;
                
                for (auto& obj : *currentObjects_) {
                    std::string cat = obj->GetSaveCategory();
                    catCount[cat]++;
                    catCpuTotal[cat] += obj->GetCpuUpdateTimeMs();

                    // GPU 合計には、名前に対応する詳細計測がある個体だけを加える。
                    if (gpuSampleResult_.count(obj->GetName())) {
                        catGpuTotal[cat] += gpuSampleResult_[obj->GetName()];
                    }
                }

                // 概要欄を開いたときに、分類別の個数と CPU・GPU の合計を表示する。
                if (ImGui::CollapsingHeader(ICON_FA_LIST " カテゴリ別・合計負荷サマリー", ImGuiTreeNodeFlags_DefaultOpen)) {
                    // 集計表を開始できた場合にだけ、カテゴリの行を作る。
                    if (ImGui::BeginTable("CategorySummary", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                        ImGui::TableSetupColumn("カテゴリ", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("個数", ImGuiTableColumnFlags_WidthFixed, 50.0f);
                        ImGui::TableSetupColumn("GPU合計 (ms)", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                        ImGui::TableSetupColumn("CPU合計 (ms)", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                        ImGui::TableHeadersRow();

                        for (auto& pair : catCount) {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::Text("%s", pair.first.c_str());
                            ImGui::TableNextColumn();
                            ImGui::Text("%d", pair.second);
                            ImGui::TableNextColumn();
                            ImGui::Text("%.3f", catGpuTotal[pair.first]);
                            ImGui::TableNextColumn();
                            ImGui::Text("%.3f", catCpuTotal[pair.first]);
                        }

                        ImGui::EndTable();
                    }
                }

                ImGui::Spacing();

                static ImGuiTableFlags sFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable;

                // 個体表を描画できる場合だけ、現在のオブジェクト一覧を表示用に整える。
                if (ImGui::BeginTable("ObjectPerfTable", 5, sFlags)) {
                    ImGui::TableSetupColumn("名前", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("クラス", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableSetupColumn("カテゴリ", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableSetupColumn("GPU (ms avg)", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthFixed, 100.0f);
                    ImGui::TableSetupColumn("CPU (ms)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableHeadersRow();

                    auto& objects = *currentObjects_;
                    // 表の並び替えは借用ポインタの配列で行い、シーンの所有配列と更新順は維持します。
                    std::vector<engine::Object3D*> sortedList;

                    for (auto& obj : objects)
                        sortedList.push_back(obj.get());

                    ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();

                    // ImGui が並べ替えの変更を通知した描画で、借用一覧を指定順に並べ替える。
                    if (sortSpecs && sortSpecs->SpecsDirty) {
                        std::sort(sortedList.begin(), sortedList.end(), [&](engine::Object3D* a, engine::Object3D* b) {
                            for (std::int32_t n = 0; n < sortSpecs->SpecsCount; n++) {
                                const ImGuiTableColumnSortSpecs* spec = &sortSpecs->Specs[n];
                                
                                // 名前の列の並べ替えには Object 名を使う。
                                if (spec->ColumnIndex == 0) { // 名前
                                    // 名前が違う場合は指定順で比較し、同名なら次の条件を調べる。
                                    if (a->GetName() != b->GetName())
                                        return (spec->SortDirection == ImGuiSortDirection_Ascending) ? (a->GetName() < b->GetName()) : (a->GetName() > b->GetName());
                                }
                                // クラス列には登録されたクラス名を使う。
                                else if (spec->ColumnIndex == 1) { // クラス
                                    // クラス名が違う場合は指定された並び順で比較する。
                                    if (a->GetClassName() != b->GetClassName())
                                        return (spec->SortDirection == ImGuiSortDirection_Ascending) ? (a->GetClassName() < b->GetClassName()) : (a->GetClassName() > b->GetClassName());
                                }
                                // カテゴリ列の並べ替えには保存カテゴリを使う。
                                else if (spec->ColumnIndex == 2) { // カテゴリ
                                    // 保存カテゴリが違う場合に、その列で順序を決める。
                                    if (a->GetSaveCategory() != b->GetSaveCategory())
                                        return (spec->SortDirection == ImGuiSortDirection_Ascending) ? (a->GetSaveCategory() < b->GetSaveCategory()) : (a->GetSaveCategory() > b->GetSaveCategory());
                                }
                                // GPU 列は収集した名前別の平均を使い、未計測の項目は比較上0とする。
                                else if (spec->ColumnIndex == 3) { // GPU
                                    float valA = gpuSampleResult_.count(a->GetName()) ? gpuSampleResult_[a->GetName()] : 0.0f;
                                    float valB = gpuSampleResult_.count(b->GetName()) ? gpuSampleResult_[b->GetName()] : 0.0f;
                                    // GPU 時間が異なる場合だけ、この条件で並び順を確定する。
                                    if (valA != valB)
                                        return (spec->SortDirection == ImGuiSortDirection_Ascending) ? (valA < valB) : (valA > valB);
                                }
                                // CPU 列は Object の現在の更新時間を使う。
                                else if (spec->ColumnIndex == 4) { // CPU
                                    float valA = a->GetCpuUpdateTimeMs();
                                    float valB = b->GetCpuUpdateTimeMs();
                                    // CPU 時間が異なる場合に、指定の昇順・降順で比較する。
                                    if (valA != valB)
                                        return (spec->SortDirection == ImGuiSortDirection_Ascending) ? (valA < valB) : (valA > valB);
                                }
                            }
                            return a < b;
                        });
                        sortSpecs->SpecsDirty = false;
                    }

                    for (auto* obj : sortedList) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        
                        // 名前をツリーノードにして、クリックで詳細を展開できるようにする
                        bool open = ImGui::TreeNodeEx(obj->GetName().c_str(), ImGuiTreeNodeFlags_SpanFullWidth);
                        
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%s", obj->GetClassName().c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(obj->GetSaveCategory().c_str());
                        
                        ImGui::TableNextColumn();

                        // 収集済みの GPU 時間がある場合は値を表示し、時間によって色を変える。
                        if (gpuSampleResult_.count(obj->GetName())) {
                            float gpuTime = gpuSampleResult_[obj->GetName()];
                            ImVec4 color = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
                            // GPU の平均が0.1msを超える個体を黄色にする。
                            if (gpuTime > 0.1f)
                                color = ImVec4(1, 1, 0, 1);
                            // 0.5msを超える個体では、黄色よりオレンジを優先する。
                            if (gpuTime > 0.5f)
                                color = ImVec4(1, 0.5f, 0, 1);
                            ImGui::TextColored(color, "%.3f", gpuTime);
                        // GPU の詳細計測結果を持たない Object では、値の代わりに未計測を示す。
                        } else {
                            ImGui::TextDisabled("---");
                        }

                        ImGui::TableNextColumn();
                        ImGui::Text("%.3f", obj->GetCpuUpdateTimeMs());

                        // 名前の行を開いた場合に、時間の内訳と描画負荷に関係する設定を追加表示する。
                        if (open) {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            
                            ImGui::Indent();
                            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), ICON_FA_INFO_CIRCLE " 処理の内訳 (CPU)");
                            ImGui::BulletText("Animation: %.3f ms", obj->GetCpuAnimTimeMs());
                            ImGui::BulletText("Update/Matrix: %.3f ms", obj->GetCpuMatrixTimeMs());
                            
                            ImGui::Spacing();
                            
                            // モデル情報の取得
                            engine::Model* model = obj->GetModel();
                            
                            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), ICON_FA_CUBE " モデルスペック (GPU負荷要因)");

                            // モデルが読み込まれている場合だけ、メッシュ・ポリゴン・頂点の数を表示する。
                            if (model) {
                                ImGui::BulletText("ポリゴン数: %u", model->GetPolygonCount());
                                ImGui::BulletText("頂点数: %u", model->GetVertexCount());
                                ImGui::BulletText("メッシュ数: %u", model->GetMeshCount());
                            // モデルを取得できない場合は、描画資源の件数に代えて未ロードと表示する。
                            } else {
                                ImGui::BulletText("モデル未ロード");
                            }

                            ImGui::Spacing();
                            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), ICON_FA_PAINT_BRUSH " マテリアル構成");
                            ImGui::BulletText("ライティング: %s", obj->GetEnableLighting() ? "ON" : "OFF");
                            ImGui::BulletText("法線マップ: %s", obj->GetEnableNormalMap() ? "ON" : "OFF");
                            ImGui::BulletText("環境マップ: %s", obj->GetEnableEnvMap() ? "ON" : "OFF");
                            
                            ImGui::Unindent();
                            ImGui::TreePop();
                        }
                    }

                    ImGui::EndTable();
                }
            }
        }

        ImGui::EndChild();
    }

    ImGui::End();
#endif
}
