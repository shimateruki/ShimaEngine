
#include <cstdint>
#include "DirectXCommon.h"
#include "ColorSpace.h"
#include "engine/system/scene/LoadingProfiler.h"
#include "WinApp.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cctype>
#include <format>
#include <vector>
#include <dxcapi.h>
#include <fstream>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <thread>
#include "SrvManager.h"
#include "PostEffect.h"
#include"ImGuiManager.h"
#include "ProfilerManager.h"

namespace engine {

// デバッグ出力へ文字列を流し、DirectX初期化やリソース生成の状態を追えるようにする。
void Log(const std::string& message) {
    OutputDebugStringA(message.c_str());
}

std::string ConvertString(const std::wstring& str);

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxcompiler.lib")
DirectXCommon* DirectXCommon::GetInstance() {
    // デバイス・描画コマンド・同期資源を共有するインスタンスを返す。
	static DirectXCommon sInstance;
	return &sInstance;
}

void DirectXCommon::Initialize(WinApp* winApp) {
    // DirectX12のデバイス、描画先、同期、シェーダーコンパイラ、GPU計測をまとめて初期化する。
	assert(winApp);
	winApp_ = winApp;

	InitializeFixedFps();

	// デバイスを用意してから、コマンド記録、出力先、GPU同期に必要な資源を順に作る。
	InitializeDXGIDevice();
	CreateCommand();
	CreateSwapChain();
	CreateRtv();
	CreateDsv();
	CreateFence();

	// ビューポートとシザー矩形の設定
	viewport_.Width = (float)WinApp::GetClientWidth();
	viewport_.Height = (float)WinApp::GetClientHeight();
	viewport_.TopLeftX = 0;
	viewport_.TopLeftY = 0;
	viewport_.MinDepth = 0.0f;
	viewport_.MaxDepth = 1.0f;
	scissorRect_.left = 0;
	scissorRect_.right = WinApp::GetClientWidth();
	scissorRect_.top = 0;
	scissorRect_.bottom = WinApp::GetClientHeight();

	// シェーダーの読み込みとコンパイルを扱うDXCと、include解決に使うハンドラーを作る。
	DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&dxcUtils_));
	DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxcCompiler_));
	dxcUtils_->CreateDefaultIncludeHandler(&includeHandler_);


// GPU計測用に、複数区間の開始・終了タイムスタンプを記録できるクエリヒープを作る。
	D3D12_QUERY_HEAP_DESC queryHeapDesc{};
	queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	queryHeapDesc.Count = kMaxGpuQueries;
	HRESULT hr = device_->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(&queryHeap_));
	assert(SUCCEEDED(hr));

	// クエリ結果をGPUからCPUへ渡すため、タイムスタンプ数に合わせて読み戻しバッファを確保する。
	D3D12_HEAP_PROPERTIES heapProp{};
	heapProp.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC resDesc{};
	resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	resDesc.Width = sizeof(uint64_t) * kMaxGpuQueries;
	resDesc.Height = 1;
	resDesc.DepthOrArraySize = 1;
	resDesc.MipLevels = 1;
	resDesc.SampleDesc.Count = 1;
	resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	hr = device_->CreateCommittedResource(
		&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
		IID_PPV_ARGS(&queryResultBuffer_));
	assert(SUCCEEDED(hr));

	// タイムスタンプ差を秒に換算できるよう、GPUキューの毎秒カウント数を取得する。
	commandQueue_->GetTimestampFrequency(&gpuFrequency_);
}

void DirectXCommon::PreDraw() {
    // 記録済みのゲーム映像を最終表示するため、バックバッファを準備する。
	frameCount_++;
	EnsureCommandListReadyForDraw();

	// シーンと後処理の記録を保持したまま、最終出力先だけ切り替えます。
	backBufferIndex_ = swapChain_->GetCurrentBackBufferIndex();

	// 現在のバックバッファを表示待ちから描画可能な状態へ遷移させる。
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = swapChainResources_[backBufferIndex_].Get();
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	commandList_->ResourceBarrier(1, &barrier);

	// レンダーターゲットビュー(RTV)のディスクリプタハンドルのポインタを取得します。
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle;
	rtvHandle.ptr = rtvDescriptorHeap_->GetCPUDescriptorHandleForHeapStart().ptr + (backBufferIndex_ * device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV));

	// 最終合成では色だけを書き込むため、ゲーム画面用の深度ビューを接続しない。

	commandList_->OMSetRenderTargets(1, &rtvHandle, false, nullptr);

	// ゲーム映像の外側を黒帯としてクリアします。
	const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	commandList_->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);



	// ビューポート（描画する画面内の領域）を設定します。
	D3D12_VIEWPORT outputViewport{0.0f, 0.0f, static_cast<float>(WinApp::GetWindowWidth()), static_cast<float>(WinApp::GetWindowHeight()), 0.0f, 1.0f};
	commandList_->RSSetViewports(1, &outputViewport);

	// シザー矩形（ピクセルを描画する範囲を限定する矩形）を設定します。
	D3D12_RECT outputScissor{0, 0, WinApp::GetWindowWidth(), WinApp::GetWindowHeight()};
	commandList_->RSSetScissorRects(1, &outputScissor);

	// 最終合成のシェーダーやUIが参照する共有SRVヒープを接続する。
	ID3D12DescriptorHeap* descriptorHeaps[] = { engine::SrvManager::GetInstance()->GetDescriptorHeap() };
	commandList_->SetDescriptorHeaps(1, descriptorHeaps);
}

// CPUから書き込めるUploadヒープに、指定バイト数のバッファを確保します。失敗時はnullptrを返します。

Microsoft::WRL::ComPtr<ID3D12Resource> DirectXCommon::CreateBufferResource(size_t sizeInBytes) {
	D3D12_HEAP_PROPERTIES uploadHeapProperties{};
	uploadHeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;

	D3D12_RESOURCE_DESC vertResoucesDesc{};

	vertResoucesDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	vertResoucesDesc.Width = sizeInBytes;

	// 線形バッファとして作るため、高さ・配列数・ミップ数・サンプル数を1にする。
	vertResoucesDesc.Height = 1;
	vertResoucesDesc.DepthOrArraySize = 1;
	vertResoucesDesc.MipLevels = 1;
	vertResoucesDesc.SampleDesc.Count = 1;

	// テクスチャ用の配置を使わず、バイト列として扱う。
	vertResoucesDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	// 頂点や定数などをGPUが読み取れる初期状態で確保する。
	Microsoft::WRL::ComPtr<ID3D12Resource> resource = nullptr;
	HRESULT hr = device_->CreateCommittedResource(&uploadHeapProperties, D3D12_HEAP_FLAG_NONE,
		&vertResoucesDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource));
	// GPUリソースを確保できなければ、要求バイト数とHRESULTをログへ残す。
	if (FAILED(hr)) {
		Log(std::format("CreateBufferResource failed. size={}, hr=0x{:08X}\n", sizeInBytes, static_cast<std::uint32_t>(hr)));
		return nullptr;
	}

	return resource;
}

// 固定FPS制御の基準時刻を初期化する。


void DirectXCommon::InitializeFixedFps() {
	reference_ = std::chrono::steady_clock::now();
}

// 1フレームが短すぎる場合に待機し、60FPS基準のフレーム間隔へ寄せる。

void DirectXCommon::UpdateFixedFps() {
	// 目標とするフレーム時間をマイクロ秒で表す。
	const std::chrono::microseconds kMinTimer(uint64_t(1000000.0f / 60.0f));

	// 65FPS相当の比較値。現在の待機条件ではkMinTimerを使う。
	const std::chrono::microseconds  kMinCheckTime(uint64_t(1000000.0f / 65.0f));

	// 現在の時刻を取得
	std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

	// 前回記録した時刻からの経過時間を計算
	std::chrono::microseconds elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - reference_);

	// 前回の基準時刻から1/60秒相当まで達していない場合だけ、残り時間を待つ。
	if (elapsed < kMinTimer) {
		// 短い休止ごとに単調時計で経過を確認し、目標時間へ達したら抜ける。
		while (std::chrono::steady_clock::now() - reference_ < kMinTimer) {
			std::this_thread::sleep_for(std::chrono::microseconds(1));
		}
	}

	// 待機後の実時刻を次回の基準にする。
	reference_ = std::chrono::steady_clock::now();
}

