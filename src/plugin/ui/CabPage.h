#pragma once

#include "plugin/IRLibrary.h"
#include "plugin/PluginProcessor.h"
#include "plugin/ui/AmpPage.h"

namespace ui
{
/** CAB section: two-microphone impulse-response cabinet plus the IR library browser. */
class CabPage : public juce::Component,
                public juce::FileDragAndDropTarget,
                private juce::Timer
{
public:
    explicit CabPage (GuitarAmpProcessor&);
    ~CabPage() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;

private:
    void timerCallback() override;
    void loadIntoMic (int mic, const juce::File&);
    void refreshBrowser();

    class MicStrip;
    class Browser;

    GuitarAmpProcessor& processor;
    IRLibrary library;
    juce::ComboBox modeBox;
    juce::Label modeLabel, statusLabel;
    juce::AudioProcessorValueTreeState::ComboBoxAttachment modeAttachment;
    std::unique_ptr<MicStrip> strips[2];
    std::unique_ptr<Browser> browser;
    juce::Rectangle<int> topArea;
};
} // namespace ui
