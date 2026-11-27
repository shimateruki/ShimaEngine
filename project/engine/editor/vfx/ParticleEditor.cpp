#include <cstdint>
#include "ParticleEditor.h"
#include "imgui.h"
#include "SceneManager.h" 
#include "BaseScene.h"    
#include "ParticleSystem.h" 
#include "ParticleManager.h"
#include "IconsFontAwesome5.h"
#include "EffectPreviewStage.h"
#include "EditorManager.h"
#include "ProfilerManager.h"
#include "VfxAuthoringEditor.h"
#include "VfxAssetPicker.h"
#include "VfxAssetCandidates.h"
#include <algorithm>
#include <vector>
namespace fs = std::filesystem;
// 更新と編集欄を開くたびに現在シーンを引けるよう、管理器を借用する。
void ParticleEditor::Initialize(SceneManager* sceneManager) {
    sceneManager_ = sceneManager;
}

void ParticleEditor::Update(float deltaTime, bool sceneIsPlaying) {
    // シーンを切り替えた場合は古いParticleSystemを参照せず、毎フレーム現在のシーンから取り直す。
    if (!sceneManager_ || !sceneManager_->GetCurrentScene()) {
        targetSystem_ = nullptr;
        return;
    }

    targetSystem_ = sceneManager_->GetCurrentScene()->GetParticleSystem();

    // シーンに CPU 粒子のシステムがなければ、プレビューで更新する実体がない。
    if (!targetSystem_) {
        return;
    }

    EffectPreviewStage* previewStage = EffectPreviewStage::GetInstance();
    const bool isSelected = EditorManager::GetInstance()->GetSelectedObject() == this;

    // このエディターが未選択か共通プレビューが無効なら、ゲーム用の位置と時間倍率へ戻す。
    if (!isSelected || !previewStage || !previewStage->IsEnabled()) {
        // プレビュー解除時はゲーム側の発生位置・時間倍率へ戻す。
        targetSystem_->SetEditorPreviewOffset({ 0.0f, 0.0f, 0.0f });
        targetSystem_->SetSimulationTimeScale(1.0f);
        return;
    }

    targetSystem_->SetEditorPreviewOffset(previewStage->GetPreviewPosition());
    const auto& params = targetSystem_->GetEmitterParams();
    // プレビューは短い粒子でも最低 3 秒確保し、長い素材では粒子寿命の後にも消滅を見られる余白を足す。
    const float duration = (std::max)(3.0f, params.particleLifetime + 0.75f);

    const auto restartPreview = [&](float seekTime) {
        // 粒子の履歴を直接戻せないため、固定刻みで先頭から再計算してシーク位置を再現する。
        targetSystem_->ResetSimulation();
        previewTime_ = 0.0f;

        // 連続発生が無効な設定は、先頭から再現するために一度だけ粒子を出す。
        if (!params.isEmitting) {
            targetSystem_->EmitOneShot(
                params,
                params.spawnPosition + previewStage->GetPreviewPosition());
        }

        targetSystem_->SetSimulationTimeScale(1.0f);
        constexpr float kSeekStep = 1.0f / 60.0f;
        float remaining = std::clamp(seekTime, 0.0f, duration);

        // 指定時刻まで固定刻みで進め、最後に残った端数秒も反映する。
        while (remaining > 0.0001f) {
            const float step = (std::min)(kSeekStep, remaining);
            targetSystem_->Update(step);
            previewTime_ += step;
            remaining -= step;
        }
    };

    // 新しい再生要求を受けた場合は、前回の粒子を消して先頭へ戻す。
    if (previewStage->GetPlayRequestSerial() != lastStagePlayRequestSerial_) {
        // リクエスト番号の変化だけを処理し、同じ再生・停止命令を毎フレーム繰り返さない。
        lastStagePlayRequestSerial_ = previewStage->GetPlayRequestSerial();
        restartPreview(0.0f);
    }

    // 新しい停止要求では、今の粒子とプレビュー位置を初期化する。
    if (previewStage->GetStopRequestSerial() != lastStageStopRequestSerial_) {
        lastStageStopRequestSerial_ = previewStage->GetStopRequestSerial();
        targetSystem_->ResetSimulation();
        previewTime_ = 0.0f;
    }

    // 新しいシーク要求なら、指定の秒数まで先頭から粒子を再計算する。
    if (previewStage->GetSeekRequestSerial() != lastStageSeekRequestSerial_) {
        lastStageSeekRequestSerial_ = previewStage->GetSeekRequestSerial();
        restartPreview(previewStage->GetSeekTargetTime());
    }

    const float effectiveSpeed = previewStage->GetPlaybackSpeed();
    targetSystem_->SetSimulationTimeScale(effectiveSpeed);

    // ゲーム実行時はシーンが粒子を更新する。エディターから二重に更新するのは避ける。
    if (!sceneIsPlaying && effectiveSpeed > 0.0f) {
        targetSystem_->Update(deltaTime);
    }

    // 進行速度が正の場合だけ、表示上の再生ヘッドを進める。
    if (effectiveSpeed > 0.0f) {
        previewTime_ += deltaTime * effectiveSpeed;

        // ループ終端では粒子実体もリセットして先頭へ戻し、シーク再計算で一時的に変えた倍率を戻す。
        if (previewStage->IsLoopEnabled() && previewTime_ >= duration) {
            restartPreview(0.0f);
            targetSystem_->SetSimulationTimeScale(effectiveSpeed);
        } else {
            // ループしないか終端に達していない場合は、表示時刻を尺より先へ進めない。
            previewTime_ = (std::min)(previewTime_, duration);
        }
    }

    std::vector<EffectPreviewStage::TimelineEvent> events;

    // 発生期間と粒子寿命を別レーンとして示し、設定と実際の生存時間を比較できるようにする。
    events.push_back({
        params.isEmitting ? "連続発生" : "単発発生",
        0.0f,
        params.isEmitting ? duration : 0.08f,
        { 0.30f, 0.82f, 1.0f, 0.90f }
    });
    events.push_back({
        "粒子寿命",
        0.0f,
        (std::min)(params.particleLifetime, duration),
        { 0.36f, 1.0f, 0.62f, 0.85f }
    });
    // 粒子数・CPUとGPUの時間・メモリを、共通プレビューの計測表示へ渡す。
    EffectPreviewStage::PerformanceMetrics performance;
    performance.available = true;
    performance.particleCount = static_cast<std::int32_t>(targetSystem_->GetActiveParticleCount());
    performance.cpuTimeMs = targetSystem_->GetLastUpdateCpuTimeMs() + targetSystem_->GetLastDrawCpuTimeMs();
    performance.gpuTimeMs = ProfilerManager::GetInstance()->GetLatestGpuTime("Particle CPU pass");
    performance.frameDeltaSeconds = deltaTime;
    performance.memoryBytes = targetSystem_->GetEstimatedMemoryBytes();
    previewStage->ReportToolState(
        EffectPreviewStage::ToolKind::kCpuParticle,
        "通常パーティクル",
        previewTime_,
        duration,
        effectiveSpeed > 0.0f,
        static_cast<std::int32_t>(targetSystem_->GetActiveParticleCount()),
        events,
        performance);
}

