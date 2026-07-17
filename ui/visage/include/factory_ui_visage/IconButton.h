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
    };
}
