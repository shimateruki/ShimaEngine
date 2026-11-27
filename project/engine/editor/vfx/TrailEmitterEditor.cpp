#include <cstdint>
#include "TrailEmitterEditor.h"
#include <numbers>
#include <imgui.h>
#include <filesystem>
#include <fstream>
#include <json.hpp>
#include "IconsFontAwesome5.h"
#include "SceneManager.h"
#include "BaseScene.h"
#include "Object3D.h"
#include "MeshEffectManager.h"
#include "GpuParticleManager.h"
#include "EffectPreviewStage.h"
#include "EditorManager.h"
#include <algorithm>
#include <cmath>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ============================================================
// 現在のシーン管理を借用し、編集用の素材一覧を用意する。
// ============================================================
void TrailEmitterEditor::Initialize(SceneManager* sceneManager) {
    sceneManager_ = sceneManager;
    RefreshFileLists();
}

void TrailEmitterEditor::ResetDummyPreview(bool clearEffects) {
    // 再生時計は常に戻し、既存の効果を消すかは呼び出し側の指定に従う。
    previewTime_ = 0.0f;
    dummyFirstFrame_ = true;
    isDummyRunning_ = true;

    // 履歴の初期化も要求された場合は、メッシュと GPU 粒子の両方を消す。
    if (clearEffects) {
        MeshEffectManager::GetInstance()->ClearActiveEffects();
        GpuParticleManager::GetInstance()->ResetSimulation();
    }
}

void TrailEmitterEditor::StepDummyPreview(float deltaTime, bool allowLoop) {
    // 実キャラクターがいなくても往復するダミーを動かし、Trail発生条件を確認する。
    if (!isDummyRunning_) {
        return;
    }

    const float duration = (std::max)(previewDuration_, 0.01f);
    previewTime_ += (std::max)(deltaTime, 0.0f);

    // 設定した一周分に達したら、繰り返しの許可に従って時刻を調整する。
    if (previewTime_ >= duration) {
        // 繰り返す場合は余った時間を次周へ渡し、軌跡の始点を記録し直す。
        if (allowLoop) {
            previewTime_ = std::fmod(previewTime_, duration);
            dummyFirstFrame_ = true;
        // 繰り返さない場合は、ダミーの位置を末尾へ固定する。
        } else {
            previewTime_ = duration;
        }
    }

    engine::Vector3 origin = engine::Vector3::kZero;
    EffectPreviewStage* previewStage = EffectPreviewStage::GetInstance();
    // 専用ステージが有効で、このツールが選択中の場合だけ、往復の中心をステージのプレビュー点へ移す。
    if (previewStage && previewStage->IsEnabled() &&
        EditorManager::GetInstance()->GetSelectedObject() == this) {
        origin = previewStage->GetPreviewPosition();
    }

    // 正規化時刻の1周をサイン波の1周期に対応させ、中心からX方向へ往復する。
    const float t = previewTime_ / duration;
    dummyPos_ = origin;
    dummyPos_.x += sinf(t * std::numbers::pi_v<float> * 2.0f) * previewRange_;

    if (dummyFirstFrame_) {
        // 初回やループ直後の座標差で長いTrailを誤発生させない。
        lastDummyPos_ = dummyPos_;
        dummyFirstFrame_ = false;
        return;
    }

    engine::Vector3 diff = dummyPos_ - lastDummyPos_;
    const float distance = engine::Math::Length(diff);
    const TrailEmitterConfig& cfg = emitter_.GetConfig();

    if (distance < cfg.emitDistance) {
        // 設定距離に達するまで基準点を保持し、短い移動で発生数が増えすぎないようにする。
        return;
    }

    // 前回の発生点から今回の位置への向きを求め、+Zを基準にしたY回転へ変換する。
    engine::Vector3 dir = engine::Math::Normalize(diff);
    const float rotY = atan2f(dir.x, dir.z);
    engine::Vector3 spawnPos = {
        (lastDummyPos_.x + dummyPos_.x) * 0.5f,
        (lastDummyPos_.y + dummyPos_.y) * 0.5f,
        (lastDummyPos_.z + dummyPos_.z) * 0.5f
    };

    const engine::Vector3 rotation = cfg.autoOrient ? engine::Vector3{ 0.0f, rotY, 0.0f } : engine::Vector3::kZero;

    if (cfg.emitMesh && !cfg.meshEffectPreset.empty()) {
        // MeshとGPU粒子は同じ中点を使い、複合Trailの見た目を合わせる。
        MeshEffectManager::GetInstance()->SpawnEffectAt(
            "Resources/json/effect/" + cfg.meshEffectPreset + ".json",
            spawnPos,
            rotation,
            cfg.scale);
    }

    // GPU 粒子の発生が有効でプリセットが指定されていれば、同じ中点へ粒子を出す。
    if (cfg.emitParticle && !cfg.gpuParticlePreset.empty()) {
        engine::Matrix4x4 transform = engine::Math::MakeIdentity4x4();
        transform.m[3][0] = spawnPos.x;
        transform.m[3][1] = spawnPos.y;
        transform.m[3][2] = spawnPos.z;
        GpuParticleManager::GetInstance()->Emit(cfg.gpuParticlePreset, spawnPos, transform);
    }

    lastDummyPos_ = dummyPos_;
}

