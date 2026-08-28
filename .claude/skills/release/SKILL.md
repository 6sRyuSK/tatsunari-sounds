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
- **bump は PR 作成時に 1 回だけ**: ブランチ作業中は version を baseline のまま
  触らず、PR を開くときに合意した semver へ一度で上げる(プラグイン PR は
  squash-merge なので途中の bump は main に残らないノイズ)。
  **bump 忘れ = そのプラグインはリリース対象外**になるので、PR 前に必ず確認。
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

公開後、`installer.yml` が **`workflow_run`(Release workflow の completed)** で
自動起動し、TUI インストーラのバイナリと `catalog.json` を同じ Release に追加する
(release.yml や manifest.json には触らない)。`release: published` トリガーでは
**ない**: GITHUB_TOKEN が公開した Release にはそのイベントが発火しない(再帰実行
防止)ため。自動起動が失敗/欠落したときのバックフィルは
`gh workflow run installer --ref main --field tag=<year>.<n>`(workflow_dispatch が
そのために残してある)。インストーラ側の開発は `tools/installer/` の自己完結 Go
module(`go test ./...` / `go vet ./...`、ゲートは installer-ci.yml)。

## ビルドの形(release.yml が何を作るか)

- release ビルドは **`-DFACTORY_JUCE_ORACLES=OFF`**(出荷パスのみ。JUCE は fetch
  されない)。したがって**リリース実行時に等価/プリセットオラクルは走らない** —
  それらは main の ci.yml で緑になっている前提。
- **全エントリの kind は `clap`**。`tools/release_plan.py` は
  `factory_clap_plugin(...)` だけを認識し、**シェルに `juce_add_plugin` があると
  hard error** で落ちる(release.yml は make_clapfirst のアセット配置しか
  パッケージできないため)。
- アセットは `build/<slug>_assets/`。zip に入るのは **VST3 + AU のみで、native
  `.clap` は入らない**(インストーラ側の対応待ち)。macOS は AU と VST3 を別 zip、
  Windows は VST3。
- `tools/release_plan.py` を触ったら `python -m unittest discover tools/tests` を
  回す(ゲートは factory-tools-ci.yml)。

## リリース準備チェックリスト

1. 出荷対象の `plugin.toml` version が変更内容に対して正しく bump 済みか
   (bump は PR 作成時に 1 回 — マージ済み変更に bump が漏れていないか確認)。
2. `python tools/gen_catalog.py --check` が緑。
3. main で **4 つの CI ワークフロー**が緑:
   - `ci.yml` — macOS/Windows ビルド + CTest 全レート + pluginval strictness 5
     (wrapper VST3 / AU)。**CTest を回す唯一のワークフロー**
   - `clap.yml` — active 3 機種それぞれの Linux レグで clap-first ビルド +
     **native `.clap` の clap-validator**(ci.yml が出さない唯一のシグナル)
   - `factory-tools-ci.yml` — `gen_catalog.py --check` + skill/ID/旧 identity gate
     + `tools/promote/bootstrap.py --check` + `tools/tests` + `tools/worker` の
     `npm test`(node --test)
   - `installer-ci.yml` — `go test` / `go vet`(`tools/installer/**` 変更時のみ)
4. 出荷対象に **`docs/manual/<name>.md`** があり、今回のパラメータ/プリセット変更に
   追従しているか(`docs/manual/README.md` の宣言どおり出荷バイナリに追従させる)。
5. リリースノート素材: 前回 manifest.json との version 遷移を列挙
   (`mcp__github__get_latest_release` → manifest.json 参照)。
6. ここまで揃えて**人間に実行可否を確認**。指示があれば Actions の
   `Release` workflow を workflow_dispatch(GitHub MCP: `actions_run_trigger`)。
7. 実行後: release.yml → installer.yml の 2 段が両方成功し、アセット一式
   (per-OS バンドル、per-plugin zip、manifest.json、SHA256SUMS.txt、
   installer バイナリ、catalog.json)が揃ったことを確認して報告。

## Cloudflare 配信パイプライン(updates/v1) — Phase F

GitHub Releases を廃し、`https://6sryusk.com/tatsunarisounds/` から配信する経路。
**まだ本番稼働していない**(計画 §8 Phase F)。上の release.yml/installer.yml が
現行の出荷経路で、こちらは移行先。手を入れるときの地図:

| 物 | 役割 |
|---|---|
| `tools/promote/promote.py` | 8 ステップの publish と pointer rollback |
| `tools/promote/manifest.py` | `latest.json` / `catalog.json` の生成(`ZIP_RE` / `TARGETS`) |
| `tools/promote/store.py` | オブジェクトストア抽象 + キャッシュポリシー定数 |
| `tools/promote/signing.py` | minisign の署名/検証の**配線のみ**(鍵は人間) |
| `tools/promote/cdn_check.py` | エッジが返すヘッダの検査 |
| `tools/promote/bootstrap.py` | shim → payload の SHA-256 pin 生成/検査 |
| `tools/promote/rehearsal.py` | 実 zip が無いときの placeholder 生成（演習専用） |
| `tools/worker/` | 配信 Worker(R2 中継 + DL 集計)。`npm test` で gate |
| `docs/runbooks/production-rollout.md` | 演習 4 種と**リリース判定チェックリスト** |

守るべき性質(緩めるなら人間に確認):

- **immutable は上書きしない**。同じ key に別 digest が来たら promote は止まる。
- **read-back verify は省略しない**。upload 成功と取得可能は別の主張。
- **smoke は公開の前**に通す。TUI インストーラは `catalog.json` を直接読むので、
  「pointer を最後に切り替える」だけでは catalog を守れない。
- **pointer 切替は最後、かつ可逆**。旧 pointer を history に退避してから切り替える。
- **pointer は短 TTL + ETag、artifact は immutable**。逆にすると rollback が効かない。
- **署名は公開の前に作って公開鍵で検証し、文書より先にアップロードする**。
  文書が先だと、新文書 + 旧署名という不整合が「窓」ではなく公開状態として残りうる。
- **catalog の `client` を落とさない**。bootstrap ワンライナーはここから
  インストーラ本体を解決する。`--installer-dir` を渡すか前回 catalog から引き継ぐ。
- **`get()` の失敗を「存在しない」と混同しない**。auth/timeout/5xx を 404 と同じ
  シグナルにすると、読み取り障害の直後に旧 pointer を退避せず上書きしてしまう。
- **秘密鍵に触れない**。生成・表示・保存はすべて人間の手順(runbook §3)。

`--store memory` は本番と同一コードパスの完全なリハーサル。演習で落ちるものは
本番でも落ちる。
