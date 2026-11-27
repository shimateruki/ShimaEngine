#define NOMINMAX
#include <fstream>
#include "Model.h"
#include "DirectXCommon.h"
#include "engine/utility/math/Math.h"
#include <sstream>
#include <cassert>
#include <filesystem>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include "SrvManager.h"
#include <DebugConsole.h>
#include <LightManager.h>

namespace engine {

namespace {
constexpr std::array<char, 8> kModelCacheMagic = { 'G', 'E', '3', 'M', 'C', 'A', 'C', 'H' };
constexpr uint32_t kModelCacheVersion = 2;
constexpr uint32_t kMaxCacheStringSize = 1024 * 1024;
constexpr uint32_t kMaxCacheVectorCount = 10'000'000;
constexpr const char* kDefaultWhiteTexture = "Resources/sprite/common/white.png";

template <typename T>
// POD値をバイナリへそのまま書き込み、モデルキャッシュ保存の基本単位にする。
bool WritePod(std::ostream& os, const T& value) {
    os.write(reinterpret_cast<const char*>(&value), sizeof(T));
    return static_cast<bool>(os);
}

template <typename T>
// バイナリからPOD値を読み戻し、モデルキャッシュ復元の基本単位にする。
bool ReadPod(std::istream& is, T& value) {
    is.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(is);
}

// 文字列のバイト数と本文を順に書き、復元時に本文の終端を判別できるようにする。
bool WriteString(std::ostream& os, const std::string& text) {
    const uint32_t size = static_cast<uint32_t>(text.size());

    // 先に長さを書けなければ、復元時に位置を特定できないので本文を書かない。
    if (!WritePod(os, size)) {
        return false;
    }

    // 空の文字列では長さだけを保存する。
    if (size > 0) {
        os.write(text.data(), size);
    }

    return static_cast<bool>(os);
}

// 文字列長を検証しながら本文を読み戻し、壊れたキャッシュで異常確保しないようにする。
bool ReadString(std::istream& is, std::string& text) {
    uint32_t size = 0;

    // 長さを読めないか上限を超える場合は、出力の領域を確保する前に失敗を返す。
    if (!ReadPod(is, size) || size > kMaxCacheStringSize) {
        return false;
    }

    text.resize(size);

    // 長さが正の場合だけ、確保した領域へ本文を読む。
    if (size > 0) {
        is.read(text.data(), size);
    }

    return static_cast<bool>(is);
}

template <typename T>
// POD配列の要素数と中身をまとめて書き込み、頂点やインデックス配列を保存する。
bool WritePodVector(std::ostream& os, const std::vector<T>& values) {
    const uint32_t count = static_cast<uint32_t>(values.size());

    // 先に要素数を保存できなければ、配列の内容は書かない。
    if (!WritePod(os, count)) {
        return false;
    }

    // 要素がある場合にだけ、配列の連続したバイト列を書き出す。
    if (count > 0) {
        os.write(reinterpret_cast<const char*>(values.data()), sizeof(T) * values.size());
    }

    return static_cast<bool>(os);
}

template <typename T>
// POD配列の要素数を検証してから読み戻し、不正なキャッシュサイズを弾く。
bool ReadPodVector(std::istream& is, std::vector<T>& values) {
    uint32_t count = 0;

    // 件数が読めないか上限を超えれば、配列を拡張せずに失敗を返す。
    if (!ReadPod(is, count) || count > kMaxCacheVectorCount) {
        return false;
    }

    values.resize(count);

    // 要素数が正の場合だけ、確保済みの範囲へ要素を読み込む。
    if (count > 0) {
        is.read(reinterpret_cast<char*>(values.data()), sizeof(T) * values.size());
    }

    return static_cast<bool>(is);
}

// 0から1の範囲に値を丸め、色や係数の異常値を抑える。
float Clamp01(float value) {
    return (std::max)(0.0f, (std::min)(1.0f, value));
}

// 文字列を小文字化したコピーに変換し、拡張子や名前の比較を安定させる。
std::string ToLowerCopy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

// 対象文字列に候補キーワードのいずれかが含まれるかを判定する。
bool ContainsAny(std::string_view text, std::initializer_list<std::string_view> words) {
    for (std::string_view word : words) {
        // 候補の語が一つでも含まれていれば、残りを調べず一致を返す。
        if (text.find(word) != std::string_view::npos) {
            return true;
        }
    }

    return false;
}

// 材質が参照する画像のパスを解決する。実在するパスが見つからなければモデルと同階層の候補を返す。
std::string ResolveMaterialTexturePath(const std::string& directoryPath, const std::string& rawPath) {
    // 絶対パス、モデル基準の相対パス、同じフォルダ内のファイル名の順で探す。
    // 参照が空なら組み立てる画像パスがないため、空を返す。
    if (rawPath.empty()) {
        return "";
    }

    std::filesystem::path path(rawPath);

    // 絶対パスで実在する場合は、モデルディレクトリから探し直さずそのまま使う。
    if (path.is_absolute() && std::filesystem::exists(path)) {
        return path.generic_string();
    }

    const std::filesystem::path relativeCandidate = std::filesystem::path(directoryPath) / path;

    // モデルのディレクトリから連結して存在するパスを作れれば、それを採用する。
    if (std::filesystem::exists(relativeCandidate)) {
        return relativeCandidate.generic_string();
    }

    // 所在を確定できなかった画像は、記録された末尾名をモデルの隣へ置いた候補にする。
    const std::filesystem::path filenameCandidate = std::filesystem::path(directoryPath) / path.filename();
    return filenameCandidate.generic_string();
}

// Assimpマテリアルに記録された画像パスを取得する。ファイルの場所はResolveMaterialTexturePathで解決する。
bool TryGetMaterialTexture(aiMaterial* material, const std::vector<aiTextureType>& types, aiString& outPath) {
    // 材質がなければ用途ごとの登録を調べられない。
    if (!material) {
        return false;
    }

    // 呼び出し元が並べた用途の優先順位に従い、各用途で最初に登録された素材だけを調べる。
    for (aiTextureType type : types) {
        // 優先順で調べた用途の先頭画像を取得できたときに成功を返す。
        if (material->GetTexture(type, 0, &outPath) == AI_SUCCESS) {
            return true;
        }
    }

    return false;
}

// 種別情報が曖昧なテクスチャから用途に合う候補を探し、補助的に採用する。
bool TryFindUnknownTexture(aiMaterial* material, std::initializer_list<std::string_view> keywords, aiString& outPath) {
    // 用途未設定のテクスチャからファイル名のキーワードで代替候補を拾う。
    // 材質を参照できなければ、分類のない画像の一覧も走査しない。
    if (!material) {
        return false;
    }

    const std::uint32_t count = material->GetTextureCount(aiTextureType_UNKNOWN);

    for (std::uint32_t i = 0; i < count; ++i) {
        aiString candidate;

        // 候補のパスを取得できなかった枠はキーワードと比較しない。
        if (material->GetTexture(aiTextureType_UNKNOWN, i, &candidate) != AI_SUCCESS) {
            continue;
        }

        const std::string lower = ToLowerCopy(candidate.C_Str());

        // 候補名の小文字表記に指定語が含まれれば、元のパスを結果にする。
        if (ContainsAny(lower, keywords)) {
            outPath = candidate;
            return true;
        }
    }

    return false;
}

// モデルファイル名から対応する独自キャッシュファイルの保存先パスを作成する。
std::filesystem::path MakeModelCachePath(const std::filesystem::path& sourcePath) {
    // 可能な限り元モデルの相対パスを維持し、同名ファイルのキャッシュ衝突を避ける。
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::current_path(ec).lexically_normal();
    std::filesystem::path absoluteSource = std::filesystem::absolute(sourcePath, ec).lexically_normal();
    std::filesystem::path relativeSource = ec ? sourcePath : absoluteSource.lexically_relative(root);

    // 作業ルート内の相対構造で表現できなければ、キャッシュには元ファイルの末尾名を使う。
    if (relativeSource.empty() || relativeSource.native().find(L"..") == 0) {
        relativeSource = sourcePath.filename();
    }

    std::filesystem::path cachePath = std::filesystem::path("Resources/.cache/model") / relativeSource;
    cachePath += ".meshcache";
    return cachePath.lexically_normal();
}

// モデル本体と依存ファイルの更新時刻を調べ、キャッシュの新しさ判定に使う。
std::filesystem::file_time_type GetLatestModelDependencyTime(const std::filesystem::path& sourcePath) {
    // 本体の時刻を基準にし、同じディレクトリにある外部メッシュ・材質も更新判定へ含める。
    std::error_code ec;
    auto latest = std::filesystem::last_write_time(sourcePath, ec);

    // モデル本体の更新時刻を取得できなければ、時刻の最小値を返す。
    if (ec) {
        return std::filesystem::file_time_type::min();
    }

    const std::filesystem::path parent = sourcePath.parent_path();

    // 親ディレクトリを参照できない場合は、本体の時刻だけを使う。
    if (!std::filesystem::exists(parent, ec)) {
        return latest;
    }

    for (const auto& entry : std::filesystem::directory_iterator(parent, ec)) {
        // 列挙エラーがあるか通常ファイルでない項目は、更新時刻に含めない。
        if (ec || !entry.is_regular_file()) {
            continue;
        }

        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        // 外部バッファ・旧材質と対応モデル拡張子だけを依存時刻の比較に加える。
        if (ext != ".bin" && ext != ".mtl" && ext != ".gltf" && ext != ".glb" && ext != ".obj") {
            continue;
        }

        auto time = std::filesystem::last_write_time(entry.path(), ec);

        // 記録済みの時刻より新しく、取得に成功したファイルだけで最新時刻を更新する。
        if (!ec && time > latest) {
            latest = time;
        }
    }

    return latest;
}

// キャッシュファイルがモデルや依存ファイルより新しいかを確認し、再読み込みの要否を決める。
bool IsModelCacheUpToDate(const std::filesystem::path& sourcePath, const std::filesystem::path& cachePath) {
    // キャッシュの時刻を元モデルと比較し、古いデータを読み込まない。
    std::error_code ec;

    // 元モデルかキャッシュの片方がなければキャッシュを再利用できない。
    if (!std::filesystem::exists(sourcePath, ec) || !std::filesystem::exists(cachePath, ec)) {
        return false;
    }

    const auto sourceTime = GetLatestModelDependencyTime(sourcePath);
    const auto cacheTime = std::filesystem::last_write_time(cachePath, ec);

    // キャッシュ側の更新時刻を取得できない場合も、再利用しない。
    if (ec) {
        return false;
    }

    return cacheTime >= sourceTime;
}

// ノードの変換行列と子階層を再帰的に書き込み、階層構造をキャッシュへ保存する。
bool WriteNode(std::ostream& os, const Model::Node& node) {
    // ノードの変換情報を先に保存し、子を再帰的に続けて階層順を維持する。
    // 変換とノード名を一組として先に書き、いずれかが失敗したら子を書かない。
    if (!WritePod(os, node.transform) ||
        !WritePod(os, node.localMatrix) ||
        !WriteString(os, node.name)) {
        return false;
    }

    const uint32_t childCount = static_cast<uint32_t>(node.children.size());

    // 子の件数を書けなければツリーを復元できないため終了する。
    if (!WritePod(os, childCount)) {
        return false;
    }

    for (const auto& child : node.children) {
        // どの子で失敗した場合も、親の書込結果を失敗とする。
        if (!WriteNode(os, child)) {
            return false;
        }
    }

    return true;
}

// キャッシュからノード階層を再帰的に読み戻し、モデルの親子構造を復元する。
bool ReadNode(std::istream& is, Model::Node& node) {
    // 子数を上限で検証してから再帰し、壊れたキャッシュによる巨大な確保を防ぐ。
    // ノード本体の変換と名前が揃わなければ階層の復元へ進まない。
    if (!ReadPod(is, node.transform) ||
        !ReadPod(is, node.localMatrix) ||
        !ReadString(is, node.name)) {
        return false;
    }

    // この時点では親の行列を合成せず、読み込み後の骨格構築と更新に階層計算を任せます。
    node.globalMatrix = node.localMatrix;

    uint32_t childCount = 0;

    // 子の件数を読み取れないか上限を超える場合は、子を確保しない。
    if (!ReadPod(is, childCount) || childCount > kMaxCacheVectorCount) {
        return false;
    }

    node.children.resize(childCount);

    for (auto& child : node.children) {
        // いずれかの子の読込が失敗したら、親ごと失敗として扱う。
        if (!ReadNode(is, child)) {
            return false;
        }
    }

    return true;
}

// 親から子への順でノードの値を平坦な配列へ複製する。
void CollectNodes(const Model::Node& node, std::vector<Model::Node>& nodes) {
    nodes.push_back(node);

    for (const auto& child : node.children) {
        CollectNodes(child, nodes);
    }
}

// アニメーション名・時間・各ノードのキー情報をキャッシュへ保存する。
bool WriteAnimation(std::ostream& os, const Model::Animation& animation) {
    // ノード別の3種のキーを読み込み側と同じ順序で記録する。
    // 名前・継続時間・tick換算値が揃わなければ、キー配列を書き始めない。
    if (!WriteString(os, animation.name) ||
        !WritePod(os, animation.duration) ||
        !WritePod(os, animation.ticksPerSecond)) {
        return false;
    }

    const uint32_t nodeAnimCount = static_cast<uint32_t>(animation.nodeAnimations.size());

    // ノードアニメーションの件数が書けなければ、各ノードの内容を書かない。
    if (!WritePod(os, nodeAnimCount)) {
        return false;
    }

    for (const auto& nodeAnim : animation.nodeAnimations) {
        // ノード名と三種類のキーのいずれかを書けなければ、アニメーション全体を失敗とする。
        if (!WriteString(os, nodeAnim.name) ||
            !WritePodVector(os, nodeAnim.scale) ||
            !WritePodVector(os, nodeAnim.rotate) ||
            !WritePodVector(os, nodeAnim.translate)) {
            return false;
        }
    }

    return true;
}

// キャッシュからアニメーション情報を読み戻し、再生に使える形へ復元する。
bool ReadAnimation(std::istream& is, Model::Animation& animation) {
    // ノード数を検証してからキー配列を復元し、途中失敗はキャッシュ無効として返す。
    // 名前と時間情報を最後まで読めない場合は、キーを復元しない。
    if (!ReadString(is, animation.name) ||
        !ReadPod(is, animation.duration) ||
        !ReadPod(is, animation.ticksPerSecond)) {
        return false;
    }

    uint32_t nodeAnimCount = 0;

    // ノードアニメーション数を取得できないか上限を超えた場合は、配列を確保しない。
    if (!ReadPod(is, nodeAnimCount) || nodeAnimCount > kMaxCacheVectorCount) {
        return false;
    }

    animation.nodeAnimations.resize(nodeAnimCount);

    for (auto& nodeAnim : animation.nodeAnimations) {
        // ノード名とスケール・回転・移動の全キーを復元できない場合は失敗を返す。
        if (!ReadString(is, nodeAnim.name) ||
            !ReadPodVector(is, nodeAnim.scale) ||
            !ReadPodVector(is, nodeAnim.rotate) ||
            !ReadPodVector(is, nodeAnim.translate)) {
            return false;
        }
    }

    return true;
}

// 読み込み済みModelData全体を独自キャッシュとして保存し、次回起動時の読み込みを軽くする。
bool WriteModelCache(const std::filesystem::path& sourcePath, const Model::ModelData& data) {
    // キャッシュを書込形式の順に出力する。途中の書込みに失敗したらfalseを返す。
    const std::filesystem::path cachePath = MakeModelCachePath(sourcePath);
    std::error_code ec;
    std::filesystem::create_directories(cachePath.parent_path(), ec);

    // キャッシュ先のディレクトリを用意できなければ、ファイルを開かない。
    if (ec) {
        return false;
    }

    std::ofstream os(cachePath, std::ios::binary);

    // 保存先を開けない場合は、識別子や各配列を書き出せないため終了する。
    if (!os) {
        return false;
    }

    os.write(kModelCacheMagic.data(), kModelCacheMagic.size());

    // 識別子に続く形式のバージョンを出力できなければ、各設定を保存しない。
    if (!WritePod(os, kModelCacheVersion)) {
        return false;
    }

    const uint8_t hasSkinning = data.hasSkinning ? 1 : 0;
    const uint8_t usesNodeAnimationProxy = data.usesNodeAnimationProxy ? 1 : 0;

    // 二つの変形方式フラグを出力できなければ、材質以下も書けない。
    if (!WritePod(os, hasSkinning) || !WritePod(os, usesNodeAnimationProxy)) {
        return false;
    }

    const uint32_t materialCount = static_cast<uint32_t>(data.materials.size());

    // GPUハンドルは実行ごとに変わるため、ここでは再構築に必要な材質値だけ保存する。
    if (!WritePod(os, materialCount)) {
        return false;
    }

    for (const auto& material : data.materials) {
        const uint8_t hasNormalMap = material.hasNormalMap ? 1 : 0;
        const uint8_t hasOrmMap = material.hasOrmMap ? 1 : 0;
        const uint8_t doubleSided = material.doubleSided ? 1 : 0;
        // 材質一件の参照パス・係数・描画フラグを書ききれなければ、後続を書かず失敗を返す。
        if (!WriteString(os, material.textureFilePath) ||
            !WriteString(os, material.normalMapPath) ||
            !WriteString(os, material.ormMapPath) ||
            !WritePod(os, material.baseColorFactor) ||
            !WritePod(os, material.roughness) ||
            !WritePod(os, material.metallic) ||
            !WritePod(os, hasNormalMap) ||
            !WritePod(os, hasOrmMap) ||
            !WritePod(os, doubleSided)) {
            return false;
        }
    }

    const uint32_t meshCount = static_cast<uint32_t>(data.meshes.size());

    // メッシュ本体の頂点・インデックスは保存し、GPUバッファは復元時に作り直す。
    if (!WritePod(os, meshCount)) {
        return false;
    }

    for (const auto& mesh : data.meshes) {
        // 材質への参照と頂点・面の配列のいずれかが書けない場合、そのファイルの書込を中止する。
        if (!WritePod(os, mesh.materialIndex) ||
            !WritePodVector(os, mesh.vertices) ||
            !WritePodVector(os, mesh.indices)) {
            return false;
        }
    }

    const uint32_t boneCount = static_cast<uint32_t>(data.bones.size());

    // 逆バインド行列を追記する前に、ボーン件数を保存できたか確認する。
    if (!WritePod(os, boneCount)) {
        return false;
    }

    for (const auto& bone : data.bones) {
        // ボーンの名前と逆バインド行列を両方書けなければ、階層の保存に進まない。
        if (!WriteString(os, bone.name) || !WritePod(os, bone.inverseBindPoseMatrix)) {
            return false;
        }
    }

    // モデルの親子階層を書けなければ、階層に対応するアニメーションも書かない。
    if (!WriteNode(os, data.rootNode)) {
        return false;
    }

    const uint32_t animationCount = static_cast<uint32_t>(data.animations.size());

    // 階層の後にアニメーションを書き、読み込み側でも同じ順番で復元する。
    if (!WritePod(os, animationCount)) {
        return false;
    }

    for (const auto& animation : data.animations) {
        // いずれかのアニメーションの出力に失敗すれば、キャッシュ保存自体を失敗とする。
        if (!WriteAnimation(os, animation)) {
            return false;
        }
    }

    return static_cast<bool>(os);
}

// 独自キャッシュからModelData全体を復元し、通常読み込みを省略できるか確認する。
bool ReadModelCache(const std::filesystem::path& sourcePath, Model::ModelData& data) {
    // 依存ファイルが新しい場合や形式が違う場合は失敗として扱い、Assimp読込へ戻す。
    const std::filesystem::path cachePath = MakeModelCachePath(sourcePath);

    // 存在や更新時刻の条件を満たさないキャッシュは開かない。
    if (!IsModelCacheUpToDate(sourcePath, cachePath)) {
        return false;
    }

    std::ifstream is(cachePath, std::ios::binary);

    // 候補があっても読込ストリームを開けなければ通常のインポートへ戻す。
    if (!is) {
        return false;
    }

    std::array<char, 8> magic{};
    is.read(magic.data(), magic.size());
    uint32_t version = 0;

    // 識別子、版番号、読取状態が全て揃った現行形式だけを復元する。
    if (!is || magic != kModelCacheMagic || !ReadPod(is, version) || version != kModelCacheVersion) {
        return false;
    }

    uint8_t hasSkinning = 0;
    uint8_t usesNodeAnimationProxy = 0;

    // 両方の変形フラグを読めなければ残りの復元を中断する。
    if (!ReadPod(is, hasSkinning) || !ReadPod(is, usesNodeAnimationProxy)) {
        return false;
    }

    data.hasSkinning = hasSkinning != 0;
    data.usesNodeAnimationProxy = usesNodeAnimationProxy != 0;

    uint32_t materialCount = 0;

    // ファイル由来の件数を制限し、異常なキャッシュで巨大な配列を確保しない。
    if (!ReadPod(is, materialCount) || materialCount > kMaxCacheVectorCount) {
        return false;
    }

    data.materials.resize(materialCount);

    for (auto& material : data.materials) {
        uint8_t hasNormalMap = 0;
        uint8_t hasOrmMap = 0;
        uint8_t doubleSided = 0;
        // 材質の参照パス、係数、フラグに読めない項目があればキャッシュ復元を中断する。
        if (!ReadString(is, material.textureFilePath) ||
            !ReadString(is, material.normalMapPath) ||
            !ReadString(is, material.ormMapPath) ||
            !ReadPod(is, material.baseColorFactor) ||
            !ReadPod(is, material.roughness) ||
            !ReadPod(is, material.metallic) ||
            !ReadPod(is, hasNormalMap) ||
            !ReadPod(is, hasOrmMap) ||
            !ReadPod(is, doubleSided)) {
            return false;
        }

        material.hasNormalMap = hasNormalMap != 0;

        // 保存しないGPU資源の参照は明示的に外し、後段の初期化で再生成する。
        material.hasOrmMap = hasOrmMap != 0;
        material.doubleSided = doubleSided != 0;
        material.textureHandle = 0;
        material.normalMapHandle = 0;
        material.ormMapHandle = 0;
        material.materialResource.Reset();
        material.materialData = nullptr;
    }

    uint32_t meshCount = 0;

    // メッシュ件数が読めないか上限を超える場合は、頂点とインデックスの復元に進まない。
    if (!ReadPod(is, meshCount) || meshCount > kMaxCacheVectorCount) {
        return false;
    }

    data.meshes.resize(meshCount);

    for (auto& mesh : data.meshes) {
        // メッシュの材質番号と両方の配列を読めなければ、キャッシュを採用しない。
        if (!ReadPod(is, mesh.materialIndex) ||
            !ReadPodVector(is, mesh.vertices) ||
            !ReadPodVector(is, mesh.indices)) {
            return false;
        }

        mesh.vertexResource.Reset();

        // 読み込んだCPUデータに対応する描画バッファはModel初期化時に作り直す。
        mesh.indexResource.Reset();
        mesh.vertexBufferView = {};
        mesh.indexBufferView = {};
    }

    uint32_t boneCount = 0;

    // 骨の件数が読めないか上限を超える場合は、骨の配列を確保しない。
    if (!ReadPod(is, boneCount) || boneCount > kMaxCacheVectorCount) {
        return false;
    }

    data.bones.resize(boneCount);

    for (auto& bone : data.bones) {
        // ボーンの参照名か逆バインド行列を復元できなければ中断する。
        if (!ReadString(is, bone.name) || !ReadPod(is, bone.inverseBindPoseMatrix)) {
            return false;
        }
    }

    // 元となるノード階層が読めなければ、ノードの一覧やアニメーションを復元しない。
    if (!ReadNode(is, data.rootNode)) {
        return false;
    }

    data.nodes.clear();

    // 保存した親子ツリーから、名前検索とアニメーション適用に使う平坦な一覧を再構築する。
    CollectNodes(data.rootNode, data.nodes);

    uint32_t animationCount = 0;

    // アニメーションの件数を読めないか上限を超える場合は、再生データを確保しない。
    if (!ReadPod(is, animationCount) || animationCount > kMaxCacheVectorCount) {
        return false;
    }

    data.animations.resize(animationCount);

    for (auto& animation : data.animations) {
        // 読み込めないアニメーションが一件でもあれば、キャッシュ全体を利用できない。
        if (!ReadAnimation(is, animation)) {
            return false;
        }
    }

    return static_cast<bool>(is);
}
}




// モデルファイルを読み込み、メッシュ・マテリアル・ボーン・アニメーション情報をModelDataへ変換する。
Model::ModelData Model::LoadFile(const std::string& directoryPath, const std::string& filename) {

    ModelData modelData;

    std::string sep = (directoryPath.back() == '/' || directoryPath.back() == '\\') ? "" : "/";
    std::string filePath = directoryPath + sep + filename;

    // 現行形式のキャッシュを最後まで読めた場合は、骨格だけ組み立ててインポートを省く。
    if (ReadModelCache(filePath, modelData)) {
        modelData.skeleton = CreateSkeleton(modelData.rootNode);
        return modelData;
    }

    // キャッシュを使えない場合は、Assimpで三角形と左手座標系へ変換しながら読み込む。
    Assimp::Importer importer;


    const aiScene* scene = importer.ReadFile(filePath,
        aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_ConvertToLeftHanded |
        aiProcess_GenSmoothNormals |
        aiProcess_CalcTangentSpace | aiProcess_JoinIdenticalVertices | aiProcess_OptimizeMeshes);

    // シーンを取得できない、部分的にしか読めない、またはルートがない場合は、得られているデータを返す。
    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
        return modelData;
    }

