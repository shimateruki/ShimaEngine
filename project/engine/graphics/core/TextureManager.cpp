#include "TextureManager.h"
#include <cstring>
#include "engine/system/scene/LoadingProfiler.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>

#include <Windows.h>
#include <shellapi.h>

#include "DirectXCommon.h"
#include "ProfilerManager.h"
#include "SrvManager.h"
#include "d3dx12.h"

#pragma comment(lib, "shell32.lib")

namespace engine {

namespace {
const std::filesystem::path kDdsCacheRequestPath = "Resources/.cache/dds_cache_requests.jsonl";
const std::filesystem::path kDdsCacheDisableFlagPath = "Resources/.cache/disable_dds_cache.flag";

// この時間未満の読み込みはDDS化しても効果が薄いため、変換要求を出さない。
constexpr float kDdsCacheRequestMinDurationMs = 2.0f;
std::string ToLower(std::string text) {
    // 文字列を小文字化し、拡張子やパスの比較を大文字小文字に左右されない形へ揃える。
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

std::string NormalizeTexturePath(std::string path) {
    // Windowsのバックスラッシュをスラッシュへ統一し、テクスチャパスを比較しやすくする。
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

std::string NormalizeTexturePathLower(std::string path) {
    // 正規化したテクスチャパスを小文字化し、キャッシュ判定用のキーとして扱いやすくする。
    return ToLower(NormalizeTexturePath(std::move(path)));
}

// C++20のchar8_t対応コンストラクターでUTF-8をWindowsパスへ変換する。
std::filesystem::path PathFromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

// ファイルシステムのパスを、ログやキャッシュキーに使うUTF-8文字列へ戻します。
std::string PathToUtf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return { text.begin(), text.end() };
}

std::string ToProjectPath(const std::filesystem::path& path) {
    // 絶対パスを可能な限りプロジェクト相対パスへ戻し、ログやキャッシュ要求を環境依存にしにくくする。
    std::error_code ec;
    std::filesystem::path absolutePath = std::filesystem::absolute(path, ec);

    // 絶対パスを作れなければ、元の表記から区切り文字だけを統一して返す。
    if (ec) {
        return NormalizeTexturePath(PathToUtf8(path));
    }

    absolutePath = absolutePath.lexically_normal();
    std::filesystem::path rootPath = std::filesystem::current_path(ec);

    // カレントディレクトリを取得できた場合に、そこからの相対パスを求める。
    if (!ec) {
        std::filesystem::path relativePath = absolutePath.lexically_relative(rootPath.lexically_normal());

        // 相対表記を求められた場合は優先する。求められなければ絶対パスを返す。
        if (!relativePath.empty()) {
            return NormalizeTexturePath(PathToUtf8(relativePath));
        }
    }

    return NormalizeTexturePath(PathToUtf8(absolutePath));
}

bool IsSourceTextureExtension(const std::string& ext) {
    // DDS生成元として扱う画像拡張子かどうかを判定する。
    const std::string lowerExt = ToLower(ext);
    return lowerExt == ".png" ||
           lowerExt == ".jpg" ||
           lowerExt == ".jpeg" ||
           lowerExt == ".tga" ||
           lowerExt == ".hdr";
}

// 開発用フラグファイルの有無からDDSキャッシュ停止を判定します。
bool IsDdsCacheDisabled() {
    std::error_code ec;
    return std::filesystem::exists(kDdsCacheDisableFlagPath, ec) && !ec;
}

std::filesystem::path FindSourceTextureForDds(const std::filesystem::path& ddsPath) {
    // DDS指定時に元画像が存在するか探し、古いDDSを元画像から再生成できるようにする。
    static const char* kSourceExts[] = { ".png", ".jpg", ".jpeg", ".tga", ".hdr" };

    for (const char* ext : kSourceExts) {
        std::filesystem::path candidate = ddsPath;
        candidate.replace_extension(ext);

        // 候補の拡張子を定義順に試し、最初に存在する元画像を返す。
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }

    return {};
}

bool IsEditorPreviewTexture(const std::string& path) {
    // エディタの一時プレビュー画像かどうかを判定し、不要なDDSキャッシュ生成を避ける。
    const std::string normalized = "/" + NormalizeTexturePathLower(path);
    return normalized.find("/generated/text/_preview_") != std::string::npos ||
           normalized.find("/generated/editor/text_preview/") != std::string::npos;
}

bool IsLinearTexturePath(const std::string& path) {
    // 法線やマスク系のテクスチャ名を判定し、sRGB変換しないリニア扱いにする。
    const std::string lowerPath = NormalizeTexturePathLower(path);
    return lowerPath.find("normal") != std::string::npos ||
           lowerPath.find("bakedshader") != std::string::npos ||
           lowerPath.find("_mask") != std::string::npos ||
           lowerPath.find("_noise") != std::string::npos ||
           lowerPath.find("_n") != std::string::npos ||
           lowerPath.find("nor") != std::string::npos ||
           lowerPath.find("arm") != std::string::npos ||
           lowerPath.find("orm") != std::string::npos ||
           lowerPath.find("rough") != std::string::npos ||
           lowerPath.find("metal") != std::string::npos ||
           lowerPath.find("ao") != std::string::npos;
}

std::string GetDdsFormat(const std::string& filePath, bool isLinearTexture) {
    // テクスチャ用途に応じて、DDS変換時に使用する圧縮フォーマット名を決める。
    const std::string ext = ToLower(PathFromUtf8(filePath).extension().string());

    // HDRの元画像は明るさの範囲を保つため浮動小数点のBC6Hを選ぶ。
    if (ext == ".hdr") {
        return "BC6H_UF16";
    }

    // HDR以外で線形指定の画像は、sRGB変換のかからないBC7を選ぶ。
    if (isLinearTexture) {
        return "BC7_UNORM";
    }

    return "BC7_UNORM_SRGB";
}

// DDS生成要求用の監視スクリプトを最初の必要時に一度だけ起動します。
void StartDdsCacheWatcherIfNeeded() {
    static bool sWatcherStartTried = false;

    // 既に初回の起動処理を試していれば、同じプロセスから再度開かない。
    if (sWatcherStartTried) {
        return;
    }

    sWatcherStartTried = true;

    std::error_code ec;
    const std::filesystem::path scriptPath =
        (std::filesystem::current_path(ec) / "tools/dds_cache/start_dds_cache_watcher.vbs").lexically_normal();
    // カレントディレクトリを取得できないかスクリプトが未配置なら、監視プロセスは起動しない。
    if (ec || !std::filesystem::exists(scriptPath)) {
        return;
    }

    const std::wstring parameters = L"\"" + scriptPath.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"wscript.exe", parameters.c_str(), nullptr, SW_HIDE);
}

std::string EscapeJson(const std::string& text) {
    // JSONLへ書き込む文字列をエスケープし、パスに記号が含まれても壊れないようにする。
    std::string escaped;
    escaped.reserve(text.size());

    for (char c : text) {
        switch (c) {
        // パス区切りにも使うバックスラッシュを、JSONで表せる形にする。
        case '\\': escaped += "\\\\";
        break;

        // 二重引用符は文字列の終端と区別できるようにする。
        case '"': escaped += "\\\"";
        break;

        // LFをエスケープし、変換要求がJSONLの途中で改行されないようにする。
        case '\n': escaped += "\\n";
        break;

        // CRも改行文字としてエスケープする。
        case '\r': escaped += "\\r";
        break;

        // タブをJSON文字列に記録できる表記にする。
        case '\t': escaped += "\\t";
        break;

        // 上記以外のバイトはそのまま出力する。
        default: escaped += c;
        break;
        }
    }

    return escaped;
}

void AppendDdsCacheRequest(const std::string& filePath, bool isNormalMap, float durationMs) {
    // 読み込んだ元画像に対応するDDS生成要求をJSONLへ追記し、次回以降の読み込みを軽くする。
    const std::filesystem::path sourcePath = PathFromUtf8(filePath);
    const std::string ext = ToLower(sourcePath.extension().string());

    // 元画像扱いでない拡張子と、エディターの一時プレビュー画像はDDS生成の対象から外す。
    if (!IsSourceTextureExtension(ext) || IsEditorPreviewTexture(filePath)) {
        return;
    }

    // 読み込みが軽い画像はDDS化しても効果が薄いため、変換キューへ積まない。
    if (durationMs < kDdsCacheRequestMinDurationMs) {
        return;
    }

    static std::set<std::string> sRequestedPaths;
    const std::string source = ToProjectPath(sourcePath);

    // 同じ元画像パスをこのプロセスで既に受け付けていれば、要求を重複登録しない。
    if (!sRequestedPaths.insert(source).second) {
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(kDdsCacheRequestPath.parent_path(), ec);

    // 出力先ディレクトリを用意できなければ、要求ファイルへの追記に進まない。
    if (ec) {
        return;
    }

    std::filesystem::path ddsPath = sourcePath;
    ddsPath.replace_extension(".dds");

    std::ofstream ofs(kDdsCacheRequestPath, std::ios::app);

    // 要求ファイルを追記用に開けなければ、監視の起動処理に進まない。
    if (!ofs) {
        return;
    }

    // 元画像とDDSのパス、圧縮形式、計測された読み込み時間を1行で渡す。
    ofs << "{\"source\":\"" << EscapeJson(source)
        << "\",\"dds\":\"" << EscapeJson(ToProjectPath(ddsPath))
        << "\",\"format\":\"" << EscapeJson(GetDdsFormat(filePath, isNormalMap))
        << "\",\"durationMs\":" << durationMs
        << "}\n";

    StartDdsCacheWatcherIfNeeded();
}

std::string GetTextureBasePath(const std::filesystem::path& path) {
    // 拡張子を除いたテクスチャ基準パスを作り、元画像とDDSの対応確認に使う。
    std::filesystem::path base = path.parent_path() / path.stem();
    return NormalizeTexturePath(PathToUtf8(base));
}
}

// CPU側で読み込んだミップ画像をGPUテクスチャへ転送し、シェーダーから読める状態へ遷移させる。

void UploadTextureData(
    ID3D12Resource* texture,
    const DirectX::ScratchImage& mipImages,
    ID3D12Resource** intermediateResource,
    ID3D12Device* device,
    ID3D12GraphicsCommandList* commandList,
    D3D12_RESOURCE_STATES finalState) {
    // ミップごとの転送情報をDirectXTexから作成する。
    std::vector<D3D12_SUBRESOURCE_DATA> subresources;
    HRESULT hr = DirectX::PrepareUpload(device, mipImages.GetImages(), mipImages.GetImageCount(), mipImages.GetMetadata(), subresources);
    assert(SUCCEEDED(hr));

    // GPUが要求する行・サブリソースの配置に合わせ、画像データの総バイト数に整列パディングを含めます。
    uint64_t intermediateSize = GetRequiredIntermediateSize(texture, 0, UINT(subresources.size()));

    D3D12_HEAP_PROPERTIES uploadHeapProperties{};
    uploadHeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = intermediateSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    // コピー先に合わせた容量を、CPUが画素を書き込めるUploadヒープに確保する。
    hr = device->CreateCommittedResource(
        &uploadHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(intermediateResource));
    assert(SUCCEEDED(hr));

    // 画像データを中間バッファ経由でコピーするコマンドを記録する。
    UpdateSubresources(commandList, texture, *intermediateResource, 0, 0, UINT(subresources.size()), subresources.data());

    // 転送後は呼び出し元の指定状態へ遷移します。Copy Queue用ではCOMMONへ戻し、同期Direct Queue用では読み取り状態にします。
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = texture;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = finalState;
    commandList->ResourceBarrier(1, &barrier);
}

std::string MakeTextureCacheKey(const std::string& path, bool isLinearTexture) {
    // 同じ画像でも線形色空間とsRGBではGPU上の解釈が異なるため、別エントリにする。
    return NormalizeTexturePath(path) + (isLinearTexture ? "|linear" : "|srgb");
}

// 元画像、実際の読込先、キャッシュ識別子を分離する。DDS を使っても元画像の参照を失わない。
struct ResolvedTexturePath {
    // 直接DDSを指定したときに元画像への代替も考慮した、今回の基準パス。
    std::string effectiveFilePath;
    // 同名DDSの更新時刻まで比較した結果、実際にデコードするパス。
    std::string loadPath;
    std::string cacheKey;
    bool isLinearTexture = false;
    bool useDdsCache = false;
    bool isSourceTexture = false;
    bool ddsIsUpToDate = false;
};

// 元画像とDDSの有無・更新時刻・色空間から実際の読込先とキャッシュキーを決めます。
ResolvedTexturePath ResolveTexturePath(
    const std::string& filePath,
    TextureManager::TextureColorSpace colorSpace,
    bool allowDdsCache) {
    // DDSの欠落・古さを確認し、必要なら元画像へ読み込み先を戻す。
    ResolvedTexturePath resolved;
    // 線形を明示された場合と、自動判定で法線やマスクなどの名前に一致した場合に線形として扱います。
    resolved.isLinearTexture = colorSpace == TextureManager::TextureColorSpace::kLinear ||
        (colorSpace == TextureManager::TextureColorSpace::kAuto && IsLinearTexturePath(filePath));

    std::filesystem::path path = PathFromUtf8(filePath);
    resolved.useDdsCache = allowDdsCache && !IsDdsCacheDisabled();
    const bool requestedDds = ToLower(path.extension().string()) == ".dds";

    // DDSの直接指定では、比較対象になる元画像を先に探す。
    if (requestedDds) {
        const std::filesystem::path sourcePath = FindSourceTextureForDds(path);

        // 対応する元画像が見つかった場合だけ、DDSの有効性と更新時刻を比較する。
        if (!sourcePath.empty()) {
            const bool ddsMissing = !std::filesystem::exists(path);
            const bool ddsOutdated = !ddsMissing &&
                std::filesystem::last_write_time(sourcePath) > std::filesystem::last_write_time(path);
            // DDSキャッシュが無効、DDSが未配置、またはDDSが元画像より古ければ、元画像を使う。
            if (!resolved.useDdsCache || ddsMissing || ddsOutdated) {
                path = sourcePath;
            }
        }
    }

    resolved.effectiveFilePath = NormalizeTexturePath(PathToUtf8(path));
    // 色空間が自動判定なら、実際に選び直したパスに含まれる用途からも線形画像か判定する。
    if (colorSpace == TextureManager::TextureColorSpace::kAuto &&
        IsLinearTexturePath(resolved.effectiveFilePath)) {
        resolved.isLinearTexture = true;
    }

    const std::string extension = ToLower(path.extension().string());
    resolved.isSourceTexture = IsSourceTextureExtension(extension);
    std::filesystem::path ddsPath = path;
    ddsPath.replace_extension(".dds");
    resolved.loadPath = resolved.effectiveFilePath;

    // DDSを利用できる設定で元画像の拡張子に該当し、同名のDDSが存在する場合に切り替えを検討する。
    if (resolved.useDdsCache && resolved.isSourceTexture && std::filesystem::exists(ddsPath)) {
        // 元画像がなくても、対応するDDSだけがあればそれを使う。
        if (!std::filesystem::exists(path)) {
            resolved.loadPath = NormalizeTexturePath(PathToUtf8(ddsPath));
            resolved.ddsIsUpToDate = true;
        }
        // 元画像も存在する場合は、双方の更新時刻を比較する。
        else {
            const auto sourceTime = std::filesystem::last_write_time(path);
            const auto ddsTime = std::filesystem::last_write_time(ddsPath);

            // DDSの時刻が元画像と同じか新しければキャッシュを使う。
            if (sourceTime <= ddsTime) {
                resolved.loadPath = NormalizeTexturePath(PathToUtf8(ddsPath));
                resolved.ddsIsUpToDate = true;
            }
        }
    }

    // 実際の読み込み先がHDR画像なら、指定された色空間にかかわらず線形として扱う。
    if (ToLower(PathFromUtf8(resolved.loadPath).extension().string()) == ".hdr") {
        resolved.isLinearTexture = true;
    }

    // 読み込み先と線形指定を組にし、同じ画像を異なる色空間で読む要求を区別する。
    resolved.cacheKey = MakeTextureCacheKey(resolved.loadPath, resolved.isLinearTexture);
    return resolved;
}

// DDSを読めなかった場合だけ元画像を試す。白画像の寸法では成否を判定しない。
DirectX::ScratchImage DecodeResolvedTexture(const ResolvedTexturePath& resolved, bool forceSRGB) {
    bool loadedFromFile = false;
    auto image = engine::DirectXCommon::LoadTexture(resolved.loadPath, forceSRGB, &loadedFromFile);
    // デコードに成功した場合と、DDS以外の読み込みに失敗した場合は、その結果をそのまま返す。
    if (loadedFromFile || ToLower(PathFromUtf8(resolved.loadPath).extension().string()) != ".dds") {
        return image;
    }

    // PNG指定からDDSを選んだ場合は、別形式の同名画像より指定された元画像を優先する。
    std::filesystem::path sourcePath = PathFromUtf8(resolved.effectiveFilePath);
    // 指定パスが元画像の形式でなければ、選ばれたDDSに対応する元画像を探す。
    if (!IsSourceTextureExtension(sourcePath.extension().string())) {
        sourcePath = FindSourceTextureForDds(PathFromUtf8(resolved.loadPath));
    }
    // 元画像を特定できなければ、最初の読み込みで返った代替画像を使う。
    if (sourcePath.empty()) {
        return image;
    }

    // 元画像を特定できた場合は、色空間の指定を維持したままデコードを試し直します。
    const std::string source = PathToUtf8(sourcePath);
    OutputDebugStringA(("DDS texture load failed; retrying source: " + resolved.loadPath + " -> " + source + "\n").c_str());
    return engine::DirectXCommon::LoadTexture(source, forceSRGB);
}

// 実行中の描画コマンドリストを触らず、テクスチャ転送専用の一時コマンドリストでGPUへ送る。
bool UploadTextureDataWithDedicatedCommandList(
    ID3D12Resource* texture,
    const DirectX::ScratchImage& mipImages,
    ID3D12Device* device,
    ID3D12CommandQueue* commandQueue) {
    // 転送先、デバイス、投入するキューが一つでもなければ転送を開始できない。
    if (!texture || !device || !commandQueue) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> uploadAllocator;
    // 描画と同じDirect Queueへ投入しますが、レコーダーは分けて現在の描画コマンドを閉じずに転送できます。
    HRESULT hr = device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(uploadAllocator.GetAddressOf()));
    // コマンド記録領域を確保できなければ、一時リストを作れない。
    if (FAILED(hr)) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> uploadCommandList;
    hr = device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        uploadAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(uploadCommandList.GetAddressOf()));
    // 転送専用リストを作れなければ、画像のコピーは記録しない。
    if (FAILED(hr)) {
        return false;
    }

