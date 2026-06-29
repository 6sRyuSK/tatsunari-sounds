#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

//
// Dark LookAndFeel for the dynamic parametric EQ. Violet accent for its own
// identity. Header-only.
//
class FactoryLookAndFeel : public juce::LookAndFeel_V4
{
public:
    static juce::Colour background() { return juce::Colour (0xff15181e); }
    static juce::Colour panel()      { return juce::Colour (0xff1e232c); }
    static juce::Colour track()      { return juce::Colour (0xff2c333f); }
    static juce::Colour accent()     { return juce::Colour (0xff8e7bff); } // violet
    static juce::Colour accentDim()  { return juce::Colour (0xff4b3f8a); }
    static juce::Colour text()       { return juce::Colour (0xffd8dde4); }
    static juce::Colour textDim()    { return juce::Colour (0xff8b94a3); }

    FactoryLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, background());
        setColour (juce::Label::textColourId, text());
        setColour (juce::Slider::textBoxTextColourId, text());
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::ToggleButton::textColourId, text());
        setColour (juce::ToggleButton::tickColourId, accent());
        setColour (juce::ToggleButton::tickDisabledColourId, textDim());
        setColour (juce::ComboBox::backgroundColourId, panel());
        setColour (juce::ComboBox::textColourId, text());
        setColour (juce::ComboBox::outlineColourId, track());
        setColour (juce::ComboBox::arrowColourId, accent());
        setColour (juce::PopupMenu::backgroundColourId, panel());
        setColour (juce::PopupMenu::highlightedBackgroundColourId, accentDim());
        setColour (juce::PopupMenu::textColourId, text());
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider&) override
    {
        auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (6.0f);
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const float lineW = radius * 0.16f;
        const float arcR  = radius - lineW * 0.5f;
        const float toAngle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

        juce::Path bg;
        bg.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
        g.setColour (track());
        g.strokePath (bg, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        if (sliderPos > 0.0f)
        {
            juce::Path val;
            val.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, rotaryStartAngle, toAngle, true);
            g.setColour (accent());
            g.strokePath (val, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        const float bodyR = radius - lineW * 1.6f;
        g.setColour (panel().brighter (0.12f));
        g.fillEllipse (juce::Rectangle<float> (bodyR * 2.0f, bodyR * 2.0f).withCentre (centre));

        juce::Path pointer;
        const float pw = juce::jmax (2.0f, lineW * 0.5f);
        pointer.addRoundedRectangle (-pw * 0.5f, -bodyR * 0.95f, pw, bodyR * 0.55f, pw * 0.5f);
        pointer.applyTransform (juce::AffineTransform::rotation (toAngle).translated (centre.x, centre.y));
        g.setColour (accent().brighter (0.2f));
        g.fillPath (pointer);
    }

    juce::Font getLabelFont (juce::Label&) override { return juce::Font (juce::FontOptions (13.0f)); }
};
