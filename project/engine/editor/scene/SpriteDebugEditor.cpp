#define NOMINMAX
#include <cstdint>
#include "SpriteDebugEditor.h"
#include "WinApp.h"
#include "Sprite.h"
#include "imgui.h"
#include "json.hpp"
#include <fstream>
#include <string>
#include "SpriteCommon.h" 
#include "SceneManager.h"  
#include "BaseScene.h"    
#include "InputManager.h"
#include <filesystem>
#include "IconsFontAwesome5.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include "game/ui/SpriteLayoutFactory.h"
#include "engine/graphics/2d/SpriteLayoutAnimation.h"
#include "engine/graphics/2d/SpriteLayoutVideo.h"
#include <chrono>
#include"WinApp.h"
#include "SrvManager.h"
#include "TextureManager.h"
#include "DirectXCommon.h"
namespace fs = std::filesystem;

namespace {
constexpr float kSpriteAspectWarningThreshold = 0.02f;

std::string NormalizeSpritePath(std::string path) {
	// Windows形式とスラッシュ形式を同じ参照として扱い、文字画像のフォルダ名を照合する。
	std::replace(path.begin(), path.end(), '\\', '/');
	std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
		});
	return path;
}
}


// 元のJSONと各Spriteの元データを保持し、配置以外の項目も保存時に引き継ぎます。
struct SpriteDebugEditor::LayoutDocument {
    SpriteList sprites;
    nlohmann::json source;
    std::unordered_map<const engine::Sprite*, nlohmann::json> originalSprites;
    fs::path path;
    engine::Vector2 displayRatio{1.0f, 1.0f}; // 設計ピクセルから編集中の画面ピクセルへの倍率。
    std::unique_ptr<engine::Sprite> background;
    bool isNew = false;
    bool previewPlaying = false;
    std::chrono::steady_clock::time_point previewStart;
    std::chrono::steady_clock::time_point videoTick;
    std::unique_ptr<VideoSpritePlayer> videoPreview;
    engine::Sprite* videoSprite = nullptr;
    uint32_t videoPoster = 0;

    // 再生先が残っていれば元の静止画に戻し、再生器がある場合は動画を閉じてGPUの利用権を解放する。
    void StopVideo() {
        if (videoSprite)
            spriteLayoutVideo::SetTexture(*videoSprite, videoPoster);
        videoSprite = nullptr;
        if (videoPreview)
            videoPreview->Close();
    }

    // 選択したSpriteを映像へ切り替え、復帰に使う元画像と再生時刻を保持します。
    bool StartVideo(engine::Sprite& sprite, const std::string& path) {
        StopVideo();
        const auto file = spriteLayoutVideo::Resolve(path);
        // 素材の場所を解決できなければ、新しい再生を要求しない。
        if (file.empty())
            return false;
        // 再生器をまだ持っていなければ最初の要求で生成する。
        if (!videoPreview)
            videoPreview = std::make_unique<VideoSpritePlayer>();
        videoSprite = &sprite;
        videoPoster = sprite.GetTextureHandle();
        videoTick = std::chrono::steady_clock::now();
        videoPreview->RequestOpen(file);
        return true;
    }
};

SpriteDebugEditor::SpriteDebugEditor() = default;
SpriteDebugEditor::~SpriteDebugEditor() = default;

// 独立レイアウトを開いていれば編集用の配列を返し、それ以外は現シーンの一覧を借りる。
const SpriteDebugEditor::SpriteList& SpriteDebugEditor::GetEditingSprites(const BaseScene& scene) const {
    return layoutDocument_ ? layoutDocument_->sprites : scene.GetSprites();
}

// 現在の編集対象へSpriteの所有権を渡し、登録済みSpriteの参照を返す。
engine::Sprite* SpriteDebugEditor::AddEditingSprite(BaseScene& scene, std::unique_ptr<engine::Sprite> sprite) {
    // シーン編集ではシーンの登録処理へ任せる。
    if (!layoutDocument_)
        return scene.AddSprite(std::move(sprite));
    // 独立レイアウトでは親子の参照にも使う名前が重複しないよう、連番を探す。
    const std::string baseName = sprite->GetName();
    for (std::int32_t index = 2; std::any_of(layoutDocument_->sprites.begin(), layoutDocument_->sprites.end(),
         [&sprite](const auto& item) { return item->GetName() == sprite->GetName(); }); ++index) {
        sprite->SetName(baseName + "_" + std::to_string(index));
    }
    auto* result = sprite.get();
    layoutDocument_->sprites.push_back(std::move(sprite));
    return result;
}

// 表示対象の一覧から指定Spriteを削除する。見つからなければfalse。
bool SpriteDebugEditor::DeleteEditingSprite(BaseScene& scene, engine::Sprite* sprite) {
    // シーン編集ならシーン側に削除を依頼する。
    if (!layoutDocument_)
        return scene.DestroySprite(sprite);
    auto& sprites = layoutDocument_->sprites;
    const auto position = std::find_if(sprites.begin(), sprites.end(),
        [sprite](const auto& item) { return item.get() == sprite; });
    // 独立レイアウトが所有していないSpriteへは触れない。
    if (position == sprites.end())
        return false;

    // 親を消しても、残る子はキャンバス上の位置を維持します。
    const auto children = sprite->GetChildren();
    for (auto* child : children)
        child->SetParent(nullptr);
    // 動画を表示していたSpriteを削除する場合は、解放する前に動画を止める。
    if (layoutDocument_->videoSprite == sprite)
        layoutDocument_->StopVideo();
    // レイアウトが保持する元データからも対象の行を除く。
    layoutDocument_->originalSprites.erase(sprite);
    sprites.erase(position);
    return true;
}

// 描画順の配列を指定した個数だけ前後に移す。offsetは添字の差。
void SpriteDebugEditor::MoveEditingSprite(BaseScene& scene, engine::Sprite* sprite, std::int32_t offset) {
    // シーンを編集中ならシーン側の描画順を変更する。
    if (!layoutDocument_) {
        scene.MoveSprite(sprite, offset);
        return;
    }
    auto& sprites = layoutDocument_->sprites;
    const auto position = std::find_if(sprites.begin(), sprites.end(),
        [sprite](const auto& item) { return item.get() == sprite; });
    const auto next = std::distance(sprites.begin(), position) + offset;
    // 対象があり、移動先の添字が配列内なら位置を交換する。
    if (position != sprites.end() && next >= 0 && next < static_cast<std::ptrdiff_t>(sprites.size()))
        std::iter_swap(position, sprites.begin() + next);
}

void SpriteDebugEditor::SetSelectedSprite(engine::Sprite* sprite) {
    // 他のツールがシーンにスプライトを追加しても、編集中のレイアウトには混ぜません。
    if (!layoutDocument_)
        selectedSprite_ = sprite;
}

void SpriteDebugEditor::Initialize(SceneManager* sceneManager, engine::InputManager* inputManager) {
    // Sceneと入力を借用し、前SceneのSprite選択とギズモ操作を初期化します。
	sceneManager_ = sceneManager;
	inputManager_ = inputManager;
	selectedSprite_ = nullptr;
	isMovingX_ = false;
	isMovingY_ = false;


	gizmoArrowX_ = nullptr;
	gizmoArrowY_ = nullptr;
	gizmoTextureHandle_ = 0;
	initializedSpriteCommon_ = nullptr;
}


// 選択と動画の参照を解除した後、独立レイアウトを破棄する。
void SpriteDebugEditor::Finalize() {
	ClearSceneSelection();
	layoutDocument_.reset();
}

void SpriteDebugEditor::ClearSceneSelection() {
    // 独立レイアウトの参照を外す前に、再生中の動画を止める。
    if (layoutDocument_)
        layoutDocument_->StopVideo();
    // Scene切り替えでSpriteと描画基盤の参照が失効するため、ギズモも解放します。
	selectedSprite_ = nullptr;
	isMovingX_ = false;
	isMovingY_ = false;
	lastUpdatedScene_ = nullptr;
	initializedSpriteCommon_ = nullptr;
	gizmoArrowX_.reset();
	gizmoArrowY_.reset();
}

