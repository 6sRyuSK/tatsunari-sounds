---
name: factory-ui
description: Build or edit a plugin editor/GUI in this repo using the shared factory_ui design system (FactoryLookAndFeel, FactoryChrome). Use when writing PluginEditor code, styling knobs/sliders/labels, or picking colours — contains the full API so you don't need to read ui/ headers or other plugins' editors.
---

# factory_ui エディタ規約

全プラグインは同じ「kawaii warm-white」ルックを共有する。**プラグイン個別の
パレットを作らない**。以下で API は完結 — `ui/include/factory_ui/` や他プラグインの
Editor を読み直す必要はない。

## Editor の骨格(固定パターン)

- メンバに `FactoryLookAndFeel lnf;` を持ち、コンストラクタで `setLookAndFeel (&lnf)`、
  **デストラクタで `setLookAndFeel (nullptr)`**(必須)。
- タイトル: `titleLabel` を bold 20pt、色 `FactoryLookAndFeel::accent()`、テキストは
  製品名の大文字。Bypass トグルは `textColourId = textDim()`。
- `paint()` は `factory_ui::paintBackground (g, getLocalBounds());` から。
- レイアウトは `getLocalBounds().reduced (16)` から `removeFrom*` で切る。
  上段 26px にタイトル+右端 96px の Bypass、が家のスタイル。

## FactoryChrome.h ヘルパ(`namespace factory_ui`、header-only)

| 関数 | 用途 |
|---|---|
| `paintBackground (g, bounds)` | warm-white 縦グラデ背景 |
| `dropShadowFor (g, cardBounds, radius=10)` | カードの手前に呼ぶソフト影 |
| `paintCard (g, cardBoundsFloat, radius=10)` | 白カード + track 色アウトライン |
| `styleKnob (slider, label, name, suffix)` | ロータリーノブ+キャプションの家スタイル一括設定(addAndMakeVisible は呼び手側) |
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

## カスタム描画コンポーネント(カーブ表示等)

- 別ヘッダ(例 `Source/XxxComponent.h`)に切り、描画は上のパレット関数のみで着色。
- audio スレッドと共有する値は atomic 経由で Processor から読む(Processor 側に
  message-thread 用の getter を生やすのが慣例、例: `makeDisplayShaper()`)。
- カードに乗せるなら `paint()` 側で `dropShadowFor` → コンポーネント内で `paintCard`。

## GUI 確認

Standalone ターゲットが常に生成される(CMake の FORMATS に scaffold 済み)。
ローカル確認は build 後の Standalone app を起動。
