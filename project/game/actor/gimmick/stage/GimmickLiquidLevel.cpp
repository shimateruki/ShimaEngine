#include <cstdint>
#include "GimmickLiquidLevel.h"
#include "ObjectPropertyDefinition.h"
#include "CollisionConfig.h"
#include "SceneManager.h"
#include "BaseScene.h"
#include "Player.h"
#include "CameraManager.h"
#include "game/actor/gimmick/movement/GimmickMovingFloor.h"
#include "game/actor/gimmick/combat/GimmickMagmaGeyser.h"
#include "game/actor/gimmick/combat/GimmickFireCannon.h"
#include "game/actor/gimmick/trigger/GimmickSwitch.h"
#include <algorithm>
#include <cmath>

// 液面の描画と昇降用パラメーターを初期化し、モードごとの編集項目を公開する。
void GimmickLiquidLevel::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    BaseGimmick::Initialize(common, modelName);

    // 液面は足場の衝突には使わず、表示と高さの制御を担当します。
    SetClassName("Gimmick");
    SetGimmickType("LiquidLevel");
    SetName("Gimmick_LiquidLevel");
    SetCollisionAttribute(0);
    SetCollisionMask(0);
    SetScale({4.0f, 0.08f, 4.0f});
    SetStatic(false);

    // パラメーターが未作成の場合は、昇降量と開始状態を書き込めるように既定値を用意する。
    if (!GetParameters().has_value())
        ResetParameters();
    SetParameter(&engine::EntityParameter::moveAmount, 6.0f);
    SetParameter(&engine::EntityParameter::moveSpeed, 3.0f);
    SetParameter(&engine::EntityParameter::startActive, false);
    SetParameter(&engine::EntityParameter::returnOnOff, true);

    ApplyLiquidVisual();

    // 脱出モードの開始・終了・復帰マーカーはTarget IDの連番で指定する。予告時間は脱出モード専用にする。
    using Self = GimmickLiquidLevel;
    engine::ObjectPropertyDefinition<Self> properties(*this, kTypeId, "水位・マグマ上下",
        "脱出モードのTarget IDは開始マーカー、+1は終了、+2は復帰です。マーカーのScaleが進入範囲です。連動床・火山弾のTarget IDを液面のMy Event IDへ接続します。出現床とスイッチは液面ID+4、入口岩戸はID+5です。");
    using Parameter = engine::Object3D::EntityParameter;
    properties.Parameter("液面の動作", "actionMode", &Parameter::actionMode, 0.0f, 1.0f,
        nullptr, {"従来のスイッチ上下", "マグマ脱出：一定速度で上昇"});
    properties.Parameter("開始前の予告時間", "shakeDuration", &Parameter::shakeDuration, 0.0f, 10.0f,
        +[](const Self& object) { return object.GetParameters() && (object.GetParameters()->actionMode == 1); });
    properties.Parameter("液体の種類", "colorType", &Parameter::colorType, 0.0f, 1.0f,
        nullptr, {"水", "マグマ"});
    properties.Parameter("上下量", "moveAmount", &Parameter::moveAmount, -500.0f, 500.0f);
    properties.Parameter("上下速度", "moveSpeed", &Parameter::moveSpeed, 0.1f, 60.0f);
    properties.Parameter("開始時に有効", "startActive", &Parameter::startActive);
    properties.Parameter("OFFで元に戻す", "returnOnOff", &Parameter::returnOnOff);
}

void GimmickLiquidLevel::Update(float deltaTime) {
    // 脱出シーケンスだけは通常のスイッチ昇降と別の時間軸で進めます。
    // 旧配置などで設定を持たない場合も、モードや速度を参照する前に既定値を用意する。
    if (!GetParameters().has_value())
        ResetParameters();

    // 脱出モードでは領域によって開始・停止する。通常のスイッチ昇降は行わない。
    if (GetParameters()->actionMode == 1) {
        UpdateEscape(deltaTime);
        return;
    }

    bool isPlaying = false;

    // シーン管理を取得できた場合にプレイ状態を参照する。
    if (SceneManager* sceneManager = SceneManager::GetInstance()) {
        isPlaying = sceneManager->IsPlaying();
    }

    // 編集中は材質だけ適用し、次のプレイ開始で配置位置を取り直せるようにする。
    if (!isPlaying) {
        initializedForPlay_ = false;
        ApplyLiquidVisual();
        BaseGimmick::Update(deltaTime);
        return;
    }

    // 通常昇降をプレイする最初の更新で現在の配置と開始設定を取り込む。
    if (!initializedForPlay_) {
        basePosition_ = GetTransform()->translate;
        targetPosition_ = basePosition_;
        active_ = GetParameters()->startActive;
        ApplyTarget(active_);
        initializedForPlay_ = true;
    }

    // 指数補間を使い、フレーム時間が変わっても目標位置への追従を安定させます。
    float speed = (std::max)(0.01f, GetParameters()->moveSpeed);
    float t = 1.0f - std::exp(-speed * deltaTime);
    engine::Vector3 pos = GetTransform()->translate;
    pos.x = engine::Math::Lerp(pos.x, targetPosition_.x, t);
    pos.y = engine::Math::Lerp(pos.y, targetPosition_.y, t);
    pos.z = engine::Math::Lerp(pos.z, targetPosition_.z, t);
    SetTranslate(pos);

    ApplyLiquidVisual();
    BaseGimmick::Update(deltaTime);
}

