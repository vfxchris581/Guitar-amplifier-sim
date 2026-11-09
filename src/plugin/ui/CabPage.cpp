#include "plugin/ui/CabPage.h"
#include "plugin/ui/Theme.h"

namespace ui
{
using namespace juce;

namespace
{
void caption (Label& l, const String& text)
{
    l.setText (text, dontSendNotification);
    l.setFont (FontOptions (12.0f, Font::bold));
    l.setColour (Label::textColourId, colours::textDim);
}
} // namespace

// ===========================================================================
class CabPage::MicStrip : public Component
{
public:
    MicStrip (CabPage& o, int index)
        : owner (o), mic (index),
          params (o.processor.getParameters()),
          level (params, ParamIDs::mic (index, "level"), "LEVEL", "Microphone level in the blend."),
          pan (params, ParamIDs::mic (index, "pan"), "PAN", "Position of this microphone in the stereo field."),
          delay (params, ParamIDs::mic (index, "delay"), "DELAY",
                 "Delays this microphone, like moving it further from the speaker. Use it to time-align or "
                 "deliberately comb-filter two mics."),
          hpf (params, ParamIDs::mic (index, "hpf"), "HIGH-PASS", "Removes low end below this frequency (20 Hz = off)."),
          lpf (params, ParamIDs::mic (index, "lpf"), "LOW-PASS", "Removes high end above this frequency (20 kHz = off)."),
          length (params, ParamIDs::mic (index, "length"), "IR LENGTH",
                  "Uses only the first part of the impulse response (shorter = tighter, less room)."),
          onAttachment (params, ParamIDs::mic (index, "on"), onButton),
          invertAttachment (params, ParamIDs::mic (index, "invert"), invertButton),
          normAttachment (params, ParamIDs::mic (index, "normalise"), normButton)
    {
        title.setText (index == 0 ? "MIC A" : "MIC B", dontSendNotification);
        title.setFont (FontOptions (15.0f, Font::bold));
        title.setColour (Label::textColourId, colours::accent);
        irName.setFont (FontOptions (16.0f, Font::bold));
        irInfo.setFont (FontOptions (12.5f));
        irInfo.setColour (Label::textColourId, colours::textDim);
        onButton.setButtonText ("On");
        invertButton.setButtonText (String::fromUTF8 ("Phase \xc3\x98"));
        invertButton.setTooltip ("Invert polarity - use it when two mics cancel each other's low end.");
        normButton.setButtonText ("Normalise");
        normButton.setTooltip ("Scale the IR to a standard level so different IRs play at similar loudness.");
        loadButton.onClick = [this] { browse(); };
        clearButton.onClick = [this] { owner.processor.clearIR (mic); refresh(); };
        for (Component* c : std::initializer_list<Component*> { &title, &irName, &irInfo, &onButton, &invertButton,
                                                                &normButton, &loadButton, &clearButton, &level, &pan,
                                                                &delay, &hpf, &lpf, &length })
            addAndMakeVisible (c);
        refresh();
    }

    void refresh()
    {
        auto& cab = owner.processor.getCab();
        const auto info = cab.getIRInfo (mic);
        const auto path = owner.processor.getIRPath (mic);
        if (cab.hasIR (mic))
        {
            irName.setText (info.name, dontSendNotification);
            irName.setColour (Label::textColourId, colours::text);
            irInfo.setText (String (info.channels == 2 ? "Stereo" : "Mono") + ", " + String (info.sampleRate / 1000.0, 1)
                                + " kHz, " + String (info.fileLengthMs, 0) + " ms (using " + String (info.usedLengthMs, 0) + " ms)",
                            dontSendNotification);
        }
        else if (path.isNotEmpty())
        {
            irName.setText ("IR missing", dontSendNotification);
            irName.setColour (Label::textColourId, colours::error);
            irInfo.setText (path, dontSendNotification);
        }
        else
        {
            irName.setText ("No IR loaded", dontSendNotification);
            irName.setColour (Label::textColourId, colours::textDim);
            irInfo.setText ("Load a WAV/AIFF impulse response, pick one in the library, or drop a file here.", dontSendNotification);
        }
        clearButton.setEnabled (cab.hasIR (mic));
    }

