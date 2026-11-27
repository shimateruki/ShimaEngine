#include <cstdint>
#include "engine/graphics/effect/VfxAssetPath.h"
#include "CopyAbilitySupport.h"
#include "CrownLanceSettings.h"
#include "CrownLanceTrail.h"
#include "CrownGateWarp.h"
#include "CrownMeleeCombo.h"
#include "CameraManager.h"
#include <array>

namespace {
using namespace copyAbilityDetail;

// 王冠能力の溜めゲート槍・近接コンボ・ゲート回避と、その表示物の寿命を管理する。
// PlayerCopyAbilityControllerが所有する王冠の槍・ゲート回避の実装。入力はProcessInput、進行はUpdate、一時表示の回収はCancelが担当する。
class CrownCopyAbility final : public ICopyAbilitySession {
  public:
    using ICopyAbilitySession::ICopyAbilitySession;

    void ProcessInput(Player& player, const PlayerCopyAbilityInput& input) override {
        // 演出中は解除し、近接コンボ・ゲート回避・ゲート槍の入力を分けます。
        if (player.IsCinematicLocked() || player.IsDead()) {
            Cancel(player);
            return;
        }
        // 槍を発射予約してから最初の反動が終わるまでは回避・近接で割り込ませない。
        if (warp_.Active() || recovery_.Active() || (!charging_ && !releasePosePlayed_ && !gates_.empty()))
            return;
        // 近接が進行中の間は、回避への切替を優先してから次段の予約だけを受け付ける。
        if (melee_.Active()) {
            // 右入力で、近接が回避可能な段階かつ回避の再使用が可能な場合だけ、経路の確認を試す。
            if (input.secondaryTriggered && melee_.CanDodge() && warp_.Ready()) {
                // 回避の経路確認に成功した時だけ近接を終了します。壁際で失敗したら近接を続けます。
                player.SetIsControlActive(true);
                // 経路とゲートを用意できた場合だけ近接を終え、そのフレームの入力を消費する。
                if (BeginWarp(player)) {
                    melee_.End(player, false);
                    player.PlaySlimeAbilityMotion("player_ability_crown_warp_enter", false, 1.0f, 0.0f, 0.0f);
                    return;
                }
                // 回避へ切り替えられなければ、進行中の近接へ操作停止を戻す。
                player.SetIsControlActive(false);
            }
            // 近接中のE入力は次段の予約へ渡し、新規の攻撃を始めない。
            if (input.specialTriggered) melee_.QueueNext();
            return;
        }
        // 近接中でなければ、回避、近接開始、槍の溜めの順に入力を処理する。
        // 右入力で回避を開始できた場合は、近接や槍の入力を処理しない。
        if (input.secondaryTriggered && BeginWarp(player))
            return;
        // E入力が新たにあり近接を開始できるなら、溜め中の槍を中止して近接に移る。
        if (input.specialTriggered && melee_.Ready()) {
            StopLanceCharge(player);
            melee_.Begin(player, [this](const char* model) { return MakeVisual(model); },
                          [this] { return MakeGatePortal(); });
            return;
        }

        // 押した時に形成を始め、離した時だけ発射予約を作ります。
        // 左入力だけが新たにあり、再使用可能で溜めも未発射ゲートもなければ溜めを始める。
        if (!input.specialTriggered && !input.secondaryTriggered && input.primaryTriggered && lanceCooldown_ <= 0.0f &&
            !charging_ && gates_.empty())
            BeginLanceCharge(player);
        // 溜めの間に左を押していない状態になったら、開いたゲートへ発射を予約する。
        if (charging_ && !input.primaryHeld)
            ReleaseLances(player);
    }

