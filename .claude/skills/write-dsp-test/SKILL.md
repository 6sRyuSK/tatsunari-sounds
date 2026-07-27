---
name: write-dsp-test
description: Write or modify a plugin's headless DSP test (plugins/<slug>/tests/dsp_test.cpp) in this repo, or pick which of the repo's test categories a new gate belongs in. Use when writing verification/regression tests for DSP, choosing oracles/tolerances, or replacing the scaffold test stub. Contains the DspInvariants.h API and the oracle rules — do NOT read other plugins' test files as reference.
---

# DSP テストの書き方

他プラグインの `dsp_test.cpp` を参考読みしない。構造・API・オラクル規約は以下で完結。

## 0. まずどのテストを足すのかを選ぶ

このリポジトリのテストは 1 種類ではない。**置き場所を間違えると、そのゲートは
狙ったものを守らない。**

| 種別 | CTest 名 | 場所 / リンク | 何を守るか |
|---|---|---|---|
| **プラグイン DSP**(本スキルの主題) | `<slug>_dsp_<fs>` | `plugins/<slug>/tests/dsp_test.cpp`、`factory_core` のみ | そのプラグインの DSP 仕様と回帰不変量。全レート |
| core プリミティブ | `core_primitives_<fs>` / `core_linear_ramp_<fs>` / `core_pitch_detector_<fs>` / `core_psola_shifter_<fs>` | `core/tests/` | **共有プリミティブ**の仕様。新しい `core/` ヘッダのゲートはここ(`core-primitives` スキル) |
| プリセット/テーブル配線 | `<slug>_preset` | `plugins/<slug>/tests/preset_test.cpp` | id 実在・レンジ・除外・Init。pitch-fix は headless、RS/deq は JUCE リンクのオラクル形(`add-preset` スキル) |
| バイト等価オラクル | `resonance_suppressor_rscore_equiv` / `dynamic_eq_deqcore_equiv` | JUCE processor vs `<X>Core` | 脱 JUCE 移行のドリフト。**バイト一致**、全レート |
| CLAP シェル層 | `resonance_suppressor_clap_shell` | `plugins/resonance-suppressor/tests/clap_shell_test.cpp`、`factory_shell` のみ | リサイズ数学の不動点等、シェルの純粋関数 |
| 共有モデル | `params` / `presets` | `params/tests` / `presets/tests` | ParamStore・Range・StateCodec・PresetSession |
| UI(visage 不要の部分) | `factory_ui_visage_theme` / `factory_ui_visage_spectrum_<fs>` / `factory_ui_visage_value_text` / `resonance_suppressor_theme_roundtrip` / `resonance_suppressor_ui_pure` | `ui/visage/tests/`、`plugins/*/ui/tests/` | テーマ JSON の往復、スペクトラム数理、値入力フロー(`visage-ui` スキル) |

ルート CMakeLists が `core/tests` `params/tests` `presets/tests` を直接登録するので、
プラグインを 1 つも構成しなくてもこの 3 つは走る。

> **CTest を回すときは `FACTORY_JUCE_ORACLES` を ON のまま**(既定 ON)。OFF にすると
> 等価オラクルと RS/deq の preset ゲートが**構成されず、黙って緑になる**。
> OFF は出荷パスだけをビルドする用途(release.yml / clap.yml)。

## ファイル構造(固定パターン)

- テストは `factory_core` のみ include(JUCE 禁止・ホスト不要)。
- `int g_failures; void fail(const std::string&)` を貯めて main で判定、
  失敗時 return 1。
- main は必ず:

```cpp
#include "factory_core/testing/DspInvariants.h"
namespace fct = factory_core::testing;

int main (int argc, char** argv)
{
    for (double Fs : fct::sampleRatesFromArgs (argc, argv)) // 全6レートが既定
        coreTests (Fs);
    ...
}
```

**レート集合を手書きしない**(44.1/48/88.2/96/176.4/192 kHz は
`kStandardSampleRates()` が唯一の定義)。CMake 側は scaffold 済みの
`foreach(_fs ...)` ループがレートごとに 1 ケース登録する。

## オラクルの規則(検証哲学の核心)

