#include <cstdint>
#include "game/actor/enemy/boss/false_king/BossIntroSettings.h"
#define NOMINMAX
#include "game/scene/gameplay/GamePlayScene.h"
#include "engine/graphics/3d/light/SceneLighting.h"
#include "Easing.h"
#include <numbers>
#include "Log.h"
#include "game/ui/ControlsGuideOverlay.h"
#include "game/ui/PauseMenuOverlay.h"
#include "game/actor/enemy/boss/false_king/EnemyFalseKingSlime.h"
#include "GimmickEventReceiver.h"
#include "GimmickBossGate.h"
#include "CameraManager.h"
#include "PostEffect.h"
#include "SceneManager.h"
#include "VfxSequencer.h"
#include "GpuParticleManager.h"
#include "game/ui/SpriteLayoutScaler.h"
#include "TextureManager.h"
#include "game/crown/CrownFinaleTuning.h"
#include "game/crown/CrownBarrageCamera.h"
#include "game/actor/combat/CombatGroundQuery.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <unordered_set>
#include <utility>
#include "game/system/scene/ObjectManager.h"

namespace {
constexpr const char* kCrownFinalePath = "Resources/json/scenario/stage3/crown_finale.json";
constexpr std::pair<const char*, float CrownFinaleTuning::*> kFinaleMarkers[] = {
    {"break", &CrownFinaleTuning::movieFragmentBreakTime}, {"land", &CrownFinaleTuning::movieFragmentLandTime},
    {"rage", &CrownFinaleTuning::movieRageTime}, {"pull", &CrownFinaleTuning::movieFragmentPullTime},
    {"absorb", &CrownFinaleTuning::movieFragmentAbsorbTime}, {"body", &CrownFinaleTuning::movieBodyTime},
    {"crown", &CrownFinaleTuning::movieCrownTime}, {"wings", &CrownFinaleTuning::movieWingsTime},
    {"escape", &CrownFinaleTuning::movieEscapeTime}, {"arrive", &CrownFinaleTuning::movieArriveTime}};


engine::Vector3 CrownLerp(const engine::Vector3& a, const engine::Vector3& b, float t) {
    // 演出側で決めた補間率をそのまま使い、位置の行き過ぎも表現できる。
    return a + (b - a) * t;
}

engine::Vector3 CrownLook(const engine::Vector3& eye, const engine::Vector3& target) {
    // カメラから注視点への俯角と水平方向の角度を作る。
    const engine::Vector3 d = target - eye;
    return {-std::atan2(d.y, std::sqrt(d.x * d.x + d.z * d.z)), std::atan2(d.x, d.z), 0.0f};
}
} // namespace

// 王冠決着の専用進行です。通常戦闘を止めた後、演出用状態を更新し、終了時にワールドを復元します。

bool GamePlayScene::StartSealRevealMovie(EnemyFalseKingSlime* boss, const engine::Vector3& center) {
    // ボス登場や報酬の演出中は開始しません。封印失敗と共通の開始処理で操作と視点を確保できた場合に公開カットへ切り替えます。
    if (arenaBossIntroActive_ || arenaBossRewardActive_ || !StartSealBarrageMovie(boss, center))
        return false;
    auto& movie = sealBarrageMovie_;
    movie.reveal = true;
    // 実際の攻撃を発生させるとHPや当たり判定が変わるため、カット専用の弾を用意する。
    if (auto* common = GetObject3DCommon()) {
        movie.shieldDemoShot = std::make_unique<engine::Object3D>();
        auto& shot = *movie.shieldDemoShot;
        shot.Initialize(common);
        shot.SetModel("Primitives/sphere");
        shot.SetEnableLighting(false);
        shot.SetEmissive(2.4f);
        shot.SetCastShadow(false);
        shot.SetColliderType(engine::ColliderType::kNone);
        shot.SetIsVisible(false);
    }
    return true;
}

bool GamePlayScene::StartSealBarrageMovie(EnemyFalseKingSlime* boss, const engine::Vector3& strike) {
    // 操作や視点を別の演出が所有している場合は、封印失敗の固定視点を重ねません。
    if (!SceneManager::GetInstance()->IsPlaying() || !boss || !player_ || player_->IsDead() ||
        player_->IsCinematicLocked() || crownFinale_.active || sealBarrageMovie_.active ||
        isGoal_)
        return false;
    engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();
    // メインカメラを取得できなければ、視点を保存できないため開始しません。
    if (!camera)
        return false;

    // 被弾後も同じ画角と追従先へ戻せるよう、操作を止める前に状態を保存します。
    auto& movie = sealBarrageMovie_;
    movie = SealBarrageMovieState{};
    movie.active = true;
    movie.time = 0.0f;
    movie.hadControl = player_->IsControlActive();
    movie.boss = boss->GetWorldPosition();
    movie.strike = strike;
    movie.eye = camera->GetEye();
    movie.target = camera->GetTargetPoint();
    movie.rotation = camera->GetRotation();
    movie.fov = camera->GetFovY();
    movie.follow = camera->GetFollowTarget();
    movie.followMode = camera->GetFollowMode();
    // 描画補正の設定が利用できるときは、黒帯の元の高さも保存します。
    if (auto* params = PostEffect::GetInstance()->GetParams())
        movie.bars = params->cinemaBarHeight;

    // 通常のCinematicLockはGameRuleがダメージを無効にするため、この技では操作だけ止めます。
    player_->SetIsControlActive(false);
    player_->SetVelocity({});
    camera->SetInputEnabled(false);
    camera->SetFollowTarget(nullptr);
    camera->SetLockOnTarget(nullptr);
    camera->SetFollowMode(engine::Camera::FollowMode::kFixedPoint);
    return true;
}

