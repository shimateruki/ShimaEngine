#pragma once
#include "Object3D.h"
#include <string>

// シーンに配置するギミックの共通モデル、衝突情報、保存時の種類を初期化する。
// Transformや標準の描画はObject3Dを使い、固有の接触・動作・複製は派生クラスが担当する。
// 主な入口：Initialize / Update / OnCollision
class BaseGimmick : public engine::Object3D {
  public:
    virtual ~BaseGimmick() = default;

    // 指定モデルを読み込み、既定形状と衝突の属性・マスクを設定する。
    virtual void Initialize(engine::Object3DCommon* common, const std::string& modelName);

    // 秒単位のdeltaTimeをObject3Dの共通更新へ渡す。
    virtual void Update(float deltaTime) override;

    // 基底の接触処理は状態を変更せずfalseを返す。
    virtual bool OnCollision(engine::Object3D* other) override;

    // 実体がBaseGimmickなら同じモデル・設定の複製を返す。派生型ならnullptrを返す。
    std::unique_ptr<engine::Object3D> Clone() const override;
};
