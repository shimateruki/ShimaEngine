#include <cstdint>
#include "Log.h"
#include <fstream>
#include <Windows.h>

const std::string kLogFileName = "GameLog.txt";
static std::ofstream sOfs;

// ファイル保存はアプリ側から有効化されるまでは行わない。
bool Log::sIsFileEnabled = false;

void Log::WriteInternal(LogLevel level, const std::string& file, std::int32_t line, const std::string& message) {

    // 発生元は末尾のファイル名と行番号で示す。
    std::filesystem::path p(file);
    std::string filename = p.filename().string();

    // エラー、警告、情報の順に重大度をラベルへ変換し、一行にまとめる。
    const char* levelTag = "[INFO] ";
    if (level == LogLevel::kError) {
        levelTag = "[ERROR]";
    } else if (level == LogLevel::kWarning) {
        levelTag = "[WARN] ";
    }
    std::string formattedLog = std::format("{}({}): {}{}\\n", filename, line, levelTag, message);

    // ファイル保存が有効な間だけGameLog.txtへ書き込む。
    if (sIsFileEnabled) {
        // ログファイルをまだ開いていなければ、書き込みに備えてここで開く。
        if (!sOfs.is_open()) {
            // 初回の出力時にファイルを開きます。出力モードのみで開くため前回の実行ログはここで切り詰められます。
            sOfs.open(kLogFileName, std::ios::out);
        }

        // ファイルを開けた場合だけ、今回の一行を書いてバッファをフラッシュする。
        if (sOfs.is_open()) {
            sOfs << formattedLog;
            sOfs.flush();
        }
    }

    // ファイル出力の成否にかかわらず、Visual Studioなどのデバッガーへ同じ行を送る。
    OutputDebugStringA(formattedLog.c_str());

}
