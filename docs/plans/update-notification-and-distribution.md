# 配信・更新基盤 設計メモ（2026-07）

対象: プラグインの更新通知、バイナリ配信、コード署名/公証、GUI パッケージ
マネージャ、PR プレビュー。

**このドキュメントは「設計メモ」であって実装プランではない。** 決定済み事項と
設計不変条件を固定し、未決定事項を選択肢つきで残すことが目的。実装に入る前に、
本メモの「未決定事項」を潰したうえで、フェーズごとに別途スコープを切った実装
プランを起こすこと。

CLAUDE.md の全ルール（リアルタイム安全、検証哲学、スコープ規律、Ask a human）が
前提。特に **「出荷・署名・公証はすべて Ask a human」** に該当する領域なので、
実作業の着手には人間の承認が要る。

---

## 0. 背景と動機

UA Connect / Waves Central 型の「常駐アプリが勝手に更新する」体験を明確に避け
たい。実際の不満として挙がっているのは 2 点:

1. アプリが常駐し、意図しないタイミングで動く。
2. 久しぶりにプラグインの更新を確認しようとすると、**先にパッケージマネージャ
   自身の更新が入る**。

したがって本設計の中心命題は「**通知と実行を分離し、パッケージマネージャ自身は
極力更新しない**」となる。

---

## 1. 決定済み事項

### 1.1 全体方針

- **常駐アプリを置かない。**
- 役割分担: **通知はプラグイン内バッジ / 更新の実行はユーザーが任意に起動する
  パッケージマネージャ**。

### 1.2 更新通知（プラグイン内バッジ）

- Visage エディタ内に受動的なバッジを表示する。新バージョンがあるときのみ。
- **バージョン情報の取得のみ**を行う。ダウンロードもインストールもしない。
- エディタを開いている間だけ動作する（常駐プロセスなし）。
- チェック頻度は **1 日 1 回**。
- 有効化は **初回に 1 回だけ確認ダイアログ（opt-in）**。「送信するデータはあり
  ません（単なる GET です）」と明記する。
- HTTP クライアントは自前の薄いラッパ:
  macOS = `NSURLSession`（`.mm` 1 ファイル）、Windows = WinHTTP。
  外部ライブラリ依存を増やさない。

#### バッジ → ダイアログ → GUI インストーラ起動

バッジをクリックするとエディタ内にダイアログを表示する。内容:

- 最新バージョン
- 現在インストールされているバージョン
- 主な変更内容（要約）
- 「アップデート」ボタン → **GUI インストーラを起動する**

詳細な設計・制約は 4.3 を参照。

### 1.3 バイナリ配信

- **GitHub Releases から Cloudflare R2 + CDN へ全面移行**（Cloudflare Paid 契約
  済み）。GitHub Releases は**完全廃止**する。
- **ビルドは GitHub Actions を継続。** Cloudflare の CI/CD（Workers Builds /
  Pages ビルド）は Cloudflare へのデプロイ用であり、macOS/Windows ランナーも
  Apple ツールチェーンも持たないため代替にならない。
- 構成: `Actions でビルド` → `R2 へアップロード` → `CDN で配信`。
- ドメインは **単一ドメイン + 用途別サブドメイン**（`cdn.` / `updates.` /
  `preview.`）。Cloudflare Registrar で取得する。
- **Worker を DL 集計用に導入する。**
- **マニフェストに minisign / Sigstore 署名を導入する。** 自前配信は配信元の
  信頼を自分で背負うことになるため。

### 1.4 コード署名・公証 — **やらない**

**費用を理由に、Apple の公証も Windows の Authenticode 署名も見送る。**
判断材料と、それによって生じる制約は 4.4 にまとめる。

- **ad-hoc 署名だけは必ず行う（無料）。** Apple Silicon では ad-hoc 署名が無いと
  実行時に SIGKILL される。素の Go バイナリは内部リンカが自動で付けるが、
  **Wails の `.app` バンドルには `codesign -s -` を明示的に当てる必要がある**。
- `quarantine_darwin.go`（隔離属性剥がし）は**廃止できない。残す前提**。
- 後から公証を追加してもスキーマもインストーラも壊れないため、**この決定は
  いつでも巻き戻せる**。

### 1.5 パッケージマネージャ（GUI + TUI 併存）

