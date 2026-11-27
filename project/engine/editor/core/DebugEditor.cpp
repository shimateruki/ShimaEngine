#define NOMINMAX
#include <cstdint>
#include "DebugEditor.h"
#include <numbers>
#include "EditorToolSettings.h"
#include "game/ui/SettingsMenuOverlay.h"
#include "SceneManager.h"
#include "BaseScene.h"
#include "Object3D.h"
#include "imgui.h"
#include <fstream>
#include <string>
#include <vector>
#include "json.hpp"
#include "ImGuizmo.h"
#include "CameraManager.h"
#include "WinApp.h"
#include "Math.h"
#include "DirectXCommon.h"
#include "CollisionConfig.h"
#include "ModelManager.h"
#include "InputManager.h"
#include <cmath>
#include <cassert>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include "GhostRecorder.h"
#include "game/system/cinematic/GhostDirector.h"
#include "engine/editor/graphics/camera/CameraEditor.h"
#include "Transform.h"
#include "ParticleManager.h"
#include "EditorManager.h"
#include "PostEffectEditor.h"
#include "SpriteDebugEditor.h"
#include "ParticleEditor.h"
#include "GpuParticleEditor.h"
#include "VfxSequencerEditor.h"
#include "LightEditor.h"
#include "IconsFontAwesome5.h"
#include "AssetDatabase.h"

#include <DebugConsole.h>
#include <CollisionManager.h>
#include <filesystem> // ファイル操作用
#include <game/system/combat/BulletManager.h>
#include "engine/editor/assets/preset/PresetManager.h"
#include <PresetEditor.h>
#include <MeshEffectManager.h>
#include "EditorCommandRegistry.h"
#include <unordered_map>
#include <unordered_set>
#include <utility>
namespace fs = std::filesystem;




// Gizmoが返す度単位の回転を、Object3dへ渡すラジアン値に変換します。
float ToRadians(float degrees) { return degrees * (std::numbers::pi_v<float> / 180.0f); }

namespace {
    std::string GetLeafName(const std::string& path) {
        size_t slash = path.find_last_of("/\\");

        // 区切りがなければ入力全体を使う。空文字や末尾が区切りのときは既定名を返す。
        if (slash == std::string::npos) {
            return path.empty() ? "Preset" : path;
        }

        std::string leaf = path.substr(slash + 1);
        return leaf.empty() ? "Preset" : leaf;
    }

    bool SceneHasObjectName(BaseScene* scene, const std::string& name) {
        // 現在シーン内だけを調べ、別シーンの同名Objectは配置名の衝突としない。
        if (!scene)
            return false;

        for (const auto& object : scene->GetObjects()) {
            if (object && object->GetName() == name) {
                return true;
            }
        }

        return false;
    }

    bool ReservedHasName(const std::vector<std::string>& reservedNames, const std::string& name) {
        return std::find(reservedNames.begin(), reservedNames.end(), name) != reservedNames.end();
    }

    std::string MakeUniquePresetObjectNameImpl(BaseScene* scene, const std::string& baseName, const std::vector<std::string>& reservedNames) {
    // シーンと今回の配置候補の両方を調べ、衝突しない名前を作る。
        std::string base = baseName.empty() ? "PresetObject" : GetLeafName(baseName);

        if (!SceneHasObjectName(scene, base) && !ReservedHasName(reservedNames, base)) {
            return base;
        }

        // 基底名が使われていれば番号付きの候補を試し、上限に達した場合は _New を返す。
        for (std::int32_t index = 1; index < 10000; ++index) {
            std::string candidate = base + "_" + std::to_string(index);

            if (!SceneHasObjectName(scene, candidate) && !ReservedHasName(reservedNames, candidate)) {
                return candidate;
            }
        }

        return base + "_New";
    }
}

std::string DebugEditor::MakeUniquePresetObjectName(BaseScene* scene, const std::string& baseName,
    const std::vector<std::string>& reservedNames) {
    return MakeUniquePresetObjectNameImpl(scene, baseName, reservedNames);
}

// 選択表示や配置の箱には全軸で正の幅が必要。厚みが 0 の箱は別の境界へ切り替える。
bool DebugEditor::IsValidAabb(const engine::Aabb& aabb) {
    return aabb.max.x > aabb.min.x &&
        aabb.max.y > aabb.min.y &&
        aabb.max.z > aabb.min.z;
}

engine::Vector3 DebugEditor::GetAabbCenter(const engine::Aabb& aabb) {
    return {
        (aabb.min.x + aabb.max.x) * 0.5f,
        (aabb.min.y + aabb.max.y) * 0.5f,
        (aabb.min.z + aabb.max.z) * 0.5f
    };
}

void DebugEditor::AssignPresetInstanceNames(BaseScene* scene, const std::string& presetName,
    std::vector<std::unique_ptr<engine::Object3D>>& objects, PresetRootName rootNameSource) {
    // 生成結果が空ならルートも子も命名できないため終了する。
    if (objects.empty())
        return;

    std::vector<std::string> reservedNames;
    std::string baseName = GetLeafName(presetName);

    // 表示名での配置が指定されたときだけ、プリセットの表示名をルート名候補にする。
    if (rootNameSource == PresetRootName::kDisplayName) {
        const json& preset = PresetManager::GetInstance()->GetPreset(presetName);
        baseName = preset.value("presetDisplayName", baseName);
    }

    // シーン内の既存名と今回使用した名前の両方を避ける。子の命名時にもルート名を予約済みとして扱う。
    const std::string rootName = MakeUniquePresetObjectName(scene, baseName, reservedNames);
    objects.front()->SetName(rootName);
    reservedNames.push_back(rootName);

    for (size_t index = 1; index < objects.size(); ++index) {
        if (!objects[index])
            continue;

        std::string childBaseName = objects[index]->GetName();

        // 無名の子はルート名に接尾辞を付けた名前を起点にする。
        if (childBaseName.empty()) {
            childBaseName = rootName + "_Child";
        }

        const std::string uniqueName = MakeUniquePresetObjectName(scene, childBaseName, reservedNames);
        objects[index]->SetName(uniqueName);
        reservedNames.push_back(uniqueName);
    }
}

// ========================================================================
// 初期化
// ========================================================================
void DebugEditor::Initialize(SceneManager* sceneManager, engine::DirectXCommon* dxCommon) {
    // Editor各機能へSceneと描画基盤を接続し、前Sceneの選択・Undo状態は破棄します。
    sceneManager_ = sceneManager;
    dxCommon_ = dxCommon;
    selectedObject_ = nullptr;
    selectedObjects_.clear();
    lastUpdatedScene_ = nullptr;
    EditorPropertyRegistry::GetInstance()->InitializeBuiltInProperties();
    EditorTransactionManager::GetInstance()->Clear();
    AssetDatabase* assetDatabase = AssetDatabase::GetInstance();

    // 索引は初期化の成否をログに残す。失敗時も残りのエディター初期化は続ける。
    if (!assetDatabase->Initialize("Resources", true)) {
        DebugConsole::GetInstance()->AddLog("Asset Database Error: Resourcesを初期化できませんでした。");
    }
    else {
        DebugConsole::GetInstance()->AddLog("Asset Database: 起動時の差分索引を開始しました。");
    }

    PresetManager::GetInstance()->Initialize();
    PresetEditor::GetInstance()->Initialize();
    PresetEditor::GetInstance()->SetPlacePresetCallback([this](const std::string& presetName) {
        // 配置の依頼は、現在の編集シーンがあり、再生・仮配置・パス編集のいずれでもない場合に受け付ける。
        if (!sceneManager_ || !sceneManager_->GetCurrentScene())
            return;
        if (sceneManager_->IsPlaying() || previewObject_ || isPathEditMode_)
            return;

        BaseScene* currentScene = sceneManager_->GetCurrentScene();
        engine::Object3DCommon* common = currentScene->GetObject3DCommon();
        // 生成基盤がないか一つも生成できなければ、名前割り当てやプレビューを始めない。
        if (!common)
            return;

        auto objects = PresetManager::GetInstance()->CreateObjectsFromPreset(presetName, common);
        if (objects.empty())
            return;

        AssignPresetInstanceNames(currentScene, presetName, objects, PresetRootName::kDisplayName);

        gameViewCreateMenuMousePos_ = gameViewMousePos_;
        StartGameViewCreatePreview(std::move(objects), "Create Preset: " + presetName);
    });
    PresetEditor::GetInstance()->SetBrushPresetCallback([this](const std::string& presetName) {
        StartPresetBrush(presetName);
    });
    hierarchyWindow_.Initialize(this);
    projectWindow_.Initialize(this, dxCommon);
    PresetEditor::GetInstance()->SetThumbnailProvider([this](const std::string& presetName) {
        return projectWindow_.GetPresetThumbnailGpuPtr(presetName);
    });
    PresetEditor::GetInstance()->SetPreviewProvider([this](const std::string& presetName, float yaw, float pitch, float zoom) {
        return projectWindow_.GetPresetPreviewGpuPtr(presetName, yaw, pitch, zoom);
    });
    PresetEditor::GetInstance()->SetSelectionCallback([this]() {
        SetSelectedObject(nullptr);
        EditorManager::GetInstance()->SetSelectedObject(PresetEditor::GetInstance());
    });
    PresetEditor::GetInstance()->SetThumbnailInvalidator([this](const std::string& name) {
        projectWindow_.InvalidatePresetThumbnail(name);
    });
    // 子ウィンドウへ編集主体を渡し、描画・プレビュー・復旧サービスも起動する。
    inspectorWindow_.Initialize(this);
    serializer_.Initialize(this);
    primitiveDrawer_.Initialize(dxCommon);
    EffectPreviewStage::GetInstance()->Initialize(sceneManager, dxCommon);
#ifdef USE_IMGUI
    animationWorkbench_.Initialize(sceneManager, dxCommon);
#endif
    animatorControllerEditor_.Initialize(sceneManager, this);
    textSpriteGenerator_.Initialize(sceneManager, this);
    assetAuditWindow_.Initialize(this);

    gameDataDebugEditor_.Initialize(sceneManager);
    audioSettingsWindow_.Initialize(this);
    executablePackageWindow_.Initialize(this);
    captureToolWindow_.Initialize(this);
    InitializeCrashRecovery();

    const fs::path notificationLog = "Resources/.cache/dds_cache_notifications.jsonl";
    ddsCacheNotificationReadOffset_ = 0;

    // 既存の通知ログは末尾から監視する。サイズが読めなければ先頭から始める。
    if (fs::exists(notificationLog)) {
        std::error_code ec;
        ddsCacheNotificationReadOffset_ = fs::file_size(notificationLog, ec);

        if (ec) {
            ddsCacheNotificationReadOffset_ = 0;
        }
        else {
            const auto writeTime = fs::last_write_time(notificationLog, ec);

            if (!ec) {
                const auto now = fs::file_time_type::clock::now();
                // 通常は起動前の通知を読み飛ばすが、直前 30 秒の変換結果は起動後にも通知する。
                const auto recentWindow = std::chrono::seconds(30);

                if (writeTime <= now && now - writeTime <= recentWindow) {
                    ddsCacheNotificationReadOffset_ = 0;
                }
            }
        }
    }
}