void GamePlayScene::UpdateSealBarrageMovie(float deltaTime) {
    // 封印失敗時の専用カメラを進め、通常の戦闘カメラと切り分ける。
    auto& movie = sealBarrageMovie_;
    // 固定視点の演出が進行中の場合だけ、専用カメラを更新します。
    if (!movie.active)
        return;
    engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera();

    // 必要な対象を失った場合や続く継承演出が始まった場合は、固定視点を解除します。
    if (!camera || !player_ || crownFinale_.active) {
        FinishSealBarrageMovie();
        return;
    }

    const float previousTime = movie.time;
    movie.time += std::max(0.0f, deltaTime);
    const auto& tuning = CrownFinaleTuning::Get();
    const float duration = movie.reveal ? SealRevealTiming::Duration : tuning.sealBarrageMovieDuration;

    // 公開版または失敗版の尺に達したら、カメラと操作を復元します。
    if (movie.time >= duration) {
        FinishSealBarrageMovie();
        return;
    }

    player_->SetIsControlActive(false);
    player_->SetVelocity({});
    // 槍雨のカメラ姿勢を時刻で評価し、終端直前の0.42秒で元の構図へ寄せます。
    CrownBarrageCameraPose pose;
    // 公開版では、封印中心を基準に結晶と障壁の動きを見せます。
    if (movie.reveal) {
        // 中央へ向かう王を追った後、高めの斜め視点で三つの結晶と接続線を一画面に収める。
        const auto center = movie.strike;
        const float distance = std::max(22.0f, tuning.sealRadius * 1.68f);
        const float widen = Easing::SmoothStep01((movie.time - .75f) / 1.2f);
        const auto arrive = CrownLerp(movie.boss, center, Easing::SmoothStep01(movie.time / 1.55f));
        pose.eye = center + CrownLerp({distance * .48f, 12.5f, distance * .75f},
                                    {distance * .62f, 26.0f, distance * .96f}, widen);
        pose.target = CrownLerp(arrive + engine::Vector3{0, 3.8f, 0},
                               center + engine::Vector3{0, 1.6f, 0}, widen);
        pose.fov = std::lerp(.78f, 1.01f, widen);
        const float open = Easing::SmoothStep01(movie.time / .34f);
        pose.eye = CrownLerp(movie.eye, pose.eye, open);
        pose.target = CrownLerp(movie.target, pose.target, open);
        pose.fov = std::lerp(movie.fov, pose.fov, open);

        // 障壁の完成後に表面で火花を弾かせ、攻撃が王の体まで通らないことを見せる。
        constexpr float shieldImpactTime = 3.40f;
        const engine::Vector3 towardCamera{.56f, 0, .83f};
        const auto impact = center + towardCamera * tuning.sealBarrierRadius + engine::Vector3{0, 2.6f, 0};
        // 演出用の弾を用意できた場合だけ、発射から跳ね返りの終了までタイムラインで表示します。
        if (auto* shot = movie.shieldDemoShot.get()) {
            constexpr float launchTime = shieldImpactTime - .52f;
            constexpr float reboundEnd = shieldImpactTime + .32f;
            const bool visible = movie.time >= launchTime && movie.time < reboundEnd;
            shot->SetIsVisible(visible);
            // 発射済みで反発終了前の間だけ、弾の軌跡と大きさを更新します。
            if (visible) {
                // 衝突前は直進し、表面に届いた瞬間から外側へ逸れて縮む。
                const float approach = std::clamp((movie.time - launchTime) / (shieldImpactTime - launchTime), 0.0f, 1.0f);
                const float rebound = std::clamp((movie.time - shieldImpactTime) / (reboundEnd - shieldImpactTime), 0.0f, 1.0f);
                shot->SetTranslate(impact + towardCamera * (8.0f * (1.0f - approach) + 3.0f * rebound)
                                   + engine::Vector3{rebound * 2.5f, rebound * 2.4f, 0});
                const float radius = .54f * (1.0f - rebound * .8f);
                shot->SetScale({radius, radius, radius});
                shot->SetColor({1.0f, .80f, .35f, 1});
                shot->UpdateWorldMatrix(true);
            }
        }
        // 障壁へ届く時刻を跨いだフレームに一度だけ衝突の火花を出します。
        if (previousTime < shieldImpactTime && movie.time >= shieldImpactTime)
            VfxSequencer::PlayOneShot("shared/slime_elastic_hit_cue", impact);

        // 偽王の周りの紋章と上昇する粒子を見せてから、王冠を離れる光へ視線を移します。
        const float approachCaster = Easing::SmoothStep01((movie.time - SealRevealTiming::ChantStart) / .36f);
        pose.eye = CrownLerp(pose.eye, center + SealRevealTiming::CasterShotOffset, approachCaster);
        pose.target = CrownLerp(pose.target, center + engine::Vector3{0.0f, 2.4f, 1.0f}, approachCaster);
        pose.fov = std::lerp(pose.fov, .86f, approachCaster);
        const float lookSky = Easing::SmoothStep01((movie.time - SealRevealTiming::Launch) / .69f);
        const float lookGround = Easing::SmoothStep01((movie.time - SealRevealTiming::SkyPulse - .14f) / .67f);
        const auto skyEye = center + engine::Vector3{24.0f, 10.0f, 46.0f};
        const auto skyTarget = center + engine::Vector3{0.0f, tuning.sealChantSkyHeight + 5.0f, 0.0f};
        const auto groundEye = center + engine::Vector3{distance * .56f, 25.0f, distance * .96f};
        const auto groundTarget = center + engine::Vector3{0.0f, 1.5f, 4.0f};
        pose.eye = CrownLerp(pose.eye, CrownLerp(skyEye, groundEye, lookGround), lookSky);
        pose.target = CrownLerp(pose.target, CrownLerp(skyTarget, groundTarget, lookGround), lookSky);
        pose.fov = std::lerp(pose.fov, std::lerp(1.07f, 1.01f, lookGround), lookSky);
    // 公開版でなければ、封印失敗用の槍雨カメラを評価します。
    } else {
        pose = EvaluateCrownBarrageCamera(movie.time, movie.boss, movie.strike, tuning);
    }
    // 終端の0.42秒で、位置・注視点・画角を開始前の値へ戻します。
    const float finish = Easing::SmoothStep01((movie.time - duration + 0.42f) / 0.42f);
    pose.eye = CrownLerp(pose.eye, movie.eye, finish);
    pose.target = CrownLerp(pose.target, movie.target, finish);
    pose.fov += (movie.fov - pose.fov) * finish;
    camera->SetInputEnabled(false);
    camera->SetFollowTarget(nullptr);
    camera->SetFollowMode(engine::Camera::FollowMode::kFixedPoint);
    camera->SetEye(pose.eye);
    camera->SetTarget(pose.target);
    camera->SetRotation(CrownLook(pose.eye, pose.target));
    camera->ConfigFixedPoint(pose.eye, CrownLook(pose.eye, pose.target));
    camera->SetFovY(pose.fov);
    camera->Update();
    // 地形とボスの深度も、後から確定したムービー視点へ揃える。
    RefreshRenderCameraData();
    // 弾の実体があり表示されているときに、確定した視点を弾の描画にも反映します。
    if (movie.shieldDemoShot && movie.shieldDemoShot->GetIsVisible())
        movie.shieldDemoShot->RefreshRenderCameraData();
    // 中盤だけ黒帯を広げ、元の設定高を基準に導入と終端で補間します。
    // 描画補正の設定が利用できる場合に黒帯を出し入れします。
    if (auto* params = PostEffect::GetInstance()->GetParams())
        params->cinemaBarHeight = movie.bars + 0.065f *
            Easing::SmoothStep01(movie.time / 0.22f) * (1.0f - Easing::SmoothStep01((movie.time - duration + 0.40f) / 0.40f));
}

void GamePlayScene::FinishSealBarrageMovie() {
    // 演出で切り替えたカメラと操作状態を通常の戦闘へ戻す。
    auto& movie = sealBarrageMovie_;
    // 演出が有効な場合だけ、保存した値を復元します。
    if (!movie.active)
        return;
    movie.active = false;
    movie.shieldDemoShot.reset();
    // 描画補正の設定が残っていれば、黒帯を元の高さに戻します。
    if (auto* params = PostEffect::GetInstance()->GetParams())
        params->cinemaBarHeight = movie.bars;
    // 撃破されたプレイヤーへ操作を返さず、生存時にだけ保存した制御状態を戻します。
    if (player_ && !player_->IsDead()) {
        player_->SetVelocity({});
        player_->SetIsControlActive(movie.hadControl);
    }

    // 演出中の固定視点を解除し、以前の画角・追従先・姿勢をまとめて戻します。
    if (engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera()) {
        camera->SetFovY(movie.fov);
        camera->SetFollowTarget(movie.follow);
        camera->SetFollowMode(movie.followMode);
        camera->SetEye(movie.eye);
        camera->SetTarget(movie.target);
        camera->SetRotation(movie.rotation);
        camera->SetInputEnabled(true);
        camera->Update();
    }
}

CrownFinaleTuning GamePlayScene::GetCrownFinaleTiming() const {
    // ゲーム用の調整値を基準とし、有効なタイムラインのマーカー秒だけを全尺に対する割合に直します。
    auto tuning = CrownFinaleTuning::Get();
    const auto* sequence = crownFinalePlayer_.GetSequence();
    // 正の長さのタイムラインがなければ、定義済みの調整値を返します。
    if (!sequence || sequence->duration <= 0) return tuning;
    tuning.movieDuration = sequence->GetAuthoredDuration();
    for (const auto& [name, member] : kFinaleMarkers)
        tuning.*member = sequence->GetSignalTime(name, tuning.*member * tuning.movieDuration) / tuning.movieDuration;
    return tuning;
}

