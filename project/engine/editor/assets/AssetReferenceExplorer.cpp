#include <cstdint>
#include "AssetReferenceExplorer.h"

#ifdef USE_IMGUI

#include "DebugEditor.h"
#include "Object3D.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace {

std::string StripResourcesPrefix(const std::string& path) {
    const std::string normalized = AssetReferenceExplorer::Normalize(path);
    // 索引ではResourcesからの相対指定も受け付けるため、先頭の共通ディレクトリ名を取り除く。
    constexpr const char* kPrefix = "resources/";
    return normalized.rfind(kPrefix, 0) == 0 ? normalized.substr(std::strlen(kPrefix)) : normalized;
}

// 名前の類似度を比較できるよう、拡張子を除いたファイル名を小文字で返す。
std::string FileStemLower(const std::string& path) {
    std::string stem = fs::path(path).stem().generic_string();
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return stem;
}

std::int32_t CommonPrefixScore(const std::string& lhs, const std::string& rhs) {
    const std::size_t count = std::min(lhs.size(), rhs.size());
    // 途中で不一致になった時点で打ち切り、先頭から連続して一致した長さだけを類似度に使う。
    std::int32_t score = 0;

    for (std::size_t index = 0; index < count && lhs[index] == rhs[index]; ++index) {
        ++score;
    }

    return score;
}

} // namespace

// 参照一覧を表示するエディターを借用する。
void AssetReferenceExplorer::Initialize(DebugEditor* editor) {
    editor_ = editor;
}

void AssetReferenceExplorer::Finalize() {
    // Editor への参照と探索結果をまとめて破棄し、再オープン時に古い一覧を見せない。
    editor_ = nullptr;
    open_ = false;
    aliasToAssetIndices_.clear();
    usagesByGuid_.clear();
    missingReferences_.clear();
}

void AssetReferenceExplorer::Open(const std::string& assetPath) {
    // 対象を指定した場合は保持し、表示開始後の更新で参照一覧を再走査する。
    open_ = true;

    // 空でないパスを渡された場合だけ、開いた後に選択する対象を差し替える。
    if (!assetPath.empty()) {
        requestedAssetPath_ = assetPath;
    }

    rescanRequested_ = true;
}

