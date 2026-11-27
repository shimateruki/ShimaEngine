#include <cstdint>
#include "engine/animation/cinematic/CinematicAssetPath.h"
#define NOMINMAX
#include "engine/animation/cinematic/CinematicSequence.h"

#include "json.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <numbers>
#include "engine/utility/math/AnimationInterpolation.h"

namespace engine {

using json = nlohmann::json;
void SaveCinematicPresentation(json&, const CinematicPresentation&);
void LoadCinematicPresentation(const json&, CinematicPresentation&);

CinematicVisualKey SampleCinematicVisual(const CinematicObjectClip& clip, float time, const CinematicSequence* sequence) {
    // 区間設定がないクリップだけ、既存の見た目キー列を時刻で評価する。
    if (clip.motions.empty()) {
        return SampleCinematicKeys(clip.keys, time, [](const auto& a, const auto& b, float seconds) {
            auto k = a;
            static_cast<CinematicTransformKey&>(k) = InterpolateCinematicTransform(a,b,seconds,true);
            // 位置・姿勢の補間とは別に、色、発光、クリップ境界と跳ね幅をキー間の時間比でつなぐ。
            const float rate = std::clamp((seconds-a.time)/std::max(0.000001f,b.time-a.time),0.0f,1.0f);
            k.color = Math::Lerp(a.color,b.color,rate);
            k.emissive = Math::Lerp(a.emissive,b.emissive,rate);
            k.clipHeight = Math::Lerp(a.clipHeight,b.clipHeight,rate);
            k.bounce = Math::Lerp(a.bounce,b.bounce,rate);
            return k;
        });
    }
    // Signalがある区間は同じ節目を参照し、時刻を別のキー列へ複製しない。
    const auto timing = [sequence](float seconds, const std::string& signal, float offset) {
        return sequence && !signal.empty() ? sequence->GetSignalTime(signal,seconds) + offset : seconds;
    };

    // 区間の隙間では直前の終点を保持し、先頭より前は始点を返す。
    const CinematicMotionSegment* selected = &clip.motions.back();
    float sampleTime = time;
    for (size_t i=0; i<clip.motions.size(); ++i) {
        const auto& motion = clip.motions[i];
        const float begin = timing(motion.startTime,motion.startSignal,motion.startSignalOffset);
        // 次の区間に達していなければ、先頭区間の始点か直前区間の終点を保持する。
        if (time < begin) {
            selected = &clip.motions[i==0?0:i-1];
            sampleTime = i==0 ? begin : timing(selected->endTime,selected->endSignal,selected->endSignalOffset);
            break;
        }
        // 開始に達していて終了より前なら、現在時刻に対応する区間として採用する。
        if (time < timing(motion.endTime,motion.endSignal,motion.endSignalOffset)) { selected=&motion; break; }
    }

    const auto& motion = *selected;
    const float begin = timing(motion.startTime,motion.startSignal,motion.startSignalOffset);
    const float end = timing(motion.endTime,motion.endSignal,motion.endSignalOffset);
    // 秒で表した区間を0～1へ正規化します。移動の進み方を滑らかにしても、色などは変換前の割合で変化させます。
    const float raw = std::clamp((sampleTime-begin)/std::max(0.000001f,end-begin),0.0f,1.0f);
    const auto smooth=[](float x) { x=std::clamp(x,0.0f,1.0f); return x*x*(3.0f-2.0f*x); };
    const float progress = motion.smoothProgress ? smooth(raw) : raw;
    auto k = motion.from;
    k.time = time;
    k.positionReference = motion.from.positionReference;
    k.positionTarget = motion.to.positionReference;
    k.positionMix = progress;
    // 基準点間の補間は再生器が担当し、ここでは共通の弧と姿勢だけを評価する。
    // 進行率を0～πラジアンに写すと、弧の高さは両端で0、中点で最大になります。
    const float arc = std::sin(progress*std::numbers::pi_v<float>);
    k.position = Math::Lerp(motion.from.position,motion.to.position,progress);
    k.position.y += arc*motion.arcHeight;

    // 着地の揺れは時間とともにゼロへ収束する。フレーム積分を使わず巻戻しも再現する。
    // 跳ね返りは移動区間全体の割合と独立して、区間開始から bounceDuration 秒で収まります。
    const float settle = std::clamp((sampleTime-begin)/std::max(0.000001f,motion.bounceDuration),0.0f,1.0f);
    k.position.y += std::abs(std::sin(settle*2.0f*std::numbers::pi_v<float>*motion.bounceCycles))*(1.0f-settle)*motion.bounceHeight;
    k.rotation = Math::Lerp(motion.from.rotation,motion.to.rotation,progress)+motion.rotationArc*std::sin(progress*2.0f*std::numbers::pi_v<float>*motion.rotationCycles);

    // 移動の後半だけ縮める場合も、同じ進行割合から倍率を求める。
    float scaleRate = std::clamp((progress-motion.scaleStart)/std::max(0.000001f,1.0f-motion.scaleStart),0.0f,1.0f);
    // 拡縮の補間に平滑化を指定した場合は、縮小の進み方も両端で緩める。
    if (motion.smoothScale) scaleRate=smooth(scaleRate);
    // 初期の伸びを指数的に戻す場合と、着地で潰れを揺らして戻す場合を共有する。
    const float envelope = motion.scaleDecay > 0 ? std::exp(-scaleRate*motion.scaleDecay) : 1.0f-scaleRate;
    const float remaining = envelope*std::cos(scaleRate*2.0f*std::numbers::pi_v<float>*motion.scaleCycles);
    k.scale = Math::Lerp(motion.to.scale,motion.from.scale,remaining);
    // 移動中だけ加える倍率の揺れは開始から終了までに限定します。終了後に終点を保持する区間には持ち越しません。
    if (time >= begin && time < end) {
        const float amplitude = motion.scaleWaveFadeIn ? smooth(raw) : 1.0f;
        k.scale += motion.scaleWaveAmplitude * (std::sin(time * motion.scaleWaveFrequency) * amplitude);
    }
    // 軸ごとに最小倍率を適用し、揺れによる縮みが設定した下限を越えないようにする。
    k.scale = {std::max(k.scale.x,motion.minimumScale),std::max(k.scale.y,motion.minimumScale),std::max(k.scale.z,motion.minimumScale)};

    // 色は時間比でつなぎ、発光の波の位相は演出開始からの絶対秒で求める。表示は区間の終端で切り替える。
    k.color = Math::Lerp(motion.from.color,motion.to.color,raw);
    k.emissive = Math::Lerp(motion.from.emissive,motion.to.emissive,raw)+motion.emissiveAmplitude*std::sin(time*motion.emissiveFrequency);
    k.visible = raw>=1.0f ? motion.to.visible : motion.from.visible;
    return k;
}

float CinematicSequence::GetVfxStartTime(const CinematicVfxTrackData& track) const {
    // Signalが指定されていればその時刻を使い、未指定か見つからない場合はトラックの開始秒を使う。
    return track.startSignal.empty() ? track.startTime : GetSignalTime(track.startSignal, track.startTime);
}

float CinematicSequence::GetSignalTime(const std::string& name, float fallback) const {
    // 有効なマーカーのうち、表示名または通知名が一致する最初のものを探す。
    const auto marker = std::find_if(signals.begin(), signals.end(), [&](const auto& signal) {
        return signal.enabled && (signal.name == name || signal.signal == name);
    });
    // 有効な一致がない場合も、呼び出し側が指定した秒で演出を進められるようにする。
    return marker != signals.end() ? marker->time : fallback;
}

CinematicTransformKey InterpolateCinematicTransform(const CinematicTransformKey& a,
    const CinematicTransformKey& b, float time, bool continuousRotation) {
    // 連続回転の素材では区間が狭くても独自に時間比を求め、ほかは共通の区間用計算を使う。
    const float raw = continuousRotation
        ? std::clamp((time-a.time)/std::max(0.000001f,b.time-a.time),0.0f,1.0f)
        : animationInterpolation::SegmentRate(time,a.time,b.time);
    const float rate = animationInterpolation::ApplyEasing(raw,
        static_cast<animationInterpolation::EasingType>(std::clamp(a.easingToNext,0,4)));
    auto value = a;
    value.time = time;
    value.position = animationInterpolation::Lerp(a.position,b.position,rate);
    // 回転量をそのままつなぐ素材ではEuler角を補間し、通常の姿勢補間ではクォータニオンで最短向きを選びます。
    value.rotation = continuousRotation ? animationInterpolation::Lerp(a.rotation,b.rotation,rate)
                                       : animationInterpolation::SlerpEuler(a.rotation,b.rotation,rate);
    value.scale = animationInterpolation::Lerp(a.scale,b.scale,rate);
    return value;
}

// Editorの軌跡表示と再生器で、同じアンカー基準の座標変換を使います。
engine::Vector3 CinematicAnchorPosition(const engine::Vector3& local, const engine::Vector3& origin, float yaw) {
    // アンカーの向きでローカル座標を回してから、ワールド原点へ移す。
    const float c = std::cos(yaw), s = std::sin(yaw);
    return {origin.x + local.x * c + local.z * s, origin.y + local.y, origin.z - local.x * s + local.z * c};
}

engine::Vector3 CinematicAnchorLocalPosition(const engine::Vector3& world, const engine::Vector3& origin, float yaw) {
    // 編集したワールド位置を逆回転し、保存用のアンカー相対座標へ戻す。
    return CinematicAnchorPosition({world.x - origin.x, world.y - origin.y, world.z - origin.z}, {}, -yaw);
}

// JSONライブラリの型変換を使い、保存と読込の項目一覧を一か所にする。
// 座標の変換はこの翻訳単位だけで使い、他のJSONの保存形式へ影響させない。
static void to_json(json& data, const engine::Vector3& value) {
    data = {value.x, value.y, value.z};
}
static void from_json(const json& data, engine::Vector3& value) {
    value = {};
    // 3成分の配列があれば座標として読む。それ以外では初期化したゼロのままにする。
    if (data.is_array() && data.size() >= 3) value = {data[0], data[1], data[2]};
}
// 各行から対応する型のJSON変換が生成されます。指定のキーは保存時に書き出し、読み込み時に省略されていれば型の既定値を使います。
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CinematicObjectBinding, targetName, targetEventId)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CinematicTransformKey,
    time, position, rotation, scale, easingToNext)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CinematicTransformTrack,
    name, binding, legacyPathFile, startTime, enabled, muted, locked, relative, anchorRelative, holdLast, keys)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CinematicVfxTrackData,
    name, binding, sequenceName, startTime, duration, startSignal, snapshotTargetPosition, rootOffset, rootScale, enabled, muted)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CinematicCameraShot,
    name, binding, startTime, duration, blendInDuration, blendOutDuration, easing, enabled, muted)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CinematicAnimationClipData,
    name, binding, driver, clipName, startTime, duration, playbackSpeed, blendInDuration, easing, loop, restoreOnStop, enabled, muted)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CinematicAudioClipData,
    name, audioId, startTime, duration, volume, loop, enabled, muted)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CinematicSignalMarker,
    time, name, signal, payload, enabled, binding)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CinematicEventMarker, time, eventId, name, binding)

