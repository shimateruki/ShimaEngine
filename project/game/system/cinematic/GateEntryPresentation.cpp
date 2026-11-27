#define NOMINMAX
#include <cstdint>
#include "game/system/cinematic/GateEntryPresentation.h"
#include "Easing.h"
#include "Player.h"
#include "Camera.h"
#include "CameraManager.h"
#include "Object3D.h"
#include "Model.h"
#include "PostEffect.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace {
constexpr float kDuration = 1.55f;
// 演出の進行率に使う値を0〜1の範囲へ収めます。
float Clamp(float t) { return std::clamp(t, 0.0f, 1.0f); }


// 2つの座標の各成分を重みtで線形補間します。
engine::Vector3 Lerp(const engine::Vector3& a, const engine::Vector3& b, float t) {
    return {a.x + (b.x-a.x)*t, a.y + (b.y-a.y)*t, a.z + (b.z-a.z)*t};
}
// 視点から注視点を向くピッチ・ヨーをラジアンで返します。同じ位置に近ければ回転を0にします。
engine::Vector3 LookAt(const engine::Vector3& eye, const engine::Vector3& target) {
    const auto d = target - eye;
    const float length = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
    return length > 0.0001f ? engine::Vector3{-std::asin(std::clamp(d.y/length, -0.98f, 0.98f)),
                                            std::atan2(d.x, d.z), 0.0f} : engine::Vector3{};
}
engine::Object3D* ResolveAnchor(engine::Object3D* gate) {
    // 判定面が見た目の枠の子なら、枠の中心へ入ります。地形などの整理用の親は使いません。
    for (auto* parent = gate->GetParent(); parent; parent = parent->GetParent()) {
        const auto model = parent->GetModelName();
        // 親が既知のゲート枠モデルを持つ場合は、判定オブジェクトの代わりにその枠を使う。
        if (model == "Stages/gate" || model == "Gimmicks/crown_stage_gate") return parent;
    }
    return gate;
}
// 突入先の中心をワールド座標で求めます。
engine::Vector3 PortalPosition(engine::Object3D* gate, bool useModelCenter) {
    // モデル中心を使わない設定か、モデルを持たない場合はゲートの原点を使う。
    if (!useModelCenter || !gate->GetModel()) return gate->GetWorldPosition();
    const auto bounds = gate->GetModelWorldAabb();
    return (bounds.min + bounds.max) * 0.5f;
}
// ゲート面を横切る水平の単位方向を求め、プレイヤーから入口へ向かう向きに揃えます。
engine::Vector3 Direction(engine::Object3D* gate, const engine::Vector3& from, bool useModelCenter) {
    auto fallback = PortalPosition(gate,useModelCenter) - from;
    float length = std::sqrt(fallback.x*fallback.x + fallback.z*fallback.z);
    // 入口に対する水平距離がない場合に備え、代替の向きには正のZを使う。
    fallback = length > 0.0001f ? engine::Vector3{fallback.x/length, 0.0f, fallback.z/length}
                              : engine::Vector3{0.0f, 0.0f, 1.0f};
    const auto& world = gate->GetTransform()->matWorld;
    // モデルがない場合の法線には、既定のローカルZ軸を使う。
    std::int32_t normalAxis = 2;
    // モデルがある場合は、ローカルAABBで最も薄い軸を法線に選ぶ。
    if (auto* model = gate->GetModel()) {
        const auto extent = model->GetLocalAabbMax() - model->GetLocalAabbMin();
        // XとYの厚さが同じならYを候補にし、候補とZの厚さが同じならZを維持する。
        if (extent.x < extent.y) {
            if (extent.x < extent.z) {
                normalAxis = 0;
            }
        } else if (extent.y < extent.z) {
            normalAxis = 1;
        }
    }
    auto direction = engine::Vector3{world.m[normalAxis][0], 0.0f, world.m[normalAxis][2]};
    length = std::sqrt(direction.x*direction.x + direction.z*direction.z);
    // 編集されたゲートが横向き・水平の場合は、接触した側から中心へ入ります。
    if (length <= 0.0001f) return fallback;
    direction = {direction.x/length, 0.0f, direction.z/length};
    // 厚み軸の正負はモデルによって異なるため、プレイヤーから入口へ進む向きにそろえます。
    if (direction.x*fallback.x + direction.z*fallback.z < 0.0f)
        direction = {-direction.x, 0.0f, -direction.z};
    return direction;
}
// 演出中のカメラを固定点モードにし、視点と注視点のワールド座標から姿勢を設定します。
void SetCameraPose(engine::Camera* camera, const engine::Vector3& eye, const engine::Vector3& target) {
    const auto rotation = LookAt(eye, target);
    camera->SetInputEnabled(false);
    camera->SetFollowTarget(nullptr);
    camera->SetFollowMode(engine::Camera::FollowMode::kFixedPoint);
    camera->ConfigFixedPoint(eye, rotation);
    // ConfigFixedPoint は設定値の保存だけなので、追従対象のないカメラにも現在姿勢を直接渡します。
    camera->SetEye(eye);
    camera->SetTarget(target);
    camera->SetRotation(rotation);
    camera->Update(0.0f);
}
}

