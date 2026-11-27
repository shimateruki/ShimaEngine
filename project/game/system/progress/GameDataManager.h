#pragma once
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include "json.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

// セーブデータ、ステージクリア状況、スターコイン、残機/コインを一元管理する
// 関連：各シーンが進行を読み書きし、通常コインなどの保存予約と一時的な演出要求も保持する。
// 主な入口：Initialize / CommitStageClear / FlushPendingSave
class GameDataManager {
  public:
    static constexpr std::int32_t kSaveSlotCount = 3;
    static constexpr std::int32_t kStarCoinsPerStage = 3;
    static constexpr std::int32_t kStartingLives = 20;
    static constexpr std::int32_t kMaxLives = 99;
    static constexpr std::int32_t kMaxCoins = 9999;
    static constexpr std::int32_t kCoinsPerLife = 100;

    // タイトルや設定画面で表示する、セーブスロットの概要情報
    struct SaveSlotSummary {
        bool exists = false;
        bool tutorialCleared = false;
        std::int32_t clearedStageCount = 0;
        std::int32_t collectedStarCoins = 0;
        std::int32_t lives = kStartingLives;
        std::int32_t coins = 0;
        std::int32_t playTimeSeconds = 0;
    };

    // ステージクリア後にセレクト画面で王冠数を増やす演出リクエスト
    struct StageClearRewardPresentation {
        bool active = false;
        std::int32_t stageIndex = -1;
        std::int32_t previousCrownCount = 0;
        std::int32_t newCrownCount = 0;
    };

    // クリア後にセレクトへ戻った直後、対応するゲートからプレイヤーを出す演出の予約情報。
    struct StageSelectReturnPresentation {
        bool active = false;
        std::int32_t stageIndex = -1;
        std::int32_t previousCrownCount = 0;
        std::int32_t newCrownCount = 0;
        std::vector<std::int32_t> newStarCoinIndices;
    };

    // ステージ内チェックポイントからの再開に必要な一時情報です。
    // セーブデータには書き込まず、死亡によるシーン再生成を一度だけ跨ぎます。
    struct StageCheckpointRespawn {
        bool active = false;
        std::int32_t stageIndex = -1;
        float positionX = 0.0f;
        float positionY = 0.0f;
        float positionZ = 0.0f;
        std::string checkpointObjectName;
        std::string morphEnemyType;
        std::vector<std::int32_t> clearedEncounterIds;
    };

    /// セーブ状態を共有する唯一のインスタンスを返す。
    static GameDataManager* GetInstance() {
        static GameDataManager sInstance;
        return &sInstance;
    }

    // 起動時は現在のスロットから保存状態を復元します。
    void Initialize() { Load(); }

    /// <summary>有効なセーブスロットへ切り替え、その内容を読み込みます。</summary>
    /// <param name="slotIndex">0 始まりのスロット番号。範囲外は端の番号に丸めます。</param>
    void SetActiveSlot(std::int32_t slotIndex) {
        // 未保存の通常コインと時間を別スロットへ混ぜない。
        if (!FlushPendingSave())
            return;
        // 先頭より小さい番号では、最初のスロットを選ぶ。
        if (slotIndex < 0)
            slotIndex = 0;
        // 最後を越す番号では、最終スロットを選ぶ。
        if (slotIndex >= kSaveSlotCount)
            slotIndex = kSaveSlotCount - 1;
        activeSlot_ = slotIndex;
        Load();
    }

    /// 現在選択中の0始まりのセーブスロット番号を返す。
    std::int32_t GetActiveSlot() const { return activeSlot_; }

    /// 現在のスロットの保存ファイルパスを返す。
    std::string GetSaveFilePath() const { return GetSaveFilePath(activeSlot_); }

