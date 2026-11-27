#include <cstdint>
#include "engine/editor/assets/JsonAssetBrowser.h"
#define NOMINMAX
#include "ProjectWindow.h"
#include "engine/editor/preview/AssetThumbnailRenderer.h"
#include "PresetEditor.h"
#include "TextureManager.h"
#include <limits>
#include <cfloat>
#include <functional>
#include "DebugEditor.h"
#include "imgui.h"
#include "engine/editor/assets/preset/PresetManager.h"
#include "DebugConsole.h"
#include "SceneManager.h"
#include "BaseScene.h"
#include "Object3D.h"
#include "IconsFontAwesome5.h"
#include "AssetDatabase.h"
#include "EditorAssetDragPayload.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>


namespace fs = std::filesystem;

namespace {
// 拡張子を比較する前に大小文字の差を除く。
std::string ToLowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

// OBJとglTF系のファイルを、大小文字を問わず拡張子で識別する。
bool IsModelAssetFile(const fs::path& path) {
    const std::string ext = ToLowerAscii(path.extension().string());
    return ext == ".obj" || ext == ".gltf" || ext == ".glb";
}

} // namespace

// プレビューの撮影と画像キャッシュを所有するレンダラーを用意する。
ProjectWindow::ProjectWindow()
    : thumbnailRenderer_(std::make_unique<AssetThumbnailRenderer>()) {}
ProjectWindow::~ProjectWindow() = default;

// シーンと選択の操作はDebugEditorへ渡し、DirectX基盤は撮影側へ渡す。
void ProjectWindow::Initialize(DebugEditor* editor, engine::DirectXCommon* dxCommon) {
    editor_ = editor;
    thumbnailRenderer_->Initialize(dxCommon);
}

// 描画側の呼出しで、今までに予約されたサムネイルを撮影する。
void ProjectWindow::CapturePendingThumbnails() {
    thumbnailRenderer_->CapturePendingThumbnails();
}

// 撮影の完了を待っていたHUD用ポートレートの書き出しを進める。
void ProjectWindow::ExportCapturedHudPortraits() {
    thumbnailRenderer_->ExportCapturedHudPortraits();
}

// プリセットの画像を参照するGPU記述子を返す。返せるハンドルがなければ0。
uint64_t ProjectWindow::GetPresetThumbnailGpuPtr(const std::string& presetName) {
    return thumbnailRenderer_->GetPresetThumbnailGpuPtr(presetName);
}

// 一覧の画像と独立したプレビューを要求し、指定の角度と倍率で表示できるようにする。
uint64_t ProjectWindow::GetPresetPreviewGpuPtr(const std::string& presetName, float yaw, float pitch, float zoom) {
    return thumbnailRenderer_->GetPresetPreviewGpuPtr(presetName, yaw, pitch, zoom);
}

// 定義を上書きしたプリセットの画像を無効にし、次回の参照で新しい撮影を要求させる。
void ProjectWindow::InvalidatePresetThumbnail(const std::string& presetName) {
    thumbnailRenderer_->InvalidatePresetThumbnail(presetName);
}

// 色・法線・ORM画像を持つPBRプレビューのGPU記述子を返す。撮影完了前は0。
uint64_t ProjectWindow::GetPbrThumbnailGpuPtr(const std::string& name,
    const std::string& albedoPath, const std::string& normalPath, const std::string& ormPath) {
    return thumbnailRenderer_->GetPbrThumbnailGpuPtr(name, albedoPath, normalPath, ormPath);
}

// PBR画像の条件が変わった場合に、既存のサムネイルを再利用しないようにする。
void ProjectWindow::InvalidatePbrThumbnails() {
    thumbnailRenderer_->InvalidatePbrThumbnails();
}

