// Phase 0 baseline: runs the reference YIN setups over a synthetic plucked-bass corpus and reports
// per-case CSV plus a Markdown summary. No real-time constraints apply here (offline tool).
//
// Usage: bass2midi_baseline [--out <directory>] [--quick]

#include "ReferenceSetups.h"
#include "SyntheticBass.h"

#include "bass2midi/AnalysisFramer.h"
#include "bass2midi/MidiNoteUtils.h"
#include "bass2midi/YinPitchEstimator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace bass2midi;
using namespace bass2midi::eval;

namespace
{
    // ---------------------------------------------------------------------------------------------
    // Reference note decision: a plain debounce rule with the parameters Warf uses at its default
    // Sensitivity of 0.5. It is a measurement reference only, not the Bass2MIDI state machine.
    struct DebounceRule
    {
        double minClarity = 0.85;        // reject frames below this YIN clarity
        double gateRmsLinear = 0.01;     // -40 dBFS window RMS gate
        double toleranceCents = 35.0;    // max distance from the nearest semitone to count a frame
        int attackFrames = 2;            // consecutive frames on one note before Note On
        int releaseFrames = 4;           // consecutive unvoiced frames before Note Off
    };

    struct NoteEvent
    {
        bool on = true;
        int note = 0;
        std::int64_t sample = 0; // decision time: end sample of the frame that triggered it
    };

    class ReferenceDebouncer
    {
    public:
        explicit ReferenceDebouncer (DebounceRule r) : rule (r) {}

        void process (const PitchCandidate& c, std::int64_t frameEnd, std::vector<NoteEvent>& out)
        {
            const bool voiced = c.valid && c.clarity >= rule.minClarity && c.windowRmsLinear >= rule.gateRmsLinear;

            if (! voiced)
            {
                candidate = -1;
                stableFrames = 0;
                if (sounding >= 0 && ++silentFrames >= rule.releaseFrames)
                {
                    out.push_back ({ false, sounding, frameEnd });
                    sounding = -1;
                    silentFrames = 0;
                }
                return;
            }

            silentFrames = 0;
            const double exact = midi::noteFromFrequencyHz (c.frequencyHz);
            const int nearest = static_cast<int> (std::lround (exact));

            if (nearest == sounding)
            {
                candidate = -1;
                stableFrames = 0;
                return;
            }

            if (std::abs (exact - nearest) * 100.0 > rule.toleranceCents)
            {
                stableFrames = 0;
                return;
            }

            if (nearest == candidate)
                ++stableFrames;
            else
            {
                candidate = nearest;
                stableFrames = 1;
            }

            if (stableFrames >= rule.attackFrames)
            {
                if (sounding >= 0)
                    out.push_back ({ false, sounding, frameEnd });
                out.push_back ({ true, nearest, frameEnd });
                sounding = nearest;
                candidate = -1;
                stableFrames = 0;
            }
        }

        void flush (std::int64_t endSample, std::vector<NoteEvent>& out)
        {
            if (sounding >= 0)
                out.push_back ({ false, sounding, endSample });
            sounding = -1;
        }

    private:
        DebounceRule rule;
        int sounding = -1, candidate = -1, stableFrames = 0, silentFrames = 0;
    };

    // ---------------------------------------------------------------------------------------------
    struct Variant
    {
        std::string name;
        double fundamentalGainDb;
    };

    struct Level
    {
        std::string name;
        double peakLinear;
    };

    struct CaseResult
    {
        std::string setup, variant, level;
        double sampleRateHz = 0;
        int expectedNote = 0;
        int windowSamples = 0, hopSamples = 0;

        int noteOns = 0;
        int firstNote = -1;              // -1: no Note On at all (missed)
        double firstLatencyMs = NAN;     // first Note On decision time - onset
        int wrongNoteOns = 0;            // Note Ons with a note != expected
        int octaveNoteOns = 0;           // subset of wrongNoteOns that are whole octaves off

        // Raw estimator frames in the sustain region (onset + window ... onset + 0.6 s).
        int sustainFrames = 0, correctFrames = 0, octaveFrames = 0, otherFrames = 0, invalidFrames = 0;
        double medianAbsCents = NAN;     // over correct frames
    };

