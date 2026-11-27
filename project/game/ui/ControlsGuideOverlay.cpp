#include <cstdint>
#include "game/ui/MenuControls.h"
#include "Easing.h"
#include <numbers>
#define NOMINMAX
#include "ControlsGuideOverlay.h"
#include "engine/graphics/2d/VideoSpritePlayer.h"

#include "InputManager.h"
#include "game/ui/InputGlyphResolver.h"
#include "Player.h"
#include "Sprite.h"
#include "SpriteCommon.h"
#include "game/ui/SpriteLayoutScaler.h"
#include "SpriteLayoutFactory.h"
#include "TextureManager.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#ifdef USE_IMGUI
#include "imgui.h"
#include "EditorCommandRegistry.h"
#include "EditorManager.h"
#endif

using json = nlohmann::json;

namespace {


// 能力ガイドの三行分の説明画像。Sprite の生成や所有は呼出側で行う。
struct AbilityGuideVisual {
    std::array<const char*, 3> labelTextures{};
};

const char* ResolveBookKey(const Player* player) {
    // コピー状態を図鑑データの検索キーへ変換し、未変身は通常ページを使う。
    if (!player || !player->IsEnemyMorphed())
        return "normal";
    switch (player->GetEnemyMorphType()) {
    case Player::EnemyMorphType::kSlime:
        return "pink";

    case Player::EnemyMorphType::kBomber:
        return "bomb";

    case Player::EnemyMorphType::kFireSlime:
        return "fire";

    case Player::EnemyMorphType::kThunderSlime:
        return "thunder";

    case Player::EnemyMorphType::kWindSlime:
        return "wind";

    case Player::EnemyMorphType::kCrown:
        return "crown";

    default:
        return "other";
    }
}

AbilityGuideVisual ResolveAbilityGuide(const Player* player) {
    // プレイヤーの状態に対応する三行の説明画像を選ぶ。取得できない場合は空の行を返す。
    if (!player) {
        return {};
    }

    // 未変身時は基本の吸収・攻撃・フックを案内する。
    if (!player->IsEnemyMorphed()) {
        return {{"ui/control_guide/labels/absorb.png", "ui/control_guide/labels/slime_attack.png",
                 "ui/control_guide/labels/hook_aim.png"}};
    }

    switch (player->GetEnemyMorphType()) {
    case Player::EnemyMorphType::kSlime:
        return {{"ui/control_guide/labels/slime_dive.png", "ui/control_guide/labels/puni_straight.png",
                 "ui/control_guide/labels/puni_guard.png"}};

    case Player::EnemyMorphType::kBomber:
        return {{"ui/control_guide/labels/bomb_throw.png", "ui/control_guide/labels/bomb_place.png",
                 "ui/control_guide/labels/blast_jump.png"}};

    case Player::EnemyMorphType::kFireSlime:
        return {{"ui/control_guide/labels/fireball.png", "ui/control_guide/labels/flame_breath.png",
                 "ui/control_guide/labels/blaze_step.png"}};

    case Player::EnemyMorphType::kThunderSlime:
        return {{"ui/control_guide/labels/thunder_chain.png", "ui/control_guide/labels/charged_discharge.png",
                 "ui/control_guide/labels/thunder_step.png"}};

    case Player::EnemyMorphType::kWindSlime:
        return {{"ui/control_guide/labels/wind_vortex.png", "ui/control_guide/labels/wind_tornado.png",
                 "ui/control_guide/labels/wind_slide.png"}};

    default:
        // 三行の説明画像が定義されていない能力では空の行を返す。
        return {};
    }
}

const char* ResolvePortraitTexture(const Player* player) {
    // 能力ごとの肖像画像を選び、未変身はプレイヤーの画像へ戻す。
    if (!player || !player->IsEnemyMorphed()) {
        return "ui/portraits/player.png";
    }

    switch (player->GetEnemyMorphType()) {
    case Player::EnemyMorphType::kSlime:
        return "ui/control_guide/portraits/pink_slime.png";

    case Player::EnemyMorphType::kBomber:
        return "ui/control_guide/portraits/bomb_slime.png";

    case Player::EnemyMorphType::kFireSlime:
        return "ui/control_guide/portraits/fire_slime.png";

    case Player::EnemyMorphType::kThunderSlime:
        return "ui/control_guide/portraits/thunder_slime.png";

    case Player::EnemyMorphType::kWindSlime:
        return "ui/control_guide/portraits/wind_slime.png";

    case Player::EnemyMorphType::kNone:
    // 肖像の定義がない能力も共通のプレイヤー画像を使う。
    default:
        return "ui/portraits/player.png";
    }
}

// UI画像はsRGBで統一し、normalや_nameを含む名前が法線画像と誤判定されるのを防ぐ。
uint32_t LoadGuideTexture(const std::string& path) {
    return engine::TextureManager::GetInstance()->Load(engine::Sprite::ResolveTexturePath(path),
                                               engine::TextureManager::TextureColorSpace::kSRGB);
}

void SetFullTextureRect(engine::Sprite* sprite, uint32_t textureHandle) {
    // Spriteと画像ハンドルが揃う場合だけ、差し替え画像の全体をUV範囲へ反映する。
    if (!sprite || textureHandle == 0) {
        return;
    }

    sprite->SetTextureHandle(textureHandle);
    const auto& metadata = engine::TextureManager::GetInstance()->GetMetadata(textureHandle);
    sprite->SetTextureRect({0.0f, 0.0f}, {static_cast<float>(metadata.width), static_cast<float>(metadata.height)});
}

engine::Vector2 SetGlyphTexture(engine::Sprite* sprite, const InputGlyphSpec& glyph) {
    // 画像全体を設定した後、アトラス指定があれば該当するキー領域に絞る。戻り値は画像の使用範囲のピクセル寸法。
    const uint32_t handle = LoadGuideTexture(glyph.path);
    SetFullTextureRect(sprite, handle);
    sprite->SetTextureName(glyph.path);

    if (glyph.useCrop) {
        sprite->SetTextureRect({glyph.cropX, glyph.cropY}, {glyph.width, glyph.height});
        return {glyph.width, glyph.height};
    }

    const auto& metadata = engine::TextureManager::GetInstance()->GetMetadata(handle);
    return {static_cast<float>(metadata.width), static_cast<float>(metadata.height)};
}
} // namespace

void ControlsGuideOverlay::Initialize(engine::SpriteCommon* spriteCommon, Player* player, const std::string& layoutPath) {
    // レイアウト読込後に現在能力と入力機器を反映し、表示は閉じた状態から始める。
    spriteCommon_ = spriteCommon;
    inputManager_ = engine::InputManager::GetInstance();
    player_ = player;
    LoadLayout(layoutPath);
    BindLayoutSprites();
    RefreshAbilityVisuals();
    RefreshInputGlyphs(true);
    SetAllVisible(false);
#ifdef USE_IMGUI
    // 図鑑があるレイアウトだけを編集コマンドから開き、能力ページを選んで調整対象にします。
    EditorCommand command;
    command.id = "ui.abilityGuide";
    command.displayName = "能力ガイドの調整";
    command.category = "UI";
    command.description = "実モデルの能力図鑑を表示して配置とスクロールを調整";
    command.keywords = {"Tab", "能力", "ガイド", "UI"};
    command.owner = this;
    command.canExecute = [this]() { return bookEnabled_; };
    command.execute = [this]() {
        currentPage_ = 1;
        SetActive(true);
        EditorManager::GetInstance()->SetSelectedObject(this);
    };

    EditorCommandRegistry::GetInstance()->Register(std::move(command));
#endif
}

ControlsGuideOverlay::ControlsGuideOverlay() = default;

ControlsGuideOverlay::~ControlsGuideOverlay() {
    // 編集コマンドと所有スプライトを破棄する共通経路を使う。
    Finalize();
}

