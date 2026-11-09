#pragma once

#include "plugin/PluginProcessor.h"
#include "plugin/ui/AmpPage.h"
#include "plugin/ui/CabPage.h"
#include "plugin/ui/CapturePage.h"
#include "plugin/ui/Theme.h"

class GuitarAmpEditor : public juce::AudioProcessorEditor,
                        private juce::Timer
{
public:
    explicit GuitarAmpEditor (GuitarAmpProcessor&);
    ~GuitarAmpEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void showSection (int index);
    ui::CapturePage& getCapturePage() { return capturePage; }

private:
    void timerCallback() override;

    struct SectionTab : public juce::Button
    {
        explicit SectionTab (const juce::String& name) : juce::Button (name) {}
        void paintButton (juce::Graphics&, bool over, bool down) override;
    };

    GuitarAmpProcessor& processor;
    ui::Theme theme;
    juce::TooltipWindow tooltips { this, 600 };
    ui::AmpPage ampPage;
    ui::CapturePage capturePage;
    ui::CabPage cabPage;
    juce::OwnedArray<SectionTab> tabs;
    juce::String footerText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GuitarAmpEditor)
};
