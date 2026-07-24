//
// DeqEditor.cpp — the Visage port of the JUCE DynamicEqAudioProcessorEditor (see
// DeqEditor.h).
//
#include "DeqEditor.h"

#include "factory_ui_visage/Fonts.h"
#include "factory_ui_visage/Chrome.h"

#include <algorithm>
#include <cmath>

namespace deq_ui
{
    DeqEditor::DeqEditor (const factory_ui_visage::Theme& theme, factory_params::ParamStore& store,
                          DeqFeed& feed, DeqPresetModel& presets)
        : theme_ (theme), store_ (store), feed_ (feed), presets_ (presets)
    {
        using namespace factory_ui_visage;

        preset_ = std::make_unique<PresetSelectorView> (theme_);
        preset_->requestDropdown = [this] (std::vector<Dropdown::Item> items, int sel,
                                            visage::Frame* anchor, std::function<void (int)> onSel)
        { presentDropdown (std::move (items), sel, anchor, std::move (onSel)); };
        preset_->onChange = [this] (int itemRow) { loadPreset (itemRow); };
        addChild (*preset_);

        const int bypassIx = store_.indexOf ("bypass");
        bypass_ = std::make_unique<PillToggle> (store_, bypassIx, theme_);
        addChild (*bypass_);

        curve_ = std::make_unique<DeqCurveView> (theme_, store_, feed_);
        curve_->onSelectBand = [this] (int b) { panel_->setBand (b); };
        curve_->onBandEdited = [this] (int b) { if (panel_->band() == b) panel_->refresh(); };
        curve_->onTick = [this] { if (frameTick_) frameTick_(); };
        addChild (*curve_);

        panel_ = std::make_unique<DeqBandPanel> (theme_, store_);
        panel_->setDropdownRequest ([this] (std::vector<Dropdown::Item> items, int sel,
                                            visage::Frame* anchor, std::function<void (int)> onSel)
        { presentDropdown (std::move (items), sel, anchor, std::move (onSel)); });
        addChild (*panel_);

        // Shared overlay LAST so it is frontmost.
        dropdown_ = std::make_unique<Dropdown> (theme_);
        addChild (*dropdown_);

        panel_->setBand (curve_->selectedBand());
        rebuildPresetMenu();
    }

    void DeqEditor::onStateReplaced()
    {
        rebuildPresetMenu();
        panel_->setBand (curve_->selectedBand());
        redrawAll();
    }

    void DeqEditor::rebuildPresetMenu()
    {
        const auto names = presets_.names();
        preset_->setItems (names, presets_.currentIndex());
    }

    void DeqEditor::loadPreset (int itemIndex)
    {
        if (presets_.load (itemIndex))
        {
            rebuildPresetMenu();
            redrawAll();
        }
    }

    void DeqEditor::presentDropdown (std::vector<factory_ui_visage::Dropdown::Item> items, int selected,
                                     visage::Frame* anchor, std::function<void (int)> onSelect)
    {
        if (dropdown_ == nullptr || anchor == nullptr) return;
        const visage::Point a = anchor->positionInWindow();
        const visage::Point self = positionInWindow();
        const float ax = a.x - self.x, ay = a.y - self.y;
        dropdown_->setBounds (0.0f, 0.0f, width(), height()); // full-cover scrim
        dropdown_->onSelect = [onSelect] (int itemRow) { if (onSelect) onSelect (itemRow); };
        dropdown_->open (std::move (items), selected, ax, ay, (float) anchor->width(), (float) anchor->height());
    }

    void DeqEditor::draw (visage::Canvas& canvas)
    {
        using namespace factory_ui_visage;
        paintBackground (canvas, theme_, 0.0f, 0.0f, width(), height());

        // Title.
        canvas.setColor (visage::Color (theme_.palette.accent));
        canvas.text ("Dynamic EQ", boldFont (S (22.0f)), visage::Font::kLeft,
                     S (16.0f), S (14.0f), S (200.0f), S (28.0f));
    }

    void DeqEditor::resized()
    {
        const float pad = S (16.0f);
        const float x = pad, y = pad;
        const float w = std::max (0.0f, (float) width() - 2.0f * pad);

        // Header row.
        const float headerH = S (28.0f);
        const float bypassW = S (100.0f);
        bypass_->setBounds (x + w - bypassW, y, bypassW, headerH);
        const float titleW = S (150.0f);
        const float presetX = x + titleW + S (8.0f);
        const float presetW = std::max (0.0f, (x + w - bypassW - S (8.0f)) - presetX);
        preset_->setBounds (presetX, y, presetW, headerH);

        // Curve + panel.
        const float bodyY = y + headerH + S (10.0f);
        const float panelH = S (170.0f);
        const float bodyBottom = (float) height() - pad;
        const float panelY = bodyBottom - panelH;
        const float curveH = std::max (0.0f, (panelY - S (12.0f)) - bodyY);

        curve_->setBounds (x, bodyY, w, curveH);
        panel_->setBounds (x, panelY, w, panelH);

        if (dropdown_) dropdown_->setBounds (0.0f, 0.0f, (float) width(), (float) height());
    }
} // namespace deq_ui
