#define NOMINMAX
#include <cstdint>
#include "game/scene/gameplay/GamePlayScene.h"
#include "engine/graphics/3d/light/SceneLighting.h"

#include "CameraManager.h"
#include "Sprite.h"
#include "WinApp.h"
#include "engine/graphics/scene/SceneRenderCommands.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

// 残機減少時の暗転、数字演出、専用カメラを一箇所で管理します。

void GamePlayScene::StartLifeLostPresentation(std::int32_t beforeLives, std::int32_t afterLives) {
    // 演出開始時の表示数を固定し、途中でセーブ値が変わっても数字を跳ねさせません。
    lifeLostState_.Start(beforeLives, afterLives);

    // 開始時に専用スライムがある場合は非表示にそろえる。
    if (lifeLostSlimeObject_) {
        lifeLostSlimeObject_->SetIsVisible(false);
    }

    // 拘束エフェクトを生成済みでも、開始時には表示しない。
    if (lifeLostStunObject_) {
        lifeLostStunObject_->SetIsVisible(false);
    }

    // 専用カメラが用意されている場合にだけ、演出側へ視点を切り替える。
    if (lifeLostCamera_) {
        CameraManager::GetInstance()->SetActiveCamera(lifeLostCamera_.get());
    }
}

// シーン遷移や復帰で演出が中断されても、専用カメラと表示物を残しません。
void GamePlayScene::HideLifeLostPresentationOverlay() {
    // 残機減少演出のカメラとSpriteを片付け、通常シーンへ描画を戻す。
    lifeLostState_.Hide();
    CameraManager::GetInstance()->SetActiveCamera(nullptr);
    // 残機アイコンがある場合は、通常画面へ戻る前に隠す。
    if (lifeLostIcon_.sprite)
        lifeLostIcon_.sprite->SetVisible(false);
    // 掛け算記号が配置されていれば、アイコンと一緒に隠す。
    if (lifeLostXIcon_.sprite)
        lifeLostXIcon_.sprite->SetVisible(false);
    // 黒背景が配置されていれば、通常画面を覆わないよう非表示にする。
    if (lifeLostBackdrop_.sprite)
        lifeLostBackdrop_.sprite->SetVisible(false);
    // 生成済みの専用スライムは、シーンに残さず隠す。
    if (lifeLostSlimeObject_)
        lifeLostSlimeObject_->SetIsVisible(false);
    // 専用の拘束エフェクトの表示があれば解除する。
    if (lifeLostStunObject_)
        lifeLostStunObject_->SetIsVisible(false);
    for (auto& digit : lifeLostDigits_) {
        // 用意済みの数字だけを隠し、未設定の桁は飛ばす。
        if (digit.sprite)
            digit.sprite->SetVisible(false);
    }
}

