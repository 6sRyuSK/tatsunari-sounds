#pragma once

#include "factory_params/ParamDesc.h"

#include <string>
#include <vector>

//
// resonance_suppressor_params — the DECLARATIVE parameter table for the resonance
// suppressor (Phase P1 of the params-model migration). This is now the single
// source of truth for the plugin's parameters: createParameterLayout() in
// PluginProcessor.cpp GENERATES the APVTS layout from this table via
// factory_params::buildApvtsLayout(), reproducing the former hand-written
// juce::AudioParameter* objects bit-for-bit and in the same host-visible order
// (guarded by preset_test's "paramdesc parity" check).
//
// JUCE-free: this header declares the ParamDesc table only; the JUCE objects are
// built from it in juce/ApvtsAdapter.h. Every entry below is transcribed
// faithfully — in the SAME ORDER — from the pre-migration createParameterLayout()
// (ranges incl. presence/absence of an interval, skew centres, defaults, labels
// with their leading spaces, display names, ParameterID version hints, and choice
// arrays), and the load-bearing rationale comments moved here beside their
// entries.
//
namespace resonance_suppressor_params
{
    // Band count + the per-band default tables (mirror PluginProcessor.h's
    // kNumBands and createParameterLayout()'s bandFreqs/bandSens). Bands 0-3
    // shipped pre-Phase-4 (version hint 1); bands 4-7 are Phase 4 (version hint 2,
    // off by default). Their freq defaults spread across low/low-mid/high-mid/air.
    inline constexpr int kNumBands   = 8;
    inline constexpr int kNumBandsV1 = 4;

