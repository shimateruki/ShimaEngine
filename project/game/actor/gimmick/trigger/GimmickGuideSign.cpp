#include "GimmickGuideSign.h"

#include "Camera.h"
#include "DirectXCommon.h"
#include "ObjectPropertyDefinition.h"
#include "Player.h"
#include "SceneManager.h"
#include "WinApp.h"
#include "engine/graphics/2d/SpriteLayoutAnimation.h"
#include "engine/graphics/2d/SpriteLayoutVideo.h"
#include "engine/graphics/2d/SpriteCommon.h"
#include "game/system/scene/SceneGameplayAccess.h"
#include "game/ui/SpriteLayoutFactory.h"

#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>

/// <summary>看板用スプライト群と設計キャンバスを保持し、動画の再生と開閉表示を管理する。</summary>
struct GimmickGuideSign::Layout {
    /// <summary>看板に表示するスプライト一枚を所有し、設計ピクセルでの位置・サイズとコマ再生設定を保持する。</summary>
    struct Item {
        std::unique_ptr<engine::Sprite> sprite;
        spriteLayoutAnimation::Settings animation;
        engine::Vector2 position; // JSONの設計ピクセルでの基準位置。
        engine::Vector2 size; // JSONの設計ピクセルでの基準サイズ。
    };
    std::vector<Item> items;
    engine::Vector2 canvas;
    std::filesystem::path videoPath;
    std::unique_ptr<VideoSpritePlayer> video;
    engine::Sprite* videoSprite = nullptr;
    engine::SpriteCommon* common = nullptr;
    uint32_t poster = 0;
    bool videoOpen = false;
    float openAmount = 0.0f; // 看板に収まった状態を0、全開を1とします。

    // 動画を閉じ、最後に表示されたフレームをポスター画像に戻す。
    void CloseVideo() {
        // 動画を開く要求を出していなければ、プレーヤーやスプライトに触れない。
        if (!videoOpen)
            return;
        video->Close();
        spriteLayoutVideo::SetTexture(*videoSprite, poster);
        videoOpen = false;
    }
};

GimmickGuideSign::GimmickGuideSign() = default;
GimmickGuideSign::~GimmickGuideSign() = default;

void GimmickGuideSign::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    // 初期状態では看板の物理衝突を無効にする。接近はUpdate()でワールド距離から判定する。
    BaseGimmick::Initialize(common, modelName);
    SetGimmickType(kTypeId);
    SetName("Gimmick_GuideSign");
    SetColliderType(engine::ColliderType::kNone);
    SetCollisionAttribute(0);
    SetCollisionMask(0);

    // 表示形式に応じて必要な編集項目を出す。看板から開くときは高さの単位がモデル座標に変わる。
    engine::ObjectPropertyDefinition<GimmickGuideSign> properties(*this, kTypeId, "案内看板",
        "近づくとSprite Editorで作ったレイアウトを看板の上に表示します。");
    properties.Expose("レイアウトJSON", "layoutPath", &GimmickGuideSign::layoutPath_, 0, 0, nullptr,
        {.assetDirectory = "Resources/json/sprite"});
    properties.Expose("反応距離", "activationRadius", &GimmickGuideSign::activationRadius_, 0.25f, 30.0f);
    properties.Expose("看板から開く", "opensFromSign", &GimmickGuideSign::opensFromSign_);
    properties.Expose("表示位置の高さ", "displayHeight", &GimmickGuideSign::displayHeight_, 0.0f, 20.0f, nullptr,
        {.help = [](const GimmickGuideSign& sign) -> std::string {
            return sign.opensFromSign_ ? "木の差し込みを合わせる高さ。看板モデルのローカル座標です。"
                                       : "案内の下端までの高さ。看板の原点を基準にしたワールド単位です。";
        }});
    properties.Expose("表示の横ずれ（画面幅比）", "displayOffsetX", &GimmickGuideSign::displayOffsetX_, -0.35f, 0.35f,
        [](const GimmickGuideSign& sign) { return !sign.opensFromSign_; });
    properties.Expose("画面に対する表示幅", "displayWidth", &GimmickGuideSign::displayWidth_, 0.06f, 0.6f);
}

