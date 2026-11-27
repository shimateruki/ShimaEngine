#define NOMINMAX
#include "SaveIndicatorOverlay.h"
#include "Easing.h"
#include <numbers>

#include "Sprite.h"
#include "SpriteCommon.h"
#include "game/ui/SpriteLayoutScaler.h"
#include "SpriteLayoutFactory.h"
#include "TextureManager.h"
#include "json.hpp"

#ifdef USE_IMGUI
#include "imgui.h"
#endif

#include <algorithm>
#include <cmath>
#include <fstream>

using json = nlohmann::json;

namespace {
constexpr float kFadeInDuration = 0.16f;
constexpr float kFadeOutDuration = 0.30f;
constexpr float kPi = std::numbers::pi_v<float>;


} // namespace

void SaveIndicatorOverlay::Initialize(engine::SpriteCommon* spriteCommon, const std::string& layoutPath) {
    // レイアウト読込後に役割別Spriteを結び付け、開始時は非表示にする。
    spriteCommon_ = spriteCommon;
    LoadLayout(layoutPath);
    BindSprites();
    SetAllVisible(false);
}

void SaveIndicatorOverlay::Finalize() {
    // シーン終了後にSprite参照やデバッグ固定状態を持ち越さない。
    sprites_.clear();
    panel_ = {};
    icon_ = {};
    text_ = {};
    spriteCommon_ = nullptr;
    isActive_ = false;
    debugHold_ = false;
    timer_ = 0.0f;
}

void SaveIndicatorOverlay::Play(float holdDuration) {
    // 連続保存でも表示時計を先頭へ戻し、最低表示時間を確保する。
    holdDuration_ = std::max(0.35f, holdDuration);
    timer_ = 0.0f;
    isActive_ = true;
    SetAllVisible(true);
    ApplyVisibilityAnimation(0.0f);
}

// 保存通知は表示保持とフェードを分け、連続保存では表示時間を更新します。
void SaveIndicatorOverlay::Update(float deltaTime) {
    // 通知が無効なときは、時計とスプライトの更新を省く。
    if (!IsActive()) {
        return;
    }

    timer_ += deltaTime;

    float alpha = 1.0f;

    // 見た目の確認で固定中は、通知のフェードと自動終了を進めません。
    if (!debugHold_) {
        const float totalDuration = kFadeInDuration + holdDuration_ + kFadeOutDuration;

        // 出現区間内は、経過時間に比例して透明から表示状態へ近づける。
        if (timer_ < kFadeInDuration) {
            alpha = timer_ / kFadeInDuration;
        // 保持区間を過ぎたときだけ透明へ戻し、その手前の保持区間は初期値の不透明を使う。
        } else if (timer_ > kFadeInDuration + holdDuration_) {
            alpha = 1.0f - (timer_ - kFadeInDuration - holdDuration_) / kFadeOutDuration;
        }

        // 退場まで終えたら、透明なまま描画せず通知全体を非表示にします。
        if (timer_ >= totalDuration) {
            isActive_ = false;
            SetAllVisible(false);
            return;
        }
    }

    // 通知の各要素へ同じ透明度を渡し、変更した姿勢をSpriteの更新で反映します。
    ApplyVisibilityAnimation(std::clamp(alpha, 0.0f, 1.0f));

    for (auto& sprite : sprites_) {
        // レイアウトから作成済みのスプライトにだけ、変更した姿勢の更新を依頼する。
        if (sprite) {
            sprite->Update();
        }
    }
}

void SaveIndicatorOverlay::Draw() {
    // 保存通知が無効な間はSpriteの描画要求自体を出さない。
    if (!IsActive()) {
        return;
    }

    for (auto& sprite : sprites_) {
        // 実体があるスプライトだけに描画を依頼する。
        if (sprite) {
            sprite->Draw();
        }
    }
}

void SaveIndicatorOverlay::CollectReplaySprites(std::vector<engine::Sprite*>& replaySprites) const {
    // リプレイ復元で通知の見た目も同じフレームへ戻せるよう登録する。
    replaySprites.reserve(replaySprites.size() + sprites_.size());

    for (const auto& sprite : sprites_) {
        // 取り込める実体だけをリプレイ用一覧に追加する。
        if (sprite) {
            replaySprites.push_back(sprite.get());
        }
    }
}

void SaveIndicatorOverlay::CaptureReplayState(json& state) const {
    // 表示時計とデバッグ固定を併せて記録し、再生時の消える時刻を合わせる。
    state = {{"active", isActive_}, {"debugHold", debugHold_}, {"timer", timer_}, {"holdDuration", holdDuration_}};
}

void SaveIndicatorOverlay::RestoreReplayState(const json& state) {
    // 欠けた項目は現在値を保ち、復元後に0秒更新で見た目を同期する。
    if (!state.is_object()) {
        return;
    }

    isActive_ = state.value("active", isActive_);
    debugHold_ = state.value("debugHold", debugHold_);
    timer_ = state.value("timer", timer_);
    holdDuration_ = state.value("holdDuration", holdDuration_);
    Update(0.0f);
}

#ifdef USE_IMGUI
void SaveIndicatorOverlay::DrawImGui() {
    // 表示固定は保存処理を起こさず、通知の見た目だけを確認するために使う。
    if (ImGui::Button("セーブ中表示を再生", ImVec2(-1.0f, 28.0f))) {
        Play(1.35f);
    }

    bool hold = debugHold_;

    // 固定チェックの値が変更されたときだけ、デバッグ用の表示状態を切り替える。
    if (ImGui::Checkbox("セーブ中表示を固定", &hold)) {
        debugHold_ = hold;

        // 固定を有効にしたら、時計を0へ戻し、保存を実行せず表示する。
        if (debugHold_) {
            timer_ = 0.0f;
            SetAllVisible(true);
        // 固定解除時に通常の通知が動いていなければ、表示も閉じる。
        } else if (!isActive_) {
            SetAllVisible(false);
        }
    }
}
#endif

