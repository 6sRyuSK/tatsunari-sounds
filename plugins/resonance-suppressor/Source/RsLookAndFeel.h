#pragma once

#include "RsTheme.h"
#include "factory_ui/FactoryLookAndFeel.h"

#include <cmath>

//
// RsLookAndFeel — the Resonance TatSuppressor "new UI" LookAndFeel (Phase P1).
// Subclasses the shared FactoryLookAndFeel (so popup-menu / combo colours and
// every un-overridden method stay on the house palette) and replaces the knob
// drawing with the demo's chunky arc-ring style. All geometry / colours are
// transcribed from rs-ui-work/demo-analysis.md (SS2.3-2.4 knob specs, SS6 arc
// math + face gradient). Fonts route through rs::font() so a later Baloo 2 /
// Nunito swap is a one-place change.
//
class RsLookAndFeel : public FactoryLookAndFeel
{
public:
    RsLookAndFeel()
    {
        // Rotary fill defaults to coral; per-knob accents override it via
        // Slider::rotarySliderFillColourId (RsKnob sets that per knob).
        setColour (juce::Slider::rotarySliderFillColourId, rs::colour::accent());
        setColour (juce::Slider::textBoxTextColourId, rs::colour::accent());
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Label::textColourId, rs::colour::text());
    }

    // Demo-style rotary: a fat arc ring (track + per-knob active fill) around a
    // top-lit domed face with a short rounded pointer bar. The passed
    // rotaryStartAngle/EndAngle are ignored -- the sweep is fixed to the demo's
    // 225 deg start / 270 deg clockwise / 90 deg bottom gap (SS6).
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float /*rotaryStartAngle*/, float /*rotaryEndAngle*/,
                           juce::Slider& s) override
    {
        const auto area = juce::Rectangle<int> (x, y, width, height).toFloat();
        const float diameter = juce::jmin (area.getWidth(), area.getHeight());
        const auto knob = juce::Rectangle<float> (diameter, diameter).withCentre (area.getCentre());
        const auto centre = knob.getCentre();
        const float R = diameter * 0.5f;

        // Ring band thickness / face radius / arc radius. Ratios from SS6:
        // inset 15@88, 10@60, 8@46  =>  ~0.17*d holds the arc; face is inside it.
        const float band  = diameter * 0.17f;
        const float faceR = R - band;
        const float arcR  = R - band * 0.5f;

        // JUCE rotary angle convention: 0 rad = 12 o'clock, increasing clockwise.
        // 225 deg = lower-left (7:30); +270 deg clockwise ends at 135 deg (lower-
        // right, i.e. 495 deg), leaving a 90 deg dead gap across the bottom.
        constexpr float startDeg = 225.0f;
        constexpr float sweepDeg = 270.0f;
        const float startRad = juce::degreesToRadians (startDeg);
        const float endRad   = juce::degreesToRadians (startDeg + sweepDeg);
        const float valRad   = juce::degreesToRadians (startDeg + juce::jlimit (0.0f, 1.0f, sliderPos) * sweepDeg);

        // Butt end-caps (not rounded): the demo draws the ring as a conic-gradient
        // whose accent/track boundary and 90 deg bottom gap are flat radial edges,
        // so rounded caps' semicircular bulges read as unwanted "corners". The
        // pointer bar (below) stays rounded -- the demo's pointer IS rounded.
        const juce::PathStrokeType arcStroke (band, juce::PathStrokeType::curved, juce::PathStrokeType::butt);

        // Track arc (unfilled portion): #ffd6cd across the full sweep. The 90 deg
        // bottom gap is left unpainted -- it shows the footer's #fff4ee, which IS
        // the demo's "gap sector" colour, so no explicit gap arc is needed.
        juce::Path track;
        track.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, startRad, endRad, true);
        g.setColour (rs::colour::knobTrack());
        g.strokePath (track, arcStroke);

        // Active arc (0 -> value) in the per-knob accent.
        const auto accent = s.findColour (juce::Slider::rotarySliderFillColourId);
        if (sliderPos > 0.0f)
        {
            juce::Path val;
            val.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, startRad, valRad, true);
            g.setColour (accent);
            g.strokePath (val, arcStroke);
        }

        // Face: soft drop shadow, then a radial highlight (white -> cream) with
        // the light source high (~36% from the top), then a hairline rim.
        const auto face = juce::Rectangle<float> (faceR * 2.0f, faceR * 2.0f).withCentre (centre);
        {
            juce::DropShadow ds (rs::colour::shadowTaupe(), juce::jmax (3, (int) (band * 0.6f)), { 0, 2 });
            juce::Path fp; fp.addEllipse (face);
            ds.drawForPath (g, fp);
        }
        const juce::Point<float> hi (centre.x, face.getY() + face.getHeight() * 0.36f);
        juce::ColourGradient grad (rs::colour::white(), hi.x, hi.y,
                                   rs::colour::plotBottom(), hi.x, hi.y + faceR * 1.6f, true);
        g.setGradientFill (grad);
        g.fillEllipse (face);
        g.setColour (rs::colour::border());
        g.drawEllipse (face, 1.0f);

        // Pointer bar: 3px rounded, from the face centre outward (transform-origin
        // 50% 100% in the demo => bottom at centre), length ~0.30*d (26@88 etc.),
        // in the per-knob accent.
        const float len  = diameter * 0.30f;
        const float penW = juce::jmax (2.0f, diameter * 0.035f);
        const juce::Point<float> tip (centre.x + len * std::sin (valRad),
                                      centre.y - len * std::cos (valRad));
        juce::Path ptr;
        ptr.startNewSubPath (centre);
        ptr.lineTo (tip);
        g.setColour (accent);
        g.strokePath (ptr, juce::PathStrokeType (penW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Route the shared label / combo text through rs::font so the whole chrome
    // reads in one (bold, rounded) family. RsWidgets paints most of its own text
    // directly; these cover the bits that go through the LookAndFeel (the preset
    // pill's ComboBox text + any plain Label).
    juce::Font getLabelFont (juce::Label&) override
    {
        return rs::font (rs::FontKind::Ui, 13.0f, 700);
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override
    {
        return rs::font (rs::FontKind::Ui, 13.0f, 700);
    }

    // Seamless white "pill" for the (only) visible ComboBox -- the preset
    // selector -- so it reads as the demo's < name v > pill rather than a boxed
    // combo. Safe to override globally: the RsSegmented / RsValueSetting model
    // combos are never made visible, so they are never painted through this.
    void drawComboBox (juce::Graphics& g, int width, int height, bool /*down*/,
                       int /*buttonX*/, int /*buttonY*/, int /*buttonW*/, int /*buttonH*/,
                       juce::ComboBox& /*box*/) override
    {
        auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);
        const float rad = juce::jmin (rs::radius::badgeLg, height * 0.5f);
        g.setColour (rs::colour::white());
        g.fillRoundedRectangle (r, rad);
        g.setColour (rs::colour::border());
        g.drawRoundedRectangle (r.reduced (0.5f), rad, 1.0f);

        // Faint caret on the right (the preset menu affordance). Drawn inline so
        // the LookAndFeel stays independent of RsWidgets' rs::icons.
        const float cx = (float) width - height * 0.5f, cy = height * 0.5f;
        const float cw = height * 0.15f;
        juce::Path caret;
        caret.startNewSubPath (cx - cw, cy - cw * 0.5f);
        caret.lineTo (cx, cy + cw * 0.5f);
        caret.lineTo (cx + cw, cy - cw * 0.5f);
        g.setColour (rs::colour::textFaint());
        g.strokePath (caret, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    juce::Font getPopupMenuFont() override
    {
        return rs::font (rs::FontKind::Ui, 14.0f, 600);
    }
};
