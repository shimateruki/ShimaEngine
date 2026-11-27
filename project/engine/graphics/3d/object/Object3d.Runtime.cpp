#define NOMINMAX
#include <cstdint>
#include "Object3D.h"
#include "EditorPropertyRegistry.h"
#include "Object3DTransformUtils.h"
#include "engine/graphics/3d/material/MaterialInstance.h"
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
#include <array>
#include <cctype>
#include <iomanip>
#include <random>
#include <sstream>
#include <typeinfo>

namespace engine {


// ========================================================================
// Object3D 実行時補助処理
// ------------------------------------------------------------------------
// GhostRecorder連携、Clone、Shadow/LocalFog描画、CopyFromを担当する。
// ========================================================================
void Object3D::InitializeRecorder(SceneManager* sceneManager) {
    // 古い記録器の接続を先に解放し、このオブジェクトを新しい再生対象へ結び直す。
    recorder_.reset();
    recorder_ = std::make_unique<GhostRecorder>();
    recorder_->Initialize(sceneManager);
    recorder_->SetTarget(this);
}


std::unique_ptr<Object3D> Object3D::Clone() const {
    // 派生型の切り捨てを避けるため、基底型そのものだけをここで複製する。
    if (typeid(*this) != typeid(Object3D)) {
        return nullptr;
    }

    auto newObj = std::make_unique<Object3D>();
    assert(common_ != nullptr);
    newObj->Initialize(common_);
    newObj->CopyFrom(this);
    return newObj;
}

// 保存用の識別子として、乱数にバージョンとバリアントのビットを設定した UUID を返す。
std::string Object3D::GeneratePersistentGuid() {
    std::array<unsigned char, 16> bytes{};
    std::random_device random;

    for (unsigned char& value : bytes) {
        value = static_cast<unsigned char>(random());
    }

    // UUID version 4 / RFC 4122 variantのビット配置に合わせます。
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0Fu) | 0x40u);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3Fu) | 0x80u);

    std::ostringstream stream;
    stream << std::hex << std::setfill('0');

    for (std::size_t index = 0; index < bytes.size(); ++index) {
        // 16バイトを2桁ずつ書き、8-4-4-4-12文字の区切りで保存用の表記に整えます。
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            stream << '-';
        }

        stream << std::setw(2) << static_cast<std::uint32_t>(bytes[index]);
    }

    return stream.str();
}

bool Object3D::IsPersistentGuidValid(std::string_view guid) {
    // 読込データの識別子は区切り位置と16進文字を確認し、不正なら採用しない。
    if (guid.size() != 36) {
        return false;
    }

    for (std::size_t index = 0; index < guid.size(); ++index) {
        const bool isSeparator = index == 8 || index == 13 || index == 18 || index == 23;

        // 区切りを置く4位置では、16進数としてではなくハイフンとして検証する。
        if (isSeparator) {
            // 区切りが欠落していれば、36文字でも別形式として拒否する。
            if (guid[index] != '-')
                return false;
            continue;
        }

        // 区切り以外に16進文字でない文字を見つけた時点で拒否する。
        if (std::isxdigit(static_cast<unsigned char>(guid[index])) == 0) {
            return false;
        }
    }

    return true;
}

const std::string& Object3D::EnsurePersistentGuid() {
    // 保存時に識別子が欠けていれば新規発行し、以後は同じ値を返す。
    if (!IsPersistentGuidValid(persistentGuid_)) {
        persistentGuid_ = GeneratePersistentGuid();
    }

    return persistentGuid_;
}

bool Object3D::SetPersistentGuid(const std::string& guid) {
    // 不正な識別子で既存のシーン参照を壊さないよう、検証後にだけ更新する。
    if (!IsPersistentGuidValid(guid)) {
        return false;
    }

    persistentGuid_ = guid;
    return true;
}

void Object3D::RegeneratePersistentGuid() {
    // 複製や別物としての登録で元オブジェクトと識別子を共有しないようにする。
    persistentGuid_ = GeneratePersistentGuid();
}

ParticleEmitterComponent* Object3D::EnsureParticleEmitterComponent() {
    // 編集で初めて粒子を設定したときだけComponentを作り、存在情報も保存対象にする。
    if (!particleEmitterComponent_) {
        particleEmitterComponent_.emplace();
    }

    SetComponentPresenceMarker(std::string(kParticleEmitterComponentType), true);
    return &*particleEmitterComponent_;
}

