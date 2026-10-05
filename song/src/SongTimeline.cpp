#include "bass2midi/song/Song.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace bass2midi::song
{
    std::vector<int> playingOrder (const Song& song, int maxBars)
    {
        std::vector<int> order;
        const int n = static_cast<int> (song.masterBars.size());
        int start = 0; // first bar of the current repeated section
        int pass = 1;  // 1-based pass through it

        for (int i = 0; i < n && static_cast<int> (order.size()) < maxBars;)
        {
            const auto& bar = song.masterBars[static_cast<std::size_t> (i)];
            if (bar.repeatStart && i != start)
            {
                start = i;
                pass = 1;
            }
            if (bar.alternateEndings != 0 && (pass > 32 || (bar.alternateEndings & (1u << (pass - 1))) == 0))
            {
                ++i; // an ending for another pass
                continue;
            }
            order.push_back (i);
            if (bar.repeatPlays > 0)
            {
                if (pass < bar.repeatPlays)
                {
                    ++pass;
                    i = start;
                    continue;
                }
                pass = 1;
                start = i + 1;
            }
            ++i;
        }
        return order;
    }

    namespace
    {
        // Seconds between two written positions under the song's tempo map (bpm in quarters).
        struct TempoMap
        {
            const std::vector<TempoChange>& tempos;

            double bpmAt (double quarters) const
            {
                double bpm = 120.0;
                for (const auto& t : tempos)
                    if (t.atQuarters <= quarters + 1.0e-9 && t.bpm > 0.0)
                        bpm = t.bpm;
                return bpm;
            }

            double seconds (double fromQuarters, double toQuarters) const
            {
                double total = 0.0, position = fromQuarters;
                while (position < toQuarters - 1.0e-12)
                {
                    double next = toQuarters;
                    for (const auto& t : tempos)
                        if (t.atQuarters > position + 1.0e-9 && t.atQuarters < next)
                            next = t.atQuarters;
                    total += (next - position) * 60.0 / bpmAt (position);
                    position = next;
                }
                return total;
            }
        };
    }

    void assignFingering (std::vector<ExpectedNote>& notes, int maxFret)
    {
        static constexpr int tuning[4] = { 28, 33, 38, 43 };
        const auto candidates = [&] (const ExpectedNote& n, int* strings, int* frets)
        {
            if (n.string >= 0 && n.fret >= 0) // given (from the file): the only option
            {
                strings[0] = n.string;
                frets[0] = n.fret;
                return 1;
            }
            int c = 0;
            for (int s = 0; s < 4; ++s)
            {
                const int f = n.midiNote - tuning[s];
                if (f >= 0 && f <= maxFret)
                {
                    strings[c] = s;
                    frets[c] = f;
                    ++c;
                }
            }
            return c;
        };
        const auto unary = [] (int fret) { return 0.02 * fret + (fret > 12 ? 0.1 * (fret - 12) : 0.0); };
        const auto transition = [] (int s0, int f0, int s1, int f1)
        {
            if (f0 == 0 || f1 == 0)
                return 0.1 * std::abs (s1 - s0); // open strings need no position
            return static_cast<double> (std::abs (f1 - f0)) + 0.3 * std::abs (s1 - s0);
        };

        // Viterbi over runs of playable notes (an unplayable note breaks the chain).
        const std::size_t n = notes.size();
        std::vector<std::array<double, 4>> cost (n);
        std::vector<std::array<int, 4>> back (n);
        std::vector<std::array<int, 4>> strs (n), frs (n);
        std::vector<int> counts (n);
        for (std::size_t i = 0; i < n; ++i)
        {
            counts[i] = candidates (notes[i], strs[i].data(), frs[i].data());
            for (int c = 0; c < counts[i]; ++c)
            {
                cost[i][static_cast<std::size_t> (c)] = unary (frs[i][static_cast<std::size_t> (c)]);
                back[i][static_cast<std::size_t> (c)] = -1;
                if (i > 0 && counts[i - 1] > 0)
                {
                    double best = 1.0e18;
                    for (int p = 0; p < counts[i - 1]; ++p)
                    {
                        const double v = cost[i - 1][static_cast<std::size_t> (p)]
                                       + transition (strs[i - 1][static_cast<std::size_t> (p)], frs[i - 1][static_cast<std::size_t> (p)],
                                                     strs[i][static_cast<std::size_t> (c)], frs[i][static_cast<std::size_t> (c)]);
                        if (v < best)
                        {
                            best = v;
                            back[i][static_cast<std::size_t> (c)] = p;
                        }
                    }
                    cost[i][static_cast<std::size_t> (c)] += best;
                }
            }
        }
        // Backtrack each run from its end.
        for (std::size_t end = n; end-- > 0;)
        {
            if (counts[end] == 0)
            {
                notes[end].string = notes[end].fret = -1;
                continue;
            }
            if (end + 1 < n && counts[end + 1] > 0)
                continue; // not the end of a run
            int c = 0;
            for (int k = 1; k < counts[end]; ++k)
                if (cost[end][static_cast<std::size_t> (k)] < cost[end][static_cast<std::size_t> (c)])
                    c = k;
            for (std::size_t i = end + 1; i-- > 0 && c >= 0;)
            {
                notes[i].string = strs[i][static_cast<std::size_t> (c)];
                notes[i].fret = frs[i][static_cast<std::size_t> (c)];
                c = back[i][static_cast<std::size_t> (c)];
                if (i == 0 || counts[i - 1] == 0)
                    break;
            }
        }
    }

    std::vector<ExpectedNote> expectedNotes (const Song& song, int trackIndex)
    {
        std::vector<TimelineBar> bars;
        return expectedNotes (song, trackIndex, bars);
    }

    std::vector<ExpectedNote> expectedNotes (const Song& song, int trackIndex, std::vector<TimelineBar>& bars)
    {
        std::vector<ExpectedNote> out;
        bars.clear();
        if (trackIndex < 0 || trackIndex >= static_cast<int> (song.tracks.size()) || song.masterBars.empty())
            return out;
        const auto& track = song.tracks[static_cast<std::size_t> (trackIndex)];
        const TempoMap tempo { song.tempos };
        const bool standardTuning = track.tuning == std::vector<int> { 28, 33, 38, 43 };

        // Notes grouped by written bar (they are sorted by start).
        std::vector<std::vector<const Note*>> byBar (song.masterBars.size());
        for (const auto& note : track.notes)
            if (note.bar >= 0 && note.bar < static_cast<int> (byBar.size()))
                byBar[static_cast<std::size_t> (note.bar)].push_back (&note);

        double barSeconds = 0.0;
        for (const int barIndex : playingOrder (song))
        {
            const auto& bar = song.masterBars[static_cast<std::size_t> (barIndex)];
            const int playedBar = static_cast<int> (bars.size());
            TimelineBar timelineBar;
            timelineBar.writtenBar = barIndex;
            timelineBar.startSeconds = barSeconds;
            timelineBar.lengthSeconds = tempo.seconds (bar.startQuarters, bar.startQuarters + bar.lengthQuarters);
            timelineBar.numerator = bar.numerator;
            bars.push_back (timelineBar);
            for (const auto* note : byBar[static_cast<std::size_t> (barIndex)])
            {
                const double offsetQuarters = note->startQuarters - bar.startQuarters;
                const double start = barSeconds + tempo.seconds (bar.startQuarters, note->startQuarters);
                const double duration = tempo.seconds (note->startQuarters, note->startQuarters + note->durationQuarters);

                if (note->tieContinuation)
                {
                    // Lengthen the note it continues (the latest one with this pitch).
                    for (auto it = out.rbegin(); it != out.rend(); ++it)
                        if (it->midiNote == note->midiNote)
                        {
                            it->durationSeconds = std::max (it->durationSeconds, start + duration - it->startSeconds);
                            break;
                        }
                    continue;
                }
                if (note->durationQuarters <= 0.0)
                    continue; // grace note

                ExpectedNote expected;
                expected.midiNote = note->midiNote;
                if (standardTuning && note->string >= 0 && note->fret >= 0)
                {
                    expected.string = note->string;
                    expected.fret = note->fret;
                }
                {
                    // Anchor weight from metric position and length (beats in units of the denominator).
                    const double beat0 = offsetQuarters * bar.denominator / 4.0; // 0-based
                    const double lengthBeats = note->durationQuarters * bar.denominator / 4.0;
                    const auto near = [] (double v) { return std::abs (v - std::round (v)) < 1.0e-6; };
                    double w = 0.2;
                    if (std::abs (beat0) < 1.0e-6)
                        w = 1.0;
                    else if (bar.numerator == 4 && std::abs (beat0 - 2.0) < 1.0e-6)
                        w = 0.7;
                    else if (near (beat0))
                        w = 0.5;
                    else if (near (beat0 * 2.0))
                        w = 0.3;
                    if (lengthBeats >= 1.0 - 1.0e-6)
                        w += 0.15;
                    else if (lengthBeats < 0.5 - 1.0e-6)
                        w -= 0.1;
                    expected.anchorWeight = std::clamp (w, 0.1, 1.0);
                }
                expected.startSeconds = start;
                expected.durationSeconds = duration;
                expected.bar = barIndex;
                expected.beat = 1.0 + offsetQuarters * bar.denominator / 4.0;
                expected.playedBar = playedBar;

                if (! out.empty() && std::abs (out.back().startSeconds - start) < 1.0e-6)
                {
                    if (expected.midiNote < out.back().midiNote) // simultaneous: keep the lowest
                        out.back() = expected;
                    continue;
                }
                out.push_back (expected);
            }
            barSeconds += bars.back().lengthSeconds;
        }

        std::stable_sort (out.begin(), out.end(),
                          [] (const ExpectedNote& a, const ExpectedNote& b) { return a.startSeconds < b.startSeconds; });
        for (std::size_t i = out.size(); i-- > 0;) // notes are in bar order: first index = lowest
        {
            auto& b = bars[static_cast<std::size_t> (out[i].playedBar)];
            b.firstNote = static_cast<int> (i);
            ++b.noteCount;
        }
        for (auto& b : bars)
            if (b.noteCount == 0)
                b.firstNote = 0;

        // The bar's root (heuristic: the pitch class of its first note) recurring later in the bar.
        for (const auto& b : bars)
            for (int i = b.firstNote + 1; i < b.firstNote + b.noteCount; ++i)
                if ((out[static_cast<std::size_t> (i)].midiNote - out[static_cast<std::size_t> (b.firstNote)].midiNote) % 12 == 0)
                    out[static_cast<std::size_t> (i)].anchorWeight = std::min (1.0, out[static_cast<std::size_t> (i)].anchorWeight + 0.1);

        assignFingering (out);
        return out;
    }
}
