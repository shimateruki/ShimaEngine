#include <cstdint>
#include "PlayModeChangeTracker.h"

#ifdef USE_IMGUI

#include "BaseScene.h"
#include "DebugEditor.h"
#include "Object3D.h"
#include "SceneManager.h"
#include "imgui.h"

#include <algorithm>
#include <array>

// Play停止後の状態復元と編集履歴に使う、シーン管理とエディターを借用します。
void PlayModeChangeTracker::Initialize(SceneManager* sceneManager, DebugEditor* editor) {
    sceneManager_ = sceneManager;
    editor_ = editor;
}

void PlayModeChangeTracker::Finalize() {
    // 保持している候補を破棄してから、外部の管理オブジェクトへの参照を外す。
    Clear();
    sceneManager_ = nullptr;
    editor_ = nullptr;
}

void PlayModeChangeTracker::Clear() {
    baselineByGuid_.clear();
    candidates_.clear();
    open_ = false;
}

bool PlayModeChangeTracker::IsEligible(const engine::Object3D* object) const {
    // エディター内部物とGUIDが無い実体は、停止後に同じ対象を特定できないため含めない。
    if (!object || object->IsEditorInternal() || object->GetPersistentGuid().empty())
        return false;
    const std::string className = object->GetClassName();
    const std::string category = object->GetSaveCategory();

    // Player・Enemy等は実行時状態そのものが変わるため、停止時の差分候補へ混ぜません。
    static constexpr const char* kRuntimeClasses[] = {
        "Player", "Enemy", "Item", "Bullet"
    };

    // 実行中の状態変化を持つクラスは、編集の持ち帰り候補から外す。
    if (std::any_of(std::begin(kRuntimeClasses), std::end(kRuntimeClasses), [&className](const char* value) {
        return className == value;
    })) return false;
    // 保存カテゴリから実行時の役者と分かる場合も、持ち帰り候補には入れない。
    if (category == "Player" || category == "Enemy" || category == "Item")
        return false;
    return true;
}

bool PlayModeChangeTracker::IsRuntimeDrivenTransform(const engine::Object3D* object) const {
    // 実体を取得できない場合は、その姿勢を編集差分として扱わない。
    if (!object)
        return true;
    const std::string className = object->GetClassName();
    // ギミックと生成器の姿勢は実行中の自動移動を含むため、編集として採用しない。
    const std::string category = object->GetSaveCategory();
    return className == "Gimmick" || className == "Spawner" ||
        category == "Gimmick";
}

bool PlayModeChangeTracker::IsIdentityKey(const std::string& key) {
    // 同じ Object を追跡する識別情報と親子関係は持ち帰り対象外とし、停止時に復元したシーンの値を使う。
    return key == "guid" || key == "name" || key == "type" || key == "saveCategory" ||
        key == "parentName" || key == "parentGuid";
}

bool PlayModeChangeTracker::IsTransformKey(const std::string& key) {
    // 回転は Euler 角と Quaternion の両方を姿勢の比較対象に含める。設定値だけを持ち帰る場合は両方保持する。
    return key == "translate" || key == "scale" || key == "rotate" || key == "quaternion";
}

nlohmann::json PlayModeChangeTracker::ExtractTransform(const nlohmann::json& state) {
    // オブジェクト全体ではなく位置・回転・大きさだけを実行中の差分へ抽出する。
    nlohmann::json result = nlohmann::json::object();
    // JSONオブジェクトでなければ配置のキーを抽出できないため、空で返す。
    if (!state.is_object())
        return result;
    for (auto iterator = state.begin(); iterator != state.end(); ++iterator) {
        // 位置、回転、拡縮に対応するキーだけを比較用にコピーする。
        if (IsTransformKey(iterator.key()))
            result[iterator.key()] = iterator.value();
    }

    return result;
}

nlohmann::json PlayModeChangeTracker::ExtractProperties(const nlohmann::json& state) {
    // 実行中の差分確認では位置と識別子を除き、調整可能なプロパティだけ比較する。
    nlohmann::json result = nlohmann::json::object();
    // 比較する状態がJSONオブジェクトでない場合は、空の設定として扱う。
    if (!state.is_object())
        return result;
    for (auto iterator = state.begin(); iterator != state.end(); ++iterator) {
        // 配置と識別に属さない項目だけを設定値の比較に含める。
        if (!IsTransformKey(iterator.key()) && !IsIdentityKey(iterator.key())) {
            result[iterator.key()] = iterator.value();
        }
    }

    return result;
}