namespace {
using ShaderDependency = std::pair<std::filesystem::path, std::filesystem::file_time_type>;

// HLSL本体とinclude先の更新時刻を集めます。キャッシュの依存関係を特定できなければfalseを返します。
bool CollectShaderDependencies(const std::filesystem::path& source,
                               std::set<std::filesystem::path>& visited,
                               std::vector<ShaderDependency>& dependencies) {
    std::error_code error;
    const auto absolutePath = std::filesystem::absolute(source, error).lexically_normal();
    // 絶対パスへ変換できなければ、依存先を一意に記録できない。
    if (error)
        return false;
    // 同じファイルを再訪したときは依存に重複登録せず、循環したincludeでも探索を止めます。
    if (!visited.insert(absolutePath).second)
        return true;

    const auto modified = std::filesystem::last_write_time(absolutePath, error);
    // 更新時刻が取れなければ、キャッシュの有効期限を判定できない。
    if (error)
        return false;
    dependencies.emplace_back(absolutePath, modified);

    std::ifstream file(absolutePath);
    // ソースを開けなければ、さらに参照しているincludeを確認できない。
    if (!file)
        return false;
    std::string line;

    while (std::getline(file, line)) {
        const size_t first = line.find_first_not_of(" \t");
        // 空行や先頭が#includeで始まらない行を飛ばす。
        if (first == std::string::npos || line.compare(first, 8, "#include") != 0)
            continue;
        const size_t open = line.find_first_of("\"<", first + 8);
        // include先の開始区切りが見つからない場合は、この探索で依存先を確定できない。
        if (open == std::string::npos)
            return false;
        const char closeChar = line[open] == '"' ? '"' : '>';
        const size_t close = line.find(closeChar, open + 1);
        // 開始記号に対応する閉じ記号がなければ、ファイル名を確定できない。
        if (close == std::string::npos)
            return false;
        const auto includeName = std::filesystem::path(line.substr(open + 1, close - open - 1));
        auto includePath = absolutePath.parent_path() / includeName;

        // ソースと同じ階層から解決できない場合は、共通のシェーダーフォルダーを調べる。
        if (!std::filesystem::exists(includePath)) {
            includePath = std::filesystem::path("Resources/shader") / includeName;
        }

        // 依存先を特定できない場合はキャッシュせず、DXC本来のinclude解決に任せる。
        if (!std::filesystem::exists(includePath) ||
            !CollectShaderDependencies(includePath, visited, dependencies)) return false;
    }

    return true;
}
}

// 指定したファイル・プロファイル・エントリー名からDXILを取得します。依存ファイルの更新時刻が一致する場合はキャッシュを使います。
Microsoft::WRL::ComPtr<IDxcBlob> DirectXCommon::CompileShader(
	const std::wstring& filePath, const wchar_t* profile, const wchar_t* entryPoint) {
	using ShaderKey = std::tuple<std::wstring, std::wstring, std::wstring>;

	/// <summary>コンパイル済みシェーダーと依存ファイルを一緒に保持し、更新判定に使う。</summary>
	struct CachedShader {
		std::vector<ShaderDependency> dependencies;
		Microsoft::WRL::ComPtr<IDxcBlob> blob;
	};

	static std::mutex sCacheMutex;
	static std::map<ShaderKey, CachedShader> sShaderCache;
	// 同じHLSLでもシェーダーの種類や入口が異なれば別バイナリなので、3項目を組にしてキャッシュします。
	const ShaderKey key{filePath, profile, entryPoint};
	std::lock_guard<std::mutex> cacheLock(sCacheMutex);
	const auto found = sShaderCache.find(key);

	// 同じファイル・プロファイル・エントリーのキャッシュがあれば、依存先を検証する。
	if (found != sShaderCache.end()) {
		bool unchanged = true;

		for (const auto& [path, modified] : found->second.dependencies) {
			std::error_code error;

			// 更新時刻が変わった依存先や時刻を取得できない依存先が一つでもあれば、キャッシュを使えない。
			if (std::filesystem::last_write_time(path, error) != modified || error) {
				unchanged = false;
				break;
			}
		}

		// すべての依存先の時刻が一致した場合に限り、保存済みのバイナリを返す。
		if (unchanged)
			return found->second.blob;
	}

#if defined(NDEBUG) && !defined(USE_IMGUI)
	// 配布セットの事前コンパイル済みDXILを優先する。Shaderとinclude先が更新されていれば使わない。
	const std::filesystem::path compiledPath = std::filesystem::path("StartupShaderCache") /
		std::filesystem::path(filePath + L"." + profile + L"." + entryPoint + L".cso");
	std::error_code cacheError;
	const auto compiledTime = std::filesystem::last_write_time(compiledPath, cacheError);

	// 事前コンパイル済みファイルの時刻を取得できた場合だけ、配布用キャッシュを検証する。
	if (!cacheError) {
		std::set<std::filesystem::path> visited;
		std::vector<ShaderDependency> dependencies;

		// ソースからたどる依存先をすべて確認できれば、事前生成後の更新がないかを調べる。
		if (CollectShaderDependencies(filePath, visited, dependencies)) {
			const bool current = std::all_of(dependencies.begin(), dependencies.end(),
				[compiledTime](const ShaderDependency& dependency) {
					return dependency.second <= compiledTime;
				});
			// 全依存先が事前生成時刻以前なら、保存済みDXILの読み込みを試す。
			if (current) {
				Microsoft::WRL::ComPtr<IDxcBlobEncoding> compiledFile;

				// 事前生成ファイルが正常に開けた場合だけ、返却用のBlobを取得する。
				if (SUCCEEDED(dxcUtils_->LoadFile(compiledPath.c_str(), nullptr, &compiledFile))) {
					Microsoft::WRL::ComPtr<IDxcBlob> compiledBlob;

					// IDxcBlobを取得できた場合は、依存時刻とともにメモリにも登録して返す。
					if (SUCCEEDED(compiledFile.As(&compiledBlob))) {
						sShaderCache[key] = {std::move(dependencies), compiledBlob};
						return compiledBlob;
					}
				}
			}
		}
	}
#endif

	Microsoft::WRL::ComPtr<IDxcBlobEncoding> shaderSource = nullptr;

	// キャッシュを使えない場合は、HLSLのソースを読み込む。
	HRESULT hr = dxcUtils_->LoadFile(filePath.c_str(), nullptr, &shaderSource);
	assert(SUCCEEDED(hr));

	DxcBuffer shaderSourceBuffer;
	shaderSourceBuffer.Ptr = shaderSource->GetBufferPointer();
	shaderSourceBuffer.Size = shaderSource->GetBufferSize();
	shaderSourceBuffer.Encoding = DXC_CP_UTF8;

	// 配布版はシェーダーのデバッグ情報を埋め込まず、コンパイルとPSO生成の負担を減らす。
#if defined(NDEBUG) && !defined(USE_IMGUI)
	LPCWSTR arguments[] = {
		filePath.c_str(), L"-E", entryPoint, L"-T", profile, L"-Od", L"-Zpr",
	};
#else
	LPCWSTR arguments[] = {
		filePath.c_str(), L"-E", entryPoint, L"-T", profile,
		L"-Zi", L"-Qembed_debug", L"-Od", L"-Zpr",
	};
#endif

	Microsoft::WRL::ComPtr<IDxcResult> shaderResult = nullptr;

	// 初期化時のincludeハンドラーを渡し、指定の入口関数とプロファイルでコンパイルする。
	hr = dxcCompiler_->Compile(
		&shaderSourceBuffer, arguments, _countof(arguments),
		includeHandler_.Get(), IID_PPV_ARGS(&shaderResult));
	assert(SUCCEEDED(hr));

	Microsoft::WRL::ComPtr<IDxcBlobUtf8> shaderError = nullptr;
	shaderResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&shaderError), nullptr);

	// DXCの診断文字列が空でなければ、アサート有効時はここで止める。
	if (shaderError != nullptr && shaderError->GetStringLength() != 0) {
		assert(false);
	}

	Microsoft::WRL::ComPtr<IDxcBlob> shaderBlob = nullptr;
	hr = shaderResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shaderBlob), nullptr);
	assert(SUCCEEDED(hr));

	// 実行可能なバイナリが得られた場合だけ、メモリキャッシュの登録を検討する。
	if (shaderBlob) {
		std::set<std::filesystem::path> visited;
		std::vector<ShaderDependency> dependencies;

		// 依存先を漏れなく読み取れた場合に限り、次回の検証に使う時刻も保存する。
		if (CollectShaderDependencies(filePath, visited, dependencies)) {
			sShaderCache[key] = {std::move(dependencies), shaderBlob};
		// 依存先を確定できなければ、同じキーの古いキャッシュも破棄する。
		} else {
			sShaderCache.erase(key);
		}
	}

	return shaderBlob;
}

