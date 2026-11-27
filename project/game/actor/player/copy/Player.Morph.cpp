#include "game/actor/player/Player.Internal.h"
#include <cstdint>
#include "Easing.h"

#include "VfxSequencer.h"

using namespace playerImplementation;

namespace {
// 削除された練習相手を、運搬・吸収の演出から参照し続けない。
bool IsTutorialEnemyPresent(const engine::Object3D* enemy) {
    const auto* scene = SceneManager::GetInstance()->GetCurrentScene();
    // 現在のシーンが無い場合は、借用中の敵がまだ有効だと判断できない。
    if (!scene) return false;
    const auto& objects = scene->GetObjects();
    return std::any_of(objects.begin(), objects.end(), [enemy](const auto& object) { return object.get() == enemy && !object->IsReplayRemoved(); });
}
} // namespace

// 捕獲時の保存値、通常吸収の時計、コピー形態と常時演出を管理する。
// 敵の運搬・吸収からコピー開始までを扱います。途中キャンセルでも敵とプレイヤーの見た目を戻せる順序にします。

void Player::SetCarriedEnemy(engine::Object3D* enemy) {
    // 敵を新たに指定する場合は、以前の照準を解除して運搬を優先する。
    if (enemy) {
        // 照準中に捕獲した場合も、その場でマーカーと対象を破棄する。
        // 照準マーカーが生成済みなら非表示にする。
        if (hookMarker_)
            hookMarker_->SetIsVisible(false);
        aimTargetObject_ = nullptr;
    }
    if (auto* camera = CameraManager::GetInstance()->GetMainCamera(); camera && !IsEditorInternal()) {
        // コピー側の抑制を保ちつつ、運搬中は照準用の一人称視点へ切り替えない。
        camera->SetAimCameraSuppressed(enemy || isEnemyMorphed_ || enemyMorphReleaseActive_ || absorbEffectActive_);
    }

    // 抱えた時の基準サイズと姿勢を保存し、見た目の縮小を解除時に戻せるようにする。
    // 同じ敵であれば運搬先を差し替えず、無い場合だけ姿勢の保存値を補う。
    if (carriedEnemy_ == enemy) {
        // 敵が同じでも基準を未取得なら、吸収前の大きさと姿勢をまだ保存できる。
        if (enemy && !hasCarriedEnemyBaseScale_) {
            carriedEnemyBaseScale_ = GetStableEnemyCarryScale(enemy);
            const Transform* enemyTransform = enemy->GetTransform();

            // 敵の変形を取得できた場合にだけ回転方式とその姿勢を控える。
            if (enemyTransform) {
                carriedEnemyBaseRotation_ = enemyTransform->rotate;
                carriedEnemyBaseQuaternion_ = enemyTransform->quaternion;
                carriedEnemyBaseQuaternionMaster_ = enemyTransform->isQuaternionMaster;
            }

            hasCarriedEnemyBaseScale_ = true;
        }

        return;
    }

    carriedEnemy_ = enemy;

    // 新しい敵が指定された場合は、その個体の初期サイズと姿勢を記録する。
    if (enemy) {
        carriedEnemyBaseScale_ = GetStableEnemyCarryScale(enemy);
        const Transform* enemyTransform = enemy->GetTransform();

        // 変形がある場合に、解放時に戻すオイラー角・クォータニオン・優先方式を保存する。
        if (enemyTransform) {
            carriedEnemyBaseRotation_ = enemyTransform->rotate;
            carriedEnemyBaseQuaternion_ = enemyTransform->quaternion;
            carriedEnemyBaseQuaternionMaster_ = enemyTransform->isQuaternionMaster;
        }

        hasCarriedEnemyBaseScale_ = true;
    // 所持を解除した場合は、前の敵の大きさや向きを次の捕獲へ流用しないよう初期化する。
    } else {
        carriedEnemyBaseScale_ = {1.0f, 1.0f, 1.0f};
        carriedEnemyBaseRotation_ = {0.0f, 0.0f, 0.0f};
        carriedEnemyBaseQuaternion_ = {0.0f, 0.0f, 0.0f, 1.0f};
        carriedEnemyBaseQuaternionMaster_ = true;
        hasCarriedEnemyBaseScale_ = false;
    }
}

