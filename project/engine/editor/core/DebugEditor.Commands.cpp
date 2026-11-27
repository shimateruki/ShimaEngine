#define NOMINMAX
#include <cstdint>
#include "DebugEditor.h"
#include "EditorCommon.h"
#include <numbers>
#include "engine/editor/assets/EditorObjectDefaults.h"
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
#include "game/actor/enemy/spawn/EnemyFactory.h"
#include "game/system/scene/GimmickFactory.h"
#include "game/system/scene/ItemFactory.h"
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <utility>
namespace fs = std::filesystem;

namespace {
// Undo/Redoの保存状態から同じ動的型のObjectを作り直し、型情報が不正なら復元を中止します。
std::unique_ptr<engine::Object3D> CreateEditorObjectFromState(engine::Object3DCommon* common, const nlohmann::json& state) {
    // 初期化資源か保存された実行型の名前が欠けている場合は、同じ型を保証できないため復元しない。
    if (!common || !state.is_object() || !state.contains("_editorRuntimeType") ||
        !state["_editorRuntimeType"].is_string()) {
        return nullptr;
    }

    const std::string expectedType = state["_editorRuntimeType"].get<std::string>();
    // 型名が空の状態では生成に使うFactoryを選べない。
    if (expectedType.empty())
        return nullptr;
    const std::string enemyType = state.value("enemyType", std::string{});
    const std::string gimmickType = state.value("gimmickType", std::string{});
    const std::string itemType = state.value("itemType", std::string{});
    std::unique_ptr<engine::Object3D> object;

    // 最初から通常のObject3Dだった場合は、派生型を作るFactoryを介さず初期化する。
    if (expectedType == typeid(engine::Object3D).name()) {
        object = std::make_unique<engine::Object3D>();
        object->Initialize(common);
    // 敵種別が記録されている派生型は、敵のFactoryで生成する。
    } else if (!enemyType.empty()) {
        object = EnemyFactory::GetInstance()->CreateEnemy(enemyType, common);
    } else if (!gimmickType.empty() || state.contains("_editorGimmickFactoryType")) {
        // 旧シーンの未知IDを仮Objectとして読み込んだ場合も、同じ実行型へ復元します。
        object = GimmickFactory::GetInstance()->CreateGimmick(state.value("_editorGimmickFactoryType", gimmickType), common);
    // アイテム種別が記録されている場合は、アイテム用のFactoryで復元する。
    } else if (!itemType.empty()) {
        object = ItemFactory::GetInstance()->CreateItem(itemType, common);
    }

    // 不明な型やFactoryの代替オブジェクトを、元の派生型として扱わない。
    if (!object || expectedType != typeid(*object).name()) {
        return nullptr;
    }

    return object;
}

}





// Undo/Redoで使う型、属性、親への参照を保存する。対象を取得できなければ空のオブジェクトを返す。
nlohmann::json DebugEditor::CaptureObjectState(engine::Object3D* object) const {
    nlohmann::json state = nlohmann::json::object();
    // 対象が無ければ復元に使う値を控えられない。
    if (!object)
        return state;
    // 選択中のポインターが現在のシーンから外れていれば、古い参照を読まない。
    if (object == selectedObject_ && !IsObjectInCurrentScene(object))
        return state;

    state = object->ExportToJson();

    // Editor の Undo/Redo はこの実行型を復元できる場合だけ再生成する。
    state["_editorRuntimeType"] = typeid(*object).name();
    // ギミックだけは、実行型とは別にFactoryで使った作成種別を取得する。
    if (auto* gimmick = dynamic_cast<BaseGimmick*>(object)) {
        const auto creationType = GimmickFactory::GetInstance()->GetCreationType(*gimmick);
        // Factoryが種別を解決できた場合だけ、そのキーも保存状態へ含める。
        if (!creationType.empty()) state["_editorGimmickFactoryType"] = creationType;
    }
    state["name"] = object->GetName();
    editorCommon::WriteObjectParentReference(state, *object);
    state["isStatic"] = object->IsStatic();
    return state;
}

std::vector<DebugEditor::ObjectStateSnapshot> DebugEditor::CaptureObjectStates(const std::vector<engine::Object3D*>& objects) const {
    // 現在 Scene に属する Object だけ記録し、破棄済みポインタを Undo 対象にしない。
    std::vector<ObjectStateSnapshot> snapshots;
    snapshots.reserve(objects.size());

    for (engine::Object3D* object : objects) {
        // 選択集合から消去済みの個体や他シーンの個体を除く。
        if (!object || !IsObjectInCurrentScene(object)) {
            continue;
        }

        ObjectStateSnapshot snapshot;
        snapshot.object = object;
        snapshot.beforeState = CaptureObjectState(object);

        // 取得結果がJSONオブジェクトなら、元の実体との対応を記録する。
        if (snapshot.beforeState.is_object()) {
            snapshots.push_back(std::move(snapshot));
        }
    }

    return snapshots;
}

void DebugEditor::ApplyObjectState(engine::Object3D* object, const nlohmann::json& state) {
    // JSON の属性を戻した後、親は GUID を優先して探し、旧データだけ名前へフォールバックする。
    if (!object || !state.is_object())
        return;

    // 名前が保存されている場合は、実体へ元の名前を戻す。
    if (state.contains("name")) {
        object->SetName(state["name"].get<std::string>());
    }

    object->ImportFromJson(state);

    // 静的判定のフラグが記録されていた場合は、JSONを読み込んだ後に明示的に戻す。
    if (state.contains("isStatic")) {
        object->SetStatic(state["isStatic"].get<bool>());
    }

    engine::Object3D* parent = nullptr;

    // 文字列の親GUIDが保存されていれば、現在のシーンから優先して探す。
    if (state.contains("parentGuid") && state["parentGuid"].is_string()) {
        parent = FindObjectByPersistentGuid(state["parentGuid"].get<std::string>());
        // 復元先自身を親にする値は採用しない。
        if (parent == object)
            parent = nullptr;
    }

    // GUIDで親を取得できず互換用の名前がある場合は、名前で探す。
    if (!parent && state.contains("parentName")) {
        std::string parentName = state["parentName"].get<std::string>();

        // 空の名前は親なしを表すので検索しない。
        if (!parentName.empty()) {
            parent = FindObjectByName(parentName);
            // 名前で自分自身が見つかった場合も親なしとする。
            if (parent == object)
                parent = nullptr;
        }
    }

    // 親を確定してからローカル・ワールドの順に行列を更新する。
    object->SetParent(parent);
    object->UpdateLocalMatrix();
    object->UpdateWorldMatrix();
}

