#include "Timer.h"

#include <algorithm>
#include <cmath>

namespace engine {
// 同じ時計に接続しており、所有者のTimerがまだ生存しているか判定する。
bool TimerView::IsValid() const { return !state_.expired(); }
// 未接続や所有者の破棄後は0秒。接続中は同じ時計の現在時刻を返す。
float TimerView::Elapsed() const {
    const auto state = state_.lock();
    return state ? state->elapsed : 0.0f;
}
// 所有者が生存して時計を進めている場合にtrueを返す。
bool TimerView::IsRunning() const {
    const auto state = state_.lock();
    return state && state->running;
}

// ビューと共有する時計の状態を一か所に作る。
Timer::Timer() : state_(std::make_shared<TimerView::State>()) {}
// 時刻を0秒へ戻し、計測を有効にする。
void Timer::Start() { state_->elapsed = 0.0f; state_->running = true; }
// 現在時刻を保持して加算を止める。
void Timer::Pause() { state_->running = false; }
// 現在時刻から加算を再開できるようにする。
void Timer::Resume() { state_->running = true; }
// 計測を無効にし、既存ビューから見える時刻も0秒へ戻す。
void Timer::Stop() { state_->running = false; state_->elapsed = 0.0f; }
void Timer::Update(float deltaSeconds) {
    // 再生中に受け取った有限な正の経過秒数だけを加える。加算後の時刻はSeek()でも検証する。
    if (state_->running && std::isfinite(deltaSeconds) && deltaSeconds > 0.0f) {
        Seek(state_->elapsed + deltaSeconds);
    }
}
void Timer::Seek(float seconds) {
    // 有限な時刻だけ受け付け、負の秒数は0へ丸める。再生・停止の状態は保つ。
    if (std::isfinite(seconds)) state_->elapsed = std::max(0.0f, seconds);
}
// 所有する時計の現在時刻を秒で返す。
float Timer::Elapsed() const { return state_->elapsed; }
// 時刻の加算を許可しているか返す。
bool Timer::IsRunning() const { return state_->running; }
// 所有する時計の値を読み取るビューを作る。
TimerView Timer::GetView() const {
    TimerView view;
    // 弱参照だけを渡すため、参照側が時計の寿命を延ばすことはありません。
    view.state_ = state_;
    return view;
}
} // namespace engine