void SaveIndicatorOverlay::LoadLayout(const std::string& layoutPath) {
    // レイアウト JSON の座標は基準解像度から実画面へ変換します。
    sprites_.clear();

    // 描画基盤が無い場合は生成できないため、既存の要素を解放した状態で戻ります。
    if (!spriteCommon_) {
        return;
    }

    std::ifstream file(layoutPath);

    // レイアウトを開けなければ、スプライトを作らずに戻る。
    if (!file.is_open()) {
        return;
    }

    json root;
    try {
        file >> root;
    // JSONを解析できなければ、読み切れていないレイアウトを使用しない。
    } catch (...) {
        return;
    }

    // スプライトの配列を取得できない場合は、生成対象が無いため終了する。
    if (!root.contains("sprites") || !root["sprites"].is_array()) {
        return;
    }

    // 有効な要素だけを同じ解像度換算で生成し、後の名前結び付けに備えます。
    const auto layoutScale = spriteLayoutScaler::Make(root);

    for (const auto& spriteData : root["sprites"]) {
        // 配列内のオブジェクトだけを個別のスプライト定義として扱う。
        if (!spriteData.is_object()) {
            continue;
        }

        auto sprite = spriteLayoutFactory::Create(spriteCommon_, spriteData, layoutScale, "save_indicator_sprite");
        sprite->Update();
        sprites_.push_back(std::move(sprite));
    }
}

void SaveIndicatorOverlay::BindSprites() {
    // レイアウト内の名前で三要素を探し、アニメーションの基準値を控える。
    panel_.sprite = FindSprite("save_indicator_panel");
    icon_.sprite = FindSprite("save_indicator_icon");
    text_.sprite = FindSprite("save_indicator_text");

    CaptureBaseState(panel_);
    CaptureBaseState(icon_);
    CaptureBaseState(text_);
}

engine::Sprite* SaveIndicatorOverlay::FindSprite(const std::string& name) const {
    // 未定義の要素はnullptrを返し、部分的なレイアウトでも描けるようにする。
    for (const auto& sprite : sprites_) {
        // 実体があり名前が一致した最初のスプライトを返す。
        if (sprite && sprite->GetName() == name) {
            return sprite.get();
        }
    }

    return nullptr;
}

void SaveIndicatorOverlay::SetAllVisible(bool visible) {
    // 個々の演出位置を変えず、通知全体の表示状態だけを切り替える。
    for (auto& sprite : sprites_) {
        // 生成済みのスプライトだけに共通の表示状態を適用する。
        if (sprite) {
            sprite->SetVisible(visible);
        }
    }
}

void SaveIndicatorOverlay::CaptureBaseState(SpriteState& state) {
    // 拡縮や回転を毎回初期値から計算し、連続再生で変形を累積させない。
    if (!state.sprite) {
        return;
    }

    state.basePosition = state.sprite->GetPosition();
    state.baseSize = state.sprite->GetSize();
    state.baseColor = state.sprite->GetColor();
    state.baseRotation = state.sprite->GetRotation();
}

void SaveIndicatorOverlay::ApplyVisibilityAnimation(float alpha) {
    // 共通透明度に対して、パネルの拡大・アイコンの脈動・文字の揺れを別々に重ねる。
    // 経過秒を出現秒数で割り、わずかに行き過ぎてから等倍へ戻る補間に使う。
    const float appear = Easing::OutBack01(std::min(timer_ / kFadeInDuration, 1.0f));
    const float pulse = std::sin(timer_ * 5.0f);

    // パネルをわずかに拡大しながら、基準の位置へ滑り込ませます。
    if (panel_.sprite) {
        engine::Vector4 color = panel_.baseColor;
        color.w *= alpha;
        panel_.sprite->SetColor(color);
        panel_.sprite->SetSize(
            {panel_.baseSize.x * (0.92f + 0.08f * appear), panel_.baseSize.y * (0.92f + 0.08f * appear)});
        panel_.sprite->SetPosition({panel_.basePosition.x, panel_.basePosition.y + (1.0f - appear) * 10.0f});
    }

    // アイコンは保持中も回転と上下動を続け、通知が動いていることを示します。
    if (icon_.sprite) {
        const float iconScale = (0.82f + 0.18f * appear) * (1.0f + 0.045f * pulse);
        engine::Vector4 color = icon_.baseColor;
        color.w *= alpha;
        icon_.sprite->SetColor(color);
        icon_.sprite->SetSize({icon_.baseSize.x * iconScale, icon_.baseSize.y * iconScale});
        icon_.sprite->SetPosition({icon_.basePosition.x, icon_.basePosition.y + std::sin(timer_ * 6.0f) * 2.0f});
        // 回転角はラジアン。通知を出している秒数に比例して回し続ける。
        icon_.sprite->SetRotation(icon_.baseRotation + timer_ * kPi * 0.72f);
    }

    // 文字の明滅と横揺れはパネルと独立させ、表示時の基準位置を毎フレーム使います。
    if (text_.sprite) {
        engine::Vector4 color = text_.baseColor;
        color.w *= alpha * (0.88f + 0.12f * std::sin(timer_ * 7.0f));
        text_.sprite->SetColor(color);
        text_.sprite->SetSize(
            {text_.baseSize.x * (0.94f + 0.06f * appear), text_.baseSize.y * (0.94f + 0.06f * appear)});
        text_.sprite->SetPosition({text_.basePosition.x + std::sin(timer_ * 3.3f) * 1.5f, text_.basePosition.y});
    }
}
