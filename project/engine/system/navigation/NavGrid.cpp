#include <cstdint>
#include "NavGrid.h"
#include <numbers>
#include "engine/system/collision/CollisionManager.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace {
// A* の未処理候補。index は一次元セル番号、score は到達コストと残距離推定の合計。
struct OpenNode {
    std::int32_t index = -1;
    float score = 0.0f;
};

// priority_queue の先頭を最小 score にする比較。通常の最大値優先を反転する。
struct OpenNodeGreater {
    bool operator()(const OpenNode& lhs, const OpenNode& rhs) const {
        return lhs.score > rhs.score;
    }
};

float PlanarDistanceSquared(const engine::Vector3& lhs, const engine::Vector3& rhs) {
    // 経路探索は地面のXZ平面で行い、高さ差を距離に含めない。
    const float dx = lhs.x - rhs.x;
    const float dz = lhs.z - rhs.z;
    return dx * dx + dz * dz;
}

// XZ成分だけを単位長にし、経路の前後が同じ方向か調べるために使う。
engine::Vector3 NormalizePlanar(const engine::Vector3& value) {
    const float length = std::sqrt(value.x * value.x + value.z * value.z);

    // 水平の差がほぼ無ければ向きを正規化できないため、ゼロを返す。
    if (length <= 0.000001f)
        return { 0.0f, 0.0f, 0.0f };
    return { value.x / length, 0.0f, value.z / length };
}
}

// 指定平面に探索用の格子を作り、障害物を移動半径で広げて通れないセルを登録します。
bool NavGrid::Build(const NavGridBuildSettings& sourceSettings, const engine::PhysicsQueryFilter& filter) {
    // 不正な値をここで正規化し、以後のセル数計算と障害物膨張に同じ設定を使います。
    settings_ = sourceSettings;
    settings_.cellSize = (std::max)(0.1f, settings_.cellSize);
    settings_.agentRadius = (std::max)(0.05f, settings_.agentRadius);
    settings_.agentHeight = (std::max)(settings_.agentRadius * 2.0f, settings_.agentHeight);
    settings_.maxCellsPerAxis = (std::clamp)(settings_.maxCellsPerAxis, 4, 512);

    // X方向の下端と上端が逆なら入れ替える。
    if (settings_.boundsMin.x > settings_.boundsMax.x)
        std::swap(settings_.boundsMin.x, settings_.boundsMax.x);
    // Z方向の下端と上端が逆なら入れ替える。
    if (settings_.boundsMin.z > settings_.boundsMax.z)
        std::swap(settings_.boundsMin.z, settings_.boundsMax.z);

    // 探索範囲をセル幅で割って切り上げる。両端の格子点を含めるため個数に1を足す。
    const float spanX = (std::max)(settings_.cellSize, settings_.boundsMax.x - settings_.boundsMin.x);
    const float spanZ = (std::max)(settings_.cellSize, settings_.boundsMax.z - settings_.boundsMin.z);
    width_ = (std::clamp)(static_cast<std::int32_t>(std::ceil(spanX / settings_.cellSize)) + 1, 1, settings_.maxCellsPerAxis);
    depth_ = (std::clamp)(static_cast<std::int32_t>(std::ceil(spanZ / settings_.cellSize)) + 1, 1, settings_.maxCellsPerAxis);
    blocked_.assign(static_cast<std::size_t>(width_ * depth_), 0);

    engine::CollisionManager* physics = engine::CollisionManager::GetInstance();
    const float blockingHeight = settings_.planeY + (std::max)(0.15f, settings_.agentRadius * 0.30f);

    // 範囲全体を一度だけPhysics Queryし、得られた障害物AABBをGridへラスタライズします。
    // セルごとのQueryを避けることで、再探索時のスパイクを抑えます。
    const engine::Aabb queryBounds = {
        { settings_.boundsMin.x, settings_.planeY - 0.05f, settings_.boundsMin.z },
        { settings_.boundsMax.x, settings_.planeY + settings_.agentHeight, settings_.boundsMax.z },
    };

    const auto obstacles = physics->OverlapAabb(queryBounds, filter);

    for (const engine::PhysicsOverlapHit& hit : obstacles) {
        // 対応する実体が無い結果からは、障害物の境界を得られない。
        if (!hit.object)
            continue;
        engine::Aabb obstacle = hit.object->GetAabb();

        // 足元より低い床面は障害物にせず、進行を遮る高さの形状だけを採用します。
        // 上端が足元付近までしかない床と、頭より上にある障害物は移動を遮らない。
        if (obstacle.max.y <= blockingHeight ||
            obstacle.min.y >= settings_.planeY + settings_.agentHeight) {
            continue;
        }

        // エージェント中心が通れる幅を確保するため、障害物側を半径分広げます。
        obstacle.min.x -= settings_.agentRadius;
        obstacle.min.z -= settings_.agentRadius;
        obstacle.max.x += settings_.agentRadius;
        obstacle.max.z += settings_.agentRadius;
        // 膨らませた障害物の最小側は切り下げ、最大側は切り上げて、端にかかるセルも塞ぐ。
        const std::int32_t minX = (std::clamp)(
            static_cast<std::int32_t>(std::floor((obstacle.min.x - settings_.boundsMin.x) / settings_.cellSize)),
            0, width_ - 1);
        const std::int32_t maxX = (std::clamp)(
            static_cast<std::int32_t>(std::ceil((obstacle.max.x - settings_.boundsMin.x) / settings_.cellSize)),
            0, width_ - 1);
        const std::int32_t minZ = (std::clamp)(
            static_cast<std::int32_t>(std::floor((obstacle.min.z - settings_.boundsMin.z) / settings_.cellSize)),
            0, depth_ - 1);
        const std::int32_t maxZ = (std::clamp)(
            static_cast<std::int32_t>(std::ceil((obstacle.max.z - settings_.boundsMin.z) / settings_.cellSize)),
            0, depth_ - 1);
        for (std::int32_t z = minZ; z <= maxZ; ++z) {
            for (std::int32_t x = minX; x <= maxX; ++x) {
                blocked_[static_cast<std::size_t>(ToIndex(x, z))] = 1u;
            }
        }
    }

    return IsBuilt();
}

