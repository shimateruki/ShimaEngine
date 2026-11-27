#include <cstdint>
#include "game/system/rules/GameRule.h"
#include "game/system/rules/EventManager.h"
#include "DebugConsole.h"
#include "BaseScene.h"
#include "game/system/scene/SceneGameplayAccess.h"
#include "Event.h"
#include "Player.h"
#include "game/scene/gameplay/GamePlayScene.h"
#include "game/system/progress/IStageProgressScene.h"
#include "game/system/progress/GameDataManager.h"
#include "game/system/progress/StarCoinPresentation.h"
#include "GpuParticleManager.h"
#include "VfxSequencer.h"
#include "game/system/combat/HitEffectDirector.h"
#include "BaseEnemy.h"
#include "game/actor/enemy/boss/false_king/EnemyFalseKingSlime.h"
#include "Math.h"
#include <game/actor/player/state/PlayerState.h>
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace {
constexpr const char* kCheckpointLoweredModel = "Gimmicks/checkpoint_flag";
constexpr const char* kCheckpointRaisedModel = "Gimmicks/checkpoint_beacon_active";
constexpr float kCheckpointFlagRaiseDelay = 0.14f;
constexpr float kPinkGuardDamageRate = 0.5f;
constexpr float kPinkGuardKnockbackRate = 0.3f;

// 物理属性で攻撃者が分かるイベントは、旧攻撃との互換のため敵タイプから属性を補う。
DamageType ResolveDamageType(const DamageEvent& event) {
    // 物理以外の属性を指定済みか攻撃者が不明の場合は、イベントの属性をそのまま使う。
    if (event.damageType != DamageType::kPhysical || !event.attacker) {
        return event.damageType;
    }

    const std::string& enemyType = event.attacker->GetEnemyType();

    // 旧形式で炎スライムの属性が未指定なら、炎の耐性や演出へ渡す。
    if (enemyType == "FireSlime") {
        return DamageType::kFire;
    }

    // 旧形式で雷スライムなら、感電の専用処理に使う雷属性を補う。
    if (enemyType == "ThunderSlime") {
        return DamageType::kElectric;
    }

    // 旧形式で爆弾なら、範囲ダメージと同じ爆発属性を補う。
    if (enemyType == "Bomb") {
        return DamageType::kExplosion;
    }

    return DamageType::kPhysical;
}
} // namespace

// 旗が属するシーンを借用し、Activate()で報酬と復帰、Restore()で保存済みの表示、Update()で演出を扱う。
class CheckpointRule {
  public:
    // 前のシーンの保存用キーと遅延演出を破棄し、新しいシーンへ付け替える。
    void Initialize(BaseScene* scene) {
        scene_ = scene;
        activeCheckpoint_ = nullptr;
        rewardedCheckpoints_.clear();
        pendingFlagRaises_.clear();
        checkpointBaseEmissive_ = 1.0f;
        checkpointPresentationTimer_ = 0.0f;
        checkpointParticleTimer_ = 0.0f;
        checkpointAmbientRingTimer_ = 0.0f;
    }

    void Activate(Player* player, engine::Object3D* checkpoint);
    void Restore(const std::string& checkpointObjectName);
    void Update(float deltaTime);

  private:
    BaseScene* scene_ = nullptr;
    engine::Object3D* activeCheckpoint_ = nullptr;
    // 取得済みの判定には永続GUIDを優先する。旧配置でGUIDが空なら名前で照合する。
    static const std::string& CheckpointKey(const engine::Object3D& checkpoint) {
        const auto& guid = checkpoint.GetPersistentGuid();
        return guid.empty() ? checkpoint.GetName() : guid;
    }

    /// <summary>接触後の待機が終わってから旗を上げるため、対象のチェックポイントと残り秒数を保持する。</summary>
    struct PendingFlagRaise {
        engine::Object3D* checkpoint;
        float secondsRemaining;
    };

    // 旗の永続GUIDを記録し、演出中に再接触しても報酬を重ねない。
    std::unordered_set<std::string> rewardedCheckpoints_;
    std::vector<PendingFlagRaise> pendingFlagRaises_;
    float checkpointBaseEmissive_ = 1.0f;
    float checkpointPresentationTimer_ = 0.0f;
    float checkpointParticleTimer_ = 0.0f;
    float checkpointAmbientRingTimer_ = 0.0f;
};


GameRule::GameRule() = default;

// インスタンスを参照するイベントコールバックを、破棄前にすべて解除する。
GameRule::~GameRule() {
    for (auto& id : eventSubscriptions_) {
        EventManager::GetInstance()->Unsubscribe(id);
        id = EventManager::kInvalidSubscriptionId;
    }
}

