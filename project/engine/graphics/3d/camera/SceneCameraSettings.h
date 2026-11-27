#pragma once
#include <cstdint>

#include "engine/utility/math/Math.h"
#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <string>

// シーンへ保存するCamera Object専用の設定です。
// CameraManagerは描画用カメラを所有し、この設定は編集・保存・再生の入力データとして扱います。
/// <summary>シーンカメラを通常視点と演出視点のどちらに使うか。</summary>
enum class SceneCameraRole {
    kMain = 0,
    kCinematic = 1,
};

/// <summary>視点位置の取得元。</summary>
enum class SceneCameraEyeSource {
    kCameraObject = 0,
    kSceneObject = 1,
    kSceneObjectRelativeToReference = 2,
};

/// <summary>カメラの注視点を決める方法。</summary>
enum class SceneCameraTargetMode {
    kFixedPoint = 0,
    kSceneObject = 1,
    kCameraForward = 2,
};

/// <summary>視点と注視点の追従方法。</summary>
enum class SceneCameraFollowMode {
    kSnap = 0,
    kSmooth = 1,
};

/// <summary>シーンカメラ切替時の補間曲線。</summary>
enum class SceneCameraEasing {
    kLinear = 0,
    kEaseIn = 1,
    kEaseOut = 2,
    kEaseInOut = 3,
    kSmootherStep = 4,
};

/// <summary>Camera Objectが自動適用される範囲と編集用の表示色。</summary>
// 適用範囲はワールド座標。視点のTransformや親の回転・拡縮とは独立して編集する。
// 関連：CameraEditor（編集）、CameraManager（領域選択）、Object3D（保存）
struct SceneCameraRegionSettings {
    bool enabled = false;
    engine::Vector3 center = {0.0f, 0.0f, 0.0f};
    engine::Vector3 size = {20.0f, 10.0f, 20.0f};
    engine::Vector4 color = {0.25f, 0.8f, 1.0f, 0.9f};
    std::int32_t priority = 0;
    float activationDelay = 0.2f;
    float exitMargin = 0.5f;

    // 戦闘用の範囲だけで手前のバリア上部を抑え、足元の境界は残します。
    bool clearForegroundBarriers = false;
    float barrierFloorHeight = 0.0f;
    float barrierEdgeHeight = 0.6f;
    float barrierOpacity = 0.03f; // 手前バリア上部の発光と屈折に使う不透明度。
    float barrierLatticeOpacity = 0.0f; // 手前の格子と輪郭の不透明度。0なら従来の透過表示。
    bool routeEnabled = false;
    std::string endCameraGuid; // 改名に影響されない終点Camera Objectへの非所有参照。
    engine::Vector3 pathStart{}, pathEnd{}; // プレイヤーが歩く道の始点・終点（ワールド座標）。

    /// 通常は半寸法を使い、選択を維持する判定では退出余白も加えて境界の往復を防ぐ。
    bool Contains(const engine::Vector3& position, bool retain = false) const {
        const float margin = retain ? exitMargin : 0.0f;
        return std::abs(position.x - center.x) <= size.x * 0.5f + margin &&
               std::abs(position.y - center.y) <= size.y * 0.5f + margin &&
               std::abs(position.z - center.z) <= size.z * 0.5f + margin;
    }
};

/// <summary>シーンに保存するカメラ設定。描画カメラそのものは所有しない。</summary>
// 関連：CameraEditor（編集）、CameraManager（領域選択）、Object3D（保存）
struct SceneCameraSettings {
    std::int32_t version = 1;
    bool enabled = true;
    SceneCameraRole role = SceneCameraRole::kCinematic;
    SceneCameraRegionSettings region;

    // Camera::SetFovYと同じく縦画角をラジアンで保持する。クリップ距離はワールド座標の距離。
    float fovY = 0.45f;
    bool blendFov = false;
    float nearClip = 0.1f; // 近クリップ面までの距離。
    float farClip = 1000.0f; // 遠クリップ面までの距離。

    SceneCameraEyeSource eyeSource = SceneCameraEyeSource::kCameraObject;

    // シーン内の物体を視点の基準にする二つのモードで、この名前から基準位置を探す。
    std::string eyeObjectName;
    // 相対視点では二つの参照位置から前方を求める。eyeOffsetは相対視点で右・上・前、その他ではワールド軸で加える。
    std::string eyeReferenceObjectName;
    engine::Vector3 eyeOffset = engine::Vector3::kZero;
    SceneCameraFollowMode eyeFollowMode = SceneCameraFollowMode::kSnap;
    float eyeFollowResponse = 12.0f;

    SceneCameraTargetMode targetMode = SceneCameraTargetMode::kCameraForward;

