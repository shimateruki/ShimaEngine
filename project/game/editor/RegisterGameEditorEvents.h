#pragma once
#include <cstdint>

// ゲーム固有の登録入口：RegisterGameEditorEvents。登録先の共通Editorに、個別の型名や分岐を持ち込まない。

#include "EditorEventRegistry.h"
#include "CollisionConfig.h"
#include "game/actor/Event.h"

#include <cassert>

/// イベントの保存番号・表示名・選択時の設定をゲーム側で一度だけ登録します。
inline void RegisterGameEditorEvents() {
    // 旧JSONの0～7との対応を固定します。表示順を変えても保存番号は変わりません。
    static_assert(static_cast<std::int32_t>(EventType::kNone) == 0);
    static_assert(static_cast<std::int32_t>(EventType::kDamage) == 1);
    static_assert(static_cast<std::int32_t>(EventType::kWarp) == 2);
    static_assert(static_cast<std::int32_t>(EventType::kMovieBridge) == 3);
    static_assert(static_cast<std::int32_t>(EventType::kCheckpoint) == 4);
    static_assert(static_cast<std::int32_t>(EventType::kGoal) == 5);
    static_assert(static_cast<std::int32_t>(EventType::kStageSelect) == 6);
    static_assert(static_cast<std::int32_t>(EventType::kStarCoin) == 7);

    static const bool kRegistered = EditorEventRegistry::Register({
        { static_cast<std::int32_t>(EventType::kNone), "なし" },
        { static_cast<std::int32_t>(EventType::kDamage), "ダメージ" },
        { static_cast<std::int32_t>(EventType::kWarp), "ワープ" },
        { static_cast<std::int32_t>(EventType::kMovieBridge), "映像演出 (橋落ち)" },
        { static_cast<std::int32_t>(EventType::kCheckpoint), "中間地点 (Checkpoint)" },
        { static_cast<std::int32_t>(EventType::kGoal), "ゴール", [](engine::Object3D& object) {
            // 従来と同様、Goalへ変更した時だけ接触属性を設定します。
            object.SetCollisionAttribute(engine::collisionCategories::Get("trigger"));
            object.SetCollisionMask(engine::collisionCategories::Get("player"));
            // コライダーが未設定の場合だけ球を補い、既にある判定形状と寸法は保持する。
            if (object.GetColliderType() == engine::ColliderType::kNone) {
                engine::Object3D::ColliderConfig config;
                config.type = engine::ColliderType::kSphere;
                config.size = { 1.2f, 1.2f, 1.2f };
                object.SetColliderConfig(config);
                object.SetCollisionRadius(1.2f);
            }
        } },
        { static_cast<std::int32_t>(EventType::kStageSelect), "ステージセレクト" },
        { static_cast<std::int32_t>(EventType::kStarCoin), "スターコイン (StarCoin)" },
    });
    assert(kRegistered && "Event registration failed");
    (void)kRegistered;
}
