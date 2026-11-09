#pragma once

#include "train/WaveNet.h"

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace gas::train
{

/** Training settings. Defaults are NAM's classic WaveNet trainer settings. */
struct TrainOptions
{
    Preset preset = Preset::Standard;
    int epochs = 100;
    float learningRate = 0.004f;
    float learningRateDecay = 0.007f; // ExponentialLR, gamma = 1 - decay, stepped per epoch
    int batchSize = 16;
    int outputSamplesPerItem = 8192;  // NAM's "ny"
    std::uint32_t seed = 0;
    int threads = 0;                  // 0 = all hardware threads
    std::optional<double> stopAtValidationEsr; // stop early once reached
};

struct TrainProgress
{
    int epoch = 0, epochs = 0;
    int step = 0, stepsPerEpoch = 0;
    double trainLoss = 0.0;                  // running mean MSE of this epoch
    std::optional<double> validationEsr;     // of the most recent epoch
    std::optional<double> bestValidationEsr;
    double secondsElapsed = 0.0;
    double secondsRemaining = 0.0;
    std::vector<double> epochTrainLoss;      // history
    std::vector<double> epochValidationEsr;  // history
};

struct TrainResult
{
    bool success = false;
    bool cancelled = false;
    std::string error;
    WaveNetSpec spec;
    std::vector<float> bestParams;
    double bestValidationEsr = 0.0;
    int epochsRun = 0;
};

/** Error-to-signal ratio, as NAM defines it: mean((p - t)^2) / mean(t^2). */
double esr (const float* prediction, const float* target, size_t n);

/** Runs a model over a long signal (in chunks, any length) and returns
    signal.size() - receptiveField + 1 output samples. */
std::vector<float> runModel (const WaveNet& net, const std::vector<float>& signal);

/** Trains a NAM WaveNet on (input, output) recordings that are already latency-aligned.

    Matches NAM's classic trainer: MSE loss on windows of receptiveField + ny - 1
    input samples, shuffled each epoch, Adam (beta 0.9/0.999, eps 1e-8),
    exponential learning-rate decay per epoch, best-validation-ESR checkpoint.
    The batch is split across threads. Safe to call from a background thread;
    `cancel` is checked between steps. */
TrainResult trainWaveNet (const std::vector<float>& trainInput, const std::vector<float>& trainOutput,
                          const std::vector<float>& validationInput, const std::vector<float>& validationOutput,
                          const TrainOptions& options,
                          const std::function<void (const TrainProgress&)>& onProgress,
                          const std::atomic<bool>& cancel);

/** NAM's metadata loudness (trainer >= 0.12 definition): the model's response to
    NAM's loudness stimulus, K-weighted, averaged per burst in dB, + 3 dB.
    `stimulus` is nam/models/_resources/loudness_input.wav (48 kHz). */
double measureLoudnessDb (const WaveNet& net, const std::vector<float>& stimulus);

} // namespace gas::train
