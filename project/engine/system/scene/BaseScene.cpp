#include <cstdint>
#include "CameraManager.h"
#include "BaseScene.h"
#include "DirectXCommon.h"
#include "Object3D.h"
#include "game/system/combat/BulletManager.h"
#include "Camera.h"
#include "GpuParticleManager.h"
#include "LightManager.h"
#include "PostEffect.h"
#include "SceneManager.h"
#include "ScenePreloader.h"
#include "Skybox.h"
#include "engine/graphics/3d/material/MaterialRenderPolicy.h"
#include "engine/graphics/scene/SceneRenderCommands.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

engine::Sprite* BaseScene::AddSprite(std::unique_ptr<engine::Sprite> sprite) {
    // 所有権をシーンへ移した後、編集用の非所有ポインタを返します。
    auto* sprites = GetMutableSprites();
    // シーンが編集可能な Sprite 一覧を持ち、追加する Sprite も有効な場合だけ所有権を移す。
    if (!sprites || !sprite)
        return nullptr;
    engine::Sprite* raw = sprite.get();
    sprites->push_back(std::move(sprite));
    return raw;
}

bool BaseScene::MoveSprite(engine::Sprite* sprite, std::int32_t offset) {
    // 前後一つの入れ替えだけを許可し、描画順と UI 上の並びを揃えます。
    auto* sprites = GetMutableSprites();
    // 編集可能な一覧、対象の参照、隣り合う項目への移動指定が揃う場合だけ処理する。
    if (!sprites || !sprite || (offset != -1 && offset != 1))
        return false;
    auto it = std::find_if(sprites->begin(), sprites->end(), [sprite](const auto& owned) {
        return owned.get() == sprite;
    });
    // 現在のシーンが所有していない Sprite の場合は、並びを変えない。
    if (it == sprites->end())
        return false;
    const auto index = static_cast<std::ptrdiff_t>(std::distance(sprites->begin(), it));
    const auto next = index + offset;
    // 並びの両端から外れる方向へは移動できない。
    if (next < 0 || next >= static_cast<std::ptrdiff_t>(sprites->size()))
        return false;
    std::swap((*sprites)[index], (*sprites)[next]);
    return true;
}

void BaseScene::AddObject(std::unique_ptr<engine::Object3D> object) {
    // オブジェクトを持たない派生シーンは、この操作を明示的に拒否します。
    (void)object;
    throw std::logic_error("This scene does not own Object3d objects");
}

engine::Object3D* BaseScene::AddObjectImmediate(std::unique_ptr<engine::Object3D> object) {
    // 即時追加も所有権を持つ派生シーンだけが実装します。
    (void)object;
    throw std::logic_error("This scene does not own Object3d objects");
}

std::unique_ptr<engine::Object3D> BaseScene::TakeObjectImmediate(engine::Object3D* object) {
    // 所有権のない基底シーンでは持ち出す対象がありません。
    (void)object;
    return nullptr;
}

SceneLoadManifest BaseScene::BuildAsyncLoadManifest() const {
    // 非同期ロードを使う派生シーンだけが必要な資源を宣言します。
    return {};
}

void BaseScene::BeginLoadingInitialize() {
    // 分割初期化を使わないシーンの既定フックです。
}

bool BaseScene::InitializeLoadingStep() {
    // 既定実装は通常の初期化を一回で終えます。
    Initialize();
    return true;
}

float BaseScene::GetLoadingInitializeProgress() const {
    // 分割ロードを実装しない場合の既定の進捗です。
    return 0.0f;
}

void BaseScene::OnActivated() {
    // 非同期準備中ではなく、現在シーンへ切り替わった時点で描画背景を確定します。
    engine::LightManager::GetInstance()->ApplySceneClearColor();
}

void BaseScene::FixedUpdate(float fixedDeltaTime) {
    // シーンが所有する全オブジェクトを固定刻みで進めます。
    for (auto& object : GetObjects()) {
        // 参照のない項目とリプレイ上で削除されている項目を、物理の固定更新から除く。
        if (!object || object->IsReplayRemoved())
            continue;
        object->FixedUpdate(fixedDeltaTime);
    }
}