void SpriteDebugEditor::Update(const engine::Vector2& localMousePos, bool isHovered) {
#ifdef USE_IMGUI
	// 現シーンを取得する管理クラスがなければ入力を扱わない。
	if (sceneManager_ == nullptr)
		return;

	BaseScene* currentScene = sceneManager_->GetCurrentScene();

	// シーンが閉じられていたら無効になり得る選択だけ解除する。
	if (currentScene == nullptr) {
		selectedSprite_ = nullptr;
		return;
	}

	// 対象シーンが変わったフレームで、選択・ドラッグ・保存ファイル名を初期化する。
	if (lastUpdatedScene_ != currentScene) {
		selectedSprite_ = nullptr;
		isMovingX_ = false;
		isMovingY_ = false;
		strcpy_s(currentSpriteFilename_, sizeof(currentSpriteFilename_), "Please_Select_Or_Set.json");
		lastUpdatedScene_ = currentScene;
	}

	auto& currentSprites = GetEditingSprites(*currentScene);
	// 選択済みのポインタは現シーンの一覧に残っているか比較し、削除後の実体を参照しない。
	if (selectedSprite_ &&
		std::none_of(currentSprites.begin(), currentSprites.end(), [this](const std::unique_ptr<engine::Sprite>& spritePtr) {
			return spritePtr && spritePtr.get() == selectedSprite_;
		})) {
		selectedSprite_ = nullptr;
		isMovingX_ = false;
		isMovingY_ = false;
	}

	static std::string sLastSyncedSpriteFilename = "";
	std::string currentLoadedName = currentScene->GetLoadedSpriteFilename();

	// シーン編集中だけ、読み込んだファイル名が前回の同期値から変わったときに保存候補へ反映する。
	if (!layoutDocument_ && !currentLoadedName.empty() && sLastSyncedSpriteFilename != currentLoadedName) {
		SetSpriteFilename(currentLoadedName);
		sLastSyncedSpriteFilename = currentLoadedName;
	}

	// 選択中のSpriteがある場合にだけ削除・複製・移動のショートカットを扱う。
	if (selectedSprite_ != nullptr) {
		// 文字入力中（名前の変更中など）でなければショートカットを許可
		if (!ImGui::GetIO().WantCaptureKeyboard) {

			// Deleteを押したフレームで削除を要求し、削除できた場合だけ選択参照も外す。
			if (inputManager_->IsKeyTriggered(DIK_DELETE)) {
				if (DeleteEditingSprite(*currentScene, selectedSprite_)) {
					selectedSprite_ = nullptr;
				}
			}

			// 削除された場合はここで抜ける
			if (selectedSprite_ == nullptr)
				goto SKIP_SHORTCUTS;

			// 左右どちらかのCtrlを押している間にCかDを押すと複製する。
			bool isCtrlDown = inputManager_->IsKeyPressed(DIK_LCONTROL) || inputManager_->IsKeyPressed(DIK_RCONTROL);

			if (isCtrlDown && (inputManager_->IsKeyTriggered(DIK_C) || inputManager_->IsKeyTriggered(DIK_D))) {

				auto newSprite = std::make_unique<engine::Sprite>();

				// 元のスプライトからテクスチャを取得して初期化
				uint32_t handle = selectedSprite_->GetTextureHandle();
				newSprite->Initialize(currentScene->GetSpriteCommon(), handle);

				newSprite->SetName(selectedSprite_->GetName() + "_Copy");

				// 選択元の画面上の位置から右下へ20ピクセルずらし、複製元と重なり切らないようにする。
				engine::Vector2 selectedWorldPos = selectedSprite_->GetWorldPosition();
				newSprite->SetPosition({ selectedWorldPos.x + 20.0f, selectedWorldPos.y + 20.0f });
				newSprite->SetSize(selectedSprite_->GetSize());
				newSprite->SetAnchorPoint(selectedSprite_->GetAnchorPoint());
				newSprite->SetColor(selectedSprite_->GetColor());
                newSprite->SetTextureName(selectedSprite_->GetTextureName());
                newSprite->SetEmissive(selectedSprite_->GetEmissive());
                newSprite->SetRotation(selectedSprite_->GetRotation());


				// 編集中の一覧に登録した複製をそのまま選択し、続けて移動できるようにする。
				selectedSprite_ = AddEditingSprite(*currentScene, std::move(newSprite));
			}

			// ロック中は方向キーで位置を変えない。
			if (!selectedSprite_->IsLocked()) {

				// Shiftキーを押している間は 5.0 ピクセル/フレームの高速移動、
				// 通常時は 0.5 ピクセル/フレームの微細な移動にします。
				float nudgeStep = (inputManager_->IsKeyPressed(DIK_LSHIFT) || inputManager_->IsKeyPressed(DIK_RSHIFT)) ? 5.0f : 0.5f;

				engine::Vector2 pos = selectedSprite_->GetPosition();
				bool isNudged = false;

				// 押している方向ごとの差分を足す。画面の上と左へは減算、下と右へは加算する。
				if (inputManager_->IsKeyPressed(DIK_UP)) {
					pos.y -= nudgeStep;
					isNudged = true;
				}
				if (inputManager_->IsKeyPressed(DIK_DOWN)) {
					pos.y += nudgeStep;
					isNudged = true;
				}
				if (inputManager_->IsKeyPressed(DIK_LEFT)) {
					pos.x -= nudgeStep;
					isNudged = true;
				}
				if (inputManager_->IsKeyPressed(DIK_RIGHT)) {
					pos.x += nudgeStep;
					isNudged = true;
				}

				// 方向キーが押されていた場合だけ、差分を合成した座標を反映する。
				if (isNudged) {
					selectedSprite_->SetPosition(pos);
				}
			}
		}
	}

SKIP_SHORTCUTS:

	// ドラッグ中は Game View の外へ出ても、ボタンを離すまで操作を継続する。
	if (!isHovered && !isMovingX_ && !isMovingY_)
		return;

	// 選択中でロックされていないSpriteに対して、マウスでの位置調整を扱う。
	if (selectedSprite_) {
		if (!selectedSprite_->IsLocked()) {
			// 左ボタンを押した位置がSprite内なら、その時点のマウス位置とSprite位置をドラッグの基準にする。
			if (inputManager_->IsMouseButtonTriggered(0) && IsMouseOver(selectedSprite_, localMousePos)) {
				isMovingX_ = true;
				isMovingY_ = true;
				dragStartMousePos_ = localMousePos;
				dragStartSpritePos_ = selectedSprite_->GetPosition();
			}

			// 左ボタンを保持してドラッグ中なら、開始点からのマウス移動量をSpriteへ足す。
			if (inputManager_->IsMouseButtonPressed(0) && (isMovingX_ || isMovingY_)) {
				engine::Vector2 delta = { localMousePos.x - dragStartMousePos_.x, localMousePos.y - dragStartMousePos_.y };
				engine::Vector2 newPos = { dragStartSpritePos_.x + delta.x, dragStartSpritePos_.y + delta.y };

				// Shiftキーを押している間は10ピクセル単位でスナップ
				if (inputManager_->IsKeyPressed(DIK_LSHIFT) || inputManager_->IsKeyPressed(DIK_RSHIFT)) {
					float snapStep = 10.0f;
					newPos.x = std::round(newPos.x / snapStep) * snapStep;
					newPos.y = std::round(newPos.y / snapStep) * snapStep;
				}

				selectedSprite_->SetPosition(newPos);
			}
		}

		// 左ボタンを離したフレームで移動を終える。
		if (inputManager_->IsMouseButtonReleased(0)) {
			isMovingX_ = false;
			isMovingY_ = false;
		}
	}

	// クリックが既存Spriteのドラッグ開始に使われた場合は、別のSpriteに選択を移さない。
	if (!isMovingX_ && !isMovingY_ && inputManager_->IsMouseButtonTriggered(0)) {
		auto& sprites = GetEditingSprites(*currentScene);
		bool hit = false;

		// 重なり順を考慮して逆順（手前）からチェック
		for (auto it = sprites.rbegin(); it != sprites.rend(); ++it) {
			engine::Sprite* sprite = it->get();

			// クリックが重なった場合は、ロックされていない最前面の候補を選ぶ。独立レイアウトでは不可視も除外する。
			if (sprite && (!layoutDocument_ || sprite->IsVisible()) && !sprite->IsLocked() && IsMouseOver(sprite, localMousePos)) {
				selectedSprite_ = sprite;
				hit = true;
				break;
			}
		}

		// どの候補もマウス位置を含まなければ、以前の選択を外す。
		if (!hit)
			selectedSprite_ = nullptr;
	}

#endif
}

void SpriteDebugEditor::DrawImGui() {
#ifdef USE_IMGUI
    // Hierarchy・Inspector・Asset一覧をSprite編集用の独立パネルとして描きます。
	DrawHierarchyWindow();
	DrawInspectorWindow();
	DrawProjectWindow();
#endif
}

