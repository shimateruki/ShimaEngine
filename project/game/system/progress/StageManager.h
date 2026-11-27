#pragma once
#include <cstdint>

#include <fstream>
#include <string>
#include <utility>
#include <vector>
#include "json.hpp"
#include "engine/utility/JsonFile.h"
#include "game/system/progress/StageCatalog.h"

// ステージ一覧と現在の選択を管理します。JSON が読めない場合は内蔵の一覧へ戻します。
// 関連：StageCatalogから一覧を作り、シーンが選択したステージ番号を共有する。
// 主な入口：Initialize / GetCurrentStage / SetCurrentStage
class StageManager {
  public:
    /// <summary>共有のStageManagerを返す。</summary>
    static StageManager* GetInstance() {
        static StageManager sInstance;
        return &sInstance;
    }

    /// <summary>ステージ一覧を読み、失敗時は内蔵一覧へ戻す。</summary>
    /// <remarks>Developmentの選択履歴も復元する。</remarks>
    void Initialize() {
        // 設定の一覧を読み込めれば、内蔵一覧への切り替えをせず開発時の選択だけ復元する。
        if (LoadFromJson("Resources/json/stage_select/stages.json")) {
            LoadDevelopmentStageIndex();
            return;
        }

        stages_ = StageCatalog::CreateFallback();
        currentStageIndex_ = 0;
        LoadDevelopmentStageIndex();
    }

    /// <summary>ステージ一覧を JSON から読み込みます。失敗時は既存の一覧を維持します。</summary>
    /// <param name="path">ステージ一覧 JSON のパス</param>
    bool LoadFromJson(const std::string& path) {
        std::vector<StageData> loadedStages;
        // 読み込みに失敗した場合は、公開中の一覧や選択を差し替えない。
        if (!StageCatalog::LoadFromJson(path, loadedStages)) {
            return false;
        }

        stages_ = std::move(loadedStages);
        // 一覧の縮小で以前の選択番号が範囲外になった場合は、先頭へ戻してアクセスを有効にする。
        if (currentStageIndex_ < 0 || currentStageIndex_ >= static_cast<std::int32_t>(stages_.size())) {
            currentStageIndex_ = 0;
        }
        return true;
    }

    /// <summary>現在選択中のステージ定義を返す。</summary>
    /// <remarks>Initialize後に呼ぶ。参照は次の一覧再読み込みまで有効。</remarks>
    const StageData& GetCurrentStage() const { return stages_[currentStageIndex_]; }

    /// <summary>有効な配列番号を現在ステージに設定する。</summary>
    /// <remarks>範囲外は無視し、開発用ビルドでは選択をuser_config.jsonへ記録する。</remarks>
    void SetCurrentStage(std::int32_t index) {
        // 指定された番号が現在の一覧を指す場合だけ、選択を更新して開発用の履歴に残す。
        if (index >= 0 && index < static_cast<std::int32_t>(stages_.size())) {
            currentStageIndex_ = index;
            SaveDevelopmentStageIndex();
        }
    }

    /// <summary>現在ステージの配列番号を返す。</summary>
    std::int32_t GetCurrentStageIndex() const { return currentStageIndex_; }

    /// <summary>全ステージ定義を返す。参照は一覧再読み込みまで有効。</summary>
    const std::vector<StageData>& GetStages() const { return stages_; }

    /// <summary>保存データで用いるステージIDから配列番号を探す。</summary>
    /// <returns>未登録のIDなら-1。</returns>
    std::int32_t FindStageIndexById(const std::string& stageId) const {
        for (std::int32_t index = 0; index < static_cast<std::int32_t>(stages_.size()); ++index) {
            // 一覧に含まれる保存IDが一致した行の番号を返す。
            if (stages_[index].id == stageId) {
                return index;
            }
        }
        return -1;
    }

    /// <summary>ステージIDから現在ステージを変更する。</summary>
    /// <returns>IDが見つからなければfalse。選択と設定ファイルは変更しない。</returns>
    bool SetCurrentStageById(const std::string& stageId) {
        const std::int32_t index = FindStageIndexById(stageId);
        // 指定IDに対応する番号がなければ、選択状態や設定ファイルを更新しない。
        if (index < 0) {
            return false;
        }
        SetCurrentStage(index);
        return true;
    }

  private:
    /// <summary>共有インスタンスの初期状態を作る。</summary>
    StageManager() = default;

    /// <summary>共有インスタンスの保持データを破棄する。</summary>
    ~StageManager() = default;

    StageManager(const StageManager&) = delete;

    StageManager& operator=(const StageManager&) = delete;

    /// <summary>開発用ビルドで前回選択したステージの配列位置を復元する。</summary>
    /// <remarks>保存IDと照合しないため、一覧の並び替え後は別ステージを指す可能性がある。</remarks>
    void LoadDevelopmentStageIndex() {
#if defined(USE_IMGUI) && defined(NDEBUG) && !defined(DD)
        // 開発時の前回選択だけを復元し、通常のセーブデータには影響させない。
        std::ifstream file(kUserConfigPath);
        // 設定を開けなければ、一覧をロードした時点の選択を維持する。
        if (!file.is_open()) {
            return;
        }

        try {
            nlohmann::json root;
            file >> root;
            // ユーザー設定がオブジェクトでなければ、配列位置の項目を読み出さない。
            if (!root.is_object()) {
                return;
            }

            std::int32_t savedIndex = root.value("lastStageIndex", currentStageIndex_);
            // 保存された配列位置が現在の一覧に収まる場合だけ、そのステージを選ぶ。
            if (savedIndex >= 0 && savedIndex < static_cast<std::int32_t>(stages_.size())) {
                currentStageIndex_ = savedIndex;
            }
        // 旧設定の構文や型が不正な場合は、現在の選択をそのまま使う。
        } catch (...) {
        }
#endif
    }

    /// <summary>開発用ビルドで現在の配列位置をユーザー設定へ保存する。</summary>
    void SaveDevelopmentStageIndex() const {
#if defined(USE_IMGUI) && defined(NDEBUG) && !defined(DD)
        nlohmann::json values = {{"lastStageIndex", currentStageIndex_}};
        // 有効な位置を指している場合は、参照用のステージIDも番号と一緒に書き出す。
        if (currentStageIndex_ >= 0 && currentStageIndex_ < static_cast<std::int32_t>(stages_.size())) {
            values["lastStageId"] = stages_[currentStageIndex_].id;
        }
        jsonFile::UpdateObject(kUserConfigPath, values);
#endif
    }

    static constexpr const char* kUserConfigPath = "Resources/json/user_config.json";
    std::vector<StageData> stages_;
    std::int32_t currentStageIndex_ = 0;
};
