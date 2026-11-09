#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <mutex>

/** Second-order Butterworth high/low-pass (RBJ cookbook). Real-time safe. */
struct Biquad
{
    enum class Type { HighPass, LowPass };
    void set (Type type, double freq, double sampleRate) noexcept;
    void bypass() noexcept { b0 = 1; b1 = b2 = a1 = a2 = 0; }
    void reset() noexcept { z1 = z2 = 0; }
    float process (float x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return (float) y;
    }
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
};

/** Cabinet stage: two independent "microphones", each an impulse response with
    level, pan, polarity, delay and high/low-pass filters, blended to stereo.

    IRs are loaded off the audio thread (JUCE's wait-free convolution loader,
    which also resamples to the host rate). process() never allocates. */
class CabEngine
{
public:
    static constexpr int numMics = 2;
    static constexpr float maxDelayMs = 10.0f;

    struct MicParams
    {
        std::atomic<bool> enabled { true };
        std::atomic<float> levelDb { 0.0f };
        std::atomic<float> pan { 0.0f };       // -1 .. +1
        std::atomic<bool> invert { false };
        std::atomic<float> delayMs { 0.0f };   // 0 .. maxDelayMs, fractional samples
        std::atomic<float> hpfHz { 20.0f };    // <= 20 Hz: off
        std::atomic<float> lpfHz { 20000.0f }; // >= 20 kHz: off
    };
    std::array<MicParams, numMics> mics;

    struct IRInfo
    {
        juce::String name, path;
        int channels = 0;
        double sampleRate = 0.0;
        double fileLengthMs = 0.0, usedLengthMs = 0.0;
        bool normalised = true;
    };

    CabEngine();

    void prepare (double sampleRate, int maxBlockSize);

    /** Mono in, stereo out. Returns false (writes nothing) when no IR is loaded on an enabled mic. */
    bool process (const float* in, float* outLeft, float* outRight, int numSamples) noexcept;

    // ---- message thread -----------------------------------------------------
    /** Reads a WAV/AIFF IR, trims it to maxLengthMs (with a short fade) and loads it. */
    bool loadIR (int mic, const juce::File& file, double maxLengthMs, bool normalise, juce::String& error);
    /** Re-applies length / normalisation to the already-loaded file. */
    bool reloadIR (int mic, double maxLengthMs, bool normalise);
    void clearIR (int mic);
    IRInfo getIRInfo (int mic) const;
    bool hasIR (int mic) const { return irLoaded[(size_t) mic].load(); }

private:
    struct Mic
    {
        juce::dsp::Convolution convolution;
        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd> delay { 4096 };
        std::array<Biquad, 2> hpf, lpf;
        float lastHpf = -1, lastLpf = -1;
        float gainL = 0, gainR = 0;
        bool gainsInitialised = false;
    };

    void updateFilters (Mic&, const MicParams&) noexcept;

    std::array<Mic, numMics> micDsp;
    std::array<std::atomic<bool>, numMics> irLoaded {};
    std::array<std::atomic<bool>, numMics> irStereo {};

    juce::AudioBuffer<float> scratch, micOut; // pre-allocated in prepare()
    double sampleRate = 48000.0;
    int maxBlock = 512;

    // message thread only
    mutable std::mutex infoMutex;
    std::array<IRInfo, numMics> infos;
    std::array<juce::AudioBuffer<float>, numMics> originals;
    std::array<double, numMics> originalRates {};
    juce::AudioFormatManager formats;
};