// 前Sceneのイベント購読を解除し、現在Sceneの接触・被弾・死亡イベントへ登録し直します。
void GameRule::Initialize(BaseScene* scene, bool editorOnly) {
    for (auto& id : eventSubscriptions_) {
        EventManager::GetInstance()->Unsubscribe(id);
        id = EventManager::kInvalidSubscriptionId;
    }

    // 前シーンの継続効果を捨て、チェックポイント処理の参照を新しいシーンへ付け替えます。
    scene_ = scene;
    editorOnly_ = editorOnly;
    activeStatusEffects_.clear();
    // 旗の規則をまだ生成していなければ作成し、その後で現在のシーンに初期化する。
    if (!checkpointRule_)
        checkpointRule_ = std::make_unique<CheckpointRule>();
    checkpointRule_->Initialize(scene);

    // 新しいシーンに対する三つのイベント購読を登録し、破棄・再初期化で解除できるようIDを保持する。
    auto* events = EventManager::GetInstance();
    eventSubscriptions_[0] = events->Subscribe(
        [this](const PlayerHitEvent& event) { OnPlayerContact(event); });
    eventSubscriptions_[1] = events->Subscribe(
        [this](const DamageEvent& event) { OnDamage(event); });
    eventSubscriptions_[2] = events->Subscribe(
        [this](const PlayerDeathEvent& event) { OnPlayerDeath(event); });
}

// 接触イベントでは個別のEventTypeを優先し、汎用TargetIDはその後に使う。
void GameRule::OnPlayerContact(const PlayerHitEvent& event) {
    engine::Object3D* whoHit = event.me;           // プレイヤー
    engine::Object3D* objectHit = event.hitObject; // 当たった物

    // 実戦以外の接触や参照のないイベントから、ワープや進行通知を起こさないようにします。
    if (editorOnly_ || !whoHit || !objectHit || whoHit->IsEditorInternal() || objectHit->IsEditorInternal())
        return;

    EventType type = objectHit->GetEventType();

    switch (type) {
    // ワープ先は接触物の配置位置ではなく、TargetIDが指す出口から取得します。
    case EventType::kWarp: {
        std::int32_t targetId = objectHit->GetTargetId();
        // ワープ先を探すには現行シーンと有効なTargetIDの両方が必要になる。
        if (scene_ && targetId != -1) {
            engine::Object3D* exitPoint = scene_->FindObjectByEventId(targetId);
            // そのIDに対応する出口が見つかった場合だけ、触れたプレイヤーを出口へ移す。
            if (exitPoint) {
                engine::Vector3 destPos = exitPoint->GetTransform()->translate;
                whoHit->SetTranslate(destPos);
                DebugConsole::GetInstance()->AddLog("Warped to ID: " + std::to_string(targetId));
            }
        }
    } break;

    // ダメージ接触は共通のHP処理へ固定ダメージを渡す。
    case EventType::kDamage:
        ApplyDamage(whoHit, 10.0f);
        break;

    // 橋の落下はゲーム用シーンだけが扱うため、実体を確認してから開始します。
    case EventType::kMovieBridge:
        // シーンを取得できない状態では、橋の演出を起動できない。
        if (scene_) {
            // ゲームプレイ用シーンである場合に限り、橋を落とす演出を依頼する。
            if (GamePlayScene* gps = dynamic_cast<GamePlayScene*>(scene_)) {
                gps->StartBridgeDropMovie();
            }
        }
        break;

    // チェックポイントに触れたプレイヤーを、旗の取得と復帰位置の処理へ渡す。
    case EventType::kCheckpoint: {
        // 接触元がPlayerなら、復帰地点として旗を有効にする。
        if (Player* player = dynamic_cast<Player*>(whoHit)) {
            checkpointRule_->Activate(player, objectHit);
        }
    } break;

    // ゴールを受け付けるかどうかは、現在のシーンの進行条件に委ねます。
    case EventType::kGoal: {
        // 進行機能を持つシーンに限り、プレイヤーとゴールの接触を渡す。
        if (auto* progress = dynamic_cast<IStageProgressScene*>(scene_)) {
            // シーン側が接触を拒否した場合は、ゴールの分岐を終了する。
            if (!progress->TryStartGoalFromContact(whoHit, objectHit))
                break;
        }
    } break;

    // スターコインは現在のシーンに取得を通知し、取得後は当たり判定を解除する。
    case EventType::kStarCoin: {
        // 非表示か取得アニメーション中の場合は、同じコインの取得をもう一度開始しない。
        if (!objectHit->GetIsVisible() || objectHit->IsCollecting())
            break;
        const std::int32_t coinIdx = objectHit->GetTargetId();

        // ステージ進行が取得を扱える場合だけ、収集の記録と見た目の更新を行う。
        if (auto* progress = dynamic_cast<IStageProgressScene*>(scene_)) {
            progress->CollectStarCoinFromContact(coinIdx, objectHit->GetWorldPosition());
            objectHit->StartCollectionAnimation();
            objectHit->SetCollisionAttribute(0);
            objectHit->SetCollisionMask(0);
            objectHit->SetGpuParticleName("");
            const char* burst = starCoinPresentation::IsSilver(*objectHit)
                ? starCoinPresentation::kSilverGet : "shared/star_sparkleGet";
            GpuParticleManager::GetInstance()->Emit(burst, objectHit->GetWorldPosition());
        }
    } break;

    // 個別イベントを持たない接触では、汎用のTargetIDを起動に使えるか調べる。
    case EventType::kNone:
    default:
        // 敵のTargetIDは撃破完了に使うため、プレイヤーが敵へ接触しただけで汎用起動しない。
        if (dynamic_cast<BaseEnemy*>(objectHit)) {
            break;
        }
        // マグマ連動床のTarget IDは駆動元の参照であり、接触スイッチではない。
        // 着地で噴出口を反転すると、乗った瞬間に上昇が停止してしまう。
        if (objectHit->GetGimmickType() == "MovingFloor" && objectHit->GetParameters() &&
            objectHit->GetParameters()->actionMode == 5) {
            break;
        }
        // 回転棘のTarget IDは追従する床の参照であり、接触イベントではない。
        if (objectHit->GetGimmickType() == "RotatingFloor" && objectHit->GetTargetId() >= 0) {
            break;
        }
        std::int32_t tId = objectHit->GetTargetId();
        // 汎用イベントを指すIDとシーンが両方揃った場合だけ、対象イベントを起動する。
        if (tId != -1 && scene_) {
            scene_->TriggerEvent(tId);
        }
        break;
    }

}

