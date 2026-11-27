#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>

#include <windows.h> // XInputのボタン値を保持するWORDの定義。

// BindDataは、1つのアクションに割り当てるキーボード、マウス、パッド入力をまとめます。
// 関連：KeyConfig（この型を使う機能の本体）
struct BindData {
    std::int32_t keyCode = 0; // DIKキーコード。0はキーボード未割当。
    // マウスは0が左ボタンとして有効なため、未割当には-1を使う。
    std::int32_t mouseButton = -1;
    WORD padCode = 0; // XInputのボタン値。0はパッド未割当。
};

// 担当：アクション名からキーボード・マウス・パッドの固定割当を返す。
// 関連：BindData（割当）、InputManager（判定）
// 主な入口：Initialize / GetBindData / GetKeyCode / GetPadCode
class KeyConfig {
public:
    /// <summary>共通利用するキー設定インスタンスを取得する。</summary>
    static KeyConfig* GetInstance();

    /// <summary>既定のアクション割り当てを準備する。</summary>
    void Initialize();

    // 未登録アクションは0を返し、割当キーなしとして扱う。
    std::int32_t GetKeyCode(const std::string& actionName) const;

    // 未登録アクションは0を返す。複数デバイスの統合判定はInputManager側で行う。
    WORD GetPadCode(const std::string& actionName) const;

    // DIKキーコードを操作ガイド用の表示名へ変換する。未対応の非ゼロ値は16進表示。
    std::string GetKeyName(std::int32_t keyCode) const;

    /// <summary>ゲームパッドのボタンコードに対応する表示名を返す。</summary>
    std::string GetPadName(WORD padCode) const;

    /// <summary>アクション名に対応する入力割り当てを取得する。</summary>
    // 固定表の参照を返す。未登録ならnullptr。再初期化で参照は無効になる。
    const BindData* GetBindData(const std::string& actionName) const;

    /// <summary>固定表に同じキーコードがある場合にtrueを返す。実キーの競合確認に使う。</summary>
    bool IsKeyAssigned(std::int32_t keyCode) const;

private:
    KeyConfig() = default;

    ~KeyConfig() = default;

    KeyConfig(const KeyConfig&) = delete;

    KeyConfig& operator=(const KeyConfig&) = delete;

private:
    // 外部にはconst参照だけを公開する固定の入力表。
    std::unordered_map<std::string, BindData> bindings_;

};
