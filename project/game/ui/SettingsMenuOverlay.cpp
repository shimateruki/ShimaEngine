#include <cstdint>
#include "game/ui/MenuControls.h"
#define NOMINMAX
#include "SettingsMenuOverlay.h"
#include "game/system/settings/GameSettingsManager.h"
#include "InputManager.h"
#include "Sprite.h"
#include "SpriteCommon.h"
#include "TextureManager.h"
#include "DirectXCommon.h"
#include "Log.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <utility>

namespace {
constexpr std::int32_t kVisibleRows = 3;
constexpr engine::Vector4 kInk{.07f, .17f, .21f, 1};

// ポインターが半開区間の矩形内にあるか判定し、隣接項目の境界で二重選択を防ぎます。
bool Inside(engine::Vector2 p, float x, float y, float w, float h) {
    return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h;
}

WinApp* Window() {
    // 表示モードとカーソル制御は描画に使用中のウィンドウへ適用する。
    return engine::DirectXCommon::GetInstance()->GetWinApp();
}

// 最初の通常メニューを開く前のOSカーソル状態。最後の通常メニューを閉じたときに使う。
RECT previousCursorClip{};
bool previousCursorVisible = true;
} // namespace

SettingsMenuOverlay::SettingsMenuOverlay() = default;
SettingsMenuOverlay::~SettingsMenuOverlay() {
    // メニューを開いたまま破棄してもカーソル状態を戻す。
    Finalize();
}

// 描画基盤を借用し、設定画面の配置と文字画像を名前から使えるよう読み込む。
void SettingsMenuOverlay::Initialize(engine::SpriteCommon* common, const std::string& layoutPath) {
    common_ = common;
    input_ = engine::InputManager::GetInstance();
    layoutSprites_.clear();
    labelTextures_.clear();

    // 画像・配置とクリック領域の共通データを一度だけ読み取る。
    try {
        std::ifstream file(layoutPath);
        nlohmann::json j;

        // JSONを読めないかSpriteの配列がなければ、描画用の配置を作れないので失敗を記録する。
        if (!(file >> j) || !j.is_object() || !j.contains("sprites") || !j["sprites"].is_array()) {
            LOG_ERROR("Settings layout '{}' could not be loaded", layoutPath);
            return;
        }

        // 状態メッセージの画像は、配置Spriteと別の名前表へ取り込みます。
        if (j.contains("labelTextures") && j["labelTextures"].is_object()) {
            for (auto it = j["labelTextures"].begin(); it != j["labelTextures"].end(); ++it) {
                // 文字画像の参照先として使える文字列だけ、各ラベル名に割り当てる。
                if (it.value().is_string()) {
                    labelTextures_.emplace(it.key(), it.value().get<std::string>());
                }
            }
        }

        // 位置・寸法・色に必要な要素が揃う行だけ読み、切り抜き範囲は指定時にだけ上書きします。
        for (const auto& data : j["sprites"]) {
            // 名前と必須項目を持つオブジェクトだけを、配置の候補として扱う。
            if (!data.is_object() || !data.contains("name") || !data["name"].is_string() ||
                data["name"].get<std::string>().empty() ||
                !data.contains("position") || !data.contains("size") || !data.contains("color")) {
                continue;
            }

            const auto& position = data["position"];
            const auto& size = data["size"];
            const auto& color = data["color"];
            // 位置・寸法が2成分、色が4成分の形式で揃わなければ、この行だけ読み飛ばす。
            if (!position.is_array() || position.size() != 2 || !size.is_array() || size.size() != 2 ||
                !color.is_array() || color.size() != 4) {
                continue;
            }

            LayoutSprite layout;
            layout.texture = data.value("texture", std::string{});
            layout.selectedTexture = data.value("selectedTexture", std::string{});
            layout.position = {position[0].get<float>(), position[1].get<float>()};
            layout.size = {size[0].get<float>(), size[1].get<float>()};
            layout.color = {color[0].get<float>(), color[1].get<float>(),
                            color[2].get<float>(), color[3].get<float>()};

            // 画像の切り抜き範囲を4成分で指定した場合だけ、切り出しを有効にする。
            if (data.contains("textureCrop") && data["textureCrop"].is_array() && data["textureCrop"].size() == 4) {
                const auto& crop = data["textureCrop"];
                layout.cropOrigin = {crop[0].get<float>(), crop[1].get<float>()};
                layout.cropSize = {crop[2].get<float>(), crop[3].get<float>()};
                layout.hasCrop = true;
            }

            layoutSprites_.emplace(data["name"].get<std::string>(), std::move(layout));
        }
    // 数値型の不一致などで解析が途中終了した場合は、半端な配置と画像名を残しません。
    } catch (...) {
        LOG_ERROR("Settings layout '{}' contains invalid data", layoutPath);
        layoutSprites_.clear();
        labelTextures_.clear();
    }
}

void SettingsMenuOverlay::Finalize() {
    // アクティブ状態の後始末を先に行い、カーソルのクリップを残さない。
    if (active_)
        SetActive(false);
    sprites_.clear();
    layoutSprites_.clear();
    labelTextures_.clear();
    common_ = nullptr;
    input_ = nullptr;
}

