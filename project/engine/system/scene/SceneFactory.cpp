#include "SceneFactory.h"
#include <utility>

std::unique_ptr<BaseScene> SceneFactory::CreateScene(const std::string& sceneName) {
    // 未登録キーを別のシーンへ置き換えず、呼び出し側に失敗を返す。
    const auto found = creators_.find(sceneName);

    // シーン名が未登録か生成関数が空の場合は、生成できるシーンがないことを返す。
    if (found == creators_.end() || !found->second) {
        return nullptr;
    }

    return found->second();
}

std::vector<std::string> SceneFactory::GetRegisteredSceneNames() const {
    // エディター一覧は登録順を使い、マップの内部順序には依存しない。
    return registrationOrder_;
}

// 同名登録は生成関数だけ差し替え、一覧上の位置は変えません。
bool SceneFactory::RegisterScene(const std::string& sceneName, SceneCreator creator) {
    // 名前が空、または生成関数が空の要求は登録しない。
    if (sceneName.empty() || !creator) {
        return false;
    }

    const bool isNewRegistration = creators_.find(sceneName) == creators_.end();
    creators_[sceneName] = std::move(creator);

    // 初めて登録する名前だけ、エディターの表示に使う登録順一覧へ加える。
    if (isNewRegistration) {
        registrationOrder_.push_back(sceneName);
    }

    return true;
}
