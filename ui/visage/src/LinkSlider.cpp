#include "factory_ui_visage/LinkSlider.h"
#include "factory_ui_visage/Fonts.h"
#include "factory_params/Text.h"

#include <algorithm>
#include <utility>

namespace factory_ui_visage
{
    LinkSlider::LinkSlider (factory_params::ParamStore& store, int paramIndex, const Theme& theme,
                            std::string caption, int decimals)
        : store_ (store), index_ (paramIndex), theme_ (theme), caption_ (std::move (caption)),
          hasGlyph_ (false), decimals_ (decimals),
          range_ (factory_params::makeRange (store.desc (paramIndex)))
    {
    }

    LinkSlider::LinkSlider (factory_params::ParamStore& store, int paramIndex, const Theme& theme,
                            std::string caption, icons::Glyph glyph, int decimals)
        : store_ (store), index_ (paramIndex), theme_ (theme), caption_ (std::move (caption)),
          hasGlyph_ (true), glyph_ (std::move (glyph)), decimals_ (decimals),
          range_ (factory_params::makeRange (store.desc (paramIndex)))
    {
    }

    LinkSlider::Layout LinkSlider::computeLayout() const
    {
        const LinkSliderMetrics& m = theme_.linkSlider;
        Layout L;
        float x = m.paddingX;
        const float innerW = std::max (0.0f, width() - 2.0f * m.paddingX);
        float remaining = innerW;
        const float y = 0.0f;
        const float h = height();

        if (hasGlyph_)
        {
            L.icon = { x, y, m.glyphSize, h };
            x += m.glyphSize + 6.0f;
            remaining -= m.glyphSize + 6.0f;
        }
        const float captionW = captionColumnPx_ > 0.0f ? captionColumnPx_ : m.captionColumn;
        L.caption = { x, y, captionW, h };
        x += captionW;
        remaining -= captionW;

        // Value column on the right; 6 px gap before the track.
        L.value = { m.paddingX + innerW - m.valueColumn, y, m.valueColumn, h };
        remaining -= m.valueColumn + 6.0f;

        L.track = { x, y, std::max (0.0f, remaining), h };
        return L;
    }

    float LinkSlider::currentNorm() const
    {
        return factory_params::convertTo0to1 (range_, store_.value (index_));
    }

    void LinkSlider::writeNorm (float norm)
    {
        norm = std::clamp (norm, 0.0f, 1.0f);
        store_.setFromUi (index_, factory_params::convertFrom0to1 (range_, norm));
        redraw();
    }

    void LinkSlider::draw (visage::Canvas& canvas)
    {
        const LinkSliderMetrics& m = theme_.linkSlider;
        const Palette& p = theme_.palette;
        const factory_params::ParamDesc& desc = store_.desc (index_);
        const float w = width();
        const float h = height();

        // White card + hairline.
        canvas.setColor (visage::Color (p.panel));
        canvas.roundedRectangle (0.0f, 0.0f, w, h, m.cornerRadius);
        canvas.setColor (visage::Color (p.track));
        canvas.roundedRectangleBorder (0.5f, 0.5f, w - 1.0f, h - 1.0f, m.cornerRadius, 1.0f);

        const Layout L = computeLayout();

        if (hasGlyph_)
        {
            canvas.setColor (visage::Color (p.textSecondary));
            icons::paintGlyph (canvas, glyph_, L.icon.x, (h - L.icon.w) * 0.5f, L.icon.w, L.icon.w);
        }

        canvas.setColor (visage::Color (p.textSecondary));
        canvas.text (caption_, boldFont (theme_.font.caption), visage::Font::kLeft,
                     L.caption.x, 0.0f, L.caption.w, h);

        // Value readout, spaces stripped for the demo's tight "100%" / "-24.0dB"
        // look (v2.1.0 RsLinkSlider uses a no-space suffix for the same effect).
        std::string valueText = factory_params::formatValue (desc, store_.value (index_), decimals_);
        valueText.erase (std::remove (valueText.begin(), valueText.end(), ' '), valueText.end());
        canvas.setColor (visage::Color (p.accent));
        canvas.text (valueText, boldFont (theme_.font.callout), visage::Font::kRight, L.value.x, 0.0f, L.value.w, h);

        // Track + coral fill.
        const float trackY = L.track.y + (h - m.trackHeight) * 0.5f;
        canvas.setColor (visage::Color (p.track));
        canvas.roundedRectangle (L.track.x, trackY, L.track.w, m.trackHeight, m.trackCorner);
        const float prop = currentNorm();
        if (prop > 0.0f)
        {
            canvas.setColor (visage::Brush::horizontal (visage::Color (p.accentDim),
                                                        visage::Color (p.accent)));
            canvas.roundedRectangle (L.track.x, trackY, L.track.w * prop, m.trackHeight, m.trackCorner);
        }
    }