void Player::ReleaseCarriedEnemy(bool restorePose) {
    // 吸収途中なら先に演出を止め、保持敵の姿勢を指定に従って復元する。
    // チュートリアルでは工程切替で練習用の敵が除去されるため、吸収を片付ける前に借用参照を確認します。
    if (tutorialSafetyEnabled_) {
        // 吸収対象が練習シーンから取り除かれていれば、先に借用参照を外す。
        if (pendingAbsorbEnemy_ && !IsTutorialEnemyPresent(pendingAbsorbEnemy_)) pendingAbsorbEnemy_ = nullptr;
        // 運搬対象が取り除かれている場合も、照準と保存姿勢を含む所持状態を解除する。
        if (carriedEnemy_ && !IsTutorialEnemyPresent(carriedEnemy_)) SetCarriedEnemy(nullptr);
    }
    // 吸収中の敵を解放する場合は、演出の中断と外観の復元を先に行う。
    if (absorbEffectActive_) {
        CancelAbsorbEffect(restorePose);
    }

    engine::Object3D* enemy = carriedEnemy_;

    // 運搬対象をすでに手放している場合は、衝突と姿勢の復元を行わない。
    if (!enemy) {
        return;
    }

    if (auto* enemyBase = dynamic_cast<BaseEnemy*>(enemy)) {
        // 敵クラスが自身の衝突設定や復帰時間を戻せるよう、解放の通知は表示姿勢の復元より先に行います。
        enemyBase->SetCarried(false);
    // 敵クラスとして解決できない捕獲物は、汎用の敵向け接触カテゴリへ戻す。
    } else {
        enemy->SetCollisionAttribute(engine::collisionCategories::Get("enemy"));
        enemy->SetCollisionMask(engine::collisionCategories::Get("player") | engine::collisionCategories::Get("ground") | engine::collisionCategories::Get("player_bullet") | engine::collisionCategories::Get("player_attack"));
    }

    // 運搬中の非表示を解除し、必要なら捕まえる前の姿勢と大きさを復元する。
    enemy->SetIsVisible(true);

    // 姿勢の復元を要求された場合にだけ、捕獲時の大きさと回転を行列まで戻す。
    if (restorePose) {
        enemy->SetScale(hasCarriedEnemyBaseScale_ ? carriedEnemyBaseScale_ : enemy->GetScale());
        enemy->SetEulerAngles(carriedEnemyBaseRotation_);
        enemy->SetQuaternion(carriedEnemyBaseQuaternion_);
        enemy->SetQuaternionMaster(carriedEnemyBaseQuaternionMaster_);
        enemy->UpdateLocalMatrix();
        enemy->UpdateWorldMatrix();
    }

    // 捕獲時の姿勢を使い終えた後で参照を外します。SetCarriedEnemy() はその姿勢の保存値も初期化します。
    SetCarriedEnemy(nullptr);
}

bool Player::ShouldPlayAbsorbEffect(BaseEnemy* enemy) const {
    // 個別の吸い込み演出を使うのは通常スライムと炎スライム。ほかの対応形態はその場で変身する。
    if (!enemy) {
        return false;
    }

    const EnemyMorphType type = ResolveEnemyMorphType(enemy->GetEnemyType());
    return type == EnemyMorphType::kSlime || type == EnemyMorphType::kFireSlime;
}

