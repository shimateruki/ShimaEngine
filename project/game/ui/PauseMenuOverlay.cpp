#include <cstdint>
#include "game/ui/MenuControls.h"
#include "Easing.h"
#define NOMINMAX
#include "PauseMenuOverlay.h"
#include "SettingsMenuOverlay.h"
#include "SceneManager.h"

#include "InputManager.h"
#include "Sprite.h"
#include "SpriteCommon.h"
#include "game/ui/SpriteLayoutScaler.h"
#include "SpriteLayoutFactory.h"
#include "TextureManager.h"
#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>

using json = nlohmann::json;

// ポーズから共通で実行できる操作はここで受け取り、ほかの操作は各シーンへ返す。
bool HandleSharedPauseAction(PauseMenuOverlay::Action action, PauseMenuOverlay& pauseMenu, SettingsMenuOverlay* settings) {
    switch (action) {
    // 設定を重ねる操作。ポーズメニューの状態は保つ。
    case PauseMenuOverlay::Action::kOpenSettings:
        // 設定を閉じたときに戻れるよう、背面のポーズメニューは開いた状態にしておく。
        // 設定画面の実体が渡されている場合だけ、設定を表示状態にする。
        if (settings) {
            settings->SetActive(true);
        }
        return true;

    // タイトルへ戻る操作。現在のポーズを閉じてから遷移を要求する。
    case PauseMenuOverlay::Action::kReturnTitle:
        // タイトルへの切り替えを要求する前にポーズ画面を閉じる。
        pauseMenu.SetActive(false);
        SceneManager::GetInstance()->ChangeScene("TITLE");
        return true;

    // 再開やリトライなど、各シーンが担当する操作は処理済みにしない。
    default:
        return false;
    }
}

void PauseMenuOverlay::Initialize(engine::SpriteCommon* spriteCommon, const std::string& layoutPath, Mode mode) {
    // 共通レイアウトを読み込み、使用するシーンのモードに応じて項目を制御する。
    spriteCommon_ = spriteCommon;
    inputManager_ = engine::InputManager::GetInstance();
    mode_ = mode;
    LoadLayout(layoutPath);
    BindLayoutSprites();
    SetAllVisible(false);
}

void PauseMenuOverlay::Finalize() {
    // シーン切り替え後に破棄済み Sprite への参照を残さない。
    sprites_.clear();
    background_ = nullptr;
    panel_ = nullptr;
    title_ = nullptr;
    closeHintText_ = nullptr;
    confirmHintText_ = nullptr;
    cursorArrow_ = nullptr;
    cursorSlime_ = nullptr;
    panelBaseSize_ = {};
    guideTransitionOffset_ = 0.0f;
    rows_ = {};
    spriteCommon_ = nullptr;
    inputManager_ = nullptr;
    isActive_ = false;
}

void PauseMenuOverlay::SetActive(bool active) {
    // ガイドと横に並べていた位置から通常のポーズ表示位置へ戻します。
    SetGuideTransitionOffset(0.0f, false);

    // 開閉状態が変わらない場合は、遷移の位置だけ戻し、選択や入力抑止は維持する。
    if (isActive_ == active) {
        return;
    }

    isActive_ = active;
    // 開いたときの押下で直後に閉じないよう、閉じる入力が離されるまで待つ。
    suppressOpenCloseInput_ = active;
    selectedIndex_ = 0;

    // 開くときだけパネルの開きアニメーションを先頭に戻す。
    if (active) {
        time_ = 0.0f;
    }

    SetAllVisible(active);
}

void PauseMenuOverlay::ReopenFromGuide() {
    SetActive(true);

    // 操作ガイドから戻った直後は、メニューの先頭にある再開を選択する。
    selectedIndex_ = static_cast<std::int32_t>(Item::kResume);
    UpdateSprites();
}