std::string AssetReferenceExplorer::Normalize(std::string value) {
    // 区切り文字と大文字小文字を統一し、同一アセットの表記揺れを吸収する。
    std::replace(value.begin(), value.end(), '\\', '/');

    while (value.rfind("./", 0) == 0) {
        value.erase(0, 2);
    }

    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string AssetReferenceExplorer::EscapeJsonPointerToken(const std::string& token) {
    // JSON Pointerの特別文字は規約の順にエスケープして参照先を一意にする。
    std::string result;
    result.reserve(token.size());

    for (char character : token) {
        // チルダのままだと JSON Pointer のエスケープ記号になるため、~0 へ変換する。
        if (character == '~')
            result += "~0";
        // スラッシュは階層区切りにならないよう ~1 へ変換する。
        else if (character == '/') result += "~1";
        // そのまま使える文字は変更せずトークンへ加える。
        else result.push_back(character);
    }

    return result;
}

std::string AssetReferenceExplorer::GetOwnerCategory(const std::string& path) {
    // 保存場所から参照元の種類を推定し、一覧上の分類に使う。
    const std::string lower = Normalize(path);
    // 3D 配置やシーンのフォルダーにある JSON を Scene として表示する。
    if (lower.find("/3dobject/") != std::string::npos || lower.find("/scene/") != std::string::npos)
        return "Scene";
    // Prefab または Preset フォルダー内の場合は共通素材の分類を使う。
    if (lower.find("/prefab/") != std::string::npos || lower.find("/preset/") != std::string::npos)
        return "Prefab/Preset";
    // パスに演出を表す名前がある場合は、演出の設定として表示する。
    if (lower.find("effect") != std::string::npos || lower.find("particle") != std::string::npos || lower.find("vfx") != std::string::npos)
        return "Effect";
    return "JSON";
}

bool AssetReferenceExplorer::IsReferenceLikeKey(const std::string& key) {
    // JSON全項目ではなく、アセット参照を表しやすい名前だけ探索候補にする。
    const std::string lower = Normalize(key);
    static constexpr const char* kHints[] = {
        "asset", "model", "texture", "normalmap", "ormmap", "shader", "effect",
        "particle", "prefab", "preset", "audio", "sound", "animator", "font", "path"
    };

    return std::any_of(std::begin(kHints), std::end(kHints), [&lower](const char* hint) {
        return lower.find(hint) != std::string::npos;
    });
}

bool AssetReferenceExplorer::IsDirectReferenceValue(const std::string& value) {
    const std::string lower = Normalize(value);
    // 文字列の形から明示参照の候補を判定する。一意に解決できるかはVisitJsonで確認する。
    return lower.rfind("guid:", 0) == 0 || lower.rfind("resources/", 0) == 0 ||
        lower.find('/') != std::string::npos || fs::path(lower).has_extension();
}

void AssetReferenceExplorer::BuildAliasIndex() {
    // 同一資産を GUID、パス、Resources 相対パスで引けるようにします。
    aliasToAssetIndices_.clear();
    const auto& assets = AssetDatabase::GetInstance()->GetAssets();
    auto addAlias = [this](const std::string& alias, std::size_t index) {
        const std::string normalized = Normalize(alias);

        // 正規化した別名が空なら、どのアセットにも結び付けない。
        if (!normalized.empty()) {
            aliasToAssetIndices_[normalized].push_back(index);
        }
    };

    for (std::size_t index = 0; index < assets.size(); ++index) {
        const EditorAssetRecord& asset = assets[index];
        const fs::path source(asset.sourcePath);

        // JSON 側では GUID、完全 Path、ファイル名、拡張子なし名が混在するため同じ Asset を複数名で引きます。
        addAlias(asset.guid, index);
        addAlias("guid:" + asset.guid, index);
        addAlias(asset.sourcePath, index);
        addAlias(StripResourcesPrefix(asset.sourcePath), index);
        addAlias(source.filename().generic_string(), index);
        addAlias(source.stem().generic_string(), index);
        fs::path sourceWithoutExtension = source;
        sourceWithoutExtension.replace_extension();
        addAlias(sourceWithoutExtension.generic_string(), index);

        const std::string lowerPath = Normalize(asset.sourcePath);
        constexpr const char* kModelPrefix = "resources/3dmodel/";

        // 3D モデルのルート以下だけ、配置で使う親フォルダーの別名も登録する。
        if (lowerPath.rfind(kModelPrefix, 0) == 0) {
            // Model 参照にはフォルダ名だけを保存する形式もあるので、親フォルダも別名へ加えます。
            fs::path relative = fs::path(lowerPath.substr(std::strlen(kModelPrefix)));
            const fs::path parent = relative.parent_path();

            // ルート直下のファイルには相対パス上の親名がないので、親の別名を加えない。
            if (!parent.empty()) {
                addAlias(parent.generic_string(), index);

                // 親名とモデル名が一致する形式では、同じ親の別名がこの条件でも登録される。
                if (Normalize(parent.filename().generic_string()) == Normalize(source.stem().generic_string())) {
                    addAlias(parent.generic_string(), index);
                }
            }
        }
    }
}

std::vector<std::size_t> AssetReferenceExplorer::ResolveCandidates(const std::string& value, const std::string& key) const {
    (void)key;

    // Resources/ の有無だけを吸収して索引を検索します。複数一致は確定参照として扱いません。
    const std::string normalized = Normalize(value);
    auto iterator = aliasToAssetIndices_.find(normalized);

    // 入力表記のままで候補が見つかった場合は、その候補を使う。
    if (iterator != aliasToAssetIndices_.end()) {
        return iterator->second;
    }

    // 相対指定で最初の検索に失敗した場合だけ、Resources/ を補って探す。
    if (normalized.rfind("resources/", 0) != 0) {
        iterator = aliasToAssetIndices_.find("resources/" + normalized);

        // 補完した別名で候補が見つかれば、その一覧を返す。
        if (iterator != aliasToAssetIndices_.end()) {
            return iterator->second;
        }
    }

    return {};
}

void AssetReferenceExplorer::VisitJson(
    const nlohmann::json& value,
    const std::string& ownerPath,
    const std::string& pointer,
    const std::string& key,
    std::unordered_map<std::string, std::vector<Usage>>& destination,
    bool collectMissing) {
    // JSON Pointer を更新しながら葉の文字列へ降り、修復時に同じ値だけを安全に書き換えます。
    // オブジェクトなら、各キーを位置に加えて個別に調べる。
    if (value.is_object()) {
        for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
            VisitJson(iterator.value(), ownerPath, pointer + "/" + EscapeJsonPointerToken(iterator.key()), iterator.key(), destination, collectMissing);
        }

        return;
    }

    // 配列要素は添字を JSON Pointer へ足し、同じ文字列が複数あっても修復位置を区別する。
    if (value.is_array()) {
        for (std::size_t index = 0; index < value.size(); ++index) {
            VisitJson(value[index], ownerPath, pointer + "/" + std::to_string(index), key, destination, collectMissing);
        }

        return;
    }

    // 配列・オブジェクト以外で参照候補になるのは文字列だけ。
    if (!value.is_string())
        return;

    const std::string text = value.get<std::string>();
    // 空の文字列は参照先を表さないため、候補検索を行わない。
    if (text.empty())
        return;
    const std::vector<std::size_t> candidates = ResolveCandidates(text, key);
    const auto& assets = AssetDatabase::GetInstance()->GetAssets();

    // 別名で候補が得られたら、欠落候補にはせず参照の一覧へ進む。
    if (!candidates.empty()) {
        // Path/GUID と一意に一致した場合だけ Direct、曖昧な名称は動的参照候補に分類します。
        const bool direct = IsDirectReferenceValue(text) && candidates.size() == 1;

        for (std::size_t assetIndex : candidates) {
            // 範囲外の候補番号と参照元自身に一致した候補は登録しない。
            if (assetIndex >= assets.size() || assets[assetIndex].sourcePath == ownerPath)
                continue;
            Usage usage;
            usage.ownerPath = ownerPath;
            usage.jsonPointer = pointer;
            usage.value = text;
            usage.ownerCategory = GetOwnerCategory(ownerPath);
            usage.kind = direct ? UsageKind::kDirect : UsageKind::kDynamicCandidate;
            destination[assets[assetIndex].guid].push_back(std::move(usage));
        }

        return;
    }

    // 一致する Asset がなく、キー名も参照らしい文字列だけ Missing 候補に残します。
    // targetScene は Asset Path ではないため除外します。
    if (collectMissing && IsReferenceLikeKey(key) &&
        (IsDirectReferenceValue(text) || text.size() >= 3) &&
        Normalize(key).find("targetscene") == std::string::npos) {
        MissingReference missing;
        missing.ownerPath = ownerPath;
        missing.jsonPointer = pointer;
        missing.key = key;
        missing.value = text;
        const std::vector<std::size_t> repairCandidates = FindRepairCandidates(missing);

        for (std::size_t candidate : repairCandidates) {
            // 索引の現在の範囲にある候補だけ、修復時に指定できる GUID へ変換する。
            if (candidate < assets.size())
                missing.candidateGuids.push_back(assets[candidate].guid);
        }

        missingReferences_.push_back(std::move(missing));
    }
}

