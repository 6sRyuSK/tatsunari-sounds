#pragma once

#include <juce_core/juce_core.h>

#include <memory>

//
// factory_presets::UserPresetStore — persists a plugin's full state XML as
// named user presets on disk, one .xml file per preset, so end users can save
// and recall their own configurations alongside the read-only factory bank
// (factory_ui::PresetSelectorController owns the UI half; see there for how a
// preset's XML is produced/applied via the processor's own
// getStateInformation/setStateInformation, the exact same shape a host uses).
//
// JUCE-core only (no GUI dependency) and message-thread only: every call does
// direct, synchronous file I/O, so never call this from the audio thread.
// There is no cache — list() re-enumerates the directory every time, per the
// plan's "no caching beyond what's needed" note.
//
// Layout: <userAppData>/TatsunariSounds/<PluginName>/Presets/<name>.xml — a
// flat directory per plugin, no further nesting. `name` doubles as the
// on-disk identity: save()/load()/remove()/exists() all resolve it through the
// same juce::File::createLegalFileName() sanitisation, so a name containing
// characters illegal in a filename (":", "/", ...) still round-trips
// consistently through this API even though the persisted filename can differ
// from what was typed (two names that sanitise to the same legal filename
// collide on disk — an accepted minor limitation, not handled specially).
//
// Every operation fails soft: a missing directory, an unwritable disk, a
// vanished-on-disk file, or unparsable XML never asserts/crashes, only
// returns false / nullptr for the caller to react to.
//
namespace factory_presets
{
    class UserPresetStore
    {
    public:
        // Real usage: resolves the directory from the plugin's display name
        // (itself sanitised too, since it becomes a path segment).
        explicit UserPresetStore (const juce::String& pluginName)
            : presetsDir (juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                              .getChildFile ("TatsunariSounds")
                              .getChildFile (juce::File::createLegalFileName (pluginName))
                              .getChildFile ("Presets"))
        {
        }

        // Testability: point directly at the presets directory to use (already
        // the final directory, no further nesting) — lets a test keep this
        // class's file I/O off the real userAppData (see preset_test.cpp).
        explicit UserPresetStore (const juce::File& presetsDirectoryOverride)
            : presetsDir (presetsDirectoryOverride)
        {
        }

        // Sorted (case-insensitive), re-enumerated every call. Empty — not an
        // error — if the directory doesn't exist yet (nothing saved so far).
        juce::StringArray list() const
        {
            juce::StringArray names;
            if (presetsDir.isDirectory())
                for (auto& f : presetsDir.findChildFiles (juce::File::findFiles, false, "*.xml"))
                    names.add (f.getFileNameWithoutExtension());
            names.sort (true);
            return names;
        }

        bool exists (const juce::String& name) const
        {
            const auto f = fileFor (name);
            return isValid (f) && f.existsAsFile();
        }

        // Creates the presets directory on demand; overwrite allowed. False on
        // an unusable name (sanitises to empty) or a write failure (unwritable
        // disk, directory could not be created, ...).
        bool save (const juce::String& name, const juce::XmlElement& xml) const
        {
            const auto f = fileFor (name);
            if (! isValid (f))
                return false;
            if (! presetsDir.isDirectory() && ! presetsDir.createDirectory().wasOk())
                return false;
            return xml.writeTo (f);
        }

        // nullptr on a missing file, an unusable name, or unparsable XML.
        std::unique_ptr<juce::XmlElement> load (const juce::String& name) const
        {
            const auto f = fileFor (name);
            if (! isValid (f) || ! f.existsAsFile())
                return nullptr;
            return juce::XmlDocument::parse (f);
        }

        // True (no-op) if the preset doesn't exist; false only on an actual
        // delete failure or an unusable name.
        bool remove (const juce::String& name) const
        {
            const auto f = fileFor (name);
            if (! isValid (f))
                return false;
            return ! f.existsAsFile() || f.deleteFile();
        }

    private:
        juce::File fileFor (const juce::String& name) const
        {
            const auto legal = juce::File::createLegalFileName (name.trim());
            return legal.isEmpty() ? juce::File() : presetsDir.getChildFile (legal + ".xml");
        }

        static bool isValid (const juce::File& f) { return f.getFullPathName().isNotEmpty(); }

        juce::File presetsDir;
    };
}
