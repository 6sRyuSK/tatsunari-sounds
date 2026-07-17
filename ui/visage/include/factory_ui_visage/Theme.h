#pragma once

#include <array>
#include <cstdint>
#include <string>

//
// factory_ui_visage::Theme — the JSON-backed design-system model for the Visage
// UI foundation. It carries the EXISTING "kawaii" warm-white design system
// (transcribed verbatim from ui/include/factory_ui/FactoryLookAndFeel.h and
// FactoryChrome.h) as plain data so the look can be edited live (hot reload)
// without recompiling.
//
// This header is JUCE-free AND visage-free: it is pure C++/std, so the theme
// model + parser can be unit-tested headless with a host compiler. The widgets
// (Knob/PillToggle/Chrome) consume a Theme and translate its numbers into
// visage draw calls.
//
// Colours are stored as 0xAARRGGBB packed integers (the same convention as JUCE
// juce::Colour and visage::Color). In JSON they are written as "#aarrggbb" hex
// strings.
//
namespace factory_ui_visage
{
    // Warm-white palette — one coral accent, soft shadows, six pastel band hues.
    struct Palette
    {
        std::uint32_t background   = 0xfffdf6f2; // warm white
        std::uint32_t backgroundLo = 0xfffbeae2; // gradient foot
        std::uint32_t panel        = 0xffffffff; // card white
        std::uint32_t panelLo      = 0xfffff4ee; // card foot
        std::uint32_t track        = 0xfff2ddd4; // grid / outline
        std::uint32_t accent       = 0xffff7a6b; // coral
        std::uint32_t accentDim    = 0xffffd6cd; // pale coral
        std::uint32_t text         = 0xff6b5750; // soft cocoa
        std::uint32_t textSecondary= 0xff8f7a72; // caption mid-tone (labels on cards)
        std::uint32_t textDim      = 0xffb9a39b; // muted
        std::uint32_t shadow       = 0x33d6a89a; // warm soft shadow

        // One pastel hue per band — warm-biased kawaii spread (multi-band plugins).
        std::array<std::uint32_t, 6> bandColours { {
            0xffff6f91, // strawberry
            0xffff9472, // coral
            0xffffba6b, // apricot
            0xff7fd1ae, // mint
            0xff79b8ef, // sky
            0xffb79be8  // lavender
        } };
    };

    // Rotary-knob geometry, all ratios of the knob radius unless noted. Mirrors
    // FactoryLookAndFeel::drawRotarySlider exactly.
    struct KnobMetrics
    {
        float boundsInset      = 6.0f;   // px inset of the drawing bounds (JUCE reduced(6))
        float lineWidthRatio   = 0.18f;  // stroke width = radius * this
        float glowWidthFactor  = 1.9f;   // value-arc glow stroke = lineW * this
        float glowAlpha        = 0.25f;  // glow accent alpha
        float bodyInsetFactor  = 1.7f;   // body radius = radius - lineW * this
        float shadowBlurFactor = 0.5f;   // body shadow blur = max(3, bodyR * this)
        float shadowOffsetX    = 0.0f;   // body shadow offset
        float shadowOffsetY    = 2.0f;
        float pointerDotFactor = 0.55f;  // dot radius = max(2, lineW * this)
        float pointerPosFactor = 0.62f;  // dot sits at bodyR * this from centre
        // Rotary sweep — JUCE Slider defaults (juce_Slider.cpp): 1.2*pi .. 2.8*pi,
        // measured clockwise from 12 o'clock. Stored in radians.
        float arcStart         = 3.769911184307752f; // 1.2 * pi
        float arcEnd           = 8.796459430051420f; // 2.8 * pi
    };

    // Pill-toggle geometry. Mirrors FactoryLookAndFeel::drawToggleButton.
    struct ToggleMetrics
    {
        float height             = 20.0f; // pill height cap (JUCE jmin(20, h))
        float widthFactor        = 1.7f;  // pill width = height * this
        float knobInset          = 4.0f;  // knob diameter = height - this
        float cornerRadiusFactor = 0.5f;  // corner radius = height * this
        float textGap            = 8.0f;  // gap before the caption text
    };

