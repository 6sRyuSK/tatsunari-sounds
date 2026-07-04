---
name: release
description: Prepare or run a release of the plugin factory (version bumps, catalog, release.yml / installer.yml mechanics, manifest carry-over). Use for any release/ship/version-bump/tag work in this repo — contains how the pipeline works so you don't need to read the workflow files. Dispatching a release itself requires explicit human authorization.
---

# リリース作業

**大原則(Ask a human)**: リリースの実行(workflow_dispatch)・署名・公証など
ユーザーに届く操作は人間の明示的な指示があるときだけ。エージェントが自律で
やってよいのは「準備」(version bump、カタログ、ノート案)まで。

## 出荷のトリガーは version bump

- `plugins/<slug>/plugin.toml` の `version` が**唯一の真実**。リリース実行時、
  前回リリースの `manifest.json` と比較して **version が変わったプラグインだけ**
  再ビルドされ、未変更のものは前回アセットをそのまま引き継ぐ(carry-over)。
  → 出荷したいプラグインの version を上げることがトリガー。
- semver: P0/P1 修正 → patch / 新機能・新パラメータ → minor /
  state・preset 互換を壊す → major。
- **bump は対象の変更と同一コミット**(コミット規約: `feat(<slug>): 日本語説明`)。
- bump 後 `python tools/gen_catalog.py` で README カタログ再生成
  (CI は `--check` で staleness を落とす)。

## パイプラインの仕組み(読む必要が出たときの地図)

`.github/workflows/release.yml`(手動 `workflow_dispatch` のみ、tag は `<year>.<n>`
例 `2026.1` — リリース回数由来でプラグイン semver とは独立):

1. **plan** — 全 plugin.toml の version を前回リリースの manifest.json と比較、
   変更リストを作る。
2. **build** — 変更されたプラグインだけを**ネイティブ OS 上で**ビルド・zip
   (macOS は AU と VST3 を別 zip、Windows は VST3)。
3. **package** — OS ごとに、今回ビルド分 + 前回からの carry-over 分を集めて
   「全部入り」バンドル(macOS-AU / macOS-VST3 / Windows、フラット構造)を組む。
   macOS バンドルを Linux で解凍/再圧縮しない設計。
4. **release** — 1 つの Release に全アセット + `manifest.json`(slug→version、
   次回 plan の基準)+ `SHA256SUMS.txt` + 変更プラグインごとのノートを公開。

アセット名の version トークンは `v<major>_<minor>_<patch>`(tag は `v<year>_<n>`)
— `.` と `-` の衝突回避。

公開後、`installer.yml` が `release: published` で自動起動し、TUI インストーラの
バイナリと `catalog.json` を同じ Release に追加する(release.yml や manifest.json
には触らない)。インストーラ側の開発は `tools/installer/` の自己完結 Go module
(`go test ./...` / `go vet ./...`、ゲートは installer-ci.yml)。

## リリース準備チェックリスト

1. 出荷対象の `plugin.toml` version が変更内容に対して正しく bump 済みか
   (同一コミット規約も確認)。
2. `python tools/gen_catalog.py --check` が緑。
3. CI(ci.yml: macOS/Windows ビルド + CTest 全レート + pluginval strictness 5)
   が main で緑。
4. リリースノート素材: 前回 manifest.json との version 遷移を列挙
   (`mcp__github__get_latest_release` → manifest.json 参照)。
5. ここまで揃えて**人間に実行可否を確認**。指示があれば Actions の
   `Release` workflow を workflow_dispatch(GitHub MCP: `actions_run_trigger`)。
6. 実行後: release.yml → installer.yml の 2 段が両方成功し、アセット一式
   (per-OS バンドル、per-plugin zip、manifest.json、SHA256SUMS.txt、
   installer バイナリ、catalog.json)が揃ったことを確認して報告。
