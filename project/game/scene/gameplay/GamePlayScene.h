#include "game/ui/PlayerHealthHud.h"
#pragma once
#include <cstdint>
#include "LifeLostPresentationState.h"
#include "BaseScene.h"
#include "game/system/scene/SceneGameplayAccess.h"
#include "game/system/progress/IStageProgressScene.h"
#include "Camera.h"
#include "game/system/cinematic/CinematicPlayer.h"
#include "game/system/cinematic/CinematicSequence.h"
#include "game/scene/gameplay/GoalClearPlayerAnimator.h"
#include "MeshRenderer.h"
#include "game/system/scene/ObjectManager.h"
#include "Player.h"
#include "game/ui/CopyStyleHud.h"
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace goalPresentationRuntimeProbe { struct Access; }

// 前方宣言
namespace engine { class SceneRenderCommands; }
namespace engine { class SceneLighting; }
namespace engine { class InputManager; }
class SceneManager;
class LevelLoader;
class LockOnSystem;
class GameRule;
class BaseEnemy;
struct StageData;
struct CrownFinaleTuning;
namespace engine { class Object3DCommon; }
namespace engine { class ParticleCommon; }
namespace engine { class ParticleSystem; }
namespace engine { class SpriteCommon; }
class Text;
class Skybox;
class ISceneController;
class ControlsGuideOverlay;
class PauseMenuOverlay;
class SettingsMenuOverlay;
class SaveIndicatorOverlay;

/// <summary>
/// ゲームプレイ本編のシーン。ステージ読み込み、プレイヤー、HUD、ゴール演出を統合する。
/// </summary>
// 関連：BaseSceneの共通基盤を使い、Player・GameRule・HUDとステージ固有の演出をつなぐ。
// 主な入口：Initialize / Update / Draw / Finalize
class GamePlayScene : public BaseScene, public IStageProgressScene, public ISceneGameplayAccess {
  public:
    /// <summary>シーン固有リソースをInitializeで作るため、初期状態だけ用意する。</summary>
    GamePlayScene();
#ifdef USE_IMGUI
    // エンジン側の地点編集画面へ、ゲームの移動受付と既定地点を登録します。
    static void RegisterDebugTeleportEditor();
#endif

    /// <summary>所有リソースの解放はFinalizeに任せてシーンを破棄する。</summary>
    ~GamePlayScene() override;

    /// <summary>ステージ内容と演出を含むゲームプレイシーンを初期化する。</summary>
    void Initialize() override;

    /// <summary>ローディング画面から呼ぶ段階初期化を開始する。</summary>
    void BeginLoadingInitialize() override;

    /// <summary>初期化を一段進め、全段階が完了したか返す。</summary>
    bool InitializeLoadingStep() override;

    /// <summary>段階初期化の進捗を0から1の割合で返す。</summary>
    float GetLoadingInitializeProgress() const override;

    /// <summary>シーンが現在シーンに切り替わったら、共有ライト等の描画設定を再適用する。</summary>
    void OnActivated() override;

    /// <summary>ステージで使うScene Assetと依存リソースの先読み一覧を作る。</summary>
    SceneLoadManifest BuildAsyncLoadManifest() const override;

    /// <summary>ゲームプレイ中に所有した描画・演出リソースを終了処理する。</summary>
    void Finalize() override;

    /// <summary>経過秒を受け取り、優先度順に演出・ポーズを処理した後、通常プレイを更新する。</summary>
    void Update(float deltaTime) override;

    /// <summary>経過秒に合わせて通常HUD、残機喪失、疾走線、セーブ通知の表示を進める。</summary>
    void UpdateUi(float deltaTime);

    /// <summary>ステージの3D描画パスを実行する。</summary>
    void Draw() override;

    /// <summary>HUDと画面上の演出をワールド描画の後に描く。</summary>
    void DrawUi() override;

    /// <summary>ステージの影用描画パスを実行する。</summary>
    void DrawShadow() override;

    // --- IEditable ---
    /// <summary>Editorで表示するシーン名を返す。</summary>
    std::string GetName() override { return "Game Play Scene"; }

    /// <summary>プレイ中に調整できるシーン設定をEditorへ描く。</summary>
    void DrawImGui() override;

    /// <summary>ステージ内の橋が落ちるカメラ演出を開始する。</summary>
    void StartBridgeDropMovie();

    /// <summary>ボスとゲートの登場演出を開始します。</summary>
    /// <param name="bossObject">登場させるシーン所有のボスオブジェクト。</param>
    /// <param name="gateObject">登場演出に使うゲート。</param>
    // タイムラインの準備と再生開始に成功した場合にtrueを返す。cinematicPathは登場演出素材のパス。
    bool StartArenaBossIntro(engine::Object3D* bossObject, engine::Object3D* gateObject, const std::string& cinematicPath);

    /// <summary>ボス登場タイムラインの再生中か返す。</summary>
    bool IsArenaBossIntroActive() const { return arenaBossIntroActive_; }

    // 撃破ムービーのシーケンスに正の尺があるときは再生器から秒数を取得し、なければ12秒を使う。
    float GetCrownDefeatDuration() const { return crownDefeatVfxSequence_.duration > 0 ? crownDefeatVfxPlayer_.GetDuration() : 12.0f; }
    // 指定したシグナル名から、撃破演出の開始を基準とする発火時刻を返す。
    float GetCrownDefeatMarkerTime(const std::string& name) const;
    // 王冠継承の調整値を取得し、有効なシーケンスがあれば節目を全尺に対する割合に換算する。
    CrownFinaleTuning GetCrownFinaleTiming() const;

    /// <summary>アリーナボス撃破後の報酬表示を開始する。実行できない状態では何もしない。</summary>
    /// <param name="rewardObject">登場させる報酬Object。所有権は移さない。</param>
    void StartArenaBossDefeatReward(engine::Object3D* rewardObject);

    /// <summary>最終ボスの王冠演出を開始します。</summary>
    /// <returns>演出を開始できた場合は true。</returns>
    bool StartCrownFinale(class EnemyFalseKingSlime* boss);
    // ボスの更新中は演出を開始せず、Objectの走査後に開始する。
    void RequestCrownFinale(class EnemyFalseKingSlime* boss);

    /// <summary>編集用の複製ステージで王冠ムービーのプレビューを開始する。</summary>
    /// <param name="defeat">撃破ムービーを試す場合はtrue。</param>
    /// <returns>プレビューの開始に成功した場合はtrue。</returns>
    bool BeginCrownMoviePreview(bool defeat = false);

    /// <summary>最終ボスの撃破ムービーを開始する。</summary>
    /// <returns>ボスとプレイヤーが有効で、演出を開始できた場合はtrue。</returns>
    bool StartCrownDefeat(class EnemyFalseKingSlime* boss);

    /// <summary>編集中の王冠ムービープレビューを終了する。</summary>
    void EndCrownMoviePreview();

    /// <summary>王冠ムービープレビューを先頭へ巻き戻す。</summary>
    void RewindCrownMoviePreview();

    /// <summary>王冠ムービープレビューを指定秒数だけ進める。</summary>
    void StepCrownMoviePreview(float deltaTime);

    /// <summary>王冠ムービープレビューが進行中か返す。</summary>
    bool IsCrownMoviePreviewActive() const { return crownMoviePreview_.active; }

