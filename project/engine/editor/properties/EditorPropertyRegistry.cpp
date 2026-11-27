#include <cstdint>
#include "EditorPropertyRegistry.h"

#include "Object3D.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

using json = nlohmann::json;

// 複数選択の混在判定用にJSONを比較する。数値は配列の中も含めて微小な誤差を許容する。
bool JsonValuesEqual(const json& lhs, const json& rhs) {
    // 複数選択の混在判定では、編集時の浮動小数点誤差を差分として扱わないようにします。
    if (lhs.is_number() && rhs.is_number()) {
        return std::fabs(lhs.get<double>() - rhs.get<double>()) <= 0.00001;
    }

    // 配列が二つ揃う場合は、要素ごとに同じ許容誤差を使って比較する。
    if (lhs.is_array() && rhs.is_array()) {
        // 要素数が違えば位置ごとの値を比較する前に不一致とする。
        if (lhs.size() != rhs.size()) {
            return false;
        }

        for (std::size_t index = 0; index < lhs.size(); ++index) {
            // いずれかの要素が一致しなければ、残りを比較せず不一致を返す。
            if (!JsonValuesEqual(lhs[index], rhs[index])) {
                return false;
            }
        }

        return true;
    }

    return lhs == rhs;
}

bool StartsWith(const std::string& text, const char* prefix) {
    return text.rfind(prefix, 0) == 0;
}

const json* FindSerializedPath(const json& source, const std::string& path) {
    // Editor の Property Path ではなく、保存 JSON 内のドット区切り Path を順にたどります。
    const json* current = &source;
    std::size_t begin = 0;

    while (begin < path.size()) {
        const std::size_t separator = path.find('.', begin);
        const std::string key = path.substr(
            begin,
            separator == std::string::npos ? std::string::npos : separator - begin);
        // 途中のノードがオブジェクトでないかキーが欠ける場合、指定先は存在しない。
        if (!current->is_object() || !current->contains(key)) {
            return nullptr;
        }

        current = &current->at(key);

        // 最後のキーまで辿れた場合は参照を返す。
        if (separator == std::string::npos) {
            return current;
        }

        begin = separator + 1;
    }

    return current;
}

// 保存JSONに明示されたComponent存在フラグを読み、未指定ならfalseを返します。
bool TryReadSerializedPresenceMarker(
    const json& source,
    const std::string& componentTypeId,
    bool& present) {
    const json* marker = FindSerializedPath(
        source,
        "components." + componentTypeId + "._editorPresent");
    // 既存形式の推定へ進めるよう、フラグが欠けるか真偽値でなければ読取失敗とする。
    if (!marker || !marker->is_boolean()) {
        return false;
    }

    present = marker->get<bool>();
    return true;
}

// 旧形式の指定保存Pathに空でない文字列があるか調べます。
bool HasNonEmptySerializedString(const json& source, const char* path) {
    const json* value = FindSerializedPath(source, path);
    return value && value->is_string() && !value->get_ref<const std::string&>().empty();
}

// 旧形式の指定保存Pathに0以外の整数があるか調べます。
bool HasNonZeroSerializedInteger(const json& source, const char* path) {
    const json* value = FindSerializedPath(source, path);

    // 旧形式の整数フラグとして解釈できない値は存在の根拠にしない。
    if (!value || (!value->is_number_integer() && !value->is_number_unsigned())) {
        return false;
    }

    return value->get<std::int64_t>() != 0;
}

bool ResolveSerializedComponentPresence(const json& source, const std::string& componentTypeId) {
    // 明示された存在フラグを優先し、フラグのない旧JSONだけ従来フィールドから推定する。
    bool markerPresent = false;

    // 明示値がある場合は、無効指定もそのまま確定する。
    if (TryReadSerializedPresenceMarker(source, componentTypeId, markerPresent)) {
        return markerPresent;
    }

    // 旧形式の各フィールドが空かどうかを種類ごとに判定します。
    // Colliderの旧形式では形状、属性、マスクのいずれかの非0値を存在の根拠にする。
    if (componentTypeId == "Collider") {
        return HasNonZeroSerializedInteger(source, "collider.type") ||
            HasNonZeroSerializedInteger(source, "collisionAttribute") ||
            HasNonZeroSerializedInteger(source, "collisionMask");
    }

    // ParticleEmitterの旧形式ではCPUまたはGPUのエミッター名から判定する。
    if (componentTypeId == "ParticleEmitter") {
        return HasNonEmptySerializedString(source, "particleName") ||
            HasNonEmptySerializedString(source, "gpuParticleName");
    }

    // MeshEffectの旧形式では主・副いずれかのエフェクト名がある場合に存在する。
    if (componentTypeId == "MeshEffect") {
        return HasNonEmptySerializedString(source, "meshEffect1") ||
            HasNonEmptySerializedString(source, "meshEffect2");
    }

    // Animatorの旧形式では直接再生するアニメーションかControllerの指定があれば存在する。
    if (componentTypeId == "Animator") {
        return HasNonEmptySerializedString(source, "animation.animName") ||
            HasNonEmptySerializedString(source, "animation.animatorController");
    }

    // PathMoverの旧形式では再生する記録パス名があれば存在する。
    if (componentTypeId == "PathMover") {
        return HasNonEmptySerializedString(source, "recorder.recordPathName");
    }

    // GameplayLinkはイベントIDか対象IDの片方が0以上なら使用中とみなす。
    if (componentTypeId == "GameplayLink") {
        const json* eventId = FindSerializedPath(source, "myEventID");
        const json* targetId = FindSerializedPath(source, "targetID");
        const bool hasEvent = eventId && eventId->is_number_integer() && eventId->get<std::int32_t>() >= 0;
        const bool hasTarget = targetId && targetId->is_number_integer() && targetId->get<std::int32_t>() >= 0;
        return hasEvent || hasTarget;
    }

    return false;
}