    // 全ミップを転送するコマンドを記録し、転送完了まで使う中間リソースを保持する。
    Microsoft::WRL::ComPtr<ID3D12Resource> intermediateResource;
    UploadTextureData(
        texture,
        mipImages,
        intermediateResource.GetAddressOf(),
        device,
        uploadCommandList.Get(),
        D3D12_RESOURCE_STATE_GENERIC_READ);

    hr = uploadCommandList->Close();

    // 記録を閉じられなければ、不完全なリストをキューへ投入しない。
    if (FAILED(hr)) {
        return false;
    }

    ID3D12CommandList* commandLists[] = { uploadCommandList.Get() };
    commandQueue->ExecuteCommandLists(1, commandLists);

    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()));

    // 完了通知用のフェンスを作れなければ失敗を返す。転送リストは既に投入されている。
    if (FAILED(hr)) {
        return false;
    }

    constexpr UINT64 kFenceValue = 1;
    hr = commandQueue->Signal(fence.Get(), kFenceValue);

    // 投入済みのコピー後に完了値を書き込むよう指示できなければ、失敗を返す。
    if (FAILED(hr)) {
        return false;
    }

    // 転送完了値にGPUが未到達の場合だけ、イベントを作成してCPUを待たせる。
    if (fence->GetCompletedValue() < kFenceValue) {
        HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

        // 待機イベントを作れなければ、完了の通知を受けられないため失敗を返す。
        if (!fenceEvent) {
            return false;
        }

        hr = fence->SetEventOnCompletion(kFenceValue, fenceEvent);

        // 完了時にイベントを通知するよう登録できなければ、ハンドルを閉じて失敗を返す。
        if (FAILED(hr)) {
            CloseHandle(fenceEvent);
            return false;
        }

        const auto waitStart = std::chrono::steady_clock::now();
        WaitForSingleObject(fenceEvent, INFINITE);
        const auto waitMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - waitStart).count();
        LogSlowLoadingOperation("synchronous texture upload GPU wait", waitMs);
        CloseHandle(fenceEvent);
    }

    return true;
}

