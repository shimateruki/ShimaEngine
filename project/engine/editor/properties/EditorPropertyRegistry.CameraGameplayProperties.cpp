#include <cstdint>
#include "EditorPropertyRegistry.h"
#include "EditorPropertyRegistry.BuiltIn.h"
#include "Object3D.h"

#include <algorithm>

using nlohmann::json;
using editorPropertyBuiltIn::CommonFlags;

void EditorPropertyRegistry::RegisterBuiltInCameraAndGameplayProperties() {
    // Cameraオブジェクトと、汎用EntityParameterを持つゲーム用Objectに編集項目を登録する。
    const auto cameraOnly = [](const engine::Object3D& object) {
        return object.IsCameraObject();
    };

    const auto gameplayParameterOnly = [](const engine::Object3D& object) {
        const std::string className = object.GetClassName();
        return object.GetParameters().has_value() && className != "Player" && className != "Enemy";
    };

    // Cameraの有効性と役割は種類を限定し、誤った型・範囲のJSONを受け付けない。
    Register({
        "camera.enabled", "Camera有効", "Camera", EditorPropertyType::kBool, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetSceneCameraSettings().enabled); },
        [](engine::Object3D& object, const json& value) {
            // 有効フラグは JSON の真偽値だけを受け付ける。異なる型では設定を変更しない。
            if (!value.is_boolean())
                return false;
            object.GetSceneCameraSettings().enabled = value.get<bool>();
            return true;
        },
        cameraOnly,
    });
    Register({
        "camera.role", "Camera役割", "Camera", EditorPropertyType::kInteger, CommonFlags(),
        [](const engine::Object3D& object) { return json(static_cast<std::int32_t>(object.GetSceneCameraSettings().role)); },
        [](engine::Object3D& object, const json& value) {
            // 役割は整数で受け、範囲外の番号は Main と Cinematic の範囲に収める。
            if (!value.is_number_integer())
                return false;
            object.GetSceneCameraSettings().role = static_cast<SceneCameraRole>(std::clamp(
                value.get<std::int32_t>(), static_cast<std::int32_t>(SceneCameraRole::kMain), static_cast<std::int32_t>(SceneCameraRole::kCinematic)));
            return true;
        },
        cameraOnly,
        { "Main", "Cinematic" },
    });

    // 共通の保存読込で検証し、専用InspectorとPrefab編集で範囲の制限を揃える。
    auto registerRegion = [&](const std::string& key, const std::string& label, EditorPropertyType type) {
        Register({"camera.region." + key, label, "Camera", type, CommonFlags(),
            [key](const engine::Object3D& object) { return SerializeSceneCameraSettings(object.GetSceneCameraSettings())["region"][key]; },
            [key, type](engine::Object3D& object, const json& value) {
                const std::int32_t dimensions = type == EditorPropertyType::kVector3 ? 3 : type == EditorPropertyType::kVector4 ? 4 : 0;
                // 中心座標や範囲色のようなベクトル項目は、指定された成分数の配列を要求する。
                if (dimensions) {
                    // 成分数が違うときは既存範囲を維持する。
                    if (!value.is_array() || value.size() != dimensions) return false;
                    // 一つでも非数値の成分があればベクトル全体を拒否する。
                    for (const auto& component : value) if (!component.is_number()) return false;
                } else {
                    // 単独値は登録した種類に応じて真偽値・整数・数値を受け付ける。
                    bool valid;
                    if (type == EditorPropertyType::kBool) {
                        valid = value.is_boolean();
                    } else if (type == EditorPropertyType::kInteger) {
                        valid = value.is_number_integer();
                    } else {
                        valid = value.is_number();
                    }
                    if (!valid) {
                        return false;
                    }
                }
                // 一項目だけ上書きした全設定を共通のデシリアライザへ通し、専用 UI と同じ範囲補正を受ける。
                json camera = SerializeSceneCameraSettings(object.GetSceneCameraSettings());
                camera["region"][key] = value;
                DeserializeSceneCameraSettings(camera, object.GetSceneCameraSettings());
                return true;
            }, cameraOnly});
    };
    registerRegion("enabled", "範囲内で自動適用", EditorPropertyType::kBool);
    registerRegion("center", "範囲の中心", EditorPropertyType::kVector3);
    registerRegion("size", "範囲の大きさ", EditorPropertyType::kVector3);
    registerRegion("color", "範囲の色", EditorPropertyType::kVector4);
    registerRegion("priority", "範囲の優先度", EditorPropertyType::kInteger);
    registerRegion("activationDelay", "範囲の切替待ち時間", EditorPropertyType::kNumber);
    registerRegion("exitMargin", "範囲の退出余白", EditorPropertyType::kNumber);
    registerRegion("clearForegroundBarriers", "手前バリアの上部を透過", EditorPropertyType::kBool);
    registerRegion("barrierFloorHeight", "バリアの床高さ", EditorPropertyType::kNumber);
    registerRegion("barrierEdgeHeight", "バリアの足元に残す高さ", EditorPropertyType::kNumber);
    registerRegion("barrierOpacity", "手前バリアの不透明度", EditorPropertyType::kNumber);
    registerRegion("barrierLatticeOpacity", "手前バリアの格子の不透明度", EditorPropertyType::kNumber);

    // 視野角とClip距離は、描画行列が破綻しない範囲へ制限する。
    Register({
        "camera.fov", "FOV", "Camera", EditorPropertyType::kNumber, CommonFlags(true),
        [](const engine::Object3D& object) { return json(object.GetSceneCameraSettings().fovY); },
        [](engine::Object3D& object, const json& value) {
            // 視野角の入力が数値でない場合は、現在の投影設定を保つ。
            if (!value.is_number())
                return false;
            // ここへ渡る縦画角はラジアン。度への換算は入力欄だけで、保存や投影行列への値は変えない。
            object.GetSceneCameraSettings().fovY = std::clamp(value.get<float>(), 0.05f, 3.0f);
            return true;
        },
        cameraOnly,
    });
    Register({
        "camera.nearClip", "Near Clip", "Camera", EditorPropertyType::kNumber, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetSceneCameraSettings().nearClip); },
        [](engine::Object3D& object, const json& value) {
            // 近面の距離が数値でなければ、近面も遠面も更新しない。
            if (!value.is_number())
                return false;
            auto& settings = object.GetSceneCameraSettings();
            settings.nearClip = (std::max)(value.get<float>(), 0.001f);
            // 遠面には少なくとも 0.01 ワールド単位の間隔を残す。近面が遠面を追い越したときは遠面も押し出す。
            settings.farClip = (std::max)(settings.farClip, settings.nearClip + 0.01f);
            return true;
        },
        cameraOnly,
    });
    Register({
        "camera.farClip", "Far Clip", "Camera", EditorPropertyType::kNumber, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetSceneCameraSettings().farClip); },
        [](engine::Object3D& object, const json& value) {
            // 遠面は数値の場合だけ読み、既存の近面より0.01以上遠くに制限する。
            if (!value.is_number())
                return false;
            auto& settings = object.GetSceneCameraSettings();
            settings.farClip = (std::max)(value.get<float>(), settings.nearClip + 0.01f);
            return true;
        },
        cameraOnly,
    });

    // 視点の取得元、追従対象、オフセット、追従速度をひとまとまりで公開する。
    Register({
        "camera.eyeSource", "Eye取得元", "Camera", EditorPropertyType::kInteger, CommonFlags(),
        [](const engine::Object3D& object) { return json(static_cast<std::int32_t>(object.GetSceneCameraSettings().eyeSource)); },
        [](engine::Object3D& object, const json& value) {
            // 視点の取得元は整数で受け、登録済みの方式に範囲を制限する。
            if (!value.is_number_integer())
                return false;
            object.GetSceneCameraSettings().eyeSource = static_cast<SceneCameraEyeSource>(std::clamp(
                value.get<std::int32_t>(), static_cast<std::int32_t>(SceneCameraEyeSource::kCameraObject), static_cast<std::int32_t>(SceneCameraEyeSource::kSceneObjectRelativeToReference)));
            return true;
        },
        cameraOnly,
        { "Camera Transform", "Scene Object", "参照Object方向" },
    });
    Register({
        "camera.eyeObject", "Eye追従Object", "Camera", EditorPropertyType::kString, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetSceneCameraSettings().eyeObjectName); },
        [](engine::Object3D& object, const json& value) {
            // 追従対象は名前で解決するため、文字列だけを受け付ける。
            if (!value.is_string())
                return false;
            object.GetSceneCameraSettings().eyeObjectName = value.get<std::string>();
            return true;
        },
        cameraOnly,
    });
    Register({
        "camera.eyeOffset", "Eye Offset", "Camera", EditorPropertyType::kVector3, CommonFlags(true),
        [](const engine::Object3D& object) {
            const engine::Vector3& value = object.GetSceneCameraSettings().eyeOffset;
            return json::array({ value.x, value.y, value.z });
        },
        [](engine::Object3D& object, const json& value) {
            // 視点補正は先頭の XYZ を使うため、3要素未満の配列や他の型を拒否する。
            if (!value.is_array() || value.size() < 3)
                return false;
            object.GetSceneCameraSettings().eyeOffset = { value[0].get<float>(), value[1].get<float>(), value[2].get<float>() };
            return true;
        },
        cameraOnly,
    });
    Register({
        "camera.eyeFollowMode", "Eye追従方式", "Camera", EditorPropertyType::kInteger, CommonFlags(),
        [](const engine::Object3D& object) { return json(static_cast<std::int32_t>(object.GetSceneCameraSettings().eyeFollowMode)); },
        [](engine::Object3D& object, const json& value) {
            // 即時追従と補間追従の方式は整数で受け、列挙の範囲に収める。
            if (!value.is_number_integer())
                return false;
            object.GetSceneCameraSettings().eyeFollowMode =
                static_cast<SceneCameraFollowMode>(std::clamp(
                    value.get<std::int32_t>(), static_cast<std::int32_t>(SceneCameraFollowMode::kSnap), static_cast<std::int32_t>(SceneCameraFollowMode::kSmooth)));
            return true;
        },
        cameraOnly,
        { "完全追従", "Smooth" },
    });
    Register({
        "camera.eyeFollowResponse", "Eye追従レスポンス", "Camera", EditorPropertyType::kNumber, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetSceneCameraSettings().eyeFollowResponse); },
        [](engine::Object3D& object, const json& value) {
            // 視点の追従レスポンスが数値のときだけ、0.01以上へ補正して適用する。
            if (!value.is_number())
                return false;
            object.GetSceneCameraSettings().eyeFollowResponse = (std::max)(value.get<float>(), 0.01f);
            return true;
        },
        cameraOnly,
    });

    // 注視点は固定点、シーン内Object、カメラ前方から選び、追従方法を別に指定する。
    Register({
        "camera.targetMode", "注視方法", "Camera", EditorPropertyType::kInteger, CommonFlags(),
        [](const engine::Object3D& object) { return json(static_cast<std::int32_t>(object.GetSceneCameraSettings().targetMode)); },
        [](engine::Object3D& object, const json& value) {
            // 注視方法は整数の番号で受け、固定点から前方までの列挙範囲へ収める。
            if (!value.is_number_integer())
                return false;
            object.GetSceneCameraSettings().targetMode = static_cast<SceneCameraTargetMode>(std::clamp(
                value.get<std::int32_t>(), static_cast<std::int32_t>(SceneCameraTargetMode::kFixedPoint), static_cast<std::int32_t>(SceneCameraTargetMode::kCameraForward)));
            return true;
        },
        cameraOnly,
        { "固定座標", "Scene Object", "Camera前方" },
    });
    Register({
        "camera.targetObject", "注視Object", "Camera", EditorPropertyType::kString, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetSceneCameraSettings().targetObjectName); },
        [](engine::Object3D& object, const json& value) {
            // 注視対象はシーンの名前で解決するため、文字列だけを保存する。
            if (!value.is_string())
                return false;
            object.GetSceneCameraSettings().targetObjectName = value.get<std::string>();
            return true;
        },
        cameraOnly,
    });
    Register({
        "camera.targetOffset", "Target Offset", "Camera", EditorPropertyType::kVector3, CommonFlags(true),
        [](const engine::Object3D& object) {
            const engine::Vector3& value = object.GetSceneCameraSettings().targetOffset;
            return json::array({ value.x, value.y, value.z });
        },
        [](engine::Object3D& object, const json& value) {
            // 注視点の補正は先頭3要素を使い、足りない配列は受け付けない。
            if (!value.is_array() || value.size() < 3)
                return false;
            object.GetSceneCameraSettings().targetOffset = { value[0].get<float>(), value[1].get<float>(), value[2].get<float>() };
            return true;
        },
        cameraOnly,
    });
    Register({
        "camera.fixedTarget", "固定注視点", "Camera", EditorPropertyType::kVector3, CommonFlags(true),
        [](const engine::Object3D& object) {
            const engine::Vector3& value = object.GetSceneCameraSettings().fixedTarget;
            return json::array({ value.x, value.y, value.z });
        },
        [](engine::Object3D& object, const json& value) {
            // 固定注視点の XYZ を読み出せる配列か確認する。
            if (!value.is_array() || value.size() < 3)
                return false;
            object.GetSceneCameraSettings().fixedTarget = { value[0].get<float>(), value[1].get<float>(), value[2].get<float>() };
            return true;
        },
        cameraOnly,
    });
    Register({
        "camera.forwardDistance", "前方距離", "Camera", EditorPropertyType::kNumber, CommonFlags(true),
        [](const engine::Object3D& object) { return json(object.GetSceneCameraSettings().forwardDistance); },
        [](engine::Object3D& object, const json& value) {
            // 前方距離が数値の場合にだけ適用し、視点と注視点が重ならないよう下限を設ける。
            if (!value.is_number())
                return false;
            object.GetSceneCameraSettings().forwardDistance = (std::max)(value.get<float>(), 0.01f);
            return true;
        },
        cameraOnly,
    });
    Register({
        "camera.targetFollowMode", "Target追従方式", "Camera", EditorPropertyType::kInteger, CommonFlags(),
        [](const engine::Object3D& object) { return json(static_cast<std::int32_t>(object.GetSceneCameraSettings().targetFollowMode)); },
        [](engine::Object3D& object, const json& value) {
            // 注視点の追従方式は整数番号として受け付ける。
            if (!value.is_number_integer())
                return false;
            object.GetSceneCameraSettings().targetFollowMode =
                static_cast<SceneCameraFollowMode>(std::clamp(
                    value.get<std::int32_t>(), static_cast<std::int32_t>(SceneCameraFollowMode::kSnap), static_cast<std::int32_t>(SceneCameraFollowMode::kSmooth)));
            return true;
        },
        cameraOnly,
        { "完全追従", "Smooth" },
    });
    Register({
        "camera.targetFollowResponse", "Target追従レスポンス", "Camera", EditorPropertyType::kNumber, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetSceneCameraSettings().targetFollowResponse); },
        [](engine::Object3D& object, const json& value) {
            // 注視点のレスポンスが数値の場合、0.01未満を切り上げて適用する。
            if (!value.is_number())
                return false;
            object.GetSceneCameraSettings().targetFollowResponse = (std::max)(value.get<float>(), 0.01f);
            return true;
        },
        cameraOnly,
    });

    // 演出への出入り時間とEasingは、視点と注視点の計算結果を合成する際に使う。
    Register({
        "camera.blendIn", "Blend In", "Camera", EditorPropertyType::kNumber, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetSceneCameraSettings().blendInDuration); },
        [](engine::Object3D& object, const json& value) {
            // 開始時の補間時間に変換できる数値だけを受け付ける。
            if (!value.is_number())
                return false;
            // カメラへ切り替える補間時間を秒で保持する。0 秒は即時切替。
            object.GetSceneCameraSettings().blendInDuration = (std::max)(value.get<float>(), 0.0f);
            return true;
        },
        cameraOnly,
    });
    Register({
        "camera.blendOut", "Blend Out", "Camera", EditorPropertyType::kNumber, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetSceneCameraSettings().blendOutDuration); },
        [](engine::Object3D& object, const json& value) {
            // 終了時の補間時間は数値の場合だけ受け付け、負値は0秒にする。
            if (!value.is_number())
                return false;
            object.GetSceneCameraSettings().blendOutDuration = (std::max)(value.get<float>(), 0.0f);
            return true;
        },
        cameraOnly,
    });
    Register({
        "camera.easing", "切替Easing", "Camera", EditorPropertyType::kInteger, CommonFlags(),
        [](const engine::Object3D& object) { return json(static_cast<std::int32_t>(object.GetSceneCameraSettings().easing)); },
        [](engine::Object3D& object, const json& value) {
            // 切替曲線を整数で受け、有効な Easing の列挙範囲に収める。
            if (!value.is_number_integer())
                return false;
            object.GetSceneCameraSettings().easing = static_cast<SceneCameraEasing>(std::clamp(
                value.get<std::int32_t>(), static_cast<std::int32_t>(SceneCameraEasing::kLinear), static_cast<std::int32_t>(SceneCameraEasing::kSmootherStep)));
            return true;
        },
        cameraOnly,
        { "Linear", "Ease In", "Ease Out", "Ease In Out", "Smoother Step" },
    });

    // EntityParameterの数値・整数・真偽値・文字列は同じ検証と読み書き手順で登録する。
    // 対象がPlayer/Enemyではないことは冒頭の述語で確認する。
    auto registerParamNumber = [this, gameplayParameterOnly](
        const char* path,
        const char* displayName,
        float engine::Object3D::EntityParameter::* member) {
        Register({
            path, displayName, "Gameplay", EditorPropertyType::kNumber, CommonFlags(true),
            [member](const engine::Object3D& object) { return json(object.GetParameters().value().*member); },
            [member](engine::Object3D& object, const json& value) {
                // 浮動小数点に変換でき、対象にパラメーターがある場合だけ設定する。
                if (!value.is_number() || !object.GetParameters().has_value())
                    return false;
                object.SetParameter(member, value.get<float>());
                return true;
            },
            gameplayParameterOnly,
        });
    };

    auto registerParamInteger = [this, gameplayParameterOnly](
        const char* path,
        const char* displayName,
        std::int32_t engine::Object3D::EntityParameter::* member) {
        Register({
            path, displayName, "Gameplay", EditorPropertyType::kInteger, CommonFlags(),
            [member](const engine::Object3D& object) { return json(object.GetParameters().value().*member); },
            [member](engine::Object3D& object, const json& value) {
                // 整数項目では数値型のうち整数を要求し、対象にパラメーターが無ければ拒否する。
                if (!value.is_number_integer() || !object.GetParameters().has_value())
                    return false;
                object.SetParameter(member, value.get<std::int32_t>());
                return true;
            },
            gameplayParameterOnly,
        });
    };

    auto registerParamBool = [this, gameplayParameterOnly](
        const char* path,
        const char* displayName,
        bool engine::Object3D::EntityParameter::* member) {
        Register({
            path, displayName, "Gameplay", EditorPropertyType::kBool, CommonFlags(),
            [member](const engine::Object3D& object) { return json(object.GetParameters().value().*member); },
            [member](engine::Object3D& object, const json& value) {
                // 有効・無効の項目には真偽値を要求し、対象に設定先がある場合だけ書き込む。
                if (!value.is_boolean() || !object.GetParameters().has_value())
                    return false;
                object.SetParameter(member, value.get<bool>());
                return true;
            },
            gameplayParameterOnly,
        });
    };

    auto registerParamString = [this, gameplayParameterOnly](
        const char* path,
        const char* displayName,
        std::string engine::Object3D::EntityParameter::* member) {
        Register({
            path, displayName, "Gameplay", EditorPropertyType::kString, CommonFlags(),
            [member](const engine::Object3D& object) { return json(object.GetParameters().value().*member); },
            [member](engine::Object3D& object, const json& value) {
                // シーン名などの文字列は、対象にパラメーターがあり値も文字列の場合に適用する。
                if (!value.is_string() || !object.GetParameters().has_value())
                    return false;
                object.SetParameter(member, value.get<std::string>());
                return true;
            },
            gameplayParameterOnly,
        });
    };

    // パスはEditorから参照する安定ID。表示名を変えても保存項目との対応は維持する。
    registerParamNumber("gameplay.healAmount", "回復量", &engine::EntityParameter::healAmount);
    registerParamNumber("gameplay.interval", "間隔", &engine::EntityParameter::interval);
    registerParamInteger("gameplay.maxCount", "最大数", &engine::EntityParameter::maxCount);
    registerParamNumber("gameplay.detectionRange", "感知範囲", &engine::EntityParameter::detectionRange);
    registerParamNumber("gameplay.shakeDuration", "揺れ時間", &engine::EntityParameter::shakeDuration);
    registerParamNumber("gameplay.fallDuration", "落下時間", &engine::EntityParameter::fallDuration);
    registerParamInteger("gameplay.colorType", "色Type", &engine::EntityParameter::colorType);
    registerParamInteger("gameplay.switchMode", "Switch Mode", &engine::EntityParameter::switchMode);
    registerParamInteger("gameplay.actionMode", "Action Mode", &engine::EntityParameter::actionMode);
    registerParamString("gameplay.targetScene", "移動先Scene", &engine::EntityParameter::targetScene);
    registerParamNumber("gameplay.moveAmount", "移動量", &engine::EntityParameter::moveAmount);
    registerParamNumber("gameplay.moveSpeed", "移動速度", &engine::EntityParameter::moveSpeed);
    registerParamNumber("gameplay.ringWaveSpeed", "リング拡大速度", &engine::EntityParameter::ringWaveSpeed);
    registerParamBool("gameplay.startActive", "開始時有効", &engine::EntityParameter::startActive);
    registerParamBool("gameplay.returnOnOff", "OFFで戻る", &engine::EntityParameter::returnOnOff);
}
