#define NOMINMAX
#include <cstdint>
#include "Object3D.h"
#include "VfxSequencer.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <utility>

namespace engine {

namespace {
std::string ResolveAnimatorControllerPath(const std::string& assetPath) {
    // パスが空の場合は、アセット未指定を表す空文字列を返す。
    if (assetPath.empty()) {
        return {};
    }

    std::filesystem::path path(assetPath);

    // フォルダー指定がなければ、共通のAnimatorフォルダーを付ける。
    if (!path.has_parent_path()) {
        path = std::filesystem::path("Resources/json/animator") / path;
    }

    // 拡張子を指定していない場合だけ、標準の.jsonを付ける。
    if (!path.has_extension()) {
        path += ".json";
    }

    return path.generic_string();
}
}

bool Object3D::SetAnimatorController(const std::string& assetPath) {
    // 空の参照を渡された場合は、読み込みを試さずControllerを解除する。
    if (assetPath.empty()) {
        ClearAnimatorController();
        return true;
    }

    const std::string resolvedPath = ResolveAnimatorControllerPath(assetPath);
    AnimatorControllerAsset loadedAsset;

    // 定義を読み込めなければ、現在使っているControllerを保持する。
    if (!loadedAsset.Load(resolvedPath)) {
        return false;
    }

    std::unordered_map<std::string, BodyAnimationClip> loadedBodyClips;

    // 同じ身体クリップを複数の状態で使う場合も、名前ごとに一度だけ読み込みます。
    for (const auto& state : loadedAsset.states) {
        // 身体クリップの指定が無い状態や、同じ名前を読み込み済みの状態は飛ばす。
        if (state.bodyClipName.empty() || loadedBodyClips.contains(state.bodyClipName))
            continue;
        BodyAnimationClip clip;
        // 必要な身体クリップが一つでも読めなければ、一時データを破棄し既存の設定を保持する。
        if (!clip.Load(BodyAnimationClip::ResolveAssetPath(state.bodyClipName)))
            return false;
        loadedBodyClips.emplace(state.bodyClipName, std::move(clip));
    }

    animatorControllerPath_ = assetPath;
    animatorControllerAsset_ = std::move(loadedAsset);
    animatorBodyClips_ = std::move(loadedBodyClips);
    animatorControllerLoaded_ = true;
    // 実行側がアセットを借用するので、一時変数からメンバーへ移したあと、そのアドレスを渡して再生状態を初期化します。
    animatorControllerRuntime_.SetController(&animatorControllerAsset_, true);
    return true;
}

void Object3D::ClearAnimatorController() {
    // 身体補正を外してからアセットを破棄し、次のモデルへ変形を持ち越さない。
    if (animatorBodyPoseApplied_ && meshRenderer_)
        meshRenderer_->ResetVisualTransform();
    animatorBodyPoseApplied_ = false;
    animatorBodyClips_.clear();
    animatorControllerRuntime_.SetController(nullptr, false);
    animatorControllerAsset_.Clear();
    animatorControllerPath_.clear();
    animatorControllerLoaded_ = false;
}

bool Object3D::PlayAnimatorState(const std::string& stateName, float blendDuration, std::int32_t easing) {
    // Controller 未設定時に状態遷移だけを先行させない。
    if (!animatorControllerLoaded_) {
        return false;
    }

    return animatorControllerRuntime_.CrossFade(stateName, blendDuration, easing);
}

bool Object3D::EvaluateAnimatorState(
    const std::string& stateName,
    float timeSeconds,
    bool loop,
    float blendDuration,
    std::int32_t easing,
    bool exactPreview) {
    // 編集プレビューは時刻を直接指定し、実戦では Controller の補間経路を使う。
    engine::Model* model = GetModel();

    // モデルが無ければ、状態の表示姿勢を評価できない。
    if (!model) {
        return false;
    }

    // Controllerを未設定の場合は、引数の状態名を直接モデルのアニメーション名として探す。
    if (!animatorControllerLoaded_) {
        const engine::Model::Animation* animation = model->GetAnimation(stateName);

        // 同名のモデルアニメーションが無ければ、姿勢を適用せず失敗を返す。
        if (!animation) {
            return false;
        }

        animName_ = stateName;
        isAnimLoop_ = loop;
        animationTime_ = std::max(0.0f, timeSeconds);
        float sampleTime = animationTime_;

        // ループ指定で尺が正の場合は、尺で割った余りをクリップ内の時刻にする。
        if (loop && animation->duration > 0.0f) {
            sampleTime = std::fmod(sampleTime, animation->duration);
        // 単発または尺が0の場合は、末尾を越えないサンプル時刻を使う。
        } else {
            sampleTime = std::min(sampleTime, animation->duration);
        }

        model->ApplyAnimation(*animation, sampleTime);
        model->Update(true);
        return true;
    }

    // 指定の状態が再生中でない場合だけ、Controllerの再生状態を切り替える。
    if (!animatorControllerRuntime_.IsPlayingState(stateName)) {
        // 正確な時刻の確認では、補間なしで対象の状態を選ぶ。
        if (exactPreview) {
            animatorControllerRuntime_.Play(stateName, 0.0f);
        // 通常の更新では、指定した秒数と曲線で状態を切り替える。
        } else {
            animatorControllerRuntime_.CrossFade(stateName, blendDuration, easing);
        }
    }

    animatorControllerRuntime_.SetCurrentTime(std::max(0.0f, timeSeconds));
    return ApplyAnimatorControllerPose(model, true);
}

std::string Object3D::GetAnimatorCurrentStateName() const {
    // Controller に状態がない間は空名を返し、呼び出し側の代替表示に任せる。
    const AnimatorStateDefinition* state = animatorControllerRuntime_.GetCurrentState();
    return state ? state->name : std::string{};
}

AnimatorControllerRuntime::Snapshot Object3D::CaptureAnimatorSnapshot() const {
    // プレビューを抜けた後に元の再生時刻へ戻せるよう、実行状態を保存する。
    return animatorControllerRuntime_.CaptureSnapshot();
}

void Object3D::RestoreAnimatorSnapshot(const AnimatorControllerRuntime::Snapshot& snapshot) {
    // 時刻だけでなく見た目の姿勢も同期して、プレビュー終了時の残像を防ぐ。
    if (!animatorControllerLoaded_ || !snapshot.valid) {
        return;
    }

    animatorControllerRuntime_.RestoreSnapshot(snapshot);
    ApplyAnimatorControllerPose(GetModel(), true);
}

void Object3D::RestoreAnimationPlayback(const std::string& animationName, float timeSeconds, bool loop) {
    // Controller を使わないモデルの再生時刻も、巻き戻し直後に姿勢へ反映する。
    animName_ = animationName;
    animationTime_ = std::max(0.0f, timeSeconds);
    isAnimLoop_ = loop;
    engine::Model* model = GetModel();

    // 再生設定は先に保持し、モデルや名前が無ければ表示姿勢の反映だけを省略する。
    if (!model || animationName.empty()) {
        return;
    }

    // 指定名のアニメーションがモデル内にある場合に限り、その時刻の姿勢を評価する。
    if (const engine::Model::Animation* animation = model->GetAnimation(animationName)) {
        float sampleTime = animationTime_;

        // 正の長さでループ再生なら、クリップ内を周回する秒数に直す。
        if (loop && animation->duration > 0.0f) {
            sampleTime = std::fmod(sampleTime, animation->duration);
        // 単発や周回できない長さなら、末尾を上限にする。
        } else {
            sampleTime = std::min(sampleTime, animation->duration);
        }

        model->ApplyAnimation(*animation, sampleTime);
        model->Update(true);
    }
}

bool Object3D::TryGetJointWorldMatrix(const std::string& jointName, engine::Matrix4x4& outMatrix) const {
    // ボーンの局所姿勢と Object3D のワールド行列を合成して装着位置を得る。
    engine::Model* model = GetModel();

    // モデルまたは関節名が無ければ、装着先を特定できない。
    if (!model || jointName.empty()) {
        return false;
    }

    const std::int32_t jointIndex = model->FindJointIndex(jointName);
    const auto& joints = model->GetJoints();

    // 関節名に対応する番号が有効な範囲に無ければ、行列を出力しない。
    if (jointIndex < 0 || jointIndex >= static_cast<std::int32_t>(joints.size())) {
        return false;
    }

    // 関節の親子姿勢を反映済みのモデル空間行列に、Object3Dの配置を掛けて装着先のワールド姿勢を得ます。
    outMatrix = engine::Math::Multiply(joints[jointIndex].skeletonSpaceMatrix, GetWorldMatrix());
    return true;
}

bool Object3D::TryGetJointWorldPosition(const std::string& jointName, engine::Vector3& outPosition) const {
    // ジョイントが見つからない場合は出力位置を変更しない。
    engine::Matrix4x4 jointWorld = engine::Math::MakeIdentity4x4();

    // 関節のワールド行列を得られなければ、既存の出力座標を保持する。
    if (!TryGetJointWorldMatrix(jointName, jointWorld)) {
        return false;
    }

    outPosition = { jointWorld.m[3][0], jointWorld.m[3][1], jointWorld.m[3][2] };
    return true;
}

bool Object3D::UpdateAnimatorController(float deltaTime, engine::Model* model) {
    // Controllerとモデルがそろう場合だけ、時計と再生イベントを進める。
    if (!animatorControllerLoaded_ || !model) {
        return false;
    }

    // 状態の終了判定には、身体クリップがあればその秒数を使い、なければモデルの骨格クリップの秒数を使います。
    animatorControllerRuntime_.Update(deltaTime, [this, model](const AnimatorStateDefinition& state) {
        // Body Clipを取得できた場合は、その再生尺を状態の長さとして優先する。
        if (const auto body = animatorBodyClips_.find(state.bodyClipName); body != animatorBodyClips_.end())
            return body->second.GetDuration();
        const engine::Model::Animation* animation = model->GetAnimation(state.clipName);
        return animation ? animation->duration : 0.0f;
    });

    for (const AnimatorEventInstance& event : animatorControllerRuntime_.ConsumeEvents()) {
        // FeedbackCueはVFX、SE、カメラ、振動、ヒットストップを一つのAssetから再生します。
        if (event.name == "FeedbackCue" && !event.payload.empty()) {
            VfxSequencer::PlayOneShotOnTarget(event.payload, this);
        }

        // 呼び出し側のイベント処理が登録されていれば、処理中のイベントを通知する。
        if (animatorEventCallback_) {
            animatorEventCallback_(event);
        }
    }

    return ApplyAnimatorControllerPose(model, false);
}

bool Object3D::ApplyAnimatorControllerPose(engine::Model* model, bool forceModelUpdate) {
    // 前後状態を同じ時刻基準で標本化し、骨格と身体変形を一つの補間率で合成する。
    if (!animatorControllerLoaded_ || !model) {
        return false;
    }

    const AnimatorStateDefinition* currentState = animatorControllerRuntime_.GetCurrentState();

    // 現在の状態が選択されていなければ、姿勢は適用できない。
    if (!currentState) {
        return false;
    }

    const AnimatorStateDefinition* previousState = animatorControllerRuntime_.GetPreviousState();
    const bool transitioning = previousState && animatorControllerRuntime_.IsTransitioning();
    const auto sampleBody = [this](const AnimatorStateDefinition* state, float time) {
        BodyAnimationClip::Sample sample;

        // 評価対象の状態が存在する場合にだけ、紐付いたBody Clipを探す。
        if (state) {
            const auto clip = animatorBodyClips_.find(state->bodyClipName);
            // Body Clipを読み込み済みであれば、現在秒数とループ指定に合わせて姿勢を求める。
            if (clip != animatorBodyClips_.end())
                clip->second.Evaluate(time, state->loop, sample);
        }

        return sample;
    };

    const bool hasBody = !currentState->bodyClipName.empty() ||
        (transitioning && !previousState->bodyClipName.empty());
    // 現在か遷移元にBody Clipの指定があり、描画先がある場合は身体変形を反映する。
    if (hasBody && meshRenderer_) {
        const auto current = sampleBody(currentState, animatorControllerRuntime_.GetStateTime());
        const auto previous = transitioning
            ? sampleBody(previousState, animatorControllerRuntime_.GetPreviousTime()) : current;
        const float weight = transitioning ? animatorControllerRuntime_.GetTransitionWeight() : 1.0f;
        // ラジアンの回転は近い方向へ補間し、±πの境界を跨ぐときの不要な一周を抑えます。
        const auto blendAngle = [weight](float from, float to) {
            return engine::Math::LerpShortAngle(from, to, weight);
        };

        meshRenderer_->SetVisualTransform(engine::Math::Lerp(previous.scale, current.scale, weight),
            {blendAngle(previous.rotate.x, current.rotate.x), blendAngle(previous.rotate.y, current.rotate.y),
             blendAngle(previous.rotate.z, current.rotate.z)},
            engine::Math::Lerp(previous.translate, current.translate, weight));
        meshRenderer_->SetVisualShear(engine::Math::Lerp(previous.shear, current.shear, weight),
            engine::Math::Lerp(previous.shearPivotY, current.shearPivotY, weight));
        animatorBodyPoseApplied_ = true;
    // 適用する身体変形が無く、前回の変形が描画先に残っている場合は元の形へ戻す。
    } else if (animatorBodyPoseApplied_ && meshRenderer_) {
        meshRenderer_->ResetVisualTransform();
        animatorBodyPoseApplied_ = false;
    }

    // 描画用の身体変形を確定した後、骨格クリップの有無に応じて骨格の補間または単独再生を適用します。
    const engine::Model::Animation* currentAnimation = currentState->clipName.empty()
        ? nullptr : model->GetAnimation(currentState->clipName);
    const engine::Model::Animation* previousAnimation = transitioning && !previousState->clipName.empty()
        ? model->GetAnimation(previousState->clipName) : nullptr;
    // 遷移元と現在の両方に骨格アニメーションがある場合は、両方を採取して補間する。
    if (currentAnimation && previousAnimation) {
        model->ApplyBlendedAnimation(
            *previousAnimation,
            animatorControllerRuntime_.GetPreviousTime(),
            *currentAnimation,
            animatorControllerRuntime_.GetStateTime(),
            animatorControllerRuntime_.GetTransitionWeight());
    // 現在の骨格アニメーションだけ取得できた場合は、その姿勢を単独で適用する。
    } else if (currentAnimation) {
        model->ApplyAnimation(*currentAnimation, animatorControllerRuntime_.GetStateTime());
    }

    // この状態が身体変形も骨格アニメーションも指定していなければ、呼出元の代替再生に任せます。
    if (!hasBody && !currentAnimation)
        return false;
    animName_ = currentState->clipName;
    animationTime_ = animatorControllerRuntime_.GetStateTime();
    isAnimLoop_ = currentState->loop;
    model->Update(forceModelUpdate || currentAnimation != nullptr);
    return true;
}

} // namespace engine