void GamePlayScene::RequestCrownFinale(EnemyFalseKingSlime* boss) {
    // 開始待ちのボスがおらず、継承演出も準備済みでない場合だけ、更新末尾へ要求を預けます。
    if (!pendingCrownFinaleBoss_ && !crownFinale_.prepared)
        pendingCrownFinaleBoss_ = boss;
}

void GamePlayScene::StartPendingCrownFinale() {
    auto* boss = std::exchange(pendingCrownFinaleBoss_, nullptr);
    // 保留の要求を取り出せなければ、演出を開始しません。
    if (!boss)
        return;

    // 更新の末尾で削除済みのObjectには触れない。
    const auto& objects = GetObjects();
    const auto owned = std::find_if(objects.begin(), objects.end(), [boss](const auto& object) {
        return object.get() == boss && !object->IsReplayRemoved();
    });
    // リプレイで削除されている、シーンに残っていない、または死亡したボスの要求は破棄します。
    if (owned == objects.end() || boss->IsDead())
        return;

    // 演出を開始できた場合だけ、ボスにも開始を通知します。
    if (StartCrownFinale(boss))
        boss->OnCrownFinaleStarted();

    // 最終闘技場の通常追加と、開始失敗時の削除予約も同フレームで反映する。
    objectManager_->ApplyPendingChanges();
}

