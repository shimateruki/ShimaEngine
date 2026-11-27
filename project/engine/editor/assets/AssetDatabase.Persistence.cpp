#define NOMINMAX
#include <cstdint>
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

AssetDatabase::MetaLoadResult AssetDatabase::LoadOrCreateMeta(
    const fs::path& sourcePath,
    bool createMissingMeta,
    std::unordered_map<std::string, std::string>& guidOwners) {
    // 既存.metaを優先し、不正なGUIDは元ファイルを保護して索引だけPath参照に落とします。
    MetaLoadResult result;
    result.success = true;
    result.record.sourcePath = NormalizeProjectPath(sourcePath);
    fs::path metaPath = sourcePath;
    metaPath += ".meta";
    result.record.metaPath = NormalizeProjectPath(metaPath);
    result.record.type = DetectAssetType(sourcePath);
    result.record.importer = GetDefaultImporter(result.record.type);
    result.record.importSettings = GetDefaultImportSettings(result.record.type);

    // 元ファイルの印を記録し、次回の永続索引が流用できるか判断する材料にします。
    std::error_code error;
    result.record.fileSize = fs::file_size(sourcePath, error);

    // 元ファイルのサイズを取得できなければ、索引上は0として他の情報の調査を続ける。
    if (error) {
        result.record.fileSize = 0;
        error.clear();
    }

    result.record.lastWriteTime = fs::last_write_time(sourcePath, error);

    json metadata;
    bool metadataExists = fs::exists(metaPath, error);

    // メタファイルがあれば、パスだけの登録にする前に既存の定義を読む。
    if (metadataExists) {
        // 既存のImport設定は既定値より優先し、壊れた.metaは自動上書きしません。
        try {
            std::ifstream input(metaPath, std::ios::binary);

            // 既存のメタファイルを開けなければ、診断と元のパス情報を残して戻る。
            if (!input.is_open()) {
                throw std::runtime_error("ファイルを開けません");
            }

            input >> metadata;
        }
        catch (const std::exception& exception) {
            AddIssue(
                AssetDatabaseIssueSeverity::kError,
                result.record.metaPath,
                std::string(".metaを読み込めません: ") + exception.what());
            return result;
        }

        result.record.guid = ToLowerAscii(metadata.value("guid", std::string()));

        // 小文字に揃えても有効な32桁の GUID にならない場合、パスだけで登録する。
        if (!IsGuidValid(result.record.guid)) {
            AddIssue(
                AssetDatabaseIssueSeverity::kError,
                result.record.metaPath,
                "有効な32桁GUIDがありません。元.metaは保護したまま索引へ登録します。");
            result.record.guid.clear();
            return result;
        }

        // インポート設定がオブジェクトなら、型ごとの既定値に代えて利用する。
        if (metadata.contains("importSettings") && metadata["importSettings"].is_object()) {
            result.record.importSettings = metadata["importSettings"];
        }
    }
    // メタファイルが無い場合、作成が許可された索引処理でのみ GUID を発行する。
    else if (createMissingMeta) {
        // 初回索引作成で許可された場合だけ新しいGUIDを発行します。
        result.record.guid = GenerateGuid();
        std::string writeError;

        // 新規メタを書けなかった場合は、実ファイルと一致しない GUID を索引へ残さない。
        if (!WriteMeta(result.record, &writeError)) {
            AddIssue(AssetDatabaseIssueSeverity::kError, result.record.metaPath, writeError);
            result.record.guid.clear();
            return result;
        }

        result.created = true;
    }
    // 新規作成が許可されていなければ、パスだけの登録として警告を出す。
    else {
        AddIssue(
            AssetDatabaseIssueSeverity::kWarning,
            result.record.sourcePath,
            ".metaがないためPath参照だけで登録しました。");
        return result;
    }

    // 同一走査でGUIDが重複した場合、通常の索引更新では元.metaを保護します。
    // 新規作成モードでだけ新GUIDを書き、既存参照との衝突を解消します。
    const auto owner = guidOwners.find(result.record.guid);

    // 今回の索引で既に他の素材が同じ GUID を使う場合に、重複処理へ進む。
    if (!result.record.guid.empty() && owner != guidOwners.end()) {
        // 通常の走査では既存メタを保持し、後から見つかった素材の GUID だけを索引から外す。
        if (!createMissingMeta) {
            AddIssue(
                AssetDatabaseIssueSeverity::kError,
                result.record.metaPath,
                "GUIDが重複しています: " + owner->second);
            result.record.guid.clear();
            return result;
        }

        const std::string duplicateGuid = result.record.guid;
        result.record.guid = GenerateGuid();
        std::string writeError;

        // 重複解消を書き込めなかった場合は、新しい GUID を採用せず理由を記録する。
        if (!WriteMeta(result.record, &writeError)) {
            AddIssue(AssetDatabaseIssueSeverity::kError, result.record.metaPath, writeError);
            result.record.guid.clear();
            return result;
        }

        result.updated = true;
        AddIssue(
            AssetDatabaseIssueSeverity::kWarning,
            result.record.metaPath,
            "重複GUID " + duplicateGuid + " を新しいGUIDへ更新しました。");
    }

    // 有効な GUID を保持できた素材だけ、今回の走査での先着の所有者にする。
    if (!result.record.guid.empty()) {
        guidOwners[result.record.guid] = result.record.sourcePath;
    }

    // GUID以外のMeta情報が古いときは、既存Import設定を残したまま形式だけ更新します。
    // すでにメタがある素材について、形式や登録パスが現在の正規値に合うか調べる。
    if (metadataExists) {
        const bool needsUpdate = metadata.value("version", 0) != kAssetMetaVersion ||
            metadata.value("source", std::string()) != result.record.sourcePath ||
            metadata.value("assetType", std::string()) != GetAssetTypeName(result.record.type) ||
            metadata.value("importer", std::string()) != result.record.importer;
        // バージョン・パス・型・取込方式が変わっていれば、形式の書き直しを試す。
        if (needsUpdate) {
            std::string writeError;

            // 書き直しが成功した場合に、更新されたメタとして結果を記録する。
            if (WriteMeta(result.record, &writeError)) {
                result.updated = true;
            }
            // 書き直せなければ、索引自体に取れた情報は残し、保存失敗の理由を報告する。
            else {
                AddIssue(AssetDatabaseIssueSeverity::kError, result.record.metaPath, writeError);
            }
        }
    }

    return result;
}

