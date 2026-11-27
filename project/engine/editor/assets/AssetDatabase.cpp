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

AssetDatabase* AssetDatabase::GetInstance() {
    // インデックスの所有者を一つにし、各Editorから同じGUID対応を参照します。
    static AssetDatabase sInstance;
    return &sInstance;
}

bool AssetDatabase::Initialize(const std::string& resourcesRoot, bool createMissingMeta) {
    StopFilesystemWatcher();
    std::error_code error;
    projectRoot_ = fs::current_path(error);

    // 作業ディレクトリを取得できなければ、索引の基準パスを確定できない。
    if (error) {
        return false;
    }

    resourcesRoot_ = fs::absolute(PathFromUtf8(resourcesRoot), error).lexically_normal();

    // 素材ルートを絶対パスにできない場合や、存在するディレクトリでない場合は失敗を返す。
    if (error || !fs::exists(resourcesRoot_, error) || !fs::is_directory(resourcesRoot_, error)) {
        return false;
    }

    resourcesRootPath_ = NormalizeProjectPath(resourcesRoot_);
    createMissingMeta_ = createMissingMeta;
    persistentAssetsByPath_.clear();
    pendingPersistentAssetsByPath_.clear();
    persistentIndexCacheInitialized_ = false;
    persistentIndexCacheAvailable_ = false;
    initialized_ = true;

    // 起動時は永続索引を利用した差分確認だけを行います。
    // DDS全走査はProject Windowの明示操作や更新要求時に限定します。
    BeginInitialIndexBuild(createMissingMeta_, false);
    return true;
}

AssetDatabaseRefreshResult AssetDatabase::Refresh(bool createMissingMeta) {
    // 明示的な同期更新では段階走査を打ち切り、既存の .meta を一括で読み直します。
    AssetDatabaseRefreshResult result;
    initialIndexBuildInProgress_ = false;
    initialDirectoryScanInProgress_ = false;
    pendingDirectoryIterator_ = fs::recursive_directory_iterator();
    pendingSourcePaths_.clear();
    pendingSourcePathIndex_ = 0;
    pendingGuidOwners_.clear();
    pendingPersistentAssetsByPath_.clear();
    assets_.clear();
    issues_.clear();
    assetIndexByGuid_.clear();
    assetIndexByPath_.clear();
    assetsByDirectory_.clear();
    subdirectoriesByDirectory_.clear();

    // 初期化を済ませていなければ、既知のルートを走査せずエラー結果を記録する。
    if (!initialized_) {
        AddIssue(AssetDatabaseIssueSeverity::kError, resourcesRootPath_, "Asset Databaseが初期化されていません。");
        result.errorCount = 1;
        lastRefreshResult_ = result;
        return result;
    }

    std::error_code error;

    // 初期化後でも素材ディレクトリがなくなっていれば、素材の読み込みに進まない。
    if (!fs::exists(resourcesRoot_, error) || !fs::is_directory(resourcesRoot_, error)) {
        AddIssue(AssetDatabaseIssueSeverity::kError, resourcesRootPath_, "Resourcesフォルダが見つかりません。");
        result.errorCount = 1;
        lastRefreshResult_ = result;
        return result;
    }

    // 同期更新では全メタデータを読み直し、その結果から次回起動用の永続索引を作る。
    const std::vector<fs::path> sourcePaths = CollectSourcePaths();

    std::unordered_map<std::string, std::string> guidOwners;
    assets_.reserve(sourcePaths.size());

    for (const fs::path& sourcePath : sourcePaths) {
        MetaLoadResult meta = LoadOrCreateMeta(sourcePath, createMissingMeta, guidOwners);
        ++result.reparsedAssetCount;

        // メタデータを読み込み・生成できず、有効なレコードが得られなかった素材は飛ばす。
        if (!meta.success) {
            continue;
        }

        // メタファイルを新規に作成した結果は、作成件数へ反映する。
        if (meta.created) {
            ++result.createdMetaCount;
        }

        // 既存のメタファイルを更新した結果は、更新件数へ反映する。
        if (meta.updated) {
            ++result.updatedMetaCount;
        }

        StorePendingPersistentAsset(meta.record);
        assets_.push_back(std::move(meta.record));
    }

    std::sort(assets_.begin(), assets_.end(), [](const EditorAssetRecord& left, const EditorAssetRecord& right) {
        return left.sourcePath < right.sourcePath;
    });
    RebuildLookupTables();

    result.assetCount = assets_.size();
    result.errorCount = static_cast<std::size_t>(std::count_if(
        issues_.begin(),
        issues_.end(),
        [](const AssetDatabaseIssue& issue) {
            return issue.severity == AssetDatabaseIssueSeverity::kError;
        }));
    ++generation_;
    lastRefreshResult_ = result;
    persistentAssetsByPath_ = std::move(pendingPersistentAssetsByPath_);
    persistentIndexCacheAvailable_ = true;
    SavePersistentIndexCache();

    // 同期更新前から監視器を使っている場合は、古い変更通知を持ち越さないよう監視を開始し直す。
    if (filesystemWatcher_) {
        RestartFilesystemWatcher();
    }

    RequestDdsCacheBuild();
    return result;
}

