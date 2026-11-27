#include "NavAgentComponent.h"

#include "Object3D.h"
#include "engine/system/collision/CollisionManager.h"

#include <algorithm>
#include <cmath>

namespace {
// 水平面で到着距離と目標の移動量を比較するため、XZ距離の二乗を返す。
float PlanarDistanceSquared(const engine::Vector3& lhs, const engine::Vector3& rhs) {
    const float dx = lhs.x - rhs.x;
    const float dz = lhs.z - rhs.z;
    return dx * dx + dz * dz;
}

// 水平方向を単位長へ変換する。Yは常に0。
engine::Vector3 NormalizePlanar(const engine::Vector3& value) {
    const float length = std::sqrt(value.x * value.x + value.z * value.z);

    // 水平成分がほぼ0なら、向きの代わりにゼロを返す。
    if (length <= 0.000001f)
        return { 0.0f, 0.0f, 0.0f };
    return { value.x / length, 0.0f, value.z / length };
}
}

void NavAgentComponent::SetCellSize(float value) {
    // 探索解像度が変わると現在のWaypointは使えないため、次回の探索を強制する。
    cellSize_ = (std::clamp)(value, 0.2f, 10.0f);
    InvalidatePath();
}

void NavAgentComponent::SetAgentRadius(float value) {
    // 半径を変更したら、通過に必要な高さも少なくとも直径分に揃えて再探索する。
    agentRadius_ = (std::clamp)(value, 0.05f, 10.0f);
    agentHeight_ = (std::max)(agentHeight_, agentRadius_ * 2.0f);
    InvalidatePath();
}

void NavAgentComponent::SetAgentHeight(float value) {
    // 半径の2倍以上を確保する。高さが変わると障害物の収集範囲も変わるため再探索する。
    agentHeight_ = (std::clamp)(value, agentRadius_ * 2.0f, 20.0f);
    InvalidatePath();
}

// 探索範囲の余白を1〜50ワールド単位に収め、既存経路を破棄する。
void NavAgentComponent::SetSearchPadding(float value) {
    searchPadding_ = (std::clamp)(value, 1.0f, 50.0f);
    InvalidatePath();
}

// 次に経路を作成したときの探索間隔を0.05〜5秒に制限する。
void NavAgentComponent::SetRepathInterval(float value) {
    repathInterval_ = (std::clamp)(value, 0.05f, 5.0f);
}

// 到着とみなす水平距離を0〜10ワールド単位に収める。
void NavAgentComponent::SetStoppingDistance(float value) {
    stoppingDistance_ = (std::clamp)(value, 0.0f, 10.0f);
}

// 経路の点列と再探索の比較情報を消し、次の更新で新規の探索を許可する。
void NavAgentComponent::InvalidatePath() {
    currentPath_.clear();
    waypointIndex_ = 0;
    repathTimer_ = 0.0f;
    // 目標の比較も無効にして、同じ位置を指定されても次回は経路を新しく作る。
    hasLastTarget_ = false;
    lastPathSucceeded_ = false;
}

// 経路に沿う次の水平単位方向を返す。到着時はゼロ、経路がなければ目標への直接方向。
engine::Vector3 NavAgentComponent::CalculateDirection(
    engine::Object3D* owner, const engine::Vector3& currentPosition,
    const engine::Vector3& targetPosition, float deltaTime) {
    engine::Vector3 direct = targetPosition - currentPosition;
    direct.y = 0.0f;
    // 経路補正を無効にした場合や所有オブジェクトがない場合は、格子を使わず目標へ向かう。
    if (!enabled_ || !owner)
        return NormalizePlanar(direct);

    // 停止距離はワールド単位なので二乗して比較する。上下差だけでは移動を続けない。
    if (PlanarDistanceSquared(currentPosition, targetPosition) <= stoppingDistance_ * stoppingDistance_) {
        return { 0.0f, 0.0f, 0.0f };
    }

    // 経路を利用して追跡する段階で、正の経過秒だけ再探索までの残り時間を減らす。
    repathTimer_ -= (std::max)(0.0f, deltaTime);

    // 前回の目標を保持していないか、水平にセル幅の半分を超えて動いた場合は早めに更新する。
    const bool targetMoved = !hasLastTarget_ ||
        PlanarDistanceSquared(lastTargetPosition_, targetPosition) > cellSize_ * cellSize_ * 0.25f;
    // 経路が空、目標位置が大きく変わった、再探索時刻に達した、のいずれかで探索し直す。
    if (currentPath_.empty() || targetMoved || repathTimer_ <= 0.0f) {
        RebuildPath(owner, currentPosition, targetPosition);
    }

    const float reachDistance = (std::max)(stoppingDistance_, cellSize_ * 0.35f);
    // 到達済みの経由点を飛ばし、次の点へ向く。停止距離だけだと細かいセルで詰まりやすい。
    while (waypointIndex_ < currentPath_.size() &&
        PlanarDistanceSquared(currentPosition, currentPath_[waypointIndex_]) <= reachDistance * reachDistance) {
        ++waypointIndex_;
    }

    // 経路が作れない場合も追跡を止めず、目標への直接方向を返す。
    if (waypointIndex_ >= currentPath_.size())
        return NormalizePlanar(direct);
    return NormalizePlanar(currentPath_[waypointIndex_] - currentPosition);
}

// 現在地と目標を含む局所の経路を再構築し、障害物を避ける経路を取得できたか返す。
bool NavAgentComponent::RebuildPath(
    engine::Object3D* owner, const engine::Vector3& currentPosition, const engine::Vector3& targetPosition) {
    // 全マップではなく現在地と目標を囲う領域だけを探索し、グリッドの生成量を抑える。
    const float padding = (std::max)(searchPadding_, cellSize_ * 2.0f);
    NavGridBuildSettings settings;
    settings.boundsMin = {
        (std::min)(currentPosition.x, targetPosition.x) - padding,
        currentPosition.y,
        (std::min)(currentPosition.z, targetPosition.z) - padding,
    };

    settings.boundsMax = {
        (std::max)(currentPosition.x, targetPosition.x) + padding,
        currentPosition.y,
        (std::max)(currentPosition.z, targetPosition.z) + padding,
    };

    // 出発地点の高さを探索面とし、エージェントの身長分にかかる障害物を拾う。
    settings.planeY = currentPosition.y;
    settings.cellSize = cellSize_;
    settings.agentRadius = agentRadius_;
    settings.agentHeight = agentHeight_;
    settings.allowDiagonal = allowDiagonal_;
    settings.maxCellsPerAxis = 64;

    engine::PhysicsQueryFilter filter;
    filter.mask = obstacleMask_;

    // 自分自身と子オブジェクトを障害物として取り込まない。
    filter.ignoredObject = owner;
    filter.ignoreDescendants = true;
    filter.includeTriggers = false;

    // 新しい経路の取得前に以前の点列と追跡番号を消す。格子を構築できた場合だけ経路を検索する。
    currentPath_.clear();
    waypointIndex_ = 0;
    lastPathSucceeded_ = grid_.Build(settings, filter) &&
        grid_.FindPath(currentPosition, targetPosition, currentPath_);
    // 成功・失敗に関わらず、今回の探索時刻と入力位置を次回の比較に残す。
    repathTimer_ = repathInterval_;
    lastTargetPosition_ = targetPosition;
    hasLastTarget_ = true;
    return lastPathSucceeded_;
}