    /// 指定スロットの保存ファイルパスを返す。範囲外番号は端へ丸める。
    std::string GetSaveFilePath(std::int32_t slotIndex) const {
        // 負の番号をファイル名に使わず、最初のスロットの番号に揃える。
        if (slotIndex < 0)
            slotIndex = 0;
        // 上限を越えた番号でも、実在する最後の枠のパスを返す。
        if (slotIndex >= kSaveSlotCount)
            slotIndex = kSaveSlotCount - 1;
        return "Resources/json/save/savedata_slot" + std::to_string(slotIndex + 1) + ".json";
    }

    /// <summary>指定スロットの一覧表示用情報を、現在のプレイ状態を変えずに読み取ります。</summary>
    /// <param name="slotIndex">0 始まりのスロット番号。</param>
    /// <returns>読み込めない場合は exists が false の概要。</returns>
    SaveSlotSummary GetSlotSummary(std::int32_t slotIndex) const {
        SaveSlotSummary summary;
        nlohmann::json data;
        // 本体・バックアップなどを読めない場合は、スロット未作成として既定の概要を返す。
        if (!ReadSaveJson(slotIndex, data)) {
            return summary;
        }

        summary.exists = true;
        summary.lives = std::clamp(ReadInteger(data, "lives", kStartingLives), 0, kMaxLives);
        summary.coins = std::clamp(ReadInteger(data, "coins", 0), 0, kMaxCoins);
        summary.playTimeSeconds = (std::max)(0, ReadInteger(data, "playTimeSeconds", 0));

        // クリアの配列が存在する場合にのみ、チュートリアルと通常ステージの記録を集計する。
        if (data.contains("clearedStages") && data["clearedStages"].is_array()) {
            for (const auto& stage : data["clearedStages"]) {
                std::int32_t stageIndex = 0;
                // 整数にできない要素はクリアの件数に入れない。
                if (!TryReadInteger(stage, stageIndex))
                    continue;

                // -1 はチュートリアルのクリア記録で、通常ステージの件数に含めない。
                if (stageIndex == -1) {
                    summary.tutorialCleared = true;
                // 0以上の番号だけを、通常ステージのクリア件数に加える。
                } else if (stageIndex >= 0) {
                    summary.clearedStageCount++;
                }
            }
        }

        // ステージごとの有効な三枠だけを数え、旧ファイルの余分な要素を一覧の件数に含めません。
        if (data.contains("starCoins") && data["starCoins"].is_object()) {
            for (auto it = data["starCoins"].begin(); it != data["starCoins"].end(); ++it) {
                // ステージの値が配列でなければ、そのグループを集計しない。
                if (!it.value().is_array())
                    continue;
                const auto count = (std::min)(it.value().size(), static_cast<size_t>(kStarCoinsPerStage));
                for (size_t index = 0; index < count; ++index) {
                    const auto& coin = it.value()[index];
                    // 真偽値の true だけを取得済みとして数える。
                    if (coin.is_boolean() && coin.get<bool>()) {
                        summary.collectedStarCoins++;
                    }
                }
            }
        }

        return summary;
    }

    // 保存ファイルに未管理の項目があれば保持し、管理中の項目だけ更新します。
    bool Save();

    /// <summary>通常コインとプレイ時間の保留分を、シーン移動・終了時に保存します。</summary>
    bool FlushPendingSave() { return !dirty_ || Save(); }

    /// <summary>連続取得した通常コインを最大1秒分まとめ、プレイ時間は30秒ごとに保存します。</summary>
    void TickAutosave(float deltaTime) {
        // 未保存の変更がない場合や、経過秒が非正または有限でない場合は保存時計を進めない。
        if (!dirty_ || !std::isfinite(deltaTime) || deltaTime <= 0.0f)
            return;
        saveDelaySeconds_ -= deltaTime;
        // 保存に失敗すると未保存状態を残し、毎フレーム再試行せず5秒後にもう一度試します。
        if (saveDelaySeconds_ <= 0.0f && !Save())
            saveDelaySeconds_ = 5.0f;
    }

