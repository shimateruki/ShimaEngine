#include "game/system/settings/GameAudioSettings.h"

#include "json.hpp"
#include "engine/animation/cinematic/CinematicPlayer.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <unordered_set>

namespace {
constexpr const char* kCategorySE = "SE";
constexpr const char* kCategoryBGM = "BGM";
} // namespace

GameAudioSettings* GameAudioSettings::GetInstance() {
    // 音声 ID とロード済みハンドルを全シーンで共有する。
    static GameAudioSettings sInstance;
    return &sInstance;
}

void GameAudioSettings::Initialize() {
    // 登録表は初回だけ読み込み、シーン切替のたびに再構築しない。
    if (initialized_) {
        return;
    }

    initialized_ = true;
    Load();
    // 演出再生器へ音声ID表を一度だけ接続し、エンジンからゲーム設定への依存をなくす。
    CinematicPlayer::SetAudioServices(
        [this](const std::string& id, float volume, bool loop) { PlaySE(id, volume, loop); },
        [this](const std::string& id) { Stop(id); },
        [this]() {
            std::vector<CinematicPlayer::AudioEntry> result;
            for (const auto& entry : entries_) result.push_back({entry.id, entry.displayName});
            return result;
        });
}

void GameAudioSettings::Load() {
    // 登録表を読み直し、有効で一意な音声IDから再生先と設定値を組み立てる。
    entries_.clear();
    // 旧登録の音声ハンドルは持ち越さず、読み込み後の項目から再び取得させる。
    handles_.clear();

    nlohmann::json data;
    std::ifstream file(filePath_);

    // 登録表を開けた場合だけ解析する。開けなければ登録表は空のままにする。
    if (file.is_open()) {
        try {
            file >> data;
        // JSONを解析できなかった場合は空のオブジェクトとして処理し、壊れた設定を引き継がない。
        } catch (...) {
            data = nlohmann::json::object();
        }
    }

    // ルートがオブジェクトで、entriesが配列の場合だけ、登録候補を読む。
    if (data.is_object() && data.contains("entries") && data["entries"].is_array()) {
        const auto readString = [](const nlohmann::json& item, const char* key, const std::string& fallback) {
            return item.contains(key) && item[key].is_string() ? item[key].get<std::string>() : fallback;
        };

        // 同じIDを重複登録しないよう、この読込中に採用したIDを記録する。
        std::unordered_set<std::string> usedIds;

        for (const auto& item : data["entries"]) {
            // 配列に混在する文字列や数値などは、音声設定の項目として読めないため飛ばす。
            if (!item.is_object()) {
                continue;
            }

            // 表示名はIDで補い、音量やループが省略された項目にも再生用の既定値を与える。
            SoundEntry entry;
            entry.id = readString(item, "id", "");
            entry.displayName = readString(item, "displayName", entry.id);
            entry.category = readString(item, "category", kCategorySE);
            entry.path = readString(item, "path", "");
            entry.volume = item.contains("volume") && item["volume"].is_number()
                               ? ClampVolume(item["volume"].get<float>())
                               : 1.0f;
            entry.loop = item.contains("loop") && item["loop"].is_boolean() ? item["loop"].get<bool>() : false;

            // 実際に再生先を辿れるように、IDとパスが揃った最初の項目だけ採用する。
            if (!entry.id.empty() && !entry.path.empty() && usedIds.insert(entry.id).second) {
                entries_.push_back(entry);
            }
        }
    }

}

void GameAudioSettings::Save() const {
    // 登録表を整形してJSONに書き出し、音量を保存時にも0〜1の範囲へ揃える。
    nlohmann::json data;
    data["entries"] = nlohmann::json::array();

    // アセットへの参照と項目ごとの再生設定を、次回の読込に必要な形で保存する。
    for (const auto& entry : entries_) {
        data["entries"].push_back({{"id", entry.id},
                                   {"displayName", entry.displayName},
                                   {"category", entry.category},
                                   {"path", entry.path},
                                   {"volume", ClampVolume(entry.volume)},
                                   {"loop", entry.loop}});
    }

    std::filesystem::create_directories(std::filesystem::path(filePath_).parent_path());
    std::ofstream output(filePath_);

    // 保存先を開けた場合だけ、現在の登録表を書き込む。
    if (output.is_open()) {
        output << data.dump(4);
    }
}

GameAudioSettings::SoundEntry* GameAudioSettings::FindEntry(const std::string& id) {
    // 存在しない ID は nullptr とし、呼び出し側が再生を省略できる。
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const SoundEntry& entry) { return entry.id == id; });
    return it == entries_.end() ? nullptr : &(*it);
}

const GameAudioSettings::SoundEntry* GameAudioSettings::FindEntry(const std::string& id) const {
    // 読み取り専用の設定参照にも同じ ID 検索を使う。
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const SoundEntry& entry) { return entry.id == id; });
    return it == entries_.end() ? nullptr : &(*it);
}