- **OS 標準インストーラ（`.pkg` / MSI）は不採用。** 任意バージョン選択・
  ダウングレード・開発版導入は、凍結ペイロード方式では原理的に実現できない。
  必要なのはインストーラではなく**パッケージマネージャ**。
- **GUI フレームワークは Wails v3。** 既存 Go 資産（`internal/app` 以下の検出・
  計画・昇格・受領書・i18n）を再利用し、`internal/tui` と並列に GUI フロントを
  追加する。
- **既存 TUI は GUI と併存させる。** `--no-tui --json` のヘッドレスモードも維持。
- **配布導線を役割で分ける**:

  | 成果物 | ビルド | 配布導線 |
  |---|---|---|
  | `tatsunari`（TUI + ヘッドレス） | `CGO_ENABLED=0`、ubuntu からクロスコンパイル | `curl \| bash` / `irm \| iex` |
  | `tatsunari.app` / `tatsunari.exe`（Wails GUI） | CGO、macOS / Windows ランナー | `.dmg` / `.exe` |

- **★ ビルドを 2 系統に分けること。** Wails を同一バイナリに入れると CGO が
  全体に伝染し、TUI もクロスコンパイルできなくなる。`internal/app` 以下は
  両者で共有し、フロントだけをビルドタグか `cmd/` 分割で差し替える。
  これにより **TUI 側は現行の ubuntu クロスコンパイルを温存できる**。
- Wails は OS の WebView（WKWebView / WebView2）を使うため、Electron と違い
  レンダラの CVE で再リリースを迫られない。「更新頻度を下げる」方針と合致する。
- Windows は WebView2 ランタイムに依存する（Win11 は標準、Win10 も概ね配布済み。
  最悪ブートストラップが要る）。
- 起動導線は **プラグイン内バッジのダイアログから**（4.3）。

> **コスト**: Wails は CGO / OS ネイティブツールチェーンを要するため、現行の
> 「ubuntu から `CGO_ENABLED=0` で全 OS クロスビルド」は使えなくなり、
> macOS / Windows ランナーでのビルドに組み替える必要がある（5.3）。
> 公証を行わないため Apple のシークレットや notarytool は不要。

### 1.6 パッケージマネージャの機能要件

- プラグイン個別インストール（既存）
- フォーマット個別選択（既存）+ **CLAP を新たに同梱対象に加える**
- **任意バージョンの選択（ダウングレード可）**
  - UI は**行ごとの折りたたみ**。既定は latest stable、「バージョンを選ぶ」で展開。
  - ダウングレード時は**互換性警告を出して続行可**とする。
- **開発中バージョンのインストール**
  - **開発版は別のプラグイン ID + 別表示名（`… (Dev)`）**として安定版と共存させる。
  - Dev チャンネルの保持は**直近 5 件**。
  - 公証を行わない決定（1.4）により、Dev ビルドも stable と同じ扱い。
    ad-hoc 署名のみ。
- **配置先は 3 択**: 全ユーザーインストール / ユーザーインストール /
  カスタムパス指定。

### 1.7 不採用としたもの

- **WCLAP / free-audio/web-clap**: 仕様ドラフト段階。WASI 寄りでブラウザ GUI が
  第一目的ではなく、ホスト統合は Wasmer ベースの別ブリッジ構想。clap-wrapper 本体
  にも web ターゲットは存在しない（対応形式は VST3 / AUv2 / AUv3 / AAX /
  standalone のみ）。**ウォッチ対象に留める。**
- ブラウザ用途は既存の `tools/ui-dev`（Emscripten）経路を使う。

---

## 2. 設計不変条件

実装時に侵食させないこと。**緩めるときは人間に確認する。**

1. 常駐しない / 自動更新しない / 勝手にダウンロードしない。
2. **劣化は行単位に留める。** 特定プラグインが新しいクライアントを要求しても、
   アプリ全体をブロックしない。これが Waves Central との決定的な差。
3. **自己アップデート機構を作らない。** 新しいインストーラの存在は閉じられる
   小さな通知として出すだけ。モーダルで塞がない、強制しない。
4. **プラグイン固有のインストールフックを入れない。** インストールモデルは
   「このファイル群をこの場所に展開する」だけに固定する。
5. `__apply` のインストールルート検証を維持する。配置先をデータ駆動化しても、
   **場所トークンはバイナリ内の enum**、`subpath` は `..` / 絶対パスを弾いて
   検証する。ダウンロードした JSON が特権書き込み先を指定できてはならない。
