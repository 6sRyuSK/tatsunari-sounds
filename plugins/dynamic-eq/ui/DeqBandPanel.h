#pragma once
//
// plugins/dynamic-eq/ui/DeqBandPanel.h — deq_ui::DeqBandPanel, the per-band control
// panel ported from the JUCE BandControlPanel. It shows the currently-selected band's
// controls (Pro-Q style: edit one band at a time): left = EQ (type / slope / channel /
// freq / gain / Q), right = Dynamics (on / threshold / range / attack / release / knee).
// Selecting another band REBINDS the persistent widgets to that band's parameter indices
// (Knob/PillToggle/ValueSetting::rebind) — the visage counterpart of the JUCE editor
// re-pointing its APVTS attachments. GUI-thread only.
//
#include "factory_ui_visage/Theme.h"
#include "factory_ui_visage/Knob.h"
#include "factory_ui_visage/PillToggle.h"
#include "factory_ui_visage/ValueSetting.h"
#include "factory_ui_visage/Dropdown.h"
#include "factory_params/ParamStore.h"

#include <visage_ui/frame.h>

#include <memory>

namespace deq_ui
{
    class DeqBandPanel : public visage::Frame
    {
    public:
        DeqBandPanel (const factory_ui_visage::Theme& theme, factory_params::ParamStore& store);

        void setBand (int band);
        int  band() const noexcept { return band_; }

        // Redraw every bound control (e.g. after a curve gesture changed this band's
        // freq/gain/Q, so the knobs reflect the new value immediately).
        void refresh();

        // Wire the editor's shared Dropdown overlay into the choice controls.
        void setDropdownRequest (factory_ui_visage::DropdownRequest req);

        void draw (visage::Canvas& canvas) override;
        void resized() override;

    private:
        struct Ix { int byp, lsn, dyn, type, slope, chan, freq, gain, q, thr, rng, atk, rel, knee; };
        Ix indicesFor (int band) const;
        void rebind();
        // Slope (dB/oct) only applies to HP/LP cut bands; dim + disable it otherwise
        // (mirrors the JUCE BandControlPanel::updateSlopeEnablement).
        void updateTypeDependent();

        const factory_ui_visage::Theme& theme_;
        factory_params::ParamStore&     store_;
        int band_ = 0;
        Ix  ix_ {};
        float dividerX_ = -1.0f;

        std::unique_ptr<factory_ui_visage::PillToggle>   bypass_, listen_, dyn_;
        std::unique_ptr<factory_ui_visage::ValueSetting> type_, slope_, chan_;
        std::unique_ptr<factory_ui_visage::Knob>         freq_, gain_, q_, thr_, rng_, atk_, rel_, knee_;
    };
} // namespace deq_ui
