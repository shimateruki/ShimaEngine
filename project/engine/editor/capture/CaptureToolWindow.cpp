#define NOMINMAX
#include <cstdint>
#include "CaptureToolWindow.h"

#include "DebugEditor.h"
#include "DirectXCommon.h"
#include "IconsFontAwesome5.h"
#include "InputManager.h"
#include "WinApp.h"
#include "imgui.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <cmath>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace fs = std::filesystem;

namespace {

// 画像保存へ渡す CPU 側 BGRA 画素列。width と height はピクセル数。
// 関連：CaptureToolWindowの内部処理だけで使う補助型。
struct CapturedImage {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::vector<unsigned char> bgra;
};

// Windowsのエラーコードを診断欄用の16進数へ整形する。
std::string HresultToString(HRESULT hr) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    return stream.str();
}

std::string WideToUtf8(const std::wstring& text) {
    // Windows APIから得た文字列を画面やレポートで使うUTF-8へ変換する。
    // 空文字は変換APIに渡さず、そのまま空の結果を返す。
    if (text.empty()) {
        return {};
    }

    std::int32_t size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<std::int32_t>(text.size()), nullptr, 0, nullptr, nullptr);

    // 変換に必要なバイト数を取得できなければ、結果を作らず空文字を返す。
    if (size <= 0) {
        return {};
    }

    std::string utf8(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<std::int32_t>(text.size()), utf8.data(), size, nullptr, nullptr);
    return utf8;
}

std::string PathToUtf8(const fs::path& path) {
    return WideToUtf8(path.wstring());
}

std::string MakeTimestamp() {
    // 撮影ファイルを時刻順に並べられる固定幅のローカル時刻を作る。
    std::time_t now = std::time(nullptr);
    std::tm localTime{};
    localtime_s(&localTime, &now);

    std::ostringstream stream;
    stream << std::put_time(&localTime, "%Y%m%d_%H%M%S");
    return stream.str();
}

fs::path GetCaptureRoot() {
    // プロジェクト直下から起動した場合は成果物を親階層へ分けて保存する。
    std::error_code ec;
    fs::path root = fs::current_path(ec);

    // カレントフォルダーを取得できない場合は、相対パスを使う。
    if (ec || root.empty()) {
        root = fs::path(L".");
    }

    // project直下で動く場合は、その親に撮影結果をまとめる。
    if (root.filename() == L"project" && root.has_parent_path()) {
        return root.parent_path() / "captures";
    }

    return root / "captures";
}

// 用途に応じて保存先のサブフォルダーを選ぶ。
fs::path GetScreenshotDirectory(CaptureToolWindow::ScreenshotTag tag) {
    return GetCaptureRoot() /
        (tag == CaptureToolWindow::ScreenshotTag::kSlides ? "slides" : "debug");
}

fs::path MakeUniqueScreenshotPath(const fs::path& directory, const std::string& stem) {
    // 同じ秒に連続撮影した画像や、用途変更先の既存ファイルを上書きしない。
    for (std::int32_t suffix = 1; suffix <= 9999; ++suffix) {
        const fs::path candidate = directory /
            (stem + (suffix == 1 ? "" : "_" + std::to_string(suffix)) + ".png");
        std::error_code ec;

        // 同名の画像が見つからなければ候補を返す。存在確認の失敗は空パスで返す。
        if (!fs::exists(candidate, ec)) {
            return ec ? fs::path{} : candidate;
        }
    }

    return {};
}

// 撮影矩形を全モニターを含む仮想画面の範囲へ収めます。
RECT ClampRectToVirtualScreen(const RECT& source) {
    RECT screen{
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)
    };

    RECT result{
        std::max(source.left, screen.left),
        std::max(source.top, screen.top),
        std::min(source.right, screen.right),
        std::min(source.bottom, screen.bottom)
    };

    return result;
}

// 動画エンコーダーに渡す矩形の幅・高さを偶数へ揃えます。
RECT MakeEvenSizedRect(const RECT& source) {
    RECT result = source;
    std::int32_t width = static_cast<std::int32_t>(result.right - result.left);
    std::int32_t height = static_cast<std::int32_t>(result.bottom - result.top);

    // 幅が奇数なら右端を1ピクセル削って偶数にする。
    if (width % 2 != 0) {
        --width;
    }

    // 高さが奇数なら下端を1ピクセル削って偶数にする。
    if (height % 2 != 0) {
        --height;
    }

    result.right = result.left + std::max(2, width);
    result.bottom = result.top + std::max(2, height);
    return result;
}

// エンジンのメインウィンドウを優先して撮影対象のハンドルを取得します。
HWND GetMainWindowHandle() {
    // ゲームのウィンドウを取れなければ、現在のスレッドのアクティブウィンドウ、最前面の順に確認する。
    engine::DirectXCommon* dxCommon = engine::DirectXCommon::GetInstance();

    // DirectX側でウィンドウ管理を取得できる間は、ゲーム本体のハンドルを優先する。
    if (dxCommon && dxCommon->GetWinApp()) {
        HWND hwnd = dxCommon->GetWinApp()->GetHwnd();

        // ウィンドウが作成されてハンドルが有効な値になっていれば優先候補として採用する。
        if (hwnd) {
            return hwnd;
        }
    }

    HWND active = GetActiveWindow();

    // このスレッドのアクティブウィンドウがあれば、デスクトップ最前面より優先する。
    if (active) {
        return active;
    }

    return GetForegroundWindow();
}

