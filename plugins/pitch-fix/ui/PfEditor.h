#pragma once
//
// plugins/pitch-fix/ui/PfEditor.h — pf_ui::PfEditor, the JUCE-free Visage editor
// of Pitch TatFixer. DELIBERATELY SIMPLE (the product is in its DSP phase): it
// reuses the shared factory_ui_visage widgets verbatim — knobs for every
// continuous parameter, a dropdown row for Key, segment strips for Scale and
// Buffer, the shared preset selector — plus one status badge that shows the
// REPORTED LATENCY (samples + ms, so the user can dial a track delay in the DAW)
// and the live detected → target pitch as plain text. No visualizer yet.
//
// Fixed-size editor (920×560 design px). Everything binds to the ParamStore by
// id via the UI gesture path; the status badge reads the PfUiFeed atomics.
//
#include "PfModels.h"

#include "factory_ui_visage/Theme.h"
#include "factory_ui_visage/Knob.h"
#include "factory_ui_visage/Segmented.h"
#include "factory_ui_visage/ValueSetting.h"
#include "factory_ui_visage/LinkSlider.h"
#include "factory_ui_visage/PresetSelectorView.h"
#include "factory_ui_visage/Dropdown.h"
#include "factory_ui_visage/ValueEntry.h"

#include "factory_params/ParamStore.h"

#include <visage_ui/frame.h>

#include <functional>
#include <memory>
#include <vector>

namespace pf_ui
{
    // The latency / pitch read-out card. Self-driving (redraws itself every
    // frame — the ONLY animated frame in this editor, so the rest of the chrome
    // stays cost-free per the dirty-region discipline); also the editor's
    // once-per-frame tick hook (the shell pumps its inactive-edit flush here).
    class PfStatusBadge : public visage::Frame
    {
    public:
        PfStatusBadge (const factory_ui_visage::Theme& theme, const PfUiFeed& feed)
            : theme_ (theme), feed_ (feed) {}

        std::function<void()> onTick;   // fired once per drawn frame

        void draw (visage::Canvas& canvas) override;

    private:
        const factory_ui_visage::Theme& theme_;
        PfUiFeed feed_;
    };

    class PfEditor : public visage::Frame
    {
    public:
        static constexpr int kDesignW = 920;
        static constexpr int kDesignH = 560;

        PfEditor (const factory_ui_visage::Theme& theme,
                  factory_params::ParamStore& store,
                  const PfUiFeed& feed,
                  PfPresetModel& presets);
        ~PfEditor() override;

        // Host state replaced (state load): rebuild the preset selector, drop any
        // in-flight overlay edit, repaint everything.
        void onStateReplaced();

        // Once-per-frame hook (rides the status badge's self-redraw).
        void setFrameTick (std::function<void()> fn);

        void draw (visage::Canvas& canvas) override;
        void resized() override;

    private:
        void presentDropdown (std::vector<factory_ui_visage::Dropdown::Item> items,
                              int selected, visage::Frame* anchor,
                              std::function<void (int)> onSelect);
        void openValueEntry (const factory_ui_visage::ValueEntryRequest& req);
        void rebuildPresetMenu();

        float k() const;                 // uniform design scale
        float S (float v) const { return v * k(); }

        const factory_ui_visage::Theme& theme_;
        factory_params::ParamStore&     store_;
        PfPresetModel&                  presets_;

        // Big correction row.
        std::unique_ptr<factory_ui_visage::Knob> amount_, retune_, glide_, tolerance_, hysteresis_;
        // Detector / output row.
        std::unique_ptr<factory_ui_visage::Knob> minPitch_, maxPitch_, threshold_, mix_, out_;
        // Musical context.
        std::unique_ptr<factory_ui_visage::ValueSetting> key_;
        std::unique_ptr<factory_ui_visage::Segmented>    scale_;
        std::unique_ptr<factory_ui_visage::LinkSlider>   a4_;
        // Performance.
        std::unique_ptr<factory_ui_visage::Segmented>    buffer_;
        std::unique_ptr<PfStatusBadge>                   status_;
        // Header.
        std::unique_ptr<factory_ui_visage::PresetSelectorView> presetView_;
        // Shared overlays (added last == frontmost).
        std::unique_ptr<factory_ui_visage::Dropdown>   dropdown_;
        std::unique_ptr<factory_ui_visage::ValueEntry> valueEntry_;
    };
} // namespace pf_ui
