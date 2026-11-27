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
// Model スケルトン管理
// ------------------------------------------------------------------------
// ジョイントの生成、姿勢リセット、行列更新、エディタ用の姿勢操作を
// 担当する。
// ========================================================================

// モデルがスケルトンを持つかを返し、スキニング処理の有無を判断する。
bool Model::HasSkeleton() const {
    return !modelData_.skeleton.joints.empty();
}

// 読み取り専用でジョイント一覧を返し、エディタ表示などから姿勢情報を参照できるようにする。
const std::vector<Model::Joint>& Model::GetJoints() const {
    return modelData_.skeleton.joints;
}

// ジョイント名からインデックスを探し、存在しない場合は-1を返す。
std::int32_t Model::FindJointIndex(const std::string& name) const {
    auto it = modelData_.skeleton.jointMap.find(name);

    // 名前が登録されていなければ、無効な番号として-1を返す。
    if (it == modelData_.skeleton.jointMap.end()) {
        return -1;
    }

    return it->second;
}

// 指定ジョイントの現在のローカル姿勢を返す。
Model::QuaternionTransform Model::GetJointTransform(std::int32_t jointIndex) const {
    // 負の番号や範囲を超える番号には、既定初期化した姿勢を返す。
    if (jointIndex < 0 || jointIndex >= static_cast<std::int32_t>(modelData_.skeleton.joints.size())) {
        return {};
    }

    return modelData_.skeleton.joints[jointIndex].transform;
}

// 指定ジョイントのローカルトランスフォームを更新し、エディタで調整した姿勢を反映する。
bool Model::SetJointTransform(std::int32_t jointIndex, const QuaternionTransform& transform) {
    // 無効な番号を指定された場合は、現在のスケルトンを変更しない。
    if (jointIndex < 0 || jointIndex >= static_cast<std::int32_t>(modelData_.skeleton.joints.size())) {
        return false;
    }

    // 有効な関節のローカル行列を更新してから、親子の行列とGPU用の値へ反映する。
    Joint& joint = modelData_.skeleton.joints[jointIndex];
    joint.transform = transform;
    engine::Matrix4x4 mS = math_.MakeScaleMatrix(joint.transform.scale);
    engine::Matrix4x4 mR = math_.MakeRotateQuaternionMatrix(joint.transform.rotate);
    engine::Matrix4x4 mT = math_.MakeTranslateMatrix(joint.transform.translate);
    joint.localMatrix = math_.Multiply(mS, math_.Multiply(mR, mT));
    RebuildSkeletonForEditor();
    return true;
}

// 読み込み時のルートノードからスケルトンを作り直し、編集された姿勢を初期状態へ戻す。
void Model::ResetSkeletonPose() {
    modelData_.skeleton = CreateSkeleton(modelData_.rootNode);
    RebuildSkeletonForEditor();
}

// エディタでジョイントを直接編集した後、スケルトン行列とGPUバッファを再構築する。
void Model::RebuildSkeletonForEditor() {
    UpdateSkeleton(modelData_.skeleton);
    UpdateBoneBuffer();
}

// ノード情報からジョイントを作成し、親子関係と名前インデックスを登録する。
int32_t Model::CreateJoint(const Node& node, const std::optional<int32_t>& parent, std::vector<Joint>& joints) {
    Joint joint;
    joint.name = node.name;
    joint.localMatrix = node.localMatrix;
    
    engine::Math math;
    joint.skeletonSpaceMatrix = math.MakeIdentity4x4();
    joint.transform = node.transform;
    joint.index = int32_t(joints.size()); // 現在登録されている数をIndexに
    joint.parent = parent;

    joints.push_back(joint); // SkeletonのJoint列に追加

    for (const Node& child : node.children) {
        // 親の添字が必ず子より先になるように登録し、行列更新時に先頭から一度走査するだけで親を参照できるようにします。
        int32_t childIndex = CreateJoint(child, joint.index, joints);
        joints[joint.index].children.push_back(childIndex);
    }
    
    // 自身のIndexを返す
    return joint.index;
}

// ノード階層全体からスケルトン構造を生成し、各ジョイントの親子関係を整える。
Model::Skeleton Model::CreateSkeleton(const Node& rootNode) {
    Skeleton skeleton;
    skeleton.root = CreateJoint(rootNode, std::nullopt, skeleton.joints);
    
    // Joint名をキーにしてIndexを引けるMapを作る
    for (const Joint& joint : skeleton.joints) {
        skeleton.jointMap[joint.name] = joint.index;
    }
    
    return skeleton;
}

// 全ジョイントのローカル行列とスケルトン空間行列を親子順に更新する。
void Model::UpdateSkeleton(Skeleton& skeleton) {
    engine::Math math;

    // 親が先に登録されているため、一回の走査で合成済みの親行列を参照できる。
    for (Joint& joint : skeleton.joints) {
        if (joint.parent) {
            // 親がいれば親の行列を掛ける
            joint.skeletonSpaceMatrix = math.Multiply(joint.localMatrix, skeleton.joints[*joint.parent].skeletonSpaceMatrix);
        } else {
            // 親がいないのでlocalMatrixとskeletonSpaceMatrixは一致する
            joint.skeletonSpaceMatrix = joint.localMatrix;
        }
    }
}