bool NavGrid::IsBlocked(std::int32_t x, std::int32_t z) const {
    // 探索中に境界外へ踏み出せないよう、外側は障害物として扱います。
    if (x < 0 || z < 0 || x >= width_ || z >= depth_)
        return true;
    return blocked_[static_cast<std::size_t>(ToIndex(x, z))] != 0;
}

engine::Vector3 NavGrid::GetCellCenter(std::int32_t x, std::int32_t z) const {
    // セル番号をワールド座標へ戻し、Yには探索平面の高さを使う。
    return {
        settings_.boundsMin.x + static_cast<float>(x) * settings_.cellSize,
        settings_.planeY,
        settings_.boundsMin.z + static_cast<float>(z) * settings_.cellSize,
    };
}

bool NavGrid::WorldToCell(const engine::Vector3& position, std::int32_t& outX, std::int32_t& outZ) const {
    // 格子をまだ構築していなければ、入力座標をセルへ変換できない。
    if (!IsBuilt())
        return false;
    // グリッドの原点からの差をセル幅で割って最寄りの格子点へ丸め、配列の範囲に収める。
    outX = static_cast<std::int32_t>(std::round((position.x - settings_.boundsMin.x) / settings_.cellSize));
    outZ = static_cast<std::int32_t>(std::round((position.z - settings_.boundsMin.z) / settings_.cellSize));
    outX = (std::clamp)(outX, 0, width_ - 1);
    outZ = (std::clamp)(outZ, 0, depth_ - 1);
    return true;
}

