#pragma once
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "AttackSequenceTracks.h"

// 炎弾の共通描画器へ渡す値。外観の違いごとに敵専用の発射処理を増やさないための設定です。
struct AttackFireAppearance {
    float red = 1.0f, green = 0.34f, blue = 0.07f, alpha = 0.96f;
    float emissive = 2.8f;
    float effectScale = 1.15f, softness = 0.42f, intensity = 0.96f, billboardScale = 0.68f;
    float trailInterval = 0.030f, trailSpeedScale = 1.0f; // trailIntervalは粒子の発生間隔（秒）、trailSpeedScaleは速度倍率。
};


// 発射する弾の構成。未指定の旧JSONでは攻撃全体の設定を引き継ぎます。
struct AttackProjectileComponent {
    bool configured = false; // falseならこの部品の値を使わず、攻撃全体の旧設定を引き継ぐ。
    bool collisionEnabled = true; // falseでも見た目は発射するが、弾の衝突ダメージを無効にする。
    std::string model = "Primitives/sphere";
    std::string style = "fire";
    float radius = 0.5f, lifetime = 2.65f, minSpeed = 15, maxSpeed = 28, damage = 1;
    float visualScale = 1.2f;
    AttackFireAppearance appearance;
    std::string trail = "shared/fireball_trail_flame", secondaryTrail = "shared/fireball_trail_embers";
    std::string impact = "shared/fireball_impact_flame", secondaryImpact = "shared/fireball_impact_sparks";

    bool Validate() const {
        // 未設定の弾は使わず、有効時だけ描画値と速度の範囲を検証する。
        if (!configured)
            return true;
        // 表示モデルが空か、外観がfireとplainのどちらでもなければ拒否する。
        if (model.empty() || (style != "fire" && style != "plain"))
            return false;
        for (float value : {radius, lifetime, minSpeed, maxSpeed, damage, visualScale})
            // 弾の物理・表示設定は有限で0から1000に収まる必要がある。
            if (!std::isfinite(value) || value < 0 || value > 1000)
                return false;
        // 半径と寿命と表示倍率は正を要求し、倍率は20まで、最低速度は最高速度以下とする。
        if (radius <= 0 || lifetime <= 0 || visualScale <= 0 || visualScale > 20 || minSpeed > maxSpeed)
            return false;
        const auto& a = appearance;
        for (float value : {a.red, a.green, a.blue, a.alpha, a.emissive, a.effectScale, a.softness,
            a.intensity, a.billboardScale, a.trailInterval, a.trailSpeedScale})
            // 炎弾の外観に渡す各数値は、有限で0以上100以下に限る。
            if (!std::isfinite(value) || value < 0 || value > 100)
                return false;
        // 外観のうちアルファは1以下、残光の間隔は0.001秒以上を必要とする。
        return a.alpha <= 1 && a.trailInterval >= 0.001f;
    }
};

// 命令列の開始からの秒数で一度だけ実行します。開始する攻撃フェーズは接続先が決めます。
// 座標は術者の右・上・前を基準とし、yawDegreesでその基準方向を水平回転します。
struct AttackSequenceEvent {
    std::string name;
    bool enabled = true;
    AttackProjectileComponent projectile;
    float time = 0.0f; // シーケンス開始からの発生秒数。
    std::string operation = "projectile"; // projectile / gpu_vfx
    std::string condition = "always"; // always / grounded / airborne / target_in_range
    bool stopOnFalse = false; // 条件不成立時に後続命令・トラックの再生も止めるか。
    float right = 0.0f, up = 1.05f, forward = 0.65f;

    // 通常の位置に加算する、術者の基準サイズに比例した位置。伸縮アニメーションには追従しません。
    float sizeRight = 0.0f, sizeUp = 0.0f, sizeForward = 0.0f;

    float yawDegrees = 0.0f; // 術者の前方を基準に、発生位置と発射方向を水平回転します。
    bool aimAtTarget = true;
    float targetHeight = 0.35f; // 狙う対象の基準位置から上に足す距離（ワールド単位）。

    float distanceSpeedScale = 1.65f; // 0なら攻撃定義の最高速度を使います。
    float distanceSpeedBias = 0.0f; // 距離×倍率に加え、最後に攻撃の最低・最高速度で制限します。
    float verticalSpeed = 0.0f; // 発射後の速度へ加える上向き成分。重力の設定とは別。

    std::int32_t count = 1;
    float spreadDegrees = 0.0f;

