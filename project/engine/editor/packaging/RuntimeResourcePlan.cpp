#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "RuntimeResourcePlan.h"
#include "engine/graphics/3d/model/ModelAssetPath.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <deque>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace engine {
namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;

// 相対パスをレポートと索引で扱う UTF-8 の汎用区切り表記へ変換する。
std::string Text(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {value.begin(), value.end()};
}

fs::path Path(const std::string& value) {
    return fs::path(std::u8string(value.begin(), value.end()));
}

std::string Lower(std::string value) {
    // UTF-8の日本語バイトを壊さず、ASCIIのパス区切りと英字だけを正規化します。
    std::replace(value.begin(), value.end(), '\\', '/');
    // ASCII の大文字だけを小文字へ写し、非 ASCII のバイト列は保持する。
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return value;
}

// Windows のコピー照合用に、区切りと大文字・小文字の表記差を吸収したキーを返す。
std::wstring CopyKey(const fs::path& path) {
    auto key = path.generic_wstring();
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return std::towlower(c); });
    return key;
}

// 同名かスラッシュで区切った子パスかを確かめ、接頭辞が似ただけの兄弟パスを除く。
bool Under(const std::string& path, const std::string& prefix) {
    return path == prefix || path.starts_with(prefix + "/");
}

std::string Read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    // 開けない解析入力は結果に混ぜず、パス付きの例外として返す。
    if (!stream) throw std::runtime_error("読み込めません: " + Text(path));
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

bool IsModel(const fs::path& path) {
    const auto ext = Lower(Text(path.extension()));
    return ext == ".obj" || ext == ".gltf" || ext == ".glb" || ext == ".fbx";
}

bool IsTexture(const fs::path& path) {
    const auto ext = Lower(Text(path.extension()));
    return ext == ".png" || ext == ".dds" || ext == ".jpg" || ext == ".jpeg" ||
           ext == ".tga" || ext == ".bmp" || ext == ".hdr";
}

std::string DevelopmentOnly(const fs::path& path) {
    const auto key = Lower(Text(path));
    // ユーザーの保存スロットと端末固有の調整値は、新しい配布先へ持ち出しません。
    for (const auto& component : path) {
        const auto name = Lower(Text(component));
        // パスのどこかに制作専用のディレクトリ名があれば、除外理由を決める。
        if (name == ".cache" || name == ".backup" || name == ".trash" || name == "__pycache__" || name == "tools")
            return "キャッシュ・退避・制作ツール";
    }
    // エディターの生成物に該当するパスは、通常の素材と区別する。
    if (Under(key, "generated/editor")) return "エディターの生成物";
    const auto name = Lower(Text(path.filename()));
    // セーブ名や端末に固有の設定名は、個人環境のファイルとして除外する。
    if (name.starts_with("savedata") || name == "imgui.ini" || name == "thumbs.db" || name == ".ds_store")
        return "個人のセーブ・端末設定";
    const auto ext = Lower(Text(path.extension()));
    // 制作元の形式、スクリプト、文書、一時ファイルはランタイムの素材にしない。
    if (ext == ".log" || ext == ".tmp" || ext == ".bak" || ext == ".blend" || ext == ".psd" ||
        ext == ".kra" || ext == ".py" || ext == ".ps1" || ext == ".bat" || ext == ".md")
        return "制作元データ・文書・一時ファイル";
    return {};
}