void SpriteDebugEditor::DrawHierarchyWindow() {
#ifdef USE_IMGUI
	// シーンとSprite共通基盤が揃ってから階層と作成操作を描く。
	if (!sceneManager_)
		return;
	BaseScene* currentScene = sceneManager_->GetCurrentScene();
	if (!currentScene)
		return;

	engine::SpriteCommon* spriteCommon = currentScene->GetSpriteCommon();
	if (!spriteCommon)
		return;
	ImGui::Begin(ICON_FA_LIST_UL " Sprite Hierarchy");
	DrawLayoutFileControls(*currentScene);
	const auto& sprites = GetEditingSprites(*currentScene);

	// シーン編集では専用の保存欄を開ける。独立レイアウトの保存は上部の別欄で扱う。
	if (!layoutDocument_ && ImGui::CollapsingHeader(ICON_FA_SAVE " シーンのスプライト保存", ImGuiTreeNodeFlags_DefaultOpen)) {
		std::string directoryPath = "Resources/json/sprite/";

		// 最初の保存に備え、保存フォルダーがなければ作る。
		if (!std::filesystem::exists(directoryPath)) {
			std::filesystem::create_directories(directoryPath);
		}

		// 一覧で選んだファイル名は保存先の候補に入れ、選択した瞬間には書き込まない。
		if (ImGui::BeginCombo(ICON_FA_HISTORY " 既存ファイル", currentSpriteFilename_)) {
			// 一覧を開いたとき、フォルダーが存在すればJSONだけを保存名の候補に出す。
			if (std::filesystem::exists(directoryPath)) {
				for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
					if (entry.path().extension() == ".json") {
						std::string fname = entry.path().filename().string();
						bool isSelected = (std::string(currentSpriteFilename_) == fname);

						if (ImGui::Selectable(fname.c_str(), isSelected)) {
							strcpy_s(currentSpriteFilename_, fname.c_str());
						}
					}
				}
			}

			ImGui::EndCombo();
		}

		ImGui::InputText(ICON_FA_FILE_SIGNATURE " 保存名 (.json)", currentSpriteFilename_, sizeof(currentSpriteFilename_));

		// 保存ボタンが押された場合にだけ入力された名前へ書き出す。
		if (ImGui::Button(ICON_FA_DOWNLOAD " レイアウト保存 (Save)")) {
			std::string fullPath = directoryPath + std::string(currentSpriteFilename_);
			SaveSpriteLayout(fullPath);
		}
	}

	ImGui::Separator();
	ImGui::Spacing();

	// 編集対象が空なら案内を表示する。親のあるSpriteはルート一覧から除き、親の行の下で描く。
	ImGui::Text(ICON_FA_IMAGES " Sprite List");
	ImGui::BeginChild("SpriteListRegion", ImVec2(0, 0), true);


	if (sprites.empty()) {
		ImGui::TextDisabled(ICON_FA_INFO_CIRCLE " スプライトがありません");
	}

	for (const auto& spritePtr : sprites) {
		engine::Sprite* sprite = spritePtr.get();
		if (!sprite)
			continue;
		if (sprite->GetParent() != nullptr)
			continue;
		DrawSpriteNode(sprite);
	}

	// 画像のドロップは新規生成し、ツリー項目のドロップは既存 Sprite の親を外す。
	ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y));

	if (ImGui::BeginDragDropTarget()) {
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SPRITE_FILE")) {
			const char* droppedFilename = (const char*)payload->Data;
			auto newSprite = std::make_unique<engine::Sprite>();
			uint32_t handle = engine::Sprite::LoadTexture(droppedFilename);
			newSprite->Initialize(spriteCommon, handle);
			newSprite->SetName(droppedFilename);
			newSprite->SetTextureName(droppedFilename);
			selectedSprite_ = AddEditingSprite(*currentScene, std::move(newSprite));
		}

		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SPRITE_NODE")) {
			engine::Sprite* sourceSprite = *(engine::Sprite**)payload->Data;

			// 有効なSpriteがドロップされた場合だけ親を解除する。
			if (sourceSprite) {
				sourceSprite->SetParent(nullptr);
			}
		}

		ImGui::EndDragDropTarget();
	}

	ImGui::EndChild();
	ImGui::End();
#endif
}

void SpriteDebugEditor::DrawSpriteNode(engine::Sprite* sprite) {
#ifdef USE_IMGUI
    // Sprite自身をImGui IDに使い、同名Spriteの表示・選択が衝突しないようにします。
	if (!sprite)
		return;

	ImGui::PushID(sprite);

	// 目のボタンを押すと可視性を反転する。非表示のアイコンは灰色で描く。
	bool isVisible = sprite->IsVisible();
	const char* eyeIcon = isVisible ? ICON_FA_EYE : ICON_FA_EYE_SLASH;
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_Text, isVisible ? ImVec4(1, 1, 1, 1) : ImVec4(0.5f, 0.5f, 0.5f, 1));
	if (ImGui::Button(eyeIcon))
		sprite->SetVisible(!isVisible);
	ImGui::PopStyleColor(2);

	ImGui::SameLine();

	// 南京錠を押すとロックを反転する。ロック中のアイコンは赤く描く。
	bool isLocked = sprite->IsLocked();
	const char* lockIcon = isLocked ? ICON_FA_LOCK : ICON_FA_UNLOCK;
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_Text, isLocked ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
	if (ImGui::Button(lockIcon))
		sprite->SetLocked(!isLocked);
	ImGui::PopStyleColor(2);

	ImGui::SameLine();

	// 名前・選択状態・子の有無で行の表示を決め、非表示の行は灰色で描く。
	std::string label = sprite->GetName().empty() ? "Sprite" : sprite->GetName();
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
	if (selectedSprite_ == sprite)
		flags |= ImGuiTreeNodeFlags_Selected;
	if (sprite->GetChildren().empty())
		flags |= ImGuiTreeNodeFlags_Leaf;
	if (!isVisible)
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
	bool nodeOpen = ImGui::TreeNodeEx("##SpriteNode", flags, ICON_FA_IMAGE " %s", label.c_str());
	if (!isVisible)
		ImGui::PopStyleColor();

	// 行のクリックはInspectorで編集するSpriteの選択に使う。
	if (ImGui::IsItemClicked()) {
		selectedSprite_ = sprite;
	}

	// ドラッグ開始時にSpriteの参照をペイロードへ渡す。
	if (ImGui::BeginDragDropSource()) {
		engine::Sprite* payloadSprite = sprite;
		ImGui::SetDragDropPayload("SPRITE_NODE", &payloadSprite, sizeof(engine::Sprite*));
		ImGui::Text("Sprite: %s", label.c_str());
		ImGui::EndDragDropSource();
	}

	// 行へのドロップで受け取った別のSpriteを子にする。
	if (ImGui::BeginDragDropTarget()) {
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SPRITE_NODE")) {
			engine::Sprite* sourceSprite = *(engine::Sprite**)payload->Data;

			if (sourceSprite && sourceSprite != sprite) {
				sourceSprite->SetParent(sprite);
			}
		}

		ImGui::EndDragDropTarget();
	}

	// 展開した行の直下に、そのSpriteを現在も親としている子だけを描く。
	if (nodeOpen) {
		for (engine::Sprite* child : sprite->GetChildren()) {
			if (child && child->GetParent() == sprite) {
				DrawSpriteNode(child);
			}
		}

		ImGui::TreePop();
	}

	ImGui::PopID();
#endif
}

bool SpriteDebugEditor::TryGetTexturePixelSize(engine::Sprite* sprite, engine::Vector2& outSize) const {
    // 画像の実ピクセル寸法を取得できないSpriteは縦横比調整から除きます。
	if (!sprite) {
		return false;
	}

	const uint32_t textureHandle = sprite->GetTextureHandle();

	// 画像ハンドルがゼロなら実際の画像寸法を取得できない。
	if (textureHandle == 0) {
		return false;
	}

	const DirectX::TexMetadata& metadata = engine::TextureManager::GetInstance()->GetMetadata(textureHandle);

	// 画像の幅か高さがゼロなら縦横比を計算できない。
	if (metadata.width == 0 || metadata.height == 0) {
		return false;
	}

	outSize = { static_cast<float>(metadata.width), static_cast<float>(metadata.height) };
	return true;
}

bool SpriteDebugEditor::IsGeneratedTextSprite(engine::Sprite* sprite) const {
    // 自動生成した文字PNGだけに一括補正を限定するため、参照パスで判別します。
	if (!sprite) {
		return false;
	}

	const std::string textureName = NormalizeSpritePath(sprite->GetTextureName());
	return textureName.find("generated/text/") != std::string::npos;
}

