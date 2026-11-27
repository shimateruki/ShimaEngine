#pragma once

// 担当：AssetのドラッグでパスとGUIDを渡し、旧パス形式も読み取る。
// 関連：ProjectWindow（送信）、DebugEditor / InspectorWindow（受信）
// 主な入口：ReadEditorAssetDragPath / ReadEditorAssetDragGuid

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

/// ImGuiのAsset Drag & DropでPathと永続GUIDを同時に渡します。
/// Payload名は既存のMODEL_ASSETを維持し、受信側は旧Path文字列も読めます。
// 関連：ProjectWindow（送信）、DebugEditor / InspectorWindow（受信）
struct EditorAssetDragPayload {
    static constexpr std::uint32_t kMagic = 0x43474144u; // CGAD
    std::uint32_t magic = kMagic;
    char guid[33]{};
    char path[512]{};
};

// 固定長バッファに収まらない文字列は末尾を切り詰め、必ず NUL 終端します。
inline EditorAssetDragPayload MakeEditorAssetDragPayload(
    const std::string& guid,
    const std::string& path) {
    EditorAssetDragPayload payload;
    const std::size_t guidLength = (std::min)(guid.size(), sizeof(payload.guid) - 1);
    const std::size_t pathLength = (std::min)(path.size(), sizeof(payload.path) - 1);
    std::memcpy(payload.guid, guid.data(), guidLength);
    std::memcpy(payload.path, path.data(), pathLength);
    payload.guid[guidLength] = '\0';
    payload.path[pathLength] = '\0';
    return payload;
}

// サイズと識別子が一致する新形式だけを受け付けます。戻り値は入力バッファ内を指します。
inline const EditorAssetDragPayload* TryReadEditorAssetDragPayload(
    const void* data,
    std::int32_t dataSize) {
    // 入力が空、または新形式の構造体とバイト数が異なる場合は、その型として読み取らない。
    if (!data || dataSize != static_cast<std::int32_t>(sizeof(EditorAssetDragPayload))) {
        return nullptr;
    }
    const auto* payload = static_cast<const EditorAssetDragPayload*>(data);
    // 構造体と同じサイズでも、識別値が一致する場合だけ新形式を採用する。
    return payload->magic == EditorAssetDragPayload::kMagic ? payload : nullptr;
}

// 旧形式のパス文字列も受け付けます。NUL がなければ payload のバイト数を上限に読みます。
inline std::string ReadEditorAssetDragPath(const void* data, std::int32_t dataSize) {
    // 新形式を認識できた場合は、固定長欄に入ったパスを返す。
    if (const EditorAssetDragPayload* payload = TryReadEditorAssetDragPayload(data, dataSize)) {
        return payload->path;
    }
    // 空の入力または読み取れる長さがない場合は、旧形式のパスも持たない。
    if (!data || dataSize <= 0) {
        return {};
    }
    const char* legacyPath = static_cast<const char*>(data);
    const std::size_t maxLength = static_cast<std::size_t>(dataSize);
    const void* terminator = std::memchr(legacyPath, '\0', maxLength);
    const std::size_t length = terminator
        ? static_cast<const char*>(terminator) - legacyPath
        : maxLength;
    return std::string(legacyPath, length);
}

// 旧形式は GUID を持たないため空文字列を返します。
inline std::string ReadEditorAssetDragGuid(const void* data, std::int32_t dataSize) {
    // 新形式を認識できた場合だけ、識別子を取り出せる。
    if (const EditorAssetDragPayload* payload = TryReadEditorAssetDragPayload(data, dataSize)) {
        return payload->guid;
    }
    return {};
}
