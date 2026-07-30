# 3. `/updates/v1/` スキーマ設計

### 3.1 ファイル構成

用途で 2 本に分ける。バッジは数百バイトで済み、CDN 負荷も最小になる。

```
/updates/v1/
  latest.json                       バッジ専用。極小。TTL 60s + ETag
  latest.json.minisig
  catalog.json                      インストーラ用。全チャンネル・全バージョン
  catalog.json.minisig
  channels/{stable,beta,dev}.json   catalog を分割する場合
/artifacts/<slug>/<version>/<file>.zip   不変・長期 immutable
```

### 3.2 バッジ用 `latest.json`

| フィールド | 型 | 必須 | 用途 |
|---|---|---|---|
| `schema` | int | ✓ | `1` 固定 |
| `generated` | ISO8601 | ✓ | デバッグ用 |
| `plugins[].slug` | string | ✓ | 安定 ID |
| `plugins[].latest` | semver | ✓ | 最新安定版 |
| `plugins[].highlights[]` | string[] | | **主な変更内容の要約（3 行程度、多言語）**。バッジのダイアログに表示する |
| `plugins[].changelogUrl` | string | | 詳細への導線 |

意図的に入れないもの: 長文の説明、アセット一覧、URL 一覧。`catalog.json` が持つ
完全な changelog とは別に、**要約だけ**を持たせて payload を膨らませない。

> `highlights[]` は当初の設計には無かった。バッジのダイアログで「主な変更内容」を
> 表示する決定（4.3）に伴う追加。

### 3.3 インストーラ用 `catalog.json`

#### エンベロープ

| フィールド | 必須 | 説明 |
|---|---|---|
| `schema` | ✓ | `1` 固定。以後変えない |
| `generated` | ✓ | 生成時刻 |
| `generator` | | 生成ツール名 + バージョン（障害切り分け用） |
| `minClientVersion` | | 全体の下限。**原則使わない**（不変条件 2 を壊すため） |
| `ttlHint` | | クライアント側キャッシュ秒数の助言 |
| `channels[]` | ✓ | チャンネル定義 |
| `client` | | インストーラ自身の最新版情報 |
| `plugins[]` | ✓ | プラグイン定義 |
| `notice` | | 全体告知（任意・閉じられること） |

#### `channels[]`

| フィールド | 説明 |
|---|---|
| `id` | `stable` / `beta` / `dev` |
| `name` | 表示名（多言語） |
| `description` | 説明（多言語） |
| `requiresOptIn` | dev は true。UI で警告を出す |
| `retention` | 保持世代数（dev = 5） |

#### `client`

| フィールド | 必須 | 説明 |
|---|---|---|
| `latest` | ✓ | 最新インストーラのバージョン |
| `assets[]` | ✓ | OS / arch ごとの実体（下記） |
| `changelogUrl` | | 変更履歴 |
| `minSupported` | | これ未満は動作保証外（**表示のみ。ブロックしない**） |

`client.assets[]`:

| フィールド | 必須 | 説明 |
|---|---|---|
| `os` | ✓ | `macos` / `windows` |
| `arch` | ✓ | `arm64` / `amd64` |
| `url` | ✓ | `/artifacts/installer/<version>/tatsunari-<os>-<arch>` |
| `size` | ✓ | バイト数 |
| `sha256` | ✓ | ブートストラップが検証に使う |

> **`url` 単一フィールドでは足りない**（Codex レビュー P1 指摘）。インストーラは
> `tatsunari-<os>-<arch>` の複数バイナリとして配信される（5.1）ため、ブートストラップが
> 対象を選べる構造が必要。加えて、5.1 の信頼モデルは「スクリプトが SHA-256 で検証」を
> 前提にしているので、**ハッシュがスキーマ上に存在しなければその検証が成立しない**。
> `/updates/v1/` を凍結する前にこの形にすること。

> 不変条件 3 により、`latest` / `minSupported` は**閉じられる通知の材料**にしか
> 使わない（強制更新の判断には使わない）。

#### `plugins[]`

| フィールド | 必須 | 説明 |
|---|---|---|
| `slug` | ✓ | 安定 ID。`plugin.toml` の slug |
| `variant` | ✓ | `stable` / `dev`。開発版共存の表現の鍵 |
| `name` | ✓ | 表示名（多言語オブジェクト、`en` 必須） |
| `displayNameSuffix` | | dev variant の ` (Dev)` |
| `shortDescription` / `description` | | 多言語 |
| `category` | ✓ | `plugin.toml` 由来 |
| `vendor` | ✓ | |
| `iconUrl` / `screenshotUrls[]` | | GUI 用 |
| `homepageUrl` / `manualUrl` | | |
| `pluginIds` | ✓ | `{clapId, vst3Uid, auSubtype, auManufacturer}`。競合検出と dev 共存に必須 |
| `latest` | ✓ | このチャンネルでの最新版 |
| `versions[]` | ✓ | バージョン履歴 |
| `deprecated` / `replacedBy` | | アーカイブ済みプラグインの表現 |
| `minClientVersion` | | **プラグイン単位**の下限。不変条件 2 の行単位劣化はここで実現 |
| `tags[]` | | 検索・絞り込み |