// ポーズ画面は選択された操作を返すだけにし、シーン遷移の実行は呼び出し側に任せます。
PauseMenuOverlay::Action PauseMenuOverlay::Update(float deltaTime, bool acceptInput) {
    // 閉じている場合は時計を進めず、シーンへ操作要求を返さない。
    if (!isActive_) {
        return Action::kNone;
    }

    // 通常表示の基準位置へ一度戻す。ガイドへの移動量は遷移側が後から適用する。
    SetGuideTransitionOffset(0.0f, false);
    time_ += deltaTime;
    // 入力を受けられない間も、選択表示などのUI更新は続ける。
    Action action = acceptInput ? UpdateInput() : Action::kNone;
    UpdateSprites();

    // 選択状態をスプライトへ反映してから各部品の行列を確定する。
    for (auto& sprite : sprites_) {
        // 用意されている各部品だけ、位置などを描画データに反映する。
        if (sprite) {
            sprite->Update();
        }
    }

    return action;
}

void PauseMenuOverlay::SetGuideTransitionOffset(float offsetX, bool transitioning) {
    // 前フレームとの差だけ移動し、繰り返しても基準位置がずれないようにする。
    // 画面座標の相対差だけを各部品へ足す。固定した背景は位置を動かさず、遷移中に表示を切り替える。
    const float delta = offsetX - guideTransitionOffset_;
    guideTransitionOffset_ = offsetX;

    for (auto& sprite : sprites_) {
        // 空の参照と固定背景には横スライドを適用しない。
        if (!sprite || sprite.get() == background_)
            continue;
        engine::Vector2 position = sprite->GetPosition();
        position.x += delta;
        sprite->SetPosition(position);
        sprite->Update();
    }

    // 固定背景があれば、メニューが開きページ遷移がない間だけ表示する。
    if (background_)
        background_->SetVisible(isActive_ && !transitioning);

    // ページ遷移中の案内はガイド側に任せ、同じ二つの枠が重なるのを防ぐ。
    for (const char* name : {"pause_footer_close_frame", "pause_footer_close_text",
                             "pause_footer_confirm_frame", "pause_footer_confirm_text"}) {
        // 案内用の部品を名前で取得できた場合だけ、その表示状態を変更する。
        if (engine::Sprite* footer = FindSprite(name))
            footer->SetVisible(isActive_ && !transitioning);
    }
}

void PauseMenuOverlay::Draw() {
    // ポーズを閉じた後は非表示の Sprite を描画パスへ送らない。
    if (!isActive_) {
        return;
    }

    for (auto& sprite : sprites_) {
        // 存在する部品のみ描画する。各部品の表示可否は Sprite 側でも確認される。
        if (sprite) {
            sprite->Draw();
        }
    }
}

void PauseMenuOverlay::CollectReplaySprites(std::vector<engine::Sprite*>& replaySprites) const {
    // リプレイ中の見た目復元に使うため、所有権を移さず参照だけ集める。
    replaySprites.reserve(replaySprites.size() + sprites_.size());

    for (const auto& sprite : sprites_) {
        // 生成した部品だけを、見た目の再現に使う借用参照として追加する。
        if (sprite) {
            replaySprites.push_back(sprite.get());
        }
    }
}

void PauseMenuOverlay::CaptureReplayState(json& state) const {
    // 選択位置とアニメーション時刻を保存し、巻き戻しで同じ画面へ戻す。
    state = {{"active", isActive_},
             {"suppressInput", suppressOpenCloseInput_},
             {"selectedIndex", selectedIndex_},
             {"time", time_}};
}

