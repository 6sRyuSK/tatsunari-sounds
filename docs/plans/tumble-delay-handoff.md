# Tumble Delay — 実装引き継ぎ(リモート → ローカル Claude Code)

> **このファイルは作業引き継ぎ用の一時ドキュメント。** 全タスク完了時の仕上げ
> コミットで削除すること。仕様の正典は `docs/plans/physics-granular-delay.md`
> (以下「仕様書」)。

## 1. 現在の状態(このブランチ `claude/physics-granular-delay-design-ykjfne`)

| コミット | 内容 |
|---|---|
| `f613c65` | 仕様確定(名称 Tumble Delay / D11 初速減衰 / Motion 既定 0%) |
| `a3e851d` | scaffold 生成(`plugins/tumble-delay/`, v0.1.0, PLUGIN_CODE `Tumb`) |
| `c805b1f` | エンジン `core/include/factory_core/TumbleDelay.h` 実装(921 行) |
| `d1897d5` | 箱の頂点規約を flat-bottom に修正(Square が ◇ でなく □) |

- **検証済み**: 全 10 プラグイン × 6 レートの DSP テスト 60/60 green(Linux)、
  macOS/Windows CI ビルド clean。垂直ビリヤードの閉形式オラクルでエンジンの
  衝突タイミングが **±0.55 サンプル**一致、初回発火レイテンシ +49 samples@48k
  (= 検出アタック 1ms + 1kHz ティック量子化)を実測確認。
- **PR #96** が open(base: main)。CI の赤は `tumble_delay_dsp_*` 6 件のみで、
  これは scaffold の**意図的に失敗するスタブ**。実テスト(下記フェーズ A)が
  入れば解消する。それ以外は全て green。
- 委託方針(ユーザー指示): **コーディングは Opus サブエージェント、プリセット等の
  軽作業は Sonnet サブエージェント**。フェーズごとに親がビルド+ctest で検証して
  からコミット。

## 2. 残タスク(この順で)

- **A. DSP テスト** `plugins/tumble-delay/tests/dsp_test.cpp`(スタブ置換)→ §3
- **B. Processor 配線** APVTS 116 パラメータ + エンジン接続 → §4
- **C. Editor + ビジュアライザ** → §5
- **D. ファクトリープリセット 8 本**(Sonnet)→ §6
- **E. 仕上げ**: full ctest / catalog / code-review / PR 更新 / 本ファイル削除 → §7

各フェーズ共通の規則: 仕様書と該当スキル(`write-dsp-test` / `add-param` /
`factory-ui` / `add-preset`)を正とする。**他プラグインの Source/ や tests/ を
参考読みしない**(スキルに全部ある)。`core/` は変更しない(変更が必要と思ったら
停止して人間に相談)。tolerance/oracle を緩めるのは禁止(Ask a human)。
コミットは Conventional Commits(英語プレフィックス+日本語本文)。

## 3. フェーズ A — DSP テスト(Opus 委託推奨)

対象: `plugins/tumble-delay/tests/dsp_test.cpp` のみ。コミット例:
`test(tumble-delay): T1-T12の閉形式オラクルによるDSPテストを実装`

読むもの: 仕様書 §10(T1〜T12)と §4 / `.claude/skills/write-dsp-test/SKILL.md` /
`core/include/factory_core/testing/DspInvariants.h` /
`core/include/factory_core/TumbleDelay.h`(公開 API と SlotParams —
**実装内部から期待値を導かない**)/ `docs/regression-policy.md` A/C/D/E/G/H/J。

### エンジンの実測済み挙動(ハーネス設計の前提)

- 頂点規約 flat-bottom: 静止 Square は辺が水平。apothem = cos(π/N)。
  pivot(0,0) のとき箱中心 = (0,0)。
- スポーンは 1kHz ティック量子化 + 検出アタック 1ms → バーストから初回発火まで
  ~1〜2ms。**絶対時刻は ±2.5ms 許容、衝突間の間隔は ±2.5 サンプルで assert**
  (間隔は量子化フリー、実測誤差 ±0.55 サンプル)。
- mix/tone は 1-pole 平滑で**漸近収束**(厳密に目標に到達しない)→ パラメータ
  設定後 **0.6 秒の無音ウォームアップ**を置いてから測定(置かないと dry が
  −70dB 程度漏れ、1e-9 の無音判定を壊す)。
- 1 サンプルインパルスは Hann 窓の始点ゼロと重なり無音 → トリガー入力は
  **8〜12ms・220Hz・振幅 0.5 のサインバースト**。
