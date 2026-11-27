#define NOMINMAX
#include <cstdint>
#include "AnimatorController.h"

#include "engine/utility/math/AnimationInterpolation.h"
#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;

namespace {
// JSON では列挙値を整数で保存します。未知の値は既知の範囲へ丸めて読み込みます。
std::int32_t ToInt(AnimatorParameterType type) {
    return static_cast<std::int32_t>(type);
}

// 遷移条件の比較種別を保存用の整数へ変換します。
std::int32_t ToInt(AnimatorConditionMode mode) {
    return static_cast<std::int32_t>(mode);
}

// 保存値を有効範囲に丸めてAnimatorのパラメータ型へ戻します。
AnimatorParameterType ToParameterType(std::int32_t value) {
    return static_cast<AnimatorParameterType>(std::clamp(value, 0, 3));
}

// 保存値を有効範囲に丸めて遷移条件の比較種別へ戻します。
AnimatorConditionMode ToConditionMode(std::int32_t value) {
    return static_cast<AnimatorConditionMode>(std::clamp(value, 0, 6));
}

animationInterpolation::EasingType ToEasing(std::int32_t value) {
    // 補間指定を有効なenum範囲へ収めてから再生器へ渡す。
    return static_cast<animationInterpolation::EasingType>(std::clamp(value, 0, 4));
}
}

void AnimatorControllerAsset::Clear() {
    // 再読込前に状態・条件・パラメータをまとめて破棄する。
    version = kCurrentVersion;
    name = "animator_controller";
    entryState.clear();
    parameters.clear();
    states.clear();
    transitions.clear();
}

std::int32_t AnimatorControllerAsset::FindStateIndex(const std::string& stateName) const {
    // 遷移先の検索は名前を基準にし、配列順の変更に依存させない。
    for (std::int32_t index = 0; index < static_cast<std::int32_t>(states.size()); ++index) {
        // 定義順に調べ、同名の状態が見つかった位置を返す。
        if (states[index].name == stateName) {
            return index;
        }
    }

    return -1;
}

const AnimatorStateDefinition* AnimatorControllerAsset::FindState(const std::string& stateName) const {
    // 名前が一致しない場合はnullptrを返し、未定義状態の参照を防ぐ。
    const std::int32_t index = FindStateIndex(stateName);
    return index >= 0 ? &states[index] : nullptr;
}

// 状態定義を現在の形式でJSONに書き出し、ストリームの書込状態を返す。
bool AnimatorControllerAsset::Save(const std::string& filePath) const {
    json root;

    // 読み込んだ旧版の番号は引き継がず、現在の形式として書き出します。
    root["version"] = kCurrentVersion;
    root["name"] = name;
    root["entryState"] = entryState;
    root["parameters"] = json::array();
    root["states"] = json::array();
    root["transitions"] = json::array();

    // パラメーターの型と定義時の既定値を保存する。実行中の値は対象外。
    for (const auto& parameter : parameters) {
        root["parameters"].push_back({
            { "name", parameter.name },
            { "type", ToInt(parameter.type) },
            { "defaultFloat", parameter.defaultFloat },
            { "defaultInt", parameter.defaultInt },
            { "defaultBool", parameter.defaultBool }
        });
    }

    // 再生状態の設定と、状態内のイベント位置（クリップ内の0～1）を同じ要素に保存します。
    for (const auto& state : states) {
        json stateJson = {
            { "name", state.name },
            { "clipName", state.clipName },
            { "bodyClipName", state.bodyClipName },
            { "speed", state.speed },
            { "loop", state.loop },
            { "blendDuration", state.blendDuration },
            { "blendEasing", state.blendEasing },
            { "eventTimelineDuration", state.eventTimelineDuration },
            { "events", json::array() }
        };

        for (const auto& event : state.events) {
            stateJson["events"].push_back({
                { "name", event.name },
                { "payload", event.payload },
                { "normalizedTime", event.normalizedTime }
            });
        }

        root["states"].push_back(stateJson);
    }

    // 遷移元・遷移先と、再生時間やパラメーターで成立する条件を保存する。
    for (const auto& transition : transitions) {
        json item = {
            { "fromState", transition.fromState },
            { "toState", transition.toState },
            { "duration", transition.duration },
            { "easing", transition.easing },
            { "hasExitTime", transition.hasExitTime },
            { "exitTime", transition.exitTime },
            { "conditions", json::array() }
        };

        for (const auto& condition : transition.conditions) {
            item["conditions"].push_back({
                { "parameter", condition.parameter },
                { "mode", ToInt(condition.mode) },
                { "threshold", condition.threshold }
            });
        }

        root["transitions"].push_back(item);
    }

    // 保存ファイルの親を必要に応じて先に作成し、末尾では実際に書き出せたかを返します。
    const std::filesystem::path path(filePath);

    // 保存名に親ディレクトリが含まれる場合だけ、必要なディレクトリを作る。
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }

    std::ofstream file(path, std::ios::binary);

    // 保存先を開けなければJSONを出力できないため失敗を返す。
    if (!file.is_open()) {
        return false;
    }

    file << root.dump(4);
    return file.good();
}