bool ResolveClientRect(RECT& outRect, std::string& errorMessage) {
    // クライアント座標を画面座標へ変換し、ウィンドウ枠を撮影範囲に含めない。
    HWND hwnd = GetMainWindowHandle();

    // ウィンドウを参照できなければ、クライアント位置を計算できないため中止する。
    if (!hwnd || !::IsWindow(hwnd)) {
        errorMessage = "メインウィンドウを取得できませんでした。";
        return false;
    }

    RECT client{};

    // クライアント領域の取得に失敗した場合は座標変換へ進まない。
    if (!::GetClientRect(hwnd, &client)) {
        errorMessage = "クライアント領域を取得できませんでした。";
        return false;
    }

    POINT topLeft{ client.left, client.top };
    POINT bottomRight{ client.right, client.bottom };
    ::ClientToScreen(hwnd, &topLeft);
    ::ClientToScreen(hwnd, &bottomRight);
    outRect = { topLeft.x, topLeft.y, bottomRight.x, bottomRight.y };
    return true;
}

bool ResolveCaptureRect(DebugEditor* editor, CaptureToolWindow::CaptureArea area, bool forceGameViewClientCapture, RECT& outRect, std::string& errorMessage) {
    // ウィンドウ全体と Game View の座標系を区別し、画面内の有効な矩形へ解決します。
    switch (area) {
    case CaptureToolWindow::CaptureArea::kGameView: {
        // 撮影モード中だけEditor UIがないため、ゲームだけを表示するクライアント領域を使う。
        if (forceGameViewClientCapture) {
            return ResolveClientRect(outRect, errorMessage);
        }

        std::int32_t left = 0;
        std::int32_t top = 0;
        std::int32_t right = 0;
        std::int32_t bottom = 0;

        // エディターからGame Viewの画像座標が取れなければ、他の領域へ広げず中止する。
        if (!editor || !editor->GetGameViewScreenRect(left, top, right, bottom)) {
            errorMessage = "Game Viewの画像領域を取得できません。Game Viewを表示してから撮影してください。";
            return false;
        }

        outRect = { left, top, right, bottom };
        outRect = ClampRectToVirtualScreen(outRect);
        break;
    }

    // ウィンドウ全体では枠を含む画面座標の矩形を使う。
    case CaptureToolWindow::CaptureArea::kWindow: {
        HWND hwnd = GetMainWindowHandle();

        // ゲームのハンドルが無いか、矩形を取得できない場合は撮影できない。
        if (!hwnd || !GetWindowRect(hwnd, &outRect)) {
            errorMessage = "ウィンドウ全体の撮影範囲を取得できませんでした。";
            return false;
        }

        outRect = ClampRectToVirtualScreen(outRect);
        break;
    }

    // デスクトップ全体では接続モニターを含む仮想画面全域を使う。
    case CaptureToolWindow::CaptureArea::kDesktop:
        outRect = {
            GetSystemMetrics(SM_XVIRTUALSCREEN),
            GetSystemMetrics(SM_YVIRTUALSCREEN),
            GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
            GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)
        };

        break;
    }

    // クリップ後の幅か高さが0以下になった場合は、画素を取得できない。
    if (outRect.right <= outRect.left || outRect.bottom <= outRect.top) {
        errorMessage = "撮影範囲のサイズが不正です。";
        return false;
    }

    return true;
}

bool CaptureScreenRect(const RECT& rect, CapturedImage& outImage, std::string& errorMessage) {
    const std::int32_t width = rect.right - rect.left;
    const std::int32_t height = rect.bottom - rect.top;

    // 空の矩形ではGDIリソースを確保せずに中止する。
    if (width <= 0 || height <= 0) {
        errorMessage = "撮影範囲のサイズが不正です。";
        return false;
    }

    HDC screenDc = GetDC(nullptr);

    // 画面DCを取得できなければ、画面からの読み取りを行えない。
    if (!screenDc) {
        errorMessage = "画面のDCを取得できませんでした。";
        return false;
    }

    // 上から下へ連続する32bit DIBを作り、画面DCから指定領域だけコピーする。
    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);

    // ビットマップか画素の参照が作れなければ、画面DCを返して中止する。
    if (!bitmap || !bits) {
        ReleaseDC(nullptr, screenDc);
        errorMessage = "キャプチャ用ビットマップを作成できませんでした。";
        return false;
    }

    HDC memoryDc = CreateCompatibleDC(screenDc);

    // コピー先のDCを作れなければ、先に確保したビットマップと画面DCを解放する。
    if (!memoryDc) {
        DeleteObject(bitmap);
        ReleaseDC(nullptr, screenDc);
        errorMessage = "キャプチャ用DCを作成できませんでした。";
        return false;
    }

    HGDIOBJ oldObject = SelectObject(memoryDc, bitmap);
    BOOL copied = BitBlt(memoryDc, 0, 0, width, height, screenDc, rect.left, rect.top, SRCCOPY | CAPTUREBLT);

    // 画素を読み取る前に選択中のGDIオブジェクトとDCだけを戻す。
    if (oldObject) {
        SelectObject(memoryDc, oldObject);
    }

    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);

    // BitBltが失敗した場合は画像を返さず、ビットマップも解放する。
    if (!copied) {
        DeleteObject(bitmap);
        errorMessage = "画面のコピーに失敗しました。";
        return false;
    }

    const size_t byteSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    outImage.width = width;
    outImage.height = height;
    outImage.bgra.resize(byteSize);
    // DIBが持つ画素はビットマップを削除すると無効になるため、先に所有するBGRA配列へ複製します。
    std::memcpy(outImage.bgra.data(), bits, byteSize);
    DeleteObject(bitmap);

    // デスクトップの取得画素は透過を保証しないため、PNGでは不透明へ固定する。
    for (size_t i = 3; i < outImage.bgra.size(); i += 4) {
        outImage.bgra[i] = 255;
    }

    return true;
}

