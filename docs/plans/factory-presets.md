# 全プラグイン ファクトリープリセット導入プラン

対象: 全 10 プラグイン(`plugins/*`)+ 共有基盤の新設(`presets/`、`ui/`)+
`tools/scaffold_plugin.py` + skills。**このドキュメントに列挙した作業のみを
実装すること(スコープ厳守)。** CLAUDE.md の全ルール(リアルタイム安全、
検証哲学、Ask a human)が前提。

ゴール: 各プラグインが少数の吟味されたファクトリープリセットを内蔵し、
(1) ホストのプログラムリスト(VST3 / AU factory presets)と
(2) エディタ上の共通プリセットセレクタの両方から選べる状態にする。
ユーザープリセット(保存/読込/ファイル管理)は**スコープ外**。

---

## 背景: 現状の実測事実

- 全 10 プラグインが同一パターン: `AudioProcessorValueTreeState apvts` が
  単一の状態源、`getStateInformation`/`setStateInformation` は APVTS の
  XML round-trip、program API は全てスタブ(`getNumPrograms() = 1`、
  `setCurrentProgram` no-op)。スタブは `tools/scaffold_plugin.py` の
  テンプレート由来なので、新規プラグインにも同じ穴が再生産される。
- パラメータ数はプラグインごとに大きく異なる:
  saturator 4 個 〜 shimmer-reverb 15 個、dynamic-eq は
  `pid(b, "…")` 形式で帯域ループ生成(16 種 × kNumBands + `bypass`)。
- `nam-player` は APVTS ツリー内に **ファイルパス状態**(`files` 子ツリー:
  モデルスロット / IR パス)を持つ。ファクトリープリセットがユーザー環境の
  ファイルパスを含むことはできない。
- 共有コードは `factory_core`(JUCE 非依存・header-only)と
  `factory_ui`(JUCE 依存・header-only)の 2 つの INTERFACE lib。DSP テストは
  `factory_core` のみリンク(JUCE なし・headless)。
- `ci.yml` のパスフィルタは `plugins/** core/** ui/** cmake/** CMakeLists.txt`
  (push / pull_request で同一リスト維持が必須)。

## 設計方針(決定事項)

### D1. データ形式 — JUCE 非依存の plain ヘッダ

プリセットは各プラグインの `Source/FactoryPresets.h` に **JUCE 非依存の
constexpr テーブル**として置く。共有型は新設の
`presets/include/factory_presets/PresetBank.h` に定義:

```cpp
namespace factory_presets {
struct PresetParam { const char* paramID; float value; };   // value は実値(正規化前)
struct Preset      { const char* name; const PresetParam* params; int numParams; };
struct PresetBank  { const Preset* presets; int numPresets; };
}
```

- JUCE 非依存にする理由: headless テストからそのまま include して構造検証
  できる(DSP テストは JUCE をリンクしない、の規約を壊さない)。
- 生成機構(JSON→ヘッダ等)は**作らない**。テーブルは小さく、diff レビュー
  可能で、ビルド機構の追加に見合わない。

### D2. 適用機構 — JUCE program API に相乗りする共有アダプタ

`presets/include/factory_presets/ProgramAdapter.h`(JUCE 依存・header-only)に
`factory_presets::ProgramAdapter` を新設。各 Processor はこれを 1 メンバ持ち、
program API 5 関数をスタブから委譲に差し替える:

- `getNumPrograms()` = **1(Init)+ バンク数**。program 0 は常に "Init"
  (全パラメータをデフォルト値へ)。ホストはロード直後 program 0 を選ぶ
  ことがあるため、**program 0 = 現行デフォルト音**が後方互換の要。
- `setCurrentProgram (i)`: 除外リスト(D4)以外の**全**パラメータについて、
  プリセットに記載があればその値、なければデフォルト値を
  `RangedAudioParameter::convertTo0to1` → `setValueNotifyingHost` で適用。
  決定的(「前のプリセットの残り香」が出ない)。値はレンジでクランプされる。
- **RT 安全**: ホストが `setCurrentProgram` をどのスレッドから呼んでも
  良いよう、適用パスは割当・ロック・syscall なし(atomic な current index +
  パラメータの atomic 書込のみ)。`apvts.replaceState` / ValueTree 操作は
  **使用禁止**(message-thread 前提のため)。
