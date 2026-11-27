#include <cstdint>
#include "ParticleSystem.h"
#include "RenderStats.h"
#include "ParticleCommon.h"
#include "DirectXCommon.h"
#include "TextureManager.h"
#include "CameraManager.h"
#include "SrvManager.h"
#include "engine/utility/math/Math.h"
#include <cassert>
#include <chrono>
#include <string>
#include <numbers>

// 粒子の軸別のばらつきに使う、-1から1までの乱数分布。
static std::uniform_real_distribution<float> dis(-1.0f, 1.0f);
static engine::Math sMath;

// 担当：粒子の描画矩形一頂点分の位置とUV。
// 関連：ParticleSystemの内部処理だけで使う補助型。
struct ParticleVertex {
    engine::Vector4 position;
    engine::Vector2 texcoord;
};

// 描画基盤を借用し、画像とGPUバッファを用意する。連続放出は停止した状態で始める。
void engine::ParticleSystem::Initialize(engine::ParticleCommon* common, const std::string& texturePath) {
    assert(common);
    common_ = common;

    // プリセットに画像が指定されていない場合は、初期化時に受け取った画像パスを使う。
    if (params_.textureName.empty()) {
        params_.textureName = texturePath;
    }

    SetTexture(params_.textureName);

    // 画像のハンドルと描画用バッファを用意してから、CPU粒子の格納領域と乱数を初期化する。
    assert(textureHandle_ != 0);
    CreateResources();
    particles_.reserve(kMaxParticles);
    std::random_device seedGenerator;
    randomEngine_.seed(seedGenerator());

    // 連続放出の端数を消し、発生を開始する操作を待つ。
    spawnTimer_ = 0.0f;
    params_.isEmitting = false;
}

float engine::ParticleSystem::ResolveEmissionScale(const EmitterParams& params, const engine::Vector3& origin) const {
    // LODが有効なときだけカメラ距離から放出量の倍率を評価する。
    if (!params.lod.enabled) {
        return 1.0f;
    }

    const engine::Camera* camera = CameraManager::GetInstance()->GetActiveCamera();

    // 視点の距離を取れない場合は、LODによる放出量の減少を適用しない。
    if (!camera) {
        return 1.0f;
    }

    const engine::Vector3 eye = camera->GetEye();
    const float x = origin.x - eye.x;
    const float y = origin.y - eye.y;
    const float z = origin.z - eye.z;
    return params.lod.EvaluateEmissionScale(std::sqrt(x * x + y * y + z * z));
}

std::size_t engine::ParticleSystem::ResolveParticleLimit(const EmitterParams& params) const {
    // LODに正の上限指定がなければ、生成上限はGPUへ転送できる固定の粒子数とする。
    if (!params.lod.enabled || params.lod.maxAliveParticles <= 0) {
        return static_cast<std::size_t>(kMaxParticles);
    }

    return static_cast<std::size_t>(std::clamp(params.lod.maxAliveParticles, 1, kMaxParticles));
}

/// <summary>
/// 連続放出の設定を使い、エディタープレビューのオフセット込みの位置から1個の生成を試みる。
/// </summary>
void engine::ParticleSystem::SpawnFromEmitter() {
    SpawnFromParams(params_, params_.spawnPosition + editorPreviewOffset_);
}

