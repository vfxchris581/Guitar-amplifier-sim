#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace gas::capture
{
/** RFC 1321 MD5, used only to recognise NAM's standard capture files the same
    way NAM's trainer does (it identifies them by the MD5 of the file bytes). */
class Md5
{
public:
    Md5();
    void update (const void* data, size_t size);
    std::string hexDigest(); // finalises

    static std::string ofString (const std::string&);
    static std::optional<std::string> ofFile (const std::filesystem::path&);

private:
    void transform (const unsigned char block[64]);
    std::uint32_t state[4];
    std::uint64_t bitCount = 0;
    unsigned char buffer[64];
    size_t bufferLen = 0;
};
} // namespace gas::capture