// エディターのシェーダー適用用にコンパイルします。失敗時は診断をerrorMessageへ書き、取得できたBlobを返します。
Microsoft::WRL::ComPtr<IDxcBlob> DirectXCommon::TryCompileShader(
    const std::wstring& filePath, const wchar_t* profile, const wchar_t* entryPoint, std::string& errorMessage) {
    errorMessage.clear();
    // DXCのユーティリティまたはコンパイラーが未初期化なら、読み込みを開始できない。
    if (!dxcUtils_ || !dxcCompiler_) {
        errorMessage = "DXCが初期化されていません";
        return nullptr;
    }
    Microsoft::WRL::ComPtr<IDxcBlobEncoding> source;
    // HLSLの読み込みが失敗したかデータが得られなければ、コンパイルを行わない。
    if (FAILED(dxcUtils_->LoadFile(filePath.c_str(), nullptr, &source)) || !source) {
        errorMessage = "HLSLを読み込めません";
        return nullptr;
    }
    DxcBuffer buffer{source->GetBufferPointer(), source->GetBufferSize(), DXC_CP_UTF8};
    LPCWSTR arguments[] = {filePath.c_str(), L"-E", entryPoint, L"-T", profile, L"-Od", L"-Zpr"};
    Microsoft::WRL::ComPtr<IDxcResult> result;
    // DXCの呼び出しに失敗したか結果を受け取れなければ、シェーダーの診断を参照できない。
    if (FAILED(dxcCompiler_->Compile(&buffer, arguments, _countof(arguments), includeHandler_.Get(), IID_PPV_ARGS(&result))) || !result) {
        errorMessage = "DXCの実行に失敗しました";
        return nullptr;
    }
    // DXC呼び出しが成功してもシェーダー自体がエラーになるため、コンパイル結果を別に確認します。
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    // シェーダーのコンパイル結果が失敗なら、取得できた診断文字列を優先する。
    if (FAILED(status)) {
        Microsoft::WRL::ComPtr<IDxcBlobUtf8> diagnostics;
        result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&diagnostics), nullptr);
        errorMessage = diagnostics ? std::string(diagnostics->GetStringPointer(), diagnostics->GetStringLength()) : "HLSLのコンパイルに失敗しました";
        return nullptr;
    }
    Microsoft::WRL::ComPtr<IDxcBlob> shader;
    // コンパイル結果からバイトコードが得られなかった場合は、その段階の失敗を伝える。
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shader), nullptr)) || !shader)
        errorMessage = "DXCからバイトコードを取得できません";
    return shader;
}

void DirectXCommon::Finalize() {
    // GPU処理の完了を待ってから、DirectX関連リソースを安全に解放する。
	// コマンドキューかフェンスが未作成なら、GPUの完了通知を予約できない。
	if (!commandQueue_ || !fence_)
		return;

	// キュー上に終了用フェンスを挿入し、それ以前の命令が完了してから参照を外す。
	fenceValue_++;
	HRESULT hr = commandQueue_->Signal(fence_.Get(), fenceValue_);
	assert(SUCCEEDED(hr));

	// GPUが終了用のフェンス値へ未到達の場合は、完了イベントまで待つ。
	if (fence_->GetCompletedValue() < fenceValue_) {
		fence_->SetEventOnCompletion(fenceValue_, fenceEvent_);
		const auto waitStart = std::chrono::steady_clock::now();
		WaitForSingleObject(fenceEvent_, INFINITE);
		const auto waitMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - waitStart).count();
		LogSlowLoadingOperation("DirectXCommon GPU fence wait", waitMs);
	}

	// --- コマンドリストが開いていたら安全に閉じる ---
    if (commandListOpen_) {
        hr = commandList_->Close();
        assert(SUCCEEDED(hr));
        commandListOpen_ = false;
    }


	// キューの完了確認とリストの終了後、描画基盤の参照を順に外す。
	fenceEvent_ = nullptr;
	fence_.Reset();
	commandList_.Reset();
	commandAllocator_.Reset();
	commandQueue_.Reset();
	swapChain_.Reset();
	device_.Reset();
	dxgiFactory_.Reset();

	Log("[DirectXCommon] Finalized successfully.\n");
}

// 描画後のバリア、クエリ解決、コマンド実行、Present、GPU同期をまとめて行う。


void DirectXCommon::PostDraw() {


	// 後続のPresentが扱えるよう、描画先を表示用の状態へ戻す。
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = swapChainResources_[backBufferIndex_].Get();
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	commandList_->ResourceBarrier(1, &barrier);

    // GPU計測が1件以上ある場合だけ、使用したクエリ範囲をCPU読み戻し用バッファへ転送する。
    if (nextQueryIndex_ > 0) {
        commandList_->ResolveQueryData(
            queryHeap_.Get(),
            D3D12_QUERY_TYPE_TIMESTAMP,
            0,
            nextQueryIndex_,
            queryResultBuffer_.Get(),
            0
        );
    }

	// コマンドリストへのコマンドの記録を終了します。
	HRESULT hr = commandList_->Close();
	assert(SUCCEEDED(hr));
	commandListOpen_ = false;

	// コマンドリストの配列を作成します
	ID3D12CommandList* commandLists[] = { commandList_.Get() };

	// コマンドキューにコマンドリストを投入し、GPUに実行を指示します。
	commandQueue_->ExecuteCommandLists(1, commandLists);
	const auto presentStart = std::chrono::steady_clock::now();
	swapChain_->Present(1, 0);
	static bool sFirstPresentLogged = false;

	// 最初のPresent呼び出しだけを、起動計測の終点として記録する。
	if (!sFirstPresentLogged) {
		LogStartupProfileCheckpoint("first Present");
		sFirstPresentLogged = true;
	}

	const double presentMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - presentStart).count();
	// Presentに40ミリ秒以上かかった場合は、遅い処理として記録を試みる。
	if (presentMs >= 40.0) {
		LogSlowLoadingOperation("DirectXCommon Present", presentMs);
	}

	// フェンスの目標値をインクリメントします。
	fenceValue_++;

	// GPUがコマンドキューのここまで処理を終えたら、フェンスに新しい値を書き込むよう指示します。
	commandQueue_->Signal(fence_.Get(), fenceValue_);

	// GPUの処理がまだ完了していないかチェックします。
	if (fence_->GetCompletedValue() < fenceValue_) {
		// GPUが指定したフェンス値に達したときに発火するイベントを設定します。
		fence_->SetEventOnCompletion(fenceValue_, fenceEvent_);
		const auto waitStart = std::chrono::steady_clock::now();
		WaitForSingleObject(fenceEvent_, INFINITE);
		const double waitMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - waitStart).count();
		// フェンスの待機が40ミリ秒以上の場合は、Presentとは別にGPU待ちとして記録を試みる。
		if (waitMs >= 40.0) {
			LogSlowLoadingOperation("DirectXCommon frame GPU fence wait", waitMs);
		}
	}
}

// DXGIファクトリと高性能GPUアダプタを選び、D3D12デバイスを作成する。
void DirectXCommon::InitializeDXGIDevice() {

#ifdef _DEBUG
	// Debug構成だけD3D12診断レイヤーを有効にする。
	Microsoft::WRL::ComPtr<ID3D12Debug1> debugController;

	// D3D12の診断インターフェースを取得できた端末では、デバイスの作成前に診断を有効にする。
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
		debugController->EnableDebugLayer();
	}
#endif

	HRESULT hr = CreateDXGIFactory(IID_PPV_ARGS(&dxgiFactory_));
	assert(SUCCEEDED(hr));

	Microsoft::WRL::ComPtr<IDXGIAdapter4> useAdapter;

	// ソフトウェアアダプタを避け、高性能GPUから順に実デバイスを選ぶ。
	for (UINT i = 0; dxgiFactory_->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&useAdapter)) != DXGI_ERROR_NOT_FOUND; ++i) {
		DXGI_ADAPTER_DESC3 adapterDesc{};
		hr = useAdapter->GetDesc3(&adapterDesc);
		assert(SUCCEEDED(hr));

		// ソフトウェアフラグのない最初の候補を採用する。
		if (!(adapterDesc.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE)) {
			break;
		}

		useAdapter = nullptr;
	}

	assert(useAdapter != nullptr);

	D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0 };

	// 上位の機能レベルが使えない環境では対応可能なレベルまで下げる。
	for (size_t i = 0; i < _countof(featureLevels); ++i) {
		hr = D3D12CreateDevice(useAdapter.Get(), featureLevels[i], IID_PPV_ARGS(&device_));

		// 要求した機能レベルでデバイスを作れた時点で、これより低いレベルは試さない。
		if (SUCCEEDED(hr)) {
			break;
		}
	}

	assert(device_ != nullptr);

