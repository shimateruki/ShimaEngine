#define NOMINMAX
#include <cstdint>
#include "AnimatorControllerEditor.h"

#include "BaseScene.h"
#include "DebugConsole.h"
#include "DebugEditor.h"
#include "IconsFontAwesome5.h"
#include "Object3D.h"
#include "engine/editor/animation/AnimatorEditorRegistry.h"
#include "SceneManager.h"
#include "imgui.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace {
constexpr const char* kAnimatorDirectory = "Resources/json/animator/";
constexpr const char* kBodyAnimationDirectory = "Resources/json/animation_clip/";

const char* kEasingNames[] = { "Linear", "Ease In", "Ease Out", "Ease In Out", "Smoother Step" };
const char* kParameterTypeNames[] = { "Float", "Int", "Bool", "Trigger" };
const char* kConditionModeNames[] = { "Greater", "Less", "Equals", "Not Equal", "If", "If Not", "Trigger" };
}

void AnimatorControllerEditor::Initialize(SceneManager* sceneManager, DebugEditor* editor) {
    // 新規Controllerを初期値にし、保存済みAssetの一覧を編集開始時に集めます。
    sceneManager_ = sceneManager;
    editor_ = editor;
    NewController();
    RefreshFiles();
}

void AnimatorControllerEditor::SetPreviewTarget(engine::Object3D* target) {
    // Scene再読込後に古いポインタを持たないよう、対象は名前で記録します。
    previewTargetName_ = target ? target->GetName() : std::string{};
}

void AnimatorControllerEditor::DrawImGui() {
#ifdef USE_IMGUI
    // State・Parameter・Transitionの編集を同じController定義へ反映します。
    ImGui::Text(ICON_FA_RANDOM " Animator Controller");
    ImGui::TextDisabled("State、Transition、Parameterと補間をAssetとして編集します。");
    DrawAssetControls();
    DrawPreviewTargetControls();

    // タブバーが表示されたフレームだけ各タブを評価する。
    if (ImGui::BeginTabBar("AnimatorControllerTabs")) {
        // Statesタブでは状態ごとのクリップやイベントを編集する。
        if (ImGui::BeginTabItem("States")) {
            DrawStateEditor();
            ImGui::EndTabItem();
        }

        // Transitionsタブでは遷移先と発動条件を編集する。
        if (ImGui::BeginTabItem("Transitions")) {
            DrawTransitionEditor();
            ImGui::EndTabItem();
        }

        // Parametersタブでは初期値とプレビュー中の値を編集する。
        if (ImGui::BeginTabItem("Parameters")) {
            DrawParameterEditor();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
#endif
}

void AnimatorControllerEditor::RefreshFiles() {
    // Controllerと体アニメーションのJSONを別一覧にし、誤ったAssetを選ばせません。
    files_.clear();
    bodyAnimationFiles_.clear();

    // Controllerの保存先がまだなければ候補は空のままにする。
    if (fs::exists(kAnimatorDirectory)) {
        for (const auto& entry : fs::directory_iterator(kAnimatorDirectory)) {
            // 通常のJSONファイルだけを対象にし、拡張子を外して名前を表示する。
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                files_.push_back(entry.path().stem().string());
            }
        }
    }

    // Body Animationの保存先がまだなければ候補は空のままにする。
    if (fs::exists(kBodyAnimationDirectory)) {
        for (const auto& entry : fs::directory_iterator(kBodyAnimationDirectory)) {
            // 通常のJSONファイルだけをBody Animationの候補に加える。
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                bodyAnimationFiles_.push_back(entry.path().stem().string());
            }
        }
    }

    std::sort(files_.begin(), files_.end());
    std::sort(bodyAnimationFiles_.begin(), bodyAnimationFiles_.end());
}

void AnimatorControllerEditor::NewController() {
    // 遷移先がない新規Assetでも再生開始できるよう、Idleを入口Stateにします。
    controller_.Clear();
    AnimatorStateDefinition idle;
    idle.name = "Idle";
    controller_.states.push_back(idle);
    controller_.entryState = idle.name;
    selectedStateIndex_ = 0;
    selectedParameterIndex_ = -1;
    selectedTransitionIndex_ = -1;
}

