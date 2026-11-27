#include "engine/editor/scene/DebugTeleportEditor.h"
#include "EditorQuickSearch.h"

#ifdef USE_IMGUI

#include "AssetDatabase.h"
#include "BaseScene.h"
#include "engine/editor/graphics/camera/CameraEditor.h"
#include "DebugConsole.h"
#include "DebugEditor.h"
#include "DebrisEffectEditor.h"
#include "GpuParticleEditor.h"
#include "MeshEffectEditor.h"
#include "ParticleEditor.h"
#include "TrailEmitterEditor.h"
#include "VfxSequencerEditor.h"
#include "EditorCommandRegistry.h"
#include "IEditable.h"
#include "EditorManager.h"
#include "IconsFontAwesome5.h"
#include "KeyConfig.h"
#include "Object3D.h"
#include "engine/editor/assets/preset/PresetManager.h"
#include "SceneManager.h"
#include "imgui.h"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace {
using json = nlohmann::json;

// 英字の大小を検索条件から外すため、比較に使う文字列のコピーを小文字化する。
std::string ToLowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return text;
}

// 空白で区切った全語を順位判定へ渡す。日本語のキーワード自体を文字単位には分割しない。
std::vector<std::string> SplitWords(const std::string& text) {
    std::istringstream stream(text);
    std::vector<std::string> words;

    for (std::string word; stream >> word;) {
        words.push_back(std::move(word));
    }

    return words;
}

// 検索索引に登録するキーワードを、語の境界を残した文字列にまとめる。
std::string JoinKeywords(const std::vector<std::string>& keywords) {
    std::string result;

    for (const std::string& keyword : keywords) {
        // 先頭の語には区切りを付けず、後続の語の手前だけ空白を入れる。
        if (!result.empty())
            result += ' ';
        result += keyword;
    }

    return result;
}

// 一覧ではパス末尾を表示名にし、末尾名を得られない場合は元のパスを使う。
std::string GetPathLeaf(const std::string& path) {
    const std::filesystem::path filePath(path);
    const std::string leaf = filePath.filename().string();
    return leaf.empty() ? path : leaf;
}

std::string MakeObjectDetail(engine::Object3D* object) {
    // 検索候補に種別・Layer・ワールド位置を付け、同名Objectを見分けられるようにする。
    if (!object) {
        return {};
    }

    const engine::Vector3 position = object->GetWorldPosition();
    std::ostringstream stream;
    stream << object->GetClassName() << " / Layer " << object->GetLayer()
           << " / (" << std::fixed << std::setprecision(1)
           << position.x << ", " << position.y << ", " << position.z << ')';
    return stream.str();
}
}

void EditorQuickSearch::Initialize(
    SceneManager* sceneManager,
    DebugEditor* editor,
    std::function<void()> ensureEditorPanelsVisible) {
    // 検索対象のシーンとEditorは借用し、永続化したお気に入りと履歴だけ読み戻します。
    sceneManager_ = sceneManager;
    editor_ = editor;
    ensureEditorPanelsVisible_ = std::move(ensureEditorPanelsVisible);
    LoadState();
}

void EditorQuickSearch::Finalize() {
    // 次回起動に必要な履歴を保存してから、シーンへの参照を切ります。
    SaveState();
    results_.clear();
    ensureEditorPanelsVisible_ = {};
    editor_ = nullptr;
    sceneManager_ = nullptr;
}

void EditorQuickSearch::Open() {
    // 検索欄へフォーカスを移し、開いた時点の候補を再計算します。
    openRequested_ = true;
    focusQueryRequested_ = true;
    selectedIndex_ = 0;
    RefreshResults(true);
}

