#define NOMINMAX
#include "game/scene/gameplay/GamePlayScene.h"
#include "engine/editor/graphics/camera/CameraEditor.h"
#include "CameraManager.h"
#include "DebugConsole.h"
#include "GpuParticleManager.h"
#include "PostEffect.h"
#include "game/actor/gimmick/stage/GimmickStageGate.h"
#include "engine/utility/math/AnimationInterpolation.h"
#include <algorithm>
#include <cmath>
#include <numbers>

#include "game/scene/gameplay/GamePlayScene.PresentationMath.h"
using namespace gamePlayPresentationMath;

namespace {
constexpr float kStageEntryCameraIntroDuration = 0.24f;
constexpr float kStageEntryGateOpenLeadTime = 0.42f;
constexpr float kStageEntryCameraRestoreStartTime = 1.62f;
constexpr float kStageEntryCameraRestoreDuration = 1.05f;
constexpr float kStageEntryPresentationDuration = kStageEntryCameraRestoreStartTime + kStageEntryCameraRestoreDuration;
constexpr float kStageEntryCameraBackDistance = 15.0f;
constexpr float kStageEntryCameraHeight = 3.4f;
constexpr float kStageEntryCameraYawOffset = -0.48f;
constexpr float kStageEntryCameraFocusHeight = 1.15f;
constexpr float kStageEntryCameraLandingFocusRate = 0.72f;
constexpr float kStageEntryThirdPersonDistance = 16.0f;
constexpr float kStageEntryThirdPersonHeight = 3.2f;
constexpr float kStageEntryThirdPersonPitch = 0.32f;
constexpr float kStageEntryCinemaBarHeight = 0.105f;
constexpr float kStageEntryCinemaBarOpenDuration = 0.22f;
constexpr const char* kStageEntryCameraObjectName = "Stage1_EntryCamera";
constexpr const char* kStageEntryCameraFallbackObjectName = "Cinematic_Camera_01";
} // namespace

// ステージ入場演出の開始・更新・復帰。PlayerとCameraは借用し、終了時の操作復帰とゲート再有効化を維持する。

engine::Object3D* GamePlayScene::FindStageEntryGate() const {
    // ステージ開始演出の対象ゲートを現在の配置オブジェクトから探す。
    if (!objectManager_) {
        return nullptr;
    }

    engine::Object3D* nameMatchedGate = nullptr;

    for (const auto& object : objectManager_->GetObjects()) {
        // 入口用の命名規則に当てはまる実体だけ、ゲートの種別を確認する。
        if (!object || object->GetName().find("_EntranceGate") == std::string::npos) {
            continue;
        }

        // 現在のタイプ表記を優先し、型だけ残った古い配置は候補として控えます。
        if (object->GetGimmickType() == "StageGate") {
            return object.get();
        }

        // タイプ表記が取れない旧配置は、実体が StageGate である場合に限り最初の候補を記録する。
        if (!nameMatchedGate && dynamic_cast<GimmickStageGate*>(object.get())) {
            nameMatchedGate = object.get();
        }
    }

    return nameMatchedGate;
}