    // 第二形態で結晶を召喚し、結晶から障壁へ力が流れる演出を開始する。centerはワールド位置。
    bool StartSealRevealMovie(class EnemyFalseKingSlime* boss, const engine::Vector3& center);
    // 封印演出を再生中で、その種類が結晶召喚の場合だけtrue。
    bool IsSealRevealMovieActive() const { return sealBarrageMovie_.active && sealBarrageMovie_.reveal; }
    // 召喚ムービー内の経過秒。演出外は詠唱の見た目を開始しないようゼロを返します。
    float GetSealRevealMovieTime() const { return IsSealRevealMovieActive() ? sealBarrageMovie_.time : 0.0f; }
    // 結晶破壊の猶予切れで槍雨の演出を開始する。strikeは攻撃のワールド基準位置。
    bool StartSealBarrageMovie(class EnemyFalseKingSlime* boss, const engine::Vector3& strike);

    // BaseSceneの所有コンテナ操作。所有権の契約は基底クラスと同じです。
    /// <summary>このシーンが所有する3Dオブジェクト一覧を返す。</summary>
    const std::vector<std::unique_ptr<engine::Object3D>>& GetObjects() const override { return objectManager_->GetObjects(); }

    /// <summary>Object3dを即時登録して非所有参照を返す。</summary>
    engine::Object3D* AddObjectImmediate(std::unique_ptr<engine::Object3D> object) override { return objectManager_->AddObjectImmediate(std::move(object)); }

    /// <summary>Object3dを所有コンテナから取り出して呼び出し元へ渡す。</summary>
    std::unique_ptr<engine::Object3D> TakeObjectImmediate(engine::Object3D* object) override { return objectManager_->TakeObjectImmediate(object); }

    /// <summary>Object3dの所有権をシーンの管理器へ移す。</summary>
    void AddObject(std::unique_ptr<engine::Object3D> object) override { objectManager_->AddObject(std::move(object)); }

    /// <summary>Object3dの削除を管理器へ予約する。</summary>
    void RequestRemoveObject(engine::Object3D* object) override { objectManager_->RequestRemove(object); }

    /// <summary>Replayの記録情報から動的Object3dを再生成する。</summary>
    std::unique_ptr<engine::Object3D> CreateReplayObject(const json& descriptor) override;

    /// <summary>ReplayでObject3dを再生成した後、シーン内部の参照を結び直す。</summary>
    void OnReplayObjectsRecreated() override;

    /// <summary>このシーンが所有するSprite一覧を返す。</summary>
    const std::vector<std::unique_ptr<engine::Sprite>>& GetSprites() const override { return sprites_; }

    /// <summary>Replayに記録するSpriteを所有UIから収集する。</summary>
    void CollectReplaySprites(std::vector<engine::Sprite*>& sprites) override;

    /// <summary>Replayに必要なゲーム進行状態をJSONへ保存する。</summary>
    void CaptureReplaySceneState(json& state) const override;

    /// <summary>ReplayのJSONからゲーム進行状態を復元する。</summary>
    void RestoreReplaySceneState(const json& state) override;

    /// <summary>このシーンのスカイボックスへの非所有参照を返す。</summary>
    Skybox* GetSkybox() override { return skybox_.get(); }

    /// <summary>このシーンの粒子システムへの非所有参照を返す。</summary>
    engine::ParticleSystem* GetParticleSystem() override { return particleSystem_.get(); }

    /// <summary>現在のプレイヤーへの非所有参照を返す。</summary>
    Player* GetPlayer() const override { return player_; }

    /// <summary>シーンが参照するプレイヤーを設定する。所有権は移さない。</summary>
    void SetPlayer(Player* player) override { player_ = player; }

    /// <summary>ステージの進行規則を管理するGameRuleへの非所有参照を返す。</summary>
    GameRule* GetGameRule() override { return gameRule_.get(); }

    /// <summary>ゴール状態を切り替える。解除時は保存した姿勢・材質・操作・カメラを復元する。</summary>
    void SetIsGoal(bool isGoal);

    /// <summary>指定王冠を使ってステージクリア演出を開始する。</summary>
    void StartGoalPresentation(engine::Object3D* crownObject);

    /// <summary>このシーンがゴール処理中か返す。</summary>
    bool IsGoal() const { return isGoal_; }

    /// <summary>プレイヤーとゴールの接触から、開始条件を満たす場合にゴール処理を始める。</summary>
    bool TryStartGoalFromContact(engine::Object3D* actor, engine::Object3D* goal) override;

    /// <summary>接触したスターコインの番号とワールド位置を収集処理に渡し、ログに記録する。</summary>
    void CollectStarCoinFromContact(std::int32_t coinIndex, const engine::Vector3& worldPosition) override;
    // 今回のリザルト用に、コインの取得が成立した接触だけを数える。
    void RecordStageCoinPickup();

    /// <summary>番号で指定したスターコインの取得状態を更新する。</summary>
    void CollectStarCoin(std::int32_t coinIndex);

    /// <summary>取得状態を更新し、ワールド位置からHUDへ移動する演出を始める。</summary>
    void CollectStarCoin(std::int32_t coinIndex, const engine::Vector3& worldPosition);

    /// <summary>減少前後の残機数を使って、ライフ減少演出を開始します。</summary>
    /// <param name="beforeLives">減少前の残機数。</param>
    /// <param name="afterLives">減少後の残機数。</param>
    void StartLifeLostPresentation(std::int32_t beforeLives, std::int32_t afterLives);

    /// <summary>結晶召喚または猶予切れの槍雨のムービー中ならtrue。</summary>
    bool IsSealBarrageMovieActive() const { return sealBarrageMovie_.active; }

    /// <summary>残機減少演出が終了したか返す。</summary>
    bool IsLifeLostPresentationFinished() const { return lifeLostState_.IsFinished(); }

    /// <summary>残機減少演出の画面オーバーレイを隠す。</summary>
    void HideLifeLostPresentationOverlay();

  private:
#ifdef USE_IMGUI
    friend struct goalPresentationRuntimeProbe::Access;
#endif
    /// <summary>シーンが所有する可変Sprite配列を基底の編集処理へ渡す。</summary>
    std::vector<std::unique_ptr<engine::Sprite>>* GetMutableSprites() override { return &sprites_; }

    enum class MovieState { kNone, kBridgeDrop };

    // 橋落下ムービーの種類・経過秒と、復元するカメラ構図。
    MovieState movieState_ = MovieState::kNone;
    float movieTimer_ = 0.0f;
    engine::Vector3 movieStartCameraEye_;
    engine::Vector3 movieStartCameraTarget_;
    bool hasBridgeDropped_ = false;

  private:
    /// <summary>深度を参照する特殊材質とGPU粒子を描き、背景色をコピーした場合はtrueを返す。</summary>
    bool DrawDepthReadingEffects(engine::SceneRenderCommands& commands,
                                 const std::vector<std::unique_ptr<engine::Object3D>>& objects,
                                 const engine::Camera& camera, bool isFirstPerson);

    /// <summary>深度参照区間を閉じた後に付属エフェクトを描画し、先にコピーした背景色を再利用する。</summary>
    void DrawAttachedMeshEffects(engine::SceneRenderCommands& commands,
                                 const std::vector<std::unique_ptr<engine::Object3D>>& objects, bool sceneColorCopied,
                                 const engine::SceneLighting& lighting);

    // Game/Frameworkが所有する描画・入力基盤を借用する。
    engine::InputManager* inputManager_ = nullptr;

    // ステージ配置、ロックオン、Object一覧の管理器をシーンが所有する。
    std::unique_ptr<LevelLoader> levelLoader_;
    std::unique_ptr<LockOnSystem> lockOnSystem_;
    std::unique_ptr<ObjectManager> objectManager_;

    // シーン内で使用するSpriteを所有する一覧。各Overlay固有のSpriteは含めない。
    std::vector<std::unique_ptr<engine::Sprite>> sprites_;

