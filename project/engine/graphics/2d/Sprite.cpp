#include <cstdint>
#include "Sprite.h"
#include "DirectXCommon.h"
#include "WinApp.h"
#include <cassert>
#include "TextureManager.h"
#include "SrvManager.h"
#include "engine/graphics/core/ColorSpace.h"
#include "RenderStats.h"
#include <algorithm>
#include <atomic>

namespace engine {

namespace {
std::atomic<uint64_t> gNextSpriteReplayId{ 0x8000000000000001ull };

engine::Matrix4x4 MakeSpriteLocalMatrix(const Sprite* sprite) {
	// Sprite未指定時も親子行列の計算に使える単位姿勢を返す。
	engine::Math math;
	engine::Vector2 position = sprite ? sprite->GetPosition() : engine::Vector2::kZero;
	float rotation = sprite ? sprite->GetRotation() : 0.0f;
	return math.MakeAffineMatrix(
		{ 1.0f, 1.0f, 1.0f },
		{ 0.0f, 0.0f, rotation },
		{ position.x, position.y, 0.0f });
}

engine::Matrix4x4 MakeSpriteWorldMatrix(const Sprite* sprite) {
	// 親の行列を再帰的に掛け、子のローカル座標を描画時のワールド座標へ変換する。
	engine::Math math;
	engine::Matrix4x4 localMatrix = MakeSpriteLocalMatrix(sprite);
	Sprite* parent = sprite ? sprite->GetParent() : nullptr;

	// 親がなければローカル変換がそのまま画面上の変換になる。
	if (!parent) {
		return localMatrix;
	}

	return math.Multiply(localMatrix, MakeSpriteWorldMatrix(parent));
}

// 親Spriteの連鎖に指定対象が含まれるか確認し、親子循環を防ぎます。
bool HasParentInChain(Sprite* sprite, const Sprite* parent) {
	for (Sprite* current = sprite; current != nullptr; current = current->GetParent()) {
		// たどった親の一つが指定のSpriteなら、親子の循環を作る指定として検出する。
		if (current == parent) {
			return true;
		}
	}

	return false;
}

// テクスチャ未指定時は白DDS、取得できなければ白PNGのハンドルを返します。
uint32_t ResolveSpriteTextureHandle(uint32_t textureHandle) {
	// 画像ハンドルが指定されていれば、その画像を優先する。
	if (textureHandle != 0) {
		return textureHandle;
	}

	// 未指定テクスチャは白画像で描き、DDSがない環境ではPNGへフォールバックする。
	engine::TextureManager* textureManager = engine::TextureManager::GetInstance();
	uint32_t fallback = textureManager->Load("Resources/sprite/common/white.dds", false, false);

	// 白DDSを取得できなかった場合だけ白PNGを読み込む。
	if (fallback == 0) {
		fallback = textureManager->Load("Resources/sprite/common/white.png", false, false);
	}

	return fallback;
}
}

Sprite::~Sprite() {
	// 親子は所有関係ではないため、破棄時には相互参照だけ外す。
	SetParent(nullptr, false);

	for (Sprite* child : children_) {
		// まだ有効な子の親参照を解除し、破棄するSpriteが残らないようにする。
		if (child) {
			child->parent_ = nullptr;
		}
	}

	children_.clear();
}

// 画像をファイルから読み込み、ハンドルを使う共通の初期化へ進む。
void Sprite::Initialize(engine::SpriteCommon* common, const std::string& textureFilePath) {
	uint32_t handle = engine::TextureManager::GetInstance()->Load(textureFilePath);
	Initialize(common, handle);
}

// 共有描画機能を借り、画像と矩形のGPU資源を作る。初期化に失敗した資源がある場合は非表示にする。
void Sprite::Initialize(engine::SpriteCommon* common, uint32_t textureHandle) {
	assert(common);
	common_ = common;
	dxCommon_ = common_->GetDxCommon();

	textureHandle_ = ResolveSpriteTextureHandle(textureHandle);

	// 画像ハンドルを取得できた場合は画像全体の寸法を初期表示に使う。
	if (textureHandle_ != 0) {
		AdjustTextureSize(); // テクスチャサイズに合わせてスプライトのサイズも調整
	}
	else {
		// フォールバックも取得できない場合は描画だけ止め、初期化処理自体は継続する。
		size_ = { 1.0f, 1.0f };
		textureSize_ = { 1.0f, 1.0f };
		isVisible_ = false;
	}

	// 四頂点と六インデックスの矩形を生成し、毎フレーム更新するバッファをマップする。
	vertexData_ = nullptr;
	wvpData_ = nullptr;
	materialData_ = nullptr;
	vertexResource_ = dxCommon_->CreateBufferResource(sizeof(VertexData) * 4);
	// 頂点バッファを作成またはマップできなければ、矩形の位置を更新できないため非表示で終了する。
	if (!vertexResource_ || FAILED(vertexResource_->Map(0, nullptr, reinterpret_cast<void**>(&vertexData_))) || !vertexData_) {
		isVisible_ = false;
		return;
	}
	vertexBufferView_ = { vertexResource_->GetGPUVirtualAddress(), sizeof(VertexData) * 4, sizeof(VertexData) };

	indexResource_ = dxCommon_->CreateBufferResource(sizeof(uint32_t) * 6);
	uint32_t* indexData = nullptr; // マッピング用のポインタ
	// インデックスの書き込み先が使えなければ、矩形を構成する三角形を登録できない。
	if (!indexResource_ || FAILED(indexResource_->Map(0, nullptr, reinterpret_cast<void**>(&indexData))) || !indexData) {
		isVisible_ = false;
		return;
	}
	indexBufferView_ = { indexResource_->GetGPUVirtualAddress(), sizeof(uint32_t) * 6, DXGI_FORMAT_R32_UINT };
	indexData[0] = 0;
	indexData[1] = 1;
	indexData[2] = 2;
	indexData[3] = 1;
	indexData[4] = 3;
	indexData[5] = 2;
	indexResource_->Unmap(0, nullptr); // アンマップ

	// 頂点位置と別に、階層変換・投影とマテリアル色をGPUへ渡す。
	wvpResource_ = dxCommon_->CreateBufferResource(sizeof(TransformationMatrix));
	// 変換の定数バッファが使えない場合は、描画に必要な行列を書き込む前に終了する。
	if (!wvpResource_ || FAILED(wvpResource_->Map(0, nullptr, reinterpret_cast<void**>(&wvpData_))) || !wvpData_) {
		isVisible_ = false;
		return;
	}

	engine::Math math;
	wvpData_->wvp = math.MakeIdentity4x4();
	wvpData_->world = math.MakeIdentity4x4();

	materialResource_ = dxCommon_->CreateBufferResource(sizeof(Material));
	// マテリアルの定数バッファが使えない場合は、色や発光を設定できないため非表示にする。
	if (!materialResource_ || FAILED(materialResource_->Map(0, nullptr, reinterpret_cast<void**>(&materialData_))) || !materialData_) {
		isVisible_ = false;
		return;
	}
	color_ = { 1.0f, 1.0f, 1.0f, 1.0f };
	materialData_->color = engine::colorSpace::AuthoringToWorking(color_);
	materialData_->emissive = 1.0f;
}

// 再生中のコマ、アンカーからの矩形、親子変換を更新し、描画用バッファへ書き込む。
void Sprite::Update() {
	// GPU資源の生成やMapが失敗したSpriteは頂点へ書き込めない。死亡演出などからの直接更新も安全に抜ける。
	if (!vertexResource_ || !indexResource_ || !wvpResource_ || !materialResource_ ||
		!vertexData_ || !wvpData_ || !materialData_) {
		isVisible_ = false;
		return;
	}
	// マップ済みのマテリアルに、編集色を作業色空間へ変換して書き込む。
	if (materialData_) {
		materialData_->color = engine::colorSpace::AuthoringToWorking(color_);
	}

	// 横一列のスプライトシートを想定し、現在フレームのUV範囲を更新する。
	if (isPlaying_) {
		// このアニメーションは呼び出し一回を1/60秒として進める。
		animationTimer_ += 1.0f / 60.0f;

		// 1コマの時間に達した更新で次のコマへ進み、このアニメーションの蓄積を0へ戻す。
		if (animationTimer_ >= frameDuration_) {
			// 最終フレームを越えたらループか停止を選ぶ。
			currentFrame_++;
			animationTimer_ = 0.0f; // タイマーリセット

			// 最後のコマを越えた場合だけ、繰り返しの設定を調べる。
			if (currentFrame_ >= totalFrames_) {
				// ループ設定ではフレーム番号を0へ戻す。
				if (isLooping_) {
					currentFrame_ = 0; // ループするなら最初に戻る
				// ループしない設定では最後のコマを残して再生を止める。
				} else {
					currentFrame_ = totalFrames_ - 1; // ループしないなら最後のフレームで止める
					isPlaying_ = false;
				}
			}
		}

		// 一枚の画像を横に分割した現在フレームの矩形へ切り替える。
		textureLeftTop_.x = (float)(currentFrame_ * frameWidth_);
		textureLeftTop_.y = 0; // 今回は横一列のアニメーションを想定
		textureSize_.x = (float)frameWidth_;
		textureSize_.y = (float)frameHeight_;
	}

	// アンカー基準の矩形を作り、反転時は左右または上下の座標を入れ替える。
	{
		float left = (0.0f - anchorPoint_.x) * size_.x;
		float right = (1.0f - anchorPoint_.x) * size_.x;
		float top = (0.0f - anchorPoint_.y) * size_.y;
		float bottom = (1.0f - anchorPoint_.y) * size_.y;

		// X軸反転を指定されたら、UVを保ったまま矩形の左右の頂点を交換する。
		if (isFlipX_) {
			std::swap(left, right);
		}

		// Y軸反転を指定されたら、矩形の上下の頂点を交換する。
		if (isFlipY_) {
			std::swap(top, bottom);
		}

		// インデックスバッファの並びに合わせて四隅の座標を配置する。
		vertexData_[0].position = { left, bottom, 0.0f, 1.0f };
		vertexData_[1].position = { left, top, 0.0f, 1.0f };
		vertexData_[2].position = { right, bottom, 0.0f, 1.0f };
		vertexData_[3].position = { right, top, 0.0f, 1.0f };
	}

	// テクスチャ矩形を画像全体の大きさで割り、GPU用の正規化UVへ変換する。
	{
		// 画像ハンドルが0なら、寸法を参照できないためUVと行列の更新を省く。
		if (textureHandle_ == 0) {
			return;
		}

		const DirectX::TexMetadata& metadata = engine::TextureManager::GetInstance()->GetMetadata(textureHandle_);
		float texWidth = (float)metadata.width;
		float texHeight = (float)metadata.height;
		float texLeft = textureLeftTop_.x / texWidth;
		float texRight = (textureLeftTop_.x + textureSize_.x) / texWidth;
		float texTop = textureLeftTop_.y / texHeight;
		float texBottom = (textureLeftTop_.y + textureSize_.y) / texHeight;

		vertexData_[0].texcoord = { texLeft, texBottom };
		vertexData_[1].texcoord = { texLeft, texTop };
		vertexData_[2].texcoord = { texRight, texBottom };
		vertexData_[3].texcoord = { texRight, texTop };
	}

	// 親子変換を含むワールド行列へ画面の正射影を掛ける。
	engine::Math math;
	engine::Matrix4x4 worldMatrix = MakeSpriteWorldMatrix(this);
	engine::Matrix4x4 viewMatrix = math.MakeIdentity4x4();
	// クライアント領域のピクセル座標を画面全体へ正射影します。左上を原点として下へYが増えます。
	engine::Matrix4x4 projectionMatrix = math.MakeOrthographicMatrix(0.0f, 0.0f, (float)WinApp::GetClientWidth(), (float)WinApp::GetClientHeight(), 0.0f, 100.0f);
	engine::Matrix4x4 worldViewProjectionMatrix = math.Multiply(worldMatrix, math.Multiply(viewMatrix, projectionMatrix));

	wvpData_->wvp = worldViewProjectionMatrix;
	wvpData_->world = worldMatrix;
	materialData_->enableLighting = false;
	materialData_->uvTransform = math.MakeIdentity4x4();
}

// 編集用の色を保持し、描画資源があれば作業色空間へ変換して送る。
void Sprite::SetColor(const engine::Vector4& color) {
	color_ = color;

	// マテリアルがマップ済みなら、その場でGPUに渡す色にも反映する。
	if (materialData_) {
		materialData_->color = engine::colorSpace::AuthoringToWorking(color_);
	}
}

// 表示可能なSpriteの矩形を現在のコマンドリストへ記録し、描画統計を更新する。
void Sprite::Draw() {
	// 共有描画機能を取得できていない場合は、コマンドリストを取得しない。
	if (!common_ || !dxCommon_)
		return;
	assert(common_);
	// 非表示、リプレイから除外済み、画像なしのいずれかなら描画を行わない。
	if (!isVisible_ || replayRemoved_ || textureHandle_ == 0)
		return;
	// 頂点・インデックス・行列・マテリアルのGPU資源が揃っていなければ描画を省く。
	if (!vertexResource_ || !indexResource_ || !wvpResource_ || !materialResource_)
		return;
	// CPUから更新する頂点とマテリアルのポインターが取得済みであることも確認する。
	if (!vertexData_ || !materialData_)
		return;
	ID3D12GraphicsCommandList* commandList = dxCommon_->GetCommandList();

	// 更新済みの頂点、変換、マテリアル、テクスチャを束ねて一枚の矩形を描く。
	common_->SetPipeline(commandList);
	commandList->IASetVertexBuffers(0, 1, &vertexBufferView_);
	commandList->IASetIndexBuffer(&indexBufferView_);
	commandList->SetGraphicsRootConstantBufferView(0, wvpResource_->GetGPUVirtualAddress());
	commandList->SetGraphicsRootConstantBufferView(1, materialResource_->GetGPUVirtualAddress());
	engine::SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(commandList, 2, textureHandle_);
	commandList->DrawIndexedInstanced(6, 1, 0, 0, 0);
	engine::RenderStats::GetInstance()->RecordIndexedDraw(6);
}

// 画像全体を使うよう、表示サイズと切り出し範囲を初期化する。
void Sprite::AdjustTextureSize() {
	// 画像ハンドルが無効なら、画像情報を参照せず1×1の初期寸法を使う。
	if (textureHandle_ == 0) {
		size_ = { 1.0f, 1.0f };
		textureSize_ = { 1.0f, 1.0f };
		return;
	}

	const DirectX::TexMetadata& metadata = engine::TextureManager::GetInstance()->GetMetadata(textureHandle_);
	size_ = { (float)metadata.width, (float)metadata.height };
	textureSize_ = { (float)metadata.width, (float)metadata.height };
}

// Sprite配置用の画像パスを解決し、TextureManagerが返すハンドルを返す。
uint32_t Sprite::LoadTexture(const std::string& fileName) {
	return engine::TextureManager::GetInstance()->Load(ResolveTexturePath(fileName));
}

std::string Sprite::ResolveTexturePath(const std::string& fileName) {
	// 配置JSONはSpriteディレクトリ相対、実行時コードはResourcesからのパスも許容する。
	std::string path = fileName;
	std::replace(path.begin(), path.end(), '\\', '/');

	// 先頭の./を繰り返し外し、リソースの判定に使うパスを揃える。
	while (path.rfind("./", 0) == 0) {
		path.erase(0, 2);
	}

	return path.rfind("Resources/", 0) == 0 ? path : "Resources/sprite/" + path;
}


// 横一列の画像を指定コマ数で分け、再生を止めた状態の初期矩形を用意する。
void Sprite::SetAnimation(std::int32_t frameCount, float duration, bool loop) {
	// 画像が未設定なら、コマ寸法を取得できないため今の設定を保つ。
	if (textureHandle_ == 0)
		return;

	// durationはアニメーション全体ではなく1コマの表示秒数。設定変更だけでは再生を始めず、先頭のUVを準備する。
	totalFrames_ = (frameCount > 0) ? frameCount : 1;
	frameDuration_ = duration;
	isLooping_ = loop;
	isPlaying_ = false;
	currentFrame_ = 0;
	animationTimer_ = 0.0f;

	// 画像の横幅をコマ数で整数除算し、各コマには画像全体の高さを使う。
	const DirectX::TexMetadata& metadata = engine::TextureManager::GetInstance()->GetMetadata(textureHandle_);
	frameWidth_ = (std::int32_t)metadata.width / totalFrames_;
	frameHeight_ = (std::int32_t)metadata.height;

	// テクスチャの初期表示範囲とスプライト自体のサイズを1フレーム分に設定
	SetTextureRect({ 0.0f, 0.0f }, { (float)frameWidth_, (float)frameHeight_ });
	SetSize({ (float)frameWidth_, (float)frameHeight_ });
}

// 再生フラグを立て、先頭のコマと0秒から始める。
void Sprite::Play() {
	isPlaying_ = true;
	currentFrame_ = 0;
	animationTimer_ = 0.0f;
}

// 再生を止め、現在のコマと表示範囲を保持する。
void Sprite::Stop() {
	isPlaying_ = false;
}

// 未発行の場合にだけリプレイIDを割り当て、取得済みの同じIDを返す。
uint64_t Sprite::EnsureReplayId() {
	if (replayId_ == 0) {
		// 再生用IDは重複しない番号を発行するだけなので、他のSprite状態の同期には使いません。
		replayId_ = gNextSpriteReplayId.fetch_add(1, std::memory_order_relaxed);
	}

	return replayId_;
}

// 表示、アニメーション進行、親のリプレイIDをまとめて取得する。
Sprite::ReplayState Sprite::CaptureReplayState() const {
	ReplayState state;

	// 見た目の状態だけでなく、アニメーション進行と親子関係のIDも保存する。
	state.position = position_;
	state.rotation = rotation_;
	state.size = size_;
	state.color = color_;
	state.anchorPoint = anchorPoint_;
	state.flipX = isFlipX_;
	state.flipY = isFlipY_;
	state.textureLeftTop = textureLeftTop_;
	state.textureSize = textureSize_;
	state.textureHandle = textureHandle_;
	state.emissive = GetEmissive();
	state.visible = isVisible_;
	state.locked = isLocked_;

	state.animationPlaying = isPlaying_;
	state.animationLooping = isLooping_;
	state.frameDuration = frameDuration_;
	state.animationTimer = animationTimer_;
	state.totalFrames = totalFrames_;
	state.currentFrame = currentFrame_;
	state.frameWidth = frameWidth_;
	state.frameHeight = frameHeight_;

	// 親があれば、復元時の接続に使う親のIDを割り当てて記録する。
	state.parentReplayId = parent_ ? parent_->EnsureReplayId() : 0;
	state.replayRemoved = replayRemoved_;
	return state;
}

void Sprite::RestoreReplayState(const ReplayState& state) {
	// 親の復元順に依存しないよう、いったん親子関係を切ってローカル状態を戻す。
	SetParent(nullptr, false);
	position_ = state.position;
	rotation_ = state.rotation;
	size_ = state.size;
	anchorPoint_ = state.anchorPoint;
	isFlipX_ = state.flipX;
	isFlipY_ = state.flipY;
	textureLeftTop_ = state.textureLeftTop;
	textureSize_ = state.textureSize;
	textureHandle_ = state.textureHandle;
	isVisible_ = state.visible;
	isLocked_ = state.locked;

	// 再生位置と切り出し範囲を同じ保存フレームに揃えてから、描画用の色と行列を再構築します。
	isPlaying_ = state.animationPlaying;
	isLooping_ = state.animationLooping;
	frameDuration_ = state.frameDuration;
	animationTimer_ = state.animationTimer;
	totalFrames_ = state.totalFrames;
	currentFrame_ = state.currentFrame;
	frameWidth_ = state.frameWidth;
	frameHeight_ = state.frameHeight;
	replayRemoved_ = state.replayRemoved;
	SetColor(state.color);
	SetEmissive(state.emissive);
	RefreshAfterReplayRestore();
}

void Sprite::RefreshAfterReplayRestore() {
	// Updateはアニメーションを進めるため、復元フレームの再計算中だけ再生を止める。
	const bool wasPlaying = isPlaying_;
	isPlaying_ = false;
	Update();
	isPlaying_ = wasPlaying;
}

void Sprite::SetParent(Sprite* parent, bool keepWorldPosition) {
	// 自分自身や子孫を親に指定すると行列計算が循環するので拒否する。
	if (parent == this || HasParentInChain(parent, this)) {
		return;
	}

	// 親変更前の表示位置を保存し、必要なら新しい親のローカル座標へ換算する。
	engine::Vector2 worldPosition = GetWorldPosition();

	// 旧親がある場合は、その子一覧からこのSpriteを外す。
	if (parent_) {
		auto& siblings = parent_->children_;
		siblings.erase(std::remove(siblings.begin(), siblings.end(), this), siblings.end());
	}

	parent_ = parent;

	// 新しい親がある場合は、その子一覧へ登録する。
	if (parent_) {
		auto& siblings = parent_->children_;

		// 登録済みの参照を重複して追加しないよう確認する。
		if (std::find(siblings.begin(), siblings.end(), this) == siblings.end()) {
			siblings.push_back(this);
		}
	}

	// 表示位置を維持する指定の場合にだけ、新しい親を基準に位置を変換する。
	if (keepWorldPosition) {
		// 新しい親がある場合は、逆変換で元の画面上の位置を保つ。
		if (parent_) {
			engine::Math math;
			engine::Matrix4x4 parentWorld = MakeSpriteWorldMatrix(parent_);
			// 新しい親の逆変換で画面上の位置をローカルへ戻します。調整するのは位置だけで、自身の回転は保持します。
			engine::Matrix4x4 inverseParent = math.Inverse(parentWorld);
			engine::Vector3 local = math.Transform({ worldPosition.x, worldPosition.y, 0.0f }, inverseParent);
			position_ = { local.x, local.y };
		// 親を外した場合は、元の画面上の位置をローカル位置として使う。
		} else {
			position_ = worldPosition;
		}
	}
}

// 自分と親の変換を反映したアンカーの画面上のピクセル座標を返す。
engine::Vector2 Sprite::GetWorldPosition() const {
	engine::Math math;
	engine::Vector3 world = math.Transform({ 0.0f, 0.0f, 0.0f }, MakeSpriteWorldMatrix(this));
	return { world.x, world.y };
}

} // namespace engine
