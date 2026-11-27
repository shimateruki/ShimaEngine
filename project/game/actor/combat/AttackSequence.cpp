#include <cstdint>
#include "AttackSequence.h"
#include <numbers>
#include "game/system/combat/BulletManager.h"
#include "CollisionConfig.h"
#include "GpuParticleManager.h"
#include "MeshEffectManager.h"
#include "CollisionManager.h"
#include "game/system/rules/EventManager.h"
#include "MeshRenderer.h"
#include <numeric>
#include <cmath>

// 攻撃定義を検証して必須素材を準備する。時計を開始できた場合は、時刻0を処理して true を返す。
bool AttackSequence::Start(const EnemyAttackDefinition& definition, const AttackSequenceContext& context) {
    // 攻撃中は重複開始を防ぐ。無効または不正な定義も素材を作る前に拒否する。
    if (IsRunning() || !definition.sequence.enabled || !definition.sequence.Validate())
        return false;
    // 必須の本体アニメーションを先に全件検証し、途中まで発火した攻撃を残さない。
    std::vector<TrackState> prepared(definition.sequence.tracks.size());

    for (size_t i = 0; i < prepared.size(); ++i) {
        const auto& track = definition.sequence.tracks[i];
        // 有効な本体アニメーションだけ先読みし、クリップを開けない場合は診断を残して開始を止める。
        if (track.enabled && track.type == "body_animation" &&
            !prepared[i].bodyClip.Load(BodyAnimationClip::ResolveAssetPath(track.preset))) {
            label_ = "本体アニメーションを読み込めません: " + track.preset;
            return false; // 一部分だけを実行せず、開始前に失敗させます。
        }
    }

    // 時計が開始できない場合は、世代番号と本体姿勢の入れ替えを行わない。
    if (!clock_.Start(definition.sequence))
        return false;
    // 前回の演出と世代を切り離してから、新しい攻撃の定義・初期座標を確定する。
    ++generation_;
    ReleaseTrackEffects();
    definition_ = definition;
    initialContext_ = context;
    initialContext_.owner = nullptr; // 保存するのは座標のみ。寿命のあるActor参照を持ち越しません。
    trackStates_ = std::move(prepared);
    bodyPose_ = std::make_shared<engine::Matrix4x4>(engine::Math::MakeIdentity4x4());
    // 描画先の術者と MeshRenderer があれば、トラックが更新する変形行列を結びつける。
    if (context.owner && context.owner->GetMeshRenderer())
        context.owner->GetMeshRenderer()->SetTimelineVisualTransform(bodyPose_);
    Update(0.0f, context); // 時刻0の命令を入力されたフレームに実行します。
    return true;
}

// 経過秒に含まれるイベントとトラックを進め、最後に継続演出と確認用ラベルを更新する。
void AttackSequence::Update(float deltaTime, const AttackSequenceContext& context) {
    // 終了済みの攻撃や不正な経過時間では、イベントを進めない。
    if (!IsRunning() || !std::isfinite(deltaTime) || deltaTime < 0)
        return;
    // 通過区間の始点を記録してから時計を進め、発火条件の判定とイベント実行を渡す。
    const float previous = clock_.GetElapsed();
    clock_.Update(deltaTime, [&](const std::string& condition) { return CheckCondition(condition, context); },
        [&](const AttackSequenceEvent& event) { Execute(event, context); });

    // 条件停止の前に通過した判定区間は処理し、停止時刻より後は処理しません。
    if (!clock_.WasStoppedByCondition() || clock_.GetElapsed() > previous)
        UpdateTracks(previous, context);
    // 最後の判定を処理した後、継続中の姿勢やエフェクトを次の攻撃へ持ち越さないようにする。
    if (!IsRunning())
        ReleaseTrackEffects();
    // Editorのプレビューでも実行・省略・接触の内訳を同じ時計から読めるよう集計する。
    label_ = definition_.displayName + (IsRunning() ? " / 実行中" : " / 終了") +
        " / 発火 " + std::to_string(clock_.GetExecutedCount()) + " / 条件不成立 " + std::to_string(clock_.GetSkippedCount());
    size_t allowed = 0, skipped = 0, contacts = 0;

    for (const auto& state : trackStates_) {
        // 開始時刻に達したものだけを集計し、未到達のトラックを条件不成立数に含めない。
        if (state.entered) {
            // 開始条件を満たしたトラックは実行数に含める。
            if (state.allowed)
                ++allowed;
            // 開始時に条件を満たせなかった場合は省略数に含める。
            else ++skipped;
        }
        contacts += state.victims.size();
    }

    label_ += " / 部品実行 " + std::to_string(allowed) + " / 部品省略 " + std::to_string(skipped) +
        " / 接触 " + std::to_string(contacts);
}

