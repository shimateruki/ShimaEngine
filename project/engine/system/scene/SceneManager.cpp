#ifdef USE_IMGUI
#include "engine/editor/preview/SelectedActorSession.h"
#endif
#include "SceneManager.h"
#include <stdexcept>

#include "DirectXCommon.h"
#include "LoadingScene.h"
#include "engine/system/scene/LoadingProfiler.h"
#include "ModelManager.h"
#include "TextureManager.h"
#include "MeshEffectManager.h"
#include "DebrisEffectManager.h"
#include "engine/graphics/postprocess/Fade.h"
#ifdef USE_IMGUI
#include "engine/editor/graphics/camera/CameraEditor.h"
#endif
#include "json.hpp"

#include <cassert>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <exception>
#include <utility>
#include <Windows.h>

using json = nlohmann::json;

namespace {
// GPU待機後、Scene固有の実行中演出を片付ける。共有Common自体は破棄しない。
// LoadingSceneから次Sceneへ渡る際は、準備中に生成した演出を消さない。
void ReleaseSceneEffects(BaseScene* scene) {
    // 有効なシーンが3Dの共有基盤を使っていた場合だけ、登録されているメッシュ演出と破片を消す。
    if (scene && scene->GetObject3DCommon()) {
        MeshEffectManager::GetInstance()->Clear();
        // Commonのアドレスが変わらなくても、前Sceneの破片は持ち越さない。
        DebrisEffectManager::GetInstance()->Clear();
    }
}

// 呼び出し元でGPUの参照完了を待った後、演出を止めて現行Sceneを解放する。
void FinalizeCurrentScene(std::unique_ptr<BaseScene>& scene) {
    // 現行シーンがまだ存在しなければ、演出や初期化済み資源の解放を行う必要がない。
    if (!scene) {
        return;
    }
    ReleaseSceneEffects(scene.get());
    scene->Finalize();
    scene.reset();
}

// 遷移段階の所要時間をミリ秒へ直し、遅い処理の診断に渡す。
void LogSlowLoadingStep(const char* operation, const std::string& sceneName,
                        std::chrono::steady_clock::time_point start) {
    const double elapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    LogSlowLoadingOperation(sceneName + " / " + operation, elapsedMs);
}
}

SceneManager* SceneManager::sInstance = nullptr;

SceneManager* SceneManager::GetInstance() {
    // SceneManagerの生存期間はアプリ側が管理し、ここでは参照だけ返す。
    return sInstance;
}

SceneManager::SceneManager() {
    // アプリに一つのSceneManagerを遷移先の問い合わせ窓口として公開する。
    sInstance = this;
}

SceneManager::~SceneManager() {
    // 非同期読込と現行シーンの資源を解放してから破棄する。
    Finalize();
}

// 起動時のシーンを同期的に有効化する。CPUの素材準備は描画基盤の準備と重ねる。
void SceneManager::Initialize(AbstractSceneFactory* factory, const std::string& firstSceneName) {
    sceneFactory_ = factory;
    currentSceneName_ = firstSceneName;
    pendingSceneNameForSwap_.clear();
    loadingTargetSceneName_.clear();
    loadingElapsed_ = 0.0f;

    assert(sceneFactory_ && "SceneFactory is not set in SceneManager.");
    assert(IsSceneRegistered(firstSceneName) && "Initial scene is not registered in SceneFactory.");
    currentScene_ = sceneFactory_->CreateScene(firstSceneName);
    assert(currentScene_ && "Initial scene creation failed.");

    // 初期シーンを生成できなければ、シーン名と遷移状態を戻して初期化を打ち切る。
    if (!currentScene_) {
        currentSceneName_.clear();
        transitionPhase_ = TransitionPhase::kIdle;
        return;
    }

    ++sceneGeneration_;
    currentScene_->SetSceneManager(this);
    currentScene_->SetSceneLoadContext(activeSceneLoadContext_);

    // 共有のデバッグエディターが接続済みなら、初期シーンにも同じ参照を渡す。
    if (debugEditor_) {
        currentScene_->SetDebugEditor(debugEditor_);
    }

    // 初回起動ではローディングSceneを生成せず、最初のSceneを直接初期化します。
    // これによりロードUIの初期化と最低表示時間を省き、ロード画面はScene遷移時だけ表示します。
    engine::TextureManager* textureManager = engine::TextureManager::GetInstance();
    // Copy用バッチが取れたときだけ記録へ切り替え、取得できない場合は既存の転送経路を維持する。
    const bool initialUploadBatch = textureManager->BeginAsyncUploadBatch();
    textureManager->SetAsyncUploadRecording(initialUploadBatch);
    try {
        // アセットのCPU先読みとCommonのGPU準備を重ね、起動時の並列性を維持する。
        // 初期シーンでも、通常のシーン遷移と同じManifest契約を使用する。
        auto manifest = currentScene_->BuildAsyncLoadManifest();
        auto preload = std::async(std::launch::async, [manifest]() {
            return ScenePreloader::Prepare(manifest, nullptr);
        });
        PrepareRenderCommons(currentScene_.get());
        try {
            currentScene_->SetPreparedLoadData(preload.get());
        } catch (...) {
            // CPU先読みの失敗時は既存の同期読込経路を使う。GPU準備失敗は外側で扱う。
            currentScene_->SetPreparedLoadData(nullptr);
        }
        currentScene_->Initialize();
        currentScene_->SetPreparedLoadData(nullptr);
    }
    catch (...) {
        textureManager->SetAsyncUploadRecording(false);

        // テクスチャの転送をバッチに記録していた場合は、失敗時も投入して使用完了まで待つ。
        if (initialUploadBatch) {
            textureManager->SubmitAsyncUploadBatch();
            textureManager->WaitForAsyncUploadBatch();
        }

        // 初期化中に記録したGPU処理を待ってから、部分的に生成したSceneの参照を破棄する。
        ReleaseSceneEffects(currentScene_.get());
        currentScene_.reset();
        currentSceneName_.clear();
        throw;
    }

    textureManager->SetAsyncUploadRecording(false);

    // 初期シーンのテクスチャをバッチに記録した場合は、有効化前にGPUへの転送を終える。
    if (initialUploadBatch) {
        textureManager->SubmitAsyncUploadBatch();
        textureManager->WaitForAsyncUploadBatch();
    }

    // 有効化は初期化と、記録したテクスチャ転送の完了後に行う。
    currentScene_->OnActivated();
    transitionPhase_ = TransitionPhase::kIdle;
}

