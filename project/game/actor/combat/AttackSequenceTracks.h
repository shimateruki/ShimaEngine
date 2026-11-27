#pragma once
#include <cstdint>
#include <algorithm>
#include "Easing.h"
#include <array>
#include <cmath>
#include <string>
#include <vector>

// 時刻はトラック開始からの秒。位置は術者の右・上・前、回転はラジアンです。
// 見た目と判定を別のトラックにすることで、表示中の一部分だけ攻撃判定を有効にできます。
struct AttackTransformKey {
    float time = 0;
    std::array<float, 3> position{0, 0, 0};
    std::array<float, 3> rotation{0, 0, 0};
    std::array<float, 3> scale{1, 1, 1};
    std::int32_t interpolation = 1; // 次のキーまで: 0=保持、1=線形、2=Smoothstep
};

/// <summary>攻撃中の表示、判定、体アニメーションを時間区間と条件付きで定義する。</summary>
struct AttackSequenceTrack {
    std::string name = "表示物";
    std::string type = "mesh_vfx"; // mesh_vfx / hitbox / body_animation
    std::string preset; // MeshEffect Editorで保存したJSONへのパス

    std::string condition = "always"; // 開始時に一度だけ評価します。
    std::int32_t conditionTrack = -1; // contact / no_contact / track_completed の参照元。終了済みの判定トラックのみ。

    float playbackSpeed = 1; // body_animation のクリップ秒 / シーケンス秒。
    float clipOffset = 0; // クリップの再生開始位置（秒）。
    float blendIn = 0.05f, blendOut = 0.05f; // 区間の前後で中立姿勢へ混ぜる秒数。
    bool loop = false;

    bool enabled = true;
    bool followOwner = true; // falseなら攻撃開始時の座標・向きを固定します。

    std::string motion = "keyframed"; // keyframed / aim_launch / homing
    float launchDelay = 0.5f, moveSpeed = 6.0f, turnDegreesPerSecond = 90.0f;
    float homingDuration = 1.0f, targetHeight = 0.7f;

    std::int32_t parentTrack = -1; // hitboxが追従する表示物。自身より前のmesh_vfxだけを参照します。

    float begin = 0, end = 1; // シーケンス内の有効区間 [begin,end)
    float radius = 0.5f; // hitboxの球半径。表示物のスケールと独立しています。
    float damageScale = 1;

    std::vector<AttackTransformKey> keys{AttackTransformKey{}};

    // 絶対時刻から直接評価するため、順再生とシークで補間結果が一致します。
    AttackTransformKey Evaluate(float localTime) const {
        // 区間外では端のキーを返し、区間内だけ前後キーの補間種別を適用する。
        // キーが無いトラックは、既定の姿勢を返す。
        if (keys.empty())
            return {};
        // 先頭キーの時刻までは、そのキーの姿勢を保持する。
        if (localTime <= keys.front().time)
            return keys.front();
        for (size_t i = 1; i < keys.size(); ++i) {
            // 右端のキーより後なら次の区間を調べる。
            if (localTime > keys[i].time)
                continue;
            const auto& a = keys[i - 1];
            const auto& b = keys[i];
            // 右端の時刻に達したら、そのキーの姿勢へ切り替える。前のキーが保持指定でも同じ。
            if (localTime == b.time)
                return b;
            float t = std::clamp((localTime - a.time) / (b.time - a.time), 0.0f, 1.0f);
            // 左のキーが保持指定なら、右のキーに届くまで左の姿勢を使う。
            if (a.interpolation == 0)
                t = 0;
            // 補間種別2の区間では、線形の経過割合を滑らかな立ち上がりと終わりに変換する。
            else if (a.interpolation == 2) t = Easing::SmoothStep(t);
            auto value = a;
            value.time = localTime;
            for (size_t axis = 0; axis < 3; ++axis) {
                value.position[axis] += (b.position[axis] - a.position[axis]) * t;
                value.rotation[axis] += (b.rotation[axis] - a.rotation[axis]) * t;
                value.scale[axis] += (b.scale[axis] - a.scale[axis]) * t;
            }
            return value;
        }
        // 末尾より後は、最後の姿勢を保持する。
        return keys.back();
    }

