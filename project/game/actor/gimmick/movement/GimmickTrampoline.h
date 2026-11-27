#pragma once
#include "BaseGimmick.h"

class Player;

// 上から踏むと大ジャンプするジャンプ台ギミック
// 衝突時とプレイヤーの接地処理からTryBounceを使う。プレイヤーの上向き速度を変え、台は反発時の表示を更新する。
// 主な入口はInitialize、Update、OnCollision、TryBounce。
class GimmickTrampoline : public BaseGimmick {
  public:
    // 表示名とは独立した、既存シーンの保存IDと生成モデルです。
    static constexpr const char* kTypeId = "Trampoline";
    static constexpr const char* kModelPath = "Gimmicks/mushroom_trampoline";

    // 接地用の判定と、跳ね返りの強さを編集する項目を初期化する。
    void Initialize(engine::Object3DCommon* common, const std::string& modelName) override;

    // 保存済みの配置スケールを基準に、着地直後につぶれ、ばねで戻る。
    void Update(float deltaTime) override;

    // プレイヤーとの接触を確認し、プレイヤー側の法線へ反転してTryBounceへ渡す。
    bool OnCollision(engine::Object3D* other) override;

    // 複製後も専用の衝突・反発動作を保つ。
    std::unique_ptr<engine::Object3D> Clone() const override;

    // プレイヤーから見た上向きの接触法線を受け取る。降下・接地時に反発できた場合はtrue。
    bool TryBounce(Player* player, const engine::Vector3& playerGroundNormal);

  private:
    // 配置時の Transform を基準に揺らし、拡縮が累積しないようにする。
    engine::Vector3 authoredScale_ = {1.0f, 1.0f, 1.0f};
    engine::Vector3 authoredRotation_ = {};
    float squash_ = 0.0f;
    float squashVelocity_ = 0.0f;
    float wobbleTime_ = 0.0f;
    // 一度の着地で繰り返し反発しないように設ける、反発再許可までの残り秒数。
    float bounceCooldown_ = 0.0f;
    bool authoredTransformCaptured_ = false;
};