TextureManager* TextureManager::GetInstance() {
    // TextureManagerのシングルトンインスタンスを返す。
    static TextureManager sInstance;
    return &sInstance;
}

void TextureManager::Initialize(engine::DirectXCommon* dxCommon) {
    // DirectXCommonからデバイスを受け取り、テクスチャ生成に必要な参照を保持する。
    assert(dxCommon);
    dxCommon_ = dxCommon;
    device_ = dxCommon->GetDevice();

    // 描画キューと分けてテクスチャ転送用のキューを用意する。フェンスを作れない場合は転送キューも使わない。
    D3D12_COMMAND_QUEUE_DESC copyQueueDesc{};
    copyQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    copyQueueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    copyQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    copyQueueDesc.NodeMask = 0;

    HRESULT hr = device_->CreateCommandQueue(
        &copyQueueDesc,
        IID_PPV_ARGS(copyCommandQueue_.GetAddressOf()));
    assert(SUCCEEDED(hr));

    // 転送キューの作成に失敗した場合は、非同期転送を初期化できないため終了する。
    if (FAILED(hr)) {
        copyCommandQueue_.Reset();
        return;
    }

    hr = device_->CreateFence(
        0,
        D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(copyFence_.GetAddressOf()));
    assert(SUCCEEDED(hr));

    // フェンスを作れなければ転送完了を確認できないため、転送キューも使わない。
    if (FAILED(hr)) {
        copyCommandQueue_.Reset();
        copyFence_.Reset();
    }
}