bool SavePngWithWic(const fs::path& path, const CapturedImage& image, std::string& errorMessage) {
    // 寸法か画素が欠けている画像では保存を始めない。
    if (image.width <= 0 || image.height <= 0 || image.bgra.empty()) {
        errorMessage = "保存する画像データが空です。";
        return false;
    }

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    // 保存先を確保できない場合は、COMやエンコーダーを準備せずに中止する。
    if (ec) {
        errorMessage = "保存先フォルダを作成できませんでした: " + ec.message();
        return false;
    }

    // 呼び出しスレッド側のCOM状態に合わせて、ここで初期化した場合だけ終了処理を行う。
    HRESULT coResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = SUCCEEDED(coResult);

    // 別の方式で初期化済みならそのCOM環境を使い、それ以外の初期化失敗では終了する。
    if (FAILED(coResult) && coResult != RPC_E_CHANGED_MODE) {
        errorMessage = "COMの初期化に失敗しました。";
        return false;
    }

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));

    // WICのファクトリーが作れなければ、ここで新規に取得したCOM初期化分を返して終了する。
    if (FAILED(hr)) {
        // CoInitializeExが成功した場合だけ、今回の呼び出しに対応するCoUninitializeを行う。
        if (shouldUninitialize) {
            CoUninitialize();
        }

        errorMessage = "WIC Factoryを作成できませんでした。";
        return false;
    }

    // 以降のWIC操作は、一つ前までのHRESULTが成功の場合だけ進める。
    Microsoft::WRL::ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);

    // ストリームを作れたら、保存先のファイル名と書き込み権限を割り当てる。
    if (SUCCEEDED(hr)) {
        hr = stream->InitializeFromFilename(path.wstring().c_str(), GENERIC_WRITE);
    }

    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;

    // 出力ストリームが使える場合だけ、PNGエンコーダーを作る。
    if (SUCCEEDED(hr)) {
        hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    }

    // エンコーダーが作成できたら、書き出し先としてストリームを登録する。
    if (SUCCEEDED(hr)) {
        hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;

    // ストリームの登録が成功したら、一枚分のフレームを作る。
    if (SUCCEEDED(hr)) {
        hr = encoder->CreateNewFrame(&frame, nullptr);
    }

    // フレームを作成できたら、メタデータを設定する前に初期化する。
    if (SUCCEEDED(hr)) {
        hr = frame->Initialize(nullptr);
    }

    // フレームが初期化できた場合だけ、ピクセル単位の寸法を設定する。
    if (SUCCEEDED(hr)) {
        hr = frame->SetSize(static_cast<UINT>(image.width), static_cast<UINT>(image.height));
    }

    // 寸法を設定できた場合だけ、入力画素の形式を32bit BGRAに設定する。
    if (SUCCEEDED(hr)) {
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        hr = frame->SetPixelFormat(&format);
    }

    // 画素形式の設定に成功した場合だけ、全行をフレームへ渡す。
    if (SUCCEEDED(hr)) {
        // BGRAは1画素4バイトです。WICへは1行のバイト数と全体のバイト数を別々に渡します。
        const UINT stride = static_cast<UINT>(image.width * 4);
        const UINT bufferSize = static_cast<UINT>(image.bgra.size());
        hr = frame->WritePixels(static_cast<UINT>(image.height), stride, bufferSize, const_cast<BYTE*>(image.bgra.data()));
    }

    // 画素を書いたFrameを先に確定し、その後Encoderを確定してPNGの出力を完了する。
    if (SUCCEEDED(hr)) {
        hr = frame->Commit();
    }

    // フレームを確定できた場合だけ、エンコーダーも確定する。
    if (SUCCEEDED(hr)) {
        hr = encoder->Commit();
    }

    // この処理でCOMを初期化した場合だけ、今回の参照を返す。
    if (shouldUninitialize) {
        CoUninitialize();
    }

    // WIC操作のどこかが失敗したときは、保存結果を失敗として呼び出し元へ返す。
    if (FAILED(hr)) {
        errorMessage = "PNG保存に失敗しました。";
        return false;
    }

    return true;
}

} // namespace

CaptureToolWindow::~CaptureToolWindow() {
    // リソースに触る録画スレッドが終了してから、残っているリソースを解放する。
    StopRecordingWorker();
    CloseRecordingResources();
}

void CaptureToolWindow::Initialize(DebugEditor* editor) {
    // Game Viewの表示範囲を取得するため、Editor参照だけ保持します。
    editor_ = editor;
}