bool DebugEditor::BeginPrefabEditSession(const std::string& prefabName) {
    // 再生中や未登録Prefabは開かず、編集対象だけを隔離するセッションを作ります。
    if (prefabName.empty() || !sceneManager_ || !sceneManager_->GetCurrentScene() ||
        sceneManager_->IsPlaying() || !PresetManager::GetInstance()->HasPrefab(prefabName)) {
        return false;
    }

    // 同じPrefabなら編集を継続し、別名なら前セッションを取り消してから生成する。
    if (prefabEditMode_) {
        if (prefabEditName_ == prefabName) {
            return true;
        }

        CancelPrefabEditSession();
    }

    BaseScene* scene = sceneManager_->GetCurrentScene();
    engine::Object3DCommon* common = scene->GetObject3DCommon();

    // 描画基盤と生成済みルートを確認し、どちらかが無ければシーンの可視状態を変更しない。
    if (!common) {
        return false;
    }

    auto prefabObjects = PresetManager::GetInstance()->CreateObjectsFromPrefab(prefabName, common);

    if (prefabObjects.empty() || !prefabObjects.front()) {
        return false;
    }

    ClearObjectSelection();
    EditorManager::GetInstance()->ClearSelection();
    EditorTransactionManager::GetInstance()->Clear();
    prefabEditPreviousVisibility_.clear();

    // 元の表示状態を控えてからシーン全体を隠す。Prefab の編集用オブジェクトは次の段階で追加する。
    for (auto& sceneObject : scene->GetObjects()) {
        if (!sceneObject)
            continue;
        prefabEditPreviousVisibility_[sceneObject.get()] = sceneObject->GetIsVisible();
        sceneObject->SetIsVisible(false);
    }

    prefabEditMode_ = true;
    prefabEditDirty_ = false;
    prefabEditName_ = prefabName;
    prefabEditScene_ = scene;
    prefabEditRoot_ = prefabObjects.front().get();

    for (auto& object : prefabObjects) {
        if (!object)
            continue;
        scene->AddObjectImmediate(std::move(object));
    }

    SetSelectedObject(prefabEditRoot_);
    EditorManager::GetInstance()->SetSelectedObject(this);
    CameraEditor::GetInstance()->SetMode(CameraEditor::Mode::kEditor);
    DebugConsole::GetInstance()->AddLog("Prefab Mode: " + prefabName);
    return true;
}

// 仮ルートが生存する編集中のPrefabを保存し、配置済みの同型インスタンスを同期する。
bool DebugEditor::SavePrefabEditSession() {
    if (!prefabEditMode_ || !prefabEditRoot_ || !prefabEditScene_ ||
        !prefabEditScene_->IsAlive(prefabEditRoot_)) {
        return false;
    }

    // 一時Objectを指すUndoを先に破棄し、Asset更新だけをUndo履歴へ残します。
    EditorTransactionManager::GetInstance()->Clear();
    const std::string savedPrefabName = prefabEditName_;
    PresetManager* prefabManager = PresetManager::GetInstance();
    const auto beforePrefabs = prefabManager->GetPrefabs();

    // アセットの更新が失敗したら、仮オブジェクトを閉じずに失敗を返す。
    if (!prefabManager->UpdatePrefabFromObject(savedPrefabName, prefabEditRoot_)) {
        return false;
    }

    // 保存前後の Prefab 定義を比較して既存インスタンスへ反映し、編集用の仮 Object は同期の後に取り除く。
    const std::int32_t synchronizedObjects = prefabManager->SynchronizePrefabInstances(
        beforePrefabs,
        *prefabEditScene_,
        prefabEditScene_->GetObject3DCommon());

    EndPrefabEditSession(false);

    if (synchronizedObjects > 0) {
        // 現行Scene形式はPrefab Instanceの実値も保存するため、同期結果をScene保存対象にします。
        MarkDirty(SaveMode::kAll);
    }

    DebugConsole::GetInstance()->AddLog(
        "Saved Prefab: " + savedPrefabName +
        " / synchronized " + std::to_string(synchronizedObjects) + " objects");
    return true;
}

void DebugEditor::CancelPrefabEditSession() {
    // 未保存のPrefab編集を破棄し、隔離前のScene表示へ戻します。
    if (!prefabEditMode_) {
        return;
    }

    const std::string cancelledPrefabName = prefabEditName_;
    EndPrefabEditSession(true);
    DebugConsole::GetInstance()->AddLog("Discarded Prefab Mode: " + cancelledPrefabName);
}

bool DebugEditor::IsPrefabEditObject(const engine::Object3D* object) const {
    // 編集ルートの子孫だけを対象にし、外側のScene Objectを誤保存しません。
    if (!prefabEditMode_ || !object || !prefabEditRoot_) {
        return false;
    }

    const engine::Object3D* current = object;

    // 選択対象から親を辿り、編集ルートに届いた場合に所属と判定する。最大512段で打ち切る。
    for (std::int32_t depth = 0; current && depth < 512; ++depth) {
        if (current == prefabEditRoot_) {
            return true;
        }

        current = current->GetParent();
    }

    return false;
}

void DebugEditor::EndPrefabEditSession(bool clearTransactions) {
    // 一時Objectと可視状態を復元し、必要なら専用Undo履歴も破棄します。
    BaseScene* scene = prefabEditScene_;
    std::unordered_set<engine::Object3D*> editObjects;

    // 編集していたシーンがある場合に、仮オブジェクトを外して元の可視状態を戻す。
    if (scene) {
        for (auto& object : scene->GetObjects()) {
            if (object && IsPrefabEditObject(object.get())) {
                editObjects.insert(object.get());
            }
        }

        // 仮 Object の所有権を一時配列に移し、取り外し対象を走査している間はその実体を生かしておく。
        std::vector<std::unique_ptr<engine::Object3D>> removedObjects;
        removedObjects.reserve(editObjects.size());

        for (engine::Object3D* object : editObjects) {
            if (auto removed = scene->TakeObjectImmediate(object)) {
                removedObjects.push_back(std::move(removed));
            }
        }

        for (const auto& [object, wasVisible] : prefabEditPreviousVisibility_) {
            // 元から存在した物体でも、生存しているものにだけ可視性を戻す。
            if (scene->IsAlive(object)) {
                object->SetIsVisible(wasVisible);
            }
        }
    }

    ClearObjectSelection();
    EditorManager::GetInstance()->ClearSelection();
    prefabEditMode_ = false;
    prefabEditDirty_ = false;
    prefabEditName_.clear();
    prefabEditRoot_ = nullptr;
    prefabEditScene_ = nullptr;
    prefabEditPreviousVisibility_.clear();

    // キャンセルなど履歴を引き継がない終了経路では、編集トランザクションも消去する。
    if (clearTransactions) {
        EditorTransactionManager::GetInstance()->Clear();
    }
}

void DebugEditor::ResetPrefabEditSessionForSceneChange() {
    // Sceneの寿命を越えるPrefab参照をすべて無効にします。
    prefabEditMode_ = false;
    prefabEditDirty_ = false;
    prefabEditName_.clear();
    prefabEditRoot_ = nullptr;
    prefabEditScene_ = nullptr;
    prefabEditPreviousVisibility_.clear();
    EditorTransactionManager::GetInstance()->Clear();
}

