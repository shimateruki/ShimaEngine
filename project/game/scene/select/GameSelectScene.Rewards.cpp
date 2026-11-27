#define NOMINMAX
#include <cstdint>
#include "game/scene/select/GameSelectScene.h"
#include "ParticleSystem.h"
#include "DebugConsole.h"
#include "game/system/progress/StageManager.h"
#include "GpuParticleManager.h"
#include "ParticleSystem.h"
#include "MeshEffectManager.h"
#include "game/system/settings/GameAudioSettings.h"
#include "CollisionConfig.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <sstream>
#include "game/scene/select/GameSelectScene.Internal.h"
using namespace stageSelectDetail;

// 島の装飾と王冠報酬の表示。進行データはGameDataManager、表示状態はシーンが所有する。
void GameSelectScene::UpdateStageSelectDecorations(float deltaTime) {
    // 島は常に地形として残し、解放状態の差をゲート・橋・台座の発光で示す。
    const auto& stages = StageManager::GetInstance()->GetStages();
    float pulse = (std::sin(stageSelectTime_ * 5.0f) + 1.0f) * 0.5f;

    engine::Object3D* crown = FindObjectByName("StageSelect_CrownCore");

    // セレクトに入口があるステージ1～3を全てクリアした時だけ、中央に王冠を飾る。
    const auto* save = GameDataManager::GetInstance();
    const bool showCentralCrown = save && save->IsStageCleared(0) &&
        save->IsStageCleared(1) && save->IsStageCleared(2);
    // 中央の王冠がシーンに配置されている場合に、三つのクリア記録から可視性を決める。
    if (crown) {
        crown->SetIsVisible(showCentralCrown);
    }

    // 中央の王冠を表示中で、別の解放演出がないときだけ、通常の回転と発光を進める。
    if (crown && showCentralCrown && unlockingStageIndex_ < 0) {
        crown->SetEulerY(crown->GetRotation().y + deltaTime * 0.55f);
        crown->SetQuaternionMaster(false);
        crown->SetColor({1.0f, 0.72f, 0.18f, 1.0f});
        crown->SetEmissive(1.5f + pulse * 0.45f);
    }

    for (std::int32_t stageIndex = 0; stageIndex < static_cast<std::int32_t>(stages.size()); ++stageIndex) {
        bool unlocked = IsStageUnlocked(stageIndex);
        bool unlocking = stageIndex == unlockingStageIndex_;
        bool selected = stageIndex == selectedStageIndex_;

        engine::Object3D* island = FindObjectByName("StageIsland_" + std::to_string(stageIndex));

        // 対象の島モデルが存在する場合は、解放にかかわらず地形として有効化する。
        if (island) {
            island->SetIsVisible(true);

            // ロック中も島そのものは地形として残し、ゲートだけで進行を制限する。
            // 地形まで無効化すると、未解放の島へ移動した際にプレイヤーが落下してしまう。
            island->SetCollisionAttribute(engine::collisionCategories::Get("ground"));
            island->SetCollisionMask(kStageSelectSolidMask);

            // 新しいPBR地形は日照色を維持する。解放・選択表示はゲートと台座が担当する。
            const bool gardenTerrain = island->GetMaterialType() == 34;
            island->SetColor(gardenTerrain ? engine::Vector4{1, 1, 1, 1}
                                           : GetStageIslandColor(stageIndex, unlocked, selected, unlocking, pulse));
            island->SetEmissive(gardenTerrain ? 1.0f : GetStageIslandEmissive(unlocked, selected, unlocking, pulse));
        }

        // 楕円の輪郭に沿う短冊Colliderを重ね、外周や短冊間の隙間から落下しないようにする。
        for (std::int32_t stripIndex = 0; stripIndex < kStageIslandCollisionStripCount; ++stripIndex) {
            engine::Object3D* islandCollision = FindObjectByName("StageIslandCollision_" + std::to_string(stageIndex) + "_" +
                                                         std::to_string(stripIndex));
            // 配置されていない短冊は飛ばし、存在するものだけ描画を隠して床判定を残す。
            if (!islandCollision) {
                continue;
            }

            islandCollision->SetIsVisible(false);
            islandCollision->SetCollisionAttribute(engine::collisionCategories::Get("ground"));
            islandCollision->SetCollisionMask(kStageSelectSolidMask);
        }

        // 旧形式のSceneを開いた場合も、従来の十字Colliderを地形として維持する。
        for (const char* suffix : {"_X", "_Z"}) {
            engine::Object3D* legacyCollision = FindObjectByName("StageIslandCollision_" + std::to_string(stageIndex) + suffix);

            // 旧形式の Collider を見つけた場合は、新しい短冊と同様に判定専用で使う。
            if (legacyCollision) {
                legacyCollision->SetIsVisible(false);
                legacyCollision->SetCollisionAttribute(engine::collisionCategories::Get("ground"));
                legacyCollision->SetCollisionMask(kStageSelectSolidMask);
            }
        }

        // 橋は状態によらず足場として残し、解放の有無は色と発光だけで知らせます。
        engine::Object3D* bridge = FindObjectByName("StageSelect_Bridge_" + std::to_string(stageIndex));

        // 橋モデルがある場合は足場として有効にし、進行状態で見た目を分ける。
        if (bridge) {
            bridge->SetIsVisible(true);
            bridge->SetCollisionAttribute(engine::collisionCategories::Get("ground"));
            bridge->SetCollisionMask(kStageSelectSolidMask);

            // 解放演出中の橋は、金色の発光を周期的に強める。
            if (unlocking) {
                bridge->SetColor({1.0f, 0.78f + pulse * 0.16f, 0.28f, 1.0f});
                bridge->SetEmissive(1.55f + pulse * 0.75f);
            // 解放済みで演出中でなければ、通常の橋の色と明るさに戻す。
            } else if (unlocked) {
                bridge->SetColor({1.0f, 1.0f, 1.0f, 1.0f});
                bridge->SetEmissive(1.0f);
            // 未解放の橋は通行判定を保ったまま、暗い外観にする。
            } else {
                bridge->SetColor({0.32f, 0.34f, 0.38f, 1.0f});
                bridge->SetEmissive(0.22f);
            }
        }

        // 台座は解放中と選択中を明るくし、ゲートのある島を見つける手がかりにします。
        engine::Object3D* markerPad = FindObjectByName("StageMarkerPad_" + std::to_string(stageIndex));

        // 台座が置かれている場合だけ、島の目印として表示を更新する。
        if (markerPad) {
            markerPad->SetIsVisible(true);

            // 解放中の台座は、解放済みの通常色に先立って金色の点滅を表示する。
            if (unlocking) {
                markerPad->SetColor({1.0f, 0.82f + pulse * 0.12f, 0.28f, 1.0f});
                markerPad->SetEmissive(2.4f + pulse * 1.1f);
            // 解放後の台座では、選択中の島を淡く明るく点滅させる。
            } else if (unlocked) {
                markerPad->SetColor(selected ? engine::Vector4{0.86f, 0.92f, 1.0f, 1.0f} : engine::Vector4{0.45f, 0.66f, 0.88f, 1.0f});
                markerPad->SetEmissive(selected ? 1.8f + pulse * 0.45f : 0.95f);
            // 未解放の台座を弱い発光にし、解放先と区別する。
            } else {
                markerPad->SetColor({0.22f, 0.24f, 0.30f, 1.0f});
                markerPad->SetEmissive(0.18f);
            }
        }

        // 旗も台座と同じ進行条件で発光させ、遠くから島の状態を判別できるようにします。
        engine::Object3D* flag = FindObjectByName("StageFlag_" + std::to_string(stageIndex));

        // 旗が置かれている場合にだけ、進行状態に応じた目印として表示する。
        if (flag) {
            flag->SetIsVisible(true);

            // 解放演出中の旗は金色として強調する。
            if (unlocking) {
                flag->SetColor({1.0f, 0.88f, 0.36f, 1.0f});
                flag->SetEmissive(2.0f + pulse * 1.0f);
            // 解放済みの旗は、選択中の島だけ色と発光を強める。
            } else if (unlocked) {
                flag->SetColor(selected ? engine::Vector4{0.95f, 1.0f, 1.0f, 1.0f} : engine::Vector4{0.74f, 0.90f, 1.0f, 1.0f});
                flag->SetEmissive(selected ? 1.65f + pulse * 0.35f : 1.05f);
            // 未解放の旗は低い発光にする。
            } else {
                flag->SetColor({0.32f, 0.36f, 0.46f, 1.0f});
                flag->SetEmissive(0.2f);
            }
        }

        UpdatePathDisplay(stageIndex, unlocked, unlocking, pulse);
    }

    UpdateStarCoinDisplays(deltaTime);
    UpdateStageClearCrownDisplays(deltaTime);
}