void PauseMenuOverlay::RestoreReplayState(const json& state) {
    // リプレイの状態が JSON オブジェクトでなければ、現在の画面を維持する。
    if (!state.is_object()) {
        return;
    }

    SetGuideTransitionOffset(0.0f, false);
    isActive_ = state.value("active", isActive_);
    suppressOpenCloseInput_ = state.value("suppressInput", suppressOpenCloseInput_);
    selectedIndex_ = std::clamp(state.value("selectedIndex", selectedIndex_), 0, static_cast<std::int32_t>(Item::kCount) - 1);
    // 旧記録がガイドを示す場合や、ステージ選択でリトライ・ステージ選択を示す場合は再開へ戻す。
    if (selectedIndex_ == static_cast<std::int32_t>(Item::kGuide) ||
        (mode_ == Mode::kStageSelect && (selectedIndex_ == static_cast<std::int32_t>(Item::kRetry) ||
                                       selectedIndex_ == static_cast<std::int32_t>(Item::kStageSelect)))) {
        selectedIndex_ = static_cast<std::int32_t>(Item::kResume);
    }

    time_ = state.value("time", time_);
    SetAllVisible(isActive_);

    // 開いた状態へ復元した場合は、選択行や開く途中の表示を作り直す。
    if (isActive_) {
        UpdateSprites();
    }
}

void PauseMenuOverlay::LoadLayout(const std::string& layoutPath) {
    // JSON のスプライト定義を読み込み、保存された基準解像度に応じて配置と寸法を補正する。
    guideTransitionOffset_ = 0.0f;
    sprites_.clear();

    // 描画基盤か有効な配置データがない場合は、旧レイアウトを持ち越さずに戻ります。
    if (!spriteCommon_) {
        return;
    }

    std::ifstream file(layoutPath);

    // レイアウトを開けない場合は、旧スプライトを消した状態のまま戻る。
    if (!file.is_open()) {
        return;
    }

    json root;
    try {
        file >> root;
    } catch (...) {
        return;
    }

    // 配置のルートにスプライトの配列がなければ、部品の生成を始めない。
    if (!root.contains("sprites") || !root["sprites"].is_array()) {
        return;
    }

    // 各Spriteと選択マーカーが同じ解像度倍率を使うよう、倍率を保存してから生成します。
    const auto layoutScale = spriteLayoutScaler::Make(root);
    layoutRatio_ = layoutScale.enabled ? layoutScale.ratio : engine::Vector2{1.0f, 1.0f};

    for (const auto& spriteData : root["sprites"]) {
        // 配列にオブジェクト以外の項目が混じっていれば、その項目だけ読み飛ばす。
        if (!spriteData.is_object()) {
            continue;
        }

        auto sprite = spriteLayoutFactory::Create(spriteCommon_, spriteData, layoutScale, "pause_sprite");
        sprite->Update();
        sprites_.push_back(std::move(sprite));
    }
}

void PauseMenuOverlay::BindLayoutSprites() {
    // JSON 内の名前と操作行を一度だけ結び、更新時の文字列探索を避ける。
    background_ = FindSprite("pause_background");
    panel_ = FindSprite("pause_panel");
    title_ = FindSprite("pause_title");
    closeHintText_ = FindSprite("pause_footer_close_text");
    confirmHintText_ = FindSprite("pause_footer_confirm_text");
    cursorArrow_ = FindSprite("pause_cursor_arrow");
    cursorSlime_ = FindSprite("pause_cursor_slime");

    // スライム用カーソルが配置済みの場合だけ、元画像の透明余白を除いた範囲を使う。
    if (cursorSlime_) {
        // タイトル画面と同じアイコンの透明余白を切り、選択行に寄せる。
        cursorSlime_->SetTextureRect({115.0f, 218.0f}, {1024.0f, 827.0f});
    }

    // パネルと操作行の基準サイズを控え、選択演出で拡大を累積させません。
    panelBaseSize_ = panel_ ? panel_->GetSize() : engine::Vector2{};

    constexpr std::array<const char*, 6> kPrefixes = {"pause_resume", "pause_guide", "pause_retry",
                                                     "pause_stage_select", "pause_settings", "pause_title_return"};

    for (std::int32_t i = 0; i < static_cast<std::int32_t>(Item::kCount); ++i) {
        const std::string prefix = kPrefixes[static_cast<size_t>(i)];
        rows_[static_cast<size_t>(i)].backdrop = FindSprite(prefix + "_row");
        rows_[static_cast<size_t>(i)].label = FindSprite(prefix + "_label");

        // 行の背景が配置されている場合は、拡縮の基準サイズを控える。
        if (rows_[static_cast<size_t>(i)].backdrop) {
            rows_[static_cast<size_t>(i)].backdropBaseSize = rows_[static_cast<size_t>(i)].backdrop->GetSize();
        }

        // ラベルが配置されている場合も、文字の強調に使う基準サイズを控える。
        if (rows_[static_cast<size_t>(i)].label) {
            rows_[static_cast<size_t>(i)].labelBaseSize = rows_[static_cast<size_t>(i)].label->GetSize();
        }
    }
}

