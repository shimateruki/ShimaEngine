#include <cstdint>
#include "CollisionConfig.h"
#include "engine/utility/math/Math.h"
#include "json.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <Windows.h>

namespace engine {

namespace {
constexpr const char* kCategoryPath = "Resources/json/collision/categories.json";

// 古いシーンの数値マスクを読むための対応表。新しいカテゴリ定義には使いません。
constexpr std::array<std::pair<const char*, std::uint32_t>, 9> kLegacyCategories = {{
    {"player", 0}, {"enemy", 1}, {"ground", 2}, {"player_bullet", 3},
    {"trigger", 4}, {"map_block", 5}, {"player_attack", 6},
    {"enemy_attack", 7}, {"hook_anchor", 8},
}};

/// <summary>衝突カテゴリの定義とIDからビット値への検索表を保持する。</summary>
// 関連：CollisionConfigの内部処理だけで使う補助型。
struct CategoryState {
    /// <summary>カテゴリIDを一時文字列なしで検索するためのハッシュ関数。</summary>
    // 関連：CollisionConfigの内部処理だけで使う補助型。
    struct StringHash {
        using is_transparent = void;
        size_t operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
    };

    std::vector<CollisionCategoryDefinition> definitions;
    std::unordered_map<std::string, uint32_t, StringHash, std::equal_to<>> bitsById;
    // JSONが検証済みかを保持する。旧定義へのフォールバック中は追加を受け付けない。
    bool loadedFromJson = false;
};

// カテゴリ一覧からIDと衝突ビットの検索表を作り直します。
void RebuildLookup(CategoryState& state) {
    state.bitsById.clear();

    for (const auto& definition : state.definitions) {
        state.bitsById.emplace(definition.id, definition.bit);
    }
}

bool IsValidId(const std::string& id) {
    // カテゴリ ID は小文字で始め、以降も英小文字・数字・下線に限定する。
    if (id.empty() || !std::islower(static_cast<unsigned char>(id[0])))
        return false;
    return std::all_of(id.begin() + 1, id.end(), [](unsigned char ch) {
        return std::islower(ch) || std::isdigit(ch) || ch == '_';
    });
}

// JSONの形式・ID・ビット重複を検証して衝突カテゴリを読み込みます。
bool LoadCategories(CategoryState& state) {
    std::ifstream file(kCategoryPath, std::ios::binary);
    // 定義ファイルを開けなければ、カテゴリ一覧を読み込めないので失敗を返す。
    if (!file)
        return false;
    try {
        const auto root = nlohmann::json::parse(file);
        // 定義の形式がバージョン1のオブジェクトで、カテゴリ配列がある場合だけ中身を読む。
        if (!root.is_object() || root.value("version", 0) != 1 ||
            !root.contains("categories") || !root["categories"].is_array()) return false;

        std::vector<CollisionCategoryDefinition> parsed;
        std::unordered_set<std::string> ids;
        uint32_t usedBits = 0;

        for (const auto& item : root["categories"]) {
            // ID・表示名・ビット番号の存在と型を検証し、一件でも形式が違えば一覧全体を採用しない。
            if (!item.is_object() || !item.contains("id") || !item["id"].is_string() ||
                !item.contains("displayName") || !item["displayName"].is_string() ||
                !item.contains("bitIndex") || !item["bitIndex"].is_number_integer()) return false;
            const std::string id = item["id"].get<std::string>();
            const std::string name = item["displayName"].get<std::string>();
            const std::int32_t index = item["bitIndex"].get<std::int32_t>();
            // 使えないIDや表示名、範囲外のビット、重複したIDがあれば定義を拒否する。
            if (!IsValidId(id) || name.empty() || index < 0 || index >= 32 || !ids.insert(id).second)
                return false;
            const uint32_t bit = uint32_t{1} << index;
            // ビット番号が別カテゴリに割り当て済みなら、同じビットを再使用しない。
            if ((usedBits & bit) != 0)
                return false;
            usedBits |= bit;
            parsed.push_back({id, name, bit});
        }

        for (const auto& [id, ignoredIndex] : kLegacyCategories) {
            // 旧Maskとの互換変換に必要なカテゴリは削除を許さない。
            if (ids.count(id) == 0)
                return false;
        }

        state.definitions = std::move(parsed);
        RebuildLookup(state);
        state.loadedFromJson = true;
        return true;
    // 構文や型の変換で例外があれば失敗を返し、初回はCategories()が旧定義を用意する。
    } catch (const std::exception&) {
        return false;
    }
}

// 定義と検索表を初回の取得時に用意し、同じプロセスで共有する。
CategoryState& Categories() {
    static CategoryState sState = [] {
        CategoryState loaded;

        // カテゴリの初期読込に失敗した場合は、旧形式の定義をこのプロセスのカテゴリ表に保持する。
        if (!LoadCategories(loaded)) {
            for (const auto& [id, index] : kLegacyCategories) {
                loaded.definitions.push_back({id, id, uint32_t{1} << index});
            }

            RebuildLookup(loaded);
        }

        return loaded;
    }();
    return sState;
}

bool SaveCategories(const std::vector<CollisionCategoryDefinition>& definitions) {
    // 旧ファイルを直接編集せず、一時JSONを出力してから保存先を置き換える。
    namespace fs = std::filesystem;
    const fs::path path(kCategoryPath);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // 保存用フォルダーを用意できなければ、一時ファイルの作成に進まない。
    if (ec)
        return false;
    nlohmann::json root;
    root["version"] = 1;
    root["categories"] = nlohmann::json::array();

    for (const auto& definition : definitions) {
        // 実行時の1ビットのマスクを、保存形式が持つ0～31の位置に戻す。
        std::uint32_t index = 0;
        // 一ビットだけが立つマスクを、最下位まで右へ移した回数から保存位置に戻す。
        while ((definition.bit >> index) != 1u)
            ++index;
        root["categories"].push_back({
            {"id", definition.id}, {"displayName", definition.displayName}, {"bitIndex", index},
        });
    }

    fs::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        // 一時ファイルを開けない場合は、この保存の失敗を返す。
        if (!file)
            return false;
        file << root.dump(2) << '\n';
        // JSONを一時ファイルへ出力できなかった場合は、現在の定義を置き換えない。
        if (!file.good())
            return false;
    }

