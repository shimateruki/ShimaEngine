#include "RenderCommonResources.h"
#include "Object3DCommon.h"
#include "SpriteCommon.h"
#include "ParticleCommon.h"
#include "engine/graphics/scene/SceneRenderCommands.h"
#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace engine {
RenderCommonResources::RenderCommonResources() = default;
RenderCommonResources::~RenderCommonResources() = default;

void RenderCommonResources::Initialize(engine::DirectXCommon* dxCommon) {
    // DirectX基盤を借用するため、有効なポインターが必要。初期化済みの場合も既存資源の差し替えを防ぐため拒否する。
    if (!dxCommon || dxCommon_) {
        throw std::logic_error("RenderCommonResources requires one valid DirectX initialization");
    }
    sceneRenderCommands_ = std::make_unique<SceneRenderCommands>(*dxCommon);
    dxCommon_ = dxCommon;
}

// Spriteは全経路で必須。軽量・全体準備ではCPU粒子と、要求された範囲の3Dパイプラインの完了も待つ。
bool RenderCommonResources::IsReady(RenderPreparation preparation) const {
    // すべての準備範囲でSpriteが必要になるため、未作成なら準備中と返す。
    if (!spriteCommon_) return false;
    // Sprite専用のシーンでは、Spriteがあれば描画準備は完了。
    if (preparation == RenderPreparation::kSpritesOnly) return true;
    return particleCommon_ && (preparation == RenderPreparation::kMinimal ? minimalReady_ : fullReady_);
}

void RenderCommonResources::Prepare(RenderPreparation preparation) {
    // 同期で初期化する入口も、ロード画面と同じ準備処理を再利用する。
    while (!PrepareStep(preparation)) {}
}

bool RenderCommonResources::PrepareStep(RenderPreparation preparation) {
    assert(dxCommon_);
    // 要求された範囲を準備済みなら、共有資源を作り直さず終える。
    if (IsReady(preparation)) return true;

    // どの準備範囲でもSpriteを先に用意し、SpritesOnlyならこの段階で完了とします。
    if (!spriteCommon_) {
        auto common = std::make_unique<SpriteCommon>();
        common->Initialize(dxCommon_);
        spriteCommon_ = std::move(common);
        return preparation == RenderPreparation::kSpritesOnly;
    }
    // 要求がSprite専用であれば、3DやCPU粒子の準備を開始せず完了にする。
    if (preparation == RenderPreparation::kSpritesOnly) return true;

    // 3Dも必要な要求では、共有3D基盤の所有先を未生成時にだけ作る。
    if (!object3dCommon_) object3dCommon_ = std::make_unique<Object3DCommon>();
    // 軽量準備では、タイトルで使う範囲の3D資源を優先する。
    if (preparation == RenderPreparation::kMinimal) {
        // 軽量の3D初期化が済んでいない場合だけ、このフレームでその範囲を生成する。
        if (!minimalReady_) {
            // 既存の起動時軽量経路を利用し、不要な材質の一括生成を避ける。
            object3dCommon_->InitializeForTitle(dxCommon_);
            minimalReady_ = true;
            // 全体準備の途中で軽量経路へ切り替わった場合も、次回は不足分を再走査する。
            fullPreparationStarted_ = false;
            return false;
        }
    // 全体準備の要求なら、まだ揃っていない3D資源を分割して作る。
    } else if (!fullReady_) {
        // 全体準備をまだ始めていない場合は、3D初期化の走査を開始する。
        if (!fullPreparationStarted_) {
            object3dCommon_->BeginInitialize(dxCommon_);
            fullPreparationStarted_ = true;
        }
        // 3D資源が全段階揃うまでは、後続のCPU粒子を作らず次のフレームへ持ち越す。
        if (!object3dCommon_->InitializeStep()) return false;
        // 全材質が揃えば軽量要件も満たす。次の呼び出しでCPU粒子の準備へ進む。
        fullReady_ = true;
        minimalReady_ = true;
        return false;
    }

    // 軽量・全体のどちらでも3D側の要求範囲を満たしてから、共有のCPU粒子パイプラインを用意します。
    if (!particleCommon_) {
        auto common = std::make_unique<ParticleCommon>();
        common->Initialize(dxCommon_);
        particleCommon_ = std::move(common);
    }
    return IsReady(preparation);
}

float RenderCommonResources::GetPreparationProgress(RenderPreparation preparation) const {
    // 要求された資源がすべて利用可能になった場合は、進捗を100%とする。
    if (IsReady(preparation)) return 1.0f;
    // Sprite専用の未完了状態には中間段階がないので、進捗は0%とする。
    if (preparation == RenderPreparation::kSpritesOnly) return 0.0f;
    // 軽量準備は完了か未完了の二段階。全体準備は開始済みの場合だけ3D基盤の進捗を使う。
    float objectProgress = 0.0f;
    if (preparation == RenderPreparation::kMinimal) {
        objectProgress = minimalReady_ ? 1.0f : 0.0f;
    } else if (fullReady_) {
        objectProgress = 1.0f;
    } else if (fullPreparationStarted_) {
        objectProgress = object3dCommon_->GetInitializationProgress();
    }
    // Sprite、3D、CPU粒子の3枠を等分します。処理時間の割合ではなく準備段階の目安です。
    return std::clamp(((spriteCommon_ ? 1.0f : 0.0f) + objectProgress +
                       (particleCommon_ ? 1.0f : 0.0f)) / 3.0f, 0.0f, 1.0f);
}
} // namespace engine
