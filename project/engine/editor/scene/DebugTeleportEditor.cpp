#include "engine/editor/scene/DebugTeleportEditor.h"
#ifdef USE_IMGUI
#include "engine/editor/core/DebugEditor.h"
#include "engine/editor/core/EditorManager.h"
#include "engine/utility/JsonFile.h"
#include "SceneManager.h"
#include "imgui.h"
#include "json.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numbers>

namespace {
constexpr char kStatePath[] = "output/editor_state/debug_teleport_points.json";
constexpr float kRadiansPerDegree = std::numbers::pi_v<float> / 180.0f;

// 名前があり、位置と向きの全成分が有限なら、移動先として要求できる。
bool IsValid(const DebugTeleportEditor::Point& point) {
    return !point.name.empty() && std::isfinite(point.position.x) && std::isfinite(point.position.y) &&
           std::isfinite(point.position.z) && std::isfinite(point.yawRadians);
}
}

DebugTeleportEditor& DebugTeleportEditor::GetInstance() {
    static DebugTeleportEditor editor;
    return editor;
}

void DebugTeleportEditor::Configure(Binding binding) { binding_ = binding; }

void DebugTeleportEditor::Initialize(SceneManager* scenes, DebugEditor* editor) {
    scenes_ = scenes;
    editor_ = editor;
}

void DebugTeleportEditor::Finalize() {
    scenes_ = nullptr;
    editor_ = nullptr;
    pointsByScene_.clear();
    activeKey_.clear();
    status_.clear();
}

std::vector<DebugTeleportEditor::Point>& DebugTeleportEditor::GetPoints(const std::string& key, BaseScene& scene) {
    // 保存済みの空配列も有効な設定とし、削除した初期地点を再生成しません。
    auto [entry, created] = pointsByScene_.try_emplace(key);
    auto& points = entry->second;
    // このシーンの地点をすでに取り込んでいる場合は、ファイルを読み直さず編集中の一覧を返す。
    if (!created) return points;

    std::ifstream input(kStatePath);
    // 保存ファイルを開ける場合だけ、該当シーンの地点を読み込む。
    if (input) {
        const auto root = nlohmann::json::parse(input, nullptr, false);
        // 対象シーンのキーが配列なら、空でも保存済みとして扱い内蔵の地点へ戻さない。
        if (root.is_object() && root.contains(key) && root[key].is_array()) {
            for (const auto& item : root[key]) {
                try {
                    const auto values = item.at("position").get<std::array<float, 3>>();
                    Point point{item.at("name").get<std::string>(), {values[0], values[1], values[2]},
                                item.at("yawRadians").get<float>()};
                    // 名前と有限の座標・向きが揃った地点だけを一覧に入れる。
                    if (IsValid(point)) points.push_back(std::move(point));
                } catch (const nlohmann::json::exception&) {
                    // 未完成の1件を除外し、同じシーンの読める地点を残します。
                }
            }
            return points;
        }
    }

    // このシーンの設定が未作成のとき、ゲーム側の既定地点を編集できる形で取り込みます。
    if (binding_.defaults) points = binding_.defaults(scene);
    return points;
}

bool DebugTeleportEditor::SavePoints(const std::string& key, const std::vector<Point>& points) {
    auto entries = nlohmann::json::array();
    for (const auto& point : points) {
        // 一地点でも名前・座標・向きが無効なら、部分保存せず失敗を返す。
        if (!IsValid(point)) return false;
        entries.push_back({{"name", point.name}, {"position", {point.position.x, point.position.y, point.position.z}},
                           {"yawRadians", point.yawRadians}});
    }
    return jsonFile::UpdateObject(kStatePath, {{key, std::move(entries)}});
}

