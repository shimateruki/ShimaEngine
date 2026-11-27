#define NOMINMAX
#include "engine/editor/core/EditorWindowsText.h"
#include "AssetAuditWindow.h"

#include "AudioPlayer.h"
#include "BaseScene.h"
#include "DebugEditor.h"
#include "DebugConsole.h"
#include "EditorManager.h"
#include "EffectPreviewStage.h"
#include "IconsFontAwesome5.h"
#include "imgui.h"
#include "Object3D.h"
#include "SceneManager.h"
#include "SrvManager.h"
#include "TextureManager.h"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace {
namespace fs = std::filesystem;

constexpr const char* kReportPath = "Resources/.cache/asset_audit/latest_report.json";
constexpr const char* kMarkdownReportPath = "Resources/.cache/asset_audit/asset_audit_report.md";
constexpr const char* kReportDirectoryPath = "Resources/.cache/asset_audit";
constexpr std::int64_t kMaxInlinePreviewBytes = 2LL * 1024LL * 1024LL;
constexpr std::int32_t kMaxInlinePreviewDimension = 2048;

// ASCII範囲だけを小文字化し、カテゴリ名や拡張子比較を安定させる。
std::string ToLowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

// Windowsのファイルパスの比較に使うワイド文字列を小文字化する。
std::wstring ToLowerWide(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return text;
}

// Windowsの区切り文字をスラッシュへ揃え、JSON上のパスと比較しやすくする。
std::string NormalizeSlash(std::string text) {
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

// ShellExecuteやCreateProcessに渡すため、UTF-8文字列をWindowsのワイド文字へ変換する。
using editorWindowsText::Utf8ToWide;

std::string QuoteCommandArg(const std::string& value) {
    // 外部監査コマンドの引数を引用符で囲み、途中の引用符をエスケープする。
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');

    for (char c : value) {
        // 引数中の二重引用符の前にバックスラッシュを付け、他の文字はそのまま連結する。
        if (c == '"') {
            escaped += "\\\"";
        } else {
            escaped.push_back(c);
        }
    }

    escaped.push_back('"');
    return escaped;
}

// エクスプローラーや関連付けアプリで、対象ファイルを外部から確認できるように開く。
bool OpenShellPath(const fs::path& path) {
    const fs::path normalized = fs::absolute(path).lexically_normal();
    const std::wstring widePath = normalized.wstring();
    HINSTANCE result = ShellExecuteW(nullptr, L"open", widePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    // ShellExecuteW は 32 以下をエラーコードとして返す。
    return reinterpret_cast<std::intptr_t>(result) > 32;
}

bool RunHiddenProcessAndWait(const std::string& commandLine, DWORD* exitCode) {
    // UIスレッドからは呼ばず、監査ワーカー内で子プロセスの終了を待つ。
    std::wstring command = Utf8ToWide(commandLine);

    // ワイド文字に変換できない空のコマンドは起動しない。
    if (command.empty()) {
        return false;
    }

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    const BOOL created = CreateProcessW(
        nullptr,
        mutableCommand.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startupInfo,
        &processInfo);

    // Windowsが子プロセスを作れなければ、終了状態を取得せず起動失敗を返す。
    if (!created) {
        return false;
    }

    // プロセスハンドルは終了コード取得後に閉じ、呼び出し側へ結果を渡す。
    WaitForSingleObject(processInfo.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(processInfo.hProcess, &code);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);

    // 終了コードを受け取るポインターが指定された場合にだけ書き込む。
    if (exitCode) {
        *exitCode = code;
    }

    return true;
}

bool ReadJsonFile(const fs::path& path, nlohmann::json& outJson) {
    // レポートを開いて解析できた場合にtrueを返す。呼び出し元は失敗時に結果を表示対象にしない。
    std::ifstream file(path);

    // 開けないファイルは解析しない。
    if (!file) {
        return false;
    }

    try {
        file >> outJson;
        return true;
    } catch (...) {
        // JSONの読み込みで例外が出た場合は、解析失敗として返す。
        return false;
    }
}

std::string JsonString(const nlohmann::json& item, const char* key, const std::string& fallback = "") {
    // 文字列はそのまま、整数は十進表記、浮動小数点は小数2桁で返す。キーがないかその他の型なら既定の文字列。
    if (!item.is_object() || !item.contains(key)) {
        return fallback;
    }

    const auto& value = item.at(key);

    if (value.is_string()) {
        return value.get<std::string>();
    }

    if (value.is_number_integer()) {
        return std::to_string(value.get<std::int64_t>());
    }

    if (value.is_number_float()) {
        char buffer[64]{};
        std::snprintf(buffer, sizeof(buffer), "%.2f", value.get<double>());
        return buffer;
    }

    return fallback;
}

std::int64_t JsonInt64(const nlohmann::json& item, const char* key) {
    // 容量などの整数をそのまま取得し、浮動小数点は整数へ変換する。キー不在や他の型はゼロ。
    if (!item.is_object() || !item.contains(key)) {
        return 0;
    }

    const auto& value = item.at(key);

    if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }

    if (value.is_number_float()) {
        return static_cast<std::int64_t>(value.get<double>());
    }

    return 0;
}

std::string FormatSizeText(std::int64_t bytes) {
    // 1024ごとにBからKB、MB、GBへ繰り上げる。バイトは整数、それ以上は小数1桁で表示する。
    static constexpr const char* kUnits[] = { "B", "KB", "MB", "GB" };
    double value = static_cast<double>(std::max<std::int64_t>(0, bytes));
    std::int32_t unitIndex = 0;

    while (value >= 1024.0 && unitIndex < 3) {
        value /= 1024.0;
        ++unitIndex;
    }

    std::ostringstream stream;

    if (unitIndex == 0) {
        stream << bytes << " " << kUnits[unitIndex];
    } else {
        stream << std::fixed << std::setprecision(1) << value << " " << kUnits[unitIndex];
    }

    return stream.str();
}

// 小さな件数・寸法を扱う呼び出し向けに、容量用の整数読み取りを32ビットへ変換する。
std::int32_t JsonInt(const nlohmann::json& item, const char* key) {
    return static_cast<std::int32_t>(JsonInt64(item, key));
}

std::string JoinStringArray(const nlohmann::json& arrayValue, const char* fallback = "-") {
    // 空の配列や配列以外は既定の文字列を返す。配列では文字列だけをカンマでつなぐ。
    if (!arrayValue.is_array() || arrayValue.empty()) {
        return fallback;
    }

    std::ostringstream stream;
    bool first = true;

    for (const auto& value : arrayValue) {
        // 文字列でない値を表示せず、区切りは採用した2件目から加える。
        if (!value.is_string())
            continue;
        if (!first)
            stream << ", ";
        stream << value.get<std::string>();
        first = false;
    }

    const std::string result = stream.str();
    return result.empty() ? fallback : result;
}

// キーが指す配列を借用する。キーなし・型違いなら共有する空配列を返す。
const nlohmann::json& ArrayOrEmpty(const nlohmann::json& root, const char* key) {
    static const nlohmann::json kEmpty = nlohmann::json::array();

    if (!root.is_object() || !root.contains(key) || !root.at(key).is_array()) {
        return kEmpty;
    }

    return root.at(key);
}

// プレビュー用オブジェクト名や一時名に使える現在時刻文字列を作る。
std::string TimestampText() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    localtime_s(&localTime, &time);

    std::ostringstream stream;
    stream << std::put_time(&localTime, "%Y%m%d_%H%M%S");
    return stream.str();
}

