#pragma once
#include "PostEffect.h"
#include "IEditable.h"
#include <string>

// 担当：画面効果のパラメータを編集し、設定ファイルへ保存する。
// 関連：PostEffect（適用先）、DebugEditor（表示）
// 主な入口：Initialize / DrawImGui / SaveParams / LoadParams
class PostEffectEditor : public IEditable {
public:
    // 編集対象として借用するPostEffectを設定する。
void Initialize(PostEffect* postEffect);

    // ブルーム・色調補正など、画面効果の設定をInspectorに描く。
void DrawImGui() override;

    // Inspector上部のタイトルバーに表示される名前
    std::string GetName() override { return "Post Effect Settings"; }

    // 現在の画面効果設定を指定のJSONへ保存する。
    void SaveParams(const std::string& filename = "Resources/json/post_effect.json");

    // 指定JSONの画面効果設定を読み込む。
    void LoadParams(const std::string& filename = "Resources/json/post_effect.json");

private:
    PostEffect* targetEffect_ = nullptr;
};