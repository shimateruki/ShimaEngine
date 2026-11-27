#define NOMINMAX
#include "game/scene/editor/SceneAssetEditorScene.h"
#include "engine/graphics/3d/light/SceneLighting.h"
#include "game/scene/GameSceneDrawing.h"
#include "ScenePreloader.h"

#include "game/system/combat/BulletManager.h"
#include "Camera.h"
#include "engine/editor/graphics/camera/CameraEditor.h"
#include "CameraManager.h"
#include "CollisionManager.h"
#include "DirectXCommon.h"
#include "game/system/rules/EventManager.h"
#include "GpuParticleManager.h"
#include "InputManager.h"
#include "game/system/scene/LevelLoader.h"
#include "LightEditor.h"
#include "LightManager.h"
#include "MeshEffectManager.h"
#include "Object3D.h"
#include "Object3DCommon.h"
#include "game/system/scene/ObjectManager.h"
#include "ParticleCommon.h"
#include "ParticleManager.h"
#include "ParticleSystem.h"
#include "Player.h"
#include "PostEffect.h"
#include "SceneManager.h"
#include "Skybox.h"
#include "Sprite.h"
#include "SpriteCommon.h"
#include "TextureManager.h"

#include <filesystem>

namespace fs = std::filesystem;

// SceneAssetが指定した配置JSONと表示用の共通画像を非同期先読み対象に登録します。
SceneLoadManifest SceneAssetEditorScene::BuildAsyncLoadManifest() const {
    // シーン資産が指定した配置と描画に使う画像を先読み一覧へ登録する。
    SceneLoadManifest manifest;

    // オブジェクト配置の指定がある場合だけ、そのJSONを先読みする。
    if (!GetSceneLoadContext().objectLayoutPath.empty()) {
        manifest.AddObjectLayout(GetSceneLoadContext().objectLayoutPath);
    }

    // スプライト配置の指定がある場合だけ、そのJSONを先読みする。
    if (!GetSceneLoadContext().spriteLayoutPath.empty()) {
        manifest.AddSpriteLayout(GetSceneLoadContext().spriteLayoutPath);
    }

    // 共通の白画像は常に使用する。空の画像が未指定なら既定のキューブマップを加える。
    manifest.AddTexture("Resources/sprite/common/white.png");
    manifest.AddTexture(GetSceneLoadContext().skyboxPath.empty() ? "Resources/output_skybox.dds"
                                                                 : GetSceneLoadContext().skyboxPath);
    return manifest;
}

