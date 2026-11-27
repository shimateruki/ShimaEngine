#include "DebugConsole.h"
#include <Windows.h> 

DebugConsole* DebugConsole::GetInstance() {
    static DebugConsole sInstance;
    return &sInstance;
}

void DebugConsole::Initialize() {
    // エラー総数を先に初期化します。履歴の取り出しと同期させる用途はなく、件数の読み書きにだけ原子操作を使います。
    errorCount_.store(0, std::memory_order_relaxed);
#ifdef USE_IMGUI
    std::lock_guard<std::mutex> lock(logMutex_);
    logs_.clear();
    scrollToBottom_ = true;
#endif
}

void DebugConsole::Finalize() {
#ifdef USE_IMGUI
    std::lock_guard<std::mutex> lock(logMutex_);
    logs_.clear();
#endif
}

void DebugConsole::AddLog(const std::string& log) {
#ifdef USE_IMGUI
    // 本文中のレベル表記で既存の文字列ログを分類し、型付きログと同じ保存処理へ渡す。
    LogLevel level = LogLevel::kInfo;
    // 文字列にエラーの印があれば、警告より優先してエラーとして保存する。
    if (log.find("[ERROR]") != std::string::npos)
        level = LogLevel::kError;
    // エラーの印がなく警告の印があれば、警告として保存する。
    else if (log.find("[WARN]") != std::string::npos) level = LogLevel::kWarning;

    AddLog(level, log);
#endif
}

void DebugConsole::AddLog(LogLevel level, const std::string& log) {
#ifdef USE_IMGUI
    // ログ数を上限に保ち、表示とVisual Studioの出力へ同じ本文を渡す。
    std::lock_guard<std::mutex> lock(logMutex_);

    // エラーを受け取った場合だけ累計件数を増やす。表示履歴から古いログが消えても件数は残る。
    if (level == LogLevel::kError) {
        errorCount_.fetch_add(1, std::memory_order_relaxed);
    }


    // 上限は表示履歴だけに適用します。古い行を除いても、別に持つエラー累積数は減らしません。
    const size_t maxLogs = 200;

    while (logs_.size() >= maxLogs) {
        logs_.pop_front();
    }

    // 表示履歴の上限に空きを作ったら、受け取ったログを末尾へ追加する。
    logs_.push_back({ log, level });

    // 自動スクロールが有効な場合は、次の描画で新しいログ位置まで追従する。
    if (autoScroll_) {
        scrollToBottom_ = true;
    }

    std::string outStr = log;
    // 本文末尾に改行がない場合は、Visual Studioの出力で次のログとつながらないよう改行を補う。
    if (outStr.empty() || outStr.back() != '\n')
        outStr += "\n";
    OutputDebugStringA(outStr.c_str());
#endif
}

void DebugConsole::DrawImGui() {
#ifdef USE_IMGUI
    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);

    // ウィンドウが折りたたまれたなどの理由で中身を描画できない場合は、対応するEndを呼んで戻る。
    if (!ImGui::Begin("Debug Console")) {
        ImGui::End();
        return;
    }

    // クリアが押されたときは、書き込みとの競合を防いで表示履歴を消す。
    if (ImGui::Button("Clear")) {
        std::lock_guard<std::mutex> lock(logMutex_);
        logs_.clear();
    }

    ImGui::SameLine();
    bool copy = ImGui::Button("Copy");
    ImGui::SameLine();

    // フィルタの幅を少し調整
    filter_.Draw("Filter", -200.0f);

    //  ログレベル・トグルボタン
    ImGui::SameLine();
    ImGui::Checkbox("Info", &showInfo_);
    ImGui::SameLine();
    ImGui::Checkbox("Warn", &showWarn_);
    ImGui::SameLine();
    ImGui::Checkbox("Error", &showError_);

    ImGui::Separator();

    // --- スクロール領域 ---
    ImGui::BeginChild("LogScrollingRegion", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), false, ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lock(logMutex_);

        // コピーが押されたときだけ、検索条件と表示レベルを通ったログをクリップボードへ送る。
        if (copy) {
            std::string clipboardText;

            for (const auto& entry : logs_) {
                // 検索欄の条件を通らないログはコピーしない。
                if (!filter_.PassFilter(entry.message.c_str()))
                    continue;
                // 情報レベルを非表示にしている場合は、情報ログをコピーしない。
                if (entry.level == LogLevel::kInfo && !showInfo_)
                    continue;
                // 警告レベルを非表示にしている場合は、警告ログをコピーしない。
                if (entry.level == LogLevel::kWarning && !showWarn_)
                    continue;
                // エラーレベルを非表示にしている場合は、エラーログをコピーしない。
                if (entry.level == LogLevel::kError && !showError_)
                    continue;

                clipboardText += entry.message + "\n";
            }

            ImGui::SetClipboardText(clipboardText.c_str());
        }

        // ログ描画ループ
        for (const auto& entry : logs_) {
            // 検索欄の条件を通らないログは画面に出さない。
            if (!filter_.PassFilter(entry.message.c_str()))
                continue;

            // 情報レベルが無効なら、対応するログの描画を飛ばす。
            if (entry.level == LogLevel::kInfo && !showInfo_)
                continue;
            // 警告レベルが無効なら、対応するログの描画を飛ばす。
            if (entry.level == LogLevel::kWarning && !showWarn_)
                continue;
            // エラーレベルが無効なら、対応するログの描画を飛ばす。
            if (entry.level == LogLevel::kError && !showError_)
                continue;

            // エラーと警告を色付きにし、それ以外はテーマの既定の文字色で表示する。
            bool hasColor = true;
            ImVec4 color;

            switch (entry.level) {
            case LogLevel::kError:   color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); break; // 赤

            case LogLevel::kWarning: color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f); break; // 黄

            default:                hasColor = false; break;
            }

            // エラーか警告に個別色がある場合だけ、描画直前に文字色を上書きする。
            if (hasColor)
                ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextUnformatted(entry.message.c_str());
            // 色を上書きした場合は、次のログへ設定が残らないよう直後に元へ戻す。
            if (hasColor)
                ImGui::PopStyleColor();
        }

        // スクロールの予約があれば今回の描画で最下部へ移し、予約を消費する。
        if (scrollToBottom_) {
            ImGui::SetScrollHereY(1.0f);
            scrollToBottom_ = false;
        }
    }

    ImGui::EndChild();

    // --- フッター ---
    ImGui::Separator();
    ImGui::Checkbox("Auto-scroll", &autoScroll_);

    ImGui::End();
#endif
}