void AnimatorControllerEditor::LoadController(const std::string& fileName) {
    // 読込失敗を通知して処理を止め、成功したAssetだけ選択状態へ反映します。
    if (fileName.empty()) {
        return;
    }

    const std::string path = std::string(kAnimatorDirectory) + fileName + ".json";

    // 読込に失敗したときはログを出し、読込成功時の選択リセットを行わずに戻る。
    if (!controller_.Load(path)) {
        DebugConsole::GetInstance()->AddLog("Animator Controller load failed: " + path);
        return;
    }

    strncpy_s(fileNameBuffer_, sizeof(fileNameBuffer_), fileName.c_str(), _TRUNCATE);
    selectedStateIndex_ = controller_.states.empty() ? -1 : 0;
    selectedParameterIndex_ = -1;
    selectedTransitionIndex_ = -1;
}

void AnimatorControllerEditor::SaveController() {
    // 保存成功後だけPreview対象へ再読込し、編集中の値とSceneの実行状態を揃える。
    if (fileNameBuffer_[0] == '\0') {
        return;
    }

    controller_.name = fileNameBuffer_;

    // 未指定の入口を補ってから保存し、利用側がどの状態から開始するか決められるようにする。
    if (controller_.entryState.empty() && !controller_.states.empty()) {
        controller_.entryState = controller_.states.front().name;
    }

    const std::string path = GetAssetPath();

    // 保存に成功した場合だけ候補一覧と既存オブジェクトの参照を更新する。
    if (controller_.Save(path)) {
        DebugConsole::GetInstance()->AddLog("Animator Controller saved: " + path);
        RefreshFiles();

        // プレビュー対象が存在するときは、同じ定義を使っているか確認する。
        if (engine::Object3D* target = ResolvePreviewTarget()) {
            // 短い登録名または保存先まで含むパスが一致する場合、その定義を付け直す。
            if (target->GetAnimatorControllerPath() == fileNameBuffer_ || target->GetAnimatorControllerPath() == path) {
                target->SetAnimatorController(fileNameBuffer_);
            }
        }

        if (sceneManager_ && sceneManager_->GetCurrentScene()) {
            // 保存成功を現在シーンの対応Objectへ通知する。具体型と再読込方法は登録側が決める。
            for (const auto& object : sceneManager_->GetCurrentScene()->GetObjects()) {
                // 空のオブジェクト枠には再読込を要求しない。
                if (!object) continue;
                const auto* binding = engine::AnimatorEditorRegistry::Find(fileNameBuffer_, *object);
                // 再読込処理を登録しているオブジェクトには、その型に対応する処理を任せる。
                if (binding && binding->reload) binding->reload(*object);
            }
        }
    }
}

std::string AnimatorControllerEditor::GetAssetPath() const {
    // 保存名をController用ディレクトリのJSONパスへ組み立てます。
    return std::string(kAnimatorDirectory) + fileNameBuffer_ + ".json";
}

engine::Object3D* AnimatorControllerEditor::ResolvePreviewTarget() const {
    // 対象名を毎回現行Sceneから解決し、Scene切り替え後の不正参照を避けます。
    if (!sceneManager_ || !sceneManager_->GetCurrentScene() || previewTargetName_.empty()) {
        return nullptr;
    }

    for (const auto& object : sceneManager_->GetCurrentScene()->GetObjects()) {
        // 現在のシーンから、選択名と一致する最初のオブジェクトを返す。
        if (object && object->GetName() == previewTargetName_) {
            return object.get();
        }
    }

    return nullptr;
}