void engine::ParticleSystem::SpawnFromParams(const EmitterParams& params, const engine::Vector3& origin) {
    // このエミッターの同時生存数に達していれば、新しい粒子を生成しない。
    if (particles_.size() >= ResolveParticleLimit(params))
        return;

    // 出現形状に応じて発生位置と初速を決める。粒子の共通属性は分岐の後で埋める。
    engine::Vector3 pos = origin;
    engine::Vector3 vel = params.initialVelocity;

    if (params.emitterType == EmitterType::kBox) {
        // Boxは基準位置の各軸へ独立した乱数を加える。
        pos.x += params.spawnArea.x * dis(randomEngine_);
        pos.y += params.spawnArea.y * dis(randomEngine_);
        pos.z += params.spawnArea.z * dis(randomEngine_);

        // Boxの速度だけは初速の各軸へ乱数成分を追加する。
        vel.x += params.velocityRandomness.x * dis(randomEngine_);
        vel.y += params.velocityRandomness.y * dis(randomEngine_);
        vel.z += params.velocityRandomness.z * dis(randomEngine_);
    } else if (params.emitterType == EmitterType::kSphere) {
        // Sphereは乱数方向を正規化して球面上の位置を決め、外向きへ飛ばす。
        engine::Vector3 dir = { dis(randomEngine_), dis(randomEngine_), dis(randomEngine_) };

        float length = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        // 一定の長さを持つ乱数方向は、球の半径を掛けられる単位方向へ揃える。
        if (length != 0.0f)
            dir = dir / length;
        // 乱数方向の長さが0なら、球面上の発生方向として上向きを使う。
        else dir = { 0, 1, 0 };

        pos += dir * params.spawnRadius;

        // initialVelocityの長さを速度量として使い、方向は球面法線へ置き換える。
        float speed = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
        // 初速の大きさを受け取っていない場合も、球面から速度1で広がるようにする。
        if (speed == 0)
            speed = 1.0f;

        vel = dir * speed;
    } else if (params.emitterType == EmitterType::kCone) {
        // Coneは上方向を基準にX/Zを傾けた方向を作る簡易的な放出形状。

        float angleRad = params.coneAngle * (std::numbers::pi_v<float> / 180.0f);
        float r = std::tan(angleRad); // 指定角度をX/Z方向の広がり幅へ変換する。

        engine::Vector3 dir;
        dir.x = dis(randomEngine_) * r;
        dir.z = dis(randomEngine_) * r;
        dir.y = 1.0f; // 上向き

        float length = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        // 方向の長さが0以外なら、円錐の軸へ傾けた方向を正規化する。
        if (length != 0)
            dir = dir / length;

        // 発生点は原点から同じ方向へ0～1の範囲で散らす。
        pos += dir * (dis(randomEngine_) * 0.5f + 0.5f);

        // 初速の大きさだけ維持し、向きは放出方向へ合わせる。
        float speed = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
        // 初速の大きさが0の場合は、円錐の放出速度として5を使う。
        if (speed == 0)
            speed = 5.0f;
        vel = dir * speed;

    }


    // 共通の寿命・色・サイズと、編集用カーブの焼き込み結果を粒子へ保存する。
    Particle p;
    p.position = pos;
    p.velocity = vel;
    p.lifeTime = params.particleLifetime;
    p.currentTime = 0.0f;
    p.startColor = params.startColor;
    p.endColor = params.endColor;
    p.startSize = params.startSize;
    p.endSize = params.endSize;
    p.acceleration = params.acceleration;
    p.hdrIntensity = params.hdrIntensity;

    // 編集用カーブは両方のキーが揃っている場合だけ有効にする。
    p.useAuthoringCurves =
        params.useAuthoringCurves &&
        !params.sizeOverLife.keys.empty() &&
        !params.colorOverLife.keys.empty();
    p.sizeOverLife = BakeVfxCurve(params.sizeOverLife, params.startSize);
    p.colorOverLife = BakeVfxGradient(params.colorOverLife, params.startColor);
    p.rotation = 0.0f;
    // 発生時に選んだ度毎秒の回転速度をラジアン毎秒に変換して保持する。
    float rotSpeedDeg = params.initialRotationSpeed + dis(randomEngine_) * params.rotationSpeedRandomness;
    p.rotationSpeed = rotSpeedDeg * (std::numbers::pi_v<float> / 180.0f);

    particles_.push_back(p);
}

