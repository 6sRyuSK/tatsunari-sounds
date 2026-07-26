#!/usr/bin/env python3
"""
tools/scaffold_plugin.py — generate a new plugins/<slug>/ skeleton that follows
every factory convention, so starting a plugin never requires re-reading an
existing plugin's sources.

脱JUCE: the scaffold emits a CLAP-FIRST plugin. There is no juce_add_plugin, no
AudioProcessor and no AudioProcessorEditor anywhere in the output — the shipping
binaries (CLAP + wrapper VST3, AUv2 on Apple) come from the plugin's own
shell/CMakeLists.txt via factory_clap_plugin, exactly like the three active
plugins. A new plugin therefore never needs a 脱JUCE cutover later.

What it generates (all compiling, conventions pre-wired):
  plugins/<slug>/plugin.toml                version 0.1.0, status in-progress
  plugins/<slug>/CMakeLists.txt             factory_read_version + the headless
                                            tests ONLY (no framework target)
  plugins/<slug>/<Camel>Core.h              the framework-free DSP core: prepare/
                                            process/reset/latencySamples, no
                                            allocation off the prepare path
  plugins/<slug>/<Camel>Params.h            the declarative ParamDesc table (the
                                            single source of truth for the CLAP
                                            surface, the state codec, the editor)
  plugins/<slug>/<Camel>Presets.h           factory-preset bank (Init only)
  plugins/<slug>/ui/<Camel>Models.h         visage-free UI seams (feed + presets)
  plugins/<slug>/ui/<Camel>Editor.{h,cpp}   the Visage editor over factory_ui_visage
  plugins/<slug>/shell/CMakeLists.txt       factory_clap_plugin assembly
  plugins/<slug>/shell/ClapEntry.cpp        the CLAP Policy + entry
  plugins/<slug>/shell/<Camel>ClapEditor.*  the Visage editor host seam
  plugins/<slug>/tests/dsp_test.cpp         stub using factory_core::testing
                                            helpers; FAILS until real spec-based
                                            checks are written (intentional gate)
  plugins/<slug>/tests/preset_test.cpp      headless param/preset wiring test
                                            (links factory_params + factory_presets)

It also verifies the AUv2 subtype code is unique across the fleet, warns if the
plugin should be removed from roadmap.toml, and regenerates the README catalog.

The root CMakeLists auto-includes plugins/*/CMakeLists.txt, and any plugin
carrying a shell/CMakeLists.txt automatically joins the clap-first assembly — no
registration step is needed.

Usage:
  python tools/scaffold_plugin.py <slug> --name "Product Name" \
      --category Dynamics --reference "SSL G bus comp" \
      [--code Xxxx] [--description "one-line host description"]

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
    """A 4-char AUv2 subtype guess: first letter upper + next 3 chars, padded.
    Always verify uniqueness (done in main)."""
    letters = re.sub(r"[^a-z0-9]", "", slug)
    return (letters[:4].capitalize() + "1111")[:4]


def existing_codes() -> dict[str, str]:
    """4-char codes already claimed across the fleet, code -> plugin dir name.

    Active plugins declare theirs as AUV2_SUBTYPE_CODE in shell/CMakeLists.txt;
    archived (JUCE) plugins still carry PLUGIN_CODE. Both are the same 4-char
    identifier space as far as a host is concerned, so both are checked — a
    revived archive plugin must not collide with a new one."""
    codes: dict[str, str] = {}
    for cm in ROOT.glob("plugins/*/shell/CMakeLists.txt"):
        m = re.search(r"AUV2_SUBTYPE_CODE\s+(\S+?)\)?\s*$", cm.read_text(encoding="utf-8"),
                      re.MULTILINE)
        if m:
            codes[m.group(1)] = cm.parent.parent.name
    for pattern in ("plugins/*/CMakeLists.txt", "archive/plugins/*/CMakeLists.txt"):
        for cm in ROOT.glob(pattern):
            m = re.search(r"PLUGIN_CODE\s+(\S+)", cm.read_text(encoding="utf-8"))
            if m:
                codes.setdefault(m.group(1), cm.parent.name)
    return codes


def render(template: str, ctx: dict[str, str]) -> str:
    """Substitute @token@ placeholders. Deliberately NOT str.format: these are
    C++/CMake templates full of braces, and escaping every one of them is how
    template bugs get in."""
    out = template
    for key, value in ctx.items():
        out = out.replace(f"@{key}@", value)
    leftover = re.findall(r"@[a-z_]+@", out)
    if leftover:
        raise RuntimeError(f"unsubstituted scaffold tokens: {sorted(set(leftover))}")
    return out


# --------------------------------------------------------------------------- templates

PLUGIN_TOML = """\
[plugin]
name      = "@name@"
slug      = "@slug@"
category  = "@category@"
status    = "in-progress"
version   = "0.1.0"
formats   = ["VST3", "AU"]
reference = "@reference@"
"""

CMAKELISTS = """\
# @slug@ — @reference@. CLAP-FIRST from birth.
#
# The SHIPPING binaries (CLAP + wrapper VST3, AUv2 on Apple) are assembled by
# shell/CMakeLists.txt via factory_clap_plugin, which the ROOT CMakeLists adds
# whenever this plugin is in the configured set (any plugin carrying a
# shell/CMakeLists.txt joins the clap-first assembly). There is NO JUCE target
# here — this file registers only the headless tests.

factory_read_version(${CMAKE_CURRENT_SOURCE_DIR}/plugin.toml @var@_VERSION)
message(STATUS "@slug@ version ${@var@_VERSION} (clap-first; shell assembled from root)")