    // ノードの親子階層と参照用の一覧を読み込む。
    modelData.rootNode = ReadNode(scene->mRootNode, modelData.nodes);

    // 描画用テクスチャと材質係数を取り込み、指定がない項目には既定値を使う。
    modelData.materials.resize(scene->mNumMaterials);

    for (std::uint32_t i = 0; i < scene->mNumMaterials; ++i) {
        aiMaterial* aiMat = scene->mMaterials[i];
        aiString texPath;
        MaterialData& material = modelData.materials[i];

        // ベース色用または旧Diffuse画像があれば、モデル基準のパスに変換する。
        if (TryGetMaterialTexture(aiMat, { aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE }, texPath)) {
            material.textureFilePath = ResolveMaterialTexturePath(directoryPath, texPath.C_Str());
        }
        // ベース色に使える画像参照がなければ、共有の白画像を割り当てる。
        else {
            material.textureFilePath = kDefaultWhiteTexture;
        }

        // 法線画像の用途か名前で候補を見つけられたときに、法線を使う設定を立てる。
        if (TryGetMaterialTexture(aiMat, { aiTextureType_NORMALS, aiTextureType_NORMAL_CAMERA, aiTextureType_HEIGHT }, texPath) ||
            TryFindUnknownTexture(aiMat, { "normal", "nrm" }, texPath)) {
            material.normalMapPath = ResolveMaterialTexturePath(directoryPath, texPath.C_Str());
            material.hasNormalMap = true;
        }

        // 金属度・粗さ用の用途か名前で候補を確認できたときに、ORM画像として記録する。
        if (TryGetMaterialTexture(aiMat, { aiTextureType_METALNESS, aiTextureType_DIFFUSE_ROUGHNESS }, texPath) ||
            TryFindUnknownTexture(aiMat, { "metallicroughness", "metallic_roughness", "metallic-roughness", "orm", "arm", "roughness", "metallic" }, texPath)) {
            material.ormMapPath = ResolveMaterialTexturePath(directoryPath, texPath.C_Str());
            material.hasOrmMap = true;
        }

        aiColor4D baseColor{};
        // ベース色係数を取得できた場合は優先し、なければ旧Diffuseの色係数を使う。
        if (aiMat->Get(AI_MATKEY_BASE_COLOR, baseColor) == AI_SUCCESS ||
            aiMat->Get(AI_MATKEY_COLOR_DIFFUSE, baseColor) == AI_SUCCESS) {
            material.baseColorFactor = { baseColor.r, baseColor.g, baseColor.b, baseColor.a };
        }

        float roughness = material.roughness;

        // 粗さを直接取得できた場合に、描画が使う0～1へ補正する。
        if (aiMat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness) == AI_SUCCESS) {
            material.roughness = Clamp01(roughness);
        }
        // 粗さ指定がない場合は、旧材質の光沢から近い粗さを求める。
        else {
            float shininess = 0.0f;

            // 旧材質に正の光沢がある場合だけ、光沢が強いほど小さくなる粗さへ換算する。
            if (aiMat->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS && shininess > 0.0f) {
                material.roughness = Clamp01(1.0f - (shininess / 128.0f));
            }
        }