engine::Object3D* DebugEditor::FindObjectByName(const std::string& name) const {
    // 旧コマンド用の代替検索なので、対象は現在開いている Scene に限る。
    if (name.empty() || !sceneManager_ || !sceneManager_->GetCurrentScene())
        return nullptr;

    auto& objects = sceneManager_->GetCurrentScene()->GetObjects();

    for (const auto& object : objects) {
        // 有効な個体の名前が一致した時点で、その個体を返す。
        if (object && object->GetName() == name) {
            return object.get();
        }
    }

    return nullptr;
}

engine::Object3D* DebugEditor::FindObjectByPersistentGuid(const std::string& guid) const {
    // 名前が変わった後でも同じ Object を復元できるよう永続 ID で検索する。
    if (guid.empty() || !sceneManager_ || !sceneManager_->GetCurrentScene())
        return nullptr;
    return sceneManager_->GetCurrentScene()->FindObjectByPersistentGuid(guid);
}

engine::Object3D* DebugEditor::AddObjectFromState(const nlohmann::json& state) {
    // 保存時の実行型を再生成できる場合だけ追加し、GUID 衝突時は新しい ID を振る。
    if (!sceneManager_ || !sceneManager_->GetCurrentScene() || !state.is_object())
        return nullptr;

    engine::Object3DCommon* common = sceneManager_->GetCurrentScene()->GetObject3DCommon();
    // 3Dの生成資源が無ければ、型を再現できても個体を初期化できない。
    if (!common)
        return nullptr;

    auto object = CreateEditorObjectFromState(common, state);

    // 保存されている実行型を作れなかった場合は、代用せず復元を省く。
    if (!object) {
        DebugConsole::GetInstance()->AddLog("Object restore skipped: runtime type cannot be reconstructed.");
        return nullptr;
    }

    ApplyObjectState(object.get(), state);

    // 同じGUIDを持つ個体が既にある場合は、新しく置く側へ別のIDを付ける。
    if (FindObjectByPersistentGuid(object->GetPersistentGuid())) {
        object->RegeneratePersistentGuid();
    }

    engine::Object3D* raw = object.get();
    sceneManager_->GetCurrentScene()->AddObjectImmediate(std::move(object));

    // 復元した個体が敵の場合は、行動や生成で参照するシーンも結び直す。
    if (auto* enemy = dynamic_cast<BaseEnemy*>(raw)) {
        enemy->BindSceneSpawnContext(sceneManager_->GetCurrentScene());
    }

    return raw;
}

std::unique_ptr<engine::Object3D> DebugEditor::RemoveObjectImmediate(engine::Object3D* object) {
    // Scene 配列から即時に切り離し、所有権を呼び出し元へ返す。
    if (!object || !sceneManager_ || !sceneManager_->GetCurrentScene())
        return nullptr;
    return sceneManager_->GetCurrentScene()->TakeObjectImmediate(object);
}

void DebugEditor::RequestObjectTypeChange(engine::Object3D* object, EditorObjectFactoryRegistry::Kind kind, const std::string& type) {
    // 対象の実体と編集可能なシーンが揃っている場合だけ、種類変更を予約する。
    if (!object || !IsObjectInCurrentScene(object) || sceneManager_->IsPlaying()) return;
    const std::string guid = object->EnsurePersistentGuid();
    const auto generation = sceneManager_->GetSceneGeneration();
    // Inspector の描画中には実体を置き換えない。後で GUID から探し直し、別のシーンに切り替わっていれば予約を無効にする。
    pendingObjectTypeChange_ = [this, guid, kind, type, generation] {
        // 予約後に別シーンへ切り替わっていなければ、GUIDから現在の対象を調べ直す。
        if (sceneManager_->GetSceneGeneration() == generation)
            ChangeObjectType(FindObjectByPersistentGuid(guid), kind, type);
    };
}

bool DebugEditor::ChangeObjectType(engine::Object3D* object, EditorObjectFactoryRegistry::Kind kind, const std::string& type) {
    // 対象が現在シーンの一員で、プレイ・遷移していない場合だけ実体を交換する。
    if (!object || !IsObjectInCurrentScene(object) || sceneManager_->IsPlaying() || sceneManager_->IsTransitioning()) return false;
    auto* common = sceneManager_->GetCurrentScene()->GetObject3DCommon();
    std::unique_ptr<engine::Object3D> replacement;
    // 標準ギミックへ戻す指定では、専用の派生型を作らず通常のObject3Dを使う。
    if (kind == EditorObjectFactoryRegistry::Kind::kGimmick && type == "Default") {
        replacement = std::make_unique<engine::Object3D>();
        replacement->Initialize(common);
        replacement->SetClassName("Default");
    // その他の種類は登録したFactoryが生成を担当する。
    } else {
        replacement = EditorObjectFactoryRegistry::Create(kind, type, common);
    }
    // 希望する実体を生成できなければ、現在のオブジェクトを保持する。
    if (!replacement) return false;

    const auto before = CaptureObjectState(object);
    auto state = before;
    // 位置や識別情報は編集中の状態から引き継ぎ、復元に使う C++ 型と分類だけを置き換える。
    state["_editorRuntimeType"] = typeid(*replacement).name();
    // 新しい個体がギミックの場合は、その作成種別をUndo用の記録に含める。
    if (auto* gimmick = dynamic_cast<BaseGimmick*>(replacement.get()))
        state["_editorGimmickFactoryType"] = GimmickFactory::GetInstance()->GetCreationType(*gimmick);
    // ギミックでなくなった場合は、以前のFactoryキーを削除する。
    else
        state.erase("_editorGimmickFactoryType");
    state["type"] = replacement->GetClassName();
    const char* key = kind == EditorObjectFactoryRegistry::Kind::kEnemy ? "enemyType" :
                      kind == EditorObjectFactoryRegistry::Kind::kItem ? "itemType" : "gimmickType";
    state[key] = type;
    if (before.value(key, std::string{}) != type) {
        // 型を変える時だけ、その型の既定パラメーターにします。同じ型の修復時は保存値を保ちます。
        const auto defaults = replacement->ExportToJson();
        // 選んだ型が既定パラメーターを持つ場合のみ、その型の値を引き継ぐ。
        if (defaults.contains("param")) state["param"] = defaults["param"];
        // 既定パラメーターがない型に変える場合は、旧型の値を持ち越さない。
        else state.erase("param");
    }
    auto* changed = ReplaceObjectFromState(object, state, std::move(replacement));
    // 実体の差し替えが成立しなければ、編集履歴を追加しない。
    if (!changed) return false;
    EditorCommand command;
    command.type = EditorCommandType::kObjectReplaced;
    command.label = "種類の変更";
    command.beforeState = before;
    command.afterState = CaptureObjectState(changed);
    RegisterCommand(command);
    return true;
}