// C++のコメントや文字リテラルに書かれた例を除外し、実際の文字列リテラルだけを取り出します。
// 動的な文字列の断片も返し、フォルダー指定は呼び出し側が保守的に保持します。
std::vector<std::string> StringLiterals(const std::string& source) {
    std::vector<std::string> result;
    for (size_t i = 0; i < source.size();) {
        // 行コメントに入ったら、文中の引用符を素材名と誤認しないよう改行まで読み飛ばす。
        if (source.compare(i, 2, "//") == 0) {
            const auto end = source.find('\n', i + 2);
            i = end == std::string::npos ? source.size() : end + 1;
        // ブロックコメントの内部も終端まで読み飛ばす。閉じていなければ入力末尾で止める。
        } else if (source.compare(i, 2, "/*") == 0) {
            const auto end = source.find("*/", i + 2);
            i = end == std::string::npos ? source.size() : end + 2;
        // 生文字列は区切り文字を抽出し、エスケープを解釈せず本文を取得する。
        } else if (source.compare(i, 2, "R\"") == 0) {
            const auto open = source.find('(', i + 2);
            // 開始括弧がないか区切りが長すぎる候補は、生文字列として扱わず次の文字へ進む。
            if (open == std::string::npos || open - i > 18) { ++i; continue; }
            const auto end = source.find(")" + source.substr(i + 2, open - i - 2) + "\"", open + 1);
            // 同じ区切りによる終端が見つからなければ、それ以降のリテラルは判定しない。
            if (end == std::string::npos) break;
            result.push_back(source.substr(open + 1, end - open - 1));
            const auto delimiterLength = open - i - 2;
            i = end + delimiterLength + 2;
        // 通常文字列と文字リテラルは終端まで読む。素材名として返すのは二重引用符の文字列だけ。
        } else if (source[i] == '\'' || source[i] == '"') {
            const char quote = source[i++];
            std::string value;
            while (i < source.size() && source[i] != quote) {
                // 次の文字を取り出せるエスケープだけ、通常の終端判定から外す。
                if (source[i] == '\\' && i + 1 < source.size()) {
                    ++i;
                    // 引用符とバックスラッシュは、文字列中の実際の一文字へ戻す。
                    if (source[i] == '\\' || source[i] == '"' || source[i] == '\'') value += source[i];
                    // n は改行に戻す。改行を含む候補は参照照合側で省く。
                    else if (source[i] == 'n') value += '\n';
                    // t は水平タブに戻す。
                    else if (source[i] == 't') value += '\t';
                    // 扱っていないエスケープは、後続1文字だけを候補に残す。
                    else value += source[i];
                    ++i;
                // エスケープに入っていなければ、通常の一文字として追加する。
                } else value += source[i++];
            }
            // 閉じる引用符まで読めた場合、その一文字を消費して次へ進む。
            if (i < source.size()) ++i;
            // 文字リテラルの本文を参照候補に含めない。
            if (quote == '"') result.push_back(std::move(value));
        // どのリテラルやコメントにも当てはまらない文字は読み飛ばす。
        } else ++i;
    }
    return result;
}

/// <summary>配布素材一件の相対パスと容量を保持し、依存をたどって採否と参照元を記録する。</summary>
struct Asset {
    fs::path relative;
    uint64_t bytes = 0;
    std::string reason;
    std::string source;
    bool used = false;
};

// 一回の配布作成だけで使う索引です。共有のAssetDatabaseやGPUロード状態には触れません。
class Collector {
public:
    explicit Collector(fs::path project) : project_(std::move(project)), resources_(project_ / "Resources") {}
    RuntimeResourcePlan Build(const std::string& keep);

private:
    void Index();
    std::string ReadInput(const fs::path& path);
    void Keep(const std::string& key, const std::string& source, const std::string& reason);
    bool Folder(const std::string& key, const std::string& source, const std::string& reason);
    bool Reference(std::string value, const fs::path& relativeTo, const std::string& source, bool explicitKeep = false);
    void VisitJson(const json& value, const fs::path& relativeTo, const std::string& source);
    void Dependencies(const std::string& key);
    void Code();
    void Problem(const std::string& source, const std::string& value, const std::string& reason);

    fs::path project_, resources_;
    std::map<std::string, Asset> assets_;
    std::map<std::string, std::vector<std::string>> aliases_;
    std::map<std::string, std::vector<std::string>> guids_;
    std::set<std::string> directories_, modelFolders_;
    std::deque<std::string> pending_;
    std::set<std::string> problemKeys_;
    json problems_ = json::array();
    std::map<fs::path, RuntimeResourcePlan::Input> inputs_;
};