bool BaseScene::TakePreparedJson(const std::string& path, json& destination) {
    // 先読み済み JSON があれば、再度ディスクを読まずに受け取ります。
    return preparedLoadData_ && preparedLoadData_->TakeJson(path, destination);
}

std::string BaseScene::ResolvePrimaryObjectLayoutPath(const std::string& defaultPath) {
    // Scene Asset の主レイアウト指定は最初の一回だけ消費します。
    if (sceneAssetObjectLayoutConsumed_ || !sceneLoadContext_.IsSceneAsset()) {
        return defaultPath;
    }

    sceneAssetObjectLayoutConsumed_ = true;
    // Scene Assetのパスを使う機会は一回。空欄でも消費し、追加レイアウトは次回から呼出元の指定を使う。
    const std::string& assetPath = sceneLoadContext_.objectLayoutPath;
    return assetPath.empty() ? defaultPath : assetPath;
}

std::string BaseScene::ResolvePrimarySpriteLayoutPath(const std::string& defaultPath) {
    // Sprite 側も Scene Asset の主レイアウトを二重に読みません。
    if (sceneAssetSpriteLayoutConsumed_ || !sceneLoadContext_.IsSceneAsset()) {
        return defaultPath;
    }

    sceneAssetSpriteLayoutConsumed_ = true;
    // 3D配置の消費状態と独立させ、Spriteの主レイアウトだけを一回差し替える。
    const std::string& assetPath = sceneLoadContext_.spriteLayoutPath;
    return assetPath.empty() ? defaultPath : assetPath;
}

bool BaseScene::Destroy(engine::Object3D* object) {
    // 3D オブジェクトの削除は生存確認と遅延破棄を行う共通経路へ渡します。
    return DestroyObject(object);
}

bool BaseScene::Destroy(engine::Sprite* sprite) {
    // Sprite の削除は Replay 保持状態に応じた共通経路へ渡します。
    return DestroySprite(sprite);
}

void BaseScene::CollectReplaySprites(std::vector<engine::Sprite*>& replaySprites) {
    // Replay に現在の Sprite を非所有参照として渡します。
    auto& sprites = GetSprites();
    replaySprites.reserve(replaySprites.size() + sprites.size());

    for (auto& sprite : sprites) {
        // 実体がある Sprite だけ、記録側へ借用参照を渡す。
        if (sprite) {
            replaySprites.push_back(sprite.get());
        }
    }
}

void BaseScene::CaptureReplaySceneState(json& state) const {
    // 派生シーンの追加状態を書けるよう、基底では空のオブジェクトを用意します。
    state = json::object();
}

void BaseScene::RestoreReplaySceneState(const json& state) {
    // 基底シーンには復元する固有状態がありません。
    (void)state;
}

void BaseScene::ReleaseReplaySprites() {
    // Replay が保持していた Sprite を解放し、削除保留の子参照も切ります。
    std::vector<engine::Sprite*> replaySprites;
    CollectReplaySprites(replaySprites);

    for (engine::Sprite* sprite : replaySprites) {
        // 収集できた Sprite ごとに、リプレイが保持中であることを解除する。
        if (sprite) {
            sprite->SetReplayRetained(false);
        }
    }

    auto* sprites = GetMutableSprites();
    // 編集可能な所有リストが無い場合は、保持フラグを外すところまでで終える。
    if (!sprites)
        return;
    sprites->erase(
        std::remove_if(sprites->begin(), sprites->end(), [](const std::unique_ptr<engine::Sprite>& sprite) {
            // リプレイ中に削除された Sprite だけが、親子関係を外したうえで実破棄の対象となる。
            if (!sprite || !sprite->IsReplayRemoved()) {
                return false;
            }
            sprite->SetParent(nullptr, false);
            return true;
        }),
        sprites->end());
}

void BaseScene::RefreshRenderCameraData() {
    // カメラ切り替え後にシーン内の描画物へ定数を反映します。
    auto& objects = GetObjects();

    for (auto& object : objects) {
        // 存在する Object にだけ、切替後のビューの定数を渡す。
        if (object) {
            object->RefreshRenderCameraData();
        }
    }

}

