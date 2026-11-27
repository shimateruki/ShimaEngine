#include <cstdint>
#include "engine/editor/graphics/camera/CameraEditor.h"

#include "DebugConsole.h"
#include "PostEffect.h"
#include "json.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
// ファイル名は既定フォルダー基準、フォルダー付きの指定はそのパスとして扱う。
// UTF-8からpathへ変換し、日本語を含む保存先もWindowsのネイティブパスへ渡す。
fs::path ResolveSettingsFilePath(const std::string& name, const std::string& directory) {
    const fs::path path = fs::path(std::u8string(name.begin(), name.end()));
    return (path.is_absolute() || path.has_parent_path() ? path : fs::path(std::u8string(directory.begin(), directory.end())) / path).lexically_normal();
}
}


// 設定フォルダー直下の .json を一覧化する。カメラの作業状態ファイルは除く。
void CameraEditor::RefreshFileList() {
  fileList_.clear();

  // 保存先が存在しない場合は先に作成する。
  if (!fs::exists(kDirectoryPath_)) {
    fs::create_directories(kDirectoryPath_);
    return;
  }

  // ディレクトリ内を走査して .json だけリストに追加
  for (const auto &entry : fs::directory_iterator(kDirectoryPath_)) {
    // 拡張子が .json の項目だけを設定ファイル名の候補にする。
    if (entry.path().extension() == ".json") {
      std::string fileName = entry.path().filename().string();

      // エディタ状態保存用のファイルはリストに表示しない
      if (fileName == "editor_camera_state.json")
        continue;
      fileList_.push_back(fileName);
    }
  }
}

