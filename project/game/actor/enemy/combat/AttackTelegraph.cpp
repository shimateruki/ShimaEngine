#define NOMINMAX
#include <cstdint>
#include "game/actor/enemy/combat/AttackTelegraph.h"
#include "Easing.h"
#include <numbers>
#include "Camera.h"
#include "CameraManager.h"
#include "TextureManager.h"

#include <algorithm>
#include <cmath>

namespace {
// 装飾床の縁が接地判定より高い場所でも、予兆面が隠れない高さにします。
constexpr float kGroundLift = 0.13f;
constexpr float kTimingLayerLift = 0.012f;
constexpr float kCircleInnerRate = 0.72f;
constexpr float kCueOuterScale = 1.24f;
constexpr float kHalfPi = (std::numbers::pi_v<float> * 0.5f);
constexpr float kWarningCameraOffset = 0.14f;
constexpr std::size_t kConeSegmentCount = 8;
constexpr float kCountdownOuterScale = 1.72f;
constexpr float kTimingMarkerMinLength = 0.18f;
constexpr float kTimingMarkerMaxLength = 0.78f;
constexpr const char* kAttackWarningEffectPath = "Resources/json/effect/shared/effect_attack_warning_flash.json";
constexpr const char* kAttackWarningTexturePath = "Resources/sprite/effect/attack_warning_flash.dds";
constexpr const char* kWhiteTexturePath = "Resources/sprite/common/white.dds";

engine::Vector3 NormalizePlanar(engine::Vector3 direction) {
    // 目標と同座標でも予告の向きを失わないよう、ゼロ長には固定の前方を使う。
    direction.y = 0.0f;
    const float length = std::sqrt(direction.x * direction.x + direction.z * direction.z);

    // XZの長さがほぼゼロなら、ワールドの正のZ方向へ置き換える。
    if (length <= 0.001f) {
        return {0.0f, 0.0f, 1.0f};
    }

    return {direction.x / length, 0.0f, direction.z / length};
}


} // namespace

void AttackTelegraph::Initialize(engine::Object3DCommon* common) {
    common_ = common;

    // 初回攻撃の瞬間にDDS読込が走らないよう、シーンロード中に共有テクスチャを温めます。
    engine::TextureManager::GetInstance()->Load(kAttackWarningTexturePath);
    EnsureEffect(common_);
}

void AttackTelegraph::EnsureEffect(engine::Object3DCommon* common) {
    // 地面の基本予告は初回だけ生成し、毎フレームの表示で再利用する。
    // 本体を作成済みか3D共通資源をまだ使えない場合は生成しない。
    if (effect_ || !common) {
        return;
    }

    effect_ = std::make_unique<EffectObject3D>();
    effect_->Initialize(common);
    effect_->SetName("AttackTelegraph");
    effect_->SetBlendMode(BlendMode::kAdd);
    effect_->SetEnableReveal(false);
    effect_->SetEnableDistortion(false);
    effect_->SetEnableNoiseTexture(false);
    effect_->SetEnableColorRamp(false);
    effect_->SetIntensity(2.35f);
    effect_->SetEdgeFadeStrength(1.0f);
    effect_->SetAlphaReference(0.0f);
    ConfigureShape(shape_);
    effect_->SetIsVisible(false);
}

void AttackTelegraph::EnsureCueEffect(engine::Object3DCommon* common) {
    // 命中直前の強調表示を本体の予告と別の寿命で管理する。
    // 合図用のオブジェクトがあるか初期化資源を得られない場合は作り直さない。
    if (cueEffect_ || !common) {
        return;
    }

    cueEffect_ = std::make_unique<EffectObject3D>();
    cueEffect_->Initialize(common);
    cueEffect_->SetName("AttackTelegraphCue");
    cueEffect_->SetBlendMode(BlendMode::kAdd);
    cueEffect_->SetEnableReveal(false);
    cueEffect_->SetEnableDistortion(false);
    cueEffect_->SetEnableNoiseTexture(false);
    cueEffect_->SetEnableColorRamp(false);
    cueEffect_->SetIntensity(4.4f);
    cueEffect_->SetEdgeFadeStrength(0.35f);
    cueEffect_->SetAlphaReference(0.0f);
    ConfigureCueShape(cueShape_);
    cueEffect_->SetIsVisible(false);
}

void AttackTelegraph::EnsureWarningEffect(engine::Object3DCommon* common) {
    // 頭上の警告が既にあるか、3D共通資源が無い場合は作成しない。
    if (warningEffect_ || !common) {
        return;
    }

    warningEffect_ = std::make_unique<EffectObject3D>();
    warningEffect_->Initialize(common);
    warningEffect_->SetName("AttackWarningCue");

    // 素材を読み込めなかった場合は、既定の板と画像で警告を表示できるようにする。
    if (!warningEffect_->LoadFromJson(kAttackWarningEffectPath)) {
        // JSONを編集中でも予兆自体が消えないよう、最小構成へフォールバックします。
        warningEffect_->SetBlendMode(BlendMode::kAdd);
        warningEffect_->SetEnableReveal(false);
        warningEffect_->SetEnableDistortion(false);
        warningEffect_->SetEnableNoiseTexture(false);
        warningEffect_->SetIntensity(5.2f);
        warningEffect_->SetEdgeFadeStrength(0.35f);
        warningEffect_->SetAlphaReference(0.008f);
        warningEffect_->SetProceduralType(7);
        warningEffect_->SetMeshSetting(&EffectObject3D::MeshSettings::planeSize, {1.0f, 1.0f});
        warningEffect_->SetMeshSetting(&EffectObject3D::MeshSettings::meshSegments, 1);
        warningEffect_->UpdateProceduralMesh();
        warningEffect_->SetTexture(kAttackWarningTexturePath);
        warningEffect_->SetStartScale({0.52f, 0.52f, 0.52f});
        warningEffect_->SetEndScale({1.18f, 1.18f, 1.18f});
        warningEffect_->SetStartColor({1.0f, 1.0f, 1.0f, 1.0f});
        warningEffect_->SetEndColor({1.0f, 1.0f, 1.0f, 0.0f});
        warningEffect_->SetEasingType(5);
        warningEffect_->Play(0.18f);
    }

    warningEffect_->SetIsVisible(false);
}

