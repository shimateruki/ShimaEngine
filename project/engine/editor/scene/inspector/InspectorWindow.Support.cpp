#include <cstdint>
#include "engine/editor/scene/inspector/InspectorWindow.Internal.h"
#include "engine/graphics/3d/material/ShaderMaterialRegistry.h"

#ifdef USE_IMGUI
#include "engine/editor/cinematic/GhostDirector.h"

using namespace inspectorWindowImplementation;

// Inspectorの共通処理。複数選択、RegistryのComponent、カメラ、Prefab、共有材質、デカールを扱う。
namespace inspectorWindowImplementation {
// 検索・比較用にコピーを小文字化し、元の表示名は変更しない。
std::string ToLowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

std::vector<engine::Object3D*> CollectInspectorTargets(DebugEditor* editor, engine::Object3D* primary) {
    // 複数選択の重複を除き、主選択Objectが選択一覧から漏れても編集対象へ含める。
    std::vector<engine::Object3D*> targets;

    // 主選択がなければ、複数選択も編集対象にしない。
    if (!primary) {
        return targets;
    }

    const std::vector<engine::Object3D*>& selectedObjects = editor->GetSelectedObjects();

    // 複数選択の一覧が空の場合は、主選択だけを返す。
    if (selectedObjects.empty()) {
        targets.push_back(primary);
        return targets;
    }

    targets.reserve(selectedObjects.size());

    // 空の参照と既出の参照を除き、残った対象は選択一覧の順序を維持する。
    for (engine::Object3D* object : selectedObjects) {
        if (!object) {
            continue;
        }

        if (std::find(targets.begin(), targets.end(), object) != targets.end()) {
            continue;
        }

        targets.push_back(object);
    }

    // 主選択が一覧になかった場合は、最後に補う。
    if (std::find(targets.begin(), targets.end(), primary) == targets.end()) {
        targets.push_back(primary);
    }

    return targets;
}

void MarkInspectorTargetsDirty(DebugEditor* editor, const std::vector<engine::Object3D*>& targets) {
    // Editorを取得できた場合に、有効な参照のObjectを保存上の変更対象にする。
    if (!editor) {
        return;
    }

    for (engine::Object3D* object : targets) {
        if (!object) {
            continue;
        }

        editor->MarkDirtyForObject(object);
    }
}

// 参照が存在する各対象のローカル行列を作り直してから、親の変換を合成する。
void RefreshInspectorTargetMatrices(const std::vector<engine::Object3D*>& targets) {
    for (engine::Object3D* object : targets) {
        if (!object) {
            continue;
        }

        // 親の変換を合成する前に、このObjectの編集済みTransformからローカル行列を作り直します。
        object->UpdateLocalMatrix();
        object->UpdateWorldMatrix();
    }
}

// 元の倍率がほぼゼロの場合は拡縮比1を返し、ゼロ除算を避けます。
float SafeScaleRatio(float after, float before) {
    if (std::fabs(before) < 0.0001f) {
        return 1.0f;
    }

    return after / before;
}

// 主選択の移動・回転・拡縮差分を、他の選択Objectにも反映します。
void ApplyPrimaryTransformDeltaToTargets(
    const std::vector<engine::Object3D*>& targets,
    engine::Object3D* primary,
    const engine::Vector3& beforePosition,
    const engine::Vector3& afterPosition,
    const engine::Vector3& beforeRotation,
    const engine::Vector3& afterRotation,
    const engine::Vector3& beforeScale,
    const engine::Vector3& afterScale,
    bool applyPosition,
    bool applyRotation,
    bool applyScale) {

    // 主選択がなければ差分を反映する基準がない。
    if (!primary) {
        return;
    }

    // 他の対象の相対配置を保つため、主選択の変更前後から位置・回転の差と拡縮率を求める。
    const engine::Vector3 positionDelta = {
        afterPosition.x - beforePosition.x,
        afterPosition.y - beforePosition.y,
        afterPosition.z - beforePosition.z,
    };

    const engine::Vector3 rotationDelta = {
        afterRotation.x - beforeRotation.x,
        afterRotation.y - beforeRotation.y,
        afterRotation.z - beforeRotation.z,
    };

    // 位置・回転は加算差分、拡縮は倍率です。各対象の元のサイズ差を保ったまま同じ倍率を反映します。
    const engine::Vector3 scaleRatio = {
        SafeScaleRatio(afterScale.x, beforeScale.x),
        SafeScaleRatio(afterScale.y, beforeScale.y),
        SafeScaleRatio(afterScale.z, beforeScale.z),
    };

    // 主選択は既にUIで変更済みなので、それ以外だけローカルTransformへ差分を適用する。
    for (engine::Object3D* object : targets) {
        if (!object || object == primary) {
            continue;
        }

        Transform transformValue = *object->GetTransform();
        Transform* transform = &transformValue;

        // 直前にローカル変数のアドレスを入れているため、この空チェックで抜ける経路は現状ない。
        if (!transform) {
            continue;
        }

        // 位置欄を変更した場合だけ、同じローカル座標の差分を足す。
        if (applyPosition) {
            transform->translate.x += positionDelta.x;
            transform->translate.y += positionDelta.y;
            transform->translate.z += positionDelta.z;
        }

        // 回転欄を変更した場合は、ラジアンのオイラー角差分を足し、オイラー角から姿勢を再計算できるようにする。
        if (applyRotation) {
            transform->rotate.x += rotationDelta.x;
            transform->rotate.y += rotationDelta.y;
            transform->rotate.z += rotationDelta.z;
            transform->isQuaternionMaster = false;
        }

        // 拡縮欄を変えた場合は、各軸に主選択の変更前後と同じ倍率を掛ける。
        if (applyScale) {
            transform->scale.x *= scaleRatio.x;
            transform->scale.y *= scaleRatio.y;
            transform->scale.z *= scaleRatio.z;
        }

        object->SetLocalTransform(transformValue);
    }
}

// Planeモデル、Sprite画像パス、または名前の識別子が2.5D表示を示す場合にtrueを返す。
bool IsSpriteCardObject(const engine::Object3D* object) {
    if (!object) {
        return false;
    }

    const std::string modelName = object->GetModelName();
    const std::string texturePath = object->GetTexturePath();
    const std::string name = object->GetName();
    return modelName == "Primitives/plane" ||
        texturePath.find("Resources/sprite/") == 0 ||
        name.find("SpriteCard") != std::string::npos ||
        name.find("2.5D") != std::string::npos;
}

// Sprite Cardの表示に必要な描画・衝突設定をObjectへ適用します。
void ConfigureSpriteCardObject(engine::Object3D* object) {
    // 対象の参照がなければ描画や衝突の設定を変更できない。
    if (!object) {
        return;
    }

    // PlaneへSpriteテクスチャを貼る2.5D表示として、描画と衝突の既定値を揃える。
    object->SetClassName("Model");
    object->SetModel("Primitives/plane");

    // 空の名前や汎用の初期名だけ、Sprite Cardの既定名へ変更する。
    if (object->GetName().empty() || object->GetName() == "Object") {
        object->SetName("SpriteCard_2_5D");
    }

    // 画像が未指定なら、配置した形が確認できる白画像を設定する。
    if (object->GetTexturePath().empty()) {
        object->SetTexture("Resources/sprite/common/white.png");
    }

    // 色・透過・発光を設定し、モデル用の法線・照明や接触判定を切る。
    object->SetMaterialType(0);
    object->SetBlendMode(BlendMode::kNormal);
    object->SetColor({ 1.0f, 1.0f, 1.0f, 1.0f });
    object->SetEnableNormalMap(false);
    object->SetNormalMap("");
    object->SetOrmMap("");
    object->SetEnableEnvMap(false);
    object->SetEnableLighting(false);
    object->SetEmissive(1.35f);
    object->SetTextureTiling({ 1.0f, 1.0f });
    object->SetAutoTextureTiling(false);
    object->SetCollisionAttribute(0);
    object->SetCollisionMask(0);

    engine::Object3D::ColliderConfig colliderConfig = object->GetColliderConfig();
    colliderConfig.type = engine::ColliderType::kNone;
    object->SetColliderConfig(colliderConfig);
}

void ApplyRegisteredComponentToTargets(
    DebugEditor* editor,
    const std::vector<engine::Object3D*>& targets,
    const std::string& componentTypeId,
    bool add) {
    // 有効な選択先へ登録済みの追加または削除操作を適用し、少なくとも一件の変更があれば保存と行列を更新する。
    EditorPropertyRegistry* registry = EditorPropertyRegistry::GetInstance();
    bool changed = false;

    for (engine::Object3D* target : targets) {
        if (!target) {
            continue;
        }

        changed |= add
            ? registry->AddComponent(target, componentTypeId)
            : registry->RemoveComponent(target, componentTypeId);
    }

    if (changed) {
        MarkInspectorTargetsDirty(editor, targets);
        RefreshInspectorTargetMatrices(targets);
    }
}

// 選択対象へComponentプリセットの値をまとめて適用します。
void ApplyRegisteredComponentPresetToTargets(
    DebugEditor* editor,
    const std::vector<engine::Object3D*>& targets,
    const std::string& componentTypeId,
    const std::string& presetId) {
    EditorPropertyRegistry* registry = EditorPropertyRegistry::GetInstance();
    bool changed = false;

    for (engine::Object3D* target : targets) {
        changed |= registry->ApplyComponentPreset(target, componentTypeId, presetId);
    }

    // 一件でもプリセット適用が成立した場合に、選択範囲を変更済みにして行列を更新する。
    if (changed) {
        MarkInspectorTargetsDirty(editor, targets);
        RefreshInspectorTargetMatrices(targets);
    }
}

// 登録済みComponentの有無と編集入口をInspectorの一行へ描画します。
void DrawRegisteredComponentRow(
    DebugEditor* editor,
    const std::vector<engine::Object3D*>& targets,
    const EditorComponentDescriptor& component) {
    ImGui::PushID(component.typeId.c_str());
    ImGui::BeginGroup();
    ImGui::TextUnformatted(component.displayName.c_str());
    // 説明が登録されているComponentだけ、名前の下に説明を描く。
    if (!component.description.empty()) ImGui::TextWrapped("%s", component.description.c_str());
    ImGui::EndGroup();

    // 削除可能で削除処理も登録済みならボタンを出し、対象外は固定表示にする。
    if (component.removable && component.remove) {
        ImGui::SameLine();

        if (ImGui::SmallButton("Remove")) {
            ApplyRegisteredComponentToTargets(editor, targets, component.typeId, false);
        }
    }
    else {
        ImGui::SameLine();
        ImGui::TextDisabled("固定");
    }

    ImGui::Separator();
    ImGui::PopID();
}

// 現在の対象へ追加できるComponentをメニューに列挙します。
void DrawRegisteredAddComponentMenu(
    DebugEditor* editor,
    const std::vector<engine::Object3D*>& targets,
    engine::Object3D* primary) {
    // 追加用のポップアップを開いているときだけ登録一覧を調べる。
    if (!ImGui::BeginPopup("AddRegisteredComponentMenu")) {
        return;
    }

    EditorPropertyRegistry* registry = EditorPropertyRegistry::GetInstance();
    registry->InitializeBuiltInProperties();
    ImGui::TextDisabled("追加するComponent");
    ImGui::Separator();

    // 対象Objectへ適用可能なComponentと、各ComponentのPresetを同じメニューへ列挙する。
    bool hasAddableComponent = false;

    for (const EditorComponentDescriptor* component : registry->GetApplicableComponentsForObject(primary)) {
        // 無効なComponent定義はメニューへ出さない。
        if (!component) {
            continue;
        }

        ImGui::PushID(component->typeId.c_str());

        // 追加処理が登録された行を出し、主選択に既に存在する場合は選べないようにする。
        if (component->add) {
            hasAddableComponent = true;
            const bool present = registry->IsComponentPresent(primary, component->typeId);

            if (ImGui::MenuItem(component->displayName.c_str(), nullptr, false, !present)) {
                ApplyRegisteredComponentToTargets(editor, targets, component->typeId, true);
            }

            // 追加行に説明があり、マウスを乗せた場合だけ補足を表示する。
            if (ImGui::IsItemHovered() && !component->description.empty()) {
                ImGui::SetTooltip("%s", component->description.c_str());
            }
        }

        for (const EditorComponentPresetDescriptor& preset : component->presets) {
            hasAddableComponent = true;
            ImGui::PushID(preset.id.c_str());

            // プリセットを選んだ場合は、現在の編集対象すべてへその設定を適用する。
            if (ImGui::MenuItem(preset.displayName.c_str())) {
                ApplyRegisteredComponentPresetToTargets(
                    editor,
                    targets,
                    component->typeId,
                    preset.id);
            }

            // プリセットに説明がある場合は、行にマウスを乗せると表示する。
            if (ImGui::IsItemHovered() && !preset.description.empty()) {
                ImGui::SetTooltip("%s", preset.description.c_str());
            }

            ImGui::PopID();
        }

        ImGui::PopID();
    }

    // 追加操作もプリセットも登録されていなければ空の理由を案内する。
    if (!hasAddableComponent) {
        ImGui::TextDisabled("追加可能なComponentはありません。");
    }

    ImGui::EndPopup();
}

// Registryで定義されたComponentがObjectに存在するかを返します。
bool HasRegisteredComponent(const engine::Object3D* object, const char* componentTypeId) {
    EditorPropertyRegistry* registry = EditorPropertyRegistry::GetInstance();
    registry->InitializeBuiltInProperties();
    return registry->IsComponentPresent(object, componentTypeId);
}

// 有効な参照のうち、指定のComponentを持つ対象だけ借用して返す。
std::vector<engine::Object3D*> CollectRegisteredComponentTargets(
    const std::vector<engine::Object3D*>& targets,
    const char* componentTypeId) {
    std::vector<engine::Object3D*> result;
    EditorPropertyRegistry* registry = EditorPropertyRegistry::GetInstance();
    registry->InitializeBuiltInProperties();

    for (engine::Object3D* target : targets) {
        if (target && registry->IsComponentPresent(target, componentTypeId)) {
            result.push_back(target);
        }
    }

    return result;
}

bool IsPrefabPropertyOverridden(const engine::Object3D* object, const std::string& propertyPath) {
    // Prefabインスタンスに限って差分一覧を参照し、通常Objectでは上書き扱いにしない。
    if (!object || !object->IsPrefabInstance()) {
        return false;
    }

    for (const auto& entry : PresetManager::GetInstance()->GetPrefabOverrides(object)) {
        // 差分のパスが指定プロパティと一致した時点で上書き済みと判断する。
        if (entry.propertyPath == propertyPath) {
            return true;
        }
    }

    return false;
}

// Registryのプロパティ定義からComponentの編集UIを組み立てます。
void DrawAutomaticComponentProperties(
    DebugEditor* editor,
    engine::Object3D* primary,
    const std::vector<engine::Object3D*>& targets,
    const EditorComponentDescriptor& component) {
    // Editorと主選択があり、編集方法が自動生成に設定されたComponentだけ扱う。
    if (!editor || !primary ||
        component.inspectorMode != EditorComponentInspectorMode::kAutomatic) {
        return;
    }

    EditorPropertyRegistry* registry = EditorPropertyRegistry::GetInstance();
    const std::vector<const EditorPropertyDescriptor*> properties =
        registry->GetPropertiesForComponent(component.typeId);
    // 登録プロパティがなければ自動編集の行を出さない。
    if (properties.empty()) {
        return;
    }

    // 複数選択の中から実際にこのComponentを持つObjectだけを編集対象へ絞る。
    std::vector<engine::Object3D*> componentTargets;
    componentTargets.reserve(targets.size());

    for (engine::Object3D* target : targets) {
        if (!target || (component.applicable && !component.applicable(*target)) ||
            (component.present && !component.present(*target))) {
            continue;
        }

        componentTargets.push_back(target);
    }

    // 複数選択の中に適用可能な対象がいなければ編集欄を開かない。
    if (componentTargets.empty()) {
        return;
    }

    ImGui::Indent();

    // 一部の選択先へしか適用できない場合は、編集対象の件数を案内する。
    if (componentTargets.size() != targets.size()) {
        ImGui::TextDisabled(
            "このComponentを持つ%zu / %zu Objectを編集",
            componentTargets.size(),
            targets.size());
    }

    // 描画領域を確保できた場合に、項目名と現在値を2列で描く。
    if (ImGui::BeginTable(
        ("##AutoComponentProperties_" + component.typeId).c_str(),
        2,
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        // 主選択に適用でき、かつ可視条件を満たすプロパティだけを表示する。
        for (const EditorPropertyDescriptor* property : properties) {
            if (!property || !registry->IsApplicable(primary, property->path) ||
                (property->visible && !property->visible(*primary))) {
                continue;
            }

            ImGui::PushID(property->path.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const bool multiEditable = HasEditorPropertyFlag(property->flags, EditorPropertyFlags::kMultiEdit);
            const bool mixed = multiEditable && registry->HasMixedValue(componentTargets, property->path);
            const bool prefabOverride =
                HasEditorPropertyFlag(property->flags, EditorPropertyFlags::kPrefabOverride) &&
                IsPrefabPropertyOverridden(primary, property->path);
            // Prefab差分を優先して色と印を付ける。混在する値にはその印も併記し、どちらもなければ通常表示。
            if (prefabOverride) {
                ImGui::TextColored(
                    ImVec4(1.0f, 0.63f, 0.20f, 1.0f),
                    mixed ? "%s * —" : "%s *",
                    property->displayName.c_str());
            }
            else if (mixed) {
                ImGui::TextDisabled("%s —", property->displayName.c_str());
            }
            else {
                ImGui::TextUnformatted(property->displayName.c_str());
            }

            // 項目名にマウスを乗せた場合に内部パスを表示し、Prefab差分があればその情報も出す。
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextDisabled("%s", property->path.c_str());

                if (prefabOverride) {
                    ImGui::TextUnformatted("Prefab Override");
                }

                ImGui::EndTooltip();
            }

            ImGui::TableSetColumnIndex(1);
            // 表示用の変換が登録済みならその値を使い、なければRegistryの保存値を読む。読み取り専用か無効な値は編集を止める。
            nlohmann::json value = property->displayValue ? property->displayValue(*primary)
                : registry->GetValue(primary, property->path);
            const bool readOnly = HasEditorPropertyFlag(property->flags, EditorPropertyFlags::kReadOnly) ||
                (property->enabled && !property->enabled(*primary));
            ImGui::BeginDisabled(readOnly);
            EditorPropertyDrawOptions options;
            options.compact = true;
            options.mixed = mixed;
            auto effective = *property;
            // 対象ごとの上限は共有定義を書き換えず、今回の描画用コピーにだけ設定します。
            if (property->maximum) effective.ui.maxValue = property->maximum(*primary);
            const bool changed = editorPropertyDrawer::DrawValue(effective, value, "##Value", options);
            ImGui::EndDisabled();
            // 補助説明を生成する処理があり、現在の対象で結果が空でない場合だけ表示する。
            if (property->help) {
                const auto help = property->help(*primary);
                if (!help.empty()) ImGui::TextWrapped("%s", help.c_str());
            }
            // 有効なシナリオパスがあり、対応するエディターで編集を選んだ場合に、その参照先を開く。
            if (property->ui.assetEditor == EditorPropertyAssetEditor::kScenario && value.is_string() &&
                !value.get_ref<const std::string&>().empty() &&
                ImGui::Button("GhostDirectorで編集") && editor->GetGhostDirector()) {
                editor->GetGhostDirector()->LoadScenario(value.get_ref<const std::string&>());
                EditorManager::GetInstance()->SetSelectedObject(editor->GetGhostDirector());
            }

            // 複数選択でまとめて書けないプロパティには、主選択だけが対象であることを表示する。
            if (!multiEditable && componentTargets.size() > 1) {
                ImGui::SameLine();
                ImGui::TextDisabled("代表のみ");
            }

            // 入力に変更があり読み取り専用でない場合だけ保存値へ書く。非対応・無効な対象と、代表専用時の他の選択先を除く。
            if (changed && !readOnly) {
                for (engine::Object3D* target : componentTargets) {
                    if (!target || (!multiEditable && target != primary) ||
                        !registry->IsApplicable(target, property->path) ||
                        (property->enabled && !property->enabled(*target))) {
                        continue;
                    }

                    registry->SetValue(target, property->path, value);
                }

                MarkInspectorTargetsDirty(editor, componentTargets);
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    ImGui::Unindent();
}

void DrawComponentPanel(DebugEditor* editor, engine::Object3D* primary, const std::vector<engine::Object3D*>& targets) {
    // 共通Component定義を用いて単体・複数選択のInspectorを同じ手順で描画する。
    // 主選択がなければ表示対象のComponentは決められない。
    if (!primary) {
        return;
    }

    ImGui::SeparatorText("Components");
    ImGui::TextDisabled("Object3Dの既存機能をUnity風のComponentとして追加/削除します。");
    ImGui::TextDisabled("Prefab InstanceではRegistry Propertyの差分をOverrideとして追跡します。");

    // 追加ボタンを押した場合は、登録済みの候補を表示するポップアップを開く。
    if (ImGui::Button("Add Component", ImVec2(-1.0f, 30.0f))) {
        ImGui::OpenPopup("AddRegisteredComponentMenu");
    }

    DrawRegisteredAddComponentMenu(editor, targets, primary);

    ImGui::Spacing();
    EditorPropertyRegistry* registry = EditorPropertyRegistry::GetInstance();
    registry->InitializeBuiltInProperties();
    const std::vector<const EditorComponentDescriptor*> registeredComponents =
        registry->GetComponentsForObject(primary);
    bool hasOptionalComponent = false;

    for (const EditorComponentDescriptor* component : registeredComponents) {
        // 有効な定義でInspectorへの表示が許可されているものだけ行を出す。
        if (!component || !component->showInInspector) {
            continue;
        }

        hasOptionalComponent |= component->removable;
        DrawRegisteredComponentRow(editor, targets, *component);
        DrawAutomaticComponentProperties(editor, primary, targets, *component);
    }

    // 表示対象に取り外せるComponentが一つもなければ、追加方法を示す。
    if (!hasOptionalComponent) {
        ImGui::TextDisabled("追加済みの任意Componentはありません。Add Componentから追加できます。");
    }
}

bool DrawSceneObjectNameCombo(const char* label, BaseScene* scene, engine::Object3D* cameraObject, std::string& objectName) {
    // Scene上でカメラ自身と内部専用Objectを除いた注視対象を選ぶ。未選択も含め、項目が選ばれたらtrueを返す。
    bool changed = false;
    const char* preview = objectName.empty() ? "(未選択)" : objectName.c_str();

    // 一覧を開いていないフレームは、候補を走査せず変更なしとして扱う。
    if (!ImGui::BeginCombo(label, preview)) {
        return false;
    }

    // 未選択を選んだら対象名の参照を解除する。
    if (ImGui::Selectable("(未選択)", objectName.empty())) {
        objectName.clear();
        changed = true;
    }

    // シーンがある場合だけ名前の候補を列挙する。空の名前やカメラ自身は候補にしない。
    if (scene) {
        for (const auto& object : scene->GetObjects()) {
            if (!object || object->IsEditorInternal() || object.get() == cameraObject || object->GetName().empty()) {
                continue;
            }

            const bool selected = objectName == object->GetName();

            // 新しく選んだ行の名前を保存用の値へ入れる。
            if (ImGui::Selectable(object->GetName().c_str(), selected)) {
                objectName = object->GetName();
                changed = true;
            }

            // 現在参照している名前の行を、開いたときの既定のフォーカスにする。
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
    }

    ImGui::EndCombo();
    return changed;
}

// 共通プロパティの値が選択対象間で異なるかをRegistryへ問い合わせる。
bool HasMixedInspectorValue(const std::vector<engine::Object3D*>& targets, const char* propertyPath) {
    return EditorPropertyRegistry::GetInstance()->HasMixedValue(targets, propertyPath);
}

// 複数選択で値が異なる場合にだけ、混在表示の注記を描きます。
void DrawMixedValueHint(bool mixed) {
    if (!mixed) {
        return;
    }

    ImGui::SameLine();
    ImGui::TextDisabled("— 複数の値");
}

// ImGuiのチェック項目に複数値混在フラグを設定／解除します。
void PushMixedCheckbox(bool mixed) {
    ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, mixed);
}

// チェック項目を描き終えたら混在表示フラグを戻し、次の項目へ残さない。
void PopMixedCheckbox() {
    ImGui::PopItemFlag();
}

// シーンカメラObject固有の追従・投影・注視対象設定を描画します。
void DrawCameraObjectInspector(DebugEditor* editor, BaseScene* scene, engine::Object3D* cameraObject) {
    // 編集先とカメラの対象が揃わない場合は表示しない。
    if (!editor || !cameraObject) {
        return;
    }

    // 旧形式もカメラとして保存できるよう、専用Inspectorを開いた対象の分類をCameraへそろえる。
    cameraObject->SetClassName("Camera");
    cameraObject->SetSaveCategory("Camera");
    SceneCameraSettings& settings = cameraObject->GetSceneCameraSettings();
    bool changed = false;

    ImGui::SeparatorText("Camera Object");
    ImGui::TextColored(ImVec4(0.35f, 0.85f, 1.0f, 1.0f), ICON_FA_VIDEO " Camera専用Inspector");
    ImGui::TextDisabled("このObject自身が編集・保存・プレビュー・Ghost Recorder再生の元データです。");
    // 有効状態と役割を変更した場合は、保存対象として記録する。
    if (ImGui::Checkbox("有効", &settings.enabled))
        changed = true;

    const char* roleNames[] = { "Main", "Cinematic" };
    std::int32_t role = static_cast<std::int32_t>(settings.role);

    if (ImGui::Combo("役割", &role, roleNames, IM_ARRAYSIZE(roleNames))) {
        settings.role = static_cast<SceneCameraRole>(role);
        changed = true;
    }


    // CameraのTransformとは別の、ワールド座標の適用範囲を編集する。
    ImGui::SeparatorText("適用範囲 / 自動切替");
    ImGui::PushItemWidth((std::max)(120.0f, ImGui::GetContentRegionAvail().x * 0.52f));
    auto& region = settings.region;
    const auto regionFrameBefore = editor->CaptureObjectState(cameraObject);
    static nlohmann::json sRegionEditBefore;
    static std::string sRegionEditId;
    // 同じGUIDのシーン再読込でも、前のドラッグ途中のUndo状態を持ち越さない。
    const std::string regionObjectId = std::to_string(SceneManager::GetInstance()->GetSceneGeneration()) + ":" +
        cameraObject->GetPersistentGuid() + ":" + cameraObject->GetName();
    // 世代・GUID・名前のいずれかで操作対象が変われば、前回のUndo用の途中記録を破棄する。
    if (sRegionEditId != regionObjectId) { sRegionEditBefore = nullptr; sRegionEditId = regionObjectId; }
    auto regionEdit = [&](bool modified) {
        // ドラッグ開始直前の設定を退避し、各フレームの差分を終了時に一件のUndoへまとめます。
        if (ImGui::IsItemActivated()) sRegionEditBefore = regionFrameBefore;
        // 値を変更した場合は保存対象にし、操作開始を別フレームで拾えなかった場合も変更前を残す。
        if (modified) {
            if (!sRegionEditBefore.is_object()) sRegionEditBefore = regionFrameBefore;
            changed = true;
        }
        // 編集終了か単発の変更で操作中の項目がなければ、保存した変更前の状態をUndoに登録する。
        if (sRegionEditBefore.is_object() &&
            (ImGui::IsItemDeactivatedAfterEdit() || (modified && !ImGui::IsItemActive()))) {
            editor->RegisterObjectEdited(cameraObject, sRegionEditBefore, "カメラ適用範囲を編集");
            sRegionEditBefore = nullptr;
        }
    };
    regionEdit(ImGui::Checkbox("範囲内で自動適用", &region.enabled));
    // 範囲による自動切替が有効な場合だけ、その境界と演出設定を編集できる。
    if (region.enabled) {
        regionEdit(ImGui::DragFloat3("範囲の中心 (World)", &region.center.x, 0.1f, 0.0f, 0.0f, "%.2f"));
        regionEdit(ImGui::DragFloat3("範囲の大きさ", &region.size.x, 0.1f, 0.1f, 100000.0f, "%.2f"));
        region.size.x = (std::max)(0.1f, region.size.x);
        region.size.y = (std::max)(0.1f, region.size.y);
        region.size.z = (std::max)(0.1f, region.size.z);
        regionEdit(ImGui::ColorEdit4("範囲の色", &region.color.x));
        regionEdit(ImGui::Checkbox("手前バリアの上部を透過", &region.clearForegroundBarriers));
        // 手前バリアの透過が有効なら、床と不透明度を個別に調整できる。
        if (region.clearForegroundBarriers) {
            regionEdit(ImGui::DragFloat("バリアの床高さ", &region.barrierFloorHeight, 0.1f));
            regionEdit(ImGui::DragFloat("足元に残す高さ", &region.barrierEdgeHeight, 0.05f, 0.0f, 5.0f, "%.2f"));
            regionEdit(ImGui::SliderFloat("手前バリアの不透明度", &region.barrierOpacity, 0.0f, 1.0f, "%.2f"));
            regionEdit(ImGui::SliderFloat("手前バリアの格子の不透明度", &region.barrierLatticeOpacity, 0.0f, 1.0f, "%.2f"));
        }
        regionEdit(ImGui::Checkbox("道の進行に沿って構図をつなぐ", &region.routeEnabled));
        // 道に沿う補間を使う場合だけ、ワールド座標の両端と終点カメラを設定する。
        if (region.routeEnabled) {
            ImGui::TextUnformatted("道の始点 (World)"); ImGui::SetNextItemWidth(-1);
            regionEdit(ImGui::DragFloat3("##CameraPathStart", &region.pathStart.x, .1f));
            ImGui::TextUnformatted("道の終点 (World)"); ImGui::SetNextItemWidth(-1);
            regionEdit(ImGui::DragFloat3("##CameraPathEnd", &region.pathEnd.x, .1f));
            // 終点候補は名前ではなく永続GUIDで解決します。表示名を変更しても割り当てを維持するためです。
            const engine::Object3D* endCamera = nullptr;
            auto* scene = SceneManager::GetInstance()->GetCurrentScene();
            if (scene) for (const auto& candidate : scene->GetObjects())
                if (candidate && candidate->GetPersistentGuid() == region.endCameraGuid) endCamera = candidate.get();
            ImGui::TextUnformatted("終点のCamera Object"); ImGui::SetNextItemWidth(-1);
            // 終点の一覧を開いた場合は、シーン内の別のCamera Objectを候補にする。
            if (ImGui::BeginCombo("##CameraRouteEnd", endCamera ? endCamera->GetName().c_str() : "未設定・参照先なし")) {
                if (scene) for (const auto& candidate : scene->GetObjects()) {
                    if (!candidate || candidate.get() == cameraObject || !candidate->IsCameraObject()) continue;
                    if (ImGui::Selectable(candidate->GetName().c_str(), candidate.get() == endCamera)) {
                        region.endCameraGuid = candidate->GetPersistentGuid(); regionEdit(true);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::TextWrapped("始点Cameraの構図から終点Cameraの構図へ、道の進み具合で連続補間します。先読み距離はCamera Editorで調整します。");
            // 終点を解決できない場合は、このカメラだけで構図を作ることを表示する。
            if (!endCamera) ImGui::TextDisabled("終点がない場合は、このCamera単体の構図を使います。");
        }
        regionEdit(ImGui::DragInt("範囲の優先度", &region.priority, 1.0f, -10000, 10000));
        regionEdit(ImGui::DragFloat("切替までの待ち時間", &region.activationDelay, 0.02f, 0.0f, 10.0f, "%.2f 秒"));
        regionEdit(ImGui::DragFloat("退出時の余白", &region.exitMargin, 0.05f, 0.0f, 20.0f));
        ImGui::TextDisabled("大きい優先度を選択。同優先度では現在の範囲を維持します。");
        ImGui::TextDisabled("出入りの補間時間は下のBlend In / Outを使います。");
        auto* cameraEditor = CameraEditor::GetInstance();
        bool editRange = cameraEditor->IsEditingRegionGizmo();
        // ギズモ編集の設定を変えたらCamera Editorへ渡し、開始時は自由カメラとガイド表示も有効にする。
        if (ImGui::Checkbox("範囲のギズモを編集", &editRange)) {
            cameraEditor->SetEditingRegionGizmo(editRange);
            if (editRange) {
                cameraEditor->SetMode(CameraEditor::Mode::kEditor);
                *cameraEditor->GetCameraGuideVisiblePtr() = true;
            }
        }
        ImGui::TextDisabled("範囲ギズモは停止中・自由カメラで使用します。");
        // 範囲ギズモを編集中なら、ボタンで移動と拡縮のどちらを操作するかを指定する。
        if (editRange) {
            bool& scale = cameraEditor->RegionGizmoScaleMode();
            if (ImGui::RadioButton("範囲の移動", !scale)) scale = false;
            ImGui::SameLine();
            if (ImGui::RadioButton("範囲の拡縮", scale)) scale = true;
        }
        // 位置合わせボタンが押され、現シーンにPlayerがいれば、そのワールド位置へ範囲を移してUndoに登録する。
        if (ImGui::Button("プレイヤー位置へ範囲を移す") && scene) {
            for (const auto& object : scene->GetObjects()) {
                if (object && object->GetClassName() == "Player") {
                    const auto before = editor->CaptureObjectState(cameraObject);
                    region.center = object->GetWorldPosition();
                    editor->RegisterObjectEdited(cameraObject, before, "カメラ範囲をプレイヤー位置へ移動");
                    changed = true; break;
                }
            }
        }
        // 現シーンの最初のPlayerを見つけられれば、現在の位置が範囲内か確認できる。
        if (scene) for (const auto& object : scene->GetObjects()) {
            if (object && object->GetClassName() == "Player") {
                ImGui::Text("プレイヤー位置: %s", region.Contains(object->GetWorldPosition()) ? "範囲内" : "範囲外");
                break;
            }
        }
        std::string issue;
        // カメラの検証に失敗したときは、判定先が返した理由を表示する。
        if (!cameraEditor->ValidateSceneObjectCamera(cameraObject, issue))
            ImGui::TextWrapped("設定を確認: %s", issue.c_str());
        ImGui::TextWrapped("%s", CameraManager::GetInstance()->GetRegionStatus().c_str());
        ImGui::TextDisabled("手動操作・フック照準・ロックオン・演出を優先します。");
    }

    ImGui::PopItemWidth();

    // Camera自身のTransformはコピーへ編集し、変更時にSetterと行列更新を通す。
    ImGui::SeparatorText("Transform");
    Transform transformValue = *cameraObject->GetTransform();
    Transform* transform = &transformValue;
    if (ImGui::DragFloat3("位置", &transform->translate.x, 0.1f))
        changed = true;
    engine::Vector3 rotationDegrees = {
        ToDegrees(transform->rotate.x),
        ToDegrees(transform->rotate.y),
        ToDegrees(transform->rotate.z),
    };

    // 度表示の回転が変わったら、保存と行列計算に使うラジアンへ戻す。
    if (ImGui::DragFloat3("回転 (度)", &rotationDegrees.x, 0.5f)) {
        transform->rotate = {
            ToRadians(rotationDegrees.x),
            ToRadians(rotationDegrees.y),
            ToRadians(rotationDegrees.z),
        };

        transform->isQuaternionMaster = false;
        changed = true;
    }

    // ここまでの編集があった場合は、コピー上のTransformをObjectへ渡してローカル・ワールド行列を順に更新する。
    if (changed) {
        cameraObject->SetLocalTransform(transformValue);
        cameraObject->UpdateLocalMatrix();
        cameraObject->UpdateWorldMatrix();
    }

    // 変更したレンズと補間設定を保存対象にする。FOVは度で編集し、NearとFarはFarが奥になる範囲へ補正する。
    ImGui::SeparatorText("Lens / Blend");
    float fovDegrees = ToDegrees(settings.fovY);

    if (ImGui::SliderFloat("FOV (度)", &fovDegrees, 10.0f, 120.0f, "%.1f")) {
        settings.fovY = ToRadians(fovDegrees);
        changed = true;
    }
    if (ImGui::Checkbox(region.enabled ? "テスト再生時のFOVも補間" : "FOVも補間", &settings.blendFov))
        changed = true;

    if (ImGui::DragFloat("Near Clip", &settings.nearClip, 0.01f, 0.001f, 100.0f, "%.3f"))
        changed = true;
    if (ImGui::DragFloat("Far Clip", &settings.farClip, 1.0f, 1.0f, 100000.0f, "%.1f"))
        changed = true;
    settings.nearClip = (std::max)(settings.nearClip, 0.001f);
    settings.farClip = (std::max)(settings.farClip, settings.nearClip + 0.01f);
    if (ImGui::DragFloat("Blend In (秒)", &settings.blendInDuration, 0.02f, 0.0f, 10.0f, "%.2f"))
        changed = true;
    if (ImGui::DragFloat("Blend Out (秒)", &settings.blendOutDuration, 0.02f, 0.0f, 10.0f, "%.2f"))
        changed = true;
    const char* easingNames[] = { "Linear", "Ease In", "Ease Out", "Ease In Out", "Smoother Step" };
    std::int32_t easing = static_cast<std::int32_t>(settings.easing);

    if (ImGui::Combo("切替Easing", &easing, easingNames, IM_ARRAYSIZE(easingNames))) {
        settings.easing = static_cast<SceneCameraEasing>(easing);
        changed = true;
    }

    // 視点と注視点は取得元と追従方法を独立に設定する。
    ImGui::SeparatorText("Eye / Follow");
    const char* eyeSourceNames[] = { "Camera ObjectのTransform", "Scene Objectを追従", "参照Object方向を基準に配置" };
    std::int32_t eyeSource = static_cast<std::int32_t>(settings.eyeSource);

    if (ImGui::Combo("Eye取得元", &eyeSource, eyeSourceNames, IM_ARRAYSIZE(eyeSourceNames))) {
        settings.eyeSource = static_cast<SceneCameraEyeSource>(eyeSource);
        changed = true;
    }

    // 視点を別のObjectから求める方式でだけ、追従対象の名前を選べる。
    if (settings.eyeSource != SceneCameraEyeSource::kCameraObject) {
        if (DrawSceneObjectNameCombo("追従Object##CameraEye", scene, cameraObject, settings.eyeObjectName))
            changed = true;
    }

    // 参照対象への向きから視点を作る場合は、基準方向となる別のObjectも指定できる。
    if (settings.eyeSource == SceneCameraEyeSource::kSceneObjectRelativeToReference) {
        if (DrawSceneObjectNameCombo("方向参照Object##CameraEyeReference", scene, cameraObject,
                                     settings.eyeReferenceObjectName))
            changed = true;
        ImGui::TextDisabled("Eyeオフセット: 右 / 上 / 参照Object方向");
    }

    // オフセットと追従方式を扱い、遅延追従を選んだときだけ応答速度を調整できる。
    if (ImGui::DragFloat3("Eyeオフセット", &settings.eyeOffset.x, 0.05f))
        changed = true;
    const char* followModeNames[] = { "完全追従", "遅延追従" };
    std::int32_t eyeFollow = static_cast<std::int32_t>(settings.eyeFollowMode);

    if (ImGui::Combo("Eye追従方式", &eyeFollow, followModeNames, IM_ARRAYSIZE(followModeNames))) {
        settings.eyeFollowMode = static_cast<SceneCameraFollowMode>(eyeFollow);
        changed = true;
    }

    if (settings.eyeFollowMode == SceneCameraFollowMode::kSmooth &&
        ImGui::DragFloat("Eye追従レスポンス", &settings.eyeFollowResponse, 0.1f, 0.1f, 40.0f, "%.2f")) {
        changed = true;
    }

    // 注視位置を、固定点・シーンのObject・カメラの前方のいずれから取るかを選ぶ。
    ImGui::SeparatorText("Target / Follow");
    const char* targetModeNames[] = { "固定座標", "Scene Objectを注視", "Cameraの前方" };
    std::int32_t targetMode = static_cast<std::int32_t>(settings.targetMode);

    if (ImGui::Combo("注視方法", &targetMode, targetModeNames, IM_ARRAYSIZE(targetModeNames))) {
        settings.targetMode = static_cast<SceneCameraTargetMode>(targetMode);
        changed = true;
    }

    // 固定座標を使う場合は、そのワールド位置を直接編集する。
    if (settings.targetMode == SceneCameraTargetMode::kFixedPoint) {
        if (ImGui::DragFloat3("固定注視点", &settings.fixedTarget.x, 0.1f))
            changed = true;
    }
    // 注視するObject方式では、シーンから対象名を選ぶ。
    else if (settings.targetMode == SceneCameraTargetMode::kSceneObject) {
        if (DrawSceneObjectNameCombo("注視Object##CameraTarget", scene, cameraObject, settings.targetObjectName))
            changed = true;
    }
    else {
        // カメラの前方を使う方式では、注視点までの距離を設定する。
        if (ImGui::DragFloat("前方距離", &settings.forwardDistance, 0.1f, 0.1f, 1000.0f, "%.1f"))
            changed = true;
    }

    // 注視点が動く方式では、オフセットと追従方式も設定できる。
    if (settings.targetMode != SceneCameraTargetMode::kFixedPoint) {
        if (ImGui::DragFloat3("注視オフセット", &settings.targetOffset.x, 0.05f))
            changed = true;
        std::int32_t targetFollow = static_cast<std::int32_t>(settings.targetFollowMode);

        if (ImGui::Combo("Target追従方式", &targetFollow, followModeNames, IM_ARRAYSIZE(followModeNames))) {
            settings.targetFollowMode = static_cast<SceneCameraFollowMode>(targetFollow);
            changed = true;
        }

        // 注視点を遅延追従させる場合にだけ、応答速度を編集する。
        if (settings.targetFollowMode == SceneCameraFollowMode::kSmooth &&
            ImGui::DragFloat("Target追従レスポンス", &settings.targetFollowResponse, 0.1f, 0.1f, 40.0f, "%.2f")) {
            changed = true;
        }
    }

    // 記録パスの選択・テスト再生はCamera Objectの保存値と実行状態を分けて扱う。
    ImGui::SeparatorText("Ghost Recorder");
    const std::string recordPreview = cameraObject->GetRecordPathName().empty() ? "(なし)" : cameraObject->GetRecordPathName();

    // パス一覧が開いたら、録画パスを解除するか既存のJSONを割り当てる。
    if (ImGui::BeginCombo("パスデータ", recordPreview.c_str())) {
        // なしを選んだら保存名を空にし、録画再生器があれば止める。
        if (ImGui::Selectable("(なし)", cameraObject->GetRecordPathName().empty())) {
            cameraObject->SetRecordPathName("");
            if (cameraObject->GetRecorder())
                cameraObject->GetRecorder()->Stop();
            changed = true;
        }

        const fs::path directory = "Resources/json/animation";

        // 録画フォルダーが存在する場合にだけ、JSONの拡張子を持つ候補を列挙する。
        if (fs::exists(directory) && fs::is_directory(directory)) {
            for (const auto& entry : fs::directory_iterator(directory)) {
                if (entry.path().extension() != ".json")
                    continue;
                const std::string pathName = entry.path().stem().string();
                const bool selected = cameraObject->GetRecordPathName() == pathName;

                // 選んだパスのファイル名を保存値へ入れ、既に割り当て済みの候補に初期フォーカスを当てる。
                if (ImGui::Selectable(pathName.c_str(), selected)) {
                    cameraObject->SetRecordPathName(pathName);
                    changed = true;
                }

                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }

    bool recordLoop = cameraObject->IsRecordLoop();

    // ループ設定を変更した場合は、次回から使う再生方式を保存する。
    if (ImGui::Checkbox("ループ再生##CameraRecord", &recordLoop)) {
        cameraObject->SetRecordLoop(recordLoop);
        changed = true;
    }

    bool recordRelative = cameraObject->IsRecordRelative();

    // 相対座標の設定が変われば、録画をテストするときの座標基準へ反映する。
    if (ImGui::Checkbox("相対座標##CameraRecord", &recordRelative)) {
        cameraObject->SetRecordRelative(recordRelative);
        changed = true;
    }

    // 確認ボタンが押されたら、選択カメラの構図で一時的にテスト再生する。
    if (ImGui::Button(ICON_FA_PLAY " Cameraをテスト再生")) {
        CameraEditor::GetInstance()->PlaySceneObjectCamera(CameraManager::GetInstance()->GetMainCamera(), cameraObject);
    }

    ImGui::SameLine();

    // 録画パスを停止してからシーンカメラの一時上書きを解除し、通常の視点へ戻します。
    if (ImGui::Button(ICON_FA_STOP " 停止##CameraObject")) {
        if (cameraObject->GetRecorder())
            cameraObject->GetRecorder()->Stop();
        CameraEditor::GetInstance()->StopSceneObjectCamera(CameraManager::GetInstance()->GetMainCamera());
    }

    // 選択済みの録画パスがあり再生ボタンが押されたら、再生器を用意して保存設定で再生する。
    if (!cameraObject->GetRecordPathName().empty() && ImGui::Button(ICON_FA_GHOST " Ghost Recorder再生")) {
        if (!cameraObject->GetRecorder())
            cameraObject->InitializeRecorder(nullptr);
        if (cameraObject->GetRecorder()) {
            cameraObject->GetRecorder()->Play(
                cameraObject->GetRecordPathName(),
                cameraObject->IsRecordLoop(),
                cameraObject->IsRecordRelative(),
                true);
        }
    }

    // どこかの設定が変わった場合に、対象を保存上の変更済みにする。
    if (changed) {
        editor->MarkDirtyForObject(cameraObject);
    }
}

std::vector<engine::Object3D*> CollectSceneObjects(BaseScene* scene) {
    // 現シーンの所有物から有効で内部専用でない参照を集める。現シーンがなければ空を返す。
    std::vector<engine::Object3D*> objects;

    if (!scene) {
        return objects;
    }

    objects.reserve(scene->GetObjects().size());

    for (const auto& object : scene->GetObjects()) {
        if (object && !object->IsEditorInternal()) {
            objects.push_back(object.get());
        }
    }

    return objects;
}

std::vector<engine::Object3D*> CollectPrefabInstanceObjects(BaseScene* scene, const std::string& instanceId) {
    // 現シーンとインスタンスIDが揃う場合に、同じインスタンスに属するルートと子の参照を集める。
    std::vector<engine::Object3D*> objects;

    if (!scene || instanceId.empty()) {
        return objects;
    }

    for (const auto& object : scene->GetObjects()) {
        if (object && object->IsPrefabInstance() &&
            object->GetPrefabInstanceInfo().instanceId == instanceId) {
            objects.push_back(object.get());
        }
    }

    return objects;
}

// Prefabインスタンスの差分、適用、復元、元Assetへの操作を表示します。
void DrawPrefabInstancePanel(DebugEditor* editor, BaseScene* scene, engine::Object3D* object) {
    // 対象がPrefabインスタンスで、Editorとシーンが使える場合だけ差分の操作を提示する。
    if (!editor || !scene || !object || !object->IsPrefabInstance()) {
        return;
    }

    PresetManager* manager = PresetManager::GetInstance();
    const auto info = object->GetPrefabInstanceInfo();

    // InstanceのProperty/Component差分と、Variant自身のBase差分を別々に取得する。
    const auto overrides = manager->GetPrefabOverrides(object);
    const auto componentOverrides = manager->GetPrefabComponentOverrides(object);
    const auto variantOverrides = manager->GetPrefabVariantOverrides(object);
    const auto variantComponentOverrides = manager->GetPrefabVariantComponentOverrides(object);
    const auto structureSummary = manager->GetPrefabStructureOverrideSummary(info.prefabName);

    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.18f, 0.33f, 0.50f, 1.0f));
    const bool open = ImGui::CollapsingHeader(ICON_FA_CUBES " Prefab Instance", ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::PopStyleColor();

    // 見出しを閉じている場合は、差分の中身と操作ボタンの描画を省く。
    if (!open) {
        return;
    }

    ImGui::Text("Prefab: %s", info.prefabName.empty() ? "(名称不明)" : info.prefabName.c_str());
    const std::string basePrefabName = manager->GetPrefabBaseName(info.prefabName);

    // 継承元が登録されたVariantであれば、元アセット名を表示する。
    if (!basePrefabName.empty()) {
        ImGui::TextColored(ImVec4(0.70f, 0.55f, 1.0f, 1.0f),
            ICON_FA_CODE_BRANCH " Variant of: %s", basePrefabName.c_str());
    }

    ImGui::TextDisabled("Asset ID: %s", info.assetId.c_str());
    ImGui::TextDisabled("Instance ID: %s", info.instanceId.c_str());

    // Prefab自体を編集中は、シーン側のインスタンス操作を表示せずHierarchyの保存・破棄を案内する。
    if (editor->IsPrefabEditMode()) {
        ImGui::TextColored(ImVec4(0.35f, 0.78f, 1.0f, 1.0f),
            ICON_FA_CUBE " Prefab ModeでAssetを直接編集中です。");
        ImGui::TextDisabled("保存・破棄はHierarchy上部から実行します。");
        ImGui::Separator();
        return;
    }

    // 元Assetが見つからないインスタンスは、参照の問題を通知して復元またはリンク解除の入口を残します。
    if (!manager->HasValidPrefabSource(object)) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.25f, 1.0f),
            ICON_FA_EXCLAMATION_TRIANGLE " 元Prefabが見つかりません。UnpackするかPrefab Assetを復元してください。");
    }

    // Prefab Modeで開く入口はルートにだけ出す。ルートの位置と回転はシーン配置に属する。
    if (info.isRoot) {
        ImGui::TextDisabled("ルートの位置・回転はScene配置値として保持されます。");

        if (ImGui::Button(ICON_FA_CUBE " Prefab Modeで開く")) {
            editor->BeginPrefabEditSession(info.prefabName);
            return;
        }
    }

    const std::vector<engine::Object3D*> sceneObjects = CollectSceneObjects(scene);
    const std::vector<engine::Object3D*> instanceObjects = CollectPrefabInstanceObjects(scene, info.instanceId);

    // 全件Applyは参照するScene内Objectへの伝播を一つのUndoグループへまとめる。
    if (ImGui::Button(ICON_FA_UPLOAD " Apply All", ImVec2(112.0f, 0.0f))) {
        EditorTransactionManager::GetInstance()->BeginGroup("Apply All Prefab Overrides");
        const auto beforeStates = editor->CaptureObjectStates(sceneObjects);
        const std::int32_t applied = manager->ApplyAllPrefabOverrides(object, sceneObjects);
        editor->RegisterObjectsEdited(beforeStates, "Prefab Apply Propagation");
        EditorTransactionManager::GetInstance()->EndGroup();
        DebugConsole::GetInstance()->AddLog("Prefab Apply All: " + std::to_string(applied) + " overrides");
    }

    ImGui::SameLine();

    // 適用と違い、戻す対象はこのインスタンスです。変更前の状態も同じインスタンスに限って記録します。
    if (ImGui::Button(ICON_FA_UNDO " Revert All", ImVec2(112.0f, 0.0f))) {
        const auto beforeStates = editor->CaptureObjectStates(instanceObjects);
        const std::int32_t reverted = manager->RevertAllPrefabOverrides(object, sceneObjects);
        editor->RegisterObjectsEdited(beforeStates, "Prefab Revert All");
        DebugConsole::GetInstance()->AddLog("Prefab Revert All: " + std::to_string(reverted) + " overrides");
    }

    ImGui::SameLine();

    // Unpackを押した場合は、現在のインスタンスのリンク解除を履歴へ残す。
    if (ImGui::Button(ICON_FA_UNLINK " Unpack", ImVec2(96.0f, 0.0f))) {
        const auto beforeStates = editor->CaptureObjectStates(instanceObjects);
        const std::int32_t unpacked = manager->UnpackPrefabInstance(object, sceneObjects);
        editor->RegisterObjectsEdited(beforeStates, "Unpack Prefab Instance");
        DebugConsole::GetInstance()->AddLog("Prefab Unpack: " + std::to_string(unpacked) + " objects");
    }

    // Componentの有無と通常Propertyの値は別の差分としてApply/Revertする。
    ImGui::SeparatorText(("Component Overrides (" + std::to_string(componentOverrides.size()) + ")").c_str());

    // Componentの有無に差分がなければ、操作できる行がないことを表示する。
    if (componentOverrides.empty()) {
        ImGui::TextDisabled("このObjectにComponent構造差分はありません。");
    }

    for (const auto& entry : componentOverrides) {
        ImGui::PushID(("Component_" + entry.componentTypeId).c_str());
        ImGui::Text("%s", entry.displayName.c_str());
        ImGui::SameLine();
        ImGui::TextColored(
            entry.instancePresent ? ImVec4(0.45f, 0.90f, 0.55f, 1.0f) : ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
            entry.instancePresent ? "追加" : "削除");

        // 適用ボタンでは該当Componentの差分を元アセットへ反映し、関連Objectへの伝播が成功した場合に履歴を登録する。
        if (ImGui::SmallButton(ICON_FA_UPLOAD " Apply")) {
            EditorTransactionManager::GetInstance()->BeginGroup("Apply Prefab Component Override");
            const auto beforeStates = editor->CaptureObjectStates(sceneObjects);

            if (manager->ApplyPrefabComponent(object, entry.componentTypeId, sceneObjects)) {
                editor->RegisterObjectsEdited(beforeStates, "Prefab Component Apply Propagation");
                DebugConsole::GetInstance()->AddLog("Prefab Component Apply: " + entry.componentTypeId);
            }

            EditorTransactionManager::GetInstance()->EndGroup();
        }

        ImGui::SameLine();

        // 復元ボタンではこのインスタンスを対象にし、該当Componentを元の状態にできた場合に履歴を登録する。
        if (ImGui::SmallButton(ICON_FA_UNDO " Revert")) {
            const auto beforeStates = editor->CaptureObjectStates(instanceObjects);

            if (manager->RevertPrefabComponent(object, entry.componentTypeId)) {
                editor->RegisterObjectsEdited(beforeStates, "Prefab Component Revert");
                DebugConsole::GetInstance()->AddLog("Prefab Component Revert: " + entry.componentTypeId);
            }
        }

        ImGui::PopID();
    }

    ImGui::SeparatorText(("Property Overrides (" + std::to_string(overrides.size()) + ")").c_str());

    // 通常プロパティの差分がなければ、一覧に差分がないことを表示する。
    if (overrides.empty()) {
        ImGui::TextDisabled("このObjectに適用差分はありません。");
    }

    for (const auto& entry : overrides) {
        ImGui::PushID(entry.propertyPath.c_str());
        ImGui::Text("%s", entry.displayName.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", entry.propertyPath.c_str());

        // 適用ボタンでは選んだプロパティを元アセットへ反映し、成功した伝播を履歴に登録する。
        if (ImGui::SmallButton(ICON_FA_UPLOAD " Apply")) {
            EditorTransactionManager::GetInstance()->BeginGroup("Apply Prefab Override");
            const auto beforeStates = editor->CaptureObjectStates(sceneObjects);

            if (manager->ApplyPrefabProperty(object, entry.propertyPath, sceneObjects)) {
                editor->RegisterObjectsEdited(beforeStates, "Prefab Apply Propagation");
                DebugConsole::GetInstance()->AddLog("Prefab Apply: " + entry.propertyPath);
            }

            EditorTransactionManager::GetInstance()->EndGroup();
        }

        ImGui::SameLine();

        // 復元ボタンでは該当プロパティだけを元の値にし、成功した変更を現在のインスタンスの履歴に残す。
        if (ImGui::SmallButton(ICON_FA_UNDO " Revert")) {
            const auto beforeStates = editor->CaptureObjectStates(instanceObjects);

            if (manager->RevertPrefabProperty(object, entry.propertyPath)) {
                editor->RegisterObjectsEdited(beforeStates, "Prefab Revert Property");
                DebugConsole::GetInstance()->AddLog("Prefab Revert: " + entry.propertyPath);
            }
        }

        ImGui::PopID();
    }

    // VariantならInstance差分の下に、Base Prefabとの差分も表示する。
    if (!basePrefabName.empty()) {
        const std::size_t variantOverrideCount = variantOverrides.size() + variantComponentOverrides.size();
        ImGui::SeparatorText(("Variant Source Overrides (" + std::to_string(variantOverrideCount) + ")").c_str());
        ImGui::TextDisabled("InstanceのApplyで作られた、基底Prefabに対するVariant固有差分です。");

        // 構造差分があるときだけ、追加・削除・移動・Componentなどの件数を表示する。
        if (structureSummary.HasOverrides()) {
            ImGui::Text("構造差分: Object追加 %d / 削除 %d / 移動 %d / Component %d / その他 %d",
                structureSummary.addedObjects,
                structureSummary.removedObjects,
                structureSummary.reparentedObjects,
                structureSummary.componentOverrides,
                structureSummary.rawNodeOverrides);
        }

        // この個体に属するVariantの値やComponentの差分が空の場合は、その旨を表示する。
        if (variantOverrides.empty() && variantComponentOverrides.empty()) {
            ImGui::TextDisabled("このObjectにはVariant固有差分がありません。");
        }

        for (const auto& entry : variantComponentOverrides) {
            ImGui::PushID(("VariantComponent_" + entry.componentTypeId).c_str());
            ImGui::Text("%s", entry.displayName.c_str());
            ImGui::SameLine();
            ImGui::TextColored(
                entry.variantPresent ? ImVec4(0.45f, 0.90f, 0.55f, 1.0f) : ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                entry.variantPresent ? "追加" : "削除");
            ImGui::SameLine();

            // 選んだVariantのComponent差分を基底へ戻し、成功した場合だけ伝播の履歴を登録する。
            if (ImGui::SmallButton(ICON_FA_UNDO " Baseへ戻す")) {
                EditorTransactionManager::GetInstance()->BeginGroup("Revert Prefab Variant Component Override");
                const auto beforeStates = editor->CaptureObjectStates(sceneObjects);

                if (manager->RevertPrefabVariantComponent(object, entry.componentTypeId, sceneObjects)) {
                    editor->RegisterObjectsEdited(beforeStates, "Variant Component Revert Propagation");
                    DebugConsole::GetInstance()->AddLog("Prefab Variant Component Revert: " + entry.componentTypeId);
                }

                EditorTransactionManager::GetInstance()->EndGroup();
            }

            ImGui::PopID();
        }

        for (const auto& entry : variantOverrides) {
            ImGui::PushID(("Variant_" + entry.propertyPath).c_str());
            ImGui::Text("%s", entry.displayName.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", entry.propertyPath.c_str());
            ImGui::SameLine();

            // 選んだVariantのプロパティ差分を基底値へ戻し、成功した場合だけ伝播の履歴を登録する。
            if (ImGui::SmallButton(ICON_FA_UNDO " Baseへ戻す")) {
                EditorTransactionManager::GetInstance()->BeginGroup("Revert Prefab Variant Override");
                const auto beforeStates = editor->CaptureObjectStates(sceneObjects);

                if (manager->RevertPrefabVariantProperty(object, entry.propertyPath, sceneObjects)) {
                    editor->RegisterObjectsEdited(beforeStates, "Variant Revert Propagation");
                    DebugConsole::GetInstance()->AddLog("Prefab Variant Revert: " + entry.propertyPath);
                }

                EditorTransactionManager::GetInstance()->EndGroup();
            }

            ImGui::PopID();
        }
    }

    ImGui::Separator();
}

// 共有材質の保存先をJSONへそろえ、ファイル名だけの場合は既定の保存フォルダーを補う。
std::string NormalizeMaterialInstanceSavePath(const char* text) {
    fs::path path = text ? fs::path(text) : fs::path();

    // 入力なしは空のパスを返し、指定があれば拡張子と親の有無を正規化する。
    if (path.empty())
        return {};
    if (path.extension() != ".json")
        path.replace_extension(".json");
    if (!path.has_parent_path())
        path = fs::path("Resources/json/material_instances") / path;
    return path.lexically_normal().generic_string();
}

// 選択ObjectのMaterial Instanceと複数選択時の編集状態を表示します。
void DrawMaterialInstancePanel(
    DebugEditor* editor,
    engine::Object3D* selectedObject,
    const std::vector<engine::Object3D*>& targets) {
    // Editorと代表の選択Objectを取得できた場合に材質の編集を出す。
    if (!editor || !selectedObject)
        return;

    ImGui::SeparatorText("Material Instance");
    const std::string& linkedPath = selectedObject->GetMaterialInstancePath();
    ImGui::Text("リンク: %s", linkedPath.empty() ? "未設定" : linkedPath.c_str());
    ImGui::TextDisabled("リンク中はScene読込時にAssetの設定一式を再適用します。");

    // 材質定義のEditor一覧は同じRegistryから取得する。
    auto& shaderMaterials = ShaderMaterialRegistry::Instance();
    const std::string selectedShaderId = selectedObject->GetShaderMaterialId();
    const auto* selectedShader = shaderMaterials.Find(selectedShaderId);
    ImGui::TextDisabled("Shader Materialの変更時は共有Materialとのリンクを解除します。");
    // 通常材質タイプ0の代表だけにShader Materialの選択を出し、同タイプの選択先だけを切り替える。
    if (selectedObject->GetMaterialType() == 0) {
        if (ImGui::BeginCombo("Shader Material", selectedShader ? selectedShader->displayName.c_str() :
                              (selectedShaderId.empty() ? "標準" : "未登録のID"))) {
            // 標準を選んだ場合は共有材質のリンクとShader Material IDを消す。
            if (ImGui::Selectable("標準", selectedShaderId.empty())) {
                for (engine::Object3D* target : targets)
                    if (target && target->GetMaterialType() == 0) {
                        target->ClearMaterialInstanceLink();
                        target->SetShaderMaterialId({});
                    }
                MarkInspectorTargetsDirty(editor, targets);
            }
            for (const auto& definition : shaderMaterials.List()) {
                // 登録済みの別の材質を選んだ場合も、共有材質のリンクは外してIDを選択値へ更新する。
                if (ImGui::Selectable(definition.displayName.c_str(), definition.id == selectedShaderId)) {
                    for (engine::Object3D* target : targets)
                        if (target && target->GetMaterialType() == 0) {
                            target->ClearMaterialInstanceLink();
                            target->SetShaderMaterialId(definition.id);
                        }
                    MarkInspectorTargetsDirty(editor, targets);
                }
            }
            ImGui::EndCombo();
        }
    } else {
        ImGui::TextDisabled("Shader Materialは通常材質タイプ0で使用できます。");
    }
    // 一覧の再読込が押された場合だけ、保存されている材質定義を読み直す。
    if (ImGui::Button("Shader Material一覧を再読込"))
        shaderMaterials.Reload();
    for (const std::string& error : shaderMaterials.Errors())
        ImGui::TextWrapped("%s", error.c_str());

    static std::vector<std::string> sAssets;
    static bool sAssetsLoaded = false;

    // 初回は保存済みの共有材質Assetを検索し、次のフレーム以降に使う一覧を作る。
    if (!sAssetsLoaded) {
        sAssets = MaterialInstanceAsset::Discover();
        sAssetsLoaded = true;
    }

    // 共有材質の一覧を開いたら、現在のリンク先に印を付け、選択されたAssetを各対象へ適用する。
    if (ImGui::BeginCombo("共有Materialを選択", linkedPath.empty() ? "未設定" : linkedPath.c_str())) {
        for (const std::string& asset : sAssets) {
            const bool selected = asset == linkedPath;

            if (ImGui::Selectable(asset.c_str(), selected)) {
                std::string error;
                bool applied = false;

                for (engine::Object3D* target : targets) {
                    applied |= target && target->ApplyMaterialInstance(asset, &error);
                }

                // 一件でも適用したら変更を記録する。適用できずエラーがある場合はログへ出す。
                if (applied) {
                    MarkInspectorTargetsDirty(editor, targets);
                    DebugConsole::GetInstance()->AddLog("Material Instance applied: " + asset);
                } else if (!error.empty()) {
                    DebugConsole::GetInstance()->AddLog("Material Instance error: " + error);
                }
            }

            if (selected)
                ImGui::SetItemDefaultFocus();
        }

        // 一覧が空の場合は、保存済みのAssetがないことを表示する。
        if (sAssets.empty())
            ImGui::TextDisabled("保存済みAssetはまだありません。");
        ImGui::EndCombo();
    }

    // 代表が共有材質にリンク済みの場合だけ、リンクの再読込と解除を出す。
    if (!linkedPath.empty()) {
        if (ImGui::Button(ICON_FA_SYNC " 再読込##MaterialInstance")) {
            std::string error;
            bool applied = false;

            // 有効な選択先それぞれのリンクを再読込し、リンクがない対象には代表のパスを使う。
            for (engine::Object3D* target : targets) {
                if (!target)
                    continue;
                const std::string path = target->GetMaterialInstancePath().empty()
                    ? linkedPath : target->GetMaterialInstancePath();
                applied |= target->ApplyMaterialInstance(path, &error);
            }

            // 適用できた対象があれば変更済みにし、報告されたエラーはログへ残す。
            if (applied)
                MarkInspectorTargetsDirty(editor, targets);
            if (!error.empty())
                DebugConsole::GetInstance()->AddLog("Material Instance error: " + error);
        }

        ImGui::SameLine();

        // 共有Assetへの参照だけを消し、いま表示している材質値は保持します。
        if (ImGui::Button(ICON_FA_UNLINK " リンク解除##MaterialInstance")) {
            for (engine::Object3D* target : targets)
                if (target)
                    target->ClearMaterialInstanceLink();
            MarkInspectorTargetsDirty(editor, targets);
        }
    }

    static char sSavePath[260] = "NewMaterial.json";
    ImGui::SetNextItemWidth(-130.0f);
    ImGui::InputText("##MaterialInstanceSavePath", sSavePath, sizeof(sSavePath));
    ImGui::SameLine();

    // 保存ボタンが押されたときだけ代表の現在値からAssetを作成する。
    if (ImGui::Button(ICON_FA_SAVE " 現在値を保存")) {
        const std::string path = NormalizeMaterialInstanceSavePath(sSavePath);
        std::string error;

        // 代表の現在値を保存できた後にだけ他の選択対象へ適用し、候補一覧を最新にします。
        if (selectedObject->SaveMaterialInstance(path, &error)) {
            for (engine::Object3D* target : targets) {
                // 代表以外の有効な選択先へも、保存したAssetを適用する。
                if (target && target != selectedObject)
                    target->ApplyMaterialInstance(path, nullptr);
            }

            sAssets = MaterialInstanceAsset::Discover();
            MarkInspectorTargetsDirty(editor, targets);
            DebugConsole::GetInstance()->AddLog("Material Instance saved: " + path);
        } else {
            // 保存に失敗した場合は、他の対象へ適用せず理由をログへ残す。
            DebugConsole::GetInstance()->AddLog("Material Instance error: " + error);
        }
    }
}

// Decal設定の編集欄を選択Objectへ表示します。
void DrawDecalPanel(
    DebugEditor* editor,
    engine::Object3D* selectedObject,
    const std::vector<engine::Object3D*>& targets) {
    // 代表がデカールで編集基盤があり、見出しを開いた場合にだけ設定欄を表示する。
    if (!editor || !selectedObject || !selectedObject->IsDecal())
        return;
    if (!ImGui::CollapsingHeader("Surface Decal", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    // 投影の幅・高さ・面オフセットはワールド単位、寿命とフェードは秒。寿命0は永続。
    engine::Object3D::DecalSettings settings = selectedObject->GetDecalSettings();
    bool changed = false;
    changed |= ImGui::DragFloat2("投影サイズ", &settings.size.x, 0.05f, 0.01f, 100.0f);
    changed |= ImGui::DragFloat("面オフセット", &settings.depthOffset, 0.001f, 0.0001f, 0.2f, "%.4f");
    changed |= ImGui::DragFloat("寿命 (0=永続)", &settings.lifetime, 0.05f, 0.0f, 120.0f);
    changed |= ImGui::DragFloat("Fade In", &settings.fadeIn, 0.02f, 0.0f, 10.0f);
    changed |= ImGui::DragFloat("Fade Out", &settings.fadeOut, 0.02f, 0.0f, 10.0f);
    changed |= ImGui::Checkbox("寿命後に削除対象", &settings.transient);

    // いずれかの入力が変わったときに、有効なデカールの選択先だけへ新しい値を反映する。
    if (changed) {
        for (engine::Object3D* target : targets) {
            if (!target || !target->IsDecal())
                continue;
            const engine::Object3D::DecalSettings oldSettings = target->GetDecalSettings();
            target->SetDecalSettings(settings);
            const Transform* transform = target->GetTransform();
            const engine::Matrix4x4 rotation = transform->isQuaternionMaster
                ? engine::Math::MakeRotateQuaternionMatrix(transform->quaternion)
                : engine::Math::MakeRotateMatrix(transform->rotate);
            // デカールのローカル前面は−Zです。指定の面オフセットと以前の差だけを法線方向に足し、繰り返し編集でも累積させません。
            const engine::Vector3 normal = { -rotation.m[2][0], -rotation.m[2][1], -rotation.m[2][2] };
            target->SetTranslate(transform->translate + normal * (settings.depthOffset - oldSettings.depthOffset));
            // 元の平面が各軸−1～1なので、指定した全幅・全高を半分にしてTransformの倍率にします。
            target->SetScale({ settings.size.x * 0.5f, settings.size.y * 0.5f, 1.0f });
            target->UpdateLocalMatrix();
            target->UpdateWorldMatrix();
        }

        MarkInspectorTargetsDirty(editor, targets);
    }

    // 確認ボタンが押された場合に、選択されたデカールだけフェードを先頭から再生する。
    if (ImGui::Button(ICON_FA_PLAY " Fadeを最初から確認")) {
        for (engine::Object3D* target : targets)
            if (target && target->IsDecal())
                target->RestartDecalPlayback();
    }

    ImGui::TextDisabled("Game View配置では地面・壁の法線へ自動整列します。回転はTransformで調整できます。");
}
}

#endif // USE_IMGUI
