#include "plugin/ui/AmpPage.h"
#include "plugin/ui/Theme.h"

namespace ui
{
using namespace juce;

namespace
{
String prettyKey (const std::string& s)
{
    // "hi_gain" -> "Hi gain", "amp_cab" -> "Amp + cab"
    auto t = String (s).replace ("_", " + ");
    if (s == "hi_gain")
        t = "Hi-gain";
    return t.substring (0, 1).toUpperCase() + t.substring (1);
}

String fmt (double v, int decimals, const String& unit) { return String (v, decimals) + unit; }
} // namespace

// ---------------------------------------------------------------------------
Knob::Knob (AudioProcessorValueTreeState& state, const String& paramId, const String& caption, const String& tooltip)
    : attachment (state, paramId, slider)
{
    slider.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (Slider::TextBoxBelow, false, 80, 18);
    slider.setRotaryParameters (MathConstants<float>::pi * 1.25f, MathConstants<float>::pi * 2.75f, true);
    slider.setTooltip (tooltip);
    slider.setDoubleClickReturnValue (true, (double) state.getParameter (paramId)->convertFrom0to1 (
                                                state.getParameter (paramId)->getDefaultValue()));
    addAndMakeVisible (slider);

    label.setText (caption, dontSendNotification);
    label.setJustificationType (Justification::centred);
    label.setFont (FontOptions (12.0f, Font::bold));
    label.setColour (Label::textColourId, colours::textDim);
    addAndMakeVisible (label);
}

void Knob::resized()
{
    auto r = getLocalBounds();
    label.setBounds (r.removeFromTop (16));
    slider.setBounds (r);
}

// ---------------------------------------------------------------------------
AmpPage::AmpPage (GuitarAmpProcessor& p)
    : processor (p),
      params (p.getParameters()),
      inputKnob (params, ParamIDs::inputGain, "INPUT", "Gain into the amp model. Use it like the guitar's volume / how hard you hit the amp."),
      outputKnob (params, ParamIDs::outputGain, "OUTPUT", "Final output level after the amp model."),
      interfaceInKnob (params, ParamIDs::interfaceInputDbu, "INTERFACE IN",
                       "The analogue level (dBu) at which your audio interface's instrument input reaches 0 dBFS. "
                       "Check your interface's specifications."),
      interfaceOutKnob (params, ParamIDs::interfaceOutputDbu, "INTERFACE OUT",
                        "The analogue level (dBu) your interface's output produces at 0 dBFS. Used by Calibrated output."),
      outputModeAttachment (params, ParamIDs::outputMode, outputModeBox),
      inputChannelAttachment (params, ParamIDs::inputChannel, inputChannelBox),
      calibrateAttachment (params, ParamIDs::calibrateInput, calibrateToggle)
{
    modelName.setFont (FontOptions (24.0f, Font::bold));
    modelName.setColour (Label::textColourId, colours::text);
    modelSubtitle.setFont (FontOptions (14.0f));
    modelSubtitle.setColour (Label::textColourId, colours::accent);
    statusLabel.setFont (FontOptions (13.0f));
    statusLabel.setJustificationType (Justification::topLeft);
    statusLabel.setMinimumHorizontalScale (1.0f);
    for (auto* l : { &modelName, &modelSubtitle, &statusLabel })
        addAndMakeVisible (*l);

    loadButton.setTooltip ("Load a Neural Amp Modeler (.nam) capture. You can also drag a .nam file onto this page.");
    unloadButton.setTooltip ("Remove the model. The guitar signal then passes through unprocessed.");
    loadButton.onClick = [this] { browseForModel(); };
    unloadButton.onClick = [this] { processor.unloadModel(); };
    addAndMakeVisible (loadButton);
    addAndMakeVisible (unloadButton);

    outputModeBox.addItemList ({ "Raw", "Normalized", "Calibrated" }, 1);
    inputChannelBox.addItemList ({ "Left", "Right", "Left + Right" }, 1);
    // Item lists must exist before attachments sync; re-sync now that they do.
    outputModeBox.setSelectedItemIndex ((int) *params.getRawParameterValue (ParamIDs::outputMode), dontSendNotification);
    inputChannelBox.setSelectedItemIndex ((int) *params.getRawParameterValue (ParamIDs::inputChannel), dontSendNotification);
    outputModeBox.setTooltip ("Raw: the model's level as trained.\n"
                              "Normalized: uses the model's measured loudness so captures play at a similar level.\n"
                              "Calibrated: reproduces the real amp's output level using your interface output level.");
    inputChannelBox.setTooltip ("Which input channel carries your guitar when the track is stereo.");
    outputModeLabel.setText ("OUTPUT MODE", dontSendNotification);
    inputChannelLabel.setText ("GUITAR INPUT", dontSendNotification);
    for (auto* l : { &outputModeLabel, &inputChannelLabel })
    {
        l->setFont (FontOptions (12.0f, Font::bold));
        l->setColour (Label::textColourId, colours::textDim);
        addAndMakeVisible (*l);
    }
    outputModeBox.onChange = [this] { updateControlStates(); };
    calibrateToggle.onClick = [this] { updateControlStates(); };
    calibrateToggle.setTooltip ("Applies the gain needed so the model sees the same signal level the real amplifier "
                                "saw during capture. Requires a model that stores its input level.");

    for (Component* c : std::initializer_list<Component*> { &inputKnob, &outputKnob, &interfaceInKnob, &interfaceOutKnob,
                                                            &outputModeBox, &inputChannelBox, &calibrateToggle,
                                                            &inputMeter, &outputMeter })
        addAndMakeVisible (c);

    refreshModelInfo();
    updateControlStates();
    startTimerHz (30);
}

AmpPage::~AmpPage()
{
    stopTimer();
}

void AmpPage::browseForModel()
{
    chooser = std::make_unique<FileChooser> ("Load a NAM model", processor.getLastBrowseDirectory(), "*.nam");
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                          [this] (const FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f.existsAsFile())
                                  processor.loadModel (f);
                          });
}