// ダメージが共通HP処理に受理されたら、属性に応じた表示・状態異常・移動反応を処理する。
void GameRule::OnDamage(const DamageEvent& event) {
    // 実戦とエディター内部の対象を分け、異なる区画をまたぐ攻撃は無視します。
    if (!event.target || event.target->IsEditorInternal() != editorOnly_ ||
        (event.attacker && event.attacker->IsEditorInternal() != event.target->IsEditorInternal()))
        return;
    Player* playerTarget = dynamic_cast<Player*>(event.target);
    // 演出中に操作が固定されたプレイヤーには、被弾処理を開始しない。
    if (playerTarget && playerTarget->IsCinematicLocked()) {
        return;
    }
    const DamageType damageType = ResolveDamageType(event);
    const bool isElectricPlayerHit = playerTarget && damageType == DamageType::kElectric;

    float finalDamage = event.damageAmount;
    // 攻撃者のパラメーターがある場合は、その攻撃力を非負の倍率として掛ける。
    if (event.attacker && event.attacker->GetParameters().has_value()) {
        finalDamage *= (std::max)(0.0f, event.attacker->GetParameters()->attackPower);
    }

    // 即死指定は通常倍率の計算後に適用し、攻撃力0でもHPを確実に0へ落とします。
    if (event.lethalPlayerDamage && playerTarget)
        finalDamage = (std::max)(0.0f, playerTarget->GetHp());

    // ピンクガードは正面の通常攻撃だけを軽減する。即死・強制攻撃は対象外。
    const bool guarded = playerTarget && event.attacker && !event.lethalPlayerDamage &&
                         !event.bypassPlayerInvincibility &&
                         playerTarget->CanGuardAttackFrom(event.attacker->GetWorldPosition());
    // 正面ガードの条件が成立したため、この一撃のHP減少量を抑える。
    if (guarded) {
        finalDamage *= kPinkGuardDamageRate;
    }

    // 減算前のHPを控え、共通処理で拒否された命中は即座に終了する。HPが減ったかは後で比較する。
    const auto& before = event.target->GetParameters();
    const float previousHp = before ? before->hp : 0.0f;
    const bool damageApplied = ApplyDamage(event.target, finalDamage, event.bypassPlayerInvincibility);
    // 無敵などで共通ダメージ処理に通らなければ、反応や状態異常を通知しない。
    if (!damageApplied) {
        return;
    }

    // ガード成功なら、通常の被弾演出の代わりにガードの反応を知らせる。
    if (guarded) {
        playerTarget->NotifyGuardedHit(event.attacker->GetWorldPosition());
    // 通常の命中では、攻撃属性とノックバックから被弾エフェクトを出す。
    } else {
        HitEffectDirector::SpawnDamageEventHit(event.target, event.attacker, event.knockbackVelocity, damageType);
    }

    const auto& after = event.target->GetParameters();
    const bool hpDecreased = after && after->hp < previousHp;
    // 対象が敵で、HPが減ったことを確認できたときだけ、敵自身の被弾モーションを再生する。
    if (auto* enemyTarget = dynamic_cast<BaseEnemy*>(event.target); enemyTarget && hpDecreased) {
        enemyTarget->PlayDamageReaction(event.attacker, event.knockbackVelocity, event.damageAmount);
    }

    // 有効な状態異常を伴う命中なら、最終ダメージ量とは独立した攻撃力倍率を継続効果へ渡す。
    if (event.statusEffect.IsValid()) {
        ApplyStatusEffect(event.target, event.statusEffect,
                          event.attacker && event.attacker->GetParameters().has_value()
                              ? (std::max)(0.0f, event.attacker->GetParameters()->attackPower)
                              : 1.0f);
    }

    // 感電は専用の反応へ任せ、通常の被弾反応と二重に開始しないようにします。
    if (isElectricPlayerHit) {
        playerTarget->StartElectricShockFeedback(0.78f, 1.0f);
    // 感電以外の命中でガードしていないプレイヤーには、通常の被弾反応を検討する。
    } else if (playerTarget && !guarded) {
        const bool hasKnockback = std::abs(event.knockbackVelocity.x) > 0.001f ||
                                  std::abs(event.knockbackVelocity.y) > 0.001f ||
                                  std::abs(event.knockbackVelocity.z) > 0.001f;
        // 吹き飛ばしが指定されたか物理以外の攻撃なら、被弾反応に渡す向きを作る。
        if (hasKnockback || damageType != DamageType::kPhysical) {
            engine::Vector3 reactionDirection = event.knockbackVelocity;
            reactionDirection.y = 0.0f;
            // 水平方向がほぼゼロで攻撃者が存在する場合は、攻撃者から被弾者への方向を使う。
            if (engine::Math::Length(reactionDirection) <= 0.001f && event.attacker) {
                reactionDirection = event.target->GetWorldPosition() - event.attacker->GetWorldPosition();
                reactionDirection.y = 0.0f;
            }
            playerTarget->StartDamageFeedback(reactionDirection, (std::max)(0.0f, event.invincibilityDuration));
        }
    }

    // プレイヤーのHPが実際に減り、ガードに入らなかった場合は、通常の傷つき表情を出す。
    if (playerTarget && hpDecreased && !guarded) {
        playerTarget->StartHurtFace();
    }

    // 感電による専用移動制御を使わず、いずれかの軸に有効な吹き飛ばしがある場合は適用先を探す。
    if (!isElectricPlayerHit &&
        (std::abs(event.knockbackVelocity.x) > 0.001f || std::abs(event.knockbackVelocity.y) > 0.001f ||
         std::abs(event.knockbackVelocity.z) > 0.001f)) {
        // 対象がノックバックを扱えるCharacterなら、ガード時の倍率も反映して吹き飛ばす。
        if (Character* character = dynamic_cast<Character*>(event.target)) {
            // ガード成功では押し出しも弱める。感電時の移動制御は上で始めた専用反応に任せる。
            const float knockbackRate = guarded ? kPinkGuardKnockbackRate : 1.0f;
            character->SetVelocity(event.knockbackVelocity * knockbackRate);
        }
    }
}

