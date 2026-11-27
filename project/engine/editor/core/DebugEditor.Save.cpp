#define NOMINMAX
#include "DebugEditor.h"
#include <numbers>
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
#include "SceneController.h"
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

#include <DebugConsole.h>
#include <CollisionManager.h>
#include <filesystem> // ファイル操作用
#include <game/system/combat/BulletManager.h>
#include "engine/editor/assets/preset/PresetManager.h"
#include <PresetEditor.h>
#include <MeshEffectManager.h>
#include <unordered_map>
#include <utility>
namespace fs = std::filesystem;






// ========================================================================
// Scene Assetの管理とシーン保存
// ========================================================================

std::vector<SceneSerializer::SceneAssetInfo> DebugEditor::GetSceneAssets() const {
    // ファイル一覧の探索は Serializer に集約し、UI はメタデータを受け取る。
    return serializer_.DiscoverSceneAssets();
}

bool DebugEditor::CreateSceneAsset(
    const std::string& sceneId,
    const std::string& displayName,
    const std::string& runtimeScene,
    SceneSerializer::SceneAssetTemplate sceneTemplate,
    std::string& createdFilename,
    std::string& errorMessage) {
    // Asset の作成とエラー検証は Serializer に任せ、成功時だけ操作ログを残す。
    const bool created = serializer_.CreateSceneAsset(
        sceneId,
        displayName,
        runtimeScene,
        sceneTemplate,
        createdFilename,
        errorMessage);
    // Serializer が作成に成功したときだけ、確定したファイル名をログに残す。
    if (created) {
        DebugConsole::GetInstance()->AddLog("Created Scene Asset: " + createdFilename);
    }

    return created;
}

bool DebugEditor::DuplicateSceneAsset(
    const std::string& sourceFilename,
    const std::string& newSceneId,
    const std::string& displayName,
    std::string& createdFilename,
    std::string& errorMessage) {
    // 複製元の設定を使って新 ID の Scene Asset を生成する。
    const bool duplicated = serializer_.DuplicateSceneAsset(
        sourceFilename,
        newSceneId,
        displayName,
        createdFilename,
        errorMessage);
    // 複製が成立した場合は、元と新しいファイルの対応をログに残す。
    if (duplicated) {
        DebugConsole::GetInstance()->AddLog(
            "Duplicated Scene Asset: " + sourceFilename + " -> " + createdFilename);
    }

    return duplicated;
}

bool DebugEditor::RenameSceneAsset(
    const std::string& sourceFilename,
    const std::string& newSceneId,
    const std::string& displayName,
    std::string& renamedFilename,
    std::string& errorMessage) {
    // 開いている Asset の名前が変わる場合は、SceneManager の保存先パスも追従させる。
    const bool wasCurrentScene = IsCurrentSceneAsset(sourceFilename);
    const bool renamed = serializer_.RenameSceneAsset(
        sourceFilename,
        newSceneId,
        displayName,
        renamedFilename,
        errorMessage);
    // 改名に失敗した場合は、現在のシーン情報を更新しない。
    if (!renamed) {
        return false;
    }

    if (wasCurrentScene) {
        // 現在開いているファイルの改名に限り、次回保存のファイル名と再読込先を両方更新する。
        SetSceneFilename(renamedFilename);

        // シーン管理器が使える場合は、次の読み込みで使う Object と Sprite のパスも切り替える。
        if (sceneManager_) {
            sceneManager_->SetEditorSceneAssetPaths(
                serializer_.ResolveSceneAssetObjectPath(renamedFilename),
                serializer_.ResolveSceneAssetSpritePath(renamedFilename));
        }
    }

    DebugConsole::GetInstance()->AddLog(
        "Renamed Scene Asset: " + sourceFilename + " -> " + renamedFilename);
    return true;
}

bool DebugEditor::DeleteSceneAsset(const std::string& filename, std::string& errorMessage) {
    // 現在開いている Asset は実行状態とパスが結び付いているため削除させない。
    if (IsCurrentSceneAsset(filename)) {
        errorMessage = "開いているScene Assetは削除できません。別のシーンを開いてから削除してください。";
        return false;
    }

    // ファイル削除に失敗した場合は、Serializer のエラーを返し、完了ログを出さない。
    if (!serializer_.DeleteSceneAsset(filename, errorMessage)) {
        return false;
    }

    DebugConsole::GetInstance()->AddLog("Deleted Scene Asset: " + filename);
    return true;
}

bool DebugEditor::SetSceneAssetRuntimeScene(
    const std::string& filename,
    const std::string& runtimeScene,
    std::string& errorMessage) {
    // 実行クラス名の更新は Serializer に委ね、成功時だけ履歴へ出す。
    if (!serializer_.SetSceneAssetRuntimeScene(filename, runtimeScene, errorMessage)) {
        return false;
    }

    DebugConsole::GetInstance()->AddLog(
        "Scene Asset runtime changed: " + filename + " -> " + runtimeScene);
    return true;
}