engine::Object3D* DebugEditor::ReplaceObjectFromState(engine::Object3D* object, const nlohmann::json& state,
    std::unique_ptr<engine::Object3D> replacement) {
    // 差し替えは現在のシーンに属する対象に対し、編集時だけ許可する。
    if (!object || !IsObjectInCurrentScene(object) || sceneManager_->IsPlaying()) return nullptr;
    auto* scene = sceneManager_->GetCurrentScene();
    // 新しい実体を受け取っていない場合は、保存状態から同じ型を復元する。
    if (!replacement) replacement = CreateEditorObjectFromState(scene->GetObject3DCommon(), state);
    // 代わりの実体を用意できなければ、元の個体を取り外さない。
    if (!replacement) return nullptr;
    ApplyObjectState(replacement.get(), state);

    // 作成・検証が済むまで元Objectを触りません。所有権の取り出しに失敗した場合も現状を保ちます。
    auto previous = RemoveObjectImmediate(object);
    // 元の個体の所有権を取得できなかった場合は、登録や選択の切り替えへ進まない。
    if (!previous) return nullptr;
    // 元の所有権を受け取ったまま子を控え、新しい実体を登録してから既存の子を親付けし直す。
    const auto children = previous->GetChildren();
    previous->SetParent(nullptr);
    auto* raw = scene->AddObjectImmediate(std::move(replacement));
    // 差し替え前の子で、現在も有効な個体を新しい親へつなぐ。
    for (auto* child : children) if (child) child->SetParent(raw);
    // 選択とInspectorの基準状態を、新しい実体に切り替える。
    ClearObjectSelection();
    SetSelectedObject(raw);
    EditorManager::GetInstance()->SetSelectedObject(this);
    hasInspectorEditStart_ = false;
    inspectorEditStartStates_.clear();
    MarkDirtyForObject(raw);
    return raw;
}

void DebugEditor::RegisterCommand(const EditorCommand& command) {
    // 同じコマンドを逆方向にも適用し、共通 Transaction 履歴へ登録する。
    EditorTransaction transaction;
    transaction.label = command.label;
    transaction.undo = [this, command]() { ApplyEditorCommand(command, true); };
    transaction.redo = [this, command]() { ApplyEditorCommand(command, false); };
    EditorTransactionManager::GetInstance()->Register(std::move(transaction));
}

void DebugEditor::RegisterObjectEdited(engine::Object3D* object, const std::string& label) {
    // 編集前の記録を持たない入口。通常の編集履歴を残す呼び出し側は、変更前の JSON を控えて beforeState 付き版へ渡す。
    // 対象が取得できない場合は変更後の値も記録できない。
    if (!object)
        return;
    // 選択が古い参照なら、現在の保存状態を取得しない。
    if (object == selectedObject_ && !IsObjectInCurrentScene(object))
        return;
    RegisterObjectEdited(object, CaptureObjectState(object), label);
}

void DebugEditor::RegisterObjectEdited(engine::Object3D* object, const nlohmann::json& beforeState, const std::string& label) {
    // 前後の JSON が同じなら履歴を作らず、実際の変更だけ Dirty にする。
    if (!object || !beforeState.is_object())
        return;
    // 選択中の個体がシーンから外れている場合は、履歴に残さない。
    if (object == selectedObject_ && !IsObjectInCurrentScene(object))
        return;

    nlohmann::json afterState = CaptureObjectState(object);
    // 実際に保存状態が変わったときだけ履歴と未保存フラグを更新する。
    if (beforeState == afterState)
        return;

    EditorCommand command;
    command.type = EditorCommandType::kObjectEdited;
    command.label = label;
    command.beforeState = beforeState;
    command.afterState = afterState;
    command.beforeName = beforeState.value("name", object->GetName());
    command.afterName = afterState.value("name", object->GetName());
    RegisterCommand(command);
    MarkDirtyForObject(object);
}