bool GamePlayScene::StartCrownFinale(EnemyFalseKingSlime* boss) {
    // 演出が準備・進行済み、必要な役者が無い、プレイヤーが死亡・演出ロック中、またはゴール済みなら開始しません。
    if (crownFinale_.active || crownFinale_.prepared || !boss || !player_ || player_->IsDead() ||
        player_->IsCinematicLocked() || isGoal_)
        return false;
    engine::Camera* camera = crownMoviePreview_.active ? crownMoviePreview_.camera.get() : CameraManager::GetInstance()->GetMainCamera();
    // プレビューまたは本編で使うカメラが無い場合は開始できません。
    if (!camera)
        return false;

    // 必須の足場を確認してから生成。攻撃プレビュー等では脱出を開始しません。
    bool hasFloor = false, hasArena = false;

    for (const auto& object : GetObjects()) {
        hasFloor |= object->GetName().find("Stage3_HighCrown_ArenaFloor_") != std::string::npos;
        hasArena |= object->GetName() == "Stage3_HighCrown_ArenaVisual";
    }

    // 必須の床と闘技場がそろっていない場合は、ワールドを書き換えず終了します。
    if (!hasFloor || !hasArena)
        return false;
    // Director用のタイムラインを読み込めなければ演出を準備しません。
    if (!crownFinalePlayer_.Load(kCrownFinalePath, SceneManager::GetInstance()))
        return false;
    const auto* sequence = crownFinalePlayer_.GetSequence();
    const auto& clips = sequence->presentation.objects;
    const auto fragmentClip = std::find_if(clips.begin(), clips.end(), [](const auto& clip) { return clip.name == "fragment"; });
    // 欠片のクリップが無いか姿勢キーと移動設定の両方が無い場合は開始できません。
    if (fragmentClip == clips.end() || (fragmentClip->keys.empty() && fragmentClip->motions.empty())) return false;
    // 飛行を移動トラックから評価できる、有効なプレイヤー飛行クリップが必要です。
    if (std::none_of(clips.begin(),clips.end(),[](const auto& clip) {
        return clip.name=="player_flight" && clip.enabled && !clip.motions.empty();
    })) return false;
    auto& s = crownFinale_;
    const auto tuning = GetCrownFinaleTiming();
    // 編集した節目が欠けたり逆転した場合は、世界を変更する前に停止する。
    float previous = 0;
    for (const auto& [name, member] : kFinaleMarkers) {
        const float time = sequence->GetSignalTime(name, -1) / tuning.movieDuration;
        const float gap = member == &CrownFinaleTuning::movieFragmentPullTime ? .03f :
            (member == &CrownFinaleTuning::movieCrownTime || member == &CrownFinaleTuning::movieWingsTime ? .012f : 0);
        // 節目の欠落、前の節目からの間隔不足、または全尺以降の節目を検出したら中止します。
        if (!(time > previous + gap && time < 1)) {
            LOG_ERROR("Crown inheritance: invalid required marker {}", name);
            return false;
        }
        previous = time;
    }
    // ここから先は中断時に元へ戻せるよう、操作・姿勢・材質・画角を保存してからロックします。
    s.active = s.prepared = true;
    s.duration = tuning.movieDuration;
    s.offset = {tuning.destinationX, 0.0f, 0.0f};
    s.boss = boss;
    s.bossStart = boss->GetTranslate();
    s.bossScale = boss->GetScale();
    s.bossRotation = boss->GetRotation();
    s.bossModel = boss->GetModelName();
    s.playerYaw = player_->GetMoveYaw();
    s.hadControl = player_->IsControlActive();
    s.fov = camera->GetFovY();
    // 描画補正の設定を取得できる場合は、黒帯の高さも復元用に保存します。
    if (auto* params = PostEffect::GetInstance()->GetParams())
        s.bars = params->cinemaBarHeight;
    player_->BeginCinematicLock();
    s.start = player_->GetTranslate();
    s.playerScale = player_->GetScale();
    s.playerModel = player_->GetModelName();
    s.playerColor = player_->GetColor();
    s.playerMaterial = player_->GetMaterialType();
    std::vector<std::unique_ptr<engine::Object3D>> previewArenaCopies;

    for (const auto& object : GetObjects()) {
        // プレビューでは複製だけを崩壊させ、編集対象のTransformを変更しません。
        if (crownMoviePreview_.active &&
            std::find(crownMoviePreview_.objects.begin(), crownMoviePreview_.objects.end(), object.get()) ==
                crownMoviePreview_.objects.end())
            continue;
        const std::string& name = object->GetName();
        const bool floor = name.find("Stage3_HighCrown_ArenaFloor_") != std::string::npos;
        const bool arena = name == "Stage3_HighCrown_ArenaVisual";
        const bool wall = name.find("Stage3_HighCrown_Wall") != std::string::npos;
        const bool throne = name == "Stage3_HighCrown_Throne";
        const bool reward = name == "Stage3_HighCrown_VictoryCrown";
        const bool foundation = name == "Stage3_HighCrown_Background_Foundation";
        const bool approach =
            name.find("Stage3_HighCrown_Approach") != std::string::npos || name == "Stage3_HighCrown_StairSupport" ||
            name == "Stage3_HighCrown_Background_EntranceCitadel" ||
            name.find("Stage3_HighCrown_Entrance") != std::string::npos ||
            name.find("Stage3_HighCrown_BossGate") != std::string::npos ||
            name.find("Stage3_HighCrown_DecorTower") != std::string::npos || name == "Stage3_PreBoss_CopyMemoryStation";
        // 崩壊・移転・中断復元に関わる配置物だけ、姿勢・衝突・表示の元の値を記録します。
        if (floor || arena || wall || throne || reward || foundation || approach) {
            s.originalObjects.push_back({object.get(), object->GetTranslate(), object->GetRotation(),
                                         object->GetCollisionAttribute(), object->GetCollisionMask(),
                                         object->GetIsVisible()});
            // 対象がボス門なら、演出中の表示と開閉をオーバーライドします。
            if (auto* gate = dynamic_cast<GimmickBossGate*>(object.get()))
                gate->SetCinematicOverride(true);
        }

        // 床崩壊に使う分割モデルを、元の闘技場のワールド座標へ並べて非表示で用意します。
        if (arena) {
            for (std::int32_t i = 0; i < 64; ++i) {
                const std::int32_t x = i / 8, z = i % 8;
                // 8×8に並ぶセルのうち、両端の辺に配置しない8セルを飛ばします。
                if ((x <= 1 || x >= 6) && (z == 0 || z == 7))
                    continue;
                auto chunk = object->Clone();
                // 元モデルの複製に失敗したセルは、破片として登録しません。
                if (!chunk)
                    continue;
                char model[128];
                std::snprintf(model, sizeof(model), "Stages/high_crown/high_crown_fracture_%02d.obj", i);
                chunk->SetModel(model);
                chunk->SetName("CrownFracture_" + std::to_string(i));
                chunk->ClearEditorHiddenReasons();
                chunk->SetEditorInternal(crownMoviePreview_.active);
                chunk->SetEventId(-1);
                chunk->SetTargetId(-1);
                chunk->SetCollisionAttribute(0);
                chunk->SetCollisionMask(0);
                const engine::Vector3 position = engine::Math::Transform({-35.0f + x * 10.0f, 0.0f, 35.0f - z * 10.0f}, object->GetWorldMatrix());
                chunk->SetTranslate(position);
                chunk->SetIsVisible(false);
                chunk->UpdateWorldMatrix(true);
                s.fractureChunks.push_back({chunk.get(), position, chunk->GetRotation(), 0, 0, false});
                s.generatedObjects.push_back(chunk.get());
                previewArenaCopies.push_back(std::move(chunk));
            }
        }

        // 飛行先に移動した後も床に立てるよう、必要な闘技場部品を移動先へ複製します。
        if (floor || arena || foundation) {
            auto copy = object->Clone();
            // 複製できなかった部品は移動先の闘技場に登録しません。
            if (!copy)
                continue;
            copy->SetName("CrownFinalArena_" + name);

            // プレビュー用の闘技場複製は編集用の実体から分け、選択後の改変を抑止します。
            if (crownMoviePreview_.active) {
                copy->SetEditorInternal(true);
                copy->SetIsLocked(true);
            }

            copy->SetEventId(-1);
            copy->SetTargetId(-1);
            copy->SetTranslate(object->GetTranslate() + s.offset);

            // 移動先の闘技場の外観モデルだけは、青白く発光させます。
            if (arena) {
                copy->SetColor({0.72f, 0.84f, 1.0f, 1.0f});
                copy->SetEmissive(1.1f);
            }

            copy->UpdateWorldMatrix(true);
            s.generatedObjects.push_back(copy.get());
            // プレビュー中は走査を終えるまでシーンへの追加を保留します。
            if (crownMoviePreview_.active)
                previewArenaCopies.push_back(std::move(copy));
            // 本編の移動先の部品は通常の追加予約に回します。
            else AddObject(std::move(copy));
        }
    }

    // 走査後に床片とプレビュー用の複製をシーンへ登録します。
    for (auto& copy : previewArenaCopies)
        AddObjectImmediate(std::move(copy));

    // 最後の封印とボス胸元を基準に、欠片の出発点と接地位置を先に計算します。
    s.lastSealPosition = boss->GetLastBrokenCrownSealPosition();
    const auto bossCollider = boss->GetColliderConfig();
    boss->SetModel("Characters/false_king_slime/false_king_slime_without_emblem.obj");
    boss->SetColliderConfig(bossCollider);
    boss->UpdateWorldMatrix(true);
    s.fragmentStart = engine::Math::Transform({0.0f, 3.44f, -1.76f}, boss->GetWorldMatrix());
    const engine::Vector3 towardBoss = s.bossStart - s.start;
    const float planarLength = std::sqrt(towardBoss.x * towardBoss.x + towardBoss.z * towardBoss.z);
    const engine::Vector3 direction = planarLength > 0.01f
                                  ? engine::Vector3{towardBoss.x / planarLength, 0.0f, towardBoss.z / planarLength}
                                  : engine::Vector3{std::sin(s.playerYaw), 0.0f, std::cos(s.playerYaw)};
    const engine::Vector3 sample = s.start + direction * tuning.movieFragmentDistance;
    s.fragmentGround = combatGroundQuery::FindGroundPoint(sample, player_, {8.0f, 24.0f}, s.start.y - 0.75f);
    // 接地時の大きさも見た目キーから取り、床に埋まらない高さへ合わせる。
    const auto resting = engine::SampleCinematicVisual(*fragmentClip,
        tuning.movieFragmentLandTime*s.duration,sequence);
    s.fragmentGround.y += 0.51f * s.bossScale.y * resting.scale.y;
    // 装飾・追従・カメラは同じDirector素材から再生し、ゲーム固有の位置だけ接続する。
    crownFinalePlayer_.SetBindingResolver([this](const CinematicObjectBinding& binding) -> engine::Object3D* {
        // ボス本体またはボスの着地トラックを参照する名前は、今回のボスへ結びます。
        if (binding.targetName == "boss" || binding.targetName == "$Actor" || binding.targetName == "boss_landing") return crownFinale_.boss;
        // プレイヤーの姿勢を変更する各トラック名は現在の操作キャラクターへ結びます。
        if (binding.targetName == "player" || binding.targetName == "$Gate" || binding.targetName == "player_flight" || binding.targetName == "player_absorb" || binding.targetName == "player_anticipation") return player_;
        return CinematicPlayer::ResolveSceneTarget(this, binding);
    });
    crownFinalePlayer_.SetPositionResolver([this](const CinematicObjectBinding& binding)
        -> std::optional<engine::Vector3> {
        const auto& state = crownFinale_;
        // 接地点の参照名には、準備時に地形に合わせて求めた座標を返します。
        if (binding.targetName == "fragment_ground") return state.fragmentGround;
        // 離脱地点の参照名には、欠片が外れる最初の座標を返します。
        if (binding.targetName == "fragment_release") return state.fragmentStart;
        // 付着中の位置を要求され、ボスがいる場合は現在の姿勢から胸元を計算します。
        if (binding.targetName == "fragment_attached" && state.boss)
            return engine::Math::Transform({0.0f,3.44f,-1.76f},state.boss->GetWorldMatrix());
        // 封印を指す参照名には、最後に破った封印の座標を返します。
        if (binding.targetName == "seal_position") return state.lastSealPosition;
        // 役者の開始位置を指す参照名には、ロック時に記録した位置を返します。
        if (binding.targetName == "player_start") return state.start;
        // ボスの開始位置もワールドの書き換え前に記録した値を使います。
        if (binding.targetName == "boss_start") return state.bossStart;
        // 飛行の終点は、最終闘技場へのオフセットにプレイヤー側の着地点を加えます。
        if (binding.targetName == "destination") return state.offset + engine::Vector3{0,.82f,12};
        // 最終闘技場を見せる視点では、移転先の中心より3単位高い点を使います。
        if (binding.targetName == "final_arena_focus") return state.offset + engine::Vector3{0,3,0};
        auto direction = state.fragmentGround - state.start;
        direction.y = 0;
        const float length = engine::Math::Length(direction);
        direction = length > .001f ? direction * (1.0f / length) : engine::Vector3{0,0,1};
        // 口と変身中の視点は、欠片へ向く水平軸に沿って開始位置から離します。
        if (binding.targetName == "mouth") return state.start + direction * .70f + engine::Vector3{0,.22f,0};
        // 変身カットの視点を要求された場合は、欠片の方向へ5.5単位進んだ点に画角用の補正を加えます。
        if (binding.targetName == "transform_eye") return state.start + direction * 5.5f + engine::Vector3{1.6f,2.1f,0};
        return std::nullopt;
    });
    // 再生用オブジェクトの初期化に失敗したら、すでに準備した配置と操作状態を復元します。
    if (!crownFinalePlayer_.Prepare()) {
        ResetCrownFinaleWorld();
        return false;
    }
    s.fragment = crownFinalePlayer_.ResolveBinding({"fragment",-1});
    auto* sealAnchor = crownFinalePlayer_.ResolveBinding({"seal_anchor",-1});
    auto* releaseAnchor = crownFinalePlayer_.ResolveBinding({"release_anchor",-1});
    // 欠片・封印・離脱地点のどれかを解決できなければ、開始前のワールドへ戻します。
    if (!s.fragment || !sealAnchor || !releaseAnchor) {
        ResetCrownFinaleWorld();
        return false;
    }
    // 欠片と演出の基準点を置き、外部時刻を渡してタイムラインを開始します。
    s.fragment->SetTranslate(s.fragmentStart);
    s.fragment->SetScale(s.bossScale);
    s.fragment->SetRotation(s.bossRotation);
    s.fragment->SetIsVisible(true);
    sealAnchor->SetTranslate(s.lastSealPosition);
    releaseAnchor->SetTranslate(s.fragmentStart);
    releaseAnchor->SetScale(s.bossScale);
    crownFinalePlayer_.Play(false, &s.time);
    // 再生を開始できない場合は、役者や生成した地形を元の状態へ戻します。
    if (!crownFinalePlayer_.IsPlaying()) {
        ResetCrownFinaleWorld();
        return false;
    }
    UpdateCrownFinale(0);
    return true;
}