6. **ad-hoc 署名は必ず行う**（Apple Silicon で SIGKILL されないため）。将来
   Developer ID 署名を導入する場合は、**必ずセキュアタイムスタンプ**を付ける
   （証明書失効後も署名は有効に保たれる。公証チケットも期限切れしない）。
7. バイナリに焼き込む URL は**必ず自前の独自ドメイン**。`*.r2.dev` /
   `*.workers.dev` は不可（ホスティング変更時に出荷済みバイナリが孤児になる）。
8. 更新チェックは**静的 JSON**。動的エンドポイントにしない。
9. **プラグインにテレメトリを入れない。** 統計はサーバ側で取る。
10. 成果物は**不変オブジェクト**（`/artifacts/<slug>/<version>/…`、長期
    immutable キャッシュ）。可変なのはポインタ JSON のみ（短 TTL + ETag）。
11. スキーマは `/updates/v1/` で凍結する。未知フィールドは無視、未知フォーマット
    / プラグインは**その行だけ**スキップする。

### 2.1 不変条件を腐らせないためのゲート

- **前方互換 fixture テスト**: 「未来のスキーマ」（未知フィールド・未知フォー
  マット入り）と「過去のスキーマ」の JSON を固定データとして持ち、どちらでも
  クラッシュせず適切に劣化することを Go テストで検証し、`installer-ci.yml` に
  乗せる。
- **フック禁止の明文化**: 例外を 1 つでも入れた瞬間、プラグインが増えるたびに
  インストーラ更新が必要な世界に戻る。

---

## 3. `/updates/v1/` スキーマ設計

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

| フィールド | 説明 |
|---|---|
| `latest` | 最新インストーラのバージョン |
| `url` | ダウンロード先 |
| `changelogUrl` | 変更履歴 |
| `minSupported` | これ未満は動作保証外（**表示のみ。ブロックしない**） |

> 不変条件 3 により、これは**閉じられる通知の材料**にしか使わない。

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

### 3.5 既知の問題: `reference` の公開

`plugins/<slug>/plugin.toml` は `reference = "oeksound soothe2"` のような**競合
製品名**を持つ。`tools/gen_catalog.py --emit-json` はこれをそのまま出力し、
`installer.yml` がリリース資産として添付しているため、**現在すでに公開されて
いる**。

新スキーマでは `reference` を**公開カタログから除外**する。内部の設計意図で
あってユーザーに見せるものではない。

---

## 4. 配置先の設計（スコープ 3 択 × `subpath`）

配置先は「**スコープが決めるルート**」×「**アセットが宣言する `subpath`**」で
合成する。

```
配置先 = ルート(スコープ, OS) + asset.subpath + asset.bundleName
```

| スコープ | macOS ルート | Windows ルート | 昇格 |
|---|---|---|---|
| 全ユーザー | `/Library/Audio/Plug-Ins` | `%CommonProgramFiles%` | 要 |
| ユーザー | `~/Library/Audio/Plug-Ins` | `%LOCALAPPDATA%\Programs\Common` | 不要 |
| カスタム | ユーザー入力 | ユーザー入力 | 要検討（6.3） |

これにより CLAP 追加はデータ側だけで完結する（`subpath: "CLAP"`）。フォーマット
が増えてもインストーラは更新不要、という中核目的が達成される。

### 4.1 現状の課題

`tools/installer/internal/install/destinations.go` は VST3 / AU の 2 フォーマット
と配置先パスを**バイナリ内にハードコード**している。このままだと `.clap` 同梱の
時点でインストーラ更新が必須になる。上記のデータ駆動化はこれを解消するもの。

### 4.2 CLAP 同梱に伴う波及

- `plugins/<slug>/plugin.toml` の `formats` に `"CLAP"` を追加
- `tools/gen_catalog.py` / `tools/release_plan.py`
- release zip の構成（従来は VST3 + AU パリティのみ）
- インストーラのフォーマット選択 UI

### 4.3 プラグイン → GUI インストーラの起動

バッジのダイアログの「アップデート」ボタンから GUI インストーラ（Wails）を
起動する。

#### ★ 前提条件の欠落: インストーラがディスクに残らない

**現状のブートストラップはこの流れを成立させられない。**
`tools/installer/bootstrap/install.sh` は一時ディレクトリにバイナリを落として
`exec` するだけ（`bin="${tmp}/tatsunari"`）で、実行後にバイナリは残らない。
`install.ps1` も同様。

