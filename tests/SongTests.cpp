#include "bass2midi/song/Song.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace bass2midi;

namespace
{
    std::string dataPath (const std::string& name) { return std::string (BASS2MIDI_TEST_DATA_DIR) + "/" + name; }

    std::vector<unsigned char> readBytes (const std::string& path)
    {
        std::ifstream in (path, std::ios::binary);
        return { std::istreambuf_iterator<char> (in), std::istreambuf_iterator<char>() };
    }

    using NoteRow = std::tuple<int, long, int, int>; // track, start (1e-4 quarters), midi note, tie

    long quantise (double quarters) { return std::lround (quarters * 10000.0); }
}

TEST_CASE ("NotePalette: set operations and transposition")
{
    NotePalette palette;
    CHECK (palette.empty());
    palette.add (28);
    palette.add (64);
    palette.add (127);
    palette.add (128); // ignored
    palette.add (-1);  // ignored
    CHECK (palette.size() == 3);
    CHECK (palette.contains (28));
    CHECK (palette.contains (64));
    CHECK (palette.contains (127));
    CHECK_FALSE (palette.contains (29));

    const auto up = palette.transposed (12);
    CHECK (up.size() == 2); // 127 + 12 leaves the MIDI range
    CHECK (up.contains (40));
    CHECK (up.contains (76));
    CHECK (up.transposed (-12).contains (28));
}

TEST_CASE ("GPIF (.gp): rhythms, dots, tuplets, ties, rests, tempo and bass track")
{
    song::Song s;
    std::string error;
    REQUIRE_MESSAGE (song::loadSongFile (dataPath ("minimal.gp"), s, error), error);
    CHECK (s.format == "gp");
    CHECK (s.title == "Minimal & test");
    CHECK (s.artist == "Bass2MIDI");
    CHECK (s.barCount == 2);
    REQUIRE (s.tracks.size() == 2);
    CHECK_FALSE (s.tracks[0].bassLike);
    CHECK (s.tracks[1].bassLike);
    CHECK (s.tracks[1].tuning == std::vector<int> { 28, 33, 38, 43 });
    REQUIRE (song::findBassTrack (s) == 1);

    REQUIRE (s.tempos.size() == 2);
    CHECK (s.tempos[0].bpm == doctest::Approx (100.0));
    CHECK (s.tempos[1].atQuarters == doctest::Approx (4.0));
    CHECK (s.tempos[1].bpm == doctest::Approx (132.0));

    // Bar 1 (4/4): dotted quarter E1, eighth B1 tied into a quarter B1, then a quarter rest.
    // Bar 2 (3/4): eighth triplet D2 E2 G2, quarter G3 (12th fret), quarter C2 given only as MIDI.
    const std::vector<std::tuple<double, double, int, bool>> expected {
        { 0.0, 1.5, 28, false }, { 1.5, 0.5, 35, false }, { 2.0, 1.0, 35, true },
        { 4.0, 1.0 / 3.0, 38, false }, { 4.0 + 1.0 / 3.0, 1.0 / 3.0, 40, false }, { 4.0 + 2.0 / 3.0, 1.0 / 3.0, 43, false },
        { 5.0, 1.0, 55, false }, { 6.0, 1.0, 36, false }
    };
    const auto& notes = s.tracks[1].notes;
    REQUIRE (notes.size() == expected.size());
    for (std::size_t i = 0; i < notes.size(); ++i)
    {
        CAPTURE (i);
        CHECK (notes[i].startQuarters == doctest::Approx (std::get<0> (expected[i])));
        CHECK (notes[i].durationQuarters == doctest::Approx (std::get<1> (expected[i])));
        CHECK (notes[i].midiNote == std::get<2> (expected[i]));
        CHECK (notes[i].tieContinuation == std::get<3> (expected[i]));
    }

    const auto palette = song::paletteFromTrack (s.tracks[1]);
    CHECK (palette.size() == 7);
    CHECK (palette.contains (35));
    CHECK_FALSE (palette.contains (30));
}

