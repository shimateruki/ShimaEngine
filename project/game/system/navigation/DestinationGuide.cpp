#define NOMINMAX
#include "game/system/navigation/DestinationGuide.h"
#include "Object3D.h"
#include "engine/utility/math/AnimationInterpolation.h"
#include "json.hpp"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#ifdef USE_IMGUI
#include "imgui.h"
#endif

namespace {
// 有限値は指定範囲へ収め、NaNや無限大は既定値に置き換えます。
float SafeRange(float value, float fallback, float lo, float hi) {
    return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
}
// ワールド座標の3成分すべてが有限値かを判定します。
bool Finite(const engine::Vector3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
}

// 設定値を補正して取り込み、追従と到着判定を初期状態にします。
void DestinationGuide::Initialize(const Settings& settings) {
    SetSettings(settings);
    Reset();
}

// 編集された値を許容範囲へ補正します。現在の到着状態や矢印の位置は保持します。
void DestinationGuide::SetSettings(const Settings& settings) {
    settings_ = settings;
    const Settings defaults;
    settings_.distance = SafeRange(settings.distance, defaults.distance, 0.0f, 15.0f);
    settings_.height = SafeRange(settings.height, defaults.height, 0.0f, 10.0f);
    settings_.scale = SafeRange(settings.scale, defaults.scale, 0.05f, 5.0f);
    settings_.followResponse = SafeRange(settings.followResponse, defaults.followResponse, 0.1f, 80.0f);
    settings_.turnResponse = SafeRange(settings.turnResponse, defaults.turnResponse, 0.1f, 80.0f);
    settings_.bobAmplitude = SafeRange(settings.bobAmplitude, defaults.bobAmplitude, 0.0f, 1.0f);
    settings_.bobSpeed = SafeRange(settings.bobSpeed, defaults.bobSpeed, 0.0f, 15.0f);
    settings_.arrivalDistance = SafeRange(settings.arrivalDistance, defaults.arrivalDistance, 0.1f, 20.0f);
    settings_.arrivalHeight = SafeRange(settings.arrivalHeight, defaults.arrivalHeight, 0.1f, 20.0f);
}

void DestinationGuide::Reset() {
    // 到着判定と追従姿勢の両方を消し、次の案内を現在位置から始めます。
    position_ = {};
    yaw_ = animationTime_ = 0.0f;
    poseInitialized_ = arrived_ = false;
}

// 矢印を隠します。再表示時に古い位置から補間しないよう、姿勢の初期化状態を戻します。
void DestinationGuide::Hide(engine::Object3D& arrow) {
    arrow.SetIsVisible(false);
    poseInitialized_ = false;
}

// 追従対象の近くに目的地へ向かう矢印を描きます。座標はワールド空間、deltaTimeは秒です。
void DestinationGuide::Update(engine::Object3D& arrow, const engine::Vector3& followerPosition,
                              const engine::Vector3& targetPosition, float deltaTime) {
    // 案内が無効、到着済み、またはどちらかの座標が有限でなければ矢印を隠す。
    if (!settings_.enabled || arrived_ || !Finite(followerPosition) || !Finite(targetPosition)) {
        Hide(arrow);
        return;
    }
    // 経過秒数が有限かつ正でなければ、表示と補間位置を進めない。
    if (!std::isfinite(deltaTime) || deltaTime <= 0.0f)
        return;
    engine::Vector3 direction = targetPosition - followerPosition;
    direction.y = 0.0f;
    // 到着と方角に使う距離は水平面で求め、高低差は到着条件だけに加える。
    const float distance = std::hypot(direction.x, direction.z);
    // 水平距離と高低差がそれぞれの許容範囲内に入ったときに、到着済みとして表示を終える。
    if (distance <= settings_.arrivalDistance &&
        std::abs(targetPosition.y - followerPosition.y) <= settings_.arrivalHeight) {
        arrived_ = true;
        Hide(arrow);
        return;
    }
    // 水平距離が0.001未満では向きの計算が不安定なため、矢印を隠す。
    if (distance < 0.001f) {
        Hide(arrow);
        return;
    }
    direction = direction / distance;
    // 矢印の水平位置は追従対象と目的地の中間までに制限し、近い目標を越えないようにする。
    auto desired = followerPosition + direction * (std::min)(settings_.distance, distance * 0.5f);
    desired.y = followerPosition.y + settings_.height;
    const auto delta = desired - position_;
    // 未初期化か、目標の表示位置が水平方向に20単位を超えて離れている場合は、現在の位置と向きを直接合わせる。
    if (!poseInitialized_ || std::hypot(delta.x, delta.z) > 20.0f) {
        position_ = desired;
        yaw_ = std::atan2(direction.x, direction.z);
        poseInitialized_ = true;
    // すでに近くへ表示している場合は、経過時間に応じて追従位置をなめらかに移す。
    } else {
        position_ = animationInterpolation::Damp(position_, desired, settings_.followResponse, deltaTime);
    }
    const auto toTarget = targetPosition - position_;
    // 矢印の基準点から目的地までの水平距離が十分あれば、その方角へ旋回させる。
    if (std::hypot(toTarget.x, toTarget.z) > 0.001f)
        yaw_ = animationInterpolation::DampAngle(yaw_, std::atan2(toTarget.x, toTarget.z), settings_.turnResponse, deltaTime);
    // 上下の揺れは経過秒に角速度を掛けて求める。時計は一時間で折り返して値の増大を防ぐ。
    animationTime_ = std::fmod(animationTime_ + deltaTime, 3600.0f);
    auto displayPosition = position_;
    displayPosition.y += std::sin(animationTime_ * settings_.bobSpeed) * settings_.bobAmplitude;
    arrow.SetTranslate(displayPosition);
    arrow.SetRotation({0.0f, yaw_, 0.0f});
    arrow.SetScale({settings_.scale, settings_.scale, settings_.scale});
    arrow.SetIsVisible(true);
    // Sceneの物理更新後に呼ぶため、このモデルだけ描画定数まで確定する。
    arrow.UpdateWorldMatrix(true);
}

// UTF-8パスからJSONを読みます。すべての読み込みと適用が成功した場合にtrueを返します。
bool DestinationGuide::LoadSettings(const std::string& path) {
    try {
        std::ifstream stream{std::filesystem::path(std::u8string(path.begin(), path.end()))};
        // 設定ファイルを開けなければ、現在の設定を保って終了する。
        if (!stream)
            return false;
        const auto data = nlohmann::json::parse(stream);
        // 名前付きの設定を読み込むため、JSONのルートがオブジェクトの場合だけ採用する。
        if (!data.is_object())
            return false;
        // 読み取りを全て終えてから反映し、壊れたJSONでは編集中の値を維持する。
        Settings loaded = settings_;
        loaded.enabled = data.value("enabled", loaded.enabled);
        loaded.distance = data.value("distance", loaded.distance);
        loaded.height = data.value("height", loaded.height);
        loaded.scale = data.value("scale", loaded.scale);
        loaded.followResponse = data.value("followResponse", loaded.followResponse);
        loaded.turnResponse = data.value("turnResponse", loaded.turnResponse);
        loaded.bobAmplitude = data.value("bobAmplitude", loaded.bobAmplitude);
        loaded.bobSpeed = data.value("bobSpeed", loaded.bobSpeed);
        loaded.arrivalDistance = data.value("arrivalDistance", loaded.arrivalDistance);
        loaded.arrivalHeight = data.value("arrivalHeight", loaded.arrivalHeight);
        SetSettings(loaded);
        return true;
    // JSONの解析や個別の型変換で例外が出た場合は、変更を適用せず失敗として返す。
    } catch (...) {
        return false;
    }
}

// 設定を一時ファイルへ書き、保存先を置き換えられた場合にtrueを返します。
bool DestinationGuide::SaveSettings(const std::string& path) const {
    try {
        const auto destination = std::filesystem::path(std::u8string(path.begin(), path.end()));
        // 親フォルダーが指定されたパスでは、一時ファイルを書き出す前にそのフォルダーを作る。
        if (!destination.parent_path().empty())
            std::filesystem::create_directories(destination.parent_path());
        auto temporary = destination;
        temporary += L".tmp";
        nlohmann::json data = {{"schemaVersion", 1}, {"enabled", settings_.enabled}};
        data["distance"] = settings_.distance;
        data["height"] = settings_.height;
        data["scale"] = settings_.scale;
        data["followResponse"] = settings_.followResponse;
        data["turnResponse"] = settings_.turnResponse;
        data["bobAmplitude"] = settings_.bobAmplitude;
        data["bobSpeed"] = settings_.bobSpeed;
        data["arrivalDistance"] = settings_.arrivalDistance;
        data["arrivalHeight"] = settings_.arrivalHeight;
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream << data.dump(2) << '\n';
            stream.flush();
            // 一時ファイルへの出力を確定できなければ、既存の設定ファイルを置き換えない。
            if (!stream)
                return false;
        }
        // 保存途中のファイルを設定として読ませない。
        return MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    // パスや書き込みの処理で例外が出た場合は、呼び出し側に失敗を伝える。
    } catch (...) {
        return false;
    }
}

