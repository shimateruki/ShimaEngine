#include <cstdint>
#include "MeshEffectEditor.h"
#include <numbers>
#include "ModelManager.h"
#include "TextureManager.h"
#include "SceneManager.h" 
#include "BaseScene.h"    
#include <imgui.h>
#include <fstream>
#include "json.hpp" // プロジェクト内のJSONライブラリ
#include <DebugConsole.h>
#include "IconsFontAwesome5.h"
#include <filesystem>
#include <algorithm>
#include <MeshEffectManager.h>
#include "engine/graphics/effect/MeshEffectTarget.h"
#include "EffectPreviewStage.h"
#include "EditorManager.h"
#include "VfxAssetPicker.h"
#include "VfxAssetCandidates.h"

using json = nlohmann::json;
static const char* kEasingNames[] = {
    "Linear (等速)",
    "InSine", "OutSine", "InOutSine",
    "InQuad", "OutQuad", "InOutQuad",
    "InCubic", "OutCubic", "InOutCubic",
    "InQuart", "OutQuart", "InOutQuart",
    "InQuint", "OutQuint", "InOutQuint",
    "InExpo", "OutExpo", "InOutExpo",
    "InCirc", "OutCirc", "InOutCirc",
    "InBack", "OutBack", "InOutBack",
    "InElastic", "OutElastic", "InOutElastic",
    "InBounce", "OutBounce", "InOutBounce"
};

namespace {
// 追加レイヤーは本体と同じ形状を使い、当たり判定の設定はレイヤーごとに保持する。
void SyncAdditionalPreviewMesh(EffectObject3D& layer, const EffectObject3D::MeshSettings& shape, std::int32_t type) {
    const auto ownSettings = layer.GetMeshSettings();
    auto settings = shape;
    settings.hasCollision = ownSettings.hasCollision;
    settings.collisionShape = ownSettings.collisionShape;
    settings.collisionSize = ownSettings.collisionSize;
    settings.collisionOffset = ownSettings.collisionOffset;
    layer.SetMeshSettings(settings);
    layer.SetProceduralType(type);
    layer.UpdateProceduralMesh();
}

// 本体の形状を確定してから、所有する追加レイヤーを同じ形状で作り直します。
void SyncPreviewMesh(EffectObject3D& preview,
    const std::vector<std::unique_ptr<EffectObject3D>>& layers, std::int32_t type) {
    preview.SetProceduralType(type);
    preview.UpdateProceduralMesh();
    for (const auto& layer : layers) {
        SyncAdditionalPreviewMesh(*layer, preview.GetMeshSettings(), type);
    }
}
}

void MeshEffectEditor::Initialize(SceneManager* sceneManager) {
    // プレビューが使うシーン管理を保持し、画像の選択候補を先に読み込みます。
    sceneManager_ = sceneManager;
    RefreshTextureList();
}

void MeshEffectEditor::RefreshTextureList() {
    // 画像候補を収集し直し、相対パスで保持して選択欄の順序と索引を揃えます。
    textureFileList_.clear();
    std::string path = "Resources/sprite/"; // テクスチャフォルダのパス

    // 指定フォルダ内のファイルを走査
    for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
        // ディレクトリなどはテクスチャ候補に登録しません。
        if (!entry.is_regular_file()) {
            continue;
        }

        const std::string ext = entry.path().extension().string();

        // 選択欄で扱うPNG・DDS・JPGの拡張子だけを登録します。
        if (ext != ".png" && ext != ".dds" && ext != ".jpg") {
            continue;
        }

        textureFileList_.push_back(std::filesystem::relative(entry.path(), path).generic_string());
    }

    std::sort(textureFileList_.begin(), textureFileList_.end());

    // 画像候補が空の場合は、どの項目も選べないよう索引を無効にします。
    if (textureFileList_.empty()) {
        currentTextureIndex_ = -1;
    }
    // 候補があれば、編集パスと同じ項目を探します。
    else {
        // 現在の editTexturePath_ に一致するものを探してインデックスを合わせる
        currentTextureIndex_ = 0; // デフォルト

        for (std::int32_t i = 0; i < textureFileList_.size(); ++i) {
            std::string fullPath = "Resources/sprite/" + textureFileList_[i];
            // 相対パス、素材フォルダーから始まるパス、またはファイル名が一致した最初の候補を選びます。
            if (textureFileList_[i] == editTexturePath_ ||
                fullPath == editTexturePath_ ||
                std::filesystem::path(textureFileList_[i]).filename() == std::filesystem::path(editTexturePath_).filename()) {
                currentTextureIndex_ = i;
                break;
            }
        }
    }

    SyncTextureIndices();
}