void EditorQuickSearch::HandleShortcut() {
    // テキスト入力中のCtrl+Kは奪わず、通常操作時だけパレットを開きます。
    const ImGuiIO& io = ImGui::GetIO();
    // 文字入力の要求がなく、ShiftやAltを伴わないCtrl+Kが新たに押された場合だけ開く。
    if (!io.WantTextInput && io.KeyCtrl && !io.KeyShift && !io.KeyAlt &&
        ImGui::IsKeyPressed(ImGuiKey_K, false)) {
        Open();
    }
}

void EditorQuickSearch::Draw() {
    // 検索・実行を一つのモーダルで扱い、閉じる前に選択中の操作を確定します。
    const char* popupName = ICON_FA_SEARCH " 統合コマンドパレット###EditorQuickSearch";

    // ショートカットやボタンから要求が来たフレームだけモーダルを開く。
    if (openRequested_) {
        ImGui::OpenPopup(popupName);
        openRequested_ = false;
    }

    ImGui::SetNextWindowSize(ImVec2(760.0f, 520.0f), ImGuiCond_Appearing);
    // モーダルを描けないフレームは検索欄もキー操作も処理しない。
    if (!ImGui::BeginPopupModal(
        popupName,
        nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    // フォーカス要求が残っている場合、この直後の検索欄を入力先にする。
    if (focusQueryRequested_) {
        ImGui::SetKeyboardFocusHere();
        focusQueryRequested_ = false;
    }

    // 入力欄の Enter を別途受け取り、検索結果を更新した後で現在行を実行できるようにする。
    const bool submitted = ImGui::InputTextWithHint(
        "##EditorQuickSearchInput",
        "操作、Object、Asset、Scene、Preset、ウィンドウを検索...",
        query_,
        sizeof(query_),
        ImGuiInputTextFlags_EnterReturnsTrue);
    RefreshResults();

    ImGui::SameLine();
    ImGui::TextDisabled("%zu件", results_.size());
    ImGui::TextDisabled("Ctrl+K: 開く / ↑↓: 選択 / Enter: 実行 / Esc: 閉じる / ☆: お気に入り");
    ImGui::Separator();

    // 結果領域が描画可能な場合だけ、候補の行を組み立てる。
    if (ImGui::BeginChild("EditorQuickSearchResults", ImVec2(0.0f, -34.0f), false)) {
        for (std::size_t index = 0; index < results_.size(); ++index) {
            SearchItem& item = results_[index];
            ImGui::PushID(item.id.c_str());

            // 描画開始時点でお気に入りの候補は、星ボタンに強調色を積む。
            if (item.favorite) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.24f, 1.0f));
            }

            // 星ボタンを押したときは保存済みのお気に入りを切り替え、この行の状態も読み直す。
            if (ImGui::SmallButton(item.favorite ? ICON_FA_STAR : ICON_FA_STAR_OF_LIFE)) {
                ToggleFavorite(item.id);
                item.favorite = IsFavorite(item.id);
            }

            // 描画途中の変更を反映した後のお気に入り状態を見て、強調色を解除する。
            if (item.favorite) {
                ImGui::PopStyleColor();
            }

            ImGui::SameLine();

            ImGui::BeginDisabled(!item.enabled);
            const bool selected = static_cast<std::int32_t>(index) == selectedIndex_;
            const std::string label = item.label + "##result";

            // 操作可能な行がクリックされたら選択位置を更新する。
            if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, 38.0f))) {
                selectedIndex_ = static_cast<std::int32_t>(index);

                // 同じ操作が左ボタンのダブルクリックなら、選ぶと同時に実行する。
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    ExecuteItem(index);
                }
            }

            ImGui::EndDisabled();

            // 行の当たり判定を増やさず、分類と詳細を選択行の矩形上へ重ねる。
            const ImVec2 rowMin = ImGui::GetItemRectMin();
            const ImVec2 rowMax = ImGui::GetItemRectMax();
            const std::string category = '[' + item.category + ']';
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(rowMax.x - ImGui::CalcTextSize(category.c_str()).x - 8.0f, rowMin.y + 3.0f),
                IM_COL32(105, 190, 235, item.enabled ? 235 : 110),
                category.c_str());
            // 候補に識別用の詳細がある場合だけ、分類の下に表示する。
            if (!item.detail.empty()) {
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(rowMin.x + 4.0f, rowMin.y + 21.0f),
                    IM_COL32(165, 170, 180, item.enabled ? 230 : 100),
                    item.detail.c_str());
            }

            ImGui::PopID();
        }

        // 候補が空のときは結果領域内に検索結果がないことを示す。
        if (results_.empty()) {
            ImGui::TextDisabled("一致する項目がありません。");
        }
    }

    ImGui::EndChild();

    // 候補がある間だけ、行の移動と確定を受け付ける。
    if (!results_.empty()) {
        // マウスとキーボードの選択位置を同じindexに保ち、Enterで現在行だけを実行する。
        // 下キーでは最後の行まで選択を進める。
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false)) {
            selectedIndex_ = (std::min)(selectedIndex_ + 1, static_cast<std::int32_t>(results_.size()) - 1);
        }

        // 上キーでは先頭行まで選択を戻す。
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)) {
            selectedIndex_ = (std::max)(selectedIndex_ - 1, 0);
        }

        // 検索欄の確定かEnterが届いたときに現在の選択を実行する。
        if (submitted || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
            ExecuteItem(static_cast<std::size_t>(selectedIndex_));
        }
    }

    // Escが押された場合は、追加の操作を実行せずモーダルを閉じる。
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::Separator();
    ImGui::TextDisabled("Asset結果: Model/Preset/Prefabは配置、その他はPathをクリップボードへコピーします。");
    ImGui::EndPopup();
}

