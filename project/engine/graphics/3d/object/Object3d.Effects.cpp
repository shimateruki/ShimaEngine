#define NOMINMAX
#include <cstdint>
#include "Object3D.h"
#include <numbers>
#include "DirectXCommon.h"
#include "ModelManager.h"
#include "EffectObject3D.h"
#include "SrvManager.h"
#include "CameraManager.h"
#include "SceneManager.h"
#include "GhostRecorder.h"
#include "CollisionManager.h"
#include <cassert>
#include <algorithm> // min, max
#include <ParticleManager.h>
#include <GpuParticleManager.h>
#include <GpuParticleEmitter.h>
#include <DebugConsole.h>
#include <ProfilerManager.h>
#include <fstream>
#include <filesystem>

namespace engine {



// Object3dに追従するMeshEffectを生成、更新、描画する処理を担当する。
// 歪み系エフェクトは描画順が重要なので、通常Drawとは分けて管理する。
// ========================================================================
void Object3D::UpdateAttachedEffects(float deltaTime) {
    // エフェクトの位置は本体のワールド位置に追従させ、本体や親の拡縮はエフェクトへ渡しません。
    effectAnchor_.translate = GetWorldPosition();
    effectAnchor_.scale = { 1.0f, 1.0f, 1.0f };

    // 最上位まで親をたどり、階層のうちルートの水平回転だけを取得する。
    float rootRotY = transform_.rotate.y;
    Object3D* rootObj = this;

    while (rootObj && rootObj->GetParent()) {
        rootObj = rootObj->GetParent();
    }

    // ルートの参照を取得できたときは、本体のローカル回転ではなくルートのヨーを使う。
    if (rootObj) {
        rootRotY = rootObj->GetRotation().y;
    }

    effectAnchor_.rotate = { 0.0f, rootRotY, 0.0f };
    effectAnchor_.UpdateMatrix();

    // 変更された枠だけを作り直し、二つの枠の状態を独立して保持する。
    const auto refreshSlot = [this](const std::string& path, std::string& loadedPath,
                                    std::vector<std::unique_ptr<Object3D>>& effects) {
        // 空でない素材が同じパスですでに生成されていれば、既存の再生状態を保つ。
        if (!path.empty() && !effects.empty() && loadedPath == path) {
            return;
        }

        effects.clear();
        loadedPath.clear();
        // 今回の設定が空になった枠は、前の表示物を消した状態で終了する。
        if (path.empty()) {
            return;
        }

        // ファイルを開けない場合は枠を空に保ち、次の更新で再試行する。
        std::ifstream file(path);

        // 設定ファイルを開けた場合に限り、立体表示のモードを取得して必要枚数を作る。
        if (file.is_open()) {
            json j;
            file >> j;
            file.close();

            // 立体化の設定から面数を決め、各面をアンカー基準で配置する。
            std::int32_t volumeMode = j.contains("VolumeMode") ? (std::int32_t)j["VolumeMode"] : 0;
            std::int32_t numSpawns = (volumeMode == 2) ? 3 : (volumeMode == 1 ? 2 : 1);

            for (std::int32_t i = 0; i < numSpawns; ++i) {
                auto effect = std::make_unique<EffectObject3D>();
                effect->Initialize(common_);
                effect->SetName(path + "_" + std::to_string(i));

                // 個別の面でエフェクトの読込に成功した場合だけ、姿勢の補正とアンカーへの接続を行う。
                if (effect->LoadFromJson(path)) {
                    engine::Vector3 localRot = effect->GetRotation();
                    engine::Vector3 localPos = effect->GetTranslate();

                    // 交差表示の2枚目にだけX軸回りの90度を加え、1枚目と直交させる。
                    if (volumeMode == 1 && i == 1) {
                        localRot.x += (std::numbers::pi_v<float> * 0.5f); // 90度クロス
                    }
                    // 奥行きのある3枚表示では、回転から求めた正面に沿って各面をずらす。
                    else if (volumeMode == 2) {
                        float gap = 0.02f;
                        engine::Vector3 localZ;
                        // プリセットのピッチとヨーからローカル正面を求め、中央の面の前後に一枚ずつ離して重ねます。
                        localZ.x = sinf(localRot.y) * cosf(localRot.x);
                        localZ.y = -sinf(localRot.x);
                        localZ.z = cosf(localRot.y) * cosf(localRot.x);

                        // 2枚目は中央から正面側へ0.02単位ずらす。
                        if (i == 1) {
                            localPos.x += localZ.x * gap;
                            localPos.y += localZ.y * gap;
                            localPos.z += localZ.z * gap;
                        }
                        // 3枚目は同じ距離だけ背面側へずらし、中央の両側を覆う。
                        if (i == 2) {
                            localPos.x -= localZ.x * gap;
                            localPos.y -= localZ.y * gap;
                            localPos.z -= localZ.z * gap;
                        }
                    }

                    effect->SetTranslate(localPos);
                    effect->SetRotation(localRot);

                    // 本体のボーン階層から独立したアンカーに追従させる。
                    effect->SetTransformAnchor(&effectAnchor_);

                    // 読み込みとアンカー設定が済んだエフェクトだけを枠へ登録し、下の毎フレーム更新に引き渡します。
                    effects.push_back(std::move(effect));
                }
            }

            loadedPath = path;
        }
    };

    // 先に両方の枠を更新し、新しく読み込んだエフェクトもこのフレームから進めます。
    refreshSlot(GetMeshEffect1Name(), currentMeshEffect1_, attachedEffects1_);
    refreshSlot(GetMeshEffect2Name(), currentMeshEffect2_, attachedEffects2_);

    // 配列内のすべてのエフェクトを更新
    for (auto& effect : attachedEffects1_) {
        effect->Update(deltaTime);
    }

    for (auto& effect : attachedEffects2_) {
        effect->Update(deltaTime);
    }
}

void Object3D::DrawAttachedEffects(ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource) {
    // 本体の描画が抑止されている場合は、付属する両方のエフェクト枠も描かない。
    if (!GetIsRenderVisible())
        return;

    // 配列内のすべてのエフェクトを描画
    for (auto& effect : attachedEffects1_) {
        effect->Draw(pointLightResource, spotLightResource);
    }

    for (auto& effect : attachedEffects2_) {
        effect->Draw(pointLightResource, spotLightResource);
    }
}

} // namespace engine