void Player::BeginAbsorbEffect(BaseEnemy* enemy) {
    // 変身先と敵の元状態を記録してから、吸収アニメーションを開始する。
    // 吸収元が渡されていなければ、演出と能力選択を開始しない。
    if (!enemy) {
        return;
    }

    pendingAbsorbType_ = ResolveEnemyMorphType(enemy->GetEnemyType());

    // 専用演出のない変身先は、待ち時間を置かず成立処理へ進む。
    if (pendingAbsorbType_ != EnemyMorphType::kSlime && pendingAbsorbType_ != EnemyMorphType::kFireSlime) {
        StartEnemyMorph(enemy);
        return;
    }

    pendingAbsorbEnemy_ = enemy;
    pendingAbsorbEnemyBaseScale_ = hasCarriedEnemyBaseScale_ ? carriedEnemyBaseScale_ : GetStableEnemyCarryScale(enemy);
    pendingAbsorbEnemyStartPos_ = enemy->GetWorldPosition();
    pendingAbsorbTint_ = pendingAbsorbType_ == EnemyMorphType::kSlime ? engine::Vector4{1.0f, 0.42f, 0.82f, 1.0f}
                                                                     : GetEnemyMorphTint(pendingAbsorbType_);
    pendingAbsorbPlayerBaseColor_ = GetColor();
    absorbEffectTimer_ = 0.0f;
    absorbEffectEmitTimer_ = 0.0f;
    absorbEffectActive_ = true;

    // 吸い込みの最中は敵を見せたまま衝突から外し、プレイヤーの初期姿勢を作る。
    enemy->SetCarried(true);
    enemy->SetCollisionAttribute(0);
    enemy->SetCollisionMask(0);
    enemy->SetIsVisible(true);
    SetVelocity({0.0f, 0.0f, 0.0f});
    SetScale({2.35f, 1.65f, 2.35f});
}

