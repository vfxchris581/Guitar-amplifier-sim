#pragma once

#include "plugin/PluginProcessor.h"
#include "plugin/ui/LevelMeter.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace ui
{
/** A rotary knob with a caption and a parameter attachment. */
struct Knob : public juce::Component
{
    Knob (juce::AudioProcessorValueTreeState&, const juce::String& paramId, const juce::String& caption,
          const juce::String& tooltip);
    void resized() override;

    juce::Slider slider;
    juce::Label label;
    juce::AudioProcessorValueTreeState::SliderAttachment attachment;
};

/** The AMP section: model loading, model information, gain staging and meters. */
class AmpPage : public juce::Component,
                public juce::FileDragAndDropTarget,
                private juce::Timer
{
public:
    explicit AmpPage (GuitarAmpProcessor&);
    ~AmpPage() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

private:
    void timerCallback() override;
    void refreshModelInfo();
    void updateControlStates();
    void browseForModel();

    GuitarAmpProcessor& processor;
    juce::AudioProcessorValueTreeState& params;

    // model panel
    juce::Label modelName, modelSubtitle, statusLabel;
    juce::TextButton loadButton { "Load Model..." }, unloadButton { "Unload" };
    struct InfoRow { juce::Label key, value; };
    std::vector<std::unique_ptr<InfoRow>> infoRows;
    std::unique_ptr<juce::FileChooser> chooser;
    std::uint64_t shownStatusSerial = ~std::uint64_t (0);
    bool dragHover = false;

    // gain staging
    Knob inputKnob, outputKnob, interfaceInKnob, interfaceOutKnob;
    juce::ComboBox outputModeBox, inputChannelBox;
    juce::Label outputModeLabel, inputChannelLabel;
    juce::ToggleButton calibrateToggle { "Calibrate input to the model's capture level" };
    juce::AudioProcessorValueTreeState::ComboBoxAttachment outputModeAttachment, inputChannelAttachment;
    juce::AudioProcessorValueTreeState::ButtonAttachment calibrateAttachment;

    // meters
    LevelMeter inputMeter { "IN" }, outputMeter { "OUT" };

    juce::Rectangle<int> modelArea, gainArea, calibArea, meterArea;
};
} // namespace ui
