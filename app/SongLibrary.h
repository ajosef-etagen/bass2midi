#pragma once

#include "bass2midi/NotePalette.h"
#include "bass2midi/song/Song.h"

#include <juce_core/juce_core.h>

#include <optional>
#include <vector>

// The user's song list for the song palette (message thread only; never touched by the audio
// thread). Songs are Guitar Pro files (.gp, .gp5) parsed when added or when the list is restored;
// each entry remembers which track is the bass part and the key the band plays it in.
// Persisted as file paths plus track choice and transposition.
class SongLibrary
{
public:
    struct Entry
    {
        juce::File file;
        bass2midi::song::Song song;   // empty if loading failed
        juce::String error;           // why loading failed, empty if fine
        int track = -1;               // chosen track (default: findBassTrack), -1 = none usable
        int transpose = 0;            // semitones from the file's key to the band's key (song::transposeTrack)

        bool usable() const { return error.isEmpty() && track >= 0 && track < (int) song.tracks.size(); }
        juce::String displayName() const;
        // The song as the band plays it: the chosen track transposed by `transpose`. Palette, timeline
        // and tablature are built from this, never from `song` directly.
        bass2midi::song::Song played() const;
        bass2midi::NotePalette palette() const;
    };

    static constexpr const char* fileWildcard = "*.gp;*.gp5";

    // Adds a file (no duplicates); returns its index. Failed files are listed with their error.
    int add (const juce::File& file);
    void remove (int index);
    void setTrack (int index, int track);
    void setTranspose (int index, int semitones); // clamped to +-song::maxTransposeSemitones

    int size() const { return (int) entries.size(); }
    const Entry& operator[] (int index) const { return entries[(size_t) index]; }

    // Persistence: one line per song, "<track>/<transpose>\t<full path>" (older "<track>\t<path>"
    // lines load with transpose 0).
    juce::String toState() const;
    void restoreState (const juce::String& state);

    // Human-readable palette, e.g. "G1 A1 C2 D2 (4 notes)".
    static juce::String describe (const bass2midi::NotePalette& palette);

private:
    std::vector<Entry> entries;
};
