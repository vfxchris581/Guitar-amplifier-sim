#pragma once

#include <string>
#include <vector>

namespace gas::wav
{
/** Reads a WAV file (PCM 16/24/32-bit or float32) and returns channel 0. */
bool readWavMono (const std::string& path, std::vector<float>& samples, double& sampleRate);

/** Writes a mono 32-bit float WAV file. */
bool writeWavMono (const std::string& path, const std::vector<float>& samples, double sampleRate);
} // namespace gas::wav