    // kSceneObjectでは名前とオフセット、kFixedPointでは固定のワールド座標を使用する。
    std::string targetObjectName;
    engine::Vector3 targetOffset = engine::Vector3::kZero;
    engine::Vector3 fixedTarget = engine::Vector3::kZero;
    float forwardDistance = 10.0f;
    SceneCameraFollowMode targetFollowMode = SceneCameraFollowMode::kSnap;
    float targetFollowResponse = 14.0f;

    float blendInDuration = 0.30f; // このカメラへ切り替えるブレンド秒数。
    float blendOutDuration = 0.30f; // 通常カメラへ戻すブレンド秒数。

    // 切替時間は秒。0は補間なしの即時切替を表す。
    SceneCameraEasing easing = SceneCameraEasing::kSmootherStep;
};

/// <summary>シーンカメラの設定を保存用JSONへ変換する。</summary>
inline nlohmann::json SerializeSceneCameraSettings(const SceneCameraSettings& settings) {
    // 保存形式では列挙値を数値にし、視点と注視点の追従設定を同じ階層に揃える。
    nlohmann::json camera;
    camera["version"] = settings.version;
    camera["enabled"] = settings.enabled;
    camera["role"] = static_cast<std::int32_t>(settings.role);
    camera["fovY"] = settings.fovY;
    camera["blendFov"] = settings.blendFov;
    camera["nearClip"] = settings.nearClip;
    camera["farClip"] = settings.farClip;
    camera["eyeSource"] = static_cast<std::int32_t>(settings.eyeSource);
    camera["eyeObjectName"] = settings.eyeObjectName;
    camera["eyeReferenceObjectName"] = settings.eyeReferenceObjectName;
    camera["eyeOffset"] = { settings.eyeOffset.x, settings.eyeOffset.y, settings.eyeOffset.z };
    camera["eyeFollowMode"] = static_cast<std::int32_t>(settings.eyeFollowMode);
    camera["eyeFollowResponse"] = settings.eyeFollowResponse;
    camera["targetMode"] = static_cast<std::int32_t>(settings.targetMode);
    camera["targetObjectName"] = settings.targetObjectName;
    camera["targetOffset"] = { settings.targetOffset.x, settings.targetOffset.y, settings.targetOffset.z };
    camera["fixedTarget"] = { settings.fixedTarget.x, settings.fixedTarget.y, settings.fixedTarget.z };
    camera["forwardDistance"] = settings.forwardDistance;
    camera["targetFollowMode"] = static_cast<std::int32_t>(settings.targetFollowMode);
    camera["targetFollowResponse"] = settings.targetFollowResponse;
    camera["blendInDuration"] = settings.blendInDuration;
    camera["blendOutDuration"] = settings.blendOutDuration;
    camera["easing"] = static_cast<std::int32_t>(settings.easing);
    // 視点や注視点と別に、カメラが自動で切り替わる領域の設定をまとめる。
    const auto& region = settings.region;
    camera["region"] = {
        {"enabled", region.enabled}, {"center", {region.center.x, region.center.y, region.center.z}},
        {"size", {region.size.x, region.size.y, region.size.z}},
        {"color", {region.color.x, region.color.y, region.color.z, region.color.w}},
        {"priority", region.priority}, {"activationDelay", region.activationDelay}, {"exitMargin", region.exitMargin},
        {"clearForegroundBarriers", region.clearForegroundBarriers},
        {"barrierFloorHeight", region.barrierFloorHeight}, {"barrierEdgeHeight", region.barrierEdgeHeight},
        {"barrierOpacity", region.barrierOpacity},
        {"routeEnabled", region.routeEnabled}, {"endCameraGuid", region.endCameraGuid},
        {"pathStart", {region.pathStart.x,region.pathStart.y,region.pathStart.z}},
        {"pathEnd", {region.pathEnd.x,region.pathEnd.y,region.pathEnd.z}}
    };
    // 格子の不透明度が正のときだけ、従来設定では使わない追加キーを書き出す。
    if (region.barrierLatticeOpacity > 0.0f)
        camera["region"]["barrierLatticeOpacity"] = region.barrierLatticeOpacity;
    return camera;
}

