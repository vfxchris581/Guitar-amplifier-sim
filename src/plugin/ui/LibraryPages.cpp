#include "plugin/ui/LibraryPages.h"
#include "plugin/IRLibrary.h"
#include "plugin/ui/Theme.h"

#include <BinaryData.h>

namespace ui
{
using namespace juce;

namespace
{
void editorStyle (TextEditor& e, const String& placeholder)
{
    e.setTextToShowWhenEmpty (placeholder, colours::textDim);
    e.setColour (TextEditor::backgroundColourId, colours::well);
    e.setColour (TextEditor::outlineColourId, colours::panelEdge);
    e.setColour (TextEditor::focusedOutlineColourId, colours::accent);
    e.setFont (FontOptions (13.5f));
}
String pretty (const String& s) { return s.isEmpty() ? s : (s == "hi_gain" ? "Hi-gain" : s.replace ("_", " + ").substring (0, 1).toUpperCase() + s.replace ("_", " + ").substring (1)); }
StringArray splitList (const String& s) { StringArray a; a.addTokens (s, ",", {}); a.trim(); a.removeEmptyStrings(); a.removeDuplicates (true); return a; }
} // namespace

// ===========================================================================
class LibraryPage::PresetPanel : public Component, private ListBoxModel
{
public:
    PresetPanel (GuitarAmpProcessor& p, std::function<void()> loaded) : processor (p), onLoaded (std::move (loaded))
    {
        editorStyle (nameEdit, "Preset name");
        list.setModel (this);
        list.setRowHeight (24);
        list.setColour (ListBox::backgroundColourId, colours::well);
        saveButton.onClick = [this]
        {
            MemoryBlock state;
            processor.getStateInformation (state);
            String err;
            auto name = nameEdit.getText().trim();
            if (name.isEmpty() && selectedRow() >= 0)
                name = library.presets()[selectedRow()].name();
            if (library.save (name, state, err) == File())
                AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Preset", err);
            list.updateContent();
            list.repaint();
        };
        loadButton.onClick = [this] { loadSelected(); };
        renameButton.onClick = [this]
        {
            const int r = selectedRow();
            String err;
            if (r >= 0 && nameEdit.getText().trim().isNotEmpty() && ! library.rename (library.presets()[r].file, nameEdit.getText(), err))
                AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Preset", err);
            list.updateContent();
        };
        deleteButton.onClick = [this]
        {
            const int r = selectedRow();
            if (r < 0) return;
            const auto f = library.presets()[r].file;
            AlertWindow::showOkCancelBox (MessageBoxIconType::QuestionIcon, "Delete preset", "Delete \"" + f.getFileNameWithoutExtension() + "\"?", "Delete", "Cancel", nullptr,
                                          ModalCallbackFunction::create ([this, f] (int ok) { if (ok) { library.remove (f); list.updateContent(); list.repaint(); } }));
        };
        favButton.onClick = [this]
        {
            const int r = selectedRow();
            if (r >= 0) library.setFavourite (library.presets()[r].file, ! library.presets()[r].favourite);
            list.repaint();
        };
        importButton.onClick = [this]
        {
            chooser = std::make_unique<FileChooser> ("Import presets", File::getSpecialLocation (File::userHomeDirectory), String ("*") + PresetLibrary::extension);
            chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::canSelectMultipleItems,
                                  [this] (const FileChooser& fc) { library.importFiles (fc.getResults()); list.updateContent(); list.repaint(); });
        };
        exportButton.onClick = [this]
        {
            const int r = selectedRow();
            if (r < 0) return;
            const auto src = library.presets()[r].file;
            chooser = std::make_unique<FileChooser> ("Export preset", File::getSpecialLocation (File::userDesktopDirectory).getChildFile (src.getFileName()),
                                                     String ("*") + PresetLibrary::extension);
            chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::warnAboutOverwriting,
                                  [src] (const FileChooser& fc) { if (fc.getResult() != File()) src.copyFileTo (fc.getResult()); });
        };
        favButton.setButtonText (String::fromUTF8 ("\xe2\x98\x85"));
        favButton.setTooltip ("Favourite");
        heading.setText ("PRESETS", dontSendNotification);
        heading.setFont (FontOptions (13.0f, Font::bold));
        heading.setColour (Label::textColourId, colours::textDim);
        note.setText ("A preset stores the amp model, cab IRs and mics, every effect, the chain order and the levels. "
                      "Missing files are reported, never silently replaced.", dontSendNotification);
        note.setFont (FontOptions (12.0f));
        note.setColour (Label::textColourId, colours::textDim);
        for (Component* c : std::initializer_list<Component*> { &heading, &note, &nameEdit, &list, &saveButton, &loadButton, &renameButton,
                                                                &deleteButton, &favButton, &importButton, &exportButton })
            addAndMakeVisible (c);
    }

    void refresh() { library.rescan(); list.updateContent(); list.repaint(); }

    void paint (Graphics& g) override { drawPanel (g, getLocalBounds().toFloat()); }
    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 10);
        heading.setBounds (r.removeFromTop (20));
        auto row = r.removeFromTop (28);
        nameEdit.setBounds (row.removeFromLeft (row.getWidth() - 160));
        row.removeFromLeft (6);
        saveButton.setBounds (row.removeFromLeft (70));
        row.removeFromLeft (6);
        loadButton.setBounds (row);
        r.removeFromTop (6);
        note.setBounds (r.removeFromBottom (32));
        auto buttons = r.removeFromBottom (28);
        const int w = buttons.getWidth() / 5;
        for (auto* b : { &renameButton, &deleteButton, &favButton, &importButton, &exportButton })
            b->setBounds (buttons.removeFromLeft (w).withTrimmedRight (4));
        r.removeFromBottom (6);
        list.setBounds (r);
    }

