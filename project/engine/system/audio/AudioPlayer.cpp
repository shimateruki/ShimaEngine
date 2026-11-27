#include <cstdint>
#include "AudioPlayer.h"
#include <algorithm>
#include <string>
#include "CameraManager.h"
#include "json.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>

AudioPlayer* AudioPlayer::GetInstance() {
	// 音声デバイスとハンドル表を一つのインスタンスで共有する。
	static AudioPlayer sInstance;
	return &sInstance;
}

void AudioPlayer::Initialize() {
	// SourceVoice より先に XAudio2 と Media Foundation を初期化する。
	HRESULT result = XAudio2Create(&xAudio2_, 0, XAUDIO2_DEFAULT_PROCESSOR);
	assert(SUCCEEDED(result));
	result = xAudio2_->CreateMasteringVoice(&masteringVoice_);
	assert(SUCCEEDED(result));
	result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
	assert(SUCCEEDED(result));
}

// 通常音源と一時発音のデコードを止め、関連するVoiceとReaderを解放してからMedia Foundationを終了する。
void AudioPlayer::Finalize() {
	// 一時再生のハンドルは通常音源と別の表にあるため、先に個別のスレッドとVoiceを閉じる。
	for (auto& [handle, data] : transientSoundDatas_) {
		DestroyStreamingData(data.get());
	}

	transientSoundDatas_.clear();

	for (auto const& [handle, dataPtr] : streamingSoundDatas_) {
		SoundDataStreaming* data = dataPtr.get();

		// 登録先に音源データがなければ、解放するリーダーやVoiceがないので次へ進む。
		if (!data) {
			continue;
		}

		// 再生中は StopSe がデコードスレッドの join と Voice 破棄をまとめて行います。
		if (data->isPlaying_) {
			StopSe(handle);
		}

		// 自然終了したワンショットにも Voice が残り得るため、終了後の分も回収します。
		else if (data->sourceVoice_) {
			// 終了待ちできるデコードスレッドが残っている場合は、Voiceを閉じる前に待機する。
			if (data->decodeThread_.joinable()) {
				data->decodeThread_.join();
			}

			data->sourceVoice_->Stop(0);
			data->sourceVoice_->FlushSourceBuffers();
			data->sourceVoice_->DestroyVoice();
			data->sourceVoice_ = nullptr;
		}

		// Voice が先に解放されていても、残るデコードスレッドの終了を待ちます。
		else if (data->decodeThread_.joinable()) {
			data->decodeThread_.join();
		}


		// 読み込み用のリーダーが残っていれば、Media Foundationを止める前に解放する。
		if (data->sourceReader_) {
			data->sourceReader_.Reset();
		}
	}

	// SoundDataStreaming を先に破棄し、保持する Media Foundation 形式情報も解放します。
	streamingSoundDatas_.clear();

	MFShutdown();

	// XAudio2の本体はComPtrが所有しているため、インスタンス破棄時に解放される。
}

// UTF-8のパスから最初の音声ストリームを開き、PCM形式とReaderを保持した音源データを返す。
std::unique_ptr<SoundDataStreaming> AudioPlayer::CreateStreamingData(
	const std::string& filename) const {
	auto data = std::make_unique<SoundDataStreaming>();

	// ソースリーダーは UTF-16 の URL を要求するため、保存された UTF-8 パスを変換する。
	const std::int32_t stringSize =
		MultiByteToWideChar(CP_UTF8, 0, filename.c_str(), -1, nullptr, 0);
	// UTF-16への変換に必要な文字数を取得できなければ、開けるパスを作れない。
	if (stringSize <= 0) {
		return nullptr;
	}

	std::wstring wideFilename(static_cast<std::size_t>(stringSize), L'\0');
	MultiByteToWideChar(
		CP_UTF8, 0, filename.c_str(), -1, wideFilename.data(), stringSize);

	HRESULT result = MFCreateSourceReaderFromURL(
		wideFilename.c_str(), nullptr, &data->sourceReader_);
	// 指定したパスからソースリーダーを作れなければ、この音源を登録できない。
	if (FAILED(result)) {
		return nullptr;
	}

	Microsoft::WRL::ComPtr<IMFMediaType> mediaType;
	result = MFCreateMediaType(&mediaType);

	// 出力形式を指定するMediaTypeを作れない場合は、PCMへの設定を続けられない。
	if (FAILED(result)) {
		return nullptr;
	}

	mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);

	// デコード結果を PCM に揃え、XAudio2 に渡す WAVEFORMATEX を取得する。
	mediaType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
	result = data->sourceReader_->SetCurrentMediaType(
		static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
		nullptr,
		mediaType.Get());
	// 最初の音声ストリームをPCM出力に設定できなければ、Voice用の音源として扱わない。
	if (FAILED(result)) {
		return nullptr;
	}

	Microsoft::WRL::ComPtr<IMFMediaType> outputMediaType;
	result = data->sourceReader_->GetCurrentMediaType(
		static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
		&outputMediaType);
	// 設定後の実際の出力形式を取得できなければ、Voiceに渡す形式を決められない。
	if (FAILED(result)) {
		return nullptr;
	}

	UINT32 waveFormatSize = 0;
	result = MFCreateWaveFormatExFromMFMediaType(
		outputMediaType.Get(), &data->waveFormat_, &waveFormatSize);
	// 形式の変換に失敗したか形式情報が空なら、Voiceを作れないので読み込み失敗を返す。
	if (FAILED(result) || !data->waveFormat_) {
		return nullptr;
	}

	return data;
}