namespace {
// 既存素材の平坦なBinding・旧項目名は入口と出口だけで変換する。
template<class T> json WriteTracks(const std::vector<T>& tracks) {
    auto items = json::array();
    for (const auto& track : tracks) {
        json item = track;
        // 対象を指定できる型だけ、内部のbindingを旧形式と同じ平坦な項目に戻す。
        if constexpr (requires { track.binding; }) {
            // 既存ファイルと同じ形式を保つため、対象名とイベントIDをトラック直下へ戻します。
            item.update(item.at("binding"));
            item.erase("binding");
        }
        // 旧移動素材の参照を持つ型は、従来のファイル名の項目で保存する。
        if constexpr (requires { track.legacyPathFile; }) {
            item["pathFileName"] = track.legacyPathFile;
            item.erase("legacyPathFile");
        }
        // 変形トラックとVFXは、開始秒を読む旧ツール向けにdelayTimeも保存する。
        if constexpr (requires { track.legacyPathFile; } || requires { track.sequenceName; })
            item["delayTime"] = track.startTime;
        items.push_back(std::move(item));
    }
    return items;
}

// 省略値は従来通りとし、変更した素材だけ形式が変わることを避ける。
template<class T> void ReadTracks(const json& items, std::vector<T>& tracks, const char* defaultName) {
    for (auto item : items) {
        // ファイル直下の対象情報を実行時の binding へ再構成し、従来の名前や遅延秒を現行の項目へ読み替えます。
        // 対象指定を持つ型は、平坦な名前とイベントIDを内部のbindingへ組み直す。
        if constexpr (requires { T{}.binding; }) item["binding"] = item.get<CinematicObjectBinding>();
        // 変形トラックにだけ、旧パス参照と姿勢キーの既定値を補う。
        if constexpr (requires { T{}.legacyPathFile; }) {
            item["name"] = item.value("name", item.value("targetName", ""));
            item["legacyPathFile"] = item.value("pathFileName", "");
            // 旧データに姿勢キーの項目がなければ、空配列で後続の変換を通す。
            if (!item.contains("keys")) item["keys"] = json::array();
            for (auto& key : item["keys"])
                // 不完全な拡縮だけを除き、型の既定倍率を使用できるようにする。
                if (key.contains("scale") && (!key["scale"].is_array() || key["scale"].size() < 3)) key.erase("scale");
        // VFXトラックでは、未命名時の表示名に素材名を利用する。
        } else if constexpr (requires { T{}.sequenceName; }) {
            item["name"] = item.value("name", item.value("sequenceName", "VFX"));
            // VFXのルート拡縮が3成分未満や配列以外なら、読み込み時の既定倍率に任せる。
            if (item.contains("rootScale") && (!item["rootScale"].is_array() || item["rootScale"].size() < 3)) item.erase("rootScale");
        // カメラカットでは、名前を省略したときに対象カメラの名前を使う。
        } else if constexpr (std::is_same_v<T, CinematicCameraShot>)
            item["name"] = item.value("name", item.value("targetName", "Camera Shot"));
        // 固有の旧形式を持たない項目は、呼び出し側が指定する既定名を使う。
        else item["name"] = item.value("name", defaultName);
        // 開始時刻を新しい項目から優先し、変形とVFXに限って旧来の遅延秒も受け付ける。
        if constexpr (requires { T{}.legacyPathFile; } || requires { T{}.sequenceName; })
            item["startTime"] = item.value("startTime", item.value("delayTime", 0.0f));
        // ドライバーを持つアニメーションクリップでは、旧ファイルの省略時にModelを補う。
        if constexpr (requires { T{}.driver; }) item["driver"] = item.value("driver", "Model");
        tracks.push_back(item.get<T>());
    }
}
} // namespace

