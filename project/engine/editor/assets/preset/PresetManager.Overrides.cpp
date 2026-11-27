#include <cstdint>
#include "engine/editor/assets/preset/PresetManager.Internal.h"

using namespace presetManagerImplementation;

std::vector<PresetManager::PrefabPropertyOverride> PresetManager::GetPrefabOverrides(const engine::Object3D* object) const {
    // 配置位置と名前は個体固有として除き、元ノードと異なる編集可能 Property だけ拾う。
    std::vector<PrefabPropertyOverride> overrides;

    // リンクがないオブジェクトには比較対象がないため、空の差分を返す。
    if (!object || !object->IsPrefabInstance()) {
        return overrides;
    }

    const auto& info = object->GetPrefabInstanceInfo();
    const json* sourceNode = FindPrefabSourceNode(info);

    // 永続リンクから元のノードを解決できなければ、値を比較しない。
    if (!sourceNode) {
        return overrides;
    }

    EditorPropertyRegistry* registry = EditorPropertyRegistry::GetInstance();
    registry->InitializeBuiltInProperties();

    for (const EditorPropertyDescriptor& property : registry->GetProperties()) {
        // 差分の対象外と登録された項目、および配置固有の名前と姿勢を飛ばす。
        if (!HasEditorPropertyFlag(property.flags, EditorPropertyFlags::kPrefabOverride) ||
            property.path == "identity.name" || IsInstancePlacementProperty(info, property.path)) {
            continue;
        }

        const EditorComponentDescriptor* component = registry->FindComponent(property.componentTypeId);

        // 構造差分を扱う種類なら、元と配置の両方にそのコンポーネントがあるか先に確認する。
        if (component && IsPrefabComponentDescriptor(*component)) {
            const bool sourcePresent = registry->IsComponentPresent(*sourceNode, component->typeId);
            const bool instancePresent = registry->IsComponentPresent(object, component->typeId);

            // Component追加・削除中は、内部Propertyではなく構造差分として一括処理します。
            if (!sourcePresent || !instancePresent) {
                continue;
            }
        }

        const json sourceValue = ReadSourceProperty(*sourceNode, property.path);
        const json instanceValue = registry->GetValue(object, property.path);

        // どちらかの値が欠ける場合と、有効値が一致する場合は値の上書きとして表示しない。
        if (sourceValue.is_null() || instanceValue.is_null() || JsonValuesEqual(sourceValue, instanceValue)) {
            continue;
        }

        overrides.push_back({ property.path, property.displayName, sourceValue, instanceValue });
    }

    return overrides;
}

std::vector<PresetManager::PrefabComponentOverride> PresetManager::GetPrefabComponentOverrides(
    const engine::Object3D* object) const {
    // Component の有無は内部 Property とは別の構造差分として返す。
    std::vector<PrefabComponentOverride> overrides;

    // 比較する配置にPrefabのリンクがなければ、構造の差分を返さない。
    if (!object || !object->IsPrefabInstance()) {
        return overrides;
    }

    const json* sourceNode = FindPrefabSourceNode(object->GetPrefabInstanceInfo());

    // 比較元ノードを解決できない配置は、コンポーネントの有無を比較できない。
    if (!sourceNode) {
        return overrides;
    }

    EditorPropertyRegistry* registry = EditorPropertyRegistry::GetInstance();
    registry->InitializeBuiltInProperties();

    for (const EditorComponentDescriptor& component : registry->GetComponents()) {
        // Prefab側で有無を扱い、この配置にも適用可能な種類を調べる。
        if (!IsPrefabComponentDescriptor(component) ||
            !registry->IsComponentApplicable(object, component.typeId)) {
            continue;
        }

        const bool sourcePresent = registry->IsComponentPresent(*sourceNode, component.typeId);
        const bool instancePresent = registry->IsComponentPresent(object, component.typeId);

        // 元と配置で有無が違う場合だけ、置換の向きが分かるよう両方の値を記録する。
        if (sourcePresent != instancePresent) {
            overrides.push_back({
                component.typeId,
                component.displayName,
                sourcePresent,
                instancePresent,
            });
        }
    }

    return overrides;
}

