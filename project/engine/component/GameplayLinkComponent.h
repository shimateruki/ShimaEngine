#pragma once
#include <cstdint>

#include "ObjectComponent.h"

// シーンイベントで参照される自分のIDと、自分が通知する相手のIDを保持する。
// 関連：ObjectComponent（共通型）、Object3D（保持・設定の窓口）
// 主な入口：GetEventId / SetEventId / GetTargetId / SetTargetId
class GameplayLinkComponent final : public ObjectComponent {
public:
    static constexpr std::string_view kTypeId = "GameplayLink"; // 保存と実行時の型判定に使う固定ID。

    /// <summary>保存と型判定に使うComponent種別を返す。</summary>
    std::string_view GetTypeId() const override { return kTypeId; }

    // 発火元と接続先のID。-1は未設定。
    std::int32_t GetEventId() const { return eventId_; }

    void SetEventId(std::int32_t id) { eventId_ = id; }

    std::int32_t GetTargetId() const { return targetId_; }

    void SetTargetId(std::int32_t id) { targetId_ = id; }

private:
    std::int32_t eventId_ = -1; // 発火元ID。-1は未設定。
    std::int32_t targetId_ = -1; // 接続先ID。-1は未設定。
};