# ---------------------------------------------------------------- DSP tests
# Headless DSP spec tests: links ONLY factory_core (no JUCE/CLAP/host); one
# CTest case per standard sample rate (argv[1] = rate, none = full matrix).
add_executable(@snake@_dsp_test tests/dsp_test.cpp)
target_link_libraries(@snake@_dsp_test PRIVATE factory_core)
target_include_directories(@snake@_dsp_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(@snake@_dsp_test PRIVATE cxx_std_20)

foreach(_fs 44100 48000 88200 96000 176400 192000)
  add_test(NAME @snake@_dsp_${_fs}
           COMMAND @snake@_dsp_test ${_fs})
endforeach()

# ------------------------------------------------------- preset wiring test
# Parameter-table + factory-preset wiring test (headless, JUCE-free; nothing
# here is sample-rate dependent, so a single CTest case).
add_executable(@snake@_preset_test tests/preset_test.cpp)
target_link_libraries(@snake@_preset_test PRIVATE factory_params factory_presets)
target_include_directories(@snake@_preset_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(@snake@_preset_test PRIVATE cxx_std_20)
add_test(NAME @snake@_preset COMMAND @snake@_preset_test)
"""

CORE_H = """\
#pragma once
//
// plugins/@slug@/@camel@Core.h — @snake@_core::@camel@Core, the framework-free
// DSP core of @name@. The CLAP shell's Policy (shell/ClapEntry.cpp) is a thin
// wrapper over this class; the headless dsp_test drives it directly. No JUCE, no
// CLAP, no allocation in process().
//
// REAL-TIME CONTRACT (hard rule): prepare() allocates everything; process() must
// not allocate, lock, block, or make syscalls. Feedback nodes need finite guards.
//
// TODO(scaffold): compose the real engine from core/include/factory_core/
// primitives (see the core-primitives skill) instead of writing DSP by hand. If
// the engine needs an FFT/STFT, derive the order from
// factory_core::fftOrderForSampleRate(fs) — a fixed order is forbidden.
//
#include "factory_core/LinearRamp.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace @snake@_core
{
    // Per-block parameter snapshot in REAL units (the shell fills it from the
    // ParamStore; tests construct it directly). Mirrors @camel@Params.h.
    struct @camel@ParamSnapshot
    {
        float mixPct = 100.0f;  // %  (0..100)
        float outDb  = 0.0f;    // dB (-24..+24)
    };

    class @camel@Core
    {
    public:
        void prepare (double sampleRate, int maxBlockIn)
        {
            fs       = sampleRate;
            maxBlock = std::max (16, maxBlockIn);

            // TODO(scaffold): preallocate every buffer the engine needs here,
            // sized for the WORST CASE at this sample rate (regression policy).

            mixRamp.reset (fs, 0.02);
            gainRamp.reset (fs, 0.02);
            mixRamp.setCurrentAndTargetValue (1.0f);
            gainRamp.setCurrentAndTargetValue (1.0f);

            reset();
            uiSampleRateHz.store ((float) fs, std::memory_order_relaxed);
        }

        // Reported latency in samples. 0 while the engine is a pass-through;
        // return the real lookahead once the engine has one (the shell reports it
        // to the host and asks for a restart when it changes).
        int latencySamples() const noexcept { return 0; }

        // Transport discontinuity / bypass exit: clear state in place. No
        // reallocation, no latency change (regression policy: reset on prepare
        // and on every bypass/channel transition).
        void reset() noexcept
        {
            // TODO(scaffold): clear the engine's state here.
            uiLevel.store (0.0f, std::memory_order_relaxed);
        }

        void process (float* L, float* R, int n,
                      const @camel@ParamSnapshot& snap) noexcept
        {
            if (L == nullptr || n <= 0)
                return;

            mixRamp.setTargetValue (std::clamp (snap.mixPct, 0.0f, 100.0f) * 0.01f);
            gainRamp.setTargetValue (std::pow (10.0f, std::clamp (snap.outDb, -24.0f, 24.0f) / 20.0f));

            float peak = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                const float mix  = mixRamp.getNextValue();
                const float gain = gainRamp.getNextValue();

                // TODO(scaffold): the engine goes here. `wet` is the processed
                // sample, `dry` the untouched input; the mix/output stage below is
                // the shape every factory plugin ends with.
                const float dryL = L[i];
                const float wetL = dryL;
                L[i] = (wetL * mix + dryL * (1.0f - mix)) * gain;

                if (R != nullptr)
                {
                    const float dryR = R[i];
                    const float wetR = dryR;
                    R[i] = (wetR * mix + dryR * (1.0f - mix)) * gain;
                }

                peak = std::max (peak, std::abs (L[i]));
            }

            // GUI/audio-shared scalars are atomics, never plain floats.
            uiLevel.store (peak, std::memory_order_relaxed);
        }

        // --- published read-outs for the editor (relaxed atomics, UI thread) ---
        std::atomic<float> uiLevel { 0.0f };
        std::atomic<float> uiSampleRateHz { 0.0f };

    private:
        double fs       = 48000.0;
        int    maxBlock = 512;

        factory_core::LinearRamp<float> mixRamp, gainRamp;
    };
} // namespace @snake@_core
"""

PARAMS_H = """\
#pragma once
//
// plugins/@slug@/@camel@Params.h — the declarative parameter table of @name@
// (single source of truth for the CLAP surface, the shell state codec and the
// editor). JUCE-free; built on factory_params::ParamDesc.
//
// Ids are stable wire identifiers: the CLAP param uid is fnv1a32(id) and saved
// state keys off them, so RENAMING AN ID BREAKS EVERY SAVED SESSION. Add
// parameters at the end; never repurpose an id. (See the add-param skill.)
//
#include "factory_params/ParamDesc.h"

#include <vector>

namespace @snake@_params
{
    inline std::vector<factory_params::ParamDesc> build@camel@Params()
    {
        using namespace factory_params;
        std::vector<ParamDesc> p;

        // TODO(scaffold): declare the plugin's real parameters here. The two
        // below are the output stage every factory plugin ends with — keep them,
        // and add the engine's controls above.
        //   floatParam  (id, name, min, max, interval, default, unit, versionHint[, skewCentre])
        //   choiceParam (id, name, { "A", "B" }, defaultIndex, versionHint)
        //   boolParam   (id, name, defaultValue, versionHint)
        p.push_back (floatParam ("mix", "Mix",     0.0f, 100.0f, 0.0f, 100.0f, " %", 1));
        p.push_back (floatParam ("out", "Output", -24.0f, 24.0f, 0.0f,   0.0f, " dB", 1));

        return p;
    }
} // namespace @snake@_params
"""

PRESETS_H = """\
#pragma once
//
// plugins/@slug@/@camel@Presets.h — the @name@ factory-preset bank.
//
// SCAFFOLD: this bank starts EMPTY (Init only). Program 0 ("Init") is synthesised
// by factory_presets::PresetSession (every managed parameter to its default) and
// is NOT listed here.
//
// To add a preset (see the add-preset skill):
//   1. Declare a constexpr factory_presets::PresetParam array of (paramID, value)
//      pairs. `value` is the parameter's REAL value in its own units (dB, %, Hz…),
//      not the normalised 0..1 — PresetSession normalises on apply.
//   2. Add a factory_presets::Preset row to kPresets and grow the bank count.
//   3. tests/preset_test.cpp verifies IDs exist and values are in range.
// Preset VALUES/NAMES are taste — do not ship without a human audition sign-off.
//
#include "factory_presets/PresetBank.h"

namespace @snake@_presets
{
    using factory_presets::Preset;
    using factory_presets::PresetParam;
    using factory_presets::PresetBank;

    // TODO(scaffold): declare preset parameter arrays and list them in kPresets.
    inline constexpr Preset* kPresets = nullptr;

    // Init-only bank until curated presets are added.
    inline const PresetBank bank { kPresets, 0 };

    // Parameters presets must never write — anything that belongs to the USER's
    // session rather than to the sound (musical key, monitoring toggles, a
    // Listen/Delta solo). Empty is a valid starting point.
    inline constexpr const char* kExclude[] = { "" };
    inline constexpr int kNumExclude = 0;
} // namespace @snake@_presets
"""

UI_MODELS_H = """\
#pragma once
//
// plugins/@slug@/ui/@camel@Models.h — the visage-free seams between the editor
// and its host shell: the lock-free status feed (audio → UI read-outs) and the
// preset list model. JUCE-free AND visage-free, so a harness can mock both and
// the contracts stay headless-compilable.
//
#include <atomic>
#include <string>
#include <vector>

namespace @snake@_ui
{
    // Pointers into the live core's published atomics (the shell wires them in
    // make@camel@ClapEditor; a harness can point them at its own dummies). All
    // reads are relaxed atomic loads on the UI thread — no locks, no copies.
    struct @camel@UiFeed
    {
        std::atomic<float>* level        = nullptr;  // peak of the last block
        std::atomic<float>* sampleRateHz = nullptr;  // prepared rate
    };

    // Program list the preset selector renders (index 0 == Init, then the bank).
    // load() applies through the real PresetSession in the shell build.
    class @camel@PresetModel
    {
    public:
        virtual ~@camel@PresetModel() = default;

        virtual std::vector<std::string> names() const = 0;
        virtual int  currentIndex() const = 0;
        virtual bool load (int index) = 0;
    };
} // namespace @snake@_ui
"""

UI_EDITOR_H = """\
#pragma once
//
// plugins/@slug@/ui/@camel@Editor.h — @snake@_ui::@camel@Editor, the JUCE-free
// Visage editor of @name@. All look-and-feel comes from the shared
// factory_ui_visage design system — no local palette, no bespoke look-and-feel
// (see the visage-ui skill for the widget API).
//
// Fixed-size editor (@design_w@×@design_h@ design px). Every control binds to the
// ParamStore by id through the shared widgets' gesture path.
//
#include "@camel@Models.h"

#include "factory_ui_visage/Theme.h"
#include "factory_ui_visage/Knob.h"
#include "factory_ui_visage/PresetSelectorView.h"
#include "factory_ui_visage/Dropdown.h"
#include "factory_ui_visage/ValueEntry.h"

#include "factory_params/ParamStore.h"

#include <visage_ui/frame.h>

#include <functional>
#include <memory>
#include <vector>

namespace @snake@_ui
{
    class @camel@Editor : public visage::Frame
    {
    public:
        static constexpr int kDesignW = @design_w@;
        static constexpr int kDesignH = @design_h@;

        @camel@Editor (const factory_ui_visage::Theme& theme,
                       factory_params::ParamStore& store,
                       const @camel@UiFeed& feed,
                       @camel@PresetModel& presets);
        ~@camel@Editor() override;

        // Host state replaced (state load): rebuild the preset selector, drop any
        // in-flight overlay edit, repaint everything.
        void onStateReplaced();

        // Once-per-frame hook (the shell pumps its inactive-edit flush here).
        void setFrameTick (std::function<void()> fn);

        void draw (visage::Canvas& canvas) override;
        void resized() override;

    private:
        void presentDropdown (std::vector<factory_ui_visage::Dropdown::Item> items,
                              int selected, visage::Frame* anchor,
                              std::function<void (int)> onSelect);
        void openValueEntry (const factory_ui_visage::ValueEntryRequest& req);
        void rebuildPresetMenu();

        float k() const;                 // uniform design scale
        float S (float v) const { return v * k(); }

        const factory_ui_visage::Theme& theme_;
        factory_params::ParamStore&     store_;
        @camel@PresetModel&             presets_;
        @camel@UiFeed                   feed_;
        std::function<void()>           frameTick_;

        // TODO(scaffold): add the plugin's real controls here.
        std::unique_ptr<factory_ui_visage::Knob> mix_, out_;

        std::unique_ptr<factory_ui_visage::PresetSelectorView> presetView_;
        // Shared overlays (added last == frontmost).
        std::unique_ptr<factory_ui_visage::Dropdown>   dropdown_;
        std::unique_ptr<factory_ui_visage::ValueEntry> valueEntry_;
    };
} // namespace @snake@_ui
"""

UI_EDITOR_CPP = """\
//
// plugins/@slug@/ui/@camel@Editor.cpp — the @name@ Visage editor. See
// @camel@Editor.h for the shape; every colour/font comes from the theme (no
// local hex, per the house rule).
//
#include "@camel@Editor.h"

#include "factory_ui_visage/Chrome.h"
#include "factory_ui_visage/Fonts.h"

#include <visage_graphics/canvas.h>

#include <utility>

namespace @snake@_ui
{
    using namespace factory_ui_visage;

    @camel@Editor::@camel@Editor (const Theme& theme, factory_params::ParamStore& store,
                                  const @camel@UiFeed& feed, @camel@PresetModel& presets)
        : theme_ (theme), store_ (store), presets_ (presets), feed_ (feed)
    {
        const auto knob = [&] (const char* id, const char* caption, int decimals)
        {
            auto k = std::make_unique<Knob> (store_, store_.indexOf (id), theme_, decimals);
            k->setNameOverride (caption);
            k->requestValueEntry = [this] (const ValueEntryRequest& r) { openValueEntry (r); };
            addChild (k.get());
            return k;
        };

        // TODO(scaffold): add the plugin's real controls here.
        mix_ = knob ("mix", "MIX", 0);
        out_ = knob ("out", "OUT", 1);

        presetView_ = std::make_unique<PresetSelectorView> (theme_);
        presetView_->requestDropdown = [this] (auto items, int sel, visage::Frame* a, auto onSel)
        { presentDropdown (std::move (items), sel, a, std::move (onSel)); };
        presetView_->onChange = [this] (int row)
        {
            if (! presets_.load (row))
                presetView_->setSelectedIndex (presets_.currentIndex());
            rebuildPresetMenu();
            redrawAll();
        };
        addChild (presetView_.get());
        rebuildPresetMenu();

        // Shared overlays last == frontmost.
        dropdown_ = std::make_unique<Dropdown> (theme_);
        addChild (dropdown_.get());
        valueEntry_ = std::make_unique<ValueEntry> (theme_);
        addChild (valueEntry_.get());
    }

    @camel@Editor::~@camel@Editor() = default;

    void @camel@Editor::onStateReplaced()
    {
        if (valueEntry_)
            valueEntry_->cancelEntry();
        if (dropdown_ && dropdown_->isOpen())
            dropdown_->close();
        rebuildPresetMenu();
        redrawAll();
    }

    void @camel@Editor::setFrameTick (std::function<void()> fn)
    {
        frameTick_ = std::move (fn);
    }

    void @camel@Editor::rebuildPresetMenu()
    {
        if (presetView_)
            presetView_->setItems (presets_.names(), presets_.currentIndex());
    }

    float @camel@Editor::k() const
    {
        const float kw = width()  / (float) kDesignW;
        const float kh = height() / (float) kDesignH;
        return kw < kh ? kw : kh;
    }

    void @camel@Editor::presentDropdown (std::vector<Dropdown::Item> items, int selected,
                                         visage::Frame* anchor, std::function<void (int)> onSelect)
    {
        if (dropdown_ == nullptr || anchor == nullptr)
            return;
        dropdown_->setBounds (0.0f, 0.0f, width(), height());
        const auto ap = anchor->positionInWindow();
        const auto mp = positionInWindow();
        dropdown_->onSelect = std::move (onSelect);
        dropdown_->open (std::move (items), selected,
                         ap.x - mp.x, ap.y - mp.y, anchor->width(), anchor->height());
    }

    void @camel@Editor::openValueEntry (const ValueEntryRequest& req)
    {
        if (valueEntry_ == nullptr)
            return;
        const auto mp = positionInWindow();
        valueEntry_->open (req.x - mp.x, req.y - mp.y, req.w, req.h,
                           req.prefill, req.fontPx, req.commit);
    }

    void @camel@Editor::draw (visage::Canvas& canvas)
    {
        // The editor owns the once-per-frame shell tick. TODO(scaffold): when a
        // live meter/visualizer is added, move the tick onto THAT frame's
        // self-redraw and leave this chrome static (dirty-region discipline).
        if (frameTick_)
            frameTick_();

        paintBackground (canvas, theme_, 0.0f, 0.0f, width(), height());
        paintCard (canvas, theme_, S (18), S (18), width() - S (36), height() - S (36));

        canvas.setColor (theme_.palette.text);
        canvas.text ("@title@", boldFont (theme_.font.title * k()),
                     visage::Font::kLeft, S (44), S (34), S (400), S (30));
        canvas.setColor (theme_.palette.textDim);
        canvas.text ("@desc_upper@", regularFont (theme_.font.caption * k()),
                     visage::Font::kLeft, S (46), S (62), S (400), S (16));

        redraw();
    }

    void @camel@Editor::resized()
    {
        if (valueEntry_)
            valueEntry_->cancelEntry();

        presetView_->setBounds (S (@preset_x@), S (36), S (276), S (30));

        // TODO(scaffold): lay out the plugin's real controls.
        mix_->setBounds (S (60),  S (150), S (152), S (158));
        out_->setBounds (S (226), S (150), S (152), S (158));

        dropdown_->setBounds (0.0f, 0.0f, width(), height());

        redrawAll();
    }
} // namespace @snake@_ui
"""

SHELL_CMAKELISTS = """\
# @slug@ CLAP shell — the SHIPPING build (clap-first from birth).
#
# Assembled from the root CMakeLists whenever @slug@ is configured (any plugin
# with a shell/CMakeLists.txt joins the clap-first assembly); it produces the
# CLAP + wrapper VST3 (+ AUv2 on Apple). The GUI (FACTORY_@var@_CLAP_GUI, ON by
# default) embeds the Visage editor. There is NO JUCE target anywhere in this
# plugin — the main CMakeLists.txt registers only the headless tests.

# plugin.toml stays the single source of truth for the version (catalog == binary).
factory_read_version(${CMAKE_CURRENT_SOURCE_DIR}/../plugin.toml @var@_CLAP_VERSION)
message(STATUS "@slug@ CLAP shell: version ${@var@_CLAP_VERSION} (from plugin.toml)")

# The impl static library: the Policy + the shared factory/entry glue
# (ClapEntry.cpp, via FACTORY_CLAP_ENTRY). The exported clap_entry lives in the
# shared ENTRY_SOURCE (shell/src/FactoryClapEntryPoint.cpp), which
# factory_clap_plugin() supplies by default — no per-plugin entry TU.
add_library(@slug@-impl STATIC ClapEntry.cpp)

target_include_directories(@slug@-impl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}          # @camel@ClapEditor.h
  ${CMAKE_CURRENT_SOURCE_DIR}/..)      # @camel@Core.h / @camel@Params.h / @camel@Presets.h

