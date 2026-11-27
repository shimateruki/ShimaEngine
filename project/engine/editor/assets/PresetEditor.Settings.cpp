#include <cstdint>
#include "PresetEditor.h"

#include "ModelManager.h"
#include "EditorObjectFactoryRegistry.h"
#include "EditorPropertyDrawer.h"
#include "EditorPropertyJsonBinding.h"
#include "SceneManager.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <utility>

#ifdef USE_IMGUI

// 敵の種類を編集する。基礎ステータスの入力欄は、敵の調整・攻撃プレビューが担当する。
bool PresetEditor::DrawEnemySettings(json& data) {
    // 敵の共通ステータスは別画面で管理し、ここでは種類の選択だけ扱います。
    bool changed = false;
    ApplyHiddenCategoryFields(data, Category::kEnemy);
    json& param = EnsureParam(data);

    // 入力欄が開いているときに、敵の種類と基礎ステータスの編集先を表示する。
    if (ImGui::CollapsingHeader("敵パラメータ", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= DrawTypeCombo("敵の種類", data, param, "enemyType", GetEnemyOptions(), Category::kEnemy);
        ImGui::TextDisabled("HP・攻撃力・移動・重力などは「敵の調整・攻撃プレビュー」の「基本ステータス」で編集します。");
    }

    return changed;
}

bool PresetEditor::RemoveManagedEnemyFields(json& data) {
    // ステータス管理と二重定義になる旧フィールドだけをPresetから取り除きます。
    bool changed = false;
    changed |= data.erase("modelName") > 0;
    changed |= data.erase("scale") > 0;

    // param が無いかオブジェクトでない場合は、最上位の2項目だけの削除結果を返す。
    if (!data.contains("param") || !data["param"].is_object()) {
        return changed;
    }

    json& param = data["param"];
    const char* managedKeys[] = {
        "hp", "maxHp", "attackPower", "speed", "gravity", "maxFallSpeed",
        "jumpPower", "detectionRange", "morphLimited", "morphDuration", "interval"
    };

    for (const char* key : managedKeys) {
        changed |= param.erase(key) > 0;
    }

    return changed;
}

PresetEditor::PropertyEditorContext* PresetEditor::GetPropertyEditor(const std::string& type, bool commonObject) {
    // 同じ型の一時 Object を使い回す。接触イベント用の汎用 Object とギミックの実体はキャッシュを分ける。
    const std::string cacheKey = (commonObject ? "Object/" : "Gimmick/") + type;
    const auto found = propertyEditors_.find(cacheKey);
    // 同じ型の編集個体があれば、生成時の初期値とともに再利用する。
    if (found != propertyEditors_.end()) return &found->second;
    auto* scene = SceneManager::GetInstance()->GetCurrentScene();
    // 現在のシーンや共通描画基盤が使えない場合は、一時個体を生成できない。
    if (!scene || !scene->GetObject3DCommon()) return nullptr;
    std::unique_ptr<engine::Object3D> object;
    // 接触イベント用ならゲーム固有の型を必要としないため、基底 Object3D を用意する。
    if (commonObject) {
        object = std::make_unique<engine::Object3D>();
        object->Initialize(scene->GetObject3DCommon());
    // ギミックでは型固有の初期値が必要なため、Factory から実体を生成する。
    } else {
        object = EditorObjectFactoryRegistry::Create(EditorObjectFactoryRegistry::Kind::kGimmick, type, scene->GetObject3DCommon());
    }
    // 指定の型を生成できなければ、値を編集するコンテキストは登録しない。
    if (!object) return nullptr;
    PropertyEditorContext context;
    // 初期状態を保存し、別プリセットへ切り替えたときに前回の値を引き継がないようにする。
    context.defaults = object->ExportToJson();
    context.object = std::move(object);
    return &propertyEditors_.emplace(cacheKey, std::move(context)).first->second;
}

// ギミックの種類を選び直せるようにし、現在の種類に登録された項目だけを編集する。
bool PresetEditor::DrawGimmickSettings(json& data) {
    // 入力欄が閉じているときはプリセットに触らない。
    if (!ImGui::CollapsingHeader("ギミックパラメータ", ImGuiTreeNodeFlags_DefaultOpen)) return false;
    bool changed = false;
    // 種類を変更した時だけ初期値を入れ、閲覧だけで既存Presetを補完しません。
    const auto options = GetGimmickOptions();
    const auto readType = [&]() {
        const auto param = data.find("param");
        return ReadString(data, "gimmickType", param != data.end() && param->is_object()
            ? ReadString(*param, "gimmickType", "") : std::string());
    };
    const std::string type = readType();
    const auto current = std::find_if(options.begin(), options.end(), [&](const TypeOption& option) { return type == option.value; });
    const char* preview = current != options.end() ? current->label : type.c_str();
    // 選択欄を開いた場合に、登録されている型を並べる。
    if (ImGui::BeginCombo("ギミックの種類", preview)) {
        for (const auto& option : options) {
            // 選択操作があったときは型とそれに対応する初期値を JSON へ反映する。
            if (ImGui::Selectable(option.label, type == option.value)) {
                data["gimmickType"] = option.value;
                ApplyTypeDefaults(data, Category::kGimmick, option.value);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    const std::string selected = readType();
    auto* context = GetPropertyEditor(selected);
    // 編集個体が得られない種類では型固有値の欄を出さず、元の値を保つ。
    if (!context) {
        ImGui::TextDisabled("未登録の種類です。保存済みの値は保持します。");
        return changed;
    }
    auto& object = *context->object;
    // 前のPresetの省略値が混ざらないよう、Factoryの初期値から現在のJSONを復元します。
    if (context->source != data) {
        editorPropertyJsonBinding::Restore(object, context->defaults, data);
        context->source = data;
    }
    auto* registry = EditorPropertyRegistry::GetInstance();
    for (const auto* component : registry->GetComponentsForObject(&object)) {
        // ゲーム固有の Game_ コンポーネントだけを、この種類別の設定へ出す。
        if (component->typeId.rfind("Game_", 0) != 0) continue;
        changed |= DrawRegisteredSettings(data, *context, component->typeId);
    }
    context->source = data;
    return changed;
}


// ギミックとイベントで同じ入力・保存差分処理を使い、未知のJSON項目を保持します。
bool PresetEditor::DrawRegisteredSettings(json& data, PropertyEditorContext& context, const std::string& componentType) {
    auto& object = *context.object;
    // キャッシュ内の復元元と現在の JSON が異なる場合は、初期値を基準に復元し直す。
    if (context.source != data) {
        editorPropertyJsonBinding::Restore(object, context.defaults, data);
        context.source = data;
    }
    auto* registry = EditorPropertyRegistry::GetInstance();
    const auto* component = registry->FindComponent(componentType);
    // 登録されていないコンポーネントの入力欄は描けないため、未変更で戻す。
    if (!component) return false;
    bool changed = false;
    // 定義に説明があれば、個々の入力欄を表示する前に用途を示す。
    if (!component->description.empty()) ImGui::TextWrapped("%s", component->description.c_str());
    for (const auto* property : registry->GetPropertiesForComponent(componentType)) {
        // 対象の型では利用できないプロパティや、現在の値では隠す項目を入力欄から除く。
        if (!registry->IsApplicable(&object, property->path) ||
            (property->visible && !property->visible(object))) continue;
        ImGui::PushID(property->path.c_str());
        ImGui::TextUnformatted(property->displayName.c_str());
        auto value = property->displayValue ? property->displayValue(object) : registry->GetValue(&object, property->path);
        // 現在の Object に応じた上限は入力欄のコピーへ入れ、登録済みの共通定義を変更しない。
        auto effective = *property;
        // 動的な上限が定義されている項目だけ、対象個体から上限を計算する。
        if (property->maximum) effective.ui.maxValue = property->maximum(object);
        const bool disabled = HasEditorPropertyFlag(property->flags, EditorPropertyFlags::kReadOnly) ||
            (property->enabled && !property->enabled(object));
        // 読み取り専用の項目は値とヘルプを表示し、入力だけを止める。
        ImGui::BeginDisabled(disabled);
        EditorPropertyDrawOptions drawOptions;
        drawOptions.compact = true;
        // 入力値が変更されたときだけ、個体と JSON を同じプロパティ経由で更新する。
        if (editorPropertyDrawer::DrawValue(effective, value, "##Value", drawOptions))
            changed |= editorPropertyJsonBinding::Apply(object, *property, data, value);
        ImGui::EndDisabled();
        // ヘルプの生成処理が登録されている項目は、現在の値を使って補足を作る。
        if (property->help) {
            const auto help = property->help(object);
            // 補足が空なら表示領域を増やさない。
            if (!help.empty()) ImGui::TextWrapped("%s", help.c_str());
        }
        ImGui::PopID();
    }
    context.source = data;
    return changed;
}

bool PresetEditor::DrawEventSettings(json& data) {
    auto* context = GetPropertyEditor("ContactEvent", true);
    // イベント用の一時個体が準備できない間は、編集開始の条件を表示して戻る。
    if (!context) {
        ImGui::TextDisabled("シーンの初期化後にイベントを編集できます。");
        return false;
    }
    return DrawRegisteredSettings(data, *context, "ContactEvent");
}

bool PresetEditor::DrawTypeCombo(const char* label, json& data, json& param, const char* key, const std::vector<TypeOption>& options, Category category) {
    // 旧JSONで型がparam内にある場合も読み、変更時は両方の参照先を揃えます。
    std::string current = ReadString(data, key, ReadString(param, key, ""));
    std::int32_t currentIndex = 0;

    for (std::int32_t i = 0; i < static_cast<std::int32_t>(options.size()); ++i) {
        // 保存済みの型と一致した候補があれば、選択欄の初期位置に使う。
        if (current == options[i].value) {
            currentIndex = i;
            break;
        }
    }

    std::vector<const char*> labels;
    labels.reserve(options.size());

    for (const TypeOption& option : options) {
        labels.push_back(option.label);
    }

    // 選択が変わった場合は、旧形式の参照先も同期し、新しい型の初期項目を設定する。
    if (ImGui::Combo(label, &currentIndex, labels.data(), static_cast<std::int32_t>(labels.size()))) {
        data[key] = options[currentIndex].value;
        param[key] = options[currentIndex].value;
        ApplyHiddenCategoryFields(data, category);
        ApplyTypeDefaults(data, category, options[currentIndex].value);
        return true;
    }

    return false;
}

// 編集カテゴリを、生成時に読む型と保存先のカテゴリへ対応づける。
void PresetEditor::ApplyHiddenCategoryFields(json& data, Category category) {
    // 敵は型・保存カテゴリともに Enemy を使う。
    if (category == Category::kEnemy) {
        data["type"] = "Enemy";
        data["saveCategory"] = "Enemy";
    }
    // ギミックは Gimmick 型として生成し、汎用 Object カテゴリへ保存する。
    else if (category == Category::kGimmick) {
        data["type"] = "Gimmick";
        data["saveCategory"] = "Object";
    }
    // アイテムは Item 型として生成し、汎用 Object カテゴリへ保存する。
    else if (category == Category::kItem) {
        data["type"] = "Item";
        data["saveCategory"] = "Object";
    }
    else {
        // EffectやInvisibleBoxの数値編集で、種別をModelへ変えてしまわないようにします。
        if (!data.contains("type") || ReadString(data, "type", "").empty())
            data["type"] = "Model";
        data["saveCategory"] = "Object";
    }
}

// 型を切り替えた直後に、当たり判定や型固有値の初期設定を入れる。
void PresetEditor::ApplyTypeDefaults(json& data, Category category, const std::string& type) {
    // 敵のモデルや基礎ステータスはそのまま編集せず、初期 Collider を決める。
    if (category == Category::kEnemy) {
        // 大型の敵だけは生成時の当たり判定の寸法を通常より広く取る。
        if (type == "GiantSlime") {
            SetColliderDefaults(data, 1, { 2.2f, 2.2f, 2.2f });
        }
        // 巨大スライム以外の敵は通常寸法の球で初期化する。
        else {
            SetColliderDefaults(data, 1, { 1.0f, 1.0f, 1.0f });
        }
    }
    // ギミックの型変更では、Factory で生成した個体の定義を初期値として使う。
    else if (category == Category::kGimmick) {
        // 編集個体を取得できた場合だけ、許可したキーを既定値で置き換える。
        if (const auto* context = GetPropertyEditor(type)) {
            const auto& defaults = context->defaults;
            for (const char* key : {"param", "modelName", "scale", "color", "materialType", "blendMode", "emissive",
                 "metallic", "roughness", "collider", "collisionAttribute", "collisionMask", "collisionAttributeIds", "collisionMaskIds"})
                // 見た目と当たり判定の初期項目だけ型の初期値から採用し、プリセット名や配置情報を維持する。
                if (defaults.contains(key)) data[key] = defaults[key];
            data["gimmickType"] = type;
            EnsureParam(data)["gimmickType"] = type;
        }
    }

    // ここで作った数値Maskは現在のJSON定義の値なので、IDも同時に保存します。
    if (data.contains("collisionAttribute") && data["collisionAttribute"].is_number_unsigned()) {
        data["collisionAttributeIds"] = engine::collisionCategories::GetIds(data["collisionAttribute"].get<uint32_t>());
    }

    // 衝突相手の数値マスクがあれば、相手カテゴリの ID も保存する。
    if (data.contains("collisionMask") && data["collisionMask"].is_number_unsigned()) {
        data["collisionMaskIds"] = engine::collisionCategories::GetIds(data["collisionMask"].get<uint32_t>());
    }
}

void PresetEditor::SetColliderDefaults(json& data, std::int32_t type, const engine::Vector3& size) {
    // 形状と大きさだけ更新し、調整済みの中心・回転は残します。
    if (!data.contains("collider") || !data["collider"].is_object()) {
        data["collider"] = json::object();
    }

    data["collider"]["type"] = type;
    data["collider"]["size"] = { size.x, size.y, size.z };

    // 中心が未定義の場合は、モデル原点を使う。
    if (!data["collider"].contains("center"))
        data["collider"]["center"] = { 0.0f, 0.0f, 0.0f };
    // 回転が未定義の場合は、ローカル軸に揃える。
    if (!data["collider"].contains("rotation"))
        data["collider"]["rotation"] = { 0.0f, 0.0f, 0.0f };
}

bool PresetEditor::DrawModelField(json& data) {
    // 表示名ではなくモデルの参照名をJSONへ保存します。
    bool changed = false;
    std::string current = ReadString(data, "modelName", "");

    // モデル選択欄を開いた場合は、キャッシュ済みのモデルを候補として表示する。
    if (ImGui::BeginCombo("モデル", current.empty() ? "(なし)" : current.c_str())) {
        for (const std::string& modelName : modelNames_) {
            bool selected = modelName == current;

            // 選んだモデルの参照名を保存し、呼出元へ変更を返す。
            if (ImGui::Selectable(modelName.c_str(), selected)) {
                data["modelName"] = modelName;
                changed = true;
            }

            // 現在のモデルへ既定フォーカスを合わせ、候補が多くても見つけやすくする。
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }

    changed |= DrawStringField(data, "モデル名", "modelName", "");
    return changed;
}

bool PresetEditor::DrawCollider(json& data) {
    // colliderが欠けた旧Presetも編集できるよう、初回表示時に空オブジェクトを補います。
    bool changed = false;

    // 欠落・旧形式の値は、編集項目を書ける空のオブジェクトへ差し替える。
    if (!data.contains("collider") || !data["collider"].is_object()) {
        data["collider"] = json::object();
        changed = true;
    }

    json& collider = data["collider"];
    std::int32_t type = ReadInt(collider, "type", 0);
    const char* labels[] = { "なし", "球", "AABB", "OBB" };

    // 形状を選び直した場合は、その列挙番号を保存する。
    if (ImGui::Combo("形状", &type, labels, 4)) {
        collider["type"] = type;
        changed = true;
    }

    changed |= DrawVector3Field(collider, "中心", "center", { 0.0f, 0.0f, 0.0f }, 0.05f);
    changed |= DrawVector3Field(collider, "サイズ", "size", { 1.0f, 1.0f, 1.0f }, 0.05f);
    changed |= DrawVector3Field(collider, "回転", "rotation", { 0.0f, 0.0f, 0.0f }, 0.01f);
    return changed;
}

bool PresetEditor::DrawStringField(json& data, const char* label, const char* key, const std::string& defaultValue) {
    // 入力が変わった時だけJSONを更新し、未編集値には既定値を使います。
    char buffer[256]{};
    std::string value = ReadString(data, key, defaultValue);
    strncpy_s(buffer, value.c_str(), _TRUNCATE);

    // 入力が変更された場合にだけ、バッファーの内容を JSON へ移す。
    if (ImGui::InputText(label, buffer, sizeof(buffer))) {
        data[key] = std::string(buffer);
        return true;
    }

    return false;
}

bool PresetEditor::DrawFloatField(json& data, const char* label, const char* key, float defaultValue, float speed) {
    // Drag操作が確定した値だけ保存し、呼び出し側へ変更を伝えます。
    float value = ReadFloat(data, key, defaultValue);

    // 数値が変更された場合だけ保存し、未編集なら欠損項目を新設しない。
    if (ImGui::DragFloat(label, &value, speed)) {
        data[key] = value;
        return true;
    }

    return false;
}

bool PresetEditor::DrawIntField(json& data, const char* label, const char* key, std::int32_t defaultValue) {
    // 整数パラメータも共通の変更フラグで保存処理へ渡します。
    std::int32_t value = ReadInt(data, key, defaultValue);

    // 整数値が変化した場合だけ、JSON の値と変更フラグを更新する。
    if (ImGui::DragInt(label, &value, 1.0f)) {
        data[key] = value;
        return true;
    }

    return false;
}

bool PresetEditor::DrawColorField(json& data, const char* label, const char* key, const engine::Vector4& defaultValue) {
    // 壊れた配列要素は既定色を保ち、編集時にRGBA配列へ正規化します。
    float color[4] = { defaultValue.x, defaultValue.y, defaultValue.z, defaultValue.w };

    // 保存値が4要素以上の配列として読める場合に、先頭4成分を使う。
    if (data.contains(key) && data[key].is_array() && data[key].size() >= 4) {
        for (std::int32_t i = 0; i < 4; ++i) {
            // 数値の成分だけ採用し、型が異なる成分は既定値にする。
            if (data[key][i].is_number()) {
                color[i] = data[key][i].get<float>();
            }
        }
    }

    // 色を編集した場合は、四成分の RGBA をまとめて保存する。
    if (ImGui::ColorEdit4(label, color)) {
        data[key] = { color[0], color[1], color[2], color[3] };
        return true;
    }

    return false;
}

bool PresetEditor::DrawVector3Field(json& data, const char* label, const char* key, const engine::Vector3& defaultValue, float speed) {
    engine::Vector3 value = ReadVector3(data, key, defaultValue);
    float values[3] = { value.x, value.y, value.z };

    // 編集された場合だけ3成分を保存する。回転項目では後続で旧クォータニオンも整理する。
    if (ImGui::DragFloat3(label, values, speed)) {
        data[key] = { values[0], values[1], values[2] };

        // 保存されたQuaternionより、今回編集したEuler角を優先します。
        if (std::string(key) == "rotate")
            data.erase("quaternion");
        return true;
    }

    return false;
}

bool PresetEditor::DrawParamFloat(json& param, const char* label, const char* key, float defaultValue, float speed) {
    // param直下の浮動小数点設定も通常フィールドと同じ更新経路へ渡します。
    return DrawFloatField(param, label, key, defaultValue, speed);
}

json& PresetEditor::EnsureParam(json& data) {
    // 旧Presetでparamが欠けていても、型別の編集先を必ずObjectにします。
    if (!data.contains("param") || !data["param"].is_object()) {
        data["param"] = json::object();
    }

    return data["param"];
}

void PresetEditor::AddBlankPreset(Category category) {
    // 新規Presetを保存して即選択し、続けて同じ画面から調整できる状態にします。
    if (newName_[0] == '\0') {
        return;
    }

    std::string name = newName_;
    json data = BuildBlankPreset(name, category);
    PresetManager::GetInstance()->GetPreset(name) = data;
    PresetManager::GetInstance()->SaveAll();
    selectedName_ = name;
    newName_[0] = '\0';
}

json PresetEditor::BuildBlankPreset(const std::string& name, Category category) const {
    // 敵はモデルと倍率をステータス側で決めるため、他カテゴリと初期項目を分けます。
    json data = json::object();
    data["name"] = name;
    data["presetDisplayName"] = name;
    data["presetUsage"] = category == Category::kModel ? "装飾" : GetCategoryLabel(category);

    // 敵以外は配置時に見えるよう、ブロックモデルを暫定で指定する。
    if (category != Category::kEnemy) {
        data["modelName"] = "Stages/block";
    }

    data["translate"] = { 0.0f, 0.0f, 0.0f };
    data["rotate"] = { 0.0f, 0.0f, 0.0f };

    // 敵以外は単位倍率から始める。敵の倍率はステータスの定義を使う。
    if (category != Category::kEnemy) {
        data["scale"] = { 1.0f, 1.0f, 1.0f };
    }

    data["color"] = { 1.0f, 1.0f, 1.0f, 1.0f };
    data["materialType"] = 0;

    // 描画側の初期値と一致させ、作成直後からPBR設定をJSON上で確認できるようにします。
    data["metallic"] = 0.0f;
    data["roughness"] = 0.5f;
    data["emissive"] = 1.0f;
    data["enableLighting"] = true;
    data["enableEnvMap"] = false;
    data["envIntensity"] = 1.0f;
    data["enableNormalMap"] = false;
    data["myEventID"] = -1;
    data["targetID"] = -1;
    data["eventType"] = 0;
    data["collisionAttribute"] = 0;
    data["collisionMask"] = 0;
    data["enemyType"] = category == Category::kEnemy ? "Slime" : "";
    data["gimmickType"] = category == Category::kGimmick ? "Default" : "";
    data["itemType"] = "";
    data["collider"] = {
        { "type", 2 },
        { "center", { 0.0f, 0.0f, 0.0f } },
        { "size", { 1.0f, 1.0f, 1.0f } },
        { "rotation", { 0.0f, 0.0f, 0.0f } }
    };

    // 敵の param は型だけ保存し、基礎ステータスを二重に定義しない。
    if (category == Category::kEnemy) {
        data["param"] = { { "enemyType", "Slime" } };
    // 敵以外は共通の初期パラメーターを用意してから、カテゴリごとの上書きを適用する。
    } else {
        data["param"] = {
            { "speed", 1.0f },
            { "gravity", 50.0f },
            { "interval", 3.0f },
            { "gimmickType", category == Category::kGimmick ? "Default" : "" },
            { "itemType", "" }
        };
    }

    const_cast<PresetEditor*>(this)->ApplyHiddenCategoryFields(data, category);
    const std::string type =
        category == Category::kEnemy ? "Slime" :
        category == Category::kGimmick ? "Default" :
        "";
    const_cast<PresetEditor*>(this)->ApplyTypeDefaults(data, category, type);
    return data;
}

#endif
