#include "EditorTransactionManager.h"

#include <memory>
#include <utility>

EditorTransactionManager* EditorTransactionManager::GetInstance() {
    static EditorTransactionManager sInstance;
    return &sInstance;
}

void EditorTransactionManager::Register(EditorTransaction transaction) {
    // 履歴適用中の再登録を抑え、UndoまたはRedoのどちらかがない操作も受け付けない。
    if (isApplying_ || !transaction.undo || !transaction.redo) {
        return;
    }

    // 呼び出し元が表示名を指定しない場合は、履歴UIで使う既定名を付ける。
    if (transaction.label.empty()) {
        transaction.label = "Editor Edit";
    }

    // グループが開いている間は操作を内部一覧へ貯め、個別の履歴には積まない。
    if (groupDepth_ > 0) {
        groupTransactions_.push_back(std::move(transaction));
        return;
    }

    undoStack_.push_back(std::move(transaction));
    // 新しい編集が入った時点で、取り消した操作のRedo経路は無効になる。
    redoStack_.clear();

    // 上限を超えたら、直近の操作を残すため最古の履歴から捨てる。
    while (undoStack_.size() > kMaxHistoryEntries) {
        undoStack_.pop_front();
    }
}

void EditorTransactionManager::BeginInteractive(
    std::uint64_t itemId,
    const std::string& label,
    std::function<void()> undo) {
    // Undo/Redoの適用中か戻す処理がない場合は、ドラッグの記録を開始しない。
    if (isApplying_ || !undo) {
        return;
    }

    // ドラッグ中は開始時のUndoを保持し、毎フレーム別履歴を作らない。
    if (interactiveTransaction_ && interactiveTransaction_->itemId == itemId) {
        return;
    }

    // 別の操作が始まったら未確定の候補を破棄し、今回の開始時点の値を保存する。
    interactiveTransaction_.reset();
    InteractiveTransaction pending;
    pending.itemId = itemId;
    pending.transaction.label = label.empty() ? "Editor Property Edit" : label;
    pending.transaction.undo = std::move(undo);
    interactiveTransaction_ = std::move(pending);
}

void EditorTransactionManager::CommitInteractive(
    std::uint64_t itemId,
    std::function<void()> redo) {
    // 適用中、開始候補なし、別の入力項目、またはRedoなしの場合は確定できない。
    if (isApplying_ || !interactiveTransaction_ ||
        interactiveTransaction_->itemId != itemId || !redo) {
        return;
    }

    EditorTransaction transaction = std::move(interactiveTransaction_->transaction);
    interactiveTransaction_.reset();
    // ドラッグ開始時点の復元処理と終了時点の再適用を一組として登録する。
    transaction.redo = std::move(redo);
    Register(std::move(transaction));
}

void EditorTransactionManager::CancelInteractive(std::uint64_t itemId) {
    // 一致する操作の履歴候補を捨てるだけ。編集中に変更済みの値はこの関数で巻き戻さない。
    if (interactiveTransaction_ && interactiveTransaction_->itemId == itemId) {
        interactiveTransaction_.reset();
    }
}

void EditorTransactionManager::BeginGroup(const std::string& label) {
    // 履歴の適用中にUIからグループを開いても、履歴へ二重登録しない。
    if (isApplying_) {
        return;
    }

    // 最外層を開始するときだけ空の操作一覧と今回の名前を用意する。
    if (groupDepth_ == 0) {
        groupTransactions_.clear();
        groupLabel_ = label.empty() ? "Editor Group Edit" : label;
    }

    ++groupDepth_;
}

void EditorTransactionManager::EndGroup() {
    // 適用中か、対応するBeginGroupが残っていない場合は確定操作を行わない。
    if (isApplying_ || groupDepth_ <= 0) {
        return;
    }

    --groupDepth_;

    // 外側のグループがまだ開いている場合は、単一の履歴へまとめる処理を後回しにする。
    if (groupDepth_ > 0) {
        return;
    }

    // 編集操作が一件もなければUndoの項目を作らず、保留していた名前だけを消す。
    if (groupTransactions_.empty()) {
        groupLabel_.clear();
        return;
    }

    // 再適用と取り消しの両方から、同じ操作群の寿命を共有する。
    auto grouped = std::make_shared<std::vector<EditorTransaction>>(std::move(groupTransactions_));
    EditorTransaction transaction;
    transaction.label = groupLabel_;
    transaction.undo = [grouped]() {
        // 後から行った編集を先に戻し、依存関係を逆順にたどる。
        for (auto it = grouped->rbegin(); it != grouped->rend(); ++it) {
            // 取り消し処理を保持する操作だけを逆順で適用する。
            if (it->undo) {
                it->undo();
            }
        }
    };

    transaction.redo = [grouped]() {
        // 再適用は元の実行順。後続の編集が先行する変更を参照できるようにする。
        for (EditorTransaction& entry : *grouped) {
            // 再適用処理を保持する操作だけを登録順で適用する。
            if (entry.redo) {
                entry.redo();
            }
        }
    };

    groupLabel_.clear();
    Register(std::move(transaction));
}

bool EditorTransactionManager::Undo() {
    // 取り消せる履歴がないか既に履歴を適用中の場合は、現在の履歴位置を変更しない。
    if (undoStack_.empty() || isApplying_) {
        return false;
    }

    // 最新の操作を取り消し、適用中に再登録されないようにしてからRedo側へ移す。
    EditorTransaction transaction = std::move(undoStack_.back());
    undoStack_.pop_back();
    isApplying_ = true;
    transaction.undo();
    isApplying_ = false;
    redoStack_.push_back(std::move(transaction));
    return true;
}

bool EditorTransactionManager::Redo() {
    // やり直し履歴がないか適用中の場合は、二重実行を避けて終了する。
    if (redoStack_.empty() || isApplying_) {
        return false;
    }

    // 最新のやり直しを適用し、再度取り消せるようUndo側へ戻す。
    EditorTransaction transaction = std::move(redoStack_.back());
    redoStack_.pop_back();
    isApplying_ = true;
    transaction.redo();
    isApplying_ = false;
    undoStack_.push_back(std::move(transaction));
    return true;
}

void EditorTransactionManager::Clear() {
    // 履歴の適用中は、コールバックが使っている操作一覧を消さない。
    if (isApplying_) {
        return;
    }

    // シーン終了時などは確定履歴に加え、操作中のグループとドラッグ候補もすべて破棄する。
    undoStack_.clear();
    redoStack_.clear();
    groupTransactions_.clear();
    groupLabel_.clear();
    groupDepth_ = 0;
    interactiveTransaction_.reset();
}

const std::string& EditorTransactionManager::GetUndoLabel() const {
    static const std::string kEmpty;
    return undoStack_.empty() ? kEmpty : undoStack_.back().label;
}

const std::string& EditorTransactionManager::GetRedoLabel() const {
    static const std::string kEmpty;
    return redoStack_.empty() ? kEmpty : redoStack_.back().label;
}
