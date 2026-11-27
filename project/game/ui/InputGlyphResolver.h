#pragma once
#include <cstdint>

// 画面が指定した操作に対応する固定キー・パッド画像の切り抜き情報を返す。Spriteの所有や配置は行わない。

#include "KeyConfig.h"

#include <Xinput.h>
#include <cstdio>
#include <string>

// 固定の操作画像を、固定の操作表と1枚の透過アトラスの切り抜きへ変換する。
struct InputGlyphSpec {
    std::string path;
    bool useCrop = false;
    // 切抜き原点と幅・高さは元画像のピクセル単位。描画先の大きさとは分けて扱う。
    float cropX = 0.0f;
    float cropY = 0.0f;
    float width = 96.0f;
    float height = 64.0f;
};

// 現在の操作割り当てを比較する署名を返す。案内画像を再作成するタイミングの判断に使う。
inline std::string GetInputGlyphBindingSignature() {
    // 表示に使う固定操作表の署名。初期化前後の切替も検出する。
    std::string signature;
    for (const char* action : {"Forward", "Backward", "Left", "Right", "Jump", "Ability",
                               "PrimaryAttack", "SecondaryAction", "LockOn", "MorphRelease"}) {
        const BindData* binding = KeyConfig::GetInstance()->GetBindData(action);
        // 登録された操作だけをキー・マウス・パッドの順で署名に加える。
        if (binding) signature += std::to_string(binding->keyCode) + ":" +
                                  std::to_string(binding->mouseButton) + ":" +
                                  std::to_string(binding->padCode) + ";";
    }
    return signature;
}

// DIKコード0〜255を16列のアトラスへ対応させ、範囲外は未割当用の0へ戻す。
inline InputGlyphSpec MakeKeyboardGlyph(std::int32_t keyCode) {
    const std::int32_t code = keyCode >= 0 && keyCode <= 255 ? keyCode : 0;
    return {"ui/input/keyboard/key_scan_atlas.png", true,
            static_cast<float>((code % 16) * 96), static_cast<float>((code / 16) * 64), 96.0f, 64.0f};
}

// アクションが未登録、または対応画像がない場合は未割当キーの切抜きへ戻す。
inline InputGlyphSpec ResolveActionGlyph(const std::string& action, bool gamepadMode) {
    const BindData* binding = KeyConfig::GetInstance()->GetBindData(action);
    // 操作が未登録なら、対応する絵柄を選べないため未割当のセルを返す。
    if (!binding) {
        return MakeKeyboardGlyph(0);
    }
    // パッド表示中はボタンの割り当てを個別画像に変換する。
    if (gamepadMode) {
        const char* path = nullptr;
        // パッドのボタン名に対応する既存画像を選ぶ。画像がなければ後続で未割当に戻す。
        switch (binding->padCode) {
        case XINPUT_GAMEPAD_A: path = "ui/input/controller/pad_a.png";
        break;
        case XINPUT_GAMEPAD_B: path = "ui/input/controller/pad_b.png";
        break;
        case XINPUT_GAMEPAD_X: path = "ui/input/controller/pad_x.png";
        break;
        case XINPUT_GAMEPAD_Y: path = "ui/input/controller/pad_y.png";
        break;
        case XINPUT_GAMEPAD_LEFT_SHOULDER: path = "ui/input/controller/pad_lb.png";
        break;
        case XINPUT_GAMEPAD_RIGHT_SHOULDER: path = "ui/input/controller/pad_rb.png";
        break;
        case XINPUT_GAMEPAD_DPAD_UP: path = "ui/input/controller/pad_dpad_up.png";
        break;
        case XINPUT_GAMEPAD_DPAD_DOWN: path = "ui/input/controller/pad_dpad_down.png";
        break;
        case XINPUT_GAMEPAD_DPAD_LEFT: path = "ui/input/controller/pad_dpad_left.png";
        break;
        case XINPUT_GAMEPAD_DPAD_RIGHT: path = "ui/input/controller/pad_dpad_right.png";
        break;
        case XINPUT_GAMEPAD_LEFT_THUMB: path = "ui/input/controller/pad_l3.png";
        break;
        case XINPUT_GAMEPAD_RIGHT_THUMB: path = "ui/input/controller/pad_r3.png";
        break;
        default: break;
        }
        return path ? InputGlyphSpec{path} : MakeKeyboardGlyph(0);
    }
    // マウスボタンが割り当てられている場合は、キーボードのキーより優先して表示する。
    if (binding->mouseButton >= 0) {
        switch (binding->mouseButton) {
        case 0: return {"ui/input/mouse/mouse_left.png"};
        case 1: return {"ui/input/mouse/mouse_right.png"};
        case 2: return {"ui/input/mouse/mouse_middle.png"};
        case 3: return {"ui/input/mouse/mouse_x1.png"};
        default: break;
        }

        // 対応する拡張ボタン画像がなければ、未割当を示すアトラスのセルを返す。
        return MakeKeyboardGlyph(0);
    }
    // マウス割り当てがない場合は、キーコードのアトラスを返す。
    return MakeKeyboardGlyph(binding->keyCode);
}

// 配置時の案内画像を、現在の操作機器と割り当てに対応する画像へ解決する。
inline InputGlyphSpec ResolveInputGlyph(const std::string& authoredPath, bool gamepadMode) {
    // 保存済みのキーボード画像を操作名へ変換し、現在の入力機器に対応する絵柄を選ぶ。
    const char* action = nullptr;
    // 既存のSpace画像はジャンプ操作を表す。
    if (authoredPath == "ui/input/keyboard/key_space.png")
        action = "Jump";
    // 既存のE画像はコピー能力の操作を表す。
    else if (authoredPath == "ui/input/keyboard/key_e.png") action = "Ability";
    // 既存のQ画像はロックオン操作を表す。
    else if (authoredPath == "ui/input/keyboard/key_q.png") action = "LockOn";
    // 既存のR画像は変身解除操作を表す。
    else if (authoredPath == "ui/input/keyboard/key_r.png") action = "MorphRelease";
    // 左クリック画像は主攻撃を表す。
    else if (authoredPath == "ui/input/mouse/mouse_left.png") action = "PrimaryAttack";
    // 右クリック画像は副操作を表す。
    else if (authoredPath == "ui/input/mouse/mouse_right.png") action = "SecondaryAction";
    // WASDの移動案内は、操作機器に応じた移動用の画像へ切り替える。
    else if (authoredPath == "ui/input/combo/combo_wasd.png") {
        // ゲームパッドでは移動用の左スティックを表示する。
        if (gamepadMode)
            return {"ui/input/controller/pad_l_stick.png"};
        return {"ui/input/combo/combo_wasd.png"};
    }
    // 既知の操作画像は、操作名から現在の割り当てを調べ直す。
    if (action)
        return ResolveActionGlyph(action, gamepadMode);
    // マウス移動の案内はゲームパッドでは視点用の右スティックへ置き換える。
    if (gamepadMode && authoredPath == "ui/input/mouse/mouse_move.png")
        return {"ui/input/controller/pad_r_stick.png"};
    return {authoredPath};
}
