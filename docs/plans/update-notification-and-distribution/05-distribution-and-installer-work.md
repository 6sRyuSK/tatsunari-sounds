# 5. 決定に伴う波及作業

### 5.1 GitHub Releases 完全廃止

- **`release.yml` の全面書き換え** — Release 作成ではなく R2 へのアップロードに。
- **`installer.yml` の前提が崩れる** — `workflow_run` で Release 完了を待つ構造、
  `gh release view` によるタグ解決が無効になる。
- **`tools/release_plan.py` の差分判定** — 「前回リリースの `manifest.json`」を
  GitHub ではなく R2 から取得するよう変更。
- **bootstrap スクリプトの移転** — 現在 `raw.githubusercontent.com` 配信。
  CDN ドメインへ。
- **リリースノートの置き場所** — GitHub Releases の UI が無くなるため代替が要る。
  **決定**: repo にソースを置き（`plugins/<slug>/CHANGELOG.md`）、リリース時に CDN へ
  publish（`/notes/<slug>/<version>.md`）。`catalog.json` の `changelogUrl` がここを
  指す。**git が真実の源、CDN は配信**という関係を崩さない。GitHub Releases のノートは
  失うが、`CHANGELOG.md` が repo に残るので追跡性はむしろ上がる。
- **git tag は残す**（無料、追跡性が保てる。「Release を作らない」だけでよい）。

#### 配信対象は 3 種類すべて

プラグイン成果物だけでなく、**インストーラのバイナリとブートストラップスクリプトも
Cloudflare 配信**になる。

| 対象 | 配置 | キャッシュ |
|---|---|---|
| プラグイン成果物 | `/artifacts/<slug>/<version>/…` | 不変・長期 immutable |
| インストーラバイナリ | `/artifacts/installer/<version>/tatsunari-<os>-<arch>` | 不変・長期 immutable |
| `latest.json` / `catalog.json` | `/updates/v1/` | 短 TTL 60s + ETag |
| **ブートストラップスクリプト** | 例 `/install.sh` / `/install.ps1` | **短 TTL + ETag** |

インストーラバイナリの発見は `catalog.json` の `client.url`（3.3）で既に表現できる。

#### 利点: GitHub API のレート制限から解放される

現行の `install.sh` 34 行目は `https://api.github.com/repos/…/releases/latest` を
叩いており、**未認証で 60 req/h/IP** の制限を受ける（スタジオや企業の NAT 配下で
詰まりうる）。静的 JSON への移行でこれが消える。

#### ★ ブートストラップスクリプトは 3 つ目の「可変オブジェクト」

不変条件 10 は「成果物は不変、可変なのはポインタ JSON のみ」としているが、
**ブートストラップスクリプトも可変**になる。エントリポイントとして常に最新である
必要があるため、`latest.json` と同様に短 TTL + ETag で扱うこと。長期キャッシュに
乗せると古いスクリプトが CDN に居座る。

#### ★ ブートストラップだけは署名で守れない（制約として記録）

minisign（1.3）はマニフェストと成果物を守るが、**`curl | bash` で取得するスクリプト
自体は検証しようがない**。GitHub Releases を廃止すると「別経路での確認」も無くなる
ため、ここの信頼は **HTTPS + 自分の CDN** に完全に依存する。

```
HTTPS のみで守られる  → bootstrap スクリプト
      ↓ スクリプトが SHA256 で検証
署名で守られる        → インストーラバイナリ、プラグイン成果物
```

循環構造に見えるが、`curl | bash` 方式では原理的にこれ以上は詰められない
（公開鍵の帯域外配布が必要になる）。公開鍵をバイナリに埋め込む設計は維持しつつ、
この一点は**受け入れる制約**として記録する。

### 5.2 DL 集計 Worker

**実装方式は Workers Analytics Engine。** カウント用途に設計されており、Worker 1 本で
済み、書き込みを fire-and-forget にできるのでダウンロードの遅延にならない。

- 採用しなかった案: KV カウンタ（eventual consistency で競合しやすい）、
  Durable Objects（正確だがコストと複雑さ）、Logpush で生ログ保持
  （プライバシー方針と相性が悪い）。
- **記録する項目**: slug / version / os / format / arch / channel / 日付。
- **記録しない項目**: **IP・User-Agent・任意の識別子**（不変条件 9）。
- 集計はダウンロード経路で行い、**バッジのチェック経路では何も記録しない**。
- インストーラバイナリのダウンロードも集計対象に含めるかは実装時に決める
  （プラグインの DL 数と混ぜないこと）。

### 5.3 CI の組み替え — **不要**

GUI を見送り、公証も行わないため、**インストーラの CI は現行のまま変更不要**。

- CGO 不要 → 「ubuntu から `CGO_ENABLED=0` で全 OS クロスビルド」を維持。
- Apple のシークレット・`notarytool`・`stapler`・`.dmg` 作成いずれも不要。
- macOS / Windows ランナーの新設も不要。

