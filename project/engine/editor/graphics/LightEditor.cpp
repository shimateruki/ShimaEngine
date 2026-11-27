#include "LightEditor.h"
#include <cstdint>
#include <numbers>
#include "imgui.h"
#include "EditorToolSettings.h"
#include "json.hpp"
#include <fstream>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include "DirectXCommon.h"
#include "ModelManager.h"
#include "imgui_internal.h"
#include "CameraManager.h"
#include "Camera.h"
#include "SceneManager.h" 
#include "IconsFontAwesome5.h"
#include "DebugConsole.h"
using json = nlohmann::json;

#ifdef USE_IMGUI
static ImVec2 WorldToScreen(const engine::Vector3& worldPos, const engine::Matrix4x4& mat) {
    // 背面の点は画面外へ退避し、透視除算後にImGuiの左上原点へ変換する。
    float w = worldPos.x * mat.m[0][3] + worldPos.y * mat.m[1][3] + worldPos.z * mat.m[2][3] + mat.m[3][3];
    // 投影のwが小さいか負なら、透視除算を避けて画面外マーカーを返す。
    if (w < 0.001f)
        return ImVec2(-10000.0f, -10000.0f);

    float x = (worldPos.x * mat.m[0][0] + worldPos.y * mat.m[1][0] + worldPos.z * mat.m[2][0] + mat.m[3][0]) / w;
    float y = (worldPos.x * mat.m[0][1] + worldPos.y * mat.m[1][1] + worldPos.z * mat.m[2][1] + mat.m[3][1]) / w;

    ImGuiIO& io = ImGui::GetIO();
    float screenX = (x + 1.0f) * 0.5f * io.DisplaySize.x;
    float screenY = (1.0f - y) * 0.5f * io.DisplaySize.y;
    return ImVec2(screenX, screenY);
}
#endif

LightEditor* LightEditor::GetInstance() {
    // Sceneをまたぐ照明編集状態を一つのEditorへ集約します。
    static LightEditor sInstance;
    return &sInstance;
}

void LightEditor::SetStatusMessage(const std::string& message, bool success) {
    // 操作結果を一定時間だけ表示し、ImGuiがない構成では期限を持たせません。
    statusMessage_ = message;
    statusSuccess_ = success;
#ifdef USE_IMGUI
    statusVisibleUntil_ = ImGui::GetTime() + 3.0;
#else
    statusVisibleUntil_ = 0.0;
#endif
}

std::string LightEditor::BuildFullPathFromFileName() const {
    // 保存名を照明設定用JSONディレクトリに固定します。
    return "Resources/json/light/" + std::string(currentFileName_);
}

void LightEditor::SyncCurrentFileNameFromManager() {
    // 実行側で切り替わった照明ファイル名をUIへ取り込みます。
    // 照明の管理先を取得していなければ、ファイル名の同期は行わない。
    if (!lightManager_) {
        return;
    }

    const std::string& currentPath = lightManager_->GetCurrentStateFile();

    // 適用先が未設定、または前回の同期から同じ場合は、入力中のファイル名を維持する。
    if (currentPath.empty() || currentPath == syncedLightPath_) {
        return;
    }

    std::filesystem::path path(currentPath);
    std::string fileName = path.filename().string();

    // パスから末尾の名前を得られないときは、受け取った文字列をそのまま使う。
    if (fileName.empty()) {
        fileName = currentPath;
    }

    strncpy_s(currentFileName_, sizeof(currentFileName_), fileName.c_str(), _TRUNCATE);
    syncedLightPath_ = currentPath;
}

void LightEditor::DrawLightFileList() {
#ifdef USE_IMGUI
    // 現在適用中のファイルと保存済み候補を並べ、読込結果も同じ場所に示します。
    SyncCurrentFileNameFromManager();

    const std::string currentPath = lightManager_ ? lightManager_->GetCurrentStateFile() : "";
    ImGui::Text(ICON_FA_FOLDER_OPEN " 現在のライト設定");
    ImGui::SameLine();
    ImGui::TextColored(
        lightManager_ && lightManager_->WasLastLoadSuccessful() ? ImVec4(0.45f, 1.0f, 0.65f, 1.0f) : ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
        "%s",
        currentPath.empty() ? "(未設定)" : currentPath.c_str()
    );

    // 直近の読込が成功していない場合は、画面に保存による作成の案内を表示する。
    if (lightManager_ && !lightManager_->WasLastLoadSuccessful()) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), ICON_FA_EXCLAMATION_TRIANGLE " ファイルが未作成です。保存するとこの名前で作成されます。");
    }

    std::vector<std::string> fileNames;
    const std::filesystem::path lightDir("Resources/json/light");

    // 照明JSONの格納先が存在する場合だけ候補を走査する。
    if (std::filesystem::exists(lightDir)) {
        for (const auto& entry : std::filesystem::directory_iterator(lightDir)) {
            // 通常ファイルかつ小文字の.json拡張子を持つ候補だけ一覧へ加える。
            if (!entry.is_regular_file() || entry.path().extension() != ".json") {
                continue;
            }

            fileNames.push_back(entry.path().filename().string());
        }
    }

    std::sort(fileNames.begin(), fileNames.end());

    ImGui::BeginChild("##LightFileList", ImVec2(0.0f, 92.0f), true);

    // 候補が見つからない場合は空の一覧にその旨を出す。
    if (fileNames.empty()) {
        ImGui::TextDisabled("ライト設定ファイルがありません");
    }

    for (const auto& name : fileNames) {
        const bool selected = (name == currentFileName_);

        // 一覧の選択は入出力先の名前だけを変更し、照明への適用はLoadボタンで行う。
        if (ImGui::Selectable(name.c_str(), selected)) {
            strncpy_s(currentFileName_, sizeof(currentFileName_), name.c_str(), _TRUNCATE);
            syncedLightPath_ = currentPath;
        }
    }

    ImGui::EndChild();
