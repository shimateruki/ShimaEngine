#include <cstdint>
#include "engine/editor/assets/JsonAssetBrowser.h"
#include "engine/editor/scene/inspector/InspectorWindow.h"
#include "EditorPropertyRegistry.h"

#include "DebugEditor.h"
#include "EditorManager.h"
#include "GhostRecorder.h"
#include "IconsFontAwesome5.h"
#include "Object3D.h"
#include "engine/editor/properties/InspectorExtensionRegistry.h"
#include "imgui.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#ifdef USE_IMGUI

// 選択中のオブジェクトに経路を割り当て、再生条件を変えた場合は必要に応じて試写を始め直します。
void InspectorWindow::DrawPathMoverSection(engine::Object3D *selectedObject) {
  // 選択先が無ければ、経路設定も表示しない。
  if (!selectedObject) {
    return;
  }

  ImGui::Separator();
  ImGui::Text(ICON_FA_GHOST " 【パス移動 (GhostRecorder)】");
  // レコーダーが使えて編集ボタンが押された場合は、そのレコーダーに選択中のObjectを渡す。
  if (editor_->GetGhostRecorder() && ImGui::Button("このObjectの動きを作る##GhostRecorder")) {
    editor_->GetGhostRecorder()->SetTarget(selectedObject);
    EditorManager::GetInstance()->SetSelectedObject(editor_->GetGhostRecorder());
  }

  // 乗車タイムラインと旧Pathの排他をComponentの状態で判定し、具体的な型名を持ちません。
  const auto* ridePath = selectedObject->GetPathMoverComponent();
  // 専用の乗車タイムラインが指定済みなら、旧パスの二重設定を避けるためここで終了する。
  if (ridePath && !ridePath->GetRideSequence().empty()) {
    ImGui::TextDisabled("乗車タイムラインを使用中です。上のクラス設定から変更できます。");
    return;
  }

  std::string currentRecordPreview = selectedObject->GetRecordPathName().empty()
                                         ? "(なし)"
                                         : selectedObject->GetRecordPathName();

  // パス選択欄を開いたときだけ、候補と現在の指定を描画する。
  if (ImGui::BeginCombo("パスデータ", currentRecordPreview.c_str())) {
    bool isNoneSelected = selectedObject->GetRecordPathName().empty();

    // 割り当てパスを空にして以後の読み込みを止めるだけでなく、現在の再生も終了します。
    if (ImGui::Selectable("(なし)", isNoneSelected)) {
      selectedObject->SetRecordPathName("");
      // 再生器が存在する場合は、パスの割当解除と同時に現在の再生を止める。
      if (selectedObject->GetRecorder())
        selectedObject->GetRecorder()->Stop();
    }

    // パス未指定なら、選択欄を開いたときのフォーカスを「なし」に置く。
    if (isNoneSelected)
      ImGui::SetItemDefaultFocus();

    std::string dirPath = "Resources/json/animation/";

    // パス素材フォルダーが存在する場合だけ、編集用の候補を列挙する。
    if (fs::exists(dirPath) && fs::is_directory(dirPath)) {
      for (const auto& fileName : JsonAssetBrowser::ListKeys("Resources/json/animation")) {
          bool isSelected = (selectedObject->GetRecordPathName() == fileName);

          // 候補を選んだときに、その名前をこのObjectのパスへ設定する。
          if (ImGui::Selectable(fileName.c_str(), isSelected)) {
            selectedObject->SetRecordPathName(fileName);

            // 再生器を取得できた場合は、カメラ用かに応じた設定で選択したパスを再生する。
            if (selectedObject->GetRecorder()) {
              bool isCinematic = selectedObject->IsCameraObject();
              selectedObject->GetRecorder()->Play(
                  selectedObject->GetRecordPathName(),
                  selectedObject->IsRecordLoop(),
                  selectedObject->IsRecordRelative(), isCinematic);
            }
          }

          // 現在割り当てているパスの行に、選択欄の初期フォーカスを置く。
          if (isSelected)
            ImGui::SetItemDefaultFocus();
      }
    }

    ImGui::EndCombo();
  }

  auto* pathSettings = selectedObject->EnsurePathMoverComponent();
  // 乗車判定を使う再生条件は、カメラ以外のObjectだけで編集する。
  if (!selectedObject->IsCameraObject()) {
    std::int32_t playbackMode = static_cast<std::int32_t>(pathSettings->GetPlaybackMode());
    const char* modes[] = {"常時再生", "乗っている間再生（離れるとその場で保持）", "乗ると終点まで再生（離れても継続）"};
    // 新しい再生条件が選ばれた場合だけ、コンポーネントへ反映する。
    if (ImGui::Combo("再生条件", &playbackMode, modes, IM_ARRAYSIZE(modes))) {
      pathSettings->SetPlaybackMode(static_cast<PathMoverComponent::PlaybackMode>(playbackMode));
      // 再設定時は古い開始ラッチを破棄する。相対基準の累積を避けてから再開する。
      if (selectedObject->GetRecorder() && !selectedObject->GetRecordPathName().empty()) {
        selectedObject->GetRecorder()->Stop();
        selectedObject->GetRecorder()->Play(selectedObject->GetRecordPathName(), selectedObject->IsRecordLoop(),
                                       selectedObject->IsRecordRelative(), false);
      }
    }
    ImGui::TextWrapped("乗車条件は地面Colliderへの接地で判定します。横からの接触や敵の乗車では開始しません。");
    // 一度乗ると終点まで進む設定では、ループ指定より優先される停止条件を補足する。
    if (pathSettings->GetPlaybackMode() == PathMoverComponent::PlaybackMode::kRideToEnd)
      ImGui::TextWrapped("終点まで1回再生して停止します（ループ指定より優先）。再実行はテスト再生またはシーン再開始で行います。");
  }

  bool pathLoop = selectedObject->IsRecordLoop();

  // ループ指定を変更した場合だけ、設定と試写の再生条件を切り替える。
  if (ImGui::Checkbox("ループ再生##Record", &pathLoop)) {
    selectedObject->SetRecordLoop(pathLoop);
    // 再生器と割当済みパスがあるときだけ、新しいループ条件で再生を始め直す。
    if (selectedObject->GetRecorder() &&
        !selectedObject->GetRecordPathName().empty()) {
      bool isCinematic = selectedObject->IsCameraObject();
      selectedObject->GetRecorder()->Play(
          selectedObject->GetRecordPathName(), selectedObject->IsRecordLoop(),
          selectedObject->IsRecordRelative(), isCinematic);
    }
  }

  bool pathRelative = selectedObject->IsRecordRelative();

  // 相対座標の指定を変更したら、移動の基準を切り替える。
  if (ImGui::Checkbox("配置位置を基準に移動（固定座標を解除）##Record", &pathRelative)) {
    selectedObject->SetRecordRelative(pathRelative);
    // 現在のパスを再生できる場合だけ、新しい座標基準で始め直す。
    if (selectedObject->GetRecorder() &&
        !selectedObject->GetRecordPathName().empty()) {
      bool isCinematic = selectedObject->IsCameraObject();
      selectedObject->GetRecorder()->Play(
          selectedObject->GetRecordPathName(), selectedObject->IsRecordLoop(),
          selectedObject->IsRecordRelative(), isCinematic);
    }
  }

  ImGui::TextDisabled("OFF: パスJSONの固定座標 / ON: Objectの配置位置 + パス内の移動量");

  // 開始位相は秒ではなくループ全体に対する割合です。変更時は再生を始め直して適用します。
  float phaseOffset = pathSettings->GetPhaseOffset();
  // 開始位相をドラッグして変更した場合だけ、値を保存する。
  if (ImGui::SliderFloat("開始タイミング##Record", &phaseOffset, 0.0f, 0.99f, "周期の %.2f")) {
    pathSettings->SetPhaseOffset(phaseOffset);
    // 再生器と割当済みパスがそろっていれば、変更した位相を再生の開始時点に反映する。
    if (selectedObject->GetRecorder() && !selectedObject->GetRecordPathName().empty()) {
      selectedObject->GetRecorder()->Play(selectedObject->GetRecordPathName(), selectedObject->IsRecordLoop(),
                                      selectedObject->IsRecordRelative(), selectedObject->IsCameraObject());
    }
  }

  // テスト再生ボタンの入力を受けた場合にだけ、この経路の再生を要求する。
  if (ImGui::Button("テスト再生##Record")) {
    // 試写には、有効な再生器と割当済みのパスの両方が必要。
    if (selectedObject->GetRecorder() &&
        !selectedObject->GetRecordPathName().empty()) {
      bool isCinematic = selectedObject->IsCameraObject();
      selectedObject->GetRecorder()->Play(
          selectedObject->GetRecordPathName(), selectedObject->IsRecordLoop(),
          selectedObject->IsRecordRelative(), isCinematic);
    }
  }
}

