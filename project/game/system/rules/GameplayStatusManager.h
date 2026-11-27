#pragma once

#include "Object3D.h"
#include "game/actor/player/movement/PlayerLocomotion.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/// PlayerとEnemyのタイプ共通ステータスを一元管理します。
// 関連：JSONの共通数値をPlayer・BaseEnemyへ適用し、配置個体の設定とは分離する。
// 主な入口：Initialize / ApplyPlayerStatus / ApplyEnemyStatus / SaveIfDirty
class GameplayStatusManager {
  public:
    // キャラクター種ごとに全個体へ適用する基準値と移動モーターの設定。
    struct CharacterStatus {
        float maxHp = 100.0f;
        float attackPower = 1.0f;

        float speed = 1.0f;
        float gravity = 50.0f;
        float maxFallSpeed = 60.0f;
        float jumpPower = 10.0f;

        float detectionRange = 20.0f;

        engine::Vector3 scale = {1.0f, 1.0f, 1.0f};
        std::string modelName;

        bool morphLimited = true;
        // 時間制限付きの変身が継続できる秒数。
        float morphDuration = 5.0f;

        bool motorContinuousCollision = false;
        bool motorSnapToGround = false;
        // 歩行可能とみなす斜面の上限角度（度）。
        float motorMaxSlopeDegrees = 45.573f;
        // 衝突を避けながら乗り越える段差の高さ。
        float motorStepHeight = 0.0f;
        // 地面を探すための下向きの距離。
        float motorGroundProbeDistance = 0.18f;
        // 衝突判定で接触面から離す余裕幅。
        float motorSkinWidth = 0.025f;
    };

    // 共有設定マネージャーのインスタンスを返す。
    static GameplayStatusManager* GetInstance();

    // 全体または敵タイプごとの未保存の調整値を終了時に保存する。
    ~GameplayStatusManager();

    /// 初回だけ設定JSONを読み込みます。
    void Initialize();

    /// 設定を再読込します。共有設定と敵別ファイルの読み込みが揃えば true、失敗時は以前の設定値を戻します。
    bool Reload();

    // 敵ごとのファイルを先に書いた後、プレイヤーの共通設定を保存する。全件成功でtrueを返す。
    bool Save();

    // プレイヤー・操作応答・全敵タイプを標準値に戻し、再保存が必要な状態にする。
    void ResetToDefaults();

    // 可変参照を編集した呼び出し側は MarkDirty() を呼び、後で SaveIfDirty() する。
    CharacterStatus& GetMutablePlayerStatus() { return playerStatus_; }

    const CharacterStatus& GetPlayerStatus() const { return playerStatus_; }

    // 通常移動の加減速はPlayer共通値として管理し、敵の移動には適用しない。
    PlayerLocomotionSettings& GetMutablePlayerLocomotion() { return playerLocomotion_; }
    const PlayerLocomotionSettings& GetPlayerLocomotion() const { return playerLocomotion_; }

    // 未登録の敵種別なら nullptr。可変参照の編集後は MarkDirty を呼ぶ。
    CharacterStatus* FindMutableEnemyStatus(const std::string& enemyType);

    // 未登録の敵タイプはnullptrで知らせ、別タイプの値を誤適用しない。
    const CharacterStatus* FindEnemyStatus(const std::string& enemyType) const;

    // メモリ上の登録済み敵タイプ名を名前順に返す。
    std::vector<std::string> GetEnemyTypes() const;

    // 共通数値を未保存として記録する。実際の保存はSaveIfDirtyなどの入口で行う。
    void MarkDirty() { dirty_ = true; }

    // 空文字は全件を、登録済みのタイプ名はその敵を再保存の対象にする。
    void MarkDirty(const std::string& enemyType);

    // 全件保存の要求があるか、個別保存を待つ敵がいればtrueを返す。
    bool IsDirty() const { return dirty_ || !dirtyEnemies_.empty(); }

    // 未変更なら書き込まず true。変更があれば保存結果を返す。
    bool SaveIfDirty();

    // Playerに設定を適用する。明示的なHPリセットがなく被弾済みなら、変更前の残量の割合を使う。
    bool ApplyPlayerStatus(engine::Object3D* object, bool resetCurrentHp) const;

    // 敵タイプに対応する設定だけを適用し、旧データではParam内のタイプ名も参照する。
    bool ApplyEnemyStatus(engine::Object3D* object, bool resetCurrentHp) const;

    // クラス名でプレイヤー・敵を振り分け、それ以外のシーン物体には値を適用しない。
    bool ApplyManagedStatus(engine::Object3D* object, bool resetCurrentHp) const;

    // 対象が存在し、クラス名がPlayerまたはEnemyの場合にtrueを返す。
    static bool IsManagedCharacter(const engine::Object3D* object);

    // 敵の専用フォルダーがある場合はparameters.json、ない場合は共有設定のパスを返す。
    static std::string GetSettingsPath(const std::string& enemyType = {});

  private:
    GameplayStatusManager() = default;

    GameplayStatusManager(const GameplayStatusManager&) = delete;

    GameplayStatusManager& operator=(const GameplayStatusManager&) = delete;

    // 指定した設定JSONを読み込み、現在の調整値へ反映する。
    bool LoadFromFile(const std::string& path);
    // 敵ごとの任意の設定ファイルを読み、失敗したファイルがあっても残りを処理する。
    bool LoadDedicatedEnemyParameters();

    // HP・速度・スケール・斜面角などの値を、適用前に許容範囲へ収める。
    static void Normalize(CharacterStatus& status);

    // HP以外の値は正規化済み設定へ揃え、HPだけは残量比または明示リセットを基準にする。
    static void ApplyStatus(engine::Object3D* object, const CharacterStatus& status, bool resetCurrentHp);

  private:
    CharacterStatus playerStatus_;
    PlayerLocomotionSettings playerLocomotion_;
    std::unordered_map<std::string, CharacterStatus> enemyStatuses_;
    bool initialized_ = false;
    // 全体の設定を保存し直す必要がある場合にtrue。
    bool dirty_ = false;
    // 個別に保存が必要な登録済みの敵タイプ。
    std::unordered_set<std::string> dirtyEnemies_;
};
