#pragma once

// ゲーム固有の登録入口：RegisterGameAttackPreviews。登録先の共通Editorに、個別の型名や分岐を持ち込まない。

#include "game/editor/EnemyAttackPreviewRegistry.h"
#include "EnemySlime.h"
#include "EnemyFireSlime.h"
#include "EnemyThunderSlime.h"
#include "EnemyGiantSlime.h"
#include "EnemyBomber.h"
#include "EnemyWindSlime.h"
#include "game/actor/enemy/boss/prism/EnemyPrismSlime.h"
#include "game/actor/enemy/boss/magma/EnemyMagmaSlime.h"
#include "game/actor/enemy/boss/false_king/EnemyFalseKingSlime.h"
#include "game/actor/enemy/combat/ISequencePreviewEnemy.h"
#include "game/crown/CrownFinaleTuning.h"
#include <cassert>
#include <type_traits>

namespace gameAttackPreviewDetail {
// 専用APIを持たない敵は従来どおり通常AIで実行します。型の不一致は拒否します。
template<class T>
EnemyAttackPreviewDescriptor Make(const char* type, const char* label,
    void (T::*select)(const std::string&) = nullptr,
    const char* (T::*phase)() const = nullptr) {
    EnemyAttackPreviewDescriptor entry;
    entry.type = type;
    entry.displayName = label;
    entry.selectAttack = [select](BaseEnemy& enemy, const std::string& id) {
        auto* concrete = dynamic_cast<T*>(&enemy);
        // 登録した敵型と渡された実体の型が違う場合は、この項目の攻撃を指定できない。
        if (!concrete) return false;
        // 共通プレビューのインターフェースを実装する型では、そちらの攻撃ID設定を優先する。
        if constexpr (std::is_base_of_v<ISequencePreviewEnemy, T>)
            concrete->SetSequencePreviewAttackId(id);
        // 共通インターフェースを持たず、型専用の選択関数が登録されている場合は、その関数にIDを渡す。
        else if (select) (concrete->*select)(id);
        return true;
    };
    entry.phaseName = [phase](const BaseEnemy& enemy) -> const char* {
        const auto* concrete = dynamic_cast<const T*>(&enemy);
        // 表示用に受け取った敵の型が一致しない場合は、段階名を返さない。
        if (!concrete) return nullptr;
        // 共通インターフェースを実装する型は、現在のシーケンスの段階名を先に問い合わせる。
        if constexpr (std::is_base_of_v<ISequencePreviewEnemy, T>)
            // 共通インターフェースが段階名を返せた場合は、その名前を表示する。
            if (const char* label = concrete->GetSequenceStateLabel()) return label;
        // 段階名を得られなければ、登録された型専用の関数を使う。関数が無ければ表示しない。
        return phase ? (concrete->*phase)() : nullptr;
    };
    return entry;
}
// 本番用の既存SetSpawnCallbackを、そのまま隔離プレビューの追加先へ接続します。
template<class T>
void ConnectSpawns(BaseEnemy& enemy, PreviewEnemySpawn spawn) {
    // 登録した敵型に一致する場合だけ、生成を隔離プレビューへ届けるコールバックを付ける。
    if (auto* concrete = dynamic_cast<T*>(&enemy)) concrete->SetSpawnCallback(std::move(spawn));
}
} // namespace gameAttackPreviewDetail

