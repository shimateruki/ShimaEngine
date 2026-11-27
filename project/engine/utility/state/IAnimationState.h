#pragma once

class Player;

// プレイヤーの動作状態が持つ開始・毎フレーム更新・終了の共通契約。
// Playerが状態を所有して切り替え、PlayerStateの各実装が移動・攻撃・被弾などを担当する。
// 主な入口はEnter、Update、Exit。引数のPlayerは状態を適用する非所有参照。
class IAnimationState {
public:
    virtual ~IAnimationState() = default;

    // 状態へ入るとき一度呼ぶ。再生するアニメーションやその状態で使う値を準備する。
    virtual void Enter(Player* player) = 0;

    // 状態が続く間に呼ぶ。入力・時間・状態遷移の条件を評価する。
    // playerは借用参照、deltaTimeは経過秒数。状態を抜ける後片付けはExitへ分ける。
    virtual void Update(Player* player, float deltaTime) = 0;

    // 状態から抜けるとき一度呼ぶ。その状態が変更した表示や一時状態を必要に応じて片付ける。
    virtual void Exit(Player* player) = 0;
};