// ========================================================================
// 更新 (ImGui処理)
// ========================================================================
// ========================================================================
// 更新 (ImGui処理 / エディタ操作のコア)
// ========================================================================
void DebugEditor::Update() {
#ifdef USE_IMGUI
    AssetDatabase::GetInstance()->Update();
    // 現在のシーンがない間は、選択や配置などシーンを必要とするフレーム処理を行わない。
    if (!sceneManager_ || !sceneManager_->GetCurrentScene())
        return;

    EffectPreviewStage::GetInstance()->Update();

    textSpriteGenerator_.Update();

    BaseScene* currentScene = sceneManager_->GetCurrentScene();
    engine::InputManager* input = engine::InputManager::GetInstance();

    // =========================================================
    // 1. シーン変更リセット (最優先で行う: ダングリングポインタ防止)
    // =========================================================
    if (lastUpdatedScene_ != currentScene) {
        PresetEditor::GetInstance()->ClearPropertyEditors();
        // Prefab編集中にシーンが変わった場合、旧シーンの参照を操作せずセッションを無効にする。
        if (prefabEditMode_) {
            ResetPrefabEditSessionForSceneChange();
        }

        ClearObjectSelection();
        isSelectionRectDragging_ = false;
        isSelectionRectReady_ = false;
        previewObject_ = nullptr;
        previewChildObjects_.clear();
        brushPreviewObjects_.clear();
        brushPlacement_ = {};
        brushPresetSource_ = {};
        brushStampedRootGuids_.clear();
        isPresetBrushMode_ = false;
        brushPresetName_.clear();
        hasLastBrushStamp_ = false;
        previewCreateCommandLabel_ = "Place Preview Object";
        lastUpdatedScene_ = currentScene;
        EditorTransactionManager::GetInstance()->Clear();
        ClearDirty(SaveMode::kAll);
        hasInspectorEditStart_ = false;
        inspectorEditStartStates_.clear();
        requestGameViewCreateMenu_ = false;
        EditorManager::GetInstance()->ClearSelection();
    }

    // =========================================================
    // 2. エディタ状態の同期と更新
    // =========================================================
    // Inspectorが前フレームの借用参照を使い終わってから、実体を交換します。
    if (pendingObjectTypeChange_) {
        auto change = std::move(pendingObjectTypeChange_);
        pendingObjectTypeChange_ = {};
        change();
    }
    IEditable* current = EditorManager::GetInstance()->GetSelectedObject();
    static std::string sLastSyncedSceneFilename = "";
    std::string currentLoadedName = currentScene->GetLoadedFilename();

    // 遷移中の仮名や空名は使わず、ロード完了後のファイル名が変わったときに同期する。
    if (!sceneManager_->IsTransitioning() &&
        !currentLoadedName.empty() &&
        sLastSyncedSceneFilename != currentLoadedName) {
        SetSceneFilename(currentLoadedName);
        sLastSyncedSceneFilename = currentLoadedName;
    }

    ClearInvalidSelectedObject();

    // 動作・クリップ確認中だけ配置操作を止め、元Objectをギズモで動かさない。
    // 再生時計はGameEditorControllerから更新するため、通常編集へ必ず復帰できる。
    const bool actorToolsActive = (actorSession_ && actorSession_->IsActive()) || animationWorkbench_.IsEnabled();
    CameraEditor::GetInstance()->SetGameViewHovered(isGameViewHovered_ && !actorToolsActive);
    if (actorToolsActive)
        return;

    // 現在フォーカスがこのエディター以外に移っても、シーンObjectを選んだ場合に限り選択を同期する。
    if (current != nullptr && current != this) {
        engine::Object3D* obj = dynamic_cast<engine::Object3D*>(current);

        if (obj) {
            SetSelectedObject(obj);
        }
    }

    // --- カメラ制御 (設置モード用) ---
    bool isPreviewActive = (previewObject_ != nullptr);

    if (isPreviewActive && !wasPreviewActive_) {
        CameraEditor* camEditor = CameraEditor::GetInstance();
        previousCameraMode_ = (std::int32_t)camEditor->GetMode();
        camEditor->SetMode(CameraEditor::Mode::kEditor);

        // 配置を開始したフレームは現在の視点を保ちながらエディターカメラモードにする。
    }
    // 配置終了のフレームで、開始前に控えたカメラモードへ戻す。
    else if (!isPreviewActive && wasPreviewActive_) {
        CameraEditor::GetInstance()->SetMode((CameraEditor::Mode)previousCameraMode_);
    }

    wasPreviewActive_ = isPreviewActive;

    // =========================================================
    // 配置プレビュー中は通常選択を止め、位置の追従・配置確定・中断を処理する。
    // =========================================================
    if (SettingsMenuOverlay::IsAnyActive()) {
        // 設定項目をクリックしても背後のオブジェクトを選択・移動しないようにします。
    }
    // 配置中はGame Viewの有効な左ボタン解放で確定する。
    else if (previewObject_) {
        UpdatePreviewPlacement();

        if (EditorToolSettings::Get().IsVisible("placementPreview") && isGameViewHovered_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            ConfirmPreviewPlacement();
        }

        // 右クリック または Eキー で配置モードキャンセル
        if (input->IsKeyTriggered(DIK_E) || (isGameViewHovered_ && ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
            CancelPreviewPlacement();
        }
    }
    else if (isPresetBrushMode_) {
        // 非表示のブラシで誤って配置しないよう、表示を戻すまで操作を待ちます。
        if (EditorToolSettings::Get().IsVisible("placementPreview"))
            UpdatePresetBrush();
    }

    // =========================================================
    //  モードB: 通常選択・編集モード
    // =========================================================
    else {
        CameraEditor* cameraEditor = CameraEditor::GetInstance();
        const bool isCameraEditorSelected = current == cameraEditor;

        if (!ImGui::GetIO().WantCaptureKeyboard) {
            // 入力欄で編集中はショートカットを実行しない。
            if (input->IsKeyTriggered(DIK_DELETE)) {
                // パス編集中はレコーダーで選択している点だけを削除する。
                if (isPathEditMode_ && selectedObject_ && selectedObject_->GetRecorder() && selectedObject_->GetRecorder()->IsPinSelected()) {
                    selectedObject_->GetRecorder()->DeleteSelectedPin();
                }

                // パス編集中でなければ、通常の選択オブジェクトを削除する。
                else if (!isPathEditMode_) {
                    EditorCommandRegistry::GetInstance()->Execute(editorCommandId::kEditDelete);
                }
            }

            // Ctrl+C/Dは複製、Ctrl+Z/Yは履歴の戻し・やり直しに割り当てる。
            const bool isControlPressed =
                input->IsKeyPressed(DIK_LCONTROL) || input->IsKeyPressed(DIK_RCONTROL);
            if (isControlPressed && (input->IsKeyTriggered(DIK_C) || input->IsKeyTriggered(DIK_D))) {
                EditorCommandRegistry::GetInstance()->Execute(editorCommandId::kEditDuplicate);
            }

            if (isControlPressed && input->IsKeyTriggered(DIK_Z)) {
                EditorCommandRegistry::GetInstance()->Execute(editorCommandId::kEditUndo);
            }

            if (isControlPressed && input->IsKeyTriggered(DIK_Y)) {
                EditorCommandRegistry::GetInstance()->Execute(editorCommandId::kEditRedo);
            }

            // Endキーで選択オブジェクトを床へ移動するコマンドを実行する。
            if (input->IsKeyTriggered(DIK_END)) {
                EditorCommandRegistry::GetInstance()->Execute(editorCommandId::kObjectDropToFloor);
            }

            // Fはカメラ編集を優先し、それ以外は選択中のScene Objectへ寄せる。
            if (input->IsKeyTriggered(DIK_F)) {
                if (isCameraEditorSelected) {
                    cameraEditor->FocusSelectedCameraObject();
                } else if (selectedObject_) {
                    FocusSceneObject(selectedObject_);
                }
            }
        }

        // カメラエディター選択中だけ、軌道の中心を操作できるギズモを描く。
        if (isCameraEditorSelected) {
            cameraEditor->DrawOrbitCenterGizmo(gameViewOffset_, gameViewSize_, isGridSnapEnabled_, snapValue_);
        }

        const bool editingCameraRegion = cameraEditor->DrawRegionGizmo(
            gameViewOffset_, gameViewSize_, isGridSnapEnabled_, snapValue_, this);

        // --- マウス選択処理 (ギズモを触っていない ＆ パス編集中じゃない時) ---
        // 範囲編集中のドラッグが背後の地形選択へ流れないようにする。選択変更はHierarchyで行える。
        const bool canStartSelection = !editingCameraRegion && !isPathEditMode_ && !isCameraEditorSelected &&
            isGameViewHovered_ && !ImGuizmo::IsOver();
        if (canStartSelection || isSelectionRectDragging_) {
            const bool isShiftPressed = ImGui::GetIO().KeyShift;
            // マウスの小さなぶれを箱選択にしないよう、画像上で 6 ピクセル以上動いたときだけ枠を表示する。
            constexpr float kSelectionDragThreshold = 6.0f;

            if (canStartSelection && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                selectionRectStart_ = gameViewMousePos_;
                selectionRectEnd_ = gameViewMousePos_;
                isSelectionRectDragging_ = true;
                isSelectionRectReady_ = false;
            }

            if (isSelectionRectDragging_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                selectionRectEnd_ = gameViewMousePos_;
                const float diffX = selectionRectEnd_.x - selectionRectStart_.x;
                const float diffY = selectionRectEnd_.y - selectionRectStart_.y;
                const float absX = diffX < 0.0f ? -diffX : diffX;
                const float absY = diffY < 0.0f ? -diffY : diffY;
                isSelectionRectReady_ = absX >= kSelectionDragThreshold || absY >= kSelectionDragThreshold;
            }

            // ボタンを離したとき、しきい値以上のドラッグなら矩形、未満なら押下点で個別選択する。
            if (isSelectionRectDragging_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                if (isSelectionRectReady_) {
                    SelectObjectsInGameViewRect(selectionRectStart_, selectionRectEnd_, isShiftPressed);
                } else {
                    engine::Object3D* hit = PickObjectAtGameViewPos(selectionRectStart_);

                    // ヒット時はShiftで選択を反転し、通常クリックは単独選択にする。空白クリックはShiftなしの場合のみ解除する。
                    if (hit) {
                        if (isShiftPressed) {
                            ToggleSelectedObject(hit);
                        } else {
                            SetSelectedObject(hit);
                        }

                        SyncObjectSelectionToInspector();
                    } else if (!isShiftPressed) {
                        ClearObjectSelection();
                        SyncObjectSelectionToInspector();
                    }
                }

                isSelectionRectDragging_ = false;
                isSelectionRectReady_ = false;
            }
        }

        UpdateTransformGizmo(input, editingCameraRegion);

        // 選択境界の表示が有効な場合に、矩形と選択中の境界を重ねる。
        if (EditorToolSettings::Get().IsVisible("selectionBounds")) {
            DrawSelectionRectangleOverlay();
            DrawSelectedObjectBoundsOverlay();
        }
    }

    // =========================================================
    // 3. UI描画関連 (最前面)
    // =========================================================
    PollDdsCacheNotifications();
    // 復元用の時計には現在、実測フレーム差ではなく 60 fps 相当の固定秒数を渡す。
    UpdateCrashRecovery(1.0f / 60.0f);
    DrawSaveNotification();
    DrawSavePreview();
    DrawCrashRecoveryPrompt();

    // 再生中はゲーム画面を優先し、編集用のアイコンを重ねない。
    if (!sceneManager_->IsPlaying() && EditorToolSettings::Get().IsVisible("sceneIcons"))
        Draw3DIcons();
    // 設定オーバーレイが閉じている間に、用途ごとの表示設定を確認して編集補助を重ねる。
    if (!SettingsMenuOverlay::IsAnyActive()) {
        if (!sceneManager_->IsPlaying() && EditorToolSettings::Get().IsVisible("eventIds"))
            DrawEventIdOverlay();
        if (!sceneManager_->IsPlaying() && EditorToolSettings::Get().IsVisible("placementPreview")) {
            DrawPreviewMarker();
            DrawPresetBrushOverlay();
            textSpriteGenerator_.DrawPreview();
        }
    }
#endif
}

#ifdef USE_IMGUI
// 選択物のギズモ操作をローカル姿勢に変換し、ドラッグを一件のUndoへまとめる。
void DebugEditor::UpdateTransformGizmo(engine::InputManager* input, bool editingCameraRegion) {
    // 対象なし・ロック中・カメラ領域またはパス編集中・ツール非表示の場合は操作しない。
    if (!selectedObject_ || editingCameraRegion || isPathEditMode_ ||
        selectedObject_->GetIsLocked() || !EditorToolSettings::Get().IsVisible("transformGizmo")) {
        return;
    }

    engine::Math math;

    static ImGuizmo::OPERATION sCurOp = ImGuizmo::TRANSLATE;

    // 入力欄がキーボードを使用していないときだけ、T/R/Sで移動・回転・拡縮を選べる。
    if (!ImGui::GetIO().WantCaptureKeyboard && !ImGui::GetIO().WantTextInput) {
        if (input->IsKeyTriggered(DIK_T))
            sCurOp = ImGuizmo::TRANSLATE;
        if (input->IsKeyTriggered(DIK_R))
            sCurOp = ImGuizmo::ROTATE;
        if (input->IsKeyTriggered(DIK_S))
            sCurOp = ImGuizmo::SCALE;
    }

    engine::Camera* cam = CameraManager::GetInstance()->GetActiveCamera();

    // アクティブカメラがあるときだけ、現在の対象姿勢と表示矩形からギズモを投影する。
    if (cam) {
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect(gameViewOffset_.x, gameViewOffset_.y, gameViewSize_.x, gameViewSize_.y);

        engine::Matrix4x4 view = cam->GetViewMatrix();
        engine::Matrix4x4 proj = cam->GetProjectionMatrix();
        const Transform* tr = selectedObject_->GetTransform();

        selectedObject_->UpdateWorldMatrix();
        engine::Matrix4x4 world = selectedObject_->GetWorldMatrix();

        float snapVal = isGridSnapEnabled_ ? snapValue_ : 0.0f;
        float snapArr[3] = { snapVal, snapVal, snapVal };

        // ピボット変更はImGuizmo上の操作点だけをずらし、モデル本体のWorld行列は維持する。
        engine::Matrix4x4 gizmoWorld = world;
        engine::Vector3 gizmoPivotWorld = { world.m[3][0], world.m[3][1], world.m[3][2] };
        bool useTranslatedPivot = sCurOp == ImGuizmo::TRANSLATE && gizmoPivotMode_ != 0;

        if (useTranslatedPivot) {
            engine::Aabb pivotAabb{};
            bool hasPivotAabb = false;

            // ピボットの設定に応じてモデル境界か衝突境界を参照し、有効な箱がなければ原点を使う。
            if (gizmoPivotMode_ == 1) {
                pivotAabb = selectedObject_->GetModelWorldAabb();
                hasPivotAabb = IsValidAabb(pivotAabb);
            }
            else if (gizmoPivotMode_ == 2) {
                pivotAabb = selectedObject_->GetAabb();
                hasPivotAabb = IsValidAabb(pivotAabb);
            }

            if (hasPivotAabb) {
                gizmoPivotWorld = GetAabbCenter(pivotAabb);
                gizmoWorld.m[3][0] = gizmoPivotWorld.x;
                gizmoWorld.m[3][1] = gizmoPivotWorld.y;
                gizmoWorld.m[3][2] = gizmoPivotWorld.z;
            }
            else {
                useTranslatedPivot = false;
            }
        }

        ImGuizmo::Manipulate(&view.m[0][0], &proj.m[0][0], sCurOp, ImGuizmo::WORLD, &gizmoWorld.m[0][0], nullptr, isGridSnapEnabled_ ? snapArr : nullptr);

        // 操作中のフレームは姿勢を反映し、最初のフレームでだけ変更前を記録する。
        if (ImGuizmo::IsUsing()) {
            if (!isDraggingTransform_) {
                BeginGizmoDrag(*tr);
            }

            engine::Matrix4x4 primaryWorldBefore = world;
            engine::Matrix4x4 editedWorld = gizmoWorld;

            if (useTranslatedPivot) {
                // 操作点の移動差分だけを元のWorld行列へ戻し、ピボットの移動をObjectの移動へ変換する。
                engine::Vector3 editedPivotWorld = {
                    gizmoWorld.m[3][0],
                    gizmoWorld.m[3][1],
                    gizmoWorld.m[3][2]
                };

                engine::Vector3 pivotDelta = {
                    editedPivotWorld.x - gizmoPivotWorld.x,
                    editedPivotWorld.y - gizmoPivotWorld.y,
                    editedPivotWorld.z - gizmoPivotWorld.z
                };

                editedWorld = world;
                editedWorld.m[3][0] += pivotDelta.x;
                editedWorld.m[3][1] += pivotDelta.y;
                editedWorld.m[3][2] += pivotDelta.z;
            }

            engine::Matrix4x4 newLocalMat = editedWorld;

            // 親付きObjectはWorldのまま保存せず、親の逆行列でローカル姿勢へ戻す。
            if (selectedObject_->GetParent()) {
                engine::Matrix4x4 parentWorldInv = math.Inverse(selectedObject_->GetParent()->GetWorldMatrix());
                newLocalMat = math.Multiply(editedWorld, parentWorldInv);
            }

            engine::Vector3 s, rDeg, t;
            ImGuizmo::DecomposeMatrixToComponents(&newLocalMat.m[0][0], &t.x, &rDeg.x, &s.x);

            selectedObject_->SetTranslate(t);
            selectedObject_->SetScale(s);
            // 回転の本体はローカル行列から求めた Quaternion。Inspector の確認用回転は ImGuizmo の角度をラジアンに直して渡す。
            selectedObject_->SetQuaternionWithEulerHint(math_.MatrixToQuaternion(newLocalMat), { ToRadians(rDeg.x), ToRadians(rDeg.y), ToRadians(rDeg.z) });

            selectedObject_->UpdateLocalMatrix();
            selectedObject_->UpdateWorldMatrix();

            engine::Matrix4x4 primaryWorldAfter = selectedObject_->GetWorldMatrix();
            engine::Matrix4x4 groupDelta = math.Multiply(math.Inverse(primaryWorldBefore), primaryWorldAfter);
            // 複数選択では主ObjectのWorld差分を配る。親子を同時選択した場合は二重変換を避ける。

            for (engine::Object3D* object : selectedObjects_) {
                if (!object || object == selectedObject_ || !IsObjectInCurrentScene(object) || object->GetIsLocked())
                    continue;

                bool hasSelectedAncestor = false;

                for (engine::Object3D* parent = object->GetParent(); parent != nullptr; parent = parent->GetParent()) {
                    if (IsObjectSelected(parent)) {
                        hasSelectedAncestor = true;
                        break;
                    }
                }

                if (hasSelectedAncestor) {
                    continue;
                }

                object->UpdateWorldMatrix();
                engine::Matrix4x4 objectWorldBefore = object->GetWorldMatrix();
                engine::Matrix4x4 objectWorldAfter = math.Multiply(objectWorldBefore, groupDelta);
                engine::Matrix4x4 objectLocalAfter = objectWorldAfter;

                if (object->GetParent()) {
                    engine::Matrix4x4 parentWorldInv = math.Inverse(object->GetParent()->GetWorldMatrix());
                    objectLocalAfter = math.Multiply(objectWorldAfter, parentWorldInv);
                }

                engine::Vector3 otherScale, otherRotDeg, otherTranslate;
                ImGuizmo::DecomposeMatrixToComponents(&objectLocalAfter.m[0][0], &otherTranslate.x, &otherRotDeg.x, &otherScale.x);

                object->SetTranslate(otherTranslate);
                object->SetScale(otherScale);
                object->SetQuaternionWithEulerHint(math_.MatrixToQuaternion(objectLocalAfter), { ToRadians(otherRotDeg.x), ToRadians(otherRotDeg.y), ToRadians(otherRotDeg.z) });
                object->UpdateLocalMatrix();
                object->UpdateWorldMatrix();
            }

        }
        // ギズモが操作中でなくなれば、一連のドラッグを確定する。
        else if (isDraggingTransform_) {
            FinishGizmoDrag();
        }
    }
}

void DebugEditor::BeginGizmoDrag(const Transform& transform) {
    // ドラッグ開始時の全選択状態を記録し、連続操作を一つのUndoとして戻せるようにする。
    isDraggingTransform_ = true;
    tempTransformStart_ = transform;
    tempObjectStateStart_ = CaptureObjectState(selectedObject_);
    groupTransformStartStates_.clear();

    for (engine::Object3D* object : selectedObjects_) {
        if (!object || !IsObjectInCurrentScene(object))
            continue;
        ObjectStateSnapshot snapshot;
        snapshot.object = object;
        snapshot.beforeState = CaptureObjectState(object);
        groupTransformStartStates_.push_back(snapshot);
    }
}

void DebugEditor::FinishGizmoDrag() {
    // 操作終了時にUndoを一件登録し、Directorの自動キーへ確定姿勢を渡す。
    isDraggingTransform_ = false;

    // 複数選択の開始状態を記録していればまとめて履歴へ登録し、それ以外は主対象の変更を確定する。
    if (!groupTransformStartStates_.empty()) {
        RegisterObjectsEdited(groupTransformStartStates_,
            groupTransformStartStates_.size() > 1 ? "Gizmo Group Transform" : "Gizmo Transform");
        groupTransformStartStates_.clear();
    } else {
        RegisterObjectEdited(selectedObject_, tempObjectStateStart_, "Gizmo Transform");
    }

    if (ghostDirector_ &&
        EditorManager::GetInstance()->GetSelectedObject() == ghostDirector_ &&
        ghostDirector_->IsAutoKeyEnabled()) {
        for (engine::Object3D* object : selectedObjects_) {
            if (object && IsObjectInCurrentScene(object)) {
                ghostDirector_->RecordTransformKey(object);
            }
        }
    }
}
#endif

// ========================================================================
// 終了処理
// ========================================================================
void DebugEditor::Finalize() {
    // プレビューと復元セッションを先に閉じ、借用していたScene参照を解放します。
    if (prefabEditMode_) {
        EndPrefabEditSession(true);
    }

    PresetEditor::GetInstance()->ClearPropertyEditors();
    FinalizeCrashRecovery();
#ifdef USE_IMGUI
    animationWorkbench_.Finalize();
#endif
    primitiveDrawer_.Finalize();
    EditorTransactionManager::GetInstance()->Clear();
}


// ========================================================================
// コライダー描画処理
// ========================================================================
void DebugEditor::DrawDebug(ID3D12GraphicsCommandList* commandList) {
#ifdef USE_IMGUI
    // エディターの一時非表示、ゲーム再生、設定メニュー中は補助線を重ねない。
    if (EditorToolSettings::Get().IsTemporarilyHidden())
        return;
    if (sceneManager_ && sceneManager_->IsPlaying())
        return;
#endif
    // メニュー中だけ抑制し、エディターの表示設定そのものは変更しません。
    if (SettingsMenuOverlay::IsAnyActive())
        return;
    if (sceneManager_ == nullptr)
        return;

    BaseScene* currentScene = sceneManager_->GetCurrentScene();
    if (!currentScene)
        return;

    // PrimitiveDrawer にパイプライン設定を委譲
    primitiveDrawer_.PreDraw(commandList);

    std::int32_t instanceCount = 0;
    const std::int32_t kMaxDrawLimit = 8192; // 描画上限（PrimitiveDrawerのバッファサイズと合わせる）
    engine::Math math;

    // =========================================================
    // シーンオブジェクトのコライダー、非表示物の境界、選択した経路を描画する。
    // =========================================================
    auto& objects = currentScene->GetObjects();

    for (const auto& obj : objects) {
        if (!obj)
            continue;
        if (!obj->GetIsRenderVisible())
            continue;

        // インスタンス描画の上限チェック
        if (instanceCount >= kMaxDrawLimit)
            break;

        engine::ColliderType type = obj->GetColliderType();
        bool isInvisibleObj = (obj->GetClassName() == "InvisibleBox");

        // --- 描画判定 ---
        // コライダーがなく、かつ「見える物体（モデルあり）」ならデバッグ線は不要
        if (type == engine::ColliderType::kNone && !isInvisibleObj)
            continue;

        // コライダー表示OFF設定の時、「見える物体」のコライダーは消すが、
        // 「見えない物体(透明な壁など)」は編集用に表示したままにする
        if (!drawColliders_ && !isInvisibleObj)
            continue;
#ifdef USE_IMGUI
        // 不可視物の境界も、対応する表示設定が無効なら省く。
        if (isInvisibleObj && !drawInvisibleBounds_)
            continue;
#endif

        // --- 行列計算 (サイズと位置) ---
        engine::Matrix4x4 drawWorldMatrix = math.MakeIdentity4x4();

        // コライダーがある場合は、その形状データ(Size/Center/Rotation)に合わせて枠を変形させる
        if (type != engine::ColliderType::kNone) {
            // コライダー設定を直接取得 (ImGuiでの変更を即座に反映させるため)
            engine::Object3D::ColliderConfig config = obj->GetColliderConfig();

            if (type == engine::ColliderType::kObb) {
                // Obb (回転ありボックス) の計算
                engine::Matrix4x4 matScale = math.MakeScaleMatrix(config.size * 2.0f);
                engine::Matrix4x4 matRotX = math.MakeRotateXMatrix(config.rotation.x);
                engine::Matrix4x4 matRotY = math.MakeRotateYMatrix(config.rotation.y);
                engine::Matrix4x4 matRotZ = math.MakeRotateZMatrix(config.rotation.z);
                engine::Matrix4x4 matRot = math.Multiply(matRotZ, math.Multiply(matRotX, matRotY));
                engine::Matrix4x4 matTrans = math.MakeTranslateMatrix(config.center);
                engine::Matrix4x4 matColliderLocal = math.Multiply(matScale, math.Multiply(matRot, matTrans));
                drawWorldMatrix = math.Multiply(matColliderLocal, obj->GetWorldMatrix());

            }
            else if (type == engine::ColliderType::kAabb || type == engine::ColliderType::kTerrain) {
                // Aabb (軸平行ボックス) の計算
                engine::Matrix4x4 matScale = math.MakeScaleMatrix(config.size * 2.0f);
                engine::Matrix4x4 matTrans = math.MakeTranslateMatrix(config.center);
                engine::Matrix4x4 matColliderLocal = math.Multiply(matScale, matTrans);
                drawWorldMatrix = math.Multiply(matColliderLocal, obj->GetWorldMatrix());

            }
            else if (type == engine::ColliderType::kSphere) {
                // Sphere (球) の計算
                float radius = config.size.x; // Sphereはxを半径とする
                engine::Matrix4x4 matScale = math.MakeScaleMatrix({ radius * 2.0f, radius * 2.0f, radius * 2.0f });
                engine::Matrix4x4 matTrans = math.MakeTranslateMatrix(config.center);
                engine::Matrix4x4 matColliderLocal = math.Multiply(matScale, matTrans);
                drawWorldMatrix = math.Multiply(matColliderLocal, obj->GetWorldMatrix());
            }
            else if (type == engine::ColliderType::kCylinder) {
                // 円柱のXを半径、Yを半高として直径と全高の描画寸法を作る。
                float radius = config.size.x;
                float height = config.size.y;
                engine::Matrix4x4 matScale = math.MakeScaleMatrix({ radius * 2.0f, height * 2.0f, radius * 2.0f });
                engine::Matrix4x4 matTrans = math.MakeTranslateMatrix(config.center);
                engine::Matrix4x4 matColliderLocal = math.Multiply(matScale, matTrans);
                drawWorldMatrix = math.Multiply(matColliderLocal, obj->GetWorldMatrix());
            }

        }
        else {
            // コライダー未設定の「見えない箱」の場合の救済措置
            // Transformそのままで表示（これがないと選択すらできなくなる）
            drawWorldMatrix = obj->GetWorldMatrix();
        }

        // --- 色の決定 ---
        engine::Vector4 color;

        if (isInvisibleObj) {
            // 見えないオブジェクトは「紫」固定
            color = { 0.6f, 0.0f, 0.8f, 1.0f };
        }
        else {
            // 通常オブジェクトはコライダー種別ごとの色
            switch (type) {
            case engine::ColliderType::kObb:    color = { 1.0f, 0.2f, 0.2f, 1.0f }; break; // 赤

            case engine::ColliderType::kAabb:   color = { 0.0f, 1.0f, 0.0f, 1.0f }; break; // 緑

            case engine::ColliderType::kTerrain: color = { 0.25f, 1.0f, 0.65f, 1.0f };
            break;

            case engine::ColliderType::kSphere: color = { 0.0f, 0.5f, 1.0f, 1.0f }; break; // 青

            case engine::ColliderType::kCylinder: color = { 1.0f, 0.5f, 0.0f, 1.0f }; break; // オレンジ

            case engine::ColliderType::kRing:   color = { 1.0f, 1.0f, 0.0f, 1.0f }; break; // 黄色

            default:                    color = { 1.0f, 1.0f, 1.0f, 1.0f }; break; // 白
            }
        }

        // モデル全体の判定は三角形の辺を表示し、薄い上面だけと誤認させない。
        if (obj->GetCollider() && obj->GetCollider()->HasModelVolume()) {
            const auto hull = obj->GetCollider()->GetWorldHull();
            std::vector<std::pair<engine::Vector3, engine::Vector3>> edges;

            for (const auto& face : hull) for (std::int32_t i=0; i<3; ++i) {
                const auto from=face[i],to=face[(i+1)%3];
                const auto same=[](const engine::Vector3& a,const engine::Vector3& b){return engine::Math::Length(a-b)<0.0001f;};

                // モデルの三角形が共有する辺は重複描画せず、残り枠と線の長さを確認してから描く。
                if (std::any_of(edges.begin(),edges.end(),[&](const auto& e){return
                    (same(e.first,from)&&same(e.second,to)) || (same(e.first,to)&&same(e.second,from));})) continue;
                edges.push_back({from,to});
                if (instanceCount>=kMaxDrawLimit)
                    break;
                const auto delta=to-from;
                const float length=math.Length(delta);
                if (length<0.0001f)
                    continue;
                // 辺を細い箱で表す。XZ 平面上の長さから上下角を求め、水平角と合わせて箱の Z 軸を辺へ向ける。
                const float horizontal=std::sqrt(delta.x*delta.x+delta.z*delta.z);
                const auto world=math.Multiply(math.Multiply(math.MakeScaleMatrix({0.025f,0.025f,length}),
                    math.Multiply(math.MakeRotateXMatrix(-std::atan2(delta.y,horizontal)),
                                  math.MakeRotateYMatrix(std::atan2(delta.x,delta.z)))),math.MakeTranslateMatrix((from+to)*0.5f));
                primitiveDrawer_.DrawWireCube(commandList,world,{0.25f,1.0f,0.65f,1.0f},instanceCount++);
            }

            continue;
        }

        // 判定に使う凸輪郭をそのまま表示し、箱の四隅を判定面として誤認させない。
        if (obj->GetCollider() && obj->GetColliderConfig().footprint.size() >= 3) {
            const engine::Collider* collider = obj->GetCollider();
            const auto top = collider->GetFootprintWorldVertices(obj->GetColliderConfig().size.y);
            const auto drawSegment = [&](const engine::Vector3& from, const engine::Vector3& to) {
                // 輪郭線は描画枠が残り、辺の長さが十分なときに細い箱として描く。
                if (instanceCount >= kMaxDrawLimit)
                    return;
                const engine::Vector3 delta = to - from;
                const float horizontal = std::sqrt(delta.x * delta.x + delta.z * delta.z);
                const float length = math.Length(delta);
                if (length < 0.0001f)
                    return;
                const engine::Matrix4x4 world = math.Multiply(
                    math.Multiply(math.MakeScaleMatrix({ 0.04f, 0.04f, length }),
                                  math.Multiply(math.MakeRotateXMatrix(-std::atan2(delta.y, horizontal)),
                                                math.MakeRotateYMatrix(std::atan2(delta.x, delta.z)))),
                    math.MakeTranslateMatrix((from + to) * 0.5f));
                primitiveDrawer_.DrawWireCube(commandList, world, color, instanceCount++);
            };

            for (size_t i = 0; i < top.size(); ++i) {
                const size_t next = (i + 1) % top.size();
                drawSegment(top[i], top[next]);
            }

            continue;
        }

        // PrimitiveDrawer で描画実行
        if (type == engine::ColliderType::kSphere) {
            primitiveDrawer_.DrawWireSphere(commandList, drawWorldMatrix, color, instanceCount);
        }
        else if (type == engine::ColliderType::kCylinder) {
            primitiveDrawer_.DrawWireCylinder(commandList, drawWorldMatrix, color, instanceCount);
        }
        else if (type == engine::ColliderType::kRing) {
            // リングは外周・内周・半径方向の線を組み合わせて厚みを示す。
            engine::Ring ring = obj->GetCollider()->GetRing();
            float outerR = ring.outerRadius;
            float innerR = ring.innerRadius;

            float halfH = (obj->GetColliderConfig().size.y > 0.01f) ? obj->GetColliderConfig().size.y : 0.1f;
            halfH *= std::abs(obj->GetTransform()->scale.y);

            // 基準行列 (回転 * 平行移動 * オブジェクトのワールド(スケール抜き))
            engine::Matrix4x4 matRotX = math.MakeRotateXMatrix(obj->GetColliderConfig().rotation.x);
            engine::Matrix4x4 matRotY = math.MakeRotateYMatrix(obj->GetColliderConfig().rotation.y);
            engine::Matrix4x4 matRotZ = math.MakeRotateZMatrix(obj->GetColliderConfig().rotation.z);
            engine::Matrix4x4 matCollRot = math.Multiply(matRotZ, math.Multiply(matRotX, matRotY));

            engine::Matrix4x4 matObjWorldNoScale = obj->GetWorldMatrix();

            for (std::int32_t i = 0; i < 3; ++i) {
                engine::Vector3 axis = math.Normalize({ matObjWorldNoScale.m[i][0], matObjWorldNoScale.m[i][1], matObjWorldNoScale.m[i][2] });
                matObjWorldNoScale.m[i][0] = axis.x;
                matObjWorldNoScale.m[i][1] = axis.y;
                matObjWorldNoScale.m[i][2] = axis.z;
            }

            engine::Matrix4x4 matRingBase = math.Multiply(math.Multiply(matCollRot, math.MakeTranslateMatrix(obj->GetColliderConfig().center)), matObjWorldNoScale);

            // 1. 外周・内周のワイヤーフレーム (上下の円)
            if (instanceCount < kMaxDrawLimit) {
                // 外周
                engine::Matrix4x4 matOuter = math.Multiply(math.MakeScaleMatrix({ outerR * 2.0f, halfH * 2.0f, outerR * 2.0f }), matRingBase);
                primitiveDrawer_.DrawWireCylinder(commandList, matOuter, color, instanceCount++);
            }

            if (innerR > 0.01f && instanceCount < kMaxDrawLimit) {
                // 内周
                engine::Matrix4x4 matInner = math.Multiply(math.MakeScaleMatrix({ innerR * 2.0f, halfH * 2.0f, innerR * 2.0f }), matRingBase);
                primitiveDrawer_.DrawWireCylinder(commandList, matInner, { color.x, color.y * 0.5f, 0, 1 }, instanceCount++);
            }

            // 2. 断面をつなぐ線 (上下それぞれ8方向)
            const std::int32_t kLineCount = 8;

            for (std::int32_t i = 0; i < kLineCount && instanceCount < kMaxDrawLimit; ++i) {
                float angle = (2.0f * std::numbers::pi_v<float> / kLineCount) * i;
                float s = std::sin(angle);
                float c = std::cos(angle);

                // 上下の断面をつなぐ線 (内径から外径へ)
                for (float h : {-halfH, halfH}) {
                    engine::Vector3 pInner = { innerR * c, h, innerR * s };
                    engine::Vector3 pOuter = { outerR * c, h, outerR * s };

                    // 上下の面を構成する半径方向の線は、内外の端点の中間へ細い箱を置いて描く。
                    engine::Vector3 center = (pInner + pOuter) * 0.5f;
                    engine::Vector3 diff = pOuter - pInner;
                    float vectorLength = math.Length(diff);

                    engine::Matrix4x4 matL = math.MakeScaleMatrix({ 0.02f, 0.02f, vectorLength });
                    engine::Matrix4x4 matLR = math.MakeRotateYMatrix(angle);
                    engine::Matrix4x4 matLT = math.MakeTranslateMatrix(center);
                    engine::Matrix4x4 lineWorld = math.Multiply(math.Multiply(matL, math.Multiply(matLR, matLT)), matRingBase);

                    primitiveDrawer_.DrawWireCube(commandList, lineWorld, color, instanceCount++);
                }
            }

            continue;
        }
        else {
            primitiveDrawer_.DrawWireCube(commandList, drawWorldMatrix, color, instanceCount);
        }

        instanceCount++; // あたり判定を描画したのでカウントを進める

        // 検知範囲の可視化 (敵のみ)
        if (obj->GetClassName() == "Enemy") {
            float range = 0.0f;

            // 敵に能力値がある場合は検知距離を取り出す。正の距離で描画枠が残る場合だけ球を追加する。
            if (obj->GetParameters().has_value()) {
                range = obj->GetParameters()->detectionRange;
            }

            // インスタンス描画の上限チェック
            if (instanceCount < kMaxDrawLimit && range > 0.0f) {
                engine::Matrix4x4 matScale = math.MakeScaleMatrix({ range * 2.0f, range * 2.0f, range * 2.0f });
                engine::Matrix4x4 matTrans = math.MakeTranslateMatrix(obj->GetWorldPosition());
                engine::Matrix4x4 rangeWorldMatrix = math.Multiply(matScale, matTrans);

                // 検知範囲は薄い赤色
                engine::Vector4 rangeColor = { 1.0f, 0.0f, 0.0f, 0.3f };
                primitiveDrawer_.DrawWireSphere(commandList, rangeWorldMatrix, rangeColor, instanceCount);
                instanceCount++; // 検知範囲を描画したのでカウントを進める
            }
        }

    }

    // 選択中のNav Agentが計算した経路を、Scene上で確認できるようにします。
    engine::Object3D* navObject = GetSelectedObject3D();

    if (navObject) {
        if (const NavAgentComponent* navAgent = navObject->GetNavAgentComponent()) {
            const std::vector<engine::Vector3>& path = navAgent->GetCurrentPath();

            for (std::size_t index = 0; index < path.size() && instanceCount < kMaxDrawLimit; ++index) {
                const bool isCurrent = index == navAgent->GetCurrentWaypointIndex();
                const engine::Vector4 pointColor = isCurrent
                    ? engine::Vector4{ 1.0f, 0.85f, 0.15f, 1.0f }
                    : engine::Vector4{ 0.10f, 0.95f, 1.0f, 1.0f };
                const engine::Vector3 point = path[index] + engine::Vector3{ 0.0f, 0.12f, 0.0f };
                const engine::Matrix4x4 pointWorld = math.Multiply(
                    math.MakeScaleMatrix({ 0.20f, 0.20f, 0.20f }),
                    math.MakeTranslateMatrix(point));
                primitiveDrawer_.DrawWireSphere(commandList, pointWorld, pointColor, instanceCount++);

                // 最初の経由点には前の点がない。描画枠切れの場合も点間の線を省く。
                if (index == 0 || instanceCount >= kMaxDrawLimit)
                    continue;
                const engine::Vector3 previous = path[index - 1] + engine::Vector3{ 0.0f, 0.12f, 0.0f };
                const engine::Vector3 difference = point - previous;
                // Nav 経路の線は XZ 平面の向きと距離で描く。上下位置は隣接する地点の中間を使う。
                const float length = std::sqrt(difference.x * difference.x + difference.z * difference.z);
                if (length <= 0.001f)
                    continue;
                const float yaw = std::atan2(difference.x, difference.z);
                const engine::Vector3 center = (previous + point) * 0.5f;
                const engine::Matrix4x4 lineWorld = math.Multiply(
                    math.MakeScaleMatrix({ 0.05f, 0.05f, length }),
                    math.Multiply(math.MakeRotateYMatrix(yaw), math.MakeTranslateMatrix(center)));
                primitiveDrawer_.DrawWireCube(
                    commandList, lineWorld, { 0.10f, 0.95f, 1.0f, 1.0f }, instanceCount++);
            }
        }
    }

    // =========================================================
    // 2. 弾のコライダー描画
    // =========================================================
    CameraEditor::GetInstance()->DrawOrbitGuide(primitiveDrawer_, commandList, instanceCount, kMaxDrawLimit);
#ifdef USE_IMGUI
    if (EditorToolSettings::Get().IsVisible("placementPreview"))
#endif
        DrawPreviewWire(commandList, instanceCount, kMaxDrawLimit);

    // 衝突表示が有効なら、生存していて判定種別がある弾の枠を表示する。
    if (drawColliders_) {
        const auto& bullets = BulletManager::GetInstance()->GetBullets();

        for (const auto& bullet : bullets) {
            if (!bullet || bullet->IsDead())
                continue;
            if (instanceCount >= kMaxDrawLimit)
                break;

            engine::ColliderType type = bullet->GetColliderType();
            if (type == engine::ColliderType::kNone)
                continue;

            // 弾は黄色固定
            engine::Vector4 color = { 1.0f, 1.0f, 0.0f, 1.0f };
            engine::Matrix4x4 drawWorldMatrix = math.MakeIdentity4x4();

            // 弾は物理更新後の判定位置・軸・寸法を使って表示用行列を組む。
            if (type == engine::ColliderType::kObb) {
                engine::Obb obb = bullet->GetObb();
                engine::Matrix4x4 matScale = math.MakeScaleMatrix(obb.size * 2.0f);

                // OBBの軸から回転行列を復元
                engine::Matrix4x4 matRot = math.MakeIdentity4x4();
                matRot.m[0][0] = obb.orientations[0].x;
                matRot.m[0][1] = obb.orientations[0].y;
                matRot.m[0][2] = obb.orientations[0].z;
                matRot.m[1][0] = obb.orientations[1].x;
                matRot.m[1][1] = obb.orientations[1].y;
                matRot.m[1][2] = obb.orientations[1].z;
                matRot.m[2][0] = obb.orientations[2].x;
                matRot.m[2][1] = obb.orientations[2].y;
                matRot.m[2][2] = obb.orientations[2].z;

                engine::Matrix4x4 matTrans = math.MakeTranslateMatrix(obb.center);
                drawWorldMatrix = math.Multiply(matScale, math.Multiply(matRot, matTrans));

            }
            else if (type == engine::ColliderType::kAabb) {
                engine::Aabb aabb = bullet->GetAabb();
                engine::Vector3 center = (aabb.min + aabb.max) * 0.5f;
                engine::Vector3 size = aabb.max - aabb.min;
                engine::Matrix4x4 matScale = math.MakeScaleMatrix(size);
                engine::Matrix4x4 matTrans = math.MakeTranslateMatrix(center);
                drawWorldMatrix = math.Multiply(matScale, matTrans);

            }
            else if (type == engine::ColliderType::kSphere) {
                float radius = bullet->GetCollisionRadius();
                engine::Vector3 center = bullet->GetWorldPosition();
                engine::Matrix4x4 matScale = math.MakeScaleMatrix({ radius * 2.0f, radius * 2.0f, radius * 2.0f });
                engine::Matrix4x4 matTrans = math.MakeTranslateMatrix(center);
                drawWorldMatrix = math.Multiply(matScale, matTrans);
            }

            // PrimitiveDrawer で弾も描画実行
            if (type == engine::ColliderType::kSphere) {
                primitiveDrawer_.DrawWireSphere(commandList, drawWorldMatrix, color, instanceCount);
            }
            else if (type == engine::ColliderType::kCylinder) {
                primitiveDrawer_.DrawWireCylinder(commandList, drawWorldMatrix, color, instanceCount);
            }
            else {
                primitiveDrawer_.DrawWireCube(commandList, drawWorldMatrix, color, instanceCount);
            }

            instanceCount++;
        }

        // =========================================================
    // 3. エフェクトのコライダー描画
    // =========================================================
        // 衝突表示が有効なら、実行中とプレビューのエフェクトを同じ枠数の上限で描く。
        if (drawColliders_) {
            // ゲーム中のエフェクト ＋ エディタのプレビューエフェクトを両方収集
            std::vector<EffectObject3D*> effectsToDraw;

            for (const auto& eff : MeshEffectManager::GetInstance()->GetActiveEffects()) {
                if (eff)
                    effectsToDraw.push_back(eff.get());
            }

            if (EffectObject3D* preview = MeshEffectManager::GetInstance()->GetPreviewEffectForDebug()) {
                effectsToDraw.push_back(preview);
            }

            // 描画枠が残り、形状が指定されたエフェクトだけを描く。
            for (EffectObject3D* effect : effectsToDraw) {
                if (instanceCount >= kMaxDrawLimit)
                    break;

                engine::ColliderType type = effect->GetColliderType();
                if (type == engine::ColliderType::kNone)
                    continue;

                // エフェクトの判定枠はシアン（水色）にして区別
                engine::Vector4 color = { 0.0f, 1.0f, 1.0f, 1.0f };
                engine::Matrix4x4 drawWorldMatrix = math.MakeIdentity4x4();

                engine::Object3D::ColliderConfig config = effect->GetColliderConfig();

                if (type == engine::ColliderType::kObb) {
                    engine::Matrix4x4 matScale = math.MakeScaleMatrix(config.size * 2.0f);
                    engine::Matrix4x4 matRotX = math.MakeRotateXMatrix(config.rotation.x);
                    engine::Matrix4x4 matRotY = math.MakeRotateYMatrix(config.rotation.y);
                    engine::Matrix4x4 matRotZ = math.MakeRotateZMatrix(config.rotation.z);
                    engine::Matrix4x4 matRot = math.Multiply(matRotZ, math.Multiply(matRotX, matRotY));
                    engine::Matrix4x4 matTrans = math.MakeTranslateMatrix(config.center);
                    engine::Matrix4x4 matColliderLocal = math.Multiply(matScale, math.Multiply(matRot, matTrans));
                    drawWorldMatrix = math.Multiply(matColliderLocal, effect->GetWorldMatrix());

                }
                else if (type == engine::ColliderType::kAabb) {
                    engine::Matrix4x4 matScale = math.MakeScaleMatrix(config.size * 2.0f);
                    engine::Matrix4x4 matTrans = math.MakeTranslateMatrix(config.center);
                    engine::Matrix4x4 matColliderLocal = math.Multiply(matScale, matTrans);
                    drawWorldMatrix = math.Multiply(matColliderLocal, effect->GetWorldMatrix());

                }
                else if (type == engine::ColliderType::kSphere) {
                    float radius = config.size.x;
                    engine::Matrix4x4 matScale = math.MakeScaleMatrix({ radius * 2.0f, radius * 2.0f, radius * 2.0f });
                    engine::Matrix4x4 matTrans = math.MakeTranslateMatrix(config.center);
                    engine::Matrix4x4 matColliderLocal = math.Multiply(matScale, matTrans);
                    drawWorldMatrix = math.Multiply(matColliderLocal, effect->GetWorldMatrix());

                }
                else if (type == engine::ColliderType::kCylinder) {
                    float radius = config.size.x;
                    float height = config.size.y;
                    engine::Matrix4x4 matScale = math.MakeScaleMatrix({ radius * 2.0f, height * 2.0f, radius * 2.0f });
                    engine::Matrix4x4 matTrans = math.MakeTranslateMatrix(config.center);
                    engine::Matrix4x4 matColliderLocal = math.Multiply(matScale, matTrans);
                    drawWorldMatrix = math.Multiply(matColliderLocal, effect->GetWorldMatrix());
                }

                // リングだけは円周と断面の組み合わせで描き、通常の枠描画を行わない。
                if (type == engine::ColliderType::kRing) {
                    engine::Ring ring;

                    if (effect->GetCollider()) {
                        ring = effect->GetCollider()->GetRing();
                    } else {
                        // コライダーが無いプレビューでは、メッシュの内径・外径を使う。
                        ring.center = effect->GetWorldPosition();
                        ring.normal = { 0, 1, 0 };
                        ring.innerRadius = effect->GetMeshSettings().ringInnerRadius;
                        ring.outerRadius = effect->GetMeshSettings().ringOuterRadius;
                    }

                    float outerR = ring.outerRadius;
                    float innerR = ring.innerRadius;
                    // 素材から半径を取る場合も内側と外側を数値順にそろえ、内周と外周を正しく色分けする。
                    if (innerR > outerR)
                        std::swap(innerR, outerR);

                    float halfH = (config.size.y > 0.01f) ? config.size.y : 0.1f;
                    halfH *= std::abs(effect->GetTransform()->scale.y);

                    // リングの基準行列
                    engine::Matrix4x4 matCollRot = math.MakeIdentity4x4();

                    if (effect->GetCollider()) {
                        engine::Matrix4x4 mRX = math.MakeRotateXMatrix(config.rotation.x);
                        engine::Matrix4x4 mRY = math.MakeRotateYMatrix(config.rotation.y);
                        engine::Matrix4x4 mRZ = math.MakeRotateZMatrix(config.rotation.z);
                        matCollRot = math.Multiply(mRZ, math.Multiply(mRX, mRY));
                    }

                    // 半径と高さで別途寸法を指定するため、Object のワールド行列は軸を正規化して回転・位置だけ使う。
                    engine::Matrix4x4 matObjWorldNoScale = effect->GetWorldMatrix();

                    for (std::int32_t i = 0; i < 3; ++i) {
                        engine::Vector3 axis = math.Normalize({ matObjWorldNoScale.m[i][0], matObjWorldNoScale.m[i][1], matObjWorldNoScale.m[i][2] });
                        matObjWorldNoScale.m[i][0] = axis.x;
                        matObjWorldNoScale.m[i][1] = axis.y;
                        matObjWorldNoScale.m[i][2] = axis.z;
                    }

                    engine::Matrix4x4 matRingBase = math.Multiply(math.Multiply(matCollRot, math.MakeTranslateMatrix(config.center)), matObjWorldNoScale);

                    engine::Vector4 ringColor = { 1.0f, 1.0f, 0.0f, 1.0f };

                    // 1. シリンダー (外周・内周)
                    if (instanceCount < kMaxDrawLimit) {
                        engine::Matrix4x4 matOuter = math.Multiply(math.MakeScaleMatrix({ outerR * 2.0f, halfH * 2.0f, outerR * 2.0f }), matRingBase);
                        primitiveDrawer_.DrawWireCylinder(commandList, matOuter, { 1.0f, 0.9f, 0.0f, 0.8f }, instanceCount++);
                    }

                    if (innerR > 0.01f && instanceCount < kMaxDrawLimit) {
                        engine::Matrix4x4 matInner = math.Multiply(math.MakeScaleMatrix({ innerR * 2.0f, halfH * 2.0f, innerR * 2.0f }), matRingBase);
                        primitiveDrawer_.DrawWireCylinder(commandList, matInner, { 1.0f, 0.5f, 0.0f, 0.8f }, instanceCount++);
                    }

                    // 2. 断面をつなぐ線
                    const std::int32_t kLineCount = 8;

                    for (std::int32_t i = 0; i < kLineCount && instanceCount < kMaxDrawLimit; ++i) {
                        float angle = (2.0f * std::numbers::pi_v<float> / kLineCount) * i;
                        float s = std::sin(angle);
                        float c = std::cos(angle);

                        for (float h : { -halfH, halfH }) {
                            engine::Vector3 pInner = { innerR * c, h, innerR * s };
                            engine::Vector3 pOuter = { outerR * c, h, outerR * s };
                            engine::Vector3 center = (pInner + pOuter) * 0.5f;
                            engine::Vector3 diff = pOuter - pInner;
                            float vectorLength = math.Length(diff);
                            engine::Matrix4x4 matL = math.MakeScaleMatrix({ 0.02f, 0.02f, vectorLength });
                            engine::Matrix4x4 matLR = math.MakeRotateYMatrix(angle);
                            engine::Matrix4x4 matLT = math.MakeTranslateMatrix(center);
                            engine::Matrix4x4 lineWorld = math.Multiply(math.Multiply(matL, math.Multiply(matLR, matLT)), matRingBase);
                            primitiveDrawer_.DrawWireCube(commandList, lineWorld, ringColor, instanceCount++);
                        }
                    }

                    continue;
                }

                if (type == engine::ColliderType::kSphere) {
                    primitiveDrawer_.DrawWireSphere(commandList, drawWorldMatrix, color, instanceCount);
                }
                else if (type == engine::ColliderType::kCylinder) {
                    primitiveDrawer_.DrawWireCylinder(commandList, drawWorldMatrix, color, instanceCount);
                }
                else {
                    primitiveDrawer_.DrawWireCube(commandList, drawWorldMatrix, color, instanceCount);
                }

                instanceCount++;
            }
        }
    }


}

// ==========================================================================================
// 1. 左パネル：Hierarchy (階層構造) と 生成メニュー
// ==========================================================================================
void DebugEditor::DrawHierarchy() {
    // Hierarchyの表示と選択操作は専用Windowへ委譲します。
    hierarchyWindow_.Draw();
}

// ==========================================================================================
// 2. 右パネル：Inspector (選択したオブジェクトの詳細設定)
// ==========================================================================================
void DebugEditor::DrawImGui() {
    // Inspector操作前後のObject状態を比較し、編集履歴へ渡します。
    const std::vector<ObjectStateSnapshot> beforeStates = CaptureObjectStates(selectedObjects_);

    inspectorWindow_.Draw();

#ifdef USE_IMGUI
    // シーンObjectを選択している間だけ、操作対象とギズモ・選択表示の設定を出す。
    if (selectedObject_) {
        ImGui::Separator();

        if (ImGui::CollapsingHeader("ギズモ設定", ImGuiTreeNodeFlags_DefaultOpen)) {
            const char* pivotModes[] = { "原点", "モデル中心", "コリジョン中心" };
            ImGui::Combo("ギズモ基準", &gizmoPivotMode_, pivotModes, IM_ARRAYSIZE(pivotModes));

            // 原点以外を選ぶときは、移動と回転・拡縮で基準が異なることを案内する。
            if (gizmoPivotMode_ != 0) {
                ImGui::TextDisabled("移動ギズモの表示位置だけを補正します。回転とスケールは原点基準です。");
            }

            ImGui::SeparatorText("選択表示");
            std::int32_t overlayMode = static_cast<std::int32_t>(selectionOverlayMode_);
            const char* overlayModes[] = { "スマート", "詳細", "非表示" };

            // 表示モードが選び直されたときに、選択枠の描画設定へ反映する。
            if (ImGui::Combo("選択オブジェクトの可視化", &overlayMode, overlayModes, IM_ARRAYSIZE(overlayModes))) {
                selectionOverlayMode_ = static_cast<SelectionOverlayMode>(overlayMode);
            }

            const std::string activeName = selectedObject_->GetName().empty() ? "Selected" : selectedObject_->GetName();
            ImGui::Text("選択: %zu個 / 操作対象: %s", selectedObjects_.size(), activeName.c_str());

            // 簡略表示と非表示のときは、それぞれ画面に出る補助表示を案内する。
            if (selectionOverlayMode_ == SelectionOverlayMode::kCompact) {
                ImGui::TextDisabled("四隅だけを表示し、巨大物や画面外の物体はマーカーへ自動で切り替えます。");
                ImGui::TextDisabled("Altを押している間は回転に追従した詳細境界を表示します。");
            } else if (selectionOverlayMode_ == SelectionOverlayMode::kHidden) {
                ImGui::TextDisabled("選択枠を隠し、ImGuizmoだけを表示します。");
            }
        }
    }

    TrackInspectorEdit(beforeStates);
#endif
}

#ifdef USE_IMGUI
void DebugEditor::DrawProjectWindow() {
    // パネルを閉じたときはAsset一覧の描画を止めます。
    if (!projectWindowVisible_) {
        return;
    }

    projectWindow_.Draw();
}
#endif