void AnimatorControllerEditor::DrawAssetControls() {
#ifdef USE_IMGUI
    // 新規・読込・保存を一箇所へ集め、選択中のJSON名を明示します。
    if (!ImGui::CollapsingHeader(ICON_FA_SAVE " Controller Asset", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    const char* current = fileNameBuffer_[0] ? fileNameBuffer_ : "(新規)";

    // 既存Controllerの選択欄が開いている間だけ候補を並べる。
    if (ImGui::BeginCombo("既存Controller", current)) {
        for (const auto& fileName : files_) {
            // 選択した保存済みのControllerを読み込む。
            if (ImGui::Selectable(fileName.c_str(), fileName == fileNameBuffer_)) {
                LoadController(fileName);
            }
        }

        ImGui::EndCombo();
    }

    ImGui::InputText("ファイル名", fileNameBuffer_, sizeof(fileNameBuffer_));

    // 新規ボタンではIdleを入口とする編集用データを作る。
    if (ImGui::Button(ICON_FA_FILE " 新規")) {
        NewController();
        strncpy_s(fileNameBuffer_, sizeof(fileNameBuffer_), "new_animator", _TRUNCATE);
    }

    ImGui::SameLine();

    // 保存ボタンの操作をSaveController()に渡す。
    if (ImGui::Button(ICON_FA_SAVE " 保存")) {
        SaveController();
    }

    ImGui::SameLine();

    // 再読込ボタンでは現在のファイル名を指定して読み直す。
    if (ImGui::Button(ICON_FA_SYNC " 再読込")) {
        LoadController(fileNameBuffer_);
    }

    ImGui::SameLine();

    // 外部から追加された素材を選べるように両方の候補一覧を取得し直す。
    if (ImGui::Button("Clip一覧更新")) {
        RefreshFiles();
    }

    ImGui::TextDisabled("保存先: %s", GetAssetPath().c_str());
#endif
}

void AnimatorControllerEditor::DrawPreviewTargetControls() {
#ifdef USE_IMGUI
    // Scene内の対象を選び、Controllerの試用と割当を同じ対象へ向けます。
    if (!ImGui::CollapsingHeader(ICON_FA_CUBE " Preview / Assign", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    const char* label = previewTargetName_.empty() ? "(未選択)" : previewTargetName_.c_str();

    // 選択欄が開いたときに現在のシーンからプレビュー候補を作る。
    if (ImGui::BeginCombo("Scene Object", label)) {
        // シーン管理と現在のシーンが揃った場合だけ一覧を参照する。
        if (sceneManager_ && sceneManager_->GetCurrentScene()) {
            for (const auto& object : sceneManager_->GetCurrentScene()->GetObjects()) {
                // 有効な通常オブジェクトを選んだときに、対象を名前で覚える。
                if (object && !object->IsEditorInternal() && ImGui::Selectable(object->GetName().c_str(), object->GetName() == previewTargetName_)) {
                    previewTargetName_ = object->GetName();
                }
            }
        }

        ImGui::EndCombo();
    }

    engine::Object3D* target = ResolvePreviewTarget();

    if (target) {
        // 専用の再生規則を持つObjectだけ登録されたUIへ委譲し、未登録なら従来のModel操作を使う。
        const auto* binding = engine::AnimatorEditorRegistry::Find(fileNameBuffer_, *target);
        // 専用のプレビュー描画が登録された型はコールバックに任せて戻る。
        if (binding && binding->drawPreview) {
            const AnimatorStateDefinition* selected = selectedStateIndex_ >= 0 && selectedStateIndex_ < static_cast<std::int32_t>(controller_.states.size())
                ? &controller_.states[selectedStateIndex_] : nullptr;
            binding->drawPreview(*target, selected, [this] { SaveController(); });
            return;
        }

        ImGui::Text("割り当て中: %s", target->GetAnimatorControllerPath().empty() ? "(なし)" : target->GetAnimatorControllerPath().c_str());

        // 通常ObjectはAssetを保存してから割当・State再生する。
        if (ImGui::Button("このControllerをObjectへ割り当て", ImVec2(-1.0f, 0.0f))) {
            SaveController();
            target->SetAnimatorController(fileNameBuffer_);
        }

        // 選択中のStateが存在する場合だけ再生ボタンを出す。
        if (selectedStateIndex_ >= 0 && selectedStateIndex_ < static_cast<std::int32_t>(controller_.states.size())) {
            // 再生ボタンが押されたときに選択Stateへの切り替えを要求する。
            if (ImGui::Button(ICON_FA_PLAY " 選択Stateを再生", ImVec2(-1.0f, 0.0f))) {
                // 対象にControllerがなければ、選択Stateを再生する前に割り当てる。
                if (!target->HasAnimatorController()) {
                    target->SetAnimatorController(fileNameBuffer_);
                }

                target->PlayAnimatorState(controller_.states[selectedStateIndex_].name);
            }
        }
    // プレビュー対象を解決できないときは対象の選び方を示す。
    } else {
        ImGui::TextDisabled("Preview対象を選ぶと、State再生とObjectへの割り当てを確認できます。");
    }
#endif
}

void AnimatorControllerEditor::DrawStateEditor() {
    // 状態の名前変更や削除は参照元の遷移にも影響するため、専用操作に集約します。
#ifdef USE_IMGUI
    // 追加ボタンでは現在の件数を使った名前でStateを作り、その項目を選択する。
    if (ImGui::Button(ICON_FA_PLUS " State追加")) {
        AnimatorStateDefinition state;
        state.name = "State" + std::to_string(controller_.states.size() + 1);
        controller_.states.push_back(state);
        selectedStateIndex_ = static_cast<std::int32_t>(controller_.states.size()) - 1;
        // 入口が未設定なら新しいStateを最初に再生する設定にする。
        if (controller_.entryState.empty())
            controller_.entryState = state.name;
    }

    ImGui::BeginChild("AnimatorStateList", ImVec2(190.0f, 330.0f), true);

    for (std::int32_t index = 0; index < static_cast<std::int32_t>(controller_.states.size()); ++index) {
        std::string label = controller_.states[index].name;
        // 入口に登録されたStateには、一覧で識別できる印を付ける。
        if (controller_.entryState == controller_.states[index].name)
            label = "[Entry] " + label;
        // クリックした項目を下段の編集対象にする。
        if (ImGui::Selectable(label.c_str(), selectedStateIndex_ == index))
            selectedStateIndex_ = index;
    }

    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();

    // 選択の添字が有効な間だけStateの編集欄を表示する。
    if (selectedStateIndex_ >= 0 && selectedStateIndex_ < static_cast<std::int32_t>(controller_.states.size())) {
        auto& state = controller_.states[selectedStateIndex_];
        char stateName[128]{};
        strncpy_s(stateName, sizeof(stateName), state.name.c_str(), _TRUNCATE);
        const std::string oldName = state.name;

        // 名前を編集したら入口と遷移からの参照もRenameState()で合わせる。
        if (ImGui::InputText("State名", stateName, sizeof(stateName))) {
            RenameState(oldName, stateName);
        }

        engine::Object3D* target = ResolvePreviewTarget();
        const char* clipLabel = state.clipName.empty() ? "(なし)" : state.clipName.c_str();

        // 骨格クリップは現在のモデルに含まれるもの、身体変形は別の JSON アセットから選ぶ。
        if (ImGui::BeginCombo("Model Animation Clip", clipLabel)) {
            // 「なし」を選んだStateからモデルのクリップ指定を外す。
            if (ImGui::Selectable("(なし)", state.clipName.empty()))
                state.clipName.clear();
            // プレビュー対象がモデルを持つ場合は、そのモデルから候補を取得する。
            if (target && target->GetModel()) {
                for (const auto& animation : target->GetModel()->GetModelData().animations) {
                    // モデルのクリップを選んだときに、名前をStateへ記録する。
                    if (ImGui::Selectable(animation.name.c_str(), animation.name == state.clipName))
                        state.clipName = animation.name;
                }
            }

            ImGui::EndCombo();
        }

        const char* bodyClipLabel = state.bodyClipName.empty() ? "(Procedural / Code)" : state.bodyClipName.c_str();

        // Body Animationの選択欄が開いたらコード制御と保存済みクリップを提示する。
        if (ImGui::BeginCombo("Body Animation Clip", bodyClipLabel)) {
            // コード制御を選んだときはBody Animationのファイル指定を空にする。
            if (ImGui::Selectable("(Procedural / Code)", state.bodyClipName.empty()))
                state.bodyClipName.clear();
            for (const std::string& fileName : bodyAnimationFiles_) {
                // 保存済みの候補を選んだときは、Stateにファイル名を記録する。
                if (ImGui::Selectable(fileName.c_str(), fileName == state.bodyClipName)) {
                    state.bodyClipName = fileName;
                }
            }

            ImGui::EndCombo();
        }

        ImGui::TextDisabled("Body Clip未設定時は従来のコードAnimationを使用します。");

        // 素材固有の説明だけを登録元へ依頼し、共通の速度や補間入力はここで描画する。
        if (const auto* binding = engine::AnimatorEditorRegistry::Find(fileNameBuffer_);
            binding && binding->drawStateHelp) {
            binding->drawStateHelp(state);
        }

        ImGui::DragFloat("Speed", &state.speed, 0.01f, 0.0f, 8.0f, "%.2fx");
        ImGui::Checkbox("Loop", &state.loop);
        ImGui::DragFloat("Default Blend", &state.blendDuration, 0.01f, 0.0f, 5.0f, "%.3f sec");
        ImGui::Combo("Default Easing", &state.blendEasing, kEasingNames, IM_ARRAYSIZE(kEasingNames));
        ImGui::DragFloat("Event Timeline Duration", &state.eventTimelineDuration, 0.01f, 0.01f, 120.0f, "%.2f sec");
        ImGui::TextDisabled("Model/Body Clipに長さがある場合はそちらを優先します。");

        ImGui::SeparatorText("Animation Events");

        // Eventは正規化時刻でClipへ結び、名前とPayloadを実行時Cueへ渡す。
        if (ImGui::Button(ICON_FA_PLUS " Event追加")) {
            state.events.push_back({});
        }

        std::int32_t removeEventIndex = -1;

        for (std::int32_t eventIndex = 0; eventIndex < static_cast<std::int32_t>(state.events.size()); ++eventIndex) {
            AnimatorEventDefinition& event = state.events[eventIndex];
            ImGui::PushID(eventIndex);
            char eventName[128]{};
            char eventPayload[256]{};
            strncpy_s(eventName, sizeof(eventName), event.name.c_str(), _TRUNCATE);
            strncpy_s(eventPayload, sizeof(eventPayload), event.payload.c_str(), _TRUNCATE);
            ImGui::Text("Event %02d", eventIndex + 1);
            ImGui::SameLine();

            // 削除操作は添字を記録し、一覧を描き終えてから対象を消す。
            if (ImGui::SmallButton(ICON_FA_TRASH)) {
                removeEventIndex = eventIndex;
            }

            // 名前欄が編集されたときだけイベントの名前に反映する。
            if (ImGui::InputText("名前", eventName, sizeof(eventName))) {
                event.name = eventName;
            }

            // Payload欄が編集されたときだけイベントの補足データに反映する。
            if (ImGui::InputText("Payload", eventPayload, sizeof(eventPayload))) {
                event.payload = eventPayload;
            }

            ImGui::SliderFloat("正規化時刻", &event.normalizedTime, 0.0f, 1.0f, "%.3f");
            ImGui::TextDisabled("FeedbackCue + VFX Cue名で、VFX/SE/カメラ/振動を自動再生します。");
            ImGui::Separator();
            ImGui::PopID();
        }

        // イベントの入力欄をすべて描画してから、削除を予約した一件だけ取り除く。
        if (removeEventIndex >= 0) {
            state.events.erase(state.events.begin() + removeEventIndex);
        }

        // 設定ボタンで、選択中のStateを初期再生先にする。
        if (ImGui::Button("Entry Stateに設定", ImVec2(-1.0f, 0.0f)))
            controller_.entryState = state.name;
        ImGui::BeginDisabled(controller_.states.size() <= 1);

        if (ImGui::Button(ICON_FA_TRASH " State削除", ImVec2(-1.0f, 0.0f))) {
            // 削除したStateを参照する遷移も同時に取り除き、Entryが消えたら先頭へ戻す。
            const std::string removedName = state.name;
            controller_.states.erase(controller_.states.begin() + selectedStateIndex_);
            controller_.transitions.erase(
                std::remove_if(controller_.transitions.begin(), controller_.transitions.end(),
                    [&removedName](const AnimatorTransitionDefinition& transition) {
                        return transition.fromState == removedName || transition.toState == removedName;
                    }),
                controller_.transitions.end());
            // 入口を削除した場合は残った先頭のStateを入口にする。
            if (controller_.entryState == removedName)
                controller_.entryState = controller_.states.front().name;
            selectedStateIndex_ = std::min(selectedStateIndex_, static_cast<std::int32_t>(controller_.states.size()) - 1);
        }

        ImGui::EndDisabled();
    }

    ImGui::EndGroup();
#endif
}

void AnimatorControllerEditor::DrawParameterEditor() {
#ifdef USE_IMGUI
    // 遷移条件に参照されるParameter名と型をController内で編集します。
    if (ImGui::Button(ICON_FA_PLUS " Parameter追加")) {
        AnimatorParameterDefinition parameter;
        parameter.name = "Parameter" + std::to_string(controller_.parameters.size() + 1);
        controller_.parameters.push_back(parameter);
        selectedParameterIndex_ = static_cast<std::int32_t>(controller_.parameters.size()) - 1;
    }

    for (std::int32_t index = 0; index < static_cast<std::int32_t>(controller_.parameters.size()); ++index) {
        ImGui::PushID(index);
        // クリックしたパラメーターを初期値と実行値の編集対象にする。
        if (ImGui::Selectable(controller_.parameters[index].name.c_str(), selectedParameterIndex_ == index))
            selectedParameterIndex_ = index;
        ImGui::PopID();
    }

    // 選択がなければ設定欄から参照できる項目がないため戻る。
    if (selectedParameterIndex_ < 0 || selectedParameterIndex_ >= static_cast<std::int32_t>(controller_.parameters.size()))
        return;
    auto& parameter = controller_.parameters[selectedParameterIndex_];
    char name[128]{};
    strncpy_s(name, sizeof(name), parameter.name.c_str(), _TRUNCATE);
    // 入力変更は定義の名前に反映する。遷移条件に記録済みの名前は個別に編集する。
    if (ImGui::InputText("Parameter名", name, sizeof(name)))
        parameter.name = name;
    std::int32_t type = static_cast<std::int32_t>(parameter.type);

    // 型を選び直した場合は、以下の初期値編集と実行時操作を新しい型に合わせる。
    if (ImGui::Combo("Type", &type, kParameterTypeNames, IM_ARRAYSIZE(kParameterTypeNames))) {
        parameter.type = static_cast<AnimatorParameterType>(type);
    }

    // Float型は小数の初期値を編集する。
    if (parameter.type == AnimatorParameterType::kFloat)
        ImGui::DragFloat("Default", &parameter.defaultFloat, 0.01f);
    // Int型は整数の初期値を編集する。
    if (parameter.type == AnimatorParameterType::kInt)
        ImGui::DragInt("Default", &parameter.defaultInt);
    // Bool型は真偽の初期値を編集する。
    if (parameter.type == AnimatorParameterType::kBool)
        ImGui::Checkbox("Default", &parameter.defaultBool);
    if (engine::Object3D* target = ResolvePreviewTarget()) {
        // DefaultはAssetの値、Runtime Valueは現在再生中のObjectへの一時入力として分ける。
        const auto* binding = engine::AnimatorEditorRegistry::Find(fileNameBuffer_, *target);
        AnimatorControllerRuntime* runtime = binding && binding->getRuntime
            ? binding->getRuntime(*target) : target->GetAnimatorControllerRuntime();
        if (runtime) {
            // 特殊な読み取り専用表示などを登録側が処理し、それ以外は共通の値編集を使う。
            if (!binding || !binding->drawRuntimeParameter || !binding->drawRuntimeParameter(*runtime, parameter)) {
                // Float型は実行中の小数値を取得する。
                if (parameter.type == AnimatorParameterType::kFloat) {
                    float value = runtime->GetFloat(parameter.name);
                    // ドラッグで変化した値をプレビューのAnimatorに反映する。
                    if (ImGui::DragFloat("Runtime Value", &value, 0.01f))
                        runtime->SetFloat(parameter.name, value);
                // Int型は実行中の整数値を取得する。
                } else if (parameter.type == AnimatorParameterType::kInt) {
                    std::int32_t value = runtime->GetInt(parameter.name);
                    // 整数の入力が変わったときだけ実行値を書き戻す。
                    if (ImGui::DragInt("Runtime Value", &value))
                        runtime->SetInt(parameter.name, value);
                // Bool型は実行中の真偽値を取得する。
                } else if (parameter.type == AnimatorParameterType::kBool) {
                    bool value = runtime->GetBool(parameter.name);
                    // チェック状態を変更したときだけ実行値を書き戻す。
                    if (ImGui::Checkbox("Runtime Value", &value))
                        runtime->SetBool(parameter.name, value);
                // Trigger型はボタン操作のフレームに発火を要求する。
                } else if (ImGui::Button("Trigger発火")) {
                    runtime->SetTrigger(parameter.name);
                }
            }
        }
    }

    // 削除操作では定義と選択を外す。遷移条件に記録済みの参照はここでは変更しない。
    if (ImGui::Button(ICON_FA_TRASH " Parameter削除")) {
        controller_.parameters.erase(controller_.parameters.begin() + selectedParameterIndex_);
        selectedParameterIndex_ = -1;
    }
#endif
}

void AnimatorControllerEditor::DrawTransitionEditor() {
#ifdef USE_IMGUI
    // 遷移元・遷移先・条件を編集し、選択Stateからの追加を初期値にします。
    if (ImGui::Button(ICON_FA_PLUS " Transition追加")) {
        AnimatorTransitionDefinition transition;
        transition.fromState = selectedStateIndex_ >= 0 && selectedStateIndex_ < static_cast<std::int32_t>(controller_.states.size())
            ? controller_.states[selectedStateIndex_].name : "*";
        for (const auto& state : controller_.states) {
            // 遷移元と名前の異なる最初のStateを遷移先候補に採用する。
            if (state.name != transition.fromState) {
                transition.toState = state.name;
                break;
            }
        }

        controller_.transitions.push_back(transition);
        selectedTransitionIndex_ = static_cast<std::int32_t>(controller_.transitions.size()) - 1;
    }

    for (std::int32_t index = 0; index < static_cast<std::int32_t>(controller_.transitions.size()); ++index) {
        const auto& transition = controller_.transitions[index];
        const std::string label = (transition.fromState.empty() || transition.fromState == "*" ? "Any State" : transition.fromState)
            + " -> " + transition.toState;
        ImGui::PushID(index);
        // クリックした行を下段の遷移編集対象にする。
        if (ImGui::Selectable(label.c_str(), selectedTransitionIndex_ == index))
            selectedTransitionIndex_ = index;
        ImGui::PopID();
    }

    // 遷移の選択が無効なら設定を参照せず戻る。
    if (selectedTransitionIndex_ < 0 || selectedTransitionIndex_ >= static_cast<std::int32_t>(controller_.transitions.size()))
        return;
    auto& transition = controller_.transitions[selectedTransitionIndex_];
    // 遷移元だけは全Stateを表すワイルドカードを選べる。行き先は実在するStateの候補から選ぶ。
    const char* fromLabel = transition.fromState.empty() || transition.fromState == "*" ? "Any State" : transition.fromState.c_str();

    // From欄が開いたら任意のStateと名前指定の両方を提示する。
    if (ImGui::BeginCombo("From", fromLabel)) {
        // Any Stateを選んだ遷移は、遷移元を任意とする記号で保存する。
        if (ImGui::Selectable("Any State", transition.fromState.empty() || transition.fromState == "*"))
            transition.fromState = "*";
        for (const auto& state : controller_.states) {
            // 名前を選んだ遷移はそのStateを遷移元とする。
            if (ImGui::Selectable(state.name.c_str(), transition.fromState == state.name))
                transition.fromState = state.name;
        }

        ImGui::EndCombo();
    }

    const char* toLabel = transition.toState.empty() ? "(未設定)" : transition.toState.c_str();

    // To欄が開いたら遷移先として使えるStateを並べる。
    if (ImGui::BeginCombo("To", toLabel)) {
        for (const auto& state : controller_.states) {
            // 選択したState名を遷移先として記録する。
            if (ImGui::Selectable(state.name.c_str(), transition.toState == state.name))
                transition.toState = state.name;
        }

        ImGui::EndCombo();
    }

    ImGui::DragFloat("Duration", &transition.duration, 0.01f, 0.0f, 5.0f, "%.3f sec");
    ImGui::Combo("Easing", &transition.easing, kEasingNames, IM_ARRAYSIZE(kEasingNames));
    ImGui::Checkbox("Has Exit Time", &transition.hasExitTime);
    // 遷移の補間は秒で指定し、遷移可能になる時点はクリップ内の割合で指定する。
    if (transition.hasExitTime)
        ImGui::SliderFloat("Exit Time", &transition.exitTime, 0.0f, 1.0f, "%.2f normalized");

    ImGui::SeparatorText("Conditions");

    // 条件追加ボタンでは、新しい条件を選択中の遷移へ追加する。
    if (ImGui::Button("Condition追加"))
        transition.conditions.push_back({});
    for (std::int32_t index = 0; index < static_cast<std::int32_t>(transition.conditions.size()); ++index) {
        auto& condition = transition.conditions[index];
        ImGui::PushID(1000 + index);
        const char* parameterLabel = condition.parameter.empty() ? "(未設定)" : condition.parameter.c_str();

        // Parameter欄が開いたときはControllerに登録済みの名前を並べる。
        if (ImGui::BeginCombo("Parameter", parameterLabel)) {
            for (const auto& parameter : controller_.parameters) {
                // 選択した名前を条件が参照するパラメーターとして記録する。
                if (ImGui::Selectable(parameter.name.c_str(), condition.parameter == parameter.name))
                    condition.parameter = parameter.name;
            }

            ImGui::EndCombo();
        }

        std::int32_t mode = static_cast<std::int32_t>(condition.mode);
        if (ImGui::Combo("Mode", &mode, kConditionModeNames, IM_ARRAYSIZE(kConditionModeNames)))
            condition.mode = static_cast<AnimatorConditionMode>(mode);
        // 数値を比較する条件だけしきい値欄を出す。BoolやTriggerの条件では使わない。
        if (condition.mode == AnimatorConditionMode::kGreater || condition.mode == AnimatorConditionMode::kLess ||
            condition.mode == AnimatorConditionMode::kEquals || condition.mode == AnimatorConditionMode::kNotEqual) {
            ImGui::DragFloat("Threshold", &condition.threshold, 0.01f);
        }

        // 削除した条件への参照を以後使わず、詰まった次の要素から処理を続ける。
        if (ImGui::Button("Condition削除")) {
            transition.conditions.erase(transition.conditions.begin() + index);
            ImGui::PopID();
            --index;
            continue;
        }

        ImGui::Separator();
        ImGui::PopID();
    }

    // 遷移削除ボタンで選択中の遷移を消し、選択を解除する。
    if (ImGui::Button(ICON_FA_TRASH " Transition削除")) {
        controller_.transitions.erase(controller_.transitions.begin() + selectedTransitionIndex_);
        selectedTransitionIndex_ = -1;
    }
#endif
}

void AnimatorControllerEditor::RenameState(const std::string& oldName, const std::string& newName) {
    // 遷移と現在の開始状態が古い名前を参照し続けないよう、関連箇所を更新します。
    if (selectedStateIndex_ < 0 || selectedStateIndex_ >= static_cast<std::int32_t>(controller_.states.size()) || newName.empty()) {
        return;
    }

    controller_.states[selectedStateIndex_].name = newName;
    // 入口の名前が変更前と一致する場合は、新しい名前に合わせる。
    if (controller_.entryState == oldName)
        controller_.entryState = newName;
    for (auto& transition : controller_.transitions) {
        // 遷移元が変更前のStateなら参照を新しい名前に合わせる。
        if (transition.fromState == oldName)
            transition.fromState = newName;
        // 遷移先が変更前のStateなら参照を新しい名前に合わせる。
        if (transition.toState == oldName)
            transition.toState = newName;
    }
}
