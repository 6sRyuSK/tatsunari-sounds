#pragma once

#include "RsTheme.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <functional>
#include <vector>

//
// RsWidgets — the light-weight Component set for the Resonance TatSuppressor
// "new UI" chrome (Phase P1). Every geometry / colour here is transcribed from
// rs-ui-work/demo-analysis.md (SS2 control inventory, SS3.6 icon glyphs, SS6
// reproduction notes). These are pure views: they carry no APVTS knowledge --
// the editor wires SliderAttachment / ButtonAttachment / ComboBoxAttachment
// against the sliders / buttons / (hidden model) combos these expose, so all
// existing parameter IDs and state compatibility are preserved.
//
namespace rs
{
    // ----------------------------------------------------------- icon glyphs
    // Verbatim from demo-analysis SS3.6 (settings/mode icons, viewBox 16) plus a
    // few hand-built header glyphs (undo/redo/copy/caret, viewBox 24) for the
    // controls the mockup omits. Each returns a Path in its native viewBox; use
    // fitGlyph() to place it. SVG "C x1 y1,x2 y2,x y" maps to cubicTo(x1,y1,x2,y2,x,y).
    namespace icons
    {
        struct Glyph { juce::Path path; float box; bool filled; float stroke; };

        inline Glyph delta()  // triangle (DELTA)
        {
            juce::Path p; p.startNewSubPath (8, 3); p.lineTo (13.5f, 13); p.lineTo (2.5f, 13); p.closeSubPath();
            return { p, 16.0f, false, 1.6f };
        }
        inline Glyph sidechain() // down arrow into a baseline (S-CHAIN)
        {
            juce::Path p;
            p.startNewSubPath (8, 2.5f); p.lineTo (8, 9);
            p.startNewSubPath (5, 6); p.lineTo (8, 9.4f); p.lineTo (11, 6);
            p.startNewSubPath (2.5f, 13); p.lineTo (13.5f, 13);
            return { p, 16.0f, false, 1.8f };
        }
        inline Glyph quality() // rising bars (QUALITY)
        {
            juce::Path p;
            p.startNewSubPath (3.5f, 13); p.lineTo (3.5f, 10.5f);
            p.startNewSubPath (8, 13);    p.lineTo (8, 7);
            p.startNewSubPath (12.5f, 13); p.lineTo (12.5f, 4);
            return { p, 16.0f, false, 2.0f };
        }
        inline Glyph channel() // two overlapping circles (CH, M/S)
        {
            juce::Path p;
            p.addEllipse (5.6f - 3.2f, 8 - 3.2f, 6.4f, 6.4f);
            p.addEllipse (10.4f - 3.2f, 8 - 3.2f, 6.4f, 6.4f);
            return { p, 16.0f, false, 1.6f };
        }
        inline Glyph link() // two chain links (STEREO LINK)
        {
            juce::Path p;
            p.addRoundedRectangle (2.0f, 5.5f, 7.5f, 5.0f, 2.5f);
            p.addRoundedRectangle (6.5f, 5.5f, 7.5f, 5.0f, 2.5f);
            return { p, 16.0f, false, 1.6f };
        }
        inline Glyph modeSoft() // smooth bell
        {
            juce::Path p; p.startNewSubPath (2, 11); p.cubicTo (6, 11, 6, 5, 8, 5); p.cubicTo (10, 5, 10, 11, 14, 11);
            return { p, 16.0f, false, 2.0f };
        }
        inline Glyph modeHard() // stepped
        {
            juce::Path p; p.startNewSubPath (2, 11); p.lineTo (6, 11); p.lineTo (6, 5); p.lineTo (10, 5); p.lineTo (10, 11); p.lineTo (14, 11);
            return { p, 16.0f, false, 2.0f };
        }
        inline Glyph listen() // concentric "monitor" dot (SC LISTEN, added)
        {
            juce::Path p;
            p.addEllipse (8 - 4.5f, 8 - 4.5f, 9.0f, 9.0f);
            p.addEllipse (8 - 1.6f, 8 - 1.6f, 3.2f, 3.2f);
            return { p, 16.0f, false, 1.6f };
        }
        // Header additions (viewBox 24).
        inline Glyph undo() // hooked arrow pointing left
        {
            juce::Path p;
            p.startNewSubPath (15, 13); p.lineTo (15, 9); p.lineTo (6, 9);         // hook + shaft
            p.startNewSubPath (9.5f, 6); p.lineTo (6, 9); p.lineTo (9.5f, 12);      // arrowhead
            return { p, 24.0f, false, 2.0f };
        }
        inline Glyph redo() // mirror of undo
        {
            juce::Path p;
            p.startNewSubPath (9, 13); p.lineTo (9, 9); p.lineTo (18, 9);
            p.startNewSubPath (14.5f, 6); p.lineTo (18, 9); p.lineTo (14.5f, 12);
            return { p, 24.0f, false, 2.0f };
        }
        inline Glyph copy() // two overlapping rounded rectangles
        {
            juce::Path p;
            p.addRoundedRectangle (4.0f, 8.0f, 11.0f, 12.0f, 2.0f);  // front
            p.addRoundedRectangle (9.0f, 4.0f, 11.0f, 12.0f, 2.0f);  // back
            return { p, 24.0f, false, 1.8f };
        }
        inline Glyph caret() // small down chevron (preset menu affordance)
        {
            juce::Path p; p.startNewSubPath (8, 10); p.lineTo (12, 14); p.lineTo (16, 10);
            return { p, 24.0f, false, 2.0f };
        }

