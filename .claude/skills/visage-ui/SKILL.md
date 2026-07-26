---
name: visage-ui
description: Build or edit the JUCE-free Visage UI layer in this repo — the factory_ui_visage design system (ui/visage/), every shipping plugin's Visage editor (RsEditor / PfEditor / DeqEditor), the shared ClapEditorHost embedding, theme JSON overlays, and the tools/ui-dev WASM dev loop. Use whenever a task touches Visage widgets, editor layout/drawing, theme-rs.json / factory-default.json, the ui-dev gallery/plugin harnesses, or needs the visage core API (Frame, Canvas, MouseEvent, ApplicationWindow) — contains the full API surface and the load-bearing gotchas so you don't need to read ui/visage headers, the plugin editor sources, or the VitalAudio/visage repo.
---

# Visage UI 実装規約

出荷 UI は 3 機種すべて **JUCE-free の Visage エディタ**。層は 4 つ:

| 層 | 場所 | 役割 |
|---|---|---|
| visage SDK | FetchContent (pin は `ui/visage/CMakeLists.txt` の `FACTORY_VISAGE_GIT_TAG`) | Frame/Canvas/ウィンドウ。コア API は `references/visage-core-api.md` 参照 |
| `factory_ui_visage` | `ui/visage/` (STATIC lib) | 共有デザインシステム: Theme(JSON) + widgets + Fonts + Chrome + **`ClapEditorHost`** |
| プラグインエディタ | `plugins/<slug>/ui/` | `rs_ui::RsEditor` / `pf_ui::PfEditor` / `deq_ui::DeqEditor` + 機種固有 view。visage-free な部分 (Theme/Feed/Models) は headless テスト可 |
| アプリシェル | `tools/ui-dev/` (WASM harness) / `plugins/<slug>/shell/<X>ClapEditor.cpp` (出荷 CLAP) | エディタを窓に載せる。CLAP 側は GUI を link する唯一の TU |

機種ごとの読みどころ: **RS が最も濃い実例**(リサイズ/undo/A-B/アナライザ/ノードパネル)、
**pitch-fix は fixed-size の最小形**(共有ウィジェットのみ)、**dynamic-eq は帯域系の実例**
(`ui/DeqBandPanel.*` / `ui/DeqCurveView.*` / `ui/DeqIcons.h`)。

**visage コア API (Frame/Canvas/イベント/レイアウト/ApplicationWindow) を書く・読むときは
まず `references/visage-core-api.md` を読む** — VitalAudio/visage の examples 全読から
抽出済みで、upstream を読み直す必要はない。

## 鉄則

- `ui/` 層と `ui/visage/` は **JUCE-free**(`juce_*` include 禁止)。Theme / SpectrumModel /
  RsFeed / RsModels は visage-free でもあり、ホストコンパイラで headless テストする。
- 色・ジオメトリは **Theme 経由のみ**。widget コードに hex 直書きやフォント名直書きを
  しない(RS 固有色は `RsExtras` に足して theme-rs.json に写す)。パレットのフォークは禁止。
- **テーマ値の変更は taste = 人間サインオフ対象**(デザイン値は JSON が人間のレビュー面)。
  Quicksand が確定書体(2026-07-17 サインオフ); 既定変更には新たな人間の判断が要る。
- **dirty-region 規律**: visage は `redraw()` された Frame だけ再描画する。静的クローム
  を毎フレーム塗り直さない — アニメーションは自分の view の `draw()` 末尾で `redraw()`
  して自走させ、編集/リサイズ等の実変化時だけ `redrawAll()`。
- パラメータ書き込みは **UI ジェスチャ経路** (`beginGesture`/`setFromUi`/`endGesture`)。
  `setFromHost` はホスト/プリセット/undo 適用専用。store の host-write キューを UI 側で
  drain しない(消費者は CLAP シェル唯一 — undo は `gestureEndCount()` を**観測**する)。
- visage の pin (`20de5946…`) を勝手に上げない。上げる時は S1/S2 の再検証が要る。

## factory_ui_visage ウィジェットカタログ

全て `visage::Frame` 派生、`const Theme&` を参照で持つ(**Theme は widget より長寿**、
hot reload は同一インスタンスを書き換える)。ParamStore 系は index でバインドし、値は
draw 毎に `store.value()` を読む(setter 呼び出し不要)。

