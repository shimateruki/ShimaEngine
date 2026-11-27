#define NOMINMAX
#include <cstdint>
#include "game/scene/select/GameSelectScene.h"
#include "Player.h"
#include "CameraManager.h"
#include "game/system/progress/StageManager.h"
#include "Log.h"
#include "WinApp.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include "json.hpp"
#include "engine/utility/JsonFile.h"

namespace {
constexpr const char* kSpriteResourcePrefix = "Resources/sprite/";
std::string ToSpriteRelativePath(const std::string& path) {
    // Sprite 管理器が期待する Resources/sprite 以下の相対名へ揃える。
    if (path.rfind(kSpriteResourcePrefix, 0) == 0) {
        return path.substr(std::string(kSpriteResourcePrefix).size());
    }

    return path;
}

// 近距離は不透明のまま表示し、遠方のフェード区間だけ薄くする。距離はm、戻り値は0～1。
float GateNameAlpha(float distance, float showDistance, float fadeDistance) {
    // 距離を計算できない場合や負の距離を渡された場合は、ラベルを表示しない。
    if (!std::isfinite(distance) || distance < 0.0f) {
        return 0.0f;
    }
    return std::clamp(1.0f - (distance - std::max(showDistance, 0.0f)) / std::max(fadeDistance, 0.1f), 0.0f, 1.0f);
}

} // namespace

GameSelectScene::StageSelectHudSprite
GameSelectScene::BindStageSelectHUDSprite(const std::string& name) {
    // レイアウトの静的な画像・配置・色を保持し、数値と表示だけを更新する。
    engine::Sprite* sprite = GetSpriteByName(name);
    // 配置 JSON に指定名がない場合は、エラーを記録し、後の検索対象も空にする。
    if (!sprite) {
        LOG_ERROR("stageSelect_sprite.json: Sprite '{}' was not found", name);
        return {};
    }

    sprite->SetVisible(true);
    sprite->Update();

    return {name, sprite->GetPosition(), sprite->GetSize(), sprite->GetColor()};
}

engine::Sprite* GameSelectScene::ResolveStageSelectHUDSprite(const StageSelectHudSprite& state) {
    // Editor や Replay が Sprite を削除・再生成しても、古い借用ポインタを使わない。
    return state.name.empty() ? nullptr : GetSpriteByName(state.name);
}

void GameSelectScene::InitializeStageSelectHUD() {
    // 全部品を配置JSONの名前だけで結ぶ。画像と位置はEditorで調整できる。
    stageSelectCrownIcon_ = BindStageSelectHUDSprite("stage_select_hud_crown_icon");
    stageSelectCrownXIcon_ = BindStageSelectHUDSprite("stage_select_hud_crown_x");
    stageSelectStarIcon_ = BindStageSelectHUDSprite("stage_select_hud_star_icon");
    stageSelectStarXIcon_ = BindStageSelectHUDSprite("stage_select_hud_star_x");
    stageSelectCoinIcon_ = BindStageSelectHUDSprite("stage_select_hud_coin_icon");
    stageSelectCoinXIcon_ = BindStageSelectHUDSprite("stage_select_hud_coin_x");

    // 桁数に応じた表示と画像の切り替えだけを更新側へ残す。
    for (std::int32_t i = 0; i < 3; ++i) {
        stageSelectCrownDigits_[static_cast<size_t>(i)] = BindStageSelectHUDSprite(
            "stage_select_hud_crown_digit_" + std::to_string(i));
        stageSelectStarDigits_[static_cast<size_t>(i)] = BindStageSelectHUDSprite(
            "stage_select_hud_star_digit_" + std::to_string(i));
        stageSelectCoinDigits_[static_cast<size_t>(i)] = BindStageSelectHUDSprite(
            "stage_select_hud_coin_digit_" + std::to_string(i));
    }
}

