---
name: new-plugin
description: Start a new audio plugin in this repo (plugins/<slug>/). Use whenever creating/scaffolding a new plugin, or porting a roadmap.toml entry into a real plugin. Runs the scaffold script instead of reading existing plugins as reference — do NOT open other plugins' sources to copy conventions.
---

# 新規プラグインの作り方

**他プラグインのソースを参考読みしない**こと。規約はスキャホールドと本スキル群に
全て入っている。DSP 設計・UI・テストの詳細はそれぞれ `core-primitives` /
`factory-ui` / `write-dsp-test` スキルを参照(必要になった時だけ読む)。

## 1. スキャホールド実行

```bash
python tools/scaffold_plugin.py <slug> \
  --name "Tatsunari <Product Name>" --category <Category> \
  --reference "<reference gear>" --vst3-category "Fx <Sub>"
```

- slug は kebab-case。`--code`(4文字 PLUGIN_CODE、先頭大文字)は省略すると slug から
  導出し、既存コードとの重複は自動チェックされる。
- 生成物: `plugin.toml`(version 0.1.0 / in-progress)、`CMakeLists.txt`(規約済み:
  `factory_read_version`、AU は APPLE のみ、全レートのテストループ)、薄い
  Processor/Editor、**わざと失敗する** `tests/dsp_test.cpp` スタブ。
- README カタログは自動再生成される。roadmap.toml に同名エントリがあれば
  **その `[[plugin]]` ブロックを削除**(スクリプトが警告を出す)。
- ルート CMakeLists は `plugins/*/CMakeLists.txt` を自動 include するので登録作業は不要。

## 2. DSP エンジン

- エンジン本体は `core/include/factory_core/<Name>.h` に **header-only・JUCE 非依存**で
  置く(プラグインの `Source/` は薄いラッパのみ)。既存プリミティブの合成を最優先
  — 一覧は `core-primitives` スキル。
- FFT/STFT を使うなら次数は必ず `factory_core::fftOrderForSampleRate(fs)` から導出
  (固定次数は高レートで劣化する — 禁止)。
- `prepare()` で全確保、process 系は allocation/lock/syscall なし。フィードバック
  ノードには finite ガード。

## 3. Processor / Editor(scaffold の TODO を置換)

- パラメータ: APVTS + `getRawParameterValue` の atomic ポインタ。連続パラメータは
  `juce::SmoothedValue`(prepareToPlay で reset)— scaffold の `output` が手本。
- GUI/audio 共有スカラーは atomic。bypass はレイテンシ整合を保つ(saturator 方式:
  同一パスに identity を流す等)。`prepareToPlay` と bypass 解除で状態リセット。
- Editor は `factory-ui` スキルの規約どおり(`setSliderDecimals` は attachment の
  **後**に呼ぶ)。

## 4. テスト → ビルド

`write-dsp-test` スキルに従い spec ベースの検証を書く(スタブは書くまで赤)。

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build -R <slug_snake>_dsp --output-on-failure
```

## 5. 完了条件・コミット

- コミット規約: `feat(<slug>): 日本語説明`(識別子・技術用語は英語のまま)。
  version bump は対応する変更と**同一コミット**。
- CI ゲート: macOS/Windows ビルド + CTest 全レート + pluginval strictness 5
  (headless)。pluginval の allocation チェックを抑制しない。
- スコープ厳守: 頼まれていないバンド/フォーマット/機能を足さない。
- 音の良し悪し・トレランス変更・リリースは **Ask a human**。

scaffold は factory presets の配線(Init のみの空バンク `FactoryPresets.h`、
ProgramAdapter 委譲、`PresetSelector`、`preset_test.cpp`)を標準装備で生成する。
実際のプリセットを足すのは `add-preset` スキル(値は taste = Ask a human)。
