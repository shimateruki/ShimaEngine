#include "GimmickChikuwaBlock.h"
#include "ObjectPropertyDefinition.h"
#include "CollisionConfig.h"
#include "Math.h"
#include <random>

void GimmickChikuwaBlock::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    // 配置位置を復帰先として保持し、待機中にプレイヤーを支える地面として登録します。
    BaseGimmick::Initialize(common, modelName);
    SetClassName("Gimmick");
    SetGimmickType("ChikuwaBlock");

    startPos_ = GetTransform()->translate;

    // 設定を持たない配置には、震え・落下・復帰時間の格納先を用意する。
    if (!GetParameters().has_value())
        ResetParameters();

    SetCollisionAttribute(engine::collisionCategories::Get("ground"));

    // 落下前の予告と復帰に使う秒数、落下中の重力をInspectorに登録する。
    using Self = GimmickChikuwaBlock;
    engine::ObjectPropertyDefinition<Self> properties(*this, kTypeId, "ちくわブロック",
        "");
    using Parameter = engine::Object3D::EntityParameter;
    properties.Parameter("震え時間", "shakeDuration", &Parameter::shakeDuration, 0.0f, 10.0f);
    properties.Parameter("落下時間", "fallDuration", &Parameter::fallDuration, 0.0f, 10.0f);
    properties.Parameter("リスポーン間隔", "interval", &Parameter::interval, 0.0f, 10.0f);
    properties.Parameter("落下重力", "gravity", &Parameter::gravity, 0.0f, 200.0f);
}

// 待機・震え・落下・非表示の時計を進め、遷移時の表示と衝突は ChangeState() で切り替える。
void GimmickChikuwaBlock::Update(float deltaTime) {
    switch (state_) {
    // 待機中は現在の配置の平行移動成分を更新し、落下後に戻る位置にする。
    case State::kIdle:
        startPos_ = GetTransform()->translate;
        break;

    // 足場のまま横方向だけ震わせ、予告時間が尽きたら落下へ移します。
    case State::kShaking:
        timer_ += deltaTime;

        // 復帰位置を基準に、水平二軸へ各0.05以内の揺れを加える。
        {
            float intensity = 0.05f;
            float offX = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * intensity;
            float offZ = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * intensity;
            SetTranslateX(startPos_.x + offX);
            SetTranslateZ(startPos_.z + offZ);
        }

        // 予告の経過が設定秒数に達したら、床を無効にして落下へ移る。
        if (timer_ >= GetShakeDuration()) {
            ChangeState(State::kFalling);
        }

        break;

    // 毎フレーム鉛直速度へ重力を積分し、落下時間が尽きたら表示を消す段階へ進みます。
    case State::kFalling:
        timer_ += deltaTime;
        velocityY_ -= GetGravity() * deltaTime;
        SetTranslateY(GetTranslate().y + velocityY_ * deltaTime);

        // 落下中は経過秒数に応じてX軸とZ軸の回転を加える。
        SetEulerX(GetRotation().x + 2.0f * deltaTime);
        SetEulerZ(GetRotation().z + 1.0f * deltaTime);

        // 設定の落下時間を使い切ったら表示を消す段階へ移る。
        if (timer_ >= GetFallDuration()) {
            ChangeState(State::kHidden);
        }

        break;

    // 非表示のまま復帰時間を待ち、開始地点に戻す処理を ChangeState() に任せます。
    case State::kHidden:
        timer_ += deltaTime;

        // 非表示の経過が復帰時間に達したら、元の場所へ戻す。
        if (timer_ >= GetRespawnDuration()) {
            ChangeState(State::kIdle);
        }

        break;
    }

    BaseGimmick::Update(deltaTime);
}

bool GimmickChikuwaBlock::OnCollision(engine::Object3D* other) {
    // 待機中だけ、踏まれて震えを始めるか判定する。
    if (state_ == State::kIdle) {
        // プレイヤーとの接触の場合だけ、当たり方を調べる。
        if (other->GetClassName() == "Player") {
            engine::CollisionInfo info = CheckCollision(other);

            // 判定形状の重なりがある場合だけ、法線を確認する。
            if (info.isColliding) {
                // 床が下向きへ押される法線なら、上面に乗ったとみなす。
                if (info.normal.y < -0.5f) {
                    ChangeState(State::kShaking);
                }
            }
        }
    }

    return true;
}

void GimmickChikuwaBlock::ChangeState(State newState) {
    // 各段階の時計は遷移時にリセットし、衝突や表示をその場で切り替えます。
    state_ = newState;
    timer_ = 0.0f;

    switch (state_) {
    // 復帰時は衝突と表示を戻し、次の落下に前回の回転や速度を持ち越しません。
    case State::kIdle:
        SetIsVisible(true);
        SetCollisionAttribute(engine::collisionCategories::Get("ground"));
        SetTranslate(startPos_);
        SetEulerAngles({0, 0, 0});
        velocityY_ = 0.0f;
        break;

    case State::kShaking:
        // 予告中は表示と地面属性を維持し、足場から離れる猶予を残します。
        break;

    case State::kFalling:
        // 地面属性を外し、落下中は上に乗れないようにする。
        SetCollisionAttribute(0);
        break;

    // 復帰待ちの間は描画と衝突属性を無効にする。
    case State::kHidden:
        SetIsVisible(false);
        SetCollisionAttribute(0);
        break;
    }
}

std::unique_ptr<engine::Object3D> GimmickChikuwaBlock::Clone() const {
    // 同じモデルで新しい実体を初期化してから、配置と設定値を複製する。
    auto newObj = std::make_unique<GimmickChikuwaBlock>();
    assert(GetObjectCommon() != nullptr);
    newObj->Initialize(GetObjectCommon(), GetModelName());
    newObj->CopyFrom(this);
    return newObj;
}