bool DebugEditor::SetSceneAssetRuntimeSettings(
    const std::string& filename,
    const std::string& controllerName,
    const std::string& bgmPath,
    const std::string& lightPath,
    const std::string& cameraPath,
    const std::string& skyboxPath,
    std::string& errorMessage) {
    // Controller と環境参照を同じ Serializer の更新経路へ渡す。
    if (!serializer_.SetSceneAssetRuntimeSettings(
        filename,
        controllerName,
        bgmPath,
        lightPath,
        cameraPath,
        skyboxPath,
        errorMessage)) {
        return false;
    }

    DebugConsole::GetInstance()->AddLog("Updated Scene Asset runtime settings: " + filename);
    return true;
}

SceneSerializer::SceneAssetValidationResult DebugEditor::ValidateSceneAsset(
    const std::string& filename) const {
    // ファイル形式に加え、GAMEPLAY 用 Controller の登録状況も検証する。
    SceneSerializer::SceneAssetValidationResult result = serializer_.ValidateSceneAsset(filename);
    const std::vector<SceneSerializer::SceneAssetInfo> assets = serializer_.DiscoverSceneAssets();
    const auto asset = std::find_if(assets.begin(), assets.end(), [&](const SceneSerializer::SceneAssetInfo& candidate) {
        return candidate.filename == filename;
    });
    // 発見した Asset が GAMEPLAY 用で、指定 Controller が未登録の場合だけ、追加の検証エラーにする。
    if (asset != assets.end() && asset->runtimeScene == "GAMEPLAY" &&
        !SceneControllerFactory::GetInstance()->IsRegistered(asset->controllerName)) {
        result.errors.push_back("Scene Controllerが登録されていません: " + asset->controllerName);
    }

    return result;
}

std::vector<std::string> DebugEditor::GetRegisteredSceneNames() const {
    // SceneManager が無い初期化前は空集合を返す。
    return sceneManager_ ? sceneManager_->GetRegisteredSceneNames() : std::vector<std::string>{};
}

bool DebugEditor::OpenSceneAsset(
    const std::string& filename,
    bool discardUnsavedChanges,
    std::string& errorMessage) {
    // Play・Prefab 編集・未保存変更を確認してから Scene Asset を切り替える。
    errorMessage.clear();

    // シーン管理器の初期化前は、遷移を要求できない。
    if (!sceneManager_) {
        errorMessage = "SceneManagerが初期化されていません。";
        return false;
    }

    // プレイ中は編集中の Asset を切り替えず、停止を案内する。
    if (sceneManager_->IsPlaying()) {
        errorMessage = "実行中はScene Assetを切り替えられません。停止してから開いてください。";
        return false;
    }

    // Prefab 編集モード中は、そのセッションを終了してから開き直す必要がある。
    if (prefabEditMode_) {
        errorMessage = "Prefab Modeを終了してからScene Assetを開いてください。";
        return false;
    }

    // 未保存の変更があり、破棄の指定もなければ遷移を保留する。
    if (HasAnyDirty() && !discardUnsavedChanges) {
        errorMessage = "未保存の変更があります。保存するか、破棄して開く操作を選んでください。";
        return false;
    }

    const std::string objectPath = serializer_.ResolveSceneAssetObjectPath(filename);
    const std::string spritePath = serializer_.ResolveSceneAssetSpritePath(filename);
    const std::vector<SceneSerializer::SceneAssetInfo> assets = serializer_.DiscoverSceneAssets();
    const auto asset = std::find_if(assets.begin(), assets.end(), [&](const SceneSerializer::SceneAssetInfo& candidate) {
        return candidate.filename == filename;
    });

    // 発見した一覧に指定ファイルがなければ、実行クラスを選ぶ前に失敗を返す。
    if (asset == assets.end()) {
        errorMessage = "Scene Assetが見つかりません: " + filename;
        return false;
    }

    // メタデータの実行クラスが SceneFactory へ登録されていなければ、遷移先を用意できない。
    if (!sceneManager_->IsSceneRegistered(asset->runtimeScene)) {
        errorMessage = "実行クラスがSceneFactoryへ登録されていません: " + asset->runtimeScene;
        return false;
    }

    const SceneSerializer::SceneAssetValidationResult validation = ValidateSceneAsset(filename);

    // 致命的な検証エラーはまとめて画面へ返す。警告だけならログを残して読込要求へ進む。
    if (!validation.IsValid()) {
        errorMessage = validation.errors.front();

        for (size_t index = 1; index < validation.errors.size(); ++index) {
            errorMessage += "\n" + validation.errors[index];
        }

        return false;
    }

    for (const std::string& warning : validation.warnings) {
        DebugConsole::GetInstance()->AddLog("Scene validation warning: " + warning);
    }

    // Scene Asset で指定した実行クラスと Object・Sprite・環境データを一つの読込文脈へ渡す。
    SceneLoadContext context;
    context.sceneAssetId = asset->id;
    context.displayName = asset->displayName;
    context.runtimeScene = asset->runtimeScene;
    context.objectLayoutPath = objectPath;
    context.spriteLayoutPath = spritePath;
    context.controllerName = asset->controllerName;
    context.bgmPath = asset->bgmPath;
    context.lightPath = asset->lightPath;
    context.cameraPath = asset->cameraPath;
    context.skyboxPath = asset->skyboxPath;

    // シーン管理器が遷移要求を受け付けなければ、旧シーンの選択や Undo を残して戻る。
    if (!sceneManager_->OpenSceneAsset(context)) {
        errorMessage = "シーン遷移中のためScene Assetを開けませんでした。";
        return false;
    }

    // 遷移要求が成立した後、旧 Scene の選択・Undo・Dirty 状態を引き継がない。
    // ゴースト記録器がある場合は、切り替え前の追跡対象を解除する。
    if (ghostRecorder_) {
        ghostRecorder_->ClearTarget();
    }

    ClearObjectSelection();
    EditorManager::GetInstance()->ClearSelection();
    EditorTransactionManager::GetInstance()->Clear();
    pendingSceneAssetOpenAfterSave_.clear();
    ClearDirty(SaveMode::kAll);
    SetSceneFilename(filename);
    DebugConsole::GetInstance()->AddLog("Opening Scene Asset: " + filename);
    return true;
}

