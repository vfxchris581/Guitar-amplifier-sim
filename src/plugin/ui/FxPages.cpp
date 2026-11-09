#include "plugin/ui/FxPages.h"
#include "plugin/ui/Theme.h"

#include <complex>

namespace ui
{
using namespace juce;

namespace
{
Colour pedalColour (const String& id)
{
    if (id == "gate")   return Colour (0xff7d8a96);
    if (id == "comp")   return Colour (0xff3f8fd2);
    if (id == "od")     return Colour (0xff4cae6a);
    if (id == "boost")  return Colour (0xffd9d4c7);
    if (id == "dist")   return Colour (0xffd8a33a);
    if (id == "fuzz")   return Colour (0xffc8553d);
    if (id == "eq")     return Colour (0xffb8b2a7);
    if (id == "mod")    return Colour (0xff8a6bd1);
    if (id == "delay")  return Colour (0xff3fb3b3);
    return Colour (0xff6c8ed6);
}

String shortLabel (const String& paramName)
{
    static const StringPairArray names = []
    {
        StringPairArray a;
        a.set ("threshold", "THRESH"); a.set ("release", "RELEASE"); a.set ("ratio", "RATIO"); a.set ("attack", "ATTACK");
        a.set ("makeup", "MAKEUP"); a.set ("mix", "MIX"); a.set ("drive", "DRIVE"); a.set ("tone", "TONE"); a.set ("level", "LEVEL");
        a.set ("gain", "GAIN"); a.set ("output", "OUTPUT"); a.set ("fuzz", "FUZZ"); a.set ("type", "TYPE"); a.set ("rate", "RATE");
        a.set ("depth", "DEPTH"); a.set ("feedback", "FEEDBACK"); a.set ("time", "TIME"); a.set ("sync", "SYNC");
        a.set ("lowcut", "LOW CUT"); a.set ("highcut", "HIGH CUT"); a.set ("width", "WIDTH"); a.set ("decay", "DECAY");
        a.set ("predelay", "PRE-DELAY"); a.set ("size", "SIZE"); a.set ("damping", "DAMPING"); a.set ("hpf", "HPF"); a.set ("lpf", "LPF");
        a.set ("lowFreq", "LOW F"); a.set ("lowGain", "LOW"); a.set ("highFreq", "HIGH F"); a.set ("highGain", "HIGH");
        for (int b = 1; b <= 4; ++b)
        {
            a.set ("b" + String (b) + "Freq", "B" + String (b) + " FREQ");
            a.set ("b" + String (b) + "Gain", "B" + String (b) + " GAIN");
            a.set ("b" + String (b) + "Q", "B" + String (b) + " Q");
        }
        return a;
    }();
    return names[paramName].isNotEmpty() ? names[paramName] : paramName.toUpperCase();
}

String moduleName (const String& id)
{
    for (const auto& m : fx::allModules())
        if (id == m.id)
            return m.name;
    return id;
}
} // namespace

// ===========================================================================
struct ModulePanel::Control : public Component
{
    Control (AudioProcessorValueTreeState& state, const String& paramId, const String& caption)
    {
        auto* prm = state.getParameter (paramId);
        label.setText (caption, dontSendNotification);
        label.setJustificationType (Justification::centred);
        label.setFont (FontOptions (10.5f, Font::bold));
        label.setColour (Label::textColourId, colours::textDim);
        label.setMinimumHorizontalScale (0.7f);
        addAndMakeVisible (label);
        if (auto* choice = dynamic_cast<AudioParameterChoice*> (prm))
        {
            box = std::make_unique<ComboBox>();
            box->addItemList (choice->choices, 1);
            boxAttachment = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment> (state, paramId, *box);
            addAndMakeVisible (*box);
        }
        else
        {
            slider.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
            slider.setTextBoxStyle (Slider::TextBoxBelow, false, 64, 15);
            slider.setRotaryParameters (MathConstants<float>::pi * 1.25f, MathConstants<float>::pi * 2.75f, true);
            slider.setTooltip (prm->getName (64));
            sliderAttachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (state, paramId, slider);
            slider.setDoubleClickReturnValue (true, (double) prm->convertFrom0to1 (prm->getDefaultValue()));
            addAndMakeVisible (slider);
        }
    }
    void resized() override
    {
        auto r = getLocalBounds();
        label.setBounds (r.removeFromTop (14));
        if (box)
            box->setBounds (r.withSizeKeepingCentre (r.getWidth() - 4, 24));
        else
            slider.setBounds (r);
    }
    Label label;
    Slider slider;
    std::unique_ptr<ComboBox> box;
    std::unique_ptr<AudioProcessorValueTreeState::SliderAttachment> sliderAttachment;
    std::unique_ptr<AudioProcessorValueTreeState::ComboBoxAttachment> boxAttachment;
};

ModulePanel::ModulePanel (GuitarAmpProcessor& p, const String& moduleId, Colour c, int cols,
                          std::function<void (const String&, const String&)> onReorder)
    : id (moduleId), processor (p), colour (c), columns (cols), reorder (std::move (onReorder))
{
    auto& state = p.getParameters();
    onButton.setButtonText ("On");
    onAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (state, id + "_on", onButton);
    onButton.onStateChange = [this] { repaint(); };
    addAndMakeVisible (onButton);
    for (const auto& name : fx::parameterNames (id))
    {
        controls.push_back (std::make_unique<Control> (state, id + "_" + name, shortLabel (name)));
        addAndMakeVisible (*controls.back());
    }
    setTooltip ("Drag the title to move " + moduleName (id) + " in the chain.");
}

ModulePanel::~ModulePanel() = default;

void ModulePanel::useFootswitch()
{
    pedalStyle = true;
    onAttachment.reset();
    removeChildComponent (&onButton);
    onAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (processor.getParameters(), id + "_on", footswitch);
    footswitch.onStateChange = [this] { repaint(); };
    footswitch.setTooltip ("Switch " + moduleName (id) + " on / off");
    addAndMakeVisible (footswitch);
    resized();
}

void ModulePanel::Footswitch::paintButton (Graphics& g, bool over, bool down)
{
    auto r = getLocalBounds().toFloat().withSizeKeepingCentre (46.0f, 46.0f);
    g.setGradientFill (ColourGradient (Colour (0xffc9c9cf), r.getX(), r.getY(), Colour (0xff6b6b72), r.getRight(), r.getBottom(), false));
    g.fillEllipse (r);
    g.setColour (Colours::black.withAlpha (0.6f));
    g.drawEllipse (r, 1.5f);
    auto inner = r.reduced (down ? 9.0f : 8.0f);
    g.setGradientFill (ColourGradient (Colour (0xffe9e9ee), inner.getX(), inner.getY(), Colour (0xff8d8d94), inner.getRight(), inner.getBottom(), false));
    g.fillEllipse (inner);
    if (over)
    {
        g.setColour (colours::accent.withAlpha (0.5f));
        g.drawEllipse (r.expanded (2.0f), 1.5f);
    }
}

void ModulePanel::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const bool on = pedalStyle ? footswitch.getToggleState() : onButton.getToggleState();
    g.setColour (colours::panel);
    g.fillRoundedRectangle (r, 9.0f);
    g.setColour (colour.withAlpha (on ? 0.9f : 0.35f));
    g.fillRoundedRectangle (r.removeFromTop (5.0f), 2.5f);
    g.setColour (dropHover ? colours::accent : colours::panelEdge);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 9.0f, dropHover ? 2.0f : 1.0f);

    auto title = getLocalBounds().reduced (12, 8).removeFromTop (24);
    // LED
    auto led = title.removeFromLeft (14).withSizeKeepingCentre (10, 10).toFloat();
    g.setColour (on ? colour.brighter (0.4f) : colours::well);
    g.fillEllipse (led);
    if (on)
    {
        g.setColour (colour.withAlpha (0.35f));
        g.fillEllipse (led.expanded (3.0f));
    }
    title.removeFromLeft (6);
    g.setColour (on ? colours::text : colours::textDim);
    if (! pedalStyle)
        title.removeFromRight (64);
    g.setFont (FontOptions (14.0f, Font::bold));
    g.drawFittedText (moduleName (id).toUpperCase(), title, Justification::centredLeft, 1, 0.7f);
}