// 本編カメラの設定と名前付き演出を保存し、選択一覧も読み直す。
void CameraEditor::SaveSettings() {
  // 読み込みと同じ規則で保存先を解決し、既定フォルダーを二重に付けない。
  const fs::path filePath = ResolveSettingsFilePath(fileNameBuffer_, kDirectoryPath_);

  // ボス構図は保存前に補正し、最小・最大距離などの関係を揃える。
  settings_.bossFraming.Validate();
  json j;
  const auto& battle = settings_.bossFraming;
  j["bossFraming"] = {{"enabled", battle.enabled}, {"minDistance", battle.minDistance}, {"maxDistance", battle.maxDistance}, {"height", battle.height}, {"pitch", battle.pitch}, {"fov", battle.fov}, {"targetWeight", battle.targetWeight}, {"turnSpeed", battle.turnSpeed}, {"response", battle.response}};
  j["gameFollowMode"] = static_cast<std::int32_t>(settings_.gameFollowMode);
  j["gameplayStyle"] = static_cast<std::int32_t>(settings_.gameplayStyle);
  j["guidedControl"] = {{"yawLimit", settings_.guidedYawLimit}, {"pitchLimit", settings_.guidedPitchLimit},
      {"returnResponse", settings_.guidedReturnResponse}, {"fov", settings_.guidedFov},
      {"lookAhead", settings_.guidedLookAhead}, {"deadZoneWidth", settings_.guidedDeadZoneWidth},
      {"deadZoneHeight", settings_.guidedDeadZoneHeight}, {"framingResponse", settings_.guidedFramingResponse},
      {"obstacleResponse", static_cast<std::int32_t>(settings_.obstacleResponse)}};
  j["distance"] = settings_.distance;
  j["height"] = settings_.height;
  j["stabilizeJumpHeight"] = settings_.stabilizeJumpHeight;
  j["jumpHeightAllowance"] = settings_.jumpHeightAllowance;
  j["visibility"] = settings_.visibility.ToJson();
  j["angle"] = {settings_.angle.x, settings_.angle.y, settings_.angle.z};
  j["lockOnOffset"] = {settings_.lockOnOffset.x, settings_.lockOnOffset.y,
                       settings_.lockOnOffset.z};

  // 周回の中心・半径・速度は、表示用ガイドの設定と分けて保存する。
  j["orbitRadius"] = settings_.orbitRadius;
  j["orbitCenterOffset"] = {settings_.orbitCenterOffset.x,
                            settings_.orbitCenterOffset.y,
                            settings_.orbitCenterOffset.z};
  j["orbitCenterHeight"] = settings_.orbitCenterHeight;
  j["orbitHeight"] = settings_.orbitHeight;
  j["orbitSpeed"] = settings_.orbitSpeed;
  j["orbitStartAngleDeg"] = settings_.orbitStartAngleDeg;
  j["orbitGuideVisible"] = settings_.orbitGuideVisible;
  j["orbitCenterGizmoVisible"] = settings_.orbitCenterGizmoVisible;
  j["orbitGuideMarkerSize"] = settings_.orbitGuideMarkerSize;
  // カメラ入力とエディターのガイド・プレビュー表示設定を保存する。
  j["cameraSensitivity"] = settings_.cameraSensitivity;
  j["cameraGuideVisible"] = settings_.cameraGuideVisible;
  j["cameraBodyVisible"] = settings_.cameraBodyVisible;
  j["cameraPreviewVisible"] = settings_.cameraPreviewVisible;
  j["cameraPreviewFps"] = settings_.cameraPreviewFps;
  j["cameraPreviewResolutionScale"] = settings_.cameraPreviewResolutionScale;
  j["cameraGuideSize"] = settings_.cameraGuideSize;
  j["cameraFrustumLength"] = settings_.cameraFrustumLength;
  j["cameraPreviewHeight"] = settings_.cameraPreviewHeight;

  // エディター操作の速度と固定視点の初期設定を保存する。
  j["moveSpeed"] = settings_.moveSpeed;
  j["boostSpeed"] = settings_.boostSpeed;
  j["mouseSensitivity"] = settings_.mouseSensitivity;
  j["fixedPointPos"] = {settings_.fixedPointPos.x, settings_.fixedPointPos.y,
                        settings_.fixedPointPos.z};
  j["fixedPointAngle"] = {settings_.fixedPointAngle.x,
                          settings_.fixedPointAngle.y,
                          settings_.fixedPointAngle.z};

  // 現在の編集カメラの姿勢は、プリセットではなく SaveEditorState() が保存する。

  json overridesJson = json::object();

  // 演出名をキーにして、各視点の取得元と注視方法、補間設定を保存する。
  for (const auto &[name, param] : overrideParamsMap_) {
    json p;
    p["duration"] = param.duration;
    p["exitDuration"] = param.exitDuration;
    p["easing"] = static_cast<std::int32_t>(param.easing);
    p["eyeSource"] = static_cast<std::int32_t>(param.eyeSource);
    p["eyeObjectName"] = param.eyeObjectName;
    p["eyeObjectOffset"] = {param.eyeObjectOffset.x, param.eyeObjectOffset.y,
                            param.eyeObjectOffset.z};
    p["eyeFollowMode"] = static_cast<std::int32_t>(param.eyeFollowMode);
    p["eyeFollowResponse"] = param.eyeFollowResponse;
    p["trackEyeX"] = param.trackEyeX;
    p["trackEyeY"] = param.trackEyeY;
    p["trackEyeZ"] = param.trackEyeZ;
    p["fixedEyePos"] = {param.fixedEyePos.x, param.fixedEyePos.y,
                        param.fixedEyePos.z};

    p["targetSource"] = static_cast<std::int32_t>(param.targetSource);
    p["targetObjectName"] = param.targetObjectName;
    p["targetObjectOffset"] = {param.targetObjectOffset.x,
                               param.targetObjectOffset.y,
                               param.targetObjectOffset.z};
    p["eyeForwardDistance"] = param.eyeForwardDistance;
    p["targetFollowMode"] = static_cast<std::int32_t>(param.targetFollowMode);
    p["targetFollowResponse"] = param.targetFollowResponse;
    p["trackTargetX"] = param.trackTargetX;
    p["trackTargetY"] = param.trackTargetY;
    p["trackTargetZ"] = param.trackTargetZ;
    p["fixedTargetPos"] = {param.fixedTargetPos.x, param.fixedTargetPos.y,
                           param.fixedTargetPos.z};

    overridesJson[name] = p; // JSONオブジェクトに追加
  }

  j["Overrides"] =
      overridesJson; // 大元のJSONに「Overrides」という項目でまとめる
  std::ofstream file(filePath);

  // ファイルを開けた場合にだけ JSON を出力する。一覧は出力の成否にかかわらず更新する。
  if (file.is_open()) {
    file << j.dump(4);
  }

  RefreshFileList();
}

