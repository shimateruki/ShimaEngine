#define NOMINMAX
#include <cstdint>
#include "engine/editor/core/EditorWindowsText.h"
#include "TextSpriteGenerator.h"

#include "BaseScene.h"
#include "DebugConsole.h"
#include "DebugEditor.h"
#include "EditorManager.h"
#include "IconsFontAwesome5.h"
#include "SceneManager.h"
#include "Sprite.h"
#include "SpriteDebugEditor.h"
#include "SrvManager.h"
#include "TextureManager.h"
#include "WinApp.h"
#include "imgui.h"

#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/implements.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace {
constexpr const char* kOutputDirectory = "Resources/sprite/generated/text";
constexpr const char* kPreviewDirectory = "Resources/generated/editor/text_preview";
constexpr const char* kFontDirectory = "Resources/font";
constexpr const char* kLegacyMeiryoFontPath = "Resources/sprite/meiryo.ttc";
constexpr const char* kToolExeName = "TextPngTool.exe";
constexpr const char* kToolRequestPath = "Resources/generated/editor/text_png_request.json";
constexpr const char* kToolReportPath = "Resources/generated/editor/text_png_result.json";

// 独自フォント集合のキーに並んだパスを列挙する COM オブジェクト。参照数で寿命を管理する。
class ResourceFontFileEnumerator final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IDWriteFontFileEnumerator> {
public:
    ResourceFontFileEnumerator(IDWriteFactory* factory, const void* collectionKey, UINT32 collectionKeySize)
        : factory_(factory) {
        // DirectWriteのコレクションキーを終端付きパス列として復元する。
        const wchar_t* cursor = static_cast<const wchar_t*>(collectionKey);
        const wchar_t* end = cursor + collectionKeySize / sizeof(wchar_t);

        // 並んだパスを一件ずつ辿り、キーの末尾または空の終端に着いたら列挙を終えます。
        while (cursor && cursor < end && *cursor != L'\0') {
            std::wstring path(cursor);
            fontPaths_.push_back(path);
            cursor += path.size() + 1;
        }
    }

    HRESULT STDMETHODCALLTYPE MoveNext(BOOL* hasCurrentFile) override {
        // 参照できないフォントは飛ばし、次に開けたファイルだけ列挙する。
        // 現在ファイルの有無を返す領域が渡されていなければ、COMの引数エラーにします。
        if (!hasCurrentFile)
            return E_POINTER;
        *hasCurrentFile = FALSE;
        currentFile_.Reset();

        while (nextIndex_ < fontPaths_.size()) {
            HRESULT hr = factory_->CreateFontFileReference(fontPaths_[nextIndex_].c_str(), nullptr, currentFile_.GetAddressOf());
            ++nextIndex_;

            // 参照の作成に成功し、有効なファイルを取得したときだけ列挙の現在値として返します。
            if (SUCCEEDED(hr) && currentFile_) {
                *hasCurrentFile = TRUE;
                return S_OK;
            }
        }

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCurrentFontFile(IDWriteFontFile** fontFile) override {
        // 結果の参照を書けない場合は引数エラーを返します。
        if (!fontFile)
            return E_POINTER;
        // MoveNext() で現在位置を決めていない場合は、返せるファイルがありません。
        if (!currentFile_)
            return E_FAIL;
        // 列挙済みの現在ファイルを返し、呼び出し側が解放できる COM 参照を加える。
        return currentFile_.CopyTo(fontFile);
    }

private:
    Microsoft::WRL::ComPtr<IDWriteFactory> factory_;
    std::vector<std::wstring> fontPaths_;
    size_t nextIndex_ = 0;
    Microsoft::WRL::ComPtr<IDWriteFontFile> currentFile_;
};

// DirectWrite に独自フォントの列挙器を提供する COM 実装。OS 全体へのフォント登録は行わない。
class ResourceFontCollectionLoader final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IDWriteFontCollectionLoader> {
public:
    HRESULT STDMETHODCALLTYPE CreateEnumeratorFromKey(
        IDWriteFactory* factory,
        const void* collectionKey,
        UINT32 collectionKeySize,
        IDWriteFontFileEnumerator** fontFileEnumerator) override {
        // ファクトリー、キー、列挙器を返す先のいずれかが無ければ生成しません。
        if (!factory || !collectionKey || !fontFileEnumerator)
            return E_INVALIDARG;
        auto enumerator = Microsoft::WRL::Make<ResourceFontFileEnumerator>(factory, collectionKey, collectionKeySize);
        // 列挙器を確保できなければ、メモリ不足として返します。
        if (!enumerator)
            return E_OUTOFMEMORY;
        // COMの参照カウントを維持したまま、列挙器を解放する責任を呼び出し側へ移します。
        *fontFileEnumerator = enumerator.Detach();
        return S_OK;
    }
};

using editorWindowsText::Utf8ToWide;
using editorWindowsText::WideToUtf8;

std::string ToLowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

std::string SanitizeOutputName(const char* name) {
    // Windowsで使えない文字を置き換え、生成画像の拡張子をPNGへ統一する。
    std::string result = (name && name[0] != '\0') ? name : "generated_text.png";

    for (char& c : result) {
        // Windowsのファイル名に含められない文字を含む場合は、その文字を下線に変えます。
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }

    std::filesystem::path path(result);

    // 拡張子が無ければ .png を足します。
    if (path.extension().empty()) {
        result += ".png";
    // 既存の拡張子がPNG以外なら、画像形式と一致する拡張子へ置き換えます。
    } else if (ToLowerAscii(path.extension().string()) != ".png") {
        path.replace_extension(".png");
        result = path.generic_string();
    }

    return result;
}

void CopyToBuffer(char* buffer, size_t bufferSize, const std::string& text) {
    // 書き込み先が無いか容量が0なら、文字列をコピーしません。
    if (!buffer || bufferSize == 0)
        return;
    std::snprintf(buffer, bufferSize, "%s", text.c_str());
}

// 生成名に使う32ビット値を入力の各バイトから計算します。
uint32_t Fnv1aHash(const std::string& text) {
    uint32_t hash = 2166136261u;

    for (unsigned char c : text) {
        hash ^= c;
        hash *= 16777619u;
    }

    return hash;
}

std::string MakeOutputNameFromText(const char* text) {
    // 一行目のASCII部分だけを人が読める名前に使い、全文のハッシュで同名衝突を避ける。
    std::string source = text ? text : "";
    std::string firstLine;

    for (char c : source) {
        // 最初の改行コードで止め、短い名前の候補には一行目だけを使います。
        if (c == '\r' || c == '\n')
            break;
        firstLine.push_back(c);
    }

    size_t begin = firstLine.find_first_not_of(" \t");
    size_t end = firstLine.find_last_not_of(" \t");
    firstLine = (begin == std::string::npos) ? "" : firstLine.substr(begin, end - begin + 1);

    std::string slug;

    // ファイル名に使えない文字は落とし、連続区切りと長さを制限する。
    bool previousUnderscore = false;

    for (unsigned char c : firstLine) {
        char out = '\0';

        // 英数字は小文字に正規化し、生成名の本文に残します。
        if (std::isalnum(c)) {
            out = static_cast<char>(std::tolower(c));
        // 空白・下線・ハイフンは区切りの下線に統一します。
        } else if (c == ' ' || c == '_' || c == '-') {
            out = '_';
        }

        // 今回の変換結果が区切りなら、前に続けて追加できるか判定します。
        if (out == '_') {
            // 下線が連続するときは、後の区切りを飛ばします。
            if (previousUnderscore)
                continue;
            previousUnderscore = true;
        // 下線以外で採用できる文字が来たら、次の区切りを許可します。
        } else if (out != '\0') {
            previousUnderscore = false;
        }

        // 変換対象になった文字だけを生成名の先頭部分に加えます。
        if (out != '\0') {
            slug.push_back(out);
            // 名前の先頭部分は32バイトで打ち切ります。
            if (slug.size() >= 32)
                break;
        }
    }

    while (!slug.empty() && slug.back() == '_') {
        slug.pop_back();
    }

    // 使える文字が一文字も残っていなければ、共通の接頭部分を使います。
    if (slug.empty()) {
        slug = "text";
    }

    char hashText[16];
    std::snprintf(hashText, sizeof(hashText), "%08x", Fnv1aHash(source));
    return "text_" + slug + "_" + hashText + ".png";
}

// RGBA配列をDirect2D用の色へ変換します。
D2D1_COLOR_F ToD2DColor(const float color[4]) {
    return D2D1::ColorF(color[0], color[1], color[2], color[3]);
}

// 同じRGBA配列をImGuiの32bit色へ変換します。
ImU32 ToImGuiColor(const float color[4]) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(color[0], color[1], color[2], color[3]));
}

