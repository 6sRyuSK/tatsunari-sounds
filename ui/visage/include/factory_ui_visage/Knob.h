#pragma once

#include "factory_ui_visage/Theme.h"
#include "factory_params/ParamStore.h"
#include "factory_params/Range.h"

#include <string>
#include <utility>

#include <visage_ui/frame.h>

//
// factory_ui_visage::Knob — a rotary knob that reproduces the factory look
// (value arc + soft glow, top-lit body gradient, track outline, drop shadow,
// pointer dot) exactly like FactoryLookAndFeel::drawRotarySlider, drawn on a
// visage::Frame instead of a JUCE Graphics.
//
// It binds to a factory_params::ParamStore by parameter index: it READS the live
// value (store.value) every time it draws, and WRITES through the UI gesture path
// (beginGesture / setFromUi / endGesture) exactly as a host-facing editor would.
//
// Editing: vertical + horizontal drag (RotaryHorizontalVerticalDrag-like), shift
// for fine, double-click to reset to default, mouse-wheel to step. Below the dial
// it shows the formatted value and the parameter name (JUCE text-below convention).
//
namespace factory_ui_visage
{
    class Knob : public visage::Frame
    {
    public:
        // `theme` must outlive the knob: the gallery owns one Theme and mutates it
        // in place for hot reload, so the reference always sees the current values.
        Knob (factory_params::ParamStore& store, int paramIndex, const Theme& theme, int decimals = 1);

        // Per-instance arc/pointer accent (0xAARRGGBB). 0 (the default) keeps the
        // theme accent, so existing call sites are unaffected; a plugin editor sets
        // this to give a knob its own accent (e.g. the RS amber ATK/REL, mint TILT)
        // without forking the widget.
        void setAccentColour (std::uint32_t argb) { accentOverride_ = argb; redraw(); }

        // Override the label drawn under the dial (default: the parameter's display
        // name). Lets a plugin editor use its own short/upper-case caption (e.g. the
        // RS "ATK"/"REL"/"TILT") without forking the widget. Empty = use desc.name.
        void setNameOverride (std::string name) { nameOverride_ = std::move (name); redraw(); }

        void draw (visage::Canvas& canvas) override;
        void mouseDown (const visage::MouseEvent& e) override;
        void mouseDrag (const visage::MouseEvent& e) override;
        void mouseUp (const visage::MouseEvent& e) override;
        bool mouseWheel (const visage::MouseEvent& e) override;

        int paramIndex() const { return index_; }

    private:
        float currentNorm() const;    // live store value -> normalised 0..1
        void  writeNorm (float norm); // normalised -> real, store via setFromUi, redraw

        factory_params::ParamStore& store_;
        int index_;
        const Theme& theme_;
        int decimals_;
        factory_params::RangeSpec range_;

        std::uint32_t accentOverride_ = 0; // 0 == use theme accent
        std::string   nameOverride_;       // empty == use desc.name

        bool dragging_ = false;
        float dragNorm_ = 0.0f;         // authoritative accumulator during a drag
        visage::Point lastDragPos_;
    };
}
