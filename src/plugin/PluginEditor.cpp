#include "plugin/PluginEditor.h"

using namespace juce;

namespace
{
constexpr int kHeaderHeight = 52;
constexpr int kFooterHeight = 26;
} // namespace

GuitarAmpEditor::GuitarAmpEditor (GuitarAmpProcessor& p)
    : AudioProcessorEditor (p),
      processor (p),
      ampPage (p),
      capturePage (p, [this] (const File& f)
                   {
                       processor.loadModel (f);
                       showSection (0);
                   }),
      cabPage (p),
      stompPage (p),
      effectsPage (p)
{
    setLookAndFeel (&theme);
    sections = { &ampPage, &capturePage, &cabPage, &stompPage, &effectsPage };
    for (auto* c : sections)
        addChildComponent (c);
    ampPage.setVisible (true);

    // Sections appear here as they are built: AMP, CAPTURE, then CAB, STOMP, EFFECTS, LIBRARY, SETTINGS.
    for (auto name : { "AMP", "CAPTURE", "CAB", "STOMP", "EFFECTS" })
    {
        auto* t = tabs.add (new SectionTab (name));
        t->setClickingTogglesState (true);
        t->setRadioGroupId (1001);
        const int index = tabs.size() - 1;
        t->onClick = [this, index] { showSection (index); };
        addAndMakeVisible (t);
    }
    tabs[0]->setToggleState (true, dontSendNotification);

    setResizable (true, true);
    setResizeLimits (860, 540, 1600, 1000);
    setSize (980, 600);
    startTimerHz (4);
}

GuitarAmpEditor::~GuitarAmpEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void GuitarAmpEditor::SectionTab::paintButton (Graphics& g, bool over, bool)
{
    const bool on = getToggleState();
    g.setColour (on ? ui::colours::accent : (over ? ui::colours::text : ui::colours::textDim));
    g.setFont (FontOptions (13.0f, Font::bold));
    g.drawText (getButtonText(), getLocalBounds(), Justification::centred);
    if (on)
        g.fillRect (getLocalBounds().removeFromBottom (3).reduced (12, 0));
}

void GuitarAmpEditor::showSection (int index)
{
    for (size_t i = 0; i < sections.size(); ++i)
        sections[i]->setVisible ((int) i == index);
    tabs[index]->setToggleState (true, dontSendNotification);
}

void GuitarAmpEditor::timerCallback()
{
    const auto status = processor.getEngine().getStatus();
    String t;
    if (status.hostSampleRate > 0)
        t << String (status.hostSampleRate / 1000.0, 1) << " kHz";
    t << "   |   Latency " << processor.getEngine().getLatencySamples() << " samples";
    t << "   |   CPU " << String (processor.getCpuLoadPercent(), 1) << "%";
    if (processor.getXRunCount() > 0)
        t << "   |   Overloads " << processor.getXRunCount();
    if (t != footerText)
    {
        footerText = t;
        repaint (getLocalBounds().removeFromBottom (kFooterHeight));
    }
}

void GuitarAmpEditor::paint (Graphics& g)
{
    g.fillAll (ui::colours::background);

    auto header = getLocalBounds().removeFromTop (kHeaderHeight);
    g.setColour (ui::colours::panel);
    g.fillRect (header);
    g.setColour (ui::colours::panelEdge);
    g.drawHorizontalLine (header.getBottom() - 1, 0.0f, (float) getWidth());

    auto h = header.reduced (18, 0);
    g.setColour (ui::colours::text);
    g.setFont (FontOptions (20.0f, Font::bold));
    const String title (GAS_PRODUCT_NAME);
    g.drawText (title.toUpperCase(), h, Justification::centredLeft);

    g.setColour (ui::colours::textDim);
    g.setFont (FontOptions (12.0f));
    g.drawText ("Neural Amp Modeler engine  |  v" GAS_VERSION_STRING, h, Justification::centredRight);

    auto footer = getLocalBounds().removeFromBottom (kFooterHeight).reduced (18, 0);
    g.setColour (ui::colours::textDim);
    g.setFont (FontOptions (12.0f));
    g.drawText (footerText, footer, Justification::centredLeft);
}

void GuitarAmpEditor::resized()
{
    auto r = getLocalBounds();
    r.removeFromTop (kHeaderHeight);
    r.removeFromBottom (kFooterHeight);
    for (auto* c : sections)
        c->setBounds (r);

    const int titleW = GlyphArrangement::getStringWidthInt (FontOptions (20.0f, Font::bold),
                                                            String (GAS_PRODUCT_NAME).toUpperCase());
    auto tabRow = Rectangle<int> (18 + titleW + 20, 0, 600, kHeaderHeight);
    for (auto* t : tabs)
        t->setBounds (tabRow.removeFromLeft (t->getButtonText().length() * 11 + 34));
}