// 画像キャンバスの寸法を切り上げ、1～4096画素へ制限します。
std::int32_t ClampCanvasSize(float value) {
    return std::clamp(static_cast<std::int32_t>(std::ceil(value)), 1, 4096);
}

// 検索欄が空の場合は、すべてのフォント名を検索に一致させます。
bool ContainsFilter(const std::string& text, const std::string& filter) {
    // 検索文字列が空のときは絞り込まず、一致扱いにします。
    if (filter.empty())
        return true;
    return ToLowerAscii(text).find(ToLowerAscii(filter)) != std::string::npos;
}

bool IsFontFile(const std::filesystem::path& path) {
    std::string extension = ToLowerAscii(path.extension().generic_string());
    return extension == ".ttf" || extension == ".otf" || extension == ".ttc";
}

std::vector<std::wstring> CollectResourceFontPaths() {
    // リソース内のフォントを収集し、旧配置のMeiryoも既存データとの互換用に含める。
    std::vector<std::wstring> paths;
    std::error_code ec;

    // 標準フォントディレクトリが見つかった場合に、その中を走査します。
    if (std::filesystem::exists(kFontDirectory, ec)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(kFontDirectory, ec)) {
            // 走査エラーを検出した場合に標準フォントの探索を打ち切ります。
            if (ec)
                break;
            // 通常ファイルでない項目はフォント候補にしません。
            if (!entry.is_regular_file(ec))
                continue;
            // TTF・OTF・TTCの拡張子でないファイルを除きます。
            if (!IsFontFile(entry.path()))
                continue;
            const std::wstring path = std::filesystem::absolute(entry.path(), ec).wstring();

            // 同じ絶対パスを以前に登録していない場合だけ一覧に加えます。
            if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
                paths.push_back(path);
            }
        }
    }

    ec.clear();
    std::filesystem::path legacyMeiryoPath(kLegacyMeiryoFontPath);

    // 旧Meiryoのファイルが存在してフォント拡張子を持つ場合は、互換用の候補にもします。
    if (std::filesystem::exists(legacyMeiryoPath, ec) && IsFontFile(legacyMeiryoPath)) {
        const std::wstring path = std::filesystem::absolute(legacyMeiryoPath, ec).wstring();

        // 旧Meiryoが同じパスで登録されていなければ候補に加えます。
        if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
            paths.push_back(path);
        }
    }

    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

std::wstring MakeFontCollectionKey(const std::vector<std::wstring>& paths) {
    // DirectWriteの列挙器へ渡すため、各パスと列全体をNULで終端する。
    std::wstring key;

    for (const std::wstring& path : paths) {
        key += path;
        key.push_back(L'\0');
    }

    key.push_back(L'\0');
    return key;
}

std::wstring GetLocalizedString(IDWriteLocalizedStrings* strings, const wchar_t* fallback) {
    // 日本語名を優先し、未収録フォントでは英語名・先頭要素の順にフォールバックする。
    // ローカライズ文字列が取得できていなければ、指定の代替文字列を使います。
    if (!strings) {
        return fallback ? fallback : L"";
    }

    UINT32 index = 0;
    BOOL exists = FALSE;
    strings->FindLocaleName(L"ja-jp", &index, &exists);

    // 日本語の名称が無い場合は、英語の名称を探します。
    if (!exists) {
        strings->FindLocaleName(L"en-us", &index, &exists);
    }

    // 日本語名も英語名も無い場合は、最初の登録名を選びます。
    if (!exists) {
        index = 0;
    }

    UINT32 length = 0;

    // 名称の長さを取得できない場合は、この文字列を読み込まず代替値を返します。
    if (FAILED(strings->GetStringLength(index, &length))) {
        return fallback ? fallback : L"";
    }

    std::wstring value(length + 1, L'\0');

    // 確保した領域へ名称を書き出せない場合も、代替値を返します。
    if (FAILED(strings->GetString(index, value.data(), length + 1))) {
        return fallback ? fallback : L"";
    }

    value.resize(length);
    return value;
}

std::string GetFontFilePathFromFont(IDWriteFont* font) {
    // DirectWriteのFontからローカルファイルの参照キーを辿り、再利用可能なパスに戻す。
    // フォントを取得できていなければ、ファイルパスを探せません。
    if (!font) {
        return {};
    }

    Microsoft::WRL::ComPtr<IDWriteFontFace> fontFace;

    // フォントの実体を取得できない場合は、参照ファイルの列挙に進みません。
    if (FAILED(font->CreateFontFace(&fontFace)) || !fontFace) {
        return {};
    }

    UINT32 fileCount = 0;

    // ファイル数を取得できないか0件なら、参照キーを辿れません。
    if (FAILED(fontFace->GetFiles(&fileCount, nullptr)) || fileCount == 0) {
        return {};
    }

    std::vector<IDWriteFontFile*> files(fileCount, nullptr);

    // GetFilesが返す生COM参照は全て解放する。先頭だけComPtrへ所有権を移す。
    // 列挙に失敗したか最初のファイルが無ければ、取得済みの生参照をすべて解放して終了します。
    if (FAILED(fontFace->GetFiles(&fileCount, files.data())) || !files[0]) {
        for (IDWriteFontFile* file : files) {
            // 取得できたファイル参照だけを解放します。
            if (file) {
                file->Release();
            }
        }

        return {};
    }

    Microsoft::WRL::ComPtr<IDWriteFontFile> fontFile;
    fontFile.Attach(files[0]);

    for (UINT32 i = 1; i < fileCount; ++i) {
        // 使うのは最初のファイルだけなので、以降の取得済み参照を解放します。
        if (files[i]) {
            files[i]->Release();
        }
    }

    const void* referenceKey = nullptr;
    UINT32 referenceKeySize = 0;

    // 参照キーの取得に失敗したら、パスを復元できません。
    if (FAILED(fontFile->GetReferenceKey(&referenceKey, &referenceKeySize))) {
        return {};
    }

    Microsoft::WRL::ComPtr<IDWriteFontFileLoader> loader;

    // ファイルのローダーを取得できなければ、その参照キーをパスとして解釈できません。
    if (FAILED(fontFile->GetLoader(&loader)) || !loader) {
        return {};
    }

    Microsoft::WRL::ComPtr<IDWriteLocalFontFileLoader> localLoader;

    // ローカルファイル用のローダーとして扱えない場合は、ファイルパスを返しません。
    if (FAILED(loader.As(&localLoader)) || !localLoader) {
        return {};
    }

    UINT32 pathLength = 0;

    // 参照キーに対応するパス長を読み取れなければ、結果を空にします。
    if (FAILED(localLoader->GetFilePathLengthFromKey(referenceKey, referenceKeySize, &pathLength))) {
        return {};
    }

    std::wstring path(pathLength + 1, L'\0');

    // ローダーからパスを取り出せない場合も空を返します。
    if (FAILED(localLoader->GetFilePathFromKey(referenceKey, referenceKeySize, path.data(), pathLength + 1))) {
        return {};
    }

    path.resize(pathLength);
    return WideToUtf8(path);
}

