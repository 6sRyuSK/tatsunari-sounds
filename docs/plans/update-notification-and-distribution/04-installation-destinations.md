# 4. 配置先の設計（スコープ 2 択 × `subpath`）

> **製品 ID 移行後の正本**: bundle basename、vendor folder の例外、installer の
> filename は [§11](11-product-identity-migration.md) を正とする。

配置先は「**スコープが決めるルート**」×「**アセットが宣言する `subpath`**」で
合成する。

```
配置先 = ルート(スコープ, OS) + asset.subpath + asset.bundleName
```

| スコープ | macOS ルート | Windows ルート | 昇格 |
|---|---|---|---|
| 全ユーザー | `/Library/Audio/Plug-Ins` | `%CommonProgramFiles%` | 要 |
| ユーザー | `~/Library/Audio/Plug-Ins` | `%LOCALAPPDATA%\Programs\Common` | 不要 |

#### 既定 `subpath`（カタログ未宣言時 / レガシー zip）

| OS | Format | subpath |
|---|---|---|
| macOS | VST3 | `VST3/tatsunari-sounds` |
| macOS | AU | `Components` |
| macOS | CLAP | `CLAP/tatsunari-sounds` |
| Windows | VST3 (system) | `VST3/tatsunari-sounds` |
| Windows | VST3 (user) | `VST3/tatsunari-sounds` |
| Windows | CLAP | `CLAP/tatsunari-sounds` |

カスタムパス指定は仕様から落としたため（1.7）、**ルートはバイナリ内の enum
2 通りだけ**。これにより `__apply` の許可リストが完全に閉じる（不変条件 5）。

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

### 4.3 プラグイン → TUI インストーラの起動

バッジのダイアログの「アップデート」ボタンから TUI インストーラを起動する。

#### ★ 前提条件の欠落: インストーラがディスクに残らない

**現状のブートストラップはこの流れを成立させられない。**
`tools/installer/bootstrap/install.sh` は一時ディレクトリにバイナリを落として
`exec` するだけ（`bin="${tmp}/tatsunari"`）で、実行後にバイナリは残らない。
`install.ps1` も同様。

したがって **インストーラが自分自身を恒久的な場所にインストールする**という、
現設計に無い振る舞いを追加する必要がある。

#### 正規パス（インストーラ配置はスコープに追従する）

インストーラ自身の配置先は、**ユーザーが選んだインストールスコープに追従**する
（5.4）。したがって 2 箇所ありうる。

| スコープ | macOS | Windows |
|---|---|---|
| システム | `/Library/Application Support/tatsunari-sounds/bin/tatsunari-sounds-installer` | `%ProgramFiles%\tatsunari-sounds\tatsunari-sounds-installer.exe` |
| ユーザー | `~/Library/Application Support/tatsunari-sounds/bin/tatsunari-sounds-installer` | `%LOCALAPPDATA%\tatsunari-sounds\bin\tatsunari-sounds-installer.exe` |

**探索順: ユーザー → システム → フォールバック。** PATH には依存しない。

ユーザー配置を先に見るのは、それがそのユーザー自身の明示的な選択の結果であり、
確実に実行権限があるため。

> **副産物**: 最初のユーザーがシステムスコープを選んでいれば、**同じマシンの別
> ユーザーは何もせずにインストーラを見つけられる**。マルチユーザー環境での導線が
> 自然に整う。

- macOS で `/usr/local/bin` を使わないのは、Homebrew 環境では `/usr/local` が
  **ユーザー所有**になっていることが多く、後でユーザーが昇格して実行するバイナリを
  第三者プロセスが差し替えられてしまうため。プラグインの配置先
  （`/Library/Audio/Plug-Ins`）と同じ `/Library` 配下に揃える方が一貫性もある。
  PATH に通したい場合は `/usr/local/bin` へのシンボリックリンクを任意で足す
  （必須ではない）。
- **システム配置の場合、配置先ディレクトリは root 所有・非 root 書き込み不可で
  あることを検証する。**
- Windows は指定 filename に `installer` を含むため、explicit application manifest
  (`requestedExecutionLevel=asInvoker`) を埋め込み、UAC の Installer Detection による
  暗黙昇格を防ぐ。system scope の昇格は `__apply` だけが明示的に要求する。

#### フォールバック（必須）

