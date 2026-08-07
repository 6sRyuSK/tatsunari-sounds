---
name: clap-shell
description: Work on the CLAP shell layer of this repo — shell/include/factory_shell/ (ClapShellPlugin<Policy>, param/state bridges, IClapEditor, DenormalGuard), each plugin's shell/ClapEntry.cpp Policy, and shell/cmake/FactoryClapPlugin.cmake (SDK pins, make_clapfirst, the <slug>_assets layout). Use whenever a task touches a plugin's Policy, CLAP extensions, state/param bridging, the clap-first CMake assembly, or the clap.yml gate — contains the Policy contract and the load-bearing pins so you don't need to read the shell headers or the S2 migration report.
---

# CLAP シェル層

**出荷経路の中心**。3 機種すべてがこの層の上に乗る:
`plugins/<slug>/shell/ClapEntry.cpp`(機種ごとの Policy)+
`shell/`(全機種共通のジェネリックシェル)+ `shell/cmake/FactoryClapPlugin.cmake`
(SDK ピンと `make_clapfirst_plugins` ラッパ)。

隣接スキル: パラメータ表そのものは `add-param`、プリセット表は `add-preset`、
エディタの中身は `visage-ui`。ここは**その 3 つを CLAP に接続する層**。

## 層の地図

| ファイル | 役割 |
|---|---|
| `ClapShellPlugin.h` | ジェネリック実装(テンプレート)。audio ports / params / state / latency / tail / FTZ 境界 / ブロック粒度のパラメータ配送を持ち、DSP 固有の判断はすべて Policy に委譲 |
| `ClapEntryPoint.h` + `shell/src/FactoryClapEntryPoint.cpp` | `FACTORY_CLAP_ENTRY(Policy)` マクロと、エクスポートされる `clap_entry`。**per-plugin の entry TU はもう無い** |
| `ClapParamBridge.h` / `.cpp` | ParamDesc テーブル → `clap.params` サーフェス。plugin 非依存の**コンパイル済みユニット** |
| `ClapStateBridge.h` / `.cpp` | `clap.state` の save/load を `factory_presets::StateCodec` の上に表現 |
| `ClapEditor.h` | `IClapEditor` の seam(GUI は opt-in)。実装は `factory_ui_visage::ClapEditorHost`(`visage-ui` スキル) |
| `update/` (`factory_update`) | エディタ内更新バッジの状態機械 / HTTP / prefs。各 `*ClapEditor.cpp` が `UpdateUiHost` 経由で配線し、GUI ON のときだけ `factory_update` を link |
| `DenormalGuard.h` | process を包む scoped FTZ/DAZ。**コアは FP モード非依存**なので、denormal 対策はこの境界の責務 |
| `ResizableEditorGeometry.h` | アスペクト/上下限スナップの純粋関数(RS の `clap_shell_test` がオラクル付きで検証) |

## Policy 契約(単一の真実は `ClapShellPlugin.h` 冒頭のコメント)

```cpp
using Core = <plain C++ DSP core>;                 // rs_core::RsCore など

static const clap_plugin_descriptor_t* descriptor();
static std::vector<factory_params::ParamDesc> params();
static const factory_presets::PresetBank&     presetBank();
static std::vector<std::string>               excludeIds();

static constexpr bool kHasSidechain;               // オプショナルなステレオ SC 入力ポート
static bool isClapExposed (const factory_params::ParamDesc&);   // CLAP サーフェス述語
static void migrateState (factory_presets::StateModel&);        // 旧 state 補正フック

static void prepare (Core&, double sampleRate, std::uint32_t maxFrames);
static void process (Core&, const factory_params::ParamStore&,
                     float* L, float* R, const float* scL, const float* scR,
                     std::uint32_t frames);
static std::uint32_t latencySamples (const Core&);
static std::uint32_t primeFrames();                // activate() が流す無音フレーム数

// 任意(コンパイル時検出):
static void reset (Core&);                         // PolicyHasReset
static constexpr bool kHasEditor = true;           // PolicyHasEditor
static std::unique_ptr<factory_shell::IClapEditor>
makeEditor (Core&, factory_params::ParamStore&, factory_presets::PresetSession&,
            const clap_host_t*);
```

