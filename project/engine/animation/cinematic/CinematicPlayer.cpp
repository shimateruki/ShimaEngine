#define NOMINMAX
#include <cstdint>
#include "engine/animation/cinematic/CinematicPlayer.h"

#include "AnimationInterpolation.h"
#include "BaseScene.h"
#include "engine/editor/graphics/camera/CameraEditor.h"
#include "CameraManager.h"
#include "GhostRecorder.h"
#include "Object3D.h"
#include "MeshRenderer.h"
#include "SceneManager.h"
#include "Log.h"

#include <algorithm>
#include <cmath>

namespace engine {

namespace {
// 保存された整数を補間方式の有効範囲へ収めて変換します。
animationInterpolation::EasingType ToEasingType(std::int32_t value) {
    // 保存された番号を有効な補間方式へ制限する。
    value = std::clamp(value, 0, 4);
    return static_cast<animationInterpolation::EasingType>(value);
}

engine::Vector3 AddVector3(const engine::Vector3& a, const engine::Vector3& b) {
    // 相対トラックの位置・回転を基準姿勢へ加える。
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

engine::Vector3 MultiplyVector3(const engine::Vector3& a, const engine::Vector3& b) {
    // 相対トラックの拡大率は基準スケールとの成分積で扱う。
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

void ApplyPose(engine::Object3D* object, const engine::Vector3& position, const engine::Vector3& rotation, const engine::Vector3& scale) {
    // 対象が解決できていない場合は、姿勢も描画行列も変更しない。
    if (!object) {
        return;
    }

    // 姿勢を書き込んでからローカル行列とワールド行列の順に更新し、同じフレームに反映する。
    object->SetTranslate(position);
    object->SetRotation(rotation);
    object->SetScale(scale);
    object->UpdateLocalMatrix();
    object->UpdateWorldMatrix();
}
} // namespace

// シネマティックを時刻から評価します。シークでは姿勢を再計算し、イベントの再発火は呼び出し側が指定します。

void CinematicPlayer::Initialize(SceneManager* sceneManager) {
    // 前の演出の姿勢と画面効果を復元してから、借用先と再生時計を空にする。
    Stop(true);
    finished_ = false;
    // 新しいシーンで使う前に、前回の束縛先とトラック状態を消す。
    sceneManager_ = sceneManager;
    sequence_ = nullptr;
    boundScene_ = nullptr;
    transformRuntime_.clear();
    vfxRuntime_.clear();
    audioRuntime_.clear();
    animationRuntime_.clear();
    ownedSequence_.reset();
    playbackTimer_.Seek(0.0f);
    playbackTimer_.Pause();
    isPreviewing_ = false;
    isLooping_ = false;
    activeCameraObject_ = nullptr;
    activeCameraShotIndex_ = -1;
    bindingResolver_ = {};
    positionResolver_ = {};
}

bool CinematicPlayer::Load(const std::string& path, SceneManager* sceneManager) {
    Initialize(sceneManager);
    // 前回の再生・所有状態を初期化した後で読み込みます。成功した場合だけ新しいSequenceを所有し、参照先を更新します。
    auto loaded = std::make_unique<CinematicSequence>();
    // JSONの読込が失敗した場合は、設定を所有せずパスを診断へ残す。
    if (!loaded->Load(path)) {
        LOG_ERROR("Cinematic load failed: {}", path);
        return false;
    }
    ownedSequence_ = std::move(loaded);
    sequence_ = ownedSequence_.get();
    return true;
}

bool CinematicPlayer::Prepare() {
    RefreshBindings();
    // 現シーンの参照と素材を準備してから検証します。問題があれば停止して、途中まで適用された設定を復元します。
    const auto issues = ValidateBindings();
    for (const auto& issue : issues) LOG_ERROR("Cinematic preparation failed: {}", issue);
    // 診断がなければ生成物と対象の対応を残したまま、再生できる状態として返す。
    if (issues.empty()) return true;
    Stop(true);
    return false;
}

void CinematicPlayer::SetSequence(CinematicSequence* sequence) {
    // 再生中の演出を停止して姿勢を戻し、新しいシーケンスへ束縛し直す。
    Stop(true);
    // 現在所有する設定と同じポインターを受け取った場合は、参照先を破棄しない。
    if (sequence != ownedSequence_.get()) ownedSequence_.reset();
    sequence_ = sequence;
    playbackTimer_.Seek(0.0f);
    RefreshBindings();
}

void CinematicPlayer::RefreshBindings() {
    Stop(true);
    // 現シーンのObjectを解決し、VFX・音声・アニメーションの実行状態を作り直す。
    anchorCaptured_ = false;
    boundScene_ = sceneManager_ ? sceneManager_->GetCurrentScene() : nullptr;
    transformRuntime_.clear();
    vfxRuntime_.clear();
    audioRuntime_.clear();
    animationRuntime_.clear();

    // 演出または表示中のシーンがなければ、Objectを解決できないので準備を中断する。
    if (!sequence_ || !boundScene_) return;
    // 演出の生成オブジェクトもトラックから参照するため、対象解決より先に準備します。
    bindingsReady_ = true;
    PreparePresentation();

    // 各移動トラックに対象を対応付け、旧軌跡があれば対象のRecorderへ接続する。
    transformRuntime_.resize(sequence_->transformTracks.size());

    for (size_t index = 0; index < sequence_->transformTracks.size(); ++index) {
        const auto& track = sequence_->transformTracks[index];
        auto& runtime = transformRuntime_[index];
        runtime.target = ResolveTarget(track.binding);

        // 対象にRecorderがある場合だけ、旧軌跡の再生に必要なシーンと対象を登録する。
        if (runtime.target && runtime.target->GetRecorder()) {
            runtime.target->GetRecorder()->SetSceneManager(sceneManager_);
            runtime.target->GetRecorder()->SetTarget(runtime.target);

            // 旧軌跡のファイル名が指定されている場合だけ、Recorderに素材を読み込ませる。
            if (!track.legacyPathFile.empty()) {
                runtime.target->GetRecorder()->Load(track.legacyPathFile);
            }
        }
    }

    // VFXは追従先と再生器をトラック単位で用意する。
    vfxRuntime_.resize(sequence_->vfxTracks.size());

    for (size_t index = 0; index < sequence_->vfxTracks.size(); ++index) {
        const auto& track = sequence_->vfxTracks[index];
        auto& runtime = vfxRuntime_[index];
        runtime.target = ResolveTarget(track.binding);
        // 位置固定の素材は発生時に起点を設定するため、準備段階では追従するObjectを渡しません。
        runtime.sequencer.Initialize(track.snapshotTargetPosition ? nullptr : runtime.target);

        // 名前で素材を指定したVFXでは再生器へ読み込み、実素材の長さをトラックへ反映する。
        if (!track.sequenceName.empty()) {
            runtime.sequencer.Load(track.sequenceName);
            sequence_->vfxTracks[index].duration = std::max(0.1f, runtime.sequencer.GetDuration());
        }
    }

    // 音声は発火状態だけ確保し、アニメーションは対象と開始前の復元方針も保持する。
    audioRuntime_.resize(sequence_->audioClips.size());
    animationRuntime_.resize(sequence_->animationClips.size());

    for (size_t index = 0; index < sequence_->animationClips.size(); ++index) {
        animationRuntime_[index].target = ResolveTarget(sequence_->animationClips[index].binding);
        const auto& clip = sequence_->animationClips[index];
        animationRuntime_[index].restoreOnStop = clip.restoreOnStop;

        // 無効化されたクリップの素材は読み込まず、再生するBodyクリップだけを事前に準備する。
        if (clip.driver == "Body" && clip.enabled && !clip.muted) {
            animationRuntime_[index].bodyClip.Load(BodyAnimationClip::ResolveAssetPath(clip.clipName));
        }
    }
}

std::vector<std::string> CinematicPlayer::ValidateBindings() const {
    std::vector<std::string> issues;
    AddPresentationIssues(issues);
    // 現行シーンまたは演出を取得できない場合は、個別トラックの診断を続けられない。
    if (!sequence_ || !sceneManager_ || !sceneManager_->GetCurrentScene()) {
        issues.emplace_back("再生するシーンまたは演出がありません。");
        return issues;
    }

    for (const auto& track : sequence_->transformTracks) {
        // オフまたはミュートの移動トラックは実行対象外なので診断を省く。
        if (!track.enabled || track.muted) {
            continue;
        }

        engine::Object3D* target = ResolveTarget(track.binding);
        // 変形の適用先を解決できない場合は、対象名を直せるようトラック名を報告する。
        if (!target) {
            issues.push_back(track.name + ": 対象Objectが見つかりません。");
        }
        // 姿勢キーも旧軌跡ファイルもない場合は、評価する動きがない。
        if (track.keys.empty() && track.legacyPathFile.empty()) {
            issues.push_back(track.name + ": 移動キーがありません。");
        }
        // アンカー座標を使うトラックでは、基準と対象の両方が親を持たずワールド位置を扱えるか確認する。
        if (track.anchorRelative) {
            engine::Vector3 position{};
            float yaw = 0.0f;
            // 水平アンカーの位置と向きを取得できない場合は基準指定を報告する。
            if (!GetAnchorPose(position, yaw)) {
                issues.push_back(track.name + ": 基準Objectが見つからないか、親を持っています。");
            }
            // 移動先のObjectに親があるとアンカー相対の座標を直接適用できない。
            if (target && target->GetParent()) {
                issues.push_back(track.name + ": 基準移動の対象に親が設定されています。");
            }
        }
    }

    for (const auto& shot : sequence_->cameraShots) {
        // 無効またはミュートのカットは再生しないため、カメラを要求しない。
        if (!shot.enabled || shot.muted) {
            continue;
        }
        engine::Object3D* target = ResolveTarget(shot.binding);
        // 対象が存在しない、カメラ用Objectでない、またはカメラ設定が無効ならカットに使えない。
        if (!target || !target->IsCameraObject() || !target->GetSceneCameraSettings().enabled) {
            issues.push_back(shot.name + ": 有効なCamera Objectが見つかりません。");
        // 候補のカメラを解決できた場合は、参照先や設定の詳しい診断をCameraEditorへ委ねる。
        } else {
            std::string cameraIssue;
            // カメラの個別診断に失敗したら、その理由をカット名とともに返す。
            if (!CameraEditor::GetInstance()->ValidateSceneObjectCamera(target, cameraIssue)) {
                issues.push_back(shot.name + ": " + cameraIssue);
            }
        }
    }

    for (size_t index = 0; index < sequence_->vfxTracks.size(); ++index) {
        const auto& track = sequence_->vfxTracks[index];
        // 無効またはミュートのVFXは発生させないため、開始条件の診断を省く。
        if (!track.enabled || track.muted) {
            continue;
        }
        // 開始Signalが設定されているのに有効な0秒以上の時刻を得られない場合は、名前の設定を報告する。
        if (!track.startSignal.empty() && sequence_->GetSignalTime(track.startSignal, -1) < 0)
            issues.push_back(track.name + ": 開始Signalが見つかりません: " + track.startSignal);
        // 位置固定のVFXでは、素材名と準備済みでイベントを含む再生器の両方が必要。
        if (track.snapshotTargetPosition &&
            (track.sequenceName.empty() || index >= vfxRuntime_.size() ||
             vfxRuntime_[index].sequencer.GetEvents().empty())) {
            issues.push_back(track.name + ": VFX素材を読み込めません。");
        }
        const bool hasPosition = positionResolver_ && positionResolver_(track.binding).has_value();
        // 固定位置が外部の位置解決でも対象Objectでも得られない場合は、そのVFXの起点を報告する。
        if (track.snapshotTargetPosition && !hasPosition && !ResolveTarget(track.binding)) {
            issues.push_back(track.name + ": VFXの発生位置となるObjectが見つかりません。");
        }
    }

    for (const auto& signal : sequence_->signals) {
        // 無効なSignalと対象を指定しないSignalは、Objectへの割り当てを検証しない。
        if (!signal.enabled || (signal.binding.targetName.empty() && signal.binding.targetEventId < 0)) {
            continue;
        }
        // 指定された対象だけを検索し、解決できない場合はSignalの名前を診断に残す。
        if (!ResolveTarget(signal.binding)) {
            issues.push_back(signal.name + ": Signalの対象Objectが見つかりません。");
        }
    }
    return issues;
}

void CinematicPlayer::Play(bool loop, TimerView* timeView) {
    // 任意の時計参照は、再生が失敗した場合に古い時計を残さないよう先に無効化する。
    if (timeView) *timeView = {};
    finished_ = false;
    // 基準姿勢と発火状態を取り直し、時刻0の演出も評価する。
    // 実行中または退避中の姿勢があれば、一度停止して復元してから開始姿勢を取り直す。
    if (playbackTimer_.IsRunning() || isPreviewing_ || presentationCaptured_) Stop(true);
    // 演出とシーン管理の両方が揃っていなければ再生できない。
    if (!sequence_ || !sceneManager_) return;
    // 対象がまだ準備できていないかシーンが切り替わった場合は、現在のシーンに結び付け直す。
    if (!bindingsReady_ || sceneManager_->GetCurrentScene() != boundScene_) RefreshBindings();
    // 対象の結び付けができなかった場合は、開始姿勢や時計を変更せず終了する。
    if (!bindingsReady_) return;
    // 見た目部品・視点キーを持つ演出では、準備条件に違反がある場合は起動しない。
    if ((!sequence_->presentation.objects.empty() || !sequence_->presentation.view.empty()) && !ValidateBindings().empty())
        return;
    // 元の表示と姿勢を記録し、各トラックを時刻0から再生する。
    CapturePresentation();
    CaptureBasePoses();
    ResetRuntimeEvents();
    playbackTimer_.Seek(0.0f);
    isLooping_ = loop;
    playbackTimer_.Resume();
    isPreviewing_ = false;
    // 0秒のイベントも通過判定に含めるため、前回時刻を0秒より1/60秒前として初回評価します。
    Evaluate(0.0f, -1.0f / 60.0f, true);
    // 出力先があり、初回評価の後も再生が続いている場合だけ、有効な時計参照を渡す。
    if (timeView && playbackTimer_.IsRunning()) *timeView = playbackTimer_.GetView();
}

void CinematicPlayer::Resume(bool loop) {
    finished_ = false;
    // 読み込み済みの演出がなければ、再開する時刻や対象がないため終了する。
    if (!sequence_) {
        return;
    }

    // 保持したプレビュー姿勢がなければ、途中から再開できないので先頭から再生する。
    if (!isPreviewing_) {
        Play(loop);
        return;
    }

    // プレビュー時刻を保って時計を進め、音やVFXの発火状態を再開用に戻す。
    isLooping_ = loop;
    isPreviewing_ = false;
    playbackTimer_.Resume();
    ResetRuntimeEvents();

    for (size_t index = 0; index < sequence_->audioClips.size() && index < audioRuntime_.size(); ++index) {
        const auto& clip = sequence_->audioClips[index];
        const float endTime = clip.startTime + std::max(0.05f, clip.duration);
        // 有効で指定音があるループクリップの現在区間内にいる場合だけ、途中から鳴らし直す。
        if (clip.enabled && !clip.muted && clip.loop && !clip.audioId.empty() && playbackTimer_.Elapsed() >= clip.startTime &&
            playbackTimer_.Elapsed() < endTime) {
            // 音声サービスが接続されていれば再生を要求する。区間は開始済みとして記録する。
            if (kPlayAudio) kPlayAudio(clip.audioId, clip.volume, true);
            audioRuntime_[index].started = true;
        }
    }
}

void CinematicPlayer::Pause() {
    // 時計が止まっている場合は、停止済みの音とVFXを操作し直さない。
    if (!playbackTimer_.IsRunning()) {
        return;
    }

    // 表示中の姿勢を保持し、動き続けるVFXと音声だけを止める。
    for (auto& runtime : vfxRuntime_) {
        runtime.sequencer.Stop();
        runtime.started = false;
    }

    StopAudioClips();
    playbackTimer_.Pause();
    isPreviewing_ = true;
}

void CinematicPlayer::Stop(bool restorePose) {
    springStates_.clear();
    // 停止時はVFX・音・カメラを解除し、必要なら開始前の姿勢へ戻す。
    for (auto& runtime : vfxRuntime_) {
        runtime.sequencer.Stop();
        runtime.started = false;
    }

    StopAudioClips();
    StopCameraTrack();

    // 姿勢復元を要求された場合だけ、開始前の変形・表示・カメラなどを復元する。
    if (restorePose) {
        RestoreBasePoses();
        ReleasePresentation(true);
    }

    // 停止後は時計を先頭で止め、プレビューと直近イベントの状態を消す。
    playbackTimer_.Pause();
    isPreviewing_ = false;
    finished_ = false;
    playbackTimer_.Seek(0.0f);
    lastEventId_ = 0;
    lastEventTarget_ = nullptr;
}

void CinematicPlayer::Update(float deltaTime) {
    BaseScene* currentScene = sceneManager_ ? sceneManager_->GetCurrentScene() : nullptr;

    // 現在のシーンと対象を解決したシーンが異なれば、古い対象の評価をやめる。
    if (currentScene != boundScene_) {
        // 別シーンの Object 参照を持ち越さないよう、姿勢復元せず再束縛する。
        Stop(false);
        RefreshBindings();
        return;
    }

    // 再生中かつ演出が有効で、増分が有限の正の秒数の場合だけ時刻を進める。
    if (!playbackTimer_.IsRunning() || !sequence_ || !std::isfinite(deltaTime) || deltaTime <= 0.0f) {
        return;
    }

    const float duration = GetDuration();
    const float previousTime = playbackTimer_.Elapsed();
    float nextTime = playbackTimer_.Elapsed() + deltaTime;

    // フレームの加算時間が終端に届いた場合は、通常評価から周回または完走へ切り替える。
    if (nextTime >= duration) {
        // 周回する設定なら、現周の末尾と新しい周の先頭を分けて評価する。
        if (isLooping_) {
            // 末尾のイベントまで発火させてから基準姿勢を取り直し、次周の先頭を評価する。
            Evaluate(duration, previousTime, true);
            RestoreBasePoses();
            CaptureBasePoses();
            ResetRuntimeEvents();
            // 終端を越えた秒数を周期内へ折り返し、フレームで使い切れなかった時間を次周へ渡します。
            nextTime = std::fmod(nextTime, std::max(duration, 0.1f));
            playbackTimer_.Seek(0.0f);
            // 前回時刻を0秒より前に置き、前回時刻を含まない通過判定でも0秒のイベントを拾います。
            Evaluate(nextTime, -1.0f / 60.0f, true);
            playbackTimer_.Seek(nextTime);
            return;
        }

        // 単発再生は終端まで評価して、最後のコールバック発火や姿勢を確定する。
        Evaluate(duration, previousTime, true);
        // 終端イベントのコールバックから停止される場合があります。そのときは停止後の状態を終端プレビューで上書きしません。
        if (!playbackTimer_.IsRunning()) return;
        playbackTimer_.Seek(duration);
        playbackTimer_.Pause();
        finished_ = true;

        // 終端ポーズを保持したプレビューとして扱い、再編集時にも開始前へ復元する。
        isPreviewing_ = true;
        return;
    }

    // 終端までは通過したイベントを評価し、通常どおり時計を更新する。
    Evaluate(nextTime, previousTime, true);
    playbackTimer_.Seek(nextTime);
}

void CinematicPlayer::Step(float deltaTime, const FramePreparation& prepare) {
    // NaNや無限大の秒数は時計に加算できないため、編集ステップを無視する。
    if (!std::isfinite(deltaTime)) return;
    SetTime(playbackTimer_.Elapsed() + std::max(0.0f, deltaTime), deltaTime > 0.0f, prepare);
}

void CinematicPlayer::SetTime(float timeSeconds, bool dispatchEvents, const FramePreparation& prepare) {
    finished_ = false;
    // シーク時のイベント再発火は呼び出し側に選ばせ、姿勢は指定時刻から再評価する。
    // 演出データがなければシークで新しい姿勢を評価できない。
    if (!sequence_) {
        return;
    }

    // 再生・プレビューのどちらでもなければ、評価を始める前に復元用の姿勢を退避する。
    if (!playbackTimer_.IsRunning() && !isPreviewing_) {
        BeginPreview();
    }

    // 指定秒が非有限ならクランプで有効な時刻を決められないため評価を省く。
    if (!std::isfinite(timeSeconds)) return;
    const float nextTime = std::clamp(timeSeconds, 0.0f, GetDuration());
    const float previousTime = playbackTimer_.Elapsed();
    // 前回より前の時刻へ移る場合は、巻き戻した区間の発火済み状態を持ち越さない。
    if (nextTime < previousTime) {
        // 巻き戻した後の再生でVFXと音が再び発火できるよう、実行状態を戻す。
        for (auto& runtime : vfxRuntime_) {
            runtime.sequencer.Stop();
            runtime.started = false;
        }
        StopAudioClips();
    }
    // 外部の準備も同じ時計を見る。対象の材質・能力・床を確定してからVFXを評価する。
    // 評価前処理が指定された場合だけ、その処理から同じ時計を読めるよう先に移動する。
    if (prepare) {
        playbackTimer_.Seek(nextTime);
        // 準備側が中断を要求した場合は、新しい時計位置のままトラック評価を行わず返る。
        if (!prepare(previousTime, nextTime)) return;
    }
    Evaluate(nextTime, previousTime, dispatchEvents);
    playbackTimer_.Seek(nextTime);
}

void CinematicPlayer::BeginPreview() {
    // 編集前の姿勢を一度だけ保存し、シーク終了時に復元できるようにする。
    if (playbackTimer_.IsRunning() || isPreviewing_ || !sequence_) {
        return;
    }

    // 対象が未準備か別シーンなら、退避前に現在のシーンから対象を探し直す。
    if (!bindingsReady_ || sceneManager_->GetCurrentScene() != boundScene_) RefreshBindings();
    // 結び付けができなかった場合は、保存とプレビュー開始を行わない。
    if (!bindingsReady_) return;
    // 見た目部品や視点キーの有効性を確認できない場合は、編集対象を変更する前に終了する。
    if ((!sequence_->presentation.objects.empty() || !sequence_->presentation.view.empty()) && !ValidateBindings().empty())
        return;
    CapturePresentation();
    CaptureBasePoses();
    ResetRuntimeEvents();
    isPreviewing_ = true;
}

void CinematicPlayer::EndPreview(bool restorePose) {
    // 終了経路をStopへ集約し、VFX・音・姿勢の片付けを揃える。
    if (isPreviewing_) Stop(restorePose);
}

float CinematicPlayer::GetDuration() const {
    // JSONの指定時間より長い実VFXや旧Ghost軌跡も末尾に含める。
    if (!sequence_) {
        return 0.1f;
    }

    float result = sequence_->GetAuthoredDuration();

    // 変形トラックの尺へ旧Recorderの長さも含め、各トラックの開始秒から換算する。
    for (size_t index = 0; index < sequence_->transformTracks.size(); ++index) {
        result = std::max(result, sequence_->transformTracks[index].startTime + GetTransformTrackDuration(index));
    }

    // 読込済みVFXの実際の再生尺が長ければ、Signalも解決した発生秒から終端を延ばす。
    for (size_t index = 0; index < sequence_->vfxTracks.size() && index < vfxRuntime_.size(); ++index) {
        result = std::max(result, sequence_->GetVfxStartTime(sequence_->vfxTracks[index]) + vfxRuntime_[index].sequencer.GetDuration());
    }

    return std::max(result, 0.1f);
}

bool CinematicPlayer::GetBasePose(engine::Object3D* object, engine::Vector3& position, engine::Vector3& rotation, engine::Vector3& scale) const {
    // 編集前に取得した姿勢だけ返し、未束縛Objectの値は触らない。
    if (!object) {
        return false;
    }

    for (const auto& runtime : transformRuntime_) {
        // 同じObjectの開始姿勢を退避している場合だけ、出力引数へ値を書き込む。
        if (runtime.target == object && runtime.captured) {
            position = runtime.basePosition;
            rotation = runtime.baseRotation;
            scale = runtime.baseScale;
            return true;
        }
    }

    return false;
}

engine::Object3D* CinematicPlayer::ResolveTarget(const CinematicObjectBinding& binding) const {
    // 演出固有のObjectがある場合は、同名のシーンObjectより先に検索する。
    if (sequence_) {
        for (size_t i = 0; i < presentationObjects_.size() && i < sequence_->presentation.objects.size(); ++i)
            // 名前の一致に加えて再生器が所有する対象だけを、演出固有の対象として返す。
            if (sequence_->presentation.objects[i].name == binding.targetName && presentationObjects_[i].owned)
                return presentationObjects_[i].target;
    }
    // 専用Resolverを優先し、通常はEvent IDを名前より優先して現シーン内を探す。
    if (bindingResolver_)
        return bindingResolver_(binding);
    // 役割は制作空間でも本番でも同じ名前を使い、本番だけ保存済みの対象へ解決する。
    auto actual=binding;
    // シーケンスに保存された役割名を、通常のObject識別子へ変換する。
    if (sequence_) {
        const char* roles[] = {"$Actor", "$Gate", "$Camera"};
        for (size_t depth=0;depth<=3;++depth) {
            size_t i=0;
            while (i<3 && actual.targetName!=roles[i]) ++i;
            // 予約済みの役割名に一致しなくなれば、実Objectの名前またはIDとして検索へ渡す。
            if (i==3) break;
            if (depth==3) return nullptr; // 3回たどっても役割名が続く場合は、循環する割り当てとして拒否する。
            actual=sequence_->roleBindings[i];
        }
    }
    return ResolveSceneTarget(sceneManager_ ? sceneManager_->GetCurrentScene() : nullptr, actual);
}

engine::Object3D* CinematicPlayer::ResolveSceneTarget(BaseScene* scene, const CinematicObjectBinding& binding) {
    // 検索対象のシーンがなければ、借用できるObjectはない。
    if (!scene) return nullptr;
    engine::Object3D* nameMatch = nullptr;
    for (const auto& object : scene->GetObjects()) {
        // 空のObject枠はIDや名前を参照できないので無視する。
        if (!object) continue;
        // 有効なイベントIDが一致すれば、名前候補より優先して見つけた時点で返す。
        if (binding.targetEventId >= 0 && object->GetEventId() == binding.targetEventId) return object.get();
        // 名前が指定されている場合は同名の候補を保持し、最後までIDの一致を探す。
        if (!binding.targetName.empty() && object->GetName() == binding.targetName) nameMatch = object.get();
    }
    return nameMatch;
}

float CinematicPlayer::GetTransformTrackDuration(size_t index) const {
    // キー列が無い旧データではGhost Recorderの長さを使う。
    // 演出がないか要求された番号のトラックが存在しなければ、有効な再生尺は0秒。
    if (!sequence_ || index >= sequence_->transformTracks.size()) {
        return 0.0f;
    }

    const auto& track = sequence_->transformTracks[index];

    // キーがある場合は末尾の相対秒を使用し、旧Recorderの尺は参照しない。
    if (!track.keys.empty()) {
        return std::max(0.0f, track.keys.back().time);
    }

    // キーがない旧形式では、対象のRecorderとファイル指定が揃っている場合だけ再生尺を借りる。
    if (index < transformRuntime_.size() && transformRuntime_[index].target &&
        transformRuntime_[index].target->GetRecorder() && !track.legacyPathFile.empty()) {
        return transformRuntime_[index].target->GetRecorder()->GetDurationSeconds();
    }

    return 0.0f;
}

void CinematicPlayer::CaptureBasePoses() {
    // 演出で動かすObjectと描画用アニメーションの開始姿勢を保存する。
    anchorCaptured_ = false;
    anchorCaptured_ = GetAnchorPose(anchorPosition_, anchorYaw_);

    for (auto& runtime : transformRuntime_) {
        // 対象が解決していなければ、退避できる変形がないため次のトラックへ進む。
        if (!runtime.target) {
            continue;
        }

        runtime.basePosition = runtime.target->GetTranslate();
        runtime.baseRotation = runtime.target->GetRotation();
        runtime.baseScale = runtime.target->GetScale();
        runtime.captured = true;

        // Recorderがある対象では、旧記録を相対再生するための基準も取り直す。
        if (runtime.target->GetRecorder()) {
            runtime.target->GetRecorder()->CaptureBasePose();
        }
    }

    for (auto& runtime : animationRuntime_) {
        // アニメーション対象が未解決なら、再生状態や描画姿勢を取得しない。
        if (!runtime.target) {
            continue;
        }

        // 停止時に元へ戻せるよう、Animatorと旧方式の再生情報を退避します。Bodyクリップ用には描画だけの変形も保存します。
        runtime.animatorSnapshot = runtime.target->CaptureAnimatorSnapshot();
        runtime.legacyAnimationName = runtime.target->GetAnimationName();
        runtime.legacyAnimationTime = runtime.target->GetAnimationTime();
        runtime.legacyLoop = runtime.target->IsAnimationLoop();

        // 再生可能なBodyクリップでは停止時に戻す描画姿勢も必要になる。
        if (runtime.bodyClip.IsValid()) {
            // MeshRendererがある場合だけ、拡縮・回転・移動・せん断を戻せるよう保存する。
            if (auto* renderer = runtime.target->GetMeshRenderer()) {
                runtime.baseVisual.scale = renderer->GetVisualScale();
                runtime.baseVisual.rotate = renderer->GetVisualRotation();
                runtime.baseVisual.translate = renderer->GetVisualOffset();
                runtime.baseVisual.shear = renderer->GetVisualShear();
                runtime.baseVisual.shearPivotY = renderer->GetVisualShearPivotY();
                runtime.bodyCaptured = true;
            }
        }

        runtime.captured = true;
    }
}

void CinematicPlayer::RestoreBasePoses() {
    anchorCaptured_ = false;

    // 編集でクリップが削除・並べ替えされても、保存した対象から復元する。
    if (!sceneManager_ || sceneManager_->GetCurrentScene() != boundScene_ || !boundScene_)
        return;
    const auto live = GetLiveTargets();

    // 位置・回転・拡縮を先に戻し、その後で描画専用の変形やアニメーションの再生状態を復元します。
    for (auto& runtime : transformRuntime_) {
        // 対象が空、姿勢が未退避、またはすでに削除された場合は復元できない。
        if (!runtime.target || !runtime.captured || !live.contains(runtime.target)) {
            continue;
        }

        ApplyPose(runtime.target, runtime.basePosition, runtime.baseRotation, runtime.baseScale);
        runtime.captured = false;
    }

    for (auto& runtime : animationRuntime_) {
        // 復元が指定され、退避済みで今もシーンに存在する対象に限ってアニメーションを戻す。
        if (!runtime.target || !runtime.captured || !runtime.restoreOnStop || !live.contains(runtime.target)) {
            continue;
        }

        // 本体クリップが描画だけを変えた場合は、アニメーターの状態を切り替えずメッシュの変形だけを戻します。
        if (runtime.bodyCaptured) {
            // 描画部品がまだ取得できる場合だけ、保存した見た目へ戻す。
            if (auto* renderer = runtime.target->GetMeshRenderer()) {
                const auto& base = runtime.baseVisual;
                renderer->SetVisualTransform(base.scale, base.rotate, base.translate);
                renderer->SetVisualShear(base.shear, base.shearPivotY);
            }

            runtime.bodyCaptured = false;
        // Body姿勢の退避がなく、Animatorの状態を保存できていた場合はコントローラーを復元する。
        } else if (runtime.animatorSnapshot.valid) {
            // コントローラーがある対象は状態名と再生時刻、遷移の進み具合をまとめて戻します。
            runtime.target->RestoreAnimatorSnapshot(runtime.animatorSnapshot);
        // Body姿勢にもAnimatorにも保存状態がなければ、旧式の再生情報を使う。
        } else {
            // 旧方式ではクリップ名と秒数、ループ指定を使って再生を元に戻します。
            runtime.target->RestoreAnimationPlayback(runtime.legacyAnimationName, runtime.legacyAnimationTime,
                                                     runtime.legacyLoop);
        }

        runtime.captured = false;
    }
}

void CinematicPlayer::ResetRuntimeEvents() {
    springStates_.clear();
    // ループの次周で一回限りのVFX・音・イベントが再び発火できるようにする。
    for (auto& runtime : vfxRuntime_) {
        runtime.sequencer.Stop();
        runtime.started = false;
    }

    for (auto& runtime : audioRuntime_) {
        runtime.started = false;
    }

    lastEventId_ = 0;
    lastEventTarget_ = nullptr;
}

void CinematicPlayer::Evaluate(float timeSeconds, float previousTimeSeconds, bool dispatchEvents) {
    // 姿勢は時刻から毎回再評価しますが、イベントは通過した区間だけ発火させます。
    // エディターのシークではdispatchEventsを切り、同じイベントの重複実行を避けます。
    lastEventId_ = 0;
    lastEventTarget_ = nullptr;
    EvaluatePresentation(timeSeconds, previousTimeSeconds, dispatchEvents);

    for (size_t index = 0; sequence_ && index < sequence_->transformTracks.size(); ++index) {
        EvaluateTransformTrack(index, timeSeconds, previousTimeSeconds, dispatchEvents);
    }

    // トラックで対象の基本姿勢を置いた後、同じ再生時刻のアニメーションを評価します。
    EvaluateAnimationClips(timeSeconds);

    // 通知を許可された評価だけ、時刻を通過したイベントやSignalを発火する。
    if (dispatchEvents) {
        for (const auto& marker : sequence_->events) {
            // 通過区間は (前回秒, 今回秒] です。境界ちょうどのイベントは到達フレームで拾い、次のフレームでは拾い直しません。
            if (marker.time > previousTimeSeconds && marker.time <= timeSeconds) {
                engine::Object3D* target = ResolveTarget(marker.binding);

                // 対象付きでイベントIDが0以外の場合だけ、Objectにイベントを渡す。直近の通過情報は下で記録する。
                if (target && marker.eventId != 0) {
                    target->OnRecordEvent(marker.eventId);
                }

                lastEventId_ = marker.eventId;
                lastEventTarget_ = target;
            }
        }

        DispatchSignals(timeSeconds, previousTimeSeconds);
    }

    // 音とVFXは実再生中だけ進め、シークでプレビューの副作用を増やしません。
    if (playbackTimer_.IsRunning() && dispatchEvents) {
        UpdateVFXTracks(timeSeconds, previousTimeSeconds);
        UpdateAudioClips(timeSeconds, previousTimeSeconds);
    }

    UpdateCameraTrack(timeSeconds);
}

void CinematicPlayer::EvaluateTransformTrack(size_t index, float timeSeconds, float previousTimeSeconds,
                                             bool dispatchEvents) {
    // シーケンスかトラック定義・実行状態のどれかを取得できなければ評価しない。
    if (!sequence_ || index >= sequence_->transformTracks.size() || index >= transformRuntime_.size()) {
        return;
    }

    const auto& track = sequence_->transformTracks[index];
    auto& runtime = transformRuntime_[index];

    // 姿勢を適用するObjectを解決できていなければ、時刻計算を続けない。
    if (!runtime.target) {
        return;
    }

    // シーケンス全体の秒数から開始秒を引き、トラック先頭を0秒にします。
    const float localTime = timeSeconds - track.startTime;
    const float previousLocalTime = previousTimeSeconds - track.startTime;
    const float trackDuration = GetTransformTrackDuration(index);
    const float evaluateTime = std::clamp(localTime, 0.0f, trackDuration);

    // 無効・開始前・保持しない終端は、同じ開始姿勢への復元経路を使う。
    if (!track.enabled || track.muted || localTime < 0.0f ||
        (localTime > trackDuration && !track.holdLast) ||
        (!track.keys.empty() && evaluateTime < track.keys.front().time)) {
        // 開始前の姿勢を退避済みなら、対象に残った前フレームの変形を戻す。
        if (runtime.captured) ApplyPose(runtime.target, runtime.basePosition, runtime.baseRotation, runtime.baseScale);
        return;
    }

    // アニメーションキーがないトラックは、旧形式のRecorderによる評価だけを試す。
    if (track.keys.empty()) {
        // 旧Path Recorder素材はキー配列を持たないため、記録済み軌道へ評価を委譲する。
        if (runtime.target->GetRecorder() && !track.legacyPathFile.empty()) {
            runtime.target->GetRecorder()->EvaluateAtTime(evaluateTime, dispatchEvents, previousLocalTime);
        }

        return;
    }

    // キー付きの軌道はトラック内の秒数をサンプルし、相対再生だけ基準点の位置とヨーを合成します。
    auto value = SampleCinematicTransform(track.keys, evaluateTime);

    // 水平アンカー基準の指定を、対象の開始姿勢に対する相対指定より優先する。
    if (track.anchorRelative) {
        engine::Vector3 origin;
        float yaw = 0.0f;

        // 対象が親付き、またはアンカーのワールド位置と向きを取得できない場合は適用しない。
        if (runtime.target->GetParent() || !GetAnchorPose(origin, yaw))
            return;
        value.position = CinematicAnchorPosition(value.position, origin, yaw);
        value.rotation.y += yaw;
        ApplyPose(runtime.target, value.position, value.rotation, value.scale);
        return;
    }

    // 開始姿勢に対する相対指定なら位置と回転を足し、拡縮を掛ける。未指定ではキーを直接適用する。
    ApplyPose(runtime.target, track.relative ? AddVector3(runtime.basePosition, value.position) : value.position,
              track.relative ? AddVector3(runtime.baseRotation, value.rotation) : value.rotation,
              track.relative ? MultiplyVector3(runtime.baseScale, value.scale) : value.scale);
}

void CinematicPlayer::ApplyBodyPose() {
    // 再生かプレビューが継続し、対象を保存したシーンが現在も有効な場合だけ姿勢を保持する。
    if ((playbackTimer_.IsRunning() || isPreviewing_) && sceneManager_ && sceneManager_->GetCurrentScene() == boundScene_) {
        // 編集用の基準移動も通常更新後に保持する。イベントは再発火しない。
        for (size_t index = 0; sequence_ && index < sequence_->transformTracks.size(); ++index) {
            // アンカー基準で同じ対象を解決できるトラックを、物理更新後に副作用なしで再評価する。
            if (sequence_->transformTracks[index].anchorRelative && index < transformRuntime_.size() &&
                transformRuntime_[index].target == ResolveTarget(sequence_->transformTracks[index].binding))
                EvaluateTransformTrack(index, playbackTimer_.Elapsed(), playbackTimer_.Elapsed(), false);
        }

        // 身体の描画変形と演出Objectの表示を同じ秒で再評価する。
        EvaluateAnimationClips(playbackTimer_.Elapsed(), true);
        EvaluatePresentation(playbackTimer_.Elapsed(), playbackTimer_.Elapsed(), false);
    }
}

bool CinematicPlayer::GetAnchorPose(engine::Vector3& position, float& yaw) const {
    // 再生開始時に固定したアンカーを優先し、無ければ現シーンのObjectから求める。
    if (anchorCaptured_) {
        position = anchorPosition_;
        yaw = anchorYaw_;
        return true;
    }

    engine::Object3D* anchor = sequence_ ? ResolveTarget(sequence_->anchor) : nullptr;
    // アンカーがないか親付きなら、入力を水平ワールド座標として使えないため失敗する。
    if (!anchor || anchor->GetParent())
        return false;
    position = anchor->GetTranslate();
    yaw = anchor->GetRotation().y + sequence_->anchorYawOffset;
    return true;
}

void CinematicPlayer::EvaluateAnimationClips(float timeSeconds, bool bodyOnly) {
    // 演出データがない場合は、アニメーションの区間を選べない。
    if (!sequence_) {
        return;
    }

    // 毎回基準へ戻してから評価するため、逆方向のシークや区間外にも変形を残さない。
    // 同じ対象で区間が重なる場合は、リストの後の身体クリップを優先する。
    for (size_t index = 0; index < animationRuntime_.size(); ++index) {
        auto& runtime = animationRuntime_[index];

        // シーン内で削除された対象へ保存ポインタからアクセスしない。
        if (index >= sequence_->animationClips.size() ||
            runtime.target != ResolveTarget(sequence_->animationClips[index].binding))
            continue;
        // 対象があり描画用の姿勢を退避済みの場合は、次のサンプルを適用する前に基準へ戻す。
        if (runtime.target && runtime.bodyCaptured) {
            // 描画部品がまだ存在する場合にだけ、退避した値を適用する。
            if (auto* renderer = runtime.target->GetMeshRenderer()) {
                const auto& base = runtime.baseVisual;
                renderer->SetVisualTransform(base.scale, base.rotate, base.translate);
                renderer->SetVisualShear(base.shear, base.shearPivotY);
            }
        }
    }

    for (size_t index = 0; index < sequence_->animationClips.size(); ++index) {
        const auto& clip = sequence_->animationClips[index];
        // 身体だけを保持する評価では、他ドライバーのクリップを走らせない。
        if (bodyOnly && clip.driver != "Body")
            continue;
        // 無効またはミュートのクリップは時間内でも適用しない。
        if (!clip.enabled || clip.muted) {
            continue;
        }

        const float duration = std::max(0.05f, clip.duration);
        const float elapsed = timeSeconds - clip.startTime;

        // 開始前または終了後は区間外なので、基準に戻した状態のまま次を調べる。
        if (elapsed < 0.0f || elapsed > duration) {
            continue;
        }

        // 区間の長さとブレンドは演出の経過秒で測り、姿勢のサンプル時刻にだけ再生速度を掛けます。
        float localTime = elapsed * std::max(0.0f, clip.playbackSpeed);

        // Bodyクリップにはループ指定を渡し、素材自身の周期で折り返させます。他の系統はこの区間の長さで折り返します。
        if (clip.driver != "Body" && clip.loop && duration > 0.0f) {
            localTime = std::fmod(localTime, duration);
        }

        engine::Object3D* target =
            index < animationRuntime_.size() ? animationRuntime_[index].target : ResolveTarget(clip.binding);
        // 対象が結び付け時から変わっていれば、古いキャッシュを使わず次を評価する。
        if (target != ResolveTarget(clip.binding))
            continue;
        // Bodyクリップでは対象と実行状態が揃っている場合に限って、描画用サンプルを求める。
        if (target && clip.driver == "Body" && index < animationRuntime_.size()) {
            auto& runtime = animationRuntime_[index];
            BodyAnimationClip::Sample sample;
            auto* renderer = target->GetMeshRenderer();

            // 描画部品と基準姿勢があり、有効な姿勢を評価できた場合だけ見た目を上書きする。
            if (renderer && runtime.bodyCaptured && runtime.bodyClip.Evaluate(localTime, clip.loop, sample)) {
                const float weight =
                    clip.blendInDuration > 0.0f
                        ? animationInterpolation::ApplyEasing(std::clamp(elapsed / clip.blendInDuration, 0.0f, 1.0f),
                                                              ToEasingType(clip.easing))
                        : 1.0f;
                const auto& base = runtime.baseVisual;

                // 物理座標・当たり判定には触れず、描画用の変形だけを上書きする。
                renderer->SetVisualTransform(animationInterpolation::Lerp(base.scale, sample.scale, weight),
                                             animationInterpolation::SlerpEuler(base.rotate, sample.rotate, weight),
                                             animationInterpolation::Lerp(base.translate, sample.translate, weight));
                renderer->SetVisualShear(animationInterpolation::Lerp(base.shear, sample.shear, weight),
                                         base.shearPivotY + (sample.shearPivotY - base.shearPivotY) * weight);
            }
        }

        // Animator系または省略されたドライバーでは、解決したObjectのモデル再生へ時刻とブレンド設定を渡す。
        if (target &&
            (clip.driver.empty() || clip.driver == "Animator" || clip.driver == "Model" || clip.driver == "Skeletal")) {
            target->EvaluateAnimatorState(clip.clipName, localTime, clip.loop, clip.blendInDuration, clip.easing,
                                          isPreviewing_);
        }

        // 通常評価でコールバックが登録されている場合だけ、呼び出し元へクリップ評価を通知する。
        if (animationCallback_ && !bodyOnly) {
            animationCallback_(target, clip, localTime, isPreviewing_);
        }
    }
}

void CinematicPlayer::DispatchSignals(float timeSeconds, float previousTimeSeconds) {
    // 前回から今回までに通過した有効なSignalだけ通知する。
    if (!sequence_ || !signalCallback_ || timeSeconds < previousTimeSeconds) {
        return;
    }

    for (const auto& signal : sequence_->signals) {
        // 無効か (前回秒, 今回秒] の外にあるSignalは、今回の通知から除く。
        if (!signal.enabled || signal.time <= previousTimeSeconds || signal.time > timeSeconds) {
            continue;
        }

        signalCallback_(ResolveTarget(signal.binding), signal, isPreviewing_);
    }
}

void CinematicPlayer::UpdateVFXTracks(float timeSeconds, float previousTimeSeconds) {
    // 開始時刻を飛び越えたフレームでは経過分だけ追いつかせる。
    // 演出データがなければ、発火させるVFXトラックがない。
    if (!sequence_) {
        return;
    }

    for (size_t index = 0; index < sequence_->vfxTracks.size() && index < vfxRuntime_.size(); ++index) {
        const auto& track = sequence_->vfxTracks[index];
        auto& runtime = vfxRuntime_[index];

        // 無効、ミュート、または素材名なしのトラックは発生させない。
        if (!track.enabled || track.muted || track.sequenceName.empty()) {
            continue;
        }

        const float startTime = sequence_->GetVfxStartTime(track);
        // まだ開始していないトラックが開始時刻に達したら、今回のフレームで一度だけ起動する。
        if (!runtime.started && timeSeconds >= startTime) {
            // 位置固定のトラックでは、発生直前にワールドの起点を決めてObjectから切り離す。
            if (track.snapshotTargetPosition) {
                std::optional<engine::Vector3> position;
                // 外部から位置を受け取る処理があれば、Objectなしの演出地点も解決できるよう先に試す。
                if (positionResolver_) {
                    position = positionResolver_(track.binding);
                }
                // 外部で位置が得られず対象Objectがある場合は、その姿勢を起点にする。
                if (!position && runtime.target) {
                    // 一時Objectの姿勢はまだ行列へ反映されていない場合がある。
                    position = runtime.target->GetParent()
                        ? runtime.target->GetWorldPosition() : runtime.target->GetTranslate();
                }
                // 両方から起点を取れない場合は、このフレームでは発生を開始しない。
                if (!position) {
                    continue;
                }
                // 開始位置を固定するクリップは追従先を外し、現在位置に素材のオフセットを足して起点を一度だけ決めます。
                runtime.sequencer.SetTargetObject(nullptr);
                runtime.sequencer.SetRootPosition(*position + track.rootOffset);
                runtime.sequencer.SetRootScale(track.rootScale);
            // 位置を固定しないトラックは、対象の位置を追従先から取得する。
            } else {
                runtime.sequencer.SetTargetObject(runtime.target);
            }
            // 起点と倍率を決めてから再生を開始し、開始時刻をまたいだ分だけ時間を進める。
            runtime.sequencer.Play();
            runtime.started = true;
            const float catchUp = std::max(0.0f, timeSeconds - startTime);

            // 開始の境界を越えた場合だけ余った秒数を渡す。境界ちょうどでは起動だけ行う。
            if (catchUp > 0.0f) {
                runtime.sequencer.Update(catchUp);
            }
        // 開始済みのトラックは、前回から増えた秒だけ再生器を進める。
        } else if (runtime.started) {
            runtime.sequencer.Update(std::max(0.0f, timeSeconds - previousTimeSeconds));
        }
    }
}

void CinematicPlayer::UpdateAudioClips(float timeSeconds, float previousTimeSeconds) {
    // 開始境界を通過したClipだけ鳴らし、ループ音は終了時刻で止める。
    // 演出データがなければ、発火する音声クリップを取得できない。
    if (!sequence_) {
        return;
    }

    for (size_t index = 0; index < sequence_->audioClips.size() && index < audioRuntime_.size(); ++index) {
        const auto& clip = sequence_->audioClips[index];
        auto& runtime = audioRuntime_[index];

        // 無効、ミュート、音声IDなしのクリップは音声サービスへ通知しない。
        if (!clip.enabled || clip.muted || clip.audioId.empty()) {
            continue;
        }

        const float endTime = clip.startTime + std::max(0.05f, clip.duration);
        const bool crossedStart = clip.startTime > previousTimeSeconds && clip.startTime <= timeSeconds;

        // 大きな時間ステップでループ音の終了時刻も通り過ぎたときは、無意味な開始と停止を避けます。
        if (!runtime.started && crossedStart && (!clip.loop || timeSeconds < endTime)) {
            // 再生処理が登録されているときだけ発音を要求し、開始境界は消費済みにする。
            if (kPlayAudio) kPlayAudio(clip.audioId, clip.volume, clip.loop);
            runtime.started = true;
        }

        // 開始済みのループ音が終端へ到達したら、次フレームへ鳴り続けないよう止める。
        if (runtime.started && clip.loop && timeSeconds >= endTime) {
            // 停止処理が接続されている場合だけ、音声IDを渡して停止を要求する。
            if (kStopAudio) kStopAudio(clip.audioId);
            runtime.started = false;
        }
    }
}

void CinematicPlayer::StopAudioClips() {
    // PauseやStop後にシーケンス由来のループ音を残さない。
    // 演出データがなければ、停止対象の音声IDを取り出せない。
    if (!sequence_) {
        return;
    }

    for (size_t index = 0; index < sequence_->audioClips.size() && index < audioRuntime_.size(); ++index) {
        // 開始していないクリップには、停止する演出由来の音がない。
        if (!audioRuntime_[index].started) {
            continue;
        }

        const auto& clip = sequence_->audioClips[index];

        // 空の音声IDは停止対象を特定できないため、サービスには渡さない。
        if (!clip.audioId.empty()) {
            // 停止処理が接続されていれば、開始済みの音をIDで止める。
            if (kStopAudio) kStopAudio(clip.audioId);
        }

        audioRuntime_[index].started = false;
    }
}

void CinematicPlayer::UpdateCameraTrack(float timeSeconds) {
    // 重なるカットでは開始時刻が最も新しい有効ショットを採用します。
    if (!sequence_) {
        // 演出データがなければ、切り替えるカメラ候補がない。
        return;
    }

    engine::Object3D* requestedCamera = nullptr;
    float requestedStartTime = -1.0f;
    std::int32_t requestedShotIndex = -1;

    // 明示のカメラカットを持つ素材では、同じ演出の変形トラックを候補に混ぜない。
    if (!sequence_->cameraShots.empty()) {
        for (size_t index = 0; index < sequence_->cameraShots.size(); ++index) {
            const auto& shot = sequence_->cameraShots[index];

            // 無効またはミュートのカットは優先順の比較から除く。
            if (!shot.enabled || shot.muted) {
                continue;
            }

            const float endTime = shot.startTime + std::max(0.05f, shot.duration);

            // 時刻がカットの開始から終了までに含まれ、今の候補より開始が新しい場合に採用を試みる。
            if (timeSeconds >= shot.startTime && timeSeconds <= endTime && shot.startTime >= requestedStartTime) {
                engine::Object3D* target = ResolveTarget(shot.binding);

                // 対象を解決でき、カメラ用Objectである場合だけ候補を更新する。
                if (target && target->IsCameraObject()) {
                    requestedCamera = target;
                    requestedStartTime = shot.startTime;
                    requestedShotIndex = static_cast<std::int32_t>(index);
                }
            }
        }
    // カメラカットが1件もない場合は、変形トラックの旧挙動を使う。
    } else {
        // v3以前のSequenceはCamera ObjectのTransform Trackをカメラカットとして扱います。
        for (size_t index = 0; index < sequence_->transformTracks.size() && index < transformRuntime_.size(); ++index) {
            const auto& track = sequence_->transformTracks[index];
            engine::Object3D* target = transformRuntime_[index].target;

            // 有効な変形トラックでもカメラ対象がなければ候補にしない。
            if (!track.enabled || track.muted || !target || !target->IsCameraObject()) {
                continue;
            }

            const float endTime = track.startTime + GetTransformTrackDuration(index);
            // 開始に達し、終端内か終端保持で、今の候補より開始が遅い旧カメラを選ぶ。
            if (timeSeconds >= track.startTime && (track.holdLast || timeSeconds <= endTime) &&
                track.startTime >= requestedStartTime) {
                requestedCamera = target;
                requestedStartTime = track.startTime;
            }
        }
    }

    // Objectもカット番号も同じであれば、進行中のカメラブレンドを起動し直さない。
    if (requestedCamera == activeCameraObject_ && requestedShotIndex == activeCameraShotIndex_) {
        return;
    }

    // 次のショットがある場合はカメラが切り替わることを解除側へ伝え、旧ショットの解除後に新しいショットを開始します。
    StopCameraTrack(requestedCamera != nullptr);

    // 次の有効なカメラが見つかった場合だけ、前の上書きを解除したあとで切り替える。
    if (requestedCamera) {
        bool started = false;

        // 明示カットでは、その区間が持つブレンド時間と補間曲線を使う。
        if (requestedShotIndex >= 0) {
            const auto& shot = sequence_->cameraShots[requestedShotIndex];
            started = CameraEditor::GetInstance()->PlaySceneObjectCamera(CameraManager::GetInstance()->GetMainCamera(),
                                                                         requestedCamera, shot.blendInDuration,
                                                                         shot.blendOutDuration, shot.easing);
        // 旧変形トラックにはブレンド設定がないため、CameraEditorの既定値を使う。
        } else {
            started = CameraEditor::GetInstance()->PlaySceneObjectCamera(CameraManager::GetInstance()->GetMainCamera(),
                                                                         requestedCamera);
        }

        // 上書きの開始に成功した場合だけ、有効カメラとショット番号を保持する。
        if (started) {
            activeCameraObject_ = requestedCamera;
            activeCameraShotIndex_ = requestedShotIndex;
        }
    }
}

void CinematicPlayer::StopCameraTrack(bool switchingToNextShot) {
    // 有効なカメラの上書きがなければ、CameraEditorに解除を要求しない。
    if (!activeCameraObject_) {
        return;
    }

    CameraEditor::GetInstance()->StopSceneObjectCamera(CameraManager::GetInstance()->GetMainCamera(),
                                                       switchingToNextShot);
    activeCameraObject_ = nullptr;
    activeCameraShotIndex_ = -1;
}

} // namespace engine