したがって **インストーラが自分自身を恒久的な場所にインストールする**という、
現設計に無い振る舞いを追加する必要がある。

#### ★ 探索順（GUI と TUI の両方がありうる）

`curl | bash` は TUI を配るため、**ワンライナーで導入したユーザーの手元には TUI
しか無い**。したがってプラグインは次の順に探索する。

1. **GUI アプリが正規パスにある → GUI を起動**（最良の体験）
2. **無ければ TUI が正規パスにある → ターミナルで TUI を起動**
3. **どちらも無い → 配布ページを開く**（フォールバック）

PATH には依存しない。

#### 正規パス

| OS | GUI | TUI |
|---|---|---|
| macOS | `/Applications/tatsunari.app`<br>（無ければ `~/Applications/tatsunari.app`） | `~/Library/Application Support/tatsunari-sounds/bin/tatsunari` |
| Windows | `%LOCALAPPDATA%\Programs\tatsunari-sounds\tatsunari.exe` | `%LOCALAPPDATA%\tatsunari-sounds\bin\tatsunari.exe` |

#### フォールバック（必須）

どちらも見つからない場合（初回ユーザー、インストーラを削除したユーザー）、
ボタンを「**インストーラを入手**」に変え、配布ページをブラウザで開く +
ワンライナーをクリップボードにコピーする。これが無いと初回ユーザーで詰む。

#### 起動方法

**GUI（探索順 1）**

- **macOS**: `open <path>.app --args --plugin <slug>`
- **Windows**: `ShellExecute` で直接起動
- ターミナルは開かない。

**TUI（探索順 2）**

- **macOS**: `open -a Terminal <path>`。TUI には制御端末が要る。既存の
  `tty_other.go` / `tty_windows.go` は `curl | bash` 下での再アタッチ用で、
  **GUI プロセスからの起動は別ケース**。
- **Windows**: コンソールアプリなので `ShellExecute` でコンソールが割り当てられる。
- ダイアログに「ターミナルが開きます」と添えて驚きを減らす。

**共通**

- 引数で `--plugin <slug>` を渡し、押されたプラグインを事前選択した状態で開く。
- プロセス起動は当然ながら**オーディオスレッドから行わない**（UI スレッドのみ）。

#### ★ DAW を終了させる必要がある

**DAW がロード中の `.vst3` / `.clap` は差し替えられない。** Windows はファイル
ロックで上書きが失敗し、macOS でも実行中の差し替えは挙動が不定。

このフローは**プラグインの中から始まる**ため、ボタンが押される時点で必ず DAW は
起動している。対策:

- ダイアログに「DAW を終了してから実行してください」を明示する
- インストーラ側でも起動中の DAW を検出して警告する

#### ホスト側の制約（要検証）

プラグインからのプロセス起動は、ホストのサンドボックス方針に依存する。VST3 /
AUv2 は通常ホストプロセス内で非サンドボックスなので起動可能と見込まれるが、
**実ホストでの検証が必要**。起動に失敗した場合はフォールバック（配布ページを
開く）に落とすこと。

### 4.4 未署名・未公証で配布することの帰結

1.4 の決定に伴う制約。**GUI アプリ路線と組み合わさると、TUI 路線のときより傷が
深くなる**点に注意。

#### なぜ GUI の方が不利か

`curl | bash` 経由でダウンロードされたバイナリには **`com.apple.quarantine` が
付かない**（quarantine を付けるのはブラウザや Finder であって curl ではない）。
TUI 路線ではこれにより Gatekeeper をほぼ回避できていた。

GUI アプリは `.dmg` をブラウザでダウンロードするのが自然な導線で、**これは隔離
属性が付く経路そのもの**。近年の macOS では「右クリック → 開く」の抜け道が塞がれ
ているため、初回起動時に**システム設定 → プライバシーとセキュリティ →
「このまま開く」**が必要になる。

#### 緩和策

1. **ad-hoc 署名は必ず行う（無料・必須）。** Apple Silicon では ad-hoc 署名が
   無いと SIGKILL される。素の Go バイナリは内部リンカが自動で付けるが、
   **Wails の `.app` バンドルには `codesign -s -` を明示的に当てる**こと。