void ControlsGuideOverlay::Finalize() {
    // 動画の再生を止めてから、独立して保持していた表示Spriteとデコーダーを解放します。
    CloseMovePreview();
    videoPlayer_.reset();
    movePreviewSprite_.reset();
    movePreviewDescription_.reset();
    // エディターの選択参照を先に外し、所有する表示要素とプレイヤー参照を解放する。
#ifdef USE_IMGUI
    EditorCommandRegistry::GetInstance()->UnregisterOwner(this);

    // 選択中のオブジェクトがこのガイドなら、破棄後に参照が残らないよう選択を解除する。
    if (EditorManager::GetInstance()->GetSelectedObject() == this) {
        EditorManager::GetInstance()->ClearSelection();
    }
#endif
    // 表示先の情報をすべて外してから所有Spriteを片付け、再初期化時に前のページを持ち越しません。
    bookElements_.clear();
    bookData_ = {};
    bookKey_.clear();
    bookEnabled_ = false;
    bookVisible_ = false;
    sprites_.clear();
    background_ = {};
    leftArrow_ = {};
    rightArrow_ = {};
    pageDots_ = {};
    pages_ = {};
    spriteCommon_ = nullptr;
    inputManager_ = nullptr;
    player_ = nullptr;
    isActive_ = false;
    pauseMode_ = false;
    menuPageRequested_ = false;
    isSliding_ = false;
    portraitTexturePath_.clear();
    abilityLabelTexturePaths_ = {};
    abilityActionAvailable_ = {};
    inputGlyphGamepadMode_ = false;
    inputGlyphBindingSignature_.clear();
}

void ControlsGuideOverlay::SetActive(bool active) {
    // 開閉の切替時にスライドを止め、開いた同じ入力で閉じないよう抑制する。
    if (isActive_ == active) {
        return;
    }

    isActive_ = active;
    // 閉じた場合は再生を止め、一時的な動画無効化を次の表示に持ち越しません。
    if (!active) {
        CloseMovePreview();
        previewPlaybackEnabled_ = true;
    }
    menuPageRequested_ = false;
    // 閉じたときだけエディターで選んだ能力を解除する。
    if (!active)
        bookPreviewKey_.clear();
    // 開く操作はキーが離れるまで再入力を抑止し、開閉のたびにページ遷移を初期化する。
    suppressToggleInput_ = active;
    isSliding_ = false;
    slideTimer_ = 0.0f;
    outgoingPage_ = currentPage_;
    incomingPage_ = currentPage_;
    RefreshAbilityVisuals();
    // 開く際は表示前に入力機器に合わせて画像を強制同期する。
    if (active)
        RefreshInputGlyphs(true);
    SetAllVisible(active);

    // 開いた場合は選択ページを中央に置き、矢印とページ位置も更新する。
    if (active) {
        SetPageVisible(currentPage_, true);
        SetPageOffset(currentPage_, 0.0f);
        UpdateStaticVisuals();
    }
}

void ControlsGuideOverlay::SetPauseMode(bool pauseMode) {
    // 独立ガイドとポーズ内の能力ページで、閉じ方を示すフッターだけ切り替える。
    if (pauseMode_ == pauseMode) {
        return;
    }

    pauseMode_ = pauseMode;
    RefreshInputGlyphs(true);
}

void ControlsGuideOverlay::OpenAbilityPage(bool resetScroll) {
    // 前回見ていた基本操作ページが残っていても、ポーズからは現在能力を直接確認する。
    currentPage_ = 1;
    isSliding_ = false;

    // 呼び出し元が初期位置を要求した場合だけ、記録中のスクロール値もゼロに戻す。
    if (resetScroll) {
        bookTargetScroll_ = bookScroll_ = 0.0f;
    }

    SetActive(true);
}

bool ControlsGuideOverlay::ConsumeMenuPageRequest() {
    // ポーズメニューへの遷移要求を一度だけ取り出す。
    const bool requested = menuPageRequested_;
    menuPageRequested_ = false;
    return requested;
}

// ページ入力より先に能力と入力機器の変化を反映します。閉じても選択ページは保持します。
void ControlsGuideOverlay::Update(float deltaTime, bool acceptInput) {
    // 閉じている間は入力も表示演出も更新しない。
    if (!isActive_) {
        return;
    }

    // 表示中は入力を受け付けないフレームも、能力表示と入力デバイスに応じた案内を同期する。
    time_ += deltaTime;
    RefreshAbilityVisuals();
    RefreshInputGlyphs();

    // 開いた操作が離れるまでは解除の確認だけ行う。解除後、入力許可があり文字入力中でない場合にメニュー操作を読む。
    if (suppressToggleInput_) {
        if (!IsToggleHeld()) {
            suppressToggleInput_ = false;
        }
    } else if (acceptInput && inputManager_
#ifdef USE_IMGUI
               && (!ImGui::GetCurrentContext() || !ImGui::GetIO().WantTextInput)
#endif
    ) {
        const auto menu = menuControls::Read(menuControls::Screen::kGuide, *inputManager_);
        // 閉じる入力があれば、そのフレームの残りの操作と表示更新を打ち切る。
        if (menu.cancel) {
            SetActive(false);
            return;
        }

        // ページが移動中は次の切り替えを保留し、表示中のページにだけ上下・決定を適用する。
        if (!isSliding_) {
            // 能力ページに技があり、上下の片方だけが押されたときは技を循環選択する。
            if (currentPage_ == 1 && bookMoveCount_ > 0 && menu.up != menu.down) {
                previewMoveIndex_ = (previewMoveIndex_ + (menu.down ? 1 : -1) + bookMoveCount_) % bookMoveCount_;
            }
            const bool left = menu.left, right = menu.right;
            const bool confirm = menu.confirm;
            // 決定を方向入力より優先する。
            if (confirm) {
                // ポーズ中の能力ページでは親のメニューへ遷移を要求し、それ以外の決定は次のガイドページへ送る。
                if (pauseMode_ && currentPage_ == 1)
                    menuPageRequested_ = true;
                else StartSlide(1);
            } else if (left != right) {
                // 左右の一方だけなら、ポーズ中の能力ページから右でメニューへ進む。ポーズ中の基本操作から左への循環は止める。
                if (pauseMode_ && currentPage_ == 1 && right) {
                    menuPageRequested_ = true;
                } else if (!(pauseMode_ && currentPage_ == 0 && left)) {
                    StartSlide(left ? -1 : 1);
                }
            }
        }
    }

    // 入力を処理した後でページと技のプレビューを進め、各部品の表示状態を整える。
    UpdateBook(deltaTime);
    UpdateSlide(deltaTime);
    UpdateMovePreview(deltaTime);
    UpdateStaticVisuals();

    // 最後に表示中の部品だけ行列を更新し、移動後の現在位置で描けるようにする。
    for (auto& sprite : sprites_) {
        if (sprite && sprite->IsVisible()) {
            sprite->Update();
        }
    }
}

void ControlsGuideOverlay::SetMenuTransitionOffset(float offsetX, bool transitioning) {
    // 表示中のページを横に動かし、遷移中は配置済みの矢印とページ点を隠す。背景位置は固定する。
    SetPageOffset(currentPage_, offsetX);

    for (Element* element : {&leftArrow_, &rightArrow_, &pageDots_[0], &pageDots_[1]})
        if (element->sprite)
            element->sprite->SetVisible(isActive_ && !transitioning);
    // 配置が変わっても非表示のSpriteは行列更新を省く。
    for (auto& sprite : sprites_)
        if (sprite && sprite->IsVisible())
            sprite->Update();
}

