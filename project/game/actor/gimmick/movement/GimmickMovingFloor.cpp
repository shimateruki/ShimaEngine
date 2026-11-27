#include <cstdint>
#include "GimmickMovingFloor.h"
#include <numbers>
#include "ObjectPropertyDefinition.h"
#include <algorithm>
#include <cmath>
#include "CollisionConfig.h"
#include "RidingPlatformController.h"
#include "MovingFloorSupport.h"
#include "GhostRecorder.h"
#include "BaseScene.h"
#include "SceneManager.h"
#include "Player.h"
#include "Math.h"
#include "game/actor/gimmick/combat/GimmickMagmaGeyser.h"

GimmickMovingFloor::GimmickMovingFloor() = default;
GimmickMovingFloor::~GimmickMovingFloor() = default;

bool GimmickMovingFloor::IsRidePlatform() const {
    // 乗車タイムラインがある床は手続き移動と別の制御経路にする。
    const auto* path = GetPathMoverComponent();
    return path && !path->GetRideSequence().empty();
}

namespace {
constexpr float kLegacyMoveRange = 3.0f;
constexpr float kMaxFloatAmplitude = 2.0f;
constexpr float kFloatPitchScale = 0.025f;
constexpr float kFloatRollScale = 0.035f;
constexpr float kTwoPi = (2.0f * std::numbers::pi_v<float>);

bool HasCurvedSupportBelow(engine::Object3D& floor, const Player& player) {
    // 互換モーターで接地相手を得られないときは、足元を延長した身体の箱を使って曲面を照会する。
    // AABB最高点との比較では、円柱が少し回っただけで乗車が途切れてしまう。
    // 地面属性があり凸モデル形状を持つコライダーに対して、身体の箱を組み立てる。
    if (auto* collider = floor.GetCollider(); collider && collider->HasModelVolume() &&
        (floor.GetCollisionAttribute() & engine::collisionCategories::Get("ground"))) {
        // 足の中心だけのレイでは、箱形の足の端が曲面に乗る接触を見落とす。
        // 実際の身体の幅を使い、押し戻し直後の隙間だけ下へ補う。
        auto bounds = player.GetAabb();
        bounds.min.y -= 0.08f;
        Transform queryTransform;
        queryTransform.translate = (bounds.min + bounds.max) * 0.5f;
        queryTransform.UpdateMatrix();
        engine::Collider query(&queryTransform);
        engine::ColliderConfig config;
        config.type = engine::ColliderType::kAabb;
        config.size = (bounds.max - bounds.min) * 0.5f;
        query.SetConfig(config);
        const auto contact = query.CheckCollision(collider);
        // 身体幅を使った照会で実際に重なり、法線が上向きなら曲面で支えられている。
        if (contact.isColliding && contact.normal.y > 0.5f)
            return true;
    }
    for (auto* child : floor.GetChildren()) {
        // この部品で接地が見つからなければ、有効な子部品の曲面も調べる。
        if (child && HasCurvedSupportBelow(*child, player)) return true;
    }
    return false;
}
} // namespace

