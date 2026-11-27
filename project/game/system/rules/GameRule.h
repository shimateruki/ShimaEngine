#pragma once
#include <memory>
#include <array>
#include <string>
#include <vector>
#include "Object3D.h"
#include "Event.h"
#include "game/system/rules/EventManager.h"

class BaseScene;
class Player;
class CheckpointRule;

// 衝突イベントからダメージ、ゴール、クリア演出などのゲームルールを処理するクラス
// 関連：EventManagerを購読し、ダメージ・状態異常・チェックポイント・クリアを進行先へ接続する。
// 主な入口：Initialize / Update
class GameRule {
  public:
    /// <summary>ゲームルールの初期状態を作る。</summary>
    GameRule();

    /// <summary>接触・被弾・死亡イベントの購読を解除する。</summary>
    ~GameRule();

    GameRule(const GameRule&) = delete;

    GameRule& operator=(const GameRule&) = delete;

    /// <summary>監視対象シーンを借用し、接触・ダメージ・死亡イベントを購読する。</summary>
    /// <remarks>購読中は GameRule とシーンの両方が有効である必要がある。</remarks>
    // editorOnly は隔離プレビュー用。接触からの進行処理は止め、被弾はプレビュー内の対象だけに適用する。
    void Initialize(BaseScene* scene, bool editorOnly = false);

    /// <summary>チェックポイント演出と継続型状態異常を更新する。</summary>
    /// <param name="deltaTime">経過秒数。</param>
    void Update(float deltaTime);

    /// <summary>死亡後のシーン再生成で、旗を取得済みの姿へ戻す。</summary>
    void RestoreCheckpointPresentation(const std::string& checkpointObjectName);

  private:
    std::array<EventManager::SubscriptionId, 3> eventSubscriptions_{};

    // 接触先のイベント種別に応じて、ギミックや進行処理へ引き渡す。
    void OnPlayerContact(const PlayerHitEvent& event);

    // 攻撃の成立と威力を決め、HP・状態異常・被弾反応に反映する。
    void OnDamage(const DamageEvent& event);

    // 実プレイで死亡したプレイヤーの残機を減らす。
    void OnPlayerDeath(const PlayerDeathEvent& event);

    /// <summary>指定対象へダメージを適用し、HP減少と撃破を判定する。</summary>
    /// <returns>ダメージを適用できた場合はtrue。</returns>
    bool ApplyDamage(engine::Object3D* target, float damage, bool bypassPlayerInvincibility = false);

    /// <summary>ダメージに伴う状態異常を対象へ付与する。</summary>
    void ApplyStatusEffect(engine::Object3D* target, const StatusEffectApplication& application, float damageScale);

    /// <summary>対象が状態異常を受けられる生存状態か返す。</summary>
    bool IsStatusTargetAlive(engine::Object3D* target) const;

    /// <summary>指定対象の位置へ状態異常の演出を発生させる。</summary>
    void EmitStatusVisual(engine::Object3D* target, const std::string& presetName) const;

    // 状態異常の残り秒と継続ダメージ・演出の独立した時計。target はシーン内 Object の借用参照。
    struct ActiveStatusEffect {
        engine::Object3D* target = nullptr;
        StatusEffectType type = StatusEffectType::kNone;
        float remainingTime = 0.0f;
        float tickTimer = 0.0f;
        float tickInterval = 0.5f;
        float tickDamage = 0.0f;
        float visualTimer = 0.0f;
        std::string vfxPreset;
    };

    bool editorOnly_ = false;
    BaseScene* scene_ = nullptr;
    std::vector<ActiveStatusEffect> activeStatusEffects_;
    std::unique_ptr<CheckpointRule> checkpointRule_;
};
