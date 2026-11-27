#define NOMINMAX
#include <cstdint>
#include "game/editor/CharacterStatusEditor.h"
#include "game/system/rules/GameplayStatusManager.h"
#include "game/editor/EnemyAttackPreviewRegistry.h"
#include "EditorObjectFactoryRegistry.h"
#include "BaseScene.h"
#ifdef USE_IMGUI
#include "imgui.h"
#endif
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace {
namespace fs = std::filesystem;

constexpr const char* kModelRootDirectory = "Resources/3DModel";

// 拡張子の比較用に、小文字へ変えた文字列を返す。
std::string ToLowerAscii(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    return text;
}

// モデル選択候補は OBJ と glTF の二形式に絞る。glTF はテキスト形式とバイナリ形式を含む。
bool IsSupportedModelFile(const fs::path& path) {
    const std::string extension = ToLowerAscii(path.extension().string());
    return extension == ".obj" || extension == ".gltf" || extension == ".glb";
}

std::string MakeModelKeyFromFile(const fs::path& modelFile) {
    // モデル名と同名のフォルダに入ったファイルは、フォルダ名をAssetキーにする。
    fs::path relative = fs::relative(modelFile, kModelRootDirectory);
    fs::path parent = relative.parent_path();

    // 相対パスの親とファイルの拡張子を除いた名前が一致する場合は、親パスをそのままキーにする。
    if (!parent.empty() && parent.filename() == modelFile.stem()) {
        return parent.generic_string();
    }

    relative.replace_extension("");
    return relative.generic_string();
}

std::string GetModelDisplayName(const std::string& modelName) {
    // ステータス調整画面だけ日本語の通称を付け、未登録モデルは短い元名で示す。
    // モデルの参照名が空なら、選択欄には未設定と表示する。
    if (modelName.empty())
        return "未設定";
    const std::string shortName = fs::path(modelName).filename().string();
    // ピンク個体のモデル名を、選択欄ではゲーム内の通称に置き換える。
    if (shortName == "slime_pink")
        return "ピンクスライム";
    // 赤い個体にはファイアスライムの表示名を使う。
    if (shortName == "slime_red")
        return "ファイアスライム";
    // 黄色い個体にはサンダースライムの表示名を使う。
    if (shortName == "slime_yellow")
        return "サンダースライム";
    // 黒い個体にはブラックスライムの表示名を使う。
    if (shortName == "slime_black")
        return "ブラックスライム";
    // 通常のモデルには基本形態の表示名を使う。
    if (shortName == "slime")
        return "スライム";
    // リングバーナーも選択欄では日本語名を使う。
    if (shortName == "ring_burner")
        return "リングバーナー";
    return shortName;
}

std::vector<std::string> characterModelNames;
std::string statusText;
bool modelListReady = false;
bool DrawModelPicker(const char* label, const char* id, std::string& modelName);
void RefreshCharacterModelList();
bool DrawStatusEditor(GameplayStatusManager::CharacterStatus& status, const char* id) {
#ifdef USE_IMGUI
    // 新規出現時に使う定義値だけ編集し、現在HPの実行状態とは分けます。
    ImGui::PushID(id);
    bool changed = DrawModelPicker("モデル", "StatusModel", status.modelName);

    changed |= ImGui::DragFloat("最大HP", &status.maxHp, 1.0f, 1.0f, 9999.0f);
    status.maxHp = (std::max)(status.maxHp, 1.0f);
    ImGui::TextDisabled("現在HPは実行状態です。新規出現時は最大HPで開始します。");

    changed |= ImGui::DragFloat("攻撃力倍率", &status.attackPower, 0.05f, 0.0f, 100.0f);
    changed |= ImGui::DragFloat("移動速度", &status.speed, 0.1f, 0.0f, 100.0f);
    changed |= ImGui::DragFloat("重力", &status.gravity, 0.1f, -100.0f, 200.0f);
    changed |= ImGui::DragFloat("最大落下速度", &status.maxFallSpeed, 0.1f, 0.0f, 500.0f);
    changed |= ImGui::DragFloat("ジャンプ力", &status.jumpPower, 0.1f, 0.0f, 100.0f);
    changed |= ImGui::DragFloat("感知範囲", &status.detectionRange, 0.5f, 0.0f, 500.0f);
    changed |= ImGui::DragFloat3("タイプ共通スケール", &status.scale.x, 0.005f, 0.001f, 20.0f, "%.3f");

    // 移動設定のツリーが開いているときに描画する。接地・段差の欄は Sweep が有効な間だけ操作できる。
    if (ImGui::TreeNodeEx("Character Motor", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("標準OFF時は従来の移動結果を維持します。タイプごとに段階導入できます。");
        changed |= ImGui::Checkbox("高速移動をSweep判定", &status.motorContinuousCollision);
        ImGui::BeginDisabled(!status.motorContinuousCollision);
        changed |= ImGui::Checkbox("接地面へ吸着", &status.motorSnapToGround);
        changed |= ImGui::DragFloat("最大登坂角度", &status.motorMaxSlopeDegrees, 0.1f, 0.0f, 89.9f, "%.1f deg");
        changed |= ImGui::DragFloat("自動段差", &status.motorStepHeight, 0.01f, 0.0f, 5.0f, "%.2f m");
        changed |= ImGui::DragFloat("接地探索距離", &status.motorGroundProbeDistance, 0.005f, 0.0f, 2.0f, "%.3f m");
        changed |= ImGui::DragFloat("Skin幅", &status.motorSkinWidth, 0.001f, 0.0f, 0.25f, "%.3f m");
        ImGui::EndDisabled();
        ImGui::TreePop();
    }

    // スライダー以外から入る値も含め、負の速度や極小スケールを実行時へ渡さないよう補正します。
    status.attackPower = (std::max)(status.attackPower, 0.0f);
    status.speed = (std::max)(status.speed, 0.0f);
    status.maxFallSpeed = (std::max)(status.maxFallSpeed, 0.0f);
    status.jumpPower = (std::max)(status.jumpPower, 0.0f);
    status.detectionRange = (std::max)(status.detectionRange, 0.0f);
    status.scale.x = (std::max)(status.scale.x, 0.001f);
    status.scale.y = (std::max)(status.scale.y, 0.001f);
    status.scale.z = (std::max)(status.scale.z, 0.001f);
    status.motorMaxSlopeDegrees = std::clamp(status.motorMaxSlopeDegrees, 0.0f, 89.9f);
    status.motorStepHeight = (std::max)(status.motorStepHeight, 0.0f);
    status.motorGroundProbeDistance = (std::max)(status.motorGroundProbeDistance, 0.0f);
    status.motorSkinWidth = std::clamp(status.motorSkinWidth, 0.0f, 0.25f);

    // 変身の制限を有効にした場合だけ時間を編集し、値は次回の有効化に備えて保持します。
    changed |= ImGui::Checkbox("変身に制限時間を使う", &status.morphLimited);
    ImGui::BeginDisabled(!status.morphLimited);
    changed |= ImGui::DragFloat("変身時間（秒）", &status.morphDuration, 0.1f, 0.1f, 999.0f, "%.1f");
    ImGui::EndDisabled();
    status.morphDuration = (std::max)(status.morphDuration, 0.1f);

    ImGui::PopID();
    return changed;
#else
    (void)status;
    (void)id;
    return false;
#endif
}

bool DrawModelPicker(const char* label, const char* id, std::string& modelName) {
#ifdef USE_IMGUI
    // 保存にはモデルの参照名を使い、一覧では表示名も併記します。
    ImGui::PushID(id);
    bool changed = false;
    const std::string preview = GetModelDisplayName(modelName) + "  /  " + modelName;

    // 選択欄を開いた場合だけ、候補名と保存用のキーを列挙する。
    if (ImGui::BeginCombo(label, preview.c_str())) {
        for (const std::string& candidate : characterModelNames) {
            const bool selected = candidate == modelName;
            const std::string itemLabel = GetModelDisplayName(candidate) + "  /  " + candidate;

            // 候補を選択したときだけ、現在の参照名を差し替える。
            if (ImGui::Selectable(itemLabel.c_str(), selected)) {
                modelName = candidate;
                changed = true;
            }

            // 現在のモデルに対応する行を、選択欄を開いた際の初期フォーカスにする。
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }

    ImGui::PopID();
    return changed;
#else
    (void)label;
    (void)id;
    (void)modelName;
    return false;
#endif
}

void RefreshCharacterModelList() {
    // 従来と同じモデルルートを走査し、対応形式を重複のない一覧へまとめます。
    modelListReady = true;
    characterModelNames.clear();
    try {
        const fs::path root(kModelRootDirectory);

        // モデルのルートが存在しなければ空の候補欄にし、更新操作で再走査できるようにします。
        if (!fs::exists(root) || !fs::is_directory(root)) {
            return;
        }

        // 対応形式の実ファイルから参照名を求め、同じ参照名は一度だけ追加します。
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            // 通常ファイルかつ対応形式の場合だけ、モデルの参照名へ変換する。
            if (!entry.is_regular_file() || !IsSupportedModelFile(entry.path())) {
                continue;
            }

            const std::string modelKey = MakeModelKeyFromFile(entry.path());

            // 空の参照名と、別のファイルからすでに同じキーで登録済みの候補を除く。
            if (!modelKey.empty() && std::find(characterModelNames.begin(), characterModelNames.end(), modelKey) == characterModelNames.end()) {
                characterModelNames.push_back(modelKey);
            }
        }

        std::sort(characterModelNames.begin(), characterModelNames.end());
    // 走査途中でアクセスに失敗した場合、途中までの候補は残さず空に戻します。
    } catch (...) {
        characterModelNames.clear();
    }
}


void SavePending() {
#ifdef USE_IMGUI
    auto* manager = GameplayStatusManager::GetInstance();
    // 他の欄・ツールへ移っても、ドラッグが終わった次の更新で保存します。
    // 未保存の変更があり、ImGui が有効で編集中の項目がなくなった時点で保存を試す。
    if (manager->IsDirty() && ImGui::GetCurrentContext() && !ImGui::IsAnyItemActive())
        statusText = manager->SaveIfDirty() ? "基本ステータスを保存しました。" : "保存に失敗しました。変更はメモリに保持しています。";
#endif
}

bool Draw(BaseScene* scene, const std::string& type) {
#ifdef USE_IMGUI
    // 同じ種類の配置物で共有する基本値を開く。空の種別はコピー形態共通のプレイヤーを指す。
    auto* manager = GameplayStatusManager::GetInstance();
    manager->Initialize();
    bool changed = false;
    // 種類共通の設定欄が開いているときだけ、入力項目を描画する。
    if (ImGui::CollapsingHeader("基本ステータス（種類共通）", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped(type.empty()
            ? "全コピー形態で共通のPlayer基本値です。コピー固有の攻撃は攻撃設定で調整します。"
            : "同じ種類の敵すべてに使う設定です。配置した1体だけの変更ではありません。");
        ImGui::TextDisabled("操作を離すと自動保存。表示中のプレビューは先頭へ戻して反映します。");
        auto* status = type.empty() ? &manager->GetMutablePlayerStatus() : manager->FindMutableEnemyStatus(type);
        // 設定が登録済みの場合にだけ編集を出し、モデルの候補は初めて表示するときに用意する。
        if (status) {
            // 一覧がまだ用意されていなければ、初めて編集欄を出す前に走査する。
            if (!modelListReady) RefreshCharacterModelList();
            bool statusChanged = DrawStatusEditor(*status, "SelectedCharacterStatus");
            // 加減速はプレイヤー共通の移動設定なので、敵の種類別編集には出さない。
            if (type.empty() && ImGui::TreeNodeEx("通常移動の加速・減速", ImGuiTreeNodeFlags_DefaultOpen)) {
                auto& movement = manager->GetMutablePlayerLocomotion();
                statusChanged |= ImGui::DragFloat("最高速まで（秒）", &movement.accelerationTime, 0.01f, 0.0f, 3.0f, "%.2f");
                statusChanged |= ImGui::DragFloat("停止まで（秒）", &movement.brakingTime, 0.01f, 0.0f, 3.0f, "%.2f");
                statusChanged |= ImGui::DragFloat("空中の追従倍率", &movement.airControl, 0.01f, 0.1f, 1.0f, "%.2f");
                ImGui::TextDisabled("最高速度は上の移動速度を使用。0秒なら即時に切り替わります。");
                movement.Normalize();
                ImGui::TreePop();
            }
            // 編集中の値はその場で配置済み対象へ適用し、永続化は保留中の保存へ渡す。
            if (statusChanged) {
                manager->MarkDirty(type);
                const std::int32_t count = characterStatusEditor::Apply(scene, type);
                statusText = "配置済みの対象へ反映: " + std::to_string(count) + " 件";
                changed = true;
            }
        // この種類の定義が未登録なら、編集欄の代わりに未登録の表示を出す。
        } else ImGui::TextDisabled("この種類の基本ステータスは登録されていません。");

        // 一覧更新ボタンを押したら、後から置かれたファイルも含めて候補を走査し直す。
        if (ImGui::Button("モデル一覧更新")) RefreshCharacterModelList();
        // ファイル全体を読み直す操作と標準値に戻す操作は、現在の表示対象を超えて全種類に適用する。
        if (ImGui::TreeNode("共通設定ファイルの操作")) {
            ImGui::TextWrapped("再読込・標準値への復元は、Playerとすべての敵タイプが対象です。");
            // 再読み込みを押した場合は全設定をディスクから読み直す。
            if (ImGui::Button("全種類を再読込")) {
                changed = manager->Reload();
                // 読み込みに成功した場合だけ、配置済みの物体へ値を再適用する。
                if (changed) characterStatusEditor::Apply(scene, "*");
                statusText = changed ? "全種類の設定を再読込しました。" : "再読込に失敗しました。現在値を保持します。";
            }
            ImGui::SameLine();
            // 標準値へ戻すボタンは、すぐに変更せず確認画面を開く。
            if (ImGui::Button("全種類を標準値へ")) ImGui::OpenPopup("ResetAllStatus");
            // 確認画面が開いている間だけ、適用と取消の入力を受け取る。
            if (ImGui::BeginPopupModal("ResetAllStatus", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextWrapped("Playerと全敵タイプの設定を標準値へ戻して保存します。");
                // 初期値で上書きする操作は確認を通った場合だけ実行し、配置物の値も更新する。
                if (ImGui::Button("戻して保存")) {
                    manager->ResetToDefaults();
                    characterStatusEditor::Apply(scene, "*");
                    changed = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                // 取り消した場合は設定を保ったまま確認画面を閉じる。
                if (ImGui::Button("キャンセル")) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::TreePop();
        }
        // 編集中の連続入力が落ち着いた時点で保存する経路を進め、表示側へ結果を返す。
        SavePending();
        ImGui::TextWrapped("%s", statusText.c_str());
        ImGui::TextWrapped("保存先: %s", GameplayStatusManager::GetSettingsPath(type).c_str());
    }
    return changed;
#else
    (void)scene; (void)type;
    return false;
#endif
}
} // namespace

std::int32_t characterStatusEditor::Apply(BaseScene* scene, const std::string& type) {
    // 配置済みの対象を更新する処理なので、シーンがない場合は反映件数ゼロを返します。
    if (!scene) return 0;
    auto* manager = GameplayStatusManager::GetInstance();
    std::int32_t count = 0;
    for (const auto& object : scene->GetObjects()) {
        // 空参照とリプレイから除外済みの物体は更新しない。隔離プレビューは再生成側で状態を更新する。
        if (!object || object->IsEditorInternal() || object->IsReplayRemoved()) continue;
        // 配置物の敵種別が空ならパラメータの種別を参照し、全種・プレイヤー・指定の敵タイプを選別します。
        std::string enemyType = object->GetEnemyType();
        // 配置物から敵の種別を直接取得できずパラメーターがある場合は、そちらを調べる。
        if (enemyType.empty() && object->GetParameters()) enemyType = object->GetParameters()->enemyType;
        const bool matches = type == "*" || (type.empty() ? object->GetClassName() == "Player"
            : object->GetClassName() == "Enemy" && enemyType == type);
        // 適用に成功した対象は行列を更新し、エディターに返す反映件数へ加えます。
        // 編集中の変更で配置済みのキャラクターを回復させないため、現在HPを初期化せず適用する。
        if (matches && manager->ApplyManagedStatus(object.get(), false)) {
            object->UpdateLocalMatrix();
            object->UpdateWorldMatrix();
            ++count;
        }
    }
    return count;
}

void characterStatusEditor::Register() {
    // プレビュー側へ、対象一覧の取得・表示中の編集・ドラッグ終了後の保存の入口を登録します。
    EnemyAttackPreviewRegistry::RegisterStatusEditor({
        [] {
            auto* manager = GameplayStatusManager::GetInstance();
            manager->Initialize();
            std::vector<EnemyAttackPreviewRegistry::StatusTarget> targets;
            // 攻撃確認がある種類を従来の順に並べ、基本値だけ持つ種類を末尾へ補います。
            for (const auto& entry : EnemyAttackPreviewRegistry::GetEntries())
                targets.push_back({entry.type, entry.displayName});
            for (const auto& type : manager->GetEnemyTypes()) {
                // 攻撃確認ですでに登録された種類は、基本値だけの枠として重複追加しない。
                if (std::any_of(targets.begin(), targets.end(), [&](const auto& t) { return t.type == type; })) continue;
                std::string label = type == "Bomb" ? "ボム" : type == "RingBurner" ? "リングバーナー" : type;
                for (const auto& entry : EditorObjectFactoryRegistry::GetTypes(EditorObjectFactoryRegistry::Kind::kEnemy))
                    // 生成器に同じ種類の ID が登録されていれば、その表示名を優先する。
                    if (entry.id == type) label = entry.displayName;
                targets.push_back({type, label + "（基本ステータス）"});
            }
            return targets;
        }, Draw, SavePending
    });
}