1. **独立オラクル**: 期待値は実装と別経路で導く。実装から期待値を生成したら
   「コードとコードの照合」にしかならない — 禁止。
2. **フィルタは z 領域で**: 期待応答は離散伝達関数 `H(e^jω)` を直接評価。
   アナログプロトタイプ `H(jΩ)` と比べると Nyquist 付近で偽陽性(bilinear warp)。
3. **式に依存しない不変量も併記**: 例「peaking EQ の f0 でのゲイン = gain
   パラメータ」「DC と Nyquist で unity」。係数式自体のバグを捕まえる。
4. **実信号を流して測る**: 線形フィルタは impulse → FFT で厳密・十分。
   非線形はトーン/スイープ。トーンは **ブロック長の整数周期**に置く(整数 bin、
   リーク無し)と DFT で高調波が正確に読める。偶対称チェックは高調波が
   Nyquist 未満に収まる f0 を選ぶ。
5. **レート依存の量を assert する**: 周波数・カウント(例: Nyquist 未満の奇数次
   高調波数 = 解析値)・bin 幅など、Fs を無視した実装なら必ず落ちる形にする。
6. **worst case を撃つ**: たまたま安定な一点(COLA 点等)でなく、最悪設定
   (最大 feedback、最疎 overlap、最大 Q×slope)でゲートする。

## DspInvariants.h API(`factory_core::testing`)

| ヘルパ | 用途 |
|---|---|
| `kStandardSampleRates()` / `sampleRatesFromArgs(argc, argv)` | レート行列の唯一の定義 / argv[1] で単レート |
| `allFinite(vec)` | NaN/Inf 検出(長時間ホールドで必須) |
| `noSubnormals(vec)` | **subnormal 検出**。FTZ/DAZ を強制しないホストでも denormal cliff に落ちないこと。コアは FP モード非依存なので純粋な算術としてこれが成り立つ必要がある。`allFinite` と対で使う(finite かつ subnormal なし = 全値が正常な double か厳密なゼロ)。regression-policy class V |
| `peakAbs(vec)` | ピーク。**現実的な**上限で bound(`1e6` トレランス禁止) |
| `windowEnergy(vec, start, len)` | 窓区間のエネルギー(Σx²)。減衰・エンベロープ・テールの手書き検算に |
| `impulseResponseNonIncreasing(process, Fs, tail=4.0, win=0.25, tol=1.05)` | フィードバック安定性: インパルス応答のエネルギーが窓ごとに非増加(ループゲイン<1)。worst-case 設定・全レートで |
| `binWidthHz(Fs, order)` / `windowLengthSec(Fs, order)` | 分解能の検算 |
| `resolutionFollowsSampleRate(Fs, maxBinHz=100, minWindowSec=0.010)` | FFT/STFT 次数が `fftOrderForSampleRate` 由来で最高レートでも分解能維持 |

## 回帰ポリシーの必須ゲート(該当するものを必ず入れる)

- フィードバックあり → `impulseResponseNonIncreasing` を最悪設定で。
- 高次フィルタカスケード → ピーク段 Q の**絶対上限**を z 領域で assert。
- フィードバックノード → finite ガードの自己回復 + 長時間ホールドの現実的ピーク上限
  (`allFinite` + `noSubnormals` + `peakAbs`)。
- ディレイ/変調 → worst-case バッファサイズ(黙ったクランプ禁止)。
- 検出器 → **絶対フロア**(無音で phantom reduction が出ない)。
- FFT/STFT → `resolutionFollowsSampleRate`。
- prepare/bypass/チャンネル遷移 → 状態リセット(残留が次のブロックに漏れない)。

詳細な分類は `docs/regression-policy.md`(必要時のみ参照)。

## 禁止事項(Ask a human)

トレランス・オラクル・レート集合・disabled-tests の**緩和**は自律判断禁止。
緑にするためにゲートを緩めない — 人間に issue/PR で判断を仰ぐ。等価オラクル
(`*_equiv`)の**バイト一致要件を緩めるのは特に禁止**: あれは移行のドリフトを
検出する唯一の仕掛けで、tolerance を入れた瞬間に無意味になる。