    void paint (Graphics& g) override { drawPanel (g, getLocalBounds().toFloat()); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 10);
        auto top = r.removeFromTop (26);
        title.setBounds (top.removeFromLeft (70));
        onButton.setBounds (top.removeFromRight (70));
        irName.setBounds (r.removeFromTop (24));
        irInfo.setBounds (r.removeFromTop (34));
        auto buttons = r.removeFromTop (30);
        loadButton.setBounds (buttons.removeFromLeft (110));
        buttons.removeFromLeft (8);
        clearButton.setBounds (buttons.removeFromLeft (70));
        r.removeFromTop (8);
        auto toggles = r.removeFromBottom (28);
        invertButton.setBounds (toggles.removeFromLeft (toggles.getWidth() / 2));
        normButton.setBounds (toggles);
        const int rowH = r.getHeight() / 2;
        for (int row = 0; row < 2; ++row)
        {
            auto line = r.removeFromTop (rowH);
            const int w = line.getWidth() / 3;
            Knob* knobs[2][3] = { { &level, &pan, &delay }, { &hpf, &lpf, &length } };
            for (int k = 0; k < 3; ++k)
                knobs[row][k]->setBounds (line.removeFromLeft (w).reduced (2));
        }
    }

private:
    void browse()
    {
        chooser = std::make_unique<FileChooser> ("Load impulse response", owner.library.folder().exists()
                                                                              ? owner.library.folder()
                                                                              : File::getSpecialLocation (File::userDocumentsDirectory),
                                                 "*.wav;*.aif;*.aiff");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                              [this] (const FileChooser& fc)
                              {
                                  if (fc.getResult().existsAsFile())
                                      owner.loadIntoMic (mic, fc.getResult());
                              });
    }

    CabPage& owner;
    int mic;
    AudioProcessorValueTreeState& params;
    Label title, irName, irInfo;
    ToggleButton onButton, invertButton, normButton;
    TextButton loadButton { "Load IR..." }, clearButton { "Clear" };
    Knob level, pan, delay, hpf, lpf, length;
    AudioProcessorValueTreeState::ButtonAttachment onAttachment, invertAttachment, normAttachment;
    std::unique_ptr<FileChooser> chooser;
};

