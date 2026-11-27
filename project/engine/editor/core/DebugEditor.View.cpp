#define NOMINMAX
#include <cstdint>
#include "DebugEditor.h"
#include "engine/editor/graphics/camera/CameraObjectBadge.h"
#include <numbers>
#include "game/ui/SettingsMenuOverlay.h"
#include "SceneManager.h"    
#include "BaseScene.h"      
#include "Object3D.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <fstream>
#include <string>
#include <vector>
#include "json.hpp"
#include "ImGuizmo.h"
#include "CameraManager.h"
#include "WinApp.h"
#include "Math.h"
#include "DirectXCommon.h"
#include "CollisionConfig.h"
#include "ModelManager.h"      
#include "InputManager.h"    
#include <cmath>
#include <cassert> 
#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include "GhostRecorder.h" 
#include "engine/editor/graphics/camera/CameraEditor.h"
#include "Transform.h"
#include "ParticleManager.h"
#include "EditorManager.h"
#include "PostEffectEditor.h"
#include "SpriteDebugEditor.h"
#include "ParticleEditor.h"
#include "GpuParticleEditor.h"
#include "VfxSequencerEditor.h"
#include "LightEditor.h"      
#include "IconsFontAwesome5.h"

#include <DebugConsole.h>
#include <CollisionManager.h>
#include <filesystem> // ファイル操作用
#include <game/system/combat/BulletManager.h>
#include "engine/editor/assets/preset/PresetManager.h"
#include <PresetEditor.h>
#include <MeshEffectManager.h>
#include <unordered_map>
#include <utility>
namespace fs = std::filesystem;




namespace {
    // 実ゲームでは不可視のカメラ等も、Editorのプレビューでは識別できる描画クラスへ切り替えます。
    bool ShouldUsePreviewDrawClassOverride(const engine::Object3D* object) {
        // 描画クラスを調べられない空の参照は、上書き対象にしない。
        if (!object)
            return false;

        const std::string className = object->GetClassName();
        return object->IsCameraObject() ||
            className == "GPUParticle" ||
            className == "InvisibleBox";
    }
}

// ========================================================================
// Game View のピッキング
// ========================================================================

// 画像内のピクセルをワールドの近面・遠面に変換する。無効な入力では空のレイを返す。
engine::Ray DebugEditor::ScreenPointToRay(const engine::Vector2& mousePos) {
    const engine::Camera* camera = CameraManager::GetInstance()->GetActiveCamera();

    // 描画中のカメラがなければ、画面座標をワールドへ逆変換できない。
    if (!camera)
        return engine::Ray{};

    // 大きな平行移動を含むViewProjectionをfloatで直接反転すると桁落ちする。
    // 投影の逆変換をカメラ空間で済ませ、その後にワールドへ移す。
    const engine::Matrix4x4 inverseProjection = math_.Inverse(camera->GetProjectionMatrix());
    const engine::Matrix4x4 inverseView = math_.Inverse(camera->GetViewMatrix());
    // 画像寸法が有限で正であり、マウス位置も有限な場合だけ正規化する。
    if (!std::isfinite(gameViewSize_.x) || !std::isfinite(gameViewSize_.y) ||
        gameViewSize_.x <= 0.0f || gameViewSize_.y <= 0.0f ||
        !std::isfinite(mousePos.x) || !std::isfinite(mousePos.y))
        return engine::Ray{};


    float localMouseX = mousePos.x;
    float localMouseY = mousePos.y;

    // 実際の画像の外にあるポインターは、シーン配置のレイへ変換しない。
    if (localMouseX < 0 || localMouseX > gameViewSize_.x ||
        localMouseY < 0 || localMouseY > gameViewSize_.y) {
        return engine::Ray{ {0,0,0}, {0,0,0} };
    }

    engine::Vector3 nearPos, farPos;
    // 画像内のピクセルを DirectX の正規化座標に移す。Y は上向きに反転し、深度 0 と 1 から近面・遠面の点を作る。
    nearPos.x = (2.0f * localMouseX) / gameViewSize_.x - 1.0f;
    nearPos.y = 1.0f - (2.0f * localMouseY) / gameViewSize_.y;
    nearPos.z = 0.0f;

    farPos = nearPos;
    farPos.z = 1.0f;

    // 方向は座標変換後の2点を引かず、平行移動を除いた変換で求める。
    // near→farの長さを保持し、選択処理が使う交差パラメータ0～1と一致させる。
    const engine::Vector3 cameraNear = math_.Transform(nearPos, inverseProjection);
    const engine::Vector3 cameraFar = math_.Transform(farPos, inverseProjection);
    engine::Ray ray;
    ray.origin = math_.Transform(cameraNear, inverseView);
    ray.diff = math_.TransformNormal(cameraFar - cameraNear, inverseView);
    // 逆変換で有限な始点・方向を得られない場合は、ピッキング対象にしない。
    if (!std::isfinite(ray.origin.x) || !std::isfinite(ray.origin.y) || !std::isfinite(ray.origin.z) ||
        !std::isfinite(ray.diff.x) || !std::isfinite(ray.diff.y) || !std::isfinite(ray.diff.z))
        return engine::Ray{};

    return ray;
}