private:
    int selectedRow() const { const int r = list.getSelectedRow(); return r >= 0 && r < library.presets().size() ? r : -1; }
    int getNumRows() override { return library.presets().size(); }
    void paintListBoxItem (int row, Graphics& g, int w, int h, bool sel) override
    {
        if (row >= library.presets().size()) return;
        if (sel) g.fillAll (colours::accent.withAlpha (0.2f));
        const auto& p = library.presets()[row];
        g.setColour (p.favourite ? colours::accent : colours::panelEdge);
        g.drawText (String::fromUTF8 ("\xe2\x98\x85"), 6, 0, 18, h, Justification::centred);
        g.setColour (colours::text);
        g.setFont (FontOptions (13.5f));
        g.drawText (p.name(), 28, 0, w - 32, h, Justification::centredLeft);
    }
    void selectedRowsChanged (int r) override { if (r >= 0 && r < library.presets().size()) nameEdit.setText (library.presets()[r].name(), false); }
    void listBoxItemDoubleClicked (int, const MouseEvent&) override { loadSelected(); }
    void loadSelected()
    {
        const int r = selectedRow();
        if (r < 0) return;
        MemoryBlock state;
        String err;
        if (! library.load (library.presets()[r].file, state, err))
        {
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Preset", err);
            return;
        }
        processor.setStateInformation (state.getData(), (int) state.getSize());
        if (onLoaded) onLoaded();
    }

    GuitarAmpProcessor& processor;
    std::function<void()> onLoaded;
    PresetLibrary library;
    Label heading, note;
    TextEditor nameEdit;
    ListBox list;
    TextButton saveButton { "Save" }, loadButton { "Load" }, renameButton { "Rename" }, deleteButton { "Delete" }, favButton,
        importButton { "Import..." }, exportButton { "Export..." };
    std::unique_ptr<FileChooser> chooser;
};