- `changeProgramName` は no-op のまま(ファクトリープリセットは不変)。
- current index は `std::atomic<int>`。永続化は `getStateInformation` が
  XML に属性 `presetIndex` を**追記**し、`setStateInformation` が読み戻す
  (message thread 上なので安全)。属性欠落 → 0。**追記のみなので既存
  セッションの状態互換は維持(major 不要)**。
- プリセット選択後にノブを動かしても index は保持(業界慣行の "dirty"
  扱い。「変更済み」マーカー表示はスコープ外)。
- 適用は通常のパラメータ経路を通るため `SmoothedValue` 平滑化(規約 F)が
  そのまま効く。discrete/choice パラメータの切替は手動切替と同じ挙動。

### D3. エディタ UI — factory_ui の共通セレクタ

`ui/include/factory_ui/PresetSelector.h` に共通ウィジェットを新設
(`ComboBox` + 前/次矢印ボタン)。配色・寸法は `FactoryLookAndFeel` の
既存パレットのみ(プラグイン個別パレット禁止の規約どおり)。

- 配置: 家スタイルの上段 26px 行(タイトル ↔ Bypass の間)。
- 双方向同期: ホスト側の program 変更を `AudioProcessorListener::
  audioProcessorChanged (programChanged)` で受けてコンボを追従、ユーザー
  選択時は `setCurrentProgram` + `updateHostDisplay (withProgramChanged)`。

### D4. プリセットが触ってはいけないパラメータ(除外リスト)

- `bypass` — 全プラグイン共通で除外(プリセットで音を止めない)。
- モニタリング系: `delta`(multiband-enhancer / resonance-suppressor)、
  dynamic-eq の `lsn`(listen)。プリセットはモニタ状態を変えない。
- `nam-player` の `files` ツリー(モデル/IR パス)と各スロット選択 —
  プリセットは knob パラメータ(`in_trim` / `tone_*` / `mix` 等)のみ。
- 除外リストは各プラグインの `FactoryPresets.h` に明示定数として書く
  (アダプタに渡す)。

### D5. 検証 — 新テストカテゴリ「wiring テスト」(⚠ Ask a human #2)

プリセットの typo(存在しない paramID)・レンジ外値は**静かに壊れる**ので、
JUCE をリンクする軽量テスト `plugins/<slug>/tests/preset_test.cpp` を新設する
(Processor をヘッドレス構築し console app で検証)。アサート内容:

1. プリセット名が非空・一意。
2. 全エントリの `paramID` が APVTS レイアウトに実在する。
3. 全エントリの値がレンジ内(適用 → 読み戻しで**一致**。クランプが起きたら
   fail = 独立オラクルはレイアウト宣言そのもの)。
4. program 0 適用後の全パラメータ == デフォルト値。
5. `setCurrentProgram(i)` → `getCurrentProgram() == i`、state 保存 → 復元で
   `presetIndex` round-trip。

これは「DSP テストは factory_core のみリンク」という既存テスト構造への
**追加**(既存 DSP テストの変更・緩和は一切なし)だが、CLAUDE.md の
Ask a human #2(テスト基盤の変更)に該当するため、**このプランの承認をもって
新カテゴリ追加の人間承認とする**。承認されない場合の代替: Debug ビルドの
`jassert` + pluginval のプログラム走査のみ(検出力は落ちる)。

既存ゲートとの関係: pluginval strictness 5 は全ビルドフォーマットで
program 走査・割当チェックを含むため、適用パスの RT 安全性/クラッシュは
既存 CI ゲートでも検証される。DSP テスト(サンプルレートマトリクス)は
一切触らない。

### D6. プリセットの中身は taste(⚠ Ask a human #1)

パラメータ値と名前の決定は**音の判断**。実装側は各プラグインの reference
機材の定番設定に基づくドラフト(1 プラグイン 4〜8 個)を提案するが、
**Standalone ビルドでの人間の試聴サインオフなしにマージしない**。
プリセット名は英語(コード/識別子は英語の規約に合わせる)。

## 工程