#endif
}

void LightEditor::DrawSkyboxTextureList() {
#ifdef USE_IMGUI
    // Resources内のテクスチャだけを候補にし、参照できるSkyboxを選ばせます。
    // 照明管理が未設定なら、現在のSkyboxを取得できないため編集欄を出さない。
    if (!lightManager_) {
        return;
    }

    std::vector<std::string> texturePaths;
    const std::filesystem::path resourceDir("Resources");

    // Resourcesが存在する場合に限り、その配下から候補を集める。
    if (std::filesystem::exists(resourceDir)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(resourceDir)) {
            // DDS形式の通常ファイルだけを候補へ加える。
            if (!entry.is_regular_file() || entry.path().extension() != ".dds") {
                continue;
            }

            texturePaths.push_back(entry.path().generic_string());
        }
    }

    std::sort(texturePaths.begin(), texturePaths.end());

    bool enabled = lightManager_->IsSkyboxEnabled();

    // チェック状態が変わったときにだけ、表示設定を管理側へ渡す。
    if (ImGui::Checkbox(ICON_FA_CLOUD_SUN " スカイボックスを表示", &enabled)) {
        lightManager_->SetSkyboxEnabled(enabled);
    }

    const std::string& currentTexture = lightManager_->GetSkyboxTexturePath();

    // DDSの選択欄を開いている間だけ、候補と現在の選択を表示する。
    if (ImGui::BeginCombo(ICON_FA_IMAGE " スカイボックスDDS", currentTexture.c_str())) {
        for (const auto& path : texturePaths) {
            const bool selected = (path == currentTexture);

            // 画像を選んだ場合はその場で切り替えを試み、成功可否を表示に残す。
            if (ImGui::Selectable(path.c_str(), selected)) {
                const bool applied = lightManager_->SetSkyboxTexturePath(path);
                SetStatusMessage(applied ? "スカイボックスを差し替えました" : "スカイボックスの差し替えに失敗しました", applied);
            }

            // 現在適用されているパスに、一覧内の初期フォーカスを置く。
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }

    ImGui::TextDisabled("ライト設定JSONにON/OFFとDDSパスを保存します。");
#endif
}

void LightEditor::Initialize() {
    // 実際の照明状態を持つManagerへ編集UIを接続します。
    lightManager_ = engine::LightManager::GetInstance();
}

void LightEditor::SetObject3DCommon(engine::Object3DCommon* common) {
    // 描画基盤の切り替えで旧ギズモを残さないよう、表示オブジェクトを作り直します。
    common_ = common;
    pointLightGizmos_.clear();
    spotLightGizmos_.clear();
}