void ControlsGuideOverlay::Draw() {
    // ガイドが閉じている間は所有スプライトの描画を全て省く。
    if (!isActive_) {
        return;
    }

    // 映像枠は再生画像の上に描くため、一般の可視部品の描画から外す。
    engine::Sprite* videoFrame = FindSprite("controls_guide_page_1_book_video_frame");
    for (auto& sprite : sprites_) {
        if (sprite && sprite.get() != videoFrame && sprite->IsVisible()) {
            sprite->Draw();
        }
    }
    DrawMovePreview();
    // 映像の上へ縁を重ねる。再生画像がない間やページ移動中は枠も表示しない。
    if (videoFrame && videoFrame->IsVisible()) {
        videoFrame->SetVisible(movePreviewSprite_ && movePreviewSprite_->IsVisible());
    }
    // 映像を表示できるときだけ枠の位置とサイズを合わせて重ねる。
    if (videoFrame && videoFrame->IsVisible()) {
        videoFrame->SetPosition(movePreviewSprite_->GetPosition());
        videoFrame->SetSize(movePreviewSprite_->GetSize());
        videoFrame->Draw();
    }
}

void ControlsGuideOverlay::CollectReplaySprites(std::vector<engine::Sprite*>& replaySprites) const {
    // 生成済みの所有Spriteをリプレイの一覧へ渡す。所有権はガイドに残る。
    replaySprites.reserve(replaySprites.size() + sprites_.size());

    for (const auto& sprite : sprites_) {
        if (sprite) {
            replaySprites.push_back(sprite.get());
        }
    }
}

void ControlsGuideOverlay::CaptureReplayState(json& state) const {
    // 開閉、ページ移動、図鑑スクロールを同じフレームへ戻せるよう記録する。
    state = {{"active", isActive_},
             {"pauseMode", pauseMode_},
             {"suppressInput", suppressToggleInput_},
             {"sliding", isSliding_},
             {"currentPage", currentPage_},
             {"outgoingPage", outgoingPage_},
             {"incomingPage", incomingPage_},
             {"slideDirection", slideDirection_},
             {"slideTimer", slideTimer_},
             {"time", time_}};
    state["bookScroll"] = bookScroll_;
    state["bookTargetScroll"] = bookTargetScroll_;
}

void ControlsGuideOverlay::RestoreReplayState(const json& state) {
    // 記録値をページ・スクロール範囲へ収め、再生位置に合わせて表示を組み直す。
    // 記録がJSONオブジェクトでなければ、現在の表示状態を保持する。
    if (!state.is_object()) {
        return;
    }
    // リプレイにはデコード中のフレームを記録しないため、ライブ表示を先に外す。
    CloseMovePreview();

    isActive_ = state.value("active", isActive_);
    pauseMode_ = state.value("pauseMode", pauseMode_);
    suppressToggleInput_ = state.value("suppressInput", suppressToggleInput_);
    isSliding_ = state.value("sliding", isSliding_);
    currentPage_ = std::clamp(state.value("currentPage", currentPage_), 0, kPageCount - 1);
    outgoingPage_ = std::clamp(state.value("outgoingPage", outgoingPage_), 0, kPageCount - 1);
    incomingPage_ = std::clamp(state.value("incomingPage", incomingPage_), 0, kPageCount - 1);
    slideDirection_ = state.value("slideDirection", slideDirection_) < 0 ? -1 : 1;
    slideTimer_ = std::max(0.0f, state.value("slideTimer", slideTimer_));
    time_ = state.value("time", time_);

    RefreshAbilityVisuals();
    RefreshInputGlyphs(true);
    bookScroll_ = std::clamp(state.value("bookScroll", 0.0f), 0.0f, bookMaxScroll_);
    bookTargetScroll_ = std::clamp(state.value("bookTargetScroll", bookScroll_), 0.0f, bookMaxScroll_);
    SetAllVisible(isActive_);

    // 記録上で開いていた場合だけ、経過時間を加えずに遷移中の配置を再現する。
    if (isActive_) {
        UpdateSlide(0.0f);
        UpdateStaticVisuals();
    }
}

void ControlsGuideOverlay::LoadLayout(const std::string& layoutPath) {
    // 再読み込み時は前回の攻略本を持ち越さず、現在のレイアウトから参照を作り直す。
    layoutPath_ = layoutPath;
    bookElements_.clear();
    bookEnabled_ = false;
    bookData_ = {};
    bookKey_.clear();
    sprites_.clear();

    // Spriteの生成基盤がなければ、古い部品を破棄した状態で読込を止める。
    if (!spriteCommon_) {
        return;
    }

    std::ifstream file(layoutPath);

    // ファイルを開けなければ配置の生成を始めない。
    if (!file.is_open()) {
        return;
    }

    json root;
    try {
        file >> root;
    } catch (...) {
        // JSONを解釈できなければ、生成する部品がないまま戻る。
        return;
    }

    // 生成元のspritesが配列でなければ、要素を読み取れないので中止する。
    if (!root.contains("sprites") || !root["sprites"].is_array()) {
        return;
    }

    // デザイン時の位置から、現在の描画サイズに合わせる比率を求める。
    const auto layoutScale = spriteLayoutScaler::Make(root);
    layoutRatio_ = layoutScale.enabled ? layoutScale.ratio : engine::Vector2{1.0f, 1.0f};

    // abilityBookがオブジェクトなら、旧形式にも使う表示範囲と技プレビューの設定を読む。
    if (root.contains("abilityBook") && root["abilityBook"].is_object()) {
        const auto& book = root["abilityBook"];
        const float scaleY = layoutScale.enabled ? layoutScale.ratio.y : 1.0f;
        bookEnabled_ = spriteLayoutScaler::ReadBool(book, "enabled", true);
        bookTop_ = spriteLayoutScaler::ReadFloat(book, "viewportTop", 492.0f) * scaleY;
        bookBottom_ = std::max(bookTop_ + 1.0f,
                               spriteLayoutScaler::ReadFloat(book, "viewportBottom", 878.0f) * scaleY);
        bookMaxScroll_ = std::max(0.0f,
                                  spriteLayoutScaler::ReadFloat(book, "contentBottom", 1520.0f) * scaleY - bookBottom_);
        bookScrollSpeed_ = std::max(1.0f, spriteLayoutScaler::ReadFloat(book, "scrollSpeed", 650.0f)) * scaleY;
        bookWheelStep_ = std::max(1.0f, spriteLayoutScaler::ReadFloat(book, "wheelStep", 125.0f)) * scaleY;
        bookBob_ = std::clamp(spriteLayoutScaler::ReadFloat(book, "portraitBob", 4.0f), 0.0f, 12.0f) * scaleY;
        // 映像と説明の配置は、movePreviewがオブジェクトの場合にデザイン座標で読み取る。
        const auto preview = book.value("movePreview", json::object());
        if (preview.is_object()) {
            movePreviewPosition_ = spriteLayoutScaler::ReadVector2(preview.value("position", json::array()), {1008.0f, 418.0f});
            movePreviewSize_ = spriteLayoutScaler::ReadVector2(preview.value("size", json::array()), {568.0f, 320.0f});
        }
        previewBookPath_ = spriteLayoutScaler::ReadString(book, "dataPath", "Resources/json/ui/abilityGuideBook.json");
        try {
            std::ifstream dataFile(previewBookPath_);
            // データファイルが開けた場合だけ図鑑の内容を読み込む。
            if (dataFile)
                dataFile >> bookData_;
        } catch (...) {
            // データの構文エラー時は図鑑内容を空にする。ガイド自体の配置は読み込む。
            bookData_ = {};
        }
    }

    // 背景やボタンなどの部品を配置から生成し、名前で操作する表示先を紐づけていく。
    for (const auto& spriteData : root["sprites"]) {
        // オブジェクト以外の配列要素からはSpriteを作らない。
        if (!spriteData.is_object()) {
            continue;
        }

        // 任意の切り抜き領域は4つの有限な数値がそろった場合だけ適用します。
        bool hasTextureCrop = false;
        engine::Vector2 textureOrigin{};
        engine::Vector2 textureSize{};

        if (spriteData.contains("textureCrop")) {
            const auto& crop = spriteData["textureCrop"];
            if (crop.is_array() && crop.size() >= 4 && crop[0].is_number() && crop[1].is_number() &&
                crop[2].is_number() && crop[3].is_number()) {
                textureOrigin = {crop[0].get<float>(), crop[1].get<float>()};
                textureSize = {crop[2].get<float>(), crop[3].get<float>()};
                hasTextureCrop = std::isfinite(textureOrigin.x) && std::isfinite(textureOrigin.y) &&
                                 std::isfinite(textureSize.x) && std::isfinite(textureSize.y);
            }
        }

        auto sprite = spriteLayoutFactory::Create(spriteCommon_, spriteData, layoutScale, "controls_guide_sprite");
        const std::string name = sprite->GetName();
        const uint32_t textureHandle = sprite->GetTextureHandle();

        // 有限な切り抜き領域を取得できた場合だけ、既定の画像範囲を上書きする。
        if (hasTextureCrop) {
            sprite->SetTextureRect(textureOrigin, textureSize);
        }

        sprite->Update();
        sprites_.push_back(std::move(sprite));

        // 図鑑が有効でbookElementを指定した部品だけ図鑑管理へ加える。スクロールするかは各部品のbookScrollで決める。
        if (bookEnabled_ && spriteLayoutScaler::ReadBool(spriteData, "bookElement", false)) {
            BookElement element;
            element.visual = BindElement(name);
            element.slot = spriteLayoutScaler::ReadString(spriteData, "bookSlot", "");
            element.scroll = spriteLayoutScaler::ReadBool(spriteData, "bookScroll", false);
            element.row = spriteLayoutScaler::ReadInt(spriteData, "bookRow", -1);
            const auto& metadata = engine::TextureManager::GetInstance()->GetMetadata(textureHandle);
            element.textureSize = {static_cast<float>(metadata.width), static_cast<float>(metadata.height)};

            // 切り抜き済みの部品は再配置やクリッピングでも同じ範囲を基準にする。
            if (hasTextureCrop) {
                element.textureOrigin = textureOrigin;
                element.textureSize = textureSize;
            }

            bookElements_.push_back(std::move(element));
        }
    }
}