// アニメーションカーブをジョイントへ適用し、現在時刻の姿勢を作成する。
void Model::ApplyAnimationToSkeleton(Skeleton& skeleton, const Animation& animation, float time) {
    engine::Math math;

    for (Joint& joint : skeleton.joints) {
        // アニメーション内から、ジョイント名に一致するカーブを探す。
        auto it = std::find_if(animation.nodeAnimations.begin(), animation.nodeAnimations.end(),
            [&joint](const NodeAnimation& na) { return na.name == joint.name; });
            
        // 対応するカーブが存在する関節だけ、現在の時刻からローカル姿勢を作り直す。
        if (it != animation.nodeAnimations.end()) {
            const NodeAnimation& rootNodeAnimation = *it;
            joint.transform.translate = CalculateValue(rootNodeAnimation.translate, time);
            joint.transform.rotate = CalculateValue(rootNodeAnimation.rotate, time);
            joint.transform.scale = CalculateValue(rootNodeAnimation.scale, time);
            
            engine::Matrix4x4 mS = math.MakeScaleMatrix(joint.transform.scale);
            engine::Matrix4x4 mR = math.MakeRotateQuaternionMatrix(joint.transform.rotate);
            engine::Matrix4x4 mT = math.MakeTranslateMatrix(joint.transform.translate);
            joint.localMatrix = math.Multiply(mS, math.Multiply(mR, mT));
        }
    }
}

// 2つのAnimationをJointごとに補間し、状態遷移中の急な姿勢変化を防ぎます。
void Model::ApplyBlendedAnimation(
    const Animation& fromAnimation,
    float fromTime,
    const Animation& toAnimation,
    float toTime,
    float blendWeight) {
    engine::Math math;
    const float weight = std::clamp(blendWeight, 0.0f, 1.0f);

    for (Joint& joint : modelData_.skeleton.joints) {
        const auto findNodeAnimation = [&joint](const Animation& animation) -> const NodeAnimation* {
            const auto it = std::find_if(
                animation.nodeAnimations.begin(), animation.nodeAnimations.end(),
                [&joint](const NodeAnimation& node) { return node.name == joint.name; });
            return it == animation.nodeAnimations.end() ? nullptr : &(*it);
        };

        const NodeAnimation* fromNode = findNodeAnimation(fromAnimation);
        const NodeAnimation* toNode = findNodeAnimation(toAnimation);

        // 開始側にも終了側にも対象のカーブが無い場合は、その関節の現在姿勢を維持する。
        if (!fromNode && !toNode) {
            continue;
        }

        auto sampleScale = [](const NodeAnimation* node, float time, const engine::Vector3& fallback) {
            return node && !node->scale.empty() ? CalculateValue(node->scale, time) : fallback;
        };

        auto sampleTranslate = [](const NodeAnimation* node, float time, const engine::Vector3& fallback) {
            return node && !node->translate.empty() ? CalculateValue(node->translate, time) : fallback;
        };

        auto sampleRotate = [](const NodeAnimation* node, float time, const engine::Quaternion& fallback) {
            return node && !node->rotate.empty() ? CalculateValue(node->rotate, time) : fallback;
        };

        // 開始クリップで省略された成分は現在姿勢を使い、終了クリップで省略された成分は開始側の値を保持します。
        const engine::Vector3 fromScale = sampleScale(fromNode, fromTime, joint.transform.scale);
        const engine::Vector3 fromTranslate = sampleTranslate(fromNode, fromTime, joint.transform.translate);
        const engine::Quaternion fromRotate = sampleRotate(fromNode, fromTime, joint.transform.rotate);
        const engine::Vector3 toScale = sampleScale(toNode, toTime, fromScale);
        const engine::Vector3 toTranslate = sampleTranslate(toNode, toTime, fromTranslate);
        const engine::Quaternion toRotate = sampleRotate(toNode, toTime, fromRotate);

        joint.transform.scale = {
            std::lerp(fromScale.x, toScale.x, weight),
            std::lerp(fromScale.y, toScale.y, weight),
            std::lerp(fromScale.z, toScale.z, weight)
        };

        joint.transform.translate = {
            std::lerp(fromTranslate.x, toTranslate.x, weight),
            std::lerp(fromTranslate.y, toTranslate.y, weight),
            std::lerp(fromTranslate.z, toTranslate.z, weight)
        };

        // 回転は球面補間で向きを補間し、位置や拡縮とは異なるクォータニオンの計算を使います。
        joint.transform.rotate = engine::Math::Slerp(fromRotate, toRotate, weight);

        const engine::Matrix4x4 scaleMatrix = math.MakeScaleMatrix(joint.transform.scale);
        const engine::Matrix4x4 rotateMatrix = math.MakeRotateQuaternionMatrix(joint.transform.rotate);
        const engine::Matrix4x4 translateMatrix = math.MakeTranslateMatrix(joint.transform.translate);
        joint.localMatrix = math.Multiply(scaleMatrix, math.Multiply(rotateMatrix, translateMatrix));
    }
}

} // namespace engine