void BaseScene::DrawCameraPreview(engine::Camera* camera, std::int32_t previewBufferIndex) {
    // 指定カメラへ描く際は通常パスと特殊材質パスを同じプレビュー先へ出します。
    if (!camera) {
        return;
    }

    ID3D12Resource* pointLight = engine::LightManager::GetInstance()->GetPointLightResource();
    ID3D12Resource* spotLight = engine::LightManager::GetInstance()->GetSpotLightResource();
    auto& objects = GetObjects();

    // シーンの空モデルが存在し、ライト設定で有効になっている場合に、指定カメラで空を描く。
    if (Skybox* skybox = GetSkybox(); skybox && engine::LightManager::GetInstance()->IsSkyboxEnabled()) {
        skybox->SetTextureHandle(engine::LightManager::GetInstance()->GetSkyboxTextureHandle());
        skybox->Draw(camera->GetConstantBuffer());
    }

    // 不透明・半透明だけをこのラムダへ通し、深度や背景色が必要な材質は後段へ残す。
    auto drawObject = [&](engine::Object3D* object, bool transparentPass) {
        // 無効または非表示の Object は、不透明・半透明のどちらにも送らない。
        if (!object || !object->GetIsRenderVisible()) {
            return;
        }

        const std::int32_t materialType = object->GetMaterialType();
        const auto materialPass = materialRenderPolicy::GetPass(materialType);
        const bool isTransparent = materialPass == materialRenderPolicy::Pass::kTransparent;
        const bool isSpecialMaterial = materialPass == materialRenderPolicy::Pass::kLocalFog ||
            materialPass == materialRenderPolicy::Pass::kSpecial;

        // 特殊材質と、指定された透明パスに属さないものは、この通常描画から除く。
        if (isSpecialMaterial || isTransparent != transparentPass) {
            return;
        }

        object->DrawForCamera(camera, pointLight, spotLight, previewBufferIndex);
    };

    // 演出用カメラPreviewでは通常描画用のScene::Draw()を再利用せず、
    // 共有WVPを汚さない専用描画だけを通します。
    for (auto& object : objects) {
        drawObject(object.get(), false);
    }

    // 透過物は不透明物の深度が揃った後に描き、プレビューでも通常描画と順序を合わせます。
    for (auto& object : objects) {
        drawObject(object.get(), true);
    }

    // 所有する半透明演出も、プレビュー先の床・通常モデルの深度が決まってから描く。
    for (auto& object : objects) {
        // シーン上で描画可能な所有元だけに、それぞれの半透明演出を描かせる。
        if (object && object->GetIsRenderVisible()) {
            object->DrawOwnedTransparentVisualsForCamera(camera, pointLight, spotLight, previewBufferIndex);
        }
    }

    // 特殊材質がないカメラでは画面色と深度を参照する追加パスを確保しません。
    bool hasSpecialMaterial = false;

    for (const auto& object : objects) {
        // 描画される Object の材質か所有演出が特殊なら、通常の色と深度を使う追加パスが必要。
        if (object && object->GetIsRenderVisible() &&
            (IsSpecialMaterialType(object->GetMaterialType()) || object->HasOwnedSpecialMaterialVisuals())) {
            hasSpecialMaterial = true;
            break;
        }
    }

    // 特殊材質を一つも描画しない場合は、ビューごとの背景・深度を確保しない。
    if (!hasSpecialMaterial) {
        return;
    }

    // プレビュー先ごとの画面色・深度を準備し、本編のGrabTextureと混線させません。
    PostEffect* postEffect = PostEffect::GetInstance();
    engine::DirectXCommon* dxCommon = engine::DirectXCommon::GetInstance();
    ID3D12GraphicsCommandList* commandList = dxCommon ? dxCommon->GetCommandList() : nullptr;
    // プレビュー用の番号を後処理のテクスチャ番号へ写し、参照する深度と色を同じ描画先に合わせる。
    const std::int32_t targetTextureIndex = previewBufferIndex == 0
        ? PostEffect::kCameraPreviewTextureIndex
        : PostEffect::kCinematicCameraPreviewTextureIndex;
    // 後処理やコマンドリストを取得できないか、対象の追加パスを開始できない場合は中止する。
    if (!postEffect || !commandList ||
        !postEffect->BeginCameraPreviewSpecialPass(commandList, targetTextureIndex)) {
        return;
    }

    // 特殊材質には現在のプレビューバッファに対応するSRVだけを渡します。
    const uint32_t depthSrvHandle = postEffect->GetDepthSrvHandle(targetTextureIndex);
    const uint32_t grabSrvHandle = postEffect->GetGrabSrvHandle(targetTextureIndex);

    for (auto& object : objects) {
        // 追加パスであっても、非表示と空の Object を描画しない。
        if (!object || !object->GetIsRenderVisible()) {
            continue;
        }

        // 本体が特殊材質なら、同じプレビュー先の背景色と深度を使って描く。
        if (IsSpecialMaterialType(object->GetMaterialType())) {
            object->DrawSpecialMaterialForCamera(
                camera,
                depthSrvHandle,
                grabSrvHandle,
                previewBufferIndex);
        }

        // 本体とは独立して、オブジェクトが所有する特殊材質の演出がある場合も描く。
        if (object->HasOwnedSpecialMaterialVisuals()) {
            object->DrawOwnedSpecialMaterialVisualsForCamera(
                camera,
                depthSrvHandle,
                grabSrvHandle,
                previewBufferIndex);
        }
    }

    // 専用の深度・背景色参照を閉じ、呼び出し側がプレビューを表示用SRVに戻せるようにする。
    postEffect->EndCameraPreviewSpecialPass(commandList, targetTextureIndex);
}