// プレイヤーの接近範囲を調べ、案内を開き、必要に応じて動画の更新と停止を行う。
void GimmickGuideSign::Update(float deltaTime) {
    BaseGimmick::Update(deltaTime);
    auto* manager = SceneManager::GetInstance();
    auto* scene = manager && manager->IsPlaying() ? manager->GetCurrentScene() : nullptr;
    const auto* player = GetScenePlayer(scene);

    // プレイヤーや看板が無効なら、開閉と再生を次の接近まで初期状態に戻します。
    if (!player || !GetIsVisible()) {
        // レイアウトを読み込んでいる場合だけ、動画を閉じて開き具合を初期化する。
        if (layout_) {
            layout_->CloseVideo();
            layout_->openAmount = 0.0f;
        }
        inRange_ = false;
        elapsed_ = 0.0;
        return;
    }
    // プレイヤーと看板のワールド座標の距離を、設定した反応半径と比較する。
    const auto offset = player->GetWorldPosition() - GetWorldPosition();
    const float radius = std::clamp(activationRadius_, 0.25f, 30.0f);
    const bool nearby = offset.x * offset.x + offset.y * offset.y + offset.z * offset.z <= radius * radius;
    const float step = (std::max)(0.0f, deltaTime);
    // 閉じ切る前に引き返したときは、映像とレイアウトの再生位置を引き継ぎます。
    const bool stillOpen = opensFromSign_ && layout_ && layout_->openAmount > 0.0f;
    // 反応範囲内では継続中の再生を進める。閉じ切った後の再進入なら時刻を0へ戻す。
    if (nearby)
        elapsed_ = inRange_ || stillOpen ? elapsed_ + step : 0.0;
    inRange_ = nearby;

    // 接近した看板の素材だけを作り、選択したJSONが変わったフレームで入れ替えます。
    if (inRange_ && layoutPath_ != attemptedPath_) {
        LoadLayout(scene->GetSpriteCommon());
        elapsed_ = 0.0;
    }
    // 有効なレイアウトがなければ、開閉と動画の更新を止める。
    if (!layout_)
        return;

    // 範囲境界で反転しても、札は現在の大きさから開閉を続けます。
    // 看板から開く設定なら、接近中は0.24秒、離れたときは0.18秒を基準に開き具合を変える。
    if (opensFromSign_) {
        const float speed = nearby ? 1.0f / 0.24f : -1.0f / 0.18f;
        layout_->openAmount = std::clamp(layout_->openAmount + step * speed, 0.0f, 1.0f);
    }
    // 範囲外で閉じ切った場合や即時表示の設定の場合には、動画とコマ時刻をリセットする。
    if (!nearby && (!opensFromSign_ || layout_->openAmount <= 0.0f)) {
        layout_->CloseVideo();
        elapsed_ = 0.0;
    // 接近中に開始済みの動画がある場合だけ再生を進める。
    } else if (nearby && layout_->videoOpen) {
        layout_->video->Update(deltaTime);
    }
}

