#pragma once
#include <cstdint>

// 担当：寿命カーブと色グラデーションの編集・プレビューUIを提供する。
// 関連：VfxFloatCurve / VfxColorGradient（編集値）、各粒子Editor

#include "engine/graphics/particle/VfxAuthoring.h"
#include "engine/editor/properties/EditorPropertyTransaction.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#ifdef USE_IMGUI
#include "imgui.h"

namespace vfxAuthoringEditor {

/// 曲線キーの補間方法を UI に表示する名前へ変換します。
inline const char* GetEasingName(VfxCurveEasing easing) {
    switch (easing) {
    case VfxCurveEasing::kEaseIn: return "Ease In";
    case VfxCurveEasing::kEaseOut: return "Ease Out";
    case VfxCurveEasing::kEaseInOut: return "Ease In Out";
    case VfxCurveEasing::kSmoothStep: return "Smooth Step";
    case VfxCurveEasing::kLinear:
    default: return "Linear";
    }
}

/// 既存キーの最大の空白区間へ新しいキーを入れる時刻を返します。
template <class TKey>
float FindInsertionTime(const std::vector<TKey>& keys) {
    // 空なら先頭の0、一点だけなら終端の1を追加の時刻にする。
    if (keys.size() < 2) {
        return keys.empty() ? 0.0f : 1.0f;
    }

    // 時刻は寿命に対する 0～1 の割合。既存の時刻順を前提に、隣り合うキー間が最も広い区間の中央を選ぶ。
    float insertionTime = 0.5f;
    float largestGap = -1.0f;
    for (std::size_t index = 1; index < keys.size(); ++index) {
        const float gap = keys[index].time - keys[index - 1].time;
        // それまでより広い隣接区間を見つけたら、その中点へ追加位置を更新する。
        if (gap > largestGap) {
            largestGap = gap;
            insertionTime = (keys[index - 1].time + keys[index].time) * 0.5f;
        }
    }
    return std::clamp(insertionTime, 0.001f, 0.999f);
}

/// 曲線を編集し、変更ごとに apply と Undo 記録を同期します。
inline bool DrawFloatCurve(
    const char* id,
    VfxFloatCurve& curve,
    float minimumValue,
    float maximumValue,
    std::int32_t maximumKeys,
    const std::function<void(const VfxFloatCurve&)>& apply,
    bool allowKeyCountChange = true,
    bool allowPerKeyEasing = true) {
    bool changed = false;
    ImGui::PushID(id);

    // 見た目の曲線は現在のキーから毎フレーム評価する。描画用配列は保存値へ戻さない。
    std::vector<float> preview(64);
    for (std::size_t index = 0; index < preview.size(); ++index) {
        preview[index] = curve.Evaluate(
            static_cast<float>(index) / static_cast<float>(preview.size() - 1),
            1.0f);
    }
    ImGui::PlotLines(
        "##Preview",
        preview.data(),
        static_cast<std::int32_t>(preview.size()),
        0,
        nullptr,
        minimumValue,
        maximumValue,
        ImVec2(ImGui::GetContentRegionAvail().x, 90.0f));

    for (std::size_t index = 0; index < curve.keys.size(); ++index) {
        ImGui::PushID(static_cast<std::int32_t>(index));
        ImGui::TextDisabled("Key %d", static_cast<std::int32_t>(index + 1));
        ImGui::SameLine();

        VfxFloatCurve before = curve;
        ImGui::SetNextItemWidth(82.0f);
        bool timeChanged = ImGui::DragFloat("Time", &curve.keys[index].time, 0.005f, 0.0f, 1.0f, "%.3f");

        // 最初のキーは寿命の開始を支えるため時刻0に戻す。
        if (index == 0)
            curve.keys[index].time = 0.0f;
        // 最後のキーは寿命の終点を支えるため時刻1に戻す。
        if (index + 1 == curve.keys.size())
            curve.keys[index].time = 1.0f;
        // 中間キーは、前後のキーから0.001ずつ空けた範囲内に収める。
        if (index > 0 && index + 1 < curve.keys.size()) {
            curve.keys[index].time = std::clamp(
                curve.keys[index].time,
                curve.keys[index - 1].time + 0.001f,
                curve.keys[index + 1].time - 0.001f);
        }
        // 時刻入力に変更があった場合は、制約を掛けた値を適用する。
        if (timeChanged) {
            apply(curve);
            changed = true;
        }

        // 連続ドラッグは一つのUndo操作として追跡する。
        editorPropertyTransaction::TrackLastItem(
            std::string(id) + ": Key Time",
            before,
            curve,
            apply);

        ImGui::SameLine();
        before = curve;
        ImGui::SetNextItemWidth(92.0f);
        // 数値入力を変更した場合だけ、値を許容範囲に収めてカーブへ適用する。
        if (ImGui::DragFloat("Value", &curve.keys[index].value, 0.01f, minimumValue, maximumValue, "%.3f")) {
            curve.keys[index].value = std::clamp(curve.keys[index].value, minimumValue, maximumValue);
            apply(curve);
            changed = true;
        }
        editorPropertyTransaction::TrackLastItem(
            std::string(id) + ": Key Value",
            before,
            curve,
            apply);

        // 補間編集を許可していて次のキーがある場合だけ、区間の補間入力欄を表示する。
        if (allowPerKeyEasing && index + 1 < curve.keys.size()) {
            ImGui::SameLine();
            std::int32_t easing = static_cast<std::int32_t>(curve.keys[index].easing);
            ImGui::SetNextItemWidth(105.0f);
            before = curve;
            // 補間方式を選び直した場合は、キーへの適用と履歴登録を同時に行う。
            if (ImGui::Combo("Easing", &easing, "Linear\0Ease In\0Ease Out\0Ease In Out\0Smooth Step\0")) {
                curve.keys[index].easing = static_cast<VfxCurveEasing>(easing);
                apply(curve);
                editorPropertyTransaction::RegisterDiscrete(
                    std::string(id) + ": Easing",
                    before,
                    curve,
                    apply);
                changed = true;
            }
        }

        // 個数を変えてよい場合でも、開始・終了を支える二端を削除候補から外す。
        if (allowKeyCountChange && curve.keys.size() > 2 && index > 0 && index + 1 < curve.keys.size()) {
            ImGui::SameLine();
            before = curve;
            if (ImGui::SmallButton("削除")) {
                // erase後の参照と添字を使わないよう、このフレームのキー走査を終える。
                curve.keys.erase(curve.keys.begin() + static_cast<std::ptrdiff_t>(index));
                apply(curve);
                editorPropertyTransaction::RegisterDiscrete(
                    std::string(id) + ": Delete Key",
                    before,
                    curve,
                    apply);
                changed = true;
                ImGui::PopID();
                break;
            }
        }
        ImGui::PopID();
    }

    if (allowKeyCountChange && static_cast<std::int32_t>(curve.keys.size()) < maximumKeys && ImGui::Button("キーを追加")) {
        // 最も広い空白区間へ追加し、Normalizeで時間順を再確定する。
        const VfxFloatCurve before = curve;
        const float time = FindInsertionTime(curve.keys);
        // 追加時の値はその時刻における既存カーブの値を使う。既定値を置く場合より変化を抑えやすい。
        curve.keys.push_back({ time, curve.Evaluate(time, 1.0f), VfxCurveEasing::kLinear });
        curve.Normalize();
        apply(curve);
        editorPropertyTransaction::RegisterDiscrete(
            std::string(id) + ": Add Key",
            before,
            curve,
            apply);
        changed = true;
    }
    ImGui::PopID();
    return changed;
}

/// 色のグラデーションキーを編集し、変更を Undo 可能な単位で適用します。
inline bool DrawColorGradient(
    const char* id,
    VfxColorGradient& gradient,
    std::int32_t maximumKeys,
    const std::function<void(const VfxColorGradient&)>& apply,
    bool allowKeyCountChange = true) {
    bool changed = false;
    ImGui::PushID(id);

    // 曲線と同じ時刻制約を色キーにも適用し、色のドラッグをUndoへまとめる。
    for (std::size_t index = 0; index < gradient.keys.size(); ++index) {
        ImGui::PushID(static_cast<std::int32_t>(index));
        ImGui::TextDisabled("Color %d", static_cast<std::int32_t>(index + 1));
        ImGui::SameLine();

        VfxColorGradient before = gradient;
        ImGui::SetNextItemWidth(82.0f);
        bool timeChanged = ImGui::DragFloat("Time", &gradient.keys[index].time, 0.005f, 0.0f, 1.0f, "%.3f");
        // 色の最初のキーは寿命の開始を支えるため時刻0に戻す。
        if (index == 0)
            gradient.keys[index].time = 0.0f;
        // 色の最後のキーは寿命の終点を支えるため時刻1に戻す。
        if (index + 1 == gradient.keys.size())
            gradient.keys[index].time = 1.0f;
        // 中間色の時刻も、隣の色と0.001の間隔を保つ範囲で動かす。
        if (index > 0 && index + 1 < gradient.keys.size()) {
            gradient.keys[index].time = std::clamp(
                gradient.keys[index].time,
                gradient.keys[index - 1].time + 0.001f,
                gradient.keys[index + 1].time - 0.001f);
        }
        // 時刻の入力で変更があった場合だけ、新しい時刻を色の設定へ反映する。
        if (timeChanged) {
            apply(gradient);
            changed = true;
        }
        editorPropertyTransaction::TrackLastItem(
            std::string(id) + ": Color Time",
            before,
            gradient,
            apply);

        ImGui::SameLine();
        before = gradient;
        // RGBAの入力が変更されたときに、新しい色の値を適用する。
        if (ImGui::ColorEdit4("Color", &gradient.keys[index].color.x, ImGuiColorEditFlags_Float)) {
            apply(gradient);
            changed = true;
        }
        editorPropertyTransaction::TrackLastItem(
            std::string(id) + ": Color",
            before,
            gradient,
            apply);

        // 色の個数を変更できる場合でも、開始・終了の端点を削除候補に含めない。
        if (allowKeyCountChange && gradient.keys.size() > 2 && index > 0 && index + 1 < gradient.keys.size()) {
            ImGui::SameLine();
            before = gradient;
            if (ImGui::SmallButton("削除")) {
                // 端点は保持し、中間色だけ削除可能にする。
                gradient.keys.erase(gradient.keys.begin() + static_cast<std::ptrdiff_t>(index));
                apply(gradient);
                editorPropertyTransaction::RegisterDiscrete(
                    std::string(id) + ": Delete Color",
                    before,
                    gradient,
                    apply);
                changed = true;
                ImGui::PopID();
                break;
            }
        }
        ImGui::PopID();
    }

    // 色の追加が許可され、個数が上限未満でボタンが押された場合だけ、新しい色キーを作る。
    if (allowKeyCountChange && static_cast<std::int32_t>(gradient.keys.size()) < maximumKeys && ImGui::Button("色キーを追加")) {
        const VfxColorGradient before = gradient;
        const float time = FindInsertionTime(gradient.keys);
        // 新しい色の初期値に既存グラデーションの補間色を使い、追加した点の色を引き継ぐ。
        gradient.keys.push_back({ time, gradient.Evaluate(time), VfxCurveEasing::kLinear });
        gradient.Normalize();
        apply(gradient);
        editorPropertyTransaction::RegisterDiscrete(
            std::string(id) + ": Add Color",
            before,
            gradient,
            apply);
        changed = true;
    }
    ImGui::PopID();
    return changed;
}

/// 距離に応じた VFX の品質設定を編集します。
inline bool DrawLodSettings(
    const char* id,
    VfxLodSettings& settings,
    const std::function<void(const VfxLodSettings&)>& apply,
    std::int32_t minimumParticleLimit = 1,
    std::int32_t maximumParticleLimit = 262144) {
    bool changed = false;
    ImGui::PushID(id);

    // 有効化と数値ドラッグを別のUndo単位にし、各変更後に値の整合性を戻す。
    VfxLodSettings before = settings;
    // 距離LODの有効状態を変更した場合は、値を補正して独立したUndo操作として記録する。
    if (ImGui::Checkbox("距離LODを有効化", &settings.enabled)) {
        settings.Sanitize();
        apply(settings);
        editorPropertyTransaction::RegisterDiscrete(
            std::string(id) + ": Enable LOD",
            before,
            settings,
            apply);
        changed = true;
    }

    ImGui::BeginDisabled(!settings.enabled);
    before = settings;
    // 近距離を変えた場合だけ、遠距離と順序が矛盾しない値へ補正して適用する。
    if (ImGui::DragFloat("近距離", &settings.nearDistance, 0.25f, 0.0f, 1000.0f, "%.2f")) {
        settings.Sanitize();
        apply(settings);
        changed = true;
    }
    editorPropertyTransaction::TrackLastItem(std::string(id) + ": Near Distance", before, settings, apply);

    before = settings;
    // 遠距離を変えた場合だけ、近距離との順序を補正して適用する。
    if (ImGui::DragFloat("遠距離", &settings.farDistance, 0.25f, 0.01f, 2000.0f, "%.2f")) {
        settings.Sanitize();
        apply(settings);
        changed = true;
    }
    editorPropertyTransaction::TrackLastItem(std::string(id) + ": Far Distance", before, settings, apply);

    before = settings;
    // 遠距離の発生倍率が変わった場合に、補正した新しい値を適用する。
    if (ImGui::SliderFloat("遠距離の発生率", &settings.farEmissionScale, 0.0f, 1.0f, "%.2f")) {
        settings.Sanitize();
        apply(settings);
        changed = true;
    }
    editorPropertyTransaction::TrackLastItem(std::string(id) + ": Far Emission", before, settings, apply);

    before = settings;
    // 同時生存上限を変えた場合だけ、値を有効な範囲に直して適用する。
    if (ImGui::DragInt("同時生存上限 (0 = System上限)", &settings.maxAliveParticles, 8.0f, 0, maximumParticleLimit)) {
        settings.Sanitize();
        // 0 は System 自身の上限を使う指定として残す。明示した正の値だけツールが許す範囲へ補正する。
        if (settings.maxAliveParticles > 0) {
            settings.maxAliveParticles = std::clamp(
                settings.maxAliveParticles, (std::max)(minimumParticleLimit, 1), maximumParticleLimit);
        }
        apply(settings);
        changed = true;
    }
    editorPropertyTransaction::TrackLastItem(std::string(id) + ": Max Alive", before, settings, apply);
    ImGui::EndDisabled();

    ImGui::PopID();
    return changed;
}

} // namespace vfxAuthoringEditor
#endif