void SceneManager::Finalize() {
#ifdef USE_IMGUI
    // エディターで選択対象の単体再生がある場合は、シーン解放より先に参照を整理する。
    if (auto* session = SelectedActorSession::Current()) session->PrepareSceneChange();
#endif

    // アセット生成の非同期処理が残っている場合は、終了を待ち、結果を回収する。
    if (assetCreationFuture_.valid()) {
        assetCreationFuture_.wait();
        try {
            assetCreationFuture_.get();
        }
        catch (...) {
        }
    }

    engine::TextureManager::GetInstance()->WaitForAsyncUploadBatch();

    // CPUで先読みしている処理が残っていれば、準備中シーンを破棄する前に待つ。
    if (loadingFuture_.valid()) {
        loadingFuture_.wait();
        try {
            preparedLoadData_ = loadingFuture_.get();
        }
        catch (...) {
            preparedLoadData_.reset();
        }
    }

    // ゲーム内の資源がまだGPUから参照されないよう、現在と準備中の両方を破棄する前に同期する。
    engine::DirectXCommon::GetInstance()->WaitForGpuAndReset();

    FinalizeCurrentScene(currentScene_);

    ++sceneGeneration_;

    ReleaseSceneEffects(preparedScene_.get());

    // 準備シーンを正常に初期化済みの場合だけ、そのシーンの終了処理を呼ぶ。
    if (preparedScene_ && preparedSceneInitialized_) {
        preparedScene_->Finalize();
    }

    // 準備中の状態・読込カーソル・未実行の交換予約を解除する。
    preparedScene_.reset();
    preparedLoadData_.reset();
    preloadProgress_.reset();
    preparedSceneInitialized_ = false;
    preparedTextureIndex_ = 0;
    preparedModelIndex_ = 0;

    nextScene_.reset();
    asyncUploadBatchStarted_ = false;
    asyncUploadBatchSubmitted_ = false;
    assetCreationStarted_ = false;
    assetCreationFinished_ = false;
    preparedAssetsReady_ = false;
    sceneInitializationStarted_ = false;
    sceneInitializationFinished_ = false;
    directTransitionPending_ = false;
    directDestinationHandlesReveal_ = false;
    transitionPhase_ = TransitionPhase::kIdle;
}

void SceneManager::Update(float deltaTime) {
    // 暗転、ローディング、準備済みシーンへの交換を一つの状態機械で進める。
    // 遷移中の時計はゲームが停止していても固定の1/60秒で進め、通常のシーン更新では呼出元の時間を使う。
    const float effectiveDeltaTime = deltaTime > 0.0f ? deltaTime : (1.0f / 60.0f);

    switch (transitionPhase_) {
    // 旧シーンを暗転してから、ロードありと直接交換の経路に分かれる。
    case TransitionPhase::kFadingOutCurrent:
        // 暗転が再生中の間は、旧シーンの交換とロード画面の開始を待つ。
        if (Fade::GetInstance()->GetStatus() == Fade::Status::kFadeOut && !Fade::GetInstance()->IsFinished()) {
            return;
        }

        // ローディングを省く遷移が予約されている場合は、ここから遷移先を直接生成する。
        if (directTransitionPending_) {
            // 暗転が完了してから旧シーンを破棄し、次のシーンを直接初期化する。
            // ロード画面の最低表示時間と追加の暗転を挟まない。
            nextScene_ = sceneFactory_->CreateScene(nextSceneName_);
            nextSceneName_.clear();
            assert(nextScene_ && "Direct scene creation failed.");

            // 直接遷移先の生成に失敗したら、予約を解除して待機状態へ戻す。
            if (!nextScene_) {
                directTransitionPending_ = false;
                transitionPhase_ = TransitionPhase::kIdle;
                return;
            }

            SwapToDirectNextScene();
            // 遷移先が表示を担当する指定でアイリスが動いている場合だけ、既定のフェードイン開始を省く。
            // 遷移先が開示を担当する指定が無いか、遷移先でアイリスインを開始していなければ、既定のフェードインを始める。
            if (!directDestinationHandlesReveal_ ||
                Fade::GetInstance()->GetStatus() != Fade::Status::kIrisIn) {
                Fade::GetInstance()->StartFadeIn(0.25f);
            }

            directTransitionPending_ = false;
            directDestinationHandlesReveal_ = false;
            transitionPhase_ = TransitionPhase::kIdle;
            return;
        }

        // ロード画面を使う予約なら、旧シーンの暗転が終わった時点でロード画面を初期化する。
        BeginLoadingTransition();
        return;

    // ローディング表示中は経過時間を数えながら、先読み結果に応じて初期化を進める。
    case TransitionPhase::kLoading: {
        loadingElapsed_ += effectiveDeltaTime;

        // 現在のロード画面があれば、表示用の時間でアニメーションを進める。
        if (currentScene_) {
            currentScene_->Update(effectiveDeltaTime);
        }

        const bool asyncReady = preparedLoadData_ || preparedSceneInitialized_ || IsAsyncSceneReady();

        // 先読みが完了し、遷移先を有効化できる状態まで準備していなければ、主スレッド側の準備を進める。
        if (asyncReady && !preparedSceneInitialized_) {
            PrepareLoadedSceneOnMainThread();
        }

        SetLoadingProgress(CalculateLoadingProgress());

        // 最低表示時間、初期化の完了、暗転解除の終了を満たしたときだけ、ローディング画面を閉じ始める。
        if (loadingElapsed_ >= minLoadingDisplayTime_ &&
            preparedSceneInitialized_ &&
            Fade::GetInstance()->GetStatus() != Fade::Status::kFadeIn) {
            SetLoadingProgress(1.0f);
            Fade::GetInstance()->StartFadeOut(0.35f);
            transitionPhase_ = TransitionPhase::kFadingOutLoading;
        }

        return;
    }

    // 進捗表示を完了に固定し、ロード画面を暗転し終えてから準備済みSceneへ交換する。
    case TransitionPhase::kFadingOutLoading:
        // 閉じる暗転中もロード画面が存在すれば、暗転が完了するまで表示を進める。
        if (currentScene_) {
            currentScene_->Update(effectiveDeltaTime);
        }

        SetLoadingProgress(1.0f);

        // ロード画面を隠す暗転が終わっていない間は、準備済みシーンへ交換しない。
        if (Fade::GetInstance()->GetStatus() == Fade::Status::kFadeOut && !Fade::GetInstance()->IsFinished()) {
            return;
        }

        // 閉じる暗転の後は初期化済みのシーンを交換し、旧ロード画面を手放す。
        PrepareLoadedSceneOnMainThread();
        SwapToPreparedScene();
        return;

    // 遷移待ちがない場合は、直接交換の予約と通常のシーン更新を扱う。
    case TransitionPhase::kIdle:
    default:
        break;
    }

    // 生成済みの遷移先を直接受け取っている場合は、通常更新の前に交換する。
    if (nextScene_ != nullptr) {
        SwapToDirectNextScene();
    }

    // 有効なシーンがある場合は、呼び出し元が渡した通常の経過秒数で更新する。
    if (currentScene_) {
        currentScene_->Update(deltaTime);
    }
}