// Componentの存在マーカーを書き、削除時は旧形式の実行時フィールドも消します。
void SetSerializedComponentPresence(
    json& source,
    const std::string& componentTypeId,
    bool present) {
    // Variant の削除印だけでは基底 Prefab の値が残るため、旧形式のフィールドも初期化します。
    source["components"][componentTypeId]["_editorPresent"] = present;

    // 追加時は存在フラグを付ければよいため、既存の設定値を消さずに戻る。
    if (present) {
        return;
    }

    // Component ごとに実行時に参照される値を解除します。
    // Colliderを外すときは形状と判定フィルター、固定フラグを解除する。
    if (componentTypeId == "Collider") {
        source["collider"]["type"] = static_cast<std::int32_t>(engine::ColliderType::kNone);
        source["collisionAttribute"] = 0;
        source["collisionMask"] = 0;
        source["isStatic"] = false;
    }
    // ParticleEmitterを外すときは現行と旧形式のCPU・GPU参照を両方空にする。
    else if (componentTypeId == "ParticleEmitter") {
        source["components"][componentTypeId]["cpuParticle"] = "";
        source["components"][componentTypeId]["gpuParticle"] = "";
        source["particleName"] = "";
        source["gpuParticleName"] = "";
    }
    // MeshEffectを外すときは二つのスロットを現行と旧形式の両方で消す。
    else if (componentTypeId == "MeshEffect") {
        source["components"][componentTypeId]["primary"] = "";
        source["components"][componentTypeId]["secondary"] = "";
        source["meshEffect1"] = "";
        source["meshEffect2"] = "";
    }
    // Animatorを外すときはクリップとControllerを空にし、ループも解除する。
    else if (componentTypeId == "Animator") {
        source["animation"]["animName"] = "";
        source["animation"]["isAnimLoop"] = false;
        source["animation"]["animatorController"] = "";
    }
    // PathMoverを外すときは再生元と繰返し・相対再生・開始条件を既定に戻す。
    else if (componentTypeId == "PathMover") {
        source["components"][componentTypeId]["path"] = "";
        source["components"][componentTypeId]["loop"] = false;
        source["components"][componentTypeId]["relative"] = false;
        source["components"][componentTypeId]["playbackMode"] = "always";
        source["recorder"]["recordPathName"] = "";
        source["recorder"]["isRecordLoop"] = false;
        source["recorder"]["isRecordRelative"] = false;
    }
    // GameplayLinkを外すときは送信元と相手のIDを未指定の-1へ戻す。
    else if (componentTypeId == "GameplayLink") {
        source["components"][componentTypeId]["eventId"] = -1;
        source["components"][componentTypeId]["targetId"] = -1;
        source["myEventID"] = -1;
        source["targetID"] = -1;
    }
}

