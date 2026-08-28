# 本番移行 runbook（Phase B 演習 / Phase F 移行）

計画 §8 の Phase B（R2 staging での promote / rollback 演習）と Phase F（本番移行）の
**手順書**。Phase F の exit criteria は「承認済み runbook、監視、旧導線案内が完了」で
あり、この文書がその「承認済み runbook」にあたる。

対象読者は**人間のオペレータ**。自動化エージェントが単独で実行してはならない手順に
は 🔴 を付けてある（鍵の生成・保管、DNS、本番 promote の承認、旧配信の停止）。

---

## 0. 前提と登場物

| 物 | 場所 | 誰が持つ |
|---|---|---|
| R2 バケット `tatsunari-sounds` | Cloudflare（**private**。`*.r2.dev` は無効のまま） | 人間 |
| 配信 Worker | `tools/worker/`（`wrangler deploy` は人間が実行） | 人間 |
| promote / rollback | `tools/promote/promote.py` | CI + 人間承認 |
| CDN ヘッダ検査 | `tools/promote/cdn_check.py` | CI / 手動 |
| bootstrap の pin | `tools/promote/bootstrap.py --check`（CI で常時） | CI |
| minisign active 鍵 / backup 鍵 | 🔴 人間のみ。CI には Environment secret として active のみ | 人間 |

**鍵について（この repo の絶対規則）**: 秘密鍵は生成・表示・保存・コピーのいずれも
リポジトリ側の自動化が行わない。`tools/promote/signing.py` は署名/検証の**配線だけ**を
持ち、鍵そのものには触れない。以下で鍵を扱う手順はすべて 🔴。

### `--store memory` は本番と同じコードを通る

演習は `--store memory` で回す。8 ステップも、上書き拒否も、read-back 検証も、
smoke も**同じコードパス**を通り、バイト列の置き場所だけが dict に変わる。だから
資格情報なしで意味のある演習になる。逆に言えば、演習で落ちるものは本番でも落ちる。

---

## 1. 演習 A: promote

```bash
# 実 zip がある場合
python tools/promote/promote.py publish --store memory \
    --artifacts-dir <release zips> \
    --installer-dir <installer binaries> --installer-version <ver> \
    --out-dir out/

# 実 zip がまだ無い場合（コードパスだけの演習。生成物は「NOT A RELEASE」）
python tools/promote/rehearsal.py \
    --artifacts-dir /tmp/art --installer-dir /tmp/inst --installer-version 0.0.0-rehearsal
python tools/promote/promote.py publish --store memory \
    --artifacts-dir /tmp/art --installer-dir /tmp/inst \
    --installer-version 0.0.0-rehearsal --out-dir out/
```

`.github/workflows/promote-staging.yml`（`workflow_dispatch`）が上をそのまま回す。

本番（Phase F 以降）は GitHub Environment の人間承認付き workflow から:

```bash
python tools/promote/promote.py publish \
    --artifacts-dir dist/ --installer-dir dist-installer/ --installer-version <ver> \
    --out-dir out/ \
    --store s3 --bucket tatsunari-sounds --endpoint-url "$R2_ENDPOINT" \
    --sign --public-key "$MINISIGN_PUBLIC_KEY"
```

**`--installer-dir` を省いてよいのは、公開済み catalog に `client` セクションが
既にあるときだけ**。`catalog.json` の `client.assets[]` は bootstrap ワンライナーが
インストーラ本体を解決する先で、これを欠いた catalog を公開すると全 OS で
`curl | sh` / `irm | iex` が止まる。省略時は前回 catalog から引き継ぎ、どちらも
無ければ promote は**拒否する**。

8 ステップと、各ステップの**中断条件**:

| # | ステップ | 中断する条件 | 中断時の状態 |
|---|---|---|---|
| 1 | validate | zip / installer 名が規約外 / 未知 slug / 1 plugin に複数バージョン | 何も起きていない |
| 2 | upload immutable | 既存 key に**別の digest** | 既存オブジェクトは無傷 |
| 3 | read-back verify | 再取得の digest 不一致 / 取得不能 | manifest 未生成 |
| 4 | generate manifest | plugin.toml と ID 表の不一致 / `client` を引き継げない | ローカル出力のみ |
| 5 | sign | `--sign` なのに鍵が空 / 公開鍵未指定 / `minisign` 不在 / **自己署名が検証できない** | 何も公開していない |
| 6 | smoke（公開前） | latest と catalog の不一致 / 参照先オブジェクト欠落 / client asset 欠落 / 署名欠落 | **何も公開していない** |
| 7 | upload manifest | `catalog.json` の PUT 失敗 | **pointer は旧のまま** |
| 8 | pointer switch | 旧 pointer の history 退避に失敗 | pointer 未切替 |

**設計上の要点**:

- **7 まで落ちても本番は動き続ける。** ユーザに見える変化は 8 だけで、その 8 は
  「旧 pointer を history に退避してから切り替える」。だから rollback は
  「再ビルド」ではなく「署名済みの既知良好ドキュメントの再公開」になる。
- **smoke は公開の前。** 計画 §5 は `upload manifest → smoke` と書いているが、
  それだけでは足りなかった: `latest.json` を最後に切り替えても、**TUI インストーラは
  `catalog.json` を直接読む**ので、smoke で落ちる catalog は既に配信されている。
  現在は候補バイト列に対して公開前に smoke を通し、catalog 公開後にもう一度回す。
- **署名は最初のバイトを公開する前に作って検証する。** 文書を先に置くと、
  (a) 新文書 + 旧署名が配信される窓ができ、(b) その後 `minisign` や署名 PUT が
  失敗すると、その不整合が「窓」ではなく**公開状態そのもの**として残る。署名は
  何も触っていない段階で作るので、ここでの失敗はコストゼロ。
- **署名は文書より先にアップロードする。** 2 オブジェクトを原子的に入れ替えられない
  以上どちらかの順序を選ぶしかなく、この順なら「署名の無い文書」は決して公開されない。
  残るのは逆の組（旧文書 + 新署名）で、クライアントには検証失敗として見え、再取得で
  解消する。pointer が短 TTL なのはこの再取得を安くするためでもある。

**確認すること**:

- [ ] `out/promote-result.json` の `previousPointer` が rollback 先として残っている
- [ ] `signed: true`（本番）/ `false` と理由（演習）が明示されている
- [ ] 同じコマンドを**もう一度**流して、すべて `skip (identical)` になる（再実行安全）
- [ ] artifact の key を 1 つ選び、中身を 1 バイト変えて再実行 → **step 2 で停止**する
- [ ] 生成された `catalog.json` に `client.assets[]` が 3 件あり、それぞれが store に
      存在する（無いと `curl | sh` が全 OS で止まる）
- [ ] `--installer-dir` を省いた 2 回目の promote で `client` が引き継がれている

最後の項目は演習の本体。ここが素通りするなら immutable の保証は無い。

---

## 2. 演習 B: pointer rollback

```bash
python tools/promote/promote.py rollback --to previous --store memory --yes
```

- `--to previous` は history の最新を解決する。特定時刻に戻すときは
  `--to updates/v1/history/latest-<stamp>.json` を明示する。
- `--yes` が無ければ**必ず拒否される**。本番に見える操作を勢いで打たせないため。

**rollback が戻すのは pointer だけ**（計画 §8）。artifact は不変なので消えないし、
すでにインストールされた bundle を自動で巻き戻すこともしない。古い版が要るユーザは
TUI から明示的に選ぶ。

**確認すること**:

- [ ] `--yes` 無しで実行 → 拒否される
- [ ] 実行後の `latest.json` が退避したものと**バイト単位で一致**する
- [ ] 署名も一緒に戻っている（`latest.json.minisig`）
- [ ] rollback 自体も現行 pointer を history に退避している（戻しすぎたら戻せる）

### いつ rollback するか（閾値）

pointer 切替後、以下を監視し、超過したら**即座に** rollback する（計画 §8）:

| 指標 | 閾値の目安 | 見る場所 |
|---|---|---|
| `updates/v1/*` の 5xx 率 | 通常時から有意に上昇 | Worker のログ / Analytics |
| artifact の 404 | 1 件でも異常（immutable は消えない） | Worker のログ |
| hash 不一致の報告 | 1 件でも即 rollback | ユーザ報告 / cdn_check |
| Worker の status | エラー継続 | Cloudflare ダッシュボード |

判断を待つより戻すほうが安い。戻してから原因を見る。

---

## 3. 演習 C: active 鍵の失効 → backup 鍵での署名 🔴

**全手順が人間の作業。** 自動化に鍵を渡さない。

想定する事象: active 秘密鍵の漏洩疑い、あるいは保管媒体の喪失。

1. **止める。** 進行中の promote workflow を停止し、Environment の承認を保留にする。
2. **backup 鍵を用意する。** backup の秘密鍵は active とは別の場所に保管されている
   前提（同じ場所に置いてあるならそれは backup ではない）。
3. **公開鍵を差し替える。** backup の公開鍵を、
   - リポジトリの公開鍵記載箇所、
   - インストーラに焼き込まれた公開鍵（**バイナリの再ビルドと再配布が要る**）、
   の両方で更新する。ここが本番移行前に演習しておくべき最大の理由で、
   **鍵交換はクライアント更新を伴う**。
4. **backup 鍵で再署名する。** 現行の `latest.json` / `catalog.json` を
   `--sign --public-key <backup の公開鍵>` で publish し直す。内容は同じなので
   step 2 はすべて `skip (identical)` になり、変わるのは署名だけ。
5. **検証する。** 新しい公開鍵を持つクライアントで検証が通り、古い公開鍵では
   通らないことを両方確認する。
6. **失効を告知する。** 旧公開鍵をどこで無効としたかを記録する。

**中断条件**: 手順 3 が終わる前に 4 を実行しない。検証できない鍵で署名した
リリースは、全クライアントが拒否する壊れたリリースと区別がつかない
（`smoke` はこれを検出して停止するが、それは最後の砦であって手順ではない）。

**確認すること**:

- [ ] backup の秘密鍵が active と**物理的に別の場所**にある
- [ ] 公開鍵を焼き込んだクライアントの再ビルド手順が書かれている
- [ ] 演習は**テスト用の鍵ペア**で行い、本番鍵で練習しない

---

## 4. 演習 D: CDN ヘッダ検査

```bash
# promote が生成した文書に対して（pointer 切替の前）
python tools/promote/cdn_check.py --from-dir out/

# 本番の実配信に対して
python tools/promote/cdn_check.py --live

# 可変オブジェクトだけ手早く
python tools/promote/cdn_check.py --live --skip-artifacts
```

検査するのは「**エッジが実際に返しているもの**」であって、我々がオブジェクトに
設定したものではない。両者は Page Rules / Cache Rules / Worker の実装で簡単にずれる。

| 種別 | 要求 | ずれたときに起きること |
|---|---|---|
| artifact / history / bootstrap payload | `immutable` かつ `max-age >= 31536000` | 毎回の再ダウンロード、egress 増、失敗率上昇 |
| `latest.json` / `catalog.json` / 署名 / shim / notes | `max-age <= 300`、`immutable` でない、ETag か Last-Modified あり | **rollback が効かない**（キャッシュが切れるまで旧版が配られ続ける） |

後者が本番移行で最も危険な設定ミス。pointer を長期キャッシュに載せると、
「戻したのに戻らない」という最悪の障害になる。

**確認すること**:

- [ ] `--live` が 0 で終わる
- [ ] shim（`/install.sh`）が短 TTL、payload（`/bootstrap/<v>/install.sh`）が
      immutable —— **逆になっていない**
- [ ] Worker を経由しない経路（フォールバック）でも同じヘッダが返る

### Worker フォールバック演習

Worker が不調なとき、route を外して R2 のカスタムドメインを同じホスト名に
バインドすれば、同じ key が同じパスで配信される。失うのは集計だけ。