std::string MakeRelativeSpritePath(const std::string& fullPath) {
    // 出力先からSpriteルートへの相対化に失敗した場合はファイル名だけ採用する。
    std::filesystem::path base = "Resources/sprite";
    std::filesystem::path path = fullPath;
    std::error_code ec;
    std::filesystem::path relative = std::filesystem::relative(path, base, ec);

    // Spriteの素材ルートから相対パスにできなければ、ファイル名を返します。
    if (ec) {
        return path.filename().generic_string();
    }

    return relative.generic_string();
}

std::string EscapeJsonString(const std::string& text) {
    // 制御文字と引用符をエスケープし、日本語のUTF-8バイト列はそのまま保持する。
    std::ostringstream stream;

    for (unsigned char c : text) {
        // JSONで特別扱いになる引用符・逆斜線・制御文字には対応するエスケープを使います。
        switch (c) {
        case '\\': stream << "\\\\";
        break;

        case '"': stream << "\\\"";
        break;

        case '\b': stream << "\\b";
        break;

        case '\f': stream << "\\f";
        break;

        case '\n': stream << "\\n";
        break;

        case '\r': stream << "\\r";
        break;

        case '\t': stream << "\\t";
        break;

        default:
            // 個別の短いエスケープが無い制御文字は、16進4桁のUnicodeエスケープとして書きます。
            if (c < 0x20) {
                stream << "\\u";
                const char hex[] = "0123456789abcdef";
                stream << "00" << hex[(c >> 4) & 0x0f] << hex[c & 0x0f];
            // 制御文字以外のバイトは、UTF-8の日本語を保持できるようそのまま書きます。
            } else {
                stream << static_cast<char>(c);
            }

            break;
        }
    }

    return stream.str();
}

std::string ToAbsoluteGenericPath(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);

    // 日本語を含むプロジェクトパスでもJSONを壊さないよう、必ずUTF-8へ変換して渡す。
    return WideToUtf8((ec ? path : absolute).generic_wstring());
}

std::filesystem::path GetModuleDirectory() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));

    // モジュールのパスを取得できないか固定バッファに収まらなければ、作業ディレクトリを使います。
    if (length == 0 || length >= std::size(buffer)) {
        return std::filesystem::current_path();
    }

    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path FindTextPngToolExecutable() {
    // 実行ファイルの隣を優先し、開発中は各構成のビルド出力も探す。
    std::vector<std::filesystem::path> candidates;
    const std::filesystem::path moduleDir = GetModuleDirectory();
    const std::filesystem::path currentDir = std::filesystem::current_path();

    candidates.push_back(moduleDir / kToolExeName);
    candidates.push_back(currentDir / kToolExeName);
    candidates.push_back(currentDir / ".." / "generated" / "outputs" / "Development" / kToolExeName);
    candidates.push_back(currentDir / ".." / "generated" / "outputs" / "Debug" / kToolExeName);
    candidates.push_back(currentDir / ".." / "generated" / "outputs" / "Release" / kToolExeName);

    std::error_code ec;

    for (const std::filesystem::path& candidate : candidates) {
        // 既定の位置を優先順に調べ、存在した最初の候補を採用します。
        if (std::filesystem::exists(candidate, ec)) {
            return std::filesystem::absolute(candidate, ec);
        }

        ec.clear();
    }

    return {};
}

std::wstring QuoteCommandArg(const std::wstring& value) {
    // TextPngTool起動時に空白や引用符を含むパスを１引数として渡す。
    std::wstring result = L"\"";

    for (wchar_t c : value) {
        // 途中の引用符は引数の終端にならないようエスケープします。
        if (c == L'"') {
            result += L"\\\"";
        // 引用符以外のワイド文字はそのまま引数に含めます。
        } else {
            result += c;
        }
    }

    result += L"\"";
    return result;
}

bool RunProcessAndWait(const std::wstring& commandLine, DWORD timeoutMs, DWORD& exitCode) {
    // 外部ツールの子プロセスを画面に出さず起動し、指定ミリ秒まで終了を待ちます。
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring mutableCommand = commandLine;

    // 子プロセスを起動できなければ、Windowsのエラー値を返します。
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        exitCode = GetLastError();
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(process.hProcess, timeoutMs);

    // 待機が期限を超えた場合は子プロセスの終了を要求し、ハンドルを閉じて失敗を返します。
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        exitCode = WAIT_TIMEOUT;
        return false;
    }

    DWORD processExitCode = 1;
    GetExitCodeProcess(process.hProcess, &processExitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    exitCode = processExitCode;
    return waitResult == WAIT_OBJECT_0 && processExitCode == 0;
}

bool ReadJsonIntField(const std::string& json, const std::string& fieldName, std::int32_t& outValue) {
    // 寸法レポートから指定キーの数字列を抽出し、最小1画素に補正します。
    const std::string key = "\"" + fieldName + "\"";
    size_t pos = json.find(key);
    // 指定のキーを見つけられなければ、寸法の出力引数を保持します。
    if (pos == std::string::npos)
        return false;
    pos = json.find(':', pos + key.size());
    // キーの後に値の区切りが無ければ、その項目は読み取れません。
    if (pos == std::string::npos)
        return false;
    ++pos;

    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }

    size_t end = pos;

    while (end < json.size() && std::isdigit(static_cast<unsigned char>(json[end]))) {
        ++end;
    }

    // 空白を飛ばした先に数字が無ければ、寸法を更新しません。
    if (end == pos)
        return false;
    outValue = std::max(1, std::atoi(json.substr(pos, end - pos).c_str()));
    return true;
}

// 外部レンダラーの結果JSONから実際の画像寸法を読み、欠損時は出力値を変えず失敗を返します。
bool ReadRenderReport(const std::filesystem::path& reportPath, std::int32_t& outWidth, std::int32_t& outHeight) {
    std::ifstream file(reportPath, std::ios::binary);
    // レポートを開けない場合は、上位のキャンバス寸法で代替します。
    if (!file.is_open())
        return false;
    std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::int32_t width = 1;
    std::int32_t height = 1;
    // 幅を読み取れない場合は出力へ反映しません。
    if (!ReadJsonIntField(json, "width", width))
        return false;
    // 高さも取得できた場合にだけ、幅と高さの両方を出力します。
    if (!ReadJsonIntField(json, "height", height))
        return false;
    outWidth = width;
    outHeight = height;
    return true;
}
}

TextSpriteGenerator::~TextSpriteGenerator() {
    // 登録したローダーをFactoryより先に外し、自分が初期化したCOMだけ終了する。
    if (dwriteFactory_ && resourceFontLoader_) {
        dwriteFactory_->UnregisterFontCollectionLoader(resourceFontLoader_.Get());
    }

    resourceFontCollection_.Reset();
    resourceFontLoader_.Reset();
    dwriteFactory_.Reset();
    d2dFactory_.Reset();
    wicFactory_.Reset();

    // このインスタンスがCOMの初期化に成功していた場合だけ、対応する解放を行います。
    if (ownsCom_) {
        CoUninitialize();
        ownsCom_ = false;
    }
}

void TextSpriteGenerator::Initialize(SceneManager* sceneManager, DebugEditor* editor) {
    // 出力先とプレビュー先を分け、編集中の仮PNGが正式Assetに混ざらないようにします。
    sceneManager_ = sceneManager;
    editor_ = editor;
    std::filesystem::create_directories(kOutputDirectory);
    std::filesystem::create_directories(kPreviewDirectory);
    RefreshFonts();
    UpdateOutputNameFromText();
    previewDirty_ = true;
    previewRequestPending_ = false;
    previewDelayTimer_ = 0.0f;
}