        float metallic = material.metallic;

        // 金属度を取得できた場合に、描画が使う0～1へ補正する。
        if (aiMat->Get(AI_MATKEY_METALLIC_FACTOR, metallic) == AI_SUCCESS) {
            material.metallic = Clamp01(metallic);
        }

        std::int32_t twoSided = 0;

        // 両面フラグを取得できた場合だけ、描画設定へ反映する。
        if (aiMat->Get(AI_MATKEY_TWOSIDED, twoSided) == AI_SUCCESS) {
            material.doubleSided = twoSided != 0;
        }
    }

    // Assimpの頂点・面・ボーンを、描画バッファ作成前のCPU用データにまとめる。
    for (std::uint32_t i = 0; i < scene->mNumMeshes; i++) {
        aiMesh* aiMesh = scene->mMeshes[i];
        Mesh mesh;
        mesh.materialIndex = aiMesh->mMaterialIndex;

        std::vector<VertexData> tempVertices;
        tempVertices.resize(aiMesh->mNumVertices);

        for (std::uint32_t v = 0; v < aiMesh->mNumVertices; ++v) {
            VertexData& vertex = tempVertices[v];
            vertex.position = { aiMesh->mVertices[v].x, aiMesh->mVertices[v].y, aiMesh->mVertices[v].z, 1.0f };

            // インポート結果に法線がある場合は、その頂点の向きをコピーする。
            if (aiMesh->HasNormals()) {
                vertex.normal = { aiMesh->mNormals[v].x, aiMesh->mNormals[v].y, aiMesh->mNormals[v].z };
            }
            // 法線を取得できない場合は上向きを使う。
            else {
                vertex.normal = { 0.0f, 1.0f, 0.0f };
            }


            // 接線と従法線が用意されている場合は、接線を転送する。
            if (aiMesh->HasTangentsAndBitangents()) {
                vertex.tangent = { aiMesh->mTangents[v].x, aiMesh->mTangents[v].y, aiMesh->mTangents[v].z };
            }
            // 接線の情報がなければ、暫定の正のX方向を渡す。
            else {
                vertex.tangent = { 1.0f, 0.0f, 0.0f };
            }


            // テクスチャ座標の第1チャネルがある場合は、その頂点のUVを使う。
            if (aiMesh->HasTextureCoords(0)) {
                vertex.texcoord = { aiMesh->mTextureCoords[0][v].x, aiMesh->mTextureCoords[0][v].y };
            }
            // UVの取得先がない頂点は、座標を原点にする。
            else {
                vertex.texcoord = { 0.0f, 0.0f };
            }

            vertex.boneWeights = { 0.0f, 0.0f, 0.0f, 0.0f };
            vertex.boneIndices = { 0.0f, 0.0f, 0.0f, 0.0f };
        }

        // ボーンを使うメッシュが一つでもあれば、モデル全体をスキニング対応として記録する。
        if (aiMesh->mNumBones > 0) {
            modelData.hasSkinning = true;
        }

        for (std::uint32_t b = 0; b < aiMesh->mNumBones; ++b) {
            aiBone* aiBone = aiMesh->mBones[b];
            std::string boneName = aiBone->mName.C_Str();
            std::int32_t boneIndex = -1;

            for (std::int32_t k = 0; k < modelData.bones.size(); ++k) {
                // 同名の骨が登録済みなら、メッシュをまたいで同じ添字を使う。
                if (modelData.bones[k].name == boneName) {
                    boneIndex = k;
                    break;
                }
            }

            // 同名のボーンをまだ持たない場合は、モデル側の骨一覧へ逆バインド行列と共に加える。
            if (boneIndex == -1) {
                boneIndex = (std::int32_t)modelData.bones.size();
                Bone newBone;
                newBone.name = boneName;
                aiMatrix4x4 offset = aiBone->mOffsetMatrix;
                // Assimpの逆バインド行列を転置し、このエンジンの行ベクトルの変換順に合わせます。
                newBone.inverseBindPoseMatrix.m[0][0] = offset.a1;
                newBone.inverseBindPoseMatrix.m[0][1] = offset.b1;
                newBone.inverseBindPoseMatrix.m[0][2] = offset.c1;
                newBone.inverseBindPoseMatrix.m[0][3] = offset.d1;
                newBone.inverseBindPoseMatrix.m[1][0] = offset.a2;
                newBone.inverseBindPoseMatrix.m[1][1] = offset.b2;
                newBone.inverseBindPoseMatrix.m[1][2] = offset.c2;
                newBone.inverseBindPoseMatrix.m[1][3] = offset.d2;
                newBone.inverseBindPoseMatrix.m[2][0] = offset.a3;
                newBone.inverseBindPoseMatrix.m[2][1] = offset.b3;
                newBone.inverseBindPoseMatrix.m[2][2] = offset.c3;
                newBone.inverseBindPoseMatrix.m[2][3] = offset.d3;
                newBone.inverseBindPoseMatrix.m[3][0] = offset.a4;
                newBone.inverseBindPoseMatrix.m[3][1] = offset.b4;
                newBone.inverseBindPoseMatrix.m[3][2] = offset.c4;
                newBone.inverseBindPoseMatrix.m[3][3] = offset.d4;
                modelData.bones.push_back(newBone);
            }

            for (std::uint32_t w = 0; w < aiBone->mNumWeights; ++w) {
                std::uint32_t vertexId = aiBone->mWeights[w].mVertexId;
                float weight = aiBone->mWeights[w].mWeight;

                // 重みが存在する頂点の有効範囲を指す場合にだけ、その頂点へ影響を割り当てる。
                if (vertexId < tempVertices.size()) {
                    auto& v = tempVertices[vertexId];

                    // 頂点に用意された四枠を先に見つかった影響から埋めます。四枠を使い切った頂点には追加しません。
                    if (v.boneWeights.x == 0.0f) {
                        v.boneWeights.x = weight;
                        v.boneIndices.x = (float)boneIndex;
                    }
                    // 先頭の枠に既に重みがある場合は、次に空いている枠を探す。
                    else if (v.boneWeights.y == 0.0f) { v.boneWeights.y = weight; v.boneIndices.y = (float)boneIndex; }
                    // 二つ目も使用中なら、三つ目を確認する。
                    else if (v.boneWeights.z == 0.0f) { v.boneWeights.z = weight; v.boneIndices.z = (float)boneIndex; }
                    // 前三つが使用中の場合は最後の枠を使い、そこも埋まっていれば今回の情報を追加しない。
                    else if (v.boneWeights.w == 0.0f) { v.boneWeights.w = weight; v.boneIndices.w = (float)boneIndex; }
                }
            }
        }

        mesh.vertices = tempVertices;

        // 面に並ぶ頂点番号を登録し、頂点データ自体は共通の配列から参照する。
        for (std::uint32_t f = 0; f < aiMesh->mNumFaces; f++) {
            aiFace face = aiMesh->mFaces[f];

            for (std::uint32_t j = 0; j < face.mNumIndices; j++) {
                mesh.indices.push_back(face.mIndices[j]);
            }
        }

        modelData.meshes.push_back(mesh);
    }

    // ボーンがないモデルでは単位行列の補助ボーンを用意し、ノードアニメーションにも使えるようにする。
    if (modelData.bones.empty()) {
        Bone dummyBone;

        // 最初のアニメーションに対応ノードがある場合は、補助ボーンをそのノードへ紐付ける。
        if (scene->mNumAnimations > 0 && scene->mAnimations[0]->mNumChannels > 0) {
            dummyBone.name = scene->mAnimations[0]->mChannels[0]->mNodeName.C_Str();
            modelData.usesNodeAnimationProxy = true;
        // アニメーションの対応先がなく子ノードがある場合も、変形しない共通ボーン名を使う。
        } else if (scene->mRootNode && scene->mRootNode->mNumChildren > 0) {
            dummyBone.name = "__StaticMeshIdentityBone";
        // 子ノードもない静的モデルでは、同じ単位変換用の名前を使う。
        } else {
            dummyBone.name = "__StaticMeshIdentityBone";
        }

        engine::Math math;
        dummyBone.inverseBindPoseMatrix = math.MakeIdentity4x4();
        modelData.bones.push_back(dummyBone);

        // 補助ボーンを参照できるよう、重みのない頂点をその一つへ紐付ける。
        for (auto& mesh : modelData.meshes) {
            for (auto& v : mesh.vertices) {
                // 四枠とも重みがない場合だけ、補助ボーンに100%の影響を与える。
                if (v.boneWeights.x == 0.0f && v.boneWeights.y == 0.0f &&
                    v.boneWeights.z == 0.0f && v.boneWeights.w == 0.0f) {

                    v.boneWeights = { 1.0f, 0.0f, 0.0f, 0.0f };
                    v.boneIndices = { 0.0f, 0.0f, 0.0f, 0.0f };
                }
            }
        }
    }

    // クリップ名と各ノードのキーを読み、再生時間を秒単位に揃える。
    for (std::uint32_t i = 0; i < scene->mNumAnimations; ++i) {
        aiAnimation* aiAnim = scene->mAnimations[i];
        Animation animation;
        // 元のクリップに名前があれば、再生指定に使う名前として保持する。
        if (aiAnim->mName.length > 0)
            animation.name = aiAnim->mName.C_Str();
        // 名前がないクリップは、元データでの順序から識別名を作る。
        else animation.name = "animation_" + std::to_string(i);
        float ticksPerSecond = (float)(aiAnim->mTicksPerSecond != 0 ? aiAnim->mTicksPerSecond : 25.0);

        // 継続時間と各キーの時刻を同じ換算でtickから秒へ変えます。周波数がなければ毎秒25tickとします。
        animation.duration = (float)aiAnim->mDuration / ticksPerSecond;
        animation.ticksPerSecond = ticksPerSecond;

        for (std::uint32_t c = 0; c < aiAnim->mNumChannels; ++c) {
            aiNodeAnim* aiChannel = aiAnim->mChannels[c];
            NodeAnimation nodeAnim;
            nodeAnim.name = aiChannel->mNodeName.C_Str();

            // Hipsを含むノードだけ、インポートした位置キーの件数を診断ログに残す。
            if (nodeAnim.name.find("Hips") != std::string::npos) {
                std::string log = "AnimNode: " + nodeAnim.name +
                    " | PosKeys: " + std::to_string(aiChannel->mNumPositionKeys) + "\n";
                DebugConsole::GetInstance()->AddLog(log.c_str());
            }

            // 移動キーは秒へ換算した時刻とローカル座標を保持する。
            for (std::uint32_t k = 0; k < aiChannel->mNumPositionKeys; ++k) {
                aiVectorKey& key = aiChannel->mPositionKeys[k];
                KeyframeVector3 kf;
                kf.time = (float)key.mTime / ticksPerSecond;
                kf.value = { key.mValue.x, key.mValue.y, key.mValue.z };
                nodeAnim.translate.push_back(kf);
            }

            // 回転キーは秒へ換算した時刻とクォータニオンを保持する。
            for (std::uint32_t k = 0; k < aiChannel->mNumRotationKeys; ++k) {
                aiQuatKey& key = aiChannel->mRotationKeys[k];
                KeyframeQuaternion kf;
                kf.time = (float)key.mTime / ticksPerSecond;
                kf.value = { key.mValue.x, key.mValue.y, key.mValue.z, key.mValue.w };
                nodeAnim.rotate.push_back(kf);
            }

            // 拡縮キーは秒へ換算した時刻と各軸の倍率を保持する。
            for (std::uint32_t k = 0; k < aiChannel->mNumScalingKeys; ++k) {
                aiVectorKey& key = aiChannel->mScalingKeys[k];
                KeyframeVector3 kf;
                kf.time = (float)key.mTime / ticksPerSecond;
                kf.value = { key.mValue.x, key.mValue.y, key.mValue.z };
                nodeAnim.scale.push_back(kf);
            }

            animation.nodeAnimations.push_back(nodeAnim);
        }

        modelData.animations.push_back(animation);
    }

    // インポートしたノード階層から、スキニングで参照するスケルトンを組み立てる。
    modelData.skeleton = CreateSkeleton(modelData.rootNode);

    // 骨格を用意したCPUデータの保存を試みる。保存結果にかかわらず今回の読込結果を返す。
    WriteModelCache(filePath, modelData);

    return modelData;
}