void CinematicSequence::Clear() {
    // 次の演出を読み込む前に全トラックとプレビュー用の役割を初期状態へ戻す。
    anchor = {};
    anchorYawOffset = 0.0f;
    roleBindings = {};
    previewModels = {"Characters/slime_pink", "Primitives/cube"};
    version = kCurrentVersion;
    duration = 0.0f;
    transformTracks.clear();
    vfxTracks.clear();
    cameraShots.clear();
    animationClips.clear();
    audioClips.clear();
    signals.clear();
    events.clear();
    presentation = {};
}

// 再生側が時刻順に走査できるよう、評価に順序が必要な項目だけ並べ替えます。
void CinematicSequence::Sort() {
    // 見た目と注視の同時刻キーには既存の順序を残し、区間探索での選択を保つ。
    for (auto& clip : presentation.objects)
        std::stable_sort(clip.keys.begin(), clip.keys.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    std::stable_sort(presentation.view.begin(), presentation.view.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    // 補間が前後のキーを正しく見つけられるよう、各トラックを時刻順に揃える。
    for (auto& track : transformTracks) {
        std::sort(track.keys.begin(), track.keys.end(),
                  [](const CinematicTransformKey& a, const CinematicTransformKey& b) { return a.time < b.time; });
    }

    // 再生中に開始や通知の節目を走査する項目は、各開始時刻または発火時刻で並べる。
    std::sort(events.begin(), events.end(),
              [](const CinematicEventMarker& a, const CinematicEventMarker& b) { return a.time < b.time; });
    std::sort(cameraShots.begin(), cameraShots.end(),
              [](const CinematicCameraShot& a, const CinematicCameraShot& b) { return a.startTime < b.startTime; });
    std::sort(animationClips.begin(), animationClips.end(),
              [](const CinematicAnimationClipData& a, const CinematicAnimationClipData& b) {
                  return a.startTime < b.startTime;
              });
    std::sort(
        audioClips.begin(), audioClips.end(),
        [](const CinematicAudioClipData& a, const CinematicAudioClipData& b) { return a.startTime < b.startTime; });
    std::sort(signals.begin(), signals.end(),
              [](const CinematicSignalMarker& a, const CinematicSignalMarker& b) { return a.time < b.time; });
}

// 入力されたdurationより長いトラックがあれば、末尾まで再生時間を延ばします。
float CinematicSequence::GetAuthoredDuration() const {
    // 設定上の長さと各トラックの最後のキーを比較し、途中で演出を切らない。
    float result = std::max(0.0f, duration);
    for (const auto& clip : presentation.objects)
        // 有効でキーがある見た目クリップだけ、最後のキーを必要尺に含める。
        if (clip.enabled && !clip.keys.empty()) result = std::max(result, clip.keys.back().time);
    for (const auto& clip : presentation.objects)
        // 有効なクリップでは区間設定の終了も含め、Signalに追従する場合はオフセットを加える。
        if (clip.enabled) for (const auto& motion : clip.motions)
            result = std::max(result, motion.endSignal.empty() ? motion.endTime : GetSignalTime(motion.endSignal,motion.endTime) + motion.endSignalOffset);
    // 注視キーがある場合は、最後のカメラ姿勢にも到達する尺を確保する。
    if (!presentation.view.empty()) result = std::max(result, presentation.view.back().time);

    // 移動キーの時刻はトラック開始からの秒です。開始時刻を足して演出全体での終了時刻を求めます。
    for (const auto& track : transformTracks) {
        // キーを持つ変形トラックだけ、開始秒に末尾キーの相対秒を足す。
        if (!track.keys.empty()) {
            result = std::max(result, track.startTime + std::max(0.0f, track.keys.back().time));
        }
    }

    // VFXの起点はシグナル参照も解決した秒です。そこから発生枠の長さを含めます。
    for (const auto& track : vfxTracks) {
        result = std::max(result, GetVfxStartTime(track) + std::max(0.1f, track.duration));
    }

    // カメラ・アニメーション・音声ではそれぞれ開始秒に最小0.05秒の長さを足して比較する。
    for (const auto& shot : cameraShots) {
        result = std::max(result, shot.startTime + std::max(0.05f, shot.duration));
    }

    for (const auto& clip : animationClips) {
        result = std::max(result, clip.startTime + std::max(0.05f, clip.duration));
    }

    for (const auto& clip : audioClips) {
        result = std::max(result, clip.startTime + std::max(0.05f, clip.duration));
    }

    // 最後の素材が消えた後でも、後続のシグナルやイベントを通知できる長さを確保します。
    for (const auto& signal : signals) {
        result = std::max(result, signal.time);
    }

    for (const auto& event : events) {
        result = std::max(result, event.time);
    }

    return std::max(result, 0.1f);
}

// 保存と読込は同じ型変換を通す。旧項目の扱いは上の互換入口へ閉じ込める。
std::string CinematicSequence::ToJsonText(std::int32_t indent) const {
    json root = {{"version", kCurrentVersion}, {"name", name}, {"duration", duration},
        {"anchor", anchor}, {"anchorYawOffset", anchorYawOffset},
        {"roleBindings", roleBindings}, {"previewModels", previewModels},
        {"tracks", WriteTracks(transformTracks)}, {"vfxCues", WriteTracks(vfxTracks)},
        {"cameraShots", WriteTracks(cameraShots)}, {"animationClips", WriteTracks(animationClips)},
        {"audioClips", WriteTracks(audioClips)}, {"signals", WriteTracks(signals)}, {"events", WriteTracks(events)}};
    // トラックに続けて見た目、注視、粒子の設定を同じJSONへ格納してから文字列にする。
    SaveCinematicPresentation(root, presentation);
    return root.dump(indent);
}

bool CinematicSequence::Save(const std::filesystem::path& filePath) const {
    const auto text = ToJsonText(4);
    const std::filesystem::path path(filePath);
    // 親フォルダーが指定されている場合は、初回保存でも開けるよう作成する。
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << text;
    // クローズで書き込みを終え、エラーが残っていないかを呼び出し側へ返す。
    file.close();
    return file.good();
}

bool CinematicSequence::Load(const std::filesystem::path& filePath) {
    const auto resolved = cinematicAssetPath::Resolve("Resources/json/scenario", filePath);
    std::ifstream file(resolved, std::ios::binary);
    // 指定先も互換探索先も開けない場合は、現在のシーケンスを保持して失敗を返す。
    if (!file) return false;
    // 先にJSONの構文だけを解析する。ここで失敗した場合も、現在の設定を残す。
    json root;
    try { file >> root; }
    catch (...) { return false; }
    try {
        Clear();
        // 構文を読めたら全項目を初期化し、未記載の項目には旧形式でも読み込める既定値を補います。
        version = root.value("version", 1);
        const auto stem = filePath.stem().u8string();
        name = root.value("name", std::string(stem.begin(), stem.end()));
        duration = root.value("duration", 0.0f);
        anchor = root.value("anchor", json::object()).get<CinematicObjectBinding>();
        anchorYawOffset = root.value("anchorYawOffset", 0.0f);
        // プレビュー用モデルと実シーンへの役割割り当ては、定義済みの枠に入る分だけ読む。
        const auto roles = root.value("roleBindings", json::array());
        const auto models = root.value("previewModels", json::array());
        previewModels = {"Characters/slime_pink", "Gimmicks/portal_gate"};
        for (size_t i = 0; i < previewModels.size() && i < models.size(); ++i) previewModels[i] = models[i].get<std::string>();
        for (size_t i = 0; i < roleBindings.size() && i < roles.size(); ++i) roleBindings[i] = roles[i].get<CinematicObjectBinding>();
        ReadTracks(root.value("tracks", json::array()), transformTracks, "");
        // 現行の項目を優先し、存在しないファイルでは旧キー vfxTracks を参照します。
        ReadTracks(root.value("vfxCues", root.value("vfxTracks", json::array())), vfxTracks, "VFX");
        ReadTracks(root.value("cameraShots", json::array()), cameraShots, "Camera Shot");
        ReadTracks(root.value("animationClips", json::array()), animationClips, "Animation Clip");
        ReadTracks(root.value("audioClips", json::array()), audioClips, "Audio Clip");
        ReadTracks(root.value("signals", json::array()), signals, "Signal");
        ReadTracks(root.value("events", json::array()), events, "");
        LoadCinematicPresentation(root, presentation);
    // 意味上の項目変換に失敗した場合は、途中まで読んだトラックを消して失敗を返す。
    } catch (...) {
        Clear();
        return false;
    }
    // 読み込めたデータは再生側が時系列で探せる状態にしてから引き渡す。
    Sort();
    return true;
}

} // namespace engine