void ModulePanel::resized()
{
    auto r = getLocalBounds().reduced (10, 8);
    auto title = r.removeFromTop (24);
    if (pedalStyle)
        footswitch.setBounds (r.removeFromBottom (64).withSizeKeepingCentre (56, 56));
    else
        onButton.setBounds (title.removeFromRight (64));
    r.removeFromTop (6);
    r = r.withWidth ((int) ((float) r.getWidth() * controlsWidthFraction));
    const int n = (int) controls.size();
    const int cols = jmax (1, jmin (columns, n));
    const int rows = (n + cols - 1) / cols;
    const int cw = r.getWidth() / cols, rh = jmin (r.getHeight() / jmax (1, rows), 96);
    for (int i = 0; i < n; ++i)
        controls[(size_t) i]->setBounds (r.getX() + (i % cols) * cw, r.getY() + (i / cols) * rh, cw, rh);
}

void ModulePanel::mouseDrag (const MouseEvent& e)
{
    if (e.getMouseDownY() < 36)
        if (auto* container = DragAndDropContainer::findParentDragContainerFor (this))
            if (! container->isDragAndDropActive())
                container->startDragging (id, this);
}

bool ModulePanel::isInterestedInDragSource (const SourceDetails& d)
{
    const String dragged = d.description.toString();
    const int a = fx::moduleIndex (dragged), b = fx::moduleIndex (id);
    return a >= 0 && dragged != id && fx::allModules()[(size_t) a].preAmp == fx::allModules()[(size_t) b].preAmp;
}