target_link_libraries(@slug@-impl
  PUBLIC
    clap
    clap-wrapper-extensions
    factory_shell
    factory_core
    factory_params
    factory_presets)

target_compile_features(@slug@-impl PRIVATE cxx_std_20)
target_compile_definitions(@slug@-impl PRIVATE
  @var@_CLAP_VERSION="${@var@_CLAP_VERSION}")
set_target_properties(@slug@-impl PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- GUI: embed the Visage editor into the clap.gui extension -----------------
# ON by default (the shipping build carries the editor); set OFF for a pure
# headless shell (validators / bring-up). This is the ONLY switch that pulls
# native Visage into the @slug@ build.
option(FACTORY_@var@_CLAP_GUI
  "Embed the Visage @camel@ editor into the @slug@ CLAP (clap.gui); pulls native visage" ON)

if(FACTORY_@var@_CLAP_GUI)
  # The shared Visage design-system library (native config) — added once per
  # build tree; another clap-first plugin's shell may already have added it.
  if(NOT TARGET factory_ui_visage)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../ui/visage
                     ${CMAKE_BINARY_DIR}/factory_ui_visage)
  endif()

  set(@var@_UI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ui)

  # The Visage-backed editor host (the single visage TU) + the JUCE-free editor.
  target_sources(@slug@-impl PRIVATE
    @camel@ClapEditor.cpp
    ${@var@_UI_DIR}/@camel@Editor.cpp)

  target_include_directories(@slug@-impl PRIVATE ${@var@_UI_DIR})

  # factory_ui_visage_clap_host brings the shared Visage IClapEditor host base +
  # factory_ui_visage (visage + params/core includes) + factory_shell + clap.
  target_link_libraries(@slug@-impl PUBLIC factory_ui_visage_clap_host)

  target_compile_definitions(@slug@-impl PRIVATE FACTORY_@var@_CLAP_GUI=1)

  message(STATUS "@slug@ CLAP shell: GUI ON (Visage editor embedded)")
