#include <cstdint>
#include "GimmickBlinkBlock.h"
#include "Easing.h"
#include <numbers>
#include "ObjectPropertyDefinition.h"
#include "CollisionConfig.h"
#include "Model.h"
#include "ModelManager.h"
#include "Player.h"
#include "SceneManager.h"
#include <algorithm>
#include <cmath>

namespace {
constexpr float kPi = std::numbers::pi_v<float>;
constexpr std::int32_t kFlipFrameCount = 24;
constexpr const char* kFlipModelPrefix = "Stages/jump_flip_pair/jump_flip_pair_";

// 停止姿勢用の両端モデルと、回転途中の連番モデルの名前を組み立てる。
std::string FlipFrameModelName(std::int32_t frame) {
    // 開始コマ以下の番号では、青側で停止したモデルを使う。
    if (frame <= 0)
        return std::string(kFlipModelPrefix) + "blue_preview.obj";
    // 最終コマ以降は赤側の停止姿勢を使う。
    if (frame >= kFlipFrameCount)
        return std::string(kFlipModelPrefix) + "red_preview.obj";
    return std::string(kFlipModelPrefix) + "flip_" + (frame < 10 ? "0" : "") +
           std::to_string(frame) + ".obj";
}

// 停止姿勢のモデルから、回転板だけを床とするローカル座標のOBBを組み立てる。
engine::ColliderConfig BuildFlipPairCollider(const engine::Model* model) {
    // OBJのZ座標は読込時に反転するため、描画に使う変換済み頂点から板の位置を得る。
    // 固定枠・軸・模様より大きい、上向きの水平三角形が回転板の天面に相当する。
    engine::ColliderConfig config;
    config.type = engine::ColliderType::kNone;
    // モデルを取得できない場合は床判定を作れないため、種類をNoneのまま返す。
    if (!model)
        return config;

    // 上向きでほぼ水平な三角形のうち最大のものを選び、XZ の範囲と高さを記録します。
    float largestArea = 0.0f;
    engine::Vector3 minimum{}, maximum{};
    for (const auto& mesh : model->GetModelData().meshes) {
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const auto& a = mesh.vertices[mesh.indices[i]];
            const auto& b = mesh.vertices[mesh.indices[i + 1]];
            const auto& c = mesh.vertices[mesh.indices[i + 2]];
            const float lowY = (std::min)({a.position.y, b.position.y, c.position.y});
            const float highY = (std::max)({a.position.y, b.position.y, c.position.y});
            // 頂点間の高低差が0.001を超えるか、3頂点のY法線の合計が2.7未満なら、板の天面候補から除外する。
            if (highY - lowY > 0.001f || a.normal.y + b.normal.y + c.normal.y < 2.7f)
                continue;
            const float area = std::abs((b.position.x - a.position.x) * (c.position.z - a.position.z) -
                                        (b.position.z - a.position.z) * (c.position.x - a.position.x));
            // 同面積以下の候補では、現在選んでいる天面の範囲を更新しない。
            if (area <= largestArea)
                continue;
            largestArea = area;
            minimum = {(std::min)({a.position.x, b.position.x, c.position.x}), lowY,
                       (std::min)({a.position.z, b.position.z, c.position.z})};
            maximum = {(std::max)({a.position.x, b.position.x, c.position.x}), highY,
                       (std::max)({a.position.z, b.position.z, c.position.z})};
        }
    }
    // 水平な天面を見つけられなければOBBを生成しない。
    if (largestArea <= 0.0f)
        return config;

    // 天面上の薄い色面や模様の高さも取り込み、見た目に足が埋まらないようにする。
    const float deckPlaneY = maximum.y;
    for (const auto& mesh : model->GetModelData().meshes) {
        for (const auto& vertex : mesh.vertices) {
            const auto& p = vertex.position;
            // 天面候補のXZ範囲内かつ基準面から上へ0.1以内の頂点だけを床の上端に反映する。
            if (p.x >= minimum.x && p.x <= maximum.x && p.z >= minimum.z && p.z <= maximum.z &&
                p.y >= deckPlaneY && p.y <= deckPlaneY + 0.1f)
                maximum.y = (std::max)(maximum.y, p.y);
        }
    }
    // 上端を求めた高さに合わせ、全高0.25のOBBを天面の下に置く。
    config.type = engine::ColliderType::kObb;
    config.center = {(minimum.x + maximum.x) * 0.5f, maximum.y - 0.125f,
                     (minimum.z + maximum.z) * 0.5f};
    config.size = {(maximum.x - minimum.x) * 0.5f, 0.125f, (maximum.z - minimum.z) * 0.5f};
    return config;
}
} // namespace