    /// <summary>操作可能なシーンの実時間を秒単位で記録します。停止中は呼びません。</summary>
    void AdvancePlayTime(float deltaTime) {
        // 正で有限な経過秒だけを受け付ける。記録の最大秒数まで到達していれば加算しない。
        if (!std::isfinite(deltaTime) || deltaTime <= 0.0f || playTimeSeconds_ >= kMaxPlayTimeSeconds)
            return;
        playTimeRemainder_ += deltaTime;
        // 整数の1秒に達するまでは端数だけを保持し、保存予定を追加しない。
        if (playTimeRemainder_ < 1.0f)
            return;
        // 一秒未満の端数を次のフレームへ残してから、記録秒数と保存の予定を更新します。
        const std::int32_t addedSeconds = static_cast<std::int32_t>(playTimeRemainder_);
        playTimeRemainder_ -= static_cast<float>(addedSeconds);
        playTimeSeconds_ = (std::min)(kMaxPlayTimeSeconds, playTimeSeconds_ + addedSeconds);
        dirty_ = true;
        // 通常コインなどの保存予定がまだ無い場合は、30秒後の書き出しを予約する。
        if (saveDelaySeconds_ <= 0.0f)
            saveDelaySeconds_ = 30.0f;
    }

    /// <summary>ゴールでクリア記録と今回のスターコインを一度に確定します。</summary>
    bool CommitStageClear(std::int32_t stageIdx, const std::array<bool, kStarCoinsPerStage>& collected) {
        // 保存失敗時はクリア記録と今回のスターコインを揃って巻き戻す。
        const auto oldCleared = clearedStages_;
        const auto oldCoins = stageStarCoins_;
        // クリア済みのステージは重複登録せず、今回取ったスターコインは既存の取得状況へ足す。
        if (!IsStageCleared(stageIdx))
            clearedStages_.push_back(stageIdx);
        for (std::int32_t i = 0; i < kStarCoinsPerStage; ++i) {
            // 今回取得できた枠だけ true にし、過去の取得を false に戻さない。
            if (collected[i])
                GetOrCreateStarCoins(stageIdx)[i] = true;
        }
        // クリアと収集の両方を同じ保存で確定し、失敗した場合はメモリ上の両方を戻す。
        if (Save())
            return true;
        clearedStages_ = oldCleared;
        stageStarCoins_ = oldCoins;
        return false;
    }

    // ファイルがないか壊れている場合は、新規ゲームの初期値へ戻します。
    void Load();

    // 残機操作。変更はその場で保存します。
    std::int32_t GetLives() const { return lives_; }

    // 残機を0〜上限へ制限し、その値をすぐに保存する。
    void SetLives(std::int32_t lives) {
        lives_ = std::clamp(lives, 0, kMaxLives);
        Save();
    }

    // 旗の報酬は即時に表示し、書き込みは通常コインと同じ待ち時間にまとめる。
    void AwardCheckpointLife() {
        // 残機が上限なら増やせないため、新しい自動保存も予約しない。
        if (lives_ >= kMaxLives)
            return;
        ++lives_;
        QueueProgressSave();
    }

    void SubtractLife() {
        // 残機はゼロで止め、減少結果を即座に保存する。
        lives_--;
        // 減少後に負になった場合は、保存前に下限の0へ戻す。
        if (lives_ < 0)
            lives_ = 0;
        Save();
    }

    // 残機リセットは保存が成功したときだけ確定し、失敗時はメニューから再試行できるようにする。
    bool ResetLives() {
        const std::int32_t previousLives = lives_;
        lives_ = kStartingLives;
        // 保存が成功した場合は初期残機を確定する。失敗時は後続で以前の値を戻す。
        if (Save()) {
            return true;
        }
        lives_ = previousLives;
        return false;
    }

    // 設定操作は即時保存し、ゲーム中の連続取得だけ短時間まとめます。
    std::int32_t GetCoins() const { return coins_; }

