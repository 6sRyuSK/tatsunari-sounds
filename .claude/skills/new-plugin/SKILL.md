---
name: new-plugin
description: Start a new audio plugin in this repo (plugins/<slug>/). Use whenever creating/scaffolding a new plugin, or porting a roadmap.toml entry into a real plugin. Runs the scaffold script instead of reading existing plugins as reference — do NOT open other plugins' sources to copy conventions.
---

# 新規プラグインの作り方

**他プラグインのソースを参考読みしない**こと。規約はスキャホールドと本スキル群に
全て入っている。DSP 設計・UI・テストの詳細はそれぞれ `core-primitives` /
`visage-ui` / `write-dsp-test` スキルを参照(必要になった時だけ読む)。

新規プラグインは **CLAP ファースト**で生まれる。JUCE の `AudioProcessor` /
`AudioProcessorEditor` は一切生成されない — 脱JUCE 移行は不要。

## 1. スキャホールド実行

```bash
python tools/scaffold_plugin.py <slug> \
  --name "Tatsunari <Product Name>" --category <Category> \
  --reference "<reference gear>" --description "<one-line host description>"
```

- slug は kebab-case。`--code`(4文字 AUv2 subtype、先頭大文字)は省略すると slug から
  導出し、既存コードとの重複は自動チェックされる。`--design-size WxH` でエディタの
  固定デザインサイズ(既定 920x560)。
- 生成物: `plugin.toml`(version 0.1.0 / in-progress)、`CMakeLists.txt`(headless
  テストのみ)、`<Camel>Core.h` / `<Camel>Params.h` / `<Camel>Presets.h`、
  `ui/`(Visage エディタ + visage-free な UI seam)、`shell/`(CLAP Policy + entry +
  エディタホスト)、**わざと失敗する** `tests/dsp_test.cpp` スタブ、
  headless な `tests/preset_test.cpp`。
- README カタログは自動再生成される。roadmap.toml に同名エントリがあれば
  **その `[[plugin]]` ブロックを削除**(スクリプトが警告を出す)。
- ルート CMakeLists は `plugins/*/CMakeLists.txt` を自動 include し、
  `shell/CMakeLists.txt` を持つプラグインは自動的に clap-first 組み立てに参加する
  ので登録作業は不要。**ただし `clap.yml` の matrix に slug を足すのは手作業**
  (足さないと clap-validator ゲートが回らない)。

## 2. DSP エンジン

- エンジン本体は `core/include/factory_core/<Name>.h` に **header-only・framework
  非依存**で置き、`<Camel>Core.h` から合成する。既存プリミティブの合成を最優先
  — 一覧は `core-primitives` スキル。
- FFT/STFT を使うなら次数は必ず `factory_core::fftOrderForSampleRate(fs)` から導出
  (固定次数は高レートで劣化する — 禁止)。
- `prepare()` で全確保、`process()` は allocation/lock/syscall なし。フィードバック
  ノードには finite ガード。`reset()` は再確保なしで状態だけ消す。

## 3. パラメータ / シェル / エディタ(scaffold の TODO を置換)

- パラメータは `<Camel>Params.h` の `ParamDesc` テーブルが単一の真実。id は
  ワイヤ識別子(CLAP uid = `fnv1a32(id)`、state もこれで引く)なので
  **リネームは保存済みセッションを壊す**。追加は末尾へ、id の使い回しは禁止。
  詳細は `add-param` スキル。
- `shell/ClapEntry.cpp` の Policy: `<Camel>Ix` に `indexOf` の結果をキャッシュし
  (process 内で文字列探索をしない)、`fillSnapshot` で ParamStore →
  スナップショットへ。descriptor の CLAP feature に plugin.toml のカテゴリに
  対応するものを足す(VST3 サブカテゴリはここから導出される)。
  レイテンシがパラメータで変わるなら `primeFrames()` を 0 以外にする。
- GUI/audio 共有スカラーは atomic(`uiXxx`)。エディタは `visage-ui` スキルの規約
  どおり、共有 `factory_ui_visage` ウィジェットを合成する(独自 look-and-feel 禁止)。
- state / プリセットの配線は書かない: `factory_presets::StateCodec` +
  `PresetSession`、セレクタは `factory_ui_visage::PresetSelectorView` を
  scaffold が既に繋いでいる。

## 4. テスト → ビルド

`write-dsp-test` スキルに従い spec ベースの検証を書く(スタブは書くまで赤)。

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DFACTORY_PLUGINS=<slug>
cmake --build build
ctest --test-dir build -R <slug_snake> --output-on-failure
```

新規プラグインは JUCE を一切参照しないので、この構成は JUCE を fetch しない
(fetch されるのは CLAP/VST3/clap-wrapper + Visage のみ)。

## 5. 完了条件・コミット

- コミット規約: `feat(<slug>): 日本語説明`(識別子・技術用語は英語のまま)。
  version bump はブランチ作業中は行わず **PR 作成時に 1 回だけ**
  (squash-merge 前提。bump 忘れ=リリース対象外)。
- CI ゲート: macOS/Windows ビルド + CTest 全レート + pluginval strictness 5
  (headless) + clap.yml の clap-validator。pluginval の allocation チェックを
  抑制しない。
- スコープ厳守: 頼まれていないバンド/フォーマット/機能を足さない。
- 音の良し悪し・トレランス変更・リリースは **Ask a human**。

scaffold は factory presets の配線(Init のみの空バンク `<Camel>Presets.h`、
`PresetSession` 委譲、`PresetSelectorView`、headless な `preset_test.cpp`)を
標準装備で生成する。実際のプリセットを足すのは `add-preset` スキル
(値は taste = Ask a human)。