**任意メンバの検出はコンパイル時**(`PolicyHasReset` / `PolicyHasEditor`)。`kHasEditor`
を持たない Policy のシェルは GUI 無しビルドとバイト単位で同一 — `get_extension` が
`CLAP_EXT_GUI` に nullptr を返す。

### 3 機種の実値(迷ったらここを見る)

| | RS | pitch-fix | dynamic-eq |
|---|---|---|---|
| `kHasSidechain` | `true` | `false` | `false` |
| `isClapExposed` | `kFlagLegacyJuceOnly` を除外 | 常に true | 常に true |
| `migrateState` | 3.0.0 clean break(`Source/StateMigration.h`) | no-op(clap-first 生まれ) | `DeqStateMigration.h` |
| `latencySamples` | コアから | コアから | 常に 0 |
| `primeFrames` | `1<<15` | `1<<13` | 0 |
| `reset` | — | あり | あり |

## `<X>Ix` + `fillSnapshot` パターン(RT 安全の要件)

```cpp
struct PfIx { int amount, retune, /* ... */; };
PfIx computePfIx (const ParamStore& s) { PfIx ix{}; ix.amount = s.indexOf("amount"); /*...*/ return ix; }

// magic-static。最初の接触は activate() のレイテンシ priming = メインスレッド。
const PfIx& pfIndices (const ParamStore& s) { static const PfIx ix = computePfIx (s); return ix; }

void fillSnapshot (const ParamStore& store, pf_core::PfParamSnapshot& s) noexcept
{
    const PfIx& ix = pfIndices (store);
    s.amount = store.value (ix.amount);
    s.buffer = static_cast<int> (store.value (ix.buffer));   // choice/bool は int へ
}
```

- **`process()` の中で `indexOf` を呼ばない** — 線形の文字列探索。
- **magic-static の初期化をオーディオスレッドで起こさない** — 初回接触が
  `clapProcess` だと `indexOf` の走査に加えてスレッドセーフ static のガードを掴む。
  だから priming(メインスレッド)で先に触っておく。

## state

- 波形フォーマットは `factory_presets::StateCodec`:
  magic(`0x21324356`)+ 長さ + UTF-8 XML + NUL の枠に
  `<PARAMS presetIndex="N" stateVersion="V"><PARAM id="…" value="…"/>…</PARAMS>`。
  値は**実値**を `std::to_chars` の最短往復形で書く。枠は JUCE の
  `copyXmlToBinary` とバイト互換。
- **読みは tolerant**(never throws): 未知の属性/子要素は無視、id 欠落や解析不能な
  PARAM はスキップ → 呼び手が**そのパラメータの default を適用**。magic 不一致 /
  truncation / 非 PARAMS ルートは `nullopt`。
- load の順序は **decode → `migrateState(model)` → store へ書き戻し**(model にあれば
  その値、無ければ descriptor の default = 残留なし)。decode 済み presetIndex は
  呼び手が `PresetSession` に adopt させる。
- `migrateState` は「旧 state の穴埋め」専用。新フィールドを足すのに state コードを
  書く必要はない(欠落は default になる)。

## param(GUI 編集の見せ方)

- CLAP の `clap_id` は **`ParamDesc.uid == fnv1a32(id)`**。`id` を変えると uid が
  変わり、ホストのオートメーションと保存済み state が切れる(`add-param` の hard rule)。
- **個別編集**(ノブのドラッグ): エディタが `beginGesture`/`setFromUi`/`endGesture` で
  ParamStore の host-write キューに積み、シェルが `emitParamEventsToHost` で
  `CLAP_EVENT_PARAM_VALUE` / `PARAM_GESTURE_*` に変換して出す → **DAW がオートメーション
  として記録**。このキューの**消費者はシェルだけ**(エディタが drain してはいけない)。
- **バルク変更**(プリセットロード / A-B): `setFromHost` で直接書き、
  `rescan(VALUES|TEXT)` + `mark_dirty` を出す → **記録しない**。
