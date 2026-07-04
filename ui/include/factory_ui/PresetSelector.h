#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "factory_ui/FactoryLookAndFeel.h"

#include <functional>

//
// factory_ui::PresetSelector — the shared factory-preset picker every editor
// drops into its top row: a ComboBox flanked by prev/next arrows. Colours and
// dimensions come only from FactoryLookAndFeel (no per-plugin palette).
//
// It is a dumb view: it renders whatever names it is given and reports the
// user's chosen index through onChange. The editor owns the round-trip to the
// processor's program API (setCurrentProgram + updateHostDisplay) and pushes
// host-driven changes back via setSelectedIndex(..., dontSendNotification).
//
namespace factory_ui
{
    class PresetSelector : public juce::Component
    {
    public:
        PresetSelector()
        {
            box.setJustificationType (juce::Justification::centred);
            box.setColour (juce::ComboBox::backgroundColourId, FactoryLookAndFeel::panel());
            box.setColour (juce::ComboBox::textColourId, FactoryLookAndFeel::text());
            box.setColour (juce::ComboBox::outlineColourId, FactoryLookAndFeel::track());
            box.setColour (juce::ComboBox::arrowColourId, FactoryLookAndFeel::accent());
            box.onChange = [this]
            {
                const int idx = box.getSelectedId() - 1;
                if (idx >= 0 && onChange != nullptr)
                    onChange (idx);
            };
            addAndMakeVisible (box);

            for (auto* b : { &prev, &next })
            {
                b->setColour (juce::TextButton::buttonColourId, FactoryLookAndFeel::panel());
                b->setColour (juce::TextButton::textColourOffId, FactoryLookAndFeel::accent());
                addAndMakeVisible (*b);
            }
            prev.onClick = [this] { step (-1); };
            next.onClick = [this] { step (+1); };
        }

        // Replace the item list and select `currentIndex` without firing onChange.
        void setItems (const juce::StringArray& names, int currentIndex)
        {
            box.clear (juce::dontSendNotification);
            for (int i = 0; i < names.size(); ++i)
                box.addItem (names[i], i + 1); // ComboBox IDs are 1-based
            setSelectedIndex (currentIndex, juce::dontSendNotification);
        }

        void setSelectedIndex (int index, juce::NotificationType notify)
        {
            box.setSelectedId (index + 1, notify);
        }

        int getSelectedIndex() const { return box.getSelectedId() - 1; }

        // Fired only on a user-driven change, with the new 0-based index.
        std::function<void (int)> onChange;

        void resized() override
        {
            auto r = getLocalBounds();
            const int arrow = juce::jmin (24, r.getHeight());
            prev.setBounds (r.removeFromLeft (arrow));
            next.setBounds (r.removeFromRight (arrow));
            box.setBounds (r.reduced (4, 0));
        }

    private:
        void step (int delta)
        {
            const int n = box.getNumItems();
            if (n <= 0)
                return;
            const int next0 = juce::jlimit (0, n - 1, getSelectedIndex() + delta);
            if (next0 != getSelectedIndex())
            {
                setSelectedIndex (next0, juce::dontSendNotification);
                if (onChange != nullptr)
                    onChange (next0);
            }
        }

        juce::ComboBox   box;
        juce::TextButton prev { "<" }, next { ">" };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetSelector)
    };
}