void AttackTelegraph::EnsurePatternEffects(std::size_t count) {
    // 複数着弾地点に必要な数だけ表示物を増やし、既存分は再利用する。
    // 3D共通資源がなければ、着弾地点用の表示物を生成できない。
    if (!common_) {
        return;
    }

    // 要求数に足りない分だけ追加し、余った表示物は保持して再利用する。
    while (patternEffects_.size() < count) {
        auto effect = std::make_unique<EffectObject3D>();
        effect->Initialize(common_);
        effect->SetName("AttackTelegraphPattern_" + std::to_string(patternEffects_.size()));
        effect->SetBlendMode(BlendMode::kAdd);
        effect->SetEnableReveal(false);
        effect->SetEnableDistortion(false);
        effect->SetEnableNoiseTexture(false);
        effect->SetEnableColorRamp(false);
        effect->SetIntensity(2.35f);
        effect->SetEdgeFadeStrength(1.0f);
        effect->SetAlphaReference(0.0f);
        ConfigureEffectShape(effect.get(), patternShape_);
        effect->SetIsVisible(false);
        patternEffects_.push_back(std::move(effect));
    }
}

void AttackTelegraph::EnsureTimingEffects(std::size_t count) {
    // 時間表示の数を攻撃パターンへ合わせ、毎フレームの生成を避ける。
    // 3D共通資源がなければ時間表示を新規に作れない。
    if (!common_) {
        return;
    }

    // 既存の表示物を流用し、不足している数だけ生成する。
    while (timingEffects_.size() < count) {
        auto effect = std::make_unique<EffectObject3D>();
        effect->Initialize(common_);
        effect->SetName("AttackTelegraphTiming_" + std::to_string(timingEffects_.size()));
        effect->SetEnableReveal(false);
        effect->SetEnableDistortion(false);
        effect->SetEnableNoiseTexture(false);
        effect->SetEnableColorRamp(false);
        effect->SetAlphaReference(0.0f);
        ConfigureTimingEffectShape(effect.get(), timingShape_);
        effect->SetIsVisible(false);
        timingEffects_.push_back(std::move(effect));
    }
}

void AttackTelegraph::BeginPattern(Shape shape, std::size_t count) {
    // 単体予告を隠してから複数地点の予告へ切り替え、表示の重複を防ぐ。
    if (effect_) {
        effect_->SetIsVisible(false);
    }

    const bool shapeChanged = !isPatternShapeConfigured_ || patternShape_ != shape;
    patternShape_ = shape;
    EnsurePatternEffects(count);

    // 形状を初めて指定したか別の形状へ切り替えた場合は、既存の全表示物の形を揃える。
    if (shapeChanged) {
        for (auto& effect : patternEffects_) {
            ConfigureEffectShape(effect.get(), patternShape_);
        }

        isPatternShapeConfigured_ = true;
    }

    patternActiveCount_ = (std::min)(count, patternEffects_.size());

    for (std::size_t index = 0; index < patternEffects_.size(); ++index) {
        patternEffects_[index]->SetIsVisible(index < patternActiveCount_);
    }

    isVisible_ = patternActiveCount_ > 0;
    hasLastShape_ = false;
}

void AttackTelegraph::BeginTiming(Shape shape, std::size_t count) {
    // 形状変更時だけ描画設定を作り直し、タイミング表示の個数を揃える。
    const bool shapeChanged = !isTimingShapeConfigured_ || timingShape_ != shape;
    timingShape_ = shape;
    EnsureTimingEffects(count);

    // 初回または時間表示の形状が変わる場合だけ、全表示物の形を適用し直す。
    if (shapeChanged) {
        for (auto& effect : timingEffects_) {
            ConfigureTimingEffectShape(effect.get(), timingShape_);
        }

        isTimingShapeConfigured_ = true;
    }

    timingActiveCount_ = (std::min)(count, timingEffects_.size());

    for (std::size_t index = 0; index < timingEffects_.size(); ++index) {
        timingEffects_[index]->SetIsVisible(index < timingActiveCount_);
    }
}

void AttackTelegraph::HidePatternEffects() {
    // 攻撃終了時は前回の地点数と強調時間を破棄する。
    patternActiveCount_ = 0;
    patternCueTimer_ = 0.0f;

    for (auto& effect : patternEffects_) {
        effect->SetIsVisible(false);
    }
}

void AttackTelegraph::HideTimingEffects() {
    // 時間表示が次の攻撃へ残らないよう、再利用物を全て隠す。
    timingActiveCount_ = 0;

    for (auto& effect : timingEffects_) {
        effect->SetIsVisible(false);
    }
}

