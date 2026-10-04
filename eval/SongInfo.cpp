// Prints the tracks and the bass palette of a Guitar Pro (.gp / .gp5) file.
//   bass2midi_songinfo <file> [--notes]

#include "bass2midi/MidiNoteUtils.h"
#include "bass2midi/song/Song.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf (stderr, "usage: %s <file.gp|file.gp5> [--notes]\n", argv[0]);
        return 2;
    }
    const bool listNotes = argc > 2 && std::strcmp (argv[2], "--notes") == 0;

    bass2midi::song::Song song;
    std::string error;
    if (! bass2midi::song::loadSongFile (argv[1], song, error))
    {
        std::fprintf (stderr, "error: %s\n", error.c_str());
        return 1;
    }

    std::printf ("format %s, title \"%s\", artist \"%s\", %d bars, %zu tempo events (first %.0f bpm)\n",
                 song.format.c_str(), song.title.c_str(), song.artist.c_str(), song.barCount, song.tempos.size(),
                 song.tempos.empty() ? 0.0 : song.tempos.front().bpm);

    const int bass = bass2midi::song::findBassTrack (song);
    for (std::size_t i = 0; i < song.tracks.size(); ++i)
    {
        const auto& t = song.tracks[i];
        int ties = 0;
        for (const auto& n : t.notes)
            ties += n.tieContinuation ? 1 : 0;
        std::printf ("%c track %zu \"%s\": %zu notes (%d tie continuations), tuning", static_cast<int> (i) == bass ? '*' : ' ',
                     i, t.name.c_str(), t.notes.size(), ties);
        for (const int p : t.tuning)
            std::printf (" %d", p);
        std::printf ("%s%s\n", t.percussion ? ", percussion" : "", t.bassLike ? ", bass" : "");
    }

    if (bass < 0)
    {
        std::printf ("no bass track found\n");
    }
    else
    {
        const auto& track = song.tracks[static_cast<std::size_t> (bass)];
        std::map<int, int> counts;
        for (const auto& n : track.notes)
            if (! n.tieContinuation)
                ++counts[n.midiNote];
        std::printf ("palette (%d notes):", bass2midi::song::paletteFromTrack (track).size());
        for (const auto& [note, count] : counts)
            std::printf (" %s(%d)x%d", bass2midi::midi::noteName (note).c_str(), note, count);
        std::printf ("\n");
    }

    if (listNotes) // machine-readable: N <track> <bar> <start quarters> <duration quarters> <midi note> <tie>
        for (std::size_t i = 0; i < song.tracks.size(); ++i)
            for (const auto& n : song.tracks[i].notes)
                std::printf ("N %zu %d %.4f %.4f %d %d\n", i, n.bar, n.startQuarters, n.durationQuarters, n.midiNote,
                             n.tieContinuation ? 1 : 0);
    return 0;
}
