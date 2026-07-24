#pragma once

#include "factory_ui_visage/Theme.h"
#include "factory_params/ParamStore.h"

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

        int paramIndex() const { return index_; }
        // Re-point at a different bool parameter (a per-band panel rebinds to the
        // selected band). Reads the new value on the next draw.
        void rebind (int paramIndex) { index_ = paramIndex; redraw(); }

    private:
        bool isOn() const { return store_.value (index_) > 0.5f; }

        factory_params::ParamStore& store_;
        int index_;
        const Theme& theme_;
    };
}
