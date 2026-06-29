# Plugin Factory

Autonomous audio-plugin factory built with JUCE 8 + CMake.
See `CLAUDE.md` for the conventions every change must follow.

## Plugin catalog

<!-- BEGIN:CATALOG -->

### Shipped (0)

_None yet._


### In progress (7)

| Plugin | Category | Reference |
| --- | --- | --- |
| Bus Compressor | Dynamics | SSL G-series bus comp |
| Dynamic Parametric EQ | EQ | FabFilter Pro-Q 4 |
| Granular Delay | Delay | Granular cloud delay (pitch + tempo-sync) |
| Saturator | Saturation | Analog tape / tube soft-clip |
| Shimmer Reverb | Reverb | FDN shimmer (octave-up feedback) |
| Single-Band EQ | EQ | RBJ Audio EQ Cookbook — peaking EQ |
| Vocal MB Comp | Dynamics | Vocal-tuned 3-band compressor |


### Planned (0)

_None yet._

<!-- END:CATALOG -->

## Build

    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build --output-on-failure