    double percentile (std::vector<double> values, double p)
    {
        if (values.empty())
            return NAN;
        std::sort (values.begin(), values.end());
        // Nearest-rank percentile.
        const auto rank = static_cast<size_t> (std::ceil (p / 100.0 * static_cast<double> (values.size())));
        return values[std::clamp<size_t> (rank, 1, values.size()) - 1];
    }

    bool isOctaveError (int detected, int expected)
    {
        const int diff = detected - expected;
        return diff != 0 && diff % 12 == 0;
    }

    double toMs (std::int64_t samples, double sampleRateHz)
    {
        return 1000.0 * static_cast<double> (samples) / sampleRateHz;
    }

    // Runs one setup over one signal; collects frames and reference note events.
    void runSignal (const ReferenceSetup& setup, double sampleRateHz, const std::vector<float>& signal,
                    const std::function<void (const PitchCandidate&, std::int64_t)>& onFrame)
    {
        YinPitchEstimator estimator;
        AnalysisFramer framer;
        if (! estimator.prepare (sampleRateHz, setup.yin) || ! framer.prepare (setup.yin.windowSamples, setup.hopSamples))
        {
            std::cerr << "invalid setup " << setup.name << " at " << sampleRateHz << " Hz\n";
            std::exit (2);
        }

        // Feed in 64-sample blocks; frame timing is block-size independent (see AnalysisFramer).
        constexpr int blockSize = 64;
        for (size_t pos = 0; pos < signal.size(); pos += blockSize)
        {
            const int n = static_cast<int> (std::min<size_t> (blockSize, signal.size() - pos));
            framer.push (signal.data() + pos, n, [&] (const float* window, std::int64_t end, int)
            {
                onFrame (estimator.estimate (window), end);
            });
        }
    }

    CaseResult evaluatePluck (const ReferenceSetup& setup, const PluckSpec& spec, int expectedNote,
                              const std::string& variant, const std::string& level)
    {
        CaseResult r;
        r.setup = setup.name;
        r.variant = variant;
        r.level = level;
        r.sampleRateHz = spec.sampleRateHz;
        r.expectedNote = expectedNote;
        r.windowSamples = setup.yin.windowSamples;
        r.hopSamples = setup.hopSamples;

        const auto signal = renderPluck (spec);
        const auto onset = onsetSample (spec);
        const auto sustainStart = onset + setup.yin.windowSamples;
        const auto sustainEnd = onset + static_cast<std::int64_t> (0.6 * spec.sampleRateHz);

        ReferenceDebouncer debouncer { DebounceRule {} };
        std::vector<NoteEvent> events;
        std::vector<double> centsErrors;

        runSignal (setup, spec.sampleRateHz, signal, [&] (const PitchCandidate& c, std::int64_t end)
        {
            debouncer.process (c, end, events);

            if (end < sustainStart || end > sustainEnd)
                return;

            ++r.sustainFrames;
            if (! c.valid)
            {
                ++r.invalidFrames;
                return;
            }

            const int detected = static_cast<int> (std::lround (midi::noteFromFrequencyHz (c.frequencyHz)));
            if (detected == expectedNote)
            {
                ++r.correctFrames;
                centsErrors.push_back (std::abs (midi::centsBetween (c.frequencyHz, spec.fundamentalHz)));
            }
            else if (isOctaveError (detected, expectedNote))
                ++r.octaveFrames;
            else
                ++r.otherFrames;
        });
        debouncer.flush (static_cast<std::int64_t> (signal.size()), events);

        for (const auto& e : events)
        {
            if (! e.on)
                continue;

            if (r.noteOns++ == 0)
            {
                r.firstNote = e.note;
                r.firstLatencyMs = toMs (e.sample - onset, spec.sampleRateHz);
            }

            if (e.note != expectedNote)
            {
                ++r.wrongNoteOns;
                if (isOctaveError (e.note, expectedNote))
                    ++r.octaveNoteOns;
            }
        }

        r.medianAbsCents = percentile (centsErrors, 50.0);
        return r;
    }

