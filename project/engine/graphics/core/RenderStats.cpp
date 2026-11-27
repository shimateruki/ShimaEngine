#include "RenderStats.h"

#include <algorithm>

namespace engine {

RenderStats* RenderStats::GetInstance() {
    static RenderStats sInstance;
    return &sInstance;
}

void RenderStats::BeginFrame() {
    // 完了済みの集計とは別に、次のフレームを空の集計領域から始める。
    currentFrame_ = {};
    currentFrame_.frameNumber = nextFrameNumber_++;
    currentPass_ = RenderPass::kOther;
    frameActive_ = true;
}

void RenderStats::EndFrame() {
    // 計測フレームが開かれていなければ、前に確定した集計を保持する。
    if (!frameActive_) {
        return;
    }

    // Editor が描画中の半端な値を読まないよう、完了フレームを別領域に保持します。
    completedFrame_ = currentFrame_;
    currentPass_ = RenderPass::kOther;
    frameActive_ = false;
}

void RenderStats::SetCurrentPass(RenderPass pass) {
    // 範囲内のパスだけを記録先に採用し、集計配列で扱えない値は「その他」に振り分ける。
    currentPass_ = pass < RenderPass::kCount ? pass : RenderPass::kOther;
}

void RenderStats::RecordIndexedDraw(uint32_t indexCount, uint32_t instanceCount, uint32_t trianglesPerInstance) {
    // 描画呼び出しと推定三角形数を、現在の計測区間へ加算する。
    RenderPassStats* stats = GetCurrentPassStats();

    // 計測中のパスを取れない場合や、描くインデックスまたはインスタンスがない場合は数えない。
    if (!stats || indexCount == 0 || instanceCount == 0) {
        return;
    }

    const uint64_t instances = instanceCount;
    // 三角形数が未指定なら、三角形リストとして3インデックスで1枚と見積もる。
    const uint64_t triangles = trianglesPerInstance != 0 ? trianglesPerInstance : indexCount / 3u;
    ++stats->drawCalls;
    ++stats->indexedDrawCalls;
    stats->submittedIndices += static_cast<uint64_t>(indexCount) * instances;
    stats->submittedTriangles += triangles * instances;
    stats->submittedInstances += instances;
}

void RenderStats::RecordNonIndexedDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t trianglesPerInstance) {
    // インデックスを使わない描画も同じ計測区間へ加算する。
    RenderPassStats* stats = GetCurrentPassStats();

    // 計測中のパスを取れない場合や、描く頂点またはインスタンスがない場合は数えない。
    if (!stats || vertexCount == 0 || instanceCount == 0) {
        return;
    }

    const uint64_t instances = instanceCount;
    // 三角形数が未指定なら、三角形リストとして3頂点で1枚と見積もる。
    const uint64_t triangles = trianglesPerInstance != 0 ? trianglesPerInstance : vertexCount / 3u;
    ++stats->drawCalls;
    stats->submittedVertices += static_cast<uint64_t>(vertexCount) * instances;
    stats->submittedTriangles += triangles * instances;
    stats->submittedInstances += instances;
}

void RenderStats::RecordNonIndexedIndirectDraw() {
    // フレーム内で記録先を取得できる場合だけ集計する。GPUから頂点数を読み戻さないため発行回数だけを数える。
    if (RenderPassStats* stats = GetCurrentPassStats()) {
        ++stats->drawCalls;
        ++stats->indirectDrawCalls;
    }
}

void RenderStats::RecordComputeDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
    // 空のディスパッチは実際のGPU仕事量に含めない。
    RenderPassStats* stats = GetCurrentPassStats();

    // 計測先がなければ集計できない。いずれかの軸のグループ数が0の場合も発行として数えない。
    if (!stats || groupCountX == 0 || groupCountY == 0 || groupCountZ == 0) {
        return;
    }

    ++stats->computeDispatches;
    stats->computeThreadGroups +=
        static_cast<uint64_t>(groupCountX) * groupCountY * groupCountZ;
}

void RenderStats::RecordObjectDraw() {
    // 現在の描画パスを計測できる場合だけ、渡したオブジェクト数へ加える。
    if (RenderPassStats* stats = GetCurrentPassStats()) {
        ++stats->submittedObjects;
    }
}

void RenderStats::RecordCulledObject() {
    // 現在のパスに記録できる場合だけ、カリングで除外したオブジェクト数へ加える。
    if (RenderPassStats* stats = GetCurrentPassStats()) {
        ++stats->culledObjects;
    }
}

void RenderStats::RecordGpuParticleSystem(uint32_t capacity) {
    // フレーム外の登録は前後の計測結果へ混ぜない。
    if (!frameActive_) {
        return;
    }

    ++currentFrame_.gpuParticleSystems;
    currentFrame_.gpuParticleCapacity += capacity;
}

void RenderStats::RecordCpuParticles(uint32_t activeCount) {
    // フレーム計測中に報告された生存粒子だけ、CPU粒子数の合計へ加える。
    if (frameActive_) {
        currentFrame_.cpuParticleCount += activeCount;
    }
}

void RenderStats::RecordPostProcessPass() {
    // 計測中で、現在のパスがポスト処理の場合だけ、ポスト処理の実行回数を増やす。
    if (frameActive_ && currentPass_ == RenderPass::kPostProcess) {
        ++currentFrame_.postProcessPasses;
    }
}

void RenderStats::SetActiveLightCounts(uint32_t pointLights, uint32_t spotLights) {
    // 描画中に適用したライト数だけを当該フレームの値として残す。
    if (!frameActive_) {
        return;
    }

    currentFrame_.activePointLights = pointLights;
    currentFrame_.activeSpotLights = spotLights;
}

const char* RenderStats::GetPassDisplayName(RenderPass pass) {
    // 計測した描画区間の種別をデバッグ画面へ表示する名称へ変換する。
    switch (pass) {
    case RenderPass::kMainScene: return "メイン3D";

    case RenderPass::kShadow: return "影";

    case RenderPass::kCameraPreview: return "カメラPreview";

    case RenderPass::kGameUi: return "ゲームUI";

    case RenderPass::kPostProcess: return "後処理";

    case RenderPass::kEditorOverlay: return "Editor補助";

    case RenderPass::kOther: return "その他";

    // 未登録のパスを渡された場合も、表示用の名前を返す。
    default: return "不明";
    }
}

RenderPassStats* RenderStats::GetCurrentPassStats() {
    // フレーム外で呼ばれた計測 API は無視し、次フレームへの混入を防ぎます。
    if (!frameActive_) {
        return nullptr;
    }

    const size_t passIndex = static_cast<size_t>(currentPass_);

    // パス番号が配列の範囲外であれば、記録先を渡さない。
    if (passIndex >= currentFrame_.passes.size()) {
        return nullptr;
    }

    return &currentFrame_.passes[passIndex];
}

// 親の計測区間を保存し、スコープを抜けるときに戻すことで区間を入れ子にできる。
ScopedRenderPass::ScopedRenderPass(RenderPass pass) {
    RenderStats* stats = RenderStats::GetInstance();
    previousPass_ = stats->GetCurrentPass();
    stats->SetCurrentPass(pass);
}

ScopedRenderPass::~ScopedRenderPass() {
    RenderStats::GetInstance()->SetCurrentPass(previousPass_);
}

} // namespace engine