// 指定セルから広げた正方形の外周を順に調べ、最初の通行可能セルを返します。
bool NavGrid::FindNearestWalkable(std::int32_t sourceX, std::int32_t sourceZ, std::int32_t& outX, std::int32_t& outZ) const {
    // 指定セルがもとから通れる場合は、周囲を探索せず使う。
    if (!IsBlocked(sourceX, sourceZ)) {
        outX = sourceX;
        outZ = sourceZ;
        return true;
    }

    // プレイヤーや目標が障害物に重なっていても、近い通行可能セルへ補正します。
    const std::int32_t maxRadius = (std::max)(width_, depth_);

    for (std::int32_t radius = 1; radius < maxRadius; ++radius) {
        for (std::int32_t z = sourceZ - radius; z <= sourceZ + radius; ++z) {
            for (std::int32_t x = sourceX - radius; x <= sourceX + radius; ++x) {
                // 内側のセルは前の半径で確認済みなので、新しく広げた正方形の外周だけを調べる。
                if (std::abs(x - sourceX) != radius && std::abs(z - sourceZ) != radius)
                    continue;
                // この外周で最初に見つかった通行可能セルを採用する。
                if (!IsBlocked(x, z)) {
                    outX = x;
                    outZ = z;
                    return true;
                }
            }
        }
    }

    return false;
}

