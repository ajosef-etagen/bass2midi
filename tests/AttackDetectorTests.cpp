#include <doctest/doctest.h>

#include "SyntheticBass.h"

#include "bass2midi/AttackDetector.h"
#include "bass2midi/MidiNoteUtils.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace bass2midi;

namespace
{
    std::vector<AttackDetector::Attack> detect (const std::vector<float>& signal, double rate, int blockSize = 64,
                                                const AttackDetector::Settings& s = {}, double periodSamples = 0.0)
    {
        AttackDetector d;
        REQUIRE (d.prepare (rate, s));
        d.setPeriodSamples (periodSamples);
        std::vector<AttackDetector::Attack> out;
        for (std::size_t p = 0; p < signal.size(); p += static_cast<std::size_t> (blockSize))
        {
            AttackDetector::Attack a;
            const int n = static_cast<int> (std::min<std::size_t> (static_cast<std::size_t> (blockSize), signal.size() - p));
            if (d.process (signal.data() + p, n, a))
                out.push_back (a);
        }
        return out;
    }

    eval::PluckSpec pluck (int note, double rate, double onsetSeconds = 0.1, double durationSeconds = 1.0)
    {
        eval::PluckSpec spec;
        spec.fundamentalHz = midi::frequencyHzFromNote (note);
        spec.sampleRateHz = rate;
        spec.onsetSeconds = onsetSeconds;
        spec.durationSeconds = durationSeconds;
        spec.peakLinear = 0.3;
        spec.seed = static_cast<std::uint32_t> (note * 7 + 3);
        return spec;
    }

    double ms (std::int64_t samples, double rate) { return 1000.0 * static_cast<double> (samples) / rate; }
}

TEST_CASE ("Attack detector: settings validation")
{
    AttackDetector d;
    AttackDetector::Settings s;
    CHECK (s.isValid());
    CHECK (d.prepare (48000.0, s));
    auto bad = s;
    bad.velocityWindowMs = 50.0;
    CHECK_FALSE (bad.isValid());
    bad = s;
    bad.lookbackMs = 500.0; // exceeds the ring
    CHECK_FALSE (d.prepare (48000.0, bad));
    CHECK_FALSE (d.prepare (1000.0, s)); // high-pass above Nyquist range
    CHECK_FALSE (d.prepare (0.0, s));
}

TEST_CASE ("Attack detector: one attack per pluck, within a few milliseconds, on every string")
{
    for (const double rate : { 44100.0, 48000.0, 96000.0 })
        for (const int note : { 28, 31, 33, 38, 43, 48, 55, 62, 67 })
            for (const double gainDb : { 0.0, -18.0 })
                for (const int block : { 32, 256 })
                {
                    CAPTURE (rate);
                    CAPTURE (note);
                    CAPTURE (gainDb);
                    CAPTURE (block);
                    auto spec = pluck (note, rate);
                    spec.fundamentalGainDb = gainDb;
                    const auto attacks = detect (eval::renderPluck (spec), rate, block);
                    REQUIRE (attacks.size() == 1);
                    const auto onset = eval::onsetSample (spec);
                    const double detectMs = ms (attacks[0].detectedAtSample - onset, rate);
                    const double reportMs = ms (attacks[0].reportedAtSample - onset, rate);
                    CHECK (detectMs >= 0.0);
                    CHECK (detectMs <= 2.1);
                    CHECK (reportMs <= 4.2);
                }
}

TEST_CASE ("Attack detector: re-plucking a ringing string; sustained notes and noise give nothing")
{
    const double rate = 48000.0;
    AttackDetector::Settings withPeriodicity;
    withPeriodicity.usePeriodicity = true;
    int foundWithout = 0;

    for (const int note : { 28, 33, 38, 43, 55 })
    {
        CAPTURE (note);
        // Second pluck of the same note at 0.6 s while the first still rings (2 ms crossfade).
        const auto first = eval::renderPluck (pluck (note, rate, 0.1, 1.3));
        auto secondSpec = pluck (note, rate, 0.6, 1.3);
        secondSpec.seed += 101;
        secondSpec.noiseFloorDbfs = -200.0;
        const auto second = eval::renderPluck (secondSpec);
        std::vector<float> mix (first.size());
        const auto secondOnset = eval::onsetSample (secondSpec);
        const auto fade = static_cast<std::int64_t> (0.002 * rate);
        for (std::size_t i = 0; i < mix.size(); ++i)
        {
            const auto t = static_cast<std::int64_t> (i);
            const double g = t < secondOnset ? 1.0 : std::max (0.0, 1.0 - static_cast<double> (t - secondOnset) / static_cast<double> (fade));
            mix[i] = static_cast<float> (g * first[i] + second[i]);
        }

        // Periodicity break (period of the sounding note known): every re-pluck, quickly.
        const double period = rate / midi::frequencyHzFromNote (note);
        const auto attacks = detect (mix, rate, 64, withPeriodicity, period);
        REQUIRE (attacks.size() == 2);
        CHECK (ms (attacks[1].detectedAtSample - secondOnset, rate) <= 3.0);

        // Default (transient + level only): the first pluck always, the re-pluck only sometimes.
        const auto plain = detect (mix, rate);
        REQUIRE (plain.size() >= 1);
        CHECK (plain.size() <= 2);
        foundWithout += static_cast<int> (plain.size()) - 1;

        // A single sustained pluck with the period known: no break.
        CHECK (detect (first, rate, 64, withPeriodicity, period).size() == 1);
    }
    CHECK (foundWithout >= 2); // documents the limitation rather than hiding it

    // Noise only, at and above the gate.
    for (const double levelDbfs : { -50.0, -30.0 })
    {
        CHECK (detect (eval::renderNoise (rate, 5.0, levelDbfs, 11), rate).empty());
        CHECK (detect (eval::renderNoise (rate, 5.0, levelDbfs, 11), rate, 64, withPeriodicity, 500.0).empty());
    }
}

TEST_CASE ("Attack detector: velocity follows the attack level, refractory merges double hits")
{
    const double rate = 48000.0;
    int previous = 0;
    for (const double peak : { 0.02, 0.05, 0.1, 0.25, 0.5 })
    {
        auto spec = pluck (38, rate);
        spec.peakLinear = peak;
        const auto attacks = detect (eval::renderPluck (spec), rate);
        REQUIRE (attacks.size() == 1);
        CHECK (attacks[0].velocity >= previous);
        previous = attacks[0].velocity;
    }
    CHECK (previous > 100);

    // Below the gate: nothing.
    auto quiet = pluck (38, rate);
    quiet.peakLinear = 0.002;
    CHECK (detect (eval::renderPluck (quiet), rate).empty());
}