/// <summary>
/// ワールド座標の一点から単発で発生させる。方向は省略でき、広がり角はラジアン、寿命は秒で指定する。
/// </summary>
void engine::ParticleSystem::SpawnParticles(const engine::Vector3& position, std::int32_t count,
    float initialSpeed, const engine::Vector3* direction, float spreadAngle,
    engine::Vector4 initialColor, engine::Vector4 endColor,
    float lifeTimeMin, float lifeTimeMax,
    float startSize, float endSize) {
    // カメラ距離等の放出倍率を先に適用し、上限数を越える前に粒子を止める。
    const float emissionScale = ResolveEmissionScale(params_, position);

    // LODで放出倍率がほぼ0になった場合は、一括の発生処理を行わない。
    if (emissionScale <= 0.0001f) {
        return;
    }

    // LODで調整した個数は、放出を許可された場合は最低1個にする。寿命は指定された秒数の範囲で選ぶ。
    const std::int32_t scaledCount = (std::max)(
        1, static_cast<std::int32_t>(std::lround(static_cast<float>(count) * emissionScale)));
    const std::size_t particleLimit = ResolveParticleLimit(params_);
    std::uniform_real_distribution<float> lifeDist(lifeTimeMin, lifeTimeMax);

    for (std::int32_t i = 0; i < scaledCount; ++i) {
        // 生成中に同時生存上限へ達した場合は、残りの生成を打ち切る。
        if (particles_.size() >= particleLimit) {
            break;
        }

        engine::Vector3 spawnDir;

        if (direction) { // 方向の指定があれば、その向きをばらつきの基準にする。
            spawnDir = *direction;

            // 角度の幅が正の場合だけ、指定された発生方向をX軸・Y軸へ揺らす。
            if (spreadAngle > 0.0f) {
                std::uniform_real_distribution<float> angleDist(-spreadAngle / 2.0f, spreadAngle / 2.0f);
                engine::Matrix4x4 rotX = sMath.MakeRotateXMatrix(angleDist(randomEngine_));
                engine::Matrix4x4 rotY = sMath.MakeRotateYMatrix(angleDist(randomEngine_));
                spawnDir = sMath.TransformNormal(spawnDir, rotX * rotY);
            }
        } else { // 方向を省略した場合は、三軸の乱数から向きを作る。
            std::uniform_real_distribution<float> distance(-1.0f, 1.0f);
            spawnDir = { distance(randomEngine_), distance(randomEngine_), distance(randomEngine_) };
        }

        // 乱数や入力から有効な向きを得られない場合は、上向きを既定値にする。
        if (sMath.Length(spawnDir) < 0.001f) {
            spawnDir = { 0.0f, 1.0f, 0.0f };
        }

        spawnDir = sMath.Normalize(spawnDir);

        // 発生時点の属性と、寿命中に評価するカーブを一粒子分作る。
        Particle p;
        p.position = position;
        p.velocity = spawnDir * initialSpeed;
        p.lifeTime = lifeDist(randomEngine_);
        p.currentTime = 0.0f;
        p.startColor = initialColor;
        p.endColor = endColor;
        p.startSize = startSize;
        p.endSize = endSize;
        p.rotation = 0.0f;
        p.acceleration = params_.acceleration;
        p.hdrIntensity = params_.hdrIntensity;

        // 回転速度は度からラジアンへ直してから積分に渡す。
        float rotSpeedDeg = params_.initialRotationSpeed + dis(randomEngine_) * params_.rotationSpeedRandomness;
        p.useAuthoringCurves =
            params_.useAuthoringCurves &&
            !params_.sizeOverLife.keys.empty() &&
            !params_.colorOverLife.keys.empty();
        p.sizeOverLife = BakeVfxCurve(params_.sizeOverLife, startSize);
        p.colorOverLife = BakeVfxGradient(params_.colorOverLife, initialColor);
        p.rotationSpeed = rotSpeedDeg * (std::numbers::pi_v<float> / 180.0f);
        particles_.push_back(p);
    }
}