// 保存された定義を読み込む。開けない場合は現在の定義を保持し、解析や変換の例外では空に戻す。
bool AnimatorControllerAsset::Load(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);

    // ファイルを開けない場合は既存の状態を消す前に戻る。
    if (!file.is_open()) {
        return false;
    }

    json root;
    try {
        file >> root;

        // 再読み込みでは古い配列へ追記せず、JSON の内容で定義全体を置き換えます。
        Clear();
        version = root.value("version", 1);
        name = root.value("name", std::filesystem::path(filePath).stem().string());
        entryState = root.value("entryState", "");

        // パラメーターを型と初期値から復元し、型番号を対応範囲に収める。
        for (const auto& item : root.value("parameters", json::array())) {
            AnimatorParameterDefinition parameter;
            parameter.name = item.value("name", "Parameter");
            parameter.type = ToParameterType(item.value("type", 0));
            parameter.defaultFloat = item.value("defaultFloat", 0.0f);
            parameter.defaultInt = item.value("defaultInt", 0);
            parameter.defaultBool = item.value("defaultBool", false);
            parameters.push_back(parameter);
        }

        // 状態ごとに再生設定とイベント一覧を復元する。
        for (const auto& item : root.value("states", json::array())) {
            AnimatorStateDefinition state;
            state.name = item.value("name", "State");
            state.clipName = item.value("clipName", "");
            state.bodyClipName = item.value("bodyClipName", "");
            state.speed = item.value("speed", 1.0f);
            state.loop = item.value("loop", true);
            state.blendDuration = item.value("blendDuration", 0.12f);
            state.blendEasing = item.value("blendEasing", 4);

            // 不正な時刻とイベント位置を読み込んでも、発火判定の時間軸を有効範囲に保ちます。
            state.eventTimelineDuration = (std::max)(item.value("eventTimelineDuration", 1.0f), 0.01f);

            for (const auto& eventItem : item.value("events", json::array())) {
                AnimatorEventDefinition event;
                event.name = eventItem.value("name", "FeedbackCue");
                event.payload = eventItem.value("payload", "");
                event.normalizedTime = std::clamp(eventItem.value("normalizedTime", 0.0f), 0.0f, 1.0f);
                state.events.push_back(event);
            }

            states.push_back(state);
        }

        // 遷移先と設定、成立に必要な条件を、定義されている順に復元する。
        for (const auto& item : root.value("transitions", json::array())) {
            AnimatorTransitionDefinition transition;
            transition.fromState = item.value("fromState", "");
            transition.toState = item.value("toState", "");
            transition.duration = item.value("duration", 0.12f);
            transition.easing = item.value("easing", 4);
            transition.hasExitTime = item.value("hasExitTime", false);
            transition.exitTime = item.value("exitTime", 0.9f);

            for (const auto& conditionItem : item.value("conditions", json::array())) {
                AnimatorConditionDefinition condition;
                condition.parameter = conditionItem.value("parameter", "");
                condition.mode = ToConditionMode(conditionItem.value("mode", 0));
                condition.threshold = conditionItem.value("threshold", 0.0f);
                transition.conditions.push_back(condition);
            }

            transitions.push_back(transition);
        }
    } catch (...) {
        // 途中まで読み込んだ定義を Runtime から参照させないため、失敗時は空に戻します。
        Clear();
        return false;
    }

    // 開始状態を指定していないファイルでは、最初に定義された状態から始めます。
    if (entryState.empty() && !states.empty()) {
        entryState = states.front().name;
    }

    return true;
}