void ControlsGuideOverlay::BindLayoutSprites() {
    // JSONで生成した名前付きSpriteをページ・矢印・図鑑要素へ割り当てる。
    background_ = BindElement("controls_guide_background");
    leftArrow_ = BindElement("controls_guide_left_arrow");
    rightArrow_ = BindElement("controls_guide_right_arrow");

    // ページ名の共通接頭辞から静的な部品と操作行を集め、ページ単位で表示を切り替えられるようにします。
    for (std::int32_t page = 0; page < kPageCount; ++page) {
        const std::string prefix = "controls_guide_page_" + std::to_string(page);
        pages_[static_cast<size_t>(page)].panel = BindElement(prefix + "_panel");
        pages_[static_cast<size_t>(page)].portrait = BindElement(prefix + "_portrait");
        auto& details = pages_[static_cast<size_t>(page)].details;
        details.clear();

        for (const auto& sprite : sprites_) {
            if (sprite && sprite->GetName().rfind(prefix + "_detail_", 0) == 0) {
                details.push_back(BindElement(sprite->GetName()));
            }
        }

        for (std::int32_t action = 0; action < kActionCount; ++action) {
            pages_[static_cast<size_t>(page)].actionGlyphs[static_cast<size_t>(action)] =
                BindElement(prefix + "_action_" + std::to_string(action));
            pages_[static_cast<size_t>(page)].actionLabels[static_cast<size_t>(action)] =
                BindElement(prefix + "_action_label_" + std::to_string(action));
        }

        pageDots_[static_cast<size_t>(page)] = BindElement("controls_guide_page_dot_" + std::to_string(page));
    }
}

engine::Sprite* ControlsGuideOverlay::FindSprite(const std::string& name) const {
    // レイアウト名が一致する所有Spriteを探し、未定義ならnullptrを返す。
    for (const auto& sprite : sprites_) {
        if (sprite && sprite->GetName() == name) {
            return sprite.get();
        }
    }

    return nullptr;
}

ControlsGuideOverlay::Element ControlsGuideOverlay::BindElement(const std::string& name) const {
    // 名前と一致するSpriteがあれば、移動・拡縮を戻すための配置と色を保存する。
    Element element;
    element.sprite = FindSprite(name);

    if (element.sprite) {
        element.basePosition = element.sprite->GetPosition();
        element.baseSize = element.sprite->GetSize();
        element.baseColor = element.sprite->GetColor();
    }

    return element;
}

void ControlsGuideOverlay::SetAllVisible(bool visible) {
    // 作成済みの部品の可視性をそろえる。開くときはその後で現在ページ以外を隠す。
    bookVisible_ = visible && currentPage_ == 1;

    for (auto& sprite : sprites_) {
        if (sprite) {
            sprite->SetVisible(visible);
        }
    }

    // 閉じる場合は全部品が非表示なので、ページごとの切替は行わない。
    if (!visible) {
        return;
    }

    for (std::int32_t page = 0; page < kPageCount; ++page) {
        SetPageVisible(page, page == currentPage_);
    }
}

void ControlsGuideOverlay::SetPageVisible(std::int32_t pageIndex, bool visible) {
    // 指定ページに属する要素だけを切り替え、他ページの状態を保持する。
    if (pageIndex < 0 || pageIndex >= kPageCount) {
        return;
    }

    PageVisual& page = pages_[static_cast<size_t>(pageIndex)];

    // 図鑑ページでは通常部品に加え、選択中の技とメディアの有無を使う配置処理も更新します。
    if (pageIndex == 1 && bookEnabled_) {
        bookVisible_ = visible;
        LayoutBook(bookOffsetX_);
    }

    // パネル・肖像・詳細のうち、そのレイアウトに存在する部品の可視性を更新する。
    if (page.panel.sprite)
        page.panel.sprite->SetVisible(visible);
    if (page.portrait.sprite)
        page.portrait.sprite->SetVisible(visible);
    for (Element& detail : page.details) {
        if (detail.sprite)
            detail.sprite->SetVisible(visible);
    }

    // 配置済みの操作画像と文字を対象に、能力ページで説明のない行はページを開いていても隠す。
    for (std::int32_t action = 0; action < kActionCount; ++action) {
        const bool actionVisible = visible && (pageIndex != 1 || abilityActionAvailable_[static_cast<size_t>(action)]);
        Element& glyph = page.actionGlyphs[static_cast<size_t>(action)];
        Element& label = page.actionLabels[static_cast<size_t>(action)];
        if (glyph.sprite)
            glyph.sprite->SetVisible(actionVisible);
        if (label.sprite)
            label.sprite->SetVisible(actionVisible);
    }

}

void ControlsGuideOverlay::SetPageOffset(std::int32_t pageIndex, float offsetX) {
    // ページ移動量を基準配置へ加え、スライド後に位置を戻せるようにする。
    if (pageIndex < 0 || pageIndex >= kPageCount) {
        return;
    }

    PageVisual& page = pages_[static_cast<size_t>(pageIndex)];
    // 配置された部品のX座標だけを基準位置からずらす。
    const auto applyOffset = [offsetX](Element& element) {
        if (element.sprite) {
            element.sprite->SetPosition({element.basePosition.x + offsetX, element.basePosition.y});
        }
    };

    applyOffset(page.panel);
    applyOffset(page.portrait);

    for (Element& detail : page.details)
        applyOffset(detail);
    for (size_t index = 0; index < page.actionGlyphs.size(); ++index) {
        Element& glyph = page.actionGlyphs[index];
        applyOffset(glyph);
        const engine::Vector2 textureSize = basicGlyphTextureSizes_[index];

        if (pageIndex == 0 && glyph.sprite && textureSize.x > 0.0f && textureSize.y > 0.0f) {
            // 縮小によって生じる余白を両側へ配分する。左上寄せではSpaceやパッドだけ中心がずれる。
            const float fit = (std::min)(glyph.baseSize.x / textureSize.x, glyph.baseSize.y / textureSize.y);
            const engine::Vector2 size = {textureSize.x * fit, textureSize.y * fit};
            const engine::Vector2 anchor = glyph.sprite->GetAnchorPoint();
            glyph.sprite->SetSize(size);
            glyph.sprite->SetPosition({
                glyph.basePosition.x + offsetX + (glyph.baseSize.x - size.x) * (0.5f - anchor.x),
                glyph.basePosition.y + (glyph.baseSize.y - size.y) * (0.5f - anchor.y)});
        }
    }

    for (Element& label : page.actionLabels) {
        applyOffset(label);
    }

    // 図鑑が有効な能力ページでは、図鑑内の部品にもページ移動量を渡す。
    if (pageIndex == 1 && bookEnabled_)
        LayoutBook(offsetX);
}

