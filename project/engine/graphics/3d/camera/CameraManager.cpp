#include "CameraManager.h"
#include <cmath>
#include <format>
#include "SceneManager.h"
#include "BaseScene.h"

CameraManager* CameraManager::GetInstance() {
    static CameraManager sInstance;
    return &sInstance;
}

// メインカメラを作り直します。入力先の設定はこの後に行います。
void CameraManager::Initialize() {
    ResetCameraRegions();
    mainCamera_ = std::make_unique<engine::Camera>();
    mainCamera_->Initialize();
    activeCamera_ = nullptr;
    presentationPreviousActiveCamera_ = nullptr;
    mainCameraPresentationActive_ = false;
}

// メインカメラを演出に割り当てる。開始済みか、対象が無い場合は false。
bool CameraManager::BeginMainCameraPresentation() {
    // 演出状態が二重に保存されるのを防ぐため、未開始かつカメラが準備済みの場合だけ進める。
    if (!mainCamera_ || mainCameraPresentationActive_) {
        return false;
    }

    // 通常復帰に必要なカメラ選択と入力許可を、メインカメラを固定する前に保存します。
    presentationPreviousActiveCamera_ = activeCamera_;
    presentationPreviousInputEnabled_ = mainCamera_->IsInputEnabled();
    mainCameraPresentationActive_ = true;
    mainCamera_->SetInputEnabled(false);
    mainCamera_->SetFollowTarget(nullptr);
    mainCamera_->SetLockOnTarget(nullptr);
    mainCamera_->SetFollowMode(engine::Camera::FollowMode::kFixedPoint);
    activeCamera_ = mainCamera_.get();
    return true;
}

void CameraManager::PrepareMainCameraReturn(engine::Object3D* followTarget, const engine::Vector3& eye,
                                            const engine::Vector3& target) {
    // この管理が固定視点を保持している間だけ、復帰の構図と追従先を設定する。
    if (!mainCameraPresentationActive_ || !mainCamera_) {
        return;
    }

    // 視線方向だけを回転へ反映し、通常カメラの追従距離は変更しない。
    const engine::Vector3 direction = target - eye;
    const float horizontal = std::sqrt(direction.x * direction.x + direction.z * direction.z);
    const engine::Vector3 rotation = { std::atan2(-direction.y, horizontal),
                                       std::atan2(direction.x, direction.z), 0.0f };
    mainCamera_->SetEye(eye);
    mainCamera_->SetTarget(target);
    mainCamera_->SetRotation(rotation);
    // 演出終了直前の姿勢を先に渡し、その姿勢から通常追従への復帰を始めます。
    mainCamera_->SetFollowTarget(followTarget);
    mainCamera_->SetFollowMode(engine::Camera::FollowMode::kAimable);
}

// 復帰時に指定の距離・高さ・ピッチ角で三人称視点を即時確定する。
void CameraManager::SnapMainCameraReturn(float distance, float height, float pitch) {
    // メインカメラの演出が所有されている場合だけ、その視点を動かす。
    if (mainCameraPresentationActive_ && mainCamera_) {
        mainCamera_->SnapToThirdPerson(distance, height, pitch);
    }
}

void CameraManager::EndMainCameraPresentation() {
    // 演出が始まっていなければ、退避していないカメラや入力値を復元しない。
    if (!mainCameraPresentationActive_) {
        return;
    }
    // カメラが利用できる場合だけ、演出前の入力許可を適用する。
    if (mainCamera_) {
        mainCamera_->SetInputEnabled(presentationPreviousInputEnabled_);
    }
    // 演出が一時的に選んだカメラを解除し、演出前の選択を戻す。入力の許可も演出前の値を使う。
    activeCamera_ = presentationPreviousActiveCamera_;
    presentationPreviousActiveCamera_ = nullptr;
    mainCameraPresentationActive_ = false;
}