bool BaseScene::DestroyObject(engine::Object3D* object) {
    // 走査中の即時破棄を避け、先に非表示にして削除を予約します。
    if (!IsAlive(object)) {
        return false;
    }

    object->SetIsVisible(false);
    RequestRemoveObject(object);
    return true;
}

bool BaseScene::DestroySprite(engine::Sprite* sprite) {
    // Replay が保持する Sprite は実破棄せず、記録上の削除として隠します。
    auto* sprites = GetMutableSprites();
    // Sprite を所有・編集する一覧が無ければ、削除はできない。
    if (!sprites)
        return false;
    auto it = std::find_if(sprites->begin(), sprites->end(), [sprite](const std::unique_ptr<engine::Sprite>& candidate) {
        return candidate && candidate.get() == sprite;
    });

    // 参照がこのシーンの所有リストになければ、その実体を変更しない。
    if (it == sprites->end()) {
        return false;
    }

    // 再生で後から同じSpriteを参照できるよう、親子の接続を先に外し、実体はシーンに保管する。
    if (sprite->IsReplayRetained()) {
        std::vector<engine::Sprite*> children = sprite->GetChildren();

        for (engine::Sprite* child : children) {
            // 有効な子だけ親から外し、保持中の Sprite の追従が続かないようにする。
            if (child) {
                child->SetParent(nullptr);
            }
        }

        sprite->SetParent(nullptr);
        sprite->SetReplayRemoved(true);
        sprite->SetVisible(false);
        return true;
    }

    std::vector<engine::Sprite*> children = sprite->GetChildren();

    for (engine::Sprite* child : children) {
        // 実破棄する場合も、有効な子を孤立させてから親を解放する。
        if (child) {
            child->SetParent(nullptr);
        }
    }

    sprite->SetParent(nullptr);
    sprite->SetVisible(false);
    // Replay保持が不要なSpriteは、同じ親子解除の手順を終えてから所有リストから破棄する。
    sprites->erase(it);
    return true;
}

bool BaseScene::IsAlive(engine::Object3D* object) {
    // 生存判定は所有配列内に同じポインタがあるかで確認します。
    if (!object) {
        return false;
    }

    auto& objects = GetObjects();
    return std::any_of(objects.begin(), objects.end(), [object](const std::unique_ptr<engine::Object3D>& candidate) {
        return candidate && candidate.get() == object;
    });
}

bool BaseScene::IsAlive(engine::Sprite* sprite) {
    // Replay 上で削除済みの Sprite は所有配列にあっても生存扱いにしません。
    if (!sprite) {
        return false;
    }

    auto& sprites = GetSprites();
    return std::any_of(sprites.begin(), sprites.end(), [sprite](const std::unique_ptr<engine::Sprite>& candidate) {
        return candidate && candidate.get() == sprite && !candidate->IsReplayRemoved();
    });
}