bool IsPathInside(const fs::path& child, const fs::path& parent) {
    // 絶対パスの表記を整理し、末尾の区切り込みで大文字小文字を区別せず先頭一致を調べる。
    const fs::path childAbs = fs::absolute(child).lexically_normal();
    const fs::path parentAbs = fs::absolute(parent).lexically_normal();

    std::wstring childText = ToLowerWide(childAbs.native());
    std::wstring parentText = ToLowerWide(parentAbs.native());

    // 末尾に区切りがない場合は両方に補い、名前が類似する隣接フォルダーも区別する。
    if (!parentText.empty() && parentText.back() != L'\\' && parentText.back() != L'/') {
        parentText.push_back(L'\\');
    }

    if (!childText.empty() && childText.back() != L'\\' && childText.back() != L'/') {
        childText.push_back(L'\\');
    }

    return childText.rfind(parentText, 0) == 0;
}

// 空のパス、..を含むパス、Resources外、.cacheと.trashを削除対象から除く。ペア探索でも共通で使う。
bool IsProtectedAssetPath(const std::string& relativePath) {
    const std::string lower = ToLowerAscii(NormalizeSlash(relativePath));
    if (lower.empty())
        return true;
    if (lower.find("..") != std::string::npos)
        return true;
    if (lower.rfind("resources/", 0) != 0)
        return true;
    if (lower.rfind("resources/.cache/", 0) == 0)
        return true;
    if (lower.rfind("resources/.trash/", 0) == 0)
        return true;
    return false;
}

// 実ファイルパスをプロジェクト相対へ戻し、レポートやステータス表示に使う。
std::string RelativeToProjectSlash(const fs::path& fullPath) {
    std::error_code ec;
    fs::path relative = fs::relative(fullPath, fs::current_path(), ec);

    // 相対パスへの変換が失敗したら、入力パスの表記だけ揃えて返す。
    if (ec) {
        relative = fullPath;
    }

    return NormalizeSlash(relative.generic_string());
}

fs::path UniqueTrashPath(const fs::path& targetPath) {
    // 退避名が空いていればそのまま使い、使用済みなら拡張子を保って連番を試す。
    if (!fs::exists(targetPath)) {
        return targetPath;
    }

    const fs::path parent = targetPath.parent_path();
    const std::string stem = targetPath.stem().generic_string();
    const std::string extension = targetPath.extension().generic_string();

    for (std::int32_t index = 1; index < 1000; ++index) {
        fs::path candidate = parent / (stem + "_" + std::to_string(index) + extension);

        // 未使用の連番が見つかった時点で探索を止める。
        if (!fs::exists(candidate)) {
            return candidate;
        }
    }

    // 試す連番を使い切った場合は固定の代替名を返す。
    return parent / (stem + "_overflow" + extension);
}

void AddIfDeletable(std::vector<fs::path>& targets, const fs::path& path, const fs::path& resourcesRoot) {
    // 対象が通常ファイルで、Resourcesの境界内に見え、保護対象外である場合にだけ候補とする。
    if (!fs::exists(path) || !fs::is_regular_file(path)) {
        return;
    }

    // Resourcesの位置に対応するパスの範囲から外れる候補を除く。
    if (!IsPathInside(path, resourcesRoot)) {
        return;
    }

    const std::string relative = RelativeToProjectSlash(path);

    // ペアで見つかった項目にも同じ保護条件を適用する。
    if (IsProtectedAssetPath(relative)) {
        return;
    }

    const fs::path normalized = fs::absolute(path).lexically_normal();
    const auto alreadyAdded = std::find_if(targets.begin(), targets.end(), [&](const fs::path& item) {
        return fs::equivalent(item, normalized);
    });

    // 同じ実ファイルがリストになければ、重複を避けて追加する。
    if (alreadyAdded == targets.end()) {
        targets.push_back(normalized);
    }
}

// DDSの元画像として扱う拡張子かを判定し、ペア削除の入口にする。
bool IsTextureSourceExtensionForDds(const std::string& extension) {
    return extension == ".png" ||
           extension == ".jpg" ||
           extension == ".jpeg" ||
           extension == ".bmp" ||
           extension == ".tga" ||
           extension == ".hdr";
}

std::vector<fs::path> BuildDeleteTargets(const fs::path& mainPath, const fs::path& resourcesRoot) {
    // 拡張子ごとに補助ファイルを探し、実在する許可済みパスだけを対象にする。
    std::vector<fs::path> targets;
    AddIfDeletable(targets, mainPath, resourcesRoot);

    const std::string extension = ToLowerAscii(mainPath.extension().generic_string());

    // 元画像なら同じ場所・名のDDSを候補にする。
    if (IsTextureSourceExtensionForDds(extension)) {
        AddIfDeletable(targets, fs::path(mainPath).replace_extension(".dds"), resourcesRoot);
    } else if (extension == ".dds") {
        // DDSからは同じ場所・名の元画像拡張子を順に調べる。
        for (const char* sourceExtension : { ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".hdr" }) {
            AddIfDeletable(targets, fs::path(mainPath).replace_extension(sourceExtension), resourcesRoot);
        }
    } else if (extension == ".gltf") {
        // GLTFは同名のBINも対象候補にする。
        AddIfDeletable(targets, fs::path(mainPath).replace_extension(".bin"), resourcesRoot);
    } else if (extension == ".obj") {
        // OBJは同名のMTLも対象候補にする。
        AddIfDeletable(targets, fs::path(mainPath).replace_extension(".mtl"), resourcesRoot);
    }

    return targets;
}

// パスの拡張子が指定セットに含まれるかを調べ、プレビュー種別判定に使う。
bool HasExtension(const std::string& path, std::initializer_list<const char*> extensions) {
    const std::string ext = ToLowerAscii(fs::path(path).extension().generic_string());

    for (const char* candidate : extensions) {
        if (ext == candidate) {
            return true;
        }
    }

    return false;
}

// 画像としてプレビューできるアセットかを判定する。
bool IsTextureAssetPath(const std::string& path) {
    return HasExtension(path, { ".png", ".dds", ".jpg", ".jpeg", ".bmp", ".tga" });
}

// 音声プレビューの対象になる拡張子かを判定する。
bool IsAudioAssetPath(const std::string& path) {
    return HasExtension(path, { ".wav", ".mp3", ".ogg" });
}

// モデルプレビューの対象になる拡張子かを判定する。
bool IsModelAssetPath(const std::string& path) {
    return HasExtension(path, { ".gltf", ".glb", ".obj", ".fbx" });
}

// 画像の拡張子を持ち、Sprite・UI・生成文字のパスに属するときにSpriteとして扱う。
bool IsSpriteAssetPath(const std::string& path) {
    if (!IsTextureAssetPath(path)) {
        return false;
    }

    const std::string lowerPath = ToLowerAscii(NormalizeSlash(path));
    return lowerPath.find("/sprite/") != std::string::npos ||
        lowerPath.find("/ui/") != std::string::npos ||
        lowerPath.find("/generated/text/") != std::string::npos;
}

constexpr std::int32_t kAssetFilterAll = 0;
constexpr std::int32_t kAssetFilterModel = 1;
constexpr std::int32_t kAssetFilterSprite = 2;
constexpr std::int32_t kAssetFilterSE = 3;
constexpr std::int32_t kAssetFilterAudio = 4;
constexpr std::int32_t kAssetFilterCount = 5;