void GamePlayScene::UpdateCrownFinale(float frameDeltaTime, std::optional<float> seekSeconds) {
    auto& s = crownFinale_;
    // 継承演出がアクティブでなければ、役者の姿勢を変更しません。
    if (!s.active)
        return;
    // 編集確認では専用カメラを使う。通常プレイはシーンのメインカメラで続行する。
    engine::Camera* camera = crownMoviePreview_.active ? crownMoviePreview_.camera.get() : CameraManager::GetInstance()->GetMainCamera();

    // 必要な対象を失った場合は、再生を進める前にカメラやシーンの状態を戻す。
    if (!camera || !player_ || !s.boss || player_->IsDead()) {
        ResetCrownFinaleWorld();
        return;
    }
    // 時刻は再生器だけが進める。準備処理が失敗した場合はVFXも評価しない。
    bool prepared = false;
    const auto prepare = [&](float previous, float current) {
        prepared = PrepareCrownFinaleFrame(seekSeconds ? current : previous, current, frameDeltaTime);
        return prepared;
    };
    // 時刻指定はそのフレームの姿勢を確認する経路で、通常再生では経過時間の分だけ進める。
    if (seekSeconds)
        crownFinalePlayer_.SetTime(*seekSeconds, false, prepare);
    // 指定時刻が無い通常再生では、経過時間の分だけ進めます。
    else
        crownFinalePlayer_.Step(frameDeltaTime, prepare);
    // 姿勢の準備に失敗した場合は、以降の描画更新を行いません。
    if (!prepared) return;

    // 再生器の時刻から進行率を求め、王冠の破片を本体に合わせる区間と終了時刻の判定に使う。
    const float t = std::clamp(s.time.Elapsed() / s.duration, 0.0f, 1.0f);
    const float breakTime = GetCrownFinaleTiming().movieFragmentBreakTime;
    // 再生とプレビューのどちらも停止していれば、準備したワールドを元へ戻します。
    if (!crownFinalePlayer_.IsPlaying() && !crownFinalePlayer_.IsPreviewing()) {
        ResetCrownFinaleWorld();
        return;
    }

    // 外れる前だけは本体の変形を継承する。外れた後の拡縮は見た目キーが担当する。
    if (t < breakTime) {
        s.fragment->SetScale(s.boss->GetScale());
        s.fragment->UpdateWorldMatrix(true);
    }

    // 演出側のカメラが確定してから、有効な配置物の描画定数を最終視点へ揃えます。
    for (auto& object:GetObjects())
        // 参照でき、リプレイ上で削除されていない配置物だけを更新します。
        if (object && !object->IsReplayRemoved())
            object->UpdateWorldMatrix(true);
    // フック表示用のマーカーがある場合は、マーカーにも最終視点を反映します。
    if (engine::Object3D* marker=player_->GetHookMarker())
        marker->UpdateWorldMatrix(true);
    // 本編だけ終了を確定する。プレビューは最後のフレームを確認できるように残す。
    if (t>=1 && !crownMoviePreview_.active)
        FinishCrownFinale(true);
}