bool Object3D::RemoveParticleEmitterComponent() {
    // 設定と実行中のGPU放出器を共に消し、保存用の存在情報も解除する。
    if (!particleEmitterComponent_) {
        return false;
    }

    particleEmitterComponent_.reset();
    gpuEmitter_.reset();
    SetComponentPresenceMarker(std::string(kParticleEmitterComponentType), false);
    return true;
}

ParticleEmitterComponent* Object3D::GetParticleEmitterComponent() {
    // 未追加なら作成せずnullptrを返し、参照だけでシーンを変更しない。
    return particleEmitterComponent_ ? &*particleEmitterComponent_ : nullptr;
}

const ParticleEmitterComponent* Object3D::GetParticleEmitterComponent() const {
    // 読み取り側でも未追加Componentは生成しない。
    return particleEmitterComponent_ ? &*particleEmitterComponent_ : nullptr;
}

void Object3D::SetParticleName(const std::string& name) {
    // 空文字で未追加の場合はComponentを増やさず、実際の設定時だけ生成する。
    if (!particleEmitterComponent_ && name.empty())
        return;
    EnsureParticleEmitterComponent()->SetCpuParticle(name);
}

const std::string& Object3D::GetParticleName() const {
    // 未追加時には参照寿命が安定した空文字列を返す。
    static const std::string kEmpty;
    return particleEmitterComponent_ ? particleEmitterComponent_->GetCpuParticle() : kEmpty;
}

void Object3D::SetGpuParticleName(const std::string& name) {
    // GPU 名が空で未追加の場合は作らない。追加済みなら空を指定して現在のプリセットを解除できる。
    if (!particleEmitterComponent_ && name.empty())
        return;
    EnsureParticleEmitterComponent()->SetGpuParticle(name);
}

const std::string& Object3D::GetGpuParticleName() const {
    // GPU設定が無い場合も空文字列の参照を安全に返す。
    static const std::string kEmpty;
    return particleEmitterComponent_ ? particleEmitterComponent_->GetGpuParticle() : kEmpty;
}

MeshEffectComponent* Object3D::EnsureMeshEffectComponent() {
    // メッシュ効果を初めて設定するときだけComponentを作り、保存上の存在を記録する。
    if (!meshEffectComponent_) {
        meshEffectComponent_.emplace();
    }

    SetComponentPresenceMarker(std::string(kMeshEffectComponentType), true);
    return &*meshEffectComponent_;
}

bool Object3D::RemoveMeshEffectComponent() {
    // 設定だけでなく再生中の効果参照も消し、削除後に描画が残らないようにする。
    if (!meshEffectComponent_) {
        return false;
    }

    meshEffectComponent_.reset();
    currentMeshEffect1_.clear();
    currentMeshEffect2_.clear();
    attachedEffects1_.clear();
    attachedEffects2_.clear();
    SetComponentPresenceMarker(std::string(kMeshEffectComponentType), false);
    return true;
}

MeshEffectComponent* Object3D::GetMeshEffectComponent() {
    // 編集画面からの参照ではComponentを勝手に追加しない。
    return meshEffectComponent_ ? &*meshEffectComponent_ : nullptr;
}

const MeshEffectComponent* Object3D::GetMeshEffectComponent() const {
    // 未設定ならnullptrを返し、読み取り操作を副作用なしに保つ。
    return meshEffectComponent_ ? &*meshEffectComponent_ : nullptr;
}

void Object3D::SetMeshEffect1Name(const std::string& name) {
    // 主効果を指定するときだけComponentを確保し、空設定では余分な項目を増やさない。
    if (!meshEffectComponent_ && name.empty())
        return;
    EnsureMeshEffectComponent()->SetPrimaryEffect(name);
}

const std::string& Object3D::GetMeshEffect1Name() const {
    // 主効果が無い場合は静的な空文字列を返し、参照先の寿命を保つ。
    static const std::string kEmpty;
    return meshEffectComponent_ ? meshEffectComponent_->GetPrimaryEffect() : kEmpty;
}

