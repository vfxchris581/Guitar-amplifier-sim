#include "plugin/ui/CapturePage.h"
#include "plugin/ui/Theme.h"

#include <cmath>
#include <complex>

namespace ui
{
using namespace juce;

namespace
{
void styleLabel (Label& l, float size = 14.0f, Colour c = colours::text, bool bold = false)
{
    l.setFont (FontOptions (size, bold ? Font::bold : Font::plain));
    l.setColour (Label::textColourId, c);
    l.setJustificationType (Justification::topLeft);
    l.setMinimumHorizontalScale (1.0f);
}

void styleEditor (TextEditor& e, const String& placeholder)
{
    e.setTextToShowWhenEmpty (placeholder, colours::textDim);
    e.setColour (TextEditor::backgroundColourId, colours::well);
    e.setColour (TextEditor::outlineColourId, colours::panelEdge);
    e.setColour (TextEditor::focusedOutlineColourId, colours::accent);
    e.setColour (TextEditor::textColourId, colours::text);
    e.setFont (FontOptions (14.0f));
}

String formatTime (double seconds)
{
    if (! std::isfinite (seconds) || seconds < 0)
        return "--";
    const int s = (int) std::lround (seconds);
    if (s >= 3600)
        return String (s / 3600) + " h " + String ((s / 60) % 60).paddedLeft ('0', 2) + " min";
    return String (s / 60) + ":" + String (s % 60).paddedLeft ('0', 2);
}

String esrVerdict (double esr)
{
    // The thresholds NAM's trainer uses for its comment
    if (esr < 0.01)  return "Great!";
    if (esr < 0.035) return "Not bad!";
    if (esr < 0.1)   return "...this might sound OK.";
    if (esr < 0.3)   return "...this probably won't sound great.";
    return "...something seems to have gone wrong.";
}

Colour esrColour (double esr)
{
    return esr < 0.035 ? colours::ok : (esr < 0.1 ? colours::warning : colours::error);
}

/** Base for the step panels: a titled panel with an explanation line. */
class StepPanel : public Component
{
public:
    StepPanel (String t, String d) : title (std::move (t)), description (std::move (d)) {}

    void paint (Graphics& g) override
    {
        drawPanel (g, getLocalBounds().toFloat());
        auto r = getLocalBounds().reduced (20, 14);
        g.setColour (colours::text);
        g.setFont (FontOptions (19.0f, Font::bold));
        g.drawText (title, r.removeFromTop (26), Justification::centredLeft);
        g.setColour (colours::textDim);
        g.setFont (FontOptions (13.5f));
        g.drawFittedText (description, r.removeFromTop (38), Justification::topLeft, 2);
    }

    Rectangle<int> body() const { return getLocalBounds().reduced (20, 14).withTrimmedTop (70); }

    std::function<void()> onChanged;

private:
    String title, description;
};

/** Shows one line per check with a coloured status dot. */
struct CheckList : public Component
{
    struct Item { Colour colour; String text; };
    std::vector<Item> items;

    void set (std::vector<Item> i)
    {
        items = std::move (i);
        repaint();
    }

    void paint (Graphics& g) override
    {
        auto r = getLocalBounds();
        g.setFont (FontOptions (13.5f));
        for (const auto& it : items)
        {
            auto line = r.removeFromTop (22);
            g.setColour (it.colour);
            g.fillEllipse (line.removeFromLeft (16).withSizeKeepingCentre (8, 8).toFloat());
            g.setColour (colours::text);
            g.drawFittedText (it.text, line.withTrimmedLeft (4), Justification::centredLeft, 1, 0.9f);
        }
    }
};

struct ProgressStrip : public Component
{
    double value = 0.0;
    String text;
    void paint (Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (colours::well);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (colours::accent);
        g.fillRoundedRectangle (r.withWidth (r.getWidth() * (float) jlimit (0.0, 1.0, value)), 4.0f);
        g.setColour (colours::text);
        g.setFont (FontOptions (12.5f, Font::bold));
        g.drawText (text, r, Justification::centred);
    }
};
} // namespace

// ===========================================================================
// 1. Setup & routing
// ===========================================================================
class CapturePage::SetupPanel : public StepPanel
{
public:
    SetupPanel (GuitarAmpProcessor& p)
        : StepPanel ("1  Setup & routing",
                     "Connect the amp, tell the plugin where the reamp send and the amp's return are, and load NAM's "
                     "standard capture signal. Everything happens inside this plugin."),
          processor (p), capture (p.getCapture())
    {
        styleLabel (signalLabel);
        addAndMakeVisible (signalLabel);
        chooseButton.onClick = [this] { chooseSignal(); };
        addAndMakeVisible (chooseButton);
        getLink.setButtonText ("Get NAM's capture signal (v3_0_0.wav)");
        getLink.setURL (URL (gas::capture::V3Layout::officialDownload));
        getLink.setFont (FontOptions (13.5f), false, Justification::centredLeft);
        getLink.setColour (HyperlinkButton::textColourId, colours::accent);
        addAndMakeVisible (getLink);

        sendBox.addItemList ({ "Output left", "Output right", "Output left + right" }, 1);
        returnBox.addItemList ({ "Input left", "Input right" }, 1);
        sendBox.setSelectedItemIndex (capture.sendChannel.load(), dontSendNotification);
        returnBox.setSelectedItemIndex (capture.returnChannel.load(), dontSendNotification);
        sendBox.onChange = [this] { capture.sendChannel = sendBox.getSelectedItemIndex(); };
        returnBox.onChange = [this] { capture.returnChannel = returnBox.getSelectedItemIndex(); };
        sendBox.setTooltip ("The plugin output channel routed (in your DAW) to the interface output that feeds the reamp box.");
        returnBox.setTooltip ("The plugin input channel that carries the amp's return (load box line out or microphone).");
        for (auto* l : { &sendLabel, &returnLabel, &modeLabel })
            styleLabel (*l, 12.0f, colours::textDim, true);
        sendLabel.setText ("SEND (TO REAMP BOX)", dontSendNotification);
        returnLabel.setText ("RETURN (FROM AMP)", dontSendNotification);
        modeLabel.setText ("WHAT ARE YOU CAPTURING?", dontSendNotification);

        modeBox.addItemList ({ "Amp head via load box (no cab)", "Amp + cabinet via microphone", "Pedal", "Preamp" }, 1);
        modeBox.setSelectedItemIndex (0, dontSendNotification);
        modeBox.onChange = [this] { repaint(); if (onChanged) onChanged(); };

        styleLabel (hostLabel, 13.5f);
        for (Component* c : std::initializer_list<Component*> { &sendBox, &returnBox, &sendLabel, &returnLabel, &modeLabel,
                                                                &modeBox, &hostLabel })
            addAndMakeVisible (c);
        refresh();
    }

