#include <cstdint>
#include "Fade.h"
#include "Easing.h"

#include "DirectXCommon.h"
#include "PostEffect.h"
#include "Sprite.h"
#include "SpriteCommon.h"
#include "TextureManager.h"
#include "WinApp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace {
// 現在のクライアント領域の幅をピクセルで返します。
float ScreenW() {
    return static_cast<float>(WinApp::GetClientWidth());
}

// 現在のクライアント領域の高さをピクセルで返します。
float ScreenH() {
    return static_cast<float>(WinApp::GetClientHeight());
}

// 補間値を0～1へ制限します。
float Clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}



// フェード用連番画像の番号を2桁に揃えてファイルパスを作ります。
std::string MakeFramePath(const std::string& directory, std::int32_t index) {
    char buffer[256] = {};
    std::snprintf(buffer, sizeof(buffer), "%s/frame_%02d.png", directory.c_str(), index);
    return buffer;
}
}

Fade* Fade::GetInstance() {
    static Fade sInstance;
    return &sInstance;
}

// 深度や色の後処理側へノイズ画像を登録し、Spriteで描くフェードの初期状態を準備します。
void Fade::Initialize() {
    ResolveNoiseTexture();
    ResetPostEffectFade();
    InitializeSprites();
}

// 描画基盤と黒画面用Spriteを一度だけ作り、画像列が欠けても暗転できるようにします。
void Fade::InitializeSprites() {
    // 黒い代替画面用のSpriteを作成済みなら、同じ描画資源を使い続ける。
    if (fallbackBlack_) {
        return;
    }

    // Sprite共通の描画設定が未初期化なら、先にDirectXのデバイスで使える状態にする。
    if (!spriteCommon_) {
        spriteCommon_ = std::make_unique<engine::SpriteCommon>();
        spriteCommon_->Initialize(engine::DirectXCommon::GetInstance());
    }

    // 連番画像を読み込めない場合も画面を覆えるよう、白一枚を黒く着色するSpriteを持つ。
    const uint32_t whiteHandle = engine::TextureManager::GetInstance()->GetSrvHandle("Resources/sprite/common/white.png");

    // 白画像のハンドルが無効なら、この呼び出しでは黒い代替画面を作れない。
    if (whiteHandle == 0) {
        return;
    }

    fallbackBlack_ = std::make_unique<engine::Sprite>();
    fallbackBlack_->Initialize(spriteCommon_.get(), whiteHandle);
    fallbackBlack_->SetAnchorPoint({ 0.5f, 0.5f });
}

// 指定ディレクトリの連番を0番から読み込み、全フレームが揃ったときだけ再生用Spriteを作ります。
void Fade::LoadSequence(FrameSequence& sequence, const std::string& directory, std::int32_t frameCount) {
    // 登録済みの連番は再取得せず、以後の描画で同じ画像ハンドルを使う。
    if (!sequence.textureHandles.empty()) {
        return;
    }

    std::vector<uint32_t> resolvedHandles;
    resolvedHandles.reserve(frameCount);

    for (std::int32_t i = 0; i < frameCount; ++i) {
        const uint32_t handle = engine::TextureManager::GetInstance()->GetSrvHandle(MakeFramePath(directory, i));

        // 途中の1枚でも取得できなければ一覧を登録せず、DrawFrameSequence()の黒画面に任せる。
        if (handle == 0) {
            return;
        }

        resolvedHandles.push_back(handle);
    }

    // すべて揃ってから確定し、途中までのフレームを誤って再生しない。
    sequence.textureHandles = std::move(resolvedHandles);

    // 読み込んだフレームが1枚以上ある場合だけ、先頭画像で再生用Spriteを初期化する。
    if (!sequence.textureHandles.empty()) {
        sequence.sprite = std::make_unique<engine::Sprite>();
        sequence.sprite->Initialize(spriteCommon_.get(), sequence.textureHandles.front());
        sequence.sprite->SetAnchorPoint({ 0.5f, 0.5f });
    }
}

