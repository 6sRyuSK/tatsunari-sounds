# AGENTS.md

`CLAUDE.md` is the authoritative guide for this repo (architecture, skills, build/
test commands, CI gates, and the "ask a human" rules). Read it first. The root
`README.md` has the developer build quickstart. This file only adds the
Cursor-Cloud-specific environment notes that are not obvious from those sources.

## Cursor Cloud specific instructions

### Environment already provisioned by the startup script
The Linux build/system packages are baked into the VM image (see the README's
Linux `apt-get` list: `ninja-build` + the X11/freetype/fontconfig/GL dev headers,
plus `libasound2-dev` for the JUCE oracles). You do **not** need to reinstall
them.

### Compiler gotcha (non-obvious)
On this image the default `cc`/`c++` alternatives originally pointed at `clang`,
which **cannot find `-lstdc++`** here and makes the very first CMake configure
fail with "C++ compiler is broken". Setup has repointed the `cc`/`c++`
alternatives to `gcc`/`g++` (the Linux compiler the README expects), so the
standard `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release` now works as-is.
If a future configure ever reports the broken-compiler error again, force gcc
explicitly: add `-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++`.

### Build / test / run (Linux, local verification only)
Standard commands live in `README.md` / `CLAUDE.md`. Summary of what is verified
to work on this VM:
- Configure + build: `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release` then
  `cmake --build build`. The first configure fetches the CLAP/VST3/clap-wrapper +
  Visage SDKs (and JUCE 8.0.13 for the RS/dynamic-eq oracles, since
  `FACTORY_JUCE_ORACLES` defaults ON) — it takes a few minutes and needs network.
- Headless DSP + model tests: `ctest --test-dir build --output-on-failure`
  (full 44.1–192 kHz matrix; keep `FACTORY_JUCE_ORACLES=ON` — the equivalence +
  preset oracles live there).
- The FreeType-mirror egress gotcha documented in `CLAUDE.md` did **not** trigger
  on this VM (the configure fetched FreeType fine). If a future run hits
  `Build step for freetype failed`, follow the `-DFACTORY_FREETYPE_MIRROR_DIR`
  workaround in `CLAUDE.md`.

### Running the product (hello-world)
There is no GUI DAW here; Linux is local-verification only and not a shipping
target. The end-to-end "run" for a built plugin is **clap-validator** on the
native `.clap` (this is exactly the unique signal `clap.yml` runs in CI). The
built plugins land at `build/<slug>_assets/<Name>.clap` (e.g.
`build/resonance-suppressor_assets/Resonance TatSuppressor.clap`). Download the
prebuilt validator and run it:
```
gh release download 0.3.2 --repo free-audio/clap-validator --pattern '*ubuntu*'
# extract, then:
clap-validator validate "build/resonance-suppressor_assets/Resonance TatSuppressor.clap"
```
The 5 skipped checks (no preset-discovery-factory / no note-ports) are expected
for an audio effect and are not failures — require exit 0.

### Optional tooling
- Python factory tools (stdlib only, no venv): `python3 -m unittest discover -s tools/tests`.
- Go TUI installer: `cd tools/installer && go test ./...`. `go.mod` pins
  `go 1.24.2`; the VM's `go` is older but Go's `toolchain` mechanism
  auto-downloads 1.24.2 on first use (needs network once).
