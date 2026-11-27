#include "ModelManager.h"
#include "ModelAssetPath.h"
#include "ModelCommon.h"
#include <cassert>
#include <filesystem> 
#include <algorithm>
#include <set>
#include "ProfilerManager.h"
#include <chrono>


// 既定のモデル検索設定を保持する。
const std::string ModelManager::kDefaultBaseDirectory = "Resources/3DModel/";
const std::string ModelManager::kDefaultModelExtension = ".obj";
ModelManager* ModelManager::GetInstance() {
    // 関数内 static の寿命管理に任せ、明示的な終了処理ではGPUリソースだけ解放する。
    static ModelManager sInstance;
    return &sInstance;
}

void ModelManager::Initialize(engine::DirectXCommon* dxCommon) {
    // モデル共通描画基盤を初期化し、各Modelが共有するDirectXリソースを準備する。
    assert(dxCommon);
    modelCommon_ = std::make_unique<ModelCommon>();
    modelCommon_->Initialize(dxCommon);
}

void ModelManager::Finalize() {
    // 読み込み済みモデルと共通リソースを、DirectXの終了前に解放する。
    models_.clear(); // 読み込み済みモデルをすべて解放する。
    preparedModels_.clear();
    modelCommon_.reset(); // 共有パイプラインを借用するModelを破棄したあと、ModelCommonを解放します。
}

// モデル名から実ファイルを解決して読み込み、同じキーのモデルは再利用する。


engine::Model* ModelManager::LoadModel(const std::string& modelName) {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        // 要求名でGPUモデルを登録済みなら、同じ参照を返して再ロードを省く。
        if (models_.contains(modelName)) {
            return models_[modelName].get();
        }
    }

    std::filesystem::path path(modelName);
    std::string parentPath = path.parent_path().string();
    std::replace(parentPath.begin(), parentPath.end(), '\\', '/');
    std::string stem = path.stem().string();
    std::string ext = path.extension().string();

    // 拡張子付きで指定された場合でも、拡張子なしキーやフォルダキーの既存登録を優先して探す。
    if (!ext.empty()) {
        std::string extensionlessKey = parentPath.empty() ? stem : parentPath + "/" + stem;
        std::replace(extensionlessKey.begin(), extensionlessKey.end(), '\\', '/');
        const bool isFolderMainModel = !parentPath.empty() &&
            std::filesystem::path(parentPath).filename().string() == stem;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);

            // 拡張子を除いたキーで登録済みなら、その先読みモデルを再利用する。
            if (models_.contains(extensionlessKey)) {
                return models_[extensionlessKey].get();
            }

            // フォルダー代表のファイル名で指定され、フォルダーキーの登録がある場合も再利用する。
            if (isFolderMainModel && models_.contains(parentPath)) {
                return models_[parentPath].get();
            }
        }
    }

    std::string directoryPath;
    std::string fileName;

    // 先読みと同じ順序で実ファイルを探す。未検出時は従来の候補をローダーに渡す。
    if (!ResolveModelPath(modelName, directoryPath, fileName) && ext.empty()) {
        fileName = stem + ".obj";
    }

    std::string fullPath = directoryPath + fileName;
    OutputDebugStringA(("【LoadModel】探索パス: " + fullPath + "\n").c_str());

    auto start = std::chrono::high_resolution_clock::now();

    std::unique_ptr<PreparedModel> preparedModel;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto prepared = preparedModels_.find(modelName);

        // 先読み結果がある場合は、所有権をここへ移してキャッシュから外す。
        if (prepared != preparedModels_.end()) {
            preparedModel = std::move(prepared->second);
            preparedModels_.erase(prepared);
        }
    }

    auto newModel = std::make_unique<engine::Model>();
    ModelCommon* modelCommon = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        modelCommon = modelCommon_.get();
    }

    // 先読みが済んでいればCPU側の解析結果を移し、ここではGPUの作成と画像の登録を行います。
    if (preparedModel) {
        newModel->InitializeFromCpuData(
            modelCommon,
            std::move(preparedModel->data),
            preparedModel->fileName);
    }
    // CPUの先読み結果がなければ、この呼び出しでファイルから初期化する。
    else {
        newModel->Initialize(modelCommon, directoryPath, fileName);
    }

    auto end = std::chrono::high_resolution_clock::now();
    float duration = std::chrono::duration<float, std::milli>(end - start).count();
    ProfilerManager::GetInstance()->RecordLoadTime("Model", modelName, duration);

    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto existingIt = models_.find(modelName);

        // 別スレッドが先に同名モデルを登録していれば、後から作った重複分は使用しない。
        if (existingIt != models_.end()) {
            return existingIt->second.get();
        }

        auto inserted = models_.emplace(modelName, std::move(newModel));
        return inserted.first->second.get();
    }
}