bool GamePlayScene::PrepareCrownFinaleFrame(float previousTime, float timeSeconds, float frameDeltaTime) {
    auto& s = crownFinale_;
    const auto tuning = GetCrownFinaleTiming();
    // 節目の設定は全尺に対する0〜1の比率。Directorへ姿勢を問い合わせるときは秒を渡す。
    const float t = std::clamp(timeSeconds / s.duration, 0.0f, 1.0f);
    const float rageTime = tuning.movieRageTime, pullTime = tuning.movieFragmentPullTime;
    const float absorbTime = tuning.movieFragmentAbsorbTime, bodyTime = tuning.movieBodyTime;
    const float crownTime = tuning.movieCrownTime, wingsTime = tuning.movieWingsTime;
    const float escapeTime = tuning.movieEscapeTime, arriveTime = tuning.movieArriveTime;
    const float bodyEnd = crownTime - 0.012f, crownEnd = wingsTime - 0.012f;
    const engine::Vector3 destination = s.offset + engine::Vector3{0.0f, 0.82f, 12.0f};
    const float flightYaw = std::atan2(destination.x - s.start.x, destination.z - s.start.z);
    // 前回と今回の秒数の間に節目が入ったかを調べ、フレームが飛んでも一度だけ起こす。
    auto crossed = [&](float when) { return previousTime < when * s.duration && timeSeconds >= when * s.duration; };

    // 本体の震え・膨張・下降は、Directorの区間設定から同じ時刻で評価する。
    // ボスの必須トラックを評価できなければ、中断としてワールドを復元します。
    if (!crownFinalePlayer_.ApplyObjectTransform("boss_landing", timeSeconds)) {
        ResetCrownFinaleWorld();
        return false;
    }
    s.boss->UpdateWorldMatrix(false);
    const engine::Vector3 towardFragment = s.fragmentGround - s.start;
    const float pullYaw = std::atan2(towardFragment.x, towardFragment.z);
    const engine::Vector3 pullDirection{std::sin(pullYaw), 0.0f, std::cos(pullYaw)};
    const engine::Vector3 mouth = s.start + pullDirection * 0.70f + engine::Vector3{0.0f, 0.22f, 0.0f};
    // 腕の先端と描画で同じ時刻の位置を使い、評価順による一フレームの遅れを防ぐ。
    const auto fragmentPosition = crownFinalePlayer_.SampleObjectPosition("fragment", timeSeconds);
    // その時刻の欠片の位置が得られなければ、引き寄せを構成できないため中断します。
    if (!fragmentPosition) {
        ResetCrownFinaleWorld();
        return false;
    }

    // プレイヤーの向きを先に欠片へ寄せ、引き寄せ区間でだけ操作用のフック表示を使います。
    engine::Vector3 playerPosition=s.start;
    const float notice=Easing::SmoothStep01((t-rageTime-.03f)/(pullTime-rageTime-.03f));
    // sinとcosから角度差を±πへ折り返し、欠片を向くまでの最短回転を求める。
    const float turnDelta=std::atan2(std::sin(pullYaw-s.playerYaw),std::cos(pullYaw-s.playerYaw));
    // 引き寄せ開始前までは、開始時の向きから欠片へ最短方向に振り向きます。
    if (t<pullTime)
        player_->SetMoveYaw(s.playerYaw+turnDelta*notice);
    // 引き寄せ開始から吸収完了の直前までは、欠片を向いて腕の演出を進めます。
    if (t>=pullTime && t<absorbTime) {
        const float receive=std::clamp((t-pullTime)/(absorbTime-pullTime),0.0f,1.0f);
        player_->SetMoveYaw(pullYaw);
        player_->StepCinematicPullVisual(frameDeltaTime,towardFragment,receive);

        // マーカーが使える場合は、口から欠片へ素早く伸びる表示を構成します。
        if (engine::Object3D* marker=player_->GetHookMarker()) {
            const float shot=Easing::SmoothStep01((t-pullTime)/.012f);
            const engine::Vector3 tip=CrownLerp(mouth,*fragmentPosition,shot),diff=tip-mouth;
            marker->SetIsVisible(true);
            marker->SetColor({.10f,.82f,1,1});
            // 中心原点のマーカーを口と先端の中点に置き、Z方向の長さをその距離に合わせる。
            marker->SetTranslate(mouth+diff*.5f);
            marker->SetRotation({std::atan2(-diff.y,std::sqrt(diff.x*diff.x+diff.z*diff.z)),std::atan2(diff.x,diff.z),0});
            marker->SetScale({.10f,.10f,std::max(.01f,engine::Math::Length(diff))});
            marker->UpdateWorldMatrix(true);
        }

    }

    // 吸収が完了した境目で引き寄せ表示を閉じ、通常サイズへ戻します。
    if (crossed(absorbTime)) {
        player_->EndCinematicPullVisual();
        player_->SetScale(s.playerScale);
        player_->SetMoveYaw(pullYaw);
    }

    // 吸収から新しい体の形成まで、半透明の材質で吸収トラックを再生します。
    if (t>=absorbTime && t<bodyTime) {
        player_->SetMaterialType(1);
        player_->SetColor({.72f,.95f,1,.48f});
        // 吸収トラックを適用できない場合は演出を中断して元へ戻します。
        if (!crownFinalePlayer_.ApplyObjectTransform("player_absorb", timeSeconds)) {
            ResetCrownFinaleWorld();
            return false;
        }


    }

    // 実Playerの置換面だけを設定する。金色の仮ボディは共通再生器が逆側から形成する。
    if (t>=bodyTime && t<bodyEnd) {
        const float progress = Easing::SmoothStep01((t-bodyTime)/(bodyEnd-bodyTime));
        player_->SetScale(s.playerScale);
        player_->SetMaterialType(25);
        player_->SetColor(s.playerColor);
        engine::Object3D::PortalClipSettings clipSettings;
        clipSettings.enabled = progress < .999f;
        clipSettings.progress = 0;
        // 開始位置を基準に切断面を1.75ワールド単位上げ、元の体を下から順に隠す。
        clipSettings.center = s.start + engine::Vector3{0,-.1f+1.75f*progress+.52f,0};
        clipSettings.normal = {0,-1,0};
        clipSettings.edgeWidth = .035f;
        clipSettings.dissolve = .055f;
        clipSettings.color = {1,.85f,.3f,1};
        player_->SetPortalClip(clipSettings);
    }

    // 形成が終わってから実モデルを交換し、途中のクリップ設定を持ち越しません。
    if (t>=bodyEnd && !s.bodyFormed) {
        s.bodyFormed=true;
        player_->ClearPortalClip();
        player_->SetModel("Characters/slime_crown/slime_crown_body.gltf");
        player_->SetMaterialType(0);
        player_->SetColor({1,.77f,.25f,1});
    }

    // 能力付与は見た目の形成完了と一致させ、初めから装備済みに見えないようにします。
    if (t>=crownEnd && !s.granted) {
        // コピー解除で仮ボディへ戻らないよう、付与の直前に通常形態を復元して保存させる。
        const auto bodyCollider = player_->GetColliderConfig();
        player_->SetModel(s.playerModel);
        player_->SetColliderConfig(bodyCollider);
        player_->SetMaterialType(s.playerMaterial);
        player_->SetColor(s.playerColor);
        s.granted=player_->ApplyStoredCopy("Crown",true);

        // 王冠コピーを適用できない場合は、移転後の戦闘に進まずワールドを戻します。
        if (!s.granted) {
            ResetCrownFinaleWorld();
            return false;
        }
        player_->SetScale(s.playerScale);
        player_->SetMoveYaw(pullYaw);
    }

    // 羽の出現と飛び出しは別の時刻にし、発進前に向きとつぶれを作ります。

    // 羽が形成されて発進時刻より前の間は、飛行方向への回転と飛び出す前の姿勢を適用します。
    if (t>=wingsTime && t<escapeTime) {
        const float turn=Easing::SmoothStep01((t-wingsTime)/(escapeTime-wingsTime));
        player_->SetMoveYaw(pullYaw+std::atan2(std::sin(flightYaw-pullYaw),std::cos(flightYaw-pullYaw))*turn);
        // 発進前の姿勢トラックを評価できない場合は、中断として復元します。
        if (!crownFinalePlayer_.ApplyObjectTransform("player_anticipation", timeSeconds)) {
            ResetCrownFinaleWorld();
            return false;
        }
    }

    // 操作用の向きだけを決め、飛行・着地の姿勢はDirectorの区間設定へ任せる。
    // 発進以降は飛行トラックを使い、到着時刻を境に向きを飛行方向から正面確認用の角度に切り替えます。
    if (t>=escapeTime) {
        player_->SetMoveYaw(t<arriveTime ? flightYaw : std::numbers::pi_v<float>);
        // 飛行トラックを適用できない場合は、移転位置を確定せず中断します。
        if (!crownFinalePlayer_.ApplyObjectTransform("player_flight",timeSeconds)) {
            ResetCrownFinaleWorld();
            return false;
        }
        playerPosition=player_->GetTranslate();

    }
    // 到着時刻以降は着地済みとして記録し、終了処理による効果の重複を抑えます。
    if (t>=arriveTime) {
        // 着地の移動とVFXはDirectorへ任せ、完走時の二重発火を防ぐ状態だけ保持する。
        s.landed=true;

    }

    // 更新中の位置と停止速度を反映してから、足元と周囲の床の演出を進めます。
    player_->SetTranslate(playerPosition);
    player_->SetVelocity({});
    player_->UpdateWorldMatrix(false);
    // 表示メッシュを実際に分割した床片です。外周から割れ、最後に足元が抜けます。

    for (auto& pose:s.fractureChunks) {
        const engine::Vector3 delta=pose.position-s.start;
        const float distance=std::sqrt(delta.x*delta.x+delta.z*delta.z);
        // 外周ほど遅延率が0に近づく。プレイヤーの足元の床だけは発進時刻の後まで支える。
        const float delay=1-std::clamp(distance/48.0f,0.0f,1.0f);
        const bool supportsPlayer=std::abs(delta.x)<=5.1f && std::abs(delta.z)<=5.1f;
        const float begin=supportsPlayer?escapeTime+.004f:rageTime+.02f+delay*(escapeTime-rageTime-.01f);
        // 崩壊の開始からの実秒数で落下させる。-4.9は初速0の重力落下における加速度の半分。
        const float age=std::max(0.0f,(t-begin)*s.duration);
        pose.object->SetIsVisible(t>=rageTime && age<4.0f);
        pose.object->SetTranslate(pose.position+engine::Vector3{std::sin(age*2+pose.position.x)*age*.6f,-4.9f*age*age,0});
        pose.object->SetRotation(pose.rotation+engine::Vector3{age*.18f,age*.04f,age*.13f});
        pose.object->UpdateWorldMatrix(false);

        // 床片が崩れ始めた時刻を跨ぎ、プレイヤーから10単位より遠い場合だけ破砕VFXを出します。
        if (crossed(begin) && distance>10)
            VfxSequencer::PlayOneShot("enemy/false_king/crown_inheritance_floor_break_cue",pose.position+engine::Vector3{0,.8f,0});
    }

    // 親子を両方動かすと子だけ落下量が二重になるため、親が演出対象なら子の姿勢は親に追従させます。
    std::unordered_set<engine::Object3D*> animatedObjects;
    animatedObjects.reserve(s.originalObjects.size());
    for (const auto& pose : s.originalObjects)
        animatedObjects.insert(pose.object);

    size_t index=0;

    for (auto& pose:s.originalObjects) {
        // 親も崩落対象になっている子は自分の落下を適用せず、親の姿勢に追従します。
        if (auto* parent = pose.object->GetParent(); parent && animatedObjects.find(parent) != animatedObjects.end()) {
            // 崩壊開始以降は、親に追従する子も衝突対象から外します。
            if (t >= rageTime) {
                pose.object->SetCollisionAttribute(0);
                pose.object->SetCollisionMask(0);
            }
            continue;
        }
        const std::string& name=pose.object->GetName();
        // 勝利報酬は崩壊させず、完走時に FinishCrownFinale() が移動します。
        if (name=="Stage3_HighCrown_VictoryCrown")
            continue;
        // 分割前の闘技場の外観は崩壊開始時に隠し、以降は床片だけを描画します。
        if (name=="Stage3_HighCrown_ArenaVisual") {
            pose.object->SetIsVisible(t<rageTime);
            continue;
        }
        const bool floor=name.find("ArenaFloor_")!=std::string::npos;
        const float begin=floor?escapeTime+.005f:rageTime+.02f+(index++%9)*.012f;
        const float age=std::max(0.0f,(t-begin)*s.duration);
        pose.object->SetTranslate(pose.position+engine::Vector3{0,-4.9f*age*age,0});
        pose.object->SetRotation(pose.rotation+engine::Vector3{age*.07f,0,age*.055f});

        // 床や壁が動き出したフレームで衝突を外し、落ちる描画だけが残るようにする。
        if (age>0) {
            pose.object->SetCollisionAttribute(0);
            pose.object->SetCollisionMask(0);
        }
        pose.object->UpdateWorldMatrix(false);
    }

    return true;
}