    std::string preset;
    bool directed = false; // GPU命令だけに適用。プリセットの速度を発射方向へ揃えます。
    float particleSpeedScale = 1.0f;
};

// 術者の攻撃中の振る舞い。敵の種類や攻撃名に依存しない共通部品です。
struct AttackActorControl {
    bool enabled = false; // 旧攻撃は従来のAI接続を保ち、移行済みだけ有効にします。

    bool stopMovement = true;
    bool faceTarget = true;
    float aimLockTime = 0.24f; // 開始からの秒。以後は方向・標的位置・距離を固定します。
    float modelYawOffset = 180.0f; // モデル正面の補正角（度）。

    bool telegraph = true;
    float telegraphEnd = 0.42f; // シーケンス開始から予告を消すまでの秒数。
    float telegraphWidth = 1.0f; // 予告線の幅（ワールド単位）。

    float cueTime = 0.24f; // シーケンス開始から予告を強調するまでの秒数。

    bool tint = true;
    std::array<float, 4> color{1, 0.72f, 0.48f, 1};
    std::array<float, 4> telegraphColor{1, 0.34f, 0.06f, 0.8f};

    bool Validate(float duration) const {
        // 予告と照準の時刻は攻撃全体の長さ以内、色成分は0～1に制限する。
        if (!enabled)
            return true;
        for (float v : {aimLockTime, telegraphEnd, cueTime})
            // 照準固定、予告終了、強調表示はすべて有限の秒数で、0からシーケンスの長さまでに置く。
            if (!std::isfinite(v) || v < 0 || v > duration)
                return false;
        // 向き補正は有限で絶対値360度以下、予告の幅は有限で0より大きく100以下とする。
        if (!std::isfinite(modelYawOffset) || std::abs(modelYawOffset) > 360 ||
            !std::isfinite(telegraphWidth) || telegraphWidth <= 0 || telegraphWidth > 100) return false;
        for (auto values : {color, telegraphColor})
            for (float v : values)
                // 本体と予告線のRGBA各成分は、すべて有限かつ0〜1に収まる必要がある。
                if (!std::isfinite(v) || v < 0 || v > 1)
                    return false;
        return true;
    }
};

/// <summary>攻撃シーケンスの実行条件と、時間順のイベント・トラック設定をまとめる。</summary>
struct AttackSequenceDefinition {
    AttackActorControl actorControl;
    std::string assetPath; // 空なら埋め込み。設定時はこのJSONを正本としてプロファイル読込時に解決します。
    bool enabled = false;
    std::string binding = "enemy"; // enemy / player_special
    std::int32_t priority = 0; // 同じ入力・射程に複数候補がある場合は大きい値を優先します。
    float duration = 0.42f;
    std::string projectileStyle = "fire"; // fire / plain
    float visualScale = 1.2f;
    AttackFireAppearance fireAppearance;
    std::vector<AttackSequenceEvent> events;
    std::vector<AttackSequenceTrack> tracks;