void AttackTelegraph::ConfigureEffectShape(EffectObject3D* effect, Shape shape) {
    // 円・線・扇で使う描画設定を選び、形状変更時の見た目を揃える。
    if (!effect) {
        return;
    }

    const bool isCue = effect == cueEffect_.get();

    if (shape == Shape::kCircle || shape == Shape::kImpactDisc) {
        // 円環は内側を空け、着弾の面表示は内半径をゼロにして塗りつぶす。
        effect->SetProceduralType(10);
        effect->SetMeshSetting(&EffectObject3D::MeshSettings::ringOuterRadius, 1.0f);
        effect->SetMeshSetting(&EffectObject3D::MeshSettings::ringInnerRadius, shape == Shape::kImpactDisc ? 0.0f : kCircleInnerRate);
        effect->SetMeshSetting(&EffectObject3D::MeshSettings::meshSegments, 64);
        effect->SetTexture(kWhiteTexturePath);
        // 着弾面は通常合成で輪郭を保ち、加算合成の予兆は通常の形状より明るくする。
        if (shape == Shape::kImpactDisc) {
            effect->SetBlendMode(BlendMode::kNormal);
            effect->SetIntensity(0.92f);
            effect->SetEdgeFadeStrength(0.0f);
        } else {
            effect->SetBlendMode(BlendMode::kAdd);
            effect->SetIntensity(isCue ? 4.4f : 2.35f);
            effect->SetEdgeFadeStrength(isCue ? 0.35f : 1.0f);
        }
    } else if (shape == Shape::kDecalCircle) {
        // 円形デカールは既定の形状を作らず、正方形の板へ指定画像を貼る。
        effect->SetProceduralType(7);
        effect->SetMeshSetting(&EffectObject3D::MeshSettings::planeSize, {2.0f, 2.0f});
        effect->SetMeshSetting(&EffectObject3D::MeshSettings::meshSegments, 1);
        effect->SetTexture(decalTexturePath_);
        effect->SetBlendMode(isCue ? BlendMode::kAdd : BlendMode::kNormal);
        effect->SetIntensity(isCue ? 2.25f : 1.16f);
        effect->SetEdgeFadeStrength(0.0f);
    // 円形以外には、線と扇の区間に拡縮できる板を使う。
    } else {
        effect->SetProceduralType(7);
        // 直線や扇形の区間には細長く変形できる白い板を割り当てる。
        effect->SetMeshSetting(&EffectObject3D::MeshSettings::planeSize, {1.0f, 1.0f});
        effect->SetMeshSetting(&EffectObject3D::MeshSettings::meshSegments, 1);
        effect->SetTexture(kWhiteTexturePath);
        effect->SetBlendMode(BlendMode::kAdd);
        effect->SetIntensity(isCue ? 4.4f : 2.35f);
        effect->SetEdgeFadeStrength(isCue ? 0.35f : 1.0f);
    }

    effect->UpdateProceduralMesh();
}

void AttackTelegraph::ConfigureTimingEffectShape(EffectObject3D* effect, Shape shape) {
    // 時間マーカーは攻撃範囲と別の材質・サイズで読めるようにする。
    if (!effect) {
        return;
    }

    ConfigureEffectShape(effect, shape);
    effect->SetBlendMode(BlendMode::kAdd);
    effect->SetIntensity(5.1f);
    effect->SetEdgeFadeStrength(0.22f);

    // 円形の時間マーカーだけは、内径を大きくして範囲表示より細くする。
    if (shape == Shape::kCircle) {
        effect->SetMeshSetting(&EffectObject3D::MeshSettings::ringInnerRadius, 0.86f);
        effect->UpdateProceduralMesh();
    }
}

void AttackTelegraph::ConfigureShape(Shape shape) {
    // 同じ形状を連続表示する間は設定を再適用しない。
    if (!effect_ || (isShapeConfigured_ && shape_ == shape)) {
        return;
    }

    shape_ = shape;
    isShapeConfigured_ = true;
    ConfigureEffectShape(effect_.get(), shape_);
}

void AttackTelegraph::ConfigureCueShape(Shape shape) {
    // 強調表示の形状を予告範囲へ揃え、直前の攻撃形状を引きずらない。
    if (!cueEffect_ || (isCueShapeConfigured_ && cueShape_ == shape)) {
        return;
    }

    cueShape_ = shape;
    isCueShapeConfigured_ = true;
    ConfigureEffectShape(cueEffect_.get(), cueShape_);
}

engine::Vector4 AttackTelegraph::MakeDisplayColor(const engine::Vector4& color, float progress) const {
    // 発生までの進行率を点滅速度と透明度へ反映する。
    progress = std::clamp(progress, 0.0f, 1.0f);
    const float pulse = 0.55f + std::sin(pulseTimer_ * (8.0f + progress * 8.0f)) * 0.45f;
    const float alpha = std::clamp(color.w * (0.42f + progress * 0.42f + pulse * 0.16f), 0.0f, 1.0f);
    return {std::clamp(color.x + progress * 0.18f, 0.0f, 1.0f),
            std::clamp(color.y * (1.0f - progress * 0.22f), 0.0f, 1.0f),
            std::clamp(color.z * (1.0f - progress * 0.20f), 0.0f, 1.0f), alpha};
}

engine::Vector4 AttackTelegraph::MakeDecalColor(const engine::Vector4& color, float progress) const {
    // デカールは地面との重なりを保ちつつ、発生が近いほど目立たせる。
    progress = std::clamp(progress, 0.0f, 1.0f);
    const float pulse = 0.5f + std::sin(pulseTimer_ * (5.0f + progress * 3.0f)) * 0.5f;
    const float alpha = std::clamp(color.w * (0.54f + progress * 0.30f + pulse * 0.10f), 0.0f, 1.0f);
    return {color.x, color.y, color.z, alpha};
}

engine::Vector4 AttackTelegraph::MakeCueColor(const engine::Vector4& color, float progress) const {
    // 命中合図を短時間で消し、次の警告と混ざらないようにする。
    progress = std::clamp(progress, 0.0f, 1.0f);
    const float flash = 1.0f - progress;
    const float alpha = std::clamp(color.w * flash * flash, 0.0f, 1.0f);
    return {std::clamp(color.x + flash * 0.35f, 0.0f, 1.0f), std::clamp(color.y * 0.25f + flash * 0.35f, 0.0f, 1.0f),
            std::clamp(color.z * 0.22f + flash * 0.20f, 0.0f, 1.0f), alpha};
}

void AttackTelegraph::ApplyCircle(EffectObject3D* effect, const engine::Vector3& center, float radius, float progress,
                                  const engine::Vector4& color, float scaleMultiplier) const {
    // 警告範囲の中心は動かさず、進行率によって半径だけ実判定へ近づける。
    // 円の描画物が無ければ表示用の姿勢を適用できない。
    if (!effect) {
        return;
    }

    progress = std::clamp(progress, 0.0f, 1.0f);
    radius = (std::max)(radius, 0.05f);
    const float scaleRate = (0.58f + Easing::OutQuad01(progress) * 0.42f) * (std::max)(scaleMultiplier, 0.01f);

    engine::Vector3 pos = center;
    pos.y += kGroundLift;
    effect->SetTranslate(pos);
    effect->SetRotation({0.0f, 0.0f, 0.0f});
    effect->SetScale({radius * scaleRate, 1.0f, radius * scaleRate});
    effect->SetColor(color);
    effect->UpdateLocalMatrix();
    effect->UpdateWorldMatrix();
}