else()
  message(STATUS "@slug@ CLAP shell: GUI OFF (headless shell)")
endif()

# Assemble CLAP + VST3 (+ AUv2 on Apple) with the house identifiers.
#   clap id / bundle id : jp.tatsunari-sounds.@slug@
#   AUv2 subtype        : @code@
# Generated targets: @slug@_clap / _vst3 / _all.
factory_clap_plugin(@slug@
  IMPL_TARGET        @slug@-impl
  OUTPUT_NAME        "@name@"
  VERSION            ${@var@_CLAP_VERSION}
  AUV2_SUBTYPE_CODE  @code@)
"""

SHELL_ENTRY_CPP = """\
//
// ClapEntry.cpp — the @slug@ CLAP plugin impl (IMPL_TARGET static library for
// make_clapfirst_plugins). Composes the framework-free factory pieces:
//
//   * @snake@_core::@camel@Core         — the JUCE-free DSP core (@camel@Core.h)
//   * factory_params::ParamStore           — over @snake@_params::build@camel@Params()
//   * factory_presets::PresetSession       — over the @slug@ bank + kExclude
//   * factory_shell::ClapShellPlugin<...>  — the generic CLAP glue
//
// @slug@ is clap-first FROM BIRTH: there is no JUCE processor at all, so
// migrateState is a no-op — the only wire format that exists is StateCodec v1+.
//
// clap_plugin_descriptor id: jp.tatsunari-sounds.@slug@ (reverse-DNS).
//
#include <clap/clap.h>