void AssetDatabase::RequestRefresh(bool createMissingMeta) {
    // 初期化済みなら、初期索引と同じフレーム分割の経路に再走査を渡す。
    if (!initialized_) {
        return;
    }

    BeginInitialIndexBuild(createMissingMeta, true);
}

void AssetDatabase::RequestDdsCacheBuild() {
    // 索引後に行う DDS 変換を予約する。初期化前と変換の実行中は新たに予約しない。
    if (!initialized_ || ddsCacheBuildState_ == DdsCacheBuildState::kRunning) {
        return;
    }

    ddsCacheBuildState_ = DdsCacheBuildState::kQueued;
    ddsCacheBuildMessage_ = "Asset Database更新後のDDS変換を待機しています。";
}

bool AssetDatabase::Update() {
    // 重い初回走査はフレームごとに分割し、変更通知と DDS 作成結果もここで統合します。
    // 索引が未初期化の間は、変換の状態やファイル通知を更新しない。
    if (!initialized_) {
        return false;
    }

    const bool ddsStateChanged = PollDdsCacheBuild();

    // 現在の列挙・索引を先に進め、別の更新要求で進行中の状態を上書きしない。
    if (initialIndexBuildInProgress_) {
        return ProcessInitialIndexBuild() || ddsStateChanged;
    }

    // 変換で生成されたファイルを先に取り込む。この索引更新の完了から同じ DDS 変換は予約しない。
    if (refreshAfterDdsCacheBuild_) {
        refreshAfterDdsCacheBuild_ = false;
        BeginInitialIndexBuild(createMissingMeta_, false);
        return true;
    }

    // 下処理が済み DDS 変換が予約されている場合は、監視通知を処理するより先に起動する。
    if (ddsCacheBuildState_ == DdsCacheBuildState::kQueued) {
        StartDdsCacheBuild();
        return true;
    }

    // DDS生成中に作られるファイル通知は完了後の一括更新で取り込むため、ここでは索引を開始しません。
    if (ddsCacheBuildState_ == DdsCacheBuildState::kRunning) {
        return ddsStateChanged;
    }

    const auto now = std::chrono::steady_clock::now();
    // 最初の変更通知から所定時間待ち、その間の通知を一度の再走査にまとめる。
    if (PollFilesystemWatcher() && !filesystemChangePending_) {
        filesystemChangePending_ = true;
        filesystemChangeReadyAt_ = now + kFilesystemChangeDebounce;
    }

    // 通知がまだ来ていないか待機時間の途中なら、再走査せず DDS の更新結果だけを返す。
    if (!filesystemChangePending_ || now < filesystemChangeReadyAt_) {
        return ddsStateChanged;
    }

    BeginInitialIndexBuild(createMissingMeta_, false);
    pendingRefreshResult_.filesystemChanged = true;
    return true;
}

const EditorAssetRecord* AssetDatabase::FindByGuid(const std::string& guid) const {
    // GUIDの大文字小文字を統一して検索し、登録のない参照はnullptrで返します。
    const auto iterator = assetIndexByGuid_.find(ToLowerAscii(guid));
    return iterator == assetIndexByGuid_.end() ? nullptr : &assets_[iterator->second];
}

const EditorAssetRecord* AssetDatabase::FindByPath(const std::string& sourcePath) const {
    // 絶対・相対パスを同じプロジェクト表記に揃えてから索引を引きます。
    const std::string normalized = NormalizeProjectPath(PathFromUtf8(sourcePath));
    const auto iterator = assetIndexByPath_.find(ToLowerAscii(normalized));
    return iterator == assetIndexByPath_.end() ? nullptr : &assets_[iterator->second];
}