float SpriteDebugEditor::CalcAspectError(engine::Sprite* sprite, const engine::Vector2& textureSize) const {
    // Spriteと画像の寸法から相対誤差を求める。必要な寸法を取得できない場合は0を返す。
	if (!sprite || textureSize.x <= 0.0f || textureSize.y <= 0.0f) {
		return 0.0f;
	}

	const engine::Vector2 size = sprite->GetSize();

	// 表示寸法が正でない場合は補正の判定に使わない。
	if (size.x <= 0.0f || size.y <= 0.0f) {
		return 0.0f;
	}

	const float textureAspect = textureSize.x / textureSize.y;
	const float spriteAspect = size.x / size.y;

	// 基準の比率が正でない場合は、その値で割らない。
	if (textureAspect <= 0.0f) {
		return 0.0f;
	}

	// 画像の縦横比で割った相対誤差を返す。たとえば0.1は本来の比率から10%ずれている。
	return std::fabs((spriteAspect / textureAspect) - 1.0f);
}

engine::Vector2 SpriteDebugEditor::MakeAspectFixedSize(engine::Sprite* sprite, const engine::Vector2& textureSize, bool keepWidth) const {
    // 画像寸法が有効なら、keepWidthに応じて幅か高さを保ち、他方だけ元画像の比率に合わせる。
	if (!sprite || textureSize.x <= 0.0f || textureSize.y <= 0.0f) {
		return { 0.0f, 0.0f };
	}

	const engine::Vector2 currentSize = sprite->GetSize();
	const float textureAspect = textureSize.x / textureSize.y;

	if (keepWidth) {
		return { currentSize.x, currentSize.x / textureAspect };
	}

	return { currentSize.y * textureAspect, currentSize.y };
}

// 画像に対する縦横の平均倍率に近い0.25刻みの寸法を返す。
engine::Vector2 SpriteDebugEditor::MakeNearestIntegerScaleSize(engine::Sprite* sprite, const engine::Vector2& textureSize) const {
    // 画像と表示対象が揃わなければ空の寸法を返す。
	if (!sprite || textureSize.x <= 0.0f || textureSize.y <= 0.0f) {
		return { 0.0f, 0.0f };
	}

	const engine::Vector2 currentSize = sprite->GetSize();
	// 横倍率と縦倍率が違う場合はその平均から、両軸に共通の倍率を求める。
	float scale = ((currentSize.x / textureSize.x) + (currentSize.y / textureSize.y)) * 0.5f;
	scale = std::round(scale * 4.0f) / 4.0f;
	scale = std::max(0.25f, scale);
	return { textureSize.x * scale, textureSize.y * scale };
}

std::int32_t SpriteDebugEditor::FixSpriteAspectInCurrentScene(bool textOnly, bool keepWidth) {
    // 編集中のシーンまたは独立レイアウトを対象に、指定された軸を保って比率を補正する。戻り値は補正した数。
	if (!sceneManager_) {
		return 0;
	}

	BaseScene* currentScene = sceneManager_->GetCurrentScene();

	// 現シーンがなければ編集対象を取得できないので0件を返す。
	if (!currentScene) {
		return 0;
	}

	std::int32_t fixedCount = 0;

	for (const std::unique_ptr<engine::Sprite>& spritePtr : GetEditingSprites(*currentScene)) {
		engine::Sprite* sprite = spritePtr.get();

		// 一覧の空の項目は処理しない。
		if (!sprite) {
			continue;
		}

		// 文字PNGだけを指定された場合は、参照パスが文字PNGに該当するものへ絞る。
		if (textOnly && !IsGeneratedTextSprite(sprite)) {
			continue;
		}

		engine::Vector2 textureSize{};

		// 元画像の幅と高さを取得できない場合は補正しない。
		if (!TryGetTexturePixelSize(sprite, textureSize)) {
			continue;
		}

		// 画像比率との差が2%未満なら、既に合わせたレイアウトとしてサイズを維持する。
		if (CalcAspectError(sprite, textureSize) < kSpriteAspectWarningThreshold) {
			continue;
		}

		sprite->SetSize(MakeAspectFixedSize(sprite, textureSize, keepWidth));
		++fixedCount;
	}

	spriteQualityStatus_ = "比率補正: " + std::to_string(fixedCount) + "件";
	return fixedCount;
}