void SceneManager::Draw() {
    // 遷移中も、その時点で画面を担当するシーンだけ描く。
    if (currentScene_) {
        currentScene_->Draw();
    }
}

void SceneManager::DrawUi() {
    // LoadingSceneへ切り替わっている間は、そのシーンのUIを描く。
    if (currentScene_) {
        currentScene_->DrawUi();
    }
}

void SceneManager::DrawShadow() {
    // 影描画の担当も現行シーンへ一本化する。
    if (currentScene_) {
        currentScene_->DrawShadow();
    }
}

void SceneManager::SetNextScene(std::unique_ptr<BaseScene> nextScene) {
#ifdef USE_IMGUI
    // 単体再生セッションがあれば、直接交換を予約する前に編集用シーン参照を整理する。
    if (auto* session = SelectedActorSession::Current()) session->PrepareSceneChange();
#endif

    // 直接生成済みのシーンの所有権を受け取り、次の更新で交換する。
    nextScene_ = std::move(nextScene);
}

BaseScene* SceneManager::GetCurrentScene() const {
    // 所有権はSceneManagerに保持し、外部には現在の参照だけ渡す。
    return currentScene_.get();
}

void SceneManager::ChangeScene(const std::string& sceneName) {
#ifdef USE_IMGUI
    // エディターの単体再生中なら、通常遷移を予約する前に対象を切り離す。
    if (auto* session = SelectedActorSession::Current()) session->PrepareSceneChange();
#endif

    // 通常遷移は暗転後にローディング画面を経由する。
    QueueSceneChange(sceneName, true);
}

void SceneManager::ChangeSceneWithoutLoading(const std::string& sceneName, bool destinationHandlesReveal) {
#ifdef USE_IMGUI
    // エディターの単体再生中なら、直接遷移を予約する前に対象を切り離す。
    if (auto* session = SelectedActorSession::Current()) session->PrepareSceneChange();
#endif

    // 短い演出ではローディング画面を省き、遷移先の開示演出を選べる。
    QueueSceneChange(sceneName, true, true, destinationHandlesReveal);
}

void SceneManager::ChangeSceneAfterFade(const std::string& sceneName) {
#ifdef USE_IMGUI
    // エディターの単体再生中なら、暗転後の遷移を予約する前に対象を切り離す。
    if (auto* session = SelectedActorSession::Current()) session->PrepareSceneChange();
#endif

    // 呼び出し側が始めた暗転を待ってから通常の遷移へ進む。
    QueueSceneChange(sceneName, false);
}

void SceneManager::QueueSceneChange(const std::string& sceneName, bool startFadeOut, bool skipLoading, bool destinationHandlesReveal) {
    // シーン生成用のFactoryが未設定なら、遷移先を作れないため予約しない。
    if (sceneFactory_ == nullptr) {
        assert(false && "SceneFactory is not set in SceneManager.");
        return;
    }

    // 遷移の途中なら、進行中の要求を上書きしない。
    if (IsTransitionBusy()) {
        return;
    }

    // Factoryにないシーン名は生成できないため、フェードを開始する前に拒否する。
    if (!IsSceneRegistered(sceneName)) {
        assert(false && "Requested scene is not registered in SceneFactory.");
        return;
    }

    // 次の遷移に引き継ぐ指定がなければ、前のScene Assetの読み込み設定を外す。
    if (!preserveSceneAssetForNextChange_) {
        ClearActiveSceneAsset();
    }

    preserveSceneAssetForNextChange_ = false;

    // 暗転中の予約先と実際の交換時に使う名前を保持し、次回更新へ回す。
    nextSceneName_ = sceneName;
    pendingSceneNameForSwap_ = sceneName;
    directTransitionPending_ = skipLoading;
    directDestinationHandlesReveal_ = skipLoading && destinationHandlesReveal;
    transitionPhase_ = TransitionPhase::kFadingOutCurrent;

    // 暗転をこの要求が始める場合は、直接遷移か通常遷移かに合わせて時間を選ぶ。
    if (startFadeOut) {
        Fade::GetInstance()->StartFadeOut(skipLoading ? 0.2f : 1.0f);
    }

#ifdef USE_IMGUI
    // Editor専用Sceneは起動時のゲームSceneとして復元しません。
    if (sceneName != "SCENE_EDITOR") {
        SaveLastSceneName(sceneName);
    }
#endif
}