void GamePlayScene::UpdateLifeLostPresentation(float deltaTime) {
    // 演出が止まっている場合は、黒を保つ終了状態と通常の非表示を切り分ける。
    if (!lifeLostState_.IsActive()) {
        // 演出完了後もシーン切り替えまで黒画面を保持します。
        if (lifeLostState_.HoldsBlack()) {
            const float screenW = static_cast<float>(WinApp::GetClientWidth());
            const float screenH = static_cast<float>(WinApp::GetClientHeight());

            // 背景スプライトを取得できる場合は、画面全体を黒で覆う。
            if (lifeLostBackdrop_.sprite) {
                lifeLostBackdrop_.sprite->SetVisible(true);
                lifeLostBackdrop_.sprite->SetPosition({screenW * 0.5f, screenH * 0.5f});
                lifeLostBackdrop_.sprite->SetSize({screenW, screenH});
                lifeLostBackdrop_.sprite->SetColor({0.0f, 0.0f, 0.0f, 1.0f});
                lifeLostBackdrop_.sprite->Update();
            }

            // 黒を保持する間、残機アイコンがあれば隠す。
            if (lifeLostIcon_.sprite)
                lifeLostIcon_.sprite->SetVisible(false);
            // 黒を保持する間、掛け算記号があれば隠す。
            if (lifeLostXIcon_.sprite)
                lifeLostXIcon_.sprite->SetVisible(false);
            for (auto& digit : lifeLostDigits_) {
                // 黒だけを保持する場合は、生成済みの数字も隠す。
                if (digit.sprite)
                    digit.sprite->SetVisible(false);
            }

            return;
        }

        // 非再生かつ黒を保たない状態では、残機アイコンを表示しない。
        if (lifeLostIcon_.sprite)
            lifeLostIcon_.sprite->SetVisible(false);
        // 同じ非再生状態で、配置済みの掛け算記号も隠す。
        if (lifeLostXIcon_.sprite)
            lifeLostXIcon_.sprite->SetVisible(false);
        // 黒を保持しないため、背景があれば非表示にする。
        if (lifeLostBackdrop_.sprite)
            lifeLostBackdrop_.sprite->SetVisible(false);
        for (auto& digit : lifeLostDigits_) {
            // 非再生かつ黒を保たない状態でも、配置済みの桁は隠す。
            if (digit.sprite)
                digit.sprite->SetVisible(false);
        }

        return;
    }

    // 動作中は3D側と数字側に同じ演出時刻を使い、復活の有無で終端を切り替えます。
    lifeLostState_.Advance(deltaTime);
    UpdateLifeLostPresentationWorld(deltaTime);
    const float t = lifeLostState_.Timer();
    constexpr float kNumberDropTime = 1.28f;
    const float kFadeOutStartTime = lifeLostState_.Revives() ? 2.65f : 2.35f;
    constexpr float kFadeOutDuration = 0.35f;
    const float kEndTime = lifeLostState_.Revives() ? 3.12f : 2.85f;

    // 復活の有無に対応する終端に達したら、演出を完了して表示を片付ける。
    if (t >= kEndTime) {
        // 終端では描画を消して完了通知だけ残し、次のシーン更新に渡します。
        lifeLostState_.Finish();
        // 残機アイコンがあれば、完了とともに非表示にする。
        if (lifeLostIcon_.sprite)
            lifeLostIcon_.sprite->SetVisible(false);
        // 掛け算記号があれば、終了した表示から外す。
        if (lifeLostXIcon_.sprite)
            lifeLostXIcon_.sprite->SetVisible(false);
        // 専用スライムがある場合は、演出の完了時に非表示にする。
        if (lifeLostSlimeObject_)
            lifeLostSlimeObject_->SetIsVisible(false);
        // 専用拘束エフェクトがあれば、終了後に残らないよう隠す。
        if (lifeLostStunObject_)
            lifeLostStunObject_->SetIsVisible(false);
        for (auto& digit : lifeLostDigits_) {
            // 演出の完了時は、配置済みの数字もすべて隠す。
            if (digit.sprite)
                digit.sprite->SetVisible(false);
        }

        return;
    }

    // 残機を減らす瞬間を記録し、それ以降は減少後の値と着地の反動を表示します。
    if (t >= kNumberDropTime) {
        lifeLostState_.MarkNumberDropped();
    }

    // 表示部品は開始0.25秒で出し、復活の有無で選んだ終盤から0.35秒で消す。背景の黒は別に保持する。
    float alpha = std::clamp(t / 0.25f, 0.0f, 1.0f);

    // 消え始める時刻を過ぎたら、0.35秒かけて透明に近づける。
    if (t > kFadeOutStartTime) {
        alpha = std::clamp(1.0f - (t - kFadeOutStartTime) / kFadeOutDuration, 0.0f, 1.0f);
    }

    const std::int32_t displayLives = lifeLostState_.DidDropNumber() ? lifeLostState_.AfterLives() : lifeLostState_.BeforeLives();
    const float screenW = static_cast<float>(WinApp::GetClientWidth());
    const float screenH = static_cast<float>(WinApp::GetClientHeight());
    const float centerX = screenW * 0.5f;
    const float centerY = screenH * 0.5f;
    // 数字を減らす前は周期的にわずかに伸縮し、減らした瞬間は大きくして0.42秒で等倍へ戻す。
    const float settlePulse = lifeLostState_.DidDropNumber()
                                  ? 1.0f + std::max(0.0f, 1.0f - (t - kNumberDropTime) / 0.42f) * 0.22f
                                  : 1.0f + std::sin(t * 7.0f) * 0.035f;

    // 背景、アイコン、数字の順に更新して、数字の落下後だけ色と拡大率を切り替えます。
    if (lifeLostBackdrop_.sprite) {
        lifeLostBackdrop_.sprite->SetVisible(true);
        lifeLostBackdrop_.sprite->SetPosition({centerX, centerY});
        lifeLostBackdrop_.sprite->SetSize({screenW, screenH});
        lifeLostBackdrop_.sprite->SetColor({0.0f, 0.0f, 0.0f, 1.0f});
        lifeLostBackdrop_.sprite->Update();
    }

    // アイコンがあれば、数字の反動にそろえた伸縮と共通透明度を適用する。
    if (lifeLostIcon_.sprite) {
        lifeLostIcon_.sprite->SetVisible(true);
        lifeLostIcon_.sprite->SetPosition(lifeLostIcon_.basePosition);
        lifeLostIcon_.sprite->SetSize({lifeLostIcon_.baseSize.x * settlePulse, lifeLostIcon_.baseSize.y * settlePulse});
        lifeLostIcon_.sprite->SetColor(
            {lifeLostIcon_.baseColor.x, lifeLostIcon_.baseColor.y, lifeLostIcon_.baseColor.z, alpha});
        lifeLostIcon_.sprite->Update();
    }

    // 掛け算記号があれば、基準の大きさに共通透明度を適用する。
    if (lifeLostXIcon_.sprite) {
        lifeLostXIcon_.sprite->SetVisible(true);
        lifeLostXIcon_.sprite->SetPosition(lifeLostXIcon_.basePosition);
        lifeLostXIcon_.sprite->SetSize(lifeLostXIcon_.baseSize);
        lifeLostXIcon_.sprite->SetColor(
            {lifeLostXIcon_.baseColor.x, lifeLostXIcon_.baseColor.y, lifeLostXIcon_.baseColor.z, alpha});
        lifeLostXIcon_.sprite->Update();
    }

    // 1 桁のときは十の位を隠し、残る 1 枚を右寄せ位置へ配置します。
    const std::int32_t tens = displayLives / 10;
    const std::int32_t ones = displayLives % 10;
    const bool showTens = displayLives >= 10;
    const float digitHeight =
        (lifeLostDigits_[1].baseSize.y > 0.0f ? lifeLostDigits_[1].baseSize.y : 86.0f) * settlePulse;
    const float digitWidth = digitHeight * 0.68f;
    const float spacing = digitWidth * 0.82f;
    const float rightX = lifeLostDigits_[1].sprite ? lifeLostDigits_[1].basePosition.x : centerX + 132.0f;
    const float digitY = lifeLostDigits_[1].sprite ? lifeLostDigits_[1].basePosition.y : centerY + 2.0f;
    const float totalWidth = showTens ? spacing + digitWidth : digitWidth;
    // 右端から表示全体の幅を引き、数字画像の中心を置くため半分の幅を戻す。
    const float startX = rightX - totalWidth + digitWidth * 0.5f;
    const std::array<std::int32_t, 2> values = {tens, ones};

    for (std::int32_t i = 0; i < 2; ++i) {
        engine::Sprite* digit = lifeLostDigits_[i].sprite;
        // 桁の実体が無ければ、画像や配置の更新を飛ばす。
        if (!digit)
            continue;

        const bool digitVisible = showTens || i == 1;
        digit->SetVisible(digitVisible);
        // 一桁の表示で十の位を隠す場合は、画像と座標の更新も省く。
        if (!digitVisible)
            continue;

        const std::int32_t sourceIndex = showTens ? i : 1;
        const uint32_t handle = engine::Sprite::LoadTexture("number/big" + std::to_string(values[sourceIndex]) + ".png");
        digit->SetTextureHandle(handle);
        digit->SetPosition({startX + (sourceIndex - (2 - (showTens ? 2 : 1))) * spacing, digitY});
        digit->SetSize({digitWidth, digitHeight});
        digit->SetColor(lifeLostState_.DidDropNumber() ? engine::Vector4{1.0f, 0.65f, 0.22f, alpha}
                                               : engine::Vector4{1.0f, 0.92f, 0.38f, alpha});
        digit->Update();
    }
}

