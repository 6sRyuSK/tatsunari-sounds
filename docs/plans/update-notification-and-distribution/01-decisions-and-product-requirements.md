# 1. 決定済み事項

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

#### バッジ → ダイアログ → TUI インストーラ起動

バッジをクリックするとエディタ内にダイアログを表示する。内容:

- 最新バージョン
- 現在インストールされているバージョン
- 主な変更内容（要約）
- 「アップデート」ボタン → **TUI インストーラを起動する**

詳細な設計・制約は 4.3 を参照。

### 1.3 バイナリ配信

- **GitHub Releases から Cloudflare R2 + CDN へ全面移行**（Cloudflare Paid 契約
  済み）。GitHub Releases は**完全廃止**する。
- **ビルドは GitHub Actions を継続。** Cloudflare の CI/CD（Workers Builds /
  Pages ビルド）は Cloudflare へのデプロイ用であり、macOS/Windows ランナーも
  Apple ツールチェーンも持たないため代替にならない。
- 構成: `Actions でビルド` → `R2 へアップロード` → `CDN で配信`。
- 既存の個人所有ドメイン **`6sryusk.com`** を運営の基点とし、Tatsunari Sounds は
  **`/tatsunarisounds/` 配下の事業ブランド**として公開する。用途別サブドメインは
  増やさず、人間向け更新ページと機械向け update feed をパスで分離する（6.1）。
- **Worker を DL 集計用に導入する。**
- **マニフェストに minisign / Sigstore 署名を導入する。** 自前配信は配信元の
  信頼を自分で背負うことになるため。
- **鍵のローテーション設計**（素朴に作ると鍵更新のたびにインストーラ更新が必須に
  なり、中核方針と衝突する）:
  - **公開鍵を 2 本埋め込む**（active + backup）。将来のローテーションをバイナリ
    更新なしで実施できる。
  - マニフェストに**どの鍵で署名したかの鍵 ID** を書く。
  - 秘密鍵は **active = GitHub Actions Secret、backup = オフライン保管**
    （金庫 / パスワードマネージャ）。**backup 秘密鍵を CI に置かない**ことが要点で、
    Actions が侵害されても backup は無事なので切り替えが意味を持つ。

### 1.4 コード署名・公証 — **やらない**

**費用を理由に、Apple の公証も Windows の Authenticode 署名も見送る。**
判断材料と、それによって生じる制約は 4.4 にまとめる。

- **ad-hoc 署名だけは必ず行う（無料）。** Apple Silicon では ad-hoc 署名が無いと
  実行時に SIGKILL される。Go の内部リンカが darwin/arm64 ビルドで自動的に付ける
  ため追加作業は不要だが、**ビルド構成を変えたときに落ちていないことを確認する**。
- `quarantine_darwin.go`（隔離属性剥がし）は**廃止できない。残す前提**。
- 後から公証を追加してもスキーマもインストーラも壊れないため、**この決定は
  いつでも巻き戻せる**。

### 1.5 パッケージマネージャ（TUI のみ）

- **OS 標準インストーラ（`.pkg` / MSI）は不採用。** 任意バージョン選択・
  ダウングレード・開発版導入は、凍結ペイロード方式では原理的に実現できない。
  必要なのはインストーラではなく**パッケージマネージャ**。
- **GUI インストーラは不採用。** 既存の TUI（Bubble Tea）を拡張して、
  バージョン選択・ダウングレード・開発版導入を**すべて TUI 画面として実装する**。
  `--no-tui --json` のヘッドレスモードも維持。
- 配布は `curl | bash` / `irm | iex` のワンライナー、単一バイナリ
  （`tatsunari` / `tatsunari.exe`）。
- 起動導線は **プラグイン内バッジのダイアログから**（4.3）。

> **経緯**: 一度は Wails v3 での GUI 化を決定したが、GUI アプリを 1 本維持する
> コストに見合わないと判断して撤回した。
>
> **この構成の利点（大きい）**:
> - **CGO 不要**。現行の「ubuntu から `CGO_ENABLED=0` で全 OS クロスビルド」が
>   そのまま使え、**CI の組み替えが一切不要**（5.3）。
> - **Gatekeeper の傷がほぼ消える**。`curl | bash` で配るバイナリには隔離属性が
>   付かないため、未公証でも初回起動でシステム設定を触らせずに済む（4.4）。
> - WebView2 ランタイム依存も `.dmg` 作成も不要。単一バイナリのまま。

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
- **配置先は 2 択**: 全ユーザーインストール / ユーザーインストール。

#### dev variant の識別子体系

| 項目 | 決定 |
|---|---|
| CLAP ID | サフィックス `.dev`（`com.tatsunari-sounds.<slug>.dev`） |
| VST3 UID | CLAP ID から決定論的に派生 |
| AU subtype | 末尾 1 文字を `D` に置換 |
| AU manufacturer | 同一のまま（同一ベンダー） |
| 表示名 | `… (Dev)` |