#ifdef _DEBUG
	Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;

	// 診断キューが取得できた場合だけ、停止対象の重大度とログから除外する項目を設定する。
	if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);
		D3D12_MESSAGE_ID denyids[] = { D3D12_MESSAGE_ID_RESOURCE_BARRIER_MISMATCHING_COMMAND_LIST_TYPE };
		D3D12_MESSAGE_SEVERITY severities[] = { D3D12_MESSAGE_SEVERITY_INFO };
		D3D12_INFO_QUEUE_FILTER filter{};
		filter.DenyList.NumIDs = _countof(denyids);
		filter.DenyList.pIDList = denyids;
		filter.DenyList.NumSeverities = _countof(severities);
		filter.DenyList.pSeverityList = severities;
		infoQueue->PushStorageFilter(&filter);
	}
#endif
}

void DirectXCommon::CreateCommand() {
    // コマンドキュー、アロケータ、コマンドリストを作成する。
	D3D12_COMMAND_QUEUE_DESC commandQueueDesc{};
	HRESULT hr = device_->CreateCommandQueue(&commandQueueDesc, IID_PPV_ARGS(&commandQueue_));
	assert(SUCCEEDED(hr));
	hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator_));
	assert(SUCCEEDED(hr));
	hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator_.Get(), nullptr, IID_PPV_ARGS(&commandList_));
	assert(SUCCEEDED(hr));
	commandListOpen_ = SUCCEEDED(hr);
}

void DirectXCommon::EnsureCommandListReadyForDraw() {
	// 初回はCreateCommandで開かれたリストを使い、同じフレーム内では二重リセットしない。
	if (commandListOpen_) {
		return;
	}

	// 閉じたリストは、アロケーターの記録領域を先に戻してから開き直す。
	HRESULT hr = commandAllocator_->Reset();
	assert(SUCCEEDED(hr));
	hr = commandList_->Reset(commandAllocator_.Get(), nullptr);
	assert(SUCCEEDED(hr));
	commandListOpen_ = SUCCEEDED(hr);
}

void DirectXCommon::CreateSwapChain() {
    // ウィンドウへ表示するためのスワップチェーンを作成する。
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
	swapChainDesc.Width = WinApp::GetWindowWidth();
	swapChainDesc.Height = WinApp::GetWindowHeight();
	swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapChainDesc.SampleDesc.Count = 1;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.BufferCount = (UINT)backBufferCount_;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	HRESULT hr = dxgiFactory_->CreateSwapChainForHwnd(commandQueue_.Get(), winApp_->GetHwnd(), &swapChainDesc, nullptr, nullptr, reinterpret_cast<IDXGISwapChain1**>(swapChain_.GetAddressOf()));
	assert(SUCCEEDED(hr));
}

// 各バックバッファへ書き込むためのsRGBのRTVを、同じ番号の枠へ登録します。
void DirectXCommon::CreateRtv() {
	rtvDescriptorHeap_ = CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, (UINT)backBufferCount_, false);

	for (UINT i = 0; i < backBufferCount_; ++i) {
		HRESULT hr = swapChain_->GetBuffer(i, IID_PPV_ARGS(&swapChainResources_[i]));
		assert(SUCCEEDED(hr));
		D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
		rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		D3D12_CPU_DESCRIPTOR_HANDLE rtvStartHandle = rtvDescriptorHeap_->GetCPUDescriptorHandleForHeapStart();
		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle;
		rtvHandle.ptr = rtvStartHandle.ptr + (i * device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV));
		device_->CreateRenderTargetView(swapChainResources_[i].Get(), &rtvDesc, rtvHandle);
	}
}

void DirectXCommon::CreateDsv() {
    // 深度ステンシル用のヒープと深度バッファを作成し、DSVへ登録する。
	dsvDescriptorHeap_ = CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2, false);
	depthStencilResource_ = CreateDepthStencilTextureResource(WinApp::GetClientWidth(), WinApp::GetClientHeight());

	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
	dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	device_->CreateDepthStencilView(depthStencilResource_.Get(), &dsvDesc, dsvDescriptorHeap_->GetCPUDescriptorHandleForHeapStart());

    // 画像参照中も深度テストを続けられるよう、2番目に読み取り専用DSVを置く。
    dsvDesc.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL;
    D3D12_CPU_DESCRIPTOR_HANDLE readOnlyDsv = dsvDescriptorHeap_->GetCPUDescriptorHandleForHeapStart();
    readOnlyDsv.ptr += device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    device_->CreateDepthStencilView(depthStencilResource_.Get(), &dsvDesc, readOnlyDsv);


}

// GPU同期に使うフェンスとイベントを作成する。
void DirectXCommon::CreateFence() {
	HRESULT hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
	assert(SUCCEEDED(hr));
	fenceEvent_ = CreateEvent(NULL, FALSE, FALSE, NULL);
	assert(fenceEvent_ != nullptr);
	fenceValue_ = 0;
}

// 幅と高さをピクセルで受け取り、描画用の深度・ステンシルリソースを作ります。


Microsoft::WRL::ComPtr<ID3D12Resource> DirectXCommon::CreateDepthStencilTextureResource(int32_t width, int32_t height) {
	D3D12_RESOURCE_DESC resourceDesc{};
	resourceDesc.Width = width;
	resourceDesc.Height = height;
	resourceDesc.MipLevels = 1;
	resourceDesc.DepthOrArraySize = 1;

	// 同じ24ビット深度をDSVでは深度・ステンシル、SRVでは深度のみとして扱えるよう、リソース本体は型なしで確保します。
	resourceDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;

	resourceDesc.SampleDesc.Count = 1;
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_HEAP_PROPERTIES heapProperties{};
	heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_CLEAR_VALUE depthClearValue{};
	depthClearValue.DepthStencil.Depth = 1.0f;

	// 深度とステンシルのクリア値は、このリソースに作るDSVの形式に合わせる。
	depthClearValue.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;

	Microsoft::WRL::ComPtr<ID3D12Resource> resource = nullptr;
	HRESULT hr = device_->CreateCommittedResource(
		&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc,
		D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClearValue, IID_PPV_ARGS(&resource));
	assert(SUCCEEDED(hr));

	return resource;
}

// 読み込んだ画像メタデータに合わせてGPUテクスチャリソースを作成する。

Microsoft::WRL::ComPtr<ID3D12Resource> DirectXCommon::CreateTextureResource(
    const DirectX::TexMetadata& metadata, D3D12_RESOURCE_STATES initialState) {
	// 画像メタデータの画素寸法・配列数・ミップ数・形式をリソース記述にコピーする。
	D3D12_RESOURCE_DESC resouceDesc{};
	resouceDesc.Width = UINT(metadata.width);
	resouceDesc.Height = UINT(metadata.height);
	resouceDesc.MipLevels = UINT(metadata.mipLevels);
	resouceDesc.DepthOrArraySize = UINT16(metadata.arraySize);
	resouceDesc.Format = metadata.format;
	resouceDesc.SampleDesc.Count = 1;
	resouceDesc.Dimension = static_cast<D3D12_RESOURCE_DIMENSION>(metadata.dimension);

	// GPU上の標準ヒープから、呼び出し元が指定した初期状態で確保する。
	D3D12_HEAP_PROPERTIES heapProperties{};
	heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

	Microsoft::WRL::ComPtr<ID3D12Resource> resource = nullptr;
	HRESULT hr = device_->CreateCommittedResource(
		&heapProperties,
		D3D12_HEAP_FLAG_NONE,
		&resouceDesc,
		initialState,
		nullptr,
		IID_PPV_ARGS(&resource));
	assert(SUCCEEDED(hr));
	return resource;
}


