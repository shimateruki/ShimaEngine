#define NOMINMAX
#include "Character.h"
#include "Easing.h"
#include "CollisionConfig.h"
#include "Math.h"
#include <algorithm>
#include <cmath>
#include <typeinfo>
#include "GhostRecorder.h"

// 派生キャラクターが決めた速度へ重力と外力を加え、最後に Motor で衝突込みの移動を行います。
void Character::Update(float deltaTime) {
    engine::Object3D::Update(deltaTime);

    // 物理用の設定がなければ共通オブジェクトの更新だけを行います。
    if (!GetParameters().has_value()) {
        return;
    }

    // HPが0以下に達したときに死亡状態を設定する。
    if (GetParameters()->hp <= 0) {
        SetDead(true);
    }

    // 直接移動がモーターを停止している間は、重力と押し戻しで演出軌道を変えないようここで終える。
    if (motorSuspended_) {
        // 直接移動の終点へ重力を重ねると、着地や突進の補間軌道がずれる。
        return;
    }

    SyncCharacterMotorSettings();
    characterMotor_.BeginFrame(isGrounded_);
    characterMotor_.ApplyGravity(velocity_, GetParameters()->gravity, GetParameters()->maxFallSpeed, deltaTime);

    // 水平の外力は残り時間に応じて弱め、鉛直速度の上書きは付与後の最初の更新だけにします。
    if (externalImpulseTimer_ > 0.0f) {
        const float duration = (std::max)(externalImpulseDuration_, 0.001f);
        const float remaining = std::clamp(externalImpulseTimer_ / duration, 0.0f, 1.0f);
        const float weight = Easing::SmoothStep(remaining);
        velocity_.x = engine::Math::Lerp(velocity_.x, externalImpulseVelocity_.x, weight);
        velocity_.z = engine::Math::Lerp(velocity_.z, externalImpulseVelocity_.z, weight);

        // 鉛直の外力は予約された最初の更新でだけ、現在の上向き速度を下げない形で適用する。
        if (externalImpulseVerticalPending_) {
            velocity_.y = (std::max)(velocity_.y, externalImpulseVelocity_.y);
            externalImpulseVerticalPending_ = false;
        }

        externalImpulseTimer_ = (std::max)(0.0f, externalImpulseTimer_ - deltaTime);
    }

    // 確定した速度からモーターで移動位置を求め、接触補正も含めて本体へ反映します。
    engine::Vector3 position = GetTranslate();
    characterMotor_.Move(*this, position, velocity_, isGrounded_, deltaTime);
    SetTranslate(position);
}

void Character::SyncCharacterMotorSettings() {
    // 物理設定が存在する場合だけ、その値を毎フレームモーターに同期する。
    if (!GetParameters().has_value()) {
        return;
    }

    CharacterMotorSettings settings;
    settings.continuousCollision = GetParameters()->motorContinuousCollision;
    settings.snapToGround = GetParameters()->motorSnapToGround;
    settings.maxSlopeDegrees = GetParameters()->motorMaxSlopeDegrees;
    settings.stepHeight = GetParameters()->motorStepHeight;
    settings.groundProbeDistance = GetParameters()->motorGroundProbeDistance;
    settings.skinWidth = GetParameters()->motorSkinWidth;
    settings.collisionMask = engine::collisionCategories::Get("ground");
    characterMotor_.SetSettings(settings);
}

void Character::ApplyExternalImpulse(const engine::Vector3& velocity, float duration) {
    // 連続被弾では弱い水平外力で強いノックバックを上書きせず、上方向だけ維持できる。
    const float safeDuration = (std::max)(0.01f, duration);

    // 有効な外力がまだ無ければ、一定時間だけ効く外力を新規登録する。
    if (externalImpulseTimer_ <= 0.0f) {
        externalImpulseVelocity_ = velocity;
        externalImpulseDuration_ = safeDuration;
        externalImpulseTimer_ = safeDuration;
        externalImpulseVerticalPending_ = true;
        return;
    }

    // 既存と新規の水平速度を二乗で比べ、弱い追加衝撃で軌道が急に変わらないようにします。
    const float currentPlanarSq = externalImpulseVelocity_.x * externalImpulseVelocity_.x +
                                  externalImpulseVelocity_.z * externalImpulseVelocity_.z;
    const float nextPlanarSq = velocity.x * velocity.x + velocity.z * velocity.z;

    // 次の水平速度の二乗が現在の72%以上なら、外力を新しい値へ差し替える。
    if (nextPlanarSq >= currentPlanarSq * 0.72f) {
        externalImpulseVelocity_ = velocity;
    // 弱い衝撃では水平の外力を維持し、上向き成分だけ強い方を残す。
    } else {
        externalImpulseVelocity_.y = (std::max)(externalImpulseVelocity_.y, velocity.y);
    }

    // 連続被弾で効果時間が短くならないようにし、鉛直成分の再適用を予約します。
    externalImpulseDuration_ = (std::max)(externalImpulseDuration_, safeDuration);
    externalImpulseTimer_ = (std::max)(externalImpulseTimer_, safeDuration);
    externalImpulseVerticalPending_ = true;
}

void Character::Draw(ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource) {
    // キャラクター固有の描画はここで加えず、共通のライト付き 3D 描画経路を使う。
    engine::Object3D::Draw(pointLightResource, spotLightResource);
}

