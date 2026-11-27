#pragma once

#include "ObjectComponent.h"

#include <string>

// 担当：記録経路の名前、ループ・相対再生・乗車条件を保持する。
// 関連：ObjectComponent（共通型）、Object3D（保持）、GhostRecorder（経路再生）
// 主な入口：GetPathName / SetPathName / GetPlaybackMode
class PathMoverComponent final : public ObjectComponent {
public:
    static constexpr std::string_view kTypeId = "PathMover"; // 保存と実行時の型判定に使う固定ID。

    /// <summary>保存と型判定に使うComponent種別を返す。</summary>
    std::string_view GetTypeId() const override { return kTypeId; }

    // 排他的な再生条件。旧データは常時再生として扱う。
    // Alwaysは常時、WhileRidingは乗車中だけ、RideToEndは乗車を契機に終端まで進む指定。
    enum class PlaybackMode { kAlways, kWhileRiding, kRideToEnd };
    // 記録経路を再生する条件を返す。
    PlaybackMode GetPlaybackMode() const { return playbackMode_; }
    // 再生する条件を保存する。条件を評価して再生する処理は利用側が担当する。
    void SetPlaybackMode(PlaybackMode mode) { playbackMode_ = mode; }
    // 再生条件をシーンへ保存する識別文字列に変換する。
    std::string GetPlaybackModeName() const {
        switch (playbackMode_) {
        // 乗り続けている間だけ再生する設定。
        case PlaybackMode::kWhileRiding: return "whileRiding";
        // 乗車後は降りても終端まで再生する設定。
        case PlaybackMode::kRideToEnd: return "rideToEnd";
        // 常時再生と未定義の列挙値は従来のalwaysとして保存する。
        default: return "always";
        }
    }
    // 保存されている識別文字列を読み、再生条件に変換する。
    void SetPlaybackModeName(const std::string& name) {
        // 不明な値も旧版と同じ動作へ戻す。
        playbackMode_ = name == "whileRiding" ? PlaybackMode::kWhileRiding :
            name == "rideToEnd" ? PlaybackMode::kRideToEnd : PlaybackMode::kAlways;
    }

    // 経路名、ループ、相対座標再生の設定。
    const std::string& GetPathName() const { return pathName_; }

    // 既存の記録経路を参照する名前を保存する。空で解除する。
    void SetPathName(const std::string& name) { pathName_ = name; }

    // 記録の終端から先頭に戻る設定か返す。
    bool IsLoop() const { return loop_; }

    void SetLoop(bool loop) { loop_ = loop; }

    // 現在の配置を基準に相対座標で再生するか返す。
    bool IsRelative() const { return relative_; }

    void SetRelative(bool relative) { relative_ = relative; }

    // ループの開始位置を周期の割合でずらし、同じパスを使う障害物の動きを揃えすぎない。
    float GetPhaseOffset() const { return phaseOffset_; }
    // 値を保持するだけで、ここでは0〜1への制限や経路の再計算をしない。
    void SetPhaseOffset(float phase) { phaseOffset_ = phase; }

    // 乗車型の床は演出タイムラインを実行する。空なら従来のPath再生を維持する。
    const std::string& GetRideSequence() const { return rideSequence_; }

    // 乗車用のタイムラインのパスを設定する。空なら記録経路の設定を使う。
    void SetRideSequence(const std::string& path) { rideSequence_ = path; }

private:
    PlaybackMode playbackMode_ = PlaybackMode::kAlways; // 常時・乗車中・乗車後終端までの再生条件。
    std::string pathName_; // 記録経路の名前。空なら未設定。
    bool loop_ = false; // 経路の終端から先頭へ戻すか。
    bool relative_ = false; // 記録経路を基準姿勢に対する相対座標で使うか。
    float phaseOffset_ = 0.0f; // ループ開始位置の割合。保持時には範囲制限しない。
    std::string rideSequence_; // 乗車時の演出シーケンス。空なら通常の経路再生。
};
