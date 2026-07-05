#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "factory_ui/PresetSelector.h"

//
// factory_ui::PresetSelectorController — owns a PresetSelector and the whole
// two-way host<->editor preset sync that every editor otherwise duplicates:
//   - populates the selector from the processor's program list,
//   - on a user pick drives the program API (setCurrentProgram) and notifies the
//     host (updateHostDisplay withProgramChanged),
//   - follows host-driven program changes back into the selector.
//
// Drop one into an editor as a member, constructed with the editor as `parent`
// and its processor; lay out selector() in resized(). The editor no longer needs
// to inherit juce::AudioProcessorListener or repeat the wiring — this class is
// the listener. Declare it after the processor reference it captures so member
// init/teardown order stays correct (its destructor removes the listener).
//
// Real-time safety: audioProcessorChanged may arrive on any thread, so the
// selector update is marshalled to the message thread; a SafePointer to the
// parent editor guards against the editor (and therefore this controller) being
// deleted before the async job runs.
//
namespace factory_ui
{
    class PresetSelectorController : private juce::AudioProcessorListener
    {
    public:
        PresetSelectorController (juce::Component& parentToHost, juce::AudioProcessor& processorToDrive)
            : parent (parentToHost), processor (processorToDrive)
        {
            refresh();
            selectorView.onChange = [this] (int idx)
            {
                processor.setCurrentProgram (idx);
                processor.updateHostDisplay (
                    juce::AudioProcessorListener::ChangeDetails{}.withProgramChanged (true));
            };
            parent.addAndMakeVisible (selectorView);
            processor.addListener (this);
        }

        ~PresetSelectorController() override
        {
            processor.removeListener (this);
        }

        // The owned selector — position it in the editor's resized().
        PresetSelector& selector() noexcept { return selectorView; }

    private:
        // Populate the selector from the processor's current program list.
        void refresh()
        {
            juce::StringArray names;
            for (int i = 0; i < processor.getNumPrograms(); ++i)
                names.add (processor.getProgramName (i));
            selectorView.setItems (names, processor.getCurrentProgram());
        }

        // AudioProcessorListener — follow host-driven program changes.
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& details) override
        {
            if (! details.programChanged)
                return;

            // May arrive on any thread; marshal the selector update to the message
            // thread. The SafePointer to the parent editor ensures a deleted editor
            // (which owns this controller) can't be called back.
            juce::Component::SafePointer<juce::Component> safe (&parent);
            juce::MessageManager::callAsync ([this, safe]
            {
                if (safe != nullptr)
                    selectorView.setSelectedIndex (processor.getCurrentProgram(),
                                                   juce::dontSendNotification);
            });
        }

        void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}

        juce::Component&      parent;
        juce::AudioProcessor& processor;
        PresetSelector        selectorView;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetSelectorController)
    };
}