    // 設定画面やデバッグから指定された所持枚数を範囲内へ収めて即時保存する。
    void SetCoins(std::int32_t coins) {
        coins_ = std::clamp(coins, 0, kMaxCoins);
        Save();
    }

    /// <summary>コインを加算し、100 枚ごとに残機へ換算します。</summary>
    /// <param name="amount">加算する枚数。0 以下は無視します。</param>
    void AddCoin(std::int32_t amount = 1) {
        // 0枚以下の要求は進行を変えず、保存も予約しない。
        if (amount <= 0) {
            return;
        }

        // 大量取得でも int の加算をオーバーフローさせず、100 枚ごとに残機へ換算します。
        const int64_t totalCoins = static_cast<int64_t>(coins_) + amount;
        const int64_t earnedLives = totalCoins / kCoinsPerLife;
        lives_ =
            static_cast<std::int32_t>((std::min)(static_cast<int64_t>(kMaxLives), static_cast<int64_t>(lives_) + earnedLives));
        coins_ = static_cast<std::int32_t>(totalCoins % kCoinsPerLife);
        QueueProgressSave();
    }

    // コインを0枚へ戻し、その場で保存する。
    void ResetCoins() {
        coins_ = 0;
        Save();
    }

    std::int32_t GetPlayTimeSeconds() const { return playTimeSeconds_; }

    // 指定した秒を保存範囲へ収め、以前の端数秒を引き継がずに即時保存する。
    void SetPlayTimeSeconds(std::int32_t seconds) {
        playTimeSeconds_ = std::clamp(seconds, 0, kMaxPlayTimeSeconds);
        playTimeRemainder_ = 0.0f;
        Save();
    }

    // リスポーン時の画面遷移は一回だけ消費する要求として保持します。
    void RequestRespawnIrisIn() { pendingRespawnIrisIn_ = true; }

    // 呼出元へアイリス開始の有無を一度だけ返して、保持していた要求を消す。
    bool ConsumeRespawnIrisInRequest() {
        const bool requested = pendingRespawnIrisIn_;
        pendingRespawnIrisIn_ = false;
        return requested;
    }

    /// <summary>死亡後に戻るチェックポイントと、復帰させるコピー能力を予約します。</summary>
    /// <param name="stageIndex">復帰先ステージの番号。</param>
    /// <param name="positionX">復帰先のワールド X 座標。</param>
    /// <param name="positionY">復帰先のワールド Y 座標。</param>
    /// <param name="positionZ">復帰先のワールド Z 座標。</param>
    /// <param name="morphEnemyType">復帰後に維持するコピー元の敵種別。</param>
    /// <param name="checkpointObjectName">取得済みの旗をシーン再生成後に特定する名前。旗以外なら空。</param>
    void RequestStageCheckpointRespawn(std::int32_t stageIndex, float positionX, float positionY, float positionZ,
                                       const std::string& morphEnemyType, const std::string& checkpointObjectName) {
        // 同じステージの再開要求なら、クリア済み遭遇イベントの記録を保つ。
        if (pendingStageCheckpointRespawn_.stageIndex != stageIndex)
            pendingStageCheckpointRespawn_.clearedEncounterIds.clear();
        pendingStageCheckpointRespawn_.active = stageIndex >= 0;
        pendingStageCheckpointRespawn_.stageIndex = stageIndex;
        pendingStageCheckpointRespawn_.positionX = positionX;
        pendingStageCheckpointRespawn_.positionY = positionY;
        pendingStageCheckpointRespawn_.positionZ = positionZ;
        pendingStageCheckpointRespawn_.morphEnemyType = morphEnemyType;
        pendingStageCheckpointRespawn_.checkpointObjectName = checkpointObjectName;
    }