| widget | ctor | 用途 / プラグイン向けフック |
|---|---|---|
| `Knob` | `(store, idx, theme, decimals=1)` | ロータリー。`setAccentColour(argb)`, `setNameOverride`, `setDialProfile(textTop, textBottom, nameFontPx, valueFontPx, dialInsetPx)`(<0 で既定; RS 実値: big=`(16,17,12,13,0)` / small=`(14,14,10,11,0)`), `requestValueEntry` |
| `PillToggle` | `(store, idx, theme)` | bool ピルトグル(RS は独自 `RsPillCell` を使用) |
| `Segmented` | `(store, idx, theme)` | Choice のセグメント。`setGlyphs({...})`, `setLabelFontPx` |
| `IconButton` | `(theme, glyph, Mode::momentary\|toggle)` | `onClick` / `onToggle(bool)`, `setDimmed`, `setDirection(reversed)`(A→B 矢印モード), `setGlyph` で復帰 |
| `ValueSetting` | `(store, idx, theme, glyph, caption)` | アイコン+caption+値の行 → クリックで Dropdown。`requestDropdown`, `openMenu()` |
| `LinkSlider` | `(store, idx, theme, caption[, glyph], decimals=1)` | 横スライダ。`setCaptionColumnPx`, `requestValueEntry` |
| `PresetSelectorView` | `(theme)` | `<` 名前 `>` のコンボ。`setMenu(Entry…, sel)` (item/header/separator, `steppable=false` で矢印スキップ), `onChange(itemRow)`, `requestDropdown`, `openMenu()` |
| `Dropdown` | `(theme)` | 共有オーバーレイリスト(visage に combo は無い)。`open(items, selRow, anchorX,Y,W,H)`, `onSelect(itemRow)`, `onClose`, `rowCentreInWindow` |
| `ValueEntry` | `(theme)` | 値直接入力の共有オーバーレイ(`visage::TextEditor` ラッパ)。`open(x,y,w,h, prefill, fontPx, commit)`, `cancelEntry()` |
| `ValueText.h` | — | 値直接入力の **visage-free 側**の契約: `ValueEntryRequest`(rect は **window px** + `prefill` + `fontPx` + `commit`)/ `ValueEntryOpener` / prefill 生成 / 共有 commit(parse → 不正なら**書かず revert** → gestured write)。ホストコンパイラで通るので `value_text_test.cpp` が headless 検証する |
| `SpectrumView` | `(theme, model, sampleRate)` | アナライザ描画。`onTick`(フレーム毎のフィード注入), `setFrozen` |
| `SpectrumModel` | — | JUCE-free/visage-free の数理。`setOrderForSampleRate`(固定 order 禁止), `writeSamples`, `update`, `smoothedDb/peakDb`; `LogFreqAxis`(20Hz–20kHz log), `VerticalAxis` |

補助: `Chrome.h` の `paintBackground` / `paintCard`(warm-white 背景とカード)、
`Fonts.h` の `regularFont(px)` / `boldFont(px)`(常にこれ経由; 書体切替は
`setFontFamilyByName`)、`Icons.h` の `icons::Glyph` + 定義済みグリフ
(quality/channel/link/listen/modeSoft/modeHard = viewBox16, undo/redo/copy/caret =
viewBox24) + `paintGlyph(canvas, glyph, x,y,w,h)`(現在のブラシで描く)。

**ノブの弧は必ず `Knob.h` の共有ヘルパ経由**: `knobAngleForNorm(metrics, norm)` /
`knobNeedleTip` / `fillArcBand(canvas, cx, cy, r, a0, a1, thickness)`(角度はラジアン、
0 = 12 時から時計回り、`a0→a1` で `a1 >= a0`、`r` はリング中心線半径 — dead zone は
`arcEnd → arcStart + 2π` で塗る)。visage の `flatArc` は「中心角+半開き」規約で画面角が
+90° ずれ、広い帯は SDF が崩れるため、`fillArcBand` が ≤~17° の小弧にタイルして解決
済み — 自前で `canvas.arc` を 1 発呼ぶと針とリングが 90° ずれる(既知の再発バグ)。
大きな path fill (アナライザ曲線) は path-fill アトラスを汚染して他の path fill を
黙って落とすので、リング類は path でなく flatArc プリミティブで描く。

## Theme(JSON デザインシステム)

- モデルは `factory_ui_visage::Theme`(pure C++/std)。ブロック: `palette / knob /
  toggle / card / font / segmented / dropdown / iconButton / valueSetting /
  linkSlider / spectrum`。色は `"#aarrggbb"`。既定 = `Theme::defaults()` ==
  `ui/visage/theme/factory-default.json`。
