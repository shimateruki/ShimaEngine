#include "game/actor/player/Player.Internal.h"
#include <numbers>
#include <string_view>
#include "ModelManager.h"
#include "engine/graphics/3d/MeshRenderer.h"

using namespace playerImplementation;

namespace {

constexpr float kPlayerHurtFaceSeconds = 0.18f;

/// <summary>プレイヤーの通常モデルに対応する被弾モデル名と、被弾素材に帯が含まれるかを保持する。</summary>
struct PlayerHurtFaceModel {
    std::string_view baseName;
    std::string_view fileName;
    const char* hurtModel;
    bool includesBand;
};

// 通常モデル名とフルファイル名のどちらでも、同じ被弾素材へ対応づけます。
constexpr PlayerHurtFaceModel kPlayerHurtFaceModels[] = {
    {"Characters/slime", "Characters/slime/slime.gltf", "Characters/slime/slime_hurt.gltf", false},
    {"Characters/slime_pink", "Characters/slime_pink/slime_pink.gltf", "Characters/slime_pink/player_copy_pink_hurt.gltf", true},
    {"Characters/slime_red", "Characters/slime_red/slime_red.gltf", "Characters/slime_red/player_copy_fire_hurt.gltf", true},
    {"Characters/slime_yellow", "Characters/slime_yellow/slime_yellow.gltf", "Characters/slime_yellow/player_copy_thunder_hurt.gltf", true},
    {"Characters/slime_wind", "Characters/slime_wind/slime_wind.gltf", "Characters/slime_wind/player_copy_wind_hurt.gltf", true},
    {"Characters/slime_black", "Characters/slime_black/slime_black.gltf", "Characters/slime_black/player_copy_bomb_hurt.gltf", true},
    {"Characters/slime_crown", "Characters/slime_crown/slime_crown.gltf", "Characters/slime_crown/slime_crown_hurt.gltf", false},
    {"Characters/slime_crown/slime_crown_body.gltf", "Characters/slime_crown/slime_crown_body.gltf",
     "Characters/slime_crown/slime_crown_body_hurt.gltf", false},
};

} // namespace

void Player::PrepareHurtFaceModel() {
    engine::Model* baseModel = GetModel();
    // 現在の本体モデルが前回と同じなら、被弾顔の検索結果を再利用する。
    if (hurtFaceBaseModel_ == baseModel) {
        return;
    }

    // コピー解除などで本体が変わった場合は、前の形態の被弾顔を持ち越さない。
    ClearHurtFace();
    hurtFaceBaseModel_ = baseModel;
    hurtFaceModel_ = nullptr;
    hurtFaceIncludesBand_ = false;
    const std::string modelName = GetModelName();
    for (const auto& model : kPlayerHurtFaceModels) {
        // 登録の通常名かフルファイル名が一致した場合だけ、対応する被弾素材を読み込む。
        if (modelName == model.baseName || modelName == model.fileName) {
            hurtFaceModel_ = ModelManager::GetInstance()->LoadModel(model.hurtModel);
            hurtFaceIncludesBand_ = model.includesBand;
            break;
        }
    }
}

// 現在の形態で使える被弾顔を選び、0.18秒の表示を開始します。
void Player::StartHurtFace() {
    PrepareHurtFaceModel();
    // 素材が無い、HPが0以下、死亡済み、演出拘束中、または変身解除中なら被弾顔を表示しない。
    if (!hurtFaceModel_ || GetHp() <= 0.0f || IsDead() || isCinematicLocked_ || enemyMorphReleaseActive_) {
        ClearHurtFace();
        return;
    }

    hurtFaceRemaining_ = kPlayerHurtFaceSeconds;
    UpdateHurtFace(0.0f);
}

