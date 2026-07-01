# Plugin Factory

Autonomous audio-plugin factory built with JUCE 8 + CMake.
See `CLAUDE.md` for the conventions every change must follow.

## Plugin catalog

<!-- BEGIN:CATALOG -->

### Shipped (0)

_None yet._


### In progress (8)

| Plugin | Category | Reference |
| --- | --- | --- |
| Bus Compressor | Dynamics | SSL G-series bus comp |
| Dynamic Parametric EQ | EQ | FabFilter Pro-Q 4 |
| Granular Delay | Delay | Granular cloud delay (pitch + tempo-sync) |
| Resonance Suppressor | EQ | oeksound soothe2 |
| Saturator | Saturation | Analog tape / tube soft-clip |
| Shimmer Reverb | Reverb | FDN shimmer (octave-up feedback) |
| Single-Band EQ | EQ | RBJ Audio EQ Cookbook — peaking EQ |
| Vocal MB Comp | Dynamics | Vocal-tuned 3-band compressor |


### Planned (0)

_None yet._

<!-- END:CATALOG -->

## Install

Each GitHub Release is a single consolidated build of the whole factory, tagged
`<year>.<n>` (e.g. `2026.1`). Grab either the everything bundle for your OS
(`tatsunari-plugins-<tag>-macOS.zip` / `-Windows.zip`) or an individual
plugin zip (`<plugin>-<version>-macOS.zip` / `-Windows.zip`), then copy the
bundles into your plugin folders:

- **AU** (`.component`) → `~/Library/Audio/Plug-Ins/Components/` (or `/Library/...` for all users)
- **VST3** (`.vst3`) → `~/Library/Audio/Plug-Ins/VST3/` (or `/Library/...`)

### curl で最新版をインストール

Asset filenames embed the release version, so instead of hard-coding a URL the
snippets below ask the GitHub API for the **latest** release and download from
there (only `curl` and `unzip` — no `jq`). Downloading with `curl` also skips the
`com.apple.quarantine` flag a browser attaches, so the macOS "damaged" prompt
below does **not** appear.

#### macOS — everything bundle (AU + VST3, all plugins)

    REPO=6sRyuSK/tatsunari-plugins
    curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" \
      | grep -oE '"browser_download_url": *"[^"]*macOS-(AU|VST3)\.zip"' \
      | cut -d'"' -f4 \
      | while read -r url; do
          case "$url" in
            *macOS-AU.zip)   dest="$HOME/Library/Audio/Plug-Ins/Components" ;;
            *macOS-VST3.zip) dest="$HOME/Library/Audio/Plug-Ins/VST3" ;;
          esac
          mkdir -p "$dest"
          curl -fL "$url" -o /tmp/tp-bundle.zip
          unzip -o /tmp/tp-bundle.zip -d "$dest" && rm -f /tmp/tp-bundle.zip
        done

Swap `macOS-(AU|VST3)\.zip` for `Windows\.zip` (and pick your VST3 folder, e.g.
`%CommonProgramFiles%\VST3`) to grab the Windows bundle instead.

#### macOS — a single plugin

Filter to one plugin's zips by its slug (`resonance-suppressor`,
`dynamic-parametric-eq`, … — the `plugins/<slug>` folder name):

    REPO=6sRyuSK/tatsunari-plugins
    SLUG=resonance-suppressor
    curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" \
      | grep -oE "\"browser_download_url\": *\"[^\"]*${SLUG}-[^\"]*macOS-(AU|VST3)\\.zip\"" \
      | cut -d'"' -f4 \
      | while read -r url; do
          case "$url" in
            *macOS-AU.zip)   dest="$HOME/Library/Audio/Plug-Ins/Components" ;;
            *macOS-VST3.zip) dest="$HOME/Library/Audio/Plug-Ins/VST3" ;;
          esac
          mkdir -p "$dest"
          curl -fL "$url" -o /tmp/tp-plugin.zip
          unzip -o /tmp/tp-plugin.zip -d "$dest" && rm -f /tmp/tp-plugin.zip
        done

Then restart your DAW and rescan (for AU you may also need
`killall -9 AudioComponentRegistrar`).

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
