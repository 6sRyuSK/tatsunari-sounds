#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

//
// factory_ui — the shared "kawaii" design system for every plugin in the
// factory. Bright, white-based palette with a single coral accent, gentle
// shadows and rounded shapes: soft pastel knobs and pill toggles. One copy,
// used by every editor (originally extracted from the Dynamic EQ). Header-only.
//
// The class name is intentionally global (FactoryLookAndFeel) so the many
// `FactoryLookAndFeel::accent()` / `panel()` call sites across the plugins keep
// working unchanged after they switch to this shared header.
//
class FactoryLookAndFeel : public juce::LookAndFeel_V4
{
public:
    // ---- palette ----
    static juce::Colour background() { return juce::Colour (0xfffdf6f2); } // warm white
    static juce::Colour backgroundLo(){ return juce::Colour (0xfffbeae2); } // gradient foot
    static juce::Colour panel()      { return juce::Colour (0xffffffff); } // card white
    static juce::Colour panelLo()    { return juce::Colour (0xfffff4ee); } // card foot
    static juce::Colour track()      { return juce::Colour (0xfff2ddd4); } // grid / outline
    static juce::Colour accent()     { return juce::Colour (0xffff7a6b); } // coral
    static juce::Colour accentDim()  { return juce::Colour (0xffffd6cd); } // pale coral
    static juce::Colour text()       { return juce::Colour (0xff6b5750); } // soft cocoa
    static juce::Colour textDim()    { return juce::Colour (0xffb9a39b); } // muted
    static juce::Colour shadow()     { return juce::Colour (0x33d6a89a); } // warm soft shadow

    // One pastel hue per band — warm-biased kawaii spread (multi-band plugins).
    static juce::Colour bandColour (int band)
    {
        static const std::array<juce::Colour, 6> pal {
            juce::Colour (0xffff6f91), // strawberry
            juce::Colour (0xffff9472), // coral
            juce::Colour (0xffffba6b), // apricot
            juce::Colour (0xff7fd1ae), // mint
            juce::Colour (0xff79b8ef), // sky
            juce::Colour (0xffb79be8)  // lavender
        };
        return pal[(size_t) (band % (int) pal.size())];
    }

    FactoryLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, background());
        setColour (juce::Label::textColourId, text());
        setColour (juce::Slider::textBoxTextColourId, text());
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::ToggleButton::textColourId, text());
        setColour (juce::ToggleButton::tickColourId, accent());
        setColour (juce::ToggleButton::tickDisabledColourId, track());
        setColour (juce::ComboBox::backgroundColourId, background());
        setColour (juce::ComboBox::textColourId, text());
        setColour (juce::ComboBox::outlineColourId, track());
        setColour (juce::ComboBox::arrowColourId, accent());
        setColour (juce::PopupMenu::backgroundColourId, panel());
        setColour (juce::PopupMenu::highlightedBackgroundColourId, accentDim());
        setColour (juce::PopupMenu::textColourId, text());
        setColour (juce::PopupMenu::highlightedTextColourId, text());
        // Section headers (juce::ComboBox::addSectionHeading, e.g. the "User"
        // row PresetSelectorController adds ahead of user presets -- Phase 5c):
        // LookAndFeel_V4's base ctor defaults headerTextColourId from its DARK
        // colour scheme (white), which is invisible against our white panel()
        // popup background unless set explicitly here.
        setColour (juce::PopupMenu::headerTextColourId, accent());
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider& s) override
    {
        auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (6.0f);
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const float lineW = radius * 0.18f;
        const float arcR  = radius - lineW * 0.5f;
        const float toAngle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

        // Track arc.
        juce::Path bg;
        bg.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
        g.setColour (track());
        g.strokePath (bg, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Value arc (with a soft glow).
        if (sliderPos > 0.0f)
        {
            juce::Path val;
            val.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, rotaryStartAngle, toAngle, true);
            g.setColour (accent().withAlpha (0.25f));
            g.strokePath (val, juce::PathStrokeType (lineW * 1.9f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (accent());
            g.strokePath (val, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // Knob body — soft drop shadow + top-lit gradient for a rounded look.
        const float bodyR = radius - lineW * 1.7f;
        auto body = juce::Rectangle<float> (bodyR * 2.0f, bodyR * 2.0f).withCentre (centre);
        {
            juce::DropShadow ds (shadow(), (int) juce::jmax (3.0f, bodyR * 0.5f), { 0, 2 });
            juce::Path bp; bp.addEllipse (body);
            ds.drawForPath (g, bp);
        }
        juce::ColourGradient bodyGrad (juce::Colours::white, centre.x, body.getY(),
                                       panelLo(), centre.x, body.getBottom(), false);
        g.setGradientFill (bodyGrad);
        g.fillEllipse (body);
        g.setColour (track());
        g.drawEllipse (body, 1.0f);

        // Pointer dot near the rim.
        const float dotR = juce::jmax (2.0f, lineW * 0.55f);
        const float pr = bodyR * 0.62f;
        const juce::Point<float> dot (centre.x + pr * std::cos (toAngle - juce::MathConstants<float>::halfPi),
                                      centre.y + pr * std::sin (toAngle - juce::MathConstants<float>::halfPi));
        g.setColour (s.isEnabled() ? accent() : textDim());
        g.fillEllipse (juce::Rectangle<float> (dotR * 2.0f, dotR * 2.0f).withCentre (dot));
    }

    // Cute rounded toggle: a pill that fills with its tickColour when on.
    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b,
                           bool /*highlighted*/, bool /*down*/) override
    {
        auto r = b.getLocalBounds().toFloat();
        const float h = juce::jmin (20.0f, r.getHeight());
        auto box = juce::Rectangle<float> (h * 1.7f, h).withY (r.getCentreY() - h * 0.5f).withX (r.getX());
        const bool on = b.getToggleState();

        const auto onColour = b.findColour (juce::ToggleButton::tickColourId);
        g.setColour (on ? onColour : track());
        g.fillRoundedRectangle (box, h * 0.5f);
        const float knobD = h - 4.0f;
        const float kx = on ? box.getRight() - knobD - 2.0f : box.getX() + 2.0f;
        g.setColour (juce::Colours::white);
        g.fillEllipse (kx, box.getY() + 2.0f, knobD, knobD);

        g.setColour (text());
        g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        g.drawText (b.getButtonText(),
                    r.withTrimmedLeft (box.getWidth() + 8.0f),
                    juce::Justification::centredLeft);
    }

    juce::Font getLabelFont (juce::Label&) override { return juce::Font (juce::FontOptions (13.0f)); }
};
