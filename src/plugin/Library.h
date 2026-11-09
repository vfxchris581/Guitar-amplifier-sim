#pragma once

#include <juce_core/juce_core.h>

/** The user's NAM model library: every .nam under the Models and Captures folders,
    plus a JSON index for what the files themselves can't hold (display name,
    favourite, collections, tags, description). Metadata shown comes from the files. */
class ModelLibrary
{
public:
    struct Entry
    {
        juce::File file;
        juce::String fileName, displayName;
        // from the file (empty when the file doesn't say)
        juce::String name, modeledBy, gearMake, gearModel, gearType, toneType, architecture;
        double sampleRate = 0.0;
        double validationEsr = -1.0;
        // user data from the index
        bool favourite = false;
        juce::StringArray collections, tags;
        juce::String description;
        bool ownCapture = false; // made in this plugin's CAPTURE section
    };

    struct Filter
    {
        juce::String search, gearType, toneType, collection;
        bool favouritesOnly = false;
    };

    ModelLibrary();
    static juce::File modelsFolder();
    static juce::File capturesFolder();
    static inline juce::File rootOverride; // tests

    void rescan();
    const juce::Array<Entry>& entries() const { return all; }
    juce::Array<Entry> filtered (const Filter&) const;
    juce::StringArray allCollections() const;
    const Entry* find (const juce::File&) const;

    int importFiles (const juce::Array<juce::File>&, juce::StringArray& problems);
    bool remove (const juce::File&, juce::String& error);            // deletes the file
    bool exportTo (const juce::File& model, const juce::File& destination, juce::String& error);

    void setDisplayName (const juce::File&, const juce::String&);
    void setFavourite (const juce::File&, bool);
    void setCollections (const juce::File&, const juce::StringArray&);
    void setTags (const juce::File&, const juce::StringArray&);
    void setDescription (const juce::File&, const juce::String&);

private:
    static juce::File root();
    juce::var& record (const juce::File&);
    void save() const;
    static Entry readFile (const juce::File&);

    juce::Array<Entry> all;
    juce::var index; // object: path -> { displayName, favourite, collections, tags, description }
};

/** Presets: the complete plugin state (amp model, capture-derived settings, cab IRs
    and mics, every effect, the chain order, levels) as files. */
class PresetLibrary
{
public:
    struct Preset
    {
        juce::File file;
        juce::String name() const { return file.getFileNameWithoutExtension(); }
        bool favourite = false;
    };
    static constexpr const char* extension = ".gaspreset";

    PresetLibrary();
    static juce::File folder();
    static inline juce::File folderOverride; // tests

    void rescan();
    const juce::Array<Preset>& presets() const { return all; }

    juce::File save (const juce::String& name, const juce::MemoryBlock& state, juce::String& error);
    bool load (const juce::File&, juce::MemoryBlock& state, juce::String& error) const;
    bool rename (const juce::File&, const juce::String& newName, juce::String& error);
    bool remove (const juce::File&);
    void setFavourite (const juce::File&, bool);
    int importFiles (const juce::Array<juce::File>&);

private:
    juce::Array<Preset> all;
    juce::StringArray favourites;
};