        // Uniformly scale a glyph's viewBox into `area` (min-dimension fit,
        // centred) and paint it (stroke or fill) in `colour`.
        inline void paintGlyph (juce::Graphics& g, const Glyph& glyph,
                                juce::Rectangle<float> area, juce::Colour colour)
        {
            const float s = juce::jmin (area.getWidth(), area.getHeight()) / glyph.box;
            auto p = glyph.path;
            p.applyTransform (juce::AffineTransform::scale (s)
                                  .translated (area.getCentreX() - glyph.box * s * 0.5f,
                                               area.getCentreY() - glyph.box * s * 0.5f));
            g.setColour (colour);
            if (glyph.filled)
                g.fillPath (p);
            else
                g.strokePath (p, juce::PathStrokeType (glyph.stroke * s, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }

    // --------------------------------------------------------------- RsBrand
    // Header brand: 30x30 gradient logo square + "Resonance TatSuppressor"
    // wordmark (demo H1). Not a control.
    class RsBrand : public juce::Component
    {
    public:
        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            const float sq = 30.0f;
            auto logo = juce::Rectangle<float> (sq, sq).withY (r.getCentreY() - sq * 0.5f).withX (r.getX());
            {
                juce::DropShadow ds (colour::glowPink(), 9, { 0, 4 });
                juce::Path lp; lp.addRoundedRectangle (logo, radius::badge);
                ds.drawForPath (g, lp);
            }
            juce::ColourGradient lg (colour::orange(), logo.getTopLeft(), colour::pink(), logo.getBottomRight(), false);
            g.setGradientFill (lg);
            g.fillRoundedRectangle (logo, radius::badge);

            auto text = r.withTrimmedLeft (sq + 12.0f);
            g.setFont (font (FontKind::Display, 19.0f, 800));
            const juce::String a = "Resonance ", b = "TatSuppressor";
            const float aw = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), a);
            g.setColour (colour::accent());
            g.drawText (a, text, juce::Justification::centredLeft);
            g.setColour (colour::text());
            g.drawText (b, text.withTrimmedLeft (aw), juce::Justification::centredLeft);
        }
    };

    // Leading numeric run of `s` (optional sign, digits, one decimal point,
    // digits) -- the inverse of juce::String::getFloatValue()'s own parse.
    // Used to pre-fill a text-entry overlay with a bare number, stripping
    // whatever unit/suffix the display formatting appended ("62%" -> "62",
    // "2.6 kHz" -> "2.6" -- an nbsp-joined unit is dropped the same way, since
    // anything from the first non-numeric character on is simply discarded).
    inline juce::String stripToLeadingNumber (const juce::String& s)
    {
        const auto t = s.trimStart();
        int i = 0;
        const int n = t.length();
        if (i < n && (t[i] == '-' || t[i] == '+')) ++i;
        while (i < n && t[i] >= '0' && t[i] <= '9') ++i;
        if (i < n && t[i] == '.')
        {
            ++i;
            while (i < n && t[i] >= '0' && t[i] <= '9') ++i;
        }
        return t.substring (0, i);
    }

