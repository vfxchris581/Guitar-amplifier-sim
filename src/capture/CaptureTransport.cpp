#include "capture/CaptureTransport.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gas::capture
{

bool CaptureTransport::arm (const std::vector<float>& signal, size_t from, size_t to, size_t tail)
{
    if (state() == State::Running || from >= to || to > signal.size())
        return false;
    playback.assign (signal.begin() + (long) from, signal.begin() + (long) to);
    recorded.assign (playback.size() + tail, 0.0f);
    position = 0;
    progressValue.store (0.0f);
    currentState.store (State::Armed, std::memory_order_release);
    return true;
}

void CaptureTransport::start() noexcept
{
    auto expected = State::Armed;
    currentState.compare_exchange_strong (expected, State::Running, std::memory_order_acq_rel);
}

void CaptureTransport::cancel() noexcept
{
    auto s = state();
    if (s == State::Running || s == State::Armed)
        currentState.store (State::Cancelled, std::memory_order_release);
}

void CaptureTransport::reset()
{
    if (state() == State::Running)
        return;
    currentState.store (State::Idle, std::memory_order_release);
    std::vector<float>().swap (playback);
    std::vector<float>().swap (recorded);
    position = 0;
    progressValue.store (0.0f);
}

bool CaptureTransport::process (const float* in, float* out, int numSamples) noexcept
{
    if (currentState.load (std::memory_order_acquire) != State::Running)
        return false;

    const float gain = std::pow (10.0f, playbackGainDb.load (std::memory_order_relaxed) / 20.0f);
    float sp = 0.0f, rp = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const size_t p = position + (size_t) i;
        const float s = p < playback.size() ? playback[p] * gain : 0.0f;
        out[i] = s;
        sp = std::max (sp, std::abs (s));
        if (p < recorded.size())
        {
            recorded[p] = in[i];
            rp = std::max (rp, std::abs (in[i]));
        }
    }
    position += (size_t) numSamples;

    if (sp > sendPeak.load (std::memory_order_relaxed)) sendPeak.store (sp, std::memory_order_relaxed);
    if (rp > returnPeak.load (std::memory_order_relaxed)) returnPeak.store (rp, std::memory_order_relaxed);

    progressValue.store (std::min (1.0f, (float) position / (float) recorded.size()), std::memory_order_relaxed);
    if (position >= recorded.size())
        currentState.store (State::Finished, std::memory_order_release);
    return true;
}

} // namespace gas::capture