void DebugEditor::RegisterObjectsEdited(const std::vector<ObjectStateSnapshot>& beforeStates, const std::string& label) {
    // 一括編集の前後を GUID に結び付ける。名前変更後も同じ対象へ Undo/Redo を適用するための記録。
    struct StateChange {
        std::string guid;
        std::string beforeName;
        std::string afterName;
        nlohmann::json beforeState;
        nlohmann::json afterState;
    };

    std::vector<StateChange> changes;
    changes.reserve(beforeStates.size());

    for (const ObjectStateSnapshot& snapshot : beforeStates) {
        // 現在の実体か編集前の保存状態が無効な項目は一括変更に含めない。
        if (!snapshot.object || !snapshot.beforeState.is_object() || !IsObjectInCurrentScene(snapshot.object)) {
            continue;
        }

        nlohmann::json afterState = CaptureObjectState(snapshot.object);

        // 編集後の値が無効な項目と、編集前後が同じ項目は取り除く。
        if (!afterState.is_object() || snapshot.beforeState == afterState) {
            continue;
        }

        StateChange change;
        change.guid = snapshot.beforeState.value("guid", snapshot.object->GetPersistentGuid());
        change.beforeName = snapshot.beforeState.value("name", snapshot.object->GetName());
        change.afterName = afterState.value("name", snapshot.object->GetName());
        change.beforeState = snapshot.beforeState;
        change.afterState = std::move(afterState);
        changes.push_back(std::move(change));
        MarkDirtyForObject(snapshot.object);
    }

    // 全項目が変更なしなら、まとめた操作を一件として登録する必要がない。
    if (changes.empty()) {
        return;
    }

    auto applyStates = [this, changes](bool undo) {
        std::vector<engine::Object3D*> appliedObjects;
        appliedObjects.reserve(changes.size());

        for (const StateChange& change : changes) {
            const std::string& primaryName = undo ? change.afterName : change.beforeName;
            const std::string& fallbackName = undo ? change.beforeName : change.afterName;
            engine::Object3D* object = FindObjectByPersistentGuid(change.guid);
            // GUIDが一致しなければ、まず操作する側の履歴名で探す。
            if (!object)
                object = FindObjectByName(primaryName);
            // その名前も見つからなければ、操作の逆側の履歴名で探す。
            if (!object) {
                object = FindObjectByName(fallbackName);
            }

            // 両方の検索で取得できない対象は飛ばし、残りを適用する。
            if (!object) {
                continue;
            }

            // 保存先カテゴリが変わる編集では、復元前のカテゴリ側も更新対象に残します。
            MarkDirtyForObject(object);
            ApplyObjectState(object, undo ? change.beforeState : change.afterState);
            MarkDirtyForObject(object);
            appliedObjects.push_back(object);
        }

        // 復元できた対象が一件以上なら、それらへ選択を移す。
        if (!appliedObjects.empty()) {
            ClearObjectSelection();

            for (engine::Object3D* object : appliedObjects) {
                AddSelectedObject(object);
            }

            EditorManager::GetInstance()->SetSelectedObject(this);
        }
    };

    EditorTransaction transaction;
    transaction.label = label;
    transaction.undo = [applyStates]() { applyStates(true); };
    transaction.redo = [applyStates]() { applyStates(false); };
    EditorTransactionManager::GetInstance()->Register(std::move(transaction));
}

void DebugEditor::AddEditorObject(std::unique_ptr<engine::Object3D> object, const std::string& label) {
    // 配置する実体と対象シーンが揃わない場合は履歴も作成しない。
    if (!object || !sceneManager_ || !sceneManager_->GetCurrentScene())
        return;

    engine::Object3D* raw = object.get();
    raw->EnsurePersistentGuid();

    // 保存済みのGUIDと衝突する場合は入力のオブジェクトへ振り直す。
    if (FindObjectByPersistentGuid(raw->GetPersistentGuid())) {
        raw->RegeneratePersistentGuid();
    }

    if (prefabEditMode_ && prefabEditRoot_ && raw != prefabEditRoot_ && raw->GetParent() == nullptr) {
        // Prefab Modeで作成したRoot Objectは、編集対象Prefabの直下へ自動的に追加します。
        raw->SetParent(prefabEditRoot_, true);
    }

    // 既定の床判定を追加した後の状態を控え、作成のやり直しでも同じ判定を復元する。
    editorObjectDefaults::ApplyGroundCollision(raw);
    nlohmann::json afterState = CaptureObjectState(raw);

    sceneManager_->GetCurrentScene()->AddObjectImmediate(std::move(object));
    SetSelectedObject(raw);
    EditorManager::GetInstance()->SetSelectedObject(this);

    EditorCommand command;
    command.type = EditorCommandType::kObjectCreated;
    command.label = label;
    command.afterState = afterState;
    command.afterName = afterState.value("name", raw->GetName());
    RegisterCommand(command);
    MarkDirtyForObject(raw);
}

void DebugEditor::AddEditorObjects(std::vector<std::unique_ptr<engine::Object3D>> objects, const std::string& label) {
    // 複数配置を Scene へ移し、最後に先頭 Object を選択して編集を続けられるようにする。
    if (objects.empty())
        return;

    // 追加ごとに AddEditorObject が選択と作成履歴を更新する。最後に先頭へ選択を移し、階層全体の編集へ戻す。
    engine::Object3D* root = objects.front().get();

    for (auto& object : objects) {
        // 所有する個体のうち、実体が有効なものだけシーンへ移す。
        if (object) {
            AddEditorObject(std::move(object), label);
        }
    }

    // グループの先頭が有効なら、子を追加し終わったあと先頭の選択へ戻す。
    if (root) {
        SetSelectedObject(root);
        EditorManager::GetInstance()->SetSelectedObject(this);
    }
}

void DebugEditor::TrackInspectorEdit(const std::vector<ObjectStateSnapshot>& beforeStates) {
#ifdef USE_IMGUI
    // ドラッグ中の各フレームを履歴にせず、操作開始と終了を一つの編集として記録する。
    if (!selectedObject_ || beforeStates.empty()) {
        return;
    }

    // 選択がシーンから外れていれば、古い参照を解除して操作を終える。
    if (!IsObjectInCurrentScene(selectedObject_)) {
        ClearInvalidSelectedObject();
        return;
    }

    bool changedThisFrame = false;

    for (const ObjectStateSnapshot& snapshot : beforeStates) {
        // 同じシーンで有効な実体と開始前の記録を持つものだけ比較する。
        if (!snapshot.object || !snapshot.beforeState.is_object() || !IsObjectInCurrentScene(snapshot.object)) {
            continue;
        }

        // 保存状態が一件でも変わったらこのフレームを変更ありとする。
        if (snapshot.beforeState != CaptureObjectState(snapshot.object)) {
            changedThisFrame = true;
            break;
        }
    }

    bool active = ImGui::IsAnyItemActive();

    // 最初に値が変わったフレームの状態だけを Undo の基準にする。途中のフレームで基準を更新しない。
    if (changedThisFrame && !hasInspectorEditStart_) {
        inspectorEditStartStates_ = beforeStates;
        hasInspectorEditStart_ = true;
    }

    // このフレームに変更があれば、操作終了を待たず各対象の保存を要する状態へ更新する。
    if (changedThisFrame) {
        for (const ObjectStateSnapshot& snapshot : beforeStates) {
            // 対象が現在のシーンに残っている場合だけ、所属するカテゴリを変更済みにする。
            if (snapshot.object && IsObjectInCurrentScene(snapshot.object)) {
                MarkDirtyForObject(snapshot.object);
            }
        }
    }

    // 入力が完了した時点で前後を比較し、ドラッグ一回を一件の操作として確定する。
    if (!active && hasInspectorEditStart_) {
        RegisterObjectsEdited(inspectorEditStartStates_,
            inspectorEditStartStates_.size() > 1 ? "Inspector Multi Edit" : "Inspector Edit");
        hasInspectorEditStart_ = false;
        inspectorEditStartStates_.clear();
    }
#endif
}

