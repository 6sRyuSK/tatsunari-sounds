# Plugin Factory

CMake で構築した自律型オーディオプラグイン・ファクトリーです。**現行プラグインは
すべて CLAP ファースト**構成で、clap-wrapper により CLAP から VST3 / AU を生成し、
UI は JUCE 非依存の Visage 製です（JUCE 製のバイナリを出荷するプラグインはもう
ありません）。
すべての変更が従うべき規約は `CLAUDE.md` を参照してください。

## プラグインカタログ

<!-- BEGIN:CATALOG -->

### Shipped (2)

| Plugin | Category | Version | Formats | Reference |
| --- | --- | --- | --- | --- |
| Dynamic Tatsunari EQ | EQ | 2.0.0 | VST3, AU | Multiband dynamic EQ with per-band detection |
| Resonance TatSuppressor | EQ | 3.1.1 | VST3, AU | Dynamic spectral resonance / harshness suppression |


### In progress (1)

| Plugin | Category | Reference |
| --- | --- | --- |
| Pitch TatFixer | Pitch Correction | Real-time monophonic pitch correction |


### Planned (0)

_None yet._


### Archived (14)

_Not built or released (excluded from CI); sources kept under `archive/plugins/` and may return with reworked DSP. Local opt-in build: `-DFACTORY_INCLUDE_ARCHIVED=ON`._

| Plugin | Category | Reference |
| --- | --- | --- |
| Tatsunari Bus Compressor | Dynamics | VCA-style bus compressor |
| Fuzznari | Fuzz | Gated / octave fuzz distortion |
| Tatsunular Delay | Delay | Granular cloud delay (pitch + tempo-sync) |
| Tatsunari Madoromi | Looper | Micro-loop sampler with parallel effect stages |
| Tatsunari Mochi Stretch | Pitch | Real-time time-stretch / pitch-shift buffer effect |
| Tatsumin Enhancer | Enhancer | Multiband parallel harmonic enhancement |
| Super Tatsunari NAM Player | Amp Sim | Steven Atkinson — Neural Amp Modeler (sdatkinson/NeuralAmpModelerCore v0.5.4) |
| Tatsunari Omoide Echo | Delay | Looping echo with reverse / modifier stages |
| Onsen Delay | Delay | Harmonic glide delay (pitch-shifting feedback) |
| Taturator | Saturation | Analog tape / tube soft-clip |
| Tammer Reverb | Reverb | FDN shimmer (octave-up feedback) |
| Tatsunari Surikire | Lo-Fi | Lo-fi tape degradation (wow / flutter / dropout) |
| Tumble Delay | Delay | Physical-bounce granular echo with sequencer |
| Multi Tatsunari Comp | Dynamics | Vocal-tuned 3-band compressor |

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

## ドキュメント

各プラグインのパラメータ解説・レイテンシ・使い方は `docs/manual/` にあります。

- [Dynamic Tatsunari EQ](docs/manual/dynamic-tatsunari-eq.md)
- [Resonance TatSuppressor](docs/manual/resonance-tatsuppressor.md)

## ソースからビルドする（開発者向け）

### 依存関係のインストール

共通で必要なのは **Git / CMake 3.22 以上 / C++20 対応コンパイラ** の3つです。
CLAP / VST3 SDK / clap-wrapper / Visage などの SDK 類は初回の CMake 構成時に
自動フェッチされるため、手動でのインストールは不要です（初回はネットワーク
接続が必要）。`tools/` の Python スクリプト（カタログ生成など）を使う場合は
Python 3、TUI インストーラーを開発する場合は Go も追加で入れてください。

JUCE も同じ仕組みでフェッチされますが、**出荷バイナリはもう JUCE を使いません**。
JUCE が必要なのは resonance-suppressor と dynamic-eq が残している
byte 等価オラクルのテストだけで、その 2 つを含む構成でのみフェッチされます
（`-DFACTORY_JUCE_ORACLES=OFF` を付ければ常にスキップできます）。

**Windows**:

[Visual Studio 2022](https://visualstudio.microsoft.com/ja/downloads/)（Community で可）を
「**C++ によるデスクトップ開発**」ワークロード付きでインストールしてください。
CMake と Git が同梱されるので、これだけでビルドできます。CI と同じ Ninja 経路を
使う場合は追加で `winget install Ninja-build.Ninja`（または `choco install ninja`）
を入れ、「Developer PowerShell for VS 2022」から実行します
（[`tools/build-clap.ps1`](tools/build-clap.ps1) はこの準備ごと自動化します）。

**macOS**:

```console
xcode-select --install        # Xcode Command Line Tools（コンパイラ + Git）
brew install cmake ninja      # Homebrew: https://brew.sh/ja/
```

**Linux**（ローカル検証専用 — 出荷ターゲットではありません）:

```console
sudo apt-get update && sudo apt-get install -y build-essential git cmake ninja-build libasound2-dev libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev libfreetype-dev libfontconfig1-dev mesa-common-dev libgl1-mesa-dev
```

X11 / freetype / GL の各開発パッケージは Visage GUI と VST3 SDK の構成に必要です。
`libasound2-dev`（ALSA）は JUCE 側のオラクルテストを構成する場合にだけ必要で、
`-DFACTORY_JUCE_ORACLES=OFF` の CLAP 専用ビルドでは不要です。

### クローンとビルド

環境が整ったら、リポジトリをクローンして構成 → ビルド → テストの順に実行します。
必要に応じて以下の CMake 変数を設定してください:

- `-DFACTORY_PLUGINS=<slug>[,<slug>]` — 指定したプラグインだけを構成・ビルド
  します（例: `-DFACTORY_PLUGINS=dynamic-eq`）。省略すると全プラグインが対象です。
  現行プラグインはすべて CLAP ファーストなので、いずれか 1 つでも含む構成が
  CLAP 系 SDK / Visage をフェッチします。
- `-DFACTORY_JUCE_ORACLES=OFF` — resonance-suppressor / dynamic-eq の JUCE 製
  byte 等価オラクルのテストを構成から外し、**JUCE のフェッチごとスキップ**します。
  出荷バイナリ（CLAP / VST3 / AU）には一切影響しません。

**macOS / Linux**（または Ninja が使える環境）:

```console
git clone https://github.com/6sRyuSK/tatsunari-sounds.git
cd tatsunari-sounds
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

**Windows**（Visual Studio 2022）:

```console
git clone https://github.com/6sRyuSK/tatsunari-sounds.git
cd tatsunari-sounds
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

初回の構成は CLAP / VST3 SDK / clap-wrapper / Visage（および JUCE オラクルを
有効にしている場合は JUCE）をフェッチするため数分かかります。2回目以降は
`cmake --build` だけで済み、ソース変更時の再構成も自動で走ります。
Visual Studio ジェネレータはマルチコンフィグなので、`Release` は構成時ではなく
ビルド/テスト時に `--config` / `-C` で指定します。構成に失敗すると `build/` の
キャッシュが壊れることがあるので、その場合は `build/` を削除して 1 からやり直して
ください。

### 補足

- resonance-suppressor の CLAP ビルドは CI と同じ Ninja 経路で検証されています。
  Windows では [`tools/build-clap.ps1`](tools/build-clap.ps1) が VS ツールチェーン
  の準備からビルド（`-Install` でシステムへの配置まで）を自動化します。ビルド済み
  VST3 の一括インストールには [`tools/install.ps1`](tools/install.ps1) が使えます。
- Visage UI の開発は WASM ハーネス [`tools/ui-dev/`](tools/ui-dev/README.md) で
  行います（ローカル専用・CI 非依存。ブラウザでのライブリロード付き）。