// 領域カメラの選択を行ってから描画用の視点と遮蔽透過データを進める。
void CameraManager::Update(float deltaTime) {
    // メインカメラが作られていない間は、視点と透過を評価できない。
    if (mainCamera_) {
        // シーンの領域を先に確定し、その構図でカメラを更新してから描画側の遮蔽用データを作る。
        const auto* scene = SceneManager::GetInstance()->GetCurrentScene();
        // 現行シーンがあれば、追従対象とカメラ入力の状態を使って配置済みの領域を調べる。
        if (scene) {
            auto* subject = mainCamera_->GetFollowTarget();
            UpdateCameraRegions(scene->GetObjects(), subject, deltaTime,
                SceneManager::GetInstance()->IsPlaying() && subject && mainCamera_->IsInputEnabled(),
                mainCamera_->ConsumeManualLookInput());
        // 現行シーンが無ければ、以前の領域設定を持ち越さない。
        } else ResetCameraRegions();
        mainCamera_->Update(deltaTime);
        UpdateVisibility(deltaTime);
    }
}

void CameraManager::SetInputManager(engine::InputManager* inputManager) {
    // メインカメラ生成後のみ、入力の借用参照を接続する。
    if (mainCamera_) {
        mainCamera_->SetInputManager(inputManager);
    }
}

// 演出は現在のアクティブカメラへ送ります。カメラがなければ要求を破棄します。
void CameraManager::PlayShake(float duration, float amplitude, float frequency, const engine::Vector3& axisWeight) {
    // 現在の描画に使うカメラがある場合だけ、画面揺れを開始する。
    if (engine::Camera* camera = GetActiveCamera()) {
        camera->StartShake(duration, amplitude, frequency, axisWeight);
    }
}

// アクティブカメラへ一時的な視野角変化を送る。amountRadians は縦画角の増減量。
void CameraManager::PlayFovPulse(float duration, float amountRadians, float attackRatio) {
    // 描画対象のカメラがある場合だけ、視野角演出を開始する。
    if (engine::Camera* camera = GetActiveCamera()) {
        camera->StartFovPulse(duration, amountRadians, attackRatio);
    }
}

void CameraManager::ClearPresentationLayers() {
    // カメラがある場合は、そのカメラの追加演出を解除する。
    if (engine::Camera* camera = GetActiveCamera()) {
        camera->ClearPresentationLayers();
    }
}


namespace {
std::string RegionIdentity(const engine::Object3D& object) {
    // 名前変更と同名カメラに影響されない。未保存Objectには一時IDを使う。
    return object.GetPersistentGuid().empty()
        ? std::to_string(reinterpret_cast<uintptr_t>(&object)) : object.GetPersistentGuid();
}
}

// 配置カメラからワールド座標の視点と注視点を求める。構図を解決できない場合は false。
bool CameraManager::ResolveSceneCameraPose(const engine::Object3D& object,
    const std::vector<std::unique_ptr<engine::Object3D>>& objects, engine::Vector3& eye, engine::Vector3& target, bool requireTarget) {
    // カメラ型でない Object からは構図設定を読まない。
    if (!object.IsCameraObject()) return false;
    const auto& settings = object.GetSceneCameraSettings();
    auto find = [&](const std::string& name) -> const engine::Object3D* {
        // 対象名が未設定なら探索せず、参照を空にする。
        if (name.empty()) return nullptr;
        for (const auto& candidate : objects)
            // 同じ名前の有効なオブジェクトがあれば、一覧で最初のものを返す。
            if (candidate && candidate->GetName() == name) return candidate.get();
        return nullptr;
    };
    const auto* origin = &object;
    // 視点の起点がカメラ以外なら、設定名に対応するオブジェクトを必要とする。
    if (settings.eyeSource != SceneCameraEyeSource::kCameraObject) {
        origin = find(settings.eyeObjectName);
        // 指定された視点の起点が見つからなければ、構図の評価に失敗する。
        if (!origin) return false;
    }
    // 別の参照物に対する方向指定のときだけ、起点と参照先の両方を使う。
    if (settings.eyeSource == SceneCameraEyeSource::kSceneObjectRelativeToReference) {
        const auto* reference = find(settings.eyeReferenceObjectName);
        // 相対位置を求める基準が見つからなければ、構図を作れない。
        if (!reference) return false;
        eye = engine::Camera::ResolveRelativeEye(*origin, *reference, settings.eyeOffset);
    // 通常のカメラまたは追従起点では、親子階層を考慮した位置に視点補正を足す。
    } else {
        eye = (origin->GetParent() ? origin->GetWorldPosition() : origin->GetTranslate()) + settings.eyeOffset;
    }
    auto forward = engine::Math::TransformNormal({0,0,1}, origin->GetWorldMatrix());
    const float length = engine::Math::Length(forward);
    // 回転の前方向を単位長へ戻し、参照Objectの拡縮で forwardDistance が変わらないようにします。
    forward = length > 0.0001f ? forward * (1.0f / length) : engine::Vector3{0,0,1};
    // 固定点モードでは、保存した注視座標をそのまま使う。
    if (settings.targetMode == SceneCameraTargetMode::kFixedPoint) {
        target = settings.fixedTarget;
    // Object 注視では、指定した対象の位置に注視補正を足す。
    } else if (settings.targetMode == SceneCameraTargetMode::kSceneObject) {
        const auto* targetObject = find(settings.targetObjectName);
        // 対象必須の呼び出しでは候補から外し、対象が任意の場合だけ前方の仮の注視点へ切り替えます。
        if (!targetObject && requireTarget) return false;
        target = targetObject ? targetObject->GetWorldPosition() + settings.targetOffset
            : eye + forward * settings.forwardDistance + settings.targetOffset;
    // 前方注視では、起点の正規化した前方向へ指定距離を進める。
    } else {
        target = eye + forward * settings.forwardDistance + settings.targetOffset;
    }
    return engine::Math::Length(target - eye) > 0.001f;
}