void SettingsMenuOverlay::ReadValues() {
    // 編集開始時点の音量と感度を退避し、キャンセル時の基準にする。
    auto* s = GameSettingsManager::GetInstance();
    original_.bgm = s->GetBGMVolume();
    original_.se = s->GetSEVolume();
    original_.sensitivity = s->GetCameraSensitivity();
    draft_ = original_;
}

void SettingsMenuOverlay::SetActive(bool active) {
    // 複数の設定画面が重なっても、最初の開閉だけでカーソル状態を切り替える。
    // 表示の状態が既に要求と同じなら、カーソルや編集値を再初期化しない。
    if (active_ == active)
        return;
    // 開く場合は入力を受け取る画面として登録し、現在の設定から編集を始める。
    if (active) {
        if (sActiveCount == 0) {
            // 最初に開く画面だけがOSのクリップ矩形と表示状態を退避し、複数の画面で保存値を上書きしない。
            GetClipCursor(&previousCursorClip);
            CURSORINFO cursor{sizeof(CURSORINFO)};
            previousCursorVisible = !GetCursorInfo(&cursor) || (cursor.flags & CURSOR_SHOWING) != 0;
            ClipCursor(nullptr);
            WinApp::SetCursorVisibility(true);
        }

        // 入力を預かる画面が増えたことを記録し、選択・スクロール・押しっぱなしの状態を開くたびに初期化します。
        ++sActiveCount;
        Window()->SetCursorRecenteringSuppressed(true);
        ReadValues();
        animationTime_ = 0.0f;
        horizontalHeldDirection_ = 0;
        horizontalNextRepeatMs_ = 0;
        horizontalHoldStartMs_ = 0;
        category_ = row_ = scroll_ = 0;
        footer_ = -1;
        modal_ = Modal::kNone;
        status_.clear();
        suppressInput_ = true;
        replayOnly_ = false;
    // 閉じる場合は画面数を減らし、他の設定画面が残っているかを確認する。
    } else {
        --sActiveCount;
        Window()->SetCursorRecenteringSuppressed(sActiveCount > 0);

        // 最後の通常画面を閉じたときだけ実カーソルを戻し、別ウィンドウへ移っていたらクリップは適用しません。
        if (sActiveCount == 0 && !replayOnly_) {
            // ゲームのウィンドウが前面にある場合だけ、元のクリップ矩形を適用する。
            if (GetForegroundWindow() == Window()->GetHwnd())
                ClipCursor(&previousCursorClip);
            WinApp::SetCursorVisibility(previousCursorVisible);
        }

        // 未適用の音量プレビューを終了し、最後に確定した値へ戻します。
        if (!replayOnly_) {
            // 最後に適用した値を音量プレビューへ戻し、閉じる直前の調整をキャンセルする。
            draft_ = original_;
            PreviewAudio();
        }

        modal_ = Modal::kNone;
    }

    // 使い回すSpriteを一度全て隠し、開く場合だけ現在の状態から画面を組み直します。
    active_ = active;

    for (auto& s : sprites_)
        s->SetVisible(false);
    // 開いた場合だけ、非表示にしたSpriteから内容を組み直す。
    if (active_)
        BuildFrame();
}

bool SettingsMenuOverlay::IsDirty() const {
    // 公開している三項目だけを比較し、未適用の変更を判定する。
    return original_.bgm != draft_.bgm || original_.se != draft_.se ||
           original_.sensitivity != draft_.sensitivity;
}

void SettingsMenuOverlay::PreviewAudio() {
    // 音量だけは保存前に試聴できるよう、編集中の値を即時反映する。
    auto* s = GameSettingsManager::GetInstance();
    s->SetBGMVolume(draft_.bgm);
    s->SetSEVolume(draft_.se);
}

void SettingsMenuOverlay::Apply() {
    // 音量と感度だけを保存し、表示モードや反転設定には触れない。
    // 未適用の値がある場合だけ保存を試す。
    if (IsDirty()) Commit();
}