    String gearType() const
    {
        static const char* types[] = { "amp", "amp_cab", "pedal", "preamp" };
        return types[jlimit (0, 3, modeBox.getSelectedItemIndex())];
    }

    void refresh()
    {
        const auto saved = capture.savedSignalPath();
        if (capture.hasSignal())
            signalLabel.setText (String::fromUTF8 ("\xe2\x9c\x93 ") + "Verified NAM v3 capture signal: " + capture.signalPath(), dontSendNotification);
        else if (saved.isNotEmpty() && File (saved).existsAsFile())
            signalLabel.setText ("Capture signal: " + saved + " (verified when the capture starts)", dontSendNotification);
        else
            signalLabel.setText ("No capture signal loaded yet.", dontSendNotification);
        signalLabel.setColour (Label::textColourId, capture.hasSignal() || saved.isNotEmpty() ? colours::ok : colours::warning);

        const double rate = processor.getHostSampleRate();
        const int ins = processor.getTotalNumInputChannels(), outs = processor.getTotalNumOutputChannels();
        String h;
        h << "Host: " << (rate > 0 ? String (rate / 1000.0, 1) + " kHz" : String ("not running")) << ", " << ins
          << " in / " << outs << " out.";
        if (rate > 0 && std::abs (rate - 48000.0) > 0.5)
            h << "  Set the project to 48 kHz to capture.";
        if (ins < 2 && returnBox.getSelectedItemIndex() == 1)
            h << "  The track is mono: the return will be read from its only input.";
        hostLabel.setText (h, dontSendNotification);
        hostLabel.setColour (Label::textColourId, std::abs (rate - 48000.0) < 0.5 ? colours::textDim : colours::warning);
    }

    void paint (Graphics& g) override
    {
        StepPanel::paint (g);
        drawGuide (g, guideArea.toFloat(), modeBox.getSelectedItemIndex() == 1);
    }

    void resized() override
    {
        auto b = body();
        auto row = b.removeFromTop (24);
        signalLabel.setBounds (row);
        row = b.removeFromTop (30);
        chooseButton.setBounds (row.removeFromLeft (190).reduced (0, 2));
        row.removeFromLeft (12);
        getLink.setBounds (row.removeFromLeft (320));
        b.removeFromTop (10);

        auto cols = b.removeFromTop (58);
        auto c1 = cols.removeFromLeft (cols.getWidth() / 3).reduced (0, 0);
        auto c2 = cols.removeFromLeft (cols.getWidth() / 2);
        auto c3 = cols;
        sendLabel.setBounds (c1.removeFromTop (18));
        sendBox.setBounds (c1.removeFromTop (28).withTrimmedRight (12));
        returnLabel.setBounds (c2.removeFromTop (18));
        returnBox.setBounds (c2.removeFromTop (28).withTrimmedRight (12));
        modeLabel.setBounds (c3.removeFromTop (18));
        modeBox.setBounds (c3.removeFromTop (28));
        hostLabel.setBounds (b.removeFromTop (24));
        guideArea = b;
    }

private:
    void chooseSignal()
    {
        chooser = std::make_unique<FileChooser> ("Choose NAM's capture signal (v3_0_0.wav)",
                                                 File::getSpecialLocation (File::userHomeDirectory), "*.wav");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                              [this] (const FileChooser& fc)
                              {
                                  const auto f = fc.getResult();
                                  if (! f.existsAsFile())
                                      return;
                                  String err;
                                  if (! capture.loadSignal (f, err))
                                      AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon,
                                                                        "Not NAM's capture signal", err);
                                  refresh();
                                  if (onChanged)
                                      onChanged();
                              });
    }

    static void box (Graphics& g, Rectangle<float> r, const String& text, Colour edge)
    {
        g.setColour (colours::well);
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (edge);
        g.drawRoundedRectangle (r, 6.0f, 1.5f);
        g.setColour (colours::text);
        g.setFont (FontOptions (12.5f, Font::bold));
        g.drawFittedText (text, r.reduced (4).toNearestInt(), Justification::centred, 2);
    }

    static void arrow (Graphics& g, Point<float> a, Point<float> b, const String& label, Colour c)
    {
        g.setColour (c);
        Line<float> line (a, b);
        g.drawArrow (line, 2.0f, 9.0f, 9.0f);
        if (label.isNotEmpty())
        {
            g.setFont (FontOptions (10.5f));
            g.setColour (colours::textDim);
            const float width = jmax (40.0f, std::abs (b.x - a.x) + 6.0f);
            g.drawFittedText (label, Rectangle<float> (width, 13).withCentre (line.getPointAlongLineProportionally (0.5f))
                                         .translated (0, -11).toNearestInt(),
                              Justification::centred, 1, 0.8f);
        }
    }

    /** The connection guide, drawn for the selected capture type. */
    void drawGuide (Graphics& g, Rectangle<float> area, bool micd) const
    {
        if (area.getHeight() < 120)
            return;
        auto r = area.reduced (4);
        auto warn = r.removeFromBottom (44);

        const float w = jmin (124.0f, (r.getWidth() - 4 * 56.0f) / 5.0f), h = 48.0f;
        const float y1 = r.getY() + 8, y2 = r.getY() + 8 + h + 38;
        const float gap = (r.getWidth() - 5 * w) / 4.0f;
        auto at = [&] (int col, float y) { return Rectangle<float> (r.getX() + col * (w + gap), y, w, h); };

        const auto plug = at (0, y1), iface = at (1, y1), reamp = at (2, y1), amp = at (3, y1), load = at (4, y1);
        box (g, plug, "This plugin\n(DAW track)", colours::accent);
        box (g, iface, "Audio interface\nline output", colours::panelEdge);
        box (g, reamp, "Reamp box", colours::panelEdge);
        box (g, amp, "Amplifier\ninput", colours::panelEdge);
        arrow (g, plug.getCentre().withX (plug.getRight()), iface.getCentre().withX (iface.getX()), "send", colours::accent);
        arrow (g, iface.getCentre().withX (iface.getRight()), reamp.getCentre().withX (reamp.getX()), "line", colours::textDim);
        arrow (g, reamp.getCentre().withX (reamp.getRight()), amp.getCentre().withX (amp.getX()), "instrument", colours::textDim);

        const auto spk = at (4, y2), mic = at (3, y2), ifaceIn = at (1, y2), plugIn = at (0, y2);
        if (micd)
        {
            box (g, load, "Speaker cabinet\n(amp speaker out)", colours::panelEdge);
            arrow (g, amp.getCentre().withX (amp.getRight()), load.getCentre().withX (load.getX()), "speaker", colours::warning);
            box (g, spk, "Microphone\non the speaker", colours::panelEdge);
            arrow (g, load.getCentre().withY (load.getBottom()), spk.getCentre().withY (spk.getY()), "", colours::textDim);
            box (g, mic, "Mic preamp", colours::panelEdge);
            arrow (g, spk.getCentre().withX (spk.getX()), mic.getCentre().withX (mic.getRight()), "", colours::textDim);
        }
        else
        {
            box (g, load, "Reactive load box /\nattenuator", colours::panelEdge);
            arrow (g, amp.getCentre().withX (amp.getRight()), load.getCentre().withX (load.getX()), "speaker", colours::warning);
            box (g, spk, "Load box\nline / DI out", colours::panelEdge);
            arrow (g, load.getCentre().withY (load.getBottom()), spk.getCentre().withY (spk.getY()), "", colours::textDim);
        }
        box (g, ifaceIn, "Audio interface\nline input", colours::panelEdge);
        box (g, plugIn, "This plugin\n(return)", colours::accent);
        arrow (g, (micd ? mic : spk).getCentre().withX ((micd ? mic : spk).getX()), ifaceIn.getCentre().withX (ifaceIn.getRight()), "return", colours::textDim);
        arrow (g, ifaceIn.getCentre().withX (ifaceIn.getX()), plugIn.getCentre().withX (plugIn.getRight()), "", colours::accent);

        g.setColour (colours::error.withAlpha (0.12f));
        g.fillRoundedRectangle (warn, 6.0f);
        g.setColour (colours::error);
        g.setFont (FontOptions (13.0f, Font::bold));
        g.drawFittedText ("Never connect an amplifier's speaker output to an audio interface input, and never run a valve amp "
                          "without a speaker or a load. Use a reactive load box / attenuator with a line output, or a mic.",
                          warn.reduced (10, 4).toNearestInt(), Justification::centredLeft, 2);
    }

    GuitarAmpProcessor& processor;
    CaptureController& capture;
    Label signalLabel, sendLabel, returnLabel, modeLabel, hostLabel;
    TextButton chooseButton { "Choose capture signal..." };
    HyperlinkButton getLink;
    ComboBox sendBox, returnBox, modeBox;
    std::unique_ptr<FileChooser> chooser;
    Rectangle<int> guideArea;
};

