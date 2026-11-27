#pragma once
#include <cstdint>

#include "InputManager.h"
#include <array>
#include <cmath>

namespace engine {

// 画面ごとの割り当てを受け取る。入力の消費やリピート時計は保持しない。
// 関連：InputManager（入力状態）、各画面のメニュー操作
struct MenuButtonBinding {
    // キー・ボタンの0は未割当。stickDirectionにはXInputの十字方向を一つ指定する。
    std::array<BYTE, 3> keys{};
    std::array<WORD, 2> buttons{};
    WORD stickDirection = 0;

    // キーとボタンは押下直後、スティックはInputManager側のリピートを含む入力として読む。
    bool Triggered(const InputManager& input) const {
        for (BYTE key : keys)
            // 未割当の0を飛ばし、指定キーを押したフレームにtrueを返す。
            if (key && input.IsKeyTriggered(key)) return true;
        for (WORD button : buttons)
            // 未割当の0を飛ばし、指定パッドボタンを押したフレームにtrueを返す。
            if (button && input.IsGamepadButtonTriggered(button)) return true;
        // キーとボタンの入力がなければ、スティックの設定方向の初回・リピート入力を調べる。
        return stickDirection && input.IsMenuStickTriggered(stickDirection);
    }

    // 押し続けを毎フレーム判定する。Triggeredと異なり、反復待ち時間は挟まない。
    bool Held(const InputManager& input) const {
        for (BYTE key : keys)
            // 割り当てたキーのいずれかが現在押されていればtrueを返す。
            if (key && input.IsKeyPressed(key)) return true;
        for (WORD button : buttons)
            // 割り当てたパッドボタンのいずれかが現在押されていればtrueを返す。
            if (button && input.IsGamepadButtonPressed(button)) return true;
        const auto stick = input.GetLeftStick();
        // メニューの方向判定と同じ、主軸優先の閾値を使う。
        // 横と縦の倒し量が同じときは横を優先し、斜め入力で選択を二方向へ動かさない。
        switch (stickDirection) {
        // 左入力は横成分が-0.45以下かつ縦成分以上の大きさ。
        case XINPUT_GAMEPAD_DPAD_LEFT: return stick.x <= -.45f && std::abs(stick.x) >= std::abs(stick.y);
        // 右入力は横成分が0.45以上かつ縦成分以上の大きさ。
        case XINPUT_GAMEPAD_DPAD_RIGHT: return stick.x >= .45f && std::abs(stick.x) >= std::abs(stick.y);
        // 上入力は縦成分が0.45以上かつ横成分より大きい場合。
        case XINPUT_GAMEPAD_DPAD_UP: return stick.y >= .45f && std::abs(stick.y) > std::abs(stick.x);
        // 下入力は縦成分が-0.45以下かつ横成分より大きい場合。
        case XINPUT_GAMEPAD_DPAD_DOWN: return stick.y <= -.45f && std::abs(stick.y) > std::abs(stick.x);
        // スティック方向が割り当てられていなければ、押し続けている方向として扱わない。
        default: return false;
        }
    }
};

// メニューの方向・決定・取消・削除に使う入力割当。
// 関連：InputManager（入力状態）、各画面のメニュー操作
struct MenuInputBindings {
    MenuButtonBinding up, down, left, right, confirm, cancel, remove;
};

// このフレームの操作結果。複数画面から読んでも入力を消費しない。
// 関連：InputManager（入力状態）、各画面のメニュー操作
struct MenuInputFrame {
    bool up = false, down = false, left = false, right = false;
    bool confirm = false, cancel = false, remove = false, gamepad = false;
    // 軸の符号は右・下が正。左右または上下の同時入力は0に相殺する。
    std::int32_t Horizontal() const { return static_cast<std::int32_t>(right) - static_cast<std::int32_t>(left); }
    // 下を+1、上を-1とした方向を返す。同時入力は0。
    std::int32_t Vertical() const { return static_cast<std::int32_t>(down) - static_cast<std::int32_t>(up); }
};

// 割当からこのフレームのメニュー操作を読み取る。入力は消費しない。
inline MenuInputFrame ReadMenuInput(const InputManager& input, const MenuInputBindings& bindings) {
    return {bindings.up.Triggered(input), bindings.down.Triggered(input),
            bindings.left.Triggered(input), bindings.right.Triggered(input),
            bindings.confirm.Triggered(input), bindings.cancel.Triggered(input),
            bindings.remove.Triggered(input), input.IsGamepadMode()};
}

// 連続調整用の左右軸。音量などの反復速度は呼び出し側で管理する。
inline std::int32_t ReadMenuHorizontalHeld(const InputManager& input, const MenuInputBindings& bindings) {
    return static_cast<std::int32_t>(bindings.right.Held(input)) - static_cast<std::int32_t>(bindings.left.Held(input));
}

} // namespace engine
