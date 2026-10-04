// Offline evaluation: runs pitch-estimator + note-decision setups over a synthetic plucked-bass
// corpus (single plucks and two-note sequences) and reports per-case CSV plus a Markdown summary.
// No real-time constraints apply here (offline tool).
//
// Usage: bass2midi_baseline [--out <directory>] [--quick]

#include "ReferenceSetups.h"
#include "SyntheticBass.h"

#include "bass2midi/AnalysisFramer.h"
#include "bass2midi/MidiNoteUtils.h"
#include "bass2midi/MultiResolutionPitchTracker.h"
#include "bass2midi/NoteStateMachine.h"
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
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace bass2midi;
using namespace bass2midi::eval;

namespace
{
    struct Event
    {
        bool on = true;
        int note = 0;
        int velocity = 0;
        std::int64_t sample = 0; // decision time: end sample of the frame that triggered it
    };

    // A note-decision layer fed once per analysis frame.
    struct Decider
    {
        virtual ~Decider() = default;
        virtual void process (const PitchCandidate& c, double hopPeakLinear, std::int64_t frameEnd, std::vector<Event>& out) = 0;
        virtual void flush (std::int64_t endSample, std::vector<Event>& out) = 0;
    };

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

    class ReferenceDebouncer final : public Decider
    {
    public:
        void process (const PitchCandidate& c, double, std::int64_t frameEnd, std::vector<Event>& out) override
        {
            const bool voiced = c.valid && c.clarity >= rule.minClarity && c.windowRmsLinear >= rule.gateRmsLinear;

            if (! voiced)
            {
                candidate = -1;
                stableFrames = 0;
                if (sounding >= 0 && ++silentFrames >= rule.releaseFrames)
                {
                    out.push_back ({ false, sounding, 0, frameEnd });
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
                    out.push_back ({ false, sounding, 0, frameEnd });
                out.push_back ({ true, nearest, 100, frameEnd });
                sounding = nearest;
                candidate = -1;
                stableFrames = 0;
            }
        }

        void flush (std::int64_t endSample, std::vector<Event>& out) override
        {
            if (sounding >= 0)
                out.push_back ({ false, sounding, 0, endSample });
            sounding = -1;
        }

    private:
        DebounceRule rule;
        int sounding = -1, candidate = -1, stableFrames = 0, silentFrames = 0;
    };

    // The Bass2MIDI note state machine with default settings (and the case's song palette, if any).
    class StateMachineDecider final : public Decider
    {
    public:
        StateMachineDecider (const ReferenceSetup& analysis, double sampleRateHz, const NotePalette& palette)
        {
            machine.setPalette (palette);
            NoteStateMachine::Settings s;
            s.frameIntervalSeconds = analysis.hopSamples / sampleRateHz;
            s.onsetSettleMs = 0.75 * 1000.0 * analysis.yin.windowSamples / sampleRateHz;
            if (! machine.setSettings (s))
            {
                std::cerr << "invalid state machine settings\n";
                std::exit (2);
            }
        }

        void process (const PitchCandidate& c, double hopPeakLinear, std::int64_t frameEnd, std::vector<Event>& out) override
        {
            emit (machine.processFrame ({ c, hopPeakLinear }), frameEnd, out);
        }

        void flush (std::int64_t endSample, std::vector<Event>& out) override
        {
            emit (machine.allNotesOff(), endSample, out);
        }

    private:
        static void emit (const NoteStateMachine::Output& o, std::int64_t sample, std::vector<Event>& out)
        {
            for (int i = 0; i < o.count; ++i)
            {
                const auto& e = o.events[static_cast<size_t> (i)];
                out.push_back ({ e.type == NoteEvent::Type::noteOn, e.note, e.velocity, sample });
            }
        }

        NoteStateMachine machine;
    };

    // ---------------------------------------------------------------------------------------------
    struct EvalSetup
    {
        std::string name;
        std::function<ReferenceSetup (double)> analysis; // frame (window) and hop; YIN config for plain setups
        bool stateMachine = false;
        bool multiRes = false;                           // MultiResolutionPitchTracker instead of plain YIN
        // Song palette for the played notes of a case (Free mode if unset). Applied to the tracker and
        // the state machine alike.
        std::function<NotePalette (const std::vector<int>& playedNotes)> palette;

        NotePalette paletteFor (const std::vector<int>& playedNotes) const { return palette ? palette (playedNotes) : NotePalette {}; }
    };

