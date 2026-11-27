#include <cstdint>
#include "GimmickSwitch.h"
#include "ObjectPropertyDefinition.h"
#include "CollisionConfig.h"
#include "SceneManager.h"
#include <cmath>

// 踏める押し板と、接続先へ渡す出力方式を初期化します。
void GimmickSwitch::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    BaseGimmick::Initialize(common, modelName);

    SetClassName("Gimmick");
    SetGimmickType("Switch");
    SetName("Gimmick_Switch");
    SetCollisionAttribute(engine::collisionCategories::Get("ground"));
    SetCollisionMask(engine::collisionCategories::GetDefaultGameplayMask());

    // 押し板の石・金属・中央ボタンの素材色を保ちながら、起動状態を色で伝えます。
    SetColor({0.85f, 0.94f, 1.0f, 1.0f});
    SetScale({1.5f, 0.25f, 1.5f});

    // 方式と有効時間を保持する共通パラメーターが無ければ作成する。
    if (!GetParameters().has_value())
        ResetParameters();

    // 方式を4種類から選べるようにし、時限方式の場合だけ有効時間を表示する。
    using Self = GimmickSwitch;
    engine::ObjectPropertyDefinition<Self> properties(*this, kTypeId, "汎用スイッチ",
        "Target IDと受信側のMy Event IDを合わせてください。");
    using Parameter = engine::Object3D::EntityParameter;
    properties.Parameter("スイッチ方式", "switchMode", &Parameter::switchMode, 0.0f, 3.0f,
        nullptr, {"押している間だけ", "押すたび切替", "一定時間だけ", "一度押すと保持"});
    properties.Parameter("有効時間", "interval", &Parameter::interval, 0.1f, 60.0f,
        +[](const Self& object) { return object.GetParameters() && (object.GetParameters()->switchMode == 2); });
}

void GimmickSwitch::Update(float deltaTime) {
    // 配置パラメーターが無い場合は、方式や時間を取得できる状態にする。
    if (!GetParameters().has_value())
        ResetParameters();

    // 接触猶予が正の場合だけ残り秒数を減らす。
    if (contactTimer_ > 0.0f) {
        contactTimer_ -= deltaTime;
        // 残り秒数が0を下回ったら0で止める。
        if (contactTimer_ < 0.0f)
            contactTimer_ = 0.0f;
    }

    // 現在の接触と前フレームの接触を比較し、押し続けている間の連続切り替えを防ぎます。
    bool isPressed = contactTimer_ > 0.0f;
    bool pressedThisFrame = isPressed && !wasPressed_;
    std::int32_t mode = GetSwitchMode();

    // 押下中だけ有効にする方式では、現在の押下をそのまま出力へ渡す。
    if (mode == 0) {
        SendActive(isPressed);
    // トグル方式では、押し始めだけ出力を反転する。
    } else if (mode == 1) {
        // 押し始めのフレームだけ出力を反転する。
        if (pressedThisFrame) {
            SendActive(!isOutputActive_);
        }
    // 一度押すと保持する方式では、押し始めから有効を維持する。
    } else if (mode == 3) {
        // 押し始めで出力を有効にする。離れてもこの方式の中では解除しない。
        if (pressedThisFrame)
            SendActive(true);
    // ほかの方式に当たらない場合は、時限出力として満了まで管理する。
    } else {
        // 新たに踏んだときだけ時間を設定して起動する。設定時間が0以下なら3秒を使う。
        if (pressedThisFrame) {
            timedTimer_ = GetParameters()->interval > 0.0f ? GetParameters()->interval : 3.0f;
            SendActive(true);
        }

        // 時限出力の残りがあれば、今フレームの秒数を減らす。
        if (timedTimer_ > 0.0f) {
            timedTimer_ -= deltaTime;

            // この減算で満了した場合は、接続先へ解除を送る。
            if (timedTimer_ <= 0.0f) {
                timedTimer_ = 0.0f;
                SendActive(false);
            }
        }
    }

    // 次の押し始めの判定に使う履歴を残し、見た目には押下と出力を別の色で反映します。
    wasPressed_ = isPressed;

    // 接続先への出力が有効なら、緑のパルスで示す。
    if (isOutputActive_) {
        const float pulse = 0.86f + std::sin(timedTimer_ * 10.0f + contactTimer_ * 30.0f) * 0.06f;
        SetColor({0.72f * pulse, 1.0f, 0.78f * pulse, 1.0f});
    // 出力は無効でも押下状態なら白で示す。
    } else if (isPressed) {
        SetColor({1.0f, 1.0f, 1.0f, 1.0f});
    // 押下も出力も無ければ待機色へ戻す。
    } else {
        SetColor({0.85f, 0.94f, 1.0f, 1.0f});
    }

    BaseGimmick::Update(deltaTime);
}

bool GimmickSwitch::OnCollision(engine::Object3D* other) {
    // プレイヤー以外との接触は押下に数えない。
    if (!other || other->GetClassName() != "Player")
        return true;

    engine::CollisionInfo info = CheckCollision(other);

    // 上面への接触が成立した場合だけ、0.12秒の接触猶予を更新する。
    if (info.isColliding && info.normal.y < -0.5f) {
        contactTimer_ = 0.12f;
    }

    return true;
}

void GimmickSwitch::SendActive(bool active) {
    // 状態変更を一度だけイベントとして配信し、連続接触での重複発火を防ぎます。
    if (isOutputActive_ == active)
        return;

    isOutputActive_ = active;
    std::int32_t targetId = GetTargetId();
    // 宛先の設定がなければ、自分の内部状態を保存したところで終了する。
    if (targetId == -1)
        return;

    // 現在のシーンを取得できる場合だけ、接続先の有効状態を変更する。
    if (BaseScene* scene = SceneManager::GetInstance()->GetCurrentScene()) {
        scene->SetEventActive(targetId, active);
    }
}

std::int32_t GimmickSwitch::GetSwitchMode() const {
    // 未設定の配置物は通常モードとして扱う。
    return GetParameters().has_value() ? GetParameters()->switchMode : 0;
}

std::unique_ptr<engine::Object3D> GimmickSwitch::Clone() const {
    // スイッチ固有の設定も含めて配置物を複製する。
    auto newObj = std::make_unique<GimmickSwitch>();
    assert(GetObjectCommon() != nullptr);
    newObj->Initialize(GetObjectCommon(), GetModelName());
    newObj->CopyFrom(this);
    return newObj;
}

void GimmickSwitch::ResetEscapeCycle() {
    // 再挑戦時は押下と時間制限を戻し、接続先へ解除を通知する。
    contactTimer_ = timedTimer_ = 0.0f;
    wasPressed_ = false;
    SendActive(false);
}
