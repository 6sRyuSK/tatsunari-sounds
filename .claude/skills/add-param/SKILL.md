---
name: add-param
description: Add or change a parameter on an existing plugin in this repo (APVTS layout, atomic wiring, editor knob, test, version bump). Use when adding a knob/toggle/setting to a plugin that already exists — contains the end-to-end wiring pattern so you don't need to read other plugins as reference.
---

# 既存プラグインへのパラメータ追加

変更箇所は 5 つで固定: layout → atomic 配線 → engine 接続 → editor → test。
最後に version bump(**minor、PR 作成時に 1 回**)。

## 1. createParameterLayout(Source/PluginProcessor.cpp)

```cpp
layout.add (std::make_unique<juce::AudioParameterFloat> (
    juce::ParameterID { "attack", 1 }, "Attack",          // version hint は 1 固定
    juce::NormalisableRange<float> { 0.1f, 100.0f, 0.01f, 0.35f }, 10.0f,
    juce::AudioParameterFloatAttributes().withLabel ("ms")));
// bool は AudioParameterBool、離散選択は AudioParameterChoice
```

- ID は snake/lower の英語。既存 ID の**改名・削除は state 互換を壊す**(major
  bump + Ask a human 級の判断)。デフォルト付きの追加なら古いプリセットはそのまま
  読める(APVTS の replaceState は欠損キーを許容)→ minor。

## 2. atomic 配線(コンストラクタ)

```cpp
std::atomic<float>* attackParam = nullptr;              // ヘッダにメンバ
attackParam = apvts.getRawParameterValue ("attack");    // ctor で取得
```

audio スレッドは `attackParam->load()` で読む。GUI/audio 共有のスカラーは必ず
atomic 経由(生 float の共有禁止)。

## 3. engine 接続

- **連続パラメータは平滑化必須**: `juce::SmoothedValue<float>` をヘッダに持ち、
  `prepareToPlay` で `reset (sampleRate, 0.02)` + `setCurrentAndTargetValue`、
  processBlock で `setTargetValue` → サンプル毎 `getNextValue()`。
  (engine 側が内部で ballistics/ramp を持つ場合はそちらに任せてよい)
- engine(core/ の header-only クラス)に setter を足す場合、core/ 変更扱い —
  **全プラグインのテストを回す**(`ctest --test-dir build --output-on-failure`)。

## 4. Editor(Source/PluginEditor.*)

```cpp
juce::Slider attackSlider;  juce::Label attackLabel;            // ヘッダ
std::unique_ptr<SliderAttachment> attackAtt;

factory_ui::styleKnob (attackSlider, attackLabel, "Attack", " ms");
addAndMakeVisible (attackSlider); addAndMakeVisible (attackLabel);
attackAtt = std::make_unique<SliderAttachment> (processor.apvts, "attack", attackSlider);
factory_ui::setSliderDecimals (attackSlider, 2);   // 必ず attachment の後(#23)
```

`resized()` にレイアウト追加。詳細な UI 規約は `factory-ui` スキル。

## 5. テスト(tests/dsp_test.cpp)

新パラメータの効果を **worst-case を含む**設定グリッドでゲートに追加する
(`write-dsp-test` スキル)。特に:

- feedback/resonance に効くパラメータ → 最大値で `impulseResponseNonIncreasing`。
- レンジ端(min/max)で finite + 現実的ピーク上限。
- 量的な効果は独立オラクル(解析値)と照合。

## 6. 仕上げ

1. `plugin.toml` の `version` を **minor** bump(新パラメータ=新機能)。
   state 互換を壊した場合は major。bump はブランチ作業中は行わず
   **PR 作成時に 1 回だけ**(squash-merge 前提。bump 忘れ=リリース対象外)。
2. `python tools/gen_catalog.py`
3. ビルド + 対象プラグインの ctest 全レート緑(core/ を触ったら全プラグイン)。
4. コミット: `feat(<slug>): Attackパラメータを追加` の形式。

## ファクトリープリセットとの関係

そのプラグインが `Source/FactoryPresets.h` を持つ場合、新パラメータは Init(program
0)で自動的にデフォルトへ戻る(ProgramAdapter が除外外の全 param を管理)。既存
プリセットの意図が新パラメータのデフォルトで崩れないか確認し、必要なら各プリセットに
値を足す(モニタ/ユーザー状態依存 param なら除外リストへ)。ID 改名時は
`FactoryPresets.h` 内の参照も直す(古い ID は `preset_test` の「paramID 実在」検証で
fail する)。詳細は `add-preset` スキル。
