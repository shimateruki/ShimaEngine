#define NOMINMAX
#include <cstdint>
#include "GimmickGameplayVolume.h"
#include "ObjectPropertyDefinition.h"

#include "BaseScene.h"
#include "CollisionConfig.h"
#include "game/system/settings/GameAudioSettings.h"
#include "game/system/progress/GameDataManager.h"
#include "Player.h"
#include "SceneManager.h"
#include "VfxSequencer.h"

#include <algorithm>
#include <cassert>
#include <cmath>

void GimmickGameplayVolume::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    // プレイヤーだけを検知する透明な Obb とし、用途は JSON の volumeMode で選ぶ。
    BaseGimmick::Initialize(common, modelName);

    SetClassName("Gimmick");
    SetGimmickType("GameplayVolume");
    SetName("Gimmick_GameplayVolume");
    SetStatic(false);
    SetCastShadow(false);
    SetColor({0.15f, 0.75f, 1.0f, 0.3f});
    SetBlendMode(BlendMode::kNormal);
    SetScale({3.0f, 3.0f, 3.0f});
    SetCollisionAttribute(engine::collisionCategories::Get("trigger"));
    SetCollisionMask(engine::collisionCategories::Get("player"));

    engine::Object3D::ColliderConfig collider;
    collider.type = engine::ColliderType::kObb;
    collider.center = {0.0f, 0.0f, 0.0f};
    collider.size = {1.0f, 1.0f, 1.0f};
    SetColliderConfig(collider);

    // パラメーターがなければ、用途と再発動の既定設定を保存できるように作成する。
    if (!GetParameters().has_value()) {
        ResetParameters();
    }

    SetParameter(&engine::EntityParameter::gimmickType, "GameplayVolume");
    SetParameter(&engine::EntityParameter::volumeMode, static_cast<std::int32_t>(Mode::kEvent));
    SetParameter(&engine::EntityParameter::volumePayload, {});
    SetParameter(&engine::EntityParameter::volumeTriggerOnce, true);
    SetParameter(&engine::EntityParameter::volumeTriggerOnExit, false);
    SetParameter(&engine::EntityParameter::volumeRearmDelay, 0.0f);
    SetParameter(&engine::EntityParameter::volumeBlendDuration, 0.75f);
    SetParameter(&engine::EntityParameter::startActive, true);
    SetParameter(&engine::EntityParameter::returnOnOff, false);

    // 用途別の入力欄を登録する。効果のIDは演出・BGM・環境で使い、環境の切替時間は環境専用にする。
    using Self = GimmickGameplayVolume;
    engine::ObjectPropertyDefinition<Self> properties(*this, kTypeId, "ゲームプレイボリューム",
        "イベント連携はTarget IDを接続します。チェックポイントは中心位置を記録、落下死は既存の残機処理へ接続します。退出時のOFFは対応モードだけ編集できます。");
    using Parameter = engine::Object3D::EntityParameter;
    properties.Parameter("処理", "volumeMode", &Parameter::volumeMode, 0.0f, 5.0f,
        nullptr, {"イベント連携", "チェックポイント", "落下死", "Feedback Cue", "BGM", "Environment Profile"});
    properties.Parameter("Cue / Audio / Profile ID", "volumePayload", &Parameter::volumePayload, 0.0f, 0.0f,
        +[](const Self& object) { return object.GetParameters() && (object.GetParameters()->volumeMode >= 3 && object.GetParameters()->volumeMode <= 5); });
    properties.Parameter("環境の切替時間", "volumeBlendDuration", &Parameter::volumeBlendDuration, 0.0f, 10.0f,
        +[](const Self& object) { return object.GetParameters() && (object.GetParameters()->volumeMode == 5); });
    properties.Parameter("一度だけ実行", "volumeTriggerOnce", &Parameter::volumeTriggerOnce);
    properties.Parameter("退出時に実行", "volumeTriggerOnExit", &Parameter::volumeTriggerOnExit);
    properties.Parameter("再実行まで", "volumeRearmDelay", &Parameter::volumeRearmDelay, 0.0f, 60.0f,
        +[](const Self& object) { return object.GetParameters() && (!object.GetParameters()->volumeTriggerOnce); });
    properties.Parameter("開始時に有効", "startActive", &Parameter::startActive);
    properties.Parameter("退出時にOFFへ戻す", "returnOnOff", &Parameter::returnOnOff, 0.0f, 0.0f,
        nullptr, {}, {.enabled = +[](const Self& object) { return object.GetParameters() && ((object.GetParameters()->volumeMode == 0 || object.GetParameters()->volumeMode == 4 || object.GetParameters()->volumeMode == 5) && !object.GetParameters()->volumeTriggerOnExit); }});
}

