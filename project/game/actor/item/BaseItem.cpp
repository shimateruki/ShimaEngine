#include "BaseItem.h"
#include <numbers>
#include "CollisionConfig.h"
#include "ModelManager.h"
#include "Player.h"
#include <cassert>

void BaseItem::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    engine::Object3D::Initialize(common);

    // アイテムの見た目は派生型ごとに選び、取得判定の土台だけを共通化します。
    ModelManager::GetInstance()->LoadModel(modelName);
    SetModel(modelName);

    // 保存時の分類を共通アイテムにし、具体的な種別は生成側が後で設定する。
    SetClassName("Item");
    SetSaveCategory("Object");
    SetItemType("");
    SetStatic(false);

    // プレイヤーとの接触だけを拾うトリガーにし、地形の物理衝突には参加させません。
    engine::ColliderConfig config;
    config.type = engine::ColliderType::kSphere;
    config.size = {1.0f, 1.0f, 1.0f};
    SetColliderConfig(config);
    SetCollisionRadius(1.0f);

    SetCollisionAttribute(engine::collisionCategories::Get("trigger"));
    SetCollisionMask(engine::collisionCategories::Get("player"));
}

void BaseItem::Update(float deltaTime) {
    // 取得済みのアイテムは同じフレーム中にも再取得できないようにします。
    if (isCollected_) {
        SetCollisionAttribute(0);
        SetCollisionMask(0);
        SetIsVisible(false);
        SetDead(true);
        return;
    }

    // 待機中は経過秒数に応じて回転し、フレームレートによる速度差をなくします。
    engine::Vector3 rotate = GetRotation();
    rotate.y += rotationSpeed_ * deltaTime;

    // 回転角が一周を超えた場合は、蓄積値を抑えるため2πラジアンだけ引く。
    if (rotate.y > (2.0f * std::numbers::pi_v<float>)) {
        rotate.y -= (2.0f * std::numbers::pi_v<float>);
    }

    SetRotation(rotate);

    engine::Object3D::Update(deltaTime);
}

// 対象がプレイヤーで実際に重なった時だけ、派生クラスの取得処理を呼びます。
bool BaseItem::OnCollision(engine::Object3D* other) {
    // 既に取得済みか接触相手が空なら、取得処理を呼ばない。
    if (isCollected_ || !other) {
        return false;
    }

    Player* player = dynamic_cast<Player*>(other);

    // プレイヤー型でない相手は、衝突属性が合っていても取得できない。
    if (!player) {
        return false;
    }

    engine::CollisionInfo info = CheckCollision(other);

    // 通知を受けても形状が実際に重なっていなければ、派生クラスの取得効果を発動させない。
    if (!info.isColliding) {
        return false;
    }

    // 接触条件を満たした場合だけ、報酬と取得状態の確定を派生型に任せる。
    Collect(player);
    return true;
}

void BaseItem::Collect(Player* player) {
    // 派生アイテムは効果を適用してから、この共通の取得確定処理を呼べます。
    (void)player;
    MarkCollected();
}

// 同じフレームの二重取得を防ぐため、取得直後に衝突と表示を無効化します。
void BaseItem::MarkCollected() {
    // 次の衝突通知より前に取得・不可視・判定無効を確定し、削除用の死亡状態も立てる。
    isCollected_ = true;
    SetCollisionAttribute(0);
    SetCollisionMask(0);
    SetIsVisible(false);
    SetDead(true);
}

std::unique_ptr<engine::Object3D> BaseItem::Clone() const {
    // 初期化で描画・判定資源を作り直してから、編集可能な設定値を引き継ぎます。
    auto newObj = std::make_unique<BaseItem>();
    assert(GetObjectCommon() != nullptr);
    newObj->Initialize(GetObjectCommon(), GetModelName());
    newObj->CopyFrom(this);
    return newObj;
}