bool TextureManager::BeginAsyncUploadBatch() {
    // 転送用の資源が不足しているか、前回の投入が残っていれば新規バッチを開けない。
    if (!device_ || !copyCommandQueue_ || !copyFence_ || asyncUploadBatchSubmitted_) {
        return false;
    }

    // 有効な未投入バッチが既にあれば、再作成せずそのまま使う。
    if (asyncUploadBatchActive_) {
        return true;
    }

    asyncTextureUploads_.clear();
    asyncUploadAllocator_.Reset();
    asyncUploadCommandList_.Reset();

    // 前回の提出が残っていない場合だけ、今回のCopy用リストと記録領域を準備する。
    HRESULT hr = device_->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_COPY,
        IID_PPV_ARGS(asyncUploadAllocator_.GetAddressOf()));
    // コピー用の記録領域を確保できなければバッチを開始しない。
    if (FAILED(hr)) {
        return false;
    }

    hr = device_->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_COPY,
        asyncUploadAllocator_.Get(),
        nullptr,
        IID_PPV_ARGS(asyncUploadCommandList_.GetAddressOf()));
    // コマンドリストを確保できなければ、今回の記録領域も手放して失敗にする。
    if (FAILED(hr)) {
        asyncUploadAllocator_.Reset();
        return false;
    }

    // リストが揃った時点で、新しいバッチの記録状態を初期化する。
    pendingCopyFenceValue_ = 0;
    asyncUploadBatchActive_ = true;
    sAsyncUploadRecording = false;
    asyncUploadBatchSubmitted_ = false;
    return true;
}

thread_local bool TextureManager::sAsyncUploadRecording = false;

// 呼び出したスレッドで、未投入バッチへのコピー記録を許可または禁止します。
void TextureManager::SetAsyncUploadRecording(bool enabled) {
    sAsyncUploadRecording = enabled && asyncUploadBatchActive_ && !asyncUploadBatchSubmitted_;
}

bool TextureManager::RecordAsyncTextureUpload(
    ID3D12Resource* texture,
    const DirectX::ScratchImage& mipImages) {
    // 転送先や記録資源がない、バッチが無効・投入済み、または現在のスレッドで記録が禁止なら失敗を返す。
    if (!texture || !device_ || !asyncUploadCommandList_ ||
        !asyncUploadBatchActive_ || !sAsyncUploadRecording || asyncUploadBatchSubmitted_) {
        return false;
    }

    AsyncTextureUpload upload;
    upload.destination = texture;
    UploadTextureData(
        texture,
        mipImages,
        upload.intermediate.GetAddressOf(),
        device_.Get(),
        asyncUploadCommandList_.Get(),
        D3D12_RESOURCE_STATE_COMMON);
    // 転送用の中間リソースが作られなければ、今回の画像を保持一覧に登録しない。
    if (!upload.intermediate) {
        return false;
    }

    // 送信先と転送用の中間バッファをこのバッチに保持します。Copy Queueのフェンス到達後にまとめて解放します。
    asyncTextureUploads_.push_back(std::move(upload));
    return true;
}