engine::Sprite* PauseMenuOverlay::FindSprite(const std::string& name) const {
    // レイアウトに不足があっても nullptr として扱い、UI 全体は継続する。
    for (const auto& sprite : sprites_) {
        // 部品を名前で照合し、最初に一致した借用参照を返す。
        if (sprite && sprite->GetName() == name) {
            return sprite.get();
        }
    }

    return nullptr;
}

void PauseMenuOverlay::SetAllVisible(bool visible) {
    // 画面を閉じる時は背景だけでなく選択カーソルも一括で隠す。
    for (auto& sprite : sprites_) {
        // 未作成の部品を除き、すべての表示可否を同じ状態へ切り替える。
        if (sprite) {
            sprite->SetVisible(visible);
        }
    }
}

bool PauseMenuOverlay::IsOpenOrCloseHeld() const {
    // キーボードとゲームパッドの閉じる入力を同じ抑制判定へまとめる。
    // 入力基盤がない場合は、閉じる入力が押されていないと扱う。
    if (!inputManager_) {
        return false;
    }

    return menuControls::Get(menuControls::Screen::kPause).bindings.cancel.Held(*inputManager_);
}

PauseMenuOverlay::Action PauseMenuOverlay::UpdateInput() {
    // 押下を一回だけ Action に変え、実際のシーン遷移は呼び出し側に任せる。
    // 入力基盤がまだ使えなければ、操作なしを返す。
    if (!inputManager_) {
        return Action::kNone;
    }

    // 画面を開いた入力の解放を待ち、その押下でメニューが閉じるのを防ぎます。
    if (suppressOpenCloseInput_) {
        // 元の押下を離した時点で抑止を解除し、次のフレームから入力を受け付ける。
        if (!IsOpenOrCloseHeld()) {
            suppressOpenCloseInput_ = false;
        }

        return Action::kNone;
    }

    // 上下で選択を進めた後、ガイドへの左送り、閉じる操作、決定の順で処理します。
    const auto menu = menuControls::Read(menuControls::Screen::kPause, *inputManager_);
    const bool up = menu.up, down = menu.down;

    // 上方向の入力で、前の有効項目へ移る。
    if (up) {
        ChangeSelection(-1);
    }

    // 下方向の入力で、次の有効項目へ移る。
    if (down) {
        ChangeSelection(1);
    }

    // ゲームプレイ中のポーズに限り、左入力で能力ガイドのページを要求する。
    if (mode_ == Mode::kGameplay && menu.left) {
        // メニューは能力説明の次ページ。左送りで前ページへ戻す。
        return Action::kOpenGuide;
    }

    // 閉じる入力を検出した場合は、そのフレームでは決定操作より再開を優先する。
    if (menu.cancel) {
        return Action::kResume;
    }

    // 決定時にシーンへ通知する操作を選びます。ここでは停止解除や場面転換を行いません。
    if (menu.confirm) {
        switch (static_cast<Item>(selectedIndex_)) {
        // 現在のゲームやステージ選択へそのまま戻る要求。
        case Item::kResume:
            return Action::kResume;

        // 能力ガイドを開く要求。
        case Item::kGuide:
            return Action::kOpenGuide;

        // 現在のステージをやり直す要求。
        case Item::kRetry:
            return Action::kRetry;

        // ステージ選択へ戻る要求。
        case Item::kStageSelect:
            return Action::kStageSelect;

        // 設定画面を前面に出す要求。
        case Item::kSettings:
            return Action::kOpenSettings;

        // タイトル画面へ戻る要求。
        case Item::kTitle:
            return Action::kReturnTitle;

        // 操作の割り当てがない値では、要求を返さない。
        default:
            break;
        }
    }

    return Action::kNone;
}

