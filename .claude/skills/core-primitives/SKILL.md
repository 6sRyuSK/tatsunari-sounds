---
name: core-primitives
description: Catalog of the shared DSP primitives in core/include/factory_core/ for this repo. Use when designing/implementing DSP for a plugin, to pick existing primitives to compose instead of reinventing them or grepping every header. Also covers core/tests and the rules for adding/changing core headers.
---

# factory_core プリミティブ一覧

DSP は**既存プリミティブの合成**で作る。まずここから選び、当てはまるものが
あればそのヘッダだけを読む(全ヘッダの走査は不要)。**全 47 ヘッダを網羅**。

出荷 3 機種が実際に使っているもの:

| プラグイン | 使用ヘッダ |
|---|---|
| resonance-suppressor | `ResonanceSuppressor` `MultiResSuppressor` `ReductionProfile` `StftResolution` `FFT` `LinkwitzRiley` `LinearRamp` |
| pitch-fix | `PitchDetector` `PsolaShifter` `FFT` `Biquad` `Filters` `SmoothingCoeff` `LinearRamp` |
| dynamic-eq | `DynamicEqBand` `Biquad` `Filters` `StftResolution` `LinearRamp` |

**(archive)** 印のヘッダは archive 済みプラグイン専用のエンジン。`-DFACTORY_INCLUDE_ARCHIVED=ON`
でしか構成されない(`archive/README.md`)。プリミティブとしては読めるが、live な
consumer がいないことを前提に扱う。

## フィルタ / クロスオーバー

| ヘッダ | 内容 |
|---|---|
| `Biquad.h` | 正規化 biquad + RBJ peaking 設計 |
| `Filters.h` | RBJ cookbook 設計一式: bell / low・high shelf / high・low pass(a0=1 正規化、z 領域で検証済み) |
| `OnePole.h` | 1-pole LP(補で HP)。ダンピング・トーン用 |
| `LinkwitzRiley.h` | LR4 クロスオーバー(low/high が同相、和が allpass) |
| `Crossover3.h` / `Crossover5.h` | LR4 ベースの 3 / 5 バンドスプリッタ(allpass 補償で完全再構成、最小レイテンシ) |
| `LinearPhaseCrossover5.h` | 5 バンド **線形位相** スプリッタ(隣接 FIR ローパスの差＝帯域和が純遅延、マスタリング向け。redesign はメッセージスレッドで lock-free 差し替え、タップ長レート連動) |

## ダイナミクス

| ヘッダ | 内容 |
|---|---|
| `EnvelopeFollower.h` | attack/release 独立の 1-pole ピークフォロワ |
| `Compressor.h` | feed-forward log-domain comp(threshold/ratio/soft knee、ステレオリンク検出) |
| `MultibandCompressor.h` | Crossover3 + Compressor×3 + dry/wet |
| `DynamicEqBand.h` | パラメトリック EQ 1 バンド(帯域検出でゲイン変調可)。dynamic-eq の帯域 1 本 |

## 歪み / 倍音 / lo-fi

| ヘッダ | 内容 |
|---|---|
| `Waveshaper.h` | 無記憶・奇対称 soft-clip(drive/mix/output)。純関数なのでオーバーサンプリングで包める |
| `HarmonicShaper.h` | 5 次多項式シェイパ + ADAA1 |
| `FuzzEngine.h` | ファズ回路(連続モーフ、ゲート/自己発振域まで)**(archive)** |
| `MultibandEnhancer.h` | 5 バンド並列ハーモニックエンハンサ完成エンジン **(archive)** |
| `Surikire.h` | lo-fi メディア劣化エンジン(wow/flutter、generation loss、テープサチュレーション、hiss、ドロップアウト)。**決定論的**(固定シード)**(archive)** |
| `WowFlutter.h` | テープ系ピッチ揺れ(wow = 遅く深い / flutter = 速く浅い 2 つの sine LFO が短いディレイの読み位置を変調)。`DelayLine` を合成 |

## ディレイ / 空間 / ピッチ

| ヘッダ | 内容 |
|---|---|
| `DelayLine.h` | 円環ディレイ、線形補間の fractional read |
| `HistoryBuffer.h` | 長尺ヒストリのリングバッファ + 可変 age の fractional 読み head(`OmoideEcho` の 120 秒メモリの「テープ」プリミティブ) |
| `GranularDelay.h` | グラニュラーディレイ完成エンジン(Hann グレイン、jitter、feedback)**(archive)** |
| `TumbleDelay.h` | 物理駆動グラニュラーディレイ(回転する 2D 箱の中のボール、壁衝突ごとに 1 グレイン)**(archive)** |
| `OnsenDelay.h` | ハーモニックグライドディレイ(ディレイ時間が音楽的比率の 3 ステップを巡回)**(archive)** |
| `OmoideEcho.h` | 「記憶するエコー」: 通常のフィードバックディレイ + 120 秒ヒストリを feedback-free の SCAN head が読む **(archive)** |
| `MicroLooper.h` | 常時録音のマイクロループ(freeze で直近 LENGTH 窓を焼き込む)**(archive)** |
| `Madoromi.h` | マイクロループ + アンビエントウォッシュエンジン(可変マスタークロックを 2 世界が共有)**(archive)** |
| `MochiStretch.h` | 「タイムマシン」系タイムストレッチ(`HistoryBuffer` を常時録音テープとして使う、ホストレート駆動)**(archive)** |
| `PitchShifter.h` | delay-line crossfade(rotating head)ピッチシフタ、FFT 不使用 |
| `PsolaShifter.h` | **PSOLA(TD-PSOLA 系)ピッチシフタ** — モノフォニック用、WSOLA 相関アライメント。pitch-fix の補正エンジン |
| `ShimmerReverb.h` | 8-line FDN + feedback 内ピッチシフト ×2 のシマーリバーブ完成エンジン **(archive)** |