    // 一時ファイルで定義を置き換えられなければ、一時ファイルを取り除いて失敗を返す。
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fs::remove(temporary, ec);
        return false;
    }

    return true;
}
} // namespace

uint32_t collisionCategories::Get(std::string_view id) {
    const auto& lookup = Categories().bitsById;
    // 登録がないIDはマスク値0を返す。どのビットも立たないため、呼び出し元がORで合成しても結果を変えない。
    const auto it = lookup.find(id);
    return it == lookup.end() ? 0 : it->second;
}

// 現在の衝突カテゴリ定義を値で返し、呼出元での変更を検索表から分離する。
std::vector<CollisionCategoryDefinition> collisionCategories::GetDefinitions() {
    return Categories().definitions;
}

// 検証済みのJSONを使っている場合にtrueを返す。旧定義へのフォールバック中はfalse。
bool collisionCategories::IsDefinitionLoaded() {
    return Categories().loadedFromJson;
}

bool collisionCategories::Add(const std::string& id, const std::string& displayName, std::string* error) {
    // 使用中ビットを飛ばして最初の空きを割り当て、保存成功後にメモリ上の索引を更新する。
    auto fail = [error](const char* message) {
        // 失敗理由の出力先を渡された場合だけ、メッセージを設定する。
        if (error)
            *error = message;
        return false;
    };

    // IDの書式が不正か表示名が空の場合は、空きビットを使う前に拒否する。
    if (!IsValidId(id) || displayName.empty())
        return fail("IDは英小文字で始まる英小文字・数字・_、表示名は空欄不可です。");
    auto& definitions = Categories().definitions;
    // 定義をJSONから正常に読み込めていない場合は、既存カテゴリの保護のため保存しない。
    if (!Categories().loadedFromJson)
        return fail("カテゴリ定義JSONを読み込めません。ファイルを確認してください。");
    // 同じIDがすでにビットを持つ場合は、新しいカテゴリを割り当てない。
    if (Get(id) != 0)
        return fail("同じIDのカテゴリが既に存在します。");
    uint32_t used = 0;

    for (const auto& definition : definitions)
        used |= definition.bit;
    uint32_t freeBit = 0;

    for (std::uint32_t index = 0; index < 32; ++index) {
        const uint32_t candidate = uint32_t{1} << index;

        // 番号の小さいビットから調べ、まだ使われていない最初のビットを割り当てる。
        if ((used & candidate) == 0) {
            freeBit = candidate;
            break;
        }
    }

    // 32ビットを使い切った場合は、追加するカテゴリに利用できる番号がない。
    if (freeBit == 0)
        return fail("32個の衝突カテゴリを使い切っています。");
    // 保存が失敗した場合に既存マスクを変えないよう、追加を適用したコピーだけを保存へ渡す。
    auto updated = definitions;
    updated.push_back({id, displayName, freeBit});
    // 新しい一覧を保存できなければ、現在の定義とメモリ上の検索表はそのまま使う。
    if (!SaveCategories(updated))
        return fail("カテゴリ定義JSONへの保存に失敗しました。");
    definitions = std::move(updated);
    RebuildLookup(Categories());
    // エラーの出力先を渡された場合は、追加の成功時に古いエラーを消す。
    if (error)
        error->clear();
    return true;
}

