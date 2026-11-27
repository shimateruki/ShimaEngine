#include "game/actor/player/copy/PlayerCopyTypeCatalog.h"

namespace {
const std::array<PlayerCopyTypeDescriptor, 5> kCopyTypes = {{
    {"Slime", "Characters/slime_pink", {1.00f, 0.62f, 0.90f, 1.0f}},
    {"FireSlime", "Characters/slime_red", {1.00f, 0.42f, 0.28f, 1.0f}},
    {"ThunderSlime", "Characters/slime_yellow", {1.00f, 0.92f, 0.30f, 1.0f}},
    {"WindSlime", "Characters/slime_wind", {0.48f, 1.00f, 0.82f, 1.0f}},
    {"Bomber", "Characters/slime_black", {0.52f, 0.43f, 0.62f, 1.0f}},
}};
}

const std::array<PlayerCopyTypeDescriptor, 5>& playerCopyTypeCatalog::GetAll() {
    // 編集画面とゲーム側で同じ定義を共有し、呼び出しごとのコピーを避ける。
    return kCopyTypes;
}

const PlayerCopyTypeDescriptor* playerCopyTypeCatalog::Find(std::string_view enemyType) {
    // 王冠は専用の定義を返す。記憶台などが列挙する通常の5種の配列には含めない。
    static const PlayerCopyTypeDescriptor kCrown{"Crown", "Characters/slime_crown", {1.0f, 0.77f, 0.25f, 1.0f}};
    // 王冠を要求された場合は、通常の5種を走査せず専用定義を返す。
    if (enemyType == "Crown")
        return &kCrown;
    for (const PlayerCopyTypeDescriptor& descriptor : kCopyTypes) {
        // 通常の一覧は敵の登録名が一致した要素だけを返す。
        if (descriptor.enemyType == enemyType) {
            return &descriptor;
        }
    }

    return nullptr;
}

bool playerCopyTypeCatalog::IsSupported(std::string_view enemyType) {
    // 登録表に存在する敵型だけをコピー対象として扱う。
    return Find(enemyType) != nullptr;
}

std::string_view playerCopyTypeCatalog::GetEssenceModel(std::string_view enemyType) {
    // 炎のコピー素材は炎専用モデルを使う。
    if (enemyType == "FireSlime")
        return "Gimmicks/copy_essence_fire";
    // 雷のコピー素材は雷専用モデルを使う。
    if (enemyType == "ThunderSlime")
        return "Gimmicks/copy_essence_thunder";
    // 風のコピー素材は風専用モデルを使う。
    if (enemyType == "WindSlime")
        return "Gimmicks/copy_essence_wind";
    // 爆弾のコピー素材は爆弾専用モデルを使う。
    if (enemyType == "Bomber")
        return "Gimmicks/copy_essence_bomb";
    // 上記の専用モデルが定義されていない種類は、ピンクの素材モデルを返す。
    return "Gimmicks/copy_essence_pink";
}