- プラグインが inactive のとき `process()` が回らないので、エディタは
  `flushEditsIfInactive()`(毎フレーム)でホストに `request_flush` を頼む。
  CLAP は `flush` と `process` が同時に走らないことを保証する。
- 非公開(legacy)パラメータには CLAP id が無いので出力レーンも無い — 黙ってスキップ
  される。

## CMake / SDK ピン(`shell/cmake/FactoryClapPlugin.cmake`)

```
FACTORY_CLAP_TAG           1.2.10
FACTORY_CLAPWRAPPER_COMMIT 35f524b7… (v0.15.1)
FACTORY_VST3SDK_TAG        v3.8.0_build_66
FACTORY_AUV2SDK_TAG        AudioUnitSDK-1.1.0 (Apple のみ)
```

`factory_clap_plugin(<slug> IMPL_TARGET … OUTPUT_NAME … VERSION … AUV2_SUBTYPE_CODE …)`
が `make_clapfirst_plugins` を包む。生成ターゲットは **`<slug>_clap` / `<slug>_vst3`
(+ Apple で `<slug>_auv2`)とアグリゲート `<slug>_all`**、出力は
**`build/<slug>_assets/` にフラット**。

load-bearing な設定(触ると壊れる):

- **`CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES` は使わない** — clap-wrapper の
  `guarantee_cpm()` は CPM 自体を GitHub *release* からダウンロードする。SDK は
  こちらが git/FetchContent でピンして渡す(再現性の面でも正しい)。
- VST3 SDK のサブモジュールは **`base public.sdk pluginterfaces cmake` の 4 つだけ**
  (clap-wrapper 自身のリストと同じ)。`GIT_SHALLOW ON`。
- Windows VST3 は**フォルダ形式バンドル**(`WINDOWS_FOLDER_VST3 TRUE`)でないと
  検証が通らない。
- AUv2: manufacturer `Ttsn` 固定、**`AUV2_INSTRUMENT_TYPE aufx` を明示**
  (省略すると clap-wrapper が `aumu` にして Logic が Instrument 扱いにし、開けない)。
- `CMAKE_POSITION_INDEPENDENT_CODE ON` 必須(static impl → MODULE lib)。
- `vst3_validator` は VST3 SDK 全体を再構成するカスタムターゲット — **ビルドしない**
  (`all` には入っていない)。
- 我々の impl は `clap` + `clap-wrapper-extensions`(どちらも INTERFACE)しか link
  しないので、clap-wrapper の `-Werror` は**継承されない**。

各プラグイン側の `shell/CMakeLists.txt` は: `factory_read_version` で plugin.toml から
version を読む → `<slug>-impl` STATIC(`ClapEntry.cpp`)→ GUI option
`FACTORY_<X>_CLAP_GUI`(既定 ON、visage を引き込む唯一のスイッチ)→
`factory_clap_plugin(...)`。

## ゲート

- **`clap.yml`** — active 3 機種それぞれの Linux レグ。`-DFACTORY_PLUGINS=<slug>`
  + **`-DFACTORY_JUCE_ORACLES=OFF`**(JUCE を fetch しない)でビルドし、native
  `.clap` を **clap-validator 0.3.2** に掛ける。ci.yml が出さない唯一のシグナル。
  **新規プラグインは matrix に slug を足すのが手作業**(`paths` は glob なので
  ヘッダの手登録は不要。push と pull_request のリストは同一に保つ)。
- **`ci.yml`** — wrapper VST3 / AU を pluginval strictness 5(`pluginval-debug` スキル)。
- **`resonance_suppressor_clap_shell`** — シェル層の純粋関数(リサイズ数学の不動点)を
  `factory_shell` だけリンクして検証する CTest。

## 参考

`docs/migration/s2-clap-first.md` に spike 時の実測(生成ターゲット名、アーティファクト
ツリー、clap-validator / pluginval の結果、レイテンシプローブ)が残っている。**ピンと
罠は上に抜粋済み**なので、通常は読まなくてよい。