std::string Collector::ReadInput(const fs::path& path) {
    // 読み取り前後の更新を検査し、保存途中の内容から依存を断定しません。
    RuntimeResourcePlan::Input input{path, fs::file_size(path), fs::last_write_time(path)};
    const auto text = Read(path);
    // 読込前後でサイズか更新時刻が変わった入力は、途中で保存された可能性があるため失敗にする。
    if (fs::file_size(path) != input.bytes || fs::last_write_time(path) != input.modified)
        throw std::runtime_error("解析中に更新されました。もう一度作成してください: " + Text(path));
    // 同じパスを前にも解析した場合は、前回と同じ入力が維持されたかも確認する。
    if (const auto old = inputs_.find(path); old != inputs_.end() &&
        (old->second.bytes != input.bytes || old->second.modified != input.modified))
        throw std::runtime_error("解析中に更新されました。もう一度作成してください: " + Text(path));
    inputs_[path] = input;
    return text;
}

// 同じ参照元・入力値・理由は、未解決一覧へ一度だけ載せる。
void Collector::Problem(const std::string& source, const std::string& value, const std::string& reason) {
    // 参照元・入力値・理由の組が今回初めてなら、未解決の一覧へ追加する。
    if (problemKeys_.insert(source + "\n" + value + "\n" + reason).second)
        problems_.push_back({{"source", source}, {"value", value}, {"reason", reason}});
}

// Resources 配下のファイル・別名・GUID を、使用判定を行う前に索引化する。
void Collector::Index() {
    // 素材を列挙するディレクトリが使えなければ、採否の一覧を作らない。
    if (!fs::is_directory(resources_)) throw std::runtime_error("Resourcesが見つかりません。");
    // 名前の候補を全て集めても、依存のあるファイルだけを後で使用中にします。
    for (fs::recursive_directory_iterator it(resources_), end; it != end; ++it) {
        const auto attributes = GetFileAttributesW(it->path().c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            // 素材内部のリンク先へ入らず、コピー側と同じResources境界を守ります。
            // リンク扱いのディレクトリに出会ったら、その子を再帰走査しない。
            if (it->is_directory()) it.disable_recursion_pending();
            continue;
        }
        const auto relative = it->path().lexically_relative(resources_);
        // ディレクトリ名は動的なフォルダー参照の解決用に記録する。
        if (it->is_directory()) { directories_.insert(Lower(Text(relative))); continue; }
        // ディレクトリを除いた後も、通常ファイル以外を素材索引へ登録しない。
        if (!it->is_regular_file()) continue;
        const auto key = Lower(Text(relative));
        assets_.emplace(key, Asset{relative, it->file_size(), DevelopmentOnly(relative), {}, false});
        // 3DModel 内のモデルファイルから、フォルダー名で解決できる候補を索引へ追加する。
        if (Under(key, "3dmodel") && IsModel(relative))
            modelFolders_.insert(Lower(Text(relative.parent_path())).substr(8));
        // 制作・個人データと .meta は、短い素材名の検索候補から外す。
        if (!assets_.at(key).reason.empty() || relative.extension() == ".meta") continue;
        auto add = [&](const std::string& name) { aliases_[Lower(name)].push_back(key); };
        add(Text(relative));
        add(Text(relative.filename()));
        add(Text(relative.stem()));
        // VFX・Recorderの移動後も、ルートからの相対名と旧来の短い名前の両方を扱います。
        auto partial = relative;
        while (partial.has_parent_path()) {
            auto part = partial.begin();
            fs::path next;
            for (++part; part != partial.end(); ++part) next /= *part;
            partial = next;
            add(Text(partial));
            auto noExtension = partial;
            noExtension.replace_extension();
            add(Text(noExtension));
        }
    }
    // GUID参照は名前を推測せず、.metaが対応する実ファイルへ戻します。
    for (const auto& [key, asset] : assets_) {
        // 対象のうち、実行素材に関係するメタファイルだけ GUID の読込を試す。
        if (asset.relative.extension() != ".meta" || !asset.reason.empty()) continue;
        try {
            const auto meta = json::parse(ReadInput(resources_ / asset.relative));
            const auto guid = meta.value("guid", "");
            // GUID が記録されている場合に、拡張子の .meta を外した実体と対応づける。
            if (!guid.empty()) guids_[Lower(guid)].push_back(key.substr(0, key.size() - 5));
        } catch (const json::exception&) {
            // メタ情報の破損はレポートへ残し、通常のパス指定の解析を続けます。
            Problem(key, "", "メタ情報を解析できません");
        }
    }
}