- パースは**strict**: 未知キー・型不一致・色不正は `tryParse` が false + error
  (例外なし、wasm ブリッジ安全)。`fromJsonText/File` は throw 版(テスト用)。
- **プラグインオーバーレイ**: `Theme::applyOverlay(jsonText, error)`(bool 返し、
  失敗で false + `error` 充填)は現在値をシードに部分上書き(全キー任意)。トップ
  レベル `"rs"` オブジェクトは共有スキーマから**無視**され、
  プラグイン側 (`rs_ui::RsTheme` の `RsExtras`) が消費する。RS は
  `plugins/resonance-suppressor/ui/theme-rs.json` を `RsTheme::load` でマージ。
- **スキーマにキーを足すとき**: `Theme.h` の struct + `Theme.cpp` のパーサ/`toJson`/
  `operator==` + `factory-default.json` + roundtrip テスト
  (`ui/visage/tests/theme_roundtrip_test.cpp`, RS 側は
  `plugins/resonance-suppressor/ui/tests/rs_theme_roundtrip_test.cpp`) を揃って更新。
  RS 固有値なら共有スキーマでなく `RsExtras` + `"rs"` ブロックへ。

## エディタ構成パターン(RsEditor が家の型)

`plugins/resonance-suppressor/ui/RsEditor.{h,cpp}` が最も濃い実例で規範(最小形は
`plugins/pitch-fix/ui/PfEditor.{h,cpp}`、帯域系は `plugins/dynamic-eq/ui/DeqEditor.{h,cpp}`)。
要点:

- **所有**: 子 widget は `std::unique_ptr` メンバ + `addChild(ptr.get())`。追加順 =
  描画順。共有オーバーレイ (`Dropdown` → `ValueEntry`) は**最後に add**(最前面)。
- **バインド**: パラメータは文字列 id → `store_.indexOf(id)`。UI の無いレガシー
  パラメータはバインドしないだけで良い。
- **Dropdown 配線**: 各 widget の `requestDropdown` に editor の `presentDropdown` を
  渡す。anchor 座標は `anchor->positionInWindow() − this->positionInWindow()` で
  editor-local に変換し、`dropdown_->open(...)`。`dropdown_` は `resized()` で
  `setBounds(0,0,w,h)`(全面スクリム)。
- **ValueEntry 配線**: `requestValueEntry` → `ValueEntryRequest`(rect は **window px**)
  を editor-local に変換して共有 `valueEntry_->open(...)`。ノード切替・リサイズ時は
  `cancelEntry()`(古い rect に残留させない)。invalid 入力は**書き込まず revert**
  (JUCE の clamp-to-min からの意図的逸脱 — ユーザ要望)。
- **レイアウト**: `resized()` で手動配置。一様スケール `k() = height()/設計高` と
  `S(v) = round(v * k())` を使う(RS は 1069×747 設計 / 471×329..1320×922、開くのは
  706×493。dynamic-eq は 740×520 設計 / 620×440..1280×900。pitch-fix は 920×560 固定。
  いずれも固定アスペクト)。flex layout はエディタ本体では使っていない。
- **draw()**: 背景グラデ + 静的クロームのみ。アナライザ等の動く view は自分で
  `redraw()` 自走し、`curve().onTick` にホスト側の毎フレーム処理(gesture pump 等)を
  掛ける。エディタ本体の `draw()` は実変化時の `redrawAll()` でしか呼ばれない。
- **undo**: `factory_params::UndoStack`。`pumpGestures()` が `store.gestureEndCount()`
  の増分を観測してスナップショット(500ms コアレスは UndoStack 側)。履歴適用は
  `setFromHost` + `applyingHistory_` ガード。プリセット/A-B 切替は
  `onStateReplaced()` = 履歴クリア + 再シード(JUCE `replaceState` 相当)。
- **プリセット/A-B**: エディタは `RsPresetModel` / `RsAbModel`(`RsModels.h`)にだけ
  依存。ハーネスはモック、CLAP シェルは PresetSession / `AbCompareModel`
  (`RsAbState.h`) の実装を渡す。
- **ドライバフック**: `widgetRectInWindow(key)` / `nodeCentreInWindow` 等、Playwright
  が実クリックを狙う window-px ルックアップを editor に生やす(テスト面はここ、
  screenshot assert は `tools/ui-dev/playwright/rs.spec.js`)。