2. **回避手順を正直に案内するページを用意する**（ZL Audio と同じ方針）。
3. **TUI 導線は Gatekeeper の影響を受けない。** `curl | bash` で配る
   `tatsunari`（TUI）には隔離属性が付かないため、CLI に抵抗が無いユーザーは
   そのまま使える。

> **★ 抜け道は GUI には効かない。** 一時は「`curl | bash` で `.app` を配れば
> 隔離属性を回避できる」と整理していたが、**ワンライナーは TUI を配る**決定に
> なったため、GUI を `.dmg` で配る以上 **初回起動時のシステム設定往復は避けられ
> ない**。
>
> 後から抜け道を復活させたい場合は、**GUI 用のワンライナーを別途用意して `.app`
> を curl で配置する**導線を足せばよい（追加コストは小さい）。現時点では採らない。

#### プラグイン本体への影響

インストーラの `quarantine_darwin.go` が隔離属性を剥がすため、**インストーラ経由で
入れたプラグインはロードされる**。プラグイン側の傷は浅い。

#### Windows

未署名の `.exe` は SmartScreen 警告（「詳細情報 → 実行」で通せる）止まり。
macOS ほどの問題にはならない。

#### 判断材料（記録）

- macOS には「安い中間案」が無い。Developer ID 証明書は Apple Developer Program
  （年 $99）の加入が前提で、**公証自体は無料**。「署名するが公証しない」は節約に
  ならない。
- **先行事例**: ZL Audio（ZL Equalizer）は公式ドキュメントで
  「All installers have not been notarized」と明記して未公証配布している。
  ただし ZL は無料 / OSS で、ユーザーはソースからビルドし直せる逃げ道がある。
- **この決定は後から巻き戻せる。** 配信基盤がデータ駆動である以上、公証を後から
  追加してもスキーマもインストーラも壊れない。

---

## 5. 決定に伴う波及作業

### 5.1 GitHub Releases 完全廃止

- **`release.yml` の全面書き換え** — Release 作成ではなく R2 へのアップロードに。
- **`installer.yml` の前提が崩れる** — `workflow_run` で Release 完了を待つ構造、
  `gh release view` によるタグ解決が無効になる。
- **`tools/release_plan.py` の差分判定** — 「前回リリースの `manifest.json`」を
  GitHub ではなく R2 から取得するよう変更。
- **bootstrap スクリプトの移転** — 現在 `raw.githubusercontent.com` 配信。
  CDN ドメインへ。
- **リリースノートの置き場所** — GitHub Releases の UI が無くなるため代替が要る。
- **git tag は残す**（無料、追跡性が保てる。「Release を作らない」だけでよい）。

### 5.2 DL 集計 Worker

- プライバシー方針（不変条件 9）と整合させ、**IP や UA を保存しない**。
- 必要なのは「プラグイン別・バージョン別・日別のカウント」のみ。
- 集計はダウンロード経路で行い、**バッジのチェック経路では何も記録しない**。

### 5.3 CI の組み替え（2 系統ビルド）

1.5 のとおり、成果物を 2 系統に分ける。

- **TUI（`tatsunari`）** — `CGO_ENABLED=0` のまま。**現行の ubuntu クロス
  コンパイルをそのまま温存できる**。`installer.yml` の既存ジョブは実質そのまま。
- **GUI（Wails）** — CGO / OS ネイティブツールチェーンが要るため、
  **macOS / Windows ランナーでのビルドを新設する**。`.app` バンドルの組み立て、
  `codesign -s -`（ad-hoc）、`.dmg` の作成もここで行う。

**公証は行わないため、Apple のシークレット・`notarytool`・`stapler` は不要。**
macOS ランナーが要るのは純粋にビルドと ad-hoc 署名のため。

### 5.4 インストーラの自己インストール（新規）

4.3 のとおり、ブートストラップが一時ディレクトリにバイナリを落として捨てる
現状のままでは、プラグインからの起動導線が成立しない。インストーラが自分自身を
正規パスに配置する振る舞いを追加する必要がある。

- **TUI**: `curl | bash` / `irm | iex` が単体バイナリを正規パスに配置する。
- **GUI**: `.dmg` / `.exe` からの導入時に `.app` を正規パスへ。

両方が同時に存在しうるため、4.3 の探索順（GUI 優先 → TUI → 配布ページ）を守る
こと。

---

## 6. 未決定事項

### 6.2 ★ 具体的なドメイン名 — ブロッカー

