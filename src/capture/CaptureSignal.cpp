#include "capture/CaptureSignal.h"
#include "capture/Md5.h"
#include "common/WavIO.h"

namespace gas::capture
{

CaptureSignal CaptureSignal::loadUnverified (const std::filesystem::path& file, std::string& error)
{
    CaptureSignal s;
    double rate = 0.0;
    if (! wav::readWavMono (file.string(), s.samples, rate))
    {
        error = "Could not read this WAV file.";
        return {};
    }
    if (rate != V3Layout::sampleRate)
    {
        error = "The capture signal must be 48 kHz (this file is " + std::to_string ((int) rate) + " Hz).";
        return {};
    }
    if (s.samples.size() < (size_t) (V3Layout::trainStart + 2 * V3Layout::validationLength))
    {
        error = "This file is too short to be NAM's capture signal.";
        return {};
    }
    s.sourcePath = file.string();
    return s;
}

CaptureSignal CaptureSignal::loadNamV3 (const std::filesystem::path& file, std::string& error)
{
    const auto hash = Md5::ofFile (file);
    if (! hash)
    {
        error = "Could not open the file.";
        return {};
    }
    if (*hash != V3Layout::md5)
    {
        error = "This is not NAM's standard v3 capture signal (v3_0_0.wav / input.wav). "
                "Please use the unmodified file from the Neural Amp Modeler project.";
        return {};
    }
    return loadUnverified (file, error);
}

} // namespace gas::capture