// ===========================================================================
LibraryPage::LibraryPage (GuitarAmpProcessor& p, std::function<void()> onPresetLoaded) : processor (p)
{
    editorStyle (search, "Search models: name, amp, author, tag, description");
    editorStyle (displayName, "Display name");
    editorStyle (description, "Description / notes");
    editorStyle (tags, "Tags, comma separated");
    editorStyle (collections, "Collections, comma separated");
    description.setMultiLine (true, true);
    description.setReturnKeyStartsNewLine (true);
    search.onTextChange = [this] { refreshModels(); };

    gearFilter.addItem ("All gear", 1);
    for (auto t : { "amp", "amp_cab", "pedal", "pedal_amp", "amp_pedal_cab", "preamp", "studio" })
        gearFilter.addItem (pretty (t), gearFilter.getNumItems() + 1);
    toneFilter.addItem ("All tones", 1);
    for (auto t : { "clean", "crunch", "overdrive", "hi_gain", "fuzz" })
        toneFilter.addItem (pretty (t), toneFilter.getNumItems() + 1);
    gearFilter.setSelectedId (1, dontSendNotification);
    toneFilter.setSelectedId (1, dontSendNotification);
    for (auto* c : { &gearFilter, &toneFilter, &collectionFilter })
        c->onChange = [this] { refreshModels(); };
    favouritesOnly.onClick = [this] { refreshModels(); };

    list.setModel (this);
    list.setRowHeight (26);
    list.setColour (ListBox::backgroundColourId, colours::well);

    detailTitle.setFont (FontOptions (18.0f, Font::bold));
    detailInfo.setFont (FontOptions (12.5f));
    detailInfo.setColour (Label::textColourId, colours::textDim);
    detailInfo.setJustificationType (Justification::topLeft);
    favButton.setButtonText (String::fromUTF8 ("\xe2\x98\x85 Favourite"));
    favButton.setClickingTogglesState (false);

    importButton.onClick = [this]
    {
        chooser = std::make_unique<FileChooser> ("Import NAM models", File::getSpecialLocation (File::userHomeDirectory), "*.nam");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::canSelectMultipleItems,
                              [this] (const FileChooser& fc)
                              {
                                  StringArray problems;
                                  models.importFiles (fc.getResults(), problems);
                                  if (! problems.isEmpty())
                                      AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Import", problems.joinIntoString ("\n"));
                                  refreshModels();
                              });
    };
    loadButton.onClick = [this] { if (auto* e = selected()) processor.loadModel (e->file); };
    compareButton.setTooltip ("A = the model playing now, B = the selected model. Then switch with A / B (seamless crossfade).");
    compareButton.onClick = [this]
    {
        if (auto* e = selected())
        {
            compareA = File (String (processor.getEngine().requestedModelPath()));
            compareB = e->file;
            playingB = false;
            abButton.setEnabled (compareA.existsAsFile());
            abButton.setButtonText ("A / B  (A playing)");
        }
    };
    abButton.setEnabled (false);
    abButton.onClick = [this]
    {
        playingB = ! playingB;
        processor.loadModel (playingB ? compareB : compareA);
        abButton.setButtonText (playingB ? "A / B  (B playing)" : "A / B  (A playing)");
    };
    favButton.onClick = [this] { if (auto* e = selected()) { models.setFavourite (e->file, ! e->favourite); refreshModels(); } };
    saveDetails.onClick = [this]
    {
        auto* e = selected();
        if (e == nullptr) return;
        const auto f = e->file;
        models.setDisplayName (f, displayName.getText().trim());
        models.setDescription (f, description.getText());
        models.setTags (f, splitList (tags.getText()));
        models.setCollections (f, splitList (collections.getText()));
        refreshModels();
    };
    deleteButton.onClick = [this]
    {
        auto* e = selected();
        if (e == nullptr) return;
        const auto f = e->file;
        AlertWindow::showOkCancelBox (MessageBoxIconType::WarningIcon, "Delete model",
                                      "Delete \"" + f.getFileName() + "\" from your library? The file is removed from disk.", "Delete", "Cancel", nullptr,
                                      ModalCallbackFunction::create ([this, f] (int ok)
                                      {
                                          String err;
                                          if (ok && ! models.remove (f, err))
                                              AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Delete", err);
                                          refreshModels();
                                      }));
    };
    exportButton.onClick = [this]
    {
        auto* e = selected();
        if (e == nullptr) return;
        const auto src = e->file;
        auto doExport = [this, src]
        {
            chooser = std::make_unique<FileChooser> ("Export model", File::getSpecialLocation (File::userDesktopDirectory).getChildFile (src.getFileName()), "*.nam");
            chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::warnAboutOverwriting,
                                  [this, src] (const FileChooser& fc)
                                  {
                                      String err;
                                      if (fc.getResult() != File() && ! models.exportTo (src, fc.getResult(), err))
                                          AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Export", err);
                                  });
        };
        if (e->ownCapture)
            doExport();
        else // third-party models usually may not be redistributed (e.g. TONE3000's T3K licence)
            AlertWindow::showOkCancelBox (MessageBoxIconType::WarningIcon, "Export a model you didn't capture?",
                                          "This model was imported, not captured here. Many model licences forbid redistributing "
                                          "the file without the author's permission. Only export it if you have that right.",
                                          "I have the right", "Cancel", nullptr,
                                          ModalCallbackFunction::create ([doExport] (int ok) { if (ok) doExport(); }));
    };

    presets = std::make_unique<PresetPanel> (p, std::move (onPresetLoaded));
    for (Component* c : std::initializer_list<Component*> { &search, &gearFilter, &toneFilter, &collectionFilter, &favouritesOnly, &list,
                                                            &importButton, &detailTitle, &detailInfo, &displayName, &description, &tags,
                                                            &collections, &loadButton, &compareButton, &abButton, &favButton, &saveDetails,
                                                            &deleteButton, &exportButton, presets.get() })
        addAndMakeVisible (c);
    refreshModels();
}

