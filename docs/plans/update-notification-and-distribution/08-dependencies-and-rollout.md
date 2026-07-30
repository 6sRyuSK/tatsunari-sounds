# 8. 依存関係と推奨着手順

```
ドメイン決定 ──→ スキーマ確定 ──→ バッジ + ダイアログ実装（URL 焼き込み）
                     ├──→ 配置先データ駆動化 + CLAP 同梱
                     └──→ 自己インストール（5.4）──→ ダイアログからの起動導線

PR プレビュー ────（完全に独立）
```

1. **公開 URL 確定 + R2 移行 + スキーマ確定 + 配置先データ駆動化** — バッジより
   必ず先。出荷済みバイナリに URL が焼き込まれるため後戻りできない。
2. **インストーラの自己インストール（5.4）** — バッジのダイアログからの起動導線の
   前提。
3. **TUI の機能拡張** — バージョン選択 / チャンネル切替 / ダウングレード警告 /
   ロールバック（1.6）。CI の組み替えは不要。
4. **プラグイン内バッジ + ダイアログ** — 1 で確定した URL を焼き込む。

**商標記述の一般名称化（1.8）は上記と独立**して先行できる。`plugin.toml` の
`reference` を書き換えて `gen_catalog.py` を再実行するだけなので、ドメイン決定を
待つ必要がない。適用範囲（6.2）だけ確定させれば着手可能。

PR プレビューは上記と独立。段階 1 のみなら即着手可能。

---

## フェーズと exit criteria

| Phase | 内容 | Exit criteria |
|---|---|---|
| A | 公開 URL、脅威モデル、schema fixture | 6.1 の人間承認、parser/generator 全 fixture pass |
| B | R2 staging、promote/rollback | immutable/TTL、backup key drill が pass |
| C | resolver、scope、CLAP、受領書 | OS × scope 統合試験、旧版互換が pass |
| D | TUI 機能と自己配置 | headless/golden、権限失敗 rollback が pass |
| E | バッジ・起動導線 | fake clock/network、DAW smoke test が pass |
| F | 本番移行 | 承認済み runbook、監視、旧導線案内が完了 |

PR preview は独立 lane とするが、共通 CI 資源を変更するときは Phase A〜F の workflow 権限へ
影響しないことを確認する。

## compatibility と rollback

新クライアント公開より先に、新旧クライアント双方が読める catalog を公開する。pointer 更新後は
エラー率、404、hash 不一致、Worker status を監視し、閾値超過時は直前の署名済み pointer へ戻す。
プラグイン bundle の自動巻き戻しは行わず、TUI から明示的に旧版を選ぶ。

## リリース判定チェックリスト

- [ ] schema と公開鍵の変更がないか差分確認した。
- [ ] 全 artifact の read-back hash と対象 OS/arch を確認した。
- [ ] yanked、dev retention、client assets の生成結果を確認した。
- [ ] staging のインストール / downgrade / rollback 証跡を添付した。
- [ ] 本番 promote と旧配信停止について人間の承認を得た。
