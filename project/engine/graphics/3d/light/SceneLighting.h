#pragma once

struct ID3D12Resource;

namespace engine {

// 描画時の点光源とスポットライトを借り、既存の描画関数へ渡す。バッファはLightManagerが所有する。
class SceneLighting {
public:
    // 描画を始める時点で一度取得し、その描画の呼び出し中だけ使う。
    static SceneLighting Capture();

    // 指定した描画関数へ同じライトを渡す。仮想関数なら対象の派生実装を呼ぶ。
    template<class Target, class Drawable>
    void Draw(Target& target, void (Drawable::*draw)(ID3D12Resource*, ID3D12Resource*)) const {
        (target.*draw)(pointLight_, spotLight_);
    }

private:
    SceneLighting(ID3D12Resource* pointLight, ID3D12Resource* spotLight)
        : pointLight_(pointLight), spotLight_(spotLight) {}

    ID3D12Resource* pointLight_;
    ID3D12Resource* spotLight_;
};

} // namespace engine