void CaptureToolWindow::DrawImGui() {
#ifdef USE_IMGUI
    // 録画Workerの完了を先に反映してから、現在の撮影設定と操作を表示します。
    UpdateRecording();

    ImGui::TextColored(ImVec4(0.45f, 0.95f, 1.0f, 1.0f), ICON_FA_CAMERA " キャプチャツール");
    ImGui::TextWrapped("スクリーンショットはPNG、録画はWindows Media Foundationで直接MP4として保存します。ffmpegは不要です。");
    ImGui::TextDisabled("ショートカット: F = スクリーンショット / G = 録画開始・停止 / F10 = 撮影モード切替");
    ImGui::TextDisabled("保存先: %s", PathToUtf8(GetCaptureRoot()).c_str());
    ImGui::Separator();

    // 録画Workerへ渡した矩形とFPSは開始時に固定されるため、録画中は設定変更を受け付けない。
    ImGui::BeginDisabled(recording_);
    std::int32_t areaIndex = static_cast<std::int32_t>(captureArea_);
    constexpr std::array<const char*, 3> kCaptureAreas = {
        "ゲーム画面のみ (Game View)",
        "ウィンドウ全体",
        "デスクトップ全体"
    };

    // 範囲を選び直した場合だけ有効な列挙値に収めて保存する。
    if (ImGui::Combo("撮影範囲", &areaIndex, kCaptureAreas.data(), static_cast<std::int32_t>(kCaptureAreas.size()))) {
        captureArea_ = static_cast<CaptureArea>(std::clamp(areaIndex, 0, static_cast<std::int32_t>(kCaptureAreas.size()) - 1));
    }

    ImGui::SliderFloat("録画FPS", &recordFps_, 1.0f, 60.0f, "%.0f fps");
    ImGui::EndDisabled();
    std::int32_t tagIndex = static_cast<std::int32_t>(screenshotTag_);
    constexpr std::array<const char*, 2> kScreenshotTags = {"スライド用", "デバッグ用"};

    // 用途の選択が変わった場合だけ、その用途を次回の保存先にする。
    if (ImGui::Combo("スクショ用途", &tagIndex, kScreenshotTags.data(), static_cast<std::int32_t>(kScreenshotTags.size()))) {
        screenshotTag_ = static_cast<ScreenshotTag>(std::clamp(tagIndex, 0, static_cast<std::int32_t>(kScreenshotTags.size()) - 1));
    }

    ImGui::TextDisabled("PNG保存先: %s", PathToUtf8(GetScreenshotDirectory(screenshotTag_)).c_str());

    // Game View選択中だけ、エディターのUIが範囲外であることを表示する。
    if (captureArea_ == CaptureArea::kGameView) {
        ImGui::TextDisabled("Game Viewの画像部分だけを保存します。タブやメニューバーは含みません。");
    }

    ImGui::Separator();

    ImGui::BeginDisabled(recording_);

    // 録画中を除き、撮影ボタンを押したフレームで静止画を保存する。
    if (ImGui::Button(ICON_FA_CAMERA " スクリーンショット", ImVec2(210.0f, 0.0f))) {
        CaptureScreenshot();
    }

    ImGui::EndDisabled();

    ImGui::SameLine();

    // 録画が始まっていない場合は開始操作を表示する。
    if (!recording_) {
        // 開始ボタンの押下時だけ録画スレッドの準備へ進む。
        if (ImGui::Button(ICON_FA_VIDEO " 録画開始", ImVec2(150.0f, 0.0f))) {
            StartRecording();
        }
    // 録画中は停止操作と、ワーカーが公開したフレーム統計を表示する。
    } else {
        // 停止ボタンの押下時は録画を確定して結果を受け取る。
        if (ImGui::Button(ICON_FA_STOP " 録画停止", ImVec2(150.0f, 0.0f))) {
            StopRecording();
        }

        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "REC MP4");

        ImGui::TextDisabled(
            "非同期録画: %llu frames / 取りこぼし %llu / 直近 %.1f ms",
            static_cast<unsigned long long>(recordedFrameCount_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(skippedFrameCount_.load(std::memory_order_relaxed)),
            lastRecordingFrameCostMs_.load(std::memory_order_relaxed));
    }

    ImGui::Separator();
    ImGui::TextWrapped("%s", statusText_.c_str());

    // 成功した出力があるときだけ、そのパスを表示する。
    if (!lastOutputPath_.empty()) {
        ImGui::TextWrapped("最後の出力: %s", PathToUtf8(lastOutputPath_).c_str());
    }

    // 保存した静止画の履歴がある場合だけ、用途の付け替えを表示する。
    if (!lastScreenshotPath_.empty()) {
        const bool canRetag = lastScreenshotPath_.parent_path() != GetScreenshotDirectory(screenshotTag_);
        ImGui::BeginDisabled(!canRetag);

        // 履歴が別用途の場合に押せるボタンで、前回の静止画を移動する。
        if (ImGui::Button("直前のスクショを選択中の用途へ移動")) {
            RetagLastScreenshot();
        }

        ImGui::EndDisabled();
    }
#endif
}

void CaptureToolWindow::UpdateHotkeys() {
    // テキスト編集中は撮影キーを奪わず、録画状態の更新だけ続けます。
    UpdateRecording();

#ifdef USE_IMGUI
    bool canUseShortcut = true;

    // ImGuiを使えるときだけ、入力欄や修飾キーによるショートカット抑止を調べる。
    if (ImGui::GetCurrentContext()) {
        const ImGuiIO& io = ImGui::GetIO();
        canUseShortcut = !io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && !io.KeySuper;
    }

    // 別の操作にキーを使っている場合、撮影用の入力を処理しない。
    if (!canUseShortcut) {
        return;
    }
#endif

    engine::InputManager* input = engine::InputManager::GetInstance();

    // 入力管理に接続できなければ、押下を読み取れない。
    if (!input) {
        return;
    }

    // Fが押されたフレームで、録画中でなければ静止画を保存する。
    if (!recording_ && input->IsKeyTriggered(DIK_F)) {
        CaptureScreenshot();
    }

    // Gが押されたフレームでは、現在の録画状態に応じて開始または停止する。
    if (input->IsKeyTriggered(DIK_G)) {
        // 録画中のGは、現在の出力を確定させる停止操作になる。
        if (recording_) {
            StopRecording();
        // 録画していない場合のGは、新しい録画を開始する操作になる。
        } else {
            StartRecording();
        }
    }
}

void CaptureToolWindow::CaptureScreenshot() {
    // 用途ごとのフォルダへ保存し、成功した画像だけ後から分類し直せるようにする。
    const fs::path output = MakeUniqueScreenshotPath(
        GetScreenshotDirectory(screenshotTag_), "screenshot_" + MakeTimestamp());
    // 衝突しない保存名を決められなければファイルを作らない。
    if (output.empty()) {
        statusText_ = "保存先の空きファイル名を決定できませんでした。";
        return;
    }

    // 保存に成功した画像だけ、直前出力と用途別の移動対象に登録する。
    if (CaptureToFile(output)) {
        lastOutputPath_ = output;
        lastScreenshotPath_ = output;
        statusText_ = "スクリーンショットを保存しました: " + PathToUtf8(output);
    }
}

