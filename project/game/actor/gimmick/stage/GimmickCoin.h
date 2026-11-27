#pragma once
#include "BaseGimmick.h"

// プレイヤーが取得できるコインギミック
// 接触で取得したときはGameDataManagerの枚数を増やし、GamePlaySceneへリザルト集計用の取得を通知する。
// 主な入口：Initialize / Update / OnCollision
class GimmickCoin : public BaseGimmick {
  public:
    // 表示名とは独立した、既存シーンの保存IDと生成モデルです。
    static constexpr const char* kTypeId = "Coin";
    static constexpr const char* kModelPath = "Primitives/sphere";

    virtual ~GimmickCoin() = default;

    // 配置コインの球トリガーを準備し、取得と一時ドロップの状態を初期化する。
    void Initialize(engine::Object3DCommon* common, const std::string& modelName) override;

    // 取得時の跳ね上がりと、一時ドロップの落下・寿命は通常の回転より優先します。
    void Update(float deltaTime) override;

    // 未取得のコインにプレイヤー属性の相手が触れた場合に取得し、trueを返す。
    bool OnCollision(engine::Object3D* other) override;

    // 複製先の描画資源を初期化した上で、設定されたコインの見た目を引き継ぎます。
    std::unique_ptr<engine::Object3D> Clone() const override;

    // 敵からの一時ドロップに切り替えます。速度はワールド単位/秒、各時刻は秒、groundY はワールド Y 座標です。
    // 寿命は最低0.2秒。寿命の残りがblinkStartTime以下になると点滅し、0以下では点滅しない。
    void ConfigureTemporaryDrop(const engine::Vector3& initialVelocity, float lifetime, float blinkStartTime, float groundY);

  protected:
    // 取得演出と一時ドロップの進行を保存し、Replay 再開後も同じ位置から続けます。
    void CaptureReplayCustomState(json& state) const override;

    // 記録のないキーは現在の値を保持し、記録に存在する項目を読み戻す。
    void RestoreReplayCustomState(const json& state) override;

  private:
    // 取得演出を開始してからセーブ側の枚数を更新します。
    void Collect();

    // 落下中のコインは地面への接触後に減速し、期限直前だけ点滅させます。
    void UpdateTemporaryDrop(float deltaTime);

    // 描画・衝突の走査中に直接破棄せず、シーンの遅延削除へ依頼します。
    void RequestSelfRemove();

  private:
    bool isCollected_ = false;
    bool isTemporaryDrop_ = false;

    float rotationSpeed_ = 3.0f;
    // 取得演出の開始からの経過秒数。
    float collectAnimationTimer_ = 0.0f;

    // 一時ドロップが生成されてからの経過秒数。
    float dropAge_ = 0.0f;
    // 未取得のまま消えるまでの寿命。秒単位。
    float dropLifetime_ = 0.0f;
    // 寿命の残りがこの秒数以下になると点滅する。0で無効。
    float dropBlinkStartTime_ = 0.0f;

    // コインの下端が地面に接するために必要な原点の高さ。
    float dropGroundY_ = 0.0f;
    // 地面の高さに加えるモデル原点の補正量。
    float dropGroundOffset_ = 0.0f;
    // 0以下の間は重力と反発を更新し、正になった後は水平摩擦だけを更新する。
    float dropSettleTimer_ = 0.0f;
    // 落下・反発時の速度。ワールド単位/秒。
    engine::Vector3 dropVelocity_ = engine::Vector3::kZero;

    engine::Vector3 collectStartPosition_{};
    engine::Vector3 collectStartScale_{0.6f, 0.6f, 0.15f};
};
