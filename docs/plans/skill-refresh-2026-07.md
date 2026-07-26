# スキル改修案 — 2026-07 時点のリポジトリ実態への追従

`.claude/skills/` 全 10 本を、現在の `main`(`a9f3373`)の実態と突き合わせた棚卸しと
改修案。**この文書は提案であり、スキル本体の書き換えはまだ行っていない。**

## 1. なぜ今ズレているか

skills は「他のソースを読まずにこれだけ読めば書ける」ことを契約にしている
(CLAUDE.md「Skills (read these INSTEAD of other sources as reference)」)。
その契約下では、記述が古いことは「情報が足りない」より悪い — エージェントは実態を
確認せずに古い経路を実装してしまう。

7 月後半に土台が二段で変わった:

1. **clap-first 全面化**(#150 / #157 / #158 系): 3 機種すべてが `make_clapfirst`
   シェル + Visage エディタで出荷。JUCE は RS と dynamic-eq の**バイト等価オラクル
   専用**になり、`FACTORY_JUCE_ORACLES` の裏へ。`juce_add_plugin` は 1 つも残って
   いない(`plugins/*/CMakeLists.txt` に FORMATS / Standalone の記述なし)。
2. **共有レイヤの成長**: `factory_params::ParamStore` / `factory_presets::StateCodec`
   + `PresetSession` / `factory_ui_visage::ClapEditorHost` が「プラグイン側で書かない
   もの」を吸収した。

CLAUDE.md と README は #158 で追従済み(`ce4a639`)。スキルはそこで**部分的にしか**
追従しておらず、最終更新日が実態から離れているものが残っている:

| スキル | 最終更新 | 陳腐化度 | 優先度 |
|---|---|---|---|
| `add-param` | 2026-07-17 | **致命的**(主経路が JUCE APVTS) | P0 |
| `add-preset` | 2026-07-09 | **致命的**(主経路が JUCE program API) | P0 |
| `factory-ui` | 2026-07-09 | **高**(出荷 UI の顔をしている) | P0 |
| `visage-ui` | 2026-07-25 | 中(出荷組み込み節が旧 RS 手書き) | P0 |
| `pluginval-debug` | 2026-07-07 | 中(原因表・修正先が JUCE 前提) | P1 |
| `core-primitives` | 2026-07-06 | 中(カタログに 14 ヘッダ欠落) | P1 |
| `write-dsp-test` | 2026-07-04 | 中(テスト種別・API 表に欠落) | P1 |
| `release` | 2026-07-07 | 低(ゲート一覧が古い) | P1 |
| `installer-dev` | 2026-07-04 | 低(トリガ記述が事実誤り) | P1 |
| `new-plugin` | 2026-07-26 | ほぼ最新(微修正) | P2 |
| (新規)`clap-shell` | — | **未整備**(CLAUDE.md が「スキルなし」と明記) | P2 |

---

## 2. P0 — 誤誘導が実害になるもの

### 2.1 `add-param` — 全面書き換え

**現状の記述**: 「1. createParameterLayout(Source/PluginProcessor.cpp)」が主経路。
`ParamDesc` テーブルは *resonance-suppressor だけの例外* とされ、「他プラグインは
従来どおり(fleet 全体の移行は後続フェーズ)」と書かれている。editor 節は
`juce::Slider` + `SliderAttachment` + `factory_ui::styleKnob`、平滑化は
`juce::SmoothedValue`、atomic 配線は `apvts.getRawParameterValue`。

**実態**:

- 3 機種すべてが `ParamDesc` テーブルを単一の真実として持つ —
  `plugins/resonance-suppressor/Source/Params.h`, `plugins/pitch-fix/PfParams.h`,
  `plugins/dynamic-eq/DeqParams.h`。「例外」ではなく**唯一の経路**。
- 出荷側の消費者は CLAP Policy(`plugins/*/shell/ClapEntry.cpp`)。パターンは
  `<X>Ix` インデックスキャッシュ(`store.indexOf(id)` を prime 時に 1 回)+
  `fillSnapshot(store, snap)` → コアのスナップショット。process 内で文字列探索を
  しないことが RT 安全の要件。
- エディタ側は Visage ウィジェットを `store.indexOf(id)` でバインド。APVTS は
  `params/include/factory_params/juce/ApvtsAdapter.h` 経由でオラクルにしか出てこない。
- 平滑化は `juce::SmoothedValue` ではなくコア側(`core/include/factory_core/LinearRamp.h`
  など)。

**改修内容**(章立てを実態順に置換):

1. `<Camel>Params.h` のテーブルに 1 エントリ追加(`floatParam/boolParam/choiceParam`)。
   **id はワイヤ識別子**(CLAP uid = `fnv1a32(id)`、state もこれで引く)→ リネーム・
   削除は保存済みセッションを壊す(major + Ask a human)。追加は末尾、id 使い回し禁止。
2. Policy 配線(`shell/ClapEntry.cpp`): `<X>Ix` に `indexOf` をキャッシュ →
   `fillSnapshot` に 1 行。レイテンシに効くなら `latencySamples()` / `primeFrames()`。
   CLAP に出さない内部 param は `isClapExposed`。
3. コア接続: `prepare()` で確保、`process()` は allocation/lock/syscall なし、
   連続値はコア側でランプ。
4. エディタ(`visage-ui` スキルへ委譲)+ `tools/ui-dev` ハーネスの更新。
5. **RS / dynamic-eq のときだけ**: JUCE オラクルと**同時に**直す。テーブル → APVTS
   生成のパリティは `*_preset` が、DSP の一致は `resonance_suppressor_rscore_equiv` /
   `dynamic_eq_deqcore_equiv` がバイト単位でゲートする(片側だけ直すと必ず赤)。
6. テスト(`write-dsp-test`)+ `plugin.toml` minor bump(PR 時 1 回)+
   `python tools/gen_catalog.py`。
7. **出荷済み機種なら `docs/manual/<name>.md` も更新**(下記 4.3)。
8. `clap.yml` の `paths` は glob なので新ヘッダの手登録は不要(**新規プラグインの
   matrix 追加だけが手作業**)。

### 2.2 `add-preset` — 全面書き換え

**現状の記述**: JUCE program API に相乗り(`ProgramAdapter` + `getNumPrograms` 等の
override)、state は `stateToXml`/`applyStateXml`、セレクタは
`factory_ui::PresetSelectorController`、テストは JUCE リンクの console app
(「processor は必ず `std::make_unique` でヒープ確保」)。

**実態**: 出荷経路は `factory_presets::StateCodec` + `PresetSession`、セレクタは
`factory_ui_visage::PresetSelectorView`(CLAUDE.md「Architecture rules」)。
テーブルは clap-first 機種では plugin ルート(`plugins/pitch-fix/PfPresets.h`)。
`preset_test` は 2 系統ある:

- **headless 版**(scaffold が生成する現行形): `factory_params` + `factory_presets`
  のみリンク(`plugins/pitch-fix/tests/preset_test.cpp`)。
- **JUCE リンク版**(RS / dynamic-eq のオラクル、`FACTORY_JUCE_ORACLES` の裏)。

**記録すべき落とし穴**: RS と dynamic-eq の**バンクは今も `Source/` の下にあり、
出荷シェルがそれを include している** —
`plugins/dynamic-eq/shell/ClapEntry.cpp:30` → `Source/FactoryPresets.h`、
`plugins/resonance-suppressor/shell/ClapEntry.cpp:34-36` → `Source/Params.h` /
`Source/FactoryPresets.h` / `Source/StateMigration.h`。
「`Source/` はオラクル専用」という CLAUDE.md の一般規則の**例外**で、ここを
「オラクルだから触っても出荷に影響しない」と誤読すると出荷バイナリを壊す。

**改修内容**: 章立てを 「`<Camel>Presets.h` テーブル → Policy の `presetBank()` /
`excludeIds()` → `PresetSelectorView`(scaffold 済み)→ headless `preset_test` →
bump」に置換。JUCE program API 節は「オラクル機種の追加作業」として末尾に格納。
**除外リストの hard rule(D4)は現行のまま価値があるので保存** — ただし例示の
nam-player / shimmer は archive 済みなので、active 3 機種の例に差し替えるか
「(archive)」と明記する。ユーザープリセットは `UserPresetStoreFs.h` +
`PresetSession` 経路に書き換え。

### 2.3 `factory-ui` — 位置づけの再定義(トリガの奪い合いを解消)

**現状**: frontmatter が `Build or edit a plugin editor/GUI in this repo ... Use when
writing PluginEditor code, styling knobs/sliders/labels, or picking colours` —
「エディタを書くとき」の一等地を占めている。本文は「全プラグインは同じ kawaii
warm-white ルックを共有する」で始まり、末尾に「Standalone ターゲットが常に生成される
(CMake の FORMATS に scaffold 済み)」とある。

**実態**: `factory_ui` をリンクする出荷物はゼロ。生き残っているのは
`FACTORY_JUCE_ORACLES` 下のオラクルアプリの見た目だけ(CLAUDE.md 記載どおり)。
Standalone / FORMATS / `juce_add_plugin` は active plugin に 1 つも無い。
このまま「エディタを書く」タスクで最初に読まれると、JUCE エディタが書かれる。

**改修内容**:

- frontmatter を **oracle 専用**と明示し、トリガから "editor/GUI 全般" を外す
  (例: `Legacy JUCE look-and-feel used ONLY by the FACTORY_JUCE_ORACLES oracle apps.
  For any shipping or new editor use visage-ui instead.`)。
- 本文冒頭 1 行目に「出荷 UI と新規 UI は `visage-ui`。このスキルは
  `FACTORY_JUCE_ORACLES` のオラクルアプリを触るときだけ」を置く。
- 「GUI 確認」節(Standalone 前提)を削除し、UI 確認は `tools/ui-dev` を指す。
- `PresetSelectorController` 節はオラクル文脈に閉じる。

### 2.4 `visage-ui` — 出荷組み込み節の書き換え + 全機種化

7/25 に ui-dev ハーネス側は追従済みだが、**CLAP 組み込み節が旧世界**のまま。

**現状**: 「`shell/RsClapEditor.cpp` が RS ビルドで visage を link する唯一の TU
(`FACTORY_RS_CLAP_GUI` 下でのみコンパイル)」として、`visage::ApplicationWindow` の
生成・`#if __APPLE__` の logical/native 分岐・`adjustSize`/`setSize`・
`request_resize` の手書き手順を列挙している。

**実態**: その ~200 行は共有化された。
`ui/visage/include/factory_ui_visage/ClapEditorHost.h` が 3 層を提供する:

- `VisageClapEditorHost` — `ApplicationWindow` 所有、API 選択、create/destroy/
  setParent、show/hide、posix-fd、macOS logical/native 分岐、inactive-edit フラッシュ。
- `FixedSizeVisageClapEditor` — 非リサイズ(pitch-fix)。
- `ResizableVisageClapEditor` — 一様ズーム + アスペクト固定 + Logic-AU リサイズ
  ループ修正(RS / dynamic-eq)、`factory_shell::ResizableEditorGeometry` で
  パラメタライズ。

3 機種すべて(および `tools/scaffold_plugin.py:933`)がこれを include し、CMake
ターゲットは `factory_ui_visage_clap_host`。`FACTORY_RS_CLAP_GUI` は
`shell/include/factory_shell/ClapEditor.h:9` のコメントにしか残っていない。
**現行スキルに従うと、既に共有化された CLAP ボイラープレートを再実装する。**

**改修内容**:

1. 「CLAP 組み込み(出荷経路)」を全面置換: 「`ClapEditorHost` の 3 層から**選ぶ**。
   プラグイン側が書くのは(a)エディタ Frame の構築、(b)公開、(c)resizable なら
   window scale の反映だけ」。プラットフォーム分岐・リサイズ数学の**再実装は禁止**、
   直すなら共有ホスト側で 3 機種同時に、と明記。macOS logical/native の罠は
   「なぜ共有化されたか」の理由として残す(再発防止の知見なので削らない)。
2. 冒頭の層テーブルを RS 専用から 3 機種に一般化(`RsEditor` / `PfEditor` /
   `DeqEditor`、DEQ 固有の `DeqBandPanel` / `DeqCurveView` / `DeqIcons` に触れる)。
   「唯一の実例で規範」は「RS が最も濃い実例、PF は fixed-size の最小形、DEQ は
   バンド系の実例」に。
3. ウィジェットカタログに欠落分を追加: `ValueText.h`(visage-free な値入力契約、
   `value_text_test.cpp` で headless 検証)、`ClapEditorHost.h`。
4. ネイティブテスト一覧を実登録名に合わせる:
   `factory_ui_visage_theme` / `factory_ui_visage_spectrum_<fs>` /
   **`factory_ui_visage_value_text`**(`ui/visage/CMakeLists.txt:210`)/
   `resonance_suppressor_theme_roundtrip` / **`resonance_suppressor_ui_pure`**
   (`plugins/resonance-suppressor/CMakeLists.txt:145`)。
5. `dev.sh` のアプリ/ポート一覧は実態と一致(gallery:8080 / rs-editor:8081 /
   pitch-fix:8082 / dynamic-eq:8083)— 変更不要。

---

## 3. P1 — 穴・事実誤り

### 3.1 `pluginval-debug`

- 原因表と「修正は該当プラグインの `Source/`」が JUCE 前提。実態の修正先は
  `shell/ClapEntry.cpp`(Policy)/ `<X>Core.h` / 共有シェル。
- 書き換える行: allocation → `Core::process` と `prepare`;
  state save/restore → `StateCodec` / `PresetSession` / `migrateState`;
  bus layout → Policy の `kHasSidechain` とシェルのバス構成;
  latency/bypass → `latencySamples()` / `primeFrames()`。
  **leak detector / `setLookAndFeel(nullptr)` の行は削除**(JUCE エディタは出荷経路に
  存在しない)。
- 追記すべき事実(`ci.yml:166-205`): 検証対象は `build/<slug>_assets/` に**フラットに**
  出る wrapper VST3 と `.component`(旧 `VST3/` 階層はもう無い)。空発見時は
  `::error::` で落ちる(空振り緑の穴を塞いである)。**native `.clap` は pluginval の
  対象外** — そちらは `clap.yml` の clap-validator。
- 「どのゲートが何を言っているか」の対応表を冒頭に置く(pluginval=wrapper VST3/AU、
  clap-validator=native .clap、CTest=コア/等価/プリセット)。層を間違えて直すのを防ぐ。
- Windows の `Start-Process` 手順は現行のまま有効。`tools/vst3-probe/`(dev 専用の
  Windows VST3 ホストプローブ)への言及を追加。

### 3.2 `core-primitives`

- **カタログに 14 ヘッダ欠落**: `HistoryBuffer.h` `LinearRamp.h` `Madoromi.h`
  `MicroLooper.h` `MochiStretch.h` `MultiResSuppressor.h` `OmoideEcho.h`
  `OnsenDelay.h` `PitchDetector.h` `PsolaShifter.h` `Surikire.h` `TumbleDelay.h`
  `VariPolyphaseResampler.h` `WowFlutter.h`。pitch-fix が依存する
  `PitchDetector` / `PsolaShifter` / `VariPolyphaseResampler` が載っていないのは
  実害が大きい(合成できるのに再実装しかねない)。
- **active / archive の区別**を付ける。`NamRoutingEngine.h` と
  `cmake/NamCore.cmake` の注記は「nam-player は archive 済み、
  `-DFACTORY_INCLUDE_ARCHIVED=ON` でしか構成されない」と明記(今は live に見える)。
- **`core/tests/` の存在を追記**(`primitives_test.cpp` / `linear_ramp_test.cpp` /
  `pitch_detector_test.cpp` / `psola_shifter_test.cpp`)。「新プリミティブのゲートを
  付ける」場所がここだと分かる形にする。ルート CMake が `core/tests` `params/tests`
  `presets/tests` を登録している点も 1 行。

### 3.3 `write-dsp-test`

- `DspInvariants.h` API 表に欠落: `noSubnormals(vec)`、
  `windowEnergy(vec, start, len)`。
- **テスト種別の地図が無い**。現存するのは:
  `<slug>_dsp_<fs>`(headless、全レート)/ `<slug>_preset`(headless 版と JUCE
  リンク版の 2 形)/ `resonance_suppressor_rscore_equiv` ・
  `dynamic_eq_deqcore_equiv`(バイト等価オラクル)/
  `resonance_suppressor_ui_pure` ・ `*_theme_roundtrip` /
  `core/tests` ・ `params/tests` ・ `presets/tests` ・ `ui/visage/tests`。
  「どれを足すタスクなのか」を最初に選ばせる節を追加。
- RS の `tests/clap_shell_test.cpp`(シェル層のテスト)への言及を追加。
- **`-DFACTORY_JUCE_ORACLES=OFF` では等価・プリセットゲートが落ちる**ので、CTest を
  回すときは ON、を明記(CLAUDE.md の規則をスキル側にも)。

### 3.4 `release`

- 「リリース準備チェックリスト」の CI が ci.yml のみ。実態の 4 ワークフローに更新:
  `ci.yml`(build+CTest+pluginval)/ `clap.yml`(**active 3 機種ごとの Linux
  clap-validator**)/ `factory-tools-ci.yml`(`gen_catalog --check` + `tools/tests`)/
  `installer-ci.yml`。
- 追記: release ビルドは `-DFACTORY_JUCE_ORACLES=OFF`、全エントリは kind `clap`
  (`release_plan.py` は `juce_add_plugin` を hard error にする)、zip は **VST3 + AU
  のみで native `.clap` は入れない**(インストーラ対応待ち)。
- `tools/release_plan.py` を触ったら `tools/tests` を回す、を 1 行。
- installer.yml の `workflow_run` 記述は**正しい** — 変更不要。

### 3.5 `installer-dev`

- **事実誤り**: 「`installer.yml` が `release: published` で起動」→ 実際は
  `workflow_run(workflows: ["Release"], types: [completed])` + `workflow_dispatch(tag)`
  (`.github/workflows/installer.yml:15-21`。GITHUB_TOKEN 作成 Release には
  `release: published` が飛ばないため)。`release` スキルは正しく書いているので、
  スキル間で矛盾している状態 → こちらを合わせる。
- smoke 例の `--plugins saturator` が **archive 済み slug**。active な slug
  (`resonance-suppressor` 等)に差し替え。
- モジュール地図はほぼ現行どおり。ルート直下の `apply.go` / `tui_run.go` /
  `apply_test.go` を地図に追加(`main.go` の行に含めるだけで十分)。

---

## 4. P2 — 追加と横断ルール

### 4.1 新規スキル `clap-shell`(提案)

CLAUDE.md が明示している唯一の穴:

> No skill covers the CLAP shell layer yet: for `shell/` read that layer's own
> headers plus `docs/migration/s2-clap-first.md`

つまり出荷経路の中心層だけが「生ソースを読め」になっており、スキル体系の目的
(トークン節約・規約の単一化)から外れている。`add-param` / `add-preset` /
`new-plugin` / `visage-ui` すべてがこの層に触るので、重複記述も生んでいる。

**収録内容案**(3 機種の `ClapEntry.cpp` は 206〜263 行で、規約はほぼ共通):

- `ClapShellPlugin<Policy>` の Policy 契約 — `descriptor()` / `params()` /
  `presetBank()` / `excludeIds()` / `isClapExposed()` / `migrateState()` /
  `prepare()` / `process()` / `reset()` / `latencySamples()` / `primeFrames()` /
  `kHasSidechain` / `kHasEditor` / `createEditor()`。
- `<X>Ix` インデックスキャッシュ + `fillSnapshot` パターン(process 内で
  `indexOf` を呼ばない = RT 安全の要件)、prime での magic-static 初期化。
- state: `ClapStateBridge` / `StateCodec` / `migrateState`(RS の clean-break 例)。
- param: `ClapParamBridge`、GUI 編集 → output-event 中継 vs バルクの
  `rescan(VALUES|TEXT)` + `mark_dirty`。
- editor: `IClapEditor` と `factory_ui_visage::ClapEditorHost` の関係(層の選び方)。
- `DenormalGuard`、`shell/cmake/FactoryClapPlugin.cmake` の SDK ピンと
  `make_clapfirst_plugins` ラッパ、`<slug>_all` ターゲット、`<slug>_assets/` の形。
- ゲート: `clap.yml`(matrix への slug 追加が手作業)、`FACTORY_JUCE_ORACLES=OFF`。
- s2 レポートの load-bearing なピン/罠だけを抜粋して収録(migration 文書は残す)。

`visage-ui` から CLAP 組み込み節を、`add-param` から Policy 配線節を、この
スキルへ寄せて相互参照にすると全体の重複が減る。

### 4.2 new-plugin(微修正のみ)

- 生成物一覧に `<Camel>Models.h` を追加(scaffold は出している)。
- §3 のエディタ節に「`<Camel>ClapEditor.cpp` は `ClapEditorHost` の層を継ぐだけ」
  を 1 行(→ `clap-shell` / `visage-ui` へ)。
- 「Visage UI phase」節が英語で、他が日本語 → 言語を統一。
- 出荷する機種になったら `docs/manual/` を作る、を完了条件に追加。

### 4.3 横断: `docs/manual/` の更新ルール

`docs/manual/README.md` は「各ページはプラグインのパラメータ定義から転記され、
出荷バイナリに追従する」と宣言している(RS / dynamic-eq の 2 本)。しかし
`add-param` / `add-preset` / `release` のどのチェックリストにも登場しない。
→ 「**出荷済み機種のパラメータ/プリセットを変えたら該当マニュアルも直す**」を
`add-param` §仕上げ・`add-preset` §仕上げ・`release` 準備チェックに追加。

### 4.4 横断: frontmatter(トリガ)の整理

description は「どのスキルが選ばれるか」を決めるので、実装本文以上に効く。
現状 `factory-ui` と `visage-ui` が「editor/GUI」で競合し、`add-param` が
「APVTS layout」を名乗っている。P0 の書き換えに合わせて、

- 出荷経路のスキルが勝つ description に(`add-param` は "ParamDesc table, CLAP
  policy wiring, Visage knob"、`add-preset` は "PresetBank table, PresetSession,
  PresetSelectorView")。
- オラクル/レガシー用は description 冒頭で "Legacy JUCE oracle only" と宣言。

### 4.5 横断: 例示に archive 済み slug が残っている

`installer-dev`(saturator)、`add-preset`(nam-player / shimmer / delta)、
`core-primitives`(NAM Player)。active 3 機種の例に置換、または「(archive)」明記。

### 4.6 (任意)ドリフト検知の自動化

今回の棚卸しで見つかった誤りの多くは、**存在しない識別子への言及**という機械的に
検出可能な形をしていた(`createParameterLayout` / `FACTORY_RS_CLAP_GUI` /
`release: published` / archive 済み slug / `Standalone`)。
`tools/` に「スキル内の識別子・パス・CTest 名がリポジトリに実在するか」を照合する
小さなチェッカを置き、`factory-tools-ci.yml` に載せる案。誤りゼロにはできないが、
今回の 8 割は落とせた。**優先度は最後**(まずは中身を正す)。

---

## 5. 実施順序の案

| フェーズ | 内容 | 検証 |
|---|---|---|
| 1 | `add-param` / `add-preset` 全面書き換え、`factory-ui` の再スコープ + frontmatter 整理 | 3 機種の実ファイルと突き合わせ。`gen_catalog --check` は無関係(docs のみ) |
| 2 | `visage-ui` の CLAP 節置換 + 3 機種化、`pluginval-debug` / `write-dsp-test` / `core-primitives` / `release` / `installer-dev` の穴埋め | 各記述の根拠パスを本文に持たせ、記述 → 実ファイルの往復で確認 |
| 3 | `clap-shell` 新設 + 重複の寄せ替え、`new-plugin` 微修正、`docs/manual` ルール追記 | 新スキルだけで `ClapEntry.cpp` 相当が書けるかを実タスクで試す |
| 4 | (任意)ドリフト検知チェッカ | `tools/tests` に unittest |

スキルはコードではないので CI が正しさを保証しない。フェーズ 1〜3 の検証は
**「そのスキルだけを読んだ状態で小さな実タスクを通す」**のが唯一の実証手段
(例: pitch-fix に 1 パラメータ足す ドライラン)。書き換え時は各節に根拠ファイルの
パスを添えておくと、次回の棚卸しコストが下がる。

## 6. 今回は対象外としたもの

- **CLAUDE.md / README**: `ce4a639`(#158)で実態に追従済み。スキル側を CLAUDE.md に
  合わせる方向で矛盾を解消する。
- **`Bootstrap.sh`**: 空リポジトリ用の一回限りスケルトン生成器で「JUCE 8 を fetch」
  と書いてあるが、現在の土台とは別物。スキル体系の外なので本提案では触らない
  (別途、archive か削除の判断が要る)。
- **`docs/migration/s1,s2`**: ピンと罠の記録として現役。`clap-shell` を作るときは
  引用元として残す。