bool TextureManager::SubmitAsyncUploadBatch() {
    // 実行後のフェンス値を記録し、非同期転送の完了判定に使う。
    // 有効なバッチがなければ投入するコマンドがないため、処理済みとして返す。
    if (!asyncUploadBatchActive_) {
        return true;
    }

    // 既にコマンドを投入していれば、同じリストを再投入しない。
    if (asyncUploadBatchSubmitted_) {
        return true;
    }

    // リスト、コピーキュー、フェンスのどれかがなければ投入できない。
    if (!asyncUploadCommandList_ || !copyCommandQueue_ || !copyFence_) {
        return false;
    }

    sAsyncUploadRecording = false;
    HRESULT hr = asyncUploadCommandList_->Close();

    // コマンドリストを閉じられなければ、実行へ進まない。
    if (FAILED(hr)) {
        return false;
    }

    ID3D12CommandList* commandLists[] = { asyncUploadCommandList_.Get() };
    copyCommandQueue_->ExecuteCommandLists(1, commandLists);

    // 転送コマンドを積んだ後で同じキューへ完了の目印を積み、CPU側はこの値を実行済みかの判定に使います。
    pendingCopyFenceValue_ = ++copyFenceValue_;
    hr = copyCommandQueue_->Signal(copyFence_.Get(), pendingCopyFenceValue_);

    // 完了値の通知を予約できなければ、完了待ちに使う値を消し、投入済みの印は立てずに失敗を返す。
    if (FAILED(hr)) {
        pendingCopyFenceValue_ = 0;
        return false;
    }

    asyncUploadBatchSubmitted_ = true;
    return true;
}

bool TextureManager::PollAsyncUploadBatch() {
    // 有効なバッチがない場合は、待っている転送がないため完了扱いにする。
    if (!asyncUploadBatchActive_) {
        return true;
    }

    // 未投入か、完了を確認するフェンスがなければ完了扱いにできない。
    if (!asyncUploadBatchSubmitted_ || !copyFence_) {
        return false;
    }

    // フェンス値に未到達なら中間リソースを保持し、次回の呼び出しで再判定する。
    if (copyFence_->GetCompletedValue() < pendingCopyFenceValue_) {
        return false;
    }

    // GPUが転送を完了したら、バッチが保持していた資源をまとめて解放する。
    ReleaseCompletedAsyncUploadBatch();
    return true;
}

void TextureManager::WaitForAsyncUploadBatch() {
    // 同期が必要な呼び出し元では、Copy Queue完了を待って転送資源を片付ける。
    // 有効なバッチがなければ、投入や待機を行う必要がない。
    if (!asyncUploadBatchActive_) {
        return;
    }

    // 未投入のバッチを投入できなかった場合は、保持していた転送資源を片付けて終了する。
    if (!asyncUploadBatchSubmitted_ && !SubmitAsyncUploadBatch()) {
        ReleaseCompletedAsyncUploadBatch();
        return;
    }

    // フェンスが存在し、転送完了値に未到達であれば、イベントによる待機を試す。
    if (copyFence_ && copyFence_->GetCompletedValue() < pendingCopyFenceValue_) {
        HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

        // イベントを作成できた場合だけ、完了値での通知を要求する。
        if (fenceEvent) {
            const HRESULT hr = copyFence_->SetEventOnCompletion(
                pendingCopyFenceValue_,
                fenceEvent);
            // GPU完了時の通知を登録できた場合に限り、イベントを待つ。
            if (SUCCEEDED(hr)) {
                const auto waitStart = std::chrono::steady_clock::now();
                WaitForSingleObject(fenceEvent, INFINITE);
                const auto waitMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - waitStart).count();
                LogSlowLoadingOperation("async texture upload GPU wait", waitMs);
            }

            CloseHandle(fenceEvent);
        }
    }

    // 待機処理を抜けたら、バッチの状態と保持資源を片付ける。
    ReleaseCompletedAsyncUploadBatch();
}

bool TextureManager::IsAsyncUploadBatchPending() const {
    // 提出前の記録中も未完了として扱い、二重開始を防ぐ。
    return asyncUploadBatchActive_;
}

void TextureManager::ReleaseCompletedAsyncUploadBatch() {
    // バッチの記録・投入状態を初期値へ戻し、保持していたコピー用資源を解放する。
    sAsyncUploadRecording = false;
    asyncUploadBatchSubmitted_ = false;
    asyncUploadBatchActive_ = false;
    pendingCopyFenceValue_ = 0;
    asyncTextureUploads_.clear();
    asyncUploadCommandList_.Reset();
    asyncUploadAllocator_.Reset();
}

uint32_t TextureManager::Load(const std::string& filePath, bool isNormalMap, bool allowDdsCache, bool forceReload) {
    // テクスチャを読み込み、必要ならDDSキャッシュを優先しながらSRVハンドルとして登録する。
    return LoadInternal(
        filePath,
        isNormalMap ? TextureColorSpace::kLinear : TextureColorSpace::kAuto,
        allowDdsCache,
        forceReload);
}

uint32_t TextureManager::Load(
    const std::string& filePath,
    TextureColorSpace colorSpace,
    bool allowDdsCache,
    bool forceReload) {
    // 色空間の指定を保ったまま共通の読込経路へ渡す。
    return LoadInternal(filePath, colorSpace, allowDdsCache, forceReload);
}

