---
name: add-param
description: Add or change a parameter on an existing plugin in this repo (ParamDesc table, CLAP policy wiring, core snapshot, Visage control, test, version bump). Use when adding a knob/toggle/setting to a plugin that already exists — contains the end-to-end wiring pattern so you don't need to read other plugins as reference.
---

# 既存プラグインへのパラメータ追加

出荷経路は **clap-first のみ**。パラメータの単一の真実は各プラグインの
`ParamDesc` テーブルで、そこから CLAP サーフェス・state・エディタ・(オラクル機種のみ)
APVTS レイアウトがすべて派生する。**`createParameterLayout` を手書きする経路はもう
存在しない。**

変更箇所は 5 つ: テーブル → CLAP Policy 配線 → コア接続 → エディタ → テスト。
最後に version bump(**minor、PR 作成時に 1 回**)。

| プラグイン | テーブル | Policy | エディタ |
|---|---|---|---|
| tn-resonance-suppressor | `plugins/tn-resonance-suppressor/Source/Params.h`(`resonance_suppressor_params::buildRsParams`) | `plugins/tn-resonance-suppressor/shell/ClapEntry.cpp` | `plugins/tn-resonance-suppressor/ui/RsEditor.h` |
| tn-vocal-tuner | `plugins/tn-vocal-tuner/PfParams.h`(`pitch_fix_params::buildPfParams`) | `plugins/tn-vocal-tuner/shell/ClapEntry.cpp` | `plugins/tn-vocal-tuner/ui/PfEditor.h` |
| tn-equalizer | `plugins/tn-equalizer/DeqParams.h`(`dynamic_eq_params::buildDeqParams`) | `plugins/tn-equalizer/shell/ClapEntry.cpp` | `plugins/tn-equalizer/ui/DeqEditor.h` |

RS のテーブルだけ `Source/` の下にあるが、**出荷シェルがそれを include している**
(`plugins/tn-resonance-suppressor/shell/ClapEntry.cpp:34`)。「`Source/` はオラクル専用」
という一般規則の例外なので、「オラクルだから出荷に影響しない」と誤読しないこと。

## 1. ParamDesc テーブルに 1 エントリ

`params/include/factory_params/ParamDesc.h` の 3 ヘルパで書く。テーブルは
`std::vector<ParamDesc>` を返す関数。

```cpp
// float: (id, name, min, max, interval, default, unit, versionHint[, skewCentre][, flags])
p.push_back (floatParam ("attack", "Attack", 0.1f, 100.0f, 0.0f, 10.0f, " ms", 1, 10.0f));
// bool: (id, name, default, versionHint[, flags])
p.push_back (boolParam ("sc_on", "Sidechain", false, 1));
// choice: (id, name, {labels}, defaultIndex, versionHint[, flags])
p.push_back (choiceParam ("mode", "Mode", { "Soft", "Hard" }, 0, 1));
```

- `interval` 0 = 連続、`skewCentre` 0 = リニア(非 0 は JUCE の `setSkewForCentre`
  と同義)。`unit` は**先頭スペース込みで verbatim**(`" %"` / `" dB"`)。
  default は**実値**(bool は 0/1、choice は index)。
- `uid` はヘルパが `fnv1a32(id)` で自動計算する — 手で書かない。
- flags: `kFlagBypass`(ホスト bypass param、機種に 1 つ)/ `kFlagLegacyJuceOnly`
  (APVTS 側にだけ残す旧 param。CLAP サーフェスには出さない)。

### id はワイヤ識別子(hard rule)

`id` から CLAP param uid(`fnv1a32(id)`)と state のキーが決まる。したがって

- **追加はテーブル末尾**。テーブル順 = ホストに見えるパラメータ順なので、途中に挿すと
  既存 param の順序が動く(オラクル機種では APVTS の `layout.add()` 順も動く)。
- **既存 id の改名・削除は保存済みセッション/プリセットを壊す** → major bump +
  **Ask a human**。id の使い回しは禁止(古い値が新しい意味で読み込まれる)。
- デフォルト付きの純粋な追加は前方互換 → minor。

## 2. CLAP Policy 配線(`plugins/<slug>/shell/ClapEntry.cpp`)

Policy の契約は `shell/include/factory_shell/ClapShellPlugin.h` 冒頭のコメントが単一の
真実。パラメータ追加で触るのは 2 箇所だけ。

```cpp
// (a) インデックスキャッシュに 1 フィールド
struct PfIx { int amount, /* ... */ attack; };
ix.attack = store.indexOf ("attack");

// (b) スナップショット詰めに 1 行
s.attackMs = store.value (ix.attack);
// choice / bool は int へ: s.mode = static_cast<int> (store.value (ix.mode));
```

**`process()` の中で `indexOf` を呼ばないことが RT 安全の要件**(線形の文字列探索。
`ParamStore::indexOf` は `params/include/factory_params/ParamStore.h:73`)。
インデックスは magic-static で 1 回だけ計算し、**最初の接触は必ずメインスレッド**
(`activate()` のレイテンシ priming)に置く — 3 機種すべてこの形。

Policy の他メンバに触るのは、そのパラメータが以下に効くときだけ:

| 効くもの | 触るメンバ |
|---|---|
| レイテンシ(FFT order / lookahead / buffer mode) | `latencySamples(core)` と `primeFrames()` |
| サイドチェイン入力の有無 | `kHasSidechain`(ポート宣言。param では切り替えられない) |
| CLAP に出さない内部 param | `isClapExposed(desc)` を false に(RS は `kFlagLegacyJuceOnly` で判定) |
| state のマイグレーション | `migrateState(StateModel&)` — 旧 state の穴埋めが必要なときのみ |

## 3. コア接続(`<X>Core.h`)

- スナップショット構造体(`PfParamSnapshot` 等)にフィールドを足し、`process()` で
  読む。**`process()` は allocation / lock / syscall 禁止**、バッファは `prepare()` で
  確保(CLAUDE.md「Real-time safety」)。
- **連続値の平滑化はコア側**。`juce::SmoothedValue` は使わない(出荷経路に JUCE は
  無い)— `core/include/factory_core/LinearRamp.h` などの core プリミティブを使う
  (`core-primitives` スキル)。
- `core/` にヘルパを足したら core 変更扱い → **全プラグインのテストを回す**。

## 4. エディタ(Visage)

バインドは id → index の 1 行で、値は draw 毎に `store.value()` が読まれる(setter
呼び出し不要)。ウィジェットの選び方・所有・レイアウト規約は **`visage-ui` スキル**。

```cpp
auto k = std::make_unique<Knob> (store_, store_.indexOf ("attack"), theme_, /*decimals*/ 1);
k->setNameOverride ("ATTACK");
k->requestValueEntry = [this] (const ValueEntryRequest& r) { openValueEntry (r); };
addChild (k.get());
```

書き込みは必ず **UI ジェスチャ経路**(`beginGesture`/`setFromUi`/`endGesture`。
ウィジェットが内部でやる)。`setFromHost` はホスト/プリセット/undo 適用専用。
UI を付けない param は「バインドしないだけ」で良い。

`tools/ui-dev` ハーネスでその機種のエディタを開いて確認する(`visage-ui` スキルの
開発ループ)。

## 5. RS / tn-equalizer のときだけ: JUCE オラクルと同時に直す

この 2 機種は `Source/` に JUCE `AudioProcessor` をオラクルとして残しており、
`FACTORY_JUCE_ORACLES=ON`(既定)で 2 つのゲートが走る:

- `resonance_suppressor_preset` / `dynamic_eq_preset` — テーブル → APVTS 生成
  (`factory_params::buildApvtsLayout`)の **paramdesc parity をビット単位で**照合。
- `resonance_suppressor_rscore_equiv` / `dynamic_eq_deqcore_equiv` — JUCE processor と
  コアの出力が**バイト一致**であることを全レートで照合。

つまり **テーブルだけ直してコア/processor の片側を放置すると必ず赤**。DSP に効く
パラメータは processor 側とコア側を同じ意味で同時に実装する。tn-vocal-tuner には
オラクルが無いので、この節は不要。

## 6. テスト

`write-dsp-test` スキル。新パラメータの効果を **worst-case を含む**設定グリッドで
ゲートに追加する。特に:

- feedback/resonance に効く → 最大値で `impulseResponseNonIncreasing`。
- レンジ端(min/max)で finite + 現実的ピーク上限。
- 量的な効果は独立オラクル(解析値)と照合。

`preset_test`(headless)はテーブル健全性 — id/uid 一意、レンジ非空、default が
レンジ内 — を自動で拾うので、パラメータ数を数えているアサート
(`plugins/tn-vocal-tuner/tests/preset_test.cpp:41`)がある機種は期待値を更新する。

## 7. 仕上げ

1. `plugin.toml` の `version` を **minor** bump(新パラメータ=新機能)。state 互換を
   壊した場合は major。bump はブランチ作業中は行わず **PR 作成時に 1 回だけ**
   (squash-merge 前提。bump 忘れ=リリース対象外)。
2. `python tools/gen_catalog.py`
3. ビルド + 対象プラグインの ctest 全レート緑(core/ を触ったら全プラグイン)。
   CTest を回すときは `FACTORY_JUCE_ORACLES` を **ON のまま**(等価・プリセットゲートが
   そこに居る)。
4. **出荷済み機種なら `docs/manual/<name>.md` も更新**(現在 RS と tn-equalizer の 2 本。
   `docs/manual/README.md` が「パラメータ定義から転記され出荷バイナリに追従する」と
   宣言している)。
5. コミット: `feat(<slug>): Attackパラメータを追加` の形式。

`clap.yml` の `paths` は glob なので、新しいヘッダを手登録する必要は無い(手作業が
必要なのは**新規プラグインの matrix 追加**だけ)。

## ファクトリープリセットとの関係

`PresetSession` は**除外リスト以外の全 param**を管理するので、新パラメータは Init
(program 0)で自動的にデフォルトへ戻る(`presets/include/factory_presets/PresetSession.h`
の applyProgram)。既存プリセットの意図が新パラメータのデフォルトで崩れないか確認し、
必要なら各プリセットに値を足す。ユーザーが選ぶ環境設定/モニタ系なら除外リストへ。
id 改名時は preset テーブル内の参照も直す(古い id は `preset_test` の実在検証で
fail する)。詳細は `add-preset` スキル。
