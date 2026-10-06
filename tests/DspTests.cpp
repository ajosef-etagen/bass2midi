#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "ReferenceSetups.h"
#include "SyntheticBass.h"

#include "bass2midi/AnalysisFramer.h"
#include "bass2midi/Fft.h"
#include "bass2midi/MidiNoteUtils.h"
#include "bass2midi/YinPitchEstimator.h"

#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

using namespace bass2midi;

namespace
{
    std::vector<float> sine (double frequencyHz, double sampleRateHz, int numSamples, double amplitude = 0.5)
    {
        std::vector<float> out (static_cast<size_t> (numSamples));
        for (int i = 0; i < numSamples; ++i)
            out[static_cast<size_t> (i)] = static_cast<float> (amplitude * std::sin (2.0 * std::numbers::pi * frequencyHz * i / sampleRateHz));
        return out;
    }

    // Direct O(N * L) YIN difference function, used to cross-check the FFT path.
    std::vector<double> directCmndf (const std::vector<float>& x, int windowSamples, int maxLag)
    {
        const int integration = windowSamples - maxLag;
        std::vector<double> out (static_cast<size_t> (maxLag) + 1, 1.0);
        double runningSum = 0.0;
        for (int tau = 1; tau <= maxLag; ++tau)
        {
            double d = 0.0;
            for (int j = 0; j < integration; ++j)
            {
                const double delta = static_cast<double> (x[static_cast<size_t> (j)]) - static_cast<double> (x[static_cast<size_t> (j + tau)]);
                d += delta * delta;
            }
            runningSum += d;
            out[static_cast<size_t> (tau)] = runningSum > 0.0 ? d * tau / runningSum : 1.0;
        }
        return out;
    }
}

TEST_CASE ("MIDI note utilities")
{
    CHECK (midi::noteFromFrequencyHz (440.0) == doctest::Approx (69.0));
    CHECK (midi::noteFromFrequencyHz (41.2034) == doctest::Approx (28.0).epsilon (1e-4));
    CHECK (midi::frequencyHzFromNote (67) == doctest::Approx (391.995).epsilon (1e-5));
    CHECK (midi::noteName (28) == "E1");
    CHECK (midi::noteName (67) == "G4");
    CHECK (midi::noteName (61) == "C#4");
    CHECK (midi::centsBetween (880.0, 440.0) == doctest::Approx (1200.0));
}

TEST_CASE ("FFT round trip and known spectrum")
{
    Fft fft;
    fft.prepare (16);
    std::vector<std::complex<double>> data (16);
    for (int i = 0; i < 16; ++i)
        data[static_cast<size_t> (i)] = { std::cos (2.0 * std::numbers::pi * 3.0 * i / 16.0), 0.0 };

    auto original = data;
    fft.perform (data.data(), false);
    CHECK (std::abs (data[3]) == doctest::Approx (8.0));
    CHECK (std::abs (data[13]) == doctest::Approx (8.0));
    CHECK (std::abs (data[5]) == doctest::Approx (0.0).epsilon (1e-9));

    fft.perform (data.data(), true);
    for (size_t i = 0; i < data.size(); ++i)
        CHECK (std::abs (data[i] - original[i]) < 1e-12);
}

TEST_CASE ("YIN config validation")
{
    YinPitchEstimator::Config config;
    config.windowSamples = 1000;
    config.minLagSamples = 2;
    config.maxLagSamples = 900; // integration 100 < maxLag / 4
    CHECK_FALSE (config.isValid());

    YinPitchEstimator estimator;
    CHECK_FALSE (estimator.prepare (48000.0, config));
    CHECK_FALSE (estimator.isPrepared());

    config.maxLagSamples = 500;
    CHECK (config.isValid());
    CHECK_FALSE (estimator.prepare (0.0, config));
    CHECK (estimator.prepare (48000.0, config));

    config.minLagSamples = 1;
    CHECK_FALSE (config.isValid());
}

TEST_CASE ("YIN FFT difference function matches direct computation")
{
    YinPitchEstimator::Config config { 1500, 2, 700, 0.15 };
    YinPitchEstimator estimator;
    REQUIRE (estimator.prepare (44100.0, config));

    eval::PluckSpec spec;
    spec.fundamentalHz = 73.4;
    spec.sampleRateHz = 44100.0;
    spec.onsetSeconds = 0.0;
    spec.fundamentalGainDb = -12.0;
    const auto signal = eval::renderPluck (spec);

    estimator.estimate (signal.data());
    const auto expected = directCmndf (signal, config.windowSamples, config.maxLagSamples);
    const auto& actual = estimator.getLastCmndf();

    REQUIRE (actual.size() == expected.size());
    double worst = 0.0;
    for (size_t i = 1; i < expected.size(); ++i)
        worst = std::max (worst, std::abs (actual[i] - expected[i]));
    CHECK (worst < 1e-6);
}

TEST_CASE ("YIN estimates steady sines across the bass range and sample rates")
{
    for (const double rate : { 44100.0, 48000.0, 96000.0 })
    {
        const auto setup = eval::fullRangeYin (rate);
        YinPitchEstimator estimator;
        REQUIRE (estimator.prepare (rate, setup.yin));

        for (const int note : { 28, 33, 38, 43, 55, 67 })
        {
            CAPTURE (rate);
            CAPTURE (note);
            const double f0 = midi::frequencyHzFromNote (note);
            const auto signal = sine (f0, rate, setup.yin.windowSamples);
            const auto result = estimator.estimate (signal.data());

            REQUIRE (result.valid);
            CHECK (std::abs (midi::centsBetween (result.frequencyHz, f0)) < 5.0);
            CHECK (result.clarity > 0.95);
            CHECK (result.windowRmsLinear == doctest::Approx (0.5 / std::sqrt (2.0)).epsilon (0.02));
        }
    }
}