// 直前に保存したPNGを、現在選択している用途のフォルダーへ移す。
void CaptureToolWindow::RetagLastScreenshot() {
    // 静止画の履歴がなければ対象がないため何もしない。
    if (lastScreenshotPath_.empty()) {
        return;
    }

    const fs::path directory = GetScreenshotDirectory(screenshotTag_);

    // すでに選択した用途に保存されていれば移動は不要。
    if (lastScreenshotPath_.parent_path() == directory) {
        return;
    }

    std::error_code ec;
    fs::create_directories(directory, ec);

    // 用途のフォルダーを準備できなければ、画像はその場に残す。
    if (ec) {
        statusText_ = "用途フォルダを作成できませんでした: " + ec.message();
        return;
    }

    const fs::path destination = MakeUniqueScreenshotPath(directory, lastScreenshotPath_.stem().string());

    // 既存の画像に重ならない移動先を作れなければ中止する。
    if (destination.empty()) {
        statusText_ = "移動先の空きファイル名を決定できませんでした。";
        return;
    }

    // 実ファイルの移動が成功してから履歴を更新し、失敗時は元の保存先を表示し続ける。
    fs::rename(lastScreenshotPath_, destination, ec);

    // 移動が失敗した場合は、既存の出力履歴を保ったままエラーを示す。
    if (ec) {
        statusText_ = "スクリーンショットを移動できませんでした: " + ec.message();
        return;
    }

    // 最後の出力が移動した画像を指す場合だけ、出力履歴も新しい場所へ更新します。
    if (lastOutputPath_ == lastScreenshotPath_) {
        lastOutputPath_ = destination;
    }

    lastScreenshotPath_ = destination;
    statusText_ = "スクリーンショットの用途を変更しました: " + PathToUtf8(destination);
}

bool CaptureToolWindow::CaptureGameViewToFile(const fs::path& path) {
    // 一時的にGame Viewへ範囲を切り替え、撮影後は元の設定へ戻します。
    const CaptureArea previousArea = captureArea_;
    captureArea_ = CaptureArea::kGameView;
    const bool succeeded = CaptureToFile(path);
    captureArea_ = previousArea;

    // 撮影が成功した場合だけ、直前の出力パスを更新する。
    if (succeeded) {
        lastOutputPath_ = path;
        statusText_ = "Game Viewを保存しました。";
    }

    return succeeded;
}

void CaptureToolWindow::StartRecording() {
    // Worker起動前に撮影矩形を確定し、無効な領域では録画を始めません。
    // 録画中に再度開始が呼ばれても、同時に二つのワーカーを起動しない。
    if (recording_) {
        return;
    }

    RECT captureRect{};
    std::string errorMessage;

    // 選択した範囲を画面座標で取得できなければ録画を始めない。
    if (!ResolveCaptureRect(editor_, captureArea_, forceGameViewClientCapture_, captureRect, errorMessage)) {
        statusText_ = errorMessage;
        return;
    }

    captureRect = MakeEvenSizedRect(captureRect);
    const std::int32_t width = static_cast<std::int32_t>(captureRect.right - captureRect.left);
    const std::int32_t height = static_cast<std::int32_t>(captureRect.bottom - captureRect.top);

    // エンコーダーへ渡せる最小サイズ未満なら中止する。
    if (width < 2 || height < 2) {
        statusText_ = "録画範囲が小さすぎます。";
        return;
    }

    std::error_code ec;
    fs::create_directories(GetCaptureRoot(), ec);

    // 保存先を作れなければ、録画ワーカーを起動しない。
    if (ec) {
        statusText_ = "録画保存先フォルダを作成できませんでした: " + ec.message();
        return;
    }

    // 前回Workerを回収した後に、今回の完了フラグ・統計・結果メッセージを初期化する。
    StopRecordingWorker();
    recordingTimestamp_ = MakeTimestamp();
    recordingOutputPath_ = GetCaptureRoot() / ("recording_" + recordingTimestamp_ + ".mp4");
    recordingFps_ = std::clamp(recordFps_, 1.0f, 60.0f);
    recordingStopRequested_.store(false, std::memory_order_release);
    recordingWorkerFinished_.store(false, std::memory_order_release);
    recordingWorkerSucceeded_.store(false, std::memory_order_release);
    recordedFrameCount_.store(0, std::memory_order_relaxed);
    skippedFrameCount_.store(0, std::memory_order_relaxed);
    lastRecordingFrameCostMs_.store(0.0f, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(recordingResultMutex_);
        recordingWorkerMessage_.clear();
    }

    try {
        recordingWorker_ = std::thread(
            &CaptureToolWindow::RecordingWorkerMain,
            this,
            recordingOutputPath_,
            captureRect,
            recordingFps_);
    } catch (const std::system_error& error) {
        statusText_ = "録画ワーカーを開始できませんでした: " + std::string(error.what());
        return;
    }

    // スレッド作成が成功した時点で録画中へ切り替え、エンコーダー準備の成否は後で結果として受け取る。
    recording_ = true;
    statusText_ = "録画を開始しました。画面取得とMP4圧縮はバックグラウンドで実行しています。";
}

void CaptureToolWindow::StopRecording() {
    // Workerの終了を待ってから結果を回収し、未書込の動画を成功扱いしません。
    // 録画状態も回収待ちのワーカーも無ければ結果を変更しない。
    if (!recording_ && !recordingWorker_.joinable()) {
        return;
    }

    StopRecordingWorker();
    ApplyRecordingWorkerResult();
}

void CaptureToolWindow::UpdateRecording() {
    // Workerが非同期に終了した場合も、UI側で停止処理と結果表示を完了します。
    // 録画中でなければ確認する終了通知がない。
    if (!recording_) {
        return;
    }

    // ワーカーが終了を公開したら、UI側で回収して結果を表示する。
    if (recordingWorkerFinished_.load(std::memory_order_acquire)) {
        StopRecording();
    }
}