    // 同じチェックポイントへの復帰では、クリア済み遭遇イベントを再起動しません。
    void RememberCheckpointEncounter(std::int32_t stageIndex, std::int32_t eventId) {
        // ステージが異なる場合は、旧ステージの撃破 ID を予約情報から外す。
        if (pendingStageCheckpointRespawn_.stageIndex != stageIndex) {
            pendingStageCheckpointRespawn_.clearedEncounterIds.clear();
            pendingStageCheckpointRespawn_.stageIndex = stageIndex;
        }
        auto& ids = pendingStageCheckpointRespawn_.clearedEncounterIds;
        // 同じ遭遇を再び記録しないよう、未登録の ID のみ追記する。
        if (std::find(ids.begin(), ids.end(), eventId) == ids.end())
            ids.push_back(eventId);
    }

    // Consume 系の要求は返却時に空にし、シーン再生成で二重再生しません。
    StageCheckpointRespawn ConsumeStageCheckpointRespawn() {
        StageCheckpointRespawn request = pendingStageCheckpointRespawn_;
        pendingStageCheckpointRespawn_ = {};
        return request;
    }

    /// <summary>王冠数が増えた場合に、セレクト画面の報酬演出を予約します。</summary>
    /// <param name="stageIndex">クリアしたステージ番号。</param>
    /// <param name="previousCrownCount">演出開始前の王冠数。</param>
    /// <param name="newCrownCount">演出終了後の王冠数。</param>
    void RequestStageClearRewardPresentation(std::int32_t stageIndex, std::int32_t previousCrownCount, std::int32_t newCrownCount) {
        // ステージ番号が無効か王冠数が増えていない場合は、古い報酬予約を消して終了する。
        if (stageIndex < 0 || newCrownCount <= previousCrownCount) {
            pendingStageClearReward_ = {};
            return;
        }

        pendingStageClearReward_.active = true;
        pendingStageClearReward_.stageIndex = stageIndex;
        pendingStageClearReward_.previousCrownCount = std::clamp(previousCrownCount, 0, 999);
        pendingStageClearReward_.newCrownCount = std::clamp(newCrownCount, 0, 999);
    }

    /// 王冠報酬の演出予約を取り出し、同時に予約を空にする。
    StageClearRewardPresentation ConsumeStageClearRewardPresentation() {
        StageClearRewardPresentation request = pendingStageClearReward_;
        pendingStageClearReward_ = {};
        return request;
    }

    /// <summary>クリア後にステージゲートから戻る演出の内容を予約します。</summary>
    /// <param name="stageIndex">戻り元のステージ番号。</param>
    /// <param name="previousCrownCount">クリア前の王冠数。</param>
    /// <param name="newCrownCount">クリア後の王冠数。</param>
    /// <param name="newStarCoinIndices">今回新たに取得したスターコインの番号。</param>
    void RequestStageSelectReturnPresentation(std::int32_t stageIndex, std::int32_t previousCrownCount, std::int32_t newCrownCount,
                                              const std::vector<std::int32_t>& newStarCoinIndices) {
        pendingStageSelectReturn_.active = true;
        pendingStageSelectReturn_.stageIndex = stageIndex;
        pendingStageSelectReturn_.previousCrownCount = std::clamp(previousCrownCount, 0, 999);
        pendingStageSelectReturn_.newCrownCount = std::clamp(newCrownCount, 0, 999);
        pendingStageSelectReturn_.newStarCoinIndices = newStarCoinIndices;
    }

    /// ゲートから帰還する演出予約を取り出し、同時に予約を空にする。
    StageSelectReturnPresentation ConsumeStageSelectReturnPresentation() {
        StageSelectReturnPresentation request = pendingStageSelectReturn_;
        pendingStageSelectReturn_ = {};
        return request;
    }

    // ステージクリア・解放確認の状態。変更時のみ保存します。
    void MarkStageCleared(std::int32_t index) {
        // 未クリアの場合だけステージ番号を追記し、その変化を保存する。
        if (std::find(clearedStages_.begin(), clearedStages_.end(), index) == clearedStages_.end()) {
            clearedStages_.push_back(index);
            Save();
        }
    }

