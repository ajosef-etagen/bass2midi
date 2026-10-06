#include "ZipReader.h"

#include <zlib.h>

#include <cstdint>
#include <cstring>

namespace bass2midi::song::detail
{
    namespace
    {
        std::uint32_t u32 (const std::vector<unsigned char>& b, std::size_t p)
        {
            return static_cast<std::uint32_t> (b[p]) | (static_cast<std::uint32_t> (b[p + 1]) << 8)
                 | (static_cast<std::uint32_t> (b[p + 2]) << 16) | (static_cast<std::uint32_t> (b[p + 3]) << 24);
        }

        std::uint16_t u16 (const std::vector<unsigned char>& b, std::size_t p)
        {
            return static_cast<std::uint16_t> (b[p] | (b[p + 1] << 8));
        }

        constexpr std::uint32_t endOfCentralDirectory = 0x06054b50;
        constexpr std::uint32_t centralHeader = 0x02014b50;
        constexpr std::uint32_t localHeader = 0x04034b50;
        constexpr std::size_t maxEntryBytes = 256u * 1024u * 1024u; // a score.gpif is a few MB at most
    }

    bool looksLikeZip (const std::vector<unsigned char>& bytes)
    {
        return bytes.size() >= 4 && u32 (bytes, 0) == localHeader;
    }

    bool extractZipEntry (const std::vector<unsigned char>& zip, const std::string& entryName,
                          std::vector<unsigned char>& out, std::string& error)
    {
        if (zip.size() < 22)
        {
            error = "file too small for a ZIP archive";
            return false;
        }

        // The end-of-central-directory record sits in the last 22 + up to 65535 (comment) bytes.
        std::size_t eocd = 0;
        bool found = false;
        const std::size_t lowest = zip.size() > 22 + 65535 ? zip.size() - 22 - 65535 : 0;
        for (std::size_t p = zip.size() - 22 + 1; p-- > lowest;)
            if (u32 (zip, p) == endOfCentralDirectory)
            {
                eocd = p;
                found = true;
                break;
            }
        if (! found)
        {
            error = "ZIP end of central directory not found";
            return false;
        }

        const std::size_t entries = u16 (zip, eocd + 10);
        std::size_t p = u32 (zip, eocd + 16);

        for (std::size_t i = 0; i < entries; ++i)
        {
            if (p + 46 > zip.size() || u32 (zip, p) != centralHeader)
            {
                error = "corrupt ZIP central directory";
                return false;
            }

            const int method = u16 (zip, p + 10);
            const std::size_t compressedSize = u32 (zip, p + 20);
            const std::size_t uncompressedSize = u32 (zip, p + 24);
            const std::size_t nameLength = u16 (zip, p + 28);
            const std::size_t extraLength = u16 (zip, p + 30);
            const std::size_t commentLength = u16 (zip, p + 32);
            const std::size_t localOffset = u32 (zip, p + 42);
            if (p + 46 + nameLength > zip.size())
            {
                error = "corrupt ZIP entry name";
                return false;
            }
            const std::string name (reinterpret_cast<const char*> (zip.data() + p + 46), nameLength);
            p += 46 + nameLength + extraLength + commentLength;

            if (name != entryName)
                continue;

            if (localOffset + 30 > zip.size() || u32 (zip, localOffset) != localHeader)
            {
                error = "corrupt ZIP local header";
                return false;
            }
            const std::size_t dataStart = localOffset + 30 + u16 (zip, localOffset + 26) + u16 (zip, localOffset + 28);
            if (dataStart + compressedSize > zip.size())
            {
                error = "truncated ZIP entry";
                return false;
            }

            if (method == 0)
            {
                out.assign (zip.begin() + static_cast<std::ptrdiff_t> (dataStart),
                            zip.begin() + static_cast<std::ptrdiff_t> (dataStart + compressedSize));
                return true;
            }
            if (method != 8)
            {
                error = "unsupported ZIP compression method " + std::to_string (method);
                return false;
            }

            if (uncompressedSize > maxEntryBytes)
            {
                error = "ZIP entry too large";
                return false;
            }
            out.assign (uncompressedSize, 0);
            z_stream stream {};
            if (inflateInit2 (&stream, -MAX_WBITS) != Z_OK) // raw deflate, no zlib header
            {
                error = "zlib init failed";
                return false;
            }
            stream.next_in = const_cast<Bytef*> (zip.data() + dataStart);
            stream.avail_in = static_cast<uInt> (compressedSize);
            stream.next_out = out.data();
            stream.avail_out = static_cast<uInt> (out.size());
            const int result = inflate (&stream, Z_FINISH);
            inflateEnd (&stream);
            if (result != Z_STREAM_END || stream.total_out != uncompressedSize)
            {
                error = "ZIP entry does not inflate cleanly";
                return false;
            }
            return true;
        }

        error = "ZIP entry '" + entryName + "' not found";
        return false;
    }
}
