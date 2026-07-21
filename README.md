# Plugin Factory

CMake で構築した自律型オーディオプラグイン・ファクトリーです。プラグインは
JUCE 8 製の VST3 / AU として出荷されます。Resonance TatSuppressor のみ
**CLAP ファースト**構成で、clap-wrapper により CLAP から VST3 / AU を生成し、
UI は JUCE 非依存の Visage 製です。
すべての変更が従うべき規約は `CLAUDE.md` を参照してください。

## プラグインカタログ

<!-- BEGIN:CATALOG -->

### Shipped (2)

| Plugin | Category | Version | Formats | Reference |
| --- | --- | --- | --- | --- |
| Dynamic Tatsunari EQ | EQ | 1.2.0 | VST3, AU | FabFilter Pro-Q 4 |
| Resonance TatSuppressor | EQ | 3.0.2 | VST3, AU | oeksound soothe2 |


### In progress (9)

| Plugin | Category | Reference |
| --- | --- | --- |
| Tatsunari Bus Compressor | Dynamics | SSL G-series bus comp |
| Fuzznari | Fuzz | ZVEX Fuzz Factory / Fuzz Face / Big Muff |
| Tatsunular Delay | Delay | Granular cloud delay (pitch + tempo-sync) |
| Tatsumin Enhancer | Enhancer | Waves Vitamin (multiband parallel harmonic enhancer) |
| Super Tatsunari NAM Player | Amp Sim | Steven Atkinson — Neural Amp Modeler (sdatkinson/NeuralAmpModelerCore v0.5.4) |
| Taturator | Saturation | Analog tape / tube soft-clip |
| Tammer Reverb | Reverb | FDN shimmer (octave-up feedback) |
| Tumble Delay | Delay | OP-1 Tombola sequencer × granular echo |
| Multi Tatsunari Comp | Dynamics | Vocal-tuned 3-band compressor |


### Planned (0)

_None yet._

<!-- END:CATALOG -->

## インストール

<!-- BEGIN:BOOTSTRAP -->
### TUI インストーラー（推奨）

ターミナルに下の1行を貼り付けてインストーラーを起動してください。あとは
メニューで欲しいプラグインと形式（**VST3 / AU**）を選ぶだけで、DAW が読み込むフォルダへ
インストールされます（Mac / Windows 対応、更新も同じ手順）。

**macOS**（ターミナル）:

    curl -fsSL https://raw.githubusercontent.com/6sRyuSK/tatsunari-sounds/main/tools/installer/bootstrap/install.sh | bash

**Windows**（PowerShell）:

    irm https://raw.githubusercontent.com/6sRyuSK/tatsunari-sounds/main/tools/installer/bootstrap/install.ps1 | iex

UI は日本語 / 英語のバイリンガルで、OS のロケールに従います。ソースとビルド手順は
[`tools/installer/`](tools/installer/README.md) にあります。
<!-- END:BOOTSTRAP -->

## ビルド

### Windows (Visual Studio 2022)

    # 1. 構成（初回、または build/ を作り直したときだけ。JUCE/NAM に加え
    #    resonance-suppressor 用の CLAP / VST3 SDK / clap-wrapper / Visage も
    #    フェッチされるため数分かかる）
    cmake -B build -G "Visual Studio 17 2022"

    # 2. ビルド（普段はこれだけ。ソース変更時の再構成も自動で走る）
    cmake --build build --config Release --parallel

    # 3. テスト
    ctest --test-dir build -C Release --output-on-failure

Visual Studio ジェネレータはマルチコンフィグなので、`Release` は構成時ではなく
ビルド/テスト時に `--config` / `-C` で指定する。`-G Ninja` は Ninja と
Developer プロンプトが必要（CI 用）。構成に失敗すると `build/` のキャッシュが
壊れることがあるので、その場合は `build/` を削除して 1 からやり直す。

### macOS / Linux（または Ninja が使える環境）

    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build --output-on-failure

### 補足

- `-DFACTORY_PLUGINS=<slug>[,<slug>]` を構成時に付けると、指定したプラグイン
  だけを構成・ビルドできます（例: `-DFACTORY_PLUGINS=dynamic-eq`）。
  resonance-suppressor を含む構成だけが CLAP 系 SDK / Visage をフェッチします。
- resonance-suppressor の CLAP ビルドは CI と同じ Ninja 経路で検証されています。
  Windows では [`tools/build-clap.ps1`](tools/build-clap.ps1) が VS ツールチェーン
  の準備からビルド（`-Install` でシステムへの配置まで）を自動化します。ビルド済み
  VST3 の一括インストールには [`tools/install.ps1`](tools/install.ps1) が使えます。
- Visage UI の開発は WASM ハーネス [`tools/ui-dev/`](tools/ui-dev/README.md) で
  行います（ローカル専用・CI 非依存。ブラウザでのライブリロード付き）。
