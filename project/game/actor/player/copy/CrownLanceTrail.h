#pragma once
#include <cstdint>
#include "CrownLanceSettings.h"
#include "EffectObject3D.h"
#include <memory>

// 王冠槍は等速直進するため、柄の発射位置と現在位置だけで連続した帯の両端を決められます。
// 関連：王冠の射出槍が位置を渡し、残像の帯を更新・描画する。
// 主な入口：Initialize / Update / Draw
class CrownLanceTrail {
public:
    // この槍の帯表示Objectを生成し、モデル・専用材質・初期回転を設定する。
    void Initialize(engine::Object3DCommon* common, const engine::Vector3& start,
                    const engine::Quaternion& rotation) {
        start_ = start;
        // 共通描画設定がなければ帯のモデルを初期化できないため、位置の記録だけで終了する。
        if (!common)
            return;
        mesh_ = std::make_unique<EffectObject3D>();
        mesh_->Initialize(common);
        // 材質を取得できない場合は表示物を破棄し、残像だけを省略します。
        if (!mesh_->GetMaterialData()) {
            mesh_.reset();
            return;
        }
        // 帯は表示専用にし、槍が決めた回転を使って判定とは独立させます。
        mesh_->SetModel("Effects/crown_lance_trail");
        mesh_->SetColliderType(engine::ColliderType::kNone);
        mesh_->SetQuaternionMaster(true);
        mesh_->SetQuaternion(rotation);
        mesh_->SetBlendMode(BlendMode::kNormal);
        mesh_->SetProceduralType(15); // Effect3d.PSの王冠槍専用の帯。
        mesh_->ResetTime();
        mesh_->SetIsVisible(false);
    }

    // dtは秒。開始点から現在位置までの帯を、槍の進行方向に合わせて更新する。
    void Update(const engine::Vector3& root, const CrownLanceSettings::Appearance& style,
                bool full, float fade, float deltaTime) {
        // 帯の表示物を用意できていない場合は、見た目の更新を省く。
        if (!mesh_)
            return;
        // 発射直後は実際に進んだ距離で長さを制限し、最大長を超えたら槍の後方だけを残します。
        const float weight = full ? 1.35f : 1.0f;
        const float length = (std::min)(engine::Math::Length(root - start_), style.trailLength * weight)
                           * (1.0f - fade);
        mesh_->SetIsVisible(length > 0.015f);
        // 帯の長さが0.015ワールド単位以下なら非表示にし、行列と材質を更新しない。
        if (length <= 0.015f)
            return;
        mesh_->SetTranslate(root);
        const float width = style.trailWidth * weight * (1.0f - 0.65f * fade);
        mesh_->SetScale({width, length, width});
        // 帯の変形は移動から求め、流れるノイズと消え方は専用材質へ渡します。
        auto& material = *mesh_->GetMaterialData();
        material.time += deltaTime;
        material.distortionStrength = style.trailNoise;
        material.dissolveFade = fade;
        material.color = {style.edgeColor[0], style.edgeColor[1], style.edgeColor[2], 1.0f - fade};
        mesh_->UpdateWorldMatrix(true);
    }

    void Draw(ID3D12Resource* point, ID3D12Resource* spot, engine::Camera* camera, std::int32_t bufferIndex) {
        // 帯が未生成、または短すぎて非表示なら描画命令を出さない。
        if (!mesh_ || !mesh_->GetIsVisible())
            return;
        // カメラが渡された描画先には専用の行列を使い、省略時は現在の描画カメラを取り直します。
        if (camera) {
            mesh_->DrawForCamera(camera, point, spot, bufferIndex);
        // 専用視点が指定されなければ、現在のシーン描画カメラで描く。
        } else {
            mesh_->RefreshRenderCameraData();
            mesh_->Draw(point, spot);
        }
    }

private:
    std::unique_ptr<EffectObject3D> mesh_;
    engine::Vector3 start_{};
};