- **データフィード**: audio→UI は `RsFeed`(atomic スペクトラ、lock-free)経由のみ。
  UI→audio の口は Listen ソロと display smoothing だけ。新しい表示データが要るなら
  Feed 契約に足し、synthetic 実装(harness)と `RsFeedFromCore`(実機) の両方を更新。

## CLAP 組み込み(出荷経路)

各機種の `shell/<X>ClapEditor.cpp` が visage を link する**唯一の TU**
(`FACTORY_RS_CLAP_GUI` / `FACTORY_PF_CLAP_GUI` / `FACTORY_DEQ_CLAP_GUI` — いずれも
既定 ON の CMake option、`plugins/<slug>/shell/CMakeLists.txt`)。宣言側ヘッダ
(`<X>ClapEditor.h`)は**わざと visage-free**で、Policy を組む `ClapEntry.cpp` が GUI
フレームワークを引き込まずに include できるようにしている。

### 共有ホストの 3 層から「選ぶ」

CLAP↔Visage のボイラープレート(以前は機種ごとに ~200 行コピーされていた)は
`ui/visage/include/factory_ui_visage/ClapEditorHost.h` に共有化済み:

| 層 | 使う機種 | 中身 |
|---|---|---|
| `VisageClapEditorHost` | (基底) | `ApplicationWindow` 所有、`kNativeApi` 選択、`is_api_supported`/`get_preferred_api`/`set_scale`、create/destroy/setParent、show/hide、posix-fd、**macOS logical/native 分岐**、ホスト拡張取得、inactive-edit フラッシュ |
| `FixedSizeVisageClapEditor` | pitch-fix | 非リサイズ。`(host, store, designW, designH)` |
| `ResizableVisageClapEditor` | RS / dynamic-eq | 一様ズーム + アスペクト固定 + 上下限 + **Logic-AU リサイズループ修正**。`(host, store, EditorGeometry, defaultW, defaultH)` |

**プラットフォーム分岐・リサイズ数学・`request_resize` の再実装は禁止。** バグを見つけ
たら共有ホスト側で直す(= 3 機種同時に効く)。`factory_ui_visage_clap_host` ターゲットが
この層で、`factory_shell` + clap を link する(`factory_ui_visage` 本体は CLAP-free の
まま — WASM ハーネスと headless テストがそれに依存している)。

### プラグイン側が書くのはこれだけ

派生して 3〜4 個のフックを埋める(見本: `plugins/pitch-fix/shell/PfClapEditor.cpp` が
最小形、`plugins/dynamic-eq/shell/DeqClapEditor.cpp` が resizable の最小形):

```cpp
class PfClapEditorImpl final : public factory_ui_visage::FixedSizeVisageClapEditor
{
    // ctor: 基底へ (host, store, kDesignW, kDesignH)。theme_ / feed_ / presets_ を持つ
    visage::Frame* buildEditor() override {
        editor_ = std::make_unique<pf_ui::PfEditor> (theme_, store_, feed_, presets_);
        editor_->setFrameTick ([this] { flushEditsIfInactive(); });  // 毎フレーム
        app_->addChild (*editor_);
        return editor_.get();
    }
    visage::Frame* editorFrame() const override { return editor_.get(); }
    void resetEditor() override { editor_.reset(); }
    void onStateReplacedHook() override { if (editor_) editor_->onStateReplaced(); }
    // resizable のみ:
    void setEditorWindowScale (float s) override { if (editor_) editor_->setWindowScale (s); }
    // 任意フック: onEditorCreated / onEditorDestroying(RS の feed.setDisplayActive 等)
};
```

- **preset model**: `SessionPresetModel`(実 `PresetSession` を叩き、適用後に
  `notifyHostEdited()` + `editor_->onStateReplaced()`)を各 `<X>ClapEditor.cpp` の
  無名 namespace に置く。3 機種で同型。
- **resizable のリサイズ入口**: `editor_->onResizeRequest = [this](w,h){
  requestResizeFromEditor(w,h); }`。エディタ右下のリサイズグリップ(design px、
  スナップ済み)が唯一のリサイズ経路 — **Logic の AU 窓には OS のリサイズ端が無い**
  (ハーネスでは `onResizeRequest` 未設定 = grip 非表示)。
