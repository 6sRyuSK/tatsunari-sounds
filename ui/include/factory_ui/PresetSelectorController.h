#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "factory_presets/UserPresetStore.h"
#include "factory_ui/FactoryLookAndFeel.h"
#include "factory_ui/PresetSelector.h"

#include <memory>
#include <vector>

//
// factory_ui::PresetSelectorController — owns a PresetSelector and the whole
// two-way host<->editor preset sync that every editor otherwise duplicates:
//   - populates the selector from the processor's program list PLUS the
//     plugin's on-disk user presets (Phase 5c) plus Save As/Overwrite/Delete
//     action rows,
//   - on a factory-program pick, drives the program API (setCurrentProgram)
//     and notifies the host (updateHostDisplay withProgramChanged),
//   - on a user-preset pick, applies its state via the processor's own
//     getStateInformation/setStateInformation pair (the exact path a host
//     uses to save/restore a session -- no per-plugin cooperation needed
//     beyond the AudioProcessor API every plugin already implements),
//   - on an action row, runs Save As (a non-modal CallOutBox) / Overwrite /
//     Delete, then restores the combo's displayed text to whatever is
//     actually active (an action row must never linger as "the selection"),
//   - follows host-driven program changes back into the selector.
//
// Drop one into an editor as a member, constructed with the editor as `parent`
// and its processor; lay out selector() in resized(). The editor no longer
// needs to inherit juce::AudioProcessorListener or repeat the wiring -- this
// class is the listener. Declare it after the processor reference it captures
// so member init/teardown order stays correct (its destructor removes the
// listener). Enablement is automatic: the user-preset store is derived from
// processor.getName(), so every plugin using this controller gets user
// presets with zero per-plugin opt-in.
//
// Host program list stays factory-only (getNumPrograms()/getProgramName() are
// untouched by this class) -- a user preset never becomes a host program; it
// only ever exists as an extra row in THIS combo plus a file on disk.
//
// Real-time safety: this entire class is message-thread only (GUI events, file
// I/O via UserPresetStore); audioProcessorChanged may arrive on any thread per
// the base contract, so its work is marshalled via MessageManager::callAsync,
// guarded by a SafePointer to the parent editor.
//
namespace factory_ui
{
    class PresetSelectorController : private juce::AudioProcessorListener
    {
    public:
        PresetSelectorController (juce::Component& parentToHost, juce::AudioProcessor& processorToDrive)
            : parent (parentToHost), processor (processorToDrive), userStore (processorToDrive.getName())
        {
            selectorView.onChange = [this] (int idx) { handlePick (idx); };
            parent.addAndMakeVisible (selectorView);
            processor.addListener (this);
            refresh();
        }

        ~PresetSelectorController() override
        {
            processor.removeListener (this);
        }

        // The owned selector — position it in the editor's resized().
        PresetSelector& selector() noexcept { return selectorView; }

        // Re-enumerate factory programs + on-disk user presets and rebuild the
        // dropdown, preserving whichever is currently active (a factory program
        // or the active user preset). Public (Phase 5c) so Save As / Overwrite /
        // Delete can call it after changing what's on disk; also runs once at
        // construction.
        void refresh()
        {
            userNames = userStore.list();
            const int numFactory = processor.getNumPrograms();

            std::vector<PresetSelector::Entry> entries;
            entries.reserve ((size_t) numFactory + (size_t) userNames.size() + 6);
            for (int i = 0; i < numFactory; ++i)
                entries.push_back (PresetSelector::Entry::item (processor.getProgramName (i)));

            // "User" section: always shown (even with zero presets, so the
            // section — and the Save As entry point — stays discoverable), then
            // the action rows. Overwrite/Delete are only enabled while a user
            // preset is the active selection; both are non-steppable, together
            // with Save As, so prev/next never lands on any of the three.
            entries.push_back (PresetSelector::Entry::separator());
            entries.push_back (PresetSelector::Entry::header ("User"));
            for (auto& n : userNames)
                entries.push_back (PresetSelector::Entry::item (n));

            const bool userActive = activeUserPresetName.isNotEmpty();
            entries.push_back (PresetSelector::Entry::separator());
            entries.push_back (PresetSelector::Entry::item ("Save As...", true, false));
            entries.push_back (PresetSelector::Entry::item ("Overwrite", userActive, false));
            entries.push_back (PresetSelector::Entry::item ("Delete", userActive, false));

            selectorView.setMenu (entries, currentDisplayIndex (numFactory));
        }

    private:
        // 0-based row index of whichever program/preset is ACTUALLY active
        // right now (a factory program, or the active user preset if it's
        // still on disk) — used both for the menu's initial selection and to
        // restore the display after an action row is picked.
        int currentDisplayIndex (int numFactory) const
        {
            if (activeUserPresetName.isNotEmpty())
            {
                const int u = userNames.indexOf (activeUserPresetName);
                if (u >= 0)
                    return numFactory + u;
                // Vanished from disk since it was made active (deleted
                // out-of-band) -- fall through to the factory program below.
            }
            return juce::jlimit (0, juce::jmax (0, numFactory - 1), processor.getCurrentProgram());
        }