// 実戦での死亡だけを進行中のセーブの残機に反映する。
void GameRule::OnPlayerDeath(const PlayerDeathEvent& event) {
    // 隔離プレビューでHPを0にしても、実セーブの残機は消費しません。
    if (!event.player || event.player->IsEditorInternal())
        return;
    GameDataManager::GetInstance()->SubtractLife();
    std::int32_t lives = GameDataManager::GetInstance()->GetLives();

    DebugConsole::GetInstance()->AddLog("Player Died! Remaining Lives: " + std::to_string(lives));
}

// チェックポイント演出と継続効果を進め、死亡・消失した対象を解除します。
// 1フレームの継続ダメージ回数には上限を置きます。
void GameRule::Update(float deltaTime) {
    // 未初期化や停止フレームでは、チェックポイントと継続効果の時間を進めない。
    if (!scene_ || deltaTime <= 0.0f) {
        return;
    }

    checkpointRule_->Update(deltaTime);

    for (auto it = activeStatusEffects_.begin(); it != activeStatusEffects_.end();) {
        ActiveStatusEffect& status = *it;

        // 対象が消えていたら効果を破棄し、後続のダメージや描画で参照しない。
        if (!IsStatusTargetAlive(status.target)) {
            it = activeStatusEffects_.erase(it);
            continue;
        }

        // 効果の寿命、ダメージ間隔、粒子間隔は別々のタイマーで進める。
        status.remainingTime -= deltaTime;
        status.tickTimer -= deltaTime;
        status.visualTimer -= deltaTime;

        // 状態異常の見た目の時刻が来てプリセットも設定済みなら、対象に演出を出す。
        if (status.visualTimer <= 0.0f && !status.vfxPreset.empty()) {
            EmitStatusVisual(status.target, status.vfxPreset);
            status.visualTimer += 0.12f;
        }

        // 大きな deltaTime でも、一度の更新で適用する継続ダメージを最大4回に制限する。
        std::int32_t processedTicks = 0;

        // 経過時間が複数の発生点を跨いだ場合は、効果の期限内の未処理分を順に消化する。
        while (status.tickTimer <= 0.0f && status.remainingTime > 0.0f && processedTicks < 4) {
            ApplyDamage(status.target, status.tickDamage);
            status.tickTimer += status.tickInterval;
            ++processedTicks;
        }

        // 時間切れに加え、今回の継続ダメージで倒れた対象の効果もこの更新で取り除く。
        if (status.remainingTime <= 0.0f || !IsStatusTargetAlive(status.target)) {
            it = activeStatusEffects_.erase(it);
        // 期限内で対象も生存している効果は保持し、次の効果を調べる。
        } else {
            ++it;
        }
    }
}