void AttackTelegraph::ApplyDecalCircle(EffectObject3D* effect, const engine::Vector3& center, float radius, float progress,
                                       const engine::Vector4& color, float scaleMultiplier) const {
    // 地面から僅かに浮かせ、深度競合でデカールがちらつくのを避ける。
    // デカールの描画物が無ければ位置や回転を適用できない。
    if (!effect) {
        return;
    }

    progress = std::clamp(progress, 0.0f, 1.0f);
    radius = (std::max)(radius, 0.05f);
    scaleMultiplier = (std::max)(scaleMultiplier, 0.01f);

    engine::Vector3 position = center;
    position.y += kGroundLift + 0.006f;
    effect->SetTranslate(position);
    effect->SetRotation({0.0f, pulseTimer_ * 0.12f + progress * 0.08f, 0.0f});
    effect->SetScale({radius * scaleMultiplier, 1.0f, radius * scaleMultiplier});
    effect->SetColor(color);
    effect->UpdateLocalMatrix();
    effect->UpdateWorldMatrix();
}

engine::Vector4 AttackTelegraph::MakeTimingColor(const engine::Vector4& color, float progress) const {
    // 発生が近づく後半ほど色を強め、攻撃の直前を判別しやすくする。
    progress = std::clamp(progress, 0.0f, 1.0f);
    const float urgency = std::clamp((progress - 0.56f) / 0.44f, 0.0f, 1.0f);
    const float urgencySmooth = Easing::SmoothStep(urgency);
    const float pulse = 0.5f + std::sin(pulseTimer_ * (10.0f + urgencySmooth * 22.0f)) * 0.5f;
    const float whiteRate = 0.20f + urgencySmooth * 0.56f;
    return {std::clamp(color.x + (1.0f - color.x) * whiteRate, 0.0f, 1.0f),
            std::clamp(color.y + (1.0f - color.y) * whiteRate, 0.0f, 1.0f),
            std::clamp(color.z + (1.0f - color.z) * whiteRate, 0.0f, 1.0f),
            std::clamp(color.w * (0.72f + urgencySmooth * 0.18f + pulse * 0.10f), 0.0f, 1.0f)};
}

void AttackTelegraph::ApplyLine(EffectObject3D* effect, const engine::Vector3& center, const engine::Vector3& direction, float length,
                                float width, float progress, const engine::Vector4& color, float scaleMultiplier) const {
    // 始点を固定したまま前方へ伸ばし、射線の向きを予告から読めるようにする。
    // 射線の描画物が無ければ変形と着色を行えない。
    if (!effect) {
        return;
    }

    progress = std::clamp(progress, 0.0f, 1.0f);
    length = (std::max)(length, 0.05f);
    width = (std::max)(width, 0.05f);
    const engine::Vector3 dir = NormalizePlanar(direction);
    const float yaw = std::atan2(dir.x, dir.z);
    const float scaleRate = (0.50f + Easing::OutQuad01(progress) * 0.50f) * (std::max)(scaleMultiplier, 0.01f);

    engine::Vector3 pos = center + dir * (length * 0.5f * scaleRate);
    pos.y += kGroundLift;
    effect->SetTranslate(pos);
    effect->SetRotation({0.0f, yaw, 0.0f});
    effect->SetScale({width * scaleMultiplier, 1.0f, length * scaleRate});
    effect->SetColor(color);
    effect->UpdateLocalMatrix();
    effect->UpdateWorldMatrix();
}

void AttackTelegraph::ApplyLineTimingMarker(EffectObject3D* effect, const engine::Vector3& origin, const engine::Vector3& direction,
                                            float length, float width, float progress, const engine::Vector4& color) const {
    // 残り時間を線上の移動マーカーに変え、端を越えないよう位置を制限する。
    // マーカーの描画物が無ければ射線用の変形を適用できない。
    if (!effect) {
        return;
    }

    length = (std::max)(length, 0.05f);
    width = (std::max)(width, 0.05f);
    progress = std::clamp(progress, 0.0f, 1.0f);
    const engine::Vector3 normalizedDirection = NormalizePlanar(direction);
    const float markerLength = std::clamp(length * 0.075f, kTimingMarkerMinLength, kTimingMarkerMaxLength);
    const float startDistance =
        std::clamp(length * progress - markerLength * 0.5f, 0.0f, (std::max)(0.0f, length - markerLength));
    ApplyLine(effect, origin + normalizedDirection * startDistance, normalizedDirection, markerLength, width * 1.08f,
              1.0f, color, 1.0f);
}

void AttackTelegraph::ShowCircleTiming(const engine::Vector3& center, float radius, float progress, const engine::Vector4& color) {
    // 予告の外周を攻撃発生時に実半径へ収束させる。
    progress = std::clamp(progress, 0.0f, 1.0f);
    BeginTiming(Shape::kCircle, 1);
    const float countdownScale = kCountdownOuterScale - (kCountdownOuterScale - 1.0f) * progress;
    engine::Vector3 timingCenter = center;
    timingCenter.y += kTimingLayerLift;
    ApplyCircle(timingEffects_[0].get(), timingCenter, radius, 1.0f, MakeTimingColor(color, progress), countdownScale);
}

void AttackTelegraph::ShowLineTiming(const engine::Vector3& origin, const engine::Vector3& direction, float length, float width,
                                     float progress, const engine::Vector4& color) {
    // 射線上を進むマーカーで、線状攻撃の発生までの時間を示す。
    BeginTiming(Shape::kLine, 1);
    engine::Vector3 timingOrigin = origin;
    timingOrigin.y += kTimingLayerLift;
    ApplyLineTimingMarker(timingEffects_[0].get(), timingOrigin, direction, length, width, progress,
                          MakeTimingColor(color, progress));
}