void MeshEffectEditor::Update(float deltaTime) {
    // シーン管理が無ければプレビューの描画基盤を取得できないため、更新しません。
    if (!sceneManager_)
        return;

    // 切替中は旧シーンの基盤を参照するプレビューを破棄し、新規生成も待つ。
    if (sceneManager_->IsTransitioning()) {
        previewEffect_.reset();
        extraPreviewEffects_.clear();
        targetObject_ = nullptr;
        lastScene_ = nullptr;
        return;
    }

    BaseScene* currentScene = sceneManager_->GetCurrentScene();

    // 現在のシーンが切り替わった場合は、以前の描画基盤や追従先を引き継ぎません。
    if (lastScene_ != currentScene) {
        // Scene切替後は借用していた描画基盤と対象Objectを再利用しない。
        previewEffect_.reset();
        extraPreviewEffects_.clear();
        lastScene_ = currentScene;
        targetObject_ = nullptr;
    }

    engine::Object3DCommon* objectCommon = currentScene ? currentScene->GetObject3DCommon() : nullptr;

    // 本体プレビューが未作成で、新しいシーンの描画基盤が取得できたときに生成します。
    if (!previewEffect_ && objectCommon) {
        auto previewEffect = std::make_unique<EffectObject3D>();
        previewEffect->Initialize(objectCommon);

        // 描画用の材質を準備できた個体だけを保持し、次の更新から再生対象にする。
        if (!previewEffect->GetMaterialData()) {
            return;
        }

        previewEffect_ = std::move(previewEffect);
        previewEffect_->SetModel(editModelName_);

        // 描画実体がある場合に画像設定を反映できます。
        if (auto renderer = previewEffect_->GetMeshRenderer()) {
            // 画像パスが空でない場合だけ読み込みを依頼します。
            if (strlen(editTexturePath_) > 0)
                renderer->SetTexture(editTexturePath_);
        }

        previewEffect_->SetColor(editColor_);
        previewEffect_->SetScrollSpeed(editScrollSpeed_);
        previewEffect_->SetIntensity(editIntensity_);
        previewEffect_->SetUseTextureColor(editUseTextureColor_);
        previewEffect_->SetChantFlowStrength(editChantFlowStrength_);
        previewEffect_->SetChantRibbonStrength(editChantRibbonStrength_);
        previewEffect_->SetRingEnergyStrength(editRingEnergyStrength_);
        previewEffect_->SetRingSpinSpeed(editRingSpinSpeed_);
    }

    // 本体プレビューを用意できるまでは、レイヤーの同期と再生を進めません。
    if (!previewEffect_)
        return;

    std::int32_t neededExtras = 0;
    // 十字表示では本体と交差する面を1枚追加します。
    if (editVolumeMode_ == 1)
        neededExtras = 1;
    // 三枚重ねでは本体の前後に配置する面を2枚追加します。
    else if (editVolumeMode_ == 2) neededExtras = 2;

    // 要求された枚数に達するまで、描画できる追加レイヤーを作成します。
    while (extraPreviewEffects_.size() < neededExtras) {
        auto extra = std::make_unique<EffectObject3D>();

        // 共通基盤が無ければ、新しい面の作成を打ち切ります。
        if (!objectCommon) {
            break;
        }

        extra->Initialize(objectCommon);

        // 描画用の材質を確保できない面は、レイヤーとして登録しません。
        if (!extra->GetMaterialData()) {
            break;
        }

        // 立体化を後から有効にした場合も、作成した面に本体の形状を渡す。
        if (editProceduralType_ >= 1) {
            SyncAdditionalPreviewMesh(*extra, previewEffect_->GetMeshSettings(), editProceduralType_);
        }
        extraPreviewEffects_.push_back(std::move(extra));
    }

    // モード変更によって余った面は破棄します。
    while (extraPreviewEffects_.size() > neededExtras) {
        extraPreviewEffects_.pop_back();
    }

    std::vector<EffectObject3D*> activePreviews;
    activePreviews.push_back(previewEffect_.get());

    for (auto& ex : extraPreviewEffects_) {
        // 作成済みで材質が使える面だけを、本体と一緒に更新する一覧へ加えます。
        if (ex && ex->GetMaterialData()) {
            activePreviews.push_back(ex.get());
        }
    }

    EffectPreviewStage* previewStage = EffectPreviewStage::GetInstance();
    const bool isThisEditorSelected = EditorManager::GetInstance()->GetSelectedObject() == this;
    bool usePreviewStage = previewStage && previewStage->IsEnabled() && isThisEditorSelected;

    // 専用ステージで新しい再生要求を受け取ったら、全レイヤーの再生を後段で始めます。
    if (usePreviewStage && previewStage->GetPlayRequestSerial() != lastStagePlayRequestSerial_) {
        lastStagePlayRequestSerial_ = previewStage->GetPlayRequestSerial();
        forcePlayRequest_ = true;
    }

    // 専用ステージで新しい停止要求を受け取ったら、全レイヤーを寿命の終端にそろえます。
    if (usePreviewStage && previewStage->GetStopRequestSerial() != lastStageStopRequestSerial_) {
        lastStageStopRequestSerial_ = previewStage->GetStopRequestSerial();

        for (auto* fx : activePreviews) {
            fx->Play(editLifetime_);
            // 停止時は先頭から寿命分を進め、各レイヤーを同じ終了状態へ揃える。
            fx->Update(editLifetime_);
        }

        forcePlayRequest_ = false;
    }

    // 専用ステージで新しいシーク要求を受け取ったら、指定時刻へ各レイヤーをそろえます。
    if (usePreviewStage && previewStage->GetSeekRequestSerial() != lastStageSeekRequestSerial_) {
        // 時刻を戻すときは再生を先頭から作り直し、過去フレームの残像を残さない。
        lastStageSeekRequestSerial_ = previewStage->GetSeekRequestSerial();
        const float seekTime = std::clamp(previewStage->GetSeekTargetTime(), 0.0f, (std::max)(editLifetime_, 0.01f));

        for (auto* fx : activePreviews) {
            fx->Play(editLifetime_);
            fx->Update(seekTime);
        }
    }

    // 専用ステージでは編集座標を原点からのオフセットとして扱う。
    engine::Vector3 basePos = usePreviewStage ? previewStage->GetPreviewPosition() : editPosition_;

    // 専用ステージ中は、原点へ編集された位置の成分を足します。
    if (usePreviewStage) {
        basePos.x += editPosition_.x;
        basePos.y += editPosition_.y;
        basePos.z += editPosition_.z;
    }

    engine::Vector3 baseRot = editRotation_;
    float targetWorldY = 0.0f;

    // 追従先が指定されていれば、ステージや固定座標の代わりに対象のワールド座標を基準にします。
    if (targetObject_) {
        engine::Vector3 targetPos = targetObject_->GetWorldPosition();

        // 階層を一番上まで辿り、子自身の姿勢から独立した水平向きを求めます。
        engine::Object3D* rootObj = targetObject_;

        while (rootObj && rootObj->GetParent()) {
            rootObj = rootObj->GetParent(); // 一番上の親（プレイヤー本体など）まで遡る
        }

        // 最上位の実体があれば、そのY回転をオフセットの向きに使います。
        if (rootObj) {
            targetWorldY = rootObj->GetRotation().y;
        }

        // ローカルの位置オフセットを最上位のY回転で水平回転してから対象に足します。
        float s = sinf(targetWorldY);
        float c = cosf(targetWorldY);
        engine::Vector3 rotatedOffset;
        rotatedOffset.x = editPosition_.x * c + editPosition_.z * s;
        rotatedOffset.y = editPosition_.y;
        rotatedOffset.z = -editPosition_.x * s + editPosition_.z * c;

        basePos.x = targetPos.x + rotatedOffset.x;
        basePos.y = targetPos.y + rotatedOffset.y;
        basePos.z = targetPos.z + rotatedOffset.z;

        // 編集回転にも最上位のY回転を加え、位置の基準と向きを合わせます。
        baseRot = editRotation_;
        baseRot.y += targetWorldY;
    }

    // 各レイヤーが同時に始まるよう、再生要求とループの終端を共通で確認します。
    bool loopPreview = isAutoLoop_ ||
        (usePreviewStage && previewStage->IsLoopEnabled() && previewStage->IsTransportPlaying());
    // 明示的な再生要求があるか、ループが有効で先頭レイヤーの再生が停止している場合に全レイヤーを再生します。
    if (forcePlayRequest_ || (loopPreview && !activePreviews.empty() && !activePreviews[0]->IsPlaying())) {
        for (auto* fx : activePreviews) {
            fx->Play(editLifetime_);
        }

        forcePlayRequest_ = false; // フラグを消化
    }

    // 追加レイヤーを前後にずらすため、回転後のローカルZ軸を計算します。
    engine::Vector3 localZ;
    localZ.x = sinf(baseRot.y) * cosf(baseRot.x);
    localZ.y = -sinf(baseRot.x);
    localZ.z = cosf(baseRot.y) * cosf(baseRot.x);

    float timeStep = (deltaTime <= 0.0001f) ? (1.0f / 60.0f) : deltaTime;

    // 専用ステージ中は、更新秒数にタイムラインの再生速度を掛けます。
    if (usePreviewStage) {
        timeStep *= previewStage->GetPlaybackSpeed();
    }

    for (size_t i = 0; i < activePreviews.size(); ++i) {
        auto* fx = activePreviews[i];

        // 更新対象が無効か材質を取得できない場合、その面の設定と描画更新を飛ばします。
        if (!fx || !fx->GetMaterialData()) {
            continue;
        }

        // 生成形状を使わない場合に、選択した外部モデルを割り当てます。
        if (editProceduralType_ == 0) {
            fx->SetModel(editModelName_);
        }

        // 描画実体がある面に、画像指定を適用します。
        if (auto renderer = fx->GetMeshRenderer()) {
            // 編集パスが空でなければ、テクスチャを割り当てます。
            if (strlen(editTexturePath_) > 0)
                renderer->SetTexture(editTexturePath_);
        }

        // 全レイヤーに共通の色、拡縮、材質アニメーションを反映します。
        fx->SetBlendMode(static_cast<BlendMode>(currentBlendModeIndex_));
        fx->SetStartScale(editStartScale_);
        fx->SetEndScale(editEndScale_);
        fx->SetStartColor(editStartColor_);
        fx->SetEndColor(editEndColor_);
        fx->SetScrollSpeed(editScrollSpeed_);
        fx->SetIntensity(editIntensity_);
        fx->SetUseTextureColor(editUseTextureColor_);
        fx->SetChantFlowStrength(editChantFlowStrength_);
        fx->SetChantRibbonStrength(editChantRibbonStrength_);
        fx->SetRingEnergyStrength(editRingEnergyStrength_);
        fx->SetRingSpinSpeed(editRingSpinSpeed_);

        fx->SetDistortionStrength(editDistortionStrength_);
        fx->SetDistortionSpeed(editDistortionSpeed_);
        fx->SetEdgeFadeStrength(editEdgeFadeStrength_);
        fx->SetEnableDistortion(editEnableDistortion_);
        fx->SetEnableReveal(editEnableReveal_);
        fx->SetRevealMode(editEnableReveal_ ? editRevealMode_ : 0);
        fx->SetEasingType(editEasingType_);
        fx->SetProceduralType(editProceduralType_);
        fx->SetAlphaReference(editAlphaReference_);
        bool useRamp = (strlen(editRampTexturePath_) > 0);
        bool useNoise = (strlen(editNoiseTexturePath_) > 0);
        fx->SetEnableColorRamp(useRamp);
        fx->SetEnableNoiseTexture(useNoise);

        // ノイズのパスが指定されている場合に、画像データを取得します。
        if (useNoise)
            fx->SetNoiseTexture(engine::TextureManager::GetInstance()->Load(editNoiseTexturePath_));
        // カラーランプのパスが指定されている場合に、画像データを取得します。
        if (useRamp)
            fx->SetRampTexture(engine::TextureManager::GetInstance()->Load(editRampTexturePath_));

        engine::Vector3 finalPos = basePos;
        engine::Vector3 finalRot = baseRot;

        // 十字は2枚目を90度傾け、3枚重ねは追加面をローカルZ方向の前後にずらす。
        if (editVolumeMode_ == 1 && i == 1) {
            finalRot.x += (std::numbers::pi_v<float> * 0.5f);
        }
        // 三枚重ねの場合だけ、追加面をそれぞれ手前と奥へずらします。
        else if (editVolumeMode_ == 2) {
            float gap = 0.02f;

            // 最初の追加面をローカルZの正方向へ0.02単位移動します。
            if (i == 1) {
                finalPos.x += localZ.x * gap;
                finalPos.y += localZ.y * gap;
                finalPos.z += localZ.z * gap;
            }
            // 二枚目の追加面をローカルZの負方向へ0.02単位移動します。
            if (i == 2) {
                finalPos.x -= localZ.x * gap;
                finalPos.y -= localZ.y * gap;
                finalPos.z -= localZ.z * gap;
            }
        }

        fx->SetTranslate(finalPos);
        fx->SetRotation(finalRot);

        // 編集値を適用して再生状態を進め、その結果から描画行列を作る。
        fx->Update(timeStep);
        fx->UpdateLocalMatrix();
        fx->UpdateWorldMatrix();
    }

    // 専用ステージへ表示している場合は、再生位置とトラック情報をタイムラインへ渡します。
    if (usePreviewStage) {
        const float duration = (std::max)(editLifetime_, 0.01f);
        // 本体レイヤーの時計をタイムラインに渡し、追加面は表示中の枚数にだけ反映する。
        const float currentTime = previewEffect_ ?
            std::clamp(previewEffect_->GetCurrentTime(), 0.0f, duration) : 0.0f;
        std::vector<EffectPreviewStage::TimelineEvent> events;
        events.push_back({ "Scale / color", 0.0f, duration, engine::Vector4{ 0.9f, 0.45f, 1.0f, 1.0f } });
        events.push_back({ "Reveal", 0.0f, duration, engine::Vector4{ 0.35f, 0.8f, 1.0f, 1.0f } });
        previewStage->ReportToolState(
            EffectPreviewStage::ToolKind::kMeshEffect,
            "Mesh Effect",
            currentTime,
            duration,
            previewStage->IsTransportPlaying(),
            static_cast<std::int32_t>(activePreviews.size()),
            events);
    }
}

