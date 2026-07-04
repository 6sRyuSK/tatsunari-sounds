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
    factory_presets
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

# ------------------------------------------------------------ preset wiring test
# JUCE-linked console app: builds the processor headless and checks the factory
# preset table against the live parameter layout (IDs exist, values in range,
# Init == defaults, presetIndex round-trips). Rate-independent, one case.
juce_add_console_app({snake}_preset_test PRODUCT_NAME "{snake}_preset_test")
target_sources({snake}_preset_test PRIVATE
  tests/preset_test.cpp
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp)
target_include_directories({snake}_preset_test PRIVATE Source)
target_compile_features({snake}_preset_test PRIVATE cxx_std_20)
target_compile_definitions({snake}_preset_test PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0)
target_link_libraries({snake}_preset_test
  PRIVATE
    factory_core
    factory_ui
    factory_presets
    juce::juce_audio_utils
    juce::juce_dsp
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

add_test(NAME {snake}_preset COMMAND {snake}_preset_test)
"""

PROCESSOR_H = """\
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include "factory_presets/ProgramAdapter.h"
#include "FactoryPresets.h"

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

    // Program API rides factory_presets::ProgramAdapter (Init + factory bank).
    int getNumPrograms() override {{ return programs.getNumPrograms(); }}
    int getCurrentProgram() override {{ return programs.getCurrentProgram(); }}
    void setCurrentProgram (int index) override {{ programs.setCurrentProgram (index); }}
    const juce::String getProgramName (int index) override {{ return programs.getProgramName (index); }}
    void changeProgramName (int, const juce::String&) override {{}} // factory presets are immutable

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    factory_presets::ProgramAdapter programs;

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

    programs.configure (apvts, {snake}_presets::bank,
                        {snake}_presets::kExclude, {snake}_presets::kNumExclude);
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
    {{
        // Append the selected program index (attribute only — existing sessions
        // without it read back as program 0, so state stays compatible).
        programs.writeStateAttribute (*xml);
        copyXmlToBinary (*xml, destData);
    }}
}}

void {camel}AudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {{
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            programs.readStateAttribute (*xml);
        }}
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
#include "factory_ui/PresetSelector.h"

class {camel}AudioProcessorEditor final : public juce::AudioProcessorEditor,
                                          private juce::AudioProcessorListener
{{
public:
    explicit {camel}AudioProcessorEditor ({camel}AudioProcessor&);
    ~{camel}AudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    void refreshPresetSelector();

    // AudioProcessorListener — follow host-driven program changes.
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override;
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {{}}

    {camel}AudioProcessor& processor;
    FactoryLookAndFeel lnf;

    juce::Slider outputSlider;
    juce::Label  outputLabel, titleLabel;
    juce::ToggleButton bypassButton {{ "Bypass" }};
    factory_ui::PresetSelector presetSelector;

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

    // Preset selector: populate from the processor's program list and wire the
    // two-way host sync. User selection drives the program API + notifies the
    // host; host-driven changes come back via audioProcessorChanged.
    refreshPresetSelector();
    presetSelector.onChange = [this] (int idx)
    {{
        processor.setCurrentProgram (idx);
        processor.updateHostDisplay (
            juce::AudioProcessorListener::ChangeDetails{{}}.withProgramChanged (true));
    }};
    addAndMakeVisible (presetSelector);
    processor.addListener (this);

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
    processor.removeListener (this);
    setLookAndFeel (nullptr);
}}

void {camel}AudioProcessorEditor::refreshPresetSelector()
{{
    juce::StringArray names;
    for (int i = 0; i < processor.getNumPrograms(); ++i)
        names.add (processor.getProgramName (i));
    presetSelector.setItems (names, processor.getCurrentProgram());
}}

