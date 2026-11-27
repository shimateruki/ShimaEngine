#pragma once
#include <cstdint>

// ゲーム固有の登録入口：RegisterGameEditorFactories。登録先の共通Editorに、個別の型名や分岐を持ち込まない。

#include "EditorObjectFactoryRegistry.h"
#include "RegisterGameAttackPreviews.h"
#include "RegisterGameCopyPreviews.h"
#include "RegisterGameEditorEvents.h"
#include "RegisterGamePortraitExports.h"
#include "RegisterGameAnimatorEditors.h"
#include "RegisterGameInspectorExtensions.h"
#include "game/editor/RegisterGameActorDebug.h"
#ifdef USE_IMGUI
#include "game/scene/gameplay/GamePlayScene.h"
#endif
#include "game/editor/CharacterStatusEditor.h"
#include "game/system/rules/GameplayStatusManager.h"
#include "../actor/enemy/spawn/EnemyFactory.h"
#include "game/system/scene/GimmickFactory.h"
#include "game/system/scene/ItemFactory.h"

namespace gameEditorFactoryDetail {
// 表示名と、用途ごとの公開範囲・順番はゲームが決める。未指定の新種は末尾へ自動追加する。
inline EditorObjectFactoryRegistry::TypeInfo MakeEnemyTypeInfo(const std::string& type) {
    using Registry = EditorObjectFactoryRegistry;
    const auto* preview = EnemyAttackPreviewRegistry::Find(type);
    Registry::TypeInfo info{type, preview ? preview->displayName : type,
        [type](const engine::Object3D& object) { return object.GetEnemyType() == type; }};
    // 敵のタイプID・表示名と、生成一覧・Spawner一覧での掲載順を対応させる。
    struct Presentation {
        const char* id;
        const char* label;
        std::int32_t creationOrder;
        std::int32_t spawnerOrder; // 負値は、その用途では選択候補へ出さない。
    };
    static constexpr Presentation kPresentations[] = {
        {"Slime", "ピンクスライム", 3, 0},
        {"Bomb", "ボム", 4, 1},
        {"Bomber", "ボムスライム", 5, 2},
        {"GiantSlime", "巨大スライム", 6, 3},
        {"PrismSlime", "クリスタルスライム", 7, 4},
        {"MagmaSlime", "マグマスライム", 8, 5},
        {"FireSlime", "炎スライム", 0, 6},
        {"ThunderSlime", "雷スライム", 1, 7},
        {"WindSlime", "風スライム", 2, 8},
        {"RingBurner", "リングバーナー", 9, -1},
        {"FalseKingSlime", "偽王スライム", -1, -1},
    };
    for (const auto& presentation : kPresentations) {
        // 別種別向けの表示設定は飛ばし、一致したときだけ用途別の表示可否と番号を適用する。
        if (type != presentation.id) continue;
        info.displayName = presentation.label;
        info.creation = {presentation.creationOrder >= 0, presentation.creationOrder};
        info.spawner = {presentation.spawnerOrder >= 0, presentation.spawnerOrder};
        break;
    }
    return info;
}
} // namespace gameEditorFactoryDetail

// 具体的な敵・ギミック・アイテムの生成方法はゲーム側で登録します。
inline void RegisterGameEditorFactories() {
    // 攻撃の確認・イベント・基本値など、型ごとに変わるエディター機能をゲーム側から登録する。
    RegisterGameAttackPreviews();
    RegisterGameCopyPreviews();
    RegisterGameEditorEvents();
    characterStatusEditor::Register();
#ifdef USE_IMGUI
    RegisterGamePortraitExports();
    RegisterGameAnimatorEditors();
    RegisterGameInspectorExtensions();
    RegisterGameActorDebug();
    GamePlayScene::RegisterDebugTeleportEditor();
#endif
    // 生成を行う場面では、エディターが直接クラスを知らなくても本番と同じファクトリーを呼べるようにする。
    using Registry = EditorObjectFactoryRegistry;
    Registry::Register(Registry::Kind::kEnemy, [](const std::string& type, engine::Object3DCommon* common) {
        return EnemyFactory::GetInstance()->CreateEnemy(type, common);
    });
    Registry::Register(Registry::Kind::kGimmick, [](const std::string& type, engine::Object3DCommon* common) -> std::unique_ptr<engine::Object3D> {
        auto* factory = GimmickFactory::GetInstance();
        // Editorから未登録IDを指定した場合は、誤配置を避けるため仮ギミックを生成しない。
        return factory->IsKnownType(type) ? factory->CreateGimmick(type, common) : nullptr;
    });
    // 敵の候補表示に必要な名前は、種類ごとのステータス設定を初期化してから取り出す。
    auto* statuses = GameplayStatusManager::GetInstance();
    statuses->Initialize();
    std::vector<Registry::TypeInfo> enemyTypes;
    for (const auto& type : statuses->GetEnemyTypes())
        enemyTypes.push_back(gameEditorFactoryDetail::MakeEnemyTypeInfo(type));
    Registry::RegisterTypes(Registry::Kind::kEnemy, std::move(enemyTypes));
    // ギミックの候補と型の判定には、ゲーム側ファクトリーへ登録された情報をそのまま使う。
    std::vector<Registry::TypeInfo> gimmickTypes;
    for (const auto& type : GimmickFactory::GetInstance()->GetRegisteredTypes())
        gimmickTypes.push_back({type.id, type.displayName, type.matches});
    Registry::RegisterTypes(Registry::Kind::kGimmick, std::move(gimmickTypes));
    Registry::Register(Registry::Kind::kItem, [](const std::string& type, engine::Object3DCommon* common) {
        return ItemFactory::GetInstance()->CreateItem(type, common);
    });
}