    struct NoiseResult
    {
        std::string setup;
        double sampleRateHz = 0, levelDbfs = 0, durationSeconds = 0;
        int noteOns = 0, validFrames = 0, frames = 0;
    };

    NoiseResult evaluateNoise (const ReferenceSetup& setup, double sampleRateHz, double levelDbfs, double durationSeconds)
    {
        NoiseResult r { setup.name, sampleRateHz, levelDbfs, durationSeconds };
        const auto signal = renderNoise (sampleRateHz, durationSeconds, levelDbfs, 7);
        ReferenceDebouncer debouncer { DebounceRule {} };
        std::vector<NoteEvent> events;

        runSignal (setup, sampleRateHz, signal, [&] (const PitchCandidate& c, std::int64_t end)
        {
            ++r.frames;
            r.validFrames += c.valid ? 1 : 0;
            debouncer.process (c, end, events);
        });

        for (const auto& e : events)
            r.noteOns += e.on ? 1 : 0;
        return r;
    }

    std::string rangeGroup (int note)
    {
        if (note < midi::aOpenNote)
            return "E1-G#1 (low E only)";
        if (note < midi::dOpenNote)
            return "A1-C#2 (A string)";
        return "D2-G4 (D/G strings)";
    }

    std::string fmt (double v, int decimals = 1)
    {
        if (std::isnan (v))
            return "-";
        char buffer[32];
        std::snprintf (buffer, sizeof (buffer), "%.*f", decimals, v);
        return buffer;
    }

    void writeCaseCsv (const std::filesystem::path& path, const std::vector<CaseResult>& results)
    {
        std::ofstream out (path);
        out << "setup,sample_rate_hz,variant,level,expected_note,expected_name,window_samples,hop_samples,"
               "window_ms,hop_ms,note_ons,first_note,first_note_name,first_latency_ms,wrong_note_ons,"
               "octave_note_ons,sustain_frames,correct_frames,octave_frames,other_frames,invalid_frames,"
               "median_abs_cents\n";

        for (const auto& r : results)
        {
            out << r.setup << ',' << r.sampleRateHz << ',' << r.variant << ',' << r.level << ','
                << r.expectedNote << ',' << midi::noteName (r.expectedNote) << ','
                << r.windowSamples << ',' << r.hopSamples << ','
                << fmt (toMs (r.windowSamples, r.sampleRateHz), 2) << ',' << fmt (toMs (r.hopSamples, r.sampleRateHz), 2) << ','
                << r.noteOns << ',' << r.firstNote << ','
                << (r.firstNote >= 0 ? midi::noteName (r.firstNote) : std::string ("none")) << ','
                << fmt (r.firstLatencyMs, 2) << ',' << r.wrongNoteOns << ',' << r.octaveNoteOns << ','
                << r.sustainFrames << ',' << r.correctFrames << ',' << r.octaveFrames << ','
                << r.otherFrames << ',' << r.invalidFrames << ',' << fmt (r.medianAbsCents, 2) << '\n';
        }
    }

    void writeNoiseCsv (const std::filesystem::path& path, const std::vector<NoiseResult>& results)
    {
        std::ofstream out (path);
        out << "setup,sample_rate_hz,noise_dbfs,duration_s,frames,valid_frames,note_ons\n";
        for (const auto& r : results)
            out << r.setup << ',' << r.sampleRateHz << ',' << r.levelDbfs << ',' << r.durationSeconds << ','
                << r.frames << ',' << r.validFrames << ',' << r.noteOns << '\n';
    }

    struct Aggregate
    {
        int cases = 0, missed = 0, correctFirst = 0, octaveFirst = 0, otherFirst = 0, extraNoteOns = 0;
        std::vector<double> latencies; // first Note On latency, correct first notes only
    };

    void add (Aggregate& a, const CaseResult& r)
    {
        ++a.cases;
        if (r.firstNote < 0)
            ++a.missed;
        else if (r.firstNote == r.expectedNote)
        {
            ++a.correctFirst;
            a.latencies.push_back (r.firstLatencyMs);
        }
        else if (isOctaveError (r.firstNote, r.expectedNote))
            ++a.octaveFirst;
        else
            ++a.otherFirst;

        a.extraNoteOns += std::max (0, r.noteOns - 1);
    }