    // CPU粒子の発生と更新を担当する実行インスタンス。
    std::unique_ptr<engine::ParticleSystem> particleSystem_;

    // デバッグ文字表示と、ゲームルール・シーン制御の実行単位。
    std::unique_ptr<Text> debugText_;
    std::unique_ptr<GameRule> gameRule_;
    std::unique_ptr<ISceneController> sceneController_;

    // ObjectManager側に属するプレイヤーへの非所有参照。
    Player* player_ = nullptr;

    // 最終演出で変更したオブジェクトの状態を、終了時の復元用に保持します。
    struct CrownFinaleObjectPose {
        engine::Object3D* object = nullptr;
        engine::Vector3 position{};
        engine::Vector3 rotation{};
        uint32_t attribute = 0;
        uint32_t mask = 0;
        bool visible = true;
    };

    // 最終演出の進行状態と、演出中だけ使用するオブジェクトをまとめます。
    struct CrownFinaleState {
        // 能力付与やモデル交換など、時間の通過だけでは判断できない状態を保持する。
        bool active = false;
        bool prepared = false;
        bool granted = false;
        bool hadControl = true;
        bool bodyFormed = false;

        // 演出中に差し替える前のプレイヤー表示設定。
        std::string playerModel;
        engine::Vector4 playerColor{};
        std::int32_t playerMaterial = 25;
        bool landed = false;
        float playerYaw = 0.0f;

        // 演出経過秒は再生器の時計を借り、durationは全尺の秒数、fovはラジアン。
        engine::TimerView time;
        float duration = 9.0f;
        float fov = 0.45f;
        float bars = 0.0f;

        // プレイヤー・ボス・王冠片の基準姿勢。
        engine::Vector3 start{};
        engine::Vector3 playerScale{};

        engine::Vector3 bossStart{};
        engine::Vector3 offset{};

        engine::Vector3 bossScale{};
        engine::Vector3 bossRotation{};

        engine::Vector3 fragmentStart{};
        engine::Vector3 lastSealPosition{};
        engine::Vector3 fragmentGround{};

        std::string bossModel;

        // ボスとシーンへ追加した演出オブジェクトを借用し、終了時の復元・削除に使う。
        class EnemyFalseKingSlime* boss = nullptr;
        std::vector<CrownFinaleObjectPose> originalObjects;
        std::vector<CrownFinaleObjectPose> fractureChunks;
        std::vector<engine::Object3D*> generatedObjects;

        // 共通再生器が所有する王冠片への借用参照。操作用フックと接続する。
        engine::Object3D* fragment = nullptr;
    } crownFinale_;
    // 同じ更新内でのみ保持する非所有参照。開始前にシーン内の所有を確認する。
    class EnemyFalseKingSlime* pendingCrownFinaleBoss_ = nullptr;
    // 王冠継承のDirector素材を所有し、装飾・VFX・カメラを同じ時計で再生する。
    CinematicPlayer crownFinalePlayer_;

    // ゲーム側は撃破の確定とPlayerの操作状態だけを保持する。
    struct CrownDefeatState {
        bool active = false, completed = false, hadControl = true;
        class EnemyFalseKingSlime* boss = nullptr;
        float playerYaw = 0.0f, victoryYaw = 0.0f, victoryTime = 0.0f;
    } crownDefeat_;
    // 撃破演出全体の素材と再生状態。一時Objectはエンジンの再生器が所有する。
    CinematicSequence crownDefeatVfxSequence_;
    CinematicPlayer crownDefeatVfxPlayer_;

    /// <summary>最終ボス撃破ムービーの時刻と配置を進める。</summary>
    void UpdateCrownDefeat(float deltaTime);

    /// <summary>撃破ムービーの終了状態を確定し、通常シーンへ引き渡す。</summary>
    void FinishCrownDefeat();

    /// <summary>撃破ムービーが一時変更した状態を初期状態に戻す。</summary>
    void ResetCrownDefeat();

    /// <summary>このフレームの共有ライトを借り、撃破ムービーのワールド演出を描画する。</summary>
    void DrawCrownDefeatWorld(const engine::SceneLighting& lighting);

    // 王冠演出プレビューの一時カメラを所有し、元の Player・カメラ・表示対象は復元用に借用する。
    struct CrownMoviePreviewState {
        bool defeat = false;
        bool active = false;
        Player* originalPlayer = nullptr;
        std::unique_ptr<engine::Camera> camera;
        engine::Camera* previousCameraOverride = nullptr;
        std::vector<engine::Object3D*> hiddenObjects;
        std::vector<engine::Object3D*> objects;
        engine::Vector3 eye{}, target{}, rotation{};
        engine::Camera::FollowMode followMode = engine::Camera::FollowMode::kAimable;
        engine::Object3D* follow = nullptr;
        float fov = 0.45f;
    } crownMoviePreview_;

    // 結晶召喚・猶予切れ演出の時計と、終了時に戻す操作権・カメラ構図・追従対象。
    struct SealBarrageMovieState {
        bool active = false;
        bool reveal = false; // 結晶召喚カット。false は時間切れの槍雨。
        std::unique_ptr<engine::Object3D> shieldDemoShot; // 障壁で弾かれる描画専用の弾。
        bool hadControl = true;

        float time = 0.0f;

        float fov = 0.7f;
        float bars = 0.0f;

        engine::Vector3 eye{};
        engine::Vector3 target{};
        engine::Vector3 rotation{};

        engine::Vector3 boss{};
        engine::Vector3 strike{};

        engine::Object3D* follow = nullptr;
        engine::Object3D* lockOn = nullptr;
        engine::Camera::FollowMode followMode = engine::Camera::FollowMode::kAimable;
    } sealBarrageMovie_;

    /// <summary>演出の種類に応じて、結晶召喚カットか槍雨の経過を秒単位で進める。</summary>
    void UpdateSealBarrageMovie(float deltaTime);

    /// <summary>結晶召喚または槍雨の終了時に操作権・カメラ・一時生成物を整理する。</summary>
    void FinishSealBarrageMovie();

    // 王冠最終演出の各段階で差し替える画面メッセージ。
    std::array<std::unique_ptr<engine::Sprite>, 6> crownFinaleMessages_;

    /// <summary>王冠継承演出を進める。成功・中断時はFinishCrownFinale()へ処理を渡す。</summary>
    // seekSeconds指定時はイベントを発火せず指定秒を再現する。
    void UpdateCrownFinale(float deltaTime, std::optional<float> seekSeconds = std::nullopt);
    // 再生器が決めた秒数に合わせて能力・材質・床の状態を準備し、演出を続けられる場合はtrue。
    bool PrepareCrownFinaleFrame(float previousTime, float timeSeconds, float deltaTime);

    /// <summary>最終演出の成否を確定し、次の進行状態へ移す。</summary>
    void FinishCrownFinale(bool completed);

    /// <summary>Object走査後に要求を取り出し、まだシーンに存在する生存ボスに対してだけ継承演出を始める。</summary>
    void StartPendingCrownFinale();
    // 未処理の要求を消し、準備済みの演出があれば生成物とワールド状態を片付ける。
    void ResetCrownFinaleWorld();

    /// <summary>このフレームの共有ライトを借り、最終演出の3Dオブジェクトを描画する。</summary>
    void DrawCrownFinaleWorld(const engine::SceneLighting& lighting);

    /// <summary>最終演出中の画面UIを描画する。</summary>
    void DrawCrownFinaleUi();

#ifdef USE_IMGUI

    struct DebugTeleportRequest {
        engine::Vector3 position;
        float yawRadians;
        std::string label;
    };

