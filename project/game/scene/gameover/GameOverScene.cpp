#define NOMINMAX
#include <cstdint>
#include "game/scene/gameover/GameOverScene.h"
#include "Easing.h"
#include "engine/graphics/3d/light/SceneLighting.h"
#include "game/scene/GameSceneDrawing.h"
#include "ScenePreloader.h"
#include "DirectXCommon.h"
#include "InputManager.h"
#include "AudioPlayer.h"
#include "Object3DCommon.h"
#include "SpriteCommon.h"
#include "Object3D.h"
#include "Sprite.h"
#include "ModelManager.h"
#include "TextureManager.h"
#include "CameraManager.h"
#include "CollisionManager.h"
#include "ParticleSystem.h"
#include "LightManager.h"
#include "SceneManager.h"
#include "DebugConsole.h"
#include "game/system/combat/BulletManager.h"
#include "game/system/scene/LevelLoader.h"
#include "game/system/rules/GameRule.h"
#include "engine/editor/graphics/camera/CameraEditor.h"
#include "LightEditor.h"
#include "ParticleManager.h"
#include "GpuParticleManager.h"
#include "VfxSequencer.h"
#include "Fade.h"
#include "PostEffect.h"
#include "game/system/progress/GameDataManager.h"
#include "WinApp.h"
#ifdef USE_IMGUI
#include "imgui.h"
#endif

#include <algorithm>
#include <cmath>
#include <string>

namespace {
constexpr float kTitleLetterInterval = 0.11f;
constexpr float kTitleLetterPopDuration = 0.28f;
constexpr float kMenuRevealDelay = 0.62f;
constexpr const char* kFallingCrownSpritePrefix = "gameover_falling_crown_";
constexpr const char* kDizzyStarSpritePrefix = "gameover_dizzy_star_";
constexpr const char* kGameOverImpactCue = "shared/game_over_impact_cue";

// 落下する王冠と回転する星を透明で作り、シーンへ所有権を渡す。
engine::Sprite* AddDecorationSprite(engine::SpriteCommon* common,
                                    std::vector<std::unique_ptr<engine::Sprite>>& sprites, uint32_t texture,
                                    std::string name, const engine::Vector2& size) {
    auto sprite = std::make_unique<engine::Sprite>();
    sprite->Initialize(common, texture);
    sprite->SetName(name);
    sprite->SetAnchorPoint({0.5f, 0.5f});
    sprite->SetSize(size);
    sprite->SetColor({1.0f, 1.0f, 1.0f, 0.0f});
    sprite->SetVisible(false);

    auto* result = sprite.get();
    sprites.push_back(std::move(sprite));
    return result;
}

// タイトル・リトライへの退出前に残機を保存します。失敗時は通知して遷移を止めます。
bool TryResetLivesForExit() {
    // 残機を初期値に戻して保存できた場合は、メニューの退出処理を続けられる。
    if (GameDataManager::GetInstance()->ResetLives()) {
        return true;
    }

    ::MessageBoxW(nullptr, L"残機を保存できませんでした。保存先を確認して、もう一度選択してください。",
                  L"セーブエラー", MB_OK | MB_ICONERROR);
    return false;
}

// ゲームオーバー導入の補間値を0～1に収めます。
float Clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}


// 区間外のtを丸め、指定範囲を越えない線形補間値を返します。
float LerpFloat(float start, float end, float t) {
    return start + (end - start) * Clamp01(t);
}

engine::Vector3 MakeLookAtEuler(const engine::Vector3& eye, const engine::Vector3& target) {
    // 固定カメラの視線を、このエンジンの X/Y 回転の符号に合わせて求める。
    const engine::Vector3 direction = target - eye;
    const float planarLength = std::sqrt(direction.x * direction.x + direction.z * direction.z);
    return {std::atan2(-direction.y, planarLength), std::atan2(direction.x, direction.z), 0.0f};
}

// シーン切替フェード中なら導入演出の時計を止めるためtrueを返します。
bool IsFadePlayingForSceneIntro() {
    const Fade::Status status = Fade::GetInstance()->GetStatus();
    return status == Fade::Status::kFadeIn || status == Fade::Status::kFadeOut || status == Fade::Status::kIrisIn ||
           status == Fade::Status::kIrisOut;
}
} // namespace

SceneLoadManifest GameOverScene::BuildAsyncLoadManifest() const {
    // シーン Asset があればその配置を優先し、旧固定パスでもロードできるようにする。
    SceneLoadManifest manifest;
    manifest.AddObjectLayout(HasSceneAssetContext() && !GetSceneLoadContext().objectLayoutPath.empty()
                                 ? GetSceneLoadContext().objectLayoutPath
                                 : "Resources/json/3Dobject/gameOverScene.json");
    manifest.AddSpriteLayout(HasSceneAssetContext() && !GetSceneLoadContext().spriteLayoutPath.empty()
                                 ? GetSceneLoadContext().spriteLayoutPath
                                 : "Resources/json/sprite/gameOverScene.json");
    manifest.AddModel("Characters/player");
    manifest.AddTexture("Resources/sprite/common/white.png");
    return manifest;
}