// 選択した設定ファイルを開き、現行設定へ読み込んでプレビューを無効化する。
void CameraEditor::LoadSettings() {
  // ファイル名とScene Assetのパス指定を、保存時と同じ規則で解決する。
  const fs::path filePath = ResolveSettingsFilePath(fileNameBuffer_, kDirectoryPath_);

  std::ifstream file(filePath);
  // 設定ファイルを開けない場合は、現在のカメラ設定を維持する。
  if (!file.is_open())
    return;

  try {
    json j;
    file >> j;

    /*  if (j.contains("mode")) settings_.currentMode =
     * static_cast<Mode>(j["mode"]);*/
    // 追従モードが含まれている場合だけ、その列挙値を復元する。
    if (j.contains("gameFollowMode"))
      settings_.gameFollowMode =
          static_cast<engine::Camera::FollowMode>(j["gameFollowMode"]);

    // 旧JSONには操作方式がないため、従来の自由操作として読み込む。
    settings_.gameplayStyle = j.contains("gameplayStyle") && j["gameplayStyle"].is_number_integer() && j["gameplayStyle"].get<std::int32_t>() == 1 ? engine::Camera::GameplayStyle::kGuided : engine::Camera::GameplayStyle::kFree;
    // 旧ファイルに設定がなくても、シーンからボスが指定された間だけ既定構図を使う。
    settings_.bossFraming = {};
    // ボス構図のオブジェクトがある場合にだけ、その項目を初期値へ重ねる。
    if (j.contains("bossFraming") && j["bossFraming"].is_object()) {
      const auto& battle = j["bossFraming"];
      // 有効の設定は真偽値の場合にだけ、既定の有効状態へ上書きする。
      if (battle.contains("enabled") && battle["enabled"].is_boolean())
        settings_.bossFraming.enabled = battle["enabled"].get<bool>();
      // 最小距離が数値で保存されていれば、最大距離とは独立に反映する。
      if (battle.contains("minDistance") && battle["minDistance"].is_number()) settings_.bossFraming.minDistance = battle["minDistance"].get<float>();
      // 最大距離が数値で保存されていれば反映する。両端の関係は読込後に補正する。
      if (battle.contains("maxDistance") && battle["maxDistance"].is_number()) settings_.bossFraming.maxDistance = battle["maxDistance"].get<float>();
      // 高さが数値の場合にだけ、ボス構図の視点高を更新する。
      if (battle.contains("height") && battle["height"].is_number()) settings_.bossFraming.height = battle["height"].get<float>();
      // ピッチが数値の場合にだけ、視点の角度を更新する。
      if (battle.contains("pitch") && battle["pitch"].is_number()) settings_.bossFraming.pitch = battle["pitch"].get<float>();
      // 画角が数値の場合にだけ、ボス戦用の画角へ反映する。
      if (battle.contains("fov") && battle["fov"].is_number()) settings_.bossFraming.fov = battle["fov"].get<float>();
      // 対象側への重みが数値の場合に、構図内の注視位置の設定へ反映する。
      if (battle.contains("targetWeight") && battle["targetWeight"].is_number()) settings_.bossFraming.targetWeight = battle["targetWeight"].get<float>();
      // 旋回速度が数値の場合に、ボス戦用の旋回係数へ反映する。
      if (battle.contains("turnSpeed") && battle["turnSpeed"].is_number()) settings_.bossFraming.turnSpeed = battle["turnSpeed"].get<float>();
      // 応答値が数値の場合に、ボス構図の追従係数へ反映する。
      if (battle.contains("response") && battle["response"].is_number()) settings_.bossFraming.response = battle["response"].get<float>();
    }
    settings_.bossFraming.Validate();
    const auto guided = j.value("guidedControl", json::object());
    // 誘導操作の値は項目ごとに取り込む。旧ファイルの欠落や不正な値は既定値、範囲を超えた有限値は調整範囲の端へ戻す。
    auto readGuided = [&](const char* key, float fallback, float lo, float hi) {
      // 設定グループやキーが無い場合、または数値でない場合は項目ごとの既定値にする。
      if (!guided.is_object() || !guided.contains(key) || !guided[key].is_number()) return fallback;
      const float value = guided[key].get<float>();
      return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
    };
    settings_.guidedYawLimit = readGuided("yawLimit", 10, 0, 30);
    settings_.guidedPitchLimit = readGuided("pitchLimit", 5, 0, 15);
    settings_.guidedReturnResponse = readGuided("returnResponse", 5, 0.5f, 20);
    settings_.guidedFov = readGuided("fov", 0.65f, 0.35f, 1.2f);
    settings_.guidedLookAhead = readGuided("lookAhead", 6, 0, 15);
    settings_.guidedDeadZoneWidth = readGuided("deadZoneWidth", .1f, 0, .3f);
    settings_.guidedDeadZoneHeight = readGuided("deadZoneHeight", .1f, 0, .3f);
    settings_.guidedFramingResponse = readGuided("framingResponse", 3, .5f, 15);
    settings_.obstacleResponse = guided.is_object() && guided.contains("obstacleResponse") && guided["obstacleResponse"].is_number_integer() && guided["obstacleResponse"].get<std::int32_t>() == 0
        ? engine::Camera::ObstacleResponse::kShortenDistance : engine::Camera::ObstacleResponse::kPreserveComposition;
    // 追従距離のキーがあれば既存の距離を更新する。
    if (j.contains("distance"))
      settings_.distance = j["distance"];
    // 追従高さは、設定に含まれている場合だけ独立に更新する。
    if (j.contains("height"))
      settings_.height = j["height"];
    settings_.visibility.FromJson(j.value("visibility", json::object()));
    settings_.stabilizeJumpHeight = j.value("stabilizeJumpHeight", true);
    settings_.jumpHeightAllowance = std::clamp(j.value("jumpHeightAllowance", 8.0f), 1.0f, 30.0f);
    // 角度が設定されている場合、保存形式に応じて現在の回転へ展開する。
    if (j.contains("angle")) {
      // 配列形式では順番に X・Y・Z の角度として復元する。
      if (j["angle"].is_array()) {
        settings_.angle.x = j["angle"][0];
        settings_.angle.y = j["angle"][1];
        settings_.angle.z = j["angle"][2];
      } else {
        // 古いデータ(float)の場合はX(Pitch)にだけ入れる
        settings_.angle.x = j["angle"];
        settings_.angle.y = 0.0f;
        settings_.angle.z = 0.0f;
      }
    }

    // ロックオン補正が配列で保存されている場合、保存順に三軸へ戻す。
    if (j.contains("lockOnOffset") && j["lockOnOffset"].is_array()) {
      settings_.lockOnOffset.x = j["lockOnOffset"][0];
      settings_.lockOnOffset.y = j["lockOnOffset"][1];
      settings_.lockOnOffset.z = j["lockOnOffset"][2];
    }

    // 周回モードの個別項目は、旧ファイルで省略されていれば読み込み前の値を引き継ぐ。
    if (j.contains("orbitRadius"))
      settings_.orbitRadius = j["orbitRadius"];
    // 3成分以上の配列があれば、周回の中心補正を先頭の XYZ から復元する。
    if (j.contains("orbitCenterOffset") && j["orbitCenterOffset"].is_array() &&
        j["orbitCenterOffset"].size() >= 3) {
      settings_.orbitCenterOffset.x = j["orbitCenterOffset"][0];
      settings_.orbitCenterOffset.y = j["orbitCenterOffset"][1];
      settings_.orbitCenterOffset.z = j["orbitCenterOffset"][2];
    }

    // 中心の高さが記録されている場合に、周回の基準点の上下位置を戻す。
    if (j.contains("orbitCenterHeight"))
      settings_.orbitCenterHeight = j["orbitCenterHeight"];
    // カメラ側の高さは独立した値として、保存がある場合だけ戻す。
    if (j.contains("orbitHeight"))
      settings_.orbitHeight = j["orbitHeight"];
    // 速度の設定があれば、周回が進む速さを戻す。
    if (j.contains("orbitSpeed"))
      settings_.orbitSpeed = j["orbitSpeed"];
    // 開始角が保存されていれば、度単位で復元する。
    if (j.contains("orbitStartAngleDeg"))
      settings_.orbitStartAngleDeg = j["orbitStartAngleDeg"];
    // 経路ガイドの表示設定があれば、周回のガイドだけを復元する。
    if (j.contains("orbitGuideVisible"))
      settings_.orbitGuideVisible = j["orbitGuideVisible"];
    // 中心ギズモの表示は、個別のキーがある場合だけ復元する。
    if (j.contains("orbitCenterGizmoVisible"))
      settings_.orbitCenterGizmoVisible = j["orbitCenterGizmoVisible"];
    // 位置マーカーの寸法が保存されていれば、その寸法を復元する。
    if (j.contains("orbitGuideMarkerSize"))
      settings_.orbitGuideMarkerSize = j["orbitGuideMarkerSize"];
    // 操作感度が指定されていれば、従来の値から差し替える。
    if (j.contains("cameraSensitivity"))
      settings_.cameraSensitivity = j["cameraSensitivity"];
    // 配置カメラのガイド表示は、設定がある場合だけ差し替える。
    if (j.contains("cameraGuideVisible"))
      settings_.cameraGuideVisible = j["cameraGuideVisible"];
    // カメラ本体モデルの表示設定は、ガイドとは独立に復元する。
    if (j.contains("cameraBodyVisible"))
      settings_.cameraBodyVisible = j["cameraBodyVisible"];
    // プレビュー映像の表示の有無も、保存時に指定された項目だけ上書きする。
    if (j.contains("cameraPreviewVisible"))
      settings_.cameraPreviewVisible = j["cameraPreviewVisible"];
    // プレビューの更新回数があれば読み、後続で1〜60 FPSに収める。
    if (j.contains("cameraPreviewFps"))
      settings_.cameraPreviewFps = j["cameraPreviewFps"];
    // プレビューの解像度倍率があれば読み、後続で0.25〜1に収める。
    if (j.contains("cameraPreviewResolutionScale")) {
      settings_.cameraPreviewResolutionScale =
          j["cameraPreviewResolutionScale"];
    }

    settings_.cameraPreviewFps = std::clamp(settings_.cameraPreviewFps, 1, 60);
    settings_.cameraPreviewResolutionScale =
        std::clamp(settings_.cameraPreviewResolutionScale, 0.25f, 1.0f);
    // 読み込んだ解像度倍率を描画先へ先に適用し、以前の寸法で撮影したプレビューを再利用しない。
    PostEffect::GetInstance()->SetCameraPreviewResolutionScale(
        settings_.cameraPreviewResolutionScale);
    InvalidateCameraPreviews();
    // カメラガイドの大きさが保存されていれば戻す。
    if (j.contains("cameraGuideSize"))
      settings_.cameraGuideSize = j["cameraGuideSize"];
    // 視錐台のガイド長が保存されていれば戻す。
    if (j.contains("cameraFrustumLength"))
      settings_.cameraFrustumLength = j["cameraFrustumLength"];
    // プレビューの枠の高さが保存されていれば戻す。
    if (j.contains("cameraPreviewHeight"))
      settings_.cameraPreviewHeight = j["cameraPreviewHeight"];

    // 編集カメラの移動速度が保存されている場合に戻す。
    if (j.contains("moveSpeed"))
      settings_.moveSpeed = j["moveSpeed"];
    // 加速時の移動速度は、指定がある場合だけ別に復元する。
    if (j.contains("boostSpeed"))
      settings_.boostSpeed = j["boostSpeed"];
    // 編集用マウスの感度が記録されていれば差し替える。
    if (j.contains("mouseSensitivity"))
      settings_.mouseSensitivity = j["mouseSensitivity"];
    // 固定視点の位置が配列の場合に、保存順の XYZ を復元する。
    if (j.contains("fixedPointPos") && j["fixedPointPos"].is_array()) {
      settings_.fixedPointPos.x = j["fixedPointPos"][0];
      settings_.fixedPointPos.y = j["fixedPointPos"][1];
      settings_.fixedPointPos.z = j["fixedPointPos"][2];
    }

    // 固定視点の回転が配列の場合に、保存順の XYZ を復元する。
    if (j.contains("fixedPointAngle") && j["fixedPointAngle"].is_array()) {
      settings_.fixedPointAngle.x = j["fixedPointAngle"][0];
      settings_.fixedPointAngle.y = j["fixedPointAngle"][1];
      settings_.fixedPointAngle.z = j["fixedPointAngle"][2];
    }

    // 編集時の操作カメラ姿勢は LoadEditorState() が専用ファイルから読む。
    // 演出カメラの一覧はファイル単位で入れ替える。新しい設定にない演出名を引き継がない。
    overrideParamsMap_.clear();

    // 演出のキーが存在する場合だけ、保存名とカメラの設定を対応づけて登録する。
    if (j.contains("Overrides")) {
      for (auto &[key, val] : j["Overrides"].items()) {
        engine::Camera::CameraOverrideParams p;
        p.duration = val.value("duration", 1.0f);
        p.exitDuration = val.value("exitDuration", 0.35f);
        p.easing = static_cast<engine::Camera::OverrideEasing>(
            std::clamp(val.value("easing", 4), 0, 4));
        p.eyeSource = static_cast<engine::Camera::OverrideEyeSource>(
            std::clamp(val.value("eyeSource", 0), 0, 1));
        p.eyeObjectName = val.value("eyeObjectName", "");
        // 追従視点の補正に3成分ある場合だけ、既定値から差し替える。
        if (val.contains("eyeObjectOffset") &&
            val["eyeObjectOffset"].is_array() &&
            val["eyeObjectOffset"].size() >= 3) {
          p.eyeObjectOffset = {val["eyeObjectOffset"][0],
                               val["eyeObjectOffset"][1],
                               val["eyeObjectOffset"][2]};
        }

        p.eyeFollowMode = static_cast<engine::Camera::OverrideFollowMode>(
            std::clamp(val.value("eyeFollowMode", 0), 0, 1));
        p.eyeFollowResponse = val.value("eyeFollowResponse", 12.0f);
        p.trackEyeX = val.value("trackEyeX", false);
        p.trackEyeY = val.value("trackEyeY", false);
        p.trackEyeZ = val.value("trackEyeZ", false);

        // 固定の視点位置が保存されている場合は三軸を復元する。
        if (val.contains("fixedEyePos")) {
          p.fixedEyePos = {val["fixedEyePos"][0], val["fixedEyePos"][1],
                           val["fixedEyePos"][2]};
        }

        p.targetSource = static_cast<engine::Camera::OverrideTargetSource>(
            std::clamp(val.value("targetSource", 0), 0, 2));
        p.targetObjectName = val.value("targetObjectName", "");
        // 追従先からの注視補正に3成分ある場合のみ復元する。
        if (val.contains("targetObjectOffset") &&
            val["targetObjectOffset"].is_array() &&
            val["targetObjectOffset"].size() >= 3) {
          p.targetObjectOffset = {val["targetObjectOffset"][0],
                                  val["targetObjectOffset"][1],
                                  val["targetObjectOffset"][2]};
        }

        p.eyeForwardDistance = val.value("eyeForwardDistance", 10.0f);
        p.targetFollowMode = static_cast<engine::Camera::OverrideFollowMode>(
            std::clamp(val.value("targetFollowMode", 0), 0, 1));
        p.targetFollowResponse = val.value("targetFollowResponse", 14.0f);
        p.trackTargetX = val.value("trackTargetX", true);
        p.trackTargetY = val.value("trackTargetY", true);
        p.trackTargetZ = val.value("trackTargetZ", true);

        // 固定注視点の指定がある場合は、保存順の XYZ を使う。
        if (val.contains("fixedTargetPos")) {
          p.fixedTargetPos = {val["fixedTargetPos"][0],
                              val["fixedTargetPos"][1],
                              val["fixedTargetPos"][2]};
        }

        overrideParamsMap_[key] = p;
      }
    }

  } catch (...) {
    // 読込例外は呼出元に伝えず、途中まで反映した値もここでは巻き戻さない。
  }
}

