#define NOMINMAX
#include "ExecutablePackageWindow.h"
#include "ExecutablePackageWindow.Internal.h"
#include "RuntimeResourcePlan.h"

#include "IconsFontAwesome5.h"
#include "imgui.h"
#include "json.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

using executablePackageInternal::kConfigurations;
using executablePackageInternal::kTextureModeLabels;
using executablePackageInternal::TexturePackageMode;

namespace {
// 配布用 Resources のコピー量と除外理由別の集計。Bytes 系は圧縮前のバイト数。
struct ResourceCopyStats {
    uint64_t sourceFiles = 0;
    uint64_t sourceBytes = 0;
    uint64_t copiedFiles = 0;
    uint64_t copiedBytes = 0;
    uint64_t skippedFiles = 0;
    uint64_t skippedBytes = 0;
    uint64_t skippedDirectories = 0;
    uint64_t texturePairs = 0;
    uint64_t omittedTextureFiles = 0;
    uint64_t omittedTextureBytes = 0;
    uint64_t omittedUnusedFiles = 0;
    uint64_t omittedUnusedBytes = 0;
};

// プロジェクトコピーの結果集計。コピー・除外したファイル数と圧縮前バイト数を分ける。
struct ProjectCopyStats {
    uint64_t copiedFiles = 0;
    uint64_t copiedBytes = 0;
    uint64_t skippedFiles = 0;
    uint64_t skippedBytes = 0;
    uint64_t skippedDirectories = 0;
};

// コピー元の実パスと配布先に使う相対パス、容量見積り用のバイト数。
struct ResourceFile {
    fs::path source;
    fs::path relative;
    uint64_t bytes = 0;
};

// 途中失敗したときだけ一時ディレクトリを片付け、完成済みの出力は残します。
class ScopedDirectoryCleanup {
public:
    explicit ScopedDirectoryCleanup(fs::path path) : path_(std::move(path)) {}
    ~ScopedDirectoryCleanup() {
        // 正式名への移動が済んで削除予約が解除されていれば、作業ディレクトリの削除は行いません。
        if (!active_)
            return;
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    void Release() { active_ = false; }

private:
    fs::path path_;
    bool active_ = true;
};

std::wstring Utf8ToWide(const std::string& text) {
    // UTF-8 を優先し、旧来のANSI文字列だけ既定コードページで読み直す。
    // 空文字はコードページ変換を呼ばず、そのまま空として返します。
    if (text.empty()) {
        return {};
    }

    std::int32_t size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<std::int32_t>(text.size()), nullptr, 0);

    // UTF-8として必要文字数を得られなければ、既定のANSIコードページで試し直します。
    if (size <= 0) {
        size = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<std::int32_t>(text.size()), nullptr, 0);

        // 既定コードページでも変換できなければ、空の結果を返します。
        if (size <= 0) {
            return {};
        }

        std::wstring wide(size, L'\0');
        MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<std::int32_t>(text.size()), wide.data(), size);
        return wide;
    }

    std::wstring wide(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<std::int32_t>(text.size()), wide.data(), size);
    return wide;
}

std::string WideToUtf8(const std::wstring& text) {
    // 配布ファイル名をWindowsのワイド文字列からUTF-8へ変換する。
    if (text.empty()) {
        return {};
    }

    std::int32_t size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<std::int32_t>(text.size()), nullptr, 0, nullptr, nullptr);

    // UTF-8に必要なバイト数を得られなければ、変換先を確保せずに終了します。
    if (size <= 0) {
        return {};
    }

    std::string utf8(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<std::int32_t>(text.size()), utf8.data(), size, nullptr, nullptr);
    return utf8;
}

std::string PathToUtf8(const fs::path& path) {
    return WideToUtf8(path.wstring());
}

fs::path PathFromUtf8(const std::string& text) {
    return fs::path(Utf8ToWide(text));
}

std::string TrimAscii(const std::string& text) {
    // 配布物名の前後に混入したASCII空白だけを除く。
    size_t begin = 0;

    while (begin < text.size() && static_cast<unsigned char>(text[begin]) <= 0x20) {
        ++begin;
    }

    size_t end = text.size();

    while (end > begin && static_cast<unsigned char>(text[end - 1]) <= 0x20) {
        --end;
    }

    return text.substr(begin, end - begin);
}

std::string ToUpperAscii(std::string text) {
    for (char& c : text) {
        // ASCIIの小文字だけを大文字にし、UTF-8の他のバイトは保持します。
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }

    return text;
}

bool IsReservedWindowsName(const std::string& name) {
    // 拡張子があってもWindowsの予約名はファイル名として使えないため先頭部分で判定する。
    std::string stem = name;
    const size_t dot = stem.find('.');

    // 拡張子がある場合は、最初のピリオドより前で予約名を照合します。
    if (dot != std::string::npos) {
        stem = stem.substr(0, dot);
    }

    stem = ToUpperAscii(stem);

    static constexpr std::array<const char*, 22> kReserved = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };

    return std::find(kReserved.begin(), kReserved.end(), stem) != kReserved.end();
}

std::string SanitizePackageName(const std::string& input) {
    // Windows で使えない名前と拡張子を除き、UI のプレビューにも同じ規則を適用します。
    std::string name = TrimAscii(input);
    const std::string exeSuffix = ".exe";

    // 拡張子より長い名前なら、末尾が大文字小文字を問わず .exe か確認します。
    if (name.size() > exeSuffix.size()) {
        std::string tail = name.substr(name.size() - exeSuffix.size());

        // 末尾が .exe なら取り除き、呼び出し側が付ける拡張子との重複を防ぎます。
        if (ToUpperAscii(tail) == ".EXE") {
            name.resize(name.size() - exeSuffix.size());
        }
    }

    for (char& c : name) {
        const unsigned char byte = static_cast<unsigned char>(c);
        // 制御文字、Windowsで使えない記号、起動バッチで展開される % は下線に置き換えます。
        if (byte < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*' || c == '%') {
            c = '_';
        }
    }

    // Windowsのファイル名の末尾で扱えない空白とピリオドは取り除きます。
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
        name.pop_back();
    }

    // 正規化の結果が空欄または特殊な相対名なら、既定の名前を使います。
    if (name.empty() || name == "." || name == "..") {
        name = "GE3_Playable";
    }

    // 予約名に一致した場合は、先頭に下線を付けて有効な名前にします。
    if (IsReservedWindowsName(name)) {
        name = "_" + name;
    }

    return name;
}

fs::path GetExecutableDirectory() {
    // 実行中のEXEを基準に付属ファイルの配置先を決める。
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));

    // Win32 APIの戻り値がバッファ長と同じ場合は、パスが収まるまで領域を広げます。
    while (length == buffer.size()) {
        buffer.resize(buffer.size() * 2);
        length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }

    // 実行ファイルのパスを取得できなければ、現在の作業フォルダーを使います。
    if (length == 0) {
        return fs::current_path();
    }

    buffer.resize(length);
    return fs::path(buffer).parent_path();
}

