#include <cstdint>
#include "ControlsGuideOverlay.h"

#include "engine/graphics/2d/Sprite.h"
#include "engine/graphics/2d/SpriteCommon.h"
#include "engine/graphics/2d/VideoSpritePlayer.h"
#include "engine/graphics/core/DirectXCommon.h"
#include "engine/graphics/core/TextureManager.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

#ifdef USE_IMGUI
#include "imgui.h"
#endif

namespace {
using Json = nlohmann::json;

const Json* FindMove(const Json& book, const std::string& key, std::int32_t index) {
    // 攻略本のルートと指定ページがオブジェクトの場合だけ、技一覧を探す。
    if (!book.is_object() || !book.contains(key) || !book[key].is_object())
        return nullptr;
    const auto& entry = book[key];
    // movesがないか配列でない場合と、技番号が範囲外の場合は選択できない。
    if (!entry.contains("moves") || !entry["moves"].is_array() || index < 0 ||
        static_cast<size_t>(index) >= entry["moves"].size())
        return nullptr;
    // 指定番号のデータがオブジェクトなら、元のJSON内の技への参照を返す。
    const auto& move = entry["moves"][index];
    return move.is_object() ? &move : nullptr;
}

// 素材キーに文字列が設定されていればその値を、未設定または型違いなら空文字列を返します。
std::string ReadAsset(const Json& move, const char* key) {
    const auto it = move.find(key);
    return it != move.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

// 映像と静止画で同じ正規化UVを使用する。不正・未設定の値は全体表示へ戻す。
std::array<float, 4> ReadPreviewCrop(const Json* move) {
    constexpr std::array<float, 4> kFull{0.0f, 0.0f, 1.0f, 1.0f};
    // 選択中の技がないか切り抜き指定を省略した場合は、元画像全体を使う。
    if (!move || !move->contains("previewCrop"))
        return kFull;
    const auto& crop = (*move)["previewCrop"];
    // 切り抜き範囲は4成分の配列で受け取る。形式が異なれば全体表示へ戻す。
    if (!crop.is_array() || crop.size() != 4)
        return kFull;
    // 4成分は順に左上のX・Y、幅・高さ。全て元の画像寸法に対する割合なので解像度に依存しない。
    auto values = kFull;
    for (size_t index = 0; index < values.size(); ++index) {
        // 数値でない成分が一つでもあれば、途中の結果を使わない。
        if (!crop[index].is_number())
            return kFull;
        values[index] = crop[index].get<float>();
        // 無限大やNaNは描画の座標として使えないため、全体表示へ戻す。
        if (!std::isfinite(values[index]))
            return kFull;
    }
    // 左上が範囲外、幅か高さが0以下、または右端か下端が丸め誤差の許容範囲を超える場合は全体を使う。
    if (values[0] < 0.0f || values[1] < 0.0f || values[2] <= 0.0f || values[3] <= 0.0f ||
        values[0] + values[2] > 1.00001f || values[1] + values[3] > 1.00001f)
        return kFull;
    return values;
}

// ビルドに同梱するローカル素材だけを開き、外部URLやResources外の参照を除外する。
std::filesystem::path ResolveAsset(const std::string& value, bool video) {
    // 未入力の素材は解決先なしとして返す。
    if (value.empty())
        return {};
    // 入力は素材フォルダー内の相対パスとして扱い、絶対パスと上位への移動を拒む。
    const std::filesystem::path input(std::u8string(value.begin(), value.end()));
    // 絶対パスやドライブ指定など、ルートを持つ入力は扱わない。
    if (input.has_root_path())
        return {};
    for (const auto& component : input)
        // 上位フォルダーを表す要素を含むパスは拒否する。
        if (component == "..")
            return {};
    const auto extension = input.extension();
    // 動画は.mp4、静止画は.pngまたは.ddsの指定だけを受け付ける。
    if (video ? extension != ".mp4" : (extension != ".png" && extension != ".dds"))
        return {};
    std::error_code error;
    const auto root = std::filesystem::canonical(video ? "Resources/video" : "Resources/sprite", error);
    // ゲームに同梱する動画・静止画のルートを解決できなければ、素材を探さない。
    if (error)
        return {};
    const auto file = std::filesystem::canonical(root / input, error);
    // 実パスを解決できないか、通常ファイルでなければ素材として開けない。
    if (error || !std::filesystem::is_regular_file(file, error))
        return {};
    const auto relative = file.lexically_relative(root);
    // 解決後も素材のルート内にある場合だけ受け付ける。ルートを出るリンク先は拒否する。
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
        return {};
    return file;
}

#ifdef USE_IMGUI
// 同時編集の照合に使うため、改行も含めて設定ファイルをバイト単位で読み出します。
std::string ReadJsonBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    // 入力ファイルを開けなければ保存の比較元がないため、例外として呼び出し側へ返す。
    if (!input)
        throw std::runtime_error("read failed");
    return {std::istreambuf_iterator<char>(input), {}};
}

// 編集対象を読み直してから動画・静止画・表示範囲だけ更新し、書きかけのJSONを公開しない。
void SaveMoveAssets(const std::filesystem::path& path, const std::string& key, std::int32_t index,
                    const Json& selected, const std::string& video, const std::string& poster) {
    // 保存直前にファイルを読み、他の技に加わった編集を保持する。
    const auto original = ReadJsonBytes(path);
    auto latest = Json::parse(original);
    const auto* move = FindMove(latest, key, index);
    // 読み込み時と同じ技を指すことを確かめ、並び替えで別の技へ上書きしない。
    if (!move || ReadAsset(*move, "name") != ReadAsset(selected, "name"))
        throw std::runtime_error("move changed");
    // 編集中の技へ、動画・静止画・切り出し範囲だけを書き戻す。
    auto& target = latest[key]["moves"][index];
    target["previewVideo"] = video;
    target["previewPoster"] = poster;
    target["previewCrop"] = ReadPreviewCrop(&selected);
    // 同じディレクトリの一時ファイルに出し、書き込み中にも元のJSONを残す。
    auto temporary = path;
    temporary += L".video-preview-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    try {
        std::ofstream output(temporary, std::ios::binary);
        output << latest.dump(2) << '\n';
        output.close();
        // 出力が成功し、その間に元ファイルが変わっていない時だけ置き換える。
        if (!output || ReadJsonBytes(path) != original ||
            !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("replace failed");
    // 書き込み・比較・置換に失敗した場合は、一時ファイルを消して呼び出し側へ失敗を伝える。
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}
#endif
}

void ControlsGuideOverlay::CloseMovePreview() {
    // 動画プレイヤーがあり、現在の技を示すキーも残っている場合は、選択情報を消す前に動画を閉じる。
    if (videoPlayer_ && !previewMoveKey_.empty())
        videoPlayer_->Close();
    previewMoveKey_.clear();
    previewVideoPath_.clear();
    previewPosterPath_.clear();
    previewPosterHandle_ = 0;
    previewDescriptionHandle_ = 0;
    // 静止画・動画の表示用Spriteがあれば、前の選択の画像を隠す。
    if (movePreviewSprite_)
        movePreviewSprite_->SetVisible(false);
}

// ページと技が確定したときだけ素材を準備し、再生を許可された動画に経過秒を渡す。
void ControlsGuideOverlay::UpdateMovePreview(float deltaTime) {
    // 案内が表示中で、ページ切り替えが終わった場合にだけ技の映像を再生する。
    const auto* move = isActive_ && bookVisible_ && !isSliding_
        ? FindMove(bookData_, bookKey_, previewMoveIndex_) : nullptr;
    // 閉じている、ページ遷移中、または技を特定できない場合は、以前の技を表示し続けない。
    if (!move) {
        CloseMovePreview();
        return;
    }
    const auto video = ReadAsset(*move, "previewVideo");
    const auto poster = ReadAsset(*move, "previewPoster");
    // 両方の素材が未設定なら、前の技の映像を閉じて表示しない。
    if (video.empty() && poster.empty()) {
        CloseMovePreview();
        return;
    }
    const auto key = bookKey_ + "/" + std::to_string(previewMoveIndex_);
    // 技やパスの変更時だけプレイヤーを閉じて開き直し、同じ再生を毎フレーム初期化しない。
    if (key != previewMoveKey_ || video != previewVideoPath_ || poster != previewPosterPath_) {
        CloseMovePreview();
        previewMoveKey_ = key;
        previewVideoPath_ = video;
        previewPosterPath_ = poster;
        // 静止画の実ファイルを確認できた場合は、動画がまだ表示できないときの代替にする。
        if (!ResolveAsset(poster, false).empty())
            previewPosterHandle_ = engine::Sprite::LoadTexture(poster);
        const auto path = ResolveAsset(video, true);
        // 動画か静止画が見つかった技だけ、映像に対応する説明画像も探す。
        if (!path.empty() || previewPosterHandle_) {
            const std::string description = "ui/control_guide/book/" +
                (bookKey_ == "crown" ? std::string("crown_gate") : bookKey_) +
                "/move_" + std::to_string(previewMoveIndex_) + "_description.png";
            // 説明画像が所定の場所にある場合だけ、表示用ハンドルを取得する。
            if (!ResolveAsset(description, false).empty())
                previewDescriptionHandle_ = engine::Sprite::LoadTexture(description);
        }
        // 動画を解決できて再生設定も有効な場合だけ、デコーダーへ動画を渡す。
        if (!path.empty() && previewPlaybackEnabled_) {
            // 動画プレイヤーをまだ持たない場合は、再生要求を渡す前に作る。
            if (!videoPlayer_)
                videoPlayer_ = std::make_unique<VideoSpritePlayer>();
            videoPlayer_->RequestOpen(path);
        }
    }
    // 静止画は先に表示できる状態にし、動画を使う場合だけ再生側の時計を進める。
    if (videoPlayer_ && previewPlaybackEnabled_)
        videoPlayer_->Update(deltaTime);
}

// 利用できる動画フレームか静止画を切り抜いて枠内に描き、旧レイアウトだけ説明画像も補う。
void ControlsGuideOverlay::DrawMovePreview() {
    // Spriteが既にあれば、今回の条件確認に入る前に前フレームの表示を消す。
    if (movePreviewSprite_)
        movePreviewSprite_->SetVisible(false);
    // 攻略本が非表示、ページ遷移中、技が未選択、または描画基盤が未準備なら描かない。
    if (!bookVisible_ || isSliding_ || previewMoveKey_.empty() || !spriteCommon_)
        return;
    // 描画用コマンドを使える場合だけ動画フレームを取り出し、取得前や無効時は静止画を使います。
    auto* directX = spriteCommon_->GetDxCommon();
    auto* commands = directX ? directX->GetCommandList() : nullptr;
    const auto handle = videoPlayer_ && previewPlaybackEnabled_ && commands
        ? videoPlayer_->PrepareTextureForDraw(*commands) : std::nullopt;
    const auto textureHandle = handle.value_or(previewPosterHandle_);
    // 表示可能な動画も代替の静止画もなければ、このフレームの描画を見送る。
    if (!textureHandle)
        return;
    const auto& metadata = engine::TextureManager::GetInstance()->GetMetadata(textureHandle);
    const float width = static_cast<float>(metadata.width);
    const float height = static_cast<float>(metadata.height);
    // 編集時の枠サイズを現在の画面倍率で変換し、テクスチャのピクセル寸法と分けて扱う。
    const float frameWidth = movePreviewSize_.x * layoutRatio_.x;
    const float frameHeight = movePreviewSize_.y * layoutRatio_.y;
    // 枠サイズが有限でないか、画像または枠の寸法が正でなければ、拡大率を計算しない。
    if (!std::isfinite(frameWidth) || !std::isfinite(frameHeight) ||
        (std::min)({width, height, frameWidth, frameHeight}) <= 0.0f)
        return;
    // 正規化UVを元画像のピクセルへ変換し、切り抜きの比率を保ったまま表示枠へ収めます。
    const auto crop = ReadPreviewCrop(FindMove(bookData_, bookKey_, previewMoveIndex_));
    const engine::Vector2 sourceSize{width * crop[2], height * crop[3]};
    // 幅と高さの拡大率から小さい方を選び、切り抜き画像を縦横比のまま収める。
    const float scale = (std::min)(frameWidth / sourceSize.x, frameHeight / sourceSize.y);
    const engine::Vector2 size{sourceSize.x * scale, sourceSize.y * scale};
    // プレビューのSpriteをまだ持たない場合は、左上を基準に描画できるよう初期化する。
    if (!movePreviewSprite_) {
        movePreviewSprite_ = std::make_unique<engine::Sprite>();
        movePreviewSprite_->Initialize(spriteCommon_, textureHandle);
        movePreviewSprite_->SetAnchorPoint({0.0f, 0.0f});
    }
    // ページの遷移オフセットと表示枠の中央寄せを合わせて、現在ページの中に描きます。
    movePreviewSprite_->SetTextureHandle(textureHandle);
    movePreviewSprite_->SetTextureRect({width * crop[0], height * crop[1]}, sourceSize);
    // 左上アンカーへ枠の余白の半分を足し、切り抜き後も中央が揃うようにする。
    movePreviewSprite_->SetPosition({movePreviewPosition_.x * layoutRatio_.x + bookOffsetX_ + (frameWidth - size.x) * 0.5f,
                                    movePreviewPosition_.y * layoutRatio_.y + (frameHeight - size.y) * 0.5f});
    movePreviewSprite_->SetSize(size);
    movePreviewSprite_->SetVisible(true);
    movePreviewSprite_->Update();
    movePreviewSprite_->Draw();
    const bool hasLayoutDescription = std::any_of(bookElements_.begin(), bookElements_.end(), [](const BookElement& item) {
        return item.slot.rfind("selected_move_", 0) == 0 && item.slot.find("_description") != std::string::npos;
    });
    // 詳細欄がJSONにある場合はそちらを使い、旧レイアウトだけ従来の自動配置を残す。
    if (previewDescriptionHandle_ && !hasLayoutDescription) {
        const auto& description = engine::TextureManager::GetInstance()->GetMetadata(previewDescriptionHandle_);
        // 説明画像の幅か高さが0なら、サイズの比率を計算できないため説明だけ表示しない。
        if (!description.width || !description.height)
            return;
        // 説明用のSpriteがまだなければ、画像を設定する前に用意する。
        if (!movePreviewDescription_) {
            movePreviewDescription_ = std::make_unique<engine::Sprite>();
            movePreviewDescription_->Initialize(spriteCommon_, previewDescriptionHandle_);
            movePreviewDescription_->SetAnchorPoint({0.0f, 0.0f});
        }
        const engine::Vector2 sourceSize{static_cast<float>(description.width), static_cast<float>(description.height)};
        // 説明画像が表示幅より大きいときだけ縮小し、元画像より拡大せずに映像の下へ置く。
        const float descriptionScale = (std::min)(frameWidth / sourceSize.x, 1.0f);
        movePreviewDescription_->SetTextureHandle(previewDescriptionHandle_);
        movePreviewDescription_->SetTextureRect({0.0f, 0.0f}, sourceSize);
        movePreviewDescription_->SetSize({sourceSize.x * descriptionScale, sourceSize.y * descriptionScale});
        movePreviewDescription_->SetPosition({movePreviewPosition_.x * layoutRatio_.x + bookOffsetX_,
                                              (movePreviewPosition_.y + movePreviewSize_.y + 22.0f) * layoutRatio_.y});
        movePreviewDescription_->SetVisible(true);
        movePreviewDescription_->Update();
        movePreviewDescription_->Draw();
    }
}

// 技の選択、枠の調整、素材の試し表示と保存を Editor で受け付ける。
void ControlsGuideOverlay::DrawMovePreviewEditor() {
#ifdef USE_IMGUI
    // エディターのツリーが閉じているときは、この設定欄を描画しない。
    if (!ImGui::TreeNode("技の映像プレビュー"))
        return;
    const auto* move = FindMove(bookData_, bookKey_, previewMoveIndex_);
    // 技選択欄を開いているときだけ、現在ページの候補を列挙する。
    if (ImGui::BeginCombo("編集する技", move ? ReadAsset(*move, "name").c_str() : "なし")) {
        for (std::int32_t index = 0; index < bookMoveCount_; ++index) {
            const auto* candidate = FindMove(bookData_, bookKey_, index);
            // 有効な技がクリックされたときに、編集対象の番号を切り替える。
            if (candidate && ImGui::Selectable(ReadAsset(*candidate, "name").c_str(), previewMoveIndex_ == index))
                previewMoveIndex_ = index;
        }
        ImGui::EndCombo();
    }
    // レイアウト側に保存する枠の調整と、技データ側の動画・静止画を別々に編集する。
    ImGui::DragFloat2("映像枠の左上", &movePreviewPosition_.x, 1.0f);
    ImGui::DragFloat2("映像枠のサイズ", &movePreviewSize_.x, 1.0f, 1.0f, 4000.0f);
    // 動画の再生可否が切り替わったら、現在の再生を閉じて次の更新で設定し直す。
    if (ImGui::Checkbox("映像を再生", &previewPlaybackEnabled_))
        CloseMovePreview();
    ImGui::TextWrapped("MP4はResources/videoから、静止画はResources/spriteからの相対パス。映像は640×360、無音ループです。枠は下のレイアウト保存で保存します。");
    move = FindMove(bookData_, bookKey_, previewMoveIndex_);
    // 現在選択中の技が存在する場合だけ、素材と切り抜きの編集欄を表示する。
    if (move) {
        const auto key = bookKey_ + "/" + std::to_string(previewMoveIndex_);
        // 選択した技が変わった時だけパス欄を読み直し、入力途中の文字を毎フレーム上書きしない。
        if (key != previewEditorMove_) {
            previewEditorMove_ = key;
            const auto fill = [](auto& buffer, const std::string& value) {
                buffer.fill(0);
                std::copy_n(value.data(), (std::min)(value.size(), buffer.size() - 1), buffer.data());
            };
            fill(previewVideoInput_, ReadAsset(*move, "previewVideo"));
            fill(previewPosterInput_, ReadAsset(*move, "previewPoster"));
        }
        auto crop = ReadPreviewCrop(move);
        // 切り抜き欄の値が変わったときだけ、最小幅・高さと画像の境界を守るよう補正する。
        if (ImGui::DragFloat4("映像の範囲 (X / Y / 幅 / 高さ)", crop.data(), 0.005f, 0.0f, 1.0f)) {
            crop[0] = std::clamp(crop[0], 0.0f, 0.95f);
            crop[1] = std::clamp(crop[1], 0.0f, 0.95f);
            crop[2] = std::clamp(crop[2], 0.05f, 1.0f - crop[0]);
            crop[3] = std::clamp(crop[3], 0.05f, 1.0f - crop[1]);
            bookData_[bookKey_]["moves"][previewMoveIndex_]["previewCrop"] = crop;
        }
        ImGui::TextWrapped("範囲は0〜1。0,0,1,1で全体表示。映像設定の保存でこの技の範囲も保存します。");
        ImGui::InputText("動画", previewVideoInput_.data(), previewVideoInput_.size());
        ImGui::InputText("静止画", previewPosterInput_.data(), previewPosterInput_.size());
        const bool apply = ImGui::Button("試しに表示");
        ImGui::SameLine();
        const bool save = ImGui::Button("この技の映像設定を保存");
        // 試しに表示する場合もパスを検証し、未対応の素材を再生へ渡さない。
        if (apply || save) {
            const std::string video = previewVideoInput_.data();
            const std::string poster = previewPosterInput_.data();
            // 空欄以外の素材が見つからないか形式が無効なら、候補を採用せずエラーを表示する。
            if ((!video.empty() && ResolveAsset(video, true).empty()) ||
                (!poster.empty() && ResolveAsset(poster, false).empty())) {
                editorStatus_ = "素材が見つからないか、パス・拡張子が対応範囲外です。";
            // 入力された素材が有効か空欄の場合だけ、ディスク保存または試し表示を行う。
            } else {
                try {
                    // 保存操作だけディスクへ書き、プレビューだけの場合はメモリ上の技データを差し替える。
                    if (save) {
                        const std::filesystem::path path(std::u8string(previewBookPath_.begin(), previewBookPath_.end()));
                        SaveMoveAssets(path, bookKey_, previewMoveIndex_, *move, video, poster);
                    }
                    auto& target = bookData_[bookKey_]["moves"][previewMoveIndex_];
                    target["previewVideo"] = video;
                    target["previewPoster"] = poster;

                    // 古い再生を閉じ、新しい設定で必要な素材が開き直されるようにする。
                    CloseMovePreview();
                    editorStatus_ = save ? "この技の映像設定を保存しました。" : "プレビューに反映しました。";
                // 一時保存や置換に失敗した場合は、画面に保存失敗を表示する。
                } catch (...) {
                    editorStatus_ = "映像設定を保存できませんでした。元の技データを確認してください。";
                }
            }
        }
        // デコード失敗が報告された場合は、対応する動画の寸法と形式を案内する。
        if (videoPlayer_ && videoPlayer_->HasError())
            ImGui::TextWrapped("動画を再生できません。640×360のMP4か確認してください。");
    }
    ImGui::TreePop();
#endif
}