std::string ResolveComponentTypeId(const std::string& path) {
    // UI 上の Property Path の接頭辞を、Inspector の Component 区分へ変換します。
    // 識別や配置分類に使う編集項目は、シーンオブジェクト自体の区分へ送る。
    if (StartsWith(path, "identity."))
        return "SceneObject";
    // 配置姿勢に属する項目はTransformへ送る。
    if (StartsWith(path, "transform."))
        return "Transform";
    // 表示や材質の項目はMeshRendererへ送る。
    if (StartsWith(path, "rendering."))
        return "MeshRenderer";
    // 編集時だけ使う状態はEditorStateで扱う。
    if (StartsWith(path, "editor."))
        return "EditorState";
    // 旧接頭辞も含む衝突項目をColliderの区分に集める。
    if (StartsWith(path, "collision.") || StartsWith(path, "component.collision."))
        return "Collider";
    // 粒子設定はParticleEmitterへ送る。
    if (StartsWith(path, "component.particle."))
        return "ParticleEmitter";
    // メッシュ演出の項目はMeshEffectへ送る。
    if (StartsWith(path, "component.meshEffect."))
        return "MeshEffect";
    // 再生設定はAnimatorへ送る。
    if (StartsWith(path, "component.animation."))
        return "Animator";
    // 記録パスの再生設定はPathMoverへ送る。
    if (StartsWith(path, "component.path."))
        return "PathMover";
    // イベントの接続情報はGameplayLinkへ送る。
    if (StartsWith(path, "component.link."))
        return "GameplayLink";
    // 経路探索の設定はNavAgentへ送る。
    if (StartsWith(path, "component.nav."))
        return "NavAgent";
    // カメラ固有の調整項目はCameraへ送る。
    if (StartsWith(path, "camera."))
        return "Camera";
    // ゲーム固有の実行値はGameplayへ送る。
    if (StartsWith(path, "gameplay."))
        return "Gameplay";
    return "SceneObject";
}

