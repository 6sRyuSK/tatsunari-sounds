# pitch-fix-eval — non-gated measurement harness

Headless metrics for Pitch TatFixer detection / correction accuracy.
**Not registered in CTest** (see `docs/plans/pitch-fix-detection-accuracy.md` §3 P0 /
§4.4 — thresholds need human sign-off before gate promotion).

## Build

```bash
g++ -O2 -std=c++17 \
  -I ../../core/include -I ../../plugins/pitch-fix \
  -o /tmp/pf_eval eval_metrics.cpp
```

## Run

```bash
/tmp/pf_eval [sampleRateHz]          # default 48000
/tmp/pf_eval --resynth [sampleRateHz] # P5 scaffold: fixed-ratio PSOLA probe
```

Synthetic oracles only for now. Drop real-vocal WAV + f0 CSV support here when
the evaluation set arrives — do not wire it into CTest until Ask-a-human #2.
