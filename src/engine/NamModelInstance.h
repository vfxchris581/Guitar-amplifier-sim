#pragma once

#include "engine/ModelInfo.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace nam { class DSP; }
namespace dsp { template <typename T, int NCHANS, size_t A> class ResamplingContainer; }

namespace gas
{

/** One loaded NAM model, ready to run at the host's sample rate.

    Owns the genuine nam::DSP object plus, when the host rate differs from the
    rate the model was trained at, the same Lanczos resampling container the
    official NAM plugin uses.

    Threading:
      - load() / prepare() allocate and may take a while: call off the audio thread.
      - process() is real-time safe: no allocation, no locks, no I/O.
*/
class NamModelInstance
{
public:
    ~NamModelInstance();

    /** Loads and validates a .nam file, then prepares it. Returns nullptr and
        fills `error` with a user-readable explanation on failure. */
    static std::unique_ptr<NamModelInstance> load (const std::filesystem::path& file,
                                                   double hostSampleRate,
                                                   int maxBlockSize,
                                                   std::string& error);

    /** Re-targets the model to a new host rate / block size. Not real-time safe. */
    void prepare (double hostSampleRate, int maxBlockSize);

    /** Clears the model's internal state and re-warms it. Not real-time safe. */
    void reset();

    /** Processes `numSamples` mono samples. numSamples <= the prepared block size. */
    void process (const float* input, float* output, int numSamples) noexcept;

    const ModelInfo& info() const noexcept { return modelInfo; }

    double preparedSampleRate() const noexcept { return hostRate; }
    int preparedBlockSize() const noexcept { return maxBlock; }

    /** Extra latency (host-rate samples) introduced by resampling, 0 when none. */
    int latencySamples() const noexcept;

    bool isResampling() const noexcept { return resampler != nullptr; }

    // Calibration data straight from the model (used by AmpEngine's gain staging)
    bool hasLoudness() const noexcept      { return modelInfo.loudnessDb.has_value(); }
    bool hasInputLevel() const noexcept    { return modelInfo.inputLevelDbu.has_value(); }
    bool hasOutputLevel() const noexcept   { return modelInfo.outputLevelDbu.has_value(); }

private:
    NamModelInstance() = default;

    void processAtModelRate (float** in, float** out, int n) noexcept;

    using Resampler = dsp::ResamplingContainer<float, 1, 12>;

    std::unique_ptr<nam::DSP> model;
    std::unique_ptr<Resampler> resampler;
    ModelInfo modelInfo;

    double hostRate = 0.0;
    int maxBlock = 0;
    int maxModelBlock = 0;

    std::vector<float> inScratch, outScratch;
};

} // namespace gas
