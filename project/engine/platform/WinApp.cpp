#include <cstdint>
#include "WinApp.h"
#include <Windows.h>
#ifdef USE_IMGUI
#include "imgui_impl_win32.h"
#endif
#include <DirectXCommon.h>
#include "json.hpp"
#include <fstream>


#pragma comment(lib, "winmm.lib")
#ifdef USE_IMGUI
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
#endif

// Windowsメッセージを受け取り、ImGui入力、終了要求、リサイズ、カーソル解除を処理する。
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
#ifdef USE_IMGUI
    // ImGuiが処理済みとしたメッセージは、アプリ側の処理にも渡さない。
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
        return true;
    }
#endif

    // ImGuiが消費しなかった通知だけを、アプリのウィンドウ操作として処理する。
    switch (msg) {
    // 閉じる操作を要求に変換し、未保存の確認をゲーム側に任せる。
    case WM_CLOSE:
        WinApp::RequestClose();
        return 0;

    // アクティブ状態が変わったときに、他のウィンドウへ操作を渡す準備をする。
    case WM_ACTIVATE:
        // このウィンドウが非アクティブになった場合だけ、カーソルの移動範囲の制限を解除する。
        if (LOWORD(wparam) == WA_INACTIVE) {
            ClipCursor(nullptr);
        }

        break;

    // ウィンドウの実サイズを更新し、必要なら描画側へ再作成を予約する。
    case WM_SIZE:
        // 最小化通知では、直前の有効な描画寸法を保つ。
        if (wparam != SIZE_MINIMIZED) {
            const int32_t width = LOWORD(lparam);
            const int32_t height = HIWORD(lparam);

            // 0以下の寸法は描画先に使えないため、現在値と描画先を変更しない。
            if (width <= 0 || height <= 0)
                return 0;
            WinApp::SetWindowSize(width, height);
            // DirectXのデバイスが作成済みなら、描画ループでのサイズ変更を予約する。
            if (engine::DirectXCommon::GetInstance()->GetDevice()) {
                engine::DirectXCommon::GetInstance()->RequestResize(width, height);
            }
        }

        return 0;

    case WM_DPICHANGED: {
        // モニター間の移動などでDPIが変わったら、ウィンドウの配置を新しいDPIへ合わせる。
        // 枠のない表示では、DPI変更後も最寄りのモニター全体を使う。
        if ((GetWindowLongPtr(hwnd, GWL_STYLE) & WS_OVERLAPPEDWINDOW) == 0) {
            MONITORINFO monitor{sizeof(MONITORINFO)};
            // 最寄りモニターの範囲を取得できた場合だけ、枠なし表示の矩形を更新する。
            if (GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
                SetWindowPos(hwnd, nullptr, monitor.rcMonitor.left, monitor.rcMonitor.top,
                    monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
        // 通常の枠付きウィンドウでは、Windowsが通知した推奨矩形を使う。
        } else {
            const RECT& rect = *reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }

    // ウィンドウが破棄されたら、メッセージループの終了を要求する。
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    // ここで処理しないメッセージは、Windowsの既定処理へ渡す。
    return DefWindowProc(hwnd, msg, wparam, lparam);
}

void WinApp::Initialize(const wchar_t* title, std::int32_t width, std::int32_t height) {
    // Win32ウィンドウを作成し、実際のクライアントサイズをエンジン側へ反映する。
    // ウィンドウ生成前に設定し、座標の仮想化による描画・クリックのずれを防ぎます。
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    hInstance_ = GetModuleHandle(nullptr);
    // Windowsのタイマー分解能を1ミリ秒単位で要求する。
    timeBeginPeriod(1);

    // ウィンドウクラスにメッセージ処理関数と初期カーソルを登録する。
    WNDCLASS wc{};
    wc.lpfnWndProc = WindowProc;
    wc.lpszClassName = L"CG2WindowClass";
    wc.hInstance = hInstance_;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClass(&wc);

    // 希望するクライアント寸法に、システムDPIでのタイトルバーと枠の分を加える。
    RECT wrc = { 0, 0, width, height };
    AdjustWindowRectExForDpi(&wrc, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForSystem());


    // 指定サイズでウィンドウを生成し、Windows が確定した実寸を後で取得する。
    hwnd_ = CreateWindow(
        wc.lpszClassName,
        title,
        WS_OVERLAPPEDWINDOW | WS_MAXIMIZE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wrc.right - wrc.left,
        wrc.bottom - wrc.top,
        nullptr, nullptr,
        hInstance_,
        nullptr);


    RECT clientRect{};
    GetClientRect(hwnd_, &clientRect);
    sWindowWidth = clientRect.right - clientRect.left;
    sWindowHeight = clientRect.bottom - clientRect.top;

    // まず最大化して表示し、その後に保存済みの解像度・枠なし設定があれば適用する。
    ShowWindow(hwnd_, SW_SHOWMAXIMIZED);

    // 保存済み表示設定のみ起動時に反映。破損した設定では従来の起動を維持します。
    try {
        std::ifstream file("Resources/json/user_config.json");
        nlohmann::json config;

        // 読み取ったJSONにdisplayがあれば、保存された解像度と枠なし設定を試す。
        if (file >> config; config.contains("display")) {
            const auto& d = config.at("display");
            SetDisplayMode(d.value("width", 1280), d.value("height", 720), d.value("borderless", false));
        }
    } catch (...) {}
    // 保存値の適用後にクライアント寸法を再取得し、実際の表示サイズを記録する。
    GetClientRect(hwnd_, &clientRect);
    sWindowWidth = clientRect.right;
    sWindowHeight = clientRect.bottom;
}

WinApp::DisplayState WinApp::CaptureDisplayState() const {
    // 表示切替前のウィンドウ様式と配置を対で保持し、元の表示へ戻せるようにする。
    DisplayState state;
    state.style = GetWindowLongPtr(hwnd_, GWL_STYLE);
    GetWindowPlacement(hwnd_, &state.placement);
    return state;
}

bool WinApp::SetDisplayMode(std::int32_t width, std::int32_t height, bool borderless) {
    // ウィンドウが未作成か、有効寸法960×540〜7680×4320の範囲外なら変更しない。
    if (!hwnd_ || width < 960 || height < 540 || width > 7680 || height > 4320)
        return false;
    MONITORINFO monitor{ sizeof(MONITORINFO) };
    // 最寄りモニターの矩形と作業領域を取得できなければ、表示モードを変更できない。
    if (!GetMonitorInfo(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor))
        return false;
    RECT rect{0, 0, width, height};
    const LONG style = borderless ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    // 枠なし表示ではモニター全体を覆う矩形とポップアップ様式を使う。
    if (borderless)
        rect = monitor.rcMonitor;
    // 枠付き表示では、指定されたクライアント寸法にウィンドウ枠を加える。
    else {
        // 解像度は描画領域の画素数で受け取るため、そのモニターのDPIでタイトルバーと枠の分を足す。
        AdjustWindowRectExForDpi(&rect, style, FALSE, 0, GetDpiForWindow(hwnd_));
        width = rect.right - rect.left;
        height = rect.bottom - rect.top;
        const auto& work = monitor.rcWork;
        // 枠を含む寸法が作業領域を超えた場合は、表示位置と様式を変えずに失敗を返す。
        if (width > work.right-work.left || height > work.bottom-work.top)
            return false;
        // 作業領域に収まる場合は、その中央にウィンドウを配置する。
        rect = {work.left + (work.right-work.left-width)/2, work.top + (work.bottom-work.top-height)/2,
                work.left + (work.right-work.left+width)/2, work.top + (work.bottom-work.top+height)/2};
    }

    // 最大化などの状態を解除してから様式と矩形を設定し、新しい枠をWindowsに再計算させる。
    ShowWindow(hwnd_, SW_RESTORE);
    SetWindowLongPtr(hwnd_, GWL_STYLE, style | WS_VISIBLE);
    // ウィンドウの位置と枠の更新要求が成功したかを返す。
    return SetWindowPos(hwnd_, nullptr, rect.left, rect.top, rect.right-rect.left, rect.bottom-rect.top,
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED) != FALSE;
}

void WinApp::RestoreDisplayState(const DisplayState& state) {
    // 保存した配置を戻した後、FRAMECHANGEDで非クライアント領域を更新する。
    SetWindowLongPtr(hwnd_, GWL_STYLE, state.style);
    SetWindowPlacement(hwnd_, &state.placement);
    SetWindowPos(hwnd_, nullptr, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
}

// Windowsメッセージを処理し、終了要求が来ていればtrueを返す。
bool WinApp::Update() {
    MSG msg{};

    // 保留中の入力通知をまとめて配送し、ImGuiの入力遅延を防ぐ。
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        // WM_QUITを受け取った場合は配信せず、終了を示すtrueを返す。
        if (msg.message == WM_QUIT) {
            return true;
        }

        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // カーソルを中央へ戻す設定が有効で、一時停止もされていない場合だけ固定を検討する。
    if (isCursorLocked_ && !cursorRecenteringSuppressed_) {
        // このウィンドウがアクティブな場合だけ位置を戻し、他のアプリの操作に干渉しない。
        if (GetActiveWindow() == hwnd_) {
            RECT clientRect;
            GetClientRect(hwnd_, &clientRect);

            // クライアント領域の中央をピクセル単位で求める。
            POINT center = {
                (clientRect.right - clientRect.left) / 2,
                (clientRect.bottom - clientRect.top) / 2
            };

            // 画面座標へ変換し、カーソルをウィンドウの中央へ戻す。
            ClientToScreen(hwnd_, &center);
            SetCursorPos(center.x, center.y);
        }
    }

    return false;
}

void WinApp::RequestClose() {
    // 即終了せず、ゲーム側で未保存確認などを挟めるよう終了要求だけを立てる。
    sCloseRequested = true;
}

bool WinApp::ConsumeCloseRequest() {
    // 未処理の終了要求がなければ、そのままfalseを返す。
    if (!sCloseRequested) {
        return false;
    }

    sCloseRequested = false;
    return true;
}

void WinApp::CloseNow() {
    // 確認なしでWM_QUITを発行し、アプリケーション終了へ進める。
    sCloseRequested = false;
    PostQuitMessage(0);
}

void WinApp::SetCursorVisibility(bool isVisible) {
    // ShowCursorの内部カウンタを調整し、カーソル表示状態を確実に切り替える。
    // 可視化が要求されたら、ShowCursorのカウンターを0以上へ進める。
    if (isVisible) {
        while (ShowCursor(TRUE) < 0);
    }
    // 非表示が要求されたら、ShowCursorのカウンターを負にする。
    else {
        while (ShowCursor(FALSE) >= 0);
    }
}

// カーソルをウィンドウのクライアント領域内へ制限するか解除する。


void WinApp::SetCursorClipping(bool isClipping) {
    isCursorClipping_ = isClipping;

    // カーソル制限が要求された場合は、クライアント領域に可動範囲を設定する。
    if (isClipping) {
        RECT rect;
        GetClientRect(hwnd_, &rect);

        // ClipCursor()に渡せる画面座標へ、可動範囲の左上と右下を変換する。
        POINT topLeft = { rect.left, rect.top };
        POINT bottomRight = { rect.right, rect.bottom };
        ClientToScreen(hwnd_, &topLeft);
        ClientToScreen(hwnd_, &bottomRight);

        rect.left = topLeft.x;
        rect.top = topLeft.y;
        rect.right = bottomRight.x;
        rect.bottom = bottomRight.y;

        ClipCursor(&rect);
    }
    // カーソル制限を解除する指定なら、Windowsへ制限範囲なしを渡す。
    else {
        ClipCursor(nullptr);
    }
}

void WinApp::SetCursorLocked(bool isLocked) {
    // 毎フレームカーソルを画面中央へ戻すロック状態を切り替える。
    isCursorLocked_ = isLocked;
}