void Object3D::SetMeshEffect2Name(const std::string& name) {
    // 副効果名が空で未追加の場合は作らない。既存の枠に対する空の指定は解除として反映する。
    if (!meshEffectComponent_ && name.empty())
        return;
    EnsureMeshEffectComponent()->SetSecondaryEffect(name);
}

const std::string& Object3D::GetMeshEffect2Name() const {
    // 副効果が未設定なら寿命の安定した空文字列を返す。
    static const std::string kEmpty;
    return meshEffectComponent_ ? meshEffectComponent_->GetSecondaryEffect() : kEmpty;
}

PathMoverComponent* Object3D::EnsurePathMoverComponent() {
    // 移動経路を設定するときだけComponentを作り、シーン保存へ反映する。
    if (!pathMoverComponent_) {
        pathMoverComponent_.emplace();
    }

    SetComponentPresenceMarker(std::string(kPathMoverComponentType), true);
    return &*pathMoverComponent_;
}

bool Object3D::RemovePathMoverComponent() {
    // 経路設定を消す際はGhostRecorderの再生も止め、古い動きを残さない。
    if (!pathMoverComponent_) {
        return false;
    }

    pathMoverComponent_.reset();

    // 記録器がある場合は、Component を外した後に以前の経路を停止する。
    if (recorder_) {
        recorder_->Stop();
    }

    SetComponentPresenceMarker(std::string(kPathMoverComponentType), false);
    return true;
}

PathMoverComponent* Object3D::GetPathMoverComponent() {
    // 参照だけでは経路Componentを作らず、未設定をnullptrで知らせる。
    return pathMoverComponent_ ? &*pathMoverComponent_ : nullptr;
}

const PathMoverComponent* Object3D::GetPathMoverComponent() const {
    // 読み取り時の未設定はnullptrとして扱う。
    return pathMoverComponent_ ? &*pathMoverComponent_ : nullptr;
}

void Object3D::SetRecordPathName(const std::string& name) {
    // 未追加の物へ空の記録名を渡しても作成しない。既存の設定では記録名を解除できる。
    if (!pathMoverComponent_ && name.empty())
        return;
    EnsurePathMoverComponent()->SetPathName(name);
}

const std::string& Object3D::GetRecordPathName() const {
    // 経路Componentが無い場合でも安全な空文字列参照を返す。
    static const std::string kEmpty;
    return pathMoverComponent_ ? pathMoverComponent_->GetPathName() : kEmpty;
}

void Object3D::SetRecordLoop(bool loop) {
    // 未追加のオブジェクトへfalseを設定しても、空の経路Componentは作らない。
    if (!pathMoverComponent_ && !loop)
        return;
    EnsurePathMoverComponent()->SetLoop(loop);
}

bool Object3D::IsRecordLoop() const {
    // 経路そのものが無い場合はループ設定も無効として返す。
    return pathMoverComponent_ && pathMoverComponent_->IsLoop();
}

void Object3D::SetRecordRelative(bool relative) {
    // 未追加状態のfalseは省き、相対経路を使うときだけComponentを作る。
    if (!pathMoverComponent_ && !relative)
        return;
    EnsurePathMoverComponent()->SetRelative(relative);
}

bool Object3D::IsRecordRelative() const {
    // 経路Componentが無い場合は絶対位置扱いにする。
    return pathMoverComponent_ && pathMoverComponent_->IsRelative();
}

GameplayLinkComponent* Object3D::EnsureGameplayLinkComponent() {
    // イベント接続を設定するときだけComponentを作り、シーン保存に存在を示す。
    if (!gameplayLinkComponent_) {
        gameplayLinkComponent_.emplace();
    }

    SetComponentPresenceMarker(std::string(kGameplayLinkComponentType), true);
    return &*gameplayLinkComponent_;
}

bool Object3D::RemoveGameplayLinkComponent() {
    // 接続情報をまとめて解除し、保存上のComponent存在も外す。
    if (!gameplayLinkComponent_) {
        return false;
    }

    gameplayLinkComponent_.reset();
    SetComponentPresenceMarker(std::string(kGameplayLinkComponentType), false);
    return true;
}

GameplayLinkComponent* Object3D::GetGameplayLinkComponent() {
    // 参照だけでは接続Componentを作らない。
    return gameplayLinkComponent_ ? &*gameplayLinkComponent_ : nullptr;
}