uint32_t collisionCategories::FromLegacyMask(uint32_t legacyMask) {
    // 旧カテゴリの下位ビットだけを現在の ID 定義へ置き換え、拡張ビットは保持する。
    uint32_t resolved = legacyMask & ~((uint32_t{1} << 9) - 1);

    for (const auto& [id, index] : kLegacyCategories) {
        // 旧マスクで立っていたカテゴリだけ、現在の同じIDのビットに変換する。
        if ((legacyMask & (uint32_t{1} << index)) != 0)
            resolved |= Get(id);
    }

    return resolved;
}

std::vector<std::string> collisionCategories::GetIds(uint32_t mask) {
    // 現在登録されているカテゴリだけを ID 一覧へ戻す。
    std::vector<std::string> ids;

    for (const auto& definition : Categories().definitions) {
        // 指定マスクでビットが立っているカテゴリのIDだけ、結果の一覧へ入れる。
        if ((mask & definition.bit) != 0)
            ids.push_back(definition.id);
    }

    return ids;
}

// 登録済みの各IDのビットをORで合成する。未登録のIDはマスクに影響しない。
uint32_t collisionCategories::FromIds(const std::vector<std::string>& ids) {
    uint32_t mask = 0;

    for (const auto& id : ids)
        mask |= Get(id);
    return mask;
}

// 定義に登録された全カテゴリを許可するビットマスクを作る。
uint32_t collisionCategories::GetKnownMask() {
    uint32_t mask = 0;

    for (const auto& definition : Categories().definitions)
        mask |= definition.bit;
    return mask;
}

uint32_t collisionCategories::GetDefaultGameplayMask() {
    // 旧データと同じ基本カテゴリだけを初期のゲーム用マスクへ含める。
    uint32_t mask = 0;

    for (const auto& [id, index] : kLegacyCategories) {
        // 旧形式の先頭8カテゴリに対応するビットだけを、共通のマスクに含める。
        if (index < 8)
            mask |= Get(id);
    }

    return mask;
}