void ModulePanel::itemDropped (const SourceDetails& d)
{
    dropHover = false;
    repaint();
    if (reorder)
        reorder (d.description.toString(), id);
}

namespace
{
/** Moves `dragged` to the position of `onto` in a comma-separated order. */
String moveInOrder (const String& order, const String& dragged, const String& onto)
{
    StringArray ids;
    ids.addTokens (order, ",", {});
    const int from = ids.indexOf (dragged), to = ids.indexOf (onto);
    if (from < 0 || to < 0)
        return order;
    ids.move (from, to);
    return ids.joinIntoString (",");
}
} // namespace

// ===========================================================================
StompPage::StompPage (GuitarAmpProcessor& p) : processor (p)
{
    sectionAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (p.getParameters(), "stomps_on", sectionOn);
    addAndMakeVisible (sectionOn);
    hint.setText ("Signal flows left to right into the amp. Drag a pedal by its name to reorder. "
                  "Drives run 4x oversampled.", dontSendNotification);
    hint.setFont (FontOptions (13.0f));
    hint.setColour (Label::textColourId, colours::textDim);
    addAndMakeVisible (hint);
    rebuild();
}

void StompPage::rebuild()
{
    pedals.clear();
    StringArray order;
    order.addTokens (processor.getChain().getOrder (true), ",", {});
    for (const auto& id : order)
    {
        pedals.push_back (std::make_unique<ModulePanel> (processor, id, pedalColour (id), 2,
                                                          [this] (const String& dragged, const String& onto)
                                                          {
                                                              auto& chain = processor.getChain();
                                                              chain.setOrder (true, moveInOrder (chain.getOrder (true), dragged, onto));
                                                              MessageManager::callAsync ([safe = Component::SafePointer<StompPage> (this)]
                                                                                         { if (safe) { safe->rebuild(); safe->resized(); } });
                                                          }));
        pedals.back()->useFootswitch();
        addAndMakeVisible (*pedals.back());
    }
}

void StompPage::paint (Graphics& g)
{
    // the cable from guitar to amp
    auto r = getLocalBounds().reduced (14);
    r.removeFromTop (40);
    const float y = (float) r.getCentreY();
    g.setColour (colours::panelEdge);
    g.drawLine ((float) r.getX(), y, (float) r.getRight(), y, 3.0f);
    g.setColour (colours::textDim);
    g.setFont (FontOptions (11.0f, Font::bold));
    g.drawText ("GUITAR", r.withWidth (60).withY ((int) y + 6).withHeight (14), Justification::centredLeft);
    g.drawText ("AMP", r.removeFromRight (40).withY ((int) y + 6).withHeight (14), Justification::centredRight);
}

