#include "game/system/settings/GameSettingsManager.h"

#include "AudioPlayer.h"
#include "json.hpp"
#include "engine/utility/JsonFile.h"
#include "engine/utility/math/Math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>

engine::Vector2 GameSettingsManager::ApplyCameraLookInversion(engine::Vector2 lookDelta) const {
    // 左右反転が有効なら、横方向だけ向きを逆にする。
    if (cameraInvertX_) {
        lookDelta.x = -lookDelta.x;
    }
    // 上下反転は左右の設定から独立して適用する。
    if (cameraInvertY_) {
        lookDelta.y = -lookDelta.y;
    }
    return lookDelta;
}

namespace {
// 保存・再生に使う音量を設定可能な範囲へ制限します。
float ClampVolume(float value) {
    // 壊れた保存値の NaN は音量計算へ流さず既定値へ戻す。
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 1.0f;
}

// マウス感度を設定可能な範囲へ制限します。
float ClampSensitivity(float value) {
    // 無効な設定でカメラ操作が止まらないよう有効範囲へ丸める。
    return std::isfinite(value) ? std::clamp(value, 0.2f, 1.0f) : 1.0f;
}

GameSettingsManager::StageSelectWeatherMode ClampStageSelectWeatherMode(std::int32_t value) {
    // 未知の列挙値は自動天候へ戻して古い設定ファイルも受け入れる。
    if (value < static_cast<std::int32_t>(GameSettingsManager::StageSelectWeatherMode::kAuto) ||
        value > static_cast<std::int32_t>(GameSettingsManager::StageSelectWeatherMode::kRain)) {
        return GameSettingsManager::StageSelectWeatherMode::kAuto;
    }

    return static_cast<GameSettingsManager::StageSelectWeatherMode>(value);
}
} // namespace

GameSettingsManager* GameSettingsManager::GetInstance() {
    // 保存設定は全シーンで同じ実体を共有する。
    static GameSettingsManager sInstance;
    return &sInstance;
}

void GameSettingsManager::Initialize() {
    // 二度目以降の初期化では読み直さず、保持中の音量だけ適用する。
    if (initialized_) {
        ApplyAudioSettings();
        return;
    }

    initialized_ = true;
    Load();
}

// 音量は直ちに反映し、操作設定などは利用側が保持値を参照する。
void GameSettingsManager::Load() {
    // 構文を読めた場合は項目ごとに型を検査する。ファイル全体を読めない場合は各設定の代替値を使う。
    nlohmann::json data = nlohmann::json::object();
    std::ifstream file(filePath_);

    // 保存ファイルを開けた場合だけ読み、その内容を既定設定へ上書きする候補とする。
    if (file.is_open()) {
        try {
            file >> data;
        } catch (...) {
            // 構文解析が失敗したら、途中で得たデータを設定として扱わない。
            data = nlohmann::json::object();
        }
    }

    // 読んだ設定がオブジェクトでなければ、項目ごとの既定値を使える空の設定に戻す。
    if (!data.is_object()) {
        data = nlohmann::json::object();
    }

    // 項目ごとの欠損や型違いを吸収し、無限大やNaNを音量・感度へ渡さない。
    const auto readFloat = [&](const char* key, float fallback) {
        const auto it = data.find(key);

        // 指定されたキーがないか数値でない場合は、その項目の既定値を使う。
        if (it == data.end() || !it->is_number()) {
            return fallback;
        }

        const float value = it->get<float>();
        return std::isfinite(value) ? value : fallback;
    };

    const auto readBool = [&](const char* key, bool fallback) {
        const auto it = data.find(key);
        // 真偽値の項目だけを採用し、欠損や別の型なら指定された代替値を返す。
        return it != data.end() && it->is_boolean() ? it->get<bool>() : fallback;
    };

    // 数値をゲーム側が扱える範囲へ収め、反転はキーがなければ無効にする。
    bgmVolume_ = ClampVolume(readFloat("bgmVolume", bgmVolume_));
    seVolume_ = ClampVolume(readFloat("seVolume", seVolume_));
    cameraSensitivity_ = ClampSensitivity(readFloat("cameraSensitivity", cameraSensitivity_));
    cameraInvertX_ = readBool("cameraInvertX", false);
    cameraInvertY_ = readBool("cameraInvertY", false);

    // 天候は保存された整数が列挙値の範囲にあるときだけ採用する。
    const auto mode = data.find("stageSelectWeatherMode");

    // 天候設定が整数で保存されている場合だけ、列挙値の範囲を確認する。
    if (mode != data.end() && mode->is_number_integer()) {
        const auto maxMode = static_cast<uint64_t>(StageSelectWeatherMode::kRain);

        // 符号なし整数は上限を確認してから列挙値へ変換する。
        if (mode->is_number_unsigned()) {
            const uint64_t value = mode->get<uint64_t>();

            // 値が雨モードを超えていなければ、保存された天候を採用する。
            if (value <= maxMode) {
                stageSelectWeatherMode_ = ClampStageSelectWeatherMode(static_cast<std::int32_t>(value));
            }
        // 符号付き整数の場合は、負の値も除外してから採用する。
        } else {
            const int64_t value = mode->get<int64_t>();

            // 0から雨モードまでの範囲に収まる場合だけ、保存値を整数へ変換する。
            if (value >= 0 && static_cast<uint64_t>(value) <= maxMode) {
                stageSelectWeatherMode_ = ClampStageSelectWeatherMode(static_cast<std::int32_t>(value));
            }
        }
    }

    // 保存値を読み終えた時点で、音量を音声システムにも反映する。
    ApplyAudioSettings();
}

bool GameSettingsManager::Save() const {
    // この画面の設定キーを書き、廃止した明るさ・解像度番号・フルスクリーンのキーを削除する。
    return jsonFile::UpdateObject(filePath_, {
        {"bgmVolume", bgmVolume_},
        {"seVolume", seVolume_},
        {"cameraSensitivity", cameraSensitivity_},
        {"cameraInvertX", cameraInvertX_},
        {"cameraInvertY", cameraInvertY_},
        {"stageSelectWeatherMode", static_cast<std::int32_t>(stageSelectWeatherMode_)}
    }, {"brightness", "resolutionIndex", "fullscreen"});
}

void GameSettingsManager::SetBGMVolume(float volume) {
    // 音量調整は保存前でも再生中の BGM に即時適用する。
    bgmVolume_ = ClampVolume(volume);
    AudioPlayer::GetInstance()->SetBGMMasterVolume(bgmVolume_);
}

void GameSettingsManager::SetSEVolume(float volume) {
    // 音量調整は保存前でも再生中の SE に即時適用する。
    seVolume_ = ClampVolume(volume);
    AudioPlayer::GetInstance()->SetSEMasterVolume(seVolume_);
}

void GameSettingsManager::SetCameraSensitivity(float sensitivity) {
    // 操作側が参照する値を有効範囲で保持する。
    cameraSensitivity_ = ClampSensitivity(sensitivity);
}

void GameSettingsManager::SetStageSelectWeatherMode(StageSelectWeatherMode mode) {
    // UI からの設定でも未知の値を自動モードへ丸める。
    stageSelectWeatherMode_ = ClampStageSelectWeatherMode(static_cast<std::int32_t>(mode));
}

void GameSettingsManager::ApplyAudioSettings() const {
    // 起動時の読込と再初期化で、保持値を音声ミキサーへ同期する。
    AudioPlayer::GetInstance()->SetBGMMasterVolume(bgmVolume_);
    AudioPlayer::GetInstance()->SetSEMasterVolume(seVolume_);
}