void GameOverScene::Initialize() {
    // このシーン専用の描画・粒子・カメラを構築し、配置 JSON を読み込む。
    dxCommon_ = engine::DirectXCommon::GetInstance();
    inputManager_ = engine::InputManager::GetInstance();

    ModelManager::GetInstance()->LoadModel("Characters/player");
    LOG("GameOverScene Initialized!");


    // 固定カメラの更新先に入力を接続し、配置を読む前に粒子とライトの描画基盤を用意します。
    CameraManager::GetInstance()->Initialize();
    CameraManager::GetInstance()->SetInputManager(inputManager_);


    particleSystem_ = std::make_unique<engine::ParticleSystem>();
    particleSystem_->Initialize(GetParticleCommon(), "Resources/sprite/common/white.png");
    ParticleManager::GetInstance()->Initialize(particleSystem_.get());

    LightEditor::GetInstance()->SetObject3DCommon(GetObject3DCommon());

    // ゲームオーバー専用の配置集合とルールを作り、弾の管理もこの集合の衝突環境へ接続します。
    objectManager_ = std::make_unique<ObjectManager>();
    levelLoader_ = std::make_unique<LevelLoader>();
    gameRule_ = std::make_unique<GameRule>();
    gameRule_->Initialize(this);

    BulletManager::GetInstance()->Initialize(GetObject3DCommon(), engine::CollisionManager::GetInstance());

    PrepareGpuParticles();
    gpuParticleTexHandle_ = engine::TextureManager::GetInstance()->Load("Resources/sprite/common/white.png");

    // 配置とUIを展開した後で、名前を使ってタイトル文字とメニュー行を結び付けます。
    levelLoader_->LoadObjectLayout(this, "Resources/json/3Dobject/gameOverScene.json");
    levelLoader_->LoadSpriteLayout(this, "Resources/json/sprite/gameOverScene.json");
    BindLayoutSprites();

    // 背景の王冠に落下装飾は重ねないため、追加する装飾は頭上の星だけにする。
    InitializeDizzyStars();

    // 照明とカメラをシーンの設定へ合わせ、初回の描画前に視点を確定します。
    engine::LightManager::GetInstance()->LoadState(ResolveSceneLightPath("Resources/json/light/gameOverScene.json"));
    CameraEditor::GetInstance()->Initialize();
    CameraEditor::GetInstance()->LoadFile(ResolveSceneCameraPath("gameOver_camera.json"));
    CameraEditor::GetInstance()->SetMode(CameraEditor::Mode::kGame);
    CameraEditor::GetInstance()->Update(nullptr, false);
    CameraManager::GetInstance()->Update();
}

