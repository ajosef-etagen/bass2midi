// Song Mode evaluation: does an attack-triggered expected note beat full pitch detection, and what
// does it cost when the player deviates from the score?
//
//   bass2midi_songmode_eval [--out <dir>] [--song <file.gp|gp5> [--seconds N]] [--rate Hz] [--skip-margin X]
//
// Renders synthetic bass performances of a timeline (built-in riffs, or a Guitar Pro bass track),
// runs the same BassToMidiProcessor as the app in Free and in Song mode on the same signal, and
// scores every played note: first Note On correct?, latency from the true onset, wrong Note Ons,
// missed notes, corrections. Performances: as written (with timing jitter), faster tempo, pitch
// variations, ghost-note clicks, skipped notes, and a wrong start position.

#include "SyntheticBass.h"

#include "bass2midi/BassToMidiProcessor.h"
#include "bass2midi/MidiNoteUtils.h"
#include "bass2midi/song/Song.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace bass2midi;

namespace
{
    double skipMarginSetting = SongFollower::Settings {}.skipMargin; // --skip-margin

    struct Riff
    {
        std::string name;
        double bpm;
        double stepQuarters;          // note spacing
        std::vector<int> notes;       // one bar pattern, -1 = rest
        int bars;
    };

    std::vector<ExpectedNote> timelineFromRiff (const Riff& r)
    {
        std::vector<ExpectedNote> out;
        const double step = r.stepQuarters * 60.0 / r.bpm;
        int slot = 0;
        for (int b = 0; b < r.bars; ++b)
            for (std::size_t i = 0; i < r.notes.size(); ++i, ++slot)
            {
                if (r.notes[i] < 0)
                    continue;
                ExpectedNote n;
                n.midiNote = r.notes[i] + ((b / 2) % 2 == 1 ? 5 : 0); // every other pair of bars a fourth up
                n.startSeconds = slot * step;
                n.durationSeconds = step;
                n.bar = b;
                n.beat = 1.0 + static_cast<double> (i) * r.stepQuarters;
                out.push_back (n);
            }
        return out;
    }

