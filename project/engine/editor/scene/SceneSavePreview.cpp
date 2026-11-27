#define NOMINMAX
#include <cstdint>
#include "SceneSavePreview.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
bool ReadJsonFile(const std::string& path, json& outJson) {
    // 空ファイルは空オブジェクトとして扱い、構文エラーは保存差分の検証失敗として返す。
    std::ifstream file(path);

    // 既存ファイルを開けなければ、比較用JSONの読み込み失敗を返す。
    if (!file.is_open()) {
        return false;
    }

    try {
        // 一文字もないファイルは空のJSONオブジェクトとして比較できるようにする。
        if (file.peek() == std::ifstream::traits_type::eof()) {
            outJson = json::object();
        }
        // 内容があればJSONとして解析し、構文エラーは後段で失敗として扱う。
        else {
            file >> outJson;
        }

        return true;
    }
    // 解析中に例外が出た場合は、中途半端なJSONを残さず失敗を返す。
    catch (...) {
        outJson = json::object();
        return false;
    }
}

// Scene保存JSONにObject一覧が含まれるかを確認します。
bool HasObjectList(const json& value) {
    return value.is_object() && value.contains("objects") && value["objects"].is_array();
}

// JSONの子フィールドを差分一覧に表示するため、ドット区切りの位置を作ります。
std::string JoinPath(const std::string& parent, const std::string& child) {
    // 親パスが空のときは、子のキーを先頭の要素にする。
    if (parent.empty())
        return child;
    return parent + "." + child;
}

// 配列要素の差分を識別できるよう、親の位置へ添字を付けます。
std::string MakeArrayPath(const std::string& parent, size_t index) {
    std::ostringstream oss;
    oss << parent << "[" << index << "]";
    return oss.str();
}
}

void SceneSavePreview::Build(const std::vector<SceneSerializer::SaveTarget>& targets, const std::string& title) {
    // 保存予定の複数ファイルを先に差分化し、確認画面を開く前に件数を確定する。
    targets_ = targets;
    title_ = title;
    savedFilesLabel_.clear();
    fileDiffs_.clear();
    summary_ = {};

    for (const auto& target : targets_) {
        // ラベルが二つ目以降なら、前のラベルとカンマで区切る。
        if (!savedFilesLabel_.empty()) {
            savedFilesLabel_ += ", ";
        }

        savedFilesLabel_ += target.label;
        fileDiffs_.push_back(BuildFileDiff(target));
    }

    UpdateSummary();
}

// 次のImGui描画時に保存確認のポップアップを開く要求を立てます。
void SceneSavePreview::Open() {
    isOpen_ = true;
    requestOpenPopup_ = true;
}

// 表示状態と未処理のポップアップ開始要求をともに解除します。
void SceneSavePreview::Close() {
    isOpen_ = false;
    requestOpenPopup_ = false;
}

SceneSavePreview::FileDiff SceneSavePreview::BuildFileDiff(const SceneSerializer::SaveTarget& target) const {
    // 旧ファイルを読めなくても保存予定データの確認は続け、問題を差分に表示します。
    FileDiff diff;
    diff.target = target;
    diff.oldFileExists = fs::exists(target.path);
    diff.hasObjectList = HasObjectList(target.data);

    json oldJson = json::object();

    // 旧ファイルがある場合は、比較のため現状を読み込む。
    if (diff.oldFileExists) {
        diff.oldJsonValid = ReadJsonFile(target.path, oldJson);

        // 比較元を読めなかった場合は、上書きになる旨を一覧へ入れる。
        if (!diff.oldJsonValid) {
            diff.fileDetails.push_back("既存JSONの読み込みに失敗しました。保存すると現在の内容で上書きします。");
        }
    }
    // 旧ファイルが無ければ、既存データを置き換えない新規作成として示す。
    else {
        diff.oldJsonValid = true;
        diff.fileDetails.push_back("新規ファイルとして作成されます。");
    }

    // 保存予定JSONにオブジェクト配列があれば、名前で照合する詳細を作る。
    if (diff.hasObjectList) {
        BuildObjectDiff(diff, oldJson, target.data);
    }
    // オブジェクト配列が無ければ、ファイル全体の値の変化を示す。
    else {
        BuildFileLevelDiff(diff, oldJson, target.data);
    }

    return diff;
}