std::string AssetDatabase::ResolveAssetPath(const std::string& pathOrGuidReference) const {
    // GUID参照は索引へ解決し、通常のパスは未登録でも元の値を保ちます。
    std::string guid = pathOrGuidReference;

    // 現在の asset: 形式は、識別子の検証前に接頭辞を外す。
    if (guid.rfind("asset:", 0) == 0) {
        guid.erase(0, 6);
    }
    // guid: と書かれた旧表記も、同じ識別子として解決する。
    else if (guid.rfind("guid:", 0) == 0) {
        guid.erase(0, 5);
    }

    // 形式が有効なGUIDはパスへ代用せず、見つからないときは未解決として空を返す。
    if (IsGuidValid(guid)) {
        const EditorAssetRecord* asset = FindByGuid(guid);
        return asset ? asset->sourcePath : std::string();
    }

    const EditorAssetRecord* asset = FindByPath(pathOrGuidReference);
    return asset ? asset->sourcePath : pathOrGuidReference;
}

std::string AssetDatabase::ResolveAssetGuid(const std::string& sourcePath) const {
    // 未登録Assetに仮のGUIDを作らず、空文字で呼び出し側へ通知します。
    const EditorAssetRecord* asset = FindByPath(sourcePath);
    return asset ? asset->guid : std::string();
}

std::string AssetDatabase::MakeGuidReference(const std::string& sourcePath) const {
    // GUIDを持つAssetだけ安定参照へ変換し、未登録パスはそのまま保存します。
    const std::string guid = ResolveAssetGuid(sourcePath);
    return guid.empty() ? sourcePath : "asset:" + guid;
}

std::vector<const EditorAssetRecord*> AssetDatabase::GetAssetsInDirectory(
    const std::string& directory,
    bool recursive) const {
    // 直下はディレクトリ索引を使い、再帰指定時だけ全レコードを走査します。
    std::vector<const EditorAssetRecord*> result;
    const std::string normalizedDirectory = TrimTrailingSlash(NormalizeProjectPath(PathFromUtf8(directory)));

    // 再帰指定がなければ、全素材を走査せず指定ディレクトリ直下の索引を引く。
    if (!recursive) {
        const auto iterator = assetsByDirectory_.find(ToLowerAscii(normalizedDirectory));

        // 直下の索引に指定ディレクトリがなければ、素材一覧を空で返す。
        if (iterator == assetsByDirectory_.end()) {
            return result;
        }

        result.reserve(iterator->second.size());

        for (std::size_t index : iterator->second) {
            result.push_back(&assets_[index]);
        }

        return result;
    }

    for (const EditorAssetRecord& asset : assets_) {
        const std::string parent = TrimTrailingSlash(PathToUtf8(PathFromUtf8(asset.sourcePath).parent_path()));

        // 素材の親が指定ディレクトリ自身か配下にある場合、再帰結果へ借用参照を加える。
        if (StartsWithPath(ToLowerAscii(parent), ToLowerAscii(normalizedDirectory))) {
            result.push_back(&asset);
        }
    }

    return result;
}

std::vector<std::string> AssetDatabase::GetSubdirectories(const std::string& directory) const {
    // 区切りと大文字小文字を索引側に合わせて、同じフォルダを一意に引きます。
    const std::string normalizedDirectory = ToLowerAscii(
        TrimTrailingSlash(NormalizeProjectPath(PathFromUtf8(directory))));
    const auto iterator = subdirectoriesByDirectory_.find(normalizedDirectory);
    return iterator == subdirectoriesByDirectory_.end() ? std::vector<std::string>() : iterator->second;
}

bool AssetDatabase::RenameAsset(
    const std::string& sourcePath,
    const std::string& newFileName,
    std::string* errorMessage) {
    // 名前変更を移動処理に集約し、Asset本体と.metaを同じ手順で更新します。
    const fs::path requestedName = PathFromUtf8(newFileName);

    // 改名には単一のファイル名だけを受け付ける。空欄や親パスを含む指定では移動しない。
    if (newFileName.empty() || requestedName.has_parent_path() || requestedName.filename() != requestedName) {
        // エラー文の受け取り先が指定されている場合だけ、不正な指定の理由を設定する。
        if (errorMessage) {
            *errorMessage = "新しい名前にはファイル名だけを指定してください。";
        }

        return false;
    }

    const EditorAssetRecord* asset = FindByPath(sourcePath);

    // 元の素材が索引にない場合は、変更前のパスを確定できない。
    if (!asset) {
        // 受け取り先があれば、未登録の素材であることを通知する。
        if (errorMessage) {
            *errorMessage = "Asset Databaseに登録されていないAssetです。";
        }

        return false;
    }

    const fs::path destination = PathFromUtf8(asset->sourcePath).parent_path() / requestedName;
    return MoveAsset(asset->sourcePath, PathToUtf8(destination), errorMessage);
}