- mix=1.0 で出力 = ウェット単独(ゲイン測定・無音測定はこれで)。
- Source の発火インデックス n は 0 始まり(Step の初回グレインは anchor0 を読む)。
- 決定化の基本形: spin=0, gravity=0, dirRandom=0, spawnSpread=0, sprayMs=0,
  pitchRandSemis=0, reverseProb=0, panMode=Center, refeed=0, drag=0 とし、
  テスト対象だけ有効化。
- グレインオンセット検出: 「|out| > 1e-9 が 100 サンプル以上の無音の後に出現」
  (バースト入力窓は除外)。

### テストとオラクル(全部実装。信号長・周波数・時刻は必ず Fs から導出)

- **T1a 垂直ビリヤード**: Square/boxSize0.4(vRef=5)/speed1/direction90/
  ballSize0.08/bounce1.0 → 間隔 = 2(cos(π/4)−0.08)/5 秒 ±2.5 サンプル。
- **T1b 斜めビリヤード**: direction 60° 等 → インセット正方形
  [−(a−r), +(a−r)]² 上の鏡像展開(unfolding)で第 k 衝突時刻列を閉形式計算し
  間隔列を assert。
- **T2 重力バウンス**: gravity=0.5(4 R/s²)/direction270/speed0.25/bounce0.7 →
  床衝突間隔が公比 0.7 の等比数列、グレインピーク比も 0.7(この設定は衝突速度が
  vRef 未満なのでゲインクランプ非介入)。
- **T3 Refeed 世代比**: 垂直/bounce1.0/drag0/gravity0/lifeIsBounces/
  lifeBounces=2/refeed=0.5 → 親と子の初回衝突は同一入射速度なので世代間ピーク比
  = 0.5(±1%)。
- **T4 最悪ケース有界**(クラス A/C): 4 スロット×count8/spin+2.0/bounce1.0/
  drag0/refeed0.95/mix1.0、バースト 1 発 + 10〜15 秒ホールド → allFinite かつ
  peak < 4.0(全レート)。
- **T5 silence**(クラス J): 完全無音 2 秒と −90dBFS ノイズ(テスト内固定シードの
  自前 xorshift)→ mix=1.0 で出力**完全ゼロ** + aliveBalls 常時 0。
- **T6 決定性**: 同一バースト入力の 2 フルラン(間に reset())→ ビット一致。
- **T7 リセット**(クラス E): run → reset() → 同一入力 run が 1 回目とビット一致。
- **T8 ホライズン/クランプ**(クラス D): (a) stepSeconds=−0.5・speed4・
  boxSize0.4(間隔 ~0.0627s、τ≈9×age)→ τ=33s 到達予測 ≈ age 3.7s:
  aliveBalls が [3.0s, 4.5s] 窓で 0、以後も allFinite。(b) timeSeconds=5.0 →
  実効 2s クランプ: count=2 で 2 本目由来の初回グレインが 1 本目の約 2.0s 後
  (±3ms)。
- **T9 パン則**: PanMode::Physics。(a) 垂直上壁ヒット(接触 x=0)→ L/R 比
  1±1e-6。(b) direction=0°(右壁、接触 x=apothem)→ gL/gR = cos(θ)/sin(θ)、
  θ=(clamp(apothem)+1)π/4 の閉形式(±1%)。
- **T10 Life Bounces**: bounce1.0/垂直/lifeBounces=5 → グレインがちょうど 5 個、
  以後 aliveBalls=0。
- **T11 トリガー列(D11)**: count=4/timeSeconds=0.35/bounce0.7/
  **lifeIsBounces=true・lifeBounces=1**(各ボールがグレイン 1 個だけ発火)→
  グレイン k(k=1..4)の時刻 ≈ onset + (k−1)×0.35 + (a−r)/(5×0.7^(k−1))
  ±2.5ms、ピーク比 = 0.7^(k−1)(±2%)。
- **T12 Source**: (a) Motion: 入力 = 振幅ランプ×220Hz(4 秒で 0.5→0、
  retrigMs=2000 で再トリガー防止)。motion=0 → 全グレインピークほぼ一定、
  motion=+1.0/preDelay=0.2 → ピーク列が ramp(t−0.2) に追従して減少(±10%)。
  (b) Step: バースト A(220Hz)/B(440Hz)/C(880Hz) を 0.12s 間隔で配置
  (retrigMs=2000)、stepSeconds=+0.12/grainMs=10 → グレイン n=0,1,2 の窓内
  ゼロクロス数が 220/440/880Hz の期待値(±30%)。(c) sprayMs=200 でも 2 ランで
  ビット一致(決定性維持)。

