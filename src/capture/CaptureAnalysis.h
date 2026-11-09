#pragma once

#include <optional>
#include <string>
#include <vector>

namespace gas::capture
{

/** Result of NAM's blip-based latency calibration (trainer algorithm v1). */
struct LatencyCalibration
{
    std::optional<int> detected;     // samples the output lags the input
    std::optional<int> recommended;  // detected - safety factor (what training uses)
    bool notDetected = false;
    bool matchesLookahead = false;   // suspicious: probably wrong
    std::string message;
};

/** Port of neural-amp-modeler's _calibrate_latency_v_all for the v3 signal:
    background level from the pre-blip silence, threshold = max(bg + 0.0003, 1.001 * bg),
    the responses to the blips are averaged and the first sample over the threshold
    gives the delay; 1 sample safety factor. `output` is the full recording. */
LatencyCalibration calibrateLatencyV3 (const std::vector<float>& output);

struct LevelReport
{
    float peakDbfs = -200.0f;
    int clippedSamples = 0;           // |y| >= 0.999
    float noiseFloorDbfs = -200.0f;   // RMS (DC removed) in NAM's background interval before the blips
    bool silent = false;              // nothing came back at all
    std::vector<std::string> warnings;
};

/** Level checks for a recording of the capture signal. */
LevelReport analyseLevels (const std::vector<float>& output);

/** NAM's v3 data check: the two validation passes (start and end of the signal)
    must match closely, ESR <= 0.01, or the gear/recording changed during capture. */
struct ReplicateCheck
{
    double esr = 0.0;
    bool passed = false;
    static constexpr double threshold = 0.01;
};
ReplicateCheck checkValidationReplicate (const std::vector<float>& output, size_t inputLength);

/** Training and validation data cut exactly as NAM's trainer cuts a v3 capture:
    train = [480000, N - 432000), validation = last 432000 samples, each slice then
    shifted by the latency (x drops its end, y drops its start). */
struct PreparedData
{
    std::vector<float> trainInput, trainOutput, validationInput, validationOutput;
};
PreparedData prepareV3 (const std::vector<float>& input, const std::vector<float>& output, int latency);

} // namespace gas::capture
