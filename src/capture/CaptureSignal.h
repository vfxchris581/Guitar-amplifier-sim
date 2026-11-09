#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace gas::capture
{

/** Layout of NAM's standard v3 capture signal ("input.wav" / "v3_0_0.wav"), 48 kHz.
    Values from neural-amp-modeler's nam/train/core.py (_V3_DATA_INFO):
      0:00-0:09  validation 1        0:09-0:10  silence
      0:10-0:12  blips at 10.5 s and 11.5 s
      0:12-0:15  chirps              0:15-0:17  noise
      0:17-3:00.5 training data      3:00.5-3:01 silence
      3:01-3:10  validation 2 (repeat of validation 1) */
struct V3Layout
{
    static constexpr double sampleRate = 48000.0;
    static constexpr int validationLength = 432000;     // t_validate
    static constexpr int trainStart = 480000;           // train_start
    static constexpr int firstBlipsStart = 480000;
    static constexpr int blipsLength = 96000;           // t_blips
    static constexpr int blip1 = 504000, blip2 = 552000;
    static constexpr int noiseFrom = 492000, noiseTo = 498000; // background-level interval
    static constexpr const char* md5 = "36cd1af62985c2fac3e654333e36431e"; // NAM trainer's strong hash
    static constexpr const char* officialDownload =
        "https://drive.google.com/file/d/1Pgf8PdE0rKB1TD4TRPKbpNo1ByR3IOm9/view";
};

/** NAM's capture signal, loaded from the user's copy of the official file. */
struct CaptureSignal
{
    std::vector<float> samples;
    std::string sourcePath;

    bool empty() const { return samples.empty(); }
    double durationSeconds() const { return (double) samples.size() / V3Layout::sampleRate; }

    /** Loads and verifies the file is NAM's standard v3 signal (by the same MD5
        NAM's trainer uses). On failure returns an empty signal and explains why. */
    static CaptureSignal loadNamV3 (const std::filesystem::path& file, std::string& error);

    /** For tests only: accepts any mono 48 kHz file with the v3 layout length requirements. */
    static CaptureSignal loadUnverified (const std::filesystem::path& file, std::string& error);
};

} // namespace gas::capture