- **要確認**: `make_clapfirst` が VST3 UID を CLAP ID から導出する設定なら、
  CLAP ID を変えるだけで VST3 UID も自動的に変わり実装コストはゼロ。
  `shell/cmake/FactoryClapPlugin.cmake` と clap-wrapper の挙動を実装時に確認する。
  導出していなければ生成スクリプトを `tools/` に足す。
- **CI ゲートを 1 本追加**: 全プラグイン × 全 variant の識別子
  （CLAP ID / VST3 UID / AU subtype）を列挙して**一意性を検証**するテスト。

#### TUI への落とし込み

既存の画面遷移は `discover → plugins → formats → scope → confirm → progress →
summary`。新機能は**画面を増やさず既存画面のキー操作として足す**のが望ましい
（既定パスの操作数を増やさないため）。

| 機能 | 落とし込み |
|---|---|
| バージョン選択 | `plugins` 画面で行にカーソルを合わせ、キー 1 つ（例 `v`）でその行だけバージョン一覧を展開。既定は latest stable のまま |
| チャンネル切替 | グローバルなトグル（例 `c` で stable / beta / dev を巡回）。dev 選択時は警告を表示 |
| ダウングレード警告 | `confirm` 画面で `stateCompatVersion` の差を検出して表示 |
| ロールバック | `plugins` 画面の行に「直前の版に戻す」を出す。ローカルキャッシュは持たないので**再ダウンロードする**（1.7） |

**既定の操作数を変えないこと。** 「全部最新にして Enter」の体験は現状のまま維持
し、追加機能はすべて任意のキー操作の裏に置く。

### 1.7 不採用としたもの

- **WCLAP / free-audio/web-clap**: 仕様ドラフト段階。WASI 寄りでブラウザ GUI が
  第一目的ではなく、ホスト統合は Wasmer ベースの別ブリッジ構想。clap-wrapper 本体
  にも web ターゲットは存在しない（対応形式は VST3 / AUv2 / AUv3 / AAX /
  standalone のみ）。**ウォッチ対象に留める。**
- ブラウザ用途は既存の `tools/ui-dev`（Emscripten）経路を使う。
- **カスタムパス指定は仕様から落とす。** 配置先は全ユーザー / ユーザーの 2 択のみ。
  副作用として **`__apply` の許可リストがバイナリ内の enum だけで完全に閉じる**
  ため、不変条件 5 の防御が最も強い形で保たれる（カスタムパスは許可リストと正面から
  衝突する概念だった）。受領書とインストーラ配置のスコープ追従も曖昧さなく決まる。
- **ロールバック用ローカルキャッシュは持たない。** 直前バージョンの zip を残さず、
  戻すときは通常のバージョン選択から再ダウンロードする。ディスク消費と世代管理の
  複雑さを避ける。オフラインで即座に戻せる利点は失う。

### 1.8 商標記述の一般名称化

**商標に関わる記述はすべて削除し、一般的な名称に置き換える。**

`plugin.toml` の `reference` フィールドは競合製品名を保持しており、
`tools/gen_catalog.py` 経由で `catalog.json` と README のカタログ表の両方に出力
されている。ユーザーが見る面（インストーラ・更新ダイアログ・カタログ）に競合製品名を
並べる必要はない。

#### 棚卸し結果

`reference` フィールドは全 17 プラグイン（active 3 + archive 14）に存在し、うち
**12 件が具体的な製品名**（競合プラグイン / ハードウェアペダル / コンソール）だった。

| 区分 | 箇所 | 件数 |
|---|---|---|
| 製品メタデータ | `plugins/*/plugin.toml` の `reference` | 3 件すべて製品名 |
| 製品メタデータ | `archive/plugins/*/plugin.toml` の `reference` | 14 件中 9 件が製品名 |
| 生成物 | `README.md`（CATALOG ブロックの Reference 列） | 上記 toml から生成 |
| 生成物 | `catalog.json` | 上記 toml から生成 |
| テスト | `tools/tests/test_gen_catalog.py` のフィクスチャ | 3 件 |
| ツール | `tools/scaffold_plugin.py` の usage 例 | 1 件 |
| 設定 | `roadmap.toml` のコメント例 | 1 件 |
| 利用者向け文書 | `docs/manual/resonance-tatsuppressor.md` | 2 件（「〜-style」という比較記述） |
| 内部文書 | `.claude/skills/core-primitives/SKILL.md` | 1 件（「〜系」という分類記述） |
| 内部文書 | 本メモ §0 / §4.4 | 設計動機・未公証配布の先行事例 |

残り 5 件（`granular-delay` / `saturator` / `shimmer-reverb` / `vocal-mbcomp` /
`nam-player`）は既に一般記述か、帰属表示（後述）のため変更不要。

