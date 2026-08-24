---
name: pluginval-debug
description: Diagnose and fix pluginval failures in this repo's CI (ci.yml validate steps, strictness 5) and tell them apart from clap-validator / CTest failures. Use when a CI "Validate plugins" step is red, pluginval reports a failure, or reproducing pluginval locally — contains how CI invokes it and the typical root causes, so you don't need to read ci.yml.
---

# pluginval 失敗の直し方

## 0. どのゲートが何を見ているか(層を間違えて直さないため)

| ゲート | 対象 | 走る場所 |
|---|---|---|
| **pluginval** strictness 5 | **wrapper VST3**(macOS+Windows)と **AU `.component`**(macOS) | `ci.yml` |
| **clap-validator** | **native `.clap`** — pluginval は `.clap` を見ない | `clap.yml`(active 3 機種の Linux レグ) |
| CTest | コア DSP / 等価オラクル / プリセット / 共有モデル / UI 純粋部 | `ci.yml`(唯一 CTest を回すワークフロー) |

3 つとも**同じコア**を叩くので、症状が pluginval に出ても原因はコアにあることが多い。
逆に「pluginval だけ落ちる/clap-validator だけ落ちる」ならラッパ層固有の疑い。

## CI での実行方法(再現の前提)

`ci.yml` は最新の pluginval を GitHub Release から落とし、ビルド済みバンドルに対して:

```bash
pluginval --strictness-level 5 --skip-gui-tests --validate-in-process --validate <bundle>
```

- **バンドルの探索先は `build/<slug>_assets/` の直下(フラット)**。make_clapfirst の
  出力形で、`VST3/` や `AU/` のパス階層はもう存在しない(あれは `juce_add_plugin` の
  形だった)。macOS は `*.vst3` と `*.component`、Windows は `*.vst3`。
- macOS の AU は `~/Library/Audio/Plug-Ins/Components/` にコピーしてから検証
  (AU はインストール済みでないと登録されない)。`killall -9 AudioComponentRegistrar`
  でレジストラを再起動してキャッシュを飛ばしている。
- **発見ゼロは `::error::` で落ちる**。「1 個も見つからず緑」という空振りの穴は
  塞いである(CTest 側の `--no-tests=error` と同じ思想)。この行が出たら原因は
  プラグインの中身ではなく、**アセット配置か plugin 選択**。
- 失敗ログは GitHub MCP の `get_job_logs`(`failed_only: true`)で取る。
  各バンドルは `::group::pluginval <path>` で折り畳まれている。

ローカル再現(macOS/Windows 実機がある場合)も同じコマンド。Linux では再現不可
(サポート外)— ログから原因を特定する。

### Windows でのローカル実行(落とし穴)

pluginval.exe は **GUI-subsystem** の実行ファイル。シェルから直接実行
(`& pluginval ...`)するとコンソールにアタッチせず即座に制御が返り、stdout も
`$LASTEXITCODE` も取れない。必ず `Start-Process` 経由で起動する:

```powershell
$proc = Start-Process -NoNewWindow -Wait -PassThru `
  -FilePath build\pluginval\pluginval.exe `
  -ArgumentList '--strictness-level 5 --skip-gui-tests --validate-in-process --validate "<path>.vst3"' `
  -RedirectStandardOutput out.log -RedirectStandardError err.log
$proc.ExitCode   # 0 + ログ末尾 SUCCESS で合格
```

- `--validate-in-process` を付けないと検証が子プロセスに分離され、ログ捕捉が壊れる。
- バイナリは github.com/Tracktion/pluginval の最新リリース zip を、gitignored な
  `build/pluginval/` に展開して使う(リポジトリにはコミットしない)。
- ホストから実際に読み込ませて挙動を見たいときは dev 専用の
  `tools/vst3-probe/`(Windows VST3 ホストプローブ)。

## 典型的な失敗と原因(このリポジトリでの頻出順)

修正先は **`<X>Core.h`(コア)** か **`plugins/<slug>/shell/ClapEntry.cpp`(Policy)** か
**共有シェル(`shell/include/factory_shell/`)**。出荷経路に JUCE エディタは無いので、
JUCE 固有の症状(leak detector、`setLookAndFeel`、attachment 寿命)はもう出ない。

| pluginval の症状 | root cause / 直し方 |
|---|---|
| `Allocations detected in audio thread` 系 | `Core::process` とそこから呼ぶ全部での allocate/lock/syscall。`Core::prepare` で前確保する。**Policy の `process` で `store.indexOf` を呼んでいないか**(文字列探索。インデックスは `<X>Ix` に prime 時キャッシュ)。**チェックの抑制は禁止(Ask a human)** |
| state save/restore テストで値が変わる | `StateCodec` の往復(実値の `to_chars`/`from_chars`)、`migrateState` フックの非対称、`PresetSession` の presetIndex 復元。ゲートは `presets` テストと `<slug>_preset` |
| `Parameter thread safety` / random values でクラッシュ・NaN | GUI/audio 共有スカラーが non-atomic(コアの `uiXxx` は `std::atomic`)、パラメータ急変時の平滑化なし(`core/include/factory_core/LinearRamp.h`)、検出器の絶対フロア欠如 |
| bus layout テスト失敗 | Policy の `kHasSidechain` とシェルのポート宣言の不一致。サイドチェインは**オプショナルなステレオ入力**として宣言される |
| latency テスト / bypass で位相ズレ | `latencySamples(core)` の報告漏れ・遅れ。レイテンシがパラメータで変わるコアは `primeFrames()` を 0 以外にして activate() で落ち着かせる(tn-vocal-tuner は `1<<13`、RS は `1<<15`) |
| sample rate / buffer size 変更でクラッシュ・ノイズ | `Core::prepare` での状態リセット漏れ、worst-case バッファサイズ不足(黙ったクランプ)。`reset()` は再確保なしで状態だけ消す |
| denormal 由来の CPU スパイク | FTZ/DAZ は**シェル境界**の責務(`factory_shell/DenormalGuard.h` が process を包む)。コア側で FP モードを仮定しない。テストは `noSubnormals` |
| AU だけ落ちる(macOS) | AU キャッシュ(再検証で直ることがある — まず re-run)、または AUv2 サブタイプコード衝突(`factory_clap_plugin` の `AUV2_SUBTYPE_CODE`。manufacturer は `Ttsn` 固定、`AUV2_INSTRUMENT_TYPE aufx`) |
| VST3 だけ / Windows だけ落ちる | clap-wrapper 層。Windows VST3 は**フォルダ形式バンドル**でないと検証が通らない(`WINDOWS_FOLDER_VST3 TRUE`) |

## 進め方

1. `get_job_logs` で最初に FAIL したテスト名(pluginval はテスト単位で出す)と
   直前の出力を読む。どのバンドル(フォーマット×OS)かを特定。
2. **同じ症状が clap-validator(`clap.yml`)にも出ているか**を確認する。両方に出て
   いればコア/Policy、pluginval だけならラッパ層(VST3/AU)の疑い。
3. 上の表で当たりを付け、該当コード(`Core::process` / `prepare` / Policy / 共有
   シェル)を確認。
4. 修正して CTest を回す(`FACTORY_JUCE_ORACLES` は ON のまま — RS/deq はコアを
   直したら等価オラクルが即座に落ちるので、**pluginval を待つ前に**そこで気づける)。
5. **pluginval 側・CI 側を緩めて緑にしない**(strictness、テストスキップ、allocation
   チェック抑制はすべて Ask a human)。
6. push → CI 再実行で全フォーマット緑を確認。