void CaptureToolWindow::RecordingWorkerMain(fs::path outputPath, RECT captureRect, float fps) {
    // キャプチャと動画エンコードを描画スレッドから切り離して実行します。
    std::string resultMessage;
    bool succeeded = false;

    try {
        succeeded = StartMediaFoundationRecording(outputPath, captureRect, fps, resultMessage);

        // エンコーダーの準備が成功した場合に限って、予定時刻ごとの撮影へ進む。
        if (succeeded) {
            using Clock = std::chrono::steady_clock;
            // FPSを1枚あたりの秒数へ変え、単調増加する時計の刻みで取得予定を管理します。
            const auto frameInterval = std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(1.0 / static_cast<double>(std::max(fps, 1.0f))));
            const auto recordingStart = Clock::now();
            auto nextFrameTime = recordingStart;

            // 実際の取得時刻をMP4のPresentation Timeへ変換する。
            while (!recordingStopRequested_.load(std::memory_order_acquire)) {
                {
                    std::unique_lock<std::mutex> lock(recordingWaitMutex_);

                    // 次の時刻までの待機が停止通知で解除された場合は、次の画像を取得しない。
                    if (recordingWaitCondition_.wait_until(lock, nextFrameTime, [this]() {
                            return recordingStopRequested_.load(std::memory_order_acquire);
                        })) {
                        break;
                    }
                }

                const auto frameStart = Clock::now();
                // 録画開始からの実経過を100ナノ秒単位へ変換し、遅れたフレームでもMP4の時刻を保ちます。
                const LONGLONG sampleTime100ns = static_cast<LONGLONG>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(frameStart - recordingStart).count() / 100);
                // 取得した画像からサンプルを書き込めなければ、録画を失敗としてループを抜ける。
                if (!WriteRecordingFrame(sampleTime100ns, resultMessage)) {
                    succeeded = false;
                    break;
                }

                recordedFrameCount_.fetch_add(1, std::memory_order_relaxed);
                const auto frameEnd = Clock::now();
                const float frameCostMs = std::chrono::duration<float, std::milli>(frameEnd - frameStart).count();
                lastRecordingFrameCostMs_.store(frameCostMs, std::memory_order_relaxed);

                nextFrameTime += frameInterval;

                // このフレームの終了が次回の予定を越えたら、過ぎた予定を取りこぼしとして数える。
                if (nextFrameTime <= frameEnd) {
                    // 遅延分のフレームは撮影し直さず、次の予定時刻まで飛ばして負荷の累積を防ぐ。
                    const auto skipped = static_cast<std::uint64_t>((frameEnd - nextFrameTime) / frameInterval) + 1u;
                    skippedFrameCount_.fetch_add(skipped, std::memory_order_relaxed);
                    nextFrameTime += frameInterval * static_cast<Clock::duration::rep>(skipped);
                }
            }

            // 録画停止時もSink Writerを確定しないと、MP4の索引が未完成になる。
            HRESULT finalizeResult = S_OK;

            // 準備済みのWriterがある場合は、停止時にファイルを確定する。
            if (sinkWriter_) {
                finalizeResult = sinkWriter_->Finalize();
            }

            // 確定に失敗した場合は、フレームを書けていても保存失敗とする。
            if (FAILED(finalizeResult)) {
                succeeded = false;
                resultMessage = "MP4の保存確定に失敗しました: " + HresultToString(finalizeResult);
            // 確定が成功し、それ以前にも失敗がなければ保存完了を伝える。
            } else if (succeeded) {
                resultMessage = "録画を停止し、MP4を保存しました。";
            }
        }
    } catch (const std::exception& error) {
        succeeded = false;
        resultMessage = "録画ワーカーで例外が発生しました: " + std::string(error.what());
    } catch (...) {
        succeeded = false;
        resultMessage = "録画ワーカーで不明な例外が発生しました。";
    }

    // COMと録画資源は使用したWorker内で片付けてから、完了メッセージとフラグを公開する。
    CloseRecordingResources();
    {
        std::lock_guard<std::mutex> lock(recordingResultMutex_);
        recordingWorkerMessage_ = std::move(resultMessage);
    }

    recordingWorkerSucceeded_.store(succeeded, std::memory_order_release);
    recordingWorkerFinished_.store(true, std::memory_order_release);
}

void CaptureToolWindow::StopRecordingWorker() {
    // 停止要求を通知し、自スレッドをjoinしないようにしてWorkerを回収します。
    recordingStopRequested_.store(true, std::memory_order_release);
    recordingWaitCondition_.notify_all();

    // 起動済みのワーカーが別スレッドなら、リソースを使い終えるまで待つ。
    if (recordingWorker_.joinable() && recordingWorker_.get_id() != std::this_thread::get_id()) {
        recordingWorker_.join();
    }
}

void CaptureToolWindow::ApplyRecordingWorkerResult() {
    // Worker共有メッセージをロック下で読み、UIの録画状態へ確定します。
    recording_ = false;

    std::string workerMessage;
    {
        std::lock_guard<std::mutex> lock(recordingResultMutex_);
        workerMessage = recordingWorkerMessage_;
    }

    // ワーカーから成功が通知された場合だけ、新しい動画を出力履歴へ登録する。
    if (recordingWorkerSucceeded_.load(std::memory_order_acquire)) {
        lastOutputPath_ = recordingOutputPath_;
        statusText_ = workerMessage.empty() ? "録画を停止し、MP4を保存しました。" : workerMessage;
    // 失敗した場合は出力履歴を保ち、理由の表示だけ更新する。
    } else {
        statusText_ = workerMessage.empty() ? "録画を完了できませんでした。" : workerMessage;
    }
}