bool NavGrid::FindPath(const engine::Vector3& start, const engine::Vector3& goal, std::vector<engine::Vector3>& outPath) const {
    // A*でセル列を求め、折れ点だけ残して移動用の経路へ変換する。
    outPath.clear();
    std::int32_t startX = 0;
    std::int32_t startZ = 0;
    std::int32_t goalX = 0;
    std::int32_t goalZ = 0;
    // 始点または終点をセルへ変換できなければ、探索を開始しない。
    if (!WorldToCell(start, startX, startZ) || !WorldToCell(goal, goalX, goalZ))
        return false;
    const bool exactGoalIsWalkable = !IsBlocked(goalX, goalZ);
    // 障害物内の始点・終点は通れるセルへ寄せ、どちらかを補正できなければ終了する。
    if (!FindNearestWalkable(startX, startZ, startX, startZ) ||
        !FindNearestWalkable(goalX, goalZ, goalX, goalZ)) return false;

    // 探索する各セルに到達コスト、手前のセル、展開済みかを保持する。
    const std::int32_t startIndex = ToIndex(startX, startZ);
    const std::int32_t goalIndex = ToIndex(goalX, goalZ);
    const std::int32_t cellCount = width_ * depth_;
    std::vector<float> cost(static_cast<std::size_t>(cellCount), (std::numeric_limits<float>::max)());
    std::vector<std::int32_t> parent(static_cast<std::size_t>(cellCount), -1);
    std::vector<std::uint8_t> closed(static_cast<std::size_t>(cellCount), 0);
    std::priority_queue<OpenNode, std::vector<OpenNode>, OpenNodeGreater> open;

    // 障害物がないと仮定し、斜めに進める分を√2、残りの直進を1として残りコストを見積もる。
    const auto heuristic = [goalX, goalZ](std::int32_t x, std::int32_t z) {
        const std::int32_t dx = std::abs(goalX - x);
        const std::int32_t dz = std::abs(goalZ - z);
        const std::int32_t diagonal = (std::min)(dx, dz);
        return static_cast<float>(dx + dz - diagonal * 2) + static_cast<float>(diagonal) * std::numbers::sqrt2_v<float>;
    };

    cost[static_cast<std::size_t>(startIndex)] = 0.0f;
    open.push({ startIndex, heuristic(startX, startZ) });
    const std::int32_t directions[8][2] = {
        { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
        { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 },
    };

    // 設定で斜めを許可すると8方向、直交だけなら先頭の4方向を使う。
    const std::int32_t directionCount = settings_.allowDiagonal ? 8 : 4;

    // 未展開セルがなくなるか、目標の最小コスト経路が確定するまで探索する。
    while (!open.empty()) {
        const std::int32_t index = open.top().index;
        open.pop();
        // 同じセルを別経路で登録済みでも、既に展開を終えたものは候補から捨てる。
        if (closed[static_cast<std::size_t>(index)] != 0)
            continue;
        closed[static_cast<std::size_t>(index)] = 1;
        // 確定したセルが目標なら、これ以上隣接セルを展開しない。
        if (index == goalIndex)
            break;

        const std::int32_t x = index % width_;
        const std::int32_t z = index / width_;

        for (std::int32_t directionIndex = 0; directionIndex < directionCount; ++directionIndex) {
            const std::int32_t dx = directions[directionIndex][0];
            const std::int32_t dz = directions[directionIndex][1];
            const std::int32_t nextX = x + dx;
            const std::int32_t nextZ = z + dz;
            // 塞がれたセルや格子の外側は移動先候補にしない。
            if (IsBlocked(nextX, nextZ))
                continue;

            // 斜め移動で障害物の角をすり抜けないよう、隣接する直交セルも確認します。
            if (dx != 0 && dz != 0 && (IsBlocked(x + dx, z) || IsBlocked(x, z + dz)))
                continue;

            const std::int32_t nextIndex = ToIndex(nextX, nextZ);
            // 到達コストの単位はセル幅。斜めの移動は直交移動より√2倍の距離を使う。
            const float stepCost = dx != 0 && dz != 0 ? std::numbers::sqrt2_v<float> : 1.0f;
            const float nextCost = cost[static_cast<std::size_t>(index)] + stepCost;
            // 既知の経路より安い場合だけ、到達コスト・親セル・未処理候補を更新する。
            if (nextCost >= cost[static_cast<std::size_t>(nextIndex)])
                continue;
            cost[static_cast<std::size_t>(nextIndex)] = nextCost;
            parent[static_cast<std::size_t>(nextIndex)] = index;
            open.push({ nextIndex, nextCost + heuristic(nextX, nextZ) });
        }
    }

    // 同じセルから始まる場合は親を持たなくても到達済み。その他は親がなければ到達できていない。
    if (startIndex != goalIndex && parent[static_cast<std::size_t>(goalIndex)] < 0)
        return false;
    // 目標から親セルを逆にたどり、後で移動順に反転する。
    for (std::int32_t index = goalIndex; index >= 0; index = parent[static_cast<std::size_t>(index)]) {
        outPath.push_back(GetCellCenter(index % width_, index / width_));
        // 探索の始点を記録したら、その親を探す必要はない。
        if (index == startIndex)
            break;
    }

    std::reverse(outPath.begin(), outPath.end());
    SimplifyPath(outPath);

    // 元の目標セルが通行可能で、経路の末尾が要求位置と十分離れる場合は、元のXZ位置も追加する。
    if (exactGoalIsWalkable &&
        (outPath.empty() || PlanarDistanceSquared(outPath.back(), goal) > 0.0001f)) {
        engine::Vector3 exactGoal = goal;
        exactGoal.y = settings_.planeY;
        outPath.push_back(exactGoal);
    }

    return true;
}

void NavGrid::SimplifyPath(std::vector<engine::Vector3>& path) const {
    // 点が3個未満の経路には、省略できる中間点がない。
    if (path.size() < 3)
        return;
    std::vector<engine::Vector3> simplified;
    simplified.reserve(path.size());
    simplified.push_back(path.front());

    for (std::size_t index = 1; index + 1 < path.size(); ++index) {
        const engine::Vector3 previousDirection = NormalizePlanar(path[index] - path[index - 1]);
        const engine::Vector3 nextDirection = NormalizePlanar(path[index + 1] - path[index]);
        // 前後の方向のどちらかの水平成分が許容差を超えて変わるなら、この折れ点を残す。
        if (std::abs(previousDirection.x - nextDirection.x) > 0.01f ||
            std::abs(previousDirection.z - nextDirection.z) > 0.01f) {
            simplified.push_back(path[index]);
        }
    }

    simplified.push_back(path.back());
    path = std::move(simplified);
}