LibraryPage::~LibraryPage() = default;

void LibraryPage::visibilityChanged()
{
    if (isVisible())
    {
        models.rescan(); // pick up new captures
        refreshModels();
        presets->refresh();
    }
}

const ModelLibrary::Entry* LibraryPage::selected() const
{
    const int r = list.getSelectedRow();
    return r >= 0 && r < shown.size() ? models.find (shown[r].file) : nullptr;
}

void LibraryPage::refreshModels()
{
    const auto keepFile = selected() != nullptr ? selected()->file : File();
    const auto currentCollection = collectionFilter.getText();
    collectionFilter.clear (dontSendNotification);
    collectionFilter.addItem ("All collections", 1);
    for (const auto& c : models.allCollections())
        collectionFilter.addItem (c, collectionFilter.getNumItems() + 1);
    collectionFilter.setText (currentCollection.isEmpty() ? "All collections" : currentCollection, dontSendNotification);

    static const char* gear[] = { "", "amp", "amp_cab", "pedal", "pedal_amp", "amp_pedal_cab", "preamp", "studio" };
    static const char* tone[] = { "", "clean", "crunch", "overdrive", "hi_gain", "fuzz" };
    ModelLibrary::Filter f;
    f.search = search.getText().trim();
    f.gearType = gear[jlimit (0, 7, gearFilter.getSelectedId() - 1)];
    f.toneType = tone[jlimit (0, 5, toneFilter.getSelectedId() - 1)];
    f.collection = collectionFilter.getSelectedId() > 1 ? collectionFilter.getText() : String();
    f.favouritesOnly = favouritesOnly.getToggleState();
    shown = models.filtered (f);
    list.updateContent();
    for (int i = 0; i < shown.size(); ++i)
        if (shown[i].file == keepFile)
            list.selectRow (i, false, true);
    list.repaint();
    showDetails();
}

void LibraryPage::showDetails()
{
    const auto* e = selected();
    for (Component* c : std::initializer_list<Component*> { &loadButton, &compareButton, &favButton, &saveDetails, &deleteButton, &exportButton,
                                                            &displayName, &description, &tags, &collections })
        c->setEnabled (e != nullptr);
    if (e == nullptr)
    {
        detailTitle.setText (models.entries().isEmpty() ? "Your model library is empty" : "Select a model", dontSendNotification);
        detailInfo.setText (models.entries().isEmpty() ? "Import .nam files, or make your own in CAPTURE. Library folder: "
                                                             + ModelLibrary::modelsFolder().getFullPathName()
                                                       : String(), dontSendNotification);
        for (auto* t : { &displayName, &description, &tags, &collections })
            t->clear();
        return;
    }
    detailTitle.setText ((e->favourite ? String::fromUTF8 ("\xe2\x98\x85 ") : String()) + e->displayName, dontSendNotification);
    StringArray lines;
    auto add = [&lines] (const String& k, const String& v) { if (v.isNotEmpty()) lines.add (k + ": " + v); };
    add ("Amplifier", (e->gearMake + " " + e->gearModel).trim());
    add ("Gear", pretty (e->gearType));
    add ("Tone", pretty (e->toneType));
    add ("Captured by", e->modeledBy);
    add ("Architecture", e->architecture);
    add ("Sample rate", e->sampleRate > 0 ? String (e->sampleRate / 1000.0, 1) + " kHz" : String ("not stored (48 kHz assumed)"));
    if (e->validationEsr >= 0) add ("Validation ESR", String (e->validationEsr, 4));
    add ("Source", e->ownCapture ? String ("captured in this plugin") : String ("imported"));
    add ("File", e->file.getFileName());
    detailInfo.setText (lines.joinIntoString ("\n"), dontSendNotification);
    displayName.setText (e->displayName, false);
    description.setText (e->description, false);
    tags.setText (e->tags.joinIntoString (", "), false);
    collections.setText (e->collections.joinIntoString (", "), false);
    favButton.setButtonText (e->favourite ? String::fromUTF8 ("\xe2\x98\x85 Unfavourite") : String::fromUTF8 ("\xe2\x98\x86 Favourite"));
}