// ===========================================================================
// 2. Level check
// ===========================================================================
class CapturePage::LevelPanel : public StepPanel
{
public:
    LevelPanel (GuitarAmpProcessor& p)
        : StepPanel ("2  Level check",
                     "Plays 7 seconds of NAM's signal (calibration blips, sweeps and noise) and listens to the return: "
                     "measures the round-trip latency, the return level, clipping and background noise."),
          processor (p), capture (p.getCapture())
    {
        runButton.onClick = [this]
        {
            if (capture.phase() == CaptureController::Phase::LevelCheck)
            {
                capture.cancel();
                return;
            }
            String err;
            if (! capture.startLevelCheck (processor.getHostSampleRate(), err))
                AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Level check", err);
        };
        addAndMakeVisible (runButton);
        addAndMakeVisible (progress);
        addAndMakeVisible (checks);
        addAndMakeVisible (sendMeter);
        addAndMakeVisible (returnMeter);

        gainSlider.setSliderStyle (Slider::LinearHorizontal);
        gainSlider.setRange (-24.0, 6.0, 0.1);
        gainSlider.setValue (capture.transport.playbackGainDb.load(), dontSendNotification);
        gainSlider.setTextValueSuffix (" dB");
        gainSlider.setTextBoxStyle (Slider::TextBoxRight, false, 70, 22);
        gainSlider.setColour (Slider::trackColourId, colours::accent);
        gainSlider.setColour (Slider::backgroundColourId, colours::well);
        gainSlider.onValueChange = [this] { capture.transport.playbackGainDb = (float) gainSlider.getValue(); };
        gainSlider.setTooltip ("Level of the capture signal sent to the reamp box. NAM's signal is designed to be played "
                               "at 0 dB; set the reamp box so the amp sees a normal guitar level instead of changing this.");
        styleLabel (gainLabel, 12.0f, colours::textDim, true);
        gainLabel.setText ("SEND LEVEL", dontSendNotification);
        addAndMakeVisible (gainSlider);
        addAndMakeVisible (gainLabel);
    }

    void update()
    {
        const bool running = capture.phase() == CaptureController::Phase::LevelCheck;
        runButton.setButtonText (running ? "Stop" : (capture.levelCheck ? "Run level check again" : "Run level check"));
        progress.value = running ? capture.transport.progress() : (capture.levelCheck ? 1.0 : 0.0);
        progress.text = running ? "Listening..." : (capture.levelCheck ? "Done" : "");
        progress.repaint();
        sendMeter.setPeak (capture.transport.sendPeak.exchange (0.0f));
        returnMeter.setPeak (capture.transport.returnPeak.exchange (0.0f));

        if (capture.levelCheck && ! running)
        {
            std::vector<CheckList::Item> items;
            const auto& lc = *capture.levelCheck;
            items.push_back ({ lc.latency.detected && ! lc.latency.matchesLookahead && *lc.latency.detected >= 0 ? colours::ok : colours::error,
                               lc.latency.message });
            items.push_back ({ lc.levels.clippedSamples > 0 ? colours::error : (lc.levels.peakDbfs < -30 ? colours::warning : colours::ok),
                               "Return peak " + String (lc.levels.peakDbfs, 1) + " dBFS"
                                   + (lc.levels.clippedSamples > 0 ? " (clipping!)" : "") });
            items.push_back ({ lc.levels.noiseFloorDbfs > -60 ? colours::warning : colours::ok,
                               "Background noise " + (lc.levels.noiseFloorDbfs < -120 ? String ("below -120") : String (lc.levels.noiseFloorDbfs, 1)) + " dBFS" });
            for (const auto& w : lc.levels.warnings)
                items.push_back ({ colours::warning, String (w) });
            checks.set (items);
        }
    }

