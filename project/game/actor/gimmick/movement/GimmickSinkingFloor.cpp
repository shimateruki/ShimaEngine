#include "GimmickSinkingFloor.h"
#include "engine/editor/properties/ObjectPropertyDefinition.h"
#include "CollisionConfig.h"
#include "MovingFloorSupport.h"
#include "SceneManager.h"
#include "BaseScene.h"
#include "Player.h"
#include <algorithm>
#include <cmath>

namespace {
// 一つの沈む床を構成する子の上面も調べ、どの部品を踏んでも親を始動する。
bool HasRiderOnDeck(engine::Object3D& part, Character& actor) {
    // この部品に有効なコライダーと衝突属性がある場合だけ、部品の上面を調べる。
    if (const engine::Collider* collider = part.GetCollider(); collider && part.GetCollisionAttribute() != 0) {
        // モデル形状を使わない一般的な床で、共通の接地確認に成功すれば乗員とする。
        if (!collider->HasModelVolume() && IsStandingOnMovingFloor(part, actor))
            return true;
        // 生存して接地中かつ上昇していない相手は、モデルの実形状から支えを確認する。
        if (!actor.IsDead() && actor.IsGrounded() && actor.GetVelocity().y <= 0.1f &&
            collider->HasModelVolume()) {
            // 親の非一様拡縮で傾いた子床は、箱の上端でなく実際の凸形状の上面を測る。
            const engine::Aabb body = actor.GetAabb();
            const engine::Vector3 origin = {
                (body.min.x + body.max.x) * 0.5f, body.min.y + 0.35f,
                (body.min.z + body.max.z) * 0.5f
            };
            float distance = 0.0f;
            engine::Vector3 normal{};
            // 足元近くの下向きレイが上面に当たり、足の高さとの差が0.25以内なら乗っている。
            if (collider->RaycastModelVolume(origin, {0.0f, -1.0f, 0.0f}, 0.65f, distance, normal) &&
                normal.y > 0.5f && std::abs(origin.y - distance - body.min.y) <= 0.25f)
                return true;
        }
        // 傾いた子床ではAABB最高点が上面中央より高い。実際のOBB上面で足元を測る。
        // 生存・接地中で上昇していない相手は、凸モデルを使わない OBB の上面も調べる。
        if (!actor.IsDead() && actor.IsGrounded() && actor.GetVelocity().y <= 0.1f &&
            collider->GetType() == engine::ColliderType::kObb && !collider->HasModelVolume()) {
            const engine::Obb box = collider->GetObb();
            const engine::Vector3 normal = box.orientations[1];
            const auto body = actor.GetAabb();
            const auto bounds = part.GetAabb();
            // OBB上面の法線が上向きで、身体と床の水平範囲も重なるときだけ上面高さを計算する。
            if (normal.y > 0.5f && body.max.x > bounds.min.x && body.min.x < bounds.max.x &&
                body.max.z > bounds.min.z && body.min.z < bounds.max.z) {
                const float x = (body.min.x + body.max.x) * 0.5f;
                const float z = (body.min.z + body.max.z) * 0.5f;
                const engine::Vector3 top = box.center + normal * box.size.y;
                const float surfaceY = top.y - (normal.x * (x - top.x) + normal.z * (z - top.z)) / normal.y;
                // 足が上面から0.25以内で、輪郭がある場合も接触範囲内なら乗員とする。
                if (std::abs(body.min.y - surfaceY) <= 0.25f &&
                    (collider->GetConfig().footprint.size() < 3 || collider->OverlapsFootprint(actor.GetCollider())))
                    return true;
            }
        }
    }
    // この部品に乗っていなければ、再生から除外されていない子部品の上面も再帰的に調べます。
    for (engine::Object3D* child : part.GetChildren()) {
        // 子の参照がありリプレイで除去されていなければ、その上面も探す。
        if (child && !child->IsReplayRemoved() && HasRiderOnDeck(*child, actor))
            return true;
    }
    return false;
}
} // namespace