#ifdef USE_IMGUI
void DestinationGuide::DrawImGui(const std::string& settingsPath) {
    // 描画中は値を一時コピーへ集め、入力があったフレームだけ案内側へ反映する。
    Settings edited = settings_;
    bool changed = ImGui::Checkbox("矢印を有効にする", &edited.enabled);
    changed |= ImGui::DragFloat("追従距離", &edited.distance, 0.05f, 0.0f, 15.0f);
    changed |= ImGui::DragFloat("足元からの高さ", &edited.height, 0.05f, 0.0f, 10.0f);
    changed |= ImGui::DragFloat("矢印の大きさ", &edited.scale, 0.01f, 0.05f, 5.0f);
    changed |= ImGui::DragFloat("位置の追従速度", &edited.followResponse, 0.2f, 0.1f, 80.0f);
    changed |= ImGui::DragFloat("旋回速度", &edited.turnResponse, 0.2f, 0.1f, 80.0f);
    changed |= ImGui::DragFloat("浮遊の振幅", &edited.bobAmplitude, 0.01f, 0.0f, 1.0f);
    changed |= ImGui::DragFloat("浮遊の速さ", &edited.bobSpeed, 0.05f, 0.0f, 15.0f);
    // 到着の判定では水平距離と高低差を別々に調整できる。
    changed |= ImGui::DragFloat("到着とみなす距離", &edited.arrivalDistance, 0.05f, 0.1f, 20.0f);
    changed |= ImGui::DragFloat("到着時の許容高低差", &edited.arrivalHeight, 0.05f, 0.1f, 20.0f);
    // 項目に入力があったフレームだけ、補正を通してゲーム内の設定値を更新する。
    if (changed)
        SetSettings(edited);
    ImGui::Text("到着状態: %s", arrived_ ? "到着済み・非表示" : "案内待機／案内中");
    // 保存ボタンを押した場合だけ現在値を書き出し、結果を案内欄に表示する。
    if (ImGui::Button("矢印設定を保存"))
        settingsMessage_ = SaveSettings(settingsPath) ? "矢印設定を保存しました。" : "保存に失敗しました。現在値は保持します。";
    ImGui::SameLine();
    // 再読込ボタンを押した場合だけファイルを読み直し、成功か失敗かを表示する。
    if (ImGui::Button("矢印設定を再読込"))
        settingsMessage_ = LoadSettings(settingsPath) ? "矢印設定を読み込みました。" : "読込に失敗しました。現在値は保持します。";
    // 到着で消えた矢印を同じ場面でもう一度確認できるよう、進行状態を初期化する。
    if (ImGui::Button("到着状態をリセット"))
        Reset();
    ImGui::TextWrapped("%s", settingsMessage_.c_str());
    ImGui::TextDisabled("保存先: %s", settingsPath.c_str());
}
#endif