- **ジオメトリ**は定数で渡す: `rs_shell::kRsGeometry`(1069×747、471×329..1320×922)/
  `kDeqGeometry`(740×520、620×440..1280×900)。pitch-fix は `PfEditor::kDesignW/H`
  (920×560)。
- **編集の見せ方**: 個別ノブ = シェルの GUI-edit→CLAP output-event 中継(オートメーション
  記録)、バルク(プリセット/A-B) = `notifyHostEdited()` = `rescan(VALUES|TEXT)` +
  `mark_dirty`(記録しない)。

### なぜ共有化されたか(削るな — 再発防止の知見)

- **CLAP/VST3 の GUI サイズは macOS だけ論理ポイント**。native 系 API を mac で使うと
  Retina で窓が 1/2 になり UI の左上 1/4 しか見えない(Cubase 15/mac で実発生)。
  この分岐は今 `setPluginDimensions` / `hostFacingWidth()` の中にある。
- **dpi<1 の一様ズーム**が visage のサイズ/clamp 数学を壊す(dpi≥1 を暗黙に仮定して
  いた)。`ui/visage/patches/` 0001–0004 + 下記 gotchas 参照。
- **AU のリサイズ連鎖**(`request_resize` → `setFrame` → `set_size`)は不動点条件を
  満たさないと再帰する。`syncWindowScale()` の再入ガードと `inSetSize_` origin ガードが
  それ。clap-wrapper 側は pin v0.15.1 で実装済みを確認。
- `setScale` は **false** を返す(visage が OS DPI を自前解決する)。
- Linux は `app->window()->posixFd()` を返し `onPosixFd` で `processPluginFdEvents()`
  (ホストが fd を pump)。

このクラスのバグは **CTest / pluginval / clap-validator のどれも捕まえない** — 実 DAW
でしか出ない。直し方の作法は CLAUDE.md「Debugging host-GUI bugs」(推測より先に
計測を画面に出す)。

## 開発ループと検証

```bash
./tools/ui-dev/dev.sh              # rs-editor  → http://127.0.0.1:8081 (watch+reload)
./tools/ui-dev/dev.sh --gallery    # widget gallery → :8080
./tools/ui-dev/dev.sh --app pitch-fix  # Pitch Fix → :8082
./tools/ui-dev/dev.sh --app dynamic-eq # Dynamic EQ → :8083
./tools/ui-dev/dev.sh --app pitch-fix --verify
./tools/ui-dev/dev.sh --app dynamic-eq --verify
cd tools/ui-dev/playwright && PLAYWRIGHT_BROWSERS_PATH=/opt/pw-browsers \
  node rs.spec.js http://127.0.0.1:8081/index.html .   # 30 asserts + screenshots
```

- テーマ hot reload: `tools/ui-dev/theme.json`(rs-editor は theme-rs.json を
  `/theme.json` で配信)を編集 → 再ビルド無しで ~0.4s 後にピクセル反映。
- JS ブリッジ(`window.ui` / `window.rs`)で param 駆動・freeze・rect 取得・dropdown
  開閉ができる — 決定的スクリーンショットは `freeze` + 固定フレーム注入で撮る。
  ブリッジ一覧と emsdk/FreeType の pin・sandbox 回避は `tools/ui-dev/README.md`。
- ネイティブテスト(visage 不要、非 Emscripten configure で CTest 登録):
  `factory_ui_visage_theme`(1 case)/ `factory_ui_visage_spectrum_<fs>`(全レート)/
  `factory_ui_visage_value_text`(値入力フローの headless 検証、
  `ui/visage/CMakeLists.txt:210`)/ RS の `resonance_suppressor_theme_roundtrip`
  (theme-rs.json 同期検査)/ `resonance_suppressor_ui_pure`(FREQ 入力パーサ +
  抽出済みレイアウト計算。これらのヘッダが visage-free であることの証明も兼ねる、
  `plugins/resonance-suppressor/CMakeLists.txt:145`)— すべて `ctest` で走る。
  手動実行は各ソース冒頭のコンパイル行(theme 系は JSON パスを引数で渡す —
  既定の相対パスは cwd 依存)。widget の**見た目**の検証はハーネスの Playwright 側で、
  gallery に載せる widget は `tools/ui-dev/gallery/GalleryFrame.{h,cpp}` に追加する。

### 実プラグイン用の共通 Playwright harness