void engine::ParticleSystem::EmitOneShot(const EmitterParams& params, const engine::Vector3& position) {
    // カメラ距離によるLOD倍率と粒子上限を反映して単発放出数を決める。
    const float emissionScale = ResolveEmissionScale(params, position);

    // 指定した発生位置でLOD倍率がほぼ0なら、プリセットからの発生を省く。
    if (emissionScale <= 0.0001f) {
        return;
    }

    const std::int32_t scaledCount = static_cast<std::int32_t>(std::lround(static_cast<float>(params.emitCount) * emissionScale));
    const std::int32_t count = std::clamp(scaledCount, 1, static_cast<std::int32_t>(ResolveParticleLimit(params)));
    const std::size_t particleLimit = ResolveParticleLimit(params);

    for (std::int32_t i = 0; i < count && particles_.size() < particleLimit; ++i) {
        SpawnFromParams(params, position);
    }
}

/// <summary>
/// 連続放出を時間に応じて進め、生存粒子の位置と描画用データを更新する。deltaTimeは秒。
/// </summary>
void engine::ParticleSystem::Update(float deltaTime) {
    const auto cpuStart = std::chrono::high_resolution_clock::now();
    deltaTime *= simulationTimeScale_;

    // 放出中で秒間レートが正の場合だけ、LODと経過秒数に応じて連続放出を進める。
    if (params_.isEmitting && params_.particlesPerSecond > 0.0f) {
        const engine::Vector3 emitterPosition = params_.spawnPosition + editorPreviewOffset_;
        const float emissionScale = ResolveEmissionScale(params_, emitterPosition);
        float particlesToSpawn = params_.particlesPerSecond * emissionScale * deltaTime;
        spawnTimer_ += particlesToSpawn;

        // 一フレームで複数個の発生時点を跨いだ場合も順に消費し、1個未満を次回へ残す。
        while (spawnTimer_ >= 1.0f) {
            spawnTimer_ -= 1.0f;
            SpawnFromEmitter();
        }
    }

    const engine::Camera* camera = CameraManager::GetInstance()->GetActiveCamera();

    // カメラがなければ連続放出までで止め、個々の粒子の寿命・姿勢とGPU転送は次回に回す。
    if (!camera) {
        const auto cpuEnd = std::chrono::high_resolution_clock::now();
        lastUpdateCpuTimeMs_ = std::chrono::duration<float, std::milli>(cpuEnd - cpuStart).count();
        return;
    }

    const engine::Matrix4x4& viewMatrix = camera->GetViewMatrix();
    const engine::Matrix4x4& projectionMatrix = camera->GetProjectionMatrix();

    // カメラ行列の転送先が用意できている場合だけ、ビュー射影行列を書き込む。
    if (matrixData_) {
        matrixData_->viewProjection = viewMatrix * projectionMatrix;
    }

    // ビュー行列の逆行列からカメラの回転を取り出し、粒子を視点へ向ける。
    engine::Matrix4x4 billboardMatrix = sMath.Inverse(viewMatrix);
    billboardMatrix.m[3][0] = 0.0f;
    billboardMatrix.m[3][1] = 0.0f;
    billboardMatrix.m[3][2] = 0.0f;
    billboardMatrix.m[3][3] = 1.0f;

    // このフレームにGPUへ渡す生存粒子数をゼロから数え直す。
    particleCount_ = 0;

    // 生存粒子だけ、寿命・色・大きさ・物理状態・GPUインスタンス行列を更新する。
    for (auto it = particles_.begin(); it != particles_.end(); ) {
        Particle& p = *it;
        p.currentTime += deltaTime;

        // 経過時間が寿命以上になった粒子は、そのフレームの描画データから外す。
        if (p.currentTime >= p.lifeTime) {
            it = particles_.erase(it);
            continue;
        }

        // 寿命の進行度 (0.0:生まれたて -> 1.0:死ぬ直前)
        float lifeRatio = p.currentTime / p.lifeTime;
        // 寿命に対する進行率が1を越えた場合は、補間終点へ丸める。
        if (lifeRatio > 1.0f)
            lifeRatio = 1.0f;

        // 編集カーブが有効ならそちらを使い、なければ開始色と終了色を補間する。
        engine::Vector4 currentColor = p.useAuthoringCurves
            ? p.colorOverLife.Evaluate(lifeRatio)
            : engine::Vector4{
                std::lerp(p.startColor.x, p.endColor.x, lifeRatio),
                std::lerp(p.startColor.y, p.endColor.y, lifeRatio),
                std::lerp(p.startColor.z, p.endColor.z, lifeRatio),
                std::lerp(p.startColor.w, p.endColor.w, lifeRatio)
            };

        // 発光倍率はRGBに掛け、透過に使うアルファは寿命カーブで決めた値を保つ。
        currentColor.x *= p.hdrIntensity;
        currentColor.y *= p.hdrIntensity;
        currentColor.z *= p.hdrIntensity;

        // 旧形式では寿命の進行率を10点のサイズ表の位置へ変換する。
        std::int32_t graphIndex = (std::int32_t)(lifeRatio * 9.0f);
        // 進行率から計算した添字が負なら、サイズ曲線の先頭を使う。
        if (graphIndex < 0)
            graphIndex = 0;
        // 添字が曲線の末尾を越えたら、最後の点を使う。
        if (graphIndex > 9)
            graphIndex = 9;

        // 新形式のカーブが有効ならそれを優先し、旧形式の固定10点へフォールバックします。
        float currentSize = p.useAuthoringCurves
            ? p.sizeOverLife.Evaluate(lifeRatio)
            : params_.sizeCurve[graphIndex];
        p.velocity.x += p.acceleration.x * deltaTime;
        p.velocity.y += p.acceleration.y * deltaTime;
        p.velocity.z += p.acceleration.z * deltaTime;

        // 加速度を速度へ加えた後、その速度で位置を積分する。
        p.position.x += p.velocity.x * deltaTime;
        p.position.y += p.velocity.y * deltaTime;
        p.position.z += p.velocity.z * deltaTime;

        // 回転速度を秒単位で積分し、ビルボード面内の向きに反映します。
        p.rotation += p.rotationSpeed * deltaTime;


        // 粒子面をカメラへ向けたまま、その面内だけで回転させる。
        engine::Matrix4x4 scaleMatrix = sMath.MakeScaleMatrix({ currentSize * p.baseScale.x, currentSize * p.baseScale.y, currentSize });

        // ビルボード回転より前に Z 回転を置き、常にカメラへ向いた面の中で回します。
        engine::Matrix4x4 rotateMatrix = sMath.MakeRotateZMatrix(p.rotation);

        engine::Matrix4x4 translateMatrix = sMath.MakeTranslateMatrix(p.position);

        // 行列の順序はサイズ、面内回転、カメラ向き、配置位置。
        engine::Matrix4x4 worldMatrix = scaleMatrix * rotateMatrix * billboardMatrix * translateMatrix;


        // CPU側に粒子が残っていても、GPUの固定インスタンス容量は越えない。
        if (particleCount_ < kMaxParticles) {
            instancingData_[particleCount_].world = worldMatrix;
            instancingData_[particleCount_].color = currentColor;

            particleCount_++;
        }

        ++it;
    }

    const auto cpuEnd = std::chrono::high_resolution_clock::now();
    lastUpdateCpuTimeMs_ = std::chrono::duration<float, std::milli>(cpuEnd - cpuStart).count();
}