// 領域の選択・タイマー・描画用の値を初期化する。透過フレームの連番は保持する。
void CameraManager::ResetCameraRegions() {
    const auto serial = visibilityFrame_.serial;
    visibilityFrame_ = {};
    visibilityFrame_.serial = serial;
    // 他の演出が開始済みなら、そちらのOverrideを終了しない。
    if (mainCamera_ && ownsRegionOverride_ && mainCamera_->GetOverrideRevision() == regionOverrideRevision_)
        mainCamera_->EndOverride(0.0f);
    // メインカメラが存在する場合は、領域が指定した投影値も初期化する。
    if (mainCamera_) mainCamera_->ClearRegionProjection();
    ownsRegionOverride_ = false;
    activeRegionId_.clear(); pendingRegionId_.clear(); activeRegionName_.clear();
    pendingRegionTime_ = manualRegionDelay_ = 0.0f;
    routeProgressId_.clear(); routeProgress_ = 0;
    regionBarrierCenter_ = {}; regionBarrierParameters_ = {};
    regionStatus_ = "範囲カメラなし";
}

// 追従者の領域を選び、待機時間と復帰時間を経て視点・投影・透過パラメーターを適用する。
void CameraManager::UpdateCameraRegions(const std::vector<std::unique_ptr<engine::Object3D>>& objects,
    engine::Object3D* subject, float deltaTime, bool enabled, bool manualInput) {
    // メインカメラが無ければ構図の所有や投影を更新できない。
    if (!mainCamera_) return;
    // 区画外や演出中へ、直前の戦闘用設定を持ち越さない。
    regionBarrierCenter_ = {}; regionBarrierParameters_ = {};
    const float frameDeltaTime = std::clamp(deltaTime, 0.0f, 0.1f);
    auto& camera = *mainCamera_;
    if (ownsRegionOverride_ && camera.GetOverrideRevision() != regionOverrideRevision_) {
        // 所有権を失ったら演出を尊重し、保存していたObjectのIDだけを破棄する。
        ownsRegionOverride_ = false; activeRegionId_.clear(); activeRegionName_.clear();
        pendingRegionId_.clear(); pendingRegionTime_ = 0.0f;
    }
    // 自動選択が無効か追従者が無ければ、領域状態を捨てて今回の探索を止める。
    if (!enabled || !subject) {
        ResetCameraRegions();
        // 標的は存在するが別視点を表示している場合は、停止の理由をエディターに示す。
        if (subject && (mainCameraPresentationActive_ || activeCamera_ || camera.IsOverridden())) {
            regionStatus_ = "演出カメラを優先";
        }
        return;
    }
    auto exitRegion = [&] {
        // この領域が所有する上書きの再生中だけ、設定された復帰秒数で終了する。
        if (ownsRegionOverride_ && camera.IsOverridden()) camera.EndOverride(regionExitDuration_);
        activeRegionId_.clear(); activeRegionName_.clear();
    };
    // 自由視点で手動入力を受けたら、領域へ戻るまでの待機時間を更新する。
    if (manualInput && !camera.IsGuidedGameplay()) {
        manualRegionDelay_ = camera.GetVisibilitySettings().manualReturnDelay;
        // 領域による上書きを保持している場合は、直前の手動回転を残して通常視点へ移る。
        if (ownsRegionOverride_) {
            const auto manualRotation = camera.GetRotation();
            camera.ResumeManualView(); camera.SetRotation(manualRotation);
            camera.EndOverride(0.0f); camera.ClearRegionProjection();
            ownsRegionOverride_ = false;
        }
        activeRegionId_.clear(); activeRegionName_.clear(); pendingRegionId_.clear(); pendingRegionTime_ = 0.0f;
    // 手動入力の待機が更新されないフレームでは、残り秒数を減らす。
    } else manualRegionDelay_ = (std::max)(0.0f, manualRegionDelay_ - frameDeltaTime);

    // 固定演出や別カメラが動作中なら優先する。別の所有者の補間が残る間も領域を開始しない。
    if (mainCameraPresentationActive_ || activeCamera_ ||
        (!ownsRegionOverride_ && (camera.IsOverridden() || camera.GetOverrideWeight() > 0.0f))) {
        regionStatus_ = "演出カメラを優先"; return;
    }
    // ボス戦は配置済みの遠景より自動構図を優先する。上の演出優先判定は維持する。
    if (camera.IsBossFramingActive()) {
        // 領域カメラの上書き中なら、ボス構図へ0.6秒で譲る。
        if (ownsRegionOverride_ && camera.IsOverridden()) camera.EndOverride(.6f);
        // 領域の補間量が0に戻ったら、領域側の残りの設定も初期化する。
        if (ownsRegionOverride_ && camera.GetOverrideWeight() <= 0.0f) ResetCameraRegions();
        pendingRegionId_.clear(); pendingRegionTime_ = 0.0f;
        regionStatus_ = "ボス戦の自動構図を優先";
        return;
    }
    // 誘導モードはロックオン中も配置構図を保ち、自由視点だけ追従へ切り替えます。
    if (manualRegionDelay_ > 0.0f || camera.IsAimCameraRequested() || (camera.GetFollowMode() == engine::Camera::FollowMode::kLockOn && !camera.IsGuidedGameplay()) ||
        camera.GetFollowMode() == engine::Camera::FollowMode::kFirstPerson) {
        exitRegion(); pendingRegionId_.clear(); pendingRegionTime_ = 0.0f;
        // 手動で視点を変えた直後は、手動優先が理由になる。
        if (manualRegionDelay_ > 0.0f)
            regionStatus_ = "手動操作を優先";
        // 手動待機でなく照準が要求されている場合は、照準カメラ優先を表示する。
        else if (camera.IsAimCameraRequested())
            regionStatus_ = "照準カメラを優先";
        // 一人称で領域を離れる場合は、状態表示にも視点名を出す。
        else if (camera.GetFollowMode() == engine::Camera::FollowMode::kFirstPerson)
            regionStatus_ = "一人称視点を優先";
        // 残る終了条件は、誘導モード外でのロックオン。
        else
            regionStatus_ = "ロックオンを優先";
        return;
    }

    // 見た目の高さ保持と区画選択を揃える。別の対象を明示した呼び出しは実座標を使う。
    const auto regionPosition = camera.GetFollowTarget() == subject
        ? camera.UpdateFollowReferencePosition() : subject->GetWorldPosition();
    const engine::Object3D* best = nullptr;
    engine::Vector3 bestEye{}, bestTarget{};
    for (const auto& object : objects) {
        // 不可視またはカメラ型でないオブジェクトは候補にしない。
        if (!object || !object->IsCameraObject() || !object->GetIsVisible()) continue;
        const auto& settings = object->GetSceneCameraSettings();
        const auto id = RegionIdentity(*object);
        // 有効な領域で対象を含む場合だけ候補とし、適用中の領域には退出余白を使う。
        if (!settings.enabled || !settings.region.enabled ||
            !settings.region.Contains(regionPosition, id == activeRegionId_)) continue;
        engine::Vector3 eye{}, target{};
        // 視点と注視点を両方解決できない候補は、優先度の比較から除く。
        if (!ResolveSceneCameraPose(*object, objects, eye, target, true)) continue;
        // 同じ優先度の領域が重なるときは適用中の領域を維持し、それ以外はID順で結果を固定します。
        const bool better = !best || settings.region.priority > best->GetSceneCameraSettings().region.priority ||
            (settings.region.priority == best->GetSceneCameraSettings().region.priority &&
             (id == activeRegionId_ || (RegionIdentity(*best) != activeRegionId_ && id < RegionIdentity(*best))));
        // 比較で優先された候補の構図を、後続の補間と切替で使う。
        if (better) { best = object.get(); bestEye = eye; bestTarget = target; }
    }
    const std::string bestId = best ? RegionIdentity(*best) : "";
    float bestFov = best ? best->GetSceneCameraSettings().fovY : camera.GetFovY();
    bool routeApplied = false;
    // 誘導モードで、選んだ領域に経路が定義されている場合は終点カメラとの補間を試す。
    if (best && camera.IsGuidedGameplay() && best->GetSceneCameraSettings().region.routeEnabled) {
        const auto& route = best->GetSceneCameraSettings().region;
        const engine::Object3D* end = nullptr;
        for (const auto& candidate : objects)
            // 開始カメラとは異なるカメラで、終点 GUID と一致する最初の対象を選ぶ。
            if (candidate && candidate.get() != best && candidate->IsCameraObject() &&
                candidate->GetPersistentGuid() == route.endCameraGuid) { end = candidate.get(); break; }
        auto axis = route.pathEnd - route.pathStart; axis.y = 0;
        const float lengthSquared = axis.x*axis.x + axis.z*axis.z;
        engine::Vector3 endEye{}, endTarget{};
        // 終点カメラが有効で構図を解決でき、経路に水平長がある場合だけ経路補間を適用する。
        if (end && end->GetSceneCameraSettings().enabled && lengthSquared > .01f &&
            ResolveSceneCameraPose(*end, objects, endEye, endTarget, true)) {
            const float length = std::sqrt(lengthSquared);
            const auto delta = subject->GetWorldPosition() - route.pathStart;
            // 対象位置をXZ平面の経路直線へ射影し、経路の長さに対する割合へ変換します。
            const float progress = std::clamp((delta.x*axis.x+delta.z*axis.z)/lengthSquared,0.0f,1.0f);
            // 経路の領域が切り替わった場合は、その場の進行割合を平滑化の初期値にする。
            if (routeProgressId_ != bestId) { routeProgressId_ = bestId; routeProgress_ = progress; }
            // 短い往復では道上の基準点を動かさない。余白を越えた分を時間補間する。
            const float slack = camera.GetGuidedDeadZoneWidth()*engine::Math::Length(bestEye-bestTarget)*std::tan(bestFov*.5f)/length;
            const float goal = std::clamp(routeProgress_,(std::max)(0.0f,progress-slack),(std::min)(1.0f,progress+slack));
            routeProgress_ += (goal-routeProgress_)*(1.0f-std::exp(-camera.GetGuidedFramingResponse()*frameDeltaTime));
            // 先読み距離を経路長で割り、視点位置より先を注視します。終点より先へは進めません。
            const float lookProgress = std::clamp(routeProgress_+camera.GetGuidedLookAhead()/length,0.0f,1.0f);
            bestEye = bestEye + (endEye-bestEye)*routeProgress_;
            bestTarget = bestTarget + (endTarget-bestTarget)*lookProgress;
            bestFov += (end->GetSceneCameraSettings().fovY-bestFov)*routeProgress_;
            routeApplied = true;
        }
    }
    // 経路を適用できないフレームでは、次回経路へ戻った際に旧進行値を再利用しない。
    if (!routeApplied) routeProgressId_.clear();
    // 候補が切り替わったら、前候補で蓄積した切替待ちを引き継がない。
    if (bestId != pendingRegionId_) { pendingRegionId_ = bestId; pendingRegionTime_ = 0.0f; }
    // 同じ候補に留まった時間だけで切替を判定し、境界をかすめた瞬間の視点変更を抑えます。
    pendingRegionTime_ += frameDeltaTime;
    const float delay = best ? best->GetSceneCameraSettings().region.activationDelay : 0.2f;
    // 現在の領域と異なる候補が、必要な秒数だけ続いたときに切り替える。
    if (bestId != activeRegionId_ && pendingRegionTime_ >= delay) {
        // 新しい候補がある場合は、その構図、補間時間、追従方式で上書きを開始する。
        if (best) {
            const auto& settings = best->GetSceneCameraSettings();
            engine::Camera::CameraOverrideParams params;
            params.duration = settings.blendInDuration;
            params.easing = static_cast<engine::Camera::OverrideEasing>(settings.easing);
            params.fixedEyePos = bestEye; params.fixedTargetPos = bestTarget;
            params.trackTargetX = params.trackTargetZ = false;
            // 自機への追従だけは、通常カメラと同じジャンプ高さの保持を使う。
            params.trackTargetY = settings.targetMode == SceneCameraTargetMode::kSceneObject &&
                settings.targetObjectName == subject->GetName();
            params.trackEyeY = settings.eyeSource != SceneCameraEyeSource::kCameraObject &&
                settings.eyeObjectName == subject->GetName();
            params.eyeFollowMode = static_cast<engine::Camera::OverrideFollowMode>(settings.eyeFollowMode);
            params.eyeFollowResponse = settings.eyeFollowResponse;
            params.targetFollowMode = static_cast<engine::Camera::OverrideFollowMode>(settings.targetFollowMode);
            params.targetFollowResponse = settings.targetFollowResponse;
            camera.StartOverride(params);
            regionOverrideRevision_ = camera.GetOverrideRevision(); ownsRegionOverride_ = true;
            activeRegionId_ = bestId; activeRegionName_ = best->GetName();
            regionExitDuration_ = settings.blendOutDuration;
        // 候補が途切れた場合は、現在の領域の退出時間を使って通常視点へ戻す。
        } else exitRegion();
    }
    // 同じ領域の上書きを保持している間にだけ、移動する視点・投影・バリア設定を更新する。
    if (best && bestId == activeRegionId_ && ownsRegionOverride_) {
        const auto& settings = best->GetSceneCameraSettings();
        const auto& region = settings.region;
        // 手前バリアの透過が有効な領域では、描画に渡す床高や透明度を埋める。
        if (region.clearForegroundBarriers) {
            regionBarrierCenter_ = {region.center.x, region.center.y, region.center.z, 1.0f};
            // 正の境界幅は旧シェーダーと共通の値。格子を残す領域だけ、負値でその濃さを渡します。
            const float barrierEdgeParameter = region.barrierLatticeOpacity > 0.0f
                ? -region.barrierLatticeOpacity : 1.0f;
            regionBarrierParameters_ = {region.barrierFloorHeight, region.barrierEdgeHeight,
                                        region.barrierOpacity, barrierEdgeParameter};
        }
        camera.UpdateRegionPose(bestEye, bestTarget);
        camera.SetRegionProjection(bestFov, settings.nearClip, settings.farClip, subject);
    }
    if (ownsRegionOverride_ && !camera.IsOverridden() && camera.GetOverrideWeight() <= 0.0f) {
        // 復帰ブレンド中は射影設定を保持し、Overrideの影響が消えてから元の射影へ戻します。
        camera.ClearRegionProjection(); ownsRegionOverride_ = false;
    }
    // 判定済みの候補・時刻を表示に使い、エディター側では範囲判定をやり直さない。
    // 候補が現在の領域と異なる間は、次の切替までの残り時間を表示する。
    if (bestId != activeRegionId_) {
        const std::string from = activeRegionName_.empty() ? "通常追従" : activeRegionName_;
        const std::string to = best ? best->GetName() : "通常追従";
        const float remainingSeconds = (std::max)(0.0f, delay - pendingRegionTime_);
        regionStatus_ = std::format("{} → {}  |  切替まで {:.2f} 秒", from, to, remainingSeconds);
    // 以前の領域の上書きが終了し、復帰補間だけが残っている場合は通常への移行を表示する。
    } else if (ownsRegionOverride_ && !camera.IsOverridden()) {
        regionStatus_ = "通常追従へブレンド中";
    // 有効な領域名があれば、上書きの混合率に応じて適用中か切替中かを出す。
    } else if (!activeRegionName_.empty()) {
        regionStatus_ = (camera.GetOverrideWeight() < 1.0f ? "ブレンド中 → " : "適用中: ") + activeRegionName_;
    // 候補と適用領域がどちらも空の通常状態では、範囲外として表示する。
    } else {
        regionStatus_ = "通常追従 / 範囲外";
    }
}