    // Frame/hop plan for the multi-resolution tracker: frame = its longest rung, hop as full-range-yin.
    ReferenceSetup multiResolutionPlan (double sampleRateHz)
    {
        MultiResolutionPitchTracker tracker;
        if (! tracker.prepare (sampleRateHz, {}))
        {
            std::cerr << "invalid multi-resolution settings\n";
            std::exit (2);
        }
        ReferenceSetup plan = fullRangeYin (sampleRateHz);
        plan.name = "multi-resolution-yin";
        plan.yin.windowSamples = tracker.getFrameSamples();
        return plan;
    }

    std::unique_ptr<Decider> makeDecider (const EvalSetup& setup, const ReferenceSetup& analysis, double sampleRateHz,
                                          const NotePalette& palette)
    {
        if (setup.stateMachine)
            return std::make_unique<StateMachineDecider> (analysis, sampleRateHz, palette);
        return std::make_unique<ReferenceDebouncer>();
    }

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
        int firstVelocity = 0;
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

    // Runs one analysis setup over one signal: onFrame (candidate, hopPeakLinear, frameEnd).
    void runSignal (const ReferenceSetup& setup, bool multiRes, const NotePalette& palette, double sampleRateHz,
                    const std::vector<float>& signal, const std::function<void (const PitchCandidate&, double, std::int64_t)>& onFrame)
    {
        YinPitchEstimator estimator;
        MultiResolutionPitchTracker tracker;
        tracker.setPalette (palette);
        AnalysisFramer framer;
        const bool analysisReady = multiRes ? tracker.prepare (sampleRateHz, {}) && tracker.getFrameSamples() == setup.yin.windowSamples
                                            : estimator.prepare (sampleRateHz, setup.yin);
        if (! analysisReady || ! framer.prepare (setup.yin.windowSamples, setup.hopSamples))
        {
            std::cerr << "invalid setup " << setup.name << " at " << sampleRateHz << " Hz\n";
            std::exit (2);
        }

        const int window = setup.yin.windowSamples;
        const int hop = setup.hopSamples;

        // Feed in 64-sample blocks; frame timing is block-size independent (see AnalysisFramer).
        constexpr int blockSize = 64;
        for (size_t pos = 0; pos < signal.size(); pos += blockSize)
        {
            const int n = static_cast<int> (std::min<size_t> (blockSize, signal.size() - pos));
            framer.push (signal.data() + pos, n, [&] (const float* frame, std::int64_t end, int)
            {
                float hopPeak = 0.0f;
                for (int i = window - hop; i < window; ++i)
                    hopPeak = std::max (hopPeak, std::abs (frame[i]));
                onFrame (multiRes ? tracker.estimate (frame).pitch : estimator.estimate (frame), static_cast<double> (hopPeak), end);
            });
        }
    }

