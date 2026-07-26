---
name: add-preset
description: Add or change factory presets on an existing plugin in this repo (PresetBank table, CLAP policy presetBank/excludeIds, PresetSession, PresetSelectorView, headless preset_test, version bump). Use when adding/editing a plugin's built-in presets — contains the end-to-end pattern so you don't need to read other plugins as reference.
---

# 既存プラグインへのファクトリープリセット追加

出荷経路は clap-first。プリセットは `PresetBank` テーブル →(CLAP Policy)→
`factory_presets::PresetSession` が適用し、エディタ側は
`factory_ui_visage::PresetSelectorView` から選ぶ。ホストのプログラムリストは CLAP
シェルが同じ session から生やす。**JUCE program API(`getNumPrograms` 等の override)を
書く経路は出荷側には無い。**

変更箇所は実質 3 つ: **テーブル → 除外リスト → headless `preset_test`**。Policy と
セレクタは既に配線済み(scaffold 生成物)なので、プリセットを足すだけなら触らない。
最後に version bump(**minor、PR 作成時に 1 回**)。設計背景は
`docs/plans/factory-presets.md`(D1〜D6)。

**プリセットの値・名前は "taste" = Ask a human #1。** 値はドラフトとして書き、
テーブル冒頭に「試聴サインオフ待ち」と明記する。マージ/リリース前の実機試聴
サインオフが前提(配線と検証だけを自動で完結させる)。

| プラグイン | テーブル | 備考 |
|---|---|---|
| pitch-fix | `PfPresets.h`(`pitch_fix_presets`) | clap-first 生まれ。現行形の見本 |
| resonance-suppressor | `Source/FactoryPresets.h`(`resonance_suppressor_presets`) | ↓ |
| dynamic-eq | `Source/FactoryPresets.h`(`dynamic_eq_presets`) | ↓ |

**RS と dynamic-eq のバンクは `Source/` の下にあるが出荷シェルが include している**
(`plugins/dynamic-eq/shell/ClapEntry.cpp:30`、
`plugins/resonance-suppressor/shell/ClapEntry.cpp:34-36`)。「`Source/` はオラクル専用」
という一般規則の**例外**で、ここを「触っても出荷に影響しない」と誤読すると出荷
バイナリを壊す。

## 共有基盤(読むのはこれで十分)

- `presets/include/factory_presets/PresetBank.h` — JUCE 非依存の constexpr 型
  `PresetParam{ paramID, value }` / `Preset{ name, params, numParams }` /
  `PresetBank{ presets, numPresets }`。`value` は**実値(正規化前)**。
- `presets/include/factory_presets/PresetSession.h` — フレームワーク非依存の
  プログラム機構。`ParamStore` を実値で駆動する。
  - **index 0 = 合成された "Init"**、1..N がバンクの順。Init はテーブルに書かない。
  - `applyProgram(i)` は**管理対象の全 param** に書く: プリセットが挙げていれば
    その値、挙げていなければ**そのパラメータの default**(前のプログラムの残留を
    作らない)。除外 param は Init を含むどのプログラムでも書かれない
    (テーブルが誤って挙げていても書かれない)。
  - 書き込みは `ParamStore::setFromHost` 経由 = レンジへスナップ/クランプ + epoch
    更新。戻り値の `Applied{index,value}` をシェルがホストへ中継する。
  - `isDirty()` は「適用直後の値からの乖離」。`setCurrentProgramClean(i)` は state
    復元後にパラメータを書かずに基準を取り直す。
- `ui/visage/include/factory_ui_visage/PresetSelectorView.h` — `<` 名前 `>` の
  **ダムビュー**。`setMenu(entries, sel)`(item/header/separator 混在、
  `steppable=false` で矢印の着地を禁止)/ `setItems(names, sel)` / `onChange(itemRow)`。
  意味付けはエディタ側の preset model。
- state(保存/復元)は **CLAP シェルの担当** — `factory_presets::StateCodec` +
  `ClapStateBridge`。プリセットを足すためにプラグイン側で state コードを書く必要は
  一切ない。

> **ユーザープリセット(ディスク保存)は現在、出荷エディタに配線されていない。**
> `UserPresetStore(Fs).h` のモデルは存在するが、消費者はレガシー JUCE の
> `PresetSelectorController` とテストだけ。Visage エディタの preset model
> (`names()` / `currentIndex()` / `load(index)`)にはファクトリープログラムしか
> 出ていない。ユーザープリセットを出荷 UI に出すのは**この作業の範囲外の新機能**。

## 1. `<Camel>Presets.h`(テーブル + 除外リスト)

```cpp
#pragma once
#include "factory_presets/PresetBank.h"
// DRAFT VALUES — 試聴サインオフ待ち(CLAUDE.md "Ask a human" #1)。配線は
// preset_test.cpp が検証。program 0 "Init" は PresetSession が合成するので列挙しない。
namespace <slug>_presets
{
    using factory_presets::Preset;
    using factory_presets::PresetParam;
    using factory_presets::PresetBank;

    inline constexpr PresetParam kTapeWarmth[] = {
        { "drive", 12.0f }, { "mix", 100.0f }   // choice は index、bool は 0/1
    };
    inline constexpr Preset kPresets[] = {
        { "Tape Warmth", kTapeWarmth, 2 },
    };
    inline const PresetBank bank { kPresets, 1 };

    inline constexpr const char* kExclude[] = { "bypass" /*, monitoring... */ };
    inline constexpr int kNumExclude = 1;
}
```

