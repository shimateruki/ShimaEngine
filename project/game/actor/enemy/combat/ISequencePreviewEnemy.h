#pragma once

#include <string>

// 共通攻撃シーケンスの選択と現在段階の表示に対応する敵だけが実装する。
// 関連：敵のプレビュー登録がこの窓口から技を指定し、実行中の段階を表示する。
// 主な入口：SetSequencePreviewAttackId / GetSequenceStateLabel
class ISequencePreviewEnemy {
  public:
    virtual ~ISequencePreviewEnemy() = default;

    // 次のプレビューで選択する技を、攻撃シーケンスのIDで指定する。
    virtual void SetSequencePreviewAttackId(const std::string& attackId) = 0;

    // 現在の段階を示す表示名を返す。表示できる段階がなければnullptrを返せる。
    virtual const char* GetSequenceStateLabel() const = 0;
};
