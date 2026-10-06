#include "bass2midi/song/Song.h"

#include "Readers.h"
#include "ZipReader.h"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace bass2midi::song
{
    namespace
    {
        constexpr std::size_t maxSongFileBytes = 64u * 1024u * 1024u;
    }

    bool loadSongData (const std::vector<unsigned char>& bytes, Song& out, std::string& error)
    {
        if (detail::looksLikeZip (bytes))
            return detail::readGpif (bytes, out, error);
        if (detail::looksLikeGp5 (bytes))
            return detail::readGp5 (bytes, out, error);
        if (bytes.size() > 4 && bytes[0] == 'B' && bytes[1] == 'C' && bytes[2] == 'F' && bytes[3] == 'Z')
            error = "Guitar Pro 6 (.gpx) files are not supported; save as .gp or .gp5";
        else
            error = "not a Guitar Pro .gp or .gp5 file";
        return false;
    }

    bool loadSongFile (const std::string& path, Song& out, std::string& error)
    {
        std::ifstream in (path, std::ios::binary);
        if (! in)
        {
            error = "cannot open " + path;
            return false;
        }
        in.seekg (0, std::ios::end);
        const auto size = static_cast<std::size_t> (in.tellg());
        if (size > maxSongFileBytes)
        {
            error = "file too large for a song file";
            return false;
        }
        in.seekg (0, std::ios::beg);
        std::vector<unsigned char> bytes (size);
        in.read (reinterpret_cast<char*> (bytes.data()), static_cast<std::streamsize> (size));
        if (! in)
        {
            error = "cannot read " + path;
            return false;
        }
        return loadSongData (bytes, out, error);
    }

    int findBassTrack (const Song& song)
    {
        int best = -1;
        std::size_t bestNotes = 0;
        for (int pass = 0; pass < 2 && best < 0; ++pass) // prefer bass-like tracks, else any pitched one
            for (std::size_t i = 0; i < song.tracks.size(); ++i)
            {
                const auto& t = song.tracks[i];
                if (t.percussion || (pass == 0 && ! t.bassLike) || t.notes.empty())
                    continue;
                if (best < 0 || t.notes.size() > bestNotes)
                {
                    best = static_cast<int> (i);
                    bestNotes = t.notes.size();
                }
            }
        return best;
    }

    NotePalette paletteFromTrack (const Track& track)
    {
        NotePalette palette;
        for (const auto& note : track.notes)
            if (! note.tieContinuation)
                palette.add (note.midiNote);
        return palette;
    }

    bool transposeTrack (Track& track, int semitones)
    {
        if (semitones < -maxTransposeSemitones || semitones > maxTransposeSemitones)
            return false;
        if (semitones == 0)
            return true;
        constexpr int lowestEadgNote = 28; // E1, open low E
        constexpr int maxFret = 20;
        for (auto& note : track.notes)
        {
            const int original = note.midiNote;
            int shifted = original + semitones;
            bool folded = false;
            if (shifted < lowestEadgNote && original >= lowestEadgNote)
            {
                shifted += 12;
                folded = true;
            }
            note.midiNote = std::clamp (shifted, 0, 127);
            if (note.string >= 0 && note.fret >= 0)
            {
                const int fret = note.fret + semitones;
                if (folded || note.midiNote != shifted || fret < 0 || fret > maxFret)
                    note.string = note.fret = -1;
                else
                    note.fret = fret;
            }
        }
        return true;
    }
}