// 円の範囲と残り時間を表示する。半径は呼び出し側が渡し、ここでは当たり判定を更新しない。
void AttackTelegraph::ShowCircle(const engine::Vector3& center, float radius, float progress, const engine::Vector4& color) {
    // 複数地点の表示を解除し、この一箇所の範囲と時間だけ表示する。
    EnsureEffect(common_);

    // 共通オブジェクトを用意できなければ、単独の円を表示しない。
    if (!effect_) {
        return;
    }

    HidePatternEffects();
    ConfigureShape(Shape::kCircle);
    ApplyCircle(effect_.get(), center, radius, 1.0f, MakeDisplayColor(color, progress), 1.0f);
    ShowCircleTiming(center, radius, progress, color);
    effect_->SetIsVisible(true);
    // 直後の強調で同じ位置と半径を再現できるよう、最後の形状を記録する。
    isVisible_ = true;
    hasLastShape_ = true;
    lastShape_ = Shape::kCircle;
    lastCenter_ = center;
    lastRadius_ = radius;
    lastColor_ = color;
}

void AttackTelegraph::ShowDecalCircle(const engine::Vector3& center, float radius, float progress, const engine::Vector4& color,
                                      const std::string& texturePath) {
    // 画像が変わった時だけ形状設定を更新し、古いデカールを使い回さない。
    EnsureEffect(common_);

    // 描画本体またはデカールの画像パスが欠けていれば表示を開始しない。
    if (!effect_ || texturePath.empty()) {
        return;
    }

    HidePatternEffects();

    // 画像が別のものに変わったら、本体と強調の形状設定を次の適用時に作り直す。
    if (decalTexturePath_ != texturePath) {
        decalTexturePath_ = texturePath;
        isShapeConfigured_ = false;
        isCueShapeConfigured_ = false;
    }

    ConfigureShape(Shape::kDecalCircle);
    ApplyDecalCircle(effect_.get(), center, radius, progress, MakeDecalColor(color, progress), 1.0f);
    ShowCircleTiming(center, radius, progress, color);
    effect_->SetIsVisible(true);
    isVisible_ = true;
    hasLastShape_ = true;
    lastShape_ = Shape::kDecalCircle;
    lastCenter_ = center;
    lastRadius_ = radius;
    lastColor_ = color;
}

void AttackTelegraph::ShowLine(const engine::Vector3& center, const engine::Vector3& direction, float length, float width,
                               float progress, const engine::Vector4& color) {
    // 射線の最後の形状を覚え、命中直前の合図を同じ位置へ出せるようにする。
    EnsureEffect(common_);

    // 射線用の共通オブジェクトを用意できなければ表示を開始しない。
    if (!effect_) {
        return;
    }

    HidePatternEffects();
    ConfigureShape(Shape::kLine);
    ApplyLine(effect_.get(), center, direction, length, width, 1.0f, MakeDisplayColor(color, progress), 1.0f);
    ShowLineTiming(center, direction, length, width, progress, color);
    effect_->SetIsVisible(true);
    isVisible_ = true;
    hasLastShape_ = true;
    lastShape_ = Shape::kLine;
    lastCenter_ = center;
    lastDirection_ = NormalizePlanar(direction);
    lastLength_ = length;
    lastWidth_ = width;
    lastColor_ = color;
}

void AttackTelegraph::ShowCone(const engine::Vector3& origin, const engine::Vector3& direction, float length, float startWidth,
                               float endWidth, float progress, const engine::Vector4& color) {
    // 扇形を長さ方向の複数区間で近似し、先端へ向けた幅の変化を見せる。
    length = (std::max)(length, 0.05f);
    startWidth = (std::max)(startWidth, 0.05f);
    endWidth = (std::max)(endWidth, startWidth);
    progress = std::clamp(progress, 0.0f, 1.0f);

    BeginPattern(Shape::kLine, kConeSegmentCount);
    const engine::Vector3 normalizedDirection = NormalizePlanar(direction);
    const float segmentLength = length / static_cast<float>(kConeSegmentCount);
    const engine::Vector4 displayColor = MakeDisplayColor(color, progress);

    // 奥の区間ほど幅と不透明度を増し、隙間が出ない長さで板をつなぐ。
    for (std::size_t index = 0; index < kConeSegmentCount; ++index) {
        EffectObject3D* segment = patternEffects_[index].get();
        const float startRate = static_cast<float>(index) / static_cast<float>(kConeSegmentCount);
        const float centerRate = (static_cast<float>(index) + 0.5f) / static_cast<float>(kConeSegmentCount);
        const float segmentWidth = startWidth + (endWidth - startWidth) * centerRate;
        engine::Vector4 segmentColor = displayColor;
        segmentColor.w *= 0.72f + centerRate * 0.28f;
        ApplyLine(segment, origin + normalizedDirection * (length * startRate), normalizedDirection,
                  segmentLength * 1.08f, segmentWidth, 1.0f, segmentColor, 1.0f);
        segment->SetIsVisible(true);
    }

    const float timingWidth = startWidth + (endWidth - startWidth) * progress;
    ShowLineTiming(origin, normalizedDirection, length, timingWidth, progress, color);
}

void AttackTelegraph::ShowImpactAreas(const engine::Vector3* centers, std::size_t centerCount, float radius, float progress,
                                      const engine::Vector4& color) {
    // 複数の着弾地点を同じ進行率で示し、各地点に時間表示を重ねる。
    if (!centers || centerCount == 0) {
        Hide();
        return;
    }

    radius = (std::max)(radius, 0.05f);
    progress = std::clamp(progress, 0.0f, 1.0f);
    BeginPattern(Shape::kCircle, centerCount);
    BeginTiming(Shape::kCircle, centerCount);
    const engine::Vector4 displayColor = MakeDisplayColor(color, progress);
    // 予告ごとに時刻の表示をずらし、先に着弾する地点から外周を絞る。
    const float sequenceStep = centerCount > 1 ? (std::min)(0.105f, 0.38f / static_cast<float>(centerCount - 1)) : 0.0f;

    for (std::size_t index = 0; index < centerCount; ++index) {
        const float localProgress = std::clamp(progress - static_cast<float>(index) * sequenceStep, 0.0f, 1.0f);
        EffectObject3D* area = patternEffects_[index].get();
        engine::Vector4 areaColor = displayColor;
        areaColor.w *= 0.68f + localProgress * 0.32f;
        ApplyCircle(area, centers[index], radius, 1.0f, areaColor, 1.0f);
        area->SetIsVisible(true);

        // 各地点の外周は床の円より少し上に置き、着弾の順で実半径へ縮める。
        const float countdownScale = kCountdownOuterScale - (kCountdownOuterScale - 1.0f) * localProgress;
        engine::Vector3 timingCenter = centers[index];
        timingCenter.y += kTimingLayerLift;
        ApplyCircle(timingEffects_[index].get(), timingCenter, radius, 1.0f, MakeTimingColor(color, localProgress),
                    countdownScale);
    }
}

