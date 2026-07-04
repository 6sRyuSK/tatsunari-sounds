#!/usr/bin/env python3
"""
tools/scaffold_plugin.py — generate a new plugins/<slug>/ skeleton that follows
every factory convention, so starting a plugin never requires re-reading an
existing plugin's sources.

What it generates (all compiling, conventions pre-wired):
  plugins/<slug>/plugin.toml               version 0.1.0, status in-progress
  plugins/<slug>/CMakeLists.txt            factory_read_version, FORMATS w/ AU
                                           on APPLE only, full sample-rate test loop
  plugins/<slug>/Source/PluginProcessor.*  thin AudioProcessor wrapper: APVTS,
                                           atomic param pointers, SmoothedValue
                                           output gain, RT-safe processBlock
  plugins/<slug>/Source/PluginEditor.*     factory_ui chrome (paintBackground,
                                           styleKnob, setSliderDecimals-after-
                                           attachment)
  plugins/<slug>/tests/dsp_test.cpp        stub using factory_core::testing
                                           helpers; FAILS until real spec-based
                                           checks are written (intentional gate)

It also verifies the PLUGIN_CODE is unique across the fleet, warns if the
plugin should be removed from roadmap.toml, and regenerates the README catalog.

The root CMakeLists auto-includes plugins/*/CMakeLists.txt — no registration
step is needed.

Usage:
  python tools/scaffold_plugin.py <slug> --name "Product Name" \
      --category Dynamics --reference "SSL G bus comp" \
      [--code Xxxx] [--vst3-category "Fx Dynamics"]

Requires Python 3.11+ (stdlib only).
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def camel(slug: str) -> str:
    return "".join(part.capitalize() for part in slug.split("-"))


def snake(slug: str) -> str:
    return slug.replace("-", "_")


def default_code(slug: str) -> str:
    """A 4-char PLUGIN_CODE guess: first letter upper + next 3 consonant-ish
    chars. Always verify uniqueness (done in main)."""
    letters = re.sub(r"[^a-z0-9]", "", slug)
    return (letters[:4].capitalize() + "1111")[:4]


def existing_codes() -> dict[str, str]:
    codes: dict[str, str] = {}
    for cm in ROOT.glob("plugins/*/CMakeLists.txt"):
        m = re.search(r"PLUGIN_CODE\s+(\S+)", cm.read_text(encoding="utf-8"))
        if m:
            codes[m.group(1)] = cm.parent.name
    return codes


# --------------------------------------------------------------------------- templates

PLUGIN_TOML = """\
[plugin]
name      = "{name}"
slug      = "{slug}"
category  = "{category}"
status    = "in-progress"
version   = "0.1.0"
formats   = ["VST3", "AU"]
reference = "{reference}"
"""

CMAKELISTS = """\
# {slug} — {reference}. Composed from factory_core.

factory_read_version(${{CMAKE_CURRENT_SOURCE_DIR}}/plugin.toml {var}_VERSION)
message(STATUS "{slug} version ${{{var}_VERSION}} (from plugin.toml)")

# VST3 everywhere, AU on Apple, plus a Standalone app for local GUI review.
set({var}_FORMATS VST3 Standalone)
if(APPLE)
  list(APPEND {var}_FORMATS AU)
endif()

juce_add_plugin({target}
  VERSION                     ${{{var}_VERSION}}
  PRODUCT_NAME                "{name}"
  COMPANY_NAME                "Tatsunari Sounds"
  PLUGIN_MANUFACTURER_CODE    Ttsn
  PLUGIN_CODE                 {code}
  FORMATS                     ${{{var}_FORMATS}}
  IS_SYNTH                    FALSE
  NEEDS_MIDI_INPUT            FALSE
  NEEDS_MIDI_OUTPUT           FALSE
  IS_MIDI_EFFECT              FALSE
  COPY_PLUGIN_STEP            FALSE
  VST3_CATEGORIES             {vst3})

target_sources({target} PRIVATE
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp)

target_compile_features({target} PRIVATE cxx_std_20)