// 発火時の接地・標的の水平距離を判定する。定義外の条件は成立扱いにしない。
bool AttackSequence::CheckCondition(const std::string& condition, const AttackSequenceContext& context) const {
    // 地上指定なら、現在の術者が接地しているときだけ成立する。
    if (condition == "grounded")
        return context.grounded;
    // 空中指定なら、現在の術者が非接地のときだけ成立する。
    if (condition == "airborne")
        return !context.grounded;
    if (condition == "target_in_range") {
        // 照準時に固定した発射速度用の距離と、発火時点の射程判定を分けます。
        engine::Vector3 difference = context.targetPosition - context.position;
        difference.y = 0;
        const float distance = engine::Math::Length(difference);
        return context.hasTarget && distance >= definition_.minRange && distance <= definition_.maxRange;
    }

    return condition == "always";
}

AttackSequence::~AttackSequence() {
    // シーケンスより長く残る演出スコープを終了させる。
    ReleaseTrackEffects();
}

void AttackSequence::ReleaseTrackEffects() {
    // トラックごとのエフェクトスコープを止め、次の攻撃へ残さない。
    bodyPose_.reset();

    for (auto& state : trackStates_) {
        // このトラックに割り当てたスコープがあれば、その演出の実行だけを止める。
        if (state.effectScope)
            MeshEffectManager::GetInstance()->StopEffectScope(state.effectScope);
        state.effectScope = 0;
    }
}

void AttackSequence::Cancel() {
    // 世代番号を進め、以前の非同期イベントが現在の攻撃へ混入しないようにする。
    ++generation_;
    ReleaseTrackEffects();
    clock_.Reset();
    trackStates_.clear();
    label_ = "中断";
}