bool SceneManager::ReloadCurrentScene() {
    // 現行のシーン名が空か別の遷移が進行中なら、読み直しを予約できない。
    if (currentSceneName_.empty() || IsTransitionBusy()) {
        return false;
    }

    preserveSceneAssetForNextChange_ = HasActiveSceneAsset();
    ChangeScene(currentSceneName_);
    return true;
}

std::vector<std::string> SceneManager::GetRegisteredSceneNames() const {
    // Factory未設定時にもエディター側へ空の一覧を返せるようにする。
    return sceneFactory_ ? sceneFactory_->GetRegisteredSceneNames() : std::vector<std::string>{};
}

bool SceneManager::IsSceneRegistered(const std::string& sceneName) const {
    // 遷移前にFactoryの公開一覧で名前を照合する。
    const std::vector<std::string> sceneNames = GetRegisteredSceneNames();
    return std::find(sceneNames.begin(), sceneNames.end(), sceneName) != sceneNames.end();
}

bool SceneManager::OpenSceneAsset(
    const std::string& objectLayoutPath,
    const std::string& spriteLayoutPath,
    const std::string& runtimeSceneName) {
    // 個別のファイルパスを読込コンテキストへまとめてから共通経路へ渡す。
    SceneLoadContext context;
    context.sceneAssetId = objectLayoutPath;
    context.runtimeScene = runtimeSceneName;
    context.objectLayoutPath = objectLayoutPath;
    context.spriteLayoutPath = spriteLayoutPath;
    return OpenSceneAsset(context);
}

bool SceneManager::OpenSceneAsset(const SceneLoadContext& context) {
    // Assetと配置パスが有効で、再生中・遷移中のどちらでもなく、実行先名が登録済みの場合だけ開く。
    if (!context.IsSceneAsset() || context.objectLayoutPath.empty() ||
        isPlaying_ || IsTransitionBusy() || !IsSceneRegistered(context.runtimeScene)) {
        return false;
    }

    // Assetから得たパスを次の遷移だけに引き継げるようにする。
    activeSceneLoadContext_ = context;
    preserveSceneAssetForNextChange_ = true;
    ChangeScene(context.runtimeScene);
    return true;
}

bool SceneManager::OpenEditorSceneAsset(
    const std::string& objectLayoutPath,
    const std::string& spriteLayoutPath) {
    // Asset編集ではゲーム進行用ではなく編集専用のシーンを選ぶ。
    return OpenSceneAsset(objectLayoutPath, spriteLayoutPath, "SCENE_EDITOR");
}

void SceneManager::SetEditorSceneAssetPaths(
    const std::string& objectLayoutPath,
    const std::string& spriteLayoutPath) {
    // 表示用IDはファイル名から拡張子を除き、実際の読込パスは保持する。
    activeSceneLoadContext_.objectLayoutPath = objectLayoutPath;
    activeSceneLoadContext_.spriteLayoutPath = spriteLayoutPath;
    std::string sceneAssetId = objectLayoutPath;
    const size_t slash = sceneAssetId.find_last_of("/\\");

    // パスの区切りがあれば、表示用IDを末尾のファイル名だけにする。
    if (slash != std::string::npos) {
        sceneAssetId = sceneAssetId.substr(slash + 1);
    }

    // 末尾が.jsonの場合は、その拡張子だけを表示用IDから取り除く。
    if (sceneAssetId.size() >= 5 && sceneAssetId.substr(sceneAssetId.size() - 5) == ".json") {
        sceneAssetId.resize(sceneAssetId.size() - 5);
    }

    activeSceneLoadContext_.sceneAssetId = sceneAssetId;
}

void SceneManager::ClearActiveSceneAsset() {
    // 次の通常遷移で編集用Assetが誤って適用されないよう、指定を消す。
    activeSceneLoadContext_ = {};
}

void SceneManager::BeginLoadingTransition() {
    // GPUが旧シーンを使い終えてから解放し、LoadingSceneへ所有権を切り替える。
    const auto transitionWaitStart = std::chrono::steady_clock::now();
    engine::TextureManager::GetInstance()->WaitForAsyncUploadBatch();
    engine::DirectXCommon::GetInstance()->WaitForGpuAndReset();
    LogSlowLoadingStep("old scene GPU synchronization", currentSceneName_, transitionWaitStart);

    FinalizeCurrentScene(currentScene_);

    // 前の準備結果と転送フラグを空にし、新しいロード画面で進捗を数え直す。
    preparedScene_.reset();
    preparedLoadData_.reset();
    preloadProgress_.reset();
    preparedSceneInitialized_ = false;
    preparedTextureIndex_ = 0;
    preparedModelIndex_ = 0;
    asyncUploadBatchStarted_ = false;
    asyncUploadBatchSubmitted_ = false;
    assetCreationStarted_ = false;
    assetCreationFinished_ = false;
    preparedAssetsReady_ = false;
    sceneInitializationStarted_ = false;
    sceneInitializationFinished_ = false;

    // 旧画面を手放したら名前をロード用の予約へ移し、一時シーンを開く。
    loadingTargetSceneName_ = nextSceneName_;
    nextSceneName_.clear();
    loadingElapsed_ = 0.0f;

    currentScene_ = std::make_unique<LoadingScene>();
    ++sceneGeneration_;
#ifdef USE_IMGUI
    CameraEditor::GetInstance()->InvalidatePreviewForSceneChange();
#endif
    currentScene_->SetSceneManager(this);

    // 共有のデバッグエディターが設定済みなら、一時的なロード画面にも渡す。
    if (debugEditor_) {
        currentScene_->SetDebugEditor(debugEditor_);
    }

    PrepareRenderCommons(currentScene_.get());
    currentScene_->Initialize();
    SetLoadingProgress(0.08f);

    // 先読みを別スレッドで開始し、ロード画面の表示と並行させる。
    StartAsyncSceneCreate();
    Fade::GetInstance()->StartFadeIn(0.25f);
    transitionPhase_ = TransitionPhase::kLoading;
}