// ---------------------------------------------------------
// レイと平面(Y=0)の交点を計算する関数
// 戻り値: 交差したら true, その座標を intersectOut に入れる
// ---------------------------------------------------------
bool DebugEditor::IntersectRayPlane(const engine::Ray& ray, engine::Vector3& intersectOut) {
    // 計算用のMathクラス
    engine::Math math;

    // 平面の定義（上向きの法線, 高さ0）
    engine::Vector3 planeNormal = { 0.0f, 1.0f, 0.0f };
    float planeHeight = 0.0f;

    // 平行かどうかチェック (内積が0に近い＝平行)
    // レイの方向ベクトルと平面の法線の内積をとる
    float denominator = math.Dot(ray.diff, planeNormal);

    // レイが水平に近いなら判定しない (0除算防止)
    if (std::abs(denominator) < 0.0001f) {
        return false;
    }

    // 交点までの距離 t を求める公式
    // t = (PlaneHeight - Dot(RayOrigin, PlaneNormal)) / Dot(RayDir, PlaneNormal)

    // まずレイの方向を正規化する
    engine::Vector3 rayDir = math.Normalize(ray.diff);
    float dotDir = math.Dot(rayDir, planeNormal);

    // 正規化後にも法線との角度を確認する。ほぼ平行な場合は距離を割り出さない。
    if (std::abs(dotDir) < 0.0001f)
        return false;

    // 距離tの計算
    // 単位方向を使うため t はレイ始点からのワールド距離。置き場所の候補として、始点より先の平面だけを採用する。
    float t = (planeHeight - math.Dot(ray.origin, planeNormal)) / dotDir;

    // tがマイナス＝カメラの後ろ側にあるので無視
    if (t < 0.0f) {
        return false;
    }

    // 交点座標 = 原点 + 方向 * 距離
    // intersectOut = ray.origin + (rayDir * t)
    engine::Vector3 travel = { rayDir.x * t, rayDir.y * t, rayDir.z * t };
    intersectOut = { ray.origin.x + travel.x, ray.origin.y + travel.y, ray.origin.z + travel.z };

    return true;
}

void DebugEditor::DrawPreview(ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource) {
    // 編集プレビューの描画時は、現在のライト定数を表示専用の描画経路へ渡す。
    auto drawPreviewObject = [&](engine::Object3D* object) {
        // プレビューの親子一覧やブラシに欠けた参照がある場合、その描画だけを飛ばす。
        if (!object)
            return;

        std::string originalClassName;
        bool useClassOverride = ShouldUsePreviewDrawClassOverride(object);

        // 通常の表示経路では識別しにくいクラスの場合だけ、モデル描画へ切り替える。
        if (useClassOverride) {
            originalClassName = object->GetClassName();
            object->SetClassName("Model");
        }

        // 表示経路を一時的に変えた Object は、描画直後に元のクラスを戻す。保存や選択には本来のクラスを使う。
        object->Draw(pointLightResource, spotLightResource);

        // クラスを入れ替えた場合にだけ、描画直後に元の登録名を復元する。
        if (useClassOverride) {
            object->SetClassName(originalClassName);
        }
    };

    // 配置候補の本体がある場合に、本体に続けて子のプレビューも描画する。
    if (previewObject_) {
        drawPreviewObject(previewObject_.get());

        for (auto& child : previewChildObjects_) {
            drawPreviewObject(child.get());
        }
    }

    // ブラシの置き場所が見つかったときだけ、その位置のプレビューを描く。
    if (brushPlacement_.found) {
        for (auto& object : brushPreviewObjects_)
            drawPreviewObject(object.get());
    }

}