void AmpPage::timerCallback()
{
    auto& engine = processor.getEngine();
    inputMeter.setPeak (engine.takeInputPeak());
    outputMeter.setPeak (engine.takeOutputPeak());

    if (engine.getStatusSerial() != shownStatusSerial)
        refreshModelInfo();
}

void AmpPage::refreshModelInfo()
{
    const auto status = processor.getEngine().getStatus();
    shownStatusSerial = status.serial;

    using State = gas::AmpEngine::Status::State;
    infoRows.clear();

    auto addRow = [this] (const String& key, const String& value)
    {
        auto row = std::make_unique<InfoRow>();
        row->key.setText (key, dontSendNotification);
        row->key.setColour (Label::textColourId, colours::textDim);
        row->key.setFont (FontOptions (13.0f));
        row->value.setText (value, dontSendNotification);
        row->value.setFont (FontOptions (13.0f));
        row->value.setTooltip (value);
        addAndMakeVisible (row->key);
        addAndMakeVisible (row->value);
        infoRows.push_back (std::move (row));
    };

    if (status.info.has_value())
    {
        const auto& i = *status.info;
        modelName.setText (String (i.displayName()), dontSendNotification);

        StringArray sub;
        if (! i.gearMake.empty() || ! i.gearModel.empty())
            sub.add ((String (i.gearMake) + " " + String (i.gearModel)).trim());
        if (! i.toneType.empty())
            sub.add (prettyKey (i.toneType));
        if (! i.gearType.empty())
            sub.add (prettyKey (i.gearType));
        modelSubtitle.setText (sub.isEmpty() ? String ("No amp details stored in this file")
                                             : sub.joinIntoString ("  \xc2\xb7  "),
                               dontSendNotification);
        modelSubtitle.setColour (Label::textColourId, sub.isEmpty() ? colours::textDim : colours::accent);

        if (! i.modeledBy.empty())      addRow ("Captured by", String (i.modeledBy));
        if (! i.gearMake.empty())       addRow ("Manufacturer", String (i.gearMake));
        if (! i.gearModel.empty())      addRow ("Amplifier", String (i.gearModel));
        if (! i.gearType.empty())       addRow ("Gear type", prettyKey (i.gearType));
        if (! i.toneType.empty())       addRow ("Tone", prettyKey (i.toneType));
        addRow ("Architecture", String (i.architecture));
        addRow ("Sample rate", fmt (i.sampleRate / 1000.0, 1, " kHz")
                                   + (i.sampleRateFromFile ? String() : String (" (not in file - NAM default)"))
                                   + (status.resampling ? String (" - resampled from host ")
                                                              + fmt (status.hostSampleRate / 1000.0, 1, " kHz")
                                                        : String()));
        if (i.loudnessDb)               addRow ("Loudness", fmt (*i.loudnessDb, 1, " dB"));
        if (i.inputLevelDbu)            addRow ("Capture input level", fmt (*i.inputLevelDbu, 1, " dBu"));
        if (i.outputLevelDbu)           addRow ("Capture output level", fmt (*i.outputLevelDbu, 1, " dBu"));
        if (i.validationEsr)            addRow ("Validation ESR", fmt (*i.validationEsr, 4, ""));
        if (! i.date.empty())           addRow ("Date", String (i.date));
        if (i.numWeights > 0)           addRow ("Parameters", String ((int64) i.numWeights));
        addRow ("File", String (i.fileName));
    }
    else
    {
        modelName.setText ("No amp model", dontSendNotification);
        modelSubtitle.setText ("Load a .nam capture or drop one here", dontSendNotification);
        modelSubtitle.setColour (Label::textColourId, colours::textDim);
    }

    Colour statusColour = colours::textDim;
    switch (status.state)
    {
        case State::Loading: statusColour = colours::warning; break;
        case State::Ready:   statusColour = colours::ok; break;
        case State::Error:   statusColour = colours::error; break;
        case State::Empty:   break;
    }
    auto msg = String (status.message);
    if (status.state == State::Empty && msg.isEmpty())
        msg = "No amp model loaded - the guitar signal passes through unprocessed.";
    statusLabel.setText (msg, dontSendNotification);
    statusLabel.setColour (Label::textColourId, statusColour);

    unloadButton.setEnabled (status.info.has_value());
    updateControlStates();
    resized();
    repaint();
}