void Player::UpdateAbsorbEffect(float deltaTime) {
    // 対象が消えた場合は中断処理へ進め、無効な参照で演出を続けない。
    if (!absorbEffectActive_) {
        return;
    }

    // 練習シーンで対象が消えた場合は、演出の借用参照を除去する。
    if (tutorialSafetyEnabled_ && pendingAbsorbEnemy_ && !IsTutorialEnemyPresent(pendingAbsorbEnemy_)) {
        // 所持参照も同じ対象なら、同時に解放して次の運搬更新から除く。
        if (carriedEnemy_ == pendingAbsorbEnemy_) SetCarriedEnemy(nullptr);
        pendingAbsorbEnemy_ = nullptr;
    }
    // 対象を失った場合、中断処理に敵の外観を戻さない指定を渡す。
    if (!pendingAbsorbEnemy_) {
        CancelAbsorbEffect(false);
        return;
    }

    // 非正の秒数では位置と粒子の時計を進めない。
    if (deltaTime <= 0.0f) {
        return;
    }

    absorbEffectTimer_ += deltaTime;
    absorbEffectEmitTimer_ -= deltaTime;

    // 演出時間を0〜1に変換し、敵の捕獲位置からプレイヤー上方の吸収点へ補間する。
    const float t = std::clamp(absorbEffectTimer_ / kAbsorbEffectDuration, 0.0f, 1.0f);
    const float ease = Easing::SmoothStep(t);
    const engine::Vector3 playerPos = GetWorldPosition();
    const engine::Vector3 targetPos = playerPos + engine::Vector3{0.0f, 1.35f, 0.0f};

    engine::Vector3 absorbPos{engine::Math::Lerp(pendingAbsorbEnemyStartPos_.x, targetPos.x, ease),
                      engine::Math::Lerp(pendingAbsorbEnemyStartPos_.y, targetPos.y, ease),
                      engine::Math::Lerp(pendingAbsorbEnemyStartPos_.z, targetPos.z, ease)};
    absorbPos.y += std::sin(t * kPi) * 0.22f;

    // 中心へ近づく敵を回転させながら縮め、見た目の行列をその場で更新する。
    const float scaleRate = engine::Math::Lerp(1.0f, 0.06f, ease);
    const float squash = std::sin(t * kPi) * 0.22f;
    {
        pendingAbsorbEnemy_->SetTranslate(absorbPos);
        pendingAbsorbEnemy_->SetScale({pendingAbsorbEnemyBaseScale_.x * scaleRate * (1.0f + squash),
                                 pendingAbsorbEnemyBaseScale_.y * scaleRate * (1.0f - squash * 0.75f),
                                 pendingAbsorbEnemyBaseScale_.z * scaleRate * (1.0f + squash)});
        pendingAbsorbEnemy_->SetEulerX(pendingAbsorbEnemy_->GetRotation().x + 9.0f * deltaTime);
        pendingAbsorbEnemy_->SetEulerY(pendingAbsorbEnemy_->GetRotation().y + 18.0f * deltaTime);
        pendingAbsorbEnemy_->SetEulerZ(pendingAbsorbEnemy_->GetRotation().z + 7.0f * deltaTime);
        pendingAbsorbEnemy_->SetQuaternionMaster(false);
        pendingAbsorbEnemy_->UpdateLocalMatrix();
        pendingAbsorbEnemy_->UpdateWorldMatrix();
    }

    pendingAbsorbEnemy_->SetColor(pendingAbsorbTint_);
    pendingAbsorbEnemy_->SetCollisionAttribute(0);
    pendingAbsorbEnemy_->SetCollisionMask(0);

    // 敵の色をプレイヤーと子オブジェクトへ重ね、吸収の進行を全身の発光で示す。
    const float flash = std::sin(t * kPi);
    const float colorRate = 0.35f + flash * 0.55f;
    engine::Vector4 flashColor{std::clamp(pendingAbsorbPlayerBaseColor_.x * (1.0f - colorRate) +
                                      pendingAbsorbTint_.x * colorRate + flash * 0.18f,
                                  0.0f, 1.0f),
                       std::clamp(pendingAbsorbPlayerBaseColor_.y * (1.0f - colorRate) +
                                      pendingAbsorbTint_.y * colorRate + flash * 0.18f,
                                  0.0f, 1.0f),
                       std::clamp(pendingAbsorbPlayerBaseColor_.z * (1.0f - colorRate) +
                                      pendingAbsorbTint_.z * colorRate + flash * 0.18f,
                                  0.0f, 1.0f),
                       1.0f};
    SetColor(flashColor);

    for (engine::Object3D* child : GetChildren()) {
        // 描画可能な子がある場合は、体と同じ吸収色で揃える。
        if (child) {
            child->SetColor(flashColor);
        }
    }

    const float bodyPulse = std::sin(t * kPi);
    SetScale({2.0f + bodyPulse * 0.55f, 2.0f - bodyPulse * 0.38f, 2.0f + bodyPulse * 0.55f});

    // 吸い込む方向へ粒子を流す。姿勢は毎フレーム、粒子は設定した間隔で更新する。
    if (particleSystem_ && absorbEffectEmitTimer_ <= 0.0f) {
        engine::Vector3 toPlayer = targetPos - absorbPos;
        const float length = std::sqrt(toPlayer.x * toPlayer.x + toPlayer.y * toPlayer.y + toPlayer.z * toPlayer.z);

        // 吸収点までの距離が十分ある場合は、その方向を単位化する。
        if (length > 0.001f) {
            toPlayer = toPlayer / length;
        // 吸収点とほぼ同じ位置なら、軸を決められないため上向きを使う。
        } else {
            toPlayer = {0.0f, 1.0f, 0.0f};
        }

        const std::int32_t count = pendingAbsorbType_ == EnemyMorphType::kFireSlime ? 14 : 12;
        particleSystem_->SpawnParticles(absorbPos, count, 1.4f, &toPlayer, 26.0f, pendingAbsorbTint_,
                                        {pendingAbsorbTint_.x, pendingAbsorbTint_.y, pendingAbsorbTint_.z, 0.0f}, 0.08f,
                                        0.24f, 0.34f, 0.035f);
        absorbEffectEmitTimer_ = kAbsorbEffectParticleInterval;
    }

    // 演出の終了で、表示だけの吸収から実際の能力獲得へ切り替える。
    if (absorbEffectTimer_ >= kAbsorbEffectDuration) {
        FinishAbsorbEffect();
    }
}

// 吸収用の一時表示を戻してから、控えていた敵をコピー元に切り替える。
void Player::FinishAbsorbEffect() {
    BaseEnemy* enemy = pendingAbsorbEnemy_;

    // 対象の参照がない場合は、新しい能力への切替を行わず一時表示だけを戻す。
    if (!enemy) {
        CancelAbsorbEffect(false);
        return;
    }

    // 吸収で加えた色と伸縮を戻す。獲得時の発光は後続の StartEnemyMorph() から出す。

    SetColor(pendingAbsorbPlayerBaseColor_);

    for (engine::Object3D* child : GetChildren()) {
        // 構成部品が残っていれば、吸収中の色を親と同じ元の色へ戻す。
        if (child) {
            child->SetColor(pendingAbsorbPlayerBaseColor_);
        }
    }

    SetScale(managedBaseScale_);
    absorbEffectActive_ = false;
    pendingAbsorbEnemy_ = nullptr;
    pendingAbsorbType_ = EnemyMorphType::kNone;
    absorbEffectTimer_ = 0.0f;
    absorbEffectEmitTimer_ = 0.0f;

    // 色と大きさを元に戻して吸収フラグを落とした後に、保存しておいた対象から新しいコピーを開始します。
    StartEnemyMorph(enemy);
}

