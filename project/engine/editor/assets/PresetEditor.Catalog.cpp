#include "PresetEditor.h"

#include <cstdint>
#include "ModelManager.h"
#include "EditorObjectFactoryRegistry.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <utility>

#ifdef USE_IMGUI

// プリセットの生成、分類、検索、モデル候補の解決を担当します。
void PresetEditor::HandleDeferredDelete() {
    // 削除要求が無いか対象が選択されていない場合は、一覧を変更しない。
    if (!requestDelete_ || selectedName_.empty()) {
        return;
    }

    // 描画中に一覧を変えず、削除が予約されてから選択と保留フラグをまとめて解除する。
    PresetManager::GetInstance()->RemovePreset(selectedName_);
    selectedName_.clear();
    requestDelete_ = false;
}

void PresetEditor::RefreshModelList() {
    // フォルダー単位のModel名を集め、同じフォルダーに複数形式があっても候補は一件にする。
    modelNames_.clear();
    const std::filesystem::path root = "Resources/3DModel";

    // 素材のルートフォルダーが無ければ、候補が空のまま終了する。
    if (!std::filesystem::exists(root)) {
        return;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        // フォルダーをモデル候補として扱わないよう、通常ファイルだけを見る。
        if (!entry.is_regular_file()) {
            continue;
        }

        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        // 読み込み形式に合うOBJ、glTF、GLBだけを候補にする。
        if (ext != ".obj" && ext != ".gltf" && ext != ".glb") {
            continue;
        }

        // ルート直下のファイルだけはファイル名を使い、それ以外は相対フォルダーをModel名にする。
        std::filesystem::path relative = std::filesystem::relative(entry.path().parent_path(), root);
        std::string modelName = relative.generic_string();

        // ルート直下では相対フォルダー名が「.」になるため、拡張子を除いたファイル名を使う。
        if (modelName == ".") {
            modelName = entry.path().stem().generic_string();
        }

        // 同じモデル名を登録済みの場合は、形式が異なっても候補を増やさない。
        if (std::find(modelNames_.begin(), modelNames_.end(), modelName) == modelNames_.end()) {
            modelNames_.push_back(modelName);
        }
    }

    // ファイルシステムの列挙順に依存しないよう、候補名で並べる。
    std::sort(modelNames_.begin(), modelNames_.end());
}

bool PresetEditor::MatchesSearch(const std::string& name, const json& data) const {
    std::string needle = ToLower(searchBuffer_);

    // 検索欄が空なら、プロパティの照合を行わず一致とする。
    if (needle.empty()) {
        return true;
    }

    // 表示名だけでなく型、分類、説明、元Object名まで検索対象に含める。
    std::string haystack = name + " " +
        ReadString(data, "presetDisplayName", "") + " " +
        ReadString(data, "presetUsage", "") + " " +
        ReadString(data, "name", "") + " " +
        ReadString(data, "modelName", "") + " " +
        ReadString(data, "enemyType", "") + " " +
        ReadString(data, "gimmickType", "") + " " +
        ReadString(data, "itemType", "") + " " +
        ReadString(data, "presetCollection", "") + " " +
        ReadString(data, "presetSection", "") + " " +
        ReadString(data, "presetDescription", "") + " " +
        ReadString(data, "presetSourceObject", "");

    // 旧形式でparam内に保存された型名も拾う。
    if (data.contains("param") && data["param"].is_object()) {
        const json& param = data["param"];
        haystack += " " + ReadString(param, "enemyType", "");
        haystack += " " + ReadString(param, "gimmickType", "");
        haystack += " " + ReadString(param, "itemType", "");
    }

    return ToLower(haystack).find(needle) != std::string::npos;
}

bool PresetEditor::MatchesCategory(const json& data, Category category) const {
    // 「すべて」を選んだ場合はカテゴリの違いで候補を絞らない。
    if (category == Category::kAll) {
        return true;
    }

    // ステージ別カテゴリでは、コレクションに属し、絞り込みがあればその名前にも一致する必要がある。
    if (category == Category::kStage1) {
        return IsStageLibraryPreset(data) &&
            (stageCollection_.empty() || ReadString(data, "presetCollection", "") == stageCollection_);
    }

    return DetectCategory(data) == category;
}