bool AssetDatabase::WriteMeta(const EditorAssetRecord& record, std::string* errorMessage) const {
    // 途中失敗で既存.metaを壊さないよう、一時ファイルへ書いてから置き換えます。
    if (!IsGuidValid(record.guid)) {
        // 診断の出力先が指定されている場合は、GUID の検証エラーを返す。
        if (errorMessage) {
            *errorMessage = "無効なGUIDのため.metaを書き込めません。";
        }

        return false;
    }

    json metadata;
    metadata["version"] = kAssetMetaVersion;
    metadata["guid"] = ToLowerAscii(record.guid);
    metadata["source"] = record.sourcePath;
    metadata["assetType"] = GetAssetTypeName(record.type);
    metadata["importer"] = record.importer;
    metadata["importSettings"] = record.importSettings.is_object()
        ? record.importSettings
        : json::object();

    // 不正なImport設定等があってもEditorを終了させず、既存.metaを保護して失敗を返す。
    std::string serialized;
    try {
        serialized = metadata.dump(2);
    }
    catch (const std::exception& exception) {
        // エラーの受け取り先が指定されていれば、変換時の例外メッセージをコピーする。
        if (errorMessage) {
            *errorMessage = ".metaのJSON変換に失敗しました: " + std::string(exception.what());
        }
        return false;
    }

    // Metaファイルと同じ場所の一時ファイルを確定後に置き換えます。
    const fs::path metaPath = ResolveAbsolutePath(record.metaPath);
    fs::path temporaryPath = metaPath;
    temporaryPath += ".tmp";
    std::error_code error;
    fs::create_directories(metaPath.parent_path(), error);

    // 保存フォルダーの作成に失敗した場合、一時ファイルを開かない。
    if (error) {
        // 出力先があれば OS のフォルダーエラーを渡す。
        if (errorMessage) {
            *errorMessage = ".metaフォルダを作成できません: " + error.message();
        }

        return false;
    }

    {
        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);

        // 一時ファイルを開けない場合は既存のメタをそのまま保つ。
        if (!output.is_open()) {
            // 任意のエラーの出力先がある場合にだけ、一時ファイルの問題を返す。
            if (errorMessage) {
                *errorMessage = ".meta一時ファイルを開けません。";
            }

            return false;
        }

        output << serialized << '\n';
        output.flush();

        // 書き込みや flush に失敗した場合は、途中の内容で置き換えない。
        if (!output.good()) {
            // 出力先がある場合にだけ、書き込み失敗の理由を返す。
            if (errorMessage) {
                *errorMessage = ".meta一時ファイルの書き込みに失敗しました。";
            }

            return false;
        }
    }

    // Windows側で既存ファイルを置き換え、書き込み完了まで要求します。
    if (!MoveFileExW(
        temporaryPath.c_str(),
        metaPath.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD windowsError = GetLastError();
        fs::remove(temporaryPath, error);

        // 置換失敗時の Windows エラーは、呼出元が詳細を受け取れる場合に返す。
        if (errorMessage) {
            *errorMessage = ".metaを確定できません。Windows Error: " + std::to_string(windowsError);
        }

        return false;
    }

    return true;
}