void CameraEditor::LoadFile(const std::string &fileName) {
  // 長すぎる指定でバッファを壊さず、使用中の設定を保持して戻る。
  if (fileName.size() >= sizeof(fileNameBuffer_)) {
    DebugConsole::GetInstance()->AddLog("Camera setting path is too long.");
    return;
  }
  // 設定名を使用中のバッファーへコピーして、保存・読込で同じパスを参照する。
  strcpy_s(fileNameBuffer_, sizeof(fileNameBuffer_), fileName.c_str());

  // Scene Asset がフォルダー付きで指定したパスには、既定フォルダーを付け足さない。
  const fs::path filePath = ResolveSettingsFilePath(fileNameBuffer_, kDirectoryPath_);

  // 指定先がまだ無い場合は、操作モードを保った状態で既定値の保存を試す。
  if (!fs::exists(filePath)) {
    const Mode selectedMode = settings_.currentMode;
    settings_ = Settings(); // デフォルトコンストラクタで初期化

    // シーン固有ファイルが未作成でも、利用中の自由カメラを解除しません。
    settings_.currentMode = selectedMode;

    DebugConsole::GetInstance()->AddLog("New Camera Setting Created: " +
                                        fileName);

    SaveSettings();
  }

  // 既存ファイルと新規設定を同じ LoadSettings() の経路で適用する。
  LoadSettings();
}

