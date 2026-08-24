#pragma once
//
// plugins/tn-equalizer/ui/DeqEditor.h — deq_ui::DeqEditor, the JUCE-free Visage editor of
// TN Equalizer, ported from the JUCE DynamicEqAudioProcessorEditor. It composes:
// a header row (title + shared PresetSelectorView + global Bypass pill), the big
// DeqCurveView analyser/EQ centrepiece, and the DeqBandPanel for the selected band, plus
// the shared Dropdown overlay the band panel's choice controls use.
//
// It binds every control to a factory_params::ParamStore by string id and reads the
// analyser/live-gain via a deq_ui::DeqFeed. Resizable: the editor always lays out at the
// fixed 740×520 design (k() == 1); the CLAP shell uniform-zooms the whole window. (v1 has
// no in-editor resize grip, so a host without a window resize edge — Logic's AU — opens at
// the default size; host-edge resize works everywhere else.)
//
#include "DeqModels.h"
#include "DeqCurveView.h"
#include "DeqBandPanel.h"

#include "factory_ui_visage/Theme.h"
#include "factory_ui_visage/PillToggle.h"
#include "factory_ui_visage/PresetSelectorView.h"
#include "factory_ui_visage/Dropdown.h"
#include "factory_params/ParamStore.h"

#include <visage_ui/frame.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace deq_ui
{
    class DeqEditor : public visage::Frame
    {
    public:
        static constexpr int kDesignW = 740;
        static constexpr int kDesignH = 520;

        DeqEditor (const factory_ui_visage::Theme& theme, factory_params::ParamStore& store,
                   DeqFeed& feed, DeqPresetModel& presets);

        // Host-window resize relay (WINDOW px, aspect-snapped by the shell). Reserved for a
        // future in-editor resize grip; unwired in v1.
        std::function<void (float w, float h)> onResizeRequest;
        void setWindowScale (float s) noexcept { windowScale_ = s > 0.0f ? s : 1.0f; }

        // Once-per-frame hook (rides the curve's self-redraw); the shell pumps the
        // inactive-edit param flush here.
        void setFrameTick (std::function<void()> fn) { frameTick_ = std::move (fn); }

        // Wholesale state replacement (host state.load / preset load): rebuild the preset
        // selector + redraw everything.
        void onStateReplaced();

        DeqCurveView& curve() noexcept { return *curve_; }

        // Harness / driver surface.
        factory_params::ParamStore& store() noexcept { return store_; }
        const factory_ui_visage::Theme& theme() const noexcept { return theme_; }
        factory_ui_visage::Dropdown* dropdown() noexcept { return dropdown_.get(); }
        int presetIndex() const { return presets_.currentIndex(); }
        int selectedBand() const noexcept { return curve_ ? curve_->selectedBand() : -1; }
        void selectBand (int band);
        bool openNamedDropdown (const std::string& name); // "type"|"slope"|"chan"|"preset"
        bool widgetRectInWindow (const std::string& key, float& x, float& y, float& w, float& h) const;

        void draw (visage::Canvas& canvas) override;
        void resized() override;

    private:
        void presentDropdown (std::vector<factory_ui_visage::Dropdown::Item> items, int selected,
                              visage::Frame* anchor, std::function<void (int)> onSelect);
        void rebuildPresetMenu();
        void loadPreset (int itemIndex);
        void pumpHostChanges();

        float k() const { return height() > 0.0f ? (float) height() / (float) kDesignH : 1.0f; }
        float S (float v) const { return v * k(); }

        const factory_ui_visage::Theme& theme_;
        factory_params::ParamStore&     store_;
        DeqFeed&                        feed_;
        DeqPresetModel&                 presets_;

        std::unique_ptr<factory_ui_visage::PresetSelectorView> preset_;
        std::unique_ptr<factory_ui_visage::PillToggle>         bypass_;
        std::unique_ptr<DeqCurveView>                          curve_;
        std::unique_ptr<DeqBandPanel>                          panel_;
        std::unique_ptr<factory_ui_visage::Dropdown>           dropdown_; // shared overlay (frontmost)

        // Host-driven change observer: a non-consuming per-frame sweep over the store's
        // epochs. Visage only repaints frames that were redraw()n, and every widget except
        // the self-driving curve view is redrawn only by its OWN mouse handling — so without
        // this sweep, automation playback / the host's generic UI / host undo / MIDI-learn
        // would move the curve while the band panel's knobs and the header bypass pill kept
        // showing stale values. (Same mechanism as RsEditor's fix F1.)
        factory_params::ChangeSweeper sweeper_;
        std::vector<int>              bandOfParam_; // param index -> band, or -1
        int                           bypassIx_ = -1;

        float windowScale_ = 1.0f;
        std::function<void()> frameTick_;
    };
} // namespace deq_ui