void SceneManager::StartAsyncSceneCreate() {
    // 次シーンのManifestを先に集め、CPU側の資源準備を別スレッドへ渡す。
    assert(sceneFactory_);
    const std::string targetName = loadingTargetSceneName_;
    preparedScene_ = sceneFactory_->CreateScene(targetName);
    assert(preparedScene_ && "Scene creation failed before async preload.");

    // 指定したシーンの生成に失敗したら、マニフェストを作らず戻る。
    if (!preparedScene_) {
        return;
    }

    preparedScene_->SetSceneManager(this);
    preparedScene_->SetSceneLoadContext(activeSceneLoadContext_);

    // 共有エディターが設定済みなら、先読みするシーンからも使えるよう参照を渡す。
    if (debugEditor_) {
        preparedScene_->SetDebugEditor(debugEditor_);
    }

    SceneLoadManifest manifest = preparedScene_->BuildAsyncLoadManifest();

    // Scene Asset側でBGMが上書きされている場合は、通常のマニフェストにその音源を加える。
    if (!activeSceneLoadContext_.bgmPath.empty()) {
        manifest.AddAudio(activeSceneLoadContext_.bgmPath);
    }

    preloadProgress_ = std::make_shared<ScenePreloadProgress>();
    // 進捗へ共有所有権を渡し、作業スレッドが完了するまで表示用の状態を有効に保つ。
    const std::shared_ptr<ScenePreloadProgress> progress = preloadProgress_;

    loadingFuture_ = std::async(std::launch::async, [manifest, progress, targetName]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        try {
            return ScenePreloader::Prepare(manifest, progress);
        }
        catch (const std::exception& exception) {
            auto failed = std::make_shared<ScenePreloadData>();
            failed->warnings.push_back(
                "Scene preload failed for " + targetName + ": " + exception.what());
            // 進捗の通知先がある場合は、既知の例外が起きたときも先読みを終了扱いにする。
            if (progress) {
                progress->Finish();
            }
            return failed;
        }
        catch (...) {
            auto failed = std::make_shared<ScenePreloadData>();
            failed->warnings.push_back("Scene preload failed for " + targetName + ".");
            // 進捗の通知先がある場合は、例外の種類を特定できないときも先読みを終了扱いにする。
            if (progress) {
                progress->Finish();
            }
            return failed;
        }
    });
}

