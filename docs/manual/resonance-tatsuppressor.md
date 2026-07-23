# Resonance TatSuppressor — User Manual

A spectral resonance / harshness suppressor (soothe-style). It analyses the
signal in the frequency domain, builds a self-referencing "how much does each
frequency stick out" profile, and applies dynamic spectral attenuation. This
page documents every user-facing parameter; the values come straight from the
plugin's parameter table, so it stays accurate to the shipped binary.

> Formats: CLAP + VST3 (macOS / Windows), AU (macOS). The editor is a
> JUCE-free Visage GUI. See the repository README for installation.

## Latency (read this first)

The suppressor is a linear-phase STFT process, so it introduces a fixed
**lookahead latency** that the **Quality** control changes:

| Quality | FFT size | Latency (samples) | Latency @ 48 kHz | Overlap |
|---|---|---|---|---|
| Fast | 1024 | 1024 | ~21.3 ms | 4× |
| **Normal** (default) | 2048 | 2048 | ~42.7 ms | 8× |
| High | 4096 | 4096 | ~85.3 ms | 8× |

The host compensates for this on playback (PDC). Two consequences:

- **Not ideal for live monitoring / tracking** at Normal or High — use Fast, or
  monitor a dry path.
- **Avoid automating Quality.** Changing it re-negotiates the host's latency,
  which can glitch the transport in some DAWs. Set it once per instance. (Quality
  is deliberately excluded from presets for the same reason.)

## Main controls

| Control | Range / choices | Default | Notes |
|---|---|---|---|
| **Depth** | 0 – 100 % | 30 % | Overall amount of suppression. |
| **Detail** | 0 – 100 % | 50 % | Detection macro: how tightly the process targets narrow resonances vs. broad tonal balance. Also sets the reduction-smoothing width. |
| **Mode** | Soft / Hard | Soft | Soft = adaptive, level-independent (reacts to *relative* tonal spikes). Hard = absolute-level threshold set by Depth (soothe2-style). |
| **Attack** | 1 – 200 ms | 20 ms | How fast the suppressor grabs a rising resonance. |
| **Release** | 5 – 500 ms | 65 ms | How fast it lets go. |
| **Tilt** | −100 … +100 % | 0 % | Frequency-dependent ballistics: bias reaction speed toward highs (faster) or lows (slower), pivoting around 1 kHz. |
| **Mix** | 0 – 100 % | 100 % | Dry/wet, applied in the spectral domain (avoids phase interference). |
| **Output** | −24 … +24 dB | 0 dB | Output trim, applied after Mix (also affects the Delta signal; not applied while bypassed). |
| **Quality** | Fast / Normal / High | Normal | Latency vs. low-frequency resolution — see the latency table above. |
| **Delta** | off / on | off | Monitor **only what is being removed**. Great for hearing exactly what the plugin acts on. (Best left un-automated.) |
| **Bypass** | off / on | off | Latency-preserving bypass (host PDC stays correct). |

## Stereo / routing

| Control | Range / choices | Default | Notes |
|---|---|---|---|
| **Stereo Link** | off / on | on | Link the two channels' suppression. |
| **Link Amount** | 0 – 100 % | 100 % | How strongly the channels are linked (only active while Stereo Link is on). |
| **Channel Mode** | Stereo / Mid-Side | Stereo | Process L/R or M/S. |
| **Sidechain** | off / on | off | Detect from an external sidechain input instead of the main signal. Safe when no sidechain is patched (falls back to internal). |
| **SC Listen** | off / on | off | Audition the sidechain detection signal. |

## Spectral shaping nodes

Two **cuts** bound where the process acts, and eight **bands** locally raise or
lower sensitivity. This lets you focus the suppressor (e.g. de-ess only the
5–10 kHz region) instead of processing full-band.

**Low Cut / High Cut** (each):

| Control | Range / choices | Default |
|---|---|---|
| On | off / on | Low Cut on, High Cut on |
| Freq | 20 Hz – 20 kHz | Low Cut 450 Hz, High Cut 16 kHz |
| Slope | 6 / 12 / 24 / 48 dB/oct | 24 dB/oct |

**Bands 1–8** (each):

| Control | Range / choices | Default |
|---|---|---|
| On | off / on | Bands 1–4 on, 5–8 off |
| Freq | 20 Hz – 20 kHz | 1 k / 2.5 k / 5 k / 8 k / 150 / 500 / 3 k / 12 k Hz |
| Type | Bell / Low Shelf / High Shelf / Band Shelf / Band Reject / Tilt | Bell |
| Sens | −30 … +30 dB | 0 dB (band 3 = +6 dB @ 5 kHz) |
| Width | 0.10 – 2.00 oct | 0.50 oct |

The factory default is mid-focused (a +6 dB emphasis at 5 kHz); bands 5–8 are
off so the shipped sound is unchanged until you enable one.

> **Legacy controls:** older sessions may still carry `Sharpness` and
> `Selectivity`. These are superseded by **Detail** and no longer affect the
> sound — they remain only so old sessions and automation lanes keep resolving.

## Presets

The factory presets (Vocal De-Harsh, Full Mix Tame, Gentle Smooth, Aggressive
Cut, De-Harsh M/S, Sibilance Tamer) plus **Init** are wired and validated, but
their voicing is still a work in progress — treat them as starting points.

## Tips

- **Delta is your friend.** Turn it on, sweep Depth, and listen to *what* is
  being removed before committing.
- For a transparent "polish", keep Depth low (20–40 %) and Detail near 50 %.
- To chase a specific resonance, enable one band, set its Type to Bell, raise
  its Sens, and narrow its Width.
