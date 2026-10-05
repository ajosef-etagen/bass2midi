#include "bass2midi/song/Song.h"

#include <algorithm>
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
        return out;
    }
}
