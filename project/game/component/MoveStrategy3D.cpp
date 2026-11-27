#include "MoveStrategy3D.h"
#include "Player.h"
#include "InputManager.h"
#include "CameraManager.h"
#include "engine/utility/math/Math.h"
#include <cmath>

// カメラの水平前方と右方向を基準に、移動入力をワールド速度へ変換します。
engine::Vector3 MoveStrategy3D::CalculateVelocity(Player* player) {
    engine::Vector3 move = engine::Vector3::kZero;
    float moveSpeed = player->GetMoveSpeed();

    engine::InputManager* input = player->GetInputManager();
    engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();

    // 視点と入力の両方が揃うまでは、移動の基準を作れないため速度ゼロを返す。
    if (!camera || !input) {
        return move;
    }

    engine::Vector3 forward, right;

    {
        // 現在のメインカメラの視線を使う。ロックオン対象の位置から移動方向を直接求める処理は持たない。
        forward = camera->GetTargetPoint() - camera->GetEye();
        // 視点の上下角を移動速度へ入れないよう、前方を地面の平面へ投影する。
        forward.y = 0.0f;
        // ワールドの上方向と水平前方の外積を取ると、カメラから見た右向きが求まる。
        right = math_.Cross({0.0f, 1.0f, 0.0f}, forward);

        // 水平に投影した前方に十分な長さがあれば、視点の傾きで移動量が変わらないよう単位化する。
        if (math_.Length(forward) > 0.001f) {
            forward = math_.Normalize(forward);
        }

        // 右方向も十分な長さがある場合だけ単位化する。
        if (math_.Length(right) > 0.001f) {
            right = math_.Normalize(right);
        }
    }

    // 前進アクションを押していれば、カメラの水平前方を加える。
    if (input->IsActionPressed("Forward")) {
        move += forward;
    }

    // 後退アクションを押していれば、カメラの水平前方と逆向きを加える。
    if (input->IsActionPressed("Backward")) {
        move += (forward * -1.0f);
    }

    // 左移動アクションを押していれば、カメラから見て左を加える。
    if (input->IsActionPressed("Left")) {
        move += (right * -1.0f);
    }

    // 右移動アクションを押していれば、カメラから見て右を加える。
    if (input->IsActionPressed("Right")) {
        move += right;
    }

    // 左スティックを確認し、アナログ入力が無ければ合成済みのデジタル入力を使う。
    const engine::Vector2 stick = input->GetLeftStick();
    const float stickLength = std::sqrt(stick.x * stick.x + stick.y * stick.y);

    // スティックの倒し量が十分ならアナログを優先し、Xを右、Yを前へ配分して速度に換算する。
    if (stickLength > 0.001f) {
        return (right * stick.x + forward * stick.y) * moveSpeed;
    }

    // デジタル入力が打ち消し合っていない場合は、斜めでも同じ速さになるよう単位化する。
    if (math_.Length(move) > 0.001f) {
        move = math_.Normalize(move) * moveSpeed;
    }

    return move;
}
