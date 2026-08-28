//
// DeqEditor.cpp — the Visage port of the JUCE DynamicEqAudioProcessorEditor (see
// DeqEditor.h).
//
#include "DeqEditor.h"

#include "factory_ui_visage/Fonts.h"
#include "factory_ui_visage/Chrome.h"

#include <algorithm>
#include <cmath>
#include <string>

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

        bypassIx_ = store_.indexOf ("bypass");
        bypass_ = std::make_unique<PillToggle> (store_, bypassIx_, theme_);
        addChild (*bypass_);

        // Map each parameter to the band it belongs to ("b<N>_<suffix>"), so the host-change
        // sweep can refresh the band panel only when the band it is SHOWING moved.
        bandOfParam_.assign ((std::size_t) store_.size(), -1);
        for (int i = 0; i < store_.size(); ++i)
        {
            const std::string& id = store_.desc (i).id;
            if (id.empty() || id[0] != 'b') continue;
            std::size_t p = 1;
            while (p < id.size() && id[p] >= '0' && id[p] <= '9') ++p;
            if (p == 1 || p >= id.size() || id[p] != '_') continue;
            bandOfParam_[(std::size_t) i] = std::stoi (id.substr (1, p - 1));
        }

        curve_ = std::make_unique<DeqCurveView> (theme_, store_, feed_);
        curve_->onSelectBand = [this] (int b) { panel_->setBand (b); };
        curve_->onBandEdited = [this] (int b) { if (panel_->band() == b) panel_->refresh(); };
        curve_->onTick = [this] { pumpHostChanges(); if (frameTick_) frameTick_(); };
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

    // Redraw every widget whose parameter changed since the last tick — the sweep that makes
    // the editor follow edits it did not make itself (automation playback, the host's generic
    // UI, host undo, MIDI learn: all setFromHost, which touches no widget). Runs on the
    // curve's ~30 Hz analyser tick. The UI's own edits bump the same epochs and land here
    // too, but redraw() only marks a frame dirty, so the extra mark costs nothing. The curve
    // view self-redraws every frame and needs no nudge.
    void DeqEditor::pumpHostChanges()
    {
        const int shown = panel_ != nullptr ? panel_->band() : -1;
        bool panelChanged = false;
        sweeper_.sweep (store_, [&] (int i)
        {
            if (i == bypassIx_ && bypass_ != nullptr) { bypass_->redraw(); return; }
            if (i >= 0 && (std::size_t) i < bandOfParam_.size()
                && bandOfParam_[(std::size_t) i] == shown)
                panelChanged = true;
        });
        // refresh() redraws the band's widgets AND re-evaluates the slope enablement, so a
        // host-driven band-type change dims / undims Slope exactly as a UI-driven one does.
        if (panelChanged && panel_ != nullptr)
            panel_->refresh();
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

    void DeqEditor::selectBand (int band)
    {
        if (band < 0 || band >= DeqCurveView::kNumBands) return;
        curve_->setSelectedBand (band);
        panel_->setBand (band);
        redrawAll();
    }

    bool DeqEditor::openNamedDropdown (const std::string& name)
    {
        if (name == "preset" && preset_) { preset_->openMenu(); return true; }
        return panel_ && panel_->openNamedDropdown (name);
    }

    bool DeqEditor::widgetRectInWindow (const std::string& key, float& x, float& y, float& w, float& h) const
    {
        auto rectOf = [&] (const visage::Frame* frame)
        {
            if (frame == nullptr) return false;
            const auto p = frame->positionInWindow();
            x = p.x; y = p.y; w = frame->width(); h = frame->height();
            return true;
        };
        if (key == "preset") return rectOf (preset_.get());
        if (key == "bypass") return rectOf (bypass_.get());
        if (key == "curve") return rectOf (curve_.get());
        if (key == "panel") return rectOf (panel_.get());
        if (key == "plot") return curve_ && curve_->plotRectInWindow (x, y, w, h);

        // "b<n>_node" -> the curve handle for band n. Parsed by hand rather than
        // with std::stoi + catch: emcc builds this TU with exception CATCHING off,
        // so a throwing stoi would abort the module instead of falling through.
        const auto marker = key.find ("_node");
        if (key.rfind ("b", 0) == 0 && marker != std::string::npos && marker > 1)
        {
            int band = 0;
            bool digits = true;
            for (std::size_t i = 1; i < marker; ++i)
            {
                const char c = key[i];
                if (c < '0' || c > '9') { digits = false; break; }
                band = band * 10 + (c - '0');
            }
            float cx = 0.0f, cy = 0.0f;
            if (digits && curve_ && curve_->nodeCentreInWindow (band, cx, cy))
            {
                x = cx - 10.0f; y = cy - 10.0f; w = 20.0f; h = 20.0f;
                return true;
            }
        }
        return panel_ && panel_->widgetRectInWindow (key, x, y, w, h);
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
