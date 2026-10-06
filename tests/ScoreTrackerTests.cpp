#include <doctest/doctest.h>

#include "bass2midi/ScorePositionTracker.h"

#include <cmath>
#include <vector>

using namespace bass2midi;

namespace
{
    // 4/4 at 120 bpm (0.5 s beats). Bar: root on 1 (anchor), fills on 2.5, 3, 4, 4.5. Repetitive
    // (pairs of bars, an 8-bar cycle) unless `uniqueRoots`: then every bar's root differs from its
    // neighbours (a 12-bar cycle).
    struct Song
    {
        std::vector<ExpectedNote> notes;
        std::vector<TimelineBar> bars;

        explicit Song (int barCount, bool uniqueRoots = false)
        {
            const int roots[] = { 28, 33, 35, 31 };
            const double beats[] = { 1.0, 2.5, 3.0, 4.0, 4.5 };
            for (int b = 0; b < barCount; ++b)
            {
                TimelineBar bar;
                bar.writtenBar = b;
                bar.startSeconds = 2.0 * b;
                bar.lengthSeconds = 2.0;
                bar.firstNote = static_cast<int> (notes.size());
                const int root = uniqueRoots ? 28 + (b * 5) % 12 : roots[(b / 2) % 4];
                for (int i = 0; i < 5; ++i)
                {
                    ExpectedNote n;
                    n.midiNote = root + (i == 0 ? 0 : 7 + i);
                    n.startSeconds = bar.startSeconds + (beats[i] - 1.0) * 0.5;
                    n.durationSeconds = 0.25;
                    n.bar = b;
                    n.beat = beats[i];
                    n.playedBar = b;
                    n.anchorWeight = i == 0 ? 1.0 : 0.25;
                    notes.push_back (n);
                }
                bar.noteCount = 5;
                bars.push_back (bar);
            }
        }
    };

    double beatError (const ScorePositionTracker& t, double liveTime, double trueScore)
    {
        return (t.positionAt (liveTime).scoreSeconds - trueScore) / 0.5;
    }
}

TEST_CASE ("Score tracker: the clock runs from the first played note at the expected BPM")
{
    const Song song (16);
    ScorePositionTracker t;
    REQUIRE (t.setSettings ({}));
    t.setTimeline (song.notes.data(), (int) song.notes.size(), song.bars.data(), (int) song.bars.size());
    t.arm (2, 120.0);
    CHECK (t.isArmed());
    CHECK (t.positionAt (0.0).writtenBar == 2);
    t.onNote (1.0, song.notes[10].midiNote); // bar 3's root
    REQUIRE (t.isRunning());
    const auto p = t.positionAt (2.0);       // two beats later, no further notes
    CHECK (p.writtenBar == 2);
    CHECK (p.beat == doctest::Approx (3.0));
    CHECK (p.bpm == doctest::Approx (120.0));
    CHECK (t.positionAt (3.0).writtenBar == 3);
}

TEST_CASE ("Score tracker: roots only, live tempo 8 % off the given BPM, plus foreign fills")
{
    const Song song (24);
    ScorePositionTracker t;
    t.setTimeline (song.notes.data(), (int) song.notes.size(), song.bars.data(), (int) song.bars.size());
    t.arm (0, 120.0);
    const double ratio = 1.08; // the band plays 8 % faster than announced
    for (int b = 0; b < 24; ++b)
    {
        const double live = 2.0 * b / ratio;
        if (b >= 6)
            CHECK (std::abs (beatError (t, live, 2.0 * b)) < 0.25); // what the display shows before the note
        t.onNote (live + 0.01, song.notes[(std::size_t) b * 5].midiNote);
        t.onNote (live + 0.6 / ratio, 50); // a fill the score does not have
    }
    CHECK (t.positionAt (2.0 * 23 / ratio).bpm == doctest::Approx (120.0 * ratio).epsilon (0.03));
}