void GamePlayScene::DrawLifeLostPresentation() {
    // 暗転保持中も背景は描き、演出終了まで画面の飛びを防ぐ。
    if (!lifeLostState_.IsActive() && !lifeLostState_.HoldsBlack())
        return;
    // 演出が終わり黒だけを保つ場合は、背景を描く。
    if (!lifeLostState_.IsActive()) {
        DrawGameplayHUDSprite(lifeLostBackdrop_);
    }

    // 黒のみの状態では、記号と残機数を描く前に戻る。
    if (!lifeLostState_.IsActive())
        return;
    DrawGameplayHUDSprite(lifeLostIcon_);
    DrawGameplayHUDSprite(lifeLostXIcon_);

    for (auto& digit : lifeLostDigits_) {
        DrawGameplayHUDSprite(digit);
    }
}

// ライフ減少専用の3D表示物は初期化時に用意し、演出中は表示だけ切り替えます。
void GamePlayScene::InitializeLifeLostPresentationObjects() {
    // 専用カメラとモデルは3D描画基盤の初期化後に生成する。
    if (!GetObject3DCommon()) {
        return;
    }

    // ゲームの追従カメラと独立した固定カメラを用意し、残機表示中の入力では動かさないようにします。
    lifeLostCamera_ = std::make_unique<engine::Camera>();
    lifeLostCamera_->Initialize();
    lifeLostCamera_->SetInputEnabled(false);
    lifeLostCamera_->SetFollowTarget(nullptr);
    lifeLostCamera_->SetFollowMode(engine::Camera::FollowMode::kFixedPoint);
    lifeLostCamera_->SetFovY(0.48f);
    lifeLostCamera_->ConfigFixedPoint({0.0f, 2.25f, -7.2f}, {0.10f, 0.0f, 0.0f});
    lifeLostCamera_->Update();

    // 残機演出のスライムと拘束表示は初期状態で隠し、通常の配置物と別に管理します。
    lifeLostSlimeObject_ = std::make_unique<engine::Object3D>();
    lifeLostSlimeObject_->Initialize(GetObject3DCommon());
    lifeLostSlimeObject_->SetName("LifeLostSlime");
    lifeLostSlimeObject_->SetModel("Characters/slime");
    lifeLostSlimeObject_->SetBlendMode(BlendMode::kNormal);
    lifeLostSlimeObject_->SetEnableEnvMap(false);
    lifeLostSlimeObject_->SetEmissive(1.22f);
    lifeLostSlimeObject_->SetColor({0.38f, 0.92f, 1.0f, 1.0f});
    lifeLostSlimeObject_->SetIsVisible(false);

    lifeLostStunObject_ = std::make_unique<engine::Object3D>();
    lifeLostStunObject_->Initialize(GetObject3DCommon());
    lifeLostStunObject_->SetName("LifeLostStun");
    lifeLostStunObject_->SetModel("Primitives/sphere");
    lifeLostStunObject_->SetMaterialType(18);
    lifeLostStunObject_->SetBlendMode(BlendMode::kNormal);
    lifeLostStunObject_->SetColor({1.0f, 0.94f, 0.28f, 0.78f});
    lifeLostStunObject_->SetIsVisible(false);

    // 拘束表示の波設定は描画側の領域が確保できた場合だけ渡します。
    if (lifeLostStunObject_->GetMeshRenderer() && lifeLostStunObject_->GetMeshRenderer()->GetWaterParamData()) {
        auto* stun = lifeLostStunObject_->GetMeshRenderer()->GetWaterParamData();
        stun->effectType = 0.0f;
        stun->effectScale = 0.86f;
        stun->effectSoftness = 0.72f;
        stun->effectIntensity = 0.55f;
        stun->billboardScale = 1.08f;
        stun->waveSpeed = 1.4f;
        stun->waveFrequency = 2.8f;
    }
}