`installer.yml` に手が入るのは 5.1（GitHub Releases 廃止に伴う配信先の変更）と
5.4（自己インストール）の都合のみ。

### 5.4 インストーラの自己インストール（新規）

4.3 のとおり、ブートストラップが一時ディレクトリにバイナリを落として捨てる
現状のままでは、プラグインからの起動導線が成立しない。
`curl | bash` / `irm | iex` が**単体バイナリを正規パス（全ユーザー）に配置する**
よう変更する。

#### ★ 配置を行う主体（Codex レビュー P1 指摘への対応）

**ブートストラップだけでは配置先を決められない。** スコープはダウンロード済みバイナリ
内の TUI で後から選ばれるため、ブートストラップが走る時点では行き先が未定である。

したがって **配置は apply ステップの一部として行う**。ブートストラップの役割は
「一時ディレクトリに落として起動する」までで変えない。

| ユーザーが選ぶスコープ | 配置の実行主体 |
|---|---|
| システム | **`__apply` の計画（`plan.json`）に実行中バイナリのコピーを含める**。特権側が他の成果物と同時に移す |
| ユーザー | **非特権の親プロセスがその場でコピーする**（昇格不要） |

「起動時に毎回自己複製する方式」は採用しない（どんな入手経路でも効くが、ワンライナー
導線でほぼカバーできるため過剰）。ここで採るのは**あくまで apply の一部としての配置**
であり、両者は別物。

> この区別を書いていなかったため、初版のメモは「配置先はスコープに追従する」と
> 「自己複製は不採用」を並べて**配置を実行する主体が存在しない**状態になっていた。

#### 追加コストはほぼゼロ

`install.sh` は 34 行目で `https://api.github.com/repos/…/releases/latest` を
叩いており、**GitHub Releases 廃止（5.1）の時点で取得部分は書き直し確定**。
自己インストールの差分は実質「配置先を `mktemp -d` から正規パスに変える」だけ。

現行の該当部分（`install.sh` 44〜50 行）:

```sh
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
bin="${tmp}/tatsunari"
curl -fsSL "$url" -o "$bin"
chmod +x "$bin"
```

#### 配置先はスコープに追従する

**インストーラ自身の配置先は、ユーザーが選んだインストールスコープに追従する。**

| ユーザーが選ぶスコープ | インストーラの配置先 | 昇格 | パスワード入力 |
|---|---|---|---|
| システム | `/Library/…` / `%ProgramFiles%` | `__apply` にまとめる | **1 回**（プラグイン導入と同時） |
| ユーザー | `~/Library/…` / `%LOCALAPPDATA%` | 不要 | **0 回** |

これにより:

- **昇格ポイントが増えない。** 特権コードは `__apply` の 1 箇所に閉じ込めたまま。
- **「パスワードを一度も入力せずに導入できる」性質が維持される。** 常駐なし・
  テレメトリなしという方針と一貫する。
- ブートストラップがいきなり管理者パスワードを要求する形にならない。

**採用しなかった案**: インストーラを常に全ユーザー配置にする案。ユーザースコープを
選んだ場合でもパスワードが 1 回必要になり、かつ**その場合 `__apply` の昇格が発生
しないためまとめる先が存在せず**、専用の昇格経路を増やすことになる。

#### 注意点

- `__apply` のインストールルート検証（不変条件 5）の許可リストに、インストーラ
  自身の配置先を**明示的に加える**こと。
- 受領書の扱いは 5.5 を参照（**受領書のスコープもインストールのスコープに追従
  する**）。

### 5.5 受領書のスコープ（新規）

#### 問題: 受領書がユーザーごとなので、別ユーザーが更新状態を見られない

受領書は `internal/install/receipt.go:41` の `ConfigDir()` で決まり、**完全に
ユーザーごと**である。

- macOS: `~/Library/Application Support/tatsunari-sounds/receipt.json`
- Windows: `%APPDATA%\tatsunari-sounds\receipt.json`

そしてこれが `Reconcile()`（`internal/release/reconcile.go`）の
`installed map[string]string` を埋めている。

したがって **ユーザー A がシステムスコープでプラグインを入れた後、同じマシンの
ユーザー B が TUI を起動すると、B には受領書が無いため「何も入っていない」と
判定される**。

| | ユーザー A | ユーザー B |
|---|---|---|
| `↑ update` バッジ | 出る | **出ない** |
| `up to date` 表示 | 出る | **出ない** |
| 全行の表示 | 正しい | **全部 `NEW` に見える** |
| 更新のある行の事前選択 | 効く | **効かない** |

「更新のある行だけ 1 キーで更新」という最大の売りが B では機能しない。上書き
インストールになるだけで壊れはしないが、体験は明確に劣る。

> これは**インストーラのバイナリをどこに置いても発生する**別問題。配置先の議論
> （5.4）とは独立している。

#### 決定: 受領書のスコープをインストールのスコープに追従させる

