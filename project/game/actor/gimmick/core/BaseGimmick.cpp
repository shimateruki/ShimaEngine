#include "BaseGimmick.h"
#include "ModelManager.h"
#include <typeinfo>

// モデルと基本のOBBを設定します。ギミック固有の判定形状や属性は派生側で設定できます。
void BaseGimmick::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    engine::Object3D::Initialize(common);

    ModelManager::GetInstance()->LoadModel(modelName);
    SetModel(modelName);

    // 派生側が判定を上書きするまで、各軸1のOBBを使う。
    engine::ColliderConfig config;
    config.type = engine::ColliderType::kObb;
    config.size = {1.0f, 1.0f, 1.0f};
    SetColliderConfig(config);

    // 初期マスクは下位4ビットを対象とし、派生側が必要な衝突カテゴリへ変更する。
    SetCollisionAttribute(0b0010);
    SetCollisionMask(0b1111);

    SetClassName("Gimmick");
    SetSaveCategory("Object");
}

void BaseGimmick::Update(float deltaTime) {
    // 共通の Transform・描画・当たり判定更新を派生ギミックにも適用する。
    engine::Object3D::Update(deltaTime);
}

bool BaseGimmick::OnCollision(engine::Object3D* other) {
    // 基底では接触による状態変更を行わず、falseを返す。
    (void)other;
    return false;
}

std::unique_ptr<engine::Object3D> BaseGimmick::Clone() const {
    // 派生型を基底型として複製しない。各派生クラスが自分の複製を担当する。
    if (typeid(*this) != typeid(BaseGimmick)) {
        return nullptr;
    }

    // 基底型そのものなら、描画基盤とモデルで初期化してから配置と設定を写す。
    auto newObj = std::make_unique<BaseGimmick>();
    assert(GetObjectCommon() != nullptr);
    newObj->Initialize(GetObjectCommon(), GetModelName());
    newObj->CopyFrom(this);
    return newObj;
}