// ===========================================================================
class CabPage::Browser : public Component,
                         private ListBoxModel
{
public:
    explicit Browser (CabPage& o) : owner (o)
    {
        heading.setText ("IR LIBRARY", dontSendNotification);
        heading.setFont (FontOptions (13.0f, Font::bold));
        heading.setColour (Label::textColourId, colours::textDim);
        search.setTextToShowWhenEmpty ("Search name or category", colours::textDim);
        search.setColour (TextEditor::backgroundColourId, colours::well);
        search.setColour (TextEditor::outlineColourId, colours::panelEdge);
        search.setColour (TextEditor::focusedOutlineColourId, colours::accent);
        search.onTextChange = [this] { update(); };
        for (const auto& c : IRLibrary::categories())
        {
            auto* b = chips.add (new TextButton (c));
            b->setClickingTogglesState (true);
            b->onClick = [this] { update(); };
            addAndMakeVisible (b);
        }
        list.setModel (this);
        list.setRowHeight (24);
        list.setColour (ListBox::backgroundColourId, colours::well);
        list.setColour (ListBox::outlineColourId, colours::panelEdge);
        list.setOutlineThickness (1);
        importButton.onClick = [this] { importIRs(); };
        loadA.onClick = [this] { loadSelected (0); };
        loadB.onClick = [this] { loadSelected (1); };
        tagsButton.onClick = [this] { editTags(); };
        importButton.setTooltip ("Copy your own IR files into the library (" + owner.library.folder().getFullPathName() + ").");
        tagsButton.setTooltip ("Set the categories of the selected IR.");
        for (Component* c : std::initializer_list<Component*> { &heading, &search, &list, &importButton, &loadA, &loadB, &tagsButton })
            addAndMakeVisible (c);
        update();
    }

    void update()
    {
        StringArray required;
        for (auto* c : chips)
            if (c->getToggleState())
                required.add (c->getButtonText());
        shown = owner.library.filtered (required, search.getText().trim());
        list.updateContent();
        list.repaint();
        const bool sel = list.getSelectedRow() >= 0 && list.getSelectedRow() < shown.size();
        loadA.setEnabled (sel);
        loadB.setEnabled (sel);
        tagsButton.setEnabled (sel);
    }

    void paint (Graphics& g) override
    {
        drawPanel (g, getLocalBounds().toFloat());
        if (owner.library.entries().isEmpty())
        {
            g.setColour (colours::textDim);
            g.setFont (FontOptions (13.0f));
            g.drawFittedText ("Your IR library is empty. Import your own cabinet impulse responses (WAV/AIFF) - "
                              "no third-party IRs are bundled.",
                              list.getBounds().reduced (12), Justification::centred, 4);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 10);
        heading.setBounds (r.removeFromTop (20));
        search.setBounds (r.removeFromTop (28));
        r.removeFromTop (6);
        // chips wrap in rows
        auto row = r.removeFromTop (24);
        for (auto* c : chips)
        {
            const int w = c->getButtonText().length() * 7 + 18;
            if (row.getWidth() < w)
            {
                r.removeFromTop (4);
                row = r.removeFromTop (24);
            }
            c->setBounds (row.removeFromLeft (w));
            row.removeFromLeft (4);
        }
        r.removeFromTop (8);
        auto buttons = r.removeFromBottom (28);
        importButton.setBounds (buttons.removeFromLeft (buttons.getWidth() / 4).withTrimmedRight (4));
        loadA.setBounds (buttons.removeFromLeft (buttons.getWidth() / 3).withTrimmedRight (4));
        loadB.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).withTrimmedRight (4));
        tagsButton.setBounds (buttons);
        r.removeFromBottom (6);
        list.setBounds (r);
    }

private:
    int getNumRows() override { return shown.size(); }

    void paintListBoxItem (int row, Graphics& g, int w, int h, bool selected) override
    {
        if (row < 0 || row >= shown.size())
            return;
        if (selected)
            g.fillAll (colours::accent.withAlpha (0.2f));
        const auto& e = shown.getReference (row);
        g.setColour (colours::text);
        g.setFont (FontOptions (13.5f));
        g.drawText (e.name(), 8, 0, w / 2, h, Justification::centredLeft);
        g.setColour (colours::textDim);
        g.setFont (FontOptions (11.5f));
        g.drawText (e.tags.joinIntoString (", "), w / 2, 0, w / 2 - 8, h, Justification::centredRight);
    }

    void selectedRowsChanged (int) override { update(); }
    void listBoxItemDoubleClicked (int row, const MouseEvent&) override
    {
        if (row >= 0 && row < shown.size())
            owner.loadIntoMic (0, shown[row].file);
    }

    void loadSelected (int mic)
    {
        const int row = list.getSelectedRow();
        if (row >= 0 && row < shown.size())
            owner.loadIntoMic (mic, shown[row].file);
    }

    void importIRs()
    {
        chooser = std::make_unique<FileChooser> ("Import impulse responses", File::getSpecialLocation (File::userHomeDirectory),
                                                 "*.wav;*.aif;*.aiff");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles
                                  | FileBrowserComponent::canSelectMultipleItems,
                              [this] (const FileChooser& fc)
                              {
                                  StringArray skipped;
                                  owner.library.importFiles (fc.getResults(), skipped);
                                  if (! skipped.isEmpty())
                                      AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Some files were not imported",
                                                                        skipped.joinIntoString ("\n"));
                                  update();
                                  repaint();
                              });
    }

    void editTags()
    {
        const int row = list.getSelectedRow();
        if (row < 0 || row >= shown.size())
            return;
        const auto file = shown[row].file;
        const auto current = owner.library.tagsFor (file);
        PopupMenu menu;
        int id = 1;
        for (const auto& c : IRLibrary::categories())
            menu.addItem (id++, c, true, current.contains (c));
        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&tagsButton),
                            [this, file, current] (int result)
                            {
                                if (result <= 0)
                                    return;
                                auto tags = current;
                                const auto c = IRLibrary::categories()[result - 1];
                                if (tags.contains (c))
                                    tags.removeString (c);
                                else
                                    tags.add (c);
                                owner.library.setTags (file, tags);
                                update();
                                editTags(); // keep the menu open for more ticks
                            });
    }

    CabPage& owner;
    Label heading;
    TextEditor search;
    OwnedArray<TextButton> chips;
    ListBox list;
    TextButton importButton { "Import..." }, loadA { "To Mic A" }, loadB { "To Mic B" }, tagsButton { "Categories..." };
    Array<IRLibrary::Entry> shown;
    std::unique_ptr<FileChooser> chooser;
};