bool LooksLikeProjectRoot(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path / "ShimaEngine.sln", ec) && fs::exists(path / "Resources", ec);
}

fs::path FindProjectRoot() {
    // 作業ディレクトリと実行ファイル位置の両方からソリューションを探します。
    std::vector<fs::path> seeds;
    std::error_code ec;
    seeds.push_back(fs::current_path(ec));
    seeds.push_back(GetExecutableDirectory());

    for (const fs::path& seed : seeds) {
        // 作業場所を取得できなかった候補は探索しません。
        if (seed.empty()) {
            continue;
        }

        fs::path cursor = seed;

        for (std::int32_t i = 0; i < 8 && !cursor.empty(); ++i) {
            // 現在の候補にソリューションとResourcesがそろっていれば、その絶対位置を採用します。
            if (LooksLikeProjectRoot(cursor)) {
                return fs::weakly_canonical(cursor, ec);
            }

            const fs::path projectChild = cursor / "project";

            // 候補自体が一致しなければ、その直下の project も確認します。
            if (LooksLikeProjectRoot(projectChild)) {
                return fs::weakly_canonical(projectChild, ec);
            }

            // 親が無いか同じパスへ戻る位置まで来たら、それ以上は遡りません。
            if (!cursor.has_parent_path() || cursor == cursor.parent_path()) {
                break;
            }

            cursor = cursor.parent_path();
        }
    }

    return {};
}

bool IsInsideDirectory(const fs::path& root, const fs::path& target) {
    // 正規化後の相対パスで親への脱出を検出し、出力先の誤削除を防ぐ。
    std::error_code ec;
    fs::path canonicalRoot = fs::weakly_canonical(root, ec);

    // 基準ディレクトリを正規化できない場合、範囲内と判定しません。
    if (ec) {
        return false;
    }

    fs::path canonicalTarget = fs::weakly_canonical(target, ec);

    // 出力候補の正規化に失敗した場合も、範囲内とは扱いません。
    if (ec) {
        return false;
    }

    fs::path relative = fs::relative(canonicalTarget, canonicalRoot, ec);

    // 相対パスを取れないか空の場合は、正規化済みのパスそのものが等しいかを確認します。
    if (ec || relative.empty()) {
        return canonicalTarget == canonicalRoot;
    }

    for (const fs::path& part : relative) {
        // 相対パスに親へ遡る要素があれば、基準の外側と判定します。
        if (part == "..") {
            return false;
        }
    }

    return true;
}