void DebugEditor::SaveThenOpenSceneAsset(const std::string& filename) {
    // 保存確認が確定した後で開くため、遷移先を保留する。
    pendingSceneAssetOpenAfterSave_ = filename;
    SaveScene(SaveMode::kAll);
}

bool DebugEditor::IsCurrentSceneAsset(const std::string& filename) const {
    // パス表記にかかわらず末尾の JSON 名で現在の Asset と照合する。
    if (!sceneManager_ || !sceneManager_->HasActiveSceneAsset()) {
        return false;
    }

    std::string leafName = filename;
    const size_t slash = leafName.find_last_of("/\\");

    // パス区切りを含む入力は、末尾のファイル名だけで比較する。
    if (slash != std::string::npos) {
        leafName = leafName.substr(slash + 1);
    }

    return leafName == currentSceneFilename_;
}

void DebugEditor::OpenCreateSceneAssetDialog() {
    // 作成 UI の状態管理は Hierarchy に集約する。
    hierarchyWindow_.OpenCreateSceneDialog();
}

void DebugEditor::RequestOpenSceneAssetFromMenu(const std::string& filename) {
    // メニューからの読込も Hierarchy の未保存確認経路を使う。
    hierarchyWindow_.OpenSceneAssetFromMenu(filename);
}

void DebugEditor::SaveScene(SaveMode mode) {
    // Prefab 編集時は Scene JSON へ書かず、専用セッションを保存する。
    if (prefabEditMode_) {
        SavePrefabEditSession();
        return;
    }

    // 未選択のSceneに名前のない分割JSONを作らない。保存先はScene Assetの選択か新規作成で確定する。
    if (currentSceneFilename_[0] == '\0') {
        DebugConsole::GetInstance()->AddLog("Scene Assetを開くか新規作成してから保存してください。");
        return;
    }
#ifdef USE_IMGUI
    // ImGui 有効時は保存対象をプレビューに積み、ユーザーの確定まで書き込まない。
    auto targets = serializer_.BuildSceneSaveTargets(currentSceneFilename_, mode);

    // 選択した保存モードに該当するファイルがなければ、確認画面は開かない。
    if (targets.empty()) {
        DebugConsole::GetInstance()->AddLog("Save Preview: 保存対象がありません。");
        return;
    }

    sceneSavePreview_.Build(targets, MakeSavePreviewTitle(mode));
    pendingSaveMode_ = mode;
    pendingSaveIsSingleObject_ = false;
    sceneSavePreview_.Open();
#else
    serializer_.SaveScene(currentSceneFilename_, mode);
#endif
}