// 各短い経路について、解放中の見た目と、通行できるかどうかを別々に更新する。
void GameSelectScene::UpdatePathDisplay(std::int32_t stageIndex, bool active, bool unlocking, float pulse) {
    // 未解放の経路は見た目だけでなく接触も無効化して進行を制限する。
    for (std::int32_t segmentIndex = 0; segmentIndex < 12; ++segmentIndex) {
        std::ostringstream name;
        name << "StagePath_" << stageIndex << "_" << segmentIndex;
        engine::Object3D* path = FindObjectByName(name.str());

        // 該当する短い経路が配置されていなければ、次の番号を確認する。
        if (!path) {
            continue;
        }

        // 解放演出の途中は先に見た目だけ表示し、通行が解放されるまでは接触を無効にします。
        bool visible = active || unlocking;
        path->SetIsVisible(visible);
        path->SetCollisionAttribute(active ? engine::collisionCategories::Get("ground") : 0);
        path->SetCollisionMask(active ? kStageSelectSolidMask : 0);

        // 解放演出中は、経路を金色で点灯させる。
        if (unlocking) {
            path->SetColor({1.0f, 0.78f + pulse * 0.16f, 0.28f, 1.0f});
            path->SetEmissive(2.2f + pulse * 1.4f);
        // 通行可能で演出が終わった経路は、通常の青系の外観へ戻す。
        } else if (active) {
            path->SetColor({0.66f, 0.84f, 1.0f, 1.0f});
            path->SetEmissive(1.05f);
        // 未解放で演出もない経路は非表示のまま、低い発光と暗い色を設定する。
        } else {
            path->SetColor({0.12f, 0.14f, 0.18f, 0.35f});
            path->SetEmissive(0.1f);
        }
    }
}