    // ---------------------------------------------------------------- RsKnob
    // Vertical stack: name (top) / rotary (middle) / value (bottom). The arc &
    // pointer take a per-knob accent (Slider::rotarySliderFillColourId, drawn by
    // RsLookAndFeel); the value read-out is always coral (demo SS2.3/2.4). The
    // editor attaches a SliderAttachment to slider() and sets decimals after.
    //
    // Direct text entry (P3c): double-clicking the value read-out opens a
    // juce::Label overlay over valueArea, pre-filled with the current value
    // (stripToLeadingNumber()'d so the user edits a bare number). It rides the
    // Label's own built-in editor lifecycle -- Enter and focus-loss both COMMIT
    // (Label's default: lossOfFocusDiscardsChanges=false), Esc CANCELS -- so
    // this class adds no bespoke keyboard handling. A committed edit calls
    // slider().setValue (slider().getValueFromText (text), sendNotificationSync),
    // i.e. the same SliderAttachment parser + NormalisableRange clamp that
    // already backs the (invisible) NoTextBox text box, so validation/range
    // clamping is free. An owner that rebinds this knob's attachment to a
    // different parameter without hiding it (NodePanel::setNode(), which keeps
    // the popover open across a node switch) must call closeValueEditor()
    // first so a stray in-flight edit can never land on the wrong parameter;
    // visibilityChanged()/parentHierarchyChanged() do the same automatically
    // for a knob that is hidden/reparented while its editor is open.
    class RsKnob : public juce::Component
    {
    public:
        RsKnob()
        {
            s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            s.setRotaryParameters (juce::degreesToRadians (225.0f), juce::degreesToRadians (225.0f + 270.0f), true);
            s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            s.onValueChange = [this] { repaint(); };
            addAndMakeVisible (s);

            valueEditor.setEditable (false, true, false); // double-click only; focus loss COMMITS (Label default)
            valueEditor.setJustificationType (juce::Justification::centred);
            valueEditor.setColour (juce::Label::textColourId, colour::accent());
            valueEditor.setColour (juce::Label::backgroundColourId, colour::white());
            valueEditor.setColour (juce::Label::outlineColourId, colour::accent());
            valueEditor.setColour (juce::TextEditor::textColourId, colour::text());
            valueEditor.setColour (juce::TextEditor::backgroundColourId, colour::white());
            valueEditor.setColour (juce::TextEditor::highlightColourId, colour::accent().withAlpha (0.35f));
            valueEditor.setColour (juce::TextEditor::focusedOutlineColourId, colour::accent());
            valueEditor.onEditorHide = [this] { valueEditor.setVisible (false); };
            valueEditor.onTextChange = [this]
            {
                // Only fires on an actually-committed edit (Label dedups a
                // no-op Enter against the pre-fill text, so re-submitting the
                // unedited value never round-trips through setValue at all);
                // the attachment's own parser + range clamp validate the rest.
                s.setValue (s.getValueFromText (valueEditor.getText()), juce::sendNotificationSync);
            };
            addChildComponent (valueEditor);
        }

        juce::Slider& slider() noexcept { return s; }

        void setup (const juce::String& name, juce::Colour accent, bool big, const juce::String& suffix)
        {
            nm = name; bigKnob = big;
            s.setColour (juce::Slider::rotarySliderFillColourId, accent);
            s.setTextValueSuffix (suffix);
            valueEditor.setFont (font (FontKind::Ui, big ? 13.0f : 11.0f, 800));
        }

        void paint (juce::Graphics& g) override
        {
            g.setColour (colour::text());
            g.setFont (font (FontKind::Ui, bigKnob ? 12.0f : 10.0f, 700, 0.02f));
            g.drawText (nm, nameArea, juce::Justification::centred);

            // Live value straight from the slider (respects the editor's post-
            // attachment setSliderDecimals / suffix); spaces stripped for the
            // demo's tight "62%" / "120ms" read-out.
            auto v = s.getTextFromValue (s.getValue()).removeCharacters (" ");
            g.setColour (colour::accent());
            g.setFont (font (FontKind::Ui, bigKnob ? 13.0f : 11.0f, 800));
            g.drawText (v, valueArea, juce::Justification::centred);
        }

