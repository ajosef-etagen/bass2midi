#include "Readers.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

// Guitar Pro 5 (.gp5, versions 5.00 and 5.10): a little-endian binary stream with no index, so every
// field up to the last note has to be consumed in order. Only what the song palette needs is kept
// (tracks, tunings, note pitches/timing, tempo); everything else is skipped by its known size.
namespace bass2midi::song::detail
{
    namespace
    {
        // Sanity limits so a corrupt file fails cleanly instead of looping over garbage.
        constexpr int maxMeasures = 4096;
        constexpr int maxTracks = 128;
        constexpr int maxBeatsPerVoice = 512;
        constexpr int maxListItems = 1024;

        struct ByteReader
        {
            const std::vector<unsigned char>& data;
            std::size_t pos = 0;
            bool failed = false;

            bool has (std::size_t n)
            {
                if (failed || pos + n > data.size())
                {
                    failed = true;
                    return false;
                }
                return true;
            }

            void skip (std::size_t n)
            {
                if (has (n))
                    pos += n;
            }

            int u8()
            {
                return has (1) ? data[pos++] : 0;
            }

            int i8() { return static_cast<std::int8_t> (static_cast<std::uint8_t> (u8())); }

            int i16()
            {
                if (! has (2))
                    return 0;
                const auto v = static_cast<std::uint16_t> (data[pos] | (data[pos + 1] << 8));
                pos += 2;
                return static_cast<std::int16_t> (v);
            }

            int i32()
            {
                if (! has (4))
                    return 0;
                const std::uint32_t v = static_cast<std::uint32_t> (data[pos]) | (static_cast<std::uint32_t> (data[pos + 1]) << 8)
                                      | (static_cast<std::uint32_t> (data[pos + 2]) << 16) | (static_cast<std::uint32_t> (data[pos + 3]) << 24);
                pos += 4;
                return static_cast<std::int32_t> (v);
            }

            std::string chars (std::size_t n)
            {
                if (! has (n))
                    return {};
                std::string s (reinterpret_cast<const char*> (data.data() + pos), n);
                pos += n;
                return s;
            }

            // One length byte, then a fixed-size field of `capacity` bytes.
            std::string byteSizeString (std::size_t capacity)
            {
                const auto length = static_cast<std::size_t> (u8());
                auto field = chars (capacity);
                return field.substr (0, std::min (length, field.size()));
            }

            // Int32 (field size = length + 1), then a length byte and the characters.
            std::string intByteSizeString()
            {
                const int fieldSize = i32();
                if (fieldSize < 0 || fieldSize > 1 << 20)
                {
                    failed = true;
                    return {};
                }
                if (fieldSize == 0)
                    return {};
                const auto length = static_cast<std::size_t> (u8());
                auto field = chars (static_cast<std::size_t> (fieldSize - 1));
                return field.substr (0, std::min (length, field.size()));
            }

            // Int32 length, then the characters.
            std::string intSizeString()
            {
                const int length = i32();
                if (length < 0 || length > 1 << 20)
                {
                    failed = true;
                    return {};
                }
                return chars (static_cast<std::size_t> (length));
            }
        };

        struct MeasureHeader
        {
            int numerator = 4;
            int denominator = 4;
        };

        struct TrackInfo
        {
            int stringCount = 0;
            std::array<int, 7> tuning {}; // string 1 (highest) first
            std::array<std::array<int, 7>, 2> lastPitch {}; // per voice: ties continue the previous note of the string
        };

        void skipBend (ByteReader& r)
        {
            r.skip (1 + 4); // type, value
            const int points = r.i32();
            if (points < 0 || points > maxListItems)
            {
                r.failed = true;
                return;
            }
            r.skip (static_cast<std::size_t> (points) * 9); // position, value, vibrato
        }

        void skipBeatEffects (ByteReader& r)
        {
            const int flags1 = r.u8();
            const int flags2 = r.u8();
            if (flags1 & 0x20) r.skip (1);     // tapping / slap / pop
            if (flags2 & 0x04) skipBend (r);   // tremolo bar
            if (flags1 & 0x40) r.skip (2);     // stroke down / up speed
            if (flags2 & 0x02) r.skip (1);     // pick stroke direction
        }