void EditorQuickSearch::RefreshResults(bool force) {
    // 各エディター機能の候補を集め、検索語と利用履歴に基づいて並べ直します。
    EditorCommandRegistry* commandRegistry = EditorCommandRegistry::GetInstance();
    AssetDatabase* assetDatabase = AssetDatabase::GetInstance();
    PresetManager* presetManager = PresetManager::GetInstance();
    const std::string normalizedQuery = ToLowerAscii(query_);
    const std::uint64_t commandRevision = commandRegistry->GetRevision();
    const std::uint64_t sceneGeneration = sceneManager_ ? sceneManager_->GetSceneGeneration() : 0;
    const std::uint64_t assetGeneration = assetDatabase->GetGeneration();
    const std::size_t presetCount = presetManager->GetPresets().size();
    const std::size_t prefabCount = presetManager->GetPrefabs().size();

    // 描画のたびに索引を再走査しないよう、検索語と各台帳の変更指標が同じ間は候補を使い回す。
    if (!force && normalizedQuery == lastQuery_ &&
        commandRevision == lastCommandRevision_ &&
        sceneGeneration == lastSceneGeneration_ &&
        assetGeneration == lastAssetGeneration_ &&
        presetCount == lastPresetCount_ &&
        prefabCount == lastPrefabCount_) {
        return;
    }

    lastQuery_ = normalizedQuery;
    lastCommandRevision_ = commandRevision;
    lastSceneGeneration_ = sceneGeneration;
    lastAssetGeneration_ = assetGeneration;
    lastPresetCount_ = presetCount;
    lastPrefabCount_ = prefabCount;

    std::vector<SearchItem> candidates;
    CollectCommands(candidates);
    CollectObjects(candidates);
    CollectAssets(candidates);
    CollectSceneAssets(candidates);
    CollectPresets(candidates);
    CollectEditorWindows(candidates);

    results_.clear();
    const bool hasHistory = !favoriteIds_.empty() || !recentIds_.empty();

    for (SearchItem& item : candidates) {
        item.favorite = IsFavorite(item.id);
        item.recentRank = FindRecentRank(item.id);
        item.score = ScoreItem(item, normalizedQuery);

        // 検索語が空なら履歴とお気に入りを出す。履歴がない初回は操作とウィンドウだけを案内する。
        if (normalizedQuery.empty()) {
            // 履歴かお気に入りがあれば、どちらにも属さない項目を除く。
            if (hasHistory && !item.favorite && item.recentRank < 0) {
                continue;
            }

            // 初回など保存項目がなければ、操作とウィンドウに絞る。
            if (!hasHistory && item.kind != ItemKind::kCommand && item.kind != ItemKind::kWindow) {
                continue;
            }
        }
        // 検索語がある場合、全語一致を満たさない負の点数の項目を除く。
        else if (item.score < 0) {
            continue;
        }

        results_.push_back(std::move(item));
    }

    // 検索一致度を最優先し、同点では実行可能な候補を上へ置く。分類と表示名でさらに揃える。
    std::stable_sort(results_.begin(), results_.end(), [](const SearchItem& left, const SearchItem& right) {
        // 一致度が違う場合は高い点数を優先する。
        if (left.score != right.score)
            return left.score > right.score;
        // 同点で実行可否が異なる場合は実行可能な方を先に並べる。
        if (left.enabled != right.enabled)
            return left.enabled;
        // 点数と実行可否が同じ場合は分類を先に比べる。
        if (left.category != right.category)
            return left.category < right.category;
        return left.label < right.label;
    });
    // 上位 160 件を描画対象とし、入力で候補が減ったときは選択位置も範囲に収める。
    constexpr std::size_t kMaximumResults = 160;

    // 上限を超えた場合は並べ替え後の上位のみ残す。
    if (results_.size() > kMaximumResults) {
        results_.resize(kMaximumResults);
    }

    selectedIndex_ = results_.empty()
        ? 0
        : std::clamp(selectedIndex_, 0, static_cast<std::int32_t>(results_.size()) - 1);
}