    // 能力呼び出しに必要な実敵インスタンスを、デバッグ変身中だけ所有します。
    std::unique_ptr<BaseEnemy> debugPlayerMorphSource_;

    // ImGui描画中にゲーム状態を直接変更せず、次の更新開始時に安全に処理します。
    std::optional<DebugTeleportRequest> pendingDebugTeleport_;
#endif

    // BGMの二重再生防止と、粒子演出時に鳴らすSEの登録ハンドル。
    bool isBGMPlaying_ = false;
    uint32_t particleSEHandle_ = 0;

    // GPU粒子が参照する画像と、HUDに表示するロックオンマーカー。
    uint32_t gpuParticleTexHandle_ = 0;
    // 疾走線は中央から外へ伸ばす二枚を交互に表示し、継ぎ目を隠す。
    std::array<std::unique_ptr<engine::Sprite>, 2> dashSpeedLineSprites_;
    float dashSpeedLinePhase_ = 0.0f; // 0〜1の流れ位置。
    float dashSpeedLineAlpha_ = 0.0f; // 2枚への配分前の不透明度。
    float dashSpeedLineMaxAlpha_ = 0.72f; // 加速線が最も濃いときの配分前不透明度。
    float dashSpeedLineScale_ = 1.0f; // 画面サイズに対する倍率。
    // 加速パネルによる水平速度から疾走線の強さを更新する。deltaTimeは秒。
    void UpdateDashSpeedLines(float deltaTime);

    std::unique_ptr<engine::Sprite> lockOnSprite_;
    bool isDrawLockOn_ = false;

    // ステージの空とその画像ハンドル。
    std::unique_ptr<Skybox> skybox_;
    uint32_t skyboxTextureHandle_ = 0;

    // デバッグ用アニメーションプレビューの表示Object。
    std::unique_ptr<engine::Object3D> animatedCube_;

    /// <summary>ゴール時の王冠・カメラ・結果UIの演出パラメータをまとめる。</summary>
    // 演出の時刻は秒、視点と王冠の距離はワールド単位、FOVはラジアンで保持する。
    struct GoalPresentationTuning {
        float crownFocusEndTime = 0.72f;
        float crownMoveStartTime = 0.98f;

        float crownDropHeight = 2.00f;
        float crownSeatDepth = 0.10f;
        float crownWearWidthRatio = 0.78f;
        float cameraSafeMargin = 0.10f;




        float resultCameraDistance = 8.60f;
        float resultCameraHeight = 1.85f;
        float resultTargetSide = 0.55f;

        float resultFov = 0.54f;

        // リザルト配置を解像度に合わせるため、中心位置は画面幅と高さに対する割合で指定する。
        float resultUiCenterX = 0.735f;
        float resultUiCenterY = 0.20f; // 見出しのY座標を画面高に対する割合で指定。
        float resultPanelTop = 0.40f; // 獲得枠の上端を画面高に対する割合で指定。
        float resultUiScale = 1.00f;

        float resultBackdropAlpha = 1.00f;
        float resultGlowAlpha = 0.72f;

        // 見出しの文字ごとの時間差と落下時間は秒、移動量は設計座標のピクセル。
        float resultLetterStagger = 0.085f;
        float resultLetterFallDuration = 0.34f;
        float resultLetterDropHeight = 260.0f;
        float resultLetterBounceHeight = 48.0f;
        float resultLetterBounceDamping = 2.90f;
        float resultLetterBounceFrequency = 9.80f;
        float resultCoinCountDuration = 0.65f; // 獲得コインを数え上げる秒数。
    };

    // ゴール成立と保存の重複防止、入場前のクリア状態を保持する。
    bool isGoal_ = false;
    bool goalSavePerformed_ = false;
    std::int32_t stageCoinPickups_ = 0; // このステージに入ってから取得した通常コインの枚数。
    double stageElapsedSeconds_ = 0.0; // 操作可能なフレームで加算した、この挑戦の秒数。
    std::int32_t goalResultClearSeconds_ = 0; // ゴール成立時に表示用として確定した秒数。
    std::int32_t goalResultCoins_ = 0; // クリア時に確定した今回の取得枚数。
    enum class GoalResultStar { kEmpty, kOwned, kNew };
    std::array<GoalResultStar, 3> goalResultStars_{};
    bool goalResultRevealed_ = false; // 決定入力で結果の表示を最後まで進めたか。
    bool goalWasStageCleared_ = false;

    // 今回のステージ滞在中に取得したスターコインをスロット別に記録する。
    bool sessionStarCoins_[3] = {false, false, false};

    // --- ステージ開始時のゲート登場演出 ---
    // pending/active/completedを分け、初期化待ちと再実行防止を管理する。
    bool stageEntryPresentationActive_ = false;
    bool stageEntryPresentationPending_ = true;
    bool stageEntryPresentationCompleted_ = false;
    bool stageEntryRuntimeWasPlaying_ = false;
    bool stageEntryPlayerEmergenceStarted_ = false;
    bool stageEntryHadPlayerControl_ = true;
    bool stageEntryCinemaBarOverrideActive_ = false;

    // 演出と再試行の経過時間は秒。黒帯の基準値を終了時の復元に使う。
    float stageEntryPresentationTimer_ = 0.0f;
    float stageEntryPresentationRetryTimer_ = 0.0f;
    float stageEntryCinemaBarBaseHeight_ = 0.0f;

    // 入場ゲートはシーン内Objectを借用し、位置とカメラ構図だけ保存する。
    engine::Object3D* stageEntryGate_ = nullptr;
    engine::Vector3 stageEntryDirection_ = {1.0f, 0.0f, 0.0f};
    engine::Vector3 stageEntryCameraStartEye_ = engine::Vector3::kZero;
    engine::Vector3 stageEntryCameraStartTarget_ = engine::Vector3::kZero;
    engine::Vector3 stageEntryCameraFocusEye_ = engine::Vector3::kZero;
    engine::Vector3 stageEntryCameraFocusTarget_ = engine::Vector3::kZero;
    engine::Vector3 stageEntryCameraRestoreEye_ = engine::Vector3::kZero;
    engine::Vector3 stageEntryCameraRestoreTarget_ = engine::Vector3::kZero;
    float stageEntryCameraBaseFov_ = 0.45f;
    float stageEntryCameraFocusFov_ = 0.45f;

    // --- ステージ3 ボス登場演出 ---
    // 開始前の操作権とカメラを保存し、終了時に通常視点へ戻す。
    bool arenaBossIntroActive_ = false;
    bool arenaBossIntroHadPlayerControl_ = true;
    bool arenaBossIntroCinemaBarOverrideActive_ = false;
    bool arenaBossIntroGateTimelineControlled_ = false;

    float arenaBossIntroTimer_ = 0.0f;
    float arenaBossIntroDuration_ = 6.45f;
    float arenaBossIntroReturnStart_ = 5.45f;
    float arenaBossIntroBarOpenEnd_ = 0.24f;
    float arenaBossIntroMessageInTime_ = 2.7f;
    float arenaBossIntroMessageOutTime_ = 5.1f;

    // GhostDirectorの登場カメラ演出。ゲートとボスのゲーム状態は各Objectが管理する。
    CinematicSequence arenaBossIntroSequence_;
    CinematicPlayer arenaBossIntroPlayer_;

    float arenaBossIntroCinemaBarBaseHeight_ = 0.0f;

    engine::Object3D* arenaBossIntroBoss_ = nullptr;
    engine::Object3D* arenaBossIntroGate_ = nullptr;

    engine::Vector3 arenaBossIntroRestoreEye_ = engine::Vector3::kZero;
    engine::Vector3 arenaBossIntroRestoreTarget_ = engine::Vector3::kZero;