// プレビュー中の値を確定し、保存が成功した設定だけ次回起動へ引き継ぎます。
void SettingsMenuOverlay::Commit() {
    const auto fail = [this]() {
        status_ = "save_error";
    };

    // 保存完了までカメラ感度を確定しない。既存ファイルは一時ファイルから置換する。
    try {
        namespace fs = std::filesystem;
        const fs::path userPath = "Resources/json/user_config.json";
        auto read = [](const fs::path& path) {
            std::ifstream f(path);
            return std::string(std::istreambuf_iterator<char>(f), {});
        };

        // 同じファイルにある別の設定を消さないよう、既存のオブジェクトの公開三項目だけ更新します。
        const std::string oldUser = read(userPath);
        auto j = oldUser.empty() ? nlohmann::json::object() : nlohmann::json::parse(oldUser);
        // 既存値が項目を保持できるオブジェクトでなければ、新しいオブジェクトとして三項目を書き込む。
        if (!j.is_object())
            j = nlohmann::json::object();
        j["bgmVolume"] = draft_.bgm;
        j["seVolume"] = draft_.se;
        j["cameraSensitivity"] = draft_.sensitivity;

        auto replace = [](const fs::path& path, const std::string& text) {
            fs::create_directories(path.parent_path());

            // 既存の設定と一時ファイルを分け、現在のプロセスIDを一時ファイル名に含める。
            const fs::path temporary = path.wstring() + L".options-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
            // 同じ名前の一時ファイルが残っていれば、既存の一時データを上書きしない。
            if (fs::exists(temporary))
                return false;
            {
                std::ofstream f(temporary, std::ios::binary);
                f << text;
                f.flush();

                // 出力やフラッシュに失敗した場合は、一時ファイルを閉じて取り除く。
                if (!f) {
                    f.close();
                    fs::remove(temporary);
                    return false;
                }
            }

            // 一時ファイルを閉じた後に置き換え、書き込み途中で失敗したときは元のファイルを使う。
            if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                return true;
            fs::remove(temporary);
            return false;
        };

        // 一時ファイルを保存先へ移せなければ、適用済みの基準とカメラ感度は変更せず失敗を示す。
        if (!replace(userPath, j.dump(4))) {
            fail();
            return;
        }
    // 既存JSONの解析やファイル処理で例外が起きた場合も、適用済みの値には進まない。
    } catch (...) {
        fail();
        return;
    }

    // ファイルの置き換えが完了してから、確定値を音量・カメラとキャンセル時の基準へ反映します。
    auto* s = GameSettingsManager::GetInstance();
    PreviewAudio();
    s->SetCameraSensitivity(draft_.sensitivity);

    // 保存した設定を新しい基準にし、次回のキャンセルで適用前の値に戻らないようにする。
    original_ = draft_;
    modal_ = Modal::kNone;
    status_ = "saved";
}

void SettingsMenuOverlay::ResetCategory() {
    // この画面にある三項目だけを初期値へ戻す。保存は適用時に行う。
    draft_.bgm = draft_.se = .8f;
    draft_.sensitivity = 1.0f;
    PreviewAudio();
}

std::int32_t SettingsMenuOverlay::RowCount() const {
    // カテゴリを分けず、音量二項目とカメラ感度を並べる。
    return 3;
}

void SettingsMenuOverlay::Adjust(std::int32_t direction) {
    // 表示している百分率を整数で動かし、浮動小数点の誤差で1刻みが飛ばないようにする。
    status_.clear();
    float& value = row_ == 0 ? draft_.bgm : row_ == 1 ? draft_.se : draft_.sensitivity;
    // 音量は0〜100%、感度は20〜100%。保存値はどれも百分率を100で割った値に戻す。
    const std::int32_t minimum = row_ == 2 ? 20 : 0;
    const std::int32_t percent = std::clamp(static_cast<std::int32_t>(std::lround(value * 100.f)) + direction,
                                   minimum, 100);
    value = percent / 100.f;
    // 音量行の値を変えた場合には試聴用として直ちに反映する。
    if (row_ < 2)
        PreviewAudio();
}

// 行を決定したときは、選択中の値を表示上の1刻みだけ増やす。
void SettingsMenuOverlay::ActivateRow() {
    Adjust(1);
}

void SettingsMenuOverlay::ActivateFooter(std::int32_t index) {
    // 戻る際に編集値が残っていれば、破棄確認を先に表示する。
    dragging_ = false;
    modalSelection_ = 1;

    // 左のボタンは画面を戻る操作として扱う。
    if (index == 0) {
        // 未適用の値がある場合だけ、破棄を確認する。
        if (IsDirty())
            modal_ = Modal::kDiscard;
        // 未適用の変更がなければ確認せず閉じる。
        else
            SetActive(false);
    }

    // 中央のボタンでは既定値に戻す前に確認を出す。
    if (index == 1)
        modal_ = Modal::kReset;
    // 右のボタンでは変更がある場合にだけ適用を試す。
    if (index == 2)
        Apply();
}

void SettingsMenuOverlay::HandleModal(bool accept, bool back) {
    // 確定したときだけ初期化または破棄を実行し、キャンセル時は編集値を保つ。
    if (back || (accept && modalSelection_ == 1)) {
        modal_ = Modal::kNone;
        return;
    }

    // 取消も確定もなければ、モーダルを開いたまま待つ。
    if (!accept) return;
    const Modal mode = modal_;
    modal_ = Modal::kNone;
    // 破棄が確定した場合は、通常画面を閉じて未適用のプレビューを戻す。
    if (mode == Modal::kDiscard) SetActive(false);
    // 初期化が確定した場合は、画面にある三項目の編集値を既定値へ戻す。
    if (mode == Modal::kReset) ResetCategory();
}

void SettingsMenuOverlay::SetEditorViewport(std::int32_t left, std::int32_t top, std::int32_t right, std::int32_t bottom) {
    // 編集画面内のマウス座標をゲーム領域基準へ変換するため矩形を保持する。
    sEditorViewport = {left, top, right, bottom};
    sHasEditorViewport = right > left && bottom > top;
}