// 索引済みの素材を使用対象にし、初回の参照理由を残して依存の調査を予約する。
void Collector::Keep(const std::string& key, const std::string& source, const std::string& reason) {
    const auto found = assets_.find(key);
    // 索引に無いか処理済みなら、依存を調べるキューへ重複して入れない。
    if (found == assets_.end() || found->second.used) return;
    auto& asset = found->second;
    // 除外理由が付けられた素材を実行時参照が指した場合は、矛盾をレポートへ残す。
    if (!asset.reason.empty()) {
        Problem(source, Text(asset.relative), "実行参照が制作・保存データの除外規則と競合: " + asset.reason);
        return;
    }
    asset.used = true;
    asset.source = source;
    asset.reason = reason;
    pending_.push_back(key);
    // 実行時にGUID解決する素材の.metaは本体と対で残します。
    Keep(key + ".meta", source, "使用素材のメタ情報");
    // 画像を採用する場合は、同じファイル名に別の画像形式があるか確認して併せて保持する。
    if (IsTexture(asset.relative)) {
        for (const char* extension : {".png", ".dds", ".jpg", ".jpeg", ".tga", ".hdr"}) {
            auto paired = asset.relative;
            paired.replace_extension(extension);
            Keep(Lower(Text(paired)), source, "使用画像のPNG/DDSペア");
        }
    }
}

// フォルダー以下の索引済み素材を保持候補にする。照合したファイルが一つでもあれば true。
bool Collector::Folder(const std::string& key, const std::string& source, const std::string& reason) {
    bool any = false;
    for (const auto& [candidate, asset] : assets_) {
        // 指定したフォルダー配下に該当しない素材は飛ばす。
        if (!Under(candidate, key)) continue;
        Keep(candidate, source, reason);
        any = true;
    }
    return any;
}