// 触れた旗を復帰地点にし、実プレイの初回取得で残機とHPを補充する。
// 取得後のモデル切替と発光演出もここで開始します。
void CheckpointRule::Activate(Player* player, engine::Object3D* checkpoint) {
    // プレイヤーか旗の参照が空なら、復帰地点を更新できない。
    if (!player || !checkpoint) {
        return;
    }

    // 同じ旗を続けて踏んだ場合は、復帰の設定や取得演出を繰り返さない。
    if (activeCheckpoint_ == checkpoint) {
        return;
    }

    // 別の旗へ切り替えると復帰地点を更新します。報酬は、未取得の旗を初めて有効化した実プレイヤーだけに与えます。
    const bool firstActivation = checkpoint->GetModelName() == kCheckpointLoweredModel &&
        rewardedCheckpoints_.insert(CheckpointKey(*checkpoint)).second;
    player->ActivateCheckpoint(checkpoint->GetWorldPosition() + engine::Vector3{0.0f, 0.35f, 0.0f}, checkpoint->GetName());

    // 実プレイでその旗を初めて取得した場合だけ、残機を加算してHPを全回復する。
    if (firstActivation && !player->IsEditorInternal()) {
        GameDataManager::GetInstance()->AwardCheckpointLife();
        player->RestoreHpToFull();
    }

    // 切替前の旗が残っていれば元の発光へ戻し、新しい旗だけ取得演出を始めます。
    if (activeCheckpoint_ && scene_ && scene_->IsAlive(activeCheckpoint_)) {
        activeCheckpoint_->SetEmissive(checkpointBaseEmissive_);
    }

    activeCheckpoint_ = checkpoint;
    checkpointBaseEmissive_ = checkpoint->GetEmissive();
    checkpointPresentationTimer_ = 0.0f;

    // 初取得の旗だけ、獲得演出の時刻に合わせてモデルの切り替えを予約する。
    if (firstActivation) {
        pendingFlagRaises_.push_back({checkpoint, kCheckpointFlagRaiseDelay});
    }

    // 色は保持し、取得演出を始める時点で発光が下限を割らないようにする。
    checkpoint->SetEmissive((std::max)(0.28f, checkpointBaseEmissive_));
    VfxSequencer::PlayOneShot("shared/checkpoint_activate_cue", checkpoint->GetWorldPosition(), checkpoint->GetScale(),
                              checkpoint->GetRotation());
    DebugConsole::GetInstance()->AddLog("Checkpoint activated. Respawn and copy memory updated.");
}

void GameRule::RestoreCheckpointPresentation(const std::string& checkpointObjectName) {
    // 旗の規則が初期化されている場合に限り、保存済みの旗を復元する。
    if (checkpointRule_)
        checkpointRule_->Restore(checkpointObjectName);
}

