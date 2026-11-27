#include "game/system/combat/BulletManager.h"
#include "CollisionManager.h"
#include "Object3DCommon.h"
#include "engine/graphics/3d/material/MaterialRenderPolicy.h"
#include <algorithm>

BulletManager* BulletManager::GetInstance() {
    // 弾リストと衝突登録をシーン内で一箇所に集約する。
    static BulletManager sInstance;
    return &sInstance;
}

void BulletManager::Initialize(engine::Object3DCommon* common, engine::CollisionManager* colManager) {
    // 新しいシーンの描画・衝突基盤へ付け替え、旧弾を残さない。
    common_ = common;
    colManager_ = colManager;
    bullets_.clear();
}

void BulletManager::Finalize() {
    // CollisionManagerへ登録した弾を先に外してから参照を破棄する。
    Clear();
    common_ = nullptr;
    colManager_ = nullptr;
}

void BulletManager::Clear() {
    // 衝突管理の参照が有効な場合は、弾の所有権を消す前に衝突登録を外す。
    if (colManager_) {
        for (const auto& bullet : bullets_) {
            // 実体がありプレビュー専用ではない弾だけ、登録していた衝突リストから外す。
            if (bullet && !bullet->IsPreviewOnly()) {
                colManager_->RemoveObject(bullet.get());
            }
        }
    }

    bullets_.clear();
}

void BulletManager::ClearPreviewForOwner(const engine::Object3D* owner) {
    // 編集を止めた砲台の弾だけ消し、他の砲台やゲーム用の弾を保ちます。
    bullets_.remove_if([owner](const std::unique_ptr<Bullet>& bullet) {
        return bullet && bullet->IsPreviewOnly() && bullet->GetPreviewOwner() == owner;
    });
}