std::string ResolveSerializedPath(const std::string& path) {
    // 編集用 Path と保存 JSON のキー名は一致しないため、互換フィールドへ写像します。
    // transform/描画と、旧形式の component フィールドを同じ窓口から扱います。
    // シーン上の表示名はルートのnameを使う。
    if (path == "identity.name")
        return "name";
    // 永続識別子はルートのguidを使う。
    if (path == "identity.guid")
        return "guid";
    // 検索・判定用のタグはルートのtagを使う。
    if (path == "identity.tag")
        return "tag";
    // レイヤーはルートのlayerを使う。
    if (path == "identity.layer")
        return "layer";
    // 分割保存の分類はルートのsaveCategoryを使う。
    if (path == "identity.saveCategory")
        return "saveCategory";
    // 座標は旧形式と同じtranslateで保存する。
    if (path == "transform.position")
        return "translate";
    // 回転は旧形式のrotateを更新する。
    if (path == "transform.rotation")
        return "rotate";
    // 各軸の倍率はルートのscaleを使う。
    if (path == "transform.scale")
        return "scale";
    // 表示の有無を旧形式のisVisibleへ対応付ける。
    if (path == "rendering.visible")
        return "isVisible";
    // 影を落とす設定はルートのcastShadowを使う。
    if (path == "rendering.castShadow")
        return "castShadow";
    // カメラの遮蔽物処理はルートのcameraOcclusionModeを使う。
    if (path == "rendering.cameraOcclusionMode")
        return "cameraOcclusionMode";
    // 材質色はルートのcolorを使う。
    if (path == "rendering.color")
        return "color";
    // 金属度は材質のmetallicへ対応付ける。
    if (path == "rendering.metallic")
        return "metallic";
    // 表面の粗さは材質のroughnessへ対応付ける。
    if (path == "rendering.roughness")
        return "roughness";
    // 自己発光は材質のemissiveへ対応付ける。
    if (path == "rendering.emissive")
        return "emissive";
    // モデルの参照は登録名を保持するmodelNameへ対応付ける。
    if (path == "rendering.model")
        return "modelName";
    // 合成方式はルートのblendModeを使う。
    if (path == "rendering.blendMode")
        return "blendMode";
    // 特殊描画の材質番号はルートのmaterialTypeを使う。
    if (path == "rendering.materialType")
        return "materialType";
    // ベース画像は参照先のtexturePathへ対応付ける。
    if (path == "rendering.texture")
        return "texturePath";
    // UVの繰返しはtextureTilingを使う。
    if (path == "rendering.textureTiling")
        return "textureTiling";
    // 寸法からUVを計算する設定はautoTextureTilingを使う。
    if (path == "rendering.autoTextureTiling")
        return "autoTextureTiling";
    // 照明の有無はenableLightingへ対応付ける。
    if (path == "rendering.lighting")
        return "enableLighting";
    // 環境反射の有無はenableEnvMapへ対応付ける。
    if (path == "rendering.environmentMap")
        return "enableEnvMap";
    // 環境反射の強さはenvIntensityへ対応付ける。
    if (path == "rendering.environmentIntensity")
        return "envIntensity";
    // 法線画像を使うかはenableNormalMapで保存する。
    if (path == "rendering.normalMapEnabled")
        return "enableNormalMap";
    // 法線画像の参照先はnormalMapPathで保存する。
    if (path == "rendering.normalMap")
        return "normalMapPath";
    // ORM画像の参照先はormMapPathで保存する。
    if (path == "rendering.ormMap")
        return "ormMapPath";
    // 編集ロックは保存上のisLockedへ対応付ける。
    if (path == "editor.locked")
        return "isLocked";
    // 衝突対象としての属性ビットをcollisionAttributeへ対応付ける。
    if (path == "collision.attribute")
        return "collisionAttribute";
    // 受け付ける衝突相手のビットをcollisionMaskへ対応付ける。
    if (path == "collision.mask")
        return "collisionMask";
    // 判定形状の種類はcollider内部のtypeへ書く。
    if (path == "component.collision.type")
        return "collider.type";
    // 判定形状の基準位置はcollider内部のcenterへ書く。
    if (path == "component.collision.center")
        return "collider.center";
    // 判定形状の大きさはcollider内部のsizeへ書く。
    if (path == "component.collision.size")
        return "collider.size";
    // 判定形状の回転はcollider内部のrotationへ書く。
    if (path == "component.collision.rotation")
        return "collider.rotation";
    // 固定フラグはcolliderの外にあるisStaticへ書く。
    if (path == "component.collision.static")
        return "isStatic";
    // CPU粒子は旧形式のparticleNameを使う。
    if (path == "component.particle.cpu")
        return "particleName";
    // GPU粒子は旧形式のgpuParticleNameを使う。
    if (path == "component.particle.gpu")
        return "gpuParticleName";
    // 主エフェクトは従来の一つ目のスロットへ対応付ける。
    if (path == "component.meshEffect.primary")
        return "meshEffect1";
    // 副エフェクトは従来の二つ目のスロットへ対応付ける。
    if (path == "component.meshEffect.secondary")
        return "meshEffect2";
    // 直接再生するクリップはanimation内部のanimNameを使う。
    if (path == "component.animation.name")
        return "animation.animName";
    // アニメーションの連続再生はanimation内部のisAnimLoopを使う。
    if (path == "component.animation.loop")
        return "animation.isAnimLoop";
    // 状態遷移を持つ定義はanimation内部のanimatorControllerを使う。
    if (path == "component.animation.controller")
        return "animation.animatorController";
    // 移動に使う記録はrecorder内部のrecordPathNameを使う。
    if (path == "component.path.name")
        return "recorder.recordPathName";
    // 記録の繰返し再生はrecorder内部のisRecordLoopを使う。
    if (path == "component.path.loop")
        return "recorder.isRecordLoop";
    // パスの開始条件はPathMoverの現行プロパティへ書く。
    if (path == "component.path.playbackMode")
        return "components.PathMover.playbackMode";
    // 基準位置からの相対再生はrecorder内部のisRecordRelativeを使う。
    if (path == "component.path.relative")
        return "recorder.isRecordRelative";
    // 発行するイベントのIDは旧形式のmyEventIDへ対応付ける。
    if (path == "component.link.eventId")
        return "myEventID";
    // 接続相手のIDは旧形式のtargetIDへ対応付ける。
    if (path == "component.link.targetId")
        return "targetID";

    // NavAgent、ゲームパラメーター、カメラは既知の例外を除き、末尾の項目名を保持する。
    if (StartsWith(path, "component.nav.")) {
        const std::string property = path.substr(std::string("component.nav.").size());
        return "components.NavAgent." + property;
    }

    // ゲーム固有の値は項目名を保ち、実行パラメーターの区分へ移す。
    if (StartsWith(path, "gameplay."))
        return "param." + path.substr(9);
    // カメラ項目では先に、編集上の短い名前と保存名が違う項目を個別に変換する。
    if (StartsWith(path, "camera.")) {
        const std::string property = path.substr(7);
        // 視野角は水平と区別できるよう、保存上の縦FOVの項目を使う。
        if (property == "fov")
            return "camera.fovY";
        // 視点側のオブジェクト参照は名前として保存する項目へ送る。
        if (property == "eyeObject")
            return "camera.eyeObjectName";
        // 注視対象のオブジェクト参照は名前として保存する項目へ送る。
        if (property == "targetObject")
            return "camera.targetObjectName";
        // カメラが切り替わる際の入りの時間はblendInDurationへ送る。
        if (property == "blendIn")
            return "camera.blendInDuration";
        // カメラから離れる際の戻り時間はblendOutDurationへ送る。
        if (property == "blendOut")
            return "camera.blendOutDuration";
        return "camera." + property;
    }

    return {};
}

