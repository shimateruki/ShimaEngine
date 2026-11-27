#define NOMINMAX
#include "ExecutablePackageWindow.h"
#include "ExecutablePackageWindow.Internal.h"

#include "IconsFontAwesome5.h"
#include "imgui.h"
#include "json.hpp"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

using executablePackageInternal::kConfigurations;
using executablePackageInternal::kTextureModeLabels;
using executablePackageInternal::TexturePackageMode;

void ExecutablePackageWindow::Initialize(DebugEditor* editor) {
    editor_ = editor;
}

bool ExecutablePackageWindow::IsTaskRunning() {
    // 結果未回収のタスクだけを確認し、待ち時間0秒で描画を止めずに状態を取得します。
    return task_.valid() && task_.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}

void ExecutablePackageWindow::PollTask() {
    // 開始済みで結果をまだ回収していないタスクが無ければ、確認するものが無い。
    if (!task_.valid()) {
        return;
    }

    // 完了していないタスクは次の描画で再確認し、ここでは待機しない。
    if (task_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }

    // ready を確認してから get し、描画スレッドを待機させません。
    PackageResult result = task_.get();
    // 成功した場合だけ、新しい解析レポートへのリンクを保持する。
    if (result.success) reportPath_ = std::move(result.reportPath);
    statusText_ = result.success ? (ICON_FA_CHECK_CIRCLE " " + result.message) : (ICON_FA_EXCLAMATION_TRIANGLE " " + result.message);
}

void ExecutablePackageWindow::StartPackageTask(bool buildBeforePackage, bool analyzeOnly) {
    // 連打で二つの出力処理が同じパッケージ先を書き換えないよう、実行中は開始しない。
    if (IsTaskRunning()) {
        return;
    }

    // UIの選択番号を有効な構成・素材形式へ収め、作成条件をこの時点で確定します。
    const std::int32_t safeIndex = std::clamp(configurationIndex_, 0, static_cast<std::int32_t>(kConfigurations.size()) - 1);
    PackageRequest request;
    request.packageName = packageNameBuffer_;
    request.configuration = kConfigurations[safeIndex];
    request.buildBeforePackage = buildBeforePackage;
    request.analyzeOnly = analyzeOnly;
    request.textureMode = std::clamp(textureModeIndex_, 0, static_cast<std::int32_t>(kTextureModeLabels.size()) - 1);
    request.createZip = createZip_;
    request.includeProject = includeProject_;
    // ReadMeは提出形式にだけ付属させ、実行ファイルセット単体には混ぜない。
    request.includeReadMe = includeProject_ && includeReadMe_;
    request.excludeUnusedResources = excludeUnusedResources_;
    request.additionalKeep = additionalKeepBuffer_;

    const char* packageKind = includeProject_ ? "提出パッケージ" : "実行ファイルセット";
    statusText_ = analyzeOnly ? "実行素材の依存関係を解析しています..." : buildBeforePackage
        ? std::string("ビルドしてから") + packageKind + "を作成しています..."
        : std::string("既存のビルド出力から") + packageKind + "を作成しています...";

    // ウィンドウの可変メンバーをワーカーから読まず、確定済み条件を値で渡します。
    task_ = std::async(std::launch::async, [request]() {
        return RunPackageTask(request);
    });
}

