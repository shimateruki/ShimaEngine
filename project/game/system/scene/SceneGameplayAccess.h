#pragma once

#include "BaseScene.h"
#include <cassert>

class Player;
class GameRule;

// ゲーム固有のPlayerとルールへの参照を、汎用BaseSceneから分離します。
// 関連：BaseSceneからゲーム固有型への直接依存を外し、Player・GameRuleの借用参照を公開する。
// 主な入口：GetPlayer / SetPlayer / GetGameRule
class ISceneGameplayAccess {
  public:
    virtual ~ISceneGameplayAccess() = default;

    // このシーンのプレイヤーを借用する。配置前など対象がいない間の扱いは実装側が決める。
    virtual Player* GetPlayer() const = 0;

    // シーンに配置されたプレイヤーへの参照を設定する。
    virtual void SetPlayer(Player* player) = 0;

    // ゲームルールを提供しないシーンで使う既定値はnullptr。
    virtual GameRule* GetGameRule() { return nullptr; }
};

inline Player* GetScenePlayer(const BaseScene* scene) {
    const auto* gameplay = dynamic_cast<const ISceneGameplayAccess*>(scene);
    // ゲーム用インターフェースを持たないシーンには、この入口から返せるプレイヤーがいない。
    return gameplay ? gameplay->GetPlayer() : nullptr;
}

inline void SetScenePlayer(BaseScene* scene, Player* player) {
    auto* gameplay = dynamic_cast<ISceneGameplayAccess*>(scene);
    assert(gameplay && "Playerを読み込むSceneにはISceneGameplayAccessが必要です。");
    // 実行シーンがゲーム用インターフェースを実装している場合だけ、プレイヤー参照を渡す。
    if (gameplay)
        gameplay->SetPlayer(player);
}

inline GameRule* GetSceneGameRule(BaseScene* scene) {
    auto* gameplay = dynamic_cast<ISceneGameplayAccess*>(scene);
    // 汎用シーンや未指定のシーンでは、借用できるゲームルールはない。
    return gameplay ? gameplay->GetGameRule() : nullptr;
}