// ========================================================================
// セーブ通知のトリガー
// ========================================================================
void DebugEditor::TriggerSaveNotification(const std::string& filename) {
#ifdef USE_IMGUI
    saveNotificationTimer_ = 1.0f;
    saveNotificationMsg_ = "[ " + filename + " ] にセーブしました";
#endif
}

void DebugEditor::PollDdsCacheNotifications() {
#ifdef USE_IMGUI
    // 変換ツールの追記ログを前回の読み取り位置から読み、完了通知だけを表示する。
    const fs::path notificationLog = "Resources/.cache/dds_cache_notifications.jsonl";

    // ログが存在しない場合は読み取り位置を初期化し、後で作成されたときに先頭から読む。
    if (!fs::exists(notificationLog)) {
        ddsCacheNotificationReadOffset_ = 0;
        return;
    }

    std::error_code ec;
    const std::uintmax_t fileSize = fs::file_size(notificationLog, ec);

    // ファイルサイズが取得できなければ、読み取り位置を保持して次回に回す。
    if (ec) {
        return;
    }

    if (fileSize < ddsCacheNotificationReadOffset_) {
        // ログのローテートや作り直しでサイズが縮んだ場合は先頭から読み直す。
        ddsCacheNotificationReadOffset_ = 0;
    }

    // 前回からサイズが変わっていなければ、新規通知はないためファイルを開かない。
    if (fileSize == ddsCacheNotificationReadOffset_) {
        return;
    }

    std::ifstream ifs(notificationLog, std::ios::binary);

    // 追記ログを開けない場合は、読み取り位置を進めず次回に回す。
    if (!ifs) {
        return;
    }

    ifs.seekg(static_cast<std::streamoff>(ddsCacheNotificationReadOffset_), std::ios::beg);

    std::string line;
    bool hasCompletedConversion = false;

    while (std::getline(ifs, line)) {
        // 空の行からは通知を取り出せないため飛ばす。
        if (line.empty()) {
            continue;
        }

        try {
            // 途中書き込みなどで壊れた行があっても、次の通知の読み取りを続ける。
            nlohmann::json j = nlohmann::json::parse(line);

            // 変換完了を示す built だけを画面通知へ渡す。
            if (j.value("status", "") != "built") {
                continue;
            }

            hasCompletedConversion = true;
        }
        catch (...) {
        }
    }

    ddsCacheNotificationReadOffset_ = fileSize;

    // 今回に完了が一件以上あれば、複数行を一つの完了表示にまとめる。
    if (hasCompletedConversion) {
        saveNotificationTimer_ = 1.5f;
        saveNotificationMsg_ = "DDS変換が完了しました";
    }
#endif
}