FFT 不要(全て時間領域)。実行時間は 1 レートあたり合計 ~2 分以内。エンジンの
バグと確信したら**修正せず報告**(親が判断)。完了条件:
`ctest --test-dir build -R tumble_delay_dsp --output-on-failure` が 6/6 green、
変更が dsp_test.cpp のみ。

## 4. フェーズ B — Processor 配線(Opus 委託推奨)

対象: `plugins/tumble-delay/Source/PluginProcessor.h/.cpp` の TODO(scaffold) 置換。
読むもの: 仕様書 §5(パラメータ表)/ `.claude/skills/add-param/SKILL.md` /
エンジンの公開 API。コミット例:
`feat(tumble-delay): APVTS 116パラメータとエンジン配線`

### パラメータ ID 規約(確定)

- **グローバル 16**: `boxShape`(choice), `boxSize`(0.05–4s log, 0.40),
  `boxSizeSync`, `spin`(−2..+2 rev/s, +0.20), `spinSync`, `pivotX`/`pivotY`
  (−1..+1, 0), `gravity`(0–100%, 0), `ballCollide`(bool, off),
  `sense`(−60..0 dB, −30), `retrig`(20–2000ms log, 150),
  `spawnSpread`(0–100%, 10), `refeed`(0–95%, 0), `tone`(500Hz–20kHz log, 12k),
  `mix`(0–100%, 35)。**出力ゲインは scaffold 既存の `output` を流用**(別 ID を
  作らない)。`bypass` も scaffold のまま。
- **スロット 25×4**: プレフィックス `a`/`b`/`c`/`d` + PascalCase:
  `aOn`(A のみ true), `aCount`(1–8 int, 1), `aBallSize`(2–25%, 8),
  `aSpeed`(0.25–4 log, 1), `aDirection`(0–360°, 90), `aDirRandom`(0–100%, 100),
  `aPreDelay`(0–1000ms, 0), `aPreDelaySync`, `aTime`(10–2000ms log, 350),
  `aTimeSync`, `aBounce`(20–100%, 70), `aDrag`(0–100%, 10),
  `aDecayCurve`(−100..+100, 0), `aLifeMode`(Time/Bounces), `aLifeTime`
  (0.1–16s log, 3), `aLifeBounces`(1–99 int, 12), `aPitch`(−24..+24st, 0),
  `aPitchRand`(0–12st, 0), `aGrain`(10–500ms log, 90), `aReverse`(0–100%, 0),
  `aMotion`(−100..+100%, 0), `aStep`(−500..+500ms, 0), `aSpray`(0–500ms, 0),
  `aPanMode`(Physics/Center/Random), `aGain`(−24..+6dB, 0)。
- **シンク音価テーブル**(choice、`factory_core::tempoSyncSeconds` で秒に解決。
  BPM は getPlayHead、無ければ 120):
  - `boxSizeSync`: off, 1/32, 1/16, 1/8, 1/4, 1/2, 1bar, 2bar(音価 = 横断時間)
  - `spinSync`: off, 1/4, 1/2, 1bar, 2bar, 4bar(1 回転あたり。回転方向は `spin`
    ノブの符号、spin=0 なら正)
  - `preDelaySync`: off, 1/64, 1/32, 1/16, 1/8, 1/4, 1/2(実効 1s クランプ)
  - `timeSync`: off, 1/32, 1/16, 1/16T, 1/8, 1/8T, 1/8., 1/4, 1/4T, 1/4., 1/2,
    1bar(実効 2s クランプ。T=×2/3、付点=×1.5)
  - sync ≠ off のとき free 値を上書き。クランプは仕様書 §4.9(クラス D)。

### ラッパの責務

- `prepareToPlay`: `engine.prepare(sampleRate)`(reset 内包)+ outputGain
  スムーザ reset(scaffold 踏襲)。
- `processBlock`: ブロック頭で全パラメータを atomic から読み、シンク解決して
  エンジン setter + `setSlotParams`×4 を呼ぶ(全て noexcept・alloc なし)。
  サンプルループで float↔double 変換して `processStereo`。モノバスは
  in を複製し出力は L/R 平均。出力ゲイン(smoothed)とバイパスはラッパで適用。