// 文字列を素材の GUID・パス・名前と照合し、解決できた候補を使用対象にする。
bool Collector::Reference(std::string value, const fs::path& relativeTo, const std::string& source, bool explicitKeep) {
    // 空文字、埋め込みデータ、複数行の文字列はファイルへの参照から外す。
    if (value.empty() || value.starts_with("data:") || value.find('\n') != std::string::npos) return false;
    // 候補の全体が一組の引用符で囲まれている場合、照合前にそれだけを外す。
    if (value.size() > 1 && value.front() == '"' && value.find('"', 1) == value.size() - 1)
        value = value.substr(1, value.size() - 2);
    value = Lower(value);
    // glTFの外部URIでは空白などが%xxで保存されるため、ファイル照合前に復号します。
    for (size_t i = 0; i + 2 < value.size(); ++i) {
        // パーセントの直後が16進2文字の場合だけ、ファイル名の一文字へ変換する。
        if (value[i] != '%' || !std::isxdigit(static_cast<unsigned char>(value[i + 1])) ||
            !std::isxdigit(static_cast<unsigned char>(value[i + 2]))) continue;
        const auto code = static_cast<char>(std::stoi(value.substr(i + 1, 2), nullptr, 16));
        value.replace(i, 3, 1, code);
    }
    // GUID 用の接頭辞があれば外し、索引の登録形式と揃える。
    if (value.starts_with("guid:")) value.erase(0, 5);
    if (const auto guid = guids_.find(value); guid != guids_.end()) {
        // GUIDの重複がある素材も、どちらかを未使用と断定せず両方を残します。
        for (const auto& candidate : guid->second) Keep(candidate, source, "GUIDの参照先");
        // 同じ GUID の実ファイルが複数ある場合は、曖昧な参照としてレポートにも残す。
        if (guid->second.size() > 1) Problem(source, value, "GUIDが重複しているため全候補を保持");
        return true;
    }
    std::string key = value.starts_with("resources/") ? value.substr(10) : value;
    const auto path = Path(key).lexically_normal();
    // キャッシュの生成先や個人セーブの保存先は、コードに登場しても配布素材にはなりません。
    if (!DevelopmentOnly(path).empty()) return false;
    // 絶対パスやコロンを含む指定は、配布素材の相対パスとして照合しない。
    if (path.is_absolute() || key.find(":") != std::string::npos) return false;

    // 同名ファイルの推測より、参照元からの相対パスとResources内の完全指定を優先します。
    const std::string local = Lower(Text((relativeTo / path).lexically_normal()));
    for (const auto& candidate : {local, Lower(Text(path))}) {
        // 索引のパスと一致すれば、曖昧な短縮名を調べる前にその参照を確定する。
        if (assets_.contains(candidate)) { Keep(candidate, source, "直接参照"); return true; }
        // PNGが存在しなくてもTextureManagerが同名DDSへ解決できる素材を保持します。
        if (IsTexture(Path(candidate))) {
            for (const auto* extension : {".dds", ".png", ".jpg", ".jpeg", ".tga", ".hdr"}) {
                auto alternative = Path(candidate);
                alternative.replace_extension(extension);
                const auto alternativeKey = Lower(Text(alternative));
                // 代替の画像形式が実際に索引にある場合、そのファイルを使って参照を解決する。
                if (assets_.contains(alternativeKey)) {
                    Keep(alternativeKey, source, "画像の代替形式");
                    return true;
                }
            }
        }
        // 旧シーンには入口JSONがなく、分割ファイルだけが残っている形式もあります。
        if (Under(candidate, "json/3dobject") && path.extension() == ".json") {
            bool foundSplit = false;
            for (const char* suffix : {"_player.json", "_enemy.json", "_object.json", "_camera.json"}) {
                const auto split = Path(candidate).parent_path() / (Text(Path(candidate).stem()) + suffix);
                const auto splitKey = Lower(Text(split));
                // 既存の分割ファイルは個別にすべて保持する。
                if (assets_.contains(splitKey)) { Keep(splitKey, source, "入口のない分割シーン"); foundSplit = true; }
            }
            // 分割されたものを一つでも解決した場合、入口名の参照は解決済みとする。
            if (foundSplit) return true;
        }
    }

    // 実際のモデル読込と共通の規則で、フォルダー名から一つのモデルを解決します。
    std::string modelName = key.starts_with("3dmodel/") ? key.substr(8) : key;
    fs::path directory, file;
    const auto requestedModel = Path(modelName);
    const auto modelFolder = requestedModel.parent_path() / requestedModel.stem();
    const bool modelCandidate = modelFolders_.contains(Lower(Text(modelFolder))) ||
                                assets_.contains("3dmodel/" + modelName);
    // 任意の文字列ごとにディスクを探さず、収集済み索引にモデル候補がある場合だけ実ロードの探索へ進みます。
    if (modelCandidate && ResolveModelAssetPath(resources_ / "3DModel", modelName, directory, file)) {
        const auto modelKey = Lower(Text((directory / file).lexically_relative(resources_)));
        // ロード規則が見つけたモデルも、索引内の素材だけを採用する。
        if (assets_.contains(modelKey)) { Keep(modelKey, source, "ModelManagerと同じ探索順で解決"); return true; }
    }

    // 動的に末尾を連結する固有フォルダーは、その範囲を保護します。ルート全体の列挙定数は起点にしません。
    while (!key.empty() && key.back() == '/') key.pop_back();
    const bool broadRoot = key == "" || key == "json" || key == "sprite" || key == "3dmodel" || key == "texture";
    // 共通ルートより具体的なフォルダー名が一致した場合は、動的な依存として中を保持する。
    if (!broadRoot && directories_.contains(key)) {
        return Folder(key, source, explicitKeep ? "追加保持フォルダー" : "動的なフォルダー参照を保護");
    }
    // 広いルートを保持するのは、追加保持で明示的に指定された場合に限る。
    if (explicitKeep && broadRoot) return Folder(key, source, "明示的な追加保持");

    // unlock_ + 番号などの動的な名前は、同じ親フォルダー内で一致する接頭辞の全候補を残します。
    if (!path.has_extension() && !broadRoot && value.starts_with("resources/")) {
        bool matchedPrefix = false;
        for (auto it = assets_.lower_bound(key); it != assets_.end() && it->first.starts_with(key); ++it) {
            Keep(it->first, source, "動的なファイル名の接頭辞を保護");
            matchedPrefix = true;
        }
        // 接頭辞が一致する素材を一つでも保持した場合は、別名での照合を続けない。
        if (matchedPrefix) return true;
    }

    // 短いプリセット名が複数フォルダーで一致する場合は全候補を保持し、除外を断定しません。
    if (const auto alias = aliases_.find(value); alias != aliases_.end()) {
        for (const auto& candidate : alias->second) Keep(candidate, source, "短い素材名の参照");
        return true;
    }
    // 具体的な Resources 内の指定を解決できなかった場合に、見直せる参照として記録する。
    if (value.starts_with("resources/") && !broadRoot)
        Problem(source, value, "直接参照を解決できません。追加保持欄で動的な参照元を指定できます");
    return false;
}