// ========================================================================
// セーブ通知の描画処理 (フラッシュ & メッセージ)
// ========================================================================
void DebugEditor::DrawSaveNotification() {
#ifdef USE_IMGUI
    // 表示時間が尽きている場合は、フラッシュも文字も描かない。
    if (saveNotificationTimer_ <= 0.0f)
        return;

    // タイマー減算 (ImGuiのDeltaTimeを利用してフレームレート非依存に)
    saveNotificationTimer_ -= ImGui::GetIO().DeltaTime;

    // 残り時間を1.5秒で割り、通知の不透明度に使う。
    float fadeRatio = saveNotificationTimer_ / 1.5f;
    // 比率が1より大きい場合は、色のアルファ値が上限を超えないようにする。
    if (fadeRatio > 1.0f)
        fadeRatio = 1.0f;

    // 最前面レイヤーを取得
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    ImVec2 displaySize = ImGui::GetIO().DisplaySize;

    // 画面全体へ半透明の白を描き、通知の残り時間に合わせて弱める。
    std::int32_t whiteAlpha = (std::int32_t)(fadeRatio * 80.0f);
    drawList->AddRectFilled(ImVec2(0, 0), displaySize, IM_COL32(255, 255, 255, whiteAlpha));

    // 同じ描画レイヤーに、2倍サイズの文字を画面中央に置く。
    float fontSize = ImGui::GetFontSize() * 2.0f; // 文字を2倍サイズに
    const char* text = saveNotificationMsg_.c_str();

    // 文字のピクセルサイズを計算して、画面中央の座標を求める
    ImVec2 baseTextSize = ImGui::CalcTextSize(text);
    ImVec2 textSize = ImVec2(baseTextSize.x * 2.0f, baseTextSize.y * 2.0f);
    ImVec2 textPos = ImVec2((displaySize.x - textSize.x) * 0.5f, (displaySize.y - textSize.y) * 0.5f);

    // 文字のアルファ値 (フェードに合わせて透明になっていく)
    std::int32_t textAlpha = (std::int32_t)(fadeRatio * 255.0f);
    ImU32 textColor = IM_COL32(40, 40, 40, textAlpha);         // 濃いグレー
    ImU32 outlineColor = IM_COL32(255, 255, 255, textAlpha);   // 白いフチ取り

    // 少し文字を見やすくするために白い縁取り（シャドウ）を先に描画
    drawList->AddText(ImGui::GetFont(), fontSize, ImVec2(textPos.x + 2, textPos.y + 2), outlineColor, text);

    // メインの文字を描画
    drawList->AddText(ImGui::GetFont(), fontSize, textPos, textColor, text);
#endif
}


// ========================================================================
// 3D座標をGameViewのスクリーン座標に変換
// ========================================================================
engine::Vector3 DebugEditor::WorldToScreen(const engine::Vector3& worldPos) {
    engine::Camera* cam = CameraManager::GetInstance()->GetActiveCamera();

    // 有効なカメラがなければ投影できないため、無効な深度を返す。
    if (!cam)
        return { 0, 0, -1 }; // カメラが無い場合はZを-1(無効)にして返す

    engine::Matrix4x4 viewProj = math_.Multiply(cam->GetViewMatrix(), cam->GetProjectionMatrix());

    // 透視除算後の正規化座標は X・Y が画面方向で、Z の描画範囲は0から1。
    engine::Vector3 ndc = math_.Transform(worldPos, viewProj);

    // 深度が近面から遠面の範囲外なら、アイコンや接続線の描画に使わない。
    if (ndc.z < 0.0f || ndc.z > 1.0f) {
        return { 0, 0, -1 }; // 描画しないフラグとしてZに-1を入れる
    }

    // NDCからGameViewのピクセル座標へ変換
    float screenX = gameViewOffset_.x + (ndc.x + 1.0f) * 0.5f * gameViewSize_.x;
    float screenY = gameViewOffset_.y + (1.0f - ndc.y) * 0.5f * gameViewSize_.y;

    return { screenX, screenY, ndc.z };
}

