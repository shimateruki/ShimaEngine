#include "game/system/scene/ItemFactory.h"

ItemFactory* ItemFactory::GetInstance() {
    // アイテム生成を依頼する側で同じFactoryを共有する。
    static ItemFactory sInstance;
    return &sInstance;
}

// 廃止済み種別は生成せず、それ以外の旧アイテムデータは共通型で読み込みます。
std::unique_ptr<BaseItem> ItemFactory::CreateItem(const std::string& itemName, engine::Object3DCommon* common) {
    // 廃止済みの回復アイテムや種別未設定のデータは、別アイテムへ置き換えず生成しません。
    if (itemName.empty() || itemName == "Heal") {
        return nullptr;
    }

    // 現行の生成対象は共通アイテム型で初期化し、仮表示用の球モデルを用意する。
    auto newItem = std::make_unique<BaseItem>();
    newItem->Initialize(common, "Primitives/sphere");

    // イベントとEditorのどちらからも判定できるよう、本体とパラメーターに同じ種別名を登録する。
    newItem->SetItemType(itemName);

    // 個別のパラメーター領域がまだなければ、種別を書き込む前に用意する。
    if (!newItem->GetParameters().has_value()) {
        newItem->ResetParameters();
    }

    newItem->SetParameter(&engine::EntityParameter::itemType, itemName);

    return newItem;
}
