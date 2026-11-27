#include "EditorCommandRegistry.h"

#include <algorithm>
#include <utility>

EditorCommandRegistry* EditorCommandRegistry::GetInstance() {
    static EditorCommandRegistry sInstance;
    return &sInstance;
}

bool EditorCommandRegistry::Register(EditorCommand command) {
    // 識別子、表示名、実行処理が揃っていなければ、一覧や履歴で利用できないため登録を拒否する。
    if (command.id.empty() || command.displayName.empty() || !command.execute) {
        return false;
    }

    auto existing = commands_.find(command.id);

    // 同じ識別子が登録済みの場合は、新しい行を作らず差し替えの可否を確認する。
    if (existing != commands_.end()) {
        // 違う登録者から同じ識別子を指定された場合は、既存のコマンドを保護する。
        if (existing->second.owner != command.owner) {
            return false;
        }

        // 同じ登録者の再登録では画面上の順番を保ち、検索一覧が更新できるよう世代だけ進める。
        existing->second = std::move(command);
        ++revision_;
        return true;
    }

    const std::string id = command.id;
    commands_.emplace(id, std::move(command));
    registrationOrder_.push_back(id);
    ++revision_;
    return true;
}

bool EditorCommandRegistry::Unregister(std::string_view id, const void* owner) {
    // 登録者が一致するコマンドだけを解除し、他のツールの登録を保持する。
    const std::string key(id);
    auto it = commands_.find(key);

    // 未登録IDと、指定の登録者が一致しないIDは解除できない。登録者未指定ならIDだけで解除する。
    if (it == commands_.end() || (owner && it->second.owner != owner)) {
        return false;
    }

    commands_.erase(it);
    registrationOrder_.erase(
        std::remove(registrationOrder_.begin(), registrationOrder_.end(), key),
        registrationOrder_.end());
    ++revision_;
    return true;
}

void EditorCommandRegistry::UnregisterOwner(const void* owner) {
    // 登録者の指定がなければ、個別ツールの終了処理として安全に対象を特定できない。
    if (!owner) {
        return;
    }

    bool removed = false;
    registrationOrder_.erase(
        std::remove_if(
            registrationOrder_.begin(),
            registrationOrder_.end(),
            // 表示順の一覧を掃除しながら対応する定義を外す。他の登録者は残し、古い ID だけの行は表示一覧から外す。
            [this, owner, &removed](const std::string& id) {
                const auto it = commands_.find(id);
                // 表示順の一覧にだけ残ったIDは、対応する定義がないため取り除く。
                if (it == commands_.end()) {
                    return true;
                }
                // 終了するツール以外の登録は、定義も表示順も維持する。
                if (it->second.owner != owner) {
                    return false;
                }
                commands_.erase(it);
                removed = true;
                return true;
            }),
        registrationOrder_.end());

    // 対象のコマンドを実際に解除した場合だけ、検索一覧の再取得を促す。
    if (removed) {
        ++revision_;
    }
}

void EditorCommandRegistry::Clear() {
    // 空の一覧では改訂番号を進めず、実際に一覧が変わった場合だけ通知する。
    if (commands_.empty() && registrationOrder_.empty()) {
        return;
    }

    commands_.clear();
    registrationOrder_.clear();
    ++revision_;
}

const EditorCommand* EditorCommandRegistry::Find(std::string_view id) const {
    const auto it = commands_.find(std::string(id));
    return it != commands_.end() ? &it->second : nullptr;
}

bool EditorCommandRegistry::Contains(std::string_view id) const {
    return Find(id) != nullptr;
}

bool EditorCommandRegistry::CanExecute(std::string_view id) const {
    // 実行関数のないコマンドは無効とし、条件関数がなければ実行可能とする。
    const EditorCommand* command = Find(id);

    // 対象IDがないか実行処理のないコマンドは、UI上から実行できない。
    if (!command || !command->execute) {
        return false;
    }

    return !command->canExecute || command->canExecute();
}

bool EditorCommandRegistry::Execute(std::string_view id) const {
    // UI 側と同じ実行条件を直前にも評価し、無効になった操作を実行しない。
    const EditorCommand* command = Find(id);

    // 未登録、実行処理なし、または直前の実行条件が不成立ならコールバックを呼ばない。
    if (!command || !command->execute || (command->canExecute && !command->canExecute())) {
        return false;
    }

    command->execute();
    return true;
}

std::vector<const EditorCommand*> EditorCommandRegistry::GetCommands() const {
    // ハッシュ表の走査順ではなく登録時の順番で、エディターへ表示する。
    std::vector<const EditorCommand*> result;
    result.reserve(registrationOrder_.size());

    for (const std::string& id : registrationOrder_) {
        const auto it = commands_.find(id);

        // 現在も登録されているIDの定義だけを、元の表示順で借用する。
        if (it != commands_.end()) {
            result.push_back(&it->second);
        }
    }

    return result;
}
