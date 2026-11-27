#include "engine/editor/assets/preset/PresetManager.Internal.h"

#include <cstdint>
#include "BaseScene.h"

using namespace presetManagerImplementation;

// Variantの差分を基底Prefabへ適用し、継承後の階層を更新する。
void PresetManager::RefreshPrefabInheritance() {
    // 解決すべきPrefabがなければ継承の反復を開始しない。
    if (prefabs_.empty()) {
        return;
    }

    // Variantの親子順がMap順と一致しなくても解決できるよう、最大Asset数だけ反復します。
    for (std::size_t pass = 0; pass < prefabs_.size(); ++pass) {
        bool changed = false;

        for (auto& [name, asset] : prefabs_) {
            (void)name;
            const std::string baseAssetId = asset.value("baseAssetId", "");

            // 基底IDを持たないPrefabは自分の階層が確定しているため、Variantとして処理しない。
            if (baseAssetId.empty()) {
                continue;
            }

            auto baseIt = std::find_if(prefabs_.begin(), prefabs_.end(), [&baseAssetId](const auto& pair) {
                return pair.second.value("assetId", "") == baseAssetId;
            });
            // 基底アセットが見つかり、有効なルートを持つ場合にだけ差分を合成する。
            if (baseIt == prefabs_.end() || !baseIt->second.contains("root") ||
                !baseIt->second["root"].is_object()) {
                continue;
            }

            json resolvedRoot = baseIt->second["root"];

            // Variantの差分は削除、追加、親変更、値変更の順で適用する。
            // 親になる追加Objectを先に用意しないと、既存Objectをその下へ移せない。
            const json& removedObjectIds = asset["removedObjectIds"];

            // 削除IDの配列を持つVariantでは、基底ツリーから該当ノードを取り除く。
            if (removedObjectIds.is_array()) {
                for (const auto& sourceObjectId : removedObjectIds) {
                    // 文字列の要素だけを、削除するノードのIDとして使う。
                    if (sourceObjectId.is_string()) {
                        RemovePrefabNodeRecursive(resolvedRoot, sourceObjectId.get<std::string>());
                    }
                }
            }

            const json& addedChildren = asset["addedChildren"];

            // 追加差分が配列なら、保存された順序でツリーへ追加する準備をする。
            if (addedChildren.is_array()) {
                // 同じ親に対しては保存された挿入位置が手前の子から配置し、先に置いた子の位置が後の挿入でずれないようにする。
                std::vector<json> sortedAddedChildren(addedChildren.begin(), addedChildren.end());
                std::stable_sort(sortedAddedChildren.begin(), sortedAddedChildren.end(), [](const json& lhs, const json& rhs) {
                    const std::string lhsParent = lhs.value("parentObjectId", "");
                    const std::string rhsParent = rhs.value("parentObjectId", "");
                    // 親が異なる場合は親IDで区切り、同じ親に対してだけ子の位置を比較する。
                    if (lhsParent != rhsParent)
                        return lhsParent < rhsParent;
                    return lhs.value("order", static_cast<std::size_t>(0)) < rhs.value("order", static_cast<std::size_t>(0));
                });

                for (auto& entry : sortedAddedChildren) {
                    // ノードの実体がない追加項目や形式の違う項目は挿入できないため読み飛ばす。
                    if (!entry.is_object() || !entry.contains("node") || !entry["node"].is_object())
                        continue;
                    InsertPrefabChild(
                        resolvedRoot,
                        entry.value("parentObjectId", ""),
                        entry.value("order", static_cast<std::size_t>(0)),
                        entry["node"]);
                }
            }

            // 担当：Prefab差分で後から適用する親変更と子の並び順。
            // 関連：PresetManagerの内部処理だけで使う補助型。
            struct PendingReparent {
                std::string sourceObjectId;
                std::string parentObjectId;
                std::size_t order = 0;
                json node;
            };

            std::vector<PendingReparent> pendingReparents;
            const json& reparentedObjects = asset["reparentedObjects"];

            // 親変更の配列がある場合は、移動可能な元ノードをいったん元の親から外す。
            if (reparentedObjects.is_array()) {
                for (const auto& entry : reparentedObjects) {
                    // 移動元や移動先を参照できない形式の項目は無視する。
                    if (!entry.is_object())
                        continue;
                    PendingReparent pending;
                    pending.sourceObjectId = entry.value("sourceObjectId", "");
                    pending.parentObjectId = entry.value("parentObjectId", "");
                    pending.order = entry.value("order", static_cast<std::size_t>(0));
                    // 両方のIDがあり、新しい親が存在し、元ノードを外せた場合だけ挿入待ちに入れる。
                    if (!pending.sourceObjectId.empty() && !pending.parentObjectId.empty() &&
                        FindSourceNodeRecursive(resolvedRoot, pending.parentObjectId) &&
                        TakePrefabNodeRecursive(resolvedRoot, pending.sourceObjectId, pending.node)) {
                        pendingReparents.push_back(std::move(pending));
                    }
                }
            }

            // 移動対象を元の位置からすべて外した後、新しい親と並び順で挿入する。
            std::stable_sort(pendingReparents.begin(), pendingReparents.end(), [](const auto& lhs, const auto& rhs) {
                // 親IDが異なるときは親でまとめ、共通の親の中だけで並び順を比較する。
                if (lhs.parentObjectId != rhs.parentObjectId)
                    return lhs.parentObjectId < rhs.parentObjectId;
                return lhs.order < rhs.order;
            });

            for (auto& pending : pendingReparents) {
                InsertPrefabChild(resolvedRoot, pending.parentObjectId, pending.order, std::move(pending.node));
            }

            const json& nodeOverrides = asset["nodeOverrides"];

            // Registry管理外の生JSON差分を戻してから、既知Propertyを型付きの経路で上書きする。
            if (nodeOverrides.is_object()) {
                for (auto nodeIt = nodeOverrides.begin(); nodeIt != nodeOverrides.end(); ++nodeIt) {
                    json* node = FindSourceNodeRecursive(resolvedRoot, nodeIt.key());

                    // 対象ノードが残っていて差分がオブジェクトの場合だけ、JSONのマージ差分を適用する。
                    if (node && nodeIt.value().is_object()) {
                        ApplyJsonMergePatch(*node, nodeIt.value());
                    }
                }
            }

            const json& overrides = asset["propertyOverrides"];

            // プロパティ差分をノードIDと項目名でたどれる場合に、既知項目を書き戻す。
            if (overrides.is_object()) {
                for (auto objectIt = overrides.begin(); objectIt != overrides.end(); ++objectIt) {
                    json* node = FindSourceNodeRecursive(resolvedRoot, objectIt.key());

                    // 対象のノードがないか、項目一覧として読めない差分は適用しない。
                    if (!node || !objectIt.value().is_object()) {
                        continue;
                    }

                    for (auto propertyIt = objectIt.value().begin(); propertyIt != objectIt.value().end(); ++propertyIt) {
                        WriteSourceProperty(*node, propertyIt.key(), propertyIt.value());
                    }
                }
            }

            // Componentの追加・削除はProperty値とは別の構造差分として最後に反映します。
            const json& componentOverrides = asset["componentOverrides"];

            // コンポーネントの差分マップがあれば、登録済みの型を使って有無を反映する。
            if (componentOverrides.is_object()) {
                EditorPropertyRegistry* registry = EditorPropertyRegistry::GetInstance();
                registry->InitializeBuiltInProperties();

                for (auto objectIt = componentOverrides.begin(); objectIt != componentOverrides.end(); ++objectIt) {
                    json* node = FindSourceNodeRecursive(resolvedRoot, objectIt.key());

                    // 対象のノードとコンポーネント一覧の両方が揃う場合にだけ適用する。
                    if (!node || !objectIt.value().is_object()) {
                        continue;
                    }

                    for (auto componentIt = objectIt.value().begin();
                        componentIt != objectIt.value().end(); ++componentIt) {
                        // 真偽値だけを追加・削除の指示として受け付ける。
                        if (componentIt.value().is_boolean()) {
                            registry->SetComponentPresent(
                                *node,
                                componentIt.key(),
                                componentIt.value().get<bool>());
                        }
                    }
                }
            }

            // 合成後の階層が変わった場合は、さらにその下のVariantへ伝播できるよう反復を続ける。
            if (!asset.contains("root") || asset["root"] != resolvedRoot) {
                asset["root"] = std::move(resolvedRoot);
                changed = true;
            }

            asset["version"] = 3;
        }

        // 一周してルートがどれも変化しなければ、継承が安定したので反復を終える。
        if (!changed) {
            break;
        }
    }
}

