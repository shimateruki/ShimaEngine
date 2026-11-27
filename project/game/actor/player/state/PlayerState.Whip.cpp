#define NOMINMAX
#include <cstdint>
#include "game/actor/player/state/PlayerState.h"
#include "Player.h"
#include "BaseEnemy.h"
#include "EnemyGiantSlime.h"
#include "CollisionManager.h"
#include "CollisionConfig.h"
#include "GimmickHookPullBlock.h"
#include "game/system/combat/HitEffectDirector.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include "Easing.h"
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include "ModelManager.h"
#include "MeshRenderer.h"
#include <cstring>
#include "game/actor/player/animation/PlayerWhipDeformer.h"
#ifdef USE_IMGUI
#include "imgui.h"
#endif

namespace {
constexpr const char* kSettingsPath = "Resources/json/player/whip.json";
void Sanitize(PlayerWhipSettings& settings) {
    // 無効な数値は既定値を採用し、有効な値でも演出や判定に使える範囲へ収める。
    const auto clamp = [](float value, float lo, float hi, float fallback) {
        return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
    };
    settings.maxDistance = clamp(settings.maxDistance, 1.0f, 200.0f, 8.0f);
    settings.shootSpeed = clamp(settings.shootSpeed, 30.0f, 1000.0f, 450.0f);
    settings.returnSpeed = clamp(settings.returnSpeed, 30.0f, 1500.0f, 650.0f);
    settings.hitRadius = clamp(settings.hitRadius, 0.1f, 3.0f, 0.65f);
    // 溜め・当たり判定・復帰の各段階に、0秒にならない範囲を設ける。
    settings.windupDuration = clamp(settings.windupDuration, 0.02f, 0.4f, 0.08f);
    settings.sweepDuration = clamp(settings.sweepDuration, 0.06f, 0.8f, 0.18f);
    settings.recoverDuration = clamp(settings.recoverDuration, 0.04f, 0.6f, 0.14f);
    settings.sweepAngleDegrees = clamp(settings.sweepAngleDegrees, 30.0f, 170.0f, 140.0f);
    settings.bendDegrees = clamp(settings.bendDegrees, 0.0f, 45.0f, 28.0f);
    settings.bodyTwistDegrees = clamp(settings.bodyTwistDegrees, 0.0f, 65.0f, 32.0f);
    settings.bodySquash = clamp(settings.bodySquash, 0.0f, 0.35f, 0.18f);
    // 先端の追従や捕獲後の補間に使う値を補正して、接触した瞬間の形を保てるようにする。
    settings.tipDelay = clamp(settings.tipDelay, 0.0f, 0.12f, 0.045f);
    settings.bindBlendDuration = clamp(settings.bindBlendDuration, 0.04f, 0.4f, 0.16f);
    settings.pullBendRatio = clamp(settings.pullBendRatio, 0.0f, 0.3f, 0.10f);
    settings.thickness = clamp(settings.thickness, 0.1f, 1.5f, 0.65f);
    settings.groundSkimTolerance = clamp(settings.groundSkimTolerance, 0.0f, 1.0f, 0.30f);
    settings.bodyShear = clamp(settings.bodyShear, 0.0f, 1.0f, 0.55f);
    settings.bodyStretch = clamp(settings.bodyStretch, 0.0f, 0.6f, 0.28f);
    settings.captureBrace = clamp(settings.captureBrace, 0.0f, 0.8f, 0.40f);
    settings.bodyRebound = clamp(settings.bodyRebound, 0.0f, 0.3f, 0.12f);
    settings.surfaceRootAngle = clamp(settings.surfaceRootAngle, 35.0f, 65.0f, 55.0f);
    settings.surfaceLift = clamp(settings.surfaceLift, 0.8f, 1.8f, 1.25f);
    // 体表上で鞭の付け根に含める角度は度、持ち上げと先端の丸みは表示専用の係数です。
    settings.tipRoundness = clamp(settings.tipRoundness, 0.0f, 0.4f, 0.22f);
    // 接触と戻りで使う見た目の変形を、進行や衝突に使う値とは別に制限する。
    settings.contactSquash = clamp(settings.contactSquash, 0.0f, 0.8f, 0.55f);
    settings.contactDuration = clamp(settings.contactDuration, 0.06f, 0.35f, 0.16f);
    settings.returnBulge = clamp(settings.returnBulge, 0.0f, 0.8f, 0.30f);
    settings.returnReceive = clamp(settings.returnReceive, 0.0f, 0.4f, 0.22f);
    settings.returnSettleDuration = clamp(settings.returnSettleDuration, 0.08f, 0.4f, 0.20f);
    settings.launchMound = clamp(settings.launchMound, 0.0f, 0.35f, 0.18f);
    settings.launchDraw = clamp(settings.launchDraw, 0.0f, 0.2f, 0.10f);
    settings.launchFlow = clamp(settings.launchFlow, 0.0f, 0.5f, 0.24f);

}
}

