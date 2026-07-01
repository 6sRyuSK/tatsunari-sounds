# tatsunari-plugins TUI installer

A single-binary, cross-platform (macOS + Windows) terminal installer for the
plugins published by this repo. Built with Go + [Charm](https://charm.sh)
(Bubble Tea / Lip Gloss / Bubbles). No runtime dependencies.

## What it does

- Discovers the latest consolidated release (`<year>.<n>`) from GitHub.
- Lets you pick **individual plugins** and **formats** (VST3 / AU; AU is
  macOS-only), with per-row badges for `NEW` / `↑ update` / `up to date`.
- **Updates** in place: installed versions come from a receipt, so re-running
  offers exactly the plugins that changed (update-available rows are
  pre-selected — one keystroke to update everything).
- Installs to the standard folders, choosing **system** (all users, one native
  password / UAC prompt) or **user** (no password) scope.
- Verifies every download against the release's `SHA256SUMS.txt` before
  extracting, over HTTPS only.
- Bilingual UI (日本語 / English) chosen from the OS locale.

## Install (end users)

macOS:

    curl -fsSL https://raw.githubusercontent.com/6sRyuSK/tatsunari-plugins/main/tools/installer/bootstrap/install.sh | bash

Windows (PowerShell):

    irm https://raw.githubusercontent.com/6sRyuSK/tatsunari-plugins/main/tools/installer/bootstrap/install.ps1 | iex

## Layout

```
main.go              CLI dispatch: TUI / __apply (privileged) / --no-tui / --dry-run
tty_*.go             reattach the controlling terminal under `curl | bash` / `irm | iex`
internal/
  model/             plain types, semver, install-plan (no dependencies)
  release/           GitHub discovery: releases, manifest, asset matrix, catalog, checksums
  install/           destinations, zip extract, the apply engine, quarantine/AU, receipt
  elevate/           per-user (in-proc) + system (osascript / RunAs) elevation
  i18n/              bilingual string selection
  app/               orchestration shared by the TUI and headless mode
  tui/               Bubble Tea screens: discover → plugins → formats → scope → confirm → progress → summary
bootstrap/           curl / irm one-liners
```

## Develop

Requires Go (see the `go` directive in `go.mod`).

    cd tools/installer
    go test ./...
    go vet ./...
    go build -o tatsunari .          # local binary (avoid names containing "install"; see below)

Headless / CI smoke (no TTY, no writes, hits the live release):

    go run . --no-tui --dry-run --json --os macOS  --plugins all --formats vst3,au
    go run . --no-tui --dry-run        --os Windows --plugins saturator --formats vst3

> **Binary name matters on Windows.** Windows' UAC *installer detection*
> heuristic forces an elevation prompt on unmanifested executables whose name
> contains `install`/`setup`/`update`/`patch`. The shipped binary is therefore
> named `tatsunari` (no such substring) so the base process runs unelevated;
> only the `__apply` re-launch elevates. `go run .` builds a temp `installer.exe`
> and *will* prompt — build to a neutral name to test locally.

## How elevation works

The unprivileged process stages every download+extract into a `0700` temp dir,
writes a `plan.json`, then invokes itself **once** as the hidden `__apply`
subcommand under the OS elevation mechanism (macOS `osascript … with
administrator privileges`; Windows `Start-Process -Verb RunAs`). `__apply`
re-validates every destination against an install-root allowlist and every
source against the staging dir before moving anything, so a tampered plan can't
turn elevation into an arbitrary privileged write. The **receipt is always
written by the unprivileged parent**, never as root, so later per-user updates
keep working.

Per-user scope skips all of this and applies in-process (no prompt).

## Release integration

`.github/workflows/installer.yml` runs on `release: published` (and manual
dispatch), cross-compiles `tatsunari-{darwin-amd64,darwin-arm64,windows-amd64}`
with `CGO_ENABLED=0`, generates `catalog.json` via
`tools/gen_catalog.py --emit-json`, and uploads all four to the release. It does
**not** modify `release.yml` or `manifest.json`.

## Manual smoke test (per OS)

1. **Windows** clean VM: run the `irm … | iex` one-liner. Do a per-user install
   (0 prompts), then a system install (exactly one UAC). Confirm the `.vst3`
   lands in `C:\Program Files\Common Files\VST3\tatsunari`.
2. **macOS** Intel + Apple Silicon: run the `curl … | bash` one-liner. Per-user
   then system (exactly one auth dialog). Confirm the `.vst3` / `.component`
   land under `~/Library` or `/Library`, and the DAW rescans.
3. Re-run → rows show **up to date**. Edit the receipt version down
   (`~/Library/Application Support/tatsunari-plugins/receipt.json` /
   `%APPDATA%\tatsunari-plugins\receipt.json`) → the row shows **↑ update** →
   run → confirm the bundle is replaced.
4. Confirm the `curl | bash` / `irm | iex` path accepts keyboard input (the TTY
   reattach in `tty_*.go`).