/// <summary>JSONにある項目だけを既存設定へ読み込み、数値を有効範囲に収める。</summary>
/// <remarks>入力がオブジェクトでなければ何もしない。通常項目は現在値を維持し、region未設定は旧データとの互換性のため無効へ戻す。</remarks>
inline void DeserializeSceneCameraSettings(const nlohmann::json& camera, SceneCameraSettings& settings) {
    // キーで設定を取り出せない入力は、既存値を書き換えず終了する。
    if (!camera.is_object()) {
        return;
    }

    auto readVector3 = [&camera](const char* key, engine::Vector3& value) {
        // 通常の座標項目では、配列の先頭三成分を取り出せる場合にだけ反映する。
        if (camera.contains(key) && camera[key].is_array() && camera[key].size() >= 3) {
            value = { camera[key][0], camera[key][1], camera[key][2] };
        }
    };

    // バージョンが記録されている場合だけ、現在の設定形式番号へ反映する。
    if (camera.contains("version"))
        settings.version = camera["version"].get<std::int32_t>();
    // カメラの有効状態が記録されている場合は、その指定を引き継ぐ。
    if (camera.contains("enabled"))
        settings.enabled = camera["enabled"].get<bool>();
    // 用途の指定がある場合は、通常と演出の二つの有効値に収める。
    if (camera.contains("role"))
        settings.role = static_cast<SceneCameraRole>(std::clamp(camera["role"].get<std::int32_t>(), 0, 1));
    // 縦画角がある場合は、ラジアンの値を読み、読込後に範囲補正する。
    if (camera.contains("fovY"))
        settings.fovY = camera["fovY"].get<float>();
    // 画角の補間指定は真偽値として存在する場合だけ採用する。
    if (camera.contains("blendFov") && camera["blendFov"].is_boolean())
        settings.blendFov = camera["blendFov"].get<bool>();
    // 近クリップ距離が指定されている場合は読み込み、射影の下限は後で補う。
    if (camera.contains("nearClip"))
        settings.nearClip = camera["nearClip"].get<float>();
    // 遠クリップ距離が指定されている場合は読み込み、近面との差は後で確保する。
    if (camera.contains("farClip"))
        settings.farClip = camera["farClip"].get<float>();
    // 視点をどこから得るかの指定があれば、定義された0〜2の値へ収める。
    if (camera.contains("eyeSource"))
        settings.eyeSource = static_cast<SceneCameraEyeSource>(std::clamp(camera["eyeSource"].get<std::int32_t>(), 0, 2));
    // 視点の基準物体名を文字列で指定している場合だけ、名前を設定する。
    if (camera.contains("eyeObjectName") && camera["eyeObjectName"].is_string())
        settings.eyeObjectName = camera["eyeObjectName"].get<std::string>();
    // 方向を決める参照先は、名前が文字列で保存されている場合だけ設定する。
    if (camera.contains("eyeReferenceObjectName") && camera["eyeReferenceObjectName"].is_string())
        settings.eyeReferenceObjectName = camera["eyeReferenceObjectName"].get<std::string>();
    readVector3("eyeOffset", settings.eyeOffset);
    // 視点の追従方式が指定されている場合は、即時と滑らかの二種類に収める。
    if (camera.contains("eyeFollowMode"))
        settings.eyeFollowMode = static_cast<SceneCameraFollowMode>(std::clamp(camera["eyeFollowMode"].get<std::int32_t>(), 0, 1));
    // 視点の追従速度が指定されている場合は、補正用に読み込む。
    if (camera.contains("eyeFollowResponse"))
        settings.eyeFollowResponse = camera["eyeFollowResponse"].get<float>();
    // 注視点の方式が指定されている場合は、定義された0〜2の値へ収める。
    if (camera.contains("targetMode"))
        settings.targetMode = static_cast<SceneCameraTargetMode>(std::clamp(camera["targetMode"].get<std::int32_t>(), 0, 2));
    // 注視対象の名前は、JSONの文字列として存在する場合だけ反映する。
    if (camera.contains("targetObjectName") && camera["targetObjectName"].is_string())
        settings.targetObjectName = camera["targetObjectName"].get<std::string>();
    readVector3("targetOffset", settings.targetOffset);
    readVector3("fixedTarget", settings.fixedTarget);
    // 前方向を注視する距離が保存されている場合は、下限補正前の入力として読む。
    if (camera.contains("forwardDistance"))
        settings.forwardDistance = camera["forwardDistance"].get<float>();
    // 注視点の追従方式が指定されている場合は、即時と滑らかの二種類に収める。
    if (camera.contains("targetFollowMode"))
        settings.targetFollowMode = static_cast<SceneCameraFollowMode>(std::clamp(camera["targetFollowMode"].get<std::int32_t>(), 0, 1));
    // 注視点の追従速度が指定されている場合だけ読み込む。
    if (camera.contains("targetFollowResponse"))
        settings.targetFollowResponse = camera["targetFollowResponse"].get<float>();
    // このカメラへ切り替える秒数が保存されている場合だけ読み込む。
    if (camera.contains("blendInDuration"))
        settings.blendInDuration = camera["blendInDuration"].get<float>();
    // 通常カメラへ戻す秒数が保存されている場合だけ読み込む。
    if (camera.contains("blendOutDuration"))
        settings.blendOutDuration = camera["blendOutDuration"].get<float>();
    // 補間曲線が指定されている場合は、定義した五種類の範囲に収める。
    if (camera.contains("easing"))
        settings.easing = static_cast<SceneCameraEasing>(std::clamp(camera["easing"].get<std::int32_t>(), 0, 4));

    // 古いJSONと項目追加前へのUndoでは、範囲だけを既定値へ戻す。
    settings.region = {};
    // 領域がオブジェクトで指定された場合だけ、初期値の上に範囲と経路を反映する。
    if (camera.contains("region") && camera["region"].is_object()) {
        const auto& data = camera["region"];
        auto& region = settings.region;
        // 領域の有効指定が真偽値の場合だけ、初期値から変更する。
        if (data.contains("enabled") && data["enabled"].is_boolean()) region.enabled = data["enabled"].get<bool>();
        // 優先度が整数として指定されている場合だけ、-10000〜10000へ収めて適用する。
        if (data.contains("priority") && data["priority"].is_number_integer())
            region.priority = std::clamp(data["priority"].get<std::int32_t>(), -10000, 10000);
        auto number = [&](const char* key, float fallback, float minimum, float maximum) {
            // キーが無いか数値以外の場合は、この項目の既定値を返す。
            if (!data.contains(key) || !data[key].is_number()) return fallback;
            const float value = data[key].get<float>();
            return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
        };
        // 通常は無効の手前バリア透過を、対象の領域だけで設定します。床と境界の高さはワールド単位です。
        if (data.contains("clearForegroundBarriers") && data["clearForegroundBarriers"].is_boolean())
            region.clearForegroundBarriers = data["clearForegroundBarriers"].get<bool>();
        region.barrierFloorHeight = number("barrierFloorHeight", 0.0f, -1000000.0f, 1000000.0f);
        region.barrierEdgeHeight = number("barrierEdgeHeight", 0.6f, 0.0f, 5.0f);
        region.barrierOpacity = number("barrierOpacity", 0.03f, 0.0f, 1.0f);
        region.barrierLatticeOpacity = number("barrierLatticeOpacity", 0.0f, 0.0f, 1.0f);
        region.activationDelay = number("activationDelay", 0.2f, 0.0f, 10.0f);
        region.exitMargin = number("exitMargin", 0.5f, 0.0f, 20.0f);
        auto vector = [&](const char* key, float* destination, std::int32_t count, float minimum, float maximum) {
            // 指定キーが無いか要求と同じ成分数の配列でない場合は、現在のベクトルを保持する。
            if (!data.contains(key) || !data[key].is_array() || data[key].size() != count) return;
            for (std::int32_t i = 0; i < count; ++i) {
                // 配列の成分が数値以外なら、その成分だけを飛ばす。
                if (!data[key][i].is_number()) continue;
                const float value = data[key][i].get<float>();
                // 有限の成分だけを値域へ収めて反映し、無効値は現在の成分を保つ。
                if (std::isfinite(value)) destination[i] = std::clamp(value, minimum, maximum);
            }
        };
        // 経路自体の座標と、終点に使うカメラの識別子を読み込みます。終点解決は実行時に行います。
        if (data.contains("routeEnabled") && data["routeEnabled"].is_boolean()) region.routeEnabled = data["routeEnabled"].get<bool>();
        // 終点カメラのGUIDが文字列として指定されていれば、参照を解決するため保持する。
        if (data.contains("endCameraGuid") && data["endCameraGuid"].is_string()) region.endCameraGuid = data["endCameraGuid"].get<std::string>();
        vector("pathStart", &region.pathStart.x, 3, -1000000.0f, 1000000.0f);
        vector("pathEnd", &region.pathEnd.x, 3, -1000000.0f, 1000000.0f);
        vector("center", &region.center.x, 3, -1000000.0f, 1000000.0f);
        vector("size", &region.size.x, 3, 0.1f, 100000.0f);
        vector("color", &region.color.x, 4, 0.0f, 1.0f);
    }

    // 射影行列や追従補間が破綻しない範囲へ、読み取り後にまとめて補正する。
    settings.fovY = std::clamp(settings.fovY, 0.05f, 3.0f);
    settings.nearClip = (std::max)(settings.nearClip, 0.001f);
    // 近クリップ面より遠クリップ面を奥に置き、投影に必要な正の距離差を確保します。
    settings.farClip = (std::max)(settings.farClip, settings.nearClip + 0.01f);
    settings.eyeFollowResponse = (std::max)(settings.eyeFollowResponse, 0.01f);
    settings.targetFollowResponse = (std::max)(settings.targetFollowResponse, 0.01f);
    settings.forwardDistance = (std::max)(settings.forwardDistance, 0.01f);
    settings.blendInDuration = (std::max)(settings.blendInDuration, 0.0f);
    settings.blendOutDuration = (std::max)(settings.blendOutDuration, 0.0f);
}