// 正の経過秒数だけ表示時間を減らし、本体の表示枠が空いたときに被弾顔を反映します。
void Player::UpdateHurtFace(float deltaTime) {
    PrepareHurtFaceModel();
    // 表示の残り時間が無ければ、モデルの再設定を行わない。
    if (hurtFaceRemaining_ <= 0.0f) {
        return;
    }
    // 正の経過秒数だけ、被弾顔の残り時間から引く。
    hurtFaceRemaining_ -= (std::max)(0.0f, deltaTime);
    // 表示期限か、死亡・拘束・変身解除・ゲート復帰へ達したら被弾顔を解除する。
    if (hurtFaceRemaining_ <= 0.0f || GetHp() <= 0.0f || IsDead() || isCinematicLocked_ ||
        enemyMorphReleaseActive_ || gateReturnAnimation_.IsActive()) {
        ClearHurtFace();
        return;
    }

    // ムチ変形などが表示枠を使っている間は、終了後の更新まで差し替えを待つ。
    if (auto* renderer = GetMeshRenderer(); renderer && !renderer->GetVisualModelOverride()) {
        renderer->SetVisualModelOverride(hurtFaceModel_);
    }
}

// 被弾顔の表示時間を0にし、現在の表示枠をこの被弾顔が使っている場合に限り取り除きます。
void Player::ClearHurtFace() {
    hurtFaceRemaining_ = 0.0f;
    // 表示枠にこの被弾顔が残っている場合にだけ消し、別の変形が入れたモデルは残します。
    if (auto* renderer = GetMeshRenderer(); hurtFaceModel_ && renderer &&
        renderer->GetVisualModelOverride() == hurtFaceModel_) {
        renderer->SetVisualModelOverride(nullptr);
    }
}

void Player::StartDamageFeedback(const engine::Vector3& knockbackDirection, float invincibleDuration) {
    // チュートリアルの保護中、死亡中、演出拘束中、または感電拘束中は通常の被弾状態へ入らない。
    if (tutorialSafetyEnabled_ || IsDead() || isCinematicLocked_ || electricShockControlLocked_) {
        return;
    }

    // 通常攻撃を管理している場合は、被弾で攻撃を中断する。
    if (baseCombatController_) {
        baseCombatController_->Cancel(*this, false);
    }

    // コピー能力がある場合は、被弾状態へ入る前に通知する。どの技を中断するかは能力自身が判定する。
    if (copyAbilityController_)
        copyAbilityController_->NotifyDamageStarted(*this);
    // 既存の被弾無敵秒数と新しい指定値のうち、長い方を維持する。
    damageCooldownTimer_ = (std::max)(damageCooldownTimer_, (std::max)(0.0f, invincibleDuration));
    SetDamageInvincible(damageCooldownTimer_ > 0.0f);
    ChangeState(std::make_unique<PlayerStateDamage>(knockbackDirection));
}

// 感電拘束を開始または延長します。解除後に付与する無敵秒数は、拘束中の時間と別に保持します。
void Player::StartElectricShockFeedback(float duration, float invincibleDuration) {
    // チュートリアルの保護を有効にしている間は、感電拘束も無敵の予約も行わない。
    if (tutorialSafetyEnabled_) {
        return;
    }

    duration = (std::max)(0.0f, duration);
    electricShockPendingInvincibleDuration_ =
        (std::max)(electricShockPendingInvincibleDuration_, (std::max)(0.0f, invincibleDuration));

    // 拘束時間が0以下なら、新たに拘束せず解除側で無敵の予約を処理する。
    if (duration <= 0.0f) {
        EndElectricShockFeedback();
        return;
    }

    // コピー攻撃の管理が存在する場合は、感電で操作を止める前に攻撃側へ被弾を通知する。
    if (copyAbilityController_)
        copyAbilityController_->NotifyDamageStarted(*this);
    RestoreDamageBlinkVisibility();

    // 初回の拘束時にだけ位置・姿勢・操作可否を保存し、再感電では元の復帰先を保つ。
    if (!electricShockControlLocked_) {
        electricShockWasControlActive_ = isControlActive_;
        electricShockLockedPosition_ = GetTranslate();
        electricShockBaseScale_ = GetScale();
        electricShockBaseRotation_ = GetRotation();
        electricShockControlLocked_ = true;
    }

    // 感電の追加は残り時間を短縮せず、解除まで入力と速度を止めます。
    electricShockFeedbackTimer_ = (std::max)(electricShockFeedbackTimer_, duration);
    electricShockFeedbackTotalDuration_ = (std::max)(electricShockFeedbackTotalDuration_, electricShockFeedbackTimer_);
    electricShockFeedbackEmitTimer_ = 0.0f;

    SetIsControlActive(false);
    SetVelocity({0.0f, 0.0f, 0.0f});
    SetTranslate(electricShockLockedPosition_);
    UpdateElectricShockAuraEffect(1.0f / 60.0f);
}