        // Serialise the processor's CURRENT live state the same way a host
        // save/reload would (getStateInformation), so a saved user preset is
        // exactly what host persistence would have stored. copyXmlToBinary/
        // getXmlFromBinary are public statics of juce::AudioProcessor, so no
        // access to the concrete processor's own APVTS is needed here at all.
        std::unique_ptr<juce::XmlElement> currentStateXml() const
        {
            juce::MemoryBlock block;
            processor.getStateInformation (block);
            return juce::AudioProcessor::getXmlFromBinary (block.getData(), (int) block.getSize());
        }

        void handlePick (int idx)
        {
            const int numFactory = processor.getNumPrograms();
            if (idx < numFactory)
            {
                activeUserPresetName.clear();
                processor.setCurrentProgram (idx);
                refresh(); // Overwrite/Delete must go back to disabled promptly
                notifyProgramChanged();
                return;
            }

            const int u = idx - numFactory;
            if (u < userNames.size())
            {
                loadUserPreset (userNames[u]);
                return;
            }

            // An action row: restore the display to whatever is actually active
            // BEFORE running the action -- Save As's callout is asynchronous, so
            // the combo must not sit showing "Save As..." while it's open, and
            // Overwrite/Delete's own refresh() would otherwise be the only thing
            // fixing the display up (still correct, but this keeps the display
            // right from the very first repaint).
            selectorView.setSelectedIndex (currentDisplayIndex (numFactory), juce::dontSendNotification);
            switch (u - userNames.size())
            {
                case 0: showSaveAsCallout(); break;
                case 1: overwriteActive();   break;
                case 2: deleteActive();      break;
                default: break; // defensive; refresh() never adds a 4th action row
            }
        }

        void loadUserPreset (const juce::String& name)
        {
            auto xml = userStore.load (name);
            if (xml == nullptr)
            {
                refresh(); // vanished on disk since list() -- drop the stale row
                return;
            }

            // Same path a host uses to restore a session (setStateInformation).
            // NOTE (Phase 5b precedent): this rides apvts.replaceState()
            // underneath, which clears undo history -- accepted for preset loads
            // exactly as it is for A/B slot switches (see loadStateFromSlot in
            // PluginProcessor.cpp).
            juce::MemoryBlock block;
            juce::AudioProcessor::copyXmlToBinary (*xml, block);
            processor.setStateInformation (block.getData(), (int) block.getSize());

            activeUserPresetName = name;
            refresh();
            notifyProgramChanged();
        }

        void overwriteActive()
        {
            if (activeUserPresetName.isEmpty())
                return; // guarded by the row's enabled state; defensive only
            if (auto xml = currentStateXml())
                userStore.save (activeUserPresetName, *xml);
            refresh();
        }

        void deleteActive()
        {
            if (activeUserPresetName.isEmpty())
                return; // guarded by the row's enabled state; defensive only
            userStore.remove (activeUserPresetName);
            activeUserPresetName.clear();
            refresh();
        }

        void saveAs (const juce::String& rawName)
        {
            const auto name = rawName.trim();
            if (name.isNotEmpty())
                if (auto xml = currentStateXml())
                {
                    userStore.save (name, *xml);
                    activeUserPresetName = name;
                }
            refresh(); // no-op display fix-up even on an empty/failed name
        }

