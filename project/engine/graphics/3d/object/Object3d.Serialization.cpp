#define NOMINMAX
#include <cstdint>
#include "Object3D.h"
#include "EditorPropertyRegistry.h"
#include "engine/graphics/3d/material/MaterialRenderPolicy.h"
#include "engine/graphics/3d/material/WaterParamJsonCodec.h"
#include "DirectXCommon.h"
#include "ModelManager.h"
#include "EffectObject3D.h"
#include "SrvManager.h"
#include "CameraManager.h"
#include "SceneManager.h"
#include "GhostRecorder.h"
#include "CollisionManager.h"
#include <cassert>
#include <algorithm> // min, max
#include <ParticleManager.h>
#include <GpuParticleManager.h>
#include <GpuParticleEmitter.h>
#include <DebugConsole.h>
#include <ProfilerManager.h>
#include <fstream>
#include <filesystem>
#include <initializer_list>

namespace engine {


// Object3D JSON保存・読み込み
// Editor保存、シーンロード、Prefab的な複製で使うObject3dの入出力を担当する。
// 保存項目が増えた時は、このファイル内でExport/Importの対応をそろえる。
bool Object3D::HasComponentPresenceMarker(const std::string& componentTypeId) const {
    // 設定値が空でも Editor 上に配置したコンポーネントを区別する。
    if (componentTypeId.empty()) {
        return false;
    }

    const auto component = opaqueComponents_.find(componentTypeId);
    return component != opaqueComponents_.end() && component->is_object() &&
        component->value("_editorPresent", false);
}

void Object3D::SetComponentPresenceMarker(const std::string& componentTypeId, bool present) {
    // コンポーネントの存在印だけを変更し、未知の設定値は消さない。
    if (componentTypeId.empty()) {
        return;
    }

    // 追加を明示する場合は、部品設定用のJSONに存在フラグを残す。
    if (present) {
        json& component = opaqueComponents_[componentTypeId];

        // 既存のペイロードがオブジェクトでなければ、キーを保存できるように作り直す。
        if (!component.is_object()) {
            component = json::object();
        }

        component["_editorPresent"] = true;
        return;
    }

    const auto component = opaqueComponents_.find(componentTypeId);

    // 削除するフラグを持つオブジェクトがなければ、既存のJSONを変更しない。
    if (component == opaqueComponents_.end() || !component->is_object()) {
        return;
    }

    component->erase("_editorPresent");

    // フラグを消した結果、値を一つも保持していなければ空の部品項目も消す。
    if (component->empty()) {
        opaqueComponents_.erase(component);
    }
}

json Object3D::SerializeFeatureComponents() const {
    // 既知項目を現在の状態で書き直す前に、未知の拡張項目を残す。
    json components = opaqueComponents_.is_object() ? opaqueComponents_ : json::object();

    // 削除された既知Componentの旧値だけ消し、同じJSONに残る未知フィールドは保持します。
    const auto clearKnownPayload = [&components](
        const std::string& typeId,
        std::initializer_list<const char*> knownKeys) {
        // 既知のキーを消せるオブジェクトが無い場合は、現在の項目へ触れない。
        if (!components.contains(typeId) || !components[typeId].is_object()) {
            return;
        }

        json& payload = components[typeId];
        payload.erase("_editorPresent");
        payload.erase("version");

        for (const char* key : knownKeys) {
            payload.erase(key);
        }

        // 既知項目だけを消した結果空になった場合は、この部品の項目ごと除く。
        if (payload.empty()) {
            components.erase(typeId);
        // 将来の形式などの未知項目が残る場合は、再読込に備えて存在しないことだけを記録する。
        } else {
            payload["_editorPresent"] = false;
        }
    };

    // 粒子部品が実体にある場合、現在のCPU・GPU素材名を保存する。
    if (particleEmitterComponent_) {
        json& payload = components[std::string(kParticleEmitterComponentType)];
        // 粒子用の値がオブジェクト形式でなければ、項目を格納できる形に初期化する。
        if (!payload.is_object())
            payload = json::object();
        payload["version"] = 1;
        payload["_editorPresent"] = true;
        payload["cpuParticle"] = particleEmitterComponent_->GetCpuParticle();
        payload["gpuParticle"] = particleEmitterComponent_->GetGpuParticle();
    // 粒子部品がない場合は、旧素材名を保存値から除く。
    } else {
        clearKnownPayload(std::string(kParticleEmitterComponentType), { "cpuParticle", "gpuParticle" });
    }

    // メッシュエフェクト部品があれば、二枠の素材名を保存する。
    if (meshEffectComponent_) {
        json& payload = components[std::string(kMeshEffectComponentType)];
        // 二枠のキーを持てない古い値は、オブジェクトへ置き換える。
        if (!payload.is_object())
            payload = json::object();
        payload["version"] = 1;
        payload["_editorPresent"] = true;
        payload["primary"] = meshEffectComponent_->GetPrimaryEffect();
        payload["secondary"] = meshEffectComponent_->GetSecondaryEffect();
    // メッシュ効果を外している場合は、旧の二枠と存在情報を取り除く。
    } else {
        clearKnownPayload(std::string(kMeshEffectComponentType), { "primary", "secondary" });
    }

    // 移動経路部品があれば、経路名と時間・ループ・乗車の設定を保存する。
    if (pathMoverComponent_) {
        json& payload = components[std::string(kPathMoverComponentType)];
        // 経路の記録がオブジェクトでなければ、各設定キーを持てる形式へ揃える。
        if (!payload.is_object())
            payload = json::object();
        payload["version"] = 1;
        payload["_editorPresent"] = true;
        payload["path"] = pathMoverComponent_->GetPathName();
        payload["loop"] = pathMoverComponent_->IsLoop();
        payload["relative"] = pathMoverComponent_->IsRelative();
        payload["phaseOffset"] = pathMoverComponent_->GetPhaseOffset();
        payload["rideSequence"] = pathMoverComponent_->GetRideSequence();
        payload["playbackMode"] = pathMoverComponent_->GetPlaybackModeName();
    // 経路部品が無い場合は、既知の経路設定を保存値へ持ち越さない。
    } else {
        clearKnownPayload(std::string(kPathMoverComponentType), { "path", "loop", "relative", "phaseOffset", "rideSequence", "playbackMode" });
    }

    // ゲーム連動部品があれば、送信元と接続先の両方を保存する。
    if (gameplayLinkComponent_) {
        json& payload = components[std::string(kGameplayLinkComponentType)];
        // 連動先の旧記録が別の型なら、ID用のオブジェクトを用意する。
        if (!payload.is_object())
            payload = json::object();
        payload["version"] = 1;
        payload["_editorPresent"] = true;
        payload["eventId"] = gameplayLinkComponent_->GetEventId();
        payload["targetId"] = gameplayLinkComponent_->GetTargetId();
    // 連動部品が無ければ、以前の接続IDだけを除く。
    } else {
        clearKnownPayload(std::string(kGameplayLinkComponentType), { "eventId", "targetId" });
    }

    // 経路探索部品があれば、探索範囲・更新間隔・障害物などの設定を保存する。
    if (navAgentComponent_) {
        json& payload = components[std::string(kNavAgentComponentType)];
        // 探索設定をオブジェクトとして保存できるよう、旧形式で別の型なら初期化する。
        if (!payload.is_object())
            payload = json::object();
        payload["version"] = 1;
        payload["_editorPresent"] = true;
        payload["enabled"] = navAgentComponent_->IsEnabled();
        payload["cellSize"] = navAgentComponent_->GetCellSize();
        payload["agentRadius"] = navAgentComponent_->GetAgentRadius();
        payload["agentHeight"] = navAgentComponent_->GetAgentHeight();
        payload["searchPadding"] = navAgentComponent_->GetSearchPadding();
        payload["repathInterval"] = navAgentComponent_->GetRepathInterval();
        payload["stoppingDistance"] = navAgentComponent_->GetStoppingDistance();
        payload["obstacleMask"] = navAgentComponent_->GetObstacleMask();
        payload["allowDiagonal"] = navAgentComponent_->AllowsDiagonal();
    // 探索部品を外した場合は、既知の探索設定を削除する。
    } else {
        clearKnownPayload(std::string(kNavAgentComponentType), {
            "enabled", "cellSize", "agentRadius", "agentHeight", "searchPadding",
            "repathInterval", "stoppingDistance", "obstacleMask", "allowDiagonal"
        });
    }

    // 組み込みのComponentを書き出した後、ゲーム側が登録したクラス固有の設定を同じcomponentsへ加える。
    EditorPropertyRegistry::GetInstance()->WriteClassProperties(*this, components);
    return components;
}

void Object3D::DeserializeFeatureComponents(const json& objectData) {
    // 保存データの未知項目は後で再保存できるよう保持し、既知項目だけ実体へ反映する。
    opaqueComponents_ = objectData.contains("components") && objectData["components"].is_object()
        ? objectData["components"]
        : json::object();

    EditorPropertyRegistry::GetInstance()->ReadClassProperties(*this, opaqueComponents_);

    // 先に現在の実体と再生中の派生物を破棄し、読み直すJSONの状態だけで再構成します。
    particleEmitterComponent_.reset();
    meshEffectComponent_.reset();
    pathMoverComponent_.reset();
    gameplayLinkComponent_.reset();
    navAgentComponent_.reset();
    gpuEmitter_.reset();
    currentMeshEffect1_.clear();
    currentMeshEffect2_.clear();
    attachedEffects1_.clear();
    attachedEffects2_.clear();
    // 経路レコーダーが存在する場合は、置き換え前の再生も止める。
    if (recorder_)
        recorder_->Stop();

    const auto getPayload = [this](std::string_view typeId) -> const json* {
        const std::string key(typeId);
        return opaqueComponents_.contains(key) && opaqueComponents_[key].is_object()
            ? &opaqueComponents_[key]
            : nullptr;
    };

    // 新形式の明示的な有無を優先し、旧形式のフラットな項目も読み込めるようにします。
    const auto resolvePresence = [](const json* payload, bool legacyPresent) {
        // 保存された有無に正しい真偽値があれば、旧形式の値が残っていてもそれを優先する。
        if (payload && payload->contains("_editorPresent") && (*payload)["_editorPresent"].is_boolean()) {
            return (*payload)["_editorPresent"].get<bool>();
        }

        return payload != nullptr || legacyPresent;
    };

    const json* particlePayload = getPayload(kParticleEmitterComponentType);
    // Component形式があればそちらを優先し、古いScene JSONの直下フィールドも読み続ける。
    const std::string legacyCpu = objectData.value("particleName", "");
    const std::string legacyGpu = objectData.value("gpuParticleName", "");

    // 新形式の存在情報か旧素材名が粒子の存在を示す場合にだけ部品を復元する。
    if (resolvePresence(particlePayload, !legacyCpu.empty() || !legacyGpu.empty())) {
        ParticleEmitterComponent* component = EnsureParticleEmitterComponent();
        component->SetCpuParticle(
            particlePayload && particlePayload->contains("cpuParticle")
                ? particlePayload->value("cpuParticle", "") : legacyCpu);
        component->SetGpuParticle(
            particlePayload && particlePayload->contains("gpuParticle")
                ? particlePayload->value("gpuParticle", "") : legacyGpu);
    }

    // メッシュ効果の2枠は、Component内の項目がなければ旧シーン直下の素材名をそれぞれ使う。
    const json* meshEffectPayload = getPayload(kMeshEffectComponentType);
    const std::string legacyPrimary = objectData.value("meshEffect1", "");
    const std::string legacySecondary = objectData.value("meshEffect2", "");

    // 新形式の存在情報か旧二枠の素材名がある場合に、復元する部品を作る。
    if (resolvePresence(meshEffectPayload, !legacyPrimary.empty() || !legacySecondary.empty())) {
        MeshEffectComponent* component = EnsureMeshEffectComponent();
        component->SetPrimaryEffect(
            meshEffectPayload && meshEffectPayload->contains("primary")
                ? meshEffectPayload->value("primary", "") : legacyPrimary);
        component->SetSecondaryEffect(
            meshEffectPayload && meshEffectPayload->contains("secondary")
                ? meshEffectPayload->value("secondary", "") : legacySecondary);
    }

    const json* recorder = objectData.contains("recorder") && objectData["recorder"].is_object()
        ? &objectData["recorder"] : nullptr;
    const json* animation = objectData.contains("animation") && objectData["animation"].is_object()
        ? &objectData["animation"] : nullptr;
    std::string legacyPath;
    bool legacyLoop = false;
    bool legacyRelative = false;
    float legacyPhaseOffset = 0.0f;

    // 旧経路がanimation内に入っている場合は、専用設定を読む前の基準として取得する。
    if (animation) {
        legacyPath = animation->value("recordPathName", "");
        legacyRelative = animation->value("isAnimRelative", false);
    }

    // 旧経路の専用recorder項目が存在すれば、先に読んだ基準値の上へ重ねる。
    if (recorder) {
        // 同じ経路情報が両方ある場合は専用 recorder 側の値を優先する。
        legacyPath = recorder->value("recordPathName", legacyPath);
        legacyLoop = recorder->value("isRecordLoop", false);
        legacyRelative = recorder->value("isRecordRelative", legacyRelative);
        legacyPhaseOffset = recorder->value("phaseOffset", 0.0f);
    }

    const json* pathPayload = getPayload(kPathMoverComponentType);

    // 部品として有効な設定か保存済み経路名がある場合に、移動部品を作る。
    if (resolvePresence(pathPayload, !legacyPath.empty())) {
        PathMoverComponent* component = EnsurePathMoverComponent();
        component->SetRideSequence(pathPayload ? pathPayload->value("rideSequence", "") : "");
        component->SetPathName(
            pathPayload && pathPayload->contains("path")
                ? pathPayload->value("path", "") : legacyPath);
        component->SetLoop(
            pathPayload && pathPayload->contains("loop")
                ? pathPayload->value("loop", false) : legacyLoop);
        component->SetPlaybackModeName(pathPayload ? pathPayload->value("playbackMode", std::string{"always"}) : "always");
        component->SetRelative(
            pathPayload && pathPayload->contains("relative")
                ? pathPayload->value("relative", false) : legacyRelative);
        component->SetPhaseOffset(pathPayload ? pathPayload->value("phaseOffset", legacyPhaseOffset) : legacyPhaseOffset);
    }

    // イベントの送信元と接続先を別のIDとして読み、両方が未指定なら旧形式からはComponentを増やさない。
    const std::int32_t legacyEventId = objectData.value("myEventID", -1);
    const std::int32_t legacyTargetId = objectData.value("targetID", -1);
    const json* linkPayload = getPayload(kGameplayLinkComponentType);

    // 新形式で有効か、旧形式に有効なIDが一つでもあれば連動部品を作る。
    if (resolvePresence(linkPayload, legacyEventId >= 0 || legacyTargetId >= 0)) {
        GameplayLinkComponent* component = EnsureGameplayLinkComponent();
        component->SetEventId(
            linkPayload && linkPayload->contains("eventId")
                ? linkPayload->value("eventId", -1) : legacyEventId);
        component->SetTargetId(
            linkPayload && linkPayload->contains("targetId")
                ? linkPayload->value("targetId", -1) : legacyTargetId);
    }

    const json* navPayload = getPayload(kNavAgentComponentType);

    // NavAgentは旧形式を持たず、部品の値が存在し、明示的に無効化されていなければ作る。
    if (resolvePresence(navPayload, false)) {
        NavAgentComponent* component = EnsureNavAgentComponent();

        // 部品の設定を取得できる場合にだけ、探索の各値を適用する。
        if (navPayload) {
            component->SetCellSize(navPayload->value("cellSize", 1.0f));
            component->SetAgentRadius(navPayload->value("agentRadius", 0.55f));
            component->SetAgentHeight(navPayload->value("agentHeight", 1.5f));
            component->SetSearchPadding(navPayload->value("searchPadding", 5.0f));
            component->SetRepathInterval(navPayload->value("repathInterval", 0.35f));
            component->SetStoppingDistance(navPayload->value("stoppingDistance", 0.25f));
            component->SetObstacleMask(navPayload->value("obstacleMask", engine::collisionCategories::Get("ground")));
            component->SetAllowDiagonal(navPayload->value("allowDiagonal", true));
            component->SetEnabled(navPayload->value("enabled", true));
        }
    }
}

// 永続GUIDを確保し、実体をシーンやPrefabへ保存できるJSONへ変換する。
json Object3D::ExportToJson() {
    json d;
    const bool isManagedCharacter = className_ == "Player" || className_ == "Enemy";

    // 先に実体の識別子を記録し、部品の状態と基本属性をまとめる。
    d["guid"] = EnsurePersistentGuid();
    d["name"] = name_;
    const json components = SerializeFeatureComponents();

    // 部品の保存項目が一つ以上ある場合だけ、専用オブジェクトを出力する。
    if (!components.empty()) {
        d["components"] = components;
    }

    if (!isManagedCharacter) {
        // PlayerとEnemyは共通設定からモデルを決める。通常の配置物だけモデル名とスケールをここで保存する。
        d["modelName"] = GetModelName();
    }

    d["type"] = className_;
    d["tag"] = tag_;
    d["layer"] = layer_.empty() ? "Default" : layer_;
    d["saveCategory"] = saveCategory_;
    d["enemyType"] = enemyType_;
    d["gimmickType"] = gimmickType_;
    d["itemType"] = itemType_;
    d["isVisible"] = isVisible_;
    d["isLocked"] = isLocked_;
    d["isStatic"] = isStatic_;
    d["castShadow"] = castShadow_;
    d["cameraOcclusionMode"] = static_cast<std::int32_t>(cameraOcclusionMode_);

    // Prefabとの関連が有効なら、参照先とインスタンス内の識別子を保存する。
    if (prefabInstanceInfo_.IsLinked()) {
        d["prefabInstance"]["assetId"] = prefabInstanceInfo_.assetId;
        d["prefabInstance"]["prefabName"] = prefabInstanceInfo_.prefabName;
        d["prefabInstance"]["instanceId"] = prefabInstanceInfo_.instanceId;
        d["prefabInstance"]["sourceObjectId"] = prefabInstanceInfo_.sourceObjectId;
        d["prefabInstance"]["isRoot"] = prefabInstanceInfo_.isRoot;
    } else {
        // EditorのUndoスナップショットでは、Unpack済み状態を明示的に表します。
        d["prefabInstance"] = nullptr;
    }

    // カメラに該当する型だけ、シーン内カメラ専用の設定を保存する。
    if (IsCameraObject()) {
        d["camera"] = SerializeSceneCameraSettings(sceneCameraSettings_);
    }

    // ワールドへ配置するためのTransformを保存する。位置と回転は管理キャラクターにも必要になる。
    d["translate"] = { transform_.translate.x, transform_.translate.y, transform_.translate.z };

    // 共通のステータスから大きさを決めない配置物だけ、固有のスケールを保存する。
    if (!isManagedCharacter) {
        d["scale"] = { transform_.scale.x, transform_.scale.y, transform_.scale.z };
    }

    d["rotate"] = { transform_.rotate.x, transform_.rotate.y, transform_.rotate.z };
    d["quaternion"] = { transform_.quaternion.x, transform_.quaternion.y, transform_.quaternion.z, transform_.quaternion.w };

    // 形状とカテゴリを別々に保存し、旧形式のビットと現在の名前付きカテゴリを併記する。
    // コライダーを所有する場合だけ、形状とモデルへの追従設定を出力する。
    if (collider_) {
        const auto& c = collider_->GetConfig();
        d["collider"]["type"] = static_cast<std::int32_t>(c.type);
        d["collider"]["size"] = { c.size.x, c.size.y, c.size.z };
        d["collider"]["center"] = { c.center.x, c.center.y, c.center.z };
        d["collider"]["rotation"] = { c.rotation.x, c.rotation.y, c.rotation.z };
        // モデル表面を使う指定が有効な場合だけ、表面判定のフラグを出力する。
        if (c.useModelSurface)
            d["collider"]["useModelSurface"] = true;
        // モデル内部を使う指定が有効な場合だけ、体積判定のフラグを出力する。
        if (c.useModelVolume)
            d["collider"]["useModelVolume"] = true;
        // 個別の底面頂点がある場合だけ、二成分の座標列を出力する。
        if (!c.footprint.empty()) {
            d["collider"]["footprint"] = nlohmann::json::array();

            for (const engine::Vector2& point : c.footprint) {
                d["collider"]["footprint"].push_back({ point.x, point.y });
            }
        }
    }

    d["collisionAttribute"] = GetCollisionAttribute();
    d["collisionMask"] = GetCollisionMask();
    d["collisionAttributeIds"] = engine::collisionCategories::GetIds(GetCollisionAttribute());
    d["collisionMaskIds"] = engine::collisionCategories::GetIds(GetCollisionMask());
    // 名前付きカテゴリだけで表せないビットも別に保存し、読み直した際に現在のレジストリで未知のパターンを保ちます。
    d["collisionAttributeUnknownBits"] = GetCollisionAttribute() & ~engine::collisionCategories::GetKnownMask();
    d["collisionMaskUnknownBits"] = GetCollisionMask() & ~engine::collisionCategories::GetKnownMask();

    // 地形形状を外部ファイルから読む設定の場合だけ、その参照パスを保存する。
    if (!terrainCollisionPath_.empty()) {
        d["terrainCollisionPath"] = terrainCollisionPath_;
    }

    // イベントの種類と、送信・受信で使うIDを旧形式と互換のキーへ書く。
    d["eventType"] = static_cast<std::int32_t>(eventType_);
     d["targetID"] = GetTargetId();
     d["myEventID"] = GetEventId();

    // パラメーターがある場合は、共通管理される値と配置ごとの設定を分けて保存する。
    if (param_.has_value()) {
        auto& p = param_.value();
        json jp;

        // プレイヤーと敵の基本ステータスは共通設定から戻すため、個体の配置に要る値だけを出す。
        if (isManagedCharacter) {
            // 管理対象の中では、敵の種別が指定されている場合だけ敵固有の設定を選ぶ。
            if (className_ == "Enemy" && !p.enemyType.empty()) {
                jp["enemyType"] = p.enemyType;

                // プリセットや再生開始時のスナップショットにも出現条件を保持します。
                if (p.enemyType == "PrismSlime" || p.enemyType == "MagmaSlime" || p.enemyType == "FalseKingSlime") {
                    jp["actionMode"] = p.actionMode;
                    jp["shakeDuration"] = p.shakeDuration;
                    jp["startActive"] = p.startActive;
                }

                // リング攻撃の配置ごとの調整値は共通ステータスとは別に保持する。
                if (p.enemyType == "RingBurner") {
                    jp["moveAmount"] = p.moveAmount;
                    jp["interval"] = p.interval;
                    jp["shakeDuration"] = p.shakeDuration;
                    jp["ringWaveSpeed"] = p.ringWaveSpeed;
                }
            }
        // 共通管理の対象外では、ギミック・アイテムなどのパラメーターを配置ごとに保存する。
        } else {
            jp["hp"] = p.hp;
            jp["maxHp"] = p.maxHp;
            jp["attackPower"] = p.attackPower;
            jp["speed"] = p.speed;
            jp["gravity"] = p.gravity;
            jp["jumpPower"] = p.jumpPower;
            jp["maxFallSpeed"] = p.maxFallSpeed;
            jp["morphLimited"] = p.morphLimited;
            jp["morphDuration"] = p.morphDuration;
            jp["enemyType"] = p.enemyType;
            jp["gimmickType"] = p.gimmickType;
            jp["itemType"] = p.itemType;
            jp["healAmount"] = p.healAmount;
            jp["interval"] = p.interval;
            jp["maxCount"] = p.maxCount;
            jp["detectionRange"] = p.detectionRange;
            jp["projectileLifetime"] = p.projectileLifetime;
            jp["colorType"] = p.colorType;
            jp["shakeDuration"] = p.shakeDuration;
            jp["fallDuration"] = p.fallDuration;
            jp["switchMode"] = p.switchMode;
            jp["actionMode"] = p.actionMode;
            jp["targetScene"] = p.targetScene;
            jp["moveAmount"] = p.moveAmount;
            jp["moveSpeed"] = p.moveSpeed;
            jp["ringWaveSpeed"] = p.ringWaveSpeed;
            jp["startActive"] = p.startActive;
            jp["returnOnOff"] = p.returnOnOff;
            jp["editorPreviewFiring"] = p.editorPreviewFiring;
            // Sceneと単体Objectの両方で、ボリューム設定と非表示中の値を保持します。
            jp["volumeMode"] = p.volumeMode;
            jp["volumePayload"] = p.volumePayload;
            jp["volumeTriggerOnce"] = p.volumeTriggerOnce;
            jp["volumeTriggerOnExit"] = p.volumeTriggerOnExit;
            jp["volumeRearmDelay"] = p.volumeRearmDelay;
            jp["volumeBlendDuration"] = p.volumeBlendDuration;
            // 演出素材が指定されている場合だけ、シーケンスへの参照パスを保存する。
            if (!p.cinematicPath.empty()) jp["cinematicPath"] = p.cinematicPath;
            jp["copyMemoryTypeA"] = p.copyMemoryTypeA;
            jp["copyMemoryTypeB"] = p.copyMemoryTypeB;
            jp["copyMemoryTypeC"] = p.copyMemoryTypeC;
            jp["copyMemoryTypeD"] = p.copyMemoryTypeD;
            jp["copyMemoryActivationRadius"] = p.copyMemoryActivationRadius;
            jp["copyMemoryEssenceScale"] = p.copyMemoryEssenceScale;
            jp["copyMemoryHoverAmount"] = p.copyMemoryHoverAmount;
            jp["copyMemoryGlow"] = p.copyMemoryGlow;
            jp["copyMemoryUnlimitedDuration"] = p.copyMemoryUnlimitedDuration;
        }

        // 管理対象などで保存する値が一つもない場合は、空のparamを出力しない。
        if (!jp.empty()) {
            d["param"] = jp;
        }
    }

    // 参照Assetの有無に関わらず描画値を保存し、同じ設定を復元できるようにする。
    // 材質Assetとリンクしている場合だけ、再解決に使うパスも記録する。
    if (!materialInstancePath_.empty()) {
        d["materialInstance"] = materialInstancePath_;
    }

    engine::Vector4 col = GetColor();
    d["color"] = { col.x, col.y, col.z, col.w };
    d["blendMode"] = static_cast<std::int32_t>(GetBlendMode());
    d["materialType"] = GetMaterialType();
    // シェーダー材質の識別子が設定されている場合だけ、材質の番号と合わせて保存する。
    if (!GetShaderMaterialId().empty())
        d["shaderMaterialId"] = GetShaderMaterialId();
    d["meshDrawIndex"] = GetMeshDrawIndex();

    // 材質の金属度と粗さを、編集した値で復元できるように保存する。
    d["metallic"] = GetMetallic();
    d["roughness"] = GetRoughness();

    d["meshEffect1"] = GetMeshEffect1Name();
    d["meshEffect2"] = GetMeshEffect2Name();
    // Componentsと併せて直下の粒子名も出力し、古いデータ経路で読める表現を保持します。
    d["particleName"] = GetParticleName();
    d["gpuParticleName"] = GetGpuParticleName();

    d["enableNormalMap"] = GetEnableNormalMap();
    d["normalMapPath"] = GetNormalMapPath();
    d["ormMapPath"] = GetOrmMapPath();
    d["texturePath"] = GetTexturePath();
    engine::Vector2 tiling = GetTextureTiling();
    d["textureTiling"] = { tiling.x, tiling.y };
    d["autoTextureTiling"] = GetAutoTextureTiling();
    d["enableLighting"] = GetEnableLighting();
    d["enableEnvMap"] = GetEnableEnvMap();
    d["envIntensity"] = GetEnvIntensity();
    d["emissive"] = GetEmissive();

    // デカールとして有効な場合に、面からの距離・寿命・フェードを出力する。
    if (decalSettings_.enabled) {
        d["decal"] = {
            { "enabled", true },
            { "size", { decalSettings_.size.x, decalSettings_.size.y } },
            { "depthOffset", decalSettings_.depthOffset },
            { "lifetime", decalSettings_.lifetime },
            { "fadeIn", decalSettings_.fadeIn },
            { "fadeOut", decalSettings_.fadeOut },
            { "transient", decalSettings_.transient }
        };
    }

    // 特殊材質か水系の保存項目を持ち、描画担当と定数が利用できる場合は特殊材質値も保存する。
    if ((materialRenderPolicy::IsSpecial(GetMaterialType()) ||
         EditorPropertyRegistry::GetInstance()->HasSerializedProperties(*this, "waterParam.")) && GetMeshRenderer() && GetMeshRenderer()->GetWaterParamData()) {
        auto* water = GetMeshRenderer()->GetWaterParamData();
        d["waterParam"] = waterParamJsonCodec::Write(*water);
    }

    // 再生に指定されたクリップ名・ループ・Animatorの参照を保存する。
    d["animation"]["animName"] = animName_;
    d["animation"]["isAnimLoop"] = isAnimLoop_;
    d["animation"]["animatorController"] = animatorControllerPath_;

    // 旧レコーダーから読み直せるよう、経路の現在値も別項目へ書く。
    d["recorder"]["recordPathName"] = GetRecordPathName();
    d["recorder"]["isRecordLoop"] = IsRecordLoop();
    d["recorder"]["isRecordRelative"] = IsRecordRelative();
    d["recorder"]["phaseOffset"] = pathMoverComponent_ ? pathMoverComponent_->GetPhaseOffset() : 0.0f;

    // ローカルフォグの定数が存在する場合に、霧の色や密度を保存する。
    if (auto* fogData = GetLocalFogData()) {
        d["localFog"]["color"] = { fogData->fogColor.x, fogData->fogColor.y, fogData->fogColor.z, fogData->fogColor.w };
        d["localFog"]["density"] = fogData->fogDensity;
        d["localFog"]["edgeFade"] = fogData->edgeFade;
        d["localFog"]["noiseSpeed"] = fogData->noiseSpeed;
        d["localFog"]["noiseScale"] = fogData->noiseScale;
        d["localFog"]["scatteringG"] = fogData->scatteringG;
        d["localFog"]["scatteringIntensity"] = fogData->scatteringIntensity;
    }

    return d;
}

void Object3D::ImportFromJson(const json& j) {
    // GUIDを確定してから、旧形式を含むComponentとオブジェクトの種別を復元する。
    // 有効な文字列のGUIDがある場合は取り込み、無ければ後で既存値または新規IDを確保する。
    if (j.contains("guid") && j["guid"].is_string()) {
        SetPersistentGuid(j["guid"].get<std::string>());
    }

    EnsurePersistentGuid();

    // 実体Componentと未知Payloadを一度に復元し、旧Top-Level形式も受け入れます。
    DeserializeFeatureComponents(j);
    // 保存側が型を指定している場合にだけ、実体の分類を更新する。
    if (j.contains("type"))
        className_ = j["type"];
    const bool isManagedCharacter = className_ == "Player" || className_ == "Enemy";
    // モデルを共通管理しない型でモデル名が保存されていれば、そのモデルを割り当てる。
    if (!isManagedCharacter && j.contains("modelName"))
        SetModel(j["modelName"].get<std::string>());
    // タグの文字列があれば、検索や分類で使う値を戻す。
    if (j.contains("tag") && j["tag"].is_string()) {
        tag_ = j["tag"].get<std::string>();
    }

    // レイヤー名が文字列なら現在の設定に適用する。
    if (j.contains("layer") && j["layer"].is_string()) {
        SetLayer(j["layer"].get<std::string>());
    // 保存された名前を使えず現在値も空の場合だけ既定レイヤーにする。
    } else if (layer_.empty()) {
        layer_ = "Default";
    }

    // 以下の分類・表示・保存フラグは、保存されたキーが存在する場合だけ更新する。
    if (j.contains("saveCategory"))
        saveCategory_ = j["saveCategory"];
    if (j.contains("enemyType"))
        enemyType_ = j["enemyType"];
    if (j.contains("gimmickType"))
        gimmickType_ = j["gimmickType"];
    if (j.contains("itemType"))
        itemType_ = j["itemType"];
    if (j.contains("isVisible"))
        isVisible_ = j["isVisible"];
    if (j.contains("isLocked"))
        isLocked_ = j["isLocked"];
    if (j.contains("isStatic"))
        isStatic_ = j["isStatic"];
    if (j.contains("castShadow"))
        castShadow_ = j["castShadow"].get<bool>();
    // 旧JSONは生成時の方針を維持する。不正な指定では安全な自動へ戻す。
    if (j.contains("cameraOcclusionMode")) {
        cameraOcclusionMode_ = CameraOcclusionMode::kAutomatic;
        // 整数形式の値だけを列挙の候補として読む。
        if (j["cameraOcclusionMode"].is_number_integer()) {
            const auto value = j["cameraOcclusionMode"].get<int64_t>();
            // 定義済みの0〜2のみを採用し、外れた値は自動のままにする。
            if (value >= 0 && value <= 2) cameraOcclusionMode_ = static_cast<CameraOcclusionMode>(value);
        }
    }
    // Prefab参照の項目が保存されている場合は、関連付けの復元または解除を行う。
    if (j.contains("prefabInstance")) {
        // オブジェクト形式の値がある場合は、Assetと元ノードへの参照を復元する。
        if (j["prefabInstance"].is_object()) {
            const auto& prefab = j["prefabInstance"];
            prefabInstanceInfo_.assetId = prefab.value("assetId", "");
            prefabInstanceInfo_.prefabName = prefab.value("prefabName", "");
            prefabInstanceInfo_.instanceId = prefab.value("instanceId", "");
            prefabInstanceInfo_.sourceObjectId = prefab.value("sourceObjectId", "");
            prefabInstanceInfo_.isRoot = prefab.value("isRoot", false);
        // nullはPrefabから切り離された状態を表すため、残っている参照を消す。
        } else if (j["prefabInstance"].is_null()) {
            prefabInstanceInfo_ = PrefabInstanceInfo{};
        }
    }

    // 現在または旧形式のカメラ型なら、保存種別をカメラへ統一する。
    if (IsCameraObject()) {
        // 旧CinematicCameraも読み込み時に新しいCamera Objectとして扱います。
        className_ = "Camera";
        saveCategory_ = "Camera";

        // 個別のカメラ設定がある場合だけ、視点と演出の保存値を復元する。
        if (j.contains("camera")) {
            DeserializeSceneCameraSettings(j["camera"], sceneCameraSettings_);
        }
    }

    // 姿勢の正本を選び、後続のColliderやレコーダーが使う行列を更新する。
    // 位置の配列が指定されている場合は、ローカルの平行移動を読み込む。
    if (j.contains("translate"))
        transform_.translate = { j["translate"][0], j["translate"][1], j["translate"][2] };
    // 管理キャラクターの共通サイズは維持し、その他では保存された倍率を使う。
    if (!isManagedCharacter && j.contains("scale"))
        transform_.scale = { j["scale"][0], j["scale"][1], j["scale"][2] };

    // クォータニオンが保存されていれば、姿勢の計算にはその値を優先する。
    if (j.contains("quaternion")) {
        transform_.quaternion = { j["quaternion"][0], j["quaternion"][1], j["quaternion"][2], j["quaternion"][3] };
        transform_.isQuaternionMaster = true;

        // Euler値も併記されている場合は、回転の各軸の表示値を戻す。
        if (j.contains("rotate"))
            transform_.rotate = { j["rotate"][0], j["rotate"][1], j["rotate"][2] };
    }
    // クォータニオンが無くEuler値がある旧形式は、Eulerを姿勢の基準にする。
    else if (j.contains("rotate")) {
        transform_.rotate = { j["rotate"][0], j["rotate"][1], j["rotate"][2] };
        transform_.isQuaternionMaster = false;
    }

    transform_.UpdateMatrix();

    // 既存Colliderへ形状を適用してから、衝突カテゴリと地形データを復元する。
    if (j.contains("collider") && collider_) {
        const auto& col = j["collider"];
        engine::ColliderConfig config = collider_->GetConfig();
        // 形状の各キーは存在する場合だけ既存設定を上書きする。モデル利用と底面は続く処理で読込値に揃える。
        if (col.contains("type"))
            config.type = static_cast<engine::ColliderType>(col["type"]);
        if (col.contains("size"))
            config.size = { col["size"][0], col["size"][1], col["size"][2] };
        if (col.contains("center"))
            config.center = { col["center"][0], col["center"][1], col["center"][2] };
        if (col.contains("rotation"))
            config.rotation = { col["rotation"][0], col["rotation"][1], col["rotation"][2] };
        config.useModelSurface = col.value("useModelSurface", false);
        config.useModelVolume = col.value("useModelVolume", false);
        config.footprint.clear();

        // 底面の頂点が配列で保存されている場合だけ、頂点を一つずつ読み込む。
        if (col.contains("footprint") && col["footprint"].is_array()) {
            for (const auto& point : col["footprint"]) {
                // 二成分の数値を持つ項目だけ、底面の有効な点として追加する。
                if (point.is_array() && point.size() == 2 &&
                    point[0].is_number() && point[1].is_number()) {
                    config.footprint.push_back({ point[0].get<float>(), point[1].get<float>() });
                }
            }
        }

        SetColliderConfig(config);
    }

    // ID配列を優先し、新形式がなければ旧マスクを変換する。キーがなければ現在値を使う。
    auto loadCategoryMask = [&j](const char* idsKey, const char* legacyKey, uint32_t fallback) {
        // 名前付きカテゴリの配列があるときは、旧ビットより優先する。
        if (j.contains(idsKey) && j[idsKey].is_array()) {
            std::vector<std::string> ids;

            for (const auto& id : j[idsKey]) {
                // 配列中の文字列だけを、現在のカテゴリ登録へ渡す。
                if (id.is_string())
                    ids.push_back(id.get<std::string>());
            }

            const std::string unknownKey = std::string(idsKey) == "collisionAttributeIds"
                ? "collisionAttributeUnknownBits" : "collisionMaskUnknownBits";
            const uint32_t unknown = j.value(unknownKey, uint32_t{0});
            return engine::collisionCategories::FromIds(ids) | unknown;
        }

        // 旧形式の値が符号なしなら、その型で読み取って現在のマスクへ変換する。
        if (j.contains(legacyKey) && j[legacyKey].is_number_unsigned()) {
            return engine::collisionCategories::FromLegacyMask(j[legacyKey].get<uint32_t>());
        }

        // 符号付きの旧形式も、ビット表現を変換して現在のマスクへ移す。
        if (j.contains(legacyKey) && j[legacyKey].is_number_integer()) {
            return engine::collisionCategories::FromLegacyMask(static_cast<uint32_t>(j[legacyKey].get<std::int32_t>()));
        }

        return fallback;
    };

    SetCollisionAttribute(loadCategoryMask("collisionAttributeIds", "collisionAttribute", GetCollisionAttribute()));
    SetCollisionMask(loadCategoryMask("collisionMaskIds", "collisionMask", GetCollisionMask()));

    // 外部地形のパスが文字列で指定されているときだけ、地形衝突データを読み込む。
    if (j.contains("terrainCollisionPath") && j["terrainCollisionPath"].is_string()) {
        LoadTerrainCollisionFromFile(j["terrainCollisionPath"].get<std::string>());
    }

    // イベント種別を復元してから、配置ごとのパラメータを読み込む。
    if (j.contains("eventType"))
        eventType_ = static_cast<EventType>(j["eventType"]);

    // Player・Enemyの共通ステータスを保持しつつ、配置固有の値を復元する。
    // 個体のparamが保存されている場合に、その配置で決める値を読み直す。
    if (j.contains("param")) {
        EntityParameter p = isManagedCharacter && param_.has_value() ? param_.value() : EntityParameter{};
        const auto& jp = j["param"];
        const bool hasHp = !isManagedCharacter && jp.contains("hp");
        const bool hasMaxHp = !isManagedCharacter && jp.contains("maxHp");
        // 以下の数値は、フィールドがある場合にだけ上書きする。管理キャラクターのHP・能力値・探知距離は共通設定を優先する。
        if (hasHp)
            p.hp = jp["hp"];
        if (hasMaxHp)
            p.maxHp = jp["maxHp"];
        if (!isManagedCharacter && jp.contains("attackPower"))
            p.attackPower = jp["attackPower"];
        if (!isManagedCharacter && jp.contains("speed"))
            p.speed = jp["speed"];
        if (!isManagedCharacter && jp.contains("gravity"))
            p.gravity = jp["gravity"];
        if (!isManagedCharacter && jp.contains("jumpPower"))
            p.jumpPower = jp["jumpPower"];
        if (!isManagedCharacter && jp.contains("maxFallSpeed"))
            p.maxFallSpeed = jp["maxFallSpeed"];
        if (!isManagedCharacter && jp.contains("morphLimited"))
            p.morphLimited = jp["morphLimited"];
        if (!isManagedCharacter && jp.contains("morphDuration"))
            p.morphDuration = jp["morphDuration"];
        // 種類と動作の値は配置固有のため、該当するキーの存在を確かめて読む。
        if (jp.contains("enemyType"))
            p.enemyType = jp["enemyType"];
        if (jp.contains("gimmickType"))
            p.gimmickType = jp["gimmickType"];
        if (jp.contains("itemType"))
            p.itemType = jp["itemType"];
        if (jp.contains("healAmount"))
            p.healAmount = jp["healAmount"];
        if (jp.contains("interval"))
            p.interval = jp["interval"];
        if (jp.contains("maxCount"))
            p.maxCount = jp["maxCount"];
        if (!isManagedCharacter && jp.contains("detectionRange"))
            p.detectionRange = jp["detectionRange"];
        if (jp.contains("projectileLifetime"))
            p.projectileLifetime = jp["projectileLifetime"];
        if (jp.contains("colorType"))
            p.colorType = jp["colorType"];
        if (jp.contains("shakeDuration"))
            p.shakeDuration = jp["shakeDuration"];
        if (jp.contains("fallDuration"))
            p.fallDuration = jp["fallDuration"];
        if (jp.contains("switchMode"))
            p.switchMode = jp["switchMode"];
        if (jp.contains("actionMode"))
            p.actionMode = jp["actionMode"];
        if (jp.contains("targetScene"))
            p.targetScene = jp["targetScene"].get<std::string>();
        if (jp.contains("cinematicPath"))
            p.cinematicPath = jp["cinematicPath"].get<std::string>();
        if (jp.contains("moveAmount"))
            p.moveAmount = jp["moveAmount"];
        if (jp.contains("moveSpeed"))
            p.moveSpeed = jp["moveSpeed"];
        // リングの速度が保存されている場合は、利用範囲の0.25〜4へ制限する。
        if (jp.contains("ringWaveSpeed"))
            p.ringWaveSpeed = (std::clamp)(jp["ringWaveSpeed"].get<float>(), 0.25f, 4.0f);
        if (jp.contains("startActive"))
            p.startActive = jp["startActive"];
        if (jp.contains("returnOnOff"))
            p.returnOnOff = jp["returnOnOff"];
        // ボリュームの発火条件・再発火待ち・環境切替時間を復元し、時間の負値は0に丸める。
        if (jp.contains("volumeMode") && jp["volumeMode"].is_number_integer())
            p.volumeMode = jp["volumeMode"].get<std::int32_t>();
        if (jp.contains("volumePayload") && jp["volumePayload"].is_string())
            p.volumePayload = jp["volumePayload"].get<std::string>();
        if (jp.contains("volumeTriggerOnce") && jp["volumeTriggerOnce"].is_boolean())
            p.volumeTriggerOnce = jp["volumeTriggerOnce"].get<bool>();
        if (jp.contains("volumeTriggerOnExit") && jp["volumeTriggerOnExit"].is_boolean())
            p.volumeTriggerOnExit = jp["volumeTriggerOnExit"].get<bool>();
        if (jp.contains("volumeRearmDelay") && jp["volumeRearmDelay"].is_number())
            p.volumeRearmDelay = (std::max)(0.0f, jp["volumeRearmDelay"].get<float>());
        if (jp.contains("volumeBlendDuration") && jp["volumeBlendDuration"].is_number())
            p.volumeBlendDuration = (std::max)(0.0f, jp["volumeBlendDuration"].get<float>());
        // 発射プレビューの設定は、明示的な真偽値を取得できる場合だけ読み込む。
        if (jp.contains("editorPreviewFiring") && jp["editorPreviewFiring"].is_boolean())
            p.editorPreviewFiring = jp["editorPreviewFiring"].get<bool>();
        // コピー能力の記憶装置は、各枠の能力名と表示・有効距離を配置ごとに復元する。
        if (jp.contains("copyMemoryTypeA"))
            p.copyMemoryTypeA = jp["copyMemoryTypeA"].get<std::string>();
        if (jp.contains("copyMemoryTypeB"))
            p.copyMemoryTypeB = jp["copyMemoryTypeB"].get<std::string>();
        if (jp.contains("copyMemoryTypeC"))
            p.copyMemoryTypeC = jp["copyMemoryTypeC"].get<std::string>();
        if (jp.contains("copyMemoryTypeD"))
            p.copyMemoryTypeD = jp["copyMemoryTypeD"].get<std::string>();
        // 作動範囲が指定されていれば、極端に短くならないよう最小半径を適用する。
        if (jp.contains("copyMemoryActivationRadius"))
            p.copyMemoryActivationRadius = (std::max)(0.1f, jp["copyMemoryActivationRadius"].get<float>());
        if (jp.contains("copyMemoryEssenceScale"))
            p.copyMemoryEssenceScale = jp["copyMemoryEssenceScale"].get<float>();
        if (jp.contains("copyMemoryHoverAmount"))
            p.copyMemoryHoverAmount = jp["copyMemoryHoverAmount"].get<float>();
        if (jp.contains("copyMemoryGlow"))
            p.copyMemoryGlow = jp["copyMemoryGlow"].get<float>();
        if (jp.contains("copyMemoryUnlimitedDuration"))
            p.copyMemoryUnlimitedDuration = jp["copyMemoryUnlimitedDuration"];
        // HPと攻撃力の補正は、共通ステータスで管理されていない配置物だけに行う。
        if (!isManagedCharacter) {
            p.maxHp = (std::max)(p.maxHp, 1.0f);

            // 最大HPだけ指定された場合は満タンから始めます。現在HPが最大を上回る保存値では最大HPを引き上げます。
            if (hasMaxHp && !hasHp) {
                p.hp = p.maxHp;
            }

            p.hp = (std::max)(p.hp, 0.0f);

            // 現在HPの保存値の方が大きい場合は、その値を保持するため最大HPを引き上げる。
            if (p.hp > p.maxHp) {
                p.maxHp = p.hp;
            }

            p.attackPower = (std::max)(p.attackPower, 0.0f);
        }

        param_ = p;
    }

    // 描画方式を決めてから、Sceneに保存された材質・テクスチャ・水面の値を適用する。
    // 次の描画項目は、キーが保存されている場合だけ、対応する設定を順に適用する。
    if (j.contains("color"))
        SetColor({ j["color"][0], j["color"][1], j["color"][2], j["color"][3] });
    if (j.contains("blendMode"))
        SetBlendMode(static_cast<BlendMode>(j["blendMode"]));
    if (j.contains("materialType"))
        SetMaterialType(j["materialType"]);
    if (j.contains("shaderMaterialId") && j["shaderMaterialId"].is_string())
        SetShaderMaterialId(j["shaderMaterialId"].get<std::string>());
    if (j.contains("meshDrawIndex"))
        SetMeshDrawIndex(j["meshDrawIndex"].get<std::int32_t>());

    if (j.contains("metallic"))
        SetMetallic(j["metallic"].get<float>());
    if (j.contains("roughness"))
        SetRoughness(j["roughness"].get<float>());


    if (j.contains("enableNormalMap"))
        SetEnableNormalMap(j["enableNormalMap"]);
    if (j.contains("normalMapPath"))
        SetNormalMap(j["normalMapPath"]);
    if (j.contains("ormMapPath"))
        SetOrmMap(j["ormMapPath"]);
    if (j.contains("texturePath"))
        SetTexture(j["texturePath"]);
    // UVの倍率は二成分以上の配列が存在する場合だけ更新する。
    if (j.contains("textureTiling") && j["textureTiling"].is_array() && j["textureTiling"].size() >= 2) {
        SetTextureTiling({ j["textureTiling"][0].get<float>(), j["textureTiling"][1].get<float>() });
    }

    if (j.contains("autoTextureTiling"))
        SetAutoTextureTiling(j["autoTextureTiling"].get<bool>());
    if (j.contains("enableLighting"))
        SetEnableLighting(j["enableLighting"].get<bool>());
    if (j.contains("enableEnvMap"))
        SetEnableEnvMap(j["enableEnvMap"]);
    if (j.contains("envIntensity"))
        SetEnvIntensity(j["envIntensity"]);
    if (j.contains("emissive"))
        SetEmissive(j["emissive"].get<float>());
    // 水系の記録があり、現在の材質かRegistryがその設定を扱える場合だけ復元する。
    if (j.contains("waterParam") && (materialRenderPolicy::IsSpecial(GetMaterialType()) ||
        EditorPropertyRegistry::GetInstance()->HasSerializedProperties(*this, "waterParam."))) {
        // 描画担当から更新先の定数を取得できる場合に限り、水面データを適用する。
        if (GetMeshRenderer() && GetMeshRenderer()->GetWaterParamData()) {
            auto* water = GetMeshRenderer()->GetWaterParamData();
            waterParamJsonCodec::Read(j["waterParam"], *water);
        }
    }

    // Scene側の材質値を用意してから、利用可能ならリンク先の材質を適用する。
    if (j.contains("materialInstance") && j["materialInstance"].is_string()) {
        const std::string assetPath = j["materialInstance"].get<std::string>();
        std::string materialError;

        if (!ApplyMaterialInstance(assetPath, &materialError)) {
            // Assetが一時的に見つからなくても、Scene内の解決済み値とリンク先は保持します。
            materialInstancePath_ = assetPath;
        }
    }

    // Decal設定を復元した場合は、保存した寿命・フェードで再生を開始し直す。
    if (j.contains("decal") && j["decal"].is_object()) {
        const auto& decal = j["decal"];
        DecalSettings settings;
        settings.enabled = decal.value("enabled", true);

        // デカールのサイズ配列に横と縦がある場合は、標準サイズを上書きする。
        if (decal.contains("size") && decal["size"].is_array() && decal["size"].size() >= 2) {
            settings.size = { decal["size"][0].get<float>(), decal["size"][1].get<float>() };
        }

        settings.depthOffset = decal.value("depthOffset", 0.012f);
        settings.lifetime = decal.value("lifetime", 0.0f);
        settings.fadeIn = decal.value("fadeIn", 0.0f);
        settings.fadeOut = decal.value("fadeOut", 0.35f);
        settings.transient = decal.value("transient", false);
        SetDecalSettings(settings);
        // 色と材質の復元後に再生を始め、その時点のアルファを今後のフェードの基準として確定します。
        RestartDecalPlayback();
    }

    // 描画設定の復元後、保存されたアニメーションとAnimator Controllerを接続する。
    if (j.contains("animation")) {
        const auto& anim = j["animation"];
        // クリップの名前が保存されている場合だけ現在の再生素材を更新する。
        if (anim.contains("animName"))
            animName_ = anim["animName"];
        // ループの設定が保存されている場合だけ繰り返し条件を更新する。
        if (anim.contains("isAnimLoop"))
            isAnimLoop_ = anim["isAnimLoop"];
        // コントローラーのキーがある場合は、名前の有無に従って付け替える。
        if (anim.contains("animatorController")) {
            const std::string controller = anim.value("animatorController", "");

            // 空でないパスを取得できた場合は、指定のAnimatorへ切り替える。
            if (!controller.empty()) {
                SetAnimatorController(controller);
            // 明示したパスが空なら、これまで接続していたAnimatorを解除する。
            } else {
                ClearAnimatorController();
            }
        }
    }

    // 復元した配置とComponentを使い、指定があればGhost経路を再生する。
    if (recorder_ && !GetRecordPathName().empty()) {
        bool isCinematic = IsCameraObject();
        recorder_->Play(GetRecordPathName(), IsRecordLoop(), IsRecordRelative(), isCinematic);
    }

    // ローカルフォグのバッファが利用できる場合に、保存値を反映する。
    if (j.contains("localFog")) {
        // 霧の描画定数が存在する場合は、保存値があるフィールドだけを上書きする。
        if (auto* fogData = GetLocalFogData()) {
            const auto& jf = j["localFog"];

            if (jf.contains("color"))
                fogData->fogColor = { jf["color"][0], jf["color"][1], jf["color"][2], jf["color"][3] };
            if (jf.contains("density"))
                fogData->fogDensity = jf["density"];
            if (jf.contains("edgeFade"))
                fogData->edgeFade = jf["edgeFade"];
            if (jf.contains("noiseSpeed"))
                fogData->noiseSpeed = jf["noiseSpeed"];
            if (jf.contains("noiseScale"))
                fogData->noiseScale = jf["noiseScale"];
            if (jf.contains("scatteringG"))
                fogData->scatteringG = jf["scatteringG"];
            if (jf.contains("scatteringIntensity"))
                fogData->scatteringIntensity = jf["scatteringIntensity"];
        }
    }
}

} // namespace engine
