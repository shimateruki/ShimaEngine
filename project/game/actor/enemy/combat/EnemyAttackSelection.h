#pragma once

#include <algorithm>
#include <span>
#include <string_view>

// 敵固有の候補・状態は所有せず、攻撃の対応付けと選択手順だけを共有する。
namespace enemyAttackSelection {
// 敵内の技の列挙値と、EnemyAttackProfileの保存IDを対応させる一行。
template<class Kind>
struct Entry {
    Kind kind;
    const char* id;
};

template<class Kind>
const char* FindId(std::span<const Entry<Kind>> entries, Kind kind, const char* fallback) {
    for (const auto& entry : entries) {
        // 敵側の技種別と一致した登録から、保存・設定用のIDを返す。
        if (entry.kind == kind)
            return entry.id;
    }
    // 未登録の種別は、呼び出し側が指定したIDで扱う。
    return fallback;
}

template<class Kind>
Kind FindKind(std::span<const Entry<Kind>> entries, std::string_view id, Kind fallback) {
    for (const auto& entry : entries) {
        // 保存IDが一致した場合だけ、対応する敵内の技種別へ変換する。
        if (id == entry.id)
            return entry.kind;
    }
    // 未知のIDは、呼び出し側が選んだ種別へフォールバックする。
    return fallback;
}

// 呼び出し元が選んだ有効範囲だけを混ぜる。乱数の所有者と消費順は従来どおり。
template<class Kind, class Random>
void Shuffle(std::span<Kind> cycle, Kind previous, Random& random) {
    std::shuffle(cycle.begin(), cycle.end(), random);
    // 周期の境目で同じ技が続くときは、次の候補を先頭に繰り上げる。
    if (cycle.size() > 1 && cycle.front() == previous)
        std::swap(cycle[0], cycle[1]);
}

// cycleは空でなく、currentが有効な状態で呼ぶ。周期の再構築は敵側が担当する。
// 先頭の連続技を避け、最初に射程が合う未使用技を繰り上げる。全候補が外れても元の技を返す。
template<class Kind, class InRange>
Kind SelectInRange(std::span<Kind> cycle, std::size_t current, Kind previous, InRange&& inRange) {
    for (std::size_t i = current; i < cycle.size(); ++i) {
        // 新しい周期の先頭を埋めるときだけ、直前と同じ技を候補から外す。
        if (current == 0 && cycle[i] == previous)
            continue;
        // 射程など呼び出し側の条件を満たした最初の技を、現在位置へ移す。
        if (inRange(cycle[i])) {
            std::swap(cycle[current], cycle[i]);
            break;
        }
    }
    return cycle[current];
}
} // namespace enemyAttackSelection