void SceneSavePreview::BuildObjectDiff(FileDiff& diff, const json& oldJson, const json& newJson) const {
    // Object一覧は名前をキーにして旧版と新版を突き合わせる。
    std::map<std::string, json> oldObjects;
    std::map<std::string, json> newObjects;

    // 名前のない要素は新旧で別のキーにし、偶然同じ添字でも同一Objectに見立てない。
    if (HasObjectList(oldJson)) {
        std::int32_t unnamedIndex = 0;

        for (const auto& obj : oldJson["objects"]) {
            std::string name = GetObjectName(obj, "__old_unnamed_" + std::to_string(unnamedIndex++));
            oldObjects[name] = obj;
        }
    }

    // 保存予定側に対象配列がある場合は、新版専用の仮名を使って索引を作る。
    if (HasObjectList(newJson)) {
        std::int32_t unnamedIndex = 0;

        for (const auto& obj : newJson["objects"]) {
            std::string name = GetObjectName(obj, "__new_unnamed_" + std::to_string(unnamedIndex++));
            newObjects[name] = obj;
        }
    }

    // 新版に存在するObjectを追加・変更・同一へ分類し、内容変更は詳細まで辿る。
    for (const auto& [name, newObj] : newObjects) {
        auto oldIt = oldObjects.find(name);
        ObjectChange change;
        change.name = name;
        change.category = GetObjectCategory(newObj);

        // 新版にだけある名前を、保存で追加されるオブジェクトに数える。
        if (oldIt == oldObjects.end()) {
            change.kind = ChangeKind::kAdded;
            change.details.push_back("type: " + JsonValueToText(newObj.contains("type") ? newObj["type"] : json("")));
            change.details.push_back("modelName: " + JsonValueToText(newObj.contains("modelName") ? newObj["modelName"] : json("")));
            // 追加した定義に位置のキーがある場合だけ、座標も要約する。
            if (newObj.contains("position"))
                change.details.push_back("position: " + JsonValueToText(newObj["position"]));
            diff.addedCount++;
        }
        // 同じ名前で値が変わっている場合は、最大12件の変更理由を集める。
        else if (oldIt->second != newObj) {
            change.kind = ChangeKind::kModified;
            CollectJsonDiffs(oldIt->second, newObj, "", change.details, 12);

            // 内容は異なるが詳細行を作れなかった場合は、変更がある旨を一行残す。
            if (change.details.empty()) {
                change.details.push_back("JSONの内容が変更されています。");
            }

            diff.modifiedCount++;
        }
        // 名前と内容が同じ場合は、未変更として数える。
        else {
            change.kind = ChangeKind::kUnchanged;
            change.details.push_back("変更なし");
            diff.unchangedCount++;
        }

        diff.objects.push_back(std::move(change));
    }

    // 旧版にしか残っていないObjectは保存後の削除として表示する。
    for (const auto& [name, oldObj] : oldObjects) {
        // 同じ名前が新版にも残っていれば、削除として扱わない。
        if (newObjects.find(name) != newObjects.end())
            continue;

        ObjectChange change;
        change.kind = ChangeKind::kRemoved;
        change.name = name;
        change.category = GetObjectCategory(oldObj);
        change.details.push_back("保存後のJSONから削除されます。");
        // 削除される定義に型名がある場合は、項目に型を補足する。
        if (oldObj.contains("type"))
            change.details.push_back("type: " + JsonValueToText(oldObj["type"]));
        diff.removedCount++;
        diff.objects.push_back(std::move(change));
    }

    // 追加・削除・変更を上から確認できるよう種類順に並べ、同じ種類は名前順に揃える。
    std::sort(diff.objects.begin(), diff.objects.end(), [](const ObjectChange& a, const ObjectChange& b) {
        // 種類が違う場合は種別順、同じ種類は名前順で並べる。
        if (a.kind != b.kind)
            return static_cast<std::int32_t>(a.kind) < static_cast<std::int32_t>(b.kind);
        return a.name < b.name;
    });
}