std::vector<PresetManager::PrefabVariantOverride> PresetManager::GetPrefabVariantOverrides(
    const engine::Object3D* object) const {
    // 派生 Prefab と元 Prefab の同じ sourceObjectId を比較し、明示された差分を列挙する。
    std::vector<PrefabVariantOverride> overrides;

    // 配置リンクがないオブジェクトでは、元のVariantを特定できない。
    if (!object || !object->IsPrefabInstance()) {
        return overrides;
    }

    const auto& info = object->GetPrefabInstanceInfo();
    auto assetIt = std::find_if(prefabs_.begin(), prefabs_.end(), [&info](const auto& pair) {
        return pair.second.value("assetId", "") == info.assetId;
    });

    // 配置が参照している永続IDのアセットを解決できなければ差分は空。
    if (assetIt == prefabs_.end()) {
        return overrides;
    }

    const std::string baseAssetId = assetIt->second.value("baseAssetId", "");

    // 基底のIDが未指定なら通常Prefabなので、Variantの差分はない。
    if (baseAssetId.empty()) {
        return overrides;
    }

    auto baseIt = std::find_if(prefabs_.begin(), prefabs_.end(), [&baseAssetId](const auto& pair) {
        return pair.second.value("assetId", "") == baseAssetId;
    });

    // 基底の台帳かルートが欠けていれば、基底値と比較できない。
    if (baseIt == prefabs_.end() || !baseIt->second.contains("root")) {
        return overrides;
    }

    const json& propertyOverrides = assetIt->second["propertyOverrides"];
    // この永続ノードIDに対する値の上書き一覧がなければ返す項目がない。
    if (!propertyOverrides.is_object() || !propertyOverrides.contains(info.sourceObjectId) ||
        !propertyOverrides[info.sourceObjectId].is_object()) {
        return overrides;
    }

    const json* baseNode = FindSourceNodeRecursive(baseIt->second["root"], info.sourceObjectId);
    const json* variantNode = FindPrefabSourceNode(info);

    // 基底・派生双方から同じノードを解決できる場合だけ表示値を作る。
    if (!baseNode || !variantNode) {
        return overrides;
    }

    EditorPropertyRegistry* registry = EditorPropertyRegistry::GetInstance();
    registry->InitializeBuiltInProperties();

    for (auto it = propertyOverrides[info.sourceObjectId].begin();
        it != propertyOverrides[info.sourceObjectId].end(); ++it) {
        const EditorPropertyDescriptor* property = registry->Find(it.key());
        // 存在状態も上書きされているコンポーネントは、内部値を個別の上書き一覧へ重複表示しない。
        const json& componentOverrides = assetIt->second["componentOverrides"];
        // 値が属する種類に構造の上書きがあれば、値の項目を重ねて表示しない。
        if (property && componentOverrides.is_object() &&
            componentOverrides.contains(info.sourceObjectId) &&
            componentOverrides[info.sourceObjectId].is_object() &&
            componentOverrides[info.sourceObjectId].contains(property->componentTypeId)) {
            continue;
        }

        overrides.push_back({
            it.key(),
            property ? property->displayName : it.key(),
            ReadSourceProperty(*baseNode, it.key()),
            ReadSourceProperty(*variantNode, it.key()),
        });
    }

    return overrides;
}

std::vector<PresetManager::PrefabVariantComponentOverride>
PresetManager::GetPrefabVariantComponentOverrides(const engine::Object3D* object) const {
    // 派生側の Component 上書きだけを、元と派生の存在状態を添えて返す。
    std::vector<PrefabVariantComponentOverride> overrides;

    // 配置とPrefabのリンクが揃わなければ、差分を解決できない。
    if (!object || !object->IsPrefabInstance()) {
        return overrides;
    }

    const auto& info = object->GetPrefabInstanceInfo();
    auto assetIt = std::find_if(prefabs_.begin(), prefabs_.end(), [&info](const auto& pair) {
        return pair.second.value("assetId", "") == info.assetId;
    });

    // 該当するアセットが台帳に残っていなければ空の一覧を返す。
    if (assetIt == prefabs_.end()) {
        return overrides;
    }

    const std::string baseAssetId = assetIt->second.value("baseAssetId", "");
    auto baseIt = std::find_if(prefabs_.begin(), prefabs_.end(), [&baseAssetId](const auto& pair) {
        return pair.second.value("assetId", "") == baseAssetId;
    });

    // 基底を持たないか、比較できる基底ルートがなければ構造を比較しない。
    if (baseAssetId.empty() || baseIt == prefabs_.end() || !baseIt->second.contains("root")) {
        return overrides;
    }

    const json& allOverrides = assetIt->second["componentOverrides"];
    // このノードのコンポーネント差分がオブジェクト形式で保存されていなければ終了する。
    if (!allOverrides.is_object() || !allOverrides.contains(info.sourceObjectId) ||
        !allOverrides[info.sourceObjectId].is_object()) {
        return overrides;
    }

    const json* baseNode = FindSourceNodeRecursive(baseIt->second["root"], info.sourceObjectId);
    const json* variantNode = FindPrefabSourceNode(info);

    // 同じノードを基底と派生の両方から引ける場合だけ、存在状態を読み取る。
    if (!baseNode || !variantNode) {
        return overrides;
    }

    EditorPropertyRegistry* registry = EditorPropertyRegistry::GetInstance();
    registry->InitializeBuiltInProperties();

    for (auto it = allOverrides[info.sourceObjectId].begin();
        it != allOverrides[info.sourceObjectId].end(); ++it) {
        // 真偽値以外は、付け外しの記録として使わない。
        if (!it.value().is_boolean()) {
            continue;
        }

        const EditorComponentDescriptor* component = registry->FindComponent(it.key());

        // 現在の台帳に登録され、Prefabの構造差分に対応する種類だけを出す。
        if (!component || !IsPrefabComponentDescriptor(*component)) {
            continue;
        }

        overrides.push_back({
            component->typeId,
            component->displayName,
            registry->IsComponentPresent(*baseNode, component->typeId),
            registry->IsComponentPresent(*variantNode, component->typeId),
        });
    }

    return overrides;
}