void Player::UpdateElectricShockFeedback(float deltaTime) {
    // 感電の残り時間が無いか経過秒数が0以下なら、揺れや粒子の時計を進めない。
    if (electricShockFeedbackTimer_ <= 0.0f || deltaTime <= 0.0f) {
        return;
    }

    const float previousShockTimer = electricShockFeedbackTimer_;
    electricShockFeedbackTimer_ = (std::max)(0.0f, electricShockFeedbackTimer_ - deltaTime);
    electricShockFeedbackEmitTimer_ -= deltaTime;

    // 開始からの実時間を使い、変身色へ感電の点滅を重ねます。
    const float totalDuration = (std::max)(electricShockFeedbackTotalDuration_, 0.001f);
    const float elapsed = (std::max)(0.0f, totalDuration - electricShockFeedbackTimer_);
    const float flicker = std::sin(elapsed * 74.0f);
    const engine::Vector4 base = isEnemyMorphed_ ? GetEnemyMorphTint(enemyMorphType_) : engine::Vector4{1.0f, 1.0f, 1.0f, 1.0f};
    SetColor({(std::clamp)(base.x + 0.24f * std::max(0.0f, flicker), 0.0f, 1.0f),
              (std::clamp)(base.y + 0.18f, 0.0f, 1.0f),
              (std::clamp)(base.z + 0.42f * std::max(0.0f, -flicker), 0.0f, 1.0f), base.w});

    // 拘束中は元の位置・大きさ・回転を基準に、速度を0に保ったまま揺れを表現する。
    if (electricShockControlLocked_) {
        engine::Vector3 shockPosition = electricShockLockedPosition_;
        shockPosition.x += std::sin(elapsed * 94.0f) * kElectricShockShakeAmount;
        shockPosition.z += std::cos(elapsed * 83.0f) * kElectricShockShakeAmount * 0.75f;
        SetTranslate(shockPosition);
        SetVelocity({0.0f, 0.0f, 0.0f});

        const float squash = 0.055f + std::abs(std::sin(elapsed * 46.0f)) * 0.075f;
        SetScale({electricShockBaseScale_.x * (1.0f + squash), electricShockBaseScale_.y * (1.0f - squash * 0.65f),
                  electricShockBaseScale_.z * (1.0f + squash * 0.9f)});

        engine::Vector3 shockRotation = electricShockBaseRotation_;
        shockRotation.x += std::sin(elapsed * 67.0f) * 0.045f;
        shockRotation.z += std::sin(elapsed * 89.0f) * 0.075f;
        SetRotation(shockRotation);
    }

    // 身体の姿勢を確定してからオーラを更新し、間隔を設けて外周の粒子を足します。
    UpdateElectricShockAuraEffect(deltaTime);

    // 外周粒子の残り間隔が0以下なら粒子を出し、次を0.045秒後にする。
    if (electricShockFeedbackEmitTimer_ <= 0.0f) {
        EmitOuterThunderParticles(GetWorldPosition(), GetScale(), elapsed * 3.4f, 5);
        electricShockFeedbackEmitTimer_ = 0.045f;
    }

    // 今回の更新で正の残り時間から0に達した場合だけ、感電の解除を呼ぶ。
    if (previousShockTimer > 0.0f && electricShockFeedbackTimer_ <= 0.0f) {
        EndElectricShockFeedback();
    }
}