bool AssetDatabase::MoveAsset(
    const std::string& sourcePath,
    const std::string& destinationPath,
    std::string* errorMessage) {
    // Resources 外への移動を拒否する。.meta の移動に失敗した場合は、本体を元の位置へ戻す処理を試みる。
    const EditorAssetRecord* asset = FindByPath(sourcePath);

    // 移動元が索引にない場合、ディスクの操作を始めない。
    if (!asset) {
        // 結果文の指定があれば、元の素材が未登録であることを返す。
        if (errorMessage) {
            *errorMessage = "Asset Databaseに登録されていないAssetです。";
        }

        return false;
    }

    // 移動と再索引で参照が変わる前に、本体と.metaの元パスを値として控える。
    const std::string sourceProjectPath = asset->sourcePath;
    const std::string sourceMetaProjectPath = asset->metaPath;
    const fs::path sourceAbsolute = ResolveAbsolutePath(sourceProjectPath);
    const fs::path sourceMetaAbsolute = ResolveAbsolutePath(sourceMetaProjectPath);
    const fs::path destinationAbsolute = ResolveAbsolutePath(destinationPath).lexically_normal();

    // 移動先は Resources 配下の通常素材に限り、.meta の名前を直接指定させない。
    if (!IsPathInsideResources(destinationAbsolute) || destinationAbsolute.extension() == ".meta") {
        // エラーを受け取る指定がある場合は、許可する移動先を案内する。
        if (errorMessage) {
            *errorMessage = "移動先はResources配下の通常Assetを指定してください。";
        }

        return false;
    }

    std::error_code error;

    // 移動先がすでに存在する場合は、上書きしない。
    if (fs::exists(destinationAbsolute, error)) {
        // 結果文の指定があれば、移動先の名前が使用中であることを返す。
        if (errorMessage) {
            *errorMessage = "移動先に同名Assetが存在します。";
        }

        return false;
    }

    fs::create_directories(destinationAbsolute.parent_path(), error);

    // 移動先の親フォルダーを作成できなければ、本体を移動しない。
    if (error) {
        // 受け取り先があれば、フォルダー作成に失敗した理由を返す。
        if (errorMessage) {
            *errorMessage = "移動先フォルダを作成できません: " + error.message();
        }

        return false;
    }

    fs::rename(sourceAbsolute, destinationAbsolute, error);

    // 本体の移動に失敗した場合は、識別子のファイルも動かさない。
    if (error) {
        // 受け取り先があれば、本体の移動に失敗した理由を返す。
        if (errorMessage) {
            *errorMessage = "Assetを移動できません: " + error.message();
        }

        return false;
    }

    fs::path destinationMetaAbsolute = destinationAbsolute;
    destinationMetaAbsolute += ".meta";

    // 元の .meta が存在する場合に限り、名前を合わせて本体と同じ場所へ移動する。
    if (fs::exists(sourceMetaAbsolute, error)) {
        error.clear();
        fs::rename(sourceMetaAbsolute, destinationMetaAbsolute, error);

        // 本体だけ移動した後に .meta の移動が失敗した場合は、本体を戻す処理を試みる。
        if (error) {
            // 移動に失敗した経路では再索引を予約しない。
            std::error_code rollbackError;
            fs::rename(destinationAbsolute, sourceAbsolute, rollbackError);

            // 受け取り先があれば、.meta の移動に失敗して復元を試みた理由を返す。
            if (errorMessage) {
                *errorMessage = "Assetは移動しましたが.metaを移動できないためロールバックしました: " + error.message();
            }

            return false;
        }
    }

    RequestRefresh(createMissingMeta_);
    return true;
}

bool AssetDatabase::MoveAssetToTrash(
    const std::string& sourcePath,
    std::string* recoveredPath,
    std::string* errorMessage) {
    // 元の相対配置を退避先へ残し、同名衝突時はGUIDで回復パスを一意にします。
    const EditorAssetRecord* asset = FindByPath(sourcePath);

    // 移動元が索引にない場合は、元の相対配置や復元名を決められない。
    if (!asset) {
        // 受け取り先があれば、元の素材が未登録であることを返す。
        if (errorMessage) {
            *errorMessage = "Asset Databaseに登録されていないAssetです。";
        }

        return false;
    }

    fs::path relativeToResources;
    std::error_code error;
    relativeToResources = fs::relative(ResolveAbsolutePath(asset->sourcePath), resourcesRoot_, error);

    // 素材ルートからの相対位置を取得できなければ、退避先の階層を作らず失敗を返す。
    if (error || relativeToResources.empty()) {
        // エラー文が必要な呼出元へ、相対化に失敗したことを伝える。
        if (errorMessage) {
            *errorMessage = "Resourcesからの相対パスを作成できません。";
        }

        return false;
    }

    fs::path destination = resourcesRoot_ / ".trash" / "asset_database" / MakeTimestamp() / relativeToResources;

    // 時間別の退避先に同名の項目があれば、素材 GUID の先頭8文字を名前に添える。
    if (fs::exists(destination, error)) {
        destination = destination.parent_path() /
            PathFromUtf8(PathToUtf8(destination.stem()) + "_" + asset->guid.substr(0, 8) + PathToUtf8(destination.extension()));
    }

    const std::string destinationProjectPath = NormalizeProjectPath(destination);

    // 通常の移動処理が失敗した場合は、復元先の記録を更新しない。
    if (!MoveAsset(asset->sourcePath, destinationProjectPath, errorMessage)) {
        return false;
    }

    // 呼出元が受け取り先を指定した場合だけ、実際の退避パスを返す。
    if (recoveredPath) {
        *recoveredPath = destinationProjectPath;
    }

    return true;
}

