#pragma once

#include <string>
#include <vector>

namespace bass2midi::eval
{
    // Minimal RIFF/WAVE reader for offline evaluation: PCM 16/24/32-bit integer and 32/64-bit float,
    // WAVE_FORMAT_EXTENSIBLE included, any channel count. Unknown chunks (Logic's LGWV, bext, cue,
    // ...) are skipped. Not for the audio thread.
    struct WavData
    {
        double sampleRateHz = 0.0;
        int channels = 0;
        int bitsPerSample = 0;
        std::vector<std::vector<float>> channelData; // [channel][sample], full scale = 1.0
    };

    // Returns false and sets `error` on unreadable or unsupported files.
    bool readWav (const std::string& path, WavData& out, std::string& error);
}