void GameSelectScene::UpdateStarCoinDisplays(float deltaTime) {
    // ステージごとの取得記録を、島上の三つのスター表示へ反映する。
    (void)deltaTime;
    const auto& stages = StageManager::GetInstance()->GetStages();
    auto* save = GameDataManager::GetInstance();

    for (std::int32_t stageIndex = 0; stageIndex < static_cast<std::int32_t>(stages.size()); ++stageIndex) {
        // ボス専用ステージには収集枠そのものを表示しない。
        bool unlocked = IsStageUnlocked(stageIndex) && stages[stageIndex].hasStarCoins;

        for (std::int32_t coinIndex = 0; coinIndex < 3; ++coinIndex) {
            std::ostringstream name;
            name << "StageCoin_" << stageIndex << "_" << coinIndex;
            engine::Object3D* coin = FindObjectByName(name.str());

            // レイアウトに対応するスター表示がない場合は、その枠だけ飛ばす。
            if (!coin) {
                continue;
            }

            // 収集状況を示す置物なので、表示中も拾えるコインとしては扱いません。
            coin->SetIsVisible(unlocked);
            coin->SetCollisionAttribute(0);
            coin->SetCollisionMask(0);

            // ロック中または収集枠のないステージでは、非表示にした後の色更新は行わない。
            if (!unlocked) {
                continue;
            }

            // 取得している枠は金色にして光らせる。
            if (save->IsStarCoinCollected(stageIndex, coinIndex)) {
                coin->SetColor({1.0f, 0.85f, 0.12f, 1.0f});
                coin->SetEmissive(2.2f);
            // 未取得の表示枠は暗くして、まだ獲得していないことを示す。
            } else {
                coin->SetColor({0.32f, 0.34f, 0.38f, 0.78f});
                coin->SetEmissive(0.35f);
            }
        }
    }
}

