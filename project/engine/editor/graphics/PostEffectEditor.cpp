#include <cstdint>
#include "PostEffectEditor.h"
#include "imgui.h"
#include "json.hpp" // JSONライブラリ
#include <fstream>
#include <filesystem>
#include "IconsFontAwesome5.h"
#include "Fade.h"
#include "engine/graphics/core/ColorSpace.h"
#include "engine/graphics/3d/light/LightManager.h"
using json = nlohmann::json;

void PostEffectEditor::Initialize(PostEffect* postEffect) {
    targetEffect_ = postEffect;

    // 起動時に自動で前回の設定をロードする
    LoadParams();
}

// PostEffect の描画定数とワークフローを現在の画面に反映し、フェードの試写と設定保存を受け付ける。
void PostEffectEditor::DrawImGui() {
#ifdef USE_IMGUI
    // 描画先がセットされるまでは、描画定数の取得を避ける。
    if (!targetEffect_)
        return;

    auto* params = targetEffect_->GetParams();

    ImGui::Text("Color Workflow");
    bool linearWorkflow = engine::colorSpace::WorkflowSettings::GetInstance().IsLinearWorkflowEnabled();

    // 色空間を切り替えたときは共有設定とシーンのクリア色を更新する。
    if (ImGui::Checkbox("リニアワークフローを使用", &linearWorkflow)) {
        engine::colorSpace::WorkflowSettings::GetInstance().SetLinearWorkflowEnabled(linearWorkflow);
        engine::LightManager::GetInstance()->ApplySceneClearColor();

        // リニアへ切り替えた直後にトーンマッピングが無効なら、HDR 合成値を表示へ変換するモードを選ぶ。
        if (linearWorkflow && params->enableToneMapping == 0) {
            params->enableToneMapping = 1;
        }
    }

    ImGui::TextDisabled(
        linearWorkflow
            ? "sRGB入力をリニア化し、HDR合成後にsRGB出力します。"
            : "従来互換の色計算です。既存シーンとの比較用に残しています。");
    ImGui::Separator();

    // ==========================================================
    // トーンマッピングのモード切り替え
    // ==========================================================
    ImGui::Text(ICON_FA_ADJUST " Tone Mapping Mode");

    std::int32_t mode = params->enableToneMapping;

    // 無効ボタンでは、合成後のトーンマッピングを通さない。
    if (ImGui::RadioButton("OFF (No ToneMap)", mode == 0)) {
        mode = 0;
    }
    // 実写向けボタンでは、明るい部分を白へ寄せる ACES を選ぶ。
    if (ImGui::RadioButton(ICON_FA_EYE " Real (ACES - White Out)", mode == 1)) {
        mode = 1;
    }
    // アニメ向けボタンでは、鮮やかさを残す ACES を選ぶ。
    if (ImGui::RadioButton(ICON_FA_PALETTE " Anime (ACES - Vivid Color)", mode == 2)) {
        mode = 2;
    }
    params->enableToneMapping = mode;

    // 実写向けを選んでいるときは、用途の補足を表示する。
    if (mode == 1) {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "  Good for Realistic/Movie style.");
    }
    // アニメ向けを選んでいるときは、鮮やかな演出に向くことを表示する。
    else if (mode == 2) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "  Good for Anime/Vivid Magic effects.");
    }

    ImGui::Separator();

    ImGui::Text(ICON_FA_PALETTE " Color Grading / LUT");
    ImGui::DragFloat(" LUT Intensity", &params->lutIntensity, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat(" Exposure", &params->colorExposure, 0.01f, -2.0f, 2.0f);
    ImGui::DragFloat(" Contrast", &params->colorContrast, 0.01f, 0.0f, 2.5f);
    ImGui::DragFloat(" Saturation", &params->colorSaturation, 0.01f, 0.0f, 2.5f);
    ImGui::DragFloat(" Temperature", &params->colorTemperature, 0.01f, -1.0f, 1.0f);
    ImGui::DragFloat(" Tint", &params->colorTint, 0.01f, -1.0f, 1.0f);

    // 色調と周辺減光だけをまとめて差し替える。Bloom の設定やファイル上の保存値はこのボタンでは変更しない。
    if (ImGui::Button("Soft Adventure Preset")) {
        params->enableToneMapping = 2;
        params->lutIntensity = 0.28f;
        params->colorExposure = 0.02f;
        params->colorContrast = 1.08f;
        params->colorSaturation = 1.14f;
        params->colorTemperature = 0.10f;
        params->colorTint = 0.0f;
        params->vignetteIntensity = 0.04f;
        params->vignettePower = 1.15f;
    }

    ImGui::Separator();

    // ==========================================================
    // ブルームの設定
    // ==========================================================
    ImGui::Text(ICON_FA_SUN " Bloom Settings");

	bool bloomEnabled = targetEffect_->IsBloomEnabled();

	// チェックが変化したときに Bloom パスの有効状態を切り替える。
	if (ImGui::Checkbox("Bloomを使用", &bloomEnabled)) {
		targetEffect_->SetBloomEnabled(bloomEnabled);
	}

	std::int32_t bloomQuality = static_cast<std::int32_t>(targetEffect_->GetBloomQuality());
	const char* bloomQualityItems[] = {
		"低 (4 passes / 1 level)",
		"中 (6 passes / 2 levels)",
		"高 (10 passes / 4 levels)",
	};

	// 選択した品質を反映し、縮小レベル数を切り替える。
	if (ImGui::Combo("Bloom品質", &bloomQuality, bloomQualityItems, IM_ARRAYSIZE(bloomQualityItems))) {
		targetEffect_->SetBloomQuality(static_cast<PostEffect::BloomQuality>(bloomQuality));
	}

	ImGui::Text(
		"現在の後処理: %d passes",
		targetEffect_->GetExpectedPostEffectPassCount());
	// Bloom の処理条件を満たさない場合は、合成だけになることを表示する。
	if (!targetEffect_->IsBloomActive()) {
		ImGui::TextDisabled("Bloom無効またはIntensityが0のため、最終Composite 1回だけ実行します。");
	}

	ImGui::BeginDisabled(!bloomEnabled);
    ImGui::DragFloat(" Threshold (発光の閾値)", &params->threshold, 0.01f, 0.0f, 5.0f);
    ImGui::DragFloat(" Intensity (光の強さ)", &params->bloomIntensity, 0.01f, 0.0f, 10.0f);
    ImGui::DragFloat(" Spread (ぼかしの広がり)", &params->spread, 0.1f, 0.0f, 10.0f);
	ImGui::EndDisabled();

    ImGui::Separator();

    // ==========================================================
    // シネマティック効果
    // ==========================================================
    ImGui::Text(ICON_FA_FILM " Cinematic Effects");
    ImGui::DragFloat(ICON_FA_MOON " Vignette (周辺減光強度)", &params->vignetteIntensity, 0.01f, 0.0f, 5.0f);
    ImGui::DragFloat(" Vignette Power (減光の丸み)", &params->vignettePower, 0.01f, 0.0f, 5.0f);
    ImGui::DragFloat(ICON_FA_COINS " Chromatic Aberration (色収差)", &params->chromaticAberration, 0.001f, 0.0f, 0.1f);
    ImGui::DragFloat(ICON_FA_BROADCAST_TOWER " Film Grain (ノイズ)", &params->filmGrainIntensity, 0.001f, 0.0f, 0.5f);

    ImGui::Spacing();
    ImGui::Text(ICON_FA_VIDEO " Action Camera Effects");
    ImGui::DragFloat(ICON_FA_EXPAND_ARROWS_ALT " Radial Blur (集中線の強さ)", &params->radialIntensity, 0.01f, 0.0f, 1.0f);
    // 中心は画像上の正規化座標。0.5、0.5 で画面中央になり、解像度を変えても比率を保つ。
    ImGui::DragFloat(" Radial Center X", &params->radialCenterX, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat(" Radial Center Y", &params->radialCenterY, 0.01f, 0.0f, 1.0f);
    ImGui::DragInt(" Radial Blur Samples (サンプリング数)", &params->radialBlurSamples, 0.1f, 1, 64);

    ImGui::Spacing();
    ImGui::Text(ICON_FA_GAMEPAD " Action Game Effects");
    ImGui::DragFloat(ICON_FA_EXCLAMATION_TRIANGLE " Damage Flash (被弾赤画面)", &params->damageFlash, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat(ICON_FA_WINDOW_MINIMIZE " Cinema Bar (黒帯)", &params->cinemaBarHeight, 0.005f, 0.0f, 0.5f);
    ImGui::DragFloat(ICON_FA_WAVE_SQUARE " Wobble (波打ち)", &params->wobbleIntensity, 0.001f, 0.0f, 0.1f);

    ImGui::Separator();

    // ==========================================================
    // レトロ効果
    // ==========================================================
    ImGui::Text(ICON_FA_TV " Retro Effects");
    ImGui::DragFloat(ICON_FA_BARS " Scanline (ブラウン管)", &params->scanlineIntensity, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat(ICON_FA_TH " Mosaic Size (ドット絵化)", &params->mosaicSize, 1.0f, 0.0f, 64.0f);
    ImGui::Spacing();
    ImGui::Text(ICON_FA_HEARTBEAT " Danger / Health Effects");
    ImGui::DragFloat(" Danger Vignette (瀕死赤枠)", &params->dangerVignette, 0.01f, 0.0f, 2.0f);
    ImGui::Spacing();
    ImGui::Text(ICON_FA_PALETTE " Color Effect");
    ImGui::DragFloat(" Grayscale (モノクロ)", &params->grayscaleIntensity, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat(" Sepia (セピア調)", &params->sepiaIntensity, 0.01f, 0.0f, 1.0f);
    ImGui::Spacing();
    ImGui::Text(ICON_FA_CLOUD " Blur Effects (資料)");
    ImGui::DragInt(" Box Filter Size (0:Off, 1:3x3...)", &params->boxFilterSize, 0.1f, 0, 10);
    ImGui::DragInt(" Gaussian Filter Size (0:Off, 1:3x3...)", &params->gaussianFilterSize, 0.1f, 0, 10);
    ImGui::DragFloat(" Gaussian Sigma", &params->gaussianSigma, 0.01f, 0.1f, 10.0f);
    ImGui::Spacing();
    ImGui::Text(ICON_FA_SQUARE " Outline Effects (資料)");
    ImGui::DragFloat(" Luminance Outline (輝度ベース)", &params->luminanceOutlineIntensity, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat(" Depth Outline (深度ベース)", &params->depthOutlineIntensity, 0.01f, 0.0f, 1.0f);
    ImGui::Spacing();
    ImGui::Text(ICON_FA_FIRE " Dissolve (資料)");
    ImGui::DragFloat(" Dissolve Threshold", &params->dissolveThreshold, 0.001f, 0.0f, 1.1f);
    ImGui::DragFloat(" Dissolve Edge Width", &params->dissolveEdgeWidth, 0.001f, 0.0f, 0.5f);
    ImGui::ColorEdit3(" Dissolve Edge Color", &params->dissolveEdgeColor.x);
    ImGui::Spacing();
    ImGui::Text(ICON_FA_DICE " Random (資料)");
    ImGui::DragFloat(" Random Intensity", &params->randomIntensity, 0.01f, 0.0f, 1.0f);
    ImGui::Separator();

    // ==========================================================
    // スライムフェードの設定
    // ==========================================================
    ImGui::Text(ICON_FA_WATER " Slime Fade Settings");
    ImGui::DragFloat(" Slime Intensity", &params->slimeFadeIntensity, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat(" Slime Density", &params->slimeDensity, 0.1f, 0.1f, 10.0f);
    ImGui::ColorEdit3(" Slime Color", &params->slimeColor.x);
    
    ImGui::Spacing();

    // Fade 本体の 1.5 秒の遷移を使って試写する。保存操作で調整値を書き出す処理とは独立している。
    if (ImGui::Button("Start Fade In")) {
        Fade::GetInstance()->StartFadeIn(1.5f);
    }

    ImGui::SameLine();

    // 閉じる試写ボタンでは、1.5秒のフェードアウトを開始する。
    if (ImGui::Button("Start Fade Out")) {
        Fade::GetInstance()->StartFadeOut(1.5f);
    }

    ImGui::Separator();

    // ==========================================================
    // アイリスフェードの設定
    // ==========================================================
    ImGui::Text(ICON_FA_DOT_CIRCLE " Iris Fade Settings");
    ImGui::DragFloat(" Iris Intensity", &params->irisFadeIntensity, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat2(" Iris Center", &params->irisCenterX, 0.01f, 0.0f, 1.0f);
    
    ImGui::Spacing();

    // 試写ボタンは Fade の既定中心（0.5、0.5）で 1 秒の開閉を始める。
    if (ImGui::Button("Start Iris In")) {
        Fade::GetInstance()->StartIrisIn(1.0f);
    }

    ImGui::SameLine();

    // 閉じる試写ボタンでは、画面中央へ 1 秒でアイリスを閉じる。
    if (ImGui::Button("Start Iris Out")) {
        Fade::GetInstance()->StartIrisOut(1.0f);
    }

    ImGui::Separator();

    // ==========================================================
    // セーブ・ロードボタン
    // ==========================================================
    if (ImGui::Button(ICON_FA_SAVE " Save JSON")) {
        SaveParams();
    }

    ImGui::SameLine();

    // 読込操作では、保存ファイルにあるキーを現在の表示設定へ重ねる。
    if (ImGui::Button(ICON_FA_UPLOAD " Load JSON")) {
        LoadParams();
    }
#endif
}

// 現在の後処理設定を指定の JSON に書き出す。
void PostEffectEditor::SaveParams(const std::string& filename) {
    // 対象が未設定なら書き出す値を取得できないため処理を止める。
    if (!targetEffect_)
        return;

    // 独立したファイル名を受け付けるため、親ディレクトリの指定は省略できる。
    std::filesystem::path dir = std::filesystem::path(filename).parent_path();

    // 親ディレクトリが指定され、まだ存在しない場合だけ作成する。
    if (!dir.empty() && !std::filesystem::exists(dir)) {
        std::filesystem::create_directories(dir);
    }

    auto* params = targetEffect_->GetParams();
    json j;
    // Editorで調整した描画パラメータと、全シーン共有の色空間設定を同じPresetへ保存する。
    j["bloomEnabled"] = targetEffect_->IsBloomEnabled();
    j["bloomQuality"] = static_cast<std::int32_t>(targetEffect_->GetBloomQuality());
    j["threshold"] = params->threshold;
    j["bloomIntensity"] = params->bloomIntensity;
    j["spread"] = params->spread;
    j["enableToneMapping"] = params->enableToneMapping;
    j["linearWorkflowEnabled"] = engine::colorSpace::WorkflowSettings::GetInstance().IsLinearWorkflowEnabled();
    j["vignetteIntensity"] = params->vignetteIntensity;
    j["vignettePower"] = params->vignettePower;
    j["chromaticAberration"] = params->chromaticAberration;
    j["filmGrainIntensity"] = params->filmGrainIntensity;
    j["radialIntensity"] = params->radialIntensity;
    j["radialBlurSamples"] = params->radialBlurSamples;
    j["radialCenterX"] = params->radialCenterX;
    j["radialCenterY"] = params->radialCenterY;
    j["lutIntensity"] = params->lutIntensity;
    j["colorExposure"] = params->colorExposure;
    j["colorContrast"] = params->colorContrast;
    j["colorSaturation"] = params->colorSaturation;
    j["colorTemperature"] = params->colorTemperature;
    j["colorTint"] = params->colorTint;
    j["damageFlash"] = params->damageFlash;
    j["cinemaBarHeight"] = params->cinemaBarHeight;
    j["wobbleIntensity"] = params->wobbleIntensity;
    j["scanlineIntensity"] = params->scanlineIntensity;
    j["mosaicSize"] = params->mosaicSize;
    j["grayscaleIntensity"] = params->grayscaleIntensity;
    j["sepiaIntensity"] = params->sepiaIntensity;
    j["boxFilterSize"] = params->boxFilterSize;
    j["gaussianFilterSize"] = params->gaussianFilterSize;
    j["gaussianSigma"] = params->gaussianSigma;
    j["luminanceOutlineIntensity"] = params->luminanceOutlineIntensity;
    j["depthOutlineIntensity"] = params->depthOutlineIntensity;
    j["dissolveThreshold"] = params->dissolveThreshold;
    j["dissolveEdgeWidth"] = params->dissolveEdgeWidth;
    j["dissolveEdgeColor"] = { params->dissolveEdgeColor.x, params->dissolveEdgeColor.y, params->dissolveEdgeColor.z };
    j["randomIntensity"] = params->randomIntensity;
    j["slimeFadeIntensity"] = params->slimeFadeIntensity;
    j["slimeDensity"] = params->slimeDensity;
    j["slimeColor"] = { params->slimeColor.x, params->slimeColor.y, params->slimeColor.z };
    j["irisFadeIntensity"] = params->irisFadeIntensity;
    j["irisCenter"] = { params->irisCenterX, params->irisCenterY };
    std::ofstream file(filename);

    if (file.is_open()) {
        file << j.dump(4); // インデント4で綺麗に出力
    }
}

// 指定の JSON を読み、存在するキーだけを現在の後処理設定へ反映する。
void PostEffectEditor::LoadParams(const std::string& filename) {
    // 後処理の適用先がまだない場合は読み込まない。
    if (!targetEffect_)
        return;

    std::ifstream file(filename);
    // 設定ファイルを開けない場合は、現在の設定で続行する。
    if (!file.is_open())
        return;

    try {
        json j;
        file >> j;

        // 旧形式にないキーは読込前の値を保つ。以下の各存在チェックは、この互換性のために行う。
        auto* params = targetEffect_->GetParams();
		// 有効フラグが記録されていれば GPU 処理の有無を変更する。
		if (j.contains("bloomEnabled")) {
			targetEffect_->SetBloomEnabled(j["bloomEnabled"].get<bool>());
		}

		// 品質の番号が記録されていれば有効フラグとは独立に復元する。
		if (j.contains("bloomQuality")) {
			targetEffect_->SetBloomQuality(
				static_cast<PostEffect::BloomQuality>(j["bloomQuality"].get<std::int32_t>()));
		}

        if (j.contains("linearWorkflowEnabled")) {
            // 色空間の変更は背景クリア色にも影響するため、LightManager側へ再反映する。
            engine::colorSpace::WorkflowSettings::GetInstance().SetLinearWorkflowEnabled(
                j["linearWorkflowEnabled"].get<bool>());
            engine::LightManager::GetInstance()->ApplySceneClearColor();
        }

        // Bloom の発光閾値・強度・ぼかし幅は、定義された項目だけ戻す。
        if (j.contains("threshold"))
            params->threshold = j["threshold"];
        // 強度が保存されている場合は、合成する Bloom の量を更新する。
        if (j.contains("bloomIntensity"))
            params->bloomIntensity = j["bloomIntensity"];
        if (j.contains("spread"))
            params->spread = j["spread"];
        // 後処理の切り替えと周辺減光・収差・ノイズの値を、含まれる項目ごとに復元する。
        if (j.contains("enableToneMapping"))
            params->enableToneMapping = j["enableToneMapping"];
        // 周辺減光の強さがあれば復元する。
        if (j.contains("vignetteIntensity"))
            params->vignetteIntensity = j["vignetteIntensity"];
        // 減光の丸みは独立項目のため、指定されている場合だけ更新する。
        if (j.contains("vignettePower"))
            params->vignettePower = j["vignettePower"];
        // 色のずれ幅が保存されている場合だけ、色収差の設定を更新する。
        if (j.contains("chromaticAberration"))
            params->chromaticAberration = j["chromaticAberration"];
        if (j.contains("filmGrainIntensity"))
            params->filmGrainIntensity = j["filmGrainIntensity"];
        // 集中線のサンプル数と画面上の中心を、保存された成分ごとに反映する。
        if (j.contains("radialIntensity"))
            params->radialIntensity = j["radialIntensity"];
        // サンプル数が指定されている場合は、集中線の描画に使う回数を復元する。
        if (j.contains("radialBlurSamples"))
            params->radialBlurSamples = j["radialBlurSamples"];
        // 中心の X があればその成分だけを復元する。Y が省略されている設定にも対応する。
        if (j.contains("radialCenterX"))
            params->radialCenterX = j["radialCenterX"];
        if (j.contains("radialCenterY"))
            params->radialCenterY = j["radialCenterY"];
        // 色調補正は LUT の適用量と色の調整値を個別に反映する。
        if (j.contains("lutIntensity"))
            params->lutIntensity = j["lutIntensity"];
        // 露出設定があれば、全体の明るさ補正を復元する。
        if (j.contains("colorExposure"))
            params->colorExposure = j["colorExposure"];
        // コントラスト設定があれば、明暗差の補正を復元する。
        if (j.contains("colorContrast"))
            params->colorContrast = j["colorContrast"];
        // 彩度設定があれば、色の鮮やかさの補正を復元する。
        if (j.contains("colorSaturation"))
            params->colorSaturation = j["colorSaturation"];
        // 色温度があれば、暖色・寒色への補正を復元する。
        if (j.contains("colorTemperature"))
            params->colorTemperature = j["colorTemperature"];
        if (j.contains("colorTint"))
            params->colorTint = j["colorTint"];
        // 被弾・黒帯・波打ちに続いて、走査線、モザイク、減色系の値を個別に反映する。
        if (j.contains("damageFlash"))
            params->damageFlash = j["damageFlash"];
        // 黒帯の高さが指定されている場合にだけ、帯の設定値を戻す。
        if (j.contains("cinemaBarHeight"))
            params->cinemaBarHeight = j["cinemaBarHeight"];
        // 波打ち量が保存されている場合だけ、画像の歪み設定を戻す。
        if (j.contains("wobbleIntensity"))
            params->wobbleIntensity = j["wobbleIntensity"];
        // 走査線の強度が保存されている場合だけ、その濃さを戻す。
        if (j.contains("scanlineIntensity"))
            params->scanlineIntensity = j["scanlineIntensity"];
        // モザイクの大きさが指定されている場合だけ、画素化の設定を戻す。
        if (j.contains("mosaicSize"))
            params->mosaicSize = j["mosaicSize"];
        // グレースケールの混合量が保存されている場合だけ復元する。
        if (j.contains("grayscaleIntensity"))
            params->grayscaleIntensity = j["grayscaleIntensity"];
        if (j.contains("sepiaIntensity"))
            params->sepiaIntensity = j["sepiaIntensity"];
        // ぼかしの半径・広がり、輝度と深度の輪郭強度を独立に反映する。
        if (j.contains("boxFilterSize"))
            params->boxFilterSize = j["boxFilterSize"];
        // ガウシアンぼかしの半径の指定があれば、ボックスぼかしとは独立に復元する。
        if (j.contains("gaussianFilterSize"))
            params->gaussianFilterSize = j["gaussianFilterSize"];
        // 分布の幅が指定されている場合にだけガウシアンの係数を差し替える。
        if (j.contains("gaussianSigma"))
            params->gaussianSigma = j["gaussianSigma"];
        // 輝度による輪郭の強さがあれば、深度の輪郭設定と独立に復元する。
        if (j.contains("luminanceOutlineIntensity"))
            params->luminanceOutlineIntensity = j["luminanceOutlineIntensity"];
        if (j.contains("depthOutlineIntensity"))
            params->depthOutlineIntensity = j["depthOutlineIntensity"];
        // 消失の進行と縁幅は、縁色の RGB 配列とは別の値として反映する。
        if (j.contains("dissolveThreshold"))
            params->dissolveThreshold = j["dissolveThreshold"];
        if (j.contains("dissolveEdgeWidth"))
            params->dissolveEdgeWidth = j["dissolveEdgeWidth"];
        // 色と中心は数値配列の保存順を維持する。既存の RGB と XY をそれぞれ同じ添字から戻す。
        if (j.contains("dissolveEdgeColor")) {
            params->dissolveEdgeColor.x = j["dissolveEdgeColor"][0];
            params->dissolveEdgeColor.y = j["dissolveEdgeColor"][1];
            params->dissolveEdgeColor.z = j["dissolveEdgeColor"][2];
        }

        // ノイズとスライムフェードの各強度・密度は独立して復元できる。
        if (j.contains("randomIntensity"))
            params->randomIntensity = j["randomIntensity"];
        // スライムフェードの強さが保存されている場合にだけ、密度と独立に復元する。
        if (j.contains("slimeFadeIntensity"))
            params->slimeFadeIntensity = j["slimeFadeIntensity"];
        if (j.contains("slimeDensity"))
            params->slimeDensity = j["slimeDensity"];
        // スライム色があれば、保存順の RGB 三成分を復元する。
        if (j.contains("slimeColor")) {
            params->slimeColor.x = j["slimeColor"][0];
            params->slimeColor.y = j["slimeColor"][1];
            params->slimeColor.z = j["slimeColor"][2];
        }

        // アイリスの強度と画面上の中心は独立した項目として復元する。
        if (j.contains("irisFadeIntensity"))
            params->irisFadeIntensity = j["irisFadeIntensity"];
        // 中心があれば保存順に X と Y を取り出す。
        if (j.contains("irisCenter")) {
            params->irisCenterX = j["irisCenter"][0];
            params->irisCenterY = j["irisCenter"][1];
        }

    }
    catch (...) {
        // 読込失敗を呼び出し元へ通知しません。途中まで適用済みの設定もここでは巻き戻しません。
    }
}