// 入力の位置を、設定レイアウトが使う1280×720の座標へ変換する。
engine::Vector2 SettingsMenuOverlay::Pointer() const {
    engine::Vector2 p = input_->GetMousePosition();
    const auto area = WinApp::CalculateGameViewRect(WinApp::GetWindowWidth(), WinApp::GetWindowHeight());
    // 黒帯のクリックは画面外のまま扱い、端のボタンへ吸い寄せません。
    WinApp::ClientToGamePosition(p.x, p.y, area, p.x, p.y);
    float w = static_cast<float>(WinApp::GetClientWidth()), h = static_cast<float>(WinApp::GetClientHeight());
#ifdef USE_IMGUI
    // エディターのゲームビューが指定されている場合、OSのスクリーン座標をその矩形の相対座標にする。
    if (sHasEditorViewport) {
        POINT screen{};
        GetCursorPos(&screen);
        p = {static_cast<float>(screen.x - sEditorViewport.left), static_cast<float>(screen.y - sEditorViewport.top)};
        w = static_cast<float>(sEditorViewport.right - sEditorViewport.left);
        h = static_cast<float>(sEditorViewport.bottom - sEditorViewport.top);
    }
#endif
    // 描画先の矩形をそのまま設計座標へ戻す。Game Viewへの再拡縮も同じ比率で扱う。
    return {p.x * 1280.f / std::max(1.f, w), p.y * 720.f / std::max(1.f, h)};
}

