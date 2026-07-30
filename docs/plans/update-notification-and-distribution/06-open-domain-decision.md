# 6. ドメイン・公開 URL の決定

### 6.1 具体的なドメイン名と URL 階層

既存の個人所有ドメイン **`6sryusk.com`** を運営の基点とし、Tatsunari Sounds は
`/tatsunarisounds/` 配下の事業ブランドとして扱う。現状は Tatsunari Sounds のみのため、
`updates.` / `cdn.` 等の用途別サブドメインや `/api/updates/` は設けない。

人間向け更新案内と機械向け update feed は同じ `/updates/` 名前空間に置き、`v1/` を
機械向けの固定プロトコル境界にする。

---

## 決定記録

| 項目 | 決定値 |
|---|---|
| apex domain / 登録主体 | `6sryusk.com` / 6sRyuSK の既存個人所有ドメイン |
| Tatsunari Sounds トップ | `https://6sryusk.com/tatsunarisounds/` |
| 人間向け更新案内 | `https://6sryusk.com/tatsunarisounds/updates/` |
| 機械向け update feed | `https://6sryusk.com/tatsunarisounds/updates/v1/{latest,catalog}.json` |
| 不変 artifact | `https://6sryusk.com/tatsunarisounds/artifacts/<slug>/<version>/…` |
| origin | Cloudflare Worker 等で固定パスを R2 へルーティング。R2 の開発 URL は公開しない |
| DNSSEC、registrar lock、自動更新 | 本番 URL 焼き込み前に確認 |
| 障害・失効時の連絡先 | 本番 promote 前に決定 |
| 年間予算上限 | 既存ドメイン更新費は共通費。R2 / Worker の上限は 9 を参照 |

## URL 互換性規則

- `/tatsunarisounds/updates/` は HTML の人間向けページを返す。
- `/tatsunarisounds/updates/v1/` は静的 JSON と署名だけを返す機械向け領域とする。
- `/tatsunarisounds/artifacts/` は不変成果物だけを配信する。
- `/tatsunarisounds/notes/` は人間向け changelog を配信し、`changelogUrl` だけが参照する。
- バイナリは host `6sryusk.com` に加え、URL フィールドの用途に応じて
  `/tatsunarisounds/updates/v1/`、`/tatsunarisounds/artifacts/`、
  `/tatsunarisounds/notes/` の path prefix を検証する。
- サイト構成や配信バックエンドを変更しても、出荷済みバイナリが参照する URL は維持する。
  将来専用ドメインを追加しても、旧 URL は redirect だけにせず同じ内容を配信し続ける。

## 技術的受け入れ条件

Cloudflare account のアクセスを個人 1 名だけに依存させないこと。人間向け HTML、可変 pointer
JSON、不変 artifact は同じ host 上でも path ごとに origin と cache policy を分離する。本番前に
DNSSEC、CAA、TLS、HTTP→HTTPS、IPv4/IPv6、複数地域からの疎通、host + path allowlist、redirect
後の再検証を確認してから URL をバイナリへ焼き込む。

DNS 本番変更は Ask a human であり、自動化エージェントは実施しない。