// Assimpのノード階層をゲーム側のNode構造へ再帰的に変換する。
Model::Node Model::ReadNode(aiNode* node, std::vector<Node>& nodes) {
    // Assimpの行列をエンジンの行列形式へ写し、分解したTransformも保持する。
    Node result;
    result.name = node->mName.C_Str();
    aiMatrix4x4 transform = node->mTransformation;
    result.localMatrix.m[0][0] = transform.a1;
    result.localMatrix.m[0][1] = transform.b1;
    result.localMatrix.m[0][2] = transform.c1;
    result.localMatrix.m[0][3] = transform.d1;
    result.localMatrix.m[1][0] = transform.a2;
    result.localMatrix.m[1][1] = transform.b2;
    result.localMatrix.m[1][2] = transform.c2;
    result.localMatrix.m[1][3] = transform.d2;
    result.localMatrix.m[2][0] = transform.a3;
    result.localMatrix.m[2][1] = transform.b3;
    result.localMatrix.m[2][2] = transform.c3;
    result.localMatrix.m[2][3] = transform.d3;
    result.localMatrix.m[3][0] = transform.a4;
    result.localMatrix.m[3][1] = transform.b4;
    result.localMatrix.m[3][2] = transform.c4;
    result.localMatrix.m[3][3] = transform.d4;
    
    aiVector3D scale, translate;
    aiQuaternion rotate;
    transform.Decompose(scale, rotate, translate);
    result.transform.scale = {scale.x, scale.y, scale.z};
    result.transform.rotate = {rotate.x, rotate.y, rotate.z, rotate.w};
    result.transform.translate = {translate.x, translate.y, translate.z};

    // 参照用の一覧へこのノードを加え、子の実際の階層は戻り値のツリーへ保持する。
    nodes.push_back(result);
    result.children.resize(node->mNumChildren);

    for (std::uint32_t i = 0; i < node->mNumChildren; ++i) {
        result.children[i] = ReadNode(node->mChildren[i], nodes);
    }

    return result;
}



} // namespace engine
