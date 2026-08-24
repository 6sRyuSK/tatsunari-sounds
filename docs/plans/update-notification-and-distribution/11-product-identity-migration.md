# 11. 製品 ID・名称・配置の移行

## 11.1 決定

既存 3 製品は表示名だけを変更するのではなく、**新しい製品 ID を持つ別製品として再出発**
する。新旧 ID 間の state / preset / automation の互換性は約束せず、旧製品への
in-place update も行わない。

| 現行 source directory | 新 product key / slug | 新表示名 | bundle filename | 初回 version |
|---|---|---|---|---|
| `plugins/resonance-suppressor` | `tn-resonance-suppressor` | TN Resonance Suppressor | `tn-resonance-suppressor.vst3` / `tn-resonance-suppressor.component` / `tn-resonance-suppressor.clap` | `0.1.0` |
| `plugins/dynamic-eq` | `tn-equalizer` | TN Equalizer | `tn-equalizer.vst3` / `tn-equalizer.component` / `tn-equalizer.clap` | `0.1.0` |
| `plugins/pitch-fix` | `tn-vocal-tuner` | TN Vocal Tuner | `tn-vocal-tuner.vst3` / `tn-vocal-tuner.component` / `tn-vocal-tuner.clap` | `0.1.0` |

表記は **Suppressor** を正とする（`Supressor` ではない）。旧表示名
`Resonance TatSuppressor`、`Dynamic Tatsunari EQ`、`Pitch TatFixer` は新しい成果物、
カタログ、マニュアル、UI、リリースノートでは使用しない。移行案内で旧製品を特定する
場合だけ旧名を記載してよい。

source directory は実装 PR で新 slug に rename する。更新カタログ、release manifest、
受領書のキーも新 slug にする。ファイル名だけ変えて旧 slug を再利用してはならない。

## 11.2 version 方針

新 ID はホスト、更新基盤、受領書から見て既存 ID と異なる製品なので、旧 ID の
`3.1.1` / `2.0.0` / `0.2.0` から major bump する必要はない。3 製品とも `0.1.0` から
開始する。

SemVer の major `0` は「初期開発で public contract が安定していない」ことを表し、
音質やテスト品質が低いことを表さない。`0.y.z` 期間は次の規則を採用する。

- compatible bug fix は patch (`0.1.0` → `0.1.1`)。
- feature addition は minor (`0.1.x` → `0.2.0`)。
- state / preset / parameter contract の破壊的変更も minor を上げる。変更内容と移行不能を
  release notes に明記する。
- public contract を維持できると判断した時点で `1.0.0` にする。品質ゲートは 0.x でも
  1.x と同じで、major `0` をテスト省略や既知の P0/P1 不具合の免責に使わない。

これは「既存製品を 0.x に downgrade」する決定ではない。旧 ID と旧 version は凍結し、
新 ID を新規登録する。manifest が version の大小だけで旧製品を新製品へ置換したり、
旧 slug に `0.1.0` を載せたりすると updater から downgrade に見えるため禁止する。

`stateCompatVersion` は製品 SemVer から独立して管理する。新製品の初期値を `1.0.0` とし、
互換な state では維持、読み込み不能になる変更でだけ上げる。major `0` だから常に state を
捨ててよい、とは解釈しない。

## 11.3 plugin ID

各フォーマットの安定 ID は新 product key から生成し、以後変更しない。

| product key | CLAP ID / reverse-DNS base | VST3 / AU identity |
|---|---|---|
| `tn-resonance-suppressor` | `jp.tatsunari-sounds.tn-resonance-suppressor` | 実装時に新規の VST3 class ID と AU subtype を採番 |
| `tn-equalizer` | `jp.tatsunari-sounds.tn-equalizer` | 実装時に新規の VST3 class ID と AU subtype を採番 |
| `tn-vocal-tuner` | `jp.tatsunari-sounds.tn-vocal-tuner` | 実装時に新規の VST3 class ID と AU subtype を採番 |

旧 class ID / subtype の流用は、ホストに旧セッションとの互換性を誤認させるため禁止する。
AU manufacturer code はベンダー識別子なので維持してよいが、subtype は製品ごとに新規採番
する。Dev variant は既存規則どおり stable ID と衝突しない `.dev` 系 ID を使う。