void AmpPage::updateControlStates()
{
    const bool calibrated = outputModeBox.getSelectedItemIndex() == (int) gas::OutputMode::Calibrated;
    interfaceInKnob.setEnabled (calibrateToggle.getToggleState());
    interfaceOutKnob.setEnabled (calibrated);
    repaint (calibArea); // the panel note depends on these settings
}

void AmpPage::paint (Graphics& g)
{
    drawPanel (g, modelArea.toFloat(), "Amp model");
    drawPanel (g, gainArea.toFloat(), "Gain");
    drawPanel (g, calibArea.toFloat(), "Level calibration");
    drawPanel (g, meterArea.toFloat());

    if (dragHover)
    {
        g.setColour (colours::accent.withAlpha (0.12f));
        g.fillRoundedRectangle (modelArea.toFloat(), 8.0f);
        g.setColour (colours::accent);
        g.drawRoundedRectangle (modelArea.toFloat().reduced (1.0f), 8.0f, 2.0f);
    }

    // Calibration notes
    const auto status = processor.getEngine().getStatus();
    String note;
    if (status.info)
    {
        if (calibrateToggle.getToggleState() && ! status.info->inputLevelDbu)
            note = "This model has no stored capture input level - input calibration is inactive.";
        else if (outputModeBox.getSelectedItemIndex() == (int) gas::OutputMode::Calibrated && ! status.info->outputLevelDbu)
            note = "This model has no stored output level - Calibrated output falls back to Normalized.";
        else if (outputModeBox.getSelectedItemIndex() != (int) gas::OutputMode::Raw && ! status.info->loudnessDb)
            note = "This model has no loudness measurement - its output plays at its raw level.";
    }
    if (note.isNotEmpty())
    {
        g.setColour (colours::warning);
        g.setFont (FontOptions (12.5f));
        g.drawFittedText (note, calibArea.reduced (16, 12).removeFromBottom (34), Justification::bottomLeft, 2);
    }
}