void DebugEditor::MarkDirtyForObject(engine::Object3D* object) {
    // Prefab 編集中の変更は Scene Asset ではなく Prefab の未保存状態へ振り分ける。
    // 具体的な対象が無ければ、一般オブジェクトの保存を要する状態にする。
    if (!object) {
        MarkDirty(SaveMode::kObject);
        return;
    }

    // Prefab編集中にそのPrefabの個体が変わった場合は、Sceneとは別の未保存フラグを使う。
    if (prefabEditMode_ && IsPrefabEditObject(object)) {
        prefabEditDirty_ = true;
        return;
    }

    MarkDirtyForCategory(object->GetSaveCategory());
}

void DebugEditor::MarkDirtyForCategory(const std::string& category) {
    // 保存先 JSON の区分に合わせて未保存フラグを立てる。
    // Playerカテゴリは、プレイヤー用の配置JSONを保存対象にする。
    if (category == "Player")
        dirtyPlayer_ = true;
    // Enemyカテゴリの場合は、敵の配置JSONを保存対象にする。
    else if (category == "Enemy") dirtyEnemy_ = true;
    // Cameraカテゴリの場合は、カメラの配置JSONを保存対象にする。
    else if (category == "Camera") dirtyCamera_ = true;
    // 残りのカテゴリは一般オブジェクトの配置JSONを保存対象にする。
    else dirtyObject_ = true;
}

void DebugEditor::MarkDirty(SaveMode mode) {
    // All 指定は各カテゴリをまとめて未保存へし、個別指定は対象だけ変える。
    switch (mode) {
    case SaveMode::kPlayer:
        dirtyPlayer_ = true;
        break;

    case SaveMode::kEnemy:
        dirtyEnemy_ = true;
        break;

    case SaveMode::kObject:
        dirtyObject_ = true;
        break;

    case SaveMode::kCamera:
        dirtyCamera_ = true;
        break;

    case SaveMode::kAll:
    default:
        dirtyPlayer_ = true;
        dirtyEnemy_ = true;
        dirtyObject_ = true;
        dirtyCamera_ = true;
        break;
    }
}

void DebugEditor::ClearDirty(SaveMode mode) {
    // 保存済みになったカテゴリだけフラグを下ろす。
    switch (mode) {
    case SaveMode::kPlayer:
        dirtyPlayer_ = false;
        break;

    case SaveMode::kEnemy:
        dirtyEnemy_ = false;
        break;

    case SaveMode::kObject:
        dirtyObject_ = false;
        break;

    case SaveMode::kCamera:
        dirtyCamera_ = false;
        break;

    case SaveMode::kAll:
    default:
        dirtyPlayer_ = false;
        dirtyEnemy_ = false;
        dirtyObject_ = false;
        dirtyCamera_ = false;
        break;
    }
}

bool DebugEditor::IsDirty(SaveMode mode) const {
    // All はいずれか一件でも未保存なら true とする。
    switch (mode) {
    case SaveMode::kPlayer:
        return dirtyPlayer_;

    case SaveMode::kEnemy:
        return dirtyEnemy_;

    case SaveMode::kObject:
        return dirtyObject_;

    case SaveMode::kCamera:
        return dirtyCamera_;

    case SaveMode::kAll:
    default:
        return HasAnyDirty();
    }
}

bool DebugEditor::HasAnyDirty() const {
    // Scene の保存区分だけを判定する。Prefab 編集専用の変更は prefabEditDirty_ で別途管理する。
    return dirtyPlayer_ || dirtyEnemy_ || dirtyObject_ || dirtyCamera_;
}

std::string DebugEditor::GetDirtySummaryText() const {
    // 未保存の JSON 区分だけ列挙し、画面上で保存漏れを判別できるようにする。
    if (!HasAnyDirty())
        return "保存済み";

    std::string text = "未保存:";
    // プレイヤー配置に変更がある場合だけ表示へ含める。
    if (dirtyPlayer_)
        text += " Player";
    // 敵配置に変更がある場合は表示へ含める。
    if (dirtyEnemy_)
        text += " Enemy";
    // 一般オブジェクトに変更がある場合は表示へ含める。
    if (dirtyObject_)
        text += " Object";
    // カメラ配置に変更がある場合は表示へ含める。
    if (dirtyCamera_)
        text += " Camera";
    return text;
}