## 11.4 vendor folder と標準配置先

「すべて vendor folder に入れる」は、**各フォーマットの標準探索 root を変えず、root の
直下に `tatsunari-sounds` を作れるフォーマットだけで適用する**。

| OS / format | system scope | user scope | 決定 |
|---|---|---|---|
| macOS VST3 | `/Library/Audio/Plug-Ins/VST3/tatsunari-sounds/<bundle>.vst3` | `~/Library/Audio/Plug-Ins/VST3/tatsunari-sounds/<bundle>.vst3` | vendor folder を使う |
| Windows VST3 | `%CommonProgramFiles%\VST3\tatsunari-sounds\<bundle>.vst3` | `%LOCALAPPDATA%\Programs\Common\VST3\tatsunari-sounds\<bundle>.vst3` | vendor folder を使う |
| macOS AUv2 | `/Library/Audio/Plug-Ins/Components/<bundle>.component` | `~/Library/Audio/Plug-Ins/Components/<bundle>.component` | **vendor folder を作らない** |
| macOS CLAP | `/Library/Audio/Plug-Ins/CLAP/tatsunari-sounds/<bundle>.clap` | `~/Library/Audio/Plug-Ins/CLAP/tatsunari-sounds/<bundle>.clap` | vendor folder を使う。ただし配布開始前に対象 host の再帰探索を検証する |
| Windows CLAP | `%CommonProgramFiles%\CLAP\tatsunari-sounds\<bundle>.clap` | `%LOCALAPPDATA%\Programs\Common\CLAP\tatsunari-sounds\<bundle>.clap` | vendor folder を使う。ただし配布開始前に対象 host の再帰探索を検証する |

AU は Audio Component の標準 `Components` directory へ bundle を直接置く。AU の vendor
identity は filesystem folder ではなく bundle identifier / manufacturer code に持たせる。
`Components/tatsunari-sounds/` は、全対象 DAW が再帰探索するという契約を置けないため
採用しない。「全形式で同じ見た目の階層」にするために host discovery を危険にさらさない。

インストーラは表の絶対 root を配信データから受け取らず、許可済み root enum と format
別 subpath から組み立てる。旧 bundle の検出・削除は新 bundle の配置と別の明示的な移行
操作にし、ユーザーの確認なしに旧版を削除しない。

## 11.5 TUI installer の名称

配布する実行ファイル名を次に統一する。

| Platform | filename |
|---|---|
| Windows | `tatsunari-sounds-installer.exe` |
| macOS | `tatsunari-sounds-installer` |
| Linux | `tatsunari-sounds-installer` |

恒久配置、bootstrap の一時ファイル、release asset、プラグインからの探索、help の program
name をすべて同時に変更する。旧 `tatsunari.exe` / `tatsunari` は新しい仕様では廃止する。

Windows は filename に `installer` を含めるため、実行ファイルへ explicit application
manifest（`requestedExecutionLevel=asInvoker`）を埋め込み、filename による Installer
Detection の暗黙昇格へ依存しない。system scope の昇格は従来どおり検証済み InstallPlan を
渡す専用 `__apply` 境界だけで行う。

## 11.6 移行と受け入れ条件

1. 旧 ID と新 ID を同じ環境へ入れ、DAW が別 plugin として列挙する。
2. 新 bundle の basename、UI 名、catalog 名が表 11.1 と完全一致する。
3. VST3 は vendor folder から、AU は `Components` 直下から、対象 DAW と validator で
   認識される。CLAP を出荷対象に含める時点で vendor folder からの認識も対象 host ごとに
   確認する。
4. 新 3 slug の release manifest がすべて `0.1.0` を持ち、旧 slug の version を
   downgrade していない。
5. updater / receipt は旧 ID と新 ID を異なる entry として扱い、自動置換・自動削除しない。
6. installer の全 release asset と自己配置先が新 filename になり、Windows の起動時に
   scope 選択前の UAC prompt が出ない。
7. 旧名・旧 basename・旧 ID の残存を repository-wide check で列挙し、移行資料または
   legacy detection 以外の残存を gate failure にする。