void GimmickLiquidLevel::OnTrigger() {
    // 脱出用液面はエリア進行で動くため、通常のトリガー入力を無視します。
    if (GetParameters() && GetParameters()->actionMode == 1)
        return;
    OnSwitchEvent(true);
}

void GimmickLiquidLevel::OnSwitchEvent(bool active) {
    // 通常昇降モードに対してオン・オフの目標を設定する。解除通知を受け付けるかは設定に従う。
    // 開始前に通知が届いても設定を参照できるよう、存在しなければ初期化する。
    if (!GetParameters().has_value())
        ResetParameters();
    // 脱出の上下動は開始マーカーから進めるため、スイッチ通知を使わない。
    if (GetParameters()->actionMode == 1)
        return;
    // オフで戻す設定が無効なら、解除通知でも今の目標を維持する。
    if (!active && !GetParameters()->returnOnOff)
        return;
    ApplyTarget(active);
}

void GimmickLiquidLevel::ApplyTarget(bool active) {
    // 配置時の高さを基準に、設定した距離だけ上下させます。
    active_ = active;
    float amount = GetParameters().has_value() ? GetParameters()->moveAmount : 6.0f;
    targetPosition_ = active ? basePosition_ + engine::Vector3{0.0f, amount, 0.0f} : basePosition_;
}

void GimmickLiquidLevel::ApplyLiquidVisual() {
    // 色種別に合わせて水とマグマの材質・透過色を切り替えます。
    std::int32_t colorType = GetParameters().has_value() ? GetParameters()->colorType : 0;

    // 設定値1ならマグマの材質と赤橙の透過色にする。
    if (colorType == 1) {
        SetMaterialType(9);
        SetColor({1.0f, 0.25f, 0.02f, 0.88f});
    // 設定がない場合やマグマ以外の値では、水の青い透過材質にする。
    } else {
        SetMaterialType(8);
        SetColor({0.45f, 0.85f, 1.0f, 0.65f});
    }
}

std::unique_ptr<engine::Object3D> GimmickLiquidLevel::Clone() const {
    // 複製先の描画資源を作った後で、液面の設定を引き継ぎます。
    auto newObj = std::make_unique<GimmickLiquidLevel>();
    assert(GetObjectCommon() != nullptr);
    newObj->Initialize(GetObjectCommon(), GetModelName());
    newObj->CopyFrom(this);
    return newObj;
}

