#pragma once

#include <string>
#include <vector>

namespace bass2midi::song::detail
{
    // Extracts one entry from an in-memory ZIP archive (stored or deflated, no ZIP64, no encryption).
    bool extractZipEntry (const std::vector<unsigned char>& archive, const std::string& entryName,
                          std::vector<unsigned char>& out, std::string& error);

    bool looksLikeZip (const std::vector<unsigned char>& bytes);
}
