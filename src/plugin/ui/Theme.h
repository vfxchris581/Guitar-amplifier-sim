#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace ui
{
namespace colours
{
inline const juce::Colour background { 0xff121214 };
inline const juce::Colour panel      { 0xff1b1b1f };
inline const juce::Colour panelEdge  { 0xff2b2b31 };
inline const juce::Colour well       { 0xff0d0d0f };
inline const juce::Colour text       { 0xffece9e4 };
inline const juce::Colour textDim    { 0xff8d8a85 };
inline const juce::Colour accent     { 0xffe7a23c }; // warm valve amber
inline const juce::Colour ok         { 0xff63c27b };
inline const juce::Colour warning    { 0xffe7c23c };
inline const juce::Colour error      { 0xffe2574c };
} // namespace colours

class Theme : public juce::LookAndFeel_V4
{
public:
    Theme();

    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float start, float end,
                           juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool over, bool down) override;
    void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int) override;
    juce::Label* createSliderTextBox (juce::Slider&) override;
};

/** Rounded panel with an optional caption, used to group controls. */
void drawPanel (juce::Graphics&, juce::Rectangle<float> area, const juce::String& caption = {});

} // namespace ui