    std::string aggregateRow (const Aggregate& a)
    {
        std::ostringstream row;
        row << a.cases << " | " << a.correctFirst << " | " << a.missed << " | " << a.octaveFirst << " | "
            << a.otherFirst << " | " << a.extraNoteOns << " | "
            << fmt (percentile (a.latencies, 50)) << " | " << fmt (percentile (a.latencies, 95)) << " | "
            << fmt (a.latencies.empty() ? NAN : *std::max_element (a.latencies.begin(), a.latencies.end())) << " |";
        return row.str();
    }

    void writeSummary (std::ostream& out, const std::vector<ReferenceSetup>& setupsAt48k,
                       const std::vector<CaseResult>& results, const std::vector<NoiseResult>& noise)
    {
        out << "# Bass2MIDI Phase 0 baseline (synthetic corpus)\n\n";
        out << "Generated by `bass2midi_baseline`. Latency = first Note On decision (end sample of the "
               "triggering analysis frame) minus the synthetic onset sample. It excludes audio-interface "
               "I/O latency and the host block delay (add up to one block, e.g. 64 samples = 1.3 ms at 48 kHz).\n\n";

        out << "Columns: cases | first Note On correct | missed (no Note On) | first Note On octave error | "
               "first Note On other wrong note | extra Note Ons beyond the first (duplicates/wrong) | "
               "latency of correct first Note Ons in ms: median, p95, max.\n\n";

        out << "## Setups (at 48 kHz)\n\n| setup | window | hop | lag range |\n|---|---|---|---|\n";
        for (const auto& s : setupsAt48k)
            out << "| " << s.name << " | " << s.yin.windowSamples << " smp (" << fmt (toMs (s.yin.windowSamples, 48000.0)) << " ms) | "
                << s.hopSamples << " smp (" << fmt (toMs (s.hopSamples, 48000.0), 2) << " ms) | "
                << fmt (48000.0 / s.yin.maxLagSamples) << "-" << fmt (48000.0 / s.yin.minLagSamples, 0) << " Hz |\n";

        const auto table = [&] (const std::string& title, const std::function<std::string (const CaseResult&)>& key)
        {
            std::map<std::string, Aggregate> groups;
            for (const auto& r : results)
                add (groups[key (r)], r);

            out << "\n## " << title << "\n\n| group | cases | correct | missed | octave | other | extra | p50 ms | p95 ms | max ms |\n"
                   "|---|---|---|---|---|---|---|---|---|---|\n";
            for (const auto& [name, aggregate] : groups)
                out << "| " << name << " | " << aggregateRow (aggregate) << "\n";
        };

        table ("By setup, sample rate and range", [] (const CaseResult& r)
        {
            return r.setup + " @ " + fmt (r.sampleRateHz / 1000.0) + " kHz, " + rangeGroup (r.expectedNote);
        });

        table ("By setup, timbre variant and level (all sample rates)", [] (const CaseResult& r)
        {
            return r.setup + ", " + r.variant + ", " + r.level;
        });

        out << "\n## Sustain-region frame accuracy (raw estimator, all cases)\n\n"
               "| setup | frames | correct % | octave % | other % | invalid % | median abs cents |\n|---|---|---|---|---|---|---|\n";
        std::map<std::string, std::vector<const CaseResult*>> bySetup;
        for (const auto& r : results)
            bySetup[r.setup].push_back (&r);
        for (const auto& [name, rs] : bySetup)
        {
            double frames = 0, correct = 0, octave = 0, other = 0, invalid = 0;
            std::vector<double> cents;
            for (const auto* r : rs)
            {
                frames += r->sustainFrames;
                correct += r->correctFrames;
                octave += r->octaveFrames;
                other += r->otherFrames;
                invalid += r->invalidFrames;
                if (! std::isnan (r->medianAbsCents))
                    cents.push_back (r->medianAbsCents);
            }
            const auto pct = [&] (double v) { return fmt (frames > 0 ? 100.0 * v / frames : NAN); };
            out << "| " << name << " | " << static_cast<long> (frames) << " | " << pct (correct) << " | " << pct (octave)
                << " | " << pct (other) << " | " << pct (invalid) << " | " << fmt (percentile (cents, 50), 2) << " |\n";
        }

        out << "\n## Noise-only input (false triggers)\n\n| setup | rate | noise dBFS | seconds | valid frames | Note Ons |\n|---|---|---|---|---|---|\n";
        for (const auto& n : noise)
            out << "| " << n.setup << " | " << fmt (n.sampleRateHz / 1000.0) << " kHz | " << fmt (n.levelDbfs, 0) << " | "
                << fmt (n.durationSeconds, 0) << " | " << n.validFrames << "/" << n.frames << " | " << n.noteOns << " |\n";

        out << "\n## Worst cases (missed or wrong first Note On)\n\n";
        int listed = 0;
        for (const auto& r : results)
        {
            if (r.firstNote == r.expectedNote)
                continue;
            if (listed++ >= 40)
            {
                out << "- ... (see cases.csv for the full list)\n";
                break;
            }
            out << "- " << r.setup << " @ " << fmt (r.sampleRateHz / 1000.0) << " kHz, " << r.variant << ", " << r.level
                << ": expected " << midi::noteName (r.expectedNote) << ", got "
                << (r.firstNote >= 0 ? midi::noteName (r.firstNote) : std::string ("nothing")) << "\n";
        }
        if (listed == 0)
            out << "- none\n";
    }
}

