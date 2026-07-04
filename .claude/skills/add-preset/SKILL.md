---
name: add-preset
description: Add or change factory presets on an existing plugin in this repo (FactoryPresets.h table, ProgramAdapter wiring, PresetSelector, preset wiring test, version bump). Use when adding/editing a plugin's built-in presets — contains the end-to-end pattern so you don't need to read other plugins as reference.
---

# 既存プラグインへのファクトリープリセット追加

プリセットは JUCE program API に相乗りし、ホストのプログラムリスト(VST3/AU
factory presets)とエディタの `PresetSelector` の両方から選べる。変更箇所は 5 つ:
`FactoryPresets.h` → Processor 配線 → Editor セレクタ → `preset_test.cpp` →
version bump。設計背景は `docs/plans/factory-presets.md`(D1〜D6)。

**プリセットの値・名前は "taste" = Ask a human #1。** 値はドラフトとして書き、
`FactoryPresets.h` 冒頭に「試聴サインオフ待ち」と明記する。マージ/リリース前の
Standalone 試聴サインオフが前提(配線と検証だけを自動で完結させる)。

## 共有基盤(読むのはこれで十分)

- `presets/include/factory_presets/PresetBank.h` — JUCE 非依存の constexpr 型
  `PresetParam{ paramID, value }` / `Preset{ name, params, numParams }` /
  `PresetBank{ presets, numPresets }`。`value` は**実値(正規化前)**。
- `presets/include/factory_presets/ProgramAdapter.h` — program API への委譲。
  `configure()`(message thread)で「プログラム×パラメータ」の正規化ターゲット表を
  事前計算し、`setCurrentProgram()` は atomic index + `setValueNotifyingHost` のみ
  (**RT 安全**: 割当・ロック・ValueTree 操作なし)。program 0 = "Init" = 全
  パラメータをデフォルトへ。
- `ui/include/factory_ui/PresetSelector.h` — ComboBox + 前/次矢印(配色は
  `FactoryLookAndFeel` パレットのみ)。

## 1. Source/FactoryPresets.h(プリセットテーブル + 除外リスト)

```cpp
#pragma once
#include "factory_presets/PresetBank.h"
// DRAFT VALUES — 試聴サインオフ待ち(CLAUDE.md "Ask a human" #1)。配線は
// preset_test.cpp が検証。program 0 "Init" は ProgramAdapter が合成するので列挙しない。
namespace <slug>_presets
{
    inline constexpr const char* kExclude[] = { "bypass" /*, monitoring...*/ };
    inline constexpr int kNumExclude = (int) (sizeof (kExclude) / sizeof (kExclude[0]));

    inline constexpr factory_presets::PresetParam kTapeWarmth[] = {
        { "drive", 12.0f }, { "mix", 100.0f }   // choice/bool は index/0-1 で
    };
    inline constexpr factory_presets::Preset kPresets[] = {
        { "Tape Warmth", kTapeWarmth, (int) (sizeof (kTapeWarmth) / sizeof (kTapeWarmth[0])) },
    };
    inline constexpr factory_presets::PresetBank bank {
        kPresets, (int) (sizeof (kPresets) / sizeof (kPresets[0])) };
}
```

**除外リスト(hard rule, D4)** — プリセットが触ってはいけない ID:
- `bypass` は**全機種で除外**(プリセットで音を止めない)。
- モニタ系(`delta` の delta-listen、dynamic-eq の帯域 `lsn` listen、shimmer の
  `freeze`)。ProgramAdapter は**完全一致**除外なので、帯域ループ生成の ID は
  全帯域分を明示列挙する(例 dynamic-eq: `b0_lsn`..`bN_lsn`)。
- **ユーザーのロード状態に依存する機能 param**(nam-player の `ir_enable` /
  `ir_level` / `slotN_*` ルーティング等)。プリセットはユーザーが組んだ構成を戻さない。

値は各 `createParameterLayout` の**実レンジ内**。choice は選択肢 index、bool は 0/1。
レンジ外や存在しない ID は `preset_test` が fail にする。