// ============================================================
// 実オブジェクトまたは往復ダミーの軌跡と、専用ステージの再生状態を更新する。
// ============================================================
void TrailEmitterEditor::Update(float deltaTime) {
    // 有効に使える刻みがほぼ0以下なら、通常の1フレーム分として1/60秒を使う。
    if (deltaTime <= 0.0001f)
        deltaTime = 1.0f / 60.0f;

    bool isGamePlaying = SceneManager::GetInstance() && SceneManager::GetInstance()->IsPlaying();
    EffectPreviewStage* previewStage = EffectPreviewStage::GetInstance();
    const bool isThisEditorSelected = EditorManager::GetInstance()->GetSelectedObject() == this;
    const bool usePreviewStage = previewStage && previewStage->IsEnabled() && isThisEditorSelected;
    MeshEffectManager* meshManager = MeshEffectManager::GetInstance();
    GpuParticleManager* gpuManager = GpuParticleManager::GetInstance();
    const float previewScale = usePreviewStage ? previewStage->GetPlaybackSpeed() : 1.0f;
    const float previewDelta = deltaTime * previewScale;

    // 専用ステージがこのツールの再生を管理している場合は、GPU 粒子にも同じ再生倍率を渡す。
    if (usePreviewStage) {
        gpuManager->SetTimeScale(previewScale);
    }

    // シーンがプレイしていない間だけ、プレビュー側がメッシュと粒子の更新を受け持つ。
    if (!isGamePlaying) {
        meshManager->Update(previewDelta);
        // 粒子はマネージャー側で時間倍率が掛かるため、手動更新にも未補正の秒数を渡す。
        gpuManager->Update(deltaTime);
    }

    // プレイ開始で編集時のオブジェクトが作り直されるため、以前の借用参照と再生を止める。
    if (isGamePlaying && !wasPlaying_) {
        targetObject_    = nullptr;
        isTracking_      = false;
        isDummyRunning_  = false;
        dummyFirstFrame_ = true;
        emitter_.Stop();
    }

    // プレイ終了でゲーム中のオブジェクトが破棄されるため、同様に追従とダミーを停止する。
    if (!isGamePlaying && wasPlaying_) {
        targetObject_    = nullptr;
        isTracking_      = false;
        isDummyRunning_  = false;
        dummyFirstFrame_ = true;
        emitter_.Stop();
    }

    wasPlaying_ = isGamePlaying;

    // シーンをまたいで以前の Object へ追従しないよう、現在のシーンを照合する。
    BaseScene* currentScene = sceneManager_ ? sceneManager_->GetCurrentScene() : nullptr;

    // シーンが変わったフレームは旧参照と発生状態を捨て、再生の評価を次回へ送る。
    if (lastScene_ != currentScene) {
        lastScene_       = currentScene;
        targetObject_    = nullptr;
        isTracking_      = false;
        isDummyRunning_  = false;
        dummyFirstFrame_ = true;
        emitter_.Stop();
        return;
    }

    // 発生履歴も巻き戻し、ダミー位置と両方のエフェクトを同じ刻みで進め直す。
    auto seekDummyPreview = [&](float seekTime) {
        ResetDummyPreview(true);
        const float targetTime = std::clamp(seekTime, 0.0f, (std::max)(previewDuration_, 0.01f));
        const float previousGpuScale = gpuManager->GetTimeScale();
        gpuManager->SetTimeScale(1.0f);

        while (previewTime_ + (1.0f / 60.0f) < targetTime) {
            StepDummyPreview(1.0f / 60.0f, false);
            meshManager->UpdateEditorPreviewStep(1.0f / 60.0f);
            gpuManager->UpdateEditorPreviewStep(1.0f / 60.0f);
        }

        // 1/60秒の固定ステップで残った端数は、ダミーと両方の演出を同じ秒数だけ進める。
        if (targetTime > previewTime_) {
            const float remainder = targetTime - previewTime_;
            StepDummyPreview(remainder, false);
            meshManager->UpdateEditorPreviewStep(remainder);
            gpuManager->UpdateEditorPreviewStep(remainder);
        }

        gpuManager->SetTimeScale(previousGpuScale);
    };

    // 専用ステージの新しい再生要求で、先頭へシークして履歴も作り直す。
    if (usePreviewStage && previewStage->GetPlayRequestSerial() != lastStagePlayRequestSerial_) {
        lastStagePlayRequestSerial_ = previewStage->GetPlayRequestSerial();
        seekDummyPreview(0.0f);
    }

    // 専用ステージの新しい停止要求で、ダミーの時刻と発生中の演出を消す。
    if (usePreviewStage && previewStage->GetStopRequestSerial() != lastStageStopRequestSerial_) {
        lastStageStopRequestSerial_ = previewStage->GetStopRequestSerial();
        isDummyRunning_ = false;
        previewTime_ = 0.0f;
        meshManager->ClearActiveEffects();
        gpuManager->ResetSimulation();
    }

    // 新しいシーク要求を受けたフレームだけ、指定秒数へ履歴を再現する。
    if (usePreviewStage && previewStage->GetSeekRequestSerial() != lastStageSeekRequestSerial_) {
        lastStageSeekRequestSerial_ = previewStage->GetSeekRequestSerial();
        seekDummyPreview(previewStage->GetSeekTargetTime());
    }

    // 実在する対象を追従中なら、ダミーを進めずエミッターに経過秒を渡す。
    if (isTracking_ && targetObject_) {
        // エディットモードのみ: シーンのオブジェクトリストで有効性を検証
        // Play中は GetObjects() にゲームオブジェクトが含まれないケースがあるためスキップ
        if (!isGamePlaying && currentScene) {
            bool stillValid = false;

            for (auto& obj : currentScene->GetObjects()) {
                // シーン内に借りたポインターと同じ個体があるか確認する。
                if (obj.get() == targetObject_) {
                    stillValid = true;
                    break;
                }
            }

            // 編集シーンに対象が見つからなければ借用参照を外し、この追従を停止する。
            if (!stillValid) {
                targetObject_ = nullptr;
                isTracking_   = false;
                emitter_.Stop();
                return;
            }
        }

        emitter_.Update(previewDelta);
        return;
    }

    // 追従しておらずダミーを再生中なら、ステージの繰り返し指定に合わせて往復を進める。
    if (isDummyRunning_) {
        const bool allowLoop = usePreviewStage ? previewStage->IsLoopEnabled() : true;
        StepDummyPreview(previewDelta, allowLoop);

        // 専用ステージで繰り返し無しのまま末尾に達したら、ダミーを停止する。
        if (usePreviewStage && !allowLoop && previewTime_ >= previewDuration_) {
            isDummyRunning_ = false;
        }
    }

    // 専用ステージでの確認中は、共通タイムラインへ軌跡と発生設定を表示する。
    if (usePreviewStage) {
        const float duration = (std::max)(previewDuration_, 0.01f);
        const TrailEmitterConfig& config = emitter_.GetConfig();
        std::vector<EffectPreviewStage::TimelineEvent> events;
        events.push_back({ "Motion path", 0.0f, duration, engine::Vector4{ 0.65f, 0.45f, 1.0f, 1.0f } });

        // メッシュの発生を有効にしている場合は、対応する発生行を追加する。
        if (config.emitMesh) {
            events.push_back({ "Mesh emit", 0.0f, duration, engine::Vector4{ 0.25f, 0.8f, 1.0f, 1.0f } });
        }

        // 粒子の発生を有効にしている場合は、粒子側の行も追加する。
        if (config.emitParticle) {
            events.push_back({ "Particle emit", 0.0f, duration, engine::Vector4{ 1.0f, 0.55f, 0.2f, 1.0f } });
        }

        previewStage->ReportToolState(
            EffectPreviewStage::ToolKind::kTrail,
            "Trail Emitter",
            std::clamp(previewTime_, 0.0f, duration),
            duration,
            previewStage->IsTransportPlaying(),
            static_cast<std::int32_t>(meshManager->GetActiveEffects().size()) + gpuManager->GetActiveSystemCount(),
            events);
    }
}