// 軸に平行な箱を比較し、重なる場合はAをBから離すための法線と食い込み量を返す。
CollisionInfo CheckAabbCollision(const Aabb& a, const Aabb& b) {
    CollisionInfo info;
    info.isColliding = false;

    // 各軸の中心座標
    engine::Vector3 centerA = (a.min + a.max) * 0.5f;
    engine::Vector3 centerB = (b.min + b.max) * 0.5f;

    // 各軸の(半分の)サイズ
    engine::Vector3 sizeA = (a.max - a.min) * 0.5f;
    engine::Vector3 sizeB = (b.max - b.min) * 0.5f;

    // 中心間の距離ベクトル
    engine::Vector3 distanceVec = centerA - centerB;

    // 半寸法の和から中心間の距離を引く。負値は分離、0は境界の接触を表す。
    float overlapX = (sizeA.x + sizeB.x) - std::abs(distanceVec.x);
    float overlapY = (sizeA.y + sizeB.y) - std::abs(distanceVec.y);
    float overlapZ = (sizeA.z + sizeB.z) - std::abs(distanceVec.z);

    // いずれかの軸で投影範囲が離れていれば、他の軸が重なっても衝突しない。
    if (overlapX < 0.0f || overlapY < 0.0f || overlapZ < 0.0f) {
        return info;
    }

    // 全軸で範囲が重なるか接していれば、押し戻しに使う軸と深さを選ぶ。
    info.isColliding = true;

    // Yの重なりがXとZの両方より小さいときは、上下方向を押し戻し軸に使う。
    if (overlapY < overlapX && overlapY < overlapZ) {
        info.penetration = overlapY;

        // AがBより上にあるときは、Aを押し戻す法線を上向きにする。
        info.normal = (distanceVec.y > 0.0f) ? engine::Vector3{ 0, 1, 0 } : engine::Vector3{ 0, -1, 0 };
    // Yを採用せず、Xの重なりがZより小さければ、X方向を押し戻し軸に使う。
    } else if (overlapX < overlapZ) {
        info.penetration = overlapX;

        // AがBより正のX側にあれば、Aを押し戻す法線も正のXへ向ける。
        info.normal = (distanceVec.x > 0.0f) ? engine::Vector3{ 1, 0, 0 } : engine::Vector3{ -1, 0, 0 };
    // 残る場合はZ方向を採る。XとZが同じ深さの場合もこの分岐に入る。
    } else {
        info.penetration = overlapZ;

        // AがBの正のZ側にあれば、Aを押し戻す法線も正のZへ向ける。
        info.normal = (distanceVec.z > 0.0f) ? engine::Vector3{ 0, 0, 1 } : engine::Vector3{ 0, 0, -1 };
    }

    return info;
}


// 二つの球の中心距離から食い込みを求め、重なる場合はAを外へ向ける法線を返す。
CollisionInfo CheckSphereCollision(
    const engine::Vector3& posA, float rA, const engine::Vector3& posB, float rB) {
    CollisionInfo info;
    engine::Math math;
    engine::Vector3 vecAtoB = posB - posA;
    float distance = math.Length(vecAtoB);
    float totalRadius = rA + rB;
    float penetration = totalRadius - distance;

    // 二つの半径の和が中心間距離より大きい場合だけ、球が重なったと判定する。
    if (penetration > 0) {
        info.isColliding = true;
        info.penetration = penetration;

        // 中心の間隔が十分なら、BからAに向かう単位方向を押し戻し法線にする。
        if (distance > 0.001f) {
            info.normal = (vecAtoB / distance) * -1.0f;
        // 中心の間隔が小さすぎる場合は、正規化せず正のXを法線にする。
        } else {
            info.normal = { 1.0f, 0, 0 };
        }
    // 食い込みが0以下なら、接しているだけの場合も含めて未衝突とする。
    } else {
        info.isColliding = false;
    }

    return info;
}

// 軸に平行な箱と球の重なりを最近接点で調べ、球を外へ向ける法線を返す。
CollisionInfo CheckSphereAabbCollision(
    const engine::Vector3& spherePos, float sphereRadius, const Aabb& aabb) {
    CollisionInfo info;
    engine::Math math;

    // 各軸で球中心を箱の範囲へ収め、箱の内部または表面にある最近接点を得る。
    engine::Vector3 closestPoint = {
        math.Clamp(spherePos.x, aabb.min.x, aabb.max.x),
        math.Clamp(spherePos.y, aabb.min.y, aabb.max.y),
        math.Clamp(spherePos.z, aabb.min.z, aabb.max.z)
    };

    // 距離二乗と半径二乗を先に比較し、軸方向が必要な接触時だけ平方根を求める。
    const engine::Vector3 difference = spherePos - closestPoint;
    const float distanceSq = math.Dot(difference, difference);

    // 最近接点と球中心の距離二乗が半径二乗未満なら、球と箱が重なる。
    if (distanceSq < (sphereRadius * sphereRadius)) {
        info.isColliding = true;

        // 最近接点との差が十分なら、その差から食い込みと球を外へ向ける法線を決める。
        if (distanceSq > 0.001f) {
            float distance = std::sqrt(distanceSq);
            info.penetration = sphereRadius - distance;
            info.normal = (spherePos - closestPoint) / distance;
        // 最近接点との差が微小なときは、箱の中心から球中心への方向を代用する。
        } else {
            engine::Vector3 aabbCenter = (aabb.min + aabb.max) * 0.5f;
            engine::Vector3 vecToCenter = aabbCenter - spherePos;

            // 箱の中心と球の中心もほぼ一致する場合、向きを確定できないので正のXに固定する。
            if (math.Length(vecToCenter) < 0.001f) {
                info.normal = { 1, 0, 0 };
            // 中心同士の間隔がある場合は、箱の中心から球中心へ向かう方向を正規化する。
            } else {
                info.normal = math.Normalize(vecToCenter) * -1.0f;
            }

            info.penetration = sphereRadius;
        }
    // 最近接点が球の半径以上に離れていれば、重なりとして扱わない。
    } else {
        info.isColliding = false;
    }

    return info;
}