    void Update(Player& player, float deltaTime) override {
        // 近接は槍の弧、飛び道具は移動線分を検査してフレーム間のすり抜けを防ぎます。
        // 非有限または0以下の経過秒は、演出更新や攻撃の移動に使わない。
        if (!std::isfinite(deltaTime) || deltaTime <= 0.0f)
            return;
        // 演出による操作固定か死亡の場合は、近接・回避・槍を解除する。
        if (player.IsCinematicLocked() || player.IsDead()) {
            Cancel(player);
            return;
        }
        // 共通の経過時間と残光を更新してから、回避・近接・槍の形成を進める。
        time_ += deltaTime;
        recovery_.Update(deltaTime);
        effects_.Update(deltaTime);
        readyFlashTime_ = (std::max)(0.0f, readyFlashTime_ - deltaTime);
        lanceCooldown_ = std::max(0.0f, lanceCooldown_ - deltaTime);
        warp_.Update(player, deltaTime);
        melee_.Update(player, deltaTime);
        UpdateLanceCharge(player, deltaTime);
        const auto& tuning = CrownFinaleTuning::Get();

        // 命中済みの槍は見た目だけを消散させ、予約から生まれた弾をこの後の命中判定へ加える。
        std::erase_if(remnants_, [deltaTime](Shot& shot) { return UpdateShotDissolve(shot, deltaTime); });
        UpdateGates(player, deltaTime);
        for (auto it = shots_.begin(); it != shots_.end();) {
            // 低fpsでも同じフレーム内の発射順が距離へ反映されるよう、発射後の残り時間だけ進めます。
            const float step = it->firstStep >= 0.0f ? (std::min)(it->firstStep, deltaTime) : deltaTime;
            it->firstStep = -1.0f;
            it->life -= step;
            const engine::Vector3 movement = it->velocity * step;

            // 高速弾は線分で調べ、フレーム間で敵や地形を飛び越えません。
            engine::PhysicsQueryFilter filter;
            filter.mask = engine::collisionCategories::Get("enemy") | engine::collisionCategories::Get("ground");
            filter.ignoredObject = &player;
            const float length = engine::Math::Length(movement);
            const auto hit = engine::CollisionManager::GetInstance()->SphereCast(
                it->position, 0.16f * it->scale,
                length > 0.0001f ? movement / length : engine::Vector3{0.0f, -1.0f, 0.0f}, length, filter);
            // 移動先までの球の掃引で敵・地形に触れたら、飛行を止める。
            if (hit.isHit) {
                // 当たった実体から敵本体を解決できた場合だけ、命中のダメージを送る。
                if (engine::Object3D* enemy = FindEnemyRoot(hit.hitObject)) {
                    const auto generation = cancelGeneration_;
                    DamageEnemy(player, enemy, tuning.lanceDamage * it->damageScale, DamageType::kPhysical, {});
                    // 命中イベント内でコピー解除されたら、消された弾の参照を使いません。
                    if (generation != cancelGeneration_) return;
                }

                const auto direction = Unit(it->velocity);
                const auto normal = Unit(hit.normal, direction * -1.0f);
                // 接触時の球の中心で槍を固定し、残り時間だけ消散の時計を進めます。
                const float travel = std::clamp(hit.distance, 0.0f, length);
                it->position += direction * travel;
                it->visual->SetTranslate(it->position);
                EmitLanceImpact(hit.hitPoint, normal, it->isFullChargeCenter, it->presentation);
                BeginShotDissolve(*it);
                const float remaining = step - travel / (std::max)(0.001f, engine::Math::Length(it->velocity));
                // 残った経過秒を消化しても表示が消え切らない場合にだけ、残光リストへ移す。
                if (!UpdateShotDissolve(*it, (std::max)(0.0f, remaining)))
                    remnants_.push_back(std::move(*it));
                it = shots_.erase(it);
                continue;
            }

            // 命中しなかった弾は移動を確定し、柄までゲートを通過したら断面表示を外す。
            it->position += movement;
            it->visual->SetTranslate(it->position);
            it->visual->Update(step);
            const auto direction = Unit(it->velocity);
            // ゲートから出る途中の槍は、移動方向への距離が柄尻の寸法に達した段階で断面を外す。
            if (it->emerging && engine::Math::Dot(it->position - it->gateOrigin, direction) >= RearOffset(it->scale)) {
                it->visual->ClearPortalClip();
                it->emerging = false;
            }
            UpdateTrail(*it, 0.0f, step);
            // 寿命切れは命中通知を出さず、消えていく表示だけを残す。
            if (it->life <= 0.0f) {
                BeginShotDissolve(*it);
                UpdateShotDissolve(*it, 0.0f);
                remnants_.push_back(std::move(*it));
                it = shots_.erase(it);
            }
            // 寿命が残っている弾は次回の更新へ残す。
            else
                ++it;
        }

        ambientTimer_ -= deltaTime;

        // 待機用の粒子は、能力全体の発生間隔を満たしたときだけ本体の上へ出す。
        if (ambientTimer_ <= 0.0f) {
            EmitPreset("player/copy/crown/crown_goal_idle_sparkle", player.GetWorldPosition() + engine::Vector3{0.0f, 1.8f, 0.0f});
            ambientTimer_ = 0.65f;
        }
    }

    void Draw(Player& player, ID3D12Resource* point, ID3D12Resource* spot, engine::Camera* camera,
              std::int32_t previewBufferIndex) override {
        melee_.Draw(point, spot, camera, previewBufferIndex);
        effects_.Draw(point, spot, camera, previewBufferIndex);
        warp_.DrawEffects(point, spot, camera, previewBufferIndex);

        // 未発射の槍、飛行中の槍、消散中の槍と軌跡を通常パスで描く。ゲートは専用描画側が担当する。
        for (const auto& gate : gates_) {
            // ゲートに保持している未発射の槍だけ描く。発射済みは飛行側が担当する。
            if (!gate.fired)
                DrawVisual(gate.lance.get(), point, spot, camera, previewBufferIndex);
        }
        for (const auto& shot : shots_)
            DrawVisual(shot.visual.get(), point, spot, camera, previewBufferIndex);
        for (const auto& shot : remnants_)
            DrawVisual(shot.visual.get(), point, spot, camera, previewBufferIndex);
        for (auto& shot : shots_)
            shot.trail.Draw(point, spot, camera, previewBufferIndex);
        for (auto& shot : remnants_)
            shot.trail.Draw(point, spot, camera, previewBufferIndex);
    }

