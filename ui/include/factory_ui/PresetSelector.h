#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "factory_ui/FactoryLookAndFeel.h"

#include <functional>
#include <set>
#include <vector>

//
// factory_ui::PresetSelector — the shared factory-preset picker every editor
// drops into its top row: a ComboBox flanked by prev/next arrows. Colours and
// dimensions come only from FactoryLookAndFeel (no per-plugin palette).
//
// It is a dumb view: it renders whatever rows it is given and reports the
// user's chosen row through onChange as a 0-based ROW INDEX (position among
// selectable rows only, in the order they were added — headers/separators
// don't consume an index, matching juce::ComboBox's own getNumItems()/
// getItemId(), which already exclude them). The editor-side controller owns
// what a row *means* (factory program vs. user preset vs. an action) and the
// round-trip to the processor's program API; this view never assigns that
// meaning itself. Row ids are auto-assigned 1..N in insertion order, so
// `index == id - 1` always holds and callers never have to think about ids.
//
namespace factory_ui
{
    class PresetSelector : public juce::Component
    {
    public:
        // One dropdown row. `item` rows are real, clickable ComboBox entries;
        // `header`/`separator` rows are pure chrome (JUCE's addSectionHeading /
        // addSeparator) and are already excluded from the id/index space by
        // ComboBox itself. `steppable` further restricts what the prev/next
        // ARROWS may land on: an action row (e.g. "Save As...") is a real,
        // directly-pickable item but must never be reached by stepping, only by
        // an explicit pick — set steppable = false for those.
        struct Entry
        {
            enum class Kind { item, header, separator };

            static Entry item (juce::String text, bool enabled = true, bool steppable = true)
            {
                Entry e;
                e.kind = Kind::item;
                e.text = std::move (text);
                e.enabled = enabled;
                e.steppable = steppable;
                return e;
            }

            static Entry header (juce::String text)
            {
                Entry e;
                e.kind = Kind::header;
                e.text = std::move (text);
                return e;
            }

            static Entry separator()
            {
                Entry e;
                e.kind = Kind::separator;
                return e;
            }

            Kind kind = Kind::item;
            juce::String text;
            bool enabled = true;
            bool steppable = true;
        };

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

        // Replace the item list and select `currentIndex` without firing
        // onChange. Back-compat convenience for a flat, all-steppable menu with
        // no sections/actions (equivalent to setMenu() with all-item entries).
        void setItems (const juce::StringArray& names, int currentIndex)
        {
            std::vector<Entry> entries;
            entries.reserve ((size_t) names.size());
            for (int i = 0; i < names.size(); ++i)
                entries.push_back (Entry::item (names[i]));
            setMenu (entries, currentIndex);
        }

        // The general form: a mixed set of selectable rows, section headers and
        // separators (in order). `selectedIndex` is a 0-based row index (into
        // just the item rows, i.e. what onChange/getSelectedIndex use) — pass -1
        // for no selection. Never fires onChange.
        void setMenu (const std::vector<Entry>& entries, int selectedIndex)
        {
            box.clear (juce::dontSendNotification);
            nonSteppableIds.clear();

            int nextId = 1;
            for (auto& e : entries)
            {
                switch (e.kind)
                {
                    case Entry::Kind::header:
                        box.addSectionHeading (e.text);
                        break;
                    case Entry::Kind::separator:
                        box.addSeparator();
                        break;
                    case Entry::Kind::item:
                        box.addItem (e.text, nextId);
                        box.setItemEnabled (nextId, e.enabled);
                        if (! e.steppable)
                            nonSteppableIds.insert (nextId);
                        ++nextId;
                        break;
                }
            }
            setSelectedIndex (selectedIndex, juce::dontSendNotification);
        }

        void setSelectedIndex (int index, juce::NotificationType notify)
        {
            box.setSelectedId (index + 1, notify);
        }

        int getSelectedIndex() const { return box.getSelectedId() - 1; }

        // Fired only on a user-driven change (a direct pick or an arrow step
        // landing on a steppable row), with the new 0-based row index.
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
        bool isNonSteppable (int index) const
        {
            return nonSteppableIds.count (box.getItemId (index)) != 0;
        }

        void step (int delta)
        {
            const int n = box.getNumItems();
            if (n <= 0)
                return;
            const int start = getSelectedIndex();
            int candidate = start;
            for (;;)
            {
                const int next0 = candidate + delta;
                if (next0 < 0 || next0 >= n)
                    break; // ran off the end without finding a steppable row
                candidate = next0;
                if (! isNonSteppable (candidate))
                    break;
            }
            if (candidate != start && ! isNonSteppable (candidate))
            {
                setSelectedIndex (candidate, juce::dontSendNotification);
                if (onChange != nullptr)
                    onChange (candidate);
            }
        }

        juce::ComboBox   box;
        juce::TextButton prev { "<" }, next { ">" };
        // Item ids (1-based, dense, insertion order — see setMenu()) that the
        // prev/next arrows must skip over (action rows).
        std::set<int> nonSteppableIds;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetSelector)
    };
}