const GameplayLinkComponent* Object3D::GetGameplayLinkComponent() const {
    // 接続先が未設定ならnullptrを返す。
    return gameplayLinkComponent_ ? &*gameplayLinkComponent_ : nullptr;
}

void Object3D::ApplyWorldMatrix(const engine::Matrix4x4& worldMatrix) {
    // 入力行列からローカル姿勢を組み立てます。親付きでワールド姿勢を維持するには、親の逆変換を呼び出し側で適用します。
    object3DTransformUtils::ApplyMatrixToTransform(transform_, worldMatrix);
    UpdateLocalMatrix();
    UpdateWorldMatrix();
}

NavAgentComponent* Object3D::EnsureNavAgentComponent() {
    // 経路探索の設定が必要になった時点でComponentを作る。
    if (!navAgentComponent_) {
        navAgentComponent_.emplace();
    }

    SetComponentPresenceMarker(std::string(kNavAgentComponentType), true);
    return &*navAgentComponent_;
}

bool Object3D::RemoveNavAgentComponent() {
    // 経路探索設定を消し、シーン保存上の存在情報も解除する。
    if (!navAgentComponent_)
        return false;
    navAgentComponent_.reset();
    SetComponentPresenceMarker(std::string(kNavAgentComponentType), false);
    return true;
}

NavAgentComponent* Object3D::GetNavAgentComponent() {
    // 未追加の経路探索Componentは生成せずnullptrを返す。
    return navAgentComponent_ ? &*navAgentComponent_ : nullptr;
}

const NavAgentComponent* Object3D::GetNavAgentComponent() const {
    // 読み取り操作では経路探索設定を実体化しない。
    return navAgentComponent_ ? &*navAgentComponent_ : nullptr;
}

void Object3D::SetEventId(std::int32_t id) {
    // 無効IDだけのオブジェクトには空の接続Componentを作らない。
    if (!gameplayLinkComponent_ && id < 0)
        return;
    EnsureGameplayLinkComponent()->SetEventId(id);
}

std::int32_t Object3D::GetEventId() const {
    // 接続Componentが無ければ-1を未設定IDとして返す。
    return gameplayLinkComponent_ ? gameplayLinkComponent_->GetEventId() : -1;
}

void Object3D::SetTargetId(std::int32_t id) {
    // 未追加かつ負の ID なら接続設定を作らない。設定済みの物には解除用の値も反映する。
    if (!gameplayLinkComponent_ && id < 0)
        return;
    EnsureGameplayLinkComponent()->SetTargetId(id);
}

std::int32_t Object3D::GetTargetId() const {
    // 接続先が無い場合は-1を返し、通常のIDと区別する。
    return gameplayLinkComponent_ ? gameplayLinkComponent_->GetTargetId() : -1;
}

std::vector<Object3D::BuiltInComponentInfo> Object3D::GetBuiltInComponentInfos() const {
    // 編集画面用に必須Componentと着脱可能Componentの存在を一覧化する。
    return {
        { kTransformComponentType, "Transform", true, false },
        { kMeshRendererComponentType, "Mesh Renderer", meshRenderer_ != nullptr, false },
        { kColliderComponentType, "Collider", collider_ != nullptr, false },
        { kParticleEmitterComponentType, "Particle Emitter", particleEmitterComponent_.has_value(), true },
        { kMeshEffectComponentType, "Mesh Effect", meshEffectComponent_.has_value(), true },
        { kPathMoverComponentType, "Path Mover", pathMoverComponent_.has_value(), true },
        { kGameplayLinkComponentType, "Gameplay Link", gameplayLinkComponent_.has_value(), true },
        { kNavAgentComponentType, "Nav Agent", navAgentComponent_.has_value(), true },
    };
}

bool Object3D::HasBuiltInComponent(std::string_view typeId) const {
    // 実体を探す経路と存在判定を共有し、表示だけの空項目を数えない。
    return FindBuiltInComponent(typeId) != nullptr;
}

void* Object3D::FindBuiltInComponent(std::string_view typeId) {
    // 検索条件をconst版に集約し、変更可能な呼び出し側へ同じ実体を返す。
    return const_cast<void*>(static_cast<const Object3D*>(this)->FindBuiltInComponent(typeId));
}