    // --- ステージ3 ボス撃破・王冠落下演出 ---
    // 報酬Objectは借用し、演出中の視点と終了時の復元位置を保持する。
    bool arenaBossRewardActive_ = false;
    bool arenaBossRewardHadPlayerControl_ = true;
    bool arenaBossRewardCinemaBarOverrideActive_ = false;
    bool arenaBossRewardReturnPrepared_ = false;
    float arenaBossRewardReturnStart_ = 1.82f;
    float arenaBossRewardDuration_ = 2.78f;
    float arenaBossRewardCinemaBarBaseHeight_ = 0.0f;
    CinematicSequence arenaBossRewardSequence_;
    CinematicPlayer arenaBossRewardPlayer_;
    engine::Object3D* arenaBossRewardObject_ = nullptr;
    engine::Vector3 arenaBossRewardGameplayEye_ = engine::Vector3::kZero;
    engine::Vector3 arenaBossRewardGameplayTarget_ = engine::Vector3::kZero;

    enum class GoalPresentationState { kInactive, kCelebrating, kReadyToReturn, kReturning };

    // ゴール後の入力受付と選択画面への復帰を切り替える段階。
    GoalPresentationState goalPresentationState_ = GoalPresentationState::kInactive;

    // 再生中は演出時刻へ同期し、終了後もリザルトの表示を進める秒時計。
    engine::Timer goalPresentationTimer_;

    // 星・画面演出の発生間隔と、待機中の王冠演出を管理する。
    float goalStarEmitTimer_ = 0.0f;
    float goalBurstEmitTimer_ = 0.0f;
    float goalCrownIdleTime_ = 0.0f;
    float goalCrownSparkleTimer_ = 0.0f;
    size_t goalCrownSparklePatternIndex_ = 0;

    // 演出開始前のプレイヤー・王冠の姿勢を保持し、終了時に復元する。
    engine::Vector3 goalCrownPosition_ = engine::Vector3::kZero;
    engine::Vector3 goalPlayerBasePosition_ = engine::Vector3::kZero;
    engine::Vector3 goalPlayerBaseScale_ = {1.0f, 1.0f, 1.0f};
    engine::Vector3 goalPlayerBaseRotation_ = engine::Vector3::kZero;
    engine::Vector3 goalPlayerPosePosition_ = engine::Vector3::kZero;
    engine::Vector3 goalMoveForward_ = {0.0f, 0.0f, 1.0f};
    engine::Vector3 goalMoveRight_ = {1.0f, 0.0f, 0.0f};
    engine::Vector3 goalCrownBasePosition_ = engine::Vector3::kZero;
    // 配置時の姿と装着後の寸法を分離し、親付きの王冠もワールド空間で運ぶ。
    engine::Vector3 goalCrownStartWorldPosition_{};
    engine::Vector3 goalCrownWearScale_ = {1.0f, 1.0f, 1.0f};
    std::string goalCrownIdleEffect1_, goalCrownIdleEffect2_;
    bool goalCrownBaseEnvMap_ = false;
    float goalCrownBaseEnvIntensity_ = 1.0f, goalCrownBaseRoughness_ = 0.3f;
    engine::Vector3 goalCrownBaseScale_ = {1.0f, 1.0f, 1.0f};
    engine::Vector3 goalCrownBaseRotation_ = engine::Vector3::kZero;


    // 通常・王冠注視・着地・ジャンプ・結果表示の各カメラ構図。
    engine::Vector3 goalCameraGameplayEye_ = engine::Vector3::kZero;
    engine::Vector3 goalCameraGameplayTarget_ = engine::Vector3::kZero;
    float goalCameraGameplayFov_ = 0.45f;
    static constexpr const char* kFalseKingRewardName = "Stage3_HighCrown_VictoryCrown";
    // 通常ゴールで受け取った報酬にだけ、専用アイコンを表示する。
    bool HasFalseKingGoalReward() const;
    float goalDefeatResultBars_ = 0.0f; // ステージ3で勝利カットから引き継いだ黒帯の高さ。

    // 復元に必要なスナップショットの有効性と、一度だけ鳴らす演出の実行状態。
    bool goalSavedPlayerControlActive_ = true;
    bool goalPlayerSnapshotValid_ = false;
    bool goalCrownSnapshotValid_ = false;
    bool goalCameraSnapshotValid_ = false;
    bool goalReturnFadeStarted_ = false;
    bool goalLandingCuePlayed_ = false;
    bool goalResultCuePlayed_ = false;
    bool goalEditorPreviewMode_ = false;

    // 既存の王冠Objectとカメラは借用し、演出専用カメラとUIだけ所有する。
    engine::Object3D* goalCrownObject_ = nullptr;
    engine::Camera* goalCameraRestoreOverride_ = nullptr;
    engine::Camera* goalLockedPrimaryCamera_ = nullptr;
    engine::Camera* goalLockedSecondaryCamera_ = nullptr;

    // ゴール専用のプレイヤー姿勢、タイムライン、再生状態。
    GoalClearPlayerAnimator goalClearPlayerAnimator_;
    CinematicSequence goalCinematicSequence_;
    CinematicPlayer goalCinematicPlayer_;
    bool goalCinematicTimelineLoaded_ = false;
    GoalPresentationTuning goalPresentationTuning_;
    std::unique_ptr<engine::Camera> goalPresentationCamera_;
    std::unique_ptr<engine::Sprite> goalOverlayLandingRipple_;
    std::unique_ptr<engine::Sprite> goalOverlayReturnPrompt_; // 帰還先のラベルと帯を焼き込んだ画像。
    std::unique_ptr<engine::Sprite> goalOverlayReturnGlyph_; // 現在の入力デバイスと割当に対応する決定キー。
    std::unique_ptr<engine::Sprite> goalOverlayResultPanel_;
    std::unique_ptr<engine::Sprite> goalOverlayFalseKingCrown_;
    std::unique_ptr<engine::Sprite> goalOverlayClearTimeLabel_;
    std::array<std::unique_ptr<engine::Sprite>, 8> goalOverlayClearTimeGlyphs_;
    std::array<std::unique_ptr<engine::Sprite>, 3> goalOverlayCoinTrails_;
    std::unique_ptr<engine::Sprite> goalOverlayCoinMultiplier_;
    std::array<std::unique_ptr<engine::Sprite>, 3> goalOverlayNewStars_;
    std::array<std::unique_ptr<engine::Sprite>, 3> goalOverlayRewardStars_;
    std::unique_ptr<engine::Sprite> goalOverlayRewardCoin_;
    std::array<std::unique_ptr<engine::Sprite>, 10> goalOverlayRewardDigits_;
    std::uint32_t goalRewardFilledStarTexture_ = 0;
    std::uint32_t goalRewardEmptyStarTexture_ = 0;
    std::array<std::unique_ptr<engine::Sprite>, 7> goalOverlayStageClearLetters_;
    std::array<std::unique_ptr<engine::Sprite>, 7> goalOverlaySparkles_;

    // HUD の基準値。演出で拡縮しても戻せるよう保存する。
    struct HudSpriteState {
        engine::Sprite* sprite = nullptr;
        engine::Vector2 basePosition = engine::Vector2::kZero;
        engine::Vector2 baseSize = engine::Vector2::kZero;
        engine::Vector4 baseColor = engine::colors::kWhite;
    };

    // --- プレイ中 HUD ---
    // HudSpriteStateは配置JSONのSpriteを借用し、演出前の基準配置も保持する。
    HudSpriteState hudCopyStyle_;
    CopyStyleHud copyStyleHud_;
    PlayerHealthHud healthHud_;