void ControlsGuideOverlay::StartSlide(std::int32_t direction) {
    // 移動中の再操作と方向ゼロを無視し、開始できるときは送り元と送り先の両方を表示する。
    if (isSliding_ || direction == 0) {
        return;
    }

    slideDirection_ = direction < 0 ? -1 : 1;
    outgoingPage_ = currentPage_;
    incomingPage_ = (currentPage_ + slideDirection_ + kPageCount) % kPageCount;
    slideTimer_ = 0.0f;
    isSliding_ = true;
    SetPageVisible(outgoingPage_, true);
    SetPageVisible(incomingPage_, true);
    UpdateSlide(0.0f);
}

void ControlsGuideOverlay::UpdateSlide(float deltaTime) {
    // 同じ補間率で二つのページを送り、完了時に送り元を隠す。
    // 移動中でなければ、現在ページを中央へ戻すだけでよい。
    if (!isSliding_) {
        SetPageOffset(currentPage_, 0.0f);
        return;
    }

    slideTimer_ += std::max(0.0f, deltaTime);
    const float rate = slideDuration_ > 0.0f ? std::clamp(slideTimer_ / slideDuration_, 0.0f, 1.0f) : 1.0f;
    const float eased = Easing::SmoothStep01(rate);
    // 背景の幅に比例する移動量を使い、背景未設定の場合だけ固定の画面座標を使う。
    const float slideDistance = background_.baseSize.x > 0.0f ? background_.baseSize.x * 0.86f : 1650.0f;

    // 送り元を中心から画面外へ動かし、送り先は反対側の外から同じ方向・同じ進行率で中心へ運ぶ。
    SetPageOffset(outgoingPage_, -static_cast<float>(slideDirection_) * slideDistance * eased);
    SetPageOffset(incomingPage_, static_cast<float>(slideDirection_) * slideDistance * (1.0f - eased));

    // 送り元を非表示にしてから送り先を現ページとして確定し、ページ位置をゼロへそろえる。
    if (rate >= 1.0f) {
        SetPageVisible(outgoingPage_, false);
        currentPage_ = incomingPage_;
        SetPageOffset(currentPage_, 0.0f);
        isSliding_ = false;
        slideTimer_ = 0.0f;
    }
}

void ControlsGuideOverlay::UpdateStaticVisuals() {
    // 選択ページに応じた矢印やページ位置の表示を更新する。
    const float pulse = 0.5f + 0.5f * std::sin(time_ * 5.2f);

    const auto updateArrow = [pulse](Element& arrow, float phase) {
        // 配置がない矢印はアニメーションの対象外。
        if (!arrow.sprite) {
            return;
        }

        const float scale = 1.0f + pulse * 0.08f;
        arrow.sprite->SetSize({arrow.baseSize.x * scale, arrow.baseSize.y * scale});
        arrow.sprite->SetPosition({arrow.basePosition.x + std::sin(phase) * 3.0f, arrow.basePosition.y});
        arrow.sprite->SetColor({1.0f, 0.96f, 0.62f + pulse * 0.18f, 1.0f});
    };

    // 左右の矢印は位相を半周ずらし、それぞれの基準位置の周囲で動かします。
    updateArrow(leftArrow_, time_ * 4.4f);
    updateArrow(rightArrow_, time_ * 4.4f + std::numbers::pi_v<float>);

    // ページ送りの途中は到着予定のページも強調し、遷移先が分かるようにします。
    for (std::int32_t page = 0; page < kPageCount; ++page) {
        Element& dot = pageDots_[static_cast<size_t>(page)];

        // ページ点が未配置なら、次の点を調べる。
        if (!dot.sprite) {
            continue;
        }

        const bool selected = page == currentPage_ || (isSliding_ && page == incomingPage_);
        const float scale = selected ? 1.16f : 0.82f;
        dot.sprite->SetSize({dot.baseSize.x * scale, dot.baseSize.y * scale});
        dot.sprite->SetColor(selected ? engine::Vector4{1.0f, 0.86f, 0.28f, 1.0f} : engine::Vector4{0.65f, 0.90f, 1.0f, 0.72f});
    }
}

void ControlsGuideOverlay::RefreshAbilityVisuals() {
    // 図鑑を使うレイアウトでは、旧形式の三行ラベルを更新せず図鑑全体を差し替えます。
    if (bookEnabled_) {
        RefreshBook();
        return;
    }

    const std::string nextPath = ResolvePortraitTexture(player_);

    // 能力から求めた肖像のパスが変わったときだけ更新候補にする。
    if (nextPath != portraitTexturePath_) {
        portraitTexturePath_ = nextPath;
        engine::Sprite* portrait = pages_[1].portrait.sprite;

        // 配置に肖像がある場合だけ、新しい画像を読み込む。
        if (portrait) {
            const uint32_t textureHandle = LoadGuideTexture(nextPath);
            SetFullTextureRect(portrait, textureHandle);
            portrait->SetTextureName(nextPath);
        }
    }

    const AbilityGuideVisual guide = ResolveAbilityGuide(player_);
    bool guideChanged = false;

    // 対応画像の有無を各行に記録し、説明が空の行はページの表示条件から外します。
    for (std::int32_t action = 0; action < kActionCount; ++action) {
        const char* texturePath = guide.labelTextures[static_cast<size_t>(action)];
        const std::string nextLabelPath = texturePath ? texturePath : "";
        const bool available = !nextLabelPath.empty();

        guideChanged |= abilityActionAvailable_[static_cast<size_t>(action)] != available;
        abilityActionAvailable_[static_cast<size_t>(action)] = available;

        // 画像が同じ行は可否の記録だけ行い、画像は差し替えない。
        if (abilityLabelTexturePaths_[static_cast<size_t>(action)] == nextLabelPath) {
            continue;
        }

        guideChanged = true;
        abilityLabelTexturePaths_[static_cast<size_t>(action)] = nextLabelPath;
        engine::Sprite* label = pages_[1].actionLabels[static_cast<size_t>(action)].sprite;

        // 表示先と説明画像の両方がある行だけ読み込む。
        if (label && available) {
            const uint32_t textureHandle = LoadGuideTexture(nextLabelPath);
            SetFullTextureRect(label, textureHandle);
            label->SetTextureName(nextLabelPath);
        }
    }

    // 表示中に説明の構成が変わったときだけ、遷移の送り元・送り先を考慮して可視性を更新します。
    if (!guideChanged || !isActive_) {
        return;
    }

    const bool abilityPageVisible = isSliding_ ? (outgoingPage_ == 1 || incomingPage_ == 1) : currentPage_ == 1;
    SetPageVisible(1, abilityPageVisible);
}

