#include "VideoSpritePlayer.h"
#include "engine/graphics/core/TextureManager.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
constexpr size_t kRowBytes = VideoSpritePlayer::kWidth * 4;
constexpr size_t kFrameBytes = kRowBytes * VideoSpritePlayer::kHeight;

/// <summary>デコードした1コマの画素、ループ先頭からの表示時刻、周回開始の印を描画側へ渡す。</summary>
struct VideoFrame {
    double seconds = 0.0; // ループ先頭からの時刻。
    bool startsLoop = false;
    std::vector<uint8_t> pixels;
};

// デコード済みRGB32を、行の余白を除いた上から下のBGRA画素へ変換します。
bool CopySample(IMFSample& sample, LONG stride, std::vector<uint8_t>& pixels) {
    ComPtr<IMFMediaBuffer> buffer;
    // サンプルを1つの連続したバッファにまとめられなければ、画素をコピーできない。
    if (FAILED(sample.ConvertToContiguousBuffer(&buffer)))
        return false;
    pixels.resize(kFrameBytes);
    ComPtr<IMF2DBuffer> buffer2D;
    BYTE* firstRow = nullptr;
    DWORD length = 0;
    // 2Dのロックが使える場合は、先頭行と実際のストライドをMedia Foundationから受け取る。
    const bool use2D = SUCCEEDED(buffer.As(&buffer2D));
    HRESULT hr = use2D ? buffer2D->Lock2D(&firstRow, &stride) : buffer->Lock(&firstRow, nullptr, &length);
    // 画素バッファへアクセスできなければ読み込みを中止する。
    if (FAILED(hr))
        return false;
    const auto pitch = std::abs(static_cast<int64_t>(stride));
    // 行末の余白も含めて最後の行の開始位置まで進み、コピーする画素列全体がバッファ内に収まるか確認します。
    const auto required = pitch * (VideoSpritePlayer::kHeight - 1) + kRowBytes;
    const bool valid = firstRow && pitch >= kRowBytes && (use2D || required <= length);
    // 先頭行と行幅が有効な場合だけコピーする。通常バッファでは最終行までが取得サイズに収まることも確かめる。
    if (valid) {
        // 負のストライドで先頭が物理バッファの先頭を指す場合は、画像の論理的な最上行へ移してから降順に読みます。
        if (!use2D && stride < 0)
            firstRow += pitch * (VideoSpritePlayer::kHeight - 1);
        for (uint32_t y = 0; y < VideoSpritePlayer::kHeight; ++y) {
            auto* destination = pixels.data() + y * kRowBytes;
            std::memcpy(destination, firstRow + static_cast<ptrdiff_t>(y) * stride, kRowBytes);
            // RGB32の未定義バイトを不透明なアルファへ揃える。
            for (size_t x = 3; x < kRowBytes; x += 4)
                destination[x] = 255;
        }
    }
    // 2Dバッファの場合は、Lock2D()に対応する解除処理を呼ぶ。
    if (use2D)
        buffer2D->Unlock2D();
    // 通常のバッファの場合は、Lock()に対応する解除処理を呼ぶ。
    else
        buffer->Unlock();
    return valid;
}

// GPU転送先と同じ寸法・RGB32の動画だけを扱い、行の間隔は通知された値を優先する。
bool ReadFormat(IMFSourceReader& reader, LONG& stride) {
    ComPtr<IMFMediaType> actual;
    UINT32 width = 0, height = 0, pitch = 0;
    GUID subtype{};
    // フォーマットを取得できないか、RGB32またはGPU転送先の寸法と一致しなければ使用しない。
    if (FAILED(reader.GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actual)) ||
        FAILED(actual->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_RGB32 ||
        FAILED(MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &width, &height)) ||
        width != VideoSpritePlayer::kWidth || height != VideoSpritePlayer::kHeight)
        return false;
    // 既定ストライドを取得できた場合は、通知された値を使う。
    if (SUCCEEDED(actual->GetUINT32(MF_MT_DEFAULT_STRIDE, &pitch)))
        stride = static_cast<LONG>(pitch);
    // ストライド属性がない場合は形式と幅から算出する。算出できなければ失敗を返す。
    else if (FAILED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, width, &stride)))
        return false;
    return true;
}

// 先頭の映像ストリームだけを有効にし、Media FoundationにRGB32への変換を任せる。
bool OpenReader(const std::filesystem::path& path, ComPtr<IMFSourceReader>& reader, LONG& stride) {
    ComPtr<IMFAttributes> attributes;
    ComPtr<IMFMediaType> output;
    return SUCCEEDED(MFCreateAttributes(&attributes, 1)) &&
        SUCCEEDED(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE)) &&
        SUCCEEDED(MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), &reader)) &&
        SUCCEEDED(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE)) &&
        SUCCEEDED(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE)) &&
        SUCCEEDED(MFCreateMediaType(&output)) &&
        SUCCEEDED(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) &&
        SUCCEEDED(output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) &&
        SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, output.Get())) &&
        ReadFormat(*reader.Get(), stride);
}
}