bool CaptureToolWindow::CaptureToFile(const fs::path& path) {
    // 選択した画面範囲を確定してから画素を取得し、PNGへ保存します。
    RECT rect{};
    std::string errorMessage;

    // 画面上の範囲が決まらなければ画素の取得は始めない。
    if (!ResolveCaptureRect(editor_, captureArea_, forceGameViewClientCapture_, rect, errorMessage)) {
        statusText_ = errorMessage;
        return false;
    }

    CapturedImage image;

    // 画面のコピーに失敗した場合は未確定の画素を保存しない。
    if (!CaptureScreenRect(rect, image, errorMessage)) {
        statusText_ = errorMessage;
        return false;
    }

    // PNGを確定できなかった場合は状態欄に保存の失敗理由を残す。
    if (!SavePngWithWic(path, image, errorMessage)) {
        statusText_ = errorMessage;
        return false;
    }

    return true;
}

bool CaptureToolWindow::StartMediaFoundationRecording(
    const fs::path& outputPath,
    const RECT& captureRect,
    float fps,
    std::string& errorMessage) {
    // 録画環境を初期化し、確保に成功した資源だけを終了処理へ引き継ぐ。
    CloseRecordingResources();

    HRESULT coResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // 既に別のCOM方式を使っているスレッドは継続可能。それ以外の初期化失敗では中止する。
    if (FAILED(coResult) && coResult != RPC_E_CHANGED_MODE) {
        errorMessage = "COMの初期化に失敗しました: " + HresultToString(coResult);
        return false;
    }

    recordingComInitialized_ = SUCCEEDED(coResult);

    HRESULT hr = MFStartup(MF_VERSION);

    // Media Foundationの起動に失敗したときは、今回初期化したCOMの分だけ戻して終了する。
    if (FAILED(hr)) {
        // ワーカー自身がCoInitializeExに成功した場合だけ、参照を返して記録を消す。
        if (recordingComInitialized_) {
            CoUninitialize();
            recordingComInitialized_ = false;
        }

        errorMessage = "Media Foundationの初期化に失敗しました: " + HresultToString(hr);
        return false;
    }

    mediaFoundationStarted_ = true;

    recordingRect_ = captureRect;
    recordingWidth_ = static_cast<std::int32_t>(recordingRect_.right - recordingRect_.left);
    recordingHeight_ = static_cast<std::int32_t>(recordingRect_.bottom - recordingRect_.top);
    // フレーム長はMedia Foundationの100ns単位へ変換し、サンプルの時刻も同じ単位で渡す。
    recordingFrameDuration100ns_ = static_cast<LONGLONG>(
        10000000.0 / static_cast<double>(std::max(fps, 1.0f)));

    // 対応GPUがある環境ではH.264のハードウェア変換を優先し、SinkWriter側の待機は無効化する。
    Microsoft::WRL::ComPtr<IMFAttributes> writerAttributes;

    // 属性を作成できた場合にだけGPU変換と待機制御のオプションを渡す。
    if (SUCCEEDED(MFCreateAttributes(&writerAttributes, 2))) {
        writerAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        writerAttributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    }

    Microsoft::WRL::ComPtr<IMFSinkWriter> writer;
    hr = MFCreateSinkWriterFromURL(
        outputPath.wstring().c_str(),
        nullptr,
        writerAttributes.Get(),
        &writer);
    // 指定した名前で出力先を開けなければ、録画環境を解放して失敗を返す。
    if (FAILED(hr)) {
        CloseRecordingResources();
        errorMessage = "MP4書き込み先の作成に失敗しました: " + HresultToString(hr);
        return false;
    }

    // 保存先の圧縮形式をH.264で定義し、続く入力形式とは別に解像度とFPSを設定する。
    Microsoft::WRL::ComPtr<IMFMediaType> outputType;
    hr = MFCreateMediaType(&outputType);
    // 以下の形式設定は、各段階のHRESULTが成功する間だけ次へ進める。
    if (SUCCEEDED(hr))
        hr = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    // 映像として識別できたら、圧縮方式にH.264を指定する。
    if (SUCCEEDED(hr))
        hr = outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    // 圧縮方式を設定できたら、目標ビットレートを指定する。
    if (SUCCEEDED(hr))
        hr = outputType->SetUINT32(MF_MT_AVG_BITRATE, 8000000);
    // ビットレートの設定が通ったら、全行を一度に表す映像形式を指定する。
    if (SUCCEEDED(hr))
        hr = outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    // 走査方式を設定できたら、選択範囲から得た出力寸法を指定する。
    if (SUCCEEDED(hr))
        hr = MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(recordingWidth_), static_cast<UINT32>(recordingHeight_));
    // 寸法が受理されたら、丸めたFPSを出力の速度に設定する。
    if (SUCCEEDED(hr))
        hr = MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(std::round(fps)), 1);
    // FPSの設定が通ったら、画素の縦横比を1対1にする。
    if (SUCCEEDED(hr))
        hr = MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    // H.264の設定一式が揃ったら、Writerに出力ストリームを登録する。
    if (SUCCEEDED(hr))
        hr = writer->AddStream(outputType.Get(), &videoStreamIndex_);

    // 画面から取得するRGB32を入力形式に指定し、Sink WriterにH.264への変換を任せる。
    Microsoft::WRL::ComPtr<IMFMediaType> inputType;
    // 出力ストリームの登録が済んだら、入力側の型を作る。
    if (SUCCEEDED(hr))
        hr = MFCreateMediaType(&inputType);
    // 入力側の型を作成できたら、その内容を映像として登録する。
    if (SUCCEEDED(hr))
        hr = inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    // 映像として登録できたら、画面から読むRGB32を入力の形式にする。
    if (SUCCEEDED(hr))
        hr = inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    // RGB32を設定できたら、入力側も全行を一度に表す形式にする。
    if (SUCCEEDED(hr))
        hr = inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    // 入力の走査方式を設定できたら、キャプチャ矩形の寸法を指定する。
    if (SUCCEEDED(hr))
        hr = MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(recordingWidth_), static_cast<UINT32>(recordingHeight_));
    // 入力寸法が受理されたら、出力と同じFPSを指定する。
    if (SUCCEEDED(hr))
        hr = MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(std::round(fps)), 1);
    // 入力のFPSを設定できたら、出力と同じ正方画素を指定する。
    if (SUCCEEDED(hr))
        hr = MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    // 入力形式の設定がすべて揃ったら、変換元として登録する。
    if (SUCCEEDED(hr))
        hr = writer->SetInputMediaType(videoStreamIndex_, inputType.Get(), nullptr);
    // 入力形式の登録が成功して初めて、書き込みを開始する。
    if (SUCCEEDED(hr))
        hr = writer->BeginWriting();

    // 入力・出力形式の設定か書き込み開始に失敗したら、録画環境を解放する。
    if (FAILED(hr)) {
        CloseRecordingResources();
        errorMessage = "MP4エンコーダーの準備に失敗しました: " + HresultToString(hr);
        return false;
    }

    // 書込開始まで成功したWriterだけを録画資源へ移し、終了処理で解放する。
    sinkWriter_ = writer.Detach();
    return true;
}