        void resized() override
        {
            auto r = getLocalBounds();
            nameArea  = r.removeFromTop (bigKnob ? 16 : 14);
            valueArea = r.removeFromBottom (bigKnob ? 17 : 14);
            const int d = juce::jmin (r.getWidth(), r.getHeight());
            s.setBounds (juce::Rectangle<int> (d, d).withCentre (r.getCentre()));
            valueEditor.setBounds (valueArea);
        }

        // Double-click the value read-out (only) opens the text editor; the
        // rotary itself covers none of valueArea (see resized() above) so this
        // never competes with dragging the knob.
        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            if (valueArea.contains (e.getPosition()))
                openValueEditor();
        }

        // Force-CANCEL (never commit) any in-progress edit. Public so an owner
        // that is about to rebind this knob's attachment to a different
        // parameter can close it first (see class doc).
        void closeValueEditor() { valueEditor.hideEditor (true); }

        void visibilityChanged() override      { closeValueEditor(); }
        void parentHierarchyChanged() override { closeValueEditor(); }

    private:
        void openValueEditor()
        {
            valueEditor.setBounds (valueArea);
            valueEditor.setText (stripToLeadingNumber (s.getTextFromValue (s.getValue())), juce::dontSendNotification);
            valueEditor.setVisible (true);
            valueEditor.toFront (true);
            valueEditor.showEditor();
        }

