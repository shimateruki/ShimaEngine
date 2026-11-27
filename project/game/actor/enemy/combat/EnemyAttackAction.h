#pragma once
#include "game/actor/combat/AttackSequence.h"
#include <numbers>
#include "BaseEnemy.h"

// AIは候補を選んで開始するだけ。予備動作から後隙まで同じ時計で進めます。
// ownerはこの部品を所有するActorで、Cancelまたは部品の破棄まで生存する必要があります。
// 関連：AttackSequenceの命令再生に、BaseEnemyの向き・予告・色の制御と復元を加える。
// 主な入口：Start / Update / Cancel
class EnemyAttackAction : public AttackSequence {
public:
    ~EnemyAttackAction() { Cancel(); }

    // 開始中ならfalse。攻撃定義をコピーし、Actor制御が有効なら開始前の色を退避する。
    bool Start(const EnemyAttackDefinition& definition, const AttackSequenceContext& context) {
        // 既に再生している技があれば、予告と照準を上書きしない。
        if (IsRunning())
            return false;
        // 次の攻撃へ残った予告・色を戻してから、新しい所有元を調べる。
        Restore();
        owner_ = dynamic_cast<BaseEnemy*>(context.owner);
        // Actor制御を使う定義では、向きや予告を変更できるBaseEnemyが必要。
        if (definition.sequence.actorControl.enabled && !owner_)
            return false;
        // 敵が取得できた場合は、攻撃前の色を停止時の復元用に保存する。
        if (owner_)
            originalColor_ = owner_->GetColor();
        aim_ = context;
        aim_.owner = nullptr;
        cuePlayed_ = false;
        controlled_ = definition.sequence.actorControl.enabled;
        // 命令列の準備に失敗した場合は、接続した所有元と予告状態を解除して開始失敗を返す。
        if (!AttackSequence::Start(definition, context)) {
            Restore();
            return false;
        }
        ApplyPresentation(context);
        return true;
    }

    // deltaTime は秒。照準固定時刻までは対象を追い、以降は固定した照準と現在の成立条件で進める。
    void Update(float deltaTime, const AttackSequenceContext& context) {
        // 未再生、非有限の時間、負の時間では命令列や照準を進めない。
        if (!IsRunning() || !std::isfinite(deltaTime) || deltaTime < 0)
            return;
        const auto& control = GetDefinition().sequence.actorControl;
        // Actor制御が有効で対象があり、照準固定時刻に達していなければ、このフレームの照準を保存する。
        if (controlled_ && context.hasTarget && GetElapsed() < control.aimLockTime) {
            aim_ = context;
            aim_.owner = nullptr;
        }
        // 照準だけを確定済みの値に置き換え、発射元や対象の有無はこのフレームの情報を保つ。
        auto execution = context;
        // Actor制御を使う場合だけ、命令列へ確定した方向、狙う位置、距離を渡す。
        if (controlled_) {
            execution.forward = aim_.forward;
            execution.targetPosition = aim_.targetPosition;
            execution.targetDistance = aim_.targetDistance;

            // 命令を実行する前に直前の時刻で予告を描き、今回の更新で強調時刻を跨ぐなら一度だけ光らせる。
            ApplyPresentation(context);
            // まだ強調を処理しておらず、その時刻に今回到達する場合は一度だけ済んだ状態にする。
            if (!cuePlayed_ && GetElapsed() + deltaTime >= control.cueTime) {
                cuePlayed_ = true;
                // 予告線を出す設定で敵が存在する場合だけ、記録した強調時刻で予告を発光させる。
                if (control.telegraph && owner_)
                    owner_->TriggerAttackTelegraphCue(ToColor(control.telegraphColor));
            }
        }
        AttackSequence::Update(deltaTime, execution);
        // 命令が終われば色と予告を戻し、続いていれば進んだ時刻に表示を合わせる。
        if (!IsRunning())
            Restore();
        // 命令列が続く場合は、進行後の時計に予告と色をそろえる。
        else ApplyPresentation(context);
    }

    // 命令列を止め、攻撃中に変えた色と予告表示も同時に戻す。
    void Cancel() { AttackSequence::Cancel(); Restore(); }

    // Actor制御付きの攻撃が再生中の場合だけtrue。単にシーケンスがあるだけではtrueにしない。
    bool ControlsActor() const { return controlled_ && IsRunning(); }

private:
    static engine::Vector4 ToColor(const std::array<float, 4>& c) { return {c[0], c[1], c[2], c[3]}; }

    void ApplyPresentation(const AttackSequenceContext& context) {
        // Actor制御が有効で敵が存在し、現在の命令列が再生中のときだけ姿勢と予告に反映する。
        if (!controlled_ || !owner_ || !IsRunning())
            return;
        const auto& c = GetDefinition().sequence.actorControl;
        // 移動を止める指定があるときは水平速度を0にし、落下や跳躍に使う鉛直速度を残す。
        if (c.stopMovement) {
            auto v = owner_->GetVelocity();
            v.x = v.z = 0;
            owner_->SetVelocity(v);
        }
        // 対象方向へ向く指定がある場合は、確定した照準へモデルの正面補正を足す。補正角は度からラジアンへ変換する。
        if (c.faceTarget)
            owner_->SetRotationY(std::atan2(aim_.forward.x, aim_.forward.z) + c.modelYawOffset * (std::numbers::pi_v<float> / 180.0f));
        // 着色が有効なときは予告終了時刻まで技の色を使い、その後は開始前の色へ戻す。
        if (c.tint)
            owner_->SetColor(GetElapsed() < c.telegraphEnd ? ToColor(c.color) : originalColor_);
        // 現在の発射元から確定した方向へ、攻撃射程内の長さで予告を伸ばす。
        if (c.telegraph && GetElapsed() < c.telegraphEnd) {
            const auto& d = GetDefinition();
            owner_->ShowAttackTelegraphLine(context.position, aim_.forward,
                std::clamp(aim_.targetDistance, d.minRange, d.maxRange), c.telegraphWidth,
                GetElapsed() / (std::max)(0.001f, c.telegraphEnd), ToColor(c.telegraphColor));
        // 予告線が無効、または表示する時間を過ぎた場合は残っている線を隠す。
        } else owner_->HideAttackTelegraph();
    }

    // 制御対象を所有しないため、復元後に借用参照を外して二重復元を避ける。
    void Restore() {
        // Actor制御を使った攻撃で対象がまだ有効な場合だけ、控えた色に戻して線を隠す。
        if (controlled_ && owner_) {
            owner_->SetColor(originalColor_);
            owner_->HideAttackTelegraph();
        }
        owner_ = nullptr;
        controlled_ = false;
    }

    BaseEnemy* owner_ = nullptr; // 予告と色を変更する敵の借用参照。Cancelまで生存している必要がある。
    engine::Vector4 originalColor_{1, 1, 1, 1}; // 攻撃開始前の色。終了・中断時に戻す。
    AttackSequenceContext aim_; // 照準固定時の座標・方向。Actorの所有権は持たない。
    bool controlled_ = false, cuePlayed_ = false; // Actor制御の有効状態、cuePlayed_は現在の攻撃での予告強調済み状態。
};