TEST_CASE ("GP5: notes of every track match the reference parse (5.00 and 5.10)")
{
    std::ifstream csv (dataPath ("synthetic_gp5_expected.csv"));
    REQUIRE (csv.good());
    std::map<std::string, std::vector<NoteRow>> expected;
    std::string line;
    std::getline (csv, line); // header
    while (std::getline (csv, line))
    {
        std::istringstream row (line);
        std::string version, field;
        std::getline (row, version, ',');
        std::vector<std::string> fields;
        while (std::getline (row, field, ','))
            fields.push_back (field);
        REQUIRE (fields.size() == 4);
        expected[version].emplace_back (std::stoi (fields[0]), quantise (std::stod (fields[1])), std::stoi (fields[2]),
                                        std::stoi (fields[3]));
    }

    for (const auto* version : { "5.00", "5.10" })
    {
        CAPTURE (version);
        song::Song s;
        std::string error;
        REQUIRE_MESSAGE (song::loadSongFile (dataPath (std::string ("synthetic_") + version + ".gp5"), s, error), error);
        CHECK (s.format == "gp5");
        CHECK (s.title == "Synthetic");
        CHECK (s.barCount == 8);
        REQUIRE (s.tracks.size() == 3);
        CHECK (s.tracks[0].bassLike);
        CHECK (s.tracks[2].percussion);
        CHECK (song::findBassTrack (s) == 0);
        CHECK (s.tempos.size() > 1);

        std::vector<NoteRow> actual;
        for (std::size_t t = 0; t < s.tracks.size(); ++t)
            for (const auto& n : s.tracks[t].notes)
                actual.emplace_back (static_cast<int> (t), quantise (n.startQuarters), n.midiNote, n.tieContinuation ? 1 : 0);
        std::sort (actual.begin(), actual.end());
        auto& want = expected[version];
        std::sort (want.begin(), want.end());
        CHECK (actual.size() == want.size());
        CHECK (actual == want);
    }
}

TEST_CASE ("Song loader: truncated and foreign data fail cleanly")
{
    std::string error;
    song::Song s;
    CHECK_FALSE (song::loadSongData ({}, s, error));
    CHECK_FALSE (error.empty());

    const std::vector<unsigned char> text { 'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd' };
    CHECK_FALSE (song::loadSongData (text, s, error));

    for (const auto* name : { "synthetic_5.10.gp5", "minimal.gp" })
    {
        CAPTURE (name);
        const auto bytes = readBytes (dataPath (name));
        REQUIRE (bytes.size() > 100);
        // Every proper prefix (except one ending only before the optional final line-break byte)
        // must be rejected without crashing.
        int accepted = 0;
        for (std::size_t length = 0; length + 1 < bytes.size(); length += 7)
        {
            const std::vector<unsigned char> prefix (bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t> (length));
            accepted += song::loadSongData (prefix, s, error) ? 1 : 0;
        }
        CHECK (accepted == 0);

        // Flipped bytes must not crash either (the result may or may not parse).
        auto corrupted = bytes;
        for (std::size_t i = 40; i < corrupted.size(); i += 53)
            corrupted[i] = static_cast<unsigned char> (corrupted[i] ^ 0x5a);
        (void) song::loadSongData (corrupted, s, error);
    }
}

TEST_CASE ("Song timeline: repeats and alternate endings are unrolled into playing order (.gp and .gp5)")
{
    for (const auto* name : { "repeats.gp", "repeats.gp5" })
    {
        CAPTURE (name);
        song::Song s;
        std::string error;
        REQUIRE_MESSAGE (song::loadSongFile (dataPath (name), s, error), error);
        REQUIRE (s.masterBars.size() == 5);
        CHECK (s.masterBars[0].repeatStart);
        CHECK (s.masterBars[2].repeatPlays == 2);
        CHECK (s.masterBars[2].alternateEndings == 1u);
        CHECK (s.masterBars[3].alternateEndings == 2u);
        CHECK (song::playingOrder (s) == std::vector<int> { 0, 1, 2, 0, 1, 3, 4 });

        const auto notes = song::expectedNotes (s, 0);
        REQUIRE (notes.size() == 7);
        const std::vector<int> pitches { 28, 29, 30, 28, 29, 31, 32 };
        for (std::size_t i = 0; i < notes.size(); ++i)
        {
            CAPTURE (i);
            CHECK (notes[i].midiNote == pitches[i]);
            CHECK (notes[i].startSeconds == doctest::Approx (2.0 * static_cast<double> (i))); // whole notes at 120 bpm
            CHECK (notes[i].beat == doctest::Approx (1.0));
        }
        CHECK (notes[5].bar == 3);
        // .gp: tempo drops to 60 bpm in bar 4 (written), so the last whole note lasts 4 s.
        CHECK (notes[6].durationSeconds == doctest::Approx (std::string (name) == "repeats.gp" ? 4.0 : 2.0));
    }
}