CollisionFace GetCollisionFace(const engine::Vector3& normal, float threshold) {
    // 上下面を優先し、正のY成分がしきい値を超える法線を上面として扱う。
    if (normal.y > threshold) {
        return CollisionFace::kTop;
    }
    // 負のY成分の大きさがしきい値を超えれば下面として扱う。
    if (normal.y < -threshold) {
        return CollisionFace::kBottom;
    }

    // 上下面の条件に該当せず、正のX成分がしきい値を超えれば右面として扱う。
    if (normal.x > threshold) {
        return CollisionFace::kRight;
    }
    // 負のX成分の大きさがしきい値を超えれば左面として扱う。
    if (normal.x < -threshold) {
        return CollisionFace::kLeft;
    }

    // 上下左右に該当せず、正のZ成分がしきい値を超えれば前面として扱う。
    if (normal.z > threshold) {
        return CollisionFace::kFront;
    }
    // 負のZ成分の大きさがしきい値を超えれば背面として扱う。
    if (normal.z < -threshold) {
        return CollisionFace::kBack;
    }

    // どの軸にも強く当たっていない場合は「斜め」
    return CollisionFace::kOther;
}

// 回転した箱の三軸を使って球との最近接点を求める。境界に接した場合も衝突に含める。
CollisionInfo CheckSphereObbCollision(const engine::Vector3& spherePos, float sphereRadius, const Obb& obb) {
    CollisionInfo info = { false, {0,0,0}, 0.0f };

    // 球の中心を箱の軸へ投影し、箱の半寸法に収まる最近接点を求める。
    engine::Vector3 centerToSphere = spherePos - obb.center;
    engine::Vector3 closestPoint = obb.center;

    engine::Math math;

    for (std::int32_t i = 0; i < 3; ++i) {
        float distanceValue = math.Dot(centerToSphere, obb.orientations[i]);

        // X軸の半寸法を既定とし、YとZでは下の分岐で対応する軸の寸法を使う。
        float clamped = std::clamp(distanceValue, -obb.size.x, obb.size.x);

        // ローカルY軸では、Y方向の半サイズで投影点を箱の範囲に収める。
        if (i == 1)
            clamped = std::clamp(distanceValue, -obb.size.y, obb.size.y);
        // ローカルZ軸では、Z方向の半サイズで投影点を箱の範囲に収める。
        if (i == 2)
            clamped = std::clamp(distanceValue, -obb.size.z, obb.size.z);

        // 各軸の寄与を足し、ワールド座標の最近接点を組み立てる。
        closestPoint += obb.orientations[i] * clamped;
    }

    engine::Vector3 diff = spherePos - closestPoint;
    float distanceSq = math.Dot(diff, diff);

    // 箱上の最近接点が球半径以内にある場合は、接触も含めて衝突として扱う。
    if (distanceSq <= (sphereRadius * sphereRadius)) {
        info.isColliding = true;

        float distance = std::sqrt(distanceSq);

        // 最近接点から離れている場合は、その差を球を外へ押し戻す法線に使う。
        if (distance > 0.0001f) {
            info.penetration = sphereRadius - distance;
            info.normal = math.Normalize(diff);
        // 中心が箱内にあるか最近接点との差が微小なら、箱のローカルY軸を法線に使う。
        } else {
            info.penetration = sphereRadius;
            info.normal = obb.orientations[1];
        }
    }

    return info;
}

