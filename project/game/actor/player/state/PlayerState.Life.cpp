#define NOMINMAX
#include <cstdint>
#include "game/actor/player/state/PlayerState.h"
#include "game/actor/player/Player.h"
#include "DebugConsole.h"
#include "engine/graphics/postprocess/Fade.h"
#include "engine/graphics/postprocess/PostEffect.h"
#include "engine/graphics/3d/camera/CameraManager.h"
#include "game/system/progress/GameDataManager.h"
#include "SceneManager.h"
#include "game/scene/gameplay/GamePlayScene.h"
#include "engine/utility/math/Math.h"
#include <algorithm>
#include <cmath>
#include <memory>

// 死亡と落下復帰の演出状態をまとめています。

namespace {
engine::Vector2 CalculatePlayerIrisCenter(Player* player) {
    // プレイヤーを画面へ投影し、暗転へ渡す正規化中心を求める。既定値は中央。
    engine::Vector2 irisCenter = {0.5f, 0.5f};
    engine::Camera* cam = CameraManager::GetInstance()->GetActiveCamera();

    // プレイヤーかカメラが無ければ投影できないため、中央を返す。
    if (!player || !cam) {
        return irisCenter;
    }

    // 足元より1単位上を、カメラの正規化デバイス座標へ投影する。
    engine::Vector3 worldPos = player->GetWorldPosition();
    worldPos.y += 1.0f;
    engine::Vector3 ndc = engine::Math::Transform(worldPos, cam->GetViewProjectionMatrix());
    const bool isOnScreen = std::isfinite(ndc.x) && std::isfinite(ndc.y) && std::isfinite(ndc.z) && ndc.z >= 0.0f &&
                            std::abs(ndc.x) <= 1.15f && std::abs(ndc.y) <= 1.15f;
    // 有限な投影値で深度が0以上、X・Yが画面近辺なら、画面端へ寄りすぎないよう補正して採用する。
    if (isOnScreen) {
        irisCenter = {std::clamp((ndc.x + 1.0f) * 0.5f, 0.18f, 0.82f), std::clamp((1.0f - ndc.y) * 0.5f, 0.16f, 0.78f)};
    }

    return irisCenter;
}

GamePlayScene* GetCurrentGamePlayScene() {
    // 残機表示の通知先として、現在のゲームプレイシーンを取得する。
    SceneManager* sceneManager = SceneManager::GetInstance();

    // シーン管理が取得できなければ、残機表示を通知できない。
    if (!sceneManager) {
        return nullptr;
    }

    return dynamic_cast<GamePlayScene*>(sceneManager->GetCurrentScene());
}

// 現在のGamePlaySceneへ残機減少前後の値を渡し、死亡演出を開始します。
void StartLifeLostPresentationOnScene() {
    // 現在がGamePlaySceneの場合だけ、減少後の残機から表示用の減少前と減少後の値を渡す。
    if (GamePlayScene* scene = GetCurrentGamePlayScene()) {
        const std::int32_t afterLives = GameDataManager::GetInstance()->GetLives();
        scene->StartLifeLostPresentation(afterLives + 1, afterLives);
    }
}
} // namespace

