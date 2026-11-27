#pragma once
#include <cstdint>
#ifdef USE_IMGUI
#include "DebugEditor.h"
#include "EditorEventRegistry.h"
#include "EditorPropertyJsonBinding.h"
#include "SceneManager.h"
#include "SceneSerializer.h"
#include "WinApp.h"
#include "CollisionConfig.h"
#include <fstream>
#include <set>
#include <stdexcept>

// 明示した専用起動だけで、実Objectと保存・復元の互換性を検証します。
namespace editorEventRuntimeProbe {
inline bool Enabled() { return GetEnvironmentVariableA("CG2_EDITOR_EVENT_TEST", nullptr, 0) > 0; }
inline void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
inline void Check(engine::Object3DCommon* common, BaseScene* scene, DebugEditor* editor, std::ofstream& log) {
    using json = nlohmann::json;
    auto* registry = EditorPropertyRegistry::GetInstance();
    const auto* property = registry->Find(EditorEventRegistry::kTypePath);
    Require(property && property->serializedPath == "eventType", "missing event property or changed save key");
    Require(property->enumLabels.size() == 8 && property->enumValues.size() == 8, "legacy event count");
    std::set<std::int32_t> ids;
    for (const auto& value : property->enumValues) ids.insert(value.get<std::int32_t>());
    Require(ids == std::set<std::int32_t>({0,1,2,3,4,5,6,7}), "legacy numeric identifiers changed");
    Require(property->enumLabels[5] == "ゴール", "game label mismatch");
    auto object = std::make_unique<engine::Object3D>();
    object->Initialize(common);
    auto defaults = object->ExportToJson();
    SceneSerializer legacySerializer;
    legacySerializer.Initialize(editor);
    for (std::int32_t id = 0; id < 8; ++id) {
        auto saved = defaults;
        saved["eventType"] = id;
        saved["collisionAttribute"] = 123u;
        saved["collisionMask"] = 456u;
        // 旧形式の数値Maskを検証します。現行形式ではID配列が数値より優先されます。
        saved.erase("collisionAttributeIds");
        saved.erase("collisionMaskIds");
        saved["collider"]["type"] = static_cast<std::int32_t>(engine::ColliderType::kNone);
        object->ImportFromJson(saved);
        Require(registry->GetValue(object.get(), property->path) == id, "legacy read");
        Require(object->ExportToJson()["eventType"] == id, "legacy write");
        // シーンはeventID、Object/PresetはeventTypeという既存の違いを保ちます。
        const auto targets = legacySerializer.BuildSingleObjectSaveTargets(object.get(), "__event_probe.json");
        Require(targets.size() == 1 && targets[0].data["objects"][0]["eventID"] == id,
            "legacy scene eventID save key");
        Require(object->GetCollisionAttribute() == 123u && object->GetCollisionMask() == 456u &&
            object->GetColliderType() == engine::ColliderType::kNone, "load reapplied editor defaults");
        auto clone = object->Clone();
        Require(clone && static_cast<std::int32_t>(clone->GetEventType()) == id, "clone event identifier");
        auto replay = object->CaptureReplayState();
        // イベント種別は配置設定のままにし、従来Replayが記録する衝突属性の復元を確認します。
        object->SetCollisionAttribute(999u);
        object->RestoreReplayState(replay);
        Require(static_cast<std::int32_t>(object->GetEventType()) == id && object->GetCollisionAttribute() == 123u,
            "Replay changed event or collision settings");
    }
    log << "OK all 8 legacy IDs: import, export, clone, Replay; loading does not apply editor defaults" << std::endl;

    object->ImportFromJson(defaults);
    engine::Object3D::ColliderConfig config;
    config.type = engine::ColliderType::kNone;
    object->SetColliderConfig(config);
    Require(registry->SetValue(object.get(), property->path, 5), "Goal selection rejected");
    Require(object->GetColliderType() == engine::ColliderType::kSphere && object->GetColliderConfig().size.x == 1.2f,
        "missing Goal sphere default");
    Require(object->GetCollisionAttribute() == engine::collisionCategories::Get("trigger") &&
        object->GetCollisionMask() == engine::collisionCategories::Get("player"), "Goal collision categories");
    object->SetCollisionMask(789u);
    Require(!registry->SetValue(object.get(), property->path, 5) && object->GetCollisionMask() == 789u,
        "reselection overwrote adjusted collision mask");
    object->SetEventType(EventType::kNone);
    config.type = engine::ColliderType::kAabb;
    config.size = {3.0f,4.0f,5.0f};
    config.center = {1.0f,2.0f,3.0f};
    object->SetColliderConfig(config);
    const auto collider = object->ExportToJson()["collider"];
    Require(registry->SetValue(object.get(), property->path, 5), "Goal selection with existing collider");
    Require(object->ExportToJson()["collider"] == collider, "existing collider overwritten");
    log << "OK Goal defaults only on selection; existing collider and same-event adjustments preserved" << std::endl;

    auto preset = defaults;
    preset["eventType"] = 73;
    preset["unknownTop"] = {{"keep",913}};
    preset["components"]["FutureComponent"] = {{"payload",914}};
    preset["children"] = json::array({{{"name","unchanged child"}}});
    const auto original = preset;
    editorPropertyJsonBinding::Restore(*object, defaults, preset);
    Require(preset == original && registry->GetValue(object.get(), property->path) == 73,
        "inspection changed unknown event or source JSON");
    Require(!registry->SetValue(object.get(), property->path, 73) &&
        !registry->SetValue(object.get(), property->path, 1.5) &&
        !registry->SetValue(object.get(), property->path, "5"), "invalid choice accepted");
    Require(object->ExportToJson()["eventType"] == 73, "unknown ID was clamped");
    Require(editorPropertyJsonBinding::Apply(*object, *property, preset, 5), "preset selection failed");
    Require(preset["eventType"] == 5 && preset["unknownTop"] == original["unknownTop"] &&
        preset["components"]["FutureComponent"] == original["components"]["FutureComponent"] &&
        preset["children"] == original["children"], "preset lost unrelated payload");
    Require(preset["collisionAttribute"] == engine::collisionCategories::Get("trigger") &&
        preset["collisionMask"] == engine::collisionCategories::Get("player"), "preset missed initializer side effects");
    auto second = std::make_unique<engine::Object3D>();
    second->Initialize(common);
    second->ImportFromJson(preset);
    Require(second->ExportToJson()["eventType"] == 5, "preset roundtrip");
    log << "OK unknown IDs, malformed choices, no-write inspection and preset JSON delta preservation" << std::endl;

    object->SetEventType(EventType::kNone);
    object->SetColliderConfig(config);
    object->SetCollisionAttribute(123u);
    object->SetCollisionMask(456u);
    const auto guid = object->GetPersistentGuid();
    auto* raw = scene->AddObjectImmediate(std::move(object));
    editor->SetSelectedObject(raw);
    const auto before = editor->CaptureObjectState(raw);
    const auto beforeSaved = raw->ExportToJson();
    Require(registry->SetValue(raw, property->path, 5), "editor selection");
    editor->RegisterObjectEdited(raw, before, "検証・接触イベント変更");
    const auto afterSaved = raw->ExportToJson();
    const auto find = [&]() -> engine::Object3D* {
        for (const auto& entry : scene->GetObjects()) if (entry->GetPersistentGuid() == guid) return entry.get();
        return nullptr;
    };
    editor->PerformUndo();
    raw = find();
    Require(raw && raw->ExportToJson()["eventType"] == beforeSaved["eventType"] &&
        raw->GetCollisionAttribute() == 123u && raw->GetCollisionMask() == 456u, "Undo did not restore event and side effects");
    editor->PerformRedo();
    raw = find();
    Require(raw && raw->ExportToJson()["eventType"] == afterSaved["eventType"] &&
        raw->GetCollisionAttribute() == engine::collisionCategories::Get("trigger"), "Redo event or side effects");
    SceneSerializer serializer;
    serializer.Initialize(editor);
    bool saved = false;
    for (const auto& target : serializer.BuildSingleObjectSaveTargets(raw, "stage1.json")) {
        if (!target.data.contains("objects")) continue;
        for (const auto& record : target.data["objects"])
            if (record.value("guid", "") == guid) saved = record["eventID"] == 5;
    }
    Require(saved, "SceneSerializer changed event save format");
    log << "OK Editor Undo/Redo including collision side effects and SceneSerializer numeric save key" << std::endl;
}
inline void AfterDraw(SceneManager* manager, DebugEditor* editor, const std::string& path) {
    static bool sDone = false;
    static std::ofstream log(path, std::ios::trunc);
    if (sDone || !manager || !editor || manager->IsTransitioning() || !manager->GetCurrentScene()) return;
    try {
        manager->SetIsPlaying(false);
        auto* scene = manager->GetCurrentScene();
        Check(scene->GetObject3DCommon(), scene, editor, log);
        log << "PASS: event registration, existing behavior, serialization and shared editor binding" << std::endl;
    } catch (const std::exception& error) {
        log << "FAIL: " << error.what() << std::endl;
    }
    sDone = true;
    WinApp::CloseNow();
}
}
#endif