void Player::CancelAbsorbEffect(bool restoreEnemy) {
    // 中断経路でも敵の縮小・姿勢とPlayerの表示を必要に応じて戻す。
    // 対象を戻す指定があり、借用参照も有効な場合だけ、敵の姿勢と表示を戻す。
    if (restoreEnemy && pendingAbsorbEnemy_) {
        {
            pendingAbsorbEnemy_->SetScale(pendingAbsorbEnemyBaseScale_);
            pendingAbsorbEnemy_->SetQuaternionMaster(true);
            pendingAbsorbEnemy_->UpdateLocalMatrix();
            pendingAbsorbEnemy_->UpdateWorldMatrix();
        }

        // 縮小と行列を先に戻してから再表示し、吸収途中の姿勢を描画へ残しません。
        pendingAbsorbEnemy_->SetIsVisible(true);
    }

    SetColor(pendingAbsorbPlayerBaseColor_);

    // 通常形態にいる場合にだけ、通常の子オブジェクトの色も戻す。
    if (!isEnemyMorphed_) {
        for (engine::Object3D* child : GetChildren()) {
            // 現存する子だけに中断時の色を適用する。
            if (child) {
                child->SetColor(pendingAbsorbPlayerBaseColor_);
            }
        }
    }

    absorbEffectActive_ = false;
    pendingAbsorbEnemy_ = nullptr;
    pendingAbsorbType_ = EnemyMorphType::kNone;
    absorbEffectTimer_ = 0.0f;
    absorbEffectEmitTimer_ = 0.0f;
}