#### `versions[]`

| フィールド | 必須 | 説明 |
|---|---|---|
| `version` | ✓ | semver |
| `channel` | ✓ | 所属チャンネル |
| `releasedAt` | ✓ | |
| `changelog` / `changelogUrl` | | 多言語 |
| `stateCompatVersion` | ✓ | **重要**。プリセット/state フォーマットの世代。ダウングレード警告を semver major の推測ではなく正確に判定できる |
| `minOsVersion` | | `{macos, windows}` |
| `yanked` / `yankedReason` | ✓相当 | **重要**。不良リリースをオブジェクト削除せず一覧から外せる。immutable 配信での安全弁 |
| `supersededBy` | | 推奨移行先 |
| `knownIssues` | | 多言語 |
| `assets[]` | ✓ | 実体 |

#### `assets[]`

| フィールド | 必須 | 説明 |
|---|---|---|
| `format` | ✓ | `vst3` / `au` / `clap`。**未知は行ごとスキップ** |
| `os` | ✓ | `macos` / `windows` |
| `arch` | ✓ | `universal` / `x86_64` / `arm64` |
| `url` | ✓ | 不変オブジェクトへの絶対 URL |
| `size` | ✓ | バイト数。HEAD なしで進捗バーが出せる |
| `sha256` | ✓ | 検証用。マニフェスト署名と鎖で繋ぐ |
| `subpath` | ✓ | ルートからの相対配置先。配置先データ駆動化の鍵 |
| `bundleName` | ✓ | `Resonance TatSuppressor.vst3` 等。検出・アンインストールに必要 |
| `archiveRoot` | | zip 内のルートディレクトリ（剥がす階層） |
| `compression` | | 現状 `zip` 固定。将来のため |
| `notarized` / `signed` | | 情報表示用 |

### 3.4 絶対に入れてはいけないフィールド

将来も追加しないと決めておく。

- `postInstallScript` / `preInstallCommand` などの**実行可能フック**
  （不変条件 4 に違反。署名済みインストーラを任意コード実行の踏み台にする）
- `absolutePath` / `installRoot` などの**絶対パス指定**（不変条件 5 に違反）
- テレメトリ送信先 URL（不変条件 9）
- クライアントの挙動を変えるリモート設定フラグ

### 3.5 `reference` フィールド

`plugins/<slug>/plugin.toml` の `reference` は内部の設計意図であり、ユーザー向け面に
出すものではない。値の一般名称化（§1.8）は完了済み。**新スキーマでは `reference` を
公開カタログからフィールドごと除外する**（parser は出現時にその plugin 行を拒否）。
過去の GitHub Release 添付 `catalog.json` に残る値は、R2 移行後に陳腐化する。

---

## wire format の確定仕様

- JSON は UTF-8、時刻は UTC の RFC 3339、サイズは非負整数、SHA-256 は小文字 64 桁。
- SemVer は `MAJOR.MINOR.PATCH` を必須とし、stable では prerelease/build metadata を拒否する。
- URL は HTTPS の絶対 URLとし、許可済み独自 host のみ。redirect 後も host を再検証する。
- `slug`、channel、variant、format、OS、arch は ASCII の列挙値として比較する。
- 多言語値は `en` を必須 fallback とし、未知 locale は無視する。
- 重複する `(slug, variant)`、version、asset `(format, os, arch)` は manifest 全体の生成エラー。
- クライアントは未知フィールドを無視する一方、既知フィールドの型不正はその plugin または
  asset 行だけを不正として扱う。エンベロープ必須値の不正だけを全体エラーにする。

## 署名・取得プロトコル

1. JSON と `.minisig` を同じ cache generation で取得する。
2. サイズ上限を適用してメモリへ読み、埋め込み済み active/backup 公開鍵の `keyId` と照合する。
3. 署名成功後にだけ parse する。署名対象は配信された JSON byte列そのもの。
4. 検証済み JSON、署名、ETag、取得時刻を atomic rename でキャッシュする。
5. ネットワーク失敗時は TTL 内の検証済み cache のみ使用し、未検証データへ fallback しない。

推奨上限は `latest.json` 256 KiB、`catalog.json` 8 MiB、changelog 1 項目 64 KiB。
上限値は定数化し、fixture で境界値を試験する。

## 生成・公開の原子性

artifact を先に全件アップロードして HEAD/size/hash を照合し、次に署名済み catalog、最後に
latest pointer の順で公開する。失敗時は pointer を更新しない。ロールバックは以前に署名した
pointer を再配置し、不変 artifact を削除しない。

## 必須 fixture

- v1 最小 / 全フィールド、未知トップレベル値、未知 format、未来 client 要求。
- OS/arch ごとの `client.assets[]` と SHA 不一致。
- yanked、stable/dev 同居、同一 slug の両 variant、5 世代を超える dev。
- traversal、絶対パス、重複キー、巨大 payload、不正 UTF-8、不正署名、backup 鍵署名。

JSON Schema、生成器の validation、Go parser の fixture test の 3 層が同じデータを通過する
ことを受け入れ条件とする。
