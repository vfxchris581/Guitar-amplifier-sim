#pragma once

#include <atomic>
#include <vector>

namespace gas::capture
{

/** Plays a section of the capture signal out of the plugin and records what
    comes back, sample-synchronously, on the audio thread.

    Memory for the recording is allocated in arm() (message thread). process()
    only copies samples: no allocation, locks or I/O. The recording may be read
    from other threads once the state is Finished. */
class CaptureTransport
{
public:
    enum class State { Idle, Armed, Running, Finished, Cancelled };

    /** Prepare to play signal[from, to) and record (to - from) + tailSamples. Not real-time safe. */
    bool arm (const std::vector<float>& signal, size_t from, size_t to, size_t tailSamples);

    void start() noexcept;   // Armed -> Running (picked up at the next audio block)
    void cancel() noexcept;  // Running/Armed -> Cancelled
    void reset();            // back to Idle, frees memory (not while running)

    /** Audio thread. Writes the playback signal to `out` and records `in`.
        Returns false (and touches nothing) when not running. */
    bool process (const float* in, float* out, int numSamples) noexcept;

    State state() const noexcept { return currentState.load (std::memory_order_acquire); }
    float progress() const noexcept { return progressValue.load (std::memory_order_relaxed); }

    /** Valid once state() == Finished. */
    const std::vector<float>& recording() const { return recorded; }
    size_t playbackLength() const { return playback.size(); }

    std::atomic<float> playbackGainDb { 0.0f };
    std::atomic<float> returnPeak { 0.0f }, sendPeak { 0.0f }; // linear, reset by the reader

private:
    std::vector<float> playback, recorded;
    size_t position = 0; // audio thread
    std::atomic<State> currentState { State::Idle };
    std::atomic<float> progressValue { 0.0f };
};

} // namespace gas::capture