// 敵のプロフィールからコピーセッションを開始し、コピー元を実戦の更新対象から外す。
void Player::StartEnemyMorph(BaseEnemy* enemy) {
    // 敵を取得していない場合は、能力も外観も変更しない。
    if (!enemy) {
        return;
    }

    const EnemyMorphType resolvedType = ResolveEnemyMorphType(enemy->GetEnemyType());

    // 対応表にない敵はコピーできないため、診断を出してから戻る。
    if (resolvedType == EnemyMorphType::kNone) {
        DebugConsole::GetInstance()->AddLog("Enemy morph rejected: unsupported enemy type.");
        return;
    }

    if (baseCombatController_) {
        // 進行中の攻撃を先に止め、変身前の判定や演出を次の能力へ持ち込まない。
        baseCombatController_->Cancel(*this, true);
    }

    // 別能力へ変身する場合は、先に前のコピーセッションに後片付けを依頼する。
    if (isEnemyMorphed_ && copyAbilityController_) {
        copyAbilityController_->Cancel(*this);
    }

    const float currentMoveYaw = GetMoveYaw();
    SaveEnemyMorphBaseAppearance(enemy->GetIsVisible());

    enemyMorphType_ = resolvedType;
    enemyMorphSource_ = enemy;
    // 練習中は時間切れで能力を奪わず、解除操作を学ぶまでコピーを維持する。
    enemyMorphHasTimeLimit_ = !tutorialSafetyEnabled_ && (!enemy->GetParameters().has_value() || enemy->GetParameters()->morphLimited);
    enemyMorphDuration_ =
        enemy->GetParameters().has_value() ? (std::max)(0.1f, enemy->GetParameters()->morphDuration) : kEnemyMorphDuration;
    enemyMorphTimer_ = enemyMorphHasTimeLimit_ ? enemyMorphDuration_ : 0.0f;
    enemyMorphEffectTimer_ = 0.0f;
    enemyMorphVisualTimer_ = 0.0f;

    // 敵が持つ攻撃設定をセッションの開始に使い、固有技の進行をプレイヤー側へ渡す。
    if (copyAbilityController_) {
        copyAbilityController_->Activate(static_cast<std::int32_t>(enemyMorphType_), enemy->GetAttackProfile());
    }

    enemyMorphTint_ = GetEnemyMorphTint(enemyMorphType_);
    isEnemyMorphed_ = true;
    CompleteEnemyMorphStart(currentMoveYaw);
    enemy->SetCarried(true);

    // パラメーターがある個体は HP も0にして、続く死亡状態と一致させる。
    if (enemy->GetParameters().has_value()) {
        enemy->SetParameter(&engine::EntityParameter::hp, 0.0f);
    }

    enemy->SetDead(true);
    // コピー元は以後の敵更新・当たり判定から除外し、能力だけをPlayer側へ移す。
    enemy->SetIsVisible(false);
    enemy->SetCollisionAttribute(0);
    enemy->SetCollisionMask(0);
    enemy->SetParticleName("");
    enemy->SetGpuParticleName("");
    enemy->SetMeshEffect1Name("");
    enemy->SetMeshEffect2Name("");

    // 能力の準備後にモデル・素材・色をコピー元へ切り替える。未指定のモデルとテクスチャは触らない。
    // 敵にモデル指定がある場合だけ、プレイヤーのモデルを差し替える。
    if (!enemy->GetModelName().empty()) {
        SetModel(enemy->GetModelName());
    }

    const std::string enemyTexture = enemy->GetTexturePath();

    // 敵に個別のテクスチャ指定がある場合にだけ、その参照をコピーする。
    if (!enemyTexture.empty()) {
        SetTexture(enemyTexture);
    }

    SetMaterialType(enemy->GetMaterialType());
    SetEmissive(enemy->GetEmissive());
    SetAnimationName("");
    ResetAnimationTime();
    SetColor(enemy->GetColor());

    EmitEnemyMorphStartEffects();

    DebugConsole::GetInstance()->AddLog("Enemy morph start: " + enemy->GetEnemyType());

    // コピー元を所持していた場合は、見えなくした敵を運搬し続けないよう参照を外す。
    if (carriedEnemy_ == enemy) {
        SetCarriedEnemy(nullptr);
    }

    // スライム系のコピー能力は元敵の寿命から完全に切り離します。
    // 以降の更新・解除はPlayerCopyAbilityControllerだけが担当します。
    if (copyAbilityController_ && copyAbilityController_->HandlesMorphType(static_cast<std::int32_t>(enemyMorphType_))) {
        enemyMorphSource_ = nullptr;
    }
}

void Player::SaveEnemyMorphBaseAppearance(bool sourceVisible) {
    // 初回変身前のモデルと可視状態だけを保存し、連続コピーで基準を上書きしない。
    if (isEnemyMorphed_) {
        return;
    }

    savedMorphModelName_ = GetModelName();
    savedMorphTexturePath_ = GetTexturePath();
    savedMorphAnimName_ = GetAnimationName();
    savedMorphAnimLoop_ = IsAnimationLoop();
    savedMorphColor_ = GetColor();
    savedMorphScale_ = GetScale();
    savedMorphMaterialType_ = GetMaterialType();
    savedMorphEmissive_ = GetEmissive();
    savedMorphSourceVisible_ = sourceVisible;
    savedMorphChildVisible_.clear();
    savedMorphChildVisible_.reserve(GetChildren().size());

    for (engine::Object3D* child : GetChildren()) {
        savedMorphChildVisible_.push_back(child ? child->GetIsVisible() : false);
    }
}

