#include "engine/utility/JsonFile.h"
#include "json.hpp"

#include <fstream>
#include <string>
#include <system_error>

bool jsonFile::UpdateObject(const std::filesystem::path& path, const nlohmann::json& values,
    std::initializer_list<const char*> removedKeys) {
    // キー単位の更新はオブジェクト同士に限ります。条件を満たさない場合はファイルを開く前に終了します。
    if (!values.is_object()) {
        return false;
    }

    nlohmann::json root = nlohmann::json::object();
    {
        std::ifstream input(path);
        // 既存ファイルを開けた場合は、指定外のキーを引き継ぐために中身を読み込む。
        if (input.is_open()) {
            try {
                input >> root;
            } catch (const nlohmann::json::parse_error&) {
                // 構文を読めない既存内容は空のオブジェクトとして組み立て直します。
                root = nlohmann::json::object();
            }
            // 低レベルの読み取りエラーが発生した場合は、元ファイルを書き換えず失敗を返す。
            if (input.bad()) {
                return false;
            }
        // 入力を開けない場合は、ファイルが存在するか確認して新規作成と読み取り失敗を区別する。
        } else {
            std::error_code error;
            const bool exists = std::filesystem::exists(path, error);
            // 既存ファイルを開けなかった場合や存在確認に失敗した場合は、上書きを防ぐため終了する。
            if (exists || error) {
                return false;
            }
        }
    }

    // 読めたJSONがオブジェクト以外なら、キー単位で更新できる空のオブジェクトへ置き換える。
    if (!root.is_object()) {
        root = nlohmann::json::object();
    }
    // 指定キーだけを更新して他のキーを保持します。削除指定を後に処理するため、両方にあるキーは削除されます。
    root.update(values);
    for (const char* key : removedKeys) {
        root.erase(key);
    }

    // 文字列の不正などによる変換失敗は、ファイルを開く前に検出する。
    std::string text;
    try {
        text = root.dump(4);
    } catch (const nlohmann::json::exception&) {
        return false;
    }

    std::error_code error;
    // 保存パスに親ディレクトリが含まれる場合は、出力を開く前にその場所を作る。
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        // 親ディレクトリを作成できなければ、ファイルを開く処理へ進まない。
        if (error) {
            return false;
        }
    }
    std::ofstream output(path);
    // 出力ファイルを開けなければ、書き込みを行わず失敗を返す。
    if (!output.is_open()) {
        return false;
    }
    output << text;
    // 書き込み後のフラッシュとクローズも済ませてからストリーム状態を返します。
    output.flush();
    output.close();
    return output.good();
}
