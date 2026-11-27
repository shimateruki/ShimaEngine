#pragma once

// ゲーム固有の登録入口：RegisterGameAnimatorEditors。登録先の共通Editorに、個別の型名や分岐を持ち込まない。
#ifdef USE_IMGUI

#include "engine/editor/animation/AnimatorEditorRegistry.h"
#include "Player.h"
#include "DebugConsole.h"
#include "imgui.h"

// Player固有の再生規則とUIはゲーム側で定義し、汎用Editorには具体型を教えない。
inline void RegisterGameAnimatorEditors() {
    engine::AnimatorEditorRegistry::Entry entry;
    entry.assetName = "player_slime";
    entry.accepts = [](engine::Object3D& object) {
        return dynamic_cast<Player*>(&object) != nullptr;
    };
    entry.reload = [](engine::Object3D& object) {
        static_cast<Player&>(object).ReloadSlimeAnimatorController();
    };

    // acceptsを通った現在のObjectだけが渡される。選択先の生ポインタは保存しない。
    entry.drawPreview = [](engine::Object3D& object, const AnimatorStateDefinition* selectedState,
                           const engine::AnimatorEditorRegistry::SaveAction& save) {
        auto* player = &static_cast<Player&>(object);
        AnimatorControllerRuntime* runtime = player->GetSlimeAnimatorControllerRuntime();
        const AnimatorStateDefinition* state = runtime ? runtime->GetCurrentState() : nullptr;
        ImGui::Text("Playerの現在State: %s", state ? state->name.c_str() : "(未読込)");

        // 現在のAnimatorを取得できた場合だけ、行動モードと遷移の割合を表示する。
        if (runtime) {
            ImGui::Text("LocomotionMode: %d / 切替重み: %.2f",
                runtime->GetInt("LocomotionMode"), runtime->GetTransitionWeight());
        }

        // 編集中の状態が能力用の本体クリップを持つ場合だけ、保存してから見た目を試せる入口を出します。
        // 状態を選択している場合に限り、その状態をプレビューできるか確認する。
        if (selectedState) {
            const AnimatorStateDefinition& selected = *selectedState;

            // 能力用のState名で身体クリップが指定されている場合だけ、専用の再生ボタンを表示する。
            if (selected.name.rfind("player_", 0) == 0 && !selected.bodyClipName.empty()) {
                // ボタンを押したときだけ、編集中の設定を保存して見た目の再生を要求する。
                if (ImGui::Button("保存して選択した能力モーションを確認", ImVec2(-1.0f, 0.0f))) {
                    save();
                    // 能力モーションを開始できなければ、調査できるよう状態名をログに残す。
                    if (!player->PlaySlimeAbilityMotion(selected.name, selected.loop, 1.0f,
                        selected.blendDuration, selected.blendDuration)) {
                        DebugConsole::GetInstance()->AddLog("Animator ability preview failed: " + selected.name);
                    }
                }

                ImGui::TextDisabled("見た目のみを再生します。攻撃判定とダメージは発生しません。");
            }
        }

        ImGui::TextWrapped("保存するとシーン内のPlayerへ再読込します。通常動作はゲーム側の状態から決まり、Model用のState再生ボタンは使いません。");
    };

    entry.drawStateHelp = [](const AnimatorStateDefinition& state) {
        // 能力用のState名にだけ、実行時の再生キーや攻撃側が持つ設定を説明する。
        if (state.name.rfind("player_", 0) == 0) {
            ImGui::TextWrapped("能力モーション: State名がC++の再生キーです。Body ClipとSpeedは本編へ反映します。LoopとBlendは攻撃処理側が指定し、Eventはこの別レイヤーでは未接続です。");
        }
    };
    entry.getRuntime = [](engine::Object3D& object) {
        return static_cast<Player&>(object).GetSlimeAnimatorControllerRuntime();
    };
    entry.drawRuntimeParameter = [](AnimatorControllerRuntime& runtime, const AnimatorParameterDefinition& parameter) {
        // 行動Stateが毎フレーム書く値だけ読み取り専用。他の値編集は共通Editorに任せる。
        if (parameter.name != "LocomotionMode") return false;
        ImGui::TextDisabled("現在値: %d （ゲームの行動状態から毎フレーム設定）", runtime.GetInt(parameter.name));
        return true;
    };
    engine::AnimatorEditorRegistry::Register(std::move(entry));
}
#endif
