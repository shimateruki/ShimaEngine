#define NOMINMAX
#include "game/actor/player/state/PlayerState.h"
#include "game/actor/player/Player.h"
#include "InputManager.h"
#include "DebugConsole.h"
#include "engine/graphics/3d/camera/CameraManager.h"
#include "engine/utility/math/Math.h"
#include "BaseEnemy.h"
#include "EnemyGiantSlime.h"
#include "GimmickHookPullBlock.h"
#include "CollisionConfig.h"
#include "CollisionManager.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>

// 拘束した敵を持ち運ぶ状態をまとめています。

void PlayerStateCarry::Enter(Player* player) {
    // 運搬者を取得できた場合だけ、操作を有効にして運搬アニメーションと抵抗演出を初期化する。
    if (player) {
        player->SetIsControlActive(true);
        player->SetSlimeAnimationMode(PlayerSlimeAnimator::Mode::kCarry);
        struggleTimer_ = 0.0f;

        engine::Object3D* enemy = player->GetCarriedEnemy();

        // 保持対象があれば、抵抗演出で伸縮する前のサイズを終了時の復元用に保存する。
        if (enemy) {
            carriedBaseScale_ = enemy->GetScale();
        }
    }
}

void PlayerStateCarry::Update(Player* player, float deltaTime) {
    // 運搬者がいなければ位置基準がないため、姿勢を更新しません。
    if (!player)
        return;

    struggleTimer_ += deltaTime;

    engine::Object3D* enemy = player->GetCarriedEnemy();

    // 保持中の敵がいるときだけ、プレイヤーの頭上に揺れを重ねます。
    if (enemy) {
        engine::Vector3 playerPos = player->GetWorldPosition();

        // プレイヤーの頭上を基準に、時間に応じた小さな位置の揺れを作る。
        float offsetX = std::sin(struggleTimer_ * 35.0f) * 0.15f;
        float offsetZ = std::cos(struggleTimer_ * 30.0f) * 0.15f;
        float offsetY = std::sin(struggleTimer_ * 45.0f) * 0.08f;

        enemy->SetTranslate({playerPos.x + offsetX, playerPos.y + 1.55f + offsetY,
                                            playerPos.z + offsetZ});

        // 運搬中の抵抗を見せるため、位置の揺れに体をひねる回転も重ねる。
        engine::Vector3 rot;
        rot.x = std::sin(struggleTimer_ * 20.0f) * 0.2f;

        // プレイヤーの現在のY軸回転を基準に、首振り角度をオフセットとして適用する
        rot.y = player->GetRotation().y + std::sin(struggleTimer_ * 15.0f) * 0.4f;
        rot.z = std::cos(struggleTimer_ * 22.0f) * 0.2f;
        enemy->SetEulerAngles(rot);

        // 保存したスケールを基準に、上下に伸びるときは横と奥行きを縮める。
        float stretch = std::sin(struggleTimer_ * 25.0f) * 0.05f;
        enemy->SetScale({carriedBaseScale_.x * (1.0f - stretch), carriedBaseScale_.y * (1.0f + stretch),
                                        carriedBaseScale_.z * (1.0f - stretch)});

        // このフレームで設定したオイラー角を反映できるよう、回転方式を切り替えて行列を確定する。
        enemy->SetQuaternionMaster(false);
        enemy->UpdateLocalMatrix();
        enemy->UpdateWorldMatrix();
    }

    // 投げ・吸収の入力はPlayer側が処理し、この状態では運搬姿勢の更新だけを担当する。
}

void PlayerStateCarry::Exit(Player* player) {
    // 運搬者が残っている場合は、保持対象の状態を戻してから参照を解放する。
    if (player) {
        engine::Object3D* enemy = player->GetCarriedEnemy();

        // 保持対象がまだ存在する場合だけ、演出前のスケールと通常の回転方式に戻す。
        if (enemy) {
            enemy->SetScale(carriedBaseScale_);
            enemy->SetEulerAngles({0.0f, 0.0f, 0.0f});
            enemy->SetQuaternionMaster(true); // クォータニオンモードに戻す
            enemy->UpdateLocalMatrix();
            enemy->UpdateWorldMatrix();
        }

        // 参照を解放する前に敵の姿勢を復元できるよう、解除は最後に行います。
        player->ReleaseCarriedEnemy(true);
    }
}
