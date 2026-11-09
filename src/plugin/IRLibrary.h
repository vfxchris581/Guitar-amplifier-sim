#pragma once

#include <juce_core/juce_core.h>

/** The user's impulse-response library: a folder of IR files plus a small JSON
    index holding each file's categories. Nothing is bundled; users import
    their own IRs (files are copied into the library folder). Message thread only. */
class IRLibrary
{
public:
    struct Entry
    {
        juce::File file;
        juce::StringArray tags;
        juce::String name() const { return file.getFileNameWithoutExtension(); }
    };

    static const juce::StringArray& categories(); // 1x12, 2x12, 4x12, Open-back, ...

    explicit IRLibrary (juce::File folder = defaultFolder());
    static juce::File defaultFolder();
    static inline juce::File folderOverride; // tests

    void rescan();
    const juce::Array<Entry>& entries() const { return all; }

    /** Entries matching all selected categories and the search text (name or tag). */
    juce::Array<Entry> filtered (const juce::StringArray& requiredTags, const juce::String& search) const;

    /** Copies files into the library. Returns the number imported; `skipped` lists failures. */
    int importFiles (const juce::Array<juce::File>& files, juce::StringArray& skipped);

    void setTags (const juce::File& file, const juce::StringArray& tags);
    juce::StringArray tagsFor (const juce::File& file) const;
    juce::File folder() const { return root; }

private:
    void loadIndex();
    void saveIndex() const;
    juce::String key (const juce::File& f) const { return f.getRelativePathFrom (root); }

    juce::File root;
    juce::Array<Entry> all;
    juce::NamedValueSet tagIndex; // relative path -> "tag1|tag2"
};
