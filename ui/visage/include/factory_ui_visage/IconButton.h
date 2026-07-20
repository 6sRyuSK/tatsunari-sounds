#pragma once

#include "factory_ui_visage/Theme.h"
#include "factory_ui_visage/Icons.h"

#include <visage_ui/frame.h>

#include <functional>

//
// factory_ui_visage::IconButton — a small square glyph button, ported from
// rs::RsIconButton: a themed rounded button with a stroked/filled glyph and two
// modes. In `momentary` mode it just fires onClick (header Undo/Redo/Copy); in
// `toggle` mode it keeps an on/off state (tinted accent when on) and fires
// onToggle. Hover/press draw the soft highlight the RS button used.
//
namespace factory_ui_visage
{
    class IconButton : public visage::Frame
    {
    public:
        enum class Mode { momentary, toggle };

        IconButton (const Theme& theme, icons::Glyph glyph, Mode mode = Mode::momentary);

        void setGlyph (icons::Glyph glyph);
        void setToggleState (bool on);
        bool toggleState() const { return on_; }

        // Directional A/B copy affordance (round-3 fix 4): instead of a glyph, draw
        // fixed "A" (left) / "B" (right) letters in the accent with an arrow between
        // them; only the arrow flips to show the copy direction. reversed=false =>
        // "A->B" (arrow points right), reversed=true => "B->A" (arrow points left).
        // Matches the shipped JUCE RsIconButton directional mode. Calling this puts
        // the button in directional mode; setGlyph() switches it back to a glyph.
        void setDirection (bool reversed) { directional_ = true; reversedDir_ = reversed; redraw(); }

        // Dim the glyph (a disabled affordance, e.g. Undo/Redo with nothing to
        // undo). Purely visual — the owner still gates the action in onClick.
        void setDimmed (bool dim);
        bool dimmed() const { return dimmed_; }

        std::function<void()>     onClick;   // fired on every press
        std::function<void (bool)> onToggle; // fired (with the new state) in toggle mode

        void draw (visage::Canvas& canvas) override;
        void mouseDown (const visage::MouseEvent& e) override;
        void mouseUp (const visage::MouseEvent& e) override;
        void mouseEnter (const visage::MouseEvent& e) override;
        void mouseExit (const visage::MouseEvent& e) override;

    private:
        const Theme& theme_;
        icons::Glyph glyph_;
        Mode mode_;
        bool on_ = false;
        bool hover_ = false;
        bool down_ = false;
        bool dimmed_ = false;
        bool directional_ = false; // draw A->B letters instead of a glyph (fix 4)
        bool reversedDir_ = false; // false = A->B, true = B->A
    };
}
