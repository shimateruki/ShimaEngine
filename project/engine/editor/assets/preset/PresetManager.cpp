#include "engine/editor/assets/preset/PresetManager.Internal.h"

#include <cstdint>
#include "engine/system/scene/BaseScene.h"
#include "engine/system/scene/SceneManager.h"

using namespace presetManagerImplementation;

PresetManager* PresetManager::GetInstance() {
    // 編集画面と配置処理が同じプリセット集合を参照する。
    static PresetManager sInstance;
    return &sInstance;
}

void PresetManager::Initialize() {
    // 現行形式を先に読み、旧ファイルが残る場合だけ統合して保存し直す。
    presets_.clear();
    prefabs_.clear();

    LoadPresets("Resources/json/preset/presets.json");
    LoadPrefabs();

    bool needsMigration = false;

    // 旧形式の敵プリセットが残っていれば、現行の登録一覧へ読み足す。
    if (fs::exists("Resources/json/preset/EnemyPresets.json")) {
        LoadPresets("Resources/json/preset/EnemyPresets.json");
        needsMigration = true;
    }

    // 旧形式のギミックプリセットが残っている場合も、移行対象に含める。
    if (fs::exists("Resources/json/preset/GimmickPresets.json")) {
        LoadPresets("Resources/json/preset/GimmickPresets.json");
        needsMigration = true;
    }

    // 統合保存を呼んだ後に旧ファイルを削除します。書き込みの成否はここでは判定せず、削除のエラーは例外にしません。
    if (needsMigration) {
        SaveAll();
        std::error_code error;
        fs::remove("Resources/json/preset/EnemyPresets.json", error);
        fs::remove("Resources/json/preset/GimmickPresets.json", error);
    }
}

void PresetManager::LoadPresets(const std::string& filename) {
    // Scene の GUID はプリセットの同一性に含めず、配置時に新しい識別子へ委ねる。
    std::ifstream file(filename);

    // 指定ファイルを開けなければ、現在のプリセット一覧をそのまま残す。
    if (!file.is_open()) {
        return;
    }

    json root;
    try {
        file >> root;

        // 名前をキーに持つオブジェクト形式の場合だけ、各定義を取り込む。
        if (root.is_object()) {
            for (auto& element : root.items()) {
                json preset = element.value();
                RemoveSceneObjectGuids(preset);
                presets_[element.key()] = std::move(preset);
            }
        }
    // JSON を読めない場合は、ファイル名を付けて標準エラーへ出す。
    } catch (...) {
        std::cerr << "Failed to parse preset file: " << filename << std::endl;
    }
}

void PresetManager::SavePresets(const std::string& filename) {
    // 保存時にも GUID を除き、配置元 Scene の固有 ID を持ち越さない。
    json root = json::object();

    for (const auto& pair : presets_) {
        json preset = pair.second;
        RemoveSceneObjectGuids(preset);
        root[pair.first] = std::move(preset);
    }

    try {
        fs::create_directories(fs::path(filename).parent_path());
        std::ofstream file(filename);

        // 保存先を開けた場合だけ、全プリセットを整形した JSON で書き出す。
        if (file.is_open()) {
            file << root.dump(4);
        }
    } catch (...) {
    }
}

void PresetManager::AddPresetFromObject(const std::string& presetName, engine::Object3D* obj) {
    // 巡回集合を渡して階層を複製し、循環参照をたどり続けない。
    // 元の物体や登録名が指定されていなければ、階層を記録しない。
    if (!obj || presetName.empty()) {
        return;
    }

    std::unordered_set<const engine::Object3D*> visited;
    presets_[presetName] = BuildPresetNode(obj, visited);
    SaveAll();
}

void PresetManager::ApplyPresetToObject(const std::string& presetName, engine::Object3D* obj) {
    // 保存元の GUID を除いて属性を反映した後、管理対象ステータスを再適用する。
    auto it = presets_.find(presetName);

    // 登録名が見つからないか対象がない場合は、設定を反映しない。
    if (it == presets_.end() || !obj) {
        return;
    }

    try {
        json preset = it->second;
        RemoveSceneObjectGuids(preset);
        obj->ImportFromJson(preset);
        auto* statusManager = GameplayStatusManager::GetInstance();
        statusManager->Initialize();
        statusManager->ApplyManagedStatus(obj, false);
    } catch (...) {
    }
}