bool AssetDatabase::LoadPersistentIndexCache() {
    // キャッシュは高速化のための補助情報です。欠損や形式違いは再走査で回復します。
    persistentAssetsByPath_.clear();
    persistentIndexCacheAvailable_ = false;

    const fs::path cachePath = resourcesRoot_ / kPersistentIndexCachePath;
    std::error_code error;

    // キャッシュが無いか存在確認に失敗した場合は、キャッシュなしで続行する。
    if (!fs::exists(cachePath, error) || error) {
        return false;
    }

    try {
        std::ifstream input(cachePath, std::ios::binary);

        // 存在しても開けない場合は、例外経路で全解析へ切り替える理由を記録する。
        if (!input.is_open()) {
            throw std::runtime_error("キャッシュファイルを開けません");
        }

        // Cacheの版だけでなく対象ResourcesのRootも照合し、別プロジェクトの情報を使いません。
        json document;
        input >> document;
        // オブジェクト形式、2種類の版、対象ルート、素材配列が全部一致する場合だけ内容を調べる。
        if (!document.is_object() ||
            document.value("version", 0) != kPersistentIndexCacheVersion ||
            document.value("assetMetaVersion", 0) != kAssetMetaVersion ||
            document.value("resourcesRoot", std::string()) != resourcesRootPath_ ||
            !document.contains("assets") ||
            !document["assets"].is_array()) {
            throw std::runtime_error("キャッシュ形式または対象Resourcesが一致しません");
        }

        // 全項目を一時Mapに検証してから採用し、途中で不正データが出たら一件も流用しません。
        std::unordered_map<std::string, PersistentAssetEntry> loadedAssets;
        loadedAssets.reserve(document["assets"].size());

        for (const json& item : document["assets"]) {
            // 素材の各行はオブジェクトを要求する。一件でも異なる形式ならキャッシュ全体を無効にする。
            if (!item.is_object()) {
                throw std::runtime_error("Asset項目がObjectではありません");
            }

            PersistentAssetEntry entry;
            entry.record.guid = ToLowerAscii(item.value("guid", std::string()));
            entry.record.sourcePath = item.value("sourcePath", std::string());
            entry.record.metaPath = item.value("metaPath", std::string());
            entry.record.importer = item.value("importer", std::string());
            entry.record.fileSize = item.value("fileSize", std::uintmax_t{0});
            entry.sourceWriteTimeTicks = item.value("sourceWriteTimeTicks", std::int64_t{0});
            entry.metaFileSize = item.value("metaFileSize", std::uintmax_t{0});
            entry.metaWriteTimeTicks = item.value("metaWriteTimeTicks", std::int64_t{0});
            entry.metaExists = item.value("metaExists", false);
            entry.record.importSettings =
                item.contains("importSettings") && item["importSettings"].is_object()
                ? item["importSettings"]
                : json::object();

            const std::int32_t assetType = item.value("assetType", -1);
            // 型の番号が既知の列挙範囲から外れていれば、別の形式として受け付けない。
            if (assetType < static_cast<std::int32_t>(EditorAssetType::kUnknown) ||
                assetType > static_cast<std::int32_t>(EditorAssetType::kBinary)) {
                throw std::runtime_error("Asset種別が範囲外です");
            }

            entry.record.type = static_cast<EditorAssetType>(assetType);

            // 両方のパスが必須。パスだけの登録では空の GUID を許容し、空でない場合は検証する。
            if (entry.record.sourcePath.empty() ||
                entry.record.metaPath.empty() ||
                (!entry.record.guid.empty() && !IsGuidValid(entry.record.guid))) {
                throw std::runtime_error("Asset項目の必須情報が不正です");
            }

            // 相対PathがResources外へ抜けないことと、Metaが元Assetに対応することを確認します。
            const fs::path sourceAbsolute = ResolveAbsolutePath(entry.record.sourcePath);
            fs::path expectedMetaAbsolute = sourceAbsolute;
            expectedMetaAbsolute += ".meta";
            // 原本が Resources 内で、元パスと対応する .meta パスが正規の表記に一致する場合だけ索引へ採用する。
            if (!IsPathInsideResources(sourceAbsolute) ||
                entry.record.sourcePath != NormalizeProjectPath(sourceAbsolute) ||
                entry.record.metaPath != NormalizeProjectPath(expectedMetaAbsolute)) {
                throw std::runtime_error("Resources外または不正なAssetパスが含まれています");
            }

            const std::string key = ToLowerAscii(entry.record.sourcePath);

            // 正規化後に同じ素材のパスが重なるキャッシュは、どちらの項目も信用せず再走査する。
            if (!loadedAssets.emplace(key, std::move(entry)).second) {
                throw std::runtime_error("同じAssetパスが重複しています");
            }
        }

        persistentAssetsByPath_ = std::move(loadedAssets);
        persistentIndexCacheAvailable_ = true;
        return true;
    }
    catch (const std::exception& exception) {
        persistentAssetsByPath_.clear();
        AddIssue(
            AssetDatabaseIssueSeverity::kWarning,
            NormalizeProjectPath(cachePath),
            std::string("永続索引を読み込めないため安全な全解析へ切り替えました: ") + exception.what());
        return false;
    }
}