// previous から時計の現在時刻までを秒で評価し、姿勢・追従・命中通知を更新する。
void AttackSequence::UpdateTracks(float previous, const AttackSequenceContext& context) {
    const float now = clock_.GetElapsed();
    std::vector<DamageEvent> damageEvents;
    // 前回の姿勢を引き継がないよう、描画用行列があれば既定姿勢から評価し直す。
    if (bodyPose_)
        *bodyPose_ = engine::Math::MakeIdentity4x4();
    std::vector<size_t> order(definition_.sequence.tracks.size());
    std::iota(order.begin(), order.end(), size_t{0});

    // 一更新で分岐元の終了と後続の開始を跨いでも、JSONの配列順に依存させません。
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return definition_.sequence.tracks[a].begin < definition_.sequence.tracks[b].begin;
    });

    for (size_t i : order) {
        const auto& track = definition_.sequence.tracks[i];
        auto& state = trackStates_[i];
        // 無効なトラックや、現在時刻が開始に達していないトラックは評価しない。
        if (!track.enabled || now < track.begin)
            continue;
        // 中断時刻から新しいトラックを開始しない。中断より前に始まった範囲は評価できる。
        if (clock_.WasStoppedByCondition() && now <= track.begin)
            continue;
        // 開始時刻を通過した最初の更新でだけ、トラック自身の条件と分岐元の結果を確認します。
        if (!state.entered) {
            state.entered = true;
            state.allowed = CheckCondition(track.condition, context);

            // 前のトラックを条件にする場合、許可された状態で終了時刻まで到達し、接触条件も一致する必要がある。
            if (track.condition == "contact" || track.condition == "no_contact" || track.condition == "track_completed") {
                const auto& source = trackStates_[track.conditionTrack];
                const bool completed = source.entered && source.allowed && now >= definition_.sequence.tracks[track.conditionTrack].end;
                state.allowed = completed && (track.condition == "track_completed" ||
                    (track.condition == "contact" ? !source.victims.empty() : source.victims.empty()));
            }
        }

        // そのトラックの開始判定で条件が成立しなかった場合は、後の更新でも起動しない。
        if (!state.allowed)
            continue;
        // 親の起動が許可されなかった場合は、親の姿勢に依存する子の評価も省く。
        if (track.parentTrack >= 0 && !trackStates_[track.parentTrack].allowed)
            continue;
        // 開始時の基準、追従中の術者、親トラックの順に使用する座標を決めます。
        auto origin = track.followOwner ? context : initialContext_;

        // 親トラックが指定されていれば、親の計算済みワールド位置と向きを基準にする。
        if (track.parentTrack >= 0) {
            origin.position = trackStates_[track.parentTrack].worldPosition;
            origin.forward = trackStates_[track.parentTrack].worldForward;
        }

        engine::Vector3 forward{origin.forward.x, 0, origin.forward.z};
        const float length = engine::Math::Length(forward);
        forward = length > 0.0001f ? forward * (1 / length) : engine::Vector3{0, 0, 1};
        const engine::Vector3 right{forward.z, 0, -forward.x};
        auto positionAt = [&](const AttackTransformKey& key) {
            // 弾道を初期化済みのトラックは、固定キー位置の代わりに今回の移動状態を使う。
            if (track.motion != "keyframed" && state.motionInitialized)
                return state.motion.position;
            return origin.position + right * key.position[0] + engine::Vector3{0, key.position[1], 0} + forward * key.position[2];
        };

        // 攻撃全体の時刻を各トラックの範囲に切り詰め、前回の位置と今回の位置を求めます。
        const float localTime = std::clamp(now - track.begin, 0.0f, track.end - track.begin);
        const auto key = track.Evaluate(localTime);
        state.frameStartPosition = positionAt(track.Evaluate(std::clamp(previous - track.begin, 0.0f, track.end - track.begin)));

        // キーに従わないトラックは開始位置を一度決め、弾道設定と現在の標的を渡して移動状態を進めます。
        if (track.motion != "keyframed") {
            // 初回だけ開始キーから位置を作り、基準の水平向きで弾道を初期化する。
            if (!state.motionInitialized) {
                state.motion.position = positionAt(track.Evaluate(0));
                state.motion.direction = forward;
                state.motionInitialized = true;
            }

            state.motion.Advance(track, localTime, context.targetPosition + engine::Vector3{0, track.targetHeight, 0}, context.hasTarget);
        }

        state.worldPosition = positionAt(key);
        state.worldForward = track.motion != "keyframed" ? state.motion.direction : forward;

        if (track.type == "body_animation") {
            // 本体アニメーションはクリップの時刻を求め、開始と終了のブレンドを掛けた描画姿勢にします。
            // 終了時刻以降や姿勢の反映先がない場合は、クリップ評価を省く。
            if (now >= track.end || !bodyPose_)
                continue;
            BodyAnimationClip::Sample sample;
            // 指定したクリップ秒数の姿勢を取得できない場合、そのトラックの変形を適用しない。
            if (!state.bodyClip.Evaluate(track.clipOffset + localTime * track.playbackSpeed, track.loop, sample))
                continue;
            const float weight = (std::min)(track.blendIn > 0 ? std::clamp(localTime / track.blendIn, 0.0f, 1.0f) : 1.0f,
                track.blendOut > 0 ? std::clamp((track.end - now) / track.blendOut, 0.0f, 1.0f) : 1.0f);
            engine::Matrix4x4 shear = engine::Math::MakeIdentity4x4();
            shear.m[1][0] = sample.shear.x * weight;
            shear.m[1][2] = sample.shear.z * weight;
            shear.m[3][0] = -sample.shearPivotY * shear.m[1][0];
            shear.m[3][2] = -sample.shearPivotY * shear.m[1][2];
            const engine::Vector3 scale{1 + (sample.scale.x - 1) * weight, 1 + (sample.scale.y - 1) * weight, 1 + (sample.scale.z - 1) * weight};
            *bodyPose_ = engine::Math::Multiply(shear, engine::Math::MakeAffineMatrix(scale, sample.rotate * weight, sample.translate * weight));
        } else if (track.type == "mesh_vfx") {
            // 継続するメッシュ演出は専用の発生枠を持ち、トラックが終わったらその枠だけ停止します。
            auto* effects = MeshEffectManager::GetInstance();

            // トラック終端に達したら、表示の評価を行わずスコープを片付ける。
            if (now >= track.end) {
                // 割り当て済みのスコープだけを停止する。
                if (state.effectScope)
                    effects->StopEffectScope(state.effectScope);
                state.effectScope = 0;
                continue;
            }

            // 有効区間に入りスコープが無ければ新規発行し、プリセットの開始を試す。
            if (!state.effectScope) {
                state.effectScope = effects->CreateEffectScope();

                // プリセットが開始できなければ、その後のフレームからもこのトラックを除外する。
                if (!effects->StartTimelineEffect(state.effectScope, track.preset)) {
                    state.allowed = false; // 存在しないプリセットを毎フレーム読み直さない。
                    continue;
                }
            }

            effects->EvaluateTimelineEffect(state.effectScope, localTime, track.end - track.begin, positionAt(key),
                {key.rotation[0] - (track.motion != "keyframed" ? std::atan2(state.worldForward.y,
                    std::sqrt(state.worldForward.x * state.worldForward.x + state.worldForward.z * state.worldForward.z)) : 0),
                 key.rotation[1] + std::atan2(state.worldForward.x, state.worldForward.z), key.rotation[2]},
                {key.scale[0], key.scale[1], key.scale[2]});
        } else if (track.type == "hitbox" && previous < track.end && now >= track.begin) {
            // 攻撃判定は有効区間を通った更新だけ出し、術者の所属から攻撃相手を選びます。
            engine::PhysicsQueryFilter filter;
            filter.mask = engine::collisionCategories::Get(context.playerOwned ? "enemy" : "player");
            filter.ignoredObject = context.owner;
            const float previousLocal = std::clamp(previous - track.begin, 0.0f, track.end - track.begin);
            auto oldKey = track.Evaluate(previousLocal);
            engine::Vector3 oldPosition = state.hasPreviousPosition ? state.previousPosition : positionAt(oldKey);

            // 親が動いたフレームで判定が始まった場合にも、開始時刻に対応する親の位置を復元します。
            if (!state.hasPreviousPosition && track.parentTrack >= 0) {
                const auto& parent = definition_.sequence.tracks[track.parentTrack];
                const auto& parentState = trackStates_[track.parentTrack];
                const float frameBegin = (std::max)(previous, parent.begin);
                const float frameEnd = (std::min)(now, parent.end);
                const float fraction = frameEnd > frameBegin
                    ? std::clamp(((std::max)(previous, track.begin) - frameBegin) / (frameEnd - frameBegin), 0.0f, 1.0f) : 1;
                oldPosition = engine::Math::Lerp(parentState.frameStartPosition, parentState.worldPosition, fraction)
                    + positionAt(oldKey) - parentState.worldPosition;
            }

            std::vector<float> sampleTimes;

            // 一度の更新で往復するキーを通過しても、最初と最後だけを結んで軌道を失わない。
            for (const auto& knot : track.keys)
                // 今回の区間の内側にあるキーだけを中継点にし、末尾は別途追加する。
                if (knot.time > previousLocal && knot.time < localTime)
                    sampleTimes.push_back(knot.time);
            sampleTimes.push_back(localTime);

            for (float sampleTime : sampleTimes) {
                const auto sample = track.Evaluate(sampleTime);
                const engine::Vector3 position = positionAt(sample);

                // 保持キーの切り替えは瞬間移動なので、中間空間へ攻撃判定を作らない。
                const auto hits = oldKey.interpolation == 0
                    ? engine::CollisionManager::GetInstance()->OverlapSphere(position, track.radius, filter)
                    : engine::CollisionManager::GetInstance()->OverlapCapsule(oldPosition, position, track.radius, filter);
                for (const auto& hit : hits) {
                    // 対象 Object の参照がない接触はダメージ候補にできないため飛ばす。
                    if (!hit.object)
                        continue;

                    // 隔離プレビューの攻撃から、本編のActorへダメージを配信しない。
                    if (context.owner && context.owner->IsEditorInternal() != hit.object->IsEditorInternal())
                        continue;
                    // 同じトラックの別の標本で接触済みなら、同じ Replay ID へ重複通知を送らない。
                    if (!state.victims.insert(hit.object->GetReplayId()).second)
                        continue;
                    DamageEvent damage;
                    damage.attacker = context.owner;
                    damage.target = hit.object;
                    damage.damageAmount = definition_.damage * track.damageScale;
                    damage.knockbackVelocity = definition_.ResolveKnockback(forward, {});
                    damage.invincibilityDuration = definition_.ResolveInvincibilityDuration();
                    damageEvents.push_back(damage);
                }

                oldKey = sample;
                oldPosition = position;
            }

            // 次の掃引でこの位置を出発点にできるよう、今回確定した末尾の位置を残します。
            state.previousPosition = positionAt(key);
            state.hasPreviousPosition = true;
        }
    }

    // コールバックで攻撃が中断されても、走査中のトラックを無効化しないよう最後に通知します。
    const auto generation = generation_;

    for (const auto& damage : damageEvents) {
        // 通知の途中で中断・再開が起きた場合、古い世代の残りの通知を破棄する。
        if (generation != generation_)
            break;
        const auto& objects = engine::CollisionManager::GetInstance()->GetObjects();
        // 先行する通知で消えた対象を除き、まだ衝突管理に登録されている相手へだけ配信する。
        if (std::find(objects.begin(), objects.end(), damage.target) != objects.end())
            EventManager::GetInstance()->Dispatch(damage);
    }
}