void PauseMenuOverlay::UpdateSprites() {
    // 選択行だけ拡縮と脈動を適用し、カーソルをその行の配置へ追従させる。
    const engine::Vector4 normalText = {1.0f, 1.0f, 1.0f, 0.88f};
    const engine::Vector4 normalRowColor = {1.0f, 1.0f, 1.0f, 0.90f};

    // 大きな共通フレームだけを軽く広げ、文字を伸縮させずに読みやすさを守る。
    // 共通パネルが配置済みの場合に、開き始めの寸法と透明度を反映する。
    if (panel_) {
        // 開いてから0.20秒で枠だけを基準寸法へ戻し、同じ進行率で不透明にする。
        const float progress = std::clamp(time_ / 0.20f, 0.0f, 1.0f);
        const float eased = Easing::SmoothStep(progress);
        panel_->SetSize({panelBaseSize_.x * (0.98f + 0.02f * eased),
                         panelBaseSize_.y * (0.98f + 0.02f * eased)});
        panel_->SetColor({1.0f, 1.0f, 1.0f, 0.72f + 0.28f * eased});
    }

    // 入力機器が分かる場合だけ、操作案内をキーボード用またはゲームパッド用に揃える。
    if (inputManager_) {
        // 下端の二つの案内だけを入力機器に合わせて差し替える。
        const bool gamepad = inputManager_->IsGamepadMode();
        const auto& controls = menuControls::Get(menuControls::Screen::kPause);
        const auto updateHint = [](engine::Sprite* sprite, const char* path) {
            // 案内部品がない場合や指定画像をすでに表示している場合は、テクスチャを読み直さない。
            if (!sprite || sprite->GetTextureName() == path) return;
            const uint32_t handle = engine::Sprite::LoadTexture(path);
            sprite->SetTextureHandle(handle);
            const auto& metadata = engine::TextureManager::GetInstance()->GetMetadata(handle);
            sprite->SetTextureRect({0.0f, 0.0f},
                                   {static_cast<float>(metadata.width), static_cast<float>(metadata.height)});
            sprite->SetTextureName(path);
        };
        updateHint(closeHintText_, controls.closeHint.Resolve(gamepad));
        updateHint(confirmHintText_, controls.confirmHint.Resolve(gamepad));
    }

    // 現在行には明るい版の背景と脈動を使い、非選択行は一定の見た目にします。
    for (std::int32_t i = 0; i < static_cast<std::int32_t>(Item::kCount); ++i) {
        MenuRow& row = rows_[static_cast<size_t>(i)];
        const bool selected = selectedIndex_ == i;
        const float pulse = selected ? (0.5f + 0.5f * std::sin(time_ * 5.6f)) : 0.0f;
        const float labelScale = selected ? 1.03f + pulse * 0.025f : 0.94f;
        const float rowScale = selected ? 1.02f + pulse * 0.014f : 0.98f;
        const engine::Vector4 selectedText = {1.0f, 1.0f, 1.0f, 1.0f};

        // 背景の部品があれば、選択状態に応じた画像・色・寸法へ更新する。
        if (row.backdrop) {
            const uint32_t handle = engine::Sprite::LoadTexture(selected ? "ui/settings/settings_row_selected.png"
                                                                 : "ui/settings/settings_row.png");
            row.backdrop->SetTextureHandle(handle);
            const auto& metadata = engine::TextureManager::GetInstance()->GetMetadata(handle);
            row.backdrop->SetTextureRect({0.0f, 0.0f},
                                         {static_cast<float>(metadata.width), static_cast<float>(metadata.height)});
            row.backdrop->SetSize({row.backdropBaseSize.x * rowScale, row.backdropBaseSize.y * rowScale});
            row.backdrop->SetColor(selected ? engine::Vector4{0.95f, 1.0f, 1.0f, 1.0f} : normalRowColor);
        }

        // ラベルの部品があれば、選択状態に応じて文字の色と寸法を更新する。
        if (row.label) {
            row.label->SetSize({row.labelBaseSize.x * labelScale, row.labelBaseSize.y * labelScale});
            row.label->SetColor(selected ? selectedText : normalText);
        }
    }

    // 拡縮後の行左端からカーソル位置を出し、レイアウト変更にも追従させます。
    MenuRow& selectedRow = rows_[static_cast<size_t>(selectedIndex_)];

    // 選択行の背景がある場合にだけ、その左端を基準にカーソルを置く。
    if (selectedRow.backdrop) {
        const engine::Vector2 rowPosition = selectedRow.backdrop->GetPosition();
        const engine::Vector2 rowSize = selectedRow.backdrop->GetSize();
        const float pulse = 0.5f + 0.5f * std::sin(time_ * 5.6f);
        const float bob = std::sin(time_ * 7.5f) * 3.0f;
        // アンカーが中央の行なので、拡縮後の半幅を引いてカーソルの基準になる左端を求める。
        const float rowLeft = rowPosition.x - rowSize.x * 0.5f;

        // スライム用カーソルがある場合は、左端の外側で上下に動かす。
        if (cursorSlime_) {
            cursorSlime_->SetPosition({rowLeft - 75.0f * layoutRatio_.x, rowPosition.y + bob});
            cursorSlime_->SetColor({1.0f, 1.0f, 1.0f, 1.0f});
        }

        // 矢印用カーソルがある場合は、左端の近くで左右に動かす。
        if (cursorArrow_) {
            cursorArrow_->SetPosition({rowLeft - 27.0f * layoutRatio_.x + std::sin(time_ * 8.0f) * 3.0f,
                                       rowPosition.y});
            cursorArrow_->SetColor({1.0f, 1.0f, 1.0f, 1.0f});
        }
    }
}

