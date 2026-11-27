#pragma once

#include <cstdint>
#include "BaseGimmick.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>

class Player;

// 一台に一能力を置くコピー台です。旧シーンの複数コア設定も読み込めます。
// 台座の接触から選択したコアを探し、Playerへ能力を渡す。能力別の見た目はPlayerCopyTypeCatalogから取得する。
// 主な入口：Initialize / Update / OnCollision / OnTrigger
class GimmickCopyMemoryStation final : public BaseGimmick {
  public:
    // 表示名とは独立した、既存シーンの保存IDと生成モデルです。
    static constexpr const char* kTypeId = "CopyMemoryStation";
    static constexpr const char* kModelPath = "Gimmicks/copy_memory_station";

    /// コアの種類と配置設定を読み込み、表示を準備する。
    void Initialize(engine::Object3DCommon* common, const std::string& modelName) override;

    /// コアの浮遊表示と接触選択の状態を更新する。
    void Update(float deltaTime) override;

    /// 記憶台と各コアの表示を描画する。
    void Draw(ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource) override;

    // 有効なプレイ中にコアと接触したら能力を付与する。押し戻しをしないため常にfalseを返す。
    bool OnCollision(engine::Object3D* other) override;

    /// 外部トリガーで記憶台を起動する。
    void OnTrigger() override;

    /// スイッチ状態に合わせてコア選択を有効・無効にする。
    void OnSwitchEvent(bool active) override;

    /// 配置用の記憶台複製を作る。
    std::unique_ptr<engine::Object3D> Clone() const override;

  private:
    static constexpr size_t kCoreCount = 4;

    // 旧Scene JSONのparamを、このギミックが使う設定だけへ写す境界です。
    struct Settings {
        std::array<std::string_view, kCoreCount> types{};
        float activationRadius = 0.9f;
        float essenceScale = 1.0f;
        float hoverAmount = 0.10f;
        float glow = 0.45f;
        bool unlimitedDuration = true;
        bool startActive = true;
    };

    /// Scene JSON の設定を記憶台用の値へ正規化する。
    Settings ReadSettings() const;

    /// Play 中に使用するコアと選択状態を準備する。
    void BeginPlayState();

    // 編集用の開始設定へ戻し、直前の取得パルスと接触履歴を消す。
    void ResetEditorState();

    /// コアの浮遊位置と発光を更新する。
    void UpdateCoreVisuals(float deltaTime);

    // indexのコアについて、接近度0〜1と見た目の倍率を使い、共有する固定数の光点と輪を更新する。
    void UpdateCoreEffects(size_t index, float deltaTime, float approach, float scale);

    /// コアに敵種別と対応する見た目を設定する。
    void ConfigureCore(size_t index, const std::string& enemyType);

    // 登録済みの有効な能力が一つだけ設定されていればtrue。
    bool IsSingleCore() const;

    // indexに対応する台座ローカルの位置。単一コアは中央。
    engine::Vector3 GetCoreLocalPosition(size_t index) const;

    // 台座の行列でローカル位置を変換したワールド座標。
    engine::Vector3 GetCoreWorldPosition(size_t index) const;

    // indexの保存済み能力名。範囲外か未設定なら空文字。
    std::string GetConfiguredType(size_t index) const;

    // プレイヤーに水平距離が最も近い有効なコアの番号を返す。接触範囲内になければ-1。
    std::int32_t FindTouchedCore(const engine::Vector3& playerPosition) const;

    // 有効なプレイヤーへ能力を付与し、成功時だけ再取得待ちとパルスを開始する。
    void ActivateCore(Player& player, size_t index);

    std::array<std::unique_ptr<engine::Object3D>, kCoreCount> coreVisuals_{};
    std::array<std::string, kCoreCount> displayedTypes_{};
    std::array<float, kCoreCount> corePulseTimers_{};
    std::array<engine::Vector3, kCoreCount> transferTargets_{};
    std::array<std::unique_ptr<engine::Object3D>, kCoreCount * 4> effects_{};
    // 浮遊と光点の周期に用いる経過秒数。600秒で巻き戻る。
    float visualTimer_ = 0.0f;
    // 次のコア取得までの残り秒数。
    float selectionCooldown_ = 0.0f;
    std::int32_t latchedCore_ = -1; // 前回接触したコア番号。-1は未接触。
    bool initializedForPlay_ = false;
    bool runtimeEnabled_ = true;
    bool collisionObserved_ = false; // 直近の衝突通知でプレイヤー接触を検出したか。
};