bool PresetManager::HasValidPrefabSource(const engine::Object3D* object) const {
    // リンク情報があっても元ノードを解決できなければ編集対象にしない。
    return object && object->IsPrefabInstance() &&
        FindPrefabSourceNode(object->GetPrefabInstanceInfo()) != nullptr;
}

bool PresetManager::ApplyPrefabProperty(
    engine::Object3D* object,
    const std::string& propertyPath,
    const std::vector<engine::Object3D*>& sceneObjects) {
    // 個体固有の配置値は共有元へ書かず、差分のある Property だけ Asset へ反映する。
    // 実体と配置リンクがなければ、書き戻すアセットを特定できない。
    if (!object || !object->IsPrefabInstance()) {
        return false;
    }

    const auto info = object->GetPrefabInstanceInfo();

    // 表示名と配置専用の項目は、その配置だけに残してアセットに適用しない。
    if (propertyPath == "identity.name" || IsInstancePlacementProperty(info, propertyPath)) {
        return false;
    }

    EditorPropertyRegistry* registry = EditorPropertyRegistry::GetInstance();
    const EditorPropertyDescriptor* property = registry->Find(propertyPath);
    json* sourceNode = FindPrefabSourceNode(info);

    // 項目の定義か対象ノードがない場合は、書き込み先を確定できない。
    if (!property || !sourceNode) {
        return false;
    }

    std::string prefabName = info.prefabName;

    // 表示名が改名されても、保存されたAsset IDを優先して実体を探し直す。
    auto assetIt = prefabs_.find(prefabName);

    // 名前から見つからないかIDが一致しなければ、改名を考慮して永続IDで探す。
    if (assetIt == prefabs_.end() || assetIt->second.value("assetId", "") != info.assetId) {
        assetIt = std::find_if(prefabs_.begin(), prefabs_.end(), [&info](const auto& pair) {
            return pair.second.value("assetId", "") == info.assetId;
        });

        // 永続IDでもアセットを見つけられない場合は適用を中断する。
        if (assetIt == prefabs_.end()) {
            return false;
        }

        prefabName = assetIt->first;
    }

    // 更新前の有効値をAsset別に退避し、後で手動上書きしていないInstanceだけ同期する。
    std::unordered_map<std::string, json> oldEffectiveValues;

    for (const auto& [name, asset] : prefabs_) {
        (void)name;
        // 比較可能なルートがないアセットは同期前の基準値を取得できない。
        if (!asset.contains("root") || !asset["root"].is_object())
            continue;
        const json* node = FindSourceNodeRecursive(asset["root"], info.sourceObjectId);

        // 同じ永続ノードIDを持つアセットは、その時点の有効値を覚える。
        if (node) {
            oldEffectiveValues[asset.value("assetId", "")] = ReadSourceProperty(*node, propertyPath);
        }
    }

    const json beforeAsset = assetIt->second;
    const json newSourceValue = registry->GetValue(object, propertyPath);

    // 値を取得できない項目は、空の値で共有定義を上書きしない。
    if (newSourceValue.is_null()) {
        return false;
    }

    const std::string baseAssetId = assetIt->second.value("baseAssetId", "");

    // 基底を持たないPrefabでは元ノードへ直接書き込む。
    if (baseAssetId.empty()) {
        // 保存側の項目へ値を書き込めなければ、継承解決や他の配置への伝播を行わない。
        if (!WriteSourceProperty(*sourceNode, propertyPath, newSourceValue)) {
            return false;
        }
    // Variantの場合は有効値を基底と比較し、違いをこの派生に記録する。
    } else {
        auto baseIt = std::find_if(prefabs_.begin(), prefabs_.end(), [&baseAssetId](const auto& pair) {
            return pair.second.value("assetId", "") == baseAssetId;
        });
        const json* baseNode = baseIt != prefabs_.end() && baseIt->second.contains("root")
            ? FindSourceNodeRecursive(baseIt->second["root"], info.sourceObjectId)
            : nullptr;
        const json baseValue = baseNode ? ReadSourceProperty(*baseNode, propertyPath) : json();
        json& objectOverrides = assetIt->second["propertyOverrides"][info.sourceObjectId];

        // 基底値と同じなら明示上書きを消し、基底を後から編集したときに追従できるようにする。
        if (!baseValue.is_null() && JsonValuesEqual(baseValue, newSourceValue)) {
            objectOverrides.erase(propertyPath);

            // この項目を消してノードの上書きがなくなった場合は、空になったノードの差分も除く。
            if (objectOverrides.empty()) {
                assetIt->second["propertyOverrides"].erase(info.sourceObjectId);
            }
        // 基底と値が異なるか基底の値を読めない場合は明示上書きとして記録する。
        } else {
            objectOverrides[propertyPath] = newSourceValue;
        }
    }

    RefreshPrefabInheritance();

    // 同じ元ノードを共有する配置のうち、旧値のままのものだけ新値へ追従させる。
    // 個体側で上書きした値は変更しない。
    for (engine::Object3D* candidate : sceneObjects) {
        // 無効な枠、適用元自身、Prefab外のオブジェクトにはアセットの結果を配らない。
        if (!candidate || candidate == object || !candidate->IsPrefabInstance()) {
            continue;
        }

        const auto& candidateInfo = candidate->GetPrefabInstanceInfo();

        // 同じ永続ノードでも、別の配置グループに属する場合だけ同期対象にする。
        if (candidateInfo.sourceObjectId != info.sourceObjectId || candidateInfo.instanceId == info.instanceId) {
            continue;
        }

        auto oldIt = oldEffectiveValues.find(candidateInfo.assetId);
        const json* newNode = FindPrefabSourceNode(candidateInfo);
        const json candidateNewValue = newNode ? ReadSourceProperty(*newNode, propertyPath) : json();
        // 新旧の有効値があり、共有値だけが変更され、配置は旧値を使っているときに更新する。
        if (oldIt != oldEffectiveValues.end() && !candidateNewValue.is_null() &&
            !JsonValuesEqual(oldIt->second, candidateNewValue) &&
            JsonValuesEqual(registry->GetValue(candidate, propertyPath), oldIt->second)) {
            registry->SetValue(candidate, propertyPath, candidateNewValue);
        }
    }

    SavePrefabs();

    // 一括適用による履歴の抑止がない場合だけ、今回のアセット更新を一件の編集履歴にする。
    if (!suppressPrefabAssetTransaction_) {
        RegisterPrefabAssetTransaction(this, prefabName, beforeAsset, assetIt->second, "Apply Prefab Override");
    }

    return true;
}