    HudSpriteState hudLifeIcon_;
    HudSpriteState hudLifeXIcon_;
    std::array<HudSpriteState, 2> hudLifeDigits_;

    HudSpriteState hudCoinIcon_;
    HudSpriteState hudCoinXIcon_;
    std::array<HudSpriteState, 2> hudCoinDigits_;

    std::array<HudSpriteState, 3> hudStageStarSlots_;

    // 脈動の残り秒数と、HUDの枠が取得済み画像へ切り替わったかを別に保持する。
    std::array<float, 3> hudStageStarPulseTimers_ = {0.0f, 0.0f, 0.0f};
    std::array<bool, 3> hudStageStarVisualCollected_ = {false, false, false};

    // スターコイン取得地点の画面座標から、HUDの枠または火花の終点へ飛ぶ粒子。
    struct StageStarUiFlyParticle {
        std::unique_ptr<engine::Sprite> sprite;
        engine::Vector2 start = engine::Vector2::kZero;
        engine::Vector2 control = engine::Vector2::kZero;
        engine::Vector2 end = engine::Vector2::kZero;
        // 経過と寿命は秒。回転速度はラジアン毎秒。
        float timer = 0.0f;
        float duration = 0.6f;
        float baseSize = 20.0f;
        float rotationSpeed = 0.0f;
        // 0～2は対応する取得枠。-1は枠を埋めない火花。
        std::int32_t starIndex = -1;
        bool fillsSlot = false;
    };

    std::vector<StageStarUiFlyParticle> hudStageStarFlyParticles_;

    // 残機・コインの増減を前回値から検出し、表示を強調するための残り秒数を保持する。
    float hudLifeGainPulseTimer_ = 0.0f;
    float hudCoinPulseTimer_ = 0.0f;

    std::int32_t hudPreviousLives_ = 0;
    std::int32_t hudPreviousCoins_ = 0;


    // --- ボス／中ボス共通 HUD ---
    enum class PrismBossHudPhase {
        kHidden,
        kIntroducing,
        kActive,
        kDismissing,
    };

    enum class BossHudTheme {
        kPrism,
        kMagma,
        kFalseKing,
    };

    // 今フレームの表示値。敵への参照は保持しない。
    struct BossHudDisplay {
        BossHudTheme theme = BossHudTheme::kPrism;
        bool visible = false;
        float maximumHp = 1.0f;
        float currentHp = 0.0f;
        float appearanceProgress = 1.0f;
        std::int32_t falseKingPhase = 1;
        float falseKingPhaseTransition = 1.0f;
    };

    // ボスHUDの各Spriteは配置Spriteを借用し、テーマごとに画像と色を変える。
    HudSpriteState prismBossHudName_;
    HudSpriteState prismBossHudGlow_;
    HudSpriteState prismBossHudBack_;
    HudSpriteState prismBossHudTrack_;

    HudSpriteState prismBossHudDamageFill_;
    HudSpriteState prismBossHudFill_;
    HudSpriteState prismBossHudHighlight_;

    HudSpriteState prismBossHudFrameTop_;
    HudSpriteState prismBossHudFrameBottom_;

    HudSpriteState prismBossHudShardLeft_;
    HudSpriteState prismBossHudShardRight_;
    std::array<HudSpriteState, 6> prismBossHudGlints_;

    std::array<HudSpriteState, 3> falseKingBossHudPhaseCrowns_;
    std::array<HudSpriteState, 2> falseKingBossHudPhaseDividers_;


    // 表示・退場の段階、実HPとの差分表示、被弾点滅の進行状態。
    PrismBossHudPhase prismBossHudPhase_ = PrismBossHudPhase::kHidden;
    float prismBossHudTimer_ = 0.0f;
    float prismBossHudDisplayedRate_ = 0.0f; // すぐに追従するHP表示率（0〜1）。
    float prismBossHudDelayedRate_ = 0.0f; // ダメージ後に遅れて減るHP表示率（0〜1）。
    float prismBossHudPreviousHp_ = 0.0f; // HP減少を検出するための前回値。
    float prismBossHudDamagePulseTimer_ = 0.0f;
    float prismBossHudDamageHoldTimer_ = 0.0f;
    float prismBossHudAnimationTimer_ = 0.0f;
    BossHudTheme prismBossHudTheme_ = BossHudTheme::kPrism;
    std::int32_t falseKingBossHudPreviousPhase_ = 1;
    float falseKingBossHudPhasePulseTimer_ = 0.0f;

    // --- ライフ減少演出 ---
    // UIの基準配置と、この演出専用のカメラ・3DObjectを分けて所有する。
    LifeLostPresentationState lifeLostState_;
    HudSpriteState lifeLostIcon_;
    HudSpriteState lifeLostXIcon_;
    std::array<HudSpriteState, 2> lifeLostDigits_;
    HudSpriteState lifeLostBackdrop_;
    std::unique_ptr<engine::Camera> lifeLostCamera_;
    std::unique_ptr<engine::Object3D> lifeLostSlimeObject_;
    std::unique_ptr<engine::Object3D> lifeLostStunObject_;

    // --- オーバーレイ ---
    // 通常HUDの上に出す独立UI。それぞれが表示と入力受付を担当する。
    std::unique_ptr<ControlsGuideOverlay> controlsGuideOverlay_;
    std::unique_ptr<PauseMenuOverlay> pauseMenuOverlay_;

    // ポーズから能力ページを開いた間だけ、Tab/Backでポーズ内メニューへ移れる。
    bool pauseGuideSession_ = false; // ポーズから開いたガイドを同じ停止セッションとして扱う状態。

    // ポーズメニューと操作ガイドを横移動で切り替える進行状態。
    bool pausePageSliding_ = false;
    bool pausePageToMenu_ = true;
    float pausePageSlideTime_ = 0.0f; // ポーズと操作ガイドのページ移動の経過秒数。
    std::unique_ptr<SettingsMenuOverlay> settingsOverlay_;
    std::unique_ptr<SaveIndicatorOverlay> saveIndicatorOverlay_;

    /// <summary>ステージ中に使うHUDスプライトと初期表示状態を用意する。</summary>
    void InitializeGameplayHUD();

    /// <summary>選択中のステージ設定を使い、シーンの基幹システムを初期化します。</summary>
    /// <param name="currentStage">読み込むステージの設定。</param>
    void InitializeCoreSystems(const StageData& currentStage);

    /// <summary>準備済みの描画基盤を借りてCPU粒子とGameRuleを作り、LightEditorへ3D基盤を渡す。</summary>
    void InitializeSceneParticles();

    /// <summary>ObjectManagerとステージObjectの実行基盤を用意する。</summary>
    void InitializeGameplayObjectSystems();

    /// <summary>GPU粒子のプリセットと描画用テクスチャを準備する。</summary>
    void InitializeGameplayParticleRuntime();

    /// <summary>破片演出の実行環境を初期化する。</summary>
    void InitializeGameplayDebrisRuntime();

    /// <summary>ステージの空を描くスカイボックスを初期化する。</summary>
    void InitializeGameplaySkybox();

    /// <summary>ステージ設定に応じた描画状態を適用します。</summary>
    /// <param name="currentStage">描画設定の元になるステージ。</param>
    void ApplyGameplayRenderState(const StageData& currentStage);

    /// <summary>Sprite配置とステージの視点設定を読み込む。</summary>
    void LoadCurrentStageSpritesAndView(const StageData& currentStage);

    /// <summary>ポーズ、操作ガイド、設定などの画面オーバーレイを用意する。</summary>
    void InitializeGameplayOverlays();

    /// <summary>保存済みのクリア状況をステージ内の王冠表示へ反映する。</summary>
    void ApplyGoalCrownState();