// 操作や照準に関わる共通状態をコピー用へ切り替え、通常形態の子表示を隠す。
void Player::CompleteEnemyMorphStart(float moveYaw) {
    // 通常のコピーはチェックポイントにも記録する。特別な王冠コピーはこの保存経路に含めない。
    if (checkpointActivated_ && enemyMorphType_ != EnemyMorphType::kCrown) {
        const std::string currentMorph = GetEnemyTypeForCurrentMorph();

        // 再取得可能な型名が得られた場合に、チェックポイントの保存を依頼する。
        if (!currentMorph.empty()) {
            checkpointMorphEnemyType_ = currentMorph;
            RefreshCheckpointMorphSnapshot();
        }
    }

    if (engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera(); camera && !IsEditorInternal()) {
        // コピー中の右クリックは固有能力専用なので、通常フックの一人称ズームを無効化します。
        camera->SetAimCameraSuppressed(true);
    }

    // フックのマーカーが有効なら、通常のフック操作を停止したことに合わせて隠す。
    if (hookMarker_) {
        hookMarker_->SetIsVisible(false);
    }

    aimTargetObject_ = nullptr;
    SetMoveYaw(moveYaw);

    for (engine::Object3D* child : GetChildren()) {
        // 現存する通常形態の子だけを隠し、能力のモデルと重ならないようにする。
        if (child) {
            child->SetIsVisible(false);
        }
    }
}

// 形態ごとの獲得演出を選び、専用演出を持たない形態には共通の粒子を使う。
void Player::EmitEnemyMorphStartEffects() {
    const char* cue = nullptr;

    switch (enemyMorphType_) {
    // 通常スライムの獲得時はピンク用の演出を使う。
    case EnemyMorphType::kSlime:
        cue = "player/copy/pink/player_copy_pink";
        break;

    // 炎コピーの獲得では専用の炎の演出を選ぶ。
    case EnemyMorphType::kFireSlime:
        cue = "player/copy/fire/player_copy_fire";
        break;

    // 雷コピーの獲得では専用の放電演出を選ぶ。
    case EnemyMorphType::kThunderSlime:
        cue = "player/copy/thunder/player_copy_thunder";
        break;

    // 風コピーの獲得では風の軌跡を持つ演出を選ぶ。
    case EnemyMorphType::kWindSlime:
        cue = "player/copy/wind/player_copy_wind";
        break;

    // ボムコピーの獲得では爆発系の演出を選ぶ。
    case EnemyMorphType::kBomber:
        cue = "player/copy/bomb/player_copy_bomb";
        break;

    // 専用のシーケンスがない形態は、後続の共通演出に任せる。
    default:
        break;
    }

    // 専用の獲得演出を選べた場合は、この経路だけを使う。
    if (cue) {
        // 見た目だけの変形。速度・操作・当たり判定・コピー成立時刻は変更しない。
        // 攻撃は同じモーション枠を上書きでき、ジャンプ時はAnimator側で解除する。
        ClearSlimeAbilityMotion();
        PlaySlimeAbilityMotion("player_copy_acquire", false, 1.0f, 0.025f, 0.08f);
        VfxSequencer::PlayOneShotOnTarget(cue, this);

        // 獲得演出中に常時オーラを重ねない。
        enemyMorphEffectTimer_ = 0.55f;
        return;
    }

    // 王冠などの特殊コピーは既存演出を維持する。
    if (particleSystem_ && enemyMorphType_ != EnemyMorphType::kSlime) {
        engine::Vector3 up = {0.0f, 1.0f, 0.0f};
        particleSystem_->SpawnParticles(
            GetWorldPosition() + engine::Vector3{0.0f, 1.0f, 0.0f}, 32, 2.2f, &up, 22.0f, enemyMorphTint_,
            {enemyMorphTint_.x, enemyMorphTint_.y, enemyMorphTint_.z, 0.0f}, 0.14f, 0.45f, 0.42f, 0.08f);
    }
}