// 開始時に通常のスポーン地点を保存し、入口ゲートから出現する演出位置へ一時的に移します。
bool GamePlayScene::StartStageEntryPresentation() {
    // すでに演出中なら再初期化せず、開始済みとして成功を返す。
    if (stageEntryPresentationActive_) {
        return true;
    }

    stageEntryGate_ = FindStageEntryGate();
    engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();

    // 入口・プレイヤー・メインカメラのどれかが未配置なら、部分的に演出を始めません。
    if (!stageEntryGate_ || !player_ || !camera) {
        stageEntryGate_ = nullptr;
        return false;
    }

    // OnActivated直後は、ロードしたTransformのワールド行列がまだ更新されていない場合があります。
    // 親階層を含めて確定してから入口演出の基準座標を取得します。
    engine::Object3D* gateTransformRoot = stageEntryGate_;

    while (gateTransformRoot->GetParent()) {
        gateTransformRoot = gateTransformRoot->GetParent();
    }

    gateTransformRoot->UpdateWorldMatrix(false);

    // 初期スポーン地点をリスポーン地点として確定させてから、演出用の位置へ移動します。
    player_->Update(0.0f);
    const engine::Vector3 playerSpawn = player_->GetWorldPosition();
    player_->SetRespawnPosition(playerSpawn);
    const engine::Vector3 gatePosition = stageEntryGate_->GetWorldPosition();
    stageEntryDirection_ = NormalizePlanarDirection(playerSpawn - gatePosition, {1.0f, 0.0f, 0.0f});

    // プレイヤーの経路には、門の内側、門の中心、通常スポーン地点と元の大きさを渡す。
    PlayerGateReturnAnimation::Route route;
    route.start = gatePosition - stageEntryDirection_ * 1.38f;
    route.gateCenter = gatePosition;
    route.end = playerSpawn;
    route.direction = stageEntryDirection_;
    route.baseScale = player_->GetScale();
    player_->StartGateReturnAnimation(route);

    // ゲートの反応を先に見せ、内部から出始める瞬間までプレイヤーを隠します。
    player_->SetIsVisible(false);
    stageEntryPlayerEmergenceStarted_ = false;

    // 入口の物体が専用ゲート型の場合は、接触遷移を止めて開く反応だけを開始する。
    if (auto* gate = dynamic_cast<GimmickStageGate*>(stageEntryGate_)) {
        // 登場中に入口ゲートへ触れて、即座にステージ選択へ戻らないようにします。
        gate->SetTransitionEnabled(false);
        gate->TriggerEntryReaction();
    }

    // 演出の黒帯で既存の画面設定を失わないよう、開始時の高さを保存します。
    if (PostEffect::Params* params = PostEffect::GetInstance()->GetParams()) {
        stageEntryCinemaBarBaseHeight_ = params->cinemaBarHeight;
        stageEntryCinemaBarOverrideActive_ = true;
    }

    const engine::Vector3 gateFocus = {gatePosition.x, playerSpawn.y + kStageEntryCameraFocusHeight, gatePosition.z};
    const engine::Vector3 landingFocus = playerSpawn + engine::Vector3{0.0f, kStageEntryCameraFocusHeight, 0.0f};
    stageEntryCameraFocusTarget_ =
        animationInterpolation::Lerp(gateFocus, landingFocus, kStageEntryCameraLandingFocusRate);

    // ステージ内部の中庭へ直進すると地形内へ入るため、入口の外側から斜めに見ます。
    const float thirdPersonYaw =
        std::atan2(stageEntryDirection_.x, stageEntryDirection_.z) + kStageEntryCameraYawOffset;
    const engine::Matrix4x4 cinematicYawRotation = engine::Math::MakeRotateYMatrix(thirdPersonYaw);
    stageEntryCameraFocusEye_ =
        stageEntryCameraFocusTarget_ +
        engine::Math::TransformNormal({0.0f, 0.0f, -kStageEntryCameraBackDistance}, cinematicYawRotation) +
        engine::Vector3{0.0f, kStageEntryCameraHeight, 0.0f};

    // 編集中の自由カメラ位置を引き継がず、毎回ゲートを読める決定的な導入ショットから始めます。
    const engine::Vector3 cinematicBackDirection =
        NormalizePlanarDirection(stageEntryCameraFocusEye_ - stageEntryCameraFocusTarget_,
                                 {-stageEntryDirection_.x, 0.0f, -stageEntryDirection_.z});
    stageEntryCameraStartTarget_ = gateFocus;
    stageEntryCameraStartEye_ = stageEntryCameraFocusEye_ + cinematicBackDirection * 2.0f + engine::Vector3{0.0f, 0.6f, 0.0f};

    // 復帰先はSnapToThirdPersonと同じ式で作り、演出終了フレームの段差をなくします。
    const engine::Matrix4x4 thirdPersonRotation =
        engine::Math::MakeRotateXMatrix(kStageEntryThirdPersonPitch) * engine::Math::MakeRotateYMatrix(thirdPersonYaw);
    stageEntryCameraRestoreTarget_ = playerSpawn + engine::Vector3{0.0f, kStageEntryThirdPersonHeight, 0.0f};
    stageEntryCameraRestoreEye_ =
        stageEntryCameraRestoreTarget_ +
        engine::Math::TransformNormal({0.0f, 0.0f, -kStageEntryThirdPersonDistance}, thirdPersonRotation);

    // 入口専用名を最優先し、旧データでは入口ゲートに最も近い汎用カメラを採用します。
    // 名前を分けることで、JSON読込時に同名Objectとして上書きされることも防ぎます。
    engine::Object3D* authoredEntryCamera = nullptr;
    engine::Object3D* fallbackEntryCamera = nullptr;
    float nearestCameraDistanceSq = (std::numeric_limits<float>::max)();

    // 配置物の所有一覧を取得できる場合は、カメラの設定を検索する。
    if (objectManager_) {
        for (const auto& object : objectManager_->GetObjects()) {
            // 空参照と Camera Object でない配置物は候補に含めない。
            if (!object || !object->IsCameraObject()) {
                continue;
            }

            // 入口専用の名前を見つけたら、そのカメラを優先して検索を終える。
            if (object->GetName() == kStageEntryCameraObjectName) {
                authoredEntryCamera = object.get();
                break;
            }

            // 専用名に当てはまらない場合、旧形式の汎用名と一致するときだけ距離を比較する。
            if (object->GetName() != kStageEntryCameraFallbackObjectName) {
                continue;
            }

            engine::Object3D* candidateTransformRoot = object.get();

            // 候補が親に従う場合は、階層のルートから姿勢を確定してから距離を測る。
            while (candidateTransformRoot->GetParent()) {
                candidateTransformRoot = candidateTransformRoot->GetParent();
            }

            candidateTransformRoot->UpdateWorldMatrix(false);
            const engine::Vector3 offset = object->GetWorldPosition() - gatePosition;
            const float distanceSq = offset.x * offset.x + offset.y * offset.y + offset.z * offset.z;

            // 前の候補より入口に近ければ、旧形式で使う代替カメラを更新する。
            if (distanceSq < nearestCameraDistanceSq) {
                nearestCameraDistanceSq = distanceSq;
                fallbackEntryCamera = object.get();
            }
        }
    }

    // 専用名のカメラがなければ、見つかった中で最も入口に近い汎用カメラを候補にする。
    if (!authoredEntryCamera) {
        authoredEntryCamera = fallbackEntryCamera;
    }

    stageEntryCameraBaseFov_ = camera->GetFovY();
    stageEntryCameraFocusFov_ = stageEntryCameraBaseFov_;

    // 編集カメラの姿勢が解決できた場合だけ、計算した導入ショットと画角を上書きします。
    if (authoredEntryCamera) {
        engine::Object3D* cameraTransformRoot = authoredEntryCamera;

        while (cameraTransformRoot->GetParent()) {
            cameraTransformRoot = cameraTransformRoot->GetParent();
        }

        cameraTransformRoot->UpdateWorldMatrix(false);

        engine::Vector3 authoredEye{};
        engine::Vector3 authoredTarget{};

        // 配置設定から視点と注視点を両方解決できたときだけ、計算で用意した構図と画角を差し替える。
        if (CameraEditor::GetInstance()->ResolveSceneCameraPose(authoredEntryCamera, authoredEye, authoredTarget)) {
            stageEntryCameraStartEye_ = authoredEye;
            stageEntryCameraStartTarget_ = authoredTarget;
            stageEntryCameraFocusEye_ = authoredEye;
            stageEntryCameraFocusTarget_ = authoredTarget;
            stageEntryCameraFocusFov_ = authoredEntryCamera->GetSceneCameraSettings().fovY;
            DebugConsole::GetInstance()->AddLog("Stage Entry: using authored camera for Stage1_EntranceGate.");
        }
    }

    // Game Viewが別の一時カメラを参照していても、入口演出中は必ずこのメインカメラを描画します。
    CameraManager::GetInstance()->SetActiveCamera(camera);
    camera->SetInputEnabled(false);
    camera->SetFollowTarget(nullptr);
    camera->SetLockOnTarget(nullptr);
    camera->SetFollowMode(engine::Camera::FollowMode::kFixedPoint);
    camera->SetEye(stageEntryCameraStartEye_);
    camera->SetTarget(stageEntryCameraStartTarget_);
    const engine::Vector3 startRotation = MakeLookAtEuler(stageEntryCameraStartEye_, stageEntryCameraStartTarget_);
    camera->SetRotation(startRotation);
    camera->ConfigFixedPoint(stageEntryCameraStartEye_, startRotation);
    camera->SetFovY(stageEntryCameraFocusFov_);
    camera->Update(0.0f);

    // 初期構図を反映した後で演出を実行中にし、開始の再試行と保留を解除する。
    stageEntryPresentationTimer_ = 0.0f;
    stageEntryPresentationRetryTimer_ = 0.0f;
    stageEntryPresentationPending_ = false;
    stageEntryPresentationActive_ = true;
    DebugConsole::GetInstance()->AddLog("Stage Entry: gate presentation start.");
    return true;
}

