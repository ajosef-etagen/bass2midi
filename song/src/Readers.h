#pragma once

#include "bass2midi/song/Song.h"

#include <string>
#include <vector>

namespace bass2midi::song::detail
{
    // Guitar Pro 7/8: ZIP archive with Content/score.gpif.
    bool readGpif (const std::vector<unsigned char>& archive, Song& song, std::string& error);

    // Guitar Pro 5.00 / 5.10 binary.
    bool looksLikeGp5 (const std::vector<unsigned char>& bytes);
    bool readGp5 (const std::vector<unsigned char>& bytes, Song& song, std::string& error);
}