## FFT / スペクトル / 検出

| ヘッダ | 内容 |
|---|---|
| `FFT.h` | radix-2 complex FFT(prepare で前計算、in-place) |
| `FftConvolver.h` | ゼロレイテンシ FFT 畳み込み(IR を lock-free 差し替え)。**テスト用比較オラクル専用** — 出荷パスは `PartitionedConvolver` |
| `PartitionedConvolver.h` | 長い IR 用のゼロレイテンシ分割畳み込み |
| `StftResolution.h` | **`fftOrderForSampleRate(fs)`** — 分解能をレート追従させる唯一の入口 |
| `ResonanceSuppressor.h` | soothe 系動的レゾナンス抑制(STFT、75% overlap、完全再構成) |
| `MultiResSuppressor.h` | `ResonanceSuppressor` の**デュアル解像度**フロントエンド(LR4 で 2 帯域に分け、高域の時間分解能を 4 倍に) |
| `ReductionProfile.h` | 抑制量の周波数プロファイル(オーディオフィルタではない) |
| `PitchDetector.h` | **モノフォニック基本周波数推定**(McLeod Pitch Method / NSDF を FFT 自己相関で評価、key-maximum ピッキング)。pitch-fix の検出器 |

## リサンプリング / ルーティング / 制御

| ヘッダ | 内容 |
|---|---|
| `Oversampler.h` | 整数比 1x/2x/4x(Kaiser windowed-sinc)。非線形段を包む用 |
| `Resampler.h` | 任意比ストリーミング(Catmull-Rom)。**テスト用比較オラクル専用**(帯域制限なし) — 出荷パスは `PolyphaseResampler` |
| `PolyphaseResampler.h` | 任意比ストリーミング(帯域制限 windowed-sinc)、Resampler と同じ契約 |
| `VariPolyphaseResampler.h` | **可変比**帯域制限ストリーミングリサンプラ(`PolyphaseResampler` と同じ Kaiser 数学、kHalfTaps=31 / kDensity=512) |
| `ResamplerLatency.h` | host↔model 往復レイテンシの純関数 |
| `RateBracket.h` | 「固定レート区間をホストレート内で走らせる」ブラケット |
| `NamRoutingEngine.h` | 3 スロット直列/並列ルーティング **(archive: nam-player)** |
| `LinearRamp.h` | **線形パラメータランプ** — JUCE `SmoothedValue<Linear>` の framework-free な代替。出荷経路の平滑化はこれ(JUCE は無い) |

## 共通ヘルパ / 数値

| ヘッダ | 内容 |
|---|---|
| `KaiserBessel.h` | 変形ベッセル関数 `besselI0(x)`。Kaiser 窓設計の共有実装(Oversampler / PolyphaseResampler / VariPolyphaseResampler / LinearPhaseCrossover5 が使用) |
| `SmoothingCoeff.h` | 1-pole 平滑係数の共有式: `onePoleCoeffForMs(ms, rate)`(ms 時定数→係数)/ `onePoleAlphaForTauSamples(tau)`(サンプル時定数→α = 1-exp(-1/tau)) |

## テスト

- `testing/DspInvariants.h` — レート行列と不変量ヘルパ(`write-dsp-test` スキル参照)。
- **`core/tests/`** — core 自身のゲート。ルート CMakeLists が
  `core/tests` / `params/tests` / `presets/tests` を直接 `add_subdirectory` するので、
  プラグインを 1 つも構成しなくてもこれらは走る。登録名は全レートで 1 ケースずつ:

  | ソース | CTest 名 |
  |---|---|
  | `primitives_test.cpp` | `core_primitives_<fs>` |
  | `linear_ramp_test.cpp` | `core_linear_ramp_<fs>` |
  | `pitch_detector_test.cpp` | `core_pitch_detector_<fs>` |
  | `psola_shifter_test.cpp` | `core_psola_shifter_<fs>` |

  **新しいプリミティブのゲートはここに足す**(プラグイン側の `dsp_test` ではなく)。

## core/ を触るときの規則

- 全て **header-only・JUCE 非依存**・process 中 allocation/lock/syscall なし。
  安定 API 扱い — 既存ヘッダのシグネチャ変更は依存プラグイン全部に波及する。
- FFT/STFT の次数は必ず `fftOrderForSampleRate` から導出(固定次数禁止)。
- フィードバックノードには finite ガード(1 個の NaN/Inf から自己回復)。
- **core/ 配下を変更したら、依存する全プラグインのテストを回す**:
  `ctest --test-dir build --output-on-failure`(全部)。`FACTORY_JUCE_ORACLES` は
  ON のまま(等価オラクルがそこに居る)。
- 新プリミティブには `docs/regression-policy.md` の該当不変量でゲートを付ける
  (置き場所は `core/tests/`)。
- `NamRoutingEngine.h` は archive 済み nam-player 専用で、JUCE 外の依存
  (NeuralAmpModelerCore)を持つ唯一の系統。`cmake/NamCore.cmake` は OBJECT
  ライブラリ・Eigen ピン・`NAM_SAMPLE_FLOAT`・PIC が全部 load-bearing — 触る前に
  ヘッダコメントを読むこと(通常の作業では登場しない)。