bool PresetManager::ApplyPrefabComponent(
    engine::Object3D* object,
    const std::string& componentTypeId,
    const std::vector<engine::Object3D*>& sceneObjects) {
    // Component の追加・削除を構造差分へ保存し、必要なら内部 Property も維持する。
    // 配置リンクがなければ、共有元へ構造変更を書き込めない。
    if (!object || !object->IsPrefabInstance()) {
        return false;
    }

    EditorPropertyRegistry* registry = EditorPropertyRegistry::GetInstance();
    registry->InitializeBuiltInProperties();
    const EditorComponentDescriptor* component = registry->FindComponent(componentTypeId);
    // 台帳にあり、Prefabで構造差分を持てて、対象にも適用できる種類だけ扱う。
    if (!component || !IsPrefabComponentDescriptor(*component) ||
        !registry->IsComponentApplicable(object, componentTypeId)) {
        return false;
    }

    const auto info = object->GetPrefabInstanceInfo();
    json* sourceNode = FindPrefabSourceNode(info);

    // 配置元のノードを見つけられなければ、反映先がないため戻る。
    if (!sourceNode) {
        return false;
    }

    const bool sourcePresent = registry->IsComponentPresent(*sourceNode, componentTypeId);
    const bool instancePresent = registry->IsComponentPresent(object, componentTypeId);

    // 現在の共有元と配置で有無が同じなら、適用する付け外しがない。
    if (sourcePresent == instancePresent) {
        return false;
    }

    std::string prefabName = info.prefabName;
    auto assetIt = prefabs_.find(prefabName);

    // 配置に残った名前で正しいアセットを得られなければ永続IDから解決する。
    if (assetIt == prefabs_.end() || assetIt->second.value("assetId", "") != info.assetId) {
        assetIt = std::find_if(prefabs_.begin(), prefabs_.end(), [&info](const auto& pair) {
            return pair.second.value("assetId", "") == info.assetId;
        });

        // 名前でも永続IDでもアセットを解決できなければ設定を保存しない。
        if (assetIt == prefabs_.end()) {
            return false;
        }

        prefabName = assetIt->first;
    }

    MigrateLegacyComponentOverrides(assetIt->second);

    // Componentの追加・削除前の有効状態を各Prefab Assetごとに控える。
    std::unordered_map<std::string, bool> oldEffectivePresence;

    for (const auto& [name, asset] : prefabs_) {
        (void)name;

        // 比較用のルートを持たないアセットでは、旧コンポーネントの状態を確認できない。
        if (!asset.contains("root") || !asset["root"].is_object()) {
            continue;
        }

        const json* node = FindSourceNodeRecursive(asset["root"], info.sourceObjectId);

        // 同一ノードがあるアセットでだけ、現在の有無を記録する。
        if (node) {
            oldEffectivePresence[asset.value("assetId", "")] =
                registry->IsComponentPresent(*node, componentTypeId);
        }
    }

    const json beforeAsset = assetIt->second;
    const std::string baseAssetId = assetIt->second.value("baseAssetId", "");

    // 通常Prefabでは有無と保存対象の値を、解決した元ノードへ直接書き込む。
    if (baseAssetId.empty()) {
        // 元ノードへ構造を保存できない場合は、継承の更新や配置への反映を中断する。
        if (!WriteComponentStateFromObject(*sourceNode, object, *component, instancePresent)) {
            return false;
        }
    // Variantでは基底の状態を見つけ、共有アセットに書く内容を差分として分ける。
    } else {
        auto baseIt = std::find_if(prefabs_.begin(), prefabs_.end(), [&baseAssetId](const auto& pair) {
            return pair.second.value("assetId", "") == baseAssetId;
        });
        const json* baseNode = baseIt != prefabs_.end() && baseIt->second.contains("root")
            ? FindSourceNodeRecursive(baseIt->second["root"], info.sourceObjectId)
            : nullptr;
        // 基底のアセットまたは同じ永続ノードを取得できなければ差分を書かない。
        if (!baseNode) {
            return false;
        }

        RemoveComponentPropertyOverrides(assetIt->second, info.sourceObjectId, componentTypeId);

        // 有無が親と一致すれば構造差分を消し、追加した場合は内部Propertyの差分を残す。
        json& objectComponentOverrides = assetIt->second["componentOverrides"][info.sourceObjectId];
        const bool basePresent = registry->IsComponentPresent(*baseNode, componentTypeId);

        // 配置の有無が基底と同じになれば、派生だけにある構造差分を解除する。
        if (basePresent == instancePresent) {
            objectComponentOverrides.erase(componentTypeId);

            // この種類を解除して構造差分が空になれば、ノードのキーも取り除く。
            if (objectComponentOverrides.empty()) {
                assetIt->second["componentOverrides"].erase(info.sourceObjectId);
            }
        // 基底と異なる有無は、この派生Prefabの付け外しとして保存する。
        } else {
            objectComponentOverrides[componentTypeId] = instancePresent;
        }

        // 配置側に実体がある場合は、構造の有無に加えて保存可能な内部値も調べる。
        if (instancePresent) {
            for (const EditorPropertyDescriptor* property : registry->GetPropertiesForComponent(componentTypeId)) {
                // 定義があり、Prefabで上書き可能かつ保存パスがある項目だけを扱う。
                if (!property || !HasEditorPropertyFlag(property->flags, EditorPropertyFlags::kPrefabOverride) ||
                    property->serializedPath.empty()) {
                    continue;
                }

                const json instanceValue = registry->GetValue(object, property->path);
                const json baseValue = ReadSourceProperty(*baseNode, property->path);

                // 有効な値を取得でき、基底と異なるときだけ内部値の差分を保存する。
                if (!instanceValue.is_null() && !JsonValuesEqual(baseValue, instanceValue)) {
                    assetIt->second["propertyOverrides"][info.sourceObjectId][property->path] = instanceValue;
                }
            }
        }
    }

    RefreshPrefabInheritance();

    // 派生関係を再計算し、明示的に構造を変更していない配置だけ同期する。
    for (engine::Object3D* candidate : sceneObjects) {
        // 無効な枠と適用元の配置物、Prefab以外のオブジェクトを飛ばす。
        if (!candidate || candidate == object || !candidate->IsPrefabInstance()) {
            continue;
        }

        const auto& candidateInfo = candidate->GetPrefabInstanceInfo();
        // 同じ永続ノードの別の配置グループを同期対象にする。
        if (candidateInfo.sourceObjectId != info.sourceObjectId ||
            candidateInfo.instanceId == info.instanceId) {
            continue;
        }

        const auto oldIt = oldEffectivePresence.find(candidateInfo.assetId);
        const json* newNode = FindPrefabSourceNode(candidateInfo);

        // 更新の前後を確認できない配置では、既存のコンポーネントを変えない。
        if (oldIt == oldEffectivePresence.end() || !newNode) {
            continue;
        }

        const bool newPresence = registry->IsComponentPresent(*newNode, componentTypeId);
        const bool candidatePresence = registry->IsComponentPresent(candidate, componentTypeId);

        // 共有の存在状態が変わり、配置側で独自の付け外しがない場合だけ伝播させる。
        if (newPresence != oldIt->second && candidatePresence == oldIt->second) {
            ApplySerializedComponentState(candidate, *newNode, *component);
        }
    }

    SavePrefabs();

    // 個別の編集履歴を許可されている場合だけ、この構造変更一件を登録する。
    if (!suppressPrefabAssetTransaction_) {
        RegisterPrefabAssetTransaction(
            this,
            prefabName,
            beforeAsset,
            assetIt->second,
            "Apply Prefab Component Override");
    }

    return true;
}