bool TextureManager::Prepare(
    const std::string& filePath,
    TextureColorSpace colorSpace,
    bool allowDdsCache) {
    // デコードはGPU転送より先に行い、同じキャッシュキーの準備済み画像は再利用する。
    const ResolvedTexturePath resolved = ResolveTexturePath(filePath, colorSpace, allowDdsCache);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 同じキーでSRVまたは先読み画像が既にあれば、デコードを増やさずに準備成功とする。
        if (textureHandleMap_.contains(resolved.cacheKey) ||
            preparedTextures_.contains(resolved.cacheKey)) {
            return true;
        }
    }

    const auto start = std::chrono::high_resolution_clock::now();
    auto prepared = std::make_unique<PreparedTextureData>();
    prepared->mipImages = DecodeResolvedTexture(resolved, !resolved.isLinearTexture);
    const auto end = std::chrono::high_resolution_clock::now();
    prepared->decodeDurationMs = std::chrono::duration<float, std::milli>(end - start).count();

    // 代替画像も含めて取得した画像が0枚なら、先読みデータを登録できない。
    if (prepared->mipImages.GetImageCount() == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    // ロックを外してデコードしていた間に同じキーが登録されていれば、先に完成したデータを使う。
    if (textureHandleMap_.contains(resolved.cacheKey) ||
        preparedTextures_.contains(resolved.cacheKey)) {
        return true;
    }

    // 再確認後も登録が空いている場合は、デコード済み画像を次のロードへ引き渡す。
    preparedTextures_.emplace(resolved.cacheKey, std::move(prepared));
    return true;
}

// 画像をデコードしてGPU転送を同期実行または予約し、シェーダー参照用のSRV番号を返します。リソース作成や転送に失敗した場合は0を返します。
uint32_t TextureManager::LoadInternal(
    const std::string& filePath,
    TextureColorSpace colorSpace,
    bool allowDdsCache,
    bool forceReload) {
    const ResolvedTexturePath resolved = ResolveTexturePath(filePath, colorSpace, allowDdsCache);

    // 通常ロードではデコード前に登録済みSRVを再利用する。
    if (!forceReload) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = textureHandleMap_.find(resolved.cacheKey);

        // 同じ読み込み先と色空間のSRVが登録済みなら、再読み込みせず番号を返す。
        if (it != textureHandleMap_.end()) {
            return it->second;
        }
    }

    const auto start = std::chrono::high_resolution_clock::now();

    // 先読み済み画像を取り出し、なければ色空間の指定を保ってデコードする。
    const bool forceSRGB = !resolved.isLinearTexture;
    DirectX::ScratchImage mipImages;
    float preparedDecodeDurationMs = 0.0f;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto prepared = preparedTextures_.find(resolved.cacheKey);

        // 先読み画像が見つかれば、所要時間と画素を取り出し、準備中の一覧から外す。
        if (prepared != preparedTextures_.end()) {
            preparedDecodeDurationMs = prepared->second->decodeDurationMs;
            mipImages = std::move(prepared->second->mipImages);
            preparedTextures_.erase(prepared);
        }
    }

    // 先読み画像がなければ、その場で指定された色空間に沿ってデコードする。
    if (mipImages.GetImageCount() == 0) {
        mipImages = DecodeResolvedTexture(resolved, forceSRGB);
    }

    // デコード済みの寸法・形式・ミップ数から、GPUの保存領域とビューの設定を決めます。
    const DirectX::TexMetadata& metadata = mipImages.GetMetadata();
    // Copy Queueへ渡す画像はCOMMONから開始し、同期転送は従来のCOPY_DESTを維持します。
    const bool useCopyQueue = sAsyncUploadRecording;
    const D3D12_RESOURCE_STATES initialState = useCopyQueue
        ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_COPY_DEST;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource = dxCommon_->CreateTextureResource(metadata, initialState);

    // GPU上のテクスチャを確保できなければ、転送やSRV登録を行わない。
    if (!resource) {
        return 0;
    }

    // 現在のスレッドで非同期記録が許可されているときは、Copy Queue用のバッチへ転送を記録する。
    if (useCopyQueue) {
        // コピーの記録を行えなかった場合は、SRV番号を払い出さない。
        if (!RecordAsyncTextureUpload(resource.Get(), mipImages)) {
            return 0;
        }
    }
    // 同期経路では専用のDirectリストで転送を試す。失敗した場合はSRV登録に進まない。
    else if (!UploadTextureDataWithDedicatedCommandList(
                 resource.Get(),
                 mipImages,
                 device_.Get(),
                 dxCommon_->GetCommandQueue())) {
        return 0;
    }

    // 転送を記録できた画像だけを、通常画像またはキューブマップとして登録する。
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = metadata.format;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    // 同じ画像データでもキューブマップは六面を扱うビューにし、通常の2D画像とシェーダーからの参照方法を分けます。
    if (metadata.IsCubemap()) {
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srvDesc.TextureCube.MostDetailedMip = 0;
        srvDesc.TextureCube.MipLevels = UINT(metadata.mipLevels);
        srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
    // キューブマップではない2D画像には、通常の2Dビューとそのミップ数を指定する。
    } else if (metadata.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = UINT(metadata.mipLevels);
    }

    uint32_t srvHandle = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto existingIt = textureHandleMap_.find(resolved.cacheKey);

        // 通常読み込みで別スレッドが同じキーを先に登録していた場合は、そのSRV番号を返す。
        if (!forceReload && existingIt != textureHandleMap_.end()) {
            return existingIt->second;
        }

        // forceReload時は既存SRV番号を再利用し、参照しているSpriteやMaterialを壊さない。
        // 強制再読み込みで既存の番号があれば、同じビューを新しい画像へ向け直す。
        if (forceReload && existingIt != textureHandleMap_.end()) {
            srvHandle = existingIt->second;
            SrvManager::GetInstance()->CreateSrvForResource(srvHandle, resource.Get(), srvDesc);
        // ここまでに再利用できる番号がなければ、SRVを新規に確保する。
        } else {
            srvHandle = SrvManager::GetInstance()->CreateSrv(resource.Get(), srvDesc);
        }

        // 参照されるSRV番号に画像とメタデータを保持し、次回から同じキーで探せるようにする。
        TextureData& newData = textureDatas_[srvHandle];
        newData.filePath = resolved.loadPath;
        newData.metadata = metadata;
        newData.resource = resource;
        newData.srvHandle = srvHandle;

        textureHandleMap_[resolved.cacheKey] = srvHandle;
    }

    // 先読みがあった場合はデコード時間を今回の転送準備時間に足し、ミリ秒で記録します。
    const auto end = std::chrono::high_resolution_clock::now();
    float duration = preparedDecodeDurationMs +
        std::chrono::duration<float, std::milli>(end - start).count();
    ProfilerManager::GetInstance()->RecordLoadTime("Sprite", filePath, duration);

    // まだ有効なDDSがない元画像は、裏側でDDS生成できるよう要求を積む。
    if (resolved.useDdsCache && resolved.isSourceTexture && !resolved.ddsIsUpToDate) {
        AppendDdsCacheRequest(resolved.effectiveFilePath, resolved.isLinearTexture, duration);
    }

    return srvHandle;
}