// 選択中の実行型を複製する。複数選択ではコピー同士の親子関係も再現する。
void DebugEditor::DuplicateSelected() {
    PruneInvalidSelectedObjects();
    // 現在の選択とシーンの組み合わせが有効な場合だけ複製する。
    if (!selectedObject_ || !sceneManager_->GetCurrentScene())
        return;

    // 複数選択中は、親子の対応をまとめて記録してから配置する。
    if (selectedObjects_.size() > 1) {
        std::vector<engine::Object3D*> sourceObjects = selectedObjects_;
        std::vector<engine::Object3D*> createdObjects;
        createdObjects.reserve(sourceObjects.size());
        std::unordered_map<engine::Object3D*, engine::Object3D*> duplicateMap;

        /// <summary>複製元、保存状態、生成中の複製Objectを確定前にまとめて保持する。</summary>
        struct PreparedDuplicate {
            engine::Object3D* source;
            nlohmann::json state;
            std::unique_ptr<engine::Object3D> object;
        };

        std::vector<PreparedDuplicate> prepared;
        std::unordered_set<std::string> reservedNames;
        engine::Object3DCommon* common = sceneManager_->GetCurrentScene()->GetObject3DCommon();
        // 生成用の3D資源が無ければ、どの候補も登録しない。
        if (!common)
            return;

        const engine::Vector3 duplicateOffset = { 1.5f, 0.0f, 1.5f };

        // 一つでも元と同じ型を生成できなければ、シーンへ何も追加せず複製を中止する。
        for (engine::Object3D* source : sourceObjects) {
            // 元が無効か現在のシーンに属さない場合は、この候補を複製しない。
            if (!source || !IsObjectInCurrentScene(source))
                continue;

            nlohmann::json duplicatedState = CaptureObjectState(source);
            duplicatedState.erase("guid");
            std::string baseName = source->GetName().empty() ? "Object" : source->GetName();
            std::string newName = baseName + "_copy";
            std::int32_t suffix = 1;

            // 既存の名前か同じ操作で予約した名前に当たる間は、末尾の番号を変えて空きを探す。
            while (FindObjectByName(newName) || reservedNames.find(newName) != reservedNames.end()) {
                newName = baseName + "_copy" + std::to_string(suffix++);
            }

            reservedNames.insert(newName);
            duplicatedState["name"] = newName;

            auto object = CreateEditorObjectFromState(common, duplicatedState);

            // 保存状態と同じ型の実体を一つでも作れなければ、未登録の一括操作を中止する。
            if (!object) {
                DebugConsole::GetInstance()->AddLog("Duplicate cancelled: " + source->GetName() +
                                                    " has no matching runtime factory.");
                return;
            }

            prepared.push_back({source, std::move(duplicatedState), std::move(object)});
        }

        // 全候補を生成できた後でシーンに追加する。先に全コピーの対応を作り、親子関係は次の段階で結び直す。
        for (auto& candidate : prepared) {
            ApplyObjectState(candidate.object.get(), candidate.state);

            // 読み込んだGUIDが現シーンにあれば、複製した実体のIDを作り直す。
            if (FindObjectByPersistentGuid(candidate.object->GetPersistentGuid())) {
                candidate.object->RegeneratePersistentGuid();
            }

            engine::Object3D* created = candidate.object.get();

            // 敵の複製にも、敵の生成イベントが参照する現在シーンを渡す。
            if (auto* enemy = dynamic_cast<BaseEnemy*>(created)) {
                enemy->BindSceneSpawnContext(sceneManager_->GetCurrentScene());
            }

            sceneManager_->GetCurrentScene()->AddObjectImmediate(std::move(candidate.object));

            const Transform* sourceTransform = candidate.source->GetTransform();
            created->SetTranslate(sourceTransform->translate + duplicateOffset);
            created->UpdateLocalMatrix();
            created->UpdateWorldMatrix();

            duplicateMap[candidate.source] = created;
            createdObjects.push_back(created);
        }

        // 選択範囲内の親子関係は、複製元ではなく複製先同士へ張り直します。
        for (const auto& [source, created] : duplicateMap) {
            engine::Object3D* sourceParent = source ? source->GetParent() : nullptr;
            const auto parent = duplicateMap.find(sourceParent);

            // 元の親も同じ操作で複製した場合だけ、新しい親へ付け替える。
            if (parent != duplicateMap.end()) {
                created->SetParent(parent->second, true);
                created->UpdateWorldMatrix();
            }
        }

        for (engine::Object3D* created : createdObjects) {
            // 再接続後の親情報と姿勢を記録し、やり直しでコピー元の親に戻らないようにする。
            nlohmann::json afterState = CaptureObjectState(created);
            EditorCommand command;
            command.type = EditorCommandType::kObjectCreated;
            command.label = "Duplicate Selected Objects";
            command.afterState = afterState;
            command.afterName = afterState.value("name", created->GetName());
            RegisterCommand(command);
            MarkDirtyForObject(created);
        }

        ClearObjectSelection();

        for (engine::Object3D* created : createdObjects) {
            AddSelectedObject(created);
        }

        PresetManager::GetInstance()->AssignNewPrefabInstanceId(createdObjects);

        // 複製後に選択が残っていれば、その選択をEditorManagerへ知らせる。
        if (selectedObject_) {
            EditorManager::GetInstance()->SetSelectedObject(this);
        }

        DebugConsole::GetInstance()->AddLog("Duplicated Objects: " + std::to_string(createdObjects.size()));
        return;
    }

    // 保存状態から同じ実行型を再生成する。未対応型は別型に変えず複製を中止する。
    nlohmann::json duplicateState = CaptureObjectState(selectedObject_);
    duplicateState.erase("guid");
    std::unique_ptr<engine::Object3D> newObj = CreateEditorObjectFromState(
        sceneManager_->GetCurrentScene()->GetObject3DCommon(), duplicateState);
    // 単体の実行型もFactoryで作れなければ、その型のまま複製できない。
    if (!newObj) {
        DebugConsole::GetInstance()->AddLog("Duplicate cancelled: " + selectedObject_->GetName() +
                                            " has no matching runtime factory.");
        return;
    }

    ApplyObjectState(newObj.get(), duplicateState);

    // Prefab由来の複製には、元のPrefab Instanceとは別の識別子を割り当てる。
    if (newObj && newObj->IsPrefabInstance()) {
        PresetManager::GetInstance()->AssignNewPrefabInstanceId({ newObj.get() });
    }

    // 単体の複製名へ、今回のエディターで使う通し番号を付ける。
    static std::int32_t sDuplicateCount = 0;
    newObj->SetName(selectedObject_->GetName() + "_Copy" + std::to_string(sDuplicateCount++));

    // Game Viewのカーソルからレイを作り、単体の配置候補を探す。
    engine::Math math;
    engine::Ray ray = ScreenPointToRay(gameViewMousePos_);
    engine::Vector3 finalPos = engine::Vector3::kZero;
    bool found = false;

    // 候補をワールド原点とスケールから組み立てた境界箱で調べ、最も手前の交差を保持する。
    auto& objects = sceneManager_->GetCurrentScene()->GetObjects();
    engine::RayResult best;
    best.isHit = false;
    best.distance = 1e5f;

    for (auto& obj : objects) {
        // 自分自身、カーソル、非表示のものは無視
        if (obj.get() == selectedObject_ || obj->GetName() == "Cursor" || obj->GetName() == "Line" || !obj->GetIsRenderVisible())
            continue;

        engine::Matrix4x4 wm = obj->GetWorldMatrix();
        engine::Vector3 wp = { wm.m[3][0], wm.m[3][1], wm.m[3][2] };
        engine::Vector3 ws = obj->GetTransform()->scale;
        engine::RayResult tmp;

        // 候補の境界箱とレイが交差した場合だけ、現在の最短距離と比べる。
        if (math.IntersectRayAabb(ray, wp - ws, wp + ws, &tmp)) {
            // 既に見つかった候補より手前なら、この接点へ入れ替える。
            if (tmp.distance < best.distance)
                best = tmp;
        }
    }

    // 候補へ当たった場合は、交差点のYに複製物の高さの倍率を足す。
    if (best.isHit) {
        finalPos = best.point;
        finalPos.y += newObj->GetTransform()->scale.y;
        found = true;
    }
    // 境界箱への交差が無い場合は、コピー元と視点の間の距離を使う。
    else {
        engine::Matrix4x4 sourceWm = selectedObject_->GetWorldMatrix();
        engine::Vector3 sourcePos = { sourceWm.m[3][0], sourceWm.m[3][1], sourceWm.m[3][2] };

        float diffX = sourcePos.x - ray.origin.x;
        float diffY = sourcePos.y - ray.origin.y;
        float diffZ = sourcePos.z - ray.origin.z;
        float distToRef = std::sqrt(diffX * diffX + diffY * diffY + diffZ * diffZ);

        // カーソル方向へ、その距離を保った点を配置候補とする。
        engine::Vector3 rayDir = math.Normalize(ray.diff);
        finalPos = { ray.origin.x + rayDir.x * distToRef,
                     ray.origin.y + rayDir.y * distToRef,
                     ray.origin.z + rayDir.z * distToRef };
        found = true;
    }


    // 求めた位置を使い、吸着が有効であればXZだけを格子へ合わせる。
    if (found) {
        // 格子吸着を使用中は、現在の間隔の最も近い倍数にXとZを丸める。
        if (isGridSnapEnabled_) {
            finalPos.x = std::round(finalPos.x / snapValue_) * snapValue_;
            finalPos.z = std::round(finalPos.z / snapValue_) * snapValue_;
        }

        newObj->SetTranslate(finalPos);
        DebugConsole::GetInstance()->AddLog("Smart Pasted at Mouse Cursor!");
    }
    // 候補なしの場合に備えて、複製の初期位置から少しX方向へずらす経路を持つ。
    else {
        newObj->SetTranslateX(newObj->GetTranslate().x + 2.0f);
        DebugConsole::GetInstance()->AddLog("Pasted at offset (Ray missed).");
    }


    // ワールド行列を更新した後、派生型の追加設定を行ってシーンへ登録する。
    newObj->UpdateWorldMatrix();

    // 新しい実体が敵なら、現在のシーンを敵の生成コンテキストに設定する。
    if (auto* enemy = dynamic_cast<BaseEnemy*>(newObj.get())) {
        enemy->BindSceneSpawnContext(sceneManager_->GetCurrentScene());
    }

    AddEditorObject(std::move(newObj), "Duplicate Object");
}