bool PresetEditor::IsStageLibraryPreset(const json& data) const {
    return !ReadString(data, "presetCollection", "").empty();
}

PresetEditor::Category PresetEditor::DetectCategory(const json& data) const {
    // 新しいtype、直下の種別名、旧param内の種別名の順に分類を決める。
    std::string type = ReadString(data, "type", "");
    // 明示された種別が敵なら、旧項目を見ず敵カテゴリへ分類する。
    if (type == "Enemy")
        return Category::kEnemy;
    // 明示された種別がギミックなら、ギミックカテゴリへ分類する。
    if (type == "Gimmick")
        return Category::kGimmick;
    // 明示された種別がアイテムなら、アイテムカテゴリへ分類する。
    if (type == "Item")
        return Category::kItem;
    // 明示された種別がモデルなら、モデルカテゴリへ分類する。
    if (type == "Model")
        return Category::kModel;

    // 明示された種別で分類できない場合は、まず直下の敵種別を確認する。
    if (!ReadString(data, "enemyType", "").empty())
        return Category::kEnemy;
    // 直下にギミック種別があれば、ギミックカテゴリへ分類する。
    if (!ReadString(data, "gimmickType", "").empty())
        return Category::kGimmick;
    // 直下にアイテム種別があれば、アイテムカテゴリへ分類する。
    if (!ReadString(data, "itemType", "").empty())
        return Category::kItem;

    // これまでに分類できず、旧形式のparamがオブジェクトで残っている場合は、その中の種別を読む。
    if (data.contains("param") && data["param"].is_object()) {
        const json& param = data["param"];
        // 旧配置で敵種別が入っている場合は、敵カテゴリへ分類する。
        if (!ReadString(param, "enemyType", "").empty())
            return Category::kEnemy;
        // 旧配置でギミック種別が入っている場合は、ギミックカテゴリへ分類する。
        if (!ReadString(param, "gimmickType", "").empty())
            return Category::kGimmick;
        // 旧配置でアイテム種別が入っている場合は、アイテムカテゴリへ分類する。
        if (!ReadString(param, "itemType", "").empty())
            return Category::kItem;
    }

    // 種別を判定できないプリセットは、通常のモデルとして扱う。
    return Category::kModel;
}

const char* PresetEditor::GetCategoryLabel(Category category) const {
    // 未知の値は一覧で空欄にせず、全件カテゴリの表示名へ戻す。
    switch (category) {
    case Category::kStage1: return "ステージ別";

    case Category::kEnemy: return "敵";

    case Category::kGimmick: return "ギミック";

    case Category::kItem: return "アイテム";

    case Category::kModel: return "モデル";

    case Category::kAll:
    default: return "すべて";
    }
}

ImVec4 PresetEditor::GetCategoryColor(Category category) const {
    // カテゴリごとの識別色を返し、未知の値には中立色を使う。
    switch (category) {
    case Category::kStage1: return ImVec4(0.16f, 0.76f, 0.88f, 1.0f);

    case Category::kEnemy: return ImVec4(0.78f, 0.28f, 0.24f, 1.0f);

    case Category::kGimmick: return ImVec4(0.24f, 0.50f, 0.84f, 1.0f);

    case Category::kItem: return ImVec4(0.28f, 0.66f, 0.38f, 1.0f);

    case Category::kModel: return ImVec4(0.56f, 0.48f, 0.78f, 1.0f);

    case Category::kAll:
    default: return ImVec4(0.40f, 0.42f, 0.46f, 1.0f);
    }
}

ImVec4 PresetEditor::GetStage1SectionColor(std::int32_t sectionOrder) const {
    // セクションの並び番号を100単位の帯で区切り、同じ帯を一覧で同じ色にする。
    if (sectionOrder < 200)
        return ImVec4(0.30f, 0.84f, 0.55f, 1.0f);
    // 200以上300未満は、次のセクション帯として青を使う。
    if (sectionOrder < 300)
        return ImVec4(0.24f, 0.70f, 1.0f, 1.0f);
    // 300以上400未満は、紫のセクション帯にする。
    if (sectionOrder < 400)
        return ImVec4(0.78f, 0.52f, 1.0f, 1.0f);
    // 400以上500未満は、黄色のセクション帯にする。
    if (sectionOrder < 500)
        return ImVec4(1.0f, 0.76f, 0.24f, 1.0f);
    return ImVec4(0.60f, 0.78f, 0.50f, 1.0f);
}