target_compile_definitions({target} PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries({target}
  PRIVATE
    factory_core
    factory_ui
    juce::juce_audio_utils
    juce::juce_dsp
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------- DSP tests
add_executable({snake}_dsp_test tests/dsp_test.cpp)
target_link_libraries({snake}_dsp_test PRIVATE factory_core)
target_compile_features({snake}_dsp_test PRIVATE cxx_std_20)

foreach(_fs 44100 48000 88200 96000 176400 192000)
  add_test(NAME {snake}_dsp_${{_fs}}
           COMMAND {snake}_dsp_test ${{_fs}})
endforeach()
"""

PROCESSOR_H = """\
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include <atomic>

//
// {name}. The AudioProcessor is a thin wrapper around a factory_core engine
// (see core/include/factory_core/) — the DSP itself must live there, header-only
// and JUCE-independent, so tests/dsp_test.cpp can exercise it headless.
// Everything is preallocated in prepareToPlay; processBlock does not allocate,
// lock, or make syscalls.
//
class {camel}AudioProcessor final : public juce::AudioProcessor
{{
public:
    {camel}AudioProcessor();
    ~{camel}AudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {{}}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override {{ return true; }}

    const juce::String getName() const override {{ return "{name}"; }}
    bool acceptsMidi() const override {{ return false; }}
    bool producesMidi() const override {{ return false; }}
    bool isMidiEffect() const override {{ return false; }}
    double getTailLengthSeconds() const override {{ return 0.0; }}

    int getNumPrograms() override {{ return 1; }}
    int getCurrentProgram() override {{ return 0; }}
    void setCurrentProgram (int) override {{}}
    const juce::String getProgramName (int) override {{ return {{}}; }}
    void changeProgramName (int, const juce::String&) override {{}}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // TODO(scaffold): add the plugin's real parameters here (atomic pointers via
    // apvts.getRawParameterValue) and the factory_core engine instance.
    std::atomic<float>* outputParam = nullptr; // dB
    std::atomic<float>* bypassParam = nullptr; // bool

    // Continuous parameters must be smoothed (regression policy): reset in
    // prepareToPlay, setTargetValue in processBlock, ramp per sample.
    juce::SmoothedValue<float> outputGain;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR ({camel}AudioProcessor)
}};
"""

PROCESSOR_CPP = """\
#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorValueTreeState::ParameterLayout
{camel}AudioProcessor::createParameterLayout()
{{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // TODO(scaffold): add the plugin's real parameters.
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID {{ "output", 1 }}, "Output",
        juce::NormalisableRange<float> {{ -24.0f, 24.0f, 0.01f }}, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID {{ "bypass", 1 }}, "Bypass", false));

    return layout;
}}

{camel}AudioProcessor::{camel}AudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{{
    outputParam = apvts.getRawParameterValue ("output");
    bypassParam = apvts.getRawParameterValue ("bypass");
}}

void {camel}AudioProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{{
    // TODO(scaffold): prepare() the factory_core engine here (preallocate all
    // buffers; processBlock must never allocate). Reset all state (regression
    // policy: state reset on prepare and bypass transitions).
    outputGain.reset (sampleRate, 0.02);
    outputGain.setCurrentAndTargetValue (
        juce::Decibels::decibelsToGain (outputParam->load()));
}}

bool {camel}AudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return layouts.getMainInputChannelSet() == out;
}}

void {camel}AudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{{
    juce::ScopedNoDenormals noDenormals;

    const int totalIn  = getTotalNumInputChannels();
    const int totalOut = getTotalNumOutputChannels();
    for (int ch = totalIn; ch < totalOut; ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    const bool bypassed = bypassParam->load() > 0.5f;

    // TODO(scaffold): run the factory_core engine here (per sample or per
    // block). On bypass, keep latency-aligned and reset/crossfade state.

    outputGain.setTargetValue (bypassed ? 1.0f
        : juce::Decibels::decibelsToGain (outputParam->load()));

    const int numCh = juce::jmin (totalIn, totalOut);
    const int n     = buffer.getNumSamples();
    for (int i = 0; i < n; ++i)
    {{
        const float g = outputGain.getNextValue();
        for (int ch = 0; ch < numCh; ++ch)
            buffer.getWritePointer (ch)[i] *= g;
    }}
}}

juce::AudioProcessorEditor* {camel}AudioProcessor::createEditor()
{{
    return new {camel}AudioProcessorEditor (*this);
}}

void {camel}AudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}}

void {camel}AudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{{
    return new {camel}AudioProcessor();
}}
"""

EDITOR_H = """\
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "factory_ui/FactoryLookAndFeel.h"

