// Bar playback evaluation: the player plays only each bar's anchor note (the root on the downbeat);
// Bass2MIDI plays the song's notes. How close do the played notes land to where the player's own
// tempo puts them, and what happens on tempo changes, pauses, wrong roots and extra notes?
//
//   bass2midi_barmode_eval [--out <dir>] [--song <file.gp|gp5> [--bars N] [--start-bar B]] [--rate Hz]
//
// Same BassToMidiProcessor as the app (Mode::bar). Ideal time of a written note = the performance's
// own tempo map applied to its score time (known exactly for these synthetic performances).

#include "SyntheticBass.h"

#include "bass2midi/BassToMidiProcessor.h"
#include "bass2midi/MidiNoteUtils.h"
#include "bass2midi/song/Song.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace bass2midi;

namespace
{
    struct Timeline
    {
        std::string name;
        std::vector<ExpectedNote> notes;
        std::vector<TimelineBar> bars;
    };

    // 16 bars of 4/4 at 140 bpm in the style of the owner's song: root on 1, fills on 2+, 3, 4, 4+.
    Timeline builtInTimeline()
    {
        Timeline t;
        t.name = "groove-140bpm";
        const double beat = 60.0 / 140.0;
        const int patterns[2][5] = { { 37, 41, 42, 43, 44 }, { 42, 46, 47, 48, 49 } };
        const double beats[5] = { 1.0, 2.5, 3.0, 4.0, 4.5 };
        for (int b = 0; b < 16; ++b)
        {
            TimelineBar bar;
            bar.writtenBar = b;
            bar.startSeconds = 4.0 * beat * b;
            bar.lengthSeconds = 4.0 * beat;
            bar.firstNote = static_cast<int> (t.notes.size());
            const auto& p = patterns[(b / 2) % 2];
            for (int i = 0; i < 5; ++i)
            {
                ExpectedNote n;
                n.midiNote = p[i];
                n.startSeconds = bar.startSeconds + (beats[i] - 1.0) * beat;
                n.durationSeconds = (i + 1 < 5 ? beats[i + 1] - beats[i] : 5.0 - beats[i]) * beat * 0.9;
                n.bar = b;
                n.beat = beats[i];
                n.playedBar = b;
                t.notes.push_back (n);
            }
            bar.noteCount = 5;
            t.bars.push_back (bar);
        }
        return t;
    }

