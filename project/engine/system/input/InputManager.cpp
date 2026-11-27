#include <cstdint>
#include "InputManager.h"
#include <cassert>
#include "math.h"
#include <algorithm>
#include <cmath>
#include"DebugConsole.h"

namespace {
// 円形デッドゾーンを除いたスティック入力を長さ0～1のベクトルへ変換します。
engine::Vector2 NormalizeStick(SHORT rawX, SHORT rawY, SHORT deadZone) {
    // 生の軸は符号付き16ビット値。32768で割って-1〜1の大きさへ変換する。
    const float x = static_cast<float>(rawX) / 32768.0f;
    const float y = static_cast<float>(rawY) / 32768.0f;
    const float length = std::hypot(x, y);
    const float threshold = static_cast<float>(deadZone) / 32768.0f;

    // 倒し量が円形デッドゾーン内なら、入力なしを返す。
    if (length <= threshold)
        return {0.0f, 0.0f};

    // 円形デッドゾーンを差し引き、倒した量が移動速度へ滑らかにつながるようにします。
    const float magnitude = std::clamp((length - threshold) / (1.0f - threshold), 0.0f, 1.0f);
    return {x / length * magnitude, y / length * magnitude};
}
}

engine::InputManager* engine::InputManager::GetInstance() {
    static engine::InputManager sInstance;
    return &sInstance;
}

void engine::InputManager::OpenFirstAvailableSdlController() {
    // 接続済みコントローラーを先頭から探し、メニュー入力に使うデバイスを開く。
    for (std::int32_t i = 0; i < SDL_NumJoysticks(); ++i) {
        // SDLがゲームコントローラーとして扱えない機器は候補から外す。
        if (!SDL_IsGameController(i))
            continue;
        sdlController_ = SDL_GameControllerOpen(i);
        // オープンできなければ、残りの接続機器から選ぶ。
        if (!sdlController_)
            continue;
        // 利用する機器が決まったら、加速度と角速度を有効にし、前の機器の基準値を消す。
        SDL_GameControllerSetSensorEnabled(sdlController_, SDL_SENSOR_ACCEL, SDL_TRUE);
        SDL_GameControllerSetSensorEnabled(sdlController_, SDL_SENSOR_GYRO, SDL_TRUE);
        baseAccel_ = {};
        isCalibrated_ = false;
        return;
    }
}

void engine::InputManager::UpdateMenuStickNavigation() {
    // スティックを方向入力へ変換し、押し始めと長押しで異なる間隔のリピートを発生させる。
    menuStickTriggered_.fill(false);
    const engine::Vector2 stick = GetLeftStick();
    std::int32_t direction = 0;

    // 正規化した軸の大きい方が0.45以上なら方向を選ぶ。同値の斜め入力は縦を優先する。
    if (std::max(std::abs(stick.x), std::abs(stick.y)) >= 0.45f) {
        if (std::abs(stick.y) >= std::abs(stick.x)) {
            direction = stick.y > 0 ? 1 : 2;
        } else {
            direction = stick.x < 0 ? 3 : 4;
        }
    }

    const uint64_t now = GetTickCount64();

    // 向きが変わったときは長押しの待ち時間を最初から計り直す。
    if (direction != menuStickDirection_) {
        menuStickDirection_ = direction;
        // 最初の長押し待ちは320ミリ秒とし、その後の反復を130ミリ秒間隔で進める。
        menuStickNextRepeatMs_ = now + 320;
        // しきい値を下回って無入力になった変化は通知せず、新たに倒した方向だけを即時通知する。
        if (direction)
            menuStickTriggered_[direction - 1] = true;
    // 同じ方向を維持して次の時刻に達したら、1回通知し、次を130ミリ秒後に予約する。
    } else if (direction && now >= menuStickNextRepeatMs_) {
        menuStickTriggered_[direction - 1] = true;
        menuStickNextRepeatMs_ = now + 130;
    }
}