// 数値列をグラフに描き、ドラッグ位置から該当サンプルの値だけを編集します。
void EditCurve(const char* label, float* values, std::int32_t count, float minVal, float maxVal) {
#ifdef USE_IMGUI
    ImGui::Text("%s", label);

    // 横幅は現在の表示領域、高さは固定で、マウス座標の換算にも同じ寸法を使う。
    ImVec2 graphSize(ImGui::GetContentRegionAvail().x, 100.0f);

    // 編集できる値域を縦軸に設定し、サンプル列を描く。
    ImGui::PlotLines("##Curve", values, count, 0, nullptr, minVal, maxVal, graphSize);

    // グラフ上で左ボタンを押し続けている間だけ、マウス位置に対応するサンプルを書き換える。
    if (ImGui::IsItemHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        ImGuiIO& io = ImGui::GetIO();

        // マウス位置がグラフの左端から何%の位置にあるか計算
        float mouseX = io.MousePos.x - ImGui::GetItemRectMin().x;
        float normalizedX = mouseX / graphSize.x;

        // インデックスに変換 (0 ～ count-1)
        std::int32_t index = (std::int32_t)(normalizedX * count);

        // 求めた添字が対象配列の範囲内に入る場合だけ、対応する値を書き換える。
        if (index >= 0 && index < count) {
            // マウスのY位置から値を計算
            float mouseY = io.MousePos.y - ImGui::GetItemRectMin().y;

            // 画面の Y は下向き。上端を最大値、下端を最小値へ対応させるため割合を反転する。
            float normalizedY = 1.0f - (mouseY / graphSize.y);

            // 値を更新
            float newValue = minVal + normalizedY * (maxVal - minVal);
            values[index] = std::clamp(newValue, minVal, maxVal);
        }
    }
#endif
}