TEST_CASE ("YIN reports silence as invalid")
{
    const auto setup = eval::fullRangeYin (48000.0);
    YinPitchEstimator estimator;
    REQUIRE (estimator.prepare (48000.0, setup.yin));

    std::vector<float> silence (static_cast<size_t> (setup.yin.windowSamples), 0.0f);
    const auto result = estimator.estimate (silence.data());
    CHECK_FALSE (result.valid);
    CHECK (result.frequencyHz == 0.0);
    CHECK (result.windowRmsLinear == 0.0);
}

TEST_CASE ("Warf-like reference cannot reach E1: documented baseline limitation")
{
    const auto setup = eval::warfLikeYin (48000.0);
    CHECK (setup.yin.windowSamples == 2048);
    CHECK (setup.hopSamples == 512);

    YinPitchEstimator estimator;
    REQUIRE (estimator.prepare (48000.0, setup.yin));
    CHECK (estimator.getLowestFrequencyHz() > midi::frequencyHzFromNote (28)); // 46.9 Hz > 41.2 Hz
}

TEST_CASE ("Analysis framer emits frames on a block-size independent grid")
{
    const auto signal = eval::renderNoise (48000.0, 0.25, -20.0, 3);

    const auto collect = [&] (int blockSize)
    {
        AnalysisFramer framer;
        REQUIRE (framer.prepare (1000, 256));
        std::vector<std::int64_t> ends;
        std::vector<float> firstSamples, lastSamples;

        for (size_t pos = 0; pos < signal.size(); pos += static_cast<size_t> (blockSize))
        {
            const int n = static_cast<int> (std::min<size_t> (static_cast<size_t> (blockSize), signal.size() - pos));
            framer.push (signal.data() + pos, n, [&] (const float* window, std::int64_t end, int indexInBlock)
            {
                CHECK (static_cast<std::int64_t> (pos) + indexInBlock + 1 == end);
                ends.push_back (end);
                firstSamples.push_back (window[0]);
                lastSamples.push_back (window[999]);
            });
        }
        return std::make_tuple (ends, firstSamples, lastSamples);
    };

    const auto reference = collect (1);
    for (const int blockSize : { 7, 32, 64, 128, 256, 4096 })
    {
        CAPTURE (blockSize);
        CHECK (collect (blockSize) == reference);
    }

    const auto& ends = std::get<0> (reference);
    REQUIRE (! ends.empty());
    CHECK (ends.front() == 1000);
    CHECK (ends[1] - ends[0] == 256);
    CHECK (std::get<1> (reference)[0] == signal[0]);
    CHECK (std::get<2> (reference)[0] == signal[999]);
}

TEST_CASE ("Analysis framer rejects invalid sizes and resets")
{
    AnalysisFramer framer;
    CHECK_FALSE (framer.prepare (0, 1));
    CHECK_FALSE (framer.prepare (100, 0));
    CHECK_FALSE (framer.prepare (100, 101));
    REQUIRE (framer.prepare (100, 50));

    std::vector<float> block (150, 1.0f);
    int frames = 0;
    framer.push (block.data(), 150, [&] (const float*, std::int64_t, int) { ++frames; });
    CHECK (frames == 2);

    framer.reset();
    frames = 0;
    framer.push (block.data(), 99, [&] (const float*, std::int64_t, int) { ++frames; });
    CHECK (frames == 0);
}

TEST_CASE ("Synthetic pluck is deterministic and silent before onset")
{
    eval::PluckSpec spec;
    spec.noiseFloorDbfs = -200.0;
    const auto a = eval::renderPluck (spec);
    const auto b = eval::renderPluck (spec);
    CHECK (a == b);

    const auto onset = eval::onsetSample (spec);
    CHECK (onset == 4800);
    CHECK (a[static_cast<size_t> (onset - 1)] == 0.0f);

    float peak = 0.0f;
    for (float s : a)
        peak = std::max (peak, std::abs (s));
    CHECK (peak > 0.05f);
    CHECK (peak <= 0.5f * 1.1f); // tone bounded by peakLinear, plus the short pluck noise burst
}

TEST_CASE ("YIN level-change check rejects windows that straddle an attack over background noise")
{
    constexpr double rate = 48000.0;
    auto config = YinPitchEstimator::Config::forFrequencyRange (rate, 38.0, 420.0, 0.15);

    // Older half: noise at -50 dBFS (not silent enough for the silence-edge check); newer half: a tone.
    std::vector<float> window = eval::renderNoise (rate, config.windowSamples / rate + 0.01, -50.0, 5);
    window.resize (static_cast<size_t> (config.windowSamples));
    const auto tone = sine (110.0, rate, config.windowSamples, 0.3);
    for (size_t i = window.size() / 2; i < window.size(); ++i)
        window[i] += tone[i];

    YinPitchEstimator plain, checked;
    REQUIRE (plain.prepare (rate, config));
    config.levelChangeDb = 12.0;
    REQUIRE (checked.prepare (rate, config));

    const auto withCheck = checked.estimate (window.data());
    CHECK_FALSE (withCheck.valid);
    CHECK (withCheck.straddlesSilence);

    // A steady tone over the same noise passes the check.
    std::vector<float> steady (window.size());
    for (size_t i = 0; i < steady.size(); ++i)
        steady[i] = tone[i];
    CHECK (checked.estimate (steady.data()).valid);
    (void) plain;
}
