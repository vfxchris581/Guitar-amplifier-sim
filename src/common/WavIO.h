#pragma once

#include <string>
#include <vector>

namespace gas::wav
{
/** Reads a WAV file (PCM 16/24/32-bit or float32) and returns channel 0. */
bool readWavMono (const std::string& path, std::vector<float>& samples, double& sampleRate);

/** Same as readWavMono, from a WAV file held in memory. */
bool readWavMonoFromMemory (const void* data, size_t size, std::vector<float>& samples, double& sampleRate);

/** Writes a mono 32-bit float WAV file. */
bool writeWavMono (const std::string& path, const std::vector<float>& samples, double sampleRate);
/** Writes a mono 24-bit PCM WAV file (the format NAM's trainer expects). Clips to [-1, 1]. */
bool writeWavMono24 (const std::string& path, const std::vector<float>& samples, double sampleRate);
} // namespace gas::wav