void InspectorWindow::DrawGameplayDataSection(engine::Object3D *selectedObject) {
  // 選択物のイベント、生成種別、型固有パラメータを同じInspectorへ表示します。
  // Scene JSONに保存される値を直接編集するため、型変更時だけ必要な初期値もここで補います。
  if (!selectedObject) {
    return;
  }

  // 専用UIの対象型と処理はゲーム側で登録する。既存の表示位置・代表選択・無効化範囲を保つ。
  using InspectorExtensions = engine::InspectorExtensionRegistry;
  InspectorExtensions::Draw(InspectorExtensions::Slot::kGameplay, selectedObject);

  ImGui::Separator();
  // ゲームデータ欄が開いているときだけ、クラスや個別設定を描く。
  if (ImGui::CollapsingHeader(ICON_FA_GAMEPAD " ゲームデータ (Stats)",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
                       "--- Object Type Settings ---");
    const char *classItems[] = {"Model",   "Spawner", "Player",       "Enemy",
                                "Gimmick", "InvisibleBox", "Block"};
    std::string currentClass = selectedObject->GetClassName();
    std::int32_t currentClassIndex = 0;

    for (std::int32_t i = 0; i < IM_ARRAYSIZE(classItems); i++) {
      // 現在のクラスが候補の一覧にあれば、その番号を最初の選択にする。
      if (currentClass == classItems[i]) {
        currentClassIndex = i;
        break;
      }
    }

    // 一時ドロップ星はゲームが生成するため、配置用の型へ切り替えません。
    if (currentClass == "Item") {
      ImGui::TextUnformatted("Class Type: Item（実行時生成）");
    // Item以外でクラスが変更されたときに、保存用のクラス名を設定する。
    } else if (ImGui::Combo(ICON_FA_CUBES " Class Type", &currentClassIndex,
                     classItems, IM_ARRAYSIZE(classItems))) {
      selectedObject->SetClassName(classItems[currentClassIndex]);

      // Spawnerへ変更した場合だけ、名前と設定の不足を補う。
      if (std::string(classItems[currentClassIndex]) == "Spawner") {
        // 名前にObjectが含まれる場合は、Spawnerの既定名へ変更する。
        if (selectedObject->GetName().find("Object") != std::string::npos)
          selectedObject->SetName("Spawner_New");
        // パラメータが未設定の場合だけ、Spawnerの編集に必要な既定値を作る。
        if (!selectedObject->GetParameters().has_value())
          selectedObject->ResetParameters();
      }
    }

    // Spawnerの場合は、生成対象と出現の設定を表示する。
    if (selectedObject->GetClassName() == "Spawner")
      DrawSpawnerSettings();

    ImGui::Spacing();

    // Enemyの場合は、使う敵種の選択欄を表示する。
    if (selectedObject->GetClassName() == "Enemy") {
      ImGui::Indent();
      DrawEnemyTypeSelector();
      ImGui::Unindent();
    }

    // Gimmickの場合は、ギミック種別の選択欄を表示する。
    if (selectedObject->GetClassName() == "Gimmick") {
      ImGui::Indent();
      DrawGimmickTypeSelector();
      ImGui::Unindent();
    }

    // Itemの場合は、ゲーム側が登録した専用の編集欄へ委譲する。
    if (selectedObject->GetClassName() == "Item") {
      ImGui::Indent();
      InspectorExtensions::Draw(InspectorExtensions::Slot::kItem, selectedObject);
      ImGui::Unindent();
    }

    // Player/Enemyの共通ステータスは専用管理器が正本です。この表からは変更させず、
    // 配置ごとの例外設定は各クラスのProperty欄で編集します。
    if (selectedObject->GetClassName() == "Enemy" ||
        selectedObject->GetClassName() == "Player") {
      ImGui::SeparatorText("キャラクター・ステータス（表示のみ）");
      ImGui::TextDisabled("共通値の編集は下の「この種類の基本ステータスを調整」から行います。現在HPだけは"
                          "実行中に変動します。");
      // キャラクターの実行時パラメータがそろっている場合は、現在値を読み取る。
      if (selectedObject->GetParameters().has_value()) {
        const auto p = selectedObject->GetParameters().value();
        // 表を開始できた場合だけ、HPや移動値を読み取り専用で表示する。
        if (ImGui::BeginTable("ManagedCharacterStatus", 2,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
          auto drawValue = [](const char *name, const char *format,
                              float value) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(name);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text(format, value);
          };

          drawValue("現在HP", "%.1f", p.hp);
          drawValue("最大HP", "%.1f", p.maxHp);
          drawValue("攻撃力倍率", "%.2f", p.attackPower);
          drawValue("移動速度", "%.2f", p.speed);
          drawValue("重力", "%.2f", p.gravity);
          drawValue("最大落下速度", "%.2f", p.maxFallSpeed);
          drawValue("ジャンプ力", "%.2f", p.jumpPower);
          drawValue("感知範囲", "%.2f", p.detectionRange);
          ImGui::EndTable();
        }
      // パラメータ未設定なら、次の生成や読み込みで設定される旨を表示する。
      } else {
        ImGui::TextDisabled(
            "ステータスは次回の生成・ロード時に自動設定されます。");
      }



    } else if (selectedObject->GetClassName() == "Gimmick") {
      // 種類別の入力欄はクラス側の登録から共通Component欄へ自動生成します。
      ImGui::TextDisabled("固有設定は上のクラス名付きComponent欄で編集できます。");
    }
  }
}

#else

void InspectorWindow::DrawPathMoverSection(engine::Object3D *) {}
void InspectorWindow::DrawGameplayDataSection(engine::Object3D *) {}

#endif
