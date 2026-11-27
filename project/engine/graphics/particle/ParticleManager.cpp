#include <cstdint>
#include "engine/graphics/effect/VfxAssetPath.h"
#include "ParticleManager.h"
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <algorithm>
#include "externals/nlohmann/json.hpp"

using json = nlohmann::json;

ParticleManager* ParticleManager::GetInstance() {
    static ParticleManager sInstance;
    return &sInstance;
}

void ParticleManager::Initialize(engine::ParticleSystem* particleSystem) {
    particleSystem_ = particleSystem;

    // この粒子システムに結び付けた後、登録済みの発生プリセットを読み込む。
    LoadAllParams();
}

// ---------------------------------------------------------
// JSON変換ヘルパー関数
// ---------------------------------------------------------
namespace {

// 粒子のベクトルと色は成分名を持つJSONオブジェクトとして保存する。
json WriteParticleVector(const engine::Vector3& v) {
    return {{"x", v.x}, {"y", v.y}, {"z", v.z}};
}

// 位置や速度を表す三つの成分を読みます。キーや型の不備は例外として呼び出し側へ伝わります。
engine::Vector3 ReadParticleVector(const json& data) {
    return {data.at("x").get<float>(), data.at("y").get<float>(), data.at("z").get<float>()};
}

json WriteParticleColor(const engine::Vector4& color) {
    return {{"x", color.x}, {"y", color.y}, {"z", color.z}, {"w", color.w}};
}

// RGBAに対応する四成分を、保存時のx・y・z・wキーから読みます。
engine::Vector4 ReadParticleColor(const json& data) {
    return {data.at("x").get<float>(), data.at("y").get<float>(),
            data.at("z").get<float>(), data.at("w").get<float>()};
}

// 時刻と値のキー列をParticle保存JSONへ変換します。
json WriteFloatCurve(const VfxFloatCurve& curve) {
    json result = json::array();

    for (const VfxFloatCurveKey& key : curve.keys) {
        result.push_back({
            { "time", key.time },
            { "value", key.value },
            { "easing", static_cast<std::int32_t>(key.easing) },
        });
    }

    return result;
}

// 時刻と色のキー列をParticle保存JSONへ変換します。
json WriteColorGradient(const VfxColorGradient& gradient) {
    json result = json::array();

    for (const VfxColorGradientKey& key : gradient.keys) {
        result.push_back({
            { "time", key.time },
            { "color", WriteParticleColor(key.color) },
            { "easing", static_cast<std::int32_t>(key.easing) },
        });
    }

    return result;
}

// 保存済みの数値カーブキーをEmitter設定へ復元します。
void ReadFloatCurve(const json& source, VfxFloatCurve& curve) {
    // 保存値を曲線キーへ戻し、時刻順や境界値はNormalizeで整える。
    if (!source.is_array())
        return;
    curve.keys.clear();

    for (const json& item : source) {
        // 配列の中でも、キーの項目を持てるオブジェクトだけを読む。
        if (!item.is_object())
            continue;
        VfxFloatCurveKey key;
        key.time = item.value("time", 0.0f);
        key.value = item.value("value", 1.0f);
        key.easing = static_cast<VfxCurveEasing>(
            std::clamp(item.value("easing", 0), 0, 4));
        curve.keys.push_back(key);
    }

    curve.Normalize();
}

// 保存済みの色グラデーションキーをEmitter設定へ復元します。
void ReadColorGradient(const json& source, VfxColorGradient& gradient) {
    // 色キーと補間方法を読み戻し、不正な補間番号は有効範囲へ丸める。
    if (!source.is_array())
        return;
    gradient.keys.clear();

    for (const json& item : source) {
        // 色キーの設定を持てるJSONオブジェクトだけを受け付ける。
        if (!item.is_object())
            continue;
        VfxColorGradientKey key;
        key.time = item.value("time", 0.0f);

        // 色が記録されている場合だけ、キーの既定色を置き換える。
        if (item.contains("color")) {
            key.color = ReadParticleColor(item.at("color"));
        }

        key.easing = static_cast<VfxCurveEasing>(
            std::clamp(item.value("easing", 0), 0, 4));
        gradient.keys.push_back(key);
    }

    gradient.Normalize();
}

// VFXの距離別詳細度設定を保存JSONへ変換します。
json WriteLod(const VfxLodSettings& lod) {
    return {
        { "enabled", lod.enabled },
        { "nearDistance", lod.nearDistance },
        { "farDistance", lod.farDistance },
        { "farEmissionScale", lod.farEmissionScale },
        { "maxAliveParticles", lod.maxAliveParticles },
    };
}

// 保存JSONで指定された距離別詳細度設定を復元します。
void ReadLod(const json& source, VfxLodSettings& lod) {
    // 距離設定がオブジェクト形式の場合だけ、各項目を取り込む。
    if (!source.is_object())
        return;
    lod.enabled = source.value("enabled", false);
    lod.nearDistance = source.value("nearDistance", 12.0f);
    lod.farDistance = source.value("farDistance", 45.0f);
    lod.farEmissionScale = source.value("farEmissionScale", 0.25f);
    lod.maxAliveParticles = source.value("maxAliveParticles", 0);
    lod.Sanitize();
}
}