void TextSpriteGenerator::Update() {
#ifdef USE_IMGUI
    // 通知の残りは秒で保持し、このツールでは1更新を固定の1/60秒として扱います。
    if (exportNoticeTimer_ > 0.0f) {
        exportNoticeTimer_ = std::max(0.0f, exportNoticeTimer_ - (1.0f / 60.0f));
    }

    TickPreviewAutoUpdate();
#endif
}

void TextSpriteGenerator::DrawPreview() {
#ifdef USE_IMGUI
    // 固定解像度の配置をGame Viewの実表示領域へ縮尺変換して重ねます。
    // Game Viewのプレビューが無効のときは重ね描きしません。
    if (!previewEnabled_)
        return;
    // 別のエディターツールを選んでいる場合は、文字画像を重ねません。
    if (EditorManager::GetInstance()->GetSelectedObject() != this)
        return;
    // 入力が空なら、画像や文字を表示しません。
    if (textBuffer_[0] == '\0')
        return;
    // Game Viewの幅か高さが0以下なら座標の縮尺を求めません。
    if (gameViewSize_.x <= 0.0f || gameViewSize_.y <= 0.0f)
        return;

    float scaleX = gameViewSize_.x / static_cast<float>(WinApp::GetClientWidth());
    float scaleY = gameViewSize_.y / static_cast<float>(WinApp::GetClientHeight());
    float viewScale = std::min(scaleX, scaleY);
    float sourceFontSize = std::max(4.0f, fontSize_);
    float drawScale = previewScale_ * viewScale;
    float previewFontSize = std::max(4.0f, sourceFontSize * drawScale);

    ImVec2 center = {
        gameViewOffset_.x + previewPosition_.x * scaleX,
        gameViewOffset_.y + previewPosition_.y * scaleY,
    };

    // PNGのテクスチャを取得済みなら、外部ツールが描いた画像を中心位置に重ねます。
    if (previewTextureHandle_ != 0) {
        ImVec2 canvasSize = {
            std::max(1.0f, static_cast<float>(previewWidth_)) * drawScale,
            std::max(1.0f, static_cast<float>(previewHeight_)) * drawScale,
        };

        ImVec2 canvasMin = { center.x - canvasSize.x * 0.5f, center.y - canvasSize.y * 0.5f };
        ImVec2 canvasMax = { canvasMin.x + canvasSize.x, canvasMin.y + canvasSize.y };
        ImDrawList* drawList = ImGui::GetForegroundDrawList();

        // キャンバス境界の設定が有効な場合にだけ画像の背後へ枠を表示します。
        if (previewBoundsEnabled_) {
            drawList->AddRectFilled(canvasMin, canvasMax, IM_COL32(32, 48, 64, 28));
            drawList->AddRect(canvasMin, canvasMax, IM_COL32(255, 210, 80, 190), 0.0f, 0, 1.5f);
        }

        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = engine::SrvManager::GetInstance()->GetGpuDescriptorHandle(previewTextureHandle_);
        drawList->AddImage((ImTextureID)(uintptr_t)gpuHandle.ptr, canvasMin, canvasMax);
        return;
    }

    // 生成済み PNG がない場合は ImGui のフォントを使い、編集中の余白と装飾を近似する。
    ImFont* font = ImGui::GetFont();
    ImVec2 sourceTextSize = font->CalcTextSizeA(sourceFontSize, FLT_MAX, 0.0f, textBuffer_);
    float outlinePad = outlineEnabled_ ? std::max(0.0f, outlineWidth_) : 0.0f;
    float shadowPadX = shadowEnabled_ ? std::abs(shadowOffset_[0]) : 0.0f;
    float shadowPadY = shadowEnabled_ ? std::abs(shadowOffset_[1]) : 0.0f;
    // 左右・上下の余白と縁取りに加え、影が片側へ伸びる分だけ自動キャンバスを広げる。
    float extraPadX = padding_ * 2.0f + outlinePad * 2.0f + shadowPadX;
    float extraPadY = padding_ * 2.0f + outlinePad * 2.0f + shadowPadY;

    std::int32_t sourceCanvasWidth = autoCanvas_ ? ClampCanvasSize(sourceTextSize.x + extraPadX) : std::clamp(canvasWidth_, 1, 4096);
    std::int32_t sourceCanvasHeight = autoCanvas_ ? ClampCanvasSize(sourceTextSize.y + extraPadY) : std::clamp(canvasHeight_, 1, 4096);
    ImVec2 canvasSize = {
        static_cast<float>(sourceCanvasWidth) * drawScale,
        static_cast<float>(sourceCanvasHeight) * drawScale,
    };

    ImVec2 canvasMin = { center.x - canvasSize.x * 0.5f, center.y - canvasSize.y * 0.5f };
    ImVec2 canvasMax = { canvasMin.x + canvasSize.x, canvasMin.y + canvasSize.y };
    ImVec2 pos = {
        canvasMin.x + (padding_ + outlinePad + (shadowEnabled_ ? std::max(0.0f, -shadowOffset_[0]) : 0.0f)) * drawScale,
        canvasMin.y + (padding_ + outlinePad + (shadowEnabled_ ? std::max(0.0f, -shadowOffset_[1]) : 0.0f)) * drawScale,
    };

    ImDrawList* drawList = ImGui::GetForegroundDrawList();

    // 軽量表示でも境界が有効なら、近似したキャンバスの範囲を表示します。
    if (previewBoundsEnabled_) {
        drawList->AddRectFilled(canvasMin, canvasMax, IM_COL32(32, 48, 64, 28));
        drawList->AddRect(canvasMin, canvasMax, IM_COL32(80, 210, 255, 180), 0.0f, 0, 1.5f);
    }

    drawList->PushClipRect(canvasMin, canvasMax, true);

    // 影を有効にしていれば、指定のオフセットと色で文字より先に描きます。
    if (shadowEnabled_) {
        ImVec2 shadowPos = {
            pos.x + shadowOffset_[0] * drawScale,
            pos.y + shadowOffset_[1] * drawScale,
        };

        drawList->AddText(font, previewFontSize, shadowPos, ToImGuiColor(shadowColor_), textBuffer_);
    }

    // 縁取りが有効で幅が正の場合だけ、文字の周囲に縁取り色を重ねます。
    if (outlineEnabled_ && outlineWidth_ > 0.0f) {
        // 周囲に文字を重ねて縁取りを近似する。画面のピクセルへ換算し、重ね描きの半径を制限する。
        std::int32_t radius = std::clamp(static_cast<std::int32_t>(std::ceil(outlineWidth_ * drawScale)), 1, 16);
        ImU32 outlineColor = ToImGuiColor(outlineColor_);

        for (std::int32_t y = -radius; y <= radius; ++y) {
            for (std::int32_t x = -radius; x <= radius; ++x) {
                // 中心は最後に本来の色で描くため、縁取りの重ね描きに含めません。
                if (x == 0 && y == 0)
                    continue;
                // 半径内に収まる点だけを使い、縁取りが四角くならないようにします。
                if ((x * x + y * y) > radius * radius)
                    continue;
                drawList->AddText(font, previewFontSize, { pos.x + static_cast<float>(x), pos.y + static_cast<float>(y) }, outlineColor, textBuffer_);
            }
        }
    }

    drawList->AddText(font, previewFontSize, pos, ToImGuiColor(textColor_), textBuffer_);
    drawList->PopClipRect();
#endif
}

void TextSpriteGenerator::SetGameViewRegion(const engine::Vector2& offset, const engine::Vector2& size) {
    // Previewの投影基準をGame Viewの現在の位置・大きさへ更新します。
    gameViewOffset_ = offset;
    gameViewSize_ = size;
}

