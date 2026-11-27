#include <cstdint>
#include "EditorPropertyRegistry.h"
#include "EditorPropertyRegistry.BuiltIn.h"
#include "Object3D.h"

#include <algorithm>
#include <utility>

using nlohmann::json;
using editorPropertyBuiltIn::CommonFlags;

void EditorPropertyRegistry::RegisterBuiltInRenderAndComponentProperties() {
    // 各登録は安定したProperty ID、表示情報、JSONの読取・書込、適用対象を一組にします。
    // Scene/Prefabの差分もこのIDを参照するため、表示名だけを変える場合もIDは維持します。
    const auto renderableOnly = [](const engine::Object3D& object) {
        return !object.IsCameraObject() && object.GetMeshRenderer() != nullptr;
    };

    // 描画項目はCamera ObjectやRendererのない配置物へ出さず、適用前にJSON型を確認します。
    Register({
        "rendering.model", "モデル", "Rendering", EditorPropertyType::kString, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetModelName()); },
        [](engine::Object3D& object, const json& value) {
            // モデルは素材の参照名で指定するため、文字列だけを反映する。
            if (!value.is_string())
                return false;
            object.SetModel(value.get<std::string>());
            return true;
        },
        renderableOnly,
    });

    // BlendModeはenumの有効範囲に収め、保存データの未知値で不正なPipelineを選ばせません。
    Register({
        "rendering.blendMode", "合成方式", "Rendering", EditorPropertyType::kInteger, CommonFlags(),
        [](const engine::Object3D& object) { return json(static_cast<std::int32_t>(object.GetBlendMode())); },
        [](engine::Object3D& object, const json& value) {
            // 整数以外で合成用パイプラインを選べないよう拒否する。範囲外の整数は続く処理で補正する。
            if (!value.is_number_integer())
                return false;
            const std::int32_t mode = std::clamp(value.get<std::int32_t>(), 0, static_cast<std::int32_t>(BlendMode::kCount) - 1);
            object.SetBlendMode(static_cast<BlendMode>(mode));
            return true;
        },
        renderableOnly,
        { "なし", "通常", "加算", "減算", "乗算", "スクリーン" },
    });
    Register({
        "rendering.materialType", "マテリアルType", "Rendering", EditorPropertyType::kInteger, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetMaterialType()); },
        [](engine::Object3D& object, const json& value) {
            // 材質の切替に使う番号は整数だけを受け付ける。
            if (!value.is_number_integer())
                return false;
            object.SetMaterialType(value.get<std::int32_t>());
            return true;
        },
        renderableOnly,
    });
    Register({
        "rendering.texture", "基本Texture", "Rendering", EditorPropertyType::kString, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetTexturePath()); },
        [](engine::Object3D& object, const json& value) {
            // 基本画像はパスの文字列としてのみ受け付ける。
            if (!value.is_string())
                return false;
            object.SetTexture(value.get<std::string>());
            return true;
        },
        renderableOnly,
    });

    // Tilingは2成分の配列として保存し、必要成分がない差分は反映しません。
    Register({
        "rendering.textureTiling", "Texture Tiling", "Rendering", EditorPropertyType::kVector2, CommonFlags(true),
        [](const engine::Object3D& object) {
            const engine::Vector2 value = object.GetTextureTiling();
            return json::array({ value.x, value.y });
        },
        [](engine::Object3D& object, const json& value) {
            // U と V の倍率を取り出すため、2要素以上の配列を要求する。
            if (!value.is_array() || value.size() < 2)
                return false;
            object.SetTextureTiling({ value[0].get<float>(), value[1].get<float>() });
            return true;
        },
        renderableOnly,
    });
    Register({
        "rendering.autoTextureTiling", "自動Tiling", "Rendering", EditorPropertyType::kBool, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetAutoTextureTiling()); },
        [](engine::Object3D& object, const json& value) {
            // モデルに合わせる自動タイリングの指定は真偽値だけを受け付ける。
            if (!value.is_boolean())
                return false;
            object.SetAutoTextureTiling(value.get<bool>());
            return true;
        },
        renderableOnly,
    });
    Register({
        "rendering.lighting", "Lighting", "Rendering", EditorPropertyType::kBool, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetEnableLighting()); },
        [](engine::Object3D& object, const json& value) {
            // ライティングの有効状態は真偽値の場合だけ更新する。
            if (!value.is_boolean())
                return false;
            object.SetEnableLighting(value.get<bool>());
            return true;
        },
        renderableOnly,
    });
    Register({
        "rendering.environmentMap", "環境Map", "Rendering", EditorPropertyType::kBool, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetEnableEnvMap()); },
        [](engine::Object3D& object, const json& value) {
            // 環境マップを利用するかは真偽値の場合だけ更新する。
            if (!value.is_boolean())
                return false;
            object.SetEnableEnvMap(value.get<bool>());
            return true;
        },
        renderableOnly,
    });
    Register({
        "rendering.environmentIntensity", "環境反射強度", "Rendering", EditorPropertyType::kNumber, CommonFlags(true),
        [](const engine::Object3D& object) { return json(object.GetEnvIntensity()); },
        [](engine::Object3D& object, const json& value) {
            // 環境反射の強度は数値だけを受け、負の強度は0へ補正する。
            if (!value.is_number())
                return false;
            object.SetEnvIntensity((std::max)(value.get<float>(), 0.0f));
            return true;
        },
        renderableOnly,
    });
    Register({
        "rendering.normalMapEnabled", "Normal Map", "Rendering", EditorPropertyType::kBool, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetEnableNormalMap()); },
        [](engine::Object3D& object, const json& value) {
            // 法線マップの使用フラグを変更するには、真偽値が必要。
            if (!value.is_boolean())
                return false;
            object.SetEnableNormalMap(value.get<bool>());
            return true;
        },
        renderableOnly,
    });
    Register({
        "rendering.normalMap", "Normal Texture", "Rendering", EditorPropertyType::kString, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetNormalMapPath()); },
        [](engine::Object3D& object, const json& value) {
            // 法線マップの参照は文字列でなければ更新しない。
            if (!value.is_string())
                return false;
            object.SetNormalMap(value.get<std::string>());
            return true;
        },
        renderableOnly,
    });
    Register({
        "rendering.ormMap", "ORM Texture", "Rendering", EditorPropertyType::kString, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetOrmMapPath()); },
        [](engine::Object3D& object, const json& value) {
            // 粗さなどをまとめた ORM テクスチャの参照は、文字列の場合だけ更新する。
            if (!value.is_string())
                return false;
            object.SetOrmMap(value.get<std::string>());
            return true;
        },
        renderableOnly,
    });

    // Colliderは現在の設定を複製して対象成分だけ差し替えます。
    // 型・中心・大きさ・回転を別々にUndo/Prefab差分へ載せても他の成分を失いません。
    Register({
        "component.collision.type", "Collider Type", "Component", EditorPropertyType::kInteger, CommonFlags(),
        [](const engine::Object3D& object) { return json(static_cast<std::int32_t>(object.GetColliderConfig().type)); },
        [](engine::Object3D& object, const json& value) {
            // 整数で指定された形状だけを受け付け、既存の Collider の他の設定を保つ。
            if (!value.is_number_integer())
                return false;
            engine::Object3D::ColliderConfig config = object.GetColliderConfig();
            config.type = static_cast<engine::ColliderType>(std::clamp(
                value.get<std::int32_t>(), static_cast<std::int32_t>(engine::ColliderType::kNone), static_cast<std::int32_t>(engine::ColliderType::kTerrain)));
            object.SetColliderConfig(config);
            return true;
        },
        {},
        { "なし", "Sphere", "AABB", "OBB", "Cylinder", "Ring", "Terrain" },
    });
    Register({
        "component.collision.center", "Collider中心", "Component", EditorPropertyType::kVector3, CommonFlags(true),
        [](const engine::Object3D& object) {
            const engine::Vector3& value = object.GetColliderConfig().center;
            return json::array({ value.x, value.y, value.z });
        },
        [](engine::Object3D& object, const json& value) {
            // 中心の補正を XYZ の3成分で指定できる配列か確認する。
            if (!value.is_array() || value.size() < 3)
                return false;
            engine::Object3D::ColliderConfig config = object.GetColliderConfig();
            config.center = { value[0].get<float>(), value[1].get<float>(), value[2].get<float>() };
            object.SetColliderConfig(config);
            return true;
        },
    });
    Register({
        "component.collision.size", "Colliderサイズ", "Component", EditorPropertyType::kVector3, CommonFlags(true),
        [](const engine::Object3D& object) {
            const engine::Vector3& value = object.GetColliderConfig().size;
            return json::array({ value.x, value.y, value.z });
        },
        [](engine::Object3D& object, const json& value) {
            // 形状ごとの寸法を XYZ から読むため、配列に3成分以上が必要。
            if (!value.is_array() || value.size() < 3)
                return false;
            engine::Object3D::ColliderConfig config = object.GetColliderConfig();
            config.size = { value[0].get<float>(), value[1].get<float>(), value[2].get<float>() };
            object.SetColliderConfig(config);
            return true;
        },
    });
    Register({
        "component.collision.rotation", "Collider回転", "Component", EditorPropertyType::kVector3, CommonFlags(true),
        [](const engine::Object3D& object) {
            const engine::Vector3& value = object.GetColliderConfig().rotation;
            return json::array({ value.x, value.y, value.z });
        },
        [](engine::Object3D& object, const json& value) {
            // Collider の回転を三軸から読むため、3要素以上の配列を要求する。
            if (!value.is_array() || value.size() < 3)
                return false;
            engine::Object3D::ColliderConfig config = object.GetColliderConfig();
            config.rotation = { value[0].get<float>(), value[1].get<float>(), value[2].get<float>() };
            object.SetColliderConfig(config);
            return true;
        },
    });
    Register({
        "component.collision.static", "Static Collision", "Component", EditorPropertyType::kBool, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.IsStatic()); },
        [](engine::Object3D& object, const json& value) {
            // 静的な衝突物へ切り替える入力は、真偽値だけ適用する。
            if (!value.is_boolean())
                return false;
            object.SetStatic(value.get<bool>());
            return true;
        },
    });

    // CPUとGPUのParticle名はそれぞれ別の保存先を使います。
    Register({
        "component.particle.cpu", "CPU Particle", "Component", EditorPropertyType::kString, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetParticleName()); },
        [](engine::Object3D& object, const json& value) {
            // CPU 粒子の参照名を文字列で指定した場合に設定する。
            if (!value.is_string())
                return false;
            object.SetParticleName(value.get<std::string>());
            return true;
        },
    });
    Register({
        "component.particle.gpu", "GPU Particle", "Component", EditorPropertyType::kString, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetGpuParticleName()); },
        [](engine::Object3D& object, const json& value) {
            // GPU 粒子枠でも参照名は文字列で指定する。
            if (!value.is_string())
                return false;
            object.SetGpuParticleName(value.get<std::string>());
            return true;
        },
    });
    Register({
        "component.meshEffect.primary", "Mesh Effect 1", "Component", EditorPropertyType::kString, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetMeshEffect1Name()); },
        [](engine::Object3D& object, const json& value) {
            // 第1メッシュエフェクトの参照先は、文字列を受けた場合だけ更新する。
            if (!value.is_string())
                return false;
            object.SetMeshEffect1Name(value.get<std::string>());
            return true;
        },
    });
    Register({
        "component.meshEffect.secondary", "Mesh Effect 2", "Component", EditorPropertyType::kString, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetMeshEffect2Name()); },
        [](engine::Object3D& object, const json& value) {
            // 第2メッシュエフェクトは、別の文字列入力として第1枠から独立に更新する。
            if (!value.is_string())
                return false;
            object.SetMeshEffect2Name(value.get<std::string>());
            return true;
        },
    });

    // アニメーション名・Controller・記録パスはそれぞれ別の実行系が消費します。
    // ControllerだけはSetAnimatorControllerの失敗を呼び出し元へ返します。
    Register({
        "component.animation.name", "Animation", "Component", EditorPropertyType::kString, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetAnimationName()); },
        [](engine::Object3D& object, const json& value) {
            // モデル上のアニメーションは文字列の名前で指定された場合だけ切り替える。
            if (!value.is_string())
                return false;
            object.SetAnimationName(value.get<std::string>());
            return true;
        },
    });
    Register({
        "component.animation.loop", "Animation Loop", "Component", EditorPropertyType::kBool, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.IsAnimationLoop()); },
        [](engine::Object3D& object, const json& value) {
            // 繰り返し再生するかどうかは真偽値だけを受け付ける。
            if (!value.is_boolean())
                return false;
            object.SetAnimationLoop(value.get<bool>());
            return true;
        },
    });
    Register({
        "component.animation.controller", "Animator Controller", "Component", EditorPropertyType::kString, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetAnimatorControllerPath()); },
        [](engine::Object3D& object, const json& value) {
            // コントローラーのパスが文字列の場合に読込を行い、成功可否をそのまま返す。
            if (!value.is_string())
                return false;
            return object.SetAnimatorController(value.get<std::string>());
        },
    });
    Register({
        "component.path.name", "Ghost Path", "Component", EditorPropertyType::kString, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetRecordPathName()); },
        [](engine::Object3D& object, const json& value) {
            // 軌道の参照先を文字列で指定した場合だけ、記録パスを更新する。
            if (!value.is_string())
                return false;
            object.SetRecordPathName(value.get<std::string>());
            return true;
        },
    });
    Register({
        "component.path.loop", "Path Loop", "Component", EditorPropertyType::kBool, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.IsRecordLoop()); },
        [](engine::Object3D& object, const json& value) {
            // 経路を繰り返す指定は真偽値の場合だけ反映する。
            if (!value.is_boolean())
                return false;
            object.SetRecordLoop(value.get<bool>());
            return true;
        },
    });
    Register({
        "component.path.relative", "Path相対座標", "Component", EditorPropertyType::kBool, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.IsRecordRelative()); },
        [](engine::Object3D& object, const json& value) {
            // 相対位置で軌道を再生するかは、真偽値の場合だけ反映する。
            if (!value.is_boolean())
                return false;
            object.SetRecordRelative(value.get<bool>());
            return true;
        },
    });

    // 再生条件もUndoとPrefab差分の対象に含める。
    Register({
        "component.path.phaseOffset", "Path開始タイミング", "Component", EditorPropertyType::kNumber, CommonFlags(),
        [](const engine::Object3D& object) {
            const auto* path = object.GetPathMoverComponent();
            return json(path ? path->GetPhaseOffset() : 0.0f);
        },
        [](engine::Object3D& object, const json& value) {
            // 開始位置が数値でなければ、PathMover を作成・変更しない。
            if (!value.is_number()) return false;
            // 秒ではなく記録全体に対する開始位置の割合。記録終端からそのまま始めないよう上限を 0.99 にする。
            object.EnsurePathMoverComponent()->SetPhaseOffset(std::clamp(value.get<float>(), 0.0f, 0.99f));
            return true;
        },
    });

    Register({
        "component.path.playbackMode", "Path再生条件", "Component", EditorPropertyType::kString, CommonFlags(),
        [](const engine::Object3D& object) {
            const auto* path = object.GetPathMoverComponent();
            // 未装着でも旧データと同じ常時再生を表示する。表示しただけでは Component を追加しない。
            return json(path ? path->GetPlaybackModeName() : "always");
        },
        [](engine::Object3D& object, const json& value) {
            // 再生条件を示す名前が文字列の場合だけ PathMover を準備して切り替える。
            if (!value.is_string()) return false;
            object.EnsurePathMoverComponent()->SetPlaybackModeName(value.get<std::string>());
            return true;
        },
    });

    // Event ID はこの Object が受ける番号、Target ID はイベントの通知先の番号。
    Register({
        "component.link.eventId", "Event ID", "Component", EditorPropertyType::kInteger, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetEventId()); },
        [](engine::Object3D& object, const json& value) {
            // 受信側の ID を整数で指定した場合のみ更新する。
            if (!value.is_number_integer())
                return false;
            object.SetEventId(value.get<std::int32_t>());
            return true;
        },
    });
    Register({
        "component.link.targetId", "Target ID", "Component", EditorPropertyType::kInteger, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetTargetId()); },
        [](engine::Object3D& object, const json& value) {
            // 送信先の ID を整数で指定した場合のみ更新する。
            if (!value.is_number_integer())
                return false;
            object.SetTargetId(value.get<std::int32_t>());
            return true;
        },
    });

    // NavAgentは未装着なら表示に既定値を使い、最初の編集時にComponentを生成します。
    // 参照だけのためにSceneへ新しいComponentを増やしません。
    Register({
        "component.nav.enabled", "経路探索を有効化", "Component", EditorPropertyType::kBool, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetNavAgentComponent() && object.GetNavAgentComponent()->IsEnabled()); },
        [](engine::Object3D& object, const json& value) {
            // 真偽値で有効状態を指定した場合だけ NavAgent を用意し、フラグを切り替える。
            if (!value.is_boolean())
                return false;
            object.EnsureNavAgentComponent()->SetEnabled(value.get<bool>());
            return true;
        },
    });
    Register({
        "component.nav.cellSize", "セルサイズ", "Component", EditorPropertyType::kNumber, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetNavAgentComponent() ? object.GetNavAgentComponent()->GetCellSize() : 1.0f); },
        [](engine::Object3D& object, const json& value) {
            // セルの幅に使える数値の場合だけ、探索グリッドの設定を更新する。
            if (!value.is_number())
                return false;
            object.EnsureNavAgentComponent()->SetCellSize(value.get<float>());
            return true;
        },
    });
    Register({
        "component.nav.agentRadius", "Agent半径", "Component", EditorPropertyType::kNumber, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetNavAgentComponent() ? object.GetNavAgentComponent()->GetAgentRadius() : 0.55f); },
        [](engine::Object3D& object, const json& value) {
            // Agent の半径に変換できる数値の場合だけ、横方向の判定幅を更新する。
            if (!value.is_number())
                return false;
            object.EnsureNavAgentComponent()->SetAgentRadius(value.get<float>());
            return true;
        },
    });
    Register({
        "component.nav.agentHeight", "Agent高さ", "Component", EditorPropertyType::kNumber, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetNavAgentComponent() ? object.GetNavAgentComponent()->GetAgentHeight() : 1.5f); },
        [](engine::Object3D& object, const json& value) {
            // Agent の高さを数値で指定した場合だけ、縦方向の寸法を更新する。
            if (!value.is_number())
                return false;
            object.EnsureNavAgentComponent()->SetAgentHeight(value.get<float>());
            return true;
        },
    });
    Register({
        "component.nav.searchPadding", "探索余白", "Component", EditorPropertyType::kNumber, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetNavAgentComponent() ? object.GetNavAgentComponent()->GetSearchPadding() : 5.0f); },
        [](engine::Object3D& object, const json& value) {
            // 探索範囲の余白が数値の場合だけ、ナビゲーションの探索設定を更新する。
            if (!value.is_number())
                return false;
            object.EnsureNavAgentComponent()->SetSearchPadding(value.get<float>());
            return true;
        },
    });
    Register({
        "component.nav.repathInterval", "再探索間隔", "Component", EditorPropertyType::kNumber, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetNavAgentComponent() ? object.GetNavAgentComponent()->GetRepathInterval() : 0.35f); },
        [](engine::Object3D& object, const json& value) {
            // 再探索の間隔が数値の場合だけ、NavAgent に待機秒数を渡す。
            if (!value.is_number())
                return false;
            object.EnsureNavAgentComponent()->SetRepathInterval(value.get<float>());
            return true;
        },
    });
    Register({
        "component.nav.stoppingDistance", "停止距離", "Component", EditorPropertyType::kNumber, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetNavAgentComponent() ? object.GetNavAgentComponent()->GetStoppingDistance() : 0.25f); },
        [](engine::Object3D& object, const json& value) {
            // 停止距離が数値の場合だけ、目標に接近したとみなす距離を更新する。
            if (!value.is_number())
                return false;
            object.EnsureNavAgentComponent()->SetStoppingDistance(value.get<float>());
            return true;
        },
    });
    Register({
        "component.nav.obstacleMask", "障害物Mask", "Component", EditorPropertyType::kInteger, CommonFlags(),
        [](const engine::Object3D& object) { return json(object.GetNavAgentComponent() ? object.GetNavAgentComponent()->GetObstacleMask() : engine::collisionCategories::Get("ground")); },
        [](engine::Object3D& object, const json& value) {
            // ビットマスクには整数が必要なため、実数や文字列を拒否する。
            if (!value.is_number_integer() && !value.is_number_unsigned())
                return false;
            object.EnsureNavAgentComponent()->SetObstacleMask(value.get<std::uint32_t>());
            return true;
        },
    });
    Register({
        "component.nav.allowDiagonal", "斜め移動", "Component", EditorPropertyType::kBool, CommonFlags(),
        [](const engine::Object3D& object) { return json(!object.GetNavAgentComponent() || object.GetNavAgentComponent()->AllowsDiagonal()); },
        [](engine::Object3D& object, const json& value) {
            // 斜め方向へ移動できるかは、真偽値の場合だけ設定する。
            if (!value.is_boolean())
                return false;
            object.EnsureNavAgentComponent()->SetAllowDiagonal(value.get<bool>());
            return true;
        },
    });
}