void GimmickSinkingFloor::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    // プレイヤーが乗れる床として衝突属性を設定する。
    BaseGimmick::Initialize(common, modelName);

    SetClassName("Gimmick");
    SetGimmickType("SinkingFloor");
    SetName("Gimmick_SinkingFloor");

    // 登録済みのクラスでは共通処理が二重登録を省きます。個体のアドレスは台帳へ渡しません。
    engine::ObjectPropertyDefinition<GimmickSinkingFloor> properties(*this, kTypeId, "沈む床");
    const auto shallow = +[](const GimmickSinkingFloor& floor) { return !floor.GetParameters() || floor.GetParameters()->actionMode != 1; };
    const auto submerged = +[](const GimmickSinkingFloor& floor) { return floor.GetParameters() && floor.GetParameters()->actionMode == 1; };
    using Parameter = engine::Object3D::EntityParameter;
    properties.Parameter("沈み方", "actionMode", &Parameter::actionMode, 0, 1, nullptr,
                         {"従来の浅い沈み込み", "踏むと水没して復帰"});
    properties.Expose("浅い沈み深さ", ENGINE_PROPERTY(sinkDepth_), 0.1f, 100.0f, shallow);
    properties.Expose("沈み込みの追従係数", ENGINE_PROPERTY(sinkSpeed_), 0.1f, 20.0f, shallow);
    properties.Parameter("沈む深さ", "moveAmount", &Parameter::moveAmount, 0.1f, 100.0f, submerged);
    properties.Parameter("昇降速度", "speed", &Parameter::speed, 0.1f, 20.0f, submerged);
    properties.Parameter("水没後の待機時間", "interval", &Parameter::interval, 0.1f, 10.0f, submerged);
    SetCollisionAttribute(engine::collisionCategories::Get("ground"));
    SetCollisionMask(engine::collisionCategories::GetDefaultGameplayMask());
}

void GimmickSinkingFloor::Update(float deltaTime) {
    // actionMode で単純な踏み込みと、段階的な水没演出を切り替える。
    if (GetParameters() && GetParameters()->actionMode == 1) {
        UpdateSubmerging(deltaTime);
        return;
    }

    // 浅い沈み込みの初回は、移動前の位置を復帰の基準にする。
    if (!initializedStart_) {
        startPos_ = GetTransform()->translate;
        initializedStart_ = true;
    }

    // 前回の接触猶予があれば経過秒数を減らす。
    if (contactTimer_ > 0.0f) {
        contactTimer_ -= deltaTime;
        // 経過が猶予を超えた場合は0秒にそろえる。
        if (contactTimer_ < 0.0f)
            contactTimer_ = 0.0f;
    }

    // 浅いモードでは接触の猶予中だけ沈む高さを目標にし、解けたら元の高さへ滑らかに戻します。
    const bool isStepped = contactTimer_ > 0.0f;
    float targetY = startPos_.y + (isStepped ? -sinkDepth_ : 0.0f);
    // sinkSpeed_は追従率です。経過秒数を使う指数補間で、フレーム数に左右されず目標との高さ差を縮めます。
    float t = 1.0f - std::exp(-sinkSpeed_ * deltaTime);

    engine::Vector3 pos = GetTransform()->translate;
    pos.y = engine::Math::Lerp(pos.y, targetY, t);
    SetTranslate(pos);

    BaseGimmick::Update(deltaTime);
}

bool GimmickSinkingFloor::OnCollision(engine::Object3D* other) {
    // プレイヤーが上面を踏んだ短い間だけ、目標の沈み位置を維持する。
    if (other && other->GetClassName() == "Player") {
        engine::CollisionInfo info = CheckCollision(other);

        // 実際に衝突し、床が下へ押される上面側の接触だけ猶予を更新する。
        if (info.isColliding && info.normal.y < -0.5f) {
            contactTimer_ = 0.12f;
        }
    }

    return true;
}

std::unique_ptr<engine::Object3D> GimmickSinkingFloor::Clone() const {
    // 移動中の位相は複製せず、保存された配置と設定だけ引き継ぐ。
    auto newObj = std::make_unique<GimmickSinkingFloor>();
    assert(GetObjectCommon() != nullptr);
    newObj->Initialize(GetObjectCommon(), GetModelName());
    newObj->CopyFrom(this);
    return newObj;
}