void TextSpriteGenerator::DrawImGui() {
#ifdef USE_IMGUI
    // 文字や描画設定の変更でプレビューを更新対象にし、正式PNGの書き出しは出力ボタンから行います。
    bool textureChanged = false;

    ImGui::Text(ICON_FA_FONT " テキストPNG生成");
    ImGui::TextDisabled("透明PNGとして出力し、必要ならSpriteとしてシーンに追加できます。");
    ImGui::Separator();

    // Game View 上の配置値は正式PNGの内容には含めず、編集時の見え方だけを調整します。
    ImGui::Checkbox("Game Viewにプレビュー", &previewEnabled_);
    ImGui::SameLine();

    // 自動更新を有効にした瞬間に、まだ更新していない設定を描画対象にします。
    if (ImGui::Checkbox("PNG自動更新", &previewAutoUpdate_) && previewAutoUpdate_) {
        MarkPreviewDirty();
    }
    ImGui::Checkbox("透明PNG範囲を表示", &previewBoundsEnabled_);
    ImGui::DragFloat2("プレビュー位置", &previewPosition_.x, 1.0f, -4096.0f, 4096.0f);
    ImGui::DragFloat("プレビュー倍率", &previewScale_, 0.01f, 0.05f, 8.0f, "%.2f");
    ImGui::Separator();

    // 文字を変えたときは名前の自動生成も更新し、プレビューを後段で無効化します。
    if (ImGui::InputTextMultiline("文字", textBuffer_, sizeof(textBuffer_), ImVec2(-1.0f, 96.0f))) {
        textureChanged = true;

        // 自動命名が有効なときだけ、入力された文字から名前を作り直します。
        if (autoOutputName_) {
            UpdateOutputNameFromText();
        }
    }

    // 再取得を押されたらフォント一覧を再構築し、プレビューも更新対象にします。
    if (ImGui::Button(ICON_FA_SYNC_ALT " フォント再取得")) {
        RefreshFonts();
        textureChanged = true;
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputText("フォント検索", fontFilterBuffer_, sizeof(fontFilterBuffer_));

    // フォント一覧はリソースから取得した実パスと表示名が同じ添字で対応します。
    const char* selectedFont = fontNamesUtf8_.empty() ? "Meiryo" : fontNamesUtf8_[selectedFontIndex_].c_str();

    // コンボを開いたときに、検索語に一致するフォントを選べるようにします。
    if (ImGui::BeginCombo("フォント", selectedFont)) {
        std::string filter = fontFilterBuffer_;

        for (std::int32_t i = 0; i < static_cast<std::int32_t>(fontNamesUtf8_.size()); ++i) {
            // 入力した検索語に一致しない名前は候補に表示しません。
            if (!ContainsFilter(fontNamesUtf8_[i], filter))
                continue;
            bool selected = (i == selectedFontIndex_);

            // フォント候補が選ばれたら、その索引を使ってプレビューを作り直します。
            if (ImGui::Selectable(fontNamesUtf8_[i].c_str(), selected)) {
                selectedFontIndex_ = i;
                textureChanged = true;
            }

            // 現在選ばれている候補に、コンボを開いたときの既定フォーカスを合わせます。
            if (selected)
                ImGui::SetItemDefaultFocus();
        }

        ImGui::EndCombo();
    }

    // サイズ・色・余白・キャンバスの変更は再レンダリングを必要とする設定です。
    textureChanged |= ImGui::DragFloat("フォントサイズ", &fontSize_, 1.0f, 4.0f, 320.0f, "%.0f");
    textureChanged |= ImGui::Checkbox("太字", &bold_);
    textureChanged |= ImGui::DragFloat("余白", &padding_, 1.0f, 0.0f, 256.0f, "%.0f");
    textureChanged |= ImGui::ColorEdit4("文字色", textColor_);
    textureChanged |= ImGui::Checkbox("自動キャンバス", &autoCanvas_);

    // 自動サイズを無効にした場合にだけ、キャンバスのピクセル幅と高さを編集できます。
    if (!autoCanvas_) {
        textureChanged |= ImGui::DragInt("幅", &canvasWidth_, 1.0f, 1, 4096);
        textureChanged |= ImGui::DragInt("高さ", &canvasHeight_, 1.0f, 1, 4096);
    }

    ImGui::Separator();

    // アウトラインと影は透明PNG内へ焼き込まれるため、切替時もプレビューを更新します。
    textureChanged |= ImGui::Checkbox("アウトライン", &outlineEnabled_);

    // 縁取りを有効にした場合だけ、幅と色の調整を表示します。
    if (outlineEnabled_) {
        textureChanged |= ImGui::DragFloat("アウトライン幅", &outlineWidth_, 0.25f, 0.0f, 32.0f, "%.2f");
        textureChanged |= ImGui::ColorEdit4("アウトライン色", outlineColor_);
    }

    textureChanged |= ImGui::Checkbox("影", &shadowEnabled_);

    // 影が有効な場合だけ、原点からのオフセットと色を調整できます。
    if (shadowEnabled_) {
        textureChanged |= ImGui::DragFloat2("影オフセット", shadowOffset_, 0.5f, -128.0f, 128.0f);
        textureChanged |= ImGui::ColorEdit4("影色", shadowColor_);
    }

    ImGui::Separator();

    // 外部ツールによるPNG生成は明示操作か自動更新タイマーからだけ実行します。
    // 手動更新を押されたら、待機を0にしてプレビュー更新を予約します。
    if (ImGui::Button("PNGプレビュー更新")) {
        MarkPreviewDirty();
        previewRequestPending_ = true;
        previewDelayTimer_ = 0.0f;
    }

    ImGui::SameLine();

    // 現在の設定に対応するPNGを取得済みなら、その寸法を表示します。
    if (previewTextureHandle_ != 0 && !previewDirty_) {
        ImGui::TextDisabled("外部ツールPNG: %d x %d", previewWidth_, previewHeight_);
    }
    // 画像は残っていても設定変更後なら、更新が必要なことを表示します。
    else if (previewTextureHandle_ != 0) {
        ImGui::TextDisabled("設定変更あり: PNGプレビューを更新してください");
    }
    // 画像のハンドルを取得していない場合は未生成と表示します。
    else {
        ImGui::TextDisabled("外部ツールPNG: 未生成");
    }

    ImGui::Separator();

    // 手動で出力名を編集したら自動命名を解除し、次の文字変更で上書きしません。
    // 自動命名の設定が変わった場合に、現在の文字から名前を作る必要があるか判定します。
    if (ImGui::Checkbox("出力名を文字から自動生成", &autoOutputName_)) {
        // 自動命名を有効にしたときだけ、現在の文字から出力名を更新します。
        if (autoOutputName_) {
            UpdateOutputNameFromText();
        }
    }

    // 出力名を直接編集した場合は、次の文字変更で上書きされないよう自動命名を解除します。
    if (ImGui::InputText("出力名", outputNameBuffer_, sizeof(outputNameBuffer_))) {
        autoOutputName_ = false;
    }

    ImGui::SameLine();

    // 手動で文字からの更新を押したら、自動命名を有効に戻して出力名を作ります。
    if (ImGui::Button("文字から更新")) {
        autoOutputName_ = true;
        UpdateOutputNameFromText();
    }

    // Sceneへの追加は正式PNGの出力成功後にだけ行い、生成失敗時は配置しません。
    // PNG出力を押した場合に、現在の文字を正式な素材へ書き出します。
    if (ImGui::Button(ICON_FA_FILE_EXPORT " PNG出力")) {
        ExportToFile();
    }

    ImGui::SameLine();

    // Sprite追加を押した場合は、出力先を受け取れる形式でPNG生成を依頼します。
    if (ImGui::Button(ICON_FA_PLUS " Spriteとして追加")) {
        std::string fullPath;
        std::string relativePath;

        // 正式な画像の出力に成功した場合だけ、追加処理に使用するパスを渡します。
        if (ExportToFile(&fullPath, &relativePath)) {
            pendingSpriteFullPath_ = fullPath;
            pendingSpriteRelativePath_ = relativePath;
            AddPendingSpriteToScene();
        }
    }

    // 通知の残り時間が正で、表示する内容がある場合だけ結果を描きます。
    if (exportNoticeTimer_ > 0.0f && !exportNoticeMessage_.empty()) {
        const ImVec4 noticeColor = exportNoticeSuccess_
            ? ImVec4(0.36f, 1.0f, 0.58f, 1.0f)
            : ImVec4(1.0f, 0.36f, 0.28f, 1.0f);
        ImGui::TextColored(noticeColor, "%s", exportNoticeMessage_.c_str());
    }

    ImGui::TextDisabled("生成先: %s", kOutputDirectory);
    ImGui::TextDisabled("Game Viewは軽量プレビューを表示し、PNGプレビュー更新時だけ外部ツール画像に切り替えます。");
    ImGui::TextDisabled("フォント参照元: Resources/font + Resources/sprite/meiryo.ttc");
    ImGui::TextDisabled("PNG生成: 外部CLI TextPngTool.exe / DirectWrite");

    // 画面上で画像の内容に関わる項目が一つでも変わったら、プレビューを再描画対象にします。
    if (textureChanged) {
        MarkPreviewDirty();
    }
#endif
}

bool TextSpriteGenerator::EnsureFactories() {
    // 既存のスレッドでCOMを使い、WIC・Direct2D・DirectWriteの未生成のFactoryを順に用意します。
    // WICファクトリーを持っていない場合に、COMの初期化とWICの作成を試します。
    if (!wicFactory_) {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        // 成功時は既に初期化済みでも対応する解放が必要。既存のスレッド方式が異なる場合はそのまま利用する。
        if (hr == S_OK || hr == S_FALSE) {
            ownsCom_ = true;
        // 現在のCOM方式が違う場合は既存の初期化を利用します。それ以外の失敗は継続できません。
        } else if (hr != RPC_E_CHANGED_MODE) {
            DebugConsole::GetInstance()->AddLog("Text PNG: COM initialize failed.");
            return false;
        }

        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wicFactory_.GetAddressOf()));

        // WICファクトリーの作成に失敗したら、画像の符号化を準備できません。
        if (FAILED(hr)) {
            DebugConsole::GetInstance()->AddLog("Text PNG: WIC factory failed.");
            return false;
        }
    }

    // Direct2Dファクトリーが無い場合だけ作成します。
    if (!d2dFactory_) {
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.GetAddressOf());

        // Direct2Dを生成できなければ、文字を描く準備を中止します。
        if (FAILED(hr)) {
            DebugConsole::GetInstance()->AddLog("Text PNG: D2D factory failed.");
            return false;
        }
    }

    // DirectWriteファクトリーが無ければ、フォント取得用に共有ファクトリーを生成します。
    if (!dwriteFactory_) {
        HRESULT hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf()));
        // DirectWriteを取得できなければ、フォントの準備を続けません。
        if (FAILED(hr)) {
            DebugConsole::GetInstance()->AddLog("Text PNG: DirectWrite factory failed.");
            return false;
        }
    }

    return true;
}