bool BaseScene::IsSpecialMaterialType(std::int32_t materialType) const {
    // Scene Color や深度を参照する材質を専用パスへ振り分けます。
    return materialRenderPolicy::IsSpecial(materialType);
}

bool BaseScene::IsCameraNearViewActor(const engine::Camera* camera, const engine::Object3D* viewActor) const {
    // カメラか視点主体が未設定なら、距離を理由にモデルを隠さない。
    if (!camera || !viewActor) {
        return false;
    }

    // モデル原点から上へ1ワールド単位ずらし、視界を塞ぐ胸元までの距離を測る。
    engine::Vector3 chest = viewActor->GetWorldPosition();
    chest.y += 1.0f;
    const engine::Vector3& eye = camera->GetEye();
    const engine::Vector3 offset = {eye.x - chest.x, eye.y - chest.y, eye.z - chest.z};
    const float distance = std::sqrt(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
    return distance < 3.0f;
}

bool BaseScene::IsHiddenByFirstPerson(engine::Object3D* object, engine::Object3D* viewActor, bool isFirstPerson) const {
    // 一人称視点では視点の主体とその子オブジェクトを隠します。
    if (!isFirstPerson || !object || !viewActor) {
        return false;
    }

    engine::Object3D* current = object;

    while (current) {
        // 親をたどって視点主体に着いた場合、描画しようとした部品も一人称の除外対象になる。
        if (current == viewActor) {
            return true;
        }

        current = current->GetParent();
    }

    return false;
}

bool BaseScene::DrawLocalFogObjects(const std::vector<std::unique_ptr<engine::Object3D>>& objects, engine::Object3D* viewActor, bool isFirstPerson) {
    // 対象がないフレームは専用パスの開始と終了を省略します。
    auto* commands = GetRenderCommands();
    // 共通描画資源を取得できなければ、深度を読む描画を開始できない。
    if (!commands) {
        return false;
    }

    // カメラプレビューでは専用バッファーを使うため、本編の霧パスを呼ばない。
    if (commands->IsCameraPreviewRendering()) {
        return false;
    }

    bool hasFog = false;

    for (auto& obj : objects) {
        // 描画可視でフォグ材質を持ち、一人称の除外にも該当しない物があるか確認する。
        if (obj && obj->GetIsRenderVisible() && obj->GetMaterialType() == 7 && !IsHiddenByFirstPerson(obj.get(), viewActor, isFirstPerson)) {
            hasFog = true;
            break;
        }
    }

    // 霧を描く対象がいなければ、深度バッファーを読み取りへ切り替えない。
    if (!hasFog) {
        return false;
    }

    commands->BeginDepthRead();

    for (auto& obj : objects) {
        // 準備時と同じ条件の霧だけに、現在の深度 SRV を渡す。
        if (obj && obj->GetIsRenderVisible() && obj->GetMaterialType() == 7 && !IsHiddenByFirstPerson(obj.get(), viewActor, isFirstPerson)) {
            obj->DrawLocalFog(commands->GetDepthSrvHandle());
        }
    }

    commands->EndDepthRead();
    return true;
}

bool BaseScene::DrawSpecialMaterialObjects(const std::vector<std::unique_ptr<engine::Object3D>>& objects, BulletManager* bulletManager, engine::Object3D* viewActor, bool isFirstPerson) {
    // 深度・背景色を読む材質をまとめ、必要な時だけ Grab Texture を更新します。
    auto* commands = GetRenderCommands();
    // シーンの描画コマンドを取得できない場合は、カラーのコピーも特殊描画も行わない。
    if (!commands) {
        return false;
    }

    // プレビューでは対象ビュー用の専用処理へ任せ、本編の深度・背景色には触れない。
    if (commands->IsCameraPreviewRendering()) {
        return false;
    }

    bool hasSpecialObjects = false;

    for (auto& obj : objects) {
        // 表示が許可され、本体または所有演出に特殊材質がある Object を探す。
        if (obj && obj->GetIsRenderVisible() &&
            (IsSpecialMaterialType(obj->GetMaterialType()) || obj->HasOwnedSpecialMaterialVisuals()) &&
            !IsHiddenByFirstPerson(obj.get(), viewActor, isFirstPerson)) {
            hasSpecialObjects = true;
            break;
        }
    }

    // Objectの描画パスがなくても、弾だけで背景色を参照する場合を取りこぼさない。
    const bool hasSpecialBullets = bulletManager && bulletManager->HasSpecialMaterialBullets();

    auto* cameraManager = CameraManager::GetInstance();
    auto* activeCamera = cameraManager->GetActiveCamera();
    engine::Object3D* silhouetteSubject = nullptr;
    // 三人称視点のメインカメラで遮蔽表示が有効かつ輪郭色のアルファが正の場合に、輪郭の描画対象を探す。
    if (!isFirstPerson && cameraManager->GetVisibilityFrame().enabled &&
        activeCamera && activeCamera == cameraManager->GetMainCamera() &&
        activeCamera->GetVisibilitySettings().silhouetteColor[3] > 0.0f) {
        // シーンにまだ所属している実体だけ使う。装飾を何重にも重ねず本体を一度描く。
        for (const auto& object : objects) {
            // 追従先と一致し、シーンで現在描画できる Object だけを輪郭表示に使う。
            if (object && object.get() == activeCamera->GetFollowTarget() && object->GetIsRenderVisible()) {
                silhouetteSubject = object.get(); break;
            }
        }
    }
    // オブジェクト、弾、追従者の輪郭のいずれも必要なければ、このパスを省く。
    if (!hasSpecialObjects && !hasSpecialBullets && !silhouetteSubject) {
        return false;
    }

    // 背景色を参照する材質や弾がある場合だけコピーする。輪郭の表示だけなら既存の色を使う。
    const bool grabUpdated = hasSpecialObjects || hasSpecialBullets;
    // 特殊材質の本体か弾を描くときだけ、背景の色を更新する。
    if (grabUpdated) commands->CopySceneColor();

    // 特殊マテリアルはScene深度をSRVとして読むため、描画中だけDSVを読み取り状態へ切り替えます。
    commands->BeginDepthRead();

    // 特殊材質の Object が見つかっている場合は、本体と所有物の描画入口を走査する。
    if (hasSpecialObjects) {
        const uint32_t depthSrvHandle = commands->GetDepthSrvHandle();
        const uint32_t grabSrvHandle = commands->GetSceneColorSrvHandle();

        for (auto& obj : objects) {
            // 無効な参照、非表示、一人称の視点主体とその子を特殊描画から除外する。
            if (!obj || !obj->GetIsRenderVisible() || IsHiddenByFirstPerson(obj.get(), viewActor, isFirstPerson)) {
                continue;
            }

            obj->DrawSpecialMaterial(depthSrvHandle, grabSrvHandle);
            obj->DrawOwnedSpecialMaterialVisuals(depthSrvHandle, grabSrvHandle);
        }
    }

    // 背景色と深度を読む弾がある場合だけ、弾管理の専用描画を呼ぶ。
    if (hasSpecialBullets) {
        bulletManager->DrawSpecial(commands->GetDepthSrvHandle(), commands->GetSceneColorSrvHandle());
    }

    // 輪郭対象とそのメッシュレンダラーを取得できた場合に、透過後の視認性を補う輪郭を描く。
    if (silhouetteSubject && silhouetteSubject->GetMeshRenderer()) {
        silhouetteSubject->GetMeshRenderer()->DrawVisibilitySilhouette(
            commands->GetDepthSrvHandle(), commands->GetSceneColorSrvHandle());
    }
    commands->EndDepthRead();
    // 背景色をコピーしたかだけを返し、この後に描くGPU粒子との二重コピーを避ける。
    return grabUpdated;
}

bool BaseScene::DrawGpuParticles(engine::Camera* camera, uint32_t textureHandle, bool grabAlreadyUpdated) {
    // 背景色が必要な粒子だけコピーを追加し、既済みなら再利用します。
    GpuParticleManager* particleManager = GpuParticleManager::GetInstance();
    auto* commands = GetRenderCommands();

    // 描画手段かカメラが無い場合、または粒子を描く必要が無い場合は、以前のコピー状態をそのまま返す。
    if (!commands || !camera || particleManager->IsEmpty()) {
        return grabAlreadyUpdated;
    }

    // プレビューの呼び出しでは、本編用の深度を参照する粒子描画を省く。
    if (commands->IsCameraPreviewRendering()) {
        return grabAlreadyUpdated;
    }

    // 加算・半透明パーティクルは背景色を参照しないため、全画面コピーを省略します。
    if (!grabAlreadyUpdated && particleManager->RequiresSceneColorCopy()) {
        commands->CopySceneColor();
        grabAlreadyUpdated = true;
    }

    commands->BeginDepthRead();
    commands->RecordGpuParticles(*particleManager, *camera, textureHandle);
    commands->EndDepthRead();
    return grabAlreadyUpdated;
}

void BaseScene::TriggerEvent(std::int32_t targetId) {
    // 送信先が未設定の -1 ならシーンのイベントを発火しない。
    if (targetId == -1)
        return;

    auto& objects = GetObjects();

    for (auto& obj : objects) {
        // 複数のオブジェクトが同じ受信 ID を持てるため、一致した全てに単発イベントを通知する。
        if (obj->GetEventId() == targetId) {
            obj->OnTrigger();
        }
    }
}

void BaseScene::SetEventActive(std::int32_t targetId, bool active) {
    // 同じ受信 ID の全オブジェクトへオン・オフ状態を伝えます。
    if (targetId == -1)
        return;

    auto& objects = GetObjects();

    for (auto& obj : objects) {
        // 各受信 ID が指定値と一致した場合に、同じオン・オフ状態を渡す。
        if (obj->GetEventId() == targetId) {
            obj->OnSwitchEvent(active);
        }
    }
}

engine::Object3D* BaseScene::FindObjectByEventId(std::int32_t eventId) {
    // 未設定の -1 は探索しない。複数一致する場合は所有リストの先頭を返す。
    if (eventId == -1)
        return nullptr;

    auto& objects = GetObjects();

    for (auto& obj : objects) {
        // 最初に一致した受信 ID の借用参照を返す。
        if (obj->GetEventId() == eventId) {
            return obj.get();
        }
    }

    return nullptr;
}

engine::Object3D* BaseScene::FindObjectByPersistentGuid(const std::string& guid) {
    // 保存用 GUID の形式を確かめてからシーン内を探索します。
    if (!engine::Object3D::IsPersistentGuidValid(guid)) {
        return nullptr;
    }

    for (const auto& object : GetObjects()) {
        // 有効な所有参照で、永続識別子が完全に一致した個体を返す。
        if (object && object->GetPersistentGuid() == guid) {
            return object.get();
        }
    }

    return nullptr;
}

const engine::Object3D* BaseScene::FindObjectByPersistentGuid(const std::string& guid) const {
    // 読み取り側も同じ GUID 探索規則を使います。
    return const_cast<BaseScene*>(this)->FindObjectByPersistentGuid(guid);
}

std::size_t BaseScene::EnsureUniquePersistentObjectGuids() {
    // 重複 GUID だけ再発行し、Scene Asset の安定した参照を保ちます。
    std::unordered_set<std::string> usedGuids;
    usedGuids.reserve(GetObjects().size());
    std::size_t regeneratedCount = 0;

    for (const auto& object : GetObjects()) {
        // 所有リストの空要素には識別子を発行しない。
        if (!object)
            continue;

        const std::string currentGuid = object->EnsurePersistentGuid();

        // 今回の走査で初めて使う値なら保持する。重複分だけ新しい識別子を発行する。
        if (usedGuids.insert(currentGuid).second) {
            continue;
        }

        do {
            object->RegeneratePersistentGuid();
        } while (!usedGuids.insert(object->GetPersistentGuid()).second);
        ++regeneratedCount;
    }

    return regeneratedCount;
}

// 名前からスプライトを取得する
engine::Sprite* BaseScene::GetSpriteByName(const std::string& name) {
    auto& sprites = GetSprites();

    for (auto& sprite : sprites) {
        // 同じ名前の最初の有効な Sprite を借用参照として返す。
        if (sprite && sprite->GetName() == name) {
            return sprite.get();
        }
    }

    return nullptr; // 見つからなかった場合
}

// CommonはFramework所有。SceneはManager経由で借りるだけで、解放操作を持たない。
engine::Object3DCommon* BaseScene::GetObject3DCommon() {
    // Sprite 専用の準備かマネージャー未接続なら、3D 用の共通部を要求しない。
    if (GetRenderPreparation() == engine::RenderPreparation::kSpritesOnly || !sceneManager_) return nullptr;
    auto* resources = sceneManager_->GetRenderCommonResources();
    return resources ? resources->GetObject3DCommon() : nullptr;
}

// シーン管理が公開している Sprite 用の描画基盤を借用する。
engine::SpriteCommon* BaseScene::GetSpriteCommon() {
    auto* resources = sceneManager_ ? sceneManager_->GetRenderCommonResources() : nullptr;
    return resources ? resources->GetSpriteCommon() : nullptr;
}

engine::ParticleCommon* BaseScene::GetParticleCommon() const {
    // Sprite 専用の準備かマネージャー未接続なら、CPU 粒子の共通部は使えない。
    if (GetRenderPreparation() == engine::RenderPreparation::kSpritesOnly || !sceneManager_) return nullptr;
    auto* resources = sceneManager_->GetRenderCommonResources();
    return resources ? resources->GetParticleCommon() : nullptr;
}

// 準備済みのシーン描画コマンドを借用する。未接続の場合は nullptr。
engine::SceneRenderCommands* BaseScene::GetRenderCommands() const {
    auto* resources = sceneManager_ ? sceneManager_->GetRenderCommonResources() : nullptr;
    return resources ? resources->GetSceneRenderCommands() : nullptr;
}

void BaseScene::BeginSpriteDrawing() {
    auto* commands = GetRenderCommands();
    auto* sprites = GetSpriteCommon();
    // 描画コマンドと Sprite の共通部が揃わなければ、準備不足として明示的に失敗する。
    if (!commands || !sprites) {
        throw std::logic_error("Scene sprite drawing requires prepared render resources");
    }
    // 描画先のコマンドと共通Sprite設定を結び付け、個別Spriteは描画に必要な設定だけを渡す。
    commands->BindSprites(*sprites);
}

void BaseScene::BeginWorldObjectDrawing(Skybox* skybox, const engine::Camera* camera) {
    // 空とカメラが揃い、環境設定で空が有効なときだけ、通常モデルより先に背景を描きます。
    if (skybox && camera && engine::LightManager::GetInstance()->IsSkyboxEnabled()) {
        skybox->SetTextureHandle(engine::LightManager::GetInstance()->GetSkyboxTextureHandle());
        skybox->Draw(camera->GetConstantBuffer());
    }

    // 空の有無にかかわらず、後続のモデルが使うルートシグネチャと入力方式を設定します。
    GetObject3DCommon()->SetGraphicsCommand();
}

void BaseScene::PrepareGpuParticles(GpuParticlePresetScope scope) {
    // GPU基盤の取得・受け渡しは共通層に置き、シーンは準備範囲だけを選ぶ。
    auto* particles = GpuParticleManager::GetInstance();
    auto* dxCommon = engine::DirectXCommon::GetInstance();
#ifndef USE_IMGUI
    // 配布版の軽量起動では、先読み結果が空でも「指定対象なし」として維持する。
    // 先読み自体が無い場合だけ従来のInitializeへ戻す。
    if (scope == GpuParticlePresetScope::kPreparedScene) {
        // 先読みの一覧がある場合は、空も指定なしとしてその一覧だけを管理に渡す。
        if (GetPreparedLoadData()) {
            particles->InitializeWithPresets(dxCommon, GetPreparedLoadData()->gpuParticlePresetNames);
        // 先読みデータが無い場合は、通常の初期化経路を使う。
        } else {
            particles->Initialize(dxCommon);
        }
        return;
    }
#else
    // Editorで他のプリセットを選べるよう、軽量範囲の指定でも全件を読み込む。
    (void)scope;
#endif
    particles->Initialize(dxCommon);
    particles->LoadAllPresets("Resources/json/gpu_particles/");
}