void GimmickGameplayVolume::Update(float deltaTime) {
    // 衝突通知がないフレームを退出として扱い、再発動待ち時間を更新する。
    SceneManager* sceneManager = SceneManager::GetInstance();
    const bool isPlaying = sceneManager && sceneManager->IsPlaying();

    // プレイ中でなければ、発動履歴を片付けて編集用の範囲へ戻す。
    if (!isPlaying) {
        ResetForEditor();
        BaseGimmick::Update(deltaTime);
        return;
    }

    // プレイ開始後の最初の更新で、設定した開始状態と空の占有状態を用意する。
    if (!initializedForPlay_) {
        InitializeForPlay();
    }

    // 発動後の待ち時間を秒単位で減らし、0になると再発動できるようにする。
    rearmTimer_ = (std::max)(0.0f, rearmTimer_ - (std::max)(0.0f, deltaTime));

    // 前回の走査で衝突の通知がなくなったときだけ離脱を処理し、次の走査のため観測フラグをリセットします。
    if (occupied_ && !collisionObserved_) {
        HandleExit(occupant_);
    }

    collisionObserved_ = false;

    BaseGimmick::Update(deltaTime);
}

void GimmickGameplayVolume::Draw(ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource) {
    // 編集中は範囲を見せ、ゲーム再生中は検知形状を描画しない。
    SceneManager* sceneManager = SceneManager::GetInstance();

    // シーン管理が未初期化の場合と、プレイしていない場合だけ編集用の形状を描く。
    if (!sceneManager || !sceneManager->IsPlaying()) {
        BaseGimmick::Draw(pointLightResource, spotLightResource);
    }
}

bool GimmickGameplayVolume::OnCollision(engine::Object3D* other) {
    // プレイヤーが入った最初のフレームだけ入場処理を呼び、物理的には押し戻さない。
    auto* player = dynamic_cast<Player*>(other);

    // 接触相手をプレイヤーとして扱えない場合は、占有状態を変更しない。
    if (!player) {
        return false;
    }

    // スイッチなどで無効になっている間は、プレイヤーの接触でも検知しない。
    if (!runtimeEnabled_) {
        return false;
    }

    collisionObserved_ = true;
    occupant_ = player;

    // 前回の接触から滞在が続いていない場合だけ、進入に切り替えて設定効果を調べる。
    if (!occupied_) {
        occupied_ = true;
        HandleEnter(player);
    }

    return false;
}

void GimmickGameplayVolume::OnTrigger() {
    // 単発の外部イベントは、スイッチと同じ経路で範囲の検知を有効にする。
    OnSwitchEvent(true);
}

void GimmickGameplayVolume::OnSwitchEvent(bool active) {
    // スイッチの状態に合わせて検知を切り替える。無効化時は設定に従って効果を解除し、占有状態を破棄する。
    runtimeEnabled_ = active;

    // 無効化された範囲は、次に有効になったときの進入を検知できるように占有を解除する。
    if (!runtimeEnabled_) {
        // プレイヤーが滞在中で、進入時に発動して退出時に戻す設定なら、継続効果を解除する。
        if (occupied_ && GetParameters().has_value() && !GetParameters()->volumeTriggerOnExit && GetParameters()->returnOnOff) {
            Deactivate();
        }

        occupied_ = false;
        collisionObserved_ = false;
        occupant_ = nullptr;
    }
}

std::unique_ptr<engine::Object3D> GimmickGameplayVolume::Clone() const {
    // 実行時の占有と一度きりの発動履歴は複製先へ引き継がない。
    auto clone = std::make_unique<GimmickGameplayVolume>();
    assert(GetObjectCommon() != nullptr);
    clone->Initialize(GetObjectCommon(), GetModelName());
    clone->CopyFrom(this);
    return clone;
}

