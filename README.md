# Plugin Factory

JUCE 8 + CMake で構築した自律型オーディオプラグイン・ファクトリーです。
すべての変更が従うべき規約は `CLAUDE.md` を参照してください。

## プラグインカタログ

<!-- BEGIN:CATALOG -->

### Shipped (3)

| Plugin | Category | Version | Formats | Reference |
| --- | --- | --- | --- | --- |
| Dynamic Tatsunari EQ | EQ | 1.2.0 | VST3, AU | FabFilter Pro-Q 4 |
| Tatsumin Enhancer | Enhancer | 1.0.0 | VST3, AU | Waves Vitamin (multiband parallel harmonic enhancer) |
| Resonance TatSuppressor | EQ | 1.2.0 | VST3, AU | oeksound soothe2 |


### In progress (7)

| Plugin | Category | Reference |
| --- | --- | --- |
| Tatsunari Bus Compressor | Dynamics | SSL G-series bus comp |
| Fuzznari | Fuzz | ZVEX Fuzz Factory / Fuzz Face / Big Muff |
| Tatsunular Delay | Delay | Granular cloud delay (pitch + tempo-sync) |
| Super Tatsunari NAM Player | Amp Sim | Steven Atkinson — Neural Amp Modeler (sdatkinson/NeuralAmpModelerCore v0.5.4) |
| Taturator | Saturation | Analog tape / tube soft-clip |
| Tammer Reverb | Reverb | FDN shimmer (octave-up feedback) |
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

    # 1. 構成（初回、または build/ を作り直したときだけ。JUCE/NAM のフェッチで数分かかる）
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
