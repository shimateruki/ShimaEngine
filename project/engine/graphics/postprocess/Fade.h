#pragma once

#include "engine/utility/math/Math.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine { class Sprite; }
namespace engine { class SpriteCommon; }

// 担当は連番画像を使った画面全体のフェードとアイリス演出。
// 関連：Sprite（被覆描画）、PostEffect（画面効果）、SceneManager（遷移）
// 主な入口：Initialize / StartFadeIn / StartFadeOut / Update / Draw
class Fade {
public:
    /// <summary>現在進行中の画面遷移。</summary>
    enum class Status {
        kNone,
        kFadeIn,
        kFadeOut,
        kIrisIn,
        kIrisOut,
        kFinished
    };

    /// <summary>共有の画面遷移インスタンスを取得する。</summary>
    static Fade* GetInstance();

    /// <summary>フェード用Spriteを初期化し、PostEffectにノイズを渡してフェード強度をリセットする。</summary>
    void Initialize();

    /// <summary>指定秒数だけ演出を進める。</summary>
    void Update(float deltaTime);

    /// <summary>現在のフェード画面を描画する。</summary>
    void Draw();

    /// <summary>duration秒かけて、全面を覆った状態から画面を開く。</summary>
    void StartFadeIn(float duration);

    /// <summary>duration秒かけて、開いた状態から画面全体を覆う。</summary>
    void StartFadeOut(float duration);

    /// <summary>閉じるアイリス演出を開始する。指定中心は保持するが、画像は画面中央に描く。</summary>
    void StartIrisOut(float duration, const engine::Vector2& center = { 0.5f, 0.5f });

    /// <summary>開くアイリス演出を開始する。指定中心は保持するが、画像は画面中央に描く。</summary>
    void StartIrisIn(float duration, const engine::Vector2& center = { 0.5f, 0.5f });

    /// <summary>現在の演出状態が完了になったか返す。Stop() で解除した状態は未実行として扱う。</summary>
    bool IsFinished() const { return status_ == Status::kFinished; }

    /// <summary>現在の演出状態を返す。</summary>
    Status GetStatus() const { return status_; }

    /// <summary>演出を停止して画面の覆いを消し、PostEffectのフェード強度も0に戻す。</summary>
    void Stop();

private:
    /// <summary>フェードに使用する画像系列の種類。</summary>
    enum class VisualStyle {
        kSlimeWipe,
        kCrownIris
    };

    /// <summary>連番テクスチャとその描画Sprite。</summary>
    struct FrameSequence {
        std::unique_ptr<engine::Sprite> sprite;
        std::vector<uint32_t> textureHandles;
    };

    Fade() = default;

    ~Fade() = default;

    Fade(const Fade&) = delete;

    Fade& operator=(const Fade&) = delete;

    // Sprite共通設定と、連番が欠けたときの黒画面用Spriteを準備する。
    void InitializeSprites();

    // directoryの0番からframeCount枚を読み、すべてのハンドルを取得できたときに登録する。
    void LoadSequence(FrameSequence& sequence, const std::string& directory, std::int32_t frameCount);

    // ノイズ画像を取得できたときにPostEffectへ渡す。未取得の場合は次回に再試行する。
    void ResolveNoiseTexture();

    // PostEffect用の定数を書ける場合に、スライムとアイリスのフェード強度を0にする。
    void ResetPostEffectFade();

    // 開始時の画面被覆率を含め、すべての遷移経路を同じ状態初期化に通す。
    void Begin(Status status, VisualStyle style, float duration, const engine::Vector2& center, float initialCoverage);

    // 被覆率に対応するフレームを描画し、画像がなければ黒画面へフォールバックする。
    void DrawFrameSequence(FrameSequence& sequence, float coverage);

    // 代替の黒い全画面Spriteを指定アルファで描画する。
    void DrawFallbackBlack(float alpha);

    // 経過秒数を演出時間で割り、0〜1に収めて返す。
    float GetNormalizedTime() const;

    // 画面を覆う向きに進むFadeOutとIrisOutの場合にtrueを返す。
    bool IsClosing() const;

private:
    Status status_ = Status::kNone;
    VisualStyle style_ = VisualStyle::kSlimeWipe;

    // 演出全体の長さ（秒）。Begin()で0.001秒以上にする。
    float duration_ = 1.0f;
    // 演出開始後の経過時間（秒）。
    float counter_ = 0.0f;
    // 画面の被覆率。0は開状態、1は全体を覆った状態。
    float coverage_ = 0.0f;
    // 指定中心は画面幅・高さに対する0～1の座標です。現在の連番画像の配置には使いません。
    engine::Vector2 center_ = { 0.5f, 0.5f };

    std::unique_ptr<engine::SpriteCommon> spriteCommon_;
    std::unique_ptr<engine::Sprite> fallbackBlack_;
    FrameSequence slimeWipe_;
    FrameSequence crownIris_;
    // ノイズのSRVをPostEffectへ渡したかを保持する。
    bool noiseTextureResolved_ = false;
};