    // 誤った命令を黙って実行せず、読込・保存・実行開始で同じ検証を使います。
    bool Validate(std::string* error = nullptr) const {
        // 出力先が指定された場合だけ失敗理由を書き、検証をfalseで終える。
        auto fail = [&](const char* reason) { if (error) *error = reason; return false; };
        // 未使用のシーケンスは、中身を検証せず許可する。
        if (!enabled)
            return true;
        // 接続先には、敵用とプレイヤーの特殊入力用のいずれかを指定する。
        if (binding != "enemy" && binding != "player_special")
            return fail("実行先が不正です。");
        // 術者の予告と照準の値が有効であることを確認する。
        if (!actorControl.Validate(duration))
            return fail("術者制御の時刻・幅・色が範囲外です。");
        // 術者制御を使うシーケンスは、敵側へ接続する場合に限る。
        if (actorControl.enabled && binding != "enemy")
            return fail("術者制御は敵用の共通アクションで使用します。");
        // シーケンスの長さは、有限の0〜30秒に限る。
        if (!std::isfinite(duration) || duration < 0.0f || duration > 30.0f)
            return fail("再生時間は0～30秒です。");
        // 旧形式の弾の外観はfireまたはplainに限る。
        if (projectileStyle != "fire" && projectileStyle != "plain")
            return fail("弾の外観が不正です。");
        // 旧形式の表示倍率は、有限の0より大きく20以下に限る。
        if (!std::isfinite(visualScale) || visualScale <= 0 || visualScale > 20)
            return fail("表示倍率が不正です。");
        const auto& appearance = fireAppearance;
        for (float value : {appearance.red, appearance.green, appearance.blue, appearance.alpha,
            appearance.emissive, appearance.effectScale, appearance.softness, appearance.intensity,
            appearance.billboardScale, appearance.trailInterval, appearance.trailSpeedScale})
            // 旧形式の炎弾の外観はすべて有限で0〜100の範囲を要求する。
            if (!std::isfinite(value) || value < 0 || value > 100)
                return fail("炎弾の外観設定が不正です。");
        // アルファは1以下、残光間隔は0.001秒以上にさらに制限する。
        if (appearance.alpha > 1 || appearance.trailInterval < 0.001f)
            return fail("炎弾の透明度または軌跡間隔が範囲外です。");
        // 空のシーケンスと、どちらかが個数上限を超える設定を拒否する。
        if ((events.empty() && tracks.empty()) || events.size() > 128 || tracks.size() > 64)
            return fail("命令またはトラックが必要です。命令は128件、トラックは64件までです。");
        // 個別の設定を先に検証し、その後に親子関係と分岐元を調べます。
        for (const auto& track : tracks)
            // トラックの個別条件を満たさない場合は、その失敗理由を保持して終える。
            if (!track.Validate(duration, error))
                return false;
        for (size_t i = 0; i < tracks.size(); ++i) {
            const auto& track = tracks[i];
            // 親表示の有効期間が、子の当たり判定を使う時間をすべて含むようにします。
            if (track.parentTrack >= 0) {
                // 親を持てるのは当たり判定だけ。処理順の前にあるトラックを参照させる。
                if (track.type != "hitbox" || static_cast<size_t>(track.parentTrack) >= i)
                    return fail("判定の親は一覧で自身より前にある表示Objectを指定してください。");
                const auto& parent = tracks[track.parentTrack];
                // 親には有効なメッシュ表示を指定し、子の判定区間全体を覆わせる。
                if (parent.type != "mesh_vfx" || !parent.enabled || parent.begin > track.begin || parent.end < track.end)
                    return fail("親Objectは有効で、判定の区間全体を含む必要があります。");
            }
            // 当たったかどうかで分岐するなら、分岐元の判定が先に終わる必要があります。
            if (track.condition == "contact" || track.condition == "no_contact" || track.condition == "track_completed") {
                // 結果を参照する分岐では、参照先の番号がトラック一覧内にあることを要求する。
                if (track.conditionTrack < 0 || static_cast<size_t>(track.conditionTrack) >= tracks.size())
                    return fail("分岐の参照元となる判定トラックを選んでください。");
                const auto& source = tracks[track.conditionTrack];
                // 結果が確定してから分岐するため、有効な判定が後続開始までに終了する必要がある。
                if (source.type != "hitbox" || !source.enabled || source.end > track.begin)
                    return fail("分岐元は有効な判定トラックで、後続の開始までに終了している必要があります。");
            }

            // 同時に二つの本体姿勢を上書きしない。条件が排他的でも区間を分けます。
            if (track.enabled && track.type == "body_animation")
                for (size_t j = 0; j < i; ++j)
                    // 先に登録済みの有効な身体トラックと時間が重なる場合は、その二つを同時に許可しない。
                    if (tracks[j].enabled && tracks[j].type == "body_animation" &&
                        tracks[j].begin < track.end && track.begin < tracks[j].end)
                        return fail("本体アニメーションの区間は重ねられません。");
        }
        // 単発命令では起動時刻、弾数、座標と速度を確認し、再生時に不正値を使わせません。
        for (const auto& event : events) {
            // 弾の個別設定を使うイベントでは、その半径や速度の制約も満たす必要がある。
            if (!event.projectile.Validate())
                return fail("弾部品の半径・寿命・速度・見た目を確認してください。");
            // 実行時刻は有限かつ0から終了時刻までに置く。
            if (!std::isfinite(event.time) || event.time < 0 || event.time > duration)
                return fail("イベント時刻が再生区間外です。");
            // 単発イベントの命令として、弾の発射とGPU粒子の二種類を受け付ける。
            if (event.operation != "projectile" && event.operation != "gpu_vfx")
                return fail("未対応の命令です。");
            // 常時、接地、空中、対象が射程内の四条件だけを受け付ける。
            if (event.condition != "always" && event.condition != "grounded" && event.condition != "airborne" && event.condition != "target_in_range")
                return fail("未対応の条件です。");
            // 同時発射は1〜32個の範囲に限る。
            if (event.count < 1 || event.count > 32)
                return fail("同時発射数は1～32です。");
            for (float value : {event.right, event.up, event.forward, event.sizeRight, event.sizeUp, event.sizeForward,
                event.yawDegrees, event.targetHeight, event.distanceSpeedScale, event.distanceSpeedBias,
                event.verticalSpeed, event.spreadDegrees, event.particleSpeedScale})
                // 座標、速度、角度などの数値は有限で、絶対値1000以下を要求する。
                if (!std::isfinite(value) || std::abs(value) > 1000.0f)
                    return fail("イベントの数値が不正です。");
            // 距離による速度倍率と拡散角は負値を許さず、拡散角は360度までにする。
            if (event.distanceSpeedScale < 0 || event.spreadDegrees < 0 || event.spreadDegrees > 360)
                return fail("速度倍率または拡散角が範囲外です。");
            // 粒子の速度倍率に負の値を指定した場合は拒否する。
            if (event.particleSpeedScale < 0)
                return fail("粒子の速度倍率は0以上です。");
        }
        // エラーの出力先がある場合は、有効な設定の古いエラーを消す。
        if (error)
            error->clear();
        return true;
    }
};

