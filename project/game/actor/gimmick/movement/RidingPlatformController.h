#pragma once
#include "game/system/cinematic/CinematicPlayer.h"
#include <string>
#include <vector>

class GimmickMovingFloor;
class BaseScene;
class BaseEnemy;

// 乗車開始と乗員の追従を担当する。床の軌道と敵の登場時刻はタイムラインから取得し、停止条件をこのクラスで評価する。
// 関連：床側がCinematicSequenceと開始条件を渡し、シーン内の登場Actorと乗員を接続する。
// 毎フレームの入口はUpdate。Resetは乗り場へ戻す内部処理。
class RidingPlatformController {
  public:
    // deltaTime は秒。前フレームの接地から乗員を確定し、タイムラインで床と乗員を進めます。
    void Update(GimmickMovingFloor& floor, float deltaTime);

  private:
    // タイムラインに有効なride.ferry信号があるとき、離脱中の停止と乗り場への呼び戻しに船用の条件を使う。
    bool ferry_ = false;
    float absentTime_ = 0.0f;

    // 敵の初期座標から床への移乗を補間する一時状態。attribute/mask は解放時の復元用です。
    struct Boarder {
        std::string name;
        engine::Vector3 origin{};
        engine::Vector3 offset{};
        float duration = 1.1f;
        float height = 6.0f;
        float elapsed = 0.0f;
        uint32_t attribute = 0, mask = 0;
        bool active = false, released = false;
        bool falling = false; // 高い島から降りる場合だけ、縦方向を加速落下にする。
    };

    // 指定名でリプレイ除外されていない最初のオブジェクトを敵として解決する。該当しなければnullptr。
    BaseEnemy* FindEnemy(BaseScene& scene, const std::string& name) const;

    // 床・敵・当たり判定を演出開始前の配置へ戻し、再乗車できる状態にする。
    void Reset(GimmickMovingFloor& floor, BaseScene& scene);

    CinematicSequence sequence_;
    CinematicPlayer player_;
    std::vector<Boarder> boarders_;
    std::string loadedPath_;
    engine::Vector3 start_{};
    bool ready_ = false, running_ = false, wasPlaying_ = false;
    bool riderWasDead_ = false;
};
