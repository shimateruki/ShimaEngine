#define NOMINMAX
#include "AssetDatabase.h"
#include "AssetDatabase.Internal.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;
using json = nlohmann::json;

using namespace assetDatabaseInternal;

namespace {
std::uintmax_t gDdsNotificationSizeBeforeBuild = 0;
}

bool AssetDatabase::StartDdsCacheBuild() {
    // DDS 変換をエディタープロセスから分離して起動します。
    CloseDdsCacheProcessHandle();

    const fs::path scriptPath = (projectRoot_ / "tools/dds_cache/dds_cache_builder.ps1").lexically_normal();
    std::error_code error;

    // 変換スクリプトがないか存在確認に失敗した場合は、プロセスを開始せず失敗を記録する。
    if (!fs::exists(scriptPath, error) || error) {
        ddsCacheBuildState_ = DdsCacheBuildState::kFailed;
        ddsCacheBuildMessage_ = "DDS変換スクリプトが見つかりません。";
        lastDdsCacheBuildExitCode_ = ERROR_FILE_NOT_FOUND;
        return false;
    }

    const fs::path notificationPath = resourcesRoot_ / ".cache/dds_cache_notifications.jsonl";
    // 前回分の通知を今回の変換結果と取り違えないよう、開始前の末尾位置を控える。
    gDdsNotificationSizeBeforeBuild = 0;

    // 起動前に通知ファイルが存在し照会できた場合だけ、前回分の長さを取得する。
    if (fs::exists(notificationPath, error) && !error) {
        gDdsNotificationSizeBeforeBuild = fs::file_size(notificationPath, error);

        // ファイルサイズを取得できなければ、比較の開始位置を0バイトへ戻す。
        if (error) {
            gDdsNotificationSizeBeforeBuild = 0;
            error.clear();
        }
    }

    std::wstring commandLine =
        L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" +
        scriptPath.wstring() + L"\" -Root \"" + resourcesRoot_.wstring() + L"\"";

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startupInfo.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION processInfo{};
    // CreateProcessWはコマンドラインを書き換えるため、変更可能な文字列を渡す。
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    const BOOL created = CreateProcessW(
        nullptr,
        mutableCommand.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        projectRoot_.c_str(),
        &startupInfo,
        &processInfo);
    // 外部プロセスを起動できなければ、Windowsの失敗コードを保持して画面へ返す。
    if (!created) {
        lastDdsCacheBuildExitCode_ = GetLastError();
        ddsCacheBuildState_ = DdsCacheBuildState::kFailed;
        ddsCacheBuildMessage_ = "DDS変換プロセスを開始できませんでした。";
        return false;
    }

    // スレッドハンドルは起動直後に閉じ、終了コードの回収に必要なプロセスハンドルだけ保持する。
    CloseHandle(processInfo.hThread);
    ddsCacheProcessHandle_ = processInfo.hProcess;
    lastDdsCacheBuildExitCode_ = STILL_ACTIVE;
    ddsCacheBuildState_ = DdsCacheBuildState::kRunning;
    ddsCacheBuildMessage_ = "未変換・更新済みでない画像をバックグラウンドで確認しています。";
    return true;
}

bool AssetDatabase::PollDdsCacheBuild() {
    // 終了するまで待機せず、Update() から状態だけを回収します。
    if (ddsCacheBuildState_ != DdsCacheBuildState::kRunning || !ddsCacheProcessHandle_) {
        return false;
    }

    HANDLE process = static_cast<HANDLE>(ddsCacheProcessHandle_);
    // 待機時間は0ミリ秒。変換中でもエディターのフレームを止めない。
    const DWORD waitResult = WaitForSingleObject(process, 0);

    // 待機時間内に終了通知が来なければまだ変換中として、次のフレームで確認する。
    if (waitResult == WAIT_TIMEOUT) {
        return false;
    }

    DWORD exitCode = 1;

    // 終了が通知された場合は、そのプロセスの終了コードを回収する。
    if (waitResult == WAIT_OBJECT_0) {
        GetExitCodeProcess(process, &exitCode);
    }
    // 終了通知以外の結果は監視失敗として、最後のWindowsエラーを保持する。
    else {
        exitCode = GetLastError();
    }

    lastDdsCacheBuildExitCode_ = exitCode;
    CloseDdsCacheProcessHandle();

    // 変換通知が増えた場合だけ、追加されたDDSをAsset Databaseへ取り込む。
    // 全DDSが最新だった場合の無駄な二回目の全索引を避ける。
    const fs::path notificationPath = resourcesRoot_ / ".cache/dds_cache_notifications.jsonl";
    std::error_code error;
    std::uintmax_t notificationSizeAfterBuild = 0;

    // 変換後に通知ファイルを照会できた場合だけ、比較用のバイト数を取り出す。
    if (fs::exists(notificationPath, error) && !error) {
        notificationSizeAfterBuild = fs::file_size(notificationPath, error);

        // 変換後のサイズが取れない場合は0とし、通知増加を確定しない。
        if (error) {
            notificationSizeAfterBuild = 0;
        }
    }

    // 通知ファイルのバイト数を起動前と比べ、増えたときだけ索引更新を予約する。
    const bool generatedDds = notificationSizeAfterBuild > gDdsNotificationSizeBeforeBuild;
    refreshAfterDdsCacheBuild_ = generatedDds;

    // プロセスの終了通知と終了コード0が揃った場合を成功とする。
    if (waitResult == WAIT_OBJECT_0 && exitCode == 0) {
        ddsCacheBuildState_ = DdsCacheBuildState::kSucceeded;
        ddsCacheBuildMessage_ = generatedDds
            ? "DDSキャッシュの確認・変換が完了しました。"
            : "DDSキャッシュは最新です。再索引は不要です。";
    }
    // 終了を確認できないか非ゼロで終わった場合は、取得した番号を失敗として表示する。
    else {
        ddsCacheBuildState_ = DdsCacheBuildState::kFailed;
        ddsCacheBuildMessage_ = "DDS変換に失敗しました。Exit Code: " + std::to_string(exitCode);
    }

    return true;
}

void AssetDatabase::CloseDdsCacheProcessHandle() {
    // 保持するプロセスハンドルがなければ、そのまま返す。ハンドルを閉じる処理自体はプロセスを終了させない。
    if (!ddsCacheProcessHandle_) {
        return;
    }

    CloseHandle(static_cast<HANDLE>(ddsCacheProcessHandle_));
    ddsCacheProcessHandle_ = nullptr;
}