void PresetManager::AddPrefabFromObject(const std::string& prefabName, engine::Object3D* obj) {
    // 同名Assetがあれば新規IDを発行せず更新し、既存Instanceのリンクを保つ。
    // 元オブジェクトか保存名がなければアセットを作れないため戻る。
    if (!obj || prefabName.empty()) {
        return;
    }

    auto current = prefabs_.find(prefabName);

    // 保存名が使用中の場合はUpdatePrefabFromObject()へ渡して既存IDを保持する。
    if (current != prefabs_.end()) {
        UpdatePrefabFromObject(prefabName, obj);
        return;
    }

    std::string assetId;

    // 新規登録で未割当のIDを発行する。
    if (assetId.empty()) {
        assetId = MakeUniqueId("prefab");
    }

    // シーン階層から新しい基底Prefabのルートを作り、台帳と保存ファイルへ登録する。
    std::unordered_set<const engine::Object3D*> visited;
    json asset;
    asset["version"] = 3;
    asset["assetId"] = assetId;
    asset["baseAssetId"] = "";
    asset["propertyOverrides"] = json::object();
    EnsurePrefabVariantFields(asset);
    asset["root"] = BuildPrefabNode(obj, assetId, visited);
    prefabs_[prefabName] = std::move(asset);
    SavePrefabs();
}