void AnimatorControllerRuntime::SetController(const AnimatorControllerAsset* controller, bool playEntryState) {
    // Assetの差し替え後は前の状態を使わず、必要なら入口状態から再生する。
    controller_ = controller;
    Reset(playEntryState);
}

void AnimatorControllerRuntime::Reset(bool playEntryState) {
    // 先に現在・遷移元の添字と経過秒を無効化します。パラメータを既定値に戻してから、必要に応じて入口イベントを予約します。
    currentStateIndex_ = -1;
    previousStateIndex_ = -1;
    currentTime_ = 0.0f;
    previousTime_ = 0.0f;
    transitionElapsed_ = 0.0f;
    transitionDuration_ = 0.0f;
    transitionEasing_ = 4;
    pendingEvents_.clear();
    InitializeParameters();

    // 入口の再生が要求され、定義も割り当てられている場合だけ再生を始める。
    if (playEntryState && controller_) {
        const std::int32_t entryIndex = controller_->FindStateIndex(controller_->entryState);

        // 指定された初期状態を優先する。見つからなければ先頭を使い、状態が空なら未選択のままにする。
        if (entryIndex >= 0) {
            currentStateIndex_ = entryIndex;
        } else if (!controller_->states.empty()) {
            currentStateIndex_ = 0;
        }
        QueueEntryEvents(GetCurrentState());
    }
}

// 現在状態の時刻とイベントを進め、ブレンドを更新してから自動遷移を評価する。deltaTimeは秒。
void AnimatorControllerRuntime::Update(float deltaTime, const DurationResolver& durationResolver) {
    // 定義がない、未再生、または正の経過秒がないときは時刻を進めない。
    if (!controller_ || currentStateIndex_ < 0 || deltaTime <= 0.0f) {
        return;
    }

    const AnimatorStateDefinition* current = GetCurrentState();
    const float previousCurrentTime = currentTime_;
    const float speed = current ? std::max(0.0f, current->speed) : 0.0f;
    const float clipDuration = ResolveStateDuration(current, durationResolver);

    // クリップ長が取れない状態でも、イベントと exitTime は同じ代替時間軸で評価します。
    // 実際のアニメーション再生位置はクリップ長が分からないまま折り返しません。
    float eventDuration = 0.0f;
    if (clipDuration > 0.0f) {
        eventDuration = clipDuration;
    } else if (current) {
        eventDuration = (std::max)(current->eventTimelineDuration, 0.01f);
    }
    const float rawNewTime = previousCurrentTime + deltaTime * speed;

    // 時刻をループ位置へ折り返す前に通過イベントを調べ、1 更新で複数周した場合も拾います。
    CollectCrossedEvents(current, previousCurrentTime, rawNewTime, eventDuration);
    AdvanceStateTime(currentTime_, current, deltaTime, durationResolver);

    // ブレンド中は遷移元のクリップ時刻も進めて両方の姿勢を評価できるようにする。
    if (IsTransitioning()) {
        AdvanceStateTime(previousTime_, GetPreviousState(), deltaTime, durationResolver);
        // ブレンド時間は実時間の秒です。各クリップの再生速度は姿勢の時刻だけに掛かります。
        transitionElapsed_ += deltaTime;

        // ブレンドの終了時に遷移元を外し、次回から遷移先の姿勢だけを評価します。
        if (transitionElapsed_ >= transitionDuration_) {
            previousStateIndex_ = -1;
            previousTime_ = 0.0f;
            transitionElapsed_ = transitionDuration_;
        }
    }

    // 折り返した時刻だけを見ると、大きな deltaTime で exitTime をまたいだ遷移を取りこぼします。
    const float normalizedTime = GetNormalizedTime(current, rawNewTime, durationResolver);

    // 退出時刻と条件を満たす定義が見つかった場合に、遷移先を名前で確認する。
    if (const AnimatorTransitionDefinition* transition = FindAutomaticTransition(normalizedTime)) {
        const std::int32_t destination = controller_->FindStateIndex(transition->toState);

        if (destination >= 0 && destination != currentStateIndex_) {
            // 行き先が有効なときだけ Trigger を消費し、失敗した定義で入力を失わないようにします。
            ConsumeTriggers(*transition);
            CrossFade(transition->toState, transition->duration, transition->easing);
        }
    }
}