bool AssetDatabase::SavePersistentIndexCache() {
    // 次回起動時に変更のない資産を再利用できるよう、今回の結果を保存します。
    const fs::path cachePath = resourcesRoot_ / kPersistentIndexCachePath;
    fs::path temporaryPath = cachePath;
    temporaryPath += ".tmp";

    // Mapの列挙順に依存しない安定したJSONを作り、差分と再現性を保ちます。
    std::vector<const PersistentAssetEntry*> sortedEntries;
    sortedEntries.reserve(persistentAssetsByPath_.size());

    for (const auto& pair : persistentAssetsByPath_) {
        sortedEntries.push_back(&pair.second);
    }

    std::sort(
        sortedEntries.begin(),
        sortedEntries.end(),
        [](const PersistentAssetEntry* left, const PersistentAssetEntry* right) {
            return left->record.sourcePath < right->record.sourcePath;
        });

    json document;
    document["version"] = kPersistentIndexCacheVersion;
    document["assetMetaVersion"] = kAssetMetaVersion;
    document["resourcesRoot"] = resourcesRootPath_;
    document["assets"] = json::array();

    for (const PersistentAssetEntry* entry : sortedEntries) {
        json item;
        item["guid"] = entry->record.guid;
        item["sourcePath"] = entry->record.sourcePath;
        item["metaPath"] = entry->record.metaPath;
        item["assetType"] = static_cast<std::int32_t>(entry->record.type);
        item["importer"] = entry->record.importer;
        item["fileSize"] = entry->record.fileSize;
        item["sourceWriteTimeTicks"] = entry->sourceWriteTimeTicks;
        item["metaFileSize"] = entry->metaFileSize;
        item["metaWriteTimeTicks"] = entry->metaWriteTimeTicks;
        item["metaExists"] = entry->metaExists;
        item["importSettings"] = entry->record.importSettings.is_object()
            ? entry->record.importSettings
            : json::object();
        document["assets"].push_back(std::move(item));
    }

    std::error_code error;
    fs::create_directories(cachePath.parent_path(), error);

    // キャッシュディレクトリを用意できない場合は、一時出力へ進まず警告する。
    if (error) {
        AddIssue(
            AssetDatabaseIssueSeverity::kWarning,
            NormalizeProjectPath(cachePath),
            "永続索引フォルダを作成できません: " + error.message());
        return false;
    }

    {
        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);

        // 一時ファイルが開けなければ、既存キャッシュを維持して書込失敗を返す。
        if (!output.is_open()) {
            AddIssue(
                AssetDatabaseIssueSeverity::kWarning,
                NormalizeProjectPath(cachePath),
                "永続索引の一時ファイルを開けません。");
            return false;
        }

        output << document.dump() << '\n';
        output.flush();

        // バッファーを出した後も出力が正常な場合だけ、既存ファイルを置き換える。
        if (!output.good()) {
            AddIssue(
                AssetDatabaseIssueSeverity::kWarning,
                NormalizeProjectPath(cachePath),
                "永続索引の書き込みに失敗しました。");
            return false;
        }
    }

    // Cacheも一時ファイルを完成させてから差し替え、途中失敗時は次回の全解析に任せます。
    if (!MoveFileExW(
        temporaryPath.c_str(),
        cachePath.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD windowsError = GetLastError();
        fs::remove(temporaryPath, error);
        AddIssue(
            AssetDatabaseIssueSeverity::kWarning,
            NormalizeProjectPath(cachePath),
            "永続索引を確定できません。Windows Error: " + std::to_string(windowsError));
        return false;
    }

    return true;
}