void engine::ParticleSystem::Draw() {
    // 転送した描画対象が0個なら、描画時間を0にしコマンドを発行しない。
    if (particleCount_ == 0) {
        lastDrawCpuTimeMs_ = 0.0f;
        return;
    }

    const auto cpuStart = std::chrono::high_resolution_clock::now();

    ID3D12GraphicsCommandList* commandList = common_->GetDxCommon()->GetCommandList();
    engine::DirectXCommon* dxCommon = common_->GetDxCommon();
    dxCommon->StartGpuProfile("Particle CPU pass");
    common_->SetPipeline(commandList, params_.blendMode);

    // 共通の四角形メッシュと、Updateで詰めた粒子ごとの姿勢・色をインスタンス描画する。
    D3D12_VERTEX_BUFFER_VIEW vbvs[] = { vertexBufferView_, instancingBufferView_ };
    commandList->IASetVertexBuffers(0, 2, vbvs);
    commandList->IASetIndexBuffer(&indexBufferView_);

    commandList->SetGraphicsRootConstantBufferView(0, matrixResource_->GetGPUVirtualAddress());
    engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 1, textureHandle_);

    commandList->DrawIndexedInstanced(6, particleCount_, 0, 0, 0);
    dxCommon->EndGpuProfile("Particle CPU pass");
    engine::RenderStats::GetInstance()->RecordIndexedDraw(6, particleCount_, 2);
    engine::RenderStats::GetInstance()->RecordCpuParticles(particleCount_);

    const auto cpuEnd = std::chrono::high_resolution_clock::now();
    lastDrawCpuTimeMs_ = std::chrono::duration<float, std::milli>(cpuEnd - cpuStart).count();
}