bool AnimatorControllerRuntime::Play(const std::string& stateName, float timeSeconds) {
    // 定義がなければ状態名を解決できないため再生を開始しない。
    if (!controller_) {
        return false;
    }

    const std::int32_t stateIndex = controller_->FindStateIndex(stateName);

    // 未定義の状態名なら、現在の再生とイベントを保持して失敗を返す。
    if (stateIndex < 0) {
        return false;
    }

    // 指定された状態へ直接移るため、以前のブレンドと発火待ちイベントを破棄します。開始位置は秒です。
    currentStateIndex_ = stateIndex;
    previousStateIndex_ = -1;
    currentTime_ = std::max(0.0f, timeSeconds);
    previousTime_ = 0.0f;
    transitionElapsed_ = 0.0f;
    transitionDuration_ = 0.0f;
    pendingEvents_.clear();

    // 時刻 0 からの再生だけ、開始位置に配置されたイベントを即時発火させます。
    if (currentTime_ <= 0.0001f)
        QueueEntryEvents(GetCurrentState());
    return true;
}

// 指定状態に切り替え、遷移元の姿勢を設定に応じて混ぜる。有効な宛先がなければfalseを返す。
bool AnimatorControllerRuntime::CrossFade(const std::string& stateName, float durationOverride, std::int32_t easingOverride) {
    // 定義が割り当てられていなければ切替先を検索しない。
    if (!controller_) {
        return false;
    }

    const std::int32_t destination = controller_->FindStateIndex(stateName);

    // 切替先の名前が解決できない場合は現在の姿勢を保持する。
    if (destination < 0) {
        return false;
    }

    // 同じ状態を通常再生中は進行時刻とイベントを初期化せず、そのまま成功として扱います。
    if (destination == currentStateIndex_ && !IsTransitioning()) {
        return true;
    }

    // 引数の負値は「未指定」です。遷移定義、遷移先状態の順で既定値を探します。
    float duration = durationOverride;
    std::int32_t easing = easingOverride;

    // 現在と遷移先の組に適用できる定義を、未指定の引数を補う候補にする。
    if (const AnimatorTransitionDefinition* transition = FindRequestedTransition(currentStateIndex_, destination)) {
        // 時間の引数が負なら定義内のブレンド秒数を使う。
        if (duration < 0.0f)
            duration = transition->duration;
        // 補間方式の引数が負なら遷移定義のイージングを使う。
        if (easing < 0)
            easing = transition->easing;
    }

    const AnimatorStateDefinition& destinationState = controller_->states[destination];
    // 遷移定義も時間を指定しない場合は、遷移先状態側の既定秒数を使う。
    if (duration < 0.0f)
        duration = destinationState.blendDuration;
    // 遷移定義も方式を指定しない場合は、遷移先状態側の既定イージングを使う。
    if (easing < 0)
        easing = destinationState.blendEasing;

    // 現在の状態番号と再生秒を遷移元へ移し、遷移先は0秒から評価できるようにします。
    previousStateIndex_ = currentStateIndex_;
    previousTime_ = currentTime_;
    currentStateIndex_ = destination;
    currentTime_ = 0.0f;
    transitionElapsed_ = 0.0f;
    transitionDuration_ = std::max(0.0f, duration);
    transitionEasing_ = std::clamp(easing, 0, 4);

    // 遷移元がない場合や補間時間が0秒の場合は旧姿勢を混ぜず、遷移先だけを使用します。
    if (previousStateIndex_ < 0 || transitionDuration_ <= 0.0f) {
        previousStateIndex_ = -1;
        previousTime_ = 0.0f;
    }

    // 遷移先の再生時刻は0なので、入口イベントを通知待ちに入れる。
    QueueEntryEvents(GetCurrentState());
    return true;
}

bool AnimatorControllerRuntime::IsPlayingState(const std::string& stateName) const {
    // 現在状態が有効な場合だけ名前を比較する。
    const AnimatorStateDefinition* state = GetCurrentState();
    return state && state->name == stateName;
}

