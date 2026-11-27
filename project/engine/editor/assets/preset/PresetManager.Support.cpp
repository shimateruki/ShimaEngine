#include <cstdint>
#include "engine/editor/assets/preset/PresetManager.Internal.h"
#include "game/system/scene/GimmickFactory.h"
#include "engine/editor/assets/EditorObjectDefaults.h"

using namespace presetManagerImplementation;

namespace presetManagerImplementation {
std::string ReadString(const json& source, const char* key, const std::string& fallback) {
    // 指定のキーが文字列として存在する場合だけ取り出し、それ以外は呼び出し元の既定値を返す。
    if (source.is_object() && source.contains(key) && source.at(key).is_string()) {
        return source.at(key).get<std::string>();
    }

    return fallback;
}

std::string ResolveEnemyType(const json& node) {
    // 古い汎用Paramの敵種別より、明示されたオブジェクト種別を優先します。
    const std::string type = ReadString(node, "type");
    // 明示された種別がEnemy以外なら、旧パラメーターに敵の設定が残っていても敵として生成しない。
    if (!type.empty() && type != "Enemy")
        return {};
    std::string enemyType = ReadString(node, "enemyType");

    // 直下の敵種別が無ければ、旧式のparam内から種別を探す。
    if (enemyType.empty() && node.contains("param") && node["param"].is_object()) {
        enemyType = ReadString(node["param"], "enemyType");
    }

    return enemyType;
}

// 旧プレハブの識別子を復元するため、同じ文字列の各バイトから毎回同じ値を得る。
std::uint64_t HashText(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ull;

    for (unsigned char value : text) {
        hash ^= value;
        hash *= 1099511628211ull;
    }

    return hash;
}

// 識別番号を接頭辞と16桁の16進表記で組み立てる。
std::string MakeHexId(const std::string& prefix, std::uint64_t value) {
    std::ostringstream stream;
    stream << prefix << '-' << std::hex << std::setw(16) << std::setfill('0') << value;
    return stream.str();
}

// 同じ入力から同じ識別子を得る。旧形式を開き直しても差分の参照先を維持できる。
std::string MakeStableId(const std::string& prefix, const std::string& seed) {
    return MakeHexId(prefix, HashText(seed));
}

std::string MakeUniqueId(const std::string& prefix) {
    static std::atomic<std::uint64_t> sCounter = 0;
    // 同じ時刻に生成を繰り返しても入力が変わるよう、時刻に通し番号を混ぜる。
    const auto ticks = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    return MakeHexId(prefix, ticks ^ (++sCounter * 0x9E3779B97F4A7C15ull));
}

// Prefab差分用にJSONを比較します。数値には小さい保存誤差を許容します。
bool JsonValuesEqual(const json& lhs, const json& rhs) {
    // 浮動小数の保存誤差を許容しつつ、配列の要素は順番どおりに比較する。
    if (lhs.is_number() && rhs.is_number()) {
        return std::fabs(lhs.get<double>() - rhs.get<double>()) <= 0.00001;
    }

    // 両方が配列なら、数値の許容誤差を子要素にも適用するため要素単位で調べる。
    if (lhs.is_array() && rhs.is_array()) {
        // 配列の長さが異なれば同じ値にはならない。
        if (lhs.size() != rhs.size()) {
            return false;
        }

        for (std::size_t index = 0; index < lhs.size(); ++index) {
            // 途中の要素が異なれば、それ以降を比較せず不一致を返す。
            if (!JsonValuesEqual(lhs[index], rhs[index])) {
                return false;
            }
        }

        return true;
    }

    return lhs == rhs;
}

// Assetへ移すノードと子孫から、シーン固有のGUIDを除きます。
void RemoveSceneObjectGuids(json& node) {
    // JSONオブジェクト以外には保存時に除くGUIDキーがない。
    if (!node.is_object())
        return;
    node.erase("guid");
    // 保存済みの子の配列がなければ、現在のノードだけを処理する。
    if (!node.contains("children") || !node["children"].is_array())
        return;
    for (json& child : node["children"]) {
        RemoveSceneObjectGuids(child);
    }
}

// 親子Objectを再帰的に保存用JSONへ変換し、循環参照を避けます。
json BuildPresetNode(engine::Object3D* object, std::unordered_set<const engine::Object3D*>& visited) {
    // 親子参照の循環や重複を避け、Scene固有GUIDを除いて再利用可能なツリーを作ります。
    if (!object || visited.count(object) != 0) {
        return json::object();
    }

    visited.insert(object);
    json node = object->ExportToJson();
    node.erase("guid");
    node.erase("prefabInstance");
    node.erase("prefabObjectId");
    json children = json::array();

    for (engine::Object3D* child : object->GetChildren()) {
        json childNode = BuildPresetNode(child, visited);

        // 循環や重複で空の結果を返した子は保存配列に入れない。
        if (!childNode.empty()) {
            children.push_back(std::move(childNode));
        }
    }

    // 保存する子がある場合だけ、その一覧を現在のノードへ結び付ける。
    if (!children.empty()) {
        node["children"] = std::move(children);
    }

    return node;
}

// Prefabの安定IDを残しながらObject階層をAsset用ノードへ変換します。
json BuildPrefabNode(
    engine::Object3D* object,
    const std::string& assetId,
    std::unordered_set<const engine::Object3D*>& visited) {
    // ポインターがないか処理済みの場合は、階層の重複や無限再帰を防ぐため省く。
    if (!object || visited.count(object) != 0) {
        return json::object();
    }

    // 元Prefabから来たObjectは安定IDを引き継ぎ、追加したObjectだけ新IDを発行します。
    visited.insert(object);
    json node = object->ExportToJson();
    node.erase("guid");
    node.erase("prefabInstance");

    const auto& instance = object->GetPrefabInstanceInfo();

    // 同じAssetに由来し元ノードIDが残る場合は、そのIDを変更せず上書きから追跡できるようにする。
    if (instance.assetId == assetId && !instance.sourceObjectId.empty()) {
        node["prefabObjectId"] = instance.sourceObjectId;
    // 新規または別Asset由来なら、この階層用のIDを割り当てる。
    } else {
        node["prefabObjectId"] = MakeUniqueId("object");
    }

    json children = json::array();

    for (engine::Object3D* child : object->GetChildren()) {
        json childNode = BuildPrefabNode(child, assetId, visited);

        // 子の生成が空になった場合は、保存される階層に含めない。
        if (!childNode.empty()) {
            children.push_back(std::move(childNode));
        }
    }

    // 保存する子があるときだけ現在のノードへ子の配列を設定する。
    if (!children.empty()) {
        node["children"] = std::move(children);
    }

    return node;
}

// Assetと階層位置から、IDのないPrefabノードへ安定IDを補います。
void EnsurePrefabObjectIds(json& node, const std::string& assetId, const std::string& hierarchyPath) {
    // ノードの形式がオブジェクトでなければIDを補えない。
    if (!node.is_object()) {
        return;
    }

    // 旧AssetでIDが欠ける場合だけ、Asset IDと階層位置から再現可能なIDを補います。
    if (!node.contains("prefabObjectId") || !node["prefabObjectId"].is_string() ||
        node["prefabObjectId"].get<std::string>().empty()) {
        node["prefabObjectId"] = MakeStableId("object", assetId + ':' + hierarchyPath);
    }

    node.erase("prefabInstance");

    // Prefab AssetはScene上の実体GUIDを所有しません。
    node.erase("guid");

    // 子の配列がない場合はこれ以上の階層を補わない。
    if (!node.contains("children") || !node["children"].is_array()) {
        return;
    }

    for (std::size_t index = 0; index < node["children"].size(); ++index) {
        EnsurePrefabObjectIds(node["children"][index], assetId,
            hierarchyPath + '/' + std::to_string(index));
    }
}

// 旧Variantに不足する差分フィールドを空の配列・オブジェクトで補います。
void EnsurePrefabVariantFields(json& asset) {
    // 旧Variantの不足フィールドを空の差分として補い、現在の読込処理へ渡します。
    // 削除したノードの一覧が無いか配列でなければ、削除なしとして初期化する。
    if (!asset.contains("removedObjectIds") || !asset["removedObjectIds"].is_array()) {
        asset["removedObjectIds"] = json::array();
    }

    // 追加した子の一覧が無いか形式が違えば、空の配列に揃える。
    if (!asset.contains("addedChildren") || !asset["addedChildren"].is_array()) {
        asset["addedChildren"] = json::array();
    }

    // 親変更の記録が無いか形式が違えば、空の配列に揃える。
    if (!asset.contains("reparentedObjects") || !asset["reparentedObjects"].is_array()) {
        asset["reparentedObjects"] = json::array();
    }

    // ノードごとの値の差分は、キーで参照できるオブジェクトとして持つ。
    if (!asset.contains("nodeOverrides") || !asset["nodeOverrides"].is_object()) {
        asset["nodeOverrides"] = json::object();
    }

    // 部品の有無の差分も、未定義または別形式なら空のオブジェクトを持つ。
    if (!asset.contains("componentOverrides") || !asset["componentOverrides"].is_object()) {
        asset["componentOverrides"] = json::object();
    }
}

// 親IDと兄弟内の位置を含む比較用索引を作る。ノードへのポインタは参照元ツリーの生存中に使う。
void CollectPrefabNodeLocations(
    const json& node,
    const std::string& parentObjectId,
    std::size_t order,
    std::unordered_map<std::string, PrefabNodeLocation>& locations) {
    // オブジェクト以外の値は索引のノードとして記録できない。
    if (!node.is_object()) {
        return;
    }

    const std::string objectId = node.value("prefabObjectId", "");

    // 比較キーになるIDを持つノードだけ、親と子の順番を記録する。
    if (!objectId.empty()) {
        locations[objectId] = { &node, parentObjectId, order };
    }

    // 子の配列を持たないノードでは、この階層までで探索を終える。
    if (!node.contains("children") || !node["children"].is_array()) {
        return;
    }

    for (std::size_t childIndex = 0; childIndex < node["children"].size(); ++childIndex) {
        CollectPrefabNodeLocations(node["children"][childIndex], objectId, childIndex, locations);
    }
}

// 現ノードと子孫を再帰的に数えます。
std::int32_t CountPrefabNodes(const json& node) {
    // オブジェクトの形を持つノードだけを個数に含める。
    if (!node.is_object()) {
        return 0;
    }

    std::int32_t count = 1;

    // 子の配列がある場合だけ、子孫の個数も合計する。
    if (node.contains("children") && node["children"].is_array()) {
        for (const auto& child : node["children"]) {
            count += CountPrefabNodes(child);
        }
    }

    return count;
}

// Prefab元から継承できる既知IDの子をOverride側から除きます。
void RemovePrefabChildrenWithKnownIds(json& node, const std::unordered_set<std::string>& knownIds) {
    // 既知IDの子はOverrideから除き、Prefab本体から継承できる状態を維持する。
    if (!node.is_object() || !node.contains("children") || !node["children"].is_array()) {
        return;
    }

    auto& children = node["children"];

    for (auto it = children.begin(); it != children.end();) {
        // 子がオブジェクトでなければ、継承できるIDは無いと見なす。
        const std::string childId = it->is_object() ? it->value("prefabObjectId", "") : "";

        // Prefab本体に同じIDがある子は、この子以下を本体から継承できる。
        if (!childId.empty() && knownIds.find(childId) != knownIds.end()) {
            it = children.erase(it);
        // IDが不明な子は残し、その内側に継承できる子があるか調べる。
        } else {
            RemovePrefabChildrenWithKnownIds(*it, knownIds);
            ++it;
        }
    }

    // 差分として保持する子が全て無くなったら、空のchildrenも消す。
    if (children.empty()) {
        node.erase("children");
    }
}

// 指定IDの子孫ノードを階層から一つ削除し、削除できた場合にtrueを返します。
bool RemovePrefabNodeRecursive(json& root, const std::string& sourceObjectId) {
    // Prefab内の識別子を階層から探し、最初に一致した子だけを削除する。
    if (!root.is_object() || !root.contains("children") || !root["children"].is_array()) {
        return false;
    }

    auto& children = root["children"];

    for (auto childIt = children.begin(); childIt != children.end(); ++childIt) {
        // 調べている子のIDが一致した時点で、その枝を子一覧から消す。
        if (childIt->is_object() && childIt->value("prefabObjectId", "") == sourceObjectId) {
            children.erase(childIt);
            return true;
        }

        // 子孫で対象を削除できたら、残りの枝を調べず呼び出し元へ成功を返す。
        if (RemovePrefabNodeRecursive(*childIt, sourceObjectId)) {
            return true;
        }
    }

    return false;
}

// 指定IDの子孫ノードを取り出して元の親から外し、移動先へ渡します。
bool TakePrefabNodeRecursive(json& root, const std::string& sourceObjectId, json& removedNode) {
    // ノードを移動用に取り出して元の親から外し、識別子を保持したまま再配置できるようにする。
    if (!root.is_object() || !root.contains("children") || !root["children"].is_array()) {
        return false;
    }

    auto& children = root["children"];

    for (auto childIt = children.begin(); childIt != children.end(); ++childIt) {
        // 一致する子を見つけたら値を引数へ移し、元の並びから該当項目を外す。
        if (childIt->is_object() && childIt->value("prefabObjectId", "") == sourceObjectId) {
            removedNode = std::move(*childIt);
            children.erase(childIt);
            return true;
        }

        // 子孫からノードを取り出せたら、親の探索もその時点で終える。
        if (TakePrefabNodeRecursive(*childIt, sourceObjectId, removedNode)) {
            return true;
        }
    }

    return false;
}

// 指定親の子一覧へ順序を指定して挿入します。親がなければfalseを返します。
bool InsertPrefabChild(json& root, const std::string& parentObjectId, std::size_t order, json child) {
    // 指定順が子の総数を超える場合は末尾へ追加する。
    json* parent = FindSourceNodeRecursive(root, parentObjectId);

    // 指定した親IDが見つからなければ、元の階層を変更せず失敗を返す。
    if (!parent) {
        return false;
    }

    // 子を初めて持つか子の形式が不正な親には、配列を準備する。
    if (!parent->contains("children") || !(*parent)["children"].is_array()) {
        (*parent)["children"] = json::array();
    }

    auto& children = (*parent)["children"];
    const std::size_t insertOrder = (std::min)(order, children.size());
    children.insert(children.begin() + static_cast<json::difference_type>(insertOrder), std::move(child));
    return true;
}

// nullを削除として扱い、Prefabの差分JSONを対象へ再帰的に適用します。
void ApplyJsonMergePatch(json& target, const json& patch) {
    // nullは削除、オブジェクトは再帰的に統合し、それ以外は値を置き換える。
    // オブジェクト以外のパッチは、対象の値全体を置き換える。
    if (!patch.is_object()) {
        target = patch;
        return;
    }

    // オブジェクトの差分を適用するため、元の値が別の型なら空のオブジェクトへ置き換える。
    if (!target.is_object()) {
        target = json::object();
    }

    for (auto it = patch.begin(); it != patch.end(); ++it) {
        // nullが指定されたキーは、値の設定ではなく削除を行う。
        if (it.value().is_null()) {
            target.erase(it.key());
        // null以外は、対象キーの値へ同じ合成規則を再帰適用する。
        } else {
            ApplyJsonMergePatch(target[it.key()], it.value());
        }
    }
}

// 元Assetと編集後の値から、変更された項目だけの差分JSONを作ります。
json BuildJsonMergePatch(const json& base, const json& edited, bool& changed) {
    // Prefab元との差分だけを保存し、元データの更新をインスタンスへ引き継げるようにする。
    // 許容誤差を含めて同じ値なら、保存すべき上書きは無い。
    if (JsonValuesEqual(base, edited)) {
        changed = false;
        return json::object();
    }

    // 片方でもオブジェクトでなければ、編集後の値を差分として丸ごと返す。
    if (!base.is_object() || !edited.is_object()) {
        changed = true;
        return edited;
    }

    json patch = json::object();

    // 元にあって編集後にないキーはnullを削除命令として記録する。
    for (auto baseIt = base.begin(); baseIt != base.end(); ++baseIt) {
        // 元のキーが編集後に存在しなければ、値を消すためにnullを記録する。
        if (!edited.contains(baseIt.key())) {
            patch[baseIt.key()] = nullptr;
        }
    }

    for (auto editedIt = edited.begin(); editedIt != edited.end(); ++editedIt) {
        // オブジェクトの子要素だけ再帰的に比較し、値全体の複製を抑える。
        // 旧状態に存在しないキーは比較を省き、値全体を追加する。
        if (!base.contains(editedIt.key())) {
            patch[editedIt.key()] = editedIt.value();
            continue;
        }

        bool childChanged = false;
        json childPatch = BuildJsonMergePatch(base.at(editedIt.key()), editedIt.value(), childChanged);

        // 既存キーは、再帰比較で変更を検出した場合だけ記録する。
        if (childChanged) {
            patch[editedIt.key()] = std::move(childPatch);
        }
    }

    changed = !patch.empty();
    return patch;
}

// ドット区切りのプロパティを削除し、空になった親ノードも畳みます。
bool EraseJsonPath(json& source, const std::string& path) {
    // 子の差分を消した結果、空になった親オブジェクトも畳む。
    const std::size_t separator = path.find('.');

    // 区切りが残っていなければ、現在のオブジェクトから最後のキーを消す。
    if (separator == std::string::npos) {
        return source.is_object() && source.erase(path) > 0;
    }

    const std::string head = path.substr(0, separator);

    // 途中のオブジェクトや子キーが無ければ、削除できる項目が無い。
    if (!source.is_object() || !source.contains(head)) {
        return false;
    }

    const bool erased = EraseJsonPath(source[head], path.substr(separator + 1));

    // 再帰削除が成功して、その結果空になった親オブジェクトは畳む。
    if (erased && source.contains(head) && source[head].is_object() && source[head].empty()) {
        source.erase(head);
    }

    return erased;
}

// 旧形式のComponent存在フラグを現在のOverride形式へ移します。
void MigrateLegacyComponentOverrides(json& asset) {
    // 旧nodeOverrides内の存在フラグを新しいcomponentOverridesへ移し、差分の意味を保つ。
    EnsurePrefabVariantFields(asset);
    json& nodeOverrides = asset["nodeOverrides"];

    // 旧ノード差分の形式を扱えない場合は変換を続けない。
    if (!nodeOverrides.is_object()) {
        return;
    }

    EditorPropertyRegistry* registry = EditorPropertyRegistry::GetInstance();
    registry->InitializeBuiltInProperties();

    for (auto nodeIt = nodeOverrides.begin(); nodeIt != nodeOverrides.end();) {
        json& patch = nodeIt.value();

        // 部品差分がオブジェクトとして入っていないノードは読み飛ばす。
        if (!patch.is_object() || !patch.contains("components") || !patch["components"].is_object()) {
            ++nodeIt;
            continue;
        }

        json& serializedComponents = patch["components"];

        // 取り外し可能なComponentだけを対象にし、プロパティ差分は元のノードに残す。
        for (const EditorComponentDescriptor& component : registry->GetComponents()) {
            // 有無を変更できる登録済み部品が、旧差分にもオブジェクトとして存在する場合だけ移行する。
            if (!component.removable || !component.serializedPresent || !component.setSerializedPresent ||
                !serializedComponents.contains(component.typeId) ||
                !serializedComponents[component.typeId].is_object()) {
                continue;
            }

            json& componentPatch = serializedComponents[component.typeId];
            // 存在フラグだけを新形式へ移し、他のプロパティが残るパッチは保持する。
            if (componentPatch.contains("_editorPresent") &&
                componentPatch["_editorPresent"].is_boolean()) {
                asset["componentOverrides"][nodeIt.key()][component.typeId] =
                    componentPatch["_editorPresent"];
                componentPatch.erase("_editorPresent");
            }

            // 移行後に値の差分が残らない部品は、旧オブジェクトから取り除く。
            if (componentPatch.empty()) {
                serializedComponents.erase(component.typeId);
            }
        }

        // 残る部品差分が無ければ、空のcomponentsキーを消す。
        if (serializedComponents.empty()) {
            patch.erase("components");
        }

        // 元ノード側に何も残らなかった場合だけ、ノードの差分項目自体を削除する。
        if (patch.empty()) {
            nodeIt = nodeOverrides.erase(nodeIt);
        // 別の変更が残っているノードは保持して、次の項目へ進む。
        } else {
            ++nodeIt;
        }
    }
}

// 旧形式も現在のPrefab Asset構造へ包み直し、IDと差分フィールドを補います。
json NormalizePrefabAsset(const std::string& prefabName, const json& value) {
    // 旧形式のルート直書きを包み、以後の処理が単一のPrefab形式を読めるようにする。
    json asset;

    // rootがオブジェクトとして保存されていれば、既にラッパー付きの形式として扱う。
    if (value.is_object() && value.contains("root") && value["root"].is_object()) {
        asset = value;
    // 旧形式の直書きノードは、rootの値として包んでから整える。
    } else {
        asset["root"] = value;
    }

    asset["version"] = 3;
    std::string assetId = asset.value("assetId", "");

    // 保存済みのAsset IDが空の場合だけ、名前から再現可能なIDを生成する。
    if (assetId.empty()) {
        assetId = MakeStableId("prefab", prefabName);
        asset["assetId"] = assetId;
    }

    // 有効なルートオブジェクトが無ければ、空のルートを用意する。
    if (!asset.contains("root") || !asset["root"].is_object()) {
        asset["root"] = json::object();
    }

    // 継承元のIDが無いか型が異なれば、継承元なしに揃える。
    if (!asset.contains("baseAssetId") || !asset["baseAssetId"].is_string()) {
        asset["baseAssetId"] = "";
    }

    // 値の差分が定義されていなければ、空のプロパティ一覧を用意する。
    if (!asset.contains("propertyOverrides") || !asset["propertyOverrides"].is_object()) {
        asset["propertyOverrides"] = json::object();
    }

    EnsurePrefabVariantFields(asset);

    // 旧Component形式を変換してから、親子ノードの安定IDを補う。
    MigrateLegacyComponentOverrides(asset);
    EnsurePrefabObjectIds(asset["root"], assetId, "root");

    for (std::size_t index = 0; index < asset["addedChildren"].size(); ++index) {
        json& entry = asset["addedChildren"][index];

        // 追加した子の実体がJSONノードとして存在する場合だけ、追加位置を種にIDを補う。
        if (entry.is_object() && entry.contains("node") && entry["node"].is_object()) {
            EnsurePrefabObjectIds(entry["node"], assetId, "added/" + std::to_string(index));
        }
    }

    return asset;
}

// Preset/Prefabノードから実体と子孫を生成し、所有権をoutObjectsへ移します。
void CreateObjectFromPresetNode(
    const json& node,
    engine::Object3DCommon* common,
    engine::Object3D* parent,
    std::vector<std::unique_ptr<engine::Object3D>>& outObjects,
    const PrefabCreationContext* prefabContext) {
    // 有効な定義と3D共通資源が揃う場合にだけ個体を作る。
    if (!node.is_object() || !common) {
        return;
    }

    // EnemyとGimmickはFactoryで派生型を生成し、配置直後から固有の更新処理を有効にする。
    const std::string enemyType = ResolveEnemyType(node);
    std::unique_ptr<engine::Object3D> object;

    // 敵種別が解決できた場合は先に敵Factoryで派生型を生成する。
    if (!enemyType.empty()) {
        object = EnemyFactory::GetInstance()->CreateEnemy(enemyType, common);
    }

    // 固有の更新処理が必要なギミックは、名前を解決して専用Factoryへ渡す。
    std::string gimmickType = node.value("gimmickType", std::string{});
    // 直下にギミック種別が無い場合は、旧形式のparamも調べる。
    if (gimmickType.empty() && node.contains("param") && node["param"].is_object())
        gimmickType = node["param"].value("gimmickType", std::string{});
    // まだ個体がなく、Gimmickと明示され種別も確認できた場合にFactoryを使う。
    if (!object && node.value("type", std::string{}) == "Gimmick" && !gimmickType.empty())
        object = GimmickFactory::GetInstance()->CreateGimmick(gimmickType, common);

    // どちらのFactoryからも作れなければ、通常のObject3Dを初期化する。
    if (!object) {
        object = std::make_unique<engine::Object3D>();
        object->Initialize(common);
    }

    object->ImportFromJson(node);

    // Preset/Prefabの生成物は、Asset内データとは別のScene実体です。
    object->RegeneratePersistentGuid();

    // JSON読込後に敵種別とStatusを再適用し、生成した実体の戦闘値を揃えます。
    if (!enemyType.empty()) {
        object->SetClassName("Enemy");
        object->SetEnemyType(enemyType);

        // 取り込んだJSONに敵用のパラメーターがなければ、戦闘値を適用する前に初期化する。
        if (!object->GetParameters().has_value()) {
            object->ResetParameters();
        }

        object->SetParameter(&engine::EntityParameter::enemyType, enemyType);
        auto* statusManager = GameplayStatusManager::GetInstance();
        statusManager->Initialize();
        statusManager->ApplyEnemyStatus(object.get(), true);
    }

    // 明示された当たり判定と衝突属性を優先し、旧テンプレートの省略値だけ補完します。
    if (!node.contains("collider") && !node.contains("collisionAttribute") && !node.contains("collisionMask"))
        editorObjectDefaults::ApplyGroundCollision(object.get());
    // 保存名が文字列なら、用意されていればプリセット表示名を優先する。
    if (node.contains("name") && node["name"].is_string()) {
        object->SetName(node.value("presetDisplayName", node["name"].get<std::string>()));
    }

    // Prefab配置では元Asset内のObject IDとScene実体のInstance IDを分けて保持します。
    if (prefabContext) {
        engine::Object3D::PrefabInstanceInfo info;
        info.prefabName = prefabContext->prefabName;
        info.assetId = prefabContext->assetId;
        info.instanceId = prefabContext->instanceId;
        info.sourceObjectId = node.value("prefabObjectId", "");
        info.isRoot = parent == nullptr;
        object->SetPrefabInstanceInfo(info);
    }

    // 親子関係と行列を確定してから所有権を出力配列へ移し、子ノードを再帰生成します。
    engine::Object3D* raw = object.get();

    // 子として呼ばれた場合だけ、指定された実体を親に設定する。
    if (parent) {
        raw->SetParent(parent);
    }

    raw->UpdateLocalMatrix();
    raw->UpdateWorldMatrix();
    outObjects.push_back(std::move(object));

    // 子を持たない定義では、今の個体の登録を済ませて生成を終える。
    if (!node.contains("children") || !node["children"].is_array()) {
        return;
    }

    for (const auto& childNode : node["children"]) {
        CreateObjectFromPresetNode(childNode, common, raw, outObjects, prefabContext);
    }
}

json* FindSourceNodeRecursive(json& node, const std::string& sourceObjectId) {
    // Prefab内の固定IDを子までたどり、編集できるノードを返す。
    if (!node.is_object()) {
        return nullptr;
    }

    // 現在の固定IDが一致すれば、子へ進まず編集可能な参照を返す。
    if (node.value("prefabObjectId", "") == sourceObjectId) {
        return &node;
    }

    // 子の配列があるときだけ再帰探索する。
    if (node.contains("children") && node["children"].is_array()) {
        for (auto& child : node["children"]) {
            // 子孫で対象を見つけた時点で、残りの探索を打ち切る。
            if (json* result = FindSourceNodeRecursive(child, sourceObjectId)) {
                return result;
            }
        }
    }

    return nullptr;
}

const json* FindSourceNodeRecursive(const json& node, const std::string& sourceObjectId) {
    // 読み取り専用のPrefab階層でも同じ固定IDで該当ノードを探す。
    if (!node.is_object()) {
        return nullptr;
    }

    // 現在の固定IDが一致すれば、読み取り専用の参照を返す。
    if (node.value("prefabObjectId", "") == sourceObjectId) {
        return &node;
    }

    // 参照できる子の配列がある場合だけ、その中を検索する。
    if (node.contains("children") && node["children"].is_array()) {
        for (const auto& child : node["children"]) {
            // 子孫の一致を見つけたら、それ以降の枝は確認しない。
            if (const json* result = FindSourceNodeRecursive(child, sourceObjectId)) {
                return result;
            }
        }
    }

    return nullptr;
}

// Registryの情報を優先して、指定された編集プロパティの保存キーを返す。
const char* GetSerializedPropertyKey(const std::string& propertyPath) {
    EditorPropertyRegistry* registry = EditorPropertyRegistry::GetInstance();
    registry->InitializeBuiltInProperties();

    // Registryに項目が存在する場合は、登録された保存パスの有無を優先し旧定義へは進まない。
    if (const EditorPropertyDescriptor* descriptor = registry->Find(propertyPath)) {
        return descriptor->serializedPath.empty() ? nullptr : descriptor->serializedPath.c_str();
    }

    // 以下の互換表では、Registryに無い既知の編集パスに一致した場合だけ保存キーを返す。
    // カメラやゲーム設定も、各行の左の編集パスから右のJSONパスへ対応させる。
    if (propertyPath == "identity.name")
        return "name";
    if (propertyPath == "identity.tag")
        return "tag";
    if (propertyPath == "identity.layer")
        return "layer";
    if (propertyPath == "identity.saveCategory")
        return "saveCategory";
    if (propertyPath == "transform.position")
        return "translate";
    if (propertyPath == "transform.rotation")
        return "rotate";
    if (propertyPath == "transform.scale")
        return "scale";
    if (propertyPath == "rendering.visible")
        return "isVisible";
    if (propertyPath == "rendering.castShadow")
        return "castShadow";
    if (propertyPath == "rendering.cameraOcclusionMode")
        return "cameraOcclusionMode";
    if (propertyPath == "rendering.color")
        return "color";
    if (propertyPath == "rendering.metallic")
        return "metallic";
    if (propertyPath == "rendering.roughness")
        return "roughness";
    if (propertyPath == "rendering.emissive")
        return "emissive";
    if (propertyPath == "editor.locked")
        return "isLocked";
    if (propertyPath == "collision.attribute")
        return "collisionAttribute";
    if (propertyPath == "collision.mask")
        return "collisionMask";
    if (propertyPath == "rendering.model")
        return "modelName";
    if (propertyPath == "rendering.blendMode")
        return "blendMode";
    if (propertyPath == "rendering.materialType")
        return "materialType";
    if (propertyPath == "rendering.texture")
        return "texturePath";
    if (propertyPath == "rendering.textureTiling")
        return "textureTiling";
    if (propertyPath == "rendering.autoTextureTiling")
        return "autoTextureTiling";
    if (propertyPath == "rendering.lighting")
        return "enableLighting";
    if (propertyPath == "rendering.environmentMap")
        return "enableEnvMap";
    if (propertyPath == "rendering.environmentIntensity")
        return "envIntensity";
    if (propertyPath == "rendering.normalMapEnabled")
        return "enableNormalMap";
    if (propertyPath == "rendering.normalMap")
        return "normalMapPath";
    if (propertyPath == "rendering.ormMap")
        return "ormMapPath";
    if (propertyPath == "component.collision.type")
        return "collider.type";
    if (propertyPath == "component.collision.center")
        return "collider.center";
    if (propertyPath == "component.collision.size")
        return "collider.size";
    if (propertyPath == "component.collision.rotation")
        return "collider.rotation";
    if (propertyPath == "component.collision.static")
        return "isStatic";
    if (propertyPath == "component.particle.cpu")
        return "particleName";
    if (propertyPath == "component.particle.gpu")
        return "gpuParticleName";
    if (propertyPath == "component.meshEffect.primary")
        return "meshEffect1";
    if (propertyPath == "component.meshEffect.secondary")
        return "meshEffect2";
    if (propertyPath == "component.animation.name")
        return "animation.animName";
    if (propertyPath == "component.animation.loop")
        return "animation.isAnimLoop";
    if (propertyPath == "component.animation.controller")
        return "animation.animatorController";
    if (propertyPath == "component.path.name")
        return "recorder.recordPathName";
    if (propertyPath == "component.path.loop")
        return "recorder.isRecordLoop";
    if (propertyPath == "component.path.playbackMode")
        return "components.PathMover.playbackMode";
    if (propertyPath == "component.path.relative")
        return "recorder.isRecordRelative";
    if (propertyPath == "component.link.eventId")
        return "myEventID";
    if (propertyPath == "component.link.targetId")
        return "targetID";
    if (propertyPath == "camera.enabled")
        return "camera.enabled";
    if (propertyPath == "camera.role")
        return "camera.role";
    if (propertyPath == "camera.fov")
        return "camera.fovY";
    if (propertyPath == "camera.nearClip")
        return "camera.nearClip";
    if (propertyPath == "camera.farClip")
        return "camera.farClip";
    if (propertyPath == "camera.eyeSource")
        return "camera.eyeSource";
    if (propertyPath == "camera.eyeObject")
        return "camera.eyeObjectName";
    if (propertyPath == "camera.eyeOffset")
        return "camera.eyeOffset";
    if (propertyPath == "camera.eyeFollowMode")
        return "camera.eyeFollowMode";
    if (propertyPath == "camera.eyeFollowResponse")
        return "camera.eyeFollowResponse";
    if (propertyPath == "camera.targetMode")
        return "camera.targetMode";
    if (propertyPath == "camera.targetObject")
        return "camera.targetObjectName";
    if (propertyPath == "camera.targetOffset")
        return "camera.targetOffset";
    if (propertyPath == "camera.fixedTarget")
        return "camera.fixedTarget";
    if (propertyPath == "camera.forwardDistance")
        return "camera.forwardDistance";
    if (propertyPath == "camera.targetFollowMode")
        return "camera.targetFollowMode";
    if (propertyPath == "camera.targetFollowResponse")
        return "camera.targetFollowResponse";
    if (propertyPath == "camera.blendIn")
        return "camera.blendInDuration";
    if (propertyPath == "camera.blendOut")
        return "camera.blendOutDuration";
    if (propertyPath == "camera.easing")
        return "camera.easing";
    if (propertyPath == "gameplay.healAmount")
        return "param.healAmount";
    if (propertyPath == "gameplay.interval")
        return "param.interval";
    if (propertyPath == "gameplay.maxCount")
        return "param.maxCount";
    if (propertyPath == "gameplay.detectionRange")
        return "param.detectionRange";
    if (propertyPath == "gameplay.shakeDuration")
        return "param.shakeDuration";
    if (propertyPath == "gameplay.fallDuration")
        return "param.fallDuration";
    if (propertyPath == "gameplay.colorType")
        return "param.colorType";
    if (propertyPath == "gameplay.switchMode")
        return "param.switchMode";
    if (propertyPath == "gameplay.actionMode")
        return "param.actionMode";
    if (propertyPath == "gameplay.targetScene")
        return "param.targetScene";
    if (propertyPath == "gameplay.moveAmount")
        return "param.moveAmount";
    if (propertyPath == "gameplay.moveSpeed")
        return "param.moveSpeed";
    if (propertyPath == "gameplay.ringWaveSpeed")
        return "param.ringWaveSpeed";
    if (propertyPath == "gameplay.startActive")
        return "param.startActive";
    if (propertyPath == "gameplay.returnOnOff")
        return "param.returnOnOff";
    // Registryにも互換表にも無いキーは、保存パス未対応として返す。
    return nullptr;
}

const char* GetCanonicalComponentPropertyKey(const std::string& propertyPath) {
    // 以下の既知の編集パスに一致する場合だけ、現行部品の保存パスを返して旧キーと同期できるようにする。
    if (propertyPath == "component.particle.cpu")
        return "components.ParticleEmitter.cpuParticle";
    if (propertyPath == "component.particle.gpu")
        return "components.ParticleEmitter.gpuParticle";
    if (propertyPath == "component.meshEffect.primary")
        return "components.MeshEffect.primary";
    if (propertyPath == "component.meshEffect.secondary")
        return "components.MeshEffect.secondary";
    if (propertyPath == "component.path.name")
        return "components.PathMover.path";
    if (propertyPath == "component.path.loop")
        return "components.PathMover.loop";
    if (propertyPath == "component.path.playbackMode")
        return "components.PathMover.playbackMode";
    if (propertyPath == "component.path.relative")
        return "components.PathMover.relative";
    if (propertyPath == "component.link.eventId")
        return "components.GameplayLink.eventId";
    if (propertyPath == "component.link.targetId")
        return "components.GameplayLink.targetId";
    // 同期する現行部品のキーが定義されていない項目には、追加の書き込み先を返さない。
    return nullptr;
}

const json* FindJsonPath(const json& source, const std::string& path) {
    // ドット区切りのパスを段階的にたどり、途中でキーが欠けたら探索を止める。
    const json* current = &source;
    std::size_t begin = 0;

    while (begin < path.size()) {
        const std::size_t separator = path.find('.', begin);
        const std::string key = path.substr(begin, separator == std::string::npos ? std::string::npos : separator - begin);

        // 途中がオブジェクトではないかキーが無い場合は、参照先を得られない。
        if (!current->is_object() || !current->contains(key)) {
            return nullptr;
        }

        current = &current->at(key);

        // 区切りが尽きたら、移動した直後の値が指定された保存先になる。
        if (separator == std::string::npos) {
            return current;
        }

        begin = separator + 1;
    }

    return current;
}

json* EnsureJsonPath(json& source, const std::string& path) {
    // Prefab差分を保存するため、途中のObjectがない場合は生成して末端の値を返します。
    json* current = &source;
    std::size_t begin = 0;

    while (begin < path.size()) {
        const std::size_t separator = path.find('.', begin);
        const std::string key = path.substr(begin, separator == std::string::npos ? std::string::npos : separator - begin);

        // 最後のキーでは、値を書き込める要素への参照を返す。
        if (separator == std::string::npos) {
            return &(*current)[key];
        }

        json& next = (*current)[key];

        // 途中の要素がオブジェクトになっていなければ、子キーを保持できる形へ揃える。
        if (!next.is_object()) {
            next = json::object();
        }

        current = &next;
        begin = separator + 1;
    }

    return nullptr;
}

// 元Assetで省略されたプロパティをRegistryまたは旧形式の既定値で補います。
json GetMissingSourceDefault(const std::string& propertyPath) {
    EditorPropertyRegistry* registry = EditorPropertyRegistry::GetInstance();
    registry->InitializeBuiltInProperties();

    // Registryが項目を持つ場合は、そこに示された既定値を採用する。
    if (const EditorPropertyDescriptor* descriptor = registry->Find(propertyPath)) {
        return descriptor->defaultValue;
    }

    // Registry未登録の旧Propertyだけを従来値で扱います。
    if (propertyPath == "component.collision.static" ||
        propertyPath == "component.path.loop" ||
        propertyPath == "component.path.relative") {
        return false;
    }

    // 素材や動作の識別名が省略された場合、その素材や動作なしを表す空文字を返す。
    if (propertyPath == "component.particle.cpu" ||
        propertyPath == "component.particle.gpu" ||
        propertyPath == "component.meshEffect.primary" ||
        propertyPath == "component.meshEffect.secondary" ||
        propertyPath == "component.animation.name" ||
        propertyPath == "component.animation.controller" ||
        propertyPath == "component.path.name") {
        return "";
    }

    return json();
}

json ReadSourceProperty(const json& sourceNode, const std::string& propertyPath) {
    // Registryで決まる保存Pathを解決し、旧Assetで省略された値は型ごとの既定値に戻します。
    const char* key = GetSerializedPropertyKey(propertyPath);

    // 保存パスを解決できない項目は、値を取得できないためnullを返す。
    if (!key) {
        return json();
    }

    const json* value = FindJsonPath(sourceNode, key);
    return value ? *value : GetMissingSourceDefault(propertyPath);
}

// Registryの保存Pathへ値を書き、必要なら正規のComponentキーにも同期します。
bool WriteSourceProperty(json& sourceNode, const std::string& propertyPath, const json& value) {
    const char* key = GetSerializedPropertyKey(propertyPath);

    // 対応する保存キーが無ければAssetを変更しない。
    if (!key) {
        return false;
    }

    json* target = EnsureJsonPath(sourceNode, key);

    // パスの作成後も書き込み先が得られない場合は変更を適用できない。
    if (!target) {
        return false;
    }

    *target = value;

    // 旧キーと現行コンポーネントキーの両方がある項目は同じ値に揃え、読み込む形式による差を防ぐ。
    if (const char* canonicalKey = GetCanonicalComponentPropertyKey(propertyPath)) {
        // 現行の保存先を作成できれば、先に書いた互換キーと同じ値を設定する。
        if (json* canonicalTarget = EnsureJsonPath(sourceNode, canonicalKey)) {
            *canonicalTarget = value;
        }
    }

    if (propertyPath == "transform.rotation") {
        // Euler角のOverrideを適用した時は、古いQuaternionが優先されないようにします。
        sourceNode.erase("quaternion");
    }

    return true;
}

// Componentの有無をPrefabへ保存・復元できる定義か判定します。
bool IsPrefabComponentDescriptor(const EditorComponentDescriptor& component) {
    return component.removable && component.serializedPresent && component.setSerializedPresent;
}

// Componentを取り除く際に、そのComponentに属するプロパティ差分を消します。
// 部品の有無を戻す際に、その部品の項目だけを上書きから取り除き、同じノードの他の編集を残す。
void RemoveComponentPropertyOverrides(
    json& asset,
    const std::string& sourceObjectId,
    const std::string& componentTypeId) {
    // 指定ノードの差分オブジェクトを取得できない場合は、消すプロパティがない。
    if (!asset.contains("propertyOverrides") || !asset["propertyOverrides"].is_object() ||
        !asset["propertyOverrides"].contains(sourceObjectId) ||
        !asset["propertyOverrides"][sourceObjectId].is_object()) {
        return;
    }

    EditorPropertyRegistry* registry = EditorPropertyRegistry::GetInstance();
    json& objectOverrides = asset["propertyOverrides"][sourceObjectId];

    for (const EditorPropertyDescriptor* property : registry->GetPropertiesForComponent(componentTypeId)) {
        // 登録済みのプロパティが有効なら、該当部品の差分だけを消す。
        if (property) {
            objectOverrides.erase(property->path);
        }
    }

    // 対象ノードに他の差分が残らない場合は、空のノード項目も消す。
    if (objectOverrides.empty()) {
        asset["propertyOverrides"].erase(sourceObjectId);
    }
}

// Object上のComponentの有無と保存対象プロパティをPrefabノードへ反映します。
bool WriteComponentStateFromObject(
    json& sourceNode,
    engine::Object3D* object,
    const EditorComponentDescriptor& component,
    bool present) {
    // 実体か有無の保存を担当する処理が欠けていれば失敗を返す。
    if (!object || !component.setSerializedPresent) {
        return false;
    }

    EditorPropertyRegistry* registry = EditorPropertyRegistry::GetInstance();

    // 部品を持つ状態を保存する場合は、先に現在の値を保存する。
    if (present) {
        // 追加したComponentのPrefab上書き対象だけを保存し、実行時専用値を混ぜない。
        for (const EditorPropertyDescriptor* property : registry->GetPropertiesForComponent(component.typeId)) {
            // Prefabで上書きでき、保存パスが割り当てられたプロパティだけを書き出す。
            if (!property || !HasEditorPropertyFlag(property->flags, EditorPropertyFlags::kPrefabOverride) ||
                property->serializedPath.empty()) {
                continue;
            }

            const json value = registry->GetValue(object, property->path);

            // 値を取得できたプロパティだけ、Prefabの保存パスへ書く。
            if (!value.is_null()) {
                WriteSourceProperty(sourceNode, property->path, value);
            }
        }
    }

    // 値の書き込みを終えてから、部品が存在するかを保存する。
    component.setSerializedPresent(sourceNode, present);
    return true;
}

// 保存されたComponentの有無を先に合わせ、その後でプロパティ値を戻します。
bool ApplySerializedComponentState(
    engine::Object3D* object,
    const json& sourceNode,
    const EditorComponentDescriptor& component) {
    // 実体と保存された有無の照会処理が揃わない場合は適用を行えない。
    if (!object || !component.serializedPresent) {
        return false;
    }

    EditorPropertyRegistry* registry = EditorPropertyRegistry::GetInstance();
    const bool desiredPresent = component.serializedPresent(sourceNode);
    const bool currentPresent = registry->IsComponentPresent(object, component.typeId);

    // 値の適用より先にComponentの有無を揃え、存在しないComponentへの書き込みを避ける。
    if (desiredPresent != currentPresent) {
        const bool changed = desiredPresent
            ? registry->AddComponent(object, component.typeId)
            : registry->RemoveComponent(object, component.typeId);
        // 部品の追加または削除に失敗したら、残るプロパティは適用しない。
        if (!changed) {
            return false;
        }
    }

    // 最終状態に部品が存在する場合だけ、保存値を実体へ反映する。
    if (desiredPresent) {
        for (const EditorPropertyDescriptor* property : registry->GetPropertiesForComponent(component.typeId)) {
            // 保存先の無いプロパティや無効な定義は復元できないため省く。
            if (!property || property->serializedPath.empty()) {
                continue;
            }

            const json value = ReadSourceProperty(sourceNode, property->path);

            // 参照値か既定値がnull以外の場合だけ、実体の値を上書きする。
            if (!value.is_null()) {
                registry->SetValue(object, property->path, value);
            }
        }
    }

    return true;
}

// 登録済み部品の有無は個別に差分化するため、汎用のJSON差分に旧マーカーとバージョンを含めない。
void RemoveKnownComponentPresenceMarkers(json& node) {
    EditorPropertyRegistry* registry = EditorPropertyRegistry::GetInstance();

    for (const EditorComponentDescriptor& component : registry->GetComponents()) {
        // 有無を差分として扱える部品だけ、汎用差分から旧マーカーと形式番号を除く。
        if (IsPrefabComponentDescriptor(component)) {
            EraseJsonPath(node, "components." + component.typeId + "._editorPresent");
            EraseJsonPath(node, "components." + component.typeId + ".version");
        }
    }
}

// インスタンスの配置位置・回転としてPrefab元から独立させるプロパティか判定します。
bool IsInstancePlacementProperty(const engine::Object3D::PrefabInstanceInfo& info, const std::string& propertyPath) {
    return info.isRoot &&
        (propertyPath == "transform.position" || propertyPath == "transform.rotation");
}

// Prefab Assetの変更前後をEditor共通のUndo/Redo履歴へ登録します。
void RegisterPrefabAssetTransaction(
    PresetManager* manager,
    const std::string& prefabName,
    const json& before,
    const json& after,
    const std::string& label) {
    // 管理オブジェクトが無いか実際の差が無い場合は、履歴に項目を増やさない。
    if (!manager || before == after) {
        return;
    }

    EditorTransaction transaction;
    transaction.label = label;
    // 履歴から Asset の定義を差し戻した後、派生する Prefab を解決し直して保存する。
    transaction.undo = [manager, prefabName, before]() {
        manager->GetPrefab(prefabName) = before;
        manager->RefreshPrefabInheritance();
        manager->SavePrefabs();
    };

    transaction.redo = [manager, prefabName, after]() {
        manager->GetPrefab(prefabName) = after;
        manager->RefreshPrefabInheritance();
        manager->SavePrefabs();
    };

    // 前後のAsset状態を保持するUndoとRedoを、エディターの履歴へ渡す。
    EditorTransactionManager::GetInstance()->Register(std::move(transaction));
}
}