TEST_CASE ("Song timeline: ties merge, chords reduce to the lowest note, beats are reported")
{
    song::Song s;
    std::string error;
    REQUIRE_MESSAGE (song::loadSongFile (dataPath ("minimal.gp"), s, error), error);
    const auto notes = song::expectedNotes (s, 1);
    // minimal.gp bass: E1 (dotted quarter), B1 eighth tied into a quarter, triplet D2 E2 G2, G3, C2.
    REQUIRE (notes.size() == 7);
    CHECK (notes[1].midiNote == 35);
    CHECK (notes[1].durationSeconds == doctest::Approx (1.5 * 60.0 / 100.0)); // eighth + tied quarter at 100 bpm
    CHECK (notes[1].beat == doctest::Approx (2.5));
    CHECK (notes[2].midiNote == 38);
    CHECK (notes[2].bar == 1);
    CHECK (notes[3].beat == doctest::Approx (1.0 + 1.0 / 3.0));

    // Generated GP5 with chords: no two expected notes share a start, every one is the lowest there.
    song::Song g;
    REQUIRE (song::loadSongFile (dataPath ("synthetic_5.10.gp5"), g, error));
    const auto expected = song::expectedNotes (g, 0);
    REQUIRE (! expected.empty());
    for (std::size_t i = 1; i < expected.size(); ++i)
        CHECK (expected[i].startSeconds > expected[i - 1].startSeconds);
}

TEST_CASE ("Song timeline: malformed repeat structures stay bounded")
{
    song::Song s;
    for (int i = 0; i < 3; ++i)
    {
        song::MasterBar bar;
        bar.startQuarters = 4.0 * i;
        bar.repeatStart = true;
        bar.repeatPlays = 200; // absurd
        bar.alternateEndings = i == 1 ? 0xffffffffu : 0u;
        s.masterBars.push_back (bar);
    }
    const auto order = song::playingOrder (s, 1000);
    CHECK (order.size() <= 1000);
    CHECK (! order.empty());
    CHECK (song::expectedNotes (s, 0).empty()); // no tracks
}

TEST_CASE ("Song timeline: bars in playing order carry their notes")
{
    song::Song s;
    std::string error;
    REQUIRE_MESSAGE (song::loadSongFile (dataPath ("repeats.gp"), s, error), error);
    std::vector<TimelineBar> bars;
    const auto notes = song::expectedNotes (s, 0, bars);
    REQUIRE (bars.size() == 7); // 0 1 2 0 1 3 4
    const std::vector<int> written { 0, 1, 2, 0, 1, 3, 4 };
    for (std::size_t i = 0; i < bars.size(); ++i)
    {
        CAPTURE (i);
        CHECK (bars[i].writtenBar == written[i]);
        CHECK (bars[i].startSeconds == doctest::Approx (2.0 * static_cast<double> (i)));
        CHECK (bars[i].noteCount == 1);
        CHECK (bars[i].firstNote == static_cast<int> (i));
        CHECK (notes[i].playedBar == static_cast<int> (i));
    }
    CHECK (bars[6].lengthSeconds == doctest::Approx (4.0)); // 60 bpm from bar 4

    // minimal.gp: bar 1 holds 2 attacked notes (the tie merges into B1), bar 2 holds 5.
    REQUIRE (song::loadSongFile (dataPath ("minimal.gp"), s, error));
    const auto m = song::expectedNotes (s, 1, bars);
    REQUIRE (bars.size() == 2);
    CHECK (bars[0].noteCount == 2);
    CHECK (bars[1].firstNote == 2);
    CHECK (bars[1].noteCount == 5);
    CHECK (bars[1].numerator == 3);
    CHECK (m.size() == 7);
}