void EditorQuickSearch::CollectCommands(std::vector<SearchItem>& items) const {
    // コマンドの実行可否を収集時に評価し、無効な操作も検索結果に残します。
    EditorCommandRegistry* registry = EditorCommandRegistry::GetInstance();

    for (const EditorCommand* command : registry->GetCommands()) {
        // 登録枠が空なら表示名や実行関数を参照できないため読み飛ばす。
        if (!command)
            continue;
        SearchItem item;
        item.id = "command:" + command->id;
        item.label = command->displayName;
        item.category = "操作 / " + command->category;
        item.detail = command->shortcut.empty()
            ? command->description
            : command->shortcut + "  |  " + command->description;
        item.searchableText = item.label + ' ' + item.category + ' ' + item.detail + ' ' + JoinKeywords(command->keywords);
        item.kind = ItemKind::kCommand;
        item.enabled = registry->CanExecute(command->id);
        const std::string commandId = command->id;
        item.execute = [commandId]() {
            EditorCommandRegistry::GetInstance()->Execute(commandId);
        };

        items.push_back(std::move(item));
    }
}

void EditorQuickSearch::CollectObjects(std::vector<SearchItem>& items) const {
    // Scene内のObjectは永続GUIDを優先して識別し、Editor内部の補助Objectは除きます。
    BaseScene* scene = sceneManager_ ? sceneManager_->GetCurrentScene() : nullptr;

    // 表示対象のシーンと選択操作を受け取るエディターが揃う場合だけ候補を作る。
    if (!scene || !editor_) {
        return;
    }

    std::int32_t fallbackIndex = 0;

    for (const auto& object : scene->GetObjects()) {
        // 空の枠とエディター内部の補助物は選択項目に含めない。
        if (!object || object->IsEditorInternal()) {
            continue;
        }

        engine::Object3D* raw = object.get();
        SearchItem item;
        const std::string guid = raw->GetPersistentGuid();
        item.id = engine::Object3D::IsPersistentGuidValid(guid)
            ? "object:" + guid
            : "object:fallback:" + std::to_string(fallbackIndex++);
        item.label = raw->GetName().empty() ? "NoName Object" : raw->GetName();
        item.category = "Scene Object";
        item.detail = MakeObjectDetail(raw);
        item.searchableText = item.label + ' ' + item.detail + ' ' + raw->GetTag() + ' ' + raw->GetSaveCategory();
        item.kind = ItemKind::kObject;
        item.enabled = !sceneManager_->IsPlaying();
        item.execute = [this, raw]() {
            // 候補作成から実行までに選択対象が消える場合があるため、現在のシーンで生存を確認してから渡す。
            BaseScene* current = sceneManager_ ? sceneManager_->GetCurrentScene() : nullptr;
            if (!current || !current->IsAlive(raw) || !editor_)
                return;
            // パネル表示用の処理が登録されていれば、選択を渡す前に開く。
            if (ensureEditorPanelsVisible_)
                ensureEditorPanelsVisible_();
            editor_->SetSelectedObject(raw);
            editor_->SyncObjectSelectionToInspector();
            editor_->FocusSceneObject(raw);
        };

        items.push_back(std::move(item));
    }
}

