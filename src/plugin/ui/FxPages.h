#pragma once

#include "plugin/PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace ui
{
/** One effect module: title, on switch and a control for each of its parameters
    (knobs for continuous values, menus for choices). Draggable for reordering. */
class ModulePanel : public juce::Component,
                    public juce::DragAndDropTarget,
                    public juce::SettableTooltipClient
{
public:
    ModulePanel (GuitarAmpProcessor&, const juce::String& moduleId, juce::Colour accent, int columns,
                 std::function<void (const juce::String& dragged, const juce::String& onto)> onReorder);
    ~ModulePanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDrag (const juce::MouseEvent&) override;

    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override { dropHover = true; repaint(); }
    void itemDragExit (const SourceDetails&) override { dropHover = false; repaint(); }
    void itemDropped (const SourceDetails&) override;

    const juce::String id;
    float controlsWidthFraction = 1.0f; // leave room on the right for extra content (EQ curve)
    void useFootswitch(); // pedal style: big footswitch at the bottom instead of the title switch

private:
    struct Control;
    GuitarAmpProcessor& processor;
    juce::Colour colour;
    int columns;
    std::function<void (const juce::String&, const juce::String&)> reorder;
    struct Footswitch : public juce::Button
    {
        Footswitch() : juce::Button ("On") { setClickingTogglesState (true); }
        void paintButton (juce::Graphics&, bool over, bool down) override;
    };
    juce::ToggleButton onButton;
    Footswitch footswitch;
    bool pedalStyle = false;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> onAttachment;
    std::vector<std::unique_ptr<Control>> controls;
    bool dropHover = false;
};

/** STOMP: input gate, compressor and the four drive/boost pedals on a reorderable pedalboard. */
class StompPage : public juce::Component,
                  public juce::DragAndDropContainer
{
public:
    explicit StompPage (GuitarAmpProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void rebuild();
    GuitarAmpProcessor& processor;
    juce::ToggleButton sectionOn { "Stomps on" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> sectionAttachment;
    juce::Label hint;
    std::vector<std::unique_ptr<ModulePanel>> pedals;
};

/** EFFECTS: parametric EQ with response curve and analyser, modulation, delay, reverb. */
class EffectsPage : public juce::Component,
                    public juce::DragAndDropContainer,
                    private juce::Timer
{
public:
    explicit EffectsPage (GuitarAmpProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void rebuild();
    void paintCurve (juce::Graphics&, juce::Rectangle<float>);
    struct Curve : public juce::Component
    {
        explicit Curve (EffectsPage& p) : page (p) { setInterceptsMouseClicks (false, false); }
        void paint (juce::Graphics& g) override { page.paintCurve (g, getLocalBounds().toFloat()); }
        EffectsPage& page;
    };
    Curve curve { *this };
    GuitarAmpProcessor& processor;
    juce::ToggleButton sectionOn { "Effects on" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> sectionAttachment;
    juce::Label orderLabel;
    std::vector<std::unique_ptr<ModulePanel>> units;
    juce::Rectangle<int> curveArea, orderArea;
    std::vector<float> spectrum;  // smoothed analyser, dB
    std::vector<float> fifoScratch;
    std::vector<float> window;
};
} // namespace ui