const char* GetAssetFilterLabel(std::int32_t filter) {
    // 監査一覧のカテゴリ番号を日本語の表示名へ対応付ける。
    switch (filter) {
    case kAssetFilterModel:
        return "モデル";

    case kAssetFilterSprite:
        return "Sprite";

    case kAssetFilterSE:
        return "SE";

    case kAssetFilterAudio:
        return "オーディオ";

    default:
        return "すべて";
    }
}

std::int32_t GetAssetFilterIndex(const nlohmann::json& item) {
    // 監査レポートの分類名が揺れる場合も拡張子から表示カテゴリを補う。
    const std::string category = JsonString(item, "category");
    const std::string path = JsonString(item, "path");

    // レポート上のモデル分類か、モデルの拡張子であれば最優先のモデル分類にする。
    if (category == "Model" || category == "ModelData" || IsModelAssetPath(path)) {
        return kAssetFilterModel;
    }

    // Spriteの分類名か、対象フォルダー内の画像ならSpriteとして集計する。
    if (category == "Sprite" || IsSpriteAssetPath(path)) {
        return kAssetFilterSprite;
    }

    // SEの明示指定を一般の音声より先に判定する。
    if (category == "Audio-SE") {
        return kAssetFilterSE;
    }

    // BGMまたは一般の音声分類や拡張子は、オーディオ欄へまとめる。
    if (category == "Audio-BGM" || category == "Audio" || IsAudioAssetPath(path)) {
        return kAssetFilterAudio;
    }

    return kAssetFilterAll;
}

// UIのカテゴリフィルタに合わせて、表示する監査項目を絞り込む。
bool PassesAssetFilter(const nlohmann::json& item, std::int32_t filter) {
    return filter == kAssetFilterAll || GetAssetFilterIndex(item) == filter;
}

ImVec4 GetAssetFilterColor(std::int32_t filter) {
    // カテゴリごとの色を固定し、複数の一覧で識別方法を揃える。
    switch (filter) {
    case kAssetFilterModel:
        return ImVec4(0.52f, 0.82f, 1.0f, 1.0f);

    case kAssetFilterSprite:
        return ImVec4(0.65f, 1.0f, 0.62f, 1.0f);

    case kAssetFilterSE:
        return ImVec4(1.0f, 0.77f, 0.43f, 1.0f);

    case kAssetFilterAudio:
        return ImVec4(1.0f, 0.62f, 0.82f, 1.0f);

    default:
        return ImVec4(0.80f, 0.80f, 0.80f, 1.0f);
    }
}

std::string GetCategoryLabel(const std::string& category);

// アセット分類を色付きラベルとして描画し、一覧の視認性を上げる。
void DrawAssetCategoryBadge(const nlohmann::json& item) {
    const std::int32_t filter = GetAssetFilterIndex(item);
    ImGui::TextColored(GetAssetFilterColor(filter), "%s", GetAssetFilterLabel(filter));
    const std::string category = JsonString(item, "category");

    // 変換前のカテゴリがあり表示名と異なる場合に、マウスを乗せたとき元の分類を示す。
    if (!category.empty() && category != GetAssetFilterLabel(filter) && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("元カテゴリ: %s", GetCategoryLabel(category).c_str());
    }
}

// Audioカテゴリの中からBGMらしいものを判定し、再生方式を切り替える。
bool IsBgmCategory(const std::string& category, const std::string& path) {
    const std::string lowerCategory = ToLowerAscii(category);
    const std::string lowerPath = ToLowerAscii(NormalizeSlash(path));
    return lowerCategory.find("bgm") != std::string::npos ||
        lowerPath.find("/bgm/") != std::string::npos ||
        lowerPath.find("bgm") != std::string::npos;
}

bool IsSafeInlineTexturePreview(const nlohmann::json& item) {
    // レポートの値が2 MiBを超えるか、幅・高さのどちらかが2048ピクセルを超える場合は一覧内プレビューから除く。
    const std::int64_t sizeBytes = JsonInt64(item, "sizeBytes");
    const std::int32_t width = JsonInt(item, "width");
    const std::int32_t height = JsonInt(item, "height");

    if (sizeBytes > kMaxInlinePreviewBytes) {
        return false;
    }

    if (width > kMaxInlinePreviewDimension || height > kMaxInlinePreviewDimension) {
        return false;
    }

    return true;
}

std::string BuildHeavyAssetDetailText(const nlohmann::json& item) {
    // 画像は寸法、モデルは頂点・三角形数を優先して詳細欄へ示す。
    const std::int32_t width = JsonInt(item, "width");
    const std::int32_t height = JsonInt(item, "height");
    const std::int32_t vertices = JsonInt(item, "vertices");
    const std::int32_t triangles = JsonInt(item, "triangles");

    // 幅か高さが記録されていれば、まず画像の寸法を表示する。
    if (width > 0 || height > 0) {
        return std::to_string(width) + " x " + std::to_string(height);
    }

    // 画像寸法が分からずモデルの情報があれば、頂点数と三角形数を表示する。
    if (vertices > 0 || triangles > 0) {
        return "V:" + std::to_string(vertices) + " / T:" + std::to_string(triangles);
    }

    return "-";
}