// 一枚の四角形を共有し、粒子ごとのワールド行列と色を別バッファへ書く構成を作る。
void engine::ParticleSystem::CreateResources() {
    engine::DirectXCommon* dxCommon = common_->GetDxCommon();

    // 単位サイズの四角形と画像のUVを頂点バッファに用意する。
    vertexResource_ = dxCommon->CreateBufferResource(sizeof(ParticleVertex) * 4);
    vertexBufferView_ = { vertexResource_->GetGPUVirtualAddress(), sizeof(ParticleVertex) * 4, sizeof(ParticleVertex) };
    ParticleVertex* vertexData = nullptr;
    vertexResource_->Map(0, nullptr, reinterpret_cast<void**>(&vertexData));
    vertexData[0] = { { -0.5f, -0.5f, 0.0f, 1.0f }, { 0.0f, 1.0f } }; // 左下
    vertexData[1] = { { -0.5f,  0.5f, 0.0f, 1.0f }, { 0.0f, 0.0f } }; // 左上
    vertexData[2] = { {  0.5f, -0.5f, 0.0f, 1.0f }, { 1.0f, 1.0f } }; // 右下
    vertexData[3] = { {  0.5f,  0.5f, 0.0f, 1.0f }, { 1.0f, 0.0f } }; // 右上
    vertexResource_->Unmap(0, nullptr);

    // 四角形を三角形2枚として描く頂点順を登録する。
    indexResource_ = dxCommon->CreateBufferResource(sizeof(uint32_t) * 6);
    indexBufferView_ = { indexResource_->GetGPUVirtualAddress(), sizeof(uint32_t) * 6, DXGI_FORMAT_R32_UINT };
    uint32_t* indexData = nullptr;
    indexResource_->Map(0, nullptr, reinterpret_cast<void**>(&indexData));
    indexData[0] = 0;
    indexData[1] = 1;
    indexData[2] = 2;
    indexData[3] = 2;
    indexData[4] = 1;
    indexData[5] = 3;
    indexResource_->Unmap(0, nullptr);

    // 最大数までの粒子の描画姿勢と色をまとめて書き込む。
    instancingResource_ = dxCommon->CreateBufferResource(sizeof(ParticleForGpu) * kMaxParticles);
    instancingBufferView_ = { instancingResource_->GetGPUVirtualAddress(), sizeof(ParticleForGpu) * kMaxParticles, sizeof(ParticleForGpu) };
    // 毎フレーム粒子ごとの行列と色を書き込むため、インスタンス用バッファをマップしたまま保持する。
    instancingResource_->Map(0, nullptr, reinterpret_cast<void**>(&instancingData_));

    // 全粒子で共通のビュー射影行列の書き込み先をマップする。
    matrixResource_ = dxCommon->CreateBufferResource(sizeof(TransformationMatrix));
    matrixResource_->Map(0, nullptr, reinterpret_cast<void**>(&matrixData_));
}