namespace {
// デコードがVoiceを参照し終えてから、待機中のバッファとVoiceを解放する。
void StopStreamingVoice(SoundDataStreaming& data) {
	data.isPlaying_ = false;
	// キューの空きを待っているワーカーを起こし、バッファやVoiceを回収する前に終了できるようにする。
	data.cv_.notify_one();

	// 起動済みで合流できるスレッドがあれば、Voiceやバッファに触れなくなるまで待つ。
	if (data.decodeThread_.joinable()) {
		data.decodeThread_.join();
	}

	// Voiceを作成済みの音源だけ、キューを空にしてからVoiceを破棄する。
	if (data.sourceVoice_) {
		data.sourceVoice_->Stop(0);
		data.sourceVoice_->FlushSourceBuffers();
		data.sourceVoice_->DestroyVoice();
		data.sourceVoice_ = nullptr;
	}
}
}

void AudioPlayer::DestroyStreamingData(SoundDataStreaming* data) {
	// 音源データがない場合は、停止やリーダーの解放を行う対象がないので戻る。
	if (!data) {
		return;
	}

	StopStreamingVoice(*data);

	data->sourceReader_.Reset();
}

AudioPlayer::AudioHandle AudioPlayer::LoadSoundFile(const std::string& filename) {
	// 同じファイルは同じハンドルを返し、二重のソースリーダー生成を避ける。
	std::lock_guard<std::mutex> lock(audioDataMutex_);
	auto existing = audioHandleMap_.find(filename);

	// 同じファイル名をすでに登録していれば、既存の音源ハンドルを返す。
	if (existing != audioHandleMap_.end()) {
		return existing->second;
	}

	auto data = CreateStreamingData(filename);

	// ソースリーダーと形式情報を準備できなければ、有効なハンドルを発行しない。
	if (!data) {
		return kInvalidAudioHandle;
	}

	const AudioHandle newHandle = nextHandle_++;
	audioHandleMap_[filename] = newHandle;
	streamingSoundDatas_[newHandle] = std::move(data);
	return newHandle;
}


void AudioPlayer::PlaySE(AudioHandle handle, bool loop, float volume) {
	// 個別 SE 音量とマスター音量を掛けてから再生する。
	PlayStreaming(handle, loop, std::clamp(volume, 0.0f, 1.0f) * seMasterVolume_);
}

void AudioPlayer::SetSEMasterVolume(float volume) {
	// 再生開始時の SE に適用する基準値は 0～1 に制限する。
	seMasterVolume_ = std::clamp(volume, 0.0f, 1.0f);
}

void AudioPlayer::SetBGMMasterVolume(float volume) {
	// BGM は再生中の Voice にも即時反映する。
	bgmMasterVolume_ = std::clamp(volume, 0.0f, 1.0f);
	ApplyCurrentBGMVolume();
}

// 同じ通常ハンドルに対応する前回のVoiceを閉じ、リーダーの先頭から再生し直す。
void AudioPlayer::PlayStreaming(AudioHandle handle, bool loop, float volume) {
	auto it = streamingSoundDatas_.find(handle);

	// 指定ハンドルに音源が登録されていなければ、再生を始められない。
	if (it == streamingSoundDatas_.end()) {
		return;
	}

	SoundDataStreaming* data = it->second.get();


	// 同じ音源がまだ再生中なら、StopSe()でデコードとVoiceを閉じてから再開する。
	if (data->isPlaying_) {
		StopSe(handle);
	}

	// 自然終了後に残った Voice も使い回さず破棄します。
	else if (data->sourceVoice_) {
		// 以前のデコードがまだ終了処理中なら、バッファ解放より先に待ちます。
		if (data->decodeThread_.joinable()) {
			data->decodeThread_.join();
		}

		data->sourceVoice_->Stop(0);
		data->sourceVoice_->FlushSourceBuffers();
		data->sourceVoice_->DestroyVoice();
		data->sourceVoice_ = nullptr;
	}

	// 前回の再生位置を引き継がないよう、ソースリーダーに先頭を指定する。
	PROPVARIANT var = {};
	PropVariantInit(&var);
	var.vt = VT_I8;
	var.hVal.QuadPart = 0;
	data->sourceReader_->SetCurrentPosition(GUID_NULL, var);
	PropVariantClear(&var);


	// 新しいVoiceへPCMの再生形式と、バッファを使い終えたことを知らせるコールバックを渡す。
	xAudio2_->CreateSourceVoice(&data->sourceVoice_, data->waveFormat_, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &data->voiceCallback_, NULL, NULL);

	// 呼び出し側の音量を 0～1 に収めて新しい Voice へ適用します。
	data->sourceVoice_->SetVolume(std::clamp(volume, 0.0f, 1.0f));

	data->sourceVoice_->Start(0);

	// 次のデコードループ用に再生中・終端・ループの状態を確定します。
	data->isPlaying_ = true;
	data->isEndOfStream_ = false;
	data->loop_ = loop;

	// 再生中のフラグとループ設定を確定した後で、デコードスレッドを起動する。
	data->decodeThread_ = std::thread(&AudioPlayer::DecodeThread, this, data);
}