engine::Object3D* GameSelectScene::EnsureStageClearCrown(std::int32_t stageIndex) {
    // 保存レイアウトに王冠がない場合だけ演出用モデルを生成し、重複を避ける。
    if (!objectManager_ || !GetObject3DCommon()) {
        return nullptr;
    }

    const std::string name = MakeStageClearCrownName(stageIndex);

    // 元のシーンに配置済みの王冠があれば、現在の実体を再利用する。
    if (engine::Object3D* existing = FindObjectByName(name)) {
        return existing;
    }

    // 保存レイアウトに無い場合だけ、接触しない表示専用の王冠を作ります。
    auto crown = std::make_unique<engine::Object3D>();
    crown->Initialize(GetObject3DCommon());
    crown->SetModel(kStageClearCrownModel);
    crown->SetName(name);
    crown->SetClassName("EditorOnly");
    crown->SetSaveCategory("Object");
    crown->SetIsLocked(true);
    crown->SetTargetId(stageIndex);
    crown->SetColliderType(engine::ColliderType::kNone);
    crown->SetCollisionAttribute(0);
    crown->SetCollisionMask(0);
    crown->SetScale({kStageClearCrownBaseScale, kStageClearCrownBaseScale, kStageClearCrownBaseScale});

    crown->SetEnableLighting(false);
    crown->SetColor({1.18f, 1.10f, 0.94f, 1.0f});
    crown->SetEmissive(2.0f);
    crown->SetGpuParticleName(kCrownIdleParticlePreset);
    crown->SetMeshEffect1Name("");
    crown->SetMeshEffect2Name("");
    crown->SetTranslate(GetStageClearCrownPosition(stageIndex));
    crown->UpdateLocalMatrix();
    crown->UpdateWorldMatrix();

    // 即時登録してこのフレームから演出を更新できるよう、所有先をシーンへ渡します。
    engine::Object3D* raw = crown.get();
    AddObjectImmediate(std::move(crown));
    return raw;
}