    /// ステージのクリア記録を設定する。状態が変わった場合だけ保存する。
    void SetStageCleared(std::int32_t index, bool cleared) {
        auto it = std::find(clearedStages_.begin(), clearedStages_.end(), index);
        // クリア済みにする指定は、新たに登録する必要がある場合だけ保存する。
        if (cleared) {
            // すでに記録があれば、重複登録と同じ内容の保存を省く。
            if (it == clearedStages_.end()) {
                clearedStages_.push_back(index);
                Save();
            }
            return;
        }

        // クリアを取り消す指定で、記録が存在するときだけ削除して保存する。
        if (it != clearedStages_.end()) {
            clearedStages_.erase(it);
            Save();
        }
    }

    /// 指定ステージがクリア済みか返す。
    bool IsStageCleared(std::int32_t index) const {
        return std::find(clearedStages_.begin(), clearedStages_.end(), index) != clearedStages_.end();
    }

    void MarkStageUnlockSeen(std::int32_t index) {
        // 同じ解放通知を再表示しないよう、未登録のステージだけ記録する。
        if (std::find(seenUnlockedStages_.begin(), seenUnlockedStages_.end(), index) == seenUnlockedStages_.end()) {
            seenUnlockedStages_.push_back(index);
            Save();
        }
    }

    /// ステージ解放の確認済み状態を設定し、変化時だけ保存する。
    void SetStageUnlockSeen(std::int32_t index, bool seen) {
        auto it = std::find(seenUnlockedStages_.begin(), seenUnlockedStages_.end(), index);
        // 視聴済みにする指定では、未登録の場合だけ追加する。
        if (seen) {
            // 前に記録された番号は、同じ記録を書き直さない。
            if (it == seenUnlockedStages_.end()) {
                seenUnlockedStages_.push_back(index);
                Save();
            }
            return;
        }

        // 視聴済みを取り消す指定で、該当記録がある場合だけ削除して保存する。
        if (it != seenUnlockedStages_.end()) {
            seenUnlockedStages_.erase(it);
            Save();
        }
    }

    /// ステージ解放を確認済みか返す。
    bool IsStageUnlockSeen(std::int32_t index) const {
        return std::find(seenUnlockedStages_.begin(), seenUnlockedStages_.end(), index) != seenUnlockedStages_.end();
    }

    // スターコインはステージ番号とステージ内の番号で管理します。
    void MarkStarCoinCollected(std::int32_t stageIdx, std::int32_t coinIdx) {
        // ステージ内の三枠の外を指定した場合は取得処理に渡さない。
        if (coinIdx < 0 || coinIdx >= kStarCoinsPerStage)
            return;
        SetStarCoinCollected(stageIdx, coinIdx, true);
    }

    /// ステージ内のスターコイン取得状態を設定する。番号が無効なら何もしない。
    void SetStarCoinCollected(std::int32_t stageIdx, std::int32_t coinIdx, bool collected) {
        // 有効な枠の番号を要求し、範囲外ではステージ用の保存項目も作らない。
        if (coinIdx < 0 || coinIdx >= kStarCoinsPerStage)
            return;

        auto& coins = GetOrCreateStarCoins(stageIdx);
        // 既存の取得状態と同じなら、保存ファイルの更新は省く。
        if (coins[coinIdx] == collected)
            return;
        coins[coinIdx] = collected;
        Save();
    }

    /// 指定ステージのコインを取得済みか返す。未登録や無効番号は false。
    bool IsStarCoinCollected(std::int32_t stageIdx, std::int32_t coinIdx) const {
        // ステージの記録が無ければ未取得として扱う。
        if (stageStarCoins_.find(stageIdx) == stageStarCoins_.end())
            return false;
        // 三枠の外にある番号を配列へ渡さず、未取得を返す。
        if (coinIdx < 0 || coinIdx >= kStarCoinsPerStage)
            return false;
        return stageStarCoins_.at(stageIdx)[coinIdx];
    }

