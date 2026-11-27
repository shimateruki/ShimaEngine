#pragma once

#include <string_view>

// Object3Dに付ける機能の共通基底型。派生機能を保存・復元するときの種別IDを定義する。
// 関連：Object3D（保持）、PathMoverComponent / ParticleEmitterComponent（派生）
// 主な入口：GetTypeId
class ObjectComponent {
public:
    virtual ~ObjectComponent() = default;

    /// <summary>保存と実行時の型判定に使う安定した種別IDを返す。</summary>
    virtual std::string_view GetTypeId() const = 0;
};
