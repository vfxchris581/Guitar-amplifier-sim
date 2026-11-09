#include "plugin/ui/Theme.h"

namespace ui
{
using namespace juce;

Theme::Theme()
{
    setColour (ResizableWindow::backgroundColourId, colours::background);
    setColour (Label::textColourId, colours::text);
    setColour (Slider::textBoxTextColourId, colours::text);
    setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    setColour (Slider::textBoxBackgroundColourId, Colours::transparentBlack);
    setColour (Slider::rotarySliderFillColourId, colours::accent);
    setColour (ComboBox::backgroundColourId, colours::well);
    setColour (ComboBox::outlineColourId, colours::panelEdge);
    setColour (ComboBox::textColourId, colours::text);
    setColour (ComboBox::arrowColourId, colours::textDim);
    setColour (PopupMenu::backgroundColourId, colours::panel);
    setColour (PopupMenu::textColourId, colours::text);
    setColour (PopupMenu::highlightedBackgroundColourId, colours::accent.withAlpha (0.25f));
    setColour (PopupMenu::highlightedTextColourId, colours::text);
    setColour (TextButton::buttonColourId, colours::panelEdge);
    setColour (TextButton::textColourOffId, colours::text);
    setColour (TextButton::textColourOnId, colours::text);
    setColour (ToggleButton::textColourId, colours::text);
    setColour (TooltipWindow::backgroundColourId, colours::panel);
    setColour (TooltipWindow::textColourId, colours::text);
    setColour (TooltipWindow::outlineColourId, colours::panelEdge);
    setColour (AlertWindow::backgroundColourId, colours::panel);
}

void Theme::drawRotarySlider (Graphics& g, int x, int y, int w, int h, float pos, float start, float end, Slider& s)
{
    const auto bounds = Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (4.0f);
    const float radius = jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float angle = start + pos * (end - start);
    const float trackWidth = jmax (3.0f, radius * 0.09f);
    const float arcRadius = radius - trackWidth;
    const bool enabled = s.isEnabled();
    const auto fill = enabled ? colours::accent : colours::textDim;

    Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, start, end, true);
    g.setColour (colours::well);
    g.strokePath (track, PathStrokeType (trackWidth, PathStrokeType::curved, PathStrokeType::rounded));

    // Bipolar ranges (e.g. -24..+24 dB) fill from the centre, unipolar from the start.
    const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0;
    const float zeroAngle = bipolar ? start + (float) s.valueToProportionOfLength (0.0) * (end - start) : start;
    Path value;
    value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, jmin (zeroAngle, angle), jmax (zeroAngle, angle), true);
    g.setColour (fill);
    g.strokePath (value, PathStrokeType (trackWidth, PathStrokeType::curved, PathStrokeType::rounded));

    // Knob body
    const float bodyR = arcRadius - trackWidth * 1.6f;
    const auto body = Rectangle<float> (bodyR * 2.0f, bodyR * 2.0f).withCentre (centre);
    g.setGradientFill (ColourGradient (Colour (0xff35353c), centre.x, body.getY(), Colour (0xff1a1a1e), centre.x,
                                       body.getBottom(), false));
    g.fillEllipse (body);
    g.setColour (Colours::black.withAlpha (0.6f));
    g.drawEllipse (body, 1.0f);

    // Pointer
    Path pointer;
    const float pw = jmax (2.0f, bodyR * 0.11f);
    pointer.addRoundedRectangle (-pw * 0.5f, -bodyR + bodyR * 0.15f, pw, bodyR * 0.45f, pw * 0.5f);
    g.setColour (enabled ? colours::text : colours::textDim);
    g.fillPath (pointer, AffineTransform::rotation (angle).translated (centre));
}

void Theme::drawButtonBackground (Graphics& g, Button& b, const Colour&, bool over, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    auto base = b.getToggleState() ? colours::accent.withAlpha (0.25f) : Colour (0xff26262b);
    if (down)
        base = base.brighter (0.15f);
    else if (over)
        base = base.brighter (0.07f);
    g.setColour (base);
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (over ? colours::accent.withAlpha (0.7f) : colours::panelEdge);
    g.drawRoundedRectangle (r, 5.0f, 1.0f);
}

void Theme::drawToggleButton (Graphics& g, ToggleButton& b, bool over, bool)
{
    const auto r = b.getLocalBounds().toFloat();
    const float h = jmin (18.0f, r.getHeight() - 4.0f);
    const auto sw = Rectangle<float> (h * 1.8f, h).withY (r.getCentreY() - h * 0.5f).withX (r.getX() + 2.0f);
    const bool on = b.getToggleState();

    g.setColour (on ? colours::accent : colours::well);
    g.fillRoundedRectangle (sw, h * 0.5f);
    g.setColour (over ? colours::accent.withAlpha (0.8f) : colours::panelEdge);
    g.drawRoundedRectangle (sw, h * 0.5f, 1.0f);

    const auto thumb = Rectangle<float> (h - 4.0f, h - 4.0f)
                           .withCentre ({ on ? sw.getRight() - h * 0.5f : sw.getX() + h * 0.5f, sw.getCentreY() });
    g.setColour (on ? colours::background : colours::textDim);
    g.fillEllipse (thumb);

    g.setColour (b.isEnabled() ? colours::text : colours::textDim);
    g.setFont (FontOptions (14.0f));
    g.drawFittedText (b.getButtonText(), r.withLeft (sw.getRight() + 8.0f).toNearestInt(), Justification::centredLeft, 1);
}

void Theme::drawComboBox (Graphics& g, int w, int h, bool, int, int, int, int, ComboBox& box)
{
    const auto r = Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
    g.setColour (colours::well);
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (box.hasKeyboardFocus (false) ? colours::accent : colours::panelEdge);
    g.drawRoundedRectangle (r, 5.0f, 1.0f);

    Path arrow;
    const float ax = (float) w - 16.0f, ay = (float) h * 0.5f;
    arrow.startNewSubPath (ax - 4.0f, ay - 2.0f);
    arrow.lineTo (ax, ay + 2.5f);
    arrow.lineTo (ax + 4.0f, ay - 2.0f);
    g.setColour (colours::textDim);
    g.strokePath (arrow, PathStrokeType (1.5f));
}

Font Theme::getComboBoxFont (ComboBox&) { return FontOptions (14.0f); }
Font Theme::getTextButtonFont (TextButton&, int) { return FontOptions (14.0f, Font::bold); }

Label* Theme::createSliderTextBox (Slider& s)
{
    auto* l = LookAndFeel_V4::createSliderTextBox (s);
    l->setFont (FontOptions (13.0f));
    l->setColour (Label::textColourId, colours::text);
    l->setColour (Label::outlineWhenEditingColourId, colours::accent);
    l->setColour (Label::backgroundWhenEditingColourId, colours::well);
    return l;
}

void drawPanel (Graphics& g, Rectangle<float> area, const String& caption)
{
    g.setColour (colours::panel);
    g.fillRoundedRectangle (area, 8.0f);
    g.setColour (colours::panelEdge);
    g.drawRoundedRectangle (area.reduced (0.5f), 8.0f, 1.0f);

    if (caption.isNotEmpty())
    {
        g.setColour (colours::textDim);
        g.setFont (FontOptions (11.5f, Font::bold));
        g.drawText (caption.toUpperCase(), area.reduced (14.0f, 10.0f).removeFromTop (14.0f).toNearestInt(),
                    Justification::centredLeft);
    }
}

} // namespace ui