bool PresetManager::RevertPrefabComponent(engine::Object3D* object, const std::string& componentTypeId) {
    // 配置個体の Component 有無を、解決済みの Prefab 元ノードへ戻す。
    // 配置リンクのない実体には、コンポーネントを戻せる共有元がない。
    if (!object || !object->IsPrefabInstance()) {
        return false;
    }

    const json* sourceNode = FindPrefabSourceNode(object->GetPrefabInstanceInfo());

    // 共有元のノードが解決できなければ、復元時の有無を決められない。
    if (!sourceNode) {
        return false;
    }

    EditorPropertyRegistry* registry = EditorPropertyRegistry::GetInstance();
    registry->InitializeBuiltInProperties();
    const EditorComponentDescriptor* component = registry->FindComponent(componentTypeId);

    // Prefabの構造差分に対応する登録済みの種類だけを戻す。
    if (!component || !IsPrefabComponentDescriptor(*component)) {
        return false;
    }

    const bool sourcePresent = registry->IsComponentPresent(*sourceNode, componentTypeId);
    const bool instancePresent = registry->IsComponentPresent(object, componentTypeId);

    // 現在と元の有無が同じなら、取り消す構造変更はない。
    if (sourcePresent == instancePresent) {
        return false;
    }

    return ApplySerializedComponentState(object, *sourceNode, *component);
}