void PlayModeChangeTracker::CaptureBaseline() {
    baselineByGuid_.clear();
    candidates_.clear();
    open_ = false;
    // シーン管理やエディターが無い場合、あるいは遷移中は比較基準を作らない。
    if (!sceneManager_ || !editor_ || sceneManager_->IsTransitioning())
        return;
    BaseScene* scene = sceneManager_->GetCurrentScene();
    // 現在シーンが取得できなければ、対象の記録を行えない。
    if (!scene)
        return;

    // 復元後も同じ対象を探せるよう、Object のアドレスではなく永続 GUID で記録する。
    for (const auto& object : scene->GetObjects()) {
        // 停止後も識別できる編集対象だけを初回の比較基準へ登録する。
        if (!IsEligible(object.get()))
            continue;
        const std::string guid = object->EnsurePersistentGuid();
        BaselineEntry entry;
        entry.name = object->GetName();
        entry.className = object->GetClassName();
        entry.state = editor_->CaptureObjectState(object.get());
        baselineByGuid_[guid] = std::move(entry);
    }
}

void PlayModeChangeTracker::CaptureRuntimeChanges() {
    // 停止直前の状態をPlay開始時のGUID別Snapshotと比較し、採用候補だけ残す。
    candidates_.clear();
    // 比較基準が空か必要な管理が無い場合は、持ち帰り候補の検出を見送る。
    if (!sceneManager_ || !editor_ || baselineByGuid_.empty())
        return;
    BaseScene* scene = sceneManager_->GetCurrentScene();
    // 停止直前のシーンを取得できなければ、比較を行わない。
    if (!scene)
        return;
    for (const auto& object : scene->GetObjects()) {
        // 記録時と同じ対象条件を満たすオブジェクトだけを比較する。
        if (!IsEligible(object.get()))
            continue;
        const auto baseline = baselineByGuid_.find(object->GetPersistentGuid());
        // Play 開始前に存在しなかった Object は、停止後の持ち帰り候補から除く。
        if (baseline == baselineByGuid_.end())
            continue;
        const nlohmann::json runtimeState = editor_->CaptureObjectState(object.get());

        // Gimmick 等の実行時移動を編集差分と誤認しない。
        const bool transformChanged = !IsRuntimeDrivenTransform(object.get()) &&
            ExtractTransform(baseline->second.state) != ExtractTransform(runtimeState);
        const bool propertiesChanged = ExtractProperties(baseline->second.state) != ExtractProperties(runtimeState);
        // 持ち帰れる姿勢と設定値のどちらも変わらなければ、候補を追加しない。
        if (!transformChanged && !propertiesChanged)
            continue;

        ChangeCandidate candidate;
        candidate.guid = object->GetPersistentGuid();
        candidate.name = baseline->second.name;
        candidate.className = baseline->second.className;
        candidate.beforeState = baseline->second.state;
        candidate.runtimeState = runtimeState;
        candidate.transformChanged = transformChanged;
        candidate.propertiesChanged = propertiesChanged;
        candidate.applyTransform = transformChanged;
        candidate.applyProperties = propertiesChanged;
        candidates_.push_back(std::move(candidate));
    }
}

void PlayModeChangeTracker::OnSceneRestored(bool restored) {
    baselineByGuid_.clear();

    // シーンの復元に失敗した場合は、変更を当てる先が無いため候補を破棄する。
    if (!restored) {
        candidates_.clear();
        open_ = false;
        return;
    }

    // 停止後は作業を遮らず、メニューから必要なときだけ確認します。
    open_ = false;
    // 復元できて候補も存在する場合だけ、メニューで差分を確認できる旨を示す。
    if (!candidates_.empty())
        statusMessage_ = "Play中の編集可能な変更を検出しました。";
}