void EditorQuickSearch::CollectAssets(std::vector<SearchItem>& items) const {
    // 配置可能なモデルだけシーンへ生成し、その他のAssetはパスをコピーします。
    AssetDatabase* database = AssetDatabase::GetInstance();

    // アセット台帳または配置側が使えない場合は候補を作らない。
    if (!database->IsInitialized() || !editor_) {
        return;
    }

    for (const EditorAssetRecord& asset : database->GetAssets()) {
        SearchItem item;
        item.id = asset.guid.empty() ? "asset:" + asset.sourcePath : "asset:" + asset.guid;
        item.label = GetPathLeaf(asset.sourcePath);
        item.category = std::string("Asset / ") + AssetDatabase::GetAssetTypeName(asset.type);
        item.detail = asset.sourcePath;
        item.searchableText = item.label + ' ' + item.category + ' ' + asset.sourcePath + ' ' + asset.importer;
        item.kind = ItemKind::kAsset;
        const bool placeableModel = asset.type == EditorAssetType::kModel &&
            asset.sourcePath.find("Resources/3DModel/") == 0;
        item.enabled = !placeableModel || (sceneManager_ && !sceneManager_->IsPlaying());
        const std::string assetPath = asset.sourcePath;
        // 3D モデルだけは配置用の相対パスへ変換する。変換できなかったモデルとその他の素材は参照パスをコピーする。
        item.execute = [this, assetPath, placeableModel]() {
            // 配置対象のモデルをエディターが扱えるときは、モデル用ルートからの相対パスを求める。
            if (placeableModel && editor_) {
                std::error_code error;
                const std::filesystem::path relative = std::filesystem::relative(
                    std::filesystem::path(assetPath),
                    std::filesystem::path("Resources/3DModel"),
                    error);
                // パス変換に成功した場合は配置して戻る。それ以外は下の共通経路で元パスをコピーする。
                if (!error) {
                    editor_->InstantiateModelAtCursor(relative.generic_string());
                    return;
                }
            }

            ImGui::SetClipboardText(assetPath.c_str());
            DebugConsole::GetInstance()->AddLog("Asset Path copied: " + assetPath);
        };

        items.push_back(std::move(item));
    }
}