void GameSelectScene::InitializeStageGateNameLabels() {
    const auto& stages = StageManager::GetInstance()->GetStages();

    // 最後のスロットをチュートリアル専用にし、通常ステージの添字は変えません。
    const size_t tutorialIndex = stages.size();
    stageGateNameLabels_.clear();
    stageGateNameLabels_.resize(stages.size() + 1);
    stageGateStarIcons_.clear();
    stageGateStarIcons_.resize(stages.size() + 1);

    for (size_t index = 0; index <= tutorialIndex; ++index) {
        const bool tutorial = index == tutorialIndex;
        const std::string texturePath = tutorial ? "Resources/sprite/ui/stage_select/tutorial_gate_name.png"
                                                 : stages[index].gateNameTexturePath;
        engine::Object3D* gate = tutorial ? FindObjectByName("TutorialArrivalGate")
                                  : FindStageGateByIndex(static_cast<std::int32_t>(index));
        // 描画先のゲートや指定画像が無い場合は、そのステージ番号のスロットを空のまま保ちます。
        if (texturePath.empty() || !gate) {
            continue;
        }

        // 画像の縦横比を維持して名札を作り、毎フレームの投影位置が決まるまで隠します。
        auto label = std::make_unique<engine::Sprite>();
        label->Initialize(GetSpriteCommon(), texturePath);
        label->SetName("stage_gate_name_" + std::to_string(index));
        label->SetTextureName(ToSpriteRelativePath(texturePath));
        label->SetAnchorPoint({0.5f, 1.0f});
        const engine::Vector2 textureSize = label->GetSize();
        const float width = 350.0f;
        label->SetSize({width, width * textureSize.y / std::max(textureSize.x, 1.0f)});
        label->SetVisible(false);
        label->Update();
        stageGateNameLabels_[index] = std::move(label);

        // チュートリアルとスターコインがないステージには、完了マークの列を作りません。
        if (tutorial || !stages[index].hasStarCoins) {
            continue;
        }

        // 名札と同じスクリーン座標で描き、セーブ済みの3個の収集状況を並べます。
        for (std::int32_t coinIndex = 0; coinIndex < GameDataManager::kStarCoinsPerStage; ++coinIndex) {
            auto star = std::make_unique<engine::Sprite>();
            star->Initialize(GetSpriteCommon(), "Resources/sprite/ui/hud/stage_star_empty.png");
            star->SetName("stage_gate_star_" + std::to_string(index) + "_" + std::to_string(coinIndex));
            star->SetTextureName("ui/hud/stage_star_empty.png");
            star->SetAnchorPoint({0.5f, 0.5f});
            star->SetSize({42.0f, 42.0f});
            star->SetVisible(false);
            star->Update();
            stageGateStarIcons_[index][static_cast<size_t>(coinIndex)] = std::move(star);
        }
    }
}