int LibraryPage::getNumRows() { return shown.size(); }

void LibraryPage::paintListBoxItem (int row, Graphics& g, int w, int h, bool sel)
{
    if (row >= shown.size()) return;
    const auto& e = shown.getReference (row);
    if (sel) g.fillAll (colours::accent.withAlpha (0.2f));
    g.setColour (e.favourite ? colours::accent : colours::panelEdge);
    g.drawText (String::fromUTF8 ("\xe2\x98\x85"), 4, 0, 18, h, Justification::centred);
    g.setColour (colours::text);
    g.setFont (FontOptions (13.5f));
    g.drawText (e.displayName, 26, 0, w / 2, h, Justification::centredLeft);
    g.setColour (colours::textDim);
    g.setFont (FontOptions (11.5f));
    StringArray info;
    if (e.gearModel.isNotEmpty()) info.add (e.gearModel);
    if (e.toneType.isNotEmpty()) info.add (pretty (e.toneType));
    if (e.ownCapture) info.add ("my capture");
    g.drawText (info.joinIntoString ("  |  "), w / 2, 0, w / 2 - 6, h, Justification::centredRight);
}

void LibraryPage::selectedRowsChanged (int) { showDetails(); }
void LibraryPage::listBoxItemDoubleClicked (int, const MouseEvent&) { if (auto* e = selected()) processor.loadModel (e->file); }

void LibraryPage::paint (Graphics& g)
{
    drawPanel (g, modelArea.toFloat(), "Models");
    drawPanel (g, detailArea.toFloat());
}

void LibraryPage::resized()
{
    auto r = getLocalBounds().reduced (14);
    modelArea = r.removeFromLeft (r.getWidth() * 45 / 100);
    r.removeFromLeft (12);
    detailArea = r.removeFromTop (r.getHeight() * 58 / 100);
    r.removeFromTop (12);
    presets->setBounds (r);

    auto m = modelArea.reduced (12, 10).withTrimmedTop (22);
    search.setBounds (m.removeFromTop (28));
    m.removeFromTop (6);
    auto filters = m.removeFromTop (26);
    const int fw = (filters.getWidth() - 100) / 3;
    gearFilter.setBounds (filters.removeFromLeft (fw).withTrimmedRight (4));
    toneFilter.setBounds (filters.removeFromLeft (fw).withTrimmedRight (4));
    collectionFilter.setBounds (filters.removeFromLeft (fw).withTrimmedRight (4));
    favouritesOnly.setBounds (filters);
    m.removeFromTop (6);
    importButton.setBounds (m.removeFromBottom (28).removeFromLeft (120));
    m.removeFromBottom (6);
    list.setBounds (m);

    auto d = detailArea.reduced (12, 10);
    detailTitle.setBounds (d.removeFromTop (26));
    auto row = d.removeFromBottom (28);
    const int bw = row.getWidth() / 4;
    for (auto* b : { &loadButton, &compareButton, &abButton, &exportButton })
        b->setBounds (row.removeFromLeft (bw).withTrimmedRight (4));
    d.removeFromBottom (6);
    auto left = d.removeFromLeft (d.getWidth() / 2);
    detailInfo.setBounds (left.withTrimmedRight (8));
    displayName.setBounds (d.removeFromTop (26));
    d.removeFromTop (4);
    tags.setBounds (d.removeFromTop (26));
    d.removeFromTop (4);
    collections.setBounds (d.removeFromTop (26));
    d.removeFromTop (4);
    auto actions = d.removeFromBottom (26);
    favButton.setBounds (actions.removeFromLeft (actions.getWidth() / 3).withTrimmedRight (4));
    saveDetails.setBounds (actions.removeFromLeft (actions.getWidth() / 2).withTrimmedRight (4));
    deleteButton.setBounds (actions);
    d.removeFromBottom (4);
    description.setBounds (d);
}