void SceneAssetEditorScene::Initialize() {
    // シーン資産を単独で編集できるよう、ゲーム本編と同じ表示基盤をこの場で初期化します。
    dxCommon_ = engine::DirectXCommon::GetInstance();
    inputManager_ = engine::InputManager::GetInstance();

    // カメラと粒子の共通管理へシーンの入力・描画基盤を接続します。
    CameraManager::GetInstance()->Initialize();
    CameraManager::GetInstance()->SetInputManager(inputManager_);

    particleSystem_ = std::make_unique<engine::ParticleSystem>();
    particleSystem_->Initialize(GetParticleCommon(), "Resources/sprite/common/white.png");

    ParticleManager::GetInstance()->Initialize(particleSystem_.get());
    LightEditor::GetInstance()->SetObject3DCommon(GetObject3DCommon());

    // オブジェクトと配置ファイルの管理を用意し、編集シーンで使う弾の衝突管理も接続します。
    objectManager_ = std::make_unique<ObjectManager>();
    levelLoader_ = std::make_unique<LevelLoader>();
    BulletManager::GetInstance()->Initialize(GetObject3DCommon(), engine::CollisionManager::GetInstance());

    PrepareGpuParticles();
    gpuParticleTextureHandle_ = engine::TextureManager::GetInstance()->Load("Resources/sprite/common/white.png");

    const uint32_t skyboxTexture =
        engine::TextureManager::GetInstance()->Load(ResolveSceneSkyboxPath("Resources/output_skybox.dds"));
    skybox_ = std::make_unique<Skybox>();
    skybox_->Initialize(GetObject3DCommon(), skyboxTexture);

    const std::string objectLayoutPath = GetSceneManager() ? GetSceneManager()->GetEditorSceneAssetObjectPath() : std::string();
    const std::string spriteLayoutPath = GetSceneManager() ? GetSceneManager()->GetEditorSceneAssetSpritePath() : std::string();

    // オブジェクト配置のパスが指定され、ファイルが実在するときは既存の配置を展開する。
    if (!objectLayoutPath.empty() && fs::exists(objectLayoutPath)) {
        levelLoader_->LoadObjectLayout(this, objectLayoutPath);
    // 配置が未指定か見つからない場合は、保存先の仮ファイル名を用意する。
    } else {
        SetLoadedFilename("untitled_scene.json");
    }

    // スプライト配置は、パスが指定されファイルが実在する場合だけ展開する。
    if (!spriteLayoutPath.empty() && fs::exists(spriteLayoutPath)) {
        levelLoader_->LoadSpriteLayout(this, spriteLayoutPath);
    }

    // 配置を展開した後でライトとカメラの設定を読み、ゲーム用操作から編集用カメラへ切り替えます。
    engine::LightManager::GetInstance()->LoadState(ResolveSceneLightPath("Resources/json/light/light_layout.json"));
    CameraEditor::GetInstance()->Initialize();
    CameraEditor::GetInstance()->LoadFile(ResolveSceneCameraPath("game_camera.json"));
    CameraEditor::GetInstance()->SetMode(CameraEditor::Mode::kEditor);
}

void SceneAssetEditorScene::Finalize() {
    // シーン内の接触登録と弾を先に片付け、エディターから古い実体を参照させない。
    engine::CollisionManager::GetInstance()->ClearObjects();
    BulletManager::GetInstance()->Finalize();
    player_ = nullptr;
    sprites_.clear();
    skybox_.reset();
    levelLoader_.reset();
    objectManager_.reset();
    particleSystem_.reset();
}

void SceneAssetEditorScene::Update(float deltaTime) {
    // 編集用カメラとライトを更新し、オブジェクトの見え方を現在設定へ追従させる。
    PostEffect::GetInstance()->Update(deltaTime);
    LightEditor::GetInstance()->Update();
    CameraEditor::GetInstance()->Update(player_, false);
    CameraManager::GetInstance()->Update(deltaTime);

    // 配置管理が準備できている間だけ、配置オブジェクトの状態を進める。
    if (objectManager_) {
        objectManager_->Update(deltaTime);
    }

    // シーンのCPU粒子システムを生成済みなら、GPU粒子に先立って更新する。
    if (particleSystem_) {
        particleSystem_->Update(deltaTime);
    }

    GpuParticleManager::GetInstance()->Update(deltaTime);

    for (auto& sprite : sprites_) {
        // 実体を保持しているスプライトだけ、新しいフレームの姿勢へ更新する。
        if (sprite) {
            sprite->Update();
        }
    }

    // 弾の位置を進めた後で、このフレームの接触を計算します。
    BulletManager::GetInstance()->Update(deltaTime);
    engine::CollisionManager::GetInstance()->Update();
}