#include "factory_shell/ClapEntryPoint.h" // ClapShellPlugin + the shared factory/entry glue

#if FACTORY_@var@_CLAP_GUI
 // Visage-free declaration (the definition — the only visage TU — is
 // @camel@ClapEditor.cpp, compiled only under FACTORY_@var@_CLAP_GUI). Keeping
 // this header framework-free keeps THIS file headless-buildable.
 #include "@camel@ClapEditor.h"
 #include <memory>
#endif

#include "@camel@Core.h"     // @snake@_core::@camel@Core / @camel@ParamSnapshot
#include "@camel@Params.h"   // @snake@_params::build@camel@Params()
#include "@camel@Presets.h"  // @snake@_presets::bank / kExclude

#include <cstdint>
#include <vector>

#ifndef @var@_CLAP_VERSION
 #define @var@_CLAP_VERSION "0.0.0" // real value injected by CMake from plugin.toml
#endif

namespace
{
    // ── descriptor ───────────────────────────────────────────────────────────
    // TODO(scaffold): add the category feature that matches plugin.toml
    // (CLAP_PLUGIN_FEATURE_EQUALIZER / _COMPRESSOR / _DELAY / _REVERB / …) —
    // make_clapfirst derives the VST3 subcategory from this list.
    const char* const s_features[] = {
        CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
        CLAP_PLUGIN_FEATURE_STEREO,
        nullptr
    };

    const clap_plugin_descriptor_t s_desc = {
        CLAP_VERSION_INIT,
        "jp.tatsunari-sounds.@slug@",                       // id (reverse-DNS)
        "@name@",                                           // name
        "Tatsunari Sounds",                                 // vendor
        "https://github.com/tatsunari-sounds",              // url
        "",                                                 // manual_url
        "",                                                 // support_url
        @var@_CLAP_VERSION,                                 // version (from plugin.toml)
        "@description@",                                    // description
        s_features
    };

    // ── ParamStore index cache (fixed layout, computed once) ──────────────────
    // indexOf() is a string lookup; resolving once keeps process() allocation- and
    // search-free. TODO(scaffold): extend alongside @camel@Params.h.
    struct @camel@Ix
    {
        int mix, out;
    };

    @camel@Ix compute@camel@Ix (const factory_params::ParamStore& store)
    {
        @camel@Ix ix {};
        ix.mix = store.indexOf ("mix");
        ix.out = store.indexOf ("out");
        return ix;
    }

    // First touch happens on the main thread inside activate()'s priming, so the
    // magic-static init never runs on the audio thread.
    const @camel@Ix& indices (const factory_params::ParamStore& store)
    {
        static const @camel@Ix ix = compute@camel@Ix (store);
        return ix;
    }

    void fillSnapshot (const factory_params::ParamStore& store,
                       @snake@_core::@camel@ParamSnapshot& s) noexcept
    {
        const @camel@Ix& ix = indices (store);
        s.mixPct = store.value (ix.mix);
        s.outDb  = store.value (ix.out);
    }

    // ── the shell Policy for @slug@ ───────────────────────────────────────────
    struct @camel@ClapPolicy
    {
        using Core = @snake@_core::@camel@Core;

        static const clap_plugin_descriptor_t* descriptor() { return &s_desc; }

        static std::vector<factory_params::ParamDesc> params()
        {
            return @snake@_params::build@camel@Params();
        }

        static const factory_presets::PresetBank& presetBank()
        {
            return @snake@_presets::bank;
        }

        static std::vector<std::string> excludeIds()
        {
            return std::vector<std::string> (
                @snake@_presets::kExclude,
                @snake@_presets::kExclude + @snake@_presets::kNumExclude);
        }

