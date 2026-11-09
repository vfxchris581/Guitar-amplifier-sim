#pragma once

#include "plugin/PluginProcessor.h"
#include "plugin/ui/LevelMeter.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace ui
{
/** The CAPTURE section: profile a real amplifier without leaving the plugin.
    Steps: 1 Setup & routing, 2 Level check, 3 Capture, 4 Train, 5 Compare & save. */
class CapturePage : public juce::Component,
                    private juce::Timer
{
public:
    CapturePage (GuitarAmpProcessor&, std::function<void (const juce::File&)> loadIntoAmp);
    ~CapturePage() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void showStep (int index);
    void refreshNow() { timerCallback(); }

private:
    void timerCallback() override;
    void refreshStepStates();

    GuitarAmpProcessor& processor;
    CaptureController& capture;

    struct StepButton : public juce::Button
    {
        StepButton (int number, juce::String title);
        void paintButton (juce::Graphics&, bool over, bool down) override;
        int number;
        bool done = false;
    };
    juce::OwnedArray<StepButton> stepButtons;

    class SetupPanel;
    class LevelPanel;
    class CapturePanel;
    class TrainPanel;
    class ComparePanel;
    std::unique_ptr<SetupPanel> setup;
    std::unique_ptr<LevelPanel> level;
    std::unique_ptr<CapturePanel> record;
    std::unique_ptr<TrainPanel> train;
    std::unique_ptr<ComparePanel> compare;
    std::vector<juce::Component*> panels;
    int currentStep = 0;
    int frames = 0;
    juce::Rectangle<int> contentArea, stepArea;
};
} // namespace ui