    // Deterministic xorshift.
    struct Random
    {
        std::uint32_t s;
        double uniform() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return static_cast<double> (s) / 4294967296.0; }
        double bipolar() { return 2.0 * uniform() - 1.0; }
    };

    struct PlayedNote
    {
        double onsetSeconds = 0.0;
        int midiNote = 0;
        bool ghost = false;    // dead-note click: no pitch, should produce no Note On
    };

    struct Performance
    {
        std::string name;
        std::vector<PlayedNote> notes;
        int startPosition = 0; // follower start index (wrong start = misplaced)
    };

    Performance perform (const std::string& name, const std::vector<ExpectedNote>& timeline, std::uint32_t seed,
                         double tempoRatio, double jitterMs, double variationRate, double ghostRate, double skipRate,
                         int startOffset)
    {
        Performance p;
        p.name = name;
        p.startPosition = startOffset;
        Random rnd { seed };
        const double lead = 0.3; // silence before the first note
        for (std::size_t i = 0; i < timeline.size(); ++i)
        {
            const auto& n = timeline[i];
            const double t = lead + n.startSeconds / tempoRatio + jitterMs * 1.0e-3 * rnd.bipolar();
            if (skipRate > 0.0 && rnd.uniform() < skipRate && i > 0)
                continue;
            int note = n.midiNote;
            if (variationRate > 0.0 && rnd.uniform() < variationRate)
            {
                const int offsets[] = { -2, -1, 1, 2, 3 };
                note = std::clamp (note + offsets[static_cast<int> (rnd.uniform() * 5.0)], 28, 67);
            }
            p.notes.push_back ({ t, note, false });
            if (ghostRate > 0.0 && rnd.uniform() < ghostRate && i + 1 < timeline.size())
            {
                const double next = lead + timeline[i + 1].startSeconds / tempoRatio;
                p.notes.push_back ({ t + 0.5 * (next - t), -1, true });
            }
        }
        return p;
    }

    // A ghost note mutes the string (the previous note stops) and clicks.
    std::vector<float> render (const Performance& p, double rate)
    {
        const double total = (p.notes.empty() ? 1.0 : p.notes.back().onsetSeconds) + 1.0;
        std::vector<float> out (static_cast<std::size_t> (total * rate), 0.0f);
        Random noise { 99 };
        for (std::size_t i = 0; i < p.notes.size(); ++i)
        {
            const auto& n = p.notes[i];
            const auto onset = static_cast<std::int64_t> (n.onsetSeconds * rate);
            const double end = i + 1 < p.notes.size() ? p.notes[i + 1].onsetSeconds : n.onsetSeconds + 0.8;
            const auto endSample = static_cast<std::int64_t> (end * rate);
            const auto fade = static_cast<std::int64_t> (0.002 * rate);
            if (n.ghost)
            {
                for (std::int64_t k = 0; k < static_cast<std::int64_t> (0.004 * rate) && onset + k < (std::int64_t) out.size(); ++k)
                    out[static_cast<std::size_t> (onset + k)] += static_cast<float> (0.03 * noise.bipolar() * (1.0 - static_cast<double> (k) / (0.004 * rate)));
                continue;
            }
            eval::PluckSpec spec;
            spec.fundamentalHz = midi::frequencyHzFromNote (n.midiNote);
            spec.sampleRateHz = rate;
            spec.onsetSeconds = 0.0;
            spec.durationSeconds = std::max (0.05, end - n.onsetSeconds) + 0.003;
            spec.peakLinear = 0.25 + 0.05 * static_cast<double> (i % 3);
            spec.noiseFloorDbfs = -200.0;
            spec.seed = static_cast<std::uint32_t> (1000 + i * 17);
            const auto tone = eval::renderPluck (spec);
            for (std::size_t k = 0; k < tone.size(); ++k)
            {
                const auto t = onset + static_cast<std::int64_t> (k);
                if (t >= static_cast<std::int64_t> (out.size()))
                    break;
                // The next pluck (or mute) stops this note with a short fade.
                double g = 1.0;
                if (t >= endSample)
                    g = std::max (0.0, 1.0 - static_cast<double> (t - endSample) / static_cast<double> (fade));
                if (g <= 0.0)
                    break;
                out[static_cast<std::size_t> (t)] += static_cast<float> (g * tone[k]);
            }
        }
        // Noise floor.
        for (auto& s : out)
            s += static_cast<float> (1.0e-4 * noise.bipolar());
        return out;
    }

    struct NoteResult
    {
        double onsetSeconds = 0.0;
        int played = 0;
        int firstNoteOn = -1;      // first Note On in the note's slot
        double latencyMs = NAN;    // of the first CORRECT Note On
        bool firstCorrect = false;
        bool predicted = false;    // the first Note On was a Song Mode prediction
        int wrongNoteOns = 0;
    };

    struct RunResult
    {
        std::string timeline, performance, mode;
        std::vector<NoteResult> notes;
        int ghostTriggers = 0;     // Note Ons in ghost-click slots
        int corrections = 0, resyncs = 0, predictions = 0;
        double finalConfidence = 0.0, tempoRatio = 1.0;
    };

    RunResult run (const std::string& timelineName, const std::vector<ExpectedNote>& timeline, const Performance& perf,
                   bool songMode, bool periodicity, double rate)
    {
        BassToMidiProcessor processor;
        BassToMidiProcessor::Settings settings;
        settings.attacks.usePeriodicity = periodicity;
        settings.follower.skipMargin = skipMarginSetting;
        if (! processor.prepare (rate, settings))
        {
            std::cerr << "prepare failed\n";
            std::exit (2);
        }
        processor.setTimeline (timeline.data(), static_cast<int> (timeline.size()));
        processor.setSongPosition (perf.startPosition);
        processor.setMode (songMode ? BassToMidiProcessor::Mode::song : BassToMidiProcessor::Mode::free);

        const auto signal = render (perf, rate);
        struct On { std::int64_t sample; int note; bool predicted; };
        std::vector<On> ons;
        constexpr int block = 64;
        for (std::size_t p = 0; p < signal.size(); p += block)
        {
            BassToMidiProcessor::Output out;
            processor.process (signal.data() + p, static_cast<int> (std::min<std::size_t> (block, signal.size() - p)), out);
            for (int i = 0; i < out.count; ++i)
            {
                const auto& e = out.events[static_cast<std::size_t> (i)];
                if (e.event.type == NoteEvent::Type::noteOn)
                    ons.push_back ({ e.sample, e.event.note, e.predicted });
            }
        }

        RunResult r { timelineName, perf.name, std::string (songMode ? "song" : "free") + (periodicity ? "+periodicity" : ""), {}, 0, 0, 0, 0, 0.0, 1.0 };
        const auto& d = processor.getDiagnostics();
        r.corrections = d.corrections;
        r.resyncs = d.resyncs;
        r.predictions = d.predictions;
        r.finalConfidence = d.confidence;
        r.tempoRatio = d.tempoRatio;

        // Slot of a played note: from 10 ms before its onset to 10 ms before the next onset.
        for (std::size_t i = 0; i < perf.notes.size(); ++i)
        {
            const auto& n = perf.notes[i];
            const auto from = static_cast<std::int64_t> ((n.onsetSeconds - 0.01) * rate);
            const auto to = i + 1 < perf.notes.size() ? static_cast<std::int64_t> ((perf.notes[i + 1].onsetSeconds - 0.01) * rate)
                                                      : static_cast<std::int64_t> (signal.size());
            if (n.ghost)
            {
                for (const auto& on : ons)
                    if (on.sample >= from && on.sample < to)
                        ++r.ghostTriggers;
                continue;
            }
            NoteResult nr;
            nr.onsetSeconds = n.onsetSeconds;
            nr.played = n.midiNote;
            for (const auto& on : ons)
            {
                if (on.sample < from || on.sample >= to)
                    continue;
                if (nr.firstNoteOn < 0)
                {
                    nr.firstNoteOn = on.note;
                    nr.firstCorrect = on.note == n.midiNote;
                    nr.predicted = on.predicted;
                }
                if (on.note == n.midiNote)
                {
                    if (std::isnan (nr.latencyMs))
                        nr.latencyMs = 1000.0 * (static_cast<double> (on.sample) / rate - n.onsetSeconds); // NOLINT
                }
                else
                    ++nr.wrongNoteOns;
            }
            r.notes.push_back (nr);
        }
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
    std::filesystem::path outDir = "songmode-results";
    std::string songPath;
    double songSeconds = 40.0;
    double rate = 48000.0;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc)
            outDir = argv[++i];
        else if (a == "--song" && i + 1 < argc)
            songPath = argv[++i];
        else if (a == "--seconds" && i + 1 < argc)
            songSeconds = std::atof (argv[++i]);
        else if (a == "--rate" && i + 1 < argc)
            rate = std::atof (argv[++i]);
        else if (a == "--skip-margin" && i + 1 < argc)
            skipMarginSetting = std::atof (argv[++i]);
        else
        {
            std::cerr << "usage: " << argv[0] << " [--out <dir>] [--song <file.gp|gp5> [--seconds N]] [--rate Hz] [--skip-margin X]\n";
            return 2;
        }
    }
    std::filesystem::create_directories (outDir);

    std::vector<std::pair<std::string, std::vector<ExpectedNote>>> timelines;
    if (! songPath.empty())
    {
        song::Song s;
        std::string error;
        if (! song::loadSongFile (songPath, s, error))
        {
            std::cerr << error << "\n";
            return 1;
        }
        auto t = song::expectedNotes (s, song::findBassTrack (s));
        // Shift to start at 0 and keep the first songSeconds.
        if (! t.empty())
        {
            const double first = t.front().startSeconds;
            for (auto& n : t)
                n.startSeconds -= first;
            t.erase (std::remove_if (t.begin(), t.end(), [&] (const ExpectedNote& n) { return n.startSeconds > songSeconds; }), t.end());
        }
        timelines.push_back ({ std::filesystem::path (songPath).stem().string(), t });
    }
    else
    {
        const std::vector<Riff> riffs {
            { "octave-pump-eighths-120", 120.0, 0.5, { 28, 28, 40, 28, 31, 31, 43, 31 }, 8 },
            { "walking-quarters-110", 110.0, 1.0, { 33, 37, 40, 42 }, 8 },
            { "repeated-sixteenths-100", 100.0, 0.25, { 38, 38, 38, 38, 41, 41, 43, 45, 38, 38, 38, 38, 36, 36, 33, 35 }, 4 },
            { "upper-melody-eighths-130", 130.0, 0.5, { 43, 47, 50, 55, 53, 50, -1, 48 }, 8 },
        };
        for (const auto& r : riffs)
            timelines.push_back ({ r.name, timelineFromRiff (r) });
    }

    struct PerfSpec { std::string name; double tempo, jitterMs, variation, ghost, skip; int startOffset; };
    const std::vector<PerfSpec> perfs {
        { "as-written", 1.0, 15.0, 0.0, 0.0, 0.0, 0 },
        { "tempo+8%", 1.08, 15.0, 0.0, 0.0, 0.0, 0 },
        { "variations-10%", 1.0, 15.0, 0.10, 0.0, 0.0, 0 },
        { "ghost-clicks-10%", 1.0, 15.0, 0.0, 0.10, 0.0, 0 },
        { "skipped-5%", 1.0, 15.0, 0.0, 0.0, 0.05, 0 },
        { "wrong-start+4", 1.0, 15.0, 0.0, 0.0, 0.0, 4 },
        { "tempo-20%", 0.8, 15.0, 0.0, 0.0, 0.0, 0 },
    };

    std::vector<RunResult> results;
    for (const auto& [name, timeline] : timelines)
        for (std::size_t pi = 0; pi < perfs.size(); ++pi)
        {
            const auto& ps = perfs[pi];
            const auto perf = perform (ps.name, timeline, static_cast<std::uint32_t> (17 + pi * 101), ps.tempo, ps.jitterMs,
                                       ps.variation, ps.ghost, ps.skip, ps.startOffset);
            std::cerr << name << " / " << ps.name << " ...\n";
            results.push_back (run (name, timeline, perf, false, false, rate));
            results.push_back (run (name, timeline, perf, true, false, rate));
            results.push_back (run (name, timeline, perf, true, true, rate));
        }

    // Per-note CSV.
    {
        std::ofstream csv (outDir / "notes.csv");
        csv << "timeline,performance,mode,onset_s,played,first_note_on,first_correct,first_predicted,latency_ms,wrong_note_ons\n";
        for (const auto& r : results)
            for (const auto& n : r.notes)
                csv << r.timeline << ',' << r.performance << ',' << r.mode << ',' << fmt (n.onsetSeconds, 3) << ','
                    << midi::noteName (n.played) << ',' << (n.firstNoteOn >= 0 ? midi::noteName (n.firstNoteOn) : std::string ("none")) << ','
                    << (n.firstCorrect ? 1 : 0) << ',' << (n.predicted ? 1 : 0) << ',' << fmt (n.latencyMs, 2) << ',' << n.wrongNoteOns << '\n';
    }

    std::ostringstream md;
    md << "# Song Mode evaluation (synthetic performances)\n\n"
       << "Generated by `bass2midi_songmode_eval` at " << fmt (rate / 1000.0) << " kHz, 64-sample blocks (events are stamped at "
       << "the block end, so latencies include up to 1.3 ms of block quantisation). Same signal for both modes; "
       << "default settings of `BassToMidiProcessor`.\n\n"
       << "Columns: notes played | first Note On correct | first Note On wrong | missed (no correct Note On in the note's slot) | "
       << "wrong Note Ons in total (incl. corrected ones) | Note Ons on ghost clicks | latency of the first correct Note On "
       << "(p50 / p95 / max ms) | predictions | corrections | re-alignments | final confidence / tempo ratio.\n\n"
       << "| timeline | performance | mode | notes | first correct | first wrong | missed | wrong ons | ghost ons | p50 | p95 | max | pred | corr | resync | conf / tempo |\n"
       << "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    struct Total { int notes = 0, correct = 0, wrong = 0, missed = 0, wrongOns = 0, ghost = 0; std::vector<double> lat; };
    std::map<std::string, Total> totals;
    for (const auto& r : results)
    {
        Total t;
        for (const auto& n : r.notes)
        {
            ++t.notes;
            if (n.firstNoteOn >= 0)
                (n.firstCorrect ? t.correct : t.wrong)++;
            if (std::isnan (n.latencyMs))
                ++t.missed;
            else
                t.lat.push_back (n.latencyMs);
            t.wrongOns += n.wrongNoteOns;
        }
        t.ghost = r.ghostTriggers;
        md << "| " << r.timeline << " | " << r.performance << " | " << r.mode << " | " << t.notes << " | " << t.correct << " | " << t.wrong
           << " | " << t.missed << " | " << t.wrongOns << " | " << t.ghost << " | " << fmt (percentile (t.lat, 50)) << " | "
           << fmt (percentile (t.lat, 95)) << " | " << fmt (t.lat.empty() ? NAN : *std::max_element (t.lat.begin(), t.lat.end())) << " | "
           << r.predictions << " | " << r.corrections << " | " << r.resyncs << " | " << fmt (r.finalConfidence, 2) << " / "
           << fmt (r.tempoRatio, 2) << " |\n";

        auto& all = totals[r.performance + ", " + r.mode];
        all.notes += t.notes;
        all.correct += t.correct;
        all.wrong += t.wrong;
        all.missed += t.missed;
        all.wrongOns += t.wrongOns;
        all.ghost += t.ghost;
        all.lat.insert (all.lat.end(), t.lat.begin(), t.lat.end());
    }
    md << "\n## Totals by performance and mode\n\n| performance, mode | notes | first correct | first wrong | missed | wrong ons | ghost ons | p50 | p95 | max |\n"
       << "|---|---|---|---|---|---|---|---|---|---|\n";
    for (const auto& [k, t] : totals)
        md << "| " << k << " | " << t.notes << " | " << t.correct << " | " << t.wrong << " | " << t.missed << " | " << t.wrongOns
           << " | " << t.ghost << " | " << fmt (percentile (t.lat, 50)) << " | " << fmt (percentile (t.lat, 95)) << " | "
           << fmt (t.lat.empty() ? NAN : *std::max_element (t.lat.begin(), t.lat.end())) << " |\n";

    std::ofstream (outDir / "summary.md") << md.str();
    std::cout << md.str();
    return 0;
}