// 本編の追従者を遮るオブジェクト用に、今回の画面へ渡す透過データを準備する。
void CameraManager::UpdateVisibility(float deltaTime) {
    ++visibilityFrame_.serial;
    visibilityFrame_.enabled = false;
    visibilityFrame_.parameters = {};
    visibilityFrame_.foregroundBarrierCenter = {};
    visibilityFrame_.foregroundBarrierParameters = {};
    auto* camera = GetActiveCamera();
    // プレイ中のメインカメラだけで使う。カット中や別ビューでは無効のまま返す。
    if (!camera || camera != mainCamera_.get() || mainCameraPresentationActive_ ||
        !SceneManager::GetInstance()->IsPlaying()) return;
    auto* subject = camera->GetFollowTarget();
    const auto& settings = camera->GetVisibilitySettings();
    // 追従対象が無い、遮蔽設定が無効、別演出の上書き中、または一人称の場合は必要な透過円を作らない。
    if (!subject || !settings.enabled || (camera->IsOverridden() && !ownsRegionOverride_) || camera->GetFollowMode() == engine::Camera::FollowMode::kFirstPerson) return;
    // 物理境界の中心を使い、巨大なaimHeightや装飾のサイズに透過円を引きずらせない。
    const engine::Aabb bounds = subject->GetAabb();
    const auto extent = bounds.max - bounds.min;
    const bool validBounds = extent.y > 0.01f && extent.y < 100.0f;
    const auto center = validBounds ? (bounds.min + bounds.max) * 0.5f
        : subject->GetWorldPosition() + engine::Vector3{0, 0.8f, 0};
    const float radius = (std::max)(settings.radius, validBounds ? extent.y * 0.65f : 0.8f);
    visibilityFrame_.centerRadius = {center.x, center.y, center.z, radius};
    visibilityFrame_.parameters = {1.0f - settings.minimumOpacity, settings.feather,
        validBounds ? bounds.min.y : subject->GetWorldPosition().y,
        validBounds ? (std::max)(0.15f, (std::max)(extent.x, extent.z) * 0.5f) : 0.3f};
    visibilityFrame_.deltaTime = std::isfinite(deltaTime) ? std::clamp(deltaTime, 0.0f, 0.1f) : 0.0f;
    visibilityFrame_.foregroundBarrierCenter = regionBarrierCenter_;
    // カメラの切替量に応じて手前バリアの透過を弱め、領域境界で突然切り替わらないようにします。
    visibilityFrame_.foregroundBarrierCenter.w *= camera->GetOverrideWeight();
    visibilityFrame_.foregroundBarrierParameters = regionBarrierParameters_;
    visibilityFrame_.enabled = true;
}

