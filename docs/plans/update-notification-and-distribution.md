# 配信・更新基盤 実装プラン索引（2026-07）

プラグインの更新通知、バイナリ配信、TUI パッケージマネージャ、PR プレビューを
実装可能な単位に分割したプラン群の索引。本ファイルは方針の入口だけを担い、詳細仕様、
受け入れ条件、検証方法は各セクションのファイルを正本とする。

> **運用ルール**: 実装 PR は対応するプランのチェックリストと受け入れ条件を引用し、
> 完了した項目だけを更新する。複数セクションを同時に変更するときも、検証結果は
> セクション別に記録する。出荷、署名、公証、DNS 本番変更は人間の明示承認なしに行わない。

## セクション別プラン

| # | プラン | 実装時の成果物 |
|---:|---|---|
| 0 | [背景・目的](update-notification-and-distribution/00-background-and-goals.md) | スコープ、非目標、成功指標 |
| 1 | [決定事項・製品要件](update-notification-and-distribution/01-decisions-and-product-requirements.md) | バッジ、TUI、variant、商標表現の仕様 |
| 2 | [設計不変条件](update-notification-and-distribution/02-design-invariants.md) | 自動検証ゲート、セキュリティ境界 |
| 3 | [`/tatsunarisounds/updates/v1/` スキーマ](update-notification-and-distribution/03-updates-v1-schema.md) | JSON Schema、fixture、署名・互換性試験 |
| 4 | [配置先](update-notification-and-distribution/04-installation-destinations.md) | scope × subpath、昇格、起動導線 |
| 5 | [配信・インストーラ作業](update-notification-and-distribution/05-distribution-and-installer-work.md) | R2、Worker、bootstrap、自己配置、受領書 |
| 6 | [ドメイン決定](update-notification-and-distribution/06-open-domain-decision.md) | 人間による名称決定と DNS 設計 |
| 7 | [PR プレビュー](update-notification-and-distribution/07-pr-preview.md) | WASM ビルド、成果物、スクリーンショット |
| 8 | [依存関係・展開順](update-notification-and-distribution/08-dependencies-and-rollout.md) | フェーズ、移行・ロールバック判定 |
| 9 | [費用](update-notification-and-distribution/09-cost-estimate.md) | 予算確認、上限アラート |
| 10 | [参考資料](update-notification-and-distribution/10-references.md) | 実装判断の根拠と再検証項目 |
| 11 | [製品 ID・名称・配置の移行](update-notification-and-distribution/11-product-identity-migration.md) | `tn-*` への再識別、0.x version、vendor folder、installer 名 |

## 横断 Definition of Done

- 対応セクションの必須受け入れ条件がすべて自動試験または手動試験で確認されている。
- 未知フィールド、未知フォーマット、単一アセット障害が全体障害に波及しない。
- 更新確認だけではダウンロード、インストール、テレメトリ送信が発生しない。
- 権限昇格前後で同一の検証済み計画を使い、許可リスト外へ書き込めない。
- 出荷操作は dry-run と人間の承認を分離し、監査可能なログを残す。