// 分離軸定理で回転箱を比較し、最小食い込み量とAを押し戻す法線を返す。
CollisionInfo CheckObbCollision(const Obb& a, const Obb& b) {
    CollisionInfo info = { false, {0,0,0}, 0.0f };
    engine::Math math;

    const float aH[3] = { a.size.x, a.size.y, a.size.z };
    const float bH[3] = { b.size.x, b.size.y, b.size.z };

    float R[3][3];
    float absR[3][3];


    // AとBの軸間の回転係数を、両方の箱の投影範囲を求めるために用意する。
    for (std::int32_t i = 0; i < 3; ++i) {
        for (std::int32_t j = 0; j < 3; ++j) {
            R[i][j] = math.Dot(a.orientations[i], b.orientations[j]);

            // 投影半径の計算に使う軸同士の内積は、絶対値で記録する。
            absR[i][j] = std::fabs(R[i][j]);
        }
    }

    // 中心の差をAの座標系へ移し、B軸や外積軸へ射影するときも使い回す。
    engine::Vector3 tVec = b.center - a.center;
    float tA[3] = {
        math.Dot(tVec, a.orientations[0]),
        math.Dot(tVec, a.orientations[1]),
        math.Dot(tVec, a.orientations[2])
    };

    float minOverlap = std::numeric_limits<float>::infinity();
    engine::Vector3 minAxis = engine::Vector3::kZero;

    // 最初にAの各面の法線へ両方の箱を投影する。
    for (std::int32_t i = 0; i < 3; ++i) {
        float ra = aH[i];
        float rb = bH[0] * absR[i][0] + bH[1] * absR[i][1] + bH[2] * absR[i][2];
        float distanceValue = std::fabs(tA[i]);
        float overlap = (ra + rb) - distanceValue;
        // Aのこの軸で投影範囲が接するか離れていれば、二つの箱は重ならない。
        if (overlap <= 0.0f)
            return info;
        // この軸の食い込みが最小なら、押し戻し量と軸の候補を更新する。
        if (overlap < minOverlap) {
            minOverlap = overlap;
            minAxis = a.orientations[i];
            // 軸の向きがBからAへの方向と逆なら、Aを押し戻せる向きに反転する。
            if (math.Dot(a.center - b.center, minAxis) < 0.0f)
                minAxis = minAxis * -1.0f;
        }
    }

    // 次にBの各面の法線を調べる。中心の差はAの座標系から必要な軸へ変換する。
    for (std::int32_t j = 0; j < 3; ++j) {
        float ra = aH[0] * absR[0][j] + aH[1] * absR[1][j] + aH[2] * absR[2][j];
        float rb = bH[j];
        float tB = std::fabs(tA[0] * R[0][j] + tA[1] * R[1][j] + tA[2] * R[2][j]);
        float overlap = (ra + rb) - tB;
        // Bのこの軸で投影範囲が接するか離れていれば、二つの箱は重ならない。
        if (overlap <= 0.0f)
            return info;
        // Bのこの軸で最小の食い込みが見つかった場合も、押し戻し量と軸を更新する。
        if (overlap < minOverlap) {
            minOverlap = overlap;
            minAxis = b.orientations[j];
            // 軸がAからBの側を向いていれば、BからAの側に向け直す。
            if (math.Dot(a.center - b.center, minAxis) < 0.0f)
                minAxis = minAxis * -1.0f;
        }
    }

    // 最後にAとBの軸の外積を調べ、面の法線では検出できない分離を確認する。
    for (std::int32_t i = 0; i < 3; ++i) {
        for (std::int32_t j = 0; j < 3; ++j) {
            engine::Vector3 axis = math.Cross(a.orientations[i], b.orientations[j]);
            float axisLen = math.Length(axis);
            // ほぼ平行な軸同士の外積は長さが小さく、法線にできないため飛ばす。
            if (axisLen < 1e-6f)
                continue;
            axis = axis / axisLen;

            std::int32_t i1 = (i + 1) % 3;
            std::int32_t i2 = (i + 2) % 3;
            std::int32_t j1 = (j + 1) % 3;
            std::int32_t j2 = (j + 2) % 3;

            float ra = aH[i1] * absR[i2][j] + aH[i2] * absR[i1][j];
            float rb = bH[j1] * absR[i][j2] + bH[j2] * absR[i][j1];

            float proj = std::fabs(tA[i2] * R[i1][j] - tA[i1] * R[i2][j]);
            float overlap = (ra + rb) - proj;

            // この外積軸で投影範囲が接するか離れていれば、重なりを否定できる。
            if (overlap <= 0.0f)
                return info;

            // 外積軸の投影値を軸の長さで割り、箱の各軸で求めたワールド距離と比較できるようにする。
            overlap /= axisLen;

            // 外積軸の食い込みをワールド距離に換算しても最小なら、候補を更新する。
            if (overlap < minOverlap) {
                minOverlap = overlap;
                minAxis = axis;
                // 外積軸も押し戻す側がAになるよう、BからAと逆向きなら反転する。
                if (math.Dot(a.center - b.center, minAxis) < 0.0f)
                    minAxis = minAxis * -1.0f;
            }
        }
    }

    // 分離軸がなければ、候補の中で最小の食い込みとその軸を衝突結果にする。
    info.isColliding = true;
    info.penetration = minOverlap;
    info.normal = minAxis;
    return info;
}