void EditorQuickSearch::CollectSceneAssets(std::vector<SearchItem>& items) const {
    // 再生・遷移中はScene切り替えを許可せず、候補の表示だけ残します。
    // シーン一覧を提供するエディターがなければ検索項目を作らない。
    if (!editor_) {
        return;
    }

    for (const SceneSerializer::SceneAssetInfo& asset : editor_->GetSceneAssets()) {
        SearchItem item;
        item.id = "scene:" + asset.id;
        item.label = asset.displayName.empty() ? asset.id : asset.displayName;
        item.category = "Scene Asset";
        item.detail = asset.filename + " / " + asset.runtimeScene;
        item.searchableText = item.label + ' ' + item.detail + ' ' + asset.id + ' ' + asset.controllerName;
        item.kind = ItemKind::kScene;
        item.enabled = sceneManager_ && !sceneManager_->IsPlaying() && !sceneManager_->IsTransitioning();
        const std::string filename = asset.filename;
        item.execute = [this, filename]() {
            // シーンを開く時点でエディターが使えなければ要求を破棄する。
            if (!editor_)
                return;
            // パネルを開く処理があれば実行し、続いて既存のシーン読込要求を出す。
            if (ensureEditorPanelsVisible_)
                ensureEditorPanelsVisible_();
            editor_->RequestOpenSceneAssetFromMenu(filename);
        };

        items.push_back(std::move(item));
    }
}

void EditorQuickSearch::CollectPresets(std::vector<SearchItem>& items) const {
    // 配置できる状態かを候補ごとに固定し、再生中の誤配置を防ぎます。
    // 貼り付け先を扱うエディターがなければ配置用の候補は作らない。
    if (!editor_) {
        return;
    }

    PresetManager* manager = PresetManager::GetInstance();
    const bool canPlace = sceneManager_ && !sceneManager_->IsPlaying();

    for (const auto& [name, data] : manager->GetPresets()) {
        SearchItem item;
        item.id = "preset:" + name;
        item.label = name;
        item.category = "Preset";
        item.detail = "Game Viewの現在位置へ配置";
        item.searchableText = item.label + ' ' + item.category + ' ' + data.dump();
        item.kind = ItemKind::kPreset;
        item.enabled = canPlace;
        // 操作を確定した時点でエディターが使える場合にだけPresetを配置する。
        item.execute = [this, name]() { if (editor_) editor_->InstantiatePresetAtCursor(name); };
        items.push_back(std::move(item));
    }

    for (const auto& [name, data] : manager->GetPrefabs()) {
        SearchItem item;
        item.id = "prefab:" + name;
        item.label = name;
        item.category = "Prefab";
        item.detail = manager->IsPrefabVariant(name) ? "VariantをGame Viewへ配置" : "Game Viewの現在位置へ配置";
        item.searchableText = item.label + ' ' + item.category + ' ' + data.dump();
        item.kind = ItemKind::kPrefab;
        item.enabled = canPlace;
        // 操作を確定した時点でエディターが使える場合にだけPrefabを配置する。
        item.execute = [this, name]() { if (editor_) editor_->InstantiatePrefabAtCursor(name); };
        items.push_back(std::move(item));
    }
}