void ControlsGuideOverlay::RefreshInputGlyphs(bool force) {
    // 強制更新時か、入力機器・割り当てのどちらかが変わった場合に表示を差し替える。
    const bool gamepadMode = inputManager_ && inputManager_->IsGamepadMode();
    std::string signature = GetInputGlyphBindingSignature();
    if (!force && inputGlyphGamepadMode_ == gamepadMode && inputGlyphBindingSignature_ == signature)
        return;
    inputGlyphGamepadMode_ = gamepadMode;
    inputGlyphBindingSignature_ = std::move(signature);

    // 配置されている閉じる・決定の案内を探し、入力機器に応じた画像へ必要な場合だけ差し替える。
    const auto& controls = menuControls::Get(menuControls::Screen::kGuide);

    for (const char* action : {"close", "confirm"}) {
        engine::Sprite* footer = FindSprite(std::string("controls_guide_footer_") + action + "_text");
        if (!footer)
            continue;
        const std::string path = (std::string(action) == "close" ? controls.closeHint : controls.confirmHint).Resolve(gamepadMode);

        if (footer->GetTextureName() != path) {
            SetFullTextureRect(footer, LoadGuideTexture(path));
            footer->SetTextureName(path);
        }
    }

    // 配置済みの各操作行の条件文も入力機器と同期する。既に同じ画像が設定されていれば読み込みを省く。
    for (std::int32_t action = 0; action < kActionCount; ++action) {
        if (engine::Sprite* condition = FindSprite("controls_guide_page_0_detail_condition_" + std::to_string(action))) {
            const std::string path = "ui/control_guide/book/basics/move_" + std::to_string(action) +
                                     (gamepadMode ? "_condition_gamepad.png" : "_condition.png");
            if (condition->GetTextureName() != path) {
                SetFullTextureRect(condition, LoadGuideTexture(path));
                condition->SetTextureName(path);
            }
        }
    }

    // 基本ページのフッターがあれば、ポーズ内か単独表示かも反映する。
    if (engine::Sprite* footer = FindSprite("controls_guide_page_0_detail_footer")) {
        const char* path;
        if (pauseMode_) {
            path = gamepadMode ? "generated/text/pause_guide/basic_footer_gamepad.png"
                               : "generated/text/pause_guide/basic_footer_keyboard.png";
        } else {
            path = gamepadMode ? "ui/control_guide/book/basics/footer_gamepad.png"
                               : "ui/control_guide/book/basics/footer.png";
        }
        // 計算した画像名と現在の画像が異なる場合だけ差し替える。
        if (footer->GetTextureName() != path) {
            SetFullTextureRect(footer, LoadGuideTexture(path));
            footer->SetTextureName(path);
        }
    }

    // 移動・ジャンプ・視点操作の入力画像は、後で枠に合わせるため元の画像サイズも保存する。
    constexpr std::array<const char*, kActionCount> kBasicGlyphs = {
        "ui/input/combo/combo_wasd.png", "ui/input/keyboard/key_space.png", "ui/input/mouse/mouse_move.png"};
    for (std::int32_t action = 0; action < kActionCount; ++action) {
        Element& visual = pages_[0].actionGlyphs[static_cast<size_t>(action)];
        // 配置のない入力画像は更新しない。
        if (!visual.sprite)
            continue;
        basicGlyphTextureSizes_[static_cast<size_t>(action)] = SetGlyphTexture(
            visual.sprite, ResolveInputGlyph(kBasicGlyphs[static_cast<size_t>(action)], gamepadMode));
    }

    const json fallback = json::object();
    const json& entry = bookData_.contains(bookKey_) ? bookData_[bookKey_] : fallback;

    for (BookElement& item : bookElements_) {
        // 図鑑のフッターがある場合は、ポーズ内か単独表示かで案内を選ぶ。
        if (item.visual.sprite && item.visual.sprite->GetName() == "controls_guide_page_1_book_footer") {
            const char* path;
            if (pauseMode_) {
                path = gamepadMode ? "generated/text/pause_guide/page_footer_gamepad.png"
                                   : "generated/text/pause_guide/page_footer_keyboard.png";
            } else {
                path = gamepadMode ? "ui/control_guide/book/footer_gamepad.png"
                                   : "ui/control_guide/book/footer.png";
            }
            // 画像を差し替えるときは図鑑の配置に使う切り抜き範囲も画像全体に戻す。
            if (item.visual.sprite->GetTextureName() != path) {
                const uint32_t handle = LoadGuideTexture(path);
                SetFullTextureRect(item.visual.sprite, handle);
                item.visual.sprite->SetTextureName(path);
                const auto& metadata = engine::TextureManager::GetInstance()->GetMetadata(handle);
                item.textureOrigin = {};
                item.textureSize = {static_cast<float>(metadata.width), static_cast<float>(metadata.height)};
            }
        }

        // 以降は、図鑑内でキー画像として登録されていて実体がある部品だけを更新する。
        if ((item.slot.rfind("glyph_", 0) != 0 && item.slot.rfind("selected_glyph_", 0) != 0) || !item.visual.sprite)
            continue;
        const std::string keyboardPath =
            item.row >= 0 && item.row < bookMoveCount_
                ? entry["moves"][item.row].value("glyph", std::string("ui/input/keyboard/key_e.png"))
                : "ui/input/keyboard/key_e.png";
        const InputGlyphSpec glyph = ResolveInputGlyph(keyboardPath, gamepadMode);
        item.textureSize = SetGlyphTexture(item.visual.sprite, glyph);
        item.textureOrigin = glyph.useCrop ? engine::Vector2{glyph.cropX, glyph.cropY} : engine::Vector2{};
    }

    // 図鑑が有効なら、差し替えた画像寸法で配置を計算し直す。
    if (bookEnabled_)
        LayoutBook(bookOffsetX_);
}

void ControlsGuideOverlay::RefreshBook() {
    // エディターが指定した能力を優先し、表示中の能力キーが変わった場合だけ図鑑と選択位置を組み直す。
    const std::string nextKey = bookPreviewKey_.empty() ? ResolveBookKey(player_) : bookPreviewKey_;
    if (nextKey == bookKey_)
        return;
    bookKey_ = nextKey;
    previewMoveIndex_ = 0;
    bookScroll_ = bookTargetScroll_ = 0.0f;
    const json fallback = json::object();
    const json& entry = bookData_.contains(bookKey_) ? bookData_[bookKey_] : fallback;
    bookMoveCount_ = entry.contains("moves") && entry["moves"].is_array() ? static_cast<std::int32_t>(entry["moves"].size()) : 0;

    for (BookElement& item : bookElements_) {
        // 能力で画像を替えない部品、用途未指定、実体がない部品は読み直さない。
        if (item.slot.empty() || item.slot.rfind("static_", 0) == 0 || !item.visual.sprite)
            continue;
        // selected_ は選択中の技だけ表示する詳細欄。画像は一覧と同じ素材を共有する。
        const std::string slot = (item.slot.rfind("selected_", 0) == 0 || item.slot.rfind("fallback_", 0) == 0) ? item.slot.substr(9) : item.slot;
        std::string path = "ui/control_guide/book/" + bookKey_ + "/" + slot + ".png";
        // 技名・操作・説明は全能力で同じ寸法の画像を使う。覇王の補足だけ既存素材を共有する。
        if (bookKey_ == "crown" && (slot == "role" || slot == "traits" || slot == "tip"))
            path = "ui/control_guide/book/crown_gate/" + slot + ".png";

        // キー欄は技の割り当てを現在の入力機器の画像に変換し、画像全体の読込処理を通さない。
        if (slot.rfind("glyph_", 0) == 0) {
            path = item.row >= 0 && item.row < bookMoveCount_
                       ? entry["moves"][item.row].value("glyph", std::string("ui/input/keyboard/key_e.png"))
                       : "ui/input/keyboard/key_e.png";
            const InputGlyphSpec glyph = ResolveInputGlyph(path, inputManager_ && inputManager_->IsGamepadMode());
            item.textureSize = SetGlyphTexture(item.visual.sprite, glyph);
            item.textureOrigin = glyph.useCrop ? engine::Vector2{glyph.cropX, glyph.cropY} : engine::Vector2{};
            continue;
        }

        const bool isThumbnail = slot.rfind("thumbnail_", 0) == 0;
        // サムネイルは技データの静止画を使い、指定や実ファイルがない場合は範囲ゼロのままにする。
        if (isThumbnail) {
            path = item.row >= 0 && item.row < bookMoveCount_
                       ? entry["moves"][item.row].value("previewPoster", std::string{})
                       : std::string{};
            item.textureOrigin = {};
            item.textureSize = {};
            if (path.empty() || !std::filesystem::is_regular_file(engine::Sprite::ResolveTexturePath(path))) {
                continue;
            }
        }

        const uint32_t handle = LoadGuideTexture(path);
        SetFullTextureRect(item.visual.sprite, handle);
        item.visual.sprite->SetTextureName(path);
        const auto& metadata = engine::TextureManager::GetInstance()->GetMetadata(handle);
        item.textureOrigin = {};
        item.textureSize = {static_cast<float>(metadata.width), static_cast<float>(metadata.height)};

        // 範囲は画像内のピクセル座標。古いデータや画像の差し替えで範囲が合わない場合は全体を使う。
        const auto readCrop = [&item](const json& crop) {
            if (!crop.is_array() || crop.size() != 4 ||
                !std::all_of(crop.begin(), crop.end(), [](const json& n) { return n.is_number(); })) {
                return;
            }
            const float x = crop[0].get<float>(), y = crop[1].get<float>();
            const float w = crop[2].get<float>(), h = crop[3].get<float>();
            // 画像の内側に収まる正の幅・高さを指定した範囲なら、全体から切り抜きへ切り替える。
            if (x >= 0.0f && y >= 0.0f && w > 0.0f && h > 0.0f &&
                x + w <= item.textureSize.x && y + h <= item.textureSize.y) {
                item.textureOrigin = {x, y};
                item.textureSize = {w, h};
            }
        };
        // サムネイルは技ごとの切り抜き指定を使う。
        if (isThumbnail) {
            const auto& move = entry["moves"][item.row];
            if (move.contains("thumbnailCrop"))
                readCrop(move["thumbnailCrop"]);
        } else if (FindSprite("controls_guide_page_1_book_intro_card") &&
                   entry.contains("textBounds") && entry["textBounds"].is_object()) {
            // カード形式の文字は、この欄の文字境界が指定されている場合だけ切り抜く。
            const auto& bounds = entry["textBounds"];
            if (bounds.contains(slot))
                readCrop(bounds[slot]);
        }
    }

    portraitTexturePath_ =
        nextKey == "other" ? ResolvePortraitTexture(player_) : "ui/control_guide/book/portraits/" + nextKey + ".png";
    // 肖像の配置がある場合は、文字と操作画像に続いて肖像も新しい能力の画像へ合わせる。
    if (engine::Sprite* portrait = pages_[1].portrait.sprite) {
        SetFullTextureRect(portrait, LoadGuideTexture(portraitTexturePath_));
        portrait->SetTextureName(portraitTexturePath_);
    }

    LayoutBook(bookOffsetX_);
}