json ResolveDefaultValue(const std::string& path) {
    // パス再生条件が欠ける旧データは常時再生に補う。
    if (path == "component.path.playbackMode") return "always";
    // 固定・ループ・相対再生の未指定時は無効値を使う。
    if (path == "component.collision.static" ||
        path == "component.path.loop" ||
        path == "component.path.relative") {
        return false;
    }

    // パスや演出名の未指定時は空の文字列を使う。
    if (path == "component.particle.cpu" || path == "component.particle.gpu" ||
        path == "component.meshEffect.primary" || path == "component.meshEffect.secondary" ||
        path == "component.animation.name" || path == "component.animation.controller" ||
        path == "component.path.name") {
        return "";
    }

    return json();
}

// 編集欄の入力刻み・表示単位・範囲を、基本の型とプロパティ個別の規則から決める。
EditorPropertyUiHints ResolveUiHints(const std::string& path, EditorPropertyType type) {
    EditorPropertyUiHints hints;
    hints.configured = true;

    switch (type) {
    // スカラー数値には共通のドラッグ刻みと小数表示を使う。
    case EditorPropertyType::kNumber:
        hints.speed = 0.05f;
        hints.format = "%.3f";
        break;

    // ベクトルの各成分にも同じドラッグ刻みと小数表示を使う。
    case EditorPropertyType::kVector2:
    case EditorPropertyType::kVector3:
    case EditorPropertyType::kVector4:
        hints.speed = 0.05f;
        hints.format = "%.3f";
        break;

    // 数値以外の型は既定値を保持し、必要なら下の個別規則で変更する。
    default:
        break;
    }

    // 角度と FOV は表示単位が違うため、通常の数値入力より専用ヒントを優先します。
    if (path == "transform.rotation" || path == "component.collision.rotation") {
        hints.displayAsDegrees = true;
        hints.speed = 0.25f;
    }
    // カメラのFOVだけは角度の範囲を指定したスライダーにする。
    else if (path == "camera.fov") {
        hints.useSlider = true;
        hints.displayAsDegrees = true;
        hints.minValue = 3.0f;
        hints.maxValue = 170.0f;
        hints.format = "%.1f deg";
    }
    // 金属度と粗さは0から1の範囲で調整する。
    else if (path == "rendering.metallic" || path == "rendering.roughness") {
        hints.useSlider = true;
        hints.minValue = 0.0f;
        hints.maxValue = 1.0f;
    }
    // 自己発光は光量を調整するため、金属度などとは別の刻みと範囲を使う。
    else if (path == "rendering.emissive") {
        hints.speed = 0.02f;
        hints.minValue = 0.0f;
        hints.maxValue = 100.0f;
    }
    // 描画色の入力は成分ごとの数値欄ではなくカラーピッカーで扱う。
    else if (path == "rendering.color") {
        hints.useColorPicker = true;
    }

    return hints;
}

}

EditorPropertyRegistry* EditorPropertyRegistry::GetInstance() {
    static EditorPropertyRegistry sInstance;
    return &sInstance;
}

// コンポーネントとプリセットの定義を検証し、旧形式との互換処理を補って登録する。
bool EditorPropertyRegistry::RegisterComponent(EditorComponentDescriptor descriptor) {
    // 型ID・表示名を欠くか、IDがすでに登録されている場合は登録しない。
    if (descriptor.typeId.empty() || descriptor.displayName.empty() ||
        componentIndices_.find(descriptor.typeId) != componentIndices_.end()) {
        return false;
    }

    // Component と Preset の識別子重複を防いでから、Editor の一覧へ追加します。
    std::vector<std::string> presetIds;
    presetIds.reserve(descriptor.presets.size());

    for (const EditorComponentPresetDescriptor& preset : descriptor.presets) {
        // プリセットを識別、表示、実行できないか、同じIDが重複する場合は定義全体を拒否する。
        if (preset.id.empty() || preset.displayName.empty() || !preset.apply ||
            std::find(presetIds.begin(), presetIds.end(), preset.id) != presetIds.end()) {
            return false;
        }

        presetIds.push_back(preset.id);
    }

    // 削除可能Componentが旧JSONにも対応できるよう、保存状態の判定・更新を補う。
    if (descriptor.removable && !descriptor.serializedPresent) {
        const std::string typeId = descriptor.typeId;
        descriptor.serializedPresent = [typeId](const json& source) {
            return ResolveSerializedComponentPresence(source, typeId);
        };
    }

    // 削除可能で保存状態の書き込みを指定していない種類に、旧形式も更新する共通処理を割り当てる。
    if (descriptor.removable && !descriptor.setSerializedPresent) {
        const std::string typeId = descriptor.typeId;
        descriptor.setSerializedPresent = [typeId](json& source, bool present) {
            SetSerializedComponentPresence(source, typeId, present);
        };
    }

    componentIndices_[descriptor.typeId] = components_.size();
    components_.push_back(std::move(descriptor));
    return true;
}