const void* Object3D::FindBuiltInComponent(std::string_view typeId) const {
    // 編集用の型IDから組み込みComponent実体へ対応付ける。
    // 変形は Value として常に存在するため、直接そのアドレスを返す。
    if (typeId == kTransformComponentType)
        return &transform_;
    // 描画型を要求された場合は、生成済みのレンダラーを返す。
    if (typeId == kMeshRendererComponentType)
        return meshRenderer_.get();
    // 判定型の指定では、現在の Collider を返す。
    if (typeId == kColliderComponentType)
        return collider_.get();
    // 粒子型では、未追加なら nullptr を返す取得経路を使う。
    if (typeId == kParticleEmitterComponentType)
        return GetParticleEmitterComponent();
    // メッシュ効果型では、現在の効果設定を生成せずに返す。
    if (typeId == kMeshEffectComponentType)
        return GetMeshEffectComponent();
    // 移動型では、記録した経路の設定を取得する。
    if (typeId == kPathMoverComponentType)
        return GetPathMoverComponent();
    // 接続型では、送受信 ID を持つ設定の実体を返す。
    if (typeId == kGameplayLinkComponentType)
        return GetGameplayLinkComponent();
    // 探索型では、NavAgent が存在するときにその設定を返す。
    if (typeId == kNavAgentComponentType)
        return GetNavAgentComponent();
    return nullptr;
}


void Object3D::DrawShadow() {
    // 非表示とカメラオブジェクトは影パスへ送らない。
    if (!GetIsRenderVisible() || IsCameraObject())
        return;
    // 描画を許可された Object でも、Renderer があるものだけ影パスへ渡す。
    if (meshRenderer_) {
        meshRenderer_->DrawShadow();
    }
}

void Object3D::SetShadowCommonState() {
    // メッシュがある場合だけ影描画の共通パイプライン状態を設定する。
    if (meshRenderer_) {
        meshRenderer_->SetShadowCommonState();
    }
}

void Object3D::DrawShadowOnly() {
    // 影だけの描画でも可視性とカメラ除外の条件を本体描画と揃える。
    if (!GetIsRenderVisible() || IsCameraObject())
        return;
    // Renderer がある場合は、影のみを描く経路を呼ぶ。
    if (meshRenderer_) {
        meshRenderer_->DrawShadowOnly();
    }
}

void Object3D::DrawLocalFog(uint32_t depthSrvHandle) {
    // 現在のビューで描画対象となり、メッシュの描画先もある場合だけ霧を描く。
    if (GetIsRenderVisible() && meshRenderer_) {
        // 深度を参照する霧の描画はメッシュレンダラーの専用パスへ渡す。
        meshRenderer_->DrawLocalFog(depthSrvHandle);
    }
}

engine::MeshRenderer::LocalFogData* Object3D::GetLocalFogData() {
    // メッシュが無いオブジェクトではローカルフォグ設定も存在しない。
    return meshRenderer_ ? meshRenderer_->GetLocalFogData() : nullptr;
}

