# NAM Player 0.2.0 実装プラン

対象: `plugins/nam-player/` と、それが使う `core/` プリミティブ。
0.1.0 のコードレビュー（2026-07）で確認された問題の修正と、新機能「リアンプ WAV
書き出し（NAM マージ用）」の実装。**このドキュメントに列挙した作業のみを実装する
こと（スコープ厳守）。** CLAUDE.md の全ルール（リアルタイム安全、検証哲学、
サンプルレートマトリクス、Ask a human）が前提。

実装順序は必ず **Step 1 → 2 → 3 → 4**。各 Step は独立にコミットし、テストは
修正と同じコミットに含める。

---

## 背景: レビューで確認された事実（テストの根拠となる実測値）

実装をそのまま再現したシミュレーションによる測定結果。修正後のテストゲートは
これらの再発を防ぐこと。

### (a) wet/dry レイテンシ不整合（P0-2）

`PluginProcessor.cpp` は `outFifo.pushZeros(reportedLatency)` で FIFO を
事前充填するが、wet の実遅延は「事前充填 L」+「リサンプラ往復群遅延 g」の
**L+g** になる。dry 遅延とホスト報告は L なので g サンプルずれる。

| ホストレート | g（往復群遅延） | 報告値/dry = L | wet 実測遅延 | ずれ |
|---|---|---|---|---|
| 44.1k | 4 | 20 | 24 | 4 |
| 88.2k | 6 | 22 | 28 | 6 |
| 96k   | 6 | 22 | 28 | 6 |
| 176.4k| 9 | 25 | 34 | 9 |
| 192k  | 10 | 26 | 36 | 10 |

→ Mix < 100% でコムフィルタ、PDC も g ずれ。

### (b) リサンプラに帯域制限が無い（P0-1）

`factory_core::Resampler`（4点 Catmull-Rom）は LPF を持たない。実測:

| 経路 | 入力トーン | 折り返し先 | レベル |
|---|---|---|---|
| 96k→48k（下り） | 30 kHz | 18 kHz | **0 dB（無減衰）** |
| 192k→48k（下り） | 40 kHz | 8 kHz | **0 dB（無減衰）** |
| 48k→44.1k（上り） | 23 kHz | 21.1 kHz | −5.4 dB |
| 48k→96k/192k（上り） | 20 kHz のイメージ @28 kHz | — | −11 dB |

→ 48 kHz 以外のホストで、入力の折り返しと NAM 生成倍音の折り返しが可聴レベル。

---

## Step 1 — P0-2: wet/dry 整合バグ修正 + E2E 整合テスト

### 1-1. リサンプラブラケットを core へ抽出（refactor）

現在 `PluginProcessor.h` にある「48 kHz セクションを挟む構造」
（`HostFifo` + down/up `Resampler` + 事前充填 + dry ディレイ整合）は JUCE 側に
埋まっていてヘッドレステストできない。**テスト対象は本番コードそのもの**で
なければならない（テスト内にパイプラインを複製するのは不可）ので、これを
`core/include/factory_core/RateBracket.h`（header-only、JUCE 非依存）へ抽出する。

要件:
- ステレオ（2ch）を 1 インスタンスで扱う。中央のセクション処理はコールバック
  （テンプレート引数 or 関数参照）で受ける:
  `process(inL, inR, outL, outR, n, sectionFn)` — sectionFn は
  `(float* l, float* r, int m)` を 48k ドメインで受ける。
- `prepare(hostRate, modelRate, maxHostBlock)` で全確保。`process` は
  無確保・無ロック・noexcept。
- `latencySamples()` が決定的な報告値を返す（下記 1-2 の定義）。
- hostRate == modelRate のときは完全バイパス（レイテンシ 0、現挙動どおり）。
- `PluginProcessor` はこのクラスを使うだけの薄いラッパーになる
  （`HostFifo`/`downSamp`/`upSamp`/`namBuf`/`upScratch`/`outFifo` を置き換え）。

### 1-2. バグ修正本体（fix）

- FIFO 事前充填を **安全マージンのみ** にする: `kFifoMargin = 16` を定義し
  `pushZeros(kFifoMargin)`。
- 報告レイテンシは従来どおり
  `resamplerRoundTripLatency(host, model, base) + kFifoMargin`。
  これで wet 実遅延（マージン + 群遅延 g）と dry 遅延・報告値が一致する。
- dry ディレイ・`setLatencySamples` は `latencySamples()` を参照。

### 1-3. E2E 整合テスト（dsp_test.cpp に追加）

`RateBracket` + 素通しセクション（identity）で:

- 全レート `kStandardSampleRates()` × ブロックサイズ {64, 480, 512, 2048}。
  480 は非 2 冪を意図的に含める。
- 帯域制限した既知信号（例: 0.3×48k までのローパス済み乱数）を数百ブロック
  流し、出力と入力の相互相関ピークの**整数ラグが `latencySamples()` と一致**
  すること、ピーク相関が高い（> 0.99）ことを assert。
- アンダーラン検出: 整合位置での一致度チェックが FIFO のゼロ挿入を検出する
  （ゼロが混ざれば相関が落ちて fail する）。
- 48k ホストではレイテンシ 0・ビット等価パススルーを assert。

このテストは (a) の表のバグを Step 1 適用前に流せば fail する構造にすること
（＝バグ経路を実際に踏むテスト）。

---

## Step 2 — P0-1: 帯域制限付きリサンプラ + エイリアス抑圧テスト

### 2-1. `factory_core/PolyphaseResampler.h`（新規、header-only）

窓関数 sinc（Kaiser 窓）のポリフェーズ FIR 補間による任意比ストリーミング
リサンプラ。既存 `Resampler` と同じストリーミング契約
（`prepare(inRate, outRate)` / `process(in, n, out, outCap)`、位相と履歴を
ブロック間で保持、`process` は無確保）。

設計仕様:
- **阻止域減衰 ≥ 80 dB**（Kaiser β ≈ 7.86）。阻止域開始 =
  `0.5 × min(inRate, outRate)`。通過域は `0.4 × min(inRate, outRate)` まで
  リップル ±0.1 dB 目標。
- タップ数は上記を満たす**奇数**（Kaiser 設計式でおよそ 63。奇数にして
  群遅延 `D = (taps−1)/2` を入力サンプル整数にする）。
- 係数はポリフェーズテーブル（例: 512 位相 + 位相間線形補間）として
  `prepare()` で生成（確保は prepare のみ）。
- `groupDelayInputSamples()` が D を返す。
- 注意: `process` は `outCap` に達すると**残り入力を捨てる**（既存と同じ）。
  この事実をヘッダコメントに明記し、必要バッファ量
  `ceil(n/step) + 2` を文書化すること（P1-4 参照）。

既存 `Resampler.h`（Catmull-Rom）は削除しない（core は安定 API）。ヘッダに
「帯域制限なし。非線形処理を挟むレート変換経路では使用禁止」と追記する。

### 2-2. レイテンシ計算の更新

- `resamplerRoundTripLatency(host, model, base)` の式はそのまま、呼び出し側の
  `base` を `PolyphaseResampler::groupDelayInputSamples()`（= D）にする。
- 真の往復遅延 `D + D·host/model` は 44.1k 系で非整数になる。報告値は
  `lround` した整数とし、**残差 ≤ 0.5 サンプルは許容**（dry は整数遅延で読む。
  現行 0.1.0 と同じ扱い。E2E テストは整数ラグ一致 + 高相関で担保）。
- 追加レイテンシの目安: 44.1k で ~75 samples（≈1.7 ms）、192k で ~171 samples
  （≈0.9 ms）。アンプシムとして許容範囲。

### 2-3. `RateBracket` / nam-player を新リサンプラへ切り替え

- `RateBracket` 内の down/up を `PolyphaseResampler` に置換。
- **P1-4 をここで吸収**: 中間バッファは `namMaxBlk`・`upScratch` とも計算式
  （`ceil(n/step) + 2`）に **+32** の余裕で確保し、根拠をコメントに書く。

### 2-4. エイリアス抑圧テスト（dsp_test.cpp に追加、独立オラクル）

オラクルは出力の DFT（窓掛け）で直接スペクトルを測る（実装と独立）。
レートペアは `kStandardSampleRates()` の各レートと 48k の双方向全て。

- **折り返し（下り host→48k、host > 48k のみ）**: 出力ナイキスト
  （24 kHz）超かつ遷移帯域外（≥ 1.1×24 kHz）のトーン数点（例: 30 k, 40 k,
  0.9×入力ナイキスト）→ 折り返し先ビンのレベル **≤ −70 dB**
  （設計 80 dB に対しゲート 70 dB。実測 0 dB だった経路）。
- **折り返し（上り 48k→44.1k）**: 23 kHz トーン → 21.1 kHz **≤ −70 dB**
  （実測 −5.4 dB だった経路）。
- **イメージング（上り 48k→host、host > 48k）**: 18 kHz トーン → 48k±18k の
  イメージビン **≤ −70 dB**（実測 −11 dB だった経路）。