// 編集用の定義と、Inspectorや保存処理が使う補足情報をまとめて登録する。
bool EditorPropertyRegistry::Register(EditorPropertyDescriptor descriptor) {
    // 一意のパスと読み書きの処理が揃わない項目は登録しない。
    if (descriptor.path.empty() || !descriptor.getter || !descriptor.setter ||
        propertyIndices_.find(descriptor.path) != propertyIndices_.end()) {
        return false;
    }

    // 定義ごとの重複設定を減らしつつ、明示指定があればそれを優先する。
    // Inspectorでの所属先が未指定なら、編集パスの接頭辞から割り当てる。
    if (descriptor.componentTypeId.empty()) {
        descriptor.componentTypeId = ResolveComponentTypeId(descriptor.path);
    }

    // 保存パスに明示指定がなければ、旧形式も含む既定の対応を使う。
    if (descriptor.serializedPath.empty()) {
        descriptor.serializedPath = ResolveSerializedPath(descriptor.path);
    }

    // 既定値がnullなら、種類ごとの互換値を補えるか確認する。
    if (descriptor.defaultValue.is_null()) {
        descriptor.defaultValue = ResolveDefaultValue(descriptor.path);
    }

    // 入力表示の指定が確認できない場合は、型と項目名から標準設定を用意する。
    if (!descriptor.ui.configured) {
        descriptor.ui = ResolveUiHints(descriptor.path, descriptor.type);
    }

    // 所属先があることを確認してから登録する。Inspector が所属 Component ごとに項目を取り出せるようにする。
    if (!FindComponent(descriptor.componentTypeId)) {
        return false;
    }

    propertyIndices_[descriptor.path] = properties_.size();
    properties_.push_back(std::move(descriptor));
    return true;
}

// 型IDを引き、未登録ならnullptrを返す。
const EditorComponentDescriptor* EditorPropertyRegistry::FindComponent(const std::string& typeId) const {
    const auto it = componentIndices_.find(typeId);
    return it == componentIndices_.end() ? nullptr : &components_[it->second];
}

// 指定した種類で使えるプリセットを検索する。登録がなければnullptrを返す。
const EditorComponentPresetDescriptor* EditorPropertyRegistry::FindComponentPreset(
    const std::string& typeId,
    const std::string& presetId) const {
    const EditorComponentDescriptor* component = FindComponent(typeId);

    // 種類自体が未登録ならプリセット一覧を参照できない。
    if (!component) {
        return nullptr;
    }

    const auto it = std::find_if(
        component->presets.begin(),
        component->presets.end(),
        [&presetId](const EditorComponentPresetDescriptor& preset) {
            return preset.id == presetId;
        });
    return it == component->presets.end() ? nullptr : &*it;
}

// 対象に適用できる種類の一覧を、追加候補も含めて表示順で返す。
std::vector<const EditorComponentDescriptor*> EditorPropertyRegistry::GetApplicableComponentsForObject(
    const engine::Object3D* object) const {
    std::vector<const EditorComponentDescriptor*> result;

    // 実体がない場合は適用可否を評価できないので空の一覧を返す。
    if (!object) {
        return result;
    }

    for (const EditorComponentDescriptor& component : components_) {
        // 適用条件を持たない種類と、個別条件を満たす種類を候補にする。
        if (!component.applicable || component.applicable(*object)) {
            result.push_back(&component);
        }
    }

    // 表示順が同じ Component は登録順を維持し、Inspector 内の並びを安定させる。
    std::stable_sort(result.begin(), result.end(), [](const auto* left, const auto* right) {
        return left->displayOrder < right->displayOrder;
    });
    return result;
}

std::vector<const EditorComponentDescriptor*> EditorPropertyRegistry::GetComponentsForObject(
    const engine::Object3D* object) const {
    std::vector<const EditorComponentDescriptor*> result;

    // 追加候補にもなり得る適用可能一覧から、対象 Object に実際に存在する Component だけを抜き出す。
    for (const EditorComponentDescriptor* component : GetApplicableComponentsForObject(object)) {
        // 存在判定が登録されていない種類は常設扱いとし、それ以外は現在の実体で確認する。
        if (component && (!component->present || component->present(*object))) {
            result.push_back(component);
        }
    }

    return result;
}

std::vector<const EditorPropertyDescriptor*> EditorPropertyRegistry::GetPropertiesForComponent(
    const std::string& typeId) const {
    // 登録順に抽出する。Inspector の入力欄はこの順を使い、取得結果は台帳内の定義を借用する。
    std::vector<const EditorPropertyDescriptor*> result;

    for (const EditorPropertyDescriptor& property : properties_) {
        // 指定した区分に所属する項目だけ、定義の参照を返す一覧へ加える。
        if (property.componentTypeId == typeId)
            result.push_back(&property);
    }

    return result;
}

