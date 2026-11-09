#include "WavIO.h"

#include <cstdint>
#include <cstring>
#include <fstream>

namespace gas::test
{
namespace
{
template <typename T>
T readLE (const unsigned char* p)
{
    T v;
    std::memcpy (&v, p, sizeof (T));
    return v;
}
} // namespace

bool readWavMono (const std::string& path, std::vector<float>& samples, double& sampleRate)
{
    std::ifstream f (path, std::ios::binary);
    if (! f)
        return false;

    std::vector<unsigned char> data ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
    if (data.size() < 12 || std::memcmp (data.data(), "RIFF", 4) != 0 || std::memcmp (data.data() + 8, "WAVE", 4) != 0)
        return false;

    int format = 0, channels = 0, bits = 0;
    size_t pos = 12;
    while (pos + 8 <= data.size())
    {
        const auto size = readLE<uint32_t> (data.data() + pos + 4);
        const unsigned char* body = data.data() + pos + 8;

        if (std::memcmp (data.data() + pos, "fmt ", 4) == 0)
        {
            format = readLE<uint16_t> (body);
            channels = readLE<uint16_t> (body + 2);
            sampleRate = readLE<uint32_t> (body + 4);
            bits = readLE<uint16_t> (body + 14);
            if (format == 0xFFFE && size >= 40)
                format = readLE<uint16_t> (body + 24);
        }
        else if (std::memcmp (data.data() + pos, "data", 4) == 0 && channels > 0)
        {
            const int bytes = bits / 8;
            const size_t frames = size / (size_t) (bytes * channels);
            samples.resize (frames);
            for (size_t i = 0; i < frames; ++i)
            {
                const unsigned char* s = body + i * (size_t) (bytes * channels);
                if (format == 3 && bits == 32)
                    samples[i] = readLE<float> (s);
                else if (bits == 16)
                    samples[i] = (float) readLE<int16_t> (s) / 32768.0f;
                else if (bits == 24)
                {
                    int32_t v = (int32_t) ((uint32_t) s[0] << 8 | (uint32_t) s[1] << 16 | (uint32_t) s[2] << 24) >> 8;
                    samples[i] = (float) v / 8388608.0f;
                }
                else if (bits == 32)
                    samples[i] = (float) ((double) readLE<int32_t> (s) / 2147483648.0);
                else
                    return false;
            }
            return true;
        }
        pos += 8 + size + (size & 1);
    }
    return false;
}

bool writeWavMono (const std::string& path, const std::vector<float>& samples, double sampleRate)
{
    std::ofstream f (path, std::ios::binary);
    if (! f)
        return false;

    auto u32 = [&] (uint32_t v) { f.write ((const char*) &v, 4); };
    auto u16 = [&] (uint16_t v) { f.write ((const char*) &v, 2); };

    const uint32_t dataBytes = (uint32_t) (samples.size() * 4);
    f.write ("RIFF", 4); u32 (36 + dataBytes); f.write ("WAVE", 4);
    f.write ("fmt ", 4); u32 (16); u16 (3); u16 (1); u32 ((uint32_t) sampleRate);
    u32 ((uint32_t) sampleRate * 4); u16 (4); u16 (32);
    f.write ("data", 4); u32 (dataBytes);
    f.write ((const char*) samples.data(), dataBytes);
    return (bool) f;
}
} // namespace gas::test