// ===========================================================================
CabPage::CabPage (GuitarAmpProcessor& p)
    : processor (p), modeAttachment (p.getParameters(), ParamIDs::cabMode, modeBox)
{
    modeBox.addItemList ({ "Auto", "On", "Off" }, 1);
    modeBox.setSelectedItemIndex ((int) *p.getParameters().getRawParameterValue (ParamIDs::cabMode), dontSendNotification);
    modeBox.setTooltip ("Auto: on, except when the loaded amp capture already includes a cabinet (prevents double cab).\n"
                        "On: always on.  Off: always off.");
    caption (modeLabel, "CABINET");
    statusLabel.setFont (FontOptions (13.5f));
    addAndMakeVisible (modeBox);
    addAndMakeVisible (modeLabel);
    addAndMakeVisible (statusLabel);
    for (int m = 0; m < 2; ++m)
    {
        strips[m] = std::make_unique<MicStrip> (*this, m);
        addAndMakeVisible (*strips[m]);
    }
    browser = std::make_unique<Browser> (*this);
    addAndMakeVisible (*browser);
    startTimerHz (4);
}

CabPage::~CabPage() { stopTimer(); }

void CabPage::loadIntoMic (int mic, const File& file)
{
    String err;
    if (! processor.loadIR (mic, file, err))
        AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Impulse response", err);
    strips[mic]->refresh();
}

void CabPage::timerCallback()
{
    String why;
    const bool active = processor.isCabActive (&why);
    statusLabel.setText (why, dontSendNotification);
    statusLabel.setColour (Label::textColourId, active ? colours::ok : colours::warning);
    for (auto& s : strips)
        s->refresh();
}

void CabPage::paint (Graphics&) {}

void CabPage::resized()
{
    auto r = getLocalBounds().reduced (14);
    auto top = r.removeFromTop (30);
    modeLabel.setBounds (top.removeFromLeft (80));
    modeBox.setBounds (top.removeFromLeft (110));
    top.removeFromLeft (14);
    statusLabel.setBounds (top);
    r.removeFromTop (10);
    browser->setBounds (r.removeFromRight (jmax (320, r.getWidth() / 3)));
    r.removeFromRight (12);
    const int w = (r.getWidth() - 12) / 2;
    strips[0]->setBounds (r.removeFromLeft (w));
    r.removeFromLeft (12);
    strips[1]->setBounds (r);
}

bool CabPage::isInterestedInFileDrag (const StringArray& files)
{
    for (const auto& f : files)
        if (! File (f).hasFileExtension ("wav;aif;aiff"))
            return false;
    return ! files.isEmpty();
}

void CabPage::filesDropped (const StringArray& files, int x, int)
{
    // Dropped on a mic strip: load it there. Elsewhere: import into the library.
    for (int m = 0; m < 2; ++m)
        if (strips[m]->getBounds().contains (x, strips[m]->getY() + 1) && files.size() == 1)
        {
            loadIntoMic (m, File (files[0]));
            return;
        }
    Array<File> list;
    for (const auto& f : files)
        list.add (File (f));
    StringArray skipped;
    library.importFiles (list, skipped);
    browser->update();
    browser->repaint();
}

void CabPage::refreshBrowser() { browser->update(); }
} // namespace ui