// ゲート通過中だけ所有する進行状態。Actor・Cameraは借用し、開始前の姿勢・材質・追従先を退避する。
struct GateEntryPresentation::State {
    // 一つの本体・子モデルの元材質と表示状態。Resetでディゾルブ前の値へ戻す。
    struct MaterialState {
        engine::Object3D* object = nullptr;
        engine::Object3D::MaterialState material{};
        engine::Vector4 color{};
        BlendMode blend = BlendMode::kNormal;
        std::int32_t materialType = 0;
        float emissive = 0.0f;
        bool visible = true;
    };
    Player* player = nullptr;
    engine::Object3D* gate = nullptr;
    engine::Camera* camera = nullptr;
    engine::Object3D* cameraFollow = nullptr;
    engine::Camera::FollowMode cameraMode = engine::Camera::FollowMode::kAimable;
    bool cameraInput = true;
    engine::Vector3 start{}, scale{}, rotation{}, direction{};
    engine::Vector3 eye{}, target{}, cameraRotation{};
    bool visible = true;
    bool useModelCenter = false;
    bool manageBars = true;
    float baseBarHeight = 0.0f;
    float timer = 0.0f;
    std::vector<MaterialState> materials;
};

GateEntryPresentation::GateEntryPresentation() = default;
GateEntryPresentation::~GateEntryPresentation() = default;
// 演出用の状態を持っている間はtrueを返します。終了時刻に達したあともReset()までは状態を保持します。
bool GateEntryPresentation::IsActive() const { return state_ != nullptr; }
// 演出用の状態を持ち、1.55秒の進行を終えたらtrueを返します。
bool GateEntryPresentation::IsFinished() const { return state_ && state_->timer >= kDuration; }
// 演出がある場合の進行率を0〜1で返します。状態がなければ0です。
float GateEntryPresentation::GetProgress() const { return state_ ? Clamp(state_->timer/kDuration) : 0.0f; }

bool GateEntryPresentation::Start(Player* player, engine::Object3D* gate, bool manageCinemaBars) {
    engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();
    // すでに演出中、必要な実体がない、HPが0以下、または別の演出やゲート復帰で操作を制限している場合は開始しない。
    if (state_ || !player || !gate || !camera || player->GetHp() <= 0.0f || player->IsCinematicLocked() ||
        player->IsGateReturnAnimationActive())
        return false;
    // 演出前の姿勢やカメラ設定を退避し、終了時に同じ状態へ戻せるようにします。
    auto state = std::make_unique<State>();
    state->player = player;
    state->gate = ResolveAnchor(gate);
    state->useModelCenter = state->gate != gate;
    state->camera = camera;
    state->start = player->GetWorldPosition();
    state->scale = player->GetScale();
    state->rotation = player->GetRotation();
    state->visible = player->GetIsVisible();
    state->direction = Direction(state->gate, state->start, state->useModelCenter);
    state->cameraFollow = camera->GetFollowTarget();
    state->cameraMode = camera->GetFollowMode();
    state->cameraInput = camera->IsInputEnabled();
    state->eye = camera->GetEye();
    state->target = camera->GetTargetPoint();
    state->cameraRotation = camera->GetRotation();
    state->manageBars = manageCinemaBars;
    // 帯の管理を開始する前の高さを控える。管理フラグが偽なら更新と復元ではこの値を書き込まない。
    state->baseBarHeight = PostEffect::GetInstance()->GetParams()->cinemaBarHeight;

    // 衝突フレーム内で操作・被弾・物理を止め、コピー形態はそのままゲートへ入れます。
    player->BeginCinematicLock(true);
    // コピー形態の子パーツにも切り抜きを適用するため、階層をたどって各部品の元材質を控えます。
    std::vector<engine::Object3D*> pending{player};
    while (!pending.empty()) {
        auto* object = pending.back();
        pending.pop_back();
        State::MaterialState saved;
        saved.object = object;
        saved.color = object->GetColor();
        saved.blend = object->GetBlendMode();
        saved.emissive = object->GetEmissive();
        saved.materialType = object->GetMaterialType();
        saved.visible = object->GetIsVisible();
        saved.material = object->CaptureMaterialState();
        state->materials.push_back(saved);
        // 有効な子参照だけを走査対象にし、子の材質も切り抜きと復元の対象に含める。
        for (auto* child : object->GetChildren()) if (child) pending.push_back(child);
    }
    // 復元用の状態を保存してから、最初のフレームの姿勢・材質・カメラに反映する。
    state_ = std::move(state);
    Update(0.0f);
    return true;
}