- **バイパス遷移で `engine.reset()`**(クラス E)。bypass 中はドライ素通し。
- `getTailLengthSeconds` → `engine.tailSeconds()`。レイテンシ申告 0。
- preset_test が「Init == レイアウト既定値」を検査するので、既定値は上の表と
  一致させること。pluginval strictness 5(allocation 検査)対象。

## 5. フェーズ C — Editor + ビジュアライザ(Opus 委託推奨)

対象: `plugins/tumble-delay/Source/PluginEditor.h/.cpp`。
読むもの: `.claude/skills/factory-ui/SKILL.md`(全 API がある)/ 仕様書 §7。
コミット例: `feat(tumble-delay): factory_uiエディタと物理ビジュアライザ`

- レイアウト: ヘッダ = FactoryChrome + PresetSelector(scaffold 配線済み)。
  左 ~55% ビジュアライザ / 右 World(Shape, Size+sync, Spin+sync, Gravity,
  Ball Collide)・Detect(Sense, Retrig, Spread)・Out(Refeed, Tone, Mix,
  Output) / 下段 スロットタブ A–D + 選択スロットのノブバンク
  (Physics / Source(`Motion` `Step` `Spray`) / Sound の 3 グループ)。
- ビジュアライザ: 30–60Hz の Timer。エンジンの `snapshotBalls` / `drainHits` /
  `boxAngle` だけを読む(**UI から物理状態への書き込みは APVTS 経由のみ**)。
  箱はパラメータ(shape/pivot)+ boxAngle から描画(flat-bottom 規約)。
  無信号でも回転を見せる(仕様の既定挙動)。衝突はフラッシュ+リップル。
  Pivot は箱上のハンドルをドラッグで `pivotX`/`pivotY` を編集
  (beginChangeGesture/endChangeGesture を忘れない)。HUD: トリガー列
  (Time×Count)・推定初回飛行時間・スロット別生存数。
- スロット別アクセント色はパレットから 4 色。**パレットに 4 アクセントが
  無ければ fork せず停止して人間に相談**(仕様書 §15)。
- 数値表示は `factory_ui::setSliderDecimals` 等(attachment の**後**に呼ぶ)。

## 6. フェーズ D — ファクトリープリセット(Sonnet 委託推奨)

対象: `plugins/tumble-delay/Source/FactoryPresets.h`。
読むもの: `.claude/skills/add-preset/SKILL.md` / 仕様書 §8(8 本のレシピ)。
コミット例: `feat(tumble-delay): ファクトリープリセット8本(仮レシピ)`

仕様書 §8 の 8 本(First Bounce / Tumbler / Ball Drop / Pinwheel /
Shimmer Fall / Marble Duet / Phrase Scanner / Fog Chamber)をパラメータ ID→値で
記述。**値は仮置き(音決めのリスニングは人間 — Ask a human)**。
`tumble_delay_preset` テスト green が完了条件。

## 7. フェーズ E — 仕上げ

1. `python tools/gen_catalog.py`(README カタログ再生成、`--check` が CI ゲート)。
2. **full ctest**(core/ にヘッダを足しているので全プラグイン・全レート)+
   ローカルでプラグインもビルド。
3. `/code-review`(または code-review スキル)を diff に当て、正当な指摘を反映。
4. **このファイル(`docs/plans/tumble-delay-handoff.md`)を削除**。
5. push → PR #96 のタイトル/本文を「仕様+実装」に更新(現状 docs 前提の文面)。
6. CI green 確認(macOS/Windows ビルド + CTest + **pluginval strictness 5**)。
   pluginval が落ちたら `.claude/skills/pluginval-debug` を参照。
7. 仕様書 §15 の残判断(音の初期値・プリセットのリスニング、メモリ/CPU 実測の
   予算承認、リリース)は**人間に確認**。version は 0.1.0 のまま
   (bump = リリーストリガーなので勝手に上げない)。

## 8. サブエージェント運用の教訓(リモート環境で得たもの)

ローカルでは不要かもしれないが、ブリーフ設計として有効:
- ブリーフは**自己完結**(必要な API 契約・数式・定数を全部書く。「読んで察して」
  にしない)。読むファイルは明示列挙し、それ以外を漁らせない。
- 「まずファイル骨格を書いてから肉付け」で進捗を外部から観測可能にする。
- 検証条件(コンパイルコマンド・スモークの合格数値)と報告フォーマットを
  ブリーフに含める。エージェントに git commit させず、親が検証してからコミット。