    struct Random
    {
        std::uint32_t s;
        double uniform() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return static_cast<double> (s) / 4294967296.0; }
        double bipolar() { return 2.0 * uniform() - 1.0; }
    };

    struct Scenario
    {
        std::string name;
        double ratioStart = 1.0, ratioEnd = 1.0; // live tempo / score tempo over the song (linear in score time)
        double jitterMs = 10.0;
        std::vector<int> pausedBars;             // anchors not played (relative to the start bar)
        std::vector<int> wrongRootBars;          // anchors played 2 semitones off
        double extraNoteRate = 0.0;              // fraction of the written fills the player plays as well
    };

    // Live time of score time T (seconds after the start bar's start) for a linear tempo ramp.
    struct TempoMap
    {
        double r0, r1, span, lead;
        double live (double scoreT) const
        {
            // dt_live = dT / r(T), r(T) = r0 + (r1 - r0) T / span  ->  integral in closed form.
            const double k = (r1 - r0) / span;
            if (std::abs (k) < 1.0e-12)
                return lead + scoreT / r0;
            return lead + std::log ((r0 + k * scoreT) / r0) / k;
        }
    };

    struct PlayedNote { double time; int midi; };

    struct Result
    {
        std::string timeline, scenario;
        int expected = 0, matched = 0, extra = 0, wrongPitchNear = 0;
        std::vector<double> errorsMs;
        int stops = 0, fallbacks = 0, barsStarted = 0;
    };

    Result run (const Timeline& tl, int startBar, int barLimit, const Scenario& sc, double rate)
    {
        const int endBar = std::min (static_cast<int> (tl.bars.size()), startBar + barLimit);
        const double origin = tl.bars[static_cast<std::size_t> (startBar)].startSeconds;
        const double span = tl.bars[static_cast<std::size_t> (endBar - 1)].startSeconds + tl.bars[static_cast<std::size_t> (endBar - 1)].lengthSeconds - origin;
        const TempoMap tempo { sc.ratioStart, sc.ratioEnd, span, 0.5 };
        Random rnd { 4711 };

        const auto contains = [] (const std::vector<int>& v, int x) { return std::find (v.begin(), v.end(), x) != v.end(); };

        // The player's notes.
        std::vector<PlayedNote> played;
        for (int b = startBar; b < endBar; ++b)
        {
            const auto& bar = tl.bars[static_cast<std::size_t> (b)];
            if (bar.noteCount == 0 || contains (sc.pausedBars, b - startBar))
                continue;
            for (int i = 0; i < bar.noteCount; ++i)
            {
                const auto& n = tl.notes[static_cast<std::size_t> (bar.firstNote + i)];
                const bool anchor = i == 0;
                if (! anchor && ! (sc.extraNoteRate > 0.0 && rnd.uniform() < sc.extraNoteRate))
                    continue;
                const double t = tempo.live (n.startSeconds - origin) + sc.jitterMs * 1.0e-3 * rnd.bipolar();
                const int midiNote = anchor && contains (sc.wrongRootBars, b - startBar) ? n.midiNote + 2 : n.midiNote;
                played.push_back ({ t, midiNote });
            }
        }
        std::sort (played.begin(), played.end(), [] (const PlayedNote& a, const PlayedNote& b) { return a.time < b.time; });

        // Render: each played note rings until the next one (2 ms crossfade), the last for 1 s.
        const double total = (played.empty() ? 1.0 : played.back().time) + 1.5;
        std::vector<float> signal (static_cast<std::size_t> (total * rate), 0.0f);
        for (std::size_t i = 0; i < played.size(); ++i)
        {
            eval::PluckSpec spec;
            spec.fundamentalHz = midi::frequencyHzFromNote (played[i].midi);
            spec.sampleRateHz = rate;
            spec.onsetSeconds = 0.0;
            const double end = i + 1 < played.size() ? played[i + 1].time : played[i].time + 1.0;
            spec.durationSeconds = end - played[i].time + 0.003;
            spec.peakLinear = 0.3;
            spec.noiseFloorDbfs = -200.0;
            spec.seed = static_cast<std::uint32_t> (17 + i);
            const auto tone = eval::renderPluck (spec);
            const auto onset = static_cast<std::size_t> (played[i].time * rate);
            const auto fade = static_cast<std::size_t> (0.002 * rate);
            for (std::size_t k = 0; k < tone.size() && onset + k < signal.size(); ++k)
            {
                const std::size_t remaining = tone.size() - k;
                const double g = remaining < fade ? static_cast<double> (remaining) / static_cast<double> (fade) : 1.0;
                signal[onset + k] += static_cast<float> (g * tone[k]);
            }
        }
        Random noise { 3 };
        for (auto& s : signal)
            s += static_cast<float> (1.0e-4 * noise.bipolar());

        BassToMidiProcessor processor;
        if (! processor.prepare (rate, {}))
        {
            std::cerr << "prepare failed\n";
            std::exit (2);
        }
        processor.setTimeline (tl.notes.data(), static_cast<int> (tl.notes.size()), tl.bars.data(), static_cast<int> (tl.bars.size()));
        processor.setBarStart (startBar);
        processor.setMode (BassToMidiProcessor::Mode::bar);

        std::vector<PlayedNote> ons;
        constexpr int block = 64;
        for (std::size_t p = 0; p < signal.size(); p += block)
        {
            BassToMidiProcessor::Output out;
            const auto before = processor.getDiagnostics();
            processor.process (signal.data() + p, static_cast<int> (std::min<std::size_t> (block, signal.size() - p)), out);
            if (std::getenv ("BAR_TRACE") != nullptr && sc.name == std::getenv ("BAR_TRACE"))
            {
                const auto& d = processor.getDiagnostics();
                if (d.attackCount != before.attackCount)
                    std::fprintf (stderr, "%8.3f attack v%d  state %d bar %d next %.3f\n", static_cast<double> (d.lastAttackSample) / rate,
                                  d.lastAttackVelocity, d.barState, d.barPlayed, d.nextAnchorSeconds);
                if (d.barsStarted != before.barsStarted)
                    std::fprintf (stderr, "%8.3f   started bar %d\n", static_cast<double> (p) / rate, d.barPlayed);
                if (d.barStops != before.barStops)
                    std::fprintf (stderr, "%8.3f   STOP\n", static_cast<double> (p) / rate);
            }
            for (int i = 0; i < out.count; ++i)
                if (out.events[static_cast<std::size_t> (i)].event.type == NoteEvent::Type::noteOn)
                    ons.push_back ({ static_cast<double> (out.events[static_cast<std::size_t> (i)].sample) / rate,
                                     out.events[static_cast<std::size_t> (i)].event.note });
        }

        Result r;
        r.timeline = tl.name;
        r.scenario = sc.name;
        const auto& d = processor.getDiagnostics();
        r.stops = d.barStops;
        r.fallbacks = d.barFallbacks;
        r.barsStarted = d.barsStarted;

        // Expected: every written note of every bar whose anchor the player played (with the right root).
        std::vector<PlayedNote> ideal;
        for (int b = startBar; b < endBar; ++b)
        {
            const auto& bar = tl.bars[static_cast<std::size_t> (b)];
            if (contains (sc.pausedBars, b - startBar) || contains (sc.wrongRootBars, b - startBar))
                continue;
            for (int i = 0; i < bar.noteCount; ++i)
            {
                const auto& n = tl.notes[static_cast<std::size_t> (bar.firstNote + i)];
                ideal.push_back ({ tempo.live (n.startSeconds - origin), n.midiNote });
            }
        }
        r.expected = static_cast<int> (ideal.size());
        std::vector<bool> used (ons.size(), false);
        for (const auto& e : ideal)
        {
            int best = -1;
            double bestError = 0.06; // a note more than 60 ms off counts as missed
            for (std::size_t i = 0; i < ons.size(); ++i)
                if (! used[i] && ons[i].midi == e.midi && std::abs (ons[i].time - e.time) <= bestError)
                {
                    best = static_cast<int> (i);
                    bestError = std::abs (ons[i].time - e.time);
                }
            if (best >= 0)
            {
                used[static_cast<std::size_t> (best)] = true;
                ++r.matched;
                r.errorsMs.push_back (1000.0 * (ons[static_cast<std::size_t> (best)].time - e.time));
            }
        }
        for (std::size_t i = 0; i < ons.size(); ++i)
            if (! used[i])
                ++r.extra;
        return r;
    }

    double percentile (std::vector<double> v, double p)
    {
        if (v.empty())
            return NAN;
        std::sort (v.begin(), v.end());
        const auto rank = static_cast<std::size_t> (std::ceil (p / 100.0 * static_cast<double> (v.size())));
        return v[std::clamp<std::size_t> (rank, 1, v.size()) - 1];
    }

    std::string fmt (double v, int decimals = 1)
    {
        if (std::isnan (v))
            return "-";
        char b[32];
        std::snprintf (b, sizeof (b), "%.*f", decimals, v);
        return b;
    }
}