const AnimatorStateDefinition* AnimatorControllerRuntime::GetCurrentState() const {
    // Assetが無いかインデックスが範囲外なら、状態なしとしてnullptrを返す。
    if (!controller_ || currentStateIndex_ < 0 || currentStateIndex_ >= static_cast<std::int32_t>(controller_->states.size())) {
        return nullptr;
    }

    return &controller_->states[currentStateIndex_];
}

const AnimatorStateDefinition* AnimatorControllerRuntime::GetPreviousState() const {
    // 補間元が存在する場合だけ旧状態を返す。
    if (!controller_ || previousStateIndex_ < 0 || previousStateIndex_ >= static_cast<std::int32_t>(controller_->states.size())) {
        return nullptr;
    }

    return &controller_->states[previousStateIndex_];
}

float AnimatorControllerRuntime::GetTransitionWeight() const {
    // 遷移中は設定されたイージングで重みを計算し、通常時は現状態を100%使う。
    if (!IsTransitioning()) {
        return 1.0f;
    }

    const float rate = std::clamp(transitionElapsed_ / std::max(transitionDuration_, 0.0001f), 0.0f, 1.0f);
    return animationInterpolation::ApplyEasing(rate, ToEasing(transitionEasing_));
}

void AnimatorControllerRuntime::SetCurrentTime(float timeSeconds) {
    // スクラブ後に過去のイベントを再発火させないよう、待機中イベントを捨てる。
    currentTime_ = std::max(0.0f, timeSeconds);
    pendingEvents_.clear();
}

std::vector<AnimatorEventInstance> AnimatorControllerRuntime::ConsumeEvents() {
    // イベントを呼び出し側へ移し、次フレームの二重通知を防ぐ。
    std::vector<AnimatorEventInstance> result;
    result.swap(pendingEvents_);
    return result;
}

void AnimatorControllerRuntime::SetFloat(const std::string& name, float value) {
    // Float条件の評価に使う現在値を名前で保持する。
    floatParameters_[name] = value;
}

void AnimatorControllerRuntime::SetInt(const std::string& name, std::int32_t value) {
    // Int条件の評価に使う現在値を名前で保持する。
    intParameters_[name] = value;
}

void AnimatorControllerRuntime::SetBool(const std::string& name, bool value) {
    // Bool条件の評価に使う現在値を名前で保持する。
    boolParameters_[name] = value;
}

void AnimatorControllerRuntime::SetTrigger(const std::string& name) {
    // Triggerは発火済み状態にし、遷移が使うまで保持する。
    triggerParameters_[name] = true;
}

void AnimatorControllerRuntime::ResetTrigger(const std::string& name) {
    // Triggerの消費後に再遷移しないよう発火状態を解除する。
    triggerParameters_[name] = false;
}

float AnimatorControllerRuntime::GetFloat(const std::string& name) const {
    // 未設定のFloat条件は0として評価する。
    const auto it = floatParameters_.find(name);
    return it == floatParameters_.end() ? 0.0f : it->second;
}

std::int32_t AnimatorControllerRuntime::GetInt(const std::string& name) const {
    // 未設定のInt条件は0として評価する。
    const auto it = intParameters_.find(name);
    return it == intParameters_.end() ? 0 : it->second;
}

bool AnimatorControllerRuntime::GetBool(const std::string& name) const {
    // 未設定のBool条件はfalseとして評価する。
    const auto it = boolParameters_.find(name);
    return it != boolParameters_.end() && it->second;
}

bool AnimatorControllerRuntime::IsTriggerSet(const std::string& name) const {
    // 未登録Triggerは発火していないものとして扱う。
    const auto it = triggerParameters_.find(name);
    return it != triggerParameters_.end() && it->second;
}

AnimatorControllerRuntime::Snapshot AnimatorControllerRuntime::CaptureSnapshot() const {
    // Assetの再読み込み後も状態名を使って再生位置を戻せるよう、時間と補間情報を保存する。
    Snapshot snapshot;

    // 定義がなければ再生を復元できる情報がないため、無効のまま返す。
    if (!controller_) {
        return snapshot;
    }

    // 現在状態が存在する場合だけ、復元用の名前を記録する。
    if (const auto* state = GetCurrentState())
        snapshot.currentState = state->name;
    // ブレンド元が存在する場合だけ、その状態名も記録する。
    if (const auto* state = GetPreviousState())
        snapshot.previousState = state->name;
    snapshot.currentTime = currentTime_;
    snapshot.previousTime = previousTime_;
    snapshot.transitionElapsed = transitionElapsed_;
    snapshot.transitionDuration = transitionDuration_;
    snapshot.transitionEasing = transitionEasing_;
    snapshot.valid = !snapshot.currentState.empty();
    return snapshot;
}