void ParticleEditor::DrawImGui() {
#ifdef USE_IMGUI
    // シーン管理器が接続されるまでは、編集欄を構築しない。
    if (sceneManager_ == nullptr)
        return;

    BaseScene* currentScene = sceneManager_->GetCurrentScene();

    // 現在シーンが存在しなければ、編集できない理由を表示する。
    if (currentScene == nullptr) {
        ImGui::TextDisabled(ICON_FA_EXCLAMATION_TRIANGLE " アクティブなシーンがありません");
        return;
    }

    engine::ParticleSystem* targetSystem = currentScene->GetParticleSystem();

    // CPU 粒子を持たないシーンは編集対象外とし、案内を表示する。
    if (targetSystem == nullptr) {
        ImGui::TextDisabled(ICON_FA_EXCLAMATION_TRIANGLE " このシーンにはパーティクルシステムがありません");
        return;
    }

    auto params = targetSystem->GetEmitterParams();

    // -------------------------------------------------------------
    // 1. 描画・形状設定
    // -------------------------------------------------------------
    // 描画・形状の欄を開いている場合だけ、合成と発生形状を編集できる。
    if (ImGui::CollapsingHeader(ICON_FA_PAINT_BRUSH " 描画・形状設定 (Rendering & Shape)", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* modes[] = { "Alpha (Normal)", "Add (Glow)", "Subtract (Dark)", "Multiply (Shadow)", "Screen (Soft Light)" };
        std::int32_t currentMode = (std::int32_t)params.blendMode;

        // 合成モードを選び直した場合に、設定に列挙値を反映する。
        if (ImGui::Combo(ICON_FA_ADJUST " Blend Mode", &currentMode, modes, IM_ARRAYSIZE(modes))) {
            params.blendMode = (ParticleBlendMode)currentMode;
        }

        const char* shapes[] = { "Box", "Sphere", "Cone" };
        std::int32_t currentShape = (std::int32_t)params.emitterType;

        // 発生形状を選び直した場合に、下に表示する形状別の項目も切り替える。
        if (ImGui::Combo(ICON_FA_SHAPES " Type (形状)", &currentShape, shapes, IM_ARRAYSIZE(shapes))) {
            params.emitterType = (EmitterType)currentShape;
        }

        // 箱型の発生範囲は、各軸の寸法を編集する。
        if (params.emitterType == EmitterType::kBox) {
            ImGui::DragFloat3(" 発生範囲 (Size)", &params.spawnArea.x, 0.1f);
        }
        // 球型は発生範囲の半径を編集する。
        else if (params.emitterType == EmitterType::kSphere) {
            ImGui::DragFloat(" 半径 (Radius)", &params.spawnRadius, 0.1f);
        }
        // 円錐型は広がり角を度で編集する。
        else if (params.emitterType == EmitterType::kCone) {
            ImGui::DragFloat(" 角度 (Angle)", &params.coneAngle, 1.0f, 0.0f, 90.0f);
        }

        ImGui::Checkbox(ICON_FA_TOGGLE_ON " 放出有効 (Emit)", &params.isEmitting);
        // 単発発生数が変わったら、システムの最大粒子数を越えない範囲に収める。
        if (ImGui::DragInt(
            ICON_FA_SORT_NUMERIC_UP " 単発の発生数 (Emit Count)",
            &params.emitCount, 1.0f, 1, engine::ParticleSystem::GetMaxParticles())) {
            params.emitCount = std::clamp(params.emitCount, 1, engine::ParticleSystem::GetMaxParticles());

            // 単発モードなら、個数の変更をすぐに見られるよう既存の粒子を消して再発生する。
            if (!params.isEmitting) {
                targetSystem->ResetSimulation();
                engine::Vector3 origin = params.spawnPosition;

                // プレビューが有効な場合は、ゲーム用の発生位置にプレビューの位置を足す。
                if (EffectPreviewStage* stage = EffectPreviewStage::GetInstance(); stage && stage->IsEnabled()) {
                    origin += stage->GetPreviewPosition();
                }

                targetSystem->EmitOneShot(params, origin);
            }
        }

        // 単発個数の入力にマウスがあるときだけ、個数が使われる場面を表示する。
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("単発プレビューとEmitOneShot 1回で生成する粒子数です");
        }

        ImGui::DragFloat(ICON_FA_STREAM " 生成レート (個/秒)", &params.particlesPerSecond, 1.0f, 0.0f, 1000.0f);
        // 生成レートにマウスがあるときだけ、毎秒の発生数であることを表示する。
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("1秒間に何個パーティクルを出すか");

        ImGui::DragFloat(ICON_FA_HOURGLASS_HALF " 生存時間 (Lifetime)", &params.particleLifetime, 0.1f, 0.1f, 10.0f);
    }

    // -------------------------------------------------------------
    // 2. 位置・速度・回転・重力設定
    // -------------------------------------------------------------
    // 挙動欄を開いた場合に、発生中心、初速度、角速度と加速度を編集できる。
    if (ImGui::CollapsingHeader(ICON_FA_COGS " 挙動設定 (Transform & Physics)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat3(ICON_FA_CROSSHAIRS " 発生中心座標", &params.spawnPosition.x, 0.1f);
        ImGui::DragFloat3(ICON_FA_EXPAND_ARROWS_ALT " 発生範囲 (乱数幅)", &params.spawnArea.x, 0.1f);
        // 発生範囲にマウスがあるとき、中心からの乱数幅を案内する。
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("中心座標から±この範囲内でランダムに出現します");

        ImGui::DragFloat3(ICON_FA_TACHOMETER_ALT " 初速度 (Velocity)", &params.initialVelocity.x, 0.1f);
        ImGui::DragFloat3(ICON_FA_RANDOM " 速度のばらつき", &params.velocityRandomness.x, 0.1f);

        ImGui::DragFloat(ICON_FA_SYNC " 回転スピード (度/秒)", &params.initialRotationSpeed, 1.0f);
        ImGui::DragFloat(ICON_FA_REDO " 回転のばらつき", &params.rotationSpeedRandomness, 1.0f);

        ImGui::DragFloat3(ICON_FA_ARROW_CIRCLE_DOWN " 加速度 (Gravity)", &params.acceleration.x, 0.01f);
        // 加速度にマウスがあるとき、Y の負値が下向きであることを案内する。
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Yをマイナスにすると重力がかかります");
    }

    // -------------------------------------------------------------
    // 3. 色・サイズ・テクスチャ設定
    // -------------------------------------------------------------
    // ビジュアル欄を開いた場合に、粒子の色・サイズ・画像を編集できる。
    if (ImGui::CollapsingHeader(ICON_FA_MAGIC " ビジュアル設定 (Visuals)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat(ICON_FA_SUN " HDR Intensity", &params.hdrIntensity, 0.1f, 0.0f, 50.0f);

        ImGui::ColorEdit4(ICON_FA_PALETTE " 開始色 (Start)", &params.startColor.x);
        ImGui::ColorEdit4(ICON_FA_FILL_DRIP " 終了色 (End)", &params.endColor.x);

        ImGui::Spacing();
        ImGui::Text(ICON_FA_CHART_LINE " サイズ推移 (Lifetime Graph)");
        ImGui::TextDisabled("※グラフをマウスでなぞって描けます");
        EditCurve("##SizeCurve", params.sizeCurve, 10, 0.0f, 5.0f);

        // リセットを押した場合は、旧曲線の全サンプルを等倍の1に戻す。
        if (ImGui::Button(ICON_FA_UNDO " カーブをリセット")) {
            for (std::int32_t i = 0; i < 10; i++)
                params.sizeCurve[i] = 1.0f;
        }

        ImGui::SeparatorText("共通Curve / Gradient");
        const engine::ParticleSystem::EmitterParams beforeAuthoringToggle = params;

        // 旧カーブから新形式へ切り替えた初回だけ、空のキーを既存の 10 点と始終色から補う。既に作った新キーは残す。
        if (ImGui::Checkbox("Authoring Curveを使用", &params.useAuthoringCurves)) {
            // 切り替えて共通カーブを有効にした場合にだけ、既存値から不足するキーを用意する。
            if (params.useAuthoringCurves) {
                // サイズのキーが一つもなければ、旧形式の10点を取り込む。
                if (params.sizeOverLife.keys.empty()) {
                    params.sizeOverLife = VfxFloatCurve::FromLegacySamples(params.sizeCurve, 10);
                }

                // 色のキーが一つもなければ、開始色と終了色からグラデーションを作る。
                if (params.colorOverLife.keys.empty()) {
                    params.colorOverLife = VfxColorGradient::FromTwoColors(params.startColor, params.endColor);
                }
            }

            const auto applyParams = [targetSystem](const engine::ParticleSystem::EmitterParams& value) {
                // トランザクションの反映先を参照できる場合だけ、設定全体を渡す。
                if (targetSystem)
                    targetSystem->SetEmitterParams(value);
            };

            editorPropertyTransaction::RegisterDiscrete(
                "CPU Particle: Authoring Curve",
                beforeAuthoringToggle,
                params,
                applyParams);
        }

        ImGui::TextDisabled("旧JSONは従来の10点Curveで同じ見た目を維持します。有効化後だけ新Curveを使用します。");

        // 共通カーブが有効なときだけ、サイズと色をキー単位で編集できる。
        if (params.useAuthoringCurves) {
            vfxAuthoringEditor::DrawFloatCurve(
                "CpuParticleSize",
                params.sizeOverLife,
                0.0f,
                8.0f,
                12,
                [targetSystem](const VfxFloatCurve& value) {
                    // 粒子のシステムが渡されていれば、編集したサイズカーブを反映する。
                    if (targetSystem)
                        targetSystem->SetSizeOverLife(value);
                });
            vfxAuthoringEditor::DrawColorGradient(
                "CpuParticleColor",
                params.colorOverLife,
                8,
                [targetSystem](const VfxColorGradient& value) {
                    // 粒子のシステムが渡されていれば、編集したグラデーションを反映する。
                    if (targetSystem)
                        targetSystem->SetColorOverLife(value);
                });
        }

        ImGui::Spacing();
        ImGui::Text(ICON_FA_IMAGE " テクスチャ (Texture)");
        const std::string directoryPath(vfxAssetCandidates::kCpuTextureRoot);
        static vfxAssetCandidates::CandidateList textureCandidates;
        const auto& files = textureCandidates.Get(vfxAssetCandidates::CpuTextureName);

        std::string_view currentPath = params.textureName;
        // CPU 粒子の共通フォルダーが先頭に付くパスは、候補一覧用の相対表記へ揃える。
        if (currentPath.starts_with(vfxAssetCandidates::kCpuTextureRoot))
            currentPath.remove_prefix(vfxAssetCandidates::kCpuTextureRoot.size());
        const auto selected = std::lower_bound(files.begin(), files.end(), currentPath);
        std::int32_t currentItem = selected != files.end() && *selected == currentPath
            ? static_cast<std::int32_t>(selected - files.begin()) : -1;

        // 候補が選ばれた場合は、ルートを補った画像の参照名でテクスチャを切り替える。
        if (vfxAssetPicker::DrawCombo(ICON_FA_FILE_IMAGE " Texture File", files,
                                      currentItem, textureSearch_, sizeof(textureSearch_))) {
            params.textureName = directoryPath + files[currentItem];
            targetSystem->SetTexture(params.textureName);
        }

        // 共通フォルダーの候補に含まれない画像も、参照名があれば現在値として表示する。
        if (currentItem < 0 && !params.textureName.empty())
            ImGui::TextDisabled("現在の画像: %s", params.textureName.c_str());
    }

    // 距離 LOD 欄が開いている間だけ、発生制限と現在の生存粒子数を表示する。
    if (ImGui::CollapsingHeader("距離LOD / Budget", ImGuiTreeNodeFlags_DefaultOpen)) {
        vfxAuthoringEditor::DrawLodSettings(
            "CpuParticleLod",
            params.lod,
            [targetSystem](const VfxLodSettings& value) {
                // 反映先が指定されている場合にだけ、編集した LOD 設定を渡す。
                if (targetSystem)
                    targetSystem->SetLodSettings(value);
            },
            1,
            engine::ParticleSystem::GetMaxParticles());
        const std::size_t limit = params.lod.enabled && params.lod.maxAliveParticles > 0
            ? static_cast<std::size_t>(params.lod.maxAliveParticles)
            : static_cast<std::size_t>(engine::ParticleSystem::GetMaxParticles());
        ImGui::TextDisabled("現在 %zu / 上限 %zu particles", targetSystem->GetActiveParticleCount(), limit);
    }

    // -------------------------------------------------------------
    // 4. データ保存・読み込み
    // -------------------------------------------------------------
    ImGui::Separator();

    files_.Draw("パーティクルJSONファイル", "Resources/json/particle",
        [&](const fs::path& path) {
            // 読み込んだパラメーターのうち GPU に使う画像は先にシステムへ適用し、残りは関数末尾で反映する。
            // JSON が読み込めない場合は、画像を切り替えずブラウザーへ失敗を返す。
            if (!ParticleManager::ReadParamFile(path, params)) return false;
            targetSystem->SetTexture(params.textureName);
            return true;
        },
        [&](const fs::path& path, const std::string&) { return ParticleManager::WriteParamFile(path, params); },
        [&] {
            params = engine::ParticleSystem::EmitterParams{};
            // 新規設定に既定の画像名があれば、テクスチャも即時に切り替える。
            if (!params.textureName.empty()) targetSystem->SetTexture(params.textureName);
        },
        [&](const fs::path& relative) {
            auto stem = relative;
            const auto key = stem.replace_extension().generic_u8string();
            // 保存後のキャッシュも更新する。別パネルが同じ相対キーを参照しても古い発生設定が残らない。
            ParticleManager::GetInstance()->GetParam(std::string(key.begin(), key.end())) = params;
        });

    // このフレームに編集した値を、次の更新・描画で使う設定へ適用する。
    targetSystem->SetEmitterParams(params);

#endif
}