    // 回避・近接で描画が必要な状態か、溜めや発射のゲートが残るかを返す。
    bool HasOwnedVisuals() const override { return melee_.Active() || warp_.Active() || !gates_.empty(); }
    void DrawOwnedVisuals(Player& player, uint32_t depth, uint32_t grab, engine::Camera* camera, std::int32_t bufferIndex) override {
        // ゲートは深度・画面色を参照する専用パスへ渡し、通常の槍と二重描画しません。
        // 死亡中か演出で操作を固定されている場合は、特殊材質のゲートを描かない。
        if (player.IsDead() || player.IsCinematicLocked())
            return;
        // 明示された描画先がなければ、現在の有効なカメラを取得する。
        if (!camera)
            camera = CameraManager::GetInstance()->GetActiveCamera();
        // 現在の視点も得られなければ、深度と背景色を使うゲートの描画を省く。
        if (!camera)
            return;
        warp_.Draw(camera, depth, grab, bufferIndex);
        melee_.DrawGate(camera, depth, grab, bufferIndex);
        for (auto& gate : gates_) {
            gate.portal->UpdateWorldMatrix(true);
            gate.portal->DrawSpecialMaterialForCamera(camera, depth, grab, bufferIndex);
        }
    }
    // 回避、近接、槍の順に、UIで区別できる現在段階の名前を返す。
    const char* GetStateLabel() const override {
        // ゲート回避の途中なら、ほかの能力表示より先に回避を表示する。
        if (warp_.Active()) return "覇王・ゲート回避";
        // 近接中は横薙ぎ、斬り上げ、叩きつけを段数で切り替える。
        if (melee_.Active()) {
            static constexpr const char* kLabels[]{"覇王・横薙ぎ", "覇王・斬り上げ", "覇王・叩きつけ"};
            return kLabels[melee_.Stage()];
        }
        // 溜め中は設定されたしきい値に応じて、ゲート数と最大段階を表示する。
        if (charging_)
            return chargeTime_ >= lanceSettings_.fullChargeTime  ? "覇王・最大溜め"
                   : chargeTime_ >= lanceSettings_.fiveGateTime  ? "覇王・5ゲート"
                   : chargeTime_ >= lanceSettings_.threeGateTime ? "覇王・3ゲート"
                                                                 : "覇王・1ゲート";
        return gates_.empty() ? "覇王・待機" : "覇王・ゲート槍発射";
    }
    PlayerAttackMovement GetAttackMovement() const override {
        if (recovery_.Active()) return recovery_.Movement();
        return charging_ ? CopyAttackMovement(GetProfile(), "player_crown_lance_charge") : PlayerAttackMovement{};
    }

    // 被弾開始で近接と構えを終了する。発射済みの槍はコピー解除まで維持する。
    void NotifyDamageStarted(Player& player) override {
        recovery_.Clear();
        melee_.End(player);
        if (charging_ || !releasePosePlayed_) StopLanceCharge(player);
    }
    void Cancel(Player& player) override {
        // 同期の命中通知から解除された場合、通知前の弾参照を使えないことを世代番号で伝える。
        ++cancelGeneration_;
        recovery_.Clear();
        melee_.End(player);
        melee_.ClearEffects();
        effects_.Clear();
        warp_.ClearEffects();
        // 溜め中と発射済みの表示を両方回収し、回避中であれば解除時に操作と断面も復元する。
        StopLanceCharge(player);
        player.ClearSlimeAbilityMotion();
        warp_.End(player);
        shots_.clear();
        remnants_.clear();
    }

  private:
    // 王冠能力の弾表示と移動状態。lifeは残り秒、scaleは外観と命中半径の倍率です。
    struct Shot {
        std::unique_ptr<engine::Object3D> visual;
        engine::Vector3 position{}, velocity{}, gateOrigin{};
        CrownLanceTrail trail;
        CrownLanceSettings::Appearance appearance;
        CrownPresentationSettings presentation;
        float dissolveAge = 0.0f;
        std::int32_t nextFlake = 0;
        bool emerging = true;
        float life = 2.2f;
        float scale = 1.0f, damageScale = 1.0f, firstStep = -1.0f;
        bool isFullChargeCenter = false;
    };

    std::unique_ptr<engine::Object3D> MakeVisual(const char* model) {
        // 弾や翼は表示専用で生成し、物理Colliderの対象には入れない。
        auto* scene = SceneManager::GetInstance()->GetCurrentScene();
        // シーンまたは共通の描画資源を取得できない場合は表示物を生成しない。
        if (!scene || !scene->GetObject3DCommon())
            return {};
        auto visual = std::make_unique<engine::Object3D>();
        visual->Initialize(scene->GetObject3DCommon());
        visual->SetModel(model);
        visual->SetMaterialType(0);
        visual->SetBlendMode(BlendMode::kNone);
        visual->SetColliderType(engine::ColliderType::kNone);
        visual->SetCollisionAttribute(0);
        visual->SetCollisionMask(0);
        visual->SetColor({1.0f, 0.92f, 0.58f, 1.0f});
        visual->SetEmissive(1.7f);
        visual->SetEnableLighting(true);
        return visual;
    }

    // 通常の表示物を、材質22を使う発光ゲートへ初期化する。
    std::unique_ptr<engine::Object3D> MakeGatePortal() {
        auto portal = MakeVisual("Gimmicks/portal_surface");
        // 元になる面を生成できなければ、材質や回転の設定は行わない。
        if (!portal) return {};
        // 背景の照明に左右されない発光ゲートを作り、姿勢はクォータニオンで合わせる。
        portal->SetMaterialType(22);
        portal->SetBlendMode(BlendMode::kNormal);
        portal->SetEnableLighting(false);
        portal->SetEmissive(2.5f);
        portal->SetQuaternionMaster(true);
        // ゲート用の材質がある場合は、厚みを抑えた面に波と縁の見た目を設定する。
        if (auto* material = portal->GetMeshRenderer()->GetWaterParamData()) {
            material->waveSpeed = 2.25f;
            material->waveHeight = 1.18f;
            material->waveFrequency = 19;
            material->effectSoftness = 0.56f;
            material->effectScale = 1.12f;
            material->effectScaleX = material->effectScaleY = 1;
            material->effectScaleZ = 0.12f;
            material->effectType = 1;
            material->flowSpeedX = material->flowSpeedY = 0;
            material->billboardScale = 1;
        }
        return portal;
    }
    // 専用の経路判定とゲート生成へ回避を委譲し、移動を開始できた場合にtrueを返す。
    bool BeginWarp(Player& player) {
        // 回避を再使用できない間は、移動経路や射出口を作らない。
        if (!warp_.Ready()) return false;
        // 安全な経路とゲートを用意できなければ失敗を返し、槍の溜めもそのまま続ける。
        if (!warp_.Begin(player, CrownLanceSettings::Get(), [this](const auto& normal) {
                auto gate = MakeGatePortal();
                // ゲートが作れた場合だけ、経路の法線から入口を向ける角度を設定する。
                if (gate) {
                    gate->SetQuaternionMaster(false);
                    gate->SetEulerAngles({-std::asin(normal.y), std::atan2(normal.x, normal.z), 0});
                }
                return gate;
            })) return false;
        // 回避の開始が成功した後にだけ、槍の溜めを解除して突入動作へ切り替える。
        StopLanceCharge(player);
        player.PlaySlimeAbilityMotion("player_ability_crown_warp_enter", false, 1.0f, 0.0f, 0.0f);
        return true;
    }

