#pragma once

#include "factory_ui_visage/Theme.h"
#include "factory_ui_visage/Knob.h"
#include "factory_ui_visage/PillToggle.h"
#include "factory_ui_visage/Segmented.h"
#include "factory_ui_visage/IconButton.h"
#include "factory_ui_visage/ValueSetting.h"
#include "factory_ui_visage/LinkSlider.h"
#include "factory_ui_visage/PresetSelectorView.h"
#include "factory_ui_visage/Dropdown.h"
#include "factory_ui_visage/SpectrumModel.h"
#include "factory_ui_visage/SpectrumView.h"
#include "factory_params/ParamStore.h"

#include <visage_ui/frame.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

//
// GalleryFrame — the daily-development widget gallery. It owns a real
// factory_params::ParamStore (mock host: nothing drains the host-write queue) and
// the single Theme every child reads (hot reload mutates it in place). Two cards:
//   * card 1 — the P2a primitives (Knobs + PillToggles).
//   * card 2 — the P2b widget set (PresetSelectorView, Segmented, IconButtons,
//     ValueSetting, LinkSlider) + an animated SpectrumView fed by a deterministic
//     synthetic generator (pink-ish slope + two moving peaks; frozen by ui_freeze).
// It hosts the shared Dropdown overlay for the controls that pop a menu.
//
class GalleryFrame : public visage::Frame
{
public:
    GalleryFrame();

    void draw (visage::Canvas& canvas) override;
    void resized() override;
    void mouseDown (const visage::MouseEvent& e) override; // records last hit (harness probe)

    // Last mouse-down position visage delivered, in window px — lets the driver
    // calibrate its page->window coordinate mapping (see tools/ui-dev README).
    float lastMouseX() const { return lastMouseX_; }
    float lastMouseY() const { return lastMouseY_; }

    // --- bridge surface (called from the JS <-> WASM bridge) ------------------
    factory_params::ParamStore& store() { return store_; }
    const factory_ui_visage::Theme& theme() const { return theme_; }

    bool reloadTheme (const std::string& jsonText, std::string& error);

    // Freeze/unfreeze the animation. Freezing stops the SpectrumView loop and
    // injects a fixed synthetic frame so the held image is deterministic.
    void setFrozen (bool frozen);
    bool frozen() const { return frozen_; }

    // Inject a FIXED synthetic spectrum frame at `phase` and converge the model so
    // the result is deterministic (repeated calls give identical pixels). Used by
    // ui_feed_spectrum for stable frozen screenshots.
    void feedSpectrum (double phase);

    // Open the shared Dropdown for a named control (0 = preset selector,
    // 1 = value setting) at its own location — for deterministic dropdown capture.
    bool openNamedDropdown (int which);

    // Dropdown query surface (window px), so the driver can aim at rows.
    bool dropdownOpen() const { return dropdown_ && dropdown_->isOpen(); }
    int  presetIndex() const { return presetSelector_ ? presetSelector_->selectedIndex() : -1; }

    // Rect (window px) of a control, keyed by param id ("mode"/"link"/…) or a
    // special name ("preset"/"spectrum"/"valueSetting"). Returns false if unknown.
    bool widgetRectInWindow (const std::string& key, float& x, float& y, float& w, float& h) const;

    // Centre of the widget bound to `paramIndex`, in window px (back-compat with
    // the P2a driver's ui_widget_x/ui_widget_y).
    bool widgetCentreInWindow (int paramIndex, float& outX, float& outY) const;

    // The shared Dropdown (for the bridge's row-position queries).
    factory_ui_visage::Dropdown* dropdown() { return dropdown_.get(); }

private:
    static std::vector<factory_params::ParamDesc> buildParams();

    void presentDropdown (std::vector<factory_ui_visage::Dropdown::Item> items, int selected,
                          visage::Frame* anchor, std::function<void (int)> onSelect);
    void spectrumTick();               // advance + feed one animated frame
    void buildSynthFrame (std::vector<float>& out, double phase) const;
    visage::Frame* frameForParam (int paramIndex) const; // param-bound widget lookup

    factory_params::ParamStore store_;
    factory_ui_visage::Theme theme_;
    double sampleRate_ = 48000.0;

    std::vector<std::unique_ptr<factory_ui_visage::Knob>> knobs_;
    std::vector<std::unique_ptr<factory_ui_visage::PillToggle>> toggles_;
    std::unique_ptr<factory_ui_visage::PresetSelectorView> presetSelector_;
    std::unique_ptr<factory_ui_visage::Segmented> segmented_;
    std::vector<std::unique_ptr<factory_ui_visage::IconButton>> iconButtons_;
    std::unique_ptr<factory_ui_visage::ValueSetting> valueSetting_;
    std::unique_ptr<factory_ui_visage::LinkSlider> linkSlider_;
    std::unique_ptr<factory_ui_visage::SpectrumView> spectrumView_;
    std::unique_ptr<factory_ui_visage::Dropdown> dropdown_;

    // Spectrum model + synthetic-generator state.
    factory_ui_visage::SpectrumModel spectrumModel_;
    factory_params::ChangeSweeper sweeper_;
    double animPhase_ = 0.0;
    float  spectrumPeakGain_ = 0.28f; // driven by the "depth" param via ChangeSweeper
    std::vector<float> synthScratch_; // preallocated synthetic-frame buffer (no per-frame alloc)

    bool frozen_ = false;
    float lastMouseX_ = -1.0f, lastMouseY_ = -1.0f;

    // Card rectangles (frame-local), computed in resized().
    float card1X_ = 0, card1Y_ = 0, card1W_ = 0, card1H_ = 0;
    float card2X_ = 0, card2Y_ = 0, card2W_ = 0, card2H_ = 0;
};