void GamePlayScene::UpdateStageEntryPresentation(float deltaTime) {
    // 途中で役者がなくなった場合も、通常制御の復元経路に入ります。
    if (!stageEntryPresentationActive_ || !player_) {
        FinishStageEntryPresentation();
        return;
    }

    stageEntryPresentationTimer_ += std::max(deltaTime, 0.0f);

    // ゲートを先に動かし、所定の秒数に達してからプレイヤーを出現させます。
    if (!stageEntryPlayerEmergenceStarted_ && stageEntryPresentationTimer_ >= kStageEntryGateOpenLeadTime) {
        stageEntryPlayerEmergenceStarted_ = true;
        player_->SetIsVisible(true);
    }

    // 出現を開始した後は、Player 自身にゲートから出る経路を進めてもらう。
    if (stageEntryPlayerEmergenceStarted_) {
        player_->Update(deltaTime);
    // 出現時刻までは体を隠し、操作と速度も止めたままゲート側の反応を見せる。
    } else {
        player_->SetIsVisible(false);
        player_->SetIsControlActive(false);
        player_->SetVelocity({0.0f, 0.0f, 0.0f});
    }

    // 入口ゲートの参照が残っている間だけ、ゲート自身の演出を実時間で進める。
    if (stageEntryGate_) {
        stageEntryGate_->Update(deltaTime);
    }

    // 黒帯は導入で開き、通常視点への復帰と同じ区間で元の高さに戻します。
    if (stageEntryCinemaBarOverrideActive_) {
        // ポストエフェクトの書き込み先が使えるフレームだけ、帯の進行率を反映する。
        if (PostEffect::Params* params = PostEffect::GetInstance()->GetParams()) {
            const float openRate = animationInterpolation::ApplyEasing(
                animationInterpolation::SegmentRate(stageEntryPresentationTimer_, 0.0f,
                                                    kStageEntryCinemaBarOpenDuration),
                animationInterpolation::EasingType::kSmootherStep);
            const float closeRate = animationInterpolation::ApplyEasing(
                animationInterpolation::SegmentRate(stageEntryPresentationTimer_, kStageEntryCameraRestoreStartTime,
                                                    kStageEntryPresentationDuration),
                animationInterpolation::EasingType::kSmootherStep);
            // 開く側の進行率に閉じる側の残り率を掛け、開始と終了の両方で元の帯の高さに一致させる。
            const float blend = openRate * (1.0f - closeRate);
            const float targetHeight = std::max(stageEntryCinemaBarBaseHeight_, kStageEntryCinemaBarHeight);
            params->cinemaBarHeight =
                stageEntryCinemaBarBaseHeight_ + (targetHeight - stageEntryCinemaBarBaseHeight_) * blend;
        }
    }

    engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();

    // メインカメラを取得できる場合は、演出中の構図から通常構図へ毎フレーム補間する。
    if (camera) {
        CameraManager::GetInstance()->SetActiveCamera(camera);
        engine::Vector3 eye = stageEntryCameraFocusEye_;
        engine::Vector3 target = stageEntryCameraFocusTarget_;
        float fov = stageEntryCameraFocusFov_;

        // カメラは最初にゲートへ寄り、後半で通常追従の位置と画角へ補間します。
        if (stageEntryPresentationTimer_ < kStageEntryCameraRestoreStartTime) {
            const float introRate = animationInterpolation::ApplyEasing(
                animationInterpolation::SegmentRate(stageEntryPresentationTimer_, 0.0f, kStageEntryCameraIntroDuration),
                animationInterpolation::EasingType::kEaseOut);
            eye = animationInterpolation::Lerp(stageEntryCameraStartEye_, stageEntryCameraFocusEye_, introRate);
            target =
                animationInterpolation::Lerp(stageEntryCameraStartTarget_, stageEntryCameraFocusTarget_, introRate);
        } else {
            // 復帰区間だけを0〜1に換算し、目の位置・注視点・画角を同じ割合で通常視点へ戻す。
            const float restoreRate = animationInterpolation::ApplyEasing(
                animationInterpolation::SegmentRate(stageEntryPresentationTimer_, kStageEntryCameraRestoreStartTime,
                                                    kStageEntryPresentationDuration),
                animationInterpolation::EasingType::kSmootherStep);
            eye = animationInterpolation::Lerp(stageEntryCameraFocusEye_, stageEntryCameraRestoreEye_, restoreRate);
            target =
                animationInterpolation::Lerp(stageEntryCameraFocusTarget_, stageEntryCameraRestoreTarget_, restoreRate);
            fov = animationInterpolation::Lerp(stageEntryCameraFocusFov_, stageEntryCameraBaseFov_, restoreRate);
        }

        camera->SetInputEnabled(false);
        camera->SetFollowTarget(nullptr);
        camera->SetFollowMode(engine::Camera::FollowMode::kFixedPoint);

        // 描画へ使う視点と、固定点モードが次に使う基準の両方を現在の補間結果へそろえます。
        camera->SetEye(eye);
        camera->SetTarget(target);
        const engine::Vector3 rotation = MakeLookAtEuler(eye, target);
        camera->SetRotation(rotation);
        camera->ConfigFixedPoint(eye, rotation);
        camera->SetFovY(fov);
        camera->Update(deltaTime);
    }

    // 通常更新へ入る前に入口演出が始まるため、全Objectの描画Transformを
    // 演出カメラ確定後に更新します。deltaTimeは進めず、敵やギミックの時間は停止させます。
    if (objectManager_) {
        objectManager_->Update(0.0f);
    }

    // シーンが CPU 粒子を持つ場合は実時間で更新する。GPU 粒子は共通管理側で続けて進める。
    if (particleSystem_) {
        particleSystem_->Update(deltaTime);
    }

    GpuParticleManager::GetInstance()->Update(deltaTime);

    // 復帰区間も終わってから操作とゲート判定を本編へ返します。
    if (stageEntryPresentationTimer_ >= kStageEntryPresentationDuration) {
        FinishStageEntryPresentation();
    }
}