// ライトの座標と表示色を、調整用の3Dギズモに毎フレーム反映する。
void LightEditor::Update() {
    // 照明管理と描画基盤が揃わなければ補助形状を生成・更新できない。
    if (!lightManager_ || !common_)
        return;

    // 点光源と同じ順序・件数になるよう、球の目印を同期する。
    auto& pointLights = lightManager_->GetPointLights();

    // ライトと同じ添字で表示物を対応付け、追加・削除された分だけ表示物の配列を更新する。
    if (pointLightGizmos_.size() != pointLights.size()) {
        pointLightGizmos_.resize(pointLights.size());

        for (size_t i = 0; i < pointLightGizmos_.size(); ++i) {
            // 件数が変わった後に実体がない枠だけ、新しい球で補う。
            if (!pointLightGizmos_[i]) {
                pointLightGizmos_[i] = std::make_unique<engine::Object3D>();
                pointLightGizmos_[i]->Initialize(common_);
                pointLightGizmos_[i]->SetModel("Primitives/sphere");
                pointLightGizmos_[i]->SetMaterialType(0);
            }
        }
    }

    // 目印の座標と色を現在の点光源から取得する。
    for (size_t i = 0; i < pointLights.size(); ++i) {
        // 対応する目印が作成済みの場合にだけ姿勢と色を更新する。
        if (pointLightGizmos_[i]) {

            pointLightGizmos_[i]->SetTranslate(pointLights[i].data.position);
            pointLightGizmos_[i]->SetScale({ gizmoScale_, gizmoScale_, gizmoScale_ });

            // 暗いライトでも位置を識別できるよう、ギズモの表示色だけに明るさの下限を設ける。
            engine::Vector4 color = pointLights[i].data.color;
            color.x = color.x < 0.35f ? 0.35f : color.x;
            color.y = color.y < 0.35f ? 0.35f : color.y;
            color.z = color.z < 0.1f ? 0.1f : color.z;
            color.w = 0.9f;
            pointLightGizmos_[i]->SetColor(color);
            pointLightGizmos_[i]->SetEmissive(2.5f);

            pointLightGizmos_[i]->Update(0.0f);
            pointLightGizmos_[i]->UpdateLocalMatrix();
            pointLightGizmos_[i]->UpdateWorldMatrix();
        }
    }

    // スポットライト側の目印も同じ順序・件数に合わせる。
    auto& spotLights = lightManager_->GetSpotLights();

    // 点光源と別の配列へ同期する。位置の目印は縦長にして見分けられるようにする。
    if (spotLightGizmos_.size() != spotLights.size()) {
        spotLightGizmos_.resize(spotLights.size());

        for (size_t i = 0; i < spotLightGizmos_.size(); ++i) {
            // 配列の空いた枠にだけ、スポットライト用の球を作る。
            if (!spotLightGizmos_[i]) {
                spotLightGizmos_[i] = std::make_unique<engine::Object3D>();
                spotLightGizmos_[i]->Initialize(common_);
                spotLightGizmos_[i]->SetModel("Primitives/sphere");
                spotLightGizmos_[i]->SetMaterialType(0);
            }
        }
    }

    for (size_t i = 0; i < spotLights.size(); ++i) {
        // 対応するスポットライトの表示物を持つ枠だけ更新する。
        if (spotLightGizmos_[i]) {
            // スポットライト位置をギズモに反映
            spotLightGizmos_[i]->SetTranslate(spotLights[i].data.position);
            spotLightGizmos_[i]->SetScale({ gizmoScale_ * 0.85f, gizmoScale_ * 1.35f, gizmoScale_ * 0.85f });
            engine::Vector4 color = spotLights[i].data.color;
            color.x = color.x < 0.15f ? 0.15f : color.x;
            color.y = color.y < 0.55f ? 0.55f : color.y;
            color.z = color.z < 0.85f ? 0.85f : color.z;
            color.w = 0.9f;
            spotLightGizmos_[i]->SetColor(color);
            spotLightGizmos_[i]->SetEmissive(2.5f);

            // 球は照射角や向きの表示を兼ねないため、縦長のまま位置と色を更新する。
            spotLightGizmos_[i]->Update(0.0f);
            spotLightGizmos_[i]->UpdateLocalMatrix();
            spotLightGizmos_[i]->UpdateWorldMatrix();
        }
    }
}

void LightEditor::Draw3D() {
#ifdef USE_IMGUI
    // Editorの可視設定とギズモ設定が両方有効なときだけ補助形状を描画します。
    if (!EditorToolSettings::Get().IsVisible("light"))
        return;
#endif
    // 補助形状を非表示にしているか照明を参照できない場合は描かない。
    if (!isVisibleGizmos_ || !lightManager_)
        return;

    ID3D12Resource* plRes = lightManager_->GetPointLightResource();
    ID3D12Resource* slRes = lightManager_->GetSpotLightResource();

    for (auto& gizmo : pointLightGizmos_) {
        // 点光源の目印は生成済みの分だけ描画する。
        if (gizmo)
            gizmo->Draw(plRes, slRes);
    }

    for (auto& gizmo : spotLightGizmos_) {
        // スポットライトの目印も生成済みの分だけ描画する。
        if (gizmo)
            gizmo->Draw(plRes, slRes);
    }
}