// セーブ済みの旗をSceneから探し、取得済みモデルと待機アニメーションへ復元します。
void CheckpointRule::Restore(const std::string& checkpointObjectName) {
    // シーンまたは保存された旗名がなければ、取得済みの旗を探せない。
    if (!scene_ || checkpointObjectName.empty())
        return;

    for (const auto& owned : scene_->GetObjects()) {
        engine::Object3D* checkpoint = owned.get();
        // 保存名と一致する実体だけ、旗の種類を判定する。
        if (!checkpoint || checkpoint->GetName() != checkpointObjectName)
            continue;

        const std::string modelName = checkpoint->GetModelName();
        // 見つかった物体が復元対象の旗モデルでなければ、その物体を変更しない。
        if (modelName != kCheckpointLoweredModel && modelName != kCheckpointRaisedModel)
            return;

        // SetModelによるコライダー自動調整を戻し、取得済みの待機モーションから再開する。
        const engine::Object3D::ColliderConfig collider = checkpoint->GetColliderConfig();
        // 未取得のモデルで配置されていた場合だけ、取得済みモデルへ差し替える。
        if (modelName == kCheckpointLoweredModel)
            checkpoint->SetModel(kCheckpointRaisedModel);
        checkpoint->RestoreAnimationPlayback("Idle", 0.0f, true);
        checkpoint->SetColliderConfig(collider);

        // すでに取得済みとして記録し、遅延中の旗上げを消して演出の途中から再開しないようにします。
        activeCheckpoint_ = checkpoint;
        rewardedCheckpoints_.insert(CheckpointKey(*checkpoint));
        std::erase_if(pendingFlagRaises_, [checkpoint](const PendingFlagRaise& pending) {
            return pending.checkpoint == checkpoint;
        });
        checkpointBaseEmissive_ = checkpoint->GetEmissive();
        checkpointPresentationTimer_ = kCheckpointFlagRaiseDelay + 0.60f;
        checkpoint->SetEmissive((std::max)(0.28f, checkpointBaseEmissive_));
        return;
    }
}