// 軸に平行な箱を回転角0のOBBへ変換し、回転箱同士の判定と押し戻し規則を使う。
CollisionInfo CheckAabbObbCollision(const Aabb& a, const Obb& b) {
    Obb obbA;

    obbA.center = (a.min + a.max) * 0.5f;
    obbA.size = (a.max - a.min) * 0.5f;

    // AABBの軸はワールドのXYZに固定し、その軸に沿ったOBBとして扱う。
    obbA.orientations[0] = { 1.0f, 0.0f, 0.0f };
    obbA.orientations[1] = { 0.0f, 1.0f, 0.0f };
    obbA.orientations[2] = { 0.0f, 0.0f, 1.0f };

    return CheckObbCollision(obbA, b);
}

// 厚みと内外の半径を持つリングから球中心に近い点を作り、球の接触と押し戻しを調べる。
CollisionInfo CheckRingSphereCollision(const Ring& ring, const engine::Vector3& spherePos, float sphereRadius) {
    CollisionInfo info = { false, {0,0,0}, 0.0f };
    engine::Math math;

    // 球の中心がリングの平面からどれだけ離れているかを求め、平面上への投影点を出す。
    engine::Vector3 v = spherePos - ring.center;
    float distPlane = math.Dot(v, ring.normal); // 平面からの垂直距離
    engine::Vector3 P = spherePos - ring.normal * distPlane; // 平面上の投影点

    // 法線方向の厚みを考慮し、最近接点へ戻す法線方向の移動量を求める。
    float halfH = ring.height * 0.5f;
    float clampedH = std::clamp(distPlane, -halfH, halfH);
    
    // リング中心から見た平面上の方向と半径の候補を求める。
    engine::Vector3 centerToP = P - ring.center;
    float distCenterToP = math.Length(centerToP);

    engine::Vector3 closestPointOnAnnulus;

    // 球中心の投影点がリングのほぼ中心にある場合、まずリング中心を最近接点の候補にする。
    if (distCenterToP < 0.0001f) {
        closestPointOnAnnulus = ring.center;

        // リングに穴がある場合は、中心軸上から穴の内周へ向けた平面の方向を選ぶ。
        if (ring.innerRadius > 0.0f) {
            engine::Vector3 axis = { 1, 0, 0 };

            // 基準のX軸がリングの法線とほぼ並行なら、外積が求められるよう基準をZ軸に替える。
            if (std::abs(math.Dot(axis, ring.normal)) > 0.99f)
                axis = { 0, 0, 1 };
            engine::Vector3 dir = math.Normalize(math.Cross(ring.normal, axis));
            closestPointOnAnnulus += dir * ring.innerRadius;
        }
    // 投影点が中心から離れていれば、その方向のまま半径を内周と外周の間に収める。
    } else {
        float clampedRadius = std::clamp(distCenterToP, ring.innerRadius, ring.outerRadius);
        closestPointOnAnnulus = ring.center + math.Normalize(centerToP) * clampedRadius;
    }

    // 厚みの範囲で求めておいた法線方向のずれを戻し、立体上の最近接点にする。
    closestPointOnAnnulus += ring.normal * clampedH;

    // 立体上の最近接点と球中心の距離を、球の半径と比較する。
    engine::Vector3 diff = spherePos - closestPointOnAnnulus;
    float distance = math.Length(diff);

    // リング上の最近接点が球の半径以内にあれば、リングと球が接触している。
    if (distance <= sphereRadius) {
        info.isColliding = true;

        // 最近接点と球の中心が十分に離れていれば、球中心へ向かう方向を法線にする。
        if (distance > 0.0001f) {
            info.penetration = sphereRadius - distance;
            info.normal = diff / distance;
        // 最近接点と球中心がほぼ同じ場合は、リング面の法線を返す。
        } else {
            info.penetration = sphereRadius;
            info.normal = ring.normal;
        }
    }

    return info;
}

