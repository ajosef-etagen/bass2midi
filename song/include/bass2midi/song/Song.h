#pragma once

#include "bass2midi/NotePalette.h"

#include <string>
#include <vector>

// Song files (Guitar Pro) for the optional song palette. Non-real-time: parsing allocates freely
// and must never run on the audio thread. The result is reduced to an immutable NotePalette that
// real-time code consumes.
namespace bass2midi::song
{
    struct Note
    {
        int midiNote = 0;               // sounding pitch (tuning + fret)
        double startQuarters = 0.0;     // from the start of the song, written order (repeats not unrolled)
        double durationQuarters = 0.0;
        int bar = 0;                    // 0-based master bar index
        bool tieContinuation = false;   // continues a tied note: no new attack
    };

    struct Track
    {
        std::string name;
        std::vector<int> tuning;        // MIDI notes, lowest string first
        bool percussion = false;
        bool bassLike = false;          // instrument says bass, or the lowest string is below C2
        std::vector<Note> notes;
    };

    struct TempoChange
    {
        double atQuarters = 0.0;
        double bpm = 120.0;
    };

    struct Song
    {
        std::string title, artist;
        std::string format;             // "gp" (Guitar Pro 7/8) or "gp5" (Guitar Pro 5)
        int barCount = 0;
        std::vector<Track> tracks;
        std::vector<TempoChange> tempos;
    };

    // Loads .gp (Guitar Pro 7/8: ZIP + GPIF XML) and .gp5 (Guitar Pro 5.00/5.10, binary). The format is
    // detected from the file contents, not the extension. Returns false and sets `error` otherwise.
    bool loadSongFile (const std::string& path, Song& out, std::string& error);

    // Same, from bytes already in memory.
    bool loadSongData (const std::vector<unsigned char>& bytes, Song& out, std::string& error);

    // Index of the most plausible bass track (bass-like, not percussion, most notes), or -1.
    int findBassTrack (const Song& song);

    // Palette of all attacked notes of a track (tie continuations add nothing new).
    NotePalette paletteFromTrack (const Track& track);
}
