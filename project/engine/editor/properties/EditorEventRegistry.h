#pragma once
#include <cstdint>

#include "EditorPropertyRegistry.h"
#include "Object3D.h"

#include <algorithm>
#include <memory>
#include <set>
#include <utility>

/// 保存済みの番号と表示名、選択時だけ適用する初期設定をまとめます。
/// 固有の編集項目には、既存のProperty定義と保存パスをそのまま使えます。
// 関連：EditorEventRegistry（この型を使う機能の本体）
struct EditorEventDescriptor {
    std::int32_t value;
    std::string displayName;
    std::function<void(engine::Object3D&)> initialize;
    std::vector<EditorPropertyDescriptor> properties;
};

// 担当：ゲーム定義の接触イベントを既存のComponent・Property UIへ登録する。
// 関連：EditorEventDescriptor（定義）、EditorPropertyRegistry（登録先）、Object3D（設定対象）
// 主な入口：Register
class EditorEventRegistry {
public:
    static constexpr const char* kComponentType = "ContactEvent";
    static constexpr const char* kTypePath = "event.type";

    /// 起動時に一度、ゲーム側で定義した一覧を登録します。
    /// 番号は配列の添字ではなく保存値です。空の一覧や重複した定義は登録しません。
    static bool Register(std::vector<EditorEventDescriptor> events) {
        auto* registry = EditorPropertyRegistry::GetInstance();
        registry->InitializeBuiltInProperties();
        // ゲームからイベントを受け取れないか、接触イベントの型・選択欄が既に登録されている場合は重複登録しない。
        if (events.empty() || registry->FindComponent(kComponentType) || registry->Find(kTypePath))
            return false;

        // 登録途中で不正な定義が見つからないよう、全件を先に検証します。
        std::set<std::int32_t> values;
        std::set<std::string> paths{ kTypePath };
        for (const auto& event : events) {
            // 表示名が空か、保存番号が別のイベントと重複する定義を拒否する。
            if (event.displayName.empty() || !values.insert(event.value).second)
                return false;
            for (const auto& property : event.properties) {
                // 個別設定のパス、取得・設定処理が揃わない場合や、他の設定と同じパスを使う場合は登録を拒否する。
                if (property.path.empty() || !property.getter || !property.setter ||
                    registry->Find(property.path) || !paths.insert(property.path).second)
                    return false;
            }
        }

        EditorComponentDescriptor component;
        component.typeId = kComponentType;
        component.displayName = "接触イベント";
        component.description = "接触時に実行するイベントを選びます。";
        component.displayOrder = 85;
        component.required = true;
        component.applicable = [](const engine::Object3D&) { return true; };
        component.present = component.applicable;
        component.serializedPresent = [](const nlohmann::json&) { return true; };
        // 接触イベントの共通枠を登録できなければ、その枠へ選択欄を追加できない。
        if (!registry->RegisterComponent(std::move(component)))
            return false;

        // Setterが初期設定を保持します。表示と保存は既存のProperty機構へ任せます。
        auto choices = std::make_shared<const std::vector<EditorEventDescriptor>>(std::move(events));
        EditorPropertyDescriptor selector;
        selector.path = kTypePath;
        selector.displayName = "イベント種類";
        selector.category = "Gameplay";
        selector.type = EditorPropertyType::kInteger;
        selector.flags = EditorPropertyFlags::kMultiEdit | EditorPropertyFlags::kPrefabOverride;
        selector.componentTypeId = kComponentType;
        selector.serializedPath = "eventType";
        selector.defaultValue = 0;
        selector.getter = [](const engine::Object3D& object) {
            return nlohmann::json(static_cast<std::int32_t>(object.GetEventType()));
        };
        selector.setter = [choices](engine::Object3D& object, const nlohmann::json& value) {
            // 保存番号は整数型だけを受け付ける。
            if (!value.is_number_integer())
                return false;
            const auto selected = std::find_if(choices->begin(), choices->end(), [&](const auto& event) {
                return value == event.value;
            });
            // 未登録のイベント番号と、現在と同じ番号は変更として扱わない。
            if (selected == choices->end() || static_cast<std::int32_t>(object.GetEventType()) == selected->value)
                return false;

            // 種類が実際に変わったときだけ初期化を呼ぶ。同じ種類を入力し直して個別調整を作り直すことは避ける。
            object.SetEventType(static_cast<decltype(object.GetEventType())>(selected->value));
            // 選んだイベントに専用の初期化が登録されている場合だけ、型の切り替え後に適用する。
            if (selected->initialize)
                selected->initialize(object);
            return true;
        };
        for (const auto& event : *choices) {
            selector.enumLabels.push_back(event.displayName);
            selector.enumValues.push_back(event.value);
        }
        // 共通選択欄の登録に失敗したら、各イベント専用の入力欄を登録しない。
        if (!registry->Register(std::move(selector)))
            return false;

        // 入力欄の表示条件だけをイベント番号へ結び付け、保存時には旧値を消しません。
        for (const auto& event : *choices) {
            for (auto property : event.properties) {
                const auto visible = property.visible;
                property.componentTypeId = kComponentType;
                property.visible = [value = event.value, visible](const engine::Object3D& object) {
                    return static_cast<std::int32_t>(object.GetEventType()) == value && (!visible || visible(object));
                };
                // 個別入力欄を登録できない場合は、この後の登録を中止して失敗を返す。
                if (!registry->Register(std::move(property)))
                    return false;
            }
        }
        return true;
    }
};