void BulletManager::Fire(const engine::Vector3& pos, const engine::Vector3& vel, uint32_t attr, uint32_t mask, const std::string& model,
                         float radius, float life, const BulletVisualConfig& visualConfig, float damage,
                         const StatusEffectApplication& statusEffect, DamageType damageType, float gravity) {

    // 描画と衝突の初期化先が揃う前は、弾を作成しません。
    if (!common_ || !colManager_) {
        return;
    }

    // 編集用の発射だけ、砲台ごとと全体の同時表示数を確認する。
    if (visualConfig.previewOnly) {
        // 既存弾の持続を確認できるよう、上限に達していればその発射要求を見送る。
        // 最短プレビュー間隔0.8秒と設定可能な最大寿命20秒を一台分収容します。
        constexpr size_t kMaxPerCannon = 32;
        constexpr size_t kMaxTotal = 128;
        const auto isOwnerPreview = [&visualConfig](const std::unique_ptr<Bullet>& bullet) {
            return bullet->IsPreviewOnly() && bullet->GetPreviewOwner() == visualConfig.previewOwner;
        };
        // 同じ砲台のプレビュー弾が上限に達していれば、新しい弾の生成を見送る。
        if (std::count_if(bullets_.begin(), bullets_.end(), isOwnerPreview) >= kMaxPerCannon) {
            return;
        }
        const auto isPreview = [](const std::unique_ptr<Bullet>& bullet) { return bullet->IsPreviewOnly(); };
        // 全砲台を合わせたプレビュー弾数が上限に達している場合も、生成を見送る。
        if (std::count_if(bullets_.begin(), bullets_.end(), isPreview) >= kMaxTotal) {
            return;
        }
    }

    // 弾を登録する前に、モデルと共通の描画設定を初期化する。
    auto bullet = std::make_unique<Bullet>();
    bullet->Initialize(common_);
    // プレビューなどから個別の確認用ラベルを渡された場合だけ、弾の名前を付ける。
    if (!visualConfig.debugLabel.empty())
        bullet->SetName(visualConfig.debugLabel);
    bullet->SetModel(model);
    bullet->SetScale({visualConfig.visualScale, visualConfig.visualScale, visualConfig.visualScale});
    bullet->SetMaterialType(visualConfig.materialType);
    bullet->SetBlendMode(visualConfig.blendMode);
    bullet->SetColor(visualConfig.color);
    bullet->SetEmissive(visualConfig.emissive);

    // テクスチャパスが指定された弾だけ、モデルの元のテクスチャを上書きする。
    if (!visualConfig.texturePath.empty()) {
        bullet->SetTexture(visualConfig.texturePath);
    }

    // メッシュを持つ弾だけ、材質の追加パラメーターを調べる。
    if (engine::MeshRenderer* renderer = bullet->GetMeshRenderer()) {
        // 材質のパラメーター領域がある場合は、波や発光などの表示設定を渡す。
        if (auto* param = renderer->GetWaterParamData()) {
            param->waveSpeed = visualConfig.waveSpeed;
            param->waveHeight = visualConfig.waveHeight;
            param->waveFrequency = visualConfig.waveFrequency;
            param->effectType = visualConfig.effectType;
            param->effectScale = visualConfig.effectScale;
            param->effectSoftness = visualConfig.effectSoftness;
            param->effectIntensity = visualConfig.effectIntensity;
            param->billboardScale = visualConfig.billboardScale;

            // マグマ弾の表示を使う場合は、発射位置と半径から液面のクリップ高さを渡す。
            if (visualConfig.magmaHopperPresentation) {
                param->effectScaleY = pos.y - radius * 0.5f;
            }
        }
    }

    // 球の当たり判定を設定します。半径がワールド単位の場合はモデルの拡縮を判定に掛けません。
    bullet->SetColliderType(engine::ColliderType::kSphere);
    bullet->SetCollisionRadius(radius);
    bullet->SetCollisionSize({radius, radius, radius});
    // ワールド半径を指定され、コライダーがある場合は、モデルの拡縮を判定半径に掛けない。
    if (visualConfig.collisionRadiusIsWorld && bullet->GetCollider())
        bullet->GetCollider()->SetScaleOverride({1.0f, 1.0f, 1.0f});
    // マグマ弾のコライダーがあれば、表示の伸縮を判定に掛けず、弾の影も止める。
    if (visualConfig.magmaHopperPresentation && bullet->GetCollider()) {
        bullet->GetCollider()->SetScaleOverride({1.0f, 1.0f, 1.0f});
        bullet->SetCastShadow(false);
    }

    // プレビューは発射時の属性・マスク・ダメージを無効にし、発射後に所有する砲台を記録する。
    if (visualConfig.previewOnly) {
        bullet->Fire(pos, vel, life, 0, 0, 0.0f, statusEffect, damageType);
        bullet->SetPreviewOnly(visualConfig.previewOwner);
    } else {
        bullet->Fire(pos, vel, life, attr, mask, damage, statusEffect, damageType);
    }
    bullet->SetMagmaHopperPresentation(visualConfig.magmaHopperPresentation);
    bullet->SetExplosionRadius(visualConfig.explosionRadius);
    // 爆発半径を持つ弾は、コライダーがあれば直撃判定のサイズを表示倍率から独立させる。
    if (visualConfig.explosionRadius > 0.0f && bullet->GetCollider())
        bullet->GetCollider()->SetScaleOverride({1.0f, 1.0f, 1.0f});
    // ここで受け取る重力は下向き加速度の大きさ。負の値は加速なしとして扱う。
    bullet->SetGravity((std::max)(0.0f, gravity));
    // プレビューの負荷を抑えるため、残光の間隔に下限を設けて二重の残光を省きます。
    const float trailInterval = visualConfig.previewOnly
        ? (std::max)(0.14f, visualConfig.trailInterval) : visualConfig.trailInterval;
    const std::string secondaryTrail = visualConfig.previewOnly ? std::string{} : visualConfig.trailSecondaryPreset;
    bullet->ConfigureVfx(visualConfig.trailPreset, secondaryTrail, visualConfig.impactPreset,
                         visualConfig.impactSecondaryPreset, trailInterval, visualConfig.trailSpeedScale,
                         visualConfig.trailReverseDirection, visualConfig.impactDirected);
    bullet->ConfigureBurningVfx(visualConfig.burningPreset, radius);
    bullet->ConfigureDestructionVfx(visualConfig.impactDebrisPreset, visualConfig.impactDebrisSecondaryPreset,
                                    visualConfig.impactDebrisScale, visualConfig.impactOnExpiry,
                                    visualConfig.destroyOnSolidCastHit);

    // 実戦弾だけを衝突管理へ登録し、弾の所有は実戦・プレビューとも管理リストへ移します。
    if (!visualConfig.previewOnly)
        colManager_->AddObject(bullet.get());
    bullets_.push_back(std::move(bullet));
}