void CheckpointRule::Update(float deltaTime) {
    // 未到達の旗も案内として光らせます。有限寿命の演出を間隔を空けて再生します。
    checkpointParticleTimer_ -= deltaTime;
    checkpointAmbientRingTimer_ -= deltaTime;

    // シーンを取得でき、粒子か輪の時刻が来た場合に限り、表示中の旗を探す。
    if (scene_ && (checkpointParticleTimer_ <= 0.0f || checkpointAmbientRingTimer_ <= 0.0f)) {
        const bool emitMotes = checkpointParticleTimer_ <= 0.0f;
        const bool emitRing = checkpointAmbientRingTimer_ <= 0.0f;

        for (const auto& owned : scene_->GetObjects()) {
            engine::Object3D* checkpoint = owned.get();

            // 実体がない旗や非表示の物体には、待機用の演出を出さない。
            if (!checkpoint || !checkpoint->GetIsVisible()) {
                continue;
            }

            const auto& modelName = checkpoint->GetModelName();
            // 旗の対応モデル三種類だけを、待機演出の発生対象にする。
            if (modelName != kCheckpointLoweredModel && modelName != kCheckpointRaisedModel &&
                modelName != "Gimmicks/checkpoint_flag_active") {
                continue;
            }

            // 小粒子の発生時刻が来た場合は、GPU粒子を使用できるか確認する。
            if (emitMotes) {
                // GPU粒子が初期化済みの場合は、旗のローカル位置をワールドに変換して発生させる。
                if (auto* particles = GpuParticleManager::GetInstance(); particles && particles->IsInitialized()) {
                    particles->Emit("shared/checkpoint_active_motes",
                                    engine::Math::Transform(engine::Vector3{0.0f, 0.55f, 0.0f}, checkpoint->GetWorldMatrix()));
                }
            }

            // 輪の発生時刻が来た場合は、旗の位置・回転・大きさを使って待機演出を出す。
            if (emitRing) {
                VfxSequencer::PlayOneShot("shared/checkpoint_idle_cue", checkpoint->GetWorldPosition(), checkpoint->GetScale(),
                                          checkpoint->GetRotation());
            }
        }

        // 小粒子を処理したフレームでは、その発生間隔を数え直す。
        if (emitMotes)
            checkpointParticleTimer_ = 0.45f;
        // 輪を処理したフレームでは、輪の発生間隔だけを数え直す。
        if (emitRing)
            checkpointAmbientRingTimer_ = 1.35f;
    }

    // 演出のタイミングに合わせてモデルを切り替え、削除済みの旗や処理済みの予約はリストから外します。
    std::erase_if(pendingFlagRaises_, [this, deltaTime](PendingFlagRaise& pending) {
        auto* checkpoint = pending.checkpoint;
        // 旗がシーンから消えていれば、参照を使わずモデル切替の予約を破棄する。
        if (!scene_ || !scene_->IsAlive(checkpoint)) {
            return true;
        }

        pending.secondsRemaining -= deltaTime;
        // 待機時間が残っている予約は、次のフレームまで保持する。
        if (pending.secondsRemaining > 0.0f) {
            return false;
        }

        // SetModelによるコライダーの自動調整を元に戻す。
        const auto collider = checkpoint->GetColliderConfig();
        checkpoint->SetModel(kCheckpointRaisedModel);
        checkpoint->RestoreAnimationPlayback(checkpoint == activeCheckpoint_ ? "Activate" : "Idle",
            0.0f, checkpoint != activeCheckpoint_);
        checkpoint->SetColliderConfig(collider);
        return true;
    });

    // 現在の復帰地点に旗が設定されていなければ、個別の発光・取得モーションは更新しない。
    if (!activeCheckpoint_) {
        return;
    }

    // 有効な旗が消えていれば、次回も参照しないよう現在の旗を解除する。
    if (!scene_ || !scene_->IsAlive(activeCheckpoint_)) {
        activeCheckpoint_ = nullptr;
        return;
    }

    // 取得演出の終了時刻をまたぐ一回だけ待機モーションへ切り替えます。
    const float previousTime = checkpointPresentationTimer_;
    checkpointPresentationTimer_ += deltaTime;
    // 取得時のモーションが終わる時刻をまたぎ、取得済みモデルになっている場合だけ待機へ移す。
    if (previousTime < kCheckpointFlagRaiseDelay + 0.60f &&
        checkpointPresentationTimer_ >= kCheckpointFlagRaiseDelay + 0.60f &&
        activeCheckpoint_->GetModelName() == kCheckpointRaisedModel) {
        activeCheckpoint_->RestoreAnimationPlayback("Idle", 0.0f, true);
    }

    // 取得からの秒数で明滅の下限を計算する。元の発光の方が強い場合はそちらを保持する。
    const float emissivePulse = 0.12f + std::sin(checkpointPresentationTimer_ * 2.0f) * 0.025f;
    activeCheckpoint_->SetEmissive((std::max)(checkpointBaseEmissive_, emissivePulse));
}

// 同じ対象・同じ種類の状態異常は重ねず、残り時間と威力を弱めないよう更新します。
void GameRule::ApplyStatusEffect(engine::Object3D* target, const StatusEffectApplication& application, float damageScale) {
    // 対象が見つからないか効果の指定が無効なら、継続効果を登録しない。
    if (!target || !application.IsValid()) {
        return;
    }

    // tickIntervalは秒単位。繰り返し間隔を最低0.05秒にし、威力は負なら0にする。
    const float interval = (std::max)(0.05f, application.tickInterval);
    const float tickDamage = (std::max)(0.0f, application.tickDamage * damageScale);
    const auto existing = std::find_if(activeStatusEffects_.begin(), activeStatusEffects_.end(),
                                       [target, &application](const ActiveStatusEffect& status) {
                                           return status.target == target && status.type == application.type;
                                       });

    // 同じ対象に同種の効果があれば、既存の効果の時刻と威力を弱めないよう更新する。
    // 同じ対象に同じ種類があれば新規登録せず、残り時間と威力は強い方を残す。発生間隔は今回の値に差し替える。
    if (existing != activeStatusEffects_.end()) {
        existing->remainingTime = (std::max)(existing->remainingTime, application.duration);
        existing->tickInterval = interval;
        // 再付与で次の発生時刻を先送りしない。新しい間隔が短ければ次回を早める。
        existing->tickTimer = (std::min)(existing->tickTimer, interval);
        existing->tickDamage = (std::max)(existing->tickDamage, tickDamage);

        // 新しい発生プリセットが指定されている場合は、継続中の見た目も差し替える。
        if (!application.vfxPreset.empty()) {
            existing->vfxPreset = application.vfxPreset;
        }

        return;
    }

    // 新規の継続効果は初回ダメージまで待ちます。表示だけはすぐ出せるよう時計をゼロにします。
    ActiveStatusEffect status;
    status.target = target;
    status.type = application.type;
    status.remainingTime = application.duration;
    status.tickTimer = interval;
    status.tickInterval = interval;
    status.tickDamage = tickDamage;
    status.visualTimer = 0.0f;
    status.vfxPreset = application.vfxPreset;
    activeStatusEffects_.push_back(std::move(status));
}