bool SceneManager::IsAsyncSceneReady() const {
    // 先読み結果のFutureがまだ作られていないか消費済みなら、準備完了として扱わない。
    if (!loadingFuture_.valid()) {
        return false;
    }

    return loadingFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

void SceneManager::PrepareLoadedSceneOnMainThread() {
    // 準備済みのシーンは再初期化せず、表示と交換の待ちへ進む。
    if (preparedSceneInitialized_) {
        return;
    }

    // 結果をまだ受け取っておらず、先読み処理が存在する場合だけ、非同期結果の回収を試す。
    if (!preparedLoadData_ && loadingFuture_.valid()) {
        // 先読みスレッドが結果を用意していなければ、このフレームでは待機せず戻る。
        if (!IsAsyncSceneReady()) {
            return;
        }

        preparedLoadData_ = loadingFuture_.get();

        // 先読みから有効な結果を受け取れなかった場合は空の結果で続け、シーン初期化側の読み込みに任せる。
        if (!preparedLoadData_) {
            preparedLoadData_ = std::make_shared<ScenePreloadData>();
        }

        // 先読みの利用可否にかかわらず、得られた警告を診断に出す。
        for (const std::string& warning : preparedLoadData_->warnings) {
            OutputDebugStringA((warning + "\n").c_str());
        }

        // 遷移先の実体が残っている場合は、回収した先読みデータをそのシーンへ渡す。
        if (preparedScene_) {
            preparedScene_->SetPreparedLoadData(preparedLoadData_);
        }

        // CPUデータ回収の後に、先読みした画像をまとめて送るためのバッチを開始する。
        asyncUploadBatchStarted_ = engine::TextureManager::GetInstance()->BeginAsyncUploadBatch();
    }

    assert(preparedScene_ && "Scene creation failed during loading transition.");

    // 遷移先が生成されていなければ、初期化先がないので遷移を待機状態へ戻す。
    if (!preparedScene_) {
        transitionPhase_ = TransitionPhase::kIdle;
        return;
    }

    // 先読み分が揃ってから分割初期化へ進む。初期化中に追加されたGPU転送も完了まで待つ。
    if (!PreparePreloadedAssets() || !AdvancePreparedSceneInitialization() || !SubmitAndPollTextureUploads()) {
        return;
    }

    // Scene初期化後のGPU転送まで終わった段階で、借りていた事前読み込み結果を取り外す。
    preparedScene_->SetPreparedLoadData(nullptr);
    preparedSceneInitialized_ = true;
}

bool SceneManager::PreparePreloadedAssets() {
    // アセット生成と最初のGPU転送を確認済みなら、次の初期化ステップへ進める。
    if (preparedAssetsReady_) {
        return true;
    }

    // アセット生成をまだ依頼していないときは、作業スレッドを起動して次のフレームを待つ。
    if (!assetCreationStarted_) {
        StartPreparedAssetsAsync();
        return false;
    }

    // 作業スレッドの結果を回収済みでない場合は、停止せず完了を確認する。
    if (!assetCreationFinished_) {
        // 有効な完了結果が届いていない間は、描画スレッドを止めず再び戻る。
        if (!assetCreationFuture_.valid() ||
            assetCreationFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return false;
        }

        try {
            assetCreationFuture_.get();
        }
        // CPU側の資源生成例外を出力する。アサートが無効な構成ではその後の件数照合で未完了を扱う。
        catch (const std::exception& exception) {
            OutputDebugStringA((std::string("Asset creation failed: ") + exception.what() + "\n").c_str());
            assert(false && "Asset creation failed.");
        }
        catch (...) {
            OutputDebugStringA("Asset creation failed with an unknown exception.\n");
            assert(false && "Asset creation failed.");
        }

        // 結果を回収したことを記録する。使用可能かどうかは件数と転送も確認する。
        assetCreationFinished_ = true;
    }

    // 作成済みのテクスチャ数かモデル数が先読み一覧の件数に満たなければ、資源を使用可能にしない。
    if (preparedLoadData_ &&
        (preparedTextureIndex_.load(std::memory_order_relaxed) < preparedLoadData_->textures.size() ||
         preparedModelIndex_.load(std::memory_order_relaxed) < preparedLoadData_->modelNames.size())) {
        return false;
    }

    // 必要なGPU転送をまだ投入・完了できていなければ、次のフレームでもこの段階を調べる。
    if (!SubmitAndPollTextureUploads()) {
        return false;
    }

    // 件数とGPU転送の条件が揃ったら、シーンの初期化用にバッチのフラグを戻す。
    preparedAssetsReady_ = true;
    asyncUploadBatchStarted_ = false;
    asyncUploadBatchSubmitted_ = false;
    return true;
}

bool SceneManager::AdvancePreparedSceneInitialization() {
    // シーンの分割初期化を開始する前だけ、共有描画資源の準備と参照の接続を行う。
    if (!sceneInitializationStarted_) {
        // 共有描画資源の準備が一回のステップで終わらない場合は、シーン固有の初期化を見送る。
        if (!PrepareRenderCommons(preparedScene_.get(), true)) return false;
        preparedScene_->SetSceneManager(this);
        preparedScene_->SetSceneLoadContext(activeSceneLoadContext_);

        // 共有エディターがある場合は、分割初期化の前にその参照を渡す。
        if (debugEditor_) {
            preparedScene_->SetDebugEditor(debugEditor_);
        }

        // Scene::Initialize() は CameraManager / LightManager / GPU描画管理など、
        // メインスレッド専用の共有状態を更新します。アセットの読み込み準備だけを
        // 非同期にし、Scene本体の構築は描画と競合しないこのスレッドで確定します。
        asyncUploadBatchStarted_ = engine::TextureManager::GetInstance()->BeginAsyncUploadBatch();
        sceneInitializationStarted_ = true;
        const auto beginStart = std::chrono::steady_clock::now();
        preparedScene_->BeginLoadingInitialize();
        LogSlowLoadingStep("BeginLoadingInitialize", loadingTargetSceneName_, beginStart);
    }

    // シーンの分割初期化が続いている間は、テクスチャ転送を記録しつつ一段階だけ進める。
    if (!sceneInitializationFinished_) {
        engine::TextureManager::GetInstance()->SetAsyncUploadRecording(asyncUploadBatchStarted_);
        try {
            const auto stepStart = std::chrono::steady_clock::now();
            const float progressBefore = preparedScene_->GetLoadingInitializeProgress();
            sceneInitializationFinished_ = preparedScene_->InitializeLoadingStep();
            const double stepMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - stepStart).count();
            LogSlowLoadingOperation(loadingTargetSceneName_ +
                " / InitializeLoadingStep progress=" + std::to_string(progressBefore), stepMs);
        }
        catch (...) {
            // 途中のステップが例外を返した場合は、以降の処理へ記録状態を漏らさない。
            engine::TextureManager::GetInstance()->SetAsyncUploadRecording(false);
            throw;
        }

        engine::TextureManager::GetInstance()->SetAsyncUploadRecording(false);

        // 今回のステップで初期化が完了しなければ、次のフレームで残りを行う。
        if (!sceneInitializationFinished_) {
            return false;
        }
    }

    return true;
}

// 提出やGPU処理の完了状態を確認し、未完了なら次のフレームへ持ち越す。
bool SceneManager::SubmitAndPollTextureUploads() {
    // 提出に失敗した場合とGPU上で実行中の場合は、次のフレームでもこの段階を確認する。
    auto* textures = engine::TextureManager::GetInstance();
    // バッチを用意したがまだ提出していない場合に限り、GPUキューへの投入を試す。
    if (asyncUploadBatchStarted_ && !asyncUploadBatchSubmitted_) {
        asyncUploadBatchSubmitted_ = textures->SubmitAsyncUploadBatch();
        // バッチを提出できなければ、次のフレームでも提出を試せるよう未完了を返す。
        if (!asyncUploadBatchSubmitted_) {
            return false;
        }
    }
    return !asyncUploadBatchSubmitted_ || textures->PollAsyncUploadBatch();
}