// 選択中の階層を削除し、同じ実行型を復元できる状態をUndo履歴に残す。
void DebugEditor::DeleteSelected() {
    // 削除先のシーンがなければ選択を処理しない。
    if (!sceneManager_ || !sceneManager_->GetCurrentScene())
        return;
    PruneInvalidSelectedObjects();
    // 無効な選択を解除した結果、対象がなければ終了する。
    if (selectedObjects_.empty())
        return;
    // Prefabを単独編集中にそのルートが選ばれている場合は、Assetの根を失うため削除しない。
    if (prefabEditMode_ && std::find(selectedObjects_.begin(), selectedObjects_.end(), prefabEditRoot_) != selectedObjects_.end()) {
        DebugConsole::GetInstance()->AddLog("Prefab Mode: Root Objectは削除できません。");
        return;
    }

    // 親から子の順で一度だけ収集する。親子を同時選択しても二重登録しない。
    const auto& sceneObjects = sceneManager_->GetCurrentScene()->GetObjects();
    std::unordered_set<engine::Object3D*> selected(selectedObjects_.begin(), selectedObjects_.end());
    std::unordered_set<engine::Object3D*> visited;
    std::vector<engine::Object3D*> targets;
    std::function<void(engine::Object3D*)> collect = [&](engine::Object3D* object) {
        // 対象が空か既に収集済みなら、子も含めて同じ枝を調べ直さない。
        if (!object || !visited.insert(object).second)
            return;
        targets.push_back(object);
        for (const auto& candidate : sceneObjects) {
            // 現在の個体を親としている子だけ、同じ削除対象に含める。
            if (candidate && candidate->GetParent() == object)
                collect(candidate.get());
        }
    };
    for (auto* object : selectedObjects_) {
        bool ancestorSelected = false;
        std::unordered_set<engine::Object3D*> ancestors;
        for (auto* parent = object->GetParent(); parent && ancestors.insert(parent).second; parent = parent->GetParent()) {
            // 上位に選択済みの親が見つかれば、そちらからこの枝も処理される。
            if (selected.count(parent)) {
                ancestorSelected = true;
                break;
            }
        }
        // 先祖に選択が無い個体だけを、各削除対象の起点として収集する。
        if (!ancestorSelected)
            collect(object);
    }
    // 削除可能な階層を一件も収集できなかった場合は履歴を作らない。
    if (targets.empty())
        return;

    std::vector<nlohmann::json> states;
    auto* common = sceneManager_->GetCurrentScene()->GetObject3DCommon();
    for (auto* object : targets) {
        auto state = CaptureObjectState(object);
        // 復元できない型があれば、何も削除する前に操作全体を中止する。
        if (!CreateEditorObjectFromState(common, state)) {
            DebugConsole::GetInstance()->AddLog("Delete cancelled: " + object->GetName() + " cannot be restored as the same runtime type.");
            return;
        }
        states.push_back(std::move(state));
    }
    auto remove = [this, states]() {
        ClearObjectSelection();
        EditorManager::GetInstance()->ClearSelection();
        // 親を破棄する前に子を消し、親Transformへの参照を残さない。
        for (auto it = states.rbegin(); it != states.rend(); ++it) {
            auto* object = FindObjectByPersistentGuid(it->value("guid", std::string{}));
            // GUIDに対応する実体がまだ存在する場合だけ、カテゴリを記録して取り外す。
            if (object) {
                MarkDirtyForObject(object);
                RemoveObjectImmediate(object);
            }
        }
        // Prefabの編集モードで削除した場合は、Assetの変更を保存対象にする。
        if (prefabEditMode_)
            prefabEditDirty_ = true;
    };
    EditorTransaction transaction;
    transaction.label = states.size() > 1 ? "Delete Object Hierarchy" : "Delete Object";
    transaction.undo = [this, states]() {
        engine::Object3D* first = nullptr;
        // 保存順に親を先に生成し、GUIDで元の階層とローカル座標を復元する。
        for (const auto& state : states) {
            auto* object = AddObjectFromState(state);
            // 保存状態から復元できた個体だけ、未保存と選択の候補にする。
            if (object) {
                MarkDirtyForObject(object);
                // 初めて復元に成功した個体を、最後の選択に使うため保持する。
                if (!first)
                    first = object;
            }
        }
        // 復元できた個体があれば、最初のものへ選択を移す。
        if (first) {
            SetSelectedObject(first);
            EditorManager::GetInstance()->SetSelectedObject(this);
        }
        // Prefab編集中の復元は、Sceneとは別のAssetの未保存状態を更新する。
        if (prefabEditMode_)
            prefabEditDirty_ = true;
    };
    transaction.redo = remove;
    remove();
    EditorTransactionManager::GetInstance()->Register(std::move(transaction));
    DebugConsole::GetInstance()->AddLog("Deleted Objects: " + std::to_string(states.size()));
}