値は ParamDesc テーブルの**実レンジ内**の実値。レンジ外や存在しない id は
`preset_test` が fail にする。**house style: default 値のプリセットは列挙しない**
(未列挙 param は Init 同様 default に戻るため)。

### 除外リスト(hard rule, D4)— プリセットが触ってはいけない id

- `bypass` は**全機種で除外**(プリセットで音を止めない)。
- モニタ系(RS の `delta` / `scListen`、dynamic-eq の帯域 `b<N>_lsn` listen)。
  除外は**完全一致**なので、帯域ループで生成する id は**全帯域分を明示列挙**する。
- **レイテンシ/CPU トレードオフ、および外部ルーティングの有効化**(RS の `quality`、
  `scEnable`)。プリセット切替がホストの PDC を勝手に再ネゴシエートしたり、ユーザーが
  配線したサイドチェインを勝手に有効化してはいけない。
- **ユーザーの音楽的コンテキスト**(pitch-fix の `key` / `scale` / `a4`)。曲のキー設定は
  プリセットの持ち物ではない。
- **ユーザーのロード状態に依存する機能 param**(モデル/IR のスロット等)。プリセットは
  ユーザーが組んだ構成を戻さない。

一般則: 「ユーザーが明示的に選ぶ環境設定」であって「プリセットが表現する音作り」で
ないものは除外する。

## 2. Policy(既に配線済み — 新規バンク時のみ)

`shell/ClapEntry.cpp` の Policy に 2 メンバがあるだけ。既存機種では既に入っている。

```cpp
static const factory_presets::PresetBank& presetBank() { return <slug>_presets::bank; }
static std::vector<std::string> excludeIds()
{
    return std::vector<std::string> (<slug>_presets::kExclude,
                                     <slug>_presets::kExclude + <slug>_presets::kNumExclude);
}
```

シェルがこの 2 つから `PresetSession` を構築し、ホストのプログラムリスト・state・
エディタへ供給する。**プリセットを足すだけならここは触らない。**

## 3. エディタ(既に配線済み)

エディタは `<X>PresetModel`(`ui/<X>Models.h` の純粋インターフェース:
`names()` / `currentIndex()` / `load(index)`)にだけ依存し、`PresetSelectorView` に
その一覧を流す。実装は 2 つあり、どちらもプリセット追加で変更不要:

- 出荷: `shell/<X>ClapEditor.cpp` 内の `SessionPresetModel`(実 `PresetSession` を叩き、
  適用後に `notifyHostEdited()`(= `rescan(VALUES|TEXT)` + `mark_dirty`)+
  `onStateReplaced()`)。
- ハーネス: `tools/ui-dev/common/HarnessPresetModel.h`。

バンクが増えれば一覧は自動で伸びる。レイアウト規約は `visage-ui` スキル。

## 4. `tests/preset_test.cpp`(headless wiring テスト, D5)

現行形は **JUCE を link しない** console app(`factory_params` + `factory_presets`
のみ)。見本は `plugins/pitch-fix/tests/preset_test.cpp`。
**アサーション/tolerance/oracle の緩和は Ask a human #2。**

ゲートする項目:

1. テーブル健全性: id/uid 一意、`uid == fnv1a32(id)`、レンジ非空、default がレンジ内。
2. プリセット名が非空・一意。
3. 全 `paramID` が**実在**(`store.indexOf(id) >= 0`)。
4. 全値が**レンジ内**(オラクルは ParamDesc の宣言そのもの)。
5. どのプリセットも**除外 id を書いていない**。
6. `PresetSession` の振る舞い: `numPrograms() == 1 + N`、各プログラム適用直後は
   `isDirty()` が false、除外 param が**適用前の値のまま**、代表プリセットの狙った
   値が入る、Init で管理 param が default に戻る。
7. その機種固有の構造契約があるならそれも(例 pitch-fix の「performance 系は buffer
   だけを書く / sound 系は buffer を Normal に固定」)。

CMake は `add_executable` + `add_test` で登録(既存ブロックを踏襲)。
**既存 DSP テストには触らない。**

> **RS / dynamic-eq は加えて JUCE リンク版 `preset_test` を持つ**
> (`FACTORY_JUCE_ORACLES` の裏、`juce_add_console_app`)。これはオラクル側 —
> APVTS レイアウトとの **paramdesc parity** をビット単位で見る。プリセットを足すとき
> `Source/FactoryPresets.h` を変えたなら、そのテストの期待プログラム数も更新する。

## 5. 仕上げ

1. `plugin.toml` の `version` を **minor** bump(新プリセット=新機能)。bump は
   ブランチ作業中は行わず **PR 作成時に 1 回だけ**(squash-merge 前提)。
2. `python tools/gen_catalog.py`(shipped は README の version 列に差分が出る)。
3. ビルド + 対象プラグインの ctest 全レート緑(既存 DSP + preset_test)。CTest は
   `FACTORY_JUCE_ORACLES` を **ON のまま**回す。
4. **出荷済み機種なら `docs/manual/<name>.md` のプリセット節も更新**(現在 RS と
   dynamic-eq の 2 本)。
5. プログラム数が変わると**ホスト側のプログラム index がずれる**(既存セッションが
   別のプリセットを指す)。末尾に足すのが原則で、順序を変える場合は PR 本文に明記。
6. コミット: `feat(<slug>): ファクトリープリセットN種を追加` の形式。

新規プラグインは `tools/scaffold_plugin.py` が Init のみの空バンク + 上記配線 +
headless `preset_test.cpp` を自動生成する(`new-plugin` スキル)。プリセットを足すだけ
なら本スキルの 1・4・5 のみ。