void AttackTelegraph::ShowReadableImpactAreas(const engine::Vector3* centers, std::size_t centerCount, float radius,
                                              float progress, const engine::Vector4& color) {
    // 着弾点が一つもなければ、前フレームの範囲と時計を隠す。
    if (!centers || centerCount == 0) {
        Hide();
        return;
    }

    radius = (std::max)(radius, 0.05f);
    progress = std::clamp(progress, 0.0f, 1.0f);
    BeginPattern(Shape::kImpactDisc, centerCount);
    BeginTiming(Shape::kCircle, centerCount);

    const float sequenceStep = centerCount > 1 ? (std::min)(0.072f, 0.32f / static_cast<float>(centerCount - 1)) : 0.0f;

    // 各地点へ順に遅れを与え、面の色と時間表示の強さを個別に決める。
    for (std::size_t index = 0; index < centerCount; ++index) {
        const float localProgress = std::clamp(progress - static_cast<float>(index) * sequenceStep, 0.0f, 1.0f);
        const float urgencyT = std::clamp((localProgress - 0.58f) / 0.42f, 0.0f, 1.0f);
        const float urgency = Easing::SmoothStep(urgencyT);

        // 面は攻撃範囲だけを静かに伝え、重なっても白飛びしない通常合成にします。
        engine::Vector4 discColor = color;
        discColor.x = std::clamp(discColor.x * 0.82f, 0.0f, 1.0f);
        discColor.y = std::clamp(discColor.y * 0.54f, 0.0f, 1.0f);
        discColor.z = std::clamp(discColor.z * 0.48f, 0.0f, 1.0f);
        discColor.w = std::clamp(color.w * (0.16f + urgency * 0.10f), 0.0f, 0.34f);
        ApplyCircle(patternEffects_[index].get(), centers[index], radius, 1.0f, discColor, 1.0f);

        // 外周は着弾時だけ実半径へ収束し、順番も明るさで判別できるようにします。
        const float countdownScale = 1.46f - 0.46f * localProgress;
        engine::Vector4 timingColor = MakeTimingColor(color, localProgress);
        timingColor.w *= 0.38f + localProgress * 0.54f;
        engine::Vector3 timingCenter = centers[index];
        timingCenter.y += kTimingLayerLift;
        ApplyCircle(timingEffects_[index].get(), timingCenter, radius, 1.0f, timingColor, countdownScale);
    }
}

void AttackTelegraph::ShowLaneFan(const engine::Vector3& origin, const engine::Vector3& direction, float length, float width,
                                  std::int32_t laneCount, float lateralSpacing, float angleStep, float progress,
                                  const engine::Vector4& color) {
    // 本数の中央を基準に左右へ射線を広げ、入力順で見た目が偏らないようにする。
    laneCount = (std::max)(1, laneCount);
    length = (std::max)(length, 0.05f);
    width = (std::max)(width, 0.05f);
    progress = std::clamp(progress, 0.0f, 1.0f);

    BeginPattern(Shape::kLine, static_cast<std::size_t>(laneCount));
    BeginTiming(Shape::kLine, static_cast<std::size_t>(laneCount));
    const engine::Vector3 baseDirection = NormalizePlanar(direction);
    const engine::Vector3 side = {baseDirection.z, 0.0f, -baseDirection.x};
    const float centerIndex = static_cast<float>(laneCount - 1) * 0.5f;
    const engine::Vector4 displayColor = MakeDisplayColor(color, progress);

    // 中央からの距離を基準に横へずらし、射線の向きも左右対称に回す。
    for (std::int32_t index = 0; index < laneCount; ++index) {
        const float centeredIndex = static_cast<float>(index) - centerIndex;
        const float angle = centeredIndex * angleStep;
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        const engine::Vector3 laneDirection = NormalizePlanar({
            baseDirection.x * cosine + baseDirection.z * sine,
            0.0f,
            -baseDirection.x * sine + baseDirection.z * cosine,
        });
        ApplyLine(patternEffects_[static_cast<std::size_t>(index)].get(),
                  origin + side * (centeredIndex * lateralSpacing), laneDirection, length, width, 1.0f, displayColor,
                  1.0f);
        ApplyLineTimingMarker(timingEffects_[static_cast<std::size_t>(index)].get(),
                              origin + side * (centeredIndex * lateralSpacing) + engine::Vector3{0.0f, kTimingLayerLift, 0.0f},
                              laneDirection, length, width, progress, MakeTimingColor(color, progress));
    }
}

