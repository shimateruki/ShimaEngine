#include "engine/editor/scene/inspector/InspectorTextureCatalog.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace {
std::string ToLowerAscii(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

std::string NormalizeAssetPath(const fs::path &path) {
  return path.generic_string();
}

bool IsSupportedTextureFile(const fs::path &path) {
  const std::string ext = ToLowerAscii(path.extension().string());
  return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".dds";
}

bool IsDdsTexture(const std::string &path) {
  return ToLowerAscii(fs::path(path).extension().string()) == ".dds";
}

// 変換済みDDSを優先し、同じ用途の候補を安定して一つに絞る。
void AssignPreferredTexture(std::string &slot, const std::string &candidate) {
  // 空き枠に登録する。登録済みの場合は、DDS以外からDDSへ差し替える場合だけ更新する。
  if (slot.empty() || (!IsDdsTexture(slot) && IsDdsTexture(candidate))) {
    slot = candidate;
  }
}

// 解像度やマップ種別の接尾辞を除き、3枚セット共通のキーを作る。
std::string BuildPbrPresetKey(const fs::path &path) {
  // 解像度と用途を示す語を除き、同じ素材の3種類のマップを同じキーへ寄せる。
  std::string stem = ToLowerAscii(path.stem().string());

  for (char &c : stem) {
    // ハイフンと空白をアンダースコアに揃え、同じ規則で名前を分割できるようにする。
    if (c == '-' || c == ' ') {
      c = '_';
    }
  }

  static const std::set<std::string> kIgnoredTokens = {
      "1k",      "2k",     "4k",        "8k",       "dx",       "gl",
      "directx", "opengl", "diff",      "diffuse",  "albedo",   "basecolor",
      "base",    "color",  "colour",    "nor",      "normal",   "arm",
      "orm",     "ao",     "roughness", "metallic", "metalness"};

  std::string key;
  size_t start = 0;

  while (start <= stem.size()) {
    const size_t end = stem.find('_', start);
    const std::string token = stem.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    // 空の区切りや用途・解像度を表す語を除き、材質名の一部だけを残す。
    if (!token.empty() && kIgnoredTokens.count(token) == 0) {
      // 二つ目以降の語はアンダースコアでつなぎ、先頭に区切りを付けない。
      if (!key.empty()) {
        key += "_";
      }

      key += token;
    }

    // 次の区切りが存在しない場合は、末尾の語まで読んだため走査を終える。
    if (end == std::string::npos) {
      break;
    }

    start = end + 1;
  }

  // 名前の全要素が除外された場合は、元の拡張子なしファイル名を使う。
  return key.empty() ? stem : key;
}

std::string BuildPbrPresetName(const std::string &key) {
    // テクスチャ組のキーを人が読みやすい表示名へ整える。
  std::string name = key;

  for (char &c : name) {
    // 表示名ではアンダースコアを空白に変える。
    if (c == '_') {
      c = ' ';
    }
  }

  // 空でない表示名だけ、先頭の文字を大文字にする。
  if (!name.empty()) {
    name[0] =
        static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
  }

  return name;
}
} // namespace

bool InspectorTextureCatalog::PbrPreset::IsComplete() const {
  return !albedoPath.empty() && !normalPath.empty() && !ormPath.empty();
}

void InspectorTextureCatalog::EnsureLoaded() {
  // まだ一覧を読み込んでいないときに、ファイルから作り直す。
  if (!isLoaded_) {
    Rebuild();
  }
}

void InspectorTextureCatalog::Refresh() { Rebuild(); }