    static void DrawVisual(engine::Object3D* visual, ID3D12Resource* point, ID3D12Resource* spot, engine::Camera* camera,
                           std::int32_t previewBufferIndex) {
        // 指定カメラがあればプレビューへ描き、なければ現在シーンのカメラを使う。
        // 描画物がない場合は行列を更新せず戻る。
        if (!visual)
            return;
        visual->UpdateWorldMatrix(true);
        // 明示されたカメラがあれば、通常のカメラ定数を使わず専用の描画経路へ渡す。
        if (camera)
            visual->DrawForCamera(camera, point, spot, previewBufferIndex);
        // カメラを指定していない場合は、現在の視点を反映して通常描画する。
        else {
            visual->RefreshRenderCameraData();
            visual->Draw(point, spot);
        }
    }

    // 溜め中は追従し、解除時に射出口・方向・発射時刻を固定します。
    struct Gate {
        std::unique_ptr<engine::Object3D> portal, lance;
        engine::Vector3 position{}, direction{0, 0, 1};
        float age = 0.0f, releaseAt = 0.0f, closeAge = 0.0f, scale = 1.0f;
        std::int32_t index = 0;
        bool fired = false;
    };
    // 十分な長さを持つ方向を単位化し、向きを得られなければ呼出元が指定した既定方向を返す。
    static engine::Vector3 Unit(const engine::Vector3& v, const engine::Vector3& fallback = {0, 0, 1}) {
        return engine::Math::Length(v) > 0.0001f ? engine::Math::Normalize(v) : fallback;
    }
    // モデルの基準軸を指定方向へ向けるため、最短回転のクォータニオンを求める。
    static engine::Quaternion AimRotation(const engine::Vector3& axis, const engine::Vector3& direction) {
        const auto to = Unit(direction, axis);
        const float dot = std::clamp(engine::Math::Dot(axis, to), -1.0f, 1.0f);
        // 向きが一致するなら恒等回転を返します。ほぼ正反対では外積が小さくなるため、別途半回転の軸を選びます。
        // 基準軸と目標方向がほぼ平行で同じ向きなら、回転は不要。
        if (dot > 0.9999f)
            return {0, 0, 0, 1};
        // ほぼ正反対の場合は通常の外積が小さくなるため、直交する基準軸で半回転を作る。
        if (dot < -0.9999f) {
            const auto perpendicular = Unit(engine::Math::Cross(axis, {1, 0, 0}), {0, 1, 0});
            return {perpendicular.x, perpendicular.y, perpendicular.z, 0};
        }
        const auto cross = engine::Math::Cross(axis, to);
        // 外積を回転軸の成分に使い、内積から半角に対応する係数を求めて単位クォータニオンを組み立てます。
        const float q = std::sqrt((1 + dot) * 2);
        return {cross.x / q, cross.y / q, cross.z / q, q * 0.5f};
    }
    // プリセットは共有のまま保持し、今回の位置・方向・大きさだけをコピーへ反映します。
    static void EmitLanceEffect(const char* preset, const engine::Vector3& position,
                                const engine::Vector3& direction = {0, 1, 0}, float scale = 1.0f,
                                bool directed = false) {
        auto* particles = GpuParticleManager::GetInstance();
        // GPU粒子を利用できない場合は、共有設定の検索や発生を行わない。
        if (!particles || !particles->IsInitialized())
            return;
        const auto& presets = particles->GetPresets();
        const auto found = vfxAssetPath::FindPreset(presets, preset);
        // 要求された名前を見つけられない場合は、似た別の素材へ置き換えない。
        if (found == presets.end())
            return;
        auto config = found->second;
        config.emitPos = position;
        config.baseSize *= scale;
        config.midSize *= scale;
        config.endSize *= scale;
        config.emitArea = config.emitArea * scale;
        config.shapeRadius *= scale;
        config.fieldRadius *= scale;
        // 向きを指定した場合だけ、プリセットの速度の大きさを保って方向を上書きする。
        if (directed)
            config.emitVelocity = Unit(direction) * engine::Math::Length(config.emitVelocity);
        particles->EmitFromConfig(config);
    }
    static void EmitLanceImpact(const engine::Vector3& position, const engine::Vector3& normal, bool full,
                                const CrownPresentationSettings& style) {
        // 接触面の手前へ少し出し、敵・壁の中へ閃光が埋まるのを防ぎます。
        const auto origin = position + normal * 0.08f;
        EmitLanceEffect(full ? "player/copy/crown/player_crown_impact_full" : "player/copy/crown/player_crown_impact", origin, normal, (full ? 0.65f : 0.46f) * style.particleScale);
        EmitLanceEffect("player/copy/crown/player_crown_impact_sparks", origin, normal, (full ? 1.4f : 1.1f) * style.particleScale, true);
        // 最大溜めの中央の槍でだけ、接触面の法線側へ追加の破片を出す。
        if (full)
            EmitLanceEffect("player/copy/crown/player_crown_impact_shards", origin, normal, style.particleScale, true);
    }
    // 槍モデルのローカルY範囲は -2.02（穂先）～+1.63（柄尻）。縦倍率はモデルと共有します。
    static constexpr float kAxialScale = 0.8f;
    static float RearOffset(float scale) { return 1.63f * kAxialScale * scale; }
    // 射出口を切断面とし、槍がまだ通過していない側を隠す材質設定を作る。
    static engine::Object3D::PortalClipSettings GateClip(const Gate& gate,
                                                         const CrownLanceSettings::Appearance& style) {
        engine::Object3D::PortalClipSettings clip;
        clip.enabled = true;
        clip.center = gate.position;
        clip.normal = gate.direction * -1.0f;
        clip.progress = 0.5f;
        clip.edgeWidth = style.edgeWidth;
        clip.dissolve = 0.12f;
        clip.color = {style.edgeColor[0], style.edgeColor[1], style.edgeColor[2], 1.0f};
        return clip;
    }
    static void UpdateTrail(Shot& shot, float fade, float deltaTime) {
        const auto direction = Unit(shot.velocity);
        // 柄がゲートから出る前の帯はゲート面へ畳み、溜め側へ突き抜けないようにします。
        const float fromGate = engine::Math::Dot(shot.position - shot.gateOrigin, direction);
        const auto root = shot.gateOrigin + direction * (std::max)(0.0f, fromGate - RearOffset(shot.scale));
        shot.trail.Update(root, shot.appearance, shot.isFullChargeCenter, fade, deltaTime);
    }
    // 飛行が止まった時点の表示へ切り替え、ゲート用の断面を外して消散時計を始める。
    static void BeginShotDissolve(Shot& shot) {
        shot.visual->ClearPortalClip();
        shot.dissolveAge = 0.0f;
    }
    // 接触後の見た目と軌跡を進め、表示を破棄してよい終端でtrueを返す。
    static bool UpdateShotDissolve(Shot& shot, float deltaTime) {
        shot.dissolveAge += deltaTime;
        const float duration = shot.appearance.dissolveTime * (shot.isFullChargeCenter ? 1.25f : 1.0f);
        // 約2フレームだけ金属の姿を残し、衝撃と崩れ始めを区別します。
        constexpr float kImpactHoldSeconds = 0.03f;
        // 着弾して0.03秒未満の間は、崩れを始めず槍と未減衰の軌跡を描く。
        if (shot.dissolveAge < kImpactHoldSeconds) {
            shot.visual->Update(deltaTime);
            UpdateTrail(shot, 0.0f, deltaTime);
            return false;
        }
        const float t = SmoothStep((shot.dissolveAge - kImpactHoldSeconds) / duration);
        // 設定した消散時間を消化したら、破棄の合図を返して追加の表示は作らない。
        if (t >= 1.0f)
            return true;
        const auto direction = Unit(shot.velocity);
        // 一斉命中時も金属の輪郭が残るよう、ボスの強い発光は最大溜めの中央だけに使います。
        if (shot.isFullChargeCenter && shot.visual->GetMaterialType() != 4) {
            shot.visual->SetMaterialType(4);
            shot.visual->SetBlendMode(BlendMode::kNormal);
            shot.visual->SetColor({1, 1, 1, 1});
            shot.visual->SetEmissive(1.35f);
        }
        const auto& tint = shot.appearance.edgeColor;
        engine::Object3D::PortalClipSettings clip;
        clip.enabled = true;
        clip.center = shot.position;
        clip.normal = direction;
        // 最大溜めの中央の槍は専用材質に渡す進行率で消散させる。
        if (shot.isFullChargeCenter) {
            clip.progress = t;
        } else {
            // 材質0のノイズ付き断面を穂先から柄尻へ通します。
            const float tip = 2.02f * kAxialScale * shot.scale;
            const float rear = RearOffset(shot.scale);
            const float margin = 0.30f + shot.appearance.edgeWidth;
            const float cut = std::lerp(tip + margin, -rear - margin, t);
            clip.center = shot.position + direction * cut;
            clip.progress = 0.5f;
            clip.dissolve = 0.38f;
        }
        clip.edgeWidth = shot.appearance.edgeWidth * (1.0f - 0.30f * t);
        clip.color = {tint[0], tint[1], tint[2], 1.0f};
        shot.visual->SetPortalClip(clip);
        shot.visual->Update(deltaTime);
        UpdateTrail(shot, t, deltaTime);
        // 発生回数は槍ごとに制限。穂先から柄へ崩れる位置に小片を置きます。
        constexpr std::array<float, 3> kDissolveFlakeThresholds{0.22f, 0.50f, 0.76f};
        const std::int32_t count = shot.isFullChargeCenter ? 3 : 1;
        // 一回の更新で複数の発生しきい値を越えた場合も、槍ごとの上限まで順に処理する。
        while (shot.nextFlake < count && t >= kDissolveFlakeThresholds[shot.nextFlake]) {
            const float offset = (0.40f - kDissolveFlakeThresholds[shot.nextFlake] * 1.85f) * shot.scale;
            EmitLanceEffect("player/copy/crown/player_crown_dissolve_flakes", shot.position + direction * offset);
            ++shot.nextFlake;
        }
        return false;
    }
    void BeginLanceCharge(Player& player) {
        // 溜め始めに設定をコピーし、エディターで調整しても実行中の発射段階や見た目を変えない。
        lanceSettings_ = CrownLanceSettings::Get();
        lanceSettings_.Sanitize();
        presentation_ = CrownPresentationSettings::Get();
        presentation_.Sanitize();
        charging_ = true;
        fullCuePlayed_ = false;
        chargeVfxTimer_ = readyFlashTime_ = 0.0f;
        chargeTime_ = releaseTime_ = 0;
        player.PlaySlimeAbilityMotion("player_ability_crown_charge", true, 1.0f, 0.05f, 0.08f);
        UpdateLanceCharge(player, 0.0f);
    }
    // 溜めか発射を中断し、まだゲートが所有する表示物を破棄する。
    void StopLanceCharge(Player& player) {
        // モーションを再生している可能性がある溜め中とゲートのある場合だけ停止を要求する。
        if (charging_ || !gates_.empty())
            player.StopSlimeAbilityMotion(0.08f);
        charging_ = false;
        chargeTime_ = releaseTime_ = 0;
        gates_.clear();
        chargeVfxTimer_ = readyFlashTime_ = 0.0f;
    }
    // 溜めの時間で必要なゲートを追加し、照準に向けて編隊を組み直す。deltaTimeは秒。
    void UpdateLanceCharge(Player& player, float deltaTime) {
        // 溜めを離した後は射出口の位置が確定しているため、形成を更新しない。
        if (!charging_)
            return;
        const float previousCharge = chargeTime_;
        chargeTime_ = (std::min)(lanceSettings_.fullChargeTime, chargeTime_ + deltaTime);
        const std::int32_t count = lanceSettings_.GateCount(chargeTime_);
        // 溜めが進むほどゲートを増やし、ゲートと槍を両方作れた分だけ編隊へ加える。
        while (static_cast<std::int32_t>(gates_.size()) < count) {
            Gate gate;
            gate.index = static_cast<std::int32_t>(gates_.size());
            const float birth = gate.index == 0   ? 0.0f
                                : gate.index <= 2 ? lanceSettings_.threeGateTime
                                                  : lanceSettings_.fiveGateTime;
            // 段階を跨いだフレームの途中から開き始め、低fpsでも突然全開にしません。
            gate.age = -std::clamp(birth - previousCharge, 0.0f, deltaTime);
            gate.portal = MakeGatePortal();
            gate.lance = MakeVisual("Effects/false_king_crown_lance");
            // ポータルか槍の片方が作れなかったら、組が揃ったものだけを残し追加を中止する。
            if (!gate.portal || !gate.lance)
                break;
            gate.portal->SetIsVisible(false);
            gate.lance->SetIsVisible(false);
            // 本体の金属色を残し、強い発光はゲートと軌跡に受け持たせます。
            gate.lance->SetEmissive(1.05f);
            gate.lance->SetEnableEnvMap(true);
            gate.lance->SetEnvIntensity(0.5f);
            gate.lance->SetRoughness(0.28f);
            gate.lance->SetMetallic(0.55f);
            gate.lance->SetQuaternionMaster(true);
            gates_.push_back(std::move(gate));
        }
        UpdateGateFormation(player);
        // 最大溜めに達した瞬間に、中央ゲートで強調表示を一度だけ出す。
        if (!fullCuePlayed_ && chargeTime_ >= lanceSettings_.fullChargeTime) {
            fullCuePlayed_ = true;
            readyFlashTime_ = 0.16f;
            // 中央のゲートを作成できているときだけ、その位置に最大溜めの粒子を出す。
            if (!gates_.empty())
                EmitLanceEffect("player/copy/crown/player_crown_ready", gates_.front().position, {0,1,0}, presentation_.particleScale);
        }
        chargeVfxTimer_ -= deltaTime;
        // 溜め中の発生時計が切れたら、現在用意できている各ゲートに粒子を出す。
        if (chargeVfxTimer_ <= 0.0f) {
            for (const auto& gate : gates_)
                EmitLanceEffect("player/copy/crown/player_crown_charge", gate.position, gate.direction, presentation_.lanceGateScale);
            // 自動ループを使わず、溜め解除後には新しい粒子を発生させません。
            chargeVfxTimer_ = 0.10f;
        }
    }
    // 本体のワールド位置と照準点を使い、各ゲートの位置・発射方向・大きさを更新する。
    void UpdateGateFormation(Player& player) {
        const auto forward = NormalizePlanar(player.GetForwardDirection());
        const engine::Vector3 right{forward.z, 0, -forward.x};
        const auto aim = Unit(player.GetLockOnAttackDirection(), forward);
        const auto anchor = player.GetWorldPosition() + engine::Vector3{0, 1.2f, 0};
        // 照準にある敵か地面を共通の発射先にし、何にも当たらなければ前方の仮の点を使う。
        engine::PhysicsQueryFilter filter;
        filter.mask = engine::collisionCategories::Get("enemy") | engine::collisionCategories::Get("ground");
        filter.ignoredObject = &player;
        const auto hit = engine::CollisionManager::GetInstance()->Raycast(anchor, aim, 32, filter);
        const auto target = hit.isHit ? hit.hitPoint : anchor + aim * 18;
        // 五本目のしきい値以降の溜めを最大溜めまでで評価し、中央槍の大きさに使う。
        const float full = SmoothStep((chargeTime_ - lanceSettings_.fiveGateTime) /
                                      (lanceSettings_.fullChargeTime - lanceSettings_.fiveGateTime));
        // 中央と左右を交互に並べ、各ゲートの位置から同じ照準点へ向ける。
        for (auto& gate : gates_) {
            // 並びは中央、左、右の順に外側へ広げます。外側の出口ほど低く置きます。
            const std::int32_t column = gate.index == 0 ? 0 : (gate.index + 1) / 2 * (gate.index % 2 ? -1 : 1);
            gate.position = player.GetWorldPosition() - forward * 0.35f +
                            right * (static_cast<float>(column) * lanceSettings_.gateSpacing * std::sqrt(presentation_.lanceGateScale)) +
                            engine::Vector3{0, lanceSettings_.gateHeight - std::abs(column) * 0.30f, 0};
            gate.direction = Unit(target - gate.position, aim);
            gate.scale = (gate.index == 0 ? 1 + (lanceSettings_.fullCenterScale - 1) * full : 1) * presentation_.lanceScale;
        }
    }
    // 溜めを解除した時点の編隊を固定し、外側から中央の順で発射時刻を決める。
    void ReleaseLances(Player& player) {
        // 溜め中でなければ発射を予約済みか未開始なので何もしない。
        if (!charging_)
            return;
        UpdateGateFormation(player);
        charging_ = false;
        releaseTime_ = 0;
        lanceCooldown_ = lanceSettings_.cooldown;
        // 全ゲートが開き切る時刻を基準にし、外側を先に、中央の槍を最後に予約する。
        float readyWait = 0.0f;
        for (const auto& gate : gates_)
            readyWait = (std::max)(readyWait, lanceSettings_.openDuration - gate.age);
        for (auto& gate : gates_) {
            const std::int32_t rank = gate.index == 0 ? static_cast<std::int32_t>(gates_.size()) - 1 : gate.index - 1;
            gate.releaseAt = readyWait + rank * lanceSettings_.shotInterval;
        }
        // 発射反動は最初の槍が実際に出る時に始めます。
        player.StopSlimeAbilityMotion(0.08f);
        releasePosePlayed_ = false;
    }
    void FireGate(Player& player, Gate& gate) {
        // 弾を生成できるかにかかわらず予約を消費し、ゲートは閉じ始める。最初の予約で反動を再生する。
        gate.fired = true;
        gate.closeAge = 0;
        // 最初の発射予約を消費するときだけ、プレイヤーの反動を再生する。
        if (!releasePosePlayed_) {
            player.PlaySlimeAbilityMotion("player_ability_crown_release", false, 1.0f, 0.02f, 0.08f);
            releasePosePlayed_ = true;
            recovery_.Start(CopyAttackMovement(GetProfile(), "player_crown_lance_recovery"));
        }
        // 飛行中の弾が上限の24に達していたら、予約を消費するだけで実弾を出さない。
        if (shots_.size() >= 24)
            return;
        const auto position = gate.position + gate.direction * 0.45f;
        const auto anchor = player.GetWorldPosition() + engine::Vector3{0, 1.2f, 0};
        const auto delta = position - anchor;
        const float distance = engine::Math::Length(delta);
        engine::PhysicsQueryFilter wall;
        wall.mask = engine::collisionCategories::Get("ground");
        wall.ignoredObject = &player;
        // 本体と射出口の間の壁を調べ、塞がれている場合は接触点の演出だけを出す。
        const auto obstruction =
            engine::CollisionManager::GetInstance()->SphereCast(anchor, 0.16f * gate.scale, Unit(delta), distance, wall);
        // 本体から射出口までに地形があれば、弾を出さず地形との接触位置に着弾表示を出す。
        if (obstruction.isHit) {
            EmitLanceImpact(obstruction.hitPoint, Unit(obstruction.normal, gate.direction * -1.0f), false, presentation_);
            return;
        }
        // ゲートの槍を飛行中の弾へ引き継ぎ、座標・速度・寿命・消散の設定を独立させる。
        Shot shot;
        shot.visual = std::move(gate.lance);
        shot.visual->SetIsVisible(true);
        shot.position = gate.position;
        shot.velocity = gate.direction * lanceSettings_.shotSpeed;
        shot.scale = gate.scale;
        // 外観倍率で威力まで増えないよう、溜めによる倍率だけを使います。
        shot.damageScale = gate.scale / presentation_.lanceScale;
        shot.presentation = presentation_;
        shot.isFullChargeCenter = gate.index == 0 && chargeTime_ >= lanceSettings_.fullChargeTime;
        shot.gateOrigin = gate.position;
        shot.appearance = lanceSettings_.appearance;
        shot.appearance.trailWidth *= presentation_.trailScale;
        shot.appearance.trailLength *= std::sqrt(presentation_.lanceScale);
        shot.trail.Initialize(shot.visual->GetObjectCommon(), gate.position, AimRotation({0, -1, 0}, gate.direction));
        shot.visual->SetPortalClip(GateClip(gate, shot.appearance));
        shot.visual->SetQuaternion(AimRotation({0, -1, 0}, gate.direction));
        shot.visual->SetTranslate(shot.position);
        shot.visual->SetScale({0.6f * gate.scale, kAxialScale * gate.scale, 0.6f * gate.scale});
        // 弾を登録した後、発射口に閃光と輪を出し、最大溜めの中央槍だけ粒子を強める。
        shots_.push_back(std::move(shot));
        EmitLanceEffect(shots_.back().isFullChargeCenter ? "player/copy/crown/player_crown_muzzle_full" : "player/copy/crown/player_crown_muzzle",
                        gate.position, gate.direction, presentation_.particleScale);
        effects_.Ring(gate.position, gate.direction, lanceSettings_.gateScale * presentation_.lanceGateScale * 1.3f);
        EmitLanceEffect("player/copy/crown/player_crown_launch_streak", gate.position, gate.direction,
                        (shots_.back().isFullChargeCenter ? 1.4f : 1.1f) * presentation_.particleScale, true);
    }
    // 予約済みの槍を時刻順に発射し、各ゲートの開閉を進める。deltaTimeは秒。
    void UpdateGates(Player& player, float deltaTime) {
        // 溜めを解除してからだけ、発射予約の基準時計を進める。
        if (!charging_)
            releaseTime_ += deltaTime;
        std::array<Gate*, 5> due{};
        size_t count = 0;
        for (auto& gate : gates_) {
            // 発射済みのゲートは次の発射候補にせず、閉じ始めてからの秒数を進める。
            if (gate.fired)
                gate.closeAge += deltaTime;
            // 未発射かつ溜めを終え、今回の時刻までに予約へ到達した槍を発射候補にする。
            else if (!charging_ && releaseTime_ + 0.00001f >= gate.releaseAt)
                due[count++] = &gate;
        }
        // 同一フレームに複数本が発射されても、予約時刻順に出し中央槍を最後に保ちます。
        std::sort(due.begin(), due.begin() + count,
                  [](const Gate* a, const Gate* b) { return a->releaseAt < b->releaseAt; });
        for (size_t i = 0; i < count; ++i) {
            auto& gate = *due[i];
            const auto before = shots_.size();
            FireGate(player, gate);
            gate.closeAge = std::clamp(releaseTime_ - gate.releaseAt, 0.0f, deltaTime);
            // 実際に弾を登録できた場合だけ、予約時刻から今までの残り時間を初回移動用に渡す。
            if (shots_.size() > before)
                shots_.back().firstStep = gate.closeAge;
        }
        // 発射前は開き具合に合わせて槍を見せ、発射後はゲートだけを縮める。
        for (auto& gate : gates_) {
            gate.age += deltaTime;
            const float visibility = gate.fired ? 1 - SmoothStep(gate.closeAge / lanceSettings_.closeDuration)
                                                : SmoothStep(gate.age / lanceSettings_.openDuration);
            const float visibleScale = (std::max)(0.001f, visibility);
            const float size = lanceSettings_.gateScale * gate.scale / presentation_.lanceScale *
                               presentation_.lanceGateScale * visibleScale;
            gate.portal->SetIsVisible(visibility > 0.02f);
            gate.portal->SetTranslate(gate.position);
            gate.portal->SetScale({size, size, size});
            gate.portal->SetQuaternion(AimRotation({0, 0, 1}, gate.direction));
            gate.portal->SetColor({1.0f, 0.70f, 0.22f, 0.92f * visibility});
            // 特殊材質の定数が利用できれば、開閉率・溜めと中央の発光を反映する。
            if (auto* material = gate.portal->GetMeshRenderer()->GetWaterParamData()) {
                material->effectIntensity =
                    0.08f + 1.6f * visibility + (gate.index == 0 ? 0.8f * readyFlashTime_ / 0.16f : 0.0f);
                material->uvOffsetY = visibility;
                material->uvOffsetX = visibility * (0.45f + 0.55f * chargeTime_ / lanceSettings_.fullChargeTime);
            }
            gate.portal->Update(deltaTime);
            // ゲートが槍を所有している未発射中だけ、開き具合から露出する長さと姿勢を更新する。
            if (!gate.fired) {
                gate.lance->SetIsVisible(visibility > 0.02f);
                const float tip = 2.02f * kAxialScale * gate.scale;
                gate.lance->SetTranslate(gate.position - gate.direction * (tip * (1.0f - visibility)));
                gate.lance->SetScale({0.6f * gate.scale, kAxialScale * gate.scale, 0.6f * gate.scale});
                gate.lance->SetPortalClip(GateClip(gate, lanceSettings_.appearance));
                gate.lance->SetQuaternion(AimRotation({0, -1, 0}, gate.direction));
                gate.lance->Update(deltaTime);
            }
        }
        // 閉じ切ったゲートを取り除き、次の溜めを開始できる状態へ戻す。
        std::erase_if(gates_,
                      [&](const Gate& gate) { return gate.fired && gate.closeAge >= lanceSettings_.closeDuration; });
    }