void SceneManager::StartPreparedAssetsAsync() {
    // 先読みした一覧がないか、すでに作業を開始している場合は、新しい資源生成を起動しない。
    if (!preparedLoadData_ || assetCreationStarted_) {
        return;
    }

    const std::shared_ptr<ScenePreloadData> loadData = preparedLoadData_;
    const bool recordDeferredUploads = asyncUploadBatchStarted_;
    assetCreationFuture_ = std::async(
        std::launch::async,
        [this, loadData, recordDeferredUploads]() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
              // ワーカーから画像を準備するため、そのスレッド自身でCOMを初期化する。
              const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
              engine::TextureManager::GetInstance()->SetAsyncUploadRecording(recordDeferredUploads);
              try {
                  // マニフェストに記録された色空間を保ってGPU資源を作り、完了数だけ主画面へ公開する。
                  for (const SceneLoadManifest::TextureRequest& texture : loadData->textures) {
                      engine::TextureManager::GetInstance()->Load(
                          texture.path,
                          texture.linear ? engine::TextureManager::TextureColorSpace::kLinear
                                         : engine::TextureManager::TextureColorSpace::kAuto);
                      preparedTextureIndex_.fetch_add(1, std::memory_order_relaxed);
                  }
                  // テクスチャに続きモデルを読み込み、処理済みの件数を進捗側へ渡す。
                  for (const std::string& modelName : loadData->modelNames) {
                      ModelManager::GetInstance()->LoadModel(modelName);
                      preparedModelIndex_.fetch_add(1, std::memory_order_relaxed);
                  }
              }
              catch (...) {
                  engine::TextureManager::GetInstance()->SetAsyncUploadRecording(false);
                  // このスレッドでCOMを初期化できていた場合は、例外時も対応する終了処理を呼ぶ。
                  if (SUCCEEDED(comResult)) {
                      CoUninitialize();
                  }
                  throw;
              }
              engine::TextureManager::GetInstance()->SetAsyncUploadRecording(false);
              // このスレッドでCOMを初期化できていた場合は、資源生成の完了時に解放する。
              if (SUCCEEDED(comResult)) {
                  CoUninitialize();
              }
        });
    assetCreationStarted_ = true;
}

float SceneManager::CalculateLoadingProgress() const {
    // シーンの初期化と必要なGPU転送を完了した段階では、ロード進捗を100%とする。
    if (preparedSceneInitialized_) {
        return 1.0f;
    }

    const float workerRatio = preloadProgress_ ? preloadProgress_->GetRatio() : 0.0f;

    // CPU先読み中は8～60%に割り当て、ロード画面を開いた時点から進捗を表示する。
    if (!preparedLoadData_) {
        return 0.08f + workerRatio * 0.52f;
    }

    const std::size_t totalAssets =
        preparedLoadData_->textures.size() + preparedLoadData_->modelNames.size();
    const std::size_t completedAssets =
        preparedTextureIndex_.load(std::memory_order_relaxed) +
        preparedModelIndex_.load(std::memory_order_relaxed);
    // 進捗の60〜96%をテクスチャ・モデルの準備件数で配分する。対象が0件ならこの帯を完了扱いにする。
    const float assetRatio = totalAssets == 0
        ? 1.0f
        : static_cast<float>(completedAssets) / static_cast<float>(totalAssets);
    const float assetProgress = 0.60f + (std::min)(1.0f, assetRatio) * 0.36f;

    // アセットが揃い共有資源を準備中なら、シーン初期化前の進捗帯に共有資源の進捗を足す。
    if (preparedAssetsReady_ && !sceneInitializationStarted_ && preparedScene_ && renderCommonResources_) {
        return 0.96f + renderCommonResources_->GetPreparationProgress(preparedScene_->GetRenderPreparation()) * 0.01f;
    }

    // シーンの分割初期化中は、シーンから取得したステップ進捗に合わせて表示を進める。
    if (sceneInitializationStarted_ && !sceneInitializationFinished_) {
        const float initializeRatio = preparedScene_
            ? std::clamp(preparedScene_->GetLoadingInitializeProgress(), 0.0f, 1.0f)
            : 0.0f;
        return 0.97f + initializeRatio * 0.015f;
    }

    // 初期化を終えてもGPU転送中は99%で止め、描画可能になるまで完了表示を出さない。
    if (sceneInitializationFinished_ &&
        engine::TextureManager::GetInstance()->IsAsyncUploadBatchPending()) {
        return 0.99f;
    }

    // アセット転送を提出した直後でGPUが処理中なら、より後の進捗帯へ進めない。
    if (asyncUploadBatchSubmitted_ &&
        engine::TextureManager::GetInstance()->IsAsyncUploadBatchPending()) {
        return 0.97f;
    }

    return assetProgress;
}

void SceneManager::ReplaceCurrentScene(std::unique_ptr<BaseScene>&& incomingScene) {
    // 旧シーンが終了処理を完了するまで、遷移先の所有権を元のスロットに残す。
    FinalizeCurrentScene(currentScene_);
    currentScene_ = std::move(incomingScene);
    ++sceneGeneration_;
}

void SceneManager::SwapToPreparedScene() {
#ifdef USE_IMGUI
    // シーンを交換する前に、存在するエディターの単体再生セッションから旧シーン参照を外す。
    if (auto* session = SelectedActorSession::Current()) session->PrepareSceneChange();
#endif

    // GPU使用が終わってからLoadingSceneを解放し、準備済みシーンを有効化する。
    const auto swapWaitStart = std::chrono::steady_clock::now();
    engine::DirectXCommon::GetInstance()->WaitForGpuAndReset();
    LogSlowLoadingStep("loading scene GPU synchronization", loadingTargetSceneName_, swapWaitStart);

    ReplaceCurrentScene(std::move(preparedScene_));
#ifdef USE_IMGUI
    CameraEditor* cameraEditor = CameraEditor::GetInstance();
    cameraEditor->InvalidatePreviewForSceneChange();
    cameraEditor->SetObject3DCommon(
        currentScene_ ? currentScene_->GetObject3DCommon() : nullptr);
#endif
    // 新しいシーンの所有権を受け取った後、ロード専用の参照と進捗を廃棄する。
    preparedSceneInitialized_ = false;
    preparedLoadData_.reset();
    preloadProgress_.reset();
    preparedTextureIndex_ = 0;
    preparedModelIndex_ = 0;
    asyncUploadBatchStarted_ = false;
    asyncUploadBatchSubmitted_ = false;
    assetCreationStarted_ = false;
    assetCreationFinished_ = false;
    preparedAssetsReady_ = false;
    sceneInitializationStarted_ = false;
    sceneInitializationFinished_ = false;

    // 交換するシーン名が控えてあれば、現在の名前へ反映して予約から消す。
    if (!pendingSceneNameForSwap_.empty()) {
        currentSceneName_ = pendingSceneNameForSwap_;
        pendingSceneNameForSwap_.clear();
    }

    loadingTargetSceneName_.clear();

    // 有効化処理は、SceneManager上でも現在シーンへ切り替わった後に行います。
    // カメラ、ライト、スカイボックスなどがLoadingScene側の状態で上書きされるのを防ぎます。
    if (currentScene_) {
        currentScene_->OnActivated();
    }

    // 有効化した画面を開示し、以後の更新を通常の状態へ戻す。
    Fade::GetInstance()->StartFadeIn(1.0f);
    transitionPhase_ = TransitionPhase::kIdle;
}