bool GameAudioSettings::AddEntry(const SoundEntry& entry) {
    // 空 ID・空パス・重複 ID を拒否して保存データの一意性を保つ。
    if (entry.id.empty() || entry.path.empty() || FindEntry(entry.id)) {
        return false;
    }

    entries_.push_back(entry);
    return true;
}

bool GameAudioSettings::RemoveEntry(const std::string& id) {
    // 削除前に再生を停止し、古いロードハンドルを表から外す。
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const SoundEntry& entry) { return entry.id == id; });

    // 該当IDが未登録なら停止も削除も行わず、falseを返す。
    if (it == entries_.end()) {
        return false;
    }

    Stop(id);
    handles_.erase(id);
    entries_.erase(it);
    return true;
}

float GameAudioSettings::GetVolume(const std::string& id, float fallback) const {
    // 未登録の音でも呼び出し側が指定した音量で安全に扱える。
    const SoundEntry* entry = FindEntry(id);
    return entry ? ClampVolume(entry->volume) : ClampVolume(fallback);
}

void GameAudioSettings::SetVolume(const std::string& id, float volume) {
    // 設定のメモリ上の値を更新する。ファイルへ書くときはSave()を別に呼ぶ。
    SoundEntry* entry = FindEntry(id);

    // 登録済みのIDに限り、次回の再生で使用する設定音量を差し替える。
    if (entry) {
        entry->volume = ClampVolume(volume);
    }
}

AudioPlayer::AudioHandle GameAudioSettings::LoadHandle(const SoundEntry& entry) {
    // 同じ音声ファイルを繰り返し読み込まないよう、読み込み済みハンドルを共有します。
    auto cached = handles_.find(entry.id);

    // 同じIDのハンドルを取得済みなら、ファイルを開き直さずその値を返す。
    if (cached != handles_.end()) {
        return cached->second;
    }

    // ファイルがないIDは失敗を返し、後から配置された場合は次回の再生時に調べ直します。
    if (!std::filesystem::exists(entry.path)) {
        return AudioPlayer::kInvalidAudioHandle;
    }

    // ファイルが存在する場合はロードを試し、返された値を ID ごとに保持する。無効値も登録表の再読込までは保持する。
    AudioPlayer::AudioHandle handle = AudioPlayer::GetInstance()->LoadSoundFile(entry.path);
    handles_[entry.id] = handle;
    return handle;
}

void GameAudioSettings::PlaySE(const std::string& id, float volumeScale, bool loopOverride) {
    // 登録音量と呼び出し側の倍率を掛けてSEを再生する。どちらかがループを要求すれば繰り返す。
    Initialize();

    SoundEntry* entry = FindEntry(id);

    // IDが登録表に存在しなければ、ロード先を特定できないため再生を見送る。
    if (!entry) {
        return;
    }

    AudioPlayer::AudioHandle handle = LoadHandle(*entry);

    // SEのロード結果が無効なら、AudioPlayerへ再生を要求しない。
    if (handle == AudioPlayer::kInvalidAudioHandle) {
        return;
    }

    AudioPlayer::GetInstance()->PlaySE(handle, entry->loop || loopOverride, ClampVolume(entry->volume * volumeScale));
}

void GameAudioSettings::PlayBGM(const std::string& id, float volumeScale, bool loopOverride) {
    // 登録音量と呼び出し側の倍率を掛けてBGMを再生する。どちらかがループを要求すれば繰り返す。
    Initialize();

    SoundEntry* entry = FindEntry(id);

    // IDが登録表に存在しなければ、新しいBGMへの切り替えを要求しない。
    if (!entry) {
        return;
    }

    AudioPlayer::AudioHandle handle = LoadHandle(*entry);

    // BGMのロード結果が無効なら、AudioPlayerへ再生を要求しない。
    if (handle == AudioPlayer::kInvalidAudioHandle) {
        return;
    }

    AudioPlayer::GetInstance()->PlayBGM(handle, entry->loop || loopOverride, ClampVolume(entry->volume * volumeScale));
}

void GameAudioSettings::Stop(const std::string& id) {
    // カテゴリに応じて BGM 全体停止か、該当 SE ハンドル停止を選ぶ。
    SoundEntry* entry = FindEntry(id);

    // IDが未登録なら、停止する音のカテゴリとハンドルを特定できない。
    if (!entry) {
        return;
    }

    auto cached = handles_.find(entry->id);

    // このIDで取得済みの再生用ハンドルがなければ、停止を要求しない。
    if (cached == handles_.end()) {
        return;
    }

    // 登録カテゴリがBGMの場合は、AudioPlayerで現在再生中のBGMを停止する。
    if (entry->category == kCategoryBGM) {
        AudioPlayer::GetInstance()->StopBGM();
    // それ以外のカテゴリは、このIDのハンドルを使ってSEの停止を要求する。
    } else {
        AudioPlayer::GetInstance()->StopSe(cached->second);
    }
}

float GameAudioSettings::ClampVolume(float value) {
    // 有限値は0〜1に制限する。NaNや無限大の場合は等倍の1.0を使う。
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 1.0f;
}
