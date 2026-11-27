#include <cstdint>
#include "game/actor/enemy/spawn/EnemyFactory.h"
#include "EnemySlime.h"
#include "EnemyBomb.h"
#include "EnemyFireSlime.h"
#include "EnemyThunderSlime.h"
#include "EnemyWindSlime.h"
#include "EnemyGiantSlime.h"
#include "game/actor/enemy/boss/prism/EnemyPrismSlime.h"
#include "game/actor/enemy/boss/magma/EnemyMagmaSlime.h"
#include "game/actor/enemy/boss/false_king/EnemyFalseKingSlime.h"
#include "EnemyRingBurner.h"
#include <EnemyBomber.h>
#include "game/system/rules/GameplayStatusManager.h"
#include <algorithm>

namespace {
constexpr std::int32_t kSlimeSoftMaterialType = 25;
constexpr std::int32_t kPrismCrystalMaterialType = 27;

bool IsSlimeEnemyType(const std::string& enemyType) {
    // 共通の柔らかい材質を適用する種族だけを列挙し、爆弾などは含めない。
    return enemyType == "Slime" || enemyType == "Bomber" || enemyType == "FireSlime" || enemyType == "ThunderSlime" ||
           enemyType == "WindSlime" || enemyType == "GiantSlime" || enemyType == "PrismSlime" ||
           enemyType == "MagmaSlime" || enemyType == "FalseKingSlime";
}

// ファクトリーで初期化したスライムへ、種族ごとの初期材質を設定する。マグマはモデルの複数材質を使用する。
void ApplySlimeMaterialDefault(BaseEnemy* enemy) {
    // 敵が存在しないか共通材質の対象種でない場合は、初期化済みの材質を触らない。
    if (!enemy || !IsSlimeEnemyType(enemy->GetEnemyType())) {
        return;
    }

    // マグマスライムは複数のPBR材質で外皮と発光亀裂を描き分けます。
    if (enemy->GetEnemyType() == "MagmaSlime") {
        return;
    }

    // プリズム種には結晶材質を使い、残りの対象種には軟質材質を使う。
    enemy->SetMaterialType(enemy->GetEnemyType() == "PrismSlime" ? kPrismCrystalMaterialType : kSlimeSoftMaterialType);
}
} // namespace

EnemyFactory* EnemyFactory::GetInstance() {
    // 生成経路を一つにまとめ、敵タイプの登録先を共有する。
    static EnemyFactory sInstance;
    return &sInstance;
}

// 敵タイプ名ごとに専用クラスを生成します。共通ステータスはGameplayStatusManagerが設定します。
std::unique_ptr<BaseEnemy> EnemyFactory::CreateEnemy(const std::string& enemyName, engine::Object3DCommon* common) {
    std::unique_ptr<BaseEnemy> newEnemy;

    // 登録済みの名前ごとに実体の型と標準モデルを選び、各敵の初期化を済ませてから共通設定へ進みます。
    // 通常のピンクスライムは急降下する敵クラスを生成する。
    if (enemyName == "Slime") {
        auto slime = std::make_unique<EnemySlime>();

        slime->Initialize(common, "Characters/slime_pink");

        newEnemy = std::move(slime);
    // 爆弾として運搬・投擲されるBombは専用の爆弾モデルと導火線を持つ。
    } else if (enemyName == "Bomb") {
        auto bomb = std::make_unique<EnemyBomb>();

        bomb->Initialize(common, "Gimmicks/blob");

        newEnemy = std::move(bomb);
    }

    // 爆弾を投げる敵Bomberは、爆弾本体とは別の黒いスライムとして生成する。
    else if (enemyName == "Bomber") {
        auto bomber = std::make_unique<EnemyBomber>();

        bomber->Initialize(common, "Characters/slime_black");

        newEnemy = std::move(bomber);
    // 炎の通常敵には赤いモデルと炎攻撃を持つクラスを選ぶ。
    } else if (enemyName == "FireSlime") {
        auto fireSlime = std::make_unique<EnemyFireSlime>();
        fireSlime->Initialize(common, "Characters/slime_red");

        newEnemy = std::move(fireSlime);
    // 雷の通常敵には黄色いモデルと雷攻撃を持つクラスを選ぶ。
    } else if (enemyName == "ThunderSlime") {
        auto thunderSlime = std::make_unique<EnemyThunderSlime>();
        thunderSlime->Initialize(common, "Characters/slime_yellow");

        newEnemy = std::move(thunderSlime);
    // 風の通常敵には風専用モデルと風攻撃を持つクラスを選ぶ。
    } else if (enemyName == "WindSlime") {
        auto windSlime = std::make_unique<EnemyWindSlime>();
        windSlime->Initialize(common, "Characters/slime_wind");

        newEnemy = std::move(windSlime);
    // 巨大スライムは専用の移動と拘束条件を扱う派生型を使う。
    } else if (enemyName == "GiantSlime") {
        auto giantSlime = std::make_unique<EnemyGiantSlime>();
        giantSlime->Initialize(common, "Characters/slime");

        newEnemy = std::move(giantSlime);
    // プリズムの中ボスは、結晶モデルと専用の攻撃状態で初期化する。
    } else if (enemyName == "PrismSlime") {
        auto prismSlime = std::make_unique<EnemyPrismSlime>();
        prismSlime->Initialize(common, "Characters/prism_slime");

        newEnemy = std::move(prismSlime);
    // マグマの中ボスには発光する外皮のあるモデルと専用の攻撃状態を用意する。
    } else if (enemyName == "MagmaSlime") {
        auto magmaSlime = std::make_unique<EnemyMagmaSlime>();
        magmaSlime->Initialize(common, "Characters/magma_slime");

        newEnemy = std::move(magmaSlime);
    // 偽王のボスは、偽王専用モデルと複数形態の実装を選ぶ。
    } else if (enemyName == "FalseKingSlime") {
        auto falseKing = std::make_unique<EnemyFalseKingSlime>();
        falseKing->Initialize(common, "Characters/false_king_slime");
        newEnemy = std::move(falseKing);
    // 熱輪を出す固定砲台は、砲台モデルと足場を扱う派生型を選ぶ。
    } else if (enemyName == "RingBurner") {
        auto ringBurner = std::make_unique<EnemyRingBurner>();
        ringBurner->Initialize(common, "Characters/ring_burner");

        newEnemy = std::move(ringBurner);
    }

    // 専用型を生成できた場合だけ、タイプ名を登録して共通ステータスを適用する。
    if (newEnemy) {
        newEnemy->SetEnemyType(enemyName);
        newEnemy->SetClassName("Enemy");
        ApplySlimeMaterialDefault(newEnemy.get());
        auto* statusManager = GameplayStatusManager::GetInstance();
        statusManager->Initialize();
        statusManager->ApplyEnemyStatus(newEnemy.get(), true);
    } else {
        // 未登録の種別には、配置位置を確認できるよう立方体で表示する共通の敵を生成する。
        newEnemy = std::make_unique<BaseEnemy>();
        newEnemy->Initialize(common, "Primitives/cube");
        newEnemy->SetEnemyType("");
    }

    return newEnemy;
}