void GameSelectScene::UpdateStageClearCrownDisplays(float deltaTime) {
    // 通常時は回転・上下動、クリア直後は専用の落下演出へ切り替える。
    const auto& stages = StageManager::GetInstance()->GetStages();
    auto* save = GameDataManager::GetInstance();
    const float pulse = (std::sin(stageSelectTime_ * 4.2f) + 1.0f) * 0.5f;

    for (std::int32_t stageIndex = 0; stageIndex < static_cast<std::int32_t>(stages.size()); ++stageIndex) {
        engine::Object3D* crown = EnsureStageClearCrown(stageIndex);

        // 王冠が見つからず生成もできなければ、そのステージの表示を更新しない。
        if (!crown) {
            continue;
        }

        const bool cleared = save->IsStageCleared(stageIndex);
        crown->SetEnableLighting(false);
        crown->SetIsVisible(cleared);
        crown->SetCollisionAttribute(0);
        crown->SetCollisionMask(0);

        // 未クリアの王冠は非表示と接触無効まで設定し、姿勢や演出の更新を省く。
        if (!cleared) {
            continue;
        }

        // 報酬対象の王冠だけ通常の浮遊から落下へ切り替えます。
        if (crownCountPresentationActive_ && stageIndex == crownCountPresentationStageIndex_) {
            UpdateStageClearRewardCrown(crown, stageIndex, deltaTime);
            continue;
        }

        // 報酬対象以外はゲート上の定位置を基準に、島ごとに位相の異なる揺れを付けます。
        const float bob = std::sin(stageSelectTime_ * 2.6f + static_cast<float>(stageIndex) * 0.75f) * 0.10f;
        engine::Vector3 targetPos = GetStageClearCrownPosition(stageIndex);
        targetPos.y += bob;
        crown->SetTranslate(targetPos);
        crown->SetEulerY(crown->GetRotation().y + deltaTime * 1.15f);
        crown->SetEulerZ(std::sin(stageSelectTime_ * 2.0f + static_cast<float>(stageIndex)) * 0.08f);
        crown->SetQuaternionMaster(false);

        const float scale = kStageClearCrownBaseScale * (1.0f + pulse * 0.08f);
        crown->SetScale({scale, scale, scale});
        crown->SetColor({1.18f, 1.06f + pulse * 0.08f, 0.90f + pulse * 0.08f, 1.0f});
        crown->SetEmissive(1.9f + pulse * 0.65f);
        crown->SetGpuParticleName(kCrownIdleParticlePreset);
        crown->SetMeshEffect1Name("");
        crown->SetMeshEffect2Name("");
        crown->UpdateLocalMatrix();
        crown->UpdateWorldMatrix();
    }
}

// クリア後の王冠数変化を表示対象ステージに固定して演出を開始します。
void GameSelectScene::StartStageClearRewardPresentation(const GameDataManager::StageClearRewardPresentation& request) {
    // 保存済みの報酬要求が有効なステージを指す場合だけ演出を開始する。
    const auto& stages = StageManager::GetInstance()->GetStages();

    // 無効な要求や範囲外のステージには、報酬表示や選択の変更を始めない。
    if (!request.active || request.stageIndex < 0 || request.stageIndex >= static_cast<std::int32_t>(stages.size())) {
        return;
    }

    crownCountPresentationActive_ = true;
    crownCountPresentationImpactDone_ = false;
    crownCountPresentationStageIndex_ = request.stageIndex;
    crownCountPresentationFrom_ = std::clamp(request.previousCrownCount, 0, 999);
    crownCountPresentationTo_ = std::clamp(request.newCrownCount, 0, 999);
    crownCountPresentationTimer_ = 0.0f;
    crownCountPresentationParticleTimer_ = 0.0f;
    selectedStageIndex_ = crownCountPresentationStageIndex_;

    DebugConsole::GetInstance()->AddLog("Stage Select: crown count presentation start.");
}