int main (int argc, char** argv)
{
    std::filesystem::path outDir = "barmode-results";
    std::string songPath;
    int barLimit = 16, startBar = 0;
    double rate = 48000.0;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc)
            outDir = argv[++i];
        else if (a == "--song" && i + 1 < argc)
            songPath = argv[++i];
        else if (a == "--bars" && i + 1 < argc)
            barLimit = std::atoi (argv[++i]);
        else if (a == "--start-bar" && i + 1 < argc)
            startBar = std::atoi (argv[++i]);
        else if (a == "--rate" && i + 1 < argc)
            rate = std::atof (argv[++i]);
        else
        {
            std::cerr << "usage: " << argv[0] << " [--out <dir>] [--song <file.gp|gp5> [--bars N] [--start-bar B]] [--rate Hz]\n";
            return 2;
        }
    }
    std::filesystem::create_directories (outDir);

    Timeline tl;
    if (songPath.empty())
        tl = builtInTimeline();
    else
    {
        song::Song s;
        std::string error;
        if (! song::loadSongFile (songPath, s, error))
        {
            std::cerr << error << "\n";
            return 1;
        }
        tl.name = std::filesystem::path (songPath).stem().string();
        tl.notes = song::expectedNotes (s, song::findBassTrack (s), tl.bars);
    }
    if (tl.bars.empty() || startBar >= static_cast<int> (tl.bars.size()))
    {
        std::cerr << "no bars\n";
        return 1;
    }

    const std::vector<Scenario> scenarios {
        { "score tempo, 10 ms jitter", 1.0, 1.0, 10.0, {}, {}, 0.0 },
        { "8 % faster than the file", 1.08, 1.08, 10.0, {}, {}, 0.0 },
        { "accelerando 1.00 -> 1.12", 1.0, 1.12, 10.0, {}, {}, 0.0 },
        { "pause in bars 6-7", 1.0, 1.0, 10.0, { 5, 6 }, {}, 0.0 },
        { "wrong roots in bars 9-10", 1.0, 1.0, 10.0, {}, { 8, 9 }, 0.0 },
        { "player plays half the fills too", 1.0, 1.0, 10.0, {}, {}, 0.5 },
        { "25 ms timing jitter", 1.0, 1.0, 25.0, {}, {}, 0.0 },
    };

    std::ostringstream md;
    md << "# Bar playback evaluation (synthetic)\n\n"
       << "Generated by `bass2midi_barmode_eval`, timeline `" << tl.name << "`, bars " << startBar + 1 << "-"
       << std::min (static_cast<int> (tl.bars.size()), startBar + barLimit) << ", " << fmt (rate / 1000.0) << " kHz, "
       << "64-sample blocks. The player plays only the anchor (first) note of each bar; expected = every written note of the "
       << "bars the player started with the right root, at the time the performance's own tempo map puts it. A note more "
       << "than 60 ms off counts as missed.\n\n"
       << "| scenario | expected | played on time (<= 60 ms) | timing error p50 / p95 / max ms | extra Note Ons | bars started | stops | fallbacks |\n"
       << "|---|---|---|---|---|---|---|---|\n";
    for (const auto& sc : scenarios)
    {
        std::cerr << sc.name << " ...\n";
        const auto r = run (tl, startBar, barLimit, sc, rate);
        std::vector<double> absErrors;
        for (const double e : r.errorsMs)
            absErrors.push_back (std::abs (e));
        md << "| " << r.scenario << " | " << r.expected << " | " << r.matched << " | " << fmt (percentile (absErrors, 50)) << " / "
           << fmt (percentile (absErrors, 95)) << " / " << fmt (absErrors.empty() ? NAN : *std::max_element (absErrors.begin(), absErrors.end()))
           << " | " << r.extra << " | " << r.barsStarted << " | " << r.stops << " | " << r.fallbacks << " |\n";
    }
    std::ofstream (outDir / "summary.md") << md.str();
    std::cout << md.str();
    return 0;
}