void Player::UpdateEnemyMorph(float deltaTime) {
    // 解除演出中は残り時間の通常更新より先に解除処理へ渡す。
    if (enemyMorphReleaseActive_) {
        UpdateEnemyMorphRelease(deltaTime);
        return;
    }

    // 解除演出でもコピー中でもないフレームは、残り時間や粒子を更新しない。
    if (!isEnemyMorphed_) {
        return;
    }

    // まだ依存するコピー元が撃破演出へ入ったら、その能力を期限切れ扱いで解除する。
    if (enemyMorphSource_ && enemyMorphSource_->IsDefeatEffectPlaying()) {
        BeginEnemyMorphRelease(true);
        return;
    }

    // 制限付きなら解除までの時計を減らす。制限のないコピーでも色や粒子の時計は進める。
    if (enemyMorphHasTimeLimit_ && deltaTime > 0.0f) {
        enemyMorphTimer_ -= deltaTime;
        enemyMorphEffectTimer_ -= deltaTime;
        enemyMorphVisualTimer_ += deltaTime;
    // 無期限の場合は、残り使用時間を更新せず演出の時計だけを進める。
    } else if (!enemyMorphHasTimeLimit_ && deltaTime > 0.0f) {
        enemyMorphEffectTimer_ -= deltaTime;
        enemyMorphVisualTimer_ += deltaTime;
    }

    // コピー元への参照を保持中は、通常の敵表示が戻らないよう再度隠す。
    if (enemyMorphSource_) {
        enemyMorphSource_->SetIsVisible(false);
    }

    // 使用制限付きのコピーで残りが尽きたら、常時粒子より先に解除へ移る。
    if (enemyMorphHasTimeLimit_ && enemyMorphTimer_ <= 0.0f) {
        BeginEnemyMorphRelease(true);
        return;
    }

    for (engine::Object3D* child : GetChildren()) {
        // 通常形態の子が存在する場合は、コピーが続く間は非表示を維持する。
        if (child) {
            child->SetIsVisible(false);
        }
    }

    // 獲得演出の待ち時間を終えた後だけ、形態ごとの色・位置・周期で常時粒子を発生させる。
    if (enemyMorphEffectTimer_ <= 0.0f) {
        // 雷コピーは経過時刻で周回位置を変え、外周の粒子を0.48秒おきに出す。
        if (enemyMorphType_ == EnemyMorphType::kThunderSlime) {
            const float morphElapsed = enemyMorphVisualTimer_;
            EmitOuterThunderParticles(GetWorldPosition(), GetScale(), morphElapsed * 2.6f, 1);
            enemyMorphEffectTimer_ = 0.48f;
            return;
        // ピンクは本体サイズに応じた高さへ専用粒子を出し、専用の間隔を設定する。
        } else if (enemyMorphType_ == EnemyMorphType::kSlime) {
            EmitCopyMorphPreset(kPinkSlimeMorphIdlePreset,
                                GetWorldPosition() + engine::Vector3{0.0f, GetScale().y * 0.82f, 0.0f});
            enemyMorphEffectTimer_ = kPinkSlimeMorphParticleInterval;
            return;
        // ボムはさらに高い位置へ専用オーラを置き、0.68秒後にまた出す。
        } else if (enemyMorphType_ == EnemyMorphType::kBomber) {
            EmitCopyMorphPreset(kBomberMorphAuraPreset, GetWorldPosition() + engine::Vector3{0.0f, GetScale().y * 0.96f, 0.0f});
            enemyMorphEffectTimer_ = 0.68f;
            return;
        // 炎は高さ補正した位置へ専用オーラを0.48秒おきに出す。
        } else if (enemyMorphType_ == EnemyMorphType::kFireSlime) {
            EmitCopyMorphPreset(kFireMorphAuraPreset, GetWorldPosition() + engine::Vector3{0.0f, GetScale().y * 0.88f, 0.0f});
            enemyMorphEffectTimer_ = 0.48f;
            return;
        // 風のオーラは専用の高さと0.62秒の周期を使う。
        } else if (enemyMorphType_ == EnemyMorphType::kWindSlime) {
            EmitCopyMorphPreset(kWindMorphAuraPreset, GetWorldPosition() + engine::Vector3{0.0f, GetScale().y * 0.82f, 0.0f});
            enemyMorphEffectTimer_ = 0.62f;
            return;
        // 専用粒子を持たない形態でも CPU 粒子が使えれば、共通の少量のオーラを上へ出す。
        } else if (particleSystem_) {
            engine::Vector3 up = {0.0f, 1.0f, 0.0f};
            particleSystem_->SpawnParticles(
                GetWorldPosition() + engine::Vector3{0.0f, 1.1f, 0.0f}, 6, 1.2f, &up, 12.0f, enemyMorphTint_,
                {enemyMorphTint_.x, enemyMorphTint_.y, enemyMorphTint_.z, 0.0f}, 0.08f, 0.22f, 0.26f, 0.03f);
        }

        enemyMorphEffectTimer_ = kEnemyMorphParticleInterval;
    }
}