// 配列の先頭面について、すべてのミップ階層の画素を対応するGPUサブリソースへ書き込みます。
void DirectXCommon::UploadTextureData(const Microsoft::WRL::ComPtr<ID3D12Resource>& texture, const DirectX::ScratchImage& mipImages) {
	const DirectX::TexMetadata& metadata = mipImages.GetMetadata();

	for (size_t mipLevel = 0; mipLevel < metadata.mipLevels; mipLevel++) {
		// ミップ階層ごとに同じ番号のサブリソースへ書きます。この経路は配列の先頭面だけを扱います。
		const DirectX::Image* img = mipImages.GetImage(mipLevel, 0, 0);
		HRESULT hr = texture->WriteToSubresource
		(
			UINT(mipLevel),
			nullptr,
			img->pixels,
			UINT(img->rowPitch),
			UINT(img->slicePitch)
		);
		assert(SUCCEEDED(hr));
	}
}

// 画像を読み込み、色空間に合わせた形式とミップを返します。読み込みやミップ生成に失敗した場合は白い1ピクセル画像を返します。


DirectX::ScratchImage DirectXCommon::LoadTexture(
    const std::string& filePath, bool forceSRGB, bool* loadedFromFile) {
    // 成否の出力先が渡された場合は、実ファイルを処理できるまで失敗状態にする。
    if (loadedFromFile) {
        *loadedFromFile = false;
    }

    // WICやミップ生成後も、DDSと同じ指定色空間でGPUへ登録する。
    const auto applyColorSpace = [forceSRGB](DirectX::ScratchImage& texture) {
        texture.OverrideFormat(forceSRGB ? DirectX::MakeSRGB(texture.GetMetadata().format)
                                        : DirectX::MakeLinear(texture.GetMetadata().format));
    };

    // 読込・ミップ生成に失敗した場合の白画像。実ファイルの成否は false のままにする。
    const auto makeWhiteTexture = [] {
        DirectX::ScratchImage white;
        white.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1, 1, 1, 1);
        std::fill_n(white.GetPixels(), white.GetPixelsSize(), uint8_t{255});
        return white;
    };

    DirectX::ScratchImage image;
    const std::wstring filePathW = ConvertString(filePath);
    std::string extension = filePath.size() >= 4 ? filePath.substr(filePath.size() - 4) : std::string{};
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    // 拡張子の末尾が大文字・小文字を問わず.ddsなら、DDS専用の読み込みを使う。
    if (extension == ".dds") {
        const HRESULT result = DirectX::LoadFromDDSFile(
            filePathW.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image);
        // DDSをデコードできなければ、既定の白画像を返す。
        if (FAILED(result)) {
            return makeWhiteTexture();
        }
        // DDS内のミップを再利用し、用途に応じた色空間だけを適用する。
        applyColorSpace(image);
        // 成否の出力先があれば、DDSで正常に読み込めたことを伝える。
        if (loadedFromFile) {
            *loadedFromFile = true;
        }
        return image;
    }

    const auto wicFlags = forceSRGB ? DirectX::WIC_FLAGS_FORCE_SRGB : DirectX::WIC_FLAGS_IGNORE_SRGB;
    // DDS以外でWICのデコードに失敗した場合は、ミップを作らず白画像を返す。
    if (FAILED(DirectX::LoadFromWICFile(filePathW.c_str(), wicFlags, nullptr, image))) {
        return makeWhiteTexture();
    }

    applyColorSpace(image);

    // 1×1画像には下位ミップが無いため、デコードできた画像をそのまま返す。
    if (image.GetMetadata().width == 1 && image.GetMetadata().height == 1) {
        // 成否の出力先があれば、下位ミップのない1ピクセル画像も読み込み成功として伝える。
        if (loadedFromFile) {
            *loadedFromFile = true;
        }
        return image;
    }

    DirectX::ScratchImage mipImages;
    const auto filterFlags = forceSRGB ? DirectX::TEX_FILTER_SRGB : DirectX::TEX_FILTER_DEFAULT;
    // WICの画像からミップ階層を生成できなければ、白画像を返す。
    if (FAILED(DirectX::GenerateMipMaps(image.GetImages(), image.GetImageCount(),
                                      image.GetMetadata(), filterFlags, 0, mipImages))) {
        return makeWhiteTexture();
    }
    applyColorSpace(mipImages);
    // 成否の出力先があれば、読み込みとミップ生成が完了したことを伝える。
    if (loadedFromFile) {
        *loadedFromFile = true;
    }
    return mipImages;
}

// UTF-8文字列をワイド文字列へ変換し、Win32やDXCなどのWCHAR APIへ渡せる形にする。
std::wstring DirectXCommon::ConvertString(const std::string& str) {
	// 空文字列なら変換APIを呼ばずに空の結果を返す。
	if (str.empty()) {

		return std::wstring();
	}

	auto sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(&str[0]), static_cast<std::int32_t>(str.size()), NULL, 0);

	// 必要な出力文字数を取得できなければ、書き込みを行わず空文字列を返す。
	if (sizeNeeded == 0) {
		return std::wstring();
	}

	std::wstring result(sizeNeeded, 0);
	MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(&str[0]), static_cast<std::int32_t>(str.size()), &result[0], sizeNeeded);
	return result;
}

// コマンドキューへフェンスを打ち、必要に応じてGPU完了まで待つ。
void DirectXCommon::FlushCommandQueue(bool reset) {
	HRESULT hr = S_OK;
	// 記録中のコマンドリストだけを閉じて実行する。PostDraw()で実行済みのリストは再投入しない。
	if (commandListOpen_) {
		hr = commandList_->Close();
		assert(SUCCEEDED(hr));
		commandListOpen_ = false;
		ID3D12CommandList* commandLists[] = { commandList_.Get() };
		commandQueue_->ExecuteCommandLists(_countof(commandLists), commandLists);
	}

	// --- フェンスシグナル送信 ---
	fenceValue_++;
	hr = commandQueue_->Signal(fence_.Get(), fenceValue_);
	assert(SUCCEEDED(hr) && "Failed to signal command queue fence");

	// キューに積んだ目標フェンス値へ未到達の場合だけ、通知イベントでCPUを待たせる。
	if (fence_->GetCompletedValue() < fenceValue_) {
		fence_->SetEventOnCompletion(fenceValue_, fenceEvent_);
		WaitForSingleObject(fenceEvent_, INFINITE);
	}

	// 呼び出し側が再開を指定した場合は、GPUの完了後にアロケーターとリストの順でリセットする。
	if (reset) {
		hr = commandAllocator_->Reset();
		assert(SUCCEEDED(hr) && "CommandAllocator reset failed");

		hr = commandList_->Reset(commandAllocator_.Get(), nullptr);
		assert(SUCCEEDED(hr) && "CommandList reset failed");
		commandListOpen_ = SUCCEEDED(hr);
	}
}

// 指定した種類と枠数のディスクリプタヒープを確保し、要求時だけシェーダーから見えるようにする。

Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> DirectXCommon::CreateDescriptorHeap(
	D3D12_DESCRIPTOR_HEAP_TYPE heapType,
	UINT numDescriptors,
	bool shaderVisible) {
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptorHeap = nullptr;
	D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc{};
	descriptorHeapDesc.Type = heapType;
	descriptorHeapDesc.NumDescriptors = numDescriptors;
	descriptorHeapDesc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	HRESULT hr = device_->CreateDescriptorHeap(&descriptorHeapDesc, IID_PPV_ARGS(&descriptorHeap));
	assert(SUCCEEDED(hr));
	return descriptorHeap;
}

/// <summary>
/// 標準的な深度ステンシルデスクリプタを取得する
/// </summary>
D3D12_DEPTH_STENCIL_DESC DirectXCommon::GetDefaultDepthStencilDesc() const {
	D3D12_DEPTH_STENCIL_DESC depthStencilDesc{};

	depthStencilDesc.DepthEnable = TRUE;

	depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;

	// 保存済みの深度以下なら通すため、手前と同じ深度の描画を許可する。
	depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

	depthStencilDesc.StencilEnable = FALSE;

	return depthStencilDesc;
}

/// <summary>
/// 深度ステンシルビューのフォーマットを取得する
/// </summary>
DXGI_FORMAT DirectXCommon::GetDsvFormat() const {

	return DXGI_FORMAT_D24_UNORM_S8_UINT;
}

namespace {
// キューへフェンスを積み、必要なときだけCPUを待たせる。リストの再開は呼び出し元が担当する。
void SignalAndWaitForGpu(ID3D12CommandQueue& queue, ID3D12Fence& fence, UINT64& value, HANDLE event, const char* label) {
	value++;
	HRESULT hr = queue.Signal(&fence, value);
	assert(SUCCEEDED(hr));

	// 通知したフェンス値にGPUが未到達の場合だけイベントを待ち、待機時間を渡された名前で記録する。
	if (fence.GetCompletedValue() < value) {
		fence.SetEventOnCompletion(value, event);
		const auto waitStart = std::chrono::steady_clock::now();
		WaitForSingleObject(event, INFINITE);
		const auto waitMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - waitStart).count();
		LogSlowLoadingOperation(label, waitMs);
	}
}
}

