#include <cstdint>
#include "GimmickChainCollapseFloor.h"
#include "ObjectPropertyDefinition.h"
#include "CollisionConfig.h"
#include "SceneManager.h"
#include "ModelManager.h"
#include "BaseScene.h"
#include "Player.h"
#include "game/system/scene/SceneGameplayAccess.h"
#include <cstdlib>

namespace {
// 落下復帰後にプレイヤーと床が重ならないよう、十分な退避時間を確保する。
constexpr float kReturnDelaySeconds = 5.0f;
} // namespace

// 待機時に乗れる床を設定し、揺れ・落下・連鎖遅延・復帰の項目を登録する。
void GimmickChainCollapseFloor::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    BaseGimmick::Initialize(common, modelName);

    // 踏まれる前は通常の床として衝突し、落下開始時に判定を外します。
    SetClassName("Gimmick");
    SetGimmickType("ChainCollapseFloor");
    SetName("Gimmick_ChainCollapseFloor");
    SetCollisionAttribute(engine::collisionCategories::Get("ground"));
    SetCollisionMask(engine::collisionCategories::GetDefaultGameplayMask());
    SetColor({0.75f, 0.92f, 1.0f, 0.82f});
    SetScale({2.0f, 0.25f, 2.0f});
    SetStatic(false);

    // パラメーターが未作成なら既定の入れ物を用意してから、崩れ床用の初期値を設定する。
    if (!GetParameters().has_value())
        ResetParameters();
    SetParameter(&engine::EntityParameter::shakeDuration, 0.45f);
    SetParameter(&engine::EntityParameter::fallDuration, 1.4f);
    SetParameter(&engine::EntityParameter::interval, 0.18f);
    SetParameter(&engine::EntityParameter::gravity, 48.0f);

    // 従来の一度きりの挙動を既定とし、必要なステージだけ明示的に復帰を有効化する。
    SetParameter(&engine::EntityParameter::returnOnOff, false);

    // Target IDと連鎖先のMy Event IDを結び付け、既存のパラメーターで各段階を調整できるようにする。
    using Self = GimmickChainCollapseFloor;
    engine::ObjectPropertyDefinition<Self> properties(*this, kTypeId, "連鎖崩れ床",
        "Target IDに次の床のMy Event IDを入れると連鎖します。");
    using Parameter = engine::Object3D::EntityParameter;
    properties.Parameter("揺れ時間", "shakeDuration", &Parameter::shakeDuration, 0.0f, 10.0f);
    properties.Parameter("連鎖までの時間", "interval", &Parameter::interval, 0.0f, 10.0f,
        nullptr, {}, {.speed = 0.01f});
    properties.Parameter("落下時間", "fallDuration", &Parameter::fallDuration, 0.1f, 10.0f);
    properties.Parameter("重力", "gravity", &Parameter::gravity, 0.0f, 200.0f);
    properties.Parameter("終了後に復帰を許可", "returnOnOff", &Parameter::returnOnOff);
}

