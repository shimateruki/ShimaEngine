#include "PlayerClone.h"
#include "engine/graphics/3d/model/ModelManager.h"

// 分身にワールド座標と初速を設定し、以後はCharacterの重力と衝突で動かします。
void PlayerClone::Initialize(engine::Object3DCommon* common, const engine::Vector3& position, const engine::Vector3& velocity) {
    Character::Initialize(common);
    // ゲーム側で通常プレイヤーと区別できるクラス名を登録する。
    SetClassName("PlayerClone");

    // Characterの物理更新が使う重力加速度と最大落下速度を設定する。
    SetGravity(50.0f);
    SetMaxFallSpeed(60.0f);

    // 通常スライムのモデルを表示に使う。
    SetModel("Characters/slime");

    // 生成元から渡された位置と初速で、出現直後の軌道を決めます。
    SetTranslate(position);
    SetVelocity(velocity);

    // 通常プレイヤーと区別できるよう、青みのある不透明な色を使う。
    SetColor({0.4f, 0.7f, 1.0f, 1.0f});

    SetScale({2.0f, 2.0f, 2.0f});

    // player属性を持たせ、player属性との接触だけを衝突マスクから外す。
    SetCollisionAttribute(engine::collisionCategories::Get("player"));
    SetCollisionMask(~engine::collisionCategories::Get("player"));


    // 渡された位置と初期拡縮を、最初の描画や衝突参照に使う行列へ反映する。
    UpdateLocalMatrix();
    UpdateWorldMatrix();
}

// 寿命を秒で進め、接地中は水平移動を止めたうえで物理更新します。
void PlayerClone::Update(float deltaTime) {
    // 寿命を使い切った分身は、この関数での移動と物理更新を止めます。
    if (lifetimeTimer_ <= 0.0f)
        return;

    // この更新中に残り時間がゼロをまたいでも、更新開始時に有効だった分身の物理は一度進める。
    lifetimeTimer_ -= deltaTime;

    // 地面への接地が確定していた場合は、初速の水平成分を残さない。
    if (IsGrounded()) {
        SetVelocityX(0.0f);
        SetVelocityZ(0.0f);
    }

    // 水平速度を決めた後、重力と移動はCharacterの更新へ渡す。
    Character::Update(deltaTime);
}