    /// <summary>保存済みのスターコイン取得状況をステージに反映する。</summary>
    void ApplyStageStarCoinState();

    /// <summary>ゴール王冠の待機中の動きを進める。</summary>
    void UpdateGoalCrownIdleAnimation(float deltaTime);

    /// <summary>復帰演出が必要なときだけ開始します。</summary>
    /// <returns>復帰演出を開始した場合は true。</returns>
    bool StartRespawnIrisInIfNeeded();

    /// <summary>ステージ入場演出の開始条件を確認し、成立したら開始します。</summary>
    /// <returns>入場演出を開始した場合は true。</returns>
    bool StartStageEntryPresentation();

    /// <summary>入口ゲートから出現する演出を更新する。</summary>
    void UpdateStageEntryPresentation(float deltaTime);

    /// <summary>入口ゲート演出を終え、通常操作とカメラを復元する。</summary>
    void FinishStageEntryPresentation();

    /// <summary>現在のステージで入場演出に使うゲートを探す。</summary>
    engine::Object3D* FindStageEntryGate() const;

    /// <summary>アリーナボスの登場ムービーを更新する。</summary>
    void UpdateArenaBossIntro(float deltaTime);

    /// <summary>ボス登場ムービーを終了し、対象参照と操作制限を整理する。</summary>
    void FinishArenaBossIntro();

    /// <summary>ボス撃破後の報酬登場演出を更新する。</summary>
    void UpdateArenaBossDefeatReward(float deltaTime);

    /// <summary>報酬登場演出を終了し、保持した対象参照を解除する。</summary>
    void FinishArenaBossDefeatReward();

    /// <summary>Editorからアニメーションを確認するためのプレビュー状態を用意する。</summary>
    void InitializeDebugAnimationPreview();

    /// <summary>シーンが所有する描画・音声・演出リソースを終了時に解放します。</summary>
    void FinalizeGameplayResources();

#ifdef USE_IMGUI

    /// <summary>Editorで指定した敵能力をプレイヤーへ適用する。</summary>
    void ApplyDebugPlayerMorph(const char* enemyType);

    /// <summary>Editorで適用したプレイヤーの能力状態を解除する。</summary>
    void ClearDebugPlayerMorph();

    // 次のゲーム更新で位置と水平向き（ラジアン）を適用します。
    bool QueueDebugTeleport(DebugTeleportRequest request);
    void ProcessPendingDebugTeleport();
    engine::Object3D* FindDebugObjectByName(const char* objectName) const;

    // 従来の固定ボタンと同じ基準をEditorの初期地点に変換します。
    std::optional<DebugTeleportRequest> ResolveDebugTeleportTarget(const char* anchorName, const engine::Vector3& anchorOffset,
                                                                   const char* facingObjectName, const char* label);
    void ApplyDebugTeleport(const DebugTeleportRequest& request);
#endif

    // 分割初期化で次に実行する処理。素材一覧は同じ段階のまま複数フレームに分ける。
    enum class LoadingPhase {
        kSelectStage,
        kCoreSystems,
        kSceneParticles,
        kObjectSystems,
        kGpuParticleRuntime,
        kGpuParticlePresets,
        kMeshEffects,
        kDebrisRuntime,
        kDebrisPresets,
        kVfxSequences,
        kSkybox,
        kStageObjects,
        kSpritesAndView,
        kHud,
        kOverlays,
        kSceneController,
        kCinematicPreviews,
        kComplete,
    };
    LoadingPhase loadingInitializePhase_ = LoadingPhase::kSelectStage;
    size_t loadingInitializeItemIndex_ = 0;
    size_t loadingInitializeCompletedUnits_ = 0;
    size_t loadingInitializeTotalUnits_ = 1;
    bool loadingStageObjectsStarted_ = false;

    /// <summary>撃破演出中は粒子、ワールド行列、UIまで更新し、通常進行を止める。</summary>
    bool UpdateCrownDefeatIfActive(float deltaTime);

    /// <summary>入場演出の準備または再生がこのフレームを使った場合はtrueを返す。</summary>
    bool UpdateStageEntryIfActive(float deltaTime, bool runtimePlaying);

    /// <summary>ゴール成立後の保存、結果表示、帰還を進め、フレームを受け持った場合はtrueを返す。</summary>
    bool UpdateGoalClearIfActive(float deltaTime);

    /// <summary>クリア記録を保存し、成功した内容をリザルトとステージ選択に引き継ぐ。</summary>
    void SaveGoalResultIfNeeded();

    /// <summary>通常進行を止めた状態でゴール演出、粒子、UIを更新する。</summary>
    void UpdateGoalClearPresentation(float deltaTime);

    /// <summary>決定入力で結果の数え上げを完了するか、ステージ選択への帰還を要求する。</summary>
    void HandleGoalResultConfirmInput();

    /// <summary>ゴール結果を表示するSpriteオーバーレイを用意する。</summary>
    void InitializeGoalPresentationOverlay();
    // 各時刻はクリア演出の開始からの秒数。見出しを出し、最後の文字の着地後に獲得物を表示する。
    float GetGoalResultTitleStartTime() const;
    float GetGoalRewardCoinStartTime() const;
    // コインの数え上げと祝福演出が進み、通常の帰還入力を受けられる時刻を返す。
    float GetGoalRewardReadyTime() const;
    // 画面ピクセル座標と表示倍率を指定し、獲得スターとコインの演出を時刻に合わせる。
    void UpdateGoalRewardOverlay(float time, float centerX, float rewardTop, float scale, bool visible);

    /// <summary>ゴール演出のタイムラインと各イベントを準備する。</summary>
    void InitializeGoalCinematicTimeline();

    /// <summary>シーケンスに保存された時刻をゴール演出の設定値へ反映します。</summary>
    void ApplyGoalCinematicTimingFromSequence();

    /// <summary>現在の設定値をシーケンスの時刻へ反映します。</summary>
    void SyncGoalCinematicTimelineFromTuning();

    /// <summary>シーケンス内のアニメーションクリップを対象へ適用します。</summary>
    /// <param name="target">アニメーションの対象。</param>
    /// <param name="clip">適用するクリップ。</param>
    /// <param name="localTime">クリップ内の経過秒。</param>
    /// <param name="isPreview">Editor プレビューからの呼び出しかどうか。</param>
    void HandleGoalCinematicAnimation(engine::Object3D* target, const CinematicAnimationClipData& clip, float localTime,
                                      bool isPreview);

    /// <summary>ゴール演出のシグナルを受け取り、終了待ち状態へ進めます。</summary>
    /// <param name="target">シーケンスが指定した対象。現在のシグナルでは使用しません。</param>
    /// <param name="signal">発火したシグナル。</param>
    /// <param name="isPreview">Editor プレビューからの呼び出しかどうか。現在のシグナルでは使用しません。</param>
    void HandleGoalCinematicSignal(engine::Object3D* target, const CinematicSignalMarker& signal, bool isPreview);

    /// <summary>ゴール演出の調整値を保存ファイルから読み込む。</summary>
    void LoadGoalPresentationTuning();

    /// <summary>現在のゴール演出調整値を保存する。</summary>
    void SaveGoalPresentationTuning() const;

    /// <summary>ゴール演出調整値を有効な範囲へ補正する。</summary>
    void SanitizeGoalPresentationTuning();

    /// <summary>ゴール演出の調整UIをEditorへ描く。</summary>
    void DrawGoalPresentationEditor();

    /// <summary>現在のステージに置かれたゴール王冠を探す。</summary>
    engine::Object3D* FindGoalCrownObject() const;