    CaseResult evaluatePluck (const EvalSetup& evalSetup, const PluckSpec& spec, int expectedNote,
                              const std::string& variant, const std::string& level)
    {
        const auto setup = evalSetup.analysis (spec.sampleRateHz);

        CaseResult r;
        r.setup = evalSetup.name;
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

        const auto palette = evalSetup.paletteFor ({ expectedNote });
        auto decider = makeDecider (evalSetup, setup, spec.sampleRateHz, palette);
        std::vector<Event> events;
        std::vector<double> centsErrors;

        runSignal (setup, evalSetup.multiRes, palette, spec.sampleRateHz, signal, [&] (const PitchCandidate& c, double hopPeak, std::int64_t end)
        {
            decider->process (c, hopPeak, end, events);

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
        decider->flush (static_cast<std::int64_t> (signal.size()), events);

        for (const auto& e : events)
        {
            if (! e.on)
                continue;

            if (r.noteOns++ == 0)
            {
                r.firstNote = e.note;
                r.firstVelocity = e.velocity;
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

    // ---------------------------------------------------------------------------------------------
    // Two-note sequences: a first pluck, then at secondOnsetSeconds either a re-pluck of the same
    // note or a different note. With gapMs > 0 the first note is muted gapMs before the second
    // onset; otherwise the second pluck replaces the first with a 2 ms crossfade (string re-excited
    // while ringing).
    struct SequenceSpec
    {
        std::string scenario;
        int firstNote = 0, secondNote = 0;
        double gapMs = 0.0;
    };

    struct SequenceResult
    {
        std::string setup, scenario, variant;
        double sampleRateHz = 0;
        int firstNote = 0, secondNote = 0;
        std::string emitted;            // emitted Note Ons, e.g. "A1 A1"
        bool pass = false;              // exactly the two expected Note Ons, in order
        double secondLatencyMs = NAN;   // second expected Note On - second onset
        int noteOns = 0, noteOffs = 0;
    };

    constexpr double sequenceFirstOnsetSeconds = 0.1;
    constexpr double sequenceSecondOnsetSeconds = 0.6;
    constexpr double sequenceDurationSeconds = 1.3;

    std::vector<float> renderSequence (const SequenceSpec& seq, double sampleRateHz, double fundamentalGainDb, double peakLinear)
    {
        const auto makeSpec = [&] (int note, double onsetSeconds, std::uint32_t seed)
        {
            PluckSpec spec;
            spec.fundamentalHz = midi::frequencyHzFromNote (note);
            spec.sampleRateHz = sampleRateHz;
            spec.durationSeconds = sequenceDurationSeconds;
            spec.onsetSeconds = onsetSeconds;
            spec.fundamentalGainDb = fundamentalGainDb;
            spec.peakLinear = peakLinear;
            spec.seed = seed;
            return spec;
        };

        const auto first = renderPluck (makeSpec (seq.firstNote, sequenceFirstOnsetSeconds, static_cast<std::uint32_t> (seq.firstNote * 31 + 5)));
        auto secondSpec = makeSpec (seq.secondNote, sequenceSecondOnsetSeconds, static_cast<std::uint32_t> (seq.secondNote * 37 + 11));
        secondSpec.noiseFloorDbfs = -200.0; // the first render already carries the noise floor
        const auto second = renderPluck (secondSpec);

        const auto secondOnset = static_cast<std::int64_t> (std::llround (sequenceSecondOnsetSeconds * sampleRateHz));
        const auto fadeSamples = std::max<std::int64_t> (1, std::llround (0.002 * sampleRateHz));
        const auto muteStart = secondOnset - static_cast<std::int64_t> (std::llround (seq.gapMs * 1.0e-3 * sampleRateHz)) - (seq.gapMs > 0.0 ? fadeSamples : 0);

        std::vector<float> out (first.size());
        for (size_t i = 0; i < out.size(); ++i)
        {
            const auto t = static_cast<std::int64_t> (i);
            double firstGain = 1.0;
            if (seq.gapMs > 0.0)
                firstGain = t < muteStart ? 1.0 : std::max (0.0, 1.0 - static_cast<double> (t - muteStart) / static_cast<double> (fadeSamples));
            else if (t >= secondOnset)
                firstGain = std::max (0.0, 1.0 - static_cast<double> (t - secondOnset) / static_cast<double> (fadeSamples));

            out[i] = static_cast<float> (firstGain * first[i] + second[i]);
        }
        return out;
    }

    SequenceResult evaluateSequence (const EvalSetup& evalSetup, const SequenceSpec& seq, double sampleRateHz, const Variant& variant)
    {
        const auto setup = evalSetup.analysis (sampleRateHz);
        SequenceResult r;
        r.setup = evalSetup.name;
        r.scenario = seq.scenario;
        r.variant = variant.name;
        r.sampleRateHz = sampleRateHz;
        r.firstNote = seq.firstNote;
        r.secondNote = seq.secondNote;

        const auto signal = renderSequence (seq, sampleRateHz, variant.fundamentalGainDb, 0.3);
        const auto palette = evalSetup.paletteFor ({ seq.firstNote, seq.secondNote });
        auto decider = makeDecider (evalSetup, setup, sampleRateHz, palette);
        std::vector<Event> events;

        runSignal (setup, evalSetup.multiRes, palette, sampleRateHz, signal, [&] (const PitchCandidate& c, double hopPeak, std::int64_t end)
        {
            decider->process (c, hopPeak, end, events);
        });
        decider->flush (static_cast<std::int64_t> (signal.size()), events);

        const auto secondOnset = static_cast<std::int64_t> (std::llround (sequenceSecondOnsetSeconds * sampleRateHz));
        std::vector<const Event*> ons;
        for (const auto& e : events)
        {
            if (e.on)
            {
                ons.push_back (&e);
                r.emitted += (r.emitted.empty() ? "" : " ") + midi::noteName (e.note);
            }
            else
                ++r.noteOffs;
        }
        r.noteOns = static_cast<int> (ons.size());
        if (r.emitted.empty())
            r.emitted = "none";

        r.pass = ons.size() == 2 && ons[0]->note == seq.firstNote && ons[1]->note == seq.secondNote
              && ons[0]->sample < secondOnset && ons[1]->sample >= secondOnset;
        if (r.pass)
            r.secondLatencyMs = toMs (ons[1]->sample - secondOnset, sampleRateHz);
        return r;
    }

    // ---------------------------------------------------------------------------------------------
    struct NoiseResult
    {
        std::string setup;
        double sampleRateHz = 0, levelDbfs = 0, durationSeconds = 0;
        int noteOns = 0, validFrames = 0, frames = 0;
    };

    NoiseResult evaluateNoise (const EvalSetup& evalSetup, double sampleRateHz, double levelDbfs, double durationSeconds)
    {
        const auto setup = evalSetup.analysis (sampleRateHz);
        NoiseResult r { evalSetup.name, sampleRateHz, levelDbfs, durationSeconds };
        const auto signal = renderNoise (sampleRateHz, durationSeconds, levelDbfs, 7);
        const auto palette = evalSetup.paletteFor ({ 28, 33, 38, 43 }); // open strings stand in for a song
        auto decider = makeDecider (evalSetup, setup, sampleRateHz, palette);
        std::vector<Event> events;

        runSignal (setup, evalSetup.multiRes, palette, sampleRateHz, signal, [&] (const PitchCandidate& c, double hopPeak, std::int64_t end)
        {
            ++r.frames;
            r.validFrames += c.valid ? 1 : 0;
            decider->process (c, hopPeak, end, events);
        });
        decider->flush (static_cast<std::int64_t> (signal.size()), events);

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
               "window_ms,hop_ms,note_ons,first_note,first_note_name,first_latency_ms,first_velocity,wrong_note_ons,"
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
                << fmt (r.firstLatencyMs, 2) << ',' << r.firstVelocity << ',' << r.wrongNoteOns << ',' << r.octaveNoteOns << ','
                << r.sustainFrames << ',' << r.correctFrames << ',' << r.octaveFrames << ','
                << r.otherFrames << ',' << r.invalidFrames << ',' << fmt (r.medianAbsCents, 2) << '\n';
        }
    }

    void writeSequenceCsv (const std::filesystem::path& path, const std::vector<SequenceResult>& results)
    {
        std::ofstream out (path);
        out << "setup,scenario,sample_rate_hz,variant,first_note,second_note,emitted_note_ons,note_ons,note_offs,pass,second_latency_ms\n";
        for (const auto& r : results)
            out << r.setup << ',' << r.scenario << ',' << r.sampleRateHz << ',' << r.variant << ','
                << midi::noteName (r.firstNote) << ',' << midi::noteName (r.secondNote) << ',' << r.emitted << ','
                << r.noteOns << ',' << r.noteOffs << ',' << (r.pass ? 1 : 0) << ',' << fmt (r.secondLatencyMs, 2) << '\n';
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

    void writeSummary (std::ostream& out, const std::vector<ReferenceSetup>& analysesAt48k,
                       const std::vector<CaseResult>& results, const std::vector<SequenceResult>& sequences,
                       const std::vector<NoiseResult>& noise)
    {
        out << "# Bass2MIDI evaluation (synthetic corpus)\n\n";
        out << "Generated by `bass2midi_baseline`. Latency = first Note On decision (end sample of the "
               "triggering analysis frame) minus the synthetic onset sample. It excludes audio-interface "
               "I/O latency and the host block delay (add up to one block, e.g. 64 samples = 1.3 ms at 48 kHz).\n\n";

        out << "Setups: `warf-like-yin` and `full-range-yin` use the reference debounce rule (Warf defaults); "
               "`full-range-yin+nsm` feeds `full-range-yin` into the Bass2MIDI `NoteStateMachine` with default settings; "
               "`multires-yin+nsm` uses the `MultiResolutionPitchTracker` (pitch-adaptive windows, sub-octave check) instead. "
               "With `--palette`, `free` is `multires-yin+nsm` and the `palette-*` setups add a song palette built around the "
               "played notes (`palette-in`: played notes plus a minor-pentatonic neighbourhood; `palette-off-*`: the played notes "
               "are missing - chromatic neighbours only, or the written part an octave above/below what is played). "
               "Noise runs use the four open strings as palette.\n\n";

        out << "Columns: cases | first Note On correct | missed (no Note On) | first Note On octave error | "
               "first Note On other wrong note | extra Note Ons beyond the first (duplicates/wrong) | "
               "latency of correct first Note Ons in ms: median, p95, max.\n\n";

        out << "## Analysis setups (at 48 kHz)\n\n| analysis | window | hop | lag range |\n|---|---|---|---|\n";
        for (const auto& s : analysesAt48k)
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

        table ("Single plucks by setup, sample rate and range", [] (const CaseResult& r)
        {
            return r.setup + " @ " + fmt (r.sampleRateHz / 1000.0) + " kHz, " + rangeGroup (r.expectedNote);
        });

        table ("Single plucks by setup, timbre variant and level (all sample rates)", [] (const CaseResult& r)
        {
            return r.setup + ", " + r.variant + ", " + r.level;
        });

        // Velocity: medium vs. soft plucks under the state machine.
        {
            std::map<std::string, std::vector<double>> velocities;
            for (const auto& r : results)
                if (r.firstNote == r.expectedNote && r.firstVelocity > 0 && r.setup.find ("nsm") != std::string::npos)
                    velocities[r.setup + ", " + r.level].push_back (r.firstVelocity);
            if (! velocities.empty())
            {
                out << "\n## Velocity of correct first Note Ons (state machine)\n\n| group | n | min | p50 | max |\n|---|---|---|---|---|\n";
                for (const auto& [name, v] : velocities)
                    out << "| " << name << " | " << v.size() << " | " << fmt (*std::min_element (v.begin(), v.end()), 0) << " | "
                        << fmt (percentile (v, 50), 0) << " | " << fmt (*std::max_element (v.begin(), v.end()), 0) << " |\n";
            }
        }

        // Two-note sequences.
        {
            struct SeqAggregate { int cases = 0, pass = 0; std::vector<double> latencies; };
            std::map<std::string, SeqAggregate> groups;
            for (const auto& r : sequences)
            {
                auto& g = groups[r.setup + ", " + r.scenario];
                ++g.cases;
                if (r.pass)
                {
                    ++g.pass;
                    g.latencies.push_back (r.secondLatencyMs);
                }
            }

            out << "\n## Two-note sequences\n\nPass = exactly the two expected Note Ons in order, the second after "
                   "the second onset. Latency = second Note On minus second onset.\n\n"
                   "| setup, scenario | cases | pass | p50 ms | p95 ms | max ms |\n|---|---|---|---|---|---|\n";
            for (const auto& [name, g] : groups)
                out << "| " << name << " | " << g.cases << " | " << g.pass << " | " << fmt (percentile (g.latencies, 50)) << " | "
                    << fmt (percentile (g.latencies, 95)) << " | "
                    << fmt (g.latencies.empty() ? NAN : *std::max_element (g.latencies.begin(), g.latencies.end())) << " |\n";
        }

        out << "\n## Sustain-region frame accuracy (raw estimator, all single-pluck cases)\n\n"
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

        out << "\n## Worst cases\n\n";
        int listed = 0;
        const auto list = [&] (const std::string& line)
        {
            if (listed++ < 60)
                out << "- " << line << "\n";
            else if (listed == 61)
                out << "- ... (see the CSV files for the full list)\n";
        };
        for (const auto& r : results)
            if (r.firstNote != r.expectedNote)
                list (r.setup + " @ " + fmt (r.sampleRateHz / 1000.0) + " kHz, " + r.variant + ", " + r.level + ": expected "
                      + midi::noteName (r.expectedNote) + ", got " + (r.firstNote >= 0 ? midi::noteName (r.firstNote) : std::string ("nothing")));
        for (const auto& r : sequences)
            if (! r.pass)
                list (r.setup + " @ " + fmt (r.sampleRateHz / 1000.0) + " kHz, " + r.variant + ", " + r.scenario + " "
                      + midi::noteName (r.firstNote) + "->" + midi::noteName (r.secondNote) + ": emitted " + r.emitted);
        if (listed == 0)
            out << "- none\n";
    }
}

int main (int argc, char** argv)
{
    std::filesystem::path outDir = "baseline-results";
    bool quick = false;
    bool paletteStudy = false; // score-guidance study: Free vs. song palettes instead of the reference setups

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc)
            outDir = argv[++i];
        else if (arg == "--quick")
            quick = true;
        else if (arg == "--palette")
            paletteStudy = true;
        else
        {
            std::cerr << "usage: " << argv[0] << " [--out <directory>] [--quick] [--palette]\n";
            return 2;
        }
    }

    std::filesystem::create_directories (outDir);

    const std::vector<double> sampleRates = quick ? std::vector<double> { 48000.0 }
                                                  : std::vector<double> { 44100.0, 48000.0, 96000.0 };
    const std::vector<Variant> variants { { "full-fundamental", 0.0 }, { "weak-fundamental", -18.0 } };
    const std::vector<Level> levels { { "medium", 0.3 }, { "soft", 0.03 } };
    const std::vector<EvalSetup> referenceSetups {
        { "warf-like-yin", warfLikeYin, false, false, {} },
        { "full-range-yin", fullRangeYin, false, false, {} },
        { "full-range-yin+nsm", fullRangeYin, true, false, {} },
        { "multires-yin+nsm", multiResolutionPlan, true, true, {} },
    };
    // Score guidance: every palette is built around the notes actually played. "in" contains them
    // (plus a minor-pentatonic neighbourhood); the others leave them out on purpose - a chromatic
    // passing tone, or the part played an octave below/above the written one (the hardest cases
    // for octave-error suppression, since the written note IS the octave error).
    const auto pal = [] (std::initializer_list<int> offsets, bool excludePlayed)
    {
        return [offsets = std::vector<int> (offsets), excludePlayed] (const std::vector<int>& played)
        {
            NotePalette p;
            for (const int n : played)
                for (const int o : offsets)
                    p.add (n + o);
            if (! excludePlayed)
                return p;
            NotePalette without;
            for (int n = 0; n < 128; ++n)
                if (p.contains (n) && std::find (played.begin(), played.end(), n) == played.end())
                    without.add (n);
            return without;
        };
    };
    const std::vector<EvalSetup> paletteSetups {
        { "free", multiResolutionPlan, true, true, {} },
        { "palette-in", multiResolutionPlan, true, true, pal ({ 0, 3, 5, 7, 10 }, false) },
        { "palette-off-chromatic", multiResolutionPlan, true, true, pal ({ 1, 3, 5, 8, 10 }, true) },
        { "palette-off-written-octave-up", multiResolutionPlan, true, true, pal ({ 12, 15, 17, 19, 22 }, true) },
        { "palette-off-written-octave-down", multiResolutionPlan, true, true, pal ({ -12, -9, -7, -5, -2 }, true) },
    };
    const auto& setups = paletteStudy ? paletteSetups : referenceSetups;

    std::vector<SequenceSpec> sequenceSpecs;
    for (const int note : { 28, 33, 38, 43, 48, 55 })
    {
        sequenceSpecs.push_back ({ "repeat-ringing", note, note, 0.0 });
        sequenceSpecs.push_back ({ "repeat-muted-30ms", note, note, 30.0 });
        sequenceSpecs.push_back ({ "change-fourth", note, note + 5, 0.0 });
        if (note + 12 <= midi::v1HighestNote)
            sequenceSpecs.push_back ({ "change-octave", note, note + 12, 0.0 });
    }

    std::vector<CaseResult> results;
    std::vector<SequenceResult> sequences;
    std::vector<NoiseResult> noise;

    for (const double rate : sampleRates)
    {
        for (const auto& setup : setups)
        {
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

            for (const auto& seq : sequenceSpecs)
                for (const auto& variant : variants)
                    sequences.push_back (evaluateSequence (setup, seq, rate, variant));

            for (const double noiseDbfs : { -50.0, -30.0 })
                noise.push_back (evaluateNoise (setup, rate, noiseDbfs, quick ? 2.0 : 10.0));
        }
    }

    writeCaseCsv (outDir / "cases.csv", results);
    writeSequenceCsv (outDir / "sequences.csv", sequences);
    writeNoiseCsv (outDir / "noise.csv", noise);

    const std::vector<ReferenceSetup> analyses { warfLikeYin (48000.0), fullRangeYin (48000.0) };
    std::ofstream summary (outDir / "summary.md");
    writeSummary (summary, analyses, results, sequences, noise);
    writeSummary (std::cout, analyses, results, sequences, noise);
    return 0;
}
