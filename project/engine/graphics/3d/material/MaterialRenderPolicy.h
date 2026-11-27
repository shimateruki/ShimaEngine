#pragma once

#include <array>
#include <cstdint>
#include <string_view>

// 保存済みの整数は互換用の固定値。新しい通常材質は文字列IDを使用する。
namespace materialRenderPolicy {
enum class Pass : std::uint8_t { kOpaque, kTransparent, kLocalFog, kSpecial };
enum class SpecialKind : std::uint8_t {
    kNone, kWater, kMagma, kIce, kFire, kLaser, kSlimeGel, kShockwave,
    kLiquidContact, kDamageCrack, kUpdraft, kStunBind, kCrownUnlock,
    kPoisonSpore, kCloud, kGatePortal, kWindOrb,
};
// 追加で必要な描画入力のビット集合。画面色・深度・エフェクト定数を個別に表す。
enum Input : std::uint8_t { NoExtraInput = 0, SceneColor = 1, SceneDepth = 2, EffectParameters = 4 };
// 保存済み材質番号とID・描画パス・必要入力の対応。
// 関連：BaseScene / MeshRenderer / Object3DCommon（描画分類）
struct LegacyMaterial {
    std::int32_t type;
    std::string_view id;
    const char* displayName;
    Pass pass;
    SpecialKind special;
    std::uint8_t inputs;
};
// ID と番号はシーンJSON・Replayとの互換性のため変更しない。
inline constexpr std::array<LegacyMaterial, 32> kLegacyMaterials = {{
    {0, "standard", "通常 (Standard)", Pass::kOpaque, SpecialKind::kNone, NoExtraInput},
    {1, "glass", "ガラス (Glass)", Pass::kTransparent, SpecialKind::kNone, NoExtraInput},
    {2, "crystal", "氷・宝石 (Ice/Crystal)", Pass::kOpaque, SpecialKind::kNone, NoExtraInput},
    {3, "hologram", "ホログラム (Hologram)", Pass::kOpaque, SpecialKind::kNone, NoExtraInput},
    {4, "dissolve", "消滅 (Dissolve)", Pass::kOpaque, SpecialKind::kNone, NoExtraInput},
    {5, "legacy_magma", "旧マグマ (Emissive)", Pass::kOpaque, SpecialKind::kNone, NoExtraInput},
    {6, "cel_shaded", "トゥーン調 (Cel Shaded)", Pass::kOpaque, SpecialKind::kNone, NoExtraInput},
    {7, "local_fog", "ローカルフォグ (Local Fog)", Pass::kLocalFog, SpecialKind::kNone, NoExtraInput},
    {8, "water", "水 (Water)", Pass::kSpecial, SpecialKind::kWater, 7},
    {9, "magma", "新マグマ (Magma)", Pass::kSpecial, SpecialKind::kMagma, 7},
    {10, "thick_ice", "分厚い氷 (Ice)", Pass::kSpecial, SpecialKind::kIce, 7},
    {11, "fire", "炎 (Fire)", Pass::kSpecial, SpecialKind::kFire, 7},
    {12, "laser", "レーザー (Laser)", Pass::kSpecial, SpecialKind::kLaser, 7},
    {13, "slime_gel", "スライムジェル (Slime Gel)", Pass::kSpecial, SpecialKind::kSlimeGel, 7},
    {14, "shockwave", "地面衝撃波 (Shockwave)", Pass::kSpecial, SpecialKind::kShockwave, 7},
    {15, "liquid_contact", "水/マグマ接触 (Liquid Contact)", Pass::kSpecial, SpecialKind::kLiquidContact, 7},
    {16, "damage_crack", "ダメージ亀裂 (Damage Crack)", Pass::kSpecial, SpecialKind::kDamageCrack, 7},
    {17, "updraft", "上昇気流 (Updraft)", Pass::kSpecial, SpecialKind::kUpdraft, 7},
    {18, "stun_bind", "スタン拘束 (Stun Bind)", Pass::kSpecial, SpecialKind::kStunBind, 7},
    {19, "crown_unlock", "王冠解放 (Crown Unlock)", Pass::kSpecial, SpecialKind::kCrownUnlock, 7},
    {20, "poison_spore", "毒胞子 (Poison Spore)", Pass::kSpecial, SpecialKind::kPoisonSpore, 7},
    {21, "cloud", "雲 (Cloud)", Pass::kSpecial, SpecialKind::kCloud, 7},
    {22, "gate_portal", "ゲートポータル (Gate Portal)", Pass::kSpecial, SpecialKind::kGatePortal, 7},
    {23, "stylized_terrain", "アニメ調地形 (Stylized Terrain)", Pass::kOpaque, SpecialKind::kNone, NoExtraInput},
    {24, "legacy_dash_panel", "ダッシュパネル (Dash Panel)", Pass::kOpaque, SpecialKind::kNone, NoExtraInput},
    {25, "slime_soft", "スライム補正 (Slime Soft)", Pass::kOpaque, SpecialKind::kNone, NoExtraInput},
    {26, "wind_orb", "風弾 (Wind Orb)", Pass::kSpecial, SpecialKind::kWindOrb, 7},
    {27, "prism_crystal", "プリズム結晶 (Prism Crystal)", Pass::kOpaque, SpecialKind::kNone, NoExtraInput},
    {28, "decal", "デカール (Decal)", Pass::kOpaque, SpecialKind::kNone, NoExtraInput},
    {29, "meadow_turf", "芝生面 (Meadow Turf)", Pass::kOpaque, SpecialKind::kNone, NoExtraInput},
    {30, "animated_grass", "揺れる草 (Animated Grass)", Pass::kOpaque, SpecialKind::kNone, NoExtraInput},
    {31, "animated_foliage", "揺れる樹木 (Animated Foliage)", Pass::kOpaque, SpecialKind::kNone, NoExtraInput},
}};
// 番号／IDが未登録なら nullptr。カタログは固定配列なので返す参照は再配置されない。
constexpr const LegacyMaterial* Find(std::int32_t type) noexcept {
    for (const auto& material : kLegacyMaterials)
        // 保存時の整数番号が一致した定義だけを返す。
        if (material.type == type) return &material;
    return nullptr;
}
// 保存IDまたは旧番号から定義を探す。未登録ならnullptr。
constexpr const LegacyMaterial* Find(std::string_view id) noexcept {
    for (const auto& material : kLegacyMaterials)
        // 文字列IDが一致した定義だけを返す。
        if (material.id == id) return &material;
    return nullptr;
}
// 未知の旧番号は通常の不透明パスへ分類する。特殊種別が不明な場合は None。
constexpr Pass GetPass(std::int32_t type) noexcept {
    const auto* material = Find(type);
    return material ? material->pass : Pass::kOpaque;
}
constexpr bool IsSpecial(std::int32_t type) noexcept { return GetPass(type) == Pass::kSpecial; }
constexpr SpecialKind GetSpecialKind(std::int32_t type) noexcept {
    const auto* material = Find(type);
    return material ? material->special : SpecialKind::kNone;
}
// 番号の連続性・ID重複・特殊パスとの整合をコンパイル時に検査する。
constexpr bool ValidCatalog() noexcept {
    for (std::size_t i = 0; i < kLegacyMaterials.size(); ++i) {
        // 番号と配列位置がずれる、IDが空、または特殊パスと専用種別が一致しない定義を拒否する。
        if (kLegacyMaterials[i].type != static_cast<std::int32_t>(i) || kLegacyMaterials[i].id.empty() ||
            (kLegacyMaterials[i].pass == Pass::kSpecial) != (kLegacyMaterials[i].special != SpecialKind::kNone)) return false;
        for (std::size_t j = 0; j < i; ++j)
            // 先に検査した定義と同じ文字列IDがあれば、同名で別の材質に解決されることを防ぐ。
            if (kLegacyMaterials[j].id == kLegacyMaterials[i].id) return false;
    }
    return true;
}
static_assert(ValidCatalog(), "Legacy material IDs must remain unique and stable");
} // namespace materialRenderPolicy