// 既存IDのまま編集中の階層を取り込み、Variantなら基底との差を保存する。
bool PresetManager::UpdatePrefabFromObject(const std::string& prefabName, engine::Object3D* obj) {
    auto assetIt = prefabs_.find(prefabName);

    // 更新先が未登録か、取り込み元がなければ保存を開始しない。
    if (assetIt == prefabs_.end() || !obj) {
        return false;
    }

    const json beforeAsset = assetIt->second;
    const std::string assetId = assetIt->second.value("assetId", "");

    // 親やInstanceが参照するアセットIDを確定できなければ更新しない。
    if (assetId.empty()) {
        return false;
    }

    std::unordered_set<const engine::Object3D*> visited;
    json editedRoot = BuildPrefabNode(obj, assetId, visited);

    // 編集中のオブジェクトから有効なルートを構築できなければ処理を中止する。
    if (!editedRoot.is_object()) {
        return false;
    }

    EnsurePrefabObjectIds(editedRoot, assetId, "root");
    EnsurePrefabVariantFields(assetIt->second);

    const std::string baseAssetId = assetIt->second.value("baseAssetId", "");

    if (baseAssetId.empty()) {
        // 親のないPrefabは編集済みツリーをそのまま正とし、Variant用差分を空に戻す。
        assetIt->second["root"] = std::move(editedRoot);
        assetIt->second["propertyOverrides"] = json::object();
        assetIt->second["removedObjectIds"] = json::array();
        assetIt->second["addedChildren"] = json::array();
        assetIt->second["reparentedObjects"] = json::array();
        assetIt->second["nodeOverrides"] = json::object();
        assetIt->second["componentOverrides"] = json::object();
    // Variantの場合は基底ルートを探し、階層と値の変更を差分として抽出する。
    } else {
        auto baseIt = std::find_if(prefabs_.begin(), prefabs_.end(), [&baseAssetId](const auto& pair) {
            return pair.second.value("assetId", "") == baseAssetId;
        });
        // 比較できる基底ルートがなければVariant差分を生成できないため失敗を返す。
        if (baseIt == prefabs_.end() || !baseIt->second.contains("root") ||
            !baseIt->second["root"].is_object()) {
            return false;
        }

        const json& baseRoot = baseIt->second["root"];
        std::unordered_map<std::string, PrefabNodeLocation> baseLocations;
        std::unordered_map<std::string, PrefabNodeLocation> editedLocations;
        CollectPrefabNodeLocations(baseRoot, "", 0, baseLocations);
        CollectPrefabNodeLocations(editedRoot, "", 0, editedLocations);

        // sourceObjectIdを軸に、削除・追加・移動と値の差分を別々に作る。
        json removedObjectIds = json::array();

        for (const auto& [sourceObjectId, location] : baseLocations) {
            (void)location;

            // 基底のIDが編集後ツリーに見つからない場合は削除差分へ入れる。
            if (editedLocations.find(sourceObjectId) == editedLocations.end()) {
                removedObjectIds.push_back(sourceObjectId);
            }
        }

        json addedChildren = json::array();
        std::unordered_set<std::string> baseObjectIds;

        for (const auto& [sourceObjectId, location] : baseLocations) {
            (void)location;
            baseObjectIds.insert(sourceObjectId);
        }

        for (const auto& [sourceObjectId, location] : editedLocations) {
            // 基底に存在するIDや保存できるノードを持たない項目は新規追加の対象外とする。
            if (baseLocations.find(sourceObjectId) != baseLocations.end() || !location.node) {
                continue;
            }

            // 追加Objectの子も追加Objectなら、最上位だけを保存して重複生成を防ぎます。
            if (editedLocations.find(location.parentObjectId) != editedLocations.end() &&
                baseLocations.find(location.parentObjectId) == baseLocations.end()) {
                continue;
            }

            // 親のないノードは子の挿入差分として保存できないため除く。
            if (location.parentObjectId.empty()) {
                continue;
            }

            json addedNode = *location.node;
            RemovePrefabChildrenWithKnownIds(addedNode, baseObjectIds);
            addedChildren.push_back({
                { "parentObjectId", location.parentObjectId },
                { "order", location.order },
                { "node", std::move(addedNode) },
            });
        }

        json reparentedObjects = json::array();
        // 既存のルートは移動対象から外し、配下の親変更と兄弟順の変更だけを記録する。
        const std::string rootId = baseRoot.value("prefabObjectId", "");

        for (const auto& [sourceObjectId, editedLocation] : editedLocations) {
            auto baseLocationIt = baseLocations.find(sourceObjectId);

            // 基底に存在しないノードと元のルートは親変更として記録しない。
            if (baseLocationIt == baseLocations.end() || sourceObjectId == rootId) {
                continue;
            }

            const PrefabNodeLocation& baseLocation = baseLocationIt->second;
            // 親IDか兄弟順が変化していれば、変更後の親と順序を記録する。
            if (editedLocation.parentObjectId != baseLocation.parentObjectId ||
                editedLocation.order != baseLocation.order) {
                reparentedObjects.push_back({
                    { "sourceObjectId", sourceObjectId },
                    { "parentObjectId", editedLocation.parentObjectId },
                    { "order", editedLocation.order },
                });
            }
        }

        json propertyOverrides = json::object();
        json nodeOverrides = json::object();
        json componentOverrides = json::object();
        EditorPropertyRegistry* registry = EditorPropertyRegistry::GetInstance();
        registry->InitializeBuiltInProperties();

        for (const auto& [sourceObjectId, editedLocation] : editedLocations) {
            auto baseLocationIt = baseLocations.find(sourceObjectId);

            // 更新前後の有効なノードが両側にある場合だけ同じIDの内容を比較する。
            if (baseLocationIt == baseLocations.end() || !editedLocation.node || !baseLocationIt->second.node) {
                continue;
            }

            const json& baseNode = *baseLocationIt->second.node;
            const json& editedNode = *editedLocation.node;

            // コンポーネントの有無を先に比較し、存在差分を値の変更と区別して保存する。
            for (const EditorComponentDescriptor& component : registry->GetComponents()) {
                // Prefab差分に対応するコンポーネントの種類だけ有無を調べる。
                if (!IsPrefabComponentDescriptor(component)) {
                    continue;
                }

                const bool basePresent = registry->IsComponentPresent(baseNode, component.typeId);
                const bool editedPresent = registry->IsComponentPresent(editedNode, component.typeId);

                // 基底から追加または削除された種類の変更後の有無を記録する。
                if (basePresent != editedPresent) {
                    componentOverrides[sourceObjectId][component.typeId] = editedPresent;
                }
            }

            for (const EditorPropertyDescriptor& descriptor : registry->GetProperties()) {
                // Prefabで上書きでき、保存キーが既知の項目だけ型付きの差分にする。
                if (!HasEditorPropertyFlag(descriptor.flags, EditorPropertyFlags::kPrefabOverride) ||
                    !GetSerializedPropertyKey(descriptor.path)) {
                    continue;
                }

                const json baseValue = ReadSourceProperty(baseNode, descriptor.path);
                const json editedValue = ReadSourceProperty(editedNode, descriptor.path);

                // 編集後の値があり、基底の値と異なる場合だけプロパティ差分に保存する。
                if (!editedValue.is_null() && !JsonValuesEqual(baseValue, editedValue)) {
                    propertyOverrides[sourceObjectId][descriptor.path] = editedValue;
                }
            }

            json baseRaw = baseNode;
            json editedRaw = editedNode;

            for (json* raw : { &baseRaw, &editedRaw }) {
                // 親子や既知Componentは専用の差分へ出したので、残りだけJSON Merge Patchにする。
                raw->erase("children");
                raw->erase("prefabObjectId");
                raw->erase("prefabInstance");
                raw->erase("quaternion");
                RemoveKnownComponentPresenceMarkers(*raw);

                for (const EditorPropertyDescriptor& descriptor : registry->GetProperties()) {
                    const char* serializedPath = GetSerializedPropertyKey(descriptor.path);

                    // 旧形式を含む保存パスがある既知項目は、生JSONの差分から除く。
                    if (serializedPath) {
                        EraseJsonPath(*raw, serializedPath);
                    }

                    // コンポーネント側の標準パスがある場合は、そちらも生JSONの差分から除く。
                    if (const char* canonicalPath = GetCanonicalComponentPropertyKey(descriptor.path)) {
                        EraseJsonPath(*raw, canonicalPath);
                    }
                }
            }

            bool rawChanged = false;
            json rawPatch = BuildJsonMergePatch(baseRaw, editedRaw, rawChanged);

            // 専用の差分へ移したデータを除いても変化が残る場合だけマージ差分を保存する。
            if (rawChanged) {
                nodeOverrides[sourceObjectId] = std::move(rawPatch);
            }
        }

        assetIt->second["removedObjectIds"] = std::move(removedObjectIds);
        assetIt->second["addedChildren"] = std::move(addedChildren);
        assetIt->second["reparentedObjects"] = std::move(reparentedObjects);
        assetIt->second["propertyOverrides"] = std::move(propertyOverrides);
        assetIt->second["nodeOverrides"] = std::move(nodeOverrides);
        assetIt->second["componentOverrides"] = std::move(componentOverrides);

        // RootはRefreshPrefabInheritanceで必ず基底＋差分から再構築します。
        assetIt->second["root"] = baseRoot;
    }

    // 継承後のルートを再構築した状態を保存し、更新前後の記録を編集履歴へ渡す。
    RefreshPrefabInheritance();
    const json afterAsset = assetIt->second;
    SavePrefabs();
    RegisterPrefabAssetTransaction(this, prefabName, beforeAsset, afterAsset, "Save Prefab Mode");
    return true;
}

