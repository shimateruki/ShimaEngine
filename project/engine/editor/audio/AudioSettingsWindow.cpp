#include <cstdint>
#include "AudioSettingsWindow.h"

#include "AudioPlayer.h"
#include "game/system/settings/GameAudioSettings.h"
#include "game/system/settings/GameSettingsManager.h"
#include "IconsFontAwesome5.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>

#include <filesystem>
#include <vector>
namespace {
const char* GetCategoryLabel(const std::string& category) {
    return category == "BGM" ? "BGM" : "SE";
}
}

void AudioSettingsWindow::Initialize(DebugEditor* editor) {
    editor_ = editor;
    GameAudioSettings::GetInstance()->Initialize();
}

void AudioSettingsWindow::DrawAudioEventEditor() {
    // 音源ファイルとイベント設定を分けて扱い、イベント側の調整値を編集します。
#ifdef USE_IMGUI
    // 設定欄が閉じている間は、このパネルの項目を描かない。
    if (!ImGui::CollapsingHeader(
        ICON_FA_VOLUME_UP " Audio Event",
        ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    ImGui::TextWrapped(
        "複数clipのランダム再生、音量・Pitch揺らぎ、同時発音数、距離減衰を1つのJSONで管理します。");

    // ディスク上の Event 定義を毎回列挙し、別ツールで追加された JSON も一覧へ反映します。
    const std::filesystem::path directory("Resources/json/audio_events");
    std::vector<std::string> eventFiles;
    std::error_code error;

    // イベント設定のフォルダーが確認できたときだけ、候補ファイルを探す。
    if (std::filesystem::exists(directory, error) && !error) {
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            // イベント設定として選べる候補は、拡張子が .json の通常ファイルに絞る。
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                eventFiles.push_back(entry.path().filename().string());
            }
        }
    }

    std::sort(eventFiles.begin(), eventFiles.end());

    // 選択欄が開いている間だけ、ファイル候補を表示する。
    if (ImGui::BeginCombo("Event一覧", audioEventFileName_)) {
        for (const std::string& fileName : eventFiles) {
            const bool selected = fileName == audioEventFileName_;

            // 候補が選ばれたら、そのパスを読み込み・保存用の入力欄に入れる。
            if (ImGui::Selectable(fileName.c_str(), selected)) {
                strncpy_s(
                    audioEventFileName_,
                    sizeof(audioEventFileName_),
                    fileName.c_str(),
                    _TRUNCATE);
            }

            // 現在の設定ファイルに対応する行を既定のフォーカスにする。
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }

    ImGui::InputText(
        "Eventファイル名",
        audioEventFileName_,
        sizeof(audioEventFileName_));

    // 入力名に拡張子がなければ JSON を補い、読込・保存・試聴で同じ Path を使います。
    std::filesystem::path eventPath =
        directory / std::filesystem::path(audioEventFileName_);
    // 拡張子を省略された入力には、イベント設定用の .json を補う。
    if (eventPath.extension().empty()) {
        eventPath.replace_extension(".json");
    }

    const std::string eventPathString = eventPath.generic_string();

    // 読み込みを押した場合は、入力された設定ファイルを開き、成否を表示する。
    if (ImGui::Button(ICON_FA_UPLOAD " 読込##AudioEvent")) {
        std::string message;
        const bool loaded = AudioEventSystem::GetInstance()->LoadEvent(
            eventPathString,
            audioEventDefinition_,
            &message);
        statusText_ = loaded ? "Audio Eventを読み込みました。" : message;
    }

    ImGui::SameLine();

    // 保存を押した場合は、入力先に現在の設定を書き出し、成否を表示する。
    if (ImGui::Button(ICON_FA_SAVE " 保存##AudioEvent")) {
        std::string message;
        const bool saved = AudioEventSystem::GetInstance()->SaveEvent(
            eventPathString,
            audioEventDefinition_,
            &message);
        statusText_ = saved ? "Audio Eventを保存しました。" : message;
    }

    ImGui::SameLine();

    // 試聴に渡すのはイベントのパスです。編集中の値は保存してから、ゲームと同じイベント再生経路で確認します。
    if (ImGui::Button(ICON_FA_PLAY " 試聴##AudioEvent")) {
        const auto handle =
            AudioEventSystem::GetInstance()->Play(eventPathString);
        statusText_ = handle != AudioPlayer::kInvalidPlaybackHandle
            ? "Audio Eventを再生しました。"
            : "Audio Eventを再生できませんでした。保存内容を確認してください。";
    }

    // Clip の重複を避けます。削除時は次の要素が同じ位置へ詰まるので index を進めません。
    ImGui::InputText(
        "追加するclip",
        audioEventClipPath_,
        sizeof(audioEventClipPath_));
    ImGui::SameLine();
    // 追加操作があり、パスが空でない場合に重複を調べる。
    if (ImGui::Button(ICON_FA_PLUS " 追加##AudioEventClip") &&
        audioEventClipPath_[0] != '\0') {
        const std::string clipPath = audioEventClipPath_;
        // 同じパス文字列が一覧にない場合だけ、候補に追加する。
        if (std::find(
            audioEventDefinition_.clips.begin(),
            audioEventDefinition_.clips.end(),
            clipPath) == audioEventDefinition_.clips.end()) {
            audioEventDefinition_.clips.push_back(clipPath);
        }
    }

    for (std::size_t index = 0;
        index < audioEventDefinition_.clips.size();) {
        ImGui::PushID(static_cast<std::int32_t>(index));
        ImGui::TextUnformatted(audioEventDefinition_.clips[index].c_str());
        ImGui::SameLine();

        // 削除された要素の次の項目は同じ添字に詰まるので、その添字から確認し直す。
        if (ImGui::SmallButton(ICON_FA_TRASH)) {
            audioEventDefinition_.clips.erase(
                audioEventDefinition_.clips.begin() + index);
            ImGui::PopID();
            continue;
        }

        ImGui::PopID();
        ++index;
    }

    // Event 全体の再生揺らぎと発音制限を編集します。距離減衰は 3D 指定時だけ表示します。
    ImGui::DragFloatRange2(
        "音量範囲",
        &audioEventDefinition_.volumeMin,
        &audioEventDefinition_.volumeMax,
        0.01f,
        0.0f,
        1.0f,
        "Min %.2f",
        "Max %.2f");
    ImGui::DragFloatRange2(
        "Pitch範囲",
        &audioEventDefinition_.pitchMin,
        &audioEventDefinition_.pitchMax,
        0.01f,
        0.5f,
        2.0f,
        "Min %.2f",
        "Max %.2f");
    ImGui::DragInt(
        "最大同時発音数",
        &audioEventDefinition_.maxInstances,
        1.0f,
        1,
        64);
    ImGui::Checkbox("3D距離減衰", &audioEventDefinition_.spatial);

    // 3D 音声が有効なイベントだけ、距離減衰の設定を表示する。
    if (audioEventDefinition_.spatial) {
        ImGui::DragFloat(
            "減衰開始距離",
            &audioEventDefinition_.minDistance,
            0.1f,
            0.0f,
            1000.0f);
        ImGui::DragFloat(
            "無音になる距離",
            &audioEventDefinition_.maxDistance,
            0.1f,
            // 開始距離と終了距離の間隔を確保し、減衰を評価する区間の長さが0にならないようにします。
            audioEventDefinition_.minDistance + 0.01f,
            2000.0f);
    }

    ImGui::Separator();
#endif
}

void AudioSettingsWindow::DrawImGui() {
    // 現在の設定を表示し、変更操作は Audio の設定管理へ渡します。
#ifdef USE_IMGUI
    GameSettingsManager* gameSettings = GameSettingsManager::GetInstance();
    GameAudioSettings* audioSettings = GameAudioSettings::GetInstance();
    audioSettings->Initialize();

    ImGui::TextColored(ImVec4(0.45f, 0.95f, 1.0f, 1.0f), ICON_FA_MUSIC " 音声設定");
    ImGui::TextWrapped("全体音量と、SE/BGMごとの個別音量を調整して保存できます。");
    ImGui::Separator();

    // マスター音量は操作時に反映します。保存操作で Game 設定と音源一覧を両方永続化します。
    float seMaster = gameSettings->GetSEVolume();
    float bgmMaster = gameSettings->GetBGMVolume();

    // SE の音量入力が変わったときに、再生側の設定を更新する。
    if (ImGui::SliderFloat("SE 全体音量", &seMaster, 0.0f, 1.0f, "%.2f")) {
        gameSettings->SetSEVolume(seMaster);
    }

    // BGM の音量入力が変わったときに、再生側の設定を更新する。
    if (ImGui::SliderFloat("BGM 全体音量", &bgmMaster, 0.0f, 1.0f, "%.2f")) {
        gameSettings->SetBGMVolume(bgmMaster);
    }

    // 保存ボタンで全体設定と登録一覧を書き出す。
    if (ImGui::Button(ICON_FA_SAVE " 保存")) {
        gameSettings->Save();
        audioSettings->Save();
        statusText_ = "音声設定を保存しました。";
    }

    ImGui::SameLine();

    // 再読み込みボタンで全体設定と登録一覧をディスクから読み直す。
    if (ImGui::Button(ICON_FA_SYNC " 再読み込み")) {
        gameSettings->Load();
        audioSettings->Load();
        statusText_ = "音声設定を再読み込みしました。";
    }

    ImGui::SameLine();

    // 停止ボタンは現在鳴っている BGM を停止する。
    if (ImGui::Button(ICON_FA_STOP " BGM停止")) {
        AudioPlayer::GetInstance()->StopBGM();
    }

    ImGui::TextColored(ImVec4(0.65f, 1.0f, 0.65f, 1.0f), "%s", statusText_.c_str());
    ImGui::Separator();

    DrawAudioEventEditor();

    // 新規登録欄はファイルを検索して選べる一方、入力した ID の重複と Path の実在も検証します。
    ImGui::SeparatorText("音声を登録");
    ImGui::InputText("ID##NewAudio", newAudioId_, sizeof(newAudioId_));
    ImGui::InputText("表示名##NewAudio", newAudioName_, sizeof(newAudioName_));
    ImGui::InputText("音源パス##NewAudio", newAudioPath_, sizeof(newAudioPath_));
    ImGui::Checkbox("BGM##NewAudio", &newAudioBgm_);

    // 音声ファイルの選択欄を開いている間だけ、候補を調べる。
    if (ImGui::BeginCombo("音源ファイルから選択", newAudioPath_[0] ? newAudioPath_ : "(未選択)")) {
        const std::filesystem::path audioDirectory("Resources/audio");
        std::error_code error;

        // 候補フォルダーが存在し、確認時にエラーがない場合だけ走査する。
        if (std::filesystem::exists(audioDirectory, error) && !error) {
            for (std::filesystem::recursive_directory_iterator it(
                     audioDirectory, std::filesystem::directory_options::skip_permission_denied, error), end;
                 it != end && !error; it.increment(error)) {
                // 列挙エラーが出た項目や通常ファイルではない項目は候補から外す。
                if (!it->is_regular_file(error) || error) {
                    continue;
                }

                const std::string extension = it->path().extension().string();

                // 対応する小文字拡張子の音声ファイルだけ表示する。
                if (extension != ".wav" && extension != ".mp3" && extension != ".ogg" && extension != ".flac") {
                    continue;
                }

                const std::string path = it->path().generic_string();

                // 候補を選択したら、登録用のパス入力に反映する。
                if (ImGui::Selectable(path.c_str(), path == newAudioPath_)) {
                    strncpy_s(newAudioPath_, sizeof(newAudioPath_), path.c_str(), _TRUNCATE);
                }
            }
        }

        ImGui::EndCombo();
    }

    // 登録ボタンを押した時点の ID とファイルパスで入力を検証する。
    if (ImGui::Button(ICON_FA_PLUS " 音声を追加")) {
        const std::filesystem::path path(newAudioPath_);

        // どちらかが空のままなら、登録せず必須入力を案内する。
        if (newAudioId_[0] == '\0' || newAudioPath_[0] == '\0') {
            statusText_ = "IDと音源ファイルを指定してください。";
        // パスが通常ファイルを指していない場合は、登録せずエラーを表示する。
        } else if (!std::filesystem::is_regular_file(path)) {
            statusText_ = "音源ファイルが見つかりません。";
        // 両方の入力がありファイルを確認できた場合に、一覧に追加するデータを作る。
        } else {
            GameAudioSettings::SoundEntry entry;
            entry.id = newAudioId_;
            entry.displayName = newAudioName_[0] ? newAudioName_ : newAudioId_;
            entry.category = newAudioBgm_ ? "BGM" : "SE";
            entry.path = path.generic_string();

            // IDを登録できた場合にだけ一覧を保存し、次の入力に備えて欄を空にします。
            // 登録側が受け付けた場合は一覧を保存し、次の登録用に入力欄を空にする。
            if (audioSettings->AddEntry(entry)) {
                audioSettings->Save();
                newAudioId_[0] = newAudioName_[0] = newAudioPath_[0] = '\0';
                statusText_ = "音声を登録して保存しました。";
            // 追加を受け付けなかった場合は、ID の重複を表示して入力を残す。
            } else {
                statusText_ = "そのIDは既に登録されています。";
            }
        }
    }

    ImGui::InputTextWithHint("検索", "id / 表示名 / パス", searchBuffer_, sizeof(searchBuffer_));

    // 一覧は検索で絞り、各行の音量・Loop・試聴・停止をその場で操作します。
    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_Borders |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable |
        ImGuiTableFlags_ScrollY;

    // テーブルを開始できた場合だけ、列と行を組み立てる。
    if (ImGui::BeginTable("AudioSettingsTable", 8, kTableFlags, ImVec2(0.0f, 430.0f))) {
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("表示名", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("種類", ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupColumn("個別音量", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("値", ImGuiTableColumnFlags_WidthFixed, 45.0f);
        ImGui::TableSetupColumn("Loop", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 165.0f);
        ImGui::TableSetupColumn("パス", ImGuiTableColumnFlags_WidthFixed, 260.0f);
        ImGui::TableHeadersRow();

        // 行の反復中には削除せず ID を退避し、Table 終了後に保存します。
        auto& entries = audioSettings->GetEntries();
        std::string removeId;

        for (std::int32_t index = 0; index < static_cast<std::int32_t>(entries.size()); ++index) {
            auto& entry = entries[index];
            const std::string searchText = entry.id + " " + entry.displayName + " " + entry.category + " " + entry.path;

            // 絞り込み条件に合わない行は表示しない。
            if (!MatchesSearch(searchText)) {
                continue;
            }

            ImGui::PushID(index);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(entry.id.c_str());

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(entry.displayName.c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(GetCategoryLabel(entry.category));

            ImGui::TableSetColumnIndex(3);

            // 項目の音量を変更した場合は、入力値を設定可能な範囲に収める。
            if (ImGui::SliderFloat("##volume", &entry.volume, 0.0f, 1.0f, "%.2f")) {
                entry.volume = std::clamp(entry.volume, 0.0f, 1.0f);
            }

            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.2f", entry.volume);

            ImGui::TableSetColumnIndex(5);
            ImGui::Checkbox("##loop", &entry.loop);

            ImGui::TableSetColumnIndex(6);

            // 試聴を押した行の音声を、項目に登録されたカテゴリで再生する。
            if (ImGui::SmallButton(ICON_FA_PLAY " 試聴")) {
                // BGM に登録された音声は BGM として試聴する。
                if (entry.category == "BGM") {
                    audioSettings->PlayBGM(entry.id, 1.0f, entry.loop);
                // BGM 以外の音声は SE として試聴する。
                } else {
                    audioSettings->PlaySE(entry.id, 1.0f, entry.loop);
                }
            }

            ImGui::SameLine();

            // 停止ボタンを押した項目は、登録 ID を指定して停止する。
            if (ImGui::SmallButton(ICON_FA_STOP)) {
                audioSettings->Stop(entry.id);
            }

            ImGui::SameLine();

            // 一覧の走査を終えてから削除するため、この時点では対象の ID を控える。
            if (ImGui::SmallButton(ICON_FA_TRASH)) {
                removeId = entry.id;
            }

            ImGui::TableSetColumnIndex(7);
            ImGui::TextUnformatted(entry.path.c_str());

            ImGui::PopID();
        }

        ImGui::EndTable();

        // テーブル描画後、指定 ID の削除が成功した場合だけ一覧を保存する。
        if (!removeId.empty() && audioSettings->RemoveEntry(removeId)) {
            audioSettings->Save();
            statusText_ = "音声を削除して保存しました。";
        }
    }
#endif
}

bool AudioSettingsWindow::MatchesSearch(const std::string& text) const {
    // 検索欄が空なら全件を表示し、入力時だけASCIIの大小文字を揃えて比較する。
    if (searchBuffer_[0] == '\0') {
        return true;
    }

    std::string haystack = text;
    std::string needle = searchBuffer_;
    std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return haystack.find(needle) != std::string::npos;
}
