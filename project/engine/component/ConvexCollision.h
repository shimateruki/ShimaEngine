#pragma once
#include <cstdint>

// 担当：モデル頂点から凸包を作り、レイ・球・箱との交差を計算する。
// 関連：Collider（呼出側）、MathのVector3（同一座標系の入力）
// 主な入口：Build / Ray / Sphere / Box
#include "engine/utility/math/Math.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

// 描画頂点から作る凸包。凹形状の門などは、柱・梁を別Colliderにして使用する。
namespace convexCollision {
using V = engine::Vector3;
using Face = std::array<V, 3>;
// 二つのベクトルの内積を返す。
inline float Dot(const V& a, const V& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
// 二つのベクトルに垂直な外積ベクトルを返す。
inline V Cross(const V& a, const V& b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
// 三角形の単位法線を返す。面積ゼロならゼロベクトル。
inline V Normal(const Face& f) {
    // 面積ゼロの面からNaNを広げない。
    const V n = Cross(f[1]-f[0], f[2]-f[0]);
    const float length = std::sqrt(Dot(n,n));
    return length > 1e-7f ? n / length : V{};
}
// 入力点は同一座標系で渡す。立体の初期四面体を作れない点列は空の凸包になる。
inline std::vector<Face> Build(const std::vector<V>& input) {
    // 最初の四面体を外側の点へ順に拡張する。読み込み時だけ実行する。
    std::vector<V> points;
    for (const V& p : input) {
        // 既存点から距離の二乗が10のマイナス10乗未満の点は重複とみなし、離れた点だけを使う。
        if (std::none_of(points.begin(), points.end(), [&](const V& q) { return Dot(p-q,p-q)<1e-10f; }))
            points.push_back(p);
    }
    // 重複頂点を除いた後で四面体を作れる点数がなければ、体積を持つ凸包を作れない。
    if (points.size()<4)
        return {};
    // 基準点、そこから最遠の点、二点を結ぶ直線から最遠の点の順に、初期三角形の頂点を選ぶ。
    size_t a=0,b=0,c=0,d=0;
    float best=0;
    for (size_t i=1;i<points.size();++i) {
        const float value=Dot(points[i]-points[a],points[i]-points[a]);
        // 基準点aからより遠い点があれば、二つ目の頂点候補を更新する。
        if (value>best) {
            best=value;
            b=i;
        }
    }
    best=0;
    for (size_t i=0;i<points.size();++i) {
        const V n=Cross(points[b]-points[a],points[i]-points[a]);
        const float value=Dot(n,n);
        // abとの外積がより大きい場合は、直線abからより遠い点として三つ目の候補を更新する。
        if(value>best) {
            best=value;
            c=i;
        }
    }
    // 最大の外積でもほぼゼロなら全点がほぼ同一直線にあり、初期面を作れない。
    if(best<1e-12f)
        return {};
    // 三角形の平面から法線方向に最も離れた点を四つ目の頂点にして、平面の退化を避ける。
    const V n=Normal({points[a],points[b],points[c]});
    best=0;
    for(size_t i=0;i<points.size();++i) {
        const float value=std::abs(Dot(n,points[i]-points[a]));
        // 初期平面からの距離が最大となる点を、四つ目の候補にする。
        if(value>best) {
            best=value;
            d=i;
        }
    }
    // 全点が初期平面に近すぎるなら、体積のある四面体を作れないため凸包を返さない。
    if(best<1e-6f)
        return {};
    const V inside=(points[a]+points[b]+points[c]+points[d])*0.25f;
    // 内部点を基準に全ての面を外向きへ揃え、後続の平面判定で符号を統一する。
    using Indices=std::array<size_t,3>;
    std::vector<Indices> faces;
    const auto add=[&](size_t x,size_t y,size_t z) {
        // 面の法線が内部点を向く場合は二頂点を入れ替え、法線を外へ向ける。
        if(Dot(Cross(points[y]-points[x],points[z]-points[x]),inside-points[x])>0)
            std::swap(y,z);
        faces.push_back({x,y,z});
    };
    add(a,b,c);
    add(a,d,b);
    add(a,c,d);
    add(b,d,c);
    for(size_t i=0;i<points.size();++i) {
        // 追加点から見える面を除き、逆向きの共有辺を相殺して拡張境界だけを残す。
        std::vector<std::array<size_t,2>> boundary;
        std::vector<Indices> kept;
        for(const auto& f:faces) {
            // 追加点が面の外側に十分離れていなければ、その面は現在の凸包に残す。
            if(Dot(Normal({points[f[0]],points[f[1]],points[f[2]]}),points[i]-points[f[0]])<=1e-5f) {
                kept.push_back(f);
                continue;
            }
            for(std::int32_t e=0;e<3;++e) {
                const std::array<size_t,2> edge{f[e],f[(e+1)%3]};
                auto reverse=std::find(boundary.begin(),boundary.end(),std::array<size_t,2>{edge[1],edge[0]});
                // 逆向きに同じ辺をまだ見つけていなければ、穴の縁の候補として保持する。
                if(reverse==boundary.end())
                    boundary.push_back(edge);
                // 逆向きの辺があれば、外す二面が共有する辺なので縁の候補から除く。
                else boundary.erase(reverse);
            }
        }
        faces=std::move(kept);
        for(const auto& e:boundary)
            add(e[0],e[1],i);
    }
    std::vector<Face> result;
    for(const auto& f:faces)
        result.push_back({points[f[0]],points[f[1]],points[f[2]]});
    return result;
}

// 三角形の面・辺・頂点から、指定点に最も近い位置を求める。
inline V Closest(const V& p,const Face& f) {
    // 三角形の面・辺・頂点の領域を分け、球が角に触れる場合も距離を正しく求める。
    const V a=f[0],b=f[1],c=f[2],ab=b-a,ac=c-a,ap=p-a;
    const float d1=Dot(ab,ap),d2=Dot(ac,ap);
    // 点がABにもACにも逆側へ投影される場合は、頂点Aが最も近い。
    if(d1<=0 && d2<=0)
        return a;
    const V bp=p-b;
    const float d3=Dot(ab,bp),d4=Dot(ac,bp);
    // Bから見てABの先側にあり、BC側へ出ていなければ、頂点Bが最も近い。
    if(d3>=0 && d4<=d3)
        return b;
    // AとBを結ぶ辺の外側へ射影される場合は、その辺の上で最近点を補間する。
    const float vc=d1*d4-d3*d2;
    // 辺ABの外側で両端の投影範囲にも入っていれば、その辺上の最近点を求める。
    if(vc<=0 && d1>=0 && d3<=0)
        return a+ab*(d1/(d1-d3));
    const V cp=p-c;
    const float d5=Dot(ab,cp),d6=Dot(ac,cp);
    // Cから見てACの先側にあり、BC側へ出ていなければ、頂点Cが最も近い。
    if(d6>=0 && d5<=d6)
        return c;
    // AとCを結ぶ辺でも同じ判定を行い、該当すればその辺へ制限する。
    const float vb=d5*d2-d1*d6;
    // 辺ACの外側で両端に挟まれる場合は、その辺上を補間する。
    if(vb<=0 && d2>=0 && d6<=0)
        return a+ac*(d2/(d2-d6));
    // 残るBとCを結ぶ辺を確認してから、三角形の内部へ進む。
    const float va=d3*d6-d5*d4;
    // 辺BCの外側で両端に挟まれる場合は、その辺上を補間する。
    if(va<=0 && d4-d3>=0 && d5-d6>=0)
        return b+(c-b)*((d4-d3)/((d4-d3)+(d5-d6)));
    // 辺や頂点が最近点となる場合を除いた後、三角形内の重みを正規化して位置を補間する。
    const float inverse=1.0f/(va+vb+vc);
    return a+ab*(vb*inverse)+ac*(vc*inverse);
}

// レイ始点が凸包の内側なら出口を返す。directionの長さを揃える処理は呼び出し側で行う。
inline bool Ray(const std::vector<Face>& hull,const V& start,const V& direction,float limit,float& distance,V& normal) {
    // 全ての外向き平面でレイ区間を切り詰める。側面・底面も同じ経路で調べる。
    float enter=0,exit=limit;
    V enterNormal{},exitNormal{};
    bool inside=true;
    for(const auto& f:hull) {
        const V n=Normal(f);
        const float signedDistance=Dot(n,start-f[0]),speed=Dot(n,direction);
        inside=inside && signedDistance<=1e-5f;
        // レイの法線方向速度がほぼゼロなら、その面と平行として時刻の除算を省く。
        if(std::abs(speed)<1e-7f) {
            // 平行で始点が面の外にあれば、どれだけ進んでも凸包内へ入れない。
            if(signedDistance>1e-5f)
                return false;
            continue;
        }
        // 平面までの符号付き距離を法線方向の速度で割る。負の速度は入射、正は退出の境界を決める。
        const float t=-signedDistance/speed;
        // 外向き法線に逆らって進む場合は、凸包へ入る側の境界として扱う。
        if(speed<0) {
            // すでに記録した入射時刻より遅ければ、この面まで待たないと内部へ入れない。
            if(t>enter){
                enter=t;
                enterNormal=n;
            }
        }
        // 法線と同じ側へ進み、より手前で面に届くなら、その面を退出境界にする。
        else if(t<exit){
            exit=t;
            exitNormal=n;
        }
        // 入射の下限が退出の上限を超えた場合は、全平面の内側を同時に満たせない。
        if(enter>exit)
            return false;
    }
    // 始点が凸包内なら最初に出る面、外なら最初に入る面を接触として返す。
    distance=inside?exit:enter;
    normal=inside?exitNormal:enterNormal;
    return distance>=0 && distance<=limit && Dot(normal,normal)>0.5f;
}

// 凸包と球の重なりを調べ、接触時に深さと法線を返す。
inline bool Sphere(const std::vector<Face>& hull,const V& center,float radius,float& depth,V& normal) {
    // 外側は最近接点、内側は最寄りの面から押し出す。
    float nearest=(std::numeric_limits<float>::max)(),plane=-nearest;
    V closest{},planeNormal{};
    bool inside=true;
    for(const auto& f:hull) {
        const V n=Normal(f);
        const float signedDistance=Dot(n,center-f[0]);
        // 符号付き距離が最も大きい面を、内部から抜ける際の候補として保持する。
        if(signedDistance>plane){
            plane=signedDistance;
            planeNormal=n;
        }
        inside=inside && signedDistance<=0;
        const V q=Closest(center,f);
        const float squared=Dot(center-q,center-q);
        // 三角形までの距離の二乗が最小なら、外側からの最接近点を更新する。
        if(squared<nearest){
            nearest=squared;
            closest=q;
        }
    }
    if(inside){
        // 球が内部にある場合は最寄りの面までの距離に半径を足し、球全体が外へ出る量を返す。
        depth=radius-plane;
        normal=planeNormal;
        return true;
    }
    // 球の中心が凸包外なら、表面の最近点へ届く場合だけ半径との差を押し戻し量にする。
    const float distance=std::sqrt(nearest);
    // 最接近点までの距離が半径以上なら、押し戻す重なりが無い。
    if(distance>=radius)
        return false;
    depth=radius-distance;
    normal=distance>1e-6f?(center-closest)/distance:planeNormal;
    return true;
}

// 頂点・面法線・辺方向は同一空間で渡す。trueのときに重なり深さと法線を採用する。
inline bool Polyhedra(const std::vector<V>& pointsA, const std::vector<V>& normalsA,
                      const std::vector<V>& edgesA, const std::vector<V>& pointsB,
                      const std::vector<V>& normalsB, const std::vector<V>& edgesB,
                      float& depth, V& normal) {
    // キャッシュ済みの頂点・方向を使う。辺の外積を巨大な配列へ集めて
    // 重複比較すると二乗以上の負荷になるため、生成した軸をその場で判定する。
    depth = (std::numeric_limits<float>::max)();
    const auto test = [&](V axis) {
        const float squaredLength = Dot(axis, axis);
        // ゼロに近い外積は分離方向に使えないので、この軸で交差を否定しない。
        if (squaredLength < 1e-12f)
            return true;
        axis = axis / std::sqrt(squaredLength);
        float minA = (std::numeric_limits<float>::max)(), maxA = -minA;
        float minB = minA, maxB = -minB;
        for (const V& point : pointsA) {
            const float value = Dot(point, axis);
            minA = (std::min)(minA, value);
            maxA = (std::max)(maxA, value);
        }
        for (const V& point : pointsB) {
            const float value = Dot(point, axis);
            minB = (std::min)(minB, value);
            maxB = (std::max)(maxB, value);
        }
        // 二つの投影を正と負のどちらへ動かすと離れるか調べ、全軸のうち最小の移動量と向きを残す。
        const float positive = maxA - minB, negative = maxB - minA;
        // いずれかの向きで投影が離れるか接するだけなら、重なりを返さない。
        if (positive <= 0 || negative <= 0)
            return false;
        const float overlap = (std::min)(positive, negative);
        // 現在より小さく動かすだけで分離できる軸なら、押し戻しの深さと向きを更新する。
        if (overlap < depth) {
            depth = overlap;
            normal = positive < negative ? axis : axis * -1.0f;
        }
        return true;
    };
    // 面だけで分離できる組み合わせを先に除外し、辺同士の比較まで進めない。
    for (const V& axis : normalsA)
        // Aの面法線で分離できれば、残りの軸の比較を省く。
        if (!test(axis))
            return false;
    for (const V& axis : normalsB)
        // Bの面法線で分離できる場合も、交差なしで終了する。
        if (!test(axis))
            return false;
    for (const V& edgeA : edgesA)
        for (const V& edgeB : edgesB)
            // 辺の外積で分離軸が見つかれば、交差なしで終了する。
            if (!test(Cross(edgeA, edgeB)))
                return false;
    return !pointsA.empty() && !pointsB.empty();
}

inline bool Box(const std::vector<V>& points, const std::vector<V>& normals, const std::vector<V>& edges,
                const V& center, const V* boxAxes, const V& halfSize, float& depth, V& normal) {
    // 箱の投影幅は3軸の内積から直接計算し、頂点・軸の重複生成を接触ごとに行わない。
    depth=(std::numeric_limits<float>::max)();
    const auto test=[&](V axis) {
        const float length=std::sqrt(Dot(axis,axis));
        // ほぼ長さの無い軸では投影を比較できないため飛ばす。
        if(length<1e-6f)
            return true;
        axis=axis/length;
        float minimum=(std::numeric_limits<float>::max)(),maximum=-minimum;
        for(const V& point:points){
            const float value=Dot(point,axis);
            minimum=(std::min)(minimum,value);
            maximum=(std::max)(maximum,value);
        }
        const float middle=Dot(center,axis);
        // それぞれの箱の軸を判定軸へ射影し、半寸法で重みを付けて中心からの幅を得る。
        const float extent=std::abs(Dot(boxAxes[0],axis))*halfSize.x+
                           std::abs(Dot(boxAxes[1],axis))*halfSize.y+
                           std::abs(Dot(boxAxes[2],axis))*halfSize.z;
        const float positive=maximum-(middle-extent),negative=(middle+extent)-minimum;
        // 凸包と箱の投影が離れるか接するだけなら、この軸で重なりを否定できる。
        if(positive<=0 || negative<=0)
            return false;
        const float overlap=(std::min)(positive,negative);
        // 現在より少ない移動量で離せる場合は、その向きと深さを保存する。
        if(overlap<depth){
            depth=overlap;
            normal=positive<negative?axis:axis*-1.0f;
        }
        return true;
    };
    for(const V& axis:normals)
        // 凸包の面法線で分離できれば、箱の軸を調べず終了する。
        if(!test(axis))
            return false;
    for(std::int32_t i=0;i<3;++i)
        // 箱の三本の軸のいずれかで投影が分離する場合は、交差しない。
        if(!test(boxAxes[i]))
            return false;
    for(const V& edge:edges)
        for(std::int32_t i=0;i<3;++i)
            // 凸包の辺と箱の軸の外積で分離できる場合も、交差しない。
            if(!test(Cross(edge,boxAxes[i])))
                return false;
    return !points.empty();
}
} // namespace convexCollision
