#define NOMINMAX
#include "RidingPlatformController.h"
#include "game/system/scene/SceneGameplayAccess.h"
#include "GimmickMovingFloor.h"
#include "BaseEnemy.h"
#include "game/actor/gimmick/combat/GimmickMagmaGeyser.h"
#include "game/actor/gimmick/trigger/GimmickSwitch.h"
#include "BaseScene.h"
#include "Player.h"
#include "SceneManager.h"
#include "GhostRecorder.h"
#include "DebugConsole.h"
#include "GpuParticleManager.h"
#include "MovingFloorSupport.h"
#include <algorithm>
#include <cmath>

namespace {
float DistanceXZ(const engine::Vector3& a, const engine::Vector3& b) {
    // 乗り場との近さは高さを除いた水平面で判定する。
    const float x = a.x - b.x, z = a.z - b.z;
    return std::sqrt(x * x + z * z);
}

} // namespace

BaseEnemy* RidingPlatformController::FindEnemy(BaseScene& scene, const std::string& name) const {
    // 指定名に一致する最初の有効オブジェクトを返す。同名でもリプレイから除外されていれば探索を続ける。
    for (auto& object : scene.GetObjects()) {
        // 有効な最初の同名オブジェクトが敵ならそのポインターを返す。型が違えばnullptrになる。
        if (object && !object->IsReplayRemoved() && object->GetName() == name)
            return dynamic_cast<BaseEnemy*>(object.get());
    }

    return nullptr;
}

void RidingPlatformController::Reset(GimmickMovingFloor& floor, BaseScene& scene) {
    // 床・敵・当たり判定を演出開始前の配置へ戻し、再乗車できる状態にする。
    player_.Stop(false);
    floor.SetTranslate(start_);
    floor.UpdateLocalMatrix();
    floor.UpdateWorldMatrix();
    running_ = false;
    absentTime_ = 0.0f;
    // 再出発に備え、タイムラインから操作する水柱をリセットする。
    for (const auto& signal : sequence_.signals) {
        // 無効な信号と水柱の操作以外の信号は対象外。
        if (!signal.enabled || signal.signal != "ride.waterjet") continue;
        for (const auto& object : scene.GetObjects()) {
            // 信号の接続先と名前が一致し、種類がWaterGeyserのオブジェクトだけを調べる。
            if (object && object->GetName() == signal.binding.targetName && object->GetGimmickType() == "WaterGeyser") {
                // 水柱の制御型へ変換できた場合に、初期状態へ戻す。
                if (auto* jet = dynamic_cast<GimmickMagmaGeyser*>(object.get())) jet->ResetEscapeCycle(false);
            }
        }
    }

    // 乗り込み演出の途中状態を消し、動かせる敵だけを元の位置と衝突へ戻します。
    for (auto& boarder : boarders_) {
        boarder.active = false;
        boarder.released = false;
        boarder.elapsed = 0;

        // 敵が見つかり、生存していてプレイヤーに運ばれていない場合だけ、保存した姿勢と衝突を復元する。
        if (auto* enemy = FindEnemy(scene, boarder.name); enemy && !enemy->IsDead() && !enemy->IsCarried()) {
            enemy->SetTranslate(boarder.origin);
            enemy->SetCollisionAttribute(boarder.attribute);
            enemy->SetCollisionMask(boarder.mask);
            enemy->SetDormant(true);
        }
    }
}

