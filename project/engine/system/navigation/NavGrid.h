#pragma once

#include "engine/utility/math/Math.h"

#include <cstdint>
#include <vector>

namespace engine { struct PhysicsQueryFilter; }

/// <summary>物理障害物から 2D 経路探索グリッドを作るための設定です。</summary>
// 関連：NavGrid（この型を使う機能の本体）
struct NavGridBuildSettings {
    // XZ の探索領域と、移動面の高さ。Y の境界値はグリッド寸法に使いません。
    engine::Vector3 boundsMin = { -10.0f, 0.0f, -10.0f };
    engine::Vector3 boundsMax = { 10.0f, 0.0f, 10.0f };
    // 移動点のYに使う探索平面のワールド高度。
    float planeY = 0.0f;

    // 障害物は agentRadius だけ膨張させ、agentHeight の範囲で高さを判定します。
    // 隣り合う格子点のXZ間隔。ワールド単位。
    float cellSize = 1.0f;
    float agentRadius = 0.5f;
    float agentHeight = 1.5f;
    // trueでは斜め方向も探索する。両隣の直交セルを通る余地が必要。
    bool allowDiagonal = true;
    std::int32_t maxCellsPerAxis = 128; // 広い領域でのメモリ使用量を制限します。
};

// 担当：障害物をXZ平面のグリッドへ投影し、A*で経路を探索する。
// 関連：NavAgentComponent（利用）、NavGridBuildSettings（条件）、CollisionManager（照会）
// 主な入口：Build / FindPath / IsBlocked
class NavGrid {
public:
    /// <summary>指定領域の障害物を収集し、通行可能セルを構築します。</summary>
    /// <param name="settings">探索領域、セル寸法、エージェント寸法。</param>
    /// <param name="filter">障害物を収集する物理クエリのフィルター。</param>
    /// <returns>グリッドを構築できた場合は true。</returns>
    bool Build(const NavGridBuildSettings& settings, const engine::PhysicsQueryFilter& filter);

    /// <summary>通行可能な開始・目標セルを選び、曲がり角だけを残した経路を返します。</summary>
    /// <param name="start">開始位置。XZ のみをセル選択に使います。</param>
    /// <param name="goal">目標位置。通行不可なら近くの通行可能セルへ移します。</param>
    /// <param name="outPath">成功時の経路。失敗時は空になります。</param>
    /// <returns>経路が見つかった場合は true。</returns>
    /// <remarks>探索用のセルは格子端に収める。元の目標セルが通行可能なら、結果の末尾に元の目標XZを加える場合がある。</remarks>
    bool FindPath(const engine::Vector3& start, const engine::Vector3& goal, std::vector<engine::Vector3>& outPath) const;

    /// <summary>幅と奥行きのセル数が正で、通行判定の配列を持つ場合にtrueを返す。</summary>
    bool IsBuilt() const { return width_ > 0 && depth_ > 0 && !blocked_.empty(); }

    /// <summary>セルが障害物またはグリッド外か返します。</summary>
    bool IsBlocked(std::int32_t x, std::int32_t z) const;

    /// <summary>セルのワールド座標を移動面上で返します。</summary>
    engine::Vector3 GetCellCenter(std::int32_t x, std::int32_t z) const;

    /// <summary>グリッドの X 方向のセル数を返します。</summary>
    std::int32_t GetWidth() const { return width_; }

    /// <summary>グリッドの Z 方向のセル数を返します。</summary>
    std::int32_t GetDepth() const { return depth_; }

    /// <summary>構築時に正規化した設定を返します。</summary>
    const NavGridBuildSettings& GetSettings() const { return settings_; }

private:
    // X・Zのセル番号を、Z行優先の一次元インデックスへ変換する。
    std::int32_t ToIndex(std::int32_t x, std::int32_t z) const { return z * width_ + x; }

    // ワールド位置を最寄りのセルへ丸め、グリッド端に収めます。
    bool WorldToCell(const engine::Vector3& position, std::int32_t& outX, std::int32_t& outZ) const;

    // 指定セルが塞がっている場合、周囲を正方形状に広げて最初の空きセルを返します。
    // 同じ輪の中で距離順には並べないため、ユークリッド距離での最寄り点を保証しません。
    bool FindNearestWalkable(std::int32_t sourceX, std::int32_t sourceZ, std::int32_t& outX, std::int32_t& outZ) const;

    // 直線上の中間セルを取り除き、曲がり角を残します。
    void SimplifyPath(std::vector<engine::Vector3>& path) const;

    NavGridBuildSettings settings_;
    std::int32_t width_ = 0;
    std::int32_t depth_ = 0;
    std::vector<std::uint8_t> blocked_; // 0 は通行可能、1 は障害物。
};