bool ShouldSkipResourceDirectory(const fs::path& name) {
    std::wstring wide = name.wstring();
    std::transform(wide.begin(), wide.end(), wide.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return wide == L".cache" || wide == L".backup" || wide == L".trash" || wide == L"tools";
}

std::wstring ToLowerWide(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return text;
}

bool IsEditorGeneratedDirectory(const fs::path& relative) {
    const std::wstring normalized = ToLowerWide(relative.generic_wstring());
    return normalized == L"generated/editor" || normalized.rfind(L"generated/editor/", 0) == 0;
}

bool HasPathPrefix(const std::wstring& normalized, const std::wstring& prefix) {
    return normalized == prefix || normalized.rfind(prefix + L"/", 0) == 0;
}

bool IsProjectDirectoryExcluded(const fs::path& relative) {
    // 提出元の project から生成物と開発端末固有のフォルダを除外します。
    const std::wstring normalized = ToLowerWide(relative.generic_wstring());
    static constexpr std::array<const wchar_t*, 11> kExcludedRoots = {
        L".vs",
        L".git",
        L".agents",
        L".codex",
        L"output",
        L"tmp",
        L"debug",
        L"development",
        L"release",
        L"executablesets",
        L"submissionpackages"
    };

    for (const wchar_t* root : kExcludedRoots) {
        // 提出から除くルートとその配下に一致した場合は走査対象にしません。
        if (HasPathPrefix(normalized, root)) {
            return true;
        }
    }

    // 素材の作業キャッシュや生成済みエディター資産も、ソース同梱から外します。
    if (HasPathPrefix(normalized, L"resources/.cache") ||
        HasPathPrefix(normalized, L"resources/.backup") ||
        HasPathPrefix(normalized, L"resources/.trash") ||
        HasPathPrefix(normalized, L"resources/generated/editor") ||
        HasPathPrefix(normalized, L"externals/.cache")) {
        return true;
    }

    for (const fs::path& part : relative) {
        // 階層のどの位置でも、Pythonのキャッシュを含むディレクトリは除外します。
        if (ToLowerWide(part.wstring()) == L"__pycache__") {
            return true;
        }
    }

    return false;
}

bool IsProjectFileExcluded(const fs::path& relative, uint64_t fileBytes, ProjectCopyStats& stats) {
    // 配布に不要な端末固有ファイルと一時生成物を除外し、除外量も集計する。
    const std::wstring fileName = ToLowerWide(relative.filename().wstring());
    const std::wstring extension = ToLowerWide(relative.extension().wstring());
    const bool rootLocalState = relative.parent_path().empty() &&
        (fileName == L"imgui.ini" ||
         fileName == L"nodeeditor.json" ||
         fileName == L"shader_compile.log" ||
         fileName == L"system.drawing.drawing2d.graphicspath" ||
         fileName == L"スプリント振り返り_2026-07-10.md");
    const bool generatedFile =
        fileName == L"thumbs.db" ||
        fileName == L".ds_store" ||
        extension == L".user" ||
        extension == L".suo" ||
        extension == L".opendb" ||
        extension == L".ipch" ||
        extension == L".pyc" ||
        extension == L".pyo" ||
        extension == L".log" ||
        extension == L".tmp";
    // 端末固有設定にも一時生成物にも該当しない場合は、コピー候補に残します。
    if (!rootLocalState && !generatedFile) {
        return false;
    }

    ++stats.skippedFiles;
    stats.skippedBytes += fileBytes;
    return true;
}

bool IsReparsePoint(const fs::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// シンボリックリンクや除外対象を踏まず、提出用の project をコピーします。
bool CopyProjectFiltered(
    const fs::path& source,
    const fs::path& destination,
    ProjectCopyStats& stats,
    std::string& errorMessage) {
    std::error_code ec;

    // コピー元が見つからないかフォルダーでなければ提出用のツリーを作りません。
    if (!fs::exists(source, ec) || !fs::is_directory(source, ec)) {
        errorMessage = "プロジェクトフォルダが見つかりません: " + PathToUtf8(source);
        return false;
    }

    fs::create_directories(destination, ec);

    // コピー先を作成しようとしてエラーが出た場合は、走査へ進まず失敗理由を返します。
    if (ec) {
        errorMessage = "projectコピー先を作成できません: " + PathToUtf8(destination);
        return false;
    }

    // 提出対象の project を相対Pathで再構成し、除外フォルダの内側へは降りません。
    fs::recursive_directory_iterator it(source, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;

    // コピー元を開けなければ、提出用ツリーのコピーを開始できません。
    if (ec) {
        errorMessage = "プロジェクトを走査できません: " + ec.message();
        return false;
    }

    for (; it != end; it.increment(ec)) {
        // コピー元の走査でエラーが出た場合、完全な提出物として扱わないよう中止します。
        if (ec) {
            errorMessage = "プロジェクトコピー中に走査エラーが発生しました: " + ec.message();
            return false;
        }

        const fs::path entryPath = it->path();
        const fs::path relative = fs::relative(entryPath, source, ec);

        // 相対パスに変換できない項目があれば、正しいコピー先を決められないため中止します。
        if (ec) {
            errorMessage = "プロジェクト内の相対パスを作れません: " + ec.message();
            return false;
        }

        // リパースポイントを辿ると元プロジェクト外へ出る可能性があるため再帰を止めます。
        // ディレクトリは再帰走査に任せ、リンクや除外先なら内側へ降りません。
        if (it->is_directory(ec)) {
            // リンクまたは除外対象のフォルダーだけを集計し、配下を走査しません。
            if (IsReparsePoint(entryPath) || IsProjectDirectoryExcluded(relative)) {
                ++stats.skippedDirectories;
                it.disable_recursion_pending();
            }

            continue;
        }

        // 通常ファイル以外とリンクは、提出用ツリーへ複製しません。
        if (!it->is_regular_file(ec) || IsReparsePoint(entryPath)) {
            continue;
        }

        const uint64_t bytes = static_cast<uint64_t>(it->file_size(ec));

        // サイズを取得できなければ、対象名を返して集計とコピーを中止します。
        if (ec) {
            errorMessage = "プロジェクトファイルのサイズを取得できません: " + PathToUtf8(entryPath);
            return false;
        }

        // 生成物や端末固有ファイルは集計だけしてコピー対象から外します。
        if (IsProjectFileExcluded(relative, bytes, stats)) {
            continue;
        }

        const fs::path target = destination / relative;
        fs::create_directories(target.parent_path(), ec);

        // コピー先の親フォルダーを作れなければ、ファイルの複製へ進みません。
        if (ec) {
            errorMessage = "project内のコピー先を作成できません: " + PathToUtf8(target.parent_path());
            return false;
        }

        fs::copy_file(entryPath, target, fs::copy_options::overwrite_existing, ec);

        // 複製に失敗した場合は、成功件数に加えず中断します。
        if (ec) {
            errorMessage = "プロジェクトファイルをコピーできません: " + PathToUtf8(entryPath);
            return false;
        }

        ++stats.copiedFiles;
        stats.copiedBytes += bytes;
    }

    return true;
}

void AccumulateSkippedDirectory(const fs::path& directory, ResourceCopyStats& stats) {
    // 丸ごと除外したディレクトリも元容量へ算入し、削減量を正しく示す。
    std::error_code ec;
    fs::recursive_directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;

    // 除外先の中身を取得できなければ、集計できる容量だけで処理を続けます。
    if (ec) {
        return;
    }

    for (; it != end; it.increment(ec)) {
        // 途中で読めない項目はエラーを解除し、この除外容量の集計から外します。
        if (ec) {
            ec.clear();
            continue;
        }

        // 通常ファイルでなければ、除外されたファイル数へ加えません。
        if (!it->is_regular_file(ec)) {
            ec.clear();
            continue;
        }

        const uint64_t bytes = static_cast<uint64_t>(it->file_size(ec));

        // 容量を読めないファイルは、件数・容量どちらにも加えません。
        if (ec) {
            ec.clear();
            continue;
        }

        ++stats.sourceFiles;
        stats.sourceBytes += bytes;
        ++stats.skippedFiles;
        stats.skippedBytes += bytes;
    }
}

// Editor上の数値を有効なテクスチャ梱包モードへ丸めて変換します。
TexturePackageMode ToTexturePackageMode(std::int32_t value) {
    return static_cast<TexturePackageMode>(std::clamp(value, 0, static_cast<std::int32_t>(TexturePackageMode::kComplete)));
}

const char* GetTextureModeId(TexturePackageMode mode) {
    // 梱包モードを、配布レポートで安定して使う文字列へ変換します。
    switch (mode) {
    case TexturePackageMode::kCompactPng: return "compact_png";

    case TexturePackageMode::kFastDds: return "fast_dds";

    case TexturePackageMode::kComplete: return "complete";
    }

    return "compact_png";
}

std::wstring GetTexturePairKey(const fs::path& relative) {
    fs::path base = relative;
    base.replace_extension();
    return ToLowerWide(base.generic_wstring());
}

// 画像の本体だけでペアを作り、除外済み画像や.metaで相方の有無を誤認しないようにします。
std::unordered_map<std::wstring, uint8_t> BuildTexturePairs(
    const std::vector<ResourceFile>& files, const std::unordered_set<std::wstring>& excluded) {
    std::unordered_map<std::wstring, uint8_t> pairs;
    for (const auto& file : files) {
        // 依存解析で省く画像は、代替画像の有無を決めるペアにも使いません。
        if (excluded.contains(ToLowerWide(file.relative.generic_wstring()))) continue;
        const auto extension = ToLowerWide(file.relative.extension().wstring());
        // 対象がDDSかPNGなら、同じ名前に対して存在する形式を別々のビットに記録します。
        if (extension == L".dds") pairs[GetTexturePairKey(file.relative)] |= 1;
        if (extension == L".png") pairs[GetTexturePairKey(file.relative)] |= 2;
    }
    return pairs;
}

// 実コピーと解析だけのプレビューが、画像とその.metaを同じ条件で省くための判定です。
bool OmitTexture(const fs::path& relative, TexturePackageMode mode,
                 const std::unordered_map<std::wstring, uint8_t>& pairs) {
    fs::path texture = relative;
    // 画像の .meta は、その画像と同じ判断を使うよう外側の拡張子を除きます。
    if (texture.extension() == ".meta") texture.replace_extension();
    const auto found = pairs.find(GetTexturePairKey(texture));
    // 同じ位置にDDSとPNGの両方がそろっていなければ、元の画像を保持します。
    if (found == pairs.end() || found->second != 3) return false;
    const auto extension = ToLowerWide(texture.extension().wstring());
    return (mode == TexturePackageMode::kCompactPng && extension == L".dds") ||
           (mode == TexturePackageMode::kFastDds && extension == L".png");
}

nlohmann::json BuildCopyReport(const engine::RuntimeResourcePlan& plan, TexturePackageMode mode) {
    auto report = plan.report;
    std::vector<ResourceFile> files;
    for (const auto& entry : report.at("files"))
        files.push_back({{}, PathFromUtf8(entry.at("path").get<std::string>()), entry.at("bytes").get<uint64_t>()});
    const auto pairs = BuildTexturePairs(files, plan.excludedPaths);
    uint64_t included = 0, includedBytes = 0, excludedBytes = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        auto& entry = report["files"][i];
        // 依存に必要でも、代替する画像形式が残る場合は最終コピーから省けます。
        entry["requiredByDependencies"] = entry.at("included");
        // 依存上必要な画像でも、選択する画像モードで有効な相方が残る場合は配布から省きます。
        if (entry.at("included").get<bool>() && OmitTexture(files[i].relative, mode, pairs)) {
            entry["included"] = false;
            entry["reason"] = "選択したPNG/DDS構成で相方を使用";
        }
        // 最終的に配布に採用する項目は、保持件数と非圧縮容量に加えます。
        if (entry.at("included").get<bool>()) { ++included; includedBytes += files[i].bytes; }
        // 配布しない項目は、除外された容量に加えます。
        else excludedBytes += files[i].bytes;
    }
    report["dependencySummary"] = report.at("summary");
    report["summary"] = {{"includedFiles", included}, {"excludedFiles", files.size() - included},
                         {"includedBytes", includedBytes}, {"excludedBytes", excludedBytes}};
    report["textureMode"] = GetTextureModeId(mode);
    return report;
}

bool CopyDirectoryFiltered(
    const fs::path& source,
    const fs::path& destination,
    TexturePackageMode textureMode,
    const std::unordered_set<std::wstring>& unusedResourcePaths,
    ResourceCopyStats& stats,
    std::string& errorMessage) {
    std::error_code ec;

    // 実行素材のフォルダーが無いかディレクトリでない場合は、コピーに進みません。
    if (!fs::exists(source, ec) || !fs::is_directory(source, ec)) {
        errorMessage = "Resourcesフォルダが見つかりません: " + PathToUtf8(source);
        return false;
    }

    fs::create_directories(destination, ec);

    // 実行用Resourcesの出力を作れない場合は、収集を中止します。
    if (ec) {
        errorMessage = "Resourcesコピー先を作成できません: " + PathToUtf8(destination);
        return false;
    }

    // 先に全ファイルを集め、同名のDDS/PNGペアを確定してからコピー方針を決めます。
    std::vector<ResourceFile> files;
    fs::recursive_directory_iterator it(source, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;

    // Resourcesの走査を開始できなければ、必要な素材の一覧を確定できません。
    if (ec) {
        errorMessage = "Resourcesを走査できません: " + ec.message();
        return false;
    }

    for (; it != end; it.increment(ec)) {
        // 走査中のエラーを受け取った時点で処理を止め、素材が欠けた出力を完成扱いにしません。
        if (ec) {
            errorMessage = "Resourcesコピー中に走査エラーが発生しました: " + ec.message();
            return false;
        }

        const fs::path entryPath = it->path();
        const fs::path relative = fs::relative(entryPath, source, ec);

        // 素材の相対パスを求められなければ、安全なコピー先を決められません。
        if (ec) {
            errorMessage = "Resources内の相対パスを作れません: " + ec.message();
            return false;
        }

        // Resources内部のリンクはコピーせず、元フォルダーの外にある素材へ処理を広げません。
        if (IsReparsePoint(entryPath)) {
            // リンクがディレクトリなら、反復子がリンク先に降りないようにします。
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }

        // Editor生成フォルダを丸ごと除外し、容量集計へも反映します。
        if (it->is_directory(ec)) {
            // キャッシュ・作業用フォルダー・エディター生成物なら、配下を素材候補に入れず除外容量を集計します。
            if (ShouldSkipResourceDirectory(entryPath.filename()) || IsEditorGeneratedDirectory(relative)) {
                ++stats.skippedDirectories;
                AccumulateSkippedDirectory(entryPath, stats);
                it.disable_recursion_pending();
                continue;
            }

            continue;
        }

        // 通常ファイルとして読める項目だけを候補に登録します。
        if (!it->is_regular_file(ec)) {
            continue;
        }

        const uint64_t bytes = static_cast<uint64_t>(it->file_size(ec));

        // 候補素材のバイト数を読めなければ、エラーを返して止めます。
        if (ec) {
            errorMessage = "ファイルサイズを取得できません: " + PathToUtf8(entryPath);
            return false;
        }

        ++stats.sourceFiles;
        stats.sourceBytes += bytes;
        files.push_back({ entryPath, relative, bytes });
    }

    const auto texturePairs = BuildTexturePairs(files, unusedResourcePaths);
    for (const auto& [key, flags] : texturePairs) {
        (void)key;
        // DDSとPNGの両方のビットが立っている場合に限り、ペア数を加算します。
        if (flags == 3) ++stats.texturePairs;
    }

    // 依存解析で除外した素材、次に指定モードで不要なテクスチャ形式を省きます。
    for (const ResourceFile& file : files) {
        const std::wstring normalizedRelative = ToLowerWide(file.relative.generic_wstring());

        // 同じ正規化相対パスが依存解析の除外集合にあれば、依存による除外へ計上します。
        if (unusedResourcePaths.find(normalizedRelative) != unusedResourcePaths.end()) {
            ++stats.skippedFiles;
            stats.skippedBytes += file.bytes;
            ++stats.omittedUnusedFiles;
            stats.omittedUnusedBytes += file.bytes;
            continue;
        }

        // 両形式のうち指定モードで不要な画像や付随ファイルは、形式選択による除外へ計上します。
        if (OmitTexture(file.relative, textureMode, texturePairs)) {
            ++stats.skippedFiles;
            stats.skippedBytes += file.bytes;
            ++stats.omittedTextureFiles;
            stats.omittedTextureBytes += file.bytes;
            continue;
        }

        const fs::path target = destination / file.relative;
        fs::create_directories(target.parent_path(), ec);

        // コピー先のフォルダーを作れない場合は、その素材を配置せずエラーにします。
        if (ec) {
            errorMessage = "コピー先フォルダを作成できません: " + PathToUtf8(target.parent_path());
            return false;
        }

        fs::copy_file(file.source, target, fs::copy_options::overwrite_existing, ec);

        // ファイルをコピーできなかった場合は、元と先のパスを返して処理を止めます。
        if (ec) {
            errorMessage = "ファイルをコピーできません: " + PathToUtf8(file.source) + " -> " + PathToUtf8(target);
            return false;
        }

        ++stats.copiedFiles;
        stats.copiedBytes += file.bytes;
    }

    return true;
}

fs::path FindMSBuildPath() {
    // 既知のVisual Studio構成を順に探し、見つからなければPATH上の実行ファイルを使う。
    const std::array<fs::path, 4> candidates = {
        fs::path(L"C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\MSBuild\\Current\\Bin\\MSBuild.exe"),
        fs::path(L"C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\MSBuild\\Current\\Bin\\MSBuild.exe"),
        fs::path(L"C:\\Program Files\\Microsoft Visual Studio\\2022\\BuildTools\\MSBuild\\Current\\Bin\\MSBuild.exe"),
        fs::path(L"C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\MSBuild\\Current\\Bin\\MSBuild.exe")
    };

    std::error_code ec;

    for (const fs::path& candidate : candidates) {
        // 既知のVisual Studioの場所に実行ファイルがあれば、最初に見つかったものを使います。
        if (fs::exists(candidate, ec)) {
            return candidate;
        }
    }

    return fs::path(L"MSBuild.exe");
}

std::wstring Quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

bool RunMSBuild(const fs::path& projectRoot, const std::string& configuration, std::string& errorMessage) {
    // UI に指定された構成をパッケージ作成前にビルドし、失敗時は収集を始めません。
    const fs::path solution = projectRoot / "ShimaEngine.sln";
    std::error_code ec;

    // ビルド入口のソリューションを見つけられなければ、MSBuildを起動しません。
    if (!fs::exists(solution, ec)) {
        errorMessage = "ShimaEngine.slnが見つかりません: " + PathToUtf8(solution);
        return false;
    }

    const fs::path msbuild = FindMSBuildPath();
    const std::wstring configWide = Utf8ToWide(configuration);
    const std::wstring command =
        Quote(msbuild.wstring()) + L" " +
        Quote(solution.wstring()) +
        L" /p:Configuration=" + configWide +
        L" /p:Platform=x64 /m:1";

    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    const std::wstring workingDir = projectRoot.wstring();

    BOOL created = CreateProcessW(
        nullptr,
        mutableCommand.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        workingDir.c_str(),
        &startupInfo,
        &processInfo);

    // 外部プロセスを開始できない場合は、ビルド未完了として上位へ返します。
    if (!created) {
        errorMessage = "MSBuildを起動できませんでした。Visual StudioのMSBuildが見つからない可能性があります。";
        return false;
    }

    // ビルド子プロセスの終了を待って終了コードを取得し、結果にかかわらずハンドルを閉じます。
    WaitForSingleObject(processInfo.hProcess, INFINITE);

    DWORD exitCode = 1;
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);

    // ビルドの終了コードが0以外なら、出力を収集しません。
    if (exitCode != 0) {
        errorMessage = "MSBuildが失敗しました。ExitCode: " + std::to_string(exitCode);
        return false;
    }

    return true;
}

bool WriteLaunchBatch(const fs::path& destination, const std::string& exeName, std::string& errorMessage) {
    // 配布先自身を作業ディレクトリにして起動し、日本語を含むパスもUTF-8で扱う。
    const fs::path batchPath = destination / "launch.bat";
    std::ofstream file(batchPath, std::ios::binary);

    // 起動バッチの作成先を開けなければ、パッケージ作成を続けません。
    if (!file) {
        errorMessage = "launch.batを作成できません: " + PathToUtf8(batchPath);
        return false;
    }

    const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
    file.write(reinterpret_cast<const char*>(bom), sizeof(bom));
    file << "@echo off\r\n";
    file << "chcp 65001 >nul\r\n";
    file << "cd /d \"%~dp0\"\r\n";
    file << "start \"\" \"" << exeName << "\"\r\n";
    return true;
}

std::string FormatMiB(uint64_t bytes) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2)
        << static_cast<double>(bytes) / (1024.0 * 1024.0) << " MiB";
    return stream.str();
}