| インストールのスコープ | 受領書の位置 | 書き手 | 権限 |
|---|---|---|---|
| システム | `/Library/Application Support/tatsunari-sounds/receipt.json`<br>`%ProgramData%\tatsunari-sounds\receipt.json` | `__apply`（特権側） | **全ユーザー読み取り可 / root のみ書き込み可** |
| ユーザー | 現状のまま `~/…` / `%APPDATA%\…` | 非特権の親 | ユーザーのみ |

配置先の決め方（スコープがルートを決める）と同じ構造なので一貫する。

**README の「受領書を root で書かない」という原則との関係**: あの原則の理由は
「後の per-user 更新が権限エラーで壊れないように」であり、**ユーザー受領書**を
守るためのもの。システムスコープの導入記録を別ファイルとしてシステム側に置くのは
趣旨に反しない（システムプラグインの変更にはどうせ昇格が要る）。
**ユーザー受領書を root 所有にしてはならない**という点は維持する。

#### ★ 受領書のキーを (slug, variant, scope) にする（Codex レビュー P2 指摘への対応）

現在の受領書と `Reconcile()` の `installed` は **slug だけをキー**にしている
（`map[string]string`）。この設計には衝突が 2 系統ある。

| 衝突 | 例 |
|---|---|
| **スコープ間** | 同じプラグインがシステムとユーザーの両方に入っている場合、マージで片方のバージョンが失われる |
| **variant 間** | stable と dev は**同じ slug** を共有する（1.6）ので、slug キーでは区別できない |

どちらも「更新・ダウングレード判定が別の実体を参照する」ことにつながる。したがって:

- **受領書の識別子を `(slug, variant, scope)` にする。** `InstalledVersions()` の
  戻り値も slug キーの map から、この 3 つ組をキーにした形へ変える。
- **TUI は 3 つ組ごとに行を出す。** 同じプラグインがシステムとユーザーの両方に
  入っていれば 2 行になる。
- **更新対象は、その行のスコープに対して適用する。** 暗黙の優先順位で片方を選ばない。
- **同一 (slug, variant) が両スコープに存在する場合は警告を出す。** ホストからは
  重複プラグインとして見えるため、それ自体が知らせるべき状態である。

読み取り時は両方の受領書を読み、**マージではなく 3 つ組で結合**する。

#### 将来の頑健化（未実装・任意）

インストール済みバンドルから**ディスク上の実バージョンを直接読む**
（macOS は `Info.plist` の `CFBundleShortVersionString`、Windows VST3 は
`moduleinfo.json` / VERSIONINFO）。これにより:

- 受領書が無い / 壊れた / 手で消された場合でも正しい状態が出る
- 手動でプラグインを削除したケースも拾える
- 別ユーザー問題も自動的に解決する

工数が大きいため今回はスコープ外。上記の受領書スコープ追従で当面は足りる。

---

## 配信パイプライン仕様

### build

タグ/手動 dispatch から再現可能な成果物を生成し、各 asset の SHA-256、size、SBOM、build
provenance をジョブ成果物として集約する。dev は保持 5 件の catalog 表示対象とするが、R2 の
削除は別の承認済み lifecycle とし、公開中 pointer が参照する object を削除しない。

### promote

`validate → upload immutable assets → read-back verify → generate manifest → sign → upload manifest
→ smoke test → pointer switch` の順にする。本番環境の promote と rollback は GitHub Environment
の人間承認を必須とし、PR から secrets を読めないようにする。object key が既存なら内容が同じ
hash の場合だけ成功扱いにし、上書きしない。

### bootstrap

POSIX shell と PowerShell は OS/arch を明示的に対応表へ変換し、未対応環境では取得前に終了
する。catalog の署名を埋め込み公開鍵で検証できない初回問題があるため、bootstrap 自体には
固定 URL と期待 SHA-256 を持つ版付きスクリプトを用意し、短縮ワンライナーはその不変版へ
誘導する。redirect、TLS エラー、hash 不一致では実行しない。

### Worker 集計

Worker は artifact key の allowlist 検証後に R2 へ中継し、IP、Cookie、query の永続保存を
行わない。集計キーは日付、asset key、HTTP status とし、bot 除外は集計時の規則として文書化
する。Range/HEAD を正しく転送し、失敗応答を成功数に含めない。Worker 障害時に静的 CDN URL
へ切り替えられることを staging で確認する。

## 受領書仕様

受領書は schema version、`slug, variant, scope, version`、format ごとの destination と hash、
install timestamp、installer version を持つ。system と user を別 root から読み、UI では両方を
別行としてマージする。破損受領書は「unknown install」として表示し、削除や上書きを自動で
決めない。書き込みは temp + fsync + atomic rename とする。

## 運用 Runbook と完了条件

- staging で promote、pointer rollback、active 鍵失効→backup 鍵署名を演習する。
- CDN header（manifest は短 TTL + ETag、artifact は immutable）を自動検査する。
- GitHub Releases を廃止する前に旧 URL の利用期間と案内方法を人間が承認する。
- macOS arm64 の ad-hoc signature を `codesign` で確認し、Windows/macOS の実機 smoke test を残す。
