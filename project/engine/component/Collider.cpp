#include <cstdint>
#include "Collider.h"
#include "ConvexCollision.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace engine {

namespace {
// XZ断面の分離軸。輪郭と相手の断面の両方の辺を調べ、角の空白も除外する。
bool OverlapOnAxes(const std::vector<engine::Vector3>& a, const std::vector<engine::Vector3>& b,
                   float& minimumDepth, engine::Vector3& outwardNormal) {
    const auto testEdges = [&](const std::vector<engine::Vector3>& polygon) {
        for (size_t i = 0; i < polygon.size(); ++i) {
            const engine::Vector3 edge = polygon[(i + 1) % polygon.size()] - polygon[i];
            const float length = std::sqrt(edge.x * edge.x + edge.z * edge.z);
            // 長さがほぼゼロの辺からは安定した法線を作れないため、分離軸に使わない。
            if (length < 0.00001f)
                continue;
            const engine::Vector3 axis = { -edge.z / length, 0.0f, edge.x / length };
            // 両方の輪郭をこの軸へ射影し、各区間の最小と最大を得る。
            float minA = std::numeric_limits<float>::max(), maxA = -minA;
            float minB = std::numeric_limits<float>::max(), maxB = -minB;

            for (const auto& point : a) {
                const float value = point.x * axis.x + point.z * axis.z;
                minA = (std::min)(minA, value);
                maxA = (std::max)(maxA, value);
            }

            for (const auto& point : b) {
                const float value = point.x * axis.x + point.z * axis.z;
                minB = (std::min)(minB, value);
                maxB = (std::max)(maxB, value);
            }

            const float overlap = (std::min)(maxA, maxB) - (std::max)(minA, minB);
            // 射影した区間の重なりが正でなければ、断面の間に分離する方向が見つかった。
            if (overlap <= 0.0f)
                return false;
            // より浅く押し出せる軸が見つかれば、相手側を向く符号で記録する。
            if (overlap < minimumDepth) {
                minimumDepth = overlap;
                const float middleA = (minA + maxA) * 0.5f;
                const float middleB = (minB + maxB) * 0.5f;
                outwardNormal = axis * (middleB >= middleA ? 1.0f : -1.0f);
            }
        }

        return true;
    };

    // 片方の輪郭だけでは分離軸を見落とすため、両方の辺を候補にする。
    return testEdges(a) && testEdges(b);
}

engine::Vector3 CrossSurface(const engine::Vector3& a, const engine::Vector3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float DotSurface(const engine::Vector3& a, const engine::Vector3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

engine::Vector3 UpwardSurfaceNormal(const engine::Vector3& a, const engine::Vector3& b,
                                    const engine::Vector3& c) {
    engine::Vector3 normal = CrossSurface(b - a, c - a);
    const float length = std::sqrt(DotSurface(normal, normal));

    // 三角形から安定した長さの法線を得られない場合は、上向きの単位軸を返す。
    if (length <= 0.000001f)
        return {0.0f, 1.0f, 0.0f};
    // 接地に使うため、正規化した法線はYの成分が下向きなら反転する。
    normal = normal * (1.0f / length);
    return normal.y < 0.0f ? normal * -1.0f : normal;
}
} // namespace

// コライダーの位置と姿勢を決める、所有側のTransformを借用する。
Collider::Collider(Transform* ownerTransform) {
    transform_ = ownerTransform;
}

void Collider::SetModelSurface(std::vector<ModelSurfaceTriangle> triangles) {
    // 表面三角形を差し替え、体積判定を使う設定なら凸包と外接範囲も作り直す。
    modelSurface_ = std::move(triangles);
    modelHull_.clear();
    hullCacheValid_=false;
    // 体積判定を使わないか三角形が空の場合は、凸包構築を省く。
    if (!config_.useModelVolume || modelSurface_.empty())
        return;
    std::vector<engine::Vector3> points;

    for (const auto& t : modelSurface_) {
        points.push_back(t.a);
        points.push_back(t.b);
        points.push_back(t.c);
    }
    modelHull_ = convexCollision::Build(points);
    // 頂点から有効な凸包を作れなければ、既存の外接設定は上書きしない。
    if (modelHull_.empty())
        return;
    engine::Vector3 minimum=points.front(), maximum=minimum;

    for (const auto& p:points) {
        minimum.x=(std::min)(minimum.x,p.x);
        minimum.y=(std::min)(minimum.y,p.y);
        minimum.z=(std::min)(minimum.z,p.z);
        maximum.x=(std::max)(maximum.x,p.x);
        maximum.y=(std::max)(maximum.y,p.y);
        maximum.z=(std::max)(maximum.z,p.z);
    }

    // 頂点の最小と最大からローカルの中心と半寸法を保存し、自動の箱に揃える。
    config_.center=(minimum+maximum)*0.5f;
    config_.size=(maximum-minimum)*0.5f;
    config_.rotation={};
    config_.footprint.clear();
    config_.type=engine::ColliderType::kObb;
}

void Collider::UpdateWorldHullCache() const {
    // 座標変換済みの凸包・頂点・方向を保持し、判定行列が一致する間は再利用する。
    const auto world=GetCollisionWorldMatrix();
    bool unchanged=hullCacheValid_;

    for(std::int32_t row=0;row<4;++row)for(std::int32_t col=0;col<4;++col)
        unchanged=unchanged && world.m[row][col]==hullCacheMatrix_.m[row][col];
    // 有効なキャッシュの行列が今回と全要素一致していれば、凸包を再計算しない。
    if(unchanged)
        return;
    hullCacheMatrix_=world;
    hullCacheValid_=true;
    worldHull_=modelHull_;
    hullPoints_.clear();
    hullNormals_.clear();
    hullEdges_.clear();
    const engine::Vector3 x{world.m[0][0],world.m[0][1],world.m[0][2]};
    const engine::Vector3 y{world.m[1][0],world.m[1][1],world.m[1][2]};
    const engine::Vector3 z{world.m[2][0],world.m[2][1],world.m[2][2]};
    const bool mirrored=convexCollision::Dot(x,convexCollision::Cross(y,z))<0;
    // 分離軸に使える単位方向だけを、同じ軸は一つにまとめて保持する。
    const auto addDirection=[](std::vector<engine::Vector3>& list,engine::Vector3 direction){
        const float length=std::sqrt(convexCollision::Dot(direction,direction));
        // 長さがほぼゼロの候補は、正規化できないため除外する。
        if(length<1e-6f)
            return;
        direction=direction/length;
        for(const auto& n:list)
            // 既に同軸の方向があれば、正負を問わず追加しない。
            if(std::abs(convexCollision::Dot(n,direction))>0.99999f)
                return;
        list.push_back(direction);
    };

    // 担当：凸包の共有辺に接する面数と、折れ目の有無を集計する。
    struct EdgeFaces {
        engine::Vector3 firstNormal{};
        std::int32_t count = 0;
        bool crease = false;
    };

    std::map<std::pair<size_t,size_t>, EdgeFaces> sharedEdges;

    for (auto& face : worldHull_) {
        for (auto& point : face)
            point = engine::Math::Transform(point, world);
        // 反転した行列では頂点の巡回順も逆になるため、面の外向き法線を維持するよう入れ替える。
        if (mirrored)
            std::swap(face[1], face[2]);
        const auto normal = convexCollision::Normal(face);
        addDirection(hullNormals_, normal);
        size_t indices[3]{};

        for (std::int32_t i = 0; i < 3; ++i) {
            auto it = std::find_if(hullPoints_.begin(), hullPoints_.end(), [&](const auto& p) {
                return convexCollision::Dot(face[i]-p,face[i]-p)<1e-10f;
            });
            indices[i] = static_cast<size_t>(it-hullPoints_.begin());
            // 許容範囲内の同じ点が登録済みでなければ、新しい頂点として記録する。
            if (it == hullPoints_.end())
                hullPoints_.push_back(face[i]);
        }

        // 同じ両端を持つ辺を頂点番号でまとめ、隣の面の向きが変わるか調べる。
        for (std::int32_t i = 0; i < 3; ++i) {
            auto& edge = sharedEdges[std::minmax(indices[i],indices[(i+1)%3])];
            // 辺に最初に触れた面の法線を、後から来る面との比較に残す。
            if (edge.count == 0)
                edge.firstNormal = normal;
            // 次の面の法線が最初の面と異なれば、辺を折れ目として記録する。
            else if (convexCollision::Dot(edge.firstNormal,normal)<0.999999f) edge.crease = true;
            ++edge.count;
        }
    }

    // 同一平面内の三角形分割線は立体の稜線ではない。SATの辺対から除外し、
    // 広い面を細かく分割したモデルでも計算量を描画三角形数に比例させない。
    for (const auto& [indices, edge] : sharedEdges) {
        // 折れ目があるか、面がちょうど二枚で共有していない辺だけを分離軸に残す。
        if (edge.crease || edge.count != 2)
            addDirection(hullEdges_,hullPoints_[indices.second]-hullPoints_[indices.first]);
    }

}

std::vector<convexCollision::Face> Collider::GetWorldHull() const {
    // エディター側にはコピーを渡し、キャッシュを変更させない。
    UpdateWorldHullCache();
    return worldHull_;
}

bool Collider::RaycastModelVolume(const engine::Vector3& start, const engine::Vector3& direction,
                                 float maxDistance, float& distance, engine::Vector3& normal) const {
    // カメラや接地レイは上面・側面・底面で同じ凸包を参照する。
    // モデルの凸包を使える状態でなければ、交点は返さない。
    if(!HasModelVolume())
        return false;
    UpdateWorldHullCache();
    return convexCollision::Ray(worldHull_,start,direction,maxDistance,distance,normal);
}

engine::CollisionInfo Collider::CheckModelVolume(const Collider* other) const {
    // 戻す法線は通常のCheckCollisionと同様、このColliderを押し出す方向。
    engine::CollisionInfo result;
    // 外接箱が交差しなければ、面と辺を使う凸包の判定を省く。
    if (!engine::CheckAabbCollision(GetAabb(),other->GetAabb()).isColliding)
        return result;
    UpdateWorldHullCache();
    const auto& hull=worldHull_;

    // 相手が球なら、中心と半径から凸包との接触を評価する。
    if(other->GetType()==engine::ColliderType::kSphere){
        result.isColliding=convexCollision::Sphere(hull,other->GetSphereCenter(),other->GetRadius(),result.penetration,result.normal);
    // 球以外は相手側が凸包を持つかどうかで使う判定を分ける。
    } else {
        // 相手に凸包が無い場合は、箱として判定する。
        if(!other->HasModelVolume()){
            engine::Obb box=other->GetObb();

            // 軸に平行な箱は、回転軸を含む箱の表現へ変換して共通の判定へ渡す。
            if(other->GetType()==engine::ColliderType::kAabb){
                const engine::Aabb bounds=other->GetAabb();
                box.center=(bounds.min+bounds.max)*0.5f;
                box.size=(bounds.max-bounds.min)*0.5f;
                box.orientations[0]={1,0,0};
                box.orientations[1]={0,1,0};
                box.orientations[2]={0,0,1};
            }

            result.isColliding=convexCollision::Box(hullPoints_,hullNormals_,hullEdges_,box.center,
                                                     box.orientations,box.size,result.penetration,result.normal);
        // 相手もモデルの体積形状を持つ場合は、二つの凸包を使う。
        } else {
            // 相手の形状も行列変更時だけ更新し、接触のたびに凸包をコピーしない。
            other->UpdateWorldHullCache();
            result.isColliding=convexCollision::Polyhedra(hullPoints_,hullNormals_,hullEdges_,
                other->hullPoints_,other->hullNormals_,other->hullEdges_,result.penetration,result.normal);
        }
    }

    // 凸包側が押し出される向きとして、このコライダー基準へ法線を反転する。
    result.normal=result.normal*-1.0f;
    return result;
}

engine::Vector3 Collider::GetCollisionScale() const {
    // 見た目だけを伸縮させる物は、判定用の固定スケールを優先する。
    // 所有側のTransformが無ければ、既定の倍率1を返す。
    if (!transform_) {
        return { 1.0f, 1.0f, 1.0f };
    }

    return useScaleOverride_ ? scaleOverride_ : transform_->scale;
}

engine::Matrix4x4 Collider::GetCollisionWorldMatrix() const {
    // スケール上書き時も親の座標変換は維持し、表示行列とは別に判定行列を組む。
    // 上書きを使わない場合は元のワールド行列を使う。Transformが無ければ単位行列を返す。
    if (!transform_ || !useScaleOverride_) {
        return transform_ ? transform_->matWorld : engine::Math::MakeIdentity4x4();
    }

    engine::Math math;
    engine::Quaternion rotation = transform_->quaternion;

    // Eulerが姿勢の正本の場合は、回転行列に使うクォータニオンを計算し直す。
    if (!transform_->isQuaternionMaster) {
        rotation = math.EulerToQuaternion(transform_->rotate);
    }

    const engine::Matrix4x4 local = math.Multiply(
        math.Multiply(math.MakeScaleMatrix(scaleOverride_), math.MakeRotateQuaternionMatrix(rotation)),
        math.MakeTranslateMatrix(transform_->translate));
    return transform_->parent ? math.Multiply(local, transform_->parent->matWorld) : local;
}

// 球の半径はローカルのX寸法と、判定用スケールの絶対値の最大から求める。
float Collider::GetRadius() const {
    // 所有側のTransformが無い場合は、ローカル半径をそのまま返す。
    if (!transform_)
        return config_.size.x;

    // 最大倍率を用い、非一様な拡縮でも衝突形状は球として扱う。
    const engine::Vector3 collisionScale = GetCollisionScale();
    float maxScale = (std::max)({
        std::abs(collisionScale.x),
        std::abs(collisionScale.y),
        std::abs(collisionScale.z)
        });
    return config_.size.x * maxScale;
}

engine::Vector3 Collider::GetSphereCenter() const {
    // ローカル中心を判定用行列で変換し、見た目の拡縮と判定のずれを防ぐ。
    if (!transform_) {
        return config_.center;
    }

    engine::Math math;
    engine::Matrix4x4 matTrans = math.MakeTranslateMatrix(config_.center);
    engine::Matrix4x4 matFinal = math.Multiply(matTrans, GetCollisionWorldMatrix());
    return { matFinal.m[3][0], matFinal.m[3][1], matFinal.m[3][2] };
}

engine::Obb Collider::GetObb() const {
    engine::Obb obb;
    // 所有物のTransformが無い場合は、ワールドの箱に変換できない。
    if (!transform_)
        return obb;

    // 円柱などの判定へ箱を渡す場合も、AABBは持ち主の傾きに追従させません。
    // GetAABBと同じ範囲を使い、広域判定と接触時の形状を一致させます。
    if (config_.type == engine::ColliderType::kAabb) {
        const engine::Aabb bounds = GetAabb();
        obb.center = (bounds.min + bounds.max) * 0.5f;
        obb.size = (bounds.max - bounds.min) * 0.5f;
        obb.orientations[0] = {1.0f, 0.0f, 0.0f};
        obb.orientations[1] = {0.0f, 1.0f, 0.0f};
        obb.orientations[2] = {0.0f, 0.0f, 1.0f};
        return obb;
    }

    engine::Math math;

    // コライダー固有の中心と回転を、所有側の行列へ掛ける前に作る。
    engine::Matrix4x4 matRotX = math.MakeRotateXMatrix(config_.rotation.x);
    engine::Matrix4x4 matRotY = math.MakeRotateYMatrix(config_.rotation.y);
    engine::Matrix4x4 matRotZ = math.MakeRotateZMatrix(config_.rotation.z);
    engine::Matrix4x4 matRot = math.Multiply(matRotZ, math.Multiply(matRotX, matRotY));

    engine::Matrix4x4 matTrans = math.MakeTranslateMatrix(config_.center);
    engine::Matrix4x4 matColliderLocal = math.Multiply(matRot, matTrans);

    // 所有側とその親の変換を含め、コライダーの中心と軸をワールドへ移す。
    engine::Matrix4x4 matFinal = math.Multiply(matColliderLocal, GetCollisionWorldMatrix());

    // 中心、各軸の向きと長さを取り出し、ローカル半寸法をワールド半寸法へ直す。
    obb.center = { matFinal.m[3][0], matFinal.m[3][1], matFinal.m[3][2] };
    const engine::Vector3 worldAxes[3] = {
        { matFinal.m[0][0], matFinal.m[0][1], matFinal.m[0][2] },
        { matFinal.m[1][0], matFinal.m[1][1], matFinal.m[1][2] },
        { matFinal.m[2][0], matFinal.m[2][1], matFinal.m[2][2] }
    };

    float worldScale[3]{};
    const engine::Vector3 fallbackAxes[3] = {
        { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }
    };

    for (std::int32_t axis = 0; axis < 3; ++axis) {
        const engine::Vector3& value = worldAxes[axis];
        const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);

        // 親の拡大率も含む行列から軸と長さを分け、子の箱判定を見た目に追従させる。
        obb.orientations[axis] = length > 0.000001f ? value / length : fallbackAxes[axis];
        worldScale[axis] = length;
    }

    obb.size = {
        std::abs(config_.size.x) * worldScale[0],
        std::abs(config_.size.y) * worldScale[1],
        std::abs(config_.size.z) * worldScale[2]
    };

    return obb;
}

engine::Aabb Collider::GetAabb() const {
    // 軸平行の箱では、所有側の回転を外した中心と各軸の半寸法を計算する。
    if (config_.type == engine::ColliderType::kAabb && transform_) {
        // AABBは所有Objectの見た目の傾きへ追従させません。
        // 親Transformは維持しつつ、ローカル回転だけを除いた行列で中心を求めます。
        engine::Math math;
        const engine::Vector3 collisionScale = GetCollisionScale();
        engine::Matrix4x4 ownerMatrix = math.Multiply(
            math.MakeScaleMatrix(collisionScale),
            math.MakeTranslateMatrix(transform_->translate));
        // 親がいる場合は、ローカル回転を除いた行列に親のワールド行列を合成する。
        if (transform_->parent) {
            ownerMatrix = math.Multiply(ownerMatrix, transform_->parent->matWorld);
        }

        const engine::Matrix4x4 centerMatrix = math.Multiply(
            math.MakeTranslateMatrix(config_.center), ownerMatrix);
        const engine::Vector3 center = {
            centerMatrix.m[3][0],
            centerMatrix.m[3][1],
            centerMatrix.m[3][2]
        };

        const engine::Vector3 extent = {
            std::abs(config_.size.x * collisionScale.x),
            std::abs(config_.size.y * collisionScale.y),
            std::abs(config_.size.z * collisionScale.z)
        };

        return { center - extent, center + extent };
    }

    engine::Obb obb = GetObb();
    engine::Vector3 axisX = obb.orientations[0] * obb.size.x;
    engine::Vector3 axisY = obb.orientations[1] * obb.size.y;
    engine::Vector3 axisZ = obb.orientations[2] * obb.size.z;

    engine::Vector3 corners[8] = {
        obb.center - axisX - axisY - axisZ,
        obb.center + axisX - axisY - axisZ,
        obb.center - axisX + axisY - axisZ,
        obb.center + axisX + axisY - axisZ,
        obb.center - axisX - axisY + axisZ,
        obb.center + axisX - axisY + axisZ,
        obb.center - axisX + axisY + axisZ,
        obb.center + axisX + axisY + axisZ
    };

    // 傾いた箱は8頂点の各ワールド軸の最小・最大から外接箱を作る。
    engine::Vector3 minPos = corners[0];
    engine::Vector3 maxPos = corners[0];

    for (std::int32_t i = 1; i < 8; ++i) {
        minPos.x = std::min(minPos.x, corners[i].x);
        minPos.y = std::min(minPos.y, corners[i].y);
        minPos.z = std::min(minPos.z, corners[i].z);

        maxPos.x = std::max(maxPos.x, corners[i].x);
        maxPos.y = std::max(maxPos.y, corners[i].y);
        maxPos.z = std::max(maxPos.z, corners[i].z);
    }

    return { minPos, maxPos };
}

std::vector<engine::Vector3> Collider::GetFootprintWorldVertices(float localY) const {
    // 床の輪郭点を回転・親変換込みのワールド座標へ移し、見た目の縁と揃える。
    std::vector<engine::Vector3> vertices;
    // 3点以上の輪郭を持つ箱と変換の参照がそろう場合だけ、ワールドの輪郭を返す。
    if (config_.footprint.size() < 3 || !transform_ ||
        (config_.type != engine::ColliderType::kAabb && config_.type != engine::ColliderType::kObb)) return vertices;
    vertices.reserve(config_.footprint.size());
    engine::Math math;
    const engine::Matrix4x4 rotation = math.Multiply(
        math.MakeRotateZMatrix(config_.rotation.z),
        math.Multiply(math.MakeRotateXMatrix(config_.rotation.x), math.MakeRotateYMatrix(config_.rotation.y)));
    const engine::Matrix4x4 localToWorld = math.Multiply(
        math.Multiply(rotation, math.MakeTranslateMatrix(config_.center)), GetCollisionWorldMatrix());
    for (const engine::Vector2& vertex : config_.footprint) {
        vertices.push_back(engine::Math::Transform({ vertex.x, localY, vertex.y }, localToWorld));
    }

    return vertices;
}

bool Collider::ContainsFootprintPoint(const engine::Vector3& worldPoint) const {
    // XZ 断面の辺を同じ向きに回る点だけ内側とし、四角形の余白を除く。
    const auto vertices = GetFootprintWorldVertices(config_.size.y);
    // 輪郭が三点未満なら面を作れず、内側とは扱わない。
    if (vertices.size() < 3)
        return false;
    std::int32_t winding = 0;

    for (size_t i = 0; i < vertices.size(); ++i) {
        const engine::Vector3& a = vertices[i];
        const engine::Vector3& b = vertices[(i + 1) % vertices.size()];
        const float cross = (b.x - a.x) * (worldPoint.z - a.z) -
                            (b.z - a.z) * (worldPoint.x - a.x);
        // 辺上に十分近い点は符号の判定から外し、境界として内側扱いにする。
        if (std::abs(cross) < 0.0001f)
            continue;
        const std::int32_t sign = cross > 0.0f ? 1 : -1;
        // 辺上以外で符号が逆になった点は、凸な輪郭の外側にある。
        if (winding != 0 && sign != winding)
            return false;
        winding = sign;
    }

    return true;
}

bool Collider::SampleModelSurface(const engine::Vector3& worldPoint, float& outHeight,
                                  engine::Vector3& outNormal) const {
    // 三角形をXZへ投影し、接地点に重なる最も高い面と上向き法線を返す。
    // モデル表面か所有側のTransformがなければ、ワールド高さを測れない。
    if (!HasModelSurface() || !transform_)
        return false;
    const engine::Matrix4x4 world = GetCollisionWorldMatrix();
    bool found = false;
    float bestHeight = -std::numeric_limits<float>::max();

    for (const ModelSurfaceTriangle& triangle : modelSurface_) {
        const engine::Vector3 a = engine::Math::Transform(triangle.a, world);
        const engine::Vector3 b = engine::Math::Transform(triangle.b, world);
        const engine::Vector3 c = engine::Math::Transform(triangle.c, world);
        // XZに射影した三角形の面積に比例する分母。ほぼ線になる面は高さを安定して補間できないため除く。
        const float denominator = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
        // XZで面積がほぼ無い面は、補間に使う分母を求められないので除く。
        if (std::abs(denominator) < 0.000001f)
            continue;
        const float u = ((b.z - c.z) * (worldPoint.x - c.x) +
                         (c.x - b.x) * (worldPoint.z - c.z)) / denominator;
        const float v = ((c.z - a.z) * (worldPoint.x - c.x) +
                         (a.x - c.x) * (worldPoint.z - c.z)) / denominator;
        // 三頂点に対する重みの合計を1にする。どれかの重みが負ならその点は三角形の外。
        const float w = 1.0f - u - v;
        // いずれかの重みが許容値より小さければXZ投影の三角形外なので、面の高さは使わない。
        if (u < -0.0001f || v < -0.0001f || w < -0.0001f)
            continue;
        // XZ上で求めた三頂点の割合をワールドYへ掛け、三角形内部の表面高度を得る。
        const float height = u * a.y + v * b.y + w * c.y;

        // 初めて該当した面、またはこれまでより上にある面を採用する。
        if (!found || height > bestHeight) {
            bestHeight = height;
            outNormal = UpwardSurfaceNormal(a, b, c);
            found = true;
        }
    }

    // 面が見つかった場合にだけ、高さの出力引数を書き換える。
    if (found)
        outHeight = bestHeight;
    return found;
}

bool Collider::RaycastModelSurface(const engine::Vector3& start, const engine::Vector3& direction,
                                   float maxDistance, float& outDistance,
                                   engine::Vector3& outNormal) const {
    // 岩の上面へレイを飛ばし、箱の天面に先に当たる誤判定を避ける。
    // モデル表面や変換を取得できない場合、または距離が負の場合は交差を試さない。
    if (!HasModelSurface() || !transform_ || maxDistance < 0.0f)
        return false;
    const engine::Matrix4x4 world = GetCollisionWorldMatrix();
    bool found = false;
    float nearest = maxDistance;

    for (const ModelSurfaceTriangle& triangle : modelSurface_) {
        const engine::Vector3 a = engine::Math::Transform(triangle.a, world);
        const engine::Vector3 b = engine::Math::Transform(triangle.b, world);
        const engine::Vector3 c = engine::Math::Transform(triangle.c, world);
        const engine::Vector3 edge1 = b - a;
        const engine::Vector3 edge2 = c - a;
        const engine::Vector3 p = CrossSurface(direction, edge2);
        // 値が0に近い場合はレイが面と平行に近く、交点を求める際の除算が不安定なので調べない。
        const float determinant = DotSurface(edge1, p);
        // 面にほぼ平行なレイは、除算を避けるため判定しない。
        if (std::abs(determinant) < 0.000001f)
            continue;
        const float inverse = 1.0f / determinant;
        const engine::Vector3 fromA = start - a;
        const float u = DotSurface(fromA, p) * inverse;
        // 第一の三角形座標が0〜1の範囲外なら、面の内側へ届かない。
        if (u < 0.0f || u > 1.0f)
            continue;
        const engine::Vector3 q = CrossSurface(fromA, edge1);
        const float v = DotSurface(direction, q) * inverse;
        // もう一方の座標と合わせて三角形の中に収まる場合にだけ距離を調べる。
        if (v < 0.0f || u + v > 1.0f)
            continue;
        const float distance = DotSurface(edge2, q) * inverse;
        // 開始点より後ろか、現在の最短より遠い交点を省く。
        if (distance < 0.0f || distance > nearest)
            continue;
        nearest = distance;
        outNormal = UpwardSurfaceNormal(a, b, c);
        found = true;
    }

    // 有効な交差があった場合だけ、最短の距離を呼び出し元へ渡す。
    if (found)
        outDistance = nearest;
    return found;
}

// 床のXZ輪郭と相手の断面を調べ、必要なら押し出しに使う方向と深さを返す。
bool Collider::OverlapsFootprint(const Collider* other, engine::Vector3* normal, float* depth) const {
    // 相手が無いか輪郭が未設定なら、輪郭専用の交差は成立しない。
    if (!other || config_.footprint.size() < 3)
        return false;
    const std::vector<engine::Vector3> polygon = GetFootprintWorldVertices(config_.size.y);
    // 変換後に有効な三角形以上の輪郭を得られなければ判定を打ち切る。
    if (polygon.size() < 3)
        return false;

    float minimumDepth = std::numeric_limits<float>::max();
    engine::Vector3 outward = engine::Vector3::kZero;

    // 球は水平面では円として扱い、相手側で選ぶ軸を円の中心から作る。
    if (other->GetType() == engine::ColliderType::kSphere) {
        const engine::Vector3 center = other->GetSphereCenter();
        const float radius = other->GetRadius();

        // 円と各辺・最も近い頂点を分離軸で調べる。
        std::vector<engine::Vector3> axes;
        axes.reserve(polygon.size() + 1);
        float nearestDistance = std::numeric_limits<float>::max();
        engine::Vector3 nearestVertex{};

        for (size_t i = 0; i < polygon.size(); ++i) {
            const engine::Vector3 edge = polygon[(i + 1) % polygon.size()] - polygon[i];
            const float length = std::sqrt(edge.x * edge.x + edge.z * edge.z);

            // 有効な長さの辺だけ、輪郭の外向きを調べる分離軸へ加える。
            if (length > 0.00001f)
                axes.push_back({ -edge.z / length, 0.0f, edge.x / length });
            const engine::Vector3 delta = center - polygon[i];
            const float distance = delta.x * delta.x + delta.z * delta.z;

            // 候補の頂点が円の中心により近ければ、角の判定に使う頂点を更新する。
            if (distance < nearestDistance) {
                nearestDistance = distance;
                nearestVertex = polygon[i];
            }
        }

        const engine::Vector3 vertexDelta = center - nearestVertex;

        // 中心と頂点がほぼ同じ位置でなければ、その方向も角用の分離軸へ加える。
        if (nearestDistance > 0.0000001f) {
            const float length = std::sqrt(nearestDistance);
            axes.push_back({ vertexDelta.x / length, 0.0f, vertexDelta.z / length });
        }

        for (const engine::Vector3& axis : axes) {
            float minimum = std::numeric_limits<float>::max(), maximum = -minimum;

            for (const engine::Vector3& point : polygon) {
                const float projection = point.x * axis.x + point.z * axis.z;
                minimum = (std::min)(minimum, projection);
                maximum = (std::max)(maximum, projection);
            }

            const float projectedCenter = center.x * axis.x + center.z * axis.z;
            // 円の射影は中心の射影から半径だけ両側へ広がる。どの軸でも正の重なりが必要。
            const float overlap = (std::min)(maximum, projectedCenter + radius) -
                                  (std::max)(minimum, projectedCenter - radius);
            // 射影に正の重なりが無い軸が一つでもあれば、交差しない。
            if (overlap <= 0.0f)
                return false;
            // 最小の重なりを更新できる場合に、円の中心側を向く符号で方向を保持する。
            if (overlap < minimumDepth) {
                minimumDepth = overlap;
                outward = axis * (projectedCenter >= (minimum + maximum) * 0.5f ? 1.0f : -1.0f);
            }
        }
    // 球以外は箱のXZ矩形へ置き換え、輪郭同士を調べる。
    } else {
        std::vector<engine::Vector3> section;

        // 回転した箱は、ワールドの中心から水平面の二軸を伸ばして輪郭を作る。
        if (other->GetType() == engine::ColliderType::kObb) {
            const engine::Obb box = other->GetObb();
            const engine::Vector3 x = box.orientations[0] * box.size.x;
            const engine::Vector3 z = box.orientations[2] * box.size.z;
            section = { box.center - x - z, box.center + x - z,
                        box.center + x + z, box.center - x + z };
        // それ以外の形は外接する軸平行の箱から水平面の矩形を作る。
        } else {
            const engine::Aabb box = other->GetAabb();
            section = { { box.min.x, 0.0f, box.min.z }, { box.max.x, 0.0f, box.min.z },
                        { box.max.x, 0.0f, box.max.z }, { box.min.x, 0.0f, box.max.z } };
        }

        // 箱はXZの断面を用意し、床と箱の両方の辺を分離軸の候補にする。
        if (!OverlapOnAxes(polygon, section, minimumDepth, outward))
            return false;
    }

    // 呼び出し側が必要とする場合に、輪郭の外向き法線と XZ 平面上の押し出し距離を返す。
    // 法線の受け取り先が指定されている場合だけ、輪郭からの外向きを渡す。
    if (normal)
        *normal = outward;
    // 深さの受け取り先が指定されている場合だけ、最も浅い重なりを渡す。
    if (depth)
        *depth = minimumDepth;
    return true;
}

engine::Ring Collider::GetRing() const {
    engine::Ring ring;
    // Transformが無ければ、ワールドのリング形状を設定できないため既定値を返す。
    if (!transform_)
        return ring;

    engine::Math math;

    // まずリング自身の角度からローカルの回転を組み立てる。
    engine::Matrix4x4 matRotX = math.MakeRotateXMatrix(config_.rotation.x);
    engine::Matrix4x4 matRotY = math.MakeRotateYMatrix(config_.rotation.y);
    engine::Matrix4x4 matRotZ = math.MakeRotateZMatrix(config_.rotation.z);
    engine::Matrix4x4 matRot = math.Multiply(matRotZ, math.Multiply(matRotX, matRotY));

    // リングのローカル上向きを、判定行列と合成した変換でワールド方向へ移す。
    engine::Vector3 localNormal = { 0, 1, 0 }; // ローカル空間でのリングの向き (上向き)
    
    engine::Matrix4x4 worldRot = math.Multiply(matRot, GetCollisionWorldMatrix());

    // 平行移動を使わず方向を変換し、長さを1に揃える。
    ring.normal = math.Normalize({
        localNormal.x * worldRot.m[0][0] + localNormal.y * worldRot.m[1][0] + localNormal.z * worldRot.m[2][0],
        localNormal.x * worldRot.m[0][1] + localNormal.y * worldRot.m[1][1] + localNormal.z * worldRot.m[2][1],
        localNormal.x * worldRot.m[0][2] + localNormal.y * worldRot.m[1][2] + localNormal.z * worldRot.m[2][2]
    });

    // 位置オフセットを所有側の判定行列で変換して中心を求める。
    engine::Matrix4x4 matTrans = math.MakeTranslateMatrix(config_.center);
    engine::Matrix4x4 matFinal = math.Multiply(matTrans, GetCollisionWorldMatrix());
    ring.center = { matFinal.m[3][0], matFinal.m[3][1], matFinal.m[3][2] };

    // Xは外半径、Zは内半径、Yは半分の厚みとして判定倍率を適用する。
    const engine::Vector3 collisionScale = GetCollisionScale();
    ring.outerRadius = config_.size.x * std::abs(collisionScale.x);
    ring.innerRadius = config_.size.z * std::abs(collisionScale.z);
    ring.height = config_.size.y * 2.0f * std::abs(collisionScale.y);

    // 外半径より内半径が大きい保存値は、順番を入れ替えて判定に渡す。
    if (ring.outerRadius < ring.innerRadius) {
        std::swap(ring.outerRadius, ring.innerRadius);
    }

    return ring;
}

engine::Cylinder Collider::GetCylinder() const {
    engine::Cylinder cyl;
    // 所有側のTransformが無ければ、変換前の既定の円柱を返す。
    if (!transform_)
        return cyl;
    engine::Math math;

    // 円柱自体の傾きをローカルのXYZ角度から組み立てる。
    engine::Matrix4x4 matRotX = math.MakeRotateXMatrix(config_.rotation.x);
    engine::Matrix4x4 matRotY = math.MakeRotateYMatrix(config_.rotation.y);
    engine::Matrix4x4 matRotZ = math.MakeRotateZMatrix(config_.rotation.z);
    engine::Matrix4x4 matRot = math.Multiply(matRotZ, math.Multiply(matRotX, matRotY));

    // ローカルの中心オフセットを所有側の判定行列でワールドへ移す。
    engine::Matrix4x4 matTrans = math.MakeTranslateMatrix(config_.center);
    engine::Matrix4x4 matFinal = math.Multiply(math.Multiply(matRot, matTrans), GetCollisionWorldMatrix());
    cyl.center = { matFinal.m[3][0], matFinal.m[3][1], matFinal.m[3][2] };

    // ローカルのY軸をワールドへ移し、正規化して円柱の向きを得る。
    engine::Vector3 localAxis = { 0, 1, 0 };
    engine::Matrix4x4 worldRot = math.Multiply(matRot, GetCollisionWorldMatrix());
    cyl.axis = math.Normalize({
        localAxis.x * worldRot.m[0][0] + localAxis.y * worldRot.m[1][0] + localAxis.z * worldRot.m[2][0],
        localAxis.x * worldRot.m[0][1] + localAxis.y * worldRot.m[1][1] + localAxis.z * worldRot.m[2][1],
        localAxis.x * worldRot.m[0][2] + localAxis.y * worldRot.m[1][2] + localAxis.z * worldRot.m[2][2]
    });

    // Xの判定倍率で半径を求め、Yの半寸法は全高へ直す。
    const engine::Vector3 collisionScale = GetCollisionScale();
    cyl.radius = config_.size.x * std::abs(collisionScale.x);
    cyl.height = config_.size.y * 2.0f * std::abs(collisionScale.y);

    return cyl;
}

bool Collider::SampleTerrain(const Collider* terrain, const engine::Vector3& worldPosition, float& outHeight, engine::Vector3& outNormal) const {
    // ワールド位置を地形の格子へ戻し、補間した高さと法線を衝突側へ渡す。
    // 地形と位置基準のTransformがなければ、ワールドから格子へ戻せない。
    if (!terrain || !terrain->transform_)
        return false;
    const TerrainCollisionData& data = terrain->terrainData_;
    // 衝突格子が無効か解像度が0以下の場合は、高さを取得できない。
    if (!data.enabled || data.resolution <= 0)
        return false;

    const std::int32_t sampleCount = data.resolution + 1;
    // 四隅の補間に必要な端点を含む格子のデータ量が足りなければ中止する。
    if (static_cast<std::int32_t>(data.heights.size()) < sampleCount * sampleCount)
        return false;

    const engine::Vector3 terrainPos = terrain->transform_->translate;
    const engine::Vector3 terrainScale = terrain->transform_->scale;
    const float scaleX = std::max(0.0001f, std::abs(terrainScale.x));
    const float scaleY = std::max(0.0001f, std::abs(terrainScale.y));
    const float scaleZ = std::max(0.0001f, std::abs(terrainScale.z));
    // 原点は地形の中央なので、ローカル座標を格子の0～1に直すとき半分ずらす。
    const float localX = (worldPosition.x - terrainPos.x) / scaleX;
    const float localZ = (worldPosition.z - terrainPos.z) / scaleZ;
    const float u = localX / std::max(0.0001f, data.sizeX) + 0.5f;
    const float v = localZ / std::max(0.0001f, data.sizeZ) + 0.5f;
    // 地形の水平範囲外では、端の高さを使って接地した扱いにしない。
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
        return false;

    // 解像度はセルの数。端点を含む頂点格子で周囲の 4 点を選び、セル内の比率を求める。
    const float fx = u * static_cast<float>(data.resolution);
    const float fz = v * static_cast<float>(data.resolution);
    const std::int32_t x0 = std::clamp(static_cast<std::int32_t>(std::floor(fx)), 0, data.resolution);
    const std::int32_t z0 = std::clamp(static_cast<std::int32_t>(std::floor(fz)), 0, data.resolution);
    const std::int32_t x1 = std::clamp(x0 + 1, 0, data.resolution);
    const std::int32_t z1 = std::clamp(z0 + 1, 0, data.resolution);
    const float tx = fx - static_cast<float>(x0);
    const float tz = fz - static_cast<float>(z0);

    auto heightAt = [&](std::int32_t x, std::int32_t z) -> float {
        return data.heights[static_cast<size_t>(z * sampleCount + x)];
    };

    const float h00 = heightAt(x0, z0);
    const float h10 = heightAt(x1, z0);
    const float h01 = heightAt(x0, z1);
    const float h11 = heightAt(x1, z1);
    // X 方向の補間を 2 本計算してから Z 方向で補間し、地形の Y スケールと位置を戻す。
    const float hx0 = h00 + (h10 - h00) * tx;
    const float hx1 = h01 + (h11 - h01) * tx;
    const float localHeight = hx0 + (hx1 - hx0) * tz;
    outHeight = terrainPos.y + localHeight * scaleY;

    // 隣接頂点の高さ差をワールド距離で割り、横方向の勾配から上向き法線を近似する。
    const std::int32_t xm = std::clamp(x0 - 1, 0, data.resolution);
    const std::int32_t xp = std::clamp(x0 + 1, 0, data.resolution);
    const std::int32_t zm = std::clamp(z0 - 1, 0, data.resolution);
    const std::int32_t zp = std::clamp(z0 + 1, 0, data.resolution);
    const float cellX = std::max(0.0001f, data.sizeX / static_cast<float>(data.resolution) * scaleX);
    const float cellZ = std::max(0.0001f, data.sizeZ / static_cast<float>(data.resolution) * scaleZ);
    const float gradX = ((heightAt(xp, z0) - heightAt(xm, z0)) * scaleY) / cellX;
    const float gradZ = ((heightAt(x0, zp) - heightAt(x0, zm)) * scaleY) / cellZ;
    outNormal = { -gradX, 1.0f, -gradZ };
    const float vectorLength = std::sqrt(outNormal.x * outNormal.x + outNormal.y * outNormal.y + outNormal.z * outNormal.z);

    // 勾配から作った法線を正規化できる場合に、押し出しに使える長さへ揃える。
    if (vectorLength > 0.0001f) {
        outNormal.x /= vectorLength;
        outNormal.y /= vectorLength;
        outNormal.z /= vectorLength;
    // 長さが十分でない法線には、地面の既定の上向きを用いる。
    } else {
        outNormal = { 0.0f, 1.0f, 0.0f };
    }

    return true;
}

engine::CollisionInfo Collider::CheckSphereTerrainCollision(const engine::Vector3& spherePos, float radius, const Collider* terrain) const {
    // 球の底が地形面を跨いだ場合だけ、法線方向へ押し戻す情報を作る。
    engine::CollisionInfo collision;
    float terrainHeight = 0.0f;
    engine::Vector3 normal = { 0.0f, 1.0f, 0.0f };
    // 球のXZで地形の高さを得られなければ、地形と接触していない結果を返す。
    if (!SampleTerrain(terrain, spherePos, terrainHeight, normal))
        return collision;

    const float bottom = spherePos.y - radius;
    const float top = spherePos.y + radius;

    // 球が高さを跨ぐ場合に加え、球の上端が面から半径の1/4以内まで下にある場合も許容する。
    if (bottom <= terrainHeight && top >= terrainHeight - radius * 0.25f) {
        collision.isColliding = true;
        collision.normal = normal;
        collision.penetration = terrainHeight - bottom;
    }

    return collision;
}

engine::CollisionInfo Collider::CheckAabbTerrainCollision(const engine::Aabb& aabb, const Collider* terrain) const {
    // 中心と四隅を採点し、斜面で最も深く侵入した位置を基準にする。
    engine::CollisionInfo collision;
    const engine::Vector3 points[5] = {
        { (aabb.min.x + aabb.max.x) * 0.5f, aabb.min.y, (aabb.min.z + aabb.max.z) * 0.5f },
        { aabb.min.x, aabb.min.y, aabb.min.z },
        { aabb.max.x, aabb.min.y, aabb.min.z },
        { aabb.min.x, aabb.min.y, aabb.max.z },
        { aabb.max.x, aabb.min.y, aabb.max.z },
    };

    float bestPenetration = 0.0f;
    engine::Vector3 bestNormal = { 0.0f, 1.0f, 0.0f };

    for (const engine::Vector3& point : points) {
        float terrainHeight = 0.0f;
        engine::Vector3 normal = { 0.0f, 1.0f, 0.0f };
        // 今調べている点で地形の高さを取れなければ、残りの点へ進む。
        if (!SampleTerrain(terrain, point, terrainHeight, normal))
            continue;
        // 箱の下端が面以下で、上端が面の0.05以内まで届く点を接触候補にする。
        if (aabb.min.y <= terrainHeight && aabb.max.y >= terrainHeight - 0.05f) {
            const float penetration = terrainHeight - aabb.min.y;

            // さらに深い侵入が見つかった場合だけ、高さとその地点の法線を置き換える。
            if (penetration > bestPenetration) {
                bestPenetration = penetration;
                bestNormal = normal;
            }
        }
    }

    // 五点のうち一つでも正の深さを取得したら、最深箇所を接触結果として返す。
    if (bestPenetration > 0.0f) {
        collision.isColliding = true;
        collision.normal = bestNormal;
        collision.penetration = bestPenetration;
    }

    return collision;
}

engine::CollisionInfo Collider::CheckCollision(const Collider* other) const {
    engine::CollisionInfo collision;
    collision.isColliding = false;

    // 相手またはいずれかの変換が無い場合は判定を作れない。
    if (!other || !transform_ || !other->transform_)
        return collision;

    engine::ColliderType myType = this->GetType();
    engine::ColliderType otherType = other->GetType();

    // どちらかが形状なしなら、接触は発生しない。
    if (myType == engine::ColliderType::kNone || otherType == engine::ColliderType::kNone) {
        return collision;
    }

    // 箱・球との接触は厚みのあるモデル形状を優先し、上面専用補正へ戻さない。
    const auto supported=[](engine::ColliderType type){return type==engine::ColliderType::kSphere || type==engine::ColliderType::kAabb || type==engine::ColliderType::kObb;};
    // 自分の凸包を使い、相手が対応する球か箱なら、ほかの近似形状は使わない。
    if(HasModelVolume() && supported(otherType))
        return CheckModelVolume(other);
    // 相手側の凸包を使う場合は、判定後の法線だけ自分が押し出される向きに直す。
    if(other->HasModelVolume() && supported(myType)) {
        collision=other->CheckModelVolume(this);
        collision.normal=collision.normal*-1.0f;
        return collision;
    }

    // 球の中心はコライダーの中心オフセットを含んだ座標を使う。
    engine::Vector3 myPos = myType == engine::ColliderType::kSphere ? GetSphereCenter() : transform_->translate;
    engine::Vector3 otherPos = otherType == engine::ColliderType::kSphere ? other->GetSphereCenter() : other->transform_->translate;

    // 自分が球・相手が地形の場合は、球の中心と下端で地形の高さを調べる。
    if (myType == engine::ColliderType::kSphere && otherType == engine::ColliderType::kTerrain) {
        collision = CheckSphereTerrainCollision(myPos, this->GetRadius(), other);
    // 自分が地形の場合は相手の球を調べ、結果の法線を自分側へ反転する。
    } else if (myType == engine::ColliderType::kTerrain && otherType == engine::ColliderType::kSphere) {
        collision = other->CheckSphereTerrainCollision(otherPos, other->GetRadius(), this);
        collision.normal = collision.normal * -1.0f;
    // 箱または円柱と地形では、移動側を包むAABBの底面を調べる。
    } else if ((myType == engine::ColliderType::kAabb || myType == engine::ColliderType::kObb || myType == engine::ColliderType::kCylinder) && otherType == engine::ColliderType::kTerrain) {
        collision = CheckAabbTerrainCollision(this->GetAabb(), other);
    // 自分が地形で相手が箱・円柱の場合も同じ底面を調べ、法線を反転する。
    } else if (myType == engine::ColliderType::kTerrain && (otherType == engine::ColliderType::kAabb || otherType == engine::ColliderType::kObb || otherType == engine::ColliderType::kCylinder)) {
        collision = other->CheckAabbTerrainCollision(other->GetAabb(), this);
        collision.normal = collision.normal * -1.0f;
    }

    // 両方の箱が軸平行なら、各ワールド軸の重なりを使う。
    else if (myType == engine::ColliderType::kAabb && otherType == engine::ColliderType::kAabb) {
        collision = engine::CheckAabbCollision(this->GetAabb(), other->GetAabb());
    // 両方が球なら、二つの中心距離と半径から接触を求める。
    } else if (myType == engine::ColliderType::kSphere && otherType == engine::ColliderType::kSphere) {
        collision = engine::CheckSphereCollision(
            myPos, this->GetRadius(),
            otherPos, other->GetRadius());
    // 両方の箱が回転する場合は、向きを含むOBBの判定を使う。
    } else if (myType == engine::ColliderType::kObb && otherType == engine::ColliderType::kObb) {
        collision = engine::CheckObbCollision(this->GetObb(), other->GetObb());
    }

    // 球と軸平行の箱では、球を主語とする共通の判定を使う。
    else if (myType == engine::ColliderType::kSphere && otherType == engine::ColliderType::kAabb) {
        collision = engine::CheckSphereAabbCollision(myPos, this->GetRadius(), other->GetAabb());
    // 自分が軸平行の箱なら同じ判定の引数を入れ替え、法線を反転する。
    } else if (myType == engine::ColliderType::kAabb && otherType == engine::ColliderType::kSphere) {
        collision = engine::CheckSphereAabbCollision(otherPos, other->GetRadius(), this->GetAabb());
        collision.normal = collision.normal * -1.0f;
    // 球と回転する箱は、箱の軸を使って球との距離を判定する。
    } else if (myType == engine::ColliderType::kSphere && otherType == engine::ColliderType::kObb) {
        collision = engine::CheckSphereObbCollision(myPos, this->GetRadius(), other->GetObb());
    // 自分が回転する箱の場合は相手の球を調べた後、法線を反転する。
    } else if (myType == engine::ColliderType::kObb && otherType == engine::ColliderType::kSphere) {
        collision = engine::CheckSphereObbCollision(otherPos, other->GetRadius(), this->GetObb());
        collision.normal = collision.normal * -1.0f;
    // 自分がAABBなら、AABBを先に渡す共通の箱判定を使う。
    } else if (myType == engine::ColliderType::kAabb && otherType == engine::ColliderType::kObb) {
        collision = engine::CheckAabbObbCollision(this->GetAabb(), other->GetObb());

    // 自分がOBBなら共通判定の順序を揃え、戻った法線を反転する。
    } else if (myType == engine::ColliderType::kObb && otherType == engine::ColliderType::kAabb) {
        collision = engine::CheckAabbObbCollision(other->GetAabb(), this->GetObb());
        collision.normal = collision.normal * -1.0f;
    }

    // リングと球は、穴と外周を持つリングの判定を使う。
    else if (myType == engine::ColliderType::kRing && otherType == engine::ColliderType::kSphere) {
        collision = engine::CheckRingSphereCollision(this->GetRing(), otherPos, other->GetRadius());
    // 自分が球の場合はリングを相手側から取り出し、法線を反転する。
    } else if (myType == engine::ColliderType::kSphere && otherType == engine::ColliderType::kRing) {
        collision = engine::CheckRingSphereCollision(other->GetRing(), myPos, this->GetRadius());
        collision.normal = collision.normal * -1.0f;
    }
    // リングと箱は、軸平行の箱もOBB表現に統一して調べる。
    else if (myType == engine::ColliderType::kRing && (otherType == engine::ColliderType::kObb || otherType == engine::ColliderType::kAabb)) {
        collision = engine::CheckRingObbCollision(this->GetRing(), other->GetObb());
    // 自分が箱の場合は、同じリング判定の法線を反転する。
    } else if ((myType == engine::ColliderType::kObb || myType == engine::ColliderType::kAabb) && otherType == engine::ColliderType::kRing) {
        collision = engine::CheckRingObbCollision(other->GetRing(), this->GetObb());
        collision.normal = collision.normal * -1.0f;
    }

    // 自分が円柱で相手が球なら、円柱の軸と半径を使う判定へ渡す。
    else if (myType == engine::ColliderType::kCylinder && otherType == engine::ColliderType::kSphere) {
        collision = engine::CheckCylinderSphereCollision(this->GetCylinder(), otherPos, other->GetRadius());
    // 自分が球の組み合わせでは、同じ円柱・球判定の法線を反転する。
    } else if (myType == engine::ColliderType::kSphere && otherType == engine::ColliderType::kCylinder) {
        collision = engine::CheckCylinderSphereCollision(other->GetCylinder(), myPos, this->GetRadius());
        collision.normal = collision.normal * -1.0f;
    }
    // 円柱と箱の組み合わせは、箱をOBB表現へ揃えて調べる。
    else if (myType == engine::ColliderType::kCylinder && (otherType == engine::ColliderType::kObb || otherType == engine::ColliderType::kAabb)) {
        collision = engine::CheckCylinderObbCollision(this->GetCylinder(), other->GetObb());
    // 自分が箱で相手が円柱の場合も同じ判定を使い、法線を反転する。
    } else if ((myType == engine::ColliderType::kObb || myType == engine::ColliderType::kAabb) && otherType == engine::ColliderType::kCylinder) {
        collision = engine::CheckCylinderObbCollision(other->GetCylinder(), this->GetObb());
        collision.normal = collision.normal * -1.0f;
    }

    // 箱同士などの判定で接触していても、実際の輪郭から外れた接触はここで除く。
    if (collision.isColliding && (myType == engine::ColliderType::kAabb || myType == engine::ColliderType::kObb) &&
        config_.footprint.size() >= 3) {
        engine::Vector3 horizontalNormal;
        float horizontalDepth = 0.0f;

        // 自分の輪郭と相手に水平の重なりが無ければ、先に得た接触を取り消す。
        if (!OverlapsFootprint(other, &horizontalNormal, &horizontalDepth)) {
            return {};
        }

        // 輪郭側の侵入が浅い場合は、水平方向の押し出しを優先する。
        if (horizontalDepth < collision.penetration) {
            collision.normal = horizontalNormal * -1.0f;
            collision.penetration = horizontalDepth;
        }
    }

    // 相手にも輪郭があれば調べ、法線をこの Collider の押し出し方向へ揃える。
    if (collision.isColliding && (otherType == engine::ColliderType::kAabb || otherType == engine::ColliderType::kObb) &&
        other->config_.footprint.size() >= 3) {
        engine::Vector3 horizontalNormal;
        float horizontalDepth = 0.0f;

        // 相手の輪郭から外れる場合も、箱の外接範囲で得た接触を取り消す。
        if (!other->OverlapsFootprint(this, &horizontalNormal, &horizontalDepth)) {
            return {};
        }

        // 相手の輪郭側からより短く押し出せるなら、その法線と深さを使う。
        if (horizontalDepth < collision.penetration) {
            collision.normal = horizontalNormal;
            collision.penetration = horizontalDepth;
        }
    }

    // 接触が残った場合だけ、実際のモデル高さで追補できるか確認する。
    if (collision.isColliding) {
        // 側面は従来形状で受け、上からの接触だけをモデルの実際の高さへ合わせる。
        // 両方にモデル上面がある場合は、このコライダーを補正に使う。
        const Collider* surface = nullptr;
        if (HasModelSurface()) {
            surface = this;
        } else if (other->HasModelSurface()) {
            surface = other;
        }
        const Collider* mover = surface == this ? other : this;
        // 上面を持つ相手と、上下の範囲を求められる形状の組み合わせだけを補正する。
        if (surface && mover != surface &&
            (mover->GetType() == engine::ColliderType::kSphere || mover->GetType() == engine::ColliderType::kAabb ||
             mover->GetType() == engine::ColliderType::kObb || mover->GetType() == engine::ColliderType::kCylinder)) {
            const engine::Aabb moverBounds = mover->GetAabb();
            const engine::Vector3 center = mover->GetType() == engine::ColliderType::kSphere
                ? mover->GetSphereCenter() : (moverBounds.min + moverBounds.max) * 0.5f;
            const float bottom = mover->GetType() == engine::ColliderType::kSphere
                ? center.y - mover->GetRadius() : moverBounds.min.y;
            float surfaceHeight = 0.0f;
            engine::Vector3 surfaceNormal{};
            // 実際の表面をサンプルでき、相手の中心が表面の少し下より上なら、上面の接触として見直す。
            if (surface->SampleModelSurface(center, surfaceHeight, surfaceNormal) &&
                center.y >= surfaceHeight - 0.12f) {
                // 当たった箱の範囲内でも、相手の底が実際の面より上なら未接触とする。
                if (bottom > surfaceHeight) {
                    collision = {};
                // 底が面を越えている場合は、実際の高さとの差と上向き法線を判定に使う。
                } else {
                    collision.normal = surface == other ? surfaceNormal : surfaceNormal * -1.0f;
                    collision.penetration = surfaceHeight - bottom;
                }
            }
        }
    }

    return collision;
}

} // namespace engine
