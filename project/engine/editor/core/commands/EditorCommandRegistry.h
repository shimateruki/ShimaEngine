#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// メニュー・ショートカット・Quick Searchが共有するEditor操作の登録データ。
// 関連：EditorCommandRegistry（この型を使う機能の本体）
struct EditorCommand {
    std::string id;
    std::string displayName;
    std::string category;
    std::string shortcut;
    std::string description;
    std::vector<std::string> keywords;
    std::function<bool()> canExecute;
    std::function<void()> execute;
    // 登録者を識別する値で、解放や寿命管理はしない。終了時はコールバックの捕捉先も含め解除する。
    const void* owner = nullptr;
};

// 担当：Editor操作を登録し、実行可否・所有元・表示順を管理する。
// 関連：EditorCommand（操作定義）、GameEditorController / EditorQuickSearch（実行）
// 主な入口：Register / CanExecute / Execute / UnregisterOwner
class EditorCommandRegistry {
public:
    static EditorCommandRegistry* GetInstance();

    // 同じOwnerが同じIDを登録した場合は内容を更新します。
    // 別Ownerが使用中のIDは上書きせずfalseを返します。
    bool Register(EditorCommand command);

    // owner 指定時は登録者が一致するものだけ削除する。nullptr は所有者を限定しない。
    bool Unregister(std::string_view id, const void* owner = nullptr);

    // 登録者の全コマンドをまとめて解除する。nullptr は解除対象にしない。
    void UnregisterOwner(const void* owner);

    // 登録コマンドをすべて取り除き、一覧の変更を通知する。
    void Clear();

    // 見つからなければ nullptr。返す定義は借用参照なので解除後は使わない。
    const EditorCommand* Find(std::string_view id) const;

    // 指定IDのコマンドが登録されているか返す。
    bool Contains(std::string_view id) const;

    // 実行条件が未指定なら実行可能。未知IDや実行関数のない定義は false。
    bool CanExecute(std::string_view id) const;

    // 実行直前にも条件を確認する。false の場合はコールバックを呼ばない。
    bool Execute(std::string_view id) const;

    // 登録順を保持した一覧です。Quick SearchやCommand一覧表示で利用します。
    std::vector<const EditorCommand*> GetCommands() const;

    // 検索候補に使う世代番号。登録内容が変わったら EditorQuickSearch が候補を収集し直す。
    uint64_t GetRevision() const { return revision_; }

private:
    std::unordered_map<std::string, EditorCommand> commands_;
    std::vector<std::string> registrationOrder_;
    uint64_t revision_ = 0;
};

// 主要Command IDを共有し、文字列の打ち間違いを防ぎます。
namespace editorCommandId {
inline constexpr const char* kPlay = "game.play";
inline constexpr const char* kStop = "game.stop";
inline constexpr const char* kPauseResume = "game.pauseResume";

inline constexpr const char* kSceneNew = "scene.new";
inline constexpr const char* kSceneSave = "scene.save";

inline constexpr const char* kEditUndo = "edit.undo";
inline constexpr const char* kEditRedo = "edit.redo";
inline constexpr const char* kEditDuplicate = "edit.duplicate";
inline constexpr const char* kEditDelete = "edit.delete";

inline constexpr const char* kObjectDropToFloor = "object.dropToFloor";

inline constexpr const char* kViewEditorPanels = "view.editorPanels";
inline constexpr const char* kViewQuickSearch = "view.quickSearch";
inline constexpr const char* kViewAssetReferences = "view.assetReferences";
inline constexpr const char* kViewIsolateSelection = "view.isolateSelection";
inline constexpr const char* kViewRestoreTemporaryVisibility = "view.restoreTemporaryVisibility";
inline constexpr const char* kViewConsole = "view.console";
inline constexpr const char* kViewStatus = "view.status";
inline constexpr const char* kViewPortfolio = "view.portfolio";

inline constexpr const char* kWorkspaceVfx = "workspace.vfx";
inline constexpr const char* kWorkspaceAnimation = "workspace.animation";

inline constexpr const char* kHelpManual = "help.manual";
inline constexpr const char* kHelpProfiler = "help.profiler";
}