// 対象ウィンドウにキーボードとマウスを登録し、XInputとSDLのコントローラーを準備します。
void engine::InputManager::Initialize(HWND hwnd) {
    HRESULT result;
    hwnd_ = hwnd;

    // キーボードとマウスのデバイスを作る前に、共通のDirectInputインターフェースを取得する。
    result = DirectInput8Create(GetModuleHandle(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8, reinterpret_cast<void**>(&directInput_), nullptr);
    assert(SUCCEEDED(result));

    // キーボードは対象ウィンドウが前面のときに、他のアプリと共有して使う。
    result = directInput_->CreateDevice(GUID_SysKeyboard, &keyboardDevice_, nullptr);
    assert(SUCCEEDED(result));
    result = keyboardDevice_->SetDataFormat(&c_dfDIKeyboard);
    assert(SUCCEEDED(result));
    result = keyboardDevice_->SetCooperativeLevel(hwnd_, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
    assert(SUCCEEDED(result));
    keyboardDevice_->Acquire();

    // マウスも前面・非排他で取得する。相対移動とボタンをフレームごとに読む。
    result = directInput_->CreateDevice(GUID_SysMouse, &mouseDevice_, nullptr);
    assert(SUCCEEDED(result));
    result = mouseDevice_->SetDataFormat(&c_dfDIMouse);
    assert(SUCCEEDED(result));
    result = mouseDevice_->SetCooperativeLevel(hwnd_, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
    assert(SUCCEEDED(result));
    mouseDevice_->Acquire();

    // 初期化直後も前回値を揃え、最初の更新で接続状態の差分を判定できるようにします。
    prevGamepadState_ = gamepadState_;

    // XInputの0番を最初の候補として、接続の有無と初期状態を取得する。
    ZeroMemory(&gamepadState_, sizeof(XINPUT_STATE));
    xinputConnected_ = XInputGetState(0, &gamepadState_) == ERROR_SUCCESS;


    // SDLへジョイスティック用スレッドとJoy-ConのHIDAPI処理を有効にする設定を渡す。
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, "1");

    // SDLのコントローラーまたはセンサーの初期化に失敗した場合は、アサートが有効な構成で止める。
    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_SENSOR) < 0) {
        assert(false && "SDL Init Failed");
    }

    // この処理がポーリングで読む入力は、SDL側の高頻度の通知キューへ積まない。
    SDL_EventState(SDL_CONTROLLERSENSORUPDATE, SDL_IGNORE);
    SDL_EventState(SDL_CONTROLLERAXISMOTION, SDL_IGNORE);
    SDL_EventState(SDL_JOYAXISMOTION, SDL_IGNORE);
    SDL_EventState(SDL_JOYBALLMOTION, SDL_IGNORE);
    SDL_EventState(SDL_JOYHATMOTION, SDL_IGNORE);
    SDL_EventState(SDL_JOYBUTTONDOWN, SDL_IGNORE);
    SDL_EventState(SDL_JOYBUTTONUP, SDL_IGNORE);

    // 高頻度の通知を止めても、軸やセンサーの最新値はUpdateで直接取得する。
    OpenFirstAvailableSdlController();

}