void StompPage::resized()
{
    auto r = getLocalBounds().reduced (14);
    auto top = r.removeFromTop (30);
    sectionOn.setBounds (top.removeFromLeft (130));
    hint.setBounds (top);
    r.removeFromTop (10);
    r = r.reduced (36, 0);
    const int n = (int) pedals.size();
    const int gap = 10, w = (r.getWidth() - gap * (n - 1)) / jmax (1, n);
    for (int i = 0; i < n; ++i)
        pedals[(size_t) i]->setBounds (r.getX() + i * (w + gap), r.getY(), w, r.getHeight());
}

// ===========================================================================
EffectsPage::EffectsPage (GuitarAmpProcessor& p) : processor (p)
{
    sectionAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (p.getParameters(), "fx_on", sectionOn);
    addAndMakeVisible (sectionOn);
    orderLabel.setFont (FontOptions (13.0f));
    orderLabel.setColour (Label::textColourId, colours::textDim);
    addAndMakeVisible (orderLabel);
    window.resize (2048);
    for (size_t i = 0; i < window.size(); ++i)
        window[i] = 0.5f - 0.5f * std::cos (2.0f * MathConstants<float>::pi * (float) i / (float) (window.size() - 1));
    fifoScratch.reserve (8192);
    rebuild();
    startTimerHz (25);
}

void EffectsPage::rebuild()
{
    units.clear();
    StringArray order;
    order.addTokens (processor.getChain().getOrder (false), ",", {});
    for (const auto& id : order)
    {
        units.push_back (std::make_unique<ModulePanel> (processor, id, pedalColour (id), id == "eq" ? 9 : 4,
                                                         [this] (const String& dragged, const String& onto)
                                                         {
                                                             auto& chain = processor.getChain();
                                                             chain.setOrder (false, moveInOrder (chain.getOrder (false), dragged, onto));
                                                             MessageManager::callAsync ([safe = Component::SafePointer<EffectsPage> (this)]
                                                                                        { if (safe) { safe->rebuild(); safe->resized(); } });
                                                         }));
        if (id == "eq")
            units.back()->controlsWidthFraction = 0.68f;
        addAndMakeVisible (*units.back());
    }
    addAndMakeVisible (curve);
    curve.toFront (false);
    StringArray names;
    for (const auto& id : order)
        names.add (moduleName (id));
    orderLabel.setText ("After the cab: " + names.joinIntoString (String::fromUTF8 ("  \xe2\x86\x92  ")) + "   (drag a unit's name to reorder)",
                        dontSendNotification);
}

void EffectsPage::timerCallback()
{
    // analyser: newest 2048 samples after the EQ
    float tmp[4096];
    int got;
    while ((got = processor.getChain().analyser.pull (tmp, 4096)) > 0)
        fifoScratch.insert (fifoScratch.end(), tmp, tmp + got);
    if (fifoScratch.size() > 2048)
        fifoScratch.erase (fifoScratch.begin(), fifoScratch.end() - 2048);
    if (fifoScratch.size() == 2048)
    {
        std::vector<std::complex<float>> a (2048);
        for (size_t i = 0; i < 2048; ++i)
            a[i] = { fifoScratch[i] * window[i], 0.0f };
        // in-place radix-2 FFT
        for (size_t i = 1, j = 0; i < 2048; ++i)
        {
            size_t bit = 1024;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap (a[i], a[j]);
        }
        for (size_t len = 2; len <= 2048; len <<= 1)
        {
            const float ang = -2.0f * MathConstants<float>::pi / (float) len;
            const std::complex<float> wl (std::cos (ang), std::sin (ang));
            for (size_t i = 0; i < 2048; i += len)
            {
                std::complex<float> w (1.0f, 0.0f);
                for (size_t k = 0; k < len / 2; ++k, w *= wl)
                {
                    const auto u = a[i + k], v = a[i + k + len / 2] * w;
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                }
            }
        }
        spectrum.resize (1024, -120.0f);
        for (size_t i = 0; i < 1024; ++i)
        {
            const float db = 20.0f * std::log10 (std::abs (a[i]) / 512.0f + 1e-9f);
            spectrum[i] = db > spectrum[i] ? db : spectrum[i] - 1.5f;
        }
    }
    curve.repaint();
}