- `tools/ui-dev/common/PluginHarness.{h,cpp}` が `window.ui` の標準 ABI
  (ParamStore、Theme、widget rect、Dropdown、preset)を実装する。新しい bridge で
  同じ C export を複製しない — rs-editor / pitch-fix / dynamic-eq は全て
  `ui_dev_harness::Target` を `attach` するだけで、`rs_*` / `pf_*` / `deq_*` には
  **そのプラグイン固有の状態しか置かない**(RS の undo・A-B、PF の status feed、
  DEQ の band/analyser feed)。gallery だけは plugin editor ではないので自前 bridge
  だが、export する ui_* 名と署名は同一。export 一覧は CMake の
  `FACTORY_UI_ABI_EXPORTS` が単一の真実。
- **Dropdown は名前で開く**: `openNamedDropdown(const std::string&)` /
  `ui.openDropdown("preset")`。gallery `"preset"`/`"valueSetting"`、RS
  `"quality"`/`"channel"`/`"preset"`、PF `"key"`/`"preset"`、DEQ
  `"type"`/`"slope"`/`"chan"`/`"preset"`。共有 ABI の引数にプラグインごとの
  マジックインデックスを持ち込まない。
- editor が独自の theme ドキュメントを持つ場合(RS の `RsTheme` = 共有 Theme +
  `"rs"` extras)は `makeEditorTarget(editor, nullptr)` にして `Target::accent` /
  `Target::reloadTheme` を自分で差す。既定の `useSharedTheme` を使うと extras が
  hot reload で落ちる。
- `tools/ui-dev/common/HarnessPresetModel.h` で実際の `PresetSession` を editor の
  preset model に接続する。
- 新しい Visage editor を完成させるときは `tools/ui-dev/<slug>/` に
  `main.cpp`、決定論的 `SyntheticFeed.h`、薄い plugin bridge を置き、CMake、
  `dev.sh`/`dev.ps1`、`verify.js`、Playwright spec を同時に登録する。
- テストは bridge の `ui.set` だけで済ませず、最低1つの knob/node drag と
  Dropdown row click を real mouse event で通し、スクリーンショット、JS/HTTP
  error、preset exclusions を検証する。
- CI: `clap.yml` が **active 3 機種それぞれ**の Linux レグで clap-first(GUI 込み、
  `FACTORY_JUCE_ORACLES=OFF`)をビルド + native `.clap` を clap-validator、
  `ci.yml` が macOS/Windows ビルド + wrapper VST3/AU を pluginval。
  ui-dev ハーネス自体は CI 外。

## visage 固有の operational gotchas(ハマり実績のある順)

1. `mouseDown`/`mouseDrag` のヒット座標は **`e.position`**(frame-local)。
   `e.relativePosition()` は relative-drag 用の**移動デルタ**で、使うと全ヒットが
   (0,0) に飛ぶ。ドラッグは `lastDragPos_` を自分で持ち position の差分で書く
   (`Knob.cpp` 参照; 感度 250px/全域, Shift=0.25 fine)。
2. シングルクリックは `repeatClickCount() == 1`。**ダブルクリック判定は `>= 2`**
   (`>= 1` にすると全プレスが発火)。
3. `circle(x, y, diameter)` / `ring` / `arc` の x,y は**中心でなく外接矩形の左上**、
   第 3 引数は**直径**。中心 c 半径 r なら `circle(c.x−r, c.y−r, 2r)`。
4. 影は shape 側プリミティブ(`roundedRectangleShadow` 等)で、本体より**先に**描く。
   テキスト2色重ねは同一原点で全文→prefix を上書き(brand の作法、幅測定不要)。
5. Emscripten では native window リサイズ不可(`computeWindowBounds` 非リンク) —
   ハーネスは canvas を最大サイズ固定にし editor sub-rect を `rs.setSize` で再レイアウト。
6. `visage::PopupMenu` / `UiButton` / `VISAGE_THEME_COLOR`+`Palette` は**使わない**
   (OS ネイティブ/別テーマ系)。家の等価物は Dropdown / IconButton / Theme(JSON)。
   `visage::TextEditor` だけは `ValueEntry` の基底として使用(そのため CMake で
   `VisageWidgets` を明示 link)。
7. 新しい `.cpp` は `ui/visage/CMakeLists.txt` の `factory_ui_visage` ターゲットに
   明示列挙(glob ではない)。フォント追加は `ui/visage/fonts/` に置けば埋め込みは
   glob + `add_embedded_resources`(`factory_ui_visage::fonts` 名前空間)。