bool ModelManager::PrepareModel(const std::string& modelName) {
    // GPU登録前のモデルを準備し、同名モデルの重複デコードを避ける。
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        // GPUモデルかCPU先読みデータのどちらかが登録済みなら、この準備は完了。
        if (models_.contains(modelName) || preparedModels_.contains(modelName)) {
            return true;
        }
    }

    std::string directoryPath;
    std::string fileName;

    // 要求名から実ファイルを特定できない場合は、CPUの解析を開始できない。
    if (!ResolveModelPath(modelName, directoryPath, fileName)) {
        return false;
    }

    auto prepared = std::make_unique<PreparedModel>();
    prepared->directoryPath = directoryPath;
    prepared->fileName = fileName;
    prepared->data = engine::Model::LoadCpuData(directoryPath, fileName);

    // 通常ロードと同じ色空間でテクスチャを先読みし、後のLoadがデコード済みの画像を見つけられるようにします。
    for (const engine::Model::MaterialData& material : prepared->data.materials) {
        engine::TextureManager::GetInstance()->Prepare(
            material.textureFilePath.empty()
                ? "Resources/sprite/common/white.png"
                : material.textureFilePath,
            engine::TextureManager::TextureColorSpace::kSRGB);
        // 有効な法線画像が指定されている場合だけ、線形データとして先に読み込む。
        if (material.hasNormalMap && !material.normalMapPath.empty()) {
            engine::TextureManager::GetInstance()->Prepare(
                material.normalMapPath,
                engine::TextureManager::TextureColorSpace::kLinear);
        }

        // 有効なORM画像が指定されている場合だけ、材質係数のデータとして先に読み込む。
        if (material.hasOrmMap && !material.ormMapPath.empty()) {
            engine::TextureManager::GetInstance()->Prepare(
                material.ormMapPath,
                engine::TextureManager::TextureColorSpace::kLinear);
        }
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // 解析中に別の処理で同じモデルが登録済みになった場合は、後から結果を上書きしない。
    if (models_.contains(modelName) || preparedModels_.contains(modelName)) {
        return true;
    }

    preparedModels_.emplace(modelName, std::move(prepared));
    return true;
}

bool ModelManager::ResolveModelPath(
    const std::string& modelName, std::string& directoryPath, std::string& fileName) const {
    // 配布時の依存解析でも同じ探索順を使い、同名の別形式を誤って選ばないようにします。
    std::filesystem::path directory, file;
    const bool found = engine::ResolveModelAssetPath(kDefaultBaseDirectory, modelName, directory, file);
    directoryPath = directory.generic_string() + "/";
    fileName = file.string();
    return found;
}

bool ModelManager::ReloadModel(const std::string& modelName) {
    // 指定モデルをキャッシュから外して再読み込みし、ファイル更新を反映する。
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        models_.erase(modelName);
    }

    return LoadModel(modelName) != nullptr;
}

std::vector<std::string> ModelManager::GetLoadedModelNames() const {
    // 現在読み込み済みのモデルキー一覧を返し、エディタやデバッグ表示で使えるようにする。
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    std::vector<std::string> names;

    for (const auto& pair : models_) {
        names.push_back(pair.first);
    }

    return names;
}