// ソースと .meta のサイズ・更新時刻が一致した場合のみ、前回の解析結果を使います。
bool AssetDatabase::TryReusePersistentAsset(
    const fs::path& sourcePath,
    std::unordered_map<std::string, std::string>& guidOwners,
    EditorAssetRecord* reusedRecord) {
    // 出力を受け取る位置と検証済みのキャッシュが揃う場合に限り、再利用を判定する。
    if (!reusedRecord || !persistentIndexCacheAvailable_) {
        return false;
    }

    const std::string normalizedSource = NormalizeProjectPath(sourcePath);
    const std::string key = ToLowerAscii(normalizedSource);
    const auto cached = persistentAssetsByPath_.find(key);

    // この素材のパスが索引になければ、通常の読み込みに任せる。
    if (cached == persistentAssetsByPath_.end()) {
        return false;
    }

    const PersistentAssetEntry& entry = cached->second;
    std::uintmax_t sourceFileSize = 0;
    fs::file_time_type sourceWriteTime{};
    // 本体のサイズ・更新時刻・パス・形式・標準インポート方式の不一致は再利用しない。
    if (!ReadFileStamp(sourcePath, &sourceFileSize, &sourceWriteTime) ||
        entry.record.sourcePath != normalizedSource ||
        entry.record.fileSize != sourceFileSize ||
        entry.sourceWriteTimeTicks != FileTimeToTicks(sourceWriteTime) ||
        entry.record.type != DetectAssetType(sourcePath) ||
        entry.record.importer != GetDefaultImporter(entry.record.type)) {
        return false;
    }

    fs::path metaPath = sourcePath;
    metaPath += ".meta";
    std::error_code error;
    // .meta の有無が以前と異なる場合や、新規作成が必要になった場合は解析済みレコードを使わない。
    const bool metaExists = fs::exists(metaPath, error) && !error;
    // 存在確認の失敗、メタの有無の変化、メタ未作成で新規作成が必要な場合は、通常の調査へ戻す。
    if (error || entry.metaExists != metaExists ||
        (pendingCreateMissingMeta_ && !metaExists)) {
        return false;
    }

    // メタが存在する場合は、元ファイルと独立にその印と GUID を照合する。
    if (metaExists) {
        std::uintmax_t metaFileSize = 0;
        fs::file_time_type metaWriteTime{};
        // メタの情報を読み取れない場合、記録との差がある場合、または GUID が無効な場合は再利用できない。
        if (!ReadFileStamp(metaPath, &metaFileSize, &metaWriteTime) ||
            entry.metaFileSize != metaFileSize ||
            entry.metaWriteTimeTicks != FileTimeToTicks(metaWriteTime) ||
            !IsGuidValid(entry.record.guid)) {
            return false;
        }
    }
    // メタが無い素材で GUID が記録されている場合も矛盾として再利用しない。
    else if (!entry.record.guid.empty()) {
        return false;
    }

    // 別のレコードが先に同じ GUID を取得した場合は再利用を中止し、通常読み込み側の重複処理に任せる。
    const auto owner = guidOwners.find(entry.record.guid);

    // GUID の先着レコードが既にある場合は、重複解消を含む通常の調査へ戻す。
    if (!entry.record.guid.empty() && owner != guidOwners.end()) {
        return false;
    }

    *reusedRecord = entry.record;
    reusedRecord->fileSize = sourceFileSize;
    reusedRecord->lastWriteTime = sourceWriteTime;

    // 再利用した記録が有効な GUID を持つ場合、以降の重複検査の所有者として登録する。
    if (!reusedRecord->guid.empty()) {
        guidOwners[reusedRecord->guid] = reusedRecord->sourcePath;
    }

    PersistentAssetEntry refreshedEntry = entry;
    refreshedEntry.record = *reusedRecord;
    pendingPersistentAssetsByPath_[key] = std::move(refreshedEntry);
    return true;
}