// 表示中の粒子と放出の端数を破棄する。次の更新でも放出設定自体は引き継ぐ。
void engine::ParticleSystem::Clear() {
    particles_.clear();
    particleCount_ = 0;
    spawnTimer_ = 0.0f;
}

// 既存粒子と連続放出の端数を消し、プレビューを同じ乱数の最初から再現できるようにする。
void engine::ParticleSystem::ResetSimulation(uint32_t randomSeed) {
    Clear();
    randomEngine_.seed(randomSeed);
}

size_t engine::ParticleSystem::GetEstimatedMemoryBytes() const {
    // パーティクル配列と描画用リソースから診断表示向けの使用量を概算する。
    size_t bytes = particles_.capacity() * sizeof(Particle);
    ID3D12Resource* resources[] = {
        vertexResource_.Get(),
        indexResource_.Get(),
        instancingResource_.Get(),
        matrixResource_.Get()
    };

    for (ID3D12Resource* resource : resources) {
        // 作成済みのバッファだけを、GPUメモリの推定に含める。
        if (resource) {
            bytes += static_cast<size_t>(resource->GetDesc().Width);
        }
    }

    return bytes;
}

void engine::ParticleSystem::SetTexture(const std::string& texturePath) {
    // 設定上の画像名も更新し、TextureManagerから描画用ハンドルを取得する。
    params_.textureName = texturePath;

    textureHandle_ = engine::TextureManager::GetInstance()->Load(texturePath);
}

// ヒット位置に留まって短時間で透明になる、等間隔の光条を並べる。
void engine::ParticleSystem::SpawnPrimitiveHitEffect(const engine::Vector3& position) {
    std::uniform_real_distribution<float> angleDist(0.0f, std::numbers::pi_v<float>);
    float baseRotation = angleDist(randomEngine_);

    // ランダムな基準角を起点に、半回転の範囲へ4本を等間隔で並べる。
    std::int32_t lineCount = 4;

    for (std::int32_t i = 0; i < lineCount; ++i) {
        // 全体の粒子数が固定上限へ達した時点で、残りの線を生成しない。
        if (particles_.size() >= kMaxParticles)
            break;

        Particle p;
        p.position = position;
        p.velocity = { 0.0f, 0.0f, 0.0f }; // 固定
        p.lifeTime = 0.15f;
        p.currentTime = 0.0f;

        p.startColor = { 1.0f, 1.0f, 1.0f, 1.0f }; // 真っ白
        p.endColor = { 1.0f, 1.0f, 1.0f, 0.0f };

        p.startSize = 0.5f;
        p.endSize = 6.0f;   // 大きく広げる

        p.rotation = baseRotation + i * (std::numbers::pi_v<float> / lineCount);
        p.rotationSpeed = 0.0f;
        p.acceleration = { 0.0f, 0.0f, 0.0f };
        p.hdrIntensity = 1.5f;

        p.baseScale = { 0.04f, 1.0f }; // 細長く

        particles_.push_back(p);
    }

}

// 楕円の粒子を異なるZ回転で重ね、星型のヒット演出を作る。
void engine::ParticleSystem::SpawnStarHitEffect(const engine::Vector3& position) {
    // Z回転をランダムに (-π〜π)
    std::uniform_real_distribution<float> distRotate(
        -std::numbers::pi_v<float>, std::numbers::pi_v<float>);

    // 光条ごとの縦倍率をばらつかせ、同じ方向・長さが揃わないようにする。
    std::uniform_real_distribution<float> distScale(1.0f, 3.5f);

    constexpr std::int32_t kCount = 8;

    for (std::int32_t i = 0; i < kCount; ++i) {
        // 固定の容量を使い切った場合は、残りの星の光条を生成しない。
        if (particles_.size() >= kMaxParticles)
            break;

        Particle p;
        p.position    = position;
        p.velocity    = { 0.0f, 0.0f, 0.0f };
        p.lifeTime    = 0.25f;
        p.currentTime = 0.0f;

        p.startColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        p.endColor   = { 1.0f, 1.0f, 1.0f, 0.0f };

        p.startSize  = 1.5f;
        p.endSize    = 14.0f;

        p.rotation      = distRotate(randomEngine_);
        p.rotationSpeed = 0.0f;
        p.acceleration  = { 0.0f, 0.0f, 0.0f };
        p.hdrIntensity  = 5.0f;

        p.baseScale = { 0.04f, distScale(randomEngine_) };

        particles_.push_back(p);
    }
}

