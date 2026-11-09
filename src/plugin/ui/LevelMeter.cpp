#include "plugin/ui/LevelMeter.h"
#include "plugin/ui/Theme.h"

#include <cmath>

namespace ui
{
using namespace juce;

namespace
{
constexpr float kMinDb = -60.0f;
constexpr float kMaxDb = 6.0f;
constexpr float kFallDbPerFrame = 1.2f; // at 30 fps ≈ 36 dB/s
constexpr int kHoldFrames = 45;
} // namespace

LevelMeter::LevelMeter (String c) : caption (std::move (c))
{
    setTooltip ("Peak level in dBFS. The red light latches when the signal reaches 0 dBFS - click to reset.");
}

void LevelMeter::setPeak (float linearPeak)
{
    const float db = linearPeak > 0.0f ? 20.0f * std::log10 (linearPeak) : -100.0f;
    levelDb = jmax (db, levelDb - kFallDbPerFrame);

    if (db >= holdDb)
    {
        holdDb = db;
        holdFrames = kHoldFrames;
    }
    else if (--holdFrames <= 0)
    {
        holdDb = jmax (levelDb, holdDb - kFallDbPerFrame);
    }

    if (db >= 0.0f)
        clipped = true;

    repaint();
}

float LevelMeter::dbToY (float db, Rectangle<float> bar)
{
    const float p = jlimit (0.0f, 1.0f, (db - kMinDb) / (kMaxDb - kMinDb));
    return bar.getBottom() - p * bar.getHeight();
}

void LevelMeter::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat();

    auto captionArea = r.removeFromBottom (16.0f);
    auto ledRow = r.removeFromTop (12.0f);
    r.removeFromTop (4.0f);

    auto bar = r.withWidth (jmin (12.0f, r.getWidth() * 0.4f)).withX (r.getX() + 4.0f);
    auto scale = r.withLeft (bar.getRight() + 3.0f);

    const auto led = Rectangle<float> (10.0f, 8.0f).withCentre ({ bar.getCentreX(), ledRow.getCentreY() });
    g.setColour (clipped ? colours::error : colours::well);
    g.fillRoundedRectangle (led, 2.0f);

    g.setColour (colours::well);
    g.fillRoundedRectangle (bar, 3.0f);

    if (levelDb > kMinDb)
    {
        const float top = dbToY (levelDb, bar);
        const auto fill = bar.withTop (top);
        ColourGradient grad (colours::error, bar.getX(), dbToY (0.0f, bar), colours::ok, bar.getX(),
                             dbToY (-24.0f, bar), false);
        grad.addColour (0.3, colours::warning);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (fill, 3.0f);
    }

    g.setColour (colours::textDim);
    g.setFont (FontOptions (11.0f, Font::bold));
    g.drawText (caption, captionArea, Justification::centredLeft, false);

    if (holdDb > kMinDb)
    {
        g.setColour (holdDb >= 0.0f ? colours::error : colours::text);
        g.fillRect (bar.withY (dbToY (holdDb, bar) - 1.0f).withHeight (2.0f));
    }

    g.setFont (FontOptions (9.5f));
    for (float db : { 0.0f, -6.0f, -12.0f, -24.0f, -40.0f })
    {
        const float y = dbToY (db, bar);
        g.setColour (colours::textDim.withAlpha (0.8f));
        g.drawText (String ((int) db), scale.withY (y - 6.0f).withHeight (12.0f), Justification::centredLeft, false);
    }
}

void LevelMeter::mouseDown (const MouseEvent&)
{
    clipped = false;
    holdDb = -100.0f;
    repaint();
}
} // namespace ui