void GameSelectScene::UpdateStageGateNameLabels() {
    // 通常操作時だけ名札を表示します。カメラ・プレイヤー不在や演出切替中は全てのゲートを表示対象外にします。
    engine::Camera* camera = CameraManager::GetInstance()->GetActiveCamera();
    const bool showLabels = player_ && camera && !isChangingStage_ && !gateEntryCinematicActive_ &&
                            !gateReturnPresentationActive_ && !crownCountPresentationActive_ &&
                            unlockingStageIndex_ < 0;
    engine::Math math;
    const engine::Matrix4x4 viewProjection = showLabels
        ? math.Multiply(camera->GetViewMatrix(), camera->GetProjectionMatrix())
        : math.MakeIdentity4x4();

    for (size_t index = 0; index < stageGateNameLabels_.size(); ++index) {
        const bool tutorial = index == stageGateNameLabels_.size() - 1;
        engine::Sprite* label = stageGateNameLabels_[index].get();

        // 初期化時に作らなかった名札のスロットは飛ばす。
        if (!label) {
            continue;
        }

        // 名札が見えなくなった場合は、それに付随するスターも同じタイミングで隠します。
        auto hide = [this, index, label]() {
            label->SetVisible(false);

            for (auto& star : stageGateStarIcons_[index]) {
                // このステージに生成済みのスターだけを名札と一緒に隠す。
                if (star) {
                    star->SetVisible(false);
                }
            }
        };

        // 名札を表示する間だけ、チュートリアルの固定名またはステージ番号でゲートを探す。
        engine::Object3D* gate = nullptr;
        if (showLabels) {
            gate = tutorial ? FindObjectByName("TutorialArrivalGate")
                            : FindStageGateByIndex(static_cast<std::int32_t>(index));
        }
        // 演出中で名札を止める場合やゲートが見つからない場合は、名札とスターを隠す。
        if (!gate) {
            hide();
            continue;
        }

        const engine::Vector3 gatePosition = gate->GetWorldPosition();
        const engine::Vector3 playerPosition = player_->GetWorldPosition();
        const float dx = gatePosition.x - playerPosition.x;
        const float dz = gatePosition.z - playerPosition.z;
        const float distance = std::sqrt(dx * dx + dz * dz);

        // プレイヤーとゲートの水平距離を使い、表示距離の範囲外で徐々に透明にします。
        const float alpha = GateNameAlpha(distance, gateNameShowDistance_, gateNameFadeDistance_);
        // 水平距離がフェード区間を越えたら、投影せず名札とスターを隠す。
        if (alpha <= 0.0f) {
            hide();
            continue;
        }

        // モデル上端の少し上を投影し、カメラ背後の点や視錐台外の点は画面座標へ変換しません。
        const engine::Aabb bounds = gate->GetModelWorldAabb();
        const engine::Vector3 worldPosition = {gatePosition.x, bounds.max.y + gateNameHeightOffset_, gatePosition.z};
        const float w = worldPosition.x * viewProjection.m[0][3] + worldPosition.y * viewProjection.m[1][3] +
                        worldPosition.z * viewProjection.m[2][3] + viewProjection.m[3][3];
        // カメラ後方や投影の除算に使う w が小さすぎる点は表示しない。
        if (w <= 0.001f) {
            hide();
            continue;
        }

        const engine::Vector3 ndc = engine::Math::Transform(worldPosition, viewProjection);
        // NDC が非有限値か視錐台外なら、名札とスターを画面上に配置しない。
        if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y) || !std::isfinite(ndc.z) ||
            ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f ||
            ndc.z < 0.0f || ndc.z > 1.0f) {
            hide();
            continue;
        }
		// NDC を画面座標に変換し、名札が左右からはみ出さないよう X を制限する。
        const float screenWidth = static_cast<float>(WinApp::GetClientWidth());
        const float screenHeight = static_cast<float>(WinApp::GetClientHeight());
        const float halfWidth = label->GetSize().x * 0.5f;
        const float screenX = std::clamp((ndc.x + 1.0f) * 0.5f * screenWidth, halfWidth, screenWidth - halfWidth);
        const float screenY = (1.0f - ndc.y) * 0.5f * screenHeight;
        label->SetPosition({screenX, screenY - 34.0f});
        label->SetColor({1.0f, 1.0f, 1.0f, alpha});
        label->SetVisible(true);
        label->Update();

        // 通常ステージだけセーブ済みの収集状態を参照して、名札に沿ってスターを並べます。
        auto* save = GameDataManager::GetInstance();

        // チュートリアルにはスター欄がないため、名札の更新で終える。
        if (tutorial) {
            continue;
        }

        for (std::int32_t coinIndex = 0; coinIndex < GameDataManager::kStarCoinsPerStage; ++coinIndex) {
            engine::Sprite* star = stageGateStarIcons_[index][static_cast<size_t>(coinIndex)].get();

            // スターコインの欄が用意されていない位置は描画を更新しない。
            if (!star) {
                continue;
            }

            const bool collected = save && save->IsStarCoinCollected(static_cast<std::int32_t>(index), coinIndex);
            const std::string texture = collected ? "ui/hud/stage_star_filled.png" : "ui/hud/stage_star_empty.png";

            // 収集・未収集の表示が前回と異なる場合だけ画像を差し替える。透明度は名札と揃える。
            if (star->GetTextureName() != texture) {
                star->SetTextureHandle(engine::Sprite::LoadTexture(texture));
                star->SetTextureName(texture);
            }

            star->SetPosition({screenX + (static_cast<float>(coinIndex) - 1.0f) * 50.0f, screenY - 7.0f});
            star->SetColor({1.0f, 1.0f, 1.0f, alpha});
            star->SetVisible(true);
            star->Update();
        }
    }
}

