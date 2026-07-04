---
name: core-primitives
description: Catalog of the shared DSP primitives in core/include/factory_core/ for this repo. Use when designing/implementing DSP for a plugin, to pick existing primitives to compose instead of reinventing them or grepping every header. Also covers the rules for adding/changing core headers.
---

# factory_core プリミティブ一覧

DSP は**既存プリミティブの合成**で作る。まずここから選び、当てはまるものが
あればそのヘッダだけを読む(全ヘッダの走査は不要)。

## フィルタ / クロスオーバー

| ヘッダ | 内容 |
|---|---|
| `Biquad.h` | 正規化 biquad + RBJ peaking 設計 |
| `Filters.h` | RBJ cookbook 設計一式: bell / low・high shelf / high・low pass(a0=1 正規化、z 領域で検証済み) |
| `OnePole.h` | 1-pole LP(補で HP)。ダンピング・トーン用 |
| `LinkwitzRiley.h` | LR4 クロスオーバー(low/high が同相、和が allpass) |
| `Crossover3.h` / `Crossover5.h` | LR4 ベースの 3 / 5 バンドスプリッタ(allpass 補償で完全再構成) |

## ダイナミクス

| ヘッダ | 内容 |
|---|---|
| `EnvelopeFollower.h` | attack/release 独立の 1-pole ピークフォロワ |
| `Compressor.h` | feed-forward log-domain comp(threshold/ratio/soft knee、ステレオリンク検出) |
| `MultibandCompressor.h` | Crossover3 + Compressor×3 + dry/wet |
| `DynamicEqBand.h` | パラメトリック EQ 1 バンド(帯域検出でゲイン変調可) |

## 歪み / 倍音

| ヘッダ | 内容 |
|---|---|
| `Waveshaper.h` | 無記憶・奇対称 soft-clip(drive/mix/output)。純関数なのでオーバーサンプリングで包める |
| `HarmonicShaper.h` | 5 次多項式シェイパ + ADAA1 |
| `FuzzEngine.h` | Fuzznari のファズ回路(連続モーフ、ゲート/自己発振域まで) |
| `MultibandEnhancer.h` | 5 バンド並列ハーモニックエンハンサ完成エンジン |

## ディレイ / 空間 / ピッチ

| ヘッダ | 内容 |
|---|---|
| `DelayLine.h` | 円環ディレイ、線形補間の fractional read |
| `GranularDelay.h` | グラニュラーディレイ完成エンジン(Hann グレイン、jitter、feedback) |
| `PitchShifter.h` | delay-line crossfade(rotating head)ピッチシフタ、FFT 不使用 |
| `ShimmerReverb.h` | 8-line FDN + feedback 内ピッチシフト ×2 のシマーリバーブ完成エンジン |

## FFT / スペクトル

| ヘッダ | 内容 |
|---|---|
| `FFT.h` | radix-2 complex FFT(prepare で前計算、in-place) |
| `FftConvolver.h` | ゼロレイテンシ FFT 畳み込み(IR を lock-free 差し替え) |
| `PartitionedConvolver.h` | 長い IR 用のゼロレイテンシ分割畳み込み |
| `StftResolution.h` | **`fftOrderForSampleRate(fs)`** — 分解能をレート追従させる唯一の入口 |
| `ResonanceSuppressor.h` | soothe 系動的レゾナンス抑制(STFT、75% overlap、完全再構成) |
| `ReductionProfile.h` | 抑制量の周波数プロファイル(オーディオフィルタではない) |

## リサンプリング / ルーティング

| ヘッダ | 内容 |
|---|---|
| `Oversampler.h` | 整数比 1x/2x/4x(Kaiser windowed-sinc)。非線形段を包む用 |
| `Resampler.h` | 任意比ストリーミング(Catmull-Rom) |
| `PolyphaseResampler.h` | 任意比ストリーミング(帯域制限 windowed-sinc)、Resampler と同じ契約 |
| `ResamplerLatency.h` | host↔model 往復レイテンシの純関数 |
| `RateBracket.h` | 「固定レート区間をホストレート内で走らせる」ブラケット |
| `NamRoutingEngine.h` | NAM Player の 3 スロット直列/並列ルーティング |

## テスト補助

`testing/DspInvariants.h` — レート行列と不変量ヘルパ(`write-dsp-test` スキル参照)。

## core/ を触るときの規則

- 全て **header-only・JUCE 非依存**・process 中 allocation/lock/syscall なし。
  安定 API 扱い — 既存ヘッダのシグネチャ変更は依存プラグイン全部に波及する。
- FFT/STFT の次数は必ず `fftOrderForSampleRate` から導出(固定次数禁止)。
- フィードバックノードには finite ガード(1 個の NaN/Inf から自己回復)。
- **core/ 配下を変更したら、依存する全プラグインのテストを回す**:
  `ctest --test-dir build --output-on-failure`(全部)。
- 新プリミティブには docs/regression-policy.md の該当不変量でゲートを付ける。
- NAM Player だけ JUCE 外の依存(NeuralAmpModelerCore)を持つ。`cmake/NamCore.cmake`
  は OBJECT ライブラリ・Eigen ピン・`NAM_SAMPLE_FLOAT`・PIC が全部 load-bearing —
  触る前にヘッダコメントを読むこと。