        juce::Slider s;
        juce::Label  valueEditor;
        juce::String nm;
        bool bigKnob = true;
        juce::Rectangle<int> nameArea, valueArea;
    };

    // ----------------------------------------------------------- RsPillToggle
    // A demo pill switch (SS2.1/2.5). Draws only the pill, right-aligned & v-
    // centred within its bounds, so the editor can give it the whole settings
    // cell (whole-cell click toggles) and paint the icon/caption behind it, or a
    // tight pill-only box (header Bypass). ButtonAttachment-wireable.
    class RsPillToggle : public juce::Button
    {
    public:
        RsPillToggle() : juce::Button ({})
        {
            setClickingTogglesState (true);
        }

        void setOnColour (juce::Colour c) { onColour = c; }
        void setPillSize (int w, int h) { pillW = w; pillH = h; }
        void setPillRightInset (int px) { rightInset = px; } // inset the pill from the cell's right edge so it sits inside the card

        void paintButton (juce::Graphics& g, bool highlighted, bool /*down*/) override
        {
            auto r = getLocalBounds().toFloat();
            auto pill = juce::Rectangle<float> ((float) pillW, (float) pillH)
                            .withY (r.getCentreY() - pillH * 0.5f)
                            .withRightX (r.getRight() - (float) rightInset);
            const bool on = getToggleState();
            g.setColour (on ? onColour : colour::toggleOffBg());
            g.fillRoundedRectangle (pill, pillH * 0.5f);
            if (highlighted)
            {
                g.setColour (juce::Colours::white.withAlpha (0.12f));
                g.fillRoundedRectangle (pill, pillH * 0.5f);
            }
            const float knobD = (float) pillH - 4.0f;
            const float kx = on ? pill.getRight() - knobD - 2.0f : pill.getX() + 2.0f;
            g.setColour (colour::white());
            g.fillEllipse (kx, pill.getY() + 2.0f, knobD, knobD);
        }

    private:
        juce::Colour onColour { colour::teal() };
        int pillW = 42, pillH = 22;
        int rightInset = 0;
    };

    // ------------------------------------------------------------ RsSegmented
    // 2-3 segment selector (MODE Soft/Hard, or the header A|B). Backed by a
    // hidden juce::ComboBox that is the value model + ComboBoxAttachment target
    // (for param-bound use like MODE) -- for manual use (A|B) leave it
    // unattached and drive via onSelect + setSelectedIndex. Optional per-segment
    // glyph (MODE bells).
    class RsSegmented : public juce::Component
    {
    public:
        RsSegmented()
        {
            addChildComponent (model); // invisible value model; never painted
            model.onChange = [this] { repaint(); };
        }

        // Populate segments (manual addItemList -- attachments don't populate in
        // this JUCE, matching the existing plugin's combo pattern).
        void setSegments (const juce::StringArray& labels, std::vector<icons::Glyph> segGlyphs = {})
        {
            texts = labels;
            glyphs = std::move (segGlyphs);
            model.clear (juce::dontSendNotification);
            model.addItemList (labels, 1);
        }

        juce::ComboBox& comboBox() noexcept { return model; }        // ComboBoxAttachment target
        void setAccent (juce::Colour c) { accent = c; }
        int  getSelectedIndex() const { return model.getSelectedItemIndex(); }
        void setSelectedIndex (int i, juce::NotificationType n) { model.setSelectedItemIndex (i, n); }

        std::function<void (int)> onSelect; // fired on a user click (manual / A-B use)

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (! getLocalBounds().toFloat().contains (e.position)) return;
            const int n = texts.size();
            if (n <= 0) return;
            const int seg = juce::jlimit (0, n - 1, (int) (e.position.x / (getWidth() / (float) n)));
            if (seg != getSelectedIndex())
                setSelectedIndex (seg, juce::sendNotificationSync); // writes the bound param (if any) + repaints
            if (onSelect) onSelect (seg);
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (colour::segTrackBg());
            g.fillRoundedRectangle (r, radius::badge);

            const int n = texts.size();
            if (n <= 0) return;
            const float segW = r.getWidth() / (float) n;
            const int sel = getSelectedIndex();
            for (int i = 0; i < n; ++i)
            {
                auto cell = juce::Rectangle<float> (r.getX() + i * segW, r.getY(), segW, r.getHeight()).reduced (3.0f);
                const bool active = (i == sel);
                if (active)
                {
                    g.setColour (accent);
                    g.fillRoundedRectangle (cell, radius::badge - 2.0f);
                }
                auto content = cell;
                if (i < (int) glyphs.size())
                {
                    auto gi = content.removeFromLeft (content.getHeight());
                    icons::paintGlyph (g, glyphs[(size_t) i], gi.reduced (3.0f),
                                       active ? colour::white() : colour::textMuted());
                }
                g.setColour (active ? colour::white() : colour::textMuted());
                g.setFont (font (FontKind::Ui, 12.0f, 800));
                g.drawText (texts[i], content, juce::Justification::centred);
            }
        }

    private:
        juce::ComboBox model;
        juce::StringArray texts;
        std::vector<icons::Glyph> glyphs;
        juce::Colour accent { colour::accent() };
    };

    // ---------------------------------------------------------- RsIconButton
    // A stroked/filled glyph button (header Undo/Redo). Also supports a
    // "directional" mode (header Copy) that draws the two A/B slot letters with
    // an arrow between them ("A>B" / "B>A") so the copy direction is explicit.
    // enabled state dims the content. onClick drives the action.
    class RsIconButton : public juce::Button
    {
    public:
        RsIconButton() : juce::Button ({}) {}

        void setGlyph (icons::Glyph g) { glyph = std::move (g); directional = false; repaint(); }
        void setColours (juce::Colour on, juce::Colour off) { colOn = on; colOff = off; }

        // Directional copy affordance: fixed "A" (left) / "B" (right); only the
        // arrow between them flips to show the copy direction.
        // reversed=false => arrow points A->B; reversed=true => arrow points B->A.
        void setDirection (bool reversed) { directional = true; reversedDir = reversed; repaint(); }

        void paintButton (juce::Graphics& g, bool highlighted, bool down) override
        {
            auto r = getLocalBounds().toFloat();
            if (highlighted || down)
            {
                g.setColour (colour::white().withAlpha (down ? 0.9f : 0.6f));
                g.fillRoundedRectangle (r, radius::badge);
                g.setColour (colour::border());
                g.drawRoundedRectangle (r.reduced (0.5f), radius::badge, 1.0f);
            }

            const auto col = isEnabled() ? colOn : colOff;
            if (directional)
            {
                auto inner = r.reduced (r.getWidth() * 0.08f, r.getHeight() * 0.18f);
                const float w = inner.getWidth();
                auto left  = inner.removeFromLeft (w * 0.34f);
                auto right = inner.removeFromRight (w * 0.34f);
                auto mid   = inner; // arrow sits between the letters
                g.setColour (col);
                // Size the letters to the (narrow) sub-box WIDTH too, and disable
                // drawText's ellipsis fallback -- otherwise an "A" wider than its
                // box silently renders as "..." (three dots) instead of the glyph.
                const float fsize = juce::jmin (inner.getHeight() * 0.92f, left.getWidth() * 1.15f);
                g.setFont (font (FontKind::Ui, fsize, 800));
                // Fixed slot letters: A on the left, B on the right (they do NOT
                // swap). Only the arrow flips to show the copy direction, and it
                // is drawn a touch smaller than the letters.
                g.drawText ("A", left,  juce::Justification::centred, false);
                g.drawText ("B", right, juce::Justification::centred, false);
                const float ay   = mid.getCentreY();
                const float head = fsize * 0.28f;                              // arrowhead smaller than the A/B letters
                const float alen = juce::jmin (mid.getWidth() * 0.82f, fsize * 1.15f);
                const float acx  = mid.getCentreX();
                const float x0   = acx - alen * 0.5f;
                const float x1   = acx + alen * 0.5f;
                juce::Path a;
                a.startNewSubPath (x0, ay);
                a.lineTo (x1, ay);
                if (reversedDir) { a.startNewSubPath (x0 + head, ay - head); a.lineTo (x0, ay); a.lineTo (x0 + head, ay + head); } // B->A: points left
                else             { a.startNewSubPath (x1 - head, ay - head); a.lineTo (x1, ay); a.lineTo (x1 - head, ay + head); } // A->B: points right
                g.strokePath (a, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                return;
            }

            icons::paintGlyph (g, glyph, r.reduced (r.getHeight() * 0.28f), col);
        }

    private:
        icons::Glyph glyph { icons::copy() };
        juce::Colour colOn { colour::text() }, colOff { colour::textFaint() };
        bool directional = false, reversedDir = false;
    };

    // --------------------------------------------------------- RsValueSetting
    // icon + caption + value cell that opens a non-modal menu of all choices on
    // click (QUALITY / CH). Backed by a hidden combo (value model +
    // ComboBoxAttachment target). Draws its own white card.
    class RsValueSetting : public juce::Component,
                           public juce::SettableTooltipClient
    {
    public:
        RsValueSetting()
        {
            addChildComponent (model);
            model.onChange = [this] { repaint(); };
        }

        void setup (icons::Glyph g, const juce::String& caption, const juce::StringArray& items)
        {
            glyph = std::move (g);
            cap = caption;
            model.clear (juce::dontSendNotification);
            model.addItemList (items, 1);
        }

        juce::ComboBox& comboBox() noexcept { return model; }

        void mouseUp (const juce::MouseEvent&) override
        {
            juce::PopupMenu m;
            const int n = model.getNumItems();
            const int cur = model.getSelectedItemIndex();
            for (int i = 0; i < n; ++i)
                m.addItem (i + 1, model.getItemText (i), true, i == cur);

            juce::Component::SafePointer<RsValueSetting> safe (this);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                             [safe] (int r) { if (safe != nullptr && r > 0) safe->model.setSelectedItemIndex (r - 1, juce::sendNotificationSync); });
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (colour::white());
            g.fillRoundedRectangle (r, radius::badge);
            g.setColour (colour::border());
            g.drawRoundedRectangle (r.reduced (0.5f), radius::badge, 1.0f);

            auto inner = r.reduced (9.0f, 0.0f);
            auto gi = inner.removeFromLeft (inner.getHeight());
            icons::paintGlyph (g, glyph, gi.reduced (inner.getHeight() * 0.28f), colour::textSecondary());
            inner.removeFromLeft (6.0f);

            g.setColour (colour::textSecondary());
            g.setFont (font (FontKind::Ui, 11.0f, 800, 0.04f));
            g.drawText (cap, inner, juce::Justification::centredLeft);

            g.setColour (colour::accent());
            g.setFont (font (FontKind::Ui, 12.0f, 800));
            g.drawText (model.getText(), inner, juce::Justification::centredRight);
        }

    private:
        juce::ComboBox model;
        icons::Glyph glyph { icons::quality() };
        juce::String cap;
    };

    // ----------------------------------------------------------- RsLinkSlider
    // Horizontal amount slider: optional leading glyph + caption + track +
    // coral fill + value (right). SliderAttachment-wireable (it IS a Slider).
    // We map the drag ourselves so motion is 1:1 with the DRAWN track (not the
    // full component width, which the base LinearHorizontal would use) and so
    // vertical drag changes the value too. setup() parameterizes the caption /
    // glyph / caption-column-width -- STEREO LINK, MIX and OUT all share this
    // one class. The value read-out rides the slider's own getTextFromValue()
    // (suffix + decimals via the normal setTextValueSuffix()/
    // factory_ui::setSliderDecimals() path, same #23 after-the-attachment rule
    // as RsKnob) instead of the old hand-rolled "roundToInt(...) + '%'" format.
    //
    // Direct text entry (P3c): same idiom as RsKnob (see its class doc) --
    // double-clicking the value read-out (computeValueArea()) opens a
    // juce::Label overlay pre-filled with the current value; Enter/focus-loss
    // commit via setValue(getValueFromText(text), sendNotificationSync), Esc
    // cancels. A double-click anywhere ELSE on the control falls back to the
    // base Slider's own double-click-to-default (SliderParameterAttachment
    // enables this for every attached slider), unchanged from before this
    // class grew an overlay.
    class RsLinkSlider : public juce::Slider
    {
    public:
        RsLinkSlider()
        {
            setSliderStyle (juce::Slider::LinearHorizontal);
            setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            setSliderSnapsToMousePosition (false); // no click-jump; value follows the drag delta

            valueEditor.setEditable (false, true, false); // double-click only; focus loss COMMITS (Label default)
            valueEditor.setJustificationType (juce::Justification::centred);
            valueEditor.setFont (font (FontKind::Ui, 12.0f, 800));
            valueEditor.setColour (juce::Label::textColourId, colour::accent());
            valueEditor.setColour (juce::Label::backgroundColourId, colour::white());
            valueEditor.setColour (juce::Label::outlineColourId, colour::accent());
            valueEditor.setColour (juce::TextEditor::textColourId, colour::text());
            valueEditor.setColour (juce::TextEditor::backgroundColourId, colour::white());
            valueEditor.setColour (juce::TextEditor::highlightColourId, colour::accent().withAlpha (0.35f));
            valueEditor.setColour (juce::TextEditor::focusedOutlineColourId, colour::accent());
            valueEditor.onEditorHide = [this] { valueEditor.setVisible (false); };
            valueEditor.onTextChange = [this]
            {
                setValue (getValueFromText (valueEditor.getText()), juce::sendNotificationSync);
            };
            addChildComponent (valueEditor);
        }

        // Per-instance chrome. No-glyph overload for a plain slider (MIX/OUT);
        // the glyph overload reproduces the original STEREO LINK look exactly
        // (icon + "STEREO LINK" + 86px caption column -- see PluginEditor.cpp,
        // which is the only caller and keeps that instance visually identical
        // to before). Neither overload touches suffix/decimals -- callers use
        // the normal setTextValueSuffix()/factory_ui::setSliderDecimals() path
        // on this Slider directly, same #23 after-the-attachment rule as
        // everywhere else (see class doc).
        void setup (const juce::String& caption, int captionColumnW = 86)
        {
            capText = caption; hasGlyph = false; capColW = captionColumnW;
            repaint();
        }
        void setup (const juce::String& caption, icons::Glyph glyph, int captionColumnW = 86)
        {
            capText = caption; hasGlyph = true; glyphIcon = std::move (glyph); capColW = captionColumnW;
            repaint();
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            // Record the anchor BEFORE calling the base: base mouseDown begins the
            // APVTS change gesture and internally re-invokes mouseDrag(e), which
            // must see a 0-delta (no-op) rather than a stale anchor.
            dragStartValue = getValue();
            dragStartPos   = e.position;
            juce::Slider::mouseDown (e);
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            // Convert drag distance to a PROPORTION delta over the drawn track
            // width (horizontal 1:1 with the visible fill); add the vertical delta
            // (up = increase) so up/down drag also moves the value.
            const float span = juce::jmax (1.0f, computeTrack().getWidth());
            const float dx = e.position.x - dragStartPos.x;
            const float dy = dragStartPos.y - e.position.y;
            const double start = valueToProportionOfLength (dragStartValue);
            const double prop  = juce::jlimit (0.0, 1.0, start + (double) (dx + dy) / span);
            setValue (proportionOfLengthToValue (prop), juce::sendNotificationSync);
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            juce::Slider::mouseUp (e); // ends the APVTS change gesture
        }

        // Double-click the value read-out opens the text editor; a double-
        // click anywhere else on the control falls back to the base class.
        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            if (computeValueArea().contains (e.position))
                openValueEditor();
            else
                juce::Slider::mouseDoubleClick (e);
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (colour::white());
            g.fillRoundedRectangle (r, radius::badge);
            g.setColour (colour::border());
            g.drawRoundedRectangle (r.reduced (0.5f), radius::badge, 1.0f);

            juce::Rectangle<float> iconArea, capArea, trackArea, valArea;
            computeLayout (iconArea, capArea, trackArea, valArea);

            if (hasGlyph)
                icons::paintGlyph (g, glyphIcon, iconArea, colour::textSecondary());

            // Caption on the left, value on the right, track+fill between them.
            g.setColour (colour::textSecondary());
            g.setFont (font (FontKind::Ui, 11.0f, 800, 0.04f));
            g.drawText (capText, capArea, juce::Justification::centredLeft);

            g.setColour (colour::accent());
            g.setFont (font (FontKind::Ui, 12.0f, 800));
            g.drawText (getTextFromValue (getValue()), valArea, juce::Justification::centredRight);

            auto track = trackArea.withSizeKeepingCentre (trackArea.getWidth(), 7.0f);
            g.setColour (colour::linkTrack());
            g.fillRoundedRectangle (track, 3.5f);
            const float prop = (float) valueToProportionOfLength (getValue());
            if (prop > 0.0f)
            {
                auto fill = track.withWidth (track.getWidth() * prop);
                juce::ColourGradient fg (colour::linkFillStart(), fill.getTopLeft(),
                                         colour::accent(), fill.getTopRight(), false);
                g.setGradientFill (fg);
                g.fillRoundedRectangle (fill, 3.5f);
            }
        }

        void resized() override
        {
            juce::Slider::resized();
            valueEditor.setBounds (computeValueArea().getSmallestIntegerContainer());
        }

        // Force-CANCEL (never commit) any in-progress edit; see RsKnob's class
        // doc for the rationale. Kept for API symmetry even though nothing in
        // this plugin currently rebinds an RsLinkSlider's attachment live.
        void closeValueEditor() { valueEditor.hideEditor (true); }

        void visibilityChanged() override      { closeValueEditor(); }
        void parentHierarchyChanged() override { closeValueEditor(); }

    private:
        void openValueEditor()
        {
            valueEditor.setBounds (computeValueArea().getSmallestIntegerContainer());
            valueEditor.setText (stripToLeadingNumber (getTextFromValue (getValue())), juce::dontSendNotification);
            valueEditor.setVisible (true);
            valueEditor.toFront (true);
            valueEditor.showEditor();
        }

        // Single source of truth for the icon/caption/track/value rectangles:
        // paint(), computeTrack() (the drag mapping) and computeValueArea()
        // (the text-entry overlay + its hit-test) all go through this so they
        // can never drift out of sync (the old code separately hand-duplicated
        // the same insets in both paint() and computeTrack()).
        void computeLayout (juce::Rectangle<float>& iconArea, juce::Rectangle<float>& capArea,
                            juce::Rectangle<float>& trackArea, juce::Rectangle<float>& valArea) const
        {
            auto inner = getLocalBounds().toFloat().reduced (9.0f, 0.0f);
            if (hasGlyph)
            {
                iconArea = inner.removeFromLeft (16.0f);
                inner.removeFromLeft (6.0f);
            }
            else
            {
                iconArea = {};
            }
            capArea = inner.removeFromLeft ((float) capColW);
            valArea = inner.removeFromRight (kValColW);
            inner.removeFromRight (6.0f);
            trackArea = inner;
        }

        // The drawn track rect, sized down to the fill's stroke height.
        juce::Rectangle<float> computeTrack() const
        {
            juce::Rectangle<float> ic, cap, track, val;
            computeLayout (ic, cap, track, val);
            return track.withSizeKeepingCentre (track.getWidth(), 7.0f);
        }

        juce::Rectangle<float> computeValueArea() const
        {
            juce::Rectangle<float> ic, cap, track, val;
            computeLayout (ic, cap, track, val);
            return val;
        }

        // Widened from the original 42px: right-justified, so STEREO LINK's
        // "75%" renders at the exact same pixels as before (only the empty
        // space to its LEFT inside the column grows) -- but OUT's widest text
        // ("-24.0dB", 7 glyphs incl. sign) needs the extra room at 12pt bold.
        static constexpr float kValColW = 52.0f;

        juce::String capText;
        bool hasGlyph = false;
        icons::Glyph glyphIcon {};
        int capColW = 86;

        juce::Label valueEditor;

        double dragStartValue = 0.0;
        juce::Point<float> dragStartPos;
    };
}