void GimmickGameplayVolume::InitializeForPlay() {
    // 再生開始時に設定の有効状態へ戻し、編集用の可視化を消す。
    initializedForPlay_ = true;
    runtimeEnabled_ = GetParameters().has_value() ? GetParameters()->startActive : true;
    occupied_ = false;
    collisionObserved_ = false;
    triggered_ = false;
    rearmTimer_ = 0.0f;
    occupant_ = nullptr;
    SetIsVisible(false);
    SetCollisionAttribute(engine::collisionCategories::Get("trigger"));
    SetCollisionMask(engine::collisionCategories::Get("player"));
}

void GimmickGameplayVolume::ResetForEditor() {
    // 再生中の検知履歴を消し、Inspector で範囲を見られるよう可視化する。
    initializedForPlay_ = false;
    runtimeEnabled_ = true;
    occupied_ = false;
    collisionObserved_ = false;
    triggered_ = false;
    rearmTimer_ = 0.0f;
    occupant_ = nullptr;
    SetIsVisible(true);
    SetCollisionAttribute(engine::collisionCategories::Get("trigger"));
    SetCollisionMask(engine::collisionCategories::Get("player"));
}

void GimmickGameplayVolume::HandleEnter(Player* player) {
    // パラメーターがなければ実行対象がない。退出時発動の設定なら退出まで待つ。
    if (!GetParameters().has_value() || GetParameters()->volumeTriggerOnExit) {
        return;
    }

    Activate(player);
}

void GimmickGameplayVolume::HandleExit(Player* player) {
    // 退出時発動と、滞在中だけ有効な用途の終了を分けて扱う。
    // 退出時発動の設定では、範囲を離れた時点で用途別の処理を呼ぶ。
    if (GetParameters().has_value() && GetParameters()->volumeTriggerOnExit) {
        Activate(player);
    // 進入時に発動する設定で退出時に戻す指定があれば、継続していた効果を解除する。
    } else if (GetParameters().has_value() && GetParameters()->returnOnOff) {
        Deactivate();
    }

    occupied_ = false;
    occupant_ = nullptr;
}

void GimmickGameplayVolume::Activate(Player* player) {
    // 用途に応じてイベント、再開地点、落下死、演出、BGM、環境を扱う。
    // 無効・再実行待ち・一回限りの発動済み、または設定がない場合は効果を適用しない。
    if (!CanActivate() || !GetParameters().has_value()) {
        return;
    }

    BaseScene* scene = SceneManager::GetInstance() ? SceneManager::GetInstance()->GetCurrentScene() : nullptr;

    switch (GetMode()) {
    // 滞在中だけ有効な設定は Target ID をオンにし、それ以外は単発イベントとして通知します。
    case Mode::kEvent:
        // 現在のシーンがあり、正のTarget IDが指定されている場合だけイベントを送る。
        if (scene && GetTargetId() > 0) {
            // 進入時に発動し退出時に戻す設定なら、接続先を継続してオンにする。
            if (GetParameters()->returnOnOff && !GetParameters()->volumeTriggerOnExit) {
                scene->SetEventActive(GetTargetId(), true);
            // 継続オンの設定に当たらない場合は、接続先へ単発の通知を送る。
            } else {
                scene->TriggerEvent(GetTargetId());
            }
        }

        break;

    // 領域の中心を再開地点に使い、同じ地点から再開しただけなら残機と回復を重ねません。
    case Mode::kCheckpoint:
        // プレイヤーへの参照を取得できた場合に、領域の中心を再開地点として登録する。
        if (player) {
            const engine::Vector3 checkpointPosition = GetWorldPosition();
            const engine::Vector3 previousRespawnPosition = player->GetRespawnPosition();
            const bool restoringSameCheckpoint =
                std::abs(previousRespawnPosition.x - checkpointPosition.x) < 0.01f &&
                std::abs(previousRespawnPosition.y - checkpointPosition.y) < 0.01f &&
                std::abs(previousRespawnPosition.z - checkpointPosition.z) < 0.01f;
            player->ActivateCheckpoint(checkpointPosition);

            // 前の再開地点と各軸0.01未満の差で一致する場合や、編集内部用プレイヤーの場合は報酬を与えない。
            if (!restoringSameCheckpoint && !player->IsEditorInternal()) {
                // 領域型のチェックポイントも旗と同じ報酬を与え、復活直後の再進入は除外します。
                GameDataManager* gameData = GameDataManager::GetInstance();

                // 残機が上限に達していない場合は1増やす。体力の回復は上限に到達していても行う。
                if (gameData->GetLives() < GameDataManager::kMaxLives) {
                    gameData->SetLives(gameData->GetLives() + 1);
                }

                player->RestoreHpToFull();
            }

            VfxSequencer::PlayOneShot("shared/checkpoint_activate_cue", checkpointPosition);
        }

        break;

    // その場で別の死亡処理を実装せず、既存の落下死判定までプレイヤーを下げます。
    case Mode::kFallDeath:
        // 生存中のプレイヤーを落下死判定に入る高さまで下げ、移動速度を0にする。
        if (player && !player->IsDead()) {
            engine::Vector3 position = player->GetTranslate();
            position.y = -19.0f;
            player->SetTranslate(position);
            player->SetVelocity({0.0f, 0.0f, 0.0f});
        }

        break;

    // 指定された演出を領域の中心から一度再生します。
    case Mode::kFeedbackCue:
        // 演出IDが設定されている場合だけ、領域中心でその演出を再生する。
        if (!GetParameters()->volumePayload.empty()) {
            VfxSequencer::PlayOneShot(GetParameters()->volumePayload, GetWorldPosition());
        }

        break;

    // 設定された曲をゲームの音声管理へ渡します。
    case Mode::kBgm:
        // 曲のIDが設定されている場合だけ、ゲームの音声管理へ再生を依頼する。
        if (!GetParameters()->volumePayload.empty()) {
            GameAudioSettings::GetInstance()->PlayBGM(GetParameters()->volumePayload);
        }

        break;

    // 現在の照明・環境を退避し、指定秒数で新しいプロファイルに切り替えます。
    case Mode::kEnvironment:
        // 切替先のプロファイルIDがある場合に、戻すための環境を保存してから切り替える。
        if (!GetParameters()->volumePayload.empty()) {
            previousEnvironment_ = engine::LightManager::GetInstance()->CaptureEnvironmentProfile();
            hasPreviousEnvironment_ = true;
            engine::LightManager::GetInstance()->ApplyEnvironmentProfile(GetParameters()->volumePayload, GetParameters()->volumeBlendDuration);
        }

        break;
    }

    // 用途の条件で個別の効果が省略された場合も発動履歴を記録し、負の待ち時間は0秒として扱う。
    triggered_ = true;
    rearmTimer_ = (std::max)(0.0f, GetParameters()->volumeRearmDelay);
}