/// <summary>デコード要求と先読みキューをワーカーと共有し、描画側で選んだフレームと再生位置を保持する。</summary>
struct VideoSpritePlayer::Playback {
    std::mutex mutex;
    std::condition_variable changed;
    std::filesystem::path path;
    std::deque<VideoFrame> queue;
    uint64_t request = 0; // ファイル切替前のデコード結果を識別する。
    bool stop = false;
    bool error = false;
    std::thread worker;

    VideoFrame selected;
    double seconds = 0.0;
    bool pending = false;
    bool hasImage = false;

    // ワーカーが要求を待ち、ファイルを読み進める。終了または要求の切替を検知したらデコードを区切る。
    void Read() {
        // デコードを担当するスレッドでCOMとMedia Foundationを初期化し、同じスレッドの終了時に解放する。
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool mediaStarted = SUCCEEDED(com) && SUCCEEDED(MFStartup(MF_VERSION));
        uint64_t previous = 0;
        for (;;) {
            std::filesystem::path file;
            uint64_t id;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] { return stop || previous != request; });
                // 終了フラグが立ったら、新しい要求を取り出さずにワーカーを終了する。
                if (stop)
                    break;
                // ファイルと要求番号をまとめて取得し、時間のかかるデコードはロックを外して実行する。
                id = previous = request;
                file = path;
            }
            // ファイル名が空の要求はClose()による中断なので、デコードを始めない。
            if (file.empty())
                continue;
            // Media Foundationを準備できないかデコードが失敗した場合だけ、現在の要求へエラーを通知する。
            if (!mediaStarted || !Decode(file, id)) {
                std::lock_guard lock(mutex);
                // 閉じた動画や切り替え前の失敗を、新しい再生要求のエラーとして扱わないようにします。
                if (request == id)
                    error = true;
            }
        }
        // Media Foundationを開始できたときだけ、対になる終了処理を行う。
        if (mediaStarted)
            MFShutdown();
        // ワーカーのCOM初期化が成功したときだけ、同じスレッドで解放する。
        if (SUCCEEDED(com))
            CoUninitialize();
    }

    // 現在の要求に対し、終端で先頭へ戻りながら最大3枚の画像を先読みします。
    bool Decode(const std::filesystem::path& file, uint64_t id) {
        ComPtr<IMFSourceReader> reader;
        LONG stride = 0;
        // Readerを開けないか必要なRGB32形式を設定できなければ、フレームを読み始めない。
        if (!OpenReader(file, reader, stride))
            return false;
        bool first = true;
        LONGLONG origin = 0;
        for (;;) {
            {
                std::unique_lock lock(mutex);
                // 未表示フレームを最大3枚に制限し、表示側が消費するまでデコードの先走りを抑えます。
                changed.wait(lock, [&] { return stop || request != id || queue.size() < 3; });
                // 終了やファイル切り替えの要求で起きた場合は、旧デコードを正常に中断する。
                if (stop || request != id)
                    return true;
            }
            ComPtr<IMFSample> sample;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            // サンプル取得の呼び出しが失敗するか、ストリームエラーを受けた場合は再生継続できない。
            if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr,
                                         &flags, &timestamp, &sample)) ||
                (flags & MF_SOURCE_READERF_ERROR))
                return false;
            // 映像形式の変更通知が来た場合は、寸法・画素形式・ストライドを確かめ直す。
            if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) && !ReadFormat(*reader.Get(), stride))
                return false;
            // 実際のサンプルが届いた場合にだけ表示候補を作る。
            if (sample) {
                // 周回先頭のフレームだけを基準にし、続く画像の表示時刻の原点とする。
                if (first)
                    origin = timestamp;
                VideoFrame frame;
                frame.startsLoop = first;
                // Media Foundationの100ナノ秒刻みを秒へ変換し、ループ先頭のフレームを時刻0に揃えます。
                frame.seconds = static_cast<double>(timestamp - origin) / 10000000.0;
                // 画素配置を解釈できなければ、そのフレームをキューに入れず失敗を返す。
                if (!CopySample(*sample.Get(), stride, frame.pixels))
                    return false;
                std::lock_guard lock(mutex);
                // デコード中に中断・切り替えが起きていないことを確かめてから登録する。
                if (stop || request != id)
                    return true;
                queue.push_back(std::move(frame));
                first = false;
            }
            // ストリーム終端の通知を受けたら、周回先頭へ戻る処理に進む。
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                // 今の周回で1枚も読めていなければ、空の動画として失敗を返す。
                if (first)
                    return false;
                PROPVARIANT position{};
                position.vt = VT_I8;
                position.hVal.QuadPart = 0;
                // 時刻0へ戻れなければ、次の周回を開始できない。
                if (FAILED(reader->SetCurrentPosition(GUID_NULL, position)))
                    return false;
                first = true;
            }
        }
    }
};