// 脱出モードは、Target IDから順に開始・終了・復帰マーカーを参照します。
// マーカーは中心とScaleを半径として使い、Editorから範囲を調整できます。
void GimmickLiquidLevel::UpdateEscape(float deltaTime) {
    auto* manager = SceneManager::GetInstance();

    // プレイを終えたら開始高さと脱出の進行を戻し、編集時に上昇を継続しません。
    if (!manager || !manager->IsPlaying()) {
        // プレイ中に開始位置を確保していた場合は、編集中の液面をその高さに戻す。
        if (initializedForPlay_)
            SetTranslate(basePosition_);
        initializedForPlay_ = false;
        escapeStarted_ = escapeFinished_ = false;
        escapeTime_ = 0.0f;
        BaseGimmick::Update(deltaTime);
        return;
    }

    auto* scene = manager->GetCurrentScene();
    // シーンがまだ取得できなければ、マーカーと妨害の探索を行わない。
    if (!scene)
        return;
    // プレイの初回に、上昇の基準となる高さを記録する。
    if (!initializedForPlay_) {
        basePosition_ = GetTranslate();
        initializedForPlay_ = true;
        SetStatic(false);
    }

    Player* player = nullptr;

    for (auto& obj : scene->GetObjects()) {
        // シーン内で最初に見つかったプレイヤーをマーカーの判定に使う。
        if ((player = dynamic_cast<Player*>(obj.get())))
            break;
    }

    // 連番で参照したマーカーを箱の範囲として使い、プレイヤーの位置で開始と終了を判定します。
    auto* start = scene->FindObjectByEventId(GetTargetId());
    auto* finish = scene->FindObjectByEventId(GetTargetId() + 1);
    auto* checkpoint = scene->FindObjectByEventId(GetTargetId() + 2);
    const auto inside = [&](engine::Object3D* marker) {
        // マーカーまたはプレイヤーがなければ、その領域へ入っていないものとして扱う。
        if (!marker || !player)
            return false;
        // 配置座標の各軸の差をScaleで指定した半径と比べ、回転を考慮しない箱の中か調べる。
        const engine::Vector3 p = player->GetTranslate() - marker->GetTranslate();
        const engine::Vector3 radius = marker->GetScale();
        return std::abs(p.x) <= radius.x && std::abs(p.y) <= radius.y && std::abs(p.z) <= radius.z;
    };

    // 生存して操作できるプレイヤーがいる間だけ、開始と終了の進入を受け付ける。
    if (player && !player->IsDead() && player->IsControlActive()) {
        // 開いた入口へ引き返しても上昇は継続。死亡後のScene再生成で初期化します。
        // 開始マーカーに入り、まだ脱出を始めていなければ予告の時計を起動する。
        if (!escapeStarted_ && inside(start)) {
            escapeStarted_ = true;
            escapeTime_ = 0.0f;
            // 任意の復帰マーカーが設定されていれば、その位置をチェックポイントに登録する。
            if (checkpoint)
                player->ActivateCheckpoint(checkpoint->GetTranslate());
            ResetEscapeObjects(true);
            // アクティブカメラが存在する場合にだけ、脱出開始を知らせる揺れを加える。
            if (auto* camera = CameraManager::GetInstance()->GetActiveCamera())
                camera->StartShake(0.65f, 0.10f, 18.0f, {0.4f, 1.0f, 0.4f});
        }

        // 開始済みの脱出で、最初に終了マーカーへ入ったときだけ上昇を止め、到達地点を記録する。
        if (escapeStarted_ && !escapeFinished_ && inside(finish)) {
            escapeFinished_ = true;
            player->ActivateCheckpoint(finish->GetTranslate());

            // 到達後は追撃を止め、頂上のゴールへ安全に進めるようにします。
            for (auto& obj : scene->GetObjects())
                // 液面のイベントIDを参照する妨害だけを停止する。
                if (obj->GetTargetId() == GetEventId()) {
                    // 噴出口は脱出モードの再生を終了する。
                    if (auto* geyser = dynamic_cast<GimmickMagmaGeyser*>(obj.get()))
                        geyser->ResetEscapeCycle(false);
                    // 砲台はオフ通知を送り、追撃を止める。
                    else if (auto* cannon = dynamic_cast<GimmickFireCannon*>(obj.get()))
                        cannon->OnSwitchEvent(false);
                }
        }
    }

    // 開始演出の待ち時間を引いてから一定速度で上げ、配置で指定された高さを超えないようにします。
    if (escapeStarted_ && !escapeFinished_) {
        escapeTime_ += (std::max)(0.0f, deltaTime);
        const float elapsed = (std::max)(0.0f, escapeTime_ - GetParameters()->shakeDuration);
        const float rise =
            (std::min)((std::max)(0.0f, GetParameters()->moveAmount), elapsed * (std::max)(0.01f, GetParameters()->moveSpeed));
        SetTranslate(basePosition_ + engine::Vector3{0.0f, rise, 0.0f});
    }

    // 接触判定はPlayerが現在の液面を読むため、ここで行列まで確定します。
    SetMaterialType(9);
    BaseGimmick::Update(deltaTime);
}

// 接続先の種類に応じて走行・噴火・発射を初期状態にし、スイッチと入口の連番イベントも処理する。
void GimmickLiquidLevel::ResetEscapeObjects(bool active) {
    auto* scene = SceneManager::GetInstance()->GetCurrentScene();
    // シーンがなければ接続先を取得できないため、開始状態を適用しない。
    if (!scene)
        return;
    // 液面に直接リンクした可動床・噴出口・砲台・受信物だけに開始時の状態を適用します。
    for (auto& obj : scene->GetObjects()) {
        // 自分自身、無効な参照、液面のイベントIDに接続されていない物体は除外する。
        if (!obj || obj.get() == this || obj->GetTargetId() != GetEventId())
            continue;
        // 移動床は配置と走行周期を出発前へ戻す。
        if (auto* floor = dynamic_cast<GimmickMovingFloor*>(obj.get()))
            floor->ResetEscapeCycle();
        // 噴出口は脱出サイクルをリセットし、activeで発動の可否を指定する。
        else if (auto* geyser = dynamic_cast<GimmickMagmaGeyser*>(obj.get()))
            geyser->ResetEscapeCycle(active);
        // 砲台にはactiveをそのまま発射可否として通知する。
        else if (auto* cannon = dynamic_cast<GimmickFireCannon*>(obj.get()))
            cannon->OnSwitchEvent(active);
        // イベント受信物はオフ通知で解除する。
        else if (obj->GetGimmickType() == "EventReceiver")
            obj->OnSwitchEvent(false);
    }

    // 出現床のスイッチは独立したリンクなので、受信側IDから同じ組を探します。
    for (auto& obj : scene->GetObjects())
        // 液面のイベントIDに4を加えた接続先が出現床用のスイッチ系統に当たる。
        if (obj->GetTargetId() == GetEventId() + 4)
            // スイッチとして扱えるオブジェクトだけ出力と接触履歴をリセットする。
            if (auto* button = dynamic_cast<GimmickSwitch*>(obj.get()))
                button->ResetEscapeCycle();
    // 入口の岩戸へは、液面のイベントIDに5を加えたIDで状態を通知する。
    scene->SetEventActive(GetEventId() + 5, active);
}