- **通過域**: 300 Hz と 0.4×min(rates) 付近のトーンの振幅保存
  （RMS 解析オラクル、±0.5 dB）。既存の RMS テストの流儀を踏襲。
- **1:1 恒等**: 比 1:1 で正確に D サンプル遅延（既存の 2-sample 版の置換え）。
- Step 1 の E2E 整合テストが新レイテンシで全レート×全ブロックサイズで
  通ること（マージン 16 の妥当性検証を兼ねる）。

トレランスは新設ゲートなので自由に設定してよいが、**一度入れた後に緩めるのは
"Ask a human"**（CLAUDE.md）。

---

## Step 3 — P1 修正

### 3-1. バイパスのクロスフェード + ホスト統合

- `bypass` 切替を即時分岐からクロスフェードにする: `juce::SmoothedValue`
  （~10 ms、`prepareToPlay` で reset）で bypass 係数 b を作り、
  出力 = `dry·b + wetChain·(1−b)`。
- 完全バイパス静定後（b==1 が数ブロック続いたら）は現行どおり wet チェーンを
  スキップして CPU を節約してよい。解除時は stale な NAM 状態から再開するが、
  クロスフェードがトランジェントを覆う（regression policy の
  「reset() or crossfade」を crossfade で満たす）。
- `getBypassParameter()` override を追加し、layout の `AudioParameterBool`
  ("bypass") を返す。
- dry 書き込み・FIFO の位相は bypass 中も現行どおり維持する（整合を崩さない）。

### 3-2. IR キャップの時間ベース化 + パーティション畳み込み

- `kMaxIrSamples = 8192`（固定サンプル）を **`kMaxIrSeconds = 0.17`**（時間）に
  変更: `maxIr = lround(0.17 × hostRate)`。
- 192 kHz では単一パーティション overlap-save の FFT が N=65536 になり
  重すぎるため、**均一パーティション overlap-save** に切り替える:
  新規 `core/include/factory_core/PartitionedConvolver.h`
  （または `FftConvolver` の拡張。ゼロレイテンシ維持が必須 — 先頭
  パーティションが現在ブロックを含む構成にする）。
  - パーティション長 = maxBlock ベース、FFT サイズ 2×パーティション、
    per-block コスト = FFT 2 回 + K 回のスペクトル積和。
  - カーネル（周波数領域・パーティション列）は従来どおり `ModelHandoff` で
    lock-free 差し替え。`buildKernel` は const・メッセージスレッド、
    `process` は無確保。
- IR がキャップで切り詰められた場合のみ、末尾 **5 ms のフェードアウト**を
  かけてから カーネル化する。
- テスト（既存 convolutionTests を移植・拡張、独立オラクル維持）:
  - インパルス恒等（ゼロレイテンシ）、naive 時間領域畳み込みとの一致
    （ブロックサイズ {1, 7, 64, 512}）、IR 長を超えたエネルギー無し。
  - **レート不変性**: 同一 IR（秒数指定で生成）を全レートで処理し、有効 IR
    長（秒）がレートに依らないことを assert。
- `resampleIr`（現在は線形補間）は `PolyphaseResampler` を流用して帯域制限
  付きにする（オフラインなのでコスト自由。P2 だったが機構流用でほぼ無料）。

### 3-3. 想定超ブロックのチャンク処理

`processBlock` の `n > currentBlock` 早期 return（未処理音漏れ）を廃止し、
内部処理を `currentBlock` 以下のチャンクに分割して複数回呼ぶループにする。

---

## Step 4 — 新機能: リアンプ WAV 書き出し（NAM マージ）

**目的**: 音作り完成後、3 スロット + ルーティングのチェーン全体を単一の .nam に
置き換えて負荷とレイテンシを減らす。方式は**リアンプペア書き出し**:
プラグインがユーザー指定の入力 WAV を現在のチェーンでオフラインレンダリングし、
出力 WAV を書き出す。ユーザーは公式 NAM トレーナー（Colab / GUI）で
input/output ペアから単一 .nam を学習し、スロット 1 に読み込む。
**プラグイン内で学習は行わない。**

### 仕様（ユーザー確認済み）

- 入力信号: **ユーザーが WAV を指定**（NAM 標準トレーニング信号 v3_0_0.wav 等を
  想定）。**48 kHz の WAV のみ受け付け**、それ以外はエラーダイアログ
  （トレーナー較正互換のため。リサンプルはしない）。ステレオ入力は ch0 を使用。