// 死亡状態 (Dead)
// 死亡演出の時計と状態を初期化します。実戦では残機に応じてカメラを固定し、操作を止めます。
void PlayerStateDead::Enter(Player* player) {
    DebugConsole::GetInstance()->AddLog("Enter: Dead");
    timer_ = 0.0f;
    sceneChangeRequested_ = false;
    lifePresentationStarted_ = false;
    // 状態に入った時点の残機が1以下なら、死亡開始時から最終残機向けの演出を使う。
    finalDeath_ = GameDataManager::GetInstance()->GetLives() <= 1;

    // プレビューの死亡演出は隔離し、実戦のカメラ・暗転・シーン遷移を操作しません。
    if (player && player->IsEditorInternal()) {
        deathAnimation_.Start(player, false);
        player->SetIsControlActive(false);
        return;
    }

    Fade::GetInstance()->Stop();

    // プレイヤーが存在する場合だけ、被弾時の無敵を解除して死亡モーションを開始する。
    if (player) {
        player->SetDamageInvincible(false);
        deathAnimation_.Start(player, finalDeath_);

        // フックのマーカーが残っていれば、死亡モーション中は隠す。
        if (engine::Object3D* hookMarker = player->GetHookMarker()) {
            hookMarker->SetIsVisible(false);
        }

        engine::Camera* cam = CameraManager::GetInstance()->GetActiveCamera();

        // カメラがあり、槍雨ムービーがそのカメラを使用中でなければ視点を固定する。
        if (cam && !(GetCurrentGamePlayScene() && GetCurrentGamePlayScene()->IsSealBarrageMovieActive())) {
            cam->SetFreezeEye(true);

            // 残機が尽きる死亡では、0.24秒の寄りと短い揺れを加える。
            if (finalDeath_) {
                const engine::Vector3 focus = player->GetWorldPosition() + engine::Vector3{0.0f, 0.58f, 0.0f};
                engine::Vector3 focusEye = engine::Math::Lerp(cam->GetEye(), focus, 0.16f);
                focusEye.y += 0.10f;

                engine::Camera::CameraOverrideParams cameraOverride;
                cameraOverride.duration = 0.24f;
                cameraOverride.exitDuration = 0.0f;
                cameraOverride.easing = engine::Camera::OverrideEasing::kEaseOut;
                cameraOverride.fixedEyePos = focusEye;
                cameraOverride.fixedTargetPos = focus;
                cameraOverride.trackEyeX = false;
                cameraOverride.trackEyeY = false;
                cameraOverride.trackEyeZ = false;
                cameraOverride.trackTargetX = false;
                cameraOverride.trackTargetY = false;
                cameraOverride.trackTargetZ = false;
                cam->StartOverride(cameraOverride);
                cam->StartShake(0.22f, 0.085f, 30.0f, {0.85f, 0.60f, 0.35f});
            }
        }

        // 暗転に渡す中心候補を、死亡時のプレイヤーの投影位置から保存する。
        irisCenter_ = CalculatePlayerIrisCenter(player);
    }

    // プレイヤーがいる場合は、死亡演出中の通常入力を止める。
    if (player)
        player->SetIsControlActive(false);
}

