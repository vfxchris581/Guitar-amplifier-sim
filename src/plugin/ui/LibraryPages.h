#pragma once

#include "plugin/Library.h"
#include "plugin/PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace ui
{
/** LIBRARY: the user's NAM models (search, filter, favourites, collections, tags,
    descriptions, rename, delete, import, export, preview, A/B compare) and presets. */
class LibraryPage : public juce::Component,
                    private juce::ListBoxModel
{
public:
    LibraryPage (GuitarAmpProcessor&, std::function<void()> onPresetLoaded);
    ~LibraryPage() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;

private:
    int getNumRows() override;
    void paintListBoxItem (int, juce::Graphics&, int, int, bool) override;
    void selectedRowsChanged (int) override;
    void listBoxItemDoubleClicked (int, const juce::MouseEvent&) override;

    void refreshModels();
    void showDetails();
    const ModelLibrary::Entry* selected() const;

    class PresetPanel;

    GuitarAmpProcessor& processor;
    ModelLibrary models;
    juce::Array<ModelLibrary::Entry> shown;

    juce::TextEditor search, displayName, description, tags, collections;
    juce::ComboBox gearFilter, toneFilter, collectionFilter;
    juce::ToggleButton favouritesOnly { "Favourites" };
    juce::ListBox list;
    juce::Label detailTitle, detailInfo;
    juce::TextButton importButton { "Import..." }, loadButton { "Load into AMP" }, compareButton { "Compare with current" },
        abButton { "A / B" }, favButton, saveDetails { "Save details" }, deleteButton { "Delete" }, exportButton { "Export..." };
    juce::File compareA, compareB;
    bool playingB = false;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<PresetPanel> presets;
    juce::Rectangle<int> modelArea, detailArea;
};

/** SETTINGS: where things live, how the plugin sits in the host, and licences. */
class SettingsPage : public juce::Component,
                     private juce::Timer
{
public:
    explicit SettingsPage (GuitarAmpProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    GuitarAmpProcessor& processor;
    juce::OwnedArray<juce::TextButton> showButtons;
    juce::OwnedArray<juce::Label> folderLabels;
    juce::Label hostInfo;
    juce::TextEditor notices;
};
} // namespace ui