int main (int argc, char** argv)
{
    std::filesystem::path outDir = "baseline-results";
    bool quick = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc)
            outDir = argv[++i];
        else if (arg == "--quick")
            quick = true;
        else
        {
            std::cerr << "usage: " << argv[0] << " [--out <directory>] [--quick]\n";
            return 2;
        }
    }

    std::filesystem::create_directories (outDir);

    const std::vector<double> sampleRates = quick ? std::vector<double> { 48000.0 }
                                                  : std::vector<double> { 44100.0, 48000.0, 96000.0 };
    const std::vector<Variant> variants { { "full-fundamental", 0.0 }, { "weak-fundamental", -18.0 } };
    const std::vector<Level> levels { { "medium", 0.3 }, { "soft", 0.03 } };
    const std::vector<std::function<ReferenceSetup (double)>> setupFactories { warfLikeYin, fullRangeYin };

    std::vector<CaseResult> results;
    std::vector<NoiseResult> noise;

    for (const double rate : sampleRates)
    {
        for (const auto& makeSetup : setupFactories)
        {
            const auto setup = makeSetup (rate);
            std::cerr << setup.name << " @ " << rate << " Hz ...\n";

            for (int note = midi::lowEOpenNote; note <= midi::v1HighestNote; note += quick ? 3 : 1)
                for (const auto& variant : variants)
                    for (const auto& level : levels)
                    {
                        PluckSpec spec;
                        spec.fundamentalHz = midi::frequencyHzFromNote (note);
                        spec.sampleRateHz = rate;
                        spec.fundamentalGainDb = variant.fundamentalGainDb;
                        spec.peakLinear = level.peakLinear;
                        spec.seed = static_cast<std::uint32_t> (note * 131 + 17);
                        results.push_back (evaluatePluck (setup, spec, note, variant.name, level.name));
                    }

            for (const double noiseDbfs : { -50.0, -30.0 })
                noise.push_back (evaluateNoise (setup, rate, noiseDbfs, quick ? 2.0 : 10.0));
        }
    }

    writeCaseCsv (outDir / "cases.csv", results);
    writeNoiseCsv (outDir / "noise.csv", noise);

    std::ofstream summary (outDir / "summary.md");
    writeSummary (summary, { warfLikeYin (48000.0), fullRangeYin (48000.0) }, results, noise);
    writeSummary (std::cout, { warfLikeYin (48000.0), fullRangeYin (48000.0) }, results, noise);
    return 0;
}
