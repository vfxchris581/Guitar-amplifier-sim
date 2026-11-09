#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace ui
{
/** Vertical peak meter (-60..+6 dBFS) with peak hold and a latching clip LED.
    Feed it with setPeak() from a UI timer; click the meter to reset the clip LED. */
class LevelMeter : public juce::Component,
                   public juce::SettableTooltipClient
{
public:
    explicit LevelMeter (juce::String caption);

    void setPeak (float linearPeak); // call at the UI refresh rate
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    static float dbToY (float db, juce::Rectangle<float> bar);

    juce::String caption;
    float levelDb = -100.0f;
    float holdDb = -100.0f;
    int holdFrames = 0;
    bool clipped = false;
};
} // namespace ui
