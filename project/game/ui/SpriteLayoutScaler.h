#pragma once

// 各UIがJSONの設計解像度を実画面の位置・サイズへ変換する。入力判定用の基準値も同じ倍率で解決する。

#include "Math.h"
#include "WinApp.h"
#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

// JSON の設計解像度から実画面への座標変換を行います。
namespace spriteLayoutScaler {

// 有効時のみ ratio を適用し、既存 JSON の絶対座標もそのまま利用できます。
struct LayoutScale {
    engine::Vector2 designResolution = {1920.0f, 1080.0f};
    engine::Vector2 ratio = {1.0f, 1.0f};
    bool enabled = false;
};

// レイアウトの任意項目を型確認して読み、壊れた項目だけ既定値に戻します。
inline std::string ReadString(const nlohmann::json& object, const char* key, const std::string& fallback) {
    return object.is_object() && object.contains(key) && object[key].is_string()
               ? object[key].get<std::string>()
               : fallback;
}

// 指定キーがJSONの真偽値の場合だけ読み取り、それ以外はfallbackを返す。
inline bool ReadBool(const nlohmann::json& object, const char* key, bool fallback) {
    return object.is_object() && object.contains(key) && object[key].is_boolean() ? object[key].get<bool>() : fallback;
}

// 有限の数値として読める項目だけを採用する。読み取れなければfallback。
inline float ReadFloat(const nlohmann::json& object, const char* key, float fallback) {
    // オブジェクト以外、キー欠損、数値以外のいずれかなら、変換せず既定値を使う。
    if (!object.is_object() || !object.contains(key) || !object[key].is_number()) {
        return fallback;
    }
    const float value = object[key].get<float>();
    return std::isfinite(value) ? value : fallback;
}

inline std::int32_t ReadInt(const nlohmann::json& object, const char* key, std::int32_t fallback) {
    // 整数型の値だけを読み込み、32ビット符号付き整数の範囲外ならfallbackを返す。
    if (!object.is_object() || !object.contains(key) || !object[key].is_number_integer()) {
        return fallback;
    }
    const auto& value = object[key];
    // 符号なし整数は負に変換せず、符号付き32ビットの上限に収まるかを確認する。
    if (value.is_number_unsigned()) {
        const uint64_t number = value.get<uint64_t>();
        return number <= static_cast<uint64_t>((std::numeric_limits<std::int32_t>::max)()) ? static_cast<std::int32_t>(number)
                                                                                  : fallback;
    }
    // 符号付き整数は上下限を確認してから32ビットへ変換する。
    const int64_t number = value.get<int64_t>();
    return number >= (std::numeric_limits<std::int32_t>::min)() && number <= (std::numeric_limits<std::int32_t>::max)()
               ? static_cast<std::int32_t>(number)
               : fallback;
}

// 先頭2要素が有限の数値ならXYとして使う。読み取れなければfallback。
inline engine::Vector2 ReadVector2(const nlohmann::json& value, const engine::Vector2& fallback) {
    // 配列の先頭2要素が揃っており、両方とも数値の場合だけ変換できる。
    if (!value.is_array() || value.size() < 2 || !value[0].is_number() || !value[1].is_number()) {
        return fallback;
    }

    const engine::Vector2 result = {value[0].get<float>(), value[1].get<float>()};
    return std::isfinite(result.x) && std::isfinite(result.y) ? result : fallback;
}

// RGBと任意のアルファを読み取る。3要素だけの旧形式ではfallbackのアルファを保持する。
inline engine::Vector4 ReadColor(const nlohmann::json& value, const engine::Vector4& fallback) {
    // RGBだけの旧データでは既存の不透明度を維持し、NaNや無限大は採用しない。
    if (!value.is_array() || value.size() < 3) {
        return fallback;
    }
    const size_t count = (std::min)(value.size(), static_cast<size_t>(4));
    for (size_t index = 0; index < count; ++index) {
        // 読み取る成分に数値以外が混ざっていたら、色全体を既定値へ戻す。
        if (!value[index].is_number()) {
            return fallback;
        }
    }
    engine::Vector4 color = {value[0].get<float>(), value[1].get<float>(), value[2].get<float>(),
                     count == 4 ? value[3].get<float>() : fallback.w};
    return std::isfinite(color.x) && std::isfinite(color.y) && std::isfinite(color.z) && std::isfinite(color.w)
               ? color
               : fallback;
}

// サイズの分母に使う設計解像度を 1 以上に保ち、ゼロ除算を避けます。
inline float AtLeastOne(float value) { return std::isfinite(value) && value >= 1.0f ? value : 1.0f; }

// 現在の画面寸法からXとYの倍率を作る。倍率を適用するかはscaleToWindowに従う。
inline LayoutScale Make(const nlohmann::json& root) {
    LayoutScale scale;
    scale.enabled = root.is_object() && root.contains("scaleToWindow") && root["scaleToWindow"].is_boolean()
                        ? root["scaleToWindow"].get<bool>()
                        : false;

    // 設計解像度が指定されている場合は、型を確認して現在の既定解像度を置き換える。
    if (root.contains("designResolution")) {
        scale.designResolution = ReadVector2(root["designResolution"], scale.designResolution);
    }

    // 各軸の設計寸法を1以上としてから、クライアント寸法との比を求める。
    scale.designResolution.x = AtLeastOne(scale.designResolution.x);
    scale.designResolution.y = AtLeastOne(scale.designResolution.y);

    scale.ratio = {static_cast<float>(WinApp::GetClientWidth()) / scale.designResolution.x,
                   static_cast<float>(WinApp::GetClientHeight()) / scale.designResolution.y};

    return scale;
}

// 位置とサイズは X/Y を別々の倍率で伸縮します。縦横比は維持しません。
inline engine::Vector2 ScalePosition(const engine::Vector2& value, const LayoutScale& scale) {
    // 倍率の適用が無効なら、従来の画面ピクセル位置をそのまま使う。
    if (!scale.enabled) {
        return value;
    }

    return {value.x * scale.ratio.x, value.y * scale.ratio.y};
}

// 設計した幅と高さを各軸の倍率で実画面へ換算する。
inline engine::Vector2 ScaleSize(const engine::Vector2& value, const LayoutScale& scale) {
    // 倍率の適用が無効なら、幅と高さは変更しない。
    if (!scale.enabled) {
        return value;
    }

    return {value.x * scale.ratio.x, value.y * scale.ratio.y};
}

// JSON の有効フラグに関係なく設計座標を実画面へ変換します。
inline engine::Vector2 ScaleDesignPosition(const engine::Vector2& value, const engine::Vector2& designResolution = {1920.0f, 1080.0f}) {
    LayoutScale scale;
    scale.enabled = true;
    scale.designResolution = designResolution;
    scale.designResolution.x = AtLeastOne(scale.designResolution.x);
    scale.designResolution.y = AtLeastOne(scale.designResolution.y);
    scale.ratio = {static_cast<float>(WinApp::GetClientWidth()) / scale.designResolution.x,
                   static_cast<float>(WinApp::GetClientHeight()) / scale.designResolution.y};
    return ScalePosition(value, scale);
}

inline engine::Vector2 ScaleDesignSize(const engine::Vector2& value, const engine::Vector2& designResolution = {1920.0f, 1080.0f}) {
    // 保存済みの設計解像度を実クライアント寸法へ換算し、ゼロ寸法は1として扱う。
    LayoutScale scale;
    scale.enabled = true;
    scale.designResolution = designResolution;
    scale.designResolution.x = AtLeastOne(scale.designResolution.x);
    scale.designResolution.y = AtLeastOne(scale.designResolution.y);
    scale.ratio = {static_cast<float>(WinApp::GetClientWidth()) / scale.designResolution.x,
                   static_cast<float>(WinApp::GetClientHeight()) / scale.designResolution.y};
    return ScaleSize(value, scale);
}

// 設計上の横位置または横幅を、指定した設計幅からクライアントのピクセルへ換算する。
inline float ScaleDesignX(float value, float designWidth = 1920.0f) {
    designWidth = AtLeastOne(designWidth);
    return value * static_cast<float>(WinApp::GetClientWidth()) / designWidth;
}

// 設計上の縦位置または高さを、指定した設計高からクライアントのピクセルへ換算する。
inline float ScaleDesignY(float value, float designHeight = 1080.0f) {
    designHeight = AtLeastOne(designHeight);
    return value * static_cast<float>(WinApp::GetClientHeight()) / designHeight;
}

} // namespace spriteLayoutScaler