void ExecutablePackageWindow::DrawImGui() {
#ifdef USE_IMGUI
    PollTask();

    ImGui::TextColored(ImVec4(0.45f, 0.95f, 1.0f, 1.0f), ICON_FA_BOX_OPEN " パッケージ作成");
    ImGui::TextWrapped("実行ファイルセットだけ、または提出要件に合わせた完全パッケージをプロジェクト外へ作成します。元のファイルは変更しません。");
    ImGui::Separator();

    ImGui::InputTextWithHint("出力名", "例: LE3A_99_カマタ_タロウ", packageNameBuffer_, sizeof(packageNameBuffer_));
    ImGui::Combo("構成", &configurationIndex_, kConfigurations.data(), static_cast<std::int32_t>(kConfigurations.size()));
    ImGui::Combo("テクスチャ構成", &textureModeIndex_, kTextureModeLabels.data(), static_cast<std::int32_t>(kTextureModeLabels.size()));

    // PNG優先モードでは、対応するDDSとのペアからPNG側を残すことを表示する。
    if (textureModeIndex_ == static_cast<std::int32_t>(TexturePackageMode::kCompactPng)) {
        ImGui::TextDisabled("同名DDS/PNGペアはPNGだけを残します。DDSしかないSkybox等は保持します。");
    // DDS優先モードでは、同名ペアのDDS側が残ることを表示する。
    } else if (textureModeIndex_ == static_cast<std::int32_t>(TexturePackageMode::kFastDds)) {
        ImGui::TextDisabled("同名DDS/PNGペアはDDSだけを残し、起動時の画像デコード負荷を抑えます。");
    // その他のモードでは、両形式を同梱することを表示する。
    } else {
        ImGui::TextDisabled("DDSとPNGを両方コピーします。容量は最大ですが、素材確認用に安全です。");
    }

    ImGui::Checkbox("実行で参照する素材だけ同梱（依存関係を解析）", &excludeUnusedResources_);

    // 実行素材を絞る設定が有効な場合だけ、解析と追加保持の入力欄を表示する。
    if (excludeUnusedResources_) {
        ImGui::TextColored(
            ImVec4(1.0f, 0.78f, 0.28f, 1.0f),
            "シーン・コード・モデルの依存を調べます。保持・除外の理由はruntime_resource_report.jsonへ出力します。");
        ImGui::InputTextMultiline("追加で保持する素材・フォルダー", additionalKeepBuffer_, sizeof(additionalKeepBuffer_), ImVec2(-1, 65));
        ImGui::TextDisabled("Resources相対パスを1行1指定。例: 3DModel/Characters/custom_enemy");
    }

    ImGui::Checkbox("project一式も含める（提出形式）", &includeProject_);

    // プロジェクトを含める提出形式でだけ、ReadMeの同梱を選べる。
    if (includeProject_) {
        ImGui::Indent();
        ImGui::Checkbox("プロジェクト外のReadMe.mdを同梱", &includeReadMe_);
        ImGui::TextDisabled("参照元: projectと同じ階層のReadMe.md（このツールでは内容を生成・変更しません）");
        ImGui::Unindent();
    }

    ImGui::Checkbox("提出用ZIPも作成", &createZip_);

    // プレビューと実作成で同じ検証・パス変換を通し、表示先の食い違いを防ぎます。
    const std::string sanitizedName = executablePackageInternal::PreviewPackageName(packageNameBuffer_);
    const fs::path detectedProjectRoot = executablePackageInternal::PreviewProjectRoot();
    const fs::path previewRoot = detectedProjectRoot.empty() ? fs::path{} : detectedProjectRoot.parent_path();

    // 出力元のルートを特定できた場合は、選んだパッケージ形式の出力先を表示する。
    if (!previewRoot.empty()) {
        const fs::path outputFolder = includeProject_ ? fs::path("SubmissionPackages") : fs::path("ExecutableSets");
        ImGui::TextWrapped("出力先: %s", executablePackageInternal::PreviewPathToUtf8(previewRoot / outputFolder / executablePackageInternal::PreviewPathFromUtf8(sanitizedName)).c_str());

        // ZIP作成も指定されている場合は、ZIPの予定パスも表示する。
        if (createZip_) {
            ImGui::TextWrapped("ZIP: %s", executablePackageInternal::PreviewPathToUtf8(previewRoot / outputFolder / executablePackageInternal::PreviewPathFromUtf8(sanitizedName + ".zip")).c_str());
        }
    // 出力元のルートを特定できない場合は、想定パスの代わりに警告を出す。
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f), "プロジェクトルートを検出できません。");
    }

    // 入力名がファイル名用に変換された場合だけ、変換後の名前を併記する。
    if (sanitizedName != packageNameBuffer_) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "実際の出力名: %s", sanitizedName.c_str());
    }

    const bool running = IsTaskRunning();
    ImGui::BeginDisabled(running);

    // ビルド付きのボタンが押されたら、先にビルドする設定でパッケージ作成を開始する。
    if (ImGui::Button(ICON_FA_ROCKET " ビルドして作成", ImVec2(180.0f, 0.0f))) {
        StartPackageTask(true);
    }

    ImGui::SameLine();

    // 既存出力を使うボタンが押されたら、ビルドを省いてパッケージを作成する。
    if (ImGui::Button(ICON_FA_FOLDER_OPEN " 既存出力から作成", ImVec2(180.0f, 0.0f))) {
        StartPackageTask(false);
    }

    // 素材を絞る設定で確認ボタンが押された場合だけ、パッケージを作らず選別結果を解析する。
    if (excludeUnusedResources_ && ImGui::Button("素材の選別結果だけ確認")) {
        StartPackageTask(false, true);
    }

    ImGui::EndDisabled();

    // ワーカーが実行中の間だけ、ボタンの横へ進行中の表示を出す。
    if (running) {
        ImGui::SameLine();
        ImGui::TextDisabled("処理中...");
    }

    ImGui::Separator();
    ImGui::TextWrapped("%s", statusText_.c_str());
    // レポートへのパスがあり、ボタンが押されたら、Windowsの関連付けで開く。
    if (!reportPath_.empty() && ImGui::Button("解析レポートを開く")) {
        const auto path = executablePackageInternal::PreviewPathFromUtf8(reportPath_);
        // Windowsが32以下を返した場合は、ファイルを開けなかった旨を表示する。
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            statusText_ = "レポートを開けません: " + reportPath_;
    }

    // 提出形式では、プロジェクトの同梱構成とコピーから外す項目を説明する。
    if (includeProject_) {
        ImGui::BulletText("提出構成: 実行ファイルセット / project / ReadMe.md");
        ImGui::BulletText("project除外: .vs / .git / .agents / .codex / キャッシュ / バックアップ / 一時ファイル");
    // 実行セットだけを作る場合は、実行に関係する同梱物と除外項目を説明する。
    } else {
        ImGui::BulletText("コピー対象: exe / dll / Runtime Resources / package_manifest.json");
        ImGui::BulletText("除外対象: .cache / .backup / .trash / tools / generated/editor");
    }

    // 実行素材を絞る場合だけ、解析でさらに除外される項目を表示する。
    if (excludeUnusedResources_) {
        ImGui::BulletText("追加除外: 実行の起点から参照されない素材・制作元データ・個人セーブ");
    }

    ImGui::BulletText("DDS/PNGは同一フォルダ・同一名のペアだけを安全に整理します。");
    ImGui::BulletText("作成失敗時は既存の完成済みSetを残し、一時フォルダだけを削除します。");
#endif
}
