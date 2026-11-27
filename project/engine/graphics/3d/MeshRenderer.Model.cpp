#include "MeshRenderer.h"
#include "ModelManager.h"

namespace engine {

void MeshRenderer::SetModel(Model* model) {
    // 直接指定したモデルを使い、描画だけの差し替えを解除する。
    visualModelOverride_ = nullptr;
    model_ = model;
    // ポインターで直接指定した場合、以前の名前でモデルを参照しないよう名前を破棄します。
    modelName_.clear();

    // モデルと描画用のマテリアル領域が揃っている場合だけ、素材の既定値を引き継ぐ。
    if (model_ && materialData_) {
        // モデルに主マテリアルがある場合だけ、色・粗さ・金属度・法線マップの有無を取り出す。
        if (const Model::MaterialData* material = model_->GetPrimaryMaterialData()) {
            SetColor(material->baseColorFactor);
            materialData_->roughness = material->roughness;
            materialData_->metallic = material->metallic;
            materialData_->enableNormalMap = material->hasNormalMap ? 1 : 0;
        }
    }
}

void MeshRenderer::SetModel(const std::string& modelName) {
    // 名前でモデルを指定し直すため、以前の描画差し替えは解除する。
    visualModelOverride_ = nullptr;
    modelName_ = modelName;
    // 名前からモデルを読み込んで保持する。
    model_ = ModelManager::GetInstance()->LoadModel(modelName);

    // 読み込み済みモデルと描画用のマテリアル領域が揃えば、主マテリアルの取得を試す。
    if (model_ && materialData_) {
        // 主マテリアルが取得できた場合だけ、描画用の初期値を設定する。
        if (const Model::MaterialData* material = model_->GetPrimaryMaterialData()) {
            SetColor(material->baseColorFactor);
            materialData_->roughness = material->roughness;
            materialData_->metallic = material->metallic;
            materialData_->enableNormalMap = material->hasNormalMap ? 1 : 0;
        }
    }
}

Model* MeshRenderer::ResolveDrawModel() const {
    // 描画差し替えが設定されている間はそちらを使い、元モデルへの参照は保持します。
    return visualModelOverride_ ? visualModelOverride_ : model_;
}

} // namespace engine
