#define NOMINMAX
#include <cstdint>
#include "engine/editor/assets/JsonAssetBrowser.h"

#include <algorithm>

#ifdef USE_IMGUI
#include "IconsFontAwesome5.h"
#include "imgui.h"
#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
namespace {
ImGuiID bottomDockId = 0;

// ファイルシステムのパスを、表示欄で使う UTF-8 の文字列へ変換する。
std::string Utf8(const fs::path& path) {
    const auto bytes = path.generic_u8string();
    return {bytes.begin(), bytes.end()};
}

// UTF-8 で入力されたファイル名を、ネイティブのパス型へ変換する。
fs::path FromUtf8(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

// 空欄、禁止文字、制御文字、末尾のドットや空白を名前として受け付けない。
bool ValidName(const std::string& name) {
    return !name.empty() && name.find_first_of("<>:\"/\\|?*") == std::string::npos &&
        name.back() != '.' && name.back() != ' ' &&
        std::none_of(name.begin(), name.end(), [](unsigned char c) { return c < 32; });
}

// ルート外へのリンクや相対パスを経由した読み書きを防ぐ。
bool Within(const fs::path& root, const fs::path& path) {
    for (const auto& part : path.lexically_relative(root)) {
        // 相対パスに親へ抜ける要素があれば、このルート内の項目として扱わない。
        if (part == "..") return false;
    }
    // 文字列上の検証に加えて実パスも比較し、シンボリックリンクなどを経由したルート外への操作を防ぐ。
    const auto base = fs::weakly_canonical(root);
    const auto target = fs::weakly_canonical(path);
    const auto relative = target.lexically_relative(base);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

// 既存コード・設定に名前が見つかる場合は安全側に倒して移動を止める。
bool HasReference(const fs::path& target, const fs::path& rootDirectory, const fs::path& archive) {
    const auto filename = Utf8(target.filename());
    auto relative = target.lexically_relative(rootDirectory);
    const auto assetKey = Utf8(relative.replace_extension());
    // 参照を調べるソースが見つからない環境では、参照がないと判断せず退避を止める。
    if (!fs::is_directory("game") || !fs::is_directory("engine")) return true;
    for (const fs::path root : {"game", "engine", "Resources/json"}) {
        for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
            const auto& path = it->path();
            // 退避中のファイル同士を参照とは数えないため、ゴミ箱の配下は走査しない。
            if (it->is_directory() && path.filename() == archive.filename()) {
                it.disable_recursion_pending();
                continue;
            }
            const auto ext = path.extension();
            // JSON と C++ の候補拡張子だけを参照検索する。
            if (ext != ".json" && ext != ".cpp" && ext != ".h" && ext != ".inl") continue;
            // 移動するファイル自身に名前が含まれても参照とは数えない。
            if (fs::equivalent(path, target)) continue;
            std::ifstream file(path, std::ios::binary);
            // 候補を読み取れない場合も参照なしとみなさず、移動を止める。
            if (!file) return true;
            const std::string content((std::istreambuf_iterator<char>(file)), {});
            // 完全ファイル名、引用符付きの短い名前、相対キーのいずれかを文字列検索する。
            if (content.find(filename) != std::string::npos ||
                content.find('"' + Utf8(target.stem()) + '"') != std::string::npos ||
                content.find('"' + assetKey + '"') != std::string::npos) return true;
        }
    }
    return false;
}
}
#endif

std::vector<std::string> JsonAssetBrowser::ListKeys(const Path& root) {
    std::vector<std::string> keys;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        // 小文字の .json を持つ通常ファイルだけ、ルートからの相対キーを作る。
        if (!it->is_regular_file(error) || it->path().extension() != ".json") continue;
        auto relative = it->path().lexically_relative(root);
        const auto key = relative.replace_extension().generic_u8string();
        keys.emplace_back(key.begin(), key.end());
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

void JsonAssetBrowser::RequestSaveAs(const std::string& suggestedName) {
    // 次の描画で開く別名保存欄へ候補名を入れる。入力欄に収まらない名前は切り詰めず空にする。
    if (suggestedName.size() < sizeof(name_)) {
        suggestedName.copy(name_, suggestedName.size());
        name_[suggestedName.size()] = '\0';
    // 名前が入力バッファに収まらない場合は、意図しない切り詰め名で保存しないよう入力欄を空にする。
    } else {
        name_[0] = '\0';
    }
    archive_ = false;
    saveAsRequested_ = true;
}

void JsonAssetBrowser::SetBottomDock(std::uint32_t dockId) {
#ifdef USE_IMGUI
    bottomDockId = dockId;
#else
    (void)dockId;
#endif
}

void JsonAssetBrowser::SetDocument(const Path& relative) {
    // 開いた文書と現在選択したファイルを区別し、読み込み時はその文書のフォルダーへ合わせる。
    opened_ = relative;
    // 編集中の文書とのひも付けが解除された場合は、保留中の別名保存要求も取り消す。
    if (relative.empty()) saveAsRequested_ = false;
    // 空でない文書を開いた場合は、一覧を通常表示に戻し、その場所と選択を同期する。
    if (!relative.empty()) {
        folder_ = relative.parent_path();
        selected_ = relative;
        archive_ = false;
    }
}

void JsonAssetBrowser::Draw(const char* title, const Path& root, const Read& read, const Write& write,
    const std::function<void()>& reset, const std::function<void(const Path&)>& onSaved, bool unsaved) {
#ifdef USE_IMGUI
    // 下部のドックが構築されるまではウィンドウを出さない。
    if (bottomDockId == 0) return;
    ImGui::SetNextWindowDockID(bottomDockId, ImGuiCond_Always);
    // 別名保存が要求されたフレームは、モーダルを開くウィンドウに先にフォーカスを合わせる。
    if (saveAsRequested_) ImGui::SetNextWindowFocus();
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;
    // 未保存フラグを受け取った場合は、ウィンドウのタイトルにその状態を出す。
    if (unsaved) flags |= ImGuiWindowFlags_UnsavedDocument;
    const bool visible = ImGui::Begin(title, nullptr, flags);
    // ウィンドウの内容を描ける場合にだけ、一覧と保存操作を組み立てる。
    if (visible) {
        // 別名保存の保留要求は、モーダルを一度開いた時点で消費する。
        if (saveAsRequested_) {
            ImGui::OpenPopup("別名保存");
            saveAsRequested_ = false;
        }
        try {
            // 復元で元の配置を使えるよう、保存ルートごとの退避先に相対パスを維持する。
            const auto archiveRoot = fs::path(".editor-json-trash") / root.filename();
            const auto base = archive_ ? archiveRoot : root;
            // ごみ箱と通常表示の切り替え操作では、旧ルートでの選択とフォルダーをリセットする。
            if (ImGui::Button(archive_ ? "ファイルに戻る" : ICON_FA_TRASH " ゴミ箱")) {
                archive_ = !archive_;
                folder_.clear();
                selected_.clear();
            // 表示モードを変えないフレームでは、現在のルートから一覧と操作欄を描く。
            } else {
                const auto directory = base / folder_;
                ImGui::SameLine();
                ImGui::TextUnformatted(Utf8(directory).c_str());
                // 下位のフォルダーで上へ戻るボタンを押したときは、相対パスを一階層戻す。
                if (!folder_.empty() && ImGui::Button(ICON_FA_ARROW_UP " 上へ")) {
                    folder_ = folder_.parent_path();
                    selected_.clear();
                // 表示先が有効なルート内の既存フォルダーなら、子項目を一覧にする。
                } else if (Within(base, directory) && fs::is_directory(directory)) {
                    std::vector<fs::directory_entry> items;
                    for (const auto& item : fs::directory_iterator(directory)) {
                        // 別のエディターが使う退避フォルダーは一覧から隠す。
                        if (item.path().filename() == ".editor-trash") continue;
                        // フォルダーまたは .json ファイルで、現在のルートから外れないものだけ並べる。
                        if ((item.is_directory() || (item.is_regular_file() && item.path().extension() == ".json"))
                            && Within(base, item.path())) items.push_back(item);
                    }
                    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
                        // 項目の種類が異なる場合は、ファイルよりフォルダーを前へ置く。
                        if (a.is_directory() != b.is_directory()) return a.is_directory();
                        return a.path().filename() < b.path().filename();
                    });
                    // 一覧用の子ウィンドウを描ける場合だけ、項目カードを配置する。
                    if (ImGui::BeginChild("json-files", ImVec2(0, 155), true)) {
                        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
                        for (const auto& item : items) {
                            const auto relative = item.path().lexically_relative(base);
                            const auto label = Utf8(item.path().filename());
                            ImGui::PushID(label.c_str());
                            ImGui::BeginGroup();
                            const bool clicked = ImGui::Button(item.is_directory() ? ICON_FA_FOLDER : ICON_FA_FILE_CODE,
                                ImVec2(76, 35));
                            const bool open = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0);
                            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 104.0f);
                            ImGui::TextUnformatted(label.c_str());
                            ImGui::PopTextWrapPos();
                            ImGui::TextDisabled("%s", selected_ == relative ? "選択中" : " ");
                            ImGui::EndGroup();
                            ImGui::PopID();
                            // 一度押したときは、開く前に選択だけを更新する。
                            if (clicked) selected_ = relative;
                            // フォルダーをダブルクリックした場合は中へ移動し、旧一覧の走査を打ち切る。
                            if (open && item.is_directory()) {
                                folder_ = relative;
                                selected_.clear();
                                break;
                            }
                            // 通常表示でファイルをダブルクリックした場合は、読み込み確認へ進む。
                            if (open && !archive_) {
                                pending_ = relative;
                                ImGui::OpenPopup("読み込み確認");
                            }
                            // 右に次のカードを置ける間は、同じ行に並べる。
                            if (ImGui::GetItemRectMax().x + 125 < right)
                                ImGui::SameLine();
                        }
                    }
                    ImGui::EndChild();
                }

                // 対象と同じフォルダーに一時ファイルを作り、データの書き込みが成功した後に確定名へ移す。
                auto save = [&](bool replace) {
                    const auto target = root / pending_;
                    // 保存パスが通常表示のルート外なら、一時ファイルも作らない。
                    if (!Within(root, target)) { status_ = "保存先が無効です。"; return; }
                    fs::create_directories(target.parent_path());
                    wchar_t temporary[MAX_PATH]{};
                    // 保存先と同じフォルダーに一時ファイルを作れなければ、確定名へ進まない。
                    if (!GetTempFileNameW(fs::absolute(target.parent_path()).c_str(), L"jsn", 0, temporary)) {
                        status_ = "一時ファイルを作成できません。";
                        return;
                    }
                    const fs::path temp(temporary);
                    bool saved = false;
                    try {
                        saved = write(temp, Utf8(target.stem())) && MoveFileExW(temp.c_str(), target.c_str(),
                            replace ? MOVEFILE_REPLACE_EXISTING : 0);
                    } catch (const std::exception&) {
                        saved = false;
                    }
                    std::error_code error;
                    fs::remove(temp, error);
                    // 一時ファイルから確定名への移動まで成功してから、編集中の名前と選択を合わせる。
                    if (saved) {
                        opened_ = selected_ = pending_;
                        // 実ファイルの確定後にだけ、呼び出し元へ保存済みパスを通知する。
                        if (onSaved) onSaved(opened_);
                    }
                    status_ = saved ? "保存しました。" : "保存できませんでした。";
                };

                const auto selected = base / selected_;
                const bool hasFile = !selected_.empty() && Within(base, selected) && fs::is_regular_file(selected);
                // ごみ箱を表示している間は、通常の編集操作の代わりに復元を受け付ける。
                if (archive_) {
                    // ゴミ箱内で保持している相対フォルダーへ戻す。同名ファイルを上書きしない。
                    // ごみ箱内の既存ファイルを選び、復元を押した場合に対象を調べる。
                    if (hasFile && ImGui::Button("選択したJSONを復元")) {
                        const auto target = root / selected_;
                        // 元のルートの範囲外や、すでに同名の項目がある位置へは移さない。
                        if (!Within(root, target) || fs::exists(target)) status_ = "復元先に同名ファイルがあります。";
                        // 復元先を確保できる場合は、元の相対位置へ移してゴミ箱内の選択を消す。
                        else {
                            fs::create_directories(target.parent_path());
                            fs::rename(selected, target);
                            selected_.clear();
                            status_ = "復元しました。";
                        }
                    }
                // 通常のルートを参照している場合は、新規・保存・読み込み・退避を操作できる。
                } else {
                    ImGui::Text("編集中: %s%s", opened_.empty() ? "新規" : Utf8(opened_).c_str(), unsaved ? "（未保存）" : "");
                    // 新規ボタンは現在の編集内容を置き換える確認を開く。
                    if (ImGui::Button(ICON_FA_PLUS " 新規")) ImGui::OpenPopup("新規確認");
                    ImGui::SameLine();
                    // 保存を押した場合は、編集中の文書に保存名があるかで経路を分ける。
                    if (ImGui::Button(ICON_FA_SAVE " 保存")) {
                        // 保存名がまだない場合は、書き込む前に別名保存の入力を開く。
                        if (opened_.empty()) ImGui::OpenPopup("別名保存");
                        // 紐付いた文書があれば、そのパスへの上書き保存を要求する。
                        else {
                            pending_ = opened_;
                            save(true);
                        }
                    }
                    ImGui::SameLine();
                    // 明示的に別名保存を選んだときは、確定名の入力を開く。
                    if (ImGui::Button("別名保存")) ImGui::OpenPopup("別名保存");
                    ImGui::SameLine();
                    // フォルダー作成ボタンは、新しいディレクトリ名を入力する画面を開く。
                    if (ImGui::Button(ICON_FA_FOLDER " フォルダー作成")) ImGui::OpenPopup("新規フォルダー");
                    // 有効なファイルを選び、開く操作をした場合だけ読み込み確認へ進む。
                    if (hasFile && ImGui::Button("開く")) {
                        pending_ = selected_;
                        ImGui::OpenPopup("読み込み確認");
                    }
                    // ファイルを選択しているときだけ、退避操作を表示する。
                    if (hasFile) {
                        ImGui::SameLine();
                        // 退避操作を押した場合は、選択時のパスで確認を開く。
                        if (ImGui::Button("ゴミ箱へ移動")) {
                            pending_ = selected_;
                            ImGui::OpenPopup("削除確認");
                        }
                    }
                }

                // 確認ダイアログは共通の枠で開き、選択した操作が失敗した場合も画面へ戻せるようにする。
                auto modal = [&](const char* label, auto content) {
                    // 確認画面が開いている間だけ、操作に応じた中身を描画する。
                    if (ImGui::BeginPopupModal(label, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                        try { content(); }
                        catch (const std::exception&) { status_ = "ファイル操作に失敗しました。"; }
                        // 閉じるボタンは、確認画面を閉じる。
                        if (ImGui::Button("閉じる")) ImGui::CloseCurrentPopup();
                        ImGui::EndPopup();
                    }
                };
                modal("新規確認", [&] {
                    ImGui::TextUnformatted("編集中の内容を新規データで置き換えます。");
                    // 新規作成を確定した場合に編集データを初期化し、元の保存名も外す。
                    if (ImGui::Button("新規作成")) { reset(); opened_.clear(); name_[0] = 0; ImGui::CloseCurrentPopup(); }
                });
                modal("読み込み確認", [&] {
                    ImGui::TextWrapped("%s を開き、編集中の内容を置き換えます。", Utf8(pending_).c_str());
                    // 読み込みの確定後、選択をルート内として再確認してから読み込みを試す。
                    if (ImGui::Button("読み込む")) {
                        // ルート内にあり、読み込めた場合だけ、開いた文書の名前を更新する。
                        if (Within(root, root / pending_) && read(root / pending_)) opened_ = pending_;
                        // ルート検証または読み込みに失敗した場合は、エラーを表示する。
                        else status_ = "読み込めませんでした。";
                        ImGui::CloseCurrentPopup();
                    }
                });
                modal("新規フォルダー", [&] {
                    ImGui::InputText("名前", folderName_, sizeof(folderName_));
                    // 作成を押し、名前に使えない文字がない場合だけ、フォルダーを作る処理へ進む。
                    if (ImGui::Button("作成") && ValidName(folderName_)) {
                        const auto target = directory / FromUtf8(folderName_);
                        // ルート内で新しいフォルダーを作成できた場合だけ、その中へ移動する。
                        if (Within(root, target) && fs::create_directories(target)) {
                            folder_ = target.lexically_relative(root);
                            selected_.clear();
                            ImGui::CloseCurrentPopup();
                        // 対象外のパスか、実際に作成できなかった場合は失敗を表示する。
                        } else status_ = "フォルダーを作成できませんでした。";
                    }
                });
                modal("別名保存", [&] {
                    ImGui::Text("保存先: %s", Utf8(directory).c_str());
                    ImGui::InputText("名前（拡張子なし）", name_, sizeof(name_));
                    // 名前として有効な文字列を入力した場合だけ、拡張子を補った保存候補を表示する。
                    if (ValidName(name_)) {
                        pending_ = folder_ / FromUtf8(std::string(name_) + ".json");
                        const bool exists = fs::exists(root / pending_);
                        // 確定ボタンが押された場合は書き込み、同名の項目があるときだけ上書きを許可する。
                        if (ImGui::Button(exists ? "同名ファイルを上書き" : "保存する")) {
                            save(exists);
                            ImGui::CloseCurrentPopup();
                        }
                    }
                });
                modal("削除確認", [&] {
                    ImGui::TextWrapped("%s をゴミ箱に移します。", Utf8(pending_).c_str());
                    // 退避を確定した場合だけ、パスと参照の事前確認を行う。
                    if (ImGui::Button("移動する")) {
                        // 編集中の文書でも移動できるかを判定し、成功時だけファイルとの紐付けを外す。
                        const auto source = root / pending_;
                        const auto target = archiveRoot / pending_;
                        // 移動元か移動先が各ルート外、同名の退避先がある、または参照が見つかった場合は移動しない。
                        if (!Within(root, source) || !Within(archiveRoot, target) || fs::exists(target) || HasReference(source, root, archiveRoot))
                            status_ = "参照されているか、同名のゴミ箱ファイルがあるため移動できません。";
                        // 対象を移せる場合は、元のフォルダー構成を保ったまま退避する。
                        else {
                            fs::create_directories(target.parent_path());
                            fs::rename(source, target);
                            // 編集中の文書を退避したときだけ、開いているファイルとのひも付けを外す。
                            if (opened_ == pending_) opened_.clear();
                            selected_.clear();
                            status_ = "ゴミ箱に移しました。";
                        }
                        ImGui::CloseCurrentPopup();
                    }
                });
            }
        } catch (const std::exception&) {
            status_ = "フォルダーを表示できませんでした。";
        }
        // 保存・復元などの結果がある場合は、一覧の下へ表示する。
        if (!status_.empty()) ImGui::TextWrapped("%s", status_.c_str());
    }
    ImGui::End();
#endif
}
