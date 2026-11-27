#include "D3DResouceLeakChecKer.h"
#include <wrl.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxgidebug.h>
D3DResourceLeakChecKer::~D3DResourceLeakChecKer() {
	// DXGIのデバッグインターフェースを取得できる場合にだけ、終了時の生存リソースを確認する。
	Microsoft::WRL::ComPtr<IDXGIDebug1> debug;
	if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&debug)))) {
		// 全体・アプリ・D3D12の各分類で、生存しているオブジェクトをデバッグ出力に列挙する。
		debug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
		debug->ReportLiveObjects(DXGI_DEBUG_APP, DXGI_DEBUG_RLO_ALL);
		debug->ReportLiveObjects(DXGI_DEBUG_D3D12, DXGI_DEBUG_RLO_ALL);
	}
}