正規パスにバイナリが無い場合（初回ユーザー、インストーラを削除したユーザー）、
ボタンを「**インストーラを入手**」に変え、配布ページをブラウザで開く +
ワンライナーをクリップボードにコピーする。これが無いと初回ユーザーで詰む。

#### 起動方法

- **macOS**: `open -a Terminal <path>`。TUI には制御端末が要る。既存の
  `tty_other.go` / `tty_windows.go` は `curl | bash` 下での再アタッチ用で、
  **GUI プロセスからの起動は別ケース**。
- **Windows**: コンソールアプリなので `ShellExecute` で直接起動すればコンソールが
  割り当てられる。
- 引数で `--plugin <slug>` を渡し、押されたプラグインを事前選択した状態で開く。
- プロセス起動は当然ながら**オーディオスレッドから行わない**（UI スレッドのみ）。

#### UX 上の割り切り

プラグインの「アップデート」を押したらターミナルが開く体験は、音楽制作者には
多少面食らうものになる。頻度が低く、ユーザーが明示的に押した結果であり、GUI
アプリを 1 本維持するコストを丸ごと回避できるため割り切る。ダイアログに
「ターミナルが開きます」と添えて驚きを減らす。

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

1.4 の決定に伴う制約。**TUI のみの構成では傷はかなり浅い。**

#### なぜ TUI 路線が有利か

`curl | bash` 経由でダウンロードされたバイナリには **`com.apple.quarantine` が
付かない**（quarantine を付けるのはブラウザや Finder であって curl ではない）。
ワンライナーを主導線にする限り、**未公証でも初回起動でシステム設定を触らせずに
済む**。

対照的に、GUI アプリを `.dmg` でブラウザ配布する構成（一度検討して撤回）は隔離
属性が付く経路そのもので、初回起動時に**システム設定 → プライバシーとセキュリ
ティ →「このまま開く」**が必要になっていた。GUI を見送ったことで、この問題は
まるごと消えている。

#### 緩和策

1. **ad-hoc 署名は必ず行う（無料・必須）。** Apple Silicon では ad-hoc 署名が
   無いと SIGKILL される。Go の内部リンカが darwin/arm64 ビルドで自動的に付ける
   （`installer.yml` のコメント参照）が、**ビルド構成を変えたときに落ちていない
   ことを確認する**こと。
2. **ワンライナーを主導線として維持する。** ブラウザからバイナリを直接ダウン
   ロードさせる導線を主にしない。
3. **ブラウザで落とした場合の回避手順を案内するページを用意する**
   （ZL Audio と同じ方針）。

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

## 配置計画のデータモデル

配置前に `InstallPlan` を確定し、各項目へ `slug, variant, version, format, scope,
sourceDigest, destinationRootId, relativePath` を持たせる。`destinationRootId` はバイナリ内 enum
で、manifest の文字列を root として解釈しない。計画を正規 JSON 化した SHA-256 digest を
通常プロセスと `__apply` の間で照合する。

### トランザクション

1. scope 決定後に全 destination を検証し、競合と空き容量を調べる。
2. 同一 filesystem の staging へ展開し、hash、bundle 構造、symlink 不在を検査する。
3. 既存 bundle を同一 root の rollback 名へ atomic rename する。
4. staging を最終名へ rename し、全項目成功後に受領書を atomic 更新する。
5. 途中失敗時は逆順で旧 bundle を戻し、新しい受領書を残さない。

DAW が bundle を使用中なら置換を開始せず、終了を促して再検査する。Windows の sharing
violation と macOS の起動中ホスト検出は「再試行」「中止」のみを提供し、強制終了しない。

## インストーラ自己配置

bootstrap は OS/arch asset を取得・SHA 検証後、一時バイナリを起動する。一時バイナリ内の
TUI で scope を選び、その実行ファイル自身を `InstallPlan` に含める。ユーザースコープは
ユーザー bin、全ユーザーは許可済み system bin へ `__apply` で配置する。完了後のプラグイン
ダイアログは正規パスだけを検索し、一時パスを永続化しない。

## 受け入れ試験マトリクス

macOS/Windows × user/system × 新規/更新/ダウングレード × stable/dev を最低組合せとし、
権限拒否、ディスク不足、ロック、途中 kill、壊れた zip を注入する。各失敗後に旧 bundle と
旧受領書が一致し、root 外のファイルが変化していないことを検証する。