std::string MakeLocalTimestamp() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
    localtime_s(&local, &now);
    std::ostringstream stream;
    stream << std::put_time(&local, "%Y-%m-%dT%H:%M:%S%z");
    return stream.str();
}

// 出力に含めた資産量と除外理由を記録し、提出後も内容を確認できるようにします。
bool WritePackageManifest(
    const fs::path& destination,
    const std::string& packageName,
    const std::string& configuration,
    bool builtBeforePackage,
    TexturePackageMode textureMode,
    bool excludeUnusedResources,
    bool zipRequested,
    const std::string& exeName,
    std::int32_t copiedDlls,
    const ResourceCopyStats& stats,
    std::string& errorMessage) {
    nlohmann::json manifest;
    manifest["formatVersion"] = 1;
    manifest["packageName"] = packageName;
    manifest["configuration"] = configuration;
    manifest["createdAt"] = MakeLocalTimestamp();
    manifest["builtBeforePackage"] = builtBeforePackage;
    manifest["executable"] = exeName;
    manifest["dllCount"] = copiedDlls;
    manifest["zipRequested"] = zipRequested;
    manifest["textureMode"] = GetTextureModeId(textureMode);
    manifest["excludeUnusedResources"] = excludeUnusedResources;
    manifest["resources"] = {
        { "sourceFiles", stats.sourceFiles },
        { "sourceBytes", stats.sourceBytes },
        { "copiedFiles", stats.copiedFiles },
        { "copiedBytes", stats.copiedBytes },
        { "skippedFiles", stats.skippedFiles },
        { "skippedBytes", stats.skippedBytes },
        { "skippedDirectories", stats.skippedDirectories },
        { "texturePairs", stats.texturePairs },
        { "omittedTextureFiles", stats.omittedTextureFiles },
        { "omittedTextureBytes", stats.omittedTextureBytes },
        { "omittedUnusedFiles", stats.omittedUnusedFiles },
        { "omittedUnusedBytes", stats.omittedUnusedBytes }
    };

    manifest["resourceExclusions"] = {
        "Resources/.cache",
        "Resources/.backup",
        "Resources/.trash",
        "Resources/tools",
        "Resources/generated/editor"
    };

    manifest["notes"] = {
        "元のResourcesは変更していません。",
        "DDSとPNGは同一フォルダ・同一ファイル名のペアだけを整理しています。",
        "片方しか存在しないテクスチャは保持しています。",
        "実行素材の選別は作成時の依存解析結果を使用し、runtime_resource_report.jsonに根拠を記録しています。"
    };

    const fs::path manifestPath = destination / "package_manifest.json";
    std::ofstream file(manifestPath, std::ios::binary);

    // マニフェストの作成先を開けなければ、出力を完成扱いにしません。
    if (!file) {
        errorMessage = "package_manifest.jsonを作成できません: " + PathToUtf8(manifestPath);
        return false;
    }

    file << manifest.dump(2);

    // JSONを書き込んだ後のストリームが失敗していれば、失敗理由を返します。
    if (!file) {
        errorMessage = "package_manifest.jsonを書き込めません: " + PathToUtf8(manifestPath);
        return false;
    }

    return true;
}