// GPU完了待ち後にコマンドアロケータとコマンドリストを再利用できる状態へ戻す。
void DirectXCommon::WaitForGpuAndReset() {
	// GPU同期かリスト再開に必要なオブジェクトが一つでも未作成・破棄済みなら終了する。
	if (!fence_ || !commandQueue_ || !commandAllocator_ || !commandList_) {
		Log("[DirectXCommon] WaitForGPUAndReset skipped (resources not ready)\n");
		return;
	}

	SignalAndWaitForGpu(*commandQueue_.Get(), *fence_.Get(), fenceValue_, fenceEvent_, "DirectXCommon GPU fence wait");

	// 記録中のリストは保持し、閉じている場合だけ次の描画用に開く。
	EnsureCommandListReadyForDraw();
}

void DirectXCommon::WaitForGpuIdle() {
    // 現在キューに積まれているGPU処理が完了するまでCPU側で待機する。
	// キューかフェンスが未作成なら、GPU完了を予約できないため終了する。
	if (!fence_ || !commandQueue_) {
		return;
	}

	SignalAndWaitForGpu(*commandQueue_.Get(), *fence_.Get(), fenceValue_, fenceEvent_, "DirectXCommon WaitForGPUIdle");
}

void DirectXCommon::CreateRenderTexture() {
    // ゲーム画面やポストエフェクト用のオフスクリーン描画テクスチャを作成する。
	// 1. リソース設定
	D3D12_RESOURCE_DESC resDesc = {};
	resDesc.Width = WinApp::GetClientWidth();
	resDesc.Height = WinApp::GetClientHeight();
	resDesc.MipLevels = 1;
	resDesc.DepthOrArraySize = 1;

	// HDRフォーマット (歪み用コピー先もこれに合わせる)
	resDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

	resDesc.SampleDesc.Count = 1;
	resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	// 2. クリアカラー
	D3D12_CLEAR_VALUE clearValue = {};
	clearValue.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	clearValue.Color[0] = clearColor_[0];
	clearValue.Color[1] = clearColor_[1];
	clearValue.Color[2] = clearColor_[2];
	clearValue.Color[3] = clearColor_[3];

	// 後処理から参照できる初期状態で、HDRの描画先を一時変数へ確保する。
	D3D12_HEAP_PROPERTIES heapProps = { D3D12_HEAP_TYPE_DEFAULT };
	Microsoft::WRL::ComPtr<ID3D12Resource> newRenderTexture;
	HRESULT hr = device_->CreateCommittedResource(
		&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,
		IID_PPV_ARGS(&newRenderTexture)
	);

	// HDRの色リソースを作れなければ、描画ビューの作成には進まない。
	if (FAILED(hr) || !newRenderTexture) {
		Log("CreateRenderTexture failed: render texture resource could not be created.\n");
		return;
	}

	// 4. RTV (Render Target View) の作成
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
	rtvHeapDesc.NumDescriptors = 1;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> newRtRtvHeap;
	hr = device_->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&newRtRtvHeap));

	// 色のRTVを配置するヒープを確保できなければ、参照ビューの登録には進まない。
	if (FAILED(hr) || !newRtRtvHeap) {
		Log("CreateRenderTexture failed: RTV heap could not be created.\n");
		return;
	}

	D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
	rtvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	device_->CreateRenderTargetView(newRenderTexture.Get(), &rtvDesc, newRtRtvHeap->GetCPUDescriptorHandleForHeapStart());

	// 5. SRV (Shader Resource View) の作成
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

	uint32_t newRenderTextureSrvHandle = renderTextureSrvHandle_;

	// 色のSRV番号があれば、同じ番号を新しい色リソースに向け直す。
	if (newRenderTextureSrvHandle != 0) {
		engine::SrvManager::GetInstance()->CreateSrvForResource(newRenderTextureSrvHandle, newRenderTexture.Get(), srvDesc);
	// 色のSRV番号をまだ持たない場合は、新しい枠を確保する。
	} else {
		newRenderTextureSrvHandle = engine::SrvManager::GetInstance()->CreateSrv(newRenderTexture.Get(), srvDesc);
	}

	// 歪みのパスが直前の描画結果を読めるよう、描画先と同じ形式で背景コピー用のテクスチャを作る。
	Microsoft::WRL::ComPtr<ID3D12Resource> newGrabTexture;
	HRESULT hrGrab = device_->CreateCommittedResource(
		&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,
		IID_PPV_ARGS(&newGrabTexture)
	);

	// 背景コピー用のリソースを作れなければ保持先の差し替えは中断する。色のSRV設定までは完了している。
	if (FAILED(hrGrab) || !newGrabTexture) {
		Log("CreateRenderTexture failed: grab texture resource could not be created.\n");
		return;
	}

	// 7. GrabTexture をシェーダーで読むための SRV 作成
	uint32_t newGrabSrvHandle = grabSrvHandle_;

	// 背景コピー用SRVの既存番号があれば、そのビューを更新する。
	if (newGrabSrvHandle != 0) {
		engine::SrvManager::GetInstance()->CreateSrvForResource(newGrabSrvHandle, newGrabTexture.Get(), srvDesc);
	// 背景コピー用SRVをまだ持たない場合は、新しい番号を確保する。
	} else {
		newGrabSrvHandle = engine::SrvManager::GetInstance()->CreateSrv(newGrabTexture.Get(), srvDesc);
	}

	// 作成した色と背景コピーの各リソースを、後続の描画から使う保持先へ登録する。
	renderTexture_ = newRenderTexture;
	rtRtvHeap_ = newRtRtvHeap;
	renderTextureSrvHandle_ = newRenderTextureSrvHandle;
	grabTexture_ = newGrabTexture;
	grabSrvHandle_ = newGrabSrvHandle;
}

void DirectXCommon::PreDrawRenderTexture() {
    // シーンをレンダーテクスチャへ描画する前に、RTV状態と描画先を準備する。
	// 色リソース、色のRTVヒープ、深度リソースが揃わない場合はシーン描画先を開けない。
	if (!renderTexture_ || !rtRtvHeap_ || !depthStencilResource_) {
		return;
	}

	EnsureCommandListReadyForDraw();

	// 1. バリア：読むモード -> 描くモード
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = renderTexture_.Get();
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	commandList_->ResourceBarrier(1, &barrier);

	// 2. 描画先を「レンダーテクスチャ」に切り替える
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtRtvHeap_->GetCPUDescriptorHandleForHeapStart();
	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = dsvDescriptorHeap_->GetCPUDescriptorHandleForHeapStart();
	commandList_->OMSetRenderTargets(1, &rtvHandle, false, &dsvHandle);

	// 3. クリア
	commandList_->ClearRenderTargetView(rtvHandle, clearColor_, 0, nullptr);
	commandList_->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

	// 4. ビューポート設定
	commandList_->RSSetViewports(1, &viewport_);
	commandList_->RSSetScissorRects(1, &scissorRect_);

	// 5. ヒープ設定
	ID3D12DescriptorHeap* descriptorHeaps[] = { engine::SrvManager::GetInstance()->GetDescriptorHeap() };
	commandList_->SetDescriptorHeaps(1, descriptorHeaps);
}

// レンダーテクスチャをシェーダーから読めるSRV状態へ戻す。
void DirectXCommon::PostDrawRenderTexture() {
	// 色リソースが未作成なら、読み取り用へ遷移する対象がないため終了する。
	if (!renderTexture_) {
		return;
	}

	// バリア：描くモード -> 読むモード
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = renderTexture_.Get();
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	commandList_->ResourceBarrier(1, &barrier);
}