void PlayModeChangeTracker::ApplySelectedChanges() {
    // 復元後のシーンと履歴登録を使える場合だけ、持ち帰りを適用する。
    if (!sceneManager_ || !editor_ || !sceneManager_->GetCurrentScene())
        return;
    std::int32_t appliedCount = 0;

    for (const ChangeCandidate& candidate : candidates_) {
        // 実際に変更され、反映が選ばれている項目が一つも無い候補は飛ばす。
        if ((!candidate.applyTransform || !candidate.transformChanged) &&
            (!candidate.applyProperties || !candidate.propertiesChanged)) continue;
        engine::Object3D* object = sceneManager_->GetCurrentScene()->FindObjectByPersistentGuid(candidate.guid);
        // 復元したシーン内で同じGUIDを確認できない場合は適用しない。
        if (!object)
            continue;
        const nlohmann::json beforeApply = editor_->CaptureObjectState(object);
        // 設定値を持ち帰るときは Play 中の JSON を土台にする。姿勢だけなら、停止後の設定値を保持したまま差分を重ねる。
        nlohmann::json merged = candidate.applyProperties ? candidate.runtimeState : beforeApply;

        // GUID・名前・親子識別子は復元後の編集シーンのものを維持する。
        for (auto iterator = beforeApply.begin(); iterator != beforeApply.end(); ++iterator) {
            // 識別用のキーには、プレイ中の値を適用せず編集側の値を維持する。
            if (IsIdentityKey(iterator.key()))
                merged[iterator.key()] = iterator.value();
            // 姿勢を持ち帰らない場合は、計算の土台に実行中のJSONを使っていても、編集側の姿勢を戻す。
            if (!candidate.applyTransform && IsTransformKey(iterator.key()))
                merged[iterator.key()] = iterator.value();
        }

        // 姿勢の反映が選択された場合は、プレイ停止直前の姿勢を設定へ重ねる。
        if (candidate.applyTransform) {
            for (auto iterator = candidate.runtimeState.begin(); iterator != candidate.runtimeState.end(); ++iterator) {
                // 実行中の差分から抽出するのは姿勢のキーだけに絞る。
                if (IsTransformKey(iterator.key()))
                    merged[iterator.key()] = iterator.value();
            }
        }

        // 再描画で新しい姿勢を使えるようローカル・ワールド行列を更新して、適用前の状態を Undo 用に登録する。
        object->ImportFromJson(merged);
        object->UpdateLocalMatrix();
        object->UpdateWorldMatrix();
        editor_->RegisterObjectEdited(object, beforeApply, "Play中変更を持ち帰り");
        ++appliedCount;
    }

    statusMessage_ = std::to_string(appliedCount) + " Objectの変更を編集状態へ反映しました。";
    candidates_.clear();
    open_ = false;
}

void PlayModeChangeTracker::Draw() {
    // Play中の差分はObjectごとにTransformと設定値を独立して選択させる。
    if (!open_)
        return;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(760.0f, 520.0f), ImGuiCond_FirstUseEver);

    // 折り畳みなどで内容を描画できない場合も、Beginと対になるEndを呼ぶ。
    if (!ImGui::Begin("Play中変更の持ち帰り", &open_)) {
        ImGui::End();
        return;
    }

    ImGui::TextWrapped("停止前に変わった編集可能Objectだけを表示しています。Player・Enemyの状態とGimmickの実行時Transformは除外済みです。");
    ImGui::TextDisabled("Transformと設定値をObjectごとに選んで反映できます。反映操作はUndo可能です。");
    ImGui::TextWrapped("この一覧は一時データです。次のPlay開始で更新されます。保存済みリプレイとは別の機能です。");
    ImGui::Separator();
    ImGui::BeginChild("PlayModeChangeCandidates", ImVec2(0.0f, -52.0f), true);

    for (std::size_t index = 0; index < candidates_.size(); ++index) {
        ChangeCandidate& candidate = candidates_[index];
        ImGui::PushID(static_cast<std::int32_t>(index));
        ImGui::SeparatorText(candidate.name.c_str());
        ImGui::TextDisabled("%s / %s", candidate.className.c_str(), candidate.guid.c_str());
        // 姿勢に変更がある行にだけ、反映するかを選ぶ欄を出す。
        if (candidate.transformChanged)
            ImGui::Checkbox("Transformを反映", &candidate.applyTransform);
        // 姿勢が変わっていない行では、選択欄の位置に状態を表示する。
        else ImGui::TextDisabled("Transform変更なし");
        ImGui::SameLine();
        // 設定値に変更がある行にだけ、反映するかを選ぶ欄を出す。
        if (candidate.propertiesChanged)
            ImGui::Checkbox("設定値を反映", &candidate.applyProperties);
        // 設定値が変わっていない行では、選択欄の代わりに状態を表示する。
        else ImGui::TextDisabled("設定値変更なし");
        ImGui::PopID();
    }

    ImGui::EndChild();
    // 反映ボタンが押された場合、選択済みの項目だけを編集シーンへ適用する。
    if (ImGui::Button("選択した変更を反映"))
        ApplySelectedChanges();
    ImGui::SameLine();

    // 全破棄が選ばれた場合は、候補を消してウィンドウを閉じる。
    if (ImGui::Button("すべて破棄")) {
        candidates_.clear();
        open_ = false;
        statusMessage_ = "Play中変更を破棄しました。";
    }

    ImGui::SameLine();
    ImGui::TextDisabled("%s", statusMessage_.c_str());
    ImGui::End();
}

#endif