// EmitterParams のシリアライズ
namespace engine {

void to_json(json& j, const engine::ParticleSystem::EmitterParams& p) {
    j = json{
        {"spawnArea", WriteParticleVector(p.spawnArea)},
        {"initialVelocity", WriteParticleVector(p.initialVelocity)},
        {"velocityRandomness", WriteParticleVector(p.velocityRandomness)},
        {"emitCount", p.emitCount},
        {"particlesPerSecond", p.particlesPerSecond},
        {"particleLifetime", p.particleLifetime},
        {"startColor", WriteParticleColor(p.startColor)},
        {"endColor", WriteParticleColor(p.endColor)},
        {"startSize", p.startSize},
        {"endSize", p.endSize},
        {"initialRotationSpeed", p.initialRotationSpeed},
    {"rotationSpeedRandomness", p.rotationSpeedRandomness},
    {"hdrIntensity", p.hdrIntensity},
    {"emitterType", (std::int32_t)p.emitterType}, 
    {"spawnRadius", p.spawnRadius},      
    {"coneAngle", p.coneAngle},
    {"acceleration", WriteParticleVector(p.acceleration)},
    {"textureName", p.textureName},
        {"blendMode", (std::int32_t)p.blendMode}

    };

    json legacyCurve = json::array();

    for (float value : p.sizeCurve) {
        legacyCurve.push_back(value);
    }

    // 新しい編集カーブを無効にした場合も従来の十点カーブへ戻せるよう、両方の形式を保存します。
    j["legacySizeCurve"] = std::move(legacyCurve);
    j["authoring"] = {
        { "enabled", p.useAuthoringCurves },
        { "sizeOverLife", WriteFloatCurve(p.sizeOverLife) },
        { "colorOverLife", WriteColorGradient(p.colorOverLife) },
    };

    j["lod"] = WriteLod(p.lod);
}

void from_json(const json& j, engine::ParticleSystem::EmitterParams& p) {
    // 旧JSONで欠けている項目は既定値を維持し、存在する設定だけ取り込む。
    if (j.contains("textureName")) {
        p.textureName = j.at("textureName").get<std::string>();
    }

    // 発生領域が保存されている場合だけ、各軸のばらつき幅を戻す。
    if (j.contains("spawnArea"))
        p.spawnArea = ReadParticleVector(j.at("spawnArea"));
    // 初速が保存されている場合だけ、発生時の方向と速さを戻す。
    if (j.contains("initialVelocity"))
        p.initialVelocity = ReadParticleVector(j.at("initialVelocity"));
    // 初速の乱数幅が保存されている場合だけ、各軸のばらつきを戻す。
    if (j.contains("velocityRandomness"))
        p.velocityRandomness = ReadParticleVector(j.at("velocityRandomness"));
    // 単発の発生数は、省略時の1粒も含めて現在の最大粒子数以下にする。
    p.emitCount = std::clamp(j.value("emitCount", 1), 1, engine::ParticleSystem::GetMaxParticles());
    // 毎秒の発生数が保存されている場合だけ、連続発生の頻度に反映する。
    if (j.contains("particlesPerSecond"))
        j.at("particlesPerSecond").get_to(p.particlesPerSecond);
    // 寿命の設定がある場合だけ、粒子が消えるまでの秒数を戻す。
    if (j.contains("particleLifetime"))
        j.at("particleLifetime").get_to(p.particleLifetime);
    // 開始色がある場合だけ、発生直後のRGBAを戻す。
    if (j.contains("startColor"))
        p.startColor = ReadParticleColor(j.at("startColor"));
    // 終了色がある場合だけ、寿命を迎える時点のRGBAを戻す。
    if (j.contains("endColor"))
        p.endColor = ReadParticleColor(j.at("endColor"));
    // 開始サイズがある場合だけ、発生直後の寸法を戻す。
    if (j.contains("startSize"))
        j.at("startSize").get_to(p.startSize);
    // 終了サイズがある場合だけ、寿命を迎える時点の寸法を戻す。
    if (j.contains("endSize"))
        j.at("endSize").get_to(p.endSize);
    // 回転速度が保存されている場合だけ、粒子の基準角速度を戻す。
    if(j.contains("initialRotationSpeed"))
        j.at("initialRotationSpeed").get_to(p.initialRotationSpeed);
    // HDRの強さがある場合だけ、描画側で使う発光の倍率を戻す。
    if (j.contains("hdrIntensity"))
        j.at("hdrIntensity").get_to(p.hdrIntensity);
    // 角速度のばらつきがある場合だけ、回転の乱数幅を戻す。
    if (j.contains("rotationSpeedRandomness"))
        j.at("rotationSpeedRandomness").get_to(p.rotationSpeedRandomness);
    p.emitterType = (EmitterType)j.value("emitterType", (std::int32_t)EmitterType::kBox); 
    p.spawnRadius = j.value("spawnRadius", 1.0f); 
    p.coneAngle = j.value("coneAngle", 30.0f);
    // 加速度が指定されている場合にだけ、発生後の速度変化を適用する。
    if (j.contains("acceleration"))
        p.acceleration = ReadParticleVector(j.at("acceleration"));
    p.blendMode = (ParticleBlendMode)j.value("blendMode", (std::int32_t)ParticleBlendMode::kAlpha);

    if (j.contains("legacySizeCurve") && j["legacySizeCurve"].is_array()) {
        // 古いサイズカーブは固定長なので配列境界で切って読み込む。
        const json& values = j["legacySizeCurve"];
        const std::size_t count = (std::min)(values.size(), static_cast<std::size_t>(10));

        for (std::size_t index = 0; index < count; ++index) {
            p.sizeCurve[index] = values[index].get<float>();
        }
    }

    if (j.contains("authoring") && j["authoring"].is_object()) {
        // 編集用カーブのキーが不足する設定は有効扱いにしない。
        const json& authoring = j["authoring"];
        p.useAuthoringCurves = authoring.value("enabled", false);

        // 寿命に沿ったサイズの入力が保存されている場合は、数値カーブを復元する。
        if (authoring.contains("sizeOverLife")) {
            ReadFloatCurve(authoring["sizeOverLife"], p.sizeOverLife);
        }

        // 寿命に沿った色の入力が保存されている場合は、色のグラデーションを復元する。
        if (authoring.contains("colorOverLife")) {
            ReadColorGradient(authoring["colorOverLife"], p.colorOverLife);
        }

        // サイズまたは色のキーが揃わない場合は、編集カーブを有効にしない。
        if (p.sizeOverLife.keys.empty() || p.colorOverLife.keys.empty()) {
            p.useAuthoringCurves = false;
        }
    }

    // LODの項目がある場合は、オブジェクト形式の検査と補正をReadLodへ任せる。
    if (j.contains("lod")) {
        ReadLod(j["lod"], p.lod);
    }
}

} // namespace engine