        // TODO(scaffold): true if the engine reads a sidechain input.
        static constexpr bool kHasSidechain = false;

        // No legacy build ever existed — the whole table is the CLAP surface.
        static bool isClapExposed (const factory_params::ParamDesc&) { return true; }

        // Clap-first from birth: StateCodec v1+ is the only wire format, so there
        // is nothing to migrate (foreign blobs are rejected upstream).
        static void migrateState (factory_presets::StateModel&) {}

        static void prepare (Core& core, double sampleRate, std::uint32_t maxFrames)
        {
            core.prepare (sampleRate, static_cast<int> (maxFrames));
        }

        static void process (Core& core, const factory_params::ParamStore& store,
                             float* L, float* R, const float*, const float*,
                             std::uint32_t frames)
        {
            @snake@_core::@camel@ParamSnapshot snap;
            fillSnapshot (store, snap);
            core.process (L, R, static_cast<int> (frames), snap);
        }

        static std::uint32_t latencySamples (const Core& core)
        {
            return static_cast<std::uint32_t> (core.latencySamples());
        }

        // Shell reset hook: on a transport discontinuity the shell clears the
        // core's state in place (no realloc, no latency change).
        static void reset (Core& core) { core.reset(); }

        // Silent frames activate() runs so latencySamples() reports the SETTLED
        // value before the shell latches it. 0 is correct for a constant-latency
        // core (the scaffold's pass-through is one). TODO(scaffold): return a
        // power-of-two frame count once the engine's reported latency depends on
        // parameters that only settle after the first process() call.
        static std::uint32_t primeFrames() { return 0u; }

#if FACTORY_@var@_CLAP_GUI
        // The presence of kHasEditor makes the generic shell expose CLAP_EXT_GUI
        // (+ Linux posix-fd); makeEditor builds the Visage editor host over the
        // shell's live core / store / session.
        static constexpr bool kHasEditor = true;

        static std::unique_ptr<factory_shell::IClapEditor>
        makeEditor (Core& core, factory_params::ParamStore& store,
                    factory_presets::PresetSession& session, const clap_host_t* host)
        {
            return @snake@_shell::make@camel@ClapEditor (core, store, session, host);
        }
#endif
    };

} // namespace

// The single-plugin CLAP factory + the three common-named entry hooks. The
// exported clap_entry symbol lives in the shared ENTRY_SOURCE and forwards here.
FACTORY_CLAP_ENTRY (@camel@ClapPolicy)
"""

SHELL_EDITOR_H = """\
#pragma once
//
// @camel@ClapEditor.h — the framework-free factory the Policy hands the generic
// shell to build the CLAP editor. The DECLARATION is deliberately Visage-free
// (only the factory_shell seam + CLAP host type + forward-declared refs), so
// ClapEntry.cpp — which composes the headless Policy — includes it WITHOUT
// pulling in any GUI framework. The visage-backed definition lives in
// @camel@ClapEditor.cpp, the single translation unit in the @slug@ CLAP build
// that links Visage; it is compiled ONLY under FACTORY_@var@_CLAP_GUI.
//
#include "factory_shell/ClapEditor.h"

#include <clap/clap.h>

#include <memory>

namespace factory_params  { class ParamStore; }
namespace factory_presets { class PresetSession; }
namespace @snake@_core    { class @camel@Core; }

namespace @snake@_shell
{
    // Construct the Visage editor host, backed by the SAME live objects the CLAP
    // shell owns: the core (its ui* atomics feed the read-outs), the ParamStore
    // (every control binds by id), and the PresetSession (the real program list /
    // apply path). `host` lets the editor relay GUI-driven bulk changes back
    // (rescan + mark-dirty) and ask for a param flush while inactive. The returned
    // object allocates no native window until its create() is called by the shell.
    std::unique_ptr<factory_shell::IClapEditor>
    make@camel@ClapEditor (@snake@_core::@camel@Core& core,
                           factory_params::ParamStore& store,
                           factory_presets::PresetSession& session,
                           const clap_host_t* host);
}
"""

SHELL_EDITOR_CPP = """\
//
// @camel@ClapEditor.cpp — the Visage-backed factory_shell::IClapEditor for the
// @slug@ CLAP plugin. It DERIVES the shared
// factory_ui_visage::FixedSizeVisageClapEditor (which owns the CLAP↔Visage host
// boilerplate + the fixed-size resize surface), so this file keeps ONLY the
// plugin-specific pieces: constructing the editor over the shell's live
// core/store/session, the real preset model, the status feed, and the per-frame
// flush. Compiled ONLY under FACTORY_@var@_CLAP_GUI (the single visage-linking TU
// of the @slug@ CLAP build).
//
// FIXED-SIZE editor (@design_w@×@design_h@ design px). Switch the base class to
// ResizableVisageClapEditor when the editor grows a resize surface.
//
#include "@camel@ClapEditor.h"

#include "@camel@Editor.h"
#include "@camel@Models.h"