void ProjectWindow::Draw() {
#ifdef USE_IMGUI
    AssetDatabase* assetDatabase = AssetDatabase::GetInstance();

    // Preset、Model、Prefab、VFXを一つのProjectウィンドウのタブで切り替える。
    ImGui::Begin("Project (Assets)");

    // タブバーを描けない場合はウィンドウを閉じ、このフレームの登録一覧を出さない。
    if (!ImGui::BeginTabBar("ProjectAssetTabs")) {
        ImGui::End();
        return;
    }

    // プリセットタブの選択中は、既存ライブラリーの表示を委譲する。
    if (ImGui::BeginTabItem("プリセット")) {
        PresetEditor::GetInstance()->DrawLibrary(true);
        ImGui::EndTabItem();
    }

    // モデルタブでは台帳・変換状態と、シーンへ配置できるモデルを表示する。
    if (ImGui::BeginTabItem("モデル")) {
        ImGui::SeparatorText("Asset Database");

    // 初回索引中は進捗、完了後はAsset数・再解析数・エラー数を示す。
    const AssetDatabaseRefreshResult& databaseStatus = assetDatabase->GetLastRefreshResult();

    // 初回索引の完了前は、処理段階ごとの進捗を表示する。
    if (assetDatabase->IsInitialIndexBuildInProgress()) {
        // ディレクトリ走査中は全件数が決まっていないので、検出件数と循環バーだけを出す。
        if (assetDatabase->IsInitialDirectoryScanInProgress()) {
            ImGui::Text(
                "Asset一覧を確認中: %zu件検出",
                assetDatabase->GetInitialDiscoveredAssetCount());
            const float scanAnimation = static_cast<float>(std::fmod(ImGui::GetTime() * 0.5, 1.0));
            ImGui::ProgressBar(scanAnimation, ImVec2(-1.0f, 0.0f));
        }
        // 走査後に索引の総数が決まれば、再利用と再解析を含む進捗を件数から求める。
        else {
            const std::size_t progress = assetDatabase->GetInitialIndexProgress();
            const std::size_t total = assetDatabase->GetInitialIndexTotal();
            const float fraction = total > 0
                ? static_cast<float>(progress) / static_cast<float>(total)
                : 1.0f;
            ImGui::Text(
                "差分索引を確認中: %zu / %zu  (Cache %zu / 再解析 %zu)",
                progress,
                total,
                assetDatabase->GetInitialReusedAssetCount(),
                assetDatabase->GetInitialReparsedAssetCount());
            ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f));
        }
    }
    // 初回索引の処理中でなければ、直近の更新結果を表示する。
    else {
        ImGui::Text(
            "%zu Assets / Cache %zu / 再解析 %zu / Meta作成 %zu / Error %zu",
            databaseStatus.assetCount,
            databaseStatus.reusedAssetCount,
            databaseStatus.reparsedAssetCount,
            databaseStatus.createdMetaCount,
            databaseStatus.errorCount);
    }

    // Asset索引とDDS変換が重なる間は、手動更新と追加の変換要求を無効にする。
    const DdsCacheBuildState ddsState = assetDatabase->GetDdsCacheBuildState();
    const bool ddsProcessBusy = ddsState == DdsCacheBuildState::kQueued ||
        ddsState == DdsCacheBuildState::kRunning;
    ImGui::BeginDisabled(assetDatabase->IsInitialIndexBuildInProgress() || ddsProcessBusy);

    // 操作可能な更新ボタンを押した場合に、台帳の再走査を要求する。
    if (ImGui::Button(ICON_FA_SYNC " Asset Databaseを更新")) {
        assetDatabase->RequestRefresh(true);
    }

    ImGui::EndDisabled();

    // DDS変換を予約中または実行中なら、メッセージと進行中の表示を出す。
    if (ddsState == DdsCacheBuildState::kRunning || ddsState == DdsCacheBuildState::kQueued) {
        ImGui::TextUnformatted(assetDatabase->GetDdsCacheBuildMessage().c_str());
        const float animation = static_cast<float>(std::fmod(ImGui::GetTime() * 0.35, 1.0));
        ImGui::ProgressBar(animation, ImVec2(-1.0f, 0.0f), "DDS Cache");
    }
    // DDS変換が成功した場合は、結果の内容を成功色で出す。
    else if (ddsState == DdsCacheBuildState::kSucceeded) {
        ImGui::TextColored(
            ImVec4(0.45f, 0.9f, 0.55f, 1.0f),
            "%s",
            assetDatabase->GetDdsCacheBuildMessage().c_str());
    }
    // DDS変換が失敗した場合は、理由をエラー色で示す。
    else if (ddsState == DdsCacheBuildState::kFailed) {
        ImGui::TextColored(
            ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
            "%s",
            assetDatabase->GetDdsCacheBuildMessage().c_str());
    }

    const bool ddsBusy = ddsProcessBusy ||
        assetDatabase->IsInitialIndexBuildInProgress();
    ImGui::BeginDisabled(ddsBusy);

    // DDS生成を選んだ場合は、台帳側に非同期の変換を要求する。
    if (ImGui::Button(ICON_FA_IMAGE " 未変換DDSを生成")) {
        assetDatabase->RequestDdsCacheBuild();
    }

    ImGui::EndDisabled();

    // DDS変換ボタンにポインターを置いた場合だけ、対象画像を説明する。
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("未変換または元画像より古いDDSだけを非同期で生成します。");
    }

    // 診断が一件以上あり一覧を開いている場合は、エラーと警告を区別して表示する。
    if (!assetDatabase->GetIssues().empty() && ImGui::TreeNode("Asset Database Issues")) {
        for (const AssetDatabaseIssue& issue : assetDatabase->GetIssues()) {
            const ImVec4 color = issue.severity == AssetDatabaseIssueSeverity::kError
                ? ImVec4(1.0f, 0.35f, 0.35f, 1.0f)
                : ImVec4(1.0f, 0.78f, 0.25f, 1.0f);
            ImGui::TextColored(color, "%s", issue.path.c_str());
            ImGui::TextWrapped("  %s", issue.message.c_str());
        }

        ImGui::TreePop();
    }

    // Modelの実ファイルとディレクトリを、サムネイル付きの格子へ並べる。
    if (ImGui::CollapsingHeader("Models (Source)", ImGuiTreeNodeFlags_DefaultOpen)) {
        std::string baseDirectory = "Resources/3DModel/";

        // 参照先が未指定かモデルルートから始まらないときは、Rootの一覧から表示する。
        if (currentModelDirectory_.empty() || currentModelDirectory_.find(baseDirectory) != 0) {
            currentModelDirectory_ = baseDirectory;
        }

        // 現在地がルート以外の場合は、親へ戻る操作を表示する。
        if (currentModelDirectory_ != baseDirectory) {
            // 戻る操作を受けたときは、末尾の区切りを落とし一段上のディレクトリへ移る。
            if (ImGui::Button(ICON_FA_ARROW_UP " Back")) {
                fs::path p(currentModelDirectory_);
                currentModelDirectory_ = p.parent_path().parent_path().generic_string() + "/";
                // 移動後のパスがルート範囲から外れた場合は、モデルルートに戻す。
                if (currentModelDirectory_.length() < baseDirectory.length() ||
                    currentModelDirectory_.find(baseDirectory) == std::string::npos) {
                    currentModelDirectory_ = baseDirectory;
                }
            }

            ImGui::SameLine();
        }

        ImGui::TextDisabled("%s", currentModelDirectory_.c_str());
        ImGui::Separator();

        // ディレクトリ内のAssetだけを表示し、下位フォルダーは個別の入口として描く。
        const std::vector<std::string> modelDirectories = assetDatabase->GetSubdirectories(currentModelDirectory_);
        const std::vector<const EditorAssetRecord*> directoryAssets =
            assetDatabase->GetAssetsInDirectory(currentModelDirectory_, false);
        ImGui::TextDisabled("Drag & Drop to Scene to Place");
        ImGui::Separator();

        const float thumbnailSize = 64.0f;
        const float padding = 16.0f;
        const float cellSize = thumbnailSize + padding;
        const float panelWidth = ImGui::GetContentRegionAvail().x;
        const std::int32_t columnCount = std::max(1, static_cast<std::int32_t>(panelWidth / cellSize));

        // 格子を表示できる場合だけ、現在地のフォルダーとモデルを列挙する。
        if (ImGui::BeginTable("ModelAssetTable", columnCount)) {
            for (const std::string& directoryPath : modelDirectories) {
                const fs::path path(directoryPath);
                const std::string folderName = path.filename().string();
                std::string payloadName = fs::relative(path, baseDirectory).generic_string();
                std::replace(payloadName.begin(), payloadName.end(), '\\', '/');
                const std::vector<const EditorAssetRecord*> childAssets =
                    assetDatabase->GetAssetsInDirectory(directoryPath, false);
                const auto modelAssetIterator = std::find_if(
                    childAssets.begin(),
                    childAssets.end(),
                    [](const EditorAssetRecord* asset) {
                        return asset && asset->type == EditorAssetType::kModel;
                    });

                // 直下にModelを含むフォルダーは、フォルダー自体を配置可能なModelとして扱う。
                const bool isModelFolder = modelAssetIterator != childAssets.end();

                ImGui::TableNextColumn();

                // 直下にモデルを持つフォルダーは、フォルダー名を配置名として画像付きで表示する。
                if (isModelFolder) {
                    const uint64_t modelThumbnail = thumbnailRenderer_->GetModelThumbnailGpuPtr(payloadName);

                    ImGui::PushID(directoryPath.c_str());
                    ImGui::BeginGroup();
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::ImageButton(
                        payloadName.c_str(),
                        static_cast<ImTextureID>(modelThumbnail),
                        ImVec2(thumbnailSize, thumbnailSize));
                    ImGui::PopStyleColor();

                    // フォルダーをドラッグした場合は、直下で見つけたモデルのGUIDと相対名を渡す。
                    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                        const EditorAssetDragPayload dragPayload = MakeEditorAssetDragPayload(
                            (*modelAssetIterator)->guid,
                            payloadName);
                        ImGui::SetDragDropPayload("MODEL_ASSET", &dragPayload, sizeof(dragPayload));
                        ImGui::Image(
                            static_cast<ImTextureID>(modelThumbnail),
                            ImVec2(32.0f, 32.0f));
                        ImGui::SameLine();
                        ImGui::Text("Place: %s", folderName.c_str());

                        // 永続GUIDを取得できた場合にだけドラッグ中の補足に表示する。
                        if (!(*modelAssetIterator)->guid.empty()) {
                            ImGui::TextDisabled("GUID: %s", (*modelAssetIterator)->guid.c_str());
                        }

                        ImGui::EndDragDropSource();
                    }

                    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + thumbnailSize);
                    ImGui::TextWrapped("%s", folderName.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::EndGroup();
                    ImGui::PopID();
                }
                // 直下にモデルがないフォルダーは、移動できるディレクトリとして表示する。
                else {
                    ImGui::PushID(directoryPath.c_str());
                    ImGui::BeginGroup();
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.7f, 0.2f, 1.0f));
                    ImGui::SetWindowFontScale(3.0f);

                    // フォルダーのボタンを押した場合は、次回表示するディレクトリを切り替える。
                    if (ImGui::Button(ICON_FA_FOLDER, ImVec2(thumbnailSize, thumbnailSize))) {
                        currentModelDirectory_ = directoryPath + "/";
                    }

                    ImGui::SetWindowFontScale(1.0f);
                    ImGui::PopStyleColor(4);
                    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + thumbnailSize);
                    ImGui::TextWrapped("%s", folderName.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::EndGroup();
                    ImGui::PopID();
                }
            }

            // 個別ModelはGUIDをDrag Payloadへ載せ、Sceneへ配置するときの参照を保つ。
            for (const EditorAssetRecord* asset : directoryAssets) {
                // 台帳の空の参照とモデル以外の素材は格子の対象外にする。
                if (!asset || asset->type != EditorAssetType::kModel) {
                    continue;
                }

                const fs::path sourcePath(asset->sourcePath);
                const std::string fileName = sourcePath.filename().string();
                std::string payloadName = fs::relative(sourcePath, baseDirectory).generic_string();
                std::replace(payloadName.begin(), payloadName.end(), '\\', '/');

                const uint64_t modelThumbnail = thumbnailRenderer_->GetModelThumbnailGpuPtr(payloadName);

                ImGui::TableNextColumn();
                ImGui::PushID(asset->guid.empty() ? asset->sourcePath.c_str() : asset->guid.c_str());
                ImGui::BeginGroup();

                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));

                ImGui::ImageButton(
                    payloadName.c_str(),
                    static_cast<ImTextureID>(modelThumbnail),
                    ImVec2(thumbnailSize, thumbnailSize));
                ImGui::PopStyleColor(3);

                // 個別モデルをドラッグ中なら、シーン側の配置にGUIDと相対パスを渡す。
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    const EditorAssetDragPayload dragPayload = MakeEditorAssetDragPayload(asset->guid, payloadName);
                    ImGui::SetDragDropPayload("MODEL_ASSET", &dragPayload, sizeof(dragPayload));
                    ImGui::Image(
                        static_cast<ImTextureID>(modelThumbnail),
                        ImVec2(32.0f, 32.0f));
                    ImGui::SameLine();
                    ImGui::Text("Place: %s", fileName.c_str());

                    // 台帳でGUIDを解決済みのモデルは、ドラッグ中にそのIDを確認できる。
                    if (!asset->guid.empty()) {
                        ImGui::TextDisabled("GUID: %s", asset->guid.c_str());
                    }

                    ImGui::EndDragDropSource();
                }

                // GUIDを持つモデルにポインターがあるとき、元パスと取り込み方法を表示する。
                if (ImGui::IsItemHovered() && !asset->guid.empty()) {
                    ImGui::SetTooltip(
                        "%s\nGUID: %s\nImporter: %s",
                        asset->sourcePath.c_str(),
                        asset->guid.c_str(),
                        asset->importer.c_str());
                }

                ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + thumbnailSize);
                ImGui::TextWrapped("%s", fileName.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndGroup();
                ImGui::PopID();
            }

            ImGui::EndTable();
        }
    }

    ImGui::EndTabItem();
    }

    // Presetの作成・上書き・名前変更など、内容を変更する操作は管理タブに置く。
    if (ImGui::BeginTabItem("管理")) {
    bool openDeleteConfirmation = false;

    // プリセット管理を開いた場合に、作成と既存定義の操作を表示する。
    if (ImGui::CollapsingHeader("Presets (Configured)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), ICON_FA_PLUS_SQUARE " Create New Preset");
        ImGui::TextDisabled(" (Use '/' in name for folders, e.g. 'Enemy/Slime')");

        // 編集対象が選択されている場合だけ、その設定を保存する入力欄を出す。
        if (editor_->GetSelectedObject()) {
            static char sPresetNameBuf[64] = "NewPreset";
            ImGui::PushItemWidth(200);
            ImGui::InputText("##PresetName", sPresetNameBuf, 64);
            ImGui::PopItemWidth();

            ImGui::SameLine();

            // 選択内容の保存を押した場合にだけ名前を検証して登録を要求する。
            if (ImGui::Button(ICON_FA_SAVE " Save Selection")) {
                // 名前が空でなければ定義を書き、以前のサムネイルを無効にする。
                if (strlen(sPresetNameBuf) > 0) {
                    std::string pName = sPresetNameBuf;
                    PresetManager::GetInstance()->AddPresetFromObject(pName, editor_->GetSelectedObject());
                    InvalidatePresetThumbnail(pName);
                    DebugConsole::GetInstance()->AddLog("Saved Preset: " + pName);
                }
            }
        }
        // 対象をまだ選択していない場合は、保存前に必要な操作を表示する。
        else {
            ImGui::TextDisabled("(Select an object in Scene to save)");
        }

        ImGui::Separator();
        ImGui::Spacing();

        // HUDの再生成ボタンを押した場合は、敵プリセットの画像の撮影予約を入れる。
        if (ImGui::Button("HUDアイコンを再生成")) {
            thumbnailRenderer_->QueueHudPortraitExports(true);
            DebugConsole::GetInstance()->AddLog("HUD icon regeneration queued.");
        }

        ImGui::SameLine();
        ImGui::TextDisabled("実際の敵プリセットから通常・被弾PNGを生成します");

        ImGui::Separator();
        ImGui::Spacing();

        const auto& presets = PresetManager::GetInstance()->GetPresets();

        // 登録が一件もないあいだは、格子の代わりに空の状態を表示する。
        if (presets.empty()) {
            ImGui::TextDisabled("(No Presets Saved)");
        }
        // 登録があれば、名前の先頭区分で格子を分ける。
        else {
            // 名前の先頭のスラッシュ区切りを仮想フォルダーとして表示する。
            std::map<std::string, std::vector<std::string>> folders;
            std::vector<std::string> rootPresets;

            for (const auto& [name, data] : presets) {
                size_t slashPos = name.find('/');

                // 名前に区切りがある場合は、最初の区分を仮想フォルダー名にする。
                if (slashPos != std::string::npos) {
                    folders[name.substr(0, slashPos)].push_back(name);
                }
                // 区切りを持たない名前はルート直下として扱う。
                else {
                    rootPresets.push_back(name);
                }
            }

            // どのフォルダーでも同じサムネイルとコンテキスト操作を使う。
            auto drawPresetGrid = [&](const std::vector<std::string>& list, const char* tableId) {
                float thumbnailSize = 80.0f;
                float padding = 16.0f;
                float cellSize = thumbnailSize + padding;
                float panelWidth = ImGui::GetContentRegionAvail().x;
                std::int32_t cols = std::max(1, (std::int32_t)(panelWidth / cellSize));

                // 格子を開始できた場合だけ、そのグループの画像とコンテキスト操作を並べる。
                if (ImGui::BeginTable(tableId, cols)) {
                    for (const std::string& name : list) {
                        ImGui::TableNextColumn();
                        // スクロールで見えている項目だけ撮影を要求し、一覧全体のサムネイルを一度に生成しない。
                        const uint64_t thumbnail = ImGui::IsRectVisible(ImVec2(thumbnailSize, thumbnailSize)) ? GetPresetThumbnailGpuPtr(name) : 0;



                        ImGui::PushID(name.c_str());
                        ImGui::BeginGroup();
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.1f, 0.15f, 1.0f));
                        const bool selected = thumbnail
                            ? ImGui::ImageButton(name.c_str(), (ImTextureID)(uintptr_t)thumbnail, ImVec2(thumbnailSize, thumbnailSize))
                            : ImGui::Button(name.c_str(), ImVec2(thumbnailSize, thumbnailSize));
                        // 画像か代替ボタンがクリックされたら、プリセットエディターの選択を切り替える。
                        if (selected)
                            PresetEditor::GetInstance()->SelectPreset(name);
                        ImGui::PopStyleColor();

                        // プリセットをドラッグ中は、終端文字を含む登録名を配置先へ渡す。
                        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                            ImGui::SetDragDropPayload("PRESET_ASSET", name.c_str(), name.size() + 1);
                            ImGui::Image((ImTextureID)(uintptr_t)thumbnail, ImVec2(32.0f, 32.0f));
                            ImGui::SameLine();
                            ImGui::Text("Preset: %s", name.c_str());
                            ImGui::EndDragDropSource();
                        }

                        size_t lastSlash = name.find_last_of('/');
                        std::string shortName = PresetEditor::GetInstance()->GetDisplayName(name, presets.at(name));
                        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + thumbnailSize);
                        ImGui::TextWrapped("%s", shortName.c_str());
                        ImGui::PopTextWrapPos();

                        bool openRename = false;

                        // 右クリックから上書き、Prefab化、サムネイル再生成、削除を行う。
                        if (ImGui::BeginPopupContextItem("PresetContextMenu")) {
                            // 改名を選んだ場合は、コンテキストメニューを閉じた後のポップアップ開示を予約する。
                            if (ImGui::MenuItem(ICON_FA_EDIT " Rename")) {
                                openRename = true;
                            }

                            // 上書きを選んだときは、現在のシーン選択から定義を読み直す。
                            if (ImGui::MenuItem(ICON_FA_SAVE " Overwrite")) {
                                // 更新元の選択が残っている場合だけ、新しい定義と画像の再取得を要求する。
                                if (editor_->GetSelectedObject()) {
                                    PresetManager::GetInstance()->AddPresetFromObject(name, editor_->GetSelectedObject());
                                    InvalidatePresetThumbnail(name);
                                }
                            }

                            // Prefabへの変換を選んだ場合は、登録先の名前を決めてから変換する。
                            if (ImGui::MenuItem(ICON_FA_CODE_BRANCH " Prefabへ変換")) {
                                std::string prefabName = name;
                                std::int32_t suffix = 1;

                                // 変換元と同名の Prefab がある場合は連番で空きを探し、既存の定義を上書きしない。
                                while (PresetManager::GetInstance()->HasPrefab(prefabName)) {
                                    prefabName = name + "_Prefab" + std::to_string(suffix++);
                                }

                                // Prefabへ変換できた場合だけ、元と新しい名前をログに残す。
                                if (PresetManager::GetInstance()->CreatePrefabFromPreset(name, prefabName)) {
                                    DebugConsole::GetInstance()->AddLog(
                                        "Converted Preset to Prefab: " + name + " -> " + prefabName);
                                }
                            }

                            // 再撮影を選んだ場合は現在のキャッシュを無効にする。
                            if (ImGui::MenuItem(ICON_FA_CAMERA " Update Thumbnail"))
                                InvalidatePresetThumbnail(name);

                            ImGui::Separator();

                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.27f, 0.30f, 1.0f));

                            // 削除を選んだ場合は、ここで削除せず確認に渡す名前を覚える。
                            if (ImGui::MenuItem(ICON_FA_TRASH_ALT " 削除...")) {
                                pendingDeletePresetName_ = name;
                                openDeleteConfirmation = true;
                            }

                            ImGui::PopStyleColor();
                            ImGui::EndPopup();
                        }

                        // ImGuiのPopupはメニューを閉じた後に開くため、改名要求だけ保持する。
                        if (openRename) {
                            ImGui::OpenPopup("RenamePresetPopup");
                        }

                        // 改名ポップアップが開いている間は必要な入力を受け、確定を待つ。
                        if (ImGui::BeginPopup("RenamePresetPopup")) {
                            static char sRenameBuf[64] = "";

                            // 開いたフレームだけ元の名前をコピーし、その後の入力を上書きしない。
                            if (openRename) {
                                strncpy_s(sRenameBuf, name.c_str(), sizeof(sRenameBuf));
                            }

                            ImGui::Text("New Name:");
                            bool isEnter = ImGui::InputText("##NewName", sRenameBuf, sizeof(sRenameBuf), ImGuiInputTextFlags_EnterReturnsTrue);

                            bool isOk = ImGui::Button("OK");
                            ImGui::SameLine();
                            bool isCancel = ImGui::Button("Cancel");

                            // エンターかOKが押されたらリネーム実行
                            if (isEnter || isOk) {
                                std::string newName = sRenameBuf;

                                // 名前が空でなく元の名前と違う場合だけ、台帳に改名を要求する。
                                if (!newName.empty() && newName != name) {
                                    auto* presets = PresetManager::GetInstance();
                                    presets->RenamePreset(name, newName);
                                    // 台帳で元の名前が消えた場合にだけ、キャッシュも新名称に移す。
                                    if (!presets->HasPreset(name)) {
                                        thumbnailRenderer_->RenamePresetThumbnail(name, newName);
                                    }
                                }

                                ImGui::CloseCurrentPopup();
                            }
                            // 取消が押された場合は名前を変更せずポップアップを閉じる。
                            else if (isCancel) {
                                ImGui::CloseCurrentPopup();
                            }

                            ImGui::EndPopup();
                        }

                        ImGui::EndGroup();
                        ImGui::PopID();
                    }

                    ImGui::EndTable();
                }
                };

            // フォルダーごとに同じ描画ラムダを呼び、ルート直下だけ最後へ置く。
            for (const auto& [folderName, list] : folders) {
                // フォルダーが開いている場合だけ、配下のプリセットを描画する。
                if (ImGui::TreeNodeEx(folderName.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    drawPresetGrid(list, folderName.c_str());
                    ImGui::TreePop();
                }
            }

            // ルート直下のプリセットがあれば、他のフォルダーの配置に応じて表示先を選ぶ。
            if (!rootPresets.empty()) {
                // フォルダーが存在しない場合は、ルートの格子をそのまま表示する。
                if (folders.empty())
                    drawPresetGrid(rootPresets, "RootPresetsTable");
                // フォルダーも存在する場合は、Othersを開いているときだけ格子を描く。
                else if (ImGui::TreeNodeEx("Others", ImGuiTreeNodeFlags_DefaultOpen)) {
                    drawPresetGrid(rootPresets, "RootPresetsTable");
                    ImGui::TreePop();
                }
            }
        }
    }

    // 一覧のサムネイル描画中は登録を削除せず、一覧の参照を使い終えてから確認する。
    if (openDeleteConfirmation)
        ImGui::OpenPopup("プリセットの削除を確認");
    // 削除確認が表示された場合は、保留した名前を示して確定操作を待つ。
    if (ImGui::BeginPopupModal("プリセットの削除を確認", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("「%s」を登録一覧から削除します。", pendingDeletePresetName_.c_str());
        ImGui::TextDisabled("シーン上のObjectやモデルファイルは削除されません。");

        // 確定した場合にだけ台帳から登録を消し、保留名を解除する。
        if (ImGui::Button("削除する")) {
            PresetManager::GetInstance()->RemovePreset(pendingDeletePresetName_);

            pendingDeletePresetName_.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        // 取消では登録を保持し、確認のために保持した名前だけ解除する。
        if (ImGui::Button("キャンセル")) {
            pendingDeletePresetName_.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

        ImGui::Separator();
    ImGui::EndTabItem();
    }

    // Prefabタブでは基底と派生の両方を一覧と配置操作に出す。
    if (ImGui::BeginTabItem("Prefab")) {
        // セクションが開いているときだけ新規保存の入力と既存定義の表示を行う。
        if (ImGui::CollapsingHeader("Prefabs (v3 / Variants)", ImGuiTreeNodeFlags_DefaultOpen)) {
            static char sPrefabNameBuf[64] = "NewPrefab";
            ImGui::InputText("Prefab名", sPrefabNameBuf, IM_ARRAYSIZE(sPrefabNameBuf));

            engine::Object3D* selectedObject = editor_ ? editor_->GetSelectedObject() : nullptr;

            // シーンの選択がなければ、保存元を選ぶ方法を表示する。
            if (!selectedObject) {
                ImGui::TextDisabled("保存するObjectをHierarchyまたはGame Viewで選択してください。");
            }

            // 保存操作で元の実体と保存名が揃う場合にだけPrefabを保存する。
            if (ImGui::Button("選択ObjectをPrefab保存") && selectedObject && sPrefabNameBuf[0] != '\0') {
                PresetManager::GetInstance()->AddPrefabFromObject(sPrefabNameBuf, selectedObject);
            }

            ImGui::TextDisabled("Scene上のInstanceは元Prefabとのリンクを保持し、InspectorからApply/Revertできます。");
            ImGui::TextDisabled("旧Prefab v1/v2は読み込み時にv3へ移行されます。Presetは右クリックからPrefabへ明示変換できます。");

            const auto& prefabs = PresetManager::GetInstance()->GetPrefabs();

            // 登録が空の場合は、サムネイルの代わりに未登録と示す。
            if (prefabs.empty()) {
                ImGui::TextDisabled("Prefabはまだありません。");
            }

            std::int32_t prefabIndex = 0;

            for (const auto& [prefabName, prefabJson] : prefabs) {
                const bool isVariant = PresetManager::GetInstance()->IsPrefabVariant(prefabName);
                const std::string basePrefabName = PresetManager::GetInstance()->GetPrefabBaseName(prefabName);
                const auto structureSummary =
                    PresetManager::GetInstance()->GetPrefabStructureOverrideSummary(prefabName);
                ImGui::PushID(prefabName.c_str());
                ImGui::BeginGroup();
                ImGui::Button(isVariant ? "Variant" : "Prefab", ImVec2(86.0f, 46.0f));

                // ダブルクリックは定義の編集へ移る。シーンにインスタンスを置く操作はドラッグで受け付ける。
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && editor_) {
                    editor_->BeginPrefabEditSession(prefabName);
                }

                // ドラッグ中は配置先へPrefab登録名を渡す。
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("PREFAB_ASSET", prefabName.c_str(), prefabName.size() + 1);
                    ImGui::Text("Prefab: %s", prefabName.c_str());
                    ImGui::EndDragDropSource();
                }

                ImGui::TextWrapped("%s", prefabName.c_str());

                // 派生の場合は参照する基底の名前を表示し、解決できなければ不明を示す。
                if (isVariant) {
                    ImGui::TextDisabled("Base: %s", basePrefabName.empty() ? "Missing" : basePrefabName.c_str());

                    // 構造の上書きがある場合だけ、追加・削除・親変更の件数を出す。
                    if (structureSummary.HasOverrides()) {
                        ImGui::TextDisabled("Structure: +%d -%d Move:%d",
                            structureSummary.addedObjects,
                            structureSummary.removedObjects,
                            structureSummary.reparentedObjects);
                    }
                }

                // Prefabのコンテキストメニューを開いたときだけ、編集や派生作成を表示する。
                if (ImGui::BeginPopupContextItem("PrefabContextMenu")) {
                    // 編集メニューが選ばれ、DebugEditorが使える場合に専用編集へ移る。
                    if (ImGui::MenuItem(ICON_FA_CUBE " Prefab Modeで開く") && editor_) {
                        editor_->BeginPrefabEditSession(prefabName);
                    }

                    ImGui::Separator();

                    // 同じ基底から複数の派生を作れるよう、既存のPrefab名と衝突しない名前を探す。
                    if (ImGui::MenuItem(ICON_FA_CODE_BRANCH " Variantを作成")) {
                        std::string variantName = prefabName + "_Variant";
                        std::int32_t suffix = 1;

                        while (PresetManager::GetInstance()->HasPrefab(variantName)) {
                            variantName = prefabName + "_Variant" + std::to_string(suffix++);
                        }

                        // Variantの作成に成功した場合に作成先と基底名をログに残す。
                        if (PresetManager::GetInstance()->CreatePrefabVariant(prefabName, variantName)) {
                            DebugConsole::GetInstance()->AddLog(
                                "Created Prefab Variant: " + variantName + " from " + prefabName);
                        }
                    }

                    ImGui::TextDisabled("Asset ID: %s", prefabJson.value("assetId", "").c_str());
                    ImGui::EndPopup();
                }

                ImGui::EndGroup();
                ImGui::PopID();

                ++prefabIndex;

                // 四列の終端以外では、次のPrefabを同じ行へ並べる。
                if (prefabIndex % 4 != 0) {
                    ImGui::SameLine();
                }
            }
        }

    ImGui::EndTabItem();
    }

    // VFXタブでは粒子アセットの配置用一覧を表示する。
    if (ImGui::BeginTabItem("VFX")) {
// パーティクル一覧が開いている間は、保存済み設定を配置候補にする。
if (ImGui::CollapsingHeader("VFX / Particles", ImGuiTreeNodeFlags_DefaultOpen)) {
        std::string particleDir = "Resources/json/gpu_particles/";

        // 粒子JSONの格納先が存在する場合だけ候補を集める。
        if (fs::exists(particleDir)) {
            float thumbnailSize = 64.0f;
            float padding = 16.0f;
            float cellSize = thumbnailSize + padding;
            float panelWidth = ImGui::GetContentRegionAvail().x;
            std::int32_t columnCount = std::max(1, (std::int32_t)(panelWidth / cellSize));

            // 格子を描ける場合に、格納先から相対名を取得して並べる。
            if (ImGui::BeginTable("ParticleAssetTable", columnCount)) {
                for (const auto& presetName : JsonAssetBrowser::ListKeys("Resources/json/gpu_particles")) {
                        ImGui::TableNextColumn();

                        ImGui::PushID(("Particle_" + presetName).c_str());
                        ImGui::BeginGroup();

                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.1f, 0.1f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.15f, 0.15f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.4f, 0.2f, 0.2f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.0f, 1.0f));

                        ImGui::SetWindowFontScale(2.5f);
                        ImGui::Button(ICON_FA_FIRE, ImVec2(thumbnailSize, thumbnailSize));
                        ImGui::SetWindowFontScale(1.0f);
                        ImGui::PopStyleColor(4);

                        // 粒子をドラッグし始めたときにだけ、配置先へ渡すキーを設定する。
                        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                            // 拡張子なしのルート相対名を渡し、置いた先が粒子設定を解決する。
                            ImGui::SetDragDropPayload("PARTICLE_ASSET", presetName.c_str(), presetName.size() + 1);
                            ImGui::Text("Place Particle: %s", presetName.c_str());
                            ImGui::EndDragDropSource();
                        }

                        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + thumbnailSize);
                        ImGui::TextWrapped("%s", presetName.c_str());
                        ImGui::PopTextWrapPos();

                        ImGui::EndGroup();
                        ImGui::PopID();
                }

                ImGui::EndTable();
            }
        }
    }

    ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    ImGui::End();
#endif
}