void AttackSequence::Execute(const AttackSequenceEvent& event, const AttackSequenceContext& context) {
    // 定義された操作種別を共有の発生位置・速度へ解決してから実演出を起動する。
    const auto part = ResolveProjectileComponent(definition_, event);
    const auto emission = ResolveAttackSequenceEmission(event, context, part.minSpeed, part.maxSpeed);
    const engine::Vector3 position = emission.position;

    if (event.operation == "gpu_vfx") {
        // 見た目だけのイベントは弾と当たり判定を作らず、その場で発生を終える。
        auto* particles = GpuParticleManager::GetInstance();

        // 粒子管理が初期化され、プリセット名が指定されている場合にだけ生成要求を出す。
        if (particles->IsInitialized() && !event.preset.empty()) {
            // 向き指定がある粒子には発生方向と速度倍率を渡す。
            if (event.directed)
                particles->EmitDirected(event.preset, position, emission.direction, event.particleSpeedScale);
            // 向き指定がなければプリセットの発生方向を使う。
            else particles->Emit(event.preset, position);
        }

        return;
    }

    const engine::Vector3 direction = emission.direction;
    const float speed = emission.speed;
    BulletVisualConfig visual;
    visual.collisionRadiusIsWorld = true;
    visual.debugLabel = event.name;
    visual.visualScale = part.visualScale;
    visual.color = {part.appearance.red, part.appearance.green, part.appearance.blue, part.appearance.alpha};
    visual.emissive = part.appearance.emissive;

    if (part.style == "fire") {
        // 火球は指定された地形との接触で着弾演出を出して終了する。
        visual.destroyOnSolidCastHit = true;
        visual.materialType = 11;
        const auto& appearance = part.appearance;
        visual.color = {appearance.red, appearance.green, appearance.blue, appearance.alpha};
        visual.emissive = appearance.emissive;
        visual.effectType = 1;
        visual.effectScale = appearance.effectScale;
        visual.effectSoftness = appearance.softness;
        visual.effectIntensity = appearance.intensity;
        visual.billboardScale = appearance.billboardScale;
        visual.trailInterval = appearance.trailInterval;
        visual.trailSpeedScale = appearance.trailSpeedScale;
        visual.trailReverseDirection = true;
        visual.impactDirected = false;
    }

    // 発射・着弾の表示と状態異常は、同じ攻撃設定から弾の生成先へ渡します。
    visual.trailPreset = part.trail;
    visual.trailSecondaryPreset = part.secondaryTrail;
    visual.impactPreset = part.impact;
    visual.impactSecondaryPreset = part.secondaryImpact;
    visual.trailInterval = part.appearance.trailInterval;
    visual.trailSpeedScale = part.appearance.trailSpeedScale;
    StatusEffectApplication status;
    // 定義の状態異常が burning の場合だけ、炎上として弾へ渡す。
    if (definition_.statusEffectType == "burning")
        status.type = StatusEffectType::kBurning;
    status.duration = definition_.statusDuration;
    status.tickInterval = definition_.statusTickInterval;
    status.tickDamage = definition_.statusTickDamage;
    status.vfxPreset = definition_.statusVfx;
    const auto attribute = engine::collisionCategories::Get(context.playerOwned ? "player_attack" : "enemy_attack");
    // 同じ攻撃定義を敵とプレイヤーのコピー能力で共用できるよう、所有者で衝突先を切り替える。
    const auto mask = engine::collisionCategories::Get(context.playerOwned ? "enemy" : "player") | engine::collisionCategories::Get("ground");

    // 複数弾は指定角度の中に等間隔で展開し、一発なら照準の中心へ飛ばします。
    for (std::int32_t i = 0; i < event.count; ++i) {
        const float angle = event.count == 1 ? 0.0f : (static_cast<float>(i) / (event.count - 1) - 0.5f) * event.spreadDegrees * (std::numbers::pi_v<float> / 180.0f);
        const engine::Vector3 spread{direction.x * std::cos(angle) + direction.z * std::sin(angle), direction.y,
                            direction.z * std::cos(angle) - direction.x * std::sin(angle)};
        BulletManager::GetInstance()->Fire(position, spread * speed + engine::Vector3{0, event.verticalSpeed, 0},
            part.collisionEnabled ? attribute : 0, part.collisionEnabled ? mask : 0,
            part.model, part.radius, part.lifetime, visual, part.damage, status,
            part.style == "fire" ? DamageType::kFire : DamageType::kPhysical);
    }
}
