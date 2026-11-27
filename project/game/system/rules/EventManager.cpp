#include "game/system/rules/EventManager.h"
#include <utility>

EventManager* EventManager::GetInstance() {
    static EventManager sInstance;
    return &sInstance;
}

template <typename Event>
// 空のCallbackは登録せず、解除に使う一意のIDを返します。
EventManager::SubscriptionId EventManager::SubscribeTo(
    ListenerList<Event>& listeners, std::function<void(const Event&)> callback) {
    // 呼び出せる関数がない要求は登録せず、無効な解除IDを返す。
    if (!callback)
        return kInvalidSubscriptionId;
    const SubscriptionId id = nextSubscriptionId_++;
    listeners.push_back({id, std::move(callback), true});
    return id;
}

template <typename Event>
// 配信開始時点の購読だけへ通知し、配信中の解除は最外層の終了まで遅延します。
void EventManager::DispatchTo(ListenerList<Event>& listeners, const Event& event) {
    // 配信中に追加された購読は次のイベントから通知します。
    const SubscriptionId lastExistingId = nextSubscriptionId_ - 1;
    // コールバックから同じ配信処理に入る場合に備えて、現在の入れ子の深さを数えます。
    ++dispatchDepth_;
    try {
        for (auto& listener : listeners) {
            // 途中で解除された購読と、この配信が始まった後に登録された購読を飛ばします。
            if (listener.active && listener.id <= lastExistingId)
                listener.callback(event);
        }
    // 例外で途中終了しても深さを戻し、最外層であれば解除済みの購読を消してから例外を伝えます。
    } catch (...) {
        --dispatchDepth_;
        // 例外で抜ける配信が一番外側なら、走査中には消せなかった購読をここで削除する。
        if (dispatchDepth_ == 0)
            PruneInactiveListeners();
        throw;
    }

    // 正常終了でも、配信中の走査が全て終わった時点で登録リストを整理します。
    --dispatchDepth_;
    // 正常終了した配信が一番外側なら、無効化した購読を削除できる。
    if (dispatchDepth_ == 0)
        PruneInactiveListeners();
}

template <typename Event>
// 配信中でもイテレータを無効にしないよう、対象を削除せず非アクティブにします。
void EventManager::MarkInactive(ListenerList<Event>& listeners, SubscriptionId id) {
    for (auto& listener : listeners) {
        // 一致した解除IDだけを非アクティブにし、それ以外の購読を残す。
        if (listener.id == id) {
            listener.active = false;
            return;
        }
    }
}

template <typename Event>
void EventManager::PruneInactive(ListenerList<Event>& listeners) {
    listeners.remove_if([](const Listener<Event>& listener) { return !listener.active; });
}

// すべてのイベント種別から無効な購読を除去します。配信の最外層が終わってから呼びます。
void EventManager::PruneInactiveListeners() {
    PruneInactive(playerHitListeners_);
    PruneInactive(bulletHitListeners_);
    PruneInactive(damageListeners_);
    PruneInactive(playerDeathListeners_);
    PruneInactive(playerJumpListeners_);
}

EventManager::SubscriptionId EventManager::Subscribe(std::function<void(const PlayerHitEvent&)> callback) {
    return SubscribeTo(playerHitListeners_, std::move(callback));
}

void EventManager::Dispatch(const PlayerHitEvent& event) {
    DispatchTo(playerHitListeners_, event);
}

EventManager::SubscriptionId EventManager::Subscribe(std::function<void(const BulletHitEvent&)> callback) {
    return SubscribeTo(bulletHitListeners_, std::move(callback));
}

void EventManager::Dispatch(const BulletHitEvent& event) {
    DispatchTo(bulletHitListeners_, event);
}

EventManager::SubscriptionId EventManager::Subscribe(std::function<void(const DamageEvent&)> callback) {
    return SubscribeTo(damageListeners_, std::move(callback));
}

void EventManager::Dispatch(const DamageEvent& event) {
    DispatchTo(damageListeners_, event);
}

EventManager::SubscriptionId EventManager::Subscribe(std::function<void(const PlayerDeathEvent&)> callback) {
    return SubscribeTo(playerDeathListeners_, std::move(callback));
}

void EventManager::Dispatch(const PlayerDeathEvent& event) {
    DispatchTo(playerDeathListeners_, event);
}

EventManager::SubscriptionId EventManager::Subscribe(std::function<void(const PlayerJumpEvent&)> callback) {
    return SubscribeTo(playerJumpListeners_, std::move(callback));
}

void EventManager::Dispatch(const PlayerJumpEvent& event) {
    DispatchTo(playerJumpListeners_, event);
}

// 指定IDを全種別から無効化し、配信中でなければ直ちに整理します。
void EventManager::Unsubscribe(SubscriptionId id) {
    // 無効IDはどのイベント種別にも登録されていないため、探索を省く。
    if (id == kInvalidSubscriptionId)
        return;
    MarkInactive(playerHitListeners_, id);
    MarkInactive(bulletHitListeners_, id);
    MarkInactive(damageListeners_, id);
    MarkInactive(playerDeathListeners_, id);
    MarkInactive(playerJumpListeners_, id);
    // 配信が進行中でない場合だけ、解除した購読をその場でリストから消す。
    if (dispatchDepth_ == 0)
        PruneInactiveListeners();
}

// 全種別の購読を無効化し、進行中の配信があれば実削除を後回しにします。
void EventManager::ClearAllListeners() {
    for (auto& listener : playerHitListeners_)
        listener.active = false;

    for (auto& listener : bulletHitListeners_)
        listener.active = false;

    for (auto& listener : damageListeners_)
        listener.active = false;

    for (auto& listener : playerDeathListeners_)
        listener.active = false;

    for (auto& listener : playerJumpListeners_)
        listener.active = false;
    // 配信の途中なら無効化だけを行い、現在の走査が全て終わるまで物理削除を保留する。
    if (dispatchDepth_ == 0)
        PruneInactiveListeners();
}
