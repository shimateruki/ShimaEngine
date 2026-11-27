#include "game/scene/gameplay/GamePlayScene.h"
#include <numbers>
#include "game/actor/enemy/boss/false_king/EnemyFalseKingSlime.h"
#include "game/crown/CrownFinaleTuning.h"
#include "CameraManager.h"
#include "InputManager.h"
#include "SceneManager.h"
#include "engine/editor/graphics/camera/CameraEditor.h"
#include <algorithm>

namespace {
constexpr uint32_t kCrownMovieHiddenReason = 1u << 8;
}

bool GamePlayScene::BeginCrownMoviePreview(bool defeat) {
    // プレビューを開いている場合は、元の配置やカメラを重ねて退避せず、そのまま成功を返す。
    if (crownMoviePreview_.active)
        return true;
    // 編集中の配置を借りるため、すでにプレイ中か本編演出が準備済みの場合は始めません。
    if (!player_ || crownFinale_.prepared || !GetSceneManager() || GetSceneManager()->IsPlaying())
        return false;
    EnemyFalseKingSlime* sourceBoss = nullptr;
    bool hasFloor = false;

    for (const auto& object : GetObjects()) {
        // エディター内部の実体とリプレイ上で消えている実体を、元の役者や床の候補から外す。
        if (object->IsEditorInternal() || object->IsReplayRemoved())
            continue;
        // 偽王スライムを見つけたら、専用のプレビュー役者を複製する元として控える。
        if (auto* boss = dynamic_cast<EnemyFalseKingSlime*>(object.get()))
            sourceBoss = boss;
        hasFloor |= object->GetName().find("Stage3_HighCrown_ArenaFloor_") != std::string::npos;
    }

    // 複製元の役者と床を確認し、見つかった場合だけ元の編集カメラを退避します。
    engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();
    // 偽王、闘技場の床、メインカメラのどれかが無ければ、元の配置を変更せず開始を見送る。
    if (!sourceBoss || !hasFloor || !camera)
        return false;
    auto& p = crownMoviePreview_;
    p.defeat = defeat;
    p.originalPlayer = player_;
    p.previousCameraOverride = CameraManager::GetInstance()->GetActiveCameraOverride();
    p.camera = std::make_unique<engine::Camera>();
    p.camera->Initialize();
    p.camera->SetClipRange(camera->GetNearClip(), camera->GetFarClip());
    p.camera->SetInputEnabled(false);
    CameraEditor::GetInstance()->SetEditorStateSaveBlocker(1u << 3, true);
    p.eye = camera->GetEye();
    p.target = camera->GetTargetPoint();
    p.rotation = camera->GetRotation();
    p.followMode = camera->GetFollowMode();
    p.follow = camera->GetFollowTarget();
    p.fov = camera->GetFovY();
    std::vector<std::unique_ptr<engine::Object3D>> copies;
    EnemyFalseKingSlime* previewBoss = nullptr;

    // 生成前に走査を終え、元のScene保存値から表示だけを一時的に分離します。
    for (const auto& object : GetObjects()) {
        // 編集用の内部物、リプレイから削除されたもの、カメラを一時非表示の対象から外す。
        if (object->IsEditorInternal() || object->IsReplayRemoved() || object->IsCameraObject())
            continue;
        p.hiddenObjects.push_back(object.get());
        // 元プレイヤーは隠す対象だけに含め、プレビュー側では別途新規作成する。
        if (object.get() == player_)
            continue;
        // 複製対象は偽王とStage3_HighCrown_で始まる闘技場の配置に限る。
        if (object.get() != sourceBoss && object->GetName().rfind("Stage3_HighCrown_", 0) != 0)
            continue;
        auto copy = object->Clone();
        // 複製できない配置物は飛ばし、ボスを複製できたかは走査後に確認する。
        if (!copy)
            continue;
        // 複製は保存・操作・実戦の当たり判定から外し、プレビューの表示に限定します。
        copy->ClearEditorHiddenReasons();
        copy->SetEditorInternal(true);
        copy->SetIsLocked(true);
        copy->SetEventId(-1);
        copy->SetTargetId(-1);
        copy->SetCollisionAttribute(0);
        copy->SetCollisionMask(0);

        // 偽王の複製だけは演出の役者として控え、待機状態の表示を解除する。
        if (object.get() == sourceBoss) {
            previewBoss = dynamic_cast<EnemyFalseKingSlime*>(copy.get());
            copy->SetIsVisible(true);
        }

        p.objects.push_back(copy.get());
        copies.push_back(std::move(copy));
    }

    // ボスの複製に失敗した場合は、配置を隠す前にカメラの保存制限を解除します。
    if (!previewBoss) {
        CameraEditor::GetInstance()->SetEditorStateSaveBlocker(1u << 3, false);
        p = CrownMoviePreviewState{};
        return false;
    }

    auto player = std::make_unique<Player>();
    player->Initialize(GetObject3DCommon(), engine::InputManager::GetInstance(), GetParticleSystem(), GetSpriteCommon());

    // Initializeだけでは本体モデルが設定されません。実Playerの基本形を明示します。
    player->SetModel("Characters/slimeBody");
    player->SetColor({1.0f,1.0f,1.0f,1.0f});
    player->SetName("__Editor_CrownMovie_Player");
    player->SetClassName("Player");
    player->SetEditorInternal(true);
    player->SetIsLocked(true);
    player->SetColliderConfig(player_->GetColliderConfig());
    player->SetCollisionAttribute(0);
    player->SetCollisionMask(0);
    player->SetMaterialType(25);
    player->SetEnableLighting(true);
    player->SetIsControlActive(false);
    player->SetScale(player_->GetScale());
    player->SetGrounded(true);

    // 登場操作を毎回やり直さず、第二形態の中央位置で演出を開始します。
    const auto& tuning = CrownFinaleTuning::Get();
    const engine::Vector3 bossPosition = sourceBoss->GetTranslate() + engine::Vector3{0.0f, 0.0f, tuning.sealCenterOffsetZ};
    previewBoss->SetTranslate(bossPosition);
    previewBoss->SetRotation(sourceBoss->GetRotation());
    previewBoss->UpdateWorldMatrix(true);
    player->SetTranslate({0.0f, 0.82f, 12.0f});
    player->SetMoveYaw(std::numbers::pi_v<float>);
    player->UpdateWorldMatrix(true);
    player_ = player.get();
    p.objects.push_back(player.get());
    copies.push_back(std::move(player));
    p.active = true;
    CameraManager::GetInstance()->SetActiveCamera(p.camera.get());

    for (engine::Object3D* object : p.hiddenObjects)
        object->SetEditorHiddenReason(kCrownMovieHiddenReason, true);

    // Editor更新の安全な境界で登録し、開始時の床・壁の走査から参照可能にします。
    for (auto& copy : copies)
        AddObject(std::move(copy));
    objectManager_->ApplyPendingChanges();

    // 本編と同じ継承処理で再生を始め、準備失敗時は隔離した表示とカメラを元に戻します。
    if (!StartCrownFinale(previewBoss)) {
        EndCrownMoviePreview();
        return false;
    }
    // 撃破演出を確認する場合は、先に継承を完了させて撃破開始時の闘技場を組み立てる。
    if (defeat) {
        // 継承完了時の実際の闘技場を用意し、同じ配置・能力で撃破を確認します。
        UpdateCrownFinale(0, crownFinale_.duration);
        FinishCrownFinale(true);
        CameraManager::GetInstance()->SetActiveCamera(p.camera.get());

        // 撃破演出を開始できないときは、プレビュー全体を終了して編集状態へ戻す。
        if (!StartCrownDefeat(previewBoss)) {
            EndCrownMoviePreview();
            return false;
        }
    }

    // 最終闘技場も保存対象へ混ざらない、プレビュー専用の複製です。
    StepCrownMoviePreview(0.0f);
    return true;
}

