#include <doctest/doctest.h>

#include "bass2midi/BarPlayer.h"

#include <cmath>
#include <vector>

using namespace bass2midi;

namespace
{
    // 4/4 at 120 bpm (2 s bars). Each bar: root on beat 1, fills on beats 2, 3, 3.5 and 4.
    struct Song
    {
        std::vector<ExpectedNote> notes;
        std::vector<TimelineBar> bars;

        explicit Song (int barCount, bool restInBar3 = false)
        {
            const int roots[] = { 28, 33, 38, 31 };
            for (int b = 0; b < barCount; ++b)
            {
                TimelineBar bar;
                bar.writtenBar = b;
                bar.startSeconds = 2.0 * b;
                bar.lengthSeconds = 2.0;
                bar.firstNote = static_cast<int> (notes.size());
                if (! (restInBar3 && b == 2))
                    for (const double beat : { 1.0, 2.0, 3.0, 3.5, 4.0 })
                    {
                        ExpectedNote n;
                        n.midiNote = roots[b % 4] + (beat == 1.0 ? 0 : static_cast<int> (beat * 2) % 7);
                        n.startSeconds = bar.startSeconds + (beat - 1.0) * 0.5;
                        n.durationSeconds = 0.25;
                        n.bar = b;
                        n.beat = beat;
                        n.playedBar = b;
                        notes.push_back (n);
                    }
                bar.noteCount = static_cast<int> (notes.size()) - bar.firstNote;
                bars.push_back (bar);
            }
        }
    };

    struct Event { double time; NoteEvent e; };

    // Runs the player in 1 ms steps; anchors: (time, velocity). Checks Note On/Off pairing.
    struct Run
    {
        BarPlayer player;
        std::vector<Event> events;
        int sounding = -1;

        Run (const Song& s, int startBar = 0)
        {
            REQUIRE (player.setSettings ({}));
            player.setTimeline (s.notes.data(), (int) s.notes.size(), s.bars.data(), (int) s.bars.size());
            BarPlayer::Output out;
            player.start (startBar, out);
            take (out, 0.0);
        }

        void take (const BarPlayer::Output& out, double t)
        {
            for (int i = 0; i < out.count; ++i)
            {
                const auto& e = out.events[(std::size_t) i];
                if (e.type == NoteEvent::Type::noteOn)
                {
                    REQUIRE (sounding < 0);
                    sounding = e.note;
                }
                else
                {
                    REQUIRE (sounding == e.note);
                    sounding = -1;
                }
                events.push_back ({ t, e });
            }
        }

        // Plays until `end`; attacks at the given times; verdicts (pitch) after 40 ms if given.
        void play (const std::vector<std::pair<double, int>>& attacks, double end, const std::vector<int>& roots = {})
        {
            std::size_t next = 0;
            std::vector<std::pair<double, int>> verdicts;
            for (double t = 0.0; t <= end; t += 0.001)
            {
                BarPlayer::Output out;
                while (next < attacks.size() && attacks[next].first <= t)
                {
                    const bool anchor = player.onAttack (attacks[next].first, attacks[next].second, out);
                    if (anchor && next < roots.size())
                        verdicts.push_back ({ attacks[next].first + 0.04, roots[next] });
                    ++next;
                }
                for (auto& v : verdicts)
                    if (v.second >= 0 && v.first <= t)
                    {
                        player.onAnchorPitch (v.second);
                        v.second = -1;
                    }
                player.advance (t, out);
                take (out, t);
            }
        }

        std::vector<Event> noteOns() const
        {
            std::vector<Event> ons;
            for (const auto& e : events)
                if (e.e.type == NoteEvent::Type::noteOn)
                    ons.push_back (e);
            return ons;
        }
    };
}

TEST_CASE ("Bar player: roots on the downbeats play every written note on time")
{
    const Song song (4);
    Run run (song);
    run.play ({ { 1.0, 90 }, { 3.0, 90 }, { 5.0, 90 }, { 7.0, 90 } }, 9.5);
    const auto ons = run.noteOns();
    REQUIRE (ons.size() == song.notes.size());
    for (std::size_t i = 0; i < ons.size(); ++i)
    {
        CAPTURE (i);
        CHECK (ons[i].e.note == song.notes[i].midiNote);
        CHECK (ons[i].e.velocity == 90);
        CHECK (std::abs (ons[i].time - (1.0 + song.notes[i].startSeconds)) <= 0.0011);
    }
    CHECK (run.sounding < 0); // ended after the last bar
    CHECK (run.player.getStops() == 1); // end of song
}

TEST_CASE ("Bar player: the live tempo is learned from the downbeats")
{
    const Song song (6);
    Run run (song);
    const double ratio = 1.1; // 10 % faster than the score: bars of 2 / 1.1 s
    std::vector<std::pair<double, int>> attacks;
    for (int b = 0; b < 6; ++b)
        attacks.push_back ({ 1.0 + 2.0 * b / ratio, 80 });
    run.play (attacks, 1.0 + 12.0 / ratio + 0.5);
    CHECK (run.player.getTempoRatio() == doctest::Approx (ratio).epsilon (0.03));
    const auto ons = run.noteOns();
    REQUIRE (ons.size() == song.notes.size());
    // From bar 5 (after four anchor spacings at tempoAdapt 0.5): fills within 15 ms of where the
    // player's tempo puts them. Bar 1 runs at the score tempo; bars 2-4 converge.
    for (std::size_t i = 20; i < ons.size(); ++i)
    {
        CAPTURE (i);
        const double ideal = 1.0 + song.notes[i].startSeconds / ratio;
        CHECK (std::abs (ons[i].time - ideal) < 0.015);
    }
}

