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
go build -o tatsunari .    # ローカルバイナリ(名前に install を含めない — 下記)
```

ヘッドレス smoke(TTY 不要・書き込みなし・実リリースに接続):

```bash
go run . --no-tui --dry-run --json --os macOS  --plugins all --formats vst3,au
go run . --no-tui --dry-run        --os Windows --plugins saturator --formats vst3
```

## モジュール地図

| 場所 | 役割 |
|---|---|
| `main.go` | CLI ディスパッチ: TUI / `__apply`(特権)/ `--no-tui` / `--dry-run` |
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

- **Windows のバイナリ名**: UAC の installer 検出ヒューリスティックは、名前に
  `install`/`setup`/`update`/`patch` を含む unmanifested exe に昇格を強制する。
  出荷名は `tatsunari`。`go run .` は一時 `installer.exe` を作るのでプロンプトが
  出る — ローカルテストは中立名でビルドすること。
- **昇格モデル**: 非特権プロセスが `0700` の temp dir に全 download+extract を
  ステージし `plan.json` を書き、**一度だけ** `__apply` サブコマンドとして OS の
  昇格機構(osascript / `Start-Process -Verb RunAs`)で自分を再起動する。
  `__apply` は宛先を install-root **allowlist** で、ソースを staging dir で
  再検証してから move する(改竄された plan が任意特権書き込みにならないため)。
  この検証を弱めない。
- **receipt は常に非特権の親が書く**(root で書くと以後の per-user 更新が壊れる)。
- per-user スコープは昇格なしで in-process 適用。
- ダウンロードは HTTPS のみ + `SHA256SUMS.txt` 照合してから展開。
- 詳細・手動 smoke 手順は `tools/installer/README.md`(必要時のみ)。

## リリースとの関係

`installer.yml` が `release: published` で起動し、`CGO_ENABLED=0` で
`tatsunari-{darwin-amd64,darwin-arm64,windows-amd64}` をクロスコンパイル、
`tools/gen_catalog.py --emit-json` で `catalog.json` を作り、同じ Release に添付
する。`release.yml` や `manifest.json` には触らない(触らせない)。