void GimmickMovingFloor::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    // 床として接地判定できる属性を与え、旧データの速度欠落を補う。
    BaseGimmick::Initialize(common, modelName);
    SetClassName("Gimmick");
    SetGimmickType("MovingFloor");

    // 旧配置に設定が無ければ、移動方式と速度の格納先を用意する。
    if (!GetParameters().has_value())
        ResetParameters();
    // 速度が0の配置には、初期化時に既定の速度2を入れる。
    if (GetParameters()->speed == 0.0f)
        SetParameter(&engine::EntityParameter::speed, 2.0f);

    // 当たり判定設定 (地面として扱う)
    SetCollisionAttribute(engine::collisionCategories::Get("ground"));

    // タイムラインと速度、方式ごとの移動幅をInspectorに登録する。
    using Self = GimmickMovingFloor;
    engine::ObjectPropertyDefinition<Self> properties(*this, kTypeId, "移動床",
        "噴出口連動ではTarget IDを噴出口のMy Event IDへ接続します。高さと周期は噴出口側、記録軌道はGhost Recorderで調整します。");
    using Parameter = engine::Object3D::EntityParameter;
    properties.Accessor<std::string>("乗車開始タイムライン", "rideSequence", "components.PathMover.rideSequence",
        [](const Self& object) {
            const auto* path = object.GetPathMoverComponent();
            return path ? path->GetRideSequence() : std::string();
        },
        [](Self& object, const std::string& value) {
            object.EnsurePathMoverComponent()->SetRideSequence(value);
            // 乗車タイムラインを指定した場合は、旧Recorderの経路を解除する。
            if (!value.empty()) {
                object.SetRecordPathName("");
                // 旧Recorderがある場合は、経路名を外した後に再生も停止する。
                if (object.GetRecorder()) object.GetRecorder()->Stop(false);
            }
        }, 0, 0, nullptr, {},
        {.assetDirectory = "Resources/json/scenario", .assetEditor = EditorPropertyAssetEditor::kScenario,
         .help = +[](const Self& object) -> std::string {
             const auto* path = object.GetPathMoverComponent();
             return !path || path->GetRideSequence().empty() ? "" :
                 "乗ると出発し終点で停止します。床は水平に平行移動させ、死亡後は乗り場へ戻ると再準備します。旧Pathとは排他です。"
                 "敵の乗り込みはride.board、対象は敵名、payload例は{\"duration\":1.1,\"height\":6,\"offset\":[-3,0,0]}です。"
                 "ride.wait_clearは破壊待ち、ride.wait_switchはスイッチ待ちです。ride.fireballは噴出口を対象にし、警告時間+1.4秒以上の停船区間を設けます。";
         }});

    properties.Parameter("移動速度", "speed", &Parameter::speed, 0.0f, 100.0f,
        nullptr, {}, {.speed = 0.1f});
    properties.Parameter("移動方式", "actionMode", &Parameter::actionMode, 0.0f, 5.0f,
        nullptr, {"従来の上下移動", "浮遊", "X方向に往復", "Z方向に往復", "Y方向に往復", "マグマ噴出口と連動"});
    properties.Parameter("浮き沈み幅", "moveAmount", &Parameter::moveAmount, 0.0f, 2.0f,
        +[](const Self& object) { return object.GetParameters() && (object.GetParameters()->actionMode == 1); }, {}, {.speed = 0.01f});
    properties.Parameter("往復幅", "moveAmount_travel", &Parameter::moveAmount, 0.0f, 100.0f,
        +[](const Self& object) { return object.GetParameters() && (object.GetParameters()->actionMode >= 2 && object.GetParameters()->actionMode <= 4); }, {}, {.speed = 0.1f, .parameterId = "moveAmount"});
}

