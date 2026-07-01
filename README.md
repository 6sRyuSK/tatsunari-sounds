# Plugin Factory

Autonomous audio-plugin factory built with JUCE 8 + CMake.
See `CLAUDE.md` for the conventions every change must follow.

## Plugin catalog

<!-- BEGIN:CATALOG -->

### Shipped (0)

_None yet._


### In progress (9)

| Plugin | Category | Reference |
| --- | --- | --- |
| Bus Compressor | Dynamics | SSL G-series bus comp |
| Dynamic Parametric EQ | EQ | FabFilter Pro-Q 4 |
| Granular Delay | Delay | Granular cloud delay (pitch + tempo-sync) |
| NAM Player | Amp Sim | Steven Atkinson — Neural Amp Modeler (sdatkinson/NeuralAmpModelerCore v0.5.4) |
| Resonance Suppressor | EQ | oeksound soothe2 |
| Saturator | Saturation | Analog tape / tube soft-clip |
| Shimmer Reverb | Reverb | FDN shimmer (octave-up feedback) |
| Single-Band EQ | EQ | RBJ Audio EQ Cookbook — peaking EQ |
| Vocal MB Comp | Dynamics | Vocal-tuned 3-band compressor |


### Planned (0)

_None yet._

<!-- END:CATALOG -->

## Install

<!-- BEGIN:BOOTSTRAP -->
### TUI インストーラー（推奨 / recommended）

A cross-platform terminal installer (`tatsunari`) that lets you pick individual
plugins and formats (VST3 / AU), installs them into the right folders, and
updates in place. It runs **unelevated** and asks the **OS** for a password /
UAC prompt only when you choose a system-wide install — the app never handles
your password itself. Choose "just me" for a no-password, per-user install.

**macOS** (Terminal):

    curl -fsSL https://raw.githubusercontent.com/6sRyuSK/tatsunari-plugins/main/tools/installer/bootstrap/install.sh | bash

**Windows** (PowerShell):

    irm https://raw.githubusercontent.com/6sRyuSK/tatsunari-plugins/main/tools/installer/bootstrap/install.ps1 | iex

The UI is bilingual (Japanese / English, following your OS locale). Source and
build notes live in [`tools/installer/`](tools/installer/README.md).
<!-- END:BOOTSTRAP -->

### Manual install

Each GitHub Release is a single consolidated build of the whole factory, tagged
`<year>.<n>` (e.g. `2026.1`). Grab either the everything bundle for your OS
(`tatsunari-plugins-<tag>-macOS.zip` / `-Windows.zip`) or an individual
plugin zip (`<plugin>-<version>-macOS.zip` / `-Windows.zip`), then copy the
bundles into your plugin folders:

- **AU** (`.component`) → `~/Library/Audio/Plug-Ins/Components/` (or `/Library/...` for all users)
- **VST3** (`.vst3`) → `~/Library/Audio/Plug-Ins/VST3/` (or `/Library/...`)

<!-- BEGIN:INSTALL -->
### curl でインストール

The commands below install the **everything bundle** (all plugins) for **all
users**. Asset filenames embed the release version, so replace `2026.2` /
`v2026_2` with the tag you want from the
[Releases page](https://github.com/6sRyuSK/tatsunari-plugins/releases).
Downloading with `curl` also skips the `com.apple.quarantine` flag a browser
attaches, so the macOS "damaged" prompt below does **not** appear. Restart your
DAW and rescan afterwards.

#### macOS — AU (`.component`)

    curl -fL https://github.com/6sRyuSK/tatsunari-plugins/releases/download/2026.2/tatsunari-plugins-v2026_2-macOS-AU.zip -o /tmp/tp-au.zip
    sudo unzip -o /tmp/tp-au.zip -d /Library/Audio/Plug-Ins/Components
    rm -f /tmp/tp-au.zip

#### macOS — VST3 (`.vst3`)

    curl -fL https://github.com/6sRyuSK/tatsunari-plugins/releases/download/2026.2/tatsunari-plugins-v2026_2-macOS-VST3.zip -o /tmp/tp-vst3.zip
    sudo unzip -o /tmp/tp-vst3.zip -d /Library/Audio/Plug-Ins/VST3
    rm -f /tmp/tp-vst3.zip

#### Windows — VST3 (`.vst3`, PowerShell as Administrator)

    Invoke-WebRequest https://github.com/6sRyuSK/tatsunari-plugins/releases/download/2026.2/tatsunari-plugins-v2026_2-Windows.zip -OutFile tp-win.zip
    Expand-Archive tp-win.zip -DestinationPath "C:\Program Files\Common Files\VST3" -Force
    Remove-Item tp-win.zip

The bundles are **not code-signed or notarized**, but each release ships a
build-provenance attestation (`gh attestation verify`) and `SHA256SUMS.txt` — see
the release notes to verify a download before trusting it.
<!-- END:INSTALL -->

### macOS: "「…」は壊れているため開けません" / "…is damaged and can't be opened"

The release binaries are **not code-signed or notarized**, and macOS attaches a
quarantine flag (`com.apple.quarantine`) to anything downloaded via a browser.
Gatekeeper then reports the unsigned bundle as *damaged* — the plugin is fine,
it is just quarantined. Strip the flag after installing:

    # Adjust the names/paths to what you installed (use ~/Library if you installed per-user).
    sudo xattr -dr com.apple.quarantine "/Library/Audio/Plug-Ins/Components/Resonance Suppressor.component"
    sudo xattr -dr com.apple.quarantine "/Library/Audio/Plug-Ins/VST3/Resonance Suppressor.vst3"
    sudo xattr -dr com.apple.quarantine "/Library/Audio/Plug-Ins/Components/Dynamic Parametric EQ.component"
    sudo xattr -dr com.apple.quarantine "/Library/Audio/Plug-Ins/VST3/Dynamic Parametric EQ.vst3"

Then restart your DAW and rescan (for AU you may also need
`killall -9 AudioComponentRegistrar`).

## Build

    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build --output-on-failure