bool GameRule::IsStatusTargetAlive(engine::Object3D* target) const {
    // 対象またはシーンを取得できない場合は、継続効果の有効対象として扱わない。
    if (!target || !scene_) {
        return false;
    }

    // シーン専用のプレイヤー以外は、シーンの生存リストにいることを確かめる。
    if (target != GetScenePlayer(scene_) && !scene_->IsAlive(target)) {
        return false;
    }

    // 死亡か非表示の対象は、継続ダメージや粒子を送る対象から外す。
    if (target->IsDead() || !target->GetIsVisible()) {
        return false;
    }

    return !target->GetParameters().has_value() || target->GetParameters()->hp > 0.0f;
}

void GameRule::EmitStatusVisual(engine::Object3D* target, const std::string& presetName) const {
    // 身体の高さから中心を求め、サイズの異なる敵でも状態異常が体に重なるようにする。
    GpuParticleManager* particles = GpuParticleManager::GetInstance();

    // 対象と初期化済みのGPU粒子が揃った場合だけ、演出の座標を計算する。
    if (!target || !particles || !particles->IsInitialized()) {
        return;
    }

    const engine::Aabb aabb = target->GetAabb();
    engine::Vector3 position = target->GetWorldPosition();
    const float height = (std::max)(0.25f, aabb.max.y - aabb.min.y);
    position.y = aabb.min.y + height * 0.48f;
    particles->EmitDirected(presetName, position, {0.0f, 1.0f, 0.0f}, 1.0f);
}

// ボス固有の軽減とプレイヤーの無敵判定を通した後、共通 HP を更新します。
bool GameRule::ApplyDamage(engine::Object3D* target, float damage, bool bypassPlayerInvincibility) {
    // 有効な対象がなければ、HPを変更できないので失敗を返す。
    if (!target) {
        return false;
    }

    // 偽王は形態や障壁に応じてダメージを制限し、減算できない場合はここで止めます。
    if (auto* king = dynamic_cast<EnemyFalseKingSlime*>(target)) {
        damage = king->FilterFinaleDamage(damage);
        // 偽王の特殊ルールで受ける量が0以下になった場合は、共通のHP減算を行わない。
        if (damage <= 0.0f)
            return false;
    }

    // 保存用の分類名はEditorで変更できるため、実体の型でプレイヤーを判定する。
    auto* player = dynamic_cast<Player*>(target);
    // 無敵を無視する指定がない無敵中のプレイヤーと、演出で固定中のプレイヤーにはダメージを通さない。
    if (player && ((player->IsInvincible() && !bypassPlayerInvincibility) || player->IsCinematicLocked())) {
        return false;
    }

    // HPのパラメーターがある対象だけ、値の有効範囲を整えてからダメージを引く。
    if (target->GetParameters().has_value()) {
        target->SetParameter(&engine::EntityParameter::maxHp, (std::max)(target->GetParameters()->maxHp, 1.0f));
        target->SetParameter(&engine::EntityParameter::hp, (std::max)(target->GetParameters()->hp, 0.0f));

        // 読み込んだHPが最大値より高い場合は、ダメージ適用前のHPを収めるため最大値を引き上げる。
        if (target->GetParameters()->hp > target->GetParameters()->maxHp) {
            target->SetParameter(&engine::EntityParameter::maxHp, target->GetParameters()->hp);
        }

        const float previousHp = target->GetParameters()->hp;
        const float remainingHp = std::clamp(previousHp - damage, 0.0f, target->GetParameters()->maxHp);
        target->SetParameter(&engine::EntityParameter::hp, remainingHp);

        // 被弾中はノックバックを優先するが、生存していれば操作復帰後にパネル走行を再開する。
        if (player && remainingHp <= 0.0f) {
            player->CancelDashPanelBoost();
        }

        DebugConsole::GetInstance()->AddLog(target->GetName() + " に " + std::to_string(damage) +
                                            " のダメージ！ 残りHP: " + std::to_string(target->GetParameters()->hp));

        // HPが0以下に達した場合は、ダメージ量に加えて撃破もログへ記録する。
        if (target->GetParameters()->hp <= 0.0f) {
            DebugConsole::GetInstance()->AddLog(target->GetName() + " を撃破！");
        }

        // 有効なHPへ反映した場合は、HPが減らなかった命中も受理として返す。
        return true;
    }

    return false;
}