void SpriteDebugEditor::DrawInspectorWindow() {
#ifdef USE_IMGUI
	// 現シーンを取得できる場合だけ編集欄を開く。
	if (!sceneManager_)
		return;
	BaseScene* currentScene = sceneManager_->GetCurrentScene();
	if (!currentScene)
		return;

	ImGui::Begin(ICON_FA_INFO_CIRCLE " Sprite Inspector");

	// 一括補正を展開した場合に、対象範囲と維持する軸を指定するボタンを出す。
	if (ImGui::CollapsingHeader("Sprite比率クリーンアップ", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::TextDisabled("画像本来の縦横比に合わせて、引き伸ばしを補正します。");

		if (ImGui::Button("文字Spriteを横幅維持で一括補正", ImVec2(-1.0f, 0.0f))) {
			FixSpriteAspectInCurrentScene(true, true);
		}

		if (ImGui::Button("全Spriteを横幅維持で一括補正", ImVec2(-1.0f, 0.0f))) {
			FixSpriteAspectInCurrentScene(false, true);
		}

		if (ImGui::Button("全Spriteを高さ維持で一括補正", ImVec2(-1.0f, 0.0f))) {
			FixSpriteAspectInCurrentScene(false, false);
		}

		// 一括補正を実行して結果があれば、変更件数を表示する。
		if (!spriteQualityStatus_.empty()) {
			ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.8f, 1.0f), "%s", spriteQualityStatus_.c_str());
		}

		ImGui::Separator();
	}

	// 個別の設定は選択がある場合に表示し、未選択時は案内だけを出す。
	if (selectedSprite_) {
		ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), ICON_FA_EDIT " 編集: %s", selectedSprite_->GetName().c_str());

        // 独立レイアウトの動画欄を開いたら、保存行と結び付いた動画を編集できる。
        if (layoutDocument_ && ImGui::CollapsingHeader("動画レイヤー", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& document = *layoutDocument_;
            auto& row = document.originalSprites[selectedSprite_];
            const auto stored = spriteLayoutScaler::ReadString(row, "video", "");
            char path[512]{};
            strncpy_s(path, stored.c_str(), _TRUNCATE);
            // パスを変更したら現再生を止める。空欄の場合は、存在する行から動画指定を外す。
            if (ImGui::InputText("動画 (Resources/video/)", path, sizeof(path))) {
                document.StopVideo();
                if (path[0])
                    row["video"] = path;
                else if (row.is_object())
                    row.erase("video");
            }
            // プレビューボタンで現在のパスを開き、要求できなければパスの案内を出す。
            if (ImGui::Button("動画をプレビュー")) {
                layoutFileStatus_ = document.StartVideo(*selectedSprite_, path)
                    ? "" : "Resources/video 内のMP4を指定してください。";
            }
            ImGui::SameLine();
            // 停止操作は再生器を閉じ、元の画像へ戻す。
            if (ImGui::Button("動画を停止"))
                document.StopVideo();
            ImGui::TextDisabled("準備中は指定した画像を表示します。");
        }

        // 独立レイアウトのコマ設定が展開されている場合に、分割数と時間設定を編集する。
        if (layoutDocument_ && ImGui::CollapsingHeader("コマアニメーション", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& row = layoutDocument_->originalSprites[selectedSprite_];
            auto settings = spriteLayoutAnimation::Read(row);
            const std::int32_t previousFrameCount = settings.frameCount;
            bool changed = ImGui::SliderInt("横一列のコマ数", &settings.frameCount, 1, 128);
            changed |= ImGui::SliderFloat("FPS", &settings.fps, 1.0f, 60.0f, "%.1f");
            changed |= ImGui::Checkbox("ループ", &settings.loop);
            // 分割数・FPS・ループのどれかを変更した場合にだけJSONの設定と再生開始時刻を更新する。
            if (changed) {
                auto& animation = row["animation"];
                // 旧形式や異なる型のanimationは、設定を書き込めるオブジェクトに置き換える。
                if (!animation.is_object())
                    animation = nlohmann::json::object();
                animation["frameCount"] = settings.frameCount;
                animation["fps"] = settings.fps;
                animation["loop"] = settings.loop;
                layoutDocument_->previewStart = std::chrono::steady_clock::now();

                // 有効な画像で均等に分割でき、編集サイズが変更前の1コマの原寸に一致する場合だけ新しい1コマの原寸へ変更する。
                if (selectedSprite_->GetTextureHandle() != 0) {
                    const auto& texture = engine::TextureManager::GetInstance()->GetMetadata(selectedSprite_->GetTextureHandle());
                    const float width = static_cast<float>(texture.width);
                    const float height = static_cast<float>(texture.height);
                    const bool canSplit = texture.width >= static_cast<size_t>(settings.frameCount) &&
                        texture.width % static_cast<size_t>(settings.frameCount) == 0;
                    const auto size = selectedSprite_->GetSize();
                    if (canSplit && std::abs(size.x - width / static_cast<float>(previousFrameCount)) < 0.01f &&
                        std::abs(size.y - height) < 0.01f) {
                        selectedSprite_->SetSize({width / static_cast<float>(settings.frameCount), height});
                    }
                    // 分割解除・設定途中では前に表示したコマのUVを残しません。
                    if (!canSplit || settings.frameCount == 1)
                        selectedSprite_->SetTextureRect({0.0f, 0.0f}, {width, height});
                }
            }
            // 開始ボタンは再生時刻をゼロに戻す。停止ボタンでは経過秒の更新を止める。
            if (ImGui::Button("先頭からプレビュー")) {
                layoutDocument_->previewStart = std::chrono::steady_clock::now();
                layoutDocument_->previewPlaying = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("停止"))
                layoutDocument_->previewPlaying = false;
        }

		// 押した側に応じて描画順の添字を1つ移動し、前後のSpriteと順番を入れ替える。
		if (ImGui::Button(ICON_FA_ARROW_UP " 前面へ (手前)")) {
			MoveEditingSprite(*currentScene, selectedSprite_, 1);
		}

		ImGui::SameLine();

		if (ImGui::Button(ICON_FA_ARROW_DOWN " 背面へ (奥)")) {
			MoveEditingSprite(*currentScene, selectedSprite_, -1);
		}

		ImGui::Separator();

		// 名前欄を編集した場合だけ、選択Spriteの名前へ反映する。
		char nameBuffer[256] = { 0 };
		strcpy_s(nameBuffer, selectedSprite_->GetName().c_str());

		if (ImGui::InputText(ICON_FA_PEN " 名前 (Name)", nameBuffer, sizeof(nameBuffer))) {
			selectedSprite_->SetName(nameBuffer);
		}

		// 親がある場合だけ親名と解除ボタンを出し、なければ親なしと表示する。
		if (selectedSprite_->GetParent()) {
			ImGui::TextDisabled(ICON_FA_SITEMAP " Parent: %s", selectedSprite_->GetParent()->GetName().c_str());

			if (ImGui::Button(ICON_FA_UNLINK " Unparent Sprite")) {
				selectedSprite_->SetParent(nullptr);
			}
		} else {
			ImGui::TextDisabled(ICON_FA_SITEMAP " Parent: None");
		}

		ImGui::Separator();

		// 位置・寸法・アンカー・色・発光は、それぞれの欄を操作して値が変わった場合だけ反映する。
		engine::Vector2 pos = selectedSprite_->GetPosition();
		engine::Vector2 size = selectedSprite_->GetSize();
		engine::Vector2 anchor = selectedSprite_->GetAnchorPoint();
		engine::Vector4 color = selectedSprite_->GetColor();
		float emissive = selectedSprite_->GetEmissive();
		if (ImGui::DragFloat2(ICON_FA_ARROWS_ALT " 座標 (Pos)", &pos.x, 1.0f))
			selectedSprite_->SetPosition(pos);
		if (ImGui::DragFloat2(ICON_FA_EXPAND_ARROWS_ALT " サイズ (Size)", &size.x, 1.0f))
			selectedSprite_->SetSize(size);
		engine::Vector2 texturePixelSize{};

		// 元画像の幅と高さを取得できた場合だけ、倍率と縦横比の補正を提示する。
		if (TryGetTexturePixelSize(selectedSprite_, texturePixelSize)) {
			const engine::Vector2 currentSize = selectedSprite_->GetSize();
			const float scaleX = texturePixelSize.x > 0.0f ? currentSize.x / texturePixelSize.x : 0.0f;
			const float scaleY = texturePixelSize.y > 0.0f ? currentSize.y / texturePixelSize.y : 0.0f;
			const float aspectError = CalcAspectError(selectedSprite_, texturePixelSize);
			ImGui::Text("Texture: %.0f x %.0f px", texturePixelSize.x, texturePixelSize.y);
			ImGui::Text("Display: %.1f x %.1f / Scale %.2f x %.2f", currentSize.x, currentSize.y, scaleX, scaleY);

			// 元画像と2%以上比率が違う場合には、差の割合を表示する。
			if (aspectError >= kSpriteAspectWarningThreshold) {
				ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), "縦横比が %.1f%% ずれています", aspectError * 100.0f);
			}

			// 選択した補正ボタンに応じて、原寸、幅維持、高さ維持、または0.25刻みの倍率を適用する。
			if (ImGui::Button("実画像サイズに合わせる")) {
				selectedSprite_->SetSize(texturePixelSize);
			}

			ImGui::SameLine();

			if (ImGui::Button("横幅維持で比率補正")) {
				selectedSprite_->SetSize(MakeAspectFixedSize(selectedSprite_, texturePixelSize, true));
			}

			if (ImGui::Button("高さ維持で比率補正")) {
				selectedSprite_->SetSize(MakeAspectFixedSize(selectedSprite_, texturePixelSize, false));
			}

			ImGui::SameLine();

			if (ImGui::Button("近い0.25倍率に補正")) {
				selectedSprite_->SetSize(MakeNearestIntegerScaleSize(selectedSprite_, texturePixelSize));
			}
		}

		if (ImGui::DragFloat2(ICON_FA_ANCHOR " アンカー (Anchor)", &anchor.x, 0.01f))
			selectedSprite_->SetAnchorPoint(anchor);
		if (ImGui::ColorEdit4(ICON_FA_PALETTE " 色 (Color)", &color.x))
			selectedSprite_->SetColor(color);
		if (ImGui::DragFloat(ICON_FA_SUN " Emissive (発光)", &emissive, 0.1f, 1.0f, 50.0f, "%.1f")) {
			selectedSprite_->SetEmissive(emissive);
		}

		ImGui::Separator();

		// クイック配置
		ImGui::Text(ICON_FA_CROSSHAIRS " クイック配置 (Quick Snap)");
		float gw = (float)WinApp::GetClientWidth();
		float gh = (float)WinApp::GetClientHeight();
		ImVec2 btnSz(30.0f, 30.0f);
		// 画面内の基準点と画像のアンカーを同じ割合に揃え、角や中央へ置く。
		auto quickSnapButton = [&](const char* label, float xRatio, float yRatio) {
			if (ImGui::Button(label, btnSz)) {
				selectedSprite_->SetPosition({ gw * xRatio, gh * yRatio });
				selectedSprite_->SetAnchorPoint({ xRatio, yRatio });
			}
			};

		quickSnapButton("↖", 0.0f, 0.0f);
		ImGui::SameLine();
		quickSnapButton("⬆", 0.5f, 0.0f);
		ImGui::SameLine();
		quickSnapButton("↗", 1.0f, 0.0f);

		quickSnapButton("⬅", 0.0f, 0.5f);
		ImGui::SameLine();
		quickSnapButton("・", 0.5f, 0.5f);
		ImGui::SameLine();
		quickSnapButton("➡", 1.0f, 0.5f);

		quickSnapButton("↙", 0.0f, 1.0f);
		ImGui::SameLine();
		quickSnapButton("⬇", 0.5f, 1.0f);
		ImGui::SameLine();
		quickSnapButton("↘", 1.0f, 1.0f);

		ImGui::Separator();

		// 画像差し替え
		ImGui::Text(ICON_FA_IMAGE " テクスチャ変更 (Texture Replace)");
		ImGui::Button("下から画像をここにドロップ！", ImVec2(-1, 30.0f));

		if (ImGui::BeginDragDropTarget()) {
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SPRITE_FILE")) {
				// 画像をドロップしたら、置き換え先で再生中の動画を止めてから画像を設定する。
				const char* droppedFilename = (const char*)payload->Data;
                if (layoutDocument_ && layoutDocument_->videoSprite == selectedSprite_)
                    layoutDocument_->StopVideo();
				uint32_t newHandle = engine::Sprite::LoadTexture(droppedFilename);
				// 描画用ハンドルと保存用の参照パスを同じ画像に更新する。
				selectedSprite_->SetTextureHandle(newHandle);
				selectedSprite_->SetTextureName(droppedFilename);
			}

			ImGui::EndDragDropTarget();
		}

		ImGui::Separator();
		ImGui::Dummy(ImVec2(0.0f, 5.0f));
		// 選択解除を押すと、Inspectorの対象を外す。
		if (ImGui::Button(ICON_FA_TIMES_CIRCLE " 選択解除 (Deselect)", ImVec2(-1, 0)))
			selectedSprite_ = nullptr;

	}
	else {
		ImGui::TextDisabled(ICON_FA_MOUSE_POINTER " リストからスプライトを選択してください");
	}

	ImGui::End();
#endif
}