// 入力と表示を更新し、このフレームに閉じた場合だけkClosedを返す。deltaTimeは秒。
SettingsMenuOverlay::Result SettingsMenuOverlay::Update(float deltaTime) {
    // 閉じた状態では入力と描画データを変更しない。
    if (!active_)
        return Result::kNone;

    // ポーズ中でもUIへ渡される時間で、既存ポーズと同じ周期のカーソルを動かす。
    animationTime_ = std::fmod(animationTime_ + std::max(0.f, deltaTime), 120.f);

    if (replayOnly_) {
        // 通常更新が再開したら履歴の編集値は破棄し、現在の設定から開き直します。
        replayOnly_ = false;
        active_ = false;
        --sActiveCount;
        SetActive(true);
        return Result::kNone;
    }

    // 開くときに押した入力が残っている間は描画だけ更新し、全て離してから設定操作を受け付ける。
    if (suppressInput_) {
        // キー、マウスボタン、ゲームパッドのボタンがすべて離れたら、次回から入力を受け付ける。
        if (input_->GetPressedKeys().empty() && input_->GetPressedMouseButton() < 0 &&
            input_->GetPressedGamepadButton() == 0)
            suppressInput_ = false;
        BuildFrame();
        return Result::kNone;
    }

    const auto menu = menuControls::Read(menuControls::Screen::kSettings, *input_);
    const bool accept = menu.confirm, back = menu.cancel;
    std::int32_t horizontal = menu.Horizontal();
    const std::int32_t vertical = menu.Vertical();
    engine::Vector2 p = Pointer();
    const bool click = input_->IsMouseButtonTriggered(0);

    // 確認画面が入力を受け取る間は、背後の設定値や行選択へ伝えない。
    if (modal_ != Modal::kNone) {
        horizontalHeldDirection_ = 0;
        // モーダルで左右が押されたら、現在の二択を反転する。
        if (horizontal)
            modalSelection_ = 1 - modalSelection_;
        bool choose = accept;

        // クリックが発生した場合だけ、ポインターで選んだ側を確定入力に変換する。
        if (click) {
            // 左の確認ボタン内なら、処理を実行する側を選ぶ。
            if (InsideLayout(p, "settings_modal_button_0")) {
                modalSelection_ = 0;
                choose = true;
            }

            // 右の確認ボタン内なら、処理を取り消す側を選ぶ。
            if (InsideLayout(p, "settings_modal_button_1")) {
                modalSelection_ = 1;
                choose = true;
            }
        }

        HandleModal(choose, back);
    // 確認画面が出ていないときだけ、行とフッターの移動・調整を受け付ける。
    } else {
        // 項目の端から上下へ進むとフッターへ移り、フッターから戻ると移動方向に応じた端の項目を選ぶ。
        // 上下入力が発生した場合は、現在選んでいる領域を基準に移動先を決める。
        if (vertical) {
            // フッター選択中の上下入力で設定行に戻り、移動方向に応じた端を選ぶ。
            if (footer_ >= 0) {
                footer_ = -1;
                row_ = vertical < 0 ? RowCount() - 1 : 0;
            // 次の行が先頭より前か末尾より後ならフッターの最初へ移る。
            } else if (row_ + vertical >= RowCount() || row_ + vertical < 0)
                footer_ = 0;
            // 行の範囲内に収まる場合は、入力方向に一行動かす。
            else
                row_ += vertical;
        }

        // 項目の値だけは押し続けても動かせるようにする。短押しは1、長押し開始後は一定間隔で1ずつ進める。
        if (footer_ < 0) {
            const std::int32_t held = engine::ReadMenuHorizontalHeld(
                *input_, menuControls::Get(menuControls::Screen::kSettings).bindings);
            // 長押しはOSの経過ミリ秒で測る。初回は320ms待ち、1.2秒を超えたら繰り返し間隔を65msから35msへ縮める。
            const uint64_t now = GetTickCount64();

            // 押されている方向がなければ、長押しの状態をリセットする。
            if (held == 0) {
                horizontalHeldDirection_ = 0;
            // 新たに押すか方向を変えたら、その場で一刻み進めて次の繰り返しを待つ。
            } else if (held != horizontalHeldDirection_) {
                horizontalHeldDirection_ = held;
                horizontalHoldStartMs_ = now;
                horizontalNextRepeatMs_ = now + 320;
                horizontal = held;
            // 長押しの次回時刻に達した場合は一刻み進め、継続時間に応じて次の間隔を短くする。
            } else if (now >= horizontalNextRepeatMs_) {
                horizontal = held;
                horizontalNextRepeatMs_ = now + (now - horizontalHoldStartMs_ >= 1200 ? 35 : 65);
            } else {
                // スティック側の既存リピート通知と二重に増加しないよう、この画面で一元管理する。
                horizontal = 0;
            }
        // フッターでは同じ方向を押し続けた状態を引き継がない。
        } else {
            horizontalHeldDirection_ = 0;
        }

        // 左右入力がある場合にだけ、現在選択中の領域へ操作を適用する。
        if (horizontal) {
            // フッター側では三つのボタンの間を循環する。
            if (footer_ >= 0)
                footer_ = (footer_ + horizontal + 3) % 3;
            // 設定行を選択中なら、値を指定方向に変更する。
            else
                Adjust(horizontal);
        }

        // 戻る入力を受けた場合は、そのフレームの決定入力より先に戻る処理へ進む。
        if (back)
            ActivateFooter(0);
        // 戻る入力がなく決定が押されたら、選んでいる行かボタンを実行する。
        else if (accept) {
            // フッターを選択中なら、そのボタンに割り当てた操作を実行する。
            if (footer_ >= 0)
                ActivateFooter(footer_);
            // 行を選択中なら、選択行の値を一刻み増やす。
            else
                ActivateRow();
        }

        // マウスではクリック位置の項目を選び、スライダーをつかんだ場合だけ押下中の調整へ進みます。
        if (click && modal_ == Modal::kNone) {
            for (std::int32_t f = 0; f < 3; ++f)
                // クリック位置がフッターのボタン内に入る場合は、対応する操作をその場で実行する。
                if (InsideLayout(p, "settings_footer_button_" + std::to_string(f))) {
                    footer_ = f;
                    ActivateFooter(f);
                }

            for (std::int32_t n = 0; n < kVisibleRows && n + scroll_ < RowCount(); ++n)
                // 設定行内を押した場合はその行を選び、バーの横範囲に触れているか調べる。
                if (InsideLayout(p, "settings_row_" + std::to_string(n))) {
                    row_ = scroll_ + n;
                    footer_ = -1;

                    // つまみはバー端から半幅だけはみ出すため、その部分もつかめるようにする。
                    const LayoutSprite* track = FindLayoutSprite("settings_slider_track_" + std::to_string(n));
                    dragging_ = track && p.x >= track->position.x - 6.0f &&
                                p.x <= track->position.x + track->size.x + 6.0f;
                }
        }

        // 押下終了でドラッグを解除し、継続中はバーの端を超えても値を範囲内に収めます。
        if (!input_->IsMouseButtonPressed(0))
            dragging_ = false;
        // ドラッグを継続中で、確認画面がなく行が有効なときだけスライダーの値を更新する。
        if (dragging_ && modal_ == Modal::kNone && (row_ >= 0 && row_ < 3)) {
            const LayoutSprite* track = FindLayoutSprite("settings_slider_track_" + std::to_string(row_));
            // ポインターがバー左端から進んだ比率を求め、感度の行だけ保存範囲0.2〜1.0へ写す。
            const float t = track && track->size.x > 0.0f
                                ? std::clamp((p.x - track->position.x) / track->size.x, 0.0f, 1.0f)
                                : 0.0f;

            // 上二つの行は音量なので、バーの比率を直接使って試聴値も更新する。
            if (row_ < 2) {
                // 先頭行はBGMの音量に割り当てる。
                if (row_ == 0)
                    draft_.bgm = t;
                // 二行目は効果音の音量に割り当てる。
                else
                    draft_.se = t;
                PreviewAudio();
            // 感度の行だけ、バーの比率を0.2～1.0の実値へ変換する。
            } else
                draft_.sensitivity = .2f + t * .8f;
        }

        // ホイール操作は設定行の上に限定し、表示中の選択がスクロール範囲に収まるようにします。
        const LayoutSprite* firstRow = FindLayoutSprite("settings_row_0");
        const LayoutSprite* lastRow = FindLayoutSprite("settings_row_2");
        const bool pointerOverRows = firstRow && lastRow &&
            Inside(p, firstRow->position.x, firstRow->position.y, firstRow->size.x,
                   lastRow->position.y + lastRow->size.y - firstRow->position.y);
        // 設定行の領域にポインターがあり、ホイール入力がある場合だけ行選択を動かす。
        if (pointerOverRows && input_->GetMouseWheelDelta() != 0) {
            row_ = std::clamp(row_ + (input_->GetMouseWheelDelta() > 0 ? -1 : 1), 0, RowCount() - 1);
            footer_ = -1;
        }

        // 現在行が表示窓の先頭から末尾に収まる範囲へ、先頭の行番号を制限する。
        scroll_ = std::clamp(scroll_, std::max(0, row_ - kVisibleRows + 1), row_);
    }

    // 戻る操作などで閉じていない場合だけ、現在の選択と編集値で描画を作り直します。
    if (active_)
        BuildFrame();
    return active_ ? Result::kNone : Result::kClosed;
}