// プレイ中の移動元を選び、床を動かす前に確認した乗員をその変位で運びます。
void GimmickMovingFloor::Update(float deltaTime) {
    // 編集中は手続き移動を行わず、配置を次の再生の原点として保存する。
    if (!SceneManager::GetInstance()->IsPlaying()) {
        startPosition_ = GetTranslate();
        startRotation_ = GetRotation();
        hasCapturedStartTransform_ = true;
        time_ = 0.0f;
        frameDelta_ = {};
        // 噴出口連動の床は、編集中に警告時の発光を残さない。
        if (GetParameters() && GetParameters()->actionMode == 5)
            SetEmissive(0.10f);
        BaseGimmick::Update(deltaTime);
        return;
    }

    // 乗車用のタイムラインがあれば専用コントローラーが運行を担当し、以降の手続き移動は実行しません。
    if (IsRidePlatform()) {
        // 乗車用タイムラインを初めて更新する場合は、運行を管理するコントローラーを作る。
        if (!ride_)
            ride_ = std::make_unique<RidingPlatformController>();
        ride_->Update(*this, deltaTime);
        BaseGimmick::Update(deltaTime);
        return;
    }

    // 床を動かす前に接地中の乗員を記録し、移動後の床と位置がずれても乗員を見失わないようにします。
    std::vector<Character*> passengers;
    bool playerRidingPath = false;
    auto* scene = SceneManager::GetInstance()->GetCurrentScene();

    // 実行中のシーンと正の経過秒数がある場合だけ、移動前の乗員を集める。
    if (scene && SceneManager::GetInstance()->IsPlaying() && deltaTime > 0.0f) {
        for (auto& object : scene->GetObjects()) {
            auto* actor = object && !object->IsReplayRemoved() ? dynamic_cast<Character*>(object.get()) : nullptr;
            const bool standing = actor && IsStandingOnMovingFloor(*this, *actor);
            // 共通の接地判定を満たした実体を、追従させる一覧へ入れる。
            if (standing) passengers.push_back(actor);
            // 既存の乗員走査で通知も集め、Pathごとの追加衝突照会を避ける。
            auto* player = dynamic_cast<Player*>(actor);
            // 生存したプレイヤーが接地し上昇していない場合は、経路への乗車も調べる。
            if (player && !player->IsDead() && player->IsGrounded() && player->GetVelocity().y <= 0.1f) {
                const auto& ground = player->GetCharacterMotor().GetResult();
                // モーターが接地と判定している場合だけ、接地相手から親を確認する。
                if (ground.grounded) {
                    for (auto* support = ground.groundObject; support; support = support->GetParent())
                        // この床または子の上にいる場合は、経路に乗車を通知できる。
                        if (support == this) playerRidingPath = true;
                }
                // 旧モーターで接地相手が得られない場合だけ上端照会へ戻す。
                if (!ground.groundObject && (standing || HasCurvedSupportBelow(*this, *player)))
                    playerRidingPath = true;
                // 円柱の斜面はAABBの最高点と一致しない。Motorで接地が確定した乗員も運ぶ。
                // 別経路で乗車を確認でき、通常判定の一覧に入っていない場合は追従に追加する。
                if (playerRidingPath && !standing) passengers.push_back(player);
            }
        }
    }

    const auto carryPassengers = [&]() {
        // 押し戻し後の再判定には依存せず、移動前に接地していた乗員を一度だけ運ぶ。
        for (auto* actor : passengers) {
            actor->SetTranslate(actor->GetTranslate() + frameDelta_);
            actor->UpdateLocalMatrix();
            actor->UpdateWorldMatrix(false);
            // プレイヤーには床の変位に加え、秒当たりの移動量を伝える。
            if (auto* player = dynamic_cast<Player*>(actor))
                player->SetPlatformMotion(frameDelta_ * (1.0f / deltaTime));
        }
    };

    // Ghost RecorderのPathが割り当てられている床は、記録済みの軌道を唯一の移動元にする。
    // 手続き的な往復移動を先に重ねると、Editorで調整したPathと乗り移り判定がずれる。
    if (!GetRecordPathName().empty()) {
        // 丸い床や傾斜でも、AABB上端ではなくMotorの接地相手で再生を判断する。
        // 経路名が登録済みで Recorder が存在する場合は、移動前に調べた乗車状態を渡す。
        if (GetRecorder()) GetRecorder()->SetPlayerRiding(playerRidingPath);
        const engine::Vector3 previousPosition = GetTransform()->translate;
        UpdateLocalMatrix();
        UpdateWorldMatrix(false);
        const auto previousWorld = GetWorldMatrix();
        const auto previousInverse = engine::Math::Inverse(previousWorld);
        BaseGimmick::Update(deltaTime);
        frameDelta_ = GetTransform()->translate - previousPosition;

        // Recorder が座標を書き換えた後、新しい床行列を先に確定し、その行列から乗員の移動量を求める。
        UpdateWorldMatrix(false);
        hasCapturedStartTransform_ = true;
        // 足の接触点を旧床座標から新床座標へ移す。円柱の回転でも乗員を置き去りにしない。
        // キャラクター自身を回転させず、ジャンプで離れた後はこの処理の対象外にする。
        for (auto* actor : passengers) {
            const auto worldPosition = actor->GetWorldPosition();
            const engine::Vector3 contact = {worldPosition.x, actor->GetAabb().min.y, worldPosition.z};
            const auto localContact = engine::Math::Transform(contact, previousInverse);
            const auto displacement = engine::Math::Transform(localContact, GetWorldMatrix()) - contact;
            const auto nextWorld = worldPosition + displacement;
            actor->SetTranslate(actor->GetParent()
                ? engine::Math::Transform(nextWorld, engine::Math::Inverse(actor->GetParent()->GetWorldMatrix()))
                : nextWorld);
            actor->UpdateLocalMatrix();
            actor->UpdateWorldMatrix(false);
            // プレイヤーを運んだ場合、正の経過秒数で変位を割って足場速度を渡す。
            if (auto* player = dynamic_cast<Player*>(actor); player && deltaTime > 0.0f)
                player->SetPlatformMotion(displacement * (1.0f / deltaTime));
        }
        return;
    }

    // 編集時の配置がまだ捕捉されていなければ、現在の Transform を手続き移動の原点にする。
    if (!hasCapturedStartTransform_) {
        startPosition_ = GetTransform()->translate;
        startRotation_ = GetTransform()->rotate;
        // 配置の水平位置から一周分の位相を作り、似た床を並べても初動をずらします。乱数は使わず配置ごとに固定です。
        phaseOffset_ = std::fmod(std::abs(startPosition_.x * 0.037f + startPosition_.z * 0.061f), kTwoPi);
        hasCapturedStartTransform_ = true;
    }

    const engine::Vector3 previousPosition = GetTransform()->translate;
    time_ += deltaTime;

    const float speed = GetParameters().has_value() ? (std::max)(GetParameters()->speed, 0.0f) : 0.0f;
    const std::int32_t actionMode = GetParameters().has_value() ? GetParameters()->actionMode : 0;

    // 配置位置を往復運動の中心として扱う。位相差は自然な浮遊モードだけに適用する。
    const float waveTime = time_ * speed + (actionMode == 1 ? phaseOffset_ : 0.0f);
    Transform transformValue = *GetTransform();
    Transform* transform = &transformValue;
    transform->translate = startPosition_;
    transform->rotate = startRotation_;

    // 方式5では、割り当てられたマグマ噴出口の進行から高さを決める。
    if (actionMode == 5) {
        // 有効な宛先IDが設定され、シーンが実行中なら噴出口を探す。
        if (scene && GetTargetId() >= 0 && SceneManager::GetInstance()->IsPlaying()) {
            for (const auto& object : scene->GetObjects()) {
                auto* geyser = dynamic_cast<GimmickMagmaGeyser*>(object.get());

                // 型とイベントIDが一致した噴出口の警告発光と上昇量を採用する。
                if (geyser && geyser->GetEventId() == GetTargetId()) {
                    SetEmissive(0.10f + geyser->GetWarningProgress() * 0.65f);
                    const float desiredY = startPosition_.y + geyser->GetLiftOffset();
                    const float maxStep = 12.0f * (std::max)(0.0f, deltaTime);
                    transform->translate.y =
                        std::clamp(desiredY, previousPosition.y - maxStep, previousPosition.y + maxStep);
                    break;
                }
            }
        }
    // 浮遊方式では上下移動と緩やかな二軸の傾きを加える。
    } else if (actionMode == 1) {
        const float amplitude = (std::clamp)(std::abs(GetParameters()->moveAmount), 0.0f, kMaxFloatAmplitude);
        const float tiltWeight = (std::min)(amplitude, 1.0f);
        transform->translate.y += std::sin(waveTime) * amplitude;
        transform->rotate.x += std::sin(waveTime * 0.73f + 1.2f) * kFloatPitchScale * tiltWeight;
        transform->rotate.z += std::sin(waveTime * 0.91f) * kFloatRollScale * tiltWeight;
        transform->isQuaternionMaster = false;
    // 往復方式では設定の振幅から変位を求め、指定軸だけを移動させる。
    } else if (actionMode >= 2 && actionMode <= 4) {
        const float travel = (std::max)(0.0f, std::abs(GetParameters()->moveAmount));
        const float offset = std::sin(waveTime) * travel;
        // 方式2ならX方向へ往復する。
        if (actionMode == 2)
            transform->translate.x += offset;
        // 方式3ならZ方向へ往復する。
        if (actionMode == 3)
            transform->translate.z += offset;
        // 方式4ならY方向へ往復する。
        if (actionMode == 4)
            transform->translate.y += offset;
    // 専用の方式に該当しない場合は、既定の幅で上下に往復する。
    } else {
        transform->translate.y += std::sin(waveTime) * kLegacyMoveRange;
    }

    // 床の姿勢と一緒に今回の移動量を確定し、床の更新後に乗員へ同じ変位を渡します。
    frameDelta_ = hasCapturedStartTransform_ ? transform->translate - previousPosition : engine::Vector3{};
    SetLocalTransform(transformValue);

    // 移動後のTransformを同じフレームで行列と当たり判定へ反映する。
    BaseGimmick::Update(deltaTime);
    carryPassengers();
}

bool GimmickMovingFloor::OnCollision(engine::Object3D* other) {
    // 乗員の移動はUpdate()が担当する。この通知では運搬を行わず、共通の衝突解決を許可する。
    (void)other;
    return true;
}

std::unique_ptr<engine::Object3D> GimmickMovingFloor::Clone() const {
    // 複製時は配置と設定だけを引き継ぎ、乗員・周期・原点は再生時に再取得する。
    auto clone = std::make_unique<GimmickMovingFloor>();
    clone->Initialize(GetObjectCommon(), GetModelName());
    clone->CopyFrom(this);
    return clone;
}

void GimmickMovingFloor::ResetEscapeCycle() {
    // 脱出床を保存済み原点へ戻し、前フレームの乗員移動量も消す。
    time_ = 0.0f;
    frameDelta_ = {};

    // 初期姿勢を取得済みの場合だけ、位置を原点へ戻して行列を作り直す。
    if (hasCapturedStartTransform_) {
        SetTranslate(startPosition_);
        UpdateLocalMatrix();
        UpdateWorldMatrix();
    }
}