    void LinkSlider::mouseDown (const visage::MouseEvent& e)
    {
        // Double-click the value read-out opens the direct text entry; a double-click
        // ANYWHERE ELSE (or an alt-click) restores the default — matching the JUCE
        // RsLinkSlider, where the value-area double-click edits and everything else
        // falls back to the slider's double-click-to-default. A single click still
        // drags from anywhere (the value read-out included).
        if (e.repeatClickCount() >= 2)
        {
            const Layout L = computeLayout();
            const Rect& v = L.value;
            const bool inValue = e.position.x >= v.x && e.position.x < v.x + v.w
                              && e.position.y >= v.y && e.position.y < v.y + v.h;
            if (inValue && requestValueEntry) { openValueEntry(); return; }
        }

        // Alt-click OR double-click (elsewhere) restores the default (round-3 fix 5:
        // alt-click reset on MIX/OUT/STEREO LINK too). A single click is repeat count
        // 1 in visage (double-click is 2), so the double-click threshold must be >= 2
        // — otherwise every press resets instead of dragging.
        if (e.isAltDown() || e.repeatClickCount() >= 2)
        {
            store_.beginGesture (index_);
            store_.setFromUi (index_, store_.desc (index_).defaultValue);
            store_.endGesture (index_);
            dragging_ = false;
            redraw();
            return;
        }

        dragging_ = true;
        dragStartNorm_ = currentNorm();
        dragStartPos_ = e.position; // frame-local anchor (not a move delta)
        store_.beginGesture (index_);
    }

    void LinkSlider::mouseDrag (const visage::MouseEvent& e)
    {
        if (! dragging_)
            return;
        const visage::Point pos = e.position; // frame-local, so dx/dy are true deltas
        const float span = std::max (1.0f, computeLayout().track.w);
        const float dx = pos.x - dragStartPos_.x;
        const float dy = dragStartPos_.y - pos.y; // up = increase
        writeNorm (dragStartNorm_ + (dx + dy) / span);
    }

    void LinkSlider::mouseUp (const visage::MouseEvent&)
    {
        if (dragging_)
        {
            dragging_ = false;
            store_.endGesture (index_);
        }
    }

    void LinkSlider::openValueEntry()
    {
        if (! requestValueEntry) return;
        const factory_params::ParamDesc& desc = store_.desc (index_);
        std::string disp = factory_params::formatValue (desc, store_.value (index_), decimals_);
        disp.erase (std::remove (disp.begin(), disp.end(), ' '), disp.end()); // match the drawn read-out

        const Layout L = computeLayout();
        const visage::Point o = positionInWindow();
        ValueEntryRequest req;
        req.x = o.x + L.value.x; req.y = o.y + L.value.y; req.w = L.value.w; req.h = L.value.h;
        req.prefill = stripLeadingNumber (disp);
        req.fontPx  = theme_.font.callout; // the value read-out font (RS: 12 px)
        req.commit  = [this] (const std::string& t) { commitValueEntry (t); };
        requestValueEntry (req);
    }

    void LinkSlider::commitValueEntry (const std::string& text)
    {
        const factory_params::ParamDesc& desc = store_.desc (index_);
        float real = 0.0f; // JUCE getValueFromText("") == 0 -> range-clamped by setFromUi
        factory_params::parseValue (desc, text, real);
        store_.beginGesture (index_);
        store_.setFromUi (index_, real); // snapToLegalValue clamps + snaps to the range
        store_.endGesture (index_);
        redraw();
    }
}