    /// チュートリアルを除くクリア済みステージ数を返す。
    std::int32_t GetClearedStageCount() const {
        std::int32_t count = 0;
        for (std::int32_t stageIndex : clearedStages_) {
            // 通常ステージの0以上だけを数え、チュートリアルなどの負の番号は除く。
            if (stageIndex >= 0) {
                ++count;
            }
        }
        return count;
    }

    /// 全ステージで取得済みのスターコイン数を返す。
    std::int32_t GetCollectedStarCoinCount() const {
        std::int32_t count = 0;
        for (const auto& [stageIndex, coins] : stageStarCoins_) {
            (void)stageIndex;
            for (bool collected : coins) {
                // 取得フラグが真の枠だけ、全ステージの合計へ加える。
                if (collected) {
                    ++count;
                }
            }
        }
        return count;
    }

    /// 現在のスロットの進行・残機・コイン・時間を初期値に戻して保存する。
    void ResetAll() {
        lives_ = kStartingLives;
        coins_ = 0;
        playTimeSeconds_ = 0;
        playTimeRemainder_ = 0.0f;
        clearedStages_.clear();
        seenUnlockedStages_.clear();
        stageStarCoins_.clear();
        Save();
    }

    /// <summary>指定スロットの保存ファイルを削除します。</summary>
    /// <param name="slotIndex">0 始まりのスロット番号。</param>
    /// <returns>本体・バックアップ・一時ファイル・対象となる旧ファイルのいずれかを削除でき、途中エラーがなければ true。</returns>
    bool DeleteSlot(std::int32_t slotIndex) {
        // 削除はスロットの番号が厳密に有効範囲内の場合にだけ受け付ける。
        if (slotIndex < 0 || slotIndex >= kSaveSlotCount) {
            return false;
        }

        std::error_code ec;
        const std::string path = GetSaveFilePath(slotIndex);

        // バックアップを残すと削除したスロットが次回起動時に復活してしまう。
        bool removed = std::filesystem::remove(path + ".bak", ec);
        // バックアップを削除できない場合は、本体の削除前に失敗を返す。
        if (ec)
            return false;
        removed = std::filesystem::remove(path + ".tmp", ec) || removed;
        // 一時ファイルの削除で問題が起きた場合は、それ以降を処理しない。
        if (ec)
            return false;
        removed = std::filesystem::remove(path, ec) || removed;
        // 本体の削除に失敗したら、メモリの読込状態を変更する前にエラーを返す。
        if (ec)
            return false;
        // 最初のスロットだけ旧ファイルも読込対象なので、同時に取り除きます。
        if (slotIndex == 0) {
            removed = std::filesystem::remove(GetLegacySaveFilePath(), ec) || removed;
            // 旧形式を削除できなかった場合も、該当スロットの削除は失敗として扱う。
            if (ec)
                return false;
        }

        // 現在の枠を削除した場合は、以前の進行を使わないよう読込をやり直します。
        if (slotIndex == activeSlot_) {
            Load();
        }
        return removed;
    }

  private:
    GameDataManager() : lives_(kStartingLives), coins_(0), playTimeSeconds_(0) {}

    ~GameDataManager() = default;

    // JSON の整数が int の範囲外なら変換せず、破損した値を読み飛ばします。
    static bool TryReadInteger(const nlohmann::json& value, std::int32_t& output) {
        // 小数など整数以外の JSON を、丸めてセーブ上の整数として扱わない。
        if (!value.is_number_integer()) {
            return false;
        }
        // 符号なし値は符号付き64ビットへ変換せず、先に正の上限と比べる。
        if (value.is_number_unsigned()) {
            const uint64_t number = value.get<uint64_t>();
            // 符号なしの値が int32 の正の上限を越える場合は、出力を変更しない。
            if (number > static_cast<uint64_t>((std::numeric_limits<std::int32_t>::max)())) {
                return false;
            }
            output = static_cast<std::int32_t>(number);
            return true;
        }
        const int64_t number = value.get<int64_t>();
        // 符号付きの値は上下限の両方を照合し、収まった場合だけ変換する。
        if (number < (std::numeric_limits<std::int32_t>::min)() || number > (std::numeric_limits<std::int32_t>::max)()) {
            return false;
        }
        output = static_cast<std::int32_t>(number);
        return true;
    }