TEST_CASE ("Bar player: no downbeat, no further bar; attacks inside a bar are ignored")
{
    const Song song (4);
    Run run (song);
    // Two bars, extra attacks inside bar 1 (player plays some notes himself), then silence.
    run.play ({ { 1.0, 90 }, { 1.6, 70 }, { 2.3, 60 }, { 3.0, 90 } }, 9.0);
    const auto ons = run.noteOns();
    CHECK (ons.size() == 10); // exactly two bars
    CHECK (ons.back().time < 5.0);
    CHECK (run.player.getStops() == 1);
    CHECK (run.player.getState() == BarPlayer::State::waiting);
    CHECK (run.player.getBar() == 2); // resumes at bar 3 with the next attack
    CHECK (run.sounding < 0);
}

TEST_CASE ("Bar player: contradicting roots fall back to Free; matching roots resume")
{
    const Song song (8);
    Run run (song);
    std::vector<std::pair<double, int>> attacks;
    for (int b = 0; b < 8; ++b)
        attacks.push_back ({ 1.0 + 2.0 * b, 90 });
    // Bars 1-2 played as written, bars 3-4 wrong roots, bars 5-8 right again (octave lower is fine).
    const int roots[] = { 28, 33, 38, 31 };
    std::vector<int> played;
    for (int b = 0; b < 8; ++b)
        played.push_back (b == 2 || b == 3 ? roots[b % 4] + 1 : roots[b % 4] - (b >= 4 ? 12 : 0));
    run.play (attacks, 18.0, played);
    CHECK (run.player.getFallbacks() == 1);
    CHECK (run.player.isTrusted());
    // Bar 4 (after the 2nd mismatch) and bar 5 (1st match) are silent; bars 1-3 and 6-8 play.
    int bar4or5 = 0, others = 0;
    for (const auto& e : run.noteOns())
        (e.time >= 7.05 && e.time < 10.95 ? bar4or5 : others)++;
    CHECK (bar4or5 <= 1);
    CHECK (others >= 25);
}

TEST_CASE ("Bar player: an empty bar passes on the clock; settings validation")
{
    const Song song (4, true);
    Run run (song);
    run.play ({ { 1.0, 90 }, { 3.0, 90 }, { 7.0, 90 } }, 9.5);
    CHECK (run.noteOns().size() == 15);
    CHECK (run.player.getStops() == 1); // only the end of the song

    BarPlayer::Settings s;
    s.earlyToleranceBeats = 0.0;
    CHECK_FALSE (s.isValid());
    s = {};
    s.rootMismatchesToFallback = 0;
    CHECK_FALSE (s.isValid());
}

TEST_CASE ("Bar player: after a pause the clock runs on; the player re-enters on a later downbeat")
{
    const Song song (10);
    Run run (song);
    // Bars 1-3 played, bars 4-5 left out (player pauses), re-entry on the downbeat of bar 6 (t = 11 s),
    // with a weak off-grid click at 9.4 s that must not start anything.
    run.play ({ { 1.0, 90 }, { 3.0, 90 }, { 5.0, 90 }, { 9.4, 50 }, { 11.0, 90 }, { 13.0, 90 } }, 15.5);
    const auto ons = run.noteOns();
    int bar6 = 0, during = 0;
    for (const auto& e : ons)
    {
        if (e.time >= 7.05 && e.time < 10.95)
            ++during;
        if (e.time >= 10.99 && e.time < 12.99)
            ++bar6;
    }
    CHECK (during == 0);
    CHECK (bar6 == 5);
    // Bar 6's notes are bar 6's (root 33 + fills of the second pattern).
    for (const auto& e : ons)
        if (e.time >= 10.99 && e.time < 11.01)
            CHECK (e.e.note == song.notes[25].midiNote);
}

TEST_CASE ("Bar player: follows a player who pauses and resumes off the clock, or plays slower")
{
    const Song song (10);
    {
        // Pause after bar 3, resume at an arbitrary time (9.3 s, off the clock's grid): the awaited
        // bar 4 starts there, and bar 5 follows two seconds later at the unchanged tempo.
        Run run (song);
        run.play ({ { 1.0, 90 }, { 3.0, 90 }, { 5.0, 90 }, { 9.3, 90 }, { 11.3, 90 } }, 13.5);
        int bar4 = 0, bar5 = 0;
        for (const auto& e : run.noteOns())
        {
            if (e.time >= 9.29 && e.time < 11.29)
                ++bar4;
            if (e.time >= 11.29 && e.time < 13.29)
                ++bar5;
            if (e.time >= 9.29 && e.time < 9.31)
                CHECK (e.e.note == song.notes[15].midiNote); // bar 4's root
        }
        CHECK (bar4 == 5);
        CHECK (bar5 == 5);
    }
    {
        // 20 % slower than the file from the start: every bar is played, the tempo is learned.
        Run run (song);
        std::vector<std::pair<double, int>> attacks;
        for (int b = 0; b < 8; ++b)
            attacks.push_back ({ 1.0 + 2.5 * b, 90 });
        run.play (attacks, 1.0 + 2.5 * 8);
        int started = 0;
        for (const auto& e : run.noteOns())
            for (const auto& a : attacks)
                if (std::abs (e.time - a.first) < 0.01)
                    ++started;
        CHECK (started == 8);
        CHECK (run.player.getTempoRatio() == doctest::Approx (0.8).epsilon (0.03));
    }
}
