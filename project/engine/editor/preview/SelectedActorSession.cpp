#include <cstdint>
#include "SelectedActorSession.h"
#ifdef USE_IMGUI
#include "SceneManager.h"
#include "BaseScene.h"
#include "Object3D.h"
#include <algorithm>
#include <cmath>
namespace { constexpr float kStep = 1.0f / 60.0f; constexpr uint32_t kActorHidden = 1u << 12; }
// シーン管理を登録し、必要ならゲーム側のプレビュー準備も接続します。
void SelectedActorSession::Initialize(SceneManager* manager) {
    manager_ = manager; sCurrent = this;
    // 初期化サービスが登録済みの場合だけ、ゲーム側へセッションを渡す。
    if (auto& call = ServicesTable().initialize) call(manager, *this);
}
// プレビューを停止してゲーム側のサービスを解放し、選択先を消します。
void SelectedActorSession::Finalize() {
    Stop();
    // 終了サービスが登録済みなら、プレビュー停止後に呼ぶ。
    if (auto& call = ServicesTable().finalize) call();
    manager_ = nullptr; targetId_ = 0;
    // 現在公開されているセッションがこの実体なら、公開ポインターを消す。
    if (sCurrent == this) sCurrent = nullptr;
}
// 選択IDを現在のシーンから探し直す。対象が生存して解決できた場合にだけ非所有ポインターを返す。
engine::Object3D* SelectedActorSession::GetTarget() const {
    // Sceneが変わったら旧Objectのポインタには触れない。IDを現Sceneから毎回解決する。
    if (!targetId_ || !manager_ || manager_->GetSceneGeneration() != sceneGeneration_) return nullptr;
    auto* scene = manager_->GetCurrentScene();
    // 現在のシーンを取得できなければIDを照合できない。
    if (!scene) return nullptr;
    for (const auto& object : scene->GetObjects())
        // 対象IDが一致し、リプレイで削除されていない実体だけを返す。
        if (object && object->GetReplayId() == targetId_ && !object->IsReplayRemoved()) return object.get();
    return nullptr;
}
// 選択先の識別情報が変わる場合に、旧プレビューを閉じて選び直します。
void SelectedActorSession::SetTarget(engine::Object3D* target) {
    // エディター用の内部物体は実行対象にせず、選択なしとして扱う。
    if (target && target->IsEditorInternal()) target = nullptr;
    const uint64_t id = target ? target->EnsureReplayId() : 0;
    const uint64_t generation = manager_ ? manager_->GetSceneGeneration() : 0;
    const std::string kind = target && ServicesTable().identity ? ServicesTable().identity(*target) : std::string{};
    // ID、シーン世代、対象種別がすべて同じならプレビューを維持する。
    if (id == targetId_ && generation == sceneGeneration_ && kind == targetKind_) return;
    // 元の対象がまだ解決できる間に停止し、一時非表示を解除してから識別情報を新しい対象へ差し替える。
    Stop(); targetId_ = id; sceneGeneration_ = generation; targetKind_ = kind;
    // 新しい実体と選択サービスが揃う場合だけ、ゲーム側へ選択を通知する。
    if (target && ServicesTable().select) ServicesTable().select(*target);
}
// 選択中の実体でプレビューを作り直します。作成できた場合にtrueを返します。
bool SelectedActorSession::Begin() {
    // 準備中、シーン管理なし、プレイ中、遷移中、または対象が解決できない場合は開始しない。
    if (changing_ || !manager_ || manager_->IsPlaying() || manager_->IsTransitioning() || !GetTarget()) return false;
    Stop(); changing_ = true;
    // ゲーム側の準備中は停止などの再入を防ぐ。確認用の実体を作れた場合に限り元の対象を一時非表示にする。
    active_ = ServicesTable().begin && ServicesTable().begin();
    changing_ = false; accumulator_ = 0;
    // 対象がまだ有効なら、プレビューを作成できた場合だけ元の実体を隠す。
    if (auto* target = GetTarget()) target->SetEditorHiddenReason(kActorHidden, active_);
    return active_;
}
// 単体確認を止めて派生側の片付けを依頼し、元の対象に付けた一時非表示を解除する。
void SelectedActorSession::Stop() {
    // 開始・終了サービスの呼び出し中は、停止への再入を避ける。
    if (changing_) return;
    // 終了コールバックの前に再生状態を止め、未処理の時間も捨てる。片付けが終わってから元の表示を戻す。
    const bool wasActive = active_; active_ = false; accumulator_ = 0;
    // プレビューが実際に起動しており、終了サービスがあれば片付けを依頼する。
    if (wasActive && ServicesTable().end) {
        changing_ = true; ServicesTable().end(); changing_ = false;
    }
    // 対象をまだ解決できる場合は、このセッションが付けた非表示理由だけを解除する。
    if (auto* target = GetTarget()) target->SetEditorHiddenReason(kActorHidden, false);
}
// 現在の選択先で最初からプレビューを準備し直します。
void SelectedActorSession::Restart() { Begin(); }
// 停止中なら先頭から準備し、連続再生を止めて 1/60 秒だけ進める。既に残っている端数時間は使わない。
void SelectedActorSession::StepFrame() {
    // 停止中は起動を試し、失敗した場合は固定フレームを進めない。
    if (!active_ && !Begin()) return;
    // 連続再生の操作が実装されていれば停止する。
    if (ServicesTable().setPlaying) ServicesTable().setPlaying(false);
    // 固定更新のサービスがあれば1/60秒分だけ進める。
    if (ServicesTable().step) ServicesTable().step(kStep);
    accumulator_ = 0;
}
// 先頭から固定フレームを積み上げ、指定秒数に近い状態へ移動します。
void SelectedActorSession::Seek(float seconds) {
    // 非有限の時刻、または新しいプレビューを作れない場合はシークしない。
    if (!std::isfinite(seconds) || !Begin()) return;
    // 連続再生を止められる場合は、シークで進める前に停止する。
    if (ServicesTable().setPlaying) ServicesTable().setPlaying(false);
    // 再構築する時刻を0〜60秒に制限し、最も近い固定フレーム数へ丸める。
    const std::int32_t frames = static_cast<std::int32_t>(std::round(std::clamp(seconds, 0.0f, 60.0f) / kStep));
    for (std::int32_t i=0; i<frames && active_; ++i)
        // 固定更新が登録されていれば、このステップを一回だけ再生する。
        if (ServicesTable().step) ServicesTable().step(kStep);
}
// 編集時の保守処理へ実秒数を渡し、連続再生中は蓄積時間を固定刻みで使う。
void SelectedActorSession::Update(float realDeltaTime) {
    // 編集状態を離れたか選択対象が消えた場合は、残っているプレビューを停止する。
    if (!manager_ || manager_->IsPlaying() || manager_->IsTransitioning() || (targetId_ && !GetTarget())) {
        Stop(); return;
    }
    // 保守サービスがあれば、連続再生の有無にかかわらず実時間を渡す。
    if (ServicesTable().maintain) ServicesTable().maintain(realDeltaTime);
    // プレビューが無効か再生情報を取得できなければ、固定更新を進めない。
    if (!active_ || !ServicesTable().playback) return;
    auto state = ServicesTable().playback();
    // 再生停止中は時間の余りを消し、再開時にまとめて進まないようにする。
    if (!state.playing) { accumulator_ = 0; return; }
    // 通常フレームの経過秒を倍率で調整して蓄積する。停止や長い描画待ちで大量の固定更新が走らないよう入力時間を制限する。
    accumulator_ += std::clamp(realDeltaTime, 0.0f, 0.25f) * std::clamp(state.speed, 0.05f, 2.0f);
    // 1/60 秒に達した分を一段ずつ消費する。1 フレームで複数段進んでいても、各段の後で演出終了を確認する。
    while (accumulator_ >= kStep && active_) {
        accumulator_ -= kStep;
        // 固定更新サービスが登録済みなら、蓄積時間の1/60秒分を進める。
        if (ServicesTable().step) ServicesTable().step(kStep);
        state = ServicesTable().playback();
        // 再生終端に着いたらループ時だけ初期状態を作り直す。残りの蓄積時間は次周へ持ち越さない。
        if (state.time >= state.duration && state.duration > 0) {
            // ループ指定では終端から先頭へ作り直す。
            if (state.loop) Restart();
            // ループしない場合は、操作サービスを通して連続再生を止める。
            else if (ServicesTable().setPlaying) ServicesTable().setPlaying(false);
            accumulator_ = 0; break;
        }
    }
}
// Inspectorから受け取った選択先を反映し、対応するゲーム側の編集欄を描く。
void SelectedActorSession::DrawInspector(engine::Object3D* target, ProjectWindow* assets) {
    SetTarget(target);
    // 選択先が有効で表示サービスがある場合だけ、対象の操作欄を描く。
    if (GetTarget() && ServicesTable().draw) ServicesTable().draw(assets);
}
// 現在の選択先に対応する補助描画を、画面上の指定矩形へ出す。
void SelectedActorSession::DrawOverlay(float x,float y,float width,float height) {
    // 選択先と画面表示サービスがある場合に、指定領域へオーバーレイを描く。
    if (GetTarget() && ServicesTable().overlay) ServicesTable().overlay(x,y,width,height);
}
#endif