void LightEditor::DrawEnvironmentProfilePanel() {
#ifdef USE_IMGUI
    // 照明・空・PostEffectの組を一つのEnvironment Profileとして編集します。
    // 設定を適用する照明管理がなければパネルは使えない。
    if (!lightManager_) {
        return;
    }

    ImGui::Separator();
    ImGui::Text(ICON_FA_PALETTE " Environment Profile");
    ImGui::TextWrapped(
        "太陽光、環境光、フォグ、背景、影、Bloom、Tone Mapping、色調補正をまとめて保存します。");

    const std::filesystem::path directory("Resources/json/environment");
    std::vector<std::string> fileNames;
    std::error_code error;

    // 環境プロファイルの保存先を確認できた場合にだけ、候補を列挙する。
    if (std::filesystem::exists(directory, error) && !error) {
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            // 通常のJSONファイルだけを書込・適用先の候補にする。
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                fileNames.push_back(entry.path().filename().string());
            }
        }
    }

    std::sort(fileNames.begin(), fileNames.end());

    // 選択欄が開いている間だけ、保存済みプロファイルを並べる。
    if (ImGui::BeginCombo(
        ICON_FA_FOLDER_OPEN " Profile",
        environmentProfileFileName_)) {
        for (const std::string& fileName : fileNames) {
            const bool selected = fileName == environmentProfileFileName_;

            // 選んだ候補名をバッファに入れ、次の保存または適用の対象にする。
            if (ImGui::Selectable(fileName.c_str(), selected)) {
                strncpy_s(
                    environmentProfileFileName_,
                    sizeof(environmentProfileFileName_),
                    fileName.c_str(),
                    _TRUNCATE);
            }

            // 現在入力されている名前と一致する候補に既定のフォーカスを置く。
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }

    ImGui::InputText(
        "Profileファイル名",
        environmentProfileFileName_,
        sizeof(environmentProfileFileName_));
    ImGui::DragFloat(
        "切替時間",
        &environmentProfileBlendDuration_,
        0.05f,
        0.0f,
        10.0f,
        "%.2f s");
    environmentProfileBlendDuration_ =
        (std::max)(0.0f, environmentProfileBlendDuration_);

    std::filesystem::path profilePath =
        directory / std::filesystem::path(environmentProfileFileName_);
    // ファイル名に拡張子がなければ、保存形式の.jsonを補う。
    if (profilePath.extension().empty()) {
        profilePath.replace_extension(".json");
    }

    // Profileは一覧で選ぶだけでは適用せず、保存または切替ボタンの操作先として使う。
    const std::string profilePathString = profilePath.generic_string();

    // 保存ボタンでは現在効いている環境を、選択または入力した名前で書き出す。
    if (ImGui::Button(ICON_FA_SAVE " 現在の環境を保存")) {
        const bool saved =
            lightManager_->SaveEnvironmentProfile(profilePathString);
        SetStatusMessage(
            saved ? "Environment Profileを保存しました"
                  : "Environment Profileの保存に失敗しました",
            saved);
        DebugConsole::GetInstance()->AddLog(
            saved ? "Environment Profile saved: " + profilePathString
                  : "Environment Profile save failed: " + profilePathString);
    }

    ImGui::SameLine();

    // 選択した環境へ、現在の環境から指定秒数で補間する要求を送る。進捗は Manager から取得する。
    if (ImGui::Button(ICON_FA_PLAY " Profileを適用")) {
        const bool applied = lightManager_->ApplyEnvironmentProfile(
            profilePathString,
            environmentProfileBlendDuration_);
        SetStatusMessage(
            applied ? "Environment Profileを適用しました"
                    : "Environment Profileの読込に失敗しました",
            applied);
    }

    // 管理側が環境を補間している間だけ、切替の進捗を表示する。
    if (lightManager_->IsEnvironmentProfileBlending()) {
        const float progress =
            lightManager_->GetEnvironmentProfileBlendProgress();
        ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f), "切替中");
    }

    const std::string& currentProfile =
        lightManager_->GetCurrentEnvironmentProfileFile();
    // 現在のプロファイルが特定できるときだけ適用中のパスを表示する。
    if (!currentProfile.empty()) {
        ImGui::TextDisabled("現在: %s", currentProfile.c_str());
    }
#endif
}