// JSON の値をたどり、同じ基準フォルダーから解決できる文字列参照を探す。
void Collector::VisitJson(const json& value, const fs::path& relativeTo, const std::string& source) {
    // 文字列は素材名として照合に渡す。
    if (value.is_string()) Reference(value.get<std::string>(), relativeTo, source);
    // 配列は要素ごとに同じ規則で調べる。
    else if (value.is_array()) for (const auto& child : value) VisitJson(child, relativeTo, source);
    // オブジェクトはキーごとに、メモや説明を除いて値を調べる。
    else if (value.is_object()) for (const auto& [key, child] : value.items()) {
        // メモや説明文にだけ登場するパスを、実行参照へ混ぜません。
        if (!key.starts_with("_") && key != "description" && key != "comment") VisitJson(child, relativeTo, source);
        // アンダースコア付きでも公開シーンのメタ項目は例外として調べる。
        else if (key == "_sceneAsset") VisitJson(child, relativeTo, source);
    }
}

// 本編ソースから、実行時に素材を読む起点になり得る文字列を集める。
void Collector::Code() {
    for (const char* directory : {"engine", "game", "application"}) {
        const auto root = project_ / directory;
        // プロジェクトに存在するソースフォルダーだけ走査する。
        if (!fs::is_directory(root)) continue;
        for (fs::recursive_directory_iterator it(root), end; it != end; ++it) {
            // 編集専用の例・テスト・外部ライブラリの参照を実行素材の根拠にしません。
            if (it->is_directory() && (it->path().filename() == "editor" || it->path().filename() == "tests")) {
                it.disable_recursion_pending();
                continue;
            }
            const auto ext = it->path().extension();
            // C++ の実装・ヘッダー・インラインの通常ファイルだけを解析する。
            if (!it->is_regular_file() || (ext != ".cpp" && ext != ".h" && ext != ".hpp" && ext != ".inl")) continue;
            const auto source = Text(it->path().lexically_relative(project_));
            for (const auto& value : StringLiterals(ReadInput(it->path()))) Reference(value, {}, source);
        }
    }
}

