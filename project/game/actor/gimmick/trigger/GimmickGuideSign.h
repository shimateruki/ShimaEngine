#pragma once

#include "BaseGimmick.h"
#include <memory>
#include <string>

namespace engine { class Camera; class SpriteCommon; }

// 担当：プレイヤーに反応する案内看板。Sprite Editorと同じレイアウトを読み込み、ゲームUIで再生します。
// 関連：GimmickFactory（生成）、GamePlayScene（UI描画）、SpriteLayoutAnimation（コマ選択）。
// 主な入口はInitialize、Update、GamePlaySceneからのDrawGuide。
class GimmickGuideSign final : public BaseGimmick {
public:
    static constexpr const char* kTypeId = "GuideSign";
    static constexpr const char* kModelPath = "Gimmicks/guide_sign/guide_sign.obj";

    GimmickGuideSign();
    ~GimmickGuideSign() override;
    // 看板を設定し、案内のレイアウト・反応距離・表示方法を編集項目に公開する。
    void Initialize(engine::Object3DCommon* common, const std::string& modelName) override;
    // プレイ中の接近を調べ、必要な画像の読み込みと動画の進行・停止を行う。deltaTimeは秒。
    void Update(float deltaTime) override;
    // 看板の配置と表示設定を複製する。レイアウトは複製先が接近時に読み込む。
    std::unique_ptr<engine::Object3D> Clone() const override;

    // 看板を画面に投影し、接近中または閉じている途中の案内を描きます。
    void DrawGuide(engine::Camera* camera);

private:
    struct Layout;
    // 設定のJSONを読み直す。成功した場合にだけ、親子関係も復元したレイアウトを保持する。
    void LoadLayout(engine::SpriteCommon* common);

    std::unique_ptr<Layout> layout_;
    // 案内の読み込み先は、ゲームの作業ディレクトリからResources/json/sprite配下に解決する。
    std::string layoutPath_;
    std::string attemptedPath_; // 同じ読み込みエラーの再試行を抑制します。
    float activationRadius_ = 5.0f; // 看板からの反応距離。ワールド単位。
    float displayHeight_ = 2.3f; // 通常表示ではワールド単位、看板から開く場合はモデルのローカルY座標。
    float displayWidth_ = 0.19f; // 画面幅に対する実演の幅。
    float displayOffsetX_ = 0.0f; // 看板の投影位置からの横ずれ。画面幅に対する割合。
    bool opensFromSign_ = false; // 下辺中央を看板に固定して開閉する表示を選びます。
    // 現在のワールド距離が反応範囲内か。
    bool inRange_ = false;
    // スプライトのコマ再生に使う、接近してからの経過秒数。
    double elapsed_ = 0.0;
};
