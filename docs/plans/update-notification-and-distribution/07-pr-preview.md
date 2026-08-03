# 7. PR プレビュー（独立して進行可）

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
4. `tools/ui-dev` は現在意図的に CI 外。独立ワークフローにして
   **落ちても PR をブロックしない**形で始める。
5. **★ paths フィルタは `tools/ui-dev/CMakeLists.txt` の入力を網羅すること。**
   `ui/visage/**` と `plugins/*/ui/**` の 2 つだけでは起動しない変更が多数ある
   （Codex レビュー P2 指摘）。実際の入力は:

   | パス | 根拠 |
   |---|---|
   | `tools/ui-dev/**` | プレビュー自身（ハーネス・shell.html・harness.js・Playwright） |
   | `ui/visage/**` | `add_subdirectory(../../ui/visage)`（14 行目） |
   | `params/**` | 全 4 ターゲットが `params/include` を include |
   | `core/**` | 全 4 ターゲットが `core/include` を include |
   | `presets/**` | pitch-fix / dynamic-eq が `presets/include` を include |
   | `plugins/*/ui/**` | 各プラグインの Visage エディタ本体 |
   | `plugins/resonance-suppressor/Source/Params.h` | rs-editor がパラメータ表をここから取る（`CMakeLists.txt:36` のコメント参照） |

   パラメータ表を変えるとブラウザ上の実エディタに反映されるのに、狭い paths だと
   スクリーンショットが生成されず**変化を見落とす**。

### 7.4 将来案（未決定）

AudioWorklet で実音を鳴らす。`RsCore` / `PfCore` / `DeqCore` はフレームワーク
非依存なので載せられる。PR レビュー用であると同時に、**製品サイトの「ブラウザで
試す」デモに転用可能**。工数は段階 2 より一段重い（AudioWorklet スコープでの
WASM ロード、`-sMODULARIZE`、サンプル配信、SAB 周り）。

---

## ワークフロー詳細

PR の fork 判定後、read-only 権限で Emscripten build を行い、ネットワークアクセス不要の静的
成果物を artifact として保存する。外部公開デプロイは trusted branch のみに限定する。artifact
名へ PR 番号と SHA を含め、保持期限を設定し、同一 SHA の再実行は上書き可能にする。

### paths filter

少なくとも `tools/ui-dev/**`, `ui/**`, `params/**`, `core/**`, `presets/**`, `cmake/**`,
`shell/**`, 対象 `plugins/*/ui/**`, `plugins/*/Source/**`, `plugins/*/shell/**`, 各 CMakeLists と
workflow 自身を含める。固定列挙を避けるため、CI で `tools/ui-dev/CMakeLists.txt` の参照入力を
棚卸しし、filter fixture と比較する。

### screenshot

固定 viewport、DPI、locale、theme、font、seed で各主要画面を撮る。画像差分はレビュー資料で
あり DSP、ホスト統合、アクセシビリティの合否を代替しない。build failure と screenshot failure
を別 status にし、後者でも WASM artifact を調査できるようにする。

## 受け入れ条件

UI ソース、共通 widget、parameter 表、core model、preset、ui-dev 自身の各変更 fixture で workflow
が起動し、文書のみでは起動しないこと。fork PR が secrets を取得できず、生成 HTML に token や
絶対 workspace path が含まれないことを確認する。
