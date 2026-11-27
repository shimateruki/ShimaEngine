#pragma once

// メニュー画面別の決定・取消入力と案内画像を対応させる。選択中の行やページ遷移は各画面が保持する。

#include "engine/system/input/MenuInput.h"

namespace menuControls {

enum class Screen { kTitle, kSaveSelect, kSettings, kPause, kGuide };

// 操作の割り当てと、その画面が使う既存の案内画像を一緒に定義する。
// 案内文字列はSprite識別名または画像パス。表示位置と切り出しは各画面が設定する。
struct Hint {
    const char* keyboard = "";
    const char* gamepad = "";
    // 現在の操作機器に対応する案内名を返す。
    const char* Resolve(bool useGamepad) const { return useGamepad ? gamepad : keyboard; }
};

// 一画面の決定・取消などのキーと案内。選択中の項目は各メニューが保持する。
struct Scheme {
    engine::MenuInputBindings bindings;
    Hint confirmHint, closeHint, footerHint;
};

// タイトルとセーブ選択は矢印・Spaceを使い、他画面のWASD・Enter割当とは分ける。
inline Scheme Make(Screen screen) {
    // タイトルとセーブ選択はWASDやEnterを追加しない。共通の方向キーとパッドから基準の割り当てを作る。
    const bool title = screen == Screen::kTitle || screen == Screen::kSaveSelect;
    Scheme scheme;
    scheme.bindings.up = {{{DIK_UP, static_cast<BYTE>(title ? 0 : DIK_W)}}, {{XINPUT_GAMEPAD_DPAD_UP}}, XINPUT_GAMEPAD_DPAD_UP};
    scheme.bindings.down = {{{DIK_DOWN, static_cast<BYTE>(title ? 0 : DIK_S)}}, {{XINPUT_GAMEPAD_DPAD_DOWN}}, XINPUT_GAMEPAD_DPAD_DOWN};
    scheme.bindings.left = {{{DIK_LEFT, static_cast<BYTE>(title ? 0 : DIK_A)}}, {{XINPUT_GAMEPAD_DPAD_LEFT}}, XINPUT_GAMEPAD_DPAD_LEFT};
    scheme.bindings.right = {{{DIK_RIGHT, static_cast<BYTE>(title ? 0 : DIK_D)}}, {{XINPUT_GAMEPAD_DPAD_RIGHT}}, XINPUT_GAMEPAD_DPAD_RIGHT};
    scheme.bindings.confirm = {{{DIK_SPACE, static_cast<BYTE>(title ? 0 : DIK_RETURN)}}, {{XINPUT_GAMEPAD_A}}};
    switch (screen) {
    case Screen::kTitle:
        // 決定の案内はタイトルの既存メニュー表示を使う。
        scheme.confirmHint = {"mainMenuPrompt", "mainMenuPrompt"};
        break;
    case Screen::kSaveSelect:
        // 閉じる入力に加えて削除を割り当て、デバイスに応じたフッターを選べるようにする。
        scheme.bindings.cancel = {{{DIK_ESCAPE}}, {{XINPUT_GAMEPAD_B}}};
        scheme.bindings.remove = {{{DIK_D}}, {{XINPUT_GAMEPAD_X}}};
        scheme.footerHint = {"ui/menu_hints/save_keyboard.png", "ui/menu_hints/save_gamepad.png"};
        break;
    case Screen::kSettings:
        // 設定画面から戻る入力と、決定・閉じる表示の部品名を登録する。
        scheme.bindings.cancel = {{{DIK_ESCAPE, DIK_TAB}}, {{XINPUT_GAMEPAD_B}}};
        scheme.confirmHint = {"settings_footer_confirm_text", "settings_footer_confirm_text"};
        scheme.closeHint = {"settings_footer_close_text", "settings_footer_close_text"};
        break;
    case Screen::kPause:
    case Screen::kGuide:
        // ポーズとガイドは表示素材を共有するが、閉じるパッドキーは画面ごとに切り替える。
        scheme.bindings.cancel = {{{DIK_TAB}}, {{XINPUT_GAMEPAD_B,
            static_cast<WORD>(screen == Screen::kPause ? XINPUT_GAMEPAD_START : XINPUT_GAMEPAD_BACK)}}};
        scheme.confirmHint = {"generated/text/pause_guide/footer_confirm_keyboard.png", "generated/text/pause_guide/footer_confirm_gamepad.png"};
        scheme.closeHint = {"generated/text/pause_guide/footer_close_keyboard.png", "generated/text/pause_guide/footer_close_gamepad.png"};
        break;
    }
    return scheme;
}

// 列挙した5画面に対応する固定表を返す。有効なScreen列挙値を渡す。
inline const Scheme& Get(Screen screen) {
    static const std::array<Scheme, 5> kSchemes = {Make(Screen::kTitle), Make(Screen::kSaveSelect),
        Make(Screen::kSettings), Make(Screen::kPause), Make(Screen::kGuide)};
    return kSchemes[static_cast<size_t>(screen)];
}

// 指定画面の割り当てで入力を読み、このフレームで成立した方向・決定・取消などを返す。
inline engine::MenuInputFrame Read(Screen screen, const engine::InputManager& input) {
    return engine::ReadMenuInput(input, Get(screen).bindings);
}

// タイトル案内の先頭部分の幅と、後ろの文字列を切り出すX座標。
// 値は既存タイトル画像内のピクセル位置。表示先のウィンドウ座標ではない。
struct TitlePromptCrop { float firstWidth, suffixOrigin; };
// キーボードとパッドで異なる案内文の切り出し寸法を返す。
inline TitlePromptCrop TitlePrompt(bool gamepad) {
    return gamepad ? TitlePromptCrop{65.0f, 365.0f} : TitlePromptCrop{289.0f, 517.0f};
}

} // namespace menuControls