void Fade::ResolveNoiseTexture() {
    // 既にPostEffectへノイズのSRVを渡していれば、同じ素材を探し直さない。
    if (noiseTextureResolved_) {
        return;
    }

    const uint32_t noiseHandle = engine::TextureManager::GetInstance()->GetSrvHandle(
        "Resources/sprite/effect/noise0.png");
    // ノイズのハンドルを取得できなければ解決済みにせず、次回の更新で再取得を試す。
    if (noiseHandle == 0) {
        return;
    }

    PostEffect::GetInstance()->SetNoiseTexture(noiseHandle);
    noiseTextureResolved_ = true;
}

// フレーム時間（秒）で開閉を進め、演出時間を使い切ったら完了状態にします。
void Fade::Update(float deltaTime) {
    ResolveNoiseTexture();

    // 未開始または完了済みなら、タイマーや現在の被覆率を進めない。
    if (status_ == Status::kNone || status_ == Status::kFinished) {
        return;
    }

    counter_ += deltaTime;
    const float t = GetNormalizedTime();

    // 開閉を同じ被覆率で表し、連番画像のフレーム選択に使う。
    coverage_ = IsClosing() ? Easing::SmoothStep01(t) : 1.0f - Easing::SmoothStep01(t);

    // このフェードはSpriteで描画し、旧PostEffect側のフェード値は毎回解除する。
    ResetPostEffectFade();

    // 規定時間へ達したら、閉じる演出は全面を覆い、開く演出は覆いを消した状態で止める。
    if (t >= 1.0f) {
        coverage_ = IsClosing() ? 1.0f : 0.0f;
        status_ = Status::kFinished;
    }
}

// スタイルに応じた連番画像を描き、画像が使えない場合は黒画面で被覆します。
void Fade::Draw() {
    // 画面の被覆が実質ゼロの間は、連番の読み込みと描画を行いません。
    if (coverage_ <= 0.001f) {
        return;
    }

    InitializeSprites();

    // 王冠アイリスでは専用の48枚を読み込み、被覆率に対応する絵を描く。
    if (style_ == VisualStyle::kCrownIris) {
        LoadSequence(crownIris_, "Resources/sprite/fade/crown_iris", 48);
        DrawFrameSequence(crownIris_, coverage_);
    // その他のスタイルではスライムワイプの48枚を使う。
    } else {
        LoadSequence(slimeWipe_, "Resources/sprite/fade/slime_wipe", 48);
        DrawFrameSequence(slimeWipe_, coverage_);
    }
}

// 王冠アイリスを閉状態から開くフェードを開始します。
void Fade::StartFadeIn(float duration) {
    Begin(Status::kFadeIn, VisualStyle::kCrownIris, duration, { 0.5f, 0.5f }, 1.0f);
}

// 王冠アイリスを開状態から閉じるフェードを開始します。
void Fade::StartFadeOut(float duration) {
    Begin(Status::kFadeOut, VisualStyle::kCrownIris, duration, { 0.5f, 0.5f }, 0.0f);
}

// 王冠アイリスを閉じます。中心は0〜1の画面座標として保持します。
void Fade::StartIrisOut(float duration, const engine::Vector2& center) {
    Begin(Status::kIrisOut, VisualStyle::kCrownIris, duration, center, 0.0f);
}

// 王冠アイリスを開きます。中心は0〜1の画面座標として保持します。
void Fade::StartIrisIn(float duration, const engine::Vector2& center) {
    Begin(Status::kIrisIn, VisualStyle::kCrownIris, duration, center, 1.0f);
}

// 進行中の演出と被覆率を消し、旧PostEffectのフェード値も解除します。
void Fade::Stop() {
    status_ = Status::kNone;
    counter_ = 0.0f;
    coverage_ = 0.0f;
    ResetPostEffectFade();
}