    bool Validate(float duration, std::string* error) const {
        // JSON由来の値を再生前に検査し、補間時のゼロ除算や範囲外参照を防ぐ。
        // エラーの出力先が渡された場合だけ、拒否した理由を格納する。
        auto fail = [&](const char* message) { if (error) *error = message; return false; };
        // 親トラックを指定しない値は-1まで許し、それより小さい参照は拒否する。
        if (parentTrack < -1)
            return fail("親Objectの参照番号が不正です。");
        // 対応している軌道名以外を拒否する。
        if (motion != "keyframed" && motion != "aim_launch" && motion != "homing")
            return fail("未対応の動作部品です。");
        // 弾道を持つ表示を親にして、当たり判定だけが別の軌道を持たないようにします。
        if (motion != "keyframed" && type != "mesh_vfx")
            return fail("発射・追尾は表示Objectへ設定してください。判定は親へ追従させます。");
        for (float value : {launchDelay, moveSpeed, turnDegreesPerSecond, homingDuration, targetHeight})
            // 軌道パラメーターは五つとも有限かつ0以上1000以下である必要がある。
            if (!std::isfinite(value) || value < 0 || value > 1000)
                return fail("動作部品の値が範囲外です。");
        // 発射時刻は開始以降かつ区間の長さ未満である必要がある。
        if (motion != "keyframed" && launchDelay >= end - begin)
            return fail("発射時刻はObjectの区間終了より前にしてください。");
        // トラックの種別は定義済みの三種類に限る。
        if (type != "mesh_vfx" && type != "hitbox" && type != "body_animation")
            return fail("未対応のトラック種別です。");
        // 起動方式は定義済みの文字列に限る。
        if (condition != "always" && condition != "grounded" && condition != "airborne" && condition != "target_in_range" &&
            condition != "contact" && condition != "no_contact" && condition != "track_completed")
            return fail("未対応のトラック条件です。");
        // アニメーションのサンプリングに使う速度と秒数が正常値か確認します。
        if (!std::isfinite(playbackSpeed) || playbackSpeed <= 0 || playbackSpeed > 10 ||
            !std::isfinite(clipOffset) || clipOffset < 0 || clipOffset > 300 ||
            !std::isfinite(blendIn) || blendIn < 0 || blendIn > 30 ||
            !std::isfinite(blendOut) || blendOut < 0 || blendOut > 30)
            return fail("アニメーションの速度・開始位置・ブレンド時間が不正です。");
        // 開始・終了は有限で、順序が正しく、シーケンスの長さに収まる必要がある。
        if (!std::isfinite(begin) || !std::isfinite(end) || begin < 0 || end <= begin || end > duration)
            return fail("トラック区間は0以上、開始より終了が後、攻撃時間以内にしてください。");
        // 半径は0より大きく100以下、ダメージ倍率は0以上100以下に制限する。
        if (!std::isfinite(radius) || radius <= 0 || radius > 100 || !std::isfinite(damageScale) || damageScale < 0 || damageScale > 100)
            return fail("判定半径またはダメージ倍率が不正です。");
        // 有効なメッシュまたはボディトラックは、使用するプリセットの指定を必要とする。
        if (enabled && (type == "mesh_vfx" || type == "body_animation") && preset.empty())
            return fail("表示物またはアニメーションのアセットが未指定です。");
        // キーの本数・時刻・各軸を検証してから、表示と衝突の両方で同じ補間を使います。
        // ローカルキーの数は1個以上128個以下に制限する。
        if (keys.empty() || keys.size() > 128)
            return fail("キーは1～128個です。");
        float previous = -1;
        for (const auto& key : keys) {
            // キーのローカル時刻は有限で、トラック区間内にあり、前のキーより後である必要がある。
            if (!std::isfinite(key.time) || key.time < 0 || key.time <= previous || key.time > end - begin)
                return fail("キー時刻は区間内で、重複せず昇順にしてください。");
            // キーの補間種別は0から2の範囲に限る。
            if (key.interpolation < 0 || key.interpolation > 2)
                return fail("補間方法が不正です。");
            for (size_t axis = 0; axis < 3; ++axis) {
                // 位置・回転は絶対値1000以下、拡縮は0より大きく100以下の有限値に限る。
                if (!std::isfinite(key.position[axis]) || std::abs(key.position[axis]) > 1000 ||
                    !std::isfinite(key.rotation[axis]) || std::abs(key.rotation[axis]) > 1000 ||
                    !std::isfinite(key.scale[axis]) || key.scale[axis] <= 0 || key.scale[axis] > 100)
                    return fail("キーの位置・回転・スケールが不正です。");
            }
            previous = key.time;
        }
        return true;
    }
};
