#pragma once

#include "factory_ui_visage/Theme.h"
#include "factory_ui_visage/Knob.h"
#include "factory_ui_visage/PillToggle.h"
#include "factory_params/ParamStore.h"

#include <visage_ui/frame.h>

#include <memory>
#include <string>
#include <vector>

//
// GalleryFrame — the daily-development widget gallery. It instantiates a real
// factory_params::ParamStore (mock host: nothing drains the host-write queue),
// binds several Knobs and PillToggles to it, and paints the shared background +
// card chrome. It owns the single Theme instance that every child widget reads;
// hot reload mutates that Theme in place and redraws.
//
class GalleryFrame : public visage::Frame
{
public:
    GalleryFrame();

    void draw (visage::Canvas& canvas) override;
    void resized() override;

    // --- bridge surface (called from the JS <-> WASM bridge) ------------------
    factory_params::ParamStore& store() { return store_; }
    const factory_ui_visage::Theme& theme() const { return theme_; }

    // Re-apply a theme from JSON at runtime (THE hot-reload hook). Returns false
    // and fills `error` on malformed input; the previous theme is kept on failure.
    bool reloadTheme (const std::string& jsonText, std::string& error);

    // Stops continuous animation for deterministic screenshots. The P2a widgets
    // are event-driven (static when idle), so this is currently a guaranteed-safe
    // no-op; it exists for deterministic capture and forward-compat with the
    // animated P2b widgets (Spectrum view).
    void setFrozen (bool frozen) { frozen_ = frozen; }
    bool frozen() const { return frozen_; }

    // Centre of the widget bound to `paramIndex`, in window pixels — lets the
    // Playwright driver aim real mouse events at a specific knob/toggle.
    bool widgetCentreInWindow (int paramIndex, float& outX, float& outY) const;

private:
    static std::vector<factory_params::ParamDesc> buildParams();

    factory_params::ParamStore store_;
    factory_ui_visage::Theme theme_;

    std::vector<std::unique_ptr<factory_ui_visage::Knob>> knobs_;
    std::vector<std::unique_ptr<factory_ui_visage::PillToggle>> toggles_;

    bool frozen_ = false;

    // Card rectangle (frame-local), computed in resized(), painted in draw().
    float cardX_ = 0.0f, cardY_ = 0.0f, cardW_ = 0.0f, cardH_ = 0.0f;
};
