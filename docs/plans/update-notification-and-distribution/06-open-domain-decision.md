# 6. 未決定事項

### 6.1 ★ 具体的なドメイン名 — 唯一の未決定事項

方式（単一ドメイン + 用途別サブドメイン）は決定済み。**実際の名前が未定。**
出荷済みバイナリに焼き込まれ後から変更できないため、バッジ実装より前に確定必須。
Cloudflare Registrar で原価取得できる。

---

## 決定記録テンプレート

人間の承認者は取得前に以下を埋める。

| 項目 | 決定値 |
|---|---|
| apex domain / 登録主体 / 更新担当 | 未決定 |
| `cdn.` / `updates.` / `preview.` の origin | 未決定 |
| DNSSEC、registrar lock、自動更新 | 未決定 |
| 障害・失効時の連絡先 | 未決定 |
| 年間予算上限 | 未決定 |

## 技術的受け入れ条件

ASCII で入力しやすく商標衝突を確認済みで、Registrar と Cloudflare account のアクセスが
個人 1 名に依存しないこと。`updates.` は静的 JSON のみ、`cdn.` は不変 artifact、`preview.`
は期限付き PR 成果物として origin と cache policy を分離する。取得後に DNSSEC、CAA、TLS、
HTTP→HTTPS、IPv4/IPv6、複数地域からの疎通を検証してから URL をバイナリへ焼き込む。

ドメイン候補の検索、購入、DNS 本番変更は Ask a human であり、自動化エージェントは実施しない。