    void resized() override
    {
        auto b = body();
        auto meters = b.removeFromRight (90);
        sendMeter.setBounds (meters.removeFromLeft (45).withTrimmedBottom (10));
        returnMeter.setBounds (meters.withTrimmedBottom (10));
        b.removeFromRight (16);
        auto row = b.removeFromTop (34);
        runButton.setBounds (row.removeFromLeft (190));
        row.removeFromLeft (12);
        progress.setBounds (row.reduced (0, 6));
        b.removeFromTop (12);
        row = b.removeFromTop (26);
        gainLabel.setBounds (row.removeFromLeft (100));
        gainSlider.setBounds (row.removeFromLeft (320));
        b.removeFromTop (16);
        checks.setBounds (b);
    }

private:
    GuitarAmpProcessor& processor;
    CaptureController& capture;
    TextButton runButton { "Run level check" };
    ProgressStrip progress;
    CheckList checks;
    LevelMeter sendMeter { "SEND" }, returnMeter { "RETURN" };
    Slider gainSlider;
    Label gainLabel;
};

// ===========================================================================
// 3. Capture
// ===========================================================================
class CapturePage::CapturePanel : public StepPanel
{
public:
    CapturePanel (GuitarAmpProcessor& p)
        : StepPanel ("3  Capture",
                     "Plays NAM's full 3:10 capture signal through the amp and records the return. Keep the amp's "
                     "settings, levels and room still until it finishes."),
          processor (p), capture (p.getCapture())
    {
        runButton.onClick = [this]
        {
            if (capture.phase() == CaptureController::Phase::Capturing)
            {
                capture.cancel();
                return;
            }
            String err;
            if (capture.startCapture (processor.getHostSampleRate(), err))
                peaks.clear();
            else
                AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Capture", err);
        };
        addAndMakeVisible (runButton);
        addAndMakeVisible (progress);
        addAndMakeVisible (checks);
    }

    void update()
    {
        const bool running = capture.phase() == CaptureController::Phase::Capturing;
        runButton.setButtonText (running ? "Cancel capture" : (capture.capture ? "Capture again" : "Start capture"));
        if (running)
        {
            const double total = 190.0;
            progress.value = capture.transport.progress();
            progress.text = formatTime (progress.value * total) + " / " + formatTime (total);
            peaks.push_back (capture.transport.returnPeak.exchange (0.0f));
            repaint (waveArea);
        }
        else
        {
            progress.value = capture.capture ? 1.0 : 0.0;
            progress.text = capture.capture ? "Captured" : "";
        }
        progress.repaint();

        if (capture.capture && ! running && shownResult != &*capture.capture)
        {
            shownResult = &*capture.capture;
            const auto& c = *capture.capture;
            std::vector<CheckList::Item> items;
            items.push_back ({ c.latency.recommended ? colours::ok : colours::error, c.latency.message });
            items.push_back ({ c.levels.clippedSamples > 0 ? colours::error : colours::ok,
                               "Return peak " + String (c.levels.peakDbfs, 1) + " dBFS, background noise "
                                   + (c.levels.noiseFloorDbfs < -120 ? String ("below -120") : String (c.levels.noiseFloorDbfs, 1)) + " dBFS" });
            items.push_back ({ c.replicate.passed ? colours::ok : colours::warning,
                               "Consistency (NAM's validation replicate check): ESR "
                                   + String (c.replicate.esr, 5)
                                   + (c.replicate.passed ? " - passed" : " - the amp's sound changed during the capture") });
            for (const auto& w : c.levels.warnings)
                items.push_back ({ colours::warning, String (w) });
            items.push_back ({ c.usable ? colours::ok : colours::error,
                               c.usable ? "Ready to train (step 4)." : "This capture can't be used - fix the issue above and capture again." });
            checks.set (items);
        }
    }

    void paint (Graphics& g) override
    {
        StepPanel::paint (g);
        auto r = waveArea.toFloat();
        g.setColour (colours::well);
        g.fillRoundedRectangle (r, 6.0f);
        if (peaks.empty())
        {
            g.setColour (colours::textDim);
            g.setFont (FontOptions (13.0f));
            g.drawText ("The amp's return appears here while capturing", r, Justification::centred);
            return;
        }
        const float total = 190.0f * 30.0f; // UI frames over the capture
        const float mid = r.getCentreY();
        g.setColour (colours::accent);
        for (size_t i = 0; i < peaks.size(); ++i)
        {
            const float x = r.getX() + r.getWidth() * (float) i / total;
            const float h = jmin (1.0f, peaks[i]) * r.getHeight() * 0.48f;
            g.drawVerticalLine ((int) x, mid - h, mid + h);
        }
    }

    void resized() override
    {
        auto b = body();
        auto row = b.removeFromTop (34);
        runButton.setBounds (row.removeFromLeft (190));
        row.removeFromLeft (12);
        progress.setBounds (row.reduced (0, 6));
        b.removeFromTop (12);
        waveArea = b.removeFromTop (110);
        b.removeFromTop (12);
        checks.setBounds (b);
    }

private:
    GuitarAmpProcessor& processor;
    CaptureController& capture;
    TextButton runButton { "Start capture" };
    ProgressStrip progress;
    CheckList checks;
    std::vector<float> peaks;
    Rectangle<int> waveArea;
    const void* shownResult = nullptr;
};