void SettingsMenuOverlay::Picture(const std::string& texture, float x, float y, float w, float h,
                                  const engine::Vector4& color) {
    // Spriteをフレーム間で再利用し、テクスチャ切替時は切り出し範囲を全体へ戻す。
    if (usedSprites_ == sprites_.size()) {
        auto s = std::make_unique<engine::Sprite>();
        s->Initialize(common_, engine::Sprite::LoadTexture(texture));
        sprites_.push_back(std::move(s));
    }

    auto& s = sprites_[usedSprites_++];
    const auto handle = engine::Sprite::LoadTexture(texture);
    const auto& meta = engine::TextureManager::GetInstance()->GetMetadata(handle);
    s->SetTextureHandle(handle);
    s->SetTextureName(texture);
    s->SetTextureRect({0, 0}, {static_cast<float>(meta.width), static_cast<float>(meta.height)});

    // Spriteの投影サイズと一致させ、表示とPointerの逆変換で余白の差を作らない。
    const engine::Vector2 scale{WinApp::GetClientWidth() / 1280.f, WinApp::GetClientHeight() / 720.f};
    s->SetAnchorPoint({0, 0});
    s->SetPosition({x * scale.x, y * scale.y});
    s->SetSize({w * scale.x, h * scale.y});
    s->SetColor(color);
    s->SetEmissive(1);
    s->SetVisible(true);
    s->Update();
}

void SettingsMenuOverlay::Label(const std::string& id, float x, float y, float height) {
    // 事前生成した文字画像の縦横比を保ち、指定した高さに合わせる。
    const auto it = labelTextures_.find(id);
    // 指定したラベルの文字画像が未登録なら、その場所は描かない。
    if (it == labelTextures_.end()) {
        return;
    }

    const std::string& path = it->second;
    const auto& m = engine::TextureManager::GetInstance()->GetMetadata(engine::Sprite::LoadTexture(path));
    Picture(path, x, y, height * static_cast<float>(m.width) / std::max(1.f, static_cast<float>(m.height)), height,
            kInk);
}

void SettingsMenuOverlay::Number(std::int32_t value, float x, float y, float height) {
    // 数値を文字列へ変換し、登録済みの数字画像を一桁ずつ配置する。
    for (char c : std::to_string(value)) {
        Label(std::string("digit_") + c, x, y, height);
        x += height * .65f;
    }
}

// 名前からJSONで読み込んだ配置を取得する。存在しない名前はnullptrを返す。
const SettingsMenuOverlay::LayoutSprite* SettingsMenuOverlay::FindLayoutSprite(const std::string& name) const {
    const auto it = layoutSprites_.find(name);
    return it == layoutSprites_.end() ? nullptr : &it->second;
}

// 指定した配置がある場合にだけ、設計座標で半開区間の矩形判定を行う。
bool SettingsMenuOverlay::InsideLayout(const engine::Vector2& point, const std::string& name) const {
    const LayoutSprite* layout = FindLayoutSprite(name);
    return layout && Inside(point, layout->position.x, layout->position.y, layout->size.x, layout->size.y);
}

void SettingsMenuOverlay::PictureLayout(const std::string& name, bool selected, float offsetX,
                                        float offsetY, float widthScale) {
    const LayoutSprite* layout = FindLayoutSprite(name);
    // 配置が見つからないか画像名が空なら、描画用Spriteを消費しない。
    if (!layout || layout->texture.empty()) {
        return;
    }

    // 選択用の画像がある場合はそちらを使い、位置と幅の差分を配置の基準値へ重ねます。
    const std::string& texture = selected && !layout->selectedTexture.empty()
                                     ? layout->selectedTexture : layout->texture;
    Picture(texture, layout->position.x + offsetX, layout->position.y + offsetY,
            layout->size.x * widthScale, layout->size.y, layout->color);

    // 切り抜き付きの場合は、直前に配置したSpriteにだけ元画像上の範囲を適用します。
    if (layout->hasCrop) {
        sprites_[usedSprites_ - 1]->SetTextureRect(layout->cropOrigin, layout->cropSize);
        sprites_[usedSprites_ - 1]->Update();
    }
}