void Player::EndElectricShockFeedback() {
    // 拘束していた場合だけ、保存した位置・大きさ・回転を戻す。死亡中は操作を再開しない。
    if (electricShockControlLocked_) {
        SetTranslate(electricShockLockedPosition_);
        SetVelocity({0.0f, 0.0f, 0.0f});
        SetScale(electricShockBaseScale_);
        SetRotation(electricShockBaseRotation_);
        SetIsControlActive(!IsDead() && electricShockWasControlActive_);
        electricShockControlLocked_ = false;
    }

    // 感電の時計とオーラを片付けて通常色に戻し、その後で解除時に付ける無敵を適用する。
    electricShockFeedbackTimer_ = 0.0f;
    electricShockFeedbackEmitTimer_ = 0.0f;
    electricShockFeedbackTotalDuration_ = 0.0f;
    HideElectricShockAuraEffect();
    UpdateColor();

    // 予約した無敵秒数はここで一度だけ消費します。中断側が予約をゼロにした場合は無敵を付与しません。
    const float invincibleDuration = electricShockPendingInvincibleDuration_;
    electricShockPendingInvincibleDuration_ = 0.0f;

    // 有効な無敵秒数があり、プレイヤーが生存している場合にだけ被弾無敵へ反映する。
    if (invincibleDuration > 0.0f && !IsDead()) {
        damageCooldownTimer_ = (std::max)(damageCooldownTimer_, invincibleDuration);
        invincibleBlinkTimer_ = 0.0f;
        SetDamageInvincible(true);
    }
}

void Player::InitializeElectricShockAuraEffect() {
    // 描画基盤が無いかオーラを既に作っている場合は、新たに生成しない。
    if (!GetObjectCommon() || electricShockAuraEffect_) {
        return;
    }

    electricShockAuraEffect_ = std::make_unique<EffectObject3D>();
    electricShockAuraEffect_->Initialize(GetObjectCommon());
    electricShockAuraEffect_->SetName("Player_ElectricShockAura");

    // オーラのエフェクト設定を読めなければ、作りかけのオーラを破棄する。
    if (!electricShockAuraEffect_->LoadFromJson(kElectricShockAuraEffectPath)) {
        electricShockAuraEffect_.reset();
        return;
    }

    electricShockAuraEffect_->SetIsVisible(false);
    electricShockAuraEffect_->Play(99999.0f);
}

void Player::UpdateElectricShockAuraEffect(float deltaTime) {
    // オーラが未作成なら、この更新で描画資源の初期化を試す。
    if (!electricShockAuraEffect_) {
        InitializeElectricShockAuraEffect();
    }

    // 初期化してもオーラが無ければ、表示や姿勢を更新できない。
    if (!electricShockAuraEffect_) {
        return;
    }

    engine::Vector3 auraPos{};
    float horizontalDiameter = 1.0f;
    float verticalDiameter = 1.0f;
    CalculateElectricShockAuraShape(auraPos, horizontalDiameter, verticalDiameter);

    float yaw = GetRotation().y;
    engine::Camera* camera = CameraManager::GetInstance() ? CameraManager::GetInstance()->GetActiveCamera() : nullptr;

    // カメラを取得できた場合は、オーラから視点への向きを調べる。
    if (camera) {
        const engine::Vector3 toCamera = camera->GetEye() - auraPos;

        // 視点に対する水平成分が十分にあれば、ヨーをカメラ側へ向ける。
        if (std::abs(toCamera.x) + std::abs(toCamera.z) > 0.001f) {
            yaw = std::atan2(toCamera.x, toCamera.z);
        }
    }

    // 確定した中心・回転・寸法をセットし、描画用の行列を更新する。
    electricShockAuraEffect_->SetIsVisible(true);
    electricShockAuraEffect_->Update(deltaTime);
    electricShockAuraEffect_->SetTranslate(auraPos);
    electricShockAuraEffect_->SetRotation({(std::numbers::pi_v<float> * 0.5f), yaw, 0.0f});
    electricShockAuraEffect_->SetScale({horizontalDiameter, 1.0f, verticalDiameter});
    electricShockAuraEffect_->UpdateLocalMatrix();
    electricShockAuraEffect_->UpdateWorldMatrix();
}

