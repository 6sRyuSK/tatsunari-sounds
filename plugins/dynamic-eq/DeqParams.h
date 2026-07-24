#pragma once
//
// plugins/dynamic-eq/DeqParams.h — the declarative parameter table of Dynamic
// Tatsunari EQ (single source of truth for the CLAP surface, the shell state
// codec, the editor, and the JUCE oracle's APVTS layout). JUCE-free; built on
// factory_params::ParamDesc.
//
// Every entry is transcribed faithfully — in the SAME ORDER — from the
// pre-migration DynamicEqAudioProcessor::createParameterLayout()
// (PluginProcessor.cpp): the 24 per-band blocks (on / byp / lsn / chan / type /
// freq / gain / q / slope / dyn / thr / rng / atk / rel / knee) followed by the
// global "bypass" toggle. Ranges (incl. presence/absence of an interval), skew
// centres, defaults, display names, labels (with their leading spaces), and
// choice arrays match verbatim. The processor now GENERATES its APVTS layout from
// this table via factory_params::buildApvtsLayout(); the parity is a test gate
// (dynamic-eq preset_test's "paramdesc parity" check, mirroring RS's).
//
#include "factory_params/ParamDesc.h"

#include <cmath>
#include <string>
#include <vector>

namespace dynamic_eq_params
{
    inline constexpr int kNumBands = 24;

    // Default (unused) band frequencies: log-spread 20 Hz .. 20 kHz, all bells.
    // Transcribed bit-for-bit from PluginProcessor.cpp's defaultFreq(): the same
    // float division + float std::pow, so the generated defaults match the shipped
    // layout exactly.
    inline float defaultFreq (int band)
    {
        const float t = (kNumBands > 1) ? (float) band / (float) (kNumBands - 1) : 0.0f;
        return 20.0f * std::pow (1000.0f, t); // 20 Hz .. 20 kHz
    }

    inline std::vector<factory_params::ParamDesc> buildDeqParams()
    {
        using namespace factory_params;

        std::vector<ParamDesc> p;
        p.reserve ((std::size_t) (kNumBands * 15 + 1));

        const std::vector<std::string> chanChoices  { "Stereo", "Left", "Right", "Mid", "Side" };
        const std::vector<std::string> typeChoices  { "Bell", "Low Shelf", "High Shelf", "High Pass", "Low Pass" };
        const std::vector<std::string> slopeChoices { "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct",
                                                      "60 dB/oct", "72 dB/oct", "84 dB/oct", "96 dB/oct" };

        for (int b = 0; b < kNumBands; ++b)
        {
            const std::string id = "b" + std::to_string (b) + "_";
            const std::string nm = "Band " + std::to_string (b + 1) + " ";

            p.push_back (boolParam   (id + "on",   nm + "On",      false, 1));
            // Present-but-bypassed: the band stays on the graph but is not processed.
            p.push_back (boolParam   (id + "byp",  nm + "Bypass",  false, 1));
            // Solo / listen: audition only this band's range (exclusive, enforced in UI).
            p.push_back (boolParam   (id + "lsn",  nm + "Listen",  false, 1));
            p.push_back (choiceParam (id + "chan", nm + "Channel", chanChoices, 0, 1));
            p.push_back (choiceParam (id + "type", nm + "Type",    typeChoices, 0, 1));
            // freq: 20..20000, continuous, skew centre 632.455.
            p.push_back (floatParam  (id + "freq", nm + "Freq", 20.0f, 20000.0f, 0.0f, defaultFreq (b), " Hz", 1, 632.455f));
            p.push_back (floatParam  (id + "gain", nm + "Gain", -24.0f, 24.0f, 0.01f, 0.0f, " dB", 1));
            // q: 0.1..18, continuous, skew centre 1.0, no label.
            p.push_back (floatParam  (id + "q",    nm + "Q",    0.1f, 18.0f, 0.0f, 0.707f, "", 1, 1.0f));
            // slope: 12..96 dB/oct (Butterworth cascade). Index k => (k+1) sections.
            p.push_back (choiceParam (id + "slope", nm + "Slope", slopeChoices, 0, 1));
            p.push_back (boolParam   (id + "dyn",  nm + "Dynamics",  false, 1));
            p.push_back (floatParam  (id + "thr",  nm + "Threshold", -60.0f, 0.0f, 0.01f, -24.0f, " dB", 1));
            p.push_back (floatParam  (id + "rng",  nm + "Range",     -24.0f, 24.0f, 0.01f, 0.0f, " dB", 1));
            // atk: 0.05..100, continuous, skew centre 10.
            p.push_back (floatParam  (id + "atk",  nm + "Attack",  0.05f, 100.0f, 0.0f, 10.0f, " ms", 1, 10.0f));
            // rel: 5..2000, continuous, skew centre 120.
            p.push_back (floatParam  (id + "rel",  nm + "Release", 5.0f, 2000.0f, 0.0f, 120.0f, " ms", 1, 120.0f));
            p.push_back (floatParam  (id + "knee", nm + "Knee",    0.0f, 24.0f, 0.01f, 6.0f, " dB", 1));
        }

        p.push_back (boolParam ("bypass", "Bypass", false, 1, kFlagBypass));

        return p;
    }
} // namespace dynamic_eq_params