void TextSpriteGenerator::RefreshFonts() {
    // 一覧を再取得しても選択中のフォントをパスとファミリー名で探し直します。
    std::string previousPath;
    std::string previousFamily;

    // 現在の選択が一覧の範囲内であれば、再取得後に探せるよう名前とパスを先に控えます。
    if (!fontPaths_.empty() && selectedFontIndex_ >= 0 && selectedFontIndex_ < static_cast<std::int32_t>(fontPaths_.size())) {
        previousPath = fontPaths_[selectedFontIndex_];
        previousFamily = fontNamesUtf8_[selectedFontIndex_];
    }

    // 古い一覧と DirectWrite の独自Collectionを外してから、現在のリソースフォントを登録し直します。
    fontNamesWide_.clear();
    fontNamesUtf8_.clear();
    fontPaths_.clear();

    // 描画とフォントの基盤を準備できない場合は、既定フォントだけを候補にして終了します。
    if (!EnsureFactories()) {
        fontNamesWide_.push_back(L"Meiryo UI");
        fontNamesUtf8_.push_back("Meiryo UI");
        fontPaths_.push_back("");
        selectedFontIndex_ = 0;
        return;
    }

    // 以前のローダーが登録されている場合は、古いコレクションを捨てる前に外します。
    if (dwriteFactory_ && resourceFontLoader_) {
        dwriteFactory_->UnregisterFontCollectionLoader(resourceFontLoader_.Get());
    }

    resourceFontCollection_.Reset();
    resourceFontLoader_.Reset();
    resourceFontCollectionKey_.clear();

    std::vector<std::wstring> resourceFontPaths = CollectResourceFontPaths();

    // リソースからフォントを一つも集められなければ、ファイルパスを指定しない既定フォントへ戻します。
    if (resourceFontPaths.empty()) {
        DebugConsole::GetInstance()->AddLog("Text PNG: no font files found in Resources/font or Resources/sprite/meiryo.ttc.");
        fontNamesWide_.push_back(L"Meiryo UI");
        fontNamesUtf8_.push_back("Meiryo UI");
        fontPaths_.push_back("");
        selectedFontIndex_ = 0;
        return;
    }

    // Collection Key はフォントPathの集合から作り、Loaderの寿命をCollectionより長く保ちます。
    resourceFontCollectionKey_ = MakeFontCollectionKey(resourceFontPaths);
    resourceFontLoader_ = Microsoft::WRL::Make<ResourceFontCollectionLoader>();
    HRESULT hr = resourceFontLoader_
        ? dwriteFactory_->RegisterFontCollectionLoader(resourceFontLoader_.Get())
        : E_OUTOFMEMORY;
    // 独自ローダーを登録できた場合だけ、そのローダーからリソースのフォント集合を作ります。
    if (SUCCEEDED(hr)) {
        hr = dwriteFactory_->CreateCustomFontCollection(
            resourceFontLoader_.Get(),
            resourceFontCollectionKey_.data(),
            static_cast<UINT32>(resourceFontCollectionKey_.size() * sizeof(wchar_t)),
            resourceFontCollection_.GetAddressOf());
    }

    // 集合を作れないか有効な参照が得られなければ、既定フォントにフォールバックします。
    if (FAILED(hr) || !resourceFontCollection_) {
        DebugConsole::GetInstance()->AddLog("Text PNG: resource font collection create failed.");
        fontNamesWide_.push_back(L"Meiryo UI");
        fontNamesUtf8_.push_back("Meiryo UI");
        fontPaths_.push_back("");
        selectedFontIndex_ = 0;
        return;
    }

    // Family名と実ファイルPathを同じ順に保存し、同名Familyでも別Pathなら区別します。
    const UINT32 familyCount = resourceFontCollection_->GetFontFamilyCount();

    for (UINT32 familyIndex = 0; familyIndex < familyCount; ++familyIndex) {
        Microsoft::WRL::ComPtr<IDWriteFontFamily> family;

        // 指定番号のファミリーを取得できない場合は、候補にしません。
        if (FAILED(resourceFontCollection_->GetFontFamily(familyIndex, family.GetAddressOf())) || !family) {
            continue;
        }

        Microsoft::WRL::ComPtr<IDWriteLocalizedStrings> names;

        // 言語別の名前を取得できないファミリーは、表示名を作れないため飛ばします。
        if (FAILED(family->GetFamilyNames(names.GetAddressOf())) || !names) {
            continue;
        }

        const std::wstring familyNameWide = GetLocalizedString(names.Get(), L"");
        const std::string familyNameUtf8 = WideToUtf8(familyNameWide);

        // 表示名が空のファミリーは選択候補にしません。
        if (familyNameUtf8.empty()) {
            continue;
        }

        Microsoft::WRL::ComPtr<IDWriteFont> font;
        // 通常ウェイト・幅・スタイルの候補を取得できなければ、ファイル参照を調べません。
        if (FAILED(family->GetFirstMatchingFont(
            DWRITE_FONT_WEIGHT_REGULAR,
            DWRITE_FONT_STRETCH_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            font.GetAddressOf())) || !font) {
            continue;
        }

        std::string fontPath = GetFontFilePathFromFont(font.Get());
        bool duplicate = false;

        for (size_t i = 0; i < fontNamesUtf8_.size(); ++i) {
            // 同じファミリー名とファイルパスの組み合わせで登録済みなら重複と判定します。
            if (fontNamesUtf8_[i] == familyNameUtf8 && i < fontPaths_.size() && fontPaths_[i] == fontPath) {
                duplicate = true;
                break;
            }
        }

        // すでにある組み合わせは一覧へ追加しません。
        if (duplicate) {
            continue;
        }

        fontNamesWide_.push_back(familyNameWide);
        fontNamesUtf8_.push_back(familyNameUtf8);
        fontPaths_.push_back(fontPath);
    }

    // 選べるファミリーが一件も無い場合は、既定フォントの表示を追加します。
    if (fontNamesWide_.empty()) {
        fontNamesWide_.push_back(L"Meiryo UI");
        fontNamesUtf8_.push_back("Meiryo UI");
        fontPaths_.push_back("");
    }

    // 再取得前と同じFamily/Pathが残っていれば選択を戻します。
    selectedFontIndex_ = 0;

    // 再取得前のファミリー名を保持している場合は、元の選択を探します。
    if (!previousFamily.empty()) {
        for (std::int32_t i = 0; i < static_cast<std::int32_t>(fontNamesUtf8_.size()); ++i) {
            // 名前が一致し、元のパスが空かパスも一致する場合に以前の選択を復元します。
            if (fontNamesUtf8_[i] == previousFamily && (previousPath.empty() || fontPaths_[i] == previousPath)) {
                selectedFontIndex_ = i;
                MarkPreviewDirty();
                return;
            }
        }
    }

    for (std::int32_t i = 0; i < static_cast<std::int32_t>(fontNamesUtf8_.size()); ++i) {
        std::string lower = ToLowerAscii(fontNamesUtf8_[i]);

        // 元の選択を復元できないときは、M PLUSまたはMeiryoを含む最初の候補を優先します。
        if (lower.find("mplus") != std::string::npos || lower.find("m plus") != std::string::npos || lower.find("meiryo") != std::string::npos) {
            selectedFontIndex_ = i;
            break;
        }
    }

    MarkPreviewDirty();
}

