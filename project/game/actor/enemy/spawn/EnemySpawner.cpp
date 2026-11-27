#include <cstdint>
#include "game/actor/enemy/spawn/EnemySpawner.h"
#include "game/actor/enemy/spawn/EnemyFactory.h"

void EnemySpawner::Initialize(engine::Object3DCommon* common, const std::string& enemyType, float interval, std::int32_t maxCount) {
    // 配置物として初期化し、生成する敵の種類と通知間隔を秒で保持する。
    engine::Object3D::Initialize(common);
    common_ = common;
    enemyType_ = enemyType;
    spawnInterval_ = interval;

    // 最大生成数と内部カウンタを初期化する
    maxCount_ = maxCount;
    spawnCount_ = 0;
    timer_ = 0.0f;
}

void EnemySpawner::Update(float deltaTime) {
    // 正の上限があり、通知回数がその上限に達したら更新を打ち切る。0以下の設定では回数を制限しない。
    if (maxCount_ > 0 && spawnCount_ >= maxCount_) {
        return;
    }

    timer_ += deltaTime;

    // 経過秒が間隔に達したらこのフレームで一度だけSpawnを呼び、余った時間は持ち越さない。
    if (timer_ >= spawnInterval_) {
        timer_ = 0.0f;
        Spawn();
        spawnCount_++;
    }

    engine::Object3D::Update(deltaTime);
}

void EnemySpawner::Spawn() {
    // 生成は登録されたコールバックの担当です。通知先が無ければ何も行いません。
    if (onSpawnCallback_) {
        // 自分の配置座標をスポーン位置として使う
        engine::Vector3 spawnPos = engine::Vector3::kZero;

        // 配置用のTransformがあれば、その移動成分を通知位置に使う。なければ原点のまま通知する。
        if (GetTransform()) {
            spawnPos = GetTransform()->translate;
        }

        // シーン側へ「この場所に敵を出してほしい」と通知する
        onSpawnCallback_(spawnPos);
    }
}