        // Returns the new tempo in bpm, or -1 if the mix table does not change it.
        int readMixTable (ByteReader& r, bool v500)
        {
            r.skip (1);                      // instrument
            r.skip (v500 ? 4 + 4 + 4 + 2 + 1 : 16); // RSE instrument
            if (v500)
                r.skip (1);
            std::array<int, 6> values {};    // volume, balance, chorus, reverb, phaser, tremolo
            for (auto& v : values)
                v = r.i8();
            r.intByteSizeString();           // tempo name
            const int tempo = r.i32();
            for (const int v : values)
                if (v >= 0)
                    r.skip (1);              // transition duration
            if (tempo >= 0)
            {
                r.skip (1);                  // tempo transition duration
                if (! v500)
                    r.skip (1);              // hide tempo
            }
            r.skip (1);                      // apply-to-all-tracks flags
            r.skip (1);                      // wah
            if (! v500)
            {
                r.intByteSizeString();       // RSE effect
                r.intByteSizeString();       // RSE effect category
            }
            return tempo;
        }

        void skipNoteEffects (ByteReader& r)
        {
            const int flags1 = r.u8();
            const int flags2 = r.u8();
            if (flags1 & 0x01) skipBend (r);
            if (flags1 & 0x10) r.skip (5);   // grace note: fret, velocity, transition, duration, flags
            if (flags2 & 0x04) r.skip (1);   // tremolo picking speed
            if (flags2 & 0x08) r.skip (1);   // slide type
            if (flags2 & 0x10)               // harmonic
            {
                const int type = r.i8();
                if (type == 2) r.skip (3);   // artificial: semitone, accidental, octave
                else if (type == 3) r.skip (1); // tapped: fret
            }
            if (flags2 & 0x20) r.skip (2);   // trill: fret, period
        }

        constexpr int noteTypeTie = 2;
        constexpr int noteTypeDead = 3;

        double tupletTimes (int enters)
        {
            switch (enters)
            {
                case 3: return 2.0;
                case 5: case 6: case 7: return 4.0;
                case 9: case 10: case 11: case 12: case 13: return 8.0;
                default: return static_cast<double> (enters);
            }
        }
    }

    bool looksLikeGp5 (const std::vector<unsigned char>& bytes)
    {
        if (bytes.size() < 31)
            return false;
        const std::string version (reinterpret_cast<const char*> (bytes.data() + 1), std::min<std::size_t> (bytes[0], 30));
        return version.rfind ("FICHIER GUITAR PRO v5", 0) == 0;
    }