    // セーブの数値項目が壊れていても、他の項目は復元できるよう既定値に戻します。
    static std::int32_t ReadInteger(const nlohmann::json& data, const char* key, std::int32_t fallback) {
        // 省略されたキーでは指定された既定値を返す。
        if (!data.contains(key)) {
            return fallback;
        }
        std::int32_t value = fallback;
        return TryReadInteger(data[key], value) ? value : fallback;
    }

    std::string GetLegacySaveFilePath() const { return "Resources/json/save/savedata.json"; }

    /// <summary>一つの保存ファイルを開いて解析する。複数候補の選択は ReadSaveJson() が担当する。</summary>
    /// <returns>JSON オブジェクトを読めた場合は true。</returns>
    static bool ReadJsonFile(const std::filesystem::path& path, nlohmann::json& outData) {
        std::ifstream file(path);
        // 指定した保存ファイルを開けない場合は、その世代の読込を失敗させる。
        if (!file)
            return false;
        try {
            file >> outData;
        } catch (...) {
            return false;
        }
        return outData.is_object();
    }

    /// 本体、バックアップ、旧形式の順にスロットの保存JSONを探す。
    bool ReadSaveJson(std::int32_t slotIndex, nlohmann::json& outData) const;

    /// ステージの取得フラグ配列を三枠に整えて返す。未登録なら作成する。
    std::vector<bool>& GetOrCreateStarCoins(std::int32_t stageIdx) {
        auto& coins = stageStarCoins_[stageIdx];
        coins.resize(kStarCoinsPerStage, false);
        return coins;
    }

    std::int32_t lives_ = kStartingLives;
    std::int32_t coins_ = 0;

    std::int32_t playTimeSeconds_ = 0;
    static constexpr std::int32_t kMaxPlayTimeSeconds = 99 * 60 * 60 + 59 * 60 + 59;
    float playTimeRemainder_ = 0.0f; // プレイ時間を整数秒へ加える前の端数秒。

    // 通常取得の進行を、最大1秒待って他の取得とまとめて書き出す。
    void QueueProgressSave() {
        dirty_ = true;

        // 追加の報酬で期限を延長せず、連続取得中も保存を進める。
        if (saveDelaySeconds_ <= 0.0f || saveDelaySeconds_ > 1.0f)
            saveDelaySeconds_ = 1.0f;
    }

    float saveDelaySeconds_ = 0.0f; // まとめて保存するまでの待ち秒数。
    bool dirty_ = false; // メモリ上の進行が保存ファイルへ未反映か。
    std::int32_t activeSlot_ = 0; // 現在のセーブ枠。0始まり。

    bool pendingRespawnIrisIn_ = false; // 再生成したシーンが一度だけ消費する復帰アイリス要求。
    StageCheckpointRespawn pendingStageCheckpointRespawn_; // ファイル保存せず、次のシーン生成へ一度だけ渡す再開情報。

    StageClearRewardPresentation pendingStageClearReward_; // ステージセレクトが一度だけ消費する王冠数の演出要求。
    StageSelectReturnPresentation pendingStageSelectReturn_; // ステージセレクトが一度だけ消費するゲート帰還演出の情報。

    std::vector<std::int32_t> clearedStages_;
    std::vector<std::int32_t> seenUnlockedStages_;
    std::map<std::int32_t, std::vector<bool>> stageStarCoins_; // ステージ番号 -> [コイン0, コイン1, コイン2]
};