// 破棄後にジャンプ通知が届かないよう、登録した購読を解除する。
GimmickBlinkBlock::~GimmickBlinkBlock() {
    EventManager::GetInstance()->Unsubscribe(jumpSubscription_);
}

// 通常モデルの初期表示を確定し、ジャンプ通知と共通の編集項目を登録する。
void GimmickBlinkBlock::Initialize(engine::Object3DCommon* common, const std::string& modelName) {
    BaseGimmick::Initialize(common, modelName);
    SetClassName("Gimmick");
    SetGimmickType("BlinkBlock");

    // 再初期化時は以前の購読を解除し、一回のジャンプで同じ個体が二度通知されないようにする。
    EventManager::GetInstance()->Unsubscribe(jumpSubscription_);
    jumpSubscription_ = EventManager::GetInstance()->Subscribe([this](const PlayerJumpEvent& e) { OnPlayerJump(e); });

    // 初期化時点ですでに色の設定を持つ場合だけ既定値を置き換える。
    if (GetParameters().has_value()) {
        colorType_ = GetParameters()->colorType;
    }

    // ジャンプ前は、偶数回に対応する色のブロックだけを有効にして最初の表示と衝突を決める。
    isActive_ = (0 % 2 == colorType_);
    UpdateAppearance();

    // モデル共通の初期面と、反転板モデルだけで使う速度を既存の保存キーに結び付ける。
    using Self = GimmickBlinkBlock;
    engine::ObjectPropertyDefinition<Self> properties(*this, kTypeId, "点滅・ジャンプ反転ブロック",
        "反転板モデルは枠の中央を軸に回転し、着地側の板だけが床になります。通常モデルはジャンプ回数で出現面を切り替えます。");
    using Parameter = engine::Object3D::EntityParameter;
    properties.Parameter("初期の色・反転面", "colorType", &Parameter::colorType, 0.0f, 1.0f,
        nullptr, {"青（ジャンプ偶数回）", "赤（ジャンプ奇数回）"});
    properties.Parameter("反転速度倍率", "speed", &Parameter::speed, 0.25f, 3.0f,
        +[](const Self& object) { return object.GetModelName().rfind("Stages/jump_flip_pair/jump_flip_pair_", 0) == 0; });
}

// モデルに応じた表示と床判定を更新してから、共通の毎フレーム処理を行う。
void GimmickBlinkBlock::Update(float deltaTime) {
    // 反転板は専用モデルとコライダーを同期させるため、通常の透明度更新を通しません。
    if (IsFlipPairModel()) {
        UpdateFlipPair(deltaTime);
        BaseGimmick::Update(deltaTime);
        return;
    }

    // Scene JSONはInitialize後に適用されるため、初回Updateで色種別と初期ON/OFFを同期する。
    // 青(0)は既定値と一致して変更検知だけでは再描画されないので、見た目は毎フレーム確定する。
    if (GetParameters().has_value() && GetParameters()->colorType != colorType_) {
        colorType_ = GetParameters()->colorType;
        isActive_ = (colorType_ == 0);
    }

    UpdateAppearance();
    BaseGimmick::Update(deltaTime);
}