// 記録された状態名と再生時刻を使って、現在の定義に再生位置を復元する。
void AnimatorControllerRuntime::RestoreSnapshot(const Snapshot& snapshot) {
    // 対応する定義がないか記録が無効なら、現在の再生に触れない。
    if (!controller_ || !snapshot.valid) {
        return;
    }

    // 配列位置ではなく状態名で復元し、Asset 内の並び替えに依存しないようにします。
    currentStateIndex_ = controller_->FindStateIndex(snapshot.currentState);
    previousStateIndex_ = controller_->FindStateIndex(snapshot.previousState);
    currentTime_ = snapshot.currentTime;
    previousTime_ = snapshot.previousTime;
    transitionElapsed_ = snapshot.transitionElapsed;
    transitionDuration_ = snapshot.transitionDuration;
    transitionEasing_ = snapshot.transitionEasing;
    // 巻き戻し前に溜まった通知を捨て、復元後の時刻から発火し直せるようにします。
    pendingEvents_.clear();
}

// 旧定義の実行値を破棄し、現在の定義にある名前と既定値から評価用の値を作る。
void AnimatorControllerRuntime::InitializeParameters() {
    floatParameters_.clear();
    intParameters_.clear();
    boolParameters_.clear();
    triggerParameters_.clear();

    // 定義が外れている場合は実行値を空にした状態で終了する。
    if (!controller_) {
        return;
    }

    for (const auto& parameter : controller_->parameters) {
        switch (parameter.type) {
        // 小数条件の値をFloat用の表へ登録する。
        case AnimatorParameterType::kFloat: floatParameters_[parameter.name] = parameter.defaultFloat;
        break;

        // 整数条件の値をInt用の表へ登録する。
        case AnimatorParameterType::kInt: intParameters_[parameter.name] = parameter.defaultInt;
        break;

        // 真偽条件の値をBool用の表へ登録する。
        case AnimatorParameterType::kBool: boolParameters_[parameter.name] = parameter.defaultBool;
        break;

        // Trigger は持続値ではないため、定義の初期値に関係なく未発火から始めます。
        case AnimatorParameterType::kTrigger: triggerParameters_[parameter.name] = false;
        break;
        }
    }
}

float AnimatorControllerRuntime::ResolveStateDuration(
    const AnimatorStateDefinition* state,
    const DurationResolver& durationResolver) const {
    // クリップ長が解決できない場合は0とし、時刻の折り返しを行わない。
    if (!state) {
        return 0.0f;
    }

    const float resolved = durationResolver ? durationResolver(*state) : 0.0f;
    return resolved > 0.0f ? resolved : 0.0f;
}

void AnimatorControllerRuntime::QueueEntryEvents(const AnimatorStateDefinition* state) {
    // 状態に入った瞬間の時刻0イベントだけを待機キューへ入れる。
    if (!state) {
        return;
    }

    for (const AnimatorEventDefinition& event : state->events) {
        // 時刻0相当の位置に設定されたイベントだけを入口通知として予約する。
        if (event.normalizedTime <= 0.0001f) {
            pendingEvents_.push_back({ state->name, event.name, event.payload, event.normalizedTime });
        }
    }
}