// 死亡モーション、暗転、残機表示を順に進め、ゲームオーバーかステージ再開へ引き渡します。
void PlayerStateDead::Update(Player* player, float deltaTime) {
    // 対象がいない場合と、遷移を要求済みの場合は演出を進めない。
    if (!player || sceneChangeRequested_)
        return;

    // エディター内のプレイヤーは、プレビューの死亡モーションだけ更新する。
    if (player->IsEditorInternal()) {
        deathAnimation_.Update(player, deltaTime);
        return;
    }

    // 即死のHP判定は済んでいても、全域放電と槍の崩壊が終わるまで暗転を待ちます。
    if (auto* scene = GetCurrentGamePlayScene(); scene && scene->IsSealBarrageMovieActive())
        return;

    // 死亡演出の時計がまだ進んでいない場合は、最初に視点固定を試す。
    if (timer_ == 0.0f)
        // 現在のカメラを取得できた場合だけ視点を固定する。
        if (engine::Camera* camera = CameraManager::GetInstance()->GetActiveCamera())
            camera->SetFreezeEye(true);
    timer_ += deltaTime;
    finalDeath_ = finalDeath_ || GameDataManager::GetInstance()->GetLives() <= 0;
    deathAnimation_.Update(player, deltaTime);

    // 最終残機では1.45秒、残機がある場合は1.25秒を暗転時間にする。
    const float irisDuration = finalDeath_ ? 1.45f : 1.25f;
    // 死亡モーションが暗転可能で、残機表示が未開始かつフェードが無いときに暗転を開始する。
    if (deathAnimation_.IsReadyForFade() && !lifePresentationStarted_ &&
        Fade::GetInstance()->GetStatus() == Fade::Status::kNone) {
        Fade::GetInstance()->StartIrisOut(irisDuration, irisCenter_);
    }

    // 暗転完了後、残機表示が未開始なら、このフレームで残機表示へ引き渡す。
    if (!lifePresentationStarted_ && Fade::GetInstance()->IsFinished()) {
        Fade::GetInstance()->Stop();
        lifePresentationStarted_ = true;
        StartLifeLostPresentationOnScene();
        return;
    }

    // 残機表示を開始していなければ、シーン遷移の判定へ進まない。
    if (!lifePresentationStarted_) {
        return;
    }

    GamePlayScene* scene = GetCurrentGamePlayScene();

    // ゲームプレイシーンの残機表示が終わるまで、シーン遷移を見送る。
    if (scene && !scene->IsLifeLostPresentationFinished()) {
        return;
    }

    // 再度遷移を要求されないようフラグを立て、次のシーンへ移る前に現在のカメラ固定を解除します。
    sceneChangeRequested_ = true;
    engine::Camera* cam = CameraManager::GetInstance()->GetActiveCamera();

    // 現在のカメラがあれば、次のシーンへ視点固定を持ち越さないようにする。
    if (cam) {
        // 一時的な視点上書きが残っている場合は、待ち時間0秒で終える。
        if (cam->IsOverridden()) {
            cam->EndOverride(0.0f);
        }

        cam->SetFreezeEye(false);
    }

    // 残機が無ければ、フェードを停止してゲームオーバーへ移る。
    if (GameDataManager::GetInstance()->GetLives() <= 0) {
        Fade::GetInstance()->Stop();
        SceneManager::GetInstance()->ChangeSceneWithoutLoading("GAMEOVER");
    // 残機がある場合は、次のステージ開始時に画面を開くよう予約する。
    } else {
        GameDataManager::GetInstance()->RequestRespawnIrisIn();
        SceneManager::GetInstance()->ChangeSceneWithoutLoading("GAMEPLAY", true);
    }
}

void PlayerStateDead::Exit(Player* player) {
    // シーンが継続する場合でも、カメラ固定と操作禁止を残さない。
    // プレイヤーが残っていれば、死亡演出が変更した見た目を元に戻して操作を許可する。
    if (player) {
        deathAnimation_.RestoreVisual(player);
        player->SetIsControlActive(true);
    }

    engine::Camera* cam = CameraManager::GetInstance()->GetActiveCamera();

    // 現在のカメラがあれば、退出時にも視点の固定を解除する。
    if (cam) {
        // 視点上書きが残っていれば、固定解除の前に終了する。
        if (cam->IsOverridden()) {
            cam->EndOverride(0.0f);
        }

        cam->SetFreezeEye(false);
    }
}

// 落下演出状態 (FallingOut)
void PlayerStateFallingOut::Enter(Player* player) {
    // プレイヤーが無ければ、落下時の入力停止と暗転の中心計算を実行できない。
    if (!player)
        return;
    player->SetIsControlActive(false);
    irisCenter_ = CalculatePlayerIrisCenter(player);
    engine::Camera* cam = CameraManager::GetInstance()->GetActiveCamera();

    // アクティブなカメラを取得できれば、落下を見せる間の視点を固定する。
    if (cam) {
        cam->SetFreezeEye(true);
    }

    // 落下を見せる待機段階へ入り、経過秒数を0にする。
    phase_ = Phase::kWaiting;
    waitTimer_ = 0.0f;
}

