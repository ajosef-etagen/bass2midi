#include "SyntheticBass.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace bass2midi::eval
{
    namespace
    {
        // Small deterministic PRNG (xorshift32) so signals are identical on every platform.
        struct Random
        {
            std::uint32_t state;

            explicit Random (std::uint32_t seed) : state (seed == 0 ? 0x9e3779b9u : seed) {}

            std::uint32_t next() noexcept
            {
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                return state;
            }

            double uniform() noexcept { return static_cast<double> (next()) / 4294967296.0; } // [0, 1)
            double bipolar() noexcept { return 2.0 * uniform() - 1.0; }
        };

        double dbToGain (double db) { return std::pow (10.0, db / 20.0); }

        // Uniform white noise in [-1, 1) has RMS 1/sqrt(3).
        constexpr double uniformNoiseRms = 0.57735026918962576;
    }

    std::int64_t onsetSample (const PluckSpec& spec)
    {
        return static_cast<std::int64_t> (std::llround (spec.onsetSeconds * spec.sampleRateHz));
    }

    std::vector<float> renderPluck (const PluckSpec& spec)
    {
        const auto numSamples = static_cast<size_t> (std::llround (spec.durationSeconds * spec.sampleRateHz));
        const auto onset = onsetSample (spec);
        std::vector<float> out (numSamples, 0.0f);

        Random random (spec.seed);
        const double twoPi = 2.0 * std::numbers::pi;

        struct Partial { double omega, phase, gain, decayPerSample; };
        std::vector<Partial> partials;
        double gainSum = 0.0;

        for (int k = 1; k <= spec.maxHarmonics; ++k)
        {
            const double frequency = spec.fundamentalHz * k;
            if (frequency >= 0.45 * spec.sampleRateHz)
                break;

            double gain = 1.0 / k;
            if (k == 1)
                gain *= dbToGain (spec.fundamentalGainDb);

            const double decaySeconds = spec.fundamentalDecaySeconds / (1.0 + 0.35 * (k - 1));
            partials.push_back ({ twoPi * frequency / spec.sampleRateHz,
                                  twoPi * random.uniform(),
                                  gain,
                                  std::exp (-1.0 / (decaySeconds * spec.sampleRateHz)) });
            gainSum += gain;
        }

        const double normalise = gainSum > 0.0 ? spec.peakLinear / gainSum : 0.0; // peak <= peakLinear
        const auto attackSamples = std::max<std::int64_t> (1, std::llround (spec.attackMs * 1.0e-3 * spec.sampleRateHz));
        const auto burstSamples = std::llround (spec.pluckNoiseMs * 1.0e-3 * spec.sampleRateHz);
        const double burstGain = spec.pluckNoiseDb > -120.0 ? spec.peakLinear * dbToGain (spec.pluckNoiseDb) : 0.0;
        const double floorGain = spec.noiseFloorDbfs > -120.0 ? dbToGain (spec.noiseFloorDbfs) / uniformNoiseRms : 0.0;

        for (size_t i = 0; i < numSamples; ++i)
        {
            double sample = floorGain > 0.0 ? floorGain * random.bipolar() : 0.0;
            const auto t = static_cast<std::int64_t> (i) - onset;

            if (t >= 0)
            {
                double tone = 0.0;
                for (const auto& p : partials)
                    tone += p.gain * std::pow (p.decayPerSample, static_cast<double> (t))
                          * std::sin (p.omega * static_cast<double> (t) + p.phase);

                const double attack = t < attackSamples
                                          ? 0.5 - 0.5 * std::cos (std::numbers::pi * static_cast<double> (t) / static_cast<double> (attackSamples))
                                          : 1.0;
                sample += normalise * attack * tone;

                if (t < burstSamples && burstGain > 0.0)
                {
                    const double fade = 1.0 - static_cast<double> (t) / static_cast<double> (burstSamples);
                    sample += burstGain * fade * random.bipolar();
                }
            }

            out[i] = static_cast<float> (sample);
        }

        return out;
    }

    std::vector<float> renderNoise (double sampleRateHz, double durationSeconds, double levelDbfs, std::uint32_t seed)
    {
        const auto numSamples = static_cast<size_t> (std::llround (durationSeconds * sampleRateHz));
        std::vector<float> out (numSamples);
        Random random (seed);
        const double gain = dbToGain (levelDbfs) / uniformNoiseRms;
        for (auto& s : out)
            s = static_cast<float> (gain * random.bipolar());
        return out;
    }
}