void EffectsPage::paint (Graphics&) {}

void EffectsPage::paintCurve (Graphics& g, Rectangle<float> r)
{
    // EQ response + analyser, drawn inside the EQ unit's free area
    g.setColour (colours::well);
    g.fillRoundedRectangle (r, 6.0f);
    const double rate = processor.getSampleRate() > 0 ? processor.getSampleRate() : 48000.0;
    auto xOf = [&] (float hz) { return r.getX() + r.getWidth() * std::log (hz / 20.0f) / std::log (20000.0f / 20.0f); };
    auto yOfDb = [&] (float db, float range) { return r.getCentreY() - r.getHeight() * 0.5f * jlimit (-1.0f, 1.0f, db / range); };

    g.setColour (colours::panelEdge);
    for (float f : { 100.0f, 1000.0f, 10000.0f })
        g.drawVerticalLine ((int) xOf (f), r.getY(), r.getBottom());
    g.drawHorizontalLine ((int) r.getCentreY(), r.getX(), r.getRight());

    if (! spectrum.empty())
    {
        Path sp;
        bool started = false;
        for (size_t i = 1; i < spectrum.size(); ++i)
        {
            const float hz = (float) i * (float) rate / 2048.0f;
            if (hz < 20.0f || hz > 20000.0f) continue;
            const float y = r.getBottom() - r.getHeight() * jlimit (0.0f, 1.0f, (spectrum[i] + 90.0f) / 90.0f);
            started ? sp.lineTo (xOf (hz), y) : sp.startNewSubPath (xOf (hz), y);
            started = true;
        }
        g.setColour (colours::textDim.withAlpha (0.45f));
        g.strokePath (sp, PathStrokeType (1.0f));
    }

    std::vector<float> freqs;
    for (int i = 0; i <= 200; ++i)
        freqs.push_back (20.0f * std::pow (1000.0f, (float) i / 200.0f));
    const auto resp = fx::eqResponseDb (processor.getParameters(), freqs, rate);
    Path curve;
    for (size_t i = 0; i < freqs.size(); ++i)
    {
        const float x = xOf (freqs[i]), y = yOfDb (resp[i], 18.0f);
        i == 0 ? curve.startNewSubPath (x, y) : curve.lineTo (x, y);
    }
    g.setColour (pedalColour ("eq").brighter (0.3f));
    g.strokePath (curve, PathStrokeType (2.0f));
    g.setColour (colours::textDim);
    g.setFont (FontOptions (10.5f));
    g.drawText ("EQ response (+/-18 dB) over the live spectrum", r.reduced (6, 3), Justification::topLeft);
}

void EffectsPage::resized()
{
    auto r = getLocalBounds().reduced (14);
    auto top = r.removeFromTop (30);
    sectionOn.setBounds (top.removeFromLeft (130));
    orderLabel.setBounds (top);
    r.removeFromTop (10);
    auto upper = r.removeFromTop (r.getHeight() / 2 - 5);
    r.removeFromTop (10);
    ModulePanel* eq = nullptr;
    std::vector<ModulePanel*> others;
    for (auto& u : units)
    {
        if (u->id == "eq")
            eq = u.get();
        else
            others.push_back (u.get());
    }
    if (eq != nullptr)
    {
        eq->setBounds (upper);
        curve.setBounds (upper.reduced (12).withTrimmedTop (34).removeFromRight ((int) (upper.getWidth() * 0.30f)));
    }
    const int w = (r.getWidth() - 20) / 3;
    for (size_t i = 0; i < others.size(); ++i)
        others[i]->setBounds (r.getX() + (int) i * (w + 10), r.getY(), w, r.getHeight());
}
} // namespace ui