void RidingPlatformController::Update(GimmickMovingFloor& floor, float deltaTime) {
    // Timeline の信号と床の軌跡を再生し、乗員と敵を同じフレームの移動量で運ぶ。
    auto* manager = SceneManager::GetInstance();
    auto* scene = manager ? manager->GetCurrentScene() : nullptr;
    // シーンを取得できなければ、信号の対象や乗員を解決できないため更新を終える。
    if (!scene)
        return;
    const std::string path = floor.GetPathMoverComponent()->GetRideSequence();

    // 同じ床の旧RecorderとTimelineを同時に動かさない。
    if (floor.GetRecorder())
        floor.GetRecorder()->Stop(false);

    // Editorで同じファイルへ保存した変更も、次のゲーム開始時に読み直す。
    // ファイルの変更時と、読み込み済みの床が編集からプレイに入る時に、シーケンスを読み直す。
    if (loadedPath_ != path || (manager->IsPlaying() && !wasPlaying_ && ready_)) {
        // 以前のシーケンスが利用可能なら、床と敵の位置を先に復元する。
        if (ready_)
            Reset(floor, *scene);
        loadedPath_ = path;
        ready_ = sequence_.Load(path);
        start_ = floor.GetTranslate();
        boarders_.clear();
        ferry_ = std::any_of(sequence_.signals.begin(), sequence_.signals.end(),
                             [](const auto& signal) { return signal.enabled && signal.signal == "ride.ferry"; });
        // 読み込みに失敗した場合はパスをログに残し、タイムラインを開始しない。
        if (!ready_) {
            DebugConsole::GetInstance()->AddLog("乗車床: タイムラインを読み込めません: " + path);
            return;
        }

        // 床そのものの絶対座標トラックがあれば、最初のキーを乗り場の基準に使います。
        for (const auto& track : sequence_.transformTracks) {
            // 有効でミュートされていない床の絶対座標トラックにキーがあれば、先頭の位置を使う。
            if (track.enabled && !track.muted && !track.relative && !track.anchorRelative &&
                track.binding.targetName == floor.GetName() && !track.keys.empty()) {
                start_ = track.keys.front().position;
                break;
            }
        }

        player_.Initialize(manager);
        player_.SetSequence(&sequence_);

        // 乗り込む敵ごとに初期位置と判定を退避し、信号の設定値から移動時間・高さ・着地点を取り込みます。
        for (const auto& signal : sequence_.signals) {
            // 有効なride.board信号だけを乗り込み予定として登録する。
            if (!signal.enabled || signal.signal != "ride.board")
                continue;
            auto* enemy = FindEnemy(*scene, signal.binding.targetName);
            // 接続先の敵が見つからないか死亡済みなら、乗り込み予定に登録しない。
            if (!enemy || enemy->IsDead())
                continue;
            Boarder boarder;
            boarder.name = enemy->GetName();
            boarder.origin = enemy->GetTranslate();
            boarder.attribute = enemy->GetCollisionAttribute();
            boarder.mask = enemy->GetCollisionMask();
            const auto payload = json::parse(signal.payload, nullptr, false);

            // 信号の付加データがJSONオブジェクトの場合だけ、既定の演出設定を上書きする。
            if (payload.is_object()) {
                // 数値のdurationは乗り込みにかける秒数として0.4〜4.0に収める。
                if (payload.contains("duration") && payload["duration"].is_number())
                    boarder.duration = std::clamp(payload["duration"].get<float>(), .4f, 4.0f);
                // 数値のheightはジャンプの高さとして0〜15の範囲に収める。
                if (payload.contains("height") && payload["height"].is_number())
                    boarder.height = std::clamp(payload["height"].get<float>(), 0.0f, 15.0f);
                // fallingが真偽値の場合にだけ、ジャンプと加速落下を切り替える。
                if (payload.contains("falling") && payload["falling"].is_boolean())
                    boarder.falling = payload["falling"].get<bool>();
                // 着地位置の補正は数値3要素のoffsetだけをXYZとして読み取る。
                if (payload.contains("offset") && payload["offset"].is_array() && payload["offset"].size() == 3 &&
                    payload["offset"][0].is_number() && payload["offset"][1].is_number() &&
                    payload["offset"][2].is_number())
                    boarder.offset = {payload["offset"][0], payload["offset"][1], payload["offset"][2]};
            }

            boarders_.push_back(boarder);
        }

        // 再生中の信号で水柱・飛沫・火山弾を起動し、乗り込み通知は対象の敵を進行可能にします。
        player_.SetSignalCallback([this, &floor](engine::Object3D* target, const CinematicSignalMarker& signal, bool preview) {
            // プレビュー中や接続先が解決できなかった信号では、ゲーム内の水柱・粒子・敵を動かさない。
            if (preview || !target)
                return;
            // ride.waterjet信号は水柱を発停する。
            if (signal.signal == "ride.waterjet") {
                // 接続先の種別が水柱であることを確認し、マグマの噴出口には適用しない。
                if (target->GetGimmickType() == "WaterGeyser") {
                    // 噴出口の制御型を取得できた場合は、payloadがoffのときだけ停止を通知する。
                    if (auto* jet = dynamic_cast<GimmickMagmaGeyser*>(target)) jet->OnSwitchEvent(signal.payload != "off");
                }
                return;
            }
            // ride.splash信号は床の現在位置で着水飛沫を出す。
            if (signal.signal == "ride.splash") {
                // GPU粒子の管理機能が初期化済みの場合だけ発生を依頼する。
                if (auto* particles = GpuParticleManager::GetInstance(); particles && particles->IsInitialized())
                    particles->Emit("shared/raft_water_landing", floor.GetTranslate());
                return;
            }
            // ride.fireball信号は接続先の噴出口から火山弾を出す。
            if (signal.signal == "ride.fireball") {
                // 噴出口の制御型を取得できた場合にだけ噴火を依頼する。
                if (auto* vent = dynamic_cast<GimmickMagmaGeyser*>(target)) {
                    vent->RequestFireballEruption();
                }
                return;
            }
            // 他の信号を処理したあとは、ride.boardだけを乗り込み予定へ伝える。
            if (signal.signal != "ride.board")
                return;
            for (auto& boarder : boarders_) {
                // 接続先の名前が一致し、まだ解放していない敵の移乗を開始する。
                if (boarder.name == target->GetName() && !boarder.released)
                    boarder.active = true;
            }
        });
    }

    // 読み込み済みシーケンスがない状態では、再生や乗員の移動を行わない。
    if (!ready_)
        return;
    const bool playing = manager->IsPlaying();

    // 編集時はタイムラインを進めず、通常の編集状態に戻す。
    if (!playing) {
        // プレイから編集に戻った最初の更新で床と敵をリセットする。
        if (wasPlaying_)
            Reset(floor, *scene);

        // 編集中はGhostDirectorのプレビューを上書きしない。
        for (auto& b : boarders_)
            // 対象の敵がシーンに残っている場合は、演出待ちによる休止を解除する。
            if (auto* enemy = FindEnemy(*scene, b.name))
                enemy->SetDormant(false);
        wasPlaying_ = false;
        return;
    }

    // プレイに入る最初の更新では、床・敵・信号の状態を乗り場の初期状態に揃える。
    if (!wasPlaying_)
        Reset(floor, *scene);
    wasPlaying_ = true;
    auto* rider = GetScenePlayer(scene);
    // プレイヤーがいないか時間が進んでいなければ、乗車判定と追従速度の計算を省く。
    if (!rider || deltaTime <= 0)
        return;
    // 出発直後の落水は水平距離が短いため、死亡復帰でも確実に乗り場へ戻す。
    if (rider->IsDead()) riderWasDead_ = true;
    // 直前まで死亡していたプレイヤーが復帰したら、床を乗り場へ戻してこの更新を終える。
    else if (riderWasDead_) {
        riderWasDead_ = false;
        Reset(floor, *scene);
        return;
    }
    const engine::Vector3 previous = floor.GetTranslate();
    const bool riderOnDeck = IsStandingOnMovingFloor(floor, *rider);
    // 再生中に降りたプレイヤーが乗り場近くへ戻り、床が十分離れているときに床を呼び戻す。船だけは接地・離脱時間・乗り場の高さも確認する。
    if (running_ && !riderOnDeck &&
        (!ferry_ || (rider->IsGrounded() && absentTime_ > 1.2f && rider->GetWorldPosition().y >= start_.y - 0.5f)) &&
        DistanceXZ(previous, start_) > (ferry_ ? 24.0f : 65.0f) &&
        DistanceXZ(rider->GetWorldPosition(), start_) < (ferry_ ? 36.0f : 22.0f) && !rider->IsDead()) {
        Reset(floor, *scene);
        return;
    }

    // 床に乗るまでは再生を開始しません。
    if (!running_ && riderOnDeck) {
        player_.Play(false);
        running_ = true;
    }

    // 全キャラの前フレームの接地を見てからTimelineを進める。
    std::vector<Character*> passengers;

    for (auto& object : scene->GetObjects()) {
        auto* actor = object && !object->IsReplayRemoved() ? dynamic_cast<Character*>(object.get()) : nullptr;
        // 落下中や接地していないキャラクターを外し、移動前の床に乗っている相手だけを集める。
        if (actor && IsStandingOnMovingFloor(floor, *actor))
            passengers.push_back(actor);
    }

    // 出発済みの床だけ、停車条件に合わせてこのフレームのタイムライン進行量を決める。
    if (running_) {
        float step = deltaTime;

        if (ferry_) {
            // 船はジャンプなどの短い離脱を許す。再乗車後は離脱の計測をリセットする。
            absentTime_ = riderOnDeck ? 0.0f : absentTime_ + deltaTime;
            // プレイヤーが死亡したか、連続で1.2秒を超えて降りているなら船を進めない。
            if (rider->IsDead() || absentTime_ > 1.2f)
                step = 0.0f;
        }

        const float now = player_.GetCurrentTime();

        for (const auto& signal : sequence_.signals) {
            // 有効な敵撃破待ちとスイッチ待ちだけを調べる。すでに通過した時刻の信号は無視する。
            if (!signal.enabled || (signal.signal != "ride.wait_clear" && signal.signal != "ride.wait_switch") ||
                signal.time + .001f < now)
                continue;

            // 更新後に巻き戻すと乗員・効果が先へ進むため、更新量を停止時刻で切る。
            const bool blocked =
                std::any_of(scene->GetObjects().begin(), scene->GetObjects().end(), [&](const auto& object) {
                    // 指定名の有効オブジェクトだけを待機条件に使う。
                    if (!object || object->GetName() != signal.binding.targetName || object->IsReplayRemoved())
                        return false;
                    // スイッチ待ちでは、型が一致し、まだイベントを出力していなければ停止対象にする。
                    if (signal.signal == "ride.wait_switch") {
                        const auto* button = dynamic_cast<GimmickSwitch*>(object.get());
                        return button && !button->IsOutputActive();
                    }
                    // 撃破待ちの対象が生存し、表示されている間は停止対象にする。
                    return !object->IsDead() && object->GetIsVisible();
                });
            // 停止対象が見つかった場合は、信号時刻を越えない長さまで進行量を縮める。
            if (blocked)
                step = std::min(step, std::max(0.0f, signal.time - now));
        }

        // 有効な進行時間が残った場合だけ、床の移動と到達した信号を処理する。
        if (step > 0.0f)
            player_.Update(step);
    }

    // 床の前後位置の差を、再生を進める前に集めた乗員に一度ずつ適用します。
    const auto displacement = floor.GetTranslate() - previous;

    for (auto* actor : passengers) {
        actor->SetTranslate(actor->GetTranslate() + displacement);
        actor->UpdateLocalMatrix();
        actor->UpdateWorldMatrix();
        // プレイヤーにだけ、このフレームの変位を秒で割った床の移動速度も渡す。
        if (auto* player = dynamic_cast<Player*>(actor))
            player->SetPlatformMotion(displacement * (1.0f / deltaTime));
    }

    for (auto& boarder : boarders_) {
        auto* enemy = FindEnemy(*scene, boarder.name);
        // 敵が見つからない場合、死亡済み、または既に乗り込みを終えた場合は補間しない。
        if (!enemy || enemy->IsDead() || boarder.released)
            continue;

        // プレイヤーが先に捕まえた敵を、乗り込み演出で奪い返さない。
        if (enemy->IsCarried()) {
            boarder.released = true;
            enemy->SetDormant(false);
            continue;
        }

        enemy->SetDormant(true);
        // 乗り込み信号をまだ受けていない敵は、休止状態のまま出発位置で待機させる。
        if (!boarder.active)
            continue;
        // 移動中の敵は衝突させず、落下指定では加速する軌道、通常は弧を描く軌道で床に向かわせます。
        enemy->SetCollisionAttribute(0);
        enemy->SetCollisionMask(0);
        boarder.elapsed += deltaTime;
        const float t = std::clamp(boarder.elapsed / boarder.duration, 0.0f, 1.0f);
        const auto c = floor.GetColliderConfig();
        engine::Vector3 destination = floor.GetTranslate() + boarder.offset;
        destination.y += (c.center.y + c.size.y) * floor.GetScale().y;
        // 着地点は現在の床の位置と判定の上端を基準に決め直す。
        auto position = engine::Math::Lerp(boarder.origin, destination, t);
        // 落下指定なら高さを進捗の二乗で補間し、高い島から落ちるにつれて速度を上げる。
        if (boarder.falling) position.y = boarder.origin.y + (destination.y - boarder.origin.y) * t * t;
        // 通常の移乗は直線補間に弧を重ね、途中で指定の高さに達する。
        else position.y += 4.0f * boarder.height * t * (1.0f - t);
        enemy->SetTranslate(position);

        // 着地後は乗り込みの制御を終了し、敵の当たり判定と通常更新を戻します。
        if (t >= 1) {
            boarder.released = true;
            enemy->SetDormant(false);
            enemy->SetVelocity({});
            enemy->SetGrounded(true);
            enemy->SetCollisionAttribute(boarder.attribute);
            enemy->SetCollisionMask(boarder.mask);
        }

        enemy->UpdateLocalMatrix();
        enemy->UpdateWorldMatrix();
    }
}