// プレイヤーのワールドAABBの中心を、当たり判定を伸ばす原点として返します。
engine::Vector3 Player::GetWhipOrigin() const {
    const engine::Aabb bounds = GetAabb();
    return (bounds.min + bounds.max) * 0.5f;
}
// 当たり判定の原点をモデルの描画行列へ移し、変形後の表示上の根元を返します。
engine::Vector3 Player::GetWhipVisualOrigin() const {
    const auto origin = GetWhipOrigin();
    // 描画モデルがなければ、当たり判定に使う原点をそのまま返す。
    if (!GetMeshRenderer()) return origin;
    const auto local = engine::Math::Transform(origin, engine::Math::Inverse(GetWorldMatrix()));
    return engine::Math::Transform(local, GetMeshRenderer()->GetRenderWorldMatrix());
}
// 引き寄せ状態などから受け取った見た目を、次の更新で帰還処理に渡します。
void Player::BeginWhipReturn(const std::shared_ptr<PlayerWhipVisual>& visual) {
    pendingWhipReturn_ = visual;
}
// 帰還中の鞭の所有を手放し、描画に使った状態を片付けます。
void Player::CancelWhipReturn() {
    // 帰還中の表示を保持している場合だけ、その終了処理を呼ぶ。
    if (auto visual = std::move(returningWhipVisual_)) visual->Finish(this);
}
void Player::UpdateWhipReturn(float deltaTime) {
    // 帰還を予約された鞭を、同じ表示のまま帰還中の状態へ移す。
    if (pendingWhipReturn_) {
        returningWhipVisual_ = std::move(pendingWhipReturn_);
        returningWhipVisual_->BeginReturn(this);
        activeWhipVisual_ = returningWhipVisual_;
    }
    // 帰還中の鞭が無ければ、縮める曲線を持たないため終了する。
    if (!returningWhipVisual_) return;
    // 死亡・演出拘束・変身に入った場合、または帰還が完了した場合は表示を片付ける。
    if (IsDead() || IsCinematicLocked() || IsEnemyMorphed() ||
        returningWhipVisual_->UpdateReturn(this, deltaTime)) CancelWhipReturn();
}
// 本体アニメーションの後、現在のモデル姿勢に鞭の根元を合わせます。
void Player::RefreshWhipVisual() {
    // 表示への弱参照がまだ有効なら、現在の本体変形に付け直す。
    if (auto visual = activeWhipVisual_.lock()) visual->RefreshAttachment(this);
}
// 有効なロックオン照準があれば狙う方向を、使えなければプレイヤーの前方を返します。
engine::Vector3 Player::GetWhipDirection() const {
    // 照準位置を取得できる場合は、当たり判定の根元からの差を調べる。
    if (HasLockOnAimPoint()) {
        const engine::Vector3 delta = lockOnAimPoint_ - GetWhipOrigin();
        // 照準が根元から十分離れている場合だけ、その方向を単位化して返す。
        if (engine::Math::Length(delta) > 0.001f)
            return engine::Math::Normalize(delta);
    }
    return GetForwardDirection();
}
void Player::LoadWhipSettings() {
    // 読み込みだけで保存は行わず、ファイルがない場合は既定値で遊べます。
    whipSettings_ = {};
    try {
        std::ifstream stream(kSettingsPath);
        // 設定を開けなければ、初期化済みの既定値で動かす。
        if (!stream) return;
        // 保存されたキーを既定値へ重ねます。旧ファイルに含まれないキーは既定値のままです。
        const auto json = nlohmann::json::parse(stream);
        whipSettings_.maxDistance = json.value("maxDistance", whipSettings_.maxDistance);
        whipSettings_.shootSpeed = json.value("shootSpeed", whipSettings_.shootSpeed);
        whipSettings_.returnSpeed = json.value("returnSpeed", whipSettings_.returnSpeed);
        whipSettings_.hitRadius = json.value("hitRadius", whipSettings_.hitRadius);
        whipSettings_.windupDuration = json.value("windupDuration", whipSettings_.windupDuration);
        whipSettings_.sweepDuration = json.value("sweepDuration", whipSettings_.sweepDuration);
        whipSettings_.recoverDuration = json.value("recoverDuration", whipSettings_.recoverDuration);
        whipSettings_.sweepAngleDegrees = json.value("sweepAngleDegrees", whipSettings_.sweepAngleDegrees);
        whipSettings_.bendDegrees = json.value("bendDegrees", whipSettings_.bendDegrees);
        whipSettings_.bodyTwistDegrees = json.value("bodyTwistDegrees", whipSettings_.bodyTwistDegrees);
        whipSettings_.bodySquash = json.value("bodySquash", whipSettings_.bodySquash);
        whipSettings_.tipDelay = json.value("tipDelay", whipSettings_.tipDelay);
        whipSettings_.bindBlendDuration = json.value("bindBlendDuration", whipSettings_.bindBlendDuration);
        whipSettings_.pullBendRatio = json.value("pullBendRatio", whipSettings_.pullBendRatio);
        whipSettings_.thickness = json.value("thickness", whipSettings_.thickness);
        whipSettings_.groundSkimTolerance = json.value("groundSkimTolerance", whipSettings_.groundSkimTolerance);
        whipSettings_.bodyShear = json.value("bodyShear", whipSettings_.bodyShear);
        whipSettings_.bodyStretch = json.value("bodyStretch", whipSettings_.bodyStretch);
        whipSettings_.captureBrace = json.value("captureBrace", whipSettings_.captureBrace);
        whipSettings_.bodyRebound = json.value("bodyRebound", whipSettings_.bodyRebound);
        whipSettings_.surfaceRootAngle = json.value("surfaceRootAngle", whipSettings_.surfaceRootAngle);
        whipSettings_.surfaceLift = json.value("surfaceLift", whipSettings_.surfaceLift);
        whipSettings_.contactDuration = json.value("contactDuration", whipSettings_.contactDuration);
        whipSettings_.returnSettleDuration = json.value("returnSettleDuration", whipSettings_.returnSettleDuration);
        whipSettings_.launchFlow = json.value("launchFlow", whipSettings_.launchFlow);
        whipSettings_.launchDraw = json.value("launchDraw", whipSettings_.launchDraw);
        whipSettings_.launchMound = json.value("launchMound", whipSettings_.launchMound);
        whipSettings_.returnReceive = json.value("returnReceive", whipSettings_.returnReceive);
        whipSettings_.returnBulge = json.value("returnBulge", whipSettings_.returnBulge);
        whipSettings_.contactSquash = json.value("contactSquash", whipSettings_.contactSquash);
        whipSettings_.tipRoundness = json.value("tipRoundness", whipSettings_.tipRoundness);

    // 設定に不正なJSONや型があった場合は、途中まで読み込んだ値も既定値へ戻す。
    } catch (const nlohmann::json::exception&) {
        whipSettings_ = {};
    }
    Sanitize(whipSettings_);
}
bool Player::SaveWhipSettings() const {
    // 書き終えた一時ファイルを置き換え、保存失敗で前回の設定を空にしません。
    std::error_code error;
    const std::filesystem::path destination(kSettingsPath);
    std::filesystem::create_directories(destination.parent_path(), error);
    // 親ディレクトリを準備できなければ、保存を開始しない。
    if (error) return false;
    // 書き込み途中に正式な設定を参照できるよう、同じフォルダーにプロセスID付きの一時ファイルを用意します。
    const std::filesystem::path temporary = destination.wstring() + L".tmp." + std::to_wstring(GetCurrentProcessId());
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        // 一時ファイルを開けない場合は、元の設定を保持して終了する。
        if (!stream) return false;
        stream << nlohmann::json{{"maxDistance", whipSettings_.maxDistance},
            {"shootSpeed", whipSettings_.shootSpeed}, {"returnSpeed", whipSettings_.returnSpeed},
            {"hitRadius", whipSettings_.hitRadius},
            {"windupDuration", whipSettings_.windupDuration},
            {"sweepDuration", whipSettings_.sweepDuration},
            {"recoverDuration", whipSettings_.recoverDuration},
            {"sweepAngleDegrees", whipSettings_.sweepAngleDegrees},
            {"bendDegrees", whipSettings_.bendDegrees},
            {"bodyTwistDegrees", whipSettings_.bodyTwistDegrees},
            {"bodySquash", whipSettings_.bodySquash},
            {"tipDelay", whipSettings_.tipDelay},
            {"bindBlendDuration", whipSettings_.bindBlendDuration},
            {"pullBendRatio", whipSettings_.pullBendRatio},
            {"groundSkimTolerance", whipSettings_.groundSkimTolerance},
            {"bodyShear", whipSettings_.bodyShear},
            {"bodyStretch", whipSettings_.bodyStretch},
            {"captureBrace", whipSettings_.captureBrace},
            {"bodyRebound", whipSettings_.bodyRebound},
            {"surfaceRootAngle", whipSettings_.surfaceRootAngle},
            {"surfaceLift", whipSettings_.surfaceLift},
            {"contactDuration", whipSettings_.contactDuration},
            {"returnSettleDuration", whipSettings_.returnSettleDuration},
            {"launchFlow", whipSettings_.launchFlow},
            {"launchDraw", whipSettings_.launchDraw},
            {"launchMound", whipSettings_.launchMound},
            {"returnReceive", whipSettings_.returnReceive},
            {"returnBulge", whipSettings_.returnBulge},
            {"contactSquash", whipSettings_.contactSquash},
            {"tipRoundness", whipSettings_.tipRoundness},
            {"thickness", whipSettings_.thickness}}.dump(2);
        stream.flush();
        // 一時ファイルを最後まで書けなければ、そのファイルを消し置換しない。
        if (!stream) { stream.close(); std::filesystem::remove(temporary, error); return false; }
    }
    // 一時ファイルから設定を置換できた場合だけ、保存成功を返す。
    if (MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    // ファイルの置換が失敗した場合は、残った一時出力を削除して失敗を返します。
    std::filesystem::remove(temporary, error);
    return false;
}
void Player::DrawWhipEditor() {
#ifdef USE_IMGUI
    // ウィップ欄が閉じている場合は、調整用ウィジェットを描画しない。
    if (!ImGui::CollapsingHeader("スライムウィップ（通常形態）")) return;
    // 同じ項目名でも、選んでいるプレイヤーごとに編集ウィジェットのIDを分ける。
    ImGui::PushID(this);
    ImGui::DragFloat("射程", &whipSettings_.maxDistance, 0.5f, 1.0f, 200.0f);
    ImGui::TextUnformatted("右から左へ薙ぎ払い、最初に触れた対象を捕獲します。");
    ImGui::DragFloat("鞭の判定半径（太さ以上）", &whipSettings_.hitRadius, 0.02f, 0.1f, 3.0f);
    // 動作の時間と角度に続けて、先端や本体の見た目を調整する。
    ImGui::DragFloat("振りかぶり時間（秒）", &whipSettings_.windupDuration, 0.01f, 0.02f, 0.4f);
    ImGui::DragFloat("薙ぎ払い時間（秒）", &whipSettings_.sweepDuration, 0.01f, 0.06f, 0.8f);
    ImGui::DragFloat("復帰時間（秒）", &whipSettings_.recoverDuration, 0.01f, 0.04f, 0.6f);
    ImGui::DragFloat("薙ぎ払い角度", &whipSettings_.sweepAngleDegrees, 0.5f, 30.0f, 170.0f);
    ImGui::DragFloat("しなり角度", &whipSettings_.bendDegrees, 0.5f, 0.0f, 45.0f);
    ImGui::DragFloat("体のひねり角度", &whipSettings_.bodyTwistDegrees, 0.5f, 0.0f, 65.0f);
    ImGui::DragFloat("体の潰れ量", &whipSettings_.bodySquash, 0.01f, 0.0f, 0.35f);
    ImGui::DragFloat("鞭の根元の太さ", &whipSettings_.thickness, 0.01f, 0.1f, 1.5f);
    ImGui::DragFloat("先端の遅れ（秒）", &whipSettings_.tipDelay, 0.005f, 0.0f, 0.12f);
    // ここからは接触や引き寄せの曲線、本体の受け止め方を調整する。
    ImGui::DragFloat("命中後のなじみ時間（秒）", &whipSettings_.bindBlendDuration, 0.01f, 0.04f, 0.4f);
    ImGui::DragFloat("引き寄せ中のしなり比率", &whipSettings_.pullBendRatio, 0.01f, 0.0f, 0.3f);
    ImGui::DragFloat("上半身のしなり", &whipSettings_.bodyShear, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("薙ぎ払いの横伸び", &whipSettings_.bodyStretch, 0.01f, 0.0f, 0.6f);
    ImGui::DragFloat("捕獲時の踏ん張り", &whipSettings_.captureBrace, 0.01f, 0.0f, 0.8f);
    ImGui::DragFloat("本体の戻り反動", &whipSettings_.bodyRebound, 0.01f, 0.0f, 0.3f);
    ImGui::DragFloat("体表から伸びる範囲", &whipSettings_.surfaceRootAngle, 0.5f, 35.0f, 65.0f);
    ImGui::DragFloat("先端の丸み", &whipSettings_.tipRoundness, 0.01f, 0.0f, 0.4f);
    ImGui::DragFloat("接触時の潰れ量", &whipSettings_.contactSquash, 0.01f, 0.0f, 0.8f);
    ImGui::DragFloat("先端の密着時間（秒）", &whipSettings_.contactDuration, 0.01f, 0.06f, 0.35f);
    ImGui::DragFloat("戻りの膨らみ", &whipSettings_.returnBulge, 0.01f, 0.0f, 0.8f);
    ImGui::DragFloat("本体の受け止め量", &whipSettings_.returnReceive, 0.01f, 0.0f, 0.4f);
    ImGui::DragFloat("戻りの余韻時間（秒）", &whipSettings_.returnSettleDuration, 0.01f, 0.08f, 0.4f);
    ImGui::DragFloat("振り始めの盛り上がり", &whipSettings_.launchMound, 0.01f, 0.0f, 0.35f);
    ImGui::DragFloat("本体の引き締め", &whipSettings_.launchDraw, 0.01f, 0.0f, 0.2f);
    ImGui::DragFloat("送り出す厚み", &whipSettings_.launchFlow, 0.01f, 0.0f, 0.5f);
    ImGui::DragFloat("付け根の上寄せ", &whipSettings_.surfaceLift, 0.01f, 0.8f, 1.8f);
    ImGui::DragFloat("床をかすめる許容量", &whipSettings_.groundSkimTolerance, 0.01f, 0.0f, 1.0f);
    ImGui::TextWrapped("0で床接触も停止。床上面の浅い接触だけを許容し、実効値は判定半径の75%%以内です。");
    // UI操作で入力された値を、次の鞭の動作に使う前に補正する。
    Sanitize(whipSettings_);
    ImGui::TextWrapped("次の一振りから反映。保存すると次回生成するPlayerにも適用します。");
    // 保存ボタンを押した場合だけ設定を書き、結果に応じたポップアップを予約する。
    if (ImGui::Button("鞭設定を保存")) {
        ImGui::OpenPopup(SaveWhipSettings() ? "保存完了" : "保存失敗");
    }
    ImGui::SameLine();
    // 再読込ボタンを押した場合だけ、保存済み設定を取り込む。
    if (ImGui::Button("鞭設定を再読込")) LoadWhipSettings();
    for (const char* popup : {"保存完了", "保存失敗"}) {
        // 今開いている結果ポップアップだけを表示する。
        if (ImGui::BeginPopup(popup)) { ImGui::TextUnformatted(popup); ImGui::EndPopup(); }
    }
    ImGui::TextUnformatted("保存先: Resources/json/player/whip.json");
    ImGui::PopID();
#endif
}

namespace {
constexpr float kPi = std::numbers::pi_v<float>;
// 長さのない方向は正規化できないため、呼び出し側が指定する基準方向へ置き換えます。
engine::Vector3 SafeUnit(const engine::Vector3& v, const engine::Vector3& fallback) {
    const float length = engine::Math::Length(v);
    return length > 0.0001f ? v / length : fallback;
}
}

float PlayerWhipMotion::LaunchDuration(const PlayerWhipSettings& s) {
    // 振りかぶりと薙ぎ払いの最初の25%を合計し、射程まで伸びる秒数を返す。
    return s.windupDuration + s.sweepDuration * 0.25f;
}

// 振りかぶり・薙ぎ払い・戻りを合計した動作時間（秒）を返します。
float PlayerWhipMotion::Duration(const PlayerWhipSettings& s) {
    return s.windupDuration + s.sweepDuration + s.recoverDuration;
}
PlayerWhipMotion::Curve PlayerWhipMotion::Evaluate(const PlayerWhipSettings& s, const engine::Vector3& forward, float time) {
    // 入力の向きと経過秒から、根元のワールド原点を足す前の各曲線点を計算します。
    const auto f = SafeUnit(forward, {0, 0, 1});
    const auto right = SafeUnit({f.z, 0, -f.x}, {1, 0, 0});
    float length = 0.0f;
    float bend = 0.0f;
    // 振りかぶり中は射程の4分の1まで伸ばし、しなりを強める。
    if (time < s.windupDuration) {
        const float u = Easing::SmoothStep01(time / s.windupDuration);
        length = s.maxDistance * 0.25f * u;
        bend = s.bendDegrees * u;
    // 薙ぎ払い中は序盤で射程いっぱいに伸ばし、振り抜くにつれて弧をほどく。
    } else if (time < s.windupDuration + s.sweepDuration) {
        const float u = std::clamp((time - s.windupDuration) / s.sweepDuration, 0.0f, 1.0f);
        length = s.maxDistance * (0.25f + 0.75f * Easing::SmoothStep01(u / 0.25f));
        bend = s.bendDegrees * (1.0f - Easing::SmoothStep01(u));
    // 薙ぎ払い後は先端を手元へ戻しながら、逆側へ短く反らせる。
    } else {
        const float u = Easing::SmoothStep01((time - s.windupDuration - s.sweepDuration) / s.recoverDuration);
        length = s.maxDistance * (1.0f - u);
        bend = -s.bendDegrees * std::sin(u * kPi);
    }
    // 曲線全長の各位置に、しなりと先端側の角度の遅れを配分します。
    Curve curve{};
    for (size_t i = 0; i <= kSegments; ++i) {
        const float v = static_cast<float>(i) / static_cast<float>(kSegments);
        // 先端ほど過去の角度を使います。復帰では遅れを解消し、終了時に尾を残しません。
        const float recovery = Easing::SmoothStep01((time - s.windupDuration - s.sweepDuration) / s.recoverDuration);
        const float sweepProgress = std::clamp((time - s.windupDuration) / s.sweepDuration, 0.0f, 1.0f);
        const float delay = (std::min)(s.tipDelay, s.sweepDuration * 0.45f) * v * v * std::sin(kPi * sweepProgress);
        const float delayedSweep = Easing::SmoothStep01((time - delay - s.windupDuration) / s.sweepDuration);
        const float delayedAngle = s.sweepAngleDegrees * 0.5f * (1.0f - 2.0f * delayedSweep)
            - 20.0f * std::sin(recovery * kPi);
        const float a = std::clamp(delayedAngle + bend * v * v * (1.0f - std::abs(delayedAngle) / 90.0f), -85.0f, 85.0f) * kPi / 180.0f;
        curve[i] = (f * std::cos(a) + right * std::sin(a)) * (length * v);
    }
    return curve;
}

// 経過秒数に応じて、鞭を振ったときの本体の変形・回転・せん断を求めます。
BodyAnimationClip::Sample PlayerWhipMotion::EvaluateBody(const PlayerWhipSettings& s, float time) {
    BodyAnimationClip::Sample pose;
    float twist = 0.0f;
    // 振りかぶり中は本体を圧縮し、鞭を振る側へひねる。
    if (time < s.windupDuration) {
        const float u = Easing::SmoothStep01(time / s.windupDuration);
        pose.scale = {1.0f + s.bodySquash * 0.45f * u, 1.0f - s.bodySquash * u, 1.0f};
        pose.shear = {s.bodyShear * 0.75f * u, 0, -s.bodyShear * 0.12f * u};
        twist = u;
    // 薙ぎ払い中は体を伸ばしながら、ひねりを反対側へ移す。
    } else if (time < s.windupDuration + s.sweepDuration) {
        const float u = Easing::SmoothStep01((time - s.windupDuration) / s.sweepDuration);
        const float arc = std::sin(kPi * u);
        pose.scale = {1.0f + s.bodySquash * 0.45f * (1.0f-u) + s.bodyStretch * arc,
                      1.0f - s.bodySquash * (1.0f-u) - s.bodyStretch * 0.28f * arc, 1.0f};
        pose.shear = {s.bodyShear * (0.75f - 1.75f * u), 0,
                      s.bodyShear * (-0.12f * (1.0f-u) + 0.28f * arc)};
        twist = 1.0f - 2.0f * u;
    // 薙ぎ払い後は傾きと揺れを弱め、基準形状へ戻す。
    } else {
        const float u = std::clamp((time-s.windupDuration-s.sweepDuration)/s.recoverDuration,0.0f,1.0f);
        const float recoil = s.bodyRebound * std::sin(kPi * std::clamp((u-0.35f)/0.65f,0.0f,1.0f));
        const float wobble = s.bodyRebound * std::sin(2.0f*kPi*u) * (1.0f-u);
        pose.scale = {1.0f + wobble * 0.4f, 1.0f - wobble * 0.65f, 1.0f};
        pose.shear = {-s.bodyShear * (1.0f-Easing::SmoothStep01(u/0.55f)) + recoil, 0, 0};
        twist = -(1.0f-Easing::SmoothStep01(u));
    }
    // 付け根へ量を集める間だけ押し縮め、振り抜き開始までに元の姿勢へつなぎます。
    const float gather = time >= 0.0f && time < s.windupDuration ? std::sin(kPi * time / s.windupDuration) : 0.0f;
    pose.scale.x += s.launchMound * 0.05f * gather;
    pose.scale.y -= s.launchMound * 0.10f * gather;
    // 横へ伸びる分だけ奥行きを補正し、急に体積が増える印象を抑えます。
    pose.scale.z = 1.0f / (std::max)(pose.scale.x * pose.scale.y, 0.1f);
    pose.rotate.y = twist * s.bodyTwistDegrees * kPi / 180.0f;
    return pose;
}

// アニメーターに残った鞭の姿勢と、描画モデルへの追加変形を解除します。
void Player::ClearWhipBodyPose() {
    slimeAnimator_.ClearWhipPose();
    // 描画モデルがあれば、行列に残っている一時変形もリセットする。
    if (GetMeshRenderer()) GetMeshRenderer()->ResetVisualTransform();
}

// その時点の設定と狙いを保存して、操作を止めてから鞭を出します。
void PlayerStateWhip::Enter(Player* player) {
    // プレイヤーが無ければ、曲線の原点と描画する本体を用意できない。
    if (!player) return;
    // 振り始めの設定と向きを控え、動作中の設定変更や照準移動で曲線が飛ばないようにします。
    settings_ = player->GetWhipSettings();
    Sanitize(settings_);
    origin_ = player->GetWhipOrigin();
    direction_ = SafeUnit(direction_, player->GetForwardDirection());
    player->SetIsControlActive(false);
    player->SetVelocity({0, 0, 0});
    player->SetSlimeAnimationMode(PlayerSlimeAnimator::Mode::kShootHook);
    player->SetSlimeAnimationDirection(direction_);
    // 引き寄せや帰還へ渡せるよう、曲線の表示を共有所有の実体として用意します。
    visual_ = std::make_shared<PlayerWhipVisual>();
    visual_->Initialize(player, settings_);
    player->SetActiveWhipVisual(visual_);
    player->SetSlimePullProgress(0.0f);
    UpdateBodyPose(player, elapsed_);
    UpdateVisual(player);
}

// 遮られた時点の曲線を保存し、以後の更新ではこの形を縮めます。
void PlayerStateWhip::BeginRecovery() {
    blocked_ = true;
    blockedAt_ = elapsed_;
    blockedCurve_ = curve_;
}

// 命中先を引き寄せられるか調べます。別の状態へ移ったときにtrueを返します。
bool PlayerStateWhip::HandleHit(Player* player, engine::Object3D* object, const engine::Vector3& point) {
    // 命中時の見た目を保存し、別の状態へ変わっても曲線や姿勢が急に切り替わらないようにする。
    visual_->CaptureBodyPose(player->GetWhipBodyPose());
    // 触れた部品が子の場合は親まで辿り、実際に引ける本体を探す。
    for (auto* target = object; target; target = target->GetParent()) {
        // 引けるブロックなら曲線を渡して、オブジェクト用の引き寄せ状態へ切り替える。
        if (auto* block = dynamic_cast<GimmickHookPullBlock*>(target)) {
            visual_->BeginPull(curve_, origin_, block->GetWorldPosition(), point);
            visual_->Draw(player, curve_, origin_);
            player->ChangeState(std::make_unique<PlayerStatePullObject>(block, point, true, visual_));
            return true;
        }
        // 敵本体に到達した場合は、引き寄せの可否を調べる。
        if (auto* enemy = dynamic_cast<BaseEnemy*>(target)) {
            // 引き寄せ無効・死亡・リプレイ上の除去に該当しない敵だけを捕まえる。
            if (!enemy->IsPullImmune() && !enemy->IsDead() && !enemy->IsReplayRemoved()) {
                // 巨大スライムは専用の取付点、通常の敵はモデル中心または原点を使う。
                engine::Vector3 anchor;
                if (const auto* giant = dynamic_cast<EnemyGiantSlime*>(enemy)) {
                    anchor = giant->GetHookAttachmentPosition();
                } else if (const auto* model = enemy->GetModel()) {
                    anchor = engine::Math::Transform(model->GetCenter(), enemy->GetWorldMatrix());
                } else {
                    anchor = enemy->GetWorldPosition();
                }
                visual_->BeginPull(curve_, origin_, anchor, point);
                visual_->Draw(player, curve_, origin_);
                player->ChangeState(std::make_unique<PlayerStatePullEnemy>(enemy, point, true, visual_));
                return true;
            }
            // 捕まえられない敵にも命中の表示は残し、掴まず復帰へ移る。
            HitEffectDirector::SpawnPullBindHit(point);
            break;
        }
    }
    // 接触相手の階層から引き寄せ先を決められなかった場合は、短く切った曲線を復帰用に保存します。
    BeginRecovery();
    return false;
}

// 今フレームに鞭が触れ得る候補と、そのワールド空間の境界を集めます。
void PlayerStateWhip::CollectCandidates(Player* player) {
    queryCandidates_.clear();
    const auto mask = engine::collisionCategories::Get("ground") | engine::collisionCategories::Get("enemy") | engine::collisionCategories::Get("hook_anchor");
    // ワールド形状は一更新につき一度取得し、多数の曲線サンプル間で再利用します。
    for (const auto* object : engine::CollisionManager::GetInstance()->GetObjects()) {
        // 参照が無い、自分自身、または対象の衝突属性が無い場合は候補に含めない。
        if (!object || object == player || !(object->GetCollisionAttribute() & mask)) continue;
        const auto type = object->GetColliderType();
        bool special = type != engine::ColliderType::kAabb && type != engine::ColliderType::kObb;
        engine::Aabb bounds = object->GetAabb();
        // 球の候補は、中心と半径から球全体を囲むAABBを作る。
        if (type == engine::ColliderType::kSphere) {
            const auto center = object->GetCollider()->GetObb().center;
            const float radius = std::abs(object->GetCollider()->GetRadius());
            const engine::Vector3 extent{radius, radius, radius};
            bounds = {center - extent, center + extent}; special = false;
        // 円柱は軸の半分の長さと半径から、傾きも含む外側のAABBを求める。
        } else if (type == engine::ColliderType::kCylinder) {
            const auto cylinder = object->GetCollider()->GetCylinder();
            const float half = std::abs(cylinder.height) * 0.5f;
            const float radius = std::abs(cylinder.radius);
            const engine::Vector3 extent{std::abs(cylinder.axis.x) * half + radius,
                std::abs(cylinder.axis.y) * half + radius, std::abs(cylinder.axis.z) * half + radius};
            bounds = {cylinder.center - extent, cylinder.center + extent}; special = false;
        }
        // 境界だけで選別できない形状には印を残し、後の実判定から落ちないようにします。
        queryCandidates_.push_back({object, bounds, special});
    }
}

// 鞭の曲線を根元から球でなぞります。命中によって別の状態へ移ったときだけtrueを返します。
bool PlayerStateWhip::CheckCurveHit(Player* player, const PlayerWhipMotion::Curve& curve, const engine::Vector3& origin, float time) {
    engine::PhysicsQueryFilter filter;
    filter.mask = engine::collisionCategories::Get("ground") | engine::collisionCategories::Get("enemy") | engine::collisionCategories::Get("hook_anchor");
    filter.ignoredObject = player;
    filter.includeTriggers = true;
    filter.refineSphereCastHit = true;
    const float radius = (std::max)(settings_.hitRadius, settings_.thickness);
    // 床上面のかすりだけを無視する閾値です。球判定の実効半径の75%以内に制限します。
    const float skimDepth = (std::min)(settings_.groundSkimTolerance, radius * 0.75f);
    const auto ground = engine::collisionCategories::Get("ground");
    filter.acceptsSphereContact = [skimDepth, ground](const engine::Object3D* object, const engine::CollisionInfo& contact) {
        // 約45度までの上面だけを対象にし、側面・天井・深い接触は残します。
        if (skimDepth <= 0.0f || !(object->GetCollisionAttribute() & ground) ||
            !(contact.normal.y >= 0.707107f) || !(contact.penetration >= 0.0f && contact.penetration <= skimDepth)) return true;
        // ブロックはground属性でも捕獲対象。子コライダーも親をたどって保護します。
        for (auto* target = object; target; target = target->GetParent()) {
            // 浅い床接触でも、引き寄せ対象のブロックや敵に属する部品なら命中を残す。
            if (dynamic_cast<const GimmickHookPullBlock*>(target) || dynamic_cast<const BaseEnemy*>(target)) return true;
        }
        return false;
    };
    // 根元から順に調べ、壁を越えた曲線上の敵を捕まえません。
    for (size_t i = 0; i < PlayerWhipMotion::kSegments; ++i) {
        const auto start = origin + curve[i];
        const auto end = origin + curve[i + 1];
        const auto delta = end - start;
        const float distance = engine::Math::Length(delta);
        // 点間距離がほぼ0の区間は、掃引する方向が無いので飛ばす。
        if (distance < 0.0001f) continue;
        const float r = radius;
        const engine::Aabb sweep{{(std::min)(start.x, end.x) - r, (std::min)(start.y, end.y) - r, (std::min)(start.z, end.z) - r},
                         {(std::max)(start.x, end.x) + r, (std::max)(start.y, end.y) + r, (std::max)(start.z, end.z) + r}};
        std::unordered_set<const engine::Object3D*> candidates;
        for (const auto& candidate : queryCandidates_) {
            const auto* object = candidate.object;
            // 特別な形状は境界による絞り込みを使わず、候補に残す。
            if (candidate.special) { candidates.insert(object); continue; }
            const auto& bounds = candidate.bounds;
            // 通常の候補は、鞭の区間を囲んだAABBと重なる場合だけ球の判定へ渡す。
            if (bounds.min.x <= sweep.max.x && bounds.max.x >= sweep.min.x &&
                bounds.min.y <= sweep.max.y && bounds.max.y >= sweep.min.y &&
                bounds.min.z <= sweep.max.z && bounds.max.z >= sweep.min.z) candidates.insert(object);
        }
        // この区間に候補が無ければ、球の掃引を呼ぶ必要がない。
        if (candidates.empty()) continue;
        filter.acceptsObject = [&candidates](const engine::Object3D* o) { return candidates.contains(o); };
        const auto hit = engine::CollisionManager::GetInstance()->SphereCast(start, r, delta / distance, distance, filter);
        // 命中点と対象の両方を取得できなければ、次の区間を調べる。
        if (!hit.isHit || !hit.hitObject) continue;
        // 最初の命中へ曲線を切り詰め、接触先より奥に描画を伸ばしません。
        // 当たった区間の番号に、その区間の何割まで進んだかを足し、切り詰める先端を小数の添字で表します。
        const float contactIndex = static_cast<float>(i) + hit.distance / distance;
        for (size_t j = 0; j < curve_.size(); ++j) {
            const float sample = contactIndex * static_cast<float>(j) / static_cast<float>(PlayerWhipMotion::kSegments);
            const auto index = (std::min)(static_cast<size_t>(sample), PlayerWhipMotion::kSegments - 1);
            curve_[j] = curve[index] + (curve[index + 1] - curve[index]) * (sample - static_cast<float>(index));
        }
        origin_ = origin;
        UpdateBodyPose(player, time);
        return HandleHit(player, hit.hitObject, hit.hitPoint);
    }
    return false;
}

// 薙ぎ払い区間での命中を先に調べ、残った状態で姿勢と鞭の形を更新します。
void PlayerStateWhip::Update(Player* player, float deltaTime) {
    // プレイヤーがいないか、経過秒数が有限かつ正でない場合は演出を進めない。
    if (!player || !std::isfinite(deltaTime) || deltaTime <= 0.0f) return;
    // このフレームに進む時刻と、鞭が攻撃判定を持つ区間との重なりを求める。
    const float endTime = (std::min)(elapsed_ + deltaTime, PlayerWhipMotion::Duration(settings_));
    const auto currentOrigin = player->GetWhipOrigin();
    const float sweepEnd = settings_.windupDuration + settings_.sweepDuration;
    const float from = (std::max)(elapsed_, settings_.windupDuration);
    const float to = (std::min)(endTime, sweepEnd);
    // 遮られておらず、有効時間を通過した場合だけ衝突候補を調べる。
    if (!blocked_ && to >= from && endTime >= settings_.windupDuration) {
        CollectCandidates(player);
        // 半径の半分以内の先端移動に分割。低FPSでも曲線間を飛ばさず調べます。
        const float speedBound = settings_.maxDistance *
            (4.5f + 4.0f * (settings_.sweepAngleDegrees + settings_.bendDegrees) * kPi / 180.0f) / settings_.sweepDuration;
        const float movementSpeed = engine::Math::Length(currentOrigin - origin_) / deltaTime;
        // 鞭先そのものの速度上限に持ち手の移動速度を加え、判定半径の半分だけ進むのに掛かる秒数を求めます。
        const float step = settings_.hitRadius * 0.5f / (std::max)(speedBound + movementSpeed, 0.001f);
        const std::int32_t count = (std::max)(1, static_cast<std::int32_t>(std::ceil((to - from) / step)));
        for (std::int32_t i = 0; i <= count; ++i) {
            const float t = from + (to - from) * static_cast<float>(i) / static_cast<float>(count);
            // 先端だけでなく持ち手も移動するため、調べる時刻に対応する原点を補間する。
            const auto queryOrigin = origin_ + (currentOrigin - origin_) * std::clamp((t - elapsed_) / deltaTime, 0.0f, 1.0f);
            const auto curve = PlayerWhipMotion::Evaluate(settings_, direction_, t);
            // 命中で別の状態へ移った場合は、この状態が既に破棄されているため、直ちに更新を終える。
            if (CheckCurveHit(player, curve, queryOrigin, t)) return;
            // 遮られた場合はそのサンプル時刻を保存し、以降の命中判定を打ち切る。
            if (blocked_) { blockedAt_ = t; break; }
        }
    }
    // 判定を終えてから時刻と原点を確定し、次のフレームで同じ区間を判定しない。
    elapsed_ = endTime;
    origin_ = currentOrigin;
    const float recovery = blocked_ ? (elapsed_ - blockedAt_) / settings_.recoverDuration : 0.0f;
    // 時間切れか遮られた後の戻りが完了したら、見た目の残りを Player に渡して待機へ移る。
    if (elapsed_ >= PlayerWhipMotion::Duration(settings_) || (blocked_ && recovery >= 1.0f)) {
        player->BeginWhipReturn(visual_);
        player->ChangeState(std::make_unique<PlayerStateIdle>());
        return;
    }
    // 遮られた後は、命中した時点の形のまま縮める。
    if (blocked_) {
        for (size_t i = 0; i < curve_.size(); ++i) curve_[i] = blockedCurve_[i] * (1.0f - Easing::SmoothStep01(recovery));
    // 遮られていない場合は、現在時刻の通常の曲線を使う。
    } else curve_ = PlayerWhipMotion::Evaluate(settings_, direction_, elapsed_);
    player->SetSlimeAnimationMode(PlayerSlimeAnimator::Mode::kShootHook);
    UpdateBodyPose(player, elapsed_);
    UpdateVisual(player);
}

void PlayerStateWhip::UpdateBodyPose(Player* player, float time) {
    // 専用表示がある場合は、鞭の振り始めの進行率と、遮られた後の減衰量を渡す。
    if (visual_) {
        const float phaseTime = blocked_ ? blockedAt_ : time;
        const float weight = blocked_ ? 1.0f-Easing::SmoothStep01((time-blockedAt_)/settings_.recoverDuration) : 1.0f;
        visual_->SetLaunchProgress(phaseTime / PlayerWhipMotion::LaunchDuration(settings_), weight);
    }
    auto pose = PlayerWhipMotion::EvaluateBody(settings_, blocked_ ? blockedAt_ : time);
    // 遮られた場合は、その時刻の拡縮・回転・せん断を復帰時間で基準姿勢へ戻す。
    if (blocked_) {
        const float weight = 1.0f - Easing::SmoothStep01((time-blockedAt_)/settings_.recoverDuration);
        pose.scale = engine::Vector3{1,1,1} + (pose.scale-engine::Vector3{1,1,1})*weight;
        pose.rotate = pose.rotate * weight;
        pose.shear = pose.shear * weight;
    }
    // 操作の左右をモデル固有の正面補正に合わせます。
    pose.shear = engine::Math::TransformNormal(pose.shear,
        engine::Math::MakeRotateYMatrix(-player->GetVisualYawOffset()));
    player->SetWhipBodyPose(pose.scale, pose.rotate, pose.shear);
}

// この状態の鞭の曲線を描画側へ渡します。
void PlayerStateWhip::UpdateVisual(Player* player) {
    // 専用表示がある場合にだけ、遮られた状態や薙ぎ払い終了を戻りとして扱う。
    if (visual_) {
        visual_->SetReturning(blocked_ || elapsed_ >= settings_.windupDuration + settings_.sweepDuration);
        visual_->Draw(player, curve_, player->GetWhipOrigin());
    }
}
// 通常入力を戻し、鞭を次の状態へ引き渡すか片付けます。
void PlayerStateWhip::Exit(Player* player) {
    // プレイヤーが無ければ入力や描画を復元できない。
    if (!player) return;
    player->SetIsControlActive(true);
    // 鞭の表示をこの状態だけが所有している場合に終了する。共有している場合は引き渡す。
    if (visual_ && visual_.use_count() == 1) visual_->Finish(player);
    // 専用表示が無ければ、本体の鞭用姿勢を直接消す。
    else if (!visual_) player->ClearWhipBodyPose();
    visual_.reset();
}

// 現在の本体モデルに対応する鞭変形を取得します。初期化できなければ空を返します。
std::shared_ptr<PlayerWhipDeformer> Player::AcquireWhipDeformer() {
    // 未作成か、コピー形態などで元のモデルが変わった場合は作り直す。
    if (!whipDeformer_ || whipDeformer_->GetSource() != GetModel()) {
        auto next = std::make_shared<PlayerWhipDeformer>();
        // 現在のモデルから変形用データを準備できなければ、新しい変形を登録しない。
        if (!next->Initialize(GetModel(), ModelManager::GetInstance()->GetModelCommon())) return {};
        whipDeformer_ = std::move(next);
    }
    return whipDeformer_;
}

// 今回の設定を保持し、プレイヤーの現在のモデルに対応する描画専用の変形器を借ります。
void PlayerWhipVisual::Initialize(Player* player, const PlayerWhipSettings& settings) {
    settings_ = settings;
    // 本体の複製をPlayer内で再利用し、連打時にGPUバッファを作り直しません。
    deformer_ = player->AcquireWhipDeformer();
    // 共通マーカーがあれば、専用表示と重ならないよう隠す。
    if (auto* marker = player->GetHookMarker()) marker->SetIsVisible(false);
}

// 振り始めの進行と強さを、描画に使う0〜1の値へ補正します。
void PlayerWhipVisual::SetLaunchProgress(float progress, float weight) {
    launchProgress_ = std::isfinite(progress) ? std::clamp(progress, 0.0f, 1.0f) : 1.0f;
    launchWeight_ = std::isfinite(weight) ? std::clamp(weight, 0.0f, 1.0f) : 0.0f;
}

// このフレームに描く曲線と原点を保持し、必要なら戻り量も記録します。
void PlayerWhipVisual::Draw(Player*, const PlayerWhipMotion::Curve& curve, const engine::Vector3& origin) {
    curve_ = curve;
    renderOrigin_ = origin;
    const auto launchAxis = curve[PlayerWhipMotion::kSegments / 3] - curve.front();
    // 根元付近である程度長さがある場合だけ、振り出す方向を更新する。
    if (engine::Math::Length(launchAxis) > 0.00001f) launchDirection_ = SafeUnit(launchAxis, launchDirection_);
    const float reach = engine::Math::Length(curve.back());
    peakReach_ = (std::max)(peakReach_, reach);
    // 戻り中は先端の距離が縮んだ割合を記録し、これまでの進行より後退させない。
    if (recovering_) {
        returnProgress_ = (std::max)(returnProgress_, 1.0f - reach / (std::max)(peakReach_, 0.001f));
        returnAmount_ = std::clamp(peakReach_ / settings_.maxDistance, 0.0f, 1.0f);
    }
    // 重い頂点更新はPlayerの本体アニメーション完了後に一度だけ行います。
}

void PlayerWhipVisual::RefreshAttachment(Player* player) {
    // 本体・変形データ・描画モデルのどれかが無ければ、頂点を変形できない。
    if (!player || !deformer_ || !player->GetMeshRenderer()) return;
    // 見た目の変形後に生じた根元のずれを補い、先端に近づくほど補正を弱めて命中点をずらさないようにします。
    const auto rootDelta = player->GetWhipVisualOrigin() - renderOrigin_;
    for (size_t i = 0; i < curve_.size(); ++i) {
        const float remain = 1.0f - static_cast<float>(i) / PlayerWhipMotion::kSegments;
        renderCurve_[i] = curve_[i] + rootDelta * (remain * remain * remain);
    }
    // 変形器の元モデルを確認してから、補正した曲線と付け根・接触・戻りの設定を頂点へ反映します。
    auto* renderer = player->GetMeshRenderer();
    // コピーで本体が変わったら旧モデルを変形させず、現在の描画モデルが一致する場合だけ頂点を更新します。
    if (deformer_->GetSource() != renderer->GetModel()) return;
    deformer_->Update(renderCurve_, renderOrigin_, renderer->GetRenderWorldMatrix(),
                      settings_.surfaceRootAngle, settings_.surfaceLift, settings_.thickness,
                      (std::min)(settings_.tipRoundness, settings_.hitRadius * 0.6f),
                      contactWeight_ * settings_.contactSquash, contactNormal_,
                      returnProgress_, settings_.returnBulge * returnAmount_ * returnFade_,
                      settings_.returnReceive * returnAmount_ * returnFade_,
                      {launchProgress_, settings_.launchMound * launchWeight_, settings_.launchDraw * launchWeight_,
                       settings_.launchFlow * launchWeight_, launchDirection_});
    renderer->SetVisualModelOverride(deformer_->GetModel());
}

void PlayerWhipVisual::BeginPull(const PlayerWhipMotion::Curve& curve, const engine::Vector3& origin,
                                const engine::Vector3& targetPosition, const engine::Vector3& contactPoint) {
    // 命中瞬間の曲線と対象からのずれを退避し、接触点を引き寄せの開始形状に使います。
    hitCurve_ = curve;
    targetOffset_ = origin + curve.back() - targetPosition;
    contactOffset_ = contactPoint - targetPosition;
    pullElapsed_ = 0.0f;
    contacting_ = true;
    contactWeight_ = 0.0f;
    contactNormal_ = SafeUnit(contactPoint - targetPosition, SafeUnit(origin - targetPosition, {0, 1, 0}));
}

// 命中時のしなりを徐々に弱め、引き寄せる対象へ張った曲線にします。
void PlayerWhipVisual::UpdatePull(Player* player, const engine::Vector3& targetPosition, float deltaTime) {
    // プレイヤーが無ければ根元を計算できない。
    if (!player) return;
    // 経過時間が有限であれば、正の分だけ捕獲後の時計を進める。
    if (std::isfinite(deltaTime)) pullElapsed_ += (std::max)(deltaTime, 0.0f);
    // 命中時の初動を保持したまま薄め、捕獲した瞬間に体表を戻しません。
    launchWeight_ = 1.0f - Easing::SmoothStep01(pullElapsed_ / (std::min)(0.08f, settings_.bindBlendDuration));
    // 接触直後に短く潰し、引き寄せへ移るにつれて通常の丸い先端に戻します。
    const float contactTime = pullElapsed_ / settings_.contactDuration;
    contactWeight_ = contacting_ ? Easing::SmoothStep01(contactTime / 0.2f) *
        (1.0f - Easing::SmoothStep01((contactTime - 0.4f) / 0.6f)) : 0.0f;
    // 命中した瞬間の姿勢から、対象方向へ引かれた後に踏ん張る姿勢へ移します。
    const float bodyBlend = Easing::SmoothStep01(pullElapsed_ / settings_.bindBlendDuration);
    const float finishWeight = 1.0f-Easing::SmoothStep01((player->GetSlimePullProgress()-0.65f)/0.35f);
    const auto direction = SafeUnit(engine::Vector3{targetPosition.x-player->GetWorldPosition().x,0,
        targetPosition.z-player->GetWorldPosition().z},{0,0,1});
    const auto localDirection = engine::Math::TransformNormal(direction,
        engine::Math::MakeRotateYMatrix(-player->GetRotation().y));
    const float strain = settings_.captureBrace * (1.0f + 0.06f*std::sin(pullElapsed_*6.0f*kPi));
    const float catchLean = 0.45f-1.1f*Easing::SmoothStep01(pullElapsed_/0.18f);
    BodyAnimationClip::Sample brace;
    brace.scale = {1.0f+strain*0.20f, 1.0f-strain*0.28f, 1.0f};
    brace.scale.z = 1.0f/(brace.scale.x*brace.scale.y);
    brace.shear = localDirection * (strain*catchLean);
    const auto scale = capturePose_.scale*(1.0f-bodyBlend)+brace.scale*bodyBlend;
    player->SetWhipBodyPose(engine::Vector3{1,1,1}+(scale-engine::Vector3{1,1,1})*finishWeight,
        capturePose_.rotate*((1.0f-bodyBlend)*finishWeight),
        (capturePose_.shear*(1.0f-bodyBlend)+brace.shear*bodyBlend)*finishWeight);
    const auto origin = player->GetWhipOrigin();
    const float initialLength = engine::Math::Length(hitCurve_.back() - targetOffset_);
    // 現在の相手との距離を命中時の距離で割り、越えない範囲で命中した時のしなりを縮めます。
    const float ratio = std::clamp(engine::Math::Length(targetPosition - origin) /
        (std::max)(initialLength, 0.001f), 0.0f, 1.0f);
    const float blend = Easing::SmoothStep01(pullElapsed_ / settings_.bindBlendDuration);
    // 判定球の中心から実際の接触面へなじませ、細い描画先端と対象の隙間を閉じます。
    // 捕獲がなじんだ後は体内側へ寄せ、敵の回転で接触面だけが離れて見えるのを防ぎます。
    const float settle = Easing::SmoothStep01((pullElapsed_ - settings_.bindBlendDuration) / settings_.bindBlendDuration);
    const float surfaceBlend = Easing::SmoothStep01(pullElapsed_ / (std::min)(0.04f, settings_.contactDuration * 0.25f));
    const auto attachment = (targetOffset_ * (1.0f - surfaceBlend) + contactOffset_ * surfaceBlend) * (1.0f - settle);
    const auto end = targetPosition + attachment * ratio - origin;
    // 命中時の曲がり方を引き継ぎ、接続がなじんだら対象へ張る短い弧へ近づけます。
    const auto axis = SafeUnit(end, {0, 0, 1});
    const auto originalBend = hitCurve_[PlayerWhipMotion::kSegments / 2] - hitCurve_.back() * 0.5f;
    const auto perpendicular = originalBend - axis * engine::Math::Dot(originalBend, axis);
    const auto bendDirection = SafeUnit(perpendicular, SafeUnit(engine::Math::Cross(axis, {1, 0, 0}), {0, 1, 0}));
    const float bendLength = engine::Math::Length(end) * settings_.pullBendRatio;
    PlayerWhipMotion::Curve curve;
    for (size_t i = 0; i < curve.size(); ++i) {
        const float u = static_cast<float>(i) / PlayerWhipMotion::kSegments;
        const auto inherited = (hitCurve_[i] - hitCurve_.back() * u) * ratio;
        const auto tension = bendDirection * (std::sin(kPi * u) * bendLength);
        curve[i] = end * u + inherited * (1.0f - blend) + tension * blend;
    }
    recovering_ = player->GetSlimePullProgress() > 0.0f;
    Draw(player, curve, origin);
}

void PlayerWhipVisual::BeginReturn(Player* player) {
    // 伸びた曲線と身体姿勢を退避し、戻りを開始したときの形から連続して縮めます。
    recovering_ = true;
    returnElapsed_ = 0.0f;
    returnStartProgress_ = returnProgress_;
    returnStartLaunchWeight_ = launchWeight_;
    returnStartCurve_ = curve_;
    returnStartPose_ = player->GetWhipBodyPose();
    returnAmount_ = std::clamp(peakReach_ / settings_.maxDistance, 0.0f, 1.0f);
    contacting_ = false;
    contactWeight_ = 0.0f;
}
// 曲線と本体が戻る演出を進めます。終了または表示できない場合にtrueを返します。
bool PlayerWhipVisual::UpdateReturn(Player* player, float deltaTime) {
    // 変形データを持たないか本体モデルが変わった場合は、旧モデルの表示を終了する。
    if (!deformer_ || deformer_->GetSource() != player->GetModel()) return true;
    // 経過時間が有限であれば、正の分だけ帰還の時計を進める。
    if (std::isfinite(deltaTime)) returnElapsed_ += (std::max)(deltaTime, 0.0f);
    const float u = std::clamp(returnElapsed_ / settings_.returnSettleDuration, 0.0f, 1.0f);
    // 復帰の余韻まで終わった場合は、このフレームの変形を更新せず完了を返す。
    if (u >= 1.0f) return true;
    // 鞭の長さの戻りは前半で終え、身体の反動は後半まで徐々に薄くします。
    const float retract = Easing::SmoothStep01(u / 0.45f);
    launchWeight_ = returnStartLaunchWeight_ * (1.0f-retract);
    returnProgress_ = returnStartProgress_ + (1.0f - returnStartProgress_) * retract;
    returnFade_ = 1.0f - Easing::SmoothStep01((u - 0.25f) / 0.75f);
    for (size_t i = 0; i < curve_.size(); ++i) curve_[i] = returnStartCurve_[i] * (1.0f - retract);
    renderOrigin_ = player->GetWhipOrigin();
    // 受け止め後に一度だけ反対方向へ揺らし、足元を支点に静止させます。
    const float poseWeight = 1.0f - Easing::SmoothStep01(u);
    const float rebound = settings_.bodyRebound * returnAmount_ * std::sin(2.0f*kPi*u) * (1.0f-u);
    BodyAnimationClip::Sample pose = returnStartPose_;
    pose.scale = engine::Vector3{1,1,1} + (pose.scale-engine::Vector3{1,1,1})*poseWeight;
    pose.scale.x += rebound * 0.4f;
    pose.scale.y -= rebound * 0.65f;
    pose.scale.z = 1.0f / (std::max)(pose.scale.x * pose.scale.y, 0.1f);
    pose.rotate = pose.rotate * poseWeight;
    pose.shear = pose.shear * poseWeight;
    player->SetWhipReturnPose(pose);
    return false;
}

void PlayerWhipVisual::Finish(Player* player) {
    // プレイヤーを失った場合と次の帰還処理へ引き渡す予定の場合は、ここでは表示を変更しない。
    if (!player || player->IsWhipReturnPending(this)) return;
    // 帰還へ引き渡さない場合は、接触、発射直後、プレイヤーの本体姿勢を順に解除します。
    contacting_ = false;
    contactWeight_ = 0.0f;
    launchProgress_ = 1.0f;
    launchWeight_ = 0.0f;
    player->SetActiveWhipVisual({});
    player->ClearWhipBodyPose();
    // 描画モデルを取得できた場合だけ、表示枠の持ち主を確認する。
    if (auto* renderer = player->GetMeshRenderer()) {
        // 現在の表示枠がこの鞭の変形モデルを指す場合に限り、それを解除する。
        if (deformer_ && renderer->GetVisualModelOverride() == deformer_->GetModel())
            renderer->SetVisualModelOverride(nullptr);
    }
    // 共通マーカーが残っていれば、鞭の終了に合わせて隠す。
    if (auto* marker = player->GetHookMarker()) marker->SetIsVisible(false);
}