std::vector<std::unique_ptr<engine::Object3D>> PresetManager::CreateObjectsFromPreset(
    const std::string& presetName,
    engine::Object3DCommon* common) const {
    // 部分生成で例外が出た場合は返却配列を空にし、不完全な階層を配置させない。
    std::vector<std::unique_ptr<engine::Object3D>> objects;
    auto it = presets_.find(presetName);

    // プリセットの定義か描画用の共通情報がなければ、空配列を返す。
    if (it == presets_.end() || !common) {
        return objects;
    }

    try {
        CreateObjectFromPresetNode(it->second, common, nullptr, objects);
    } catch (...) {
        objects.clear();
    }

    return objects;
}

void PresetManager::RemovePreset(const std::string& presetName) {
    // 一件だけの削除も、一覧を更新したときだけ保存する複数削除と同じ経路を使う。
    RemovePresets({ presetName });
}

void PresetManager::RemovePresets(const std::vector<std::string>& presetNames) {
    // 大量選択時もJSONを書き直すのは一回だけにして、エディターの停止を避ける。
    std::size_t removed = 0;

    for (const std::string& name : presetNames) {
        removed += presets_.erase(name);
    }

    // 登録されていた名前を一件でも消した場合だけ、プリセット一覧を書き直す。
    if (removed != 0)
        SavePresets();
}

void PresetManager::RenamePreset(const std::string& oldName, const std::string& newName) {
    // 名前衝突による上書きを防ぎ、キーだけを変更して内容を保持する。
    auto it = presets_.find(oldName);

    // 旧名が未登録、新名が空、または新名が使用済みなら変更しない。
    if (it == presets_.end() || newName.empty() || presets_.find(newName) != presets_.end()) {
        return;
    }

    presets_[newName] = it->second;
    presets_.erase(it);
    SaveAll();
}

void PresetManager::LoadPrefabs(const std::string& filename) {
    // 旧形式の直下マップと version 付きの prefabs マップを同じ形へ正規化する。
    prefabs_.clear();
    std::ifstream file(filename);

    // ファイルを開けなかった場合は、読み込み前に空にした一覧を使う。
    if (!file.is_open()) {
        return;
    }

    json data;
    try {
        file >> data;
    } catch (...) {
        return;
    }

    const json* source = &data;

    // prefabs キーがオブジェクトなら、新形式としてその中を読み取る。
    if (data.is_object() && data.contains("prefabs") && data["prefabs"].is_object()) {
        source = &data["prefabs"];
    }

    // 新旧どちらの形式でも名前と定義の組を持たない場合は、一覧へ取り込まない。
    if (!source->is_object()) {
        return;
    }

    for (auto it = source->begin(); it != source->end(); ++it) {
        prefabs_[it.key()] = NormalizePrefabAsset(it.key(), it.value());
    }

    RefreshPrefabInheritance();
}

void PresetManager::SavePrefabs(const std::string& filename) {
    // 派生元を解決した状態で version 3 形式へまとめて保存する。
    RefreshPrefabInheritance();
    const fs::path path(filename);

    // 保存先に親パスが付いている場合は、ファイルを開く前に作成する。
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path());
    }

    json data;
    data["version"] = 3;
    data["prefabs"] = json::object();

    for (const auto& [name, prefab] : prefabs_) {
        data["prefabs"][name] = prefab;
    }

    std::ofstream file(filename);

    // 保存先を開けた場合だけ、派生情報を含む JSON を出力する。
    if (file.is_open()) {
        file << data.dump(4);
    }
}

