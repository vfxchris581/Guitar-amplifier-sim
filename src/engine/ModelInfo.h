#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace gas
{

/** UTF-8 std::string from a path on every platform (C++20 u8string is char8_t). */
inline std::string pathToUtf8 (const std::filesystem::path& p)
{
    const auto s = p.u8string();
    return std::string (s.begin(), s.end());
}

/** Path from a UTF-8 std::string on every platform. */
inline std::filesystem::path utf8ToPath (const std::string& s)
{
    return std::filesystem::path (std::u8string (s.begin(), s.end()));
}

/** Descriptive information about a loaded .nam model.

    Every field is taken from the file itself. Anything the file does not
    contain is left empty / std::nullopt — nothing is guessed or invented.
*/
struct ModelInfo
{
    std::string filePath;
    std::string fileName;

    // From the file's "metadata" block (written by the NAM trainer / capturer)
    std::string name;
    std::string modeledBy;   // author
    std::string gearMake;    // manufacturer
    std::string gearModel;   // amplifier model
    std::string gearType;    // "amp", "pedal", "amp_cab", "pedal_amp", "amp_pedal_cab", ...
    std::string toneType;    // "clean", "overdrive", "crunch", "hi_gain", "fuzz"
    std::string date;        // YYYY-MM-DD if present
    std::optional<double> loudnessDb;
    std::optional<double> inputLevelDbu;
    std::optional<double> outputLevelDbu;
    std::optional<double> validationEsr; // training validation error-to-signal ratio

    // From the model definition itself
    std::string architecture;  // "WaveNet", "LSTM", "ConvNet", "Linear", ...
    std::string namFileVersion;
    double sampleRate = 48000.0;
    bool sampleRateFromFile = false; // false => file had no sample rate; NAM convention of 48 kHz assumed
    size_t numWeights = 0;

    /** True if the capture includes a speaker cabinet (drives double-cab protection later). */
    bool includesCabinet() const;

    /** A readable title: metadata name, else make+model, else the file name. */
    std::string displayName() const;
};

} // namespace gas