void ControlsGuideOverlay::UpdateBook(float deltaTime) {
    // 能力と技の説明は一画面に収める。古いリプレイのスクロール値も表示へ影響させない。
    if (!bookEnabled_)
        return;
    (void)deltaTime;
    bookScroll_ = bookTargetScroll_ = 0.0f;
    LayoutBook(bookOffsetX_);
}

// 図鑑内のSpriteをページ移動量に合わせ、キー画像は枠内へ縮小します。
// スクロール領域はUVも一緒に切り取り、見切れた文字が枠外へ出ないようにします。
void ControlsGuideOverlay::LayoutBook(float offsetX) {
    bookOffsetX_ = offsetX;

    // 現在の能力に技一覧があり、選択番号がその範囲内なら映像または静止画の指定を確認する。
    bool hasMedia = false;
    if (bookData_.contains(bookKey_) && bookData_[bookKey_].contains("moves")) {
        const auto& moves = bookData_[bookKey_]["moves"];
        if (moves.is_array() && previewMoveIndex_ >= 0 && static_cast<size_t>(previewMoveIndex_) < moves.size()) {
            const auto& move = moves[previewMoveIndex_];
            for (const char* field : {"previewVideo", "previewPoster"})
                hasMedia |= move.contains(field) && move[field].is_string() && !move[field].get_ref<const std::string&>().empty();
        }
    }
    const bool cardLayout = FindSprite("controls_guide_page_1_book_intro_card") != nullptr;
    for (BookElement& item : bookElements_) {
        Element& element = item.visual;
        engine::Sprite* sprite = element.sprite;
        // 実体のない図鑑部品は表示や配置の更新を省く。
        if (!sprite)
            continue;
        // 技数に含まれる行だけを候補にし、選択専用・代替専用・画像のない部品を条件に応じて隠します。
        const bool available = item.row < 0 || item.row < bookMoveCount_;
        const bool selectedOnly = item.slot.rfind("selected_", 0) == 0 || item.slot == "static_selection";
        const bool fallbackOnly = item.slot.rfind("fallback_", 0) == 0 || item.slot == "static_fallback_detail";
        const bool thumbnail = item.slot.rfind("thumbnail_", 0) == 0;
        sprite->SetVisible(bookVisible_ && available && (!selectedOnly || item.row == previewMoveIndex_) &&
                           (!fallbackOnly || !hasMedia) && (!thumbnail || item.textureSize.x > 0.0f));
        // 非選択の技も読みやすさを保ち、選択は背景枠で示す。
        sprite->SetColor(element.baseColor);
        engine::Vector2 position = {element.basePosition.x + offsetX, element.basePosition.y};
        engine::Vector2 size = element.baseSize;
        engine::Vector2 uv = item.textureOrigin;
        engine::Vector2 uvSize = item.textureSize;

        if ((item.slot.rfind("glyph_", 0) == 0 || item.slot.rfind("selected_glyph_", 0) == 0) && item.textureSize.x > 0.0f && item.textureSize.y > 0.0f) {
            // キー画像の縦横のうち先に枠へ届く方の倍率に揃え、残る余白の半分を足して中央に置く。
            const float fit =
                (std::min)(element.baseSize.x / item.textureSize.x, element.baseSize.y / item.textureSize.y);
            size = {item.textureSize.x * fit, item.textureSize.y * fit};
            position.x += (element.baseSize.x - size.x) * 0.5f;
            position.y += (element.baseSize.y - size.y) * 0.5f;
        }

        const bool centerText = item.slot == "guide_title" || item.slot == "intro" ||
                                item.slot.ends_with("_description");
        const bool moveText = item.slot.rfind("move_", 0) == 0 ||
                              item.slot.rfind("selected_move_", 0) == 0;
        const bool listName = item.slot.rfind("move_", 0) == 0 && item.slot.ends_with("_name");
        const bool thumbnailHasImage = listName && std::any_of(bookElements_.begin(), bookElements_.end(), [&item](const BookElement& candidate) {
            return candidate.row == item.row && candidate.slot.rfind("thumbnail_", 0) == 0 &&
                   candidate.textureSize.x > 0.0f;
        });
        // サムネイルがない行は文字欄に余った幅を渡し、映像なしの場合は詳細説明を映像枠へ移します。
        if (cardLayout && listName && !thumbnailHasImage) {
            const float shift = 136.0f * layoutRatio_.x;
            position.x -= shift;
            size.x += shift;
        }
        // カード形式で映像素材の指定がない場合、選択中の技の説明を映像枠へ移す。
        if (!hasMedia && item.slot.rfind("selected_move_", 0) == 0 && item.slot.ends_with("_description") &&
            cardLayout) {
            position.y = (movePreviewPosition_.y + 24.0f) * layoutRatio_.y;
            size.y = 76.0f * layoutRatio_.y;
        }
        // 描画枠に収め、文字の実寸で上下中央を揃える。行頭は文字画像の配置を保つ。
        if (cardLayout && (centerText || moveText || thumbnail || item.slot == "fallback_traits") &&
            uvSize.x > 0.0f && uvSize.y > 0.0f) {
            const float fit = (std::min)(size.x / uvSize.x, size.y / uvSize.y);
            const engine::Vector2 fitted{uvSize.x * fit, uvSize.y * fit};
            // 横方向も中央にそろえるのは中央配置の文字とサムネイルだけ。
            if (centerText || thumbnail)
                position.x += (size.x - fitted.x) * 0.5f;
            position.y += (size.y - fitted.y) * 0.5f;
            size = fitted;
        }

        // 旧レイアウトでスクロール指定がある部品だけ、表示範囲と重なる部分を使います。
        if (item.scroll) {
            const float y = element.basePosition.y - bookScroll_;
            const float top = std::max(y, bookTop_);
            const float bottom = std::min(y + size.y, bookBottom_);

            // 上下の重なりがないか高さがゼロ以下なら、画像の一部も描けないので隠す。
            if (bottom <= top || size.y <= 0.0f) {
                sprite->SetVisible(false);
                continue;
            }

            // UVと描画サイズを同時に切り取り、巻物の上下へ文章をはみ出させない。
            // 切り取った画面の高さを画像全体に対する割合へ直し、同じ割合の画像ピクセルだけを残す。
            uv.y += (top - y) / size.y * item.textureSize.y;
            uvSize.y = (bottom - top) / size.y * item.textureSize.y;
            position.y = top;
            size.y = bottom - top;
        }

        sprite->SetPosition(position);
        sprite->SetSize(size);
        sprite->SetTextureRect(uv, uvSize);
    }

    // 肖像が配置されていれば、ページ移動量だけを加え、基準サイズを保つ。
    if (engine::Sprite* portrait = pages_[1].portrait.sprite) {
        const Element& element = pages_[1].portrait;

        // 説明中の視線を映像へ集めるため、肖像はEditorで指定した位置・サイズに固定する。
        portrait->SetSize(element.baseSize);
        portrait->SetPosition({element.basePosition.x + offsetX, element.basePosition.y});
    }
}