// 案内JSONを範囲内のパスに解決し、全画像の生成と親子関係の復元を完了した時点で公開する。
void GimmickGuideSign::LoadLayout(engine::SpriteCommon* common) {
    // 既存の案内を先に破棄し、今回試したパスを記録して同じ配置値での再試行を抑える。
    attemptedPath_ = layoutPath_;
    layout_.reset();
    // 描画共通機能が未設定か、レイアウトが未選択なら何も生成しない。
    if (!common || layoutPath_.empty())
        return;

    try {
        const std::filesystem::path path(std::u8string(layoutPath_.begin(), layoutPath_.end()));
        const auto root = std::filesystem::canonical("Resources/json/sprite");
        const auto resolved = std::filesystem::weakly_canonical(path);
        const auto local = resolved.lexically_relative(root);
        // Resources/json/sprite配下のJSONだけを許可する。範囲外へ解決されるパスや拡張子違いは終了する。
        if (local.empty() || local.is_absolute() || *local.begin() == L".." || resolved.extension() != L".json")
            return;

        nlohmann::json data;
        std::ifstream file(resolved);
        // ファイルの読み出しやJSONの形が正しくなければ、生成を始めない。
        if (!(file >> data) || !data.is_object() || !data.contains("sprites") || !data["sprites"].is_array())
            return;

        // World UIの幅に合わせるため、Spriteは元の設計ピクセルで生成して親子関係を復元します。
        auto layout = std::make_unique<Layout>();
        layout->canvas = spriteLayoutScaler::Make(data).designResolution;
        layout->common = common;
        std::unordered_map<std::string, engine::Sprite*> byName;
        std::vector<std::pair<engine::Sprite*, std::string>> parents;
        for (const auto& row : data["sprites"]) {
            // 画像の項目がオブジェクトでなければ、レイアウト全体を無効にする。
            if (!row.is_object())
                return;
            auto sprite = spriteLayoutFactory::Create(common, row, {}, "sprite");
            // 同名の画像があると親へ一意に結び付けられないため、レイアウト全体を無効にする。
            if (!byName.emplace(sprite->GetName(), sprite.get()).second)
                return;
            sprite->SetRotation(spriteLayoutScaler::ReadFloat(row, "rotation", 0.0f));
            sprite->SetVisible(true);
            parents.emplace_back(sprite.get(), spriteLayoutScaler::ReadString(row, "parentName", ""));
            // 最初に有効な動画パスを取得できた画像を再生先とし、元のテクスチャを停止時のポスターとして保存する。
            if (!layout->videoSprite) {
                auto path = spriteLayoutVideo::Resolve(spriteLayoutScaler::ReadString(row, "video", ""));
                // 対象の動画ファイルが解決できた場合に、パスと描画先を確定する。
                if (!path.empty()) {
                    layout->videoPath = std::move(path);
                    layout->videoSprite = sprite.get();
                    layout->poster = sprite->GetTextureHandle();
                }
            }
            const auto pos = sprite->GetPosition();
            const auto size = sprite->GetSize();
            layout->items.push_back({std::move(sprite), spriteLayoutAnimation::Read(row), pos, size});
        }
        // 全オブジェクトを生成した後で親名を解決する。出現順に依存せず親子を復元できる。
        for (const auto& [sprite, name] : parents) {
            // 親名が空の画像はルートなので、親を検索しない。
            if (name.empty())
                continue;
            const auto parent = byName.find(name);
            // 指定した親名が見つからなければ、不完全な親子関係を公開しない。
            if (parent == byName.end())
                return;
            sprite->SetParent(parent->second, false);
            // 循環などで設定した親へ接続できなかった場合も、レイアウト全体を破棄する。
            if (sprite->GetParent() != parent->second)
                return;
        }
        layout_ = std::move(layout);
    } catch (const std::exception&) {
        // 無効なJSONでも看板モデルは残し、表示だけを止めます。
    }
}