void SceneSavePreview::BuildFileLevelDiff(FileDiff& diff, const json& oldJson, const json& newJson) const {
    // Object一覧を持たないメタ情報などは、ファイルのJSON全体で変更を比較する。
    if (!diff.oldFileExists) {
        diff.fileDetails.push_back("メタファイルを新規作成します。");
        return;
    }

    // 旧ファイルを解析できなければ、値を比べず置き換えとして報告する。
    if (!diff.oldJsonValid) {
        diff.fileDetails.push_back("読み込めなかった既存ファイルを置き換えます。");
        return;
    }

    // JSON全体が一致する場合は、詳細な比較をせず変更なしとする。
    if (oldJson == newJson) {
        diff.fileDetails.push_back("ファイル内容に変更はありません。");
        return;
    }

    CollectJsonDiffs(oldJson, newJson, "", diff.fileDetails, 16);

    // 変更があるのに詳細行を得られない場合は、ファイルが変わる旨だけを出す。
    if (diff.fileDetails.empty()) {
        diff.fileDetails.push_back("ファイル内容が変更されています。");
    }
}

void SceneSavePreview::UpdateSummary() {
    // ファイルとObjectの件数を別々に集計し、既存JSONの読込失敗も独立して数える。
    summary_.fileCount = static_cast<std::int32_t>(fileDiffs_.size());

    for (const auto& diff : fileDiffs_) {
        summary_.addedObjects += diff.addedCount;
        summary_.removedObjects += diff.removedCount;
        summary_.modifiedObjects += diff.modifiedCount;
        summary_.unchangedObjects += diff.unchangedCount;

        // 旧ファイルが無い保存先を、新規ファイルとして数える。
        if (!diff.oldFileExists) {
            summary_.addedFiles++;
        }
        // 旧JSONを読めない場合は、通常の変更と区別して無効件数に加える。
        else if (!diff.oldJsonValid) {
            summary_.invalidFiles++;
        }
        // オブジェクトの追加・削除・変更のいずれかがあれば、変更ファイルに数える。
        else if (diff.addedCount > 0 || diff.removedCount > 0 || diff.modifiedCount > 0) {
            summary_.modifiedFiles++;
        }
        // オブジェクト列を持たず、ファイル単位の詳細が一行で変更なし表示でもない場合は、変更件数に含める。
        else if (!diff.hasObjectList && diff.fileDetails.size() == 1 && diff.fileDetails[0] != "ファイル内容に変更はありません。") {
            summary_.modifiedFiles++;
        }
    }
}

std::string SceneSavePreview::GetObjectName(const json& obj, const std::string& fallback) {
    // 定義に文字列のnameがあれば、その名前を照合に使う。
    if (obj.is_object() && obj.contains("name") && obj["name"].is_string()) {
        return obj["name"].get<std::string>();
    }

    return fallback;
}

std::string SceneSavePreview::GetObjectCategory(const json& obj) {
    // 保存カテゴリが文字列で指定されている場合にだけ、元の分類名を表示に使う。
    if (obj.is_object() && obj.contains("saveCategory") && obj["saveCategory"].is_string()) {
        return obj["saveCategory"].get<std::string>();
    }

    return "Object";
}

