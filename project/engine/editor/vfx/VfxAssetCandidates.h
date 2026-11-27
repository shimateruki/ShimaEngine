#pragma once

#ifdef USE_IMGUI

#include "engine/editor/assets/AssetDatabase.h"
#include <algorithm>
#include <iterator>
#include <optional>
#include <string_view>
#include <utility>

namespace vfxAssetCandidates {

// 担当：VFXの素材候補を完成済みのアセット索引から作り、描画フレーム間で共有する。
// 関連：AssetDatabase（変更検知と索引）、各VFXエディタ（選択UI）。
class CandidateList {
public:
    // 索引の更新中は直前の候補を返す。初回構築中は読み込み済みの候補だけを利用できる。
    template <typename SelectPath>
    const std::vector<std::string>& Get(SelectPath selectPath, std::vector<std::string> loaded = {}) {
        const auto& database = *AssetDatabase::GetInstance();
        bool indexChanged = false;

        // GetAssets() は索引構築中に部分的な内容を返すため、世代が確定してから参照する。
        if (database.IsInitialized() && !database.IsInitialIndexBuildInProgress() &&
            generation_ != database.GetGeneration()) {
            indexed_.clear();
            for (const auto& asset : database.GetAssets()) {
                const auto name = selectPath(asset.sourcePath);
                // 用途ごとの変換で有効な素材名を取り出せた場合だけ、索引からの候補へ加える。
                if (!name.empty()) indexed_.emplace_back(name);
            }
            SortUnique(indexed_);
            generation_ = database.GetGeneration();
            indexChanged = true;
        }

        // プレビュー生成のモデルなど、索引に存在しない読み込み済み候補も選択に残す。
        if (indexChanged || loaded_ != loaded) {
            loaded_ = std::move(loaded);
            auto sortedLoaded = loaded_;
            SortUnique(sortedLoaded);
            candidates_.clear();
            std::set_union(indexed_.begin(), indexed_.end(), sortedLoaded.begin(), sortedLoaded.end(),
                std::back_inserter(candidates_));
        }
        return candidates_;
    }

private:
    static void SortUnique(std::vector<std::string>& names) {
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
    }

    std::optional<std::uint64_t> generation_; // indexed_ を作った完成済み索引の世代。
    std::vector<std::string> indexed_;
    std::vector<std::string> loaded_;
    std::vector<std::string> candidates_;
};

inline constexpr std::string_view kCpuTextureRoot = "Resources/sprite/";
inline constexpr std::string_view kGpuTextureRoot = "Resources/sprite/particle/";
inline constexpr std::string_view kModelRoot = "Resources/3DModel/";

// CPU側の保存値は旧形式のルート相対パスも受け付けるため、候補にはルートを含めない。
inline std::string_view CpuTextureName(std::string_view path) {
    return path.starts_with(kCpuTextureRoot) && path.ends_with(".png")
        ? path.substr(kCpuTextureRoot.size()) : std::string_view{};
}

inline bool IsGpuTexturePath(std::string_view path) {
    return path.find(kGpuTextureRoot) != std::string_view::npos;
}

inline std::string_view GpuTextureName(std::string_view path) {
    // GPU粒子用の素材パス以外は選択候補に含めない。
    if (!IsGpuTexturePath(path)) return {};
    const auto dot = path.find_last_of('.');
    // 拡張子がなければGPUで使う画像の種類を判定できない。
    if (dot == std::string_view::npos) return {};
    std::string ext(path.substr(dot));
    std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    });
    // 画像として扱う拡張子だけを許可し、GPUのプリセットに保存するフルパスを返す。
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
           ext == ".tga" || ext == ".dds" || ext == ".hdr" ? path : std::string_view{};
}

// ModelManagerの既存キーはモデルファイル名を含まない、素材ルートからのディレクトリ名。
inline std::string_view ModelName(std::string_view path) {
    // モデルの素材ルート外は、モデル選択に混ぜない。
    if (!path.starts_with(kModelRoot)) return {};
    // 対応する3種類のモデル形式だけを候補にする。
    if (!path.ends_with(".obj") && !path.ends_with(".gltf") && !path.ends_with(".glb")) return {};
    path.remove_prefix(kModelRoot.size());
    const auto slash = path.find_last_of('/');
    return slash == std::string_view::npos ? std::string_view(".") : path.substr(0, slash);
}

} // namespace vfxAssetCandidates
#endif
