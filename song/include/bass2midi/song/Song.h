#pragma once

#include "bass2midi/ExpectedNote.h"
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
        int string = -1;                // as written in the file: 0 = lowest string of the track's tuning; -1 unknown
        int fret = -1;                  // -1 unknown
    };

    struct Track
    {
        std::string name;
        std::vector<int> tuning;        // MIDI notes, lowest string first
        bool percussion = false;
        bool bassLike = false;          // instrument says bass, or the lowest string is below C2
        std::vector<Note> notes;
    };

    // One bar of the written score (shared by all tracks), with the repeat structure needed to
    // unroll it into playing order. Codas, segnos and D.C./D.S. jumps are not represented.
    struct MasterBar
    {
        double startQuarters = 0.0;     // written position
        double lengthQuarters = 4.0;
        int numerator = 4, denominator = 4;
        bool repeatStart = false;
        int repeatPlays = 0;            // > 0: a repeat closes after this bar, played this many times in total
        unsigned alternateEndings = 0;  // bit k set: bar belongs to ending k+1 (0 = always played)
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
        std::vector<MasterBar> masterBars;
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

    // Written bar indices in playing order: simple repeats (with a play count) and alternate endings
    // are unrolled; nested repeats, codas, segnos and D.C./D.S. are not interpreted. At most
    // maxBars entries (guards against malformed repeat structures).
    std::vector<int> playingOrder (const Song& song, int maxBars = 20000);

    // The expected-note timeline of a track for Song Mode (see bass2midi/ExpectedNote.h): playing
    // order, tempo map applied, tied continuations merged into their note, simultaneous attacks
    // reduced to the lowest note, grace notes (no metric time) dropped.
    std::vector<ExpectedNote> expectedNotes (const Song& song, int track);

    // Tablature (string/fret for 4-string EADG) and anchor weights are filled in: the file's own
    // string/fret where the track is tuned EADG, else a fingering with few position changes
    // (see assignFingering).

    // Same notes plus the bars in playing order they belong to (every played bar, also empty ones).
    std::vector<ExpectedNote> expectedNotes (const Song& song, int track, std::vector<TimelineBar>& bars);

    // Chooses string/fret on a 4-string EADG bass for every note with string < 0, minimising position
    // changes (open strings are free) with a mild preference for low positions (Viterbi over the
    // whole part). Notes outside E1..fret maxFret on G keep string = fret = -1.
    void assignFingering (std::vector<ExpectedNote>& notes, int maxFret = 20);
}