void EditorQuickSearch::CollectEditorWindows(std::vector<SearchItem>& items) const {
    // 編集ツールの参照を取得できるエディターがある場合だけ候補を登録する。
    if (!editor_) {
        return;
    }

    // 検索結果から開くツールへの借用参照と検索語。ツールの寿命はエディター側で管理する。
    // 担当：横断検索に載せるEditorの識別子・表示名・検索語と借用参照。
    // 関連：EditorQuickSearchの内部処理だけで使う補助型。
    struct WindowTarget {
        const char* id;
        const char* label;
        const char* keywords;
        IEditable* target;
    };

    const WindowTarget targets[] = {
        { "debugTeleport", "デバッグ移動エディター", "debug teleport waypoint", &DebugTeleportEditor::GetInstance() },
        { "camera", "カメラ設定", "camera view", CameraEditor::GetInstance() },
        { "animator", "Animator Controller", "animator state", editor_->GetAnimatorControllerEditor() },
        { "textPng", "テキストPNG生成", "text png", editor_->GetTextSpriteGenerator() },
        { "assetAudit", "アセット監査", "asset audit", editor_->GetAssetAuditWindow() },
        { "audio", "音声設定", "audio sound bgm", editor_->GetAudioSettingsWindow() },
        { "package", "実行ファイルセット", "executable package", editor_->GetExecutablePackageWindow() },
        { "capture", "キャプチャツール", "capture screenshot video", editor_->GetCaptureToolWindow() },
        { "gameData", "内部データ編集", "game data", editor_->GetGameDataDebugEditor() },
        { "gpuParticle", "GPUパーティクル", "gpu particle vfx", editor_->GetGpuParticleEditor() },
        { "vfxSequencer", "VFXシーケンサー", "vfx sequence", editor_->GetVfxSequencerEditor() },
        { "particle", "通常パーティクル", "particle effect", editor_->GetParticleEditor() },
        { "meshEffect", "メッシュエフェクト", "mesh effect", editor_->GetMeshEffectEditor() },
        { "debris", "3D破片エフェクト", "debris effect", editor_->GetDebrisEffectEditor() },
        { "trail", "トレイルエミッター", "trail effect", editor_->GetTrailEmitterEditor() },
    };

    for (const WindowTarget& target : targets) {
        // 用意されていないウィンドウへの操作は検索に出さない。
        if (!target.target)
            continue;
        SearchItem item;
        item.id = std::string("window:") + target.id;
        item.label = target.label;
        item.category = "Editor Window";
        item.detail = "Inspectorへ表示";
        item.searchableText = item.label + ' ' + item.category + ' ' + target.keywords;
        item.kind = ItemKind::kWindow;
        IEditable* editable = target.target;
        item.execute = [this, editable]() {
            // パネルの表示処理が登録されていれば、ツール選択を渡す前に呼ぶ。
            if (ensureEditorPanelsVisible_)
                ensureEditorPanelsVisible_();
            // デバッグ移動以外のツールを開くときは、エディターのシーンオブジェクト選択を解除する。
            if (editor_ && editable != &DebugTeleportEditor::GetInstance()) {
                editor_->SetSelectedObject(nullptr);
            }
            EditorManager::GetInstance()->SetSelectedObject(editable);
        };

        items.push_back(std::move(item));
    }
}

std::int32_t EditorQuickSearch::ScoreItem(const SearchItem& item, const std::string& normalizedQuery) const {
    // 完全一致・先頭一致・語の一致に重みを付け、お気に入りと最近使った項目を補正します。
    std::int32_t score = 0;
    // お気に入りに固定した項目は検索語と独立に加点する。
    if (item.favorite)
        score += 300;
    // 実行履歴にある項目は、最近のものほど多く加点する。
    if (item.recentRank >= 0)
        score += (std::max)(0, 160 - item.recentRank * 10);
    // 検索語がなければ履歴とお気に入りによる点数をそのまま返す。
    if (normalizedQuery.empty()) {
        return score;
    }

    const std::string label = ToLowerAscii(item.label);
    const std::string category = ToLowerAscii(item.category);
    const std::string searchable = ToLowerAscii(item.searchableText);

    for (const std::string& word : SplitWords(normalizedQuery)) {
        // 空の語は一致判定に使わない。
        if (word.empty())
            continue;
        // 名前全体と一致する語には最も高い点数を付ける。
        if (label == word) {
            score += 260;
        }
        // 先頭で一致する語は名前中の途中一致より上に置く。
        else if (label.find(word) == 0) {
            score += 180;
        }
        // 名前の途中で一致する場合も分類や説明の一致より優先する。
        else if (label.find(word) != std::string::npos) {
            score += 120;
        }
        // 名前に一致せず分類に含まれる語は分類一致として加点する。
        else if (category.find(word) != std::string::npos) {
            score += 70;
        }
        // 名前や分類に一致しない場合は説明や登録キーワードまで調べる。
        else if (searchable.find(word) != std::string::npos) {
            score += 35;
        }
        // どこにもない語が一つでもあれば、全語一致を満たさない印を返す。
        else {
            return -1;
        }
    }

    return score;
}

