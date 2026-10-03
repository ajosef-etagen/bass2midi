#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bass2midi::eval
{
    // Deterministic synthetic "plucked bass" test signal: a harmonic series with per-harmonic
    // exponential decay (upper partials die faster), a short raised-cosine attack, an optional
    // pluck noise burst and an optional white-noise floor.
    //
    // This is NOT a substitute for DI recordings. It provides reproducible ground truth (exact
    // onset sample and fundamental) for regression tests and for the Phase 0 baseline.
    struct PluckSpec
    {
        double fundamentalHz = 41.2034;
        double sampleRateHz = 48000.0;
        double durationSeconds = 1.0;
        double onsetSeconds = 0.1;          // silence (plus noise floor) before the pluck
        double peakLinear = 0.5;            // upper bound of the tone peak, linear full scale
        double fundamentalGainDb = 0.0;     // relative level of harmonic 1; negative = weak fundamental
        int maxHarmonics = 16;              // partials above 0.45 * sampleRate are always dropped
        double fundamentalDecaySeconds = 1.2; // e-folding time of harmonic 1
        double attackMs = 1.0;              // raised-cosine fade-in
        double pluckNoiseDb = -24.0;        // noise burst level relative to peakLinear; <= -120 disables
        double pluckNoiseMs = 4.0;
        double noiseFloorDbfs = -90.0;      // continuous white noise; <= -120 disables
        std::uint32_t seed = 1;             // phases and noise
    };

    std::vector<float> renderPluck (const PluckSpec& spec);

    // White noise only, for false-trigger measurement.
    std::vector<float> renderNoise (double sampleRateHz, double durationSeconds, double levelDbfs, std::uint32_t seed);

    std::int64_t onsetSample (const PluckSpec& spec);
}
