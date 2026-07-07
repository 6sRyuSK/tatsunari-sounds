---
name: pluginval-debug
description: Diagnose and fix pluginval failures in this repo's CI (ci.yml validate steps, strictness 5). Use when a CI "Validate plugins" step is red, pluginval reports a failure, or reproducing pluginval locally — contains how CI invokes it and the typical root causes, so you don't need to read ci.yml.
---

# pluginval 失敗の直し方

## CI での実行方法(再現の前提)

ci.yml は最新の pluginval を GitHub Release から落とし、ビルド済みの全フォーマット
(VST3 は macOS+Windows、AU は macOS のみ)に対して:

```bash
pluginval --strictness-level 5 --skip-gui-tests --validate-in-process --validate <bundle>
```

- macOS の AU は `~/Library/Audio/Plug-Ins/Components/` にコピーしてから検証
  (AU はインストール済みでないと登録されない)。`killall AudioComponentRegistrar`
  でレジストラを再起動してキャッシュを飛ばしている。
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

## 典型的な失敗と原因(このリポジトリでの頻出順)

| pluginval の症状 | root cause / 直し方 |
|---|---|
| `Allocations detected in audio thread` 系 | processBlock(とそこから呼ぶ全部)での allocate/lock/syscall。`prepareToPlay` で前確保する。**チェックの抑制は禁止(Ask a human)** |
| state save/restore テストで値が変わる | `getStateInformation`/`setStateInformation` の非対称、APVTS 外の独自状態の保存漏れ、パラメータ range 外のデフォルト |
| `Parameter thread safety` / random values テストでクラッシュ・NaN | GUI/audio 共有スカラーが non-atomic、パラメータ急変時の平滑化なし(SmoothedValue)、検出器の絶対フロア欠如 |
| leak detector assert(テスト終了時) | Editor が `setLookAndFeel (nullptr)` をデストラクタで呼んでいない、attachment/子コンポーネントの寿命逆転 |
| bus layout テスト失敗 | `isBusesLayoutSupported` が mono/stereo 以外を拒否していない、in/out 不一致を許してしまっている |
| latency テスト / bypass で位相ズレ | `setLatencySamples` 未報告(oversampling 等)、bypass パスがレイテンシ非整合 |
| sample rate / buffer size 変更テストでクラッシュ・ノイズ | `prepareToPlay` での状態リセット漏れ、worst-case バッファサイズ不足(黙ったクランプ) |
| AU だけ落ちる(macOS) | AU キャッシュ(再検証で直ることがある — まず re-run)、または PLUGIN_CODE/MANUFACTURER_CODE 衝突 |

## 進め方

1. `get_job_logs` で最初に FAIL したテスト名(pluginval はテスト単位で出す)と
   直前の出力を読む。どのバンドル(フォーマット×OS)かを特定。
2. 上の表で当たりを付け、該当コード(processBlock / state / editor dtor)を確認。
3. 修正は該当プラグインの `Source/` または core 側。**pluginval 側・CI 側を
   緩めて緑にしない**(strictness、テストスキップ、allocation チェック抑制は
   すべて Ask a human)。
4. 修正を push → CI の再実行で全フォーマット緑を確認。