// Resources/spriteの画像と子フォルダーを一覧にし、配置用のペイロードを渡す。
void SpriteDebugEditor::DrawProjectWindow() {
#ifdef USE_IMGUI
	ImGui::Begin(ICON_FA_FOLDER_OPEN " Sprite Assets");

	std::string baseDirPath = "Resources/sprite/";

	// ルート以外では一つ上に戻るボタンを表示する。
	if (currentSpriteDirectory_ != baseDirPath) {
		if (ImGui::Button(ICON_FA_ARROW_UP " Back")) {

			fs::path p(currentSpriteDirectory_);
			currentSpriteDirectory_ = p.parent_path().parent_path().generic_string() + "/";

			// ルート（baseDirPath）より上に戻りすぎないように最終ガード
			if (currentSpriteDirectory_.length() < baseDirPath.length() ||
				currentSpriteDirectory_.find(baseDirPath) == std::string::npos) {
				currentSpriteDirectory_ = baseDirPath;
			}
		}

		ImGui::SameLine();
	}

	ImGui::TextDisabled("%s", currentSpriteDirectory_.c_str());
	ImGui::Separator();
	
	// 表示対象のフォルダーがあるときだけ一覧を組み立てる。
	if (fs::exists(currentSpriteDirectory_)) {

		float thumbnailSize = 64.0f;
		float padding = 16.0f;
		float cellSize = thumbnailSize + padding;

		float panelWidth = ImGui::GetContentRegionAvail().x;
		// サムネイルと余白を一セルとして列数を決め、幅が狭くても最低一列を表示する。
		std::int32_t columnCount = std::max(1, (std::int32_t)(panelWidth / cellSize));

		// 列数を決めたテーブルが開けたときに内容を描画する。
		if (ImGui::BeginTable("SpriteAssetTable", columnCount)) {

			for (const auto& entry : fs::directory_iterator(currentSpriteDirectory_)) {

					// フォルダーは下の階層を開くボタンとして表示する。
				if (entry.is_directory()) {
					ImGui::TableNextColumn();
					std::string folderName = entry.path().filename().string();

					ImGui::PushID(folderName.c_str());
					ImGui::BeginGroup();

					ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
					ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
					ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
					ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.7f, 0.2f, 1.0f));

					// 一時的に文字サイズを3倍にする
					ImGui::SetWindowFontScale(3.0f);

					// フォルダーボタンを押したら、次の描画でその中身を表示する。
					if (ImGui::Button(ICON_FA_FOLDER, ImVec2(thumbnailSize, thumbnailSize))) {
						currentSpriteDirectory_ = entry.path().generic_string() + "/";
					}

					ImGui::SetWindowFontScale(1.0f); // 後続の項目に拡大率を残さない。
					ImGui::PopStyleColor(4);

					// フォルダ名表示
					ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + thumbnailSize);
					ImGui::TextWrapped("%s", folderName.c_str());
					ImGui::PopTextWrapPos();

					ImGui::EndGroup();
					ImGui::PopID();
				}

				// PNGファイルだけを画像配置の候補にする。
				else if (entry.is_regular_file() && entry.path().extension() == ".png") {
					std::error_code ec;

					// 空ファイルやサイズを取得できない項目は、画像の読み込みに渡さない。
					if (fs::file_size(entry.path(), ec) == 0 || ec) {
						continue;
					}

					ImGui::TableNextColumn();
					std::string filename = entry.path().filename().string();

					// ドラッグ＆ドロップ（エンジンへの送信）用の相対パス
					std::string relativePath = fs::relative(entry.path(), baseDirPath).generic_string();
					std::string fullPath = entry.path().generic_string();

					ImGui::PushID(filename.c_str());

					uint32_t texHandle = engine::TextureManager::GetInstance()->Load(fullPath);

					// テクスチャを取得できなければ、画像ボタンを描かず項目のIDを解除する。
					if (texHandle == 0) {
						ImGui::PopID();
						continue;
					}

					D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = engine::SrvManager::GetInstance()->GetGpuDescriptorHandle(texHandle);

					// 長辺を一覧の枠に合わせ、短辺を画像の比率で縮める。
					const DirectX::TexMetadata& metadata = engine::TextureManager::GetInstance()->GetMetadata(texHandle);
					float w = (float)metadata.width;
					float h = (float)metadata.height;
					float drawW = thumbnailSize;
					float drawH = thumbnailSize;

					if (w > h) {
						drawH = thumbnailSize * (h / w);
					}
					else if (h > w) { drawW = thumbnailSize * (w / h); }
					float offsetX = (thumbnailSize - drawW) * 0.5f;
					float offsetY = (thumbnailSize - drawH) * 0.5f;

					ImGui::BeginGroup();

					// 短辺側に余白がある場合だけ、画像が枠の中央に来るようずらす。
					if (offsetY > 0.0f)
						ImGui::Dummy(ImVec2(1.0f, offsetY));
					if (offsetX > 0.0f)
						ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);

					ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
					ImGui::ImageButton(filename.c_str(), (ImTextureID)(uintptr_t)gpuHandle.ptr, ImVec2(drawW, drawH));
					ImGui::PopStyleColor();

					// 画像をドラッグした場合、配置側が画像ルートを補って開けるよう相対パスを渡す。
					if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
						ImGui::SetDragDropPayload("SPRITE_FILE", relativePath.c_str(), relativePath.size() + 1);

						ImGui::Image((ImTextureID)(uintptr_t)gpuHandle.ptr, ImVec2(drawW * 0.5f, drawH * 0.5f));
						ImGui::SameLine();
						ImGui::Text("Place: %s", filename.c_str());

						ImGui::EndDragDropSource();
					}

					// 画像の上に余白を入れた場合は同じ高さを下にも取り、名前の開始位置をそろえる。
					if (offsetY > 0.0f)
						ImGui::Dummy(ImVec2(1.0f, offsetY));

				
					ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + thumbnailSize);
					ImGui::TextWrapped("%s", filename.c_str());
					ImGui::PopTextWrapPos();

					ImGui::EndGroup();
					ImGui::PopID();
				}
			}

			ImGui::EndTable();
		}
	}
	else {
		// フォルダが見つからなかった時の警告色
		ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "Directory Not Found: %s", currentSpriteDirectory_.c_str());
	}

	ImGui::End();
#endif
}