#include "@camel@Core.h"
#include "factory_params/ParamStore.h"
#include "factory_presets/PresetSession.h"
#include "factory_ui_visage/Theme.h"
#include "factory_ui_visage/ClapEditorHost.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // REAL preset model over the shell's PresetSession: lists Init + the bank,
    // applies through the session, then notifies the host (rescan + mark-dirty — a
    // bulk change records no per-parameter automation).
    class SessionPresetModel final : public @snake@_ui::@camel@PresetModel
    {
    public:
        SessionPresetModel (factory_presets::PresetSession& session, std::function<void()> onLoaded)
            : session_ (session), onLoaded_ (std::move (onLoaded)) {}

        std::vector<std::string> names() const override
        {
            std::vector<std::string> v;
            const int n = session_.numPrograms();
            v.reserve ((std::size_t) n);
            for (int i = 0; i < n; ++i)
                v.push_back (session_.programName (i));
            return v;
        }

        int currentIndex() const override { return session_.currentProgram(); }

        bool load (int index) override
        {
            if (index < 0 || index >= session_.numPrograms())
                return false;
            session_.applyProgram (index);
            if (onLoaded_) onLoaded_();
            return true;
        }

    private:
        factory_presets::PresetSession& session_;
        std::function<void()>           onLoaded_;
    };

    class @camel@ClapEditorImpl final : public factory_ui_visage::FixedSizeVisageClapEditor
    {
    public:
        @camel@ClapEditorImpl (@snake@_core::@camel@Core& core, factory_params::ParamStore& store,
                               factory_presets::PresetSession& session, const clap_host_t* host)
            : FixedSizeVisageClapEditor (host, store,
                                         @snake@_ui::@camel@Editor::kDesignW,
                                         @snake@_ui::@camel@Editor::kDesignH),
              theme_ (factory_ui_visage::Theme::defaults()),
              presets_ (session, [this] { onPresetLoaded(); })
        {
            feed_.level        = &core.uiLevel;
            feed_.sampleRateHz = &core.uiSampleRateHz;
        }

        ~@camel@ClapEditorImpl() override { destroy(); }

    protected:
        visage::Frame* buildEditor() override
        {
            editor_ = std::make_unique<@snake@_ui::@camel@Editor> (theme_, store_, feed_, presets_);
            // Once per drawn frame: flush pending GUI edits to the host while the
            // plugin is inactive (harmless while active).
            editor_->setFrameTick ([this] { flushEditsIfInactive(); });
            app_->addChild (*editor_);
            return editor_.get();
        }

        visage::Frame* editorFrame() const override { return editor_.get(); }
        void resetEditor() override { editor_.reset(); }
        void onStateReplacedHook() override { if (editor_) editor_->onStateReplaced(); }

    private:
        // Bulk change (preset load): the host re-pulls values/text + marks dirty (no
        // per-parameter automation), then the editor resyncs to the replaced state.
        void onPresetLoaded()
        {
            notifyHostEdited();
            if (editor_) editor_->onStateReplaced();
        }

        factory_ui_visage::Theme     theme_;   // owned; the editor holds a const ref
        @snake@_ui::@camel@UiFeed    feed_;
        SessionPresetModel           presets_;

        std::unique_ptr<@snake@_ui::@camel@Editor> editor_;
    };
} // namespace

namespace @snake@_shell
{
    std::unique_ptr<factory_shell::IClapEditor>
    make@camel@ClapEditor (@snake@_core::@camel@Core& core,
                           factory_params::ParamStore& store,
                           factory_presets::PresetSession& session,
                           const clap_host_t* host)
    {
        return std::make_unique<@camel@ClapEditorImpl> (core, store, session, host);
    }
}
"""

DSP_TEST = """\
//
// dsp_test.cpp — headless verification of the @slug@ DSP core.
//
// SCAFFOLD STUB: this test FAILS on purpose until real spec-based checks are
// written. See the write-dsp-test skill and docs/regression-policy.md — every
// check needs an independent oracle (never derived from the code under test) and
// must run across the full sample-rate matrix.
//
#include "@camel@Core.h"

#include "factory_core/testing/DspInvariants.h"

#include <cstdio>
#include <string>

namespace
{
    namespace fct = factory_core::testing;

    int g_failures = 0;
    void fail (const std::string& m) { std::printf ("  FAIL: %s\\n", m.c_str()); ++g_failures; }

    void coreTests (double Fs)
    {
        std::printf ("@slug@ core @ Fs=%.0f\\n", Fs);

        @snake@_core::@camel@Core core;
        core.prepare (Fs, 512);

        // TODO(scaffold): replace with real checks. Typical gates:
        //   - independent static oracle for the quantitative behaviour
        //     (z-domain for filters; analytic counts/levels for non-linear)
        //   - fct::impulseResponseNonIncreasing (...) at the WORST-CASE setting
        //     for any feedback path
        //   - fct::allFinite / fct::peakAbs over a long hold with a realistic
        //     peak bound (never a 1e6 "not-NaN" tolerance)
        //   - fct::resolutionFollowsSampleRate (Fs) if the core uses FFT/STFT
        fail ("dsp_test is a scaffold stub — write spec-based checks for @slug@");
    }
}

int main (int argc, char** argv)
{
    // Full standard rate matrix by default; CTest passes one rate as argv[1].
    for (double Fs : fct::sampleRatesFromArgs (argc, argv))
        coreTests (Fs);

    if (g_failures == 0) { std::printf ("OK: all checks passed.\\n"); return 0; }
    std::printf ("FAILED: %d check(s).\\n", g_failures);
    return 1;
}
"""

PRESET_TEST = """\
//
// plugins/@slug@/tests/preset_test.cpp — headless wiring test for the @name@
// parameter table and factory-preset bank. Links only factory_params +
// factory_presets (no JUCE, no CLAP, no DSP), which is also what PROVES those
// two headers stay framework-free.
//
// Gates:
//   * table sanity: unique ids/uids, uid == fnv1a32(id), sane ranges, defaults
//     inside range;
//   * every preset value targets an EXISTING parameter and lies inside its range
//     (a typo'd id or out-of-range value fails here, not in a DAW);
//   * no preset writes an excluded id;
//   * PresetSession exposes Init + the bank and leaves excluded ids untouched.
//
#include "@camel@Params.h"
#include "@camel@Presets.h"

#include "factory_params/ParamStore.h"
#include "factory_presets/PresetSession.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

static int g_failures = 0;

static void fail (const std::string& msg)
{
    ++g_failures;
    std::printf ("FAIL: %s\\n", msg.c_str());
}