void SceneAssetEditorScene::Draw() {
    // 描画基盤か配置管理が未初期化なら、シーン描画を行わない。
    if (!objectManager_ || !GetObject3DCommon()) {
        return;
    }

    engine::Camera* camera = CameraManager::GetInstance()->GetActiveCamera();

    BeginWorldObjectDrawing(skybox_.get(), camera);

    // 通常の3Dパスでライト付きの配置物・予兆・弾を描き、ライトの編集表示も重ねます。
    // この描画で使う点光源とスポットライトを一度借り、後続の描画に渡す。
    const auto lighting = engine::SceneLighting::Capture();
    lighting.Draw(*objectManager_, &ObjectManager::Draw);
    DrawEnemyTelegraphs(*this, lighting);
    lighting.Draw(*BulletManager::GetInstance(), &BulletManager::Draw);
    LightEditor::GetInstance()->Draw3D();

    // シーンのCPU粒子システムが用意されていれば、設置物と一緒に描く。
    if (particleSystem_) {
        particleSystem_->Draw();
    }

    // 霧、特殊材質、GPU粒子の順に描き、背景を使う後段へ画面コピーの状態を渡します。
    auto& objects = objectManager_->GetObjects();
    DrawLocalFogObjects(objects, player_, false);
    bool grabUpdated = DrawSpecialMaterialObjects(objects, BulletManager::GetInstance(), player_, false);
    grabUpdated = DrawGpuParticles(camera, gpuParticleTextureHandle_, grabUpdated);

    bool hasAttachedEffects = false;

    for (const auto& object : objects) {
        // 付属エフェクトのいずれかが指定されたオブジェクトを一つでも見つけたら走査を終える。
        if (object && (!object->GetMeshEffect1Name().empty() || !object->GetMeshEffect2Name().empty())) {
            hasAttachedEffects = true;
            break;
        }
    }

    // カメラのプレビュー外で、付属エフェクトが必要なフレームだけ画面コピーを補います。
    if (!dxCommon_->IsCameraPreviewRendering() && hasAttachedEffects) {
        // 前の描画パスで背景コピーを済ませていない場合だけ、ここで取得する。
        if (!grabUpdated) {
            dxCommon_->UpdateGrabTexture();
        }

        for (const auto& object : objects) {
            // 存在して表示中のオブジェクトだけ、その付属エフェクトに描画を依頼する。
            if (object && object->GetIsVisible()) {
                lighting.Draw(*object, &engine::Object3D::DrawAttachedEffects);
            }
        }
    }
}

void SceneAssetEditorScene::DrawUi() {
    // Sprite基盤がある場合だけ共通パイプラインを設定してUIを描く。
    if (!GetSpriteCommon()) {
        return;
    }

    BeginSpriteDrawing();

    for (const auto& sprite : sprites_) {
        // 実体があるスプライトだけ、共通スプライトパイプラインで描画する。
        if (sprite) {
            sprite->Draw();
        }
    }
}

void SceneAssetEditorScene::DrawShadow() {
    // 配置されたオブジェクトを通常シーンと同じ影パスへ渡す。
    if (objectManager_) {
        objectManager_->DrawShadow();
    }
}

const std::vector<std::unique_ptr<engine::Object3D>>& SceneAssetEditorScene::GetObjects() const {
    // 編集ツールへ現在の配置集合を参照で公開し、所有権は移さない。
    return objectManager_->GetObjects();
}


engine::Object3D* SceneAssetEditorScene::AddObjectImmediate(std::unique_ptr<engine::Object3D> object) {
    // 編集操作の直後に選択できるよう、即時登録した実体を返す。
    return objectManager_->AddObjectImmediate(std::move(object));
}

std::unique_ptr<engine::Object3D> SceneAssetEditorScene::TakeObjectImmediate(engine::Object3D* object) {
    // Undoなどが所有権を保持できるよう、実体を配置集合から取り出す。
    return objectManager_->TakeObjectImmediate(object);
}

void SceneAssetEditorScene::AddObject(std::unique_ptr<engine::Object3D> object) {
    // 通常の追加は管理側の追加処理へ渡し、ここで所有し続けない。
    objectManager_->AddObject(std::move(object));
}

void SceneAssetEditorScene::RequestRemoveObject(engine::Object3D* object) {
    // 更新中の走査を壊さないよう、削除要求を管理側へ預ける。
    objectManager_->RequestRemove(object);
}
