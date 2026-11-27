#pragma once
#include <algorithm>

// 入力の記憶とジャンプ権だけを管理し、速度やアニメーションは変更しません。
// 関連：PlayerMoverが入力・接地を渡し、受理した要求に従って実際の跳躍速度を設定する。
// 主な入口：Reset / Advance / Consume
class PlayerJumpRequest {
  public:
    // 入力バッファ、接地猶予、溜め量をまとめて初期状態へ戻します。
    void Reset() { *this = PlayerJumpRequest{}; }

    /// <summary>入力状態と接地状態から、ジャンプ要求を一フレーム進めます。</summary>
    /// <param name="deltaTime">前回更新からの経過秒数。負値は 0 として扱う。</param>
    /// <param name="isJumpHeld">ジャンプを押し続けているか</param>
    /// <param name="isJumpTriggered">このフレームで押し始めたか</param>
    /// <param name="isGrounded">このフレームの接地状態</param>
    /// <param name="canChargeJump">溜めジャンプが使えるコピー状態か</param>
    void Advance(float deltaTime, bool isJumpHeld, bool isJumpTriggered, bool isGrounded, bool canChargeJump) {
        // 入力予約と接地猶予の残り時間を独立して減らし、0秒で下げ止める。
        const float elapsed = (std::max)(0.0f, deltaTime);
        inputBufferRemainingSeconds_ = (std::max)(0.0f, inputBufferRemainingSeconds_ - elapsed);
        groundGraceRemainingSeconds_ = (std::max)(0.0f, groundGraceRemainingSeconds_ - elapsed);
        // 空中を一度でも観測したことを覚え、次の接地でジャンプ権を戻せるようにする。
        if (!isGrounded)
            hasLeftGround_ = true;
        // 地上にいて未消費か、消費後に一度空中を通った場合だけ、ジャンプ権と0.10秒の接地猶予を戻す。
        if (isGrounded && (!jumpConsumed_ || hasLeftGround_)) {
            jumpConsumed_ = false;
            hasLeftGround_ = false;
            groundGraceRemainingSeconds_ = kGroundGraceSeconds;
        }
        // 溜めの可否が切り替わった場合は、前の形態で得た予約や溜めを破棄する。
        if (canChargeJump != canChargeJump_) {
            inputBufferRemainingSeconds_ = chargeSeconds_ = pendingChargeSeconds_ = 0.0f;
            isCharging_ = false;
        }
        canChargeJump_ = canChargeJump;
        // 通常形態では溜めを管理せず、押し始めをジャンプ入力として扱う。
        if (!canChargeJump) {
            // このフレームで押されれば、0.15秒だけ発動を予約する。
            if (isJumpTriggered)
                inputBufferRemainingSeconds_ = kInputBufferSeconds;
            return;
        }
        // 溜め可能で押し始めた場合は、旧予約を消して新しい溜めを始める。
        if (isJumpTriggered) {
            isCharging_ = true;
            chargeSeconds_ = 0.0f;
            inputBufferRemainingSeconds_ = 0.0f;
        }
        // 溜め中にボタンを保持していれば、最大1秒まで溜め時間を加算する。
        if (isCharging_ && isJumpHeld)
            chargeSeconds_ = (std::min)(kMaxChargeSeconds, chargeSeconds_ + elapsed);
        // 溜め中にボタンが離れれば、溜めた秒数を固定し、発動を0.15秒だけ予約する。
        if (isCharging_ && !isJumpHeld) {
            pendingChargeSeconds_ = chargeSeconds_;
            inputBufferRemainingSeconds_ = kInputBufferSeconds;
            isCharging_ = false;
            chargeSeconds_ = 0.0f;
        }
    }

    /// <summary>入力と接地猶予が残っていれば一度だけ要求を取り出します。</summary>
    /// <param name="charge">成功時、コピー用の溜め時間を秒で受け取る。失敗時は変更しない。</param>
    bool Consume(float& charge) {
        // 入力予約か接地猶予が切れた場合と、地上ジャンプ権を消費済みの場合は発動しない。
        if (inputBufferRemainingSeconds_ <= 0.0f || groundGraceRemainingSeconds_ <= 0.0f || jumpConsumed_)
            return false;
        // 受理した入力の溜め時間を呼び出し元へ渡す。通常ジャンプの溜めは0秒。
        charge = canChargeJump_ ? pendingChargeSeconds_ : 0.0f;
        // 同じ入力を再利用しないよう予約と猶予を消し、一度空中へ出るまで地上ジャンプ権を戻さない。
        inputBufferRemainingSeconds_ = groundGraceRemainingSeconds_ = 0.0f;
        jumpConsumed_ = true;
        hasLeftGround_ = false;
        return true;
    }

    // 溜め可能な形態で押し始め、まだ放していない期間にtrueを返す。
    bool IsCharging() const { return isCharging_; }

    // 進行中の溜め時間を秒で返す。離した後の確定量はConsume()で渡す。
    float Charge() const { return chargeSeconds_; }

    // 発動待ちの入力が有効な残り秒数。0は有効な予約なし。
    float BufferRemaining() const { return inputBufferRemainingSeconds_; }

    // 離地後も地上ジャンプを許す残り秒数。0は猶予なし。
    float GraceRemaining() const { return groundGraceRemainingSeconds_; }

  private:
    static constexpr float kGroundGraceSeconds = 0.10f;
    static constexpr float kInputBufferSeconds = 0.15f;
    static constexpr float kMaxChargeSeconds = 1.0f;

    // 接地猶予と先行入力は独立したタイマーで、Consume 成功時に両方を消費します。
    float inputBufferRemainingSeconds_ = 0.0f, groundGraceRemainingSeconds_ = 0.0f, chargeSeconds_ = 0.0f, pendingChargeSeconds_ = 0.0f;
    bool jumpConsumed_ = false, hasLeftGround_ = false, canChargeJump_ = false, isCharging_ = false;
};