実装順序は必ず **Step 1 → 2 → 3 → 4**。各プラグインの配線+プリセット+
テスト+version bump は同一コミット(bump は変更と同じコミットの規約)。

### Step 1 — 共有基盤(PR 1 本)

1. `presets/include/factory_presets/{PresetBank.h, ProgramAdapter.h}` 新設。
   ルート `CMakeLists.txt` に `factory_presets` INTERFACE lib を
   `factory_core`/`factory_ui` と同型で追加。
2. `ui/include/factory_ui/PresetSelector.h` 新設。
3. `ci.yml` の **push / pull_request 両方**のパスフィルタに `presets/**` を
   追加(同一リスト維持)。「Select plugins to build」の共有コード判定にも
   `presets/` を追加(共有変更 → 全プラグインビルド)。
4. `tools/scaffold_plugin.py` テンプレート更新: Init のみの空バンク +
   アダプタ配線 + `preset_test.cpp` を新規プラグインの標準装備にする。
5. skills 更新: 新 skill `add-preset`(プリセット追加/変更の end-to-end 手順、
   D1〜D6 の規約を収録)、`new-plugin` / `factory-ui` / `add-param` に参照を
   追記(param 追加時は既存プリセットの要否確認、を `add-param` に明記)。

### Step 2 — パイロット: saturator(PR 1 本)

最小パラメータ(4 個)の saturator で全経路を通す:
program API 委譲、`FactoryPresets.h`(ドラフト例: Init / Tape Warmth /
Tube Drive / Parallel Glue / Full Crunch)、エディタへ `PresetSelector`、
`preset_test.cpp`、`plugin.toml` **minor bump**(0.1.4 → 0.2.0)+
`python tools/gen_catalog.py`。CI green + **人間の試聴サインオフ**(D6)。

### Step 3 — 残り 9 プラグイン展開(複雑度順、2〜3 プラグインずつ PR)

| 順 | プラグイン | bump | 留意点 |
|---|---|---|---|
| 1 | vocal-mbcomp | 0.1.4 → 0.2.0 | 6 param。用途別(Lead / BGV / Podcast 等) |
| 2 | bus-compressor | 0.1.4 → 0.2.0 | SSL 定番設定(Mix Glue / Drums 等) |
| 3 | multiband-enhancer | 0.1.0 → 0.2.0 | `delta` 除外。`quality` は含めてよい |
| 4 | fuzznari | 0.1.0 → 0.2.0 | `stab`/`osc` の発振領域はプリセットでも可(音色として意図的)だが試聴必須 |
| 5 | saturator 済み → granular-delay | 0.1.4 → 0.2.0 | `sync`/`division` を含む(テンポ同期プリセット) |
| 6 | shimmer-reverb | 0.1.4 → 0.2.0 | 15 param。`freeze` は除外検討(モニタ的挙動)→ 除外して開始 |
| 7 | resonance-suppressor | 1.0.0 → 1.1.0 | shipped。`delta` 除外。mode 別(Vocal / Full Mix) |
| 8 | nam-player | 0.2.1 → 0.3.0 | **ファイル状態に触らない**(D4)。tone/trim/mix のみ |
| 9 | dynamic-eq | 1.0.0 → 1.1.0 | shipped・最大規模。`pid(b,…)` で帯域を明示指定、`lsn` 除外。Vocal De-Harsh / Kick Punch 等 |

各 PR で catalog 再生成。shipped 2 本(resonance-suppressor / dynamic-eq)は
minor bump = 次回リリースの再ビルド対象になることを認識して進める。

### Step 4 — リリース(⚠ Ask a human #3)

version bump が ship trigger なので、Step 2/3 マージ済み分は次回
`workflow_dispatch` リリースで自動的に再ビルド対象になる。リリース実行の
判断・dispatch は人間のみ。

## Ask a human まとめ(このプランの承認ポイント)

1. **D5**: JUCE リンクの `preset_test` 新カテゴリ追加の可否。
2. **D6**: 各プラグインのプリセット内容(値・名前・個数)の試聴サインオフ
   (Step 2 以降、PR ごと)。
3. **Step 4**: リリース dispatch。
4. 既定選択の確認: program 0 = Init 固定 / ユーザープリセット機能なし /
   「変更済み」マーカーなし — 異論があればここで。