void Fade::Begin(Status status, VisualStyle style, float duration, const engine::Vector2& center, float initialCoverage) {
    // 再開時にも前回のポストエフェクト量が残らないよう、開始値を確定してからリセットする。
    InitializeSprites();
    status_ = status;
    style_ = style;
    // 0秒以下などの指定でも進行率を計算できるよう、演出時間の下限を0.001秒にする。
    duration_ = (std::max)(duration, 0.001f);
    counter_ = 0.0f;
    // 指定中心は正規化座標として保持します。現在の連番Spriteの描画位置は画面中央固定です。
    center_ = { Clamp01(center.x), Clamp01(center.y) };
    coverage_ = Clamp01(initialCoverage);
    ResetPostEffectFade();
}

// 被覆率から対応フレームを選び、画像列が未準備なら黒画面へ切り替えます。
void Fade::DrawFrameSequence(FrameSequence& sequence, float coverage) {
    // 再生用Spriteか連番のどちらかが未準備なら、同じ被覆率を黒画面の透過度に使う。
    if (!sequence.sprite || sequence.textureHandles.empty()) {
        DrawFallbackBlack(coverage);
        return;
    }

    // 画面の被覆率を連番画像の添字に対応させ、開閉に同じ画像列を使う。
    const float framePosition = Clamp01(coverage) * static_cast<float>(sequence.textureHandles.size() - 1);
    // 被覆率に最も近い画像を選び、0は最初の画像、1は最後の画像に対応します。
    const size_t frameIndex = static_cast<size_t>(std::round(framePosition));
    const uint32_t textureHandle = sequence.textureHandles[(std::min)(frameIndex, sequence.textureHandles.size() - 1)];

    // 選んだ画像をクライアント領域の中央に置き、画面全体に広げる。
    sequence.sprite->SetTextureHandle(textureHandle);
    sequence.sprite->SetPosition({ ScreenW() * 0.5f, ScreenH() * 0.5f });
    sequence.sprite->SetSize({ ScreenW(), ScreenH() });
    sequence.sprite->SetRotation(0.0f);
    sequence.sprite->SetColor({ 1.0f, 1.0f, 1.0f, 1.0f });
    sequence.sprite->Update();
    sequence.sprite->Draw();
}

// 画面端に隙間を作らない大きさで黒Spriteを描きます。
void Fade::DrawFallbackBlack(float alpha) {
    // 白画像をもとにした代替Spriteが未準備なら、黒い画面を描けないため終了する。
    if (!fallbackBlack_) {
        return;
    }

    fallbackBlack_->SetPosition({ ScreenW() * 0.5f, ScreenH() * 0.5f });

    // 画面端に隙間が出ないよう、フォールバックの黒板だけ少し大きく描く。
    fallbackBlack_->SetSize({ ScreenW() + 8.0f, ScreenH() + 8.0f });
    fallbackBlack_->SetRotation(0.0f);
    fallbackBlack_->SetColor({ 0.0f, 0.0f, 0.0f, Clamp01(alpha) });
    fallbackBlack_->Update();
    fallbackBlack_->Draw();
}

// 0秒指定でも除算が破綻しないよう、経過率を0～1へ収めます。
float Fade::GetNormalizedTime() const {
    return Clamp01(counter_ / (std::max)(duration_, 0.001f));
}

// 現在の状態が被覆率を増やす方向か判定します。
bool Fade::IsClosing() const {
    return status_ == Status::kFadeOut || status_ == Status::kIrisOut;
}

// Sprite描画へ移したフェードと旧PostEffectの値が重複しないようにします。
void Fade::ResetPostEffectFade() {
    // PostEffectの定数バッファが使えるときだけ、2種類のフェード強度を0にする。
    if (PostEffect::Params* params = PostEffect::GetInstance()->GetParams()) {
        params->slimeFadeIntensity = 0.0f;
        params->irisFadeIntensity = 0.0f;
    }
}
