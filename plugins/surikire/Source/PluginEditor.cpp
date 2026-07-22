#include "PluginEditor.h"
#include "factory_ui/FactoryChrome.h"

namespace
{
    struct KnobSpec
    {
        const char* paramID;
        const char* caption;
        const char* suffix;
        int decimals;
    };

    // Row-major: row 1 then row 2 (5 columns each). Percent and Hz knobs show
    // whole numbers; the dB knob keeps two decimals.
    constexpr KnobSpec kKnobs[] = {
        { "wow",      "Wow",      " %",  0 },
        { "flutter",  "Flutter",  " %",  0 },
        { "gen",      "Gen",      " %",  0 },
        { "saturate", "Saturate", " %",  0 },
        { "noise",    "Noise",    " %",  0 },
        { "failure",  "Failure",  " %",  0 },
        { "hp",       "HP",       " Hz", 0 },
        { "lp",       "LP",       " Hz", 0 },
        { "mix",      "Mix",      " %",  0 },
        { "output",   "Output",   " dB", 2 },
    };

    static_assert (sizeof (kKnobs) / sizeof (kKnobs[0]) == 10, "expected 10 knobs");
}

SurikireAudioProcessorEditor::SurikireAudioProcessorEditor (SurikireAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), presetController (*this, p)
{
    setLookAndFeel (&lnf);

    titleLabel.setText ("SURIKIRE", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, FactoryLookAndFeel::accent());
    addAndMakeVisible (titleLabel);

    bypassButton.setColour (juce::ToggleButton::textColourId, FactoryLookAndFeel::textDim());
    addAndMakeVisible (bypassButton);

    // The preset selector + host<->editor program sync live in presetController
    // (constructed above); nothing to wire here.

    auto& s = processor.apvts;
    for (int i = 0; i < kNumKnobs; ++i)
    {
        const auto& spec = kKnobs[i];

        factory_ui::styleKnob (sliders[i], labels[i], spec.caption, spec.suffix);
        addAndMakeVisible (sliders[i]);
        addAndMakeVisible (labels[i]);

        sliderAtts[i] = std::make_unique<SliderAttachment> (s, spec.paramID, sliders[i]);
        // Pin the text-box precision. Must run AFTER the attachment above,
        // which otherwise formats continuous ranges with up to 7 decimals
        // (see #23).
        factory_ui::setSliderDecimals (sliders[i], spec.decimals);
    }

    bypassAtt = std::make_unique<ButtonAttachment> (s, "bypass", bypassButton);

    setSize (640, 420);
}

SurikireAudioProcessorEditor::~SurikireAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void SurikireAudioProcessorEditor::paint (juce::Graphics& g)
{
    factory_ui::paintBackground (g, getLocalBounds());
}

void SurikireAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (16);

    auto top = r.removeFromTop (26);
    bypassButton.setBounds (top.removeFromRight (96));
    titleLabel.setBounds (top.removeFromLeft (120));
    top.removeFromLeft (8);
    presetController.selector().setBounds (top);

    r.removeFromTop (14);

    const int rowHeight = r.getHeight() / 2;
    for (int row = 0; row < 2; ++row)
    {
        auto rowArea = r.removeFromTop (rowHeight);
        const int cellWidth = rowArea.getWidth() / 5;
        for (int col = 0; col < 5; ++col)
        {
            const int i = row * 5 + col;
            auto cell = rowArea.removeFromLeft (cellWidth);
            labels[i].setBounds (cell.removeFromTop (18));
            sliders[i].setBounds (cell.reduced (6, 0));
        }
    }
}
