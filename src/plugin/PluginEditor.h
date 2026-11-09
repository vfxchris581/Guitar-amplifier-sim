#pragma once

#include "plugin/PluginProcessor.h"
#include "plugin/ui/AmpPage.h"
#include "plugin/ui/Theme.h"

class GuitarAmpEditor : public juce::AudioProcessorEditor,
                        private juce::Timer
{
public:
    explicit GuitarAmpEditor (GuitarAmpProcessor&);
    ~GuitarAmpEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    GuitarAmpProcessor& processor;
    ui::Theme theme;
    juce::TooltipWindow tooltips { this, 600 };
    ui::AmpPage ampPage;
    juce::String footerText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GuitarAmpEditor)
};