void GimmickSinkingFloor::UpdateSubmerging(float deltaTime) {
    // 親子で組んだ床は親だけが沈下を計算し、子はローカル姿勢のまま追従する。
    if (dynamic_cast<GimmickSinkingFloor*>(GetParent())) {
        BaseGimmick::Update(deltaTime);
        return;
    }

    // 編集状態へ戻った場合は、沈下を停止してすべての位相の時計を初期化する。
    auto* manager = SceneManager::GetInstance();

    // シーン管理器が未作成かプレイが停止中の場合は、開始位置と水没前の状態へ戻す。
    if (!manager || !manager->IsPlaying()) {
        // 開始位置を控えている場合だけ、元の高さと位置を戻す。
        if (initializedStart_)
            SetTranslate(startPos_);
        initializedStart_ = false;
        submergePhase_ = 0;
        submergeTimer_ = 0.0f;
        contactTimer_ = 0.0f;
        BaseGimmick::Update(deltaTime);
        return;
    }

    // プレイ開始時にまだ配置を記録していなければ、現在位置を開始位置にする。
    if (!initializedStart_) {
        startPos_ = GetTranslate();
        initializedStart_ = true;
    }

    // 床を動かす前の位置で乗員を集め、移動後に同じ変位を乗員へ適用できるようにします。
    std::vector<Character*> riders;

    // 現在のシーンがあれば、その実体を対象に乗車を判定する。
    if (auto* scene = manager->GetCurrentScene()) {
        for (auto& object : scene->GetObjects()) {
            auto* actor = object && !object->IsReplayRemoved() ? dynamic_cast<Character*>(object.get()) : nullptr;
            bool standing = actor && HasRiderOnDeck(*this, *actor);
            // 通常判定で乗員にならなかった接地中のプレイヤーも、モーターの接地先を別に確認する。
            if (auto* player = dynamic_cast<Player*>(actor); player && !standing && player->IsGrounded() &&
                player->GetVelocity().y <= 0.1f) {
                // Motorが子の斜面を接地相手と確定した場合は、その親まで乗車を伝える。
                const auto& ground = player->GetCharacterMotor().GetResult();
                // モーターが実際に接地を確定していれば、接地先から親をたどる。
                if (ground.grounded) {
                    for (auto* support = ground.groundObject; support; support = support->GetParent()) {
                        // この床が接地先の階層に含まれる場合は、この床の乗員とする。
                        if (support == this) { standing = true; break; }
                    }
                }
            }
            // 移動前に乗っていると確認した相手だけを、床と一緒に動かす。
            if (standing)
                riders.push_back(actor);
        }
    }

    const float frameDeltaTime = (std::max)(0.0f, deltaTime);
    const float depth = (std::max)(0.1f, GetParameters()->moveAmount);
    const float speed = (std::max)(0.1f, GetParameters()->speed);
    const engine::Vector3 before = GetTranslate();
    engine::Vector3 next = before;

    // 待機中に初めて乗員が見つかったら、沈み込みを開始する。
    if (submergePhase_ == 0 && !riders.empty()) {
        submergePhase_ = 2;
        submergeTimer_ = 0.0f;
    }

    // 一度踏んだら最深部まで沈む。接地が一瞬切れても途中で浮き戻らない。
    if (submergePhase_ == 2) {
        next.y = (std::max)(startPos_.y - depth, before.y - speed * frameDeltaTime);

        // 最深部へ届いたら、沈下を止めて滞在時間を計る。
        if (next.y <= startPos_.y - depth) {
            submergePhase_ = 3;
            submergeTimer_ = 0.0f;
        }
    // 最深部では時間だけ進め、待機が終わってから上昇へ移ります。
    } else if (submergePhase_ == 3) {
        submergeTimer_ += frameDeltaTime;
        // 最深部の待ち時間を消化したら上昇へ移る。
        if (submergeTimer_ >= (std::max)(0.1f, GetParameters()->interval))
            submergePhase_ = 4;
    // 開始時の高さまで上昇し、次に踏まれるまでは待機します。
    } else if (submergePhase_ == 4) {
        next.y = (std::min)(startPos_.y, before.y + speed * frameDeltaTime);
        // 配置時の高さへ戻り切ったら再び待機する。
        if (next.y >= startPos_.y)
            submergePhase_ = 0;
    }

    SetTranslate(next);
    BaseGimmick::Update(deltaTime);

    // 移動床と同じ順序で乗員を運び、下降中にプレイヤーが宙に残るのを防ぐ。
    const engine::Vector3 displacement = next - before;

    for (auto* rider : riders) {
        rider->SetTranslate(rider->GetTranslate() + displacement);
        rider->UpdateLocalMatrix();
        rider->UpdateWorldMatrix(false);
        // プレイヤーには正の秒数で変位を割った足場速度も伝える。
        if (auto* player = dynamic_cast<Player*>(rider); player && frameDeltaTime > 0.0f)
            player->SetPlatformMotion(displacement * (1.0f / frameDeltaTime));
    }
}
