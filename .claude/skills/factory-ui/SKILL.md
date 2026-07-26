---
name: factory-ui
description: Legacy JUCE look-and-feel (factory_ui — FactoryLookAndFeel / FactoryChrome) used ONLY by the FACTORY_JUCE_ORACLES oracle targets and archived plugins. For any shipping or new editor use the visage-ui skill instead. Use this only when editing the JUCE PluginEditor sources that the RS / dynamic-eq equivalence oracles compile, or when reviving an archived plugin.
---

# factory_ui(レガシー JUCE ルック&フィール)

> **出荷 UI と新規 UI は `visage-ui` スキル。** このスキルは JUCE エディタ
> (`plugins/{resonance-suppressor,dynamic-eq}/Source/PluginEditor.*`、および
> `archive/plugins/*/Source/`)を触るときだけ読む。

## 位置づけ(先に読む)

`factory_ui`(`ui/include/factory_ui/`、header-only INTERFACE ライブラリ)を link する
**出荷物はゼロ**。現在の消費者は 2 種類だけ:

1. **JUCE オラクルターゲット** — `FACTORY_JUCE_ORACLES=ON`(既定)のとき構成される
   RS / dynamic-eq の `preset_test` / `*core_equiv_test`。これらは
   `juce_add_console_app` で、ソースに `Source/PluginEditor.cpp` を含むため
   `factory_ui` を link する(例:
   `plugins/resonance-suppressor/CMakeLists.txt:53-72`)。**画面には出ない** —
   コンパイルが通り、processor がオラクルとして動くことだけが要件。
2. **archive 済みプラグイン** — `-DFACTORY_INCLUDE_ARCHIVED=ON` でしか構成されない
   (`archive/README.md`)。

`juce_add_plugin` / `FORMATS` / Standalone ターゲットは active plugin に **1 つも無い**。
したがって:

- **新しいエディタをこのスキルで書いてはいけない**(JUCE エディタは出荷経路に存在
  しない)。
- ここを触る動機は「オラクルのビルドを通し続ける」か「archive 機種の復活」だけ。
  RS / dynamic-eq のオラクルは **processor と `Source/` をコアと lockstep で保つ**ため
  に生きている(`add-param` スキル §5)。

## Editor の骨格(オラクル JUCE エディタの既存パターン)

- メンバに `FactoryLookAndFeel lnf;` を持ち、コンストラクタで `setLookAndFeel (&lnf)`、
  **デストラクタで `setLookAndFeel (nullptr)`**(必須 — 外すと JUCE のリークデテクタが
  落ちる)。
- タイトル: `titleLabel` を bold 20pt、色 `FactoryLookAndFeel::accent()`、テキストは
  製品名の大文字。Bypass トグルは `textColourId = textDim()`。
- `paint()` は `factory_ui::paintBackground (g, getLocalBounds());` から。
- レイアウトは `getLocalBounds().reduced (16)` から `removeFrom*` で切る。
  上段 26px にタイトル+右端 96px の Bypass、が旧来のスタイル。

## FactoryChrome.h ヘルパ(`namespace factory_ui`、header-only)

| 関数 | 用途 |
|---|---|
| `paintBackground (g, bounds)` | warm-white 縦グラデ背景 |
| `dropShadowFor (g, cardBounds, radius=10)` | カードの手前に呼ぶソフト影 |
| `paintCard (g, cardBoundsFloat, radius=10)` | 白カード + track 色アウトライン |
| `styleKnob (slider, label, name, suffix)` | ロータリーノブ+キャプションの一括設定(addAndMakeVisible は呼び手側) |
| `setSliderDecimals (slider, places)` | テキストボックスの小数桁を固定 |

**#23 の罠(重要)**: `SliderAttachment` はコンストラクタで
`textFromValueFunction` を上書きし、連続レンジだと最大 7 桁小数を表示する。
`setSliderDecimals` は **attachment 生成の後**に呼ぶこと(dB は 2 桁、% は 0 桁が慣例)。

## パレット(`FactoryLookAndFeel` static 関数、色は変更禁止)

| 関数 | 色 | 用途 |
|---|---|---|
| `background()` / `backgroundLo()` | warm white → gradient foot | 背景 |
| `panel()` / `panelLo()` | card white | カード |
| `track()` | 淡ピンクベージュ | グリッド / アウトライン |
| `accent()` / `accentDim()` | コーラル / 淡コーラル | タイトル・強調・ノブ |
| `text()` / `textDim()` | ソフトココア / muted | 文字 |
| `shadow()` | warm soft shadow | 影 |
| `bandColour (int band)` | 6 色パレット | マルチバンド系の帯色 |

出荷 Visage 側のパレットは JSON(`ui/visage/theme/factory-default.json`)が真実で、
**この表とは別系統**。片方を変えても他方には反映されない。

## カスタム描画コンポーネント(カーブ表示等)

- 別ヘッダ(例 `Source/XxxComponent.h`)に切り、描画は上のパレット関数のみで着色。
- audio スレッドと共有する値は atomic 経由で Processor から読む。
- カードに乗せるなら `paint()` 側で `dropShadowFor` → コンポーネント内で `paintCard`。

## プリセットセレクタ(JUCE 側)

`ui/include/factory_ui/PresetSelector.h`(ComboBox + 前/次矢印の view)と
`PresetSelectorController.h`(program リスト反映・双方向同期・オンディスクの
ユーザープリセット一覧 + Save As/Overwrite/Delete)。Editor はコントローラをメンバに
持ち `presetController (*this, p)` で構築、`resized()` で
`presetController.selector().setBounds(...)` するだけ。

**これは JUCE program API 上の仕組みで、出荷経路には無い。** 出荷側の等価物は
`factory_ui_visage::PresetSelectorView` + `factory_presets::PresetSession`
(`add-preset` スキル)。なお `UserPresetStoreFs` の消費者は現在この
コントローラとテストだけで、出荷 UI にユーザープリセットは出ていない。

## GUI 確認

**Standalone ターゲットは存在しない**(FORMATS の記述は active plugin に無い)。
JUCE エディタの見た目を目で確認する手段は現状ない — オラクルは headless に走るだけ。
UI の目視確認が要る作業は Visage 側の `tools/ui-dev` ハーネス(`visage-ui` スキル)。