bool CaptureToolWindow::WriteRecordingFrame(LONGLONG sampleTime100ns, std::string& errorMessage) {
    // 動画に書き込むWriterが準備できていなければ、画像を取得しない。
    if (!sinkWriter_) {
        errorMessage = "録画ライターが初期化されていません。";
        return false;
    }

    // 同じ録画スレッド内で画素バッファを再利用し、毎フレームの再確保を避ける。
    thread_local CapturedImage image;

    // 指定した画面範囲のコピーに失敗したら、その取得時刻のサンプルは作成できない。
    if (!CaptureScreenRect(recordingRect_, image, errorMessage)) {
        return false;
    }

    const DWORD bufferSize = static_cast<DWORD>(image.bgra.size());
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(bufferSize, &buffer);
    BYTE* destination = nullptr;
    DWORD maxLength = 0;
    // バッファーを確保できたら、画素を格納するために書き込み先をマップする。
    if (SUCCEEDED(hr))
        hr = buffer->Lock(&destination, &maxLength, nullptr);
    // ロックに成功した場合だけ、取得したメモリーに画素を転送する。
    if (SUCCEEDED(hr)) {
        // 領域が必要バイト数より短ければ、範囲外へ書かず失敗させる。
        if (maxLength < bufferSize) {
            hr = E_FAIL;
        } else {
            // キャプチャの上から下への画素列を、RGB32動画入力に合わせて行単位で上下反転する。
            const size_t stride = static_cast<size_t>(image.width) * 4u;

            for (std::int32_t y = 0; y < image.height; ++y) {
                const size_t sourceOffset = static_cast<size_t>(image.height - 1 - y) * stride;
                const size_t destinationOffset = static_cast<size_t>(y) * stride;
                std::memcpy(destination + destinationOffset, image.bgra.data() + sourceOffset, stride);
            }
        }
    }

    // 画素への直接書き込みが終わったらロックを解除し、長さの確定とエンコーダーへの引き渡しへ進みます。
    if (destination) {
        buffer->Unlock();
    }

    // 画素コピーまで成功したときだけ、エンコーダーが読むデータ長を設定する。
    if (SUCCEEDED(hr))
        hr = buffer->SetCurrentLength(bufferSize);

    // 画素バッファに取得時刻とフレーム長を付け、圧縮側へ一枚のサンプルとして渡す。
    Microsoft::WRL::ComPtr<IMFSample> sample;
    // 画素バッファーの長さを設定できたら、フレーム用のサンプルを作る。
    if (SUCCEEDED(hr))
        hr = MFCreateSample(&sample);
    // サンプルを作成できたら、その画素データを関連付ける。
    if (SUCCEEDED(hr))
        hr = sample->AddBuffer(buffer.Get());
    // 画素を関連付けられたら、録画開始からの実経過時刻を設定する。
    if (SUCCEEDED(hr))
        hr = sample->SetSampleTime(sampleTime100ns);
    // 取得時刻を設定できたら、FPSから計算したフレーム長を設定する。
    if (SUCCEEDED(hr))
        hr = sample->SetSampleDuration(recordingFrameDuration100ns_);
    // 内容と時刻の設定をすべて完了したサンプルだけを書き出す。
    if (SUCCEEDED(hr))
        hr = sinkWriter_->WriteSample(videoStreamIndex_, sample.Get());

    // バッファーの作成からサンプルの書き込みまでのどこかが失敗すれば、エラーをワーカーへ返す。
    if (FAILED(hr)) {
        errorMessage = "録画フレームの書き込みに失敗しました: " + HresultToString(hr);
        return false;
    }

    return true;
}

void CaptureToolWindow::CloseRecordingResources() {
    // Media FoundationのWriterを解放してからランタイムを終了します。
    // 所有しているWriterがあるときだけ参照を返し、次回のためにポインターを空にする。
    if (sinkWriter_) {
        sinkWriter_->Release();
        sinkWriter_ = nullptr;
    }

    // Media Foundationを起動済みの場合だけ、Writerの解放後に停止する。
    if (mediaFoundationStarted_) {
        MFShutdown();
        mediaFoundationStarted_ = false;
    }

    // このスレッドで取得したCOM初期化の分が残っている場合だけ、終了処理を行う。
    if (recordingComInitialized_) {
        CoUninitialize();
        recordingComInitialized_ = false;
    }
}
