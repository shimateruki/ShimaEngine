#include "engine/animation/cinematic/CinematicAssetPath.h"
#include "EditorPropertyDrawer.h"
#include <numbers>

#ifdef USE_IMGUI
#include "imgui.h"
#include "imgui_internal.h"
#endif

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <filesystem>

namespace {

using json = nlohmann::json;

constexpr float kRadiansToDegrees = (180.0f / std::numbers::pi_v<float>);
constexpr float kDegreesToRadians = (std::numbers::pi_v<float> / 180.0f);

// 数値以外の旧データや未設定値でも編集UIを開けるよう、既定値へ置き換えます。
float ReadNumber(const json& value, float fallback = 0.0f) {
    return value.is_number() ? value.get<float>() : fallback;
}

float ReadArrayNumber(const json& value, std::size_t index, float fallback = 0.0f) {
    // 配列以外・添字が範囲外・数値以外なら、UI用に既定の成分を返す。
    if (!value.is_array() || index >= value.size() || !value[index].is_number()) {
        return fallback;
    }

    return value[index].get<float>();
}

}

// 登録済みの型に合わせて入力 UI を選び、変更があった場合だけ呼び出し元へ通知します。
bool editorPropertyDrawer::DrawValue(
    const EditorPropertyDescriptor& property,
    nlohmann::json& value,
    const char* id,
    const EditorPropertyDrawOptions& options) {
#ifdef USE_IMGUI
    const float width = options.width != 0.0f
        ? options.width
        : (options.compact ? -FLT_MIN : 430.0f);
    ImGui::SetNextItemWidth(width);
    // 複数選択で値が異なるときはImGuiの混合値表示を使い、入力が確定した項目だけ書き戻す。
    ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, options.mixed);

    bool changed = false;
    const EditorPropertyUiHints& ui = property.ui;
    const char* format = ui.format.empty() ? "%.3f" : ui.format.c_str();

    switch (property.type) {
    case EditorPropertyType::kBool: {
        bool current = value.is_boolean() ? value.get<bool>() : false;

        // チェック状態が変わった場合だけ、保存する真偽値を更新する。
        if (ImGui::Checkbox(id, &current)) {
            value = current;
            changed = true;
        }

        break;
    }

    case EditorPropertyType::kInteger: {
        std::int64_t current = value.is_number_unsigned() || value.is_number_integer()
            ? value.get<std::int64_t>()
            : 0;
        // 例として 0 始まりの保存値を画面では 1 始まりにできる。数値入力の確定時に足した分を戻す。
        current += ui.integerDisplayOffset;
        // 整数にラベル付きの候補があれば、選択欄を優先する。
        if (!property.enumLabels.empty()) {
            std::string preview = value.dump();
            // 明示の保存値が全選択肢にそろう場合はそれを使う。従来形式は選択肢の位置を保存値にする。
            const auto choiceValue = [&](std::size_t index) -> json {
                return property.enumValues.size() == property.enumLabels.size()
                    ? property.enumValues[index] : json(index);
            };
            for (std::size_t i = 0; i < property.enumLabels.size(); ++i)
                // 現在の保存値と一致した候補のラベルを表示する。
                if (choiceValue(i) == value) preview = property.enumLabels[i];
            // 整数の選択欄が開いている間だけ候補を列挙する。
            if (ImGui::BeginCombo(id, preview.c_str())) {
                for (std::size_t i = 0; i < property.enumLabels.size(); ++i) {
                    ImGui::PushID(static_cast<std::int32_t>(i));
                    // 選ばれた候補に対応する値をJSONへ格納する。
                    if (ImGui::Selectable(property.enumLabels[i].c_str(), value == choiceValue(i))) {
                        value = choiceValue(i);
                        changed = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
        }
        // 候補が無く、スライダー指定と有効な上下限がある場合は範囲内の整数を操作できるようにする。
        else if (ui.useSlider && ui.maxValue > ui.minValue) {
            // 登録側で範囲を指定した整数も、共通UIだけで調整できます。
            const std::int64_t minimum = static_cast<std::int64_t>(ui.minValue) + ui.integerDisplayOffset;
            const std::int64_t maximum = static_cast<std::int64_t>(ui.maxValue) + ui.integerDisplayOffset;
            // スライダーで変更があったら、表示時の番号差を引いて保存する。
            if (ImGui::SliderScalar(id, ImGuiDataType_S64, &current, &minimum, &maximum, "%lld")) {
                value = current - ui.integerDisplayOffset;
                changed = true;
            }
        }
        // 候補も有効なスライダーも無ければ整数を直接入力し、変更時に保存値を更新する。
        else if (ImGui::InputScalar(id, ImGuiDataType_S64, &current)) {
            value = current - ui.integerDisplayOffset;
            changed = true;
        }

        break;
    }

    case EditorPropertyType::kNumber: {
        float current = ReadNumber(value);

        // 内部はラジアンのまま保持し、Inspectorで角度として見せるときだけ度へ換算する。
        if (ui.displayAsDegrees) {
            current *= kRadiansToDegrees;
        }

        // スライダーが指定されている場合は、上下限を操作範囲に使う。
        if (ui.useSlider) {
            changed = ImGui::SliderFloat(id, &current, ui.minValue, ui.maxValue, format);
        }
        // スライダーの指定が無ければ、変更速度と範囲を数値ドラッグ欄へ渡す。
        else {
            changed = ImGui::DragFloat(id, &current, ui.speed, ui.minValue, ui.maxValue, format);
        }

        // 操作で値が変わった場合だけ、保存する数値を更新する。
        if (changed) {
            // 度で表示した場合は、保存する前にラジアンへ戻す。
            if (ui.displayAsDegrees) {
                current *= kDegreesToRadians;
            }

            value = current;
        }

        break;
    }

    case EditorPropertyType::kString: {
        const std::string current = value.is_string() ? value.get<std::string>() : std::string();
        // 空でない候補の値とラベルが同数ある場合は、文字列候補から選べるようにする。
        if (property.enumLabels.size() == property.enumValues.size() && !property.enumValues.empty()) {
            std::string preview = current.empty() ? "(未設定)" : current;
            for (std::size_t i = 0; i < property.enumValues.size(); ++i)
                // 保存値に対応する候補が見つかったら、そのラベルを表示する。
                if (value == property.enumValues[i]) preview = property.enumLabels[i];
            // 文字列の選択欄が開いている間だけ候補を描画する。
            if (ImGui::BeginCombo(id, preview.c_str())) {
                for (std::size_t i = 0; i < property.enumValues.size(); ++i) {
                    ImGui::PushID(static_cast<std::int32_t>(i));
                    // 候補が選ばれたら、対応する文字列を保存値にする。
                    if (ImGui::Selectable(property.enumLabels[i].c_str(), value == property.enumValues[i])) {
                        value = property.enumValues[i];
                        changed = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
        // 文字列候補が無く素材フォルダーが指定されている場合は、JSON素材の選択欄を使う。
        } else if (!ui.assetDirectory.empty()) {
            namespace fs = std::filesystem;
            const auto utf8 = [](const fs::path& path) {
                const auto bytes = path.generic_u8string();
                return std::string(bytes.begin(), bytes.end());
            };
            const fs::path root(ui.assetDirectory);
            const fs::path stored(std::u8string(current.begin(), current.end()));
            // 演出素材の旧パスも解決したうえで、一覧には素材ルートからの相対パスを出す。
            const auto resolved = ui.assetEditor == EditorPropertyAssetEditor::kScenario
                ? cinematicAssetPath::Resolve(root, stored) : stored;
            const auto preview = current.empty() ? std::string("(なし)") : utf8(resolved.lexically_relative(root));
            // 素材の選択欄が開いている間だけ、指定フォルダーから候補を探す。
            if (ImGui::BeginCombo(id, preview.c_str())) {
                // 「なし」を選んだら、空文字列を指定して参照を解除する。
                if (ImGui::Selectable("(なし)", current.empty())) { value = ""; changed = true; }
                std::vector<fs::path> files;
                std::error_code error;
                for (fs::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
                    // 拡張子.jsonの通常ファイルだけを選択肢へ加える。
                    if (it->is_regular_file(error) && it->path().extension() == ".json") files.push_back(it->path());
                }
                std::sort(files.begin(), files.end());
                for (const auto& file : files) {
                    const auto path = utf8(file);
                    const auto label = utf8(file.lexically_relative(root));
                    // 素材が選ばれたら、そのパスを保存値にする。
                    if (ImGui::Selectable(label.c_str(), resolved == file)) { value = path; changed = true; }
                }
                ImGui::EndCombo();
            }
            std::error_code error;
            // 参照が設定済みでもファイルを確認できない場合は、設定を残したまま警告を出す。
            if (!current.empty() && !fs::is_regular_file(resolved, error))
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.3f, 1.0f), "指定されたJSONが見つかりません");
        // 文字列候補も素材フォルダーも無ければ、文字列を直接入力する。
        } else {
            std::array<char, 512> buffer = {};
            std::snprintf(buffer.data(), buffer.size(), "%s", current.c_str());
            // 文字入力に変更があった場合だけ、入力バッファの内容を保存値へ戻す。
            if (ImGui::InputText(id, buffer.data(), buffer.size())) {
                value = std::string(buffer.data());
                changed = true;
            }
        }

        break;
    }

    case EditorPropertyType::kVector2: {
        float current[2] = { ReadArrayNumber(value, 0), ReadArrayNumber(value, 1) };
        changed = ImGui::DragFloat2(
            id, current, ui.speed, ui.minValue, ui.maxValue, format);
        // 二成分の値が変わったときだけ、JSON配列を更新する。
        if (changed) {
            value = json::array({ current[0], current[1] });
        }

        break;
    }

    case EditorPropertyType::kVector3: {
        // 回転ベクトルも各成分を表示用に換算し、保存時には元の単位へ戻す。
        float current[3] = {
            ReadArrayNumber(value, 0),
            ReadArrayNumber(value, 1),
            ReadArrayNumber(value, 2),
        };

        // 度表示を指定されている場合は、三成分とも度へ変換する。
        if (ui.displayAsDegrees) {
            for (float& component : current) {
                component *= kRadiansToDegrees;
            }
        }

        changed = ImGui::DragFloat3(
            id, current, ui.speed, ui.minValue, ui.maxValue, format);
        // 三成分の入力値が変わったときだけ、JSON配列を更新する。
        if (changed) {
            // 度表示の項目を保存する場合は、各成分をラジアンに戻す。
            if (ui.displayAsDegrees) {
                for (float& component : current) {
                    component *= kDegreesToRadians;
                }
            }

            value = json::array({ current[0], current[1], current[2] });
        }

        break;
    }

    case EditorPropertyType::kVector4: {
        float current[4] = {
            ReadArrayNumber(value, 0),
            ReadArrayNumber(value, 1),
            ReadArrayNumber(value, 2),
            ReadArrayNumber(value, 3, 1.0f),
        };

        // 色指定のときは第 4 成分をアルファとして編集する。欠けた成分は不透明の 1 から始める。
        changed = ui.useColorPicker
            ? ImGui::ColorEdit4(id, current, ImGuiColorEditFlags_Float)
            : ImGui::DragFloat4(id, current, ui.speed, ui.minValue, ui.maxValue, format);
        // 色または四成分のドラッグ入力に変更があったときだけ、JSON配列を更新する。
        if (changed) {
            value = json::array({ current[0], current[1], current[2], current[3] });
        }

        break;
    }
    }

    ImGui::PopItemFlag();
    return changed;
#else
    // ImGuiを使わない構成では、入力を表示せず変更なしを返す。
    (void)property;
    (void)value;
    (void)id;
    (void)options;
    return false;
#endif
}