void DebugTeleportEditor::DrawImGui() {
    // 遷移中のSceneや移動を提供しないSceneへ要求を出さないようにします。
    if (!scenes_ || scenes_->IsTransitioning() || !binding_.sceneKey || !binding_.request) {
        ImGui::TextDisabled("移動可能なシーンを開いてください。");
        return;
    }
    auto* scene = scenes_->GetCurrentScene();
    const std::string runtimeKey = scene ? binding_.sceneKey(*scene) : std::string{};
    // ゲーム側の識別子が空なら、操作可能なステージでないため移動設定を開かない。
    if (runtimeKey.empty()) {
        ImGui::TextDisabled("プレイヤーを操作できるステージを開いてください。");
        return;
    }
    const auto& context = scenes_->GetActiveSceneLoadContext();
    const std::string key = context.IsSceneAsset() ? "asset:" + context.sceneAssetId : runtimeKey;
    // シーンを切り替えた場合は、前のシーンに出した保存・移動の結果を消す。
    if (key != activeKey_) {
        activeKey_ = key;
        status_.clear();
    }
    auto& points = GetPoints(key, *scene);

    // 現在の選択位置やプレイヤーの姿勢から地点を追加し、詳細欄で調整します。
    const auto origin = binding_.playerPoint ? binding_.playerPoint(*scene) : std::nullopt;
    const auto* selected = editor_ ? editor_->GetSelectedObject() : nullptr;
    ImGui::TextUnformatted(key.c_str());
    // 地点を追加するボタンが押された場合は、現在姿勢か原点を初期値にする。
    if (ImGui::Button("地点を追加")) {
        Point point = origin.value_or(Point{});
        // エディターで選択したObjectがあれば、地点の移動先だけはそのワールド位置を使う。
        if (selected) point.position = selected->GetWorldPosition();
        point.name = "地点 " + std::to_string(points.size() + 1);
        points.push_back(std::move(point));
    }
    ImGui::SameLine();
    // 保存を押したときだけ設定ファイルへ書き、結果を画面へ残す。
    if (ImGui::Button("地点を保存")) {
        status_ = SavePoints(key, points) ? "地点を保存しました。" : "保存できません。地点名と座標を確認してください。";
    }

    // 移動と地点編集を同じ一覧で扱い、削除で参照が無効になる操作はループの末尾に置きます。
    for (std::size_t i = 0; i < points.size(); ++i) {
        auto& point = points[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::BeginDisabled(!IsValid(point));
        const std::string title = point.name + " へ移動###move";
        // 有効な地点への移動が押された場合だけ、ゲーム側に移動の予約を依頼する。
        if (ImGui::Button(title.c_str())) {
            const bool queued = binding_.request(*scene, point);
            status_ = queued ? "次のゲーム更新で移動します。" : "今の状態では移動できません。";
            // Play開始時に解除されたInspectorの選択を戻し、続けて別の地点へ移動できます。
            if (queued) EditorManager::GetInstance()->SetSelectedObject(this);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        // 地点の編集欄を開いた場合だけ、名前・座標と向きの入力を処理する。
        if (ImGui::TreeNode("地点の編集")) {
            char name[128]{};
            std::snprintf(name, sizeof(name), "%s", point.name.c_str());
            // 名前を編集したときは、変更された入力バッファに置き換える。
            if (ImGui::InputText("地点名", name, sizeof(name))) point.name = name;
            float xyz[] = {point.position.x, point.position.y, point.position.z};
            // ワールド位置の3成分が変更されたときに、編集用の地点へまとめて反映する。
            if (ImGui::DragFloat3("位置（ワールド）", xyz, 0.1f)) point.position = {xyz[0], xyz[1], xyz[2]};
            float degrees = point.yawRadians / kRadiansPerDegree;
            // 度表示の向きを変更した場合は、保存とゲームで使うラジアンへ戻す。
            if (ImGui::DragFloat("向き（度）", &degrees, 1.0f)) point.yawRadians = degrees * kRadiansPerDegree;
            // プレイヤーの姿勢を取得できていてボタンを押したときだけ、現在の位置・向きを複製する。
            if (origin && ImGui::Button("現在のプレイヤー位置と向きを使う")) {
                point.position = origin->position;
                point.yawRadians = origin->yawRadians;
            }
            const bool remove = ImGui::Button("地点を削除");
            ImGui::TreePop();
            // 削除を選んだ場合は要素を外し、参照が無効になるのでこのフレームの走査を終える。
            if (remove) {
                points.erase(points.begin() + static_cast<std::ptrdiff_t>(i));
                ImGui::PopID();
                break;
            }
        }
        ImGui::PopID();
    }
    ImGui::TextDisabled("停止中は再生を開始してから移動します。");
    // 保存や移動の結果が残っている場合だけ、操作欄の下に表示する。
    if (!status_.empty()) ImGui::TextUnformatted(status_.c_str());
}
#endif