bool TextSpriteGenerator::RenderToFile(const std::string& fullPath, std::int32_t& outWidth, std::int32_t& outHeight) {
    // TextPngToolへ要求ファイルを渡し、生成されたPNGの寸法を結果として受け取ります。
    const std::filesystem::path toolExe = FindTextPngToolExecutable();

    // 外部PNGツールが見つからなければ、要求ファイルの準備に進みません。
    if (toolExe.empty()) {
        DebugConsole::GetInstance()->AddLog("Text PNG: TextPngTool.exe not found. Build the solution first.");
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(fullPath).parent_path(), ec);
    std::filesystem::create_directories(std::filesystem::path(kToolRequestPath).parent_path(), ec);

    // UIの選択値を安全な範囲へ収め、CLIが読める絶対PathとUTF-8文字列へ変換します。
    const std::int32_t selectedIndex = fontPaths_.empty()
        ? 0
        : std::clamp(selectedFontIndex_, 0, static_cast<std::int32_t>(fontPaths_.size()) - 1);
    const std::string selectedFontPath = fontPaths_.empty() ? std::string() : fontPaths_[selectedIndex];
    const std::string selectedFontFamily = fontNamesUtf8_.empty() ? std::string("Meiryo UI") : fontNamesUtf8_[selectedIndex];
    const std::filesystem::path requestPath = kToolRequestPath;
    const std::filesystem::path reportPath = kToolReportPath;
    const std::string outputPath = ToAbsoluteGenericPath(fullPath);
    const std::string reportPathUtf8 = ToAbsoluteGenericPath(reportPath);

    // 文字や日本語PathはJSONへエスケープし、外部CLIへの入力を一つの要求ファイルにまとめます。
    std::ofstream request(requestPath, std::ios::binary | std::ios::trunc);

    // 要求ファイルを作れなければ、外部ツールを起動せず失敗を返します。
    if (!request.is_open()) {
        DebugConsole::GetInstance()->AddLog("Text PNG: request file create failed.");
        return false;
    }

    request
        << "{\n"
        << "  \"text\": \"" << EscapeJsonString(textBuffer_) << "\",\n"
        << "  \"fontPath\": \"" << EscapeJsonString(selectedFontPath) << "\",\n"
        << "  \"fontFamilyName\": \"" << EscapeJsonString(selectedFontFamily) << "\",\n"
        << "  \"bold\": " << (bold_ ? "true" : "false") << ",\n"
        << "  \"fontSize\": " << std::max(4.0f, fontSize_) << ",\n"
        << "  \"padding\": " << std::max(0.0f, padding_) << ",\n"
        << "  \"autoCanvas\": " << (autoCanvas_ ? "true" : "false") << ",\n"
        << "  \"canvasWidth\": " << std::clamp(canvasWidth_, 1, 4096) << ",\n"
        << "  \"canvasHeight\": " << std::clamp(canvasHeight_, 1, 4096) << ",\n"
        << "  \"textColor\": [" << textColor_[0] << ", " << textColor_[1] << ", " << textColor_[2] << ", " << textColor_[3] << "],\n"
        << "  \"outline\": { \"enabled\": " << (outlineEnabled_ ? "true" : "false")
        << ", \"width\": " << std::max(0.0f, outlineWidth_)
        << ", \"color\": [" << outlineColor_[0] << ", " << outlineColor_[1] << ", " << outlineColor_[2] << ", " << outlineColor_[3] << "] },\n"
        << "  \"shadow\": { \"enabled\": " << (shadowEnabled_ ? "true" : "false")
        << ", \"offset\": [" << shadowOffset_[0] << ", " << shadowOffset_[1] << "]"
        << ", \"color\": [" << shadowColor_[0] << ", " << shadowColor_[1] << ", " << shadowColor_[2] << ", " << shadowColor_[3] << "] },\n"
        << "  \"reportPath\": \"" << EscapeJsonString(reportPathUtf8) << "\"\n"
        << "}\n";
    request.close();

    // 要求 JSON を閉じてから外部ツールを起動し、完全に書き出した内容を読み込ませる。
    const std::wstring commandLine =
        QuoteCommandArg(toolExe.wstring()) +
        L" render -config " + QuoteCommandArg(std::filesystem::absolute(requestPath, ec).wstring()) +
        L" -out " + QuoteCommandArg(std::filesystem::absolute(std::filesystem::path(fullPath), ec).wstring());

    // 外部プロセスの終了とPNGの存在を別々に確認します。
    DWORD exitCode = 1;

    // 15秒以内に正常終了できなかった場合は、ツールの終了値をログへ残します。
    if (!RunProcessAndWait(commandLine, 15000, exitCode)) {
        DebugConsole::GetInstance()->AddLog("Text PNG: external tool failed. exit=" + std::to_string(exitCode));
        return false;
    }

    // ツールが正常終了しても指定先にPNGがなければ、生成は失敗とします。
    if (!std::filesystem::exists(fullPath)) {
        DebugConsole::GetInstance()->AddLog("Text PNG: output file was not created.");
        return false;
    }

    // レポートがない場合は設定したキャンバス寸法を返し、プレビュー作成自体は続けます。
    if (!ReadRenderReport(reportPath, outWidth, outHeight)) {
        outWidth = autoCanvas_ ? std::max(1, canvasWidth_) : std::clamp(canvasWidth_, 1, 4096);
        outHeight = autoCanvas_ ? std::max(1, canvasHeight_) : std::clamp(canvasHeight_, 1, 4096);
    }

    return true;
}