// 描画やPlayer型に依存しない時計。大きなdtで通過した命令も時刻順に一度ずつ通知します。
// 関連：AttackSequenceが通知先を渡す。ここでは弾やObjectを生成しない。
// 主な入口：Start / Update / Reset
class AttackSequenceClock {
  public:
    // 定義を検証して値コピーし、同時刻の命令順を保持したまま時刻順に並べる。
    bool Start(const AttackSequenceDefinition& definition) {
        Reset();
        // 無効化された定義または検証を通らない定義では、時計を開始できない。
        if (!definition.enabled || !definition.Validate())
            return false;
        definition_ = definition;
        std::stable_sort(definition_.events.begin(), definition_.events.end(),
            [](const auto& a, const auto& b) { return a.time < b.time; });
        running_ = true;
        return true;
    }

    template<class Condition, class Execute>
    void Update(float deltaTime, Condition condition, Execute execute) {
        // 時計が停止中か、刻みが非有限または負であれば進めない。0秒は開始時刻の命令を評価できる。
        if (!running_ || !std::isfinite(deltaTime) || deltaTime < 0)
            return;
        // deltaTime は秒。大きな刻みでも通過した未処理命令を順番に取り出す。
        elapsed_ = (std::min)(definition_.duration, elapsed_ + deltaTime);
        while (running_ && next_ < definition_.events.size() && definition_.events[next_].time <= elapsed_ + 0.000001f) {
            const auto& event = definition_.events[next_++];
            // 無効な命令は飛ばした数だけを増やし、後続の再生を止めない。
            if (!event.enabled) {
                ++skipped_;
                continue;
            }
            // 命令ごとに現在の条件を評価する。不成立でも stopOnFalse がなければ後続を続ける。
            if (condition(event.condition)) {
                execute(event);
                ++executed_;
            // 条件を満たさなかった命令は実行せず、不成立の数へ加える。
            } else {
                ++skipped_;
                // 不成立時の停止が指定されている命令なら、その時刻で止めて後続の評価を終える。
                if (event.stopOnFalse) {
                    running_ = false;
                    stoppedByCondition_ = true;
                    elapsed_ = event.time; // 大きなdtでも停止イベントより後の区間を評価しません。
                }
            }
        }
        // 終端時刻の命令まで評価してから停止する。開始時刻ゼロの命令も最初の Update が実行する。
        if (elapsed_ >= definition_.duration)
            running_ = false;
    }

    // 再生中のフラグと経過時刻、通知した数を初期化します。次のStartで定義を取り直します。
    void Reset() { running_ = false; stoppedByCondition_ = false; elapsed_ = 0; next_ = executed_ = skipped_ = 0; }

    bool WasStoppedByCondition() const { return stoppedByCondition_; }

    bool IsRunning() const { return running_; }

    float GetElapsed() const { return elapsed_; }

    size_t GetExecutedCount() const { return executed_; }

    size_t GetSkippedCount() const { return skipped_; }

  private:
    AttackSequenceDefinition definition_;
    float elapsed_ = 0;
    size_t next_ = 0, executed_ = 0, skipped_ = 0;
    bool running_ = false;
    bool stoppedByCondition_ = false;
};
