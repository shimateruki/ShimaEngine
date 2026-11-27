#define NOMINMAX
#include <cstdint>
#include "game/actor/player/copy/CopyAbilitySession.h"
#include "game/actor/player/Player.h"

// コピー能力の生成・保持・終了を担当し、入力と毎フレームの更新は能力ごとのセッションへ渡す。
using namespace copyAbilityDetail;
namespace {
std::unique_ptr<ICopyAbilitySession> CreateSession(std::int32_t morphType, const EnemyAttackProfile& profile) {
    // 変身種別に対応する状態付きセッションを生成し、未対応種別は空で返す。
    // 王冠形態では、王冠の技と進行状態を持つセッションを作る。
    if (morphType == static_cast<std::int32_t>(Player::EnemyMorphType::kCrown)) {
        return CreateCrownCopyAbility(profile);
    }

    // 通常Slimeのコピーでは、ピンク形態のセッションを作る。
    if (morphType == static_cast<std::int32_t>(Player::EnemyMorphType::kSlime)) {
        return CreatePinkSlimeCopyAbility(profile);
    }

    // 炎形態では、炎の技用セッションを作る。
    if (morphType == static_cast<std::int32_t>(Player::EnemyMorphType::kFireSlime)) {
        return CreateFireSlimeCopyAbility(profile);
    }

    // 雷形態では、雷の技用セッションを作る。
    if (morphType == static_cast<std::int32_t>(Player::EnemyMorphType::kThunderSlime)) {
        return CreateThunderSlimeCopyAbility(profile);
    }

    // 風形態では、風の技用セッションを作る。
    if (morphType == static_cast<std::int32_t>(Player::EnemyMorphType::kWindSlime)) {
        return CreateWindSlimeCopyAbility(profile);
    }

    // Bomber形態では、爆弾の技用セッションを作る。
    if (morphType == static_cast<std::int32_t>(Player::EnemyMorphType::kBomber)) {
        return CreateBomberCopyAbility(profile);
    }

    return nullptr;
}

const char* GetEnemyTypeForMorph(std::int32_t morphType) {
    // 既定の攻撃プロファイルを読む際の敵種別へ変換する。
    // 王冠形態の既定技は、FalseKingSlimeの設定から読む。
    if (morphType == static_cast<std::int32_t>(Player::EnemyMorphType::kCrown))
        return "FalseKingSlime";
    // ピンク形態の既定技は、Slimeの設定から読む。
    if (morphType == static_cast<std::int32_t>(Player::EnemyMorphType::kSlime))
        return "Slime";
    // 炎形態の既定技は、FireSlimeの設定から読む。
    if (morphType == static_cast<std::int32_t>(Player::EnemyMorphType::kFireSlime))
        return "FireSlime";
    // 雷形態の既定技は、ThunderSlimeの設定から読む。
    if (morphType == static_cast<std::int32_t>(Player::EnemyMorphType::kThunderSlime))
        return "ThunderSlime";
    // 風形態の既定技は、WindSlimeの設定から読む。
    if (morphType == static_cast<std::int32_t>(Player::EnemyMorphType::kWindSlime))
        return "WindSlime";
    // 爆弾形態の既定技は、Bomberの設定から読む。
    if (morphType == static_cast<std::int32_t>(Player::EnemyMorphType::kBomber))
        return "Bomber";
    return "";
}
} // namespace

// 現在のコピー種別と能力セッションの所有者。具体的な能力型を公開ヘッダーから隠す。
class PlayerCopyAbilityController::Impl {
  public:
    std::int32_t morphType_ = static_cast<std::int32_t>(Player::EnemyMorphType::kNone);
    std::unique_ptr<ICopyAbilitySession> session_;
};

PlayerCopyAbilityController::PlayerCopyAbilityController() : impl_(std::make_unique<Impl>()) {
    // 能力セッションの具体型は非公開の実装側で所有する。
}

PlayerCopyAbilityController::~PlayerCopyAbilityController() = default;

void PlayerCopyAbilityController::Activate(std::int32_t morphType, const EnemyAttackProfile& attackProfile) {
    // 種別を切り替える時に新しいセッションへ置き換え、前の状態を持ち越さない。
    impl_->morphType_ = morphType;
    impl_->session_ = CreateSession(morphType, attackProfile);
}

void PlayerCopyAbilityController::ActivateDefault(std::int32_t morphType) {
    // 変身種別を、既定の攻撃プロファイルを取得する敵タイプへ変換する。
    const char* enemyType = GetEnemyTypeForMorph(morphType);

    // 対応する敵種別名が無ければ、既存のセッションを破棄して未コピーへ戻す。
    if (!enemyType || enemyType[0] == '\0') {
        impl_->session_.reset();
        impl_->morphType_ = static_cast<std::int32_t>(Player::EnemyMorphType::kNone);
        return;
    }

    // 対応する敵種別のプロファイルを取得し、その種別のセッションへ渡す。
    EnemyAttackProfile profile;
    // プロファイルを取得できない場合も、この時点で保持している内容をセッションへ渡す。
    EnemyAttackProfile::LoadCachedForEnemy(enemyType, profile);
    Activate(morphType, profile);
}