std::optional<uint32_t> TextureManager::AcquireVideoSurface(const void* owner) {
    std::lock_guard lock(mutex_);
    // 動画用の転送先は一つだけ共有する。同じ再生元は再取得でき、別の再生元との同時利用は拒む。
    if (!owner || !device_ || (videoOwner_ && videoOwner_ != owner))
        return std::nullopt;
    // 動画用の共有ハンドルをまだ持たない場合だけ、640×360の画像と転送資源を用意する。
    if (!videoHandle_) {
        constexpr UINT kWidth = 640;
        constexpr UINT kHeight = 360;
        constexpr auto kFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        D3D12_HEAP_PROPERTIES gpuHeap{};
        gpuHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC texture{};
        texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width = kWidth;
        texture.Height = kHeight;
        texture.DepthOrArraySize = texture.MipLevels = 1;
        texture.Format = kFormat;
        texture.SampleDesc.Count = 1;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        // 動画を表示するGPU画像を確保できなければ、使用権を渡さない。
        if (FAILED(device_->CreateCommittedResource(&gpuHeap, D3D12_HEAP_FLAG_NONE, &texture,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&resource))))
            return std::nullopt;
        UINT64 bytes = 0;
        // 動画の各行をGPUの整列幅で扱えるように、コピー領域のサイズと配置を先に取得します。
        device_->GetCopyableFootprints(&texture, 0, 1, 0, &videoFootprint_, nullptr, nullptr, &bytes);
        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC upload{};
        upload.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        upload.Width = bytes;
        upload.Height = upload.DepthOrArraySize = upload.MipLevels = 1;
        upload.SampleDesc.Count = 1;
        upload.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        // CPUが書ける転送バッファを確保できなければ、共有ハンドルも登録しない。
        if (FAILED(device_->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &upload,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&videoUpload_))))
            return std::nullopt;
        // GPU画像と転送バッファの確保後、Spriteなどから読むためのSRVを登録する。
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = kFormat;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        const auto handle = SrvManager::GetInstance()->CreateSrv(resource.Get(), view);
        TextureData data{};
        data.metadata.width = kWidth;
        data.metadata.height = kHeight;
        data.metadata.depth = data.metadata.arraySize = data.metadata.mipLevels = 1;
        data.metadata.format = kFormat;
        data.metadata.dimension = DirectX::TEX_DIMENSION_TEXTURE2D;
        data.resource = std::move(resource);
        data.srvHandle = handle;
        textureDatas_.emplace(handle, std::move(data));
        videoHandle_ = handle;
    }
    // 画像・転送バッファ・SRVを準備できた段階で、今回の再生元に使用権を渡す。
    videoOwner_ = owner;
    return videoHandle_;
}

// 現在使っている再生元だけが使用権を返せる。テクスチャとSRVは次回の動画へ再利用する。
void TextureManager::ReleaseVideoSurface(const void* owner) {
    std::lock_guard lock(mutex_);
    // 使用権を持つ再生元と一致したときだけ、他の再生元が取得できる状態にする。
    if (videoOwner_ == owner)
        videoOwner_ = nullptr;
}

// 共有の動画テクスチャへBGRA画素をコピーするコマンドを記録します。記録できた場合にtrueを返します。
bool TextureManager::UploadVideoSurface(const void* owner, ID3D12GraphicsCommandList& commands,
                                        std::span<const uint8_t> pixels) {
    std::lock_guard lock(mutex_);
    // 現在の再生元が使用権を持ち、必要なGPUテクスチャとDirectXの参照がある場合だけ書き込める。
    if (!videoHandle_ || !dxCommon_ || !owner || videoOwner_ != owner)
        return false;
    // 動画は同じ転送領域を使い回します。前フレームのコピーが領域を読み終えるまで新しい画素を書き込みません。
    if (videoUploadFence_ && dxCommon_->GetFence()->GetCompletedValue() < *videoUploadFence_)
        return false;
    const auto& footprint = videoFootprint_.Footprint;
    const size_t rowBytes = static_cast<size_t>(footprint.Width) * 4;
    // 入力が想定した幅・高さの4バイト画素と一致しなければ、転送領域を変更しない。
    if (pixels.size() != rowBytes * footprint.Height)
        return false;
    void* mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    // 転送バッファをCPUから書き込めなければ、GPUへのコピーを記録しない。
    if (FAILED(videoUpload_->Map(0, &noRead, &mapped)))
        return false;
    // 入力の1行は幅×4バイトです。GPU側の整列された行間隔を使い、各行の有効なピクセルだけをコピーします。
    auto* destination = static_cast<uint8_t*>(mapped) + videoFootprint_.Offset;
    for (UINT row = 0; row < footprint.Height; ++row)
        std::memcpy(destination + row * footprint.RowPitch, pixels.data() + row * rowBytes, rowBytes);
    videoUpload_->Unmap(0, nullptr);
    // 転送バッファへ書き終えたら、GPU画像をコピー先へ遷移させて画素を転送する。
    auto* resource = textureDatas_.at(*videoHandle_).resource.Get();
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST};
    commands.ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = videoUpload_.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = videoFootprint_;
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = resource;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    commands.CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    // コピーを記録した後、画像をピクセルシェーダーから参照できる状態へ戻す。
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    commands.ResourceBarrier(1, &barrier);
    // 今回のコピーを含むグラフィックスキューの次回Signalを、転送領域の再利用条件にします。
    videoUploadFence_ = dxCommon_->GetFenceValue() + 1;
    return true;
}

const DirectX::TexMetadata& TextureManager::GetMetadata(uint32_t textureHandle) {
    // 読み込み済みテクスチャのメタデータを取得し、サイズやミップ情報を参照できるようにする。
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = textureDatas_.find(textureHandle);
    assert(it != textureDatas_.end());
    return it->second.metadata;
}

