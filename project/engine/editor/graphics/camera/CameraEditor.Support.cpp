#include "engine/editor/graphics/camera/CameraEditor.Internal.h"

using namespace cameraEditorImplementation;

namespace cameraEditorImplementation {
    // ゲームが実行中で、編集カメラのガイドを隠すべき場合にtrueを返します。
    bool ShouldHideGameCameraGuideDuringPlay() {
        SceneManager* sceneManager = SceneManager::GetInstance();
        return sceneManager && sceneManager->IsPlaying();
    }

    // 現在のシーンを借用して返します。シーン管理が無ければnullptrを返します。
    BaseScene* GetCurrentScene() {
        SceneManager* sceneManager = SceneManager::GetInstance();
        return sceneManager ? sceneManager->GetCurrentScene() : nullptr;
    }

    bool IsObjectInCurrentScene(const engine::Object3D* target) {
        // 生ポインターの参照先がシーン切替で失効していないか、現所有一覧と照合する。
        BaseScene* scene = GetCurrentScene();

        // 現在シーンか参照先が無ければ、所有一覧で照合できない。
        if (!scene || !target) {
            return false;
        }

        return std::any_of(scene->GetObjects().begin(), scene->GetObjects().end(), [target](const std::unique_ptr<engine::Object3D>& object) {
            return object.get() == target;
        });
    }

    // 度で受け取った角度をラジアンへ換算します。
    float DegToRad(float degrees) {
        return degrees * kPi / 180.0f;
    }

    // ラジアンで受け取った角度を度へ換算します。
    float RadToDeg(float radians) {
        return radians * 180.0f / kPi;
    }

    engine::Matrix4x4 MakeLineBoxMatrix(const engine::Vector3& start, const engine::Vector3& end, float thickness) {
        // 始点と終点を結ぶ細い箱を作るため、長さ・向き・中心位置から変換行列を組み立てる。
        engine::Math math;
        engine::Vector3 diff = end - start;
        float length = math.Length(diff);

        // 端点がほぼ重なる場合も、描画する箱の長さは0.001以上にする。
        if (length < 0.001f) {
            length = 0.001f;
        }

        engine::Vector3 center = (start + end) * 0.5f;
        // 編集用プリミティブの長手方向はローカル +Z。水平角と上下角を使い、始点から終点へ向ける。
        float yaw = std::atan2(diff.x, diff.z);
        float horizontalLength = std::sqrt(diff.x * diff.x + diff.z * diff.z);
        float pitch = std::atan2(-diff.y, horizontalLength);

        engine::Matrix4x4 scale = math.MakeScaleMatrix({ thickness, thickness, length });
        engine::Matrix4x4 rotate = math.Multiply(math.MakeRotateXMatrix(pitch), math.MakeRotateYMatrix(yaw));
        engine::Matrix4x4 translate = math.MakeTranslateMatrix(center);
        return math.Multiply(math.Multiply(scale, rotate), translate);
    }

    engine::Vector3 CalculateForward(const engine::Vector3& rotation) {
        // XのピッチとYのヨーから、正のピッチで上を向く前方方向を求める。
        float x = std::sin(rotation.y) * std::cos(rotation.x);
        float y = std::sin(rotation.x);
        float z = std::cos(rotation.y) * std::cos(rotation.x);
        return { x, y, z };
    }

    // 編集カメラは正の Pitch で視線が下を向くため、Y 成分に負の sin を使う。
    engine::Vector3 CalculateEditorForward(const engine::Vector3& rotation) {
        float x = std::sin(rotation.y) * std::cos(rotation.x);
        float y = -std::sin(rotation.x);
        float z = std::cos(rotation.y) * std::cos(rotation.x);
        return { x, y, z };
    }

    // カメラ基底の軸を直交させる計算に使う外積。引数の順番で向きが決まる。
    engine::Vector3 Cross(const engine::Vector3& a, const engine::Vector3& b) {
        return {
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x
        };
    }

    float Length(const engine::Vector3& v) {
        return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    }

    // 長さを使って方向を単位化します。
    engine::Vector3 NormalizeSafe(const engine::Vector3& v, const engine::Vector3& fallback) {
        float length = Length(v);

        // 長さが十分でなければ、呼び出し側が指定した代替方向を返す。
        if (length < 0.0001f) {
            return fallback;
        }

        return { v.x / length, v.y / length, v.z / length };
    }

    engine::Vector3 MakeRotationFromForward(const engine::Vector3& forward) {
        engine::Vector3 normalized = NormalizeSafe(forward, { 0.0f, 0.0f, 1.0f });
        // 編集カメラの Pitch 符号へ戻す。丸め誤差で asin の定義域を越えないよう Y 成分を制限する。
        float pitch = std::asin(std::clamp(-normalized.y, -1.0f, 1.0f));
        float yaw = std::atan2(normalized.x, normalized.z);
        return { pitch, yaw, 0.0f };
    }

    engine::Vector3 GetObjectWorldForward(const engine::Object3D* object) {
        // 参照先が無ければ、ワールドの正のZを既定の前方とする。
        if (!object) {
            return { 0.0f, 0.0f, 1.0f };
        }

        // ローカルの +Z を回転・拡縮だけでワールド方向へ変換し、距離計算に使える単位方向へ直す。
        engine::Vector3 forward = engine::Math::TransformNormal({ 0.0f, 0.0f, 1.0f }, object->GetWorldMatrix());
        return NormalizeSafe(forward, { 0.0f, 0.0f, 1.0f });
    }

    // フォーカスの対象となる境界の全成分が有限で、各軸の最小・最大が正順であるか確かめます。
    bool IsValidFocusBounds(const engine::Aabb& bounds) {
        return std::isfinite(bounds.min.x) && std::isfinite(bounds.min.y) && std::isfinite(bounds.min.z) &&
            std::isfinite(bounds.max.x) && std::isfinite(bounds.max.y) && std::isfinite(bounds.max.z) &&
            bounds.max.x >= bounds.min.x && bounds.max.y >= bounds.min.y && bounds.max.z >= bounds.min.z;
    }
}