    bool readGp5 (const std::vector<unsigned char>& bytes, Song& song, std::string& error)
    {
        ByteReader r { bytes };
        const auto version = r.byteSizeString (30);
        bool v500 = false;
        if (version == "FICHIER GUITAR PRO v5.00")
            v500 = true;
        else if (version != "FICHIER GUITAR PRO v5.10")
        {
            error = "unsupported Guitar Pro version '" + version + "' (only 5.00 and 5.10)";
            return false;
        }

        song = {};
        song.format = "gp5";

        // Song info: title, subtitle, artist, album, words, music, copyright, tab, instructions.
        std::array<std::string, 9> songInfo;
        for (auto& s : songInfo)
            s = r.intByteSizeString();
        song.title = songInfo[0];
        song.artist = songInfo[2];
        const int noticeLines = r.i32();
        if (noticeLines < 0 || noticeLines > maxListItems)
            r.failed = true;
        for (int i = 0; i < noticeLines && ! r.failed; ++i)
            r.intByteSizeString();

        // Lyrics: track, then 5 lines of (starting measure, text).
        r.i32();
        for (int i = 0; i < 5; ++i)
        {
            r.i32();
            r.intSizeString();
        }

        if (! v500)
            r.skip (4 + 4 + 11); // RSE master effect: volume, unused, 11-band equalizer

        // Page setup: size (2), margins (4), score proportion, header/footer flags, 10 template strings.
        r.skip (7 * 4 + 2);
        for (int i = 0; i < 10; ++i)
            r.intByteSizeString();

        r.intByteSizeString(); // tempo name
        const int initialTempo = r.i32();
        if (! v500)
            r.skip (1);        // hide tempo
        r.skip (1 + 4);        // key signature, octave

        // 64 MIDI channels (4 ports x 16): instrument, volume, balance, chorus, reverb, phaser,
        // tremolo, 2 padding bytes.
        std::array<int, 64> channelProgram {};
        for (auto& program : channelProgram)
        {
            program = r.i32();
            r.skip (8);
        }

        r.skip (19 * 2); // musical directions (coda, segno, ...)
        r.skip (4);      // master reverb

        const int measureCount = r.i32();
        const int trackCount = r.i32();
        if (r.failed || measureCount < 0 || measureCount > maxMeasures || trackCount <= 0 || trackCount > maxTracks)
        {
            error = "corrupt GP5 header";
            return false;
        }
        if (initialTempo > 0)
            song.tempos.push_back ({ 0.0, static_cast<double> (initialTempo) });

        // Measure headers.
        std::vector<MeasureHeader> headers (static_cast<std::size_t> (measureCount));
        for (int m = 0; m < measureCount && ! r.failed; ++m)
        {
            auto& header = headers[static_cast<std::size_t> (m)];
            if (m > 0)
            {
                r.skip (1);
                header = headers[static_cast<std::size_t> (m - 1)];
            }
            const int flags = r.u8();
            if (flags & 0x01) header.numerator = r.i8();
            if (flags & 0x02) header.denominator = r.i8();
            if (flags & 0x08) r.skip (1);         // repeat close count
            if (flags & 0x20)                     // marker: name, colour
            {
                r.intByteSizeString();
                r.skip (4);
            }
            if (flags & 0x40) r.skip (2);         // key signature root, type
            if (flags & 0x10) r.skip (1);         // repeat alternative
            if (flags & 0x03) r.skip (4);         // beam grouping
            if ((flags & 0x10) == 0) r.skip (1);
            r.skip (1);                           // triplet feel
            if (header.numerator <= 0 || header.denominator <= 0)
                r.failed = true;
        }

        // Tracks.
        std::vector<TrackInfo> infos (static_cast<std::size_t> (trackCount));
        for (int t = 0; t < trackCount && ! r.failed; ++t)
        {
            if (t == 0 || v500)
                r.skip (1);
            const int flags = r.u8();
            Track track;
            track.name = r.byteSizeString (40);
            auto& info = infos[static_cast<std::size_t> (t)];
            info.stringCount = r.i32();
            for (auto& pitch : info.tuning)
                pitch = r.i32();
            const int port = r.i32();
            const int channel = r.i32();
            r.skip (4);     // effect channel
            r.skip (4 + 4); // fret count, capo
            r.skip (4);     // colour
            r.skip (2 + 1 + 1); // flags 2, auto accentuation, bank
            // RSE: humanize, 3 ints, 12 reserved bytes, instrument (4 ints; 5.00: 3 ints + int16 + 1).
            r.skip (1 + 12 + 12 + (v500 ? 15 : 16));
            if (! v500)
            {
                r.skip (4);                 // 4-band equalizer
                r.intByteSizeString();      // RSE effect
                r.intByteSizeString();      // RSE effect category
            }
            if (info.stringCount < 1 || info.stringCount > 7)
            {
                r.failed = true;
                break;
            }

            for (int s = info.stringCount; s-- > 0;)
                track.tuning.push_back (info.tuning[static_cast<std::size_t> (s)]);
            std::sort (track.tuning.begin(), track.tuning.end());

            const int channelIndex = (port - 1) * 16 + (channel - 1);
            const int program = channelIndex >= 0 && channelIndex < 64 ? channelProgram[static_cast<std::size_t> (channelIndex)] : -1;
            std::string lowerName = track.name;
            for (auto& c : lowerName)
                c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
            track.percussion = (flags & 0x01) != 0;
            track.bassLike = ! track.percussion
                             && ((program >= 32 && program <= 39) || lowerName.find ("bass") != std::string::npos
                                 || track.tuning.front() < 36);
            song.tracks.push_back (std::move (track));
        }
        r.skip (v500 ? 2 : 1);

        // Measures: for every measure, every track: two voices of beats, then a line-break byte.
        double measureStart = 0.0;
        for (int m = 0; m < measureCount && ! r.failed; ++m)
        {
            const auto& header = headers[static_cast<std::size_t> (m)];
            for (int t = 0; t < trackCount && ! r.failed; ++t)
            {
                auto& track = song.tracks[static_cast<std::size_t> (t)];
                auto& info = infos[static_cast<std::size_t> (t)];
                for (int voice = 0; voice < 2 && ! r.failed; ++voice)
                {
                    const int beatCount = r.i32();
                    if (beatCount < 0 || beatCount > maxBeatsPerVoice)
                    {
                        r.failed = true;
                        break;
                    }
                    double position = measureStart;
                    for (int b = 0; b < beatCount && ! r.failed; ++b)
                    {
                        const int flags = r.u8();
                        int status = 1; // 0 empty, 1 normal, 2 rest
                        if (flags & 0x40)
                            status = r.u8();
                        const int durationCode = r.i8(); // -2 whole ... 4 sixty-fourth
                        double quarters = 4.0 / static_cast<double> (1 << std::clamp (durationCode + 2, 0, 8));
                        if (flags & 0x01)
                            quarters *= 1.5;
                        if (flags & 0x20)
                        {
                            const int enters = r.i32();
                            if (enters > 0)
                                quarters = quarters * tupletTimes (enters) / enters;
                        }
                        if (flags & 0x02) r.skip (107);                // chord diagram
                        if (flags & 0x04) r.intByteSizeString();       // text
                        if (flags & 0x08) skipBeatEffects (r);
                        if (flags & 0x10)
                        {
                            const int tempo = readMixTable (r, v500);
                            if (tempo > 0)
                                song.tempos.push_back ({ position, static_cast<double> (tempo) });
                        }

                        const int stringFlags = r.u8();
                        for (int stringNumber = 1; stringNumber <= 7 && ! r.failed; ++stringNumber)
                        {
                            if ((stringFlags & (1 << (7 - stringNumber))) == 0)
                                continue;
                            const int noteFlags = r.u8();
                            int type = 1;
                            int fret = 0;
                            if (noteFlags & 0x20) type = r.u8();
                            if (noteFlags & 0x10) r.skip (1);  // dynamic
                            if (noteFlags & 0x20) fret = r.i8();
                            if (noteFlags & 0x80) r.skip (2);  // left/right hand fingering
                            if (noteFlags & 0x01) r.skip (8);  // duration percent (double)
                            r.skip (1);                        // flags 2
                            if (noteFlags & 0x08) skipNoteEffects (r);

                            if (stringNumber > info.stringCount)
                                continue;
                            const auto stringIndex = static_cast<std::size_t> (stringNumber - 1);
                            auto& lastPitch = info.lastPitch[static_cast<std::size_t> (voice)][stringIndex];
                            const bool tie = type == noteTypeTie;
                            const int pitch = tie ? lastPitch : info.tuning[stringIndex] + fret;
                            lastPitch = pitch;
                            if (type == noteTypeDead || status == 2 || pitch <= 0 || pitch > 127)
                                continue;

                            Note note;
                            note.midiNote = pitch;
                            note.startQuarters = position;
                            note.durationQuarters = quarters;
                            note.bar = m;
                            note.tieContinuation = tie;
                            track.notes.push_back (note);
                        }

                        const int flags2 = r.i16();
                        if (flags2 & 0x0800)
                            r.skip (1); // break secondary beams
                        if (status != 0)
                            position += quarters;
                    }
                }
                if (r.pos < r.data.size()) // line break; some writers omit it after the very last measure
                    r.skip (1);
            }
            measureStart += 4.0 * header.numerator / header.denominator;
        }

        if (r.failed)
        {
            error = "corrupt or unsupported GP5 data near byte " + std::to_string (r.pos);
            return false;
        }

        song.barCount = measureCount;
        std::stable_sort (song.tempos.begin(), song.tempos.end(),
                          [] (const TempoChange& a, const TempoChange& b) { return a.atQuarters < b.atQuarters; });
        for (auto& track : song.tracks)
            std::stable_sort (track.notes.begin(), track.notes.end(),
                              [] (const Note& a, const Note& b) { return a.startQuarters < b.startQuarters; });
        return true;
    }
}