// 登録済みの種類をこの実体へ適用できるか、あれば個別条件も使って判定する。
bool EditorPropertyRegistry::IsComponentApplicable(
    const engine::Object3D* object,
    const std::string& typeId) const {
    const EditorComponentDescriptor* component = FindComponent(typeId);
    return object && component && (!component->applicable || component->applicable(*object));
}

bool EditorPropertyRegistry::IsComponentPresent(
    const engine::Object3D* object,
    const std::string& typeId) const {
    const EditorComponentDescriptor* component = FindComponent(typeId);

    // 未登録、実体なし、適用対象外のいずれかなら存在確認を進めない。
    if (!IsComponentApplicable(object, typeId) || !component) {
        return false;
    }

    return !component->present || component->present(*object);
}

// 保存JSONから存在を判定する。判定処理のない種類はfalseを返す。
bool EditorPropertyRegistry::IsComponentPresent(
    const json& serializedObject,
    const std::string& typeId) const {
    const EditorComponentDescriptor* component = FindComponent(typeId);
    return component && component->serializedPresent && component->serializedPresent(serializedObject);
}

bool EditorPropertyRegistry::SetComponentPresent(
    json& serializedObject,
    const std::string& typeId,
    bool present) const {
    const EditorComponentDescriptor* component = FindComponent(typeId);

    // 保存側の更新処理が登録されていない種類では、JSONを変更せず失敗を返す。
    if (!component || !component->setSerializedPresent) {
        return false;
    }

    // 編集データの存在マーカーと、その形式が必要とする関連フィールドを更新する。既に生成済みの実体を変更する処理は AddComponent または RemoveComponent が担当する。
    component->setSerializedPresent(serializedObject, present);
    return true;
}

bool EditorPropertyRegistry::AddComponent(engine::Object3D* object, const std::string& typeId) const {
    // 対象に適用でき、まだ存在しない Component だけ追加する。
    const EditorComponentDescriptor* component = FindComponent(typeId);
    // 対象と追加処理が有効で、種類が適用可能かつ現在は存在しない場合にだけ追加する。
    if (!object || !component || !component->add ||
        !IsComponentApplicable(object, typeId) || IsComponentPresent(object, typeId)) {
        return false;
    }

    return component->add(*object);
}

// 対象に適用できる登録済みプリセットを実行し、プリセットの処理結果を返す。
bool EditorPropertyRegistry::ApplyComponentPreset(
    engine::Object3D* object,
    const std::string& typeId,
    const std::string& presetId) const {
    const EditorComponentPresetDescriptor* preset = FindComponentPreset(typeId, presetId);

    // 実体、プリセット、適用条件のすべてを確認できる場合にだけ設定を適用する。
    if (!object || !preset || !IsComponentApplicable(object, typeId)) {
        return false;
    }

    return preset->apply(*object);
}

bool EditorPropertyRegistry::RemoveComponent(engine::Object3D* object, const std::string& typeId) const {
    // 削除許可と存在確認を通った Component だけ実体から外す。
    const EditorComponentDescriptor* component = FindComponent(typeId);
    // 実体・削除許可・削除処理・現在の存在確認がすべて揃う場合にだけ外す。
    if (!object || !component || !component->removable || !component->remove ||
        !IsComponentPresent(object, typeId)) {
        return false;
    }

    return component->remove(*object);
}

bool EditorPropertyRegistry::IsApplicable(const engine::Object3D* object, const std::string& path) const {
    // 個別の適用条件が登録されていなければ、存在するプロパティは全対象で有効にする。
    const EditorPropertyDescriptor* property = Find(path);

    // 実体がないかパスが未登録なら、対象に使用できない。
    if (!object || !property) {
        return false;
    }

    return !property->applicable || property->applicable(*object);
}

const EditorPropertyDescriptor* EditorPropertyRegistry::Find(const std::string& path) const {
    auto it = propertyIndices_.find(path);

    // 登録されていない編集パスでは、未定義を示すnullptrを返す。
    if (it == propertyIndices_.end()) {
        return nullptr;
    }

    return &properties_[it->second];
}

json EditorPropertyRegistry::GetValue(const engine::Object3D* object, const std::string& path) const {
    // 不適用または読取関数のない項目は空 JSON として UI に返す。
    const EditorPropertyDescriptor* property = Find(path);

    // 実体、定義、読取処理がないか適用対象外なら値を取り出さない。
    if (!object || !property || !property->getter || !IsApplicable(object, path)) {
        return json();
    }

    return property->getter(*object);
}