// テーブルセルでは文字を一行で描き、マウスを乗せた場合だけツールチップで折り返す。
void DrawSingleLineCellText(const std::string& text) {
    const std::string displayText = text.empty() ? "-" : text;
    ImGui::TextUnformatted(displayText.c_str());

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(520.0f);
        ImGui::TextUnformatted(displayText.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// 内部カテゴリ名をUIに出しやすい表示名へ変換する。
std::string GetCategoryLabel(const std::string& category) {
    if (category == "Texture")
        return "画像";
    if (category == "Sprite")
        return "Sprite";
    if (category == "Model")
        return "モデル";
    if (category == "ModelData")
        return "モデルデータ";
    if (category == "Audio-BGM")
        return "オーディオ(BGM)";
    if (category == "Audio-SE")
        return "SE";
    if (category == "Audio")
        return "音声";
    return category;
}

std::string GetModelNameFromAssetPath(const std::string& relativePath) {
    // モデルファイルの親フォルダが同名なら、モデル管理で使うフォルダ名へ戻す。
    std::string path = NormalizeSlash(relativePath);
    const std::string prefix = "Resources/3DModel/";

    // Resources/3DModelを基点にした部分だけをモデル名に使う。
    if (path.rfind(prefix, 0) == 0) {
        path = path.substr(prefix.size());
    }

    fs::path modelPath = path;
    const fs::path parent = modelPath.parent_path();
    const std::string stem = modelPath.stem().generic_string();

    // 親の末尾がファイルの拡張子なしの名前と一致する場合、モデル管理へは親フォルダー名を渡す。
    if (!parent.empty() && parent.filename().generic_string() == stem) {
        return NormalizeSlash(parent.generic_string());
    }

    modelPath.replace_extension();
    return NormalizeSlash(modelPath.generic_string());
}

constexpr const char* kAssetAuditPreviewPrefix = "__Editor_AssetAuditPreview_";

// アセット監査が作った一時プレビューObjectだけを識別し、まとめて消せるようにする。
bool IsAssetAuditPreviewObject(const engine::Object3D* object) {
    if (!object)
        return false;
    return object->GetName().rfind(kAssetAuditPreviewPrefix, 0) == 0 ||
        object->GetClassName() == "EditorOnly_AssetAuditPreview";
}

} // namespace

void AssetAuditWindow::Initialize(DebugEditor* editor) {
    // エディターを借用する。初期状態で前回レポートは表示せず、実行か再読み込みを待つ。
    editor_ = editor;
    hasReport_ = false;
    lastStatus_ = "アセット監査を開きました。前回レポートを確認する場合は、レポート再読み込みを押してください。";
}

// アセット監査ウィンドウ全体を描画し、実行・読み込み・検索・削除をまとめて扱う。
void AssetAuditWindow::DrawImGui() {
#ifdef USE_IMGUI
    // 非同期監査の完了を先に反映し、そのフレームから最新結果を描画する。
    (void)editor_;
    UpdateAuditProcess();
    ImGui::Text(ICON_FA_SEARCH " アセット監査 / Heavy Asset Profiler + Unused Asset Scanner");
    ImGui::Separator();
    ImGui::TextWrapped("外部ツール tools/asset_audit/asset_audit.ps1 で Resources を解析し、生成されたJSONをエンジン側で確認します。重い素材と未使用候補を見つけるための作業補助ツールです。");

    // 操作ボタンを押したときだけ監査・再読込・外部表示・一時プレビューの削除を実行する。
    if (ImGui::Button(ICON_FA_SEARCH " 監査ツール実行")) {
        RunAuditTool();
    }

    ImGui::SameLine();

    if (ImGui::Button(ICON_FA_SYNC " レポート再読み込み")) {
        LoadLatestReport();
    }

    ImGui::SameLine();

    if (ImGui::Button(ICON_FA_FILE_ALT " 外部レポート")) {
        OpenExternalPath(kMarkdownReportPath);
    }

    ImGui::SameLine();

    if (ImGui::Button(ICON_FA_FOLDER_OPEN " レポートフォルダ")) {
        OpenExternalPath(kReportDirectoryPath);
    }

    ImGui::SameLine();

    if (ImGui::Button(ICON_FA_EYE_SLASH " モデル確認を消す")) {
        RemoveModelPreviews();
    }

    ImGui::Checkbox("軽い画像だけサムネイル表示", &showPreviewThumbnails_);

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("大きい画像はゲーム内に読み込まず、外部確認ボタンで開きます。");
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragInt("表示上限", &maxRowsToDraw_, 10.0f, 50, 2000);
    ImGui::TextWrapped("%s", lastStatus_.c_str());

    // 有効なレポートをまだ開いていなければ、概要や一覧を描かない。
    if (!hasReport_) {
        ImGui::TextDisabled("まだレポートが読み込まれていません。監査ツールを実行してください。");
        return;
    }

    DrawSummary();
    DrawCategorySummary();
    ImGui::Separator();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("検索", searchBuffer_, sizeof(searchBuffer_));
    static const char* kCategoryFilterLabels[] = { "すべて", "モデル", "Sprite", "SE", "オーディオ" };
    ImGui::SetNextItemWidth(180.0f);
    ImGui::Combo("分類フィルター", &categoryFilter_, kCategoryFilterLabels, IM_ARRAYSIZE(kCategoryFilterLabels));

    // 分類を絞り込んでいる場合だけ、その場で全分類に戻すボタンを出す。
    if (categoryFilter_ != kAssetFilterAll) {
        ImGui::SameLine();

        if (ImGui::SmallButton("分類解除")) {
            categoryFilter_ = kAssetFilterAll;
        }
    }

    // 共通の検索・分類条件を各一覧へ渡し、同じ監査結果を目的別に見る。
    if (ImGui::BeginTabBar("AssetAuditTabs")) {
        if (ImGui::BeginTabItem("重い素材")) {
            DrawHeavyAssets();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("未使用候補")) {
            DrawUnusedAssets();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("欠損参照")) {
            DrawMissingReferences();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    // 一覧の行で受けた削除要求を、タブの内容を描き終えてから確認ダイアログへ渡す。
    if (pendingDeletePopupRequested_) {
        pendingDeletePopupRequested_ = false;
        ImGui::OpenPopup("AssetAuditDeleteConfirm");
    }

    DrawDeleteConfirmPopup();
#endif
}

void AssetAuditWindow::UpdateAuditProcess() {
    // 実行中だけfutureを確認する。未完了のワーカーは待たずに画面更新へ戻る。
    if (!auditRunning_) {
        return;
    }

    // 実行フラグだけが残ってfutureが無効なら、実行中の表示を解除する。
    if (!auditFuture_.valid()) {
        auditRunning_ = false;
        return;
    }

    if (auditFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }

    // ワーカーの起動失敗とスクリプトの異常終了を区別し、正常終了時だけ生成結果を読む。
    const std::uint32_t exitCode = auditFuture_.get();
    auditRunning_ = false;

    // ワーカーが起動に失敗したときの専用値を、子プロセスの終了コードと区別する。
    if (exitCode == 0xFFFFFFFFu) {
        lastStatus_ = "アセット監査ツールの起動に失敗しました。PowerShellまたはtools/asset_audit/asset_audit.ps1を確認してください。";
        return;
    }

    // 起動できても終了コードがゼロ以外なら、生成結果へ切り替えない。
    if (exitCode != 0) {
        lastStatus_ = "アセット監査ツールがエラー終了しました。ツール単体で実行して詳細を確認してください。";
        return;
    }

    // プロセスが正常終了してもJSONを読めなければ、レポート読込の失敗として表示する。
    if (!LoadLatestReport()) {
        lastStatus_ = "監査は完了しましたが、レポートJSONを読み込めませんでした。";
        return;
    }

    lastStatus_ = "アセット監査が完了しました。最新レポートを読み込みました。";
}

bool AssetAuditWindow::RunAuditTool() {
    // 監査スクリプトをワーカーで起動する。すでに走っている場合はfalseを返し、前回のfutureを維持する。
    if (auditRunning_) {
        lastStatus_ = "アセット監査を実行中です。完了まで少し待ってください。";
        return false;
    }

    const std::string toolCommand =
        "powershell.exe -NoProfile -ExecutionPolicy Bypass -File " +
        QuoteCommandArg("tools/asset_audit/asset_audit.ps1");

    auditRunning_ = true;
    lastStatus_ = "アセット監査をバックグラウンドで実行しています。";

    // プロセスの待機はワーカースレッドへ逃がし、ImGui 側は状態だけ監視する。
    auditFuture_ = std::async(std::launch::async, [toolCommand]() -> std::uint32_t {
        DWORD exitCode = 1;
        // スクリプトを起動できない場合は、通常の終了コードとは別の値を返す。
        if (!RunHiddenProcessAndWait(toolCommand, &exitCode)) {
            return 0xFFFFFFFFu;
        }
        return static_cast<std::uint32_t>(exitCode);
    });
    return true;
}

bool AssetAuditWindow::LoadLatestReport() {
    // 読み込み失敗時は前回結果を有効扱いせず、誤った削除判断を避ける。
    nlohmann::json report;

    if (!ReadJsonFile(kReportPath, report)) {
        hasReport_ = false;
        lastStatus_ = "レポートが見つからないか、JSONとして読み込めません: " + std::string(kReportPath);
        return false;
    }

    latestReport_ = std::move(report);
    hasReport_ = true;
    const std::string generatedAt = JsonString(latestReport_, "generatedAt", "unknown");
    lastStatus_ = "読み込み済み: " + std::string(kReportPath) + " / " + generatedAt;
    return true;
}

void AssetAuditWindow::DrawSummary() {
#ifdef USE_IMGUI
    // レポートの集計値と実行時の閾値を並べ、警告数の根拠を確認できるようにする。
    const nlohmann::json summary = latestReport_.value("summary", nlohmann::json::object());
    const nlohmann::json thresholds = latestReport_.value("thresholds", nlohmann::json::object());

    // 概要テーブルの描画範囲を確保できたときだけ、2組の項目と値を同じ行へ出す。
    if (ImGui::BeginTable("AssetAuditSummary", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("項目", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("値", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("項目", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("値", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        auto row = [](const char* labelA, const std::string& valueA, const char* labelB, const std::string& valueB) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(labelA);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(valueA.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(labelB);
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(valueB.c_str());
        };

        row("総ファイル数", JsonString(summary, "totalFiles"), "総容量", JsonString(summary, "totalSizeText"));
        row("使用検出数", JsonString(summary, "usedFiles"), "未使用候補", JsonString(summary, "unusedFiles") + " / " + JsonString(summary, "unusedSizeText"));
        row("重い素材警告", JsonString(summary, "heavyWarningCount"), "欠損参照", JsonString(summary, "missingReferenceCount"));
        row("Texture閾値", JsonString(thresholds, "heavyTextureMB") + " MB", "Model閾値", JsonString(thresholds, "heavyModelMB") + " MB");

        ImGui::EndTable();
    }
#endif
}

void AssetAuditWindow::DrawCategorySummary() {
#ifdef USE_IMGUI
    // レポートの配列からオブジェクトだけを読み、判定できた実カテゴリ別に警告と未使用候補を集計する。
    std::array<std::int32_t, kAssetFilterCount> heavyCounts{};
    std::array<std::int32_t, kAssetFilterCount> unusedCounts{};

    for (const auto& item : ArrayOrEmpty(latestReport_, "heavyAssets")) {
        if (!item.is_object())
            continue;
        const std::int32_t index = GetAssetFilterIndex(item);

        if (index > kAssetFilterAll && index < kAssetFilterCount) {
            ++heavyCounts[static_cast<size_t>(index)];
        }
    }

    for (const auto& item : ArrayOrEmpty(latestReport_, "unusedAssets")) {
        if (!item.is_object())
            continue;
        const std::int32_t index = GetAssetFilterIndex(item);

        if (index > kAssetFilterAll && index < kAssetFilterCount) {
            ++unusedCounts[static_cast<size_t>(index)];
        }
    }

    ImGui::TextDisabled("分類別に確認できます。ボタンを押すと一覧がその分類だけになります。");

    // 分類表が開けたら内訳を表示し、行のボタンで同じ分類を一覧に適用できるようにする。
    if (ImGui::BeginTable("AssetAuditCategorySummary", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("分類");
        ImGui::TableSetupColumn("重い素材");
        ImGui::TableSetupColumn("未使用候補");
        ImGui::TableSetupColumn("表示");
        ImGui::TableHeadersRow();

        for (std::int32_t filter = kAssetFilterModel; filter < kAssetFilterCount; ++filter) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(GetAssetFilterColor(filter), "%s", GetAssetFilterLabel(filter));
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%d 件", heavyCounts[static_cast<size_t>(filter)]);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d 件", unusedCounts[static_cast<size_t>(filter)]);
            ImGui::TableSetColumnIndex(3);
            ImGui::PushID(filter);

            if (ImGui::SmallButton(categoryFilter_ == filter ? "表示中" : "表示")) {
                categoryFilter_ = filter;
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }
#endif
}

#ifdef USE_IMGUI
namespace {
// 横スクロール表では省略表示、幅に合わせる表ではパスを折り返す。サイズ欄は両表で共通。
void DrawAssetPathAndSize(const nlohmann::json& item, const std::string& path, bool wrapPath) {
    ImGui::TableSetColumnIndex(1);
    if (wrapPath) {
        ImGui::TextWrapped("%s", path.c_str());
    } else {
        DrawSingleLineCellText(path);
    }
    ImGui::TableSetColumnIndex(2);
    ImGui::TextUnformatted(JsonString(item, "sizeText").c_str());
}
}
#endif

void AssetAuditWindow::DrawHeavyAssets() {
#ifdef USE_IMGUI
    // 大容量の警告一覧を描く。検索と分類を通った行に表示上限をかけ、打ち切った場合は表示件数を示す。
    const auto& heavyAssets = ArrayOrEmpty(latestReport_, "heavyAssets");
    std::int32_t drawnCount = 0;
    bool truncated = false;
    ImGui::TextDisabled("横スクロールできます。パスやメモにマウスを乗せると全文を確認できます。");

    const ImGuiTableFlags tableFlags =
        ImGuiTableFlags_Borders |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable |
        ImGuiTableFlags_ScrollX |
        ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_SizingFixedFit;

    // テーブルが開けた場合に、見出しを残して縦横にスクロールできる形で描く。
    if (ImGui::BeginTable("HeavyAssetTable", 6, tableFlags, ImVec2(0, 420), 1040.0f)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("種別", ImGuiTableColumnFlags_WidthFixed, 78.0f);
        ImGui::TableSetupColumn("パス", ImGuiTableColumnFlags_WidthFixed, 380.0f);
        ImGui::TableSetupColumn("容量", ImGuiTableColumnFlags_WidthFixed, 86.0f);
        ImGui::TableSetupColumn("詳細", ImGuiTableColumnFlags_WidthFixed, 126.0f);
        ImGui::TableSetupColumn("メモ", ImGuiTableColumnFlags_WidthFixed, 260.0f);
        ImGui::TableSetupColumn("確認", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableHeadersRow();

        for (const auto& item : heavyAssets) {
            // 正常な行データであり、検索と分類の両方を通った場合だけ数える。
            if (!item.is_object() || !MatchesSearch(item) || !PassesAssetFilter(item, categoryFilter_))
                continue;
            if (drawnCount >= maxRowsToDraw_) {
                truncated = true;
                break;
            }

            ++drawnCount;

            const bool warning = JsonString(item, "severity") == "warning";
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);

            // 警告レベルの行だけ分類を橙色で強調し、その他は通常色を使う。
            if (warning) {
                ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), "%s", GetCategoryLabel(JsonString(item, "category")).c_str());
            } else {
                ImGui::TextUnformatted(GetCategoryLabel(JsonString(item, "category")).c_str());
            }

            DrawAssetPathAndSize(item, JsonString(item, "path"), false);
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(BuildHeavyAssetDetailText(item).c_str());
            ImGui::TableSetColumnIndex(4);
            DrawSingleLineCellText(JoinStringArray(item.value("notes", nlohmann::json::array())));
            ImGui::TableSetColumnIndex(5);
            DrawAssetPreview(item, 54.0f);
        }

        ImGui::EndTable();
    }

    if (truncated) {
        ImGui::TextDisabled("表示上限により %d 件まで表示中です。必要なら表示上限を上げてください。", drawnCount);
    }
#endif
}

void AssetAuditWindow::DrawUnusedAssets() {
#ifdef USE_IMGUI
    // 未使用候補から検索・分類を通った行を上限まで描き、選んだファイルは個別の確認へ進ませる。
    const auto& unusedAssets = ArrayOrEmpty(latestReport_, "unusedAssets");
    std::int32_t drawnCount = 0;
    bool truncated = false;
    ImGui::TextDisabled("JSONとコードから直接参照を見つけられなかった候補です。削除ボタンは Resources/.trash/asset_audit/ へ退避します。");

    // テーブルの範囲を確保できた場合だけ、理由と削除操作を持つ候補行を描く。
    if (ImGui::BeginTable("UnusedAssetTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 360))) {
        ImGui::TableSetupColumn("種別", ImGuiTableColumnFlags_WidthFixed, 82.0f);
        ImGui::TableSetupColumn("パス", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("容量", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("理由", ImGuiTableColumnFlags_WidthFixed, 230.0f);
        ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 96.0f);
        ImGui::TableSetupColumn("確認", ImGuiTableColumnFlags_WidthFixed, 118.0f);
        ImGui::TableHeadersRow();

        for (const auto& item : unusedAssets) {
            if (!item.is_object() || !MatchesSearch(item) || !PassesAssetFilter(item, categoryFilter_))
                continue;
            if (drawnCount >= maxRowsToDraw_) {
                truncated = true;
                break;
            }

            ++drawnCount;

            const std::string path = JsonString(item, "path");

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawAssetCategoryBadge(item);
            DrawAssetPathAndSize(item, path, true);
            ImGui::TableSetColumnIndex(3);
            ImGui::TextWrapped("%s", JsonString(item, "reason").c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::PushID(path.c_str());

            if (ImGui::Button(ICON_FA_TRASH_ALT " 削除", ImVec2(-1.0f, 0.0f))) {
                // 行のボタンでは対象を記録し、実際の削除は確認ダイアログで承認されるまで待つ。
                pendingDeletePath_ = path;
                pendingDeletePopupRequested_ = true;
            }

            ImGui::PopID();
            ImGui::TableSetColumnIndex(5);
            DrawAssetPreview(item, 54.0f);
        }

        ImGui::EndTable();
    }

    if (truncated) {
        ImGui::TextDisabled("表示上限により %d 件まで表示中です。必要なら表示上限を上げてください。", drawnCount);
    }
#endif
}

void AssetAuditWindow::DrawMissingReferences() {
#ifdef USE_IMGUI
    // 欠損参照の参照元・値・候補を1行に表示し、検索に一致する項目を上限まで描く。
    const auto& missingReferences = ArrayOrEmpty(latestReport_, "missingReferences");
    std::int32_t drawnCount = 0;
    bool truncated = false;
    ImGui::TextDisabled("Resources/ から始まる参照のうち、実ファイルやディレクトリが見つからなかったものです。");

    // テーブルが開けたときに、参照元と期待パスの候補を対応させて描く。
    if (ImGui::BeginTable("MissingReferenceTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 360))) {
        ImGui::TableSetupColumn("参照元", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("参照値", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("候補", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (const auto& item : missingReferences) {
            if (!item.is_object() || !MatchesSearch(item))
                continue;
            if (drawnCount >= maxRowsToDraw_) {
                truncated = true;
                break;
            }

            ++drawnCount;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextWrapped("%s", JsonString(item, "source").c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.45f, 1.0f), "%s", JsonString(item, "value").c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextWrapped("%s", JoinStringArray(item.value("expectedCandidates", nlohmann::json::array())).c_str());
        }

        ImGui::EndTable();
    }

    if (truncated) {
        ImGui::TextDisabled("表示上限により %d 件まで表示中です。必要なら表示上限を上げてください。", drawnCount);
    }
#endif
}

void AssetAuditWindow::DrawDeleteConfirmPopup() {
#ifdef USE_IMGUI
    // 行で要求された確認画面を描き、本体と相方を完全削除するか、対象を外して閉じるかを受け付ける。
    if (ImGui::BeginPopupModal("AssetAuditDeleteConfirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text(ICON_FA_EXCLAMATION_TRIANGLE " アセットを削除しますか？");
        ImGui::Separator();
        ImGui::TextWrapped("%s", pendingDeletePath_.c_str());
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "この操作は完全削除です。元に戻せません。");
        ImGui::TextWrapped("PNG/DDSやGLTF/BINなどの相方ファイルがある場合は一緒に削除します。必要な素材ではないか確認してから実行してください。");
        ImGui::Spacing();

        if (ImGui::Button(ICON_FA_TRASH_ALT " 完全削除する", ImVec2(150.0f, 0.0f))) {
            std::vector<std::string> deletedPaths;
            std::string errorMessage;

            // 削除処理が成功を返した場合だけ一覧を更新し、失敗時は理由を表示する。
            if (DeleteAssetFiles(pendingDeletePath_, deletedPaths, errorMessage)) {
                RemoveDeletedAssetsFromReport(deletedPaths);
                lastStatus_ = "アセットを完全削除しました: " + std::to_string(deletedPaths.size()) + " 件。";
            } else {
                lastStatus_ = "アセット削除に失敗しました: " + errorMessage;
            }

            pendingDeletePath_.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        // キャンセルではファイル操作を行わず、保留中のパスを外す。
        if (ImGui::Button("キャンセル", ImVec2(120.0f, 0.0f))) {
            pendingDeletePath_.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
#endif
}

uint32_t AssetAuditWindow::GetPreviewTextureHandle(const std::string& relativePath) {
    // 同じ画像の再ロードを避け、監査画面で使う SRV をパス単位で再利用する。
    const std::string normalizedPath = NormalizeSlash(relativePath);
    auto it = previewTextureHandles_.find(normalizedPath);

    // 同じパスのハンドルをキャッシュ済みなら、読み込みを省いて返す。
    if (it != previewTextureHandles_.end()) {
        return it->second;
    }

    const fs::path path = (fs::current_path() / fs::path(normalizedPath)).lexically_normal();

    // 存在する通常ファイルでなければ、読み込み前に無効ハンドルを返す。
    if (!fs::exists(path) || !fs::is_regular_file(path)) {
        return 0;
    }

    uint32_t handle = engine::TextureManager::GetInstance()->Load(normalizedPath);
    previewTextureHandles_[normalizedPath] = handle;
    return handle;
}

bool AssetAuditWindow::OpenExternalPath(const std::string& relativePath) {
    // 外部アプリへ渡す前に実在を確認し、失敗理由を UI へ残す。
    const std::string normalizedPath = NormalizeSlash(relativePath);
    const fs::path fullPath = (fs::current_path() / fs::path(normalizedPath)).lexically_normal();

    // 対象パスが存在しなければ、関連付け先を起動しない。
    if (!fs::exists(fullPath)) {
        lastStatus_ = "外部で開く対象が見つかりません: " + normalizedPath;
        return false;
    }

    // 関連付けアプリが開けなかった場合は、対象のパスを添えて通知する。
    if (!OpenShellPath(fullPath)) {
        lastStatus_ = "外部アプリで開けませんでした: " + normalizedPath;
        return false;
    }

    lastStatus_ = "外部で開きました: " + normalizedPath;
    return true;
}

bool AssetAuditWindow::PlayAudioPreview(const std::string& relativePath, bool isBgm) {
    // 試聴のたびに音声を再ロードしないよう、読み込み済みハンドルを使う。
    const std::string normalizedPath = NormalizeSlash(relativePath);
    const fs::path path = (fs::current_path() / fs::path(normalizedPath)).lexically_normal();

    // パスが存在しないか通常ファイル以外なら、音声として読み込まない。
    if (!fs::exists(path) || !fs::is_regular_file(path)) {
        lastStatus_ = "音声ファイルが見つかりません: " + normalizedPath;
        return false;
    }

    uint32_t handle = AudioPlayer::kInvalidAudioHandle;
    auto it = previewAudioHandles_.find(normalizedPath);

    // 試聴用ハンドルを登録済みなら再利用し、無ければ読み込んで記録する。
    if (it != previewAudioHandles_.end()) {
        handle = it->second;
    } else {
        handle = AudioPlayer::GetInstance()->LoadSoundFile(normalizedPath);
        previewAudioHandles_[normalizedPath] = handle;
    }

    // 読み込み結果が無効なら、音声の再生状態を変更せず終了する。
    if (handle == AudioPlayer::kInvalidAudioHandle) {
        lastStatus_ = "音声の読み込みに失敗しました: " + normalizedPath;
        return false;
    }

    // BGM は既存の再生を止めて切り替え、SE は単発で鳴らす。
    if (isBgm) {
        AudioPlayer::GetInstance()->StopBGM();
        AudioPlayer::GetInstance()->PlayBGM(handle, false, 0.75f);
    } else {
        AudioPlayer::GetInstance()->PlaySE(handle, false, 0.85f);
    }

    lastStatus_ = "音声を試聴しました: " + normalizedPath;
    return true;
}

std::int32_t AssetAuditWindow::CountModelPreviews() const {
    // エディター専用の一時 Object だけ数え、通常のシーン資産は含めない。
    if (!editor_ || !editor_->GetSceneManager() || !editor_->GetSceneManager()->GetCurrentScene()) {
        return 0;
    }

    std::int32_t count = 0;

    for (const auto& object : editor_->GetSceneManager()->GetCurrentScene()->GetObjects()) {
        // 名前の接頭辞かクラス名がプレビュー用の識別子に合うものだけ数える。
        if (IsAssetAuditPreviewObject(object.get())) {
            ++count;
        }
    }

    return count;
}

void AssetAuditWindow::CreateModelPreview(const std::string& relativePath) {
    // 既存シーンへ一時配置するので、シーンと描画共通資源がある場合だけ進める。
    if (!editor_ || !editor_->GetSceneManager() || !editor_->GetSceneManager()->GetCurrentScene()) {
        lastStatus_ = "モデルを確認できるシーンがありません。";
        return;
    }

    const std::string normalizedPath = NormalizeSlash(relativePath);
    const fs::path path = (fs::current_path() / fs::path(normalizedPath)).lexically_normal();

    // 対象を通常ファイルとして開ける場合にだけ、シーン上のプレビューを用意する。
    if (!fs::exists(path) || !fs::is_regular_file(path)) {
        lastStatus_ = "モデルファイルが見つかりません: " + normalizedPath;
        return;
    }

    BaseScene* scene = editor_->GetSceneManager()->GetCurrentScene();
    engine::Object3DCommon* common = scene->GetObject3DCommon();

    // 3D共通基盤がない場合は、一時オブジェクトを初期化できない。
    if (!common) {
        lastStatus_ = "Object3dCommonが取得できません。";
        return;
    }

    // 独立プレビューを使えるときは専用の原点とカメラを使う。ない場合はシーンの既定位置を使う。
    engine::Vector3 origin = { 0.0f, 2.0f, 0.0f };
    EffectPreviewStage* stage = EffectPreviewStage::GetInstance();

    if (stage) {
        stage->EnableForToolPreview();
        stage->RequestCameraRecenter();
        origin = stage->GetPreviewPosition();
    }

    const std::int32_t previewIndex = CountModelPreviews();
    const std::string modelName = GetModelNameFromAssetPath(normalizedPath);
    auto object = std::make_unique<engine::Object3D>();
    object->Initialize(common);
    object->SetName(std::string(kAssetAuditPreviewPrefix) + std::to_string(previewIndex) + "_" + modelName);

    // 専用クラス名で一時配置を識別し、衝突を無効にして通常の操作を妨げない。
    object->SetClassName("EditorOnly_AssetAuditPreview");
    object->SetSaveCategory("Object");
    object->SetIsLocked(true);
    object->SetCollisionAttribute(0);
    object->SetCollisionMask(0);
    object->SetModel(modelName);
    // すでに確認中のモデルと重ねず、ワールド X 方向に一体あたり 3 単位ずらす。
    object->SetTranslate({
        origin.x + static_cast<float>(previewIndex) * 3.0f,
        origin.y,
        origin.z
    });
    object->SetScale({ 1.0f, 1.0f, 1.0f });
    object->UpdateLocalMatrix();
    object->UpdateWorldMatrix();

    engine::Object3D* rawObject = object.get();
    // シーンへ所有権を移し、直後に配置済みObjectをエディターの選択対象にする。
    scene->AddObjectImmediate(std::move(object));
    editor_->SetSelectedObject(rawObject);
    EditorManager::GetInstance()->SetSelectedObject(editor_);

    lastStatus_ = "モデル確認用Objectを配置しました: " + modelName;
    DebugConsole::GetInstance()->AddLog(lastStatus_);
}

void AssetAuditWindow::RemoveModelPreviews() {
    // 現シーンがある場合だけプレビューを取り除く。走査中に配列を変更せず、監査用の個体を集めて削除を予約する。
    if (!editor_ || !editor_->GetSceneManager() || !editor_->GetSceneManager()->GetCurrentScene()) {
        return;
    }

    BaseScene* scene = editor_->GetSceneManager()->GetCurrentScene();
    std::vector<engine::Object3D*> targets;

    for (auto& object : scene->GetObjects()) {
        if (IsAssetAuditPreviewObject(object.get())) {
            targets.push_back(object.get());
        }
    }

    for (engine::Object3D* object : targets) {
        scene->RequestRemoveObject(object);
    }

    // 削除を予約した対象があれば、画面の状態を更新する。
    if (!targets.empty()) {
        lastStatus_ = "アセット監査のモデル確認用Objectを削除しました。";
    }
}

void AssetAuditWindow::DrawAssetPreview(const nlohmann::json& item, float size) {
#ifdef USE_IMGUI
    // 小さい画像のみ内部表示し、音声・モデルはそれぞれ専用の確認経路を使う。
    const std::string path = JsonString(item, "path");
    const std::string category = JsonString(item, "category");

    // 参照パスが空の行ではプレビュー先を作れないので、空の表示で戻る。
    if (path.empty()) {
        ImGui::TextDisabled("-");
        return;
    }

    ImGui::PushID(path.c_str());

    // 画像は内蔵サムネイルの設定・容量・寸法に応じて、画面内に描くか外部確認を出す。
    if (IsTextureAssetPath(path)) {
        if (!showPreviewThumbnails_) {
            ImGui::TextDisabled("画像");

            if (ImGui::SmallButton("外部確認")) {
                OpenExternalPath(path);
            }

            ImGui::PopID();
            return;
        }

        // 一覧用の上限を超えた画像はロードを省き、外部で開く操作だけを残す。
        if (!IsSafeInlineTexturePreview(item)) {
            ImGui::TextDisabled("大きい画像");

            if (ImGui::SmallButton("外部確認")) {
                OpenExternalPath(path);
            }

            ImGui::PopID();
            return;
        }

        // 一覧サイズと拡大ツールチップは同じGPUディスクリプターを参照する。
        uint32_t handle = GetPreviewTextureHandle(path);

        // 画像を読み込めた場合はサムネイルを描き、マウスを乗せると拡大する。
        if (handle != 0) {
            const D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = engine::SrvManager::GetInstance()->GetGpuDescriptorHandle(handle);
            ImGui::Image((ImTextureID)(uintptr_t)gpuHandle.ptr, ImVec2(size, size));

            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(path.c_str());
                ImGui::Image((ImTextureID)(uintptr_t)gpuHandle.ptr, ImVec2(180.0f, 180.0f));
                ImGui::EndTooltip();
            }
        } else {
            // 画像ハンドルを得られなければ、描画の代わりに失敗を案内する。
            ImGui::TextDisabled("画像なし");
        }
    } else if (IsAudioAssetPath(path)) {
        // カテゴリとパスからBGMを判別し、単発のSEとは別の試聴操作を表示する。
        const bool isBgm = IsBgmCategory(category, path);

        if (ImGui::Button(isBgm ? "BGM試聴" : "SE試聴", ImVec2(-1.0f, 0.0f))) {
            PlayAudioPreview(path, isBgm);
        }

        // BGMとして試聴する場合にだけ停止ボタンを出す。
        if (isBgm) {
            if (ImGui::SmallButton("停止")) {
                AudioPlayer::GetInstance()->StopBGM();
            }
        }
    } else if (IsModelAssetPath(path)) {
        // 3Dモデルは画像として読み込まず、一時Objectを配置してGame Viewで確認する。
        if (ImGui::Button("3D確認", ImVec2(-1.0f, 0.0f))) {
            CreateModelPreview(path);
        }

        ImGui::TextDisabled("%s", GetModelNameFromAssetPath(path).c_str());

        // 既に一時配置があり削除ボタンが押されたときは、監査のプレビューだけを取り除く。
        if (CountModelPreviews() > 0 && ImGui::SmallButton("確認削除")) {
            RemoveModelPreviews();
        }
    } else {
        // 画像・音声・モデルのいずれにも当てはまらなければ、確認操作を出さない。
        ImGui::TextDisabled("-");
    }

    ImGui::PopID();
#else
    (void)item;
    (void)size;
#endif
}

bool AssetAuditWindow::DeleteAssetFiles(const std::string& relativePath, std::vector<std::string>& deletedPaths, std::string& errorMessage) {
    // 相対パスの保護条件、通常ファイルの存在、正規化したパスの Resources 境界を順に検査する。
    deletedPaths.clear();
    errorMessage.clear();

    const std::string normalizedPath = NormalizeSlash(relativePath);

    // ファイルを参照する前に、設定された保護パスに当たる候補を除外する。
    if (IsProtectedAssetPath(normalizedPath)) {
        errorMessage = "Resources 配下の通常アセットだけ削除できます。";
        return false;
    }

    const fs::path projectRoot = fs::current_path();
    const fs::path resourcesRoot = projectRoot / "Resources";
    const fs::path mainPath = (projectRoot / fs::path(normalizedPath)).lexically_normal();

    // 本体が通常ファイルとして存在する場合だけ後続のペア探索へ進む。
    if (!fs::exists(mainPath) || !fs::is_regular_file(mainPath)) {
        errorMessage = "対象ファイルが見つかりません: " + normalizedPath;
        return false;
    }

    // 絶対パスの表記を揃えた後、Resources配下を指す候補か調べる。
    if (!IsPathInside(mainPath, resourcesRoot)) {
        errorMessage = "Resources 配下ではないファイルは削除できません。";
        return false;
    }

    // 元画像と DDS などの対になるファイルも削除対象として明示的に列挙する。
    std::vector<fs::path> targets = BuildDeleteTargets(mainPath, resourcesRoot);

    // 本体と相方のどちらも条件を満たさなければ、削除せず失敗を返す。
    if (targets.empty()) {
        errorMessage = "削除できる対象がありません。";
        return false;
    }

    try {
        for (const fs::path& target : targets) {
            const std::string deletedPath = RelativeToProjectSlash(target);

            // 削除が成立したパスだけを一覧の更新対象にする。途中で例外が出た場合、削除済みのファイルは復元しない。
            if (fs::remove(target)) {
                deletedPaths.push_back(deletedPath);
            }
        }
    } catch (const std::exception& e) {
        errorMessage = e.what();
        return false;
    }

    return true;
}

void AssetAuditWindow::RemoveDeletedAssetsFromReport(const std::vector<std::string>& deletedPaths) {
    // 読み込み済みのレポートがあり削除されたパスが渡された場合に、表示中の一覧と集計を更新する。
    if (!hasReport_ || !latestReport_.is_object() || deletedPaths.empty()) {
        return;
    }

    std::set<std::string> deletedSet;

    for (std::string path : deletedPaths) {
        deletedSet.insert(ToLowerAscii(NormalizeSlash(std::move(path))));
    }

    auto removeFromArray = [&](const char* key) {
        // レポートに対象キーの配列がある場合だけ、今回削除した行を取り除く。
        if (!latestReport_.contains(key) || !latestReport_.at(key).is_array()) {
            return;
        }

        auto& array = latestReport_[key];
        array.erase(std::remove_if(array.begin(), array.end(), [&](const nlohmann::json& item) {
            const std::string itemPath = ToLowerAscii(NormalizeSlash(JsonString(item, "path")));
            return deletedSet.find(itemPath) != deletedSet.end();
        }), array.end());
    };

    // 未使用一覧と容量警告一覧から実際に削除した項目を除き、未使用の件数と容量を計算し直す。
    removeFromArray("unusedAssets");
    removeFromArray("heavyAssets");

    // 集計欄と未使用一覧を取得できる場合に、残った行から未使用件数と容量を再計算する。
    if (latestReport_.contains("summary") && latestReport_["summary"].is_object() &&
        latestReport_.contains("unusedAssets") && latestReport_["unusedAssets"].is_array()) {
        std::int64_t unusedBytes = 0;

        for (const auto& item : latestReport_["unusedAssets"]) {
            unusedBytes += JsonInt64(item, "sizeBytes");
        }

        latestReport_["summary"]["unusedFiles"] = latestReport_["unusedAssets"].size();
        latestReport_["summary"]["unusedBytes"] = unusedBytes;
        latestReport_["summary"]["unusedSizeText"] = FormatSizeText(unusedBytes);
    }
}

bool AssetAuditWindow::MatchesSearch(const nlohmann::json& item) const {
    // 検索語が空ならすべてに一致する。語があればカテゴリ・パス・参照元・メモから部分一致を調べる。
    if (searchBuffer_[0] == '\0') {
        return true;
    }

    std::string haystack;

    // オブジェクトの行だけから検索文字列を作り、他の型は空として扱う。
    if (item.is_object()) {
        haystack += JsonString(item, "category");
        haystack += " ";
        haystack += JsonString(item, "path");
        haystack += " ";
        haystack += JsonString(item, "source");
        haystack += " ";
        haystack += JsonString(item, "value");
        haystack += " ";

        // メモがある場合は、配列の文字を連結して検索対象に足す。
        if (item.contains("notes")) {
            haystack += JoinStringArray(item.at("notes"), "");
        }
    }

    return ToLowerAscii(haystack).find(ToLowerAscii(searchBuffer_)) != std::string::npos;
}