// 回転箱を外接球で近似し、リングと球の判定を流用する。
CollisionInfo CheckRingObbCollision(const Ring& ring, const Obb& obb) {
    engine::Math math;
    // OBBの半寸法ベクトルの長さは中心から隅までの距離。これを半径にした外接球で近似する。
    return CheckRingSphereCollision(ring, obb.center, math.Length(obb.size)); 
}

// 円柱の中心線分に半径を持たせた形状で近似し、球との重なりを判定する。
CollisionInfo CheckCylinderSphereCollision(const Cylinder& cyl, const engine::Vector3& spherePos, float sphereRadius) {
    CollisionInfo info = { false, {0,0,0}, 0.0f };
    engine::Math math;

    // 円柱の中心軸上の最短点 P を求める
    engine::Vector3 d = spherePos - cyl.center;
    float distOnAxis = math.Dot(d, cyl.axis);
    
    // 軸への投影が円柱の高さを外れた場合、近い端の位置を軸上の最近接点にする。
    float clampedH = std::clamp(distOnAxis, -cyl.height * 0.5f, cyl.height * 0.5f);
    engine::Vector3 P = cyl.center + cyl.axis * clampedH;

    // P から球の中心への距離
    engine::Vector3 diff = spherePos - P;
    float distanceValue = math.Length(diff);

    // 切り詰めた軸上の線分に半径を持たせるため、端付近は平面より丸い接触範囲になる。
    if (distanceValue <= cyl.radius + sphereRadius) {
        info.isColliding = true;
        info.penetration = (cyl.radius + sphereRadius) - distanceValue;

        // 軸上の最近接点から球中心への距離が十分なら、その差から押し戻し法線を求める。
        if (distanceValue > 0.0001f) {
            info.normal = diff / distanceValue;
        // 最近接点と球中心がほぼ一致する場合は、既定の上方向を法線にする。
        } else {
            info.normal = { 0, 1, 0 };
        }
    }

    return info;
}

// 円柱に外接する回転箱を作り、OBB同士の判定で近似する。
CollisionInfo CheckCylinderObbCollision(const Cylinder& cyl, const Obb& obb) {
    engine::Math math;
    Obb cylObb;
    cylObb.center = cyl.center;
    cylObb.orientations[1] = cyl.axis;
    
    // 円柱軸をY軸として使い、外積で残りの直交する二軸を用意する。
    engine::Vector3 axisX = { 1, 0, 0 };

    // 基準Xと円柱軸がほぼ平行ならZを選び、外積がゼロに近づくのを防ぐ。
    if (std::abs(math.Dot(axisX, cyl.axis)) > 0.99f)
        axisX = { 0, 0, 1 };
    cylObb.orientations[0] = math.Normalize(math.Cross(cyl.axis, axisX));
    cylObb.orientations[2] = math.Normalize(math.Cross(cylObb.orientations[0], cyl.axis));
    
    cylObb.size = { cyl.radius, cyl.height * 0.5f, cyl.radius };

    return CheckObbCollision(cylObb, obb);
}

} // namespace engine
