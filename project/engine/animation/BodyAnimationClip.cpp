#define NOMINMAX
#include <cstdint>
#include "BodyAnimationClip.h"

#include "engine/utility/math/AnimationInterpolation.h"
#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;

namespace {
constexpr const char* kBodyAnimationDirectory = "Resources/json/animation_clip/";
constexpr const char* kLegacyAnimationDirectory = "Resources/json/enemy_animation/";
constexpr float kTimeEpsilon = 0.0001f;

engine::Vector3 ReadVector3(const json& object, const char* key, const engine::Vector3& fallback) {
    // キーがないか3成分の配列を読めない場合は、その姿勢要素の既定値を使う。
    if (!object.contains(key) || !object[key].is_array() || object[key].size() < 3) {
        return fallback;
    }

    const auto& values = object[key];
    return {
        values[0].get<float>(),
        values[1].get<float>(),
        values[2].get<float>()
    };
}

animationInterpolation::EasingType ToEasing(std::int32_t value) {
    // JSONの不正な番号で補間テーブルの範囲外を参照しないよう制限する。
    return static_cast<animationInterpolation::EasingType>(std::clamp(value, 0, 4));
}
}

void BodyAnimationClip::Clear() {
    // 読込失敗時に前回のキーが残らないよう、メタ情報と姿勢をまとめて消す。
    name_.clear();
    modelName_.clear();
    duration_ = 0.0f;
    loopDefault_ = true;
    keys_.clear();
}

bool BodyAnimationClip::Load(const std::string& filePath) {
    // 前回の姿勢を再利用しないよう、開く前に読み込み状態を空にする。
    Clear();

    std::ifstream file(filePath, std::ios::binary);

    // ファイルを開けなければ、有効なキーがない状態で読み込み失敗を返す。
    if (!file.is_open()) {
        return false;
    }

    try {
        json root;
        file >> root;

        // 名前がなければファイル名を使い、再生時間は最低0.01秒にする。
        name_ = root.value("name", std::filesystem::path(filePath).stem().string());
        modelName_ = root.value("model", "");
        duration_ = std::max(0.01f, root.value("duration", 1.0f));
        loopDefault_ = root.value("loop", true);

        // キー時刻は秒です。XZのせん断成分だけを姿勢評価へ渡し、Y成分は0で保持します。
        const json bodyKeys = root.value("bodyKeys", json::array());

        for (const auto& item : bodyKeys) {
            Key key;
            key.time = std::max(0.0f, item.value("time", 0.0f));
            key.translate = ReadVector3(item, "translate", {});
            key.rotate = ReadVector3(item, "rotate", {});
            key.scale = ReadVector3(item, "scale", { 1.0f, 1.0f, 1.0f });
            key.shear = ReadVector3(item, "shear", {});
            key.shear.y = 0.0f;
            key.shearPivotY = item.value("shearPivotY", 0.0f);
            key.easingToNext = std::clamp(item.value("easingToNext", 4), 0, 4);
            keys_.push_back(key);
        }

        // JSON の記述順には依存せず、隣り合うキーで補間できるよう時刻順へ並べる。
        std::sort(keys_.begin(), keys_.end(), [](const Key& lhs, const Key& rhs) {
            return lhs.time < rhs.time;
        });

        // キーがある場合だけ最後の時刻を参照する。
        if (!keys_.empty()) {
            // 宣言時間が短くても最後の姿勢キーまで再生できるように、終了秒を後ろへ延ばします。
            duration_ = std::max(duration_, keys_.back().time);
        }
    // JSONの解析や型変換で失敗したときは、途中まで入ったメタ情報とキーも破棄する。
    } catch (...) {
        Clear();
        return false;
    }

    // 読めたJSONでも、評価に使えるキーがなければ読み込み失敗として扱う。
    return IsValid();
}

