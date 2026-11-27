#pragma once
#include <cstdint>
#include "Object3D.h"
#include <functional>
#include <string>

namespace engine { class Object3DCommon; }

// 一定間隔で敵を発生させるための配置用オブジェクト
// 関連：生成先の座標を登録済みコールバックへ通知し、敵の生成と所有は通知先に任せる。
// 主な入口：Initialize / Update / SetOnSpawnCallback
class EnemySpawner : public engine::Object3D {
  public:
    /// <summary>
    /// 敵の種別名、通知間隔（秒）、最大通知回数を設定する。回数が0以下なら無制限。
    /// </summary>
    void Initialize(engine::Object3DCommon* common, const std::string& enemyType, float interval, std::int32_t maxCount);

    // 経過秒を進め、間隔に達して回数上限に余裕があれば通知を試みる。
    void Update(float deltaTime) override;

    using SpawnCallback = std::function<void(const engine::Vector3&)>;

    // 実際の敵生成はシーン側へ任せるため、座標だけコールバックで通知する
    void SetOnSpawnCallback(SpawnCallback callback) { onSpawnCallback_ = callback; }

  private:
    /// コールバックが設定されていれば、現在位置を生成先へ通知する。
    void Spawn();

  private:
    // 初期化時に受け取った共通描画情報。このクラスの通知には生成位置だけを渡す。
    engine::Object3DCommon* common_ = nullptr;

    std::string enemyType_;      // 生成したい敵タイプ名。
    float spawnInterval_ = 1.0f; // 次の通知までの間隔（秒）。
    float timer_ = 0.0f;         // 前回通知周期からの経過秒数。
    SpawnCallback onSpawnCallback_ = nullptr;
    std::int32_t maxCount_ = 0;   // 最大生成数。0以下なら無制限扱い。
    std::int32_t spawnCount_ = 0; // Spawnを呼んだ回数。生成そのものの成否は数えない。
};