// 保持した素材の形式に合わせて追加の依存を調べ、対象ファイルをキューへ加える。
void Collector::Dependencies(const std::string& key) {
    const auto& asset = assets_.at(key);
    const auto path = resources_ / asset.relative;
    const auto ext = Lower(Text(path.extension()));
    // JSON か glTF 系では、構造内に含まれる素材への文字列参照をたどる。
    if (ext == ".json" || ext == ".gltf" || ext == ".glb") {
        try {
            auto text = ReadInput(path);
            // バイナリ glTF は、ヘッダーを検証して先頭の JSON 部分を取り出してから解析する。
            if (ext == ".glb") {
                // GLBヘッダーと先頭JSONチャンクをリトルエンディアンで検査し、埋込画像以外のURIも追います。
                auto word = [&](size_t offset) {
                    // 4バイトに満たない位置をヘッダーの値として読み出さない。
                    if (offset + 4 > text.size()) throw std::runtime_error("GLBヘッダーが短すぎます");
                    uint32_t result = 0;
                    for (std::int32_t i = 0; i < 4; ++i) result |= static_cast<uint32_t>(static_cast<unsigned char>(text[offset + i])) << (8 * i);
                    return result;
                };
                // 形式・バージョン・先頭チャンクのサイズと種類が想定に合う場合だけ、JSON 本文を切り出す。
                if (word(0) != 0x46546c67 || word(4) != 2 || word(12) > text.size() - 20 || word(16) != 0x4e4f534a)
                    throw std::runtime_error("GLBのJSONチャンクを確認できません");
                text = text.substr(20, word(12));
            }
            VisitJson(json::parse(text), asset.relative.parent_path(), Text(asset.relative));
            // シーンの分割ファイルはJSONに列挙されず、LevelLoaderが同じ名前から導出します。
            if (Under(key, "json/3dobject")) {
                for (const char* suffix : {"_player.json", "_enemy.json", "_object.json", "_camera.json"}) {
                    const auto split = asset.relative.parent_path() / (Text(asset.relative.stem()) + suffix);
                    Keep(Lower(Text(split)), key, "LevelLoaderの分割シーン");
                }
            }
        } catch (const std::exception& error) {
            Problem(key, "", error.what());
            // 読めないモデルの外部依存は断定せず、同梱ファイルを残します。
            if (ext != ".json") Folder(Lower(Text(asset.relative.parent_path())), key, "解析できないモデルの関連ファイルを保護");
            // 実行に必要な通常の JSON を読めない場合は、依存漏れを避けるためプランの生成を失敗させる。
            else throw std::runtime_error("必要なJSONを解析できません: " + key + " / " + error.what());
        }
    // OBJ と MTL はテキスト行にある材質・画像の外部参照を追跡する。
    } else if (ext == ".obj" || ext == ".mtl") {
        std::istringstream stream(ReadInput(path));
        for (std::string line; std::getline(stream, line);) {
            std::istringstream fields(line);
            std::string command;
            fields >> command;
            command = Lower(command);
            // OBJ は材質一覧の指定、MTL は画像・法線・反射などの指定行だけを素材参照として扱う。
            if ((ext == ".obj" && command != "mtllib") ||
                (ext == ".mtl" && !command.starts_with("map_") && command != "bump" && command != "norm" &&
                 command != "disp" && command != "decal" && command != "refl")) continue;
            std::string rest;
            std::getline(fields >> std::ws, rest);
            // Windows の行末に残る CR は、素材名の一部に含めない。
            if (!rest.empty() && rest.back() == '\r') rest.pop_back();
            // 参照全体を囲む引用符がある場合は、探索前に外す。
            if (rest.size() >= 2 && rest.front() == '"' && rest.find('"', 1) == rest.size() - 1) rest = rest.substr(1, rest.size() - 2);
            // 空白を含むファイル名を先に試し、見つからなければMTLのオプションを一つずつ除きます。
            bool found = Reference(rest, asset.relative.parent_path(), key);
            // 行全体を解決できず OBJ の材質指定なら、引用符付きの複数名として試す。
            if (!found && command == "mtllib") {
                // OBJが一行で複数のMTLを指定する場合も、それぞれの依存を残します。
                std::istringstream names(rest);
                for (std::string name; names >> std::quoted(name);)
                    found = Reference(name, asset.relative.parent_path(), key) || found;
            // それ以外では、材質オプションなど先頭の語を除きながら再度照合する。
            } else {
                while (!found && rest.find(' ') != std::string::npos) {
                    rest.erase(0, rest.find(' ') + 1);
                    found = Reference(rest, asset.relative.parent_path(), key);
                }
            }
            // 外部参照を特定できなければ、診断を残してモデルと同じフォルダー全体を保護する。
            if (!found) {
                Problem(key, rest, "材質の外部依存を解決できないためモデルのフォルダーを保持");
                Folder(Lower(Text(asset.relative.parent_path())), key, "モデル外部依存の保護");
            }
        }
    // テキストで依存を追えない他形式のモデルは、親フォルダーを保守的に保持する。
    } else if (IsModel(path)) {
        Folder(Lower(Text(asset.relative.parent_path())), key, "FBX等の外部依存を保護");
    }
}