// ============================================================
// 設定の保存と素材の選択、実オブジェクト追従と往復ダミーの操作を描画する。
// ============================================================
void TrailEmitterEditor::DrawImGui() {
#ifdef USE_IMGUI
    TrailEmitterConfig& cfg = emitter_.GetConfig();

    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.1f, 1.0f), ICON_FA_FIRE " Trail Emitter Editor");
    ImGui::Separator();

    files_.Draw("トレイルJSONファイル", "Resources/json/trail_emitter",
        [this](const fs::path& path) { return Load(path); },
        [this](const fs::path& path, const std::string&) { return Save(path); },
        [this] { emitter_.GetConfig() = TrailEmitterConfig{}; });
    // 更新ボタンを押したときだけ、現在の素材フォルダーを再走査する。
    if (ImGui::Button(ICON_FA_SYNC " 素材一覧を更新")) RefreshFileLists();

    ImGui::Separator();

    // 追従対象は現在のシーンに登録されている Object から指定する。
    ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.8f, 1.0f), ICON_FA_CROSSHAIRS " 追従対象オブジェクト");

    // 対象のシーンが読み込まれている場合だけ、借用できる候補を表示する。
    if (sceneManager_ && sceneManager_->GetCurrentScene()) {
        auto& objects = sceneManager_->GetCurrentScene()->GetObjects();
        std::string currentName = targetObject_ ? targetObject_->GetName() : "なし (None)";

        // 選択欄を開いた場合に、追従解除と有効な Object を候補にする。
        if (ImGui::BeginCombo("##TargetObject", currentName.c_str())) {
            // なしを選んだら対象の借用を解除する。
            if (ImGui::Selectable("なし (None)", targetObject_ == nullptr)) {
                targetObject_ = nullptr;

                // すでに追従を開始していた場合は、エミッターを止めてから選択を確定する。
                if (isTracking_) {
                    emitter_.Stop();
                    isTracking_ = false;
                }
            }

            // 生成済みの実オブジェクトを列挙し、エディター専用の補助物を除く。
            for (auto& obj : objects) {
                // 無効な参照や内部用の個体は、選んでもゲームの確認に使えないため候補から外す。
                if (!obj || obj->IsEditorInternal())
                    continue;
                bool isSel = (targetObject_ == obj.get());
                std::string label = obj->GetName();
                // 名前が無い候補にも、選択できるよう代替の表示名を出す。
                if (label.empty())
                    label = "(NoName)";
                // 選択した個体を次の追従開始時の対象として記録する。
                if (ImGui::Selectable(label.c_str(), isSel)) {
                    targetObject_ = obj.get();

                    // 対象を切り替える際、前の座標履歴で軌跡を引かないよう、実行中なら一度止める。
                    if (isTracking_) {
                        emitter_.Stop();
                        isTracking_ = false;
                    }
                }

                // 現在の選択項目を、候補を開いた直後のフォーカス先にする。
                if (isSel)
                    ImGui::SetItemDefaultFocus();
            }

            ImGui::EndCombo();
        }
    // 現行シーンが無い場合は、選択欄の代わりに準備待ちを表示する。
    } else {
        ImGui::TextDisabled("シーンが読み込まれていません");
    }

    // 選択中オブジェクトの情報表示
    // 追従対象が選択済みのときだけ、そのワールド位置を表示する。
    if (targetObject_) {
        engine::Vector3 pos = targetObject_->GetWorldPosition();
        ImGui::TextDisabled("  位置: (%.2f, %.2f, %.2f)", pos.x, pos.y, pos.z);
    }

    ImGui::Separator();

    // ── メッシュエフェクト設定 ──
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), ICON_FA_MAGIC " メッシュエフェクト");
    ImGui::Checkbox("##emitMesh", &cfg.emitMesh);
    ImGui::SameLine();
    ImGui::BeginDisabled(!cfg.emitMesh);

    // メッシュの候補がある場合に、保存中の参照名を選び直せる。
    if (!meshEffectList_.empty()) {
        std::int32_t curIdx = 0;

        for (std::int32_t i = 0; i < (std::int32_t)meshEffectList_.size(); ++i)
            // 適用中の名前に一致する候補があれば、初期フォーカスの番号に使う。
            if (meshEffectList_[i] == cfg.meshEffectPreset) {
                curIdx = i;
                break;
            }
        // メッシュの選択欄を開いた場合に、フォルダー内のプリセット名を表示する。
        if (ImGui::BeginCombo("エフェクトプリセット##mesh", cfg.meshEffectPreset.empty() ? "(なし)" : cfg.meshEffectPreset.c_str())) {
            for (std::int32_t i = 0; i < (std::int32_t)meshEffectList_.size(); ++i) {
                bool sel = (curIdx == i);
                // 選んだ候補の名前を参照として保存する。
                if (ImGui::Selectable(meshEffectList_[i].c_str(), sel))
                    cfg.meshEffectPreset = meshEffectList_[i];
                // 一致する既存の候補へ選択欄のフォーカスを合わせる。
                if (sel)
                    ImGui::SetItemDefaultFocus();
            }

            ImGui::EndCombo();
        }
    // 利用できるメッシュ設定がない場合は、参照先のフォルダーを表示する。
    } else {
        ImGui::TextDisabled("Resources/json/effect/ にJSONが見当たりません");
    }

    ImGui::EndDisabled();

    // ── GPUパーティクル設定 ──
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), ICON_FA_FIRE " GPUパーティクル");
    ImGui::Checkbox("##emitPart", &cfg.emitParticle);
    ImGui::SameLine();
    ImGui::BeginDisabled(!cfg.emitParticle);

    // 粒子の候補がある場合にだけ、その参照を選択できる。
    if (!gpuParticleList_.empty()) {
        // 粒子の選択欄を開いた場合に、名前と現在の選択状態を表示する。
        if (ImGui::BeginCombo("パーティクルプリセット##gpu", cfg.gpuParticlePreset.empty() ? "(なし)" : cfg.gpuParticlePreset.c_str())) {
            for (auto& name : gpuParticleList_) {
                bool sel = (cfg.gpuParticlePreset == name);
                // 選択した GPU 粒子の名前を保存する。
                if (ImGui::Selectable(name.c_str(), sel))
                    cfg.gpuParticlePreset = name;
                // 適用中の粒子に選択欄の初期フォーカスを合わせる。
                if (sel)
                    ImGui::SetItemDefaultFocus();
            }

            ImGui::EndCombo();
        }
    // 粒子設定が見つからなければ、設定ファイルの場所を表示する。
    } else {
        ImGui::TextDisabled("Resources/json/gpu_particles/ にJSONが見当たりません");
    }

    ImGui::EndDisabled();

    // ── 共通設定 ──
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), ICON_FA_SLIDERS_H " 共通設定");
    ImGui::DragFloat("Emit間隔 (m)", &cfg.emitDistance, 0.01f, 0.01f, 5.0f, "%.2f");
    ImGui::DragFloat3("スケール倍率", &cfg.scale.x, 0.01f, 0.01f, 10.0f);
    ImGui::Checkbox("移動方向に自動回転 (AutoOrient)", &cfg.autoOrient);

    // ── 追従プレビュー (実オブジェクト) ──
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.8f, 0.5f, 1.0f, 1.0f), ICON_FA_RUNNING " 実オブジェクト追従");

    // 追従対象を選んでいる間は、開始または停止を操作できる。
    if (targetObject_) {
        // 未追従なら、選択先で追従を始めるボタンを表示する。
        if (!isTracking_) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.7f, 0.2f, 1.0f));

            // 追従を開始した場合はダミーを止め、指定 Object の位置で発生距離の記録を開始する。
            if (ImGui::Button(ICON_FA_PLAY " 追従開始", ImVec2(-1, 30))) {
                isDummyRunning_ = false;
                emitter_.Start(targetObject_);
                isTracking_ = true;
            }

            ImGui::PopStyleColor();
        // 追従中は、停止操作と発生距離の診断値を表示する。
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.1f, 0.1f, 1.0f));

            // 停止操作では追従を終了し、ダミーの再生は自動で始めない。
            if (ImGui::Button(ICON_FA_STOP " 追従停止", ImVec2(-1, 30))) {
                emitter_.Stop();
                isTracking_ = false;
            }

            ImGui::PopStyleColor();

            // ── 診断情報 ──
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
                ICON_FA_CIRCLE " 追従中: %s", targetObject_->GetName().c_str());

            engine::Vector3 curPos  = targetObject_->GetWorldPosition();
            engine::Vector3 lastPos = emitter_.GetLastEmitPos();
            engine::Vector3 diff    = { curPos.x - lastPos.x, curPos.y - lastPos.y, curPos.z - lastPos.z };
            float distance = sqrtf(diff.x*diff.x + diff.y*diff.y + diff.z*diff.z);

            ImGui::TextDisabled("  現在位置: (%.2f, %.2f, %.2f)", curPos.x, curPos.y, curPos.z);
            ImGui::TextDisabled("  前回Emit: (%.2f, %.2f, %.2f)", lastPos.x, lastPos.y, lastPos.z);
            ImGui::TextDisabled("  移動距離: %.3f / %.3f (しきい値)", distance, emitter_.GetConfig().emitDistance);
            ImGui::TextDisabled("  エミッター有効: %s", emitter_.IsActive() ? "YES" : "NO");
            ImGui::TextDisabled("  発火中エフェクト数: %d",
                (std::int32_t)MeshEffectManager::GetInstance()->GetActiveEffects().size());
        }
    // 対象未選択の間は、追従を開始できない案内を出す。
    } else {
        ImGui::BeginDisabled(true);
        ImGui::Button(ICON_FA_MOUSE_POINTER " 先に対象オブジェクトを選択してください", ImVec2(-1, 30));
        ImGui::EndDisabled();
    }

    // ── ダミープレビュー (サイン波) ──
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 1.0f, 1.0f), ICON_FA_PLAY_CIRCLE " サイン波ダミープレビュー");
    ImGui::DragFloat("往復距離 (m)",    &previewRange_,    0.1f, 0.5f, 30.0f, "%.1f");
    ImGui::DragFloat("サイクル時間 (s)", &previewDuration_, 0.05f, 0.2f, 10.0f, "%.2f");

    // ダミーが停止中なら、先頭から始める操作を表示する。
    if (!isDummyRunning_) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.7f, 1.0f));

        // 開始の押下で時刻を0へ戻し、初回位置を記録する。
        if (ImGui::Button(ICON_FA_PLAY " ダミー開始", ImVec2(-1, 28))) {
            // 実オブジェクトを追従中の場合は、ダミーと同時に発生しないよう停止する。
            if (isTracking_) {
                emitter_.Stop();
                isTracking_ = false;
            }
            isDummyRunning_  = true;
            previewTime_     = 0.0f;
            dummyFirstFrame_ = true;
        }

        ImGui::PopStyleColor();
    // ダミーが動作中なら、現在位置と停止操作を表示する。
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.1f, 0.5f, 1.0f));

        // 停止ボタンで往復を止める。既に発生した演出の終了は各管理に任せる。
        if (ImGui::Button(ICON_FA_STOP " ダミー停止", ImVec2(-1, 28))) {
            isDummyRunning_ = false;
        }

        ImGui::PopStyleColor();
        ImGui::TextDisabled("ダミー位置: X=%.2f  Y=%.2f  Z=%.2f", dummyPos_.x, dummyPos_.y, dummyPos_.z);
    }

    // ── ゲームコード向け使い方ガイド ──
    ImGui::Separator();

    // 使い方の欄が開いている場合だけ、今の設定を使う呼び出し例を表示する。
    if (ImGui::CollapsingHeader(ICON_FA_CODE " ゲームコードでの使い方")) {
        ImGui::TextWrapped(
            "TrailEmitter trail;\n"
            "trail.GetConfig().meshEffectPreset = \"%s\";\n"
            "trail.GetConfig().emitDistance = %.2f;\n"
            "// 攻撃開始時:\n"
            "trail.Start(swordTipObject);\n"
            "// 毎フレーム:\n"
            "trail.Update(deltaTime);\n"
            "// 攻撃終了時:\n"
            "trail.Stop();",
            cfg.meshEffectPreset.c_str(), cfg.emitDistance
        );
    }