class {camel}AudioProcessorEditor final : public juce::AudioProcessorEditor
{{
public:
    explicit {camel}AudioProcessorEditor ({camel}AudioProcessor&);
    ~{camel}AudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    {camel}AudioProcessor& processor;
    FactoryLookAndFeel lnf;

    juce::Slider outputSlider;
    juce::Label  outputLabel, titleLabel;
    juce::ToggleButton bypassButton {{ "Bypass" }};

    std::unique_ptr<SliderAttachment> outputAtt;
    std::unique_ptr<ButtonAttachment> bypassAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR ({camel}AudioProcessorEditor)
}};
"""

EDITOR_CPP = """\
#include "PluginEditor.h"
#include "factory_ui/FactoryChrome.h"

{camel}AudioProcessorEditor::{camel}AudioProcessorEditor ({camel}AudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{{
    setLookAndFeel (&lnf);

    titleLabel.setText ("{title}", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, FactoryLookAndFeel::accent());
    addAndMakeVisible (titleLabel);

    factory_ui::styleKnob (outputSlider, outputLabel, "Output", " dB");
    addAndMakeVisible (outputSlider);
    addAndMakeVisible (outputLabel);

    bypassButton.setColour (juce::ToggleButton::textColourId, FactoryLookAndFeel::textDim());
    addAndMakeVisible (bypassButton);

    auto& s = processor.apvts;
    outputAtt = std::make_unique<SliderAttachment> (s, "output", outputSlider);
    bypassAtt = std::make_unique<ButtonAttachment> (s, "bypass", bypassButton);

    // Pin the text-box precision. Must run AFTER the attachments above, which
    // otherwise format continuous ranges with up to 7 decimals (see #23).
    factory_ui::setSliderDecimals (outputSlider, 2);

    setSize (460, 300);
}}

{camel}AudioProcessorEditor::~{camel}AudioProcessorEditor()
{{
    setLookAndFeel (nullptr);
}}

void {camel}AudioProcessorEditor::paint (juce::Graphics& g)
{{
    factory_ui::paintBackground (g, getLocalBounds());
}}

void {camel}AudioProcessorEditor::resized()
{{
    auto r = getLocalBounds().reduced (16);

    auto top = r.removeFromTop (26);
    bypassButton.setBounds (top.removeFromRight (96));
    titleLabel.setBounds (top);

    r.removeFromTop (14);
    auto knob = r.removeFromTop (110).removeFromLeft (r.getWidth() / 3);
    outputLabel.setBounds (knob.removeFromTop (18));
    outputSlider.setBounds (knob.reduced (6, 0));
}}
"""

DSP_TEST = """\
//
// dsp_test.cpp — headless verification of the {slug} DSP core.
//
// SCAFFOLD STUB: this test FAILS on purpose until real spec-based checks are
// written. See .claude/skills/write-dsp-test and docs/regression-policy.md —
// every check needs an independent oracle (never derived from the code under
// test) and must run across the full sample-rate matrix.
//
#include "factory_core/testing/DspInvariants.h"

#include <cstdio>
#include <string>

namespace
{{
    namespace fct = factory_core::testing;

    int g_failures = 0;
    void fail (const std::string& m) {{ std::printf ("  FAIL: %s\\n", m.c_str()); ++g_failures; }}

    void coreTests (double Fs)
    {{
        std::printf ("{slug} core @ Fs=%.0f\\n", Fs);

        // TODO(scaffold): replace with real checks. Typical gates:
        //   - independent static oracle for the quantitative behaviour
        //     (z-domain for filters; analytic counts/levels for non-linear)
        //   - fct::impulseResponseNonIncreasing (...) at the WORST-CASE setting
        //     for any feedback path
        //   - fct::allFinite / fct::peakAbs over a long hold with a realistic
        //     peak bound (never a 1e6 "not-NaN" tolerance)
        //   - fct::resolutionFollowsSampleRate (Fs) if the core uses FFT/STFT
        fail ("dsp_test is a scaffold stub — write spec-based checks for {slug}");
    }}
}}

int main (int argc, char** argv)
{{
    // Full standard rate matrix by default; CTest passes one rate as argv[1].
    for (double Fs : fct::sampleRatesFromArgs (argc, argv))
        coreTests (Fs);

    if (g_failures == 0) {{ std::printf ("OK: all checks passed.\\n"); return 0; }}
    std::printf ("FAILED: %d check(s).\\n", g_failures);
    return 1;
}}
"""

# --------------------------------------------------------------------------- main


def main() -> int:
    ap = argparse.ArgumentParser(description="Scaffold a new plugin under plugins/<slug>/")
    ap.add_argument("slug", help="kebab-case directory name, e.g. tape-echo")
    ap.add_argument("--name", help='product name, e.g. "Tatsunari Tape Echo" (default: from slug)')
    ap.add_argument("--category", default="FX", help="catalog category, e.g. Dynamics / EQ / Reverb")
    ap.add_argument("--reference", default="—", help="reference gear/plugin the design chases")
    ap.add_argument("--code", help="unique 4-char JUCE PLUGIN_CODE (default: derived from slug)")
    ap.add_argument("--vst3-category", default="Fx",
                    help='VST3_CATEGORIES value, e.g. "Fx Dynamics" / "Fx EQ" / "Fx Reverb"')
    args = ap.parse_args()

    slug = args.slug
    if not re.fullmatch(r"[a-z0-9]+(-[a-z0-9]+)*", slug):
        print(f"error: slug '{slug}' must be kebab-case ([a-z0-9-])", file=sys.stderr)
        return 1

    dest = ROOT / "plugins" / slug
    if dest.exists():
        print(f"error: {dest} already exists", file=sys.stderr)
        return 1

    name = args.name or " ".join(p.capitalize() for p in slug.split("-"))
    code = args.code or default_code(slug)
    if not re.fullmatch(r"[A-Z][A-Za-z0-9]{3}", code):
        print(f"error: PLUGIN_CODE '{code}' must be 4 chars, first uppercase", file=sys.stderr)
        return 1
    codes = existing_codes()
    if code in codes:
        print(f"error: PLUGIN_CODE '{code}' already used by plugins/{codes[code]} — pass --code",
              file=sys.stderr)
        return 1

    ctx = dict(
        slug=slug, snake=snake(slug), camel=camel(slug), target=camel(slug),
        var=snake(slug).upper(), name=name, title=name.upper(), code=code,
        category=args.category, reference=args.reference, vst3=args.vst3_category,
    )

    files = {
        dest / "plugin.toml": PLUGIN_TOML.format(**ctx),
        dest / "CMakeLists.txt": CMAKELISTS.format(**ctx),
        dest / "Source" / "PluginProcessor.h": PROCESSOR_H.format(**ctx),
        dest / "Source" / "PluginProcessor.cpp": PROCESSOR_CPP.format(**ctx),
        dest / "Source" / "PluginEditor.h": EDITOR_H.format(**ctx),
        dest / "Source" / "PluginEditor.cpp": EDITOR_CPP.format(**ctx),
        dest / "tests" / "dsp_test.cpp": DSP_TEST.format(**ctx),
    }
    for path, content in files.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        print(f"  wrote {path.relative_to(ROOT)}")

    # Keep the roadmap and the catalog consistent with the new manifest.
    roadmap = ROOT / "roadmap.toml"
    if roadmap.exists() and name.lower() in roadmap.read_text(encoding="utf-8").lower():
        print(f"  NOTE: roadmap.toml seems to mention '{name}' — remove its [[plugin]] block "
              "(a started plugin lives only in plugins/<slug>/plugin.toml).")
    gen = subprocess.run([sys.executable, str(ROOT / "tools" / "gen_catalog.py")],
                         capture_output=True, text=True)
    print(gen.stdout.strip() or "  README catalog regenerated.")
    if gen.returncode != 0:
        print(f"  WARNING: gen_catalog.py failed:\n{gen.stderr}", file=sys.stderr)

    print(f"""
Scaffolded plugins/{slug}/ (PLUGIN_CODE {code}). Next steps:
  1. Put the DSP engine in core/include/factory_core/ (header-only,
     JUCE-independent) — compose existing primitives where possible.
  2. Replace the TODO(scaffold) markers in Source/ with real params + engine.
  3. Write real checks in tests/dsp_test.cpp (the stub FAILS by design).
  4. Build & test:  cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
                    cmake --build build && ctest --test-dir build -R {snake(slug)}_dsp
""")
    return 0


if __name__ == "__main__":
    sys.exit(main())
