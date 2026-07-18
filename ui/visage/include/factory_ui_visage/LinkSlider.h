#pragma once

#include "factory_ui_visage/Theme.h"
#include "factory_ui_visage/Icons.h"
#include "factory_params/ParamStore.h"
#include "factory_params/Range.h"

#include <string>

#include <visage_ui/frame.h>

//
// factory_ui_visage::LinkSlider — a compact horizontal amount slider, ported from
// rs::RsLinkSlider: a white card with an optional leading glyph + caption on the
// left, a coral-filled track in the middle, and the formatted value on the right.
// Binds to a Float parameter in a ParamStore (reads store.value, writes via the
// UI gesture path). The drag is mapped 1:1 with the DRAWN track width (not the
// whole component), and vertical drag (up = increase) moves the value too, exactly
// like the RS control. Double-click resets to the default.
//
namespace factory_ui_visage
{
    class LinkSlider : public visage::Frame
    {
    public:
        // No-glyph form (plain slider). `decimals` feeds factory_params::formatValue.
        LinkSlider (factory_params::ParamStore& store, int paramIndex, const Theme& theme,
                    std::string caption, int decimals = 1);
        // Leading-glyph form (e.g. STEREO LINK).
        LinkSlider (factory_params::ParamStore& store, int paramIndex, const Theme& theme,
                    std::string caption, icons::Glyph glyph, int decimals = 1);

        // Override the caption-column width in px (0 = theme.linkSlider.captionColumn).
        // The JUCE RsLinkSlider sizes this per instance — STEREO LINK 86, MIX/OUT 34 —
        // so a single theme value can't match all three; the RS editor sets it
        // per-slider to restore the MIX/OUT track width (round-3 fix 3).
        void setCaptionColumnPx (float px) { captionColumnPx_ = px; redraw(); }

        void draw (visage::Canvas& canvas) override;
        void mouseDown (const visage::MouseEvent& e) override;
        void mouseDrag (const visage::MouseEvent& e) override;
        void mouseUp (const visage::MouseEvent& e) override;

        int paramIndex() const { return index_; }

    private:
        struct Rect { float x = 0, y = 0, w = 0, h = 0; };
        struct Layout { Rect icon, caption, track, value; };

        Layout computeLayout() const;
        float  currentNorm() const;
        void   writeNorm (float norm);

        factory_params::ParamStore& store_;
        int index_;
        const Theme& theme_;
        std::string caption_;
        bool hasGlyph_;
        icons::Glyph glyph_;
        int decimals_;
        factory_params::RangeSpec range_;

        float captionColumnPx_ = 0.0f; // 0 == theme.linkSlider.captionColumn

        bool  dragging_ = false;
        float dragStartNorm_ = 0.0f;
        visage::Point dragStartPos_;
    };
}