// 前回位置から現在位置までに通過したイベントを、ループを展開した秒単位の時刻順で予約する。
void AnimatorControllerRuntime::CollectCrossedEvents(
    const AnimatorStateDefinition* state,
    float previousTime,
    float rawNewTime,
    float duration) {
    // 状態やイベントがない、時間軸が無効、または経過がない場合は通知を追加しない。
    if (!state || state->events.empty() || duration <= 0.0f || rawNewTime <= previousTime) {
        return;
    }

    // Asset 側の配列順に依存しない発火順を保証するため、参照だけを時刻順に並べます。
    std::vector<const AnimatorEventDefinition*> sortedEvents;
    sortedEvents.reserve(state->events.size());

    for (const AnimatorEventDefinition& event : state->events) {
        sortedEvents.push_back(&event);
    }

    std::stable_sort(sortedEvents.begin(), sortedEvents.end(), [](const auto* lhs, const auto* rhs) {
        return lhs->normalizedTime < rhs->normalizedTime;
    });

    // (前回時刻, 今回時刻] を対象にし、境界上のイベントを次フレームで重複発火させません。
    // rawNewTime はループ前の値なので、1 更新で複数周した場合も各周を調べられます。
    constexpr float kEventTimeEpsilon = 0.00001f;
    const std::int32_t lastLoop = state->loop ? static_cast<std::int32_t>(std::floor(rawNewTime / duration)) : 0;
    const std::int32_t firstLoop = state->loop ? (std::max)(0, static_cast<std::int32_t>(std::floor(previousTime / duration))) : 0;
    const float clampedEndTime = state->loop ? rawNewTime : (std::min)(rawNewTime, duration);

    for (std::int32_t loopIndex = firstLoop; loopIndex <= lastLoop; ++loopIndex) {
        for (const AnimatorEventDefinition* event : sortedEvents) {
            // クリップ内の割合を秒へ換算し、その周の起点を足して未折り返しの時間軸で比較します。
            const float eventTime =
                std::clamp(event->normalizedTime, 0.0f, 1.0f) * duration +
                static_cast<float>(loopIndex) * duration;
            // 前回を含まず、今回までに到達したイベントを許容誤差込みで拾う。
            if (eventTime > previousTime + kEventTimeEpsilon &&
                eventTime <= clampedEndTime + kEventTimeEpsilon) {
                pendingEvents_.push_back({ state->name, event->name, event->payload, event->normalizedTime });
            }
        }

        // 単発状態では一周目だけが対象なので、次の周を評価しない。
        if (!state->loop) {
            break;
        }
    }
}

void AnimatorControllerRuntime::AdvanceStateTime(
    float& time,
    const AnimatorStateDefinition* state,
    float deltaTime,
    const DurationResolver& durationResolver) const {
    // ループ状態はクリップ長で折り返し、単発状態は終了時刻で止める。
    if (!state) {
        return;
    }

    // speed の負値は逆再生ではなく停止として扱う仕様です。
    time += deltaTime * std::max(0.0f, state->speed);
    const float duration = ResolveStateDuration(state, durationResolver);

    // クリップ長が不明な場合は折り返し先を決められないため、時刻をそのまま進める。
    if (duration <= 0.0f) {
        return;
    }

    // ループなら時刻をクリップ内の位置へ折り返す。
    if (state->loop) {
        time = std::fmod(time, duration);
    // 単発なら終了時刻で止め、それ以降の姿勢を要求しない。
    } else {
        time = std::min(time, duration);
    }
}

float AnimatorControllerRuntime::GetNormalizedTime(
    const AnimatorStateDefinition* state,
    float time,
    const DurationResolver& durationResolver) const {
    // クリップ長が不明ならイベント用時間を代用し、正規化時刻のゼロ除算を避ける。
    if (!state) {
        return 0.0f;
    }

    const float clipDuration = ResolveStateDuration(state, durationResolver);
    const float duration = clipDuration > 0.0f
        ? clipDuration
        : (std::max)(state->eventTimelineDuration, 0.01f);
    return time / duration;
}

// 明示的な切替で使う時間・方式の定義を、状態固有から汎用への順に探す。
const AnimatorTransitionDefinition* AnimatorControllerRuntime::FindRequestedTransition(std::int32_t fromIndex, std::int32_t toIndex) const {
    // 定義がないか遷移先を解決できない場合は設定を検索しない。
    if (!controller_ || toIndex < 0 || toIndex >= static_cast<std::int32_t>(controller_->states.size())) {
        return nullptr;
    }

    const std::string from = fromIndex >= 0 ? controller_->states[fromIndex].name : "";
    const std::string& to = controller_->states[toIndex].name;

    // 明示的な切り替えでも、同じ宛先に複数定義があれば状態固有を優先します。
    for (const auto& transition : controller_->transitions) {
        // 遷移元と遷移先が両方一致する定義を優先する。
        if (transition.fromState == from && transition.toState == to) {
            return &transition;
        }
    }

    for (const auto& transition : controller_->transitions) {
        // 状態固有がなければ、同じ宛先への遷移元未指定または任意の定義を使う。
        if ((transition.fromState == "*" || transition.fromState.empty()) && transition.toState == to) {
            return &transition;
        }
    }

    return nullptr;
}