// キー、マウス、XInput、SDLの状態を前フレームと比較できる形へ揃える。
void engine::InputManager::Update() {
    // トリガー判定用の旧状態を保存してから、DirectInputの現在状態を取得する。
    memcpy(prevKeyState_, keyState_, sizeof(keyState_));
    prevMouseState_ = mouseState_;

    HRESULT result = keyboardDevice_->GetDeviceState(sizeof(keyState_), keyState_);

    // キーボードの状態を読めなければ取得し直し、現在値を再読込する。
    if (FAILED(result)) {
        keyboardDevice_->Unacquire();

        // 入力消失が続く間は、キーボードの取得を再試行する。
        while ((result = keyboardDevice_->Acquire()) == DIERR_INPUTLOST) {}
        keyboardDevice_->GetDeviceState(sizeof(keyState_), keyState_);
    }

    result = mouseDevice_->GetDeviceState(sizeof(mouseState_), &mouseState_);

    // マウスの状態を読めなければ、キーボードとは別に取得し直す。
    if (FAILED(result)) {
        mouseDevice_->Unacquire();

        // マウスも入力消失が続く間は取得を再試行する。
        while ((result = mouseDevice_->Acquire()) == DIERR_INPUTLOST) {}
        mouseDevice_->GetDeviceState(sizeof(mouseState_), &mouseState_);
    }

    // パッドはXInputを先に試し、未接続時だけSDLのボタン・軸を共通状態へ変換する。
    prevGamepadState_ = gamepadState_;

    ZeroMemory(&gamepadState_, sizeof(XINPUT_STATE));
    xinputConnected_ = XInputGetState(0, &gamepadState_) == ERROR_SUCCESS;

    // SDL側の接続解除通知と実際の接続状態の両方を確認する。
    SDL_Event event;

    // 切断通知を見落とさないよう、このフレームまでに届いたSDLイベントをすべて取り出す。
    while (SDL_PollEvent(&event)) {
        // 現在使っているSDL機器の切断通知だけを処理し、その機器のセンサー値も破棄する。
        if (event.type == SDL_CONTROLLERDEVICEREMOVED && sdlController_ &&
            SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(sdlController_)) == event.cdevice.which) {
            SDL_GameControllerClose(sdlController_);
            sdlController_ = nullptr;
            accelData_ = {};
            gyroData_ = {};
            isCalibrated_ = false;
        }
    }

    // 切断通知を受け取っていなくても、現在のSDL機器が接続済みでなければ閉じる。
    if (sdlController_ && !SDL_GameControllerGetAttached(sdlController_)) {
        SDL_GameControllerClose(sdlController_);
        sdlController_ = nullptr;
        accelData_ = {};
        gyroData_ = {};
        isCalibrated_ = false;
    }

    // SDL機器を保持していなければ、このフレームで開ける機器を探す。
    if (!sdlController_) {
        OpenFirstAvailableSdlController();
    }

    // 接続確認を終えてから軸・ボタン・センサーの最新状態を取得します。
    SDL_GameControllerUpdate();

    // SDL機器が開いている場合は、XInputの有無にかかわらずセンサーを確認する。
    if (sdlController_) {
        float data[3];
        bool hasData = false;

        // 加速度サンプルを正常に取れたときだけ現在値を更新し、基準候補にする。
        if (SDL_GameControllerGetSensorData(sdlController_, (SDL_SensorType)SDL_SENSOR_ACCEL, data, 3) == 0) {
            accelData_ = { data[0], data[1], data[2] };
            hasData = true;
        }

        // 角速度サンプルを正常に取れたときだけ角速度の現在値を更新する。
        if (SDL_GameControllerGetSensorData(sdlController_, (SDL_SensorType)SDL_SENSOR_GYRO, data, 3) == 0) {
            gyroData_ = { data[0], data[1], data[2] };
        }

        // まだ基準を取っておらず、今回の加速度を読み込めた場合だけ校正を試す。
        if (!isCalibrated_ && hasData) {
            // 全軸が0のサンプルは基準にせず、最初の非ゼロの取得値を採用する。
            if (accelData_.x != 0.0f || accelData_.y != 0.0f || accelData_.z != 0.0f) {
                baseAccel_ = accelData_;
                isCalibrated_ = true;
            }
        }

        // XInputの0番が未接続の場合だけ、SDLの操作入力を共通のXInput形式へ写す。
        if (!xinputConnected_) {
        // 左右のスティックを符号付き16ビットの軸値で取得する。
        int16_t leftX = SDL_GameControllerGetAxis(sdlController_, SDL_CONTROLLER_AXIS_LEFTX);
        int16_t leftY = SDL_GameControllerGetAxis(sdlController_, SDL_CONTROLLER_AXIS_LEFTY);
        int16_t rightX = SDL_GameControllerGetAxis(sdlController_, SDL_CONTROLLER_AXIS_RIGHTX);
        int16_t rightY = SDL_GameControllerGetAxis(sdlController_, SDL_CONTROLLER_AXIS_RIGHTY);

        // 左スティックの少なくとも一軸が非ゼロなら反映する。Yは正負を反転して16ビット範囲に収める。
        if (abs(leftX) > 0 || abs(leftY) > 0) {
            gamepadState_.Gamepad.sThumbLX = leftX;
            gamepadState_.Gamepad.sThumbLY = static_cast<SHORT>(std::clamp(-static_cast<std::int32_t>(leftY), -32768, 32767));
        }

        // 右スティックの少なくとも一軸が非ゼロなら反映する。Yの扱いは左と同じ。
        if (abs(rightX) > 0 || abs(rightY) > 0) {
            gamepadState_.Gamepad.sThumbRX = rightX;
            gamepadState_.Gamepad.sThumbRY = static_cast<SHORT>(std::clamp(-static_cast<std::int32_t>(rightY), -32768, 32767));
        }

        // トリガーはSDLの16bit範囲をXInputの8bit範囲へ縮小する。
        int16_t trigLeft = SDL_GameControllerGetAxis(sdlController_, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int16_t trigRight = SDL_GameControllerGetAxis(sdlController_, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        // 左トリガーが正の場合だけ、その押し込み量を8ビットの欄へ写す。
        if (trigLeft > 0)
            gamepadState_.Gamepad.bLeftTrigger = (BYTE)(trigLeft >> 7);
        // 右トリガーが正の場合だけ、その押し込み量を8ビットの欄へ写す。
        if (trigRight > 0)
            gamepadState_.Gamepad.bRightTrigger = (BYTE)(trigRight >> 7);


        // SDLのBが押されている場合は、共通入力のAとして扱う。
        if (SDL_GameControllerGetButton(sdlController_, SDL_CONTROLLER_BUTTON_B)) {
            gamepadState_.Gamepad.wButtons |= XINPUT_GAMEPAD_A;
        }

        // SDLのAが押されている場合は、共通入力のBとして扱う。
        if (SDL_GameControllerGetButton(sdlController_, SDL_CONTROLLER_BUTTON_A)) {
            gamepadState_.Gamepad.wButtons |= XINPUT_GAMEPAD_B;
        }

        // SDLのYが押されている場合は、共通入力のXとして扱う。
        if (SDL_GameControllerGetButton(sdlController_, SDL_CONTROLLER_BUTTON_Y)) {
            gamepadState_.Gamepad.wButtons |= XINPUT_GAMEPAD_X;
        }

        // SDLのXが押されている場合は、共通入力のYとして扱う。
        if (SDL_GameControllerGetButton(sdlController_, SDL_CONTROLLER_BUTTON_X)) {
            gamepadState_.Gamepad.wButtons |= XINPUT_GAMEPAD_Y;
        }

        // 十字キーは押されている方向を同じ方向として写す。
        // 上が押されている場合は、共通入力の上を有効にする。
        if (SDL_GameControllerGetButton(sdlController_, SDL_CONTROLLER_BUTTON_DPAD_UP)) {
            gamepadState_.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
        }

        // 下が押されている場合は、共通入力の下を有効にする。
        if (SDL_GameControllerGetButton(sdlController_, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) {
            gamepadState_.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
        }

        // 左が押されている場合は、共通入力の左を有効にする。
        if (SDL_GameControllerGetButton(sdlController_, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) {
            gamepadState_.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
        }

        // 右が押されている場合は、共通入力の右を有効にする。
        if (SDL_GameControllerGetButton(sdlController_, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) {
            gamepadState_.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
        }

        // Startが押されている場合は、メニュー系の共通Start入力へ渡す。
        if (SDL_GameControllerGetButton(sdlController_, SDL_CONTROLLER_BUTTON_START)) {
            gamepadState_.Gamepad.wButtons |= XINPUT_GAMEPAD_START;
        }

        // Backが押されている場合は、共通Back入力へ渡す。
        if (SDL_GameControllerGetButton(sdlController_, SDL_CONTROLLER_BUTTON_BACK)) {
            gamepadState_.Gamepad.wButtons |= XINPUT_GAMEPAD_BACK;
        }

        // 左ショルダーが押されている場合は、共通の左ショルダー入力へ渡す。
        if (SDL_GameControllerGetButton(sdlController_, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) {
            gamepadState_.Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        }

        // 左スティックが押し込まれている場合は、L3として渡す。
        if (SDL_GameControllerGetButton(sdlController_, SDL_CONTROLLER_BUTTON_LEFTSTICK)) {
            gamepadState_.Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
        }

        // 右スティックが押し込まれている場合は、R3として渡す。
        if (SDL_GameControllerGetButton(sdlController_, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) {
            gamepadState_.Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
        }

        // 右ショルダーが押されている場合は、共通の右ショルダー入力へ渡す。
        if (SDL_GameControllerGetButton(sdlController_, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) {
            gamepadState_.Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        }
        }
    }


    // Cキーが押されたフレームだけ、その時点の加速度を基準へ設定する。
    if (IsKeyTriggered(DIK_C)) {
        Calibrate();
        DebugConsole::GetInstance()->AddLog("InputManager: Calibrated accelerometer.");
    }


    // 最後に実際の操作があった入力系を判定し、画面の操作案内を切り替える。
    bool isKeyMouseActive = false;

    for (std::int32_t i = 0; i < 256; i++) {
        // どれか一つのキーが押されていれば、案内に使う入力系を判定できるため走査を終える。
        if (keyState_[i] & 0x80) {
            isKeyMouseActive = true;
            break;
        }
    }

    // マウスセンサーの微小な揺れでは案内表示を切り替えない。
    const long MOUSE_MOVE_THRESHOLD = 5;

    // 相対移動のX・Y・ホイールのどれかがしきい値を超えれば、マウス操作として扱う。
    if (abs(mouseState_.lX) > MOUSE_MOVE_THRESHOLD ||
        abs(mouseState_.lY) > MOUSE_MOVE_THRESHOLD ||
        abs(mouseState_.lZ) > MOUSE_MOVE_THRESHOLD) {
        isKeyMouseActive = true;
    }

    for (std::int32_t i = 0; i < 4; i++) {
        // どれか一つのマウスボタンが押されていれば、マウス操作があったとみなす。
        if (mouseState_.rgbButtons[i] & 0x80) {
            isKeyMouseActive = true;
            break;
        }
    }

    // パッド側はボタン・トリガー・大きなスティック入力を操作として扱う。
    bool isGamepadActive = false;

    // パッドのボタンビットが一つでも立っていれば、パッド操作とする。
    if (gamepadState_.Gamepad.wButtons != 0) {
        isGamepadActive = true;
    }

    // 操作案内を切り替える際は、各軸の生の値を10000と比較してわずかな揺れを無視する。
    const short DETECTION_DEADZONE = 10000;

    // 左スティックのいずれか一軸の絶対値が10000を超えれば、パッド操作とする。
    if (abs(gamepadState_.Gamepad.sThumbLX) > DETECTION_DEADZONE ||
        abs(gamepadState_.Gamepad.sThumbLY) > DETECTION_DEADZONE) {
        isGamepadActive = true;
    }

    // 右スティックも、いずれか一軸の絶対値が10000を超えればパッド操作とする。
    if (abs(gamepadState_.Gamepad.sThumbRX) > DETECTION_DEADZONE ||
        abs(gamepadState_.Gamepad.sThumbRY) > DETECTION_DEADZONE) {
        isGamepadActive = true;
    }

    // 左右どちらかのトリガーがXInputのしきい値を超えれば、パッド操作とする。
    if (gamepadState_.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ||
        gamepadState_.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) {
        isGamepadActive = true;
    }

    // 同時入力時はキーボード・マウスを優先し、案内表示がフレーム毎に揺れるのを防ぎます。
    if (isKeyMouseActive) {
        isGamepadMode_ = false;
    // キーボード・マウスの操作がなくパッド操作があった場合は、パッド案内へ切り替える。
    } else if (isGamepadActive) {
        isGamepadMode_ = true;
    // 操作がなく、XInputとSDLのどちらも未接続なら、案内をキーボード表示へ戻す。
    } else if (!xinputConnected_ && !sdlController_) {
        isGamepadMode_ = false;
    }

    // 案内用の入力判定が済んだら、現在の左スティックからメニューの反復入力を更新する。
    UpdateMenuStickNavigation();
}

// リプレイで押下・離上を再現するため、現在と直前の入力に加えて案内表示とセンサーの値を保存します。
engine::InputManager::ReplayState engine::InputManager::CaptureReplayState() const {
    ReplayState result;

    // リプレイのトリガー判定を再現するため、現在値と前回値を対で保存する。
    std::copy(std::begin(keyState_), std::end(keyState_), result.keys.begin());
    std::copy(std::begin(prevKeyState_), std::end(prevKeyState_), result.previousKeys.begin());
    std::copy(std::begin(mouseState_.rgbButtons), std::end(mouseState_.rgbButtons), result.mouseButtons.begin());
    // マウスは各ボタンに加え、移動とホイールの相対量も現在・前回の二組を保持する。
    std::copy(std::begin(prevMouseState_.rgbButtons), std::end(prevMouseState_.rgbButtons), result.previousMouseButtons.begin());
    result.mouseX = mouseState_.lX;
    result.mouseY = mouseState_.lY;
    result.mouseWheel = mouseState_.lZ;
    result.previousMouseX = prevMouseState_.lX;
    result.previousMouseY = prevMouseState_.lY;
    result.previousMouseWheel = prevMouseState_.lZ;

    // SDLもXInput形式へ統合済みなので、記録するパッド状態は一組でよい。
    const XINPUT_GAMEPAD& pad = gamepadState_.Gamepad;
    const XINPUT_GAMEPAD& previousPad = prevGamepadState_.Gamepad;
    result.gamepadButtons = pad.wButtons;
    result.previousGamepadButtons = previousPad.wButtons;
    result.leftTrigger = pad.bLeftTrigger;
    result.rightTrigger = pad.bRightTrigger;
    result.previousLeftTrigger = previousPad.bLeftTrigger;
    result.previousRightTrigger = previousPad.bRightTrigger;
    result.leftX = pad.sThumbLX;
    result.leftY = pad.sThumbLY;
    result.rightX = pad.sThumbRX;
    result.rightY = pad.sThumbRY;
    result.previousLeftX = previousPad.sThumbLX;
    result.previousLeftY = previousPad.sThumbLY;
    result.previousRightX = previousPad.sThumbRX;
    result.previousRightY = previousPad.sThumbRY;
    // 画面表示とモーション操作に必要なモードとセンサー値を記録する。
    result.gamepadMode = isGamepadMode_;
    result.accelerometer = accelData_;
    result.gyroscope = gyroData_;
    result.baseAccelerometer = baseAccel_;
    return result;
}

void engine::InputManager::ApplyReplayState(const ReplayState& state) {
    // リプレイの現在値と前回値を復元し、シーク直後でも入力の押下と離上を判定できるようにする。
    std::copy(state.keys.begin(), state.keys.end(), std::begin(keyState_));
    std::copy(state.previousKeys.begin(), state.previousKeys.end(), std::begin(prevKeyState_));

    mouseState_ = {};
    prevMouseState_ = {};
    std::copy(state.mouseButtons.begin(), state.mouseButtons.end(), std::begin(mouseState_.rgbButtons));
    std::copy(state.previousMouseButtons.begin(), state.previousMouseButtons.end(), std::begin(prevMouseState_.rgbButtons));
    mouseState_.lX = state.mouseX;
    mouseState_.lY = state.mouseY;
    mouseState_.lZ = state.mouseWheel;
    prevMouseState_.lX = state.previousMouseX;
    prevMouseState_.lY = state.previousMouseY;
    prevMouseState_.lZ = state.previousMouseWheel;

    // パッド状態も同じ形式へ戻してから、案内モードとセンサー基準を揃える。
    gamepadState_ = {};
    prevGamepadState_ = {};
    XINPUT_GAMEPAD& pad = gamepadState_.Gamepad;
    XINPUT_GAMEPAD& previousPad = prevGamepadState_.Gamepad;
    pad.wButtons = state.gamepadButtons;
    previousPad.wButtons = state.previousGamepadButtons;
    pad.bLeftTrigger = state.leftTrigger;
    pad.bRightTrigger = state.rightTrigger;
    previousPad.bLeftTrigger = state.previousLeftTrigger;
    previousPad.bRightTrigger = state.previousRightTrigger;
    pad.sThumbLX = state.leftX;
    pad.sThumbLY = state.leftY;
    pad.sThumbRX = state.rightX;
    pad.sThumbRY = state.rightY;
    previousPad.sThumbLX = state.previousLeftX;
    previousPad.sThumbLY = state.previousLeftY;
    previousPad.sThumbRX = state.previousRightX;
    previousPad.sThumbRY = state.previousRightY;
    isGamepadMode_ = state.gamepadMode;
    accelData_ = state.accelerometer;
    gyroData_ = state.gyroscope;
    baseAccel_ = state.baseAccelerometer;
}

// 指定されたキーが押されているか
bool engine::InputManager::IsKeyPressed(BYTE keyCode) const {
    // キーの状態の最上位ビットが1であれば、キーは押されている
    return (keyState_[keyCode] & 0x80) != 0;
}

// 指定されたキーがこのフレームで押された瞬間か (トリガー)
bool engine::InputManager::IsKeyTriggered(BYTE keyCode) const {
    // (現在押されている) かつ (前フレームでは押されていない) 場合にtrue
    return (keyState_[keyCode] & 0x80) && !(prevKeyState_[keyCode] & 0x80);
}

// 指定されたマウスボタンが押されているか
bool engine::InputManager::IsMouseButtonPressed(std::int32_t button) const {
    // マウスボタン配列にない番号なら、現在の状態を参照せずfalseを返す。
    if (button < 0 || button >= static_cast<std::int32_t>(sizeof(mouseState_.rgbButtons))) {
        return false;
    }

    // マウスボタンの状態の最上位ビットが1であれば、ボタンは押されている
    return (mouseState_.rgbButtons[button] & 0x80) != 0;
}

// 指定されたマウスボタンがこのフレームで押された瞬間か (トリガー)
bool engine::InputManager::IsMouseButtonTriggered(std::int32_t button) const {
    // 有効なボタン番号だけを、現在と前フレームで比較する。
    if (button < 0 || button >= static_cast<std::int32_t>(sizeof(mouseState_.rgbButtons))) {
        return false;
    }

    // (現在押されている) かつ (前フレームでは押されていない) 場合にtrue
    return (mouseState_.rgbButtons[button] & 0x80) && !(prevMouseState_.rgbButtons[button] & 0x80);
}

// マウスの移動量を取得
engine::Vector2 engine::InputManager::GetMouseMoveDelta() const {
    // DIMOUSESTATE構造体から相対移動量を取得する
    return { (float)mouseState_.lX, (float)mouseState_.lY };
}

// マウスの相対移動に、右スティックを経過秒数で換算した移動量を足して返します。
engine::Vector2 engine::InputManager::GetCameraLookDelta(float deltaTime) const {
    const engine::Vector2 mouse = GetMouseMoveDelta();
    const engine::Vector2 stick = GetRightStick();

    // マウスと右スティックの入力を合成する。反転設定はシーンからカメラへ渡す前に適用する。
    // 最大入力を1秒あたり360のマウス相当量に換算して足す。長いフレームは0.05秒で止めて視点の急変を抑える。
    const float step = 360.0f * std::clamp(deltaTime, 0.0f, 0.05f);
    // 上が正のスティックYを、下が正のマウス相対移動に合わせて逆符号で加える。
    return {mouse.x + stick.x * step, mouse.y - stick.y * step};
}

bool engine::InputManager::IsMenuStickTriggered(WORD dpadDirection) const {
    // 左スティックの反復判定結果を十字キー方向と同じ入口から返す。
    switch (dpadDirection) {
    case XINPUT_GAMEPAD_DPAD_UP: return menuStickTriggered_[0];

    case XINPUT_GAMEPAD_DPAD_DOWN: return menuStickTriggered_[1];

    case XINPUT_GAMEPAD_DPAD_LEFT: return menuStickTriggered_[2];

    case XINPUT_GAMEPAD_DPAD_RIGHT: return menuStickTriggered_[3];

    // 十字キーの4方向以外を指定した場合は、反復入力なしを返す。
    default: return false;
    }
}

// 左スティックの円形デッドゾーンを除き、上が正の入力を長さ0〜1で返す。
engine::Vector2 engine::InputManager::GetGamepadLeftStick() const {
    return NormalizeStick(gamepadState_.Gamepad.sThumbLX, gamepadState_.Gamepad.sThumbLY,
                          XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
}

// 右スティック専用の円形デッドゾーンを除き、視点操作用の方向と倒し量を返す。
engine::Vector2 engine::InputManager::GetGamepadRightStick() const {
    return NormalizeStick(gamepadState_.Gamepad.sThumbRX, gamepadState_.Gamepad.sThumbRY,
                          XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
}

// ゲームパッドのボタンが押されているか
bool engine::InputManager::IsGamepadButtonPressed(WORD button) const {
    // wButtonsはビットマスク。指定されたボタンのビットが立っているかを調べる
    return (gamepadState_.Gamepad.wButtons & button) != 0;
}

// マウスホイールのスクロール量を取得
float engine::InputManager::GetMouseWheelDelta() const {
    // 正負を持つDirectInputのホイール相対量を返す。
    return (float)mouseState_.lZ;
}

/// <summary>
/// カーソルをクライアント左上基準のピクセル位置で返す。
/// </summary>
engine::Vector2 engine::InputManager::GetMousePosition() const {

    POINT screenPos;

    // スクリーン上のカーソル位置を取得できなければ、座標を変換せず原点を返す。
    if (!GetCursorPos(&screenPos)) {
        return { 0.0f, 0.0f };
    }

    // 対象ウィンドウのクライアント座標へ変換できなければ、原点を返す。
    if (!ScreenToClient(hwnd_, &screenPos)) {
        return { 0.0f, 0.0f };
    }

    return { (float)screenPos.x, (float)screenPos.y };
}

/// <summary>
/// 指定されたマウスボタンがこのフレームで離された瞬間か (リリース)
/// </summary>
bool engine::InputManager::IsMouseButtonReleased(std::int32_t button) const {
    // 保持しているボタン配列の範囲外なら、離したボタンとして扱わない。
    if (button < 0 || button >= static_cast<std::int32_t>(sizeof(mouseState_.rgbButtons))) {
        return false;
    }

    // (現在 離されている) かつ (前フレームでは 押されていた) 場合にtrue
    return !(mouseState_.rgbButtons[button] & 0x80) && (prevMouseState_.rgbButtons[button] & 0x80);
}

// 低周波・高周波モーターの強さを0〜1で受け取り、指定の秒数だけ有効な振動要求を追加する。
void engine::InputManager::PlayRumble(float lowFrequency, float highFrequency, float duration) {
    // 持続秒数が0以下なら振動要求を登録しない。
    if (duration <= 0.0f) {
        return;
    }

    // 低周波・高周波の値はそれぞれ0〜1の強さ。要求の有効期間は秒で保持する。
    RumbleRequest request;
    request.lowFrequency = std::clamp(lowFrequency, 0.0f, 1.0f);
    request.highFrequency = std::clamp(highFrequency, 0.0f, 1.0f);
    request.remaining = duration;
    rumbleRequests_.push_back(request);

    // 同じ強さの要求でも継続時間が延びるため、SDL側の終了時刻を更新します。
    appliedLowFrequency_ = -1.0f;
    RefreshRumbleOutput();
}

void engine::InputManager::UpdateRumble(float unscaledDeltaTime) {
    // 一時停止中も振動の残り時間を進め、終了時に出力を止める。
    const float timeStep = std::max(unscaledDeltaTime, 0.0f);

    for (RumbleRequest& request : rumbleRequests_) {
        request.remaining -= timeStep;
    }

    // 残り時間を使い切った要求を除外してから、全体の振動出力を計算し直す。
    rumbleRequests_.erase(
        std::remove_if(rumbleRequests_.begin(), rumbleRequests_.end(), [](const RumbleRequest& request) {
            return request.remaining <= 0.0f;
        }),
        rumbleRequests_.end());
    RefreshRumbleOutput();
}

// 振動要求をすべて破棄し、出力を停止する状態へ更新します。
void engine::InputManager::StopRumble() {
    rumbleRequests_.clear();
    RefreshRumbleOutput();
}

void engine::InputManager::RefreshRumbleOutput() {
    // 同時に要求された振動は各モーターの最大強度を採り、最長の残り時間だけSDLへ送る。
    float lowFrequency = 0.0f;
    float highFrequency = 0.0f;
    float longestRemaining = 0.0f;

    for (const RumbleRequest& request : rumbleRequests_) {
        lowFrequency = std::max(lowFrequency, request.lowFrequency);
        highFrequency = std::max(highFrequency, request.highFrequency);
        longestRemaining = std::max(longestRemaining, request.remaining);
    }

    // 強度が前回と同じならデバイスへ再送しない。
    if (lowFrequency == appliedLowFrequency_ && highFrequency == appliedHighFrequency_) {
        return;
    }

    // 同じ強さを両入力バックエンドへ渡せるよう、正規化値を16ビットの出力範囲に変換する。
    XINPUT_VIBRATION vibration{};
    vibration.wLeftMotorSpeed = static_cast<WORD>(lowFrequency * 65535.0f);
    vibration.wRightMotorSpeed = static_cast<WORD>(highFrequency * 65535.0f);
    XInputSetState(0, &vibration);

    // SDLのコントローラーも開いている場合は、同じ強さと残り時間をSDLへ送る。
    if (sdlController_) {
        const Uint16 low = static_cast<Uint16>(lowFrequency * 65535.0f);
        const Uint16 high = static_cast<Uint16>(highFrequency * 65535.0f);
        // SDL側ではミリ秒が必要なので1000倍にし、一回の送信を60秒に制限する。
        const Uint32 durationMs = static_cast<Uint32>(std::clamp(longestRemaining * 1000.0f, 0.0f, 60000.0f));
        SDL_GameControllerRumble(sdlController_, low, high, durationMs);
    }

    appliedLowFrequency_ = lowFrequency;
    appliedHighFrequency_ = highFrequency;
}

void engine::InputManager::Finalize() {
    // 入力機器を閉じる前に振動を停止し、接続中の機器へ出力ゼロを送る。
    StopRumble();

    // SDLの機器が開いている場合はハンドルを閉じる。
    if (sdlController_) {
        SDL_GameControllerClose(sdlController_);
        sdlController_ = nullptr;
    }

    // キーボードのデバイスを作成済みなら、取得を解除してから解放する。
    if (keyboardDevice_) {
        keyboardDevice_->Unacquire();
        keyboardDevice_->Release();
        keyboardDevice_ = nullptr;
    }

    // マウスのデバイスを作成済みなら、取得を解除してから解放する。
    if (mouseDevice_) {
        mouseDevice_->Unacquire();
        mouseDevice_->Release();
        mouseDevice_ = nullptr;
    }

    // 個別デバイスの解放後、DirectInputを作成済みなら解放する。
    if (directInput_) {
        directInput_->Release();
        directInput_ = nullptr;
    }

    // 個別デバイスを解放した後でSDL本体を終了する。
    SDL_Quit();
}

// 右スティックのデッドゾーン適用後の入力を返します。
engine::Vector2 engine::InputManager::GetRightStick() const {
    return GetGamepadRightStick();
}

bool engine::InputManager::IsGamepadButtonTriggered(WORD button) const {
    // 「今は押されている」かつ「前は押されていなかった」なら true
    return (gamepadState_.Gamepad.wButtons & button) && !(prevGamepadState_.Gamepad.wButtons & button);
}

// 左スティックのデッドゾーン適用後の入力を返します。
engine::Vector2 engine::InputManager::GetLeftStick() const {
    return GetGamepadLeftStick();
}

// 現在押されている全キーのリストを取得する
std::vector<uint8_t> engine::InputManager::GetPressedKeys() const {
    std::vector<uint8_t> pressedKeys;

    // 0〜255の全キーコードを走査
    for (std::int32_t i = 0; i < 256; ++i) {
        // 押下ビットが立つキーだけを、キーコードが小さい順に追加する。
        if (keyState_[i] & 0x80) {
            pressedKeys.push_back(static_cast<uint8_t>(i));
        }
    }

    return pressedKeys;
}

// ボタン割り当て画面用に、現在押されている候補のうち優先順で一つだけ返す。
WORD engine::InputManager::GetPressedGamepadButton() const {
    // XInputのボタンはビットマスクなので、代表的なものを順番にチェックする
    const WORD buttons[] = {
        XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER,
        XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT,
        XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_BACK,
        XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB
    };

    // 同時押しでも戻り値は1個だけなので、配列の順で先に見つけたボタンを返す。
    for (WORD btn : buttons) {
        // 現在のボタンマスクと重なる最初の候補が見つかったら、そのボタンを返す。
        if (gamepadState_.Gamepad.wButtons & btn) {
            return btn;
        }
    }

    return 0; // 何も押されていない
}

// 現在押されているマウスボタンのうち最小番号を返す。未入力なら-1。
std::int32_t engine::InputManager::GetPressedMouseButton() const {
    for (std::int32_t i = 0; i < 4; ++i) {
        // 押下ビットが立つ最小番号のマウスボタンを返す。
        if (mouseState_.rgbButtons[i] & 0x80) {
            return i;
        }
    }

    return -1; // 何も押されていない
}

// 割り当てられたキー・マウス・パッドのいずれかを現在押していればtrueを返します。
bool engine::InputManager::IsActionPressed(const std::string& actionName) const {
    const BindData* data = KeyConfig::GetInstance()->GetBindData(actionName);
    // アクション名が登録されていなければ、参照できる割り当てがない。
    if (!data)
        return false;

    // キーが割り当て済みで現在押されている場合は成立する。
    if (data->keyCode != 0 && IsKeyPressed(static_cast<BYTE>(data->keyCode)))
        return true;

    // マウスボタンが割り当て済みで現在押されている場合は成立する。
    if (data->mouseButton != -1 && IsMouseButtonPressed(data->mouseButton))
        return true;

    // パッドボタンが割り当て済みで現在押されている場合は成立する。
    if (data->padCode != 0 && IsGamepadButtonPressed(data->padCode))
        return true;

    return false;
}

// アクションが「押された瞬間か」の判定
bool engine::InputManager::IsActionTriggered(const std::string& actionName) const {
    const BindData* data = KeyConfig::GetInstance()->GetBindData(actionName);
    // アクション名が未登録なら、このフレームで押されていないと判定する。
    if (!data)
        return false;

    // 割り当て済みのキーが、前フレームの非押下から現在の押下へ変わった場合は成立する。
    if (data->keyCode != 0 && IsKeyTriggered(static_cast<BYTE>(data->keyCode)))
        return true;

    // 割り当て済みのマウスボタンが、このフレームで押された場合は成立する。
    if (data->mouseButton != -1 && IsMouseButtonTriggered(data->mouseButton))
        return true;

    // 割り当て済みのパッドボタンが、このフレームで押された場合は成立する。
    if (data->padCode != 0 && IsGamepadButtonTriggered(data->padCode))
        return true;

    return false;
}

// 割り当て済みの入力のいずれかが、押下状態から離上状態へ変わったかを返します。
bool engine::InputManager::IsActionReleased(const std::string& actionName) const {
    const BindData* data = KeyConfig::GetInstance()->GetBindData(actionName);
    // アクション名が未登録なら、離したと判定できる割り当てがない。
    if (!data)
        return false;

    // 割り当て済みのキーが前フレームに押され、現在は離れている場合に成立する。
    if (data->keyCode != 0 && !(keyState_[data->keyCode] & 0x80) && (prevKeyState_[data->keyCode] & 0x80))
        return true;

    // 割り当て済みのマウスボタンがこのフレームで離された場合に成立する。
    if (data->mouseButton != -1 && IsMouseButtonReleased(data->mouseButton))
        return true;

    // 割り当て済みのパッドボタンのビットが、前フレームの1から現在の0へ変わった場合に成立する。
    if (data->padCode != 0 && !(gamepadState_.Gamepad.wButtons & data->padCode) && (prevGamepadState_.Gamepad.wButtons & data->padCode))
        return true;

    return false;
}