void GamePlayScene::FinishCrownFinale(bool completed) {
    // 結果に応じて演出終了後のボス・Player状態を確定する。
    auto& s = crownFinale_;
    // アクティブな演出が無い場合は、中断や完走の後処理を重ねません。
    if (!s.active)
        return;
    // トラックの変更を元に戻してから、完走・中断それぞれの位置と操作状態を上書きする。
    crownFinalePlayer_.Stop(true);
    s.active = false;
    // 描画補正の設定が利用できる場合、演出前の黒帯の高さを復元します。
    if (auto* params = PostEffect::GetInstance()->GetParams())
        params->cinemaBarHeight = s.bars;
    // プレイヤーが残っている場合は演出ロックを解除し、完走または中断の位置に置きます。
    if (player_) {
        player_->EndCinematicLock(s.hadControl);
        // 中断時は元位置、完走時は最終戦の開始地点へ移し、重複した着地を防ぐ。
        // 中断時は開始前の向きも復元します。
        if (!completed)
            player_->SetMoveYaw(s.playerYaw);
        player_->SetScale(s.playerScale);
        auto rotation = player_->GetRotation();
        rotation.x = rotation.z = 0.0f;
        player_->SetRotation(rotation);
        player_->SetTranslate(completed ? s.offset + engine::Vector3{0.0f, 0.82f, 12.0f} : s.start);
        player_->SetVelocity({});
        player_->SetGrounded(true);
        player_->UpdateWorldMatrix(true);

        // 完走後は2.5秒の無敵を付け、Directorが着地を処理済みなら着地の粒子を再生済みとして扱う。
        if (completed) {
            player_->StartEvasionInvincibility(2.5f);
            // タイムライン上でまだ着地扱いになっていなければ、着地の効果をここで一度再生します。
            if (!s.landed)
                VfxSequencer::PlayOneShot("shared/crown_victory_land_cue", player_->GetWorldPosition());
        }
    }

    // 完走時にだけボスと勝利報酬を最終闘技場へ移し、戦闘を再開できるようにします。
    if (completed && s.boss) {
        s.boss->BeginFinalArena(s.offset + engine::Vector3{0.0f, 0.71f, -12.0f});

        for (auto& pose : s.originalObjects) {
            // 勝利報酬の王冠だけを元の配置から最終闘技場へ移します。
            if (pose.object->GetName() == "Stage3_HighCrown_VictoryCrown") {
                // イベント受信型なら、次の更新で元の位置へ戻らないようランタイムの基準位置も移します。
                if (auto* receiver = dynamic_cast<GimmickEventReceiver*>(pose.object))
                    receiver->RelocateRuntimeBase(pose.position + s.offset);
                pose.object->SetTranslate(pose.position + s.offset);
                pose.object->UpdateWorldMatrix(true);
            }
        }
    }

    // 完走せずボスが残っている場合は、元のモデルと姿勢へ戻します。
    if (!completed && s.boss) {
        // モデル差し替えの前後で衝突形状を引き継ぎます。
        const auto collider = s.boss->GetColliderConfig();
        s.boss->SetModel(s.bossModel);
        s.boss->SetColliderConfig(collider);
        s.boss->SetScale(s.bossScale);
        s.boss->SetRotation(s.bossRotation);
        s.boss->SetTranslate(s.bossStart);
        // 本編を中断したときだけ、ボスの戦闘進行にも中断を通知します。
        if (!crownMoviePreview_.active)
            s.boss->AbortCrownFinale();
    }

    // プレイヤーが残っている場合は、引き寄せ表示と体の切断面を片付けます。
    if (player_) {
        player_->EndCinematicPullVisual();
        player_->ClearPortalClip();

        // 中断時は付与された王冠能力を解除してから、吸収前の見た目で元の体へ戻す。
        if (!completed) {
            player_->ClearCrownPower();
            player_->SetModel(s.playerModel);
            player_->SetMaterialType(s.playerMaterial);
            player_->SetColor(s.playerColor);
        }
    }

    // 演出用画角と固定視点から、着地後の通常追従へ戻します。
    if (engine::Camera* camera = CameraManager::GetInstance()->GetMainCamera()) {
        camera->SetFovY(s.fov);
        camera->SetFollowTarget(player_);
        camera->SetLockOnTarget(nullptr);
        camera->SetFollowMode(engine::Camera::FollowMode::kAimable);
        camera->SetRotation({0.24f, std::numbers::pi_v<float>, 0.0f});
        camera->SnapToThirdPerson(18.0f, 2.5f, 0.24f);
        camera->SetInputEnabled(true);
    }

    CameraManager::GetInstance()->SetActiveCamera(nullptr);
    // 装飾ObjectはStopで共通再生器が破棄する。借用参照だけを外す。
    s.fragment = nullptr;
}