// 王冠の着地を境に粒子と音を切り替え、終了後に次のステージ解放演出へつなぎます。
void GameSelectScene::UpdateStageClearRewardPresentation(float deltaTime) {
    // 王冠数の表示を進め、演出終了まで通常の選択表示との重なりを管理する。
    if (!crownCountPresentationActive_) {
        return;
    }

    crownCountPresentationTimer_ += deltaTime;
    crownCountPresentationParticleTimer_ -= deltaTime;
    selectedStageIndex_ = crownCountPresentationStageIndex_;

    engine::Object3D* crown = EnsureStageClearCrown(crownCountPresentationStageIndex_);
    const engine::Vector3 targetPos = GetStageClearCrownPosition(crownCountPresentationStageIndex_);
    const engine::Vector3 crownPos = crown ? crown->GetWorldPosition() : targetPos;
    const engine::Vector3 burstPos =
        (crownCountPresentationTimer_ < kCrownCountImpactTime ? crownPos : targetPos) + engine::Vector3{0.0f, 1.05f, 0.0f};

    // 着地前は移動中の王冠、着地後は据え付け位置から一定間隔で粒子を出します。
    if (crownCountPresentationParticleTimer_ <= 0.0f) {
        crownCountPresentationParticleTimer_ = kCrownCountParticleInterval;
        GpuParticleManager::GetInstance()->Emit(kCrownUnlockFountainParticlePreset, burstPos);

        // 着地時刻に到達した後は、噴き上がる粒子へ余韻の粒子を重ねる。
        if (crownCountPresentationTimer_ >= kCrownCountImpactTime) {
            GpuParticleManager::GetInstance()->Emit(kCrownUnlockAfterglowParticlePreset, burstPos);
        }
    }

    // 着地の瞬間を一度だけ確定し、数字が変わるのと同じタイミングで衝撃を出します。
    if (!crownCountPresentationImpactDone_ && crownCountPresentationTimer_ >= kCrownCountImpactTime) {
        crownCountPresentationImpactDone_ = true;
        MeshEffectManager::GetInstance()->SpawnEffectAt(kCrownUnlockFlashRingEffectPath,
                                                        targetPos + engine::Vector3{0.0f, 0.45f, 0.0f}, {0.0f, 0.0f, 0.0f},
                                                        {1.15f, 1.0f, 1.15f});
        MeshEffectManager::GetInstance()->SpawnEffectAt(kCrownUnlockRayFlashEffectPath, burstPos, {0.0f, 0.0f, 0.0f},
                                                        {1.05f, 1.05f, 1.05f});
        GpuParticleManager::GetInstance()->Emit(kCrownUnlockParticlePreset, burstPos);
        GpuParticleManager::GetInstance()->Emit(kCrownUnlockRayParticlePreset, burstPos);
        GpuParticleManager::GetInstance()->Emit(kCrownUnlockAfterglowParticlePreset, burstPos);

        // CPU 粒子が使える場合に、着地の GPU 演出へ飛沫を加える。
        if (particleSystem_) {
            engine::Vector3 up = {0.0f, 1.0f, 0.0f};
            particleSystem_->SpawnParticles(burstPos, 12, 6.0f, &up, 1.35f, {1.0f, 0.88f, 0.24f, 1.0f},
                                            {1.0f, 0.98f, 0.58f, 0.0f}, 0.34f, 0.82f, 0.58f, 0.04f);
        }
    }

    // 前の報酬状態を片付けてから、次に解放を見せるステージへ引き継ぎます。
    if (crownCountPresentationTimer_ >= kCrownCountPresentationDuration) {
        crownCountPresentationActive_ = false;
        crownCountPresentationStageIndex_ = -1;
        crownCountPresentationTimer_ = 0.0f;
        crownCountPresentationParticleTimer_ = 0.0f;
        crownCountPresentationImpactDone_ = false;

        unlockingStageIndex_ = FindPendingUnlockStage();

        // 未視聴の解放があれば、次に選択するステージとして登録する。
        if (unlockingStageIndex_ >= 0) {
            selectedStageIndex_ = unlockingStageIndex_;
            unlockTime_ = {};
            DebugConsole::GetInstance()->AddLog("Stage Select: crown unlock presentation start.");
        }
    }
}