        // Non-modal name entry for Save As (AlertWindow::runModalLoop is
        // banned) -- a small CallOutBox with a TextEditor + OK/Cancel, styled
        // from the shared palette. A local class: only ever instantiated here.
        void showSaveAsCallout()
        {
            class SaveAsContent : public juce::Component
            {
            public:
                std::function<void (const juce::String&)> onOk;

                SaveAsContent()
                {
                    label.setText ("Save preset as", juce::dontSendNotification);
                    label.setColour (juce::Label::textColourId, FactoryLookAndFeel::text());
                    label.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
                    addAndMakeVisible (label);

                    editor.setMultiLine (false);
                    editor.setReturnKeyStartsNewLine (false);
                    editor.setTextToShowWhenEmpty ("Preset name", FactoryLookAndFeel::textDim());
                    editor.setColour (juce::TextEditor::backgroundColourId, FactoryLookAndFeel::background());
                    editor.setColour (juce::TextEditor::textColourId, FactoryLookAndFeel::text());
                    editor.setColour (juce::TextEditor::outlineColourId, FactoryLookAndFeel::track());
                    editor.setColour (juce::TextEditor::focusedOutlineColourId, FactoryLookAndFeel::accent());
                    editor.onReturnKey = [this] { triggerOk(); };
                    addAndMakeVisible (editor);

                    okButton.setColour (juce::TextButton::buttonColourId, FactoryLookAndFeel::accent());
                    okButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
                    okButton.onClick = [this] { triggerOk(); };
                    addAndMakeVisible (okButton);

                    cancelButton.setColour (juce::TextButton::buttonColourId, FactoryLookAndFeel::panel());
                    cancelButton.setColour (juce::TextButton::textColourOffId, FactoryLookAndFeel::text());
                    cancelButton.onClick = [this] { dismiss(); };
                    addAndMakeVisible (cancelButton);

                    setSize (230, 92);
                }

                void grabInitialFocus()
                {
                    editor.grabKeyboardFocus();
                    editor.selectAll();
                }

                void paint (juce::Graphics& g) override
                {
                    g.fillAll (FactoryLookAndFeel::panel());
                }

                void resized() override
                {
                    auto r = getLocalBounds().reduced (10);
                    label.setBounds (r.removeFromTop (18));
                    r.removeFromTop (6);
                    editor.setBounds (r.removeFromTop (26));
                    r.removeFromTop (8);
                    auto buttons = r.removeFromTop (26);
                    cancelButton.setBounds (buttons.removeFromRight (80));
                    buttons.removeFromRight (8);
                    okButton.setBounds (buttons.removeFromRight (80));
                }

            private:
                void triggerOk()
                {
                    if (onOk != nullptr)
                        onOk (editor.getText());
                    dismiss();
                }

                void dismiss()
                {
                    if (auto* cb = findParentComponentOfClass<juce::CallOutBox>())
                        cb->dismiss();
                }

                juce::Label      label;
                juce::TextEditor editor;
                juce::TextButton okButton { "Save" }, cancelButton { "Cancel" };
            };

            auto content = std::make_unique<SaveAsContent>();
            // Lifetime guard: the callout outlives (or could outlive) this
            // controller if the editor is torn down while it is open --
            // launching with &parent keeps it implicitly safe today (a child
            // callout dies with the editor), but guard `this` explicitly via a
            // SafePointer to the parent editor (whose lifetime encloses this
            // controller's -- same pattern as audioProcessorChanged above) so
            // a future caller switching to a desktop/nullptr-parent callout
            // can't turn onOk into a use-after-free.
            juce::Component::SafePointer<juce::Component> safe (&parent);
            content->onOk = [this, safe] (const juce::String& name)
            {
                if (safe != nullptr)
                    saveAs (name);
            };
            auto* contentPtr = content.get();
            juce::CallOutBox::launchAsynchronously (std::move (content), selectorView.getBounds(), &parent);
            contentPtr->grabInitialFocus();
        }

        void notifyProgramChanged()
        {
            // Consumed by the very next audioProcessorChanged callback below: we
            // just set the display ourselves (refresh(), above), so that
            // callback's resync-from-processor.getCurrentProgram() must be
            // skipped -- otherwise it would clobber a just-activated user
            // preset's name with whatever factory program index happens to be
            // embedded in the loaded state's presetIndex attribute.
            suppressNextAutoSync = true;
            processor.updateHostDisplay (juce::AudioProcessorListener::ChangeDetails{}.withProgramChanged (true));
        }

        // AudioProcessorListener — follow program changes NOT driven by this
        // controller itself (host automation of the program slot, another open
        // editor instance, A/B slot switching -- see loadStateFromSlot in
        // PluginProcessor.cpp). Those always mean "some factory program is now
        // current" from this controller's point of view (the host has no
        // knowledge of user presets), so drop any user-preset display and
        // re-sync fully from the processor.
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& details) override
        {
            if (! details.programChanged)
                return;

            // Snapshot-and-clear the guard now (this call is synchronous, on
            // whichever thread called updateHostDisplay) so a self-triggered
            // change is recognised correctly even if another notification
            // arrives before the async job below runs.
            const bool selfTriggered = suppressNextAutoSync;
            suppressNextAutoSync = false;

            // May arrive on any thread per the base contract; marshal to the
            // message thread. The SafePointer guards a deleted editor (which
            // owns this controller) from being called back into.
            juce::Component::SafePointer<juce::Component> safe (&parent);
            juce::MessageManager::callAsync ([this, safe, selfTriggered]
            {
                if (safe == nullptr || selfTriggered)
                    return;
                activeUserPresetName.clear();
                refresh();
            });
        }

        void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}

        juce::Component&      parent;
        juce::AudioProcessor& processor;
        PresetSelector        selectorView;
        factory_presets::UserPresetStore userStore;
        juce::StringArray     userNames;            // last refresh()'s userStore.list()
        juce::String          activeUserPresetName; // empty == a factory program is showing
        bool                  suppressNextAutoSync = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetSelectorController)
    };
}