void AttackTelegraph::TriggerCue(const engine::Vector4& color) {
    // 直近の範囲形状を使って、攻撃が始まる瞬間だけ別の強調表示を出す。
    // 複数の地点が有効なら、単独用の強調を作らず共通の点滅タイマーを開始する。
    if (patternActiveCount_ > 0) {
        patternCueTimer_ = cueDuration_;
        patternCueColor_ = color;
        return;
    }

    // 最後に表示した範囲の記録が無ければ、単独の強調をどこへ出すか決められない。
    if (!hasLastShape_) {
        return;
    }

    EnsureCueEffect(common_);

    // 専用の表示物を用意できなければ強調を発生させない。
    if (!cueEffect_) {
        return;
    }

    cueTimer_ = cueDuration_;
    cueColor_ = color;
    ConfigureCueShape(lastShape_);
    const engine::Vector4 cueColor = MakeCueColor(color, 0.0f);

    // 最後の範囲が円なら、同じ中心と半径に重ねて強調を開始する。
    if (lastShape_ == Shape::kCircle) {
        ApplyCircle(cueEffect_.get(), lastCenter_, lastRadius_, 1.0f, cueColor, 1.0f);
    // 画像を使う円なら、同じ画像と中心を使って強調する。
    } else if (lastShape_ == Shape::kDecalCircle) {
        ApplyDecalCircle(cueEffect_.get(), lastCenter_, lastRadius_, 1.0f, cueColor, 1.0f);
    // 単独の残りの種類は射線として、開始方向・長さ・幅を再利用する。
    } else {
        ApplyLine(cueEffect_.get(), lastCenter_, lastDirection_, lastLength_, lastWidth_, 1.0f, cueColor, 1.0f);
    }

    cueEffect_->SetIsVisible(true);
}

void AttackTelegraph::TriggerCueAt(const engine::Vector3& center, float radius, const engine::Vector4& color) {
    // 汎用の円形合図は、事前の予告がなくても指定地点に生成できる。
    EnsureCueEffect(common_);

    // 強調用の表示物が準備できなければ、位置を登録せず終了する。
    if (!cueEffect_) {
        return;
    }

    cueTimer_ = cueDuration_;
    cueColor_ = color;
    hasLastShape_ = true;
    lastShape_ = Shape::kCircle;
    lastCenter_ = center;
    lastRadius_ = (std::max)(radius, 0.05f);
    lastColor_ = color;

    ConfigureCueShape(Shape::kCircle);
    ApplyCircle(cueEffect_.get(), lastCenter_, lastRadius_, 1.0f, MakeCueColor(color, 0.0f), 1.0f);
    cueEffect_->SetIsVisible(true);
}

void AttackTelegraph::TriggerWarningCue(engine::Object3D* target, const engine::Vector3& offset, float size,
                                        const engine::Vector4& accentColor) {
    // 対象へ追従する警告を開始し、移動中も本体の近くに保持する。
    EnsureWarningEffect(common_);

    // 頭上の警告素材を準備できない場合は追従を開始しない。
    if (!warningEffect_) {
        return;
    }

    warningTarget_ = target;
    warningOffset_ = offset;
    warningCenter_ = target ? target->GetWorldPosition() + offset : offset;
    warningSize_ = (std::max)(size, 0.1f);
    warningAccentColor_ = {std::clamp(accentColor.x, 0.0f, 1.0f), std::clamp(accentColor.y, 0.0f, 1.0f),
                           std::clamp(accentColor.z, 0.0f, 1.0f), std::clamp(accentColor.w, 0.0f, 1.0f)};
    warningEffect_->Restart();
    warningEffect_->SetIsVisible(true);
}

void AttackTelegraph::Hide() {
    // 地面の範囲と時間の表示、複数地点の点滅を隠す。単独の合図と頭上の警告は個別の寿命で消える。
    isVisible_ = false;

    // 単独範囲の表示物が存在する場合は非表示にする。
    if (effect_) {
        effect_->SetIsVisible(false);
    }

    HidePatternEffects();
    HideTimingEffects();
}

void AttackTelegraph::Update(float deltaTime) {
    // 表示中だけ各エフェクトの位置と点滅を進め、非表示物の更新を抑える。
    pulseTimer_ += (std::max)(deltaTime, 0.0f);

    // 単独範囲の表示が有効なときだけ、その行列を最新にする。
    if (effect_ && isVisible_) {
        effect_->UpdateLocalMatrix();
        effect_->UpdateWorldMatrix();
    }

    for (std::size_t index = 0; index < patternActiveCount_; ++index) {
        // 有効枠のうち、実際に表示中の範囲だけ行列を更新する。
        if (patternEffects_[index]->GetIsVisible()) {
            patternEffects_[index]->UpdateLocalMatrix();
            patternEffects_[index]->UpdateWorldMatrix();
        }
    }

    for (std::size_t index = 0; index < timingActiveCount_; ++index) {
        // 有効枠のうち、実際に表示中の時間マーカーだけ行列を更新する。
        if (timingEffects_[index]->GetIsVisible()) {
            timingEffects_[index]->UpdateLocalMatrix();
            timingEffects_[index]->UpdateWorldMatrix();
        }
    }

    // 複数地点の合図は各地点の色に重ね、専用のエフェクトを追加しない。
    if (patternCueTimer_ > 0.0f && patternActiveCount_ > 0) {
        patternCueTimer_ = (std::max)(0.0f, patternCueTimer_ - (std::max)(deltaTime, 0.0f));
        const float progress = 1.0f - std::clamp(patternCueTimer_ / cueDuration_, 0.0f, 1.0f);
        const engine::Vector4 cueColor = MakeCueColor(patternCueColor_, progress);

        for (std::size_t index = 0; index < patternActiveCount_; ++index) {
            // 範囲が現在表示されている場合だけ、点滅中の色を重ねる。
            if (patternEffects_[index]->GetIsVisible()) {
                patternEffects_[index]->SetColor(cueColor);
            }
        }
    }

    // 単一地点の合図は最後の形状に沿って広げながら消す。
    if (cueEffect_ && cueTimer_ > 0.0f && hasLastShape_) {
        cueTimer_ = (std::max)(0.0f, cueTimer_ - (std::max)(deltaTime, 0.0f));
        const float progress = 1.0f - std::clamp(cueTimer_ / cueDuration_, 0.0f, 1.0f);
        const float scaleMultiplier = 1.0f + (kCueOuterScale - 1.0f) * Easing::OutQuad01(progress);
        const engine::Vector4 cueColor = MakeCueColor(cueColor_, progress);

        // 円の強調は範囲の中心を維持し、タイマーに沿って半径を拡げる。
        if (lastShape_ == Shape::kCircle) {
            ApplyCircle(cueEffect_.get(), lastCenter_, lastRadius_, 1.0f, cueColor, scaleMultiplier);
        // デカールは回転と高さの補正を保ったまま拡げる。
        } else if (lastShape_ == Shape::kDecalCircle) {
            ApplyDecalCircle(cueEffect_.get(), lastCenter_, lastRadius_, 1.0f, cueColor, scaleMultiplier);
        // 射線は開始点と向きを維持しながら拡げる。
        } else {
            ApplyLine(cueEffect_.get(), lastCenter_, lastDirection_, lastLength_, lastWidth_, 1.0f, cueColor,
                      scaleMultiplier);
        }

        cueEffect_->SetIsVisible(cueTimer_ > 0.0f);
    // 強調を進める条件がなくなった場合は、表示だけ残らないように隠す。
    } else if (cueEffect_) {
        cueEffect_->SetIsVisible(false);
    }

    // 頭上の警告は地面用の点滅と別の再生時間で更新する。
    UpdateWarningEffect(deltaTime);
}