// ===========================================================================
SettingsPage::SettingsPage (GuitarAmpProcessor& p) : processor (p)
{
    const std::pair<String, File> folders[] = {
        { "Models", ModelLibrary::modelsFolder() },
        { "Captures", ModelLibrary::capturesFolder() },
        { "Impulse responses", IRLibrary::defaultFolder() },
        { "Presets", PresetLibrary::folder() },
    };
    for (const auto& [name, folder] : folders)
    {
        auto* l = folderLabels.add (new Label());
        l->setText (name + ":  " + folder.getFullPathName(), dontSendNotification);
        l->setFont (FontOptions (13.5f));
        addAndMakeVisible (l);
        auto* b = showButtons.add (new TextButton ("Show"));
        b->onClick = [folder] { folder.createDirectory(); folder.revealToUser(); };
        addAndMakeVisible (b);
    }
    hostInfo.setFont (FontOptions (13.5f));
    hostInfo.setJustificationType (Justification::topLeft);
    addAndMakeVisible (hostInfo);
    notices.setMultiLine (true);
    notices.setReadOnly (true);
    notices.setFont (FontOptions (12.5f));
    notices.setColour (TextEditor::backgroundColourId, colours::well);
    notices.setText (String::fromUTF8 (BinaryData::THIRD_PARTY_NOTICES_md, BinaryData::THIRD_PARTY_NOTICES_mdSize), false);
    addAndMakeVisible (notices);
    timerCallback();
    startTimerHz (2);
}

void SettingsPage::timerCallback()
{
    String s;
    s << "Audio interface, sample rate and buffer size are set in your DAW: a VST3 plugin uses the host's audio device. "
      << "Route the plugin's inputs/outputs in the DAW (for CAPTURE: send and return channels are chosen on its first step).\n\n"
      << "Now: " << String (processor.getHostSampleRate() / 1000.0, 1) << " kHz, block " << processor.getBlockSize() << " samples, "
      << processor.getTotalNumInputChannels() << " in / " << processor.getTotalNumOutputChannels() << " out, latency "
      << processor.getLatencySamples() << " samples.\n"
      << GAS_PRODUCT_NAME << " " << GAS_VERSION_STRING << ", NeuralAmpModelerCore 0.6.0, JUCE 9.0.5.";
    hostInfo.setText (s, dontSendNotification);
}

void SettingsPage::paint (Graphics& g)
{
    auto r = getLocalBounds().reduced (14);
    drawPanel (g, r.removeFromTop (190).toFloat(), "Folders");
    r.removeFromTop (12);
    drawPanel (g, r.removeFromTop (130).toFloat(), "Audio & host");
    r.removeFromTop (12);
    drawPanel (g, r.toFloat(), "Third-party software");
}

void SettingsPage::resized()
{
    auto r = getLocalBounds().reduced (14);
    auto f = r.removeFromTop (190).reduced (14, 10).withTrimmedTop (24);
    for (int i = 0; i < folderLabels.size(); ++i)
    {
        auto row = f.removeFromTop (36);
        showButtons[i]->setBounds (row.removeFromRight (80).reduced (0, 4));
        folderLabels[i]->setBounds (row);
    }
    r.removeFromTop (12);
    hostInfo.setBounds (r.removeFromTop (130).reduced (14, 10).withTrimmedTop (22));
    r.removeFromTop (12);
    notices.setBounds (r.reduced (14, 10).withTrimmedTop (22));
}
} // namespace ui