std::wstring QuotePowerShellLiteral(const std::wstring& value) {
    // PowerShellへ渡すパスの引用符を処理し、文字列として扱える形にする。
    std::wstring escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back(L'\'');

    for (wchar_t c : value) {
        // リテラル中の単一引用符は二重にし、パスの一部として解析させます。
        if (c == L'\'') {
            escaped.push_back(L'\'');
        }

        escaped.push_back(c);
    }

    escaped.push_back(L'\'');
    return escaped;
}

// 完成したディレクトリから ZIP を作ります。途中結果は完成品として公開しません。
bool CreateZipArchive(
    const fs::path& packageRoot,
    const fs::path& destination,
    const fs::path& zipPath,
    std::string& errorMessage) {
    std::error_code ec;

    // 指定ZIPがパッケージの出力範囲外なら、削除や上書きを始めません。
    if (!IsInsideDirectory(packageRoot, zipPath)) {
        errorMessage = "安全のためZIP出力先を操作できません: " + PathToUtf8(zipPath);
        return false;
    }

    // 同名のZIPが既にある場合だけ、圧縮前に削除します。
    if (fs::exists(zipPath, ec)) {
        fs::remove(zipPath, ec);

        // 古いZIPを削除できないときは、新しい圧縮を開始しません。
        if (ec) {
            errorMessage = "既存ZIPを削除できません: " + PathToUtf8(zipPath);
            return false;
        }
    }

    const std::wstring script =
        L"$ErrorActionPreference='Stop'; Compress-Archive -LiteralPath " + QuotePowerShellLiteral(destination.wstring()) +
        L" -DestinationPath " + QuotePowerShellLiteral(zipPath.wstring()) + L" -Force";
    const std::wstring command =
        L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command " + Quote(script);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    const std::wstring workingDirectory = packageRoot.wstring();
    const BOOL created = CreateProcessW(
        nullptr,
        mutableCommand.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        workingDirectory.c_str(),
        &startupInfo,
        &processInfo);
    // 圧縮用の子プロセスを開始できなければ、ZIPの作成失敗を返します。
    if (!created) {
        errorMessage = "ZIP作成用PowerShellを起動できませんでした。";
        return false;
    }

    WaitForSingleObject(processInfo.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);

    // プロセスの失敗、出力ファイルの欠落、または空のZIPを作成失敗として扱います。
    if (exitCode != 0 || !fs::exists(zipPath, ec) || fs::file_size(zipPath, ec) == 0) {
        errorMessage = "ZIP作成に失敗しました。ExitCode: " + std::to_string(exitCode);
        return false;
    }

    return true;
}

}