bool PresetManager::RevertPrefabVariantProperty(
    engine::Object3D* object,
    const std::string& propertyPath,
    const std::vector<engine::Object3D*>& sceneObjects) {
    // 派生側の明示的な Property 上書きを外し、元 Prefab の有効値を再継承する。
    // 配置リンクがない場合は取り消し先のVariantを特定できない。
    if (!object || !object->IsPrefabInstance()) {
        return false;
    }

    const auto info = object->GetPrefabInstanceInfo();
    auto assetIt = std::find_if(prefabs_.begin(), prefabs_.end(), [&info](const auto& pair) {
        return pair.second.value("assetId", "") == info.assetId;
    });

    // アセットが未登録か基底IDを持たなければ、Variantの取り消しはできない。
    if (assetIt == prefabs_.end() || assetIt->second.value("baseAssetId", "").empty()) {
        return false;
    }

    json& allOverrides = assetIt->second["propertyOverrides"];
    // 対象ノードの指定パスに明示上書きがなければ取り消す差分がない。
    if (!allOverrides.is_object() || !allOverrides.contains(info.sourceObjectId) ||
        !allOverrides[info.sourceObjectId].is_object() ||
        !allOverrides[info.sourceObjectId].contains(propertyPath)) {
        return false;
    }

    // 配布元の値を書き換える前に、影響を受ける可能性がある配置の旧基準値を控える。
    std::unordered_map<std::string, json> oldEffectiveValues;

    for (const auto& [name, asset] : prefabs_) {
        (void)name;
        // ルートが欠けるか形式が異なるときは、元ノードの値を取得しない。
        if (!asset.contains("root") || !asset["root"].is_object())
            continue;
        const json* node = FindSourceNodeRecursive(asset["root"], info.sourceObjectId);

        // 同じ永続ノードがあるアセットから、取り消し前の有効値を控える。
        if (node) {
            oldEffectiveValues[asset.value("assetId", "")] = ReadSourceProperty(*node, propertyPath);
        }
    }

    const std::string prefabName = assetIt->first;
    const json beforeAsset = assetIt->second;

    // Variant上のキーを消して親の有効値へ戻し、配置の手動上書きは保持する。
    allOverrides[info.sourceObjectId].erase(propertyPath);

    // 最後の上書きを取り消した場合は、空になったノードの差分キーも除く。
    if (allOverrides[info.sourceObjectId].empty()) {
        allOverrides.erase(info.sourceObjectId);
    }

    RefreshPrefabInheritance();

    EditorPropertyRegistry* registry = EditorPropertyRegistry::GetInstance();

    for (engine::Object3D* candidate : sceneObjects) {
        // 同じ永続ノードを参照する有効なPrefab配置だけ、継承変更の影響を確認する。
        if (!candidate || !candidate->IsPrefabInstance() ||
            candidate->GetPrefabInstanceInfo().sourceObjectId != info.sourceObjectId) {
            continue;
        }

        const auto& candidateInfo = candidate->GetPrefabInstanceInfo();
        auto oldIt = oldEffectiveValues.find(candidateInfo.assetId);
        const json* newNode = FindPrefabSourceNode(candidateInfo);
        const json newValue = newNode ? ReadSourceProperty(*newNode, propertyPath) : json();
        // 新旧の共有値が取れ、配置が旧値を維持していた場合だけ継承し直す。
        if (oldIt != oldEffectiveValues.end() && !newValue.is_null() &&
            JsonValuesEqual(registry->GetValue(candidate, propertyPath), oldIt->second)) {
            registry->SetValue(candidate, propertyPath, newValue);
        }
    }

    SavePrefabs();
    RegisterPrefabAssetTransaction(this, prefabName, beforeAsset, assetIt->second,
        "Revert Prefab Variant Override");
    return true;
}