// 文字入力中の連続再生成を避け、変更が落ち着いてからプレビューPNGを更新します。
void TextSpriteGenerator::TickPreviewAutoUpdate() {
#ifdef USE_IMGUI
    // 自動更新が無効、プレビューが無効、または未反映の設定変更が無い場合は、追加の要求を出しません。
    if (!previewAutoUpdate_ || !previewEnabled_ || !previewDirty_) {
        return;
    }

    // 入力が空になった場合は待機せず、現在のプレビューを解除します。
    if (textBuffer_[0] == '\0') {
        UpdatePreviewTexture();
        return;
    }

    // 入力中やドラッグ中に毎フレーム外部CLIを起動しないよう、短い待ち時間を挟む。
    if (!previewRequestPending_) {
        previewRequestPending_ = true;
        previewDelayTimer_ = 0.12f;
    }

    // 待ち時間の単位は秒です。通知表示と同様に、1更新あたり固定の1/60秒ずつ減らします。
    previewDelayTimer_ = std::max(0.0f, previewDelayTimer_ - (1.0f / 60.0f));

    // 遅延が終わった時点で保留を解除し、外部PNG更新を試します。
    if (previewDelayTimer_ <= 0.0f) {
        previewRequestPending_ = false;
        UpdatePreviewTexture();
    }
#endif
}

void TextSpriteGenerator::UpdatePreviewTexture() {
    // 編集中のPNGは4つの仮ファイル名を循環利用し、プレビュー用ファイルの増加を抑えます。
    // 設定が変わっていなければ、同じプレビューを再生成しません。
    if (!previewDirty_)
        return;
    // 入力が空の場合は画像ハンドルを解除し、寸法を初期値へ戻します。
    if (textBuffer_[0] == '\0') {
        previewTextureHandle_ = 0;
        previewWidth_ = 1;
        previewHeight_ = 1;
        previewDirty_ = false;
        return;
    }

    char fileName[64];
    std::snprintf(fileName, sizeof(fileName), "_preview_%04d.png", previewSerial_++ % 4);
    std::filesystem::path path = std::filesystem::path(kPreviewDirectory) / fileName;

    std::int32_t width = 1;
    std::int32_t height = 1;

    // 仮PNGの生成に失敗した場合は、1秒後に試し直せる状態を残します。
    if (!RenderToFile(path.generic_string(), width, height)) {
        DebugConsole::GetInstance()->AddLog("Text PNG: preview render failed.");
        previewRequestPending_ = true;
        previewDelayTimer_ = 1.0f;
        return;
    }

    // 仮ファイル名を使い回しても古い画像が残らないよう再読み込みし、成功後にだけ表示を切り替える。
    uint32_t handle = engine::TextureManager::GetInstance()->Load(path.generic_string(), false, false, true);

    // 画像のGPUハンドルを取得できなければ、新しい画像へ切り替えず再試行を予約します。
    if (handle == 0) {
        previewRequestPending_ = true;
        previewDelayTimer_ = 1.0f;
        return;
    }

    previewWidth_ = width;
    previewHeight_ = height;
    previewTextureHandle_ = handle;
    previewDirty_ = false;
    previewRequestPending_ = false;
    previewDelayTimer_ = 0.0f;
}

bool TextSpriteGenerator::ExportToFile(std::string* outFullPath, std::string* outRelativePath) {
    // 出力名を安全なファイル名に揃え、成功した正式Assetのパスだけ呼び出し側へ渡します。
    std::string fileName = SanitizeOutputName(outputNameBuffer_);
    std::filesystem::path fullPath = std::filesystem::path(kOutputDirectory) / fileName;

    std::int32_t width = 1;
    std::int32_t height = 1;

    // PNGを生成できなければ出力パスを返さず、失敗の通知を2.5秒だけ表示します。
    if (!RenderToFile(fullPath.generic_string(), width, height)) {
        DebugConsole::GetInstance()->AddLog("Text PNG: export failed.");
        exportNoticeSuccess_ = false;
        exportNoticeTimer_ = 2.5f;
        exportNoticeMessage_ = "PNG出力に失敗しました";
        return false;
    }

    pendingSpriteWidth_ = width;
    pendingSpriteHeight_ = height;

    // 呼び出し側が実ファイルへのパスを要求している場合だけ、そのパスを返します。
    if (outFullPath)
        *outFullPath = fullPath.generic_string();
    // 呼び出し側がSprite用の相対パスを要求している場合だけ、素材のルートからのパスを返します。
    if (outRelativePath)
        *outRelativePath = MakeRelativeSpritePath(fullPath.generic_string());

    DebugConsole::GetInstance()->AddLog("Text PNG exported: " + fullPath.generic_string());
    exportNoticeSuccess_ = true;
    exportNoticeTimer_ = 2.5f;
    exportNoticeMessage_ = "PNG出力完了: " + fileName;
    return true;
}

void TextSpriteGenerator::AddPendingSpriteToScene() {
    // 出力後の追加要求を一度だけ処理し、Sceneが閉じている場合は生成しません。
    pendingAddSprite_ = false;

    // シーン管理が利用できなければ、配置対象のシーンを取り出せません。
    if (!sceneManager_)
        return;
    BaseScene* scene = sceneManager_->GetCurrentScene();
    // 現在のシーンかSpriteの共通描画設定が無い場合は実体を作りません。
    if (!scene || !scene->GetSpriteCommon())
        return;

    uint32_t handle = engine::TextureManager::GetInstance()->Load(pendingSpriteFullPath_, false, false, true);
    // 生成されたPNGから有効なテクスチャを読み込めなければ配置しません。
    if (handle == 0)
        return;

    auto sprite = std::make_unique<engine::Sprite>();
    sprite->Initialize(scene->GetSpriteCommon(), handle);
    sprite->SetTextureName(pendingSpriteRelativePath_);
    sprite->SetName(std::filesystem::path(pendingSpriteRelativePath_).stem().generic_string());
    // プレビューと同じ中央アンカーを使い、生成画像の画像寸法と倍率を実際の配置へ引き継ぐ。
    sprite->SetAnchorPoint({ 0.5f, 0.5f });
    sprite->SetPosition(previewPosition_);
    sprite->SetSize({ pendingSpriteWidth_ * previewScale_, pendingSpriteHeight_ * previewScale_ });
    sprite->SetColor({ 1.0f, 1.0f, 1.0f, 1.0f });
    sprite->Update();

    engine::Sprite* addedSprite = sprite.get();
    scene->AddSprite(std::move(sprite));

    // Sprite編集ツールを取得できる場合は、追加したSpriteがそのまま編集対象になります。
    if (editor_ && editor_->GetSpriteDebugEditor()) {
        editor_->GetSpriteDebugEditor()->SetSelectedSprite(addedSprite);
        EditorManager::GetInstance()->SetSelectedObject(editor_->GetSpriteDebugEditor());
    }

    DebugConsole::GetInstance()->AddLog("Text PNG sprite added: " + pendingSpriteRelativePath_);
}

void TextSpriteGenerator::MarkPreviewDirty() {
    // 設定変更時は次の更新で描き直し、保留中の古い要求を取り消します。
    previewDirty_ = true;
    previewRequestPending_ = false;
}

void TextSpriteGenerator::UpdateOutputNameFromText() {
    // 新しい文字列から候補名だけを作り、正式出力はまだ行いません。
    CopyToBuffer(outputNameBuffer_, sizeof(outputNameBuffer_), MakeOutputNameFromText(textBuffer_));
}