void GameOverScene::OnActivated() {
    // 再訪時にも前回の選択・演出時刻・ポストエフェクトを残さない。
    BaseScene::OnActivated();
    sceneTime_ = 0.0f;
    titleRevealTimer_ = 0.0f;
    selectedIndex_ = 0;
    retryExitActive_ = false;
    retrySceneChangeRequested_ = false;
    retryExitTimer_ = 0.0f;
    VfxSequencer::ClearOneShots();
    ResetGameOverPostEffects();

    // 描画用の定数を取得できたら、以前の危険表示を消した状態で開始する。
    if (PostEffect::Params* params = PostEffect::GetInstance()->GetParams()) {
        params->dangerVignette = 0.0f;
    }

    // 役者の初期姿勢と視点を決めてから、文字とメニューの0秒時点を組み立てます。
    InitializeGameOverPresentation();
    UpdateGameOverCamera();
    CameraManager::GetInstance()->Update(0.0f);
    UpdateMenuSprites(0.0f);

    // 初回フレームから切り替え後のカメラとUIが使われるよう、描画データをここで準備します。
    for (auto& object : objectManager_->GetObjects())
        object->RefreshRenderCameraData();
    for (auto& sprite : sprites_)
        sprite->Update();
    dxCommon_->SetRenderClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void GameOverScene::Finalize() {
    // シーンに属する生ポインタを所有物の破棄と同時に無効化する。
    ResetGameOverPostEffects();
    VfxSequencer::ClearOneShots();
    engine::CollisionManager::GetInstance()->ClearObjects();
    BulletManager::GetInstance()->Finalize();

    objectManager_.reset();
    gameOverSlimeObject_ = nullptr;
    gameOverSlimeAnimator_.Reset(nullptr);
    backgroundSprite_ = nullptr;
    titleLetters_.fill(nullptr);
    titleLetterBasePositions_.fill(engine::Vector2::kZero);
    titleLetterBaseSizes_.fill(engine::Vector2::kZero);
    menuRows_ = {};
    fallingCrowns_.clear();
    dizzyStars_.clear();
    sprites_.clear();
    particleSystem_.reset();
}

// 導入フェード中は演出時計とメニュー入力を止め、シーンの通常更新だけ継続します。
void GameOverScene::Update(float deltaTime) {
    const bool canAdvanceIntro = !IsFadePlayingForSceneIntro();
    const float introDeltaTime = canAdvanceIntro ? deltaTime : 0.0f;
    sceneTime_ += introDeltaTime;
    titleRevealTimer_ = std::max(0.0f, sceneTime_ - presentationTuning_.titleRevealStartTime);

    // フェードが終わり、タイトルとメニューが表示済みになった場合だけ入力を処理します。
    if (SceneManager::GetInstance()->IsPlaying() && canAdvanceIntro && !retryExitActive_ &&
        !retrySceneChangeRequested_ && IsTitleRevealComplete()) {
        UpdateMenuInput();
    }

    UpdateMenuSprites(introDeltaTime);
    UpdateFallingCrowns(introDeltaTime);

    // 配置物を通常の経過時間で進めた後に、演出用の時計で倒れたモデルの姿勢を重ねます。
    objectManager_->Update(deltaTime);
    UpdateGameOverPresentation(introDeltaTime);

    LightEditor::GetInstance()->Update();
    CameraEditor::GetInstance()->Update(player_, false);
    UpdateGameOverCamera();
    CameraManager::GetInstance()->Update(deltaTime);

    // カメラ更新後の行列で同一フレームのモデルを描画する。
    for (auto& object : objectManager_->GetObjects())
        object->RefreshRenderCameraData();
    UpdateDizzyStars(introDeltaTime);

    // 粒子は通常時間を使い、退場完了までの待ちは導入と同じ演出時計を使います。
    particleSystem_->Update(deltaTime);
    UpdateRetryExit(introDeltaTime);

    for (auto& sprite : sprites_) {
        sprite->Update();
    }

    // 最後に弾と衝突を更新し、このフレームの接触を判定します。
    BulletManager::GetInstance()->Update(deltaTime);
    engine::CollisionManager::GetInstance()->Update();
}

void GameOverScene::Draw() {
    // 通常メッシュ、特殊マテリアル、粒子の順序を保ってゲーム画面へ合成する。
    // 2D背景をモデルの背後に置くため、不透明物を描く前に描画します。
    DrawBackgroundSprite();

    bool isFirstPerson = false;
    engine::Camera* camera = CameraManager::GetInstance()->GetActiveCamera();
#ifndef _DEBUG
    // 追従対象がいて一人称のときだけ、描画側に一人称用の判定を渡す。
    if (camera->GetFollowTarget() && camera->GetFollowMode() == engine::Camera::FollowMode::kFirstPerson) {
        isFirstPerson = true;
    }
#endif

    // この描画で使う点光源とスポットライトを一度借り、後続の描画に渡す。
    const auto lighting = engine::SceneLighting::Capture();
    GetObject3DCommon()->SetGraphicsCommand();

    auto& objects = objectManager_->GetObjects();

    DrawSceneObjectPass(objects, materialRenderPolicy::Pass::kOpaque, lighting,
        player_, isFirstPerson, false, {});

    DrawEnemyTelegraphs(*this, lighting);
    lighting.Draw(*BulletManager::GetInstance(), &BulletManager::Draw);
    LightEditor::GetInstance()->Draw3D();

    // 不透明の深度ができてから半透明物と粒子を重ねます。
    DrawSceneObjectPass(objects, materialRenderPolicy::Pass::kTransparent, lighting,
        player_, isFirstPerson, false, {});

    particleSystem_->Draw();

    DrawLocalFogObjects(objects, player_, isFirstPerson);

    // 特殊材質で画面コピーを作ったかどうかをGPU粒子に渡し、同じ背景を再取得しないようにします。
    const bool grabUpdated =
        DrawSpecialMaterialObjects(objects, BulletManager::GetInstance(), player_, isFirstPerson);
    DrawGpuParticles(camera, gpuParticleTexHandle_, grabUpdated);
}

void GameOverScene::DrawUi() {
    // 動的な王冠・星は専用リストで先に描き、通常 Sprite 側からの二重描画を避ける。
    BeginSpriteDrawing();

    for (const FallingCrown& crown : fallingCrowns_) {
        // シーンの所有配列に残っている王冠だけ、他の装飾より先に描く。
        if (crown.sprite && IsAlive(crown.sprite)) {
            crown.sprite->Draw();
        }
    }

    for (const DizzyStar& star : dizzyStars_) {
        // シーンに残っている星だけ、背景より前でメニューより奥に描く。
        if (star.sprite && IsAlive(star.sprite)) {
            star.sprite->Draw();
        }
    }

    // 共通Spriteの所有配列には背景と動的な装飾も含まれるので、その名前を除いてから描きます。
    for (auto& sprite : sprites_) {
        // 空の要素を読み飛ばし、名前を参照できるスプライトだけを分ける。
        if (!sprite) {
            continue;
        }

        const std::string& spriteName = sprite->GetName();

        // 背景は3Dの前に描画済みなので、UI側で重ねない。
        if (spriteName == "gameover_background") {
            continue;
        }

        // 落下装飾の王冠は専用パスで描画済みのため省く。
        if (spriteName.rfind(kFallingCrownSpritePrefix, 0) == 0) {
            continue;
        }

        // 頭上の星も専用パスで描画済みのため省く。
        if (spriteName.rfind(kDizzyStarSpritePrefix, 0) == 0) {
            continue;
        }

        sprite->Draw();
    }
}

void GameOverScene::DrawImGui() {
    // 演出値だけを調整し、Sprite の配置は共通エディターに委ねる。
#ifdef USE_IMGUI
    // 演出設定ウィンドウを開けた場合だけ、導入の秒数と固定視点の調整を描く。
    if (ImGui::Begin("ゲームオーバー演出調整")) {
        ImGui::TextUnformatted("うつ伏せで潰れて待機 → 起き上がる → 右へ退場");
        ImGui::SliderFloat("タイトル表示開始", &presentationTuning_.titleRevealStartTime, 0.0f, 2.0f, "%.2f 秒");
        ImGui::DragFloat3("固定カメラ位置", &presentationTuning_.settleEyeOffset.x, 0.03f);
        ImGui::DragFloat3("固定注視位置", &presentationTuning_.settleTargetOffset.x, 0.03f);
        ImGui::SliderFloat("固定FOV", &presentationTuning_.settleFov, 0.30f, 1.00f, "%.3f");
        // 既定値への復帰を押した場合は、シーンの演出パラメーターを初期化する。
        if (ImGui::Button("演出調整を既定値に戻す"))
            presentationTuning_ = PresentationTuning{};
        ImGui::TextUnformatted("UI配置はSprite Editorで調整できます。");
    }

    ImGui::End();
#endif
}

void GameOverScene::BindLayoutSprites() {
    // 編集可能な配置 JSON の名前から参照を結び、拡縮前の基準サイズを保存する。
    backgroundSprite_ = FindSprite("gameover_background");

    constexpr std::array<const char*, 8> kTitleNames = {"gameover_letter_g",  "gameover_letter_a", "gameover_letter_m",
                                                       "gameover_letter_e1", "gameover_letter_o", "gameover_letter_v",
                                                       "gameover_letter_e2", "gameover_letter_r"};

    for (size_t i = 0; i < kTitleNames.size(); ++i) {
        titleLetters_[i] = FindSprite(kTitleNames[i]);

        // 配置にその文字が存在する場合だけ、アニメーション前の位置・サイズを保存する。
        if (titleLetters_[i]) {
            titleLetterBasePositions_[i] = titleLetters_[i]->GetPosition();
            titleLetterBaseSizes_[i] = titleLetters_[i]->GetSize();
        }
    }

    constexpr std::array<const char*, 2> kRowPrefixes = {"gameover_retry", "gameover_title"};

    for (size_t i = 0; i < kRowPrefixes.size(); ++i) {
        MenuRow& row = menuRows_[i];
        const std::string prefix = kRowPrefixes[i];
        row.backdrop = FindSprite(prefix + "_row");
        row.label = FindSprite(prefix + "_label");

        // 行の背景があれば、選択時に拡大するための基準サイズを保存する。
        if (row.backdrop) {
            row.backdropBaseSize = row.backdrop->GetSize();
        }

        // ラベルが配置されていれば、背景とは別にそのラベルの基準サイズを保存する。
        if (row.label) {
            row.labelBaseSize = row.label->GetSize();
        }
    }
}

void GameOverScene::RefreshLayoutSpritePointers() {
    // Sprite Editor の再読込で実体が入れ替わっても、参照と基準サイズを取得し直す。
    backgroundSprite_ = IsAlive(backgroundSprite_) ? backgroundSprite_ : FindSprite("gameover_background");

    constexpr std::array<const char*, 8> kTitleNames = {"gameover_letter_g",  "gameover_letter_a", "gameover_letter_m",
                                                       "gameover_letter_e1", "gameover_letter_o", "gameover_letter_v",
                                                       "gameover_letter_e2", "gameover_letter_r"};

    // 生きている参照の基準値は保持し、再生成された文字だけ現在の配置を取り直します。
    for (size_t i = 0; i < kTitleNames.size(); ++i) {
        // 今の文字が削除または置き換えられた場合だけ、同名を探し直す。
        if (!IsAlive(titleLetters_[i])) {
            titleLetters_[i] = FindSprite(kTitleNames[i]);

            // 同名の文字を取得できた場合は、新しい配置の位置とサイズを使う。
            if (titleLetters_[i]) {
                titleLetterBasePositions_[i] = titleLetters_[i]->GetPosition();
                titleLetterBaseSizes_[i] = titleLetters_[i]->GetSize();
            // 同名の文字が残っていなければ、古い配置情報も消去する。
            } else {
                titleLetterBasePositions_[i] = {0.0f, 0.0f};
                titleLetterBaseSizes_[i] = {0.0f, 0.0f};
            }
        }
    }

    constexpr std::array<const char*, 2> kRowPrefixes = {"gameover_retry", "gameover_title"};

    // 行背景とラベルを別々に確認し、片方が欠けた場合も残った要素を表示できるようにします。
    for (size_t i = 0; i < kRowPrefixes.size(); ++i) {
        MenuRow& row = menuRows_[i];
        const std::string prefix = kRowPrefixes[i];

        // 背景の実体が切り替わった場合だけ、背景の参照と寸法を読み直す。
        if (!IsAlive(row.backdrop)) {
            row.backdrop = FindSprite(prefix + "_row");
            row.backdropBaseSize = row.backdrop ? row.backdrop->GetSize() : engine::Vector2::kZero;
        }

        // ラベルの実体が切り替わった場合だけ、ラベル側の参照と寸法を読み直す。
        if (!IsAlive(row.label)) {
            row.label = FindSprite(prefix + "_label");
            row.labelBaseSize = row.label ? row.label->GetSize() : engine::Vector2::kZero;
        }
    }
}

engine::Sprite* GameOverScene::FindSprite(const std::string& name) const {
    // 名前で取得し、未配置の任意演出部品は nullptr として扱う。
    for (const auto& sprite : sprites_) {
        // 有効なスプライトの名前が一致した時点で、所有元は変えず参照を返す。
        if (sprite && sprite->GetName() == name) {
            return sprite.get();
        }
    }

    return nullptr;
}

void GameOverScene::InitializeFallingCrowns() {
    // 周期と位相をずらした装飾 Sprite を作り、画面全体に同時落下しない配置にする。
    fallingCrowns_.clear();

    // スプライトの共通描画資源がなければ王冠を生成できない。
    if (!GetSpriteCommon()) {
        return;
    }

    // 背景を落下する王冠の初期レイアウトと周期差。個々の開始時刻をずらして同時に落ちるのを避ける。
    struct CrownSetup {
        float x;
        float y;
        float width;
        float timeOffset;
        float cycleDuration;
        float driftAmplitude;
        float driftSpeed;
        float rotationSpeed;
        float phase;
        float alpha;
    };

    constexpr std::array<CrownSetup, 10> kSetups = {
        CrownSetup{170.0f, -170.0f, 122.0f, 1.5f, 24.0f, 26.0f, 0.55f, 0.10f, 0.1f, 0.68f},
        CrownSetup{380.0f, -220.0f, 86.0f, 9.2f, 27.0f, 18.0f, 0.62f, -0.08f, 1.4f, 0.48f},
        CrownSetup{650.0f, -185.0f, 104.0f, 14.0f, 29.0f, 22.0f, 0.48f, 0.07f, 2.1f, 0.52f},
        CrownSetup{950.0f, -260.0f, 76.0f, 4.4f, 25.0f, 16.0f, 0.68f, -0.06f, 2.9f, 0.40f},
        CrownSetup{1250.0f, -190.0f, 112.0f, 12.5f, 28.0f, 24.0f, 0.52f, 0.08f, 3.8f, 0.58f},
        CrownSetup{1535.0f, -235.0f, 92.0f, 6.8f, 26.0f, 20.0f, 0.58f, -0.09f, 4.4f, 0.46f},
        CrownSetup{1745.0f, -155.0f, 132.0f, 18.3f, 31.0f, 28.0f, 0.44f, 0.06f, 5.2f, 0.60f},
        CrownSetup{290.0f, -360.0f, 72.0f, 20.5f, 30.0f, 16.0f, 0.70f, 0.11f, 5.9f, 0.30f},
        CrownSetup{1380.0f, -325.0f, 70.0f, 22.0f, 32.0f, 14.0f, 0.66f, -0.10f, 6.6f, 0.34f},
        CrownSetup{1110.0f, -300.0f, 82.0f, 24.0f, 33.0f, 18.0f, 0.50f, 0.07f, 7.4f, 0.36f},
    };

    const uint32_t textureHandle = engine::Sprite::LoadTexture("ui/gameover/gameover_crown_falling.png");

    for (size_t i = 0; i < kSetups.size(); ++i) {
        const CrownSetup& setup = kSetups[i];
        auto* spritePtr = AddDecorationSprite(GetSpriteCommon(), sprites_, textureHandle,
                                              std::string(kFallingCrownSpritePrefix) + std::to_string(i),
                                              {setup.width, setup.width * 0.75f});
        fallingCrowns_.push_back(FallingCrown{spritePtr, engine::Vector2{setup.x, setup.y},
                                              engine::Vector2{setup.width, setup.width * 0.75f}, setup.timeOffset,
                                              setup.cycleDuration, 1420.0f, setup.driftAmplitude, setup.driftSpeed,
                                              setup.rotationSpeed, setup.phase, setup.alpha});
    }

    UpdateFallingCrowns(0.0f);
}

void GameOverScene::UpdateFallingCrowns(float deltaTime) {
    // シーン時計から位置を決め、停止・再開後も周期がずれないようにする。
    (void)deltaTime;
    const float presentationAlpha = Easing::OutCubic01((sceneTime_ - 0.48f) / 0.82f);

    for (FallingCrown& crown : fallingCrowns_) {
        // 編集で王冠が消えた場合は古い参照を残さず、この要素を飛ばす。
        if (!crown.sprite || !IsAlive(crown.sprite)) {
            crown.sprite = nullptr;
            continue;
        }

        // 王冠ごとの秒オフセットを周期の剰余で折り返し、全てが同時に画面へ戻らないようにする。
        float localTime = std::fmod(sceneTime_ + crown.timeOffset, crown.cycleDuration);

        // 剰余が負なら一周期を足し、周期内の正の時刻へ補正する。
        if (localTime < 0.0f) {
            localTime += crown.cycleDuration;
        }

        // 一周の時刻から落下量を求め、画面への入り際と出際では透明度を落とします。
        const float fallRate = localTime / crown.cycleDuration;
        const float y = crown.basePosition.y + crown.fallDistance * fallRate;
        const float x =
            crown.basePosition.x + std::sin(localTime * crown.driftSpeed + crown.phase) * crown.driftAmplitude;
        // 上下のフェード率の小さい方を使い、入口と出口のどちらかに近付くと薄くする。
        const float fadeIn = Clamp01((y + 120.0f) / 220.0f);
        const float fadeOut = Clamp01((1180.0f - y) / 260.0f);
        const float alpha = std::min(fadeIn, fadeOut) * crown.alpha * presentationAlpha;
        const float slowTilt = std::sin(localTime * 0.42f + crown.phase) * 0.16f;

        crown.sprite->SetVisible(alpha > 0.02f);
        crown.sprite->SetPosition({x, y});
        crown.sprite->SetSize(crown.size);
        crown.sprite->SetRotation(slowTilt + localTime * crown.rotationSpeed);
        crown.sprite->SetColor({1.0f, 1.0f, 1.0f, alpha});
    }
}

void GameOverScene::InitializeDizzyStars() {
    // スライムの頭上を回る星を個別の位相とサイズで用意する。
    dizzyStars_.clear();

    // 共通の描画資源がなければ星の画像を作れない。
    if (!GetSpriteCommon()) {
        return;
    }

    const uint32_t textureHandle = engine::Sprite::LoadTexture("ui/gameover/gameover_dizzy_star.png");
    constexpr std::array<float, 3> kBaseAngles = {0.0f, 2.0943951f, 4.1887902f};
    constexpr std::array<float, 3> kSizes = {42.0f, 34.0f, 38.0f};

    for (size_t i = 0; i < kBaseAngles.size(); ++i) {
        auto* spritePtr = AddDecorationSprite(GetSpriteCommon(), sprites_, textureHandle,
                                              std::string(kDizzyStarSpritePrefix) + std::to_string(i),
                                              {kSizes[i], kSizes[i]});
        dizzyStars_.push_back(DizzyStar{spritePtr, kBaseAngles[i], kSizes[i], static_cast<float>(i) * 0.18f});
    }

    UpdateDizzyStars(0.0f);
}

void GameOverScene::UpdateDizzyStars(float deltaTime) {
    // 3D の頭部位置を画面へ投影し、モデルが見えない間は画面中央寄りに配置する。
    (void)deltaTime;

    engine::Vector2 center = {static_cast<float>(WinApp::GetClientWidth()) * 0.5f,
                      static_cast<float>(WinApp::GetClientHeight()) * 0.57f};

    engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();

    // 頭上の目印を投影できる場合だけ、画面内の既定位置をモデルに追従する座標へ切り替えます。
    if (camera && IsAlive(gameOverSlimeObject_)) {
        const engine::Vector3 headWorld = gameOverSlimeAnimator_.GetDizzyAnchorWorld();

        const engine::Matrix4x4 viewProjection = camera->GetViewProjectionMatrix();
        const engine::Matrix4x4 viewport = engine::Math::MakeViewportMatrix(0.0f, 0.0f, static_cast<float>(WinApp::GetClientWidth()),
                                                            static_cast<float>(WinApp::GetClientHeight()), 0.0f, 1.0f);
        // 視点・射影にビューポートを合成してピクセル位置を求め、深度が0〜1のときだけ使う。
        const engine::Matrix4x4 screenMatrix = engine::Math::Multiply(viewProjection, viewport);
        const engine::Vector3 screen = engine::Math::Transform(headWorld, screenMatrix);

        // 射影後の深度が描画範囲内のときだけ、既定の画面位置を頭上の位置で置き換える。
        if (screen.z >= 0.0f && screen.z <= 1.0f) {
            center = {screen.x, screen.y + 10.0f};
        }
    }

    // スライムの時計を使い、頭上を回る角度と起き上がり時に消える透明度を同期します。
    const float animationTime = gameOverSlimeAnimator_.GetTimer();
    const float introAlpha = Easing::OutCubic01((animationTime - 0.16f) / 0.42f) * gameOverSlimeAnimator_.GetDizzyAlpha();
    const float orbitTime = animationTime * 2.45f;
    // 1080ピクセルを基準に、星の画像サイズと楕円軌道の幅・高さを同じ割合で調整する。
    const float uiScale = static_cast<float>(WinApp::GetClientHeight()) / 1080.0f;
    const float radiusX = 46.0f * uiScale;
    const float radiusY = 15.0f * uiScale;

    for (DizzyStar& star : dizzyStars_) {
        // 星の実体が無効なら、古い参照を消して演出を適用しない。
        if (!star.sprite || !IsAlive(star.sprite)) {
            star.sprite = nullptr;
            continue;
        }

        // 星ごとの初期角で周回をずらし、前側を通る星を少し大きく描きます。
        const float angle = orbitTime + star.baseAngle;
        const float depth = 0.5f + 0.5f * std::sin(angle);
        const float size = star.size * (1.0f + depth * 0.15f) * uiScale;
        const float wobble = std::sin(animationTime * 3.0f + star.baseAngle) * 1.5f * uiScale;
        const engine::Vector2 position = {center.x + std::cos(angle) * radiusX, center.y + std::sin(angle) * radiusY + wobble};

        star.sprite->SetVisible(introAlpha > 0.02f);
        star.sprite->SetPosition(position);
        star.sprite->SetSize({size, size});
        star.sprite->SetRotation(-angle * 0.45f);
        star.sprite->SetColor({1.0f, 1.0f, 1.0f, 1.0f * introAlpha});
    }
}

void GameOverScene::UpdateMenuInput() {
    // タイトル表示後に限って呼ばれ、キーとゲームパッドの選択操作を共通化する。
    // 入力状態を参照できない場合は選択も確定も変更しない。
    if (!inputManager_) {
        return;
    }

    const bool up = inputManager_->IsKeyTriggered(DIK_UP) || inputManager_->IsKeyTriggered(DIK_W) ||
                    inputManager_->IsGamepadButtonTriggered(XINPUT_GAMEPAD_DPAD_UP) || inputManager_->IsMenuStickTriggered(XINPUT_GAMEPAD_DPAD_UP);
    const bool down = inputManager_->IsKeyTriggered(DIK_DOWN) || inputManager_->IsKeyTriggered(DIK_S) ||
                      inputManager_->IsGamepadButtonTriggered(XINPUT_GAMEPAD_DPAD_DOWN) || inputManager_->IsMenuStickTriggered(XINPUT_GAMEPAD_DPAD_DOWN);

    // 上に対応する入力が発生したフレームは、前の選択肢へ進む。
    if (up) {
        ChangeSelection(-1);
    }

    // 下に対応する入力が発生したフレームは、次の選択肢へ進む。
    if (down) {
        ChangeSelection(1);
    }

    // Space・Enter・パッドのAのいずれかを押したフレームで確定を試みる。
    if (inputManager_->IsKeyTriggered(DIK_SPACE) || inputManager_->IsKeyTriggered(DIK_RETURN) ||
        inputManager_->IsGamepadButtonTriggered(XINPUT_GAMEPAD_A)) {
        ConfirmSelection();
    }
}

// リトライ時は残機を戻し、スライムの退場動作を待ってからゲームへ遷移します。
void GameOverScene::StartRetryExit() {
    // 残機をリセットしてから退場演出へ進み、二重のシーン切替要求を防ぐ。
    if (retryExitActive_ || retrySceneChangeRequested_) {
        return;
    }

    // 残機の保存が失敗した場合は、退場もシーン遷移も始めない。
    if (!TryResetLivesForExit()) {
        return;
    }

    retryExitActive_ = true;
    retrySceneChangeRequested_ = false;
    retryExitTimer_ = 0.0f;

    // 退場対象のスライムをまだ参照していない場合は、配置から探す。
    if (!gameOverSlimeObject_) {
        FindGameOverSlimeObject();
    }

    // ワールドの右を基準にし、カメラがあれば画面上の右と一致する方向へ変換します。
    engine::Vector3 exitDirection = {1.0f, 0.0f, 0.0f};
    engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();

    // カメラのYawを得られる場合は、画面右に対応するXZ方向へ置き換える。
    if (camera) {
        const engine::Vector3 rotation = camera->GetRotation();
        exitDirection = {std::cos(rotation.y), 0.0f, -std::sin(rotation.y)};
    }

    // 退場させるモデルがない場合は、動作の完了を待たず遷移を要求します。
    // モデルがある場合は、決めた方向へ歩く退場をAnimatorへ依頼する。
    if (gameOverSlimeObject_) {
        gameOverSlimeAnimator_.StartExitRight(gameOverSlimeObject_, exitDirection, 11.5f);
    // モデルが無い場合は、退場演出を待てないので直ちに遷移を要求する。
    } else {
        retrySceneChangeRequested_ = true;
        SceneManager::GetInstance()->ChangeSceneWithoutLoading("GAMEPLAY");
    }

    DebugConsole::GetInstance()->AddLog("[GameOver] Retry selected. Slime exit animation started.");
}

void GameOverScene::UpdateRetryExit(float deltaTime) {
    // 退場アニメーションが終わらなくても上限時間でゲームへ戻す。
    if (!retryExitActive_) {
        return;
    }

    retryExitTimer_ += deltaTime;

    // 遷移をまだ要求しておらず、演出完了または4秒経過のどちらかを満たしたらゲームへ戻る。
    if (!retrySceneChangeRequested_ && (gameOverSlimeAnimator_.IsExitAnimationFinished() || retryExitTimer_ >= 4.0f)) {
        retrySceneChangeRequested_ = true;
        SceneManager::GetInstance()->ChangeSceneWithoutLoading("GAMEPLAY");
    }
}

void GameOverScene::UpdateMenuSprites(float deltaTime) {
    // タイトル文字の時間差登場と選択行の強調を、保存済みの配置基準から計算する。
    (void)deltaTime;
    RefreshLayoutSpritePointers();

    // 編集停止中は登場タイマーが進まないので、配置確認用に完成状態を表示する。
    const bool layoutPreview = !SceneManager::GetInstance()->IsPlaying();
    const float visibleTitleTime = layoutPreview ? 100.0f : titleRevealTimer_;

    backgroundSprite_ = FindSprite("gameover_background");

    // 背景が配置済みなら、導入中も通常の白と不透明度で描く。
    if (backgroundSprite_) {
        backgroundSprite_->SetColor({1.0f, 1.0f, 1.0f, 1.0f});
    }

    // 文字は並び順で開始時刻をずらし、未到達の文字は非表示にします。
    const engine::Vector4 hiddenTitleColor = {0.72f, 0.94f, 1.0f, 0.0f};

    for (size_t i = 0; i < titleLetters_.size(); ++i) {
        engine::Sprite* letter = titleLetters_[i];

        // この位置に文字を配置していなければ、残りの文字だけを更新する。
        if (!letter) {
            continue;
        }

        const float appearTime = static_cast<float>(i) * kTitleLetterInterval;
        const float revealElapsed = visibleTitleTime - appearTime;
        const float wave = std::sin(sceneTime_ * 2.2f + static_cast<float>(i) * 0.42f);
        const float pulse = 0.5f + 0.5f * std::sin(sceneTime_ * 3.0f + static_cast<float>(i) * 0.28f);
        const engine::Vector2 basePos = titleLetterBasePositions_[i];
        const engine::Vector2 baseSize = titleLetterBaseSizes_[i];

        // この文字の登場時刻までは基準位置の縮小姿勢で隠しておく。
        if (revealElapsed <= 0.0f) {
            letter->SetVisible(false);
            letter->SetPosition(basePos);
            letter->SetSize({baseSize.x * 0.65f, baseSize.y * 0.65f});
            letter->SetColor(hiddenTitleColor);
            continue;
        }

        // 表示開始時は上から落ちる拡大を使い、登場が終わると小さな脈動へ切り替えます。
        letter->SetVisible(true);
        const float revealRate = Clamp01(revealElapsed / kTitleLetterPopDuration);
        const float revealEase = Easing::OutCubic01(revealRate);
        const float popEase = Easing::OutBack01(revealRate);
        const float idleScale = 1.0f + pulse * 0.025f;
        const float revealScale = std::clamp(0.55f + popEase * 0.48f, 0.55f, 1.12f);
        const float scale = revealRate < 1.0f ? revealScale : idleScale;
        const float dropOffset = (1.0f - revealEase) * -34.0f;

        letter->SetPosition({basePos.x, basePos.y + dropOffset + wave * 4.0f * revealEase});
        letter->SetSize({baseSize.x * scale, baseSize.y * scale});
        letter->SetColor({0.84f + pulse * 0.16f, 0.95f + pulse * 0.05f, 1.0f, revealEase});
    }

    const float pulse = 0.5f + 0.5f * std::sin(sceneTime_ * 5.4f);
    const engine::Vector4 normalText = {0.18f, 0.20f, 0.34f, 1.0f};
    const engine::Vector4 selectedText = {0.10f, 0.12f, 0.23f, 1.0f};
    const float menuStartTime = static_cast<float>(titleLetters_.size()) * kTitleLetterInterval + kMenuRevealDelay;
    // タイトルの後でメニューを表示し、リトライ中は退場するモデルを見せるために先に消します。
    const float retryMenuFade =
        retryExitActive_ && !layoutPreview ? (1.0f - Easing::OutCubic01(retryExitTimer_ / 0.28f)) : 1.0f;
    const float menuAlpha = Easing::OutCubic01((visibleTitleTime - menuStartTime) / 0.28f) * retryMenuFade;
    const bool menuVisible = menuAlpha > 0.0f;

    // 選択画像と基準サイズの倍率を背景とラベルに分けて適用し、同じ透明度で開閉します。
    for (std::int32_t i = 0; i < static_cast<std::int32_t>(MenuItem::kCount); ++i) {
        MenuRow& row = menuRows_[static_cast<size_t>(i)];
        const bool selected = selectedIndex_ == i;
        const float rowScale = selected ? 1.025f + pulse * 0.006f : 1.0f;
        const float labelScale = selected ? 1.02f : 1.0f;

        // 背景が用意されている行は、選択表示の画像・拡大率・透明度を更新する。
        if (row.backdrop) {
            row.backdrop->SetVisible(menuVisible);
            const uint32_t handle = engine::Sprite::LoadTexture(selected ? "ui/gameover/gameover_button_selected.png"
                                                                 : "ui/gameover/gameover_button_normal.png");
            row.backdrop->SetTextureHandle(handle);
            const auto& metadata = engine::TextureManager::GetInstance()->GetMetadata(handle);
            row.backdrop->SetTextureRect({0.0f, 0.0f},
                                         {static_cast<float>(metadata.width), static_cast<float>(metadata.height)});
            row.backdrop->SetSize({row.backdropBaseSize.x * rowScale, row.backdropBaseSize.y * rowScale});
            engine::Vector4 backdropColor = selected ? engine::Vector4{1.0f, 1.0f, 1.0f, 0.98f} : engine::Vector4{1.0f, 1.0f, 1.0f, 0.90f};
            backdropColor.w *= menuAlpha;
            row.backdrop->SetColor(backdropColor);
        }

        // ラベルが用意されている行は、同じ表示タイマーで文字を更新する。
        if (row.label) {
            row.label->SetVisible(menuVisible);
            row.label->SetSize({row.labelBaseSize.x * labelScale, row.labelBaseSize.y * labelScale});
            engine::Vector4 labelColor = selected ? selectedText : normalText;
            labelColor.w *= menuAlpha;
            row.label->SetColor(labelColor);
        }
    }
}

bool GameOverScene::IsTitleRevealComplete() const {
    // 最後の文字が出た後、メニューのフェード完了まで入力を待たせる。
    const float menuStartTime = static_cast<float>(titleLetters_.size()) * kTitleLetterInterval + kMenuRevealDelay;
    return titleRevealTimer_ >= menuStartTime + 0.28f;
}

// シーン起動時のスライムと固定視点を、この演出用の状態へまとめて初期化する。
void GameOverScene::InitializeGameOverPresentation() {
    FindGameOverSlimeObject();

    // 作者が配置した基準位置を、倒れ姿勢を適用する前に保存する。
    gameOverPresentationTimer_ = 0.0f;
    gameOverImpactCuePlayed_ = false;

    // モデルを見つけた場合にだけ、配置のワールド位置とスケールを控える。
    if (gameOverSlimeObject_) {
        gameOverSlimeBasePosition_ = gameOverSlimeObject_->GetWorldPosition();
        gameOverSlimeBaseScale_ = gameOverSlimeObject_->GetScale();
    }

    gameOverSlimeAnimator_.Reset(gameOverSlimeObject_);
    engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();

    // メインカメラを取得できる場合は、追従と手動入力を切って固定視点へ移行する。
    if (camera) {
        camera->SetFollowTarget(nullptr);
        camera->SetInputEnabled(false);
        camera->SetFollowMode(engine::Camera::FollowMode::kFixedPoint);
    }

    UpdateGameOverPresentation(0.0f);
}

void GameOverScene::FindGameOverSlimeObject() {
    // レイアウト内のプレイヤーモデルを名前またはモデルパスで探す。
    gameOverSlimeObject_ = nullptr;

    // 配置の所有元が無ければ、演出対象を探索できない。
    if (!objectManager_) {
        return;
    }

    for (const auto& object : objectManager_->GetObjects()) {
        // モデル名を調べる前に、所有配列の空き項目を除外する。
        if (!object) {
            continue;
        }

        const std::string& name = object->GetName();
        const std::string modelName = object->GetModelName();
        // 既定の名前かスライム用のモデルパスの最初の一致を演出対象にする。
        if (name == "Characters/slime_1" || modelName == "Characters/slime" ||
            modelName.find("Characters/slime") != std::string::npos) {
            gameOverSlimeObject_ = object.get();
            return;
        }
    }
}

void GameOverScene::UpdateGameOverPresentation(float deltaTime) {
    // 編集によるオブジェクト再生成にも対応し、見失った場合は参照を結び直す。
    if (!IsAlive(gameOverSlimeObject_)) {
        gameOverSlimeObject_ = nullptr;
    }

    // 保持していた参照がない場合は、編集で配置し直された同じ役者を探す。
    if (!gameOverSlimeObject_) {
        FindGameOverSlimeObject();

        // 対象を見つけ直した場合は、現在の役者を使ってAnimatorの状態も作り直す。
        if (gameOverSlimeObject_) {
            gameOverSlimeAnimator_.Reset(gameOverSlimeObject_);
        }
    }

    // 探しても演出対象がなければモデル依存の更新を省く。
    if (!gameOverSlimeObject_) {
        return;
    }

    // 置き直したモデルを基準に演出時間を進め、姿勢を適用してから画面効果を整えます。
    gameOverPresentationTimer_ += std::max(0.0f, deltaTime);
    gameOverSlimeAnimator_.Update(gameOverSlimeObject_, deltaTime);

    // 倒れ姿勢から開始するので、到達時刻の一度だけ消化フラグを立てる。
    if (!gameOverImpactCuePlayed_ && gameOverPresentationTimer_ >= GameOverSlimeAnimator::GetImpactTime()) {
        gameOverImpactCuePlayed_ = true;
    }

    UpdateGameOverPostEffects();
}

void GameOverScene::UpdateGameOverCamera() {
    engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();
    // 視点か基準モデルが無い場合は、シーン専用の位置と注視点を適用できない。
    if (!camera || !gameOverSlimeObject_)
        return;

    // 2D背景の床と3Dキャラクターがずれないよう、退場まで同じ構図を維持する。
    const engine::Vector3 eye = gameOverSlimeBasePosition_ + presentationTuning_.settleEyeOffset;
    const engine::Vector3 target = gameOverSlimeBasePosition_ + presentationTuning_.settleTargetOffset;
    const engine::Vector3 rotation = MakeLookAtEuler(eye, target);
    camera->SetInputEnabled(false);
    camera->SetFollowTarget(nullptr);
    camera->SetFollowMode(engine::Camera::FollowMode::kFixedPoint);
    camera->ConfigFixedPoint(eye, rotation);
    camera->SetEye(eye);
    camera->SetTarget(target);
    camera->SetRotation(rotation);
    camera->SetFovY(presentationTuning_.settleFov);
}

void GameOverScene::UpdateGameOverPostEffects() {
    // 赤フラッシュ、色収差、ブラーで倒れ姿勢やメニューを覆わない。
    // シーン遷移用のフェード値はFade側の所有なので変更しない。
    ResetGameOverPostEffects();
}

void GameOverScene::EmitGameOverImpactCue() {
    // 着地演出を使う場合にだけ、モデル位置から VFX と揺れを開始する。
    // 基準になるモデルが無ければ、座標を決められないため衝撃を生成しない。
    if (!gameOverSlimeObject_) {
        return;
    }

    VfxSequencer::PlayOneShot(kGameOverImpactCue, gameOverSlimeObject_->GetWorldPosition());
    engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();

    // メインカメラがある場合にだけ、VFXと同時に視点を短時間揺らす。
    if (camera) {
        camera->StartShake(0.24f, presentationTuning_.impactShake, 28.0f, {0.85f, 0.55f, 0.35f});
    }
}

void GameOverScene::ResetGameOverPostEffects() {
    // 前シーンの被弾・危険表示を持ち込まず、ゲームオーバー画面の色を一定にする。
    PostEffect::Params* params = PostEffect::GetInstance()->GetParams();

    // GPU定数を取得できない間は、初期値を設定できない。
    if (!params) {
        return;
    }

    params->vignetteIntensity = 0.0f;
    params->vignettePower = 1.0f;
    params->chromaticAberration = 0.0f;
    params->filmGrainIntensity = 0.0f;
    params->radialIntensity = 0.0f;
    params->colorExposure = 0.0f;
    params->colorContrast = 1.0f;
    params->colorSaturation = 1.0f;
    params->colorTemperature = 0.0f;
    params->damageFlash = 0.0f;
    params->cinemaBarHeight = 0.0f;
    params->bloomIntensity = 0.0f;
    params->dangerVignette = 0.0f;
    params->wobbleIntensity = 0.0f;
    params->colorTint = 0.0f;
    params->scanlineIntensity = 0.0f;
    params->mosaicSize = 0.0f;
    params->grayscaleIntensity = 0.0f;
    params->sepiaIntensity = 0.0f;
    params->boxFilterSize = 0;
    params->gaussianFilterSize = 0;
    params->luminanceOutlineIntensity = 0.0f;
    params->depthOutlineIntensity = 0.0f;
    params->randomIntensity = 0.0f;
    params->dissolveThreshold = 0.0f;
}

void GameOverScene::DrawBackgroundSprite() {
    // 3D シーンより先に背景を描き、未配置時は安全に省略する。
    backgroundSprite_ = IsAlive(backgroundSprite_) ? backgroundSprite_ : FindSprite("gameover_background");

    // 同名の背景を取得できなければ、背景の描画を省いて3D描画へ進む。
    if (!backgroundSprite_) {
        return;
    }

    BeginSpriteDrawing();
    backgroundSprite_->Draw();
}

void GameOverScene::ChangeSelection(std::int32_t direction) {
    // 先頭と末尾を循環させ、どちらの方向入力でも選択を継続できる。
    const std::int32_t count = static_cast<std::int32_t>(MenuItem::kCount);
    selectedIndex_ = (selectedIndex_ + direction + count) % count;
}

void GameOverScene::ConfirmSelection() {
    // リトライ演出中の重複遷移と、メニュー表示前の決定を防ぐ。
    if (retryExitActive_ || retrySceneChangeRequested_ || !IsTitleRevealComplete()) {
        return;
    }

    switch (static_cast<MenuItem>(selectedIndex_)) {
    // リトライは退場の動きを開始し、遷移は動きの終了または時間上限まで待ちます。
    case MenuItem::kRetry:
        StartRetryExit();
        break;

    // タイトルへは残機が保存できた場合だけ即座に遷移します。
    case MenuItem::kTitle:
        // 残機を書き戻せなければ、タイトルへの切り替えを保留する。
        if (!TryResetLivesForExit()) {
            return;
        }

        retrySceneChangeRequested_ = true;
        DebugConsole::GetInstance()->AddLog("[GameOver] Return title selected.");
        SceneManager::GetInstance()->ChangeSceneWithoutLoading("TITLE");
        break;

    // 予定している項目以外の値では、シーン変更を要求しない。
    default:
        break;
    }
}

void GameOverScene::DrawShadow() {
    // オブジェクト破棄中にはシャドウ描画へ進まない。
    if (objectManager_) {
        objectManager_->DrawShadow();
    }
}