// ジャンプ回数の偶奇を基準に、反転板の目標面または通常モデルの有効状態を切り替える。
void GimmickBlinkBlock::OnPlayerJump(const PlayerJumpEvent& event) {
    // プレイヤーを取得でき、モデルが反転板の連番系統なら目標角度を更新する。
    if (event.player && IsFlipPairModel()) {
        // 初回Updateより前にジャンプを受け取った場合でも、初期面と端点の床判定を先に用意する。
        if (!flipPairInitialized_)
            ResetFlipPair();
        // シーン管理を取得できる場合は現在の再生状態を記録し、次のUpdateで不要な再初期化を避ける。
        if (const SceneManager* sceneManager = SceneManager::GetInstance())
            flipPairWasPlaying_ = sceneManager->IsPlaying();

        // 連続ジャンプ時は現在の板角度から目標へ向かい直し、角度の飛びを防ぐ。
        const std::int32_t targetSide = (static_cast<std::int32_t>(event.player->GetJumpCount() & 1u) ^ flipPairInitialSide_);
        const float targetAngle = targetSide == 0 ? 0.0f : kPi;
        // 新しい目標が現在の目標から0.001ラジアンを超えて変わる場合だけ回転を開始し直す。
        if (std::abs(targetAngle - flipTargetAngle_) > 0.001f) {
            flipFromAngle_ = flipAngle_;
            flipTargetAngle_ = targetAngle;
            flipElapsed_ = 0.0f;
            const float speed = std::clamp(GetParameters().has_value() ? GetParameters()->speed : 1.0f, 0.25f, 3.0f);
            flipDuration_ = (std::max)(0.16f, 0.48f * std::abs(flipTargetAngle_ - flipFromAngle_) / kPi / speed);
            // 移動先の板はジャンプ開始時から着地可能にする。アニメーション中に床判定を消すと、
            // 着地が先行したフレームでプレイヤーが板を通り抜けてしまう。
            ApplyFlipPairAppearance();
        }
        return;
    }

    // プレイヤーのジャンプ回数(0, 1, 2...)に合わせて状態を切り替える
    // 0回目: 青ON/赤OFF, 1回目: 青OFF/赤ON ... という同期をとる
    if (event.player) {
        uint32_t count = event.player->GetJumpCount();
        bool shouldBeActive = (count % 2 == colorType_);

        // 偶奇に対応する有効状態が変わったときに、床判定と透明度を切り替える。
        if (isActive_ != shouldBeActive) {
            isActive_ = shouldBeActive;
            UpdateAppearance();
        }
    }
}

// モデル名が反転板専用の接頭辞から始まるかを判定する。
bool GimmickBlinkBlock::IsFlipPairModel() const {
    return GetModelName().rfind(kFlipModelPrefix, 0) == 0;
}

// 端点の床判定を用意し、回転の途中状態を保存設定の初期面に戻す。
void GimmickBlinkBlock::ResetFlipPair() {
    // 端点モデルのOBBはこのインスタンスの初回リセット時にだけ構築する。
    if (!flipPairInitialized_) {
        // 編集画面で初めて表示する前に全コマを読み込み、再生中の読み込み停止を避ける。
        for (std::int32_t frame = 0; frame <= kFlipFrameCount; ++frame)
            ModelManager::GetInstance()->LoadModel(FlipFrameModelName(frame));
        flipPairColliders_[0] = BuildFlipPairCollider(ModelManager::GetInstance()->LoadModel(FlipFrameModelName(0)));
        flipPairColliders_[1] = BuildFlipPairCollider(ModelManager::GetInstance()->LoadModel(FlipFrameModelName(kFlipFrameCount)));
    }
    // 保存された初期面を始点・現在値・目標の全てに使い、途中角度やモデル番号を持ち越しません。
    flipPairInitialSide_ = GetParameters().has_value() && GetParameters()->colorType == 1 ? 1 : 0;
    flipAngle_ = flipPairInitialSide_ == 0 ? 0.0f : kPi;
    flipFromAngle_ = flipAngle_;
    flipTargetAngle_ = flipAngle_;
    flipElapsed_ = 0.0f;
    flipFrame_ = -1;
    flipPairInitialized_ = true;
    ApplyFlipPairAppearance();
}

// 初期面や再生状態の変更を反映し、再生中は秒単位の経過時間で回転を進める。
void GimmickBlinkBlock::UpdateFlipPair(float deltaTime) {
    const SceneManager* sceneManager = SceneManager::GetInstance();
    const bool playing = sceneManager && sceneManager->IsPlaying();
    const std::int32_t configuredSide = GetParameters().has_value() && GetParameters()->colorType == 1 ? 1 : 0;
    // 再生の切り替えや編集時の初期面変更を検出し、古い回転状態を引き継がない。
    if (!flipPairInitialized_ || playing != flipPairWasPlaying_ ||
        (!playing && configuredSide != flipPairInitialSide_)) {
        ResetFlipPair();
    }
    flipPairWasPlaying_ = playing;

    // 再生中に面を切り替えるときだけ角度を補間し、完了したら次の回転の始点を更新する。
    if (playing && std::abs(flipTargetAngle_ - flipFromAngle_) > 0.001f) {
        flipElapsed_ = (std::min)(flipElapsed_ + (std::max)(deltaTime, 0.0f), flipDuration_);
        const float t = std::clamp(flipElapsed_ / flipDuration_, 0.0f, 1.0f);
        const float eased = Easing::SmoothStep(t);
        flipAngle_ = flipFromAngle_ + (flipTargetAngle_ - flipFromAngle_) * eased;
        // 補間を終えた角度を始点に保存し、次のUpdateで同じ回転を進め直さないようにする。
        if (t >= 1.0f)
            flipFromAngle_ = flipTargetAngle_;
    }

    ApplyFlipPairAppearance();
}

