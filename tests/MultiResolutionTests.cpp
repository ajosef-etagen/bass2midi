#include <doctest/doctest.h>

#include "SyntheticBass.h"

#include "bass2midi/AnalysisFramer.h"
#include "bass2midi/MidiNoteUtils.h"
#include "bass2midi/MultiResolutionPitchTracker.h"

#include <cmath>
#include <cstdint>
#include <map>
#include <numbers>
#include <vector>

using namespace bass2midi;

namespace
{
    // Runs a pluck through framer + tracker; counts detected notes over all voiced frames after onset.
    struct Trace
    {
        std::map<int, int> notes;     // detected note -> frame count
        double firstCorrectMs = -1.0; // first frame reporting the expected note, relative to onset
    };

    Trace trace (int note, double fundamentalGainDb, double sampleRateHz, double noiseFloorDbfs = -90.0)
    {
        MultiResolutionPitchTracker tracker;
        REQUIRE (tracker.prepare (sampleRateHz, {}));
        AnalysisFramer framer;
        REQUIRE (framer.prepare (tracker.getFrameSamples(), static_cast<int> (std::lround (sampleRateHz * 0.0025))));

        eval::PluckSpec spec;
        spec.fundamentalHz = midi::frequencyHzFromNote (note);
        spec.sampleRateHz = sampleRateHz;
        spec.peakLinear = 0.3;
        spec.fundamentalGainDb = fundamentalGainDb;
        spec.seed = static_cast<std::uint32_t> (note * 131 + 17);
        spec.noiseFloorDbfs = noiseFloorDbfs;
        const auto signal = eval::renderPluck (spec);
        const auto onset = eval::onsetSample (spec);

        Trace t;
        framer.push (signal.data(), static_cast<int> (signal.size()), [&] (const float* frame, std::int64_t end, int)
        {
            const auto r = tracker.estimate (frame);
            if (end <= onset || ! r.pitch.valid)
                return;
            const int detected = static_cast<int> (std::lround (midi::noteFromFrequencyHz (r.pitch.frequencyHz)));
            ++t.notes[detected];
            if (detected == note && t.firstCorrectMs < 0.0)
                t.firstCorrectMs = 1000.0 * static_cast<double> (end - onset) / sampleRateHz;
        });
        return t;
    }
}

TEST_CASE ("Multi-resolution tracker: settings validation and rung layout")
{
    MultiResolutionPitchTracker::Settings s;
    CHECK (s.isValid());

    auto bad = s;
    bad.rungLowestHz = { 150.0, 160.0, 0, 0, 0, 0, 0, 0 }; // not decreasing
    CHECK_FALSE (bad.isValid());
    bad = s;
    bad.rungLowestHz = { 150.0, 0.0, 96.0, 0, 0, 0, 0, 0 }; // gap
    CHECK_FALSE (bad.isValid());
    bad = s;
    bad.rungLowestHz = { 30.0, 0, 0, 0, 0, 0, 0, 0 };       // below the range floor
    CHECK_FALSE (bad.isValid());
    bad = s;
    bad.edgeGuard = 0.6;
    CHECK_FALSE (bad.isValid());

    MultiResolutionPitchTracker tracker;
    CHECK_FALSE (tracker.prepare (0.0, s));
    REQUIRE (tracker.prepare (48000.0, s));
    CHECK (tracker.getRungCount() == 7); // 6 configured + the full-range rung
    for (int i = 1; i < tracker.getRungCount(); ++i)
        CHECK (tracker.getRungWindowSamples (i) > tracker.getRungWindowSamples (i - 1));
    CHECK (tracker.getFrameSamples() == tracker.getRungWindowSamples (tracker.getRungCount() - 1));
    CHECK (tracker.getFrameSamples() == 1264 + 632); // 38 Hz longest lag + half of it as integration
}

TEST_CASE ("Multi-resolution tracker: steady sines use a short rung for high notes, the long one for low")
{
    MultiResolutionPitchTracker tracker;
    REQUIRE (tracker.prepare (48000.0, {}));
    const int n = tracker.getFrameSamples();

    const auto run = [&] (int note)
    {
        std::vector<float> x (static_cast<size_t> (n));
        const double f0 = midi::frequencyHzFromNote (note);
        for (int i = 0; i < n; ++i)
            x[static_cast<size_t> (i)] = static_cast<float> (0.5 * std::sin (2.0 * std::numbers::pi * f0 * i / 48000.0));
        return tracker.estimate (x.data());
    };

    const auto g4 = run (67);
    REQUIRE (g4.pitch.valid);
    CHECK (std::abs (midi::centsBetween (g4.pitch.frequencyHz, midi::frequencyHzFromNote (67))) < 5.0);
    CHECK (g4.rung == 0);
    CHECK (g4.windowSamples < 600); // ~10 ms instead of ~40 ms

    const auto e1 = run (28);
    REQUIRE (e1.pitch.valid);
    CHECK (std::abs (midi::centsBetween (e1.pitch.frequencyHz, midi::frequencyHzFromNote (28))) < 5.0);
    CHECK (e1.rung == tracker.getRungCount() - 1);
}

TEST_CASE ("Multi-resolution tracker: no wrong-note frames on synthetic plucks, including weak fundamentals")
{
    // Regression for two found failure modes: edge-clipped minima (A#1 read as B1 by the 60 Hz rung)
    // and low-clarity frames of short rungs on weak-fundamental tones (fixed by earlyMinClarity 0.93).
    for (const double rate : { 44100.0, 48000.0 })
        for (const double fundamentalGainDb : { 0.0, -18.0 })
            for (const int note : { 28, 31, 34, 36, 38, 40, 43, 48, 55, 62, 67 })
            {
                CAPTURE (rate);
                CAPTURE (fundamentalGainDb);
                CAPTURE (note);
                const auto t = trace (note, fundamentalGainDb, rate);

                int wrong = 0;
                for (const auto& [detected, frames] : t.notes)
                    if (detected != note)
                        wrong += frames;

                // At most a single isolated frame (the state machine needs >= 2 consistent frames).
                CHECK (wrong <= 1);
                CHECK (t.firstCorrectMs > 0.0);
            }
}

TEST_CASE ("Multi-resolution tracker: high notes are found much earlier than with the full window")
{
    const auto g3 = trace (55, 0.0, 48000.0);
    const auto e1 = trace (28, 0.0, 48000.0);
    REQUIRE (g3.firstCorrectMs > 0.0);
    REQUIRE (e1.firstCorrectMs > 0.0);
    CHECK (g3.firstCorrectMs < 25.0);
    CHECK (e1.firstCorrectMs < 45.0);
}

TEST_CASE ("Multi-resolution tracker: no phantom notes when the window straddles digital silence")
{
    // Regression: windows that start in exact silence and hold only a few ms of a new attack compared
    // silence with silence and reported random notes with clarity ~1 (seen as a wrong Note On when
    // re-plucking after a mute). The silence-edge check rejects such windows.
    for (const double rate : { 44100.0, 48000.0, 96000.0 })
        for (const double fundamentalGainDb : { 0.0, -18.0 })
            for (const int note : { 28, 33, 38, 43, 48, 55 })
            {
                CAPTURE (rate);
                CAPTURE (fundamentalGainDb);
                CAPTURE (note);
                const auto t = trace (note, fundamentalGainDb, rate, -200.0);

                int wrong = 0;
                for (const auto& [detected, frames] : t.notes)
                    if (detected != note)
                        wrong += frames;
                CHECK (wrong <= 1);
            }
}