// 配置設定を写す。Object階層の付け替えはここでは行わず、再生中の表示物も複製しない。
void Object3D::CopyFrom(const Object3D* other) {
    // コピー元が無ければ、複製先の各設定を初期化し直さない。
    if (!other)
        return;

    // 名前・分類・見た目などのシーン設定を写し、複製先の識別子を保持する。
    this->name_ = other->name_;

    // 複製先にはInitialize時に発行した別GUIDを維持します。
    this->opaqueComponents_ = other->opaqueComponents_;
    this->className_ = other->className_;
    this->sceneCameraSettings_ = other->sceneCameraSettings_;
    this->tag_ = other->tag_;
    this->layer_ = other->layer_;
    this->saveCategory_ = other->saveCategory_;
    this->enemyType_ = other->enemyType_;
    this->gimmickType_ = other->gimmickType_;
    this->itemType_ = other->itemType_;
    this->materialInstancePath_ = other->materialInstancePath_;
    this->decalSettings_ = other->decalSettings_;
    // 寿命の設定は複製し、再生済みの経過時間は引き継がず先頭から始めます。
    this->decalElapsedTime_ = 0.0f;
    this->decalAuthoredAlpha_ = other->decalAuthoredAlpha_;
    this->isVisible_ = other->isVisible_;
    this->isLocked_ = other->isLocked_;
    this->castShadow_ = other->castShadow_;
    this->cameraOcclusionMode_ = other->cameraOcclusionMode_;
    this->prefabInstanceInfo_ = other->prefabInstanceInfo_;

    // コピー元にモデル指定がある場合だけ、各種描画設定に先立って適用する。
    if (!other->GetModelName().empty()) {
        // モデルの設定でコライダーと材質の初期値が変わるため、複製元の調整値を戻すより先に設定します。
        this->SetModel(other->GetModelName());
    }

    // ローカルの変形値と姿勢表現の方式を引き継ぐ。
    this->transform_ = other->transform_;

    // 両方に衝突の実体がある場合だけ、形状・ビット属性・地形データを移す。
    if (collider_ && other->collider_) {
        this->SetColliderConfig(other->GetColliderConfig());
        this->SetCollisionAttribute(other->GetCollisionAttribute());
        this->SetCollisionMask(other->GetCollisionMask());

        // 元が地形の判定データを持っていれば、参照パスと一緒に更新する。
        if (const engine::TerrainCollisionData* terrain = other->GetTerrainCollisionData()) {
            this->SetTerrainCollisionData(*terrain, other->GetTerrainCollisionPath());
        // 地形の設定がなければ、複製先に以前の地形が残らないよう消す。
        } else {
            terrainCollisionPath_.clear();
            collider_->ClearTerrainData();
        }
    }

    // イベントの接続と経路探索の設定を移す。
    this->eventType_ = other->eventType_;
    this->gameplayLinkComponent_ = other->gameplayLinkComponent_;
    this->navAgentComponent_ = other->navAgentComponent_;

    // 敵などの Parameter と登録済みの型固有項目を移す。
    this->param_ = other->param_;
    // 設定値だけを複製し、台帳のGetter/Setterや移動中の状態はコピーしません。
    EditorPropertyRegistry::GetInstance()->ReadClassProperties(*this, other->SerializeFeatureComponents());

    // 両側にレンダラーがあれば、Material Instance の共通変換で材質と描画メッシュの番号を移す。
    if (meshRenderer_ && other->meshRenderer_) {
        MaterialInstanceAsset::Apply(MaterialInstanceAsset::Capture(*other), *this);
        this->SetMeshDrawIndex(other->GetMeshDrawIndex());
    }


    // アニメーションの名前とループ設定を移し、コントローラーは指定の有無に合わせる。
    this->animName_ = other->animName_;
    this->isAnimLoop_ = other->isAnimLoop_;

    // 元のコントローラーにパスがあれば、複製先でもその定義を読み込む。
    if (!other->GetAnimatorControllerPath().empty()) {
        this->SetAnimatorController(other->GetAnimatorControllerPath());
    // 元にパスがない場合は、複製先に以前のコントローラーを残さない。
    } else {
        this->ClearAnimatorController();
    }

    // 実体Componentは存在状態を含めてコピーします。再生中Instanceは複製しません。
    this->particleEmitterComponent_ = other->particleEmitterComponent_;
    this->meshEffectComponent_ = other->meshEffectComponent_;
    this->pathMoverComponent_ = other->pathMoverComponent_;
    this->gpuEmitter_.reset();
    this->currentMeshEffect1_.clear();
    this->currentMeshEffect2_.clear();
    this->attachedEffects1_.clear();
    this->attachedEffects2_.clear();

    this->InitializeRecorder(nullptr);

    if (!this->GetRecordPathName().empty() && this->recorder_) {
        // 経路設定がある場合は、複製元の時計を引き継がず新しいレコーダーで先頭から再生します。
        bool isCinematic = this->IsCameraObject();
        this->recorder_->Play(
            this->GetRecordPathName(),
            this->IsRecordLoop(),
            this->IsRecordRelative(),
            isCinematic);
    }

    auto myFog = this->GetLocalFogData();
    auto otherFog = const_cast<Object3D*>(other)->GetLocalFogData();

    // 光や霧の見た目を維持するため、複製先・元の両方に定数バッファがある場合だけ設定を移す。
    if (myFog && otherFog) {
        *myFog = *otherFog;
    }
}

} // namespace engine