// 縦長の粒子を重ね、Yスケールのばらつきで斬撃の幅を出す。
void engine::ParticleSystem::SpawnSlashEffect(const engine::Vector3& position, float baseRotation) {
    // 各光条の長さを変えるため、縦倍率を1.5から4.0の範囲で選ぶ。
    std::uniform_real_distribution<float> distScale(1.5f, 4.0f);

    // 基準角と光条間の角度にラジアン単位の揺れを加える。
    std::uniform_real_distribution<float> distRotJitter(-0.4f, 0.4f);

    constexpr std::int32_t kCount = 3;

    for (std::int32_t i = 0; i < kCount; ++i) {
        // 固定の容量を使い切った時点で、残りの斬撃片を生成しない。
        if (particles_.size() >= kMaxParticles)
            break;

        Particle p;
        p.position    = position;
        p.velocity    = { 0.0f, 0.0f, 0.0f };
        p.lifeTime    = 0.28f;
        p.currentTime = 0.0f;

        p.startColor = { 0.8f, 0.9f, 1.0f, 1.0f };
        p.endColor   = { 0.2f, 0.3f, 1.0f, 0.0f };

        p.startSize  = 1.2f;
        p.endSize    = 12.0f;

        float randomY   = distScale(randomEngine_);
        p.baseScale     = { 0.06f, randomY }; // X:細い線, Y:ランダム長さ

        p.rotation      = baseRotation + (i * (std::numbers::pi_v<float> / 3.0f)) + distRotJitter(randomEngine_);
        p.rotationSpeed = 0.0f;
        p.acceleration  = { 0.0f, 0.0f, 0.0f };
        p.hdrIntensity  = 6.0f;

        particles_.push_back(p);
    }
}

#ifdef USE_IMGUI
void engine::ParticleSystem::BeginEditorSession() {
    // すでに編集セッションを開始していれば、退避した通常状態を上書きしない。
    if (editorSession_) return;
    // 通常再生を同じ粒子状態から再開できるよう、乱数の進みと連続放出の端数も退避する。
    savedEditorParams_ = params_; savedEditorRandom_ = randomEngine_;
    savedEditorScale_ = simulationTimeScale_; savedEditorTimer_ = spawnTimer_; savedEditorTexture_ = textureHandle_;
    // 通常の粒子を退避してから編集用の粒子をゼロにし、専用の初期乱数へ戻す。
    particles_.swap(savedEditorParticles_); ResetSimulation();
    params_.isEmitting = false; simulationTimeScale_ = 1; editorSession_ = true;
}
void engine::ParticleSystem::EndEditorSession() {
    // 編集セッションを開始していなければ、復元する通常状態もないので戻る。
    if (!editorSession_) return;
    // 編集中に生成した粒子を消し、通常時の粒子と設定を取り戻す。
    Clear(); particles_.swap(savedEditorParticles_);
    params_ = savedEditorParams_; randomEngine_ = savedEditorRandom_;
    simulationTimeScale_ = savedEditorScale_; spawnTimer_ = savedEditorTimer_; textureHandle_ = savedEditorTexture_;
    editorSession_ = false;
    // 経過時間0で更新し、アクティブカメラがあれば通常のGPU用インスタンスを組み直す。
    Update(0.0f);
}
#endif