void EditorQuickSearch::ExecuteItem(std::size_t index) {
    // 実行前にモーダルを閉じ、次回の検索と履歴を新しい状態から始めます。
    if (index >= results_.size() || !results_[index].enabled || !results_[index].execute) {
        return;
    }

    const std::string id = results_[index].id;
    // 操作の実行でシーンや候補一覧が変わっても参照が残らないよう、呼び出し先と ID を先にコピーする。
    const auto execute = results_[index].execute;
    ImGui::CloseCurrentPopup();
    query_[0] = '\0';
    lastQuery_.clear();
    TouchRecent(id);
    execute();
}

void EditorQuickSearch::ToggleFavorite(const std::string& id) {
    // お気に入りの重複を避け、変更をすぐ保存します。
    const auto found = std::find(favoriteIds_.begin(), favoriteIds_.end(), id);

    // すでに登録済みのIDを押した場合は解除する。
    if (found != favoriteIds_.end()) {
        favoriteIds_.erase(found);
    }
    // 未登録のIDならお気に入りの先頭に登録する。
    else {
        favoriteIds_.insert(favoriteIds_.begin(), id);
    }

    SaveState();
}

void EditorQuickSearch::TouchRecent(const std::string& id) {
    // 同一操作は先頭へ移し、履歴の保存件数を制限します。
    recentIds_.erase(std::remove(recentIds_.begin(), recentIds_.end(), id), recentIds_.end());
    recentIds_.insert(recentIds_.begin(), id);
    constexpr std::size_t kRecentLimit = 16;

    // 保存上限を超えた場合は古い履歴から落とす。
    if (recentIds_.size() > kRecentLimit) {
        recentIds_.resize(kRecentLimit);
    }

    SaveState();
}

bool EditorQuickSearch::IsFavorite(const std::string& id) const {
    // 候補の種類ではなく安定IDでお気に入り状態を参照します。
    return std::find(favoriteIds_.begin(), favoriteIds_.end(), id) != favoriteIds_.end();
}

std::int32_t EditorQuickSearch::FindRecentRank(const std::string& id) const {
    // 履歴にない候補は-1とし、順位付けで通常候補と区別します。
    const auto found = std::find(recentIds_.begin(), recentIds_.end(), id);
    return found == recentIds_.end() ? -1 : static_cast<std::int32_t>(std::distance(recentIds_.begin(), found));
}

void EditorQuickSearch::LoadState() {
    // 検索の好みだけを復元し、現在のシーンや資産の候補は毎回作り直します。
    favoriteIds_.clear();
    recentIds_.clear();
    std::ifstream file(statePath_);
    // 保存ファイルが開けない場合は、先に空にした状態から始める。
    if (!file.is_open())
        return;
    try {
        json root;
        file >> root;
        favoriteIds_ = root.value("favorites", std::vector<std::string>{});
        recentIds_ = root.value("recent", std::vector<std::string>{});
    }
    // JSONの読込や型の変換に失敗した場合は、部分的に読み込んだ履歴も破棄する。
    catch (...) {
        favoriteIds_.clear();
        recentIds_.clear();
    }
}

void EditorQuickSearch::SaveState() const {
    // UI状態だけをJSONへ保存し、検索結果やシーン参照は持ち越しません。
    namespace fs = std::filesystem;
    std::error_code error;
    const fs::path filePath(statePath_);
    fs::create_directories(filePath.parent_path(), error);
    // 保存先ディレクトリを用意できなければ、ファイルを書かずに戻る。
    if (error)
        return;
    std::ofstream file(filePath);

    // 保存先を開けた場合だけ、バージョンと二つのID一覧を書き出す。
    if (file.is_open()) {
        file << json{
            { "version", 1 },
            { "favorites", favoriteIds_ },
            { "recent", recentIds_ },
        }.dump(2);
    }
}

#endif