bool AssetDatabase::IsGuidValid(const std::string& guid) {
    // .metaの識別子はハイフンなし32桁の16進表記だけを受け付けます。
    if (guid.size() != 32) {
        return false;
    }

    return std::all_of(guid.begin(), guid.end(), [](unsigned char value) {
        return std::isxdigit(value) != 0;
    });
}

const char* AssetDatabase::GetAssetTypeName(EditorAssetType type) {
    // .metaへ保存する型名を列挙値から一箇所で決定します。
    switch (type) {
    case EditorAssetType::kModel: return "Model";

    case EditorAssetType::kTexture: return "Texture";

    case EditorAssetType::kJson: return "JSON";

    case EditorAssetType::kAudio: return "Audio";

    case EditorAssetType::kShader: return "Shader";

    case EditorAssetType::kFont: return "Font";

    case EditorAssetType::kBinary: return "Binary";

    case EditorAssetType::kUnknown:
    default: return "Unknown";
    }
}

std::string AssetDatabase::NormalizeProjectPath(const fs::path& path) const {
    // プロジェクト内だけ相対表記にし、外部パスを誤って内側へ見せません。
    std::error_code error;
    fs::path absolute = path.is_absolute() ? path : projectRoot_ / path;
    absolute = fs::absolute(absolute, error).lexically_normal();

    // 絶対化に失敗したときは、元の表記を字句上で整理して返す。
    if (error) {
        return PathToUtf8(path.lexically_normal());
    }

    const fs::path relative = fs::relative(absolute, projectRoot_, error);

    // 有効な相対パスを求められた場合だけ、プロジェクト内を指すか確認する。
    if (!error && !relative.empty() && !relative.is_absolute()) {
        const std::string normalized = PathToUtf8(relative.lexically_normal());

        // 親の階層へ出ない場合に限り、プロジェクト相対の表記を採用する。
        if (normalized != ".." && normalized.rfind("../", 0) != 0) {
            return normalized;
        }
    }

    return PathToUtf8(absolute);
}

fs::path AssetDatabase::ResolveAbsolutePath(const std::string& projectPath) const {
    // 読込・移動に使う実パスはプロジェクトルートから確定します。
    const fs::path path = PathFromUtf8(projectPath);
    return (path.is_absolute() ? path : projectRoot_ / path).lexically_normal();
}

bool AssetDatabase::IsPathInsideResources(const fs::path& absolutePath) const {
    // 相対化後の親ディレクトリ参照を拒否し、操作対象をResources内へ限定します。
    std::error_code error;
    const fs::path relative = fs::relative(absolutePath.lexically_normal(), resourcesRoot_, error);

    // 相対化に失敗したか、使用できる相対パスでなければ対象外。
    if (error || relative.empty() || relative.is_absolute()) {
        return false;
    }

    const std::string normalized = PathToUtf8(relative);
    return normalized != ".." && normalized.rfind("../", 0) != 0;
}

bool AssetDatabase::ShouldSkipPath(const fs::path& path) const {
    // キャッシュ・退避領域を再登録すると循環するため、索引対象から除外します。
    std::error_code error;
    const fs::path relative = fs::relative(path, resourcesRoot_, error);

    // 素材ルートからの位置を判定できないパスは、索引の走査から除く。
    if (error) {
        return true;
    }

    for (const fs::path& component : relative) {
        const std::string name = ToLowerAscii(PathToUtf8(component));

        // いずれかの階層にキャッシュ・バックアップ・退避・ツールの名前があれば、索引へ登録しない。
        if (name == ".cache" || name == ".backup" || name == ".trash" || name == "tools") {
            return true;
        }
    }

    return false;
}