bool PresetManager::CreatePrefabFromPreset(
    const std::string& presetName,
    const std::string& prefabName) {
    // 元プリセットの木に安定した Object ID を付け、後の差分管理の基準にする。
    auto presetIt = presets_.find(presetName);

    // 元プリセットが未登録、新名が空、または同名の Prefab が存在する場合は作成しない。
    if (presetIt == presets_.end() || prefabName.empty() || prefabs_.find(prefabName) != prefabs_.end()) {
        return false;
    }

    const std::string assetId = MakeUniqueId("prefab");
    json asset;
    asset["version"] = 3;
    asset["assetId"] = assetId;
    asset["baseAssetId"] = "";
    asset["propertyOverrides"] = json::object();
    EnsurePrefabVariantFields(asset);
    asset["root"] = presetIt->second;
    EnsurePrefabObjectIds(asset["root"], assetId, "root");
    prefabs_[prefabName] = std::move(asset);
    SavePrefabs();
    return true;
}

bool PresetManager::CreatePrefabVariant(
    const std::string& basePrefabName,
    const std::string& variantName) {
    auto baseIt = prefabs_.find(basePrefabName);

    // 派生元が未登録、新名が空、または新名が使用済みの場合は作成しない。
    if (baseIt == prefabs_.end() || variantName.empty() || prefabs_.find(variantName) != prefabs_.end()) {
        return false;
    }

    const std::string baseAssetId = baseIt->second.value("assetId", "");

    // 継承先を結ぶ素材 ID と、最初に使うルートの両方がある場合だけ派生を作る。
    if (baseAssetId.empty() || !baseIt->second.contains("root") || !baseIt->second["root"].is_object()) {
        return false;
    }

    // 新しい素材IDで派生関係を作り、元のルートを初期表示に使います。固有の差分は空の状態から始めます。
    json asset;
    asset["version"] = 3;
    asset["assetId"] = MakeUniqueId("prefab");
    asset["baseAssetId"] = baseAssetId;
    asset["propertyOverrides"] = json::object();
    EnsurePrefabVariantFields(asset);
    asset["root"] = baseIt->second["root"];
    prefabs_[variantName] = std::move(asset);
    SavePrefabs();
    return true;
}

bool PresetManager::IsPrefabVariant(const std::string& prefabName) const {
    // 派生元の assetId が設定されているものだけをバリアントと判定する。
    auto it = prefabs_.find(prefabName);
    return it != prefabs_.end() && !it->second.value("baseAssetId", "").empty();
}

std::string PresetManager::GetPrefabBaseName(const std::string& prefabName) const {
    // 名前は変更できるため、保存済みの assetId から現在の派生元名を探す。
    auto it = prefabs_.find(prefabName);

    // 指定した Prefab が登録されていなければ、派生元名を返せない。
    if (it == prefabs_.end()) {
        return "";
    }

    const std::string baseAssetId = it->second.value("baseAssetId", "");

    // 派生元の ID が空なら、基底の定義なので空文字を返す。
    if (baseAssetId.empty()) {
        return "";
    }

    auto baseIt = std::find_if(prefabs_.begin(), prefabs_.end(), [&baseAssetId](const auto& pair) {
        return pair.second.value("assetId", "") == baseAssetId;
    });
    return baseIt == prefabs_.end() ? "" : baseIt->first;
}

PresetManager::PrefabStructureOverrideSummary PresetManager::GetPrefabStructureOverrideSummary(
    const std::string& prefabName) const {
    // 削除・付け替え・追加は構造差分、componentOverrides は成分ごとに数える。
    PrefabStructureOverrideSummary summary;
    auto it = prefabs_.find(prefabName);

    // Prefab が未登録なら、差分件数をすべてゼロで返す。
    if (it == prefabs_.end()) {
        return summary;
    }

    const json& asset = it->second;

    // 削除 ID が配列で保存されている場合は、その件数を数える。
    if (asset.contains("removedObjectIds") && asset["removedObjectIds"].is_array()) {
        summary.removedObjects = static_cast<std::int32_t>(asset["removedObjectIds"].size());
    }

    // 親を付け替えた一覧が配列なら、階層を移した物体の件数を数える。
    if (asset.contains("reparentedObjects") && asset["reparentedObjects"].is_array()) {
        summary.reparentedObjects = static_cast<std::int32_t>(asset["reparentedObjects"].size());
    }

    // 生のノード差分がオブジェクトなら、変更があるノード数を数える。
    if (asset.contains("nodeOverrides") && asset["nodeOverrides"].is_object()) {
        summary.rawNodeOverrides = static_cast<std::int32_t>(asset["nodeOverrides"].size());
    }

    // 成分差分がオブジェクトの場合だけ、物体ごとの内訳を走査する。
    if (asset.contains("componentOverrides") && asset["componentOverrides"].is_object()) {
        for (const auto& objectOverrides : asset["componentOverrides"]) {
            // この物体の成分差分もオブジェクトであれば、含まれる成分数を足す。
            if (objectOverrides.is_object()) {
                summary.componentOverrides += static_cast<std::int32_t>(objectOverrides.size());
            }
        }
    }

    // 追加した根だけでなく、その下にまとめて追加した子も変更数に含める。
    // 追加したルートの配列があれば、各追加階層の中身を確認する。
    if (asset.contains("addedChildren") && asset["addedChildren"].is_array()) {
        for (const auto& entry : asset["addedChildren"]) {
            // ノード情報のある追加階層だけを対象に、子孫も含むノード数を足す。
            if (entry.is_object() && entry.contains("node")) {
                summary.addedObjects += CountPrefabNodes(entry["node"]);
            }
        }
    }

    return summary;
}

