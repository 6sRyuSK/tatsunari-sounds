# Plugin Factory

JUCE 8 + CMake で構築した自律型オーディオプラグイン・ファクトリーです。
すべての変更が従うべき規約は `CLAUDE.md` を参照してください。

## プラグインカタログ

<!-- BEGIN:CATALOG -->

### Shipped (2)

| Plugin | Category | Version | Formats | Reference |
| --- | --- | --- | --- | --- |
| Dynamic Tatsunari EQ | EQ | 1.0.0 | VST3, AU | FabFilter Pro-Q 4 |
| Resonance TatSuppressor | EQ | 1.0.0 | VST3, AU | oeksound soothe2 |


### In progress (6)

| Plugin | Category | Reference |
| --- | --- | --- |
| Tatsunari Bus Compressor | Dynamics | SSL G-series bus comp |
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

プラグインとフォーマット（VST3 / AU）を個別に選んで適切なフォルダにインストールし、
その場で更新できるクロスプラットフォームのターミナルインストーラー（`tatsunari`）です。
**管理者権限なし**で動作し、システム全体へのインストールを選んだときだけ **OS** に
パスワード / UAC プロンプトを要求します（アプリ自身がパスワードを扱うことはありません）。
パスワード不要のユーザー単位インストールには「自分だけ（just me）」を選んでください。

**macOS**（ターミナル）:

    curl -fsSL https://raw.githubusercontent.com/6sRyuSK/tatsunari-sounds/main/tools/installer/bootstrap/install.sh | bash

**Windows**（PowerShell）:

    irm https://raw.githubusercontent.com/6sRyuSK/tatsunari-sounds/main/tools/installer/bootstrap/install.ps1 | iex

UI は日本語 / 英語のバイリンガルで、OS のロケールに従います。ソースとビルド手順は
[`tools/installer/`](tools/installer/README.md) にあります。
<!-- END:BOOTSTRAP -->

## ビルド

    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build --output-on-failure