方式（単一ドメイン + 用途別サブドメイン）は決定済み。**実際の名前が未定。**
出荷済みバイナリに焼き込まれ後から変更できないため、バッジ実装より前に確定必須。

### 6.3 ★ カスタムパス × 特権昇格の安全設計

**不変条件 5 とカスタムパスは正面から衝突する。** 「許可リスト」は、改竄された
`plan.json` が昇格を任意特権書き込みに変えることを防ぐための防御なので、単純に
外すのは危険。

- **(a) カスタムパスはユーザースコープ限定（昇格しない）** — 許可リストは特権
  パスに対して完全なまま維持される。最も安全。ただし別ドライブ等で書き込み権限
  を要する場合に対応できない。← **推奨**
- (b) 許可リストに加えて拒否リストで守る — カスタムパスも昇格可とし、`/System`、
  `/usr`、`C:\Windows`、`C:\Program Files`（VST3 既定を除く）等を明示的に拒否。
  柔軟だが防御は弱まる。
- (c) 既存ディレクトリのみ許可し新規作成は不可 + 昇格前に UI でフルパスを再確認。

推奨は (a)。カスタムパスを使う層は自分で書き込める場所を選べる想定で実用上ほぼ
困らない。必要になったら (c) に緩める順序が安全。

### 6.4 ★ minisign 鍵のローテーション設計

公開鍵をインストーラに埋め込むため、素朴に作ると**鍵を更新した瞬間に全クライ
アントが壊れ、インストーラ更新が必須**になる。「更新頻度を下げる」という中核
方針と衝突する。

- **(a) 複数の公開鍵を最初から埋め込む**（現行鍵 + 予備鍵）。将来のローテーション
  をバイナリ更新なしで実施可能。← **推奨**
- (b) 単一鍵。ローテーション時はインストーラ更新を受け入れる。

秘密鍵の保管場所（Actions Secret / 外部 KMS）も要決定。

### 6.5 dev variant の識別子体系

- CLAP ID の命名規則（例 `com.tatsunari.rs.dev`）
- VST3 UID の別生成
- AU subtype code の別割り当て
- **衝突しないことを CI で検証する仕組み**

### 6.7 リリースノートの置き場所

CDN 上の Markdown / 製品サイト / カタログ内 inline。

### 6.8 DL 集計の実装方式

Workers Analytics Engine / KV カウンタ / Logpush の集計。

### 6.9 ロールバック用ローカルキャッシュ

直前バージョンの zip を残すと、再ダウンロード不要・オフラインでも即座に戻せる。
「更新したら DAW でクラッシュする、今すぐ戻したい」場面で効く。

**保持世代数が未決定。推奨は 1 世代**（ディスク消費が読め、用途の 9 割をカバー）。
それ以前に戻す場合は通常のバージョン選択から再ダウンロードすればよい。

### 6.10 `reference` の公開カタログからの除外

3.5 参照。除外することを推奨。

---

## 7. PR プレビュー（独立して進行可）

`tools/ui-dev/` が 3 つの実エディタ + ギャラリーを WASM にビルドし、`window.ui`
ブリッジ・テーマ・Playwright キャプチャまで揃っている。足りないのは「CI で焼いて
PR ごとの URL に置く」部分だけ。

### 7.1 決定済み

- **段階 1**: PR に Playwright スクリーンショットを貼る（Cloudflare 不要、最小
  コスト）。`npm run capture` が既に `ui.png` + `ui-state.json`（全パラメータ・
  ウィジェット矩形・WebGL 情報・コンソールエラー）を出力する。
- **段階 2**: Cloudflare Workers static assets によるプレビュー URL。
  **Cloudflare Access で保護**し、**必須チェックにはしない**。

構成:

```
PR push → GitHub Actions（emsdk + CMake で ui-dev をビルド、Playwright でキャプチャ）
        → wrangler で Workers static assets へデプロイ
        → PR に bot コメント（プレビュー URL + スクショ）
```

Cloudflare 側の Git 連携ビルドは使わない。ビルド環境に emsdk / CMake /
freetype・visage の FetchContent を用意するのが困難なため、Actions で焼いて
直接アップロードする。

### 7.2 限界（重要）

**これは UI レビュー用であり、プラグインの検証ではない。**

- 検証される: レイアウト、テーマ、ウィジェット挙動、エディタのインタラクション。
- **検証されない**: CLAP/VST3 ラッパ、`ClapEditorHost` によるホスト埋め込み、
  ホスト側の DPI、リアルタイム安全性、実 DSP の音。