void GamePlayScene::ResetCrownFinaleWorld() {
    pendingCrownFinaleBoss_ = nullptr;
    // プレビューや中断後に生成した演出Objectを片付け、元の世界へ戻す。
    auto& s = crownFinale_;
    // 演出の準備まで進んでいなければ、復元対象や生成物はありません。
    if (!s.prepared)
        return;
    FinishCrownFinale(false);

    // ボスと保存済みのモデル名がそろっていれば、外観を再度開始前のモデルに揃えます。
    if (s.boss && !s.bossModel.empty()) {
        const auto collider = s.boss->GetColliderConfig();
        s.boss->SetModel(s.bossModel);
        s.boss->SetColliderConfig(collider);
    }

    // プレイヤーが残っていれば、プレビューまたは中断で得た王冠能力を取り除きます。
    if (player_)
        player_->ClearCrownPower();
    // 最初に記録した配置へ戻します。イベント受信側の基準位置も戻し、次の更新で復元が打ち消されないようにします。
    for (auto& pose : s.originalObjects) {
        // 実体が無い配置物の姿勢や設定は復元しません。
        if (!pose.object)
            continue;
        // ボス門なら演出中だけのオーバーライドを解除します。
        if (auto* gate = dynamic_cast<GimmickBossGate*>(pose.object))
            gate->SetCinematicOverride(false);
        // イベント受信型なら、更新時の基準座標も演出前へ戻します。
        if (auto* receiver = dynamic_cast<GimmickEventReceiver*>(pose.object))
            receiver->RelocateRuntimeBase(pose.position);
        pose.object->SetTranslate(pose.position);
        pose.object->SetRotation(pose.rotation);
        pose.object->SetCollisionAttribute(pose.attribute);
        pose.object->SetCollisionMask(pose.mask);
        pose.object->SetIsVisible(pose.visible);
        pose.object->UpdateWorldMatrix(true);
    }

    // 元の配置を復元した後に、演出で追加した床片と闘技場を削除対象へ移します。
    for (auto* object : s.generatedObjects) {
        // プレビュー中は削除予約が反映される前に、描画とリプレイ上の対象から外します。
        if (crownMoviePreview_.active) {
            object->SetIsVisible(false);
            object->SetReplayRemoved(true);
        }

        RequestRemoveObject(object);
    }

    s = CrownFinaleState{};
}

void GamePlayScene::DrawCrownFinaleWorld(const engine::SceneLighting& lighting) {
    // Drawで取得した共有ライトを渡し、直前の描画が残したGPU状態に依存させない。
    // 継承演出がアクティブな場合だけ、タイムラインの専用オブジェクトを描きます。
    if (crownFinale_.active)
        lighting.Draw(crownFinalePlayer_, &engine::CinematicPlayer::DrawPresentation);
    // 障壁カット用の弾があり、表示区間に入っている場合だけ描きます。
    if (const auto& shot = sealBarrageMovie_.shieldDemoShot; shot && shot->GetIsVisible())
        lighting.Draw(*shot, &engine::Object3D::Draw);
}

void GamePlayScene::DrawCrownFinaleUi() {
    // ゲーム操作と演出の案内が不要な場面、またはポーズ中は描画を省きます。
    if (!player_ || (!SceneManager::GetInstance()->IsPlaying() && !crownMoviePreview_.active) || isGoal_ ||
        (controlsGuideOverlay_ && controlsGuideOverlay_->IsActive()) ||
        (pauseMenuOverlay_ && pauseMenuOverlay_->IsActive()))
        return;
    auto* boss = crownMoviePreview_.active ? crownFinale_.boss : FindFalseKingBossForHUD();
    // 該当ボスが無いか、継承演出も戦闘HUDも無効なら、案内を描きません。
    if (!boss || (!crownFinale_.active && !boss->IsEncounterHudActive()))
        return;
    // 封印のルールは召喚カットで見せ、戦闘中は王冠能力の案内を出します。
    std::int32_t index = boss->GetBattlePhase() == 3 ? 2 : -1;
    float alpha = 1.0f;

    // 登場では名前、継承では時刻ごとの指示、それ以外は現在の形態に必要な案内を選びます。
    if (arenaBossIntroActive_) {
        index = 3;
        alpha = Easing::SmoothStep01((arenaBossIntroTimer_ - arenaBossIntroMessageInTime_) / 0.35f) *
                (1.0f - Easing::SmoothStep01((arenaBossIntroTimer_ - arenaBossIntroMessageOutTime_) / 0.35f));
    // 継承演出中は、引き寄せ・吸収の時刻に応じて表示する指示を切り替えます。
    } else if (crownFinale_.active) {
        const float t = crownFinale_.time.Elapsed() / crownFinale_.duration;
        const auto tuning = GetCrownFinaleTiming();
        // 案内ごとに画像と表示区間を選ぶ。開始と終了はムービーの全尺に対する割合。
        float begin;
        float end;
        if (t < tuning.movieFragmentPullTime) {
            index = 4;
            begin = 0.0f;
            end = tuning.movieFragmentPullTime;
        } else if (t < tuning.movieFragmentAbsorbTime) {
            index = 1;
            begin = tuning.movieFragmentPullTime;
            end = tuning.movieFragmentAbsorbTime;
        } else {
            index = 5;
            begin = tuning.movieFragmentAbsorbTime;
            end = tuning.movieEscapeTime;
        }
        alpha = Easing::SmoothStep01((t - begin) / 0.025f) * (1.0f - Easing::SmoothStep01((t - end + 0.025f) / 0.025f));
    }

    // 現在の戦闘段階や演出に対応する案内が無ければ描きません。
    if (index < 0)
        return;
    // フェードで透明度が閾値以下になったフレームは描画を省きます。
    if (alpha <= 0.001f)
        return;
    // 表示対象が決まった時点で画像を用意し、次のフレームから同じSpriteを使います。
    if (!crownFinaleMessages_[index]) {
        const char* paths[] = {"ui/crown_finale/seals.png",    "ui/crown_finale/inherit.png",
                               "ui/control_guide/book/crown_gate/controls.png", "ui/crown_finale/boss_intro_title.png",
                               "ui/crown_finale/release.png",  "ui/crown_finale/awaken.png"};
        crownFinaleMessages_[index] = std::make_unique<engine::Sprite>();
        const uint32_t texture = engine::Sprite::LoadTexture(paths[index]);
        crownFinaleMessages_[index]->Initialize(GetSpriteCommon(), texture);
        crownFinaleMessages_[index]->SetTextureName(paths[index]);
        crownFinaleMessages_[index]->SetAnchorPoint({0.5f, 0.5f});
    }

    auto& message = crownFinaleMessages_[index];
    message->SetColor({1.0f, 1.0f, 1.0f, alpha});
    const bool movie = crownFinale_.active || arenaBossIntroActive_;

    // 透過文字の実寸比率を維持し、通常時は左下のHPと中央のプレイヤーを避けます。
    const auto& metadata = engine::TextureManager::GetInstance()->GetMetadata(message->GetTextureHandle());
    const auto& introStyle = BossIntroSettings::Get();
    const float titlePop = arenaBossIntroActive_ ? std::lerp(1.12f,1.0f,Easing::SmoothStep01((arenaBossIntroTimer_-arenaBossIntroMessageInTime_)/.18f)) : 1.0f;
    // ボス登場を優先して表示枠とY座標を決める。X座標はムービー中かどうかで決める。
    float maxWidth = 820.0f;
    float maxHeight = 64.0f;
    float titleY = 662.0f;
    if (arenaBossIntroActive_) {
        maxWidth = introStyle.titleWidth;
        maxHeight = 190.0f;
        titleY = introStyle.titleY;
    } else if (movie) {
        maxWidth = 1020.0f;
        maxHeight = 72.0f;
        titleY = 620.0f;
    }

    // 幅と高さそれぞれの最大倍率の小さい方を使い、文字画像を縦横比を保ったまま表示範囲に収める。
    const float fit = titlePop * std::min(maxWidth / std::max(1.0f, static_cast<float>(metadata.width)),
                                         maxHeight / std::max(1.0f, static_cast<float>(metadata.height)));
    message->SetPosition(spriteLayoutScaler::ScaleDesignPosition({movie ? 640.0f : 830.0f, titleY}, {1280.0f, 720.0f}));
    message->SetSize(
        spriteLayoutScaler::ScaleDesignSize({metadata.width * fit, metadata.height * fit}, {1280.0f, 720.0f}));
    message->Update();
    message->Draw();
}
