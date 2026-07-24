#pragma once

#include "factory_ui_visage/Theme.h"
#include "factory_params/ParamStore.h"

#include <functional>
#include <string>
#include <utility>

#include <visage_ui/frame.h>

//
// factory_ui_visage::PillToggle — the cute rounded pill toggle from
// FactoryLookAndFeel::drawToggleButton: a track/accent pill with a white knob
// that slides, plus a caption to the right. Bound to a bool parameter in a
// ParamStore the same way the Knob is (reads store.value, writes via the UI
// gesture path).
//
namespace factory_ui_visage
{
    class PillToggle : public visage::Frame
    {
    public:
        PillToggle (factory_params::ParamStore& store, int paramIndex, const Theme& theme);

        void draw (visage::Canvas& canvas) override;
        void mouseDown (const visage::MouseEvent& e) override;

        // Called after a click commits the new state (mirrors juce::Button::onClick),
        // with the resulting on/off. Lets a panel react — e.g. an exclusive "Listen"
        // that clears the other bands when it turns on. Not fired for programmatic
        // (rebind / host) changes.
        std::function<void (bool on)> onToggle;

        int paramIndex() const { return index_; }
        // Re-point at a different bool parameter (a per-band panel rebinds to the
        // selected band). Reads the new value on the next draw.
        void rebind (int paramIndex) { index_ = paramIndex; redraw(); }

        // Override the caption drawn to the right (default: the parameter's display
        // name). Lets an editor use a short label (e.g. "Bypass" / "Listen") instead of
        // the full "Band 1 Bypass" param name. Empty = use desc.name.
        void setCaption (std::string caption) { caption_ = std::move (caption); redraw(); }

    private:
        bool isOn() const { return store_.value (index_) > 0.5f; }

        factory_params::ParamStore& store_;
        int index_;
        const Theme& theme_;
        std::string caption_; // empty == use desc.name
    };
}