// 現在状態から自動で進める遷移を優先順に探し、成立した最初の定義を返す。
const AnimatorTransitionDefinition* AnimatorControllerRuntime::FindAutomaticTransition(float normalizedTime) const {
    // 定義がないか未再生なら、自動遷移の元になる状態がない。
    if (!controller_ || currentStateIndex_ < 0) {
        return nullptr;
    }

    const std::string& current = controller_->states[currentStateIndex_].name;

    // State 固有の遷移を Any State より優先します。同じ優先度内では定義順で選びます。
    for (std::int32_t pass = 0; pass < 2; ++pass) {
        for (const auto& transition : controller_->transitions) {
            // 自分自身への自動遷移を除外し、毎フレームの再開始を防ぎます。
            if (transition.toState.empty() || transition.toState == current) {
                continue;
            }

            const bool isSpecific = transition.fromState == current;
            const bool isAnyState = transition.fromState == "*" || transition.fromState.empty();

            // 最初は現在状態に固有の遷移、次に任意状態からの遷移だけを評価する。
            if ((pass == 0 && !isSpecific) || (pass == 1 && !isAnyState)) {
                continue;
            }

            // 退出時刻の指定があり、まだその進行率に届かない候補は待機する。
            if (transition.hasExitTime && normalizedTime < transition.exitTime) {
                continue;
            }

            // パラメーター条件もすべて揃った最初の候補を返す。
            if (AreConditionsMet(transition)) {
                return &transition;
            }
        }
    }

    return nullptr;
}

// 定義されたすべての条件が成立する場合にtrueを返す。
bool AnimatorControllerRuntime::AreConditionsMet(const AnimatorTransitionDefinition& transition) const {
    if (transition.conditions.empty()) {
        // 条件も exitTime もない定義が毎フレーム発火し続けるのを防ぎます。
        return transition.hasExitTime;
    }

    for (const auto& condition : transition.conditions) {
        switch (condition.mode) {
        // 条件はすべて満たした場合に成立します。Floatはしきい値をそのまま比較し、Intの判定だけ整数へ変換します。
        case AnimatorConditionMode::kGreater:
            // 現在値がしきい値を厳密に超えなければ遷移できない。
            if (!(GetFloat(condition.parameter) > condition.threshold))
                return false;
            break;

        case AnimatorConditionMode::kLess:
            // 現在値がしきい値を厳密に下回らなければ遷移できない。
            if (!(GetFloat(condition.parameter) < condition.threshold))
                return false;
            break;

        case AnimatorConditionMode::kEquals:
            // 整数へ変換したしきい値と一致したときだけ条件を通す。
            if (GetInt(condition.parameter) != static_cast<std::int32_t>(condition.threshold))
                return false;
            break;

        case AnimatorConditionMode::kNotEqual:
            // 整数へ変換したしきい値と異なるときだけ条件を通す。
            if (GetInt(condition.parameter) == static_cast<std::int32_t>(condition.threshold))
                return false;
            break;

        case AnimatorConditionMode::kIf:
            // 指定した真偽値がtrueになるまで遷移しない。
            if (!GetBool(condition.parameter))
                return false;
            break;

        case AnimatorConditionMode::kIfNot:
            // 指定した真偽値がfalseになるまで遷移しない。
            if (GetBool(condition.parameter))
                return false;
            break;

        case AnimatorConditionMode::kTrigger:
            // 指定Triggerがセットされるまでは遷移せず、消費は成立後に行う。
            if (!IsTriggerSet(condition.parameter))
                return false;
            break;
        }
    }

    return true;
}

void AnimatorControllerRuntime::ConsumeTriggers(const AnimatorTransitionDefinition& transition) {
    // 成立した遷移で使用したTriggerだけを解除し、他の条件は維持する。
    for (const auto& condition : transition.conditions) {
        // Trigger条件だけを未発火へ戻し、数値や真偽の条件値は次の評価へ持ち越す。
        if (condition.mode == AnimatorConditionMode::kTrigger) {
            ResetTrigger(condition.parameter);
        }
    }
}