// 遮蔽物の型・親階層・明示設定から、透過の対象にできるかを返す。
bool CameraManager::CanFadeObject(const engine::Object3D* object, const engine::Camera* camera) const {
    // 今フレーム用の透過情報と描画対象が揃い、一覧が有効なメインビューでのみ判定する。
    if (!visibilityFrame_.enabled || !object || camera != mainCamera_.get() ||
        (activeCamera_ && activeCamera_ != camera)) return false;
    const bool explicitlyFade = object->GetCameraOcclusionMode() == engine::Object3D::CameraOcclusionMode::kFade;
    bool automatic = true;
    for (const auto* current = object; current; current = current->GetParent()) {
        const auto kind = current->GetClassName();
        // 自機と明示的な透過禁止は常に維持する。敵本体の透過はボス戦の視界確保時だけ許可する。
        if (current == camera->GetFollowTarget() || kind == "Player" || kind == "Item" || kind == "Camera" ||
            (current == object && kind == "Enemy" && !camera->IsBossFramingActive()) ||
            current->GetCameraOcclusionMode() == engine::Object3D::CameraOcclusionMode::kKeepVisible) return false;
        // 演出物と敵の装飾は自動透過から除く。ボス構図以外では、親階層に敵がいる場合も除く。
        if (kind == "Effect" || kind == "EnemyVisualPart" || (!camera->IsBossFramingActive() && kind == "Enemy")) automatic = false;
    }
    return explicitlyFade || automatic;
}
