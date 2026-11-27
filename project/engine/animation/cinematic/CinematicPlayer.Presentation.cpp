#define NOMINMAX
#include <cstdint>
#include "engine/animation/cinematic/CinematicPlayer.h"
#include "BaseScene.h"
#include "SceneManager.h"
#include "CameraManager.h"
#include "PostEffect.h"
#include "Easing.h"
#include "engine/utility/math/AnimationInterpolation.h"
#include "GpuParticleManager.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <unordered_set>

namespace engine {


CinematicPlayer::SpringState CinematicPlayer::GetSpringState(const std::string& name) const {
    // 指定部品をまだ積分していない場合は、未初期化の状態を返す。
    const auto found = springStates_.find(name);
    return found == springStates_.end() ? SpringState{} : found->second;
}

void CinematicPlayer::RestoreSpringState(const std::string& name, const SpringState& state) {
    springStates_[name] = state;
}

CinematicTransformKey CinematicPlayer::UpdateSpringAttachment(const std::string& name, const SpringInput& input, float deltaTime) {
    const CinematicSpringSettings defaults;
    // 素材に部品名がなければ内蔵の係数を使います。積分済みの位置と速度は係数とは別に名前ごとに保持します。
    const auto* settings = &defaults;
    // 演出データがある場合だけ、部品名に対応する設定を検索する。
    if (sequence_) {
        const auto found = sequence_->presentation.springs.find(name);
        // 素材に名前付きの係数が登録されていれば、その係数で部品のばねを動かす。
        if (found != sequence_->presentation.springs.end()) settings = &found->second;
    }
    const auto& physics = *settings;
    auto& state = springStates_[name];
    const auto& targetPosition = input.targetPosition;
    const float motionEnergy = std::clamp(input.motionEnergy, 0.0f, 1.0f);
    // 一周分を加減して角度差を±πに収める。正負の境界をまたいでも短い方向へ回す。
    auto NormalizeYaw = [](float yaw) {
        constexpr float kPi = std::numbers::pi_v<float>;
        while (yaw > kPi) yaw -= 2.0f * kPi;
        while (yaw < -kPi) yaw += 2.0f * kPi;
        return yaw;
    };
    CinematicTransformKey result;
    // この名前の部品を初めて動かすときだけ、初期変位と初速を基準姿勢へ加える。
    if (!state.initialized) {
        state.position = targetPosition + physics.initialPositionOffset;
        state.velocity = input.forward * physics.initialForwardVelocity + engine::Vector3{0.0f, physics.initialVerticalVelocity, 0.0f};
        state.rotation = input.baseRotation + physics.initialRotationOffset;
        state.angularVelocity = {0.0f, 0.0f, physics.initialRollVelocity};
        state.initialized = true;
    }

    // 長いフレームを制限し、必要なら小刻みに積分して追従部品の揺れを安定させる。
    const float frameDeltaTime = std::clamp(deltaTime, 0.0f, 1.0f / 30.0f);
    // 1フレームで扱う時間は最大1/30秒とし、分割後の積分幅が1/120秒を越えない分割数を選びます。
    const std::int32_t substepCount = std::max(1, static_cast<std::int32_t>(std::ceil(frameDeltaTime / (1.0f / 120.0f))));
    const float substep = frameDeltaTime / static_cast<float>(substepCount);

    for (std::int32_t step = 0; step < substepCount; ++step) {
        const engine::Vector3 springDelta = targetPosition - state.position;
        // 目標までの距離にばねの係数を掛け、速度に比例する減衰を引きます。速度を先に更新してから位置を進めます。
        const engine::Vector3 acceleration = springDelta * (physics.positionStiffness + motionEnergy * 8.0f) -
                                     state.velocity * physics.positionDamping;
        state.velocity = state.velocity + acceleration * substep;
        state.position = state.position + state.velocity * substep;
    }

    // バネが跳ね過ぎても追従先から大きく外れないよう、水平と上下の偏差を別々に制限する。
    engine::Vector3 offset = state.position - targetPosition;
    const float maxPlanarOffset = physics.maxPlanarOffset + motionEnergy * 0.05f;
    const float planarLength = std::sqrt(offset.x * offset.x + offset.z * offset.z);

    // 水平距離が上限を超え、割り算に使える大きさがあるときだけ、境界に戻して横速度を減衰させる。
    if (planarLength > maxPlanarOffset && planarLength > 0.0001f) {
        // 上限と現在距離の比をXZ両軸に掛け、向きを保ったまま円の境界へ戻します。
        const float rate = maxPlanarOffset / planarLength;
        offset.x *= rate;
        offset.z *= rate;
        state.velocity.x *= 0.55f;
        state.velocity.z *= 0.55f;
    }

    const float unclampedY = offset.y;
    offset.y = std::clamp(offset.y, -physics.maxSink, physics.maxLift + motionEnergy * 0.04f);

    // 上下限で位置を切り詰めたときだけ鉛直速度を減衰させ、次の更新で同じ向きに飛び出しにくくします。
    if (offset.y != unclampedY) {
        state.velocity.y *= 0.32f;
    }

    result.position = targetPosition + offset;
    state.position = result.position;

    // 追従部品の位置ずれと速度から前後・左右の傾きを求め、基準対象の傾きも加える。
    const float forwardOffset = engine::Math::Dot(offset, input.forward);
    const float rightOffset = engine::Math::Dot(offset, input.right);
    // 位置差はワールド距離、速度はワールド距離毎秒です。それぞれ前後・左右の軸へ射影して傾きへ換算します。
    const float forwardSpeed = engine::Math::Dot(state.velocity, input.forward);
    const float rightSpeed = engine::Math::Dot(state.velocity, input.right);
    engine::Vector3 targetRotation = input.baseRotation;
    targetRotation.x += input.sourceRotationDelta.x * physics.sourceTiltFollow -
                        forwardOffset * physics.tiltFromOffset - forwardSpeed * physics.tiltFromVelocity;
    targetRotation.z += input.sourceRotationDelta.z * physics.sourceTiltFollow +
                        rightOffset * physics.tiltFromOffset + rightSpeed * physics.tiltFromVelocity;
    targetRotation.x =
        std::clamp(targetRotation.x, input.baseRotation.x - 0.52f, input.baseRotation.x + 0.52f);
    targetRotation.z =
        std::clamp(targetRotation.z, input.baseRotation.z - 0.52f, input.baseRotation.z + 0.52f);
    // ラジアンのヨー差を±πの範囲へ畳み、角度の境界で一周近く戻らないようにします。
    const float targetYaw = input.baseRotation.y + NormalizeYaw(input.sourceRotationDelta.y);
    targetRotation.y = state.rotation.y + NormalizeYaw(targetYaw - state.rotation.y);

    // Yaw差を最短角へ畳んでから回転バネを積分する。
    for (std::int32_t step = 0; step < substepCount; ++step) {
        engine::Vector3 rotationDelta = targetRotation - state.rotation;
        rotationDelta.y = NormalizeYaw(rotationDelta.y);
        const engine::Vector3 acceleration =
            rotationDelta * physics.angularStiffness - state.angularVelocity * physics.angularDamping;
        state.angularVelocity = state.angularVelocity + acceleration * substep;
        state.rotation = state.rotation + state.angularVelocity * substep;
    }

    result.rotation = state.rotation;

    // 下向きの偏差と速度が大きいときだけ、追従部品を少し潰して着地感を出す。
    const float compression =
        std::clamp(-offset.y * 2.2f + std::max(0.0f, -state.velocity.y) * 0.045f, 0.0f, 0.35f);
    result.scale.x *= 1.0f + compression * physics.compressionScale;
    result.scale.y *= 1.0f - compression * physics.compressionScale * 0.72f;
    result.scale.z *= 1.0f + compression * physics.compressionScale;
    return result;
}

void CinematicPlayer::FrameCameraForOverlay(std::span<const Vector3> points, float fov, float aspect, float margin,
    float reveal, float uiCenterX, float subjectOffset, Vector3& eye, Vector3& target) {
    // 結果へ切り替える間だけ見下ろしを緩め、海と足元の両方が画面に入りやすい角度にする。
    const auto direction = target - eye;
    eye.y += direction.y * (0.36f * reveal);
    engine::CinematicPlayer::FitCameraToPoints(points, fov, aspect, margin, margin, {},
                                               {0.0f, 0.0f, 1.0f}, eye, target);
    const auto forward = engine::Math::Normalize(target - eye);
    const auto right = engine::Math::Normalize(engine::Math::Cross({0.0f, 1.0f, 0.0f}, forward));
    const float tanX = std::tan(fov * 0.5f) * aspect;
    const float left = -1.0f + 2.0f * margin;
    // UIの左端より手前でモデルを止め、編集でカードを左に動かしたときにも余白を広げる。
    const float resultRight = std::clamp(2.0f * uiCenterX - 1.0f - 0.45f, -0.34f, 0.10f);
    const float rightLimit = std::max(left + 0.02f, animationInterpolation::Lerp(-left, resultRight, reveal));
    const float adjustedOffset = std::clamp(subjectOffset * 0.033f, -0.12f, 0.12f);
    const float screenCenter = std::clamp((left + rightLimit) * 0.5f + adjustedOffset * reveal, left + 0.005f, rightLimit - 0.005f);
    float distance = engine::Math::Length(target - eye);

    // 投影後の左右端が安全領域に入る距離まで後退し、注視点を同じ方向へ平行移動する。
    for (const auto& point : points) {
        const auto local = point - target;
        const float x = engine::Math::Dot(local, right);
        const float z = engine::Math::Dot(local, forward);
        distance = (std::max)({distance,
            (x - rightLimit * tanX * z) / ((rightLimit - screenCenter) * tanX),
            (left * tanX * z - x) / ((screenCenter - left) * tanX)});
    }
    target = target - right * (screenCenter * tanX * distance);
    eye = target - forward * distance;
}

void CinematicPlayer::FitCameraToPoints(std::span<const engine::Vector3> points, float fov, float aspect,
    float margin, float topMargin, const engine::Vector3& centerOffset,
    const engine::Vector3& fallbackForward, engine::Vector3& eye, engine::Vector3& target) {
    // 対象点がない、または画角か投影比率が有限の正数でない場合は、位置を変えずに終了する。
    if (points.empty() || !std::isfinite(fov) || !std::isfinite(aspect) || fov <= 0 || aspect <= 0) return;
    margin = std::clamp(margin, 0.0f, 0.45f);
    topMargin = std::clamp(topMargin, margin, 0.95f - margin);
    // 全点のワールド座標の最小・最大から、構図の基準にする包囲箱の中心を求めます。
    auto low=points.front(),high=points.front();
    for (const auto& p:points) {
        low={(std::min)(low.x,p.x),(std::min)(low.y,p.y),(std::min)(low.z,p.z)};
        high={(std::max)(high.x,p.x),(std::max)(high.y,p.y),(std::max)(high.z,p.z)};
    }
    auto direction=target-eye;
    // 注視点とカメラがほぼ同じ位置で向きが取れない場合は、基準の水平方向と下向き成分から初期方向を作る。
    if (engine::Math::Length(direction)<0.001f) direction=fallbackForward*-1.0f+engine::Vector3{0,-0.25f,0};
    auto forward=engine::Math::Normalize(direction);
    // 視線がほぼ鉛直なら、基準の水平方向を加えて右軸を作れる向きに修正する。
    if (std::abs(forward.y)>0.95f) forward=engine::Math::Normalize(fallbackForward*-0.5f+engine::Vector3{0,forward.y,0});
    // ほぼ真上・真下を向くと外積が小さくなるため、直前で向きを補正してカメラの右・上軸を作ります。
    const auto right=engine::Math::Normalize(engine::Math::Cross({0,1,0},forward));
    const auto up=engine::Math::Cross(forward,right);
    auto center=(low+high)*0.5f;
    // 垂直画角はラジアンです。半画角の正接は奥行き1あたりの縦の半幅です。横の半幅はアスペクト比を掛けて求めます。
    const float tanY=std::tan(fov*0.5f);
    const float tanX=tanY*aspect;
    center=center+centerOffset;
    // 画面に対する余白の割合を、中心0、上下端±1の正規化座標に置き換えます。
    const float top=1.0f-2.0f*topMargin;
    const float bottom=-1.0f+2.0f*margin;
    const float middle=(top+bottom)*0.5f;
    const float horizontal=1.0f-2.0f*margin;
    float distance=engine::Math::Length(target-eye);
    for (const auto& p:points) {
        const auto delta=p-center;
        const float x=engine::Math::Dot(delta,right),y=engine::Math::Dot(delta,up),z=engine::Math::Dot(delta,forward);
        // 各点について最小の前方距離と左右・上下の画角条件を逆算し、全条件を満たす最大値を採用します。
        distance=(std::max)({distance,0.25f-z,
            std::abs(x)/(tanX*horizontal)-z,
            (y-top*tanY*z)/(tanY*(top-middle)),
            (-y+bottom*tanY*z)/(tanY*(middle-bottom))});
    }
    // 開始時の向きを保持しつつ対象の中心へ寄せ、結果UIのための上側余白を確保する。
    target=center-up*(middle*tanY*distance);
    eye=target-forward*distance;
}

std::function<void(const std::string&, float, bool)> CinematicPlayer::kPlayAudio;
std::function<void(const std::string&)> CinematicPlayer::kStopAudio;
std::function<std::vector<CinematicPlayer::AudioEntry>()> CinematicPlayer::sAudioEntries;

namespace {
engine::Vector3 Product(const engine::Vector3& a, const engine::Vector3& b) { return {a.x*b.x, a.y*b.y, a.z*b.z}; }
engine::Vector3 Rotate(const engine::Vector3& v, float yaw) { return CinematicAnchorPosition(v, {}, yaw); }
CinematicViewKey ViewAt(const std::vector<CinematicViewKey>& keys, float time) {
    return SampleCinematicKeys(keys, time, [](const auto& a, const auto& b, float t) {
        auto k=a;
        // 演出時刻の秒を前後のキー間の割合へ変えます。数値項目は滑らかに補間し、参照名は先頭キーの設定を保持します。
        t = std::clamp((t-a.time)/std::max(0.000001f,b.time-a.time),0.0f,1.0f);
        k.focusMix=engine::Math::Lerp(a.focusMix,b.focusMix,t); k.focusOffset=engine::Math::Lerp(a.focusOffset,b.focusOffset,t);
        k.scaledFocusOffset=engine::Math::Lerp(a.scaledFocusOffset,b.scaledFocusOffset,t);
        k.eyeOffset=engine::Math::Lerp(a.eyeOffset,b.eyeOffset,t); k.jitter=engine::Math::Lerp(a.jitter,b.jitter,t);
        k.fov=engine::Math::Lerp(a.fov,b.fov,t); k.bars=engine::Math::Lerp(a.bars,b.bars,t);
        return k;
    });
}
template<class State> engine::Vector3 PositionAt(const State& s, const CinematicObjectClip& clip, const CinematicVisualKey& k) {
    const auto pivot=Rotate(Product(clip.pivot,s.scale),s.rotation.y);
    auto offset=k.position;
    // モデル座標を使う場合は、軸ごとの大きさとY回転を反映してワールドのずれに変換する。
    if (clip.positionSpace=="model") offset=Rotate(Product(offset,s.scale),s.rotation.y);
    // 放射方向を使う場合は、Xを水平の距離、Yを高さとして、基準点から伸ばす。
    if (clip.positionSpace=="radial") offset=s.radial*k.position.x+engine::Vector3{0,k.position.y,0};
    auto position=s.origin+pivot+offset;
    // 基準点からの床の高さに達した場合は、その位置にキーの跳ね返り量を足して接地後の高さを決めます。
    if (position.y <= s.origin.y+clip.floorHeight+0.001f)
        position.y=s.origin.y+clip.floorHeight+k.bounce;
    return position;
}
}

CinematicPlayer::~CinematicPlayer() {
    // Scene自身の破棄中は既にObject配列が失われ得る。復元は所有側のStop(true)で済ませる。
    ReleasePresentation(false);
}

void CinematicPlayer::SetAudioServices(std::function<void(const std::string&,float,bool)> play,
    std::function<void(const std::string&)> stop, std::function<std::vector<AudioEntry>()> entries) {
    // 共通の再生器から、ゲーム側の音声サービスへ通知できるよう入口を保持する。
    kPlayAudio=std::move(play); kStopAudio=std::move(stop); sAudioEntries=std::move(entries);
}
std::vector<CinematicPlayer::AudioEntry> CinematicPlayer::GetAudioEntries() {
    // ゲーム側が音声一覧を登録していなければ、編集画面には空の一覧を返す。
    return sAudioEntries ? sAudioEntries() : std::vector<AudioEntry>{};
}

std::unordered_set<engine::Object3D*> CinematicPlayer::GetLiveTargets() const {
    std::unordered_set<engine::Object3D*> live;
    // 結び付け時のシーンを現在も表示している場合だけ、そのシーンのObjectを確認する。
    if (sceneManager_ && sceneManager_->GetCurrentScene()==boundScene_ && boundScene_)
        for (const auto& object:boundScene_->GetObjects())
            // 空の枠やリプレイ上で削除済みになった対象を生存一覧に入れない。
            if (object && !object->IsReplayRemoved()) live.insert(object.get());
    for (const auto& state:presentationObjects_)
        // 再生器が自分で所有する部品は、シーン側の一覧にないため別に登録する。
        if (state.owned) live.insert(state.owned.get());
    return live;
}

void CinematicPlayer::PreparePresentation() {
    // シーケンスまたは対象Sceneがなければ、生成物や参照元を準備できない。
    if (!sequence_ || !boundScene_) return;
    // 名前は素材内で完結し、生成Objectをシーンの保存用配列へ混ぜない。
    for (const auto& clip:sequence_->presentation.objects) {
        PresentationObject s;
        s.source=ResolveTarget({clip.source,-1});
        // 生成指定なら演出専用の見た目を所有し、配置済みの対象を使う設定ではシーンから借ります。
        if (clip.spawn && s.source && boundScene_->GetObject3DCommon()) {
            s.owned=std::make_unique<engine::Object3D>();
            s.owned->Initialize(boundScene_->GetObject3DCommon());
            // 素材名が指定されていれば演出部品のモデルを設定する。指定なしでも位置の参照先にはできる。
            if (!clip.model.empty()) s.owned->SetModel(clip.model);
            s.target=s.owned.get();
            // 演出だけが扱う部品として名前を設定し、編集対象や衝突対象を増やさないようにする。
            s.target->SetName(clip.name); s.target->SetEditorInternal(true);
            s.target->SetCollisionAttribute(0); s.target->SetCollisionMask(0);
            s.target->SetMaterialType(clip.additive?1:0); s.target->SetEnableLighting(clip.lit);
            s.target->SetEnableEnvMap(clip.lit); s.target->SetEnvIntensity(0.55f);
            // 加算材質を指定する部品だけ、加算ブレンドへ切り替える。
            if (clip.additive) s.target->SetBlendMode(BlendMode::kAdd);
        // 生成しない設定の場合は名前の一致する配置済みObjectを借用する。
        } else if (!clip.spawn) s.target=ResolveTarget({clip.name,-1});
        presentationObjects_.push_back(std::move(s));
    }
}

void CinematicPlayer::AddPresentationIssues(std::vector<std::string>& issues) const {
    // 診断するシーケンスが存在しなければ、演出固有の条件は調べない。
    if (!sequence_) return;
    const auto& p=sequence_->presentation;
    const auto live=GetLiveTargets();
    // 視点キーが実カメラを所有する間は、既存Shotとの二重制御を許可しない。
    const bool hasCameraShot=std::any_of(sequence_->cameraShots.begin(),sequence_->cameraShots.end(),
        [](const auto& shot) { return shot.enabled && !shot.muted; });
    const bool hasLegacyCamera=sequence_->cameraShots.empty() && std::any_of(sequence_->transformTracks.begin(),sequence_->transformTracks.end(),
        [&](const auto& track) {
            auto* target=ResolveTarget(track.binding);
            return track.enabled && !track.muted && live.contains(target) && target->IsCameraObject();
        });
    // 視点キーとカットが両方有効だと同じ描画カメラを上書きするため、競合として扱う。
    if (!p.view.empty() && (hasCameraShot || hasLegacyCamera))
        issues.push_back("同じ演出で視点キーとCamera Shotを同時に有効化できません。");
    std::unordered_set<std::string> names;
    for (size_t i=0;i<p.objects.size();++i) {
        const auto& clip=p.objects[i];
        // 無効に設定された部品は、再生時の名前や位置の検証から外す。
        if (!clip.enabled) continue;
        // 空名または2件目以降の同名Objectは、名前による参照を一意に決められない。
        if (clip.name.empty() || !names.insert(clip.name).second) issues.push_back("演出Object名が空、または重複しています: "+clip.name);
        // 生成または取得に失敗した部品と、消失した基準対象を診断する。
        if (i>=presentationObjects_.size() || !live.contains(presentationObjects_[i].target) || !live.contains(presentationObjects_[i].source))
            issues.push_back(clip.name+": 演出Objectまたは基準対象が見つかりません。");
        for (const auto& key : clip.keys)
            for (const auto& name : {key.positionReference, key.positionTarget})
                // キーに指定された基準点・移動先のうち、実位置へ解決できない名前だけを報告する。
                if (!name.empty() && !PresentationPosition(name, live))
                    issues.push_back(clip.name + ": 位置の基準点が見つかりません: " + name);
        float previousEnd = -1.0f;
        for (const auto& motion : clip.motions) {
            const float begin=motion.startSignal.empty()?motion.startTime:sequence_->GetSignalTime(motion.startSignal,-1) + motion.startSignalOffset;
            const float end=motion.endSignal.empty()?motion.endTime:sequence_->GetSignalTime(motion.endSignal,-1) + motion.endSignalOffset;
            // 区間はシグナルの時刻と秒のオフセットを解決した後で、順序と重なりを判定します。
            const bool missingSignal = (!motion.startSignal.empty() && sequence_->GetSignalTime(motion.startSignal,-1)<0) ||
                (!motion.endSignal.empty() && sequence_->GetSignalTime(motion.endSignal,-1)<0);
            // Signalがなく時刻を決められない、開始が負、終端が開始以下、または前区間と重なる場合は失敗として扱う。
            if (missingSignal || begin<0 || end<=begin || begin<previousEnd)
                issues.push_back(clip.name+": 区間のSignal不足、時刻の逆転または重複があります。");
            previousEnd=end;
            for (const auto& point : {motion.from.positionReference,motion.to.positionReference})
                // 各区間の端で位置参照を指定している場合は、その参照先が存在するか確認する。
                if (!point.empty() && !PresentationPosition(point,live))
                    issues.push_back(clip.name+": 区間の基準点が見つかりません: "+point);
        }
        // 有効な部品なのにキーと区間の両方が空なら、見た目を評価できない。
        if ((clip.keys.empty() && clip.motions.empty())) issues.push_back(clip.name+": 見た目キーがありません。");
    }
    // 視点キーを使う素材で全体の基準対象または注視先が存在しなければ、カメラの基準座標を作れない。
    if (!p.view.empty() && (!live.contains(ResolveTarget({p.reference,-1})) || !live.contains(ResolveTarget({p.lookToward,-1}))))
        issues.push_back("演出カメラの基準対象が見つかりません。");
    std::unordered_set<std::string> viewTargets;
    for (const auto& key:p.view)
        for (const auto& name:{key.reference,key.lookToward,key.eyeReference})
            // 個別の注視とアイ位置は別のキーで同じ名前を使えるため、解決の診断は参照名ごとに一度に絞る。
            if (!name.empty() && viewTargets.insert(name).second && !PresentationPosition(name,live))
                issues.push_back("注視対象が見つかりません: "+name);
    for (const auto& name:p.hideObjects)
        // 再生中に隠す対象を名前で解決できない場合は、その指定を診断に残す。
        if (!ResolveTarget({name,-1})) issues.push_back("非表示対象が見つかりません: "+name);
    for (const auto& clip:p.particles)
        // 粒子クリップの発生点に対応する対象が存在しなければ、発生点不足を報告する。
        if (!ResolveTarget({clip.target,-1})) issues.push_back("粒子の発生点が見つかりません: "+clip.target);
}

std::optional<engine::Vector3> CinematicPlayer::PresentationPosition(const std::string& name,
    const std::unordered_set<engine::Object3D*>& live) const {
    // 物理Objectを持たない演出地点にも対応するため、外部位置の解決を先に試す。
    if (positionResolver_)
        // 外部解決が座標を返した場合は、その結果を使いシーンを検索しない。
        if (const auto position=positionResolver_({name,-1})) return position;
    auto* target=ResolveTarget({name,-1});
    return live.contains(target) ? std::optional(target->GetTranslate()) : std::nullopt;
}

std::optional<engine::Vector3> CinematicPlayer::SampleObjectPosition(const std::string& name, float time) const {
    // 基準姿勢が保存されていないか、演出または時刻が無効なら評価できない。
    if (!sequence_ || !presentationCaptured_ || !std::isfinite(time)) return std::nullopt;
    const auto& clips = sequence_->presentation.objects;
    const auto it = std::find_if(clips.begin(), clips.end(), [&](const auto& clip) { return clip.name == name; });
    // 指定名と一致する部品がなければ、位置は取得できない。
    if (it == clips.end()) return std::nullopt;
    return SampleObjectPosition(static_cast<size_t>(it - clips.begin()), time, GetLiveTargets());
}

// 実キャラクターはゲーム側が操作区間を決め、姿勢の数式は通常再生と同じ入口へ任せる。
bool CinematicPlayer::ApplyObjectTransform(const std::string& name, float time) {
    // 再生時の基準姿勢を保持しており、指定時刻が有限の場合だけ対象を動かす。
    if (!sequence_ || !presentationCaptured_ || !std::isfinite(time)) return false;
    const auto& clips = sequence_->presentation.objects;
    const auto it = std::find_if(clips.begin(), clips.end(), [&](const auto& clip) { return clip.name == name; });
    // 名前で動かす対象を特定できなければ、シーンは変更しない。
    if (it == clips.end()) return false;
    const size_t index=static_cast<size_t>(it-clips.begin());
    // 姿勢の基準位置を求められなければここで中断し、成功したときだけワールド行列へ反映する。
    if (!ApplyPresentationTransform(index,time,GetLiveTargets())) return false;
    presentationObjects_[index].target->UpdateWorldMatrix(false);
    return true;
}

bool CinematicPlayer::ApplyPresentationTransform(size_t index, float time,
    const std::unordered_set<engine::Object3D*>& live) {
    const auto position = SampleObjectPosition(index, time, live);
    // 有効な位置を評価できない場合は、回転や拡縮も適用しない。
    if (!position) return false;
    const auto& clip = sequence_->presentation.objects[index];
    auto& state = presentationObjects_[index];
    const auto key = SampleCinematicVisual(clip, time, sequence_);
    // 追従指定の成分は対象の現在値を使い、それ以外は再生開始前の採取値を使います。
    const auto rotation = clip.followRotation ? state.source->GetRotation() : state.rotation;
    const auto scale = clip.followScale ? state.source->GetScale() : state.scale;
    state.target->SetTranslate(*position);
    state.target->SetRotation(clip.inheritRotation ? Product(rotation,clip.rotationMask)+key.rotation : key.rotation);
    state.target->SetScale(clip.inheritScale ? Product(scale,key.scale) : key.scale);
    return true;
}

std::optional<engine::Vector3> CinematicPlayer::SampleObjectPosition(size_t index, float time,
    const std::unordered_set<engine::Object3D*>& live) const {
    // 同じ番号の設定と実行状態の両方が存在しない場合は、位置を計算できない。
    if (!sequence_ || index >= presentationObjects_.size() || index >= sequence_->presentation.objects.size())
        return std::nullopt;
    const auto& clip = sequence_->presentation.objects[index];
    const auto& s = presentationObjects_[index];
    // 無効な部品、姿勢定義のない部品、または基準と対象のどちらかが消失した部品は評価しない。
    if (!clip.enabled || (clip.keys.empty() && clip.motions.empty()) || !live.contains(s.source) || !live.contains(s.target)) return std::nullopt;
    /// <summary>位置キーの評価に使う基準姿勢。追従が有効な成分は、この時点の参照元の姿勢で置き換える。</summary>
    struct { engine::Vector3 origin, rotation, scale, radial; } pose{s.origin, s.rotation, s.scale, s.radial};
    // 移動追従を指定した成分だけ、再生開始時ではなく取得時点の基準位置を使う。
    if (clip.followPosition) pose.origin = s.source->GetTranslate();
    // 回転追従が有効なら、ローカル位置の変換にも現時点の回転を使う。
    if (clip.followRotation) pose.rotation = s.source->GetRotation();
    // 拡縮追従が有効なら、基準寸法に対するオフセットも現在の倍率で変換する。
    if (clip.followScale) pose.scale = s.source->GetScale();
    bool valid = true;
    auto position = [&](const CinematicVisualKey& key) {
        // 基準点の名前が指定されていないキーは、開始姿勢とピボットから位置を求める。
        if (key.positionReference.empty()) return PositionAt(pose, clip, key);
        const auto from = PresentationPosition(key.positionReference, live);
        const auto to = key.positionTarget.empty() ? from : PresentationPosition(key.positionTarget, live);
        // 名前付きの始点または終点を解決できない場合は、呼び出し元へ評価失敗を伝える。
        if (!from || !to) { valid = false; return engine::Vector3{}; }
        return engine::Math::Lerp(*from, *to, key.positionMix) + key.position;
    };
    // 名前の異なるキーも、それぞれの基準点を解決してからワールド位置を補間する。
    // 区間で定義した部品は、現在区間の始終点が混ざったキーを求めてからワールド位置へ変換する。
    if (!clip.motions.empty()) {
        const auto result = position(SampleCinematicVisual(clip,time,sequence_));
        return valid ? std::optional(result) : std::nullopt;
    }
    auto upper = std::upper_bound(clip.keys.begin(), clip.keys.end(), time,
        [](float t, const auto& key) { return t < key.time; });
    engine::Vector3 result;
    // 最初のキーより前は、そのキーの位置で待機する。
    if (upper == clip.keys.begin()) result = position(clip.keys.front());
    // 最後のキー以後は、そのキーの位置を保持する。
    else if (upper == clip.keys.end()) result = position(clip.keys.back());
    // キーの途中では前後の参照先をそれぞれ解決する。
    else {
        const auto& a = *(upper - 1); const auto& b = *upper;
        // 従来のキーは既存のイージングと接地処理を維持する。
        // 両端が名前付き基準を使わない場合は旧方式で補間し、接地処理も共通の位置計算に任せる。
        if (a.positionReference.empty() && b.positionReference.empty()) return PositionAt(pose, clip, SampleCinematicVisual(clip, time, sequence_));
        const float ratio = std::clamp((time - a.time) / std::max(0.000001f, b.time - a.time), 0.0f, 1.0f);
        result = engine::Math::Lerp(position(a), position(b), ratio);
    }
    return valid ? std::optional(result) : std::nullopt;
}

void CinematicPlayer::CapturePresentation() {
    // シーケンスがない場合や既に採取済みの場合は、復元に必要な元の姿勢を上書きしない。
    if (!sequence_ || presentationCaptured_) return;
    // 対象の準備をまだしていなければ、基準姿勢を取る前に部品を解決する。
    if (presentationObjects_.empty()) PreparePresentation();
    const auto& p=sequence_->presentation;
    // 見た目部品も視点キーもなければ、専用演出の状態を採取しない。
    if (p.objects.empty() && p.view.empty()) return;
    presentationCaptured_=true;
    // 再生直前の姿勢を採る。準備済み部品の再生成やVFXの再読込は行わない。
    for (size_t i=0;i<presentationObjects_.size();++i) {
        auto& s=presentationObjects_[i];
        // 基準と対象のどちらかを取得できなかった部品は、保存できる姿勢がないため飛ばす。
        if (!s.source || !s.target) continue;
        const auto& clip=p.objects[i];
        s.origin=s.source->GetTranslate(); s.rotation=s.source->GetRotation(); s.scale=s.source->GetScale();
        const auto pivot=Rotate(Product(clip.pivot,s.scale),s.rotation.y);
        const float length=std::sqrt(pivot.x*pivot.x+pivot.z*pivot.z);
        // 基準からの水平距離で割って放射方向を単位化します。ピボットがほぼ真上なら +X を既定方向にします。
        s.radial=length>0.01f ? engine::Vector3{pivot.x/length,0,pivot.z/length} : engine::Vector3{1,0,0};
        s.before.position=s.target->GetTranslate(); s.before.rotation=s.target->GetRotation();
        s.before.scale=s.target->GetScale(); s.before.visible=s.target->GetIsVisible();
        s.material=s.target->CaptureMaterialState();
    }
    for (const auto& name:p.hideObjects)
        // 非表示対象が実際に解決できた場合だけ、元の表示を退避して隠す。
        if (auto* object=ResolveTarget({name,-1})) {
            // 一時的に隠す対象は、終了時に元の表示状態へ戻せるよう、変更する前に可視フラグを保存します。
            presentationHidden_.push_back({object,object->GetIsVisible()}); object->SetIsVisible(false);
        }
    // 視点キーがない場合は、シーンの有効カメラを切り替えない。
    if (p.view.empty()) return;
    auto* reference=ResolveTarget({p.reference,-1}); auto* toward=ResolveTarget({p.lookToward,-1});
    // 基準位置と注視先の両方がなければ、カメラの構図を作れない。
    if (!reference || !toward) return;
    presentationOrigin_=reference->GetTranslate(); presentationScale_=reference->GetScale();
    presentationLookAt_=toward->GetTranslate();
    const auto direction=presentationLookAt_-presentationOrigin_;
    // ワールドの +Z を正面として、注視先へ向く水平角をラジアンで求めます。高さの差はここで使いません。
    presentationYaw_=std::atan2(direction.x,direction.z);
    // 上書き先を確保する前に旧カメラを保持して、終了時に元の参照へ戻せるようにします。
    presentationPreviousCamera_=CameraManager::GetInstance()->GetActiveCameraOverride();
    presentationCameraOrigin_.reset();
    presentationCamera_=std::make_unique<engine::Camera>(); presentationCamera_->Initialize();
    // 有効なカメラを取得できた場合は元の近・遠クリップ距離を引き継ぐ。
    if (auto* camera=CameraManager::GetInstance()->GetActiveCamera()) {
        presentationCamera_->SetClipRange(camera->GetNearClip(),camera->GetFarClip());
        presentationCameraOrigin_ = CameraBlendOrigin{camera->GetEye(), camera->GetTargetPoint(), camera->GetFovY()};
    }
    presentationCamera_->SetInputEnabled(false);
    // ポストエフェクトの設定を借りられるときだけ、演出帯の元の高さを覚えておく。
    if (auto* params=PostEffect::GetInstance()->GetParams()) presentationBars_=params->cinemaBarHeight;
}

void CinematicPlayer::ReleasePresentation(bool restore) {
    bindingsReady_=false;
    // 開始時に状態を採取し、呼び出し側から復元を指定された場合だけ、借用した実体を戻す。
    if (restore && presentationCaptured_) {
        const auto live=GetLiveTargets();
        // 借用した実体は生存を確かめて元の姿勢と材質へ戻します。専用に生成した実体は末尾の所有リスト破棄へ任せます。
        for (auto& s:presentationObjects_)
            // 演出自身が所有せず、現在も使える対象だけに退避値を書き戻す。
            if (!s.owned && live.contains(s.target)) {
                s.target->SetTranslate(s.before.position); s.target->SetRotation(s.before.rotation);
                s.target->SetScale(s.before.scale); s.target->SetIsVisible(s.before.visible);
                s.target->RestoreMaterialState(s.material);
                s.target->UpdateWorldMatrix(true);
            }
        for (const auto& hidden:presentationHidden_)
            // 一時的に隠した対象がまだ存在する場合だけ、退避した表示に戻す。
            if (live.contains(hidden.first)) hidden.first->SetIsVisible(hidden.second);
        // 専用カメラを使った同じシーンにいる場合だけ、演出帯の高さを復元する。
        if (presentationCamera_ && sceneManager_ && sceneManager_->GetCurrentScene()==boundScene_)
            // 後処理の設定を取得できた場合だけ、保存した帯の高さを書き戻す。
            if (auto* params=PostEffect::GetInstance()->GetParams()) params->cinemaBarHeight=presentationBars_;
    }
    // カメラの上書きを解除してから専用部品を破棄する。
    if (!restore) {
        presentationPreviousCamera_ = nullptr;
    }
    RestorePresentationCamera();
    presentationObjects_.clear(); presentationHidden_.clear(); presentationCaptured_=false;
}

void CinematicPlayer::RestorePresentationCamera() {
    // 別の処理がカメラを選び直していたら、その選択を維持する。
    if (!presentationCamera_) {
        return;
    }
    auto* manager = CameraManager::GetInstance();
    if (manager->GetActiveCameraOverride() == presentationCamera_.get()) {
        const bool sameScene = sceneManager_ && sceneManager_->GetCurrentScene() == boundScene_;
        manager->SetActiveCamera(sameScene ? presentationPreviousCamera_ : nullptr);
    }
    presentationCamera_.reset();
    presentationPreviousCamera_ = nullptr;
    presentationCameraOrigin_.reset();
}

void CinematicPlayer::EvaluatePresentation(float time, float previous, bool dispatch) {
    // 時刻から姿勢を求めるにはシーケンスと採取済みの基準が必要。
    if (!sequence_ || !presentationCaptured_) return;
    const auto& p=sequence_->presentation;
    const auto live=GetLiveTargets();
    for (size_t i=0;i<p.objects.size() && i<presentationObjects_.size();++i) {
        const auto& clip=p.objects[i]; auto& s=presentationObjects_[i];
        // 無効または姿勢の定義がない部品は、対象の更新を行わない。
        if (!clip.enabled || (clip.keys.empty() && clip.motions.empty())) continue;
        // 対象や基準がシーンから消えた時点で評価を中断します。残った対象とカメラの復元は ReleasePresentation に任せます。
        if (!live.contains(s.source) || !live.contains(s.target)) {
            playbackTimer_.Pause(); isPreviewing_=false; finished_=false;
            ReleasePresentation(true);
            return;
        }
        // 静止部品は準備時の状態を維持し、キーによる見た目の切り替えを行わない。
        if (!clip.animate) continue;
        const auto k=SampleCinematicVisual(clip,time,sequence_);
        const auto scale=clip.followScale?s.source->GetScale():s.scale;
        // 再生中の位置参照が失われ、姿勢を適用できない場合は時計と表示を止めて復元する。
        if (!ApplyPresentationTransform(i,time,live)) {
            playbackTimer_.Pause(); isPreviewing_=false; finished_=false; ReleasePresentation(true); return;
        }
        // 姿勢の適用後、その位置を基準に表示・発光とクリップ面を設定し、行列を更新する。
        const auto position=s.target->GetTranslate();
        s.target->SetColor(k.color); s.target->SetEmissive(k.emissive);
        s.target->SetIsVisible(k.visible && (!clip.inheritVisibility || s.source->GetIsVisible()));
        {
            engine::Object3D::PortalClipSettings clipSettings;
            clipSettings.enabled=k.clipEnabled; clipSettings.progress=0;
            // 切断面の高さを必要に応じてモデルの縦倍率で調整し、対象のワールド位置からのオフセットに変換します。
            clipSettings.center=position+engine::Vector3{0,k.clipHeight*(clip.clipScaleHeight?scale.y:1.0f)+clip.clipCenterOffset,0};
            clipSettings.normal={0,1,0}; clipSettings.edgeWidth=clip.clipEdgeWidth;
            clipSettings.dissolve=clip.clipDissolve; clipSettings.color=clip.clipColor;
            s.target->SetPortalClip(clipSettings);
        }
        s.target->UpdateWorldMatrix(true);
    }
    // 遅延フレームでも区間内の粒子を順に出す。停止・巻戻しのシークでは発火しない。
    if (dispatch && playbackTimer_.IsRunning() && time>=previous)
        for (const auto& clip:p.particles) {
            size_t i=0;
            while (i<p.objects.size() && p.objects[i].name!=clip.target) ++i;
            // 発生点の部品が見つからないか時間による姿勢の定義がなければ、粒子を出さない。
            if (i>=presentationObjects_.size() || (p.objects[i].keys.empty() && p.objects[i].motions.empty())) continue;
            const auto& s=presentationObjects_[i];
            // 発生間隔は秒です。前回時刻の発生番号は済んでいるので次から始め、今回はクリップの終了時刻までに制限します。
            const std::int32_t first=std::max(0,static_cast<std::int32_t>(std::floor((previous-clip.startTime)/clip.interval))+1);
            const std::int32_t last=static_cast<std::int32_t>(std::floor((std::min(time,clip.endTime)-clip.startTime)/clip.interval));
            for (std::int32_t tick=first;tick<=last;++tick) {
                // 遅延して複数回発生する場合も、現在時刻の位置へまとめず、それぞれの発生時刻の位置を再評価します。
                const float tickTime = clip.startTime + tick * clip.interval;
                const auto& objectClip = p.objects[i];
                const bool hasReferences = !objectClip.motions.empty() || std::any_of(objectClip.keys.begin(), objectClip.keys.end(),
                    [](const auto& key) { return !key.positionReference.empty(); });
                // 従来素材の粒子は開始姿勢基準の挙動を保ち、基準点キーだけ新しい評価を使う。
                const auto sampled = hasReferences ? SampleObjectPosition(i, tickTime, live)
                    : std::optional(PositionAt(s, objectClip, SampleCinematicVisual(objectClip, tickTime, sequence_)));
                // その発火時刻に発生位置を解決できなければ、この発火だけを飛ばす。
                if (!sampled) continue;
                const auto position = *sampled;
                auto direction=clip.direction;
                // 中心向きの指定では、発火点から開始時の基準位置へ向けてXZ方向を計算する。
                if (clip.directionMode=="radialIn") {
                    // 中心へ向かう水平ベクトルを単位化し、保存された鉛直方向の成分を残します。
                    const auto d=s.origin-position; const float length=std::sqrt(d.x*d.x+d.z*d.z);
                    // 中心までの水平距離がほぼ0で方向を正規化できないときは、素材の指定方向を維持する。
                    if (length>0.001f) direction={d.x/length,clip.direction.y,d.z/length};
                }
                GpuParticleManager::GetInstance()->EmitDirected(clip.preset,position,direction,clip.scale);
            }
        }
    // 演出専用カメラと視点キーが揃っている間だけ構図を時間から更新する。
    if (presentationCamera_ && !p.view.empty()) {
        const auto k=ViewAt(p.view,time);
        const auto from=k.reference.empty()?std::optional(presentationOrigin_):PresentationPosition(k.reference,live);
        const auto to=k.lookToward.empty()?std::optional(presentationLookAt_):PresentationPosition(k.lookToward,live);
        const auto eyeAnchor=k.eyeReference.empty()?std::optional(engine::Vector3{}):PresentationPosition(k.eyeReference,live);
        // カメラに必要な注視やアイ位置の参照を失ったら、評価を続けず従来の表示へ戻す。
        if (!from || !to || !eyeAnchor) {
            playbackTimer_.Pause(); isPreviewing_=false; finished_=false; ReleasePresentation(true); return;
        }
        const float yaw=p.viewWorldSpace?0.0f:presentationYaw_;
        auto target=engine::Math::Lerp(*from,*to,k.focusMix)+
            Product(k.scaledFocusOffset,presentationScale_)+Rotate(k.focusOffset,yaw);
        auto eye=(k.eyeReference.empty()?target:*eyeAnchor)+Rotate(k.eyeOffset,yaw)+k.jitter;
        float fov = k.fov;
        // 通常操作から入ったときは、開始前の視点を基準に指定秒数で補間する。
        if (presentationCameraOrigin_ && time < presentationCameraBlend_) {
            const float rate = Easing::SmoothStep01(
                animationInterpolation::SegmentRate(time, 0.0f, presentationCameraBlend_));
            eye = animationInterpolation::Lerp(presentationCameraOrigin_->eye, eye, rate);
            target = animationInterpolation::Lerp(presentationCameraOrigin_->target, target, rate);
            fov = animationInterpolation::Lerp(presentationCameraOrigin_->fov, fov, rate);
        }
        const auto d=target-eye;
        // 注視点との差からピッチとヨーをラジアンで求め、ロールは0とします。
        const engine::Vector3 rotation{-std::atan2(d.y,std::sqrt(d.x*d.x+d.z*d.z)),std::atan2(d.x,d.z),0};
        presentationCamera_->ConfigFixedPoint(eye,rotation);
        presentationCamera_->ApplyReplayView(eye,target,rotation,fov,presentationCamera_->GetNearClip(),presentationCamera_->GetFarClip());
        CameraManager::GetInstance()->SetActiveCamera(presentationCamera_.get());
        // 後処理の設定がある場合に限り、現在のキーが持つ帯の高さを反映する。
        if (auto* params=PostEffect::GetInstance()->GetParams()) params->cinemaBarHeight=k.bars;
    }
}

void CinematicPlayer::DrawPresentation(ID3D12Resource* pointLight, ID3D12Resource* spotLight) {
    // 採取済みの演出表示がなければ、描画する専用部品はない。
    if (!presentationCaptured_ || !sequence_) return;
    // 演出が所有する表示モデルを通常材質、加算材質の順で描きます。シーン所有の対象は通常の描画経路へ任せます。
    for (std::int32_t transparent=0;transparent<2;++transparent)
        for (size_t i=0;i<presentationObjects_.size() && i<sequence_->presentation.objects.size();++i) {
            auto& s=presentationObjects_[i]; const auto& clip=sequence_->presentation.objects[i];
            // 所有している表示可能なモデルのうち、今回の通常または加算の描画段階に合う部品を描く。
            if (s.owned && !clip.model.empty() && clip.enabled && clip.additive==(transparent!=0) && s.target->GetIsVisible()) {
                s.target->UpdateWorldMatrix(true); s.target->Draw(pointLight,spotLight);
            }
        }
}

} // namespace engine