    inline std::vector<factory_params::ParamDesc> buildRsParams()
    {
        using namespace factory_params;

        std::vector<ParamDesc> p;
        p.reserve (64);

        p.push_back (floatParam ("depth", "Depth", 0.0f, 100.0f, 0.1f, 30.0f, " %", 1));

        // v2.1: "sharpness" (and "selectivity" below) are LEGACY — the DSP now
        // reads only "detail" (see below). They stay registered with unchanged
        // IDs/ranges so VST3 automation lanes and old sessions keep resolving
        // (removing a parameter is a state/automation break = major bump); their
        // values are simply no longer consumed. Display names say so, and
        // kFlagLegacyJuceOnly marks them JUCE/APVTS-only (excluded from the future
        // CLAP surface).
        p.push_back (floatParam ("sharpness", "Sharpness (legacy)", 0.0f, 100.0f, 0.1f, 50.0f, " %", 1,
                                 /*skewCentre*/ 0.0f, kFlagLegacyJuceOnly));

        // Phase 6 DEFAULTS DRAFT (pending audition sign-off, CLAUDE.md "Ask a human"
        // #1): attack was 100 ms, tuned for the pre-Phase-1 detector (a coarser,
        // slower-reacting envelope that needed a sluggish attack to avoid chatter).
        // The Phase 1 rework detects per STFT frame (H/fs ~ 5.3 ms hop @ 48 kHz
        // Normal, 8x overlap) with a self-excluding-notch envelope + soft-knee
        // contrast, so it is precise enough per frame that a 100 ms attack just
        // lets a transient resonance (a harsh consonant, a pick attack) ring for
        // ~19 frames before the suppressor catches up -- audibly late for a
        // de-harsh tool. New default 20 ms (the skew centre of this range, so it
        // sits at the dial's natural middle) reacts within ~4 frames while still
        // averaging over enough frames to reject single-frame noise-floor jitter.
        // Note the range has NO interval (continuous).
        p.push_back (floatParam ("attack", "Attack", 1.0f, 200.0f, 0.0f, 20.0f, " ms", 1, /*skewCentre*/ 20.0f));

        // Release nudged 50 -> 65 ms alongside the faster attack: a snappier attack
        // with an unchanged release skewed the ballistics' overall shape toward
        // "grabs fast, lets go fast", which can pump on rhythmic material; a modest
        // release increase keeps recovery still well inside a "fast" setting
        // (release range tops out at 500 ms) while smoothing the gesture back out.
        p.push_back (floatParam ("release", "Release", 5.0f, 500.0f, 0.0f, 65.0f, " ms", 1, /*skewCentre*/ 100.0f));

        p.push_back (floatParam ("mix", "Mix", 0.0f, 100.0f, 0.1f, 100.0f, " %", 1));

        p.push_back (boolParam ("delta",  "Delta",       false, 1));
        p.push_back (boolParam ("link",   "Stereo Link", true,  1));
        p.push_back (boolParam ("bypass", "Bypass",      false, 1, kFlagBypass));

        // Detection mode. Soft (default): adaptive threshold, level-independent --
        // reacts to relative tonal change. Hard: absolute-level threshold (Depth
        // sets it), reacts to absolute harmonic level (Soothe2-style). Soft is the
        // current behaviour, so it is the default and existing presets are unchanged.
        p.push_back (choiceParam ("mode", "Mode", { "Soft", "Hard" }, 0, 1));

        // --- Phase 1 detector controls (Selectivity / Tilt / Quality) ---
        // Version hint 2: these arrived in v1.3.0, after the original v1.x set, so a
        // v1.2.0 session (which lacks them) still loads -- its state simply leaves
        // them at the defaults below (verified by preset_test's v1.2.0 fixture).
        // Tilt and Quality are applied every block in processBlock; the engine
        // epsilon-compares and rebuilds lazily, so no SmoothedValue is needed.
        // Phase 6 DEFAULTS DRAFT: selectivity/depth/sharpness reviewed and left
        // UNCHANGED (conservative) -- 50 % is already the soft-knee law's own
        // documented "nominal" point (ResonanceSuppressor::computeGains: T=3.5dB/
        // W=4dB), and depth/sharpness are audition-first-impression choices better
        // judged by ear against the Phase 6 pack than re-guessed here.
        p.push_back (floatParam ("selectivity", "Selectivity (legacy)", 0.0f, 100.0f, 0.1f, 50.0f, " %", 2,
                                 /*skewCentre*/ 0.0f, kFlagLegacyJuceOnly));

        // --- v2.1 macro + output controls (version hint 3: new in v2.1) ---
        // Detail replaces the sharpness/selectivity pair as the single detection
        // macro: sharpOct = 0.15 + 0.85*d/100, selectivity = d/100, and it also
        // drives the reduction-smoothing width, (1/12)*2^((50-d)/50) oct clamped
        // to [1/24, 1/6] (DetailParam.h). d = 50 reproduces the v2.0.1 defaults
        // bit-exactly. Pre-detail sessions are migrated in setStateInformation
        // (detail = mean of the legacy pair).
        p.push_back (floatParam ("detail", "Detail", 0.0f, 100.0f, 0.1f, 50.0f, " %", 3));

        // Output trim, applied AFTER the suppressor (post Mix, also in Delta) via
        // a ~20 ms SmoothedValue in processBlock; NOT applied while the internal
        // bypass is active (bypass stays a unity passthrough).
        p.push_back (floatParam ("out", "Output", -24.0f, 24.0f, 0.1f, 0.0f, " dB", 3));

        p.push_back (floatParam ("tilt", "Tilt", -100.0f, 100.0f, 1.0f, 0.0f, " %", 2));

        // Quality trades latency for low-frequency time resolution (Fast = half
        // latency, High = double). Excluded from presets (FactoryPresets kExclude):
        // a preset switch must not renegotiate host PDC or override the user's
        // choice. Default index 1 (Normal).
        p.push_back (choiceParam ("quality", "Quality", { "Fast", "Normal", "High" }, 1, 2));

        // --- Phase 3 routing controls (Link Amount / Channel mode / Sidechain) ---
        // Version hint 2: added in v1.5.0, after the v1.2.0 set, so an older session
        // still loads and simply leaves these at the defaults below (guarded by
        // preset_test's v1.2.0 fixture). linkAmt scales the continuous stereo-link
        // blend and is only effective while the Stereo Link toggle is on (engine:
        // lambda = link ? amt : 0); channelMode switches Stereo vs Mid/Side;
        // scEnable/scListen are additionally gated on a live sidechain connection in
        // processBlock, so an unpatched sidechain is safe. Applied every block like
        // the detector controls (no SmoothedValue: the engine epsilon-compares and
        // the routing switches ride their own crossfades).
        p.push_back (floatParam ("linkAmt", "Link Amount", 0.0f, 100.0f, 1.0f, 100.0f, " %", 2));

        p.push_back (choiceParam ("channelMode", "Channel Mode", { "Stereo", "Mid-Side" }, 0, 2));

        p.push_back (boolParam ("scEnable", "Sidechain", false, 2));
        p.push_back (boolParam ("scListen", "SC Listen", false, 2));

        // --- Reduction / depth-EQ nodes (soothe-style) ---
        // Two cuts bound where processing acts (rolling the profile off at a chosen
        // slope), eight typed bands locally raise/lower the sensitivity over a
        // per-band width (Phase 4). Defaults mirror the reference: low cut 450 Hz,
        // high cut 16 kHz, bands 1-4 flat except a +6 dB emphasis at 5 kHz (band 3),
        // so the factory sound is mid-focused, not full-band; bands 5-8 (Phase 4)
        // are off by default so the shipped sound is unchanged until the user
        // enables one. The freq ranges have NO interval (continuous), skew centre
        // 650 Hz. Cut slope default index 2 (24 dB/oct).
        const std::vector<std::string> slopeChoices { "6 dB/oct", "12 dB/oct", "24 dB/oct", "48 dB/oct" };
        const std::vector<std::string> typeChoices  { "Bell", "Low Shelf", "High Shelf", "Band Shelf", "Band Reject", "Tilt" };

        struct CutDef { const char* prefix; const char* name; float freq; };
        const CutDef cuts[2] = { { "lc_", "Low Cut",  450.0f },
                                 { "hc_", "High Cut", 16000.0f } };
        for (const auto& c : cuts)
        {
            const std::string pre = c.prefix;
            const std::string nm  = c.name;
            p.push_back (boolParam   (pre + "on",    nm + " On", true, 1));
            p.push_back (floatParam  (pre + "freq",  nm + " Freq", 20.0f, 20000.0f, 0.0f, c.freq, " Hz", 1, /*skewCentre*/ 650.0f));
            p.push_back (choiceParam (pre + "slope", nm + " Slope", slopeChoices, 2, 1)); // 24 dB/oct
        }

        // b0..b3 shipped pre-Phase-4 (version hint 1) -- defaults/hints unchanged.
        // b4..b7 are Phase 4 (version hint 2 -- brand new IDs, never shipped
        // before), off by default; their freq defaults spread across
        // low/low-mid/high-mid/air so enabling one lands somewhere useful before the
        // user retunes freq/type/sens.
        const float bandFreqs[kNumBands] = { 1000.0f, 2500.0f, 5000.0f, 8000.0f, 150.0f, 500.0f, 3000.0f, 12000.0f };
        const float bandSens [kNumBands] = { 0.0f,    0.0f,    6.0f,    0.0f,    0.0f,   0.0f,   0.0f,    0.0f };
        for (int b = 0; b < kNumBands; ++b)
        {
            const bool v1   = (b < kNumBandsV1);
            const int  hint = v1 ? 1 : 2;
            const std::string id = "b" + std::to_string (b) + "_";
            const std::string nm = "Band " + std::to_string (b + 1);
            p.push_back (boolParam   (id + "on",   nm + " On", v1, hint)); // bands 1-4 on, 5-8 off
            p.push_back (floatParam  (id + "freq", nm + " Freq", 20.0f, 20000.0f, 0.0f, bandFreqs[b], " Hz", hint, /*skewCentre*/ 650.0f));
            p.push_back (choiceParam (id + "type", nm + " Type", typeChoices, 0, hint)); // Bell
            p.push_back (floatParam  (id + "sens", nm + " Sens", -30.0f, 30.0f, 0.1f, bandSens[b], " dB", hint));
        }

        // Phase 4: per-band width, ALL 8 bands (version hint 2 -- brand new). Scales
        // each shape's half-width/edge/span in ReductionProfile.h; default 0.50 is
        // the pre-Phase-4 fixed width, so every band reproduces the old curve
        // bit-for-bit until this is moved (see ReductionProfile.h's kWidthRef). The
        // UI knob for this parameter is Phase 5a; only the parameter ships here.
        for (int b = 0; b < kNumBands; ++b)
        {
            const std::string id = "b" + std::to_string (b) + "_width";
            const std::string nm = "Band " + std::to_string (b + 1) + " Width";
            p.push_back (floatParam (id, nm, 0.10f, 2.00f, 0.01f, 0.50f, " oct", 2));
        }

        return p;
    }
}