void {camel}AudioProcessorEditor::audioProcessorChanged (juce::AudioProcessor*,
                                                         const ChangeDetails& details)
{{
    if (! details.programChanged)
        return;

    // May arrive on any thread; marshal the selector update to the message thread.
    juce::Component::SafePointer<{camel}AudioProcessorEditor> safe (this);
    juce::MessageManager::callAsync ([safe]
    {{
        if (safe != nullptr)
            safe->presetSelector.setSelectedIndex (safe->processor.getCurrentProgram(),
                                                   juce::dontSendNotification);
    }});
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
    titleLabel.setBounds (top.removeFromLeft (120));
    top.removeFromLeft (8);
    presetSelector.setBounds (top);

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

FACTORY_PRESETS = """\
#pragma once

#include "factory_presets/PresetBank.h"

//
// Factory presets for {slug}.
//
// SCAFFOLD: this bank starts EMPTY (Init only). Program 0 ("Init") is synthesised
// by ProgramAdapter (every parameter to its default) and is NOT listed here.
//
// To add a preset (see .claude/skills — the add-preset workflow):
//   1. Declare a constexpr factory_presets::PresetParam array of (paramID, value)
//      pairs. `value` is the parameter's REAL value in its own units (dB, %, Hz…),
//      not the normalised 0..1 — ProgramAdapter normalises on apply.
//   2. Add a factory_presets::Preset row to kPresets and grow `bank`.
//   3. tests/preset_test.cpp verifies IDs exist and values are in range.
// Preset VALUES/NAMES are taste — do not ship without a human audition sign-off.
//
namespace {snake}_presets
{{
    // Parameters presets must never touch. "bypass" is excluded fleet-wide so a
    // preset never silences or un-silences the plugin. Add monitoring toggles
    // (e.g. "delta", "listen") here as needed.
    inline constexpr const char* kExclude[] = {{ "bypass" }};
    inline constexpr int kNumExclude = (int) (sizeof (kExclude) / sizeof (kExclude[0]));

    // TODO(scaffold): declare preset parameter arrays and list them in kPresets.
    inline constexpr factory_presets::Preset* kPresets = nullptr;

    // Init-only bank until curated presets are added.
    inline constexpr factory_presets::PresetBank bank {{ kPresets, 0 }};
}}
"""

PRESET_TEST = """\
//
// preset_test.cpp — wiring verification for {slug}'s factory presets.
//
// Preset typos (a paramID no parameter owns) and out-of-range values fail
// silently at runtime, so this JUCE-linked console test builds the processor
// headless and checks the table against the live APVTS layout. The independent
// oracle is the parameter layout itself: a clamped value, or an ID with no
// matching parameter, is a bug. This is the "wiring test" category — a complement
// to tests/dsp_test.cpp (which links only factory_core), NOT a change to it.
//
#include "PluginProcessor.h"
#include "FactoryPresets.h"

#include <cstdio>
#include <string>

namespace
{{
    int g_failures = 0;
    void fail (const std::string& m) {{ std::printf ("  FAIL: %s\\n", m.c_str()); ++g_failures; }}

    bool isExcluded (const juce::String& id)
    {{
        for (int k = 0; k < {snake}_presets::kNumExclude; ++k)
            if (id == {snake}_presets::kExclude[k])
                return true;
        return false;
    }}

    double readReal (juce::RangedAudioParameter* rp)
    {{
        return (double) rp->convertFrom0to1 (rp->getValue());
    }}

    void check1_names ({camel}AudioProcessor& p)
    {{
        std::printf ("1. program names non-empty + unique\\n");
        juce::StringArray seen;
        for (int i = 0; i < p.getNumPrograms(); ++i)
        {{
            const auto name = p.getProgramName (i);
            if (name.trim().isEmpty())
                fail ("program " + std::to_string (i) + " has an empty name");
            if (seen.contains (name))
                fail ("duplicate program name '" + name.toStdString() + "'");
            seen.add (name);
        }}
    }}

    void check2_ids_exist ({camel}AudioProcessor& p)
    {{
        std::printf ("2. every preset paramID exists in the layout\\n");
        const auto& bank = {snake}_presets::bank;
        for (int pr = 0; pr < bank.numPresets; ++pr)
            for (int e = 0; e < bank.presets[pr].numParams; ++e)
            {{
                const char* id = bank.presets[pr].params[e].paramID;
                if (p.apvts.getParameter (id) == nullptr)
                    fail (std::string ("preset '") + bank.presets[pr].name + "' references unknown paramID '" + id + "'");
                if (isExcluded (juce::String (id)))
                    fail (std::string ("preset '") + bank.presets[pr].name + "' targets excluded paramID '" + id + "'");
            }}
    }}

    void check3_values_in_range ({camel}AudioProcessor& p)
    {{
        std::printf ("3. every preset value applies without clamping\\n");
        const auto& bank = {snake}_presets::bank;
        for (int pr = 0; pr < bank.numPresets; ++pr)
        {{
            p.setCurrentProgram (pr + 1); // program 0 is Init; bank is 1-based
            for (int e = 0; e < bank.presets[pr].numParams; ++e)
            {{
                const char* id = bank.presets[pr].params[e].paramID;
                const double intended = (double) bank.presets[pr].params[e].value;
                auto* rp = p.apvts.getParameter (id);
                if (rp == nullptr) continue; // reported by check 2
                const double got = readReal (rp);
                const double span = std::abs ((double) rp->convertFrom0to1 (1.0f)
                                              - (double) rp->convertFrom0to1 (0.0f));
                const double tol = 1.0e-3 + 1.0e-4 * span;
                if (std::abs (got - intended) > tol)
                    fail (std::string ("preset '") + bank.presets[pr].name + "' param '" + id
                          + "' applied as " + std::to_string (got) + " (intended "
                          + std::to_string (intended) + ") — out of range / clamped");
            }}
        }}
    }}

    void check4_init_is_default ({camel}AudioProcessor& p)
    {{
        std::printf ("4. program 0 (Init) == all defaults\\n");
        p.setCurrentProgram (p.getNumPrograms() - 1); // exercise a reset
        p.setCurrentProgram (0);
        for (auto* base : p.getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (base))
            {{
                if (isExcluded (rp->getParameterID())) continue;
                if (std::abs (rp->getValue() - rp->getDefaultValue()) > 1.0e-6f)
                    fail ("Init leaves '" + rp->getParameterID().toStdString() + "' off its default");
            }}
    }}

    void check5_index_roundtrip ({camel}AudioProcessor& p)
    {{
        std::printf ("5. setCurrentProgram/getCurrentProgram + presetIndex persistence\\n");
        for (int i = 0; i < p.getNumPrograms(); ++i)
        {{
            p.setCurrentProgram (i);
            if (p.getCurrentProgram() != i)
                fail ("getCurrentProgram() != " + std::to_string (i));
        }}

        const int idx = p.getNumPrograms() - 1;
        p.setCurrentProgram (idx);
        juce::MemoryBlock state;
        p.getStateInformation (state);

        {camel}AudioProcessor restored;
        restored.setStateInformation (state.getData(), (int) state.getSize());
        if (restored.getCurrentProgram() != idx)
            fail ("presetIndex did not survive state round-trip");

        // A legacy state without the presetIndex attribute must default to 0.
        {camel}AudioProcessor legacy;
        juce::MemoryBlock legacyState;
        if (auto xml = legacy.apvts.copyState().createXml())
            legacy.copyXmlToBinary (*xml, legacyState);
        {camel}AudioProcessor legacyRestored;
        legacyRestored.setStateInformation (legacyState.getData(), (int) legacyState.getSize());
        if (legacyRestored.getCurrentProgram() != 0)
            fail ("legacy state without presetIndex did not default to program 0");
    }}
}}

int main()
{{
    juce::ScopedJuceInitialiser_GUI juceInit; // MessageManager for async param updates

    {camel}AudioProcessor processor;
    std::printf ("{slug} preset wiring (%d programs)\\n", processor.getNumPrograms());

    if (processor.getNumPrograms() != 1 + {snake}_presets::bank.numPresets)
        fail ("getNumPrograms() != 1 (Init) + bank size");

    check1_names (processor);
    check2_ids_exist (processor);
    check3_values_in_range (processor);
    check4_init_is_default (processor);
    check5_index_roundtrip (processor);

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
        dest / "Source" / "FactoryPresets.h": FACTORY_PRESETS.format(**ctx),
        dest / "tests" / "dsp_test.cpp": DSP_TEST.format(**ctx),
        dest / "tests" / "preset_test.cpp": PRESET_TEST.format(**ctx),
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