// 再生時刻・先読みキューを持つ状態を作ります。デコードスレッドは動画を要求したときに起動します。
VideoSpritePlayer::VideoSpritePlayer() : playback_(std::make_unique<Playback>()) {}

// 動画の使用権を返し、ワーカーを停止してから共有の再生状態を破棄します。
VideoSpritePlayer::~VideoSpritePlayer() {
    Close();
    auto& state = *playback_;
    {
        std::lock_guard lock(state.mutex);
        state.stop = true;
    }
    state.changed.notify_one();
    // ワーカーを起動済みなら、終了通知を処理して止まるまで待つ。
    if (state.worker.joinable())
        state.worker.join();
}

// 旧動画と表示を閉じ、新しいファイル名と識別番号を既存のデコードスレッドへ渡す。
void VideoSpritePlayer::RequestOpen(const std::filesystem::path& path) {
    Close();
    auto& state = *playback_;
    // 空パスの指定では、今までの動画を閉じるだけにする。
    if (path.empty())
        return;
    {
        std::lock_guard lock(state.mutex);
        state.path = path;
        ++state.request;
    }
    // ワーカーがまだ起動していなければ新しく作り、既存のワーカーがいれば再利用する。
    if (!state.worker.joinable())
        state.worker = std::thread([&state] { state.Read(); });
    state.changed.notify_one();
}

// 要求番号を進めて旧デコードを無効化した後、表示と転送先の使用権を手放す。スレッドは次の動画に再利用する。
void VideoSpritePlayer::Close() {
    auto& state = *playback_;
    {
        std::lock_guard lock(state.mutex);
        state.path.clear();
        ++state.request;
        state.queue.clear();
        state.error = false;
        state.seconds = 0.0;
        state.selected = {};
        state.pending = state.hasImage = false;
    }
    state.changed.notify_one();
    engine::TextureManager::GetInstance()->ReleaseVideoSurface(this);
}

// 再生時刻までに表示すべきフレームを先読みキューから選び、描画時の転送候補にします。
void VideoSpritePlayer::Update(double deltaSeconds) {
    auto& state = *playback_;
    std::lock_guard lock(state.mutex);
    // デコードが失敗している間は、再生時刻と表示候補を進めない。
    if (state.error)
        return;
    // 最初の表示候補または表示画像を持つ状態で、経過秒数が有限のときだけ再生時計を進める。
    if ((state.pending || state.hasImage) && std::isfinite(deltaSeconds))
        state.seconds += std::clamp(deltaSeconds, 0.0, 0.1);
    while (!state.queue.empty()) {
        auto& next = state.queue.front();
        // 次が周回先頭なら、いまの周回が終わったか確かめて時刻をリセットする。
        if (next.startsLoop) {
            // 最終フレームの表示時刻から1/30秒を経過するまでは、次の周回の先頭をキューに残す。
            if ((state.pending || state.hasImage) && state.seconds < state.selected.seconds + 1.0 / 30.0)
                break;
            state.seconds = 0.0;
        // 周回途中で次のフレームの表示時刻に未到達なら、キューへ残す。
        } else if (next.seconds > state.seconds) {
            break;
        }
        // 現在時刻までに複数枚進んだ場合は順に取り出し、最後に到達した画像をGPU転送候補にします。
        state.selected = std::move(next);
        state.pending = true;
        state.queue.pop_front();
    }
    // キューに空きができた可能性をワーカーへ通知する。
    state.changed.notify_one();
}

// 動画用の共有テクスチャに選択フレームを転送し、表示可能ならSRV番号を返します。
std::optional<uint32_t> VideoSpritePlayer::PrepareTextureForDraw(ID3D12GraphicsCommandList& commands) {
    auto& state = *playback_;
    // エラーがあるか、転送候補も転送済み画像もなければ、表示用のテクスチャを取得しない。
    if (HasError() || (!state.pending && !state.hasImage))
        return std::nullopt;
    auto* textures = engine::TextureManager::GetInstance();
    const auto handle = textures->AcquireVideoSurface(this);
    // 共有テクスチャを取得できない場合は、その描画では動画を表示できない。
    if (!handle)
        return std::nullopt;
    // 転送待ちの画素があり、転送を記録できた場合だけ転送待ちを解除する。
    if (state.pending && textures->UploadVideoSurface(this, commands, state.selected.pixels)) {
        state.pending = false;
        state.hasImage = true;
    }
    // 転送が見送られたフレームは以前に転送できた画像を使います。初回転送前は表示に使えるハンドルを返しません。
    return state.hasImage ? handle : std::nullopt;
}

// ワーカーが記録した、現在の要求のエラー状態をロック下で読み取る。
bool VideoSpritePlayer::HasError() const {
    std::lock_guard lock(playback_->mutex);
    return playback_->error;
}