int main()
{
    const auto table = @snake@_params::build@camel@Params();

    // --- table sanity ---------------------------------------------------------
    if (table.empty())
        fail ("parameter table is empty");

    std::set<std::string> ids;
    std::set<unsigned>    uids;
    for (const auto& d : table)
    {
        if (! ids.insert (d.id).second)
            fail ("duplicate id " + d.id);
        if (! uids.insert (d.uid).second)
            fail ("uid collision at " + d.id);
        if (d.uid != factory_params::fnv1a32 (d.id))
            fail ("uid not fnv1a32(id) for " + d.id);
        if (! (d.minValue < d.maxValue))
            fail ("empty range on " + d.id);
        if (d.defaultValue < d.minValue || d.defaultValue > d.maxValue)
            fail ("default out of range on " + d.id);
    }

    factory_params::ParamStore store (table);

    // --- every preset value hits an existing, in-range, non-excluded parameter --
    const auto& bank = @snake@_presets::bank;
    std::set<std::string> names;
    for (int p = 0; p < bank.numPresets; ++p)
    {
        const auto& pr = bank.presets[p];
        if (! names.insert (pr.name).second)
            fail (std::string ("duplicate preset name ") + pr.name);
        for (int e = 0; e < pr.numParams; ++e)
        {
            const auto& pp = pr.params[e];
            const int idx = store.indexOf (pp.paramID);
            if (idx < 0)
            {
                fail (std::string (pr.name) + " references unknown id " + pp.paramID);
                continue;
            }
            const auto& d = store.desc (idx);
            if (pp.value < d.minValue || pp.value > d.maxValue)
                fail (std::string (pr.name) + "." + pp.paramID + " value out of range");
            for (int x = 0; x < @snake@_presets::kNumExclude; ++x)
                if (std::string (pp.paramID) == @snake@_presets::kExclude[x])
                    fail (std::string (pr.name) + " writes excluded id " + pp.paramID);
        }
    }

    // --- PresetSession behaviour ------------------------------------------------
    std::vector<std::string> excl (@snake@_presets::kExclude,
                                   @snake@_presets::kExclude + @snake@_presets::kNumExclude);
    factory_presets::PresetSession session (store, bank, excl);

    if (session.numPrograms() != 1 + bank.numPresets)
        fail ("numPrograms != 1 (Init) + bank size");

    for (int prog = 0; prog < session.numPrograms(); ++prog)
    {
        session.applyProgram (prog);
        if (session.isDirty())
            fail ("dirty right after applyProgram " + std::to_string (prog));
    }

    // Init restores every managed parameter to its default.
    session.applyProgram (0);
    for (int i = 0; i < (int) table.size(); ++i)
    {
        bool excluded = false;
        for (int x = 0; x < @snake@_presets::kNumExclude; ++x)
            if (table[(std::size_t) i].id == @snake@_presets::kExclude[x])
                excluded = true;
        if (excluded)
            continue;
        if (store.value (i) != table[(std::size_t) i].defaultValue)
            fail ("Init left " + table[(std::size_t) i].id + " off its default");
    }

    // TODO(scaffold): add the plugin's own structural preset gates here (e.g.
    // "every sound preset pins the quality mode"), the way pitch-fix gates its
    // two-family bank.

    if (g_failures > 0)
    {
        std::printf ("%d failure(s)\\n", g_failures);
        return 1;
    }
    std::printf ("@snake@_preset_test OK\\n");
    return 0;
}
"""

# --------------------------------------------------------------------------- main


def main() -> int:
    ap = argparse.ArgumentParser(description="Scaffold a new clap-first plugin under plugins/<slug>/")
    ap.add_argument("slug", help="kebab-case directory name, e.g. tape-echo")
    ap.add_argument("--name", help='product name, e.g. "Tatsunari Tape Echo" (default: from slug)')
    ap.add_argument("--category", default="FX", help="catalog category, e.g. Dynamics / EQ / Reverb")
    ap.add_argument("--reference", default="—", help="reference gear/plugin the design chases")
    ap.add_argument("--code", help="unique 4-char AUv2 subtype code (default: derived from slug)")
    ap.add_argument("--description", help="one-line CLAP host description (default: from name)")
    ap.add_argument("--design-size", default="920x560",
                    help="fixed editor design size in px, WxH (default: 920x560)")
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
        print(f"error: subtype code '{code}' must be 4 chars, first uppercase", file=sys.stderr)
        return 1
    codes = existing_codes()
    if code in codes:
        print(f"error: code '{code}' already used by {codes[code]} — pass --code", file=sys.stderr)
        return 1

    m = re.fullmatch(r"(\d{3,4})x(\d{3,4})", args.design_size)
    if not m:
        print(f"error: --design-size '{args.design_size}' must look like 920x560", file=sys.stderr)
        return 1
    design_w, design_h = m.group(1), m.group(2)

    description = args.description or f"{name} — {args.reference}"

    ctx = dict(
        slug=slug, snake=snake(slug), camel=camel(slug),
        var=snake(slug).upper(), name=name, title=name.upper(), code=code,
        category=args.category, reference=args.reference,
        description=description, desc_upper=description.upper(),
        design_w=design_w, design_h=design_h,
        # Preset selector sits right-aligned in the header of the design canvas.
        preset_x=str(int(design_w) - 320),
    )

    cm = camel(slug)
    files = {
        dest / "plugin.toml": PLUGIN_TOML,
        dest / "CMakeLists.txt": CMAKELISTS,
        dest / f"{cm}Core.h": CORE_H,
        dest / f"{cm}Params.h": PARAMS_H,
        dest / f"{cm}Presets.h": PRESETS_H,
        dest / "ui" / f"{cm}Models.h": UI_MODELS_H,
        dest / "ui" / f"{cm}Editor.h": UI_EDITOR_H,
        dest / "ui" / f"{cm}Editor.cpp": UI_EDITOR_CPP,
        dest / "shell" / "CMakeLists.txt": SHELL_CMAKELISTS,
        dest / "shell" / "ClapEntry.cpp": SHELL_ENTRY_CPP,
        dest / "shell" / f"{cm}ClapEditor.h": SHELL_EDITOR_H,
        dest / "shell" / f"{cm}ClapEditor.cpp": SHELL_EDITOR_CPP,
        dest / "tests" / "dsp_test.cpp": DSP_TEST,
        dest / "tests" / "preset_test.cpp": PRESET_TEST,
    }
    for path, template in files.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(render(template, ctx), encoding="utf-8")
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
Scaffolded plugins/{slug}/ — CLAP-FIRST (AUv2 subtype {code}). Next steps:
  1. Put the DSP engine in core/include/factory_core/ (header-only,
     framework-free) and compose it in {cm}Core.h — reuse existing
     primitives where possible (core-primitives skill).
  2. Replace the TODO(scaffold) markers: {cm}Params.h (real parameter
     table), {cm}Core.h (engine), shell/ClapEntry.cpp (index cache +
     snapshot + CLAP category feature), ui/{cm}Editor.cpp (controls).
  3. Write real checks in tests/dsp_test.cpp (the stub FAILS by design).
  4. Build & test:  cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
                    cmake --build build && ctest --test-dir build -R {snake(slug)}
  5. Add the slug to the clap.yml matrix so its clap-validator leg runs.
""")
    return 0


if __name__ == "__main__":
    sys.exit(main())
