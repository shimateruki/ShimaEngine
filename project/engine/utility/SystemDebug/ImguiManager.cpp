#include "ImGuiManager.h"
#include "SrvManager.h"
#include "IconsFontAwesome5.h"
ImGuiManager* ImGuiManager::GetInstance() {
    static ImGuiManager sInstance;
    return &sInstance;
}

void ImGuiManager::Initialize(WinApp* winApp, engine::DirectXCommon* dxCommon) {
    (void)winApp;
#ifdef USE_IMGUI
    dxCommon_ = dxCommon;

    // ImGuiのコンテキストを作り、エディターのドッキングを有効にする。
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;    // ドッキング有効化

    // 黒を基調にし、背景・入力・選択の明度差で操作状態を見分けやすくします。
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.PopupRounding = 3.0f;
    style.TabRounding = 3.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(0.89f, 0.92f, 0.96f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.57f, 0.63f, 0.71f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.065f, 0.073f, 0.087f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.080f, 0.089f, 0.106f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.105f, 0.118f, 0.140f, 0.98f);
    colors[ImGuiCol_Border] = ImVec4(0.19f, 0.22f, 0.26f, 0.95f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.16f, 0.19f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.24f, 0.29f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.30f, 0.39f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.050f, 0.057f, 0.068f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.095f, 0.113f, 0.139f, 1.00f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.080f, 0.091f, 0.109f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.060f, 0.069f, 0.083f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.26f, 0.31f, 0.37f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.34f, 0.41f, 0.49f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.39f, 0.72f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.34f, 0.65f, 0.93f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.49f, 0.76f, 1.00f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.16f, 0.24f, 0.34f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.37f, 0.54f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.27f, 0.45f, 0.66f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.15f, 0.19f, 0.24f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.33f, 0.44f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.25f, 0.42f, 0.60f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.19f, 0.22f, 0.26f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.085f, 0.105f, 0.132f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.21f, 0.34f, 0.47f, 1.00f);
    colors[ImGuiCol_TabActive] = ImVec4(0.16f, 0.27f, 0.39f, 1.00f);
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.075f, 0.086f, 0.103f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.13f, 0.19f, 0.26f, 1.00f);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.12f, 0.15f, 0.19f, 1.00f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.11f, 0.13f, 0.16f, 0.75f);
    colors[ImGuiCol_DockingPreview] = ImVec4(0.28f, 0.55f, 0.85f, 0.70f);

    // 日本語の文字と編集UIで使うアイコンを、同じフォントアトラスへ読み込む。
    
    float fontSize = 18.0f;
    io.Fonts->AddFontFromFileTTF(
        "Resources/sprite/meiryo.ttc",
        fontSize,
        nullptr,
        io.Fonts->GetGlyphRangesJapanese()
    );

    // 日本語フォントと同じフォントアトラスでアイコンを使えるよう、次の読み込みを結合モードにする。
    ImFontConfig config;
    config.MergeMode = true;
    config.PixelSnapH = true;
    config.GlyphMinAdvanceX = fontSize; // アイコンの幅をフォントサイズに合わせる

    // アイコンフォントから取り込む文字コードの範囲を指定する。
    static const ImWchar kIconRanges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };

    io.Fonts->AddFontFromFileTTF(
        "Resources/font/fa-solid-900.ttf",
        fontSize,
        &config,
        kIconRanges
    );


    // プラットフォームのバックエンドを初期化
    ImGui_ImplWin32_Init(winApp->GetHwnd());

    // 共有SRVヒープと割り当てコールバックを渡して、DX12バックエンドを初期化する。
    ID3D12DescriptorHeap* srvDescriptorHeap = engine::SrvManager::GetInstance()->GetDescriptorHeap();

    // CPU書き込み用とGPU参照用に、共有ヒープの同じインデックスから両方のハンドルを取得します。
    auto allocSrv = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle) {
        uint32_t index = engine::SrvManager::GetInstance()->Allocate();
        *outCpuHandle = engine::SrvManager::GetInstance()->GetCpuDescriptorHandle(index);
        *outGpuHandle = engine::SrvManager::GetInstance()->GetGpuDescriptorHandle(index);
    };

    // 現在のコールバックは割り当て番号をヒープへ返却しません。ここでハンドルを受け取っても空き枠は増えません。
    auto freeSrv = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle) {
    
    };

    ImGui_ImplDX12_InitInfo initInfo = {};
    initInfo.Device = dxCommon_->GetDevice();
    initInfo.CommandQueue = dxCommon_->GetCommandQueue();
    initInfo.NumFramesInFlight = 2;
    initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    // Editor UIは深度を使わず、実ウィンドウ用バックバッファへ直接描画します。
    initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
    initInfo.SrvDescriptorHeap = srvDescriptorHeap;

    initInfo.LegacySingleSrvCpuDescriptor = { 0 };
    initInfo.LegacySingleSrvGpuDescriptor = { 0 };
    initInfo.SrvDescriptorAllocFn = allocSrv;
    initInfo.SrvDescriptorFreeFn = freeSrv;

    ImGui_ImplDX12_Init(&initInfo);
#endif
}

void ImGuiManager::Finalize() {
#ifdef USE_IMGUI
    // ImGuiのバックエンドを終了し、使っていたコンテキストを破棄する。
    // 描画・入力バックエンドが参照するImGuiコンテキストを残したまま、バックエンドを先に終了します。
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();

    // ImGuiのコンテキストを破棄
    ImGui::DestroyContext();
#endif
}

void ImGuiManager::BeginFrame() {
#ifdef USE_IMGUI
    // Win32とDirectX 12のバックエンドの準備を済ませてから、ImGuiのフレームを開始する。
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
#endif
}

void ImGuiManager::Draw() {
#ifdef USE_IMGUI
    // 各ウィンドウの入力とレイアウトを確定し、描画データを生成する。
    ImGui::Render();

    // ImGuiが使用するSRVを参照できるよう、共用ディスクリプタヒープを設定する。
    ID3D12GraphicsCommandList* commandList = dxCommon_->GetCommandList();
    assert(commandList != nullptr);
    engine::SrvManager::GetInstance()->SetDescriptorHeaps(commandList);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);
#endif
}

void ImGuiManager::EndFrame() {
#ifdef USE_IMGUI
    ImGuiIO& io = ImGui::GetIO();
    // 複数ビューポートが有効な場合にだけ、ImGuiが管理する別ウィンドウを更新して描く。
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        // ビューポートを処理した後に戻せるよう、現在のコンテキストを控える。
        // 1. 今のコンテキストをバックアップ
        ImGuiContext* previousContext = ImGui::GetCurrentContext();

        // ImGui側のビューポートと対応するOSウィンドウを更新し、追加ウィンドウを描画する。
        // 2. ウィンドウの位置などを更新
        ImGui::UpdatePlatformWindows();

        // ImGuiのバックエンドに、別ウィンドウの描画を依頼する。
        ImGui::RenderPlatformWindowsDefault(nullptr, nullptr);

        // 追加ウィンドウの処理前に使用していたコンテキストへ戻す。
        // 4. コンテキストを復元
        ImGui::SetCurrentContext(previousContext);
    }
#endif
}