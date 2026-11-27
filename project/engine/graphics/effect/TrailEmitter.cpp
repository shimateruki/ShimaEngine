#include "TrailEmitter.h"
#include "Object3D.h"
#include "MeshEffectManager.h"
#include "GpuParticleManager.h"
#include <cmath>

// 軌跡の追従を始めます。次回の更新で新しい始点を記録します。
void TrailEmitter::Start(engine::Object3D* target) {
    target_     = target;
    isActive_   = true;
    hasLastPos_ = false;
}

// 追従を停止して、再開時に前の移動距離を持ち越さないよう履歴を消します。
void TrailEmitter::Stop() {
    isActive_   = false;
    hasLastPos_ = false;
}

// 最後に発生判定を通過した位置からの直線距離を測り、しきい値に達した位置までの中点へ軌跡を置きます。
void TrailEmitter::Update(float /*deltaTime*/) {
    // 停止中か追従先が無ければ、現在位置を調べない。
    if (!isActive_ || !target_)
        return;
    // メッシュと粒子の両方が無効なら、発生判定も行わない。
    if (!config_.emitMesh && !config_.emitParticle)
        return;

    engine::Vector3 currentPos = target_->GetWorldPosition();

    // 開始後の最初の更新では、距離の基準となる位置だけを記録する。
    if (!hasLastPos_) {
        lastEmitPos_ = currentPos;
        hasLastPos_  = true;
        return;
    }

    engine::Vector3 diff = currentPos - lastEmitPos_;
    float distance = engine::Math::Length(diff);

    // 最後に発生させた位置からの距離がしきい値未満なら、その基準を維持して次の更新を待つ。
    if (distance < config_.emitDistance)
        return;

    // 向きの自動設定が有効なら、水平面での移動方向をメッシュのヨーに使う。
    engine::Vector3 moveDir = engine::Math::Normalize(diff);
    float rotY = atan2f(moveDir.x, moveDir.z);
    engine::Vector3 effectRot = config_.autoOrient ? engine::Vector3{ 0.0f, rotY, 0.0f } : engine::Vector3::kZero;

    // 最後の発生位置と現在位置の中間へ、今回の見た目を置く。
    engine::Vector3 spawnPos = {
        (lastEmitPos_.x + currentPos.x) * 0.5f,
        (lastEmitPos_.y + currentPos.y) * 0.5f,
        (lastEmitPos_.z + currentPos.z) * 0.5f
    };

    // メッシュ発生が有効でプリセット名が指定されている場合に、対応するJSONを発生先へ渡す。
    if (config_.emitMesh && !config_.meshEffectPreset.empty()) {
        std::string path = "Resources/json/effect/" + config_.meshEffectPreset + ".json";
        MeshEffectManager::GetInstance()->SpawnEffectAt(path, spawnPos, effectRot, config_.scale);
    }

    // GPU粒子が有効でプリセット名が指定されている場合に、発生位置を平行移動に使って放出する。
    if (config_.emitParticle && !config_.gpuParticlePreset.empty()) {
        engine::Matrix4x4 emitMat       = engine::Math::MakeIdentity4x4();
        emitMat.m[3][0] = spawnPos.x;
        emitMat.m[3][1] = spawnPos.y;
        emitMat.m[3][2] = spawnPos.z;
        GpuParticleManager::GetInstance()->Emit(config_.gpuParticlePreset, spawnPos, emitMat);
    }

    // 次の距離はエフェクトを置いた中点ではなく、このフレームの対象位置から測ります。
    lastEmitPos_ = currentPos;
}