置き換えは**機能の一般記述**にする（例: 競合製品名 → 「Dynamic spectral resonance /
harshness suppression」「Multiband parallel harmonic enhancement」のような機能名）。

#### 適用範囲（決定）

**公開される製品面と利用者向け文書のみ**を対象とする。

| 対象に含める | 対象外 |
|---|---|
| `plugins/*/plugin.toml` の `reference` | `.claude/skills/**`（開発者向け） |
| `archive/plugins/*/plugin.toml` の `reference` | `docs/plans/**`（本メモを含む設計文書） |
| `README.md`（生成物 — 再生成する） | |
| `catalog.json`（生成物） | |
| `docs/manual/**` | |
| `roadmap.toml` のコメント例 | |
| `tools/tests/**` のフィクスチャ | |
| `tools/scaffold_plugin.py` の usage 例 | |

内部の設計文書を対象外にしたのは、本メモ §0（設計動機）と §4.4（未公証配布の
先行事例）から実名を落とすと**判断の根拠が追えなくなる**ため。先行事例・比較対象と
しての言及は開発者向け文書に留める。

#### 例外: 依存ライブラリへの帰属表示は残す

`archive/plugins/nam-player/plugin.toml` の `reference` は**競合製品への言及ではなく、
実際に使用しているライブラリの作者・バージョンへの帰属表示**（`cmake/NamCore.cmake`
が取り込む依存）。帰属は残すべきものなので置き換えない。

#### 実施状況

本 PR で実施済み。

- `plugin.toml` **12 ファイル**（active 3 + archive 9）
- `docs/manual/resonance-tatsuppressor.md` 2 箇所
- `roadmap.toml` のコメント例 1 箇所
- `tools/tests/test_gen_catalog.py` のフィクスチャ 3 箇所
- `tools/scaffold_plugin.py` の usage 例 1 箇所
- `README.md` は `gen_catalog.py` で再生成

検証: `gen_catalog.py --check` / `check_skill_refs.py` / `tools/tests`（46 件）
すべて通過。

---

## 詳細な振る舞い仕様

### 更新チェック状態機械

`disabled`（未同意を含む）、`idle`、`checking`、`current`、`updateAvailable`、
`transientError` の 6 状態とする。`checking` は同時に 1 リクエストだけ許可し、
エディタを閉じたらキャンセルする。HTTP 304 は成功として最終成功時刻を更新する。
タイムアウト、非 2xx、JSON 不正、署名不正は `transientError` とし、音声処理や
エディタ操作を妨げず、同一セッション中の自動再試行をしない。

- opt-in の選択肢は「有効にする」「今はしない」。閉じる操作は「今はしない」と同じ。
- 同意値、最終成功時刻、ETag はユーザー設定へ保存する。プラグイン state には入れない。
- 比較対象は実行中プラグインと同じ `slug` / stable variant。SemVer の prerelease は
  stable の最新判定から除外する。
- バッジは `latest > current` のときだけ表示する。取得失敗や版文字列不正をバッジで
  警告しない。
- ダイアログの「アップデート」はインストーラの存在確認後に起動するだけで、エディタ
  自身は成果物を取得しない。

### TUI 選択規則

1. 初期値は全プラグインの latest stable、既存フォーマット、既存スコープを優先する。
2. `v` でカーソル行の非 yanked バージョンを新しい順に展開する。
3. `c` による dev 選択は初回だけ警告確認を要求し、キャンセル時は stable に戻す。
4. installed より古い版、または `stateCompatVersion` が下がる版は confirm に警告を出す。
5. `minClientVersion` を満たさない行は理由と必要版を表示して無効化し、他行は継続する。
6. `--no-tui --json` は同じ解決器を使い、対話が必要なら安定したエラーコードで終了する。

### 識別子と受領書

インストール実体の主キーは `(slug, variant, scope)`、各形式の実体はさらに `format` を
持つ。stable/dev の ID 生成結果はリリース前に固定 fixture と照合する。表示名は識別子に
使わない。受領書の版を更新するのは、全選択アセットの置換が成功した後だけとする。

## 実装チェックリスト

- [ ] platform HTTP adapter と fake transport を追加する。
- [ ] opt-in、24 時間抑制、ETag、キャンセルを単体試験する。
- [ ] Visage のバッジ、ダイアログ、キーボード操作、スクリーンリーダー名を実装する。
- [ ] バージョン / channel 解決を UI から独立した純粋ロジックにする。
- [ ] stable/dev の全 ID 衝突試験と TUI golden transcript を CI に追加する。
- [ ] headless JSON の schema と終了コードを文書化する。

## 受け入れ条件

オフライン、タイムアウト、304、不正署名、未知 plugin、client 下限不一致を注入しても
DAW が停止せず、対象行以外が利用できること。既定操作、dev 共存、ダウングレード警告、
両スコープの同一 slug 表示を Windows/macOS で確認すること。