    // Rounded-card geometry. Mirrors factory_ui::paintCard / dropShadowFor.
    struct CardMetrics
    {
        float cornerRadius  = 10.0f;
        float outlineWidth  = 1.2f;
        float shadowBlur    = 16.0f;
        float shadowOffsetX = 0.0f;
        float shadowOffsetY = 5.0f;
    };

    // Font pixel sizes. Bold flags mirror the JUCE editors (title/callout/labelBold
    // are bold; label is regular).
    struct FontSizes
    {
        float label     = 13.0f;
        float labelBold = 13.0f;
        float title     = 20.0f;
        float callout   = 14.0f;
        float caption   = 11.0f; // small all-caps captions on cards (P2b widgets)
    };

    // Segmented-strip geometry (reference: rs::RsSegmented). A rounded track with
    // an accent "pill" on the active segment.
    struct SegmentedMetrics
    {
        float height           = 28.0f; // control height cap
        float cornerRadius     = 9.0f;  // track corner (rs radius::badge)
        float pillInset        = 3.0f;  // active-pill inset inside a cell (rs reduced(3))
        float pillCornerRadius = 7.0f;  // active-pill corner (rs radius::badge - 2)
    };

    // Overlay dropdown list geometry (our own popup; visage has no combo).
    struct DropdownMetrics
    {
        float rowHeight      = 26.0f;
        float cornerRadius   = 10.0f;
        float paddingX       = 10.0f;
        float paddingY       = 6.0f;
        float separatorInset = 10.0f;
        float shadowBlur     = 16.0f;
        float shadowOffsetY  = 6.0f;
    };

    // Square glyph button geometry (reference: rs::RsIconButton).
    struct IconButtonMetrics
    {
        float cornerRadius     = 9.0f;
        float glyphInsetFactor = 0.28f; // glyph area = bounds.reduced(height * this)
    };

    // Label+value row that opens a dropdown (reference: rs::RsValueSetting).
    struct ValueSettingMetrics
    {
        float cornerRadius = 9.0f;
        float paddingX     = 9.0f;
        float iconSize     = 16.0f;
    };

    // Compact horizontal slider (reference: rs::RsLinkSlider).
    struct LinkSliderMetrics
    {
        float cornerRadius   = 9.0f;
        float paddingX       = 9.0f;
        float trackHeight    = 7.0f;
        float trackCorner    = 3.5f;
        float captionColumn  = 76.0f;
        float valueColumn    = 52.0f;
        float glyphSize      = 16.0f;
    };

    // Spectrum-analyser display geometry + range (reference: SpectrumDisplay.h).
    struct SpectrumMetrics
    {
        float cornerRadius    = 12.0f;
        float topDb           = 0.0f;    // dB at the plot's top edge
        float bottomDb        = -100.0f; // dB at the plot's foot
        float traceWidth      = 2.0f;    // smoothed-trace stroke
        float peakWidth       = 1.4f;    // peak-hold stroke
        float fillTopAlpha    = 0.33f;   // area-fill alpha at the top
        float fillBottomAlpha = 0.02f;   // area-fill alpha at the foot
    };

    struct Theme
    {
        Palette             palette;
        KnobMetrics         knob;
        ToggleMetrics       toggle;
        CardMetrics         card;
        FontSizes           font;
        SegmentedMetrics    segmented;
        DropdownMetrics     dropdown;
        IconButtonMetrics   iconButton;
        ValueSettingMetrics valueSetting;
        LinkSliderMetrics   linkSlider;
        SpectrumMetrics     spectrum;

        // Compiled-in fallback carrying the values above verbatim.
        static Theme defaults();

        // Strict parse of a JSON theme document. Starts from defaults() and
        // overrides every key present; unknown keys and malformed syntax/types/
        // colours HARD-FAIL. Returns false and fills `error` (with a human message,
        // no exceptions) on any failure — safe to call from the wasm bridge.
        static bool tryParse (const std::string& jsonText, Theme& out, std::string& error);

        // Convenience wrappers for host tools/tests (these THROW std::runtime_error
        // on malformed input). Do not call from the exception-free wasm path.
        static Theme fromJsonText (const std::string& jsonText);
        static Theme fromJsonFile (const std::string& path);

        // Re-serialise to a canonical JSON document (round-trips through tryParse).
        std::string toJson() const;

        bool operator== (const Theme& other) const;
        bool operator!= (const Theme& other) const { return ! (*this == other); }
    };
}