// ========================================================================
// 3D空間へのアイコンオーバーレイ描画
// ========================================================================
void DebugEditor::Draw3DIcons() {
    std::int32_t left=0,top=0,right=0,bottom=0;
    GetGameViewScreenRect(left,top,right,bottom);
    // 設定画面にも同じ Game View の矩形を渡す。設定画面を操作中は、その上に 3D アイコンを重ねない。
    SettingsMenuOverlay::SetEditorViewport(left,top,right,bottom);
    // 設定画面が開いている場合は、ビューポートを知らせるところまでで終える。
    if (SettingsMenuOverlay::IsAnyActive())
        return;
#ifdef USE_IMGUI
    // ユーザーがシーンアイコンを非表示にしていれば、後続の描画をしない。
    if (!drawSceneIcons_)
        return;
#endif
#ifdef USE_IMGUI
    // 現在のシーンを取得できない場合は、アイコンの対象を列挙しない。
    if (!sceneManager_ || !sceneManager_->GetCurrentScene())
        return;

    // Game Viewの描画レイヤーに載せ、別ウィンドウの上へアイコンを重ねません。
    ImGuiWindow* viewWindow = ImGui::FindWindowByName("Game View");
    // Game View が見つからないかアクティブでない場合は、描画先を使わない。
    if (!viewWindow || !viewWindow->Active)
        return;
    ImDrawList* drawList = viewWindow->DrawList;
    drawList->PushClipRect(ImVec2(gameViewOffset_.x, gameViewOffset_.y), ImVec2(gameViewOffset_.x + gameViewSize_.x, gameViewOffset_.y + gameViewSize_.y), true);
    auto& objects = sceneManager_->GetCurrentScene()->GetObjects();

    for (const auto& obj : objects) {
        // 一時非表示などでビューに描画されないオブジェクトは、アイコンも除く。
        if (!obj->GetIsRenderVisible())
            continue;
        std::string className = obj->GetClassName();
        const bool isCamera = obj->IsCameraObject();
        const char* iconStr = nullptr;
        ImU32 iconColor = IM_COL32(255, 255, 255, 200); // デフォルト白（半透明）

        // 不可視の判定箱、スポナー、カメラだけに識別アイコンを割り当てる。
        if (className == "InvisibleBox") {
            // 不可視の箱は、名前に Trigger があればトリガー用の雷として表示する。
            if (obj->GetName().find("Trigger") != std::string::npos) {
                iconStr = ICON_FA_BOLT; // 雷マーク (トリガー用)
                iconColor = IM_COL32(255, 255, 0, 200); // 黄色
            }
            // その他の不可視の箱は、当たり判定を示す盾として表示する。
            else {
                iconStr = ICON_FA_SHIELD_ALT; // 盾マーク (当たり判定用)
                iconColor = IM_COL32(100, 200, 255, 200); // 水色
            }
        }
        // スポナーは生成物を表す箱アイコンを使う。
        else if (className == "Spawner") {
            iconStr = ICON_FA_BOX_OPEN;
            iconColor = IM_COL32(255, 150, 50, 200);  // オレンジ
        }
        // カメラは設定された役割からバッジの文字と色を決める。
        else if (isCamera) {
            const auto badge = GetCameraObjectBadge(obj->GetSceneCameraSettings());
            iconStr = badge.label;
            iconColor = badge.color;
        }

        // アイコンが設定されたオブジェクトのみ処理
        if (iconStr) {
            // オブジェクトのワールド座標を取得
            engine::Vector3 worldPos = { obj->GetWorldMatrix().m[3][0], obj->GetWorldMatrix().m[3][1], obj->GetWorldMatrix().m[3][2] };

            // カメラの場合は、頭上にアイコンを出すためにY軸を少し上げる
            if (isCamera) {
                worldPos.y += 1.5f;
            }

            // 3D -> 2D 変換
            engine::Vector3 screenPos = WorldToScreen(worldPos);

            // 投影の深度範囲に入る場合だけ描く。XY は Game View のクリップ矩形で制限する。
            if (screenPos.z >= 0.0f) {
                // 選択中のアイコンは種類にかかわらず拡大する。
                const bool isSelected = IsObjectSelected(obj.get());
                float fontSize = isSelected ? ImGui::GetFontSize() * 1.5f : ImGui::GetFontSize() * 1.2f;
                // カメラのバッジ色は保ち、それ以外の選択アイコンを不透明な黄色にする。
                if (isSelected && !isCamera)
                    iconColor = IM_COL32(255, 255, 0, 255); // 選択中は黄色

                // 影（アウトライン）を黒で描画して見やすくする
                ImVec2 pos = ImVec2(screenPos.x, screenPos.y);
                drawList->AddText(ImGui::GetFont(), fontSize, ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, 255), iconStr);

                // 本体の描画
                drawList->AddText(ImGui::GetFont(), fontSize, pos, iconColor, iconStr);
            }
        }
    }

    drawList->PopClipRect();
#endif
}