#endif
}

// ============================================================
//  RefreshFileLists
// ============================================================
void TrailEmitterEditor::RefreshFileLists() {
    // 選択欄には実在するJSONの名前だけを並べ、保存後も同じ一覧を更新する。
    meshEffectList_ = JsonAssetBrowser::ListKeys("Resources/json/effect");
    gpuParticleList_ = JsonAssetBrowser::ListKeys("Resources/json/gpu_particles");
}

// ============================================================
//  Save
// ============================================================
bool TrailEmitterEditor::Save(const fs::path& path) {
    // 軌跡の参照プリセットと放出条件を既存のJSON形式で書き出す。
    TrailEmitterConfig& cfg = emitter_.GetConfig();
    json j;
    j["meshEffectPreset"]  = cfg.meshEffectPreset;
    j["gpuParticlePreset"] = cfg.gpuParticlePreset;
    j["emitDistance"]      = cfg.emitDistance;
    j["scale"]             = { cfg.scale.x, cfg.scale.y, cfg.scale.z };
    j["emitMesh"]          = cfg.emitMesh;
    j["emitParticle"]      = cfg.emitParticle;
    j["autoOrient"]        = cfg.autoOrient;

    std::ofstream file(path);
    file << j.dump(4);
    file.close();
    return file.good();
}