void AssetDatabase::StorePendingPersistentAsset(const EditorAssetRecord& record) {
    // 今回の走査で確定したファイル印を次回起動用の索引候補へ積みます。
    const fs::path sourcePath = ResolveAbsolutePath(record.sourcePath);
    std::uintmax_t sourceFileSize = 0;
    fs::file_time_type sourceWriteTime{};

    // サイズと更新時刻を確定できない素材は、次回に使う候補へ載せない。
    if (!ReadFileStamp(sourcePath, &sourceFileSize, &sourceWriteTime)) {
        return;
    }

    PersistentAssetEntry entry;
    entry.record = record;
    entry.record.fileSize = sourceFileSize;
    entry.record.lastWriteTime = sourceWriteTime;
    entry.sourceWriteTimeTicks = FileTimeToTicks(sourceWriteTime);

    const fs::path metaPath = ResolveAbsolutePath(record.metaPath);
    std::error_code error;
    entry.metaExists = fs::exists(metaPath, error) && !error;

    // メタの有無を確認できなければ、次回は通常の調査で判断する。
    if (error) {
        return;
    }

    // 本体だけ一致しても.metaの編集は別にあり得るので、存在時は容量と更新時刻を対で残す。
    if (entry.metaExists) {
        fs::file_time_type metaWriteTime{};

        // メタが存在してもその印が不明なら、再利用候補として保存しない。
        if (!ReadFileStamp(metaPath, &entry.metaFileSize, &metaWriteTime)) {
            return;
        }

        entry.metaWriteTimeTicks = FileTimeToTicks(metaWriteTime);
    }

    pendingPersistentAssetsByPath_[ToLowerAscii(record.sourcePath)] = std::move(entry);
}

