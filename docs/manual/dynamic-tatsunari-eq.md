# Dynamic Tatsunari EQ — User Manual

A 24-band dynamic parametric EQ. Every band is an independent bell / shelf /
high-pass / low-pass filter that can additionally react to level (per-band
dynamics). This page documents every parameter; the values come straight from
the plugin's parameter definitions, so it stays accurate to the shipped binary.

> Formats: VST3 (macOS / Windows), AU (macOS). See the repository README for
> installation.

## Signal flow at a glance

- The 24 bands are applied **in series** (band 1 → band 2 → … → band 24).
- Each band is bypassed unless its **On** switch is enabled, so an unused band
  costs (almost) nothing.
- **Freq / Gain / Q** are smoothed, so automation and fast dial moves don't
  zipper or click.

## Per-band parameters

Every band exposes the same set of controls.

| Control | Range / choices | Default | Notes |
|---|---|---|---|
| **On** | off / on | off | Enables the band. |
| **Bypass** | off / on | off | Keeps the band on the graph but skips processing (unity). |
| **Listen** | off / on | off | Solo: auditions only this band's frequency range (a band-pass of the dry input). Exclusive — enabling one clears the others. |
| **Channel** | Stereo / Left / Right / Mid / Side | Stereo | Which channel(s) the band acts on. Mid/Side operate on the M/S matrix. |
| **Type** | Bell / Low Shelf / High Shelf / High Pass / Low Pass | Bell | Filter shape. **Slope** applies to High/Low Pass only. |
| **Freq** | 20 Hz – 20 kHz | per band | Centre / corner frequency (log-skewed dial, centre ≈ 632 Hz). |
| **Gain** | −24 … +24 dB | 0 dB | Bell/shelf gain. Ignored by High/Low Pass. |
| **Q** | 0.1 – 18 | 0.707 | Bandwidth / resonance (log-skewed, centre 1.0). |
| **Slope** | 12 – 96 dB/oct (8 steps) | 12 dB/oct | High/Low Pass steepness (Butterworth cascade). |
| **Dynamics** | off / on | off | Enables the per-band dynamic gain offset. |
| **Threshold** | −60 … 0 dB | −24 dB | Level above which the dynamic offset engages. |
| **Range** | −24 … +24 dB | 0 dB | Maximum dynamic gain change (see below). Positive expands, negative compresses. |
| **Attack** | 0.05 – 100 ms | 10 ms | Detector attack time. |
| **Release** | 5 – 2000 ms | 120 ms | Detector release time. |
| **Knee** | 0 – 24 dB | 6 dB | Soft-knee width around the threshold. |

### Global

| Control | Default | Notes |
|---|---|---|
| **Bypass** | off | Plugin-wide bypass (reported to the host so PDC stays correct). |

## How the dynamics behave (important)

The dynamic section adds a **gain offset** on top of the band's static Gain,
driven by how far the detected level sits above **Threshold**:

- The offset reaches the **full Range** when the detector is **24 dB above
  Threshold**, and scales linearly below that.
- Consequently **Range sets both the depth and the effective ratio together** —
  there is no separate ratio control. As a rough guide:

  | Range | ≈ effective ratio |
  |---|---|
  | −6 dB | ~1.3 : 1 |
  | −12 dB | ~2 : 1 |
  | −18 dB | ~4 : 1 |
  | −24 dB | limiting (∞ : 1) |

- **Detection is chain-coupled.** Because bands run in series, a band's detector
  sees the signal **as already shaped by the earlier bands**. Moving band 1 can
  therefore change how band 5's dynamics react. This is intentional but worth
  knowing when several dynamic bands overlap.
- The detector's side-signal is a band-pass centred on the band's own **Freq**;
  there is no independent sidechain frequency or external sidechain input.

## Presets

The factory presets (Vocal De-Harsh, Kick Punch, De-Mud, Air Lift) plus **Init**
are wired and parameter-validated, but their voicing is still a work in
progress — treat them as starting points, not finished sounds.

## Known characteristics

- Filters use classic RBJ ("Audio EQ Cookbook") designs. Like any bilinear-
  transform biquad, a bell's **bandwidth narrows as its centre approaches
  Nyquist** — at low sample rates a high-frequency bell is noticeably narrower
  than the same Q at a high sample rate. The centre frequency and peak gain stay
  accurate; the *width* is what shifts. Keep this in mind when moving a session
  between sample rates.