bool EditorPropertyRegistry::SetValue(engine::Object3D* object, const std::string& path, const json& value) const {
    // Inspector以外からの呼び出しでもReadOnlyと適用条件を守る。
    const EditorPropertyDescriptor* property = Find(path);
    // 書込可能な対象と定義が揃い、適用条件を満たす読み取り専用以外の項目だけ変更する。
    if (!object || !property || !property->setter || !IsApplicable(object, path) ||
        HasEditorPropertyFlag(property->flags, EditorPropertyFlags::kReadOnly)) {
        return false;
    }

    return property->setter(*object, value);
}

bool EditorPropertyRegistry::HasMixedValue(const std::vector<engine::Object3D*>& objects, const std::string& path) const {
    const EditorPropertyDescriptor* property = Find(path);

    // 比較可能な定義と読取処理、複数の選択対象が揃わなければ混在を検出できない。
    if (!property || !property->getter || objects.size() < 2) {
        return false;
    }

    // 適用できない Object は比較から除き、最初の適用可能値を基準にします。
    const engine::Object3D* first = nullptr;

    for (engine::Object3D* object : objects) {
        // 最初の実在する適用可能な対象を比較基準として選ぶ。
        if (object && IsApplicable(object, path)) {
            first = object;
            break;
        }
    }

    // 比較基準にできる実体が一つもなければ混在なしとする。
    if (!first) {
        return false;
    }

    // 混合表示は画面に見せる値で比較し、Prefabなどへ渡す正本はgetterのまま保ちます。
    const auto displayed = [&](const engine::Object3D& object) {
        return property->displayValue ? property->displayValue(object) : property->getter(object);
    };
    const json reference = displayed(*first);

    for (engine::Object3D* object : objects) {
        // 適用できる対象で表示値が基準と異なるものを一つでも見つけたら混在とする。
        if (object && IsApplicable(object, path) && !JsonValuesEqual(reference, displayed(*object))) {
            return true;
        }
    }

    return false;
}

void EditorPropertyRegistry::InitializeBuiltInProperties() {
    // 同じプロセスで組み込み定義が初期化済みなら、再登録しない。
    if (initialized_) {
        return;
    }

    initialized_ = true;

    // Property 登録時に所属 Component を照合するため、Component を先に登録します。
    RegisterBuiltInComponents();
    RegisterBuiltInBaseProperties();
    RegisterBuiltInRenderAndComponentProperties();
    RegisterBuiltInCameraAndGameplayProperties();
}

void EditorPropertyRegistry::WriteClassProperties(const engine::Object3D& object, json& components) const {
    // UIで隠れている設定も保存します。未知のキーは上書きせず、そのまま引き継ぎます。
    for (const auto& property : properties_) {
        // クラス保存用と指定され、対象に適用できる項目だけを書き出す。
        if (!HasEditorPropertyFlag(property.flags, EditorPropertyFlags::kClassStorage) ||
            (property.applicable && !property.applicable(object))) continue;
        json& payload = components[property.componentTypeId];
        // 保存区分がオブジェクトになっていない場合は、子のキーを書ける空のオブジェクトにする。
        if (!payload.is_object()) payload = json::object();
        payload["version"] = 1;
        payload[property.path.substr(property.path.rfind('.') + 1)] = property.getter(object);
    }
}

// 保存区分ごとの項目を実体へ読み戻し、旧データで欠ける項目には記録された既定値を使う。
void EditorPropertyRegistry::ReadClassProperties(engine::Object3D& object, const json& components) const {
    for (const auto& property : properties_) {
        // クラス保存用でない項目と対象に適用できない項目は変更しない。
        if (!HasEditorPropertyFlag(property.flags, EditorPropertyFlags::kClassStorage) ||
            (property.applicable && !property.applicable(object))) continue;
        const std::string key = property.path.substr(property.path.rfind('.') + 1);
        const auto payload = components.find(property.componentTypeId);
        // 区分とキーが揃う場合は、保存された値を登録済みのsetterへ渡す。
        if (payload != components.end() && payload->is_object() && payload->contains(key)) {
            property.setter(object, payload->at(key));
        } else if (!property.defaultValue.is_null()) {
            // 保存値が見つからなかった場合にだけ、null以外の既定値を反映する。
            property.setter(object, property.defaultValue);
        }
    }
}

// 表示条件ではなく保存パスと適用対象を確認し、非表示モードの調整値も保持します。
bool EditorPropertyRegistry::HasSerializedProperties(const engine::Object3D& object, const std::string& prefix) const {
    return std::any_of(properties_.begin(), properties_.end(), [&](const auto& property) {
        return property.serializedPath.compare(0, prefix.size(), prefix) == 0 &&
            (!property.applicable || property.applicable(object));
    });
}