// フレーム時間（秒）でゲート突入を進め、位置、切り抜き、カメラを更新します。
void GateEntryPresentation::Update(float deltaTime) {
    // 演出が開始されていなければ、プレイヤーやカメラの設定を操作しない。
    if (!state_) return;
    auto& s = *state_;
    // 時間を演出の長さまでで止め、位置や表示の補間が終了点を超えないようにする。
    s.timer = (std::min)(kDuration, s.timer + (std::max)(deltaTime, 0.0f));
    // tは1.55秒を0〜1に直した比率。整列の0.28秒など、s.timerで割る区間だけ実秒を使う。
    const float t = GetProgress();
    const auto gatePos = PortalPosition(s.gate,s.useModelCenter);
    // 向きから入口の手前と内部を求め、突入軌道の始点と終点に使う。
    const auto surface = gatePos - s.direction * 1.18f;
    const auto inside = gatePos + s.direction * 1.38f;
    // 高さ合わせを先に始め、その後に入口面への整列を重ねる。
    auto centeredStart = s.start;
    centeredStart.y += (surface.y-s.start.y)*Easing::InOutCubic01((t-0.06f)/0.36f);
    const auto alignedStart = Lerp(centeredStart, surface, Easing::OutCubic01(s.timer/0.28f));
    // 入口面から奥へ潜る曲線を作り、途中の制御点だけ持ち上げる。
    auto a = Lerp(surface, gatePos, 0.42f);
    auto b = Lerp(gatePos, inside, 0.58f);
    a.y += 0.18f;
    b.y += 0.18f*0.35f;
    const float dive = Easing::InOutCubic01((t-0.18f)/(0.88f-0.18f));
    const float u = 1.0f-dive;
    // 四つの点を使う3次ベジェ曲線。入口手前とゲート内部を両端に固定し、途中だけ高さと曲がり方を調整する。
    const auto divePos = surface*(u*u*u) + a*(3.0f*u*u*dive) + b*(3.0f*u*dive*dive) + inside*(dive*dive*dive);
    // 手前の整列から突入へ滑らかに引き継ぎ、通常入力と速度が演出位置を上書きしないようにする。
    s.player->SetTranslate(Lerp(alignedStart, divePos, Clamp((t-0.08f)/0.28f)));
    s.player->SetScale(s.scale);
    s.player->SetMoveYaw(std::atan2(s.direction.x,s.direction.z));
    s.player->SetSlimeAnimationDirection(s.direction);
    s.player->SetVelocity({0.0f,0.0f,0.0f});
    s.player->SetIsControlActive(false);

    // 入口を通過する途中から切り抜きと発光を進め、終盤で本体をフェードアウトする。
    const float clipT = Easing::InOutCubic01((t-0.10f)/(0.74f-0.10f));
    const float clip = Clamp(0.14f + Easing::OutCubic01(clipT)*0.86f);
    // 切り抜きが半分のときだけ縁を最も強くし、始まりと終わりでは発光の増分を下げる。
    const float edgePeak = 1.0f-std::abs(clip*2.0f-1.0f);
    const float fade = Easing::InOutCubic01((clip-0.72f)/0.28f);
    const float heat = Clamp(0.08f+clip*0.18f+edgePeak*0.30f);
    // 開始時に控えた各描画部品に同じゲート面を適用し、元から非表示の部品は出さない。
    // 保存された全パーツへ適用し、元が可視で切り抜きと演出時刻がしきい値に達していないものだけ表示する。
    for (const auto& saved : s.materials) {
        auto* object = saved.object;
        object->SetMaterialType(4);
        object->SetBlendMode(BlendMode::kNormal);
        object->SetEmissive((std::max)(saved.emissive,1.25f+clip*0.95f+edgePeak*1.45f));
        object->SetColor({saved.color.x*(1.0f-heat)+heat, saved.color.y*(1.0f-heat)+0.76f*heat,
                          saved.color.z*(1.0f-heat)+0.34f*heat, 1.0f-fade});
        object->SetIsVisible(saved.visible && clipT < 0.995f && t < 0.94f);
        {
            engine::Object3D::PortalClipSettings clipSettings;
            clipSettings.enabled = true;
            clipSettings.progress = clip;
            clipSettings.center = gatePos;
            clipSettings.normal = s.direction;
            clipSettings.edgeWidth = 0.30f-0.16f*clip;
            clipSettings.dissolve = 0.74f-0.18f*clip;
            clipSettings.color = {1.0f,0.66f,0.26f,0.94f};
            object->SetPortalClip(clipSettings);
        }
    }
    // プレイヤーと子モデルのワールド行列を更新し、このフレームで変わった姿勢を描画へ渡す。
    s.player->UpdateLocalMatrix();
    s.player->UpdateWorldMatrix(true);
    // 現在の入口位置からカメラの注視点を決め、演出開始時の視点から寄せる。
    const engine::Vector3 focus{gatePos.x,gatePos.y+0.92f,gatePos.z};
    // 注視点から突入方向の反対へ14.25ワールド単位離し、入口の手前から奥が見える構図を作る。
    const engine::Vector3 endEye{focus.x-s.direction.x*14.25f,focus.y+0.58f,focus.z-s.direction.z*14.25f};
    const float cameraT = Easing::InOutCubic01(s.timer/0.34f);
    SetCameraPose(s.camera,Lerp(s.eye,endEye,cameraT),Lerp(s.target,focus,cameraT));
    s.player->RefreshRenderCameraData();
    // 帯をこの演出で担当する場合だけ、元の高さからシネマ表示の高さへ寄せる。
    if (s.manageBars) {
        const float height = (std::max)(s.baseBarHeight,0.105f);
        PostEffect::GetInstance()->GetParams()->cinemaBarHeight = s.baseBarHeight + (height-s.baseBarHeight)*Clamp(s.timer*5.8f);
    }
}