void AudioPlayer::StopSe(AudioHandle handle) {
	// デコード停止を通知し、スレッド終了後に Voice を破棄する。
	auto it = streamingSoundDatas_.find(handle);

	// 未登録または再生が終わっているハンドルには、停止要求を送らない。
	if (it == streamingSoundDatas_.end() || !it->second->isPlaying_) {
		return;
	}

	SoundDataStreaming* data = it->second.get();
	StopStreamingVoice(*data);

	// デコードが終了した後にリーダーを先頭へ戻し、次回に備える。
	PROPVARIANT var = {};
	PropVariantInit(&var);
	var.vt = VT_I8;
	var.hVal.QuadPart = 0;
	data->sourceReader_->SetCurrentPosition(GUID_NULL, var);
	PropVariantClear(&var);

	data->isEndOfStream_ = false;
}

void AudioPlayer::DecodeThread(SoundDataStreaming* data) {
	// デコード元を持たないスレッドは、Voiceやリーダーに触れず終了する。
	if (!data)
		return;

	while (data->isPlaying_) {
		XAUDIO2_VOICE_STATE state = {};
		data->sourceVoice_->GetState(&state);

		// 再生キューの未処理数がリングバッファ数より少ない場合だけ、次のサンプルを読む。
		if (state.BuffersQueued < SoundDataStreaming::kNumBuffers) {

			DWORD streamFlags = 0;
			Microsoft::WRL::ComPtr<IMFSample> mfSample;
			HRESULT hr = data->sourceReader_->ReadSample(
				(DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
				0, NULL, &streamFlags, NULL, &mfSample);

			// 読み込みに成功し、終端フラグが返った場合は再生方式に応じて処理する。
			if (SUCCEEDED(hr) && (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM)) {
				// ループ再生の音源はリーダーを先頭へ戻し、次の読み込みに備える。
				if (data->loop_) {
					PROPVARIANT var = {};
					PropVariantInit(&var);
					var.vt = VT_I8;
					var.hVal.QuadPart = 0;
					data->sourceReader_->SetCurrentPosition(GUID_NULL, var);
					PropVariantClear(&var);

				// 一回再生の音源は、残りのキューが空になるか停止要求が来るまで待って終了する。
				} else {
					while (state.BuffersQueued > 0 && data->isPlaying_) {
						data->sourceVoice_->GetState(&state);
						std::this_thread::sleep_for(std::chrono::milliseconds(10));
					}

					break;
				}
			}

			// 読めたサンプルがあるときだけ、PCMデータをVoiceの再生キューへ渡す。
			if (SUCCEEDED(hr) && mfSample) {
				Microsoft::WRL::ComPtr<IMFMediaBuffer> mfBuffer;
				BYTE* localAudioData = nullptr;
				DWORD localAudioDataBytes = 0;

				mfSample->ConvertToContiguousBuffer(&mfBuffer);
				mfBuffer->Lock(&localAudioData, NULL, &localAudioDataBytes);

				// デコード直後のメモリはUnlockで参照できなくなるため、再生終了まで残る専用バッファへPCMをコピーする。
				auto& currentBuffer = data->buffers_[data->currentBufferIndex_];
				currentBuffer.assign(localAudioData, localAudioData + localAudioDataBytes);

				mfBuffer->Unlock();

				// 再生が終わるまで所有しているPCM領域をVoiceに渡す。
				XAUDIO2_BUFFER audioBuffer = {};
				audioBuffer.pAudioData = currentBuffer.data();
				audioBuffer.AudioBytes = (UINT32)currentBuffer.size();
				// 再生バッファの完了通知を待機中のデコードへ戻すため、通知先をバッファに添える。
				audioBuffer.pContext = &data->cv_;
				data->sourceVoice_->SubmitSourceBuffer(&audioBuffer);

				// 次の空き枠へ進み、最後の枠の次は先頭へ戻す。
				data->currentBufferIndex_ = (data->currentBufferIndex_ + 1) % SoundDataStreaming::kNumBuffers;
			}
		// リングバッファの全枠が使用中なら、再生バッファの完了を待つ。
		} else {
			std::unique_lock<std::mutex> lock(data->mtx_);
			// 完了通知を待ちつつ、通知の前後がずれても10ミリ秒以内に空きと停止フラグを確認し直す。
			data->cv_.wait_for(lock, std::chrono::milliseconds(10));
		}
	}

	// 終端か外部停止でデコードを終えたら、再生中フラグを下ろす。
	data->isPlaying_ = false;
}

/// <summary>
/// BGMを再生（または継続）します。
/// </summary>
void AudioPlayer::PlayBGM(AudioHandle handle, bool loop, float volume) {
	currentBgmBaseVolume_ = std::clamp(volume, 0.0f, 1.0f);

	// 要求されたBGMがすでに再生中なら、曲を継続して音量だけを反映する。
	if (currentBgmHandle_ == handle && IsPlaying(handle)) {
		ApplyCurrentBGMVolume();
		return;
	}

	// 別の有効なBGMが再生中なら、新しい曲を始める前に停止する。
	if (currentBgmHandle_ != kInvalidAudioHandle && IsPlaying(currentBgmHandle_)) {
		StopSe(currentBgmHandle_);
	}

	// 個別音量とBGMのマスター音量を掛け、切替先の音源へ渡す。
	PlayStreaming(handle, loop, currentBgmBaseVolume_ * bgmMasterVolume_);
	currentBgmHandle_ = handle;
}

/// <summary>
/// 現在再生中のBGMを停止します。
/// </summary>
void AudioPlayer::StopBGM() {
	// 管理中のBGMハンドルがあれば、その曲へ停止を要求して選択を解除する。
	if (currentBgmHandle_ != kInvalidAudioHandle) {
		StopSe(currentBgmHandle_);
		currentBgmHandle_ = kInvalidAudioHandle;
	}
}

/// <summary>
/// 指定したハンドルが現在再生中か確認します。
/// </summary>
bool AudioPlayer::IsPlaying(AudioHandle handle) const {
	auto it = streamingSoundDatas_.find(handle);

	// ハンドルが音源表に見つからなければ、再生中の音源として扱わない。
	if (it == streamingSoundDatas_.end()) {
		return false;
	}

	// 開始時に立ち、ストリーム末尾の処理または停止で下がるフラグを返す。
	return it->second->isPlaying_;
}

void AudioPlayer::ApplyCurrentBGMVolume() {
	// 管理中のBGMハンドルが空なら、反映すべき再生先がない。
	if (currentBgmHandle_ == kInvalidAudioHandle) {
		return;
	}

	auto it = streamingSoundDatas_.find(currentBgmHandle_);

	// 音源が失効したかVoiceがまだ無い場合は、現在音量を送れる先がないので戻る。
	if (it == streamingSoundDatas_.end() || !it->second || !it->second->sourceVoice_) {
		return;
	}

	it->second->sourceVoice_->SetVolume(currentBgmBaseVolume_ * bgmMasterVolume_);
}

AudioPlayer::PlaybackHandle AudioPlayer::PlayTransientSE(
	const std::string& filename,
	float volume,
	float pitch) {
	// 一時 SE は音源ハンドルを共有せず、同時発音ごとに独立した Voice を持つ。
	Update();

	// XAudio2の初期化前は一時SE用のVoiceを作成できない。
	if (!xAudio2_) {
		return kInvalidPlaybackHandle;
	}

	auto data = CreateStreamingData(filename);

	// 音源を開けなかった場合や再生形式が無い場合は、一時再生ハンドルを発行しない。
	if (!data || !data->waveFormat_) {
		return kInvalidPlaybackHandle;
	}

	HRESULT result = xAudio2_->CreateSourceVoice(
		&data->sourceVoice_,
		data->waveFormat_,
		0,
		XAUDIO2_DEFAULT_FREQ_RATIO,
		&data->voiceCallback_,
		nullptr,
		nullptr);
	// Voice作成に失敗したか参照を得られなければ、音量設定や再生を行わない。
	if (FAILED(result) || !data->sourceVoice_) {
		return kInvalidPlaybackHandle;
	}

	// マスター音量とピッチを設定し、PCMの最初のバッファから単発再生する。
	data->sourceVoice_->SetVolume(
		std::clamp(volume, 0.0f, 1.0f) * seMasterVolume_);
	// ピッチは原音を1とする周波数倍率で、0.5～2の範囲をVoiceに渡す。
	data->sourceVoice_->SetFrequencyRatio(std::clamp(pitch, 0.5f, 2.0f));
	data->isEndOfStream_ = false;
	data->loop_ = false;
	data->currentBufferIndex_ = 0;
	data->isPlaying_ = true;
	data->sourceVoice_->Start(0);

	const PlaybackHandle handle = nextPlaybackHandle_++;
	SoundDataStreaming* rawData = data.get();
	transientSoundDatas_.emplace(handle, std::move(data));
	// ハンドルの所有表へ登録してから、デコードスレッドにデータのアドレスを渡す。
	rawData->decodeThread_ = std::thread(&AudioPlayer::DecodeThread, this, rawData);
	return handle;
}

bool AudioPlayer::IsTransientPlaying(PlaybackHandle handle) const {
	// 自然終了で消えたハンドルも「再生中ではない」と扱う。
	const auto found = transientSoundDatas_.find(handle);
	return found != transientSoundDatas_.end() &&
		found->second &&
		found->second->isPlaying_.load();
}

void AudioPlayer::StopTransient(PlaybackHandle handle) {
	// 先にスレッドと Voice を閉じてから所有するデータを消す。
	auto found = transientSoundDatas_.find(handle);

	// 自然終了などですでに回収されたハンドルなら、停止すべき音源がない。
	if (found == transientSoundDatas_.end()) {
		return;
	}

	DestroyStreamingData(found->second.get());
	transientSoundDatas_.erase(found);
}

void AudioPlayer::Update() {
	// 自然終了した一時 SE をここで回収し、同時発音数を増やし続けない。
	for (auto iterator = transientSoundDatas_.begin();
		iterator != transientSoundDatas_.end();) {
		// データがありデコード中なら、発音を維持して次のハンドルへ進む。
		if (iterator->second && iterator->second->isPlaying_.load()) {
			++iterator;
			continue;
		}

		DestroyStreamingData(iterator->second.get());
		iterator = transientSoundDatas_.erase(iterator);
	}
}

namespace {
// 音声の検索キーを揃えるため、区切り文字と先頭の相対パス表記を正規化します。
std::string NormalizeAudioPath(std::string path) {
	std::replace(path.begin(), path.end(), '\\', '/');

	while (path.rfind("./", 0) == 0) {
		path.erase(0, 2);
	}

	return path;
}

bool IsAudioFilePath(const std::filesystem::path& path) {
	// 拡張子の大文字小文字に依存せず、再生対象にできる形式だけ受け付ける。
	std::string extension = path.extension().string();
	std::transform(
		extension.begin(),
		extension.end(),
		extension.begin(),
		[](unsigned char character) {
			return static_cast<char>(std::tolower(character));
		});
	return extension == ".wav" || extension == ".mp3" ||
		extension == ".ogg" || extension == ".flac";
}

void SetAudioEventError(std::string* destination, const std::string& message) {
	// エラーの出力先を要求されたときだけ、メッセージを書き込む。
	if (destination) {
		*destination = message;
	}
}
}

// イベントの設定キャッシュと直前に選んだ音の記録を、シーン間で共有する。
AudioEventSystem* AudioEventSystem::GetInstance() {
	static AudioEventSystem sInstance;
	return &sInstance;
}

bool AudioEventSystem::LoadEvent(
	const std::string& eventPath,
	AudioEventDefinition& definition,
	std::string* errorMessage) const {
	// JSON の候補音源を検証し、相対パスはイベントの隣か SE ディレクトリから解決する。
	const std::string normalizedPath = NormalizeAudioPath(eventPath);
	std::ifstream file(normalizedPath, std::ios::binary);

	// イベントファイルを開けない場合は、パスを添えて読み込み失敗を返す。
	if (!file) {
		SetAudioEventError(errorMessage, "Audio Eventを開けません: " + normalizedPath);
		return false;
	}

	try {
		nlohmann::json root;
		file >> root;
		AudioEventDefinition loaded;

		// clipsを配列として取得できないイベントは、音源候補の定義がないため読み込まない。
		if (!root.contains("clips") || !root["clips"].is_array()) {
			SetAudioEventError(errorMessage, "clips配列がありません: " + normalizedPath);
			return false;
		}

		const std::filesystem::path parent =
			std::filesystem::path(normalizedPath).parent_path();
		for (const auto& value : root["clips"]) {
			// 文字列でない候補は音声パスとして解釈できないので飛ばす。
			if (!value.is_string()) {
				continue;
			}

			std::string clip = NormalizeAudioPath(value.get<std::string>());

			// 正規化後に空の候補は、音声ファイルを指していないので飛ばす。
			if (clip.empty()) {
				continue;
			}

			std::filesystem::path clipPath(clip);

			// 絶対パスやResources起点のパスは使える形なので、短い相対名だけ探索する。
			if (!clipPath.is_absolute() && clip.rfind("Resources/", 0) != 0) {
				const std::filesystem::path besideEvent = parent / clipPath;
				const std::filesystem::path underSe =
					std::filesystem::path("Resources/audio/se") / clipPath;
				// イベントの隣に同名ファイルが存在すれば、そのパスを優先する。
				if (std::filesystem::exists(besideEvent)) {
					clip = NormalizeAudioPath(besideEvent.generic_string());
				}
				// イベントの隣になければ、共通SEフォルダーを基準にパスを組み立てる。
				else {
					clip = NormalizeAudioPath(underSe.generic_string());
				}
			}

			// 対応する音声拡張子を持つ候補だけ、抽選に使う一覧へ加える。
			if (IsAudioFilePath(std::filesystem::path(clip))) {
				loaded.clips.push_back(std::move(clip));
			}
		}

		// 有効な候補が一つも残らなければ、イベントを発音できないので読み込みを失敗させる。
		if (loaded.clips.empty()) {
			SetAudioEventError(errorMessage, "再生可能なclipがありません: " + normalizedPath);
			return false;
		}

		// 逆順で書かれた範囲も正規化し、後段の乱数選択へ常に最小→最大を渡す。
		const auto readRange = [&root](
			const char* key,
			float& minimum,
			float& maximum) {
			// 最小値と最大値の組を取得できない場合は、既定の範囲を保つ。
			if (!root.contains(key) || !root[key].is_array() ||
				root[key].size() < 2) {
				return;
			}

			minimum = root[key][0].get<float>();
			maximum = root[key][1].get<float>();

			// 最小値より最大値が小さく指定されたら、両端を入れ替えて範囲を正す。
			if (minimum > maximum) {
				std::swap(minimum, maximum);
			}
		};

		// 確率選択で使う音量・ピッチと、発音数・距離の制限を読み取り時に揃える。
		readRange("volumeRange", loaded.volumeMin, loaded.volumeMax);
		readRange("pitchRange", loaded.pitchMin, loaded.pitchMax);
		loaded.volumeMin = std::clamp(loaded.volumeMin, 0.0f, 1.0f);
		loaded.volumeMax = std::clamp(loaded.volumeMax, 0.0f, 1.0f);
		loaded.pitchMin = std::clamp(loaded.pitchMin, 0.5f, 2.0f);
		loaded.pitchMax = std::clamp(loaded.pitchMax, 0.5f, 2.0f);
		loaded.maxInstances =
			std::clamp(root.value("maxInstances", loaded.maxInstances), 1, 64);
		loaded.spatial = root.value("spatial", loaded.spatial);
		loaded.minDistance =
			(std::max)(0.0f, root.value("minDistance", loaded.minDistance));
		// 減衰の終点は開始点から離し、再生時の距離正規化で分母を正に保つ。
		loaded.maxDistance =
			(std::max)(loaded.minDistance + 0.01f,
				root.value("maxDistance", loaded.maxDistance));

		definition = std::move(loaded);
		SetAudioEventError(errorMessage, "");
		return true;
	}
	// 解析や型変換が失敗した場合は、出力設定を置き換えず例外の内容を返す。
	catch (const std::exception& exception) {
		SetAudioEventError(
			errorMessage,
			"Audio Eventの解析に失敗しました: " + std::string(exception.what()));
		return false;
	}
}

bool AudioEventSystem::SaveEvent(
	const std::string& eventPath,
	const AudioEventDefinition& definition,
	std::string* errorMessage) {
	// 音源候補が空のイベントは発音できないため、保存する前に拒否する。
	if (definition.clips.empty()) {
		SetAudioEventError(errorMessage, "1つ以上のclipが必要です。");
		return false;
	}

	const std::filesystem::path path(NormalizeAudioPath(eventPath));
	std::error_code error;

	// 親フォルダーがあるパスでは、ファイルを開く前にフォルダーを作成する。
	if (!path.parent_path().empty()) {
		std::filesystem::create_directories(path.parent_path(), error);

		// 親フォルダーを作れない場合は、保存先を開く前に失敗を返す。
		if (error) {
			SetAudioEventError(errorMessage, "保存フォルダを作成できません。");
			return false;
		}
	}

	// 保存用コピーの音量とピッチを補正し、呼出元が保持する編集値は変更しない。
	AudioEventDefinition sanitized = definition;

	// 音量の両端が逆順なら、保存する範囲を小さい値から並べる。
	if (sanitized.volumeMin > sanitized.volumeMax) {
		std::swap(sanitized.volumeMin, sanitized.volumeMax);
	}

	// ピッチの両端が逆順なら、保存する倍率の範囲を小さい値から並べる。
	if (sanitized.pitchMin > sanitized.pitchMax) {
		std::swap(sanitized.pitchMin, sanitized.pitchMax);
	}

	sanitized.volumeMin = std::clamp(sanitized.volumeMin, 0.0f, 1.0f);
	sanitized.volumeMax = std::clamp(sanitized.volumeMax, 0.0f, 1.0f);
	sanitized.pitchMin = std::clamp(sanitized.pitchMin, 0.5f, 2.0f);
	sanitized.pitchMax = std::clamp(sanitized.pitchMax, 0.5f, 2.0f);
	sanitized.maxInstances = std::clamp(sanitized.maxInstances, 1, 64);
	sanitized.minDistance = (std::max)(0.0f, sanitized.minDistance);
	sanitized.maxDistance =
		(std::max)(sanitized.minDistance + 0.01f, sanitized.maxDistance);

	nlohmann::json root = {
		{ "schemaVersion", 1 },
		{ "clips", sanitized.clips },
		{ "volumeRange", { sanitized.volumeMin, sanitized.volumeMax } },
		{ "pitchRange", { sanitized.pitchMin, sanitized.pitchMax } },
		{ "maxInstances", sanitized.maxInstances },
		{ "spatial", sanitized.spatial },
		{ "minDistance", sanitized.minDistance },
		{ "maxDistance", sanitized.maxDistance }
	};

	std::ofstream file(path, std::ios::binary);

	// 出力ファイルを開けなければ、設定を書き込まずエラーを返す。
	if (!file) {
		SetAudioEventError(errorMessage, "Audio Eventを保存できません。");
		return false;
	}

	file << root.dump(2);

	// JSONを出力した後にストリームエラーがあれば、キャッシュへ成功扱いで登録しない。
	if (!file.good()) {
		SetAudioEventError(errorMessage, "Audio Eventの書き込みに失敗しました。");
		return false;
	}

	// 書き込みを確認してから、次の発音で保存した設定を反映する。
	eventCache_[NormalizeAudioPath(path.generic_string())] = std::move(sanitized);
	SetAudioEventError(errorMessage, "");
	return true;
}

bool AudioEventSystem::Prewarm(
	const std::string& eventPath,
	std::string* errorMessage) {
	// 全候補の読み込みに成功したイベントだけをキャッシュに登録する。
	AudioEventDefinition definition;

	// イベントを読めなければ、音源候補を先に読み込む処理へ進まない。
	if (!LoadEvent(eventPath, definition, errorMessage)) {
		return false;
	}

	bool succeeded = true;

	for (const std::string& clip : definition.clips) {
		// 候補を開けなかった場合は失敗を記録し、後続の候補の読み込みは続ける。
		if (AudioPlayer::GetInstance()->LoadSoundFile(clip) ==
			AudioPlayer::kInvalidAudioHandle) {
			succeeded = false;
			SetAudioEventError(errorMessage, "音声を準備できません: " + clip);
		}
	}

	// 全候補の事前読み込みが成功したイベントだけ、次回の発音に備えてキャッシュへ入れる。
	if (succeeded) {
		eventCache_[NormalizeAudioPath(eventPath)] = std::move(definition);
	}

	return succeeded;
}

AudioPlayer::PlaybackHandle AudioEventSystem::Play(
	const std::string& eventPath,
	const engine::Vector3* worldPosition,
	float volumeScale) {
	// 同一イベントの同時発音数を制限してから候補を選び、直前の音を可能なら避ける。
	Update();
	const std::string normalizedPath = NormalizeAudioPath(eventPath);

	auto cached = eventCache_.find(normalizedPath);

	// イベントをまだキャッシュしていなければ、発音前に設定を読む。
	if (cached == eventCache_.end()) {
		AudioEventDefinition definition;

		// 設定を開けないか解析に失敗した場合は、再生ハンドルを発行しない。
		if (!LoadEvent(normalizedPath, definition, nullptr)) {
			return AudioPlayer::kInvalidPlaybackHandle;
		}

		cached = eventCache_.emplace(normalizedPath, std::move(definition)).first;
	}

	const AudioEventDefinition& definition = cached->second;

	const std::size_t activeCount = static_cast<std::size_t>(std::count_if(
		activeInstances_.begin(),
		activeInstances_.end(),
		[&](const ActiveInstance& instance) {
			return instance.eventPath == normalizedPath;
		}));
	// 同じイベントの発音が指定上限に達したら、さらにVoiceを作成しない。
	if (activeCount >= static_cast<std::size_t>(definition.maxInstances)) {
		return AudioPlayer::kInvalidPlaybackHandle;
	}

	// 候補を抽選し、直前と同じ候補を引いたときだけ一覧で次の音へ送る。
	std::uniform_int_distribution<std::size_t> clipDistribution(
		0, definition.clips.size() - 1);
	std::size_t clipIndex = clipDistribution(randomEngine_);
	const auto last = lastClipByEvent_.find(normalizedPath);
	// 候補が複数あり、直前の抽選結果と同じ候補を引いたときだけ次の候補へずらす。
	if (definition.clips.size() > 1 &&
		last != lastClipByEvent_.end() &&
		clipIndex == last->second) {
		clipIndex = (clipIndex + 1) % definition.clips.size();
	}

	// 再生の成否にかかわらず、今回抽選した候補を次回の連続回避に使う。
	lastClipByEvent_[normalizedPath] = clipIndex;

	std::uniform_real_distribution<float> volumeDistribution(
		definition.volumeMin, definition.volumeMax);
	std::uniform_real_distribution<float> pitchDistribution(
		definition.pitchMin, definition.pitchMax);
	float volume = volumeDistribution(randomEngine_) *
		std::clamp(volumeScale, 0.0f, 1.0f);

	// 距離減衰を使うイベントに発音座標が渡された場合は、視点を取得する。
	if (definition.spatial && worldPosition) {
		// 現在のカメラを取得できた場合だけ、発音位置と視点の距離を音量に反映する。
		if (engine::Camera* camera = CameraManager::GetInstance()->GetActiveCamera()) {
			// カメラの視点からのワールド距離で、最小距離の等倍から最大距離の無音まで直線的に減衰する。
			const float distance = engine::Math::Length(*worldPosition - camera->GetEye());
			const float attenuation = 1.0f - std::clamp(
				(distance - definition.minDistance) /
					(definition.maxDistance - definition.minDistance),
				0.0f,
				1.0f);
			volume *= attenuation;
		}
	}

	// 抽選した音源とピッチ、距離補正後の音量を一時Voiceの再生へ渡す。
	const AudioPlayer::PlaybackHandle handle =
		AudioPlayer::GetInstance()->PlayTransientSE(
			definition.clips[clipIndex],
			volume,
			pitchDistribution(randomEngine_));
	// 一時SEの再生が成立した場合だけ、同時発音数に数えるハンドルとして記録する。
	if (handle != AudioPlayer::kInvalidPlaybackHandle) {
		activeInstances_.push_back({ normalizedPath, handle });
	}

	return handle;
}

AudioPlayer::PlaybackHandle AudioEventSystem::PlayReference(
	const std::string& reference,
	const engine::Vector3* worldPosition,
	float volumeScale) {
	// 参照先がイベント JSON なら設定に従い、音声ファイルなら直接再生する。
	const std::string eventPath = ResolveEventPath(reference);

	// 参照名からイベントJSONを特定できた場合は、その設定に従って発音する。
	if (!eventPath.empty()) {
		return Play(eventPath, worldPosition, volumeScale);
	}

	std::string audioPath = NormalizeAudioPath(reference);

	// Resources起点でない短い参照は共通SEディレクトリから探す。絶対パスの場合はパス結合で絶対パスが優先される。
	if (audioPath.rfind("Resources/", 0) != 0) {
		audioPath = NormalizeAudioPath(
			(std::filesystem::path("Resources/audio/se") / audioPath)
				.generic_string());
	}

	// 音声として対応しない拡張子は、直接再生の対象から外す。
	if (!IsAudioFilePath(std::filesystem::path(audioPath))) {
		return AudioPlayer::kInvalidPlaybackHandle;
	}

	return AudioPlayer::GetInstance()->PlayTransientSE(
		audioPath,
		std::clamp(volumeScale, 0.0f, 1.0f),
		1.0f);
}

void AudioEventSystem::Update() {
	// 再生済みの一時 SE を回収し、イベント別同時発音数の計算から除外する。
	AudioPlayer* audioPlayer = AudioPlayer::GetInstance();
	audioPlayer->Update();
	activeInstances_.erase(
		std::remove_if(
			activeInstances_.begin(),
			activeInstances_.end(),
			[audioPlayer](const ActiveInstance& instance) {
				return !audioPlayer->IsTransientPlaying(instance.handle);
			}),
		activeInstances_.end());
}

std::string AudioEventSystem::ResolveEventPath(
	const std::string& reference) const {
	// JSON の明示パスを優先し、短い名前は既定のイベント保存先で探す。
	std::string normalized = NormalizeAudioPath(reference);

	// 正規化した参照名が空なら、探すイベントがないので空を返す。
	if (normalized.empty()) {
		return {};
	}

	std::filesystem::path direct(normalized);

	// 指定位置に実在するJSONなら、既定フォルダーで同名のイベントを探す前に採用する。
	if (direct.extension() == ".json" && std::filesystem::exists(direct)) {
		return NormalizeAudioPath(direct.generic_string());
	}

	std::filesystem::path fileName = direct.filename();

	// 参照したファイル名に拡張子がなければ、既定の検索ではJSONとして探す。
	if (fileName.extension().empty()) {
		fileName.replace_extension(".json");
	}

	// 直接指定が解決しなければ、イベントの標準フォルダーを先、旧フォルダーを後に探す。
	for (const std::filesystem::path& directory : {
		std::filesystem::path("Resources/json/audio_events"),
		std::filesystem::path("Resources/json/audio") }) {
		const std::filesystem::path candidate = directory / fileName;

		// この既定フォルダーで候補が見つかった場合は、後続フォルダーを検索せず採用する。
		if (std::filesystem::exists(candidate)) {
			return NormalizeAudioPath(candidate.generic_string());
		}
	}

	return {};
}