void TextureManager::LoadAllTexture(const std::string& directoryPath) {
    // 指定フォルダーを再帰的に走査し、画像をまとめて読み込みます。
    // フォルダーが存在しなければ、列挙を開始しない。
    if (!std::filesystem::exists(PathFromUtf8(directoryPath))) {
        return;
    }

    // 通常ファイルを列挙し、元画像が存在する基準名を先に集めてDDSの二重登録を避けます。
    std::vector<std::filesystem::path> files;
    std::set<std::string> sourceBaseNames;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(PathFromUtf8(directoryPath))) {
        // ディレクトリなど通常ファイル以外の項目は、画像候補から外す。
        if (!entry.is_regular_file()) {
            continue;
        }

        std::filesystem::path path = entry.path();
        files.push_back(path);

        const std::string ext = ToLower(path.extension().string());

        // 対応形式の元画像だった場合にだけ、同名DDSと照合するための基準名を記録する。
        if (IsSourceTextureExtension(ext) && !IsEditorPreviewTexture(PathToUtf8(path))) {
            sourceBaseNames.insert(GetTextureBasePath(path));
        }
    }

    // ファイルシステムの列挙順に依存しないよう、パス順に揃えてから読み込む。
    std::sort(files.begin(), files.end(), [](const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
        return PathToUtf8(lhs) < PathToUtf8(rhs);
    });

    for (const auto& path : files) {
        const std::string pathString = NormalizeTexturePath(PathToUtf8(path));

        // 一時プレビュー画像は、一括読み込みの対象にしない。
        if (IsEditorPreviewTexture(pathString)) {
            continue;
        }

        const std::string ext = ToLower(path.extension().string());

        // DDSは対応する元画像が一覧にあるかを確かめる。
        if (ext == ".dds") {
            // 元画像があるDDSは元画像側のLoadに選択を任せ、一括読み込みで重複登録しないようにします。
            if (sourceBaseNames.count(GetTextureBasePath(path)) != 0) {
                continue;
            }

            Load(pathString, false);
            continue;
        }

        // DDS以外は対応する元画像形式だけを読み込み、名前から線形扱いかを判定する。
        if (IsSourceTextureExtension(ext)) {
            Load(pathString, IsLinearTexturePath(pathString));
        }
    }
}

std::vector<std::string> TextureManager::GetLoadedTexturePaths() const {
    // 現在読み込み済みのテクスチャパス一覧を返し、エディタやデバッグ表示で利用する。
    std::lock_guard<std::mutex> lock(mutex_);

    std::set<std::string> uniquePaths;

    // 同じ読み込み先を色空間違いで登録していても、表示には一つのパスとして返します。
    for (const auto& pair : textureDatas_) {
        uniquePaths.insert(pair.second.filePath);
    }

    return { uniquePaths.begin(), uniquePaths.end() };
}

std::vector<std::string> TextureManager::GetAvailableTexturePaths() const {
    // 読込済み画像に加え、素材フォルダ内の未読込画像も選択肢へ含める。
    std::set<std::string> uniquePaths;

    for (const std::string& loadedPath : GetLoadedTexturePaths()) {
        uniquePaths.insert(NormalizeTexturePath(loadedPath));
    }

    const std::filesystem::path roots[] = {
        "Resources/sprite",
        "Resources/texture"
    };

    std::error_code error;

    for (const std::filesystem::path& root : roots) {
        // 素材フォルダーが無いか存在を確認できなければ、そのフォルダーの列挙を省略する。
        if (!std::filesystem::exists(root, error) || error) {
            error.clear();
            continue;
        }

        for (std::filesystem::recursive_directory_iterator iterator(
                 root,
                 std::filesystem::directory_options::skip_permission_denied,
                 error), end;
             iterator != end;
             iterator.increment(error)) {
            // 列挙中にエラーが報告された項目は、次の項目を調べられるようエラーをクリアして飛ばす。
            if (error) {
                error.clear();
                continue;
            }

            // 通常ファイルでない項目と、種類を確認できない項目は画像候補に含めない。
            if (!iterator->is_regular_file(error) || error) {
                error.clear();
                continue;
            }

            const std::string extension = ToLower(iterator->path().extension().string());
            // 対応付けられた画像拡張子だけを、プロジェクト基準のパスへ揃えて登録する。
            if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
                extension == ".tga" || extension == ".dds" || extension == ".hdr") {
                uniquePaths.insert(ToProjectPath(iterator->path()));
            }
        }
    }

    return { uniquePaths.begin(), uniquePaths.end() };
}

uint32_t TextureManager::GetSrvHandle(const std::string& filePath) {
    // 指定パスに対応するSRVハンドルを返し、元画像名で問い合わせた場合はDDS登録も補助的に探す。
    std::lock_guard<std::mutex> lock(mutex_);

    const auto findHandle = [this](const std::string& path) -> uint32_t {
        const bool autoLinear = IsLinearTexturePath(path);
        auto it = textureHandleMap_.find(MakeTextureCacheKey(path, autoLinear));

        // 自動判定された色空間で登録済みなら、その番号を優先する。
        if (it != textureHandleMap_.end()) {
            return it->second;
        }

        // 自動判定側で見つからなければ、反対の色空間で登録された同じ画像も探す。
        it = textureHandleMap_.find(MakeTextureCacheKey(path, !autoLinear));
        return it != textureHandleMap_.end() ? it->second : 0;
    };

    const std::string normalizedPath = NormalizeTexturePath(filePath);

    // 指定されたパスの登録が見つかれば、同名DDSを調べる前にその番号を返す。
    if (const uint32_t handle = findHandle(normalizedPath); handle != 0) {
        return handle;
    }

    std::filesystem::path ddsPath = PathFromUtf8(normalizedPath);

    // 元画像の拡張子で照会された場合だけ、キャッシュに登録済みの同名DDSを探す。
    if (IsSourceTextureExtension(ddsPath.extension().string())) {
        ddsPath.replace_extension(".dds");

        // DDSの登録が見つかれば、そのSRVを元画像への照会結果として返す。
        if (const uint32_t handle = findHandle(NormalizeTexturePath(PathToUtf8(ddsPath))); handle != 0) {
            return handle;
        }
    }

    return 0;
}

} // namespace engine
