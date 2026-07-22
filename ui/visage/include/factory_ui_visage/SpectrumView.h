#pragma once

#include "factory_ui_visage/Theme.h"
#include "factory_ui_visage/SpectrumModel.h"

#include <functional>

#include <visage_ui/frame.h>

//
// factory_ui_visage::SpectrumView — the spectrum display frame, porting the
// geometry of ui/include/factory_ui/SpectrumDisplay.h onto the visage canvas:
// a LogFreqAxis (20 Hz–20 kHz) × VerticalAxis (topDb…bottomDb), a smoothed-trace
// polyline with a closed area fill, and a peak-hold outline, all in theme colours
// (the palette's band colours are available for multi-trace displays).
//
// It reads its magnitudes from a SpectrumModel it does NOT own. Because the trace
// animates, draw() drives its own continuous tick: it calls onTick() (the owner's
// hook to feed a fresh frame into the model) and then requests another redraw —
// UNLESS frozen, in which case it draws the model's current state once and holds
// still, so screenshots are deterministic (ui_freeze feeds a fixed frame first).
//
namespace factory_ui_visage
{
    class SpectrumView : public visage::Frame
    {
    public:
        SpectrumView (const Theme& theme, SpectrumModel& model, double sampleRate);

        void setSampleRate (double sampleRate) { sampleRate_ = sampleRate; }

        // Freeze stops the animation loop (deterministic capture). The owner feeds a
        // fixed frame into the model before freezing so the held image is stable.
        void setFrozen (bool frozen);
        bool frozen() const { return frozen_; }

        // Called once per animated frame, before drawing, so the owner can advance
        // + feed the synthetic (or real) frame into the model. Not called frozen.
        std::function<void()> onTick;

        void draw (visage::Canvas& canvas) override;

    private:
        const Theme& theme_;
        SpectrumModel& model_;
        double sampleRate_;
        bool frozen_ = false;
    };
}