bool PresetManager::RevertPrefabVariantComponent(
    engine::Object3D* object,
    const std::string& componentTypeId,
    const std::vector<engine::Object3D*>& sceneObjects) {
    // 派生 Component の存在差分と内部 Property 差分を消し、元構造へ戻す。
    // Prefabに属していない配置では構造の差分を特定できない。
    if (!object || !object->IsPrefabInstance()) {
        return false;
    }

    const auto info = object->GetPrefabInstanceInfo();
    auto assetIt = std::find_if(prefabs_.begin(), prefabs_.end(), [&info](const auto& pair) {
        return pair.second.value("assetId", "") == info.assetId;
    });

    // 基底を持つ登録済みのアセットでなければ、派生側の構造変更を取り消せない。
    if (assetIt == prefabs_.end() || assetIt->second.value("baseAssetId", "").empty()) {
        return false;
    }

    json& allOverrides = assetIt->second["componentOverrides"];
    // 対象ノードと種類に対応する明示的な付け外しがなければ、取り消す処理がない。
    if (!allOverrides.is_object() || !allOverrides.contains(info.sourceObjectId) ||
        !allOverrides[info.sourceObjectId].is_object() ||
        !allOverrides[info.sourceObjectId].contains(componentTypeId)) {
        return false;
    }

    EditorPropertyRegistry* registry = EditorPropertyRegistry::GetInstance();
    registry->InitializeBuiltInProperties();
    const EditorComponentDescriptor* component = registry->FindComponent(componentTypeId);

    // Prefabで扱える種類の登録がなければ、配置へ状態を反映できない。
    if (!component || !IsPrefabComponentDescriptor(*component)) {
        return false;
    }

    // 差分を消す前に、同じノードを使うアセットごとの存在状態を控える。
    std::unordered_map<std::string, bool> oldEffectivePresence;

    for (const auto& [name, asset] : prefabs_) {
        (void)name;
        // 有効なルートから調べられるアセットだけを比較対象にする。
        if (!asset.contains("root") || !asset["root"].is_object())
            continue;
        const json* node = FindSourceNodeRecursive(asset["root"], info.sourceObjectId);

        // 同じ永続ノードIDがあるアセットで、その種類の有無を保存する。
        if (node) {
            oldEffectivePresence[asset.value("assetId", "")] =
                registry->IsComponentPresent(*node, componentTypeId);
        }
    }

    const std::string prefabName = assetIt->first;
    const json beforeAsset = assetIt->second;
    allOverrides[info.sourceObjectId].erase(componentTypeId);

    // この種類を消して構造変更が残らなければ、ノードの空の差分表を片付ける。
    if (allOverrides[info.sourceObjectId].empty()) {
        allOverrides.erase(info.sourceObjectId);
    }

    RemoveComponentPropertyOverrides(assetIt->second, info.sourceObjectId, componentTypeId);

    // 存在差分を消すと内部Propertyも意味を失うため、両方取り除いて継承を再計算する。
    RefreshPrefabInheritance();

    for (engine::Object3D* candidate : sceneObjects) {
        // 同じ永続ノードを参照するPrefab配置だけを同期対象にする。
        if (!candidate || !candidate->IsPrefabInstance() ||
            candidate->GetPrefabInstanceInfo().sourceObjectId != info.sourceObjectId) {
            continue;
        }

        const auto& candidateInfo = candidate->GetPrefabInstanceInfo();
        const auto oldIt = oldEffectivePresence.find(candidateInfo.assetId);
        const json* newNode = FindPrefabSourceNode(candidateInfo);
        // 取り消し前か取り消し後の状態を取得できない配置では、有無を変えない。
        if (oldIt == oldEffectivePresence.end() || !newNode)
            continue;
        const bool newPresence = registry->IsComponentPresent(*newNode, componentTypeId);
        const bool currentPresence = registry->IsComponentPresent(candidate, componentTypeId);

        // 以前の継承状態に従っていた配置だけを更新し、各配置で独自に付け外しした部品は保持する。
        if (newPresence != oldIt->second && currentPresence == oldIt->second) {
            ApplySerializedComponentState(candidate, *newNode, *component);
        }
    }

    SavePrefabs();
    RegisterPrefabAssetTransaction(
        this,
        prefabName,
        beforeAsset,
        assetIt->second,
        "Revert Prefab Variant Component Override");
    return true;
}

bool PresetManager::RevertPrefabProperty(engine::Object3D* object, const std::string& propertyPath) {
    // インスタンス固有の名前・配置値を避け、元ノードの値だけ復元する。
    // 配置リンクがない個体には復元に使える元ノードのIDがない。
    if (!object || !object->IsPrefabInstance()) {
        return false;
    }

    const auto& info = object->GetPrefabInstanceInfo();

    // 名前と配置専用の値は、その配置の編集として保護する。
    if (propertyPath == "identity.name" || IsInstancePlacementProperty(info, propertyPath)) {
        return false;
    }

    const json* sourceNode = FindPrefabSourceNode(info);

    // リンクから有効な元ノードへ辿れなければ復元しない。
    if (!sourceNode) {
        return false;
    }

    const json value = ReadSourceProperty(*sourceNode, propertyPath);

    // 元ノードに保存値がなければ、配置側の値へ空の値を適用しない。
    if (value.is_null()) {
        return false;
    }

    return EditorPropertyRegistry::GetInstance()->SetValue(object, propertyPath, value);
}