void GameSelectScene::UpdateStageClearRewardCrown(engine::Object3D* crown, std::int32_t stageIndex, float deltaTime) {
    // 獲得王冠が落下して跳ね返る時刻を、HUD の数字更新と揃える。
    // 対象王冠が存在しない場合は、表示姿勢を計算しない。
    if (!crown) {
        return;
    }

    const engine::Vector3 targetPos = GetStageClearCrownPosition(stageIndex);

    engine::Vector3 position = targetPos;
    float scale = kStageClearCrownBaseScale;
    float emissive = 3.1f;
    float silverFlash = 0.0f;
    const float t = crownCountPresentationTimer_;

    // 着地までは上空から定位置へ寄せ、着地後は反発と発光を短時間で減衰させます。
    if (t < kCrownCountImpactTime) {
        const float p = SelectClamp01(t / kCrownCountImpactTime);
        const float e = SelectEaseOutCubic(p);
        const engine::Vector3 startPos = targetPos + engine::Vector3{0.0f, 4.35f, 0.0f};
        position = SelectLerp(startPos, targetPos, e);
        // 0から1の間だけ正になる半波を高さに足し、始点・着地点を保ったまま軌道に弧を付ける。
        position.y += std::sin(p * std::numbers::pi_v<float>) * 0.62f;
        scale = kStageClearCrownBaseScale * (0.18f + SelectEaseOutBack(p) * 0.92f);
        emissive = 2.6f + p * 2.8f;
        silverFlash = p;
        crown->SetEulerY(crown->GetRotation().y + deltaTime * (8.0f + (1.0f - p) * 7.5f));
        crown->SetEulerZ(std::sin(p * std::numbers::pi_v<float>) * 0.42f);
    // 着地時刻以降は、据え付け位置からの反発だけを進める。
    } else {
        const float p = t - kCrownCountImpactTime;
        // 着地から1.35秒で跳ね返りの振幅をゼロへ戻し、常時表示の王冠へつなぐ。
        const float decay = std::max(0.0f, 1.0f - p / 1.35f);
        // 正弦の絶対値で反動の高さを常に正にし、据え付け位置より下に揺れないようにする。
        const float bounce = std::abs(std::sin(p * 11.0f)) * 0.24f * decay;
        const float pop = std::sin(SelectClamp01(p / 0.42f) * std::numbers::pi_v<float>);
        position.y += bounce;
        scale = kStageClearCrownBaseScale * (1.0f + pop * 0.34f + std::sin(p * 14.0f) * 0.08f * decay);
        emissive = 2.55f + pop * 2.2f + decay * 0.9f;
        silverFlash = std::max(pop, decay * 0.45f);
        crown->SetEulerY(crown->GetRotation().y + deltaTime * (2.0f + decay * 4.5f));
        crown->SetEulerZ(std::sin(stageSelectTime_ * 5.0f) * 0.12f * std::max(0.25f, decay));
    }

    // 位置・姿勢・発光をまとめて反映してから、描画用行列を確定します。
    crown->SetTranslate(position);
    crown->SetScale({scale, scale, scale});
    crown->SetQuaternionMaster(false);
    crown->SetIsVisible(true);
    silverFlash = SelectClamp01(silverFlash);
    crown->SetColor({1.0f, 0.94f + silverFlash * 0.06f, 0.76f + silverFlash * 0.20f, 1.0f});
    crown->SetEmissive(emissive);
    crown->SetGpuParticleName(kCrownIdleParticlePreset);
    crown->SetMeshEffect1Name("");
    crown->SetMeshEffect2Name("");
    crown->UpdateLocalMatrix();
    crown->UpdateWorldMatrix();
}

