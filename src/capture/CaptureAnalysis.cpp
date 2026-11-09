#include "capture/CaptureAnalysis.h"
#include "capture/CaptureSignal.h"

#include <algorithm>
#include <cmath>

namespace gas::capture
{
namespace
{
constexpr float kAbsThreshold = 0.0003f;   // _DELAY_CALIBRATION_ABS_THRESHOLD
constexpr float kRelThreshold = 0.001f;    // _DELAY_CALIBRATION_REL_THRESHOLD
constexpr int kSafetyFactor = 1;           // _DELAY_CALIBRATION_SAFETY_FACTOR
constexpr int kLookahead = 1000;
constexpr int kLookback = 10000;

float toDb (double linear) { return linear > 0.0 ? (float) (20.0 * std::log10 (linear)) : -200.0f; }
} // namespace

LatencyCalibration calibrateLatencyV3 (const std::vector<float>& y)
{
    LatencyCalibration r;
    using L = V3Layout;
    if (y.size() < (size_t) (L::firstBlipsStart + L::blipsLength))
    {
        r.notDetected = true;
        r.message = "The recording is too short to contain the calibration blips.";
        return r;
    }

    float background = 0.0f;
    for (int i = L::noiseFrom; i < L::noiseTo; ++i)
        background = std::max (background, std::abs (y[(size_t) i]));
    const float threshold = std::max (background + kAbsThreshold, (1.0f + kRelThreshold) * background);

    // Average the responses around both blips (window: -lookahead .. +lookback)
    std::vector<float> avg ((size_t) (kLookahead + kLookback), 0.0f);
    for (int blip : { L::blip1, L::blip2 })
        for (int i = 0; i < kLookahead + kLookback; ++i)
            avg[(size_t) i] += 0.5f * y[(size_t) (blip - kLookahead + i)];

    for (int i = 0; i < (int) avg.size(); ++i)
    {
        if (std::abs (avg[(size_t) i]) > threshold)
        {
            const int delay = i - kLookahead;
            r.detected = delay;
            r.recommended = delay - kSafetyFactor;
            r.matchesLookahead = (delay == -kLookahead);
            r.message = "Round-trip latency: " + std::to_string (delay) + " samples (" + std::to_string (delay / 48.0).substr (0, 4) + " ms)";
            if (r.matchesLookahead)
                r.message += ". This equals the search limit and is probably wrong - check the routing.";
            else if (delay < 0)
                r.message += ". A negative latency means the return arrived before the signal was sent - "
                             "the input is probably hearing something other than the amp.";
            return r;
        }
    }

    r.notDetected = true;
    r.message = "No response to the calibration blips came back. Check that the plugin's output reaches the "
                "reamp box / amp and that the amp's line output returns to the plugin's input.";
    return r;
}

LevelReport analyseLevels (const std::vector<float>& y)
{
    LevelReport r;
    double peak = 0.0;
    for (float v : y)
    {
        peak = std::max (peak, (double) std::abs (v));
        if (std::abs (v) >= 0.999f)
            ++r.clippedSamples;
    }
    r.peakDbfs = toDb (peak);

    if (y.size() >= (size_t) V3Layout::noiseTo)
    {
        // RMS about the mean: hum and hiss count, a constant DC offset does not.
        const int count = V3Layout::noiseTo - V3Layout::noiseFrom;
        double mean = 0.0, ms = 0.0;
        for (int i = V3Layout::noiseFrom; i < V3Layout::noiseTo; ++i)
            mean += y[(size_t) i];
        mean /= count;
        for (int i = V3Layout::noiseFrom; i < V3Layout::noiseTo; ++i)
            ms += (y[(size_t) i] - mean) * (y[(size_t) i] - mean);
        r.noiseFloorDbfs = toDb (std::sqrt (ms / count));
    }

    r.silent = peak < 1e-4;
    if (r.silent)
        r.warnings.push_back ("Nothing came back from the amp. Check the routing and that the amp is on.");
    if (r.clippedSamples > 0)
        r.warnings.push_back ("The return clipped (" + std::to_string (r.clippedSamples)
                              + " samples). Lower the load box / interface input gain and capture again.");
    else if (! r.silent && r.peakDbfs < -30.0f)
        r.warnings.push_back ("The return is very quiet (peak " + std::to_string ((int) std::lround (r.peakDbfs))
                              + " dBFS). Raise the interface input gain for a cleaner capture.");
    if (! r.silent && r.noiseFloorDbfs > -60.0f)
        r.warnings.push_back ("High background noise (" + std::to_string ((int) std::lround (r.noiseFloorDbfs))
                              + " dBFS in silence). Check for hum, ground loops or a noisy amp channel.");
    return r;
}

ReplicateCheck checkValidationReplicate (const std::vector<float>& y, size_t n)
{
    ReplicateCheck c;
    const size_t t = (size_t) V3Layout::validationLength;
    if (y.size() < n || n < 2 * t)
    {
        c.esr = INFINITY;
        return c;
    }
    double num = 0.0, den = 0.0;
    for (size_t i = 0; i < t; ++i)
    {
        const double a = y[i], b = y[n - t + i];
        num += (b - a) * (b - a);
        den += b * b;
    }
    c.esr = den > 0.0 ? num / den : INFINITY;
    c.passed = c.esr <= ReplicateCheck::threshold;
    return c;
}

PreparedData prepareV3 (const std::vector<float>& x, const std::vector<float>& yIn, int latency)
{
    PreparedData d;
    const size_t n = x.size();
    std::vector<float> y (yIn.begin(), yIn.begin() + (long) std::min (n, yIn.size()));
    y.resize (n, 0.0f);

    auto slice = [&] (size_t from, size_t to, std::vector<float>& xs, std::vector<float>& ys)
    {
        xs.assign (x.begin() + (long) from, x.begin() + (long) to);
        ys.assign (y.begin() + (long) from, y.begin() + (long) to);
        if (latency > 0)
        {
            xs.resize (xs.size() - (size_t) latency);
            ys.erase (ys.begin(), ys.begin() + latency);
        }
        else if (latency < 0)
        {
            xs.erase (xs.begin(), xs.begin() + (-latency));
            ys.resize (ys.size() - (size_t) (-latency));
        }
    };

    const size_t valStart = n - (size_t) V3Layout::validationLength;
    slice ((size_t) V3Layout::trainStart, valStart, d.trainInput, d.trainOutput);
    slice (valStart, n, d.validationInput, d.validationOutput);
    return d;
}

} // namespace gas::capture