// ---------------------------------------------------------
// メイン処理
// ---------------------------------------------------------

void ParticleManager::SaveParam(const std::string& name, const engine::ParticleSystem::EmitterParams& param) {
    paramsMap_[name] = param; // メモリ上の辞書更新

    // プリセットの保存先が無ければ、書き出しの前にフォルダーを用意する。
    if (!std::filesystem::exists(kDirectoryPath)) {
        std::filesystem::create_directories(kDirectoryPath);
    }

    // 名前からUTF-8の保存パスを作り、JSONとしてファイルへ書く。
    const auto utf8Path = kDirectoryPath + name + ".json";
    const std::filesystem::path filePath(std::u8string(utf8Path.begin(), utf8Path.end()));
    WriteParamFile(filePath, param);
}

bool ParticleManager::WriteParamFile(const std::filesystem::path& path, const engine::ParticleSystem::EmitterParams& param) {
    std::ofstream file(path);
    file << std::setw(4) << json(param) << std::endl;
    file.close();
    return file.good();
}

void ParticleManager::LoadAllParams() {
    // プリセットのルートが無い場合は、再帰走査を始めない。
    if (!std::filesystem::exists(kDirectoryPath))
        return;

    const std::filesystem::path root = std::filesystem::path(kDirectoryPath).parent_path();
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        // 通常ファイルで拡張子が.jsonのものだけ、プリセットの候補にする。
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
        auto relative = entry.path().lexically_relative(root);
        // プリセットの識別名にルート以下のフォルダーも含め、別フォルダーにある同名ファイルを区別します。
        const auto key = relative.replace_extension().generic_u8string();
        engine::ParticleSystem::EmitterParams param;
        // 設定を読めたファイルだけを、フォルダー階層を含むキーで登録する。
        if (ReadParamFile(entry.path(), param)) paramsMap_[std::string(key.begin(), key.end())] = std::move(param);
    }
}