// 落下後に暗転と残機表示を進め、残機に応じて復帰かゲームオーバーへ引き渡します。
void PlayerStateFallingOut::Update(Player* player, float deltaTime) {
    // プレイヤーが無ければ、位置や入力を変更できないため終了する。
    if (!player)
        return;

    // アイリスの開閉中に、保存した正規化中心を後処理の定数へ渡す。現行の Fade の連番描画は画面中央に置かれる。
    if (phase_ == Phase::kIrisOut || phase_ == Phase::kIrisIn) {
        PostEffect::GetInstance()->GetParams()->irisCenterX = irisCenter_.x;
        PostEffect::GetInstance()->GetParams()->irisCenterY = irisCenter_.y;
    }

    switch (phase_) {
    // 落下を見せる時間を待ち、保存した中心を引数に渡して暗転を始める。
    case Phase::kWaiting:
        waitTimer_ += deltaTime;

        // 落下を見せる時間が1.55秒に達したら、1.35秒の暗転を始める。
        if (waitTimer_ >= 1.55f) {
            Fade::GetInstance()->StartIrisOut(1.35f, irisCenter_);
            phase_ = Phase::kIrisOut;
        }

        break;

    // 暗転が終わってから残機減少演出へ渡します。
    case Phase::kIrisOut:
        // 暗転を閉じ終えたフレームで、残機減少の表示へ切り替える。
        if (Fade::GetInstance()->IsFinished()) {
            Fade::GetInstance()->Stop();
            StartLifeLostPresentationOnScene();
            phase_ = Phase::kLifeLost;
            break;
        }

        break;

    // 残機減少の表示を待ち、再開できる場合のみ、復帰地点とそれを映すカメラを先に更新します。
    case Phase::kLifeLost:
        // 現在がゲームプレイシーンなら、残機表示の完了を確認する。
        if (GamePlayScene* scene = GetCurrentGamePlayScene()) {
            // 残機表示中はこの段階に留まり、復帰位置もシーンも切り替えない。
            if (!scene->IsLifeLostPresentationFinished()) {
                break;
            }
        }

        // 残機が尽きた場合は再配置せず、視点固定とフェードを解除してゲームオーバーへ移る。
        if (GameDataManager::GetInstance()->GetLives() <= 0) {
            CameraManager::GetInstance()->GetActiveCamera()->SetFreezeEye(false);
            Fade::GetInstance()->Stop();
            SceneManager::GetInstance()->ChangeSceneWithoutLoading("GAMEOVER");
            return;
        }

        // 残機があれば復帰地点に配置し、落下時の速度を残さない。
        player->SetTranslate(player->GetRespawnPosition());
        player->SetVelocity({0, 0, 0});

        // 追従カメラを一度更新し、復帰地点を映す視点を固定し直す。
        CameraManager::GetInstance()->GetActiveCamera()->SetFreezeEye(false);
        CameraManager::GetInstance()->GetActiveCamera()->Update(deltaTime);
        CameraManager::GetInstance()->GetActiveCamera()->SetFreezeEye(true);

        irisCenter_ = CalculatePlayerIrisCenter(player);

        // ゲームプレイシーンの場合は、画面を開く前に残機表示を隠す。
        if (GamePlayScene* scene = GetCurrentGamePlayScene()) {
            scene->HideLifeLostPresentationOverlay();
        }

        Fade::GetInstance()->StartIrisIn(1.25f, irisCenter_);
        phase_ = Phase::kIrisIn;
        break;

    // 復帰位置で画面を開き切ってから、待機状態に移し入力を戻します。
    case Phase::kIrisIn:
        // 復帰先でアイリスを開き切ったら、待機へ遷移して入力を再開する。
        if (Fade::GetInstance()->IsFinished()) {
            Fade::GetInstance()->Stop();
            player->ChangeState(std::make_unique<PlayerStateIdle>());
            player->SetIsControlActive(true);
        }

        break;
    }
}

void PlayerStateFallingOut::Exit(Player* player) {
    // プレイヤーが残っていれば、落下演出が止めていた入力を許可する。
    if (player) {
        player->SetIsControlActive(true);
    }

    // 落下状態を抜けるときに、現在のカメラの視点固定を解除する。
    CameraManager::GetInstance()->GetActiveCamera()->SetFreezeEye(false);
}