void DirectXCommon::PreDrawBackBuffer() {
    // エディターのUIを最終表示するため、バックバッファを描画先に切り替える。
	backBufferIndex_ = swapChain_->GetCurrentBackBufferIndex();

	// スワップチェーンが示す現在のバッファを、描画先として書き込める状態へ遷移する。
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = swapChainResources_[backBufferIndex_].Get();
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	commandList_->ResourceBarrier(1, &barrier);

	// 1. バックバッファのハンドルを取得
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle;
	rtvHandle.ptr = rtvDescriptorHeap_->GetCPUDescriptorHandleForHeapStart().ptr +
		(backBufferIndex_ * device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV));

	// 2. 描画先をセット
	commandList_->OMSetRenderTargets(1, &rtvHandle, false, nullptr);

	// 設定側と同じロックで一組の色成分を取得してから、バックバッファをクリアする。
	float clearColor[4];
	{
		std::lock_guard<std::mutex> lock(clearColorMutex_);
		clearColor[0] = clearColor_[0];
		clearColor[1] = clearColor_[1];
		clearColor[2] = clearColor_[2];
		clearColor[3] = clearColor_[3];
	}

	commandList_->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);



	// 3. ビューポートなども画面サイズに戻す
	D3D12_VIEWPORT outputViewport{0.0f, 0.0f, static_cast<float>(WinApp::GetWindowWidth()), static_cast<float>(WinApp::GetWindowHeight()), 0.0f, 1.0f};
	commandList_->RSSetViewports(1, &outputViewport);
	D3D12_RECT outputScissor{0, 0, WinApp::GetWindowWidth(), WinApp::GetWindowHeight()};
	commandList_->RSSetScissorRects(1, &outputScissor);

	// 続く画像やUIの描画が使うSRVヒープを接続する。
	ID3D12DescriptorHeap* descriptorHeaps[] = { engine::SrvManager::GetInstance()->GetDescriptorHeap() };
	commandList_->SetDescriptorHeaps(1, descriptorHeaps);
}

// シャドウマップ用の深度テクスチャ、DSV、SRVを作成する。


void DirectXCommon::CreateShadowMap() {
	// 1. デスクリプタヒープの作成 (DSV用)
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
	dsvHeapDesc.NumDescriptors = 1;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	HRESULT hr = device_->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&shadowDsvHeap_));
	assert(SUCCEEDED(hr));

	// 2. リソースの設定 (DSVとSRVの両方で使えるように TYPELESS にする)
	D3D12_RESOURCE_DESC resDesc{};
	resDesc.Width = shadowMapResolution_;
	resDesc.Height = shadowMapResolution_;
	resDesc.MipLevels = 1;
	resDesc.DepthOrArraySize = 1;
	resDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	resDesc.SampleDesc.Count = 1;
	resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	// 3. ヒーププロパティとクリア値
	D3D12_HEAP_PROPERTIES heapProps{ D3D12_HEAP_TYPE_DEFAULT };
	D3D12_CLEAR_VALUE clearValue{};
	clearValue.Format = DXGI_FORMAT_D32_FLOAT; // クリア時は深度フォーマット
	clearValue.DepthStencil.Depth = 1.0f;
	clearValue.DepthStencil.Stencil = 0;

	// 4. リソースの生成 (初期状態は読み込み可能な SRV 状態にしておく)
	hr = device_->CreateCommittedResource(
		&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,
		IID_PPV_ARGS(&shadowMapResource_)
	);
	assert(SUCCEEDED(hr));

	// 5. DSV (Depth Stencil View) の作成
	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
	dsvDesc.Format = DXGI_FORMAT_D32_FLOAT; // 深度として解釈
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
	device_->CreateDepthStencilView(shadowMapResource_.Get(), &dsvDesc, shadowDsvHeap_->GetCPUDescriptorHandleForHeapStart());

	// 6. SRV (Shader Resource View) の作成
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_R32_FLOAT; // テクスチャとして読むときはFloat
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = 1;

	// 解像度変更時は既存のSRV番号を再利用し、Descriptorの消費を増やさない。
	if (shadowMapSrvHandle_ != 0) {
		engine::SrvManager::GetInstance()->CreateSrvForResource(shadowMapSrvHandle_, shadowMapResource_.Get(), srvDesc);
	}
	// シャドウマップのSRVをまだ割り当てていない場合は、新しい枠を確保する。
	else {
		shadowMapSrvHandle_ = engine::SrvManager::GetInstance()->CreateSrv(shadowMapResource_.Get(), srvDesc);
	}

	Log("Created Shadow Map successfully.\n");
}

// 要求された影の解像度を512、1024、2048、4096ピクセルのいずれかへ寄せ、必要な場合に影用リソースを作り直します。
void DirectXCommon::SetShadowMapResolution(std::int32_t resolution) {
	std::int32_t normalizedResolution = 2048;

	// 768以下の要求は512ピクセルの段階を使う。
	if (resolution <= 768) {
		normalizedResolution = 512;
	}
	// 769〜1536の要求は1024ピクセルの段階を使う。
	else if (resolution <= 1536) {
		normalizedResolution = 1024;
	}
	// 1537〜3072の要求は2048ピクセルの段階を使う。
	else if (resolution <= 3072) {
		normalizedResolution = 2048;
	}
	// 3073以上の要求は4096ピクセルの段階を使う。
	else {
		normalizedResolution = 4096;
	}

	// 選んだ段階と現在の解像度が同じで、既に影用リソースがあれば再作成しない。
	if (shadowMapResolution_ == normalizedResolution && shadowMapResource_) {
		return;
	}

	// 前フレームが参照している可能性があるため、解放前にGPU完了を保証する。
	WaitForGpuIdle();
	shadowMapResolution_ = normalizedResolution;
	shadowMapResource_.Reset();
	shadowDsvHeap_.Reset();
	CreateShadowMap();
}

void DirectXCommon::PreDrawShadow() {
    // シャドウマップへ深度を書き込むため、描画先とビューポートを影用に切り替える。
	// 1. バリア：画像として読むモード -> 深度を書き込むモードへ
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = shadowMapResource_.Get();
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	commandList_->ResourceBarrier(1, &barrier);

	// 2. 描画先を「シャドウマップのDSV」のみに設定（RTVは無し）
	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = shadowDsvHeap_->GetCPUDescriptorHandleForHeapStart();
	commandList_->OMSetRenderTargets(0, nullptr, false, &dsvHandle);

	// 光源から見た最遠の深度1.0で初期化してから、遮蔽物の深度を描く。
	commandList_->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

	// 4. ビューポートとシザー矩形をシャドウマップの解像度に合わせる
	D3D12_VIEWPORT viewport{};
	viewport.Width = static_cast<float>(shadowMapResolution_);
	viewport.Height = static_cast<float>(shadowMapResolution_);
	viewport.MaxDepth = 1.0f;
	commandList_->RSSetViewports(1, &viewport);

	D3D12_RECT scissorRect{};
	scissorRect.right = shadowMapResolution_;
	scissorRect.bottom = shadowMapResolution_;
	commandList_->RSSetScissorRects(1, &scissorRect);
}

void DirectXCommon::PostDrawShadow() {
    // シャドウマップをSRV状態へ戻し、通常描画用の描画先とビューポートを復帰する。
	// 1. バリア：深度を書き込むモード -> 画像として読むモードに戻す
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = shadowMapResource_.Get();
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	commandList_->ResourceBarrier(1, &barrier);
	// 通常のシーン描画へ戻れるよう、HDRの色と書き込み用深度を接続する。
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtRtvHeap_->GetCPUDescriptorHandleForHeapStart();
	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = dsvDescriptorHeap_->GetCPUDescriptorHandleForHeapStart();
	commandList_->OMSetRenderTargets(1, &rtvHandle, false, &dsvHandle);

	// ビューポートとシザー矩形もメイン画面用に戻す
	commandList_->RSSetViewports(1, &viewport_);
	commandList_->RSSetScissorRects(1, &scissorRect_);
}

void DirectXCommon::StartGpuProfile(const std::string& name) {
    // GPU計測区間の開始タイムスタンプを書き込む。
    // 未登録の計測名に限り、開始と終了のタイムスタンプを隣接する2枠へ割り当てる。
    if (!gpuProfileMap_.contains(name)) {
        // 残りが2枠未満なら対になる時刻を確保できないため、その区間の計測を開始しない。
        if (nextQueryIndex_ + 2 > kMaxGpuQueries)
            return;
        gpuProfileMap_[name] = nextQueryIndex_;
        nextQueryIndex_ += 2;
    }

    uint32_t startIndex = gpuProfileMap_[name];
    commandList_->EndQuery(queryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, startIndex);
}

void DirectXCommon::EndGpuProfile(const std::string& name) {
    // GPU計測区間の終了タイムスタンプを書き込む。
    // 開始時に計測名の枠を確保できなかった場合は、終了時刻を書き込まない。
    if (!gpuProfileMap_.contains(name))
        return;

    uint32_t startIndex = gpuProfileMap_[name];
    uint32_t endIndex = startIndex + 1;
    commandList_->EndQuery(queryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, endIndex);

    // PostDraw()で使用済みクエリを一括転送する。
}