## 2. Source/PluginProcessor.{h,cpp}(program API 委譲 + presetIndex)

```cpp
// ヘッダ
#include "factory_presets/ProgramAdapter.h"
#include "FactoryPresets.h"
factory_presets::ProgramAdapter programs;               // メンバ

int  getNumPrograms() override                { return programs.getNumPrograms(); }
int  getCurrentProgram() override             { return programs.getCurrentProgram(); }
void setCurrentProgram (int i) override       { programs.setCurrentProgram (i); }
const juce::String getProgramName (int i) override { return programs.getProgramName (i); }
void changeProgramName (int, const juce::String&) override {}   // 不変

// ctor 末尾(APVTS 構築後)
programs.configure (apvts, <slug>_presets::bank,
                    <slug>_presets::kExclude, <slug>_presets::kNumExclude);
```

`getStateInformation` / `setStateInformation` に presetIndex を**追記のみ**:

```cpp
void ...::getStateInformation (juce::MemoryBlock& d) {
    if (auto xml = apvts.copyState().createXml()) {   // 既存 state はそのまま
        programs.writeStateAttribute (*xml);           // presetIndex 属性を追記
        copyXmlToBinary (*xml, d);
    }
}
void ...::setStateInformation (const void* d, int n) {
    if (auto xml = getXmlFromBinary (d, n))
        if (xml->hasTagName (apvts.state.getType())) {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            programs.readStateAttribute (*xml);        // 欠落は 0(旧 state 互換)
        }
}
```

**追記のみ = 既存セッション互換**なので version は minor で済む(major 不要)。
特殊な state(nam-player の `files` 子ツリー等)がある場合は**既存ロジックを壊さず**
presetIndex 属性だけ最小差分で足す。

## 3. Editor(Source/PluginEditor.{h,cpp})

上段 26px 行(タイトル ↔ Bypass の間)に `factory_ui::PresetSelector` を置き、
ホスト↔コンボを双方向同期(選択 → `setCurrentProgram` + `updateHostDisplay
(withProgramChanged)`、ホスト側変更は `audioProcessorChanged (…, programChanged)`
を SafePointer 経由で message thread に marshal してコンボ更新)。既存レイアウトを
壊さない。詳細は既存プラグインの Editor と一致させる(`factory-ui` スキル)。

## 4. tests/preset_test.cpp(wiring テスト, D5)

JUCE リンクの console app。**processor は必ず `std::make_unique` でヒープ確保**
(大きな inline バッファを持つ processor をスタック構築すると Windows の 1MB
スタックを超えて SEGFAULT する。main と check5 の round-trip 用インスタンス全て)。
検証 5 項目(**アサーション/tolerance/oracle の緩和は Ask a human #2**):

1. プログラム名が非空・一意。
2. 全 `paramID` が APVTS レイアウトに実在。
3. 全値がレンジ内(適用 → 読み戻し一致、クランプ = fail。オラクルはレイアウト宣言)。
4. program 0(Init)適用後、除外外の全パラメータ == デフォルト。
5. `setCurrentProgram(i)` → `getCurrentProgram()==i`、state 保存 → 復元で
   presetIndex round-trip、presetIndex 欠落 state は 0。

`CMakeLists.txt` に `juce_add_console_app` のテストターゲットを追加し CTest 登録
(既存 preset_test の CMake ブロックを踏襲)。**既存 DSP テストには触らない**。

## 5. 仕上げ

1. `plugin.toml` の `version` を **minor** bump(新プリセット=新機能)、本変更と
   **同一コミット**。shipped 機種も同様(次回リリースの再ビルド対象になる)。
2. `python tools/gen_catalog.py`(shipped は README の version 列に差分が出る)。
3. ビルド + 対象プラグインの ctest 全レート緑(既存 DSP + 新 preset_test)。
4. コミット: `feat(<slug>): ファクトリープリセットN種を追加` の形式。

新規プラグインは `tools/scaffold_plugin.py` が Init のみの空バンク + 上記配線 +
`preset_test.cpp` を自動生成する(`new-plugin` スキル)。プリセットを足すだけなら
本スキルの 1・4・5 のみ。