    /// <summary>Editor内でゴール演出の試写を開始する。</summary>
    void StartGoalPresentationPreview();

    /// <summary>Editor内のゴール演出試写を終了して状態を戻す。</summary>
    void StopGoalPresentationPreview();

    /// <summary>プレイヤー、王冠、カメラ、UIを含むゴール演出を更新する。</summary>
    void UpdateGoalPresentation(float deltaTime);

    /// <summary>クリア時のプレイヤーの祝福モーションを更新する。</summary>
    void UpdateGoalPlayerCelebration(float deltaTime);

    /// <summary>クリア演出中の王冠の移動と着地を更新する。</summary>
    void UpdateGoalCrownMotion(float deltaTime);

    /// <summary>ゴール演出の時刻に合わせてエフェクトを発生させる。</summary>
    void EmitGoalPresentationEffects(float deltaTime);

    /// <summary>ゴール演出で使用するカメラの初期構図を設定する。</summary>
    void SetupGoalPresentationCamera();

    /// <summary>ゴール演出の進行に合わせてカメラ構図を更新する。</summary>
    void UpdateGoalPresentationCamera();

    /// <summary>ゴール演出中の手動カメラ操作を抑制する。</summary>
    void LockGoalPresentationCameraInput();

    /// <summary>ゴール演出が変更したカメラ操作状態を復元する。</summary>
    void RestoreGoalPresentationCameraInput();

    /// <summary>クリア結果の画面オーバーレイを更新する。</summary>
    void UpdateGoalPresentationOverlay();

    /// <summary>クリア結果の画面オーバーレイを描画する。</summary>
    void DrawGoalPresentationOverlay();

    /// <summary>保存後にステージ選択へ戻る遷移を要求する。</summary>
    void RequestGoalReturnToSelect();

    /// <summary>通常プレイ中のポストエフェクト状態を更新する。</summary>
    void UpdatePostEffectState(float deltaTime);

    /// <summary>ロックオン対象と有効カメラの追従状態を更新する。</summary>
    void UpdateLockOnAndCamera(float deltaTime);

    /// <summary>ロックオン対象を投影し、マーカーの位置と表示を更新します。</summary>
    /// <param name="camera">ワールド座標を投影するカメラ。</param>
    /// <param name="target">現在のロックオン対象。</param>
    /// <param name="deltaTime">マーカー更新に使う経過秒数。</param>
    void UpdateLockOnSprite(engine::Camera* camera, engine::Object3D* target, float deltaTime);

    /// <summary>ステージ内のゲームシステムと演出管理器を更新する。</summary>
    void UpdateSceneSystems(float deltaTime);

    /// <summary>Editorで使うエフェクト操作のショートカット入力を処理する。</summary>
    void UpdateEffectDebugShortcuts();

    /// <summary>残機、コインなど通常プレイ中のHUDを更新する。</summary>
    void UpdateGameplayHUD(float deltaTime);

    /// <summary>スターコインの取得数と表示状態をHUDへ反映する。</summary>
    void UpdateStageStarHUD(float deltaTime, bool visible);

    /// <summary>表示対象を優先順に選び、HUDに必要な値を取得する。</summary>
    BossHudDisplay FindBossHudDisplay();

    /// <summary>選んだボスの表示値から、登場・被弾・退場の表示を更新する。</summary>
    void UpdateBossHud(const BossHudDisplay& display, float deltaTime);

    /// <summary>残機減少の画面演出を進める。</summary>
    void UpdateLifeLostPresentation(float deltaTime);

    /// <summary>操作ガイドを更新し、このフレームの入力を消費したか返す。</summary>
    bool HandleControlsGuideOverlay(float deltaTime);

    /// <summary>ポーズメニューと操作ガイドのページ移動を開始する。</summary>
    /// <param name="toMenu">ポーズメニューへ移る場合はtrue。</param>
    void BeginPausePageSlide(bool toMenu);

    /// <summary>ポーズ画面のページ移動アニメーションを進める。</summary>
    void UpdatePausePageSlide(float deltaTime);

    /// <summary>ポーズ中の入力と設定画面を処理し、通常更新を止めるか返す。</summary>
    bool HandlePauseOverlay(float deltaTime);

    /// <summary>ポーズを開く入力がこのフレームで発生したか返す。</summary>
    bool IsPauseOpenTriggered() const;

    /// <summary>残機やコインの通常HUDを描画する。</summary>
    void DrawGameplayHUD();

    /// <summary>スターコイン取得状態のHUDを描画する。</summary>
    void DrawStageStarHUD();

    /// <summary>現在のボスHUDを描画する。</summary>
    void DrawBossHud();

    /// <summary>残機減少の画面オーバーレイを描画する。</summary>
    void DrawLifeLostPresentation();

    /// <summary>残機減少のワールド演出に使うObjectを用意する。</summary>
    void InitializeLifeLostPresentationObjects();

    /// <summary>残機減少のワールド演出の位置と時間を更新する。</summary>
    void UpdateLifeLostPresentationWorld(float deltaTime);

    /// <summary>ライフ減少演出のワールド側オブジェクトをライト付きで描画します。</summary>
    void DrawLifeLostPresentationWorld(const engine::SceneLighting& lighting);

    /// <summary>配置JSONから読み込んだHUDを名前で取得し、演出前の基準値を記録する。</summary>
    /// <param name="name">gameplayHUD.json内のSprite名。</param>
    HudSpriteState BindHud(const std::string& name);

    /// <summary>参照があるHUDスプライトを現在の状態で描く。可視性はSprite::Draw()が判定する。</summary>
    void DrawGameplayHUDSprite(const HudSpriteState& state);

    /// <summary>指定SpriteがゲームプレイHUDに属するか判定する。</summary>
    bool IsGameplayHUDSprite(const engine::Sprite* sprite) const;

    /// <summary>二桁の HUD 数字を、指定した右端位置に合わせて更新します。</summary>
    /// <param name="digits">更新する十の位と一の位のスプライト。</param>
    /// <param name="value">表示する数値。0～99へ丸める。</param>
    /// <param name="rightAlignedPosition">画面ピクセルで指定する表示の右端Xと中心Y。</param>
    /// <param name="digitHeight">表示する数字の高さをピクセルで指定。</param>
    /// <param name="color">数字の描画色。</param>
    /// <param name="visible">数字を表示するかどうか。</param>
    void SetGameplayHUDNumber(std::array<HudSpriteState, 2>& digits, std::int32_t value, const engine::Vector2& rightAlignedPosition,
                              float digitHeight, const engine::Vector4& color, bool visible);

    /// <summary>取得したスターコインが画面のHUDへ飛ぶ演出を開始する。</summary>
    void StartStageStarHUDCollectEffect(std::int32_t starIndex, const engine::Vector3& worldPosition);

    /// <summary>ワールド位置を画面ピクセルへ投影する。カメラ不在か投影XYが非有限値なら画面中央を返す。</summary>
    engine::Vector2 ProjectWorldToScreen(const engine::Vector3& worldPosition) const;

    /// <summary>HUD表示対象のプリズムボスを探す。</summary>
    class EnemyPrismSlime* FindPrismBossForHUD();

    /// <summary>HUD表示対象のマグマボスを探す。</summary>
    class EnemyMagmaSlime* FindMagmaBossForHUD();

    /// <summary>HUD表示対象の最終ボスを探す。</summary>
    class EnemyFalseKingSlime* FindFalseKingBossForHUD();

    /// <summary>可視指定のObjectを視錐台で判定する。偽王と王冠変身中のプレイヤーは本体だけで除外しない。</summary>
    bool IsVisible(engine::Object3D* obj);
};
