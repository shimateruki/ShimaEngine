#include "MoveStrategy2D.h"
#include "Player.h"
#include "InputManager.h"
#include "engine/utility/math/Math.h"

// ウィンドウの視点設定に関係なく、A・DキーをワールドX軸の横移動速度へ変換します。
engine::Vector3 MoveStrategy2D::CalculateVelocity(Player* player) {
    engine::Vector3 move = engine::Vector3::kZero;
    float moveSpeed = player->GetMoveSpeed();

    engine::InputManager* inputManager = player->GetInputManager();

    // 入力がまだ使えない場合は、横移動を要求しない。
    if (!inputManager) {
        return move;
    }

    // Aキーを押していれば、負のX方向の速度を要求する。
    if (inputManager->IsKeyPressed(DIK_A)) {
        move.x = -moveSpeed;
    }

    // Dキーを押していれば正のX方向を使う。Aキーも押している場合は右が優先される。
    if (inputManager->IsKeyPressed(DIK_D)) {
        move.x = moveSpeed;
    }

    return move;
}