// ========================================================================
// イベントIDのオーバーレイ描画
// ========================================================================
void DebugEditor::DrawEventIdOverlay() {
#ifdef USE_IMGUI
    // ID 表示が無効かシーンが無い場合は、接続の集計をしない。
    if (!drawEventIds_ || !sceneManager_ || !sceneManager_->GetCurrentScene())
        return;

    // ラベルと線を Game View の描画レイヤーに載せる。
    ImGuiWindow* viewWindow = ImGui::FindWindowByName("Game View");
    // Game View が利用できなければ、オーバーレイを描かない。
    if (!viewWindow || !viewWindow->Active)
        return;
    ImDrawList* drawList = viewWindow->DrawList;
    drawList->PushClipRect(ImVec2(gameViewOffset_.x, gameViewOffset_.y), ImVec2(gameViewOffset_.x + gameViewSize_.x, gameViewOffset_.y + gameViewSize_.y), true);
    auto& objects = sceneManager_->GetCurrentScene()->GetObjects();

    // 同じ受信 ID の Object が複数ある場合に、送信元から各受信先への接続を描けるよう一覧で保持する。
    std::unordered_map<std::int32_t, std::vector<engine::Object3D*>> eventMap;

    for (const auto& obj : objects) {
        std::int32_t eId = obj->GetEventId();
        // 未設定の -1 は送信先の索引に含めない。
        if (eId != -1)
            eventMap[eId].push_back(obj.get());
    }

    for (const auto& obj : objects) {
        // 非表示の Object を接続線やラベルの描画起点にしない。
        if (!obj->GetIsRenderVisible())
            continue;

        // イベント関連のIDを取得
        std::int32_t myId = obj->GetEventId();
        std::int32_t targetId = obj->GetTargetId();

        // 送信先 ID が指定され、受信する Object がある場合だけ接続線を描く。
        if (targetId != -1 && eventMap.count(targetId)) {
            engine::Vector3 startPos = obj->GetWorldPosition();
            engine::Vector3 startScreen = WorldToScreen(startPos);

            // 送信元が投影の深度範囲に入る場合に、すべての受信先を調べる。
            if (startScreen.z >= 0.0f) {
                for (engine::Object3D* targetObj : eventMap[targetId]) {
                    engine::Vector3 endPos = targetObj->GetWorldPosition();
                    engine::Vector3 endScreen = WorldToScreen(endPos);

                    // 受信先も描画深度に入る場合だけ、線と終点の丸を描く。
                    if (endScreen.z >= 0.0f) {
                        // 線を描画 (少し透明な水色)
                        drawList->AddLine(
                            ImVec2(startScreen.x, startScreen.y),
                            ImVec2(endScreen.x, endScreen.y),
                            IM_COL32(0, 255, 255, 150),
                            2.0f
                        );

                        // 終点に小さな丸を描画して方向を示す
                        drawList->AddCircleFilled(ImVec2(endScreen.x, endScreen.y), 4.0f, IM_COL32(0, 255, 255, 200));
                    }
                }
            }
        }

        // 送受信のうち実際に設定された ID をラベルへ追加する。
        std::string label = "";
        // 未設定でない受信 ID を指紋のアイコンで示す。
        if (myId != -1)
            label += ICON_FA_FINGERPRINT " [ID: " + std::to_string(myId) + "] ";
        // 未設定でない送信先 ID を照準のアイコンで示す。
        if (targetId != -1)
            label += ICON_FA_BULLSEYE " [Target: " + std::to_string(targetId) + "]";

        // 表示する ID が一つもなければ、ラベルの位置を計算しない。
        if (label.empty())
            continue;

        // 3D座標の取得と変換（頭上に表示）
        engine::Vector3 worldPos = obj->GetWorldPosition();
        worldPos.y += obj->GetTransform()->scale.y + 0.5f;

        engine::Vector3 screenPos = WorldToScreen(worldPos);

        // ラベル位置が描画深度内にある場合だけ Game View へ重ねる。
        if (screenPos.z >= 0.0f) {
            ImVec2 pos = ImVec2(screenPos.x, screenPos.y);
            ImU32 color = (myId != -1) ? IM_COL32(255, 255, 0, 255) : IM_COL32(100, 200, 255, 255);
            // 選択中は黄色・水色の種類別表示より、白を優先する。
            if (IsObjectSelected(obj.get()))
                color = IM_COL32(255, 255, 255, 255); // 選択中は白

            // アウトライン
            drawList->AddText(NULL, 0.0f, ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, 255), label.c_str());

            // 本体
            drawList->AddText(NULL, 0.0f, pos, color, label.c_str());
        }
    }

    drawList->PopClipRect();
#endif
}