    std::vector<Shot> shots_;
    // 命中済みの表示だけを持つ。こちらにはSphereCastやダメージ処理を呼びません。
    std::vector<Shot> remnants_;
    std::vector<Gate> gates_;
    CrownLanceSettings lanceSettings_;
    CrownPresentationSettings presentation_;
    CrownAbilityEffects effects_;
    float releaseTime_ = 0;
    float chargeVfxTimer_ = 0.0f, readyFlashTime_ = 0.0f;
    bool charging_ = false, fullCuePlayed_ = false, releasePosePlayed_ = false;
    CrownGateWarp warp_;
    CrownMeleeCombo melee_;
    float time_ = 0.0f, ambientTimer_ = 0.0f, chargeTime_ = 0.0f;
    CopyAttackRecovery recovery_;
    float lanceCooldown_ = 0.0f;
    // 同期ダメージ通知からCancel()された場合に、更新中の弾の参照を打ち切るための世代番号。
    uint64_t cancelGeneration_ = 0;
};
} // namespace

namespace copyAbilityDetail {
std::unique_ptr<ICopyAbilitySession> CreateCrownCopyAbility(const EnemyAttackProfile& profile) {
    // 王冠能力の一時状態を獲得ごとに独立させる。
    return std::make_unique<CrownCopyAbility>(profile);
}
} // namespace copyAbilityDetail
