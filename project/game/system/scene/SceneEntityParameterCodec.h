#pragma once

// シーンJSONとEntityParameterの対応をまとめる。タイプ共通ステータスを配置値から上書きしない読込経路も含む。

#include "game/system/rules/GameplayStatusManager.h"
#include "Object3D.h"
#include "json.hpp"

#include <algorithm>

// Scene JSON の param と実行時設定の対応を保存・読込で共有します。
namespace sceneEntityParameterCodec {

// 配置Objectの param をScene JSONへ変換する。
// 共通ステータス管理対象は個体値を出力せず、敵種別とボス固有の動作設定だけを残す。
inline nlohmann::json Serialize(const engine::Object3D& object) {
    // 配置パラメータを持たないオブジェクトは、保存する値が無いためJSONのnullを返す。
    if (!object.GetParameters())
        return nlohmann::json();
    const auto p = *object.GetParameters();
    nlohmann::json result = nlohmann::json::object();
    const std::string className = object.GetClassName();

    // 共通ステータスを参照するキャラクターのHP等はSceneへ複製せず、個体固有値だけ残す。
    if (GameplayStatusManager::IsManagedCharacter(&object)) {
        // 共通管理のオブジェクトのうち、クラス名がEnemyで種別が設定済みなら、配置から再生成できるよう種別を保存する。
        if (className == "Enemy" && !p.enemyType.empty()) {
            result["enemyType"] = p.enemyType;
            // プリズム、マグマ、偽王の各スライムは、個体別の動作モードと開始条件も残す。
            if (p.enemyType == "PrismSlime" || p.enemyType == "MagmaSlime" || p.enemyType == "FalseKingSlime") {
                result["actionMode"] = p.actionMode;
                result["shakeDuration"] = p.shakeDuration;
                result["startActive"] = p.startActive;
            }
            // RingBurnerだけは、個体の攻撃間隔とリング波の設定を保存する。
            if (p.enemyType == "RingBurner") {
                // 共通HPとは独立した個体の攻撃設定だけをSceneに残す。
                result["moveAmount"] = p.moveAmount;
                result["interval"] = p.interval;
                result["shakeDuration"] = p.shakeDuration;
                result["ringWaveSpeed"] = p.ringWaveSpeed;
            }
        }
        return result;
    }

    // 非管理Objectはシーンで調整した全項目を往復できるよう、同じキー集合で保存する。
#define SCENE_PARAM_WRITE(field) result[#field] = p.field
    SCENE_PARAM_WRITE(hp);
    SCENE_PARAM_WRITE(maxHp);
    SCENE_PARAM_WRITE(attackPower);
    SCENE_PARAM_WRITE(speed);
    SCENE_PARAM_WRITE(gravity);
    SCENE_PARAM_WRITE(jumpPower);
    SCENE_PARAM_WRITE(maxFallSpeed);
    SCENE_PARAM_WRITE(morphLimited);
    SCENE_PARAM_WRITE(morphDuration);
    SCENE_PARAM_WRITE(enemyType);
    SCENE_PARAM_WRITE(gimmickType);
    SCENE_PARAM_WRITE(itemType);
    SCENE_PARAM_WRITE(healAmount);
    SCENE_PARAM_WRITE(interval);
    SCENE_PARAM_WRITE(maxCount);
    SCENE_PARAM_WRITE(detectionRange);
    SCENE_PARAM_WRITE(projectileLifetime);
    SCENE_PARAM_WRITE(shakeDuration);
    SCENE_PARAM_WRITE(fallDuration);
    SCENE_PARAM_WRITE(colorType);
    SCENE_PARAM_WRITE(switchMode);
    SCENE_PARAM_WRITE(actionMode);
    SCENE_PARAM_WRITE(targetScene);
    SCENE_PARAM_WRITE(cinematicPath);
    SCENE_PARAM_WRITE(moveAmount);
    SCENE_PARAM_WRITE(moveSpeed);
    SCENE_PARAM_WRITE(ringWaveSpeed);
    SCENE_PARAM_WRITE(startActive);
    SCENE_PARAM_WRITE(returnOnOff);
    SCENE_PARAM_WRITE(editorPreviewFiring);
    SCENE_PARAM_WRITE(volumeMode);
    SCENE_PARAM_WRITE(volumePayload);
    SCENE_PARAM_WRITE(volumeTriggerOnce);
    SCENE_PARAM_WRITE(volumeTriggerOnExit);
    SCENE_PARAM_WRITE(volumeRearmDelay);
    SCENE_PARAM_WRITE(volumeBlendDuration);
    SCENE_PARAM_WRITE(copyMemoryTypeA);
    SCENE_PARAM_WRITE(copyMemoryTypeB);
    SCENE_PARAM_WRITE(copyMemoryTypeC);
    SCENE_PARAM_WRITE(copyMemoryTypeD);
    SCENE_PARAM_WRITE(copyMemoryActivationRadius);
    SCENE_PARAM_WRITE(copyMemoryEssenceScale);
    SCENE_PARAM_WRITE(copyMemoryHoverAmount);
    SCENE_PARAM_WRITE(copyMemoryGlow);
    SCENE_PARAM_WRITE(copyMemoryUnlimitedDuration);
#undef SCENE_PARAM_WRITE
    return result;
}

// Scene JSON の既存項目だけを param に反映する。
// 共通ステータス管理対象のHPや移動値を上書きせず、個体値は有効範囲へ補正する。
inline void Apply(engine::Object3D& object, const nlohmann::json& source) {
    // paramがJSONオブジェクトでなければ、現在の配置に何も適用しない。
    if (!source.is_object())
        return;
    // 実行時パラメータが未作成であれば、既定値を作って差分の読込先にする。
    if (!object.GetParameters())
        object.ResetParameters();
    auto p = *object.GetParameters();
    const bool managed = GameplayStatusManager::IsManagedCharacter(&object);
    const bool hasHp = !managed && source.contains("hp");
    const bool hasMaxHp = !managed && source.contains("maxHp");

    // 管理対象では共通ステータスを優先し、Scene JSONによる旧個体値の上書きを避ける。
#define SCENE_PARAM_READ(field) if (source.contains(#field)) p.field = source[#field].get<decltype(p.field)>()
#define SCENE_PARAM_READ_UNMANAGED(field) if (!managed) { SCENE_PARAM_READ(field); }
    SCENE_PARAM_READ_UNMANAGED(hp);
    SCENE_PARAM_READ_UNMANAGED(maxHp);
    SCENE_PARAM_READ_UNMANAGED(attackPower);
    SCENE_PARAM_READ_UNMANAGED(speed);
    SCENE_PARAM_READ_UNMANAGED(gravity);
    SCENE_PARAM_READ_UNMANAGED(jumpPower);
    SCENE_PARAM_READ_UNMANAGED(maxFallSpeed);
    SCENE_PARAM_READ_UNMANAGED(morphLimited);
    SCENE_PARAM_READ_UNMANAGED(morphDuration);
    SCENE_PARAM_READ_UNMANAGED(detectionRange);
    SCENE_PARAM_READ(projectileLifetime);
    SCENE_PARAM_READ(enemyType);
    SCENE_PARAM_READ(gimmickType);
    SCENE_PARAM_READ(itemType);
    SCENE_PARAM_READ(healAmount);
    SCENE_PARAM_READ(interval);
    SCENE_PARAM_READ(maxCount);
    SCENE_PARAM_READ(shakeDuration);
    SCENE_PARAM_READ(fallDuration);
    SCENE_PARAM_READ(colorType);
    SCENE_PARAM_READ(switchMode);
    SCENE_PARAM_READ(actionMode);
    SCENE_PARAM_READ(targetScene);
    // 演出指定などの文字列は、異なる型の値が混在していても対象項目を読み飛ばします。
    if (source.contains("cinematicPath") && source["cinematicPath"].is_string())
        p.cinematicPath = source["cinematicPath"].get<std::string>();
    SCENE_PARAM_READ(moveAmount);
    SCENE_PARAM_READ(moveSpeed);
    SCENE_PARAM_READ(ringWaveSpeed);
    SCENE_PARAM_READ(startActive);
    SCENE_PARAM_READ(returnOnOff);
    SCENE_PARAM_READ(editorPreviewFiring);
    SCENE_PARAM_READ(volumeMode);
    // 領域のアクション引数は、文字列として保存されているときだけ適用する。
    if (source.contains("volumePayload") && source["volumePayload"].is_string())
        p.volumePayload = source["volumePayload"].get<std::string>();
    SCENE_PARAM_READ(volumeTriggerOnce);
    SCENE_PARAM_READ(volumeTriggerOnExit);
    SCENE_PARAM_READ(volumeRearmDelay);
    SCENE_PARAM_READ(volumeBlendDuration);
    // コピー記憶のA枠は、文字列として保存されているときだけ適用する。
    if (source.contains("copyMemoryTypeA") && source["copyMemoryTypeA"].is_string())
        p.copyMemoryTypeA = source["copyMemoryTypeA"].get<std::string>();
    // コピー記憶のB枠は、文字列として保存されているときだけ適用する。
    if (source.contains("copyMemoryTypeB") && source["copyMemoryTypeB"].is_string())
        p.copyMemoryTypeB = source["copyMemoryTypeB"].get<std::string>();
    // コピー記憶のC枠は、文字列として保存されているときだけ適用する。
    if (source.contains("copyMemoryTypeC") && source["copyMemoryTypeC"].is_string())
        p.copyMemoryTypeC = source["copyMemoryTypeC"].get<std::string>();
    // コピー記憶のD枠は、文字列として保存されているときだけ適用する。
    if (source.contains("copyMemoryTypeD") && source["copyMemoryTypeD"].is_string())
        p.copyMemoryTypeD = source["copyMemoryTypeD"].get<std::string>();
    SCENE_PARAM_READ(copyMemoryActivationRadius);
    SCENE_PARAM_READ(copyMemoryEssenceScale);
    SCENE_PARAM_READ(copyMemoryHoverAmount);
    SCENE_PARAM_READ(copyMemoryGlow);
    SCENE_PARAM_READ(copyMemoryUnlimitedDuration);
#undef SCENE_PARAM_READ_UNMANAGED
#undef SCENE_PARAM_READ

    // 再起動までの待ち時間が保存されている場合は、0秒以上に補正する。
    if (source.contains("volumeRearmDelay"))
        p.volumeRearmDelay = (std::max)(0.0f, p.volumeRearmDelay);
    // ブレンド時間が保存されている場合は、0秒以上に補正する。
    if (source.contains("volumeBlendDuration"))
        p.volumeBlendDuration = (std::max)(0.0f, p.volumeBlendDuration);
    // コピー記憶の反応半径が保存されている場合は、0.1以上を保証する。
    if (source.contains("copyMemoryActivationRadius"))
        p.copyMemoryActivationRadius = (std::max)(0.1f, p.copyMemoryActivationRadius);
    // リング波速度の値が保存されている場合は、0.25以上4.0以下に収める。
    if (source.contains("ringWaveSpeed"))
        p.ringWaveSpeed = (std::clamp)(p.ringWaveSpeed, 0.25f, 4.0f);
    // 共通ステータスに委ねない個体だけ、個体HPや攻撃力の下限を補正する。
    if (!managed) {
        p.maxHp = (std::max)(p.maxHp, 1.0f);
        // 最大HPのみ指定された配置は、現在HPを最大HPにそろえて開始する。
        if (hasMaxHp && !hasHp)
            p.hp = p.maxHp;
        p.hp = (std::max)(p.hp, 0.0f);
        // 旧データの現在HPが最大HPを超えている場合は、現在HPを維持できるよう最大値を広げる。
        if (p.hp > p.maxHp)
            p.maxHp = p.hp;
        p.attackPower = (std::max)(p.attackPower, 0.0f);
        // 敵として設定されている個体だけ、コピー形態の継続時間を0.1秒以上に補正する。
        if (object.GetClassName() == "Enemy" || !object.GetEnemyType().empty()) {
            p.morphDuration = (std::max)(p.morphDuration, 0.1f);
        }
    }
    // 各項目の読込と補正が済んでから、本体のパラメータをまとめて入れ替えます。
    object.SetParameters(p);
}

} // namespace sceneEntityParameterCodec