// 指定した配置の高さに揃えた文字画像を、矩形の中央に置く。
void SettingsMenuOverlay::CenteredLabel(const std::string& id, const std::string& layoutName) {
    const LayoutSprite* layout = FindLayoutSprite(layoutName);
    const auto texture = labelTextures_.find(id);
    // 配置と文字画像の両方が揃わなければ、幅を計算できないので表示しない。
    if (!layout || texture == labelTextures_.end()) {
        return;
    }

    const auto& metadata = engine::TextureManager::GetInstance()->GetMetadata(engine::Sprite::LoadTexture(texture->second));
    const float width = layout->size.y * static_cast<float>(metadata.width) /
                        std::max(1.0f, static_cast<float>(metadata.height));
    const float center = layout->position.x + layout->size.x * 0.5f;
    Label(id, center - width * 0.5f, layout->position.y, layout->size.y);
}

void SettingsMenuOverlay::DrawCursor(std::int32_t row) {
    // 選択行のJSON位置を基準にカーソルを追従させ、弾みだけを実行時に加える。
    const LayoutSprite* first = FindLayoutSprite("settings_row_0");
    const LayoutSprite* current = FindLayoutSprite("settings_row_" + std::to_string(row));
    // 先頭行か選択行の配置が欠ける場合は、相対高さを計算せずカーソルを省く。
    if (!first || !current) {
        return;
    }

    const float rowOffset = current->position.y - first->position.y;
    const float bob = std::sin(animationTime_ * 7.5f) * 2.0f;
    PictureLayout("settings_cursor_slime", false, 0.0f, rowOffset + bob);
    PictureLayout("settings_cursor_arrow", false, std::sin(animationTime_ * 8.0f) * 2.0f, rowOffset);
}

void SettingsMenuOverlay::BuildFrame() {
    // JSONの各Spriteを名前で描き、設定値に応じた幅・文字・選択状態だけ上書きする。
    if (!common_) return;
    usedSprites_ = 0;
    PictureLayout("settings_background");
    PictureLayout("settings_frame");
    PictureLayout("settings_title");

    // 三つの設定を行ごとに描き、実値は数値に、正規化した値はゲージの幅とつまみ位置に使います。
    for (std::int32_t r = 0; r < 3; ++r) {
        const bool selected = row_ == r && footer_ < 0;
        const std::string index = std::to_string(r);
        PictureLayout("settings_row_" + index, selected);
        PictureLayout("settings_row_label_" + index);

        const float value = r == 0 ? draft_.bgm : r == 1 ? draft_.se : draft_.sensitivity;
        // 感度の最小値20%でつまみがバーの左端に来るよう、範囲全体を0〜1へ戻す。音量は保存値をそのまま使う。
        const float t = std::clamp(r == 2 ? (value - 0.2f) / 0.8f : value, 0.0f, 1.0f);
        PictureLayout("settings_slider_track_" + index);
        const LayoutSprite* fill = FindLayoutSprite("settings_slider_fill_" + index);
        // 幅が0になる場合も塗りをデザイン座標の1だけ残す。
        PictureLayout("settings_slider_fill_" + index, false, 0.0f, 0.0f,
                      fill && fill->size.x > 0.0f ? std::max(1.0f / fill->size.x, t) : t);

        const LayoutSprite* track = FindLayoutSprite("settings_slider_track_" + index);
        PictureLayout("settings_slider_thumb_" + index, false,
                      track ? t * track->size.x : 0.0f);

        // 行に数値の表示位置がある場合は、編集値を百分率にして表示する。
        if (const LayoutSprite* number = FindLayoutSprite("settings_number_" + index)) {
            Number(static_cast<std::int32_t>(std::round(value * 100)), number->position.x,
                   number->position.y, number->size.y);
        }
        // 行が選ばれ、確認画面が前面にないときだけ行のカーソルを表示する。
        if (selected && modal_ == Modal::kNone) DrawCursor(r);
    }

    // 保存結果がある時はそれを優先し、なければ変更の有無を案内します。
    if (status_.empty()) {
        CenteredLabel(IsDirty() ? "dirty" : "clean", "settings_status");
    } else {
        CenteredLabel(status_, "settings_status");
    }

    for (std::int32_t f = 0; f < 3; ++f) {
        const std::string index = std::to_string(f);
        PictureLayout("settings_footer_button_" + index, footer_ == f);
        // 確認画面がなく、このフッターボタンが選択中の場合だけ矢印を表示する。
        if (footer_ == f && modal_ == Modal::kNone)
            PictureLayout("settings_footer_arrow_" + index, false, std::sin(animationTime_ * 8.0f) * 2.0f);
        PictureLayout("settings_footer_label_" + index);
    }

    // 確認が必要な操作では下の選択カーソルを隠し、上に重ねたダイアログの二択を表示します。
    if (modal_ != Modal::kNone) {
        PictureLayout("settings_modal_mask");
        PictureLayout("settings_modal_panel");
        CenteredLabel(modal_ == Modal::kDiscard ? "confirm_discard" : "confirm_reset_all",
                      "settings_modal_title");

        for (std::int32_t b = 0; b < 2; ++b) {
            const std::string index = std::to_string(b);
            PictureLayout("settings_modal_button_" + index, b == modalSelection_);
            // 確認の二択のうち選択されている側だけに、位置を示す矢印を出す。
            if (b == modalSelection_)
                PictureLayout("settings_modal_arrow_" + index, false, std::sin(animationTime_ * 8.0f) * 2.0f);
            PictureLayout("settings_modal_label_" + index);
        }
    }

    // 確認ダイアログを開いた時も、実際に使える閉じる・決定を両端に表示する。
    const bool gamepad = input_ && input_->IsGamepadMode();
    PictureLayout("settings_footer_close_frame");
    PictureLayout(menuControls::Get(menuControls::Screen::kSettings).closeHint.Resolve(gamepad), gamepad);
    PictureLayout("settings_footer_confirm_frame");
    PictureLayout(menuControls::Get(menuControls::Screen::kSettings).confirmHint.Resolve(gamepad), gamepad);

    // 前フレームにだけ使ったダイアログなどのSpriteを非表示にして、組み直した画面に残さないようにします。
    for (size_t i = usedSprites_; i < sprites_.size(); ++i) sprites_[i]->SetVisible(false);
}