void Player::HideElectricShockAuraEffect() {
    // オーラが作成済みの場合だけ表示を止め、次の感電に再利用する。
    if (electricShockAuraEffect_) {
        electricShockAuraEffect_->SetIsVisible(false);
    }
}

void Player::CalculateElectricShockAuraShape(engine::Vector3& center, float& horizontalDiameter,
                                             float& verticalDiameter) const {
    // 変身中の拡縮を反映しつつ、極小サイズでもオーラが消えない下限を使う。
    const engine::Vector3 scale = GetScale();
    const float maxXZ = (std::max)({std::abs(scale.x), std::abs(scale.z), 1.0f});
    const float yScale = (std::max)(std::abs(scale.y), 0.18f);

    horizontalDiameter = (std::max)(2.55f, maxXZ * 1.35f);
    verticalDiameter = (std::max)(0.46f, yScale * 1.28f);

    center = GetWorldPosition();
    center.y += (std::max)(0.22f, yScale * 0.43f);
}

// 敵の登録タイプ名をコピー形態へ対応させます。対象外や未登録の名前はkNoneを返します。
Player::EnemyMorphType Player::ResolveEnemyMorphType(const std::string& enemyType) const {
    // 王冠タイプは王冠形態にする。
    if (enemyType == "Crown")
        return EnemyMorphType::kCrown;
    // Bomberは爆弾型のコピーにする。
    if (enemyType == "Bomber")
        return EnemyMorphType::kBomber;

    // 巨大スライムはフックで分裂して小型化するため、直接コピー対象にはしません。
    if (enemyType == "GiantSlime")
        return EnemyMorphType::kNone;
    // 火のスライムは炎形態にする。
    if (enemyType == "FireSlime")
        return EnemyMorphType::kFireSlime;
    // 雷のスライムは雷形態にする。
    if (enemyType == "ThunderSlime")
        return EnemyMorphType::kThunderSlime;
    // 風のスライムは風形態にする。
    if (enemyType == "WindSlime")
        return EnemyMorphType::kWindSlime;
    // 通常のSlimeはピンク形態にする。
    if (enemyType == "Slime")
        return EnemyMorphType::kSlime;
    return EnemyMorphType::kNone;
}

engine::Vector4 Player::GetEnemyMorphTint(EnemyMorphType type) const {
    // 感電点滅の基準色を能力ごとに変え、解除後の外観と一致させる。
    switch (type) {
    // 王冠形態は明るい水色を基準にする。
    case EnemyMorphType::kCrown:
        return {0.58f, 0.89f, 1.0f, 1.0f};

    // 爆弾形態は橙色を基準にする。
    case EnemyMorphType::kBomber:
        return {1.0f, 0.45f, 0.22f, 1.0f};

    // 炎形態は赤色を基準にする。
    case EnemyMorphType::kFireSlime:
        return {1.0f, 0.24f, 0.16f, 1.0f};

    // 雷形態は黄色を基準にする。
    case EnemyMorphType::kThunderSlime:
        return {1.0f, 0.92f, 0.18f, 1.0f};

    // 風形態は緑がかった水色を基準にする。
    case EnemyMorphType::kWindSlime:
        return {0.42f, 1.0f, 0.82f, 1.0f};

    // 通常のコピー形態はピンクを基準にする。
    case EnemyMorphType::kSlime:
        return {1.0f, 0.46f, 0.86f, 1.0f};

    // 対応するコピー形態が無ければ白を基準にする。
    default:
        return {1.0f, 1.0f, 1.0f, 1.0f};
    }
}