EditorAssetType AssetDatabase::DetectAssetType(const fs::path& path) {
    // 拡張子からImporterの初期値に使う型だけを推定します。
    const std::string extension = ToLowerAscii(PathToUtf8(path.extension()));

    // メッシュの代表形式はモデルとして扱い、既定のモデル用取込方式に渡す。
    if (extension == ".obj" || extension == ".gltf" || extension == ".glb" || extension == ".fbx") {
        return EditorAssetType::kModel;
    }

    // 画像の代表形式はテクスチャ用の設定を使う。
    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".dds" ||
        extension == ".tga" || extension == ".bmp" || extension == ".hdr") {
        return EditorAssetType::kTexture;
    }

    // JSON は設定と配置に使う独立した資産種別にする。
    if (extension == ".json") {
        return EditorAssetType::kJson;
    }

    // 波形・圧縮音声の代表形式は音声の分類へ入れる。
    if (extension == ".wav" || extension == ".mp3" || extension == ".ogg" || extension == ".flac") {
        return EditorAssetType::kAudio;
    }

    // ソースとインクルード、コンパイル済みの拡張子はシェーダー分類にする。
    if (extension == ".hlsl" || extension == ".hlsli" || extension == ".cso") {
        return EditorAssetType::kShader;
    }

    // TTF と OTF はフォントの分類にする。
    if (extension == ".ttf" || extension == ".otf") {
        return EditorAssetType::kFont;
    }

    // 既知の形式ではないが拡張子がある場合は、一般のバイナリとして登録する。
    if (!extension.empty()) {
        return EditorAssetType::kBinary;
    }

    return EditorAssetType::kUnknown;
}

std::string AssetDatabase::GetDefaultImporter(EditorAssetType type) {
    // .metaが未作成でも型ごとに同じImporter名へ収束させます。
    switch (type) {
    case EditorAssetType::kModel: return "ModelImporter";

    case EditorAssetType::kTexture: return "TextureImporter";

    case EditorAssetType::kJson: return "JsonImporter";

    case EditorAssetType::kAudio: return "AudioImporter";

    case EditorAssetType::kShader: return "ShaderImporter";

    case EditorAssetType::kFont: return "FontImporter";

    case EditorAssetType::kBinary: return "BinaryImporter";

    case EditorAssetType::kUnknown:
    default: return "DefaultImporter";
    }
}

json AssetDatabase::GetDefaultImportSettings(EditorAssetType type) {
    // 型別の既定設定は新規.metaにだけ入れ、既存の編集値は読込側で優先します。
    switch (type) {
    // モデルは等倍で取り込み、接線を生成する既定値を持つ。
    case EditorAssetType::kModel:
        return { { "scale", 1.0 }, { "generateTangents", true } };

    // 画像は色空間を自動判定し、ミップマップを用意する既定値を持つ。
    case EditorAssetType::kTexture:
        return { { "colorSpace", "Auto" }, { "generateMipmaps", true } };

    // 音声はストリーミングを既定で無効にする。
    case EditorAssetType::kAudio:
        return { { "streaming", false } };

    // 個別の既定項目を持たない資産形式は空の設定を返す。
    default:
        return json::object();
    }
}

std::string AssetDatabase::GenerateGuid() {
    // 複数の索引要求から呼ばれるため、乱数生成器をmutexで直列化します。
    static std::mutex sMutex;
    static std::mt19937_64 generator([] {
        std::random_device device;
        const auto ticks = static_cast<unsigned long long>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
        std::seed_seq seed{
            device(), device(),
            static_cast<std::uint32_t>(ticks),
            static_cast<std::uint32_t>(ticks >> 32),
        };
        return std::mt19937_64(seed);
    }());

    // 64 ビット乱数を 2 回生成し、それぞれゼロ埋めした 16 桁の 16 進表記を連結する。
    std::lock_guard<std::mutex> lock(sMutex);
    std::ostringstream stream;
    stream << std::hex << std::setfill('0')
           << std::setw(16) << generator()
           << std::setw(16) << generator();
    return ToLowerAscii(stream.str());
}