// 照明・背景・影の設定を編集し、保存や再適用を照明管理へ要求する。
void LightEditor::DrawImGui() {
#ifdef USE_IMGUI
    // 照明管理が利用できない間は編集欄を描かない。
    if (!lightManager_)
        return;

    ImGui::Checkbox(ICON_FA_EYE " ライト位置を表示 (Gizmos)", &isVisibleGizmos_);
    ImGui::TextWrapped("読込ライト: %s", lightManager_->GetCurrentStateFile().c_str());
    ImGui::Text("読込結果: %s", lightManager_->WasLastLoadSuccessful() ? "成功" : "未読込 / 失敗");

    // 設定値を変更せず、実際に使用中の光を確認する。
    const auto& comparisonSun = lightManager_->GetDirectionalLight();
    ImGui::Text("実効太陽設定: 強度 %.2f / 方向 %.2f, %.2f, %.2f",
        comparisonSun.intensity, comparisonSun.direction.x, comparisonSun.direction.y, comparisonSun.direction.z);
    ImGui::Text("環境光 RGB: %.2f, %.2f, %.2f", comparisonSun.ambientColor.x,
        comparisonSun.ambientColor.y, comparisonSun.ambientColor.z);
    ImGui::DragFloat(ICON_FA_EXPAND_ARROWS_ALT " ギズモサイズ", &gizmoScale_, 0.01f, 0.2f, 2.0f);
    ImGui::Separator();
    ImGui::Spacing();

    engine::Vector4& clearColor = lightManager_->GetSceneClearColor();

    // 背景色はManagerの保存値に加えて、実際の画面クリアに使う値も同時に更新する。
    if (ImGui::ColorEdit4(ICON_FA_PALETTE " 背景色 (Clear Color)", &clearColor.x)) {
        engine::DirectXCommon::GetInstance()->SetRenderClearColor(clearColor.x, clearColor.y, clearColor.z, clearColor.w);
    }

    ImGui::Spacing();

    // 環境欄を開いている間は、Skyboxと環境プロファイルの編集を表示する。
    if (ImGui::CollapsingHeader(ICON_FA_CLOUD_SUN " 環境設定 (Environment)", ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawSkyboxTextureList();
        DrawEnvironmentProfilePanel();
    }

    ImGui::Spacing();

    // ファイル管理欄が開いている間だけ、候補の選択と照明JSONの保存・読込を受け付ける。
    if (ImGui::CollapsingHeader(ICON_FA_SAVE " ファイル管理 (File I/O)", ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawLightFileList();
        ImGui::Spacing();

        ImGui::InputText(ICON_FA_FILE_CODE " ファイル名 (.json)", currentFileName_, sizeof(currentFileName_));
        std::string fullPath = BuildFullPathFromFileName();

        // 保存ボタンで入力パスへ現在の照明を書き、結果と操作ログを更新する。
        if (ImGui::Button(ICON_FA_DOWNLOAD " セーブ (Save)")) {
            const bool saved = lightManager_->SaveState(fullPath);
            syncedLightPath_.clear();
            SetStatusMessage(saved ? "ライト設定を保存しました" : "ライト設定の保存に失敗しました", saved);
            DebugConsole::GetInstance()->AddLog(saved ? "Light Editor: saved " + fullPath : "Light Editor: save failed " + fullPath);
        }

        ImGui::SameLine();

        // 読み込みを実行した後で同期済みパスを破棄し、次の描画で実際の適用先を取り直す。
        if (ImGui::Button(ICON_FA_UPLOAD " ロード (Load)")) {
            const bool loaded = lightManager_->LoadState(fullPath);
            syncedLightPath_.clear();
            SetStatusMessage(loaded ? "ライト設定をロードしました" : "ライト設定のロードに失敗しました", loaded);
            DebugConsole::GetInstance()->AddLog(loaded ? "Light Editor: loaded " + fullPath : "Light Editor: load failed " + fullPath);
        }

        ImGui::TextDisabled("ターゲットパス: %s", fullPath.c_str());

        // 結果文があり表示期限内の場合に、成功や失敗を色付きで示す。
        if (!statusMessage_.empty() && ImGui::GetTime() < statusVisibleUntil_) {
            const ImVec4 statusColor = statusSuccess_
                ? ImVec4(0.35f, 1.0f, 0.55f, 1.0f)
                : ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
            ImGui::TextColored(statusColor, "%s", statusMessage_.c_str());
        }
    }

    ImGui::Separator();

    // 太陽設定欄が開いていれば、平行光・環境光・影とフォグの調整を表示する。
    if (ImGui::CollapsingHeader(ICON_FA_SUN " 太陽設定 (Directional Light)", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto& sun = lightManager_->GetDirectionalLight();

        ImGui::Text(ICON_FA_LOCATION_ARROW " 光の向き");
        ImGui::DragFloat3("##Dir", &sun.direction.x, 0.01f, -1.0f, 1.0f);

        ImGui::ColorEdit4(ICON_FA_PALETTE " 光の色", &sun.color.x);
        ImGui::DragFloat(ICON_FA_SUN " 輝度", &sun.intensity, 0.01f, 0.0f, 5.0f);

        ImGui::Separator();
        ImGui::ColorEdit3(ICON_FA_CLOUD " 環境光 (Ambient)", &sun.ambientColor.x);

        // 明るい影のプリセットを選んだ場合は、光・霧・背景色をまとめて置き換える。
        if (ImGui::Button(ICON_FA_MAGIC " 明るい影プリセット")) {
            sun.color = { 1.0f, 0.96f, 0.88f, 1.0f };
            sun.direction = { -0.35f, -0.82f, 0.45f };
            sun.intensity = 0.86f;
            sun.ambientColor = { 0.34f, 0.38f, 0.42f };
            sun.fogColor = { 0.66f, 0.76f, 0.86f };
            clearColor = { 0.52f, 0.68f, 0.84f, 1.0f };
            engine::DirectXCommon::GetInstance()->SetRenderClearColor(clearColor.x, clearColor.y, clearColor.z, clearColor.w);
            SetStatusMessage("明るい影プリセットを適用しました", true);
        }

        ImGui::Separator();
        ImGui::Text(ICON_FA_ADJUST " シャドウ品質");
        // Comboの選択番号を実際の解像度へ変換し、影リソースの変更はManagerへ任せる。
        constexpr std::int32_t kShadowResolutions[] = { 1024, 2048, 4096 };
        const char* shadowResolutionLabels[] = {
            "1024 x 1024 (軽量)",
            "2048 x 2048 (標準)",
            "4096 x 4096 (高精細)",
        };

        std::int32_t shadowResolutionIndex = 1;
        const std::int32_t currentShadowResolution = lightManager_->GetShadowMapResolution();

        for (std::int32_t i = 0; i < IM_ARRAYSIZE(kShadowResolutions); ++i) {
            // 現在の解像度が一覧の値と一致する場合に、その選択番号へ合わせる。
            if (currentShadowResolution == kShadowResolutions[i]) {
                shadowResolutionIndex = i;
                break;
            }
        }

        // 解像度を選び直したときだけ管理側へ新しいサイズを渡す。
        if (ImGui::Combo(
            "シャドウマップ解像度",
            &shadowResolutionIndex,
            shadowResolutionLabels,
            IM_ARRAYSIZE(shadowResolutionLabels))) {
            lightManager_->SetShadowMapResolution(kShadowResolutions[shadowResolutionIndex]);
            SetStatusMessage("シャドウマップ解像度を変更しました。保存するとLight JSONへ記録されます。", true);
        }

        float shadowAreaSize = lightManager_->GetShadowAreaSize();

        // 影を作る範囲が編集された場合にだけ、新しい範囲を管理側へ反映する。
        if (ImGui::DragFloat("影を作る範囲", &shadowAreaSize, 1.0f, 20.0f, 240.0f, "%.0f")) {
            lightManager_->SetShadowAreaSize(shadowAreaSize);
        }

        ImGui::TextDisabled("範囲を狭くすると影が鮮明になり、ライト視錐台外のShadow Drawも減ります。");
        ImGui::TextDisabled("1024は2048に対して生成ピクセル数と深度メモリが1/4です。");

        ImGui::Separator();
        bool isFogEnabled = (sun.enableFog != 0);

        // 一括有効化のチェック状態が変わったら、GPU側で使う整数フラグへ反映する。
        if (ImGui::Checkbox(ICON_FA_SMOG " フォグ関連をすべて有効化 (Enable All Fog)", &isFogEnabled)) {
            sun.enableFog = isFogEnabled ? 1 : 0;
        }

        // フォグが有効な場合にだけ、距離・高さ・光の散乱を調整できる。
        if (isFogEnabled) {
            ImGui::Text(ICON_FA_CLOUD " フォグ (Fog)");
            ImGui::DragFloat(" 開始距離", &sun.fogStart, 1.0f, 0.0f, 5000.0f);
            ImGui::DragFloat(" 終了距離", &sun.fogEnd, 1.0f, 0.0f, 5000.0f);
            ImGui::ColorEdit3(" フォグ色", &sun.fogColor.x);
            ImGui::DragFloat(" 高さの底", &sun.fogHeightMin, 0.1f, -100.0f, 100.0f);
            ImGui::DragFloat(" 晴れる高さ", &sun.fogHeightMax, 0.1f, -100.0f, 100.0f);

            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), ICON_FA_MAGIC " ボリューメトリックフォグ (God Rays)");
            ImGui::DragFloat(" 強さ (Intensity)", &sun.volumetricIntensity, 0.01f, 0.0f, 5.0f);
            ImGui::DragInt(" 精密さ (Steps)", &sun.volumetricSteps, 1, 0, 64);
        }

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.2f, 0.8f, 1.0f, 1.0f), ICON_FA_GLOBE " 環境マップ (Environment Map / IBL)");
    }

    ImGui::Separator();

    // 点光源の欄が開いている間だけ、追加・削除と各ライトの調整を受け付ける。
    if (ImGui::CollapsingHeader(ICON_FA_LIGHTBULB " 点光源 (Point Light)")) {
        // 追加ボタンを押した場合は、既定値の点光源を登録する。
        if (ImGui::Button(ICON_FA_PLUS " 追加##Point"))
            lightManager_->AddPointLight();
        ImGui::SameLine();

        // 全削除の操作を受けたら、点光源と現在の選択を解除する。
        if (ImGui::Button(ICON_FA_TRASH_ALT " 全削除##Point")) {
            lightManager_->ClearPointLights();
            selectedPointLightIndex_ = -1;
        }

        auto& pointLights = lightManager_->GetPointLights();

        // 別ファイルの読込や削除で点光源が減った場合は、無効な選択位置を持ち越さない。
        if (selectedPointLightIndex_ >= static_cast<std::int32_t>(pointLights.size())) {
            selectedPointLightIndex_ = -1;
        }

        ImGui::TextDisabled("点光源: %d", static_cast<std::int32_t>(pointLights.size()));
        ImGui::BeginChild("##PointLightList", ImVec2(0.0f, 92.0f), true);

        for (std::int32_t i = 0; i < static_cast<std::int32_t>(pointLights.size()); ++i) {
            const auto& data = pointLights[i].data;
            std::string label = pointLights[i].name + "  (" +
                std::to_string(static_cast<std::int32_t>(data.position.x)) + ", " +
                std::to_string(static_cast<std::int32_t>(data.position.y)) + ", " +
                std::to_string(static_cast<std::int32_t>(data.position.z)) + ")";
            // 選択した行を、下の点光源詳細を開く対象にする。
            if (ImGui::Selectable(label.c_str(), selectedPointLightIndex_ == i)) {
                selectedPointLightIndex_ = i;
            }
        }

        ImGui::EndChild();

        for (std::int32_t i = 0; i < pointLights.size(); ++i) {
            ImGui::PushID(i);
            // 編集中はInstanceのコピーを使い、末尾のSetterでライト管理側へ書き戻す。
            engine::LightManager::PointLightInstance instance = pointLights[i];
            auto& data = instance.data;

            // 選択中のライトは次のツリーを開くよう指定する。
            if (selectedPointLightIndex_ == i) {
                ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            }

            // 詳細を開いているライトにだけ、名前や追従・描画の入力欄を出す。
            if (ImGui::TreeNode("点光源", ICON_FA_LIGHTBULB " Point Light %d", i)) {
                char nameBuffer[128]{};
                strncpy_s(nameBuffer, sizeof(nameBuffer), instance.name.c_str(), _TRUNCATE);

                // 名前欄を編集したときだけ、コピー内の名前を置き換える。
                if (ImGui::InputText(ICON_FA_TAG " 名前", nameBuffer, sizeof(nameBuffer))) {
                    instance.name = nameBuffer;
                }

                if (ImGui::Button(ICON_FA_TRASH " 削除")) {
                    // 削除後はこのInstanceを書き戻さず、開いているTreeとIDを閉じて次の要素へ進む。
                    lightManager_->RemovePointLight(static_cast<std::size_t>(i));
                    selectedPointLightIndex_ = -1;
                    ImGui::TreePop();
                    ImGui::PopID();
                    continue;
                }

                ImGui::Text(ICON_FA_COGS " --- 挙動設定 ---");
                // 追従対象があるライトは絶対位置でなく対象からのオフセットを編集する。
                std::string currentTargetName = instance.target ? instance.target->GetName() : "(None)";

                // 追従先の選択欄を開いたときにだけ、現在のSceneを取得する。
                if (ImGui::BeginCombo(ICON_FA_LINK " 追従対象", currentTargetName.c_str())) {
                    BaseScene* scene = SceneManager::GetInstance()->GetCurrentScene();

                    // 現在のSceneを参照できる場合にだけ、追従先候補を列挙する。
                    if (scene) {
                        // 「なし」を選ぶと追従先への参照を解除する。
                        if (ImGui::Selectable("(None)", instance.target == nullptr))
                            instance.target = nullptr;
                        for (auto& obj : scene->GetObjects()) {
                            // 空の登録枠やEditorの補助オブジェクトを追従候補から除く。
                            if (!obj || obj->IsEditorInternal())
                                continue;
                            bool isSelected = (instance.target == obj.get());
                            std::string name = obj->GetName().empty() ? "Object" : obj->GetName();
                            // 候補を選んだ場合に、ライトの追従先をそのオブジェクトへ更新する。
                            if (ImGui::Selectable(name.c_str(), isSelected))
                                instance.target = obj.get();
                        }
                    }

                    ImGui::EndCombo();
                }

                const char* modes[] = { "通常", "追従のみ(Follow)", "点滅(Flicker)", "明滅(Sine)" };
                std::int32_t currentMode = (std::int32_t)instance.mode;

                // モードの選択を変更したときだけ、点灯・追従などの動作を切り替える。
                if (ImGui::Combo(ICON_FA_PLAY_CIRCLE " モード", &currentMode, modes, IM_ARRAYSIZE(modes))) {
                    instance.mode = (engine::LightManager::LightMode)currentMode;
                }

                // 追従先の指定があるため、対象からの位置差を編集する。
                if (instance.target) {
                    ImGui::DragFloat3(ICON_FA_ARROWS_ALT " 位置ズレ (Offset)", &instance.offset.x, 0.1f);
                }
                // 追従先がなければライトの位置そのものを編集する。
                else {
                    ImGui::DragFloat3(ICON_FA_ARROWS_ALT " 位置 (Position)", &data.position.x, 0.1f);
                }

                ImGui::DragFloat(ICON_FA_SUN " 基準の明るさ", &instance.baseIntensity, 0.01f, 0.0f, 10.0f);

                ImGui::Text(ICON_FA_ADJUST " --- 基本設定 ---");
                ImGui::ColorEdit4(" 色", &data.color.x);
                ImGui::DragFloat(" 半径", &data.radius, 0.1f, 0.0f, 100.0f);
                ImGui::DragFloat(" 減衰", &data.decay, 0.01f, 0.0f, 10.0f);

                ImGui::TreePop();
            }

            lightManager_->SetPointLight(static_cast<std::size_t>(i), instance);
            ImGui::PopID();
        }
    }

    ImGui::Separator();

    // スポットライトの欄が開いている間だけ、個数と放射方向・角度を調整する。
    if (ImGui::CollapsingHeader(ICON_FA_STREET_VIEW " スポットライト (Spot Light)")) {
        // 追加ボタンを押した場合は、既定値のスポットライトを登録する。
        if (ImGui::Button(ICON_FA_PLUS " 追加##Spot"))
            lightManager_->AddSpotLight();
        ImGui::SameLine();

        // 全削除の操作を受けたら、スポットライトと現在の選択を解除する。
        if (ImGui::Button(ICON_FA_TRASH_ALT " 全削除##Spot")) {
            lightManager_->ClearSpotLights();
            selectedSpotLightIndex_ = -1;
        }

        auto& spotLights = lightManager_->GetSpotLights();

        // スポットライト側も件数変更後に選択位置を検証する。
        if (selectedSpotLightIndex_ >= static_cast<std::int32_t>(spotLights.size())) {
            selectedSpotLightIndex_ = -1;
        }

        ImGui::TextDisabled("スポットライト: %d", static_cast<std::int32_t>(spotLights.size()));
        ImGui::BeginChild("##SpotLightList", ImVec2(0.0f, 92.0f), true);

        for (std::int32_t i = 0; i < static_cast<std::int32_t>(spotLights.size()); ++i) {
            const auto& data = spotLights[i].data;
            std::string label = spotLights[i].name + "  (" +
                std::to_string(static_cast<std::int32_t>(data.position.x)) + ", " +
                std::to_string(static_cast<std::int32_t>(data.position.y)) + ", " +
                std::to_string(static_cast<std::int32_t>(data.position.z)) + ")";
            // クリックしたライトを下の詳細欄で開く対象にする。
            if (ImGui::Selectable(label.c_str(), selectedSpotLightIndex_ == i)) {
                selectedSpotLightIndex_ = i;
            }
        }

        ImGui::EndChild();

        for (std::int32_t i = 0; i < spotLights.size(); ++i) {
            ImGui::PushID(i + 1000);
            // スポットライトもコピーを編集して最後に書き戻し、削除した要素への反映は飛ばす。
            engine::LightManager::SpotLightInstance instance = spotLights[i];
            auto& data = instance.data;

            // 選択したライトの詳細を表示するため、続くツリーを開く。
            if (selectedSpotLightIndex_ == i) {
                ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            }

            // 詳細を開いたライトについて、名前と照射条件を編集する。
            if (ImGui::TreeNode("スポットライト", ICON_FA_STREET_VIEW " Spot Light %d", i)) {
                char nameBuffer[128]{};
                strncpy_s(nameBuffer, sizeof(nameBuffer), instance.name.c_str(), _TRUNCATE);

                // 名前欄を編集した場合だけ、コピー内の登録名を更新する。
                if (ImGui::InputText(ICON_FA_TAG " 名前", nameBuffer, sizeof(nameBuffer))) {
                    instance.name = nameBuffer;
                }

                // 削除が押された場合は現在の要素を取り除き、UIの状態を閉じて書き戻しを飛ばす。
                if (ImGui::Button(ICON_FA_TRASH " 削除")) {
                    lightManager_->RemoveSpotLight(static_cast<std::size_t>(i));
                    selectedSpotLightIndex_ = -1;
                    ImGui::TreePop();
                    ImGui::PopID();
                    continue;
                }

                ImGui::Text(ICON_FA_COGS " --- 挙動設定 ---");

                // 既存の追従先があれば相対位置を調整する。未設定時はワールド位置と向きを編集する。

                // 追従先が既にある場合は、その位置を基準とする差分を扱う。
                if (instance.target) {
                    ImGui::DragFloat3(ICON_FA_ARROWS_ALT " 位置ズレ (Offset)", &instance.offset.x, 0.1f);
                }
                // 追従なしの場合は、照射元のワールド位置と向きを編集する。
                else {
                    ImGui::DragFloat3(ICON_FA_ARROWS_ALT " 位置", &data.position.x, 0.1f);
                    ImGui::DragFloat3(ICON_FA_LOCATION_ARROW " 向き", &data.direction.x, 0.01f, -1.0f, 1.0f);
                }

                ImGui::DragFloat(ICON_FA_SUN " 基準の明るさ", &instance.baseIntensity, 0.01f, 0.0f, 10.0f);

                ImGui::Text(ICON_FA_ADJUST " --- 基本設定 ---");
                ImGui::ColorEdit4(" 色", &data.color.x);
                ImGui::DragFloat(" 距離", &data.distance, 0.1f, 0.0f, 100.0f);

                // 保存・描画は角度のcos値、UIは度を使うため、編集があったときだけcos値へ戻す。
                float angleDeg = std::acos(data.cosAngle) * 180.0f / std::numbers::pi_v<float>;

                // 度表示の角度を変更したときだけ、GPUと保存で使うcos値を計算する。
                if (ImGui::DragFloat(ICON_FA_COMPASS " 角度", &angleDeg, 1.0f, 0.1f, 179.0f)) {
                    data.cosAngle = std::cos(angleDeg * std::numbers::pi_v<float> / 180.0f);
                }

                ImGui::TreePop();
            }

            lightManager_->SetSpotLight(static_cast<std::size_t>(i), instance);
            ImGui::PopID();
        }
    }
#endif
}