void GameSelectScene::UpdateStageSelectCrownHudReward() {
    // 着地前は控えめな予告、着地後は王冠アイコンと数字を一斉に弾ませる。
    auto applySprite = [this](const StageSelectHudSprite& state, float scale, const engine::Vector4& color, float rotation) {
        // 毎回基準配置から変形して、演出後に位置や大きさを持ち越さない。
        engine::Sprite* sprite = ResolveStageSelectHUDSprite(state);
        // 配置から対応する Sprite を解決できない場合は、その一枚を更新しない。
        if (!sprite) {
            return;
        }

        sprite->SetPosition(state.basePosition);
        sprite->SetSize({state.baseSize.x * scale, state.baseSize.y * scale});
        sprite->SetColor(color);
        sprite->SetRotation(rotation);
        sprite->Update();
    };

    // 演出が終わった次のフレームには、アイコンと桁を配置時の大きさと色へ戻します。
    if (!crownCountPresentationActive_) {
        applySprite(stageSelectCrownIcon_, 1.0f, stageSelectCrownIcon_.baseColor, 0.0f);
        applySprite(stageSelectCrownXIcon_, 1.0f, stageSelectCrownXIcon_.baseColor, 0.0f);

        for (StageSelectHudSprite& digit : stageSelectCrownDigits_) {
            applySprite(digit, 1.0f, digit.baseColor, 0.0f);
        }

        return;
    }

    const float t = crownCountPresentationTimer_;
    // 着地からの時間だけを反発と発光に使い、着地前は反発を加えません。
    // 王冠数が切り替わった時点から時間を測り直し、拡大を0.52秒、光の余韻を1.15秒で収める。
    const float impactAge = std::max(0.0f, t - kCrownCountImpactTime);
    const float prePulse = std::sin(stageSelectTime_ * 11.0f) * 0.035f;
    const float pop = crownCountPresentationImpactDone_
                          ? std::sin(SelectClamp01(impactAge / 0.52f) * std::numbers::pi_v<float>) * 0.42f
                          : 0.0f;
    const float glow = crownCountPresentationImpactDone_ ? std::max(0.0f, 1.0f - impactAge / 1.15f) : 0.0f;
    const engine::Vector4 hotCrown = {1.0f, 0.88f + glow * 0.10f, 0.22f + glow * 0.34f, 1.0f};
    const engine::Vector4 hotNumber = {1.0f, 0.92f + glow * 0.08f, 0.36f + glow * 0.32f, 1.0f};

    applySprite(stageSelectCrownIcon_, 1.0f + prePulse + pop * 0.62f, hotCrown,
                std::sin(stageSelectTime_ * 7.0f) * 0.06f * (1.0f + glow));
    applySprite(stageSelectCrownXIcon_, 1.0f + pop * 0.24f, hotNumber, 0.0f);

    for (StageSelectHudSprite& digit : stageSelectCrownDigits_) {
        applySprite(digit, 1.0f + pop, hotNumber, 0.0f);
    }
}

std::int32_t GameSelectScene::GetDisplayedCrownCount() const {
    // 着地の瞬間まで旧個数を見せ、演出と数字の変化を一致させる。
    if (crownCountPresentationActive_ && !crownCountPresentationImpactDone_ &&
        crownCountPresentationTimer_ < kCrownCountImpactTime) {
        return crownCountPresentationFrom_;
    }

    return crownCountPresentationActive_ ? crownCountPresentationTo_
                                         : GameDataManager::GetInstance()->GetClearedStageCount();
}

engine::Vector3 GameSelectScene::GetStageClearCrownPosition(std::int32_t stageIndex) const {
    // ゲートモデルの上端を優先し、Aabb が無効ならノード基準の高さへ戻す。
    engine::Vector3 fallback = GetStageNodePosition(stageIndex) + engine::Vector3{0.0f, 3.0f, 0.0f};

    // 配置一覧が使えなければ、選択画面のノードから求めた代替位置を返す。
    if (!objectManager_) {
        return fallback;
    }

    for (const auto& object : objectManager_->GetObjects()) {
        // 対象の番号と一致するステージゲート以外は照合を続ける。
        if (!object || !IsStageGateObject(object.get()) || GetStageGateIndex(object.get()) != stageIndex) {
            continue;
        }

        // 対象ゲートの範囲が使える場合だけ、上面の中心に高さの余白を付けます。
        engine::Aabb gateAabb = object->GetModelWorldAabb();
        const bool validAabb = std::isfinite(gateAabb.min.x) && std::isfinite(gateAabb.min.y) &&
                               std::isfinite(gateAabb.min.z) && std::isfinite(gateAabb.max.x) &&
                               std::isfinite(gateAabb.max.y) && std::isfinite(gateAabb.max.z) &&
                               gateAabb.max.y > gateAabb.min.y;
        // 境界の成分が有限でないか上下の幅が無い場合は、モデルの上端を使わない。
        if (!validAabb) {
            return fallback;
        }

        return {(gateAabb.min.x + gateAabb.max.x) * 0.5f, gateAabb.max.y + kStageClearCrownHeightOffset,
                (gateAabb.min.z + gateAabb.max.z) * 0.5f};
    }

    return fallback;
}