void SpriteDebugEditor::DrawLayoutFileControls(BaseScene& scene) {
#ifdef USE_IMGUI
    // 独立レイアウトの欄を閉じている間は、ファイル操作を描かない。
    if (!ImGui::CollapsingHeader("独立レイアウト", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    // ファイルを開いている間は保存・閉じる操作を出す。開いていない間はパス入力と新規・読込を出す。
    if (layoutDocument_) {
        const auto path = layoutDocument_->path.filename().u8string();
        ImGui::TextUnformatted(reinterpret_cast<const char*>(path.c_str()));
        if (ImGui::Button("独立レイアウトを保存"))
            SaveLayoutDocument();
        ImGui::SameLine();
        if (ImGui::Button("閉じる"))
            ImGui::OpenPopup("独立レイアウトを閉じる");
        if (ImGui::BeginPopupModal("独立レイアウトを閉じる", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("保存していない編集は失われます。");
            // 閉じる操作を確認したときだけ、選択・動画・編集中の文書を解放する。
            if (ImGui::Button("閉じてシーン編集へ")) {
                ClearSceneSelection();
                layoutDocument_.reset();
                layoutFileStatus_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("キャンセル"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    } else {
        ImGui::InputText("JSONパス (Resources/json/)", layoutPathInput_, sizeof(layoutPathInput_));
        if (ImGui::Button("開く"))
            OpenLayoutDocument(scene.GetSpriteCommon(), false);
        ImGui::SameLine();
        if (ImGui::Button("新規レイアウト"))
            OpenLayoutDocument(scene.GetSpriteCommon(), true);
    }

    // 直前のファイル操作の結果があれば表示する。
    if (!layoutFileStatus_.empty())
        ImGui::TextWrapped("%s", layoutFileStatus_.c_str());
#else
    (void)scene;
#endif
}

// 独立レイアウトの新規作成か読込を行い、検証と初期化に成功した文書へ編集対象を切り替える。
void SpriteDebugEditor::OpenLayoutDocument(engine::SpriteCommon* common, bool createNew) {
    using nlohmann::json;
    // Spriteを初期化する共通基盤がない場合は文書を開かない。
    if (!common)
        return;

    try {
        // プロジェクト内のJSONを開き、読み込みが完了してから編集対象を切り替えます。
        const std::u8string input(reinterpret_cast<const char8_t*>(layoutPathInput_));
        const fs::path root = fs::canonical("Resources/json");
        const fs::path relative(input);
        const fs::path target = fs::weakly_canonical(root / relative);
        const auto local = target.lexically_relative(root);
        // 指定が相対パスで、解決後もResources/json内の既存フォルダーを指すJSONに限定する。
        if (relative.has_root_path() || local.empty() || local.is_absolute() ||
            *local.begin() == L".." || target.extension() != L".json" ||
            !fs::is_directory(target.parent_path())) {
            layoutFileStatus_ = "Resources/json 内の既存フォルダーと .json ファイル名を指定してください。";
            return;
        }

        auto document = std::make_unique<LayoutDocument>();
        document->path = target;
        document->isNew = createNew;
        // 新規文書は既存ファイルと重複しない場合だけ、現在の画面寸法で空の配置を用意する。
        if (createNew) {
            if (fs::exists(target)) {
                layoutFileStatus_ = "同名のファイルがあります。「開く」か別の名前を指定してください。";
                return;
            }
            document->source = {{"designResolution", {WinApp::GetClientWidth(), WinApp::GetClientHeight()}},
                                {"scaleToWindow", true}, {"sprites", json::array()}};
        } else {
            // 既存文書を開く場合は、sprites配列を含むJSONオブジェクトが必要。
            std::ifstream file(target);
            if (!(file >> document->source) || !document->source.is_object() ||
                !document->source.contains("sprites") || !document->source["sprites"].is_array()) {
                layoutFileStatus_ = "sprites 配列のあるレイアウトJSONを開いてください。";
                return;
            }
        }

        // 表示倍率は開いた時点で確定し、保存時に逆変換して設計ピクセルへ戻します。
        const auto scale = spriteLayoutScaler::Make(document->source);
        if (scale.enabled && scale.ratio.x > 0.0f && scale.ratio.y > 0.0f)
            document->displayRatio = scale.ratio;
        std::unordered_map<std::string, engine::Sprite*> byName;
        for (const auto& row : document->source["sprites"]) {
            // 部品の名前を親子の参照に使うため、名前のない行や重複名を持つ文書は開かない。
            if (!row.is_object() || spriteLayoutScaler::ReadString(row, "name", "").empty()) {
                layoutFileStatus_ = "名前がないスプライトが含まれているため、開けませんでした。";
                return;
            }
            auto sprite = spriteLayoutFactory::Create(common, row, scale, "sprite");
            if (!byName.emplace(sprite->GetName(), sprite.get()).second) {
                layoutFileStatus_ = "同名のスプライトが複数あるため、開けませんでした。";
                return;
            }

            // 切り抜きが4個の数値で指定されている場合は、プレビューでも画像の同じ範囲を使う。
            const auto crop = row.find("textureCrop");
            if (crop != row.end() && crop->is_array() && crop->size() == 4 &&
                std::all_of(crop->begin(), crop->end(), [](const auto& value) { return value.is_number(); })) {
                sprite->SetTextureRect({(*crop)[0].get<float>(), (*crop)[1].get<float>()},
                                      {(*crop)[2].get<float>(), (*crop)[3].get<float>()});
            }
            sprite->SetRotation(spriteLayoutScaler::ReadFloat(row, "rotation", 0.0f));
            sprite->SetVisible(true);
            document->originalSprites.emplace(sprite.get(), row);
            document->sprites.push_back(std::move(sprite));
        }

        // 名前で親子を結び、保存値は親からの相対位置として扱います。
        for (const auto& sprite : document->sprites) {
            const auto& row = document->originalSprites.at(sprite.get());
            const auto parentName = spriteLayoutScaler::ReadString(row, "parentName", "");
            // 親名がないSpriteはルートとしてそのまま置く。
            if (parentName.empty())
                continue;
            const auto parent = byName.find(parentName);
            // 指定の親が同じ文書に存在しない場合は、途中まで組んだ配置を公開せず終了する。
            if (parent == byName.end()) {
                layoutFileStatus_ = "参照先の親スプライトが見つかりません。";
                return;
            }
            sprite->SetParent(parent->second, false);
            // 設定後に親が一致しない場合は、拒否された親付けとして読込を中止する。
            if (sprite->GetParent() != parent->second) {
                layoutFileStatus_ = "親子関係が循環しているため、開けませんでした。";
                return;
            }
        }

        // 全Spriteと親子関係を検証してから、背景を準備して編集中の文書へ切り替える。
        document->background = std::make_unique<engine::Sprite>();
        document->background->Initialize(common, engine::Sprite::LoadTexture("common/white.png"));
        document->background->SetColor({0.075f, 0.075f, 0.085f, 1.0f});
        document->background->SetAnchorPoint({0.0f, 0.0f});
        document->background->SetVisible(true);
        ClearSceneSelection();
        layoutDocument_ = std::move(document);
        layoutFileStatus_.clear();
    } catch (const std::exception&) {
        layoutFileStatus_ = "レイアウトJSONを読み込めませんでした。";
    }
}

// 独立レイアウトの変更を新規保存または更新し、成功した内容を次回保存時の比較元にする。
void SpriteDebugEditor::SaveLayoutDocument() {
    using nlohmann::json;
    auto& document = *layoutDocument_;
    try {
        // 元ファイルが別のエディターから更新された場合は、現在の作業と一緒に保持します。
        if (document.isNew ? fs::exists(document.path) : !fs::is_regular_file(document.path)) {
            layoutFileStatus_ = "保存先の状態が変わりました。ファイルを確認してください。";
            return;
        }
        // 既存ファイルの保存では、開いた時のJSONから内容が変わっていれば上書きしない。
        if (!document.isNew) {
            std::ifstream file(document.path);
            json latest;
            if (!(file >> latest) || latest != document.source) {
                layoutFileStatus_ = "ディスク上のJSONが変わっています。ファイルを確認してください。";
                return;
            }
        }

        // 元の行に編集値を重ね、選択画像など利用側の追加項目を保持します。
        json output = document.source;
        auto& rows = output["sprites"];
        rows = json::array();
        std::unordered_set<std::string> names;
        for (const auto& sprite : document.sprites) {
            // 保存時にも空の名前や重複名を拒否し、親子の参照を曖昧にしない。
            if (sprite->GetName().empty() || !names.insert(sprite->GetName()).second) {
                layoutFileStatus_ = "各スプライトに重複しない名前を付けてください。";
                return;
            }
            const auto original = document.originalSprites.find(sprite.get());
            json row = original == document.originalSprites.end() ? json::object() : original->second;
            const auto ratio = document.displayRatio;
            const auto pos = sprite->GetPosition();
            const auto size = sprite->GetSize();
            const auto anchor = sprite->GetAnchorPoint();
            const auto color = sprite->GetColor();
            row["name"] = sprite->GetName();
            row["texture"] = sprite->GetTextureName();
            row["position"] = {pos.x / ratio.x, pos.y / ratio.y};
            row["size"] = {size.x / ratio.x, size.y / ratio.y};
            row["anchor"] = {anchor.x, anchor.y};
            row["color"] = {color.x, color.y, color.z, color.w};
            row["emissive"] = sprite->GetEmissive();
            row["parentName"] = sprite->GetParent() ? sprite->GetParent()->GetName() : "";
            rows.push_back(std::move(row));
        }

        // 完成したJSONだけを同じフォルダー内で置き換え、書き込み途中の内容を公開しません。
        const std::string serialized = output.dump(4) + '\n';
        wchar_t temporaryName[MAX_PATH]{};
        // 同じフォルダー内に一時ファイルを確保できなければ保存先を変更しない。
        if (!GetTempFileNameW(document.path.parent_path().c_str(), L"spr", 0, temporaryName)) {
            layoutFileStatus_ = "保存用の一時ファイルを作れませんでした。";
            return;
        }
        const fs::path temporary(temporaryName);
        std::ofstream file(temporary, std::ios::binary);
        file << serialized;
        file.close();
        const DWORD flags = MOVEFILE_WRITE_THROUGH | (document.isNew ? 0 : MOVEFILE_REPLACE_EXISTING);
        // 一時ファイルへの書き込み失敗または置換失敗時は、残った一時ファイルを片付ける。
        if (!file || !MoveFileExW(temporary.c_str(), document.path.c_str(), flags)) {
            std::error_code error;
            fs::remove(temporary, error);
            layoutFileStatus_ = "レイアウトJSONを保存できませんでした。";
            return;
        }
        // 保存できたJSONと各Spriteの対応を記録し、次回から既存ファイルとして扱う。
        document.source = std::move(output);
        document.originalSprites.clear();
        for (size_t i = 0; i < document.sprites.size(); ++i)
            document.originalSprites.emplace(document.sprites[i].get(), document.source["sprites"][i]);
        document.isNew = false;
        layoutFileStatus_ = "独立レイアウトを保存しました。";
    } catch (const std::exception&) {
        layoutFileStatus_ = "レイアウトJSONを保存できませんでした。";
    }
}

void SpriteDebugEditor::SaveSpriteLayout(const std::string& filename) {
    // 現在のSprite配置をScene別JSONへ書き出し、再読込時の位置と見た目を復元します。
	using json = nlohmann::json;

	// 管理クラスかシーンがなければ、書き出す配置を取得できない。
	if (sceneManager_ == nullptr)
		return;
	BaseScene* currentScene = sceneManager_->GetCurrentScene();
	if (currentScene == nullptr)
		return;

	// 未知の画面参照も残し、Editorで保存しても事前ロードの依存を失わないようにする。
	json root = json::object();
	{
		std::ifstream existing(filename);

		// 既存ファイルを開けた場合は、先に現在の全体設定を読む。
		if (existing.is_open()) {
			try {
				existing >> root;
				// 既存ファイルの構造が想定外なら、他の設定を失う上書きを避ける。
				if (!root.is_object())
					return;
			} catch (const json::exception&) {
				return;
			}
		}
	}

	json spriteArray = json::array();

	auto& sprites = currentScene->GetSprites();

	for (const auto& spritePtr : sprites) {
		engine::Sprite* sprite = spritePtr.get();
		// 一覧の空の項目を出力から除く。
		if (sprite == nullptr)
			continue;

		json spriteData;

		// 名前がなければ保存する行にアドレスを用いた仮名を付ける。
		if (!sprite->GetName().empty()) {
			spriteData["name"] = sprite->GetName();
		}
		else {

			spriteData["name"] = "NoName_" + std::to_string((size_t)sprite);
		}

		engine::Vector2 pos = sprite->GetPosition();
		engine::Vector2 size = sprite->GetSize();
		engine::Vector2 anchor = sprite->GetAnchorPoint();
		engine::Vector4 color = sprite->GetColor();
		spriteData["position"] = { pos.x, pos.y };
		spriteData["size"] = { size.x, size.y };
		spriteData["anchor"] = { anchor.x, anchor.y };
		spriteData["color"] = { color.x, color.y, color.z, color.w };
		spriteData["texture"] = sprite->GetTextureName();
		spriteData["emissive"] = sprite->GetEmissive();
		spriteData["parentName"] = sprite->GetParent() ? sprite->GetParent()->GetName() : "";
		spriteArray.push_back(spriteData);
	}

	// 保存した配置のピクセル基準を併記し、読み込み時の拡縮に使えるようにする。
	root["designResolution"] = {
		static_cast<float>(WinApp::GetClientWidth()),
		static_cast<float>(WinApp::GetClientHeight())
	};

	root["scaleToWindow"] = true;
	root["sprites"] = spriteArray;

	std::ofstream file(filename);

	// 保存先を開けた場合にだけ整形したJSONを書き込む。
	if (file.is_open()) {
		file << root.dump(4);
		file.close();
	}
}

bool SpriteDebugEditor::IsMouseOver(engine::Sprite* sprite, const engine::Vector2& localMousePos) const {
	// Spriteの参照がない場合は範囲を計算せず、ヒットなしを返す。
	if (sprite == nullptr)
		return false;

	engine::Vector2 pos = sprite->GetWorldPosition();
	engine::Vector2 size = sprite->GetSize();
	engine::Vector2 anchor = sprite->GetAnchorPoint();

	// 左上はワールドの描画位置からアンカーの割合ぶん引く。回転角を使わない軸平行矩形でクリックを判定する。
	float minX = pos.x - (anchor.x * size.x);
	float maxX = minX + size.x;
	float minY = pos.y - (anchor.y * size.y);
	float maxY = minY + size.y;

	return (localMousePos.x >= minX && localMousePos.x <= maxX &&
		localMousePos.y >= minY && localMousePos.y <= maxY);
}

bool SpriteDebugEditor::IsMouseBusy() const {
	// ImGuiがマウスを占有している場合にtrue。ImGuiを使わない構成ではfalse。
#ifdef USE_IMGUI
	return ImGui::GetIO().WantCaptureMouse;
#else
	return false;
#endif
}

void SpriteDebugEditor::Draw() {
#ifdef USE_IMGUI
	// 現在のシーンとSprite描画基盤が揃わなければ、キャンバスもギズモも描画できない。
	if (sceneManager_ == nullptr) {
		return;
	}

	BaseScene* currentScene = sceneManager_->GetCurrentScene();

	if (currentScene == nullptr) {
		return;
	}

	engine::SpriteCommon* currentSpriteCommon = currentScene->GetSpriteCommon();

	if (currentSpriteCommon == nullptr) {
		return;
	}

    // 編集用キャンバスはゲームUIの後に描き、実行中のシーンのSpriteリストとは独立させます。
    if (layoutDocument_) {
        auto& document = *layoutDocument_;
        // 再生先のSpriteがあれば前回の描画からの秒数を渡して動画を進める。
        if (document.videoSprite) {
            const auto now = std::chrono::steady_clock::now();
            document.videoPreview->Update(std::chrono::duration<double>(now - document.videoTick).count());
            document.videoTick = now;
            auto* commands = currentSpriteCommon->GetDxCommon()->GetCommandList();
            // 新しい映像が準備できたら画像を差し替える。準備中は現在の画像を使い、エラー時は元の静止画へ戻す。
            if (const auto frame = document.videoPreview->PrepareTextureForDraw(*commands)) {
                spriteLayoutVideo::SetTexture(*document.videoSprite, *frame);
            } else if (document.videoPreview->HasError()) {
                document.StopVideo();
                layoutFileStatus_ = "動画を再生できませんでした。";
            }
        }
        auto& background = *layoutDocument_->background;
        background.SetSize({static_cast<float>(WinApp::GetClientWidth()), static_cast<float>(WinApp::GetClientHeight())});
        background.Update();
        background.Draw();
        const double seconds = layoutDocument_->previewPlaying
            ? std::chrono::duration<double>(std::chrono::steady_clock::now() - layoutDocument_->previewStart).count() : 0.0;
        for (const auto& sprite : layoutDocument_->sprites) {
            // 設定行があり、動画を再生していないSpriteにだけコマアニメーションの画像範囲を反映する。
            const auto row = layoutDocument_->originalSprites.find(sprite.get());
            if (row != layoutDocument_->originalSprites.end() && sprite.get() != layoutDocument_->videoSprite)
                spriteLayoutAnimation::Apply(*sprite, spriteLayoutAnimation::Read(row->second), seconds);
            sprite->Update();
        }
        for (const auto& sprite : layoutDocument_->sprites)
            sprite->Draw();
    }

    // 選択がないかImGuiがマウス操作を使っている場合は、移動ギズモを出さない。
    if (!selectedSprite_ || ImGui::GetIO().WantCaptureMouse)
        return;

	// ギズモをまだ作っていないか、シーンでSprite共通基盤が変わったときに作り直す。
	if (gizmoArrowX_ == nullptr || initializedSpriteCommon_ != currentSpriteCommon) {

		gizmoTextureHandle_ = engine::Sprite::LoadTexture("common/white.png");

		gizmoArrowX_ = std::make_unique<engine::Sprite>();
		gizmoArrowX_->Initialize(currentSpriteCommon, gizmoTextureHandle_);
		gizmoArrowX_->SetSize({ 50.0f, 10.0f });
		gizmoArrowX_->SetColor({ 1.0f, 0.0f, 0.0f, 0.8f });
		gizmoArrowX_->SetAnchorPoint({ 0.0f, 0.5f });

		gizmoArrowY_ = std::make_unique<engine::Sprite>();
		gizmoArrowY_->Initialize(currentSpriteCommon, gizmoTextureHandle_);
		gizmoArrowY_->SetSize({ 10.0f, 50.0f });
		gizmoArrowY_->SetColor({ 0.0f, 1.0f, 0.0f, 0.8f });
		gizmoArrowY_->SetAnchorPoint({ 0.5f, 0.0f });

		// 次のフレームで再利用できるよう、初期化に使用した共通基盤を記録する。
		initializedSpriteCommon_ = currentSpriteCommon;
	}

	// 親付き Sprite でも選択マーカーの基準はローカル位置にせず、表示上のワールド位置へ重ねる。
	engine::Vector2 pos = selectedSprite_->GetWorldPosition();
	gizmoArrowX_->SetPosition(pos);
	gizmoArrowY_->SetPosition(pos);

	// 位置変更後に二本のSpriteの行列を確定してから描画する。
	gizmoArrowX_->Update();
	gizmoArrowY_->Update();

	gizmoArrowX_->Draw();
	gizmoArrowY_->Draw();
#endif
}