void GamePlayScene::RewindCrownMoviePreview() {
    // 敗北演出と戴冠演出で時計が異なるため、現在の分岐だけを先頭へ戻す。
    if (crownMoviePreview_.active && crownMoviePreview_.defeat) {
        crownDefeatVfxPlayer_.SetTime(0.0f, false);
        UpdateCrownDefeat(0);
        return;
    }

    // 継承プレビューが進行中の場合だけ、能力と演出姿勢を初期値へ戻します。
    if (!crownMoviePreview_.active || !crownFinale_.active)
        return;
    auto& s = crownFinale_;
    player_->ClearCrownPower();
    player_->EndCinematicPullVisual();
    player_->SetModel(s.playerModel);
    player_->SetMaterialType(s.playerMaterial);
    player_->SetColor(s.playerColor);
    player_->ClearPortalClip();
    player_->BeginCinematicLock();
    player_->SetScale(s.playerScale);
    player_->SetMoveYaw(s.playerYaw);
    auto rotation = player_->GetRotation();
    rotation.x = rotation.z = 0.0f;
    player_->SetRotation(rotation);
    player_->SetTranslate(s.start);
    player_->SetVelocity({});
    s.duration = GetCrownFinaleTiming().movieDuration;
    s.bodyFormed = false;
    s.granted = s.landed = false;

    // 以前の再生で変化した床・壁などを保存済みの姿勢と判定に戻してから0秒を描画します。
    for (const auto& pose : s.originalObjects) {
        pose.object->SetTranslate(pose.position);
        pose.object->SetRotation(pose.rotation);
        pose.object->SetCollisionAttribute(pose.attribute);
        pose.object->SetCollisionMask(pose.mask);
        pose.object->SetIsVisible(pose.visible);
    }

    UpdateCrownFinale(0.0f, 0.0f);
    StepCrownMoviePreview(0.0f);
}

