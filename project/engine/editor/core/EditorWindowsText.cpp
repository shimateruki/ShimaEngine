#define NOMINMAX
#include <cstdint>
#include "EditorWindowsText.h"

#include <Windows.h>

namespace editorWindowsText {

// Win32の広文字APIへ渡すためUTF-8を変換し、無効な入力では空文字を返します。
std::wstring Utf8ToWide(const std::string& text) {
    // 入力が空ならWindows APIを呼ばず、空の変換結果を返す。
    if (text.empty()) {
        return {};
    }

    // -1指定で終端文字も変換されるため、その1文字分まで確保してから取り除きます。
    const std::int32_t required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, nullptr, 0);

    // 必要な領域を数えられなければ、無効なUTF-8として扱い変換を中止する。
    if (required <= 0) {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(required), L'\0');

    // 想定した数のUTF-16コード単位を書けなければ、不完全な文字列を返さない。
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, result.data(), required) != required) {
        return {};
    }

    result.pop_back();
    return result;
}

std::string WideToUtf8(const std::wstring& text) {
    // 空のUTF-16入力は変換を呼び出す必要がない。
    if (text.empty()) {
        return {};
    }

    // 終端文字を含めた長さで確保し、APIが書き込む範囲を越えないようにします。
    const std::int32_t required = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);

    // 必要なバイト数を取得できなければ、UTF-8用の領域を作らない。
    if (required <= 0) {
        return {};
    }

    std::string result(static_cast<std::size_t>(required), '\0');

    // 必要数を書けなかったときは、途中までのUTF-8を廃棄する。
    if (WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), required, nullptr, nullptr) != required) {
        return {};
    }

    result.pop_back();
    return result;
}

} // namespace editorWindowsText