std::string PresetEditor::ShortModelName(const std::string& modelName) const {
    // リスト表示では最後のパス要素だけ見せ、保存している元の参照文字列は変えない。
    if (modelName.empty()) {
        return "(なし)";
    }

    size_t slash = modelName.find_last_of("/\\");

    // ディレクトリ区切りが無い名前は、その文字列を表示する。
    if (slash == std::string::npos) {
        return modelName;
    }

    return modelName.substr(slash + 1);
}

std::string PresetEditor::ReadString(const json& data, const char* key, const std::string& defaultValue) const {
    // キーがあり文字列型の場合に限り、既定値を置き換える。
    if (data.contains(key) && data[key].is_string()) {
        return data[key].get<std::string>();
    }

    return defaultValue;
}

float PresetEditor::ReadFloat(const json& data, const char* key, float defaultValue) const {
    // キーがありJSONの数値として保存されている場合だけ取り出す。
    if (data.contains(key) && data[key].is_number()) {
        return data[key].get<float>();
    }

    return defaultValue;
}

std::int32_t PresetEditor::ReadInt(const json& data, const char* key, std::int32_t defaultValue) const {
    // 古いJSONの数値表現が浮動小数でも、整数設定として読めるようにする。
    if (data.contains(key) && data[key].is_number_integer()) {
        return data[key].get<std::int32_t>();
    }

    // 整数形式でなくても、小数を含む数値なら旧形式として整数へ変換する。
    if (data.contains(key) && data[key].is_number()) {
        // 旧形式の小数は四捨五入せず、整数変換で小数部を切り捨てる。
        return static_cast<std::int32_t>(data[key].get<float>());
    }

    return defaultValue;
}

bool PresetEditor::ReadBool(const json& data, const char* key, bool defaultValue) const {
    // キーがあり真偽値として保存されている場合だけその値を返す。
    if (data.contains(key) && data[key].is_boolean()) {
        return data[key].get<bool>();
    }

    return defaultValue;
}

engine::Vector3 PresetEditor::ReadVector3(const json& data, const char* key, const engine::Vector3& defaultValue) const {
    // 数値でない要素だけ既定値を保ち、部分的に壊れた配置値も読めるようにする。
    engine::Vector3 value = defaultValue;

    // 配列が三成分以上ある場合だけ、既定値からの上書きを調べる。
    if (data.contains(key) && data[key].is_array() && data[key].size() >= 3) {
        // X成分は数値の場合だけ置き換える。
        if (data[key][0].is_number())
            value.x = data[key][0].get<float>();
        // Y成分は数値の場合だけ置き換える。
        if (data[key][1].is_number())
            value.y = data[key][1].get<float>();
        // Z成分は数値の場合だけ置き換える。
        if (data[key][2].is_number())
            value.z = data[key][2].get<float>();
    }

    return value;
}

std::string PresetEditor::ToLower(const std::string& value) const {
    std::string result = value;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
        });
    return result;
}

std::vector<PresetEditor::TypeOption> PresetEditor::GetEnemyOptions() const {
    // Preset作成UIに出す敵種と、Factoryへ渡す識別子を組で定義する。
    return {
        { "FireSlime", "Fire Slime" },
        { "ThunderSlime", "Thunder Slime" },
        { "WindSlime", "Wind Slime" },
        { "Slime", "スライム" },
        { "Bomb", "ボム" },
        { "Bomber", "ボマー" },
        { "GiantSlime", "巨大スライム" }
    };
}

std::vector<PresetEditor::TypeOption> PresetEditor::GetGimmickOptions() const {
    std::vector<TypeOption> options;
    // 文字列はEditorの登録一覧が所有し、この候補を描画している間は保持されます。
    for (const auto& type : EditorObjectFactoryRegistry::GetTypes(EditorObjectFactoryRegistry::Kind::kGimmick))
        options.push_back({type.id.c_str(), type.displayName.c_str()});
    return options;
}


#endif