void SettingsMenuOverlay::Draw() {
    // 非表示中はSprite群を描かず、編集中の画面だけを重ねる。
    if (active_)
        for (auto& s : sprites_)
            s->Draw();
}

void SettingsMenuOverlay::CollectReplaySprites(std::vector<engine::Sprite*>& out) const {
    // リプレイ描画が所有Spriteを参照できるよう、非所有ポインタだけ渡す。
    for (const auto& s : sprites_)
        out.push_back(s.get());
}

void SettingsMenuOverlay::CaptureReplayState(nlohmann::json& state) const {
    // 表示中の選択位置と編集中の値を記録し、再生画面で同じUIを組み立てる。
    state = {{"active", active_}, {"category", category_}, {"row", row_},
             {"scroll", scroll_}, {"footer", footer_},     {"layoutVersion", 3}, {"animationTime", animationTime_}};
    state["values"] = {{"bgm", draft_.bgm}, {"se", draft_.se}, {"sensitivity", draft_.sensitivity}};
}

// 表示位置と編集中の値を再現し、旧レイアウトの行番号も現行項目へ読み替えます。
// リプレイ専用の表示へ切り替え、実設定やカーソルの表示・クリップ位置は変更しません。
void SettingsMenuOverlay::RestoreReplayState(const nlohmann::json& state) {
    // 記録が項目を持つ形式でなければ、表示状態を変更しない。
    if (!state.is_object())
        return;
    const bool active = state.value("active", false);

    // 再生時も設定画面が出ている間は、中央へ戻すカーソル処理を抑止します。
    if (active_ != active) {
        sActiveCount += active ? 1 : -1;
        active_ = active;
        Window()->SetCursorRecenteringSuppressed(sActiveCount > 0);
    }

    replayOnly_ = true;
    modal_ = Modal::kNone;

    // 記録した編集値は表示に使える範囲に制限し、設定ファイルには書き戻しません。
    if (state.contains("values") && state["values"].is_object()) {
        const auto& v = state["values"];
        draft_.bgm = std::clamp(v.value("bgm", .8f), 0.f, 1.f);
        draft_.se = std::clamp(v.value("se", .8f), 0.f, 1.f);
        draft_.sensitivity = std::clamp(v.value("sensitivity", 1.f), .2f, 1.f);

    }

    // 旧カテゴリ形式の履歴は三項目の対応する行に移し、隠れた項目は先頭へ戻す。
    std::int32_t savedRow = state.value("row", 0);

    // レイアウトの版が3未満の場合は、旧カテゴリと行番号を現行の番号へ移す。
    if (state.value("layoutVersion", 1) < 3) {
        std::int32_t category = state.value("category", 0);
        // 最も古い形式のカテゴリは0・1を最初のカテゴリへ寄せ、2以上を一段ずらしてから現行の行に変換する。
        if (state.value("layoutVersion", 1) < 2) category = category >= 2 ? category - 1 : 0;
        if (category == 0) {
            // 旧音量カテゴリのBGM・SEの選択を引き継ぐ。
            savedRow = std::min(savedRow, 1);
        } else if (category == 1) {
            savedRow = 2; // 旧操作カテゴリは感度の行に対応する。
        } else {
            savedRow = 0; // 対応する行がなければ最初の項目を選ぶ。
        }
    }

    animationTime_ = state.value("animationTime", 0.f);
    category_ = scroll_ = 0;
    row_ = std::clamp(savedRow, 0, 2);
    // フッターの-1は行を選択している状態。記録から戻す場合も0へ丸めず保持する。
    footer_ = std::clamp(state.value("footer", -1), -1, 2);

    // 選択と編集値を戻した後、画面が開いている場合だけSpriteを組み直します。
    if (active_)
        BuildFrame();
    // 記録上で画面が閉じている場合は、既に確保した表示物をすべて隠す。
    else
        for (auto& s : sprites_)
            s->SetVisible(false);
}
