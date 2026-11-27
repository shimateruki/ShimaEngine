#pragma once
#ifdef USE_IMGUI
#include "engine/editor/core/IEditable.h"
#include "engine/utility/math/Math.h"
#include <optional>
#include <unordered_map>
#include <vector>

class BaseScene;
class SceneManager;
class DebugEditor;

// 担当：シーン別のデバッグ移動地点をEditor設定へ保存し、選んだ地点を再生側へ渡す。
// 関連：SceneManager（現在Scene）、DebugEditor（選択Object）、Binding（ゲーム側の移動処理）。
// 主な入口：Configure / Initialize / DrawImGui / Finalize
class DebugTeleportEditor final : public IEditable {
public:
    struct Point {
        std::string name;
        engine::Vector3 position{}; // プレイヤーの配置位置。ワールド座標。
        float yawRadians = 0.0f; // 移動後の水平向き。
    };

    struct Binding {
        std::string (*sceneKey)(const BaseScene&) = nullptr;
        std::vector<Point> (*defaults)(BaseScene&) = nullptr;
        std::optional<Point> (*playerPoint)(BaseScene&) = nullptr;
        bool (*request)(BaseScene&, const Point&) = nullptr;
    };

    static DebugTeleportEditor& GetInstance();
    void Configure(Binding binding);
    void Initialize(SceneManager* scenes, DebugEditor* editor);
    void Finalize();
    void DrawImGui() override;
    std::string GetName() override { return "デバッグ移動エディター"; }

private:
    std::vector<Point>& GetPoints(const std::string& key, BaseScene& scene);
    bool SavePoints(const std::string& key, const std::vector<Point>& points);

    Binding binding_{};
    SceneManager* scenes_ = nullptr;
    DebugEditor* editor_ = nullptr;
    std::unordered_map<std::string, std::vector<Point>> pointsByScene_;
    std::string activeKey_;
    std::string status_;
};

#endif
