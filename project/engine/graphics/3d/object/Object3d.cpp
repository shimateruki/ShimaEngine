#define NOMINMAX
#include <cstdint>
#include "Object3D.h"
#include "Object3DTransformUtils.h"
#include "EditorToolSettings.h"
#include "DirectXCommon.h"
#include "ModelManager.h"
#include "EffectObject3D.h"
#include "SrvManager.h"
#include "CameraManager.h"
#include "SceneManager.h"
#include "GhostRecorder.h"
#include "CollisionManager.h"
#include <cassert>
#include <algorithm> // min, max
#include <ParticleManager.h>
#include <GpuParticleManager.h>
#include <GpuParticleEmitter.h>
#include <DebugConsole.h>
#include <ProfilerManager.h>
#include <fstream>
#include <filesystem>

namespace engine {

namespace {

// 親候補の祖先に対象が含まれるか調べ、階層の循環を防ぎます。
bool HasParentInChain(Object3D* parent, const Object3D* child) {
    for (Object3D* current = parent; current != nullptr; current = current->GetParent()) {
        // 祖先が子候補そのものの場合、親を設定すると階層が循環する。
        if (current == child) {
            return true;
        }
    }

    return false;
}

}

// ========================================================================
// Object3D 基本処理
// ------------------------------------------------------------------------
// 生成、毎フレーム更新、親子Transform、MeshRendererへの基本委譲を担当する。
// コリジョン、JSON、付属エフェクトは別ファイルに分離して見通しを保つ。
// ========================================================================
Object3D::Object3D() = default;

Object3D::~Object3D() {
    // 他のメンバを片付ける前に、記録器の接続を解放する。
    recorder_.reset();

    // 親がいる場合、親の子供リストから自分自身を取り除く
    if (parent_) {
        std::vector<Object3D*>& kids = parent_->children_;
        kids.erase(std::remove(kids.begin(), kids.end(), this), kids.end());
        parent_ = nullptr;
    }
    
    // 子は所有していないため、親への参照と変形階層の参照を外す。
    for (auto* child : children_) {
        // まだ参照が残っている子だけを解除する。
        if (child) {
            child->parent_ = nullptr;
            child->transform_.parent = nullptr;
        }
    }

    children_.clear();
}

// ========================================================================
// 初期化
// ========================================================================
// 描画・衝突・記録の共通基準を用意し、リプレイと配置の識別子を確定する。
void Object3D::Initialize(engine::Object3DCommon* common) {
    assert(common);
    common_ = common;
    EnsurePersistentGuid();
    EnsureReplayId();
    replayRetained_ = false;
    replayRemoved_ = false;
    particleEmitterComponent_.reset();
    meshEffectComponent_.reset();
    pathMoverComponent_.reset();
    gameplayLinkComponent_.reset();
    materialInstancePath_.clear();
    decalSettings_ = DecalSettings{};
    decalElapsedTime_ = 0.0f;
    decalAuthoredAlpha_ = 1.0f;

    // Transform初期化
    transform_.scale = { 1.0f, 1.0f, 1.0f };
    transform_.rotate = { 0.0f, 0.0f, 0.0f };
    transform_.translate = { 0.0f, 0.0f, 0.0f };
    transform_.parent = nullptr;

    // コライダーとレンダラーが同じTransformを借用するので、位置や拡縮の更新元を一つに保ちます。
    collider_ = std::make_unique<engine::Collider>(&transform_);

    // 同じ Transform を描画側にも結び、遮蔽の対象判定に所有者を使う。
    meshRenderer_ = std::make_unique<engine::MeshRenderer>(&transform_);
    meshRenderer_->Initialize(common_);
    meshRenderer_->SetVisibilityOwner(this);

    // 初期姿勢を行列と描画定数へ確定してから、記録の入口を準備する。
    UpdateLocalMatrix();
    UpdateWorldMatrix();

    // レコーダー
    InitializeRecorder(nullptr);

    isCollecting_ = false;
    collectTimer_ = 0.0f;
    gpuEmitter_ = nullptr;
    isDead_ = false;
}

// ========================================================================
// 更新・描画
// ========================================================================

// 草木の風と水面の流れは描画回数に依存させず、実際に進んだ秒数を個別の材質時計へ渡す。
void Object3D::Update(float deltaTime) {
    // 風揺れ用材質30・31の場合に、正で有限な実経過秒を専用の時計へ渡す。
    if (meshRenderer_ && (GetMaterialType() == 30 || GetMaterialType() == 31)
        && std::isfinite(deltaTime) && deltaTime > 0.0f) {
        meshRenderer_->AdvanceMaterialTime(deltaTime);
    }

    // 水材質8の場合は、時計と UV の流れを進める専用関数へ経過秒を渡す。
    if (meshRenderer_ && GetMaterialType() == 8) {
        meshRenderer_->AdvanceWaterAnimation(deltaTime);
    }

    auto startAll = std::chrono::high_resolution_clock::now();
    
    // 毎フレームリセット
    cpuUpdateTimeMs_ = 0.0f;
    cpuAnimTimeMs_ = 0.0f;
    cpuMatrixTimeMs_ = 0.0f;

    // 一時Decalだけ寿命を進めます。lifetimeが0の配置Decalは永続表示です。
    if (decalSettings_.enabled && decalSettings_.lifetime > 0.0f && isVisible_) {
        decalElapsedTime_ += std::max(deltaTime, 0.0f);
        float alphaFactor = 1.0f;

        // 出現秒数が正の場合だけ、経過秒を開き時間で割って透明度へ反映する。
        if (decalSettings_.fadeIn > 0.0f) {
            alphaFactor = std::min(alphaFactor, decalElapsedTime_ / decalSettings_.fadeIn);
        }

        const float remaining = decalSettings_.lifetime - decalElapsedTime_;

        // 出現と消失が重なる寿命では小さい方を採用します。残り秒が負になっても、最終的な透明度は0未満にしません。
        if (decalSettings_.fadeOut > 0.0f) {
            alphaFactor = std::min(alphaFactor, remaining / decalSettings_.fadeOut);
        }

        engine::Vector4 color = GetColor();
        color.w = decalAuthoredAlpha_ * std::clamp(alphaFactor, 0.0f, 1.0f);
        SetColor(color);

        // 寿命の秒数を使い切ったデカールは、最終透明度の反映後に非表示へ移す。
        if (decalElapsedTime_ >= decalSettings_.lifetime) {
            SetIsVisible(false);

            // 一時配置のデカールだけシーンの削除対象にする。配置済みの設定を持つデカールは非表示で残す。
            if (decalSettings_.transient) {
                isDead_ = true;
            }
        }
    }

    // 拾われた収集物だけ、上昇と回転を与える消滅演出へ進める。
    if (isCollecting_) {
        // 常駐粒子を取得位置へ残さず、GameRuleの一度きりの取得演出へ切り替える。
        if (eventType_ == EventType::kStarCoin) {
            SetGpuParticleName("");
            gpuEmitter_.reset();
        }
        collectTimer_ += deltaTime;

        // 収集中だけ回転アニメーションへ切り替えます。
        transform_.isQuaternionMaster = false;
        // 収集中は秒あたり10ワールド単位ずつ上げ、毎秒15ラジアンで回します。
        transform_.translate.y += 10.0f * deltaTime; // 上昇
        transform_.rotate.y += 15.0f * deltaTime;    // 回転

        // 収集から0.5秒に達したら表示を止め、通常のシーン更新から除去できる状態にする。
        if (collectTimer_ >= 0.5f) {
            isCollecting_ = false;
            isVisible_ = false;
            isDead_ = true; // 実体の除去はシーン管理へ委ねる。
        }
    }
    // 未収集で表示中のスターコインだけ、配置のまま常駐粒子を使う。
    else if (eventType_ == EventType::kStarCoin && isVisible_) {
        // 常駐中にTransformを回し続けると、保存した向きが毎フレーム壊れて
        // エディター表示とゲーム中の見え方が一致しなくなります。
        // 保存された向きは変更せず、きらめきパーティクルだけで動きを出します。

        // 明示された銀色などのプリセットを尊重し、後段のUpdateParticleで発生器を一元管理する。
        if (GetGpuParticleName().empty()) {
            SetGpuParticleName("shared/star_sparkle");
        }
    }

    // 常駐 GPU 発生器を作成済みなら、今回の経過秒で発生時計を進める。
    if (gpuEmitter_) {
        gpuEmitter_->Update(deltaTime);
    }

    // --- アニメーション計測 ---
    auto startAnim = std::chrono::high_resolution_clock::now();

    // メッシュとモデルを持つ物だけ、コントローラーと手動クリップの姿勢を評価する。
    if (meshRenderer_ && meshRenderer_->GetModel()) {
        bool appliedAnimation = false;
        engine::Model* model = meshRenderer_->GetModel();
        const bool controllerApplied = UpdateAnimatorController(deltaTime, model);

        // コントローラーを先に評価し、姿勢を決められなかったときだけ直接指定したクリップへ進みます。
        if (controllerApplied) {
            appliedAnimation = true;
        // コントローラーが姿勢を出さず、手動のアニメーション名がある場合にだけクリップを探す。
        } else if (!animName_.empty()) {
            const engine::Model::Animation* anim = model->GetAnimation(animName_);

            // 名前のクリップが存在する場合に、専用時刻を進めて姿勢を反映する。
            if (anim) {
                animationTime_ += deltaTime;
                float time = animationTime_;

                // 繰り返し指定で尺が正の場合は、末尾を過ぎた時刻を次周へ折り返す。
                if (isAnimLoop_ && anim->duration > 0.0f) {
                    time = std::fmod(time, anim->duration);
                // 一回再生など折返しを使わない場合は、時刻をクリップの末尾に留める。
                } else {
                    time = std::min(time, anim->duration);
                }

                model->ApplyAnimation(*anim, time);
                appliedAnimation = true;
            }
        }

        // コントローラーが姿勢を適用した場合はその経路でモデルも更新されるため、ここで二重更新しません。
        if (!controllerApplied) {
            model->Update(appliedAnimation);
        }
    }

    auto endAnim = std::chrono::high_resolution_clock::now();
    cpuAnimTimeMs_ = static_cast<float>(std::chrono::duration_cast<std::chrono::microseconds>(endAnim - startAnim).count()) / 1000.0f;

    // --- 行列・その他計測 ---
    auto startMat = std::chrono::high_resolution_clock::now();

    // 派生クラスがこの後Transformを変更する可能性があるため、ここでは
    // エフェクト追従に必要な行列だけを更新します。描画定数はObjectManagerが
    // 全オブジェクトのロジック更新後に一度だけ確定します。
    UpdateWorldMatrix(false);

    // 記録器があるときだけ、先に更新した座標から移動記録・再生を進める。
    if (recorder_) {
        recorder_->Update(deltaTime);
    }

    UpdateParticle();
    UpdateAttachedEffects(deltaTime);
    auto endMat = std::chrono::high_resolution_clock::now();
    cpuMatrixTimeMs_ = static_cast<float>(std::chrono::duration_cast<std::chrono::microseconds>(endMat - startMat).count()) / 1000.0f;

    auto endAll = std::chrono::high_resolution_clock::now();
    cpuUpdateTimeMs_ = static_cast<float>(std::chrono::duration_cast<std::chrono::microseconds>(endAll - startAll).count()) / 1000.0f;
}

// CPU粒子はこの更新で放出する。GPU粒子は設定名から発生器を準備し、Object3D::Updateが次の周期更新を担当する。
void Object3D::UpdateParticle() {
    ParticleEmitterComponent* particle = GetParticleEmitterComponent();

    // 粒子 Component が削除されていたら、GPU の常駐発生器も破棄する。
    if (!particle) {
        gpuEmitter_.reset();
        return;
    }

    // CPU プリセットの指定があれば、この Object のワールド位置で発生する。
    if (!particle->GetCpuParticle().empty()) {
        engine::Vector3 pos = GetWorldPosition();
        ParticleManager::GetInstance()->Emit(
            particle->GetCpuParticle(),
            pos,
            particle->GetEmissionTimer());
    }

    // GPU プリセットがあれば、その名前に対応する常駐発生器を用意する。
    if (!particle->GetGpuParticle().empty()) {
        // 初めて設定した場合は、この Object を発生元として新しい発生器を開始する。
        if (!gpuEmitter_) {
            gpuEmitter_ = std::make_unique<GpuParticleEmitter>();
            gpuEmitter_->Initialize(particle->GetGpuParticle(), this);
            gpuEmitter_->Play();
        }

        // 既存の発生器と設定名が異なれば、新しい設定で同じ発生器を初期化する。
        else if (gpuEmitter_->GetName() != particle->GetGpuParticle()) {
            gpuEmitter_->Initialize(particle->GetGpuParticle(), this);
            gpuEmitter_->Play();
        }
    }
    // GPU 名を空にした場合は、その Object からの常駐発生を終了する。
    else {
        // 発生器が生成済みの場合だけ、所有を解除する。
        if (gpuEmitter_) {
            gpuEmitter_ = nullptr;
        }
    }
}

// 通常パスの可視モデルを描く。詳細計測中は、そのメッシュの GPU 時間も記録する。
void Object3D::Draw(ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource) {
    // 現在の描画で可視でないオブジェクトは、補助表示も含めて描かない。
    if (!GetIsRenderVisible())
        return;
#ifdef USE_IMGUI
    // シーン配置カメラも補助表示です。動作や保存データは変更せず描画だけを抑制します。
    if (IsCameraObject() && !EditorToolSettings::Get().IsVisible("cameraBody"))
        return;
    // GPU 粒子や判定箱の補助モデルを無効にしている場合、可視性の設定にかかわらず描かない。
    if ((className_ == "GPUParticle" || className_ == "InvisibleBox") && !EditorToolSettings::Get().IsVisible("helperModels"))
        return;
#endif
    // シーンに置いたカメラは編集用の物体としてのみ通常パスに出す。
    if (IsCameraObject()) {
        SceneManager* sceneManager = SceneManager::GetInstance();

        // 本編の再生中やカメラプレビューへの描画中は、カメラの筐体を映像に重ねない。
        if ((sceneManager && sceneManager->IsPlaying()) || engine::DirectXCommon::GetInstance()->IsCameraPreviewRendering()) {
            return;
        }
    }
#ifdef DD // "Release" ビルドの時だけ有効になるマクロ
    // 配布版ではカメラ・粒子・判定箱の補助モデルを描かない。
    if (IsCameraObject() || className_ == "GPUParticle" || className_ == "InvisibleBox") {
        return;
    }
#endif
    // メッシュレンダラーがある場合だけ、通常パスで本体を描く。
    if (meshRenderer_) {
        bool sampling = ProfilerManager::GetInstance()->IsGpuSampling();

        // 個別 GPU 計測が有効な場合に、描画を囲う計測を名前付きで開始する。
        if (sampling) {
            common_->GetDxCommon()->StartGpuProfile(name_);
        }

        meshRenderer_->Draw(pointLightResource, spotLightResource);

        // このメッシュの描画前に計測を開始した場合は、描画直後に対応する計測を閉じる。
        if (sampling) {
            common_->GetDxCommon()->EndGpuProfile(name_);
        }
    }

    // 付属エフェクトの歪みは背景コピーを参照する。
    // 背景コピー更新後に、付属エフェクト用の描画パスから描く。
}

// ========================================================================
// トランスフォーム操作 (Transformへの委譲)
// ========================================================================

void Object3D::DrawForCamera(engine::Camera* camera, ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource, std::int32_t previewBufferIndex) {
    // プレビュー用カメラでは補助モデルの表示条件を揃え、通常描画と同じ可視性を守る。
    if (!GetIsRenderVisible())
        return;
#ifdef USE_IMGUI
    // エディターでカメラ本体の補助表示が無効なら、明示カメラからも描かない。
    if (IsCameraObject() && !EditorToolSettings::Get().IsVisible("cameraBody"))
        return;
    // 補助モデルの表示設定が無効なら、粒子発生位置と判定箱を描かない。
    if ((className_ == "GPUParticle" || className_ == "InvisibleBox") && !EditorToolSettings::Get().IsVisible("helperModels"))
        return;
#endif
    // 映像を確認するカメラプレビューには、シーンに配置したカメラの筐体を混ぜない。
    if (IsCameraObject() && engine::DirectXCommon::GetInstance()->IsCameraPreviewRendering()) {
        return;
    }
#ifdef DD
    // 配布版の明示カメラ描画でも、編集用の補助モデルを除外する。
    if (IsCameraObject() || className_ == "GPUParticle" || className_ == "InvisibleBox") {
        return;
    }
#endif
    // メッシュレンダラーがあるとき、現在の描画とは別のビュー定数を指定して描く。
    if (meshRenderer_) {
        meshRenderer_->DrawForCamera(camera, pointLightResource, spotLightResource, previewBufferIndex);
    }
}

void Object3D::UpdateLocalMatrix() {
    // ローカルのTransform値を行列へ確定する。
    transform_.UpdateMatrix();
}

void Object3D::UpdateWorldMatrix(bool refreshRenderer) {
    // 親子階層を順に更新し、必要なときだけ描画定数も更新する。
    transform_.UpdateMatrix();

    // 描画定数までの更新が要求され、レンダラーがあればこのタイミングで反映する。
    if (refreshRenderer && meshRenderer_) {
        meshRenderer_->Update();
    }

    for (Object3D* child : children_) {
        // 親から続けて、存在する子のワールド行列も更新する。
        if (child) {
            child->UpdateWorldMatrix(refreshRenderer);
        }
    }
}

void Object3D::RefreshRenderCameraData() {
    // プレビューカメラの切替後に、子も含めてカメラ依存の描画値を更新する。
    transform_.UpdateMatrix();

    // レンダラーを持つときは、カメラに依存する描画定数だけを取り直す。
    if (meshRenderer_) {
        meshRenderer_->RefreshCameraDependentData();
    }

    for (Object3D* child : children_) {
        // 子が有効なら、親と同じビューのカメラ依存値を更新する。
        if (child) {
            child->RefreshRenderCameraData();
        }
    }
}

void Object3D::SetParent(Object3D* parent, bool keepWorldTransform) {
    // 親子の循環を拒否し、指定された場合は付け替え前のワールド姿勢を維持する。
    if (parent == this || HasParentInChain(parent, this)) {
        return;
    }

    // 同じ親に付け直す指定で姿勢保持も不要なら、子リストと行列を変更しない。
    if (parent == parent_ && !keepWorldTransform) {
        return;
    }

    UpdateWorldMatrix();

    // 新旧のワールド行列を保持してから階層を付け替え、新しい親の逆行列で同じワールド姿勢をローカルに戻します。
    // 新しい親が存在する場合だけ、その座標系を変換前に更新する。
    if (parent) {
        parent->UpdateWorldMatrix();
    }

    engine::Math math;
    engine::Matrix4x4 worldBefore = transform_.matWorld;
    engine::Matrix4x4 parentWorld = parent ? parent->GetWorldMatrix() : engine::Math::MakeIdentity4x4();

    // 古い親が存在する場合は、先に自分の借用参照を子リストから外す。
    if (parent_) {
        std::vector<Object3D*>& kids = parent_->children_;
        kids.erase(std::remove(kids.begin(), kids.end(), this), kids.end());
    }

    parent_ = parent;

    // 新しい親があるときは、オブジェクト階層と Transform 階層の両方を接続する。
    if (parent_) {
        // 親の子リストにこの個体がまだ登録されていない場合だけ追加する。
        if (std::find(parent_->children_.begin(), parent_->children_.end(), this) == parent_->children_.end()) {
            parent_->children_.push_back(this);
        }

        transform_.parent = parent_->GetTransform();
    // 親を解除した場合は、変形もワールド直下のローカルとして更新する。
    } else {
        transform_.parent = nullptr;
    }

    // ワールド姿勢を保持する指定の場合だけ、取り付け後のローカル値を再構成する。
    if (keepWorldTransform) {
        engine::Matrix4x4 localMatrix = worldBefore;

        // 親がある場合は、新しい親の逆行列でワールド姿勢を親のローカルへ戻す。
        if (parent_) {
            localMatrix = math.Multiply(worldBefore, math.Inverse(parentWorld));
        }

        object3DTransformUtils::ApplyMatrixToTransform(transform_, localMatrix);
    }

    UpdateWorldMatrix();
}

// ========================================================================
// グラフィックス設定 (MeshRendererへの委譲)
// ========================================================================

// モデル参照を借用して描画に渡し、モデルがある場合は当たり判定も合わせる。
void Object3D::SetModel(engine::Model* model) {
    // 描画先が初期化されてからモデルを設定できる。
    if (meshRenderer_) {
        meshRenderer_->SetModel(model);

        // =======================================================
        // モデルと Collider が揃う場合だけ、モデル境界の中心と半分の寸法から判定を作る。
        // =======================================================
        if (model && collider_) {
            engine::ColliderConfig config = collider_->GetConfig();
            engine::Vector3 fullSize = model->GetSize();
            config.size = { fullSize.x / 2.0f, fullSize.y / 2.0f, fullSize.z / 2.0f };

            config.center = model->GetCenter();
            collider_->SetConfig(config);
        }

        RefreshModelSurfaceCollision();
    }
}

// 素材名でモデルを読み、描画とモデル由来の衝突を更新する。空の名前は解除に使う。
void Object3D::SetModel(const std::string& modelName) {
    // 空の名前は素材検索へ渡さず、既存のモデル参照を解除する。
    if (modelName.empty()) {
        SetModel(nullptr);
        return;
    }

    // 有効な素材名と描画先があれば、モデル管理を通して描画と衝突を設定する。
    if (meshRenderer_ && !modelName.empty()) {
        engine::Model* model = ModelManager::GetInstance()->LoadModel(modelName);
        meshRenderer_->SetModel(modelName);

        // =======================================================
        // モデル読込に成功し Collider がある場合は、実寸の半分を判定サイズとして採用する。
        // =======================================================
        if (model && collider_) {
            engine::ColliderConfig config = collider_->GetConfig();
            engine::Vector3 fullSize = model->GetSize();
            config.size = { fullSize.x / 2.0f, fullSize.y / 2.0f, fullSize.z / 2.0f };

            config.center = model->GetCenter();
            collider_->SetConfig(config);
        }

        RefreshModelSurfaceCollision();
    }
}

void Object3D::DrawSpecialMaterial(uint32_t depthSrvHandle, uint32_t grabSrvHandle) {
    // シーンや弾の管理者は材質番号を知らず、描画コンポーネントに選択を委ねる。
    // 現在のビューで可視であり、レンダラーがあるオブジェクトだけに深度と背景色を渡す。
    if (GetIsRenderVisible() && meshRenderer_) {
        meshRenderer_->DrawSpecialMaterial(depthSrvHandle, grabSrvHandle);
    }
}

void Object3D::DrawWater(uint32_t depthSrvHandle, uint32_t grabSrvHandle) {
    // 可視メッシュだけを深度・背景参照付きの水描画パスへ送る。
    if (GetIsRenderVisible() && meshRenderer_) {
        meshRenderer_->DrawWater(depthSrvHandle, grabSrvHandle);
    }
}

void Object3D::DrawMagma(uint32_t depthSrvHandle, uint32_t colorSrvHandle) {
    // マグマ用パスでは深度と色バッファをレンダラーへ渡す。
    // 現在のビューで可視で、描画するレンダラーがある場合だけ描画する。
    if (GetIsRenderVisible() && meshRenderer_) {
        meshRenderer_->DrawMagma(depthSrvHandle, colorSrvHandle);
    }
}

void Object3D::DrawIce(uint32_t depthSrvHandle, uint32_t colorSrvHandle) {
    // 氷材質は画面色を参照する専用パスで可視メッシュだけ描く。
    if (GetIsRenderVisible() && meshRenderer_) {
        meshRenderer_->DrawIce(depthSrvHandle, colorSrvHandle);
    }
}

void Object3D::DrawFire(uint32_t depthSrvHandle, uint32_t grabSrvHandle) {
    // 背景を参照する炎の描画を通常の不透明パスから分離する。
    // 可視で描画先がある場合にだけ、炎のパスへ現在の深度・背景の参照を渡す。
    if (GetIsRenderVisible() && meshRenderer_) {
        meshRenderer_->DrawFire(depthSrvHandle, grabSrvHandle);
    }
}

void Object3D::DrawLaser(uint32_t depthSrvHandle, uint32_t grabSrvHandle) {
    // レーザー材質は深度と背景参照を使う専用パスへ送る。
    // 可視でレンダラーがある場合に、レーザーの描画を依頼する。
    if (GetIsRenderVisible() && meshRenderer_) {
        meshRenderer_->DrawLaser(depthSrvHandle, grabSrvHandle);
    }
}

void Object3D::DrawShockwave(uint32_t depthSrvHandle, uint32_t grabSrvHandle) {
    // 衝撃波の背景歪みを扱う描画パスへ渡す。
    // ビューで非表示かレンダラー未生成なら、背景の歪みを描かない。
    if (GetIsRenderVisible() && meshRenderer_) {
        meshRenderer_->DrawShockwave(depthSrvHandle, grabSrvHandle);
    }
}

void Object3D::DrawStunBind(uint32_t depthSrvHandle, uint32_t grabSrvHandle) {
    // 拘束演出が背景を参照できる専用パスへ送る。
    // 可視のレンダラーを持つ拘束演出だけ描画へ進める。
    if (GetIsRenderVisible() && meshRenderer_) {
        meshRenderer_->DrawStunBind(depthSrvHandle, grabSrvHandle);
    }
}

void Object3D::DrawCrownUnlock(uint32_t depthSrvHandle, uint32_t grabSrvHandle) {
    // 王冠解放の特殊材質を通常のメッシュ描画から分ける。
    // 可視でレンダラーがある場合だけ、王冠解放の特殊パスを実行する。
    if (GetIsRenderVisible() && meshRenderer_) {
        meshRenderer_->DrawCrownUnlock(depthSrvHandle, grabSrvHandle);
    }
}

void Object3D::DrawGatePortal(uint32_t depthSrvHandle, uint32_t grabSrvHandle) {
    // ゲートのポータル材質を深度・背景参照付きで描く。
    // 可視のレンダラーへ、ポータルが読む深度と背景 SRV を渡す。
    if (GetIsRenderVisible() && meshRenderer_) {
        meshRenderer_->DrawGatePortal(depthSrvHandle, grabSrvHandle);
    }
}

void Object3D::DrawSpecialMaterialForCamera(engine::Camera* camera, uint32_t depthSrvHandle, uint32_t grabSrvHandle, std::int32_t previewBufferIndex) {
    // プレビューでは材質番号と専用カメラを渡し、本番と同じ特殊材質パスを使う。
    // 非表示かレンダラー未生成なら、プレビューで特殊材質を描けない。
    if (!GetIsRenderVisible() || !meshRenderer_) {
        return;
    }

    meshRenderer_->DrawSpecialMaterialForCamera(
        GetMaterialType(),
        camera,
        depthSrvHandle,
        grabSrvHandle,
        previewBufferIndex);
}

void Object3D::DrawWindOrb(uint32_t depthSrvHandle, uint32_t grabSrvHandle) {
    // 風球の背景歪みを専用パスで描く。
    // 可視かつレンダラーが利用できる風球だけを描く。
    if (GetIsRenderVisible() && meshRenderer_) {
        meshRenderer_->DrawWindOrb(depthSrvHandle, grabSrvHandle);
    }
}

engine::Model* Object3D::GetModel() const {
    // メッシュレンダラーが無い場合はモデルも存在しない。
    return meshRenderer_ ? meshRenderer_->GetModel() : nullptr;
}

std::string Object3D::GetModelName() const {
    // モデル未設定なら空文字列を返し、管理器への参照を増やさない。
    return meshRenderer_ ? meshRenderer_->GetModelName() : "";
}

void Object3D::SetMeshDrawIndex(std::int32_t meshIndex) {
    // 複数メッシュの表示対象をレンダラー側で切り替える。
    if (meshRenderer_) {
        meshRenderer_->SetMeshDrawIndex(meshIndex);
    }
}

std::int32_t Object3D::GetMeshDrawIndex() const {
    // レンダラーが無いときは-1を未選択として返す。
    return meshRenderer_ ? meshRenderer_->GetMeshDrawIndex() : -1;
}

bool Object3D::IsMeshDrawFiltered() const {
    // メッシュ単位の表示制限はレンダラーがある場合だけ有効にする。
    return meshRenderer_ ? meshRenderer_->IsMeshDrawFiltered() : false;
}

engine::Vector4 Object3D::GetColor() const {
    // 材質が無い場合は不透明な白を既定色として返す。
    return meshRenderer_ ? meshRenderer_->GetColor() : engine::Vector4{ 1,1,1,1 };
}

void Object3D::SetColor(const engine::Vector4& color) {
    // 表示色は実際の材質を持つレンダラーへだけ反映する。
    if (meshRenderer_)
        meshRenderer_->SetColor(color);
}

void Object3D::SetBlendMode(BlendMode blendMode) {
    // 透過の描画方式をレンダラーの材質設定へ渡す。
    if (meshRenderer_)
        meshRenderer_->SetBlendMode(blendMode);
}

BlendMode Object3D::GetBlendMode() const {
    // 材質が無い場合はブレンドなしとして返す。
    return meshRenderer_ ? meshRenderer_->GetBlendMode() : BlendMode::kNone;
}

void Object3D::SetIntensity(float intensity) {
    // オブジェクトごとの照明強度をレンダラーへ反映する。
    if (meshRenderer_)
        meshRenderer_->SetIntensity(intensity);
}

float Object3D::GetIntensity() const {
    // 光源データが未設定なら中立の強度1を使う。
    if (meshRenderer_ && meshRenderer_->GetLightData()) {
        return meshRenderer_->GetLightData()->intensity;
    }

    return 1.0f;
}

// 古いアクセッサの互換性維持
Object3D::DirectionalLight* Object3D::GetDirectionalLightData() {
    // 旧APIとの互換用にレンダラー所有の光源データを公開する。
    return meshRenderer_ ? meshRenderer_->GetLightData() : nullptr;
}

const Object3D::Material* Object3D::GetMaterialData() const {
    // 旧APIからはレンダラー所有の材質データを参照させる。
    return meshRenderer_ ? meshRenderer_->GetMaterialData() : nullptr;
}

// 描画材質の状態を取得し、描画先が無いときは既定の状態を返す。
Object3D::MaterialState Object3D::CaptureMaterialState() const {
    return meshRenderer_ ? meshRenderer_->CaptureMaterialState() : MaterialState{};
}

void Object3D::RestoreMaterialState(const MaterialState& state) {
    // レンダラーが存在する場合に、退避した描画材質の値を戻す。
    if (meshRenderer_) meshRenderer_->RestoreMaterialState(state);
}

void Object3D::SetPortalClip(const PortalClipSettings& settings) {
    // 描画先がある場合に、切り抜き面と縁の設定を材質へ渡す。
    if (meshRenderer_) meshRenderer_->SetPortalClip(settings);
}

void Object3D::ClearPortalClip() {
    // レンダラーを持つ場合だけ、ポータルの切り抜きを解除する。
    if (meshRenderer_) meshRenderer_->ClearPortalClip();
}

void Object3D::SetMaterialTime(float time) {
    // 描画材質の状態がある場合に、材質時計へ秒数を設定する。
    if (meshRenderer_) meshRenderer_->SetMaterialTime(time);
}

void Object3D::SetMaterialType(int32_t type) {
    // 特殊材質の種類を描画側の設定へ渡す。
    // レンダラーがある場合だけ、新しい材質タイプを適用する。
    if (meshRenderer_) {
        meshRenderer_->SetMaterialType(type);
        // 従来の材質番号を有効にする場合は、同じObjectに追加シェーダー材質が残らないようIDを解除します。
        if (type != 0)
            meshRenderer_->SetShaderMaterialId({});
    }
}

// 通常パスへ追加するシェーダーIDなので、旧番号の特殊材質を使っている間は受け付けない。
void Object3D::SetShaderMaterialId(const std::string& id) {
    // 描画先があり、通常材質の番号0を使う場合だけ、追加シェーダーの参照を変更できる。
    if (meshRenderer_ && GetMaterialType() == 0)
        meshRenderer_->SetShaderMaterialId(id);
}

std::string Object3D::GetShaderMaterialId() const {
    return meshRenderer_ ? meshRenderer_->GetShaderMaterialId() : std::string{};
}

void Object3D::SetShininess(float shininess) {
    // 材質が存在するときだけ旧来の光沢値を書き換える。
    if (meshRenderer_) meshRenderer_->SetShininess(shininess);
}

int32_t Object3D::GetMaterialType() const {
    // 材質未設定なら標準材質の番号0を返す。
    return meshRenderer_ ? meshRenderer_->GetMaterialType() : 0;
}

void Object3D::SetSelectedLighting(int32_t type) {
    // 材質が存在するときだけライティングの選択値を更新する。
    if (meshRenderer_) meshRenderer_->SetSelectedLighting(type);
}


} // namespace engine
