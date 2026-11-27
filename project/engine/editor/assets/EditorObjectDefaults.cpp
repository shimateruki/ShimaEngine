#include "engine/editor/assets/EditorObjectDefaults.h"
#include "Object3D.h"
#include "CollisionConfig.h"
#include "CollisionManager.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace {
// モデル名やクラス名を大小文字に左右されず照合できるよう、小文字へ変換します。
std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

// モデル名に地形として扱う語が含まれるか調べます。
bool ContainsGroundKeyword(const std::string& value) {
    return value.find("block") != std::string::npos ||
        value.find("floor") != std::string::npos ||
        value.find("ground") != std::string::npos ||
        value.find("platform") != std::string::npos ||
        value.find("terrain") != std::string::npos ||
        value.find("wall") != std::string::npos;
}

bool ShouldApplyGroundDefaults(engine::Object3D* object) {
    // 参照先が無い場合は衝突設定を調べない。
    if (!object) {
        return false;
    }

    const std::string className = ToLowerAscii(object->GetClassName());
    // 敵・ギミック・子メッシュなど、専用の衝突処理や個別設定を持つクラスは対象外にする。
    if (className == "enemy" || className == "player" || className == "gimmick" ||
        className == "item" || className == "gpuparticle" || className == "cinematiccamera" ||
        className == "spritecard" || className == "meshroot" || className == "meshpart") {
        return false;
    }

    const std::string modelName = ToLowerAscii(object->GetModelName());

    // モデルが未設定なら、名前から地形を判定できない。
    if (modelName.empty()) {
        return false;
    }

    // キャラクターとアイテムのモデルには地面用の既定値を付けない。
    if (modelName.rfind("characters/", 0) == 0 || modelName.rfind("item/", 0) == 0) {
        return false;
    }

    // 平面プリミティブとポータルは、地形と似た名前であっても除外する。
    if (modelName == "primitives/plane" || modelName.find("portal") != std::string::npos) {
        return false;
    }

    // ステージ素材、立方体、または地形を表す語があるモデルを候補とする。
    return modelName.rfind("stages/", 0) == 0 ||
        modelName == "primitives/cube" ||
        ContainsGroundKeyword(modelName);
}

}

void editorObjectDefaults::ApplyGroundCollision(engine::Object3D* object) {
    // 地形の候補に当てはまらないオブジェクトは設定を変更しない。
    if (!ShouldApplyGroundDefaults(object)) {
        return;
    }

    auto colliderConfig = object->GetColliderConfig();

    // コライダーの種類が未設定の場合だけ、軸に沿った箱を割り当てる。
    if (colliderConfig.type == engine::ColliderType::kNone) {
        colliderConfig.type = engine::ColliderType::kAabb;
        object->SetColliderConfig(colliderConfig);
    }

    // 衝突属性が0なら、既定の地面属性を補う。
    if (object->GetCollisionAttribute() == 0) {
        object->SetCollisionAttribute(engine::collisionCategories::Get("ground"));
    }

    // 衝突マスクが0なら、すべてのビットを許可する初期値を補う。
    if (object->GetCollisionMask() == 0) {
        object->SetCollisionMask(0xFFFFFFFFu);
    }
}