void MeshEffectEditor::Draw() {
    // シーン管理が無い場合や切り替え中は、旧シーンの描画基盤に触れません。
    if (!sceneManager_ || sceneManager_->IsTransitioning()) {
        return;
    }

    BaseScene* currentScene = sceneManager_->GetCurrentScene();

    // 有効な現在シーンで最後に更新したプレビューだけを描画します。
    if (lastScene_ != currentScene || !currentScene) {
        return;
    }

    // 本体が用意できたシーンでは、描画できる本体と追加面を順に処理します。
    if (previewEffect_) {
        // 本体の材質が用意できている場合に、本体を描きます。
        if (previewEffect_->GetMaterialData()) {
            previewEffect_->Draw();
        }

        for (auto& ex : extraPreviewEffects_) {
            // 作成済みで材質を持つ追加面だけを描きます。
            if (ex && ex->GetMaterialData()) {
                ex->Draw();
            }
        }
    }
    else {
        // 現在シーンで本体が作成されていない場合は、描画できない原因をログに残します。
        DebugConsole::GetInstance()->AddLog(LogLevel::kError, "Error: previewEffect_ is NULL!");
    }
}

void MeshEffectEditor::DrawImGui() {
#ifdef USE_IMGUI
    // 編集対象の本体がまだ無ければ、表示や保存でその設定に触れません。
    if (!previewEffect_)
        return;

    // ウィンドウの横幅を取得
    float availWidth = ImGui::GetContentRegionAvail().x;

    // 再生要求や時間リセットを、本体と追加レイヤーの両方へ反映します。
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));

    // 再生ボタンを押したら、次の Update() へ再生要求を渡します。
    if (ImGui::Button(ICON_FA_PLAY " エフェクト再生 (ATTACK!)", ImVec2(availWidth, 40))) {
        forcePlayRequest_ = true;
    }

    ImGui::PopStyleColor();

    ImGui::Checkbox("自動ループ (Auto Loop)", &isAutoLoop_);
    ImGui::SameLine();

    // 時間リセットを押したら、本体と全追加面の時計を同じ位置へ戻します。
    if (ImGui::Button(ICON_FA_UNDO " 時間リセット")) {
        previewEffect_->ResetTime();

        for (auto& ex : extraPreviewEffects_)
            ex->ResetTime();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Separator();
    ImGui::Text(ICON_FA_CROSSHAIRS " --- ターゲット追従 (Target Tracking) ---");

    // 現在のシーンがある場合だけ、追従先の候補をシーンの所有オブジェクトから集めます。
    if (sceneManager_ && sceneManager_->GetCurrentScene()) {
        auto& objects = sceneManager_->GetCurrentScene()->GetObjects();

        // 現在選ばれているターゲットの名前
        std::string currentTargetName = targetObject_ ? targetObject_->GetName() : "なし (None)";

        // 追従先の選択欄を開いたときに、未指定と有効な対象を並べます。
        if (ImGui::BeginCombo("追従対象", currentTargetName.c_str())) {
            // 「追従解除（None）」の選択肢
            // 未指定が選ばれたら、対象への借用参照を解除します。
            if (ImGui::Selectable("なし (None)", targetObject_ == nullptr)) {
                targetObject_ = nullptr;
            }

            // 通常の配置物を候補にし、無効な参照とエディター内部用の実体は除きます。
            for (auto& obj : objects) {
                if (!obj || obj->IsEditorInternal())
                    continue;
                bool isSelected = (targetObject_ == obj.get());

                // 新しい対象が選ばれたら、以降の位置と向きの基準にします。
                if (ImGui::Selectable(obj->GetName().c_str(), isSelected)) {
                    targetObject_ = obj.get();
                }

                // 現在の選択に対応する項目へ、コンボを開いたときのフォーカスを合わせます。
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }

            ImGui::EndCombo();
        }
    }

    ImGui::Separator();

    // ブレンドや画像RGBの設定を変更したフレームに、本体へ即時反映します。
    const char* blendModeNames[] = { "なし (None)", "通常 (半透明)", "加算発光 (Add)", "減算 (Subtract)", "乗算 (Multiply)", "スクリーン (Screen)" };

    // ブレンドモードを変更したときは、本体へ設定して次の描画に反映します。
    if (ImGui::Combo(ICON_FA_ADJUST " ブレンドモード", &currentBlendModeIndex_, blendModeNames, IM_ARRAYSIZE(blendModeNames))) {
        previewEffect_->SetBlendMode(static_cast<BlendMode>(currentBlendModeIndex_));
    }

    // 画像RGBの選択を切り替えたら、本体の材質設定にも反映します。
    if (ImGui::Checkbox("画像のRGBを反映", &editUseTextureColor_)) {
        previewEffect_->SetUseTextureColor(editUseTextureColor_);
    }

    ImGui::Separator();
    ImGui::Text(ICON_FA_CUBES " [ 立体化モード (Volume Mode) ]");
    const char* volumeModes[] = { "0: なし (None)", "1: 十字クロス (Cross)", "2: 3枚重ね (Layer)" };
    ImGui::Combo("立体化", &editVolumeMode_, volumeModes, IM_ARRAYSIZE(volumeModes));

    ImGui::Checkbox(ICON_FA_RULER_HORIZONTAL " 伸びるアニメーション (Reveal Mode)", &editEnableReveal_);

    // 伸びる演出が有効な場合だけ、伸びる方向を選べるようにします。
    if (editEnableReveal_) {
        const char* revealModes[] = { "なし", "横方向", "円周方向" };
        ImGui::Combo("表示の組み立て方", &editRevealMode_, revealModes, IM_ARRAYSIZE(revealModes));
    }

    // イージングが変更されたら、本体の時間補間を更新します。
    if (ImGui::Combo(ICON_FA_CHART_LINE " イージング (Easing)", &editEasingType_, kEasingNames, IM_ARRAYSIZE(kEasingNames))) {
        previewEffect_->SetEasingType(editEasingType_);
    }

    ImGui::Separator();
    const char* procTypes[] = {
        "0: 外部モデル (Tex)",
        "1: 斜め切り (Slash)",
        "2: 回転切り (Spin)",
        "3: 突き (Thrust)",
        "4: 球 (Sphere)",
        "5: 円柱 (Cylinder)",
        "6: 箱 (Box)",
        "7: 平面 (Plane)",
        "8: トーラス (Torus)",
        "9: 円錐 (Cone)",
        "10: リング (Ring)",
        "11: 三角形 (Triangle)",
        "12: 槍 (Lance)",
        "13: 地表の全域放電",
        "14: 空間の雷撃バースト" };

    // 形状の選択が変わった場合は、本体と追加レイヤーを同じ形へ再構成します。
    if (ImGui::Combo("エフェクト形状", &editProceduralType_, procTypes, IM_ARRAYSIZE(procTypes))) {
        // 回転切りで既存の角度が一周未満なら、初期角度を400度にして切れ目が出にくくします。
        if (editProceduralType_ == 2 && previewEffect_->GetMeshSettings().slashAngle < 360.0f) {
            previewEffect_->SetMeshSetting(&EffectObject3D::MeshSettings::slashAngle, 400.0f);
        }

        SyncPreviewMesh(*previewEffect_, extraPreviewEffects_, editProceduralType_);
    }

    // プロシージャル形状を使っている場合にだけ、形状の寸法とOBJ出力を表示します。
    if (editProceduralType_ >= 1) {
        ImGui::Indent();
        bool changed = false;
        auto meshSettings = previewEffect_->GetMeshSettings();

        // SlashおよびSpinの両方でパラメータ調整を可能にする
        if (editProceduralType_ == 1 || editProceduralType_ == 2) {
            // 斜め切りと回転切りは、30〜1080度の弧とその太さ・ねじれを調整します。
            changed |= ImGui::SliderFloat("斬撃の角度", &meshSettings.slashAngle, 30.0f, 1080.0f);
            changed |= ImGui::SliderFloat("内側の半径", &meshSettings.innerRadius, 0.0f, 5.0f);
            changed |= ImGui::SliderFloat("外側の半径", &meshSettings.outerRadius, 1.0f, 15.0f);
            changed |= ImGui::SliderFloat("軌跡の厚み", &meshSettings.thickness, 0.01f, 3.0f);
            changed |= ImGui::SliderFloat("螺旋の高さ(Zズレ)", &meshSettings.spiralPitch, -5.0f, 5.0f);
        }
        // 突き形状では、突きの長さと根元の半径を調整します。
        else if (editProceduralType_ == 3) {
            changed |= ImGui::SliderFloat("突きの長さ", &meshSettings.thrustLength, 1.0f, 20.0f);
            changed |= ImGui::SliderFloat("根元の太さ", &meshSettings.thrustRadius, 0.1f, 5.0f);
        }
        // 球形状では、半径と縦方向の分割数を調整します。
        else if (editProceduralType_ == 4) {
            changed |= ImGui::SliderFloat("半径", &meshSettings.sphereRadius, 0.1f, 20.0f);
            changed |= ImGui::SliderInt("縦の分割数", &meshSettings.sphereRings, 4, 64);
        }
        // 円柱形状では、底面の半径と高さを調整します。
        else if (editProceduralType_ == 5) {
            changed |= ImGui::SliderFloat("半径", &meshSettings.cylinderRadius, 0.1f, 20.0f);
            changed |= ImGui::SliderFloat("高さ", &meshSettings.cylinderHeight, 0.1f, 20.0f);
        }

        // 箱形状では、幅・高さ・奥行きを調整します。
        else if (editProceduralType_ == 6) {
            changed |= ImGui::DragFloat3("サイズ (W,H,D)", &meshSettings.boxSize.x, 0.1f);
        }
        // 平面と二種類の放電形状では、幅と奥行きの入力を共有します。
        else if (editProceduralType_ == 7 || editProceduralType_ == 13 || editProceduralType_ == 14) {
            changed |= ImGui::DragFloat2("サイズ (W,D)", &meshSettings.planeSize.x, 0.1f);
        }
        // トーラス形状では、輪の主半径・管の半径・断面の分割を調整します。
        else if (editProceduralType_ == 8) {
            changed |= ImGui::SliderFloat("主半径 (全体サイズ)", &meshSettings.torusMajorRadius, 0.1f, 20.0f);
            changed |= ImGui::SliderFloat("副半径 (太さ)", &meshSettings.torusMinorRadius, 0.01f, 5.0f);
            changed |= ImGui::SliderInt("断面の分割数", &meshSettings.sphereRings, 3, 64);
        }
        // 円錐と槍では、根元の半径と先端までの高さを調整します。
        else if (editProceduralType_ == 9 || editProceduralType_ == 12) {
            changed |= ImGui::SliderFloat("半径", &meshSettings.coneRadius, 0.1f, 20.0f);
            changed |= ImGui::SliderFloat("高さ", &meshSettings.coneHeight, 0.1f, 20.0f);
        }
        // リング形状では、内側と外側の半径を調整します。
        else if (editProceduralType_ == 10) {
            changed |= ImGui::SliderFloat("外側の半径", &meshSettings.ringOuterRadius, 0.1f, 20.0f);
            changed |= ImGui::SliderFloat("内側の半径", &meshSettings.ringInnerRadius, 0.0f, 19.9f);
        }
        // 三角形状では、形状全体の大きさを調整します。
        else if (editProceduralType_ == 11) {
            changed |= ImGui::SliderFloat("サイズ", &meshSettings.triangleSize, 0.1f, 20.0f);
        }

        // 分割数を持たない形状(箱、三角形)以外で表示
        if (editProceduralType_ != 6 && editProceduralType_ != 11) {
            changed |= ImGui::SliderInt("基本ポリゴン分割数", &meshSettings.meshSegments, 3, 128);
        }

        ImGui::Spacing();
        changed |= ImGui::DragFloat2("UVタイリング (Repeat)", &meshSettings.uvTiling.x, 0.05f);

        previewEffect_->SetMeshSettings(meshSettings);

        // スライダー操作で形状が変わったときだけ、本体と追加面を一緒に再生成する。
        if (changed) {
            SyncPreviewMesh(*previewEffect_, extraPreviewEffects_, editProceduralType_);
        }

        ImGui::Unindent();

        // 外部OBJエクスポート機能
        ImGui::Spacing();
        static char sObjName[128] = "my_custom_primitive";
        ImGui::InputText("OBJファイル名", sObjName, sizeof(sObjName));

        // OBJ出力を押した場合に、現在の生成メッシュを入力名で保存します。
        if (ImGui::Button(ICON_FA_DOWNLOAD " この形状をOBJとして保存 (Export)", ImVec2(availWidth, 30))) {
            std::filesystem::create_directories("Resources/model");
            std::string path = "Resources/model/" + std::string(sObjName) + ".obj";
            previewEffect_->ExportToObj(path);
            DebugConsole::GetInstance()->AddLog("Exported OBJ to " + path);
        }
    }

    // 表示中の形状に応じて、外部モデルと画像の選択候補を出し分けます。
    if (ImGui::CollapsingHeader(ICON_FA_CUBES " リソース設定 (Resources)", ImGuiTreeNodeFlags_DefaultOpen)) {

        // 共通の画像選択では、項目が変わったときだけパスを保存し反映用の処理を呼びます。
        auto textureCombo = [&](const char* label, std::int32_t& currentIndex, char* targetPath,
                                char* query, auto callback) {
            if (vfxAssetPicker::DrawCombo(label, textureFileList_, currentIndex,
                                          query, 128, "なし (None)", true)) {
                const std::string fullPath = currentIndex >= 0
                    ? "Resources/sprite/" + textureFileList_[currentIndex] : "";
                strncpy_s(targetPath, 256, fullPath.c_str(), _TRUNCATE);
                callback(fullPath);
            }
        };

        // 外部モデルを使う設定の場合だけ、モデルの選択を受け付けます。
        if (editProceduralType_ == 0) {
            static vfxAssetCandidates::CandidateList modelCandidates;
            const auto& modelNames = modelCandidates.Get(vfxAssetCandidates::ModelName,
                ModelManager::GetInstance()->GetLoadedModelNames());
            std::int32_t selectedModel = -1;

            for (std::int32_t i = 0; i < static_cast<std::int32_t>(modelNames.size()); ++i) {
                // 現在のモデル名に一致する候補を見つけたら、コンボの初期選択にします。
                if (modelNames[i] == editModelName_) {
                    selectedModel = i;
                    break;
                }
            }

            // 別のモデルを選んだら、編集値とプレビューのモデルを揃えます。
            if (vfxAssetPicker::DrawCombo(ICON_FA_CUBE " メッシュ選択", modelNames,
                                          selectedModel, modelSearch_, sizeof(modelSearch_))) {
                strcpy_s(editModelName_, sizeof(editModelName_), modelNames[selectedModel].c_str());
                previewEffect_->SetModel(editModelName_);
            }

            // 現在のモデルが候補一覧になければ、文字情報で現在値を示します。
            if (selectedModel < 0)
                ImGui::TextDisabled("現在のモデル: %s", editModelName_);
        }
        else {
            // 生成形状を使う場合は、外部モデル欄が利用できないことを示します。
            ImGui::TextDisabled(ICON_FA_CUBE " メッシュ選択 (無効)");
            ImGui::TextDisabled("※プロシージャル生成を使用しているためモデル選択は不要です");
        }

        // メイン画像を更新し、描画実体がある場合はその場で割り当てます。
        textureCombo(ICON_FA_IMAGE " メインテクスチャ", currentTextureIndex_, editTexturePath_, textureSearch_[0], [&](const std::string& path) {
            // 本体の描画実体を取得できた場合だけ、メイン画像のパスを渡します。
            if (auto renderer = previewEffect_->GetMeshRenderer()) {
                renderer->SetTexture(path);
            }
            });
        // 生成形状でもメイン画像を適用できることを表示します。
        if (editProceduralType_ != 0) {
            ImGui::TextDisabled("プロシージャル形状にも発光テクスチャやストリークを貼れます。");
        }

        // ノイズを自動生成する斜め切り以外では、外部ノイズ画像を設定できます。

        // 形状が斜め切り以外なら、外部ノイズ画像を選べます。
        if (editProceduralType_ != 1) {
            textureCombo(ICON_FA_WIND " ノイズテクスチャ", currentNoiseTextureIndex_, editNoiseTexturePath_, textureSearch_[1], [&](const std::string& path) {
                uint32_t handle = path.empty() ? 0 : engine::TextureManager::GetInstance()->Load(path);
                previewEffect_->SetNoiseTexture(handle);
                });
        }
        // 斜め切りではノイズの選択を出さず、自動生成を使うことを示します。
        else {
            ImGui::TextDisabled(ICON_FA_WIND " ノイズテクスチャ (無効)");
            ImGui::TextDisabled("※プロシージャルで斬撃の筋(ノイズ)を自動生成しています");
        }

        // カラーランプの指定を変えたら、テクスチャを更新します。
        textureCombo(ICON_FA_PALETTE " カラーランプ", currentRampTextureIndex_, editRampTexturePath_, textureSearch_[2], [&](const std::string& path) {
            uint32_t handle = path.empty() ? 0 : engine::TextureManager::GetInstance()->Load(path);
            previewEffect_->SetRampTexture(handle);
            });
    }

    // 配置欄を展開した場合に、位置とラジアン単位の回転を調整します。
    if (ImGui::CollapsingHeader(ICON_FA_ARROWS_ALT " 配置設定 (Transform)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat3("座標 (Position)", &editPosition_.x, 0.1f);
        ImGui::DragFloat3("回転 (Rotation)", &editRotation_.x, 0.01f);
    }

    // アニメーション欄を展開した場合に、秒単位の寿命と開始・終了の状態を調整します。
    if (ImGui::CollapsingHeader(ICON_FA_FILM " アニメーション設定", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat(ICON_FA_CLOCK " 寿命 (秒)", &editLifetime_, 0.05f, 3.0f);

        ImGui::Spacing();
        ImGui::Text(ICON_FA_EXPAND_ARROWS_ALT " [ スケール (Scale) ]");
        ImGui::DragFloat3("開始スケール", &editStartScale_.x, 0.1f);
        ImGui::DragFloat3("終了スケール", &editEndScale_.x, 0.1f);

        ImGui::Spacing();

        // カラーランプのパスが空なら、開始色と終了色を直接調整できます。
        if (strlen(editRampTexturePath_) == 0) {
            ImGui::Text(ICON_FA_TINT " [ カラー (Color) ]");
            ImGui::ColorEdit4("開始カラー", &editStartColor_.x);
            ImGui::ColorEdit4("終了カラー", &editEndColor_.x);
        }
        else {
            // カラーランプが指定されている場合は、個別の色入力を置き換えます。
            ImGui::TextDisabled(ICON_FA_TINT " [ カラー設定無効 ]");
            ImGui::TextDisabled("※カラーランプが適用されているため無効化されています");
        }
    }

    // 材質欄を展開した場合に、発光や歪みと形状による表示の差を調整します。
    if (ImGui::CollapsingHeader(ICON_FA_SLIDERS_H " シェーダーパラメータ", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat2("スクロール速度", &editScrollSpeed_.x, 0.01f);
        ImGui::DragFloat("発光強度 (HDR)", &editIntensity_, 0.01f, 0.0f, 100.0f);
        ImGui::SliderFloat(editProceduralType_ >= 13 ? "電流の走行量" : "詠唱の光の流れ", &editChantFlowStrength_, 0.0f, 1.0f);
        ImGui::SliderFloat("術者を巻く光帯", &editChantRibbonStrength_, 0.0f, 1.0f);
        ImGui::TextDisabled("平面の光の流れ／回転切りの光帯で有効。スクロール速度で流れを調整します。");
        ImGui::SliderFloat(editProceduralType_ >= 13 ? "放電の密度" : "リング刃のエネルギー密度", &editRingEnergyStrength_, 0.0f, 1.0f);
        ImGui::SliderFloat(editProceduralType_ >= 13 ? "雷撃の更新速度" : "リング刃の周回速度", &editRingSpinSpeed_, -40.0f, 40.0f);
        // 放電形状では、分岐を更新するパラメーターとして案内します。
        if (editProceduralType_ >= 13)
            ImGui::TextDisabled("放電形状はUVを流さず、分岐の更新と電流の走行で動きを作ります。");
        // 通常の形状用の表示では、リングの明暗を周回させる調整として案内します。
        else
            ImGui::TextDisabled("トーラス形状で有効。輪郭に明暗を作り、発光部を周回させます。");

        ImGui::Separator();

        ImGui::Text(ICON_FA_WATER " [ 背景歪み (Distortion) ]");
        ImGui::Checkbox("背景歪みを有効にする", &editEnableDistortion_);
        ImGui::SliderFloat("歪みの強さ", &editDistortionStrength_, 0.0f, 0.2f);
        ImGui::SliderFloat("歪みの速度", &editDistortionSpeed_, 0.0f, 50.0f);

        ImGui::Spacing();

        ImGui::Text(ICON_FA_CUT " [ エッジフェード (形状削り出し) ]");
        ImGui::SliderFloat("削り出しの強さ", &editEdgeFadeStrength_, 1.0f, 10.0f);
        ImGui::SliderFloat("透過足切り (AlphaReference)", &editAlphaReference_, 0.0f, 1.0f);
    }

    // 衝突欄を展開した場合に、プレビューの判定設定とデバッグ表示を同期します。
    if (ImGui::CollapsingHeader("Collision Settings")) {
        auto meshSettings = previewEffect_->GetMeshSettings();
        ImGui::Checkbox("Has Collision", &meshSettings.hasCollision);

        // 衝突が有効の場合に、形状・大きさ・中心のオフセットを設定します。
        if (meshSettings.hasCollision) {
            ImGui::Combo("Shape", &meshSettings.collisionShape, "Sphere\0AABB\0OBB\0Cylinder\0\0");
            ImGui::DragFloat3("Size/Radius", &meshSettings.collisionSize.x, 0.1f);
            ImGui::DragFloat3("Offset", &meshSettings.collisionOffset.x, 0.1f);

            // プレビュー判定の型は球・AABB・OBBを割り当てます。Cylinderを選んだ場合、この分岐では既定の None を維持します。
            engine::ColliderType cType = engine::ColliderType::kNone;
            if (meshSettings.collisionShape == 0)
                cType = engine::ColliderType::kSphere;
            else if (meshSettings.collisionShape == 1) cType = engine::ColliderType::kAabb;
            else if (meshSettings.collisionShape == 2) cType = engine::ColliderType::kObb;

            // 既存の設定を基準に、判定の種類・サイズ・中心を編集値で置き換えます。
            engine::Object3D::ColliderConfig cConfig = previewEffect_->GetColliderConfig();
            cConfig.type = cType;
            cConfig.size = meshSettings.collisionSize;
            cConfig.center = meshSettings.collisionOffset;

            // 設定を戻す
            previewEffect_->SetColliderConfig(cConfig);

        }
        // 衝突を無効にしたときは、プレビュー側の衝突形状も解除します。
        else {
            previewEffect_->SetColliderType(engine::ColliderType::kNone);
        }

        previewEffect_->SetMeshSettings(meshSettings);

        MeshEffectManager::GetInstance()->SetPreviewEffectForDebug(previewEffect_.get());
    }

    files_.Draw("メッシュエフェクトJSONファイル", "Resources/json/effect",
        [this](const std::filesystem::path& path) { return LoadFromJson(path); },
        [this](const std::filesystem::path& path, const std::string&) { return SaveToJson(path); },
        [this] { ResetDocument(); });
#endif
}

// 編集中のパラメーターをJSONへ保存し、ファイルを閉じた後の状態を返します。
bool MeshEffectEditor::SaveToJson(const std::filesystem::path& path) const {
    // 本体が生成されていなければ、形状と衝突設定を取得できないため保存しません。
    if (!previewEffect_) return false;

    json j;

    // 追従先がある場合、実体の参照の代わりに名前を保存します。
    if (targetObject_) {
        j["TargetName"] = targetObject_->GetName();
    }
    // 追従先を使わない場合は、空の名前を明示します。
    else {
        j["TargetName"] = "";
    }

    // --- リソース ---
    j["ModelName"] = editModelName_;
    j["TexturePath"] = editTexturePath_;
    j["NoiseTexturePath"] = editNoiseTexturePath_;
    j["RampTexturePath"] = editRampTexturePath_;

    // --- ベース Transform ---
    j["Position"] = { editPosition_.x, editPosition_.y, editPosition_.z };
    j["Rotation"] = { editRotation_.x, editRotation_.y, editRotation_.z };

    // --- アニメーション ---
    j["Lifetime"] = editLifetime_;
    j["AutoLoop"] = isAutoLoop_;

    j["StartScale"] = { editStartScale_.x, editStartScale_.y, editStartScale_.z };
    j["EndScale"] = { editEndScale_.x, editEndScale_.y, editEndScale_.z };

    j["StartColor"] = { editStartColor_.x, editStartColor_.y, editStartColor_.z, editStartColor_.w };
    j["EndColor"] = { editEndColor_.x, editEndColor_.y, editEndColor_.z, editEndColor_.w };

    // --- シェーダーパラメータ ---
    j["ScrollSpeed"] = { editScrollSpeed_.x, editScrollSpeed_.y };
    j["Intensity"] = editIntensity_;
    j["UseTextureColor"] = editUseTextureColor_;
    j["ChantFlowStrength"] = editChantFlowStrength_;
    j["ChantRibbonStrength"] = editChantRibbonStrength_;
    j["RingEnergyStrength"] = editRingEnergyStrength_;
    j["RingSpinSpeed"] = editRingSpinSpeed_;
    j["DistortionStrength"] = editDistortionStrength_;
    j["DistortionSpeed"] = editDistortionSpeed_;
    j["EdgeFadeStrength"] = editEdgeFadeStrength_;
    j["AlphaReference"] = editAlphaReference_;
    j["EnableDistortion"] = editEnableDistortion_;
    j["BlendMode"] = currentBlendModeIndex_;
    j["EnableReveal"] = editEnableReveal_;
    j["RevealMode"] = editEnableReveal_ ? editRevealMode_ : 0;
    j["EasingType"] = editEasingType_;
    j["VolumeMode"] = editVolumeMode_;

    // 形状の種類と、その本体に設定されている生成パラメーターを記録します。
    j["ProceduralType"] = editProceduralType_;

    // 本体を取得できる場合はメッシュの寸法、分割数、UV倍率を保存します。
    if (previewEffect_) {
        j["SlashAngle"] = previewEffect_->GetMeshSettings().slashAngle;
        j["InnerRadius"] = previewEffect_->GetMeshSettings().innerRadius;
        j["OuterRadius"] = previewEffect_->GetMeshSettings().outerRadius;
        j["Thickness"] = previewEffect_->GetMeshSettings().thickness;
        j["SpiralPitch"] = previewEffect_->GetMeshSettings().spiralPitch;
        j["ThrustLength"] = previewEffect_->GetMeshSettings().thrustLength;
        j["ThrustRadius"] = previewEffect_->GetMeshSettings().thrustRadius;
        j["SphereRadius"] = previewEffect_->GetMeshSettings().sphereRadius;
        j["SphereRings"] = previewEffect_->GetMeshSettings().sphereRings;
        j["CylinderRadius"] = previewEffect_->GetMeshSettings().cylinderRadius;
        j["CylinderHeight"] = previewEffect_->GetMeshSettings().cylinderHeight;
        j["BoxSize"] = {previewEffect_->GetMeshSettings().boxSize.x, previewEffect_->GetMeshSettings().boxSize.y, previewEffect_->GetMeshSettings().boxSize.z};
        j["PlaneSize"] = {previewEffect_->GetMeshSettings().planeSize.x, previewEffect_->GetMeshSettings().planeSize.y};
        j["TorusMajorRadius"] = previewEffect_->GetMeshSettings().torusMajorRadius;
        j["TorusMinorRadius"] = previewEffect_->GetMeshSettings().torusMinorRadius;
        j["ConeRadius"] = previewEffect_->GetMeshSettings().coneRadius;
        j["ConeHeight"] = previewEffect_->GetMeshSettings().coneHeight;
        j["RingOuterRadius"] = previewEffect_->GetMeshSettings().ringOuterRadius;
        j["RingInnerRadius"] = previewEffect_->GetMeshSettings().ringInnerRadius;
        j["TriangleSize"] = previewEffect_->GetMeshSettings().triangleSize;
        j["MeshSegments"] = previewEffect_->GetMeshSettings().meshSegments;
        j["UvTiling"] = {previewEffect_->GetMeshSettings().uvTiling.x, previewEffect_->GetMeshSettings().uvTiling.y};
    }

    // 実行側でも判定を再現できるよう、形状とは別に衝突の編集値を保存します。
    j["Collision"]["HasCollision"] = previewEffect_->GetMeshSettings().hasCollision;
    j["Collision"]["Shape"] = previewEffect_->GetMeshSettings().collisionShape;
    j["Collision"]["Size"] = { previewEffect_->GetMeshSettings().collisionSize.x, previewEffect_->GetMeshSettings().collisionSize.y, previewEffect_->GetMeshSettings().collisionSize.z };
    j["Collision"]["Offset"] = { previewEffect_->GetMeshSettings().collisionOffset.x, previewEffect_->GetMeshSettings().collisionOffset.y, previewEffect_->GetMeshSettings().collisionOffset.z };
    std::ofstream file(path);
    file << j.dump(4);
    file.close();
    return file.good();
}

bool MeshEffectEditor::LoadFromJson(const std::filesystem::path& path) {
    // 設定ファイルを開き、現在の本体に反映できるかを最初に確認します。
    std::ifstream file(path);
    // 入力または反映先を用意できなければ、編集値を更新しません。
    if (!file || !previewEffect_) return false;

    json j;
    file >> j;
    // ルートがオブジェクトでないか、モデル名が文字列でなければ設定として受け付けません。
    if (!j.is_object() || (j.contains("ModelName") && !j["ModelName"].is_string())) return false;

    // パス項目を順に読み、保存されている場合に編集欄とプレビューへ適用します。

    if (j.contains("ModelName")) {
        std::string modelName = j["ModelName"];
        strncpy_s(editModelName_, modelName.c_str(), sizeof(editModelName_) - 1);
        previewEffect_->SetModel(editModelName_);
    }

    if (j.contains("TexturePath")) {
        std::string texPath = j["TexturePath"];
        strncpy_s(editTexturePath_, texPath.c_str(), sizeof(editTexturePath_) - 1);

        // 描画実体があり、復元したメイン画像のパスが空でない場合に割り当てます。
        if (auto renderer = previewEffect_->GetMeshRenderer()) {
            // 復元された画像パスに内容があれば、描画実体へ指定します。
            if (strlen(editTexturePath_) > 0) {
                renderer->SetTexture(editTexturePath_);
            }
        }
    }

    if (j.contains("NoiseTexturePath")) {
        std::string noisePath = j["NoiseTexturePath"];
        strncpy_s(editNoiseTexturePath_, noisePath.c_str(), sizeof(editNoiseTexturePath_) - 1);

        // 保存されたノイズパスが空でなければ、画像を読み込んで本体へ渡します。
        if (strlen(editNoiseTexturePath_) > 0) {
            uint32_t texHandle = engine::TextureManager::GetInstance()->Load(editNoiseTexturePath_);
            previewEffect_->SetNoiseTexture(texHandle);
        }
    }

    if (j.contains("RampTexturePath")) {
        std::string rampPath = j["RampTexturePath"];
        strncpy_s(editRampTexturePath_, rampPath.c_str(), sizeof(editRampTexturePath_) - 1);

        // 保存されたカラーランプのパスが空でなければ、画像を読み込んで本体へ渡します。
        if (strlen(editRampTexturePath_) > 0) {
            uint32_t texHandle = engine::TextureManager::GetInstance()->Load(editRampTexturePath_);
            previewEffect_->SetRampTexture(texHandle);
        }
    }

    // 配置の3成分を復元する。追従先が解決できれば次の更新でその相対配置に使う。
    if (j.contains("Position")) {
        editPosition_.x = j["Position"][0];
        editPosition_.y = j["Position"][1];
        editPosition_.z = j["Position"][2];
    }

    // 回転が保存されている場合は、3成分を次のプレビュー更新に使います。
    if (j.contains("Rotation")) {
        editRotation_.x = j["Rotation"][0];
        editRotation_.y = j["Rotation"][1];
        editRotation_.z = j["Rotation"][2];
    }

    // 秒単位の寿命、ループ、拡縮と色の始点・終点は各保存キーがある場合だけ上書きします。
    if (j.contains("Lifetime"))
        editLifetime_ = j["Lifetime"];
    if (j.contains("AutoLoop"))
        isAutoLoop_ = j["AutoLoop"];

    if (j.contains("StartScale")) {
        editStartScale_.x = j["StartScale"][0];
        editStartScale_.y = j["StartScale"][1];
        editStartScale_.z = j["StartScale"][2];
    }

    if (j.contains("EndScale")) {
        editEndScale_.x = j["EndScale"][0];
        editEndScale_.y = j["EndScale"][1];
        editEndScale_.z = j["EndScale"][2];
    }

    if (j.contains("StartColor")) {
        editStartColor_.x = j["StartColor"][0];
        editStartColor_.y = j["StartColor"][1];
        editStartColor_.z = j["StartColor"][2];
        editStartColor_.w = j["StartColor"][3];
    }

    if (j.contains("EndColor")) {
        editEndColor_.x = j["EndColor"][0];
        editEndColor_.y = j["EndColor"][1];
        editEndColor_.z = j["EndColor"][2];
        editEndColor_.w = j["EndColor"][3];
    }

    // UVスクロール速度が指定されていれば2成分を復元します。
    if (j.contains("ScrollSpeed")) {
        editScrollSpeed_.x = j["ScrollSpeed"][0];
        editScrollSpeed_.y = j["ScrollSpeed"][1];
        previewEffect_->SetScrollSpeed(editScrollSpeed_);
    }

    // 発光の強度が保存されていれば、編集値と本体の強度を揃えます。
    if (j.contains("Intensity")) {
        editIntensity_ = j["Intensity"];
        previewEffect_->SetIntensity(editIntensity_);
    }

    // 描画設定は記載がある項目を読みます。歪み強度・有効状態、透過の足切り、画像RGBは欠落時に無効相当へ戻します。
    if (j.contains("DistortionStrength"))
        editDistortionStrength_ = j["DistortionStrength"];
    else editDistortionStrength_ = 0.0f;
    if (j.contains("DistortionSpeed"))
        editDistortionSpeed_ = j["DistortionSpeed"];
    if (j.contains("EdgeFadeStrength"))
        editEdgeFadeStrength_ = j["EdgeFadeStrength"];
    if (j.contains("AlphaReference"))
        editAlphaReference_ = j["AlphaReference"];
    else editAlphaReference_ = 0.0f;
    if (j.contains("EnableDistortion"))
        editEnableDistortion_ = j["EnableDistortion"];
    else editEnableDistortion_ = false;
    if (j.contains("BlendMode"))
        currentBlendModeIndex_ = j["BlendMode"];
    if (j.contains("UseTextureColor"))
        editUseTextureColor_ = j["UseTextureColor"].get<bool>();
    else editUseTextureColor_ = false;
    previewEffect_->SetUseTextureColor(editUseTextureColor_);
    editChantFlowStrength_ = j.value("ChantFlowStrength", 0.0f);
    previewEffect_->SetChantFlowStrength(editChantFlowStrength_);
    editChantRibbonStrength_ = j.value("ChantRibbonStrength", 0.0f);
    previewEffect_->SetChantRibbonStrength(editChantRibbonStrength_);
    editRingEnergyStrength_ = j.value("RingEnergyStrength", 0.0f);
    previewEffect_->SetRingEnergyStrength(editRingEnergyStrength_);
    editRingSpinSpeed_ = j.value("RingSpinSpeed", 0.0f);
    previewEffect_->SetRingSpinSpeed(editRingSpinSpeed_);
    // 伸びる演出の有効設定を確認し、旧形式で未指定なら有効にします。
    if (j.contains("EnableReveal"))
        editEnableReveal_ = j["EnableReveal"];
    else editEnableReveal_ = true;
    // 伸び方の指定を有効範囲へ補正し、省略時は伸びる演出の有効状態から決めます。
    if (j.contains("RevealMode"))
        editRevealMode_ = std::clamp(j["RevealMode"].get<std::int32_t>(), 0, 2);
    else editRevealMode_ = editEnableReveal_ ? 1 : 0;

    // イージングがあれば保存値を使い、省略時は線形に戻します。
    if (j.contains("EasingType"))
        editEasingType_ = j["EasingType"];
    else editEasingType_ = 0;

    // 立体化モードが保存されていなければ、追加レイヤーを作らない設定にします。
    if (j.contains("VolumeMode"))
        editVolumeMode_ = j["VolumeMode"];
    else editVolumeMode_ = 0;

    // プロシージャルパラメータの読み込み
    if (j.contains("ProceduralType"))
        editProceduralType_ = j["ProceduralType"];
    // 本体が用意できていれば、形状の各パラメーターをJSONから復元します。
    if (previewEffect_) {
        previewEffect_->ReadProceduralMeshSettings(j);
    }

    // 衝突設定が含まれる場合は、判定の種類と寸法も復元します。
    if (j.contains("Collision")) {
        previewEffect_->SetMeshSetting(&EffectObject3D::MeshSettings::hasCollision, j["Collision"]["HasCollision"]);
        previewEffect_->SetMeshSetting(&EffectObject3D::MeshSettings::collisionShape, j["Collision"]["Shape"]);
        previewEffect_->SetMeshSetting(&EffectObject3D::MeshSettings::collisionSize, { j["Collision"]["Size"][0], j["Collision"]["Size"][1], j["Collision"]["Size"][2] });
        previewEffect_->SetMeshSetting(&EffectObject3D::MeshSettings::collisionOffset, { j["Collision"]["Offset"][0], j["Collision"]["Offset"][1], j["Collision"]["Offset"][2] });
    }

    // 先に前回の対象を解除し、保存名があれば実行時と同じ走査順で解決する。
    targetObject_ = nullptr;
    // 対象名が指定されている場合に、現在のシーンから追従先を探します。
    if (j.contains("TargetName")) {
        targetObject_ = FindMeshEffectTarget(sceneManager_ ? sceneManager_->GetCurrentScene() : nullptr,
            j["TargetName"].get<std::string>());
    }

    // 読み込み完了後にプレビューを再起動
    forcePlayRequest_ = true;
    SyncTextureIndices();


    // 生成形状を選択しており本体がある場合は、読み込んだ値で各面を作り直します。
    if (editProceduralType_ >= 1 && previewEffect_) {
        SyncPreviewMesh(*previewEffect_, extraPreviewEffects_, editProceduralType_);
    }
    return true;
}

void MeshEffectEditor::ResetDocument() {
    // 編集値を初期値に戻し、次の更新で既定のプレビューを作る。
    const MeshEffectEditor defaults;
    editPosition_ = defaults.editPosition_;
    editRotation_ = defaults.editRotation_;
    editScale_ = defaults.editScale_;
    std::copy_n(defaults.editModelName_, sizeof(editModelName_), editModelName_);
    std::copy_n(defaults.editTexturePath_, sizeof(editTexturePath_), editTexturePath_);
    editColor_ = defaults.editColor_;
    editScrollSpeed_ = defaults.editScrollSpeed_;
    editIntensity_ = defaults.editIntensity_;
    isAutoLoop_ = defaults.isAutoLoop_;
    editLifetime_ = defaults.editLifetime_;
    editStartScale_ = defaults.editStartScale_;
    editEndScale_ = defaults.editEndScale_;
    editStartColor_ = defaults.editStartColor_;
    editEndColor_ = defaults.editEndColor_;
    editEnableDistortion_ = defaults.editEnableDistortion_;
    editAlphaReference_ = defaults.editAlphaReference_;
    editDistortionStrength_ = defaults.editDistortionStrength_;
    editDistortionSpeed_ = defaults.editDistortionSpeed_;
    editEdgeFadeStrength_ = defaults.editEdgeFadeStrength_;
    std::copy_n(defaults.editNoiseTexturePath_, sizeof(editNoiseTexturePath_), editNoiseTexturePath_);
    std::copy_n(defaults.editRampTexturePath_, sizeof(editRampTexturePath_), editRampTexturePath_);
    currentBlendModeIndex_ = defaults.currentBlendModeIndex_;
    editEnableReveal_ = defaults.editEnableReveal_;
    editUseTextureColor_ = defaults.editUseTextureColor_;
    editChantFlowStrength_ = defaults.editChantFlowStrength_;
    editChantRibbonStrength_ = defaults.editChantRibbonStrength_;
    editRingEnergyStrength_ = defaults.editRingEnergyStrength_;
    editRingSpinSpeed_ = defaults.editRingSpinSpeed_;
    editRevealMode_ = defaults.editRevealMode_;
    editEasingType_ = defaults.editEasingType_;
    editProceduralType_ = defaults.editProceduralType_;
    editVolumeMode_ = defaults.editVolumeMode_;
    targetObject_ = nullptr;
    // 前の編集内容で作ったメッシュを破棄し、次のUpdateで新規文書の値から作り直す。
    previewEffect_.reset();
    extraPreviewEffects_.clear();
    forcePlayRequest_ = false;
    SyncTextureIndices();
}

void MeshEffectEditor::SyncTextureIndices() {
    // 保存済みパスの表記ゆれを吸収し、画像一覧の選択位置へ戻す。
    auto findIndex = [&](const std::string& path) -> std::int32_t {
        // 空の画像パスは選択なしとして扱います。
        if (path.empty())
            return -1;
        for (std::int32_t i = 0; i < (std::int32_t)textureFileList_.size(); ++i) {
            std::string fullPath = "Resources/sprite/" + textureFileList_[i];
            // パス表記かファイル名のいずれかが一致した場合に、対応する候補の索引を返します。
            if (textureFileList_[i] == path ||
                fullPath == path ||
                std::filesystem::path(textureFileList_[i]).filename() == std::filesystem::path(path).filename()) {
                return i;
            }
        }

        return -1;
        };

    currentTextureIndex_ = findIndex(editTexturePath_);
    currentNoiseTextureIndex_ = findIndex(editNoiseTexturePath_);
    currentRampTextureIndex_ = findIndex(editRampTexturePath_);
}