void SceneManager::SwapToDirectNextScene() {
#ifdef USE_IMGUI
    // 直接交換でも、エディターの単体再生セッションがあれば先に旧シーン参照を外す。
    if (auto* session = SelectedActorSession::Current()) session->PrepareSceneChange();
#endif

    // ロード画面を挟まない遷移でも、旧シーンのGPU参照を待ってから交換する。
    engine::DirectXCommon::GetInstance()->WaitForGpuAndReset();

    ReplaceCurrentScene(std::move(nextScene_));
#ifdef USE_IMGUI
    CameraEditor::GetInstance()->InvalidatePreviewForSceneChange();
#endif
    nextScene_ = nullptr;

    // シーン名による遷移として予約されている場合は、新しい現行シーン名を反映する。
    if (!pendingSceneNameForSwap_.empty()) {
        currentSceneName_ = pendingSceneNameForSwap_;
        pendingSceneNameForSwap_.clear();
    }

    // 受け取ったシーンが有効な場合は、共有資源の準備・初期化・有効化を順に行う。
    if (currentScene_) {
        currentScene_->SetSceneManager(this);
        currentScene_->SetSceneLoadContext(activeSceneLoadContext_);

        // 共有エディターの参照がある場合は、新しいシーンの初期化前に渡す。
        if (debugEditor_) {
            currentScene_->SetDebugEditor(debugEditor_);
        }

        PrepareRenderCommons(currentScene_.get());
        currentScene_->Initialize();
#ifdef USE_IMGUI
        CameraEditor::GetInstance()->SetObject3DCommon(currentScene_->GetObject3DCommon());
#endif
        currentScene_->OnActivated();
    }
}

void SceneManager::SetLoadingProgress(float progress) {
    // 現在がLoadingSceneのときだけ進捗を送り、有効範囲へ丸める。
    if (LoadingScene* loadingScene = dynamic_cast<LoadingScene*>(currentScene_.get())) {
        loadingScene->SetProgress(std::clamp(progress, 0.0f, 1.0f));
    }
}

bool SceneManager::IsTransitionBusy() const {
    // フェード中だけでなく、準備済みシーンや非同期Futureが残る間も再遷移を防ぐ。
    return transitionPhase_ != TransitionPhase::kIdle ||
           !nextSceneName_.empty() ||
           nextScene_ != nullptr ||
           preparedScene_ != nullptr ||
           loadingFuture_.valid();
}

void SceneManager::SaveLastSceneName(const std::string& sceneName) {
    // 既存のユーザー設定項目を残したまま、次回起動に使うシーン名だけ更新する。
    json root;
    {
        std::ifstream input(kUserConfigPath);

        // 既存設定が開けた場合は、最後のシーン名以外の項目を保存時に引き継ぐために読む。
        if (input.is_open()) {
            try {
                input >> root;
            } catch (...) {
                root = json::object();
            }
        }
    }

    // 読んだ設定がJSONオブジェクトでなければ、名前付きの項目を書ける空の設定を使う。
    if (!root.is_object()) {
        root = json::object();
    }

    root["lastScene"] = sceneName;

    std::ofstream file(kUserConfigPath);

    // 設定ファイルを書き込み用に開けた場合だけ、更新したJSONを出力する。
    if (file.is_open()) {
        file << root.dump(4);
    }
}

std::string SceneManager::LoadLastSceneName() {
    // 設定ファイルがない場合やJSONが壊れている場合は空名で起動側へ任せる。
    std::ifstream file(kUserConfigPath);

    // 設定ファイルが開けない場合は空のシーン名を返し、起動側の既定値へ任せる。
    if (!file.is_open()) {
        return "";
    }

    try {
        json root;
        file >> root;

        // 保存済みのシーン名が見つかれば、次回起動時に使う候補として返す。
        if (root.contains("lastScene")) {
            return root["lastScene"];
        }
    } catch (...) {
    }

    return "";
}

// シーンが要求する範囲の共通描画資源を準備する。分割中はfalseを返して次の更新へつなぐ。
bool SceneManager::PrepareRenderCommons(BaseScene* scene, bool incremental) {
    // 準備先のシーンかFrameworkから借りる共有資源がなければ、描画準備を開始できない。
    if (!scene || !renderCommonResources_) {
        throw std::logic_error("SceneManager requires Framework render resources before scene initialization");
    }
    const auto preparation = scene->GetRenderPreparation();
    // ロード画面の更新中は1ステップずつ進め、起動や直接遷移ではその場で最後まで準備する。
    if (incremental) {
        // 共有資源を一段進めても完了しなかった場合は、次のフレームへ処理を持ち越す。
        if (!renderCommonResources_->PrepareStep(preparation)) return false;
    // 起動や直接交換など一回で準備する経路では、共有資源をここで揃える。
    } else {
        renderCommonResources_->Prepare(preparation);
    }
    // シーンが3Dの共有基盤を参照できる場合に限り、それを使ってメッシュ演出の描画器を初期化する。
    if (auto* common = scene->GetObject3DCommon()) MeshEffectManager::GetInstance()->Initialize(common);
    return true;
}