std::vector<std::unique_ptr<engine::Object3D>> PresetManager::CreateObjectsFromPrefab(
    const std::string& prefabName,
    engine::Object3DCommon* common) const {
    // assetId は元データへのリンクに保ち、配置ごとに別の instanceId を発行する。
    std::vector<std::unique_ptr<engine::Object3D>> result;
    auto it = prefabs_.find(prefabName);

    // Prefab の定義か初期化用の共通情報がなければ、空の配置結果を返す。
    if (it == prefabs_.end() || !common) {
        return result;
    }

    const json& asset = it->second;

    // ルートの記述がオブジェクトとして保持されていなければ、実体を生成しない。
    if (!asset.contains("root") || !asset["root"].is_object()) {
        return result;
    }

    PrefabCreationContext context;
    context.prefabName = prefabName;
    context.assetId = asset.value("assetId", "");
    context.instanceId = MakeUniqueId("instance");
    CreateObjectFromPresetNode(asset["root"], common, nullptr, result, &context);

    // Prefab内部で接続しているイベントだけを配置ごとに振り直す。
    // 固定IDのまま複数置くと、移動床が別の噴出口を参照してしまう。
    std::unordered_set<std::int32_t> eventIds;
    std::unordered_set<std::int32_t> targetIds;
    for (const auto& object : result) {
        // 発信用 ID が負でない場合だけ、内部接続の調査へ加える。
        if (object->GetEventId() >= 0)
            eventIds.insert(object->GetEventId());
        // 参照先 ID も負でない場合だけ、内部接続の調査へ加える。
        if (object->GetTargetId() >= 0)
            targetIds.insert(object->GetTargetId());
    }

    // 発信側と受信側の両方に現れた ID だけを内部接続として扱い、片側だけのシーン向け参照を維持する。
    std::vector<std::int32_t> internalIds;
    for (std::int32_t id : eventIds) {
        // 発信用 ID を同じ配置内で参照している場合だけ、振り直しの対象にする。
        if (targetIds.contains(id))
            internalIds.push_back(id);
    }
    std::sort(internalIds.begin(), internalIds.end());

    // 内部接続が一つでもあれば、既存の ID を避けながら振り直す。
    if (!internalIds.empty()) {
        std::unordered_set<std::int32_t> usedIds = eventIds;
        usedIds.insert(targetIds.begin(), targetIds.end());
        // 配置先シーンが取れた場合は、既存物体の番号も予約して重複を防ぐ。
        if (auto* scene = SceneManager::GetInstance()->GetCurrentScene()) {
            for (const auto& object : scene->GetObjects()) {
                // シーンの物体参照が空の場合は、番号を調べず次へ進む。
                if (!object)
                    continue;
                // 既存の発信用 ID が有効なら、新しい割り当てから除外する。
                if (object->GetEventId() >= 0)
                    usedIds.insert(object->GetEventId());
                // 既存の参照先 ID が有効なら、新しい割り当てから除外する。
                if (object->GetTargetId() >= 0)
                    usedIds.insert(object->GetTargetId());
            }
        }

        // 配置先のシーンで使われている番号も避け、旧 ID の昇順に新しい ID を割り当てる。
        std::unordered_map<std::int32_t, std::int32_t> remappedIds;
        std::int32_t nextId = 10000;
        for (std::int32_t sourceId : internalIds) {
            while (usedIds.contains(nextId))
                ++nextId;
            remappedIds[sourceId] = nextId;
            usedIds.insert(nextId++);
        }

        for (const auto& object : result) {
            // 発信側の ID が内部接続に当たっていれば、新しい番号へ変更する。
            if (auto it = remappedIds.find(object->GetEventId()); it != remappedIds.end())
                object->SetEventId(it->second);
            // 受信側も同じ対応表を使い、変更後も内部の相手を指すようにする。
            if (auto it = remappedIds.find(object->GetTargetId()); it != remappedIds.end())
                object->SetTargetId(it->second);
        }
    }
    return result;
}