void DebugEditor::ApplyEditorCommand(const EditorCommand& command, bool undo) {
    // 作成のUndoと削除のRedoは、GUID優先で対象を探して選択を解除する。
    const auto removeObject = [&](const nlohmann::json& state, const std::string& name) {
        auto* object = FindObjectByPersistentGuid(state.value("guid", std::string{}));
        // 保存されたGUIDだけで実体を見つけられなければ、互換用に名前を確認する。
        if (!object) {
            object = FindObjectByName(name);
        }
        // 対象がもう存在しなければ、現在の選択には触れない。
        if (!object) {
            return;
        }
        MarkDirtyForObject(object);
        RemoveObjectImmediate(object);
        ClearObjectSelection();
        EditorManager::GetInstance()->ClearSelection();
    };

    // 復元できたオブジェクトを選択し、所属するSceneを保存対象に戻す。
    const auto addObject = [&](const nlohmann::json& state) {
        // 復元が成功した場合だけ、選択と未保存状態を更新する。
        if (auto* object = AddObjectFromState(state)) {
            SetSelectedObject(object);
            EditorManager::GetInstance()->SetSelectedObject(this);
            MarkDirtyForObject(object);
        }
    };

    switch (command.type) {
    // 種類変更は属性を上書きするだけでは元の型に戻らないため、保存された型で実体を入れ替える。
    case EditorCommandType::kObjectReplaced: {
        const auto& state = undo ? command.beforeState : command.afterState;
        auto* object = FindObjectByPersistentGuid(state.value("guid", std::string{}));
        ReplaceObjectFromState(object, state);
        break;
    }
    // 作成前に戻すときは追加した実体を除去し、やり直しでは作成後の状態から再生成する。
    case EditorCommandType::kObjectCreated:
        // 作成のUndoでは、作成後に登録された実体を除去する。
        if (undo) {
            removeObject(command.afterState, command.afterName);
        // 作成のRedoでは、登録時の保存状態から再生成する。
        } else {
            addObject(command.afterState);
        }
        break;

    // 削除前に戻すときは消す直前の状態から再生成し、やり直しでは同じ実体を除去する。
    case EditorCommandType::kObjectDeleted:
        // 削除のUndoでは、消す直前の保存状態を復元する。
        if (undo) {
            addObject(command.beforeState);
        // 削除のRedoでは、保存状態を使って削除対象を探し直す。
        } else {
            removeObject(command.beforeState, command.beforeName);
        }
        break;

    // 通常の属性編集は既存の実体を保ち、操作の向きに合った JSON を適用する。
    case EditorCommandType::kObjectEdited:
    default: {
        const std::string& primaryName = undo ? command.afterName : command.beforeName;
        const std::string& fallbackName = undo ? command.beforeName : command.afterName;
        const nlohmann::json& targetState = undo ? command.beforeState : command.afterState;
        engine::Object3D* object = FindObjectByPersistentGuid(targetState.value("guid", std::string()));
        // GUIDで見つからない場合は、操作を適用する前の名前で探す。
        if (!object)
            object = FindObjectByName(primaryName);
        // 現在側の名前でも見つからない場合は、反対側の履歴名で探す。
        if (!object)
            object = FindObjectByName(fallbackName);
        // 同じ実体を特定できたときだけ属性を戻し、選択と保存対象を更新する。
        if (object) {
            ApplyObjectState(object, undo ? command.beforeState : command.afterState);
            SetSelectedObject(object);
            EditorManager::GetInstance()->SetSelectedObject(this);
            MarkDirtyForObject(object);
        }

        break;
    }
    }
}

void DebugEditor::PerformUndo() {
    // Transaction 側で適用に成功した場合だけ操作名をログへ残す。
    EditorTransactionManager* transactions = EditorTransactionManager::GetInstance();
    const std::string label = transactions->GetUndoLabel();

    // 戻す操作が可能で履歴の実行に成功した場合だけ、操作名を記録する。
    if (transactions->Undo()) {
        DebugConsole::GetInstance()->AddLog("Undo: " + label);
    }
}

void DebugEditor::PerformRedo() {
    // やり直した操作名を履歴マネージャーから取り、成功時に表示する。
    EditorTransactionManager* transactions = EditorTransactionManager::GetInstance();
    const std::string label = transactions->GetRedoLabel();

    // やり直す操作が可能で適用に成功した場合だけ、操作名を記録する。
    if (transactions->Redo()) {
        DebugConsole::GetInstance()->AddLog("Redo: " + label);
    }
}