std::vector<std::string> ModelManager::GetAvailableModelNames() const {
    // 読込済みモデルだけでなく、ディスク上の未読込モデルも選択肢へ加える。
    std::vector<std::string> names = GetLoadedModelNames();
    std::set<std::string> uniqueNames(names.begin(), names.end());

    std::error_code error;

    // 基準フォルダーが無いか確認に失敗した場合も、読み込み済みの名前だけは返す。
    if (!std::filesystem::exists(kDefaultBaseDirectory, error) || error) {
        return names;
    }

    for (std::filesystem::recursive_directory_iterator iterator(
             kDefaultBaseDirectory,
             std::filesystem::directory_options::skip_permission_denied,
             error), end;
         iterator != end;
         iterator.increment(error)) {
        // アクセス不能ファイルは飛ばし、走査できる他のモデルを候補へ残す。
        if (error) {
            error.clear();
            continue;
        }

        // 通常ファイルとして取得できない項目は、モデル候補に含めない。
        if (!iterator->is_regular_file(error) || error) {
            error.clear();
            continue;
        }

        const std::string extension = iterator->path().extension().string();

        // OBJ、GLTF、GLBのファイルだけからモデルキーを集める。
        if (extension != ".obj" && extension != ".gltf" && extension != ".glb") {
            continue;
        }

        std::filesystem::path relativePath = std::filesystem::relative(
            iterator->path().parent_path(),
            kDefaultBaseDirectory,
            error);
        // フォルダーまでの相対パスを得られない項目は、登録キーに変換できないので飛ばす。
        if (error || relativePath.empty()) {
            error.clear();
            continue;
        }

        std::string modelName = relativePath.generic_string();

        // 同じフォルダーに複数のモデルがあっても、一覧へ入れるフォルダーキーは一度だけにする。
        if (uniqueNames.insert(modelName).second) {
            names.push_back(std::move(modelName));
        }
    }

    std::sort(names.begin(), names.end());
    return names;
}

#include <Windows.h> // OutputDebugStringAで読み込みログを出力するために使用。

// Resources/3DModel配下を走査し、見つかったモデルファイルを事前にまとめて読み込む。
void ModelManager::LoadAllModels() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // 既定フォルダーが存在しない場合は、全件のロードを始めない。
    if (!std::filesystem::exists(kDefaultBaseDirectory))
        return;

    OutputDebugStringA("=== 【ModelManager】事前ロード開始 ===\n");

    for (const auto& entry : std::filesystem::recursive_directory_iterator(kDefaultBaseDirectory)) {
        // フォルダーなどの非ファイル項目は、ローダーへ渡さない。
        if (entry.is_regular_file()) {
            std::string ext = entry.path().extension().string();

            // 対応する三つの拡張子の場合だけ、モデルの読み込み先と登録キーを組み立てる。
            if (ext == ".obj" || ext == ".gltf" || ext == ".glb") {

                // 解析に使うフォルダーとファイル名は、走査結果の実パスから取る。
                std::string dirPath = entry.path().parent_path().string() + "/";
                std::string fileName = entry.path().filename().string();

                // 事前ロード時はモデルファイル名ではなく、モデルフォルダまでの相対パスを登録キーにする。
                std::filesystem::path relPath = std::filesystem::relative(entry.path(), kDefaultBaseDirectory);
                std::string keyName = relPath.parent_path().string(); // フォルダまでのパスを取得
                std::replace(keyName.begin(), keyName.end(), '\\', '/'); // Windowsの「\」を「/」に統一

                // 同じフォルダキーを二重登録しないようにし、最初に見つかった代表モデルだけを読み込む。
                if (!models_.contains(keyName)) {
                    auto start = std::chrono::high_resolution_clock::now();

                    auto newModel = std::make_unique<engine::Model>();
                    newModel->Initialize(modelCommon_.get(), dirPath, fileName);
                    models_[keyName] = std::move(newModel);

                    auto end = std::chrono::high_resolution_clock::now();
                    float duration = std::chrono::duration<float, std::milli>(end - start).count();
                    ProfilerManager::GetInstance()->RecordLoadTime("Model", keyName, duration);

                    std::string logMsg = "[事前ロード成功] キー: [" + keyName + "] パス: " + dirPath + fileName + "\n";
                    OutputDebugStringA(logMsg.c_str());
                }
            }
        }
    }

    OutputDebugStringA("=== 【ModelManager】事前ロード終了 ===\n");
}