void GamePlayScene::StepCrownMoviePreview(float deltaTime) {
    // 撃破と継承のうち開いている方だけ時間を進め、複製の描画定数を更新します。
    if (crownMoviePreview_.active && crownMoviePreview_.defeat) {
        UpdateCrownDefeat(deltaTime);

        for(engine::Object3D* object:crownMoviePreview_.objects)
            object->UpdateWorldMatrix(true);

        for(engine::Object3D* object:crownFinale_.generatedObjects)
            object->UpdateWorldMatrix(true);
        return;
    }

    // プレビューと継承演出の両方が動いている場合に限り、継承の時計を進める。
    if (!crownMoviePreview_.active || !crownFinale_.active)
        return;
    UpdateCrownFinale(deltaTime);

    // Object3D::Updateは描画定数を確定しません。カメラの確定後に全モデルへ転送します。
    for (engine::Object3D* object : crownMoviePreview_.objects)
        object->UpdateWorldMatrix(true);

    for (engine::Object3D* object : crownFinale_.generatedObjects)
        object->UpdateWorldMatrix(true);
}

void GamePlayScene::EndCrownMoviePreview() {
    // プレビューで変更したワールド状態を片付け、通常の更新へ戻す。
    auto& p = crownMoviePreview_;
    // プレビューを既に閉じている場合は、借用した参照を改めて復元しない。
    if (!p.active)
        return;
    // プレビュー用の演出を停止し、複製がまだ有効な間に演出側が借りた参照を片付けます。
    ResetCrownDefeat();
    ResetCrownFinaleWorld();
    objectManager_->ApplyPendingChanges();
    player_ = p.originalPlayer;

    // 元の配置ではプレビュー由来の非表示理由だけ外し、別の編集操作の表示設定を保ちます。
    for (engine::Object3D* object : p.hiddenObjects)
        object->SetEditorHiddenReason(kCrownMovieHiddenReason, false);

    // 複製はすぐ非表示にし、管理側へ削除を要求します。
    for (engine::Object3D* object : p.objects) {
        object->SetIsVisible(false);
        object->SetReplayRemoved(true);
        RequestRemoveObject(object);
    }

    // 元の追従先・姿勢・画角を戻してから、編集時のカメラ選択と保存を再開します。
    if (engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera()) {
        camera->SetFollowTarget(p.follow);
        camera->SetFollowMode(p.followMode);
        camera->SetEye(p.eye);
        camera->SetTarget(p.target);
        camera->SetRotation(p.rotation);
        // 固定点追従だった場合は、固定点用に保持する位置と回転も戻す。
        if (p.followMode == engine::Camera::FollowMode::kFixedPoint)
            camera->ConfigFixedPoint(p.eye, p.rotation);
        camera->SetFovY(p.fov);
        camera->SetInputEnabled(true);
        camera->Update();
    }

    CameraManager::GetInstance()->SetActiveCamera(p.previousCameraOverride);
    CameraEditor::GetInstance()->SetEditorStateSaveBlocker(1u << 3, false);
    objectManager_->ApplyPendingChanges();
    p = CrownMoviePreviewState{};
}