// 退避したプレイヤーの姿勢・材質、カメラ設定、担当している帯を戻し、演出状態を破棄します。
void GateEntryPresentation::Reset() {
    // 演出用の状態を持っていなければ、復元する対象がないため終了する。
    if (!state_) return;
    auto& s = *state_;
    // 中断時に動かした座標・姿勢と操作制限を、演出の開始前へ戻す。
    s.player->EndCinematicLock(true);
    s.player->SetTranslate(s.start);
    s.player->SetScale(s.scale);
    s.player->SetRotation(s.rotation);
    s.player->SetIsVisible(s.visible);
    // 部品ごとに控えていた材質と表示を戻し、切り抜きが通常描画へ残らないようにする。
    for (const auto& saved : s.materials) {
        auto* object = saved.object;
        object->SetMaterialType(saved.materialType);
        object->SetBlendMode(saved.blend);
        object->SetEmissive(saved.emissive);
        object->SetColor(saved.color);
        object->SetIsVisible(saved.visible);
        object->RestoreMaterialState(saved.material);
    }
    s.player->UpdateLocalMatrix();
    s.player->UpdateWorldMatrix(true);
    // 位置と向きに加えて追従先や操作モードも元に戻し、次のカメラ更新を通常状態から始める。
    s.camera->ConfigFixedPoint(s.eye,s.cameraRotation);
    s.camera->SetEye(s.eye);
    s.camera->SetTarget(s.target);
    s.camera->SetRotation(s.cameraRotation);
    s.camera->SetFollowTarget(s.cameraFollow);
    s.camera->SetFollowMode(s.cameraMode);
    s.camera->SetInputEnabled(s.cameraInput);
    s.player->RefreshRenderCameraData();
    // この演出が変更した帯だけ復元する。外側の演出が管理していた帯はそちらへ任せる。
    if (s.manageBars) PostEffect::GetInstance()->GetParams()->cinemaBarHeight = s.baseBarHeight;
    // 復元を終えてから状態を捨て、同じ演出を再度終了しないようにする。
    state_.reset();
}