void GimmickChainCollapseFloor::Update(float deltaTime) {
    // 待機・予告・揺れ・落下・消失を状態ごとに進め、再出現もここで管理します。
    // 最初の更新で配置姿勢を記録し、岩モデルなら差し替えに使うモデルを揃える。
    if (!initializedStart_) {
        // 岩の系統なら両方のモデルを先読みし、踏んだ瞬間のディスク読み込みを避ける。
        if (GetModelName().find("Gimmicks/crumbling_rock_block") == 0) {
            ModelManager::GetInstance()->LoadModel("Gimmicks/crumbling_rock_block");
            ModelManager::GetInstance()->LoadModel("Gimmicks/crumbling_rock_block_intact");
            UpdateRockAppearance();
        }
        startPosition_ = GetTransform()->translate;
        startRotation_ = GetTransform()->rotate;
        initializedStart_ = true;
    }

    // ゲームの再生中に待機している床だけ、衝突通知に加えてプレイヤーの足元を調べる。
    if (state_ == State::kIdle && SceneManager::GetInstance()->IsPlaying())
        CheckPlayerContact();

    switch (state_) {
    case State::kIdle:
        // 待機中は配置を基準として記録し、崩れ始めた後の揺れや復帰位置に使う。
        startPosition_ = GetTransform()->translate;
        startRotation_ = GetTransform()->rotate;
        break;

    case State::kPending:
        // 連鎖で通知された床は、順番に設定された待ちが尽きた時点で崩れ始める。
        pendingTimer_ -= deltaTime;

        // 連鎖遅延が尽きたら揺れを開始する。
        if (pendingTimer_ <= 0.0f) {
            pendingTimer_ = 0.0f;
            StartCollapse();
        }

        break;

    case State::kShaking: {
        // 元の位置を中心に水平の揺れを加え、予告が終わるまで落下へ移さない。
        timer_ += deltaTime;

        float intensity = 0.045f;
        float offX = ((float)std::rand() / RAND_MAX * 2.0f - 1.0f) * intensity;
        float offZ = ((float)std::rand() / RAND_MAX * 2.0f - 1.0f) * intensity;
        SetTranslateX(startPosition_.x + offX);
        SetTranslateZ(startPosition_.z + offZ);

        // 揺れの経過が設定時間に達したら、ChangeStateで落下と次の床への通知を始める。
        if (timer_ >= GetShakeDuration()) {
            ChangeState(State::kFalling);
        }

        break;
    }

    case State::kFalling:
        // 重力と傾きを加えて落とし、落下用の時間が尽きたら非表示の状態へ移る。
        timer_ += deltaTime;
        velocityY_ -= GetGravity() * deltaTime;
        SetTranslateY(GetTranslate().y + velocityY_ * deltaTime);
        SetEulerX(GetRotation().x + 1.4f * deltaTime);
        SetEulerZ(GetRotation().z + 0.9f * deltaTime);
        SetQuaternionMaster(false);

        // 設定した落下時間に達したら床を隠し、復帰待ちに入る。
        if (timer_ >= GetFallDuration()) {
            ChangeState(State::kHidden);
        }

        break;

    case State::kHidden:
        // 再出現する設定のときだけ待ち時間を数え、配置と衝突をリセットする。
        if (ShouldReturnAfterCollapse()) {
            returnTimer_ += deltaTime;

            // 消失後に5秒経過したら、元の配置と衝突を復元する。
            if (returnTimer_ >= kReturnDelaySeconds) {
                ResetForRetry();
            }
        }

        break;
    }

    BaseGimmick::Update(deltaTime);
}

bool GimmickChainCollapseFloor::OnCollision(engine::Object3D* other) {
    // 待機中にプレイヤーとの接触が届いたときだけ、実際に触れた方向を確認する。
    if (state_ == State::kIdle && other && other->GetClassName() == "Player") {
        engine::CollisionInfo info = CheckCollision(other);

        // 床から見た法線のY成分が-0.5未満なら、上面への接触として揺れを開始する。
        if (info.isColliding && info.normal.y < -0.5f) {
            StartCollapse();
        }
    }

    return true;
}

void GimmickChainCollapseFloor::OnTrigger() {
    // 前の床からの連鎖イベントは、設定した遅延を経て崩壊させます。
    QueueCollapse();
}

void GimmickChainCollapseFloor::StartCollapse() {
    // 待機または連鎖待ちからだけ揺れへ進める。すでに進行中の崩落をやり直さない。
    if (state_ != State::kIdle && state_ != State::kPending)
        return;
    ChangeState(State::kShaking);
}

void GimmickChainCollapseFloor::QueueCollapse() {
    // 前の床だけ先に復帰して踏まれた場合も、後続床へのイベントを失わない。
    if (state_ == State::kHidden && ShouldReturnAfterCollapse()) {
        ResetForRetry();
    }

    // 待機状態になった床だけ、新たな連鎖の待ち時間を設定できる。
    if (state_ != State::kIdle)
        return;
    pendingTimer_ = GetChainDelay();
    ChangeState(State::kPending);
}

