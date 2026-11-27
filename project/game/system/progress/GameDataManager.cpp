#include "game/system/progress/GameDataManager.h"

bool GameDataManager::Save() {
    // 置換が済むまで未保存扱いにし、途中で失敗しても次回の保存対象に残す。
    dirty_ = true;
    nlohmann::json data;
    const std::filesystem::path savePath(GetSaveFilePath());
    const std::filesystem::path tempPath = savePath.wstring() + L".tmp";
    const std::filesystem::path backupPath = savePath.wstring() + L".bak";
    std::error_code ec;
    const bool hasPrimary = std::filesystem::exists(savePath, ec);
    // 本体の有無を確認する段階でファイルシステムのエラーがあれば、作成や置換を試さない。
    if (ec)
        return false;

    // 本体が既にあり、どの読込元からも既存JSONを回収できない場合は、復旧用に本体を保つ。
    if (!ReadSaveJson(activeSlot_, data) && hasPrimary)
        return false;
    // 有効なJSONオブジェクトを読めなかった新規保存などでは、進行項目を書ける空のオブジェクトを使う。
    if (!data.is_object())
        data = nlohmann::json::object();

    // 読めたJSONを土台にし、このクラスが管理する進行状況だけを書き換える。
    data["lives"] = lives_;
    data["coins"] = coins_;
    data["playTimeSeconds"] = playTimeSeconds_;
    data["clearedStages"] = clearedStages_; // クリア済みステージのインデックス。
    data["seenUnlockedStages"] = seenUnlockedStages_;

    // スターコイン取得状況を保存する
    nlohmann::json coinData;
    for (auto& [stageIdx, coins] : stageStarCoins_) {
        coinData[std::to_string(stageIdx)] = coins;
    }
    data["starCoins"] = coinData;

    try {
        std::filesystem::create_directories(savePath.parent_path());

        // 同じディレクトリの一時ファイルへ書き、完了した内容だけ本体へ置換する。
        std::ofstream outFile(tempPath, std::ios::binary | std::ios::trunc);
        // 一時ファイルを開けなければ、既存の本体を触る前に失敗を返す。
        if (!outFile)
            return false;
        outFile << data.dump(4);
        outFile.flush();
        // 一時ファイルへの書き込みやフラッシュが失敗した場合は、本体の置換へ進まない。
        if (!outFile.good())
            return false;
        outFile.close();
        // ファイルを閉じるときに書き込みエラーが出た場合も、本体は置き換えない。
        if (outFile.fail())
            return false;
        // 一時ファイルを閉じた後、Windows側の書き込みも確定させてから本体を触る。
        HANDLE tempHandle = ::CreateFileW(tempPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        // OSの書き込みを確定するためのハンドルを取得できなければ、置換を中止する。
        if (tempHandle == INVALID_HANDLE_VALUE)
            return false;
        const bool flushed = ::FlushFileBuffers(tempHandle) != 0;
        ::CloseHandle(tempHandle);
        // OS側の書き込み確定に失敗したら、正常なセーブとして本体へ移さない。
        if (!flushed)
            return false;

        nlohmann::json primary;
        // 本体が存在し、正常なJSONとして読める場合だけ、直前の世代をバックアップへコピーする。
        if (hasPrimary && ReadJsonFile(savePath, primary)) {
            std::filesystem::copy_file(savePath, backupPath, std::filesystem::copy_options::overwrite_existing, ec);
            // 有効な本体をバックアップに保存できなければ、直前の世代を失わないよう本体の置換を止める。
            if (ec)
                return false;
        }
        // バックアップの準備を終えた後で本体を置き換える。未保存フラグを下げるのは成功時だけ。
        if (!::MoveFileExW(tempPath.c_str(), savePath.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return false;
        }
        dirty_ = false;
        saveDelaySeconds_ = 0.0f;
        return true;
    // ディレクトリ準備やJSONの書き出しで例外が起きた場合も、未保存フラグを残して失敗にする。
    } catch (const std::exception&) {
        return false;
    }
}

void GameDataManager::Load() {
    // 以前のスロットから持ち越す値を消し、読込失敗時も新規ゲームの状態へ戻す。
    dirty_ = false;
    saveDelaySeconds_ = 0.0f;
    playTimeRemainder_ = 0.0f;
    nlohmann::json data;
    // 本体・バックアップなどの読込に失敗した場合は、スロットを初期状態の値に揃える。
    if (!ReadSaveJson(activeSlot_, data)) {
        lives_ = kStartingLives;
        coins_ = 0;
        playTimeSeconds_ = 0;
        clearedStages_.clear();
        seenUnlockedStages_.clear();
        stageStarCoins_.clear();
        return;
    }

    // 前に開いていたスロットの値が残らないよう、進行データを初期化してから保存値を読み込む。
    lives_ = kStartingLives;
    coins_ = 0;
    playTimeSeconds_ = 0;
    clearedStages_.clear();
    seenUnlockedStages_.clear();
    stageStarCoins_.clear();

    lives_ = std::clamp(ReadInteger(data, "lives", kStartingLives), 0, kMaxLives);

    // 個別項目の破損は既定値へ戻し、読める進行データまで捨てない。
    coins_ = std::clamp(ReadInteger(data, "coins", 0), 0, kMaxCoins);
    playTimeSeconds_ = (std::max)(0, ReadInteger(data, "playTimeSeconds", 0));

    // 配列にある値のうち整数として読めたステージ番号だけを、進行状況に戻す。
    auto readStageIndices = [&](const char* key, std::vector<std::int32_t>& output) {
        // 指定されたキーがないか配列でない場合は、そのステージ一覧の読み込みを省く。
        if (!data.contains(key) || !data[key].is_array()) {
            return;
        }
        for (const auto& value : data[key]) {
            std::int32_t stageIndex = 0;
            // 項目を整数のステージ番号として読めたときだけ、読み込み結果へ入れる。
            if (TryReadInteger(value, stageIndex)) {
                output.push_back(stageIndex);
            }
        }
    };
    readStageIndices("clearedStages", clearedStages_);
    readStageIndices("seenUnlockedStages", seenUnlockedStages_);

    // スターコインのステージ別項目は、保存値がJSONオブジェクトの場合だけ読む。
    if (data.contains("starCoins") && data["starCoins"].is_object()) {
        // ステージ番号と各コインの真偽を検証し、壊れた項目だけ読み飛ばす。
        for (auto it = data["starCoins"].begin(); it != data["starCoins"].end(); ++it) {
            std::int32_t stageIdx = 0;
            const std::string& key = it.key();
            const auto result = std::from_chars(key.data(), key.data() + key.size(), stageIdx);
            // キー全体をステージ番号にできない場合や、コインの値が配列でない項目は飛ばす。
            if (result.ec != std::errc{} || result.ptr != key.data() + key.size() || !it.value().is_array()) {
                continue;
            }
            std::vector<bool> starCoins(kStarCoinsPerStage, false);
            for (size_t index = 0; index < starCoins.size() && index < it.value().size(); ++index) {
                // 真偽値として読めたコインだけ、既定の未取得状態から書き換える。
                if (it.value()[index].is_boolean()) {
                    starCoins[index] = it.value()[index].get<bool>();
                }
            }
            stageStarCoins_[stageIdx] = std::move(starCoins);
        }
    }
}

bool GameDataManager::ReadSaveJson(std::int32_t slotIndex, nlohmann::json& outData) const {
    const std::string path = GetSaveFilePath(slotIndex);
    // 現在のスロット本体が有効なら、ほかの世代を使わずその内容を返す。
    if (ReadJsonFile(path, outData))
        return true;
    // 本体を読めない場合でも、バックアップを読めたらその内容を復元に使う。
    if (ReadJsonFile(path + ".bak", outData))
        return true;
    // 旧形式の単一ファイルは、最初のスロットを読み込む場合だけ候補にする。
    return slotIndex == 0 && ReadJsonFile(GetLegacySaveFilePath(), outData);
}