namespace executablePackageInternal {
std::string PreviewPackageName(const std::string& input) { return SanitizePackageName(input); }
fs::path PreviewProjectRoot() { return FindProjectRoot(); }
fs::path PreviewPathFromUtf8(const std::string& text) { return PathFromUtf8(text); }
std::string PreviewPathToUtf8(const fs::path& path) { return PathToUtf8(path); }
} // namespace executablePackageInternal

ExecutablePackageWindow::PackageResult ExecutablePackageWindow::RunPackageTask(const PackageRequest& request) {
    // 検証から一時出力の完成まで実行し、成功した場合だけ既存セットと入れ替えます。
    auto makeResult = [](bool success, const std::string& message, std::string reportPath = {}) {
        PackageResult result;
        result.success = success;
        result.message = message;
        result.reportPath = std::move(reportPath);
        return result;
    };

    try {
        const std::string packageName = SanitizePackageName(request.packageName);
        const fs::path projectRoot = FindProjectRoot();

        // プロジェクトのルートを検出できない場合は、実行出力や素材の特定に進みません。
        if (projectRoot.empty()) {
            return makeResult(false, "プロジェクトルートを検出できませんでした。");
        }

        // 必要なら収集前に指定構成をビルドし、その後で現在の素材への依存を調べます。
        if (request.buildBeforePackage) {
            std::string buildError;

            // 指定構成のビルドに失敗した場合、既存ビルドの収集に切り替えず終了します。
            if (!RunMSBuild(projectRoot, request.configuration, buildError)) {
                return makeResult(false, buildError);
            }
        }

        engine::RuntimeResourcePlan resourcePlan;
        // 未使用素材の除外が指定されている場合に、依存関係を収集します。
        if (request.excludeUnusedResources) {
            std::string analysisError;
            // GPUやエディターの共有状態を使わず、この配布作成用の依存一覧を組み立てます。
            // 依存一覧を組み立てられない場合は、必要な素材を欠いた配布物を作らないよう止めます。
            if (!engine::BuildRuntimeResourcePlan(projectRoot, request.additionalKeep, resourcePlan, analysisError))
                return makeResult(false, "実行素材の依存解析に失敗しました: " + analysisError);
        }

        const TexturePackageMode textureMode = ToTexturePackageMode(request.textureMode);
        // 依存解析を行った場合は、選んだ画像モードによる除外までレポートに反映します。
        if (request.excludeUnusedResources) resourcePlan.report = BuildCopyReport(resourcePlan, textureMode);

        // 分析のみの要求なら、依存と画像モードの結果をレポートへ書き、配布物のコピーへ進みません。
        if (request.analyzeOnly) {
            // EXEや素材のコピー前に、同じ選別結果を確認できます。元Resourcesへは書き込みません。
            const auto analysisRoot = projectRoot.parent_path() / "ExecutableSets";
            fs::create_directories(analysisRoot);
            const auto reportPath = analysisRoot / PathFromUtf8(packageName + ".resource-analysis.json");
            std::ofstream report(reportPath, std::ios::binary);
            report << resourcePlan.report.dump(2);
            // 解析結果の書き込みに失敗したら、集計を成功として返しません。
            if (!report.good()) return makeResult(false, "解析レポートを保存できません。");
            const auto& summary = resourcePlan.report.at("summary");
            return makeResult(true, "解析完了: 保持 " + std::to_string(summary.at("includedFiles").get<uint64_t>()) +
                "件 / 除外 " + std::to_string(summary.at("excludedFiles").get<uint64_t>()) +
                "件 / 未解決参照 " + std::to_string(resourcePlan.report.at("unresolvedReferences").size()) +
                "件。レポートで理由を確認できます。", PathToUtf8(reportPath));
        }

        // 実行ファイルのある出力構成を確かめてから、提出Setの一時・旧版・ZIPのPathを決めます。
        const fs::path outputRoot = fs::weakly_canonical(projectRoot.parent_path() / "generated" / "outputs" / PathFromUtf8(request.configuration));
        const fs::path sourceExe = outputRoot / "ShimaEngine.exe";
        std::error_code ec;

        // 指定構成に実行ファイルが無ければ、既存の完成品に触れる前に終了します。
        if (!fs::exists(sourceExe, ec)) {
            return makeResult(false, "実行ファイルが見つかりません: " + PathToUtf8(sourceExe));
        }

        const fs::path packageRoot = projectRoot.parent_path() /
            (request.includeProject ? fs::path("SubmissionPackages") : fs::path("ExecutableSets"));
        fs::create_directories(packageRoot, ec);

        // 出力ルートを作成できなければ、一時出力を作りません。
        if (ec) {
            return makeResult(false, "パッケージ出力フォルダを作成できません: " + PathToUtf8(packageRoot));
        }

        const fs::path destination = packageRoot / PathFromUtf8(packageName);
        const fs::path staging = packageRoot / PathFromUtf8(packageName + ".building");
        const fs::path previous = packageRoot / PathFromUtf8(packageName + ".previous");
        const fs::path zipPath = packageRoot / PathFromUtf8(packageName + ".zip");
        // 操作対象が一つでも出力ルートの外側にあるか、正式出力名が出力ルート自体を指す場合は中止します。
        if (!IsInsideDirectory(packageRoot, destination) || !IsInsideDirectory(packageRoot, staging) ||
            !IsInsideDirectory(packageRoot, previous) || !IsInsideDirectory(packageRoot, zipPath) ||
            fs::weakly_canonical(packageRoot, ec) == fs::weakly_canonical(destination, ec)) {
            return makeResult(false, "安全のため出力先を削除できません: " + PathToUtf8(destination));
        }

        // 中断された前回の一時Setと退避済み旧版を整理してから新しい一時Setを作ります。
        // 中断時に残った一時フォルダーがある場合は、新規作業の前に取り除きます。
        if (fs::exists(staging, ec)) {
            fs::remove_all(staging, ec);

            // 前回の一時フォルダーを除去できなければ、複製を始めません。
            if (ec) {
                return makeResult(false, "前回の一時フォルダを削除できません: " + PathToUtf8(staging));
            }
        }

        // 退避済み旧版が残っているときだけ、正式版の存在に応じて復旧か整理を選びます。
        if (fs::exists(previous, ec)) {
            // 正式版がない場合は前回の退避を戻します。正式版が残っていれば退避分だけを整理します。
            // 正式版が欠落している場合は、旧版の退避を元の名前へ戻します。
            if (!fs::exists(destination, ec)) {
                fs::rename(previous, destination, ec);
            // 正式版が残っている場合は、退避側だけを消去します。
            } else {
                fs::remove_all(previous, ec);
            }

            // 前回分の処理に失敗した場合は、既存の状態を保ったまま今回の作成を止めます。
            if (ec) {
                return makeResult(false, "前回の完成済みSetを復旧・整理できません: " + PathToUtf8(previous));
            }
        }

        fs::create_directories(staging, ec);

        // 一時出力を作成できなければ、素材や実行ファイルの収集を開始しません。
        if (ec) {
            return makeResult(false, "一時出力先を作成できません: " + PathToUtf8(staging));
        }

        // 途中で失敗したら作業用フォルダーだけを片付けます。正式名へ移し終えた場合はその削除予約を解除します。
        ScopedDirectoryCleanup stagingCleanup(staging);

        const fs::path runtimeDestination = request.includeProject
            ? staging / fs::path(L"実行ファイルセット")
            : staging;
        fs::create_directories(runtimeDestination, ec);

        // 実行用のサブフォルダーを作れなければ、出力先を確定できません。
        if (ec) {
            return makeResult(false, "実行ファイルセットの出力先を作成できません: " + PathToUtf8(runtimeDestination));
        }

        // 実行Setへ本体とDLLを揃えた後、選別したResourcesと起動バッチ・Manifestを追加します。
        const std::string exeFileName = packageName + ".exe";
        fs::copy_file(sourceExe, runtimeDestination / PathFromUtf8(exeFileName), fs::copy_options::overwrite_existing, ec);

        // 命名済みの実行ファイルを複製できなければ、一時出力の作成を打ち切ります。
        if (ec) {
            return makeResult(false, "実行ファイルをコピーできません: " + PathToUtf8(sourceExe));
        }

        std::int32_t copiedDlls = 0;

        for (const fs::directory_entry& entry : fs::directory_iterator(outputRoot, ec)) {
            // 出力の走査中にエラーが出た場合は、DLLの収集を打ち切ります。
            if (ec) {
                return makeResult(false, "ビルド出力フォルダを走査できません: " + PathToUtf8(outputRoot));
            }

            // ビルド出力の通常ファイルで拡張子が .dll のものだけを同梱します。
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".dll") {
                continue;
            }

            fs::copy_file(entry.path(), runtimeDestination / entry.path().filename(), fs::copy_options::overwrite_existing, ec);

            // DLLの複製に失敗したら、必要なファイルを欠いたまま進めません。
            if (ec) {
                return makeResult(false, "DLLをコピーできません: " + PathToUtf8(entry.path()));
            }

            ++copiedDlls;
        }

        // DLLを一つも収集できなければ、ビルド出力が揃っていないと判断します。
        if (copiedDlls == 0) {
            return makeResult(false, "DLLが見つかりません: " + PathToUtf8(outputRoot));
        }

        // Releaseの初回起動でDXCを待たないよう、ビルド済みShader Model 6を実行セットへ含める。
        const fs::path startupShaderCache = outputRoot / "StartupShaderCache";

        // ビルド済み起動シェーダーが存在するRelease構成では、そのキャッシュもコピーします。
        if (request.configuration == "Release" && fs::is_directory(startupShaderCache, ec)) {
            fs::copy(startupShaderCache, runtimeDestination / "StartupShaderCache",
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            // 起動シェーダーのコピーに失敗した場合、正式出力へ移しません。
            if (ec) {
                return makeResult(false, "シェーダーのコピーに失敗: " + PathToUtf8(startupShaderCache));
            }
        }

        ResourceCopyStats resourceStats;
        std::string copyError;
        // 必要なResourcesの複製に失敗した場合は、まだ一時出力の段階で中止します。
        if (!CopyDirectoryFiltered(
                projectRoot / "Resources",
                runtimeDestination / "Resources",
                textureMode,
                resourcePlan.excludedPaths,
                resourceStats,
                copyError)) {
            return makeResult(false, copyError);
        }

        // 未使用素材を除外した場合は、解析時とコピー時の入力が一致するか確認します。
        if (request.excludeUnusedResources) {
            // 編集が入った場合は一時Setのまま失敗させ、前回の完成品との置換を行いません。
            std::string changedInput;
            // 解析後に対象ファイルが変わった場合は、結果が古くなるため一時出力を公開しません。
            if (!engine::ValidateRuntimeResourcePlan(resourcePlan, changedInput))
                return makeResult(false, changedInput);
            // 実行用Resourcesと分けて、除外理由と解決できなかった参照を配布結果に残します。
            std::ofstream report(runtimeDestination / "runtime_resource_report.json", std::ios::binary);
            report << resourcePlan.report.dump(2);
            // レポートを最後まで書けなければ、一時出力を完成させません。
            if (!report.good()) return makeResult(false, "素材の依存解析レポートを保存できません。");
        }

        std::string batchError;

        // 起動バッチを用意できない場合は正式なパッケージには移しません。
        if (!WriteLaunchBatch(runtimeDestination, exeFileName, batchError)) {
            return makeResult(false, batchError);
        }

        std::string manifestError;
        // 資産数と作成条件を記録するマニフェストが書けなければ、作成を中止します。
        if (!WritePackageManifest(
            runtimeDestination,
            packageName,
            request.configuration,
            request.buildBeforePackage,
            textureMode,
            request.excludeUnusedResources,
            request.createZip,
            exeFileName,
            copiedDlls,
            resourceStats,
            manifestError)) {
            return makeResult(false, manifestError);
        }

        // 提出形式だけはソース側の project と README も同じ一時Setへまとめます。
        ProjectCopyStats projectStats;

        // ソース同梱が選ばれている場合だけ、提出用に project ツリーも整えます。
        if (request.includeProject) {
            std::string projectCopyError;

            // 除外規則を通した project コピーが失敗した場合、提出セットの公開を中止します。
            if (!CopyProjectFiltered(projectRoot, staging / "project", projectStats, projectCopyError)) {
                return makeResult(false, projectCopyError);
            }

            // README同梱も選ばれていれば、リポジトリ直下の案内を添えます。
            if (request.includeReadMe) {
                const fs::path readMeSource = projectRoot.parent_path() / "ReadMe.md";

                // 指定されたREADMEが見つからないか通常ファイルでなければ、作成失敗として返します。
                if (!fs::exists(readMeSource, ec) || !fs::is_regular_file(readMeSource, ec)) {
                    return makeResult(false, "プロジェクト外のReadMe.mdが見つかりません: " + PathToUtf8(readMeSource));
                }

                fs::copy_file(readMeSource, staging / "ReadMe.md", fs::copy_options::overwrite_existing, ec);

                // READMEを一時出力へコピーできなければ、提出セットの作成を止めます。
                if (ec) {
                    return makeResult(false, "ReadMe.mdを提出フォルダへコピーできません: " + PathToUtf8(readMeSource));
                }
            }
        }

        // 完成後にだけ旧Setを退避して一時Setを正式名へ移します。移動失敗なら旧Setへ戻します。
        const bool hadPreviousSet = fs::exists(destination, ec);

        // 既存の正式版があれば、一時出力を公開する前に旧版へ退避します。
        if (hadPreviousSet) {
            fs::rename(destination, previous, ec);

            // 旧版を退避できなければ、一時出力を正式名へ移動しません。
            if (ec) {
                return makeResult(false, "既存の完成済みSetを退避できません: " + PathToUtf8(destination));
            }
        }

        fs::rename(staging, destination, ec);

        // 一時出力から正式名への移動に失敗した場合は、復旧を試して失敗を返します。
        if (ec) {
            if (hadPreviousSet) {
                // 新しいフォルダーを正式名へ移せなかった場合に、退避済みの旧版を元の名前へ戻します。
                std::error_code restoreError;
                fs::rename(previous, destination, restoreError);
            }

            return makeResult(false, "完成した一時Setを移動できません: " + PathToUtf8(staging));
        }

        stagingCleanup.Release();

        // 新しい正式版への移動が済み、旧版があった場合にだけ退避を消します。
        if (hadPreviousSet) {
            fs::remove_all(previous, ec);

            // 正式版を作成済みでも、旧版の整理に失敗した場合はその状態を伝えます。
            if (ec) {
                return makeResult(false, "新しいSetは完成しましたが、旧Setの退避フォルダを削除できません: " + PathToUtf8(previous));
            }
        }

        // ZIP は正式Setから最後に作り、Set自体が完成した事実は失敗メッセージにも残します。
        if (request.createZip) {
            std::string zipError;

            // ZIP作成に失敗した場合は、完成済みのフォルダーが使えることも伝えます。
            if (!CreateZipArchive(packageRoot, destination, zipPath, zipError)) {
                return makeResult(false, zipError + " / Setフォルダ自体は作成済みです。");
            }
        // ZIPの作成を選ばなかった場合に古いZIPが残っていれば削除します。
        } else if (fs::exists(zipPath, ec)) {
            fs::remove(zipPath, ec);

            // 古いZIPを消せなければ、今回のフォルダーとの差し替え結果を成功扱いにしません。
            if (ec) {
                return makeResult(false, "古い提出用ZIPを削除できません: " + PathToUtf8(zipPath));
            }
        }

        std::ostringstream message;
        message << "作成完了: " << PathToUtf8(destination)
            << " / DLL " << copiedDlls
            << "件 / Resources " << resourceStats.copiedFiles << "件 (" << FormatMiB(resourceStats.copiedBytes) << ")"
            << " / 除外 " << resourceStats.skippedFiles << "件 (" << FormatMiB(resourceStats.skippedBytes) << ")"
            << " / Texture Pair " << resourceStats.texturePairs << "組";
        // 依存解析を行った場合は、その理由で省いた件数と容量も結果に含めます。
        if (request.excludeUnusedResources) {
            message << " / 依存解析による除外 " << resourceStats.omittedUnusedFiles
                << "件 (" << FormatMiB(resourceStats.omittedUnusedBytes) << ")";
        }

        // ソースを同梱した場合は、実行素材とは別に project の集計を載せます。
        if (request.includeProject) {
            message << " / project " << projectStats.copiedFiles << "件 (" << FormatMiB(projectStats.copiedBytes) << ")"
                << " / project除外 " << projectStats.skippedFiles << "件・" << projectStats.skippedDirectories << "フォルダ";
        }

        // ZIPを作成した場合は圧縮済みファイルのサイズも表示します。
        if (request.createZip) {
            const uint64_t zipBytes = static_cast<uint64_t>(fs::file_size(zipPath, ec));
            message << " / ZIP " << FormatMiB(zipBytes);
        }

        const auto completedRuntime = request.includeProject ? destination / fs::path(L"実行ファイルセット") : destination;
        return makeResult(true, message.str(), request.excludeUnusedResources
            ? PathToUtf8(completedRuntime / "runtime_resource_report.json") : "");
    } catch (const std::exception& e) {
        return makeResult(false, std::string("作成中に例外が発生しました: ") + e.what());
    }
}