CLAUDE.md の「ホスト GUI バグは CTest も pluginval も捕まえない」クラス（例の
リサイズクリップ回帰は visage core とホスト埋め込みで発生）は**ブラウザプレビュー
でも捕まらない**。「これで GUI バグは防げる」と誤解しないこと。

### 7.3 実装時の注意

1. **ビルド時間** — emsdk + visage + freetype を毎回取得すると PR あたり 10 分超。
   `.emsdk` と FetchContent のキャッシュを初手から入れる。
2. **fork からの PR ではシークレットが渡らない** — wrangler の認証が通らない。
   同一 repo のブランチのみデプロイする条件を付ける。
3. **プレビュー URL は既定で公開** — 未発表プラグインの UI が漏れる。
   Cloudflare Access でゲートする。
4. `tools/ui-dev` は現在意図的に CI 外。独立ワークフローにして `ui/visage/**` と
   `plugins/*/ui/**` にパススコープし、**落ちても PR をブロックしない**形で始める。

### 7.4 将来案（未決定）

AudioWorklet で実音を鳴らす。`RsCore` / `PfCore` / `DeqCore` はフレームワーク
非依存なので載せられる。PR レビュー用であると同時に、**製品サイトの「ブラウザで
試す」デモに転用可能**。工数は段階 2 より一段重い（AudioWorklet スコープでの
WASM ロード、`-sMODULARIZE`、サンプル配信、SAB 周り）。

---

## 8. 依存関係と推奨着手順

```
ドメイン決定 ──→ スキーマ確定 ──→ バッジ + ダイアログ実装（URL 焼き込み）
                     ├──→ 配置先データ駆動化 + CLAP 同梱
                     └──→ 自己インストール（5.4）──→ ダイアログからの起動導線

CI をネイティブランナーへ組み替え ──→ Wails GUI ──→ ad-hoc 署名 + .dmg / .exe

PR プレビュー ────（完全に独立）
```

1. **ドメイン取得 + R2 移行 + スキーマ確定 + 配置先データ駆動化** — バッジより
   必ず先。出荷済みバイナリに URL が焼き込まれるため後戻りできない。
2. **CI を 2 系統ビルドに組み替え** — TUI は ubuntu クロスコンパイルのまま、
   GUI 用に macOS / Windows ランナーを新設する。公証を行わないため Apple の
   シークレット類は不要で、純粋にビルド環境の話。
3. **Wails GUI + 自己インストール（5.4）** — バッジのダイアログからの起動導線の
   前提。
4. **プラグイン内バッジ + ダイアログ** — 1 で確定した URL を焼き込む。

PR プレビューは上記と独立。段階 1 のみなら即着手可能。

---

## 9. 概算コスト

署名・公証を見送る決定（1.4）により、**年額は実質ドメイン代のみ**。

| 項目 | 年額の目安 |
|---|---|
| ドメイン（Cloudflare Registrar・原価） | $10〜20 |
| R2 + Worker | 実質数ドル（egress 無料） |
| **合計** | **$10〜20 規模** |

将来 Developer ID 署名 + 公証を追加する場合は Apple Developer Program が年 $99、
Windows のクラウド署名サービスが年 $120 前後 加わる。

**金額は変動するため、発注前に必ず現在価格を確認すること。**

---

## 10. 参考

- [free-audio/clap-wrapper](https://github.com/free-audio/clap-wrapper) — 対応形式は
  VST3 / AUv2 / AUv3 / AAX / standalone。web ターゲットなし
- [free-audio/web-clap](https://github.com/free-audio/web-clap) — WCLAP のドラフト
- [Signalsmith-Audio/wasm-clap-browserhost](https://github.com/Signalsmith-Audio/wasm-clap-browserhost)
  — wasm32 CLAP のブラウザホスト実験
- [Web Audio Modules 2](https://www.webaudiomodules.com/docs/intro/)
- [Plugin Installation | ZL Audio](https://zl-audio.github.io/help/plugin_installation/)
  — 未公証配布の先行事例（4.4）
- [Installation | ZL Audio (ZL Equalizer)](https://zl-audio.github.io/plugins/zlequalizer/installation/)
- [ZL-Audio/ZLEqualizer Discussions #29](https://github.com/ZL-Audio/ZLEqualizer/discussions/29)
  — Gatekeeper 警告のユーザー報告