void AssetReferenceExplorer::ScanJsonAsset(const EditorAssetRecord& owner) {
    // 壊れたJSON一件で探索全体を止めず、そのアセットだけを読み飛ばす。
    std::ifstream stream(owner.sourcePath);
    // 参照元のファイルを開けなければ、その JSON の検索は行わない。
    if (!stream.is_open())
        return;
    nlohmann::json data;
    try {
        stream >> data;
    } catch (...) {
        return;
    }

    VisitJson(data, owner.sourcePath, "", "", usagesByGuid_, true);
}

std::vector<std::size_t> AssetReferenceExplorer::FindRepairCandidates(const MissingReference& missing) const {
    const auto& assets = AssetDatabase::GetInstance()->GetAssets();
    const std::string wanted = FileStemLower(missing.value);

    // 欠落参照の代替候補を並べる索引と類似度。高得点でも同一アセットと確定したわけではない。
    // 担当：参照修復候補のAsset位置と一致度。
    // 関連：AssetReferenceExplorerの内部処理だけで使う補助型。
    struct Scored { std::size_t index; std::int32_t score; };
    std::vector<Scored> scored;
    scored.reserve(assets.size());

    // ファイル名の前方一致とキー名に対応する Asset 種別を加点し、上位5件だけ提示します。
    for (std::size_t index = 0; index < assets.size(); ++index) {
        const std::string stem = FileStemLower(assets[index].sourcePath);
        std::int32_t score = CommonPrefixScore(wanted, stem) * 4;
        // ファイル名の片方がもう片方を含む場合は、前方一致に加えて得点を足す。
        if (stem.find(wanted) != std::string::npos || wanted.find(stem) != std::string::npos)
            score += 20;
        // モデルらしいキーには、実際のモデル素材を優先する。
        if (Normalize(missing.key).find("model") != std::string::npos && assets[index].type == EditorAssetType::kModel)
            score += 12;
        // テクスチャらしいキーには、画像素材を優先する。
        if (Normalize(missing.key).find("texture") != std::string::npos && assets[index].type == EditorAssetType::kTexture)
            score += 12;
        // 点数が5以下の素材は、修復候補に表示しない。
        if (score > 5)
            scored.push_back({ index, score });
    }

    // 点数順に並べて上位だけを表示する。類似スコアは人が選ぶ修復候補の順位に使う。
    std::sort(scored.begin(), scored.end(), [](const Scored& lhs, const Scored& rhs) {
        return lhs.score > rhs.score;
    });
    std::vector<std::size_t> result;

    for (std::size_t index = 0; index < std::min<std::size_t>(5, scored.size()); ++index) {
        result.push_back(scored[index].index);
    }

    return result;
}