void GimmickChainCollapseFloor::ResetForRetry() {
    // 復帰時は位置・判定・連鎖済みフラグを待機状態へ戻します。
    ChangeState(State::kIdle);
}

void GimmickChainCollapseFloor::ChangeState(State state) {
    // 段階を切り替えて経過時間を初期化し、その段階の見た目・衝突・連鎖処理を適用する。
    state_ = state;
    timer_ = 0.0f;

    switch (state_) {
    // 初期姿勢と衝突を戻し、前回の落下・連鎖・乗員検知の状態を破棄します。
    case State::kIdle:
        SetIsVisible(true);
        SetCollisionAttribute(engine::collisionCategories::Get("ground"));
        SetCollisionMask(engine::collisionCategories::GetDefaultGameplayMask());
        SetTranslate(startPosition_);
        SetEulerAngles(startRotation_);
        SetQuaternionMaster(false);
        velocityY_ = 0.0f;
        pendingTimer_ = 0.0f;
        returnTimer_ = 0.0f;
        triggeredNext_ = false;
        hasPreviousPlayerFoot_ = false;
        SetColor({0.75f, 0.92f, 1.0f, 0.82f});
        break;

    // 通知を受けた床は予告色にするだけで、遅延が尽きるまで支えを残します。
    case State::kPending:
        SetColor({0.95f, 0.85f, 0.35f, 0.9f});
        break;

    // 揺れ始めは色を強め、次の床へ通知する権利を用意します。
    case State::kShaking:
        SetColor({1.0f, 0.95f, 0.45f, 0.9f});
        triggeredNext_ = false;
        break;

    // 落下に入って初めて地面としての衝突を止め、次の床へ連鎖を通知します。
    case State::kFalling:
        SetCollisionAttribute(0);
        SetCollisionMask(0);
        velocityY_ = 0.0f;
        TriggerNextFloor();
        break;

    // 落下後は姿と判定を消し、この時点から復帰までの時計を進められるようにします。
    case State::kHidden:
        SetIsVisible(false);
        SetCollisionAttribute(0);
        SetCollisionMask(0);
        returnTimer_ = 0.0f;
        break;
    }
    UpdateRockAppearance();
}

void GimmickChainCollapseFloor::CheckPlayerContact() {
    // Sweepで床の直前に停止した場合と、薄い床を一フレームで通過した場合を拾う。
    BaseScene* scene = SceneManager::GetInstance()->GetCurrentScene();
    // 現在のシーンが見つからない場合は、そのフレームの補助接地判定を行わない。
    if (!scene) return;
    Player* player = GetScenePlayer(scene);
    // プレイヤーが見つからないか死亡している場合は、次の検出に古い足元座標を使わない。
    if (!player || player->IsDead()) {
        hasPreviousPlayerFoot_ = false;
        return;
    }

    // 床とプレイヤーの最新姿勢で足元と天面を比較し、前回の足元を越え判定に使います。
    UpdateLocalMatrix();
    UpdateWorldMatrix(false);
    player->UpdateLocalMatrix();
    player->UpdateWorldMatrix(false);
    const auto deck = GetAabb();
    const auto body = player->GetAabb();
    const float footY = body.min.y;
    const float topY = deck.max.y;
    const bool overlapsXZ = body.max.x > deck.min.x + 0.02f && body.min.x < deck.max.x - 0.02f &&
        body.max.z > deck.min.z + 0.02f && body.min.z < deck.max.z - 0.02f;
    const bool nearTop = footY >= topY - 0.25f && footY <= topY + 0.25f;
    // 現在の足元が天面の許容範囲を飛び越えても、前回が上側ならその間の通過を拾います。
    const bool crossedTop = hasPreviousPlayerFoot_ && previousPlayerFootY_ >= topY - 0.1f &&
        footY <= topY + 0.1f;
    previousPlayerFootY_ = footY;
    hasPreviousPlayerFoot_ = true;

    // 外接箱だけでは拾う余白を、フットプリントがある床では実際の形状でも確認します。
    const engine::Collider* collider = GetCollider();
    const bool insideFootprint = !collider || collider->GetConfig().footprint.size() < 3 ||
        collider->OverlapsFootprint(player->GetCollider());
    // 上昇していないプレイヤーの足元が天面付近を通り、XZとフットプリントが重なるときに揺れを開始する。
    if (overlapsXZ && (nearTop || crossedTop) && player->GetVelocity().y <= 0.1f && insideFootprint) {
        StartCollapse();
    }
}