// =================================================================
// 衝突判定・物理挙動
// =================================================================

bool Character::OnCollision(engine::Object3D* other) {
    engine::CollisionInfo info = CheckCollision(other);

    // 実際の接触が無ければ、押し戻しや速度の補正は行わない。
    if (!info.isColliding) {
        return false;
    }

    // 物理的な押し戻し・速度補正を適用
    ApplyPhysicsCollision(info, other->GetCollisionAttribute());

    return info.isColliding;
}

void Character::ApplyPhysicsCollision(const engine::CollisionInfo& info, uint32_t attribute) {
    // 地面や壁(ソリッド属性)以外は物理的な押し出しを行わない
    if (!(attribute & engine::collisionCategories::Get("ground"))) {
        return;
    }

    SyncCharacterMotorSettings();
    engine::Vector3 position = GetTranslate();
    characterMotor_.ResolveCollision(position, velocity_, isGrounded_, info, attribute);
    SetTranslate(position);
}

// 編集用の複製ではステータスと移動設定を引き継ぎ、経路レコーダーは新しい実体として初期化します。
std::unique_ptr<engine::Object3D> Character::Clone() const {
    // 派生クラスのコピーは派生側に任せ、Character の実体以外はこの関数でコピーしません。
    if (typeid(*this) != typeid(Character)) {
        return nullptr;
    }

    auto newObj = std::make_unique<Character>();

    assert(GetObjectCommon() != nullptr);
    newObj->Initialize(GetObjectCommon());

    // 表示と Transform を元の配置から複製します。
    // 元にモデル名がある場合は、複製にも同じモデルを割り当てる。
    if (!GetModelName().empty()) {
        newObj->SetModel(GetModelName());
    }

    newObj->CopyTransformStateFrom(*this);
    newObj->SetName(GetName());
    newObj->SetClassName(GetClassName());
    newObj->SetEnemyType(GetEnemyType());

    // 衝突属性は複製後も同じ接触相手を持つように引き継ぎます。
    newObj->SetColliderConfig(GetColliderConfig());
    newObj->SetCollisionAttribute(GetCollisionAttribute());
    newObj->SetCollisionMask(GetCollisionMask());

    // ステータスと現在の移動状態を引き継ぎます。
    newObj->SetEventType(GetEventType());

    // 元にステータスがある場合だけ、個体の設定を複製に引き継ぐ。
    if (GetParameters().has_value()) {
        newObj->SetParameters(GetParameters().value());
    }

    newObj->velocity_ = velocity_;
    newObj->isGrounded_ = isGrounded_;

    newObj->characterMotor_ = characterMotor_;

    // アニメーションの選択とループ設定を引き継ぎます。
    newObj->SetAnimationName(GetAnimationName());
    newObj->SetAnimationLoop(IsAnimationLoop());

    // 経路設定を複製し、再生器は複製先専用に作り直します。
    // 経路移動コンポーネントがある場合だけ、再生モードや位相を複製する。
    if (HasPathMoverComponent()) {
        newObj->EnsurePathMoverComponent();
        newObj->SetRecordPathName(GetRecordPathName());
        newObj->SetRecordLoop(IsRecordLoop());
        newObj->SetRecordRelative(IsRecordRelative());
        newObj->GetPathMoverComponent()->SetPlaybackMode(GetPathMoverComponent()->GetPlaybackMode());
        newObj->GetPathMoverComponent()->SetPhaseOffset(GetPathMoverComponent()->GetPhaseOffset());
    }

    newObj->InitializeRecorder(nullptr);

    // 経路名と複製専用のレコーダーの両方がそろった場合だけ、自動再生を始める。
    if (!newObj->GetRecordPathName().empty() && newObj->GetRecorder()) {
        bool isCinematic = newObj->IsCameraObject();
        newObj->GetRecorder()->Play(newObj->GetRecordPathName(), newObj->IsRecordLoop(), newObj->IsRecordRelative(),
                                isCinematic);
    }

    return newObj;
}

// 物理設定が無ければ既定値を用意してから、重力だけを更新する。
void Character::SetGravity(float gravity) {
    // 既定値を作るのは、現在のパラメータが未作成の場合だけ。
    if (!this->GetParameters().has_value()) {
        this->ResetParameters();
    }
    this->SetParameter(&engine::EntityParameter::gravity, gravity);
}

void Character::SetMaxFallSpeed(float maxFallSpeed) {
    // 物理設定をまだ持たない場合に限り、既定値を用意してから最大落下速度を変える。
    if (!this->GetParameters().has_value()) {
        this->ResetParameters();
    }
    this->SetParameter(&engine::EntityParameter::maxFallSpeed, maxFallSpeed);
}

void Character::SimulateMotorStep(engine::Vector3& position, float gravity, float maxFallSpeed, float deltaTime) {
    // 衝突コールバックからも同じ速度・接地状態を参照できるよう、保持値を直接渡す。
    characterMotor_.BeginFrame(isGrounded_);
    characterMotor_.ApplyGravity(velocity_, gravity, maxFallSpeed, deltaTime);
    characterMotor_.Move(*this, position, velocity_, isGrounded_, deltaTime);
}