// 看板のワールド位置をカメラから投影し、画面に入る案内だけを描く。必要なら動画の表示を開く。
void GimmickGuideSign::DrawGuide(engine::Camera* camera) {
    // 読み込み済みの案内がなければ描画するものがない。
    if (!layout_)
        return;
    const bool showing = opensFromSign_ ? layout_->openAmount > 0.0f : inRange_;
    // 閉じている、カメラがない、看板が非表示のいずれかなら、動画を閉じて描画しない。
    if (!showing || !camera || !GetIsVisible()) {
        layout_->CloseVideo();
        return;
    }

    // カメラの背後に回った看板は、透視除算で画面の反対側に表示させません。
    auto anchor = GetWorldPosition();
    anchor.y += displayHeight_;
    // 下辺を看板に固定する表示方式では、高さをローカル座標として看板の変換で投影点を求める。
    if (opensFromSign_)
        anchor = engine::Math::Transform({0.0f, displayHeight_, 0.0f}, GetWorldMatrix());
    // カメラの背後または視点に近すぎる場合は、投影の前に動画を閉じる。
    if (engine::Math::Transform(anchor, camera->GetViewMatrix()).z <= 0.001f) {
        layout_->CloseVideo();
        return;
    }
    const auto ndc = engine::Math::Transform(anchor, camera->GetViewProjectionMatrix());
    // 投影結果が有限でないか、画面の正規化範囲を外れる場合は表示を止める。
    if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y) || std::abs(ndc.x) > 1.0f || std::abs(ndc.y) > 1.0f) {
        layout_->CloseVideo();
        return;
    }

    // キャンバスの下辺中央を看板の上に合わせ、縦横比を維持します。
    const float width = static_cast<float>(WinApp::GetClientWidth());
    const float height = static_cast<float>(WinApp::GetClientHeight());
    float scale = width * std::clamp(displayWidth_, 0.06f, 0.6f) / layout_->canvas.x;
    const float offsetX = opensFromSign_ ? 0.0f : std::clamp(displayOffsetX_, -0.35f, 0.35f);
    const float anchoredX = ((ndc.x + 1.0f) * 0.5f + offsetX) * width;
    const float anchoredY = (1.0f - ndc.y) * height * 0.5f;
    // 看板に下辺を固定する表示では、画面端に収められる幅・高さと開き具合から倍率を決める。
    if (opensFromSign_) {
        // 画面端では札全体を縮め、木の差し込みが看板から横へずれないようにします。
        scale = (std::min)({scale, 2.0f * (std::min)(anchoredX, width - anchoredX) / layout_->canvas.x,
            anchoredY / layout_->canvas.y});
        const float t = layout_->openAmount;
        scale *= t * t * (3.0f - 2.0f * t);
        // 画面内に置ける余地がない場合、このフレームは描画しない。
        if (scale <= 0.0f)
            return;
    }
    const float canvasWidth = layout_->canvas.x * scale;
    const engine::Vector2 origin{std::clamp(anchoredX - canvasWidth * 0.5f, 0.0f, width - canvasWidth),
                                anchoredY - layout_->canvas.y * scale};

    // 実際に画面内へ出た時だけ先頭から再生し、デコード中は静止画を使います。
    if (layout_->videoSprite) {
        // 接近中で動画を開いていなければ、先頭から開く要求を出す。
        if (inRange_ && !layout_->videoOpen) {
            // 動画にアクセスするプレーヤーは最初に必要になったときだけ作る。
            if (!layout_->video)
                layout_->video = std::make_unique<VideoSpritePlayer>();
            layout_->video->RequestOpen(layout_->videoPath);
            layout_->videoOpen = true;
        }
        // 動画を開く要求が発行済みの場合は、描画用の現在フレームを取得する。
        if (layout_->videoOpen) {
            auto* commands = layout_->common->GetDxCommon()->GetCommandList();
            // 表示できるフレームが準備できた場合にだけ、動画スプライトの画像を差し替える。
            if (const auto frame = layout_->video->PrepareTextureForDraw(*commands))
                spriteLayoutVideo::SetTexture(*layout_->videoSprite, *frame);
        }
    }

    // 設計ピクセルからUIの倍率へ全項目を変換してから、親子関係を含めて描画する。
    for (const auto& item : layout_->items) {
        auto& sprite = *item.sprite;
        engine::Vector2 position{item.position.x * scale, item.position.y * scale};
        // 親のない画像だけに画面上の基点を足す。子の位置は親からのオフセットのまま扱う。
        if (!sprite.GetParent()) {
            position.x += origin.x;
            position.y += origin.y;
        }
        sprite.SetPosition(position);
        sprite.SetSize({item.size.x * scale, item.size.y * scale});
        // 動画を表示する画像にはコマアニメーションを適用せず、動画またはポスターを保つ。
        if (&sprite != layout_->videoSprite)
            spriteLayoutAnimation::Apply(sprite, item.animation, elapsed_);
    }
    for (const auto& item : layout_->items) {
        item.sprite->Update();
        item.sprite->Draw();
    }
}

// 看板の配置と案内設定を複製する。再生用の案内は複製先のUpdate()で読み込む。
std::unique_ptr<engine::Object3D> GimmickGuideSign::Clone() const {
    auto clone = std::make_unique<GimmickGuideSign>();
    clone->Initialize(GetObjectCommon(), GetModelName());
    clone->CopyFrom(this);
    clone->layoutPath_ = layoutPath_;
    clone->activationRadius_ = activationRadius_;
    clone->displayHeight_ = displayHeight_;
    clone->displayWidth_ = displayWidth_;
    clone->displayOffsetX_ = displayOffsetX_;
    clone->opensFromSign_ = opensFromSign_;
    return clone;
}