void ControlsGuideOverlay::DrawImGui() {
    // 実レイアウトを見ながら図鑑の配置とスクロールを調整する編集欄を出す。
#ifdef USE_IMGUI
    ImGui::TextWrapped("Tabの能力図鑑。表示確認だけでプレイヤーの能力は変更しません。");
    const char* keys[] = {"", "normal", "pink", "fire", "thunder", "wind", "bomb", "crown"};
    const char* names[] = {"現在のプレイヤー", "通常", "ピンク", "炎", "雷", "風", "ボム", "覇王"};
    std::int32_t selected = 0;

    // 表示能力の上書き値を選択欄に戻し、変更時だけプレビューの図鑑を読み直す。
    for (std::int32_t i = 0; i < 8; ++i)
        if (bookPreviewKey_ == keys[i])
            selected = i;
    if (ImGui::Combo("表示能力", &selected, names, 8)) {
        bookPreviewKey_ = keys[selected];
        RefreshBook();
    }

    // 能力ページを開く操作では、開いていた場合も即座に1ページ目の配置へそろえる。
    if (ImGui::Button("ガイドを開く")) {
        currentPage_ = 1;
        isSliding_ = false;
        SetActive(true);
        SetAllVisible(true);
        SetPageOffset(1, 0.0f);
    }

    ImGui::SameLine();

    // 閉じる操作でエディターの表示能力指定も解除する。
    if (ImGui::Button("閉じる")) {
        bookPreviewKey_.clear();
        SetActive(false);
    }

    ImGui::TextUnformatted("肖像は静止表示です。位置・サイズは下の実モデル肖像で調整できます。");
    const auto editElement = [](const char* label, Element& element) {
        // 部品が存在し、個別の編集欄を開いた場合だけ位置とサイズを操作できる。
        if (!element.sprite || !ImGui::TreeNode(label))
            return;
        ImGui::DragFloat2("位置", &element.basePosition.x, 1.0f);
        ImGui::DragFloat2("サイズ", &element.baseSize.x, 1.0f, 1.0f, 4000.0f);
        element.sprite->SetSize(element.baseSize);
        ImGui::TreePop();
    };

    editElement("巻物パネル", pages_[1].panel);
    editElement("実モデル肖像", pages_[1].portrait);
    DrawMovePreviewEditor();

    // 基本操作の調整を選ぶと、アニメーションを挟まず0ページ目を見せる。
    if (ImGui::Button("基本操作を表示")) {
        currentPage_ = 0;
        isSliding_ = false;
        SetActive(true);
        SetAllVisible(true);
        SetPageOffset(0, 0.0f);
    }

    // 基本操作の欄を展開したときは、固定部品・詳細・配置済みのキー画像を編集できる。
    if (ImGui::TreeNode("基本操作ページの配置")) {
        editElement("基本操作パネル", pages_[0].panel);
        editElement("通常モデル肖像", pages_[0].portrait);

        for (Element& detail : pages_[0].details)
            editElement(detail.sprite->GetName().c_str(), detail);
        for (Element& glyph : pages_[0].actionGlyphs) {
            if (glyph.sprite)
                editElement(glyph.sprite->GetName().c_str(), glyph);
        }

        ImGui::TreePop();
    }

    // 能力ページの欄を展開したときは、図鑑に登録した部品を個別に編集できる。
    if (ImGui::TreeNode("能力ページの文字配置")) {
        for (BookElement& item : bookElements_) {
            editElement(item.visual.sprite->GetName().c_str(), item.visual);
        }

        ImGui::TreePop();
    }

    // 保存ボタンが押されたときだけ、元の配置JSONを読み直して現在の編集値を書き込む。
    if (ImGui::Button("レイアウトJSONへ保存")) {
        try {
            // 既存JSONを読み込んで配置だけ更新し、Sprite Editor側の他の属性を消さない。
            json layout;
            {
                std::ifstream input(layoutPath_);
                input >> layout;
            }

            const auto saveElement = [this, &layout](const Element& element) {
                // 実体のある部品だけを対象に、実画面の座標と寸法を設計解像度へ戻す。
                if (!element.sprite)
                    return;
                for (auto& data : layout["sprites"]) {
                    // 名前が一致する配置だけを更新し、別のSpriteには変更を加えない。
                    if (data.value("name", std::string{}) != element.sprite->GetName())
                        continue;
                    data["position"] = {element.basePosition.x / layoutRatio_.x,
                                        element.basePosition.y / layoutRatio_.y};
                    data["size"] = {element.baseSize.x / layoutRatio_.x, element.baseSize.y / layoutRatio_.y};
                }
            };

            saveElement(pages_[1].panel);
            saveElement(pages_[1].portrait);
            saveElement(pages_[0].panel);
            saveElement(pages_[0].portrait);

            for (const Element& detail : pages_[0].details)
                saveElement(detail);
            for (const Element& glyph : pages_[0].actionGlyphs)
                saveElement(glyph);
            for (const BookElement& item : bookElements_)
                saveElement(item.visual);
            layout["abilityBook"]["portraitBob"] = bookBob_ / layoutRatio_.y;
            auto& preview = layout["abilityBook"]["movePreview"];
            preview["position"] = {movePreviewPosition_.x, movePreviewPosition_.y};
            preview["size"] = {movePreviewSize_.x, movePreviewSize_.y};
            std::ofstream output(layoutPath_);
            output << layout.dump(2) << '\n';
            output.flush();
            editorStatus_ = output.good() ? "レイアウトを保存しました。" : "書き込みに失敗しました。";
        } catch (...) {
            editorStatus_ = "保存に失敗しました。JSONを確認してください。";
        }
    }

    ImGui::TextWrapped("文章は Resources/json/ui/abilityGuideBook.json で編集できます。変更後は "
                       "generate_ability_book.ps1 で文字画像を再生成してください。");
    ImGui::TextWrapped("%s", editorStatus_.c_str());

    // 表示中で移動していないページなら、変更した配置をその場で反映する。
    if (isActive_ && !isSliding_) {
        SetPageOffset(currentPage_, 0.0f);
    }
#endif
}

bool ControlsGuideOverlay::IsToggleHeld() const {
    // キーボードとゲームパッドの開閉入力をまとめ、開いた直後の再入力を抑制する。
    if (!inputManager_) {
        return false;
    }

    return menuControls::Get(menuControls::Screen::kGuide).bindings.cancel.Held(*inputManager_);
}
