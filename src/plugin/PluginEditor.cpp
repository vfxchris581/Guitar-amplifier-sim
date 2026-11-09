#include "plugin/PluginEditor.h"

using namespace juce;

namespace
{
constexpr int kHeaderHeight = 52;
constexpr int kFooterHeight = 26;
} // namespace

GuitarAmpEditor::GuitarAmpEditor (GuitarAmpProcessor& p)
    : AudioProcessorEditor (p), processor (p), ampPage (p)
{
    setLookAndFeel (&theme);
    addAndMakeVisible (ampPage);

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

    // The section currently built. Further sections (CAPTURE, CAB, ...) are added
    // to this bar as they are implemented.
    const int titleW = GlyphArrangement::getStringWidthInt (FontOptions (20.0f, Font::bold), title.toUpperCase());
    auto tab = Rectangle<int> (h.getX() + titleW + 28, header.getY(), 70, header.getHeight());
    g.setColour (ui::colours::accent);
    g.setFont (FontOptions (13.0f, Font::bold));
    g.drawText ("AMP", tab, Justification::centred);
    g.fillRect (tab.removeFromBottom (3).reduced (12, 0));

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
    ampPage.setBounds (r);
}