void GamePlayScene::UpdateLifeLostPresentationWorld(float deltaTime) {
    // 演出時計からカメラとモデル姿勢を求め、ゲーム側の物理更新には依存しない。
    (void)deltaTime;
    const float t = lifeLostState_.Timer();

    // 専用カメラが用意できている場合だけ、経過秒数に応じて高さをわずかに揺らす。
    if (lifeLostCamera_) {
        CameraManager::GetInstance()->SetActiveCamera(lifeLostCamera_.get());
        lifeLostCamera_->ConfigFixedPoint({0.0f, 2.25f + std::sin(t * 0.9f) * 0.04f, -7.2f}, {0.10f, 0.0f, 0.0f});
        lifeLostCamera_->Update();
    }

    // 専用スライムの実体があっても、この更新経路では表示を出さない。
    if (lifeLostSlimeObject_) {
        lifeLostSlimeObject_->SetIsVisible(false);
    }

    // 実体のある拘束エフェクトも、現在の更新経路では表示しない。
    if (lifeLostStunObject_) {
        lifeLostStunObject_->SetIsVisible(false);
    }
}

void GamePlayScene::DrawLifeLostPresentationWorld(const engine::SceneLighting& lighting) {
    // 残機演出が有効な間だけ専用ワールドを描画する。
    if (!lifeLostState_.IsActive()) {
        return;
    }

    // 背景をまずSpriteで描き、専用モデルが見える状態の場合だけ対応する3Dパスへ切り替えます。
    BeginSpriteDrawing();
    DrawGameplayHUDSprite(lifeLostBackdrop_);

    // 専用スライムが存在し表示が有効な場合だけ、3D描画へ切り替える。
    if (lifeLostSlimeObject_ && lifeLostSlimeObject_->GetIsVisible()) {
        GetObject3DCommon()->SetGraphicsCommand();
        GetObject3DCommon()->SetPipelineState(BlendMode::kNormal);
        lighting.Draw(*lifeLostSlimeObject_, &engine::Object3D::Draw);
    }

    // 屈折表示が有効な場合は直前までに描いた背景をコピーし、深度と一緒に渡します。
    if (lifeLostStunObject_ && lifeLostStunObject_->GetIsVisible()) {
        auto* commands = GetRenderCommands();
        // 描画コマンドが未準備なら、背景を参照する屈折部分だけを描かない。
        if (!commands) {
            return;
        }
        commands->CopySceneColor();
        lifeLostStunObject_->DrawStunBind(commands->GetDepthSrvHandle(), commands->GetSceneColorSrvHandle());
    }
}