std::int32_t PresetManager::ApplyAllPrefabOverrides(engine::Object3D* object, const std::vector<engine::Object3D*>& sceneObjects) {
    // 一配置群の構造差分を先に適用し、Component内部のProperty差分を後から処理する。
    // Prefab配置の一部でなければ、まとめて適用する配置グループがない。
    if (!object || !object->IsPrefabInstance()) {
        return 0;
    }

    const auto instanceInfo = object->GetPrefabInstanceInfo();
    const std::string instanceId = instanceInfo.instanceId;
    std::string prefabName = instanceInfo.prefabName;
    auto assetIt = prefabs_.find(prefabName);

    // 名前が見つからないか別のアセットに再利用されている場合は永続IDで引き直す。
    if (assetIt == prefabs_.end() || assetIt->second.value("assetId", "") != instanceInfo.assetId) {
        assetIt = std::find_if(prefabs_.begin(), prefabs_.end(), [&instanceInfo](const auto& pair) {
            return pair.second.value("assetId", "") == instanceInfo.assetId;
        });

        // 同じ永続IDを持つ登録先が存在しなければ、差分を変更せず0件を返す。
        if (assetIt == prefabs_.end()) {
            return 0;
        }

        prefabName = assetIt->first;
    }

    const json beforeAsset = assetIt->second;

    std::int32_t applied = 0;

    // 各PropertyごとのUndo履歴を抑え、階層全体のApplyを一操作として登録する。
    suppressPrefabAssetTransaction_ = true;

    for (engine::Object3D* candidate : sceneObjects) {
        // 現在の配置と同じInstance IDを持つ有効なオブジェクトだけを処理する。
        if (!candidate || !candidate->IsPrefabInstance() ||
            candidate->GetPrefabInstanceInfo().instanceId != instanceId) {
            continue;
        }

        const auto componentOverrides = GetPrefabComponentOverrides(candidate);

        for (const auto& entry : componentOverrides) {
            // 構造差分の適用が成功した場合だけ、処理済みの件数に加える。
            if (ApplyPrefabComponent(candidate, entry.componentTypeId, sceneObjects)) {
                ++applied;
            }
        }

        const auto overrides = GetPrefabOverrides(candidate);

        for (const auto& entry : overrides) {
            // 値の差分を適用できた場合だけ、処理済みの件数に加える。
            if (ApplyPrefabProperty(candidate, entry.propertyPath, sceneObjects)) {
                ++applied;
            }
        }
    }

    // 個別適用の履歴抑止を解除してから、階層全体の変更前後を一件の Undo として登録する。
    suppressPrefabAssetTransaction_ = false;
    assetIt = prefabs_.find(prefabName);

    // 適用後にもアセットが台帳に残る場合に、変更前後をまとめて編集履歴へ渡す。
    if (assetIt != prefabs_.end()) {
        RegisterPrefabAssetTransaction(this, prefabName, beforeAsset, assetIt->second, "Apply All Prefab Overrides");
    }

    return applied;
}

// 共有アセットへの変更は発生せず、現在の配置グループのコンポーネントとプロパティを継承元へ戻す。
std::int32_t PresetManager::RevertAllPrefabOverrides(engine::Object3D* object, const std::vector<engine::Object3D*>& sceneObjects) {
    // 同じ instanceId の階層を回り、構造差分と Property 差分を元へ戻す。
    // Prefab配置でなければ戻す対象のInstance IDがないため0件を返す。
    if (!object || !object->IsPrefabInstance()) {
        return 0;
    }

    const std::string instanceId = object->GetPrefabInstanceInfo().instanceId;
    std::int32_t reverted = 0;

    for (engine::Object3D* candidate : sceneObjects) {
        // 有効なPrefab配置のうち、同じInstance IDの階層だけを元へ戻す。
        if (!candidate || !candidate->IsPrefabInstance() ||
            candidate->GetPrefabInstanceInfo().instanceId != instanceId) {
            continue;
        }

        const auto componentOverrides = GetPrefabComponentOverrides(candidate);

        for (const auto& entry : componentOverrides) {
            // 取り消し処理で有無を戻せたものだけ、構造の処理済み件数に加える。
            if (RevertPrefabComponent(candidate, entry.componentTypeId)) {
                ++reverted;
            }
        }

        const auto overrides = GetPrefabOverrides(candidate);

        for (const auto& entry : overrides) {
            // 元ノードの値へ戻せたものだけ、値の処理済み件数に加える。
            if (RevertPrefabProperty(candidate, entry.propertyPath)) {
                ++reverted;
            }
        }
    }

    return reverted;
}

std::int32_t PresetManager::UnpackPrefabInstance(engine::Object3D* object, const std::vector<engine::Object3D*>& sceneObjects) {
    // 同じ配置群のリンク情報を外し、見た目は残したまま独立 Object にする。
    // 実体とPrefabリンクが揃う場合にだけ、解除する配置グループを探す。
    if (!object || !object->IsPrefabInstance()) {
        return 0;
    }

    const std::string instanceId = object->GetPrefabInstanceInfo().instanceId;
    std::int32_t unpacked = 0;

    for (engine::Object3D* candidate : sceneObjects) {
        // 同じInstance IDで現在もリンクしている実体だけ、リンク情報を解除して数える。
        if (candidate && candidate->IsPrefabInstance() &&
            candidate->GetPrefabInstanceInfo().instanceId == instanceId) {
            candidate->ClearPrefabInstanceInfo();
            ++unpacked;
        }
    }

    return unpacked;
}