void PauseMenuOverlay::ChangeSelection(std::int32_t direction) {
    // ステージ選択では使えない項目を飛ばし、上下操作で有効項目だけ巡回する。
    if (mode_ == Mode::kStageSelect) {
        constexpr std::array<std::int32_t, 3> kItems = {static_cast<std::int32_t>(Item::kResume), static_cast<std::int32_t>(Item::kSettings),
                                               static_cast<std::int32_t>(Item::kTitle)};
        const auto current = std::find(kItems.begin(), kItems.end(), selectedIndex_);
        const std::int32_t index = current == kItems.end() ? 0 : static_cast<std::int32_t>(current - kItems.begin());
        selectedIndex_ = kItems[static_cast<size_t>((index + direction + static_cast<std::int32_t>(kItems.size())) %
                                                    static_cast<std::int32_t>(kItems.size()))];
        return;
    }

    // 能力ページへの移動は左操作にまとめ、決定項目には表示しない。
    constexpr std::array<std::int32_t, 5> kGameplayItems = {
        static_cast<std::int32_t>(Item::kResume), static_cast<std::int32_t>(Item::kRetry), static_cast<std::int32_t>(Item::kStageSelect),
        static_cast<std::int32_t>(Item::kSettings), static_cast<std::int32_t>(Item::kTitle)};
    const auto current = std::find(kGameplayItems.begin(), kGameplayItems.end(), selectedIndex_);
    const std::int32_t index = current == kGameplayItems.end() ? 0 : static_cast<std::int32_t>(current - kGameplayItems.begin());
    selectedIndex_ = kGameplayItems[static_cast<size_t>((index + direction + static_cast<std::int32_t>(kGameplayItems.size())) %
                                                    static_cast<std::int32_t>(kGameplayItems.size()))];
}