void InspectorTextureCatalog::Rebuild() {
  // 古い索引を残さず、ディスク上のPBR素材とSpriteから候補を作り直す。
  albedoPaths_.clear();
  normalPaths_.clear();
  ormPaths_.clear();
  spritePaths_.clear();
  pbrPresets_.clear();

  const fs::path pbrDir = "Resources/texture/PBR/";

  // PBR用フォルダーがある場合だけ、PBR材質の候補を集める。
  if (fs::exists(pbrDir)) {
    std::vector<std::string> allFiles;
    std::set<std::string> ddsBaseNames;
    std::map<std::string, PbrPreset> presetMap;

    for (const auto &entry : fs::recursive_directory_iterator(pbrDir)) {
      // 同名のPNG/DDSがある場合はDDSを優先するため、先に組を記録する。
      // 通常のファイルだけを見て、サブフォルダーはこのファイル処理から外す。
      if (!entry.is_regular_file()) {
        continue;
      }

      const fs::path path = entry.path();

      // 対応する画像拡張子だけを一覧の候補にする。
      if (!IsSupportedTextureFile(path)) {
        continue;
      }

      const std::string normalized = NormalizeAssetPath(path);
      allFiles.push_back(normalized);

      // DDSがある場合は、同じフォルダーとファイル名を持つ別形式を除くために拡張子なしのパスを記録する。
      if (ToLowerAscii(path.extension().string()) == ".dds") {
        ddsBaseNames.insert(
            NormalizeAssetPath(path.parent_path() / path.stem()));
      }
    }

    for (const std::string &pathString : allFiles) {
      // Albedo・Normal・ARMを同じ素材キーへ集約し、Inspectorで一組として選べるようにする。
      const fs::path path(pathString);
      const std::string ext = ToLowerAscii(path.extension().string());
      const std::string base =
          NormalizeAssetPath(path.parent_path() / path.stem());
      // 同じフォルダーとファイル名のDDSが見つかっている場合は、DDS以外の候補を除く。
      if (ext != ".dds" && ddsBaseNames.count(base) != 0) {
        continue;
      }

      // アルベド用のパスは、アルベド単体を選ぶ一覧へも登録する。
      if (pathString.find("/Albedo/") != std::string::npos) {
        albedoPaths_.push_back(pathString);
      // 法線用のパスは、法線単体を選ぶ一覧へも登録する。
      } else if (pathString.find("/Normal/") != std::string::npos) {
        normalPaths_.push_back(pathString);
      // ARM用のパスは、ARM単体を選ぶ一覧へも登録する。
      } else if (pathString.find("/ARM/") != std::string::npos) {
        ormPaths_.push_back(pathString);
      }

      const std::string key = BuildPbrPresetKey(path);
      PbrPreset &preset = presetMap[key];
      preset.name = BuildPbrPresetName(key);

      // アルベド用のパスは、材質のアルベド枠に優先形式を考慮して登録する。
      if (pathString.find("/Albedo/") != std::string::npos) {
        AssignPreferredTexture(preset.albedoPath, pathString);
      // 法線用のパスは、材質の法線枠に優先形式を考慮して登録する。
      } else if (pathString.find("/Normal/") != std::string::npos) {
        AssignPreferredTexture(preset.normalPath, pathString);
      // ARM用のパスは、材質のARM枠に優先形式を考慮して登録する。
      } else if (pathString.find("/ARM/") != std::string::npos) {
        AssignPreferredTexture(preset.ormPath, pathString);
      // 既定素材だけは用途別フォルダーで分けられないため、名前を基に3枚のうちどの画像かを判断します。
      // 既定フォルダーの画像は、用途別のフォルダー名が使えない場合にファイル名から分類する。
      } else if (pathString.find("/Default/") != std::string::npos) {
        const std::string stem = ToLowerAscii(path.stem().string());
        // 名前がalbedoまたはdiffを含む場合は、アルベド枠の候補にする。
        if (stem.find("albedo") != std::string::npos ||
            stem.find("diff") != std::string::npos) {
          AssignPreferredTexture(preset.albedoPath, pathString);
        // 名前がnormalまたはnorを含む場合は、法線枠の候補にする。
        } else if (stem.find("normal") != std::string::npos ||
                   stem.find("nor") != std::string::npos) {
          AssignPreferredTexture(preset.normalPath, pathString);
        // 名前がarmまたはormを含む場合は、ARM枠の候補にする。
        } else if (stem.find("arm") != std::string::npos ||
                   stem.find("orm") != std::string::npos) {
          AssignPreferredTexture(preset.ormPath, pathString);
        }
      }
    }

    for (const auto &[key, preset] : presetMap) {
      // 不完全なPBRセットは誤適用を避けるため候補に含めない。
      if (preset.IsComplete()) {
        pbrPresets_.push_back(preset);
      }
    }
  }

  const fs::path spriteDir = "Resources/sprite/";

  // スプライト用フォルダーが存在する場合だけ、画像候補の走査を行う。
  if (fs::exists(spriteDir)) {
    for (const auto &entry : fs::recursive_directory_iterator(spriteDir)) {
      // 対応拡張子を持つ通常ファイルだけを、スプライト一覧に加える。
      if (entry.is_regular_file() && IsSupportedTextureFile(entry.path())) {
        spritePaths_.push_back(NormalizeAssetPath(entry.path()));
      }
    }
  }

  // 再走査してもコンボの候補順がディレクトリの列挙順によって変わらないようにします。
  const auto sortPaths = [](std::vector<std::string> &paths) {
    std::sort(paths.begin(), paths.end());
  };

  sortPaths(albedoPaths_);
  sortPaths(normalPaths_);
  sortPaths(ormPaths_);
  sortPaths(spritePaths_);
  std::sort(pbrPresets_.begin(), pbrPresets_.end(),
            [](const PbrPreset &lhs, const PbrPreset &rhs) {
              return lhs.name < rhs.name;
            });

  // 候補が空でも走査済みとし、次の参照時に同じ走査を繰り返さない。
  isLoaded_ = true;
}