std::string SceneSavePreview::JsonValueToText(const json& value) {
    // 差分一覧に収まるよう短い配列は展開し、大きい配列やObjectは件数で要約する。
    if (value.is_null())
        return "null";
    // 文字列は引用符で囲み、真偽値や数値と区別する。
    if (value.is_string())
        return "\"" + value.get<std::string>() + "\"";
    // 真偽値はJSONと同じtrueまたはfalseで表す。
    if (value.is_boolean())
        return value.get<bool>() ? "true" : "false";
    // 浮動小数点は、一覧で読みやすいよう小数点以下を三桁に揃える。
    if (value.is_number_float()) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(3) << value.get<double>();
        return oss.str();
    }

    // 整数として判定された値は、long longへ取り出して十進表記にする。
    if (value.is_number_integer())
        return std::to_string(value.get<long long>());
    // 符号なし整数専用の表示経路。上の整数判定も符号なし値を含むため、現行のJSON型判定では先にそちらを通る。
    if (value.is_number_unsigned())
        return std::to_string(value.get<unsigned long long>());
    // 配列は、要素数に応じて内容または件数を表示する。
    if (value.is_array()) {
        // 六要素以下の場合だけ、個々の値を再帰的に短く整形する。
        if (value.size() <= 6) {
            std::string text = "[";

            for (size_t i = 0; i < value.size(); ++i) {
                // 二要素目以降には、前の要素との間にカンマを入れる。
                if (i > 0)
                    text += ", ";
                text += JsonValueToText(value[i]);
            }

            text += "]";
            return text;
        }

        return "array(" + std::to_string(value.size()) + ")";
    }

    // オブジェクトは中身を展開せず、キーの件数を示す。
    if (value.is_object())
        return "object(" + std::to_string(value.size()) + ")";
    return value.dump();
}

void SceneSavePreview::CollectJsonDiffs(const json& before, const json& after, const std::string& path, std::vector<std::string>& outDetails, std::int32_t maxDetails) {
    // 深い JSON でも UI が大量の行で埋まらないよう、表示件数を制限します。
    if (static_cast<std::int32_t>(outDetails.size()) >= maxDetails)
        return;
    // 値が等しい場合は、差分行を増やさず終了する。
    if (before == after)
        return;

    // Object同士はキーの和集合を作り、追加・削除・変更を各パスへ展開する。
    if (before.is_object() && after.is_object()) {
        std::vector<std::string> keys;

        for (auto it = before.begin(); it != before.end(); ++it)
            keys.push_back(it.key());

        for (auto it = after.begin(); it != after.end(); ++it) {
            // 新版だけにあるキーを和集合へ追加し、両方にあるキーを重複させない。
            if (std::find(keys.begin(), keys.end(), it.key()) == keys.end()) {
                keys.push_back(it.key());
            }
        }

        std::sort(keys.begin(), keys.end());

        for (const std::string& key : keys) {
            // 最大件数に達したら、残りの子要素を確認しない。
            if (static_cast<std::int32_t>(outDetails.size()) >= maxDetails)
                break;
            bool hasBefore = before.contains(key);
            bool hasAfter = after.contains(key);
            std::string childPath = JoinPath(path, key);

            // 旧版にキーが無い場合は、新版の値を追加として表示する。
            if (!hasBefore) {
                outDetails.push_back(childPath + ": 追加 " + JsonValueToText(after[key]));
            }
            // 新版にキーが無い場合は、旧版の値を削除として表示する。
            else if (!hasAfter) {
                outDetails.push_back(childPath + ": 削除 " + JsonValueToText(before[key]));
            }
            // 両方にキーがある場合は、同じ子パスの値同士をさらに比較する。
            else {
                CollectJsonDiffs(before[key], after[key], childPath, outDetails, maxDetails);
            }
        }

        return;
    }

    // 小さな同長配列だけ要素ごとに追い、それ以外は値のまとまりとして示す。
    if (before.is_array() && after.is_array()) {
        // 要素数が一致し三つ以下の場合だけ、添字で対応付けて差分を出す。
        if (before.size() == after.size() && before.size() <= 3) {
            for (size_t i = 0; i < before.size(); ++i) {
                // 詳細行の上限まで出せたら、現在の配列の残りは比較しない。
                if (static_cast<std::int32_t>(outDetails.size()) >= maxDetails)
                    break;
                CollectJsonDiffs(before[i], after[i], MakeArrayPath(path, i), outDetails, maxDetails);
            }

            return;
        }
    }

    // 型が異なる場合や個別の要素へ展開しない配列などは、現在のパスで値の変化を一行にする。
    std::string label = path.empty() ? "(root)" : path;
    outDetails.push_back(label + ": " + JsonValueToText(before) + " -> " + JsonValueToText(after));
}