TEST_CASE ("Score tracker: a skipped section is found again by a jump candidate")
{
    const Song song (32, true);
    ScorePositionTracker t;
    t.setTimeline (song.notes.data(), (int) song.notes.size(), song.bars.data(), (int) song.bars.size());
    t.arm (0, 120.0);
    // Bars 0-7 played, then the band jumps from bar 8 to bar 13 (5 bars skipped), playing full bars.
    std::vector<int> order;
    for (int b = 0; b < 8; ++b)
        order.push_back (b);
    for (int b = 13; b < 25; ++b)
        order.push_back (b);
    int recoveredAfter = -1;
    for (std::size_t k = 0; k < order.size(); ++k)
    {
        const int b = order[k];
        const double barStartLive = 2.0 * static_cast<double> (k);
        if (k >= 8 && recoveredAfter < 0 && std::abs (beatError (t, barStartLive, 2.0 * b)) < 0.25)
            recoveredAfter = static_cast<int> (k) - 8;
        for (int i = 0; i < 5; ++i)
        {
            const auto& n = song.notes[(std::size_t) b * 5 + (std::size_t) i];
            t.onNote (barStartLive + (n.startSeconds - 2.0 * b), n.midiNote);
        }
    }
    CHECK (recoveredAfter >= 0);
    CHECK (recoveredAfter <= 3); // within three bars of the jump
    CHECK (t.getJumps() >= 1);
}

TEST_CASE ("Score tracker: the clock waits when the player stops, and continues with the player")
{
    const Song song (16);
    ScorePositionTracker t;
    t.setTimeline (song.notes.data(), (int) song.notes.size(), song.bars.data(), (int) song.bars.size());
    t.arm (0, 120.0);
    for (int b = 0; b < 4; ++b)
        t.onNote (2.0 * b, song.notes[(std::size_t) b * 5].midiNote);

    // Silence for 14 s: the position stops one beat past bar 4's root (the awaited note).
    const auto held = t.positionAt (20.0);
    CHECK (held.waiting);
    CHECK (held.writtenBar == 4);
    CHECK (held.beat == doctest::Approx (2.0));
    CHECK (t.positionAt (30.0).scoreSeconds == doctest::Approx (held.scoreSeconds));

    // The player resumes with bar 4's root: the position continues from there at the old tempo.
    for (int b = 4; b < 8; ++b)
    {
        const double live = 20.0 + 2.0 * (b - 4);
        if (b > 4)
            CHECK (std::abs (beatError (t, live, 2.0 * b)) < 0.25);
        t.onNote (live, song.notes[(std::size_t) b * 5].midiNote);
        CHECK_FALSE (t.positionAt (live + 0.01).waiting);
    }
    CHECK (t.positionAt (26.5).bpm == doctest::Approx (120.0).epsilon (0.05));
}

TEST_CASE ("Score tracker: a player 20 % slower than the BPM is followed, not run over")
{
    const Song song (24);
    ScorePositionTracker t;
    t.setTimeline (song.notes.data(), (int) song.notes.size(), song.bars.data(), (int) song.bars.size());
    t.arm (0, 120.0);
    const double ratio = 0.8;
    for (int b = 0; b < 24; ++b)
    {
        const double live = 2.0 * b / ratio;
        // Never more than the hold (one beat) ahead of the player; on time once the tempo is learned.
        if (b >= 1)
            CHECK (beatError (t, live, 2.0 * b) <= 1.0 + 1.0e-6);
        if (b >= 8)
            CHECK (std::abs (beatError (t, live, 2.0 * b)) < 0.25);
        t.onNote (live, song.notes[(std::size_t) b * 5].midiNote);
    }
    CHECK (t.positionAt (2.0 * 23 / ratio).bpm == doctest::Approx (120.0 * ratio).epsilon (0.05));
}

TEST_CASE ("Score tracker: settings validation and empty timeline")
{
    ScorePositionTracker::Settings s;
    CHECK (s.isValid());
    s.background = 0.0;
    CHECK_FALSE (s.isValid());
    ScorePositionTracker t;
    t.arm (0, 120.0);
    t.onNote (1.0, 40);
    CHECK_FALSE (t.isRunning());
    CHECK (t.positionAt (1.0).playedBar == -1);
}
