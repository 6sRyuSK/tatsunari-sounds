# 2. 設計不変条件

実装時に侵食させないこと。**緩めるときは人間に確認する。**

1. 常駐しない / 自動更新しない / 勝手にダウンロードしない。
2. **劣化は行単位に留める。** 特定プラグインが新しいクライアントを要求しても、
   アプリ全体をブロックしない。これが Waves Central との決定的な差。
3. **自己アップデート機構を作らない。** 新しいインストーラの存在は閉じられる
   小さな通知として出すだけ。モーダルで塞がない、強制しない。
4. **プラグイン固有のインストールフックを入れない。** インストールモデルは
   「このファイル群をこの場所に展開する」だけに固定する。
5. `__apply` のインストールルート検証を維持する。配置先をデータ駆動化しても、
   **ルートはバイナリ内の enum で決まる**（スコープ 2 択のみ。カスタムパスを
   仕様から落としたので許可リストは完全に閉じる）、`subpath` は `..` / 絶対パスを
   弾いて検証する。ダウンロードした JSON が特権書き込み先を指定できてはならない。
6. **ad-hoc 署名は必ず行う**（Apple Silicon で SIGKILL されないため）。将来
   Developer ID 署名を導入する場合は、**必ずセキュアタイムスタンプ**を付ける
   （証明書失効後も署名は有効に保たれる。公証チケットも期限切れしない）。
7. バイナリに焼き込む URL は**必ず `https://6sryusk.com` の固定パス**。
   `*.r2.dev` / `*.workers.dev` は不可（ホスティング変更時に出荷済みバイナリが
   孤児になる）。取得クライアントは host だけでなく、
   `/tatsunarisounds/updates/v1/` と `/tatsunarisounds/artifacts/` の path prefix も
   許可リストで検証する。
8. 更新チェックは**静的 JSON**。動的エンドポイントにしない。
9. **プラグインにテレメトリを入れない。** 統計はサーバ側で取る。
10. 成果物は**不変オブジェクト**
    （`/tatsunarisounds/artifacts/<slug>/<version>/…`、長期 immutable キャッシュ）。
    可変なのはポインタ JSON のみ（短 TTL + ETag）。
11. スキーマは `/tatsunarisounds/updates/v1/` で凍結する。未知フィールドは無視、
    未知フォーマット / プラグインは**その行だけ**スキップする。

### 2.1 不変条件を腐らせないためのゲート

- **前方互換 fixture テスト**: 「未来のスキーマ」（未知フィールド・未知フォー
  マット入り）と「過去のスキーマ」の JSON を固定データとして持ち、どちらでも
  クラッシュせず適切に劣化することを Go テストで検証し、`installer-ci.yml` に
  乗せる。
- **フック禁止の明文化**: 例外を 1 つでも入れた瞬間、プラグインが増えるたびに
  インストーラ更新が必要な世界に戻る。

---

## 強制方法

| 不変条件 | 自動ゲート |
|---|---|
| 非常駐・非自動取得 | updater 起動点の静的レビュー + fake transport の呼び出し回数試験 |
| 行単位劣化 | 未知 plugin/format、壊れた asset の table-driven test |
| 自己更新禁止 | client version が古くても操作可能な統合試験 |
| フック禁止 | JSON Schema の `additionalProperties` 検査と禁止キー fixture |
| 書込先制限 | path traversal、絶対パス、symlink/junction adversarial test |
| 独自ドメイン | schema lint で URL host + path prefix allowlist を検証 |
| テレメトリ禁止 | 更新確認 fixture で GET 以外がないことを transport で検証 |
| immutable | artifact response の Cache-Control と上書き拒否を staging で検証 |

## セキュリティ試験ケース

`../`、percent encoding、Unicode separator、Windows drive/UNC、予約デバイス名、
case collision、展開後 symlink、zip bomb、重複 entry を拒否する。検証は展開前の宣言値
だけで終えず、作成直前の canonical path とルートの包含関係を再確認する。`__apply` は
親プロセスから渡された任意パスを信用せず、検証済み計画の digest、scope enum、
一時ディレクトリの所有者・権限を再検証する。

## 変更手続き

不変条件を緩める PR は、脅威モデル、代替案、移行、ロールバック、人間の承認者を持つ
ADR を先に追加する。単なる schema フィールド追加や UI 都合で例外を設けない。