// ===========================================================================
// 4. Train
// ===========================================================================
class CapturePage::TrainPanel : public StepPanel
{
public:
    TrainPanel (GuitarAmpProcessor& p, std::function<String()> gearTypeFn)
        : StepPanel ("4  Train the model",
                     "Trains a Neural Amp Modeler WaveNet on your capture, in the background, inside the plugin. "
                     "Name it after the amp and the settings you captured (e.g. \"5150 Lead\")."),
          capture (p.getCapture()), gearType (std::move (gearTypeFn))
    {
        styleEditor (nameEdit, "Name, e.g. 5150 Lead");
        styleEditor (authorEdit, "Captured by");
        styleEditor (makeEdit, "Manufacturer, e.g. Peavey");
        styleEditor (modelEdit, "Amplifier, e.g. 5150");
        styleEditor (settingsEdit, "Amp settings, e.g. Lead ch, Pre 6, Post 3, Low 7, Mid 5, High 6");
        styleEditor (inLevelEdit, "optional");
        styleEditor (outLevelEdit, "optional");
        inLevelEdit.setInputRestrictions (6, "-0123456789.");
        outLevelEdit.setInputRestrictions (6, "-0123456789.");
        inLevelEdit.setTooltip ("Analogue level (dBu) the reamp box delivers for a 0 dBFS signal. Lets players calibrate "
                                "their input to match this capture. Leave empty if unknown.");
        outLevelEdit.setTooltip ("Analogue level (dBu) at your interface input for 0 dBFS. Leave empty if unknown.");

        toneBox.addItemList ({ "(tone not set)", "Clean", "Crunch", "Overdrive", "Hi-gain", "Fuzz" }, 1);
        toneBox.setSelectedItemIndex (0, dontSendNotification);

        archBox.addItemList ({ "Standard (best accuracy, 13,801 parameters)", "Lite (6,553 parameters)",
                               "Feather (3,025 parameters)", "Nano (841 parameters, lowest CPU)" }, 1);
        archBox.setSelectedItemIndex (0, dontSendNotification);
        batchBox.addItemList ({ "4", "8", "16", "32" }, 1);
        batchBox.setSelectedItemIndex (2, dontSendNotification);

        auto setupSlider = [] (Slider& s, double lo, double hi, double step, double v)
        {
            s.setSliderStyle (Slider::IncDecButtons);
            s.setRange (lo, hi, step);
            s.setValue (v, dontSendNotification);
            s.setTextBoxStyle (Slider::TextBoxLeft, false, 70, 24);
            s.setIncDecButtonsMode (Slider::incDecButtonsDraggable_Vertical);
        };
        setupSlider (epochs, 1, 1000, 1, 100);
        setupSlider (learningRate, 0.0001, 0.05, 0.0001, 0.004);
        setupSlider (decay, 0.0, 0.05, 0.001, 0.007);
        learningRate.setNumDecimalPlacesToDisplay (4);
        decay.setNumDecimalPlacesToDisplay (3);

        for (auto* l : { &lName, &lAuthor, &lMake, &lModel, &lSettings, &lTone, &lIn, &lOut, &lArch, &lEpochs, &lLr,
                         &lDecay, &lBatch })
        {
            styleLabel (*l, 11.5f, colours::textDim, true);
            addAndMakeVisible (*l);
        }
        lName.setText ("PROFILE NAME", dontSendNotification);
        lAuthor.setText ("CAPTURED BY", dontSendNotification);
        lMake.setText ("MANUFACTURER", dontSendNotification);
        lModel.setText ("AMPLIFIER", dontSendNotification);
        lSettings.setText ("CAPTURED SETTINGS", dontSendNotification);
        lTone.setText ("TONE", dontSendNotification);
        lIn.setText ("REAMP LEVEL (dBu @ 0 dBFS)", dontSendNotification);
        lOut.setText ("INPUT LEVEL (dBu @ 0 dBFS)", dontSendNotification);
        lArch.setText ("MODEL SIZE", dontSendNotification);
        lEpochs.setText ("EPOCHS", dontSendNotification);
        lLr.setText ("LEARNING RATE", dontSendNotification);
        lDecay.setText ("LR DECAY / EPOCH", dontSendNotification);
        lBatch.setText ("BATCH SIZE", dontSendNotification);

        styleLabel (fixedInfo, 12.5f, colours::textDim);
        fixedInfo.setText ("Training sample rate: 48 kHz (NAM standard).  Validation: NAM's held-out final 9 s "
                           "(not used for training).  Runs on all CPU cores at low priority; GPU training is not "
                           "available inside the plugin.",
                           dontSendNotification);

        trainButton.onClick = [this] { toggleTraining(); };
        styleLabel (statusLabel, 13.5f);
        for (Component* c : std::initializer_list<Component*> { &nameEdit, &authorEdit, &makeEdit, &modelEdit, &settingsEdit,
                                                                &toneBox, &inLevelEdit, &outLevelEdit, &archBox, &epochs,
                                                                &learningRate, &decay, &batchBox, &fixedInfo, &trainButton,
                                                                &progress, &statusLabel })
            addAndMakeVisible (c);
    }

    void update()
    {
        const bool running = capture.isTraining();
        const auto st = capture.trainingStatus();
        trainButton.setButtonText (running ? "Cancel training" : "Start training");
        trainButton.setEnabled (running || (capture.capture && capture.capture->usable));

        const auto& p = st.progress;
        if (running && p.epochs > 0)
        {
            const double frac = ((p.epoch - 1) + (double) p.step / jmax (1, p.stepsPerEpoch)) / p.epochs;
            progress.value = frac;
            progress.text = "Epoch " + String (p.epoch) + " / " + String (p.epochs) + "   -   "
                            + formatTime (p.secondsRemaining) + " left";
        }
        else if (st.finished)
        {
            progress.value = 1.0;
            progress.text = "Finished";
        }
        else
        {
            progress.value = 0.0;
            progress.text = running ? "Preparing..." : "";
        }
        progress.repaint();

        String s;
        if (! capture.capture || ! capture.capture->usable)
            s = "Make a usable capture in step 3 first.";
        else
        {
            s = st.message;
            if (p.epoch > 0)
            {
                s << "   |   training MSE " << String::formatted ("%.3e", p.trainLoss);
                if (p.validationEsr)
                    s << "   |   validation ESR " << String (*p.validationEsr, 4) << " (best " << String (*p.bestValidationEsr, 4) << ")";
            }
        }
        statusLabel.setText (s, dontSendNotification);
        statusLabel.setColour (Label::textColourId, st.failed ? colours::error : colours::text);

        if (p.epochValidationEsr.size() != history.size())
        {
            history = p.epochValidationEsr;
            repaint (chartArea);
        }
    }

