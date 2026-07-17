#pragma once

#include "factory_ui_visage/Theme.h"
#include "factory_params/ParamStore.h"

#include <visage_ui/frame.h>

//
// factory_ui_visage::Segmented — a horizontal segment strip bound to a Choice
// parameter, ported from rs::RsSegmented (geometry/behaviour only): a rounded
// track with an accent "pill" on the active segment, the segment labels taken
// from the parameter's choices. Binds to a factory_params::ParamStore by index
// exactly like Knob/PillToggle (reads store.value, writes via the UI gesture
// path). The active-pill move is instant (deterministic screenshots).
//
namespace factory_ui_visage
{
    class Segmented : public visage::Frame
    {
    public:
        Segmented (factory_params::ParamStore& store, int paramIndex, const Theme& theme);

        void draw (visage::Canvas& canvas) override;
        void mouseDown (const visage::MouseEvent& e) override;

        int paramIndex() const { return index_; }

    private:
        int  segmentCount() const;
        int  currentIndex() const;

        factory_params::ParamStore& store_;
        int index_;
        const Theme& theme_;
    };
}