// 全弾の位置と寿命を先に進め、終了した弾の衝突登録と所有を後から解除します。
void BulletManager::Update(float deltaTime) {
    // 当たり判定マネージャーがない間は弾の更新と登録を行わない。
    if (!colManager_) {
        return;
    }

    for (auto& bullet : bullets_) {
        bullet->Update(deltaTime);
        bullet->UpdateLocalMatrix();
        bullet->UpdateWorldMatrix();
    }

    // 全弾の位置を更新した後で、回収する弾を判定する。
    bullets_.remove_if([this](const std::unique_ptr<Bullet>& bullet) {
        // 寿命切れや着弾で終了した弾だけ、所有リストから回収する。
        if (bullet->IsDead()) {
            // 終了した実戦弾は、破棄前に衝突リストからも外す。
            if (!bullet->IsPreviewOnly())
                colManager_->RemoveObject(bullet.get());
            return true;
        }
        return false;
    });
}

#ifdef USE_IMGUI
void BulletManager::UpdatePreview(float deltaTime) {
    // 時間が進まないフレームでは、プレビューの軌道や寿命を進めません。
    if (deltaTime <= 0.0f) {
        return;
    }

    for (auto& bullet : bullets_) {
        // 実体がありプレビュー専用に設定された弾だけ、編集時の時間で位置と寿命を進める。
        if (bullet && bullet->IsPreviewOnly()) {
            bullet->Update(deltaTime);
            bullet->UpdateLocalMatrix();
            bullet->UpdateWorldMatrix();
        }
    }

    // プレビュー弾は CollisionManager に登録していないため、参照解除は不要です。
    bullets_.remove_if([](const std::unique_ptr<Bullet>& bullet) {
        return bullet && bullet->IsPreviewOnly() && bullet->IsDead();
    });
}

// 確認用として発射した弾が管理リストに一つでも存在するかを返します。
bool BulletManager::HasPreviewBullets() const {
    return std::any_of(bullets_.begin(), bullets_.end(), [](const std::unique_ptr<Bullet>& bullet) {
        return bullet && bullet->IsPreviewOnly();
    });
}
#endif

void BulletManager::Draw(ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource) {
    // 特殊マテリアル弾は後段で描くため、この不透明描画から除く。
    for (auto& bullet : bullets_) {
        // 専用マテリアルの弾は、通常パスから外して後段のDrawSpecial()に渡す。
        if (materialRenderPolicy::IsSpecial(bullet->GetMaterialType())) {
            continue;
        }

        bullet->Draw(pointLightResource, spotLightResource);
    }
}

bool BulletManager::HasSpecialMaterialBullets() const {
    // 専用描画パスが必要な生存弾の有無を問い合わせる。
    for (const auto& bullet : bullets_) {
        // 終了前で専用マテリアルを使う弾が一つでもあれば、専用パスが必要と返す。
        if (!bullet->IsDead() && materialRenderPolicy::IsSpecial(bullet->GetMaterialType())) {
            return true;
        }
    }

    return false;
}

void BulletManager::DrawSpecial(uint32_t depthSrvHandle, uint32_t grabSrvHandle) {
    // 深度や画面コピーが揃った段階で特殊マテリアル弾だけ描く。
    for (auto& bullet : bullets_) {
        // 終了済みの弾や通常マテリアルの弾は、このパスの対象から外す。
        if (bullet->IsDead() || !materialRenderPolicy::IsSpecial(bullet->GetMaterialType())) {
            continue;
        }

        bullet->DrawSpecialMaterial(depthSrvHandle, grabSrvHandle);
    }
}

#ifdef USE_IMGUI
void BulletManager::BeginEditorSession() {
    // すでに編集セッション中なら、実戦弾を退避したリストを入れ替え直さない。
    if (editorSession_) return;
    bullets_.swap(savedEditorBullets_); editorSession_ = true;
}
void BulletManager::EndEditorSession() {
    // 編集セッションを開始していない場合は、削除する編集弾や復元対象がないので戻る。
    if (!editorSession_) return;
    Clear(); bullets_.swap(savedEditorBullets_); editorSession_ = false;
}
#endif