    void paint (Graphics& g) override
    {
        StepPanel::paint (g);
        auto r = chartArea.toFloat();
        g.setColour (colours::well);
        g.fillRoundedRectangle (r, 6.0f);
        g.setFont (FontOptions (11.5f));
        g.setColour (colours::textDim);
        g.drawText ("Validation ESR per epoch (log scale, lower is better)", r.reduced (8, 4), Justification::topLeft);
        if (history.size() < 2)
            return;
        double lo = 1e9, hi = 0;
        for (double v : history)
        {
            lo = std::min (lo, std::max (v, 1e-6));
            hi = std::max (hi, std::max (v, 1e-6));
        }
        lo = std::log10 (lo) - 0.05;
        hi = std::log10 (hi) + 0.05;
        auto plot = r.reduced (10, 22);
        Path path;
        for (size_t i = 0; i < history.size(); ++i)
        {
            const float x = plot.getX() + plot.getWidth() * (float) i / (float) (history.size() - 1);
            const float y = plot.getBottom() - plot.getHeight() * (float) ((std::log10 (std::max (history[i], 1e-6)) - lo) / (hi - lo));
            i == 0 ? path.startNewSubPath (x, y) : path.lineTo (x, y);
        }
        g.setColour (colours::accent);
        g.strokePath (path, PathStrokeType (2.0f));
        g.setColour (colours::textDim);
        g.drawText (String (history.back(), 4), r.reduced (8, 4), Justification::bottomRight);
    }

    void resized() override
    {
        auto b = body();
        auto grid = [] (Rectangle<int>& area, int cols)
        {
            std::vector<Rectangle<int>> cells;
            auto row = area.removeFromTop (46);
            const int w = row.getWidth() / cols;
            for (int i = 0; i < cols; ++i)
                cells.push_back (row.removeFromLeft (w).withTrimmedRight (10));
            area.removeFromTop (4);
            return cells;
        };
        auto place = [] (Label& l, Component& c, Rectangle<int> cell)
        {
            l.setBounds (cell.removeFromTop (16));
            c.setBounds (cell.removeFromTop (28));
        };
        auto r1 = grid (b, 4);
        place (lName, nameEdit, r1[0]);
        place (lMake, makeEdit, r1[1]);
        place (lModel, modelEdit, r1[2]);
        place (lTone, toneBox, r1[3]);
        auto r2 = grid (b, 4);
        place (lSettings, settingsEdit, r2[0].getUnion (r2[1]).getUnion (r2[2]));
        place (lAuthor, authorEdit, r2[3]);
        auto r3 = grid (b, 4);
        place (lArch, archBox, r3[0].getUnion (r3[1]));
        place (lEpochs, epochs, r3[2]);
        place (lBatch, batchBox, r3[3]);
        auto r4 = grid (b, 4);
        place (lLr, learningRate, r4[0]);
        place (lDecay, decay, r4[1]);
        place (lIn, inLevelEdit, r4[2]);
        place (lOut, outLevelEdit, r4[3]);
        fixedInfo.setBounds (b.removeFromTop (34));
        auto row = b.removeFromTop (32);
        trainButton.setBounds (row.removeFromLeft (170));
        row.removeFromLeft (12);
        progress.setBounds (row.reduced (0, 5));
        statusLabel.setBounds (b.removeFromTop (24));
        chartArea = b.withTrimmedTop (4);
    }

private:
    void toggleTraining()
    {
        if (capture.isTraining())
        {
            capture.cancelTraining();
            return;
        }
        gas::train::TrainOptions opt;
        opt.preset = (gas::train::Preset) archBox.getSelectedItemIndex();
        opt.epochs = (int) epochs.getValue();
        opt.learningRate = (float) learningRate.getValue();
        opt.learningRateDecay = (float) decay.getValue();
        opt.batchSize = batchBox.getText().getIntValue();

        CaptureController::ModelDetails d;
        d.name = nameEdit.getText().trim();
        d.modeledBy = authorEdit.getText().trim();
        d.gearMake = makeEdit.getText().trim();
        d.gearModel = modelEdit.getText().trim();
        d.settingsNote = settingsEdit.getText().trim();
        d.gearType = gearType();
        static const char* tones[] = { "", "clean", "crunch", "overdrive", "hi_gain", "fuzz" };
        d.toneType = tones[jlimit (0, 5, toneBox.getSelectedItemIndex())];
        if (inLevelEdit.getText().trim().isNotEmpty())
            d.inputLevelDbu = inLevelEdit.getText().getDoubleValue();
        if (outLevelEdit.getText().trim().isNotEmpty())
            d.outputLevelDbu = outLevelEdit.getText().getDoubleValue();

        String err;
        if (! capture.startTraining (opt, d, err))
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Training", err);
        history.clear();
    }

    CaptureController& capture;
    std::function<String()> gearType;
    TextEditor nameEdit, authorEdit, makeEdit, modelEdit, settingsEdit, inLevelEdit, outLevelEdit;
    ComboBox toneBox, archBox, batchBox;
    Slider epochs, learningRate, decay;
    Label lName, lAuthor, lMake, lModel, lSettings, lTone, lIn, lOut, lArch, lEpochs, lLr, lDecay, lBatch, fixedInfo,
        statusLabel;
    TextButton trainButton { "Start training" };
    ProgressStrip progress;
    std::vector<double> history;
    Rectangle<int> chartArea;
};

// ===========================================================================
// 5. Compare & save
// ===========================================================================
class CapturePage::ComparePanel : public StepPanel
{
public:
    ComparePanel (GuitarAmpProcessor& p, std::function<void (const File&)> loadIntoAmpFn)
        : StepPanel ("5  Compare & save",
                     "Listen to the real amp and the model on the same held-out audio (level-matched and latency-"
                     "aligned), check the difference, then keep or discard the model."),
          capture (p.getCapture()), loadIntoAmp (std::move (loadIntoAmpFn))
    {
        const char* names[] = { "Stop", "Real amp", "Model", "Difference" };
        for (int i = 0; i < 4; ++i)
        {
            auto* b = abButtons.add (new TextButton (names[i]));
            b->setClickingTogglesState (true);
            b->setRadioGroupId (77);
            b->onClick = [this, i] { capture.audition.setMode ((AuditionPlayer::Mode) i); };
            addAndMakeVisible (b);
        }
        abButtons[0]->setToggleState (true, dontSendNotification);

        saveButton.onClick = [this]
        {
            String err;
            const auto f = capture.saveModel (err);
            if (f == File())
            {
                AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Save", err);
                return;
            }
            savedFile = f;
            savedLabel.setText ("Saved to " + f.getFullPathName(), dontSendNotification);
            loadButton.setEnabled (true);
        };
        loadButton.onClick = [this] { if (savedFile.existsAsFile()) loadIntoAmp (savedFile); };
        discardButton.onClick = [this]
        {
            capture.audition.setMode (AuditionPlayer::Off);
            capture.discardModel();
            shown = false;
            savedFile = File();
            savedLabel.setText ({}, dontSendNotification);
            repaint();
        };
        loadButton.setEnabled (false);
        styleLabel (resultLabel, 15.0f, colours::text, true);
        styleLabel (savedLabel, 12.5f, colours::textDim);
        for (Component* c : std::initializer_list<Component*> { &saveButton, &loadButton, &discardButton, &resultLabel, &savedLabel })
            addAndMakeVisible (c);
    }

