#define NOMINMAX
#include <fstream>
#include "Model.h"
#include "DirectXCommon.h"
#include "engine/utility/math/Math.h"
#include <sstream>
#include <cassert>
#include <filesystem>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include "SrvManager.h"
#include <DebugConsole.h>
#include <LightManager.h>

namespace engine {




// ========================================================================
// Model アニメーション制御
// ------------------------------------------------------------------------
// ノードアニメーションの補間、ノード検索、スケルトンへの適用入口を
// まとめて管理する。
// ========================================================================

// 通常更新用の入口として、強制更新なしでモデルのアニメーションとボーン情報を更新する。
void Model::Update() {
    Update(false);
}

// 適用済みの関節姿勢からスケルトン行列とGPUボーンバッファを再計算する。
void Model::Update(bool force) {
    uint32_t currentFrame = DirectXCommon::GetInstance()->GetFrameCount();

    // 同じModelが一フレームの複数の描画経路から参照されても、通常は行列とボーンバッファを一度だけ更新します。
    if (!force && lastUpdateFrame_ == currentFrame) {
        return;
    }

    lastUpdateFrame_ = currentFrame;

    // 呼び出し元が選んだ関節姿勢を反映する。クリップの選択や時刻の進行はここでは行わない。
    UpdateSkeleton(modelData_.skeleton);

    // 関節の親子変換が最新になってから、GPUスキニングに使う行列を更新します。
    UpdateBoneBuffer();
}

// --- アニメーション計算用ヘルパー ---
// =========================================================
// 座標・スケール用 (Vector3) の補間
// =========================================================
// Vector3キーを現在時刻に合わせて線形補間し、位置やスケールの値を求める。
engine::Vector3 Model::CalculateValue(const std::vector<KeyframeVector3>& keyframes, float time) {
    // 座標キーが一件もなければ、補間結果としてゼロベクトルを返す。
    if (keyframes.empty()) {
        return { 0.0f, 0.0f, 0.0f };
    }

    // キーが1つだけ、または時間が最初のキーより前の場合
    if (keyframes.size() == 1 || time <= keyframes[0].time) {
        return keyframes[0].value;
    }

    // 時間の範囲を探す (線形探索)
    for (size_t i = 0; i < keyframes.size() - 1; ++i) {
        // 指定時刻を含む区間が見つかった時点で、先頭から末尾までの補間率を求める。
        if (time >= keyframes[i].time && time <= keyframes[i + 1].time) {
            // 0.0～1.0 の割合(t)を計算
            float t = (time - keyframes[i].time) / (keyframes[i + 1].time - keyframes[i].time);

            // 線形補間 (Lerp)
            engine::Vector3 result;
            result.x = std::lerp(keyframes[i].value.x, keyframes[i + 1].value.x, t);
            result.y = std::lerp(keyframes[i].value.y, keyframes[i + 1].value.y, t);
            result.z = std::lerp(keyframes[i].value.z, keyframes[i + 1].value.z, t);
            return result;
        }
    }

    // 時間が最後のキーを超えている場合は、最後の値を返す
    return keyframes.back().value;
}

// =========================================================
// 回転用 (Quaternion) の補間
// =========================================================
// Quaternionキーを現在時刻に合わせて球面線形補間し、回転値を求める。
engine::Quaternion Model::CalculateValue(const std::vector<KeyframeQuaternion>& keyframes, float time) {
    // 回転キーを持たない場合は、回転なしを示す単位クォータニオンを返す。
    if (keyframes.empty()) {
        return { 0.0f, 0.0f, 0.0f, 1.0f }; // 単位クォータニオン
    }

    // キーが1つ、または時間が最初より前
    if (keyframes.size() == 1 || time <= keyframes[0].time) {
        return keyframes[0].value;
    }

    // 時間の範囲を探す
    for (size_t i = 0; i < keyframes.size() - 1; ++i) {
        // 前後の回転キーが指定時刻を挟む場合だけ、その2姿勢を球面補間する。
        if (time >= keyframes[i].time && time <= keyframes[i + 1].time) {
            float t = (time - keyframes[i].time) / (keyframes[i + 1].time - keyframes[i].time);

            // 球面線形補間 (Slerp)
            return engine::Math::Slerp(keyframes[i].value, keyframes[i + 1].value, t);
        }
    }

    // 補間区間が見つからなければ、末尾の回転を維持する。
    return keyframes.back().value;
}

// ノード階層から指定名のノードを再帰的に探し、アニメーション適用先を見つける。
Model::Node* Model::FindNode(Node& node, const std::string& name) {
    // 現在のノード名が一致した場合は、子の探索を行わずそのノードを返す。
    if (node.name == name)
        return &node;
    for (auto& child : node.children) {
        Node* result = FindNode(child, name);
        // 子階層から最初の一致が返ったら、そのノードをそのまま呼び出し側へ返す。
        if (result)
            return result;
    }

    return nullptr;
}

// 受け取ったクリップの指定秒を評価し、スケルトンの関節姿勢へ適用する。
void Model::ApplyAnimation(const Animation& animation, float time) {
    ApplyAnimationToSkeleton(modelData_.skeleton, animation, time);
}

// アニメーション名からAnimationデータを探し、見つからない場合はnullptrを返す。
const Model::Animation* Model::GetAnimation(const std::string& name) const {
    for (const auto& animation : modelData_.animations) {
        // クリップ名が一致した場合だけ、モデル所有のデータへの借用参照を返す。
        if (animation.name == name) {
            return &animation;
        }
    }

    // 見つからなければ nullptr
    return nullptr;
}

} // namespace engine