void DebugEditor::SaveSingleObject() {
    // 選択 Object だけを保存対象にし、通常の Scene 全体保存とは区別する。
    // Prefab 編集中は、選択物の保存操作も専用の保存処理に委ねる。
    if (prefabEditMode_) {
        SavePrefabEditSession();
        return;
    }

    // Scene Asset が未選択なら、単体保存の書き込み先を確定できない。
    if (currentSceneFilename_[0] == '\0') {
        DebugConsole::GetInstance()->AddLog("Scene Assetを開くか新規作成してから保存してください。");
        return;
    }

    // 選択物がなければ、単体保存の対象を用意できない。
    if (!selectedObject_)
        return;

#ifdef USE_IMGUI
    auto targets = serializer_.BuildSingleObjectSaveTargets(selectedObject_, std::string(currentSceneFilename_));

    // 選択物から保存対象を組み立てられない場合は、確認画面を開かない。
    if (targets.empty()) {
        DebugConsole::GetInstance()->AddLog("Save Preview: 単体保存の対象を作成できませんでした。");
        return;
    }

    sceneSavePreview_.Build(targets, "単体保存: " + selectedObject_->GetName());
    pendingSaveMode_ = selectedObject_->IsCameraObject() ? SaveMode::kCamera : SaveMode::kObject;
    // 単体保存で同じカテゴリ内の別 Object も保存済み扱いにならないよう、確定時にカテゴリの Dirty を消す対象から外す。
    pendingSaveIsSingleObject_ = true;
    sceneSavePreview_.Open();
#else
    serializer_.UpdateObjectInSceneJson(selectedObject_, std::string(currentSceneFilename_));
#endif
}

void DebugEditor::DrawSavePreview() {
#ifdef USE_IMGUI
    // 確定時だけ書き込み、保存待ちの Scene 切り替えは Dirty が消えた後に行う。
    SceneSavePreview::Action action = sceneSavePreview_.Draw();

    // 確認もキャンセルも選ばれていないフレームは、画面の状態を保持する。
    if (action == SceneSavePreview::Action::kNone) {
        return;
    }

    // 保存が確定した場合だけ、確認画面に積んだ保存対象へ書き込む。
    if (action == SceneSavePreview::Action::kConfirm) {
        std::string savedFiles = serializer_.SaveTargets(sceneSavePreview_.GetTargets());

        // カテゴリ単位の保存の場合はそのカテゴリの未保存フラグを消す。単体保存は同じカテゴリに別の変更が残り得る。
        if (!pendingSaveIsSingleObject_) {
            ClearDirty(pendingSaveMode_);
        }

        std::string notificationName = sceneSavePreview_.GetTitle();

        // 実際に書き出したファイルの一覧が空でない場合は、保存通知の見出しに添える。
        if (!savedFiles.empty()) {
            notificationName += " (" + savedFiles + ")";
        }

        DebugConsole::GetInstance()->AddLog("Save Preview: 保存を確定しました。");
        TriggerSaveNotification(notificationName);

        // 保存後に開く Asset が予約済みで、未保存フラグが一つも残っていない場合だけ遷移を試す。
        if (!pendingSceneAssetOpenAfterSave_.empty() && !HasAnyDirty()) {
            // 開く処理が失敗しても次回の保存で勝手に再試行しないよう、遷移先の予約を消費してから要求を出す。
            const std::string filenameToOpen = pendingSceneAssetOpenAfterSave_;
            pendingSceneAssetOpenAfterSave_.clear();
            std::string openError;

            // 開く要求が成立しなければ、保存後の遷移エラーをコンソールへ出す。
            if (!OpenSceneAsset(filenameToOpen, false, openError)) {
                DebugConsole::GetInstance()->AddLog("Scene Asset Open Error: " + openError);
            }
        }
    }
    // キャンセルされた場合は書き込まず、保存後に開く予約も消す。
    else {
        DebugConsole::GetInstance()->AddLog("Save Preview: 保存をキャンセルしました。");
        pendingSceneAssetOpenAfterSave_.clear();
    }

    sceneSavePreview_.Close();
    pendingSaveIsSingleObject_ = false;
#endif
}

std::string DebugEditor::MakeSavePreviewTitle(SaveMode mode) const {
    // 保存対象のカテゴリを確認画面の見出しに含め、対象 JSON を判別しやすくする。
    std::string baseName = currentSceneFilename_;
    size_t extPos = baseName.find(".json");
    // 名前に .json が含まれていれば、その部分から後ろを見出しでは省く。
    if (extPos != std::string::npos)
        baseName = baseName.substr(0, extPos);

    switch (mode) {
    // プレイヤー配置だけを確認する保存。
    case SaveMode::kPlayer:
        return "シーン保存: " + baseName + " / Player";

    // 敵配置だけを確認する保存。
    case SaveMode::kEnemy:
        return "シーン保存: " + baseName + " / Enemy";

    // その他のオブジェクト配置だけを確認する保存。
    case SaveMode::kObject:
        return "シーン保存: " + baseName + " / Object";

    // カメラ配置だけを確認する保存。
    case SaveMode::kCamera:
        return "シーン保存: " + baseName + " / Camera";

    // 全カテゴリの保存と、個別の表示名を持たないモードには全体保存の見出しを使う。
    case SaveMode::kAll:
    default:
        return "シーン全体保存: " + baseName;
    }
}