void CameraEditor::SaveEditorState() {
  // カメラ操作中の一時状態はゲーム設定と分けて保存し、保存禁止期間は書き換えない。
  if (IsEditorStateSaveBlocked())
    return;

  const fs::path filePath(kEditorStatePath_);
  std::error_code error;
  fs::create_directories(filePath.parent_path(), error);

  // 保存ディレクトリを用意できない場合は、変更待ちフラグを残して終了する。
  if (error) {
    return;
  }

  json j;
  j["editorCameraPos"] = {settings_.editorCameraPos.x,
                          settings_.editorCameraPos.y,
                          settings_.editorCameraPos.z};
  j["editorCameraAngle"] = {settings_.editorCameraAngle.x,
                            settings_.editorCameraAngle.y,
                            settings_.editorCameraAngle.z};
  std::ofstream file(filePath);

  // ファイルを開けた場合だけ書き込み、成功を確認してから保存待ちを解除する。
  if (file.is_open()) {
    file << j.dump(4);

    // 姿勢をファイルへ書けたときだけ保存待ちを解除する。
    if (file.good()) {
      editorStateDirty_ = false;
    }
  }
}

// 専用ファイルから編集視点の位置と角度を復元する。なければ旧配置も調べる。
void CameraEditor::LoadEditorState() {
  fs::path filePath(kEditorStatePath_);

  if (!fs::exists(filePath)) {
    // 新しい保存先にファイルがなければ、旧ファイルを今回の読込元にする。
    filePath = fs::path(kDirectoryPath_) / "editor_camera_state.json";
  }

  std::ifstream file(filePath);
  // どちらの保存先でも開けなければ、現在の編集視点を維持する。
  if (!file.is_open())
    return;
  try {
    json j;
    file >> j;

    // 位置が配列で保存されていれば、その XYZ を編集視点へ戻す。
    if (j.contains("editorCameraPos") && j["editorCameraPos"].is_array()) {
      settings_.editorCameraPos.x = j["editorCameraPos"][0];
      settings_.editorCameraPos.y = j["editorCameraPos"][1];
      settings_.editorCameraPos.z = j["editorCameraPos"][2];
    }

    // 回転が配列で保存されていれば、位置とは独立に戻す。
    if (j.contains("editorCameraAngle") && j["editorCameraAngle"].is_array()) {
      settings_.editorCameraAngle.x = j["editorCameraAngle"][0];
      settings_.editorCameraAngle.y = j["editorCameraAngle"][1];
      settings_.editorCameraAngle.z = j["editorCameraAngle"][2];
    }

    editorStateDirty_ = false;
  } catch (...) {
  }
}
