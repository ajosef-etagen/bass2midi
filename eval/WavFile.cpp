#include "WavFile.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>

namespace bass2midi::eval
{
    namespace
    {
        std::uint32_t u32 (const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t> (p[3]) << 24); }
        std::uint16_t u16 (const unsigned char* p) { return static_cast<std::uint16_t> (p[0] | (p[1] << 8)); }
    }

    bool readWav (const std::string& path, WavData& out, std::string& error)
    {
        std::ifstream file (path, std::ios::binary);
        if (! file)
        {
            error = "cannot open " + path;
            return false;
        }

        const std::vector<unsigned char> bytes ((std::istreambuf_iterator<char> (file)), std::istreambuf_iterator<char>());
        if (bytes.size() < 12 || std::memcmp (bytes.data(), "RIFF", 4) != 0 || std::memcmp (bytes.data() + 8, "WAVE", 4) != 0)
        {
            error = "not a RIFF/WAVE file";
            return false;
        }

        int formatTag = 0, channels = 0, bits = 0, blockAlign = 0;
        std::uint32_t rate = 0;
        const unsigned char* data = nullptr;
        std::size_t dataSize = 0;

        for (std::size_t pos = 12; pos + 8 <= bytes.size();)
        {
            const auto* chunk = bytes.data() + pos;
            const std::size_t size = u32 (chunk + 4);
            const std::size_t available = std::min (size, bytes.size() - pos - 8);

            if (std::memcmp (chunk, "fmt ", 4) == 0 && available >= 16)
            {
                formatTag = u16 (chunk + 8);
                channels = u16 (chunk + 10);
                rate = u32 (chunk + 12);
                blockAlign = u16 (chunk + 20);
                bits = u16 (chunk + 22);
                if (formatTag == 0xFFFE && available >= 26)
                    formatTag = u16 (chunk + 8 + 24); // sub-format GUID starts with the format tag
            }
            else if (std::memcmp (chunk, "data", 4) == 0)
            {
                data = chunk + 8;
                dataSize = available;
            }

            pos += 8 + size + (size & 1);
        }

        if (data == nullptr || channels <= 0 || rate == 0 || blockAlign <= 0)
        {
            error = "missing fmt or data chunk";
            return false;
        }

        const bool isFloat = formatTag == 3;
        if (! ((formatTag == 1 && (bits == 16 || bits == 24 || bits == 32)) || (isFloat && (bits == 32 || bits == 64))))
        {
            error = "unsupported format tag " + std::to_string (formatTag) + " / " + std::to_string (bits) + " bit";
            return false;
        }

        const std::size_t frames = dataSize / static_cast<std::size_t> (blockAlign);
        const int bytesPerSample = bits / 8;
        out.sampleRateHz = rate;
        out.channels = channels;
        out.bitsPerSample = bits;
        out.channelData.assign (static_cast<std::size_t> (channels), std::vector<float> (frames));

        for (std::size_t f = 0; f < frames; ++f)
        {
            for (int c = 0; c < channels; ++c)
            {
                const auto* s = data + f * static_cast<std::size_t> (blockAlign) + static_cast<std::size_t> (c * bytesPerSample);
                double v = 0.0;
                if (isFloat && bits == 32)
                {
                    float x;
                    std::memcpy (&x, s, 4);
                    v = x;
                }
                else if (isFloat)
                {
                    double x;
                    std::memcpy (&x, s, 8);
                    v = x;
                }
                else if (bits == 16)
                    v = static_cast<std::int16_t> (u16 (s)) / 32768.0;
                else if (bits == 24)
                {
                    std::int32_t x = s[0] | (s[1] << 8) | (s[2] << 16);
                    if (x & 0x800000)
                        x -= 0x1000000;
                    v = x / 8388608.0;
                }
                else
                    v = static_cast<std::int32_t> (u32 (s)) / 2147483648.0;

                out.channelData[static_cast<std::size_t> (c)][f] = static_cast<float> (v);
            }
        }
        return true;
    }
}