// 演出の一時設定を解除し、ゲート・プレイヤー・カメラの通常制御を再開します。
void GamePlayScene::FinishStageEntryPresentation() {
    // 演出が実行中の場合だけ復帰を行う。開始前や終了済みなら操作状態を触らない。
    if (!stageEntryPresentationActive_) {
        return;
    }

    stageEntryPresentationActive_ = false;
    stageEntryPresentationPending_ = false;
    stageEntryPresentationCompleted_ = true;
    stageEntryPlayerEmergenceStarted_ = false;
    stageEntryPresentationTimer_ = 0.0f;
    stageEntryPresentationRetryTimer_ = 0.0f;

    // 演出中に抑止していた入退場判定を使えるようにし、ゲートの借用参照を外します。
    if (auto* gate = dynamic_cast<GimmickStageGate*>(stageEntryGate_)) {
        gate->SetTransitionEnabled(true);
    }

    stageEntryGate_ = nullptr;

    // 黒帯が閉じる途中で中断しても、開始時の設定値へ戻します。
    if (stageEntryCinemaBarOverrideActive_) {
        // ポストエフェクトの設定にアクセスできる場合だけ、退避した帯の高さを書き戻す。
        if (PostEffect::Params* params = PostEffect::GetInstance()->GetParams()) {
            params->cinemaBarHeight = stageEntryCinemaBarBaseHeight_;
        }

        stageEntryCinemaBarOverrideActive_ = false;
        stageEntryCinemaBarBaseHeight_ = 0.0f;
    }

    // プレイヤーが残っていれば経路アニメーションを止め、演出の準備時に控えた操作可否と待機姿勢へ戻す。
    if (player_) {
        player_->StopGateReturnAnimation(stageEntryHadPlayerControl_);
        player_->SetSlimeAnimationMode(PlayerSlimeAnimator::Mode::kIdle);
        player_->TriggerSlimeImpulse({0.96f, 1.08f, 0.96f}, 0.18f);
    }

    // 演出の画角を解除します。編集時は編集カメラ、プレイ時はプレイヤー追従へ戻します。
    if (engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera()) {
        camera->SetFovY(stageEntryCameraBaseFov_);
#ifdef USE_IMGUI
        // ImGui で自由な編集カメラを選んでいる場合は、そのモードで視点を更新する。
        if (CameraEditor::GetInstance()->IsEditorMode()) {
            camera->SetInputEnabled(true);
            CameraEditor::GetInstance()->Update(player_, false);
        // 通常視点へ戻す場合は、後続のプレイヤー追従設定を使う。
        } else
#endif
        {
            // プレイヤーが存在する場合にだけ追従先と三人称の構図を設定する。
            if (player_) {
                camera->SetFollowTarget(player_);
                camera->SetFollowMode(engine::Camera::FollowMode::kAimable);
                camera->SetRotation(
                    {kStageEntryThirdPersonPitch,
                     std::atan2(stageEntryDirection_.x, stageEntryDirection_.z) + kStageEntryCameraYawOffset, 0.0f});
                camera->SnapToThirdPerson(kStageEntryThirdPersonDistance, kStageEntryThirdPersonHeight,
                                          kStageEntryThirdPersonPitch);
            }

            camera->SetInputEnabled(true);
        }
    }

    // メインカメラを強制描画する指定を外し、以降のカメラ選択を通常経路へ戻します。
    CameraManager::GetInstance()->SetActiveCamera(nullptr);
}