void GimmickChainCollapseFloor::UpdateRockAppearance() {
    // 崩落岩モデルの系統だけ、床の段階に合わせて無傷・崩壊モデルと不透明色を適用する。
    if (GetModelName().find("Gimmicks/crumbling_rock_block") != 0)
        return;
    const std::string desired = (state_ == State::kIdle || state_ == State::kPending)
        ? "Gimmicks/crumbling_rock_block_intact" : "Gimmicks/crumbling_rock_block";
    // 現在のモデルが段階に対応するモデルと異なるときだけ差し替える。
    if (GetModelName() != desired) {
        // モデル変更の自動フィットでEditor設定の判定が上書きされないようにする。
        const auto collider = GetColliderConfig();
        SetModel(desired);
        SetColliderConfig(collider);
    }
    SetColor({1.0f, 1.0f, 1.0f, 1.0f});
}

bool GimmickChainCollapseFloor::ShouldReturnAfterCollapse() const {
    // 復帰は配置設定で明示された床にだけ許可します。
    return GetParameters().has_value() && GetParameters()->returnOnOff;
}

void GimmickChainCollapseFloor::TriggerNextFloor() {
    // 落下状態へ入った時だけ次の床へイベントを一度伝えます。
    if (triggeredNext_)
        return;

    triggeredNext_ = true;
    std::int32_t targetId = GetTargetId();
    // 連鎖先IDが未設定を表す-1なら、シーンへイベントを送らない。
    if (targetId == -1)
        return;

    // 現在のシーンを取得できた場合に、連鎖先IDへ起動イベントを送る。
    if (BaseScene* scene = SceneManager::GetInstance()->GetCurrentScene()) {
        scene->TriggerEvent(targetId);
    }
}

float GimmickChainCollapseFloor::GetShakeDuration() const {
    // 旧データに設定がない場合は既定の予告時間を使います。
    return GetParameters().has_value() ? GetParameters()->shakeDuration : 0.45f;
}

float GimmickChainCollapseFloor::GetFallDuration() const {
    // 落下を続ける秒数を取得する。パラメーターがなければ1.4秒。
    return GetParameters().has_value() ? GetParameters()->fallDuration : 1.4f;
}

float GimmickChainCollapseFloor::GetChainDelay() const {
    // 後続床が揺れ始めるまでの遅延を秒で返す。パラメーターがなければ0.18秒。
    return GetParameters().has_value() ? GetParameters()->interval : 0.18f;
}

float GimmickChainCollapseFloor::GetGravity() const {
    // 鉛直速度から毎秒差し引く加速度。パラメーターがなければ48ワールド単位/秒²。
    return GetParameters().has_value() ? GetParameters()->gravity : 48.0f;
}

std::unique_ptr<engine::Object3D> GimmickChainCollapseFloor::Clone() const {
    // 複製先を初期化し、連鎖の設定と元の配置を引き継ぎます。
    auto newObj = std::make_unique<GimmickChainCollapseFloor>();
    assert(GetObjectCommon() != nullptr);
    newObj->Initialize(GetObjectCommon(), GetModelName());
    newObj->CopyFrom(this);
    return newObj;
}