- [ ] staging で route を外し、`cdn_check --live` が**同じ結果**になることを確認する
- [ ] 戻したあと集計が再開することを確認する

---

## 5. Phase F 本番移行の手順

前提: 演習 A〜D がすべて pass していること。

1. 🔴 **DNS と公開 URL の確定**（計画 §6）。自動化エージェントは実施しない。
2. 🔴 Worker を deploy し、route を有効にする（`cd tools/worker && npx wrangler deploy`）。
3. `cdn_check --live --skip-artifacts` でヘッダを確認する。
4. **新旧どちらのクライアントも読める catalog を先に公開する**（計画 §8
   「compatibility と rollback」）。新クライアントの配布はそのあと。
5. promote を本番 store で実行（GitHub Environment の人間承認付き）。
6. `cdn_check --live` を実行する。
7. §2 の閾値で監視する。超えたら rollback。
8. 🔴 旧配信（GitHub Releases / `raw.githubusercontent.com` のワンライナー）の
   **利用期間と案内方法を人間が承認**してから停止する。停止前に README の
   ワンライナーが新ホストを指していることを確認する
   （`tools/tests/test_bootstrap_pin.py` が常時 gate している）。

---

## 6. リリース判定チェックリスト

計画 §8 のチェックリストに、この repo で実際に走る gate を対応づけたもの。
**すべて埋まるまでリリースしない。**

### 自動で確認できるもの

- [ ] `ci.yml` が green（macOS + Windows、全レート matrix の CTest、pluginval 5）
- [ ] `clap.yml` が green（active plugin ごとの clap-validator）
- [ ] `factory-tools-ci.yml` が green
      （catalog 鮮度 / skill 参照 / ID 一意性 / 旧 identity / bootstrap pin /
      `tools/tests` / Worker のテスト）
- [ ] `installer-ci.yml` が green（`go test` / `go vet`）
- [ ] promote の step 3 read-back verify が全 artifact で通った
- [ ] promote の step 7 smoke が通った（署名を含む）
- [ ] `cdn_check --live` が 0 で終わった

### 人間が確認するもの

- [ ] **schema と公開鍵に差分がない**ことを確認した（あるならクライアント更新の要否を判断した）
- [ ] 全 artifact の read-back hash と、対象 OS/arch が意図どおりであることを確認した
- [ ] `plugin.toml` の `version` を PR 時に**一度だけ**bump してある
      （bump 忘れ = そのプラグインは出荷されない）
- [ ] `stateCompatVersion` の扱いを確認した（state 契約を壊したなら minor + リリースノート）
- [ ] client assets（インストーラ本体）の生成結果を確認した
- [ ] staging での install / downgrade / rollback の証跡を添付した
- [ ] 🔴 本番 promote と旧配信停止について承認を得た
- [ ] 🔴 macOS arm64 の ad-hoc 署名を `codesign` で確認した
- [ ] 🔴 Windows / macOS の実機 smoke test を行った（DAW で読み込み、音が出る）
- [ ] 音・見た目の最終判断を人間が行った（`CLAUDE.md`「Ask a human」1）

### 判定を止める条件

以下のいずれかがあるなら、他が全部緑でもリリースしない:

- 既知の P0 / P1 が残っている（major `0` は品質ゲートを飛ばす免罪符ではない）
- テストの tolerance / oracle / `disabled-tests` を**この PR で緩めた**
- pointer を切り替える前に rollback 先（history）が存在しない
- 署名を有効にしたのに、その署名を公開鍵で検証していない

---

## 7. 関連

- 計画本体: `docs/plans/update-notification-and-distribution/`
  （§5 配信・promote・Worker、§6 ドメイン、§8 フェーズと exit criteria、§11 product identity）
- `tools/promote/` — promote / rollback / manifest / signing / bootstrap pin / cdn_check
- `tools/worker/README.md` — 配信 Worker、集計規則、フォールバック
- `.claude/skills/release/` — バージョニングとリリースパイプラインの機構
- `.claude/skills/installer-dev/` — Go TUI インストーラ