json* PresetManager::FindPrefabSourceNode(const engine::Object3D::PrefabInstanceInfo& info) {
    // 配置済み Object が記録した assetId と sourceObjectId の両方で元ノードを特定する。
    // 素材とノードへのリンクを持たないインスタンスは検索できない。
    if (!info.IsLinked()) {
        return nullptr;
    }

    for (auto& [name, asset] : prefabs_) {
        // 素材 ID が異なる場合やルート未登録の場合は、次の素材を探す。
        if (asset.value("assetId", "") != info.assetId || !asset.contains("root")) {
            continue;
        }

        return FindSourceNodeRecursive(asset["root"], info.sourceObjectId);
    }

    return nullptr;
}

const json* PresetManager::FindPrefabSourceNode(const engine::Object3D::PrefabInstanceInfo& info) const {
    // const 経路でも同じ ID 対応で検索し、呼び出し元に編集権限を渡さない。
    // 元素材とのリンクがない場合は、参照できるノードを返せない。
    if (!info.IsLinked()) {
        return nullptr;
    }

    for (const auto& [name, asset] : prefabs_) {
        // 一致する素材 ID とルートが揃うまで、各素材を飛ばす。
        if (asset.value("assetId", "") != info.assetId || !asset.contains("root")) {
            continue;
        }

        return FindSourceNodeRecursive(asset["root"], info.sourceObjectId);
    }

    return nullptr;
}

void PresetManager::AssignNewPrefabInstanceId(const std::vector<engine::Object3D*>& objects) const {
    // 同じ旧 instanceId を持つ複製群には同じ新 ID を割り当て、群のまとまりを保つ。
    std::unordered_map<std::string, std::string> replacementIds;

    for (engine::Object3D* object : objects) {
        // 空参照と Prefab インスタンスではない物体は、ID 発行の対象から外す。
        if (!object || !object->IsPrefabInstance()) {
            continue;
        }

        auto info = object->GetPrefabInstanceInfo();
        auto [it, inserted] = replacementIds.emplace(info.instanceId, std::string());

        // この旧 ID を初めて見た場合だけ、新しい ID を発行する。以後は同じ値を使う。
        if (inserted) {
            it->second = MakeUniqueId("instance");
        }

        info.instanceId = it->second;
        object->SetPrefabInstanceInfo(info);
    }
}

void PresetManager::RemovePrefab(const std::string& prefabName) {
    // 未登録名ではディスクに触れず、削除が成立したときだけ再保存する。
    if (prefabs_.erase(prefabName) > 0) {
        SavePrefabs();
    }
}

void PresetManager::RenamePrefab(const std::string& oldName, const std::string& newName) {
    // 派生関係は assetId で維持し、表示名の変更だけを保存する。
    auto it = prefabs_.find(oldName);
    // 旧名が未登録、空欄への変更、同名指定、または他の素材と名前が重なる場合は変更しない。
    if (it == prefabs_.end() || newName.empty() || oldName == newName ||
        prefabs_.find(newName) != prefabs_.end()) {
        return;
    }

    prefabs_[newName] = it->second;
    prefabs_.erase(it);
    SavePrefabs();
}

void PresetManager::SaveAll() {
    // プリセットと Prefab の両方を、各形式の保存処理に委ねる。
    SavePresets("Resources/json/preset/presets.json");
    SavePrefabs();
}
