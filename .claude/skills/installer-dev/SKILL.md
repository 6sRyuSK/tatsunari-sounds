---
name: installer-dev
description: Work on the Go/Charm TUI installer in tools/installer/ (discovery, TUI screens, elevation, receipt, bootstrap scripts, installer-ci.yml). Use for any change under tools/installer/ — contains the module layout, dev commands, and the load-bearing caveats (elevation model, Windows binary naming) so you don't need to re-read its README first.
---

# TUI インストーラ開発

`tools/installer/` は自己完結の Go module(Bubble Tea / Lip Gloss)。JUCE ゲートとは
独立で、CI は `installer-ci.yml`(`tools/installer/**` 変更時のみ、`go test` + `go vet`)。

## 開発コマンド

```bash
cd tools/installer
go test ./...
go vet ./...
go build -o tatsunari-sounds-installer .   # 出荷名と同じ(manifest は下記)
```

ヘッドレス smoke(TTY 不要・書き込みなし・実リリースに接続):

```bash
go run . --no-tui --dry-run --json --os macOS  --plugins all --formats vst3,au
go run . --no-tui --dry-run        --os Windows --plugins tn-resonance-suppressor --formats vst3
```

## モジュール地図

| 場所 | 役割 |
|---|---|
| `main.go` + `apply.go` + `tui_run.go` | CLI ディスパッチ: TUI 起動 / `__apply`(特権)/ `--no-tui` / `--dry-run`。テストは `main_test.go` / `apply_test.go` |
| `tty_*.go` | `curl \| bash` / `irm \| iex` 下での制御端末の再アタッチ |
| `internal/model` | plain 型、semver、install-plan(依存なし) |
| `internal/release` | GitHub 発見: releases / manifest / asset matrix / catalog / checksums |
| `internal/install` | インストール先、zip 展開、apply エンジン、quarantine/AU、receipt |
| `internal/elevate` | per-user(in-proc)+ system(osascript / RunAs)昇格 |
| `internal/i18n` | 日英文字列選択(OS ロケール) |
| `internal/app` | TUI とヘッドレスの共通オーケストレーション |
| `internal/tui` | 画面遷移: discover → plugins → formats → scope → confirm → progress → summary |
| `bootstrap/` | `install.sh` / `install.ps1` ワンライナー |

## load-bearing な注意(壊すと学び直しになる)

- **Windows のバイナリ名と manifest**: 出荷名は
  `tatsunari-sounds-installer(.exe)`(plan §11.5)。UAC の Installer Detection は
  名前に `install`/`setup`/`update`/`patch` を含む **unmanifested** exe に昇格を
  強制するので、Windows ビルドは `requestedExecutionLevel=asInvoker` の
  application manifest を埋め込んで無効化している。実体はコミット済みの
  `tools/installer/rsrc_windows_amd64.syso`(`tools/installer/winres/winres.json`
  から生成、再生成手順は `tools/installer/winres/README.md`)で、Go リンカが
  `GOOS=windows GOARCH=amd64` のとき自動で拾う。**この .syso を消すと出荷 exe が
  起動時に UAC を出す** — `winres_test.go` が manifest と .syso の両方をゲートする。
  system scope の昇格は従来どおり `__apply` 境界だけの責務。
- **昇格モデル**: 非特権プロセスが `0700` の temp dir に全 download+extract を
  ステージし `plan.json` を書き、**一度だけ** `__apply` サブコマンドとして OS の
  昇格機構(osascript / `Start-Process -Verb RunAs`)で自分を再起動する。
  `__apply` は宛先を install-root **allowlist** で、ソースを staging dir で
  再検証してから move する(改竄された plan が任意特権書き込みにならないため)。
  この検証を弱めない。
- **receipt**: schema 2 keys are `(slug, variant, scope)`. User receipts stay
  user-owned (non-privileged parent). System receipts live under
  `/Library/Application Support/…` / `%ProgramData%\…` and are written by
  `__apply` (plan §5.5). Legacy v1 slug-keyed files migrate on read.
- **配置先の vendor folder**(plan §11.4): VST3 と CLAP は **両スコープとも**
  標準 root 直下の `tatsunari-sounds/` に入れる(`install.VendorFolder`)。
  **AU だけは例外**で `Components` 直下 — 全対象 DAW が `Components` を再帰探索
  するという契約が置けないため。`TestAUNeverUsesVendorFolder` がこれを守る。
- per-user スコープは昇格なしで in-process 適用。
- ダウンロードは HTTPS のみ + checksum / catalog sha256 照合してから展開。
- 自己配置（plan §5.4）: apply 計画に実行中バイナリのコピーを含め、scope の
  installer bin へ置く。探索順は user → system（PATH 非依存）。
- 詳細・手動 smoke 手順は `tools/installer/README.md`(必要時のみ)。

## リリースとの関係

`installer.yml` は **`workflow_run`(`workflows: ["Release"]`, `types: [completed]`)**
で起動する。`release: published` では**ない** — release.yml は GITHUB_TOKEN で
Release を公開するが、GitHub は token 製の Release に `release: published` を
発火しない(再帰実行防止)ため、`release` を待っても永遠に起動しない。
`workflow_dispatch(tag)` は手動再実行/バックフィル用に残してある
(`gh workflow run installer --ref main --field tag=<year>.<n>`)。

起動後は `CGO_ENABLED=0` で
`tatsunari-sounds-installer-{darwin-amd64,darwin-arm64,windows-amd64}` をクロスコンパイル、
`tools/gen_catalog.py --emit-json` で `catalog.json` を作り、同じ Release に添付
する。`release.yml` や `manifest.json` には触らない(触らせない)。
