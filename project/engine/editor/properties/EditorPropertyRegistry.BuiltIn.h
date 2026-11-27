#pragma once

// 担当：組込Property登録の共通フラグと補助定義を提供する。
// 関連：EditorPropertyRegistryの登録用分割.cpp

#include "EditorPropertyRegistry.h"

namespace editorPropertyBuiltIn {

// 複数編集とPrefab差分の共通フラグです。アニメーション対応だけ個別に追加します。
inline EditorPropertyFlags CommonFlags(bool animatable = false) {
    EditorPropertyFlags flags = EditorPropertyFlags::kMultiEdit | EditorPropertyFlags::kPrefabOverride;
    // アニメーションから値を動かせるプロパティにだけ、その対応フラグを付ける。
    if (animatable) {
        flags = flags | EditorPropertyFlags::kAnimatable;
    }
    return flags;
}

} // namespace editorPropertyBuiltIn
