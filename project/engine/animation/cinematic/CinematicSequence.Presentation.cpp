#include "engine/animation/cinematic/CinematicSequence.h"
#include "json.hpp"
#include <stdexcept>
#include <cmath>

namespace engine {

using json = nlohmann::json;

namespace {
engine::Vector3 V3(const json& a) { return {a.at(0).get<float>(), a.at(1).get<float>(), a.at(2).get<float>()}; }
engine::Vector4 V4(const json& a) { return {a.at(0).get<float>(), a.at(1).get<float>(), a.at(2).get<float>(), a.at(3).get<float>()}; }
json J3(const engine::Vector3& v) { return {v.x, v.y, v.z}; }
json J4(const engine::Vector4& v) { return {v.x, v.y, v.z, v.w}; }

// 演出の配列形式をここで選ぶ。項目を省略した既存データには型の既定値を使う。
Vector3 ReadMotionVector(const json& data, const char* field, Vector3 fallback) {
    const auto item = data.find(field);
    // 項目を省略した素材には、この姿勢またはばね設定の既定ベクトルを使う。
    if (item == data.end()) return fallback;
    // 値がある場合は3成分の数値配列が必要。形が異なる場合は読込失敗として扱う。
    if (!item->is_array() || item->size() != 3) throw std::runtime_error("Invalid spring vector");
    return V3(*item);
}

Vector4 ReadMotionColor(const json& data, const char* field, Vector4 fallback) {
    const auto item = data.find(field);
    // 色の項目がなければ、素材型の既定RGBAを保持する。
    if (item == data.end()) return fallback;
    // 色が指定された場合はアルファを含む4成分が必要。
    if (!item->is_array() || item->size() != 4) throw std::runtime_error("Invalid visual color");
    return V4(*item);
}
} // namespace

// 区間の始終点は名前付きオブジェクト、ベクトルと色は既存の数値配列で書き出す。
void to_json(json& data, const CinematicVisualKey& key) {
    data = {
        {"positionReference", key.positionReference},
        {"position", J3(key.position)},
        {"rotation", J3(key.rotation)},
        {"scale", J3(key.scale)},
        {"color", J4(key.color)},
        {"emissive", key.emissive},
        {"visible", key.visible},
    };
}

void from_json(const json& data, CinematicVisualKey& key) {
    // 区間端の姿勢・発光・表示を読み、項目がなければ既定の見た目を使う。
    const CinematicVisualKey defaults;
    key.positionReference = data.value("positionReference", defaults.positionReference);
    key.position = ReadMotionVector(data, "position", defaults.position);
    key.rotation = ReadMotionVector(data, "rotation", defaults.rotation);
    key.scale = ReadMotionVector(data, "scale", defaults.scale);
    key.color = ReadMotionColor(data, "color", defaults.color);
    key.emissive = data.value("emissive", defaults.emissive);
    key.visible = data.value("visible", defaults.visible);
}

// 始終点の姿勢と、区間の時間・移動曲線・揺れの係数を保存する。
void to_json(json& data, const CinematicMotionSegment& motion) {
    data = {
        {"name", motion.name},
        {"startTime", motion.startTime},
        {"endTime", motion.endTime},
        {"startSignal", motion.startSignal},
        {"endSignal", motion.endSignal},
        {"startSignalOffset", motion.startSignalOffset},
        {"endSignalOffset", motion.endSignalOffset},
        {"from", motion.from},
        {"to", motion.to},
        {"smoothProgress", motion.smoothProgress},
        {"arcHeight", motion.arcHeight},
        {"rotationArc", J3(motion.rotationArc)},
        {"rotationCycles", motion.rotationCycles},
        {"smoothScale", motion.smoothScale},
        {"scaleWaveAmplitude", J3(motion.scaleWaveAmplitude)},
        {"scaleWaveFrequency", motion.scaleWaveFrequency},
        {"scaleWaveFadeIn", motion.scaleWaveFadeIn},
        {"scaleDecay", motion.scaleDecay},
        {"scaleCycles", motion.scaleCycles},
        {"scaleStart", motion.scaleStart},
        {"minimumScale", motion.minimumScale},
        {"bounceHeight", motion.bounceHeight},
        {"bounceDuration", motion.bounceDuration},
        {"bounceCycles", motion.bounceCycles},
        {"emissiveAmplitude", motion.emissiveAmplitude},
        {"emissiveFrequency", motion.emissiveFrequency},
    };
}

void from_json(const json& data, CinematicMotionSegment& motion) {
    // 秒とSignalで定義する両端時刻に続けて、始終点の姿勢を読む。
    const CinematicMotionSegment defaults;
    motion.name = data.value("name", defaults.name);
    motion.startTime = data.value("startTime", defaults.startTime);
    motion.endTime = data.value("endTime", defaults.endTime);
    motion.startSignal = data.value("startSignal", defaults.startSignal);
    motion.endSignal = data.value("endSignal", defaults.endSignal);
    motion.startSignalOffset = data.value("startSignalOffset", defaults.startSignalOffset);
    motion.endSignalOffset = data.value("endSignalOffset", defaults.endSignalOffset);
    motion.from = data.value("from", defaults.from);
    motion.to = data.value("to", defaults.to);
    // クリップを評価するときに使う進行、姿勢、拡縮と発光の係数を復元する。
    motion.smoothProgress = data.value("smoothProgress", defaults.smoothProgress);
    motion.arcHeight = data.value("arcHeight", defaults.arcHeight);
    motion.rotationArc = ReadMotionVector(data, "rotationArc", defaults.rotationArc);
    motion.rotationCycles = data.value("rotationCycles", defaults.rotationCycles);
    motion.smoothScale = data.value("smoothScale", defaults.smoothScale);
    motion.scaleWaveAmplitude = ReadMotionVector(data, "scaleWaveAmplitude", defaults.scaleWaveAmplitude);
    motion.scaleWaveFrequency = data.value("scaleWaveFrequency", defaults.scaleWaveFrequency);
    motion.scaleWaveFadeIn = data.value("scaleWaveFadeIn", defaults.scaleWaveFadeIn);
    motion.scaleDecay = data.value("scaleDecay", defaults.scaleDecay);
    motion.scaleCycles = data.value("scaleCycles", defaults.scaleCycles);
    motion.scaleStart = data.value("scaleStart", defaults.scaleStart);
    motion.minimumScale = data.value("minimumScale", defaults.minimumScale);
    motion.bounceHeight = data.value("bounceHeight", defaults.bounceHeight);
    motion.bounceDuration = data.value("bounceDuration", defaults.bounceDuration);
    motion.bounceCycles = data.value("bounceCycles", defaults.bounceCycles);
    motion.emissiveAmplitude = data.value("emissiveAmplitude", defaults.emissiveAmplitude);
    motion.emissiveFrequency = data.value("emissiveFrequency", defaults.emissiveFrequency);
}

// 追従再生で積分する状態は含めず、ばねの初期値、係数、上限を保存する。
void to_json(json& data, const CinematicSpringSettings& settings) {
    data = {
        {"initialPositionOffset", J3(settings.initialPositionOffset)},
        {"initialRotationOffset", J3(settings.initialRotationOffset)},
        {"initialForwardVelocity", settings.initialForwardVelocity},
        {"positionStiffness", settings.positionStiffness},
        {"positionDamping", settings.positionDamping},
        {"angularStiffness", settings.angularStiffness},
        {"angularDamping", settings.angularDamping},
        {"maxPlanarOffset", settings.maxPlanarOffset},
        {"maxSink", settings.maxSink},
        {"maxLift", settings.maxLift},
        {"tiltFromOffset", settings.tiltFromOffset},
        {"tiltFromVelocity", settings.tiltFromVelocity},
        {"sourceTiltFollow", settings.sourceTiltFollow},
        {"initialVerticalVelocity", settings.initialVerticalVelocity},
        {"initialRollVelocity", settings.initialRollVelocity},
        {"compressionScale", settings.compressionScale},
    };
}

void from_json(const json& data, CinematicSpringSettings& settings) {
    // 古い素材で係数が省略されていても、既定ばねの値から再開できるようにする。
    const CinematicSpringSettings defaults;
    settings.initialPositionOffset = ReadMotionVector(data, "initialPositionOffset", defaults.initialPositionOffset);
    settings.initialRotationOffset = ReadMotionVector(data, "initialRotationOffset", defaults.initialRotationOffset);
    settings.initialForwardVelocity = data.value("initialForwardVelocity", defaults.initialForwardVelocity);
    settings.positionStiffness = data.value("positionStiffness", defaults.positionStiffness);
    settings.positionDamping = data.value("positionDamping", defaults.positionDamping);
    settings.angularStiffness = data.value("angularStiffness", defaults.angularStiffness);
    settings.angularDamping = data.value("angularDamping", defaults.angularDamping);
    settings.maxPlanarOffset = data.value("maxPlanarOffset", defaults.maxPlanarOffset);
    settings.maxSink = data.value("maxSink", defaults.maxSink);
    settings.maxLift = data.value("maxLift", defaults.maxLift);
    settings.tiltFromOffset = data.value("tiltFromOffset", defaults.tiltFromOffset);
    settings.tiltFromVelocity = data.value("tiltFromVelocity", defaults.tiltFromVelocity);
    settings.sourceTiltFollow = data.value("sourceTiltFollow", defaults.sourceTiltFollow);
    settings.initialVerticalVelocity = data.value("initialVerticalVelocity", defaults.initialVerticalVelocity);
    settings.initialRollVelocity = data.value("initialRollVelocity", defaults.initialRollVelocity);
    settings.compressionScale = data.value("compressionScale", defaults.compressionScale);
}

namespace {
// 非有限値や不正な配列長をGPUへ渡さず、読込時にエラーへまとめる。
void CheckRow(const json& row, size_t count) {
    // 数値を読む前に配列と要素数を確認し、欠けた成分を参照しない。
    if (!row.is_array() || row.size() != count)
        throw std::runtime_error("Invalid cinematic sample size");
    for (const auto& value : row)
        // 各成分は数値かつ有限である必要がある。NaNや無限大を姿勢の計算へ渡さない。
        if (!value.is_number() || !std::isfinite(value.get<float>()))
            throw std::runtime_error("Invalid cinematic sample value");
}
}

// シーケンス本体のJSONに、一時Object・カメラ・粒子の演出部品を追加する。
void SaveCinematicPresentation(json& root, const CinematicPresentation& p) {
    auto& data = root["presentation"];
    data = {{"reference", p.reference}, {"lookToward", p.lookToward}, {"hideObjects", p.hideObjects},
            {"objects", json::array()}, {"view", json::array()}, {"particles", json::array()}};
    // ばねを使用する部品が設定されている場合だけ、名前と係数を保存する。
    if (!p.springs.empty()) data["springs"] = p.springs;
    // カメラのワールド座標を選んだ場合だけ書く。省略時は従来の相対座標で読む。
    if (p.viewWorldSpace) data["viewWorldSpace"] = true;
    // Objectごとに生成と追従の設定を保存し、評価用の区間またはキー列を格納する。
    for (const auto& clip : p.objects) {
        json item = {{"name", clip.name}, {"model", clip.model}, {"source", clip.source},
            {"positionSpace", clip.positionSpace}, {"pivot", J3(clip.pivot)}, {"spawn", clip.spawn},
            {"additive", clip.additive}, {"lit", clip.lit}, {"enabled", clip.enabled},
            {"inheritScale", clip.inheritScale}, {"inheritRotation", clip.inheritRotation},
            {"clipScaleHeight", clip.clipScaleHeight}, {"floorHeight", clip.floorHeight},
            {"clipCenterOffset", clip.clipCenterOffset},
            {"clipEdgeWidth", clip.clipEdgeWidth}, {"clipDissolve", clip.clipDissolve},
            {"clipColor", J4(clip.clipColor)}, {"keys", json::array()}};
        // キーの代わりに区間が指定された場合は、その区間データを保存する。
        if (!clip.motions.empty()) item["motions"] = clip.motions;
        // 静止部品は既定のアニメーション有効から外れるため、無効指定を残す。
        if (!clip.animate) item["animate"] = false;
        // 基準対象の移動に追従させる場合だけ指定を保存する。
        if (clip.followPosition) item["followPosition"] = true;
        // 基準対象の回転に追従させる場合だけ指定を保存する。
        if (clip.followRotation) item["followRotation"] = true;
        // 基準対象の拡縮に追従させる場合だけ指定を保存する。
        if (clip.followScale) item["followScale"] = true;
        // 基準対象の表示状態を受け継ぐ場合だけ指定を保存する。
        if (clip.inheritVisibility) item["inheritVisibility"] = true;
        // 3軸をすべて引き継ぐ既定値から外れた場合だけ、各軸の回転係数を保存する。
        if (clip.rotationMask.x != 1 || clip.rotationMask.y != 1 || clip.rotationMask.z != 1)
            item["rotationMask"] = J3(clip.rotationMask);
        // 位置参照を使うキーだけ末尾3項目を追加します。先頭19項目の並びは旧ファイルと共通です。
        for (const auto& k : clip.keys) {
            item["keys"].push_back({k.time, k.position.x, k.position.y, k.position.z,
                k.rotation.x, k.rotation.y, k.rotation.z, k.scale.x, k.scale.y, k.scale.z,
                k.color.x, k.color.y, k.color.z, k.color.w, k.emissive, k.clipHeight,
                k.bounce, k.visible ? 1 : 0, k.clipEnabled ? 1 : 0});
            // 位置参照の基準を指定したキーだけ、参照先名と混合率を末尾に付ける。
            if (!k.positionReference.empty()) {
                item["keys"].back().push_back(k.positionReference);
                item["keys"].back().push_back(k.positionTarget);
                item["keys"].back().push_back(k.positionMix);
            }
        }
        data["objects"].push_back(std::move(item));
    }
    // カメラの注視点とアイ位置を評価するための16個の数値を、時間順の配列へ書く。
    for (const auto& k : p.view) {
        data["view"].push_back({k.time, k.focusMix, k.focusOffset.x, k.focusOffset.y, k.focusOffset.z,
            k.scaledFocusOffset.x, k.scaledFocusOffset.y, k.scaledFocusOffset.z,
            k.eyeOffset.x, k.eyeOffset.y, k.eyeOffset.z, k.jitter.x, k.jitter.y, k.jitter.z, k.fov, k.bars});
        // 注視またはアイ位置の参照名が一つでもあれば、末尾の参照名の領域を作る。
        if (!k.reference.empty() || !k.lookToward.empty() || !k.eyeReference.empty()) {
            data["view"].back().push_back(k.reference);
            data["view"].back().push_back(k.lookToward);
            // アイ位置の参照名まで指定された場合は、3番目の参照名として追加する。
            if (!k.eyeReference.empty()) data["view"].back().push_back(k.eyeReference);
        }
    }
    // 粒子は姿勢の配列と別に、対象・プリセット・発生区間・方向を名前付きで保存する。
    for (const auto& clip : p.particles)
        data["particles"].push_back({{"target", clip.target}, {"preset", clip.preset},
            {"directionMode", clip.directionMode}, {"startTime", clip.startTime}, {"endTime", clip.endTime},
            {"interval", clip.interval}, {"scale", clip.scale}, {"direction", J3(clip.direction)}});
}

void LoadCinematicPresentation(const json& root, CinematicPresentation& p) {
    // 演出拡張のない旧Sequenceは呼び出し元が初期化した設定で再生できます。
    if (!root.contains("presentation"))
        return;
    const auto& data = root.at("presentation");
    p.springs = data.value("springs", std::unordered_map<std::string, CinematicSpringSettings>{});
    // 数値の有限性と係数の範囲を読込時に検査し、後段のばね積分へ不正な設定を渡さないようにします。
    for (const auto& [name, settings] : p.springs) {
        // 追従部品と係数を名前で結び付けるので、空の名前は受け付けない。
        if (name.empty()) throw std::runtime_error("Empty cinematic spring name");
        const json values = settings;
        for (const auto& value : values.flatten())
            // ばねの係数に非有限値があると積分結果を復元できないため、素材の読込を中断する。
            if (!std::isfinite(value.get<float>())) throw std::runtime_error("Non-finite cinematic spring setting");
        // ばね・減衰・移動制限は非負、圧縮は元の拡縮への0～1の比率として検証する。
        if (settings.positionStiffness < 0 || settings.positionDamping < 0 ||
            settings.angularStiffness < 0 || settings.angularDamping < 0 ||
            settings.maxPlanarOffset < 0 || settings.maxSink < 0 || settings.maxLift < 0 ||
            settings.compressionScale < 0 || settings.compressionScale > 1)
            throw std::runtime_error("Invalid cinematic spring coefficient");
    }

    // シーケンス全体の注視対象・カメラ座標系・非表示対象を復元する。
    p.reference = data.value("reference", "");
    p.lookToward = data.value("lookToward", "");
    p.viewWorldSpace = data.value("viewWorldSpace", false);
    p.hideObjects = data.value("hideObjects", std::vector<std::string>{});
    // 各Objectの生成先と追従方式を復元してから、時間で切り替わる姿勢を読む。
    for (const auto& item : data.value("objects", json::array())) {
        CinematicObjectClip clip;
        clip.name = item.at("name").get<std::string>();
        clip.source = item.at("source").get<std::string>();
        clip.model = item.value("model", "");
        clip.positionSpace = item.value("positionSpace", "world");
        // 設定済みの座標変換で扱えるワールド、モデル、放射方向の3種に制限する。
        if (clip.positionSpace != "world" && clip.positionSpace != "model" && clip.positionSpace != "radial")
            throw std::runtime_error("Unknown cinematic position space");
        clip.pivot = V3(item.value("pivot", json::array({0, 0, 0})));
        clip.spawn = item.value("spawn", true);
        clip.additive = item.value("additive", false);
        clip.lit = item.value("lit", true);
        clip.enabled = item.value("enabled", true);
        clip.animate = item.value("animate", true);
        clip.followPosition = item.value("followPosition", false);
        clip.followRotation = item.value("followRotation", false);
        clip.followScale = item.value("followScale", false);
        clip.inheritVisibility = item.value("inheritVisibility", false);
        clip.rotationMask = V3(item.value("rotationMask", json::array({1, 1, 1})));
        clip.inheritScale = item.value("inheritScale", true);
        clip.inheritRotation = item.value("inheritRotation", true);
        clip.clipScaleHeight = item.value("clipScaleHeight", false);
        clip.floorHeight = item.value("floorHeight", -100000.0f);
        clip.clipCenterOffset = item.value("clipCenterOffset", 0.0f);
        clip.clipEdgeWidth = item.value("clipEdgeWidth", 0.075f);
        clip.clipDissolve = item.value("clipDissolve", 0.20f);
        clip.clipColor = V4(item.value("clipColor", json::array({1, 1, 1, 1})));
        for (const auto& row : item.value("keys",json::array())) {
            // 見た目キーは旧形式19項目か、位置参照付き22項目だけを受け付ける。
            if (!row.is_array() || (row.size() != 19 && row.size() != 22))
                throw std::runtime_error("Invalid visual key size");
            // 旧形式と拡張形式に共通の先頭19項目は数値です。拡張形式の末尾で参照名と位置の混合率を読みます。
            CheckRow(json(row.begin(), row.begin() + 19), 19);
            CinematicVisualKey k;
            k.time = row[0]; k.position = {row[1], row[2], row[3]};
            k.rotation = {row[4], row[5], row[6]}; k.scale = {row[7], row[8], row[9]};
            k.color = {row[10], row[11], row[12], row[13]};
            k.emissive = row[14]; k.clipHeight = row[15]; k.bounce = row[16];
            k.visible = row[17].get<float>() != 0; k.clipEnabled = row[18].get<float>() != 0;
            // 拡張キーでは、数値の姿勢に基準点・移動先・混合率を組み合わせる。
            if (row.size() == 22) {
                k.positionReference = row[19].get<std::string>();
                k.positionTarget = row[20].get<std::string>();
                k.positionMix = row[21].get<float>();
                // 基準点が空、または混合率が非有限か0～1の範囲外なら位置を決められないため拒否する。
                if (k.positionReference.empty() || !std::isfinite(k.positionMix) || k.positionMix < 0 || k.positionMix > 1)
                    throw std::runtime_error("Invalid visual position binding");
            }
            clip.keys.push_back(k);
        }
        clip.motions=item.value("motions",std::vector<CinematicMotionSegment>{});
        // 動作区間と姿勢キーが同じ対象を取り合わないよう、どちらか一方の設定だけを受け付けます。
        if (!clip.motions.empty() && !clip.keys.empty()) throw std::runtime_error("Conflicting cinematic key and motion data");
        float previousEnd=-1;
        // 区間方式ではファイル中の順番を使うので、直前の区間と重ならないことを順に確認する。
        for (const auto& motion : clip.motions) {
            const json values=motion;
            for (const auto& value : values.flatten())
                // ベクトルや色を含む入れ子の数値も、姿勢計算で扱える有限値に制限する。
                if (value.is_number() && !std::isfinite(value.get<float>())) throw std::runtime_error("Non-finite motion value");
            // 時刻が逆順や重複、拡縮と弾みの係数が範囲外、始終点で参照指定が片方だけの場合は区間を拒否する。
            if (motion.startTime<0 || motion.endTime<=motion.startTime || motion.startTime<previousEnd ||
                motion.scaleStart<0 || motion.scaleStart>=1 || motion.minimumScale<0 || motion.bounceDuration<=0 ||
                motion.scaleWaveFrequency<0 || motion.bounceHeight<0 || motion.bounceCycles<0 || motion.rotationCycles<0 || motion.scaleDecay<0 || motion.scaleCycles<0 || motion.from.positionReference.empty()!=motion.to.positionReference.empty())
                throw std::runtime_error("Invalid cinematic motion interval");
            previousEnd=motion.endTime;
        }
        // すべてのキーまたは区間を検証できたObjectを、再生側へ渡す配列に登録する。
        p.objects.push_back(std::move(clip));
    }
    // カメラには、注視点とアイ位置を再現する旧形式・拡張形式のキーを読む。
    for (const auto& row : data.value("view", json::array())) {
        // 数値のみ16項目、注視参照付き18項目、アイ位置参照付き19項目だけを受け付ける。
        if (!row.is_array() || (row.size() != 16 && row.size() != 18 && row.size() != 19))
            throw std::runtime_error("Invalid cinematic view sample size");
        // カメラキーの先頭16項目は共通です。追加された参照名は項目数で見分け、既存ファイルも読めるようにします。
        CheckRow(json(row.begin(), row.begin() + 16), 16);
        CinematicViewKey k;
        k.time = row[0]; k.focusMix = row[1]; k.focusOffset = {row[2], row[3], row[4]};
        k.scaledFocusOffset = {row[5], row[6], row[7]}; k.eyeOffset = {row[8], row[9], row[10]};
        k.jitter = {row[11], row[12], row[13]}; k.fov = row[14]; k.bars = row[15];
        // 注視参照を持つ長さのキーなら、2種類の注視先名も読み込む。
        if (row.size() >= 18) {
            k.reference = row[16].get<std::string>();
            k.lookToward = row[17].get<std::string>();
            // 最長形式のキーにはアイ位置の参照先名があるので、末尾から読む。
            if (row.size() == 19) k.eyeReference = row[18].get<std::string>();
        }
        p.view.push_back(k);
    }
    // 粒子は各発生区間の対象、方向、発火間隔を復元し、再生用の配列に入れる。
    for (const auto& item : data.value("particles", json::array())) {
        CinematicParticleClip clip;
        clip.target = item.at("target").get<std::string>(); clip.preset = item.at("preset").get<std::string>();
        clip.directionMode = item.value("directionMode", "fixed");
        clip.startTime = item.value("startTime", 0.0f); clip.endTime = item.value("endTime", 1.0f);
        clip.interval = item.value("interval", 1.0f / 9.0f); clip.scale = item.value("scale", 1.0f);
        clip.direction = V3(item.value("direction", json::array({0, 1, 0})));
        // 発生間隔が非有限か0.01秒未満、または終了秒が開始より前の場合は、再生可能な発火区間として扱えないため拒否する。
        if (!std::isfinite(clip.interval) || clip.interval < 0.01f || clip.endTime < clip.startTime)
            throw std::runtime_error("Invalid cinematic particle interval");
        p.particles.push_back(clip);
    }
}

} // namespace engine