// ============================================================
//  Load
// ============================================================
bool TrailEmitterEditor::Load(const fs::path& path) {
    // 保存された放出条件をエミッターへ反映し、参照する素材名も復元する。
    std::ifstream f(path);
    // 選択したファイルを開けなければ、編集値を変更せず失敗を返す。
    if (!f) return false;
    json j;
    f >> j;
    f.close();

    // トップレベルが設定オブジェクトでなければ、反映を始めない。
    if (!j.is_object()) return false;
    // 旧ファイルにない項目は現在値を維持し、読み取った項目だけ一時設定に上書きする。
    auto cfg = emitter_.GetConfig();
    // メッシュの参照が保存されている場合だけ上書きする。
    if (j.contains("meshEffectPreset"))
        cfg.meshEffectPreset  = j["meshEffectPreset"];
    // 粒子の参照が保存されている場合だけ上書きする。
    if (j.contains("gpuParticlePreset"))
        cfg.gpuParticlePreset = j["gpuParticlePreset"];
    // 発生距離が指定されていれば復元する。
    if (j.contains("emitDistance"))
        cfg.emitDistance      = j["emitDistance"];
    // 倍率のキーがあれば、保存順の XYZ で復元する。
    if (j.contains("scale")) {
        cfg.scale.x = j["scale"][0];
        cfg.scale.y = j["scale"][1];
        cfg.scale.z = j["scale"][2];
    }
    // メッシュ発生の設定がある場合は、オン・オフを戻す。
    if (j.contains("emitMesh"))
        cfg.emitMesh          = j["emitMesh"];
    // 粒子発生の設定はメッシュと独立に復元する。
    if (j.contains("emitParticle"))
        cfg.emitParticle      = j["emitParticle"];
    // 自動回転の指定が存在する場合は、移動方向への向き合わせを切り替える。
    if (j.contains("autoOrient"))
        cfg.autoOrient        = j["autoOrient"];
    // 全項目を一時設定へ読み終えてから、実際に使うエミッターへ反映する。
    emitter_.GetConfig() = std::move(cfg);
    return true;
}
