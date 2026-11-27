#include "game/actor/enemy/boss/core/MidbossEncounterEffects.h"

#include "game/actor/enemy/core/EnemySettingsPaths.h"
#include "json.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace {

// 欠損・不正値では従来のVFX名を使い、他の設定項目の読み込みは続ける。
void ReadSequence(const nlohmann::json& settings, const char* key, std::string& sequence) {
    const auto it = settings.find(key);
    // 項目がないか文字列でない場合は、この演出の既定IDを保持する。
    if (it == settings.end() || !it->is_string()) {
        return;
    }

    // 拡張子のない相対IDだけを許可し、パスの起点や親ディレクトリへの移動を含む指定は無視します。
    const auto& name = it->get_ref<const std::string&>();
    const std::filesystem::path path(std::u8string(name.begin(), name.end()));
    const bool safe = !path.has_root_path() && !path.has_extension() &&
        std::none_of(path.begin(), path.end(), [](const auto& part) { return part.empty() || part == ".." || part == "."; });
    // 空文字でなく、ルート・拡張子・親への移動を含まない指定だけを採用する。
    if (!name.empty() && safe) {
        sequence = name;
    }
}

MidbossEncounterEffects LoadEncounterEffects(const char* enemyType, const char* prefix) {
    // 設定ファイルがなくても従来と同じ演出を再生できるよう、三つの既定IDを先に用意します。
    const std::string name = std::string("enemy/") + prefix + "_slime/" + prefix;
    MidbossEncounterEffects effects{
        name + "_arena_seal_cue", name + "_midboss_appear_cue", name + "_arena_release_cue"
    };

    std::ifstream file(enemySettingsPaths::Directory(enemyType) + "encounter.json");
    // 設定ファイルを開けない場合は、三つの既定演出をそのまま返す。
    if (!file) {
        return effects;
    }

    const auto settings = nlohmann::json::parse(file, nullptr, false);
    // JSONオブジェクトとして読めたときだけ、封鎖・登場・解除のIDを個別に上書きする。
    if (settings.is_object()) {
        ReadSequence(settings, "seal", effects.seal);
        ReadSequence(settings, "appearance", effects.appearance);
        ReadSequence(settings, "release", effects.release);
    }
    return effects;
}

} // namespace

const MidbossEncounterEffects& MidbossEncounterEffects::Prism() {
    // プリズムの演出設定は初回参照時に読み込み、以後は同じ設定を返します。
    static const auto kEffects = LoadEncounterEffects("PrismSlime", "prism");
    return kEffects;
}

const MidbossEncounterEffects& MidbossEncounterEffects::Magma() {
    // マグマの演出設定は初回参照時に読み込み、以後は同じ設定を返します。
    static const auto kEffects = LoadEncounterEffects("MagmaSlime", "magma");
    return kEffects;
}