bool BodyAnimationClip::Evaluate(float timeSeconds, bool loop, Sample& sampleOut) const {
    // 評価に失敗したときに呼び出し側へ以前の姿勢を残さないよう、先に出力を初期化する。
    sampleOut = {};

    // 姿勢キーがないか再生時間が正でなければ、補間区間を選べないため終了する。
    if (!IsValid()) {
        return false;
    }

    float sampleTime = std::max(0.0f, timeSeconds);

    // ループ時は duration を周期にし、単発時は末尾の姿勢を維持する。
    if (loop && duration_ > kTimeEpsilon) {
        sampleTime = std::fmod(sampleTime, duration_);

        // 剰余が負なら周期を1回足して戻す。入力は上で0秒以上に制限している。
        if (sampleTime < 0.0f) {
            sampleTime += duration_;
        }
    // 単発再生または周期が短すぎる場合は剰余を使わず、末尾時刻で止める。
    } else {
        sampleTime = std::min(sampleTime, duration_);
    }

    const auto copyKeyToSample = [&sampleOut](const Key& key) {
        sampleOut.translate = key.translate;
        sampleOut.rotate = key.rotate;
        sampleOut.scale = key.scale;
        sampleOut.shear = key.shear;
        sampleOut.shearPivotY = key.shearPivotY;
    };

    // キーが1個だけ、または再生時刻が最初のキー以前なら、先頭の姿勢をそのまま使う。
    if (keys_.size() == 1 || sampleTime <= keys_.front().time) {
        copyKeyToSample(keys_.front());
        return true;
    }

    // 評価時刻が末尾キー以降なら、次周または再生終端まで最後の姿勢を保つ。
    if (sampleTime >= keys_.back().time) {
        copyKeyToSample(keys_.back());
        return true;
    }

    // 先頭と末尾の間では、評価時刻を挟む隣り合ったキーを探す。
    for (size_t index = 0; index + 1 < keys_.size(); ++index) {
        const Key& from = keys_[index];
        const Key& to = keys_[index + 1];

        // この2キーの時刻範囲外なら、次の区間へ進む。
        if (sampleTime < from.time || sampleTime > to.time) {
            continue;
        }

        // 評価秒が入るキー区間を見つけてから、その区間内の補間に渡します。
        sampleOut = InterpolatePose(from, to, sampleTime, kTimeEpsilon);
        return true;
    }

    // 一致する区間を選び切れなかった場合も、最後の姿勢を返す。
    copyKeyToSample(keys_.back());
    return true;
}

BodyAnimationClip::Sample BodyAnimationClip::InterpolatePose(
    const Key& from, const Key& to, float time, float minimumSegmentDuration) {
    // 同時刻のキーでもゼロ除算せず、開始キーが指定する補間曲線を使う。
    const float segmentDuration = std::max(minimumSegmentDuration, to.time - from.time);
    const float rawRate = (time - from.time) / segmentDuration;
    const float rate = animationInterpolation::ApplyEasing(rawRate, ToEasing(from.easingToNext));

    // 回転は球面補間し、残りの姿勢は同じ進行率で線形補間する。
    Sample pose;
    pose.translate = animationInterpolation::Lerp(from.translate, to.translate, rate);
    pose.rotate = animationInterpolation::SlerpEuler(from.rotate, to.rotate, rate);
    pose.scale = animationInterpolation::Lerp(from.scale, to.scale, rate);
    pose.shear = animationInterpolation::Lerp(from.shear, to.shear, rate);
    pose.shearPivotY = engine::Math::Lerp(from.shearPivotY, to.shearPivotY, rate);
    return pose;
}

std::string BodyAnimationClip::ResolveAssetPath(const std::string& assetNameOrPath) {
    // 空の名前は素材の指定なしとして、候補を組み立てず終了する。
    if (assetNameOrPath.empty()) {
        return {};
    }

    std::filesystem::path path(assetNameOrPath);

    // フォルダー付きなら呼び出し側が指定した場所を使い、素材ディレクトリの検索は行わない。
    if (path.has_parent_path()) {
        // 拡張子を省略した明示パスにだけJSONの拡張子を補う。
        if (path.extension().empty()) {
            path += ".json";
        }

        return path.generic_string();
    }

    std::string fileName = path.string();

    // ファイル名だけの場合も、拡張子の省略時だけJSONを補う。
    if (path.extension().empty()) {
        fileName += ".json";
    }

    const std::filesystem::path primary = std::filesystem::path(kBodyAnimationDirectory) / fileName;

    // 現行の素材ディレクトリにあれば、旧配置を検索せず採用する。
    if (std::filesystem::exists(primary)) {
        return primary.generic_string();
    }

    const std::filesystem::path legacy = std::filesystem::path(kLegacyAnimationDirectory) / fileName;

    // 現行側に存在せず旧配置にあれば、既存の素材を読み込めるよう旧パスを返す。
    if (std::filesystem::exists(legacy)) {
        return legacy.generic_string();
    }

    // 両方に見つからない場合は、呼び出し側が失敗を報告できるよう現行側の候補を返す。
    return primary.generic_string();
}