// 既存の敵ID・Profile・攻撃IDをそのまま使用します。登録自体は保存や敵生成を行いません。
inline void RegisterGameAttackPreviews() {
    // ゲーム側の敵型と攻撃選択を、エディターが再利用する一覧へ起動時に一度だけ登録する。
    static const bool kRegistered = [] {
        using namespace gameAttackPreviewDetail;
        std::vector<EnemyAttackPreviewDescriptor> entries;
        entries.push_back(Make<EnemySlime>("Slime", "通常スライム（溜めジャンプ急降下）",
            nullptr, &EnemySlime::GetDebugMoveStateName));

        // 炎は本番AIとコピー能力の両方で再生できるため、シーケンスと本体制御の編集を許可する。
        auto fire = Make<EnemyFireSlime>("FireSlime", "炎スライム");
        fire.damageType = DamageType::kFire;
        fire.targetDistance = 3.2f;
        fire.duration = 3.0f;
        fire.canAuthor = true;
        fire.supportsPlayerBinding = true;
        fire.supportsActorControl = true;
        fire.sequenceHelp = "炎スライムの敵AIと炎コピーE入力に接続しています。命令の時刻0は攻撃開始時です。";
        fire.canEditSequence = [](const EnemyAttackDefinition*, bool) { return true; };
        entries.push_back(std::move(fire));

        // 雷は型に登録された攻撃IDと現在の段階をプレビューの選択・表示に使う。
        auto thunder = Make<EnemyThunderSlime>("ThunderSlime", "雷スライム（近距離放電／連続落雷）",
            &EnemyThunderSlime::SetDebugPreviewAttackId, &EnemyThunderSlime::GetDebugAttackPhaseName);
        thunder.damageType = DamageType::kElectric;
        thunder.targetDistance = 3.2f;
        thunder.duration = 3.2f;
        entries.push_back(std::move(thunder));

        auto giant = Make<EnemyGiantSlime>("GiantSlime", "巨大スライム（ジャンププレス）");
        giant.targetDistance = 9.0f;
        giant.duration = 5.0f;
        giant.phaseName = [](const BaseEnemy& enemy) -> const char* {
            const auto* concrete = dynamic_cast<const EnemyGiantSlime*>(&enemy);
            return concrete && (concrete->IsHookSplitPulled() || concrete->HasSplit())
                ? concrete->GetDebugHookSplitPhaseName() : nullptr;
        };
        // 巨大スライムは通常攻撃に加え、接地時と空中で拘束された時のフック分裂を個別に確認できる。
        giant.options = {
            {"hook_split_ground", "フック分裂（接地から抵抗）", 8.0f, [] { return 3.5f; }},
            {"hook_split_air", "フック分裂（空中拘束から着地）", 8.0f, [] { return 4.2f; }}
        };
        giant.prepare = [](BaseEnemy& enemy, const EnemyAttackPreviewContext& context) {
            auto* concrete = dynamic_cast<EnemyGiantSlime*>(&enemy);
            // 巨大スライムの実体と標的があり、地上版または空中版の分裂を選んだ場合だけ、引き合いを準備する。
            if (!concrete || !context.target ||
                (context.optionId != "hook_split_ground" && context.optionId != "hook_split_air")) return;
            // 空中版だけ標的との引き合いを始める前に高度と下降速度を設定する。
            if (context.optionId == "hook_split_air") {
                enemy.SetTranslate(context.ground + engine::Vector3{0.0f, 5.5f, 0.0f});
                enemy.SetVelocity({0.0f, -1.5f, 0.0f});
                enemy.SetGrounded(false);
            }
            concrete->BeginHookSplitPull(context.target->GetWorldPosition());
        };
        // 指定した分裂オプションに限り、本番と同じ引っ張りの更新を固定刻みごとに呼ぶ。
        giant.updateInteraction = [](BaseEnemy& enemy, float deltaTime, const EnemyAttackPreviewContext& context) {
            auto* concrete = dynamic_cast<EnemyGiantSlime*>(&enemy);
            return concrete && context.target &&
                (context.optionId == "hook_split_ground" || context.optionId == "hook_split_air") &&
                concrete->UpdateHookSplitPull(deltaTime, context.target->GetWorldPosition(), context.particles);
        };
        entries.push_back(std::move(giant));

        auto prism = Make<EnemyPrismSlime>("PrismSlime", "プリズムスライム（無属性／炎／雷／風）",
            &EnemyPrismSlime::SetDebugPreviewAttackId, &EnemyPrismSlime::GetDebugAttackPhaseName);
        prism.damageType = DamageType::kExplosion;
        prism.targetDistance = 12.0f;
        prism.duration = 4.2f;
        prism.canAuthor = true;
        prism.sequenceHelp = "この敵では炎属性の攻撃枠で実行します。命令の時刻0は詠唱完了時です。複製した攻撃も同じ詠唱とAIを使用します。";
        // プリズムは炎攻撃の既定IDか、既にシーケンスを持つ技だけ編集対象にする。
        prism.canEditSequence = [](const EnemyAttackDefinition* attack, bool playerCopy) {
            return !playerCopy && attack && (attack->id == "fire_fan" ||
                !attack->sequence.events.empty() || !attack->sequence.tracks.empty());
        };
        // 発動まで進めず詠唱だけを見る設定を、敵のプレビュー用状態に渡す。
        prism.prepare = [](BaseEnemy& enemy, const EnemyAttackPreviewContext& context) {
            // プリズムスライムの実体を取得できた場合だけ、詠唱だけで止める設定を適用する。
            if (auto* concrete = dynamic_cast<EnemyPrismSlime*>(&enemy))
                concrete->SetDebugPreviewChantOnly(context.preparationOnly);
        };
        prism.connectSpawns = ConnectSpawns<EnemyPrismSlime>;
        entries.push_back(std::move(prism));

        entries.push_back(Make<EnemyMagmaSlime>("MagmaSlime", "マグマスライム（三連砲／突進／火山プレス）",
            &EnemyMagmaSlime::SetDebugPreviewAttackId, &EnemyMagmaSlime::GetDebugAttackPhaseName));
        auto bomber = Make<EnemyBomber>("Bomber", "ボマースライム（爆弾投げ）");
        // 投げたボムも隔離プレビューと終了時の片付けに含める。
        bomber.connectSpawns = ConnectSpawns<EnemyBomber>;
        entries.push_back(std::move(bomber));
        entries.push_back(Make<EnemyWindSlime>("WindSlime", "風スライム（暴風ブレス／空中三連風弾）",
            &EnemyWindSlime::SetDebugPreviewAttackId, &EnemyWindSlime::GetDebugAttackPhaseName));

        auto king = Make<EnemyFalseKingSlime>("FalseKingSlime", "偽王スライム（最終ボス・3形態）",
            &EnemyFalseKingSlime::SetDebugPreviewAttackId, &EnemyFalseKingSlime::GetDebugAttackPhaseName);
        king.damageType = DamageType::kExplosion;
        // 封印失敗の演出は通常攻撃とは別の確認項目にし、尺は現在の調整値から求める。
        king.options = {{"seal_barrage_preview", "第2形態：封印失敗の黒晶墜槍", 15.0f, [] {
            const auto& tuning = CrownFinaleTuning::Get();
            return 1.25f + tuning.sealCastTime + tuning.sealBarrageMovieDuration + tuning.sealBarrageRecovery;
        }, true}};
        entries.push_back(std::move(king));
        // すべての敵を一括登録した結果は、直後のassertで確認する。
        return EnemyAttackPreviewRegistry::Register(std::move(entries));
    }();
    assert(kRegistered && "attack preview registration failed");
    (void)kRegistered;
}