// 素材の索引から公開シーン・コード・明示保持を起点に依存をたどり、配布可否と理由を一覧化する。
RuntimeResourcePlan Collector::Build(const std::string& keep) {
    Index();
    // シェーダーのinclude、フォント、ルート直下のシステム素材は実行基盤として保持します。
    for (const auto& [key, asset] : assets_) {
        const auto ext = Lower(Text(asset.relative.extension()));
        if (ext == ".hlsl" || ext == ".hlsli" || ext == ".cso" || Under(key, "font") ||
            (asset.relative.parent_path().empty() && ext != ".meta"))
            Keep(key, "実行基盤", "シェーダー・フォント・共通素材");
        // エディターで公開したシーンを全て起点にし、新規シーン追加でも配布設定の修正を不要にします。
        if (Under(key, "json/3dobject") && ext == ".json" && asset.reason.empty()) {
            try {
                const auto data = json::parse(ReadInput(resources_ / asset.relative));
                // シーンアセットのメタ情報を持つ配置は、コードから直接参照されない場合も起点にする。
                if (data.is_object() && data.contains("_sceneAsset")) Keep(key, "Scene Asset", "公開されたシーン");
            } catch (const json::exception&) {
                Problem(key, "", "シーン候補のJSONを解析できません");
            }
        }
    }
    Code();
    // 人が指定した動的な素材を起点に加えます。存在しない指定を黙って無視すると配布を壊すため失敗にします。
    std::istringstream lines(keep);
    for (std::string line; std::getline(lines, line);) {
        const auto begin = line.find_first_not_of(" \t\r");
        // 追加保持欄の空白だけの行は指定として扱わない。
        if (begin == std::string::npos) continue;
        line = line.substr(begin, line.find_last_not_of(" \t\r") - begin + 1);
        // 指定先を解決できない場合は、設定漏れを見直せるよう配布プランを失敗させる。
        if (!Reference(line, {}, "追加保持欄", true)) throw std::runtime_error("追加保持の指定が見つかりません: " + line);
    }
    while (!pending_.empty()) {
        auto key = std::move(pending_.front());
        pending_.pop_front();
        Dependencies(key);
    }

    RuntimeResourcePlan result;
    for (const auto& [path, input] : inputs_) result.inputs.push_back(input);
    result.report = {{"formatVersion", 1}, {"files", json::array()}, {"unresolvedReferences", problems_}};
    uint64_t keptBytes = 0, excludedBytes = 0, keptCount = 0;
    for (const auto& [key, asset] : assets_) {
        // 保持する素材は採用件数と容量に加える。
        if (asset.used) { keptBytes += asset.bytes; ++keptCount; }
        // 除外する素材は容量と、コピー側が照合する相対パスを記録する。
        else { excludedBytes += asset.bytes; result.excludedPaths.insert(CopyKey(asset.relative)); }
        result.report["files"].push_back({{"path", Text(asset.relative)}, {"included", asset.used},
            {"bytes", asset.bytes}, {"source", asset.source},
            {"reason", asset.reason.empty() ? "実行の起点から参照がありません" : asset.reason}});
    }
    result.report["summary"] = {{"includedFiles", keptCount}, {"excludedFiles", result.excludedPaths.size()},
                               {"includedBytes", keptBytes}, {"excludedBytes", excludedBytes}};
    return result;
}
} // namespace

// 配布の採否一覧を構築できたときに true を返す。失敗理由は error へ渡す。
bool BuildRuntimeResourcePlan(const std::filesystem::path& projectRoot, const std::string& additionalKeep,
                              RuntimeResourcePlan& result, std::string& error) {
    result = {};
    error.clear();
    try {
        result = Collector(projectRoot).Build(additionalKeep);
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}
// 解析に用いた入力のサイズと更新時刻を再確認し、同じ入力のままなら true を返す。
bool ValidateRuntimeResourcePlan(const RuntimeResourcePlan& plan, std::string& error) {
    for (const auto& input : plan.inputs) {
        std::error_code code;
        const auto bytes = fs::file_size(input.path, code);
        // サイズと更新時刻が同じで、両方とも取得できた入力だけ次の照合へ進む。
        if (!code && bytes == input.bytes && fs::last_write_time(input.path, code) == input.modified && !code)
            continue;
        error = "素材またはコードが解析後に更新されました。もう一度作成してください: " + Text(input.path);
        return false;
    }
    error.clear();
    return true;
}

} // namespace engine