// 現在角度のモデルと、目標面の床判定を個別に反映する。
void GimmickBlinkBlock::ApplyFlipPairAppearance() {
    // 枠は全コマに共通で静止し、中央のX軸を中心とした板だけが半回転する。
    const std::int32_t frame = std::clamp(static_cast<std::int32_t>(std::lround(flipAngle_ / kPi * kFlipFrameCount)),
                                 0, kFlipFrameCount);
    // 丸めたコマ番号が変わったフレームだけ表示モデルを差し替える。
    if (frame != flipFrame_) {
        SetModel(FlipFrameModelName(frame));
        flipFrame_ = frame;
    }

    SetIsVisible(true);
    SetColor({1.0f, 1.0f, 1.0f, 1.0f});

    // 描画の読込座標と一致する端点判定へ切り替える。途中コマの全体AABBは使わない。
    const engine::ColliderConfig& config = flipPairColliders_[flipTargetAngle_ < kPi * 0.5f ? 0 : 1];
    const engine::ColliderConfig& current = GetColliderConfig();
    const bool colliderChanged = current.type != config.type ||
        current.center.x != config.center.x || current.center.y != config.center.y || current.center.z != config.center.z ||
        current.size.x != config.size.x || current.size.y != config.size.y || current.size.z != config.size.z ||
        current.rotation.x != 0.0f || current.rotation.y != 0.0f || current.rotation.z != 0.0f ||
        current.useModelSurface || current.useModelVolume || !current.footprint.empty();
    // 既存の設定が端点のOBBと異なる場合だけ再設定する。
    if (colliderChanged)
        SetColliderConfig(config);

    const uint32_t attribute = config.type == engine::ColliderType::kNone ? 0 : engine::collisionCategories::Get("ground");
    // 床形状の有無に対応する属性へ、値が変わった場合だけ更新する。
    if (GetCollisionAttribute() != attribute)
        SetCollisionAttribute(attribute);
    const uint32_t mask = engine::collisionCategories::GetDefaultGameplayMask();
    // 現在の衝突マスクが既定のゲームプレイ用マスクと異なる場合に揃える。
    if (GetCollisionMask() != mask)
        SetCollisionMask(mask);
}

// 通常モデルではジャンプ回数と色設定で床属性と透明度を切り替える。
void GimmickBlinkBlock::UpdateAppearance() {
    // 現在のジャンプ回数と色設定が対応している間は、床として表示する。
    if (isActive_) {
        SetIsVisible(true);
        SetCollisionAttribute(engine::collisionCategories::Get("ground"));

        // 設定値0なら、青の不透明色を使う。
        if (colorType_ == 0) {
            SetColor({0.2f, 0.6f, 1.0f, 1.0f});
        } else {
            // それ以外は、赤の不透明色を使う。
            SetColor({1.0f, 0.3f, 0.3f, 1.0f});
        }
    } else {
        // ジャンプ回数が対応しない間は床属性を外し、色の不透明度を落とす。
        SetCollisionAttribute(0);

        // 設定値0は青の半透明色を使う。
        if (colorType_ == 0) {
            SetColor({0.2f, 0.6f, 1.0f, 0.2f});
        } else {
            // それ以外は、赤の半透明色を使う。
            SetColor({1.0f, 0.3f, 0.3f, 0.2f});
        }
    }
}

std::unique_ptr<engine::Object3D> GimmickBlinkBlock::Clone() const {
    // 同じモデルで新しい実体を初期化してから、配置と設定値を複製する。
    auto newObj = std::make_unique<GimmickBlinkBlock>();
    assert(GetObjectCommon() != nullptr);
    newObj->Initialize(GetObjectCommon(), GetModelName());
    newObj->CopyFrom(this);
    return newObj;
}
