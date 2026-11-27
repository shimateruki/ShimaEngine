#pragma once

#include "AudioPlayer.h"

#include <string>
#include <unordered_map>
#include <vector>

// 効果音・BGMの登録表を読み込み、IDに対応する音量、再生先、ロード済みハンドルを管理する。
// AudioPlayerが音を再生し、CinematicPlayerは登録IDを通じて再生と停止を要求する。
// 主な入口はInitialize()、PlaySE()、PlayBGM()、Stop()。
class GameAudioSettings {
  public:
    // JSON に保存する音声1件分の設定
    struct SoundEntry {
        // 保存と再生要求で共通に使う一意な識別子。
        std::string id;
        // エディターの一覧に表示する名前。読み込み時の既定値はid。
        std::string displayName;
        // 停止時にBGMとSEを振り分ける登録カテゴリ。
        std::string category;
        // AudioPlayerが読み込むアセットのパス。
        std::string path;
        float volume = 1.0f; // 音声一件ごとの音量倍率。カテゴリの全体音量とは別に掛ける。
        // 再生時に繰り返す設定。呼び出し側もループを要求できる。
        bool loop = false;
    };

    /// 音声設定を共有するインスタンスを返す。
    static GameAudioSettings* GetInstance();

    // 最初の呼び出しで登録表を読み込み、演出再生用のコールバックを設定する。
    void Initialize();

    // 登録表を読み直す。必須値が不正な項目と重複した ID は読み飛ばし、以前のハンドルも解除する。
    void Load();

    /// 現在の音声登録と音量を JSON へ書き出す。
    void Save() const;

    // エディターが編集する登録配列。要素の追加・削除後は古い要素参照を取得し直す。
    std::vector<SoundEntry>& GetEntries() { return entries_; }

    const std::vector<SoundEntry>& GetEntries() const { return entries_; }

    /// ID に一致する登録音声を返す。見つからなければ nullptr。
    // 返す要素はentries_内の借用参照。Loadや登録配列の変更後は取得し直す。
    SoundEntry* FindEntry(const std::string& id);

    const SoundEntry* FindEntry(const std::string& id) const;

    // 重複IDまたは必須のID・パスが空の項目を拒否する。登録できた場合はtrueを返す。
    bool AddEntry(const SoundEntry& entry);

    // 登録済みなら停止とハンドルの削除を行い、項目を削除してtrueを返す。
    bool RemoveEntry(const std::string& id);

    // 見つからない ID は fallback を返す。返す音量と設定する音量は 0～1 に収める。
    float GetVolume(const std::string& id, float fallback = 1.0f) const;

    // 登録済みの音量を0〜1に補正して更新する。保存はSave()で行う。
    void SetVolume(const std::string& id, float volume);

    // id ごとに音声ファイルのハンドルを再利用する。ファイルがなければ無効ハンドルを返す。
    AudioPlayer::AudioHandle LoadHandle(const SoundEntry& entry);

    // volumeScale は登録音量へ乗算する倍率。loopOverride はループ再生を追加指定する。
    void PlaySE(const std::string& id, float volumeScale = 1.0f, bool loopOverride = false);

    // 登録音量にvolumeScaleを掛ける。既定では呼び出し側がBGMのループも要求する。
    void PlayBGM(const std::string& id, float volumeScale = 1.0f, bool loopOverride = true);

    // ハンドルを取得済みのIDを止める。登録カテゴリがBGMなら現在のBGMを、それ以外なら該当のSEを止める。
    void Stop(const std::string& id);

  private:
    GameAudioSettings() = default;

    GameAudioSettings(const GameAudioSettings&) = delete;

    GameAudioSettings& operator=(const GameAudioSettings&) = delete;

    // 有限値を0〜1に収め、NaNまたは無限大では1.0を返す。
    static float ClampVolume(float value);

    std::string filePath_ = "Resources/json/audio/audio_settings.json";
    // 登録表の初回読み込みと演出サービスの設定を実施したかを保持する。
    bool initialized_ = false;
    std::vector<SoundEntry> entries_;                                   // 登録されている音声設定。
    std::unordered_map<std::string, AudioPlayer::AudioHandle> handles_; // id ごとのロード済みハンドル。
};