    void update()
    {
        const bool ready = ! capture.isTraining() && capture.validation.has_value();
        for (auto* b : abButtons)
            b->setEnabled (ready && processorRateOk);
        saveButton.setEnabled (ready);
        discardButton.setEnabled (ready);

        if (ready && ! shown)
        {
            shown = true;
            const auto& v = *capture.validation;
            resultLabel.setText ("Validation ESR " + String (v.esr, 4) + "  -  " + esrVerdict (v.esr) + "   ("
                                     + v.architecture + ", " + String (v.parameters) + " parameters)",
                                 dontSendNotification);
            resultLabel.setColour (Label::textColourId, esrColour (v.esr));
            computeSpectra (v.reference, v.model);
            repaint();
        }
        else if (! ready && shown)
        {
            shown = false;
            resultLabel.setText (capture.isTraining() ? "Training in progress..." : "Train a model in step 4 first.",
                                 dontSendNotification);
            resultLabel.setColour (Label::textColourId, colours::textDim);
            repaint();
        }
        else if (! ready)
        {
            resultLabel.setText (capture.isTraining() ? "Training in progress..." : "Train a model in step 4 first.",
                                 dontSendNotification);
            resultLabel.setColour (Label::textColourId, colours::textDim);
        }
        if (ready && capture.audition.getMode() != AuditionPlayer::Off)
            repaint (waveArea);
    }

    bool processorRateOk = true;

    void paint (Graphics& g) override
    {
        StepPanel::paint (g);
        g.setColour (colours::well);
        g.fillRoundedRectangle (waveArea.toFloat(), 6.0f);
        g.fillRoundedRectangle (specArea.toFloat(), 6.0f);
        g.setFont (FontOptions (11.5f));
        g.setColour (colours::textDim);
        g.drawText ("Waveform, 20 ms: amp (amber) vs model (white)",
                    waveArea.reduced (8, 4), Justification::topLeft);
        g.drawText ("Average spectrum, 40 Hz - 20 kHz (dB)", specArea.reduced (8, 4), Justification::topLeft);
        if (! shown || ! capture.validation)
            return;

        const auto& v = *capture.validation;
        // waveform window
        const size_t win = 960;
        size_t start = (size_t) (capture.audition.positionSeconds() * 48000.0f);
        if (start + win >= v.reference.size())
            start = v.reference.size() > win ? v.reference.size() / 2 : 0;
        auto plot = waveArea.toFloat().reduced (10, 20);
        float peak = 1e-6f;
        for (size_t i = start; i < start + win && i < v.reference.size(); ++i)
            peak = jmax (peak, std::abs (v.reference[i]), std::abs (v.model[i]));
        auto trace = [&] (const std::vector<float>& x, Colour c)
        {
            Path path;
            for (size_t i = 0; i < win && start + i < x.size(); ++i)
            {
                const float px = plot.getX() + plot.getWidth() * (float) i / (float) win;
                const float py = plot.getCentreY() - plot.getHeight() * 0.5f * x[start + i] / peak;
                i == 0 ? path.startNewSubPath (px, py) : path.lineTo (px, py);
            }
            g.setColour (c);
            g.strokePath (path, PathStrokeType (1.5f));
        };
        trace (v.reference, colours::accent);
        trace (v.model, colours::text.withAlpha (0.85f));

        // spectra
        if (! specRef.empty())
        {
            auto sp = specArea.toFloat().reduced (10, 20);
            float lo = 1e9f, hi = -1e9f;
            for (size_t i = 0; i < specRef.size(); ++i)
            {
                lo = jmin (lo, specRef[i], specModel[i]);
                hi = jmax (hi, specRef[i], specModel[i]);
            }
            lo = jmax (lo, hi - 90.0f);
            auto curve = [&] (const std::vector<float>& s, Colour c)
            {
                Path path;
                bool started = false;
                for (size_t i = 1; i < s.size(); ++i)
                {
                    const double f = (double) i * 48000.0 / (2.0 * (double) s.size());
                    if (f < 40.0 || f > 20000.0)
                        continue;
                    const float px = sp.getX() + sp.getWidth() * (float) (std::log (f / 40.0) / std::log (20000.0 / 40.0));
                    const float py = sp.getBottom() - sp.getHeight() * (jlimit (lo, hi, s[i]) - lo) / (hi - lo);
                    started ? path.lineTo (px, py) : path.startNewSubPath (px, py);
                    started = true;
                }
                g.setColour (c);
                g.strokePath (path, PathStrokeType (1.5f));
            };
            curve (specRef, colours::accent);
            curve (specModel, colours::text.withAlpha (0.85f));
        }
    }

    void resized() override
    {
        auto b = body();
        resultLabel.setBounds (b.removeFromTop (26));
        b.removeFromTop (6);
        auto row = b.removeFromTop (32);
        for (auto* btn : abButtons)
        {
            btn->setBounds (row.removeFromLeft (110));
            row.removeFromLeft (8);
        }
        b.removeFromTop (10);
        auto bottom = b.removeFromBottom (58);
        auto views = b;
        waveArea = views.removeFromLeft (views.getWidth() / 2).withTrimmedRight (6);
        specArea = views.withTrimmedLeft (6);
        bottom.removeFromTop (8);
        auto buttons = bottom.removeFromTop (30);
        saveButton.setBounds (buttons.removeFromLeft (150));
        buttons.removeFromLeft (8);
        loadButton.setBounds (buttons.removeFromLeft (150));
        buttons.removeFromLeft (8);
        discardButton.setBounds (buttons.removeFromLeft (110));
        savedLabel.setBounds (bottom);
    }

private:
    /** Averaged magnitude spectra (Hann, 4096-point FFT, 50% overlap). */
    void computeSpectra (const std::vector<float>& a, const std::vector<float>& b)
    {
        constexpr int n = 4096;
        auto spectrum = [] (const std::vector<float>& x)
        {
            std::vector<double> acc (n / 2, 0.0);
            std::vector<std::complex<double>> buf (n);
            int frames = 0;
            for (size_t start = 0; start + n <= x.size(); start += n / 2)
            {
                for (int i = 0; i < n; ++i)
                {
                    const double w = 0.5 - 0.5 * std::cos (2.0 * MathConstants<double>::pi * i / (n - 1));
                    buf[(size_t) i] = { x[start + (size_t) i] * w, 0.0 };
                }
                fft (buf);
                for (int i = 0; i < n / 2; ++i)
                    acc[(size_t) i] += std::norm (buf[(size_t) i]);
                ++frames;
            }
            std::vector<float> db (n / 2);
            for (int i = 0; i < n / 2; ++i)
                db[(size_t) i] = (float) (10.0 * std::log10 (acc[(size_t) i] / jmax (1, frames) + 1e-20));
            return db;
        };
        specRef = spectrum (a);
        specModel = spectrum (b);
    }