void GameSelectScene::LoadStageGateNameSettings() {
    std::ifstream input("Resources/json/ui/stage_gate_info_settings.json");
    // 専用設定を開けない場合は、ロード前の値をそのまま使う。
    if (!input.is_open()) {
        return;
    }
    const json data = json::parse(input, nullptr, false);
    // JSON の構文が不正かトップレベルがオブジェクトでなければ、設定全体を読み飛ばす。
    if (data.is_discarded() || !data.is_object()) {
        LOG_ERROR("stage_gate_info_settings.json: invalid settings object");
        return;
    }
    // 各設定を独立して読み、欠損や不正な数値で他の設定まで戻らないようにする。
    auto read = [&data](const char* key, float current, float minimum, float maximum) {
        // 対象キーがないか数値でなければ、この項目だけ現在値を残す。
        if (!data.contains(key) || !data[key].is_number()) {
            return current;
        }
        const float value = data[key].get<float>();
        return std::isfinite(value) ? std::clamp(value, minimum, maximum) : current;
    };
    // 不透明で表示する距離、そこから薄くする距離、ゲート上端から名札までの高さをワールド単位で制限する。
    gateNameShowDistance_ = read("nameShowDistance", gateNameShowDistance_, 5.0f, 150.0f);
    gateNameFadeDistance_ = read("nameFadeDistance", gateNameFadeDistance_, 1.0f, 40.0f);
    gateNameHeightOffset_ = read("nameHeightOffset", gateNameHeightOffset_, 0.0f, 10.0f);
}

bool GameSelectScene::SaveStageGateNameSettings() const {
    // 配置JSONとは別のファイルへ、この機能の設定キーだけ保存する。
    return jsonFile::UpdateObject("Resources/json/ui/stage_gate_info_settings.json", {
        {"nameShowDistance", gateNameShowDistance_}, {"nameFadeDistance", gateNameFadeDistance_},
        {"nameHeightOffset", gateNameHeightOffset_}
    });
}

void GameSelectScene::SetStageSelectHUDNumber(std::array<StageSelectHudSprite, 3>& digits, std::int32_t value, bool visible) {
    // 桁数に応じて先頭のゼロを隠し、固定長の Sprite 配列を再利用する。
    value = std::clamp(value, 0, 999);
    const std::array<std::int32_t, 3> digitValues = {value / 100, (value / 10) % 10, value % 10};
    const std::int32_t digitCount = value >= 100 ? 3 : (value >= 10 ? 2 : 1);
    const std::int32_t visibleStart = 3 - digitCount;

    for (std::int32_t i = 0; i < 3; ++i) {
        StageSelectHudSprite& state = digits[static_cast<size_t>(i)];

        engine::Sprite* sprite = ResolveStageSelectHUDSprite(state);
        // 配置から対応する桁のスプライトを解決できない場合は、その桁を飛ばす。
        if (!sprite) {
            continue;
        }

        // 表示する桁だけ数字画像を設定し、残りはレイアウトの並びを変えず非表示にします。
        const bool digitVisible = visible && i >= visibleStart;
        sprite->SetVisible(digitVisible);

        // 表示対象の桁だけ数字画像と初期色を設定する。
        if (digitVisible) {
            sprite->SetTextureHandle(
                engine::Sprite::LoadTexture("number/" + std::to_string(digitValues[static_cast<size_t>(i)]) + ".png"));
            sprite->SetColor(state.baseColor);
        }

        sprite->Update();
    }
}

void GameSelectScene::UpdateUi() {
    // 取得数をセーブデータから読み直し、ステージ選択へ戻った直後も最新値を見せる。
    auto* save = GameDataManager::GetInstance();

    // セーブデータ管理器を取得できない間は、表示数の更新を見送る。
    if (!save) {
        return;
    }

    const bool visible = true;
    const std::array<StageSelectHudSprite*, 6> icons = {&stageSelectCrownIcon_, &stageSelectCrownXIcon_,
                                                        &stageSelectStarIcon_,  &stageSelectStarXIcon_,
                                                        &stageSelectCoinIcon_,  &stageSelectCoinXIcon_};
    for (StageSelectHudSprite* state : icons) {
        // 有効なアイコンの状態だけを名前で引き直す。
        if (state) {
            // 対応するスプライトがシーン内に見つかった場合だけ表示する。
            if (engine::Sprite* sprite = ResolveStageSelectHUDSprite(*state)) {
                sprite->SetVisible(visible);
                sprite->Update();
            }
        }
    }

    // 王冠表示だけ演出中の値を許し、スターとコインはセーブデータの合計を描きます。
    SetStageSelectHUDNumber(stageSelectCrownDigits_, GetDisplayedCrownCount(), visible);
    SetStageSelectHUDNumber(stageSelectStarDigits_, save->GetCollectedStarCoinCount(), visible);
    SetStageSelectHUDNumber(stageSelectCoinDigits_, save->GetCoins(), visible);
    UpdateStageSelectCrownHudReward();
}