void GimmickGameplayVolume::Deactivate() {
    // イベント・BGM・環境設定のうち継続する作用だけを元に戻す。
    // 設定がなく用途別の解除先を決められない場合は何もしない。
    if (!GetParameters().has_value()) {
        return;
    }

    BaseScene* scene = SceneManager::GetInstance() ? SceneManager::GetInstance()->GetCurrentScene() : nullptr;

    // イベント用途では、シーンと正のTarget IDがある場合にOFFを通知する。
    if (GetMode() == Mode::kEvent && scene && GetTargetId() > 0) {
        scene->SetEventActive(GetTargetId(), false);
    // BGM用途で曲IDがある場合は、その曲を停止する。
    } else if (GetMode() == Mode::kBgm && !GetParameters()->volumePayload.empty()) {
        GameAudioSettings::GetInstance()->Stop(GetParameters()->volumePayload);
    // 環境用途で退避済みの設定がある場合は、切替時間を使って元の環境へ戻す。
    } else if (GetMode() == Mode::kEnvironment && hasPreviousEnvironment_) {
        engine::LightManager::GetInstance()->ApplyEnvironmentProfile(previousEnvironment_, GetParameters()->volumeBlendDuration);
        hasPreviousEnvironment_ = false;
    }
}

bool GimmickGameplayVolume::CanActivate() const {
    // 一度きり指定と再発動待ち時間を合わせて発動可否を決める。
    // 無効、設定なし、または再起動待ちの残りが正なら、発動を受け付けない。
    if (!runtimeEnabled_ || !GetParameters().has_value() || rearmTimer_ > 0.0f) {
        return false;
    }

    // 繰り返し可なら発動履歴に関係なく、一回限りならまだ発動していない場合だけ許可する。
    return !GetParameters()->volumeTriggerOnce || !triggered_;
}

GimmickGameplayVolume::Mode GimmickGameplayVolume::GetMode() const {
    // 不正な JSON 値は定義済みの用途範囲に丸める。
    const std::int32_t mode = GetParameters().has_value() ? (std::clamp)(GetParameters()->volumeMode, 0, 5) : 0;
    return static_cast<Mode>(mode);
}