void AmpPage::resized()
{
    auto r = getLocalBounds().reduced (14);
    meterArea = r.removeFromRight (112);
    r.removeFromRight (12);
    modelArea = r.removeFromLeft (r.getWidth() / 2 + 20);
    r.removeFromLeft (12);
    gainArea = r.removeFromTop (r.getHeight() / 2 - 6);
    r.removeFromTop (12);
    calibArea = r;

    // --- model panel
    {
        auto m = modelArea.reduced (16, 12);
        m.removeFromTop (22);
        modelName.setBounds (m.removeFromTop (32));
        modelSubtitle.setBounds (m.removeFromTop (20));
        m.removeFromTop (10);
        auto buttons = m.removeFromTop (32);
        loadButton.setBounds (buttons.removeFromLeft (140));
        buttons.removeFromLeft (8);
        unloadButton.setBounds (buttons.removeFromLeft (90));
        m.removeFromTop (8);
        statusLabel.setBounds (m.removeFromTop (36));
        m.removeFromTop (6);
        for (auto& row : infoRows)
        {
            const bool fits = m.getHeight() >= 21;
            row->key.setVisible (fits);
            row->value.setVisible (fits);
            if (! fits)
                continue;
            auto line = m.removeFromTop (22);
            row->key.setBounds (line.removeFromLeft (160));
            row->value.setBounds (line);
        }
    }

    // --- gain panel
    {
        auto gp = gainArea.reduced (16, 12);
        gp.removeFromTop (20);
        auto knobs = gp.removeFromLeft (gp.getWidth() / 2);
        const int kw = knobs.getWidth() / 2;
        knobs = knobs.withSizeKeepingCentre (knobs.getWidth(), jmin (knobs.getHeight(), kw + 40));
        inputKnob.setBounds (knobs.removeFromLeft (kw).reduced (2));
        outputKnob.setBounds (knobs.reduced (2));
        gp.removeFromLeft (12);
        gp.removeFromTop (6);
        outputModeLabel.setBounds (gp.removeFromTop (18));
        outputModeBox.setBounds (gp.removeFromTop (28));
        gp.removeFromTop (14);
        inputChannelLabel.setBounds (gp.removeFromTop (18));
        inputChannelBox.setBounds (gp.removeFromTop (28));
    }

    // --- calibration panel
    {
        auto cp = calibArea.reduced (16, 12);
        cp.removeFromTop (20);
        calibrateToggle.setBounds (cp.removeFromTop (28));
        cp.removeFromBottom (36);
        auto knobs = cp.withWidth (jmin (cp.getWidth(), 260));
        const int kw = knobs.getWidth() / 2;
        knobs = knobs.withHeight (jmin (knobs.getHeight(), kw + 40));
        interfaceInKnob.setBounds (knobs.removeFromLeft (kw).reduced (2));
        interfaceOutKnob.setBounds (knobs.reduced (2));
    }

    // --- meters
    {
        auto mr = meterArea.reduced (8, 14);
        const int w = mr.getWidth() / 2;
        inputMeter.setBounds (mr.removeFromLeft (w));
        outputMeter.setBounds (mr);
    }
}

// ---------------------------------------------------------------------------
bool AmpPage::isInterestedInFileDrag (const StringArray& files)
{
    return files.size() == 1 && files[0].endsWithIgnoreCase (".nam");
}

void AmpPage::fileDragEnter (const StringArray&, int, int)
{
    dragHover = true;
    repaint();
}

void AmpPage::fileDragExit (const StringArray&)
{
    dragHover = false;
    repaint();
}

void AmpPage::filesDropped (const StringArray& files, int, int)
{
    dragHover = false;
    repaint();
    if (files.size() == 1)
        processor.loadModel (File (files[0]));
}
} // namespace ui