std::string AssetReferenceExplorer::MakeReferenceValue(const EditorAssetRecord& asset, const std::string& key) const {
    // モデル参照のキーにはモデルルートからのフォルダ名を、それ以外は元パスを返す。
    const std::string lowerKey = Normalize(key);
    const std::string lowerPath = Normalize(asset.sourcePath);

    // モデル用キーとモデルルート配下が揃えば、配置で使う相対名への変換を試す。
    if (lowerKey.find("model") != std::string::npos && lowerPath.rfind("resources/3dmodel/", 0) == 0) {
        fs::path relative(lowerPath.substr(std::strlen("resources/3dmodel/")));
        // ルートからの相対パスに親名がある場合は、フォルダー名を参照値に使う。
        if (!relative.parent_path().empty())
            return relative.parent_path().generic_string();
    }

    return asset.sourcePath;
}

// 候補を選択した後だけ元 JSON を更新し、参照索引も作り直します。
bool AssetReferenceExplorer::RepairMissingReference(MissingReference& missing, std::string& message) {
    // 候補がなく選べない場合や、選択番号が範囲外の場合は編集を始めない。
    if (missing.candidateGuids.empty() || missing.selectedCandidate < 0 ||
        missing.selectedCandidate >= static_cast<std::int32_t>(missing.candidateGuids.size())) {
        message = "修復候補が選択されていません。";
        return false;
    }

    const EditorAssetRecord* asset = AssetDatabase::GetInstance()->FindByGuid(missing.candidateGuids[missing.selectedCandidate]);

    // 選んだ GUID が現在の素材索引で見つからなければ、ファイルを開かずエラーを返す。
    if (!asset) {
        message = "修復候補がAsset Databaseから見つかりません。";
        return false;
    }

    std::ifstream input(missing.ownerPath);
    nlohmann::json data;
    try {
        // 修復前に、走査時の参照位置と値を現在のファイルから再確認する。
        input >> data;
        nlohmann::json::json_pointer pointer(missing.jsonPointer);

        // 再スキャン後に元 JSON が変わった場合は、古い候補で別の値を上書きしないよう中断します。
        if (!data.contains(pointer) || !data[pointer].is_string() || data[pointer].get<std::string>() != missing.value) {
            message = "参照元が再スキャン後に変更されています。再スキャンしてください。";
            return false;
        }

        data[pointer] = MakeReferenceValue(*asset, missing.key);
    } catch (const std::exception& exception) {
        message = std::string("JSONを更新できません: ") + exception.what();
        return false;
    }

    // 実際の書き込みはユーザーが候補を選んだ後だけ行い、索引再作成を要求します。
    std::ofstream output(missing.ownerPath, std::ios::trunc);

    // 参照元を書き込み用に開けなければ、索引の再作成も要求しない。
    if (!output.is_open()) {
        message = "参照元JSONを書き込めません。";
        return false;
    }

    output << data.dump(4) << '\n';
    output.close();
    AssetDatabase::GetInstance()->RequestRefresh(true);
    message = "Missing参照を修復しました: " + missing.ownerPath;
    rescanRequested_ = true;
    return true;
}

void AssetReferenceExplorer::RefreshIndex() {
    // AssetDatabase の準備が済んでから、現在の世代に対応する参照索引を構築する。
    AssetDatabase* database = AssetDatabase::GetInstance();
    // 素材索引が未初期化か初回走査中なら、準備が整うまで既存の探索結果を保持する。
    if (!database->IsInitialized() || database->IsInitialIndexBuildInProgress())
        return;
    BuildAliasIndex();
    usagesByGuid_.clear();
    missingReferences_.clear();

    for (const EditorAssetRecord& asset : database->GetAssets()) {
        // 通常の JSON 素材だけを読み、ごみ箱内の旧ファイルは参照元から除く。
        if (asset.type == EditorAssetType::kJson && Normalize(asset.sourcePath).find("/.trash/") == std::string::npos) {
            ScanJsonAsset(asset);
        }
    }

    // 全JSONの走査を終えた索引だけを、現在世代の参照結果として扱う。
    indexedGeneration_ = database->GetGeneration();
    rescanRequested_ = false;
    statusMessage_ = "参照索引を更新しました。Asset " + std::to_string(database->GetAssets().size()) +
        " 件 / Missing候補 " + std::to_string(missingReferences_.size()) + " 件";
}

void AssetReferenceExplorer::SelectAssetByPath(const std::string& path) {
    const EditorAssetRecord* asset = AssetDatabase::GetInstance()->FindByPath(path);
    // パスから登録済み素材を確定できた場合だけ、選択中の GUID を変更する。
    if (asset)
        selectedGuid_ = asset->guid;
}

#endif // USE_IMGUI
