#include <cstdint>
#include "engine/editor/assets/JsonAssetBrowser.h"
#include "engine/editor/scene/inspector/InspectorWindow.Internal.h"
#include "engine/editor/assets/ProjectWindow.h"

using namespace inspectorWindowImplementation;

void InspectorWindow::Initialize(DebugEditor* editor) {
    // 選択状態や編集履歴を管理するDebugEditorを借用し、Inspectorから表示と変更を行う。
    editor_ = editor;
}

void InspectorWindow::Draw() {
#ifdef USE_IMGUI
    // シーンと代表選択を取得し、共通項目は対象群へ、名前や親子など固有項目は代表だけへ反映する。
    // シーン管理か現在シーンがない場合は、編集対象や保存先を解決できないため戻る。
    if (editor_->GetSceneManager() == nullptr)
        return;
    BaseScene* currentScene = editor_->GetSceneManager()->GetCurrentScene();
    if (currentScene == nullptr)
        return;

    std::string currentJsonPath = "Resources/json/3Dobject/" + std::string(editor_->GetCurrentSceneFilenameBuffer());
    engine::Object3D* selectedObject = editor_->GetSelectedObject();
    const std::size_t selectedCount = editor_->GetSelectedObjectCount();

    // ---------------------------------------------------------
    // 2. オブジェクト詳細 (Inspector本体)
    // ---------------------------------------------------------
    // 選択なしでは常時使う補助表示だけを出し、個別の編集欄には進まない。
    if (selectedObject == nullptr) {
        ImGui::TextDisabled(ICON_FA_EXCLAMATION_CIRCLE " オブジェクトが選択されていません");
        ImGui::TextDisabled("Hierarchyから選択してください");
        ImGui::Separator();

        // ゲッター経由での描画フラグ制御
        ImGui::Checkbox(ICON_FA_EYE " コライダー枠を描画", editor_->GetDrawCollidersPtr());
        ImGui::Checkbox(ICON_FA_FINGERPRINT " イベントIDを表示", editor_->GetDrawEventIdsPtr());
    }
    else {
        std::vector<engine::Object3D*> inspectorTargets = CollectInspectorTargets(editor_, selectedObject);
        const bool isMultiSelection = inspectorTargets.size() > 1;

        if (isMultiSelection) {
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 1.0f, 1.0f),
                ICON_FA_CUBES " %zu個選択中 / 共通項目は一括編集されます", inspectorTargets.size());
            ImGui::TextDisabled("名前・親子・モデル分割など、代表Object専用の項目は代表Objectだけを編集します。");
            ImGui::Separator();
        }

        DrawPrefabInstancePanel(editor_, currentScene, selectedObject);

        // 名前とパス編集は代表Object固有の操作として扱う。
        // --- 名前表示 ---
        char nameBuffer[256];
        std::string currentName = selectedObject->GetName();
        // 無名の対象は入力欄に仮の表示名を出し、編集されたときだけ本体に名前を書く。
        if (currentName.empty())
            currentName = "NoName";
        strcpy_s(nameBuffer, currentName.c_str());

        if (ImGui::InputText(ICON_FA_TAG " 名前", nameBuffer, sizeof(nameBuffer))) {
            selectedObject->SetName(std::string(nameBuffer));
        }

        ImGui::Spacing();

        // 選択対象の動作は上部から操作し、通常の配置設定はその下へ残す。
        // 動作確認とポーズ編集は代表対象の単体操作です。複数選択やパス編集へ切り替えると両方のプレビューを停止します。
        if (!isMultiSelection && !editor_->GetIsPathEditMode()) {
            if (auto* session = editor_->GetActorSession()) session->DrawInspector(selectedObject, editor_->GetProjectWindow());
            // クリップ欄を開き、動作確認セッションが停止中なら代表Objectのポーズを編集できる。
            if (ImGui::CollapsingHeader("アニメーションクリップ編集")) {
                if (auto* session = editor_->GetActorSession(); session && session->IsActive())
                    ImGui::TextDisabled("動作確認を終了するとクリップを編集できます。");
                else {
                    auto* clip = editor_->GetAnimationClipEditor(); clip->SetTarget(selectedObject); clip->DrawImGui();
                }
            } else editor_->GetAnimationClipEditor()->StopPreview();
        } else {
            if (auto* session = editor_->GetActorSession()) session->Stop();
            editor_->GetAnimationClipEditor()->StopPreview();
        }

        if (editor_->GetIsPathEditMode()) {
            // 編集モード中は目立つ色（オレンジ）にして警告を表示
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0.4f, 0.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.5f, 0.2f, 1.0f));

            // パス編集を抜けるボタンでモードを解除し、Recorderが保持する点の選択も解除する。
            if (ImGui::Button(ICON_FA_CHECK_CIRCLE " パス編集を完了してロックを解除 (Exit Edit Mode)", ImVec2(-1, 45))) {
                editor_->SetIsPathEditMode(false);
                if (selectedObject->GetRecorder())
                    selectedObject->GetRecorder()->DeselectPin();
            }

            ImGui::PopStyleColor(2);
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), ICON_FA_EXCLAMATION_TRIANGLE " 現在パスを編集中です。");
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "    本体の移動・選択・削除はロックされています。");
        }
        else {
            if (ImGui::Button(ICON_FA_PENCIL_ALT " パスの軌跡を編集する (Enter Path Edit Mode)", ImVec2(-1, 35))) {
                editor_->SetIsPathEditMode(true);
            }
        }

        ImGui::Separator();

        // =========================================================================
        // パス編集中は、次の範囲の通常Inspector操作を無効にする。
        // =========================================================================
        // Camera専用欄の早期returnでもImGuiの無効化スタックを元に戻す。
        struct PathEditDisabledScope {
            explicit PathEditDisabledScope(bool disabled) { ImGui::BeginDisabled(disabled); }
            ~PathEditDisabledScope() { End(); }

            void End() {
                // 明示的な終了後にデストラクタから呼ばれても、無効化スタックを二度閉じない。
                if (active) {
                    ImGui::EndDisabled();
                    active = false;
                }
            }

            bool active = true;
        } pathEditScope(editor_->GetIsPathEditMode());

        // パス編集中に本体を変形・削除しないため、以降の通常Inspectorを一括で無効化する。
        ImGui::Spacing();

        // ボタン操作で複製は共通コマンドへ、単体保存は選択物のJSON保存へ渡す。
        if (ImGui::Button(ICON_FA_COPY " 複製 (Duplicate)")) {
            EditorCommandRegistry::GetInstance()->Execute(editorCommandId::kEditDuplicate);
        }

        ImGui::SameLine();

        if (ImGui::Button(ICON_FA_DOWNLOAD " 単体保存 (JSON更新)")) {
            editor_->SaveSingleObject();
        }

        ImGui::Spacing();

        // --- クラス名表示 ---
        ImGui::TextDisabled(ICON_FA_CUBES " クラス: %s", selectedObject->GetClassName().c_str());

        if (selectedObject->IsCameraObject()) {
            // Camera専用UIの描画で完了し、材質・Collider欄を表示しない。
            DrawCameraObjectInspector(editor_, currentScene, selectedObject);
            return;
        }

        const bool isManagedCharacter = GameplayStatusManager::IsManagedCharacter(selectedObject);

        static engine::Object3D* sTagLayerBufferOwner = nullptr;

        // テキスト入力の編集中バッファは選択Objectが変わった時だけ同期し、毎フレーム上書きしない。
        static char sTagBuffer[128] = {};
        static char sLayerBuffer[128] = {};

        if (sTagLayerBufferOwner != selectedObject) {
            sTagLayerBufferOwner = selectedObject;
            strcpy_s(sTagBuffer, selectedObject->GetTag().c_str());
            const std::string currentLayer = selectedObject->GetLayer().empty() ? "Default" : selectedObject->GetLayer();
            strcpy_s(sLayerBuffer, currentLayer.c_str());
        }

        ImGui::SeparatorText("Tag / Layer");
        ImGui::TextDisabled("Tagは役割名、Layerは処理対象の分類として使います。");
        const bool mixedTag = HasMixedInspectorValue(inspectorTargets, "identity.tag");

        // Tag入力が変わったら、各有効な選択対象の役割名を変更し保存対象にする。
        if (ImGui::InputText("Tag", sTagBuffer, sizeof(sTagBuffer))) {
            const std::string newTag = sTagBuffer;

            for (engine::Object3D* target : inspectorTargets) {
                if (target) {
                    target->SetTag(newTag);
                }
            }

            MarkInspectorTargetsDirty(editor_, inspectorTargets);
        }

        DrawMixedValueHint(mixedTag);

        const char* layerPresets[] = {
            "Default",
            "Player",
            "Enemy",
            "Stage",
            "Trigger",
            "Effect",
            "EditorOnly"
        };

        std::int32_t layerPresetIndex = 0;
        const std::string selectedLayer = selectedObject->GetLayer().empty() ? "Default" : selectedObject->GetLayer();

        // 既定の一覧に保存済みLayerがあれば、その項目を選択表示する。
        for (std::int32_t i = 0; i < IM_ARRAYSIZE(layerPresets); ++i) {
            if (selectedLayer == layerPresets[i]) {
                layerPresetIndex = i;
                break;
            }
        }

        const bool mixedLayer = HasMixedInspectorValue(inspectorTargets, "identity.layer");

        // 一覧で分類を変更すると、手入力欄と選択対象の分類を同じ値に揃える。
        if (ImGui::Combo("Layer Preset", &layerPresetIndex, layerPresets, IM_ARRAYSIZE(layerPresets))) {
            strcpy_s(sLayerBuffer, layerPresets[layerPresetIndex]);
            const std::string newLayer = sLayerBuffer;

            for (engine::Object3D* target : inspectorTargets) {
                if (target) {
                    target->SetLayer(newLayer);
                }
            }

            MarkInspectorTargetsDirty(editor_, inspectorTargets);
        }

        DrawMixedValueHint(mixedLayer);

        // 手入力で欄を空にした場合も、処理分類に使う値と表示欄の両方をDefaultに揃えます。
        if (ImGui::InputText("Layer", sLayerBuffer, sizeof(sLayerBuffer))) {
            const std::string newLayer = sLayerBuffer[0] == '\0' ? "Default" : std::string(sLayerBuffer);

            if (sLayerBuffer[0] == '\0') {
                strcpy_s(sLayerBuffer, "Default");
            }

            for (engine::Object3D* target : inspectorTargets) {
                if (target) {
                    target->SetLayer(newLayer);
                }
            }

            MarkInspectorTargetsDirty(editor_, inspectorTargets);
        }

        DrawMixedValueHint(mixedLayer);


        // Componentの有無を編集してから、後続の専用欄で表示・適用対象を判定する。
        DrawComponentPanel(editor_, selectedObject, inspectorTargets);

        // 現在の保存カテゴリを表示し、選び直されたときに有効な選択対象の保存先を更新する。
        const char* saveCategories[] = { "Object", "Player", "Enemy" };
        std::string currentCat = selectedObject->GetSaveCategory();
        std::int32_t catIndex = 0;
        if (currentCat == "Player")
            catIndex = 1;
        else if (currentCat == "Enemy") catIndex = 2;

        const bool mixedSaveCategory = HasMixedInspectorValue(inspectorTargets, "identity.saveCategory");

        if (ImGui::Combo(ICON_FA_FOLDER " 保存先カテゴリ", &catIndex, saveCategories, IM_ARRAYSIZE(saveCategories))) {
            for (engine::Object3D* object : inspectorTargets) {
                if (!object)
                    continue;
                object->SetSaveCategory(saveCategories[catIndex]);
            }

            MarkInspectorTargetsDirty(editor_, inspectorTargets);
        }

        DrawMixedValueHint(mixedSaveCategory);

        // 親付きなら親名と解除操作を出す。解除は代表だけで、ワールド姿勢を保つ。
        if (selectedObject->GetParent()) {
            ImGui::TextDisabled(ICON_FA_SITEMAP " 親: %s", selectedObject->GetParent()->GetName().c_str());

            if (ImGui::Button(ICON_FA_UNLINK " 親を解除 (Unparent)")) {
                // 親解除は代表Objectだけに行い、ワールド上の見た目の位置を維持する。
                selectedObject->SetParent(nullptr, true);
            }
        }
        else {
            ImGui::TextDisabled(ICON_FA_SITEMAP " 親: なし");
        }

        // --- Model Asset (InvisibleBoxでない場合のみ表示) ---
        if (selectedObject->GetClassName() != "InvisibleBox") {
            ImGui::Separator();
            ImGui::Text(ICON_FA_CUBE " モデルアセット: %s", selectedObject->GetModelName().c_str());

            // Player/Enemyのモデル変更は配置値ではなくタイプ共通設定へ案内し、個別差し替えと分ける。
            if (isManagedCharacter) {
                ImGui::TextDisabled("Player/Enemyのモデルは下の動作・データ調整で種類共通の設定として変更できます。");


            } else {
                ImGui::Button(ICON_FA_BOX_OPEN " [ ここにモデルをドロップして変更 ] ", ImVec2(-1, 30));

                // ドロップ枠はモデルIDとプリセットを受け付け、空のモデルIDでは差し替えない。
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MODEL_ASSET")) {
                        const std::string modelName = ReadEditorAssetDragPath(payload->Data, payload->DataSize);

                        if (!modelName.empty()) {
                            ModelManager::GetInstance()->LoadModel(modelName);
                            selectedObject->SetModel(modelName);
                            DebugConsole::GetInstance()->AddLog("Switched model to: " + modelName);
                        }
                    }

                    // プリセットデータのドロップ受付
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PRESET_ASSET")) {
                        const char* presetName = (const char*)payload->Data;
                        PresetManager::GetInstance()->ApplyPresetToObject(presetName, selectedObject);
                        DebugConsole::GetInstance()->AddLog("Applied preset: " + std::string(presetName));
                    }

                    ImGui::EndDragDropTarget();
                }
            }

            engine::Model* currentModel = selectedObject->GetModel();
            const std::int32_t meshCount = currentModel ? static_cast<std::int32_t>(currentModel->GetMeshCount()) : 0;

            // 特定メッシュだけを描く対象は現在の番号を出し、未分割の複数メッシュは子への分割を提案する。
            if (selectedObject->IsMeshDrawFiltered()) {
                ImGui::TextDisabled("描画Mesh: %d", selectedObject->GetMeshDrawIndex());
            }

            if (meshCount > 1 && !selectedObject->IsMeshDrawFiltered()) {
                ImGui::TextDisabled("Mesh数: %d", meshCount);

                if (ImGui::Button(ICON_FA_CUBE " メッシュを子Objectに分割", ImVec2(-1, 28))) {
                    editor_->SplitSelectedModelIntoMeshChildren();
                }
            } else if (meshCount == 1) {
                ImGui::TextDisabled("Mesh数: 1");
            }
        }

        // --- 可視性設定 ---
        ImGui::Separator();
        bool isVisible = selectedObject->GetIsVisible();
        const bool mixedVisible = HasMixedInspectorValue(inspectorTargets, "rendering.visible");
        // 代表値だけで全対象が同値に見えないよう、値の混在を示してから一括編集を受け付ける。
        PushMixedCheckbox(mixedVisible);

        // 表示・編集ロック・影のチェックが変更されたときは、対応する値を有効な選択対象へ揃える。
        if (ImGui::Checkbox(ICON_FA_EYE " 表示 (ゲーム内)", &isVisible)) {
            for (engine::Object3D* object : inspectorTargets) {
                if (!object)
                    continue;
                object->SetIsVisible(isVisible);
            }

            MarkInspectorTargetsDirty(editor_, inspectorTargets);
        }

        PopMixedCheckbox();
        bool isLocked = selectedObject->GetIsLocked();
        const bool mixedLocked = HasMixedInspectorValue(inspectorTargets, "editor.locked");
        PushMixedCheckbox(mixedLocked);

        if (ImGui::Checkbox(ICON_FA_LOCK " ロック (編集保護)", &isLocked)) {
            for (engine::Object3D* object : inspectorTargets) {
                if (!object)
                    continue;
                object->SetIsLocked(isLocked);
            }

            MarkInspectorTargetsDirty(editor_, inspectorTargets);
        }

        PopMixedCheckbox();
        bool castShadow = selectedObject->GetCastShadow();
        const bool mixedCastShadow = HasMixedInspectorValue(inspectorTargets, "rendering.castShadow");
        PushMixedCheckbox(mixedCastShadow);

        if (ImGui::Checkbox(ICON_FA_LIGHTBULB " 影を落とす", &castShadow)) {
            for (engine::Object3D* object : inspectorTargets) {
                if (!object)
                    continue;
                object->SetCastShadow(castShadow);
            }

            MarkInspectorTargetsDirty(editor_, inspectorTargets);
        }

        PopMixedCheckbox();

        // 個別の遮蔽物設定は複数選択にも反映し、保存時に同じ方針を保持する。
        const char* occlusionNames[] = {"自動", "透過する", "透過しない"};
        const std::int32_t occlusionMode = static_cast<std::int32_t>(selectedObject->GetCameraOcclusionMode());
        const bool mixedOcclusion = HasMixedInspectorValue(inspectorTargets, "rendering.cameraOcclusionMode");
        // 値が混在する場合は共通値なしを表示し、選択された項目を全対象の遮蔽設定へ反映する。
        if (ImGui::BeginCombo("視界確保", mixedOcclusion ? "複数の値" : occlusionNames[occlusionMode])) {
            for (std::int32_t mode = 0; mode < 3; ++mode) {
                if (ImGui::Selectable(occlusionNames[mode], !mixedOcclusion && mode == occlusionMode)) {
                    for (auto* object : inspectorTargets)
                        if (object) object->SetCameraOcclusionMode(static_cast<engine::Object3D::CameraOcclusionMode>(mode));
                    MarkInspectorTargetsDirty(editor_, inspectorTargets);
                }
            }
            ImGui::EndCombo();
        }
        // 視界確保の欄にポインターがある間だけ、方式の違いを補足する。
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("自動: 地形・装飾の既存判定。透過する: バリア等を個別に許可。\n透過しない: 攻撃予告などの表示を維持。自機と敵本体は透過しません。");

        // --- Transform編集 ---
        ImGui::Separator();
        ImGui::Text(ICON_FA_ARROWS_ALT " トランスフォーム (Transform)");
        Transform transformValue = *selectedObject->GetTransform();

        // 代表Objectの値を一時編集し、他の選択Objectへは移動・回転・拡縮の差分を適用する。
        Transform* transform = &transformValue;
        bool isTransformChanged = false;

        const engine::Vector3 beforePos = transform->translate;
        const bool mixedPosition = HasMixedInspectorValue(inspectorTargets, "transform.position");

        // 座標欄の前後差を各対象に加え、元の配置間隔を保ったまま移動する。
        if (ImGui::DragFloat3(ICON_FA_ARROWS_ALT " 座標 (Pos)", &transform->translate.x, 0.1f)) {
            ApplyPrimaryTransformDeltaToTargets(
                inspectorTargets,
                selectedObject,
                beforePos,
                transform->translate,
                transform->rotate,
                transform->rotate,
                transform->scale,
                transform->scale,
                true,
                false,
                false);
            isTransformChanged = true;
        }

        DrawMixedValueHint(mixedPosition);

        // 回転欄だけ度で受け取り、Transformへはラジアンを戻してEuler回転を編集元にする。
        engine::Vector3 rotDeg = { ToDegrees(transform->rotate.x), ToDegrees(transform->rotate.y), ToDegrees(transform->rotate.z) };
        const engine::Vector3 beforeRot = transform->rotate;
        const bool mixedRotation = HasMixedInspectorValue(inspectorTargets, "transform.rotation");

        if (ImGui::DragFloat3(ICON_FA_SYNC " 回転 (Rot)", &rotDeg.x, 1.0f, -360.0f, 360.0f)) {
            transform->rotate = { ToRadians(rotDeg.x), ToRadians(rotDeg.y), ToRadians(rotDeg.z) };
            transform->isQuaternionMaster = false;
            ApplyPrimaryTransformDeltaToTargets(
                inspectorTargets,
                selectedObject,
                transform->translate,
                transform->translate,
                beforeRot,
                transform->rotate,
                transform->scale,
                transform->scale,
                false,
                true,
                false);
            isTransformChanged = true;
        }

        DrawMixedValueHint(mixedRotation);
        const engine::Vector3 beforeScale = transform->scale;

        // 管理対象の拡縮はタイプの共通設定に従うため値を表示し、それ以外は拡縮倍率を対象群へ配る。
        if (isManagedCharacter) {
            ImGui::TextDisabled(ICON_FA_EXPAND_ARROWS_ALT " スケール: %.3f, %.3f, %.3f（基本ステータスのタイプ共通値）",
                transform->scale.x, transform->scale.y, transform->scale.z);
        } else {
            const bool mixedScale = HasMixedInspectorValue(inspectorTargets, "transform.scale");

            if (ImGui::DragFloat3(ICON_FA_EXPAND_ARROWS_ALT " スケール (Scale)", &transform->scale.x, 0.05f)) {
                ApplyPrimaryTransformDeltaToTargets(
                    inspectorTargets,
                    selectedObject,
                    transform->translate,
                    transform->translate,
                    transform->rotate,
                    transform->rotate,
                    beforeScale,
                    transform->scale,
                    false,
                    false,
                    true);
                isTransformChanged = true;
            }

            DrawMixedValueHint(mixedScale);
        }

        if (isTransformChanged) {
            // 最後に代表ObjectもSetter経由で確定し、全対象の行列とDirty状態を更新する。
            selectedObject->SetLocalTransform(transformValue);
            RefreshInspectorTargetMatrices(inspectorTargets);
            MarkInspectorTargetsDirty(editor_, inspectorTargets);
        }


        // --- コライダー設定 ---
        ImGui::Separator();

        if (ImGui::CollapsingHeader(ICON_FA_SHIELD_ALT " コリジョン設定 (Collision)", ImGuiTreeNodeFlags_DefaultOpen)) {
            // この見出しが開いている間に、衝突設定に続いて材質・粒子・エフェクト・判定ビットを表示する。
            engine::Object3D::ColliderConfig colConfig = selectedObject->GetColliderConfig();
            bool isColChanged = false;

            if (!isMultiSelection && selectedObject->GetModel()) {
                // 単体モデルのローカル頂点範囲から箱を合わせる。複数の足場を含むモデルは個別に分割してから使う。
                if (ImGui::Button("モデル外形からAABBを設定")) {
                    const engine::Model* model = selectedObject->GetModel();
                    const engine::Vector3 localMin = model->GetLocalAabbMin();
                    const engine::Vector3 localMax = model->GetLocalAabbMax();
                    // AABBのサイズ欄はローカルでの半幅を受け取ります。最小・最大の中点は中心オフセットに使います。
                    const engine::Vector3 halfSize = (localMax - localMin) * 0.5f;

                    // 全軸の半幅が十分なモデルだけ箱へ変換し、未設定の衝突属性・マスクは既定の値で補う。
                    if (halfSize.x > 0.001f && halfSize.y > 0.001f && halfSize.z > 0.001f) {
                        colConfig.type = engine::ColliderType::kAabb;
                        colConfig.center = (localMin + localMax) * 0.5f;
                        colConfig.size = halfSize;
                        colConfig.rotation = {0.0f, 0.0f, 0.0f};
                        colConfig.footprint.clear();
                        colConfig.useModelSurface = false;
                        colConfig.useModelVolume = false;

                        if (selectedObject->GetCollisionAttribute() == 0) {
                            selectedObject->SetCollisionAttribute(engine::collisionCategories::Get("ground"));
                        }

                        if (selectedObject->GetCollisionMask() == 0) {
                            selectedObject->SetCollisionMask(engine::collisionCategories::GetDefaultGameplayMask());
                        }

                        isColChanged = true;
                    }
                }

                ImGui::TextDisabled("箱形の単体モデル向け。離れた地形は分割して設定してください。");
            }

            const char* typeNames[] = { "なし (None)", "球 (Sphere)", "箱 (AABB)", "回転箱 (OBB)", "円柱 (Cylinder)", "リング (Ring)", "地形 (Terrain)" };
            std::int32_t currentTypeIndex = (std::int32_t)colConfig.type;

            // 形状を選び直したとき、回転箱またはリングの寸法が未設定なら初期寸法で補う。
            if (ImGui::Combo(ICON_FA_SHAPES " 形状タイプ", &currentTypeIndex, typeNames, IM_ARRAYSIZE(typeNames))) {
                colConfig.type = (engine::ColliderType)currentTypeIndex;

                if ((colConfig.type == engine::ColliderType::kObb || colConfig.type == engine::ColliderType::kRing) && colConfig.size.x == 0.0f) {
                    colConfig.size = { 1.0f, 1.0f, 1.0f };
                }

                // 箱以外の形状へ切り替えたときは、箱向けの輪郭・モデル形状判定を持ち越さない。
                if (colConfig.type != engine::ColliderType::kAabb && colConfig.type != engine::ColliderType::kObb) {
                    colConfig.footprint.clear();
                    colConfig.useModelSurface = false;
                    colConfig.useModelVolume = false;
                }

                isColChanged = true;
            }

            // 形状がある場合に中心と寸法を編集する。球は半径一つ、リングは内外の半径と厚み、その他は三軸の寸法を扱う。
            if (colConfig.type != engine::ColliderType::kNone) {
                if (ImGui::DragFloat3("中心オフセット", &colConfig.center.x, 0.05f))
                    isColChanged = true;

                if (colConfig.type == engine::ColliderType::kSphere) {
                    if (ImGui::DragFloat("半径 (Radius)", &colConfig.size.x, 0.05f, 0.0f, 100.0f)) {
                        colConfig.size.y = colConfig.size.z = colConfig.size.x;
                        isColChanged = true;
                    }
                }
                else if (colConfig.type == engine::ColliderType::kRing) {
                    if (ImGui::DragFloat("外径 (Outer Radius)", &colConfig.size.x, 0.05f, 0.0f, 100.0f))
                        isColChanged = true;
                    if (ImGui::DragFloat("内径 (Inner Radius)", &colConfig.size.z, 0.05f, 0.0f, 100.0f))
                        isColChanged = true;

                    // 厚み(Y)
                    if (ImGui::DragFloat("厚み (Thickness)", &colConfig.size.y, 0.05f, 0.0f, 10.0f))
                        isColChanged = true;
                }
                else {
                    if (ImGui::DragFloat3("サイズ (Size)", &colConfig.size.x, 0.05f, 0.0f, 100.0f))
                        isColChanged = true;
                }

                // モデルを持つ箱形状にだけ、上面または全体のモデル判定を選べる欄を出す。
                if ((colConfig.type == engine::ColliderType::kAabb || colConfig.type == engine::ColliderType::kObb) &&
                    selectedObject->GetModel()) {
                    // モデル全体の凸形状を選ぶと上面専用判定と手入力輪郭を外し、競合する設定を残さない。
                    if (ImGui::Checkbox("モデル全体の凸形状で判定", &colConfig.useModelVolume)) {
                        if (colConfig.useModelVolume) {
                            colConfig.useModelSurface = false;
                            colConfig.footprint.clear();
                        }
                        isColChanged = true;
                    }

                    ImGui::TextDisabled("側面・底面も自動生成。門などの凹形状は部品ごとに分けます。");
                    ImGui::BeginDisabled(colConfig.useModelVolume);
                    if (ImGui::Checkbox("モデル上面で接地判定", &colConfig.useModelSurface))
                        isColChanged = true;
                    ImGui::TextDisabled("上面のみモデル形状。側面は箱と輪郭で判定します。");
                    ImGui::EndDisabled();
                }

                // 回転箱とリングに限ってローカル回転を度単位で編集し、保存値はラジアンへ戻す。
                if (colConfig.type == engine::ColliderType::kObb || colConfig.type == engine::ColliderType::kRing) {
                    engine::Vector3 rotDegObj = { ToDegrees(colConfig.rotation.x), ToDegrees(colConfig.rotation.y), ToDegrees(colConfig.rotation.z) };

                    if (ImGui::DragFloat3("回転 (Rotation)", &rotDegObj.x, 1.0f, -360.0f, 360.0f)) {
                        colConfig.rotation = { ToRadians(rotDegObj.x), ToRadians(rotDegObj.y), ToRadians(rotDegObj.z) };
                        isColChanged = true;
                    }
                }

                if (colConfig.footprint.size() >= 3 &&
                    ImGui::TreeNode("上面の輪郭 (XZ・ローカル座標)")) {
                    // 凸輪郭だけを受け付ける。編集中に凹形状になった頂点は直前値へ戻す。
                    ImGui::TextUnformatted("凸形状の輪郭です。移動床の乗車判定にも使います。");

                    for (size_t index = 0; index < colConfig.footprint.size(); ++index) {
                        ImGui::PushID(static_cast<std::int32_t>(index));
                        const engine::Vector2 previous = colConfig.footprint[index];

                        if (ImGui::DragFloat2("頂点", &colConfig.footprint[index].x, 0.01f)) {
                            std::int32_t winding = 0;
                            bool convex = true;

                            for (size_t i = 0; i < colConfig.footprint.size(); ++i) {
                                const engine::Vector2& a = colConfig.footprint[i];
                                const engine::Vector2& b = colConfig.footprint[(i + 1) % colConfig.footprint.size()];
                                const engine::Vector2& c = colConfig.footprint[(i + 2) % colConfig.footprint.size()];
                                // 隣接する二辺の外積の符号で曲がる向きを確認します。直線に近い点は除き、一度でも向きが逆なら元の頂点へ戻します。
                                const float cross = (b.x - a.x) * (c.y - b.y) -
                                                    (b.y - a.y) * (c.x - b.x);
                                if (std::abs(cross) < 0.00001f)
                                    continue;
                                const std::int32_t sign = cross > 0.0f ? 1 : -1;

                                if (winding != 0 && sign != winding) {
                                    convex = false;
                                    break;
                                }
                                winding = sign;
                            }

                            // 曲がる向きが一方向にそろい、曲がりが一度以上あるときだけ頂点変更を確定する。
                            if (convex && winding != 0)
                                isColChanged = true;
                            else colConfig.footprint[index] = previous;
                        }

                        ImGui::PopID();
                    }

                    ImGui::TreePop();
                }

                // 確定した判定設定は有効な全選択対象へ配り、保存対象として記録する。
                if (isColChanged) {
                    for (engine::Object3D* object : inspectorTargets) {
                        if (!object)
                            continue;
                        object->SetColliderConfig(colConfig);
                    }

                    MarkInspectorTargetsDirty(editor_, inspectorTargets);
                }
            }
            else {
                // 形状を「なし」へ変更したときも全選択対象に反映し、以前の衝突を解除する。
                if (isColChanged) {
                    for (engine::Object3D* object : inspectorTargets) {
                        if (!object)
                            continue;
                        object->SetColliderConfig(colConfig);
                    }

                    MarkInspectorTargetsDirty(editor_, inspectorTargets);
                }
            }

            ImGui::Separator();

            if (ImGui::CollapsingHeader(ICON_FA_PALETTE " グラフィックス (Material)", ImGuiTreeNodeFlags_DefaultOpen)) {
                // 材質種別ごとに同じPBRフィールドを別の意味で使う項目がある。
                bool isGraphicsChanged = false;
                DrawMaterialInstancePanel(editor_, selectedObject, inspectorTargets);
                DrawDecalPanel(editor_, selectedObject, inspectorTargets);
                const char* matTypes[] = {
                                         "通常 (Standard)", "ガラス (Glass)", "氷・宝石 (Ice/Crystal)",
                                         "ホログラム (Hologram)", "消滅 (Dissolve)", "旧マグマ (Emissive)",
                                         "トゥーン調 (Cel Shaded)", "ローカルフォグ (Local Fog)",
                                         "水 (Water)", "新マグマ (Magma)", "分厚い氷 (Ice)", "炎 (Fire)",
                                         "レーザー (Laser)", "スライムジェル (Slime Gel)",
                                         "地面衝撃波 (Shockwave)", "水/マグマ接触 (Liquid Contact)",
                                         "ダメージ亀裂 (Damage Crack)", "上昇気流 (Updraft)",
                                         "スタン拘束 (Stun Bind)", "王冠解放 (Crown Unlock)",
                                         "毒胞子 (Poison Spore)", "雲 (Cloud)",
                                         "ゲートポータル (Gate Portal)",
                                         "アニメ調地形 (Stylized Terrain)",
                                         "ダッシュパネル (Dash Panel)",
                                         "スライム補正 (Slime Soft)",
                                         "風弾 (Wind Orb)",
                                         "プリズム結晶 (Prism Crystal)",
                                         "デカール (Surface Decal)",
                                         "草地 PBR (Meadow)", "草 PBR (Foliage)", "樹木 PBR (Tree)",
                                         "土の道付き草地 PBR (Meadow Path)",
                                         "入口の土側面 PBR (Island Soil)",
                                         "庭の景物 PBR (Garden Prop)",
                                         "火山岩 PBR (Basalt)",
                                         "箱型の草地 PBR (Solid Terrain)"
                };

                std::int32_t currentMatType = selectedObject->GetMaterialType();
                // 未知の材質番号では Combo の配列外を参照しないよう標準材質へ表示を戻す。
                if (currentMatType < 0)
                    currentMatType = 0;
                if (currentMatType > 36)
                    currentMatType = 0;
                // 材質の種類を変更すると、一括対象の材質を揃え、後続の専用欄も選んだ種類へ切り替える。
                if (ImGui::Combo(ICON_FA_PAINT_BRUSH " 質感 (Material Type)", &currentMatType, matTypes, IM_ARRAYSIZE(matTypes))) {
                    for (engine::Object3D* object : inspectorTargets) {
                        if (!object)
                            continue;
                        object->SetMaterialType(currentMatType);
                    }

                    MarkInspectorTargetsDirty(editor_, inspectorTargets);
                    isGraphicsChanged = true;
                }

                if (currentMatType == 32) {
                    // 専用材質では EnvIntensity を道の幅として解釈する。共通PBR項目と意味が異なる。
                    float pathWidth = selectedObject->GetEnvIntensity();

                    if (ImGui::SliderFloat("土の道の半幅（44m幅の地面用）", &pathWidth, 1.0f, 8.0f)) {
                        for (engine::Object3D* object : inspectorTargets) {
                            if (object)
                                object->SetEnvIntensity(pathWidth);
                        }

                        MarkInspectorTargetsDirty(editor_, inspectorTargets);
                        isGraphicsChanged = true;
                    }
                }

                if (currentMatType == 30 || currentMatType == 31) {
                    // 草・樹木では同じ保存フィールドを風揺れの強さとして使う。
                    float wind = selectedObject->GetEnvIntensity();

                    if (ImGui::SliderFloat("草木の風の強さ", &wind, 0.0f, 2.0f)) {
                        for (engine::Object3D* object : inspectorTargets) {
                            if (object)
                                object->SetEnvIntensity(wind);
                        }

                        MarkInspectorTargetsDirty(editor_, inspectorTargets);
                        isGraphicsChanged = true;
                    }

                    ImGui::TextWrapped("根元を固定して葉先だけ揺らします。色・スケールで草の色と丈を調整できます。");
                }

                if (currentMatType == 36) {
                    // 箱型地形の道は上面の材質で描き、板を追加せず幅を調整します。
                    // 格納されるのは道の半幅です。編集欄では全幅を表示し、書き戻し時に半分へ変換します。
                    float pathWidth = selectedObject->GetEnvIntensity() * 2.0f;
                    if (ImGui::SliderFloat("道の幅（0で非表示）", &pathWidth, 0.0f, 32.0f)) {
                        for (engine::Object3D* object : inspectorTargets) {
                            if (object)
                                object->SetEnvIntensity(pathWidth * 0.5f);
                        }
                        MarkInspectorTargetsDirty(editor_, inspectorTargets);
                        isGraphicsChanged = true;
                    }
                    ImGui::TextWrapped("上面・側面・底面は1つの箱モデルです。地形全体の大きさはTransform、判定はColliderで調整できます。");
                }

                // 標準と地形系PBR材質では、金属度・粗さを通常の意味で一括調整する。
                if (currentMatType == 0 || (currentMatType >= 29 && currentMatType <= 36)) {
                    float metallic = selectedObject->GetMetallic();

                    if (ImGui::SliderFloat("金属度 (Metallic)", &metallic, 0.0f, 1.0f)) {
                        for (engine::Object3D* object : inspectorTargets) {
                            if (!object)
                                continue;
                            object->SetMetallic(metallic);
                        }

                        MarkInspectorTargetsDirty(editor_, inspectorTargets);
                        isGraphicsChanged = true;
                    }

                    float roughness = selectedObject->GetRoughness();

                    if (ImGui::SliderFloat("粗さ (Roughness)", &roughness, 0.0f, 1.0f)) {
                        for (engine::Object3D* object : inspectorTargets) {
                            if (!object)
                                continue;
                            object->SetRoughness(roughness);
                        }

                        MarkInspectorTargetsDirty(editor_, inspectorTargets);
                        isGraphicsChanged = true;
                    }
                }
                else if (currentMatType == 23) {
                    // 地形材質は PBR の数値スロットを塗りと凹凸の調整値に転用する。
                    ImGui::Separator();
                    ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.45f, 1.0f), ICON_FA_TREE " --- Stylized Terrain Settings ---");
                    engine::Vector4 terrainColor = selectedObject->GetColor();

                    if (ImGui::ColorEdit4("差し色 (Accent Color)", &terrainColor.x)) {
                        for (engine::Object3D* object : inspectorTargets) {
                            if (!object)
                                continue;
                            object->SetColor(terrainColor);
                        }

                        MarkInspectorTargetsDirty(editor_, inspectorTargets);
                        isGraphicsChanged = true;
                    }

                    float textureBlend = selectedObject->GetRoughness();

                    if (ImGui::SliderFloat("テクスチャ反映量 (Texture Blend)", &textureBlend, 0.0f, 1.0f)) {
                        for (engine::Object3D* object : inspectorTargets) {
                            if (object)
                                object->SetRoughness(textureBlend);
                        }

                        MarkInspectorTargetsDirty(editor_, inspectorTargets);
                        isGraphicsChanged = true;
                    }

                    float paintStrength = selectedObject->GetMetallic();

                    if (ImGui::SliderFloat("塗りの濃さ / 色ムラ (Paint Strength)", &paintStrength, 0.0f, 1.0f)) {
                        for (engine::Object3D* object : inspectorTargets) {
                            if (object)
                                object->SetMetallic(paintStrength);
                        }

                        MarkInspectorTargetsDirty(editor_, inspectorTargets);
                        isGraphicsChanged = true;
                    }

                    float reliefStrength = selectedObject->GetEnvIntensity();

                    if (ImGui::SliderFloat("立体感 / 凹凸強度 (Relief)", &reliefStrength, 0.0f, 2.0f)) {
                        for (engine::Object3D* object : inspectorTargets) {
                            if (object)
                                object->SetEnvIntensity(reliefStrength);
                        }

                        MarkInspectorTargetsDirty(editor_, inspectorTargets);
                        isGraphicsChanged = true;
                    }

                    ImGui::TextDisabled("側面の陰影・色面の差・法線マップ強度をまとめて調整します");
                }
                else if (currentMatType == 24) {
                    // ダッシュパネルは代表Objectの粗さ・金属度の数値枠を流速・線密度に使う。
                    ImGui::Separator();
                    ImGui::TextColored(ImVec4(0.35f, 0.95f, 1.0f, 1.0f), ICON_FA_FORWARD " --- Dash Panel Settings ---");
                    float flowSpeed = selectedObject->GetRoughness();

                    if (ImGui::SliderFloat("流れる速さ (Flow Speed)", &flowSpeed, 0.0f, 1.0f)) {
                        selectedObject->SetRoughness(flowSpeed);
                        isGraphicsChanged = true;
                    }

                    float lineDensity = selectedObject->GetMetallic();

                    if (ImGui::SliderFloat("ライン密度 (Line Density)", &lineDensity, 0.0f, 1.0f)) {
                        selectedObject->SetMetallic(lineDensity);
                        isGraphicsChanged = true;
                    }
                }
                else if (currentMatType == 27) {
                    // 結晶材質では反射・分光の調整値を個別の意味で表示する。
                    ImGui::Separator();
                    ImGui::TextColored(ImVec4(0.72f, 0.92f, 1.0f, 1.0f), ICON_FA_GEM " --- Prism Crystal Settings ---");
                    float facetSoftness = selectedObject->GetRoughness();

                    if (ImGui::SliderFloat("面反射の柔らかさ (Facet Softness)", &facetSoftness, 0.02f, 0.70f)) {
                        for (engine::Object3D* object : inspectorTargets) {
                            if (object)
                                object->SetRoughness(facetSoftness);
                        }

                        isGraphicsChanged = true;
                    }

                    float dispersionStrength = selectedObject->GetMetallic();

                    if (ImGui::SliderFloat("分光の強さ (Dispersion)", &dispersionStrength, 0.0f, 1.0f)) {
                        for (engine::Object3D* object : inspectorTargets) {
                            if (object)
                                object->SetMetallic(dispersionStrength);
                        }

                        isGraphicsChanged = true;
                    }

                    // 結晶の環境反射を編集した場合は、値と一緒に環境マップの使用も有効にする。
                    float environmentIntensity = selectedObject->GetEnvIntensity();

                    if (ImGui::SliderFloat("環境反射の強さ (Environment)", &environmentIntensity, 0.0f, 3.0f)) {
                        for (engine::Object3D* object : inspectorTargets) {
                            if (!object)
                                continue;
                            object->SetEnableEnvMap(true);
                            object->SetEnvIntensity(environmentIntensity);
                        }

                        isGraphicsChanged = true;
                    }
                }

                // テクスチャの手動倍率と自動繰り返しは、変更された項目だけ代表Objectへ適用する。
                ImGui::Separator();
                ImGui::Text(ICON_FA_EXPAND_ARROWS_ALT " PBRテクスチャ繰り返し");
                engine::Vector2 textureTiling = selectedObject->GetTextureTiling();

                if (ImGui::DragFloat2("繰り返し倍率 (Tiling)", &textureTiling.x, 0.05f, 0.01f, 100.0f, "%.2f")) {
                    selectedObject->SetTextureTiling(textureTiling);
                    isGraphicsChanged = true;
                }

                bool autoTextureTiling = selectedObject->GetAutoTextureTiling();

                if (ImGui::Checkbox("スケールに合わせて自動繰り返し", &autoTextureTiling)) {
                    selectedObject->SetAutoTextureTiling(autoTextureTiling);
                    isGraphicsChanged = true;
                }

                ImGui::TextDisabled("大きい床や壁でPBRテクスチャが伸びる時に使います");

                // ローカルフォグ材質を選び、霧データを取得できた場合だけ色や散乱をそのデータへ反映する。
                if (currentMatType == 7) {
                    ImGui::Separator();
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.5f, 1.0f), ICON_FA_SMOG " --- Local Fog Settings ---");
                    auto* fogData = selectedObject->GetLocalFogData();

                    if (fogData) {
                        ImGui::ColorEdit4("Fog Color (霧の色)", &fogData->fogColor.x);
                        ImGui::DragFloat("Density (濃さ)", &fogData->fogDensity, 0.01f, 0.0f, 10.0f);
                        ImGui::DragFloat("Edge Fade (境界線のボケ)", &fogData->edgeFade, 0.01f, 0.0f, 1.5f);
                        ImGui::DragFloat("Noise Speed (揺らぐ速さ)", &fogData->noiseSpeed, 0.01f, 0.0f, 10.0f);
                        ImGui::DragFloat("Noise Scale (模様の細かさ)", &fogData->noiseScale, 0.01f, 0.0f, 10.0f);
                        ImGui::Spacing();
                        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), ICON_FA_SUN " --- Light Scattering ---");
                        ImGui::DragFloat("Scattering G (光の芯の強さ)", &fogData->scatteringG, 0.01f, 0.0f, 0.99f);
                        ImGui::DragFloat("Light Intensity (光の明るさ)", &fogData->scatteringIntensity, 0.01f, 0.0f, 5.0f);
                    }
                }

                DrawEffectMaterialSettings(selectedObject, currentMatType, isGraphicsChanged);
                ImGui::Separator();
                bool enableNormal = selectedObject->GetEnableNormalMap();

                // 法線画像と環境反射の有効状態を個別に切り替える。環境マップ強度は反射が有効な間に編集できる。
                if (ImGui::Checkbox(ICON_FA_MAP " 法線マップ (Normal Map) 有効化", &enableNormal)) {
                    selectedObject->SetEnableNormalMap(enableNormal);
                    isGraphicsChanged = true;
                }

                ImGui::Separator();
                bool enableEnv = selectedObject->GetEnableEnvMap();

                if (ImGui::Checkbox(ICON_FA_GLOBE " 環境マップ (IBL) 有効化", &enableEnv)) {
                    selectedObject->SetEnableEnvMap(enableEnv);
                    isGraphicsChanged = true;
                }

                if (enableEnv) {
                    float envIntensity = selectedObject->GetEnvIntensity();

                    if (ImGui::SliderFloat("環境マップ強度", &envIntensity, 0.0f, 5.0f)) {
                        selectedObject->SetEnvIntensity(envIntensity);
                        isGraphicsChanged = true;
                    }
                }

                // ライトの影響と影を落とす設定は別の判定として、編集されたチェックだけ反映する。
                bool enableLighting = selectedObject->GetEnableLighting();

                if (ImGui::Checkbox(ICON_FA_LIGHTBULB " ライト影響を受ける", &enableLighting)) {
                    selectedObject->SetEnableLighting(enableLighting);
                    isGraphicsChanged = true;
                }

                bool castShadow = selectedObject->GetCastShadow();

                if (ImGui::Checkbox("影を落とす (Cast Shadow)", &castShadow)) {
                    selectedObject->SetCastShadow(castShadow);
                    isGraphicsChanged = true;
                }

                if (!castShadow) {
                    ImGui::TextDisabled("影マップ描画だけを無効化します。ライト影響は別設定です。");
                }

                // テクスチャ候補は画面をまたいで保持し、明示的な更新操作で再読込する。
                static InspectorTextureCatalog sTextureCatalog;
                sTextureCatalog.EnsureLoaded();
                const auto& albedoPaths = sTextureCatalog.GetAlbedoPaths();
                const auto& normalPaths = sTextureCatalog.GetNormalPaths();
                const auto& armPaths = sTextureCatalog.GetOrmPaths();
                const auto& spriteTexturePaths = sTextureCatalog.GetSpritePaths();
                const auto& pbrPresets = sTextureCatalog.GetPbrPresets();

                ImGui::Separator();
                std::string currentTexturePath = selectedObject->GetTexturePath();
                std::string currentNormalPath = selectedObject->GetNormalMapPath();
                std::string currentOrmPathForPreset = selectedObject->GetOrmMapPath();

                // 現在の画像3枚が同じプリセットと一致した場合だけ、そのセット名を表示に使う。
                std::string currentPresetName = "未設定 (3枚セットを選択)";

                for (const InspectorTextureCatalog::PbrPreset& preset : pbrPresets) {
                    if (preset.albedoPath == currentTexturePath &&
                        preset.normalPath == currentNormalPath &&
                        preset.ormPath == currentOrmPathForPreset) {
                        currentPresetName = preset.name;
                        break;
                    }
                }

                // 文字一覧と球ギャラリーで同じ適用処理を使い、複数選択にも同じ結果を渡す。
                const auto applyPbrPreset = [&](const InspectorTextureCatalog::PbrPreset& preset) {
                    for (engine::Object3D* target : inspectorTargets) {
                        if (!target)
                            continue;
                        target->SetTexture(preset.albedoPath);
                        target->SetNormalMap(preset.normalPath);
                        target->SetOrmMap(preset.ormPath);
                        target->SetEnableNormalMap(true);
                    }

                    MarkInspectorTargetsDirty(editor_, inspectorTargets);
                    DebugConsole::GetInstance()->AddLog("PBR preset applied: " + preset.name);
                    isGraphicsChanged = true;
                };

                // コンボが開いた場合はセットを列挙し、選んだセットを適用する。既存の組み合わせには初期フォーカスを置く。
                if (ImGui::BeginCombo(ICON_FA_IMAGE " PBRプリセット (Albedo/Normal/ORM)", currentPresetName.c_str())) {
                    if (pbrPresets.empty()) {
                        ImGui::TextDisabled("同名の3枚セットが見つかりません。");
                    }

                    for (const InspectorTextureCatalog::PbrPreset& preset : pbrPresets) {
                        const bool isSelected =
                            preset.albedoPath == currentTexturePath &&
                            preset.normalPath == currentNormalPath &&
                            preset.ormPath == currentOrmPathForPreset;
                        if (ImGui::Selectable(preset.name.c_str(), isSelected)) {
                            applyPbrPreset(preset);
                        }

                        if (isSelected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }

                    ImGui::EndCombo();
                }

                // 球で比較するボタンからギャラリーを開き、画像または準備中のカードを選んだ場合も同じセットを適用する。
                if (ImGui::Button(ICON_FA_IMAGES " 球プレビューから選ぶ")) {
                    ImGui::OpenPopup("PBR球プレビュー");
                }

                ImGui::SetNextWindowSize(ImVec2(510.0f, 430.0f), ImGuiCond_Appearing);

                if (ImGui::BeginPopup("PBR球プレビュー")) {
                    ProjectWindow* projectWindow = editor_->GetProjectWindow();

                    // 素材パスの索引を作り直した後で、古い組み合わせで生成した球のサムネイルも無効化します。
                    if (ImGui::Button(ICON_FA_SYNC " 素材を再読込")) {
                        sTextureCatalog.Refresh();
                        if (projectWindow)
                            projectWindow->InvalidatePbrThumbnails();
                    }

                    ImGui::SameLine();
                    ImGui::TextDisabled("同じ照明で球に貼った見た目を比較します");
                    ImGui::Separator();
                    if (pbrPresets.empty())
                        ImGui::TextDisabled("Albedo・Normal・ORMの3枚セットがありません。");
                    const float cardWidth = 112.0f;
                    const std::int32_t columns = (std::max)(1, static_cast<std::int32_t>(ImGui::GetContentRegionAvail().x / cardWidth));

                    if (ImGui::BeginTable("PbrSphereGrid", columns)) {
                        for (const auto& preset : pbrPresets) {
                            ImGui::TableNextColumn();
                            ImGui::PushID(preset.name.c_str());
                            // 表示範囲内のカードだけ球プレビューを要求し、未準備の画像には待機表示を出す。
                            const uint64_t thumbnail = projectWindow && ImGui::IsRectVisible(ImVec2(96.0f, 96.0f))
                                ? projectWindow->GetPbrThumbnailGpuPtr(preset.name,
                                    preset.albedoPath, preset.normalPath, preset.ormPath) : 0;
                            const bool selected = preset.albedoPath == currentTexturePath &&
                                preset.normalPath == currentNormalPath && preset.ormPath == currentOrmPathForPreset;
                            if (selected)
                                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.38f, 0.60f, 1.0f));
                            const bool clicked = thumbnail
                                ? ImGui::ImageButton("##sphere", (ImTextureID)(uintptr_t)thumbnail, ImVec2(96.0f, 96.0f))
                                : ImGui::Button("撮影中...", ImVec2(96.0f, 96.0f));
                            if (selected)
                                ImGui::PopStyleColor();
                            if (clicked) {
                                applyPbrPreset(preset);
                                ImGui::CloseCurrentPopup();
                            }

                            if (ImGui::IsItemHovered()) {
                                ImGui::BeginTooltip();
                                ImGui::TextUnformatted(preset.name.c_str());
                                ImGui::TextDisabled("Albedo: %s", preset.albedoPath.c_str());
                                ImGui::TextDisabled("Normal: %s", preset.normalPath.c_str());
                                ImGui::TextDisabled("ORM: %s", preset.ormPath.c_str());
                                ImGui::EndTooltip();
                            }

                            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 100.0f);
                            ImGui::TextWrapped("%s", preset.name.c_str());
                            ImGui::PopTextWrapPos();
                            ImGui::PopID();
                        }

                        ImGui::EndTable();
                    }

                    ImGui::EndPopup();
                }

                if (!pbrPresets.empty()) {
                    ImGui::TextDisabled("同じベース名の3枚をまとめて適用します。");
                }

                // 個別の画像選択は代表Objectへ適用する。上のPBRセット一括適用とは対象範囲が異なる。
                const char* previewTextureValue = currentTexturePath.empty() ? "デフォルト (モデル固有)" : currentTexturePath.c_str();

                if (ImGui::BeginCombo(ICON_FA_IMAGE " 基本画像 (Diffuse / Sprite)", previewTextureValue)) {
                    if (!spriteTexturePaths.empty()) {
                        ImGui::TextDisabled("Sprite");

                        for (const std::string& path : spriteTexturePaths) {
                            bool isSelected = (currentTexturePath == path);

                            if (ImGui::Selectable(path.c_str(), isSelected)) {
                                selectedObject->SetTexture(path);
                                isGraphicsChanged = true;
                            }

                            if (isSelected)
                                ImGui::SetItemDefaultFocus();
                        }

                        ImGui::Separator();
                    }

                    if (!albedoPaths.empty()) {
                        ImGui::TextDisabled("PBR Albedo");

                        for (const std::string& path : albedoPaths) {
                            bool isSelected = (currentTexturePath == path);

                            if (ImGui::Selectable(path.c_str(), isSelected)) {
                                selectedObject->SetTexture(path);
                                isGraphicsChanged = true;
                            }

                            if (isSelected)
                                ImGui::SetItemDefaultFocus();
                        }

                        ImGui::Separator();
                    }

                    // 既定に戻す項目を選ぶと、上書きテクスチャを外すため空のパスを保存する。
                    if (ImGui::Selectable("デフォルトに戻す", currentTexturePath.empty())) {
                        selectedObject->SetTexture("");
                        isGraphicsChanged = true;
                    }

                    ImGui::EndCombo();
                }

                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SPRITE_FILE")) {
                        const char* spritePath = static_cast<const char*>(payload->Data);

                        if (spritePath) {
                            // Sprite一覧のドロップはルートからの相対パスなので、テクスチャの共通ルートを補って適用します。
                            selectedObject->SetTexture("Resources/sprite/" + std::string(spritePath));
                            isGraphicsChanged = true;
                        }
                    }

                    ImGui::EndDragDropTarget();
                }

                ImGui::SameLine();

                if (ImGui::Button(ICON_FA_SYNC_ALT " 更新##TextureList")) {
                    sTextureCatalog.Refresh();
                }

                if (IsSpriteCardObject(selectedObject)) {
                    ImGui::Separator();

                    // Sprite板で専用欄を開いたときに、表示初期化、寸法、正面向き、判定の解除を個別に操作できる。
                    if (ImGui::CollapsingHeader(ICON_FA_IMAGE " 2.5Dスプライト板", ImGuiTreeNodeFlags_DefaultOpen)) {
                        ImGui::TextDisabled("3D空間にSprite画像を置くための板です。画像は上の基本画像から変更できます。");

                        if (ImGui::Button(ICON_FA_MAGIC " 2.5D用に整える", ImVec2(-1, 28))) {
                            ConfigureSpriteCardObject(selectedObject);
                            currentMatType = 0;
                            isGraphicsChanged = true;
                        }

                        const Transform* cardTransform = selectedObject->GetTransform();
                        engine::Vector2 cardSize = { cardTransform->scale.x, cardTransform->scale.y };

                        // 幅と高さのいずれかを編集した場合は正の値へ制限し、板の奥行倍率は1に揃える。
                        if (ImGui::DragFloat2("表示サイズ (幅 / 高さ)", &cardSize.x, 0.02f, 0.01f, 100.0f, "%.2f")) {
                            selectedObject->SetScale({ (std::max)(0.01f, cardSize.x), (std::max)(0.01f, cardSize.y), 1.0f });
                            isTransformChanged = true;
                        }

                        if (ImGui::Button(ICON_FA_UNDO " 正面向きに戻す")) {
                            selectedObject->SetRotation({ 0.0f, 0.0f, 0.0f });
                            isTransformChanged = true;
                        }

                        ImGui::SameLine();

                        if (ImGui::Button(ICON_FA_SHIELD_ALT " 当たり判定なし")) {
                            engine::Object3D::ColliderConfig colliderConfig = selectedObject->GetColliderConfig();
                            colliderConfig.type = engine::ColliderType::kNone;
                            selectedObject->SetColliderConfig(colliderConfig);
                        }
                    }
                }

                // ノーマルとORMの個別設定は法線マップを使う対象に表示します。空文字は画像割り当ての解除です。
                if (enableNormal) {
                    std::string currentPath = selectedObject->GetNormalMapPath();
                    const char* previewValue = currentPath.empty() ? "未設定 (クリックで選択)" : currentPath.c_str();

                    if (ImGui::BeginCombo(ICON_FA_IMAGE " ノーマル画像", previewValue)) {
                        for (const std::string& path : normalPaths) {
                            bool isSelected = (currentPath == path);

                            if (ImGui::Selectable(path.c_str(), isSelected)) {
                                selectedObject->SetNormalMap(path);
                                isGraphicsChanged = true;
                            }

                            if (isSelected)
                                ImGui::SetItemDefaultFocus();
                        }

                        ImGui::Separator();

                        if (ImGui::Selectable("なし (クリア)", currentPath.empty())) {
                            selectedObject->SetNormalMap("");
                            isGraphicsChanged = true;
                        }

                        ImGui::EndCombo();
                    }

                    std::string currentOrmPath = selectedObject->GetOrmMapPath();
                    const char* previewOrmValue = currentOrmPath.empty() ? "未設定 (クリックで選択)" : currentOrmPath.c_str();

                    if (ImGui::BeginCombo(ICON_FA_IMAGE " ORMマップ (AO/粗さ/金属)", previewOrmValue)) {
                        for (const std::string& path : armPaths) {
                            bool isSelected = (currentOrmPath == path);

                            if (ImGui::Selectable(path.c_str(), isSelected)) {
                                selectedObject->SetOrmMap(path);
                                isGraphicsChanged = true;
                            }

                            if (isSelected)
                                ImGui::SetItemDefaultFocus();
                        }

                        ImGui::Separator();

                        if (ImGui::Selectable("なし (クリア)", currentOrmPath.empty())) {
                            selectedObject->SetOrmMap("");
                            isGraphicsChanged = true;
                        }

                        ImGui::EndCombo();
                    }
                }

                ImGui::Separator();
                const char* blendModes[] = { "なし (None)", "通常 (Normal)", "加算 (Add)", "減算 (Subtract)", "乗算 (Multiply)", "スクリーン (Screen)" };
                std::int32_t currentBlend = static_cast<std::int32_t>(selectedObject->GetBlendMode());

                if (ImGui::Combo(ICON_FA_ADJUST " 合成 (Blend Mode)", &currentBlend, blendModes, IM_ARRAYSIZE(blendModes))) {
                    selectedObject->SetBlendMode(static_cast<BlendMode>(currentBlend));
                    isGraphicsChanged = true;
                }

                // アニメ調地形は専用欄で差し色を扱うため、それ以外の材質だけ一般の色欄を表示する。
                if (currentMatType != 23) {
                    engine::Vector4 color = selectedObject->GetColor();

                    if (ImGui::ColorEdit4(ICON_FA_FILL_DRIP " 色 (Color)", &color.x)) {
                        selectedObject->SetColor(color);
                        isGraphicsChanged = true;
                    }
                }

                ImGui::Spacing();
                float emissive = selectedObject->GetEmissive();

                // 発光強度はHDRで使うため1を基準にし、出力を明るくする値を50まで入力できる。
                if (ImGui::DragFloat(ICON_FA_SUN " 発光強度 (Emissive)", &emissive, 0.1f, 1.0f, 50.0f, "%.1f")) {
                    selectedObject->SetEmissive(emissive);
                    isGraphicsChanged = true;
                }
            }

            ImGui::Separator();

            ImGui::Separator();
            if (HasRegisteredComponent(selectedObject, "ParticleEmitter") &&
                ImGui::CollapsingHeader(ICON_FA_FIRE " パーティクル")) {
                // 旧CPU名とGPU Preset名は別の選択肢としてObjectへ保持する。
                // --- CPU Particle (Old) ---
                const auto& cpuParamsMap = ParticleManager::GetInstance()->GetParamsMap();
                std::vector<const char*> cpuItemNames;
                std::int32_t currentCpuIndex = 0;
                std::int32_t cpuIdx = 0;
                cpuItemNames.push_back("None (CPU)");

                std::string currentCpuName = selectedObject->GetParticleName();
                if (currentCpuName.empty())
                    currentCpuIndex = 0;

                // 保存名と一致するCPUプリセットを選択表示し、先頭のNoneを選ぶと名前を解除する。
                for (const auto& [name, param] : cpuParamsMap) {
                    cpuItemNames.push_back(name.c_str());
                    if (name == currentCpuName)
                        currentCpuIndex = cpuIdx + 1;
                    cpuIdx++;
                }

                if (ImGui::Combo("CPU Effect", &currentCpuIndex, cpuItemNames.data(), (std::int32_t)cpuItemNames.size())) {
                    if (currentCpuIndex == 0)
                        selectedObject->SetParticleName("");
                    else selectedObject->SetParticleName(cpuItemNames[currentCpuIndex]);
                }

                ImGui::Separator();

                // --- GPU Particle (New) ---
                const auto& gpuPresets = GpuParticleManager::GetInstance()->GetPresets();
                std::vector<const char*> gpuItemNames;
                std::int32_t currentGpuIndex = 0;
                std::int32_t gpuIdx = 0;
                gpuItemNames.push_back("None (GPU)");

                std::string currentGpuName = selectedObject->GetGpuParticleName();
                if (currentGpuName.empty())
                    currentGpuIndex = 0;

                // GPU候補も保存名に一致すれば選択表示する。一覧に無い保存名は警告の対象になる。
                for (const auto& [name, config] : gpuPresets) {
                    gpuItemNames.push_back(name.c_str());
                    if (name == currentGpuName)
                        currentGpuIndex = gpuIdx + 1;
                    gpuIdx++;
                }

                // ここではプリセット名の選択だけをObjectへ保存し、粒子の即時生成操作にはしない。
                if (ImGui::Combo("GPU Effect", &currentGpuIndex, gpuItemNames.data(), (std::int32_t)gpuItemNames.size())) {
                    if (currentGpuIndex == 0)
                        selectedObject->SetGpuParticleName("");
                    else selectedObject->SetGpuParticleName(gpuItemNames[currentGpuIndex]);
                }

                if (!currentGpuName.empty() && gpuPresets.find(currentGpuName) == gpuPresets.end()) {
                    ImGui::TextColored(ImVec4(1, 0, 0, 1), "Warning: GPU JSON not found!");
                }
            }

            ImGui::Separator();
            if (HasRegisteredComponent(selectedObject, "MeshEffect") &&
                ImGui::CollapsingHeader(ICON_FA_MAGIC " メッシュエフェクト (Mesh Effect)")) {
                // effect JSONを列挙し、二つのSlotで別々のパスを選べるようにする。
                std::vector<std::string> effectPaths;
                std::vector<std::string> effectDisplayNames;

                effectPaths.push_back("");
                effectDisplayNames.push_back("None");

                std::string effectDir = "Resources/json/effect";

                for (const auto& key : JsonAssetBrowser::ListKeys(effectDir)) {
                    effectPaths.push_back(effectDir + "/" + key + ".json");
                    effectDisplayNames.push_back(key);
                }

                std::vector<const char*> itemNames;

                for (const auto& displayName : effectDisplayNames) {
                    itemNames.push_back(displayName.c_str());
                }

                // 各スロットは保存済みパスに一致する項目を選択表示し、選び直されたときだけ代表Objectに書き戻す。
                std::string currentEff1 = selectedObject->GetMeshEffect1Name();
                std::int32_t selectIdx1 = 0;

                for (size_t i = 0; i < effectPaths.size(); ++i) {
                    if (effectPaths[i] == currentEff1) {
                        selectIdx1 = static_cast<std::int32_t>(i);
                        break;
                    }
                }

                if (ImGui::Combo("Slot 1 (Floor etc.)", &selectIdx1, itemNames.data(), (std::int32_t)itemNames.size())) {
                    selectedObject->SetMeshEffect1Name(effectPaths[selectIdx1]);
                }

                // --- スロット2 ---
                std::string currentEff2 = selectedObject->GetMeshEffect2Name();
                std::int32_t selectIdx2 = 0;

                for (size_t i = 0; i < effectPaths.size(); ++i) {
                    if (effectPaths[i] == currentEff2) {
                        selectIdx2 = static_cast<std::int32_t>(i);
                        break;
                    }
                }

                if (ImGui::Combo("Slot 2 (Pillar etc.)", &selectIdx2, itemNames.data(), (std::int32_t)itemNames.size())) {
                    selectedObject->SetMeshEffect2Name(effectPaths[selectIdx2]);
                }
            }

            ImGui::Separator();
            // 属性は自分、マスクは相手の分類。ビット列が変わったときに代表Objectへ適用し、保存対象として記録する。
            uint32_t currentAttr = selectedObject->GetCollisionAttribute();
            DrawAttributeSelector(ICON_FA_TAGS " 自分の属性 (Attribute)", &currentAttr);

            if (currentAttr != selectedObject->GetCollisionAttribute()) {
                selectedObject->SetCollisionAttribute(currentAttr);
                MarkInspectorTargetsDirty(editor_, inspectorTargets);
            }

            uint32_t currentMask = selectedObject->GetCollisionMask();
            DrawAttributeSelector(ICON_FA_TAGS " 衝突対象 (Mask)", &currentMask);

            if (currentMask != selectedObject->GetCollisionMask()) {
                selectedObject->SetCollisionMask(currentMask);
                MarkInspectorTargetsDirty(editor_, inspectorTargets);
            }
        }

        if (HasRegisteredComponent(selectedObject, "Animator")) {
            // ==========================================
            // 1. ボーンアニメーション設定
            // ==========================================
            ImGui::Separator();
            ImGui::Text(ICON_FA_BONE " 【ボーンアニメーション】");
            // 複数選択でもAnimatorを持つObjectに絞り、持たないObjectへControllerを追加しない。
            const std::vector<engine::Object3D*> animatorTargets =
                CollectRegisteredComponentTargets(inspectorTargets, "Animator");

        std::string controllerPath = selectedObject->GetAnimatorControllerPath();
        bool hasMixedController = false;

        for (engine::Object3D* target : animatorTargets) {
            if (target && target->GetAnimatorControllerPath() != controllerPath) {
                hasMixedController = true;
                break;
            }
        }

        std::string controllerPreview = "(なし)";

        // 対象間のControllerに差があれば混在表示、揃っていれば設定ファイル名または未設定を出す。
        if (hasMixedController) {
            controllerPreview = "(複数値)";
        } else if (!controllerPath.empty()) {
            controllerPreview = fs::path(controllerPath).stem().string();
        }

        if (ImGui::BeginCombo("Animator Controller##BoneAnim", controllerPreview.c_str())) {
            const bool noneSelected = !hasMixedController && controllerPath.empty();

            // 未設定を選ぶと適用前を控えて全Animator対象のControllerを外し、まとめて履歴に登録する。
            if (ImGui::Selectable("(なし)", noneSelected)) {
                const auto beforeStates = editor_->CaptureObjectStates(animatorTargets);

                for (engine::Object3D* target : animatorTargets) {
                    if (target) {
                        target->ClearAnimatorController();
                    }
                }

                editor_->RegisterObjectsEdited(beforeStates, "Animator Controller Clear");
            }

            std::vector<std::string> controllerFiles;
            const fs::path controllerDirectory = "Resources/json/animator";

            // 登録フォルダーがある場合だけ列挙し、ファイルの拡張子を小文字化してJSONに絞る。
            if (fs::exists(controllerDirectory) && fs::is_directory(controllerDirectory)) {
                for (const auto& entry : fs::directory_iterator(controllerDirectory)) {
                    if (entry.is_regular_file() && ToLowerAscii(entry.path().extension().string()) == ".json") {
                        controllerFiles.push_back(entry.path().stem().string());
                    }
                }
            }

            std::sort(controllerFiles.begin(), controllerFiles.end());

            for (const std::string& fileName : controllerFiles) {
                const bool isSelected = !hasMixedController && fs::path(controllerPath).stem().string() == fileName;

                if (ImGui::Selectable(fileName.c_str(), isSelected)) {
                    // 適用前を記録し、少なくとも一つ読み込めた場合に編集履歴へ登録する。全対象の成功判定ではない。
                    const auto beforeStates = editor_->CaptureObjectStates(animatorTargets);
                    bool assigned = false;

                    for (engine::Object3D* target : animatorTargets) {
                        if (target && target->SetAnimatorController(fileName)) {
                            assigned = true;
                        }
                    }

                    if (assigned) {
                        editor_->RegisterObjectsEdited(beforeStates, "Animator Controller Assign");
                    } else {
                        DebugConsole::GetInstance()->AddLog("Animator Controllerの読み込みに失敗しました: " + fileName);
                    }
                }

                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }

            ImGui::EndCombo();
        }

        // Controllerが設定済みなら、現在ステート名か未再生を表示する。
        if (selectedObject->HasAnimatorController()) {
            const std::string currentState = selectedObject->GetAnimatorCurrentStateName();
            ImGui::TextDisabled("現在State: %s", currentState.empty() ? "(未再生)" : currentState.c_str());
        }

        // 編集画面へ切り替える前に、現在選択しているObjectをアニメーション確認用の対象として渡します。
        if (ImGui::Button("Animator Controller Editorを開く##BoneAnim")) {
            editor_->GetAnimatorControllerEditor()->SetPreviewTarget(selectedObject);
            EditorManager::GetInstance()->SetSelectedObject(editor_->GetAnimatorControllerEditor());
        }

        ImGui::TextDisabled("Controller未設定時は下の旧単発クリップ設定を使用します。現行制作ではControllerを推奨します。");

        char animNameBuf[64];
        strncpy_s(animNameBuf, selectedObject->GetAnimationName().c_str(), sizeof(animNameBuf));

        // 旧単発クリップの名前やループは入力が変わった項目を代表Objectに保持する。
        if (ImGui::InputText("アニメ名##BoneAnim", animNameBuf, sizeof(animNameBuf))) {
            selectedObject->SetAnimationName(animNameBuf);
        }

            bool animationLoop = selectedObject->IsAnimationLoop();
            if (ImGui::Checkbox("ループ再生##BoneAnim", &animationLoop)) {
                selectedObject->SetAnimationLoop(animationLoop);
            }
        }

        // PathMoverを持つ代表Objectだけ、パス移動の専用欄を出す。
        if (HasRegisteredComponent(selectedObject, "PathMover")) {
            DrawPathMoverSection(selectedObject);
        }

        // ゲーム固有欄は代表Objectへ委譲し、ここまで開いた通常編集の無効化スコープを閉じる。
        DrawGameplayDataSection(selectedObject);
        pathEditScope.End();
        ImGui::Separator();

        // 削除ボタンは共通コマンドを発行し、削除対象を指したままにしないよう編集選択を外す。
        if (ImGui::Button(ICON_FA_TRASH_ALT " オブジェクト削除", ImVec2(-1, 0))) {
            EditorCommandRegistry::GetInstance()->Execute(editorCommandId::kEditDelete);
            EditorManager::GetInstance()->ClearSelection();
        }

        // --- Gizmo 操作切替 ---
        ImGui::Separator();
        ImGui::Text(ICON_FA_HAND_POINTER " ギズモ操作モード:");
        static ImGuizmo::OPERATION sCurrentOperation = ImGuizmo::TRANSLATE;

        // ImGuiのウィンドウにフォーカスがない間に、T/R/Sでこの欄の操作種別を変更する。
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow)) {
            if (ImGui::IsKeyPressed(ImGuiKey_T))
                sCurrentOperation = ImGuizmo::TRANSLATE;
            if (ImGui::IsKeyPressed(ImGuiKey_R))
                sCurrentOperation = ImGuizmo::ROTATE;
            if (ImGui::IsKeyPressed(ImGuiKey_S))
                sCurrentOperation = ImGuizmo::SCALE;
        }

        // 各ボタンを選ぶと欄内の操作種別を移動・回転・拡縮のいずれかへ切り替える。
        if (ImGui::RadioButton("移動", sCurrentOperation == ImGuizmo::TRANSLATE))
            sCurrentOperation = ImGuizmo::TRANSLATE;
        ImGui::SameLine();
        if (ImGui::RadioButton("回転", sCurrentOperation == ImGuizmo::ROTATE))
            sCurrentOperation = ImGuizmo::ROTATE;
        ImGui::SameLine();
        if (ImGui::RadioButton("拡大縮小", sCurrentOperation == ImGuizmo::SCALE))
            sCurrentOperation = ImGuizmo::SCALE;
    }
#endif


}