// 配置済みInstanceにアセット更新を同期する。戻り値は対象や生成、削除の件数を合計した値。
std::int32_t PresetManager::SynchronizePrefabInstances(
    const std::map<std::string, json>& beforePrefabs,
    BaseScene& scene,
    engine::Object3DCommon* common) {
    const auto& sceneObjects = scene.GetObjects();

    // 生成に必要な共通資源、比較元、シーンの配置物のいずれかが欠けていれば同期対象はない。
    if (!common || beforePrefabs.empty() || sceneObjects.empty()) {
        return 0;
    }

    auto findAssetById = [](const auto& assets, const std::string& assetId) -> const json* {
        for (const auto& [name, asset] : assets) {
            (void)name;

            // 永続IDが一致したアセットのデータを参照として返す。
            if (asset.value("assetId", "") == assetId) {
                return &asset;
            }
        }

        return nullptr;
    };

    auto findAssetNameById = [](const auto& assets, const std::string& assetId) -> std::string {
        for (const auto& [name, asset] : assets) {
            // 永続IDが一致したアセットの現在の登録名を返す。
            if (asset.value("assetId", "") == assetId) {
                return name;
            }
        }

        return "";
    };

    std::unordered_map<std::string, std::vector<engine::Object3D*>> instances;

    // 同じPrefab配置に属するObjectをInstance IDでまとめ、他の配置へ波及させない。
    for (const auto& object : sceneObjects) {
        // 空の枠とPrefabに属さないオブジェクトはInstanceのグループから除外する。
        if (!object || !object->IsPrefabInstance())
            continue;
        const auto& info = object->GetPrefabInstanceInfo();

        // Instance IDがあるオブジェクトだけを同じ配置の単位でまとめる。
        if (!info.instanceId.empty()) {
            instances[info.instanceId].push_back(object.get());
        }
    }

    EditorPropertyRegistry* registry = EditorPropertyRegistry::GetInstance();
    // 同期対象の既存ノード、生成した追加ノード、削除対象ノードの数を数える。設定項目単位では加算しない。
    std::int32_t synchronizedObjects = 0;

    for (auto& [instanceId, objects] : instances) {
        // グループの先頭からアセット情報を読めない場合はその配置を飛ばす。
        if (objects.empty() || !objects.front())
            continue;
        const auto rootInfo = objects.front()->GetPrefabInstanceInfo();
        // アセット名が変わっても同じPrefabとして扱えるよう、更新前後の両方を永続IDで探す。
        const json* oldAsset = findAssetById(beforePrefabs, rootInfo.assetId);
        const json* newAsset = findAssetById(prefabs_, rootInfo.assetId);
        // 比較できるルートが前後に揃い内容も変化している場合だけ、その配置を同期する。
        if (!oldAsset || !newAsset || !oldAsset->contains("root") || !newAsset->contains("root") ||
            !(*oldAsset)["root"].is_object() || !(*newAsset)["root"].is_object() ||
            (*oldAsset)["root"] == (*newAsset)["root"]) {
            continue;
        }

        const json& oldRoot = (*oldAsset)["root"];
        const json& newRoot = (*newAsset)["root"];

        // 更新前後のsourceObjectIdと親を比較し、Sceneのローカル編集を保持できる範囲だけ同期する。
        std::unordered_map<std::string, PrefabNodeLocation> oldLocations;
        std::unordered_map<std::string, PrefabNodeLocation> newLocations;
        CollectPrefabNodeLocations(oldRoot, "", 0, oldLocations);
        CollectPrefabNodeLocations(newRoot, "", 0, newLocations);

        std::unordered_map<std::string, engine::Object3D*> instanceObjects;

        for (engine::Object3D* object : objects) {
            // 有効な配置物を永続ノードIDで直接引けるようにする。
            if (object) {
                instanceObjects[object->GetPrefabInstanceInfo().sourceObjectId] = object;
            }
        }

        // Sourceと同じ親を保つObjectだけ移動し、Scene固有の親変更は保持する。
        const auto syncChangedParents = [&] {
            for (auto& [sourceObjectId, object] : instanceObjects) {
                auto oldIt = oldLocations.find(sourceObjectId);
                auto newIt = newLocations.find(sourceObjectId);
                // 両版で共通し、親IDが変わった既存ノードだけを移動候補にする。
                if (!object || oldIt == oldLocations.end() || newIt == newLocations.end() ||
                    oldIt->second.parentObjectId == newIt->second.parentObjectId) {
                    continue;
                }

                std::string currentParentSourceId;
                // 現在の親が同じInstance内にいる場合だけ、その親を比較用のノードIDに変換する。
                if (engine::Object3D* currentParent = object->GetParent(); currentParent && currentParent->IsPrefabInstance() &&
                    currentParent->GetPrefabInstanceInfo().instanceId == instanceId) {
                    currentParentSourceId = currentParent->GetPrefabInstanceInfo().sourceObjectId;
                }

                auto newParentIt = instanceObjects.find(newIt->second.parentObjectId);

                // シーン側の親が更新前の基底と一致し、新しい親が配置済みの場合だけ移動する。
                if (currentParentSourceId == oldIt->second.parentObjectId && newParentIt != instanceObjects.end()) {
                    // 親を変更するときはローカル姿勢を保ち、新しい親に従って配置を移す。
                    object->SetParent(newParentIt->second, false);
                }
            }
        };
        syncChangedParents();

        // InstanceがSourceと同じ値を保っているPropertyだけ更新し、ローカルOverrideは残します。
        for (auto& [sourceObjectId, object] : instanceObjects) {
            auto oldIt = oldLocations.find(sourceObjectId);
            auto newIt = newLocations.find(sourceObjectId);
            // 前後のノードと配置先が全部残っている場合に、プロパティを比較する。
            if (!object || oldIt == oldLocations.end() || newIt == newLocations.end() ||
                !oldIt->second.node || !newIt->second.node) {
                continue;
            }

            for (const EditorPropertyDescriptor& descriptor : registry->GetProperties()) {
                // 上書き対象外の項目と、配置ごとの名前や姿勢として保護する項目は同期しない。
                if (!HasEditorPropertyFlag(descriptor.flags, EditorPropertyFlags::kPrefabOverride) ||
                    descriptor.path == "identity.name" ||
                    IsInstancePlacementProperty(object->GetPrefabInstanceInfo(), descriptor.path)) {
                    continue;
                }

                // 最新のSourceと比べる前に、Instanceが更新前のSource値から変更されていないことを確かめる。
                const json oldValue = ReadSourceProperty(*oldIt->second.node, descriptor.path);
                const json newValue = ReadSourceProperty(*newIt->second.node, descriptor.path);
                const json instanceValue = registry->GetValue(object, descriptor.path);
                // 三者の値を取得でき、配置にローカル変更がなく、配布元だけが変わった値を反映する。
                if (!oldValue.is_null() && !newValue.is_null() && !instanceValue.is_null() &&
                    JsonValuesEqual(instanceValue, oldValue) && !JsonValuesEqual(oldValue, newValue)) {
                    registry->SetValue(object, descriptor.path, newValue);
                }
            }

            ++synchronizedObjects;
        }

        // 新しく追加されたSource階層は、各Instanceにも同じInstance IDで生成します。
        std::vector<std::string> addedRootIds;

        for (const auto& [sourceObjectId, location] : newLocations) {
            // 旧アセットに存在するかすでに配置済みのIDは、新規の生成対象から外す。
            if (oldLocations.find(sourceObjectId) != oldLocations.end() ||
                instanceObjects.find(sourceObjectId) != instanceObjects.end()) {
                continue;
            }

            // 追加された親の配下は親を生成する際にまとめて作られる。子を個別の生成候補へ入れない。
            if (newLocations.find(location.parentObjectId) != newLocations.end() &&
                oldLocations.find(location.parentObjectId) == oldLocations.end()) {
                continue;
            }

            addedRootIds.push_back(sourceObjectId);
        }

        for (const std::string& sourceObjectId : addedRootIds) {
            const auto locationIt = newLocations.find(sourceObjectId);
            // 新規候補からノード内容を引けない場合は生成できないため飛ばす。
            if (locationIt == newLocations.end() || !locationIt->second.node)
                continue;
            auto parentIt = instanceObjects.find(locationIt->second.parentObjectId);
            // アタッチ先の親がそのInstanceにいなければ生成しない。
            if (parentIt == instanceObjects.end())
                continue;

            PrefabCreationContext context;
            context.prefabName = findAssetNameById(prefabs_, rootInfo.assetId);
            context.assetId = rootInfo.assetId;
            context.instanceId = instanceId;
            std::vector<std::unique_ptr<engine::Object3D>> created;
            CreateObjectFromPresetNode(*locationIt->second.node, common, parentIt->second, created, &context);

            for (auto& createdObject : created) {
                // 生成できなかった枠は登録件数に入れず、生成した実体だけシーンへ移す。
                if (!createdObject)
                    continue;
                instanceObjects[createdObject->GetPrefabInstanceInfo().sourceObjectId] = createdObject.get();
                scene.AddObjectImmediate(std::move(createdObject));
                ++synchronizedObjects;
            }
        }

        // 追加Objectの生成後にもう一度親同期を行い、追加Object配下への移動を反映します。
        syncChangedParents();

        // アセットから消えた配置物を外す前に、同時に消さない子をシーン直下へ退避する。
        std::unordered_set<engine::Object3D*> removedObjects;

        for (const auto& [sourceObjectId, object] : instanceObjects) {
            // 旧アセットに存在し新アセットから消えたIDだけを、対応する配置物の削除対象にする。
            if (object && oldLocations.find(sourceObjectId) != oldLocations.end() &&
                newLocations.find(sourceObjectId) == newLocations.end()) {
                removedObjects.insert(object);
            }
        }

        // 削除対象がある場合だけ、残す子の退避と削除対象の取り外しを行う。
        if (!removedObjects.empty()) {
            for (const auto& candidate : sceneObjects) {
                // 無効な枠と同じく削除する子は、シーン側に残す対象から外す。
                if (!candidate || removedObjects.find(candidate.get()) != removedObjects.end())
                    continue;
                if (candidate->GetParent() && removedObjects.find(candidate->GetParent()) != removedObjects.end()) {
                    // 親だけが削除される場合は、残す子をワールド姿勢を保ったままScene Rootへ移す。
                    candidate->SetParent(nullptr, true);
                }
            }

            // 削除対象を所有する一時配列へまとめ、取り外しの途中で別の削除対象の実体を失わないようにする。
            std::vector<std::unique_ptr<engine::Object3D>> detachedObjects;
            detachedObjects.reserve(removedObjects.size());

            for (engine::Object3D* removed : removedObjects) {
                // 取り外しに成功した実体は一時配列に移し、残りの削除対象を外すまで保持する。
                if (auto detached = scene.TakeObjectImmediate(removed)) {
                    detachedObjects.push_back(std::move(detached));
                }
            }

            synchronizedObjects += static_cast<std::int32_t>(removedObjects.size());
        }
    }

    return synchronizedObjects;
}