// 名前に対応するEmitter設定を読み、成功したものだけ登録済みパラメータへ反映します。
void ParticleManager::LoadParam(const std::string& name) {
    const auto utf8Path = kDirectoryPath + name + ".json";
    const std::filesystem::path filePath(std::u8string(utf8Path.begin(), utf8Path.end()));
    engine::ParticleSystem::EmitterParams param;
    // 読込に成功した場合だけ、指定名の設定を差し替える。
    if (ReadParamFile(filePath, param)) paramsMap_[name] = std::move(param);
}

bool ParticleManager::ReadParamFile(const std::filesystem::path& path, engine::ParticleSystem::EmitterParams& out) {
    std::ifstream file(path);
    // 指定したファイルを開けなければ、出力値を変更せず失敗を返す。
    if (!file) return false;
    json j;
    file >> j;
    // 読み込み中は一時構造体を使い、変換が完了した設定だけ呼び出し元へ移します。JSON解析・変換の例外は呼び出し元へ伝わります。
    engine::ParticleSystem::EmitterParams loaded;
    from_json(j, loaded);
    out = std::move(loaded);
    return true;
}

void ParticleManager::Emit(const std::string& name, const engine::Vector3& position, float& timer) {
    // 設定した名前を標準の名前解決規則で探す。
    const auto preset = vfxAssetPath::FindPreset(paramsMap_, name);
    // 対応するプリセットが無い場合は、発生条件を取得できないため戻る。
    if (preset == paramsMap_.end())
        return;

    // パラメータ取得
    const auto& params = preset->second;

    // 発生頻度が正でない場合は、間隔を求めず粒子を出さない。
    if (params.particlesPerSecond <= 0.0f) {
        return;
    }

    // 毎秒の発生数の逆数を、一粒を出す間隔の秒数として使う。
    float rate = 1.0f / params.particlesPerSecond;

    // この経路では呼び出し一回を1/60秒として、発生待ちの秒数を進める。
    timer += 1.0f / 60.0f;

    // particlesPerSecondは実粒子数/秒なので、この経路では1粒ずつ生成します。
    // emitCountはEditorの単発発生や明示的なEmitOneShot呼び出しだけに適用します。
    engine::ParticleSystem::EmitterParams singleParticleParams = params;
    singleParticleParams.emitCount = 1;

    // 一回の更新で複数の発生間隔を跨いだ場合はその回数分だけ生成し、端数秒を次の更新へ残します。
    while (timer >= rate) {
        timer -= rate;
        particleSystem_->EmitOneShot(singleParticleParams, position);
    }
}