void PlayerCopyAbilityController::ProcessInput(Player& player, const PlayerCopyAbilityInput& input) {
    // 現在の能力が存在する場合だけ入力を渡し、入力が意味する技の判断は能力側に任せる。
    if (impl_->session_) {
        impl_->session_->ProcessInput(player, input);
    }
}

void PlayerCopyAbilityController::Update(Player& player, float deltaTime) {
    // 能力が存在し、経過時間が正の場合だけ、その能力の時計と状態を進める。
    if (impl_->session_ && deltaTime > 0.0f) {
        impl_->session_->Update(player, deltaTime);
    }
}

void PlayerCopyAbilityController::NotifyDamageStarted(Player& player) {
    // 能力が存在する場合は被弾を通知し、進行中の技を中断するかは能力側に任せる。
    if (impl_->session_)
        impl_->session_->NotifyDamageStarted(player);
}

void PlayerCopyAbilityController::Interrupt(Player& player) {
    // 能力が存在する場合は技を中断して演出を片付ける。セッションの破棄はCancel()が行う。
    if (impl_->session_)
        impl_->session_->Cancel(player);
}

void PlayerCopyAbilityController::Cancel(Player& player) {
    // セッションに演出を片付けさせてから破棄し、変身種別を未コピーへ戻す。
    Interrupt(player);
    impl_->session_.reset();

    impl_->morphType_ = static_cast<std::int32_t>(Player::EnemyMorphType::kNone);
}

void PlayerCopyAbilityController::Draw(Player& player, ID3D12Resource* pointLight, ID3D12Resource* spotLight,
                                       engine::Camera* camera, std::int32_t previewBufferIndex) {
    // 能力固有の3D表示を、現在のセッションがある時だけ描く。
    if (impl_->session_)
        impl_->session_->Draw(player, pointLight, spotLight, camera, previewBufferIndex);
}

bool PlayerCopyAbilityController::HasOwnedVisuals() const {
    // 専用描画パスが必要かどうかを能力側の所有状態で決める。
    return impl_->session_ && impl_->session_->HasOwnedVisuals();
}

const char* PlayerCopyAbilityController::GetStateLabel() const {
    // コピーがない間はUIへ「未コピー」を返す。
    return impl_->session_ ? impl_->session_->GetStateLabel() : "未コピー";
}

void PlayerCopyAbilityController::SetPreviewAttackId(const std::string& id) {
    // 能力が存在する場合だけ、攻撃プレビューで表示する技IDを渡す。
    if (impl_->session_)
        impl_->session_->SetPreviewAttackId(id);
}

void PlayerCopyAbilityController::DrawOwnedVisuals(Player& player, uint32_t depthSrvHandle, uint32_t grabSrvHandle,
                                                   engine::Camera* camera, std::int32_t previewBufferIndex) {
    // 能力が存在する場合だけ、深度と背景テクスチャを使う固有表示を描く。
    if (impl_->session_) {
        impl_->session_->DrawOwnedVisuals(player, depthSrvHandle, grabSrvHandle, camera, previewBufferIndex);
    }
}

bool PlayerCopyAbilityController::NotifyGuardedHit(Player& player, const engine::Vector3& sourcePosition) {
    // ガード成功後の能力固有反応が発生したかを呼び出し元へ返す。
    return impl_->session_ && impl_->session_->NotifyGuardedHit(player, sourcePosition);
}

bool PlayerCopyAbilityController::HandlesMorphType(std::int32_t morphType) const {
    // セッションの実在と変身種別の一致をともに確認する。
    return impl_->session_ && impl_->morphType_ == morphType;
}

bool PlayerCopyAbilityController::IsActive() const {
    // 種別だけ残っていても、実行可能なセッションがなければ無効とみなす。
    return impl_->session_ != nullptr;
}

PlayerAttackMovement PlayerCopyAbilityController::GetAttackMovement() const {
    // 制限の寿命は能力側が所有するため、コピー解除後にタイマーだけ残りません。
    return impl_->session_ ? impl_->session_->GetAttackMovement() : PlayerAttackMovement{};
}

bool PlayerCopyAbilityController::CanBreakImpactGate() const {
    // ゲート破壊の可否は現在の能力動作に応じて変わる。
    return impl_->session_ && impl_->session_->CanBreakImpactGate();
}

bool PlayerCopyAbilityController::IsPinkBounceSlamImpactActive() const {
    // ピンク能力の着地衝撃が有効な瞬間だけ判定を通す。
    return impl_->session_ && impl_->session_->IsPinkBounceSlamImpactActive();
}