    static void fft (std::vector<std::complex<double>>& a)
    {
        const size_t n = a.size();
        for (size_t i = 1, j = 0; i < n; ++i)
        {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j)
                std::swap (a[i], a[j]);
        }
        for (size_t len = 2; len <= n; len <<= 1)
        {
            const double ang = -2.0 * MathConstants<double>::pi / (double) len;
            const std::complex<double> wl (std::cos (ang), std::sin (ang));
            for (size_t i = 0; i < n; i += len)
            {
                std::complex<double> w (1.0, 0.0);
                for (size_t k = 0; k < len / 2; ++k)
                {
                    const auto u = a[i + k], v = a[i + k + len / 2] * w;
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                    w *= wl;
                }
            }
        }
    }

    CaptureController& capture;
    std::function<void (const File&)> loadIntoAmp;
    OwnedArray<TextButton> abButtons;
    TextButton saveButton { "Save model" }, loadButton { "Load into AMP" }, discardButton { "Discard" };
    Label resultLabel, savedLabel;
    std::vector<float> specRef, specModel;
    Rectangle<int> waveArea, specArea;
    File savedFile;
    bool shown = false;
};

// ===========================================================================
CapturePage::StepButton::StepButton (int n, String t) : Button (t), number (n) {}

void CapturePage::StepButton::paintButton (Graphics& g, bool over, bool)
{
    auto r = getLocalBounds().toFloat().reduced (2.0f);
    const bool active = getToggleState();
    g.setColour (active ? colours::accent.withAlpha (0.16f) : (over ? colours::panelEdge : Colours::transparentBlack));
    g.fillRoundedRectangle (r, 6.0f);
    if (active)
    {
        g.setColour (colours::accent);
        g.fillRoundedRectangle (r.withWidth (3.0f), 1.5f);
    }
    auto badge = r.removeFromLeft (40).withSizeKeepingCentre (24, 24);
    g.setColour (done ? colours::ok : (active ? colours::accent : colours::panelEdge));
    g.fillEllipse (badge);
    g.setColour (colours::background);
    g.setFont (FontOptions (13.0f, Font::bold));
    g.drawText (done ? String::fromUTF8 ("\xe2\x9c\x93") : String (number), badge, Justification::centred);
    g.setColour (active ? colours::text : colours::textDim);
    g.setFont (FontOptions (14.0f, active ? Font::bold : Font::plain));
    g.drawText (getButtonText(), r, Justification::centredLeft);
}

CapturePage::CapturePage (GuitarAmpProcessor& p, std::function<void (const File&)> loadIntoAmp)
    : processor (p), capture (p.getCapture())
{
    const char* titles[] = { "Setup & routing", "Level check", "Capture", "Train", "Compare & save" };
    for (int i = 0; i < 5; ++i)
    {
        auto* b = stepButtons.add (new StepButton (i + 1, titles[i]));
        b->setClickingTogglesState (true);
        b->setRadioGroupId (501);
        b->onClick = [this, i] { showStep (i); };
        addAndMakeVisible (b);
    }

    setup = std::make_unique<SetupPanel> (p);
    level = std::make_unique<LevelPanel> (p);
    record = std::make_unique<CapturePanel> (p);
    train = std::make_unique<TrainPanel> (p, [this] { return setup->gearType(); });
    compare = std::make_unique<ComparePanel> (p, std::move (loadIntoAmp));
    panels = { setup.get(), level.get(), record.get(), train.get(), compare.get() };
    for (auto* c : panels)
        addChildComponent (c);

    showStep (0);
    startTimerHz (30);
}

CapturePage::~CapturePage()
{
    stopTimer();
    capture.audition.setMode (AuditionPlayer::Off);
}

void CapturePage::showStep (int index)
{
    currentStep = index;
    for (int i = 0; i < (int) panels.size(); ++i)
        panels[(size_t) i]->setVisible (i == index);
    stepButtons[index]->setToggleState (true, dontSendNotification);
    if (index != 4)
        capture.audition.setMode (AuditionPlayer::Off);
}

void CapturePage::refreshStepStates()
{
    const bool states[] = { capture.hasSignal() || capture.savedSignalPath().isNotEmpty(),
                            capture.levelCheck.has_value() && capture.levelCheck->latency.detected.has_value(),
                            capture.capture.has_value() && capture.capture->usable,
                            capture.validation.has_value() && ! capture.isTraining(), false };
    for (int i = 0; i < 5; ++i)
        if (stepButtons[i]->done != states[i])
        {
            stepButtons[i]->done = states[i];
            stepButtons[i]->repaint();
        }
}

void CapturePage::timerCallback()
{
    const bool changed = capture.poll();
    level->update();
    record->update();
    train->update();
    compare->processorRateOk = std::abs (processor.getHostSampleRate() - 48000.0) < 0.5;
    compare->update();
    if (changed || (frames % 15) == 0)
    {
        setup->refresh();
        refreshStepStates();
    }
    ++frames;
}

void CapturePage::paint (Graphics&) {}

void CapturePage::resized()
{
    auto r = getLocalBounds().reduced (14);
    stepArea = r.removeFromLeft (200);
    r.removeFromLeft (12);
    contentArea = r;
    auto s = stepArea;
    for (auto* b : stepButtons)
        b->setBounds (s.removeFromTop (46));
    for (auto* c : panels)
        c->setBounds (contentArea);
}
} // namespace ui