void DirectXCommon::ResetGpuProfiles() {
    // 計測名と割り当て済み枠を空にし、以後の計測が先頭から使えるようにする。
    gpuProfileMap_.clear();
    nextQueryIndex_ = 0;
}

void DirectXCommon::ReadAllGpuProfiles() {
    // GPUタイムスタンプ結果を読み戻し、ProfilerManagerへミリ秒単位で記録する。
    // PostDraw()で転送済みのクエリを読み戻す。Game::Draw()は次の描画を記録する前にここを呼ぶ。
    D3D12_RANGE readRange = { 0, sizeof(uint64_t) * nextQueryIndex_ };
    uint64_t* mappedData = nullptr;
    
    // 割り当て済みのタイムスタンプ分だけをCPUの読み取り範囲として指定する。
    HRESULT hr = queryResultBuffer_->Map(0, &readRange, reinterpret_cast<void**>(&mappedData));

    // CPUへのマップに成功したときだけ、取得済みのタイムスタンプを読む。
    if (SUCCEEDED(hr)) {
        for (auto& pair : gpuProfileMap_) {
            const std::string& name = pair.first;
            uint32_t startIndex = pair.second;

            uint64_t start = mappedData[startIndex];
            uint64_t end = mappedData[startIndex + 1];

            // 終了が開始より後の値を持ち、GPUの周波数を取得できている区間だけを時間に換算する。
            if (end > start && gpuFrequency_ > 0) {
                // GPUタイムスタンプの差を毎秒カウント数で割って秒へ変え、1000倍でミリ秒にします。
                float timeMs = static_cast<float>(end - start) / static_cast<float>(gpuFrequency_) * 1000.0f;
                ProfilerManager::GetInstance()->RecordGpuTime(name, timeMs);

                // Total区間の結果だけをフレーム全体のGPU描画時間としても保存する。
                if (name == "Total") {
                    gpuDrawTimeMs_ = timeMs;
                }
            }
        }
        
        // 読み終わったら、書き込みは行わないので Empty Range で Unmap
        D3D12_RANGE writtenRange = { 0, 0 };
        queryResultBuffer_->Unmap(0, &writtenRange);
    }
}

// 深度バッファをシェーダーから参照するためのSRVを作成または更新する。
void DirectXCommon::CreateDepthSrv() {
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

	// 既存の深度SRV番号がある場合は、そのビューを現在の深度リソースへ付け替える。
	if (depthSrvHandle_ != 0) {
		engine::SrvManager::GetInstance()->CreateSrvForResource(depthSrvHandle_, depthStencilResource_.Get(), srvDesc);
	// 深度SRV番号が未確保の場合は、新しい枠を割り当てる。
	} else {
		depthSrvHandle_ = engine::SrvManager::GetInstance()->CreateSrv(depthStencilResource_.Get(), srvDesc);
	}
}

void DirectXCommon::PreDrawLocalFog() {
    // 書き込み用DSVを外してから、深度テストと画像参照が可能な状態へ切り替える。
	commandList_->OMSetRenderTargets(0, nullptr, false, nullptr);
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = depthStencilResource_.Get();
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	commandList_->ResourceBarrier(1, &barrier);

	// 深度SRVと読み取り専用DSVを同時に使い、特殊材質の深度テストを維持する。
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtRtvHeap_->GetCPUDescriptorHandleForHeapStart();
	D3D12_CPU_DESCRIPTOR_HANDLE readOnlyDsv = dsvDescriptorHeap_->GetCPUDescriptorHandleForHeapStart();
	readOnlyDsv.ptr += device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	commandList_->OMSetRenderTargets(1, &rtvHandle, false, &readOnlyDsv);
}

void DirectXCommon::PostDrawLocalFog() {
    // 読み取り専用DSVを外し、通常の深度書き込み状態へ戻す。
	commandList_->OMSetRenderTargets(0, nullptr, false, nullptr);
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = depthStencilResource_.Get();
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	commandList_->ResourceBarrier(1, &barrier);
	// 通常描画用の色RTVと書き込み用DSVを再び接続する。
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtRtvHeap_->GetCPUDescriptorHandleForHeapStart();
	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = dsvDescriptorHeap_->GetCPUDescriptorHandleForHeapStart();
	commandList_->OMSetRenderTargets(1, &rtvHandle, false, &dsvHandle);
}

void DirectXCommon::UpdateGrabTexture() {
    // 現在のレンダーテクスチャ内容をGrab用テクスチャへコピーする。
	// 背景コピーの元または先が未作成なら、状態遷移もコピーも行わない。
	if (!renderTexture_ || !grabTexture_) {
		return;
	}

	// バリア: RTV(描画先) -> COPY_SOURCE(コピー元) へ
	D3D12_RESOURCE_BARRIER barrier1{};
	barrier1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier1.Transition.pResource = renderTexture_.Get();
	barrier1.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier1.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	commandList_->ResourceBarrier(1, &barrier1);

	// バリア: SRV(画像) -> COPY_DEST(コピー先) へ
	D3D12_RESOURCE_BARRIER barrier2{};
	barrier2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier2.Transition.pResource = grabTexture_.Get();
	barrier2.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	barrier2.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	commandList_->ResourceBarrier(1, &barrier2);

	// 描画先の内容を、歪みエフェクトが読む背景テクスチャへコピーする。
	commandList_->CopyResource(grabTexture_.Get(), renderTexture_.Get());

	// コピー後は元の色を描画先へ、退避した背景をシェーダー参照用へ戻す。
	barrier1.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	barrier1.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	commandList_->ResourceBarrier(1, &barrier1);

	barrier2.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier2.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	commandList_->ResourceBarrier(1, &barrier2);
}

void DirectXCommon::ResizeSwapChain(int32_t width, int32_t height) {
    // バックバッファだけ実ウィンドウへ追従させ、ゲームの座標・深度・後処理は保持します。
    // 幅か高さが0以下なら、最小化などで有効な出力サイズがないため変更しない。
    if (width <= 0 || height <= 0)
        return;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    swapChain_->GetDesc1(&desc);
    // 現在のバックバッファと同じ寸法なら、GPU待ちと再作成を省く。
    if (desc.Width == static_cast<UINT>(width) && desc.Height == static_cast<UINT>(height))
        return;
    WaitForGpuIdle();
    // 実行済みフレームの参照が残らないのを待ち、古いバックバッファをすべて手放してからResizeBuffersを呼びます。
    for (auto& resource : swapChainResources_)
        resource.Reset();
    HRESULT hr = swapChain_->ResizeBuffers(static_cast<UINT>(backBufferCount_),
        static_cast<UINT>(width), static_cast<UINT>(height), desc.Format, desc.Flags);
    assert(SUCCEEDED(hr));
    // 以後の描画範囲が新しい実ウィンドウに合うよう、寸法を記録してRTVを作り直す。
    WinApp::SetWindowSize(width, height);
    CreateRtv();
}
void DirectXCommon::RequestResize(int32_t width, int32_t height) {
    // WM_SIZEなどから受け取ったサイズ変更要求を、次の安全なタイミングまで保留する。
	// 正でない寸法は無効として、保留中の有効な要求を上書きしない。
	if (width <= 0 || height <= 0) {
		return;
	}

	pendingResizeWidth_ = width;
	pendingResizeHeight_ = height;
	pendingResize_ = true;
}

void DirectXCommon::ProcessPendingResize() {
    // 保留中のサイズ変更要求があれば、描画ループ側の安全なタイミングで反映する。
	// サイズ変更の要求がなければ、このフレームは何も変更しない。
	if (!pendingResize_) {
		return;
	}

	// 保留を消費してから実際の変更を行い、同じ要求を次フレームへ持ち越さない。
	const int32_t width = pendingResizeWidth_;
	const int32_t height = pendingResizeHeight_;
	pendingResize_ = false;

	ResizeSwapChain(width, height);
}

void DirectXCommon::SetRenderClearColor(float r, float g, float b, float a) {
    // 編集用の色を作業色空間へ変換し、描画スレッドと競合しないよう更新する。
    const engine::Vector4 workingColor = engine::colorSpace::AuthoringToWorking({ r, g, b, a });
    std::lock_guard<std::mutex> lock(clearColorMutex_);
    clearColor_[0] = workingColor.x;
    clearColor_[1] = workingColor.y;
    clearColor_[2] = workingColor.z;
    clearColor_[3] = workingColor.w;
}

} // namespace engine