- キャプチャ範囲: 常に含む = スロット enable / Series-Parallel / In Gain /
  Level（= `NamRoutingEngine` がやること全て）。
  **UI チェックボックス「Include Cab IR + Tone」**で IR 畳み込み + IR Level +
  Lo/Hi Cut を含めるか選択（デフォルト OFF = アンプのみ、NAM 流儀）。
  `in_trim` / `out_gain` / `mix` はグローバル I/O なので常に**含めない**。
- モノラル化: L チャンネル経路のモデルを使用し、**Balance は中央（0）に固定**
  してレンダリング。アクティブな Parallel スロットに balance ≠ 0 があれば
  「Balance は中央としてキャプチャされます」と通知する。
- 出力: 48 kHz / mono / 32-bit float WAV。保存ダイアログで指定
  （デフォルト名 `<input名>_reamp.wav`）。入出力はサンプル整合
  （48k チェーンにリサンプラ無し、IR 畳み込みはゼロレイテンシなのでオフセット 0）。

### 実装構造

- **`Source/OfflineReamp.h`（header-only・JUCE 非依存）**: レンダリングの
  純粋部分。`factory_core::MonoProcessor*` の組・`NamRoutingEngine` 設定
  スナップショット・（オプションで）IR カーネル + トーン係数を受け取り、
  `std::vector<float>` in → out をチャンク処理で埋める。
  スムーザーは `snap()` で静定させ、決定的な出力にする。
- **JUCE 側（`PluginProcessor` / `PluginEditor`）**:
  - エディタに「MERGE」セクション: 「Reamp Export…」ボタン +
    「Include Cab IR + Tone」トグル。
  - レンダリングは `juce::Thread`（または ThreadPool ジョブ）で実行。
    メッセージスレッド・オーディオスレッドでは行わない。進捗表示と完了/失敗
    ダイアログ。実行中はボタンを無効化（多重起動防止）。
  - **ライブのオーディオ用オブジェクトは触らない**: モデルは `filesTree()` の
    パスから **新規に** `NamModel` を load（48k, オフライン用ブロックサイズ）、
    エンジンも新規インスタンスに現在パラメータのスナップショットを設定。
    IR を含める場合は `irRaw` から 48k カーネルを新規構築。
- **テスト（dsp_test.cpp）**: `OfflineReamp.h` を tanh モックで検証
  （dsp_test は JUCE 非依存を維持できる）:
  - 同一構成の `NamRoutingEngine` 直接呼び出しと出力一致（配線検証）。
  - インパルス → 出力先頭サンプルから応答（オフセット 0 の整合）。
  - IR+トーン込み構成が「エンジン出力 → naive 畳み込み → z-domain フィルタ
    オラクル」の合成と一致。

---

## バージョン・カタログ・コミット

- `plugin.toml` の `version` を **0.2.0** に bump（新機能を入れる Step 4 の
  コミットに含める — バンプは変更と同一コミット）。
- `python tools/gen_catalog.py` で README カタログを再生成（手編集禁止）。
- core/ を変更するので **全プラグインの ctest を全レートで**回す。
- コミット分割（例。英語 prefix + 日本語説明、識別子は英語のまま）:
  1. `refactor(nam-player): リサンプラブラケットをfactory_core::RateBracketへ抽出（ヘッドレステスト可能化）`
  2. `fix(nam-player): wet/dryレイテンシ整合を修正（FIFO事前充填をマージンのみに）+ E2E整合テスト追加`
  3. `feat(core): PolyphaseResampler（Kaiser窓sinc・阻止域80dB）を追加しNAMセクションのエイリアシングを解消 + 抑圧テスト`
  4. `fix(nam-player): バイパスをクロスフェード化しgetBypassParameterを実装`
  5. `feat(core): PartitionedConvolverを追加、IRキャップを時間ベース(170ms)に変更`
  6. `fix(nam-player): 想定超ブロックをチャンク処理に変更`
  7. `feat(nam-player): リアンプWAV書き出し（NAMマージ用）を追加 (0.2.0)` — bump + カタログ再生成込み

## 完了条件（CI ゲート）

- `cmake --build build` 全 OS マトリクス相当がローカルで通る。
- `ctest` 全プラグイン × 全レート green（新規テスト含む）。
- pluginval strictness 5 headless（CI）で全フォーマット pass。allocation
  チェックの抑制禁止。
- トレランス・オラクル・テストインフラを**緩める**変更が必要になった場合は
  実装で解決せず人間に確認（CLAUDE.md "Ask a human"）。音質判断
  （クロスフェード長・フィルタの聴感など）も同様。