void AttackTelegraph::UpdateWarningEffect(float deltaTime) {
    // 頭上の警告が未作成または非表示なら、再生と姿勢を進めない。
    if (!warningEffect_ || !warningEffect_->GetIsVisible()) {
        return;
    }

    warningEffect_->Update((std::max)(deltaTime, 0.0f));
    const float progress = warningEffect_->GetPlaybackProgress();

    // 素材が最後まで進んだら、位置合わせする前に表示を終える。
    if (!warningEffect_->IsPlaying() && progress >= 1.0f) {
        warningEffect_->SetIsVisible(false);
        return;
    }

    const engine::Vector3 presetScale = warningEffect_->GetScale();
    const auto* material = warningEffect_->GetMaterialData();
    const engine::Vector4 presetColor = material ? material->color : engine::Vector4{1.0f, 1.0f, 1.0f, 1.0f - progress};

    // 最初は全敵共通の白金色、消える直前だけ属性色を残して判別しやすくします。
    const float accentRate = progress * progress;
    const engine::Vector4 commonColor = {1.0f, 0.96f, 0.64f, 1.0f};
    const engine::Vector4 tint = {commonColor.x + (warningAccentColor_.x - commonColor.x) * accentRate,
                          commonColor.y + (warningAccentColor_.y - commonColor.y) * accentRate,
                          commonColor.z + (warningAccentColor_.z - commonColor.z) * accentRate, warningAccentColor_.w};

    // 対象の移動に追従してから、警告の板をカメラへ向けて少し手前に出す。
    warningCenter_ = warningTarget_ ? warningTarget_->GetWorldPosition() + warningOffset_ : warningCenter_;
    engine::Vector3 position = warningCenter_;
    float yaw = 0.0f;
    float pitch = 0.0f;
    engine::Camera* camera = CameraManager::GetInstance() ? CameraManager::GetInstance()->GetActiveCamera() : nullptr;

    // 描画用のカメラを取得できる場合は、板を視点へ向ける。
    if (camera) {
        engine::Vector3 toCamera = camera->GetEye() - position;
        const float length = std::sqrt(toCamera.x * toCamera.x + toCamera.y * toCamera.y + toCamera.z * toCamera.z);

        // 視点までの距離が十分あるときだけ、視点側へずらして向きを求める。
        if (length > 0.001f) {
            toCamera = toCamera / length;
            position = position + toCamera * kWarningCameraOffset;
            yaw = std::atan2(toCamera.x, toCamera.z);
            const float planarLength = std::sqrt(toCamera.x * toCamera.x + toCamera.z * toCamera.z);
            pitch = std::atan2(toCamera.y, (std::max)(planarLength, 0.001f));
        }
    }

    // 素材のその時点の拡縮と色に、警告の指定サイズと属性色を重ねて描画姿勢を確定する。
    warningEffect_->SetTranslate(position);
    warningEffect_->SetRotation({kHalfPi - pitch, yaw, 0.0f});
    warningEffect_->SetScale({presetScale.x * warningSize_, presetScale.y, presetScale.z * warningSize_});
    warningEffect_->SetColor(
        {presetColor.x * tint.x, presetColor.y * tint.y, presetColor.z * tint.z, presetColor.w * tint.w});
    warningEffect_->UpdateLocalMatrix();
    warningEffect_->UpdateWorldMatrix();
}

void AttackTelegraph::DrawGround(ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource) {
    // 地面用の範囲と時間表示だけを描画し、頭上の警告とは描画順を分ける。
    if (effect_ && isVisible_ && effect_->GetIsVisible()) {
        effect_->RefreshRenderCameraData();
        effect_->Draw(pointLightResource, spotLightResource);
    }

    for (std::size_t index = 0; index < patternActiveCount_; ++index) {
        // 複数地点のうち現在見えている範囲だけ、カメラ定数を更新して描く。
        if (patternEffects_[index]->GetIsVisible()) {
            patternEffects_[index]->RefreshRenderCameraData();
            patternEffects_[index]->Draw(pointLightResource, spotLightResource);
        }
    }

    for (std::size_t index = 0; index < timingActiveCount_; ++index) {
        // 時間マーカーは表示中のものだけ、範囲の後に描き足す。
        if (timingEffects_[index]->GetIsVisible()) {
            timingEffects_[index]->RefreshRenderCameraData();
            timingEffects_[index]->Draw(pointLightResource, spotLightResource);
        }
    }

    // 単独の強調が残っている場合は、範囲と時間の後に描く。
    if (cueEffect_ && cueEffect_->GetIsVisible()) {
        cueEffect_->RefreshRenderCameraData();
        cueEffect_->Draw(pointLightResource, spotLightResource);
    }
}

void AttackTelegraph::DrawWarning(ID3D12Resource* pointLightResource, ID3D12Resource* spotLightResource) {
    // 頭上の警告は地面の予告とは別パスで描く。
    if (warningEffect_ && warningEffect_->GetIsVisible()) {
        warningEffect_->RefreshRenderCameraData();
        warningEffect_->Draw(pointLightResource, spotLightResource);
    }
